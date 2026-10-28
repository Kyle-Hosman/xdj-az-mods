/* SPDX-License-Identifier: GPL-3.0-only */
/* ---- Thread census (diagnostics only) -----------------------------------
 * pbread_hook routes by the TLS deck tag (set by hook_pbi on the render thread),
 * then pb_deck_of(self). Recording which threads render and which read tells
 * "tag never set" apart from "tag set on a thread that never reads".
 * gettid() is cached per thread; everything else is a relaxed add. */
static __thread int g_mytid = 0;
static inline int az_tid(void) {
    if (!g_mytid)
        g_mytid = (int) syscall(SYS_gettid);
    return g_mytid;
}
#define TIDTAB_N 12
struct tidrow {
    _Atomic int tid;
    _Atomic long calls;
    _Atomic long tls_set;
    _Atomic long inj;
    char comm[20];
};                                        /* comm: filled lazily by the flusher only, never on RT */
static struct tidrow g_pbi_tid[TIDTAB_N]; /* threads that run hook_pbi  (the renderers) */
static struct tidrow g_pbr_tid[TIDTAB_N]; /* threads that run pbread_hook (the readers) */
/* ---- FGPR enclosure probe -----------------------------------------------
 * Counts untagged PageBuffer reads inside vs outside a KeyControlFGPR call,
 * keyed by the enclosing instance: the precondition for the FGPR tag below. */
static __thread int g_fgpr_depth = 0;     /* >0 = inside an FGPR call on this thread */
static __thread uintptr_t g_fgpr_ctx = 0; /* the innermost FGPR/SimpleBuffer base */
static _Atomic long g_pbr_in_fgpr = 0;    /* untagged pbreads nested inside an FGPR call */
static _Atomic long g_pbr_no_fgpr = 0;    /* untagged pbreads not inside one (uncoverable) */
#define FGCTX_N 8
struct ctxrow {
    _Atomic uintptr_t self;
    _Atomic long calls;
};
static struct ctxrow g_fgctx[FGCTX_N]; /* which FGPR instances enclose untagged reads */
static inline struct ctxrow *ctxrow_of(uintptr_t self) {
    for (int i = 0; i < FGCTX_N; i++) {
        uintptr_t v = atomic_load_explicit(&g_fgctx[i].self, memory_order_relaxed);
        if (v == self)
            return &g_fgctx[i];
        if (!v) {
            uintptr_t exp = 0;
            if (atomic_compare_exchange_strong_explicit(&g_fgctx[i].self, &exp, self,
                                                        memory_order_relaxed, memory_order_relaxed))
                return &g_fgctx[i];
            if (atomic_load_explicit(&g_fgctx[i].self, memory_order_relaxed) == self)
                return &g_fgctx[i];
        }
    }
    return NULL;
}
/* RT-visible mirror of autokey's off-RT per-deck FGPR map (ak_fgpr_of[]). */
static _Atomic uintptr_t g_ak_fgpr_rt[NDECK];

/* ---- FGPR deck tag (default on; opt-out: azstem.nofgprtag) ----------------------------
 * Nearly all untagged PageBuffer reads under a key shift are nested in
 * an FGPR call whose instance autokey has mapped to a deck, so the deck is set
 * in g_tls_deck around that call. Per call and keyed on the instance, never per
 * thread: one "FGPRKeyConMain" thread serves every deck.
 * Conservative: only sets a deck the per-deck FGPR map names (unmapped leaves
 * pb_deck_of to decide), never overwrites a tag hook_pbi set, and restores the
 * previous value on exit. mydeck < 0 still means passthrough.
 * RT-safe: <= NDECK relaxed atomic loads and two __thread writes. */
#define NOFGPRTAG_PATH \
    "/home/root/settings/azstem.nofgprtag" /* opt-out (default on; azstem.fgprtag                    \
                                              is a no-op) */
static _Atomic int g_fgprtag_on = 0;
static _Atomic long g_fgprtag_hits = 0; /* FGPR calls we tagged a deck for */
static _Atomic long g_fgprtag_miss = 0; /* FGPR calls whose instance maps to no deck */
static inline int fgpr_deck_of(uintptr_t fg) {
    if (!fg)
        return -1;
    for (int d = 0; d < NDECK; d++)
        if (atomic_load_explicit(&g_ak_fgpr_rt[d], memory_order_relaxed) == fg)
            return d;
    return -1;
}
static inline void fg_enter(struct fgscope *sc, uintptr_t self) {
    sc->save_ctx = g_fgpr_ctx;
    sc->save_deck = g_tls_deck;
    g_fgpr_ctx = self;
    g_fgpr_depth++;
    if (atomic_load_explicit(&g_fgprtag_on, memory_order_relaxed) && g_tls_deck < 0) {
        int d = fgpr_deck_of(self);
        if (d >= 0) {
            g_tls_deck = d;
            atomic_fetch_add_explicit(&g_fgprtag_hits, 1, memory_order_relaxed);
        } else
            atomic_fetch_add_explicit(&g_fgprtag_miss, 1, memory_order_relaxed);
    }
}
static inline void fg_leave(struct fgscope *sc) {
    g_fgpr_depth--;
    g_fgpr_ctx = sc->save_ctx;
    g_tls_deck = sc->save_deck;
}

/* ---- Read-ahead ring probe: FUN_00d9ea00 is the ring-fill that pulls the
 * pitched source (operate() @op_caller=0xd9eacc) into the resampler's output
 * ring. param_1 is the SimpleBuffer/FGPR base: mask +0x159840 / +0x159838,
 * capacity +0x159828, ring ptrs +0x164a30 / +0x159808, track length +0x1629a8.
 * Read-only: one-shot capture of the fields, then the original call. It also
 * carries the FGPR deck tag (fg_enter/fg_leave). */
#define RINGFILL_DEFAULT EP_d9ea00
#define RINGFILLADDR_PATH "/home/root/settings/azstem.ringaddr"
typedef void (*ringfill_fn)(long, long, long, long, unsigned);
static ringfill_fn g_orig_ringfill = NULL;
static uintptr_t g_ringfill_addr = 0;
static _Atomic long g_ring_mask1 = 0, g_ring_mask2 = 0, g_ring_cap = 0;
static _Atomic long g_ring_ptr1 = 0, g_ring_ptr2 = 0, g_ring_trklen = 0;
static _Atomic uintptr_t g_ring_base = 0;
static _Atomic int g_ring_captured = 0;
/* Fill-policy fields: L = buffered-up-to (+0x162a28), read cursor (+0x165298),
 * fill cursors (+0x159920/+0x159940), and the FUN_00d9ebc8 amount params
 * (mult +0x1650d8, count0 +0x1650c0, capA +0x1650c8, ratioA +0x162a68). The
 * cursors are re-read every call. */
static _Atomic long g_ring_L = 0, g_ring_read = 0, g_ring_fill1 = 0, g_ring_fill2 = 0;
static _Atomic long g_ring_mult = 0, g_ring_cnt0 = 0, g_ring_capA = 0, g_ring_ratA = 0;
static void ringfill_hook(long p1, long p2, long p3, long p4, unsigned p5) {
    if ((uintptr_t) p1 >= 0x10000 && (uintptr_t) p1 <= 0x0000ffffffffffffUL) {
        char *b = (char *) p1;
        if (!atomic_load_explicit(&g_ring_captured, memory_order_relaxed)) {
            atomic_store_explicit(&g_ring_base, (uintptr_t) p1, memory_order_relaxed);
            atomic_store_explicit(&g_ring_mask1, *(long *) (b + 0x159840), memory_order_relaxed);
            atomic_store_explicit(&g_ring_mask2, *(long *) (b + 0x159838), memory_order_relaxed);
            atomic_store_explicit(&g_ring_cap, *(long *) (b + 0x159828), memory_order_relaxed);
            atomic_store_explicit(&g_ring_ptr1, *(long *) (b + 0x164a30), memory_order_relaxed);
            atomic_store_explicit(&g_ring_ptr2, *(long *) (b + 0x159808), memory_order_relaxed);
            atomic_store_explicit(&g_ring_trklen, *(long *) (b + 0x1629a8), memory_order_relaxed);
            atomic_store_explicit(&g_ring_mult, *(long *) (b + 0x1650d8), memory_order_relaxed);
            atomic_store_explicit(&g_ring_cnt0, *(long *) (b + 0x1650c0), memory_order_relaxed);
            atomic_store_explicit(&g_ring_capA, *(long *) (b + 0x1650c8), memory_order_relaxed);
            atomic_store_explicit(&g_ring_ratA, *(long *) (b + 0x162a68), memory_order_relaxed);
            atomic_store_explicit(&g_ring_captured, 1, memory_order_relaxed);
        }
        /* live cursors (cheap, every call) */
        long readc = *(long *) (b + 0x165298);
        atomic_store_explicit(&g_ring_L, *(long *) (b + 0x162a28), memory_order_relaxed);
        atomic_store_explicit(&g_ring_read, readc, memory_order_relaxed);
        atomic_store_explicit(&g_ring_fill1, *(long *) (b + 0x159920), memory_order_relaxed);
        atomic_store_explicit(&g_ring_fill2, *(long *) (b + 0x159940), memory_order_relaxed);
    }
    {
        struct fgscope sc;
        fg_enter(&sc, (uintptr_t) p1);
        g_orig_ringfill(p1, p2, p3, p4, p5);
        fg_leave(&sc);
    }
}

/* FUN_00d9f320: KeyControlFGPR resample "operate", called by the async buffering
 * thread (FUN_00da2220) for every pre-render. Reads our injected source
 * (+0x162998, PageBuffer::read) at source cursor +0x1598a0, ahead of the
 * playhead. The async loop bounds each wakeup itself (+0x162154 vs
 * +0x162158*+0x162a60). The hook only carries the FGPR deck tag. */
#define FGPROP_DEFAULT EP_d9f320
#define FGPR_SRCCURSOR_OFF 0x1598a0UL
typedef long (*fgpr_op_fn)(void *self, void *a2, void *a3, long a4, long a5, long a6);
static fgpr_op_fn g_orig_fgpr_op = NULL;
static uintptr_t g_fgpr_op_addr = 0;
static long fgpr_op_hook(void *self, void *a2, void *a3, long a4, long a5, long a6) {
    /* No look-ahead cap: stalling here does not change stem-toggle latency
     * (pre-render runs only ~0.12 s ahead) and stalls 3 ms after loads/jumps. */
    {
        struct fgscope sc;
        long r;
        fg_enter(&sc, (uintptr_t) self);
        r = g_orig_fgpr_op(self, a2, a3, a4, a5, a6);
        fg_leave(&sc);
        return r;
    }
}
/* Lock-free claim, benign races: a lost race re-counts under a second row. */
static inline struct tidrow *tidrow(struct tidrow *tab, int tid) {
    for (int i = 0; i < TIDTAB_N; i++) {
        int t = atomic_load_explicit(&tab[i].tid, memory_order_relaxed);
        if (t == tid)
            return &tab[i];
        if (t == 0) {
            int exp = 0;
            if (atomic_compare_exchange_strong_explicit(&tab[i].tid, &exp, tid,
                                                        memory_order_relaxed, memory_order_relaxed))
                return &tab[i];
            if (atomic_load_explicit(&tab[i].tid, memory_order_relaxed) == tid)
                return &tab[i];
        }
    }
    return NULL; /* table full: drop, never block */
}
/* Thread census log (flusher thread): per-thread render/read counts on change
 * or once a minute, plus a TLS verdict. */
/* Thread name from /proc (opens a file: flusher only, once per row). "JUCE ALSA"
 * renders, "FGPRKeyConMain" is KeyControlFGPR's prefetch. */
