/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file stems_display.c
 * @brief Native pad-grid, tile-art and source-cell display for the stems feature.
 *
 * Drives EP147's own widgets instead of drawing over them:
 *  - bpmctor_hook captures each deck's gui::BpmState from the deck argument its
 *    constructor receives.
 *  - wavector_hook (pad tile ctor), waveswitch_hook (pad-mode switch) and
 *    modeget_hook (pad-mode getter, overridden only for callers inside the mode
 *    dispatcher) make EP147 build its native PadStemWidget while STEMS is armed.
 *  - stemref_hook (tile refresh) records each live tile, and stem_tile_fix_one
 *    sets its art through the ImagesComponent image index, so tiles follow the
 *    per-deck stem state.
 *  - s4ctor_hook captures each deck's 4-DECK source cell; stems4_tick (message
 *    thread, from rtick_hook) swaps in a per-deck native stem strip.
 *  - read_avail and reset_stems_on_track_change mirror azstem's per-deck
 *    availability and current track into azmod state; arm_stems() follows the
 *    STEMS master toggle; stems_avail_redispatch re-dispatches a deck's grid
 *    when its availability changes.
 *
 * Widget pointers are validated by alignment and vtable at use. Decks come from
 * constructor arguments and the deck census; an unresolved deck is left alone.
 *
 * Kill switch: azmod-stems4.off (4-deck source cells). Opt-in probes:
 * azmod-tilector.on, azmod-stempoke.on.
 */
static int read_avail(azmod_state *s) {
    static const char *P[AZMOD_STEM_DECKS] = {AVAIL_PATH, AVAIL2_PATH, AVAIL3_PATH, AVAIL4_PATH};
    int changed = 0;
    for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
        FILE *f = fopen(P[d], "re");
        int av = 0;
        if (f) {
            int c = fgetc(f);
            av = (c == '1');
            fclose(f);
        }
        if (av != s->stems_available[d]) {
            s->stems_available[d] = av;
            changed = 1;
        }
    }
    return changed;
}
/* azstem's per-deck track generation (azstem_trackgen). It also bumps when a
 * different file with the same name is loaded, which the name alone cannot see,
 * so a same-name swap does not keep the previous song's stem mutes. Optional: an
 * older azstem.so without it leaves name-only detection. */
static unsigned (*g_trackgen_fn)(int deck);
static int reset_stems_on_track_change(azmod_state *s) {
    static const char *P[AZMOD_STEM_DECKS] = {CURTRACK_PATH, CURTRACK2_PATH, CURTRACK3_PATH,
                                              CURTRACK4_PATH};
    static char last[AZMOD_STEM_DECKS][256] = {{0}};
    static int primed[AZMOD_STEM_DECKS] = {0};
    static unsigned lastgen[AZMOD_STEM_DECKS];
    static int genok[AZMOD_STEM_DECKS]; /* lastgen was read with azstem_trackgen */
    {
        static unsigned tries; /* azstem is preloaded after us: resolve late, cheaply */
        if (!g_trackgen_fn && (tries++ & 63u) == 0)
            *(void **) &g_trackgen_fn = dlsym(RTLD_DEFAULT, "azstem_trackgen");
    }
    int changed = 0;
    for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
        char cur[256] = {0};
        FILE *f = fopen(P[d], "re");
        if (!f)
            continue;
        if (fgets(cur, sizeof cur, f)) {
        }
        fclose(f);
        char *nl = strchr(cur, '\n');
        if (nl)
            *nl = 0;
        if (!cur[0]) {
            /* No track on this deck. azstem clears this file at init and on a
             * confirmed eject (debounced ~1.6s), so empty is a real state, not a
             * probe miss. Forget the previous name: otherwise reloading the same
             * track compares equal, the reset below never fires, and the deck
             * inherits the previous track's stem mutes. */
            last[d][0] = 0;
            continue;
        }
        unsigned gen = g_trackgen_fn ? g_trackgen_fn(d) : 0;
        if (g_trackgen_fn && !genok[d]) {
            lastgen[d] = gen;
            genok[d] = 1;
        } /* first reading: a baseline, not a change */
        if (!primed[d]) {
            snprintf(last[d], sizeof last[d], "%s", cur);
            primed[d] = 1;
            continue;
        }
        if (strcmp(cur, last[d]) != 0 || gen != lastgen[d]) {
            lastgen[d] = gen;
            for (int i = 0; i < AZMOD_STEM_COUNT; i++)
                s->stem_on[d][i] = true; /* default: all on */
            write_stems(s);              /* re-engage reset masks */
            snprintf(last[d], sizeof last[d], "%s", cur);
            changed = 1;
        }
    }
    return changed; /* caller must repaint: the pad LEDs rebuild every tick, but
                     * the X11 overlay only redraws when marked dirty */
}

/* ---- Per-deck live (tempo-adjusted) BPM, from EP147's own gui::BpmState ----
 *
 * There is one gui::BpmState per deck and its constructor takes the deck as an
 * argument, so each object is filed under the deck the ctor was given, never by
 * arrival order. The ctor writes that argument into the low byte of the u64 at
 * self+0x08 (`orr x6,x7,x1,lsl #8; stp xzr,x6,[x19]`).
 *
 *   self+0x08 u64       low byte deck 0..3, the rest a hash the ctor computes (fixed)
 *   self+0xa0 double    live, tempo-adjusted BPM  (DBL_MAX = no track)
 *   self+0xa8 u32       the same BPM x100         (0xffffffff = no track)
 *   self+0xb0 double    analyzed track BPM        (DBL_MAX = no track)
 *
 * The BPM is a double. BPMx<10> in this binary belongs to
 * gui::FxState::bpmChanged (the mixer / Beat FX BPM), not a deck.
 *
 * Lifetime: a BpmState has no vtable (that same `stp xzr` leaves +0 zero). It is made with
 * std::make_shared: its only caller (0x1fca41c; 2.00 0x2137aec) allocates 0xd0 bytes, sets the
 * use and weak counts to {1, 1}, stores the vtable of
 * std::_Sp_counted_ptr_inplace<gui::BpmState, ..., _S_atomic> (RTTI) and constructs the object
 * 0x10 bytes in. So the object is owned through shared_ptr, and EP147's own protocol applies:
 *   - The ctor hook (EP147's thread, inside its call) checks that control-block vtable and takes
 *     a weak reference (an atomic increment of the weak count, as weak_ptr's copy does). While
 *     we hold it the block, and the object in it, is never freed, whatever EP147 does.
 *   - A reader trusts the object only while the use count is above zero: shared_ptr disposes
 *     only after the count reaches zero, and nothing raises it again from zero. The count is
 *     read before and after the fields, so a read that overlapped the disposal is dropped.
 *   - Each weak reference is kept for the life of the process (releasing one could free the
 *     block under a reader on another thread). One per BpmState construction, 208 bytes each,
 *     capped at BPMSTATE_PIN_MAX; past the cap a new BpmState is not used (unknown).
 * Nothing here dereferences an object that may be gone, and nothing reads through
 * /proc/self/mem. */
#define BPMSTATE_CTOR EP_1f5da20 /* gui::BpmState::BpmState(self, int deck) */
#define BPMSTATE_DECK 0x08       /* low byte of the u64 = the ctor's deck arg */
#define BPMSTATE_TRACK 0xb0      /* double: analyzed track BPM */
#define BPMCTRL_VT EP_2dede68    /* _Sp_counted_ptr_inplace<gui::BpmState,...> vtable (RTTI) */
#define BPMCTRL_OFF 0x10         /* the object sits this far into its control block */
#define BPMCTRL_USE 0x08         /* int: shared (use) count */
#define BPMCTRL_WEAK 0x0c        /* int: weak count */
#define BPMSTATE_PIN_MAX 64
static void (*g_orig_bpmctor)(long self, long deck, long a3, long a4);
/* plain types, every access through __atomic builtins */
static unsigned long g_bpmstate[AZMOD_STEM_DECKS];    /* pinned BpmState, 0 = none */
static unsigned long g_bpmstate_id[AZMOD_STEM_DECKS]; /* its +0x08 word, from the ctor */
static int g_bpmstate_n = 0, g_bpmstate_pins = 0, g_bpmstate_refused = 0;

/* EP147's thread, inside BpmState's ctor (the object is alive and ours to read here). Returns
 * 1 when the object was pinned and published for deck d. */
static int bpmstate_capture(unsigned long self, int d, uintptr_t want_vt) {
    if (d < 0 || d >= AZMOD_STEM_DECKS || !self || (self & 7) || !want_vt)
        return 0;
    unsigned long ctrl = self - BPMCTRL_OFF;
    if (*(volatile uintptr_t *) ctrl != want_vt)
        return 0; /* not the make_shared block we know: leave it alone */
    unsigned long id = *(volatile unsigned long *) (self + BPMSTATE_DECK);
    if ((id & 0xff) != (unsigned long) d)
        return 0;
    if (__atomic_add_fetch(&g_bpmstate_pins, 1, __ATOMIC_RELAXED) > BPMSTATE_PIN_MAX) {
        __atomic_add_fetch(&g_bpmstate_refused, 1, __ATOMIC_RELAXED);
        return 0;
    }
    __atomic_add_fetch((int *) (ctrl + BPMCTRL_WEAK), 1, __ATOMIC_ACQ_REL); /* our weak_ptr */
    __atomic_store_n(&g_bpmstate_id[d], id, __ATOMIC_RELAXED);
    __atomic_store_n(&g_bpmstate[d], self, __ATOMIC_RELEASE);
    return 1;
}
/* Any thread. Deck d's live (and analyzed) BPM, 1 on a read made while the object was alive and
 * still deck d's; 0 = unknown. track may be NULL. */
static int bpmstate_read(int d, double *live, double *track) {
    if (d < 0 || d >= AZMOD_STEM_DECKS)
        return 0;
    unsigned long obj = __atomic_load_n(&g_bpmstate[d], __ATOMIC_ACQUIRE);
    if (!obj)
        return 0;
    const int *use = (const int *) (obj - BPMCTRL_OFF + BPMCTRL_USE);
    if (__atomic_load_n(use, __ATOMIC_ACQUIRE) <= 0)
        return 0; /* disposed: EP147 has let go of it */
    unsigned long id = __atomic_load_n((const unsigned long *) (obj + BPMSTATE_DECK), __ATOMIC_RELAXED);
    uint64_t lv = __atomic_load_n((const uint64_t *) (obj + BPMSTATE_LIVE), __ATOMIC_RELAXED);
    uint64_t tv = __atomic_load_n((const uint64_t *) (obj + BPMSTATE_TRACK), __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(use, __ATOMIC_RELAXED) <= 0)
        return 0; /* disposed while we read */
    if (id != __atomic_load_n(&g_bpmstate_id[d], __ATOMIC_RELAXED))
        return 0;
    memcpy(live, &lv, sizeof *live);
    if (track)
        memcpy(track, &tv, sizeof *track);
    return 1;
}

