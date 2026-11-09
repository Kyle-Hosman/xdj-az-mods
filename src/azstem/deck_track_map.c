/* SPDX-License-Identifier: GPL-3.0-only */
/* ---- DjPlayerUnit ctor hook: seed the slot table from the deck argument ----
 * @0x14a2560: DjPlayerUnit(this=x0, const PlayerId *id=x1, void *ctx=x2, int cap=x3).
 * `id` is {u8 no; u56 tag; u32 no @+8}; the deck number appears twice and is
 * accepted only when both copies agree.
 *
 * Runs at EP147 startup, before the first audio block, so decks 0..3 own their
 * slots before an unidentifiable unit (an AudioRecorderUnit) could claim one by
 * arrival order. Only stores a pointer; the object is not touched. */
#define DJPUCTOR_DEFAULT EP_14a2560
#define DJPUCTOR_PATH "/home/root/settings/azstem.djpuctor" /* hex override */
static uintptr_t g_djpuctor_addr = 0;
typedef void (*djpuctor_fn)(void *self, const void *id, void *ctx, int cap);
static djpuctor_fn g_orig_djpuctor = NULL;
/* Deck -> its KeyControlFGPR, captured by djpuctor_hook (0 = unknown). An identity token: it is
 * compared, never dereferenced, so it cannot outlive anything; djpudtor_hook clears it. */
static _Atomic uintptr_t g_djpu_fgpr[NDECK];
static int djpu_ptr_ok(uintptr_t p) {
    return p >= 0x10000 && p <= 0x0000ffffffffffffUL && !(p & 7);
}
/* [[[unit+0x180]+0x10]+0x48] - AK_SUB, every hop vtable-checked; 0 on any doubt. Plain loads:
 * called only from djpuctor_hook, right after EP147's constructor returned the unit. */
static uintptr_t djpu_fgpr_of(uintptr_t unit) {
    if (!djpu_ptr_ok(unit))
        return 0;
    uintptr_t pbu = *(const volatile uintptr_t *) (unit + IDMAP_PBU_OFF);
    if (!djpu_ptr_ok(pbu) || *(const volatile uintptr_t *) pbu != (uintptr_t) AK_VT_PBU)
        return 0;
    uintptr_t mgr = *(const volatile uintptr_t *) (pbu + IDMAP_MGR_OFF);
    if (!djpu_ptr_ok(mgr) || *(const volatile uintptr_t *) mgr != (uintptr_t) AK_VT_MGR)
        return 0;
    uintptr_t sub = *(const volatile uintptr_t *) (mgr + IDMAP_SUB_OFF);
    if (sub <= AK_SUB || !djpu_ptr_ok(sub - AK_SUB))
        return 0;
    uintptr_t f = sub - AK_SUB;
    return *(const volatile uintptr_t *) f == (uintptr_t) FGPR_VTABLE ? f : 0;
}

static void djpuctor_hook(void *self, const void *id, void *ctx, int cap) {
    unsigned n32 = ~0u;
    if (self && id) {
        unsigned n8 = *(const unsigned char *) id;
        n32 = *(const unsigned *) ((const char *) id + 8);
        if (n8 != n32)
            n32 = ~0u;
    }
    g_orig_djpuctor(self, id, ctx, cap);
    /* Published only once the ctor has returned: a half-built unit is never in a slot. Still
     * before its first audio block, which needs the finished unit. */
    if (n32 < (unsigned) NDECK) {
        /* The deck's KeyControlFGPR is built inside this constructor and stored once, so it is
         * read here, on the constructing thread, while EP147's call still holds the unit. */
        atomic_store_explicit(&g_djpu_fgpr[n32], djpu_fgpr_of((uintptr_t) self),
                              memory_order_release);
        int s = STEM_SLOT[n32];
        void *exp = NULL;
        atomic_fetch_add_explicit(&g_ctor_calls[n32], 1, memory_order_relaxed);
        if (!atomic_compare_exchange_strong_explicit(&g_deck_self[s], &exp, self,
                                                     memory_order_release, memory_order_relaxed) &&
            exp != self)
            atomic_fetch_or_explicit(&g_ap_rebind, 1u << n32, memory_order_relaxed);
        atomic_fetch_or_explicit(&g_ap_ctor, 1u << n32, memory_order_relaxed);
    }
}

/* ---- DjPlayerUnit destructor hook: the slot never outlives its unit ----
 * @0x14a2270 is the complete-object destructor (vtable slot 0); the deleting destructor and
 * both non-virtual thunks call it, so every way EP147 destroys a unit passes here. The slot is
 * cleared before the original runs, while the unit is still whole, so a consumer that loads it
 * afterwards finds NULL (unknown) instead of a pointer into freed memory. A unit built again
 * later takes its deck's slot from its own ctor. */
