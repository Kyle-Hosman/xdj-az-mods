/* SPDX-License-Identifier: GPL-3.0-only */
/* The stem offset correlator, ofs_correlate (extracted out of guest/azstem/key_shift.c by run.sh).
 * Its result is applied only when the fine peak is at least OFS_MINCORR and the coarse peak leads
 * the runner-up by OFS_MARGIN, so the runner-up decides whether a track's stems are moved at all:
 *   - broadband audio: the exact lag, confident;
 *   - bass-heavy audio, whose correlation peak is a lobe many coarse bins wide: the exact lag,
 *     confident (the lobe's shoulder is the same alignment, not a rival);
 *   - audio that repeats inside the search window: two equal peaks, not confident;
 *   - a true lag past the search window: the peak sits on the window's edge, not confident;
 *   - stems that are not this audio: not confident. */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"

#define NSTEM 4
#define OFS_N 131072
#define OFS_LAGMAX 8192
#define OFS_DEC 16
#define OFS_MINCORR 0.80
#define OFS_MARGIN 0.10
typedef struct {
    float *stem[NSTEM];
} deck_stems;
#include OFSCORR_INC

#define SR 44100
#define BASE 40000L
#define LEN (BASE + OFS_N + 4 * OFS_LAGMAX)

static unsigned g_rng = 12345;
static float noise(void) {
    g_rng = g_rng * 1103515245u + 12345u;
    return (float) ((g_rng >> 8) & 0xffff) / 32768.f - 1.f;
}

/* Kick-like 50 Hz bursts at irregular times, decaying, with a little broadband noise. */
static void bass(float *s) {
    for (long i = 0; i < LEN; i++)
        s[i] = 0.01f * noise();
    for (long t = 0; t < LEN;) {
        for (long i = 0; i < SR / 2 && t + i < LEN; i++)
            s[t + i] += (float) (sin(2 * M_PI * 50.0 * i / SR) * exp(-i / (SR * 0.15)));
        t += SR / 3 + (long) ((noise() + 1.f) * SR / 5);
    }
}
static void broadband(float *s) {
    for (long i = 0; i < LEN; i++)
        s[i] = noise() * (float) (0.5 + 0.5 * sin(2 * M_PI * 2.0 * i / SR));
}
/* Two slow sines (periods past the whole search window): one correlation lobe, wider than the
 * window, so a true lag outside it puts the highest correlation on the window's edge. */
static void slow(float *s) {
    for (long i = 0; i < LEN; i++)
        s[i] = (float) (sin(2 * M_PI * 1.7 * i / SR) + 0.5 * sin(2 * M_PI * 1.1 * i / SR + 1.0));
}
/* One noise block repeated: every lag a multiple of `period` away aligns as well as the true one. */
static void repeating(float *s, long period) {
    for (long i = 0; i < period; i++)
        s[i] = noise();
    for (long i = period; i < LEN; i++)
        s[i] = s[i - period];
}

/* Stems = the source moved by `lag` (orig[p] ~ stem[p - lag]), all in the first stem, stereo. */
static ofs_res_t measure(const float *src, const float *stemsrc, long lag) {
    static float st[LEN * 2];
    for (long q = 0; q < LEN; q++) {
        long p = q + lag;
        float v = (p >= 0 && p < LEN) ? stemsrc[p] : 0.f;
        st[q * 2] = st[q * 2 + 1] = v * 0.5f;
    }
    deck_stems dk = {{st, NULL, NULL, NULL}};
    ofs_res_t r = {0};
    ofs_correlate(&dk, LEN, src + BASE, BASE, &r);
    return r;
}
static int confident(ofs_res_t r) {
    return r.fine >= OFS_MINCORR && r.coarse - r.second >= OFS_MARGIN;
}

int main(void) {
    ut_suite = "stem offset correlator";
    static float a[LEN], b[LEN];
    char name[160];

    broadband(a);
    static const long lags[] = {0, 2257, -1105, 528, -7000};
    for (size_t i = 0; i < sizeof lags / sizeof lags[0]; i++) {
        ofs_res_t r = measure(a, a, lags[i]);
        snprintf(name, sizeof name, "broadband, lag %ld: exact (%ld) and confident (fine %.3f, lead %.3f)",
                 lags[i], r.lag, r.fine, r.coarse - r.second);
        CHECK(name, r.lag == lags[i] && confident(r));
    }

    bass(a);
    for (size_t i = 0; i < sizeof lags / sizeof lags[0]; i++) {
        ofs_res_t r = measure(a, a, lags[i]);
        snprintf(name, sizeof name, "bass-heavy, lag %ld: exact (%ld) and confident (fine %.3f, lead %.3f)",
                 lags[i], r.lag, r.fine, r.coarse - r.second);
        CHECK(name, r.lag == lags[i] && confident(r));
    }

    repeating(a, 2048);
    {
        ofs_res_t r = measure(a, a, 2257);
        snprintf(name, sizeof name, "audio repeating every 2048 samples: not confident (lead %.3f)",
                 r.coarse - r.second);
        CHECK(name, !confident(r));
    }

    bass(a);
    {
        ofs_res_t r = measure(a, a, OFS_LAGMAX + 3000);
        snprintf(name, sizeof name, "bass-heavy, lag past the window: not confident (lag %ld, lead %.3f)",
                 r.lag, r.coarse - r.second);
        CHECK(name, !confident(r));
        r = measure(a, a, -(OFS_LAGMAX + 3000));
        snprintf(name, sizeof name, "bass-heavy, lag before the window: not confident (lag %ld, lead %.3f)",
                 r.lag, r.coarse - r.second);
        CHECK(name, !confident(r));
    }

    slow(a);
    {
        ofs_res_t r = measure(a, a, OFS_LAGMAX + 1000);
        snprintf(name, sizeof name, "slow audio, lag past the window: not confident (lag %ld, fine %.3f, lead %.3f)",
                 r.lag, r.fine, r.coarse - r.second);
        CHECK(name, !confident(r));
        r = measure(a, a, -(OFS_LAGMAX + 1000));
        snprintf(name, sizeof name, "slow audio, lag before the window: not confident (lag %ld, fine %.3f, lead %.3f)",
                 r.lag, r.fine, r.coarse - r.second);
        CHECK(name, !confident(r));
    }

    broadband(a);
    g_rng = 999;
    broadband(b);
    {
        ofs_res_t r = measure(a, b, 0);
        snprintf(name, sizeof name, "stems of other audio: not confident (fine %.3f)", r.fine);
        CHECK(name, !confident(r));
    }
    return ut_summary();
}
