/* SPDX-License-Identifier: GPL-3.0-only */
/* community.c: the community mod host. #included from azmod.c after the hook installer,
 * deck identity and the BPM-state capture it uses.
 *
 * Core dlopens each enabled community mod from the p7 stage (az-usbload mirrors
 * MODS/community-mods/ there, without each mod's data/), inside EP147 only (community_init runs on the overlay thread, which
 * exists only in EP147). Every refusal carries a reason the Mod Menu shows.
 *
 * Order per mod: per-mod off switch -> azmod.json (id, api, firmware, hooks) -> ELF checks
 * (aarch64 ET_DYN, exports az_mod_v1_entry, exports nothing core interposes) -> the approved
 * list (the deck's own record of (id, sha256 of <id>.so) the DJ trusted, see below) -> dlopen
 * RTLD_LOCAL (a dlopened library cannot interpose read/ioctl for the process: structural)
 * -> descriptor checks -> init(&core).
 *
 * Quarantine: settings/azmod-community/.starting counts starts that loaded community mods
 * and did not reach 90 s. At 2, this start loads none and the Mod Menu says
 * so; the DJ re-enables from there. A crashing mod therefore costs two short restarts and
 * never reaches az-usbload's crash-loop guard (3), which would also turn official mods off.
 *
 * Nothing here runs per frame except community_tick (cheap when no mod is loaded) and the
 * on_frame dispatch, which is a no-op with no frame-handling mod. */
#include <dirent.h>
#include <elf.h>
#include <link.h>
#include <signal.h>
#include <ucontext.h>
#include <errno.h>
#include <pthread.h>
#include <sys/file.h>
#include <ctype.h>
#include <limits.h>
#include "azmod-mod.h"
#include "../common/azlogcap.h" /* azlog_rotate: per-mod logs capped per file */
#include "../common/azdeckinfo.h" /* azstem_deck_snapshot: deck_loaded / deck_title */
#include "../common/azsha256.h" /* the approved list's fingerprint of each mod's .so */
#include "utf8_cut.h"               /* text settings and titles cut at a character boundary */

/* clang-format off */
#ifndef CM_ROOT
#define CM_ROOT "/home/root/settings"   /* the harness (test/community) points this elsewhere */
#endif
#define CM_STAGE     CM_ROOT "/az-usbload/community"     /* mirrored from MODS/community-mods (without data/) */
#define CM_ALL_OFF   CM_ROOT "/az-usbload/community-mods.off" /* mirrored from MODS/community-mods.off */
#define CM_STATE     CM_ROOT "/azmod-community"          /* per-mod cfg + the quarantine counter */
#define CM_STARTING  CM_STATE "/.starting"
#define CM_CRASHES   CM_STATE "/.crashes"   /* unexplained crashes with community mods loaded (epoch s, one per line) */
#define CM_CRASH_N   3                      /* that many within CM_CRASH_S switch community mods off */
#define CM_CRASH_S   1800
#define CM_USER_OFF  CM_STATE "/off"                                /* the Mod Menu's "community mods off" */
#ifndef CM_LOGDIR
#define CM_LOGDIR    "/tmp/azmod-community"
#endif
#define CM_MAX       16
#define CM_CFG_MAX   32
#define CM_CFGS_MAX  16       /* text settings per mod */
#define CM_CFG_BUSY_MS 5000   /* a mod changing settings non-stop is written at most this often */
#define CM_DIR_TTL_MS 2000    /* data_dir / assets_dir re-check */
#ifndef CM_USB_ROOT
#define CM_USB_ROOT "/media/usb"   /* the deck's stick mounts (the harness points this elsewhere) */
#endif
#define CM_LOG_CAP   (256 * 1024)
#define CM_SO_CAP    (16 * 1024 * 1024)
#define CM_HEALTHY_MS 90000
#include "cm_orderly.h" /* an orderly stop (SIGTERM while EP147 is alive) is not a failed start */
#define CM_BLAME     CM_STATE "/.blame"   /* id of the mod whose code crashed EP147 */
#define CM_INITING   CM_STATE "/.initing" /* id of the mod whose init() is running; left behind = it froze or crashed there */
/* Approved mods: enabling a mod on the stick (MODS/azmods.cfg) is not enough to run it:
 * a stick is not the deck's, and anyone can write one. The deck keeps its own list, on p7, of the
 * exact code the DJ trusted: one line per mod, "<id> <sha256 of <id>.so>". A line is written when
 * the DJ installs or updates a mod from the deck (az-community, after its verified download: the
 * install is the trust decision, made on the deck), or presses Allow on a mod the deck found
 * already enabled ("New mod found"): one the desktop app installed, or one copied by hand. One
 * line per id: approving a new hash replaces the old one, so swapping an older or different .so in
 * under an approved id asks again. Grandfathering: see cm_grandfather. */
#define CM_APPROVED  CM_STATE "/approved"
#define CM_APPROVED_LOCK CM_STATE "/.approved.lock"   /* flock: core and az-community both write the list */
#define CM_APPROVED_INIT CM_STATE "/.approved-init"   /* the one-time grandfathering has run */
#define CM_APPROVED_MAX 64                            /* lines kept (oldest dropped): a few KB of p7 at most */

/* states shown in the UI (menu.h AZCM_*) */
typedef struct {
    char id[33], name[48], version[24], author[48], summary[120];
    int  state;                 /* AZCM_* */
    char reason[112];
    int  hooks_ok;              /* manifest "hooks": true and not an "any" mod */
    int  any;                   /* firmware: ["any"] */
    int  nhooks;
    int  sticky;                /* __atomic: used a service that lasts until the next start (hooks, vtable
                                 * slots, surfaces, X windows, audio taps, handoff): not turned off live */
    int  inflight;              /* __atomic: calls into the mod now running on other threads (cm_enter) */
    int  dirty;                 /* turned off live but a thread of it kept running: on again after a reload */
    void *h;
    const az_mod_v1 *m;
    const az_screen_v1 *screen;
    struct { char k[32]; int v; } cfg[CM_CFG_MAX];
    struct { char k[32]; char v[128]; } cfgs[CM_CFGS_MAX];   /* text settings ("key$=value" lines) */
    int  ncfg, ncfgs, cfg_loaded, cfg_dirty;
    long cfg_flushed_at;          /* last settings write (rate limit) */
    struct {                      /* log lines from on_frame (the bus thread must never write files) */
        char line[8][160];
        int ready[8];             /* __atomic: the slot holds a line for the tick to write */
        unsigned w;               /* __atomic: next slot to fill */
        unsigned dropped, refused;/* __atomic: lines that did not fit; services refused in on_frame */
    } fq;
    char owner[320];              /* where the loaded copy came from (cm_owner_key) */
    char sha[65];                 /* sha256 of the <id>.so this start checked ("" = not read) */
    int  allowed;                 /* AZCM_NEW, and the DJ pressed Allow this start */
    char data[260], assets[260];  /* data_dir / assets_dir paths: never blanked once handed out */
    int  data_ok, assets_ok;      /* the current answer: path (1) or NULL (0) */
    long data_at, assets_at;      /* re-checked every CM_DIR_TTL_MS */
    long log_bytes;               /* __atomic: any thread that logs adds to it */
    int log_sized;   /* log_bytes started from the file left by earlier starts */
    const char *refused_said[24]; /* services already logged as refused for a foreign caller */
    uintptr_t lo, hi;           /* the mod's mapped range, for crash blame */
} cm_mod;
static cm_mod g_cm[CM_MAX];
static int    g_cm_n = 0, g_cm_live = 0;       /* records / mods with state LOADED (__atomic) */
/* A mod's state is written on the UI thread and read on others (the bus, touch and audio
 * threads, the mods' own): every write and every read off the UI thread is atomic. */
static inline void cm_set_state(cm_mod *c, int s) { __atomic_store_n(&c->state, s, __ATOMIC_SEQ_CST); }
static inline int cm_running(const cm_mod *c) { return __atomic_load_n(&c->state, __ATOMIC_ACQUIRE) == AZCM_LOADED; }
static inline int cm_nlive(void) { return __atomic_load_n(&g_cm_live, __ATOMIC_RELAXED); }
/* A call into mod c from a thread other than the UI thread: 1 = make it, then cm_leave(c);
 * 0 = the mod is not running. A live stop sets the state first and then waits for inflight to
 * reach 0, so once it has, no such call is running or can start (both sides are seq_cst). */
static inline int cm_enter(cm_mod *c) {
    /* not running: out before counting, so a busy caller cannot keep a live stop waiting */
    if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) != AZCM_LOADED) return 0;
    __atomic_add_fetch(&c->inflight, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&c->state, __ATOMIC_SEQ_CST) == AZCM_LOADED) return 1;
    __atomic_sub_fetch(&c->inflight, 1, __ATOMIC_RELEASE);
    return 0;
}
static inline void cm_leave(cm_mod *c) { __atomic_sub_fetch(&c->inflight, 1, __ATOMIC_RELEASE); }
static int    g_cm_suspended = 0;              /* quarantine tripped this start */
static int    g_cm_alloff = 0;                 /* community mods switched off (stick or menu) */
static long   g_cm_t0 = 0;                     /* when this start loaded mods (ms) */
static int    g_cm_timing = 0;                 /* g_cm_t0 is set (0 is a valid clock value) */
static int    g_cm_healthy = 0;                /* .starting reset after CM_HEALTHY_MS */
static int    g_cm_loading = -1;               /* index whose init() is running */
static int    g_sdk_in_init;                   /* 1 while g_cm_loading's init() runs (community_init) */
static int    g_cm_live_try;                   /* cm_load_one for a mod turned on live (azcm_live_set) */
static int g_cm_redraw = 0;   /* __atomic_* : set by any thread, consumed by the overlay loop */
static char   g_cm_fwtag[32];
static uintptr_t g_cm_text_lo = 0, g_cm_text_hi = 0;   /* EP147's executable range */
/* clang-format on */

/* ---- firmware tag: FNV-1a 64 over EP147's executable PT_LOAD segments, read through the ELF
 * file's own program headers (/proc/self/exe). Never from /proc/self/maps: a hook installer's
 * mprotect splits and re-flags mappings, so a maps-derived tag would change with the hooks
 * installed. File bytes and file headers are a property of the binary alone, identical on the
 * emulator and on a deck running the same firmware. The code range for claim_hook comes from
 * the loaded program headers (dl_iterate_phdr). */
static uint64_t cm_fnv(uint64_t h, const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}
static int cm_text_cb(struct dl_phdr_info *info, size_t sz, void *data) {
    (void) sz;
    (void) data;
    if (info->dlpi_name && info->dlpi_name[0]) return 0;         /* the main program only */
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X)) continue;
        uintptr_t lo = info->dlpi_addr + ph->p_vaddr, hi = lo + ph->p_memsz;
        if (!g_cm_text_lo || lo < g_cm_text_lo) g_cm_text_lo = lo;
        if (hi > g_cm_text_hi) g_cm_text_hi = hi;
    }
    return 1;
}
static void cm_derive_fwtag(void) {
    dl_iterate_phdr(cm_text_cb, NULL);
    int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    Elf64_Ehdr eh;
    /* clang-format off */
    if(pread(fd, &eh, sizeof eh, 0) != (ssize_t)sizeof eh || memcmp(eh.e_ident, ELFMAG, SELFMAG)
       || eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_phentsize != sizeof(Elf64_Phdr) || eh.e_phnum > 64){
        close(fd); return; }
    /* clang-format on */
    Elf64_Phdr ph[64];
    if (pread(fd, ph, sizeof(Elf64_Phdr) * eh.e_phnum, (off_t) eh.e_phoff) != (ssize_t) (sizeof(Elf64_Phdr) * eh.e_phnum)) {
        close(fd);
        return;
    }
    uint64_t h = 1469598103934665603ULL;
    int segs = 0;
    static unsigned char buf[65536];
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || !(ph[i].p_flags & PF_X)) continue;
        size_t remain = ph[i].p_filesz;
        off_t o = (off_t) ph[i].p_offset;
        while (remain) {
            size_t c = remain < sizeof buf ? remain : sizeof buf;
            ssize_t r = pread(fd, buf, c, o);
            if (r <= 0) {
                close(fd);
                return;
            }                     /* a short read = no tag, never a partial one */
            h = cm_fnv(h, buf, (size_t) r);
            o += r;
            remain -= (size_t) r;
        }
        segs++;
    }
    close(fd);
    if (segs) snprintf(g_cm_fwtag, sizeof g_cm_fwtag, "ep147-%016llx", (unsigned long long) h);
}

/* ---- azmod.json: a tiny strict scanner. one flat object of strings, numbers, true / false /
 * null and arrays of strings; no nested objects, no repeated keys. Keys match only at the top
 * level, so the core, the fetcher and the app cannot read different values from one file. The
 * fetcher has already validated the shape; anything unreadable here is a refusal, never a
 * guess. */
static const char *cm_json_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}
static const char *cm_json_skipstr(const char *p) {        /* p at '"'; returns past the closing '"' or NULL */
    for (p++; *p; p++) {
        if (*p == '\\') {
            if (!*++p) return NULL;
        } else if (*p == '"') return p + 1;
    }
    return NULL;
}
/* Walk the object. With key: return its value (NULL if absent). Without: check the shape; why
 * gets the reason when it fails (return NULL), else returns js. */