static void tidrow_name(struct tidrow *r, int tid) {
    if (r->comm[0])
        return;
    char path[64];
    snprintf(path, sizeof path, "/proc/self/task/%d/comm", tid);
    FILE *f = fopen(path, "re");
    if (!f) {
        snprintf(r->comm, sizeof r->comm, "?");
        return;
    }
    if (!fgets(r->comm, sizeof r->comm, f))
        snprintf(r->comm, sizeof r->comm, "?");
    fclose(f);
    for (char *c = r->comm; *c; c++)
        if (*c == '\n') {
            *c = 0;
            break;
        } /* keep spaces: "JUCE ALSA" */
}
static void thread_census(void) {
    static unsigned prev_sig = 0xffffffffu;
    static time_t prev_t = 0;
    unsigned sig = 0;
    int nr = 0, nw = 0;
    long tset = atomic_load_explicit(&g_pbread_tls_set, memory_order_relaxed);
    long tuns = atomic_load_explicit(&g_pbread_tls_unset, memory_order_relaxed);
    for (int i = 0; i < TIDTAB_N; i++) {
        int a = atomic_load_explicit(&g_pbi_tid[i].tid, memory_order_relaxed);
        int b = atomic_load_explicit(&g_pbr_tid[i].tid, memory_order_relaxed);
        if (a)
            nr++;
        if (b)
            nw++;
        sig = sig * 31u + (unsigned) a;
        sig = sig * 31u + (unsigned) b;
    }
    sig = sig * 31u + (unsigned) (tset != 0); /* the verdict flipping is a change */
    time_t now_t = time(NULL);
    if (sig == prev_sig && now_t - prev_t < 60)
        return;
    prev_sig = sig;
    prev_t = now_t;

    /* a thread that both renders and reads separates the two dead cases */
    int shared = 0;
    for (int i = 0; i < TIDTAB_N; i++) {
        int a = atomic_load_explicit(&g_pbi_tid[i].tid, memory_order_relaxed);
        if (!a)
            continue;
        for (int j = 0; j < TIDTAB_N; j++)
            if (atomic_load_explicit(&g_pbr_tid[j].tid, memory_order_relaxed) == a)
                shared = 1;
    }
    const char *verdict;
    if (nw == 0)
        verdict = "NO READS YET (void round -- not a negative)";
    else if (tset == 0)
        verdict = shared ? "TLS DEAD (same thread! tag cleared too early, NOT threading)"
                         : "TLS DEAD (different threads -- pb_deck_of routes 100%)";
    else if (tuns == 0)
        verdict = "TLS LIVE (pb_deck_of routes nothing)";
    else
        verdict = "TLS MIXED -- see the untagged reader rows: those are routed by pb_deck_of";
    char line[512];
    int o = 0;
    o += snprintf(line + o, sizeof line - (size_t) o, "render[");
    for (int i = 0; i < TIDTAB_N; i++) {
        int t = atomic_load_explicit(&g_pbi_tid[i].tid, memory_order_relaxed);
        if (!t)
            continue;
        tidrow_name(&g_pbi_tid[i], t);
        o += snprintf(line + o, sizeof line - (size_t) o, "%d(%s):c%ld/t%ld ", t, g_pbi_tid[i].comm,
                      atomic_load_explicit(&g_pbi_tid[i].calls, memory_order_relaxed),
                      atomic_load_explicit(&g_pbi_tid[i].tls_set, memory_order_relaxed));
    }
    o += snprintf(line + o, sizeof line - (size_t) o, "] read[");
    for (int i = 0; i < TIDTAB_N; i++) {
        int t = atomic_load_explicit(&g_pbr_tid[i].tid, memory_order_relaxed);
        if (!t)
            continue;
        tidrow_name(&g_pbr_tid[i], t);
        o += snprintf(line + o, sizeof line - (size_t) o, "%d(%s):c%ld/tls%ld/inj%ld ", t,
                      g_pbr_tid[i].comm,
                      atomic_load_explicit(&g_pbr_tid[i].calls, memory_order_relaxed),
                      atomic_load_explicit(&g_pbr_tid[i].tls_set, memory_order_relaxed),
                      atomic_load_explicit(&g_pbr_tid[i].inj, memory_order_relaxed));
    }
    snprintf(line + o, sizeof line - (size_t) o, "]");
    /* Report per-window deltas too: cumulative totals keep every read from before
     * a change, so they cannot show it taking effect. */
    {
        static long p_set = 0, p_uns = 0;
        static int p_have = 0;
        long dset = tset - p_set, duns = tuns - p_uns;
        long tot = tset + tuns;
        int pct = tot ? (int) ((tuns * 100 + tot / 2) / tot) : 0;
        long dtot = dset + duns;
        int dpct = dtot ? (int) ((duns * 100 + dtot / 2) / dtot) : 0;
        if (!p_have) {
            p_have = 1;
            dset = tset;
            duns = tuns;
        }
        p_set = tset;
        p_uns = tuns;
        logln("THREADS %s | pbread tls_set=%ld tls_unset=%ld (%d%% via pb_deck_of)"
              " | THIS WINDOW +%ld tagged +%ld untagged (%d%% via pb_deck_of) | %s",
              line, tset, tuns, pct, dset, duns, dpct, verdict);
    }
    /* FGPR enclosure: which FGPR instances enclosed untagged reads, against
     * autokey's per-deck map. */
    {
        char cl[320];
        int co = 0;
        int nctx = 0;
        cl[0] = 0;
        for (int i = 0; i < FGCTX_N; i++) {
            uintptr_t v = atomic_load_explicit(&g_fgctx[i].self, memory_order_relaxed);
            if (!v)
                continue;
            nctx++;
            int deck = -1;
            for (int d = 0; d < NDECK; d++)
                if (atomic_load_explicit(&g_ak_fgpr_rt[d], memory_order_relaxed) == v)
                    deck = d;
            co += snprintf(cl + co, sizeof cl - (size_t) co, "0x%lx=d%d:c%ld ", (unsigned long) v,
                           deck, atomic_load_explicit(&g_fgctx[i].calls, memory_order_relaxed));
        }
        char am[160];
        int ao = 0;
        for (int d = 0; d < NDECK; d++)
            ao += snprintf(
                am + ao, sizeof am - (size_t) ao, "%sd%d=0x%lx", d ? "," : "", d,
                (unsigned long) atomic_load_explicit(&g_ak_fgpr_rt[d], memory_order_relaxed));
        logln("FGPRCTX fgprtag=%d hits=%ld miss=%ld | untagged in_fgpr=%ld no_fgpr=%ld | "
              "contexts=%d [%s] | autokey_map[%s]",
              atomic_load_explicit(&g_fgprtag_on, memory_order_relaxed),
              atomic_load_explicit(&g_fgprtag_hits, memory_order_relaxed),
              atomic_load_explicit(&g_fgprtag_miss, memory_order_relaxed),
              atomic_load_explicit(&g_pbr_in_fgpr, memory_order_relaxed),
              atomic_load_explicit(&g_pbr_no_fgpr, memory_order_relaxed), nctx, cl, am);
    }
}
/* Auto-detect the stem deck's PageBuffer: the one read most with a monotonically
 * advancing source position (actively playing). */
struct pbcand {
    uintptr_t self;
    long long lastpos;
    int advances;
    unsigned seen;
};
static struct pbcand g_pbcand[8];
/* ---- native key-ratio auto-read, RT-facing state --------------------------
 * The flusher publishes each deck's applied KeyControlFGPR ratio (float bits)
 * for the pbread injection. Opt-in; a manual azstem.keyratio always wins. */
static _Atomic int g_autokey_on = 0; /* flag file present (default off) */
static _Atomic int g_key_manual = 0; /* manual azstem.keyratio => override */
static _Atomic int g_ak_bits[NDECK] = {0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000};
static _Atomic int g_ak_anyshift = 0; /* 1 while any mapped deck is keyed */
/* KeyControlFacade event feed (kf_hook -> flusher): facade `this`, first int of
 * its UnitType<int> arg (probably the semitone step), and a sequence counter. */
static _Atomic unsigned long g_ak_kf_seq = 0;
static _Atomic uintptr_t g_ak_kf_self = 0;
static _Atomic int g_ak_kf_sem = 0;
static float g_hann[2048];  /* Hann window LUT (L=2048) for OLA pitch */
#define POS_SAMPLE_OFF 0x18 /* Position field3 = source sample index (verified) */
static _Atomic long long g_inj_start = 0, g_inj_cnt = 0,
                         g_inj_adv = 0;   /* diag: last inject pos / len / advance */
static _Atomic long long g_play_S[NDECK]; /* diag: playhead per deck (vs inj_start = read-ahead) */
/* An ejected deck's transport reads LLONG_MAX, which arithmetic would overflow,
 * so anything past a day of audio is rejected. 0 means "no playhead". */
#define PLAYHEAD_MAX ((long long) AZ_EP147_RATE * 60 * 60 * 24) /* 24h of samples */
static inline int playhead_ok(long long S) {
    return S > 0 && S < PLAYHEAD_MAX;
}
/* Observed playhead per deck, -1 = never observed. Unlike g_play_S[] (0 = sample
 * 0 or unknown, stems-engaged only) it holds 0 too and is published always;
 * LLONG_MAX is filtered at publish, so readers only test `>= 0`. */
static _Atomic long long g_play_obs[NDECK];
#ifdef AZSTEM_PCMCAP
/* Diagnostic only, never shipped: records deck 1's output and playhead per block
 * for offline alignment. Armed by /tmp/azstem.pcmcap.arm; writes
 * /tmp/azstem-pcm.raw (f32 stereo) and .idx (frame, playhead, ns). */
#define PCMCAP_FRAMES (AZ_EP147_RATE * 40)
#define PCMCAP_BLOCKS 40000
static float g_pcm[PCMCAP_FRAMES * 2];
static struct {
    long frame;
    long long play;
    int ns;
    long long t_us;
} g_pcmidx[PCMCAP_BLOCKS];
static _Atomic int g_pcm_state; /* 0 idle, 1 armed, 2 full */
static long g_pcm_n, g_pcm_nb;
static void pcmcap_block(float **chans, int nch, int ns, void *self) {
    if (atomic_load_explicit(&g_pcm_state, memory_order_acquire) != 1)
        return;
    if (g_pcm_n + ns > PCMCAP_FRAMES || g_pcm_nb >= PCMCAP_BLOCKS) {
        atomic_store_explicit(&g_pcm_state, 2, memory_order_release);
        return;
    }
    long long play = -1;
    char *pobs = *(char **) ((char *) self + OFF_POS_PTR);
    if ((uintptr_t) pobs >= 0x10000 && (uintptr_t) pobs <= 0x0000ffffffffffffUL)
        play = *(long *) (pobs + OFF_POS);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_pcmidx[g_pcm_nb].frame = g_pcm_n;
    g_pcmidx[g_pcm_nb].play = play;
    g_pcmidx[g_pcm_nb].ns = ns;
    g_pcmidx[g_pcm_nb].t_us = (long long) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    g_pcm_nb++;
    float *L = chans[0], *R = nch > 1 ? chans[1] : chans[0];
    for (int i = 0; i < ns; i++) {
        g_pcm[(g_pcm_n + i) * 2] = L ? L[i] : 0;
        g_pcm[(g_pcm_n + i) * 2 + 1] = R ? R[i] : 0;
    }
    g_pcm_n += ns;
}
static void pcmcap_tick(void) {
    int st = atomic_load_explicit(&g_pcm_state, memory_order_acquire);
    if (st == 0 && access("/tmp/azstem.pcmcap.arm", F_OK) == 0) {
        g_pcm_n = 0;
        g_pcm_nb = 0;
        atomic_store_explicit(&g_pcm_state, 1, memory_order_release);
        logln("pcmcap: armed");
    } else if (st == 1 && access("/tmp/azstem.pcmcap.arm", F_OK) != 0)
        atomic_store_explicit(&g_pcm_state, 2, memory_order_release); /* early stop */
    else if (st == 2) {
        FILE *f = fopen("/tmp/azstem-pcm.raw", "wbe");
        if (f) {
            fwrite(g_pcm, sizeof(float) * 2, g_pcm_n, f);
            fclose(f);
        }
        f = fopen("/tmp/azstem-pcm.idx", "we");
        if (f) {
            for (long i = 0; i < g_pcm_nb; i++)
                fprintf(f, "%ld %lld %d %lld\n", g_pcmidx[i].frame, g_pcmidx[i].play,
                        g_pcmidx[i].ns, g_pcmidx[i].t_us);
            fclose(f);
        }
        unlink("/tmp/azstem.pcmcap.arm");
        logln("pcmcap: wrote %ld frames, %ld blocks", g_pcm_n, g_pcm_nb);
        atomic_store_explicit(&g_pcm_state, 0, memory_order_release);
    }
}
#endif

static inline int playhead_obs_ok(long long S) {
    return S >= 0 && S < PLAYHEAD_MAX;
}
/* Gap probe: read_pos - playhead per inject, range since the last probestat_write.
 * Racy min/max is fine for a probe. */
static _Atomic long long g_gap_min = 0x7fffffffffffffffLL, g_gap_max = -0x7fffffffffffffffLL,
                         g_gap_absmin = 0x7fffffffffffffffLL;