static void bpmctor_hook(long self, long deck, long a3, long a4) {
    g_orig_bpmctor(self, deck, a3, a4); /* the deck byte only exists afterwards */
    int d = (int) (deck & 0xff);
    int pinned = bpmstate_capture((unsigned long) self, d, BPMCTRL_VT);
    __atomic_add_fetch(&g_bpmstate_n, 1, __ATOMIC_RELAXED);
    FILE *f = azlog_open("/tmp/azmod-drumbpm.log", "a");
    if (f) {
        fprintf(f, "bpmctor: self=%#lx deck(arg)=%d -> %s (pins %d, refused %d)\n",
                (unsigned long) self, d, pinned ? "pinned" : "NOT USED (block or deck mismatch, or cap)",
                __atomic_load_n(&g_bpmstate_pins, __ATOMIC_RELAXED),
                __atomic_load_n(&g_bpmstate_refused, __ATOMIC_RELAXED));
        fclose(f);
    } /* flush every line */
}

/* Constructor phase (install_static_hooks): installed before EP147 can build a BpmState. */
static void install_bpmstate_hook(void) {
    int rc = azm_install_hook(BPMSTATE_CTOR, (void *) bpmctor_hook, (void **) &g_orig_bpmctor);
    FILE *f = azlog_open("/tmp/azmod-drumbpm.log", "a");
    if (f) {
        fprintf(f, "install bpmctor=%d (1=ok, -1=declined)\n", rc);
        fclose(f);
    }
}

/* The BPM the drum roll is sent for deck d, from that deck's BpmState live BPM (`live`) and
 * the master deck's (`master_live`, valid only when `master_ok`). A deck with a track keeps
 * its own BPM; one whose BPM is out of range sends 0 (azstem's "unknown", 120). An empty deck
 * (DBL_MAX, EP147's "no track") takes the master deck's BPM, so a roll on it stays in time
 * with what is playing; no known master, or a master without a valid BPM (the master
 * being this empty deck included) sends 0 = 120. The master comes from g_master_deck, never from a guess. */
static float drum_feed_bpm(int d, double live, int master, int master_ok, double master_live) {
    (void) d;
    if (live > 20.0 && live < 400.0)
        return (float) live;
    if (live != DBL_MAX || master < 0 || master >= AZMOD_STEM_DECKS || !master_ok)
        return 0.0f;
    return (master_live > 20.0 && master_live < 400.0) ? (float) master_live : 0.0f;
}
/* The deck whose beat grid and playhead place deck d's roll (azstem_drum_follow): an empty
 * deck follows the master deck exactly when it rolls at the master's BPM (drum_feed_bpm), so
 * tempo and phase come from the same deck; anything else follows itself (-1). */
static int drum_feed_lead(int d, double live, int master, int master_ok, double master_live) {
    if (live != DBL_MAX || master == d)
        return -1;
    return drum_feed_bpm(d, live, master, master_ok, master_live) > 0.0f ? master : -1;
}

/* Master deck: MasterPlayer::onExchangedMasterPost(SyncMemberID) 0xaabb78 runs when the master
 * changes; word 0 of its argument = {u32 1, u32 deck} (0xfffe = none). Inline-hooked: other
 * bases reach it through thunks that call it directly, so a vtable-slot hook would miss
 * callers. Read by the Phase Meter (the master's ruler is orange) and the drum roll (an empty
 * deck rolls at the master's BPM), so it belongs to neither: installed for everyone, always
 * recording, gated by no mod. One address takes one inline hook (the second install sees a B
 * to the first's island and declines), so nothing else may hook this function. */
#define MASTER_EXCHANGED EP_aabb78
static long (*g_orig_master_xm)(void *, void *);
static _Atomic int g_master_deck = -1;
static long master_xm_hook(void *self, void *id) {
    long r = g_orig_master_xm(self, id);
    uintptr_t w = 0;
    int m = -1;
    /* id: this call's own argument (const SyncMemberID&), live for the call */
    if (id && own_q((uintptr_t) id, &w) && (uint32_t) w == 1 && (w >> 32) < AZMOD_STEM_DECKS)
        m = (int) (w >> 32);
    if (__atomic_exchange_n(&g_master_deck, m, __ATOMIC_RELAXED) != m) {
        FILE *f = azlog_open("/tmp/azmod-drumbpm.log", "a");
        if (f) {
            fprintf(f, "master: deck%d (%s)\n", m + 1, m < 0 ? "none" : "set");
            fclose(f);
        }
    }
    return r;
}
static void install_master_hook(void) {
    int rc = azm_install_hook(MASTER_EXCHANGED, (void *) master_xm_hook, (void **) &g_orig_master_xm);
    FILE *f = azlog_open("/tmp/azmod-drumbpm.log", "a");
    if (f) {
        fprintf(f, "install master=%d (1=ok, -1=declined)\n", rc);
        fclose(f);
    }
}
static volatile int g_wave_forcestems = 0; /* driven by the STEMS master toggle (arm_stems) */
/* Native mode: show EP147's own PadStemWidget and let the forged refresh populate
 * it, rather than blanking the grid for an overlay. The overlay's stem strips are
 * suppressed at the same time (menu_composite.c), so there is no overlay window whose late
 * mapping could let native captions flash through. */
volatile int azmod_stem_native = 0;       /* default on; azmod-stemnative.off, read at init */
static void stemref_hook(long w);         /* fwd: refresh through the forge */
static void (*g_orig_stemref)(long);
static void (*g_orig_stemref_fwd)(long);  /* set once the hook is installed */
static void wave_setvis(long w, int on) { /* JUCE Component::setVisible, vtable+0x60 */
    (*(void (**)(long, int))(*(long *) w + 0x60))(w, on);
}
/* Take over one pad tile's per-mode children (+0xe0..+0x110; PadStemWidget is
 * +0x110).
 *
 * Native mode: refresh PadStemWidget through the forge and show it in place of
 * the other six.
 *
 * Overlay mode: hide all seven so stem_overlay.c (draw_stem_grid) draws the whole grid.
 * The native stem captions are static and render even with no track, and they
 * would show for a beat before the overlay window maps, so nothing native may
 * paint there. The tile refresh (FUN_01df69f8) is not called at all on this path. */
static void wave_force(long self) {
    static const long offs[7] = {0xe0, 0xe8, 0xf0, 0xf8, 0x100, 0x108, 0x110};
    /* Drum roll never forces the stems grid. The stem arts are shared decoded
     * assets, so relabeling them would also relabel the Beat Loop stems grid.
     * SLIP LOOP has its own art instead: eight {down,normal} pairs at
     * 0x3b35fb8..0x3b36047, ending where the stems array begins (0x3b36048).
     * Tiles 3-6 there are slip-loop-only copies (the beat-loop 1_4/1_2/1/2 art
     * is at 0x2d843c0/0x2d83e10/0x2d83c30/0x2d83930), so pngpatching that block
     * changes SLIP LOOP and nothing else. The stem arts (0x2d92220..0x2d95700)
     * are disjoint from every patched address (0x2d8dcb0..0x2d8f570). */
    int dg_grid = 0; /* drum roll never forces the stems grid */
    if (azmod_stem_native || dg_grid) {
        /* Order matters: refresh first, then hide the other six children, then
         * show PadStemWidget last. Refreshing after setVisible leaves a populated
         * widget that is never re-laid-out (a black rectangle). The refresh goes
         * through stemref_hook so the forge opens its three gates. */
        long stem = *(long *) (self + 0x110);
        stemref_hook(stem);
        for (int i = 0; i < 6; i++)
            wave_setvis(*(long *) (self + offs[i]), 0);
        wave_setvis(stem, 1);
        return;
    }
    for (int i = 0; i < 7; i++)
        wave_setvis(*(long *) (self + offs[i]), 0);
}
/* Stems replace the pad grid only on the stems Beat Loop page (matching the
 * pad->stem routing). The selector at +0xd0 returns the pad-mode enum; Beat Loop
 * is {3,4} -> PadBeatLoopWidget (child +0xe0; +0xf0 is PadCueWidget = HOT CUE,
 * both by RTTI). Every other mode stays native. */
static int wave_is_beatloop(long self) {
    int m = ((int (*)(long)) WAVE_MODEGET)(*(long *) (self + 0xd0));
    return m == stems_bl_mode(); /* one page only; the other stays native */
}
/* The pad container's PadStemWidget (+0x110), stored per deck when the view is
 * built or switched so the refresh can be poked outside those hooks. Per deck,
 * never one global: last-writer-wins would act on the wrong deck's widgets. The
 * deck is deck_for_modeobj(*(long *)(container + 0xd0)); each deck's per-deck
 * object anchors a distinct value there at +0x10b0. */

static _Atomic long g_stem_widget[AZMOD_STEM_DECKS];
static _Atomic long g_slip_ctr[4];      /* all four grid containers */
/* Tile deckstate -> deck, from the per-deck table only (PERDECK_TILEDS: each deck's
 * per-deck object holds its tile deckstate). Until tileowner_hook has filled the table,
 * or for a deckstate no live deck holds, the answer is -1, never a guess: an order of
 * arrival is not an identity (4-DECK stacks its rows DECK 3,1,2,4), and a wrong deck
 * would file another deck's widgets under g_tile_w and paint its state onto them.
 * Callers do nothing for -1 (stem_tile_fix_one leaves the tile alone; the g_tile_w
 * store requires grp >= 0). */
static int deck_for_ds(long ds) {
    return deck_for_perdeck_field(ds, PERDECK_TILEDS);
}
/* A pad container's +0xd0 mode object -> its deck, from the per-deck table only
 * (PERDECK_MODEOBJ). -1 before the table is filled, and for an object no container
 * holds (e.g. the pad-title field's own mode object, a different widget tree). */
static int deck_for_modeobj(long p) {
    return deck_for_perdeck_field(p, PERDECK_MODEOBJ);
}
/* There is deliberately no "any deck has stems" helper: it would let deck 2's
 * BEAT LOOP show deck 1's stems grid. Every question here is about one deck, and
 * an unknown deck means do nothing. */
/* Per-tile widget pointers captured from the refresh, which runs once per tile.
 * Keyed [deck_for_ds(tile ds)][slot], slot = [w+0xc8] before the forge rewrites
 * it (0..7, left to right, top row then bottom), so one tile can be driven
 * alone. Every deck's tiles reuse the same slot numbers, hence the deck key.
 * Pointers can dangle after a track load: check with tilew_alive() before use. */