static const char *cm_json_walk(const char *js, const char *key, const char **why) {
    const char *seen[48];
    int nseen = 0, klen = key ? (int) strlen(key) : 0;
    const char *p = cm_json_ws(js);
    if (*p != '{') {
        if (why) *why = "azmod.json is not a JSON object";
        return NULL;
    }
    p = cm_json_ws(p + 1);
    if (*p == '}') return key ? NULL : js;
    for (;;) {
        if (*p != '"') {
            if (why) *why = "azmod.json: a key is not a string";
            return NULL;
        }
        const char *k0 = p + 1, *ke = cm_json_skipstr(p);
        if (!ke) {
            if (why) *why = "azmod.json: an unterminated string";
            return NULL;
        }
        int kl = (int) (ke - 1 - k0);
        for (int i = 0; i < nseen; i++)
            if (!strncmp(seen[i], k0, (size_t) kl) && seen[i][kl] == '"') {
                if (why) *why = "azmod.json repeats a key";
                return NULL;
            }
        if (nseen == 48) { /* a key past the ones remembered could not be checked for a repeat */
            if (why) *why = "azmod.json has too many keys";
            return NULL;
        }
        seen[nseen++] = k0;
        p = cm_json_ws(ke);
        if (*p != ':') {
            if (why) *why = "azmod.json: a key has no value";
            return NULL;
        }
        p = cm_json_ws(p + 1);
        const char *val = p;
        if (*p == '"') {
            if (!(p = cm_json_skipstr(p))) {
                if (why) *why = "azmod.json: an unterminated string";
                return NULL;
            }
        } else if (*p == '[') {
            p = cm_json_ws(p + 1);
            while (*p != ']') {
                if (*p != '"' || !(p = cm_json_skipstr(p))) {
                    if (why) *why = "azmod.json: a list holds something other than text";
                    return NULL;
                }
                p = cm_json_ws(p);
                if (*p == ',') {
                    p = cm_json_ws(p + 1);
                    continue;
                }
                if (*p != ']') {
                    if (why) *why = "azmod.json: a list is not closed";
                    return NULL;
                }
            }
            p++;
        } else if (*p == '{') {
            if (why) *why = "azmod.json holds a nested object";
            return NULL;
        } else {
            while (*p && *p != ',' && *p != '}' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        }
        if (key && kl == klen && !strncmp(k0, key, (size_t) kl)) return val;
        p = cm_json_ws(p);
        if (*p == ',') {
            p = cm_json_ws(p + 1);
            continue;
        }
        if (*p == '}') return key ? NULL : js;
        if (why) *why = "azmod.json: expected , or }";
        return NULL;
    }
}
static const char *cm_json_key(const char *js, const char *key) { return cm_json_walk(js, key, NULL); }
static int cm_json_strat(const char *p, char *out, int n) {       /* p at the opening quote */
    if (!p || *p != '"') return 0;
    int k = 0;
    p++;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            c = *p++;
            if (c == 'n') c = ' ';
        }
        if (k < n - 1) out[k++] = c;
    }
    out[k] = 0;
    return *p == '"';
}
static int cm_json_str(const char *js, const char *key, char *out, int n) {
    out[0] = 0;
    return cm_json_strat(cm_json_key(js, key), out, n);
}
static int cm_json_int(const char *js, const char *key, int def) {
    const char *p = cm_json_key(js, key);
    return (p && (*p == '-' || (*p >= '0' && *p <= '9'))) ? atoi(p) : def;
}
static int cm_json_bool(const char *js, const char *key, int def) {
    const char *p = cm_json_key(js, key);
    if (!p) return def;
    if (!strncmp(p, "true", 4)) return 1;
    if (!strncmp(p, "false", 5)) return 0;
    return def;
}
/* firmware list: 1 = lists `want`, 2 = lists "any", 0 = neither, -1 = unreadable */
static int cm_json_fw(const char *js, const char *want) {
    const char *p = cm_json_key(js, "firmware");
    if (!p || *p != '[') return -1;
    p++;
    int got = 0;
    for (;;) {
        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == ']') return got;
        char s[40];
        if (!cm_json_strat(p, s, sizeof s)) return -1;
        if (!strcmp(s, "any")) got = got ? got : 2;
        else if (want[0] && !strcmp(s, want)) got = 1;
        p = strchr(p + 1, '"');
        if (!p) return -1;
        p++;
    }
}

/* ---- ELF checks, from the file, before anything of the mod runs ---------------------- */
static const char *const CM_FORBIDDEN[] = {
    "read", "write", "open", "open64", "openat", "close", "ioctl", "recvmsg", "recv", "recvfrom",
    "sendmsg", "select", "poll", "ppoll", "mmap", "mprotect", "dlopen", "dlsym", "fopen", NULL};
/* What a mod may link against (the v2.01 rootfs has these; azmod-pack enforces the same list).
 * Not libX11/libXext/libfreetype: core owns X and the font. */
static const char *const CM_LIBS[] = {
    "libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1", "libgcc_s.so.1",
    "libstdc++.so.6", "ld-linux-aarch64.so.1", NULL};
/* exports the linker adds to every library; anything else besides the entry is refused */
static const char *const CM_BOILER[] = {"_init", "_fini", "_edata", "_end", "__bss_start",
                                        "__bss_start__", "__bss_end__", "_bss_end__", "__end__", NULL};
/* [off, off + len) lies inside the file and off is aligned for the records read there; written
 * so that no sum can wrap */
static int cm_elf_span(uint64_t off, uint64_t len, size_t sz, uint64_t align) {
    return off <= sz && len <= sz - off && off % align == 0;
}
/* a string table: inside the file and ending in NUL, so every name read from it ends inside it */
static int cm_elf_strtab(const unsigned char *b, size_t sz, const Elf64_Shdr *ss) {
    return ss->sh_size && cm_elf_span(ss->sh_offset, ss->sh_size, sz, 1) && !b[ss->sh_offset + ss->sh_size - 1];
}
static int cm_elf_check(const char *path, char *why, int n, char sha[65]) {
    sha[0] = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(why, n, "Missing %s", strrchr(path, '/') + 1);
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) || st.st_size < (off_t) sizeof(Elf64_Ehdr) || st.st_size > CM_SO_CAP) {
        close(fd);
        snprintf(why, n, "The mod file is empty or too large");
        return -1;
    }
    unsigned char *b = malloc((size_t) st.st_size);
    if (!b) {
        close(fd);
        snprintf(why, n, "Out of memory reading the mod");
        return -1;
    }
    ssize_t got = pread(fd, b, (size_t) st.st_size, 0);
    close(fd);
    int rc = -1;
    size_t sz = (size_t) st.st_size;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *) b;
    /* clang-format off */
    if(got != st.st_size || memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS64
       || eh->e_ident[EI_DATA] != ELFDATA2LSB){ snprintf(why, n, "Not a 64-bit ELF library"); goto out; }
    if(eh->e_machine != EM_AARCH64){ snprintf(why, n, "Built for the wrong CPU (not aarch64)"); goto out; }
    if(eh->e_type != ET_DYN){ snprintf(why, n, "Not a shared library"); goto out; }
    if(!eh->e_shoff || eh->e_shentsize != sizeof(Elf64_Shdr)
       || !cm_elf_span(eh->e_shoff, (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr), sz, 8)){ snprintf(why, n, "Damaged ELF section table"); goto out; }
    /* clang-format on */
    const Elf64_Shdr *sh = (const Elf64_Shdr *) (b + eh->e_shoff);
    int has_entry = 0;
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_DYNSYM) continue;
        if (sh[i].sh_link >= eh->e_shnum) break;
        const Elf64_Shdr *ss = &sh[sh[i].sh_link];
        if (!cm_elf_span(sh[i].sh_offset, sh[i].sh_size, sz, 8) || !cm_elf_strtab(b, sz, ss)) break;
        const Elf64_Sym *sym = (const Elf64_Sym *) (b + sh[i].sh_offset);
        size_t ns = sh[i].sh_size / sizeof(Elf64_Sym);
        const char *str = (const char *) (b + ss->sh_offset);
        for (size_t k = 0; k < ns; k++) {
            int bind = ELF64_ST_BIND(sym[k].st_info);
            if (sym[k].st_shndx == SHN_UNDEF || (bind != STB_GLOBAL && bind != STB_WEAK)) continue;
            if (ELF64_ST_VISIBILITY(sym[k].st_other) != STV_DEFAULT) continue;
            if (sym[k].st_name >= ss->sh_size) continue;
            const char *nm = str + sym[k].st_name;
            if (!strcmp(nm, "az_mod_v1_entry")) {
                has_entry = 1;
                continue;
            }
            int bad = !strncmp(nm, "sqlite3_", 8);
            for (int f = 0; !bad && CM_FORBIDDEN[f]; f++)
                if (!strcmp(nm, CM_FORBIDDEN[f])) bad = 1;
            if (bad) {
                snprintf(why, n, "Exports %s(), which only AZ-MODS may define", nm);
                goto out;
            }
            int boiler = 0;
            for (int f = 0; CM_BOILER[f]; f++)
                if (!strcmp(nm, CM_BOILER[f])) boiler = 1;
            if (!boiler) {
                snprintf(why, n, "Exports %.60s; a mod exports only az_mod_v1_entry", nm);
                goto out;
            }
        }
    }
    if (!has_entry) {
        snprintf(why, n, "Does not export az_mod_v1_entry");
        goto out;
    }
    /* DT_NEEDED: only the allowed libraries */
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_DYNAMIC) continue;
        if (sh[i].sh_link >= eh->e_shnum) break;
        const Elf64_Shdr *ss = &sh[sh[i].sh_link];
        if (!cm_elf_span(sh[i].sh_offset, sh[i].sh_size, sz, 8) || !cm_elf_strtab(b, sz, ss)) break;
        const Elf64_Dyn *dy = (const Elf64_Dyn *) (b + sh[i].sh_offset);
        size_t nd = sh[i].sh_size / sizeof(Elf64_Dyn);
        for (size_t k = 0; k < nd && dy[k].d_tag != DT_NULL; k++) {
            if (dy[k].d_tag != DT_NEEDED || dy[k].d_un.d_val >= ss->sh_size) continue;
            const char *lib = (const char *) (b + ss->sh_offset + dy[k].d_un.d_val);
            int ok = 0;
            for (int f = 0; CM_LIBS[f]; f++)
                if (!strcmp(lib, CM_LIBS[f])) ok = 1;
            if (!ok) {
                snprintf(why, n, "Links %.60s, which a mod may not use", lib);
                goto out;
            }
        }
    }
    /* the fingerprint the approved list is checked against: these same bytes, read once */
    azsha256_hex(b, sz, sha);
    rc = 0;
out:
    free(b);
    return rc;
}

/* ---- per-mod settings, written later off the caller's thread (community_tick) ---------- */
/* a community mod id: 3-32 of a-z 0-9 -, and not a name the folder or AZ-MODS itself uses
 * (the same list in the fetcher, the app and azmod-pack) */