/* Return address of a read-ahead PageBuffer::read (gap > 5 s). */
static _Atomic uintptr_t g_ra_caller = 0;
/* op_pos = operate()'s requested position (on demand vs prefetch). The pitch
 * buffer's [+0x100 pos, +0x108 limit] span is the pitch stage's read-ahead. */
static _Atomic long long g_op_pos = 0;
static _Atomic long g_pitch_bufpos = 0, g_pitch_buflim = 0;
static _Atomic uintptr_t g_op_caller =
    0; /* caller of operate(): the prefetch layer */

typedef void (*pbi_fn)(void *self, void *buf, void *midi);
static pbi_fn g_orig = NULL; /* trampoline to the real processBlockInternal */

static void hook_pbi(void *self, void *buf, void *midi) {
    /* Tag the thread with the rendering deck before g_orig, so a synchronous
     * FGPR::read -> PageBuffer::read inside the render routes by this per-deck
     * context, which survives the SourceId/unit handoff a few seconds in. */
    {
        int s0 = deck_slot(self);
        int d0 = -1;
        if (s0 >= 0)
            for (int i = 0; i < NDECK; i++)
                if (STEM_SLOT[i] == s0) {
                    d0 = i;
                    break;
                }
        g_tls_deck = d0;
        {
            struct tidrow *r = tidrow(g_pbi_tid, az_tid()); /* who renders */
            if (r) {
                atomic_fetch_add_explicit(&r->calls, 1, memory_order_relaxed);
                if (d0 >= 0)
                    atomic_fetch_add_explicit(&r->tls_set, 1, memory_order_relaxed);
            }
        }
    }
    g_orig(self, buf, midi); /* deck renders its audio into buf */
    g_tls_deck = -1;
    if (!buf)
        return;
    int nch = *(int *) ((char *) buf + OFF_NUMCH);
    int ns = *(int *) ((char *) buf + OFF_NUMSAMP);
    float **chans = *(float ***) ((char *) buf + OFF_CHANS);
    if (!chans || nch < 1 || ns < 1 || ns > (1 << 20))
        return;
    int slot = deck_slot(self);
    if (slot < 0)
        return;
    /* Diagnostic: the rank arrival order would have assigned this slot. */
    if (!atomic_load_explicit(&g_arr_init, memory_order_relaxed)) {
        for (int i = 0; i < MAX_DECKS; i++)
            atomic_store_explicit(&g_arr_rank[i], -1, memory_order_relaxed);
        atomic_store_explicit(&g_arr_init, 1, memory_order_relaxed);
    }
    if (atomic_load_explicit(&g_arr_rank[slot], memory_order_relaxed) < 0)
        atomic_store_explicit(&g_arr_rank[slot],
                              (int) atomic_fetch_add_explicit(&g_arr_next, 1, memory_order_relaxed),
                              memory_order_relaxed);
    /* Stems: map the audio slot to a stem deck. Runs before the mute; gated by
     * azstem.enable. The sync probe runs only for deck 1. */
    int di = -1;
    for (int i = 0; i < NDECK; i++)
        if (STEM_SLOT[i] == slot) {
            di = i;
            break;
        }
    if (di >= 0)
        deck_track_rt(di, self); /* the structural deck -> Track map, inside this render */
    {
        int id = ap_deck_of(self); /* the unit's own number, for the identity census */
        if (atomic_load_explicit(&g_slot_id[slot], memory_order_relaxed) != id)
            atomic_store_explicit(&g_slot_id[slot], id, memory_order_relaxed);
    }
    /* Publish the observed playhead before anything can gate it. */
    if (di >= 0) {
        char *pobs = *(char **) ((char *) self + OFF_POS_PTR);
        if ((uintptr_t) pobs >= 0x10000 && (uintptr_t) pobs <= 0x0000ffffffffffffUL) {
            long long Sobs = *(long *) (pobs + OFF_POS);
            if (playhead_obs_ok(Sobs))
                atomic_store_explicit(&g_play_obs[di], Sobs, memory_order_relaxed);
        }
    }
#ifdef AZSTEM_PCMCAP
    if (di == 0)
        pcmcap_block(chans, nch, ns, self);
#endif
    if (di >= 0 && atomic_load_explicit(&g_enable, memory_order_relaxed)) {
        deck_stems *dk = &g_dk[di];
        unsigned sc = atomic_load_explicit(&dk->ctrl, memory_order_relaxed);
        if ((sc & 0x80u) && dk->stem_frames > 0 && nch >= 1) {
            /* inj: stems come from the pre-DSP pbread injection, so EP147 key-shifts
             * them and this hook must not overwrite the output. */
            int inj = atomic_load_explicit(&g_inject_on, memory_order_relaxed) &&
                      (atomic_load_explicit(&g_target_pbself, memory_order_relaxed) != 0 ||
                       atomic_load_explicit(&g_structmap_on, memory_order_relaxed));
            /* Per-deck cursor PLL: the playhead (int64 at *(deck+OFF_POS_PTR)+OFF_POS)
             * refreshes only every other call, so the cursor advances at an EMA'd
             * rate, locks on each refresh and freezes when stale. Range-checked so a
             * mid-load pointer cannot fault. */
            long S = -1;
            char *p = *(char **) ((char *) self + OFF_POS_PTR);
            if ((uintptr_t) p >= 0x10000 && (uintptr_t) p <= 0x0000ffffffffffffUL)
                S = *(long *) (p + OFF_POS);
            if (atomic_load_explicit(&g_trace_on, memory_order_relaxed) && di == 0) {
                long w = atomic_fetch_add_explicit(&g_trace_w, 1, memory_order_relaxed);
                g_trace[w & (TRACE_N - 1)] = S;
            }
            /* Only a plausible playhead (never an ejected deck's LLONG_MAX). */
            if (playhead_ok(S))
                atomic_store_explicit(&g_play_S[di], S, memory_order_relaxed);
            else if (S <= 0)
                atomic_store_explicit(&g_play_S[di], 0, memory_order_relaxed);
            if (S >= 0) {
                /* sinceA = output frames since the playhead last moved. Any movement
                 * re-syncs with a signed rate (scratch, backspin); frozen = paused. */
                dk->sinceA += ns;
                long dS = (dk->anchorS >= 0) ? (S - dk->anchorS) : 0;
                if (dk->anchorS < 0) { /* first sample: anchor */
                    dk->cur = (double) S;
                    dk->anchorS = S;
                    dk->sinceA = 0;
                } else if (labs(dS) > PLL_GARBAGE) {
                    /* transport pointer swapped mid-load: ignore, incl. the pause timer */

                } else if (dS != 0) {
                    if (labs(dS) < PLL_SEEK_JUMP && dk->sinceA > 0) {
                        double robs = (double) dS / (double) dk->sinceA;
                        double m = robs < 0 ? -robs : robs;
                        if (m > PLL_RATE_MIN && m < PLL_RATE_MAX)
                            dk->rate = 0.6 * dk->rate + 0.4 * robs; /* signed: negative = reverse */
                    }
                    /* else: a seek; snap the cursor, leave the rate alone */
                    dk->cur = (double) S;
                    dk->anchorS = S;
                    dk->sinceA = 0;
                }
                dk->prevS = S;
                dk->paused = (dk->sinceA >= PLL_PAUSE_LATENCY);
                if (!dk->paused) { /* playing: render synced stems */
                    unsigned em = sc & 0x0Fu;
                    double rate = dk->rate;
                    if (em == 0) {
                        /* All stems disabled: the deck must be silent. The pre-DSP
                         * injection writes silence, but the native track can still leak
                         * into chans here, and silence has no pitch to preserve, so force
                         * it post-DSP regardless of inj. */
                        for (int s = 0; s < ns; s++) {
                            chans[0][s] = 0.f;
                            if (nch > 1 && chans[1])
                                chans[1][s] = 0.f;
                        }
                        dk->rd_valid = 0;
                        goto stems_done;
                    }
                    /* Stems reach the output only through the pbread pre-DSP injection.
                     * The rest of this block is the sync probe (azstem.probe), which
                     * correlates native output against the stems: development builds only. */
                    dk->rd_valid = 0;
                stems_done:
                    dk->cur += (double) ns * rate; /* coast the cursor (sinceA counted at top) */
                } else {
                    /* Paused: a stems-engaged paused deck must be silent. EP147's own
                     * paused deck renders a skipping artifact (the playhead wanders),
                     * so write zeros rather than leaving the native buffer. */
                    if (!inj || (sc & 0x0Fu) == 0)
                        for (int s = 0; s < ns; s++) {
                            chans[0][s] = 0.f;
                            if (nch > 1 && chans[1])
                                chans[1][s] = 0.f;
                        }
                }
            }
        }
    }
    /* Drum-roll one-shots: additive, after the stems and before the mute so a
     * muted deck stays silent. Side s is mixed into slot s; deck_slot anchors a
     * DjPlayerUnit to its deck's slot by identity; a unit it cannot identify
     * gets -1 and is left out (deck_slot). */
    /* This deck's source playhead for the beat lock, read here rather than from
     * g_play_S[] because that is maintained only while stems are engaged, and
     * the drum roll must work without stems. -1 = unknown: drum_mix_block
     * free-runs. */
    if (chans[0]) {
        long long dsrc = -1;
        {
            char *pp = *(char **) ((char *) self + OFF_POS_PTR);
            if ((uintptr_t) pp >= 0x10000 && (uintptr_t) pp <= 0x0000ffffffffffffUL) {
                long S2 = *(long *) (pp + OFF_POS);
                if (playhead_ok(S2))
                    dsrc = (long long) S2;
            }
        }
        drum_mix_block(slot, chans, nch, ns, dsrc);
    }

    /* Deck mute: zero this deck's buffer before the mixer reads it (memset only,
     * RT-safe). Peak is measured after, so a muted deck logs peak 0. */
    if (atomic_load_explicit(&g_mute_mask, memory_order_relaxed) & (1u << slot))
        for (int c = 0; c < nch; c++)
            if (chans[c])
                memset(chans[c], 0, (size_t) ns * sizeof(float));
    float pL = 0.f, pR = 0.f;
    const float *L = chans[0];
    const float *R = nch > 1 ? chans[1] : chans[0];
    /* Peak only (no zero-crossing count: g_deck_zcr is published as 0); this
     * runs per block per unit on the RT thread. */
    if (L)
        for (int s = 0; s < ns; s++) {
            float v = L[s];
            if (v < 0)
                v = -v;
            if (v > pL)
                pL = v;
        }
    if (R)
        for (int s = 0; s < ns; s++) {
            float v = R[s];
            if (v < 0)
                v = -v;
            if (v > pR)
                pR = v;
        }
    atomic_store_explicit(&g_deck_zcr[slot], 0.f, memory_order_relaxed);
    atomic_store_explicit(&g_deck_peakL[slot], pL, memory_order_relaxed);
    atomic_store_explicit(&g_deck_peakR[slot], pR, memory_order_relaxed);
    atomic_store_explicit(&g_deck_ch[slot], nch, memory_order_relaxed);
    atomic_store_explicit(&g_deck_ns[slot], ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_gen, 1, memory_order_release);
}

/* The playhead hook_pbi last observed for a deck (samples), -1 = none: the core compares two
 * readings to tell a playing deck from a stopped one. */
long long azstem_play_obs(int deck);
long long azstem_play_obs(int deck) {
    return (deck >= 0 && deck < NDECK) ? atomic_load_explicit(&g_play_obs[deck], memory_order_relaxed)
                                       : -1;
}

/* Stateless granular (Hann-OLA) pitch shift of the enabled stem sum at source
 * position p by ratio r (1.0 = identity). Grains are anchored to absolute
 * source positions (hop HO, window L=2*HO), so no cross-call state is needed,
 * and mutes apply inside the grain read. */