static _Atomic long g_tile_w[AZMOD_STEM_DECKS][8];
static _Atomic int g_anchor_budget = 8;
/* perdeck_obj[deck], from tileowner_hook (FUN_01c1fad0 receives deck and object
 * in one call). Feeds the deck census (deck_for_perdeck_field) and the anchor
 * scan. Used only through perdeck_block(), never raw.
 *
 * What the blocks are (measured on firmware 2.00): the four per-player blocks of one
 * gui::PlayerInfoState, block d = state + d * PERDECK_STRIDE, word 0 = the player index d.
 * The state is a make_shared object: its control block (vtable PISCTRL_VT, RTTI
 * _Sp_counted_ptr_inplace<gui::PlayerInfoState>) sits PISCTRL_OFF before it, with a
 * positive use count while EP147 holds the state.
 * perdeck_capture pins it with a weak reference (as bpmstate_capture does a BpmState): with
 * make_shared the object lives in the control block's allocation, so the memory stays ours
 * and is never reused while the pin is held. A use reads a block only while the use count is
 * above zero and the block still names its deck. */
static _Atomic long g_perdeck[AZMOD_STEM_DECKS];
#define PISCTRL_VT EP_2defe68 /* _Sp_counted_ptr_inplace<gui::PlayerInfoState,...> (RTTI) */
#define PISCTRL_OFF 0x10      /* the state sits this far into its control block */
#define PISCTRL_USE 0x08      /* int: shared (use) count */
#define PISCTRL_WEAK 0x0c     /* int: weak count */
#define PIS_PIN_MAX 4         /* a new state per EP147 start is expected: one */
static _Atomic unsigned long g_pis;  /* the pinned PlayerInfoState, 0 = none */
static _Atomic int g_pis_pins;
/* tileowner_hook, inside EP147's call (the block is live): file block `blk` as deck d's when
 * it is block d of a PlayerInfoState we know and hold a weak reference on. 0 = not filed. */
