/* SPDX-License-Identifier: GPL-3.0-only */
/* stem_fx.h: Stem FX, an echo out or a brake on one decoded stem.
 *
 * Every output sample is a function of the stem in RAM, the source index being read and a few
 * positions fixed at the press, never of earlier reads. EP147 reads source pages ahead of the
 * playhead, out of order and more than once (pre-render and playback), so a filter with a delay
 * line would hear its input in the wrong order; here a page read twice comes out the same both
 * times. The injection is pre-DSP: lengths are source samples, and EP147's own tempo, key lock
 * and scratch then apply to the effect as they apply to the stems.
 *
 *   echo   from the cut on, only audio from BEFORE the cut repeats: out(i) = sum over m >= 1
 *          of FB^m * stem[i - m*D], for the taps where i - m*D lies before the cut index, until
 *          FB^m falls below -60 dB. The dry stem fades out over SFX_XF frames. An echo of the
 *          phrase (SFX_ECHO_PHRASE) takes only the taps in the one delay before the cut.
 *   brake  the read position slows to a stop over T samples: p(u) = s + u - u^2 / (2T) for
 *          u = i - s < T (linear interpolation between samples), then silence. The last quarter
 *          fades to zero, since a held sample is DC.
 *   off    the dry stem fades back in over SFX_XF frames at the live position while the effect
 *          fades out.
 *
 * Positions (stem frames, i.e. source index minus the measured stem offset):
 *   on     the echo's cut point (sfx_echo_cut): the playhead at the press, or one echo later
 *          for an echo of the phrase that starts at the press. The brake starts at the press.
 *   start  the first index the injection rendered at or after `on` (latched by the reader that
 *          rendered it). Pages already rendered before the press stay dry, so the fades start
 *          here and not at `on`, which may lie in a page that has already played.
 *   offq   the playhead at the release, -1 while held on.
 *   off    the first index rendered at or after `offq` (latched like `start`).
 * A press belongs to the track loaded when it was made: after a track change it is ignored.
 *
 * Writers (the panel-bus thread, through azstem_stem_fx) publish a press through a sequence
 * counter they take by compare-and-swap; readers (the audio threads) take a consistent copy or, after a bounded number
 * of tries, play the stem dry for that page. `start` and `off` are latched by readers with a
 * compare-and-swap on a word that carries the press's generation, so a reader still holding an
 * older press cannot latch into a newer one.
 *
 * Header-only, no azstem dependencies, so the unit tests compile it as it ships. */
#ifndef AZSTEM_STEM_FX_H
#define AZSTEM_STEM_FX_H
#include <stdatomic.h>
#include <stdint.h>

#define SFX_XF 1024     /* dry <-> effect fade, frames (~23 ms at 44.1 kHz) */
#define SFX_FB 0.55f    /* echo feedback per repeat (about -5.2 dB) */
#define SFX_TAPS 12     /* SFX_FB^12 = -62 dB: the tail ends there */
#define SFX_TRIES 64    /* attempts at a consistent copy (reader) or at the slot (writer) */
#define SFX_UNSET 0xffffffffffffull /* a 48-bit position not latched yet */

enum {
    SFX_ECHO = 1,
    SFX_BRAKE = 2,
    SFX_ECHO_PHRASE = 3 /* an echo of the one delay before the cut only */
};

/* One effect slot of one deck. */
typedef struct {
    _Atomic unsigned seq;       /* even = stable, odd = the writer is mid-update */
    _Atomic unsigned gen;       /* press count: bumped by every press, low 16 bits tag latches */
    _Atomic int kind;           /* 0 = none, SFX_ECHO, SFX_BRAKE, SFX_ECHO_PHRASE */
    _Atomic long long on;       /* playhead at the press */
    _Atomic long long offq;     /* playhead at the release, -1 = still on */
    _Atomic long long len;      /* echo delay or brake length, frames (>= 1) */
    _Atomic uint64_t start_pk;  /* (index << 16) | gen tag; index SFX_UNSET = not latched */
    _Atomic uint64_t off_pk;    /* the same for the release */
    _Atomic unsigned tg;        /* the deck's track generation at the press */
} sfx_slot;

/* What a reader works from: one consistent press. */
typedef struct {
    int kind;
    unsigned tag, tg;
    long long on, offq, len;
    long long start, off; /* -1 = not latched */
} sfx_snap;

static inline uint64_t sfx_pack(long long idx, unsigned tag) {
    return ((uint64_t) idx << 16) | (tag & 0xffffu);
}
static inline long long sfx_unpack(uint64_t pk, unsigned tag) {
    if ((pk & 0xffffu) != (tag & 0xffffu))
        return -1; /* another press's latch */
    uint64_t i = pk >> 16;
    return i == SFX_UNSET ? -1 : (long long) i;
}