#define GP_HO 1024
#define GP_L 2048
static inline void gpitch(const deck_stems *dk, unsigned em, long nf, double p, double r, float *Lo,
                          float *Ro) {
    long g1 = (long) floor(p / (double) GP_HO);
    double sumL = 0.0, sumR = 0.0;
    for (int gi = 0; gi < 2; gi++) {
        long g = g1 - gi;
        double sg = (double) g * (double) GP_HO;
        double k = p - sg; /* 0..L */
        if (k < 0.0 || k >= (double) GP_L)
            continue;
        float w = g_hann[(int) k];
        double rp = sg + k * r; /* pitch-resampled read position */
        long i0 = (long) floor(rp);
        double fr = rp - (double) i0;
        long a = i0 % nf;
        if (a < 0)
            a += nf;
        long b = a + 1;
        if (b >= nf)
            b = 0;
        float sl = 0.f, sr = 0.f, sgain[NSTEM];
        stem_gains((int) (dk - g_dk), sgain);
        for (int kk = 0; kk < NSTEM; kk++)
            if ((em & (1u << kk)) && dk->stem[kk]) {
                const float *gp = dk->stem[kk];
                sl += sgain[kk] * (gp[a * 2] + (float) fr * (gp[b * 2] - gp[a * 2]));
                sr += sgain[kk] * (gp[a * 2 + 1] + (float) fr * (gp[b * 2 + 1] - gp[a * 2 + 1]));
            }
        sumL += (double) w * sl;
        sumR += (double) w * sr;
    }
    *Lo = (float) sumL;
    *Ro = (float) sumR;
}

/* Native source-read hook (PageBuffer::read). Runs on audio threads: it records
 * observations into lock-free rings for the flusher and, when injection is on,
 * overwrites dst with the owning deck's stem sum. Returns the original's
 * 32-byte Position unchanged (a 32-byte struct return uses x8, matching
 * PageBuffer::read). */
static _Atomic unsigned g_pbread_hits = 0;
static Pos32 pbread_hook(void *self, Float2 *dst, const Pos32 *pos, long long cnt) {
    Pos32 out = g_orig_pbread(self, dst, pos, cnt);
    atomic_fetch_add_explicit(&g_pbread_hits, 1, memory_order_relaxed);
    /* capture the Track vtable once for the structural deck->Track search */
    if (self && !atomic_load_explicit(&g_track_vtable, memory_order_relaxed))
        atomic_store_explicit(&g_track_vtable, *(uintptr_t *) self, memory_order_relaxed);
    /* auto-pbself candidate tracking (benign races; consumed off RT) */
    if (pos && !atomic_load_explicit(&g_target_pbself, memory_order_relaxed)) {
        long long p;
        memcpy(&p, pos->b + POS_SAMPLE_OFF, 8);
        int slot = -1, free = -1;
        for (int i = 0; i < 8; i++) {
            if (g_pbcand[i].self == (uintptr_t) self) {
                slot = i;
                break;
            }
            if (!g_pbcand[i].self && free < 0)
                free = i;
        }
        if (slot < 0 && free >= 0) {
            slot = free;
            g_pbcand[slot].self = (uintptr_t) self;
            g_pbcand[slot].lastpos = p;
            g_pbcand[slot].advances = 0;
            g_pbcand[slot].seen = 0;
        }
        if (slot >= 0) {
            struct pbcand *c = &g_pbcand[slot];
            c->seen++;
            long long d = p - c->lastpos;
            if (d > 0 && d < 200000)
                c->advances++;
            c->lastpos = p;
        }
    }
    if (atomic_load_explicit(&g_pbobs_on, memory_order_relaxed)) {
        unsigned w = atomic_fetch_add_explicit(&g_pbw, 1, memory_order_relaxed) & 127u;
        struct pbobs *e = &g_pbring[w];
        e->self = self;
        if (pos)
            e->in = *pos;
        else
            memset(&e->in, 0, sizeof e->in);
        e->out = out;
        e->cnt = cnt;
        long long n = cnt;
        if (n < 1)
            n = 1;
        if (n > 100000)
            n = 100000;
        if (dst) {
            e->s0L = dst[0].L;
            e->s0R = dst[0].R;
            e->sNL = dst[n - 1].L;
            e->sNR = dst[n - 1].R;
        } else {
            e->s0L = e->s0R = e->sNL = e->sNR = 0.f;
        }
    }
    int inj_on = atomic_load_explicit(&g_inject_on, memory_order_relaxed);
    /* Route by the enclosing processBlock / FGPR deck tag when there is one;
     * otherwise fall back to the structural/content map (pb_deck_of). */
    int tlsd = g_tls_deck;
    if (tlsd >= 0)
        atomic_fetch_add_explicit(&g_pbread_tls_set, 1, memory_order_relaxed);
    else
        atomic_fetch_add_explicit(&g_pbread_tls_unset, 1, memory_order_relaxed);
    if (tlsd < 0) { /* is it coverable? */
        if (g_fgpr_depth > 0) {
            atomic_fetch_add_explicit(&g_pbr_in_fgpr, 1, memory_order_relaxed);
            struct ctxrow *c = ctxrow_of(g_fgpr_ctx);
            if (c)
                atomic_fetch_add_explicit(&c->calls, 1, memory_order_relaxed);
        } else
            atomic_fetch_add_explicit(&g_pbr_no_fgpr, 1, memory_order_relaxed);
    }
    {
        struct tidrow *r = tidrow(g_pbr_tid, az_tid()); /* who reads */
        if (r) {
            atomic_fetch_add_explicit(&r->calls, 1, memory_order_relaxed);
            if (tlsd >= 0)
                atomic_fetch_add_explicit(&r->tls_set, 1, memory_order_relaxed);
            if (inj_on)
                atomic_fetch_add_explicit(&r->inj, 1, memory_order_relaxed);
        }
    }
    int mydeck = (tlsd >= 0) ? tlsd : pb_deck_of((uintptr_t) self);
    if (mydeck >= NDECK)
        mydeck = -1; /* out of range = unknown: never another deck's stems */
    int ak_on = atomic_load_explicit(&g_autokey_on, memory_order_relaxed);
    int ak_manual = atomic_load_explicit(&g_key_manual, memory_order_relaxed);
    if (inj_on && dst && pos) {
        /* Classify: push the original (pre-injection) PCM so the flusher can
         * content-match this PageBuffer to its deck. Only at ratio ~1, since a
         * key-shifted page cannot match the unshifted stems. With the native
         * auto-read the ratio is per deck: a classified buffer is sampled while
         * its own deck is unshifted, an unclassified one only while no deck is. */
        int gate;
        if (ak_on && !ak_manual) {
            if (mydeck >= 0) {
                int rb0 = atomic_load_explicit(&g_ak_bits[mydeck], memory_order_relaxed);
                float r0;
                memcpy(&r0, &rb0, 4);
                gate = (r0 > 0.9995f && r0 < 1.0005f);
            } else {
                gate = !atomic_load_explicit(&g_ak_anyshift, memory_order_relaxed);
            }
        } else {
            int rb0 = atomic_load_explicit(&g_keyratio_bits, memory_order_relaxed);
            float r0;
            memcpy(&r0, &rb0, 4);
            gate = (r0 > 0.9995f && r0 < 1.0005f);
        }
        if (gate) {
            long long p;
            memcpy(&p, pos->b + POS_SAMPLE_OFF, 8);
            /* capture the block peak |L| (an envelope point), not one sample: the
             * flusher correlates it against each deck's precomputed peak envelope
             * (g_env), which tolerates lag; single-sample matching does not converge. */
            float pk = 0.f;
            long long nn = cnt;
            if (nn < 1)
                nn = 1;
            if (nn > 4096)
                nn = 4096;
            for (long long i = 0; i < nn; i++) {
                float a = dst[i].L;
                if (a < 0)
                    a = -a;
                if (a > pk)
                    pk = a;
            }
            unsigned w = atomic_fetch_add_explicit(&g_clsw, 1, memory_order_relaxed) & 127u;
            struct pbcls *c = &g_clsring[w];
            c->self = (uintptr_t) self;
            c->pos = p;
            c->e = pk;
            /* stem-offset capture: original PCM for [base, base+OFS_N), base = the
             * first read after arming. Reads arrive out of order (pre-render and
             * playback), so every overlap is written and marked; the flusher decides
             * when the window is covered. */
            /* tlsd, not mydeck: only the deck tag attributes a read by identity.
             * pb_deck_of's content match cannot tell two decks apart when they play
             * the same track. */
            if (tlsd >= 0 && tlsd == mydeck &&
                atomic_load_explicit(&g_ofs_state[mydeck], memory_order_acquire) == OFS_ARMED) {
                long long b = atomic_load_explicit(&g_ofs_base[mydeck], memory_order_relaxed);
                if (b < 0) {
                    long long want = -1;
                    if (!atomic_compare_exchange_strong(&g_ofs_base[mydeck], &want, p))
                        b = want;
                    else
                        b = p;
                }
                long long i0 = b - p, i1 = b + OFS_N - p; /* dst index range inside the window */
                if (i0 < 0)
                    i0 = 0;
                if (i1 > cnt)
                    i1 = cnt;
                float *cap = g_ofs_cap[mydeck];
                unsigned char *cov = g_ofs_cov[mydeck];
                for (long long i = i0; i < i1; i++) {
                    cap[p + i - b] = dst[i].L + dst[i].R;
                    cov[p + i - b] = 1;
                }
            }
        }
    }
    /* Injection: replace track PCM with the owning deck's stem sum. mydeck < 0
     * (unknown) => passthrough, so a deck never plays another deck's stems. The
     * DSP above PageBuffer then applies tempo/keylock/scratch natively. */
    if (inj_on && mydeck >= 0 && dst && pos) {
        deck_stems *dk = &g_dk[mydeck];
        /* Evict target: recorded only, not a valid eviction object. `self` here
         * is the pitch / time-stretch source view (vtable 0x25f9c00, same object
         * as ts_src), last writer wins; the scheduler-owning PageBuffer is not on
         * this path. Never dereference it on the RT thread. The off-RT
         * do_evict_check validates via /proc/self/mem and refuses it: calling
         * clearBufferedSource on an object whose +0x20 is not a scheduler
         * NULL-derefs and crashes EP147. Eviction stays gated behind
         * azstem.evictkey, so with key shift a toggle plays out over the
         * already pre-rendered pages. */
        atomic_store_explicit(&g_evict_pb[mydeck], (uintptr_t) self, memory_order_relaxed);
        unsigned sc = atomic_load_explicit(&dk->ctrl, memory_order_relaxed);
        long nf = dk->stem_frames;
        if (atomic_load_explicit(&g_enable, memory_order_relaxed) && (sc & 0x80u) && nf > 0) {
            long long start, endp;
            memcpy(&start, pos->b + POS_SAMPLE_OFF, 8);
            memcpy(&endp, out.b + POS_SAMPLE_OFF, 8);
            /* Fill the full requested length `cnt` at the requested source position
             * `start` (Position +0x18). The audible reader (player_basis::PlayerCore ->
             * IReadable) requests rate-scaled positions and lengths and resamples
             * them itself, for both the streaming and cached readers. endp-start would
             * under-fill the cached path. */
            long long n2 = cnt;
            if (n2 < 0)
                n2 = 0;
            if (n2 > (1 << 20))
                n2 = (1 << 20);
            atomic_store_explicit(&g_inj_start, start, memory_order_relaxed);
            {
                long long pS = atomic_load_explicit(&g_play_S[mydeck], memory_order_relaxed);
                long long gap = start - pS, ag = gap < 0 ? -gap : gap;
                if (gap < atomic_load_explicit(&g_gap_min, memory_order_relaxed))
                    atomic_store_explicit(&g_gap_min, gap, memory_order_relaxed);
                if (gap > atomic_load_explicit(&g_gap_max, memory_order_relaxed))
                    atomic_store_explicit(&g_gap_max, gap, memory_order_relaxed);
                if (ag < atomic_load_explicit(&g_gap_absmin, memory_order_relaxed))
                    atomic_store_explicit(&g_gap_absmin, ag, memory_order_relaxed);
                if (gap > 5LL * AZ_EP147_RATE)
                    atomic_store_explicit(&g_ra_caller, (uintptr_t) __builtin_return_address(0),
                                          memory_order_relaxed);
            }
            atomic_store_explicit(&g_inj_cnt, cnt, memory_order_relaxed);
            atomic_store_explicit(&g_inj_adv, endp - start, memory_order_relaxed);
            unsigned em = sc & 0x0Fu;
            int tone = atomic_load_explicit(&g_testtone_on, memory_order_relaxed);
            long off = atomic_load_explicit(&dk->stem_off, memory_order_relaxed);
            float sg[NSTEM];
            stem_gains(mydeck, sg);
            /* Stem FX shaping this page (stem_fx.c); any effect needs the stem sum. */
            sfx_page fxp;
            fxp.mask = 0;
            unsigned fxm = tone ? 0u : sfx_page_prepare(mydeck, start - off, start - off + n2, &fxp);
            int orig = !tone && !fxm && stems_play_original(dk, em, sg);
            int xf = 0; /* frames to blend at a sum <-> original seam */
            unsigned em_old = g_em_prev[mydeck];
            int xf_mask = 0; /* a plain stem toggle: blend old mask -> new mask */
            if (orig != g_orig_prev[mydeck]) {
                xf = (int) (n2 < XF_ORIG ? n2 : XF_ORIG);
                g_orig_prev[mydeck] = orig;
            } else if (!orig && !tone && em != em_old) {
                xf = (int) (n2 < XF_ORIG ? n2 : XF_ORIG);
                xf_mask = 1;
            }
            g_em_prev[mydeck] = em;
            if (orig && !xf)
                n2 = 0; /* all stems on: dst already holds the original */
            for (long long i = 0; i < n2; i++) {
                long long idx = start + i - off; /* source sample -> stem index (measured offset) */
                float L = 0.f, R = 0.f;
                if (tone) {
                    float cyc = 1000.0f * (float) idx / (float) AZ_EP147_RATE;
                    cyc -= floorf(cyc);
                    L = R = 0.30f * sinf(6.2831853f * cyc); /* flat 1kHz; EP147 pitches it */
                } else if ((sc & 0x80u) && nf > 0 && idx >= 0 && idx < nf) {
                    for (int k = 0; k < NSTEM; k++)
                        if ((em & (1u << k)) && dk->stem[k]) {
                            float sL, sR;
                            sfx_stem_px(dk, &fxp, k, nf, idx, &sL, &sR);
                            L += sg[k] * sL;
                            R += sg[k] * sR; /* raw stem sum; EP147 key-shifts it */
                        }
                }
                if (xf && xf_mask) {
                    if (i < xf && (sc & 0x80u) && nf > 0 && idx >= 0 && idx < nf) {
                        float oL = 0.f, oR = 0.f; /* the old mask's sum at the same sample */
                        for (int k = 0; k < NSTEM; k++)
                            if ((em_old & (1u << k)) && dk->stem[k]) {
                                float sL, sR;
                                sfx_stem_px(dk, &fxp, k, nf, idx, &sL, &sR);
                                oL += sg[k] * sL;
                                oR += sg[k] * sR;
                            }
                        float w = (float) (i + 1) / (float) (xf + 1);
                        L = oL + w * (L - oL);
                        R = oR + w * (R - oR);
                    }
                } else if (xf) {
                    if (i >= xf) {
                        if (orig)
                            break;
                    } /* blend done: the original from here on, as written */
                    else {
                        float w = (float) (i + 1) / (float) (xf + 1); /* 0 -> 1 across the blend */
                        if (!orig)
                            w = 1.0f - w; /* sum coming in: fade the original out */
                        L = dst[i].L * w + L * (1.0f - w);
                        R = dst[i].R * w + R * (1.0f - w);
                    }
                }
                dst[i].L = L;
                dst[i].R = R;
            }
        }
    }
    return out;
}