static int perdeck_capture(long blk, int d) {
    uintptr_t b = (uintptr_t) blk, w0 = 0, vt = 0;
    if (d < 0 || d >= AZMOD_STEM_DECKS || !own_q(b, &w0) || w0 != (uintptr_t) d)
        return 0;
    uintptr_t state = b - (uintptr_t) d * PERDECK_STRIDE, ctrl = state - PISCTRL_OFF;
    if (!PISCTRL_VT || !own_q(ctrl, &vt) || vt != (uintptr_t) PISCTRL_VT)
        return 0;
    if (__atomic_load_n(&g_pis, __ATOMIC_ACQUIRE) != state) {
        if (__atomic_add_fetch(&g_pis_pins, 1, __ATOMIC_RELAXED) > PIS_PIN_MAX)
            return 0; /* past the cap: unknown */
        __atomic_add_fetch((int *) (ctrl + PISCTRL_WEAK), 1, __ATOMIC_ACQ_REL); /* our weak_ptr */
        __atomic_store_n(&g_pis, state, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&g_perdeck[d], blk, __ATOMIC_RELEASE);
    return 1;
}
/* Deck d's per-player block, or 0 = unknown: block d of the pinned state, the state not
 * disposed (use count > 0), its control block's vtable and the block's own player index
 * checked at this use. A block that fails is unfiled. Plain loads: the pin keeps the memory. */
static uintptr_t perdeck_block(int d) {
    if (d < 0 || d >= AZMOD_STEM_DECKS)
        return 0;
    uintptr_t b = (uintptr_t) __atomic_load_n(&g_perdeck[d], __ATOMIC_ACQUIRE);
    uintptr_t state = __atomic_load_n(&g_pis, __ATOMIC_ACQUIRE);
    if (!b || !state || b != state + (uintptr_t) d * PERDECK_STRIDE || (b & 7))
        return 0;
    uintptr_t ctrl = state - PISCTRL_OFF;
    if (__atomic_load_n((const int *) (ctrl + PISCTRL_USE), __ATOMIC_ACQUIRE) <= 0 ||
        *(volatile uintptr_t *) ctrl != (uintptr_t) PISCTRL_VT ||
        *(volatile uintptr_t *) b != (uintptr_t) d) {
        long e = (long) b;
        __atomic_compare_exchange_n(&g_perdeck[d], &e, 0, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        return 0;
    }
    return b;
}
/* Deck census. Both objects the pad/tile path needs are fields of the per-deck
 * object, each unique to one deck (measured across EP147 starts):
 *     per-deck object + 0x10b0 -> pad container's mode object (container +0xd0)
 *     per-deck object + 0x1160 -> tile deckstate (tile +0xd0)
 */
/* True once tileowner_hook has filled the table (diagnostics only: the lookups below
 * answer -1 by themselves until it is). */
static int perdeck_table_ready(void) {
    return __atomic_load_n(&g_perdeck[0], __ATOMIC_RELAXED) > 0x10000;
}
static int deck_for_perdeck_field(long p, long off) {
    if (p <= 0x10000 || off < 8 || off > PERDECK_STRIDE - 8 || (off & 7))
        return -1;
    for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
        uintptr_t base = perdeck_block(d); /* identity checked at this use; 0 = unknown */
        if (base && *(volatile long *) (base + (uintptr_t) off) == p)
            return d;
    }
    return -1; /* not populated, or no live block holds p: unknown, never a default deck */
}
/* Diagnostic: is this deckstate pointer held as a field of some deck's per-deck
 * object? Scans each PERDECK_STRIDE block for a matching qword and logs deck and
 * offset. Bounded (2560 reads), budgeted, and off the sample path (it runs per
 * tile refresh). */
static void ds_anchor_scan(long tds) {
    (void) tds; /* the anchor scan is a development diagnostic (reads a whole block, logs) */
    return;
}
static void waveswitch_hook(long self) {
    /* modeget_hook feeds EP147's dispatcher 0x0b (the stems case) when STEMS is
     * armed and the call comes from inside WAVE_SWITCH, so the native dispatcher
     * can select and build PadStemWidget itself.
     *
     * No-stem gate: only a deck whose loaded track has stems gets its grid
     * replaced. A no-stem deck runs the native dispatch and keeps its Beat Loop
     * pads and title; its LEDs carry the no-stems indicator instead. */
    int deck = deck_for_modeobj(*(long *) (self + 0xd0));
    ds_anchor_scan(*(long *) (self + 0xd0)); /* is the container ds anchored too? */
    /* Probe (budgeted): logs each term of the force condition below, to show
     * which one rejects the stems grid. */
    {
        static _Atomic int probe_budget = 24;
        if (wave_is_beatloop(self) && __atomic_fetch_sub(&probe_budget, 1, __ATOMIC_RELAXED) > 0) {
            FILE *pf = azlog_open("/tmp/azmod-wswitch.log", "a");
            if (pf) {
                fprintf(pf, "wswitch: modeobj=%lx deck=%d force=%d bl=%d avail=%d%d table=%d\n",
                        (unsigned long) *(long *) (self + 0xd0), deck,
                        __atomic_load_n(&g_wave_forcestems, __ATOMIC_ACQUIRE),
                        wave_is_beatloop(self), g_state.stems_available[0],
                        g_state.stems_available[1], perdeck_table_ready());
                fclose(pf);
            }
        }
    }
    /* Unknown deck keeps its native grid. Forcing it on "some deck has stems"
     * would show another deck's stems grid here, flip this deck's source pane to
     * MIDI and gray its beat-grid markers. */
    int avail = deck >= 0 ? (g_state.stems_available[deck] && !bl_on_native_vpage(deck)) : 0;
    int dg_grid = 0; /* drum roll never forces the stems grid */
    if ((__atomic_load_n(&g_wave_forcestems, __ATOMIC_ACQUIRE) && wave_is_beatloop(self) &&
         avail) ||
        dg_grid)
        wave_force(self);
    else
        g_orig_waveswitch(self);
    /* Remember this deck's widget and container. The diagnostic poke below goes
     * through stemref_hook so the forge applies. */
    if (deck >= 0 && deck < AZMOD_STEM_DECKS) {
        __atomic_store_n(&g_stem_widget[deck], *(long *) (self + 0x110), __ATOMIC_RELAXED);
        disp_wref_init();
        wref_set(WSLOT(&g_stem_container[deck]), (uintptr_t) self); /* cleared by its destructor */
    }
    {
        for (int i = 0; i < 4; i++) { /* remember every grid container */
            long e = __atomic_load_n(&g_slip_ctr[i], __ATOMIC_RELAXED);
            if (e == self)
                break;
            if (!e) {
                __atomic_store_n(&g_slip_ctr[i], self, __ATOMIC_RELAXED);
                break;
            }
        }
    }
    if (AZ_PROBE_ON("/tmp/azmod-stemdiag") && g_orig_stemref_fwd)
        stemref_hook(*(long *) (self + 0x110));
}
/* ---- Mode- and stems-independent slot-7 tile capture --------------------
 * The DRUM ROLL tile's color swap needs a handle on the tile to repaint. The
 * stemref_hook capture fires only in BEAT LOOP with stems engaged, so drum roll
 * must not borrow it (on a track without stems the tile would stay red).
 *
 * WAVE_CTOR's third argument is a pad SLOT 0..7, not a deck, despite the hook's
 * parameter name: at startup it runs 32 times, four groups of eight, with
 * self+0xc8 == arg, one vtable (0x2d8f880) and a constant +0xd0 deckstate per
 * group. Mode switches cause no further calls; tiles are built once and
 * re-skinned, so there is no per-mode slot-7 widget to miss. The handle is
 * `self` when the slot read from the object (+0xc8) is 7.
 *
 * Keyed by deckstate so each grid keeps one entry and a rebuild replaces it.
 * g_slot7_w and g_slip_tile are wref slots (ep_mem.h): the tile's destructor clears them,
 * so a non-zero entry read on the message thread is a live tile; its class is still
 * checked by vtable at every use. */
/* PADTILE_VT (PadStatusWidget vtable): azmod_internal.h, shared with the drum roll tiles */
static _Atomic long g_slot7_w[SLOT7_MAX];
static _Atomic long g_slot7_ds[SLOT7_MAX];
static _Atomic long g_padgrid_ds[SLOT7_MAX];          /* grid identity for g_slip_tile */
static _Atomic long g_slip_tile[AZMOD_STEM_DECKS][8]; /* our tiles, by (grid,slot) */

static _Atomic long g_dg8_stem[4]; /* fwd: defined with the tile-8 block */
static void slot7_capture(long self) { /* self: the tile its ctor just built (wavector_hook) */
    uintptr_t v;
    long ds = own_q((uintptr_t) self + 0xd0, &v) ? (long) v : 0;
    disp_wref_init();
    for (int i = 0; i < SLOT7_MAX; i++) {
        long e = __atomic_load_n(&g_slot7_ds[i], __ATOMIC_RELAXED);
        if (e == ds || e == 0) {
            __atomic_store_n(&g_slot7_ds[i], ds, __ATOMIC_RELAXED);
            wref_set(WSLOT(&g_slot7_w[i]), (uintptr_t) self);
            return;
        }
    }
    __atomic_store_n(&g_slot7_ds[0], ds, __ATOMIC_RELAXED);
    wref_set(WSLOT(&g_slot7_w[0]), (uintptr_t) self);
}

/* Opt-in probe of WAVE_CTOR's third argument. 0x01df6240 is called 8 times per
 * grid from 0x01df4a78 with the constants 0..7, so `self` is one tile widget
 * (slot @+0xc8, ds @+0xd0, component @+0xf0). The log distinguishes the two
 * readings: a slot shows arg cycling 0..7 with self+0xc8 == arg, a deck would
 * show only 0/1 with +0xc8 unrelated. Budgeted, own log file. */
static void wavector_probe(long self, long *ctx, unsigned arg) {
    if (!AZ_PROBE_ON("/home/root/settings/azmod-tilector.on"))
        return;
    static _Atomic int plog = 40;
    if (__atomic_fetch_sub(&plog, 1, __ATOMIC_RELAXED) <= 0)
        return;
    long c8 = -1, d0 = -1, f0 = -1, vt = -1;
    uintptr_t v;
    /* self: the tile its ctor just built, in this call. 0xc8 is 8-aligned, so the slot is
     * the low half of that quad. */
    if (own_q((uintptr_t) self, &v))
        vt = (long) v;
    if (own_q((uintptr_t) self + 0xc8, &v))
        c8 = (long) (int) (unsigned) v;
    if (own_q((uintptr_t) self + 0xd0, &v))
        d0 = (long) v;
    if (own_q((uintptr_t) self + 0xf0, &v))
        f0 = (long) v;
    FILE *f = azlog_open("/tmp/azmod-tilector.log", "a");
    if (f) {
        fprintf(f, "CTOR: self=%#lx arg=%u vt=%#lx +0xc8=%ld +0xd0=%#lx +0xf0=%#lx ctx=%p\n",
                (unsigned long) self, arg, (unsigned long) vt, c8, (unsigned long) d0,
                (unsigned long) f0, (void *) ctx);
        fclose(f);
    }
}
static void wavector_hook(long self, long *ctx, unsigned deck) {
    g_orig_wavector(self, ctx, deck); /* build this pad tile and its mode children */
    wavector_probe(self, ctx, deck);
    /* Slot read from the object, not the parameter: `deck` is really a pad slot
     * 0..7 (see slot7_capture). */
    {
        uintptr_t sv, dv; /* self: the tile its ctor just built, in this call */
        if (own_q((uintptr_t) self + 0xc8, &sv)) {
            int slot = (int) (unsigned) sv;
            long ds = own_q((uintptr_t) self + 0xd0, &dv) ? (long) dv : 0;
            if (slot == 7) {
                slot7_capture(self);
                /* remember this grid's STEM child: its ImagesComponent index is
                 * the tile-8 red/green switch (0x18 down-green / 0x19 normal-red) */
                for (int i = 0; i < SLOT7_MAX; i++) {
                    long e = __atomic_load_n(&g_slot7_ds[i], __ATOMIC_RELAXED);
                    if (e == ds || e == 0) {
                        long stem = *(long *) (self + 0x110);
                        __atomic_store_n(&g_dg8_stem[i], stem, __ATOMIC_RELAXED);
                        break;
                    }
                }
            }
        }
    }
    __atomic_store_n(&g_stem_model, 0, __ATOMIC_RELAXED);  /* old tiles are gone */
    __atomic_store_n(&g_cap_budget, 64, __ATOMIC_RELAXED); /* re-arm diagnostics */
    __atomic_store_n(&g_anchor_budget, 8, __ATOMIC_RELAXED);
    for (int g = 0; g < AZMOD_STEM_DECKS; g++)
        for (int k = 0; k < 8; k++)
            __atomic_store_n(&g_tile_w[g][k], 0, __ATOMIC_RELAXED);
    /* Capture this tile by (grid, slot) into its own array, never g_tile_w.
     * g_tile_w belongs to the stems path and holds the refresh's argument (a
     * mode child); putting a PadStatusWidget there would feed it to
     * stem_tile_fix_one / stemref_hook, which expect children. Filled after the
     * clear above so no handle from a rebuilt grid survives. */
    {
        uintptr_t sv, dv;
        if (own_q((uintptr_t) self + 0xc8, &sv)) {
            int slot = (int) (unsigned) sv;
            long ds = own_q((uintptr_t) self + 0xd0, &dv) ? (long) dv : 0;
            int g = -1;
            for (int i = 0; i < SLOT7_MAX; i++) {
                long e = __atomic_load_n(&g_padgrid_ds[i], __ATOMIC_RELAXED);
                if (e == ds || e == 0) {
                    __atomic_store_n(&g_padgrid_ds[i], ds, __ATOMIC_RELAXED);
                    g = i;
                    break;
                }
            }
            if (g >= 0 && g < AZMOD_STEM_DECKS && slot >= 0 && slot < 8) {
                disp_wref_init();
                wref_set(WSLOT(&g_slip_tile[g][slot]), (uintptr_t) self);
            }
        }
    }
    /* Deliberately no g_pad_mode_bl seed here: `deck` is a pad slot, and
     * wave_is_beatloop(self) describes this tile, not a deck's pad mode. The
     * only writer is the ERP pad-mode button edge handler, which keeps
     * g_pad_mode_bl[d] and g_pad_mode_slip[d] mutually exclusive per deck. A
     * seed that disagreed with the slip flag would let a pad-8 press fall out of
     * the drum-roll branch into the stems branch and raise a drums SOLO. */
    /* No-stem gate, same rule as waveswitch_hook: force the stems grid only for
     * a deck whose track has stems. */
    /* `deck` is a pad slot, so the deck comes from this tile's deckstate in the per-deck
     * table; unknown (table not up yet) forces nothing. */
    int dk = deck_for_perdeck_field(*(long *) (self + 0xd0), PERDECK_TILEDS);
    int dg_grid2 = 0; /* drum roll never forces the stems grid */
    if ((__atomic_load_n(&g_wave_forcestems, __ATOMIC_ACQUIRE) && wave_is_beatloop(self) &&
         dk >= 0 && dk < AZMOD_STEM_DECKS && g_state.stems_available[dk] &&
         !bl_on_native_vpage(dk)) ||
        dg_grid2)
        wave_force(self);
    /* Store the stems widget; the diagnostic poke goes through the forge */
    if (dk >= 0 && dk < AZMOD_STEM_DECKS)
        __atomic_store_n(&g_stem_widget[dk], *(long *) (self + 0x110), __ATOMIC_RELAXED);
    if (AZ_PROBE_ON("/tmp/azmod-stemdiag") && g_orig_stemref_fwd)
        stemref_hook(*(long *) (self + 0x110));
}
/* Pad-mode getter hook. The same mode-apply dispatcher (WAVE_SWITCH) drives the
 * pad grid and the title strip's per-mode children
 * (PadModeStatusFieldWidget::paint 0x1df2ce0 -> 0x1df4a18 -> 0x1df6010), reading
 * the mode through this getter. Returning the stems enum only to callers inside
 * WAVE_SWITCH or TITLE_UPDATE (lr filter), while STEMS is armed, sends both down
 * EP147's native stems case: stems tiles and stems title, no glyph work. Every
 * other caller, including wave_is_beatloop, sees the real mode. */
static int (*g_orig_modeget)(long);
/* Is this getter call the deck screen asking? Only its own widgets are: the pad-mode
 * dispatcher (WAVE_SWITCH) and the pad title (TITLE_UPDATE). azmod's own reads
 * (wave_is_beatloop from waveswitch_hook) also run while EP147 builds its views at
 * startup, before any screen is shown, so they say nothing about what is on screen. */
static int modeget_from_deck_screen(uintptr_t lr) {
    return (lr >= WAVE_SWITCH && lr < WAVE_SWITCH + 0x230) ||
           (lr >= TITLE_UPDATE && lr < TITLE_UPDATE_END);
}
static int modeget_hook(long p) {
    /* Read the caller's return address before making any call: on aarch64
     * every `bl` overwrites x30, so reading it later would yield an address
     * inside this function, which never matches an EP147 window. */
    uintptr_t caller_lr;
    __asm__ volatile("mov %0, x30" : "=r"(caller_lr));
    unsigned seq =
        __atomic_load_n(&g_mode_seq, __ATOMIC_ACQUIRE); /* before the read: stems_page freshness */
    int m = g_orig_modeget(p);
    __atomic_store_n(&g_true_mode, m, __ATOMIC_RELAXED);
    /* ...and per deck, for "is this deck in slip loop" questions. The pad-title
     * field's own mode object resolves to -1 and does not publish. */
    {
        int dk = deck_for_modeobj(p);
        if (dk >= 0 && dk < AZMOD_STEM_DECKS) {
            __atomic_store_n(&g_true_mode_deck[dk], m + 1, __ATOMIC_RELEASE);
            __atomic_store_n(&g_tm_seq[dk], seq, __ATOMIC_RELEASE);
        }
    }
    int from_deck_screen = modeget_from_deck_screen(caller_lr);
    if (from_deck_screen)
        wave_mark(); /* the deck screen's widgets read the mode only while it is up */
    /* Hijack only when the real mode is the stems Beat Loop page; otherwise HOT
     * CUE, GATE CUE and the rest would render the stems tile set too. */
    if (__atomic_load_n(&g_force_truemode, __ATOMIC_ACQUIRE))
        return m; /* rebuild flip */
    if (__atomic_load_n(&g_wave_forcestems, __ATOMIC_ACQUIRE) &&
        m == stems_bl_mode()) { /* the stems page only */
        /* No-stem gate, per deck, for both consumers: the grid's children via
         * WAVE_SWITCH and the grid's title via TITLE_UPDATE. Both pass a mode
         * object anchored at that deck's per-deck +0x10b0. */
        int deck = deck_for_modeobj(p);
        if (deck >= 0 && g_state.stems_available[deck] &&
            !bl_on_native_vpage(deck)) { /* unknown => native, see waveswitch_hook */
            if (from_deck_screen)
                return 0x0b; /* stems-basic enum: native case in the dispatcher */
        }
    }
    /* Deliberately no drum-roll hijack of SLIP LOOP to 0x0b: SLIP LOOP keeps its
     * own grid (pngpatched slip art plus an index write). Forcing PadStemWidget
     * onto a deck whose stem state was not set up for it crashes EP147. */
    {
        static int last;
        if (m != last) {
            last = m;
            static _Atomic int mlog = 30;
            if (__atomic_fetch_sub(&mlog, 1, __ATOMIC_RELAXED) > 0) {
                FILE *f = azlog_open("/tmp/azmod-drumtile.log", "a");
                if (f) {
                    fprintf(f, "MODE m=%d\n", m);
                    fclose(f);
                }
            }
        }
    }
    /* BEAT JUMP 2 First: each page's beat-jump tiles show the other page's
     * lengths, matching the swapped pad action (bj_padpress_hook). Only the tile
     * refresh's own call is swapped; every other caller, and the published true
     * mode, see EP147's real 5/6. */

    if ((m == 5 || m == 6) && caller_lr == BJ_TILE_RA && az_on(AZF_BEAT_JUMP_2_FIRST))
        return 11 - m;
    return m;
}
/* ---- The forge ---------------------------------------------------------
 * The refresh picks the STEMS tile set only if both hold:
 *   1. [ds+0x10]->vtable[+0x18](&out) leaves out.value (the byte at +8) != 0
 *   2. [ds+0x40]->vtable[+0x10]() == 1     (host-link cert; already NOP'd)
 * Condition 1 is a single slot. Disassembled:
 *
 *   0x00aa4bd8  ldrb w1,[x0,#0x58]      <- the switcher's own state byte
 *               adrp/add x0,#0x25e9ba0  <- IPcAppliSetMode::Mode static info
 *               str  x0,[x8]            <- sret: out.modeinfo
 *               strb w1,[x8,#8]         <- sret: out.value
 *               ret
 *
 * so the returned struct is connection_with_hostapp::IPcAppliSetMode::Mode
 * = { const void *modeinfo; uint8_t value; } and UsecasePcAppliModeSwitcher
 * merely republishes [this+0x58].
 *
 * stemref_hook therefore swaps in an object of our own whose slot +0x18 says
 * "mode 1" for the duration of one refresh call only, then restores the real
 * pointer. Slot +0x10 answers 1, which also passes the AlphaThetaCert host-link
 * gate (0x1df6ab8) without patching EP147 code. Nothing shared is left mutated.
 *
 * This populates the native tiles, so it belongs to native mode only; the
 * overlay's stem strips must be off when it runs. The value is a constant 1
 * because the gate means "a PC-appli mode is set"; per-stem state lives
 * elsewhere (the tile image index). */
/* Per build: set at runtime rather than as a movz/movk literal in the asm, so the address tables see it */
uintptr_t g_forge_modeinfo __attribute__((visibility("hidden")));
__asm__(".text\n.align 2\n"
        ".globl azm_forge_mode\n"
        "azm_forge_mode:\n"
        "   adrp x0, g_forge_modeinfo\n"             /* IPcAppliSetMode::Mode static info,  */
        "   ldr  x0, [x0, :lo12:g_forge_modeinfo]\n" /* set from EP_25e9ba0 (C) */
        "   str  x0, [x8]\n"
        "   mov  w1, #1\n"
        "   strb w1, [x8, #8]\n"
        "   ret\n"
        ".globl azm_forge_true\n" /* slot +0x10: the host-link gate */
        "azm_forge_true:\n"
        "   mov  w0, #1\n"
        "   ret\n"
        ".globl azm_forge_ret\n"
        "azm_forge_ret:\n"
        "   ret\n");
void azm_forge_mode(void);
void azm_forge_true(void);
void azm_forge_ret(void);

/* vtable: the refresh calls +0x10 and +0x18; the rest are bare rets so a stray
 * call cannot fault the UI thread. */
static void *g_forge_vt[5] = {
    (void *) azm_forge_ret,
    (void *) azm_forge_ret,
    (void *) azm_forge_true, /* +0x10 : host-link gate  */
    (void *) azm_forge_mode, /* +0x18 : PcAppli mode    */
    (void *) azm_forge_ret,
};
static void *g_forge_obj = g_forge_vt; /* the object = its vptr */

/* ---- Per-stem state on the native tiles -------------------------
 * EP147 keeps the stems tiles' {down, normal} PNG pointers in writable .data at
 * 0x3b36048, 16 bytes per pair in mode order:
 *
 *   +0x00 vocal   +0x10 inst    +0x20 drums   +0x30 mute_solo  +0x40 (blank?)
 *   +0x50 melody  +0x60 bass    +0x70 vocal_echo  +0x80 inst_echo
 *   +0x90 inst_braker  +0xa0 drums_echo
 *
 * The visible stems row is VOCAL / MELODY / BASS / DRUMS = pairs 0, 5, 6, 2.
 * azmod does not write this table for stems: it is process-wide and cannot hold
 * two decks' mute states. Per-deck art is each tile's own ImagesComponent index
 * (+0xc8), written by stem_tile_fix_one. (DRUM_TILE_TBL at 0x3b36028 is a
 * separate write with its own anchors.) */
/* Per-deck last-applied stem state: 0 unknown, 1 on, 2 muted. */
static int g_pad_applied[AZMOD_STEM_DECKS][AZMOD_STEM_COUNT];

/* ---- Pad-mode title strip: no global table swap ---------------------------
 * PAD_TITLE_TBL (0x3b35f40) is one process-wide table, so pointing it at the
 * STEMS caption would retitle every deck, stemless ones included. The caption is
 * per-grid art (grid+0x130) chosen by that grid's own mode, so the title follows
 * modeget_hook's per-deck hijack instead; see TITLE_UPDATE. */
/* Telemetry: one change-only line per deck to /tmp/azmod-stemtiles naming the
 * exit taken. */
static void stem_tiles_status(int deck, const char *why) {
    /* Per-deck `last`: with one shared buffer the decks' lines would alternate,
     * every call would write, and the log would grow until tmpfs filled and
     * swallowed every other azmod log. */
    static char last[AZMOD_STEM_DECKS][256];
    char buf[256];
    /* Compare the raw inputs first and format only on a change: this runs per
     * deck on every overlay tick (10 ms). */
    if (deck < 0 || deck >= AZMOD_STEM_DECKS)
        return;
    {
        struct {
            const char *why;
            int native;
            unsigned char on[AZMOD_STEM_COUNT], ap[AZMOD_STEM_COUNT];
            int act, bl;
            unsigned mb, pb, pr, rr;
            unsigned long t0, t1;
        } sig;
        static __typeof__(sig) lastsig[AZMOD_STEM_DECKS];
        static int have[AZMOD_STEM_DECKS];
        memset(&sig, 0, sizeof sig);
        sig.why = why;
        sig.native = azmod_stem_native;
        for (int i = 0; i < AZMOD_STEM_COUNT; i++) {
            sig.on[i] = (unsigned char) g_state.stem_on[deck][i];
            sig.ap[i] = (unsigned char) g_pad_applied[deck][i];
        }
        sig.act = __atomic_load_n(&g_erp_active[deck], __ATOMIC_RELAXED);
        sig.bl = g_pad_mode_bl[deck];
        sig.mb = g_dbg_modebyte[deck];
        sig.pb = g_dbg_padbits[deck];
        sig.pr = g_dbg_padrise[deck];
        sig.rr = g_dbg_reqraised[deck];
        sig.t0 = (unsigned long) __atomic_load_n(&g_tile_w[0][0], __ATOMIC_RELAXED);
        sig.t1 = (unsigned long) __atomic_load_n(&g_tile_w[1][0], __ATOMIC_RELAXED);
        if (have[deck] && memcmp(&sig, &lastsig[deck], sizeof sig) == 0)
            return;
        lastsig[deck] = sig;
        have[deck] = 1;
    }
    snprintf(buf, sizeof buf,
             "deck%d %s native=%d on=%d%d%d%d applied=%d%d%d%d "
             "erp_active=%d pad_bl=%d modebyte=%02x padbits=%02x padrise=%02x reqraised=%02x "
             "t0=%lx t1=%lx\n",
             deck, why, azmod_stem_native, g_state.stem_on[deck][0], g_state.stem_on[deck][1],
             g_state.stem_on[deck][2], g_state.stem_on[deck][3],
             (deck >= 0 && deck < AZMOD_STEM_DECKS) ? g_pad_applied[deck][0] : -1,
             (deck >= 0 && deck < AZMOD_STEM_DECKS) ? g_pad_applied[deck][1] : -1,
             (deck >= 0 && deck < AZMOD_STEM_DECKS) ? g_pad_applied[deck][2] : -1,
             (deck >= 0 && deck < AZMOD_STEM_DECKS) ? g_pad_applied[deck][3] : -1,
             __atomic_load_n(&g_erp_active[deck], __ATOMIC_RELAXED), g_pad_mode_bl[deck],
             g_dbg_modebyte[deck], g_dbg_padbits[deck], g_dbg_padrise[deck], g_dbg_reqraised[deck],
             (unsigned long) __atomic_load_n(&g_tile_w[0][0], __ATOMIC_RELAXED),
             (unsigned long) __atomic_load_n(&g_tile_w[1][0], __ATOMIC_RELAXED));
    if (deck < 0 || deck >= AZMOD_STEM_DECKS)
        return;
    if (strcmp(buf, last[deck]) == 0)
        return; /* only on change, per deck */
    snprintf(last[deck], sizeof last[deck], "%s", buf);
    FILE *f = azlog_open("/tmp/azmod-stemtiles", "a");
    if (f) {
        fputs(buf, f);
        fclose(f);
    }
}

/* There is no tile owner deck: no shared tile resource is left to arbitrate. */

/* Index 8 of a stems tile's own image array renders blank. Tiles 5-8 are left to
 * EP147, see stem_tile_fix_one. */
/* Each pad tile's component is a gui::ImagesComponent whose [+0xc8] is an image
 * index into a per-component list, not a mode. As the refresh writes it:
 *   beat-loop tiles = 2*slot+1, empty = 9, stems tiles = 2*slot+0xb.
 * The art is {down,normal} pairs, so the other state of a stems tile is index-1,
 * and it keeps its caption.
 * Loading a track rebuilds these widgets, so a cached pointer dangles, and
 * calling repaint through a dead one restarts EP147: validate the vtable at
 * every use. g_stem_model is cleared whenever the container is rebuilt. */
static int comp_is_valid(long c) {
    if (c <= 0x10000 || (c & 7))
        return 0;
    return *(long *) c == (long) IMAGESCOMP_VT; /* vtable check = liveness check */
}
static int drumcomp_is_valid(long c) {
    if (c <= 0x10000 || (c & 7))
        return 0;
    long vt = *(long *) c;
    return vt == (long) PADCUE_VT || vt == (long) IMAGESCOMP_VT;
}
/* Repaint every live captured slot-7 tile; returns how many took. JUCE message
 * thread only (see rtick_hook). */
static int slot7_repaint_all(void) {
    int n = 0;
    for (int i = 0; i < SLOT7_MAX; i++) {
        long w = (long) wref_get(WSLOT(&g_slot7_w[i])); /* 0 once the tile is destroyed */
        uintptr_t v, wv = 0, cv = 0;
        long comp = 0;
        const char *why = NULL;
        if (w <= 0x10000 || (w & 7))
            why = "badptr";
        else if (!own_q((uintptr_t) w, &v))
            why = "vt-unreadable";
        else if ((wv = v, (long) v != (long) PADTILE_VT))
            why = "vt-mismatch";
        else if (!own_q((uintptr_t) w + 0xf0, &v))
            why = "comp-unreadable";
        else if ((comp = (long) v, !drumcomp_is_valid(comp)))
            why = "comp-vt";
        if (!why) {
            /* Repaint the tile, then its component. PAD_REPAINT (0x2488570) is
             * juce::Component::repaint(): non-virtual (in no vtable), it builds a
             * rect from this+0x28 and calls through, so it is valid on any
             * Component. */
            ((void (*)(long)) PAD_REPAINT)(w);
            ((void (*)(long)) PAD_REPAINT)(comp);
            n++;
        }
        if (comp > 0x10000 && !(comp & 7) && own_q((uintptr_t) comp, &v))
            cv = v; /* comp is set only from a live tile's own field */
        /* Log which check rejected each entry; a bare count cannot tell the
         * four causes apart. */

        {
            static _Atomic int wlog = 24;
            if (__atomic_fetch_sub(&wlog, 1, __ATOMIC_RELAXED) > 0) {
                FILE *f = azlog_open("/tmp/azmod-drumtile.log", "a");
                if (f) {
                    fprintf(f,
                            "  slot7[%d] w=%#lx wvt=%#lx comp=%#lx cvt=%#lx want_wvt=%#lx "
                            "want_cvt=%#lx -> %s\n",
                            i, (unsigned long) w, (unsigned long) wv, (unsigned long) comp,
                            (unsigned long) cv, (unsigned long) PADTILE_VT,
                            (unsigned long) IMAGESCOMP_VT, why ? why : "REPAINTED");
                    fclose(f);
                }
            }
        }
    }
    return n;
}

/* ---- 4-DECK source cell: each deck's own native stem strip ----------
 * 4-DECK has no native pad/stem widget, so the stem readout lives in the deck's
 * SOURCE cell, which draws exactly one juce::Image:
 *   DeckInfo4DeckWidget ctor 0x1d82b08(this, ?, playerId, deck)   <- deck is an arg
 *     [this+0x1c0] = WaveformLoadedDeviceIconWidget (vt 0x2abc588)
 *       +0xd0 = juce::Image (pre-rendered "icon + USB1", 170x41), which its
 *       paint copies to +0xf0 and draws with drawImage(0,0,170,41, centered).
 * We replace that image with the deck's strip (index = stem mask); the USB name
 * is intentionally hidden. Survives a 2<->4 DECK switch and a track load; 2-DECK
 * (its own instances, ctor 0x1d7bd80) is never touched.
 *
 * Rules:
 *  - the deck comes from the ctor argument; an unknown deck is never touched.
 *  - parent and cell are vtable-checked at every use; a mismatch drops the entry.
 *  - message thread only (rtick_hook), never inside a paint; no allocation or
 *    logging per tick. The 16 images are decoded once and held for process life.
 *  - EP147 re-supplies +0xd0 around every track load (several NULLs, then a fresh
 *    original). Only a non-NULL image that is not ours may become the restore
 *    target, so stems-off mid-load never restores an empty cell.
 *  - Images go through EP147's own refcounting: Image::operator= 0x23b1440,
 *    copy-ctor 0x23b1410, ~Image 0x23a45f0 (refcount at +8). */
#define S4_CTOR EP_1d82b08 /* gui::waveform::DeckInfo4DeckWidget ctor */
#define S4_PARENT_VT EP_2cfe5c0
#define S4_CELL_OFF 0x1c0L /* -> WaveformLoadedDeviceIconWidget */
#define S4_CELL_VT EP_2abc588
#define S4_IMG_OFF 0xd0L         /* the cell's juce::Image */
#define S4_IMG_ASSIGN EP_23b1440 /* Image& Image::operator=(const Image&) */
#define S4_IMG_COPY EP_23b1410   /* Image::Image(const Image&) */
#define S4_IMG_DTOR EP_23a45f0   /* Image::~Image() */
#define S4_REPAINT EP_2488570    /* juce::Component::repaint() */
static _Atomic long g_s4_parent[AZMOD_STEM_DECKS];
static void (*g_orig_s4ctor)(long, long, long, long);
static void s4ctor_hook(long self, long a1, long a2, long deck) {
    g_orig_s4ctor(self, a1, a2, deck);
    if (deck >= 0 && deck < AZMOD_STEM_DECKS) {
        disp_wref_init();
        wref_set(WSLOT(&g_s4_parent[deck]), (uintptr_t) self); /* cleared by its destructor */
    }
}
static struct fx_img g_s4_art[16];
static int g_s4_art_ready;
static struct {
    long cell;
    long saved;
    int applied;
} g_s4[AZMOD_STEM_DECKS];
static int s4_is_ours(long v) {
    if (!v)
        return 0;
    for (int i = 0; i < 16; i++)
        if ((uintptr_t) v == fx_img_val(&g_s4_art[i]))
            return 1;
    return 0;
}
static void s4_forget(int d) { /* the cell is gone: drop our hold on its original */
    if (g_s4[d].saved)
        ((void (*)(long *)) S4_IMG_DTOR)(&g_s4[d].saved);
    g_s4[d].saved = 0;
    g_s4[d].applied = 0;
    g_s4[d].cell = 0;
}
static void stems4_tick(void) {
    if (!azmod_stem_native || !g_orig_s4ctor)
        return;
    /* Default on. The kill switch (azmod-stems4.off) exists because this code
     * calls raw EP147 addresses on widget objects, and a deck in front of an
     * audience needs a way out that needs no rebuild. */
    {
        static int off = -1;
        if (off < 0)
            off = (access("/home/root/settings/azmod-stems4.off", F_OK) == 0);
        if (off)
            return;
    }
    if (!g_s4_art_ready) {
        fx_fns_resolve(); /* resolves both fx_img functions; see fx_fns_resolve */
        for (int i = 0; i < 16; i++)
            g_s4_art[i] = fx_img_from_mem(STEMS4_ART[i].png, (int) STEMS4_ART[i].len);
        for (int i = 0; i < 16; i++)
            if (!fx_img_val(&g_s4_art[i]))
                return; /* retry next tick */
        /* These strips are our multi-hue art. Day Mode's generic ARGB-tint flatten
         * would collapse the four stem hues to one, so mark them for per-pixel
         * theming instead. */
        for (int i = 0; i < 16; i++)
            azmod_dm_mark_perpixel((void *) fx_img_val(&g_s4_art[i]));
        g_s4_art_ready = 1;
    }
    /* Identity checks (parent vtable, parent->cell, cell vtable). Message thread (rtick): a
     * non-zero g_s4_parent is a live widget (wref, ep_mem.h) and its cell is its own field,
     * so these are plain loads; the vtables prove the classes. */
    long par[AZMOD_STEM_DECKS];
    uintptr_t pa[2 * AZMOD_STEM_DECKS], pv[2 * AZMOD_STEM_DECKS];
    int pok[2 * AZMOD_STEM_DECKS], np = 0;
    for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
        par[d] = (long) wref_get(WSLOT(&g_s4_parent[d]));
        pa[2 * d] = (uintptr_t) par[d];
        pa[2 * d + 1] = (uintptr_t) par[d] + S4_CELL_OFF;
        if (par[d])
            np = 2 * d + 2;
    }
    if (!np)
        return;
    for (int d = 0; d < AZMOD_STEM_DECKS; d++)
        if (par[d] <= 0x10000 || (par[d] & 7))
            pa[2 * d] = pa[2 * d + 1] = 0; /* own_qv refuses 0 */
    own_qv(np, pa, pv, pok);
    uintptr_t ca[AZMOD_STEM_DECKS], cv[AZMOD_STEM_DECKS];
    int cok[AZMOD_STEM_DECKS];
    for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
        /* a cell is read only from a parent proven a DeckInfo4DeckWidget just now */
        int pok_vt = 2 * d < np && pok[2 * d] && pv[2 * d] == S4_PARENT_VT;
        long c = (pok_vt && 2 * d + 1 < np && pok[2 * d + 1]) ? (long) pv[2 * d + 1] : 0;
        ca[d] = (c > 0x10000 && !(c & 7)) ? (uintptr_t) c : 0;
    }
    own_qv(AZMOD_STEM_DECKS, ca, cv, cok); /* cells of live parents (0 = skipped) */
    for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
        long parent = par[d];
        if (!parent)
            continue;
        long cell = 0;
        if (!(2 * d < np && pok[2 * d] && pv[2 * d] == S4_PARENT_VT)) {
            wref_set(WSLOT(&g_s4_parent[d]), 0);
            s4_forget(d);
            continue;
        }
        if (!(2 * d + 1 < np && pok[2 * d + 1]) || !ca[d] || !cok[d] || cv[d] != S4_CELL_VT) {
            s4_forget(d);
            continue;
        }
        cell = (long) ca[d];
        if (g_s4[d].cell != cell) {
            s4_forget(d);
            g_s4[d].cell = cell;
        }
        long *slot = (long *) (cell + S4_IMG_OFF);
        int want = azmod_stems_engine(&g_state) && g_state.stems_available[d];
        if (want) {
            unsigned mask = 0;
            for (int i = 0; i < AZMOD_STEM_COUNT && i < 4; i++)
                if (g_state.stem_on[d][i])
                    mask |= 1u << i;
            long ours = (long) fx_img_val(&g_s4_art[mask]);
            if (*slot != ours) {
                if (*slot && !s4_is_ours(*slot)) { /* EP147's current original */
                    if (g_s4[d].saved)
                        ((void (*)(long *)) S4_IMG_DTOR)(&g_s4[d].saved);
                    ((void (*)(long *, long *)) S4_IMG_COPY)(&g_s4[d].saved, slot);
                }
                ((void (*)(long *, const struct fx_img *)) S4_IMG_ASSIGN)(slot, &g_s4_art[mask]);
                g_s4[d].applied = 1;
                ((void (*)(long)) S4_REPAINT)(cell);
            }
        } else if (g_s4[d].applied) {
            if (s4_is_ours(*slot)) { /* only undo what is ours */
                if (g_s4[d].saved) {
                    ((void (*)(long *, long *)) S4_IMG_ASSIGN)(slot, &g_s4[d].saved);
                } else {
                    long none = 0; /* never saw an original: draw nothing, EP147 re-supplies */
                    ((void (*)(long *, long *)) S4_IMG_ASSIGN)(slot, &none);
                }
                ((void (*)(long)) S4_REPAINT)(cell);
            }
            if (g_s4[d].saved)
                ((void (*)(long *)) S4_IMG_DTOR)(&g_s4[d].saved);
            g_s4[d].saved = 0;
            g_s4[d].applied = 0;
        }
    }
}