static int cm_id_ok(const char *id) {
    static const char *const reserved[] = {"enabled", "azmod", "azstem", "az-mods", "azmods", NULL};
    size_t n = id ? strlen(id) : 0;
    if (n < 3 || n > 32) return 0;
    for (const char *q = id; *q; q++)
        if (!((*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') || *q == '-')) return 0;
    for (int i = 0; reserved[i]; i++)
        if (!strcmp(id, reserved[i])) return 0;
    return 1;
}
static cm_mod *cm_find(const char *id) {
    if (!id) return NULL;
    for (int i = 0; i < g_cm_n; i++)
        if (!strcmp(g_cm[i].id, id)) return &g_cm[i];
    return NULL;
}
static int cm_key_ok(const char *key);
static __thread int g_cm_in_frame;   /* this thread is inside a mod's on_frame (the control bus) */
static void cm_logf(cm_mod *c, int force, const char *fmt, ...);
/* ---- a mod's own values (cfg_set / cfg_set_str) live in the settings file, azmod.conf, which
 * is MODS/azmods.cfg on the USB (the loader's import and write-back), so they follow the USB:
 *   <id>:cfg.<key> = <number>      <id>:str.<key> = "<text>"      <id>:owner.repo = <owner>
 * A Mod Menu row's key is [a-z0-9-] (azfeatures.c), so `<id>:<row>` never collides with these. A key
 * byte outside [a-z0-9._-] is written ":xx" (two hex digits). Text is quoted, since the file trims
 * a value's spaces and a mod's text may start or end with one. owner.repo names the owner the
 * values belong to (cm_owner_key): another author's copy under the same id never gets them.
 * g_conf belongs to the overlay thread: read when a mod loads (before its init), written by the
 * flush (community_tick, azcm_flush_settings), never from a mod's call. */
static int cm_conf_key(char *out, size_t n, const char *id, const char *ns, const char *key) {
    size_t k = (size_t) snprintf(out, n, "%s:%s.", id, ns);
    for (const unsigned char *q = (const unsigned char *) key; *q && k + 4 < n; q++)
        if ((*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') || *q == '.' || *q == '_' || *q == '-')
            out[k++] = (char) *q;
        else
            k += (size_t) snprintf(out + k, n - k, ":%02x", *q);
    out[k < n ? k : n - 1] = 0;
    return k + 4 < n;
}
static int cm_conf_dec(const char *e, char *out, size_t n) {   /* 0 = a valid key */
    size_t k = 0;
    for (; *e; e++) {
        unsigned v = (unsigned char) *e;
        if (*e == ':') {
            if (sscanf(e + 1, "%2x", &v) != 1 || !isxdigit((unsigned char) e[1]) || !isxdigit((unsigned char) e[2])) return -1;
            e += 2;
        }
        if (k + 1 >= n) return -1;
        out[k++] = (char) v;
    }
    out[k] = 0;
    return cm_key_ok(out) ? 0 : -1;
}
static void cm_cfg_put_int(cm_mod *c, const char *k, int v) {   /* a value absent so far */
    for (int i = 0; i < c->ncfg; i++)
        if (!strcmp(c->cfg[i].k, k)) return;
    if (c->ncfg >= CM_CFG_MAX) return;
    snprintf(c->cfg[c->ncfg].k, sizeof c->cfg[0].k, "%s", k);
    c->cfg[c->ncfg++].v = v;
}
static void cm_cfg_put_str(cm_mod *c, const char *k, const char *v) {
    for (int i = 0; i < c->ncfgs; i++)
        if (!strcmp(c->cfgs[i].k, k)) return;
    if (c->ncfgs >= CM_CFGS_MAX) return;
    snprintf(c->cfgs[c->ncfgs].k, sizeof c->cfgs[0].k, "%s", k);
    snprintf(c->cfgs[c->ncfgs].v, sizeof c->cfgs[0].v, "%s", v);
    utf8_cut(c->cfgs[c->ncfgs].v);
    c->ncfgs++;
}
/* the file format of 1.2.3's test builds (azmod-community/<id>.cfg: key=number, key$=text), still
 * used for another owner's values set aside on p7 (.<id>@<hash>.cfg). Values already held win. */
static int cm_cfg_read_old(cm_mod *c, const char *p) {
    FILE *f = fopen(p, "re");
    if (!f) return -1;
    char line[200];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *ds = strstr(line, "$=");
        if (ds && ds > line && ds - line < 32) {                       /* text: key$=value */
            *ds = 0;
            if (cm_key_ok(line)) cm_cfg_put_str(c, line, ds + 2);
            continue;
        }
        char k[32];
        int v;
        if (sscanf(line, "%31[^=]=%d", k, &v) == 2 && cm_key_ok(k)) cm_cfg_put_int(c, k, v);
    }
    fclose(f);
    return 0;
}
static int cm_cfg_write_old(const cm_mod *c, const char *p) {
    char t[240];
    snprintf(t, sizeof t, "%s.new", p);
    FILE *f = fopen(t, "we");
    if (!f) return -1;
    for (int i = 0; i < c->ncfg; i++) fprintf(f, "%s=%d\n", c->cfg[i].k, c->cfg[i].v);
    for (int i = 0; i < c->ncfgs; i++) fprintf(f, "%s$=%s\n", c->cfgs[i].k, c->cfgs[i].v);
    if (fclose(f) != 0 || rename(t, p) != 0) {
        unlink(t);
        return -1;
    }
    return 0;
}
/* the mod's entries in g_conf: read into c, or (remove) taken out of the file */
static void cm_conf_walk(cm_mod *c, int remove) {
    char pc[48], ps[48], po[48], k[32];
    snprintf(pc, sizeof pc, "%s:cfg.", c->id);
    snprintf(ps, sizeof ps, "%s:str.", c->id);
    snprintf(po, sizeof po, "%s:owner.repo", c->id);
    const char *key, *val;
    for (int i = 0; azconf_entry(g_conf, i, &key, &val); i++) {
        int isc = !strncmp(key, pc, strlen(pc)), iss = !strncmp(key, ps, strlen(ps));
        if (remove) {
            if (isc || iss || !strcmp(key, po)) {
                azconf_unset(g_conf, key);
                i = -1;   /* the entries moved: walk again */
            }
            continue;
        }
        if (isc && cm_conf_dec(key + strlen(pc), k, sizeof k) == 0) {
            char *e;
            errno = 0;
            long v = strtol(val, &e, 10);
            if (val[0] && !*e && !errno && v >= INT_MIN && v <= INT_MAX) cm_cfg_put_int(c, k, (int) v);
        } else if (iss && cm_conf_dec(key + strlen(ps), k, sizeof k) == 0) {
            size_t L = strlen(val);
            char v[130];
            if (L >= 2 && val[0] == '"' && val[L - 1] == '"') snprintf(v, sizeof v, "%.*s", (int) (L - 2), val + 1);
            else snprintf(v, sizeof v, "%s", val);
            cm_cfg_put_str(c, k, v);
        }
    }
}
static int g_cm_conf_save;   /* the flush changed g_conf: cm_conf_save writes it */
/* the mod's values into g_conf; 1 = changed */
static int cm_cfg_to_conf(const cm_mod *c) {
    if (!g_conf || g_conf_readonly) return 0;
    char key[160], v[136];
    for (int i = 0; i < c->ncfg; i++) {
        snprintf(v, sizeof v, "%d", c->cfg[i].v);
        if (cm_conf_key(key, sizeof key, c->id, "cfg", c->cfg[i].k)) azconf_set(g_conf, key, v);
    }
    for (int i = 0; i < c->ncfgs; i++) {
        snprintf(v, sizeof v, "\"%s\"", c->cfgs[i].v);
        if (cm_conf_key(key, sizeof key, c->id, "str", c->cfgs[i].k) && azconf_set(g_conf, key, v) != 0)
            dbg("community: %s: text setting %s not saved (not UTF-8)\n", c->id, c->cfgs[i].k);
    }
    if (c->ncfg || c->ncfgs) {
        snprintf(key, sizeof key, "%s:owner.repo", c->id);
        azconf_set(g_conf, key, c->owner);
    }
    return azconf_dirty(g_conf);
}
static int cm_conf_save(void) {   /* overlay thread, outside g_cm_mu: 0 = written (or nothing to do) */
    if (!g_cm_conf_save) return 0;
    g_cm_conf_save = 0;
    if (!g_conf || g_conf_readonly) return -1;
    return azconf_save(g_conf, CONF_PATH);
}
/* When a mod loads (overlay thread, after cm_owner_p7, before its code runs): its values from
 * g_conf. Another owner's values are set aside on p7 as before (.<id>@<hash of that owner>.cfg)
 * and taken out of the file; this owner's own set-aside values come back. A <id>.cfg of the test
 * builds (cm_owner_p7 left it to this owner) is imported once, then removed. */
static void cm_owner_hash(const char *owner, char out[17]);
static void cm_cfg_load(cm_mod *c) {
    c->cfg_loaded = 1;
    c->ncfg = c->ncfgs = 0;
    if (!g_conf) return;
    char po[48], hs[17], aside[220], legacy[200];
    snprintf(po, sizeof po, "%s:owner.repo", c->id);
    const char *was = azconf_get(g_conf, po);
    cm_conf_walk(c, 0);
    int moved = 0;
    if (was && strcmp(was, c->owner)) {
        cm_owner_hash(was, hs);
        snprintf(aside, sizeof aside, CM_STATE "/.%s@%s.cfg", c->id, hs);
        mkdir(CM_STATE, 0755);
        if (c->ncfg || c->ncfgs) cm_cfg_write_old(c, aside);
        dbg("community: %s now comes from %s (was %s): its settings start fresh\n", c->id, c->owner, was);
        c->ncfg = c->ncfgs = 0;
        cm_conf_walk(c, 1);
        moved = 1;
    }
    cm_owner_hash(c->owner, hs);
    snprintf(aside, sizeof aside, CM_STATE "/.%s@%s.cfg", c->id, hs);
    snprintf(legacy, sizeof legacy, CM_STATE "/%s.cfg", c->id);
    int got_aside = cm_cfg_read_old(c, aside) == 0, got_legacy = cm_cfg_read_old(c, legacy) == 0;
    if (!moved && !got_aside && !got_legacy) return;
    cm_cfg_to_conf(c);
    g_cm_conf_save = 1;
    if (cm_conf_save() == 0) {   /* in the file now: the p7 copies are never read again */
        if (got_aside) unlink(aside);
        if (got_legacy) {
            unlink(legacy);
            dbg("community: %s: settings moved from %s into the settings file\n", c->id, legacy);
        }
    }
}
static int cm_cfg_get_u(const char *id, const char *key, int def) {
    cm_mod *c = cm_find(id);
    if (!c || !key) return def;
    for (int i = 0; i < c->ncfg; i++)
        if (!strcmp(c->cfg[i].k, key)) return c->cfg[i].v;
    return def;
}
static void cm_cfg_set_u(const char *id, const char *key, int val) {
    cm_mod *c = cm_find(id);
    if (!c || !cm_key_ok(key)) return;
    for (int i = 0; i < c->ncfg; i++)
        if (!strcmp(c->cfg[i].k, key)) {
            if (c->cfg[i].v != val) {
                c->cfg[i].v = val;
                c->cfg_dirty = 1;
            }
            return;
        }
    if (c->ncfg >= CM_CFG_MAX) return;
    snprintf(c->cfg[c->ncfg].k, sizeof c->cfg[0].k, "%s", key);
    c->cfg[c->ncfg].v = val;
    c->ncfg++;
    c->cfg_dirty = 1;
}
static long g_cm_cfg_writes;   /* settings files written this run (the host tests count them) */
static void cm_cfg_flush(cm_mod *c) {   /* under g_cm_mu: into g_conf; cm_conf_save writes it */
    c->cfg_dirty = 0;
    g_cm_cfg_writes++;
    if (cm_cfg_to_conf(c)) g_cm_conf_save = 1;
}
static int cm_key_ok(const char *key) {
    if (!key || !key[0] || strchr(key, '=') || strchr(key, '$') || strlen(key) > 31) return 0;
    for (const unsigned char *q = (const unsigned char *) key; *q; q++)
        if (*q < 0x20 || *q == 0x7f) return 0; /* one line per key in the file */
    return 1;
}
/* text settings: up to 127 bytes, one line (CR/LF and control bytes are dropped) */
static int cm_cfg_get_str_u(const char *id, const char *key, char *buf, int n, const char *def) {
    if (!buf || n <= 0) return 0;
    cm_mod *c = cm_find(id);
    const char *v = def ? def : "";
    if (c && key) {
        for (int i = 0; i < c->ncfgs; i++)
            if (!strcmp(c->cfgs[i].k, key)) {
                v = c->cfgs[i].v;
                break;
            }
    }
    utf8_copy(buf, (size_t) n, v); /* a short buffer: whole characters only */
    return (int) strlen(buf);
}
static void cm_cfg_set_str_u(const char *id, const char *key, const char *val) {
    cm_mod *c = cm_find(id);
    if (!c || !cm_key_ok(key)) return;
    char v[128];
    int k = 0;
    const unsigned char *p = (const unsigned char *) (val ? val : "");
    for (; *p && k < (int) sizeof v - 1; p++)
        if (*p >= 0x20 && *p != 0x7f) v[k++] = (char) *p;
    v[k] = 0;
    if (*p) utf8_cut(v); /* cut at 127 bytes: never half a character */
    for (int i = 0; i < c->ncfgs; i++)
        if (!strcmp(c->cfgs[i].k, key)) {
            if (strcmp(c->cfgs[i].v, v)) {
                snprintf(c->cfgs[i].v, sizeof c->cfgs[0].v, "%s", v);
                c->cfg_dirty = 1;
            }
            return;
        }
    if (c->ncfgs >= CM_CFGS_MAX) return;
    snprintf(c->cfgs[c->ncfgs].k, sizeof c->cfgs[0].k, "%s", key);
    snprintf(c->cfgs[c->ncfgs].v, sizeof c->cfgs[0].v, "%s", v);
    c->ncfgs++;
    c->cfg_dirty = 1;
}
/* ---- one id, one owner ---------------------------------------------------------------------
 * A mod's settings (p7: azmod-community/<id>.cfg) and files (the stick: MODS/community-mods/<id>/data/) are
 * found by its id, and an id is not an author: two authors can pick the same one, and a DJ can
 * remove one author's mod and install another's under its id. Neither may start with the other's
 * settings, files or crash suspension. So that state is bound to the owner of the installed copy:
 * the repository it was installed from (the repo= line of MODS/community-mods/<id>/.origin, written by
 * the installer), or "local" for a copy without one. The stamp beside the state (<id>.owner on p7,
 * .<id>.owner on the stick) names the owner it belongs to. When the owner changes, the old owner's
 * state is set aside as .<id>@<hash of that owner> (a dot name: never an id, never handed out) and
 * the new owner's own set-aside state, if any, comes back; a stamp-less state (first run) is
 * adopted. This keeps mods from inheriting each other's state by accident. It is not a sandbox: a
 * community mod runs with the deck software's full access. */
static void cm_owner_key(const char *origin, char *out, size_t n) {
    out[0] = 0;
    int fd = open(origin, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        char b[1024];
        ssize_t k = read(fd, b, sizeof b - 1);
        close(fd);
        if (k > 0) {
            b[k] = 0;
            for (char *l = b; l && *l;) {
                char *e = strchr(l, '\n');
                if (e) *e = 0;
                if (!strncmp(l, "repo=", 5)) {
                    size_t L = strcspn(l + 5, "\r \t");
                    if (L > 0 && L < n) {
                        memcpy(out, l + 5, L);
                        out[L] = 0;
                    }
                    break;
                }
                l = e ? e + 1 : NULL;
            }
        }
    }
    if (!out[0]) {
        snprintf(out, n, "local");
        return;
    }
    /* GitHub ignores case in owner/name: Owner/Mods and owner/mods are one repository */
    if (!strncmp(out, "https://raw.githubusercontent.com/", 34))
        for (char *q = out; *q; q++)
            if (*q >= 'A' && *q <= 'Z') *q = (char) (*q - 'A' + 'a');
}
static void cm_owner_hash(const char *owner, char out[17]) {
    uint64_t h = cm_fnv(1469598103934665603ULL, (const unsigned char *) owner, strlen(owner));
    snprintf(out, 17, "%016llx", (unsigned long long) h);
}
static void cm_read_line(const char *p, char *out, size_t n) {   /* the first line of a small file, or "" */
    out[0] = 0;
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    ssize_t k = read(fd, out, n - 1);
    close(fd);
    out[k > 0 ? k : 0] = 0;
    out[strcspn(out, "\r\n")] = 0;
}
static int cm_write_line(const char *p, const char *v) {   /* tmp + rename; 0 = written */
    char t[340];
    snprintf(t, sizeof t, "%s.new", p);
    int fd = open(t, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    size_t L = strlen(v);
    int ok = write(fd, v, L) == (ssize_t) L && write(fd, "\n", 1) == 1;
    if (close(fd) != 0) ok = 0;
    if (!ok || rename(t, p) != 0) {
        unlink(t);
        return -1;
    }
    return 0;
}
/* p7: before the mod's settings are read or its crash suspension checked (cm_load_one) */
static void cm_owner_p7(cm_mod *c) {
    char sp[200], cur[320], cfg[200], aside[220], hs[17];
    snprintf(sp, sizeof sp, CM_STATE "/%s.owner", c->id);
    cm_read_line(sp, cur, sizeof cur);
    if (cur[0] && !strcmp(cur, c->owner)) return;
    mkdir(CM_STATE, 0755);
    snprintf(cfg, sizeof cfg, CM_STATE "/%s.cfg", c->id);
    if (cur[0]) {
        cm_owner_hash(cur, hs);
        snprintf(aside, sizeof aside, CM_STATE "/.%s@%s.cfg", c->id, hs);
        if (rename(cfg, aside) != 0 && errno != ENOENT) unlink(cfg);   /* never hand it to the new owner */
        snprintf(aside, sizeof aside, CM_STATE "/%s.crashed", c->id);
        unlink(aside);   /* stale crash record */
        cm_owner_hash(c->owner, hs);
        snprintf(aside, sizeof aside, CM_STATE "/.%s@%s.cfg", c->id, hs);
        rename(aside, cfg);                                            /* its own, from before (if any) */
        dbg("community: %s now comes from %s (was %s): its settings start fresh\n", c->id, c->owner, cur);
    }
    if (cm_write_line(sp, c->owner) != 0 && cur[0]) unlink(cfg);   /* unstamped: never adopted by a third owner */
}

/* ---- the approved list (CM_APPROVED): read once per start, written by Allow and the
 * grandfathering (never on a hot path). Malformed lines are dropped, never matched. */
static struct {
    char id[33], sha[65];
} g_cm_ok[CM_APPROVED_MAX];
static int g_cm_nok = 0;
static int cm_sha_ok(const char *s) {
    if (!s || strlen(s) != 64) return 0;
    for (; *s; s++)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return 0;
    return 1;
}
static void cm_ok_put(const char *id, const char *sha) {   /* one entry per id, newest last */
    for (int k = 0; k < g_cm_nok; k++)
        if (!strcmp(g_cm_ok[k].id, id)) {
            memmove(&g_cm_ok[k], &g_cm_ok[k + 1], sizeof g_cm_ok[0] * (size_t) (g_cm_nok - k - 1));
            g_cm_nok--;
            break;
        }
    if (g_cm_nok == CM_APPROVED_MAX) {
        memmove(&g_cm_ok[0], &g_cm_ok[1], sizeof g_cm_ok[0] * (CM_APPROVED_MAX - 1));
        g_cm_nok--;
    }
    snprintf(g_cm_ok[g_cm_nok].id, sizeof g_cm_ok[0].id, "%s", id);
    snprintf(g_cm_ok[g_cm_nok].sha, sizeof g_cm_ok[0].sha, "%s", sha);
    g_cm_nok++;
}
static int cm_id_ok(const char *id);
static void cm_approved_read(void) {
    g_cm_nok = 0;
    FILE *f = fopen(CM_APPROVED, "re");
    if (!f) return;
    char line[200], id[40], sha[80], more;
    while (fgets(line, sizeof line, f))   /* exactly "<id> <64 lowercase hex>"; anything else is skipped */
        if (sscanf(line, "%39s %79s %c", id, sha, &more) == 2 && cm_id_ok(id) && cm_sha_ok(sha)) cm_ok_put(id, sha);
    fclose(f);
}
static int cm_is_approved(const char *id, const char *sha) {
    if (!cm_sha_ok(sha)) return 0;
    for (int k = 0; k < g_cm_nok; k++)
        if (!strcmp(g_cm_ok[k].id, id) && !strcmp(g_cm_ok[k].sha, sha)) return 1;
    return 0;
}
/* add (or replace) one line: under the lock az-community takes too, re-read first (it may have
 * written since this start read the list), then tmp + fsync + rename. 0 = on p7. */
static int cm_approve(const char *id, const char *sha) {
    if (!cm_id_ok(id) || !cm_sha_ok(sha)) return -1;
    mkdir(CM_STATE, 0755);
    int lk = open(CM_APPROVED_LOCK, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lk < 0) return -1;
    int locked = 0;
    for (int i = 0; i < 50 && !(locked = flock(lk, LOCK_EX | LOCK_NB) == 0); i++) {
        struct timespec ts = {0, 20 * 1000000L};
        nanosleep(&ts, NULL);
    }   /* the other writer holds it for milliseconds */
    if (!locked) {
        close(lk);
        return -1;
    }
    cm_approved_read();
    cm_ok_put(id, sha);
    int rc = -1;
    char b[CM_APPROVED_MAX * 100];
    size_t L = 0;
    for (int k = 0; k < g_cm_nok; k++) L += (size_t) snprintf(b + L, sizeof b - L, "%s %s\n", g_cm_ok[k].id, g_cm_ok[k].sha);
    int fd = open(CM_APPROVED ".new", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        int ok = write(fd, b, L) == (ssize_t) L && fsync(fd) == 0;
        if (close(fd) != 0) ok = 0;
        if (ok && rename(CM_APPROVED ".new", CM_APPROVED) == 0) {
            rc = 0;
            int dfd = open(CM_STATE, O_RDONLY | O_CLOEXEC);
            if (dfd >= 0) {
                fsync(dfd);
                close(dfd);
            }
        } else unlink(CM_APPROVED ".new");
    }
    flock(lk, LOCK_UN);
    close(lk);
    if (rc) cm_approved_read();   /* memory = what is on p7 */
    return rc;
}
static int cm_sha256_file(const char *path, char out[65]) {
    out[0] = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > CM_SO_CAP) {
        close(fd);
        return -1;
    }
    azsha256_ctx h;
    azsha256_init(&h);
    static unsigned char buf[65536];
    ssize_t k;
    off_t at = 0;
    while ((k = pread(fd, buf, sizeof buf, at)) > 0) {
        azsha256_update(&h, buf, (size_t) k);
        at += k;
    }
    close(fd);
    if (k < 0 || at != st.st_size) return -1;
    azsha256_final_hex(&h, out);
    return 0;
}
/* The install record both installers write beside a mod (az-community, the desktop app): a
 * repo= https:// address, the package's sha256= and installed=. A folder copied by hand has none. */
static int cm_origin_record(const char *dir) {
    char p[200], b[1024];
    snprintf(p, sizeof p, "%s/.origin", dir);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t k = read(fd, b, sizeof b - 1);
    close(fd);
    if (k <= 0) return 0;
    b[k] = 0;
    int repo = 0, sha = 0, when = 0;
    for (char *l = b; l && *l;) {
        char *e = strchr(l, '\n');
        if (e) *e = 0;
        l[strcspn(l, "\r")] = 0;
        if (!strncmp(l, "repo=https://", 13) && l[13]) repo = 1;
        else if (!strncmp(l, "sha256=", 7) && cm_sha_ok(l + 7)) sha = 1;
        else if (!strncmp(l, "installed=", 10) && l[10]) when = 1;
        l = e ? e + 1 : NULL;
    }
    return repo && sha && when;
}
/* Grandfathering, once: the first start of a core with the approved list approves the mods that
 * are installed and turned on right then, so a DJ updating AZ-MODS is not asked about mods the
 * deck was already running. All three must hold:
 *   - the deck has community state from before (CM_STATE existed when this start began): it has
 *     run community mods under an earlier core. A deck that never did has nothing to grandfather;
 *   - the mod is turned on (in enabled, no <id>.off);
 *   - it carries an install record (cm_origin_record): installed by the deck or the desktop app.
 *     A folder copied by hand has none.
 * Anything else asks once, like any new mod. The record is not proof (a stick can carry a forged
 * one), which is why this runs exactly once: the window is one start, and every mod that appears
 * after it asks. It runs whatever else holds this start back (all off, quarantine), so "the first
 * start" is the first one, not the first that loads mods. A deck with no community state and
 * nothing enabled writes nothing: the decision waits for the first start that has
 * enabled mods, and that start grandfathers none (no earlier state). CM_APPROVED_INIT is written
 * only when every approval reached p7; otherwise the next start tries again. Reads each .so
 * (data: nothing of it runs). */
static void cm_grandfather(int nen) {
    if (access(CM_APPROVED_INIT, F_OK) == 0) return;
    struct stat sd;
    int had = stat(CM_STATE, &sd) == 0 && S_ISDIR(sd.st_mode);
    if (!had && !nen) return;
    if (!had) dbg("community: no community state from an earlier core: nothing grandfathered, new mods ask\n");
    int failed = 0, n = 0;
    for (int i = 0; had && i < nen; i++) {
        cm_mod *c = &g_cm[i];
        char dir[160], p[200], sha[65];
        snprintf(dir, sizeof dir, CM_STAGE "/%s", c->id);
        snprintf(p, sizeof p, "%s/%s.off", dir, c->id);
        if (access(p, F_OK) == 0) continue;
        if (!cm_origin_record(dir)) {
            dbg("community: %s not grandfathered: no install record (.origin); it asks once\n", c->id);
            continue;
        }
        snprintf(p, sizeof p, "%s/%s.so", dir, c->id);
        if (cm_sha256_file(p, sha)) {
            dbg("community: %s not grandfathered: its .so could not be read\n", c->id);
            continue;
        }
        if (cm_approve(c->id, sha)) {
            failed = 1;
            dbg("community: %s: the approved list could not be written\n", c->id);
            continue;
        }
        n++;
        dbg("community: %s grandfathered (installed and on before approvals existed): %s\n", c->id, sha);
        cm_logf(c, 1, "approved once without asking (installed before approvals existed): sha256 %s", sha);
    }
    if (failed) return;
    mkdir(CM_STATE, 0755);
    char v[32];
    snprintf(v, sizeof v, "grandfathered=%d", n);
    if (cm_write_line(CM_APPROVED_INIT, v) == 0) dbg("community: approved list started, %d mod(s) grandfathered\n", n);
}

/* ---- files on the USB stick: the stick this mod was installed on (the one carrying
 * MODS/community-mods/<id>/azmod.json from the same owner as the copy that loaded). data_dir = MODS/community-mods/<id>/data/
 * (created; not mirrored to the deck, kept across updates and reinstalls); assets_dir = the package's
 * assets/ folder (read-only, never copied to the deck). NULL = no such stick right now
 * (ejected): the mod carries on without its files. Answers are cached CM_DIR_TTL_MS. */
static int cm_stick_of(const cm_mod *c, char *mods, size_t n) {
    DIR *d = opendir(CM_USB_ROOT);
    if (!d) return 0;
    struct dirent *e;
    int ok = 0;
    while (!ok && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[320], o[320];   /* a path that does not fit is skipped, never used cut short */
        if ((size_t) snprintf(p, sizeof p, CM_USB_ROOT "/%s/MODS/community-mods/%s/azmod.json", e->d_name, c->id) >= sizeof p) continue;
        if (access(p, F_OK) != 0) continue;
        /* a second stick may carry another author's mod under the same id: not this mod's stick */
        if ((size_t) snprintf(p, sizeof p, CM_USB_ROOT "/%s/MODS/community-mods/%s/.origin", e->d_name, c->id) >= sizeof p) continue;
        cm_owner_key(p, o, sizeof o);
        if (strcmp(o, c->owner)) continue;
        if ((size_t) snprintf(mods, n, CM_USB_ROOT "/%s/MODS", e->d_name) < n) ok = 1;
    }
    closedir(d);
    return ok;
}
/* The buffers below are handed to mods and are never blanked or shortened: a mod that kept an
 * old pointer must still hold a full path, never "" (which "%s/save.txt" would turn into a file
 * on the deck's root filesystem). A missing stick is reported by returning NULL; the buffer only
 * changes when the stick's folder is found again at a different path. */
static void cm_set_path(char *buf, size_t n, const char *path) {
    if (strcmp(buf, path)) snprintf(buf, n, "%s", path);
}
static const char *cm_data_dir_u(const char *id) {
    cm_mod *c = cm_find(id);
    if (!c) return NULL;
    long now = now_ms();
    /* a cached path is reused only while it still exists: an eject unmounts the stick at
     * once, and a stale answer would point a mod's writes at the empty mount point */
    if (c->data_at && now - c->data_at < CM_DIR_TTL_MS && (!c->data_ok || access(c->data, F_OK) == 0))
        return c->data_ok ? c->data : NULL;
    c->data_at = now ? now : 1;
    c->data_ok = 0;
    char mods[200], base[240], dir[260], sp[260], cur[320], aside[280], hs[17];
    if (!cm_stick_of(c, mods, sizeof mods)) return NULL;
    snprintf(base, sizeof base, "%s/community-mods/%s", mods, id);   /* the mod's folder: it exists */
    snprintf(dir, sizeof dir, "%s/data", base);
    /* the folder belongs to one owner (see cm_owner_key): another owner's is set aside first */
    snprintf(sp, sizeof sp, "%s/.data.owner", base);
    cm_read_line(sp, cur, sizeof cur);
    if (!cur[0] || strcmp(cur, c->owner)) {
        if (cur[0]) {
            cm_owner_hash(cur, hs);
            snprintf(aside, sizeof aside, "%s/.data@%s", base, hs);
            if (rename(dir, aside) != 0 && errno != ENOENT) return NULL;   /* never hand it over */
            cm_owner_hash(c->owner, hs);
            snprintf(aside, sizeof aside, "%s/.data@%s", base, hs);
            rename(aside, dir);                                           /* its own, from before (if any) */
        }
        if (cm_write_line(sp, c->owner) != 0) return NULL;
    }
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) return NULL;
    cm_set_path(c->data, sizeof c->data, dir);
    c->data_ok = 1;
    return c->data;
}
static const char *cm_assets_dir_u(const char *id) {
    cm_mod *c = cm_find(id);
    if (!c) return NULL;
    long now = now_ms();
    if (c->assets_at && now - c->assets_at < CM_DIR_TTL_MS && (!c->assets_ok || access(c->assets, F_OK) == 0))
        return c->assets_ok ? c->assets : NULL;
    c->assets_at = now ? now : 1;
    c->assets_ok = 0;
    char mods[200], dir[260];
    if (!cm_stick_of(c, mods, sizeof mods)) return NULL;
    snprintf(dir, sizeof dir, "%s/community-mods/%s/assets", mods, id);
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return NULL;
    cm_set_path(c->assets, sizeof c->assets, dir);
    c->assets_ok = 1;
    return c->assets;
}

/* Settings and the file folders may be used from a mod's own threads as well as from tick and
 * its page: one lock (recursive: these call each other) around each. Not hot paths. The flush
 * in community_tick takes it too. */
static pthread_mutex_t g_cm_mu = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
/* The services take the caller's id as a plain string. A mod must only act as itself (a copied
 * example id, or a careless mod, must not change another mod's settings or hooks), so each service
 * checks that it was called from inside the named mod's own code. Unknown range = refused for
 * hooks, allowed for the rest (it is set before init() runs, so that is rare). */
/* A service called as the last statement of a function the core called is a tail call at -O2: its
 * return address lies in the core, so it reads as outside the mod. */
#define CM_OUTSIDE_CODE \
    "called with this mod's id from outside its code (or as the last call of a callback: build with -fno-optimize-sibling-calls)"
static int cm_called_by(cm_mod *c, uintptr_t ra) {
    return c && c->hi && ra >= c->lo && ra < c->hi;
}
static cm_mod *cm_self(const char *id, uintptr_t ra, const char *what) {
    cm_mod *c = cm_find(id);
    if (!c) return NULL;
    if (g_cm_in_frame) {
        __atomic_add_fetch(&c->fq.refused, 1, __ATOMIC_RELAXED);
        return NULL;
    }   /* never block the bus */
    /* Only the mod's own code may use its id. A mod that was never loaded (refused, switched
     * off, waiting for approval) has no code range, so nothing may act as it: another mod
     * could otherwise read its settings and folders or launch its unapproved programs. */
    if (!cm_called_by(c, ra)) {
        /* logged once per service and mod: every kind of misuse shows, none floods the log */
        int seen = 0, k = 0;
        for (; k < (int) (sizeof c->refused_said / sizeof c->refused_said[0]) && c->refused_said[k]; k++)
            if (c->refused_said[k] == what || !strcmp(c->refused_said[k], what)) seen = 1;
        if (!seen) {
            if (k < (int) (sizeof c->refused_said / sizeof c->refused_said[0])) c->refused_said[k] = what;
            cm_logf(c, 1, "%s refused: " CM_OUTSIDE_CODE, what);
        }
        return NULL;
    }
    return c;
}
#define CM_RA() ((uintptr_t) __builtin_extract_return_addr(__builtin_return_address(0)))
static int cm_cfg_get(const char *id, const char *key, int def) {
    if (!cm_self(id, CM_RA(), "cfg_get")) return def;
    pthread_mutex_lock(&g_cm_mu);
    int v = cm_cfg_get_u(id, key, def);
    pthread_mutex_unlock(&g_cm_mu);
    return v;
}
static void cm_cfg_set(const char *id, const char *key, int val) {
    if (!cm_self(id, CM_RA(), "cfg_set")) return;
    pthread_mutex_lock(&g_cm_mu);
    cm_cfg_set_u(id, key, val);
    pthread_mutex_unlock(&g_cm_mu);
}
static int cm_cfg_get_str(const char *id, const char *key, char *buf, int n, const char *def) {
    if (!cm_self(id, CM_RA(), "cfg_get_str")) {
        if (buf && n > 0) {
            utf8_copy(buf, (size_t) n, def ? def : "");
            return (int) strlen(buf);
        }
        return 0;
    }
    pthread_mutex_lock(&g_cm_mu);
    int v = cm_cfg_get_str_u(id, key, buf, n, def);
    pthread_mutex_unlock(&g_cm_mu);
    return v;
}
static void cm_cfg_set_str(const char *id, const char *key, const char *val) {
    if (!cm_self(id, CM_RA(), "cfg_set_str")) return;
    pthread_mutex_lock(&g_cm_mu);
    cm_cfg_set_str_u(id, key, val);
    pthread_mutex_unlock(&g_cm_mu);
}
static const char *cm_data_dir(const char *id) {
    if (!cm_self(id, CM_RA(), "data_dir")) return NULL;
    pthread_mutex_lock(&g_cm_mu);
    const char *v = cm_data_dir_u(id);
    pthread_mutex_unlock(&g_cm_mu);
    return v;
}
static const char *cm_assets_dir(const char *id) {
    if (!cm_self(id, CM_RA(), "assets_dir")) return NULL;
    pthread_mutex_lock(&g_cm_mu);
    const char *v = cm_assets_dir_u(id);
    pthread_mutex_unlock(&g_cm_mu);
    return v;
}

/* ---- per-mod log: one line per call, flushed, capped; only with logs on, except `force`
 * (the load result, which the DJ may need to send to the mod's author). */
static _Atomic int g_cm_logs_on = 0; /* tick writes it, any mod thread that logs reads it */
static void cm_vlog(cm_mod *c, int force, const char *fmt, va_list ap) {
    if (!c || (!force && !g_cm_logs_on) || __atomic_load_n(&c->log_bytes, __ATOMIC_RELAXED) >= CM_LOG_CAP) return;
    if (g_cm_in_frame) {   /* queue it: the bus thread runs at real-time priority and must not block */
        unsigned k = __atomic_fetch_add(&c->fq.w, 1, __ATOMIC_RELAXED) % 8;
        if (__atomic_load_n(&c->fq.ready[k], __ATOMIC_ACQUIRE)) {
            __atomic_add_fetch(&c->fq.dropped, 1, __ATOMIC_RELAXED);
            return;
        }
        vsnprintf(c->fq.line[k], sizeof c->fq.line[k], fmt, ap);
        __atomic_store_n(&c->fq.ready[k], 1, __ATOMIC_RELEASE);
        return;
    }
    char line[400];
    int k = vsnprintf(line, sizeof line - 1, fmt, ap);
    if (k < 0) return;
    if (k > (int) sizeof line - 2) k = (int) sizeof line - 2;
    if (k == 0 || line[k - 1] != '\n') line[k++] = '\n';
    mkdir(CM_LOGDIR, 0755);
    char p[160];
    snprintf(p, sizeof p, CM_LOGDIR "/%s.log", c->id);
    /* the cap is per file, not per start: the file outlives EP147 restarts, so the first line
     * of a run counts what is already there, or moves a full file to <id>.log.1 */
    if (!__atomic_exchange_n(&c->log_sized, 1, __ATOMIC_ACQ_REL)) {
        struct stat st;
        if (azlog_rotate(p, CM_LOG_CAP / 2) || stat(p, &st) != 0) st.st_size = 0;
        __atomic_add_fetch(&c->log_bytes, (long) st.st_size, __ATOMIC_RELAXED);
        if (__atomic_load_n(&c->log_bytes, __ATOMIC_RELAXED) >= CM_LOG_CAP) return;
    }
    int fd = open(p, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);   /* logcap-ok: log_bytes caps it */
    if (fd < 0) return;
    if (write(fd, line, (size_t) k) == k) __atomic_add_fetch(&c->log_bytes, k, __ATOMIC_RELAXED);   /* one append per line: safe from any thread */
    close(fd);
}
static void cm_logf(cm_mod *c, int force, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    cm_vlog(c, force, fmt, ap);
    va_end(ap);
}
static void cm_log(const char *id, const char *fmt, ...) {
    cm_mod *c = cm_find(id);
    if (!cm_called_by(c, CM_RA())) return;   /* only into the caller's own log (no queue needed: nothing is written) */
    va_list ap;
    va_start(ap, fmt);
    cm_vlog(c, 0, fmt, ap);
    va_end(ap);
}

/* ---- hooks: one address, one owner; core's own hooks count (g_hooked, azm_install_hook) */
static int cm_claim_hook(const char *id, uintptr_t addr, void *hook, void **orig) {
    cm_mod *c = cm_find(id);
    if (!c) {
        dbg("community: claim_hook from unknown id '%s' refused\n", id ? id : "?");
        return -1;
    }
    if (!cm_called_by(c, CM_RA())) {
        cm_logf(c, 1, "claim_hook 0x%lx refused: " CM_OUTSIDE_CODE, (unsigned long) addr);
        return -1;
    }
    if (!g_sdk_in_init || (c - g_cm) != g_cm_loading) { /* a hook goes in before the mod runs */
        cm_logf(c, 1, "claim_hook 0x%lx refused: call it from init()", (unsigned long) addr);
        return -1;
    }
    if (!c->hooks_ok) {
        cm_logf(c, 1, "claim_hook 0x%lx refused: %s", (unsigned long) addr,
                c->any ? "a mod for \"any\" firmware may not patch code" : "azmod.json does not declare \"hooks\": true");
        return -1;
    }
    if (addr < g_cm_text_lo || addr >= g_cm_text_hi) {
        cm_logf(c, 1, "claim_hook 0x%lx refused: not inside EP147's code", (unsigned long) addr);
        return -1;
    }
    /* a hook rewrites 16 bytes: any address within 16 bytes of an existing hook, either side,
     * would corrupt it (its jump or the code it relocated). One check-and-install at a time. */
    pthread_mutex_lock(&g_cm_mu);
    for (int i = 0; i < g_hooked_n; i++)
        if (addr < g_hooked[i].addr + 16 && g_hooked[i].addr < addr + 16) {
            pthread_mutex_unlock(&g_cm_mu);
            cm_logf(c, 1, "claim_hook 0x%lx refused: overlaps the hook at 0x%lx patched by %s", (unsigned long) addr,
                    (unsigned long) g_hooked[i].addr, g_hooked[i].who);
            return -1;
        }
    g_hook_owner = c->id;
    int rc = azm_install_hook(addr, hook, orig);
    g_hook_owner = "AZ-MODS";
    pthread_mutex_unlock(&g_cm_mu);
    if (rc == AZM_REFUSED_LATE) {
        cm_logf(c, 1, "claim_hook 0x%lx refused: no jump island within reach of it, and a 16-byte patch is not written while EP147 runs", (unsigned long) addr);
        return -1;
    }
    if (rc != 1) {
        cm_logf(c, 1, "claim_hook 0x%lx refused: the hook installer declined it", (unsigned long) addr);
        return -1;
    }
    c->nhooks++;
    __atomic_store_n(&c->sticky, 1, __ATOMIC_RELAXED);
    cm_logf(c, 1, "claim_hook 0x%lx ok", (unsigned long) addr);
    return 0;
}

static int azcm_deck_for_ds(void *ds) { return deck_for_ds((long) ds); }
static int azcm_deck_for_modeobj(void *o) { return deck_for_modeobj((long) o); }

/* ---- read-only deck state ---------------------------------------------------------- */
static double g_cm_bpm[AZMOD_STEM_DECKS];
static int (*g_cm_track_fn)(int, char *, int);
static int cm_deck_count(void) { return g_four_deck ? 4 : 2; }
/* Loaded state and title come from azstem's per-deck snapshot (EP147's LoadedTrackInfo for
 * every deck), which does not depend on the stick having a STEMS tree; the stems' own track
 * name (azstem_deck_track_name) exists only while stems are found. */
static azstem_deck_snapshot_fn g_cm_snap_fn;
static int cm_snap(int d, struct azstem_deck_snap *s) {
    if (!g_cm_snap_fn) *(void **) &g_cm_snap_fn = dlsym(RTLD_DEFAULT, "azstem_deck_snapshot");
    s->size = (int) sizeof *s;
    return g_cm_snap_fn ? g_cm_snap_fn(d, s) : -1;
}
static int cm_deck_loaded(int d) {
    if (d < 0 || d >= AZMOD_STEM_DECKS) return 0;
    struct azstem_deck_snap s;
    return cm_snap(d, &s) == 0 && s.loaded == 1;
}
/* 1 when a track is loaded on any deck, or when that cannot be told (the Reload confirm asks then).
 * Prefer azstem_decks_loaded (EP147's LoadedTrackInfo for every deck): the per-deck fallback
 * follows the stems' track name, which is maintained only while the stick has a STEMS tree. */
int azcm_any_loaded(void) {
    static int (*decks_fn)(void);
    if (!decks_fn) *(void **) &decks_fn = dlsym(RTLD_DEFAULT, "azstem_decks_loaded");
    if (decks_fn) return decks_fn() != 0;
    if (!g_deck_has_track_fn) return 1;
    for (int d = 0; d < AZMOD_STEM_DECKS; d++)
        if (g_deck_has_track_fn(d)) return 1;
    return 0;
}
static double cm_deck_bpm(int d) { return (d >= 0 && d < AZMOD_STEM_DECKS) ? g_cm_bpm[d] : 0.0; }
static int cm_deck_title(int d, char *buf, int n) {
    if (!buf || n <= 0) return 0;
    buf[0] = 0;
    if (d < 0 || d >= AZMOD_STEM_DECKS) return 0;
    if (!g_cm_track_fn) *(void **) &g_cm_track_fn = dlsym(RTLD_DEFAULT, "azstem_deck_track_name");
    int len = g_cm_track_fn ? g_cm_track_fn(d, buf, n) : 0;
    if (len > 0) return len; /* the file name, while the stems path knows it */
    struct azstem_deck_snap s;
    if (cm_snap(d, &s) != 0 || s.loaded != 1 || !s.title[0]) {
        buf[0] = 0;
        return 0;
    }
    utf8_copy(buf, (size_t) n, s.title); /* otherwise the track's title, whole characters only */
    return (int) strlen(buf);
}
int azmod_dm_key(void) __attribute__((weak));
static int cm_ui_theme(void) { return azmod_dm_key ? azmod_dm_key() : 0; }
static const char *cm_fw_tag(void) { return g_cm_fwtag; }
static void cm_request_redraw(void) { __atomic_store_n(&g_cm_redraw, 1, __ATOMIC_RELAXED); }
static int cm_register_screen(const char *id, const az_screen_v1 *scr) {
    cm_mod *c = cm_self(id, CM_RA(), "register_screen");
    if (!c) return -1;
    if (!g_sdk_in_init || (c - g_cm) != g_cm_loading) {
        cm_logf(c, 1, "register_screen refused: call it from init()");
        return -1;
    }
    if (!scr || scr->size < (int) (offsetof(az_screen_v1, on_input) + sizeof scr->on_input) || !scr->on_draw) {
        cm_logf(c, 1, "register_screen refused: the screen is incomplete (its size, or no on_draw)");
        return -1;
    }
    c->screen = scr;
    return 0;
}

/* sdk_input.c (included after this file) */
static int cm_frame_filter(const char *id, unsigned bus_mask, az_frame_fn fn, void *user);
static int cm_touch_filter(const char *id, az_touch_fn fn, void *user);
static void sdk_timing_init(void);
static void sdk_input_forget(const cm_mod *c);
/* A paint context (az_draw) is live only inside the on_draw / on_paint call that received it: the
 * core marks it for the call and clears it after (cm_paint_begin / cm_paint_end). A context a mod
 * kept and used later, or used from another thread, points at memory the core has moved on from:
 * every drawing service refuses it, the first refusal is logged, and nothing is drawn. */
static __thread az_draw *g_cm_paint;
static void cm_paint_begin(az_draw *d) { g_cm_paint = d; }
static void cm_paint_end(void) { g_cm_paint = NULL; }
static int cm_paint_live(const az_draw *d, const char *what) {
    static int logged;
    if (d && d == g_cm_paint)
        return 1;
    if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
        dbg("community: %s refused: a paint context used outside the on_draw / on_paint call "
            "that received it (logged once)\n",
            what);
    return 0;
}
static void cm_draw_header(az_draw *d, const char *t) {
    if (cm_paint_live(d, "draw_header")) azcm_draw_header(d, t);
}
static void cm_draw_text(az_draw *d, int x, int y, const char *t, int px, uint32_t rgb) {
    if (cm_paint_live(d, "draw_text")) azcm_draw_text(d, x, y, t, px, rgb);
}
static void cm_fill_rect(az_draw *d, int x, int y, int w, int h, uint32_t rgb) {
    if (cm_paint_live(d, "fill_rect")) azcm_fill_rect(d, x, y, w, h, rgb);
}
static void cm_draw_rect(az_draw *d, int x, int y, int w, int h, int t, uint32_t rgb) {
    if (cm_paint_live(d, "draw_rect")) azcm_draw_rect(d, x, y, w, h, t, rgb);
}
static void cm_draw_checkbox(az_draw *d, int x, int y, int size, int on, int hi) {
    if (cm_paint_live(d, "draw_checkbox")) azcm_draw_checkbox(d, x, y, size, on, hi);
}
static int cm_row_height(az_draw *d) { return cm_paint_live(d, "row_height") ? azcm_row_height(d) : 0; }
/* The drawing services. They need the overlay thread; the host's unit test builds this file
 * without one and names its own stand-ins in SDK_SURFACE_IMPL. */
#ifndef SDK_SURFACE_IMPL
#define SDK_SURFACE_IMPL "sdk_surface.c"
#endif
#include SDK_SURFACE_IMPL
/* svc: deck-ui, deck state, feedback and threads (sdk_deck.c, sdk_ui.c, #included after this file) */
static int sd_deck_info(int deck, az_deck_info *out);
static int sd_deck_grid(int deck, double *beat_phase, double *bar_phase);
static int sd_on_deck_event(const char *id, void (*fn)(int deck, int what, void *user), void *user);
static void sd_banner(const char *id, const char *text, int ms);
static int sd_confirm(const char *id, const char *title, const char *text, const char *yes,
                      void (*fn)(int yes, void *user), void *user);
static void sd_post_ui(const char *id, void (*fn)(void *user), void *user);
static int sd_thread_start(const char *id, const char *name, void *(*fn)(void *), void *arg);
static int sd_threads_of(int owner);
static void sdk_deck_forget(int owner);
static int sdk_proc_forget(const char *id);
static void sdk_rows_forget(cm_mod *c);
static void sdk_deck_tick(long now);
static void sdk_ui_tick(void);
/* The community host's unit test builds this file without azmod.c and names its stand-ins. */
#ifdef SDK_DECKUI_IMPL
#include SDK_DECKUI_IMPL
#endif
/* svc: rows (the Mod Menu's main list) */
#ifdef SDK_ROWS_IMPL
#include SDK_ROWS_IMPL /* the community host's unit test: no feature table or settings */
#else
#include "sdk_rows.c"
#endif
/* svc: proc-lib-audio (other programs, reload, the library read-only, audio taps, vtable slots) */
#ifdef SDK_PROC_IMPL
#include SDK_PROC_IMPL /* the community host's unit test: no EP147 audio, systemd or stick */
#else
#include "sdk_proc.c"
#include "sdk_lib.c"
#include "sdk_audio.c"
#include "sdk_vslot.c"
#endif

static const az_core_v1 g_cm_core = {
    .abi = AZ_MOD_ABI,
    .size = sizeof(az_core_v1),
    .deck_for_ds = azcm_deck_for_ds,
    .deck_for_modeobj = azcm_deck_for_modeobj,
    .claim_hook = cm_claim_hook,
    .cfg_get = cm_cfg_get,
    .cfg_set = cm_cfg_set,
    .log = cm_log,
    .fw_tag = cm_fw_tag,
    .register_screen = cm_register_screen,
    .draw_header = cm_draw_header,
    .draw_text = cm_draw_text,
    .fill_rect = cm_fill_rect,
    .draw_rect = cm_draw_rect,
    .draw_checkbox = cm_draw_checkbox,
    .color = azcm_color,
    .row_height = cm_row_height,
    .text_width = azcm_text_width,
    .request_redraw = cm_request_redraw,
    .deck_count = cm_deck_count,
    .deck_loaded = cm_deck_loaded,
    .deck_bpm = cm_deck_bpm,
    .deck_title = cm_deck_title,
    .ui_theme = cm_ui_theme,
    .cfg_get_str = cm_cfg_get_str,
    .cfg_set_str = cm_cfg_set_str,
    .data_dir = cm_data_dir,
    .assets_dir = cm_assets_dir,
    /* svc: rows */
    .register_row = cm_register_row,
    .row_value = cm_row_value,
    .row_set = cm_row_set,
    .row_lookup = cm_row_lookup,
    /* svc: input */
    .frame_filter = cm_frame_filter,
    .touch_filter = cm_touch_filter,
    /* svc: deck-ui */
    .deck_info = sd_deck_info,
    .deck_grid = sd_deck_grid,
    .on_deck_event = sd_on_deck_event,
    .banner = sd_banner,
    .confirm = sd_confirm,
    .post_ui = sd_post_ui,
    .thread_start = sd_thread_start,
    /* end svc: deck-ui */
    /* svc: surfaces */
    .pointer_inject = sdk_pointer_inject,
    .pointer_cursor = sdk_pointer_cursor,
    .view = sdk_view,
    .surface_create = sdk_surface_create,
    .surface_place = sdk_surface_place,
    .surface_show = sdk_surface_show,
    .surface_redraw = sdk_surface_redraw,
    .surface_focus = sdk_surface_focus,
    .xwin_show = sdk_xwin_show,
    .xwin_find = sdk_xwin_find,
    .xwin_geometry = sdk_xwin_geometry,
    .draw_image = sdk_draw_image,
    .draw_line = sdk_draw_line,
    /* end svc: surfaces */
    /* svc: proc-lib-audio */
    .launch = sdk_launch,
    .launch_alive = sdk_launch_alive,
    .launch_stop = sdk_launch_stop,
    .handoff = sdk_handoff,
    .reload = sdk_reload,
    .lib_playlists = sdk_lib_playlists,
    .lib_tracks = sdk_lib_tracks,
    .audio_tap = sdk_audio_tap,
    .claim_vslot = sdk_claim_vslot,
    .lib_spool_kick = sdk_lib_spool_kick,
};

/* ---- loading ------------------------------------------------------------------------ */
static void cm_refuse(cm_mod *c, int state, const char *fmt, ...) {
    cm_set_state(c, state);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->reason, sizeof c->reason, fmt, ap);
    va_end(ap);
    dbg("community: %s %s: %s\n", c->id, state == AZCM_DISABLED ? "off" : "NOT loaded", c->reason);
    cm_logf(c, 1, "not loaded: %s", c->reason);
}
/* The mod's loaded segments: lowest and highest address of its PT_LOAD mappings. */
static int cm_range_cb(struct dl_phdr_info *info, size_t sz, void *data) {
    (void) sz;
    cm_mod *c = data;
    if ((uintptr_t) info->dlpi_addr != c->lo) return 0;
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type != PT_LOAD) continue;
        uintptr_t a = info->dlpi_addr + info->dlpi_phdr[i].p_vaddr;
        if (a < lo) lo = a;
        if (a + info->dlpi_phdr[i].p_memsz > hi) hi = a + info->dlpi_phdr[i].p_memsz;
    }
    if (hi) {
        c->lo = lo;
        c->hi = hi;
        return 1;
    }
    return 0;
}
static void cm_range_of(cm_mod *c) {
    if (!dl_iterate_phdr(cm_range_cb, c)) { c->lo = c->hi = 0; }
}