/* ---- writer (one thread per slot) ---- */
/* Writers take the slot by moving seq from even to odd, so two writers never interleave; one
 * that cannot take it within SFX_TRIES gives up (0) and the press does nothing. */
static inline int sfx_begin(sfx_slot *s, unsigned *q) {
    for (int t = 0; t < SFX_TRIES; t++) {
        unsigned v = atomic_load_explicit(&s->seq, memory_order_relaxed);
        if (!(v & 1u) && atomic_compare_exchange_weak_explicit(&s->seq, &v, v + 1,
                                                               memory_order_relaxed,
                                                               memory_order_relaxed)) {
            atomic_thread_fence(memory_order_release);
            *q = v;
            return 1;
        }
    }
    return 0;
}
static inline void sfx_end(sfx_slot *s, unsigned q) {
    atomic_store_explicit(&s->seq, q + 2, memory_order_release); /* -> even, after every field */
}
/* The echo's cut for a press at playhead `at` with delay `len`: at the press, so the audio before
 * it repeats, or `len` later for an echo of the phrase that starts at the press (SFX_ECHO_PHRASE:
 * it plays once dry, then only it repeats). */
static inline long long sfx_echo_cut(long long at, long long len, int from_press) {
    return from_press ? at + len : at;
}
/* A press at playhead `on` on track generation `tg`. 1 = published. */
static inline int sfx_press(sfx_slot *s, int kind, long long on, long long len, unsigned tg) {
    unsigned q;
    if (!sfx_begin(s, &q))
        return 0;
    unsigned g = atomic_load_explicit(&s->gen, memory_order_relaxed) + 1;
    atomic_store_explicit(&s->gen, g, memory_order_relaxed);
    atomic_store_explicit(&s->kind, kind, memory_order_relaxed);
    atomic_store_explicit(&s->on, on, memory_order_relaxed);
    atomic_store_explicit(&s->offq, -1, memory_order_relaxed);
    atomic_store_explicit(&s->len, len < 1 ? 1 : len, memory_order_relaxed);
    atomic_store_explicit(&s->start_pk, sfx_pack((long long) SFX_UNSET, g), memory_order_relaxed);
    atomic_store_explicit(&s->off_pk, sfx_pack((long long) SFX_UNSET, g), memory_order_relaxed);
    atomic_store_explicit(&s->tg, tg, memory_order_relaxed);
    sfx_end(s, q);
    return 1;
}
/* Release at playhead `at`. A slot never pressed, or already released, is left alone. */
static inline void sfx_release(sfx_slot *s, long long at) {
    if (!atomic_load_explicit(&s->kind, memory_order_relaxed) ||
        atomic_load_explicit(&s->offq, memory_order_relaxed) >= 0)
        return;
    if (at < 0)
        at = 0;
    unsigned q;
    if (!sfx_begin(s, &q))
        return;
    atomic_store_explicit(&s->offq, at, memory_order_relaxed);
    sfx_end(s, q);
}

/* ---- reader (any audio thread) ---- */
/* 1 = *o holds one consistent press with an effect on track generation `tg`; 0 = none, another
 * track's press, or no consistent copy (the page plays dry). */
static inline int sfx_read(sfx_slot *s, sfx_snap *o, unsigned tg) {
    for (int t = 0; t < SFX_TRIES; t++) {
        unsigned a = atomic_load_explicit(&s->seq, memory_order_acquire);
        if (a & 1u)
            continue;
        o->kind = atomic_load_explicit(&s->kind, memory_order_relaxed);
        o->tag = atomic_load_explicit(&s->gen, memory_order_relaxed);
        o->on = atomic_load_explicit(&s->on, memory_order_relaxed);
        o->offq = atomic_load_explicit(&s->offq, memory_order_relaxed);
        o->len = atomic_load_explicit(&s->len, memory_order_relaxed);
        o->tg = atomic_load_explicit(&s->tg, memory_order_relaxed);
        uint64_t sp = atomic_load_explicit(&s->start_pk, memory_order_relaxed);
        uint64_t op = atomic_load_explicit(&s->off_pk, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&s->seq, memory_order_relaxed) != a)
            continue;
        if (!o->kind || o->len < 1 || o->tg != tg)
            return 0;
        o->start = sfx_unpack(sp, o->tag);
        o->off = sfx_unpack(op, o->tag);
        return 1;
    }
    return 0;
}
/* Latch `start` / `off` for a page that covers stem frames [b0, b1). Called before rendering
 * the page; the snapshot is updated with whatever value won. A page entirely before the press
 * (or release) latches nothing. */
