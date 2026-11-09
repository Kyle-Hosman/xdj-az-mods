/* SPDX-License-Identifier: GPL-3.0-only */
/* Test harness for community.c: the real host, compiled with stubs for the azmod internals it
 * borrows, loading real test mods (mods.c) from a temp stage. Run by run.sh in the build
 * container. Usage: harness <root> init|tick <ms>|tag|claim <id> <addr-kind>|starting */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include "menu.h"

#define AZMOD_STEM_DECKS 4
#define BPMSTATE_LIVE 0xa0
static void dbg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("dbg: ", stdout);
    vprintf(fmt, ap);
    va_end(ap);
}
static _Atomic long g_fake_now = 0; /* advanced by main, read by the mods' threads */
static long now_ms(void) { return g_fake_now; }
static int g_four_deck = 0;
static int g_cr_fd = -1; /* crash reporter not armed: community installs its own blame handler */
static int (*g_deck_has_track_fn)(int) = NULL;
static int bpmstate_read(int d, double *live, double *track) {
    (void) d;
    (void) live;
    (void) track;
    return 0;
}
static int deck_for_ds(long ds) { return ds == 0x1000 ? 2 : -1; }
static int deck_for_modeobj(long p) {
    (void) p;
    return -1;
}
#define AZM_HOOKED_MAX 512
enum {
    AZM_REFUSED_LATE = -3
};
static struct {
    uintptr_t addr;
    const char *who;
} g_hooked[AZM_HOOKED_MAX];
static int g_hooked_n = 0;
static const char *g_hook_owner = "AZ-MODS";
static int azm_install_hook(uintptr_t a, void *h, void **o) {
    (void) h;
    *o = (void *) a;
    g_hooked[g_hooked_n].addr = a;
    g_hooked[g_hooked_n].who = g_hook_owner;
    g_hooked_n++;
    return 1;
}
/* menu.c draw helpers: not exercised here */
static int g_headers; /* draw_header calls that reached the drawing code (the paint test) */
void azcm_draw_header(az_draw *d, const char *t) {
    (void) d;
    (void) t;
    g_headers++;
}
void azcm_draw_text(az_draw *d, int x, int y, const char *s, int px, uint32_t c) {
    (void) d;
    (void) x;
    (void) y;
    (void) s;
    (void) px;
    (void) c;
}
void azcm_fill_rect(az_draw *d, int x, int y, int w, int h, uint32_t c) {
    (void) d;
    (void) x;
    (void) y;
    (void) w;
    (void) h;
    (void) c;
}
void azcm_draw_rect(az_draw *d, int x, int y, int w, int h, int t, uint32_t c) {
    (void) d;
    (void) x;
    (void) y;
    (void) w;
    (void) h;
    (void) t;
    (void) c;
}
void azcm_draw_checkbox(az_draw *d, int x, int y, int s, int on, int hi) {
    (void) d;
    (void) x;
    (void) y;
    (void) s;
    (void) on;
    (void) hi;
}
uint32_t azcm_color(int r) { return (uint32_t) r; }
int azcm_row_height(az_draw *d) {
    (void) d;
    return 48;
}
int azcm_text_width(const char *s, int px) { return (int) strlen(s) * px / 2; }

/* ep_mem.h's fault-free read for the crash reporter's frame walk (the real one checks EP147's
 * address ranges first; here any mapped address will do) */
#include <sys/uio.h>
static int crash_peek8(uintptr_t a, uintptr_t *out) {
    struct iovec l = {out, 8}, r = {(void *) a, 8};
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == 8;
}
/* the settings file azmod.c owns (g_conf): the harness loads it at start, the flush saves it */
#include "../../azconf.c"
#define CONF_PATH CM_ROOT "/azmod.conf"
static struct azconf *g_conf;
static int g_conf_readonly;
static long g_rtick_n; /* pad_leds.c's repaint tick count (the quarantine's heartbeat): none here */
#define SDK_SURFACE_IMPL "test/community/sdk_surface_stub.c" /* no overlay thread here */
#define SDK_DECKUI_IMPL "test/community/sdk_deckui_stub.c"  /* no azstem, no EP147 here */
#define SDK_PROC_IMPL "test/community/sdk_proc_stub.c"      /* no audio, systemd or stick here */
#define SDK_ROWS_IMPL "test/community/sdk_rows_stub.c"      /* no feature table or settings here */
#include "../../community.c"
#include "../../sdk_input.c"   /* the frame and touch filters: plain logic, no EP147 needed */