/* ---- Stem FX tiles with the effect on: cyan ----
 * EP147's pressed art for the four FX tiles has the blue of the DRUMS stem tile. A tile's
 * array is grown by the four sfxtile.h images (in pad order, slots 4-7), and an effect that is
 * on shows its slot's image in place of EP147's pressed art. Grown = the array's last four
 * entries are ours, so an array EP147 rebuilt is grown again. Returns the index of the image
 * for fx (0-3), or -1: the caller keeps EP147's pressed art. comp is vtable-checked by the
 * caller; UI thread. */
#include "sfxtile.h"
static struct fx_img g_sfx_art[4];
static int g_sfx_art_ok;
static int sfx_tile_index(long comp, int fx) {
    static const unsigned char *const src[4] = {SFX_TILE_VOCAL_ECHO, SFX_TILE_INST_ECHO,
                                                SFX_TILE_INST_BRAKER, SFX_TILE_DRUMS_ECHO};
    static const int len[4] = {(int) sizeof SFX_TILE_VOCAL_ECHO, (int) sizeof SFX_TILE_INST_ECHO,
                               (int) sizeof SFX_TILE_INST_BRAKER, (int) sizeof SFX_TILE_DRUMS_ECHO};
    if (fx < 0 || fx > 3)
        return -1;
    if (!g_sfx_art_ok) {
        fx_fns_resolve();
        for (int i = 0; i < 4; i++)
            if (!fx_img_val(&g_sfx_art[i]))
                g_sfx_art[i] = fx_img_from_mem(src[i], len[i]);
        for (int i = 0; i < 4; i++)
            if (!fx_img_val(&g_sfx_art[i]))
                return -1; /* decoder not ready: retry next refresh */
        g_sfx_art_ok = 1;
    }
    uintptr_t *arrp = (uintptr_t *) (comp + 0xd0);
    uint32_t *cntp = (uint32_t *) (comp + 0xe0);
    uintptr_t arr = *arrp;
    uint32_t cnt = *cntp;
    if (arr <= 0x10000 || (arr & 7) || cnt < 4 || cnt > 256)
        return -1;
    int ours = 1;
    for (int i = 0; i < 4 && ours; i++)
        ours = ((const uintptr_t *) arr)[cnt - 4 + i] == fx_img_val(&g_sfx_art[i]);
    if (!ours) {
        void *grown = malloc((cnt + 4) * 8);
        if (!grown)
            return -1;
        memcpy(grown, (void *) arr, cnt * 8);
        memset((char *) grown + cnt * 8, 0, 4 * 8);
        for (int i = 0; i < 4; i++)
            fx_img_assign((char *) grown + (cnt + i) * 8, &g_sfx_art[i]);
        *(volatile uintptr_t *) arrp = (uintptr_t) grown;
        *(volatile uint32_t *) cntp = cnt + 4;
        /* old block leaked on purpose: a paint may still read it this frame */
        cnt += 4;
    }
    return (int) cnt - 4 + fx;
}