/* Crash blame: installed only while a community mod is loaded. On a fatal signal, find the mod
 * that owns the crash (cm_blame_fault), write its id to CM_BLAME with async-signal-safe calls
 * only, then restore the previous disposition and return: the instruction faults again and
 * EP147 dies exactly as it would have. The next start suspends only the blamed mod. A crash
 * with no mod on the PC, LR or the walked frames blames nobody (the start-time quarantine and
 * CM_CRASHES still apply). */
static struct sigaction g_cm_old[32];
static const int CM_SIGS[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
static int cm_owner_of(uintptr_t a) {
    for (int i = 0; i < g_cm_n; i++)
        if (g_cm[i].hi && a >= g_cm[i].lo && a < g_cm[i].hi) return i;
    return -1;
}
/* A crash nobody could be blamed for while community mods are loaded: append its time to
 * CM_CRASHES (repeated ones switch community mods off at the next start). Async-signal-safe. */
static void cm_note_unblamed_crash(void) {
    if (!cm_nlive()) return;
    int fd = open(CM_CRASHES, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);   /* logcap-ok: pruned to the window at every start */
    if (fd < 0) return;
    char b[24];
    int n = 0;
    unsigned long t = (unsigned long) time(NULL), d = 1;
    while (t / d >= 10) d *= 10;
    for (; d; d /= 10) b[n++] = (char) ('0' + (t / d) % 10);
    b[n++] = '\n';
    if (write(fd, b, (size_t) n) == n) { /* best effort */
    }
    fsync(fd);
    close(fd);
}
/* Name the mod a crash belongs to: the faulting pc, the return address, then up to 16 frames
 * up the stack (a mod that hands EP147 or libc a bad pointer crashes there, with the mod further
 * up). Frames are read with crash_peek8 (ep_mem.h), never dereferenced. Writes the id to
 * CM_BLAME and returns it; NULL when nobody is to blame (the unblamed crash is noted).
 * Async-signal-safe, once per process; called by cm_crash and by the crash reporter
 * (crash_guards.c), whichever owns the signal. */
static const char *cm_blame_fault(uintptr_t pc, uintptr_t lr, uintptr_t fp) {
    static volatile int written;              /* once per process: the re-faults that follow change nothing */
    if (written) return NULL;
    written = 1;
    int who = cm_owner_of(pc);
    if (who < 0) who = cm_owner_of(lr);
    for (int i = 0; who < 0 && i < 16 && fp >= 0x10000 && !(fp & 7); i++) {
        uintptr_t nfp = 0, ra = 0;
        if (!crash_peek8(fp, &nfp) || !crash_peek8(fp + 8, &ra)) break;
        who = cm_owner_of(ra);
        if (nfp <= fp) break;
        fp = nfp;
    }
    if (who < 0) {
        cm_note_unblamed_crash();
        return NULL;
    }
    int fd = open(CM_BLAME, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        size_t n = strlen(g_cm[who].id);
        if (write(fd, g_cm[who].id, n) == (ssize_t) n) { /* nothing else is safe here */
        }
        fsync(fd);
        close(fd);
    }
    return g_cm[who].id;
}
static void cm_crash(int sig, siginfo_t *si, void *ucv) {
    (void) si;
    ucontext_t *uc = ucv;
    uintptr_t pc = 0, lr = 0;
#if defined(__aarch64__)
    pc = (uintptr_t) uc->uc_mcontext.pc;
    lr = (uintptr_t) uc->uc_mcontext.regs[30];
    uintptr_t fp = (uintptr_t) uc->uc_mcontext.regs[29];
#else
    (void) uc;
    uintptr_t fp = 0;
#endif
    cm_blame_fault(pc, lr, fp);
    sigaction(sig, &g_cm_old[sig], NULL);       /* previous disposition; the fault repeats */
    if (sig == SIGABRT) raise(SIGABRT);           /* abort() does not re-fault: re-raise */
}
static void cm_install_blame(void) {
    static int done;   /* once: a second install would save cm_crash as its own previous action */
    if (done) return;
    done = 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = cm_crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof CM_SIGS / sizeof CM_SIGS[0]; i++) {
        int sig = CM_SIGS[i];
        /* One owner per signal. While the crash reporter is armed it owns all five and re-arms
         * itself in front of whoever takes them during the first minute; a second handler would
         * save the reporter as its previous action while the reporter saves it, and a fault
         * would bounce between the two forever (a hang, not a restart). The reporter calls
         * cm_blame_fault itself, so blame still lands. */
        if (g_cr_fd >= 0) continue;   /* the reporter owns all five while armed */
        sigaction(sig, &sa, &g_cm_old[sig]);
    }
}