/* mono stem sum (all present stems, regardless of mute: the original has them all) */
static inline float ofs_stem_mono(const deck_stems *dk, long long i) {
    if (i < 0 || i >= dk->stem_frames)
        return 0.f;
    float v = 0.f;
    for (int k = 0; k < NSTEM; k++)
        if (dk->stem[k])
            v += dk->stem[k][i * 2] + dk->stem[k][i * 2 + 1];
    return v;
}

/* Cross-correlate x[0..OFS_N) (EP147's audio for source positions
 * [base, base+OFS_N)) against the stem mono sum, reading stems only below `lim`
 * frames: orig[p] ~ stem[p - lag]. Called from the loader thread (ofsload.c)
 * and the flusher's verifier, so the static work buffers are behind a lock. */
typedef struct {
    long lag;
    double fine, coarse, second;
} ofs_res_t;
static pthread_mutex_t g_ofs_corr_lock = PTHREAD_MUTEX_INITIALIZER;
static inline float ofs_stem_mono_lim(const deck_stems *dk, long long i, long lim) {
    if (i < 0 || i >= lim)
        return 0.f;
    float v = 0.f;
    for (int k = 0; k < NSTEM; k++) {
        const float *g = dk->stem[k];
        if (g)
            v += g[i * 2] + g[i * 2 + 1];
    }
    return v;
}
static void ofs_correlate(const deck_stems *dk, long lim, const float *x, long long base,
                          ofs_res_t *r) {
    pthread_mutex_lock(&g_ofs_corr_lock);
    double xe = 0;
    for (int i = 0; i < OFS_N; i++)
        xe += (double) x[i] * x[i];
    /* Read the stem mix once over the whole search window (per sample per lag
     * would be ~8.6M stem reads, ~0.5 s on the deck's A53).
     * sw[k] = stem mono at source base - OFS_LAGMAX - OFS_PAD + k. */
    enum {
        OFS_PAD = 64,
        NSW = OFS_N + 2 * OFS_LAGMAX + 2 * OFS_PAD
    };
    static float sw[NSW];
    for (int k = 0; k < NSW; k++)
        sw[k] = ofs_stem_mono_lim(dk, base - OFS_LAGMAX - OFS_PAD + k, lim);
    /* coarse: 16x decimated sums, lags in +/-OFS_LAGMAX */
    enum {
        ND = OFS_N / OFS_DEC,
        LD = OFS_LAGMAX / OFS_DEC,
        NS = ND + 2 * LD
    };
    static float xd[ND], sd[NS];
    for (int k = 0; k < ND; k++) {
        float a = 0;
        for (int j = 0; j < OFS_DEC; j++)
            a += x[k * OFS_DEC + j];
        xd[k] = a;
    }
    for (int k = 0; k < NS; k++) {
        float a = 0;
        const float *q = sw + OFS_PAD + k * OFS_DEC;
        for (int j = 0; j < OFS_DEC; j++)
            a += q[j];
        sd[k] = a;
    }
    double xde = 0;
    for (int k = 0; k < ND; k++)
        xde += (double) xd[k] * xd[k];
    double best = -2, second = -2;
    int bl = 0;
    static double cc[2 * LD + 1];
    for (int l = -LD; l <= LD; l++) {
        /* orig[p] ~ stem[p - lag]  =>  xd[k] vs sd[k + LD - l] */
        const float *sp = sd + LD - l;
        float d0 = 0, d1 = 0, d2 = 0, d3 = 0, e0 = 0, e1 = 0, e2 = 0, e3 = 0;
        for (int k = 0; k < ND; k += 4) {
            d0 += xd[k] * sp[k];
            e0 += sp[k] * sp[k];
            d1 += xd[k + 1] * sp[k + 1];
            e1 += sp[k + 1] * sp[k + 1];
            d2 += xd[k + 2] * sp[k + 2];
            e2 += sp[k + 2] * sp[k + 2];
            d3 += xd[k + 3] * sp[k + 3];
            e3 += sp[k + 3] * sp[k + 3];
        }
        double dot = (double) d0 + d1 + d2 + d3, se = (double) e0 + e1 + e2 + e3;
        cc[l + LD] = (se > 0) ? dot / sqrt(xde * se) : 0;
        if (cc[l + LD] > best) {
            best = cc[l + LD];
            bl = l;
        }
    }
    /* The runner-up is the best lag outside the peak's own lobe: the lags reached by
     * walking down from the peak while the correlation keeps falling. Bass-heavy audio
     * has a lobe many bins wide, and its shoulder is the same alignment, not a rival.
     * A peak on the edge of the window may continue past it, so it has no lead. */
    int lo = bl, hi = bl;
    while (lo > -LD && cc[lo - 1 + LD] <= cc[lo + LD])
        lo--;
    while (hi < LD && cc[hi + 1 + LD] <= cc[hi + LD])
        hi++;
    if (bl == -LD || bl == LD)
        second = best;
    else
        for (int l = -LD; l <= LD; l++)
            if ((l < lo || l > hi) && cc[l + LD] > second)
                second = cc[l + LD];
    /* fine: full rate around the coarse peak; stem[base + i - lag] = sw[i - lag + OFS_LAGMAX +
     * OFS_PAD]
     * (|lag| <= OFS_LAGMAX + 2*OFS_DEC <= OFS_LAGMAX + OFS_PAD keeps the index in range) */
    double fbest = -2;
    long flag = (long) bl * OFS_DEC;
    for (long lag = (long) bl * OFS_DEC - 2 * OFS_DEC; lag <= (long) bl * OFS_DEC + 2 * OFS_DEC;
         lag++) {
        const float *sp = sw + OFS_LAGMAX + OFS_PAD - lag;
        float d0 = 0, d1 = 0, d2 = 0, d3 = 0, e0 = 0, e1 = 0, e2 = 0, e3 = 0;
        for (int i = 0; i < OFS_N; i += 4) {
            d0 += x[i] * sp[i];
            e0 += sp[i] * sp[i];
            d1 += x[i + 1] * sp[i + 1];
            e1 += sp[i + 1] * sp[i + 1];
            d2 += x[i + 2] * sp[i + 2];
            e2 += sp[i + 2] * sp[i + 2];
            d3 += x[i + 3] * sp[i + 3];
            e3 += sp[i + 3] * sp[i + 3];
        }
        double dot = (double) d0 + d1 + d2 + d3, se = (double) e0 + e1 + e2 + e3;
        double c = (se > 0) ? dot / sqrt(xe * se) : 0;
        if (c > fbest) {
            fbest = c;
            flag = lag;
        }
    }
    r->lag = flag;
    r->fine = fbest;
    r->coarse = best;
    r->second = second;
    pthread_mutex_unlock(&g_ofs_corr_lock);
}
#include "ofsload.c" /* the load-time measurement, the one that is applied */

/* Runtime stem-offset verifier (flusher; opt-in azstem.stemofs.verify): measures
 * EP147's real audio while it plays and logs it beside the load-time offset. It
 * never writes stem_off, since moving stems mid-play is an audible cut. Without
 * the flag the RT capture is never armed. Skips a deck with a loader in flight:
 * the loader and stems_tick (this thread) are the only callers of free_stems,
 * so the buffers cannot be freed under the correlation. */