/* Opt-in (azmod-tilefix.log): names the exit stem_tile_fix_one took, to
 * /tmp/azmod-tilefix, change-only. */
static void tilefix_log(const char *why, int slot, long ods, long tds, int owner, int on, long comp,
                        int idx, int want) {
    if (!AZ_PROBE_ON("/home/root/settings/azmod-tilefix.log"))
        return; /* opt-in */
    static char last[192];
    char buf[192];
    snprintf(buf, sizeof buf, "%s slot=%d owner=%d on=%d ods=%lx tds=%lx comp=%lx idx=%x want=%x\n",
             why, slot, owner, on, (unsigned long) ods, (unsigned long) tds, (unsigned long) comp,
             idx, want);
    if (strcmp(buf, last) == 0)
        return;
    snprintf(last, sizeof last, "%s", buf);
    FILE *f = azlog_open("/tmp/azmod-tilefix", "a");
    if (f) {
        fputs(buf, f);
        fclose(f);
    }
}
/* Correct one tile: the one whose refresh just ran. The refresh is called per
 * tile with that tile's widget: slot = [w+0xc8], deckstate = [w+0xd0],
 * component = [w+0xf0]. Do not walk the region at model+0xb8 instead; it holds
 * every pad component across modes and decks at irregular offsets.
 * Always on: this is the only stem-art path, and `w` is the live tile EP147 just
 * refreshed (comp is vtable-checked below). */