/* Compare MAJOR.MINOR.PATCH strings: -1/0/1, or -2 when either is malformed (refuse). */
static int cm_ver3(const char *s, long v[3]) {
    for (int i = 0; i < 3; i++) {
        if (*s < '0' || *s > '9') return 0;
        char *e;
        v[i] = strtol(s, &e, 10);
        s = e;
        if (v[i] > 9999) return 0;
        if (i < 2) {
            if (*s != '.') return 0;
            s++;
        }
    }
    return *s == 0;
}
static int cm_vercmp(const char *a, const char *b) {
    long x[3], y[3];
    if (!cm_ver3(a, x) || !cm_ver3(b, y)) return -2;
    for (int i = 0; i < 3; i++)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}
/* The display fields only (name, version, author, summary) from the stage's azmod.json, when it
 * is this mod's: so a mod refused before its manifest is checked (turned off, or every community
 * mod off) still shows its real name. Reads nothing of its code. */
static void cm_names(cm_mod *c) {
    char p[200], js[16384], mid[40];
    int jn = 0;
    snprintf(p, sizeof p, CM_STAGE "/%s/azmod.json", c->id);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        jn = (int) read(fd, js, sizeof js - 1);
        close(fd);
    }
    if (jn > 0) {
        js[jn] = 0;
        if (cm_json_str(js, "id", mid, sizeof mid) && !strcmp(mid, c->id)) {
            cm_json_str(js, "name", c->name, sizeof c->name);
            cm_json_str(js, "version", c->version, sizeof c->version);
            cm_json_str(js, "author", c->author, sizeof c->author);
            cm_json_str(js, "summary", c->summary, sizeof c->summary);
        }
    }
    if (!c->name[0]) snprintf(c->name, sizeof c->name, "%s", c->id);
}
static void cm_mark_initing(const cm_mod *c) {
    mkdir(CM_STATE, 0755);
    int fd = open(CM_INITING, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        if (write(fd, c->id, strlen(c->id)) < 0) { /* best effort */
        }
        fsync(fd);
        close(fd);
    }
}
/* init() of an opened, checked mod; CM_INITING is already written. 0 = it runs. */
static int cm_run_init(cm_mod *c, const az_mod_v1 *m) {
    c->m = m;
    g_cm_loading = (int) (c - g_cm);
    g_sdk_in_init = 1;
    int rc = m->init(&g_cm_core);
    g_sdk_in_init = 0;
    g_cm_loading = -1;
    unlink(CM_INITING);
    if (rc) {
        c->m = NULL;
        c->screen = NULL;   /* stays mapped (see cm_load_one); nothing of it is called again */
        cm_refuse(c, AZCM_FAILED, "Its init() returned %d%s", rc, c->nhooks ? " (after patching code; reload to clear)" : "");
        return -1;
    }
    c->reason[0] = 0;
    cm_set_state(c, AZCM_LOADED);
    __atomic_add_fetch(&g_cm_live, 1, __ATOMIC_RELAXED);
    dbg("community: %s %s LOADED (%d hook%s)\n", c->id, c->version, c->nhooks, c->nhooks == 1 ? "" : "s");
    cm_logf(c, 1, "loaded %s on %s (%d hooks)", c->version, g_cm_fwtag, c->nhooks);
    return 0;
}
static void cm_load_one(cm_mod *c) {
    char dir[160], p[200];
    snprintf(dir, sizeof dir, CM_STAGE "/%s", c->id);
    snprintf(p, sizeof p, "%s/%s.off", dir, c->id);
    if (access(p, F_OK) == 0) {
        cm_refuse(c, AZCM_DISABLED, "Disabled");
        return;
    }
    /* manifest */
    snprintf(p, sizeof p, "%s/azmod.json", dir);
    char js[16384];
    int jn = 0;
    {
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            jn = (int) read(fd, js, sizeof js - 1);
            close(fd);
        }
        if (jn <= 0) {
            cm_refuse(c, AZCM_REFUSED, "Missing azmod.json");
            return;
        }
        js[jn] = 0;
    }
    {
        const char *why = NULL;
        if (!cm_json_walk(js, NULL, &why)) {
            cm_refuse(c, AZCM_REFUSED, "%s", why ? why : "azmod.json is not valid");
            return;
        }
    }
    char mid[40];
    if (!cm_json_str(js, "id", mid, sizeof mid) || strcmp(mid, c->id)) {
        cm_refuse(c, AZCM_REFUSED, "azmod.json is for a different mod id");
        return;
    }
    cm_json_str(js, "name", c->name, sizeof c->name);
    cm_json_str(js, "version", c->version, sizeof c->version);
    cm_json_str(js, "author", c->author, sizeof c->author);
    cm_json_str(js, "summary", c->summary, sizeof c->summary);
    if (!c->name[0]) snprintf(c->name, sizeof c->name, "%s", c->id);
    /* whose settings and suspension these are. The owner stamp is only read here: nothing on p7
     * changes for a mod that is not approved (cm_owner_p7 runs after the approval check), so a
     * stick carrying another author's mod under an installed id sets nothing aside. A suspension
     * left by another owner's copy is that copy's, not this one's. */
    {
        char op[200];
        snprintf(op, sizeof op, "%s/.origin", dir);
        cm_owner_key(op, c->owner, sizeof c->owner);
    }
    int other_owner;
    {
        char sp[200], cur[320];
        snprintf(sp, sizeof sp, CM_STATE "/%s.owner", c->id);
        cm_read_line(sp, cur, sizeof cur);
        other_owner = cur[0] && strcmp(cur, c->owner);
    }
    /* crash-suspended: checked after the manifest so the menu shows the mod's real name, and
     * before anything of its code (ELF read, dlopen) is touched */
    /* clang-format off */
    { char cp[160]; snprintf(cp, sizeof cp, CM_STATE "/%s.crashed", c->id);
      int fd = other_owner ? -1 : open(cp, O_RDONLY | O_CLOEXEC);
      if(fd >= 0){
          char why[8] = { 0 }; if(read(fd, why, sizeof why - 1) < 0){ why[0] = 0; } close(fd);
          cm_refuse(c, AZCM_SUSPENDED, !strncmp(why, "froze", 5)
                    ? "Suspended: the deck stopped while this mod was starting" : "Suspended: it crashed the deck");
          return; } }
    /* clang-format on */
    int api = cm_json_int(js, "api", -1);
    if (api != AZ_MOD_ABI) {
        cm_refuse(c, AZCM_REFUSED, "Made for mod API %d; this AZ-MODS has %d", api, AZ_MOD_ABI);
        return;
    }
    {
        char mc[24];
        if (cm_json_str(js, "min_core", mc, sizeof mc)) {
            int r = cm_vercmp(mc, AZ_CORE_VERSION);
            if (r == -2) {
                cm_refuse(c, AZCM_REFUSED, "azmod.json min_core is not MAJOR.MINOR.PATCH");
                return;
            }
            if (r > 0) {
                cm_refuse(c, AZCM_REFUSED, "Needs AZ-MODS %s or newer", mc);
                return;
            }
        }
    }
    char src[8];
    if (!cm_json_str(js, "source", src, sizeof src)) {
        cm_refuse(c, AZCM_REFUSED, "No source link in azmod.json");
        return;
    }
    int fw = cm_json_fw(js, g_cm_fwtag);
    if (fw < 0) {
        cm_refuse(c, AZCM_REFUSED, "azmod.json has no readable firmware list");
        return;
    }
    if (fw == 0) {
        cm_refuse(c, AZCM_REFUSED, g_cm_fwtag[0] ? "Not made for this firmware" : "This deck's firmware could not be identified");
        return;
    }
    c->any = (fw == 2);
    int hooks = cm_json_bool(js, "hooks", 0);
    if (c->any && hooks) {
        cm_refuse(c, AZCM_REFUSED, "Says \"any\" firmware but also patches code");
        return;
    }
    c->hooks_ok = hooks && !c->any;
    if (g_cm_live_try && hooks) return;   /* turned on live: a mod that patches code waits for a reload */
    /* ELF */
    snprintf(p, sizeof p, "%s/%s.so", dir, c->id);
    char why[112];
    if (cm_elf_check(p, why, sizeof why, c->sha)) {
        cm_refuse(c, AZCM_REFUSED, "%s", why);
        return;
    }
    /* approved: this id with exactly these bytes, on the deck's own list. Last check before any of
     * the mod's code runs, so Allow is offered only to a mod that would otherwise load. */
    if (!cm_is_approved(c->id, c->sha)) {
        dbg("community: %s is not approved with sha256 %s\n", c->id, c->sha);
        cm_refuse(c, AZCM_NEW, "New mod found: allow?");
        return;
    }
    cm_owner_p7(c);   /* approved: settle the owner's state before its settings are read */
    cm_cfg_load(c);   /* its values, from the settings file */
    /* "this mod is starting": on p7 before any of its code runs (dlopen runs its constructors),
     * removed once init() returns. If EP147 freezes or dies in between (a freeze sends no signal)
     * the marker survives and the next start suspends exactly this mod. */
    cm_mark_initing(c);
    /* load: RTLD_LOCAL, so nothing it exports reaches the process's symbol scope. Once opened, a
     * library is never closed: a constructor or an init() may have started a thread, or left a
     * pointer to its code with the deck, so unmapping it could crash EP147 later. */
    dlerror();
    void *h = dlopen(p, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        unlink(CM_INITING);
        const char *e = dlerror();
        size_t pl = strlen(p); /* dlerror names the file first: the reason is what follows it */
        if (e && !strncmp(e, p, pl) && !strncmp(e + pl, ": ", 2)) e += pl + 2;
        cm_refuse(c, AZCM_REFUSED, "Could not load: %.80s", e ? e : "?");
        return;
    }
    c->h = h;
    {
        struct link_map *lm = NULL;
        if (dlinfo(h, RTLD_DI_LINKMAP, &lm) == 0 && lm) {
            c->lo = lm->l_addr;
            c->hi = 0;
        }
        cm_range_of(c);
    }
    const az_mod_v1 *(*entry)(void) = (const az_mod_v1 *(*) (void) ) dlsym(h, "az_mod_v1_entry");
    const az_mod_v1 *m = entry ? entry() : NULL;
    const char *bad = NULL;
    int tag_ok = 0, tag_any = 0;
    if (!m) bad = "az_mod_v1_entry returned nothing";
    else if (m->abi != AZ_MOD_ABI || m->size < (int) (offsetof(az_mod_v1, on_frame) + sizeof m->on_frame)) bad = "Built for a different mod API";
    else if (!m->id || strcmp(m->id, c->id)) bad = "The library's id does not match azmod.json";
    else {
        for (int i = 0; m->fw_tags && m->fw_tags[i] && i < 32; i++) {
            if (!strcmp(m->fw_tags[i], "any")) tag_any = 1;
            else if (g_cm_fwtag[0] && !strcmp(m->fw_tags[i], g_cm_fwtag)) tag_ok = 1;
        }
        if (!(tag_ok || (tag_any && c->any))) bad = "The library is not built for this firmware";
        else if (!m->init) bad = "The library has no init()";
    }
    if (bad) {
        unlink(CM_INITING);
        cm_refuse(c, AZCM_REFUSED, "%s", bad);
        return;
    }
    if (g_cm_live_try && !AZ_MOD_HAS_STOP(m)) {   /* turned on live: a mod without stop() waits for a reload */
        unlink(CM_INITING);
        return;
    }
    cm_run_init(c, m);
}