#define STEMOFS_VERIFY_PATH "/home/root/settings/azstem.stemofs.verify"
static void ofs_tick(void) {
    if (!dev_flag(STEMOFS_VERIFY_PATH))
        return;
    int nooff = dev_flag(NOSTEMOFS_PATH);
    atomic_store_explicit(&g_nostemofs, nooff, memory_order_relaxed);
    for (int d = 0; d < NDECK; d++) {
        deck_stems *dk = &g_dk[d];
        unsigned ep = atomic_load_explicit(&g_ofs_epoch[d], memory_order_acquire);
        int st = atomic_load_explicit(&g_ofs_state[d], memory_order_acquire);
        if (atomic_load_explicit(&g_stem_loading[d], memory_order_relaxed) || dk->stem_frames <= 0)
            continue;
        if (st == OFS_IDLE) {
            if (g_ofs_armed_epoch[d] != ep) {
                g_ofs_armed_epoch[d] = ep;
                g_ofs_tries[d] = 0;
            }
            if (g_ofs_tries[d] >= OFS_TRIES)
                continue; /* unknown for this load: stays 0 */
            atomic_store_explicit(&g_ofs_base[d], -1, memory_order_relaxed);
            atomic_store_explicit(&g_ofs_next[d], 0, memory_order_relaxed);
            memset(g_ofs_cov[d], 0, OFS_N);
            g_ofs_armticks[d] = 0;
            atomic_store_explicit(&g_ofs_state[d], OFS_ARMED, memory_order_release);
            logln("STEMOFS d%d '%s': armed (try %d)", d, dk->loaded_track, g_ofs_tries[d] + 1);
            continue;
        }
        if (st == OFS_ARMED && g_ofs_armed_epoch[d] == ep) {
            long n = 0;
            for (int i = 0; i < OFS_N; i++)
                n += g_ofs_cov[d][i];
            atomic_store_explicit(&g_ofs_next[d], n, memory_order_relaxed);
            /* a few never-read samples stay 0 and barely move the peak */
            if (n >= OFS_N - OFS_N / 256) {
                atomic_store_explicit(&g_ofs_state[d], OFS_CAPTURED, memory_order_release);
                st = OFS_CAPTURED;
            }
            /* The timeout clock starts at the first captured sample, so a window
             * armed while stopped does not time out shortly after PLAY. */
            else if (n == 0)
                g_ofs_armticks[d] = 0;
            else if (++g_ofs_armticks[d] > OFS_ARM_TICKS && n >= OFS_N / 10 * 9) {
                /* >=90% covered but never complete: analyze anyway; the holes are 0
                 * and the confidence thresholds still decide */
                atomic_store_explicit(&g_ofs_state[d], OFS_CAPTURED, memory_order_release);
                st = OFS_CAPTURED;
            } else if (g_ofs_armticks[d] > OFS_ARM_TICKS) {
                {
                    g_ofs_tries[d]++;
                    logln("STEMOFS d%d try %d: window covered %ld/%d in %ds, re-arming", d,
                          g_ofs_tries[d], n, OFS_N, OFS_ARM_TICKS / 5);
                }
                atomic_store_explicit(&g_ofs_state[d], OFS_IDLE, memory_order_release);
                continue;
            }
        }
        if (st != OFS_CAPTURED || g_ofs_armed_epoch[d] != ep)
            continue;
        long t0 = ms_now();
        long long base = atomic_load_explicit(&g_ofs_base[d], memory_order_relaxed);
        const float *x = g_ofs_cap[d];
        double xe = 0;
        for (int i = 0; i < OFS_N; i++)
            xe += (double) x[i] * x[i];
        g_ofs_tries[d]++;
        if (xe / OFS_N < 1e-6) {
            logln("STEMOFS d%d try %d: base=%lld too quiet (rms=%.5f), re-arming", d,
                  g_ofs_tries[d], base, sqrt(xe / OFS_N));
            atomic_store_explicit(&g_ofs_state[d], OFS_IDLE, memory_order_release);
            continue;
        }
        ofs_res_t rr;
        ofs_correlate(dk, dk->stem_frames, x, base, &rr);
        long flag = rr.lag;
        double fbest = rr.fine, best = rr.coarse, second = rr.second;
        long ms = ms_now() - t0;
        int ok = fbest >= OFS_MINCORR && best - second >= OFS_MARGIN;
        if (atomic_load_explicit(&g_ofs_epoch[d], memory_order_acquire) != ep) {
            logln("STEMOFS d%d: track changed during analysis, discarded", d);
            continue;
        }
        if (ok) {
            g_ofs_meas[d] = flag;
            atomic_store_explicit(&g_ofs_state[d], OFS_DONE, memory_order_release);
        } else
            atomic_store_explicit(&g_ofs_state[d], OFS_IDLE, memory_order_release);
        {
            long ld = atomic_load_explicit(&dk->stem_off, memory_order_relaxed);
            /* ofs_correlate reads the raw stem buffers, so `flag` is the total offset:
             * a correct load-time offset equals it exactly. */
            logln("STEMOFS-VERIFY d%d '%s' try %d: base=%lld runtime lag=%ld (%.1f ms) fine=%.3f "
                  "coarse=%.3f runnerup=%.3f | load-time applied=%ld -> %s [%ldms]",
                  d, dk->loaded_track, g_ofs_tries[d], base, flag, flag / 44.1, fbest, best, second,
                  ld, !ok ? "not confident" : (flag == ld ? "MATCH" : "MISMATCH"), ms);
        }
    }
}

static void pbclassify(void) {
/* Off-RT (flusher): drain the classification ring and content-match each active
 * PageBuffer to its owning deck (g_pb_self/g_pb_deck, read by the RT injection).
 * Correlates the original PCM's per-read peak envelope against each deck's
 * precomputed peak envelope (g_env, ~205 frames/bucket). Envelopes tolerate small
 * lag errors and the mix-vs-stem-sum differences that defeat sample-level
 * correlation, yet still tell tracks apart. A small lag search in bucket units
 * absorbs the encoder delay. The rolling window lets ownership flip when a
 * PageBuffer is reused for another deck's track. */
#define CLS_NLAG 33   /* lag taps in envelope buckets; li -> bucket offset (li - CLS_NLAG/2) */
#define CLS_LAGSTEP 1 /* one bucket (~205 frames, ~4.6ms) per tap; +-16 buckets (~+-75ms) */
    struct clsacc {
        uintptr_t self;
        double dot[NDECK][CLS_NLAG];
        double normS[NDECK][CLS_NLAG];
        double normO;
        unsigned n;
        int idle;
    };
    static struct clsacc acc[NPB];
    unsigned w = atomic_load_explicit(&g_clsw, memory_order_acquire);
    if ((unsigned) (w - g_clsr) > 128u)
        g_clsr = w - 128u; /* ring wrapped: keep recent */
    int drained = 0;
    while (g_clsr != w && drained < 128) {
        struct pbcls s = g_clsring[g_clsr & 127u];
        g_clsr++;
        drained++;
        if (!s.self)
            continue;
        int a = -1, freea = -1;
        for (int i = 0; i < NPB; i++) {
            if (acc[i].self == s.self) {
                a = i;
                break;
            }
            if (!acc[i].self && freea < 0)
                freea = i;
        }
        if (a < 0 && freea >= 0) {
            a = freea;
            memset(&acc[a], 0, sizeof acc[a]);
            acc[a].self = s.self;
        }
        if (a < 0)
            continue; /* table full; skip */
        struct clsacc *c = &acc[a];
        c->idle = 0;
        double e = (double) s.e; /* captured original-block peak|L| */
        c->normO += e * e;
        for (int d = 0; d < NDECK; d++) {
            deck_stems *dk = &g_dk[d];
            long nf = dk->stem_frames;
            if (nf <= 0 || !atomic_load_explicit(&g_env_ready[d], memory_order_relaxed))
                continue;
            long long b0 =
                (long long) ((double) s.pos * (double) AZW_N / (double) nf); /* envelope bucket */
            for (int li = 0; li < CLS_NLAG; li++) {
                long long b = b0 + (long long) (li - CLS_NLAG / 2) * CLS_LAGSTEP;
                if (b < 0 || b >= AZW_N)
                    continue;
                /* full-mix envelope proxy: sum of the 4 stems' peak at this bucket */
                double se =
                    (double) g_env[d][0][b] + g_env[d][1][b] + g_env[d][2][b] + g_env[d][3][b];
                c->dot[d][li] += e * se;
                c->normS[d][li] += se * se;
            }
        }
        c->n++;
    }
    /* decide on any accumulator with enough samples (rolling ~48-sample windows) */
    for (int i = 0; i < NPB; i++) {
        struct clsacc *c = &acc[i];
        if (!c->self)
            continue;
        if (c->n < 48)
            continue;
        if (c->normO < 1e-4) {
            c->normO = 0;
            c->n = 0;
            for (int d = 0; d < NDECK; d++)
                for (int li = 0; li < CLS_NLAG; li++) {
                    c->dot[d][li] = 0;
                    c->normS[d][li] = 0;
                }
            continue;
        } /* silent window */
        int bestd = -1, bestlag = 0;
        double best = -2, second = -2;
        for (int d = 0; d < NDECK; d++) {
            double dcos = -2;
            int dli = CLS_NLAG / 2;
            for (int li = 0; li < CLS_NLAG; li++) {
                double cos = c->dot[d][li] / (sqrt(c->normO * c->normS[d][li]) + 1e-12);
                if (cos > dcos) {
                    dcos = cos;
                    dli = li;
                }
            }
            if (dcos > best) {
                second = best;
                best = dcos;
                bestd = d;
                bestlag = (dli - CLS_NLAG / 2) * CLS_LAGSTEP;
            } else if (dcos > second) {
                second = dcos;
            }
        }
        int clear = (bestd >= 0) && (best > 0.70) && (best > second + 0.15);
        /* locate this self's mapping slot */
        int m = -1, freem = -1;
        for (int j = 0; j < NPB; j++) {
            if ((uintptr_t) atomic_load_explicit(&g_pb_self[j], memory_order_relaxed) == c->self) {
                m = j;
                break;
            }
            if (!atomic_load_explicit(&g_pb_self[j], memory_order_relaxed) && freem < 0)
                freem = j;
        }
        /* Sticky mapping: a wrong lock would need cos > 0.7 against a different
         * track, so once mapped, hold through quiet or misaligned windows and leave
         * only on a clear rival or a persistent mismatch (no flicker). */
        if (m < 0) {
            if (clear && freem >= 0) { /* first positive lock */
                m = freem;
                atomic_store_explicit(&g_pb_self[m], c->self, memory_order_relaxed);
                atomic_store_explicit(&g_pb_deck[m], bestd, memory_order_relaxed);
                g_pb_miss[m] = 0;
                logln("pbmap: self=%p -> deck %d (cos=%.3f 2nd=%.3f lagbkt=%d)", (void *) c->self,
                      bestd, best, second, bestlag);
            }
        } else {
            int curdeck = atomic_load_explicit(&g_pb_deck[m], memory_order_relaxed);
            if (bestd == curdeck && best > 0.45) {
                g_pb_miss[m] = 0;
            }                                     /* still ours */
            else if (clear && bestd != curdeck) { /* a rival deck clearly wins */
                atomic_store_explicit(&g_pb_deck[m], bestd, memory_order_relaxed);
                g_pb_miss[m] = 0;
                logln("pbmap: self=%p REMAP -> deck %d (cos=%.3f 2nd=%.3f)", (void *) c->self,
                      bestd, best, second);
            } else if (++g_pb_miss[m] > 12) { /* persistent mismatch => track stopped/changed */
                atomic_store_explicit(&g_pb_deck[m], -1, memory_order_relaxed);
                atomic_store_explicit(&g_pb_self[m], 0, memory_order_relaxed);
                g_pb_miss[m] = 0;
                logln("pbmap: self=%p UNMAPPED (cos=%.3f)", (void *) c->self, best);
            }
        }
        /* reset the rolling window */
        c->normO = 0;
        c->n = 0;
        for (int d = 0; d < NDECK; d++)
            for (int li = 0; li < CLS_NLAG; li++) {
                c->dot[d][li] = 0;
                c->normS[d][li] = 0;
            }
    }
    /* Age out accumulators and mappings for PageBuffers no longer read. While
     * any deck is keyed its pages cannot be sampled (pbread_hook's gate), so hold
     * mappings ~30 min instead of ~40 s, or stems would drop out on a sustained
     * key shift. A dead buffer still ages out, just slower. */
    int idle_lim = atomic_load_explicit(&g_ak_anyshift, memory_order_relaxed) ? 9000 : 200;
    for (int i = 0; i < NPB; i++) {
        if (acc[i].self) {
            if (++acc[i].idle > idle_lim) { /* ~40s at 200ms */
                uintptr_t dead = acc[i].self;
                for (int j = 0; j < NPB; j++)
                    if ((uintptr_t) atomic_load_explicit(&g_pb_self[j], memory_order_relaxed) ==
                        dead) {
                        atomic_store_explicit(&g_pb_self[j], 0, memory_order_relaxed);
                        atomic_store_explicit(&g_pb_deck[j], -1, memory_order_relaxed);
                    }
                memset(&acc[i], 0, sizeof acc[i]);
            }
        }
    }
}

/* Off-RT: mirror the pbread observation ring to the log while azstem.pbobs is
 * present. Also refreshes the injection-related flag files. */
