/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file sdk_deck.c
 * @brief Community mod services: deck_info, deck_grid, on_deck_event
 *
 * Nothing here reads EP147's memory. The track, playhead, stems state and beat grid come from
 * azstem's published per-deck state (../common/azdeckinfo.h), the tempo from the BPM feed
 * community_tick keeps (g_cm_bpm), and the pad mode from pad_mode_fresh(). deck_info and
 * deck_grid are a few atomic loads and one sequence-locked copy: any thread, never blocking.
 * "Playing" and the deck events are worked out on the core's UI thread (sdk_deck_tick, from
 * community_tick) and published from there.
 *
 * Deck numbers are logical decks 0..3. A deck that cannot be told is unknown (-1), never
 * guessed. #included by azmod.c after community.c.
 */
#include "../common/azdeckinfo.h"
#include "sdk_logic.h"

static _Atomic(azstem_deck_snapshot_fn) g_sd_snap;
static _Atomic(azstem_deck_beat_phase_fn) g_sd_phase;
static _Atomic int g_sd_playing[AZMOD_STEM_DECKS] = {-1, -1, -1, -1};

/* UI thread: look azstem's two functions up once it is loaded (dlsym takes a lock). */
static void sd_bind(void) {
    if (!atomic_load_explicit(&g_sd_snap, memory_order_acquire)) {
        azstem_deck_snapshot_fn f;
        *(void **) &f = dlsym(RTLD_DEFAULT, "azstem_deck_snapshot");
        if (f)
            atomic_store_explicit(&g_sd_snap, f, memory_order_release);
    }
    if (!atomic_load_explicit(&g_sd_phase, memory_order_acquire)) {
        azstem_deck_beat_phase_fn f;
        *(void **) &f = dlsym(RTLD_DEFAULT, "azstem_deck_beat_phase");
        if (f)
            atomic_store_explicit(&g_sd_phase, f, memory_order_release);
    }
}

static int sd_snapshot(int deck, struct azstem_deck_snap *s) {
    azstem_deck_snapshot_fn f = atomic_load_explicit(&g_sd_snap, memory_order_acquire);
    s->size = (int) sizeof *s;
    return f ? f(deck, s) : -1;
}

/* az_core_v1.deck_info */
static int sd_deck_info(int deck, az_deck_info *out) {
    if (!out || deck < 0 || deck >= AZMOD_STEM_DECKS)
        return -1;
    int size = out->size;
    if (size < (int) (offsetof(az_deck_info, loaded) + sizeof out->loaded))
        return -1;
    az_deck_info v;
    memset(&v, 0, sizeof v);
    v.loaded = v.playing = v.pad_mode = v.has_stems = v.stems_on = -1;
    v.pos_s = v.len_s = -1.0;
    struct azstem_deck_snap s;
    if (sd_snapshot(deck, &s) == 0) {
        v.loaded = s.loaded;
        v.content_id = s.content_id;
        v.pos_s = s.pos_s;
        v.has_stems = s.has_stems;
        v.stems_on = s.stems_on;
        snprintf(v.title, sizeof v.title, "%s", s.title);
        snprintf(v.artist, sizeof v.artist, "%s", s.artist);
    }
    v.playing = atomic_load_explicit(&g_sd_playing[deck], memory_order_relaxed);
    v.pad_mode = sd_pad_mode(pad_mode_fresh(deck % AZMOD_PAD_DECKS, deck), dg_slip_enum);
    v.bpm = g_cm_bpm[deck];
    /* only what the caller's struct has room for; its size stays as the caller set it */
    size_t n = (size_t) size < sizeof v ? (size_t) size : sizeof v;
    memcpy((char *) out + sizeof v.size, (char *) &v + sizeof v.size, n - sizeof v.size);
    return 0;
}

/* az_core_v1.deck_grid. The bar position is not in EP147's grid cells (a position and a
 * tempo per beat, no beat number), so it is reported unknown (-1) rather than assumed 4/4
 * from the first beat. */