static void cm_write_status(void) {
    mkdir(CM_LOGDIR, 0755);
    FILE *f = fopen(CM_LOGDIR "/loaded.new", "we");
    if (!f) return;
    fprintf(f, "fw=%s\nsuspended=%d\noff=%d\n", g_cm_fwtag, g_cm_suspended, g_cm_alloff);
    for (int i = 0; i < g_cm_n; i++)
        fprintf(f, "mod=%s|%d|%s|%s\n", g_cm[i].id, g_cm[i].state, g_cm[i].version, g_cm[i].reason);
    if (fclose(f) == 0) rename(CM_LOGDIR "/loaded.new", CM_LOGDIR "/loaded");
}

/* Called once from the overlay thread (EP147 only), after load_config. */
static void community_init(void) {
    cm_derive_fwtag();
    g_cm_logs_on = access(CM_ROOT "/azmod-logs.on", F_OK) == 0;
    dbg("community: running EP147 tag %s\n", g_cm_fwtag[0] ? g_cm_fwtag : "(not derivable: hooking mods refused)");
    g_cm_alloff = access(CM_ALL_OFF, F_OK) == 0 || access(CM_USER_OFF, F_OK) == 0;
    FILE *ef = fopen(CM_STAGE "/enabled", "re");
    if (ef) {
        char line[64];
        while (fgets(line, sizeof line, ef) && g_cm_n < CM_MAX) {
            line[strcspn(line, " \t\r\n")] = 0;
            if (!cm_id_ok(line) || cm_find(line)) continue;
            cm_mod *c = &g_cm[g_cm_n++];
            memset(c, 0, sizeof *c);
            snprintf(c->id, sizeof c->id, "%s", line);
            snprintf(c->name, sizeof c->name, "%s", line);
        }
        fclose(ef);
    }
    /* A disabled plugin is out of `enabled` but still installed (<id>/<id>.off):
     * list it too, so the Mod Menu shows it as off and can offer Enable. cm_load_one
     * refuses it on the .off file before it reads anything else, so it never loads. */
    int nen = g_cm_n;
    {
        DIR *dd = opendir(CM_STAGE);
        struct dirent *de;
        while (dd && (de = readdir(dd)) && g_cm_n < CM_MAX) {
            const char *id = de->d_name;
            size_t L = strlen(id);
            int ok = L >= 3 && L <= 32 && cm_id_ok(id);
            if (!ok || cm_find(id)) continue;
            char p[200];
            if (snprintf(p, sizeof p, CM_STAGE "/%s/%s.off", id, id) >= (int) sizeof p) continue;
            if (access(p, F_OK) != 0) continue;
            cm_mod *c = &g_cm[g_cm_n++];
            memset(c, 0, sizeof *c);
            _Static_assert(sizeof c->id > 32 && sizeof c->name > 32, "an id of L <= 32 fits");
            memcpy(c->id, id, L + 1);
            memcpy(c->name, id, L + 1);
          /* its name for the menu, best effort: the manifest is data, nothing of it runs */
            if (snprintf(p, sizeof p, CM_STAGE "/%s/azmod.json", id) < (int) sizeof p) {
                char js[16384], mid[40];
                int jn = 0, fd = open(p, O_RDONLY | O_CLOEXEC);
                if (fd >= 0) {
                    jn = (int) read(fd, js, sizeof js - 1);
                    close(fd);
                }
                if (jn > 0) {
                    js[jn] = 0;
                    if (cm_json_str(js, "id", mid, sizeof mid) && !strcmp(mid, id)) {
                        cm_json_str(js, "name", c->name, sizeof c->name);
                        cm_json_str(js, "version", c->version, sizeof c->version);
                        cm_json_str(js, "author", c->author, sizeof c->author);
                        cm_json_str(js, "summary", c->summary, sizeof c->summary);
                        if (!c->name[0]) memcpy(c->name, id, L + 1);
                    }
                }
            }
            cm_refuse(c, AZCM_DISABLED, "Disabled");
        }
        if (dd) closedir(dd);
    }
    /* from here on only the enabled ones (the first nen) take part: a disabled plugin
     * neither counts toward the quarantine nor gets relabeled by it */
    cm_grandfather(nen);
    cm_approved_read();
    if (!nen) {
        cm_write_status();
        return;
    }
    if (g_cm_alloff) {
        for (int i = 0; i < nen; i++) {
            cm_names(&g_cm[i]);
            cm_refuse(&g_cm[i], AZCM_DISABLED, "Community mods are disabled");
        }
        cm_write_status();
        return;
    }
    /* a mod that froze or died inside its own init() last start (CM_INITING left behind) */
    {
        char who[40] = {0};
        int fd = open(CM_INITING, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t k = read(fd, who, sizeof who - 1);
            close(fd);
            if (k > 0) who[k] = 0;
        }
        cm_mod *b = who[0] ? cm_find(who) : NULL;
        if (b) {
            mkdir(CM_STATE, 0755);
            char p[200];
            snprintf(p, sizeof p, CM_STATE "/%s.crashed", b->id);
            int fd2 = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd2 >= 0) {
                if (write(fd2, "froze", 5) < 0) {}
                close(fd2);
            }
            dbg("community: %s never finished starting last time: suspended\n", b->id);
            unlink(CM_STARTING);
        }
        unlink(CM_INITING);
    }
    /* crash blame from the last start: suspend only that mod */
    {
        char who[40] = {0};
        int fd = open(CM_BLAME, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t k = read(fd, who, sizeof who - 1);
            close(fd);
            if (k > 0) who[k] = 0;
        }
        cm_mod *b = who[0] ? cm_find(who) : NULL;
        if (b) {
          /* in CM_STATE, never in the stage: az-usbload re-mirrors the stick over the stage */
            mkdir(CM_STATE, 0755);
            char p[200];
            snprintf(p, sizeof p, CM_STATE "/%s.crashed", b->id);
            int fd2 = open(p, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
            if (fd2 >= 0) close(fd2);
            dbg("community: %s crashed the deck last start: suspended\n", b->id);
            unlink(CM_STARTING);   /* the culprit is known: no reason to hold the others back */
        }
        unlink(CM_BLAME);
    }
    /* quarantine */
    int starts = 0;
    {
        FILE *sf = fopen(CM_STARTING, "re");
        if (sf) {
            if (fscanf(sf, "%d", &starts) != 1) starts = 0;
            fclose(sf);
        }
    }
    if (starts >= 2) {
        g_cm_suspended = 1;
        for (int i = 0; i < nen; i++) cm_refuse(&g_cm[i], AZCM_SUSPENDED, "Suspended: the deck restarted twice right after starting with community mods");
        cm_write_status();
        return;
    }
    /* crashes nobody could be blamed for, with community mods loaded: CM_CRASH_N within
     * CM_CRASH_S hold them all back until the DJ resumes them (a mod that crashes the deck later
     * in a set, through code blame cannot see, must not do it every set). Old entries are dropped. */
    {
        long now_s = (long) time(NULL), t, keep[16];
        int nk = 0;
        FILE *cf = fopen(CM_CRASHES, "re");
        if (cf) {
            while (fscanf(cf, "%ld", &t) == 1)
                if (now_s - t >= 0 && now_s - t < CM_CRASH_S && nk < 16) keep[nk++] = t;
            fclose(cf);
        }
        if (nk >= CM_CRASH_N) {
            g_cm_suspended = 1;
            for (int i = 0; i < nen; i++) cm_refuse(&g_cm[i], AZCM_SUSPENDED, "Suspended: the deck crashed %d times in %d minutes with community mods on", nk, CM_CRASH_S / 60);
            cm_write_status();
            return;
        }
        if (cf) {
            FILE *w = fopen(CM_CRASHES ".new", "we");
            if (w) {
                for (int i = 0; i < nk; i++) fprintf(w, "%ld\n", keep[i]);
                if (fclose(w) == 0) rename(CM_CRASHES ".new", CM_CRASHES);
            }
        }
    }
    mkdir(CM_STATE, 0755);
    { /* must be on p7 before a mod's code runs (a crash in init must still count): fsync the
       * file and its folder only, not a global sync() that would also flush the USB sticks */
        int fd = open(CM_STARTING ".new", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0) {
            char b[16];
            int n = snprintf(b, sizeof b, "%d\n", starts + 1);
            int ok = write(fd, b, (size_t) n) == n && fsync(fd) == 0;
            close(fd);
            if (ok) rename(CM_STARTING ".new", CM_STARTING);
            int dfd = open(CM_STATE, O_RDONLY | O_CLOEXEC);
            if (dfd >= 0) {
                fsync(dfd);
                close(dfd);
            }
        }
        if (cmo_install(CM_STARTING) != 0) dbg("community: no SIGTERM handler: an orderly stop counts as a failed start\n");
    }
    g_cm_t0 = now_ms();
    g_cm_timing = 1;
    sdk_timing_init();
    /* handoff asks the DJ through the core's own dialog (sdk_proc.c, sdk_ui.c) */
    __atomic_store_n(&g_sdk_confirm_core, azcm_confirm, __ATOMIC_RELEASE);
    for (int i = 0; i < nen; i++) cm_load_one(&g_cm[i]);
    if (cm_nlive()) cm_install_blame();
    dbg("community: %d of %d enabled mod(s) loaded\n", cm_nlive(), g_cm_n);
    cm_write_status();
}

