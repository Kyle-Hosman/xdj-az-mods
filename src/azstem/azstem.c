/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file azstem.c
 * @brief Per-deck stems and key-shift for EP147 (LD_PRELOAD).
 *
 * EP147 is non-PIE (base 0x400000), so its unexported audio functions sit at
 * fixed addresses and are inline-hooked at runtime. The main hooks are each
 * deck's processBlockInternal (per-deck render, playhead, levels) and
 * PageBuffer::read (the decoded-PCM source, where the stem sum is injected
 * below EP147's own tempo/key/scratch DSP).
 *
 * Realtime rule: hooks on the audio thread do no file I/O, allocation or
 * locking. They read and publish atomics; the flusher thread does all file
 * work and logging.
 *
 * The installer is fail-safe: a prologue holding a PC-relative instruction it
 * cannot relocate is declined, never patched.
 *
 * Build: make CC=gcc azstem, in the directory that holds the Makefile.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sched.h> /* sched_yield: seqlock readers, the LoadState destructor wait */
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/resource.h> /* setpriority: the stem decode threads */
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h> /* SYS_gettid: thread census (diagnostics) */

/* Vendored stb_vorbis v1.22 (public domain / MIT). Declarations only here so its
 * internal `L`/`R` macros stay out of this file; the implementation is its own
 * translation unit. Decoding never runs on the audio thread. */
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#include "ep147addr.h" /* EP_<v130> -> this build's EP147 address */
#include "ep147rate.h" /* AZ_EP147_RATE: the deck engine rate, and why it is fixed */
#include "../common/azlogcap.h" /* every /tmp append is size-capped */
#include "../common/azprot.h" /* hook writes: unlock, write, restore the original protection */
#include "../common/azaltstack.h" /* every thread we start has an alternate signal stack */

/* ---- Target (EP147 non-PIE, fixed addresses) ------------------------- *
 * 0x14ab0d0 = DjPlayerAudioProcessor::processBlockInternal (a slot of the
 * vtable at 0x289f4d8). It is shared by six units: four DjPlayerUnit decks and
 * two meow::AudioRecorderUnit, each rendering 64-frame stereo float blocks. */
#define ADDR_DEFAULT EP_14ab0d0                     /* processBlockInternal entry */
/* Hook-address overrides read from a settings file (hex) are a development aid for
 * finding addresses on a new firmware. The release build never reads them: every hook
 * address comes from the per-firmware table, so a stray file cannot redirect a hook.
 * Every other address or pointer azstem would take from a file goes through this too
 * (azstem.djpuvt, .pbself, .memdump), and the release check refuses an azstem.so that
 * still names any of those files. */
#define addr_override_open(path) ((FILE *) NULL)
/* Diagnostic and test inputs (observers, probes, the test tone, the drum test, the mute
 * mask, manual overrides): files nothing in a release writes. A release build never reads
 * them and takes the answer it gets when the file is absent, so a stray file cannot play a
 * test tone or mute a deck; the paths are not even in the release .so (the release
 * check confirms it). dev_flag(path) = "the file exists"; dev_input_open(path)
 * opens a value file. */
#define dev_flag(path) 0
#define dev_input_open(path) ((FILE *) NULL)
#define ADDR_PATH "/home/root/settings/azstem.addr" /* optional override (hex) */
/* The deck object reaching that hook is a dj_player::DjPlayerUnit (vptr
 * 0x289f878, by RTTI); its deck identity is described under "Audio deck identity".
 *
 * Native routing: pcmbuf::pagebuf::PageBuffer<DefaultPageStaticConfig>::read
 * @0x7820d0 is the decoded-PCM source read, below EP147's time_domain adapters
 * (time-stretch, key, tempo, scratch). Returning the stem sum here lets all of
 * those apply to stems natively. AArch64 signature:
 *   Position read(void* this=x0, Float2* dst=x1, const Position* pos=x2, Sample cnt=x3)
 * with the 32-byte Position returned via x8. Float2 = {float L, R}. */
#define PBREAD_DEFAULT EP_7820d0
#define PBREAD_PATH "/home/root/settings/azstem.pbaddr"
static uintptr_t g_func_addr = 0;
static uintptr_t read_target_addr(void) {
    FILE *f = addr_override_open(ADDR_PATH); /* file overrides the baked default */
    if (f) {
        unsigned long long v = 0;
        int ok = (fscanf(f, "%llx", &v) == 1);
        fclose(f);
        if (ok && v)
            return (uintptr_t) v;
    }
    return ADDR_DEFAULT;
}

/* juce::AudioBuffer<float> layout (verified against JUCE headers):
 *   0: int numChannels; 4: int numSamples; 8: size_t allocatedBytes;
 *   16: float** channels;  */
#define OFF_NUMCH 0
#define OFF_NUMSAMP 4
#define OFF_CHANS 16

/* tmpfs only. /home/root/settings is the 61 MB p7 partition; logging can fill it
 * in minutes, and a full p7 costs EP147 its BPM/analysis display. */
#define LOG_PATH "/tmp/azstem.log"

/* Past this size the log is truncated and logging continues, so it cannot
 * starve the other logs on /tmp. */
#define LOG_MAX_BYTES (4L * 1024 * 1024)
/* Off unless settings/azmod-logs.on existed at start: the same switch as azmod's
 * diagnostics, so users' decks do no debug logging. */
static int g_log_on = -1;
static int log_on(void) {
    if (g_log_on < 0)
        g_log_on = access("/home/root/settings/azmod-logs.on", F_OK) == 0;
    return g_log_on;
}
static void logln(const char *fmt, ...) {
    if (!log_on())
        return;
    FILE *f = fopen(LOG_PATH, "ae"); /* logcap-ok: truncated past LOG_MAX_BYTES below */
    if (!f)
        return;
    /* On this libc ftell() right after fopen("a") returns 0 until the first
     * write, so seek to the end to get the real size. */
    {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        if (sz > LOG_MAX_BYTES) {
            fclose(f);
            f = fopen(LOG_PATH, "we");
            if (!f)
                return;
            /* Mark it, so missing earlier lines are not mistaken
             * for a probe that never fired. */
            fprintf(f, "[%ld] --- log truncated at %ld bytes ---\n", (long) time(NULL), sz);
        }
    }
    fprintf(f, "[%ld] ", (long) time(NULL));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* ---- per-deck level publish (RT hook -> flusher), keyed by `this` ----- */
#define MAX_DECKS 8
static _Atomic(void *) g_deck_self[MAX_DECKS];

/* ---- Audio deck identity (from the ctor, never arrival order) ----------- *
 * processBlockInternal sees four dj_player::DjPlayerUnit decks (vptr 0x289f878)
 * and two meow::AudioRecorderUnit (vptr 0x2e538f0).
 *
 * The deck number is handed over by code: factory @0x7ad5a0 is called by
 * @0x7ada70 with w0 = 0..3 (unit stored at owner+0x2a8+8*n), packs it into an id
 * struct ({u8 no; ...; u32 no at +8}) and passes it to DjPlayerUnit's ctor
 * @0x14a2560, which stores it at
 *       *(u32 *)( *(void **)(unit + 0x1a0) + 0x1f18 )
 * On the deck the four units read 0, 1, 2, 3 there; the recorder units have
 * +0x1a0 == 0 and are rejected.
 *
 * Everything downstream keys off g_deck_self[STEM_SLOT[deck]], so seeding the
 * right unit into the right slot makes every per-deck consumer exact. */
#define DJPU_VTABLE EP_289f878                          /* dj_player::DjPlayerUnit primary vtable */
#define DJPU_SUB_OFF 0x1a0                              /* unit -> sub-object built by the ctor */
#define DJPU_NO_OFF 0x1f18                              /* sub-object -> u32 player number 0..3 */
#define DJPUVT_PATH "/home/root/settings/azstem.djpuvt" /* hex override */
static _Atomic uintptr_t g_djpu_vt; /* = DJPU_VTABLE, set in azstem_init once the build is known */
static _Atomic unsigned g_ap_seen = 0;     /* bitmask: decks resolved by identity */
static _Atomic unsigned g_ap_conflict = 0; /* bitmask: slot already held by another unit */
static _Atomic unsigned g_ap_ctor = 0;     /* bitmask: decks seeded by the ctor hook */
/* Counters for a DjPlayerUnit constructed for a deck whose slot is already taken:
 *   g_ctor_calls[d]: DjPlayerUnits constructed for deck d (>1 = EP147 rebuilt it).
 *   g_ap_rebind:     ctor ran for a deck whose slot held a different object,
 *                      so the seed CAS was dropped. */
static _Atomic unsigned g_ctor_calls[MAX_DECKS];
static _Atomic unsigned g_ap_rebind = 0;
/* Diagnostic: the order units first render. rank[deck] is the slot an
 * arrival-order claim would have given that deck; rank != deck shows such a
 * claim would have been wrong on this boot. */
static _Atomic unsigned g_arr_next = 0;
static _Atomic int g_arr_rank[MAX_DECKS]; /* -1 = not yet rendered */
static _Atomic int g_arr_init = 0;

/* Deck 0..3 for an audio unit, or -1 if it is not an identifiable DjPlayerUnit.
 * RT-safe: the vtable check rejects other classes and the deref is range-guarded. */
static inline int ap_deck_of(void *self) {
    if (!self)
        return -1;
    uintptr_t vt = *(uintptr_t *) self;
    uintptr_t want = atomic_load_explicit(&g_djpu_vt, memory_order_relaxed);
    if (want && vt != want)
        return -1; /* not a DjPlayerUnit */
    uintptr_t sub = *(uintptr_t *) ((char *) self + DJPU_SUB_OFF);
    if (sub < 0x10000 || sub > 0x0000ffffffffffffUL)
        return -1;
    unsigned n = *(const unsigned *) ((const char *) sub + DJPU_NO_OFF);
    return (n < (unsigned) MAX_DECKS) ? (int) n : -1;
}
/* Each slot's unit's own player number, as hook_pbi last read it inside that unit's render
 * (-1 unknown). The identity census reads this copy, never the unit. */
static _Atomic int g_slot_id[MAX_DECKS] = {-1, -1, -1, -1, -1, -1, -1, -1};
static _Atomic float g_deck_peakL[MAX_DECKS];
static _Atomic float g_deck_peakR[MAX_DECKS];
static _Atomic int g_deck_ch[MAX_DECKS];
static _Atomic int g_deck_ns[MAX_DECKS];
static _Atomic float g_deck_zcr[MAX_DECKS]; /* zero-crossing rate of deck output = pitch proxy */
static _Atomic unsigned g_gen = 0;
static _Atomic unsigned g_mute_mask = 0;   /* bit per deck slot: 1 = muted */
static _Atomic int g_probe = 0;            /* field-discovery probe on (hook + flusher) */
/* Per-call playhead trace (diagnostic). A backspin lasts about a second, too
 * short for the 1/s DIAG line, so the hook records every raw playhead sample
 * and the flusher dumps the ring once a second. */
#define TRACE_N 1024
static long g_trace[TRACE_N];
static _Atomic long g_trace_w = 0;
static _Atomic int g_trace_on = 0; /* /home/root/settings/azstem.trace */
#define TRACE_PATH "/home/root/settings/azstem.trace"
#define MUTE_PATH "/home/root/settings/azstem.mute" /* decimal bitmask; development builds only (dev_input_open) */
#include "azstem_internal.h"
#include "stem_playback.c"
#include "stem_envelopes.c"
#include "deck_track_map.c"

/* ---- native source-read hook (PageBuffer::read) ---------------------- *
 * Position is a 32-byte value returned via x8; returning a 32-byte struct makes
 * the C ABI use x8 too. */
typedef struct {
    unsigned char b[32];
} Pos32;
typedef Pos32 (*pbread_fn)(void *self, Float2 *dst, const Pos32 *pos, long long cnt);
static pbread_fn g_orig_pbread = NULL; /* trampoline to real PageBuffer::read */
static uintptr_t g_pbread_addr = 0;
/* Observation ring (diagnostic, while g_pbobs_on): pbread_hook records each
 * call's args and return so the flusher can decode them off the RT thread. */
struct pbobs {
    void *self;
    Pos32 in, out;
    long long cnt;
    float s0L, s0R, sNL, sNR;
};
static struct pbobs g_pbring[128];
static _Atomic unsigned g_pbw = 0; /* write index (RT) */
static unsigned g_pbr = 0;         /* read index (flusher) */
static _Atomic int g_pbobs_on = 0; /* azstem.pbobs present */
/* azstem.fgprobs present. Refreshed once per flusher tick, never tested inside
 * fgpr_read_hook: that runs on every source read, and a syscall per read there
 * can stall the deck. */
#define FGPROBS_PATH "/home/root/settings/azstem.fgprobs"
static _Atomic int g_fgprobs_on = 0;
#define PBOBS_PATH "/home/root/settings/azstem.pbobs"
/* Injection: pbread_hook replaces the track PCM a PageBuffer read produced with
 * the owning deck's stem sum at the read's source sample index (Position +0x18,
 * 44100 Hz, the stem rate). EP147's time_domain adapters above PageBuffer then
 * apply tempo, keylock, key-shift and scratch to the stems natively. A read whose
 * deck is unknown is left untouched, which keeps previews and other decks clean. */
static _Atomic int g_inject_on = 1; /* always: pre-DSP injection is the only route */
static _Atomic uintptr_t g_target_pbself = 0;
static __thread int g_tls_deck =
    -1; /* set by hook_pbi around g_orig: the deck rendering on this thread (-1 = none) */

static _Atomic long g_pbread_tls_set = 0,
                    g_pbread_tls_unset = 0; /* diag: pbread injection routing source */
#include "stem_fx.c" /* Stem FX: state and press control; rendered by key_shift.c's read hook */
#include "key_shift.c"

/* ---- AArch64 inline-hook installer (fail-safe) ----------------------- *
 * Classify a prologue instruction: 0 = position-independent (copy verbatim),
 * 2 = ADRP (relocated), 1 = other PC-relative (adr/b/bl/b.cond/cbz/tbz/
 * ldr-literal: the hook is declined). */
static int pcrel_kind(uint32_t in) {
    if ((in & 0x9F000000u) == 0x90000000u)
        return 2; /* adrp (relocatable) */
    if ((in & 0x9F000000u) == 0x10000000u)
        return 1; /* adr  */
    if ((in & 0xFC000000u) == 0x14000000u)
        return 1; /* b */
    if ((in & 0xFC000000u) == 0x94000000u)
        return 1; /* bl */
    if ((in & 0xFF000010u) == 0x54000000u)
        return 1; /* b.cond */
    if ((in & 0x7E000000u) == 0x34000000u)
        return 1; /* cbz/cbnz */
    if ((in & 0x7E000000u) == 0x36000000u)
        return 1; /* tbz/tbnz */
    if ((in & 0x3B000000u) == 0x18000000u)
        return 1; /* ldr(literal) */
    return 0;
}

/* Does this instruction end the function (ret / br in any form, eret)? */
static int ends_fn(uint32_t in) {
    return (in & 0xFFFFF000u) == 0xD65F0000u || (in & 0xFFFFF000u) == 0xD61F0000u ||
           (in & 0xFFFFF800u) == 0xD71F0800u || in == 0xD69F03E0u;
}

/* Re-encode an ADRP so, executed from new_pc, it still targets the same page
 * it did at old_pc. (immhi:immlo is a signed 21-bit count of 4KB pages.) */
static uint32_t reloc_adrp(uint32_t in, uintptr_t old_pc, uintptr_t new_pc) {
    uint32_t immlo = (in >> 29) & 0x3u;
    uint32_t immhi = (in >> 5) & 0x7FFFFu;
    int64_t imm = (int64_t) ((immhi << 2) | immlo);
    if (imm & (1LL << 20))
        imm |= ~((1LL << 21) - 1); /* sign-extend 21b */
    uintptr_t target = (old_pc & ~0xFFFUL) + ((uintptr_t) imm << 12);
    int64_t nimm = ((int64_t) (target & ~0xFFFUL) - (int64_t) (new_pc & ~0xFFFUL)) >> 12;
    uint32_t nlo = (uint32_t) (nimm & 0x3);
    uint32_t nhi = (uint32_t) ((nimm >> 2) & 0x7FFFF);
    return (in & ~((0x3u << 29) | (0x7FFFFu << 5))) | (nlo << 29) | (nhi << 5);
}

/* Can this ADRP be re-encoded to run at new_pc? Its page offset is a signed 21-bit
 * count (+-4 GB), and the mmap'd trampoline is usually out of that range from
 * EP147's text. Re-encoding anyway would truncate the offset, so the installer
 * materializes the address instead. */
static int adrp_reloc_ok(uint32_t in, uintptr_t old_pc, uintptr_t new_pc) {
    uint32_t immlo = (in >> 29) & 0x3u;
    uint32_t immhi = (in >> 5) & 0x7FFFFu;
    int64_t imm = (int64_t) ((immhi << 2) | immlo);
    if (imm & (1LL << 20))
        imm |= ~((1LL << 21) - 1);
    uintptr_t target = (old_pc & ~0xFFFUL) + ((uintptr_t) imm << 12);
    int64_t nimm = ((int64_t) (target & ~0xFFFUL) - (int64_t) (new_pc & ~0xFFFUL)) >> 12;
    return nimm >= -(1LL << 20) && nimm < (1LL << 20);
}

/* The page address an ADRP at pc produces. */
static uint64_t adrp_target(uint32_t in, uintptr_t pc) {
    uint32_t immlo = (in >> 29) & 0x3u;
    uint32_t immhi = (in >> 5) & 0x7FFFFu;
    int64_t imm = (int64_t) ((immhi << 2) | immlo);
    if (imm & (1LL << 20))
        imm |= ~((1LL << 21) - 1);
    return (uint64_t) ((pc & ~0xFFFUL) + ((uintptr_t) imm << 12));
}

/* Load a 64-bit constant into Xd: MOVZ + 3x MOVK.
 *   MOVZ Xd,#imm16,LSL#s = 0xD2800000 | hw<<21 | imm16<<5 | Rd
 *   MOVK Xd,#imm16,LSL#s = 0xF2800000 | hw<<21 | imm16<<5 | Rd   (hw = s/16) */
static void emit_abs_addr(uint32_t *out, int rd, uint64_t val) {
    out[0] = 0xD2800000u | (0u << 21) | ((uint32_t) (val & 0xFFFFu) << 5) | (uint32_t) rd;
    out[1] = 0xF2800000u | (1u << 21) | ((uint32_t) ((val >> 16) & 0xFFFFu) << 5) | (uint32_t) rd;
    out[2] = 0xF2800000u | (2u << 21) | ((uint32_t) ((val >> 32) & 0xFFFFu) << 5) | (uint32_t) rd;
    out[3] = 0xF2800000u | (3u << 21) | ((uint32_t) ((val >> 48) & 0xFFFFu) << 5) | (uint32_t) rd;
}

/* Write a 16-byte absolute jump (LDR X17,#8 ; BR X17 ; .quad target). */
static void emit_abs_jump(uint32_t *dst, uint64_t target) {
    dst[0] = 0x58000051u; /* LDR X17, #8   */
    dst[1] = 0xD61F0220u; /* BR  X17       */
    memcpy(&dst[2], &target, 8);
}

/* Install a 16-byte absolute-jump inline hook at FUNC -> hookfn. The trampoline
 * (4 relocated prologue instructions + a jump back to FUNC+16), callable as the
 * original, is stored in *orig before the jump is written, so a call that reaches
 * hookfn the moment the jump lands already finds its original; the hook calls through
 * *orig without a null check. Returns the trampoline, or NULL (and *orig untouched)
 * if the prologue cannot be relocated safely or a syscall fails. */
/* Every azstem hook is a 16-byte absolute jump, which another core could see half written, so
 * all of them go in from azstem_init, before EP147's main runs any of its code. Set when
 * azstem_init returns; from then on install_hook_at refuses. */
static _Atomic int g_hooks_late;
static void *install_hook_at(uintptr_t FUNC, void *hookfn, void **orig) {
    if (atomic_load_explicit(&g_hooks_late, memory_order_acquire)) {
        logln("install: @0x%lx after the constructor -- REFUSED (16-byte patch)", (unsigned long) FUNC);
        return NULL;
    }
    if (FUNC == 0 || (FUNC & 3)) {
        logln("install: no or misaligned addr (0x%lx) -- declined", (unsigned long) FUNC);
        return NULL;
    }
    uint32_t *func = (uint32_t *) FUNC;

    for (int i = 0; i < 4; i++) {
        if (pcrel_kind(func[i]) == 1) {
            logln("install: @0x%lx prologue insn %d (%08x) PC-relative & non-relocatable"
                  " -- DECLINED (fail-safe)",
                  (unsigned long) FUNC, i, func[i]);
            return NULL;
        }
        /* a function that returns (or branches away) inside the 16 bytes the jump
         * covers is shorter than the jump: it would overwrite the next function */
        if (i < 3 && ends_fn(func[i])) {
            logln("install: @0x%lx insn %d (%08x) ends the function inside the 16-byte patch"
                  " -- DECLINED (fail-safe)",
                  (unsigned long) FUNC, i, func[i]);
            return NULL;
        }
    }

    /* An ADRP is re-encoded when the trampoline is in range, otherwise its page
     * address is materialized with MOVZ + 3x MOVK. Either way the copied prologue
     * computes the same register values as the original. */
    uint32_t *tramp = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) {
        logln("install: tramp mmap failed");
        return NULL;
    }
    int out = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t in = func[i];
        if (pcrel_kind(in) == 2) {
            uintptr_t old_pc = FUNC + (uintptr_t) i * 4, new_pc = (uintptr_t) &tramp[out];
            if (adrp_reloc_ok(in, old_pc, new_pc)) {
                tramp[out++] = reloc_adrp(in, old_pc, new_pc);
            } else {
                emit_abs_addr(&tramp[out], (int) (in & 0x1F), adrp_target(in, old_pc));
                out += 4;
            }
        } else {
            tramp[out++] = in;
        }
    }
    emit_abs_jump(&tramp[out], FUNC + 16);
    __builtin___clear_cache((char *) tramp, (char *) tramp + out * 4 + 16);
    /* the trampoline is complete: r-x from here on, never writable and executable */
    if (mprotect(tramp, 4096, PROT_READ | PROT_EXEC) != 0) {
        logln("install: tramp mprotect r-x failed @0x%lx -- declined", (unsigned long) FUNC);
        munmap(tramp, 4096);
        return NULL;
    }

    /* patch FUNC entry -> hook: writable for the store only, then exactly the
     * protection the page had (read from /proc/self/maps, never assumed) */
    azprot_t pt;
    if (azprot_unlock(FUNC, 16, &pt) != 0) {
        logln("install: cannot make 0x%lx writable -- declined", (unsigned long) FUNC);
        munmap(tramp, 4096);
        return NULL;
    }
    /* the original first, then a release fence, then the jump: no core can run the
     * hook while *orig still reads empty */
    __atomic_store_n(orig, (void *) tramp, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    emit_abs_jump(func, (uint64_t) (uintptr_t) hookfn);
    __builtin___clear_cache((char *) func, (char *) func + 16);
    if (azprot_relock(&pt) != 0)
        logln("install: @0x%lx code page left writable (mprotect failed)", (unsigned long) FUNC);
    logln("install: hooked @ 0x%lx -> %p, tramp @ %p", (unsigned long) FUNC, hookfn, tramp);
    return tramp;
}

static int install_hook(void) {
    return install_hook_at(g_func_addr, (void *) hook_pbi, (void **) &g_orig) ? 1 : -1;
}

/* Read the per-deck mute bitmask (off the RT thread) into g_mute_mask. */
static void read_mute(void) {
    FILE *f = dev_input_open(MUTE_PATH);
    unsigned m = 0;
    if (f) {
        if (fscanf(f, "%u", &m) != 1)
            m = 0;
        fclose(f);
    }
    atomic_store_explicit(&g_mute_mask, m, memory_order_relaxed);
    atomic_store_explicit(&g_allsum, dev_flag(ALLSUM_PATH), memory_order_relaxed);
}
static void read_stems(void) {
    static unsigned prev_em[NDECK];
    static int primed_em[NDECK];
    for (int d = 0; d < NDECK; d++) {
        FILE *f = fopen(STEMS_PATH[d], "re");
        unsigned v = 0;
        if (f) {
            if (fscanf(f, "%u", &v) != 1)
                v = 0;
            fclose(f);
        }
        atomic_store_explicit(&g_dk[d].ctrl, v, memory_order_relaxed);
        unsigned em = v & 0x0Fu;              /* enabled-stem bits only */
        if (primed_em[d] && em != prev_em[d]) /* edge: a stem was toggled */
            atomic_fetch_or_explicit(&g_evict_req, 1u << d, memory_order_relaxed);
        prev_em[d] = em;
        primed_em[d] = 1;
    }
    FILE *ef = fopen(ENABLE_PATH, "re"); /* master; default enabled if absent */
    int en = 1;
    if (ef) {
        int c = fgetc(ef);
        en = (c != '0');
        fclose(ef);
    }
    atomic_store_explicit(&g_enable, en, memory_order_relaxed);
}

/* ---- flusher (all off RT): mute at 5Hz, per-deck levels ~1/s ----------- */

/* ---- Flusher phase tracking + the mask poller -----------------------------
 *
 * read_stems() is the only reader of the stem toggle. The flusher can be busy
 * for 10+ s after a track change (USB walks, memory probes, decodes), so the
 * toggle must not wait on it:
 *   - maskpoll_thread() runs read_stems() every 50ms on its own thread.
 *   - the same thread is a watchdog: PH(n) stamps each flusher step's id and
 *     start time, and a step held too long is written to /tmp/azstem.stall from
 *     outside the stalled loop. */
/* Persistent telemetry, unaffected by log truncation: /tmp/azstem.probestat holds
 * install status, cumulative hit counts and streaming state. */
static _Atomic uintptr_t g_rs_caller = 0;
static _Atomic long g_rs_pos = 0, g_rs_size = 0;
static uintptr_t g_probe_addr[NPROBE]; /* filled in azstem_init */
static int g_probe_installed[NPROBE];
static _Atomic long g_probe_hits[NPROBE] __attribute__((used)); /* read by name from PROBE_TRAMP's asm (the development probes) */
static _Atomic int g_ph_id = -1;
static _Atomic long g_ph_ts = 0; /* ms_now() when the step began */
static _Atomic int g_ph_worst_id = -1;
static _Atomic long g_ph_worst_ms = 0;
static int g_maskpoll_up = 0; /* 0 => flusher keeps calling read_stems itself */
static void probestat_write(void) {
    static unsigned n;
    if ((n++ % 5u) != 0)
        return; /* nobody reads it live: 1/s is enough */
    FILE *f = fopen("/tmp/azstem.probestat", "we");
    if (!f)
        return;
    fprintf(f, "pbi_gen=%u PBREAD=%u FGPRread=%d setsrc=%d\n",
            atomic_load_explicit(&g_gen, memory_order_relaxed),
            atomic_load_explicit(&g_pbread_hits, memory_order_relaxed),
            atomic_load_explicit(&g_fgpr_read_hits, memory_order_relaxed),
            atomic_load_explicit(&g_fgpr_setsrc_hits, memory_order_relaxed));
    fprintf(f, "pbread_tls_set=%ld pbread_tls_unset=%ld\n",
            atomic_load_explicit(&g_pbread_tls_set, memory_order_relaxed),
            atomic_load_explicit(&g_pbread_tls_unset, memory_order_relaxed));
    {
        int rb = atomic_load_explicit(&g_key_ratio_bits, memory_order_relaxed);
        float rr;
        memcpy(&rr, &rb, 4);
        long long injs = atomic_load_explicit(&g_inj_start, memory_order_relaxed);
        long long pS = atomic_load_explicit(&g_play_S[0], memory_order_relaxed);
        fprintf(f, "inj_start=%lld inj_cnt=%lld inj_adv=%lld\n", injs,
                (long long) atomic_load_explicit(&g_inj_cnt, memory_order_relaxed),
                (long long) atomic_load_explicit(&g_inj_adv, memory_order_relaxed));
        fprintf(f, "play_S=%lld readahead_gap=%lld gap_sec=%.2f\n", pS, injs - pS,
                (double) (injs - pS) / AZ_EP147_RATE);
        {
            long long gmn = atomic_load_explicit(&g_gap_min, memory_order_relaxed);
            long long gmx = atomic_load_explicit(&g_gap_max, memory_order_relaxed);
            long long gam = atomic_load_explicit(&g_gap_absmin, memory_order_relaxed);
            fprintf(f, "gap_min=%.2fs gap_max=%.2fs gap_closest=%.3fs\n",
                    gmn == 0x7fffffffffffffffLL ? 0.0 : (double) gmn / AZ_EP147_RATE,
                    gmx == -0x7fffffffffffffffLL ? 0.0 : (double) gmx / AZ_EP147_RATE,
                    gam == 0x7fffffffffffffffLL ? 0.0 : (double) gam / AZ_EP147_RATE);
            atomic_store_explicit(&g_gap_min, 0x7fffffffffffffffLL, memory_order_relaxed);
            atomic_store_explicit(&g_gap_max, -0x7fffffffffffffffLL, memory_order_relaxed);
            atomic_store_explicit(&g_gap_absmin, 0x7fffffffffffffffLL, memory_order_relaxed);
        }
        fprintf(f, "ra_caller=0x%lx\n",
                (unsigned long) atomic_load_explicit(&g_ra_caller, memory_order_relaxed));
        {
            long long opp = atomic_load_explicit(&g_op_pos, memory_order_relaxed);
            long bp = atomic_load_explicit(&g_pitch_bufpos, memory_order_relaxed);
            long bl = atomic_load_explicit(&g_pitch_buflim, memory_order_relaxed);
            fprintf(f,
                    "op_pos_vs_play=%.2fs pitch_buf_span=%.2fs (bufpos=%ld buflim=%ld) "
                    "op_caller=0x%lx\n",
                    (double) (opp - pS) / AZ_EP147_RATE, (double) (bl - bp) / AZ_EP147_RATE, bp, bl,
                    (unsigned long) atomic_load_explicit(&g_op_caller, memory_order_relaxed));
        }
        fprintf(f, "operate=%d op_ratio=%.5f nativekey_subst=%d\n",
                atomic_load_explicit(&g_operate_hits, memory_order_relaxed), (double) rr,
                atomic_load_explicit(&g_nativekey_subst, memory_order_relaxed));
        long tsws = atomic_load_explicit(&g_ts_ws, memory_order_relaxed);
        fprintf(f,
                "ts_src=0x%lx ts_vtbl=0x%lx ts_buf=0x%lx ts_cap=%d ts_ws=%ld ts_fill=%ld "
                "ts_win_vs_play=%.2fs\n",
                (unsigned long) atomic_load_explicit(&g_ts_src, memory_order_relaxed),
                (unsigned long) atomic_load_explicit(&g_ts_vtbl, memory_order_relaxed),
                (unsigned long) atomic_load_explicit(&g_ts_buf, memory_order_relaxed),
                atomic_load_explicit(&g_ts_cap, memory_order_relaxed), tsws,
                atomic_load_explicit(&g_ts_fill, memory_order_relaxed),
                (double) (tsws - pS) / AZ_EP147_RATE);
        fprintf(f, "evict_calls=%d evict_req=%u\n",
                atomic_load_explicit(&g_evict_calls, memory_order_relaxed),
                atomic_load_explicit(&g_evict_req, memory_order_relaxed));
        {
            long m1 = atomic_load_explicit(&g_ring_mask1, memory_order_relaxed);
            long m2 = atomic_load_explicit(&g_ring_mask2, memory_order_relaxed);
            long cap = atomic_load_explicit(&g_ring_cap, memory_order_relaxed);
            fprintf(f,
                    "ring_base=0x%lx captured=%d mask1=0x%lx(=%.2fs) mask2=0x%lx(=%.2fs) "
                    "cap=%ld(=%.2fs) trklen=%ld ptr1=0x%lx ptr2=0x%lx\n",
                    (unsigned long) atomic_load_explicit(&g_ring_base, memory_order_relaxed),
                    atomic_load_explicit(&g_ring_captured, memory_order_relaxed), m1,
                    (double) (m1 + 1) / AZ_EP147_RATE, m2, (double) (m2 + 1) / AZ_EP147_RATE, cap,
                    (double) cap / AZ_EP147_RATE,
                    atomic_load_explicit(&g_ring_trklen, memory_order_relaxed),
                    (unsigned long) atomic_load_explicit(&g_ring_ptr1, memory_order_relaxed),
                    (unsigned long) atomic_load_explicit(&g_ring_ptr2, memory_order_relaxed));
        }
        {
            long rspos = atomic_load_explicit(&g_rs_pos, memory_order_relaxed);
            fprintf(f, "rs_caller=0x%lx rs_pos_vs_play=%.2fs rs_size=%ld\n",
                    (unsigned long) atomic_load_explicit(&g_rs_caller, memory_order_relaxed),
                    (double) (rspos - pS) / AZ_EP147_RATE,
                    atomic_load_explicit(&g_rs_size, memory_order_relaxed));
        }
        {
            long rd = atomic_load_explicit(&g_ring_read, memory_order_relaxed);
            long L = atomic_load_explicit(&g_ring_L, memory_order_relaxed);
            long f1 = atomic_load_explicit(&g_ring_fill1, memory_order_relaxed);
            long f2 = atomic_load_explicit(&g_ring_fill2, memory_order_relaxed);
            fprintf(f,
                    "fill: read=%ld L-read=%.2fs fill1-read=%.2fs fill2-read=%.2fs | mult=%ld "
                    "cnt0=%ld capA=%ld(=%.2fs) ratA=%ld\n",
                    rd, (double) (L - rd) / AZ_EP147_RATE, (double) (f1 - rd) / AZ_EP147_RATE,
                    (double) (f2 - rd) / AZ_EP147_RATE,
                    atomic_load_explicit(&g_ring_mult, memory_order_relaxed),
                    atomic_load_explicit(&g_ring_cnt0, memory_order_relaxed),
                    atomic_load_explicit(&g_ring_capA, memory_order_relaxed),
                    (double) atomic_load_explicit(&g_ring_capA, memory_order_relaxed) / AZ_EP147_RATE,
                    atomic_load_explicit(&g_ring_ratA, memory_order_relaxed));
        }
        {
            /* "HOOKED" tells an idle counter from a hook that was never installed.
             * fill1-read is the ring's fill frontier: the true look-ahead depth. */
            {
                int wid = atomic_load_explicit(&g_ph_worst_id, memory_order_relaxed);
                long wms = atomic_load_explicit(&g_ph_worst_ms, memory_order_relaxed);
                fprintf(f, "flusher: maskpoll=%s worst_stall=step %d %ldms\n",
                        g_maskpoll_up ? "UP" : "**DOWN**", wid, wms);
            }
            fprintf(f,
                    "pb_deck_of: struct=%ld alias_refused=%ld target(HARDWIRED d0)=%ld table=%ld "
                    "miss=%ld\n",
                    atomic_load_explicit(&g_pbd_struct, memory_order_relaxed),
                    atomic_load_explicit(&g_pbd_alias, memory_order_relaxed),
                    atomic_load_explicit(&g_pbd_target, memory_order_relaxed),
                    atomic_load_explicit(&g_pbd_table, memory_order_relaxed),
                    atomic_load_explicit(&g_pbd_miss, memory_order_relaxed));
            fprintf(f, "fgpr_hooks: ringfill=%s fgpr_op=%s\n",
                    g_orig_ringfill ? "HOOKED" : "**NOT INSTALLED**",
                    g_orig_fgpr_op ? "HOOKED" : "**NOT INSTALLED**");
            for (int d = 0; d < NDECK; d++)
                fprintf(
                    f,
                    "stemofs d%d load_off=%ld hold=%d stale=%d | verify state=%d base=%lld "
                    "captured=%ld tries=%d off=%ld%s\n",
                    d, g_ofs_load[d], atomic_load_explicit(&g_gate_hold[d], memory_order_relaxed),
                    atomic_load_explicit(&g_load_stale[d], memory_order_relaxed),
                    atomic_load_explicit(&g_ofs_state[d], memory_order_relaxed),
                    atomic_load_explicit(&g_ofs_base[d], memory_order_relaxed),
                    atomic_load_explicit(&g_ofs_next[d], memory_order_relaxed), g_ofs_tries[d],
                    atomic_load_explicit(&g_dk[d].stem_off, memory_order_relaxed),
                    atomic_load_explicit(&g_nostemofs, memory_order_relaxed) ? " (nostemofs)" : "");
        }
        /* Per-deck state every second (the deck[] log lines are change-only), so
         * a stem toggle can be timed by the peak collapsing after ctrl changes. */
        for (int d = 0; d < NDECK; d++)
            fprintf(f,
                    "deck%d ctrl=0x%02x frames=%ld play_S=%lld peakL=%.5f peakR=%.5f fgpr=0x%lx "
                    "gain=%d/%d/%d/%d\n",
                    d, atomic_load_explicit(&g_dk[d].ctrl, memory_order_relaxed),
                    g_dk[d].stem_frames,
                    (long long) atomic_load_explicit(&g_play_S[d], memory_order_relaxed),
                    atomic_load_explicit(&g_deck_peakL[STEM_SLOT[d]], memory_order_relaxed),
                    atomic_load_explicit(&g_deck_peakR[STEM_SLOT[d]], memory_order_relaxed),
                    (unsigned long) atomic_load_explicit(&g_ak_fgpr_rt[d], memory_order_relaxed),
                    atomic_load_explicit(&g_sgain_k[d][0], memory_order_relaxed),
                    atomic_load_explicit(&g_sgain_k[d][1], memory_order_relaxed),
                    atomic_load_explicit(&g_sgain_k[d][2], memory_order_relaxed),
                    atomic_load_explicit(&g_sgain_k[d][3], memory_order_relaxed));
    }
    for (int i = 0; i < NPROBE; i++)
        fprintf(f, "probe[%d] @0x%lx installed=%d hits=%ld\n", i, (unsigned long) g_probe_addr[i],
                g_probe_installed[i], atomic_load_explicit(&g_probe_hits[i], memory_order_relaxed));
    fclose(f);
}
/* PH marks the start of a step, so the time elapsed when the next PH fires
 * belongs to the step that just ended. Accumulating it gives a per-step profile
 * of the flusher. Cheap enough (a subtraction and two relaxed adds) to stay on,
 * so the profile exists whenever a stall happens. */
#define PH_MAX 40
static _Atomic long g_ph_acc[PH_MAX]; /* total ms attributed to the step */
static _Atomic long g_ph_cnt[PH_MAX]; /* visits */
static _Atomic long g_ph_max[PH_MAX]; /* worst single visit */

#define PH(n)                                                                      \
    do {                                                                           \
        long _now = ms_now();                                                      \
        int _prev = atomic_load_explicit(&g_ph_id, memory_order_relaxed);          \
        if (_prev >= 0 && _prev < PH_MAX) {                                        \
            long _d = _now - atomic_load_explicit(&g_ph_ts, memory_order_relaxed); \
            if (_d < 0)                                                            \
                _d = 0;                                                            \
            atomic_fetch_add_explicit(&g_ph_acc[_prev], _d, memory_order_relaxed); \
            atomic_fetch_add_explicit(&g_ph_cnt[_prev], 1, memory_order_relaxed);  \
            if (_d > atomic_load_explicit(&g_ph_max[_prev], memory_order_relaxed)) \
                atomic_store_explicit(&g_ph_max[_prev], _d, memory_order_relaxed); \
        }                                                                          \
        atomic_store_explicit(&g_ph_id, (n), memory_order_relaxed);                \
        atomic_store_explicit(&g_ph_ts, _now, memory_order_relaxed);               \
    } while (0)

/* Dump the profile, worst total first. Rewritten in place so it never grows. */
static void ph_profile_dump(void) {
    static long last;
    long now = ms_now();
    if (now - last < 10000)
        return; /* every 10s, not per tick */
    last = now;
    int idx[PH_MAX];
    int n = 0;
    for (int i = 0; i < PH_MAX; i++)
        if (atomic_load_explicit(&g_ph_cnt[i], memory_order_relaxed) > 0)
            idx[n++] = i;
    for (int a = 0; a < n; a++) /* selection sort by total ms */
        for (int b = a + 1; b < n; b++)
            if (atomic_load_explicit(&g_ph_acc[idx[b]], memory_order_relaxed) >
                atomic_load_explicit(&g_ph_acc[idx[a]], memory_order_relaxed)) {
                int t = idx[a];
                idx[a] = idx[b];
                idx[b] = t;
            }
    FILE *f = fopen("/tmp/azstem.phprof", "we");
    if (!f)
        return;
    fprintf(f, "flusher per-step profile (cumulative since start)\n");
    fprintf(f, "%-6s %12s %10s %10s %10s\n", "step", "total_ms", "visits", "avg_ms", "worst_ms");
    for (int a = 0; a < n; a++) {
        int i = idx[a];
        long acc = atomic_load_explicit(&g_ph_acc[i], memory_order_relaxed);
        long cnt = atomic_load_explicit(&g_ph_cnt[i], memory_order_relaxed);
        fprintf(f, "%-6d %12ld %10ld %10.2f %10ld\n", i, acc, cnt,
                cnt ? (double) acc / (double) cnt : 0.0,
                atomic_load_explicit(&g_ph_max[i], memory_order_relaxed));
    }
    fclose(f);
}
#define PH_STALL_MS 1500 /* well past the 200ms loop period */

static void *maskpoll_thread(void *arg) {
    (void) arg;
    long reported = 0;
    for (;;) {
        read_stems(); /* the stem toggle: nothing else may gate it */
        {
            long ts = atomic_load_explicit(&g_ph_ts, memory_order_relaxed);
            int id = atomic_load_explicit(&g_ph_id, memory_order_relaxed);
            long held = ts ? (ms_now() - ts) : 0;
            if (ts && held > PH_STALL_MS && id >= 0) {
                if (held > atomic_load_explicit(&g_ph_worst_ms, memory_order_relaxed)) {
                    atomic_store_explicit(&g_ph_worst_ms, held, memory_order_relaxed);
                    atomic_store_explicit(&g_ph_worst_id, id, memory_order_relaxed);
                }
                if (held - reported > 1000) { /* refresh ~1/s while it is stuck */
                    FILE *sf = fopen("/tmp/azstem.stall", "we");
                    if (sf) {
                        fprintf(sf, "flusher STALLED in step %d for %ldms\n", id, held);
                        fclose(sf);
                    }
                    reported = held;
                }
            } else
                reported = 0;
        }
        usleep(50000); /* 20Hz */
    }
    return NULL;
}
static void *flusher(void *arg) {
    (void) arg;
    unsigned last = 0;
    int tick = 0;
    for (;;) {
        /* Step 0 is the sleep. Without its own mark the 200ms idle would be
         * charged to whichever step ran last before the loop wrapped. */
        PH(0);
        usleep(200000);
        PH(1);
        read_mute();
        /* read_stems() runs on maskpoll_thread; here only if that thread failed
         * to start. */
        PH(2);
        if (!g_maskpoll_up)
            read_stems();
        PH(3);
        drum_service(); /* drum-roll bank discovery / load */
        PH(4);
        drum_grid_service();       /* drum-roll beat grid: parse per track change (off RT) */
        sfx_service();             /* Stem FX: retire faded-out effects */
        PH(5);
        atomic_store_explicit(&g_fgprtag_on, !dev_flag(NOFGPRTAG_PATH),
                              memory_order_relaxed);
#ifdef AZSTEM_PCMCAP
        pcmcap_tick();
#endif
        PH(7);
        pbobs_flush(); /* mirror PageBuffer::read observations (off RT) */
        PH(8);
        pbclassify(); /* content-match each PageBuffer -> its owning deck (multi-deck) */
        ofs_tick();   /* per-deck stem offset vs EP147's decode (off RT) */
        PH(9);
        frobs_flush(); /* mirror fileRead observations + inject */
        PH(10);
        opobs_flush(); /* read live key-shift ratio */
        PH(11);
        PH(13);
        PH(14);
        probestat_write(); /* persistent telemetry file (survives log churn) */
        PH(15);
        PH(16);
        find_track(); /* diag: BFS deck->Track (structural deck map) */
        PH(17);
        trackmap_census(); /* diag: g_deck_track[] changes, provenance, aliasing */
        PH(18);
        thread_census(); /* diag: which threads render vs read (TLS deck tag coverage) */
        PH(19);
        kfobs_flush(); /* observe KeyControlFacade key-shift */
        PH(27);
        fgpr_idmap_tick(); /* deck->FGPR identity by construction (feeds fgprtag) */
        PH(20);
        atomic_store_explicit(&g_probe, dev_flag(PROBE_PATH), memory_order_relaxed);
        atomic_store_explicit(&g_trace_on, dev_flag(TRACE_PATH), memory_order_relaxed);
        /* The path scan has no production consumer under the native route.
         * probe_strings walks pointers two levels out from each deck object for
         * something path-shaped and costs ~70ms of every 200ms tick. Its output
         * feeds only the legacy track-name lookup (native route off), the LTI
         * comparison log and its own collision fixer. So it runs only when the
         * native route is off or azstem.ltiprobe asks for the comparison. */
        PH(25);
        stems_tick(); /* discover USB tree / f32 fallback; re-decode on song change */
        PH(26);
        /* Publish per-deck "stems available for the loaded track" for azmod's
         * strips: azstem.avail for deck 1, azstem.avail2..4 for decks 2-4.
         * Written only on change. */
        {
            static int last_avail[NDECK] = {-1, -1, -1, -1};
            static const char *AVAIL_SFX[NDECK] = {"", "2", "3", "4"};
            for (int d = 0; d < NDECK; d++) {
                int av = ((g_dk[d].stem_frames > 0) ||
                          atomic_load_explicit(&g_stem_pending[d], memory_order_relaxed))
                             ? 1
                             : 0;
                if (av != last_avail[d]) {
                    char pth[64];
                    snprintf(pth, sizeof pth, "/home/root/settings/azstem.avail%s", AVAIL_SFX[d]);
                    publish_write(pth, av ? "1" : "0");
                    last_avail[d] = av;
                }
            }
        }
        if (++tick < 5)
            continue; /* log + gen-check only ~1/s */
        tick = 0;
        decks_tick(); /* /tmp/az-decks: may the loader restart EP147 (deck_track_map.c) */
        PH(28); /* (step 28 is unused) */
        PH(29);
        ph_profile_dump(); /* every 10s */
        /* Playhead trace as per-call deltas. Its own flag, because the
         * field-discovery probe logs thousands of lines a second. */
        if (atomic_load_explicit(&g_trace_on, memory_order_relaxed)) {
            static long tr = 0;
            long w = atomic_load_explicit(&g_trace_w, memory_order_relaxed);
            if (w - tr > TRACE_N)
                tr = w - TRACE_N; /* overran: keep the newest */
            while (w > tr) {
                char line[1024];
                int n = 0;
                n += snprintf(line + n, sizeof line - n, "TRACE");
                long prev =
                    (tr > 0) ? g_trace[(tr - 1) & (TRACE_N - 1)] : g_trace[tr & (TRACE_N - 1)];
                for (int i = 0; i < 48 && tr < w && n < (int) sizeof line - 16; i++, tr++) {
                    long s = g_trace[tr & (TRACE_N - 1)];
                    n += snprintf(line + n, sizeof line - n, " %ld", s - prev);
                    prev = s;
                }
                logln("%s", line);
            }
        }
        int st = atomic_load_explicit(&g_installed, memory_order_acquire);
        unsigned g = atomic_load_explicit(&g_gen, memory_order_acquire);
        if (g == last) {
            if (st != 0 && g == 0)
                logln("hooked=%d but processBlockInternal not called yet", st);
            continue;
        }
        last = g;
        /* Identity census: each slot's unit and its own player number. slot ==
         * id means the mapping is right; id -1 is an unidentified unit (recorder).
         * Diagnostic only, so it runs only with logs on. The number is the one hook_pbi
         * read inside the unit's own render (g_slot_id): a cached unit is never read here,
         * since another thread may be destroying it. */
        if (log_on()) {
            static unsigned prev_sig = 0xffffffffu;
            static time_t prev_t = 0;
            unsigned sig = 0;
            time_t now_t = time(NULL);
            for (int i = 0; i < MAX_DECKS; i++) {
                void *sf = atomic_load_explicit(&g_deck_self[i], memory_order_relaxed);
                int id = sf ? atomic_load_explicit(&g_slot_id[i], memory_order_relaxed) : -1;
                sig = sig * 17u + (unsigned) (id + 2);
                sig = sig * 17u +
                      (unsigned) (atomic_load_explicit(&g_arr_rank[i], memory_order_relaxed) + 2);
                sig = sig * 17u + (unsigned) (sf != NULL);
            }
            sig = sig * 17u + atomic_load_explicit(&g_ap_conflict, memory_order_relaxed);
            sig = sig * 17u + atomic_load_explicit(&g_ap_rebind, memory_order_relaxed);
            for (int i = 0; i < NDECK; i++)
                sig = sig * 17u + atomic_load_explicit(&g_ctor_calls[i], memory_order_relaxed);
            /* Re-print at least once a minute, so a truncated log still has a
             * baseline. */
            if (sig != prev_sig || now_t - prev_t >= 60) {
                prev_sig = sig;
                prev_t = now_t;
                char line[512];
                int o = 0;
                for (int i = 0; i < MAX_DECKS; i++) {
                    void *sf = atomic_load_explicit(&g_deck_self[i], memory_order_relaxed);
                    int id = sf ? atomic_load_explicit(&g_slot_id[i], memory_order_relaxed) : -1;
                    o += snprintf(line + o, sizeof line - (size_t) o, "%sslot%d=id%d(arr%d)@%p",
                                  i ? " " : "", i, id,
                                  atomic_load_explicit(&g_arr_rank[i], memory_order_relaxed), sf);
                }
                char cc[64];
                int co = 0;
                for (int i = 0; i < NDECK; i++)
                    co += snprintf(cc + co, sizeof cc - (size_t) co, "%s%u", i ? "," : "",
                                   atomic_load_explicit(&g_ctor_calls[i], memory_order_relaxed));
                logln("IDENTITY %s | ctor=0x%x field=0x%x conflict=0x%x rebind=0x%x ctorcalls=%s",
                      line, atomic_load_explicit(&g_ap_ctor, memory_order_relaxed),
                      atomic_load_explicit(&g_ap_seen, memory_order_relaxed),
                      atomic_load_explicit(&g_ap_conflict, memory_order_relaxed),
                      atomic_load_explicit(&g_ap_rebind, memory_order_relaxed), cc);
            }
        }
        /* Per-unit audio census: units, channel count, block size and whether
         * each side carries signal. Change-only with a 60s heartbeat; every
         * tick would be ~1.9 MB/h and ~30 syscalls/s on an idle deck. Level
         * enters the signature as an audible bit, not a value, and change
         * prints are held to one per 5s so a level at the threshold cannot
         * turn into a stream. */
        {
            static unsigned prev_sig = 0xffffffffu;
            static time_t prev_t = 0;
            unsigned sig = 0;
            time_t now_t = time(NULL);
            for (int i = 0; i < MAX_DECKS; i++) {
                void *self = atomic_load_explicit(&g_deck_self[i], memory_order_relaxed);
                sig = sig * 31u + (unsigned) (uintptr_t) self;
                if (!self)
                    continue;
                sig = sig * 31u +
                      (unsigned) atomic_load_explicit(&g_deck_ch[i], memory_order_relaxed);
                sig = sig * 31u +
                      (unsigned) atomic_load_explicit(&g_deck_ns[i], memory_order_relaxed);
                sig = sig * 31u +
                      (atomic_load_explicit(&g_deck_peakL[i], memory_order_relaxed) > 1e-3f);
                sig = sig * 31u +
                      (atomic_load_explicit(&g_deck_peakR[i], memory_order_relaxed) > 1e-3f);
            }
            if ((sig != prev_sig && now_t - prev_t >= 5) || now_t - prev_t >= 60) {
                prev_sig = sig;
                prev_t = now_t;
                for (int i = 0; i < MAX_DECKS; i++) {
                    void *self = atomic_load_explicit(&g_deck_self[i], memory_order_relaxed);
                    if (!self)
                        continue;
                    logln("deck[%d] self=%p ch=%d ns=%d peakL=%.4f peakR=%.4f zcr=%.4f", i, self,
                          atomic_load_explicit(&g_deck_ch[i], memory_order_relaxed),
                          atomic_load_explicit(&g_deck_ns[i], memory_order_relaxed),
                          atomic_load_explicit(&g_deck_peakL[i], memory_order_relaxed),
                          atomic_load_explicit(&g_deck_peakR[i], memory_order_relaxed),
                          atomic_load_explicit(&g_deck_zcr[i], memory_order_relaxed));
                }
            }
        }
    }
    return NULL;
}

__attribute__((constructor)) static void azstem_init(void) {
    char comm[32] = {0};
    FILE *f = fopen("/proc/self/comm", "re");
    if (f) {
        if (fgets(comm, sizeof comm, f)) {
        }
        fclose(f);
        char *nl = strchr(comm, '\n');
        if (nl)
            *nl = 0;
    }
    if (strcmp(comm, "EP147") != 0)
        return;
    { /* Exact md5 identity of EP147, same as azmod: an unknown build stays inert. */
        char md5[33];
        long sz = 0;
        int b = ep147_identify("/proc/self/exe", md5, &sz);
        int ok = ep147_addr_init(b);
        FILE *bf = azlog_fopen_capped("/tmp/azstem-build.log", 16L * 1024);
        if (bf) {
            fprintf(bf, "[%ld] EP147 %s md5=%s size=%ld -> %s\n", (long) time(NULL),
                    ep147_build_name(b), md5, sz,
                    ok ? "stems ON" : "INERT (unknown EP147 build or incomplete address table)");
            fclose(bf);
        }
        if (!ok)
            return;
        atomic_store_explicit(&g_djpu_vt, (uintptr_t) DJPU_VTABLE, memory_order_relaxed);
        *(void **) &g_meow_destroy_fn = (void *) MEOW_DESTROY; /* drumroll.c: the grid's release */
        const uintptr_t probes[NPROBE] = {EP_7670a8, EP_785228, EP_784e90, EP_782dc0,
                                          EP_77e260, EP_783010, EP_da07d8, EP_d58ee0};
        memcpy(g_probe_addr, probes, sizeof probes);
    }
    /* No deck is loaded yet, so publish that first. A few tiny writes, no
     * filesystem walk (see the USB-scan note below). */
    clear_published_deck_state();
    for (int d = 0; d < NDECK; d++) { /* PLL defaults (struct is zero-init) */
        g_dk[d].cur = -1;
        g_dk[d].prevS = -1;
        g_dk[d].anchorS = -1;
        g_dk[d].rate = 1.0;
        /* -1 = never observed. 0 is a legitimate playhead, so zero-init would
         * read as a real position. */
        atomic_store_explicit(&g_play_obs[d], -1LL, memory_order_relaxed);
    }
    for (int i = 0; i < GP_L; i++)
        g_hann[i] = 0.5f * (1.0f - cosf(6.2831853f * (float) i / (float) GP_L));
    g_func_addr = read_target_addr();
    /* Deck identity: the DjPlayerUnit ctor hook goes in first, before EP147
     * constructs its four players early in main(). */
    {
        FILE *cf = addr_override_open(DJPUCTOR_PATH);
        unsigned long long v = DJPUCTOR_DEFAULT;
        if (cf) {
            unsigned long long x = 0;
            if (fscanf(cf, "%llx", &x) == 1 && x)
                v = x;
            fclose(cf);
        }
        g_djpuctor_addr = (uintptr_t) v;
        void *tc =
            install_hook_at(g_djpuctor_addr, (void *) djpuctor_hook, (void **) &g_orig_djpuctor);
        if (tc) {
            logln("djpuctor: hooked @0x%llx", v);
            /* and its destructor, so a slot never outlives its unit */
            void *td = install_hook_at(DJPUDTOR_DEFAULT, (void *) djpudtor_hook,
                                       (void **) &g_orig_djpudtor);
            logln("djpudtor: %s @0x%lx", td ? "hooked" : "install FAILED",
                  (unsigned long) DJPUDTOR_DEFAULT);
        } else
            logln("djpuctor: install FAILED @0x%llx (identity falls back to ap_deck_of)", v);
    }
    /* gui::LoadState ctor -> per-deck LoadedTrackInfo. One hook, two switches:
     *   g_lti_on:    the native track identity is authoritative (production,
     *                  on unless azstem.nonativeid exists).
     *   g_lti_probe: log LTI beside the path scan's answer (diagnostic).
     * The native route must not turn the diagnostic on: that would keep
     * probe_strings running (~70ms/tick). With both off the hook is not
     * installed. */
    g_lti_probe = dev_flag(LTIPROBE_PATH);
    g_lti_on = !dev_flag("/home/root/settings/azstem.nonativeid"); /* default on */
    if (g_lti_on || g_lti_probe) { /* the hook, needed by either */
        FILE *lf = addr_override_open(LSCTOR_PATH);
        unsigned long long v = LSCTOR_DEFAULT;
        if (lf) {
            unsigned long long x = 0;
            if (fscanf(lf, "%llx", &x) == 1 && x)
                v = x;
            fclose(lf);
        }
        g_lsctor_addr = (uintptr_t) v;
        /* The destructor and LoadedTrackInfo-update hooks first: a LoadState is published only
         * by lsctor_hook, so with all three in place nothing is ever published that the other
         * two cannot see. Without the destructor hook the bracket readers rely on is missing,
         * and without the update hook the track copy is never filled: either one missing means
         * no native identity (unknown), never a guess. */
        void *td = install_hook_at(LSDTOR_DEFAULT, (void *) lsdtor_hook, (void **) &g_orig_lsdtor);
        void *tu = td ? install_hook_at(LTI_UPDATE_DEFAULT, (void *) lti_update_hook,
                                        (void **) &g_orig_lti_update)
                      : NULL;
        void *t = tu ? install_hook_at(g_lsctor_addr, (void *) lsctor_hook, (void **) &g_orig_lsctor)
                     : NULL;
        logln("lsdtor %s @0x%lx, ltiupdate %s @0x%lx", td ? "hooked" : "FAILED",
              (unsigned long) LSDTOR_DEFAULT, tu ? "hooked" : "FAILED",
              (unsigned long) LTI_UPDATE_DEFAULT);
        if (t) {
            logln("lsctor: hooked @0x%llx (pid %d)", v, (int) getpid());
        } else {
            g_lti_probe = 0;
            g_lti_on = 0; /* no hook = no identity: unknown, not a guess */
            logln("lsctor: install FAILED @0x%llx -- native route AND probe OFF", v);
        }
    }
    {
        FILE *vf = addr_override_open(DJPUVT_PATH); /* dev only: it can switch the vtable check off */
        unsigned long long x = 0;
        if (vf) {
            if (fscanf(vf, "%llx", &x) == 1)
                atomic_store_explicit(&g_djpu_vt, (uintptr_t) x, memory_order_relaxed);
            fclose(vf);
            logln("djpuvt: override 0x%llx", x);
        }
    }
    /* Never scan the USB here: this runs before EP147's main, and a walk of a
     * large library (or one with no stems tree) blocks startup to a black
     * screen. The flusher does stems discovery in the background. */
    int rc = install_hook();
    atomic_store_explicit(&g_installed, rc, memory_order_release);
    /* PageBuffer::read: stem injection and the observation ring (pbread_hook). */
    {
        FILE *pf = addr_override_open(PBREAD_PATH);
        unsigned long long v = PBREAD_DEFAULT;
        if (pf) {
            unsigned long long x = 0;
            if (fscanf(pf, "%llx", &x) == 1 && x)
                v = x;
            fclose(pf);
        }
        g_pbread_addr = (uintptr_t) v;
        void *t2 = install_hook_at(g_pbread_addr, (void *) pbread_hook, (void **) &g_orig_pbread);
        if (t2) {
            logln("pbread: hooked @0x%llx", v);
        } else
            logln("pbread: install FAILED @0x%llx", v);
    }
    {
        FILE *ff = addr_override_open(FRADDR_PATH);
        unsigned long long v = FREAD_DEFAULT;
        if (ff) {
            unsigned long long x = 0;
            if (fscanf(ff, "%llx", &x) == 1 && x)
                v = x;
            fclose(ff);
        }
        g_fread_addr = (uintptr_t) v;
        void *t3 = install_hook_at(g_fread_addr, (void *) fread_hook, (void **) &g_orig_fread);
        if (t3) {
            logln("fread: hooked @0x%llx", v);
        } else
            logln("fread: install FAILED @0x%llx", v);
    }
    {
        FILE *of = addr_override_open(OPADDR_PATH);
        unsigned long long v = OPERATE_DEFAULT;
        if (of) {
            unsigned long long x = 0;
            if (fscanf(of, "%llx", &x) == 1 && x)
                v = x;
            fclose(of);
        }
        g_operate_addr = (uintptr_t) v;
        void *t4 =
            install_hook_at(g_operate_addr, (void *) operate_hook, (void **) &g_orig_operate);
        if (t4) {
            logln("operate: hooked @0x%llx", v);
        } else
            logln("operate: install FAILED @0x%llx", v);
    }
    {
        FILE *kff = addr_override_open(KFADDR_PATH);
        unsigned long long v = KEYFAC_DEFAULT;
        if (kff) {
            unsigned long long x = 0;
            if (fscanf(kff, "%llx", &x) == 1 && x)
                v = x;
            fclose(kff);
        }
        g_kf_addr = (uintptr_t) v;
        void *t5 = install_hook_at(g_kf_addr, (void *) kf_hook, (void **) &g_orig_kf);
        if (t5) {
            logln("keyfac: hooked @0x%llx", v);
        } else
            logln("keyfac: install FAILED @0x%llx", v);
    }
    {
        FILE *gf = addr_override_open(FGPRADDR_PATH);
        unsigned long long v = FGPR_SETSRC_DEFAULT;
        if (gf) {
            unsigned long long x = 0;
            if (fscanf(gf, "%llx", &x) == 1 && x)
                v = x;
            fclose(gf);
        }
        g_fgpr_setsrc_addr = (uintptr_t) v;
        void *t6 = install_hook_at(g_fgpr_setsrc_addr, (void *) fgpr_setsrc_hook,
                                   (void **) &g_orig_fgpr_setsrc);
        if (t6) {
            logln("fgpr_setsrc: hooked @0x%llx", v);
        } else
            logln("fgpr_setsrc: install FAILED @0x%llx", v);
    }
    {
        FILE *gr = addr_override_open(FGPRREADADDR_PATH);
        unsigned long long v = FGPRREAD_DEFAULT;
        if (gr) {
            unsigned long long x = 0;
            if (fscanf(gr, "%llx", &x) == 1 && x)
                v = x;
            fclose(gr);
        }
        g_fgpr_read_addr = (uintptr_t) v;
        void *t7 =
            install_hook_at(g_fgpr_read_addr, (void *) fgpr_read_hook, (void **) &g_orig_fgpr_read);
        if (t7) {
            logln("fgpr_read: hooked @0x%llx", v);
        } else
            logln("fgpr_read: install FAILED @0x%llx", v);
    }
    /* ringfill (diagnostic ring probe) and fgpr_op (FGPR deck tag). Installed
     * unconditionally: their flags are re-read every tick, so gating the install
     * on a startup flag would leave a later flag with no hook behind it. Both are
     * pass-through and off the RT path (EP147's buffering thread and the FGPR
     * pre-render thread). */
    {
        FILE *rf = addr_override_open(RINGFILLADDR_PATH);
        unsigned long long v = RINGFILL_DEFAULT;
        if (rf) {
            unsigned long long x = 0;
            if (fscanf(rf, "%llx", &x) == 1 && x)
                v = x;
            fclose(rf);
        }
        g_ringfill_addr = (uintptr_t) v;
        void *t8 =
            install_hook_at(g_ringfill_addr, (void *) ringfill_hook, (void **) &g_orig_ringfill);
        if (t8) {
            logln("ringfill: hooked @0x%llx", v);
        } else
            logln("ringfill: install FAILED @0x%llx", v);
        /* the async pre-render hook (FGPR deck tag) */
        g_fgpr_op_addr = FGPROP_DEFAULT;
        void *t10 =
            install_hook_at(g_fgpr_op_addr, (void *) fgpr_op_hook, (void **) &g_orig_fgpr_op);
        if (t10) {
            logln("fgpr_op: hooked @0x%lx", (unsigned long) g_fgpr_op_addr);
        } else
            logln("fgpr_op: install FAILED @0x%lx", (unsigned long) g_fgpr_op_addr);
    }
    /* every hook is in: EP147's main runs once this constructor returns */
    atomic_store_explicit(&g_hooks_late, 1, memory_order_release);
    pthread_t t;
    if (az_thread_create(&t, NULL, flusher, NULL) == 0)
        pthread_detach(t);
    /* Toggle poller + flusher watchdog. If it fails to start, g_maskpoll_up stays
     * 0 and the flusher reads the toggle itself. */
    {
        pthread_t mt;
        if (az_thread_create(&mt, NULL, maskpoll_thread, NULL) == 0) {
            pthread_detach(mt);
            g_maskpoll_up = 1;
            logln("maskpoll: started (stem toggle read off the flusher, 20Hz)");
        } else
            logln("maskpoll: pthread_create FAILED -- toggle stays on the flusher");
    }
}

#include "sdk_export.c" /* per-deck state for the community mod services */
