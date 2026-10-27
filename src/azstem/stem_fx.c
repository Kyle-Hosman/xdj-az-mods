/* SPDX-License-Identifier: GPL-3.0-only */
/* stem_fx.c: Stem FX on pads 5-8 (VOCAL ECHO, INST ECHO, INST BRAKE, DRUMS ECHO), per deck.
 * #included by azstem.c before key_shift.c, whose PageBuffer read hook renders them
 * (stem_fx.h has the sound and the lock-free press state).
 *
 * azmod's panel-bus thread calls azstem_stem_fx on a pad press and release. The effect lengths
 * are beats of the deck's own grid (EP147's BeatGrid, read per deck by drumroll.c), so they
 * are refused, never guessed, while the grid is unknown. azmod's overlay thread sets the mode
 * and lengths with azstem_stem_fx_config. */
#include "stem_fx.h"

#define SFX_N 4 /* effects per deck, in pad order 5-8 */
static const int SFX_STEM[SFX_N] = {0, 1, 1, 3}; /* vocals, inst, inst, drums */
static const int SFX_KIND[SFX_N] = {SFX_ECHO, SFX_ECHO, SFX_BRAKE, SFX_ECHO};
static sfx_slot g_sfx[NDECK][SFX_N];
/* Effect lengths in quarter beats; written by the overlay thread, read at a press. */
static _Atomic int g_sfx_echo_q = 2;  /* 1/2 beat */
static _Atomic int g_sfx_brake_q = 8; /* 2 beats */
static _Atomic int g_sfx_mode = 0;    /* the pads are FX pads: the grid service runs for them */
/* Bit f set = slot f of this deck was pressed and may still sound; cleared by the flusher
 * once the playhead is past its fade-out. Read by the read hook to choose the stem-sum path
 * (the all-on original cannot carry an effect). */
static _Atomic unsigned g_sfx_live[NDECK];

static _Atomic long long g_play_obs[NDECK]; /* key_shift.c: the observed playhead */

/* This deck's beat length in source samples from its grid, 0 = unknown. */
static double sfx_beat(int deck) {
    if (deck < 0 || deck >= DRUM_SIDES)
        return 0.0;
    drum_grid *g = &g_drum_grid[deck];
    for (int t = 0; t < SFX_TRIES; t++) {
        unsigned s1 = atomic_load_explicit(&g->seq, memory_order_acquire);
        if (s1 & 1u)
            continue;
        double beat = atomic_load_explicit(&g->beat_src, memory_order_relaxed);
        int valid = atomic_load_explicit(&g->valid, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&g->seq, memory_order_relaxed) != s1)
            continue;
        return (valid && beat >= 1.0) ? beat : 0.0;
    }
    return 0.0;
}

/* Mode and lengths (quarter beats) from the Mod Menu. */
void azstem_stem_fx_config(int fx_mode, int echo_q, int brake_q);
void azstem_stem_fx_config(int fx_mode, int echo_q, int brake_q) {
    if (echo_q >= 1 && echo_q <= 16)
        atomic_store_explicit(&g_sfx_echo_q, echo_q, memory_order_relaxed);
    if (brake_q >= 1 && brake_q <= 64)
        atomic_store_explicit(&g_sfx_brake_q, brake_q, memory_order_relaxed);
    int v = fx_mode ? 1 : 0;
    if (atomic_exchange_explicit(&g_sfx_mode, v, memory_order_relaxed) != v) {
        logln("stemfx: pads 5-8 %s", v ? "FX" : "solo");
        if (!v) /* back to solo pads: every effect fades out where it is */
            for (int d = 0; d < NDECK; d++) {
                long long S = atomic_load_explicit(&g_play_obs[d], memory_order_relaxed);
                long off = atomic_load_explicit(&g_dk[d].stem_off, memory_order_relaxed);
                for (int f = 0; f < SFX_N; f++)
                    sfx_release(&g_sfx[d][f], S >= 0 ? S - off : 0);
            }
    }
}

/* Which effects of this deck are on (bit f = pad 5+f): pressed on the loaded track and not
 * released. 0 for an unknown deck. Any thread. */
unsigned azstem_stem_fx_on(int deck);
unsigned azstem_stem_fx_on(int deck) {
    if (deck < 0 || deck >= NDECK)
        return 0;
    unsigned live = atomic_load_explicit(&g_sfx_live[deck], memory_order_relaxed), on = 0;
    unsigned tg = atomic_load_explicit(&g_deck_trackgen[deck], memory_order_relaxed);
    for (int f = 0; f < SFX_N; f++) {
        sfx_snap o;
        if ((live & (1u << f)) && sfx_read(&g_sfx[deck][f], &o, tg) && o.offq < 0)
            on |= 1u << f;
    }
    return on;
}

/* Press (on = 1) or release (on = 0) effect fx (0-3, pads 5-8) of deck (0-3). A press returns 1
 * when the effect is engaged, 0 when it cannot be (unknown deck, no stems or no such stem, no
 * playhead, no beat grid): the caller then shows it off. A release always returns 0. Any
 * thread; never blocks. */