/* Every overlay tick. Cheap with no live mod. */
static void sdk_confirm_tick(void);
static void community_tick(void) {
    sdk_confirm_tick(); /* the dialog: also the core's own questions, with no mod loaded */
    if (!g_cm_n) return;
    sdk_rows_tick();    /* rows of mods that stopped leave the menu, also when none runs */
    long now = now_ms();
    cmo_note(__atomic_load_n(&g_rtick_n, __ATOMIC_RELAXED), now); /* EP147's message thread is alive */
    if (g_cm_timing && !g_cm_healthy && now - g_cm_t0 >= CM_HEALTHY_MS) {
        g_cm_healthy = 1;
        unlink(CM_STARTING);
        dbg("community: %d s healthy, quarantine counter cleared\n", CM_HEALTHY_MS / 1000);
    }
    static long last_tick = 0, last_slow = 0, last_flush = 0;
    if (now - last_slow >= 1000) {
        last_slow = now;
        g_cm_logs_on = access(CM_ROOT "/azmod-logs.on", F_OK) == 0;
    }
    /* settings reach p7 within ~250 ms of a change after a quiet spell (a tap on a mod's page
     * survives a restart right after it), but a mod that keeps changing a value is written at
     * most once every CM_CFG_BUSY_MS: p7 is small flash, not a place for a write every tick. */
    if (now - last_flush >= 250) {
        last_flush = now;
        pthread_mutex_lock(&g_cm_mu);
        for (int i = 0; i < g_cm_n; i++) {
            cm_mod *c = &g_cm[i];
            if (c->cfg_dirty && (!c->cfg_flushed_at || now - c->cfg_flushed_at >= CM_CFG_BUSY_MS)) {
                cm_cfg_flush(c);
                c->cfg_flushed_at = now ? now : 1;
            }
        }
        pthread_mutex_unlock(&g_cm_mu);
        cm_conf_save();
    }
    if (!cm_nlive()) return;
    if (now - last_tick < 33) return;
    last_tick = now;
    {
        static long last_bpm = 0;
        if (now - last_bpm >= 250) {
            last_bpm = now;
            for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
                double v = 0.0;
                g_cm_bpm[d] = (bpmstate_read(d, &v, NULL) && v > 20.0 && v < 400.0) ? v : 0.0;
            }
        }
    }
    for (int i = 0; i < g_cm_n; i++) {   /* on_frame's queued log lines and refusals, written here */
        cm_mod *c = &g_cm[i];
        for (int k = 0; k < 8; k++)
            if (__atomic_load_n(&c->fq.ready[k], __ATOMIC_ACQUIRE)) {
                cm_logf(c, 0, "%s", c->fq.line[k]);
                __atomic_store_n(&c->fq.ready[k], 0, __ATOMIC_RELEASE);
            }
        unsigned dr = __atomic_exchange_n(&c->fq.dropped, 0, __ATOMIC_RELAXED);
        if (dr) cm_logf(c, 0, "(%u log lines from on_frame dropped: log less there)", dr);
        unsigned rf = __atomic_exchange_n(&c->fq.refused, 0, __ATOMIC_RELAXED);
        if (rf) cm_logf(c, 1, "%u calls to settings or file services from on_frame were refused (allowed in tick, the page callbacks and your own threads)", rf);
    }
    sdk_deck_tick(now);                          /* deck events, before the mods' ticks */
    sdk_ui_tick();                               /* the mods' post_ui callbacks */
    for (int i = 0; i < g_cm_n; i++)
        if (g_cm[i].state == AZCM_LOADED && g_cm[i].m && g_cm[i].m->tick) g_cm[i].m->tick();
}