static void stem_tile_fix_one(long w) {
    if (w <= 0x10000 || (w & 7)) {
        tilefix_log("EXIT:badw", -1, 0, w, -1, -1, 0, -1, -1);
        return;
    }
    int slot = *(int *) (w + 0xc8);
    long tds = *(long *) (w + 0xd0);
    /* The tile shows its own deck's stem state, resolved from its deckstate
     * (per-deck object +0x1160 in the census). Never a first-match deck with
     * stems: that paints one deck's mutes onto another's tiles. An unresolved
     * tile is left alone. */
    int owner = deck_for_ds(tds);
    if (owner < 0) {
        tilefix_log("EXIT:noowner", slot, tds, tds, owner, -1, *(long *) (w + 0xf0), -1, -1);
        return;
    }
    long ods = tds;
    long comp = *(long *) (w + 0xf0);
    /* Tiles 5-8 (slots 4-7: VOCAL ECHO / INST ECHO / INST BRAKER / DRUMS ECHO). With the
     * mini stem waveforms on the bottom row they are left to EP147, which sets their index
     * on every refresh; the lanes are drawn over their interiors (draw_stem_lanes_native()
     * in stem_overlay.c). As Stem FX pads they show EP147's own art, and an effect that is
     * on shows its cyan pressed art (sfx_tile_index; EP147's own, normal - 1, if that fails).
     * The write follows EP147's own in the same refresh, as for slots 0-3. */
    if (slot >= AZMOD_STEM_COUNT && slot < 8) {
        int cur = comp_is_valid(comp) ? *(int *) (comp + 0xc8) : -1;
        if (az_get(AZF_STEM_FX) != 1 || cur < 1) {
            tilefix_log("EXIT:fxrow", slot, ods, tds, owner, -1, comp, cur, -1);
            return;
        }
        int fxon = (__atomic_load_n(&g_sfx_led_on[owner], __ATOMIC_RELAXED) >> (slot - AZMOD_STEM_COUNT)) & 1u;
        int normal = cur | 1; /* pairs are {down, normal}: normal is the odd index */
        int want = normal;
        if (fxon) {
            const int cyan = sfx_tile_index(comp, slot - AZMOD_STEM_COUNT);
            want = cyan >= 0 ? cyan : normal - 1;
        }
        if (cur == want) {
            tilefix_log("fxsame", slot, ods, tds, owner, fxon, comp, cur, want);
            return;
        }
        tilefix_log("FXWRITE", slot, ods, tds, owner, fxon, comp, cur, want);
        *(int *) (comp + 0xc8) = want;
        ((void (*)(long)) PAD_REPAINT)(comp);
        return;
    }
    if (slot < 0 || slot >= AZMOD_STEM_COUNT) {
        tilefix_log("EXIT:slot", slot, ods, tds, owner, -1, comp, -1, -1);
        return;
    }
    int on = g_state.stem_on[owner][slot];
    /* Active = the _down art (blue highlight), inactive = _normal (gray); the
     * caption shows in both. Pairs are {down, normal}, so down = normal-1. */
    int want = 0x0b + 2 * slot - (on ? 1 : 0);
    if (!comp_is_valid(comp)) {
        tilefix_log("EXIT:vtable", slot, ods, tds, owner, on, comp, -1, want);
        return;
    }
    int *p = (int *) (comp + 0xc8);
    if (*p == want) {
        tilefix_log("same", slot, ods, tds, owner, on, comp, *p, want);
        return;
    }
    tilefix_log("WRITE", slot, ods, tds, owner, on, comp, *p, want);
    *p = want;
    ((void (*)(long)) PAD_REPAINT)(comp);
}
/* A cached widget pointer is not proof the widget still exists: a track load
 * rebuilds the tiles, and callers call through the vtable (wave_setvis) or
 * dispatch on the object (stemref_hook). g_tile_w is a wref range (ep_mem.h): the
 * tile's destructor clears its slot, so on the message thread a non-zero slot is a
 * live widget, and this checks its class. Message thread only. */

static int tilew_alive(long tw) {
    if (tw <= 0x10000 || (tw & 7) || !on_msg_thread())
        return 0;
    uintptr_t vt;
    if (!own_q((uintptr_t) tw, &vt))
        return 0;
    return vt == PADTILE_VT || vt == IMAGESCOMP_VT || vt == PADCUE_VT;
}
/* Re-run the refresh for one deck's captured tiles; stem_tile_fix_one then
 * writes that tile's own image index from that tile's own deck state. Message
 * thread only: the refresh is EP147 UI code and the tiles are its widgets. */
static void stem_tiles_poke(int deck) {
    if (deck < 0 || deck >= AZMOD_STEM_DECKS || !g_orig_stemref || !on_msg_thread())
        return;
    /* All eight: the FX row's art follows the effects, and leaving FX mode must refresh it
     * back to EP147's own index. */
    for (int k = 0; k < 8; k++) {
        long tw = (long) wref_get(WSLOT(&g_tile_w[deck][k]));
        if (tilew_alive(tw))
            stemref_hook(tw);
        else if (tw)
            wref_set(WSLOT(&g_tile_w[deck][k]), 0);
    }
}
/* stem_tiles_apply runs on the overlay thread, which may not touch EP147's widgets: it
 * only raises a per-deck bit, and the message thread pokes (stem_tiles_poke_drain). */