int azstem_stem_fx(int deck, int fx, int on);
int azstem_stem_fx(int deck, int fx, int on) {
    if (deck < 0 || deck >= NDECK || fx < 0 || fx >= SFX_N)
        return 0;
    deck_stems *dk = &g_dk[deck];
    sfx_slot *s = &g_sfx[deck][fx];
    long long S = atomic_load_explicit(&g_play_obs[deck], memory_order_relaxed);
    long off = atomic_load_explicit(&dk->stem_off, memory_order_relaxed);
    if (!on) {
        if (S >= 0)
            sfx_release(s, S - off);
        else
            sfx_release(s, 0); /* no playhead: end it from where it started */
        return 0;
    }
    if (!atomic_load_explicit(&g_sfx_mode, memory_order_relaxed) || S < 0 ||
        dk->stem_frames <= 0 || !dk->stem[SFX_STEM[fx]] ||
        !(atomic_load_explicit(&dk->ctrl, memory_order_relaxed) & 0x80u))
        return 0;
    double beat = sfx_beat(deck);
    if (beat <= 0.0)
        return 0;
    int q = SFX_KIND[fx] == SFX_BRAKE ? atomic_load_explicit(&g_sfx_brake_q, memory_order_relaxed)
                                      : atomic_load_explicit(&g_sfx_echo_q, memory_order_relaxed);
    long long len = (long long) (beat * (double) q / 4.0 + 0.5);
    unsigned tg = atomic_load_explicit(&g_deck_trackgen[deck], memory_order_relaxed);
    if (!sfx_press(s, SFX_KIND[fx], S - off, len, tg))
        return 0;
    atomic_fetch_or_explicit(&g_sfx_live[deck], 1u << fx, memory_order_relaxed);
    return 1;
}

/* RT, once per injected page of deck `deck` covering stem frames [b0, b1): a snapshot per stem
 * of the press that shapes it (latched for this page), and whether any does. A stem with two
 * effects (inst: echo and brake) takes the brake while it sounds. */
typedef struct {
    sfx_snap s[NSTEM];
    unsigned mask; /* stems with an effect in this page */
} sfx_page;
static inline unsigned sfx_page_prepare(int deck, long long b0, long long b1, sfx_page *p) {
    p->mask = 0;
    unsigned live = atomic_load_explicit(&g_sfx_live[deck], memory_order_relaxed);
    if (!live)
        return 0;
    unsigned tg = atomic_load_explicit(&g_deck_trackgen[deck], memory_order_relaxed);
    for (int f = 0; f < SFX_N; f++) {
        if (!(live & (1u << f)))
            continue;
        sfx_snap o;
        if (!sfx_read(&g_sfx[deck][f], &o, tg))
            continue;
        sfx_latch(&g_sfx[deck][f], &o, b0, b1);
        if (o.start < 0 || o.start >= b1 || sfx_done_at(&o, b0))
            continue; /* dry throughout this page */
        int k = SFX_STEM[f];
        if ((p->mask & (1u << k)) && p->s[k].kind == SFX_BRAKE)
            continue; /* the brake already holds this stem */
        p->s[k] = o;
        p->mask |= 1u << k;
    }
    return p->mask;
}
/* Stem k's sample at frame i of the page (dry when it has no effect). */
static inline void sfx_stem_px(const deck_stems *dk, const sfx_page *p, int k, long nf,
                               long long i, float *L, float *R) {
    const float *g = dk->stem[k];
    if (p->mask & (1u << k))
        sfx_sample(g, nf, &p->s[k], i, L, R);
    else {
        *L = g[i * 2];
        *R = g[i * 2 + 1];
    }
}

/* Flusher: retire effects whose fade-out the playhead has passed, so the all-on deck goes back
 * to the original; and every effect of a deck whose track changed. */
static void sfx_service(void) {
    static long long told[NDECK][SFX_N][3]; /* last logged tag, start, off */
    for (int d = 0; d < NDECK; d++) {
        unsigned live = atomic_load_explicit(&g_sfx_live[d], memory_order_relaxed);
        if (!live)
            continue;
        unsigned tg = atomic_load_explicit(&g_deck_trackgen[d], memory_order_relaxed);
        long long S = atomic_load_explicit(&g_play_obs[d], memory_order_relaxed);
        long off = atomic_load_explicit(&g_dk[d].stem_off, memory_order_relaxed);
        for (int f = 0; f < SFX_N; f++) {
            if (!(live & (1u << f)))
                continue;
            sfx_snap o;
            int r = sfx_read(&g_sfx[d][f], &o, tg);
            /* One line per press, latch and release: where the effect began and ended against
             * the press, so the read-ahead it waited for is on record. */
            if (r && (told[d][f][0] != o.tag || told[d][f][1] != o.start ||
                      told[d][f][2] != o.off)) {
                told[d][f][0] = o.tag;
                told[d][f][1] = o.start;
                told[d][f][2] = o.off;
                logln("stemfx: d%d fx%d #%u on=%lld len=%lld start=%lld off=%lld (release at %lld)",
                      d, f, o.tag, o.on, o.len, o.start, o.off, o.offq);
            }
            /* Retire when the press is gone (another track) or it has faded out a second
             * behind the playhead: read-ahead pages are well inside a second. A slot being
             * written right now reads as gone; it is retried on the next tick. */
            int busy = atomic_load_explicit(&g_sfx[d][f].seq, memory_order_relaxed) & 1u;
            if (busy)
                continue;
            if (!r || (o.off >= 0 && S >= 0 && sfx_done_at(&o, S - off - AZ_EP147_RATE))) {
                atomic_fetch_and_explicit(&g_sfx_live[d], ~(1u << f), memory_order_relaxed);
                logln("stemfx: d%d fx%d retired", d, f);
            }
        }
    }
}