static void pbobs_flush(void) {
    /* Flag/config files are re-read once a second, not every 200 ms tick; the
     * ring flush after the block still runs every tick. */
    static unsigned cfg_tick;
    if ((cfg_tick++ % 5u) == 0) {
        /* Injection is the only route (no opt-out); kept as an atomic for the
         * readers that gate on it. */
        atomic_store_explicit(&g_inject_on, 1, memory_order_relaxed);
        atomic_store_explicit(&g_testtone_on, dev_flag(TESTTONE_PATH),
                              memory_order_relaxed);
        atomic_store_explicit(&g_fgprobs_on, dev_flag(FGPROBS_PATH), memory_order_relaxed);
        {
            /* A PageBuffer pointer typed into a file: dev only, like the hook-address
             * overrides. The release build never reads it (g_target_pbself stays 0). */
            FILE *pf = addr_override_open(PBSELF_PATH);
            unsigned long long v = 0;
            if (pf) {
                if (fscanf(pf, "%llx", &v) != 1)
                    v = 0;
                fclose(pf);
            }
            atomic_store_explicit(&g_target_pbself, (uintptr_t) v, memory_order_relaxed);
        }
        {
            FILE *kf = dev_input_open(KEYRATIO_PATH);
            float rr = 1.0f;
            if (kf) {
                atomic_store_explicit(
                    &g_key_manual, 1,
                    memory_order_relaxed); /* manual wins over the native auto-read */
                if (fscanf(kf, "%f", &rr) != 1 || rr < 0.25f || rr > 4.0f)
                    rr = 1.0f;
                fclose(kf);
                int rb;
                memcpy(&rb, &rr, 4);
                atomic_store_explicit(&g_keyratio_bits, rb, memory_order_relaxed);
            } else {
                atomic_store_explicit(&g_key_manual, 0, memory_order_relaxed);
                int rb = 0x3f800000;
                atomic_store_explicit(&g_keyratio_bits, rb, memory_order_relaxed);
            }
        }
        atomic_store_explicit(&g_pbobs_on, dev_flag(PBOBS_PATH), memory_order_relaxed);
    } /* end of the once-a-second flag block */
    int on = atomic_load_explicit(&g_pbobs_on, memory_order_relaxed);
    unsigned w = atomic_load_explicit(&g_pbw, memory_order_acquire);
    if (!on) {
        g_pbr = w;
        return;
    }
    int emitted = 0;
    while (g_pbr != w && emitted < 12) {
        struct pbobs *e = &g_pbring[g_pbr & 127u];
        const long long *pin = (const long long *) e->in.b;
        const long long *pout = (const long long *) e->out.b;
        logln("PBREAD self=%p cnt=%lld in[%lld %lld %lld %lld] out[%lld %lld %lld %lld] "
              "s0=%.4f,%.4f sN=%.4f,%.4f",
              e->self, e->cnt, pin[0], pin[1], pin[2], pin[3], pout[0], pout[1], pout[2], pout[3],
              e->s0L, e->s0R, e->sNL, e->sNR);
        g_pbr++;
        emitted++;
    }
    if ((unsigned) (w - g_pbr) > 128u)
        g_pbr = w; /* ring wrapped: skip ahead */
}

/* ---- Native stem key-shift: proxy the PitchSimpleCrossFade source (+0x50) ---- *
 * PitchSimpleCrossFade (vtable 0x27bb150, operate 0xdabb48) reads its source object
 * at +0x50 via the source's vtable slot +0x10 and pitches by the ratio at +0x80.
 * EP147 repoints +0x50 at the cache handoff, so +0x50 is substituted with a proxy
 * wrapping the real source: read(+0x10) calls the real read (Position + buffer),
 * then overwrites the samples with the deck's raw stem sum for EP147 to pitch.
 * Every other vtable slot is a naked trampoline that sets x0 = proxy->real and
 * tail-jumps to the real source's same slot (touches only x0/x16, preserves
 * x1-x7 and the x8 struct return), so the rest of the interface is the real
 * source's. Gated by azstem.nativekey. Deck 0 only, so development builds only
 * (operate_hook compiles it out of release). */
struct stemsrc {
    void *vtbl;
    void *real;
    int deck;
}; /* real @ +8 (see trampoline) */
/* clang-format off */
/* Each trampoline is assembled at file scope: GCC 7 has no `naked` on AArch64, so a C body
 * would get a compiler-made prologue and epilogue around the asm. The CFI gives each one an
 * unwind entry (no frame: the return address stays in x30). */