static int sd_deck_grid(int deck, double *beat_phase, double *bar_phase) {
    if (bar_phase)
        *bar_phase = -1.0;
    if (deck < 0 || deck >= AZMOD_STEM_DECKS)
        return -1;
    azstem_deck_beat_phase_fn f = atomic_load_explicit(&g_sd_phase, memory_order_acquire);
    double ph = -1.0;
    if (!f || f(deck, &ph) != 0)
        return -1;
    if (beat_phase)
        *beat_phase = ph;
    return 0;
}

/* ---- on_deck_event ---- */
#define SD_LISTEN_PER_MOD 4
#define SD_LISTEN_MAX 32
static struct {
    int owner; /* index into g_cm */
    void (*fn)(int deck, int what, void *user);
    void *user;
} g_sd_listen[SD_LISTEN_MAX];
static int g_sd_nlisten;

/* A mod turned off live: its listeners are dropped (init() adds them again). UI thread. */
static void sdk_deck_forget(int owner) {
    int k = 0;
    for (int i = 0; i < g_sd_nlisten; i++)
        if (g_sd_listen[i].owner != owner)
            g_sd_listen[k++] = g_sd_listen[i];
    g_sd_nlisten = k;
}

/* az_core_v1.on_deck_event: from init() only. */
static int sd_on_deck_event(const char *id, void (*fn)(int deck, int what, void *user),
                            void *user) {
    cm_mod *c = cm_self(id, CM_RA(), "on_deck_event");
    if (!c || !fn)
        return -1;
    int owner = (int) (c - g_cm);
    if (owner != g_cm_loading) {
        cm_logf(c, 1, "on_deck_event refused: call it from init()");
        return -1;
    }
    if (!c->hi || (uintptr_t) fn < c->lo || (uintptr_t) fn >= c->hi) {
        cm_logf(c, 1, "on_deck_event refused: the callback is not in this mod's code");
        return -1;
    }
    int mine = 0;
    for (int i = 0; i < g_sd_nlisten; i++)
        mine += g_sd_listen[i].owner == owner;
    if (mine >= SD_LISTEN_PER_MOD || g_sd_nlisten >= SD_LISTEN_MAX) {
        cm_logf(c, 1, "on_deck_event refused: at most %d per mod", SD_LISTEN_PER_MOD);
        return -1;
    }
    g_sd_listen[g_sd_nlisten].owner = owner;
    g_sd_listen[g_sd_nlisten].fn = fn;
    g_sd_listen[g_sd_nlisten].user = user;
    g_sd_nlisten++;
    return 0;
}

/* UI thread, from community_tick (~30 Hz). */
static void sdk_deck_tick(long now) {
    static struct sd_play play[AZMOD_STEM_DECKS];
    static struct sd_prev prev[AZMOD_STEM_DECKS] = {SD_PREV_INIT, SD_PREV_INIT, SD_PREV_INIT,
                                                    SD_PREV_INIT};
    sd_bind();
    for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
        struct azstem_deck_snap s;
        int loaded = -1;
        long long pos = -1;
        unsigned cid = 0;
        if (sd_snapshot(d, &s) == 0) {
            loaded = s.loaded;
            pos = s.playhead;
            cid = s.content_id;
        }
        int playing = sd_play_step(&play[d], loaded, pos, now);
        atomic_store_explicit(&g_sd_playing[d], playing, memory_order_relaxed);
        int ev[4];
        int n = sd_events(&prev[d], loaded, cid, playing, ev);
        for (int k = 0; k < n; k++)
            for (int i = 0; i < g_sd_nlisten; i++) {
                cm_mod *c = &g_cm[g_sd_listen[i].owner];
                if (cm_running(c))
                    g_sd_listen[i].fn(d, ev[k], g_sd_listen[i].user);
            }
    }
}