/* azstem's exports as the Reload confirm sees them: the stems-following per-deck export says
 * "empty" (a stick without a STEMS tree), azstem_decks_loaded answers from LoadedTrackInfo */
static int g_fake_decks = -1;
int azstem_decks_loaded(void) { return g_fake_decks; }
/* azstem's per-deck snapshot as deck_loaded / deck_title read it: CM_TEST_DECKS gives each
 * deck's state ('L' loaded, 'E' empty, anything else unknown); a loaded deck's title is
 * "Title <n>". No azstem_deck_track_name here: a stick without a STEMS tree. */
int azstem_deck_snapshot(int deck, struct azstem_deck_snap *out) {
    const char *st = getenv("CM_TEST_DECKS");
    if (!out || out->size < (int) sizeof *out || deck < 0 || deck > 3)
        return -1;
    memset(out, 0, sizeof *out);
    out->size = (int) sizeof *out;
    char c = st && (int) strlen(st) > deck ? st[deck] : 'U';
    out->loaded = c == 'L' ? 1 : c == 'E' ? 0
                                          : -1;
    if (out->loaded == 1)
        snprintf(out->title, sizeof out->title, "%s%d", getenv("CM_TEST_TITLE") ? getenv("CM_TEST_TITLE") : "Title ", deck + 1);
    return 0;
}
static int stems_export_empty(int d) {
    (void) d;
    return 0;
}
/* kept out of main(): "tagmp" re-protects pages at fixed offsets from main, so main's size matters */
static int anyloaded_cmd(int argc, char **argv) {
    g_deck_has_track_fn = stems_export_empty;
    if (!strcmp(argv[1], "anyloaded") && argc > 2) {        /* the Reload confirm's question */
        g_fake_decks = atoi(argv[2]);
        printf("%d\n", azcm_any_loaded());
        return 0;
    }
    int any = 0;                                           /* anyloaded-old: the stems export alone */
    for (int d = 0; d < AZMOD_STEM_DECKS; d++)
        if (g_deck_has_track_fn(d)) any = 1;
    printf("%d\n", any);
    return 0;
}

/* "tagmp" mis-flags a code page (PROT_READ). A page taken from main's neighbors holds
 * whatever the link put there, so a change anywhere could put code on it (a SIGSEGV, read as
 * "tag moved"). It is a dedicated page: page-aligned, a page long (64 KB
 * covers every page size), never called. */
__attribute__((noinline, aligned(65536))) static void tagmp_victim(void) { __asm__ volatile(".skip 65536"); }