#define DELEG_TRAMP(i, off)                                                                        \
    __attribute__((visibility("hidden"))) void deleg_tramp_##i(void);                              \
    __asm__(".pushsection .text\n\t.p2align 3\n\t"                                                 \
            ".globl deleg_tramp_" #i "\n\t.hidden deleg_tramp_" #i "\n\t"                          \
            ".type deleg_tramp_" #i ", %function\n"                                                \
            "deleg_tramp_" #i ":\n\t.cfi_startproc\n\t"                                             \
            "ldr x0, [x0, #8]\n\t"          /* x0 = proxy->real */                                \
            "ldr x16, [x0]\n\t"             /* x16 = *real (real vtable) */                       \
            "ldr x16, [x16, #" #off "]\n\t" /* x16 = real_vtable[off] */                          \
            "br  x16\n\t"                                                                          \
            ".cfi_endproc\n\t.size deleg_tramp_" #i ", .-deleg_tramp_" #i "\n\t.popsection");
DELEG_TRAMP(0, 0)
DELEG_TRAMP(1, 8)
DELEG_TRAMP(2, 16)
DELEG_TRAMP(3, 24)
DELEG_TRAMP(4, 32)
DELEG_TRAMP(5, 40)
DELEG_TRAMP(6, 48)
DELEG_TRAMP(7, 56)
DELEG_TRAMP(8, 64)
DELEG_TRAMP(9, 72)
DELEG_TRAMP(10, 80)
DELEG_TRAMP(11, 88)
DELEG_TRAMP(12, 96)
DELEG_TRAMP(13, 104)
DELEG_TRAMP(14, 112)
DELEG_TRAMP(15, 120)
DELEG_TRAMP(16, 128)
DELEG_TRAMP(17, 136)
DELEG_TRAMP(18, 144)
DELEG_TRAMP(19, 152)
DELEG_TRAMP(20, 160)
DELEG_TRAMP(21, 168)
DELEG_TRAMP(22, 176)
DELEG_TRAMP(23, 184)
DELEG_TRAMP(24, 192)
DELEG_TRAMP(25, 200)
DELEG_TRAMP(26, 208)
DELEG_TRAMP(27, 216)
DELEG_TRAMP(28, 224)
DELEG_TRAMP(29, 232)
DELEG_TRAMP(30, 240)
DELEG_TRAMP(31, 248)
static Pos32 stemsrc_read(void *self, Float2 *dst, const Pos32 *pos, long long cnt);
/* unused in release, where operate_hook compiles the substitution out */
__attribute__((unused)) static void *g_stemsrc_vtbl[32] = {
    (void *) deleg_tramp_0,  (void *) deleg_tramp_1,  (void *) stemsrc_read,
    (void *) deleg_tramp_3,  (void *) deleg_tramp_4,  (void *) deleg_tramp_5,
    (void *) deleg_tramp_6,  (void *) deleg_tramp_7,  (void *) deleg_tramp_8,
    (void *) deleg_tramp_9,  (void *) deleg_tramp_10, (void *) deleg_tramp_11,
    (void *) deleg_tramp_12, (void *) deleg_tramp_13, (void *) deleg_tramp_14,
    (void *) deleg_tramp_15, (void *) deleg_tramp_16, (void *) deleg_tramp_17,
    (void *) deleg_tramp_18, (void *) deleg_tramp_19, (void *) deleg_tramp_20,
    (void *) deleg_tramp_21, (void *) deleg_tramp_22, (void *) deleg_tramp_23,
    (void *) deleg_tramp_24, (void *) deleg_tramp_25, (void *) deleg_tramp_26,
    (void *) deleg_tramp_27, (void *) deleg_tramp_28, (void *) deleg_tramp_29,
    (void *) deleg_tramp_30, (void *) deleg_tramp_31};
/* clang-format on */
__attribute__((unused)) static struct stemsrc g_ssrc = {0};
static _Atomic int g_nativekey_subst = 0;
/* read(+0x10): real read for Position/buffer, then overwrite with the deck's stem sum. */
static Pos32 stemsrc_read(void *self, Float2 *dst, const Pos32 *pos, long long cnt) {
    struct stemsrc *s = (struct stemsrc *) self;
    void *real = s->real;
    pbread_fn rr = (pbread_fn) (*(void **) ((*(char **) real) + 0x10));
    Pos32 out = rr(real, dst, pos, cnt);
    int deck = s->deck;
    if (deck >= 0 && deck < NDECK && dst && pos) {
        deck_stems *dk = &g_dk[deck];
        unsigned sc = atomic_load_explicit(&dk->ctrl, memory_order_relaxed);
        long nf = dk->stem_frames;
        if (atomic_load_explicit(&g_enable, memory_order_relaxed) && (sc & 0x80u) && nf > 0) {
            long long start, endp;
            memcpy(&start, pos->b + POS_SAMPLE_OFF, 8);
            memcpy(&endp, out.b + POS_SAMPLE_OFF, 8);
            long long n2 = endp - start;
            if (n2 < 0)
                n2 = 0;
            if (n2 > cnt)
                n2 = cnt;
            unsigned em = sc & 0x0Fu;
            int tone = atomic_load_explicit(&g_testtone_on, memory_order_relaxed);
            float sg[NSTEM];
            stem_gains(deck, sg);
            if (!tone && stems_play_original(dk, em, sg))
                n2 = 0; /* all stems on: the original, as in pbread_hook */
            for (long long i = 0; i < n2; i++) {
                long long idx = start + i;
                float L = 0.f, R = 0.f;
                if (tone) {
                    float cyc = 1000.0f * (float) idx / (float) AZ_EP147_RATE;
                    cyc -= floorf(cyc);
                    L = R = 0.30f * sinf(6.2831853f * cyc);
                } else if ((sc & 0x80u) && nf > 0 && idx >= 0 && idx < nf) {
                    for (int k = 0; k < NSTEM; k++)
                        if ((em & (1u << k)) && dk->stem[k]) {
                            const float *g = dk->stem[k];
                            L += sg[k] * g[idx * 2];
                            R += sg[k] * g[idx * 2 + 1];
                        }
                }
                dst[i].L = L;
                dst[i].R = R;
            }
        }
    }
    return out;
}
typedef Pos32 (*operate_fn)(void *self, const Pos32 *pos, Float2 *dst, long long smp);
static operate_fn g_orig_operate = NULL;
static _Atomic int g_operate_hits = 0; /* diag: PitchSimpleCrossFade::operate call count */
static uintptr_t g_operate_addr = 0;
static _Atomic int g_opobs_on = 0;
/* live key ratio, keyed by the Position's TrackID (field0, field1) so it can be
 * matched to the injecting PageBuffer. */
static _Atomic(unsigned long long) g_key_tid0 = 0, g_key_tid1 = 0;
static _Atomic int g_key_ratio_bits = 0x3f800000; /* float 1.0 as int bits */
/* Pitch-source / TSRB probe state: operate_hook publishes the live
 * PitchSimpleCrossFade `this` for tsrb_probe. */
static _Atomic uintptr_t g_pitch_self = 0;
static _Atomic long g_ts_src = 0, g_ts_vtbl = 0, g_ts_buf = 0, g_ts_ws = 0, g_ts_fill = 0;
static _Atomic int g_ts_cap = 0;
static _Atomic unsigned g_evict_req = 0; /* bit d: deck d's enabled-mask changed */
static _Atomic int g_evict_calls = 0;    /* diag: clearBufferedSource invocations */
/* SourceId for the evict: the pitch Position's +0x08/+0x10 (not +0x00, which is a
 * header word). clearBufferedSource reads it as {u64,u64}. */
static _Atomic(unsigned long long) g_evict_sid0 = 0, g_evict_sid1 = 0;
/* azstem.nativekey, published by the flusher for operate_hook (an EP147 audio
 * prefetch thread, which must not make syscalls). */
__attribute__((unused)) static _Atomic int g_nativekey_on = 0;
struct opobs {
    void *self;
    float ratio;
    unsigned long long t0, t1;
};
static struct opobs g_opring[64];
static _Atomic unsigned g_opw = 0;
static unsigned g_opr = 0;

static Pos32 operate_hook(void *self, const Pos32 *pos, Float2 *dst, long long smp) {
    /* Native key-shift: substitute PitchSimpleCrossFade's source (+0x50) with our
     * proxy so EP147 pitches our stems. Re-asserted every call, since EP147
     * repoints it at the cache handoff. The flag is read by the flusher
     * (opobs_flush); this thread only loads it, so it makes no syscall. */
    Pos32 out = g_orig_operate(self, pos, dst, smp);
    atomic_fetch_add_explicit(&g_operate_hits, 1, memory_order_relaxed);
    atomic_store_explicit(&g_pitch_self, (uintptr_t) self,
                          memory_order_relaxed); /* off-RT TSRB probe follows self+0x50 */
    float ratio = *(const float *) ((const char *) self + 0x80);
    unsigned long long t0 = 0, t1 = 0;
    if (pos) {
        memcpy(&t0, pos->b + 0, 8);
        memcpy(&t1, pos->b + 8, 8);
    }
    /* publish the ratio+TrackID for the flusher to map to a deck */
    atomic_store_explicit(&g_key_tid0, t0, memory_order_relaxed);
    atomic_store_explicit(&g_key_tid1, t1, memory_order_relaxed);
    {
        unsigned long long sid0 = 0,
                           sid1 = 0; /* real SourceId for the evict: Position +0x08/+0x10 */
        if (pos) {
            memcpy(&sid0, pos->b + 0x08, 8);
            memcpy(&sid1, pos->b + 0x10, 8);
        }
        atomic_store_explicit(&g_evict_sid0, sid0, memory_order_relaxed);
        atomic_store_explicit(&g_evict_sid1, sid1, memory_order_relaxed);
    }
    {
        long long opp = 0;
        if (pos)
            memcpy(&opp, pos->b + POS_SAMPLE_OFF, 8); /* operate()'s output position vs playhead */
        atomic_store_explicit(&g_op_pos, opp, memory_order_relaxed);
    }
    atomic_store_explicit(&g_op_caller, (uintptr_t) __builtin_return_address(0),
                          memory_order_relaxed); /* the prefetch layer */
    {
        int bits;
        memcpy(&bits, &ratio, 4);
        atomic_store_explicit(&g_key_ratio_bits, bits, memory_order_relaxed);
    }
    if (atomic_load_explicit(&g_opobs_on, memory_order_relaxed)) {
        unsigned w = atomic_fetch_add_explicit(&g_opw, 1, memory_order_relaxed) & 63u;
        struct opobs *e = &g_opring[w];
        e->self = self;
        e->ratio = ratio;
        e->t0 = t0;
        e->t1 = t1;
    }
    return out;
}
static void opobs_flush(void) {
    atomic_store_explicit(&g_opobs_on, dev_flag(OPOBS_PATH), memory_order_relaxed);
    unsigned w = atomic_load_explicit(&g_opw, memory_order_acquire);
    if (!atomic_load_explicit(&g_opobs_on, memory_order_relaxed)) {
        g_opr = w;
        return;
    }
    int em = 0;
    while (g_opr != w && em < 8) {
        struct opobs *e = &g_opring[g_opr & 63u];
        logln("OPERATE self=%p ratio=%.5f tid=[%llu %llu]", e->self, e->ratio, e->t0, e->t1);
        g_opr++;
        em++;
    }
    if ((unsigned) (w - g_opr) > 64u)
        g_opr = w;
}

/* ---- KeyControlFGPR native key-shift --------------------------------------- *
 * The active pitch class is time_domain::KeyControlFGPR (PitchSimpleCrossFade's
 * operate does not fire during playback). setSource @0xd9c9d8 stores the raw
 * source object at FGPR+0x162998; FGPR reads it via the object's vtable slot
 * +0x10, pitches by the key ratio at FGPR+0x448, and its output fills the pages
 * PageBuffer::read (0x7820d0) returns. Injecting the stem sum at that source read
 * lets EP147 key-shift the stems natively. The hook learns the read-fn address
 * from setSource and snapshots the FGPR instance for the ratio auto-read. */
#define FGPR_SETSRC_DEFAULT EP_d9c9d8
#define FGPRADDR_PATH "/home/root/settings/azstem.fgpraddr"
static uintptr_t g_fgpr_setsrc_addr = 0;
static void (*g_orig_fgpr_setsrc)(void *, void *) = NULL;
static _Atomic uintptr_t g_fgpr_self = 0;   /* last FGPR instance (ratio read) */
static _Atomic uintptr_t g_fgpr_srcobj = 0; /* its raw source object          */
static _Atomic uintptr_t g_fgpr_readfn = 0; /* source vtable[+0x10] = read to inject at */
static _Atomic int g_fgpr_setsrc_hits = 0;

static void fgpr_setsrc_hook(void *self, void *src) {
    g_orig_fgpr_setsrc(self, src);
    atomic_fetch_add_explicit(&g_fgpr_setsrc_hits, 1, memory_order_relaxed);
    atomic_store_explicit(&g_fgpr_self, (uintptr_t) self, memory_order_relaxed);
    atomic_store_explicit(&g_fgpr_srcobj, (uintptr_t) src, memory_order_relaxed);
    uintptr_t vt = 0, readfn = 0;
    if (src) {
        vt = *(uintptr_t *) src;
        if (vt)
            readfn = *(uintptr_t *) (vt + 0x10);
    }
    atomic_store_explicit(&g_fgpr_readfn, readfn, memory_order_relaxed);
    if (atomic_load_explicit(&g_fgprobs_on, memory_order_relaxed))
        logln("FGPR setSource: self=%p src=%p vt=%p readfn=%p srcoff=%p (pbread=0x7820d0)", self,
              src, (void *) vt, (void *) readfn,
              src ? *(void **) ((char *) self + FGPR_SRC_OFF) : NULL);
}
/* FGPR pitched reader @0xda3df0: long read(this, out, pos, cnt).
 * Passthrough branch (mode this[0x1629b4]&~2==1) calls this-vtable+0x48 (raw source);
 * the pitch branch calls the pitch kernel 0xda07d8, which reads the sub-source at
 * FGPR+0x159b40 via its vtable slot +0x18 and pitches by the ratio at
 * FGPR+0x438/0x162134. The hook carries the FGPR deck tag and, with
 * azstem.fgprobs, logs the mode and ratios. */
#define FGPRREAD_DEFAULT EP_da3df0
#define FGPR_MODE_OFF 0x1629b4UL    /* readSimply/pitch mode selector */
#define FGPR_SUBSRC_OFF 0x159b40UL  /* pitch kernel's source object ptr */
#define FGPR_R438_OFF 0x438UL       /* per-block ratio advance float */
#define FGPR_R162134_OFF 0x162134UL /* pitch-active ratio float (>eps => pitching) */
#define FGPRREADADDR_PATH "/home/root/settings/azstem.fgprreadaddr"
typedef long (*fgpr_read_fn)(void *self, void *out, void *pos, long cnt);
static fgpr_read_fn g_orig_fgpr_read = NULL;
static uintptr_t g_fgpr_read_addr = 0;
static _Atomic int g_fgpr_read_hits = 0;
static _Atomic uintptr_t g_fgpr_read_self = 0; /* FGPR this seen by the reader */
static _Atomic unsigned g_fgpr_read_log = 0;   /* throttle */

static long fgpr_read_hook(void *self, void *out, void *pos, long cnt) {
    atomic_fetch_add_explicit(&g_fgpr_read_hits, 1, memory_order_relaxed);
    atomic_store_explicit(&g_fgpr_read_self, (uintptr_t) self, memory_order_relaxed);
    if (atomic_load_explicit(&g_fgprobs_on, memory_order_relaxed)) {
        unsigned n = atomic_fetch_add_explicit(&g_fgpr_read_log, 1, memory_order_relaxed);
        if ((n & 0x3f) == 0) { /* ~1/64 calls */
            char *b = (char *) self;
            unsigned mode = *(unsigned *) (b + FGPR_MODE_OFF);
            float r438 = *(float *) (b + FGPR_R438_OFF);
            float r162134 = *(float *) (b + FGPR_R162134_OFF);
            uintptr_t subsrc = *(uintptr_t *) (b + FGPR_SUBSRC_OFF);
            uintptr_t subvt = subsrc ? *(uintptr_t *) subsrc : 0;
            uintptr_t subread = subvt ? *(uintptr_t *) (subvt + 0x18) : 0;
            logln(
                "FGPR read: self=%p cnt=%ld mode=0x%x r438=%.5f r162134=%.5f subsrc=%p subread=%p",
                self, cnt, mode, (double) r438, (double) r162134, (void *) subsrc,
                (void *) subread);
        }
    }
    {
        struct fgscope sc;
        long r;
        fg_enter(&sc, (uintptr_t) self);
        r = g_orig_fgpr_read(self, out, pos, cnt);
        fg_leave(&sc);
        return r;
    }
}

/* ---- KeyControlFacade key-shift hook ------------------------------------ */
typedef void (*kf_fn)(void *self, const void *p2);
static kf_fn g_orig_kf = NULL;
static uintptr_t g_kf_addr = 0;
static _Atomic int g_kfobs_on = 0;
struct kfobs {
    void *self;
    int v[4];
};
static struct kfobs g_kfring[32];
static _Atomic unsigned g_kfw = 0;
static unsigned g_kfr = 0;
static void kf_hook(void *self, const void *p2) {
    g_orig_kf(self, p2);
    if (atomic_load_explicit(&g_kfobs_on, memory_order_relaxed) && p2) {
        unsigned w = atomic_fetch_add_explicit(&g_kfw, 1, memory_order_relaxed) & 31u;
        struct kfobs *e = &g_kfring[w];
        e->self = self;
        memcpy(e->v, p2, 16); /* first 16 bytes of the UnitType arg */
    }
    /* native auto-key: hand the event to the flusher, which attributes the FGPR
     * whose applied ratio moves next (fallback when the structural search finds
     * nothing). Only stores here; all logic is off-RT. */
    if (atomic_load_explicit(&g_autokey_on, memory_order_relaxed) && p2) {
        int sem = 0;
        memcpy(&sem, p2, 4);
        atomic_store_explicit(&g_ak_kf_self, (uintptr_t) self, memory_order_relaxed);
        atomic_store_explicit(&g_ak_kf_sem, sem, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_ak_kf_seq, 1, memory_order_release);
    }
}
static void kfobs_flush(void) {
    atomic_store_explicit(&g_kfobs_on, dev_flag(KFOBS_PATH), memory_order_relaxed);
    unsigned w = atomic_load_explicit(&g_kfw, memory_order_acquire);
    if (!atomic_load_explicit(&g_kfobs_on, memory_order_relaxed)) {
        g_kfr = w;
        return;
    }
    while (g_kfr != w) {
        struct kfobs *e = &g_kfring[g_kfr & 31u];
        logln("KEYFAC self=%p arg[int]= %d %d %d %d", e->self, e->v[0], e->v[1], e->v[2], e->v[3]);
        g_kfr++;
    }
}


/* ---- Deck -> FGPR identity by construction --------------------------------
 * The FGPR deck tag needs a deck -> FGPR map; without it FGPR prefetch reads fall
 * to pb_deck_of's structural map, which aliases all decks to one pointer.
 *
 * The FGPR is created by direct synchronous calls inside the ctor that receives
 * the deck as an argument:
 *   DjPlayerUnit(this, PlayerId) 0x14a2560 --(0x14a2830)--> PlayerBasisUnit ctor
 *   0x14afbc0 (vt 0x289f798), stored once at unit+0x180 --(0x14afc4c)-->
 *   CascadedTimeStretchManager ctor 0xd91858 (vt 0x27b8a50) --(0xd919c4)-->
 *   KeyControlFGPR ctor 0xda4988 (vt 0x27ba108; its sole caller).
 * So djpuctor_hook reads [[[unit+0x180]+0x10]+0x48] - AK_SUB, every hop vtable-checked, right
 * after that ctor returns, on its thread (deck_track_map.c g_djpu_fgpr), and djpudtor_hook
 * clears it. This tick only publishes those tokens: no memory is read here. Every failure is
 * "unknown" (0), never "another deck": two decks resolving to one FGPR store 0 for both. Sole
 * writer of g_ak_fgpr_rt[]. */
static _Atomic int
    g_idmap_step[NDECK]; /* 0 ok, 1 no unit, 5 no FGPR from the ctor, 6 aliased */
static void fgpr_idmap_tick(void) {
    uintptr_t fg[NDECK];
    int step[NDECK];
    for (int d = 0; d < NDECK; d++) {
        fg[d] = 0;
        step[d] = 1;
        if (!atomic_load_explicit(&g_deck_self[STEM_SLOT[d]], memory_order_acquire))
            continue;
        fg[d] = atomic_load_explicit(&g_djpu_fgpr[d], memory_order_acquire);
        step[d] = fg[d] ? 0 : 5;
    }
    for (int d = 0; d < NDECK; d++)
        for (int e = d + 1; e < NDECK; e++)
            if (fg[d] && fg[d] == fg[e]) {
                fg[d] = fg[e] = 0;
                step[d] = step[e] = 6;
            }
    for (int d = 0; d < NDECK; d++) {
        uintptr_t old = atomic_load_explicit(&g_ak_fgpr_rt[d], memory_order_relaxed);
        int ostep = atomic_load_explicit(&g_idmap_step[d], memory_order_relaxed);
        if (old != fg[d] || ostep != step[d])
            logln("FGPRIDMAP d%d 0x%lx -> 0x%lx (step %d: 0 ok, 1 no unit, 5 no FGPR from the "
                  "ctor, 6 aliased)",
                  d, (unsigned long) old, (unsigned long) fg[d], step[d]);
        atomic_store_explicit(&g_idmap_step[d], step[d], memory_order_relaxed);
        atomic_store_explicit(&g_ak_fgpr_rt[d], fg[d], memory_order_relaxed);
    }
}