#define DJPUDTOR_DEFAULT EP_14a2270
typedef void (*djpudtor_fn)(void *self);
static djpudtor_fn g_orig_djpudtor = NULL;
static _Atomic unsigned g_djpu_dtor_calls;
static void djpudtor_hook(void *self) {
    for (int d = 0; d < NDECK; d++)
        if (atomic_load_explicit(&g_deck_self[STEM_SLOT[d]], memory_order_acquire) == self)
            atomic_store_explicit(&g_djpu_fgpr[d], 0, memory_order_release);
    for (int i = 0; i < MAX_DECKS; i++) {
        void *exp = self;
        atomic_compare_exchange_strong_explicit(&g_deck_self[i], &exp, NULL, memory_order_acq_rel,
                                                memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&g_djpu_dtor_calls, 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    g_orig_djpudtor(self);
}

/* ---- gui::LoadState -> LoadedTrackInfo: track identity by construction ----
 * The deck arrives as the LoadState ctor's argument and the track comes from
 * EP147's own LoadedTrackInfo, so nothing is scanned (unlike probe_strings, whose
 * first match can be another deck's track and is then cached). LTI lives in
 * EP147's heap, reads zero on an empty deck, and cannot go stale on disk.
 *
 * Default route; settings/azstem.nonativeid disables it, settings/azstem.ltiprobe
 * logs each read. If the hook cannot install, the track stays unknown. */
#define LSCTOR_DEFAULT EP_1f9c498
#define LSCTOR_PATH "/home/root/settings/azstem.lsctor"
#define LTIPROBE_PATH "/home/root/settings/azstem.ltiprobe"
/* LS_VT, LS_DECK_OFF, LS_MAGIC_OFF, LS_MAGIC, g_ls_gen, ls_enter/ls_leave, ls_wait_readers:
 * stem_playback.c */
#define LS_LTI_OFF 0x150
#define LTI_VT EP_2dd9288
#define LTI_ID_OFF 0x08
#define LTI_TITLE_OFF 0x38
#define LTI_ARTIST_OFF 0x40
/* LoadedTrackInfo's update (vtable slot +0x88 on 1.30, +0x90 on 2.00): the one function that
 * writes the TrackID (+0x08), title (+0x38) and artist (+0x40), and zeroes them all for an empty
 * deck. It runs on EP147's loader thread with `this` = the LoadedTrackInfo. */
#define LTI_UPDATE_DEFAULT EP_1f91210

/* ---- the deck's track, copied inside EP147's own call ----
 * The LoadedTrackInfo belongs to its LoadState (the ctor allocates it and stores it at +0x150)
 * and is rewritten in place by its update. So it is never read from another thread: the update
 * hook copies TrackID, title and artist right after EP147 wrote them, on the writing thread,
 * into this per-deck snapshot, and readers take the copy. A sequence lock keeps the three fields
 * together (a reader never pairs one track's TrackID with another's title). Writers (the update
 * hook, lsctor_hook, lsdtor_hook) take the odd sequence by compare-and-swap, so two of them
 * cannot interleave and neither update is lost; the release fence after the odd store keeps the
 * field stores from becoming visible before it, and the reader's acquire fence before its second
 * look at the sequence pairs with it. `owner` is the LoadedTrackInfo the copy came from: a reader
 * takes it only while that is still the deck's LoadedTrackInfo (g_lti_tok). Every field is
 * written and read with relaxed atomic accesses (the byte fields as 8-byte words, lti_snap_put /
 * lti_snap_get): the fences order them, and a reader that overlaps a writer reads words it then
 * discards instead of racing on them. */
#define LTI_STR 200 /* a multiple of 8 */
struct lti_snap {
    _Atomic unsigned seq;
    _Atomic int valid;
    _Atomic uintptr_t owner;
    uint64_t id[2]; /* the 16-byte TrackID */
    uint64_t title[LTI_STR / 8];
    uint64_t artist[LTI_STR / 8];
};
static struct lti_snap g_lti_snap[NDECK];
static _Atomic uintptr_t g_lti_tok[NDECK]; /* deck -> its LoadedTrackInfo (identity token) */
static _Atomic unsigned g_lti_updates, g_lti_orphans; /* telemetry: copied / no deck matched */
static unsigned lti_wlock(struct lti_snap *sn) {
    unsigned s = atomic_load_explicit(&sn->seq, memory_order_relaxed);
    for (;;) {
        if (!(s & 1u) && atomic_compare_exchange_weak_explicit(&sn->seq, &s, s + 1u,
                                                               memory_order_acquire,
                                                               memory_order_relaxed))
            break;
        s = atomic_load_explicit(&sn->seq, memory_order_relaxed);
    }
    atomic_thread_fence(memory_order_release); /* the fields below land after the odd value */
    return s;
}
static void lti_wunlock(struct lti_snap *sn, unsigned s) {
    atomic_store_explicit(&sn->seq, s + 2u, memory_order_release); /* after every field */
}
/* nw words of a snapshot field from src, by a writer holding the sequence. */
static void lti_snap_put(uint64_t *w, const void *src, size_t nw) {
    for (size_t i = 0; i < nw; i++) {
        uint64_t v;
        memcpy(&v, (const char *) src + 8 * i, 8);
        __atomic_store_n(&w[i], v, __ATOMIC_RELAXED);
    }
}
/* nw words of a snapshot field into dst, between the reader's two looks at the sequence. */
static void lti_snap_get(void *dst, const uint64_t *w, size_t nw) {
    for (size_t i = 0; i < nw; i++) {
        uint64_t v = __atomic_load_n(&w[i], __ATOMIC_RELAXED);
        memcpy((char *) dst + 8 * i, &v, 8);
    }
}
/* A live juce::String (one char*) into the snapshot field w (LTI_STR bytes); stops at the NUL,
 * keeps it printable. */
static void lti_copy_str(uintptr_t field, uint64_t *w) {
    char out[LTI_STR];
    memset(out, 0, sizeof out);
    uintptr_t sp = *(const volatile uintptr_t *) field;
    if (sp >= 0x10000 && sp <= 0x0000ffffffffffffUL) {
        const volatile char *t = (const volatile char *) sp;
        for (size_t i = 0; i + 1 < sizeof out; i++) {
            char c = t[i];
            if ((unsigned char) c < 0x20)
                break;
            out[i] = c;
        }
    }
    lti_snap_put(w, out, LTI_STR / 8);
}
/* Copy LoadedTrackInfo `lti` of deck d into the snapshot, if it is still deck d's. Plain loads:
 * the caller is inside EP147's call that owns `lti` (its update, or the LoadState ctor). */
static void lti_capture(int d, uintptr_t lti) {
    struct lti_snap *sn = &g_lti_snap[d];
    unsigned s = lti_wlock(sn);
    if (atomic_load_explicit(&g_lti_tok[d], memory_order_relaxed) != lti) {
        lti_wunlock(sn, s); /* the LoadState went away meanwhile: leave its invalidation */
        return;
    }
    int ok = *(const volatile uintptr_t *) lti == (uintptr_t) LTI_VT;
    if (ok) {
        unsigned char id[16];
        for (int i = 0; i < 16; i++)
            id[i] = *(const volatile unsigned char *) (lti + LTI_ID_OFF + (uintptr_t) i);
        lti_snap_put(sn->id, id, 2);
        lti_copy_str(lti + LTI_TITLE_OFF, sn->title);
        lti_copy_str(lti + LTI_ARTIST_OFF, sn->artist);
    }
    atomic_store_explicit(&sn->owner, lti, memory_order_relaxed);
    atomic_store_explicit(&sn->valid, ok, memory_order_relaxed);
    lti_wunlock(sn, s);
}
static void lti_invalidate(int d) {
    struct lti_snap *sn = &g_lti_snap[d];
    unsigned s = lti_wlock(sn);
    atomic_store_explicit(&sn->valid, 0, memory_order_relaxed);
    atomic_store_explicit(&sn->owner, 0, memory_order_relaxed);
    lti_wunlock(sn, s);
}

typedef long (*lti_update_fn)(void *, void *, void *, void *, void *, void *, void *, void *);
static lti_update_fn g_orig_lti_update = NULL;
static long lti_update_hook(void *self, void *a1, void *a2, void *a3, void *a4, void *a5,
                            void *a6, void *a7) {
    long r = g_orig_lti_update(self, a1, a2, a3, a4, a5, a6, a7);
    int hit = 0;
    for (int d = 0; d < NDECK; d++)
        if (self && atomic_load_explicit(&g_lti_tok[d], memory_order_acquire) == (uintptr_t) self) {
            lti_capture(d, (uintptr_t) self);
            hit = 1;
        }
    atomic_fetch_add_explicit(hit ? &g_lti_updates : &g_lti_orphans, 1, memory_order_relaxed);
    return r;
}

static int g_lti_probe = 0; /* read once at init, tested below */
static uintptr_t g_lsctor_addr = 0;
/* Eight params (deck is w4): a trampoline declaring fewer would forward whatever
 * our body left in x5..x7. Declaring and forwarding all of them keeps it a passthrough. */
typedef void (*lsctor_fn)(void *, void *, void *, void *, long, void *, void *, void *);
static lsctor_fn g_orig_lsctor = NULL;

static void lsctor_hook(void *self, void *a1, void *a2, void *a3, long deck, void *a5, void *a6,
                        void *a7) {
    g_orig_lsctor(self, a1, a2, a3, deck, a5, a6, a7);
    if (!self)
        return;
    /* Confirm the deck argument against the object's own field and magic, so a
     * signature drift yields "no identity" rather than a wrong deck. */
    unsigned d = 0, magic = 0;
    uintptr_t vt = 0;
    memcpy(&vt, self, 8);
    memcpy(&d, (const char *) self + LS_DECK_OFF, 4);
    memcpy(&magic, (const char *) self + LS_MAGIC_OFF, 4);
    if (vt != (uintptr_t) LS_VT || magic != LS_MAGIC)
        return;
    if (d >= (unsigned) NDECK)
        return;
    if ((long) d != deck)
        return;
    uintptr_t lti = 0;
    memcpy(&lti, (const char *) self + LS_LTI_OFF, 8); /* allocated by this ctor */
    atomic_fetch_add_explicit(&g_ls_gen[d], 1, memory_order_acq_rel); /* readers of the old one */
    atomic_store_explicit(&g_lti_tok[d], lti, memory_order_release);
    if (lti)
        lti_capture((int) d, lti); /* its state as the ctor left it; the update hook follows */
    atomic_store_explicit(&g_ls_self[d], self, memory_order_seq_cst);
    logln("lsctor: d%d self=%p magic ok (pid %d)", (int) d, self, (int) getpid());
}

/* ---- gui::LoadState destructor hook ----
 * @0x1fa16d0 is the complete-object destructor (vtable slot 0); the deleting destructor and both
 * thunks call it. Before the original tears anything down: unpublish the LoadState and its
 * LoadedTrackInfo, invalidate the deck's track copy, move the generation, and wait for any
 * reader ls_enter let in (stem_playback.c). */
#define LSDTOR_DEFAULT EP_1fa16d0
typedef void (*lsdtor_fn)(void *self);
static lsdtor_fn g_orig_lsdtor = NULL;
static void lsdtor_hook(void *self) {
    for (int d = 0; d < NDECK; d++) {
        void *exp = self;
        if (atomic_compare_exchange_strong_explicit(&g_ls_self[d], &exp, NULL,
                                                    memory_order_seq_cst, memory_order_relaxed)) {
            atomic_fetch_add_explicit(&g_ls_gen[d], 1, memory_order_acq_rel);
            atomic_store_explicit(&g_lti_tok[d], 0, memory_order_release);
            lti_invalidate(d);
            ls_wait_readers(d);
        }
    }
    atomic_thread_fence(memory_order_seq_cst);
    g_orig_lsdtor(self);
}

/* Deck d's track, from the copy taken inside EP147's own update. 1 with all three fields from
 * one update of the deck's current LoadedTrackInfo; else 0 (unknown), and the outputs empty. */
static int lti_read(int deck, unsigned char id16[16], char *title, size_t tn, char *artist,
                    size_t an) {
    if (title && tn)
        title[0] = 0;
    if (artist && an)
        artist[0] = 0;
    memset(id16, 0, 16);
    if (deck < 0 || deck >= NDECK)
        return 0;
    struct lti_snap *sn = &g_lti_snap[deck];
    unsigned char id[16];
    char ti[LTI_STR], ar[LTI_STR];
    for (int tries = 0; tries < 256; tries++) {
        unsigned s1 = atomic_load_explicit(&sn->seq, memory_order_acquire);
        if (s1 & 1u) { /* a writer is inside (a few hundred bytes of copying) */
            if (tries > 8)
                sched_yield();
            continue;
        }
        int valid = atomic_load_explicit(&sn->valid, memory_order_relaxed);
        uintptr_t owner = atomic_load_explicit(&sn->owner, memory_order_relaxed);
        lti_snap_get(id, sn->id, 2);
        lti_snap_get(ti, sn->title, LTI_STR / 8);
        lti_snap_get(ar, sn->artist, LTI_STR / 8);
        atomic_thread_fence(memory_order_acquire); /* the copies above before the second look */
        if (atomic_load_explicit(&sn->seq, memory_order_relaxed) != s1)
            continue;
        if (!valid || !owner ||
            atomic_load_explicit(&g_lti_tok[deck], memory_order_acquire) != owner)
            return 0;
        ti[LTI_STR - 1] = ar[LTI_STR - 1] = 0;
        memcpy(id16, id, 16);
        if (title && tn)
            snprintf(title, tn, "%s", ti);
        if (artist && an)
            snprintf(artist, an, "%s", ar);
        return 1;
    }
    return 0; /* never settled: unknown */
}

static int lti_id_zero(const unsigned char id16[16]);
/* The deck's content id (trackid::TrackID + 0x08), 0 = unknown. */
static unsigned lti_content_id(int deck) {
    unsigned char id[16];
    char ti[LTI_STR], ar[LTI_STR];
    unsigned cid = 0;
    if (g_lti_on && lti_read(deck, id, ti, sizeof ti, ar, sizeof ar) && !lti_id_zero(id))
        memcpy(&cid, id + 8, 4);
    return cid;
}
static int lti_id_zero(const unsigned char id16[16]) {
    for (int i = 0; i < 16; i++)
        if (id16[i])
            return 0;
    return 1;
}

/* ---- "is a track loaded on any deck", for the USB loader and the Reload confirm --------
 * A restart drops every loaded track, so the loader (az-usbload, RESTART POLICY) restarts
 * EP147 after boot only on proof that every deck is empty. That proof is EP147's own
 * per-deck LoadedTrackInfo: an all-zero TrackID is an empty deck, and UNLOAD TRACK zeroes
 * it in place. EP147 keeps no fd open on a loaded track, so nothing outside the process
 * can tell. Published as /tmp/az-decks (tmp + rename, so a reader never sees half):
 *     azdecks 1
 *     pid <EP147 pid>          the reader checks it is the EP147 running now
 *     up <seconds>             /proc/uptime when written: the reader checks it is fresh
 *     decks <one char a deck>  E empty, L loaded, U unknown
 * Rewritten on a change and at least every DECKS_HEARTBEAT_S, from the flusher (~1 s).
 * A different song passes through a zero TrackID for ~200 ms, so a deck reads E only when
 * its TrackID was zero on this sample and the previous one; a first zero reads U. */
#define DECKS_PATH "/tmp/az-decks"
#define DECKS_HEARTBEAT_S 3
static _Atomic int g_decks_verdict = -1; /* decks_verdict of the last sample */
/* Each deck's last classification ('L' loaded, 'E' empty, 'U' unknown), for azstem_deck_snapshot. */
static _Atomic char g_deck_state[NDECK] = {'U', 'U', 'U', 'U'};
/* One deck's state from this sample and the previous one. ok = the LoadedTrackInfo read
 * validated; zero = its TrackID is all zero. */
static char decks_classify(int ok, int zero, int prev_zero) {
    if (!ok)
        return 'U';
    if (!zero)
        return 'L';
    return prev_zero ? 'E' : 'U';
}
/* 1 when any deck is loaded, 0 when every deck is empty, -1 otherwise (unknown is never
 * empty). */
static int decks_verdict(const char *st, int n) {
    int unknown = 0;
    for (int i = 0; i < n; i++) {
        if (st[i] == 'L')
            return 1;
        if (st[i] != 'E')
            unknown = 1;
    }
    return unknown ? -1 : 0;
}
static void decks_tick(void) {
    static int prev_zero[NDECK];
    static char last[NDECK + 1];
    static time_t last_write;
    char st[NDECK + 1];
    for (int d = 0; d < NDECK; d++) {
        unsigned char id[16];
        char ti[8], ar[8];
        int ok = g_lti_on && lti_read(d, id, ti, sizeof ti, ar, sizeof ar);
        int zero = ok && lti_id_zero(id);
        st[d] = decks_classify(ok, zero, prev_zero[d]);
        prev_zero[d] = zero;
    }
    st[NDECK] = 0;
    for (int d = 0; d < NDECK; d++)
        atomic_store_explicit(&g_deck_state[d], st[d], memory_order_relaxed);
    atomic_store_explicit(&g_decks_verdict, decks_verdict(st, NDECK), memory_order_relaxed);
    time_t now = time(NULL);
    if (!strcmp(st, last) && now - last_write < DECKS_HEARTBEAT_S && now >= last_write)
        return;
    char up[32] = "";
    FILE *u = fopen("/proc/uptime", "re");
    if (u) {
        if (fscanf(u, "%31s", up) != 1)
            up[0] = 0;
        fclose(u);
    }
    if (!up[0])
        return; /* without the time the file could not be judged fresh: write none */
    FILE *f = fopen(DECKS_PATH ".new", "we");
    if (!f)
        return;
    fprintf(f, "azdecks 1\npid %d\nup %s\ndecks %s\n", (int) getpid(), up, st);
    if (fclose(f) != 0 || rename(DECKS_PATH ".new", DECKS_PATH) != 0) {
        unlink(DECKS_PATH ".new");
        return;
    }
    memcpy(last, st, sizeof last);
    last_write = now;
}
/* For azmod (the Mod Menu's Reload confirm): 1 a track is loaded, 0 every deck is empty,
 * -1 not known. Unlike azstem_deck_has_track it does not depend on a STEMS tree. */
int azstem_decks_loaded(void);
int azstem_decks_loaded(void) {
    return atomic_load_explicit(&g_decks_verdict, memory_order_relaxed);
}

/* ---- per-PageBuffer -> owning deck mapping (multi-deck) -------------------
 * Content classifier: each active PageBuffer maps to the deck whose full stem
 * sum matches its original (pre-injection) PCM. An unmatched PageBuffer passes
 * through, never another deck's audio. A mapping that stops matching (track
 * change, pointer reuse) reverts to unknown. Pages are post-pitch, so samples
 * are taken only at key ratio ~= 1. */
#define NPB 8
static _Atomic uintptr_t g_pb_self[NPB]; /* PageBuffer `this` (0 = free slot) */
static _Atomic int g_pb_deck[NPB];       /* owning deck index, -1 = unknown  */
/* Per-deck PageBuffer we inject into (pbread_hook `self`): the scheduler-owning
 * main PageBuffer (Track vector +0x140/+0x148, scheduler +0x20). Eviction
 * (clearBufferedSource) targets this, guarded on a non-NULL scheduler; never
 * g_ts_src, whose +0x20 is NULL and makes clearBufferedSource NULL-deref. */
static _Atomic uintptr_t g_evict_pb[NDECK];
static int g_pb_miss[NPB]; /* consecutive mismatch windows (flusher-only, hysteresis) */
/* Classification-sample ring: RT pushes, the flusher correlates against each
 * deck's stem sum. */
struct pbcls {
    uintptr_t self;
    long long pos;
    float e;
}; /* e = peak|L| over the read block (envelope point) */
static struct pbcls g_clsring[128];
static _Atomic unsigned g_clsw = 0;
static unsigned g_clsr = 0;
/* Structural per-deck Track map. EP147 routes reads by deck number
 * (ParallelBufferingUnitList::refUnitFor(client) -> units[client.deckNumber]), each
 * unit owning that deck's pcmbuf::pagebuf::Track, whose ::read we hook. The Track
 * carries no deck field, so find_track walks from g_deck_self[deck] to the object
 * with the Track vtable captured at the hook. Takes precedence over the classifier. */
static _Atomic uintptr_t g_track_vtable = 0;  /* Track vtable, captured at Track::read (runtime) */
static _Atomic uintptr_t g_deck_track[NDECK]; /* structural Track self per deck (0 = unknown) */
static _Atomic int g_structmap_on = 0; /* structural map drives pb_deck_of (on unless azstem.nostructmap) */
/* Trackmap provenance (diagnostics only). direct_track() follows the deck's own
 * audio chain; the BFS fallback is a generic pointer walk that can stray onto
 * another deck's Track. Recording which path produced each entry, and when two
 * decks alias, makes a mis-route visible. */
#define FT_SRC_NONE 0                          /* stored 0: neither path found a Track */
#define FT_SRC_DIRECT 1                        /* stored the canonical self+0xe8->+0xb8 walk */
#define FT_SRC_BFS 2                           /* stored the BFS fallback's answer (suspect) */
#define FT_SRC_NOSELF 3                        /* loop skipped: g_deck_self[] was NULL, no store */
static _Atomic unsigned g_ft_src[NDECK];       /* provenance of this deck's current entry */
static _Atomic unsigned g_ft_direct0_n[NDECK]; /* times direct_track() returned 0 */
static _Atomic unsigned g_ft_bfs_n[NDECK];     /* times the BFS fallback actually ran */
static _Atomic unsigned g_ft_noself_n[NDECK];  /* times the deck was skipped with no store */
static _Atomic unsigned g_ft_rounds = 0;       /* find_track refresh rounds that ran */
/* RT lookup: which deck owns this PageBuffer/Track, or -1 (passthrough).
 * Order: the structural map (g_deck_track) when enabled, then the content
 * classifier (g_pb_self/g_pb_deck).
 * Per-rule counters: the TLS deck tag decides only ~1% of reads, so this function
 * decides most routing, and a bleed from the structural map must be distinguishable
 * from one from a stale table entry. */
static _Atomic long g_pbd_struct = 0, g_pbd_target = 0, g_pbd_table = 0, g_pbd_miss = 0;
static _Atomic long g_pbd_alias = 0; /* struct match refused: another deck holds the same pointer */
static inline int pb_deck_of(uintptr_t self) {
    if (!self)
        return -1;
    /* An aliased entry means unknown, never first match. The self+0xe8->+0xb8
     * walk can return one pointer for all four decks (ts_src, which every deck's
     * pitch view reads); first-match would give d0's stems to every deck.
     * Passthrough plays the track's own audio instead. RT-safe: <= NDECK^2 loads. */
    if (atomic_load_explicit(&g_structmap_on, memory_order_relaxed))
        for (int i = 0; i < NDECK; i++)
            if ((uintptr_t) atomic_load_explicit(&g_deck_track[i], memory_order_relaxed) == self) {
                for (int j = 0; j < NDECK; j++)
                    if (j != i && (uintptr_t) atomic_load_explicit(&g_deck_track[j],
                                                                   memory_order_relaxed) == self) {
                        atomic_fetch_add_explicit(&g_pbd_alias, 1, memory_order_relaxed);
                        return -1;
                    }
                atomic_fetch_add_explicit(&g_pbd_struct, 1, memory_order_relaxed);
                return i;
            }
    /* No rule maps the single global g_target_pbself to deck 0: that would be a
     * wrong answer on every other deck. g_pbd_target stays in probestat (always 0). */
    for (int i = 0; i < NPB; i++)
        if (atomic_load_explicit(&g_pb_self[i], memory_order_relaxed) == self) {
            atomic_fetch_add_explicit(&g_pbd_table, 1, memory_order_relaxed);
            return atomic_load_explicit(&g_pb_deck[i], memory_order_relaxed);
        }
    atomic_fetch_add_explicit(&g_pbd_miss, 1, memory_order_relaxed);
    return -1;
}
static _Atomic int g_testtone_on =
    0; /* azstem.testtone: inject a 1kHz sine instead of stems (pitch validator) */
#define PBSELF_PATH "/home/root/settings/azstem.pbself"
#define TESTTONE_PATH "/home/root/settings/azstem.testtone"
/* Decode-level hook: audio_format::SampleRateConverter::fileRead, the raw PCM
 * fed into the SRC/pitch resampler (below PageBuffer, so key-shift applies to
 * what it returns). vtable 0x279c340 slot[3].
 * ReaderErrorType fileRead(this=x0, aint=x1, Float2* dst=x2, aint=x3). */
#define FREAD_DEFAULT EP_d58ee0
#define FRADDR_PATH "/home/root/settings/azstem.fraddr"
/* PitchSimpleCrossFade::operate @0xdabb48 is the pitch stage; its ratio is at
 * [this+0x80] (1.0 = key 0, 2^(N/12) = key +N). Read with the input Position's
 * TrackID so the same key-shift can be applied to the injected stems. */
#define OPERATE_DEFAULT EP_dabb48
#define OPADDR_PATH "/home/root/settings/azstem.opaddr"
#define OPOBS_PATH "/home/root/settings/azstem.opobs"
#define KEYRATIO_PATH "/home/root/settings/azstem.keyratio" /* manual ratio override (float) */
#define FINDPITCH_PATH \
    "/home/root/settings/azstem.findpitch" /* one-shot: BFS deck->PitchSimpleCrossFade */
#define FINDTRACK_PATH \
    "/home/root/settings/azstem.findtrack" /* diag: BFS deck->pcmbuf Track (structural deck map)   \
                                            */
#define NOSTRUCTMAP_PATH \
    "/home/root/settings/azstem.nostructmap" /* opt-out: the structural deck->Track map is the     \
                                                default (azstem.structmap is a no-op) */
/* dj_player::KeyControlFacade (vtable @0x287b868) method 0x1381538 takes
 * (this, const UnitType<int>* semitone): the DJ's key shift for the deck. */
#define KEYFAC_DEFAULT EP_1381538
#define KFADDR_PATH "/home/root/settings/azstem.kfaddr"
#define KFOBS_PATH "/home/root/settings/azstem.kfobs"
#define PITCH_VTABLE EP_27bb148 /* time_domain::PitchSimpleCrossFade vtable */
static _Atomic int g_keyratio_bits = 0x3f800000; /* current key ratio (float bits), 1.0 */

/* azmod owns the requestAnalyze hook and therefore the TrackID -> path table;
 * reach it by dlsym so each EP147 function has one hook. Bound lazily: the
 * load order of the two .so files is not guaranteed. */
static int (*g_tidpath)(const unsigned char *, char *, size_t) = NULL;
static void lti_bind_azmod(void) {
    if (!g_tidpath)
        *(void **) &g_tidpath = dlsym(RTLD_DEFAULT, "azmod_trackid_path");
}

/* Native per-deck track path / name, no scanning:
 *   LoadedTrackInfo     -> which track (16-byte TrackID)
 *   azmod's reqan table -> its full path
 *   basename minus ext  -> the name stems are looked up by
 * Returns 1 with `out` filled, else 0 = unknown, which callers treat as "no
 * stems", never as "keep the old guess". No fallback to LTI's title: it often
 * differs from the file name ("Day & Night" vs "Day & Night - Dabeull.mp3") and
 * would silently miss every stems folder. */
static int lti_native_path(int deck, char *out, size_t n) {
    if (out && n)
        out[0] = 0;
    if (!g_lti_on)
        return 0;
    unsigned char id[16];
    char ti[200], ar[200];
    if (!lti_read(deck, id, ti, sizeof ti, ar, sizeof ar))
        return 0;
    if (lti_id_zero(id))
        return 0;
    lti_bind_azmod();
    if (!g_tidpath)
        return 0;
    return g_tidpath(id, out, n) && out[0] == '/';
}
static int lti_native_name(int deck, char *out, size_t n) {
    if (!out || !n)
        return 0; /* the snprintf and strrchr below write and read out */
    out[0] = 0;
    if (!g_lti_on)
        return 0;
    unsigned char id[16];
    char ti[200], ar[200];
    if (!lti_read(deck, id, ti, sizeof ti, ar, sizeof ar))
        return 0;
    if (lti_id_zero(id))
        return 0; /* deck genuinely empty */
    lti_bind_azmod();
    if (!g_tidpath)
        return 0; /* azmod half absent */
    char path[300];
    if (!g_tidpath(id, path, sizeof path))
        return 0; /* id not in the window yet */
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    if (!b[0])
        return 0;
    snprintf(out, n, "%s", b);
    char *dot = strrchr(out, '.');
    if (dot)
        *dot = 0;
    return out[0] ? 1 : 0;
}

/* ---- the structural deck -> Track map, kept by the audio thread ----
 * The canonical walk unit+0xe8 -> +0xb8 -> Track follows the deck's own audio chain, so it is
 * done where that chain is in use: in hook_pbi, inside EP147's render of this unit, on the
 * thread that renders it. Plain loads, the Track checked by its vtable; anything else stores 0
 * (unknown, passthrough), never a guess. Written only on a change. */
static void deck_track_rt(int deck, const void *unit) {
    if (deck < 0 || deck >= NDECK || !atomic_load_explicit(&g_structmap_on, memory_order_relaxed))
        return;
    uintptr_t tvt = atomic_load_explicit(&g_track_vtable, memory_order_relaxed), trk = 0;
    uintptr_t inter = *(const volatile uintptr_t *) ((uintptr_t) unit + 0xe8);
    if (tvt && inter >= 0x10000 && inter <= 0x0000ffffffffffffUL && !(inter & 7)) {
        uintptr_t t = *(const volatile uintptr_t *) (inter + 0xb8);
        if (t >= 0x10000 && t <= 0x0000ffffffffffffUL && !(t & 7) &&
            *(const volatile uintptr_t *) t == tvt)
            trk = t;
    }
    if (atomic_load_explicit(&g_deck_track[deck], memory_order_relaxed) != trk) {
        atomic_store_explicit(&g_deck_track[deck], trk, memory_order_relaxed);
        atomic_store_explicit(&g_ft_src[deck], trk ? FT_SRC_DIRECT : FT_SRC_NONE,
                              memory_order_relaxed);
        if (!trk)
            atomic_fetch_add_explicit(&g_ft_direct0_n[deck], 1, memory_order_relaxed);
    }
}
/* Flusher: the opt-out flag, and in development builds the diagnostic. */
static void find_track(void) {
    atomic_store_explicit(&g_structmap_on, !dev_flag(NOSTRUCTMAP_PATH),
                          memory_order_relaxed); /* default on; see NOSTRUCTMAP_PATH */
    atomic_fetch_add_explicit(&g_ft_rounds, 1, memory_order_relaxed);
}

/* ---- Trackmap census (diagnostics only; flusher thread) -----------------
 *   TRACKMAP d0=0x..(direct) d1=0x..(bfs) d2=0x0(none) d3=0x..(NOSELF!)
 *     | structmap=1 rounds=N direct0=a,b,c,d bfs=a,b,c,d noself=a,b,c,d
 *     | pbread tls_set=N tls_unset=N
 * - CHANGE: an entry moved (old -> new), e.g. on a track load.
 * - ALIAS: two decks hold the same non-zero pointer (pb_deck_of refuses it).
 * - direct0 / bfs: how often the direct walk failed and the BFS supplied the entry.
 * - tls_unset: reads routed by pb_deck_of (outside a deck render); if it stays 0
 *   this map decides nothing.
 * The full line repeats at least once a minute as a baseline. logln() flushes
 * every line. */
static const char *ft_srcname(unsigned v) {
    switch (v) {
    case FT_SRC_DIRECT:
        return "direct";
    case FT_SRC_BFS:
        return "bfs";
    case FT_SRC_NOSELF:
        return "NOSELF!";
    default:
        return "none";
    }
}
#define TRACKMAPSELFTEST_PATH "/home/root/settings/azstem.trackmapselftest"
/* Shared by the live census and the self-test, so the self-test exercises the
 * production code. Read-only. */
static int trackmap_alias_scan(const uintptr_t *cur, const unsigned *src, int say,
                               const char *tag) {
    int alias = 0;
    for (int i = 0; i < NDECK; i++)
        for (int j = i + 1; j < NDECK; j++)
            if (cur[i] && cur[i] == cur[j]) {
                alias = 1;
                if (say)
                    logln("TRACKMAP %sALIAS *** d%d(%s) and d%d(%s) BOTH @0x%lx -- pb_deck_of "
                          "returns d%d, d%d is MIS-ROUTED",
                          tag, i, ft_srcname(src[i]), j, ft_srcname(src[j]), (unsigned long) cur[i],
                          i, j);
            }
    return alias;
}
static void trackmap_census(void) {
    static uintptr_t prev[NDECK];
    static int inited = 0;
    static time_t prev_t = 0;
    static int prev_alias = 0;
    uintptr_t cur[NDECK];
    unsigned src[NDECK];
    int changed = 0;

    for (int i = 0; i < NDECK; i++) {
        cur[i] = (uintptr_t) atomic_load_explicit(&g_deck_track[i], memory_order_relaxed);
        src[i] = atomic_load_explicit(&g_ft_src[i], memory_order_relaxed);
    }
    if (!inited) {
        inited = 1;
        for (int i = 0; i < NDECK; i++)
            prev[i] = cur[i];
        changed = 1;
    }
    for (int i = 0; i < NDECK; i++) {
        if (cur[i] == prev[i])
            continue;
        changed = 1;
        logln("TRACKMAP CHANGE d%d 0x%lx -> 0x%lx (%s)", i, (unsigned long) prev[i],
              (unsigned long) cur[i], ft_srcname(src[i]));
        prev[i] = cur[i];
    }
    int alias = trackmap_alias_scan(cur, src, (!prev_alias || changed), "");
    if (prev_alias && !alias)
        logln("TRACKMAP ALIAS cleared");
    prev_alias = alias;

    /* Self-test (azstem.trackmapselftest): run the alias scan over a synthetic
     * map where d0 and d2 share a pointer, proving the detector can fire.
     * g_deck_track[] is untouched. */
    {
        static int st_done = 0;
        if (!dev_flag(TRACKMAPSELFTEST_PATH))
            st_done = 0; /* re-arm on rm+touch */
        else if (!st_done) {
            st_done = 1;
            uintptr_t fake[NDECK];
            unsigned fsrc[NDECK];
            for (int i = 0; i < NDECK; i++) {
                fake[i] = 0xAA00 + (uintptr_t) i;
                fsrc[i] = FT_SRC_DIRECT;
            }
            if (NDECK > 2) {
                fake[2] = fake[0];
                fsrc[2] = FT_SRC_BFS;
            }
            int fa = trackmap_alias_scan(fake, fsrc, 1, "SELFTEST ");
            logln("TRACKMAP SELFTEST fabricated d0==d2 -> alias=%d (1 = the detector fires)", fa);
        }
    }

    time_t now_t = time(NULL);
    if (!changed && now_t - prev_t < 60)
        return;
    prev_t = now_t;
    {
        char line[320];
        int o = 0;
        char d0[64];
        int c0 = 0;
        char bf[64];
        int c1 = 0;
        char ns[64];
        int c2 = 0;
        for (int i = 0; i < NDECK; i++) {
            o += snprintf(line + o, sizeof line - (size_t) o, "%sd%d=0x%lx(%s)", i ? " " : "", i,
                          (unsigned long) cur[i], ft_srcname(src[i]));
            c0 += snprintf(d0 + c0, sizeof d0 - (size_t) c0, "%s%u", i ? "," : "",
                           atomic_load_explicit(&g_ft_direct0_n[i], memory_order_relaxed));
            c1 += snprintf(bf + c1, sizeof bf - (size_t) c1, "%s%u", i ? "," : "",
                           atomic_load_explicit(&g_ft_bfs_n[i], memory_order_relaxed));
            c2 += snprintf(ns + c2, sizeof ns - (size_t) c2, "%s%u", i ? "," : "",
                           atomic_load_explicit(&g_ft_noself_n[i], memory_order_relaxed));
        }
        logln("TRACKMAP %s | structmap=%d rounds=%u direct0=%s bfs=%s noself=%s alias=%d"
              " | pbread tls_set=%ld tls_unset=%ld",
              line, atomic_load_explicit(&g_structmap_on, memory_order_relaxed),
              atomic_load_explicit(&g_ft_rounds, memory_order_relaxed), d0, bf, ns, alias,
              atomic_load_explicit(&g_pbread_tls_set, memory_order_relaxed),
              atomic_load_explicit(&g_pbread_tls_unset, memory_order_relaxed));
    }
}

/* ---- field-discovery probe (off-RT, only while PROBE_PATH exists) ----------
 * Snapshots the deck object via /proc/self/mem (pread returns an error on an
 * unmapped range instead of faulting) and logs fields that look like a playhead
 * or a rate. Logging only. */
#define PROBE_PATH "/home/root/settings/azstem.probe"
#define PROBE_BYTES 2048