static _Atomic unsigned g_tiles_poke_req; /* also raised by drum8_index_apply */
static void stem_tiles_poke_drain(void) {
    unsigned req = __atomic_exchange_n(&g_tiles_poke_req, 0, __ATOMIC_ACQ_REL);
    for (int d = 0; req && d < AZMOD_STEM_DECKS; d++)
        if (req & (1u << d))
            stem_tiles_poke(d);
}
static void stem_tiles_apply(int deck) {
    if (!azmod_stem_native) {
        stem_tiles_status(deck, "EXIT:not-native");
        return;
    }
    if (deck < 0 || deck >= AZMOD_STEM_DECKS)
        return; /* unknown deck: do nothing */
    /* Per deck by construction: repaint only this deck's captured tiles;
     * stem_tile_fix_one then writes each tile's own +0xc8 index in place. No
     * shared table write and no container rebuild. */
    int changed = 0;
    for (int i = 0; i < AZMOD_STEM_COUNT; i++) {
        int st = g_state.stem_on[deck][i] ? 1 : 2;
        if (g_pad_applied[deck][i] == st)
            continue;
        g_pad_applied[deck][i] = st;
        changed = 1;
    }
    {
        static unsigned fx_applied[AZMOD_STEM_DECKS];
        unsigned fx = __atomic_load_n(&g_sfx_led_on[deck], __ATOMIC_RELAXED) |
                      (az_get(AZF_STEM_FX) == 1 ? 0x100u : 0u);
        if (fx != fx_applied[deck]) {
            fx_applied[deck] = fx;
            changed = 1;
        }
    }
    stem_tiles_status(deck, changed ? "APPLIED" : "nochange");
    if (changed) /* the message thread pokes (stem_tiles_poke_drain, on rtick) */
        __atomic_fetch_or(&g_tiles_poke_req, 1u << deck, __ATOMIC_RELEASE);
}
static void stemref_hook(long w) {
    long ds = 0, saved = 0, saved40 = 0;
    int forged = 0;
    if (w > 0x10000) { /* remember this tile by its index */
        int idx = *(int *) (w + 0xc8);
        long tds = *(long *) (w + 0xd0);
        /* File every tile under its own tile-ds deck (a container's deckstate is
         * a different object). stem_tile_fix_one re-derives the deck from the
         * tile itself. */
        {
            if (__atomic_fetch_sub(&g_cap_budget, 1, __ATOMIC_RELAXED) > 0) {
                ds_anchor_scan(tds);
                FILE *cf = azlog_open("/tmp/azmod-tilecap.log", "a");
                if (cf) {
                    fprintf(cf, "cap: idx=%d tds=%lx grp=%d\n", idx, (unsigned long) tds,
                            deck_for_ds(tds));
                    fclose(cf);
                }
            }
        }
        int grp = deck_for_ds(tds);
        if (idx >= 0 && idx < 8 && grp >= 0 && grp < AZMOD_STEM_DECKS) {
            disp_wref_init();
            wref_set(WSLOT(&g_tile_w[grp][idx]), (uintptr_t) w); /* cleared by its destructor */
        }
    }
    unsigned char savedc1 = 0;
    /* Both flags latched and re-read every 256 calls: this runs on every paint
     * (waveswitch_hook -> wave_force), where two access() calls per call cost
     * ~2% of the paint thread with four decks. */
    static int diag_on = -1, forge_off = -1;
    static unsigned fl_n;
    if (diag_on < 0 || (++fl_n & 0xffu) == 0) {
        diag_on = AZ_PROBE_ON("/tmp/azmod-stemdiag");
        forge_off = access("/home/root/settings/azmod-stemforge.off", F_OK) == 0;
    }
    if (!forge_off) { /* default on; .off opts out */
        ds = *(long *) (w + 0xd0);
        if (ds > 0x10000) {
            saved = *(long *) (ds + 0x10);
            saved40 = *(long *) (ds + 0x40);
            savedc1 = *(unsigned char *) (ds + 0xc1);
            /* The first gate:
             *   0x1df6a0c  ldrb w0,[x20,#0xc1]
             *   0x1df6a10  cbnz w0, 0x1df6a64
             * Zero branches straight to the non-stems tile loop before either
             * forged service is consulted (it is 0 on the deck), so the byte
             * is set too: scoped to this one call and restored below. */
            g_forge_modeinfo = EP_25e9ba0; /* this build's IPcAppliSetMode::Mode info */
            *(unsigned char *) (ds + 0xc1) = 1;
            *(long *) (ds + 0x10) = (long) &g_forge_obj;
            *(long *) (ds + 0x40) = (long) &g_forge_obj;
            forged = 1;
        }
    }
    g_orig_stemref(w);
    /* Tile index probe (opt-in, azmod-tileidx.log): one change-only line per
     * (slot, index), to measure what each pad mode writes (e.g. SLIP LOOP, whose
     * 8th tile hosts DRUM ROLL) instead of guessing. Runs on the JUCE refresh
     * path. */
    if (AZ_PROBE_ON("/home/root/settings/azmod-tileidx.log")) {
        long comp = (w > 0x10000 && !(w & 7)) ? *(long *) (w + 0xf0) : 0;
        if (comp_is_valid(comp)) {
            int slot = *(int *) (w + 0xc8);
            int idx = *(int *) (comp + 0xc8);
            static int last_idx[16];
            static int primed;
            if (!primed) {
                for (int i = 0; i < 16; i++)
                    last_idx[i] = -1;
                primed = 1;
            }
            if (slot >= 0 && slot < 16 && last_idx[slot] != idx) {
                last_idx[slot] = idx;
                FILE *f = azlog_open("/tmp/azmod-tileidx.log", "a");
                if (f) {
                    fprintf(f, "slot=%d idx=%d (0x%x)\n", slot, idx, idx);
                    fclose(f);
                }
            }
        }
    }
    if (azmod_stem_native)
        stem_tile_fix_one(w); /* this tile, just refreshed */
    if (forged) {             /* always restore all three */
        *(long *) (ds + 0x10) = saved;
        *(long *) (ds + 0x40) = saved40;
        *(unsigned char *) (ds + 0xc1) = savedc1;
    }
}
/* Opt-in (azmod-stempoke.on): drive the refresh ourselves at 1 Hz on the
 * message thread, for the first deck with a stored widget. The other pokes fire
 * only on a view construct/switch, so a deck already sitting on the waveform
 * view otherwise gets none. Reentrancy-guarded; the refresh only writes the
 * model when the computed mode changes, so re-running it is cheap. */
static void stem_selfpoke(void) {
    static long last_ms;
    static int busy;
    long w = 0;
    for (int d = 0; d < AZMOD_STEM_DECKS && !w; d++)
        w = __atomic_load_n(&g_stem_widget[d], __ATOMIC_RELAXED);
    if (!w || busy || !g_orig_stemref)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (ms - last_ms < 1000)
        return;
    last_ms = ms;
    if (!AZ_PROBE_ON("/home/root/settings/azmod-stempoke.on"))
        return;
    busy = 1;
    stemref_hook(w);
    busy = 0;
}
static void stems_avail_redispatch(void) {
    /* An eject or a no-stem track must take the grid back. The gates in
     * waveswitch_hook/modeget_hook are consulted only when EP147 dispatches a
     * pad mode, and nothing dispatches when a track leaves. azstem drops
     * azstem.avail<N> on eject and azmod mirrors it into stems_available[], so
     * on each edge re-dispatch only that deck's own container. Message thread,
     * outside any paint traversal. */
    stem_tiles_poke_drain(); /* the stem tiles' repaint requested by the overlay thread */
    {
        static int prev_avail[AZMOD_STEM_DECKS];
        static int primed = 0;
        for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
            /* Also keyed on the STEMS master (arm_stems re-dispatches nothing)
             * and on the beat-loop page flip (no EP147 mode change, so no
             * dispatch): both are the same edge as an eject for this deck's grid
             * and caption. */
            int av = (__atomic_load_n(&g_wave_forcestems, __ATOMIC_ACQUIRE) &&
                      g_state.stems_available[d] && !bl_on_native_vpage(d))
                         ? 1
                         : 0;
            if (primed && av != prev_avail[d]) {
                /* The container is a wref slot: its destructor clears it, so a
                 * non-zero value here (message thread) is alive; the vtable proves
                 * its class before anything calls into it. */
                long c = (long) wref_get(WSLOT(&g_stem_container[d]));
                uintptr_t cvt = 0;
                if (c > 0x10000 && !(c & 7) && own_q((uintptr_t) c, &cvt) && cvt == PADTILE_VT &&
                    g_orig_waveswitch) {
                    /* the gates re-evaluate inside waveswitch_hook; with avail now
                     * 0 it falls through to the native dispatcher */
                    waveswitch_hook(c);
                }
                /* ...and that deck's title caption, which only re-reads its mode
                 * when EP147 re-runs TITLE_UPDATE on it. */
                {
                    long g = __atomic_load_n(&g_title_grid[d], __ATOMIC_RELAXED);
                    if (g_orig_titleupd && title_grid_alive(g))
                        g_orig_titleupd(g);
                    else if (g)
                        __atomic_store_n(&g_title_grid[d], 0, __ATOMIC_RELAXED);
                }
                if (c > 0x10000 && !(c & 7) && g_orig_waveswitch) {
                    FILE *f = azlog_open("/tmp/azmod-rebuild.log", "a");
                    if (f) {
                        fprintf(f, "AVAIL d%d %d->%d re-dispatched container=%#lx\n", d,
                                prev_avail[d], av, (unsigned long) c);
                        fclose(f);
                    }
                }
            } else if (prev_avail[d] != av) {
                /* first pass only (not yet primed): drop the stored container */
                wref_set(WSLOT(&g_stem_container[d]), 0);
            }
            prev_avail[d] = av;
        }
        primed = 1;
    }
}

/* ---- STEMS master switch ----
 * The STEMS mod toggle (az_on(AZF_STEMS)) is the master. On its edge:
 *   1. g_wave_forcestems lets the pad-grid hooks replace each stems deck's Beat
 *      Loop grid (native PadStemWidget, or blanked for the overlay);
 *   2. pad routing follows via g_erp_active (set in the overlay loop from the
 *      same flag), so pads become stem mute/solo.
 * azstem's audio enable is mirrored separately by write_enable().
 * The jog LCDs are left native: the "stemson" command to the aztx_deck shim
 * (which would force both jogs into the stems submode) is never sent.
 * No EP147 code is patched here. The AlphaThetaCert host-link gate in the tile
 * refresh is passed only by stemref_hook's scoped forge, in native mode. */
static void arm_stems(int on) {
    __atomic_store_n(&g_wave_forcestems, on ? 1 : 0, __ATOMIC_RELEASE);

    dbg("arm_stems(%d): wave_force=%d (jog LCDs untouched)\n", on, on);
}