int az_live_probe[4]; /* the MOD_LIVE test mod's counters: inits, stops, calls outside init..stop, filter calls */
static int g_live_stress_run;
static long g_live_stress_frames;
static void *live_stress_bus(void *a) {   /* the bus thread: on_frame and the frame filters, non-stop */
    (void) a;
    while (__atomic_load_n(&g_live_stress_run, __ATOMIC_ACQUIRE)) {
        unsigned char fr[128] = {0};
        community_frame(AZ_BUS_ERP_LED, fr, 0x78);
        sdk_filter_run(AZ_BUS_ERP_LED, fr, 0x78);
        __atomic_add_fetch(&g_live_stress_frames, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage\n");
        return 2;
    }
    g_conf = azconf_new();
    if (g_conf) azconf_load(g_conf, CONF_PATH);
    if (!strcmp(argv[1], "vernewer") && argc > 3) {
        printf("%d\n", azcm_ver_newer(argv[2], argv[3]));
        return 0;
    }
    if (!strcmp(argv[1], "verdiffer") && argc > 3) {
        printf("%d\n", strcmp(argv[2], argv[3]) != 0);
        return 0;
    }   /* control: plain inequality */
    if (!strcmp(argv[1], "sha") && argc > 2) {
        char h[65];
        printf("%s\n", cm_sha256_file(argv[2], h) ? "ERR" : h);
        return 0;
    }
    if (!strcmp(argv[1], "shastr") && argc > 2) {
        char h[65];
        azsha256_hex(argv[2], strlen(argv[2]), h);
        printf("%s\n", h);
        return 0;
    }
    if (!strcmp(argv[1], "tag")) {
        cm_derive_fwtag();
        printf("%s\n", g_cm_fwtag);
        return 0;
    }
    if (!strncmp(argv[1], "anyloaded", 9)) return anyloaded_cmd(argc, argv);
    if (!strcmp(argv[1], "tagmp")) {                       /* the tag must not move when code pages are re-protected */
        cm_derive_fwtag();
        char a[32];
        snprintf(a, sizeof a, "%s", g_cm_fwtag);
        long pg = sysconf(_SC_PAGESIZE);
        uintptr_t p = ((uintptr_t) &main) & ~(uintptr_t) (pg - 1);
        mprotect((void *) p, (size_t) pg, PROT_READ | PROT_WRITE | PROT_EXEC);   /* what a hook install does */
        uintptr_t v = (uintptr_t) &tagmp_victim;                                   /* never runs: see its comment */
        mprotect((void *) v, (size_t) pg, PROT_READ | PROT_EXEC | PROT_WRITE);
        mprotect((void *) v, (size_t) pg, PROT_READ);                               /* and a mis-flagged page */
        g_cm_fwtag[0] = 0;
        g_cm_text_lo = g_cm_text_hi = 0;
        cm_derive_fwtag();
        printf("%s %s %s\n", a, g_cm_fwtag, strcmp(a, g_cm_fwtag) ? "MOVED" : "STABLE");
        return 0;
    }
    if (!strcmp(argv[1], "addrs")) {                       /* in-text addresses for the claim test */
        uintptr_t m = ((uintptr_t) &main + 64) & ~(uintptr_t) 3;
        printf("%lx %lx\n", (unsigned long) m, (unsigned long) (m + 64));
        return 0;
    }
    /* a core-owned hook inside our own text, so "already patched by AZ-MODS" can be tested */
    {
        uintptr_t m = ((uintptr_t) &main + 64) & ~(uintptr_t) 3;
        void *o;
        azm_install_hook(m + 64, NULL, &o);
    }
    community_init();
    for (int i = 0; i < azcm_count(); i++) {
        azcm_info_t in;
        azcm_info(i, &in);
        printf("MOD %s state=%d screen=%d hooks=%d name=%s| reason=%s\n", in.id, in.state, in.has_screen, in.hooks, in.name, in.reason);
        printf("NAME %s %s\n", in.id, in.name);
        printf("FROM %s %s\n", in.id, in.from[0] ? in.from : "-");
        if (g_cm[i].sha[0]) printf("SHA %s %s\n", in.id, g_cm[i].sha);
        for (int k = 0; k < 4; k++) {
            char kr[4] = {'r', (char) ('0' + k), 0, 0};
            pthread_mutex_lock(&g_cm_mu); /* as the services do: a mod's thread may be setting */
            int v = cm_cfg_get_u(in.id, kr, -7);
            pthread_mutex_unlock(&g_cm_mu);
            if (v != -7) printf("CLAIM %s %d %d\n", in.id, k, v);
        }
    }
    printf("SUSPENDED %d\nLIVE %d\nNEW %d\n", azcm_suspended(), g_cm_live, azcm_new_count());
    if (argc > 4 && !strcmp(argv[2], "allowx")) {         /* another writer (az-community) adds a line after this start read the list */
        FILE *f = fopen(CM_APPROVED, "a");
        if (f) {
            fprintf(f, "%s\n", argv[4]);
            fclose(f);
        }
        argv[2] = "allow";
    }
    if (argc > 3 && !strcmp(argv[2], "allow")) {          /* the DJ pressed Allow on a mod's page */
        int k = -1;
        for (int i = 0; i < azcm_count(); i++)
            if (!strcmp(g_cm[i].id, argv[3])) k = i;
        int rc = azcm_allow(k);
        azcm_info_t in;
        memset(&in, 0, sizeof in);
        if (k >= 0) azcm_info(k, &in);
        printf("ALLOW %d allowed=%d NEW %d\n", rc, in.allowed, azcm_new_count());
        return 0;
    }
    if (argc > 2 && !strcmp(argv[2], "reload")) {
        azcm_reload_requested();
        return 0;
    }   /* the DJ pressed Reload */
    if (argc > 2 && !strcmp(argv[2], "paint")) {          /* each page paint, then ticks: a kept context must draw nothing */
        int painted = 0;
        for (int i = 0; i < azcm_count(); i++) {
            azcm_info_t in;
            if (azcm_info(i, &in) || in.state != AZCM_LOADED || !in.has_screen) continue;
            az_draw d;
            memset(&d, 0, sizeof d);
            az_rect a = {0, 0, 100, 100};
            azcm_screen_draw(i, &d, a);
            painted++;
        }
        int in_paint = g_headers;
        for (g_fake_now = 1; g_fake_now <= 1000; g_fake_now += 10) community_tick();
        printf("PAINT painted=%d in_paint=%d after_ticks=%d\n", painted, in_paint, g_headers - in_paint);
        return 0;
    }
    if (argc > 4 && !strcmp(argv[2], "live")) {           /* the DJ turns mod argv[3] off/on: argv[4] = "off,on,..." */
        cm_mod *c = cm_find(argv[3]);
        char ops[64];
        snprintf(ops, sizeof ops, "%s", argv[4]);
        for (char *op = strtok(ops, ","); op; op = strtok(NULL, ",")) {
            int rc = azcm_live_set(argv[3], !strcmp(op, "on"));
            unsigned char fr[128] = {0};
            int ran = community_frame(AZ_BUS_ERP_LED, fr, 0x78);
            sdk_filter_run(AZ_BUS_ERP_LED, fr, 0x78);
            int *inits = &az_live_probe[0], *stops = &az_live_probe[1], *filt = &az_live_probe[3];
            char op_off[200];
            snprintf(op_off, sizeof op_off, CM_STAGE "/%s/%s.off", argv[3], argv[3]);
            char en[256] = {0};
            FILE *ef = fopen(CM_STAGE "/enabled", "re");
            if (ef) {
                size_t k = fread(en, 1, sizeof en - 1, ef);
                en[k] = 0;
                fclose(ef);
                for (char *q = en; *q; q++)
                    if (*q == '\n') *q = ' ';
            }
            printf("LIVESET %s rc=%d state=%d byte0=%d ran=%d inits=%d stops=%d filtered=%d filters=%d off=%d enabled=[%s] dirty=%d reason=%s\n", op, rc,
                   c ? c->state : -1, fr[0], ran, inits ? *inits : -1, stops ? *stops : -1, filt ? *filt : -1,
                   g_sdk_ff_n, access(op_off, F_OK) == 0, en, c ? c->dirty : -1, c ? c->reason : "");
        }
        return 0;
    }
    if (argc > 4 && !strcmp(argv[2], "livestress")) {     /* the bus thread runs frames while the DJ turns it off and on */
        cm_mod *c = cm_find(argv[3]);
        int n = atoi(argv[4]), bad = 0;
        pthread_t t;
        __atomic_store_n(&g_live_stress_run, 1, __ATOMIC_RELEASE);
        pthread_create(&t, NULL, live_stress_bus, NULL);
        for (int i = 0; i < n; i++) {
            if (azcm_live_set(argv[3], 0) != 1) bad++;
            if (azcm_live_set(argv[3], 1) != 1) bad++;
        }
        __atomic_store_n(&g_live_stress_run, 0, __ATOMIC_RELEASE);
        pthread_join(t, NULL);
        int *ov = &az_live_probe[2], *filt = &az_live_probe[3];
        printf("STRESS bad=%d overlap=%d filtered=%d frames=%ld filters=%d state=%d\n", bad, ov ? *ov : -1, filt ? *filt : -1,
               __atomic_load_n(&g_live_stress_frames, __ATOMIC_RELAXED), g_sdk_ff_n, c ? c->state : -1);
        return 0;
    }
    if (argc > 2 && !strcmp(argv[2], "selfcrash")) {      /* a fault in "EP147", not in a mod */
        volatile int *z = (volatile int *) 0;
        *z = 2;
        return 0;
    }
    if (argc > 2 && !strcmp(argv[2], "crash")) {          /* tick until the crash mod faults */
        for (g_fake_now = 1; g_fake_now <= 5000; g_fake_now += 10) community_tick();
        printf("NO CRASH\n");
        return 0;
    }
    if (argc > 2 && !strcmp(argv[2], "run")) {            /* simulate 95 s of ticks */
        for (g_fake_now = 1; g_fake_now <= 95000; g_fake_now += 10) community_tick();
        printf("CFGWRITES %ld\n", g_cm_cfg_writes);
        printf("STARTING_EXISTS %d\n", access(CM_STARTING, F_OK) == 0);
        unsigned char fr[128] = {0};
        printf("FRAME %d byte0=%d\n", community_frame(AZ_BUS_ERP_LED, fr, 0x78), fr[0]);
        g_fake_now += 100;
        community_tick();             /* the tick after: on_frame's queued log lines */
        g_fake_now += 6000;
        community_tick();            /* past the settings rate limit */
    }
    return 0;
}