static inline void sfx_latch(sfx_slot *s, sfx_snap *o, long long b0, long long b1) {
    if (o->start < 0 && o->on < b1) {
        long long c = b0 > o->on ? b0 : o->on;
        uint64_t exp = sfx_pack((long long) SFX_UNSET, o->tag);
        if (atomic_compare_exchange_strong_explicit(&s->start_pk, &exp, sfx_pack(c, o->tag),
                                                    memory_order_relaxed, memory_order_relaxed))
            o->start = c;
        else
            o->start = sfx_unpack(exp, o->tag);
    }
    if (o->start >= 0 && o->offq >= 0 && o->off < 0 && o->offq < b1) {
        long long c = b0 > o->offq ? b0 : o->offq;
        if (c < o->start)
            c = o->start; /* released before anything rendered wet: fade from the start */
        uint64_t exp = sfx_pack((long long) SFX_UNSET, o->tag);
        if (atomic_compare_exchange_strong_explicit(&s->off_pk, &exp, sfx_pack(c, o->tag),
                                                    memory_order_relaxed, memory_order_relaxed))
            o->off = c;
        else
            o->off = sfx_unpack(exp, o->tag);
    }
}
/* Is the effect over at frame i: released and faded out? Also used to decide whether a deck
 * still needs the stem-sum path. */
static inline int sfx_done_at(const sfx_snap *o, long long i) {
    return o->off >= 0 && i >= o->off + SFX_XF;
}

/* ---- the sound ---- */
static inline void sfx_px(const float *g, long nf, long long i, float *L, float *R) {
    if (i < 0 || i >= nf) {
        *L = *R = 0.f;
        return;
    }
    *L = g[i * 2];
    *R = g[i * 2 + 1];
}
static inline void sfx_echo_wet(const float *g, long nf, const sfx_snap *o, long long i, float *L,
                                float *R) {
    float l = 0.f, r = 0.f;
    long long D = o->len;
    long long lo = o->kind == SFX_ECHO_PHRASE ? o->on - D : 0; /* the first index that repeats */
    long long m = (i - o->on) / D + 1; /* the first tap that lands before the press */
    if (i < o->on)
        m = 1;
    if (m > SFX_TAPS) {
        *L = *R = 0.f;
        return;
    }
    float w = 1.f;
    for (long long k = 0; k < m; k++)
        w *= SFX_FB;
    for (; m <= SFX_TAPS; m++, w *= SFX_FB) {
        long long j = i - m * D;
        if (j < lo)
            break;
        if (j < nf) {
            l += w * g[j * 2];
            r += w * g[j * 2 + 1];
        }
    }
    *L = l;
    *R = r;
}
static inline void sfx_brake_wet(const float *g, long nf, const sfx_snap *o, long long i, float *L,
                                 float *R) {
    long long u = i - o->start;
    double T = (double) o->len;
    if (u < 0) {
        sfx_px(g, nf, i, L, R);
        return;
    }
    if ((double) u >= T) {
        *L = *R = 0.f;
        return;
    }
    double p = (double) o->start + (double) u - (double) u * (double) u / (2.0 * T);
    long long a = (long long) p;
    float fr = (float) (p - (double) a);
    float aL, aR, bL, bR;
    sfx_px(g, nf, a, &aL, &aR);
    sfx_px(g, nf, a + 1, &bL, &bR);
    float gain = 1.f;
    double q = (double) u / T;
    if (q > 0.75)
        gain = (float) ((1.0 - q) * 4.0);
    *L = gain * (aL + fr * (bL - aL));
    *R = gain * (aR + fr * (bR - aR));
}
/* The stem's sample at frame i under this press: dry before `start`, effect after, dry again
 * after `off`, with the fades between. */
static inline void sfx_sample(const float *g, long nf, const sfx_snap *o, long long i, float *L,
                              float *R) {
    float dL, dR;
    sfx_px(g, nf, i, &dL, &dR);
    if (o->start < 0 || i < o->start || sfx_done_at(o, i)) {
        *L = dL;
        *R = dR;
        return;
    }
    float wL, wR;
    if (o->kind == SFX_BRAKE)
        sfx_brake_wet(g, nf, o, i, &wL, &wR);
    else
        sfx_echo_wet(g, nf, o, i, &wL, &wR);
    float w = 1.f; /* the effect's share */
    if (o->kind != SFX_BRAKE) {
        long long u = i - o->start + 1;
        if (u < SFX_XF)
            w = (float) u / (float) SFX_XF;
    }
    if (o->off >= 0 && i >= o->off) {
        long long v = i - o->off + 1;
        w *= 1.f - (float) v / (float) SFX_XF;
    }
    *L = dL + w * (wL - dL);
    *R = dR + w * (wR - dR);
}
#endif