/* The ErP LED frame, before core's single CRC recompute. Returns 1 if a mod ate it. */
static int community_frame(int bus, unsigned char *f, int len) {
    if (!cm_nlive()) return 0;
    for (int i = 0; i < g_cm_n; i++) {
        cm_mod *c = &g_cm[i];
        if (!cm_enter(c)) continue;
        const az_mod_v1 *m = c->m;
        int r = 0;
        if (m && m->on_frame) {
            g_cm_in_frame = 1;
            r = m->on_frame(bus, f, len);
            g_cm_in_frame = 0;
        }
        cm_leave(c);
        if (r == AZ_FRAME_EATEN) return 1;
    }
    return 0;
}
static int community_redraw_wanted(void) { return __atomic_exchange_n(&g_cm_redraw, 0, __ATOMIC_RELAXED); }

/* ---- for the Mod Menu (menu.c; declared in menu.h) ---------------------------------- */
int azcm_count(void) { return g_cm_n; }
int azcm_suspended(void) { return g_cm_suspended; }
int azcm_all_off(void) { return g_cm_alloff; }
int azcm_off_by_stick(void) { return access(CM_ALL_OFF, F_OK) == 0; }
const char *azcm_fwtag(void) { return g_cm_fwtag; }
static const az_screen_v1 *cm_scr(int i); /* below: the screen a mod's page shows */
int azcm_info(int i, azcm_info_t *out) {
    if (i < 0 || i >= g_cm_n || !out) return -1;
    const cm_mod *c = &g_cm[i];
    snprintf(out->id, sizeof out->id, "%s", c->id);
    snprintf(out->name, sizeof out->name, "%s", c->name);
    snprintf(out->version, sizeof out->version, "%s", c->version);
    snprintf(out->author, sizeof out->author, "%s", c->author);
    snprintf(out->summary, sizeof out->summary, "%s", c->summary);
    snprintf(out->reason, sizeof out->reason, "%s", c->reason);
    out->state = c->state;
    out->has_screen = cm_scr(i) != NULL;
    out->hooks = c->nhooks;
    out->allowed = c->allowed;
    /* where it came from, short: owner/repo for a GitHub repository, else the address without
     * https://; "" for a copy without an install record */
    out->from[0] = 0;
    if (c->owner[0] && strcmp(c->owner, "local")) {
        const char *o = c->owner, *gh = "https://raw.githubusercontent.com/";
        if (!strncmp(o, gh, strlen(gh))) {
            o += strlen(gh);
            const char *e = strchr(o, '/');
            e = e ? strchr(e + 1, '/') : NULL;
            snprintf(out->from, sizeof out->from, "%.*s", e ? (int) (e - o) : (int) strlen(o), o);
        } else snprintf(out->from, sizeof out->from, "%s", strncmp(o, "https://", 8) ? o : o + 8);
    }
    return 0;
}
/* Allow on a "New mod found" page: approve exactly the .so this start hashed. The stage is
 * re-mirrored from the stick at the reload; different bytes there ask again. */
int azcm_allow(int i) {
    if (i < 0 || i >= g_cm_n) return -1;
    cm_mod *c = &g_cm[i];
    if (c->state != AZCM_NEW || !cm_sha_ok(c->sha)) return -1;
    if (cm_approve(c->id, c->sha)) return -1;
    c->allowed = 1;
    dbg("community: %s allowed by the DJ: %s\n", c->id, c->sha);
    cm_logf(c, 1, "allowed on the deck: sha256 %s (loads after Reload)", c->sha);
    return 0;
}
int azcm_new_count(void) {
    int n = 0;
    for (int i = 0; i < g_cm_n; i++)
        if (g_cm[i].state == AZCM_NEW && !g_cm[i].allowed) n++;
    return n;
}
/* The screen a mod's page shows: the screen of the SCREEN row that opened it (azcm_page_screen),
 * else the mod's register_screen page. NULL: neither, or the mod is not running. */
static const az_screen_v1 *g_cm_page_scr;
static int g_cm_page_scr_mod = -1;
static const az_screen_v1 *cm_scr(int i) {
    if (i < 0 || i >= g_cm_n || g_cm[i].state != AZCM_LOADED) return NULL;
    if (i == g_cm_page_scr_mod && g_cm_page_scr) return g_cm_page_scr;
    return g_cm[i].screen;
}
void azcm_page_screen(int i, const az_screen_v1 *scr) {
    g_cm_page_scr = scr && i >= 0 && i < g_cm_n ? scr : NULL;
    g_cm_page_scr_mod = g_cm_page_scr ? i : -1;
}
/* 1 = a mod's SCREEN row has something to open: its own screen, or its mod's page */
int azcm_row_target(const char *owner, const az_screen_v1 *row_scr) {
    for (int i = 0; owner && i < g_cm_n; i++)
        if (!strcmp(g_cm[i].id, owner)) return g_cm[i].state == AZCM_LOADED && (row_scr || g_cm[i].screen);
    return 0;
}
void azcm_screen_draw(int i, az_draw *d, az_rect area) {
    const az_screen_v1 *sc = cm_scr(i);
    if (!sc) return;
    cm_paint_begin(d);
    sc->on_draw(d, area, sc->user);
    cm_paint_end();
}
int azcm_screen_input(int i, const az_event *ev) {
    const az_screen_v1 *sc = cm_scr(i);
    if (!sc || !sc->on_input) return 0;
    return sc->on_input(ev, sc->user);
}
void azcm_screen_enter(int i) {
    const az_screen_v1 *sc = cm_scr(i);
    if (!sc || !sc->on_enter) return;
    sc->on_enter(sc->user);
}
/* "Resume" from the quarantine banner: the next start loads the enabled mods again. */
void azcm_resume(void) {
    unlink(CM_STARTING);
    unlink(CM_CRASHES);
}
/* The DJ asked for a reload from the Mod Menu. Settings are written first; and since this start
 * was alive enough to be used, it is not one of the "restarted right after starting" the
 * quarantine counts (two quick installs, each with Reload now, would otherwise hold every
 * community mod back). While quarantined it changes nothing: only Resume lifts that. */
void azcm_reload_requested(void) {
    azcm_flush_settings();
    if (!g_cm_suspended) unlink(CM_STARTING);
}
/* write every pending setting now (before a reload, or when the DJ leaves a mod's page) */
void azcm_flush_settings(void) {
    pthread_mutex_lock(&g_cm_mu);
    for (int i = 0; i < g_cm_n; i++)
        if (g_cm[i].cfg_dirty) {
            cm_cfg_flush(&g_cm[i]);
            g_cm[i].cfg_flushed_at = now_ms();
        }
    pthread_mutex_unlock(&g_cm_mu);
    cm_conf_save();
}
/* 1 = `a` is a newer MAJOR.MINOR.PATCH than `b`. Either one malformed: 0 (never offer it). */
int azcm_ver_newer(const char *a, const char *b) { return a && b && cm_vercmp(a, b) == 1; }
int azcm_min_core_ok(const char *mc) { return !mc || !mc[0] || cm_vercmp(mc, AZ_CORE_VERSION) == 0 || cm_vercmp(mc, AZ_CORE_VERSION) == -1; }
/* Re-enable one mod that crashed the deck (it loads at the next start). */
void azcm_unsuspend(const char *id) {
    if (!id || !cm_find(id)) return;
    char p[160];
    snprintf(p, sizeof p, CM_STATE "/%s.crashed", id);
    unlink(p);
}
/* The Mod Menu's master switch for community mods (applies at the next start). */
void azcm_set_all_off(int off) {
    mkdir(CM_STATE, 0755);
    if (off) {
        int fd = open(CM_USER_OFF, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        if (fd >= 0) close(fd);
    } else unlink(CM_USER_OFF);
}

/* ---- turning a mod on or off while the deck runs (az_mod_v1.stop) ----------------------- */
#define CM_LIVE_DRAIN_MS 500    /* calls already inside the mod on other threads, at a live stop */
#define CM_LIVE_THREADS_MS 1000   /* threads it started through thread_start, after its stop() */

/* The stage as az-usbload writes it from azmods.cfg (cm_ctl): <id>/<id>.off for a mod turned off,
 * the sorted `enabled` list of those turned on. Written now, so a restart without a re-mirror
 * starts what the DJ just chose. */
static int cm_id_cmp(const void *a, const void *b) { return strcmp((const char *) a, (const char *) b); }
static void cm_stage_mirror(const char *id, int on) {
    char p[200];
    snprintf(p, sizeof p, CM_STAGE "/%s", id);
    if (access(p, F_OK) != 0) return;
    snprintf(p, sizeof p, CM_STAGE "/%s/%s.off", id, id);
    if (on) unlink(p);
    else {
        int fd = open(p, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        if (fd >= 0) close(fd);
    }
    char ids[CM_MAX * 2][33];
    int n = 0;
    FILE *f = fopen(CM_STAGE "/enabled", "re");
    if (f) {
        char line[64];
        while (n < CM_MAX * 2 - 1 && fgets(line, sizeof line, f)) {
            line[strcspn(line, " \t\r\n")] = 0;
            if (cm_id_ok(line) && strcmp(line, id)) snprintf(ids[n++], sizeof ids[0], "%s", line);
        }
        fclose(f);
    }
    if (on) snprintf(ids[n++], sizeof ids[0], "%s", id);
    qsort(ids, (size_t) n, sizeof ids[0], cm_id_cmp);
    FILE *w = fopen(CM_STAGE "/enabled.new", "we");
    if (!w) return;
    for (int i = 0; i < n; i++) fprintf(w, "%s\n", ids[i]);
    if (fclose(w) == 0) rename(CM_STAGE "/enabled.new", CM_STAGE "/enabled");
    else unlink(CM_STAGE "/enabled.new");
}

/* 1 = mod c, running, can be stopped now: it has stop() and used nothing that lasts until the
 * next start */
static int cm_live_stoppable(const cm_mod *c) {
    return cm_running(c) && c->m && AZ_MOD_HAS_STOP(c->m) && !c->hooks_ok && !c->nhooks &&
           !__atomic_load_n(&c->sticky, __ATOMIC_RELAXED);
}
static void cm_wait_ms(int ms) {
    struct timespec ts = {0, ms * 1000000L};
    nanosleep(&ts, NULL);
}
static int cm_live_off(cm_mod *c) {
    if (!cm_live_stoppable(c)) return 0;
    int idx = (int) (c - g_cm);
    cm_set_state(c, AZCM_DISABLED);   /* from here on nothing calls into it (cm_enter, cm_running) */
    __atomic_sub_fetch(&g_cm_live, 1, __ATOMIC_RELAXED);
    snprintf(c->reason, sizeof c->reason, "Disabled");
    int k = 0;
    while (__atomic_load_n(&c->inflight, __ATOMIC_SEQ_CST) && k++ < CM_LIVE_DRAIN_MS) cm_wait_ms(1);
    if (__atomic_load_n(&c->inflight, __ATOMIC_SEQ_CST)) {
        /* a callback on another thread has not returned: stop() could free what it uses */
        c->dirty = 1;
        cm_logf(c, 1, "turned off; a callback did not return within %d ms, so stop() was not called (on again after a reload)", CM_LIVE_DRAIN_MS);
    } else {
        cm_mark_initing(c);   /* a stop() that freezes or crashes EP147 suspends this mod, as init() would */
        g_cm_loading = idx;   /* its own settings and log stay usable inside stop() */
        c->m->stop();
        g_cm_loading = -1;
        unlink(CM_INITING);
    }
    c->screen = NULL;
    sdk_rows_forget(c);
    sdk_input_forget(c);
    sdk_deck_forget(idx);
    int np = sdk_proc_forget(c->id);
    if (np) cm_logf(c, 1, "stopped %d program%s it launched", np, np == 1 ? "" : "s");
    for (k = 0; sd_threads_of(idx) && k < CM_LIVE_THREADS_MS / 10; k++) cm_wait_ms(10);
    if (sd_threads_of(idx)) {
        c->dirty = 1;
        cm_logf(c, 1, "turned off, but %d of its threads still run (stop() must end them): on again after a reload", sd_threads_of(idx));
    }
    azcm_flush_settings();
    cm_logf(c, 1, "turned off without a reload");
    cm_write_status();
    return 1;
}
static int cm_live_on(cm_mod *c) {
    if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) != AZCM_DISABLED || g_cm_suspended || g_cm_alloff || c->dirty)
        return 0;
    if (c->m) {   /* stopped live earlier this start: init() again, if the code is still the one checked */
        char p[200], why[112], sha[65] = {0};
        snprintf(p, sizeof p, CM_STAGE "/%s/%s.so", c->id, c->id);
        if (cm_elf_check(p, why, sizeof why, sha) || strcmp(sha, c->sha) || !cm_is_approved(c->id, c->sha)) return 0;
        cm_mark_initing(c);
        cm_run_init(c, c->m);
    } else {
        if (c->h) return 0;   /* opened before without stop(): a reload starts it */
        g_cm_live_try = 1;
        cm_load_one(c);
        g_cm_live_try = 0;
        if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) == AZCM_DISABLED) return 0;   /* waits for a reload */
    }
    if (cm_nlive()) cm_install_blame();
    cm_write_status();
    return 1;
}
/* The DJ turned mod `id` on or off (the stick's azmods.cfg already says so). UI thread. 1 = done
 * now (the Mod Menu shows the result: running, disabled, or why it did not load); 0 = it takes
 * effect after a reload. */
int azcm_live_set(const char *id, int on) {
    cm_mod *c = id ? cm_find(id) : NULL;
    if (!c) return 0;
    cm_stage_mirror(c->id, on);
    return on ? cm_live_on(c) : cm_live_off(c);
}
