/* SPDX-License-Identifier: GPL-3.0-only */
/* Stem FX (guest/azstem/stem_fx.h): the echo out and brake rendered from the stem in RAM, the
 * press state shared by the panel-bus writer and the audio readers. SFX_INC = the stem_fx.h
 * under test (the shipped copy or a control with one protection removed).
 *
 *   echo     only audio from before the cut repeats, at the exact taps, with the dry fade; a cut
 *            one delay after the press repeats the phrase from the press, never what came before;
 *   brake    continuous at its start, slows, never runs past its stop, silent after it;
 *   off      dry again exactly SFX_XF frames after the latched release;
 *   pages    a page rendered twice, or out of order, comes out the same;
 *   latches  start / off taken by the first page that reaches them, never by a reader still
 *            holding an older press, and never for another track's press;
 *   threads  one writer pressing and releasing while readers render: every snapshot is one
 *            whole press (ThreadSanitizer build: no race). */
#include "check.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include SFX_INC

#define NF 200000
static float g_stem[NF * 2];

static void fill_ramp(void) { /* distinct value per frame: any wrong tap is visible */
    for (long i = 0; i < NF; i++) {
        g_stem[i * 2] = (float) (i % 977) / 977.0f;
        g_stem[i * 2 + 1] = -(float) (i % 613) / 613.0f;
    }
}
static int feq(float a, float b) { return fabsf(a - b) <= 1e-5f * (1.0f + fabsf(b)); }

/* the echo by its definition, independent of sfx_echo_wet */
static void echo_ref(long long on, long long D, long long i, float *L, float *R) {
    double l = 0, r = 0, w = 1;
    for (int m = 1; m <= SFX_TAPS; m++) {
        w *= SFX_FB;
        long long j = i - m * D;
        if (j < 0)
            break;
        if (j >= on)
            continue;
        l += w * g_stem[j * 2];
        r += w * g_stem[j * 2 + 1];
    }
    *L = (float) l;
    *R = (float) r;
}

static sfx_slot g_s;
static void render(sfx_slot *s, unsigned tg, long long b0, long long n, float *outL) {
    sfx_snap o;
    int have = sfx_read(s, &o, tg);
    if (have)
        sfx_latch(s, &o, b0, b0 + n);
    for (long long i = 0; i < n; i++) {
        float L, R;
        if (have)
            sfx_sample(g_stem, NF, &o, b0 + i, &L, &R);
        else {
            L = g_stem[(b0 + i) * 2];
            R = g_stem[(b0 + i) * 2 + 1];
        }
        outL[i] = L;
    }
}

static void test_echo(void) {
    memset(&g_s, 0, sizeof g_s);
    const long long on = 50000, D = 11025;
    CHECK("press published", sfx_press(&g_s, SFX_ECHO, on, D, 7));
    sfx_snap o;
    CHECK("read", sfx_read(&g_s, &o, 7));
    sfx_latch(&g_s, &o, on - 100, on + 4096); /* the page holding the press */
    CHECK("start latched at the press", o.start == on);
    float L, R, rL, rR;
    sfx_sample(g_stem, NF, &o, on - 1, &L, &R);
    CHECK("dry before the press", feq(L, g_stem[(on - 1) * 2]) && feq(R, g_stem[(on - 1) * 2 + 1]));
    int ok = 1;
    for (long long i = on + SFX_XF; i < on + SFX_TAPS * D + 5000; i += 37) {
        sfx_sample(g_stem, NF, &o, i, &L, &R);
        echo_ref(on, D, i, &rL, &rR);
        if (!feq(L, rL) || !feq(R, rR))
            ok = 0;
    }
    CHECK("echo = the taps before the press, after the fade", ok);
    sfx_sample(g_stem, NF, &o, on + SFX_XF / 2, &L, &R);
    echo_ref(on, D, on + SFX_XF / 2, &rL, &rR);
    float dL = g_stem[(on + SFX_XF / 2) * 2];
    CHECK("half way through the fade: between dry and wet",
          fabsf(L - (dL + 0.5f * (rL - dL))) < 0.01f);
    /* post-press audio never repeats: zero everything before the press, the tail is silent */
    static float z[NF * 2];
    for (long i = on; i < NF; i++) {
        z[i * 2] = 1.0f;
        z[i * 2 + 1] = 1.0f;
    }
    float mx = 0;
    for (long long i = on + SFX_XF; i < on + 6 * D; i++) {
        sfx_sample(z, NF, &o, i, &L, &R);
        if (fabsf(L) > mx)
            mx = fabsf(L);
    }
    CHECK("only audio before the press repeats", mx == 0.0f);
    sfx_sample(g_stem, NF, &o, on + (SFX_TAPS + 1) * D, &L, &R);
    CHECK("the tail ends after SFX_TAPS repeats", L == 0.0f && R == 0.0f);
}

static void test_off(void) {
    memset(&g_s, 0, sizeof g_s);
    const long long on = 30000, D = 5000, rel = 52000;
    sfx_press(&g_s, SFX_ECHO, on, D, 1);
    float buf[8192];
    render(&g_s, 1, on, 4096, buf);
    sfx_release(&g_s, rel);
    sfx_snap o;
    sfx_read(&g_s, &o, 1);
    CHECK("release published", o.offq == rel);
    sfx_latch(&g_s, &o, rel - 10, rel + 4096);
    CHECK("off latched at the release", o.off == rel);
    float L, R;
    sfx_sample(g_stem, NF, &o, rel + SFX_XF, &L, &R);
    CHECK("dry again SFX_XF after the release", feq(L, g_stem[(rel + SFX_XF) * 2]));
    sfx_sample(g_stem, NF, &o, rel + SFX_XF - 200, &L, &R);
    CHECK("still fading in just before", !feq(L, g_stem[(rel + SFX_XF - 200) * 2]));
    {
        long long h = rel + SFX_XF / 2 - 1; /* the effect's share is exactly 1/2 here */
        float wL, wR, dry = g_stem[h * 2];
        sfx_sample(g_stem, NF, &o, h, &L, &R);
        echo_ref(on, D, h, &wL, &wR);
        CHECK("half way through the release fade: between wet and dry",
              fabsf(L - (dry + 0.5f * (wL - dry))) < 1e-4f);
    }
    CHECK("done after the fade", sfx_done_at(&o, rel + SFX_XF) && !sfx_done_at(&o, rel + 10));
}

static void test_brake(void) {
    memset(&g_s, 0, sizeof g_s);
    const long long on = 40000, T = 44100;
    sfx_press(&g_s, SFX_BRAKE, on, T, 3);
    sfx_snap o;
    sfx_read(&g_s, &o, 3);
    sfx_latch(&g_s, &o, on + 300, on + 4096); /* the press's page already played: later start */
    CHECK("start latched at the first page rendered", o.start == on + 300);
    float L, R;
    sfx_sample(g_stem, NF, &o, o.start, &L, &R);
    CHECK("continuous at its start", feq(L, g_stem[o.start * 2]));
    sfx_sample(g_stem, NF, &o, o.start - 1, &L, &R);
    CHECK("dry before its start", feq(L, g_stem[(o.start - 1) * 2]));
    /* the read position: monotone, at most start + T/2 (sampled through a ramp stem) */
    static float ramp[NF * 2];
    for (long i = 0; i < NF; i++)
        ramp[i * 2] = ramp[i * 2 + 1] = (float) i / (float) NF;
    float prev = -1, mx = 0;
    int mono = 1;
    for (long long u = 0; u < (T * 3) / 4; u += 11) {
        sfx_sample(ramp, NF, &o, o.start + u, &L, &R);
        if (L + 1e-6f < prev)
            mono = 0;
        prev = L;
        if (L > mx)
            mx = L;
    }
    CHECK("slows without going back", mono);
    CHECK("never past start + T/2", mx <= (float) (o.start + T / 2 + 1) / (float) NF);
    sfx_sample(g_stem, NF, &o, o.start + T, &L, &R);
    CHECK("silent at its stop", L == 0.0f && R == 0.0f);
    sfx_sample(g_stem, NF, &o, o.start + T + 9999, &L, &R);
    CHECK("silent after it while held", L == 0.0f && R == 0.0f);
}

static void test_pages(void) {
    const long long on = 60000;
    static float a[40000], b[40000];
    memset(&g_s, 0, sizeof g_s);
    sfx_press(&g_s, SFX_ECHO, on, 8000, 2);
    render(&g_s, 2, on - 2000, 40000, a); /* one page */
    /* the same press in three pages, out of order, then the first one again */
    memset(&g_s, 0, sizeof g_s);
    sfx_press(&g_s, SFX_ECHO, on, 8000, 2);
    render(&g_s, 2, on - 2000, 7000, b);
    render(&g_s, 2, on - 2000 + 20000, 20000, b + 20000);
    render(&g_s, 2, on - 2000 + 7000, 13000, b + 7000);
    int same = memcmp(a, b, sizeof a) == 0;
    render(&g_s, 2, on - 2000, 7000, b);
    same = same && memcmp(a, b, sizeof a) == 0;
    CHECK("a page rendered twice or out of order comes out the same", same);
}

static void test_latches(void) {
    memset(&g_s, 0, sizeof g_s);
    sfx_press(&g_s, SFX_ECHO, 10000, 4000, 5);
    sfx_snap o;
    sfx_read(&g_s, &o, 5);
    sfx_latch(&g_s, &o, 2000, 6000); /* a page entirely before the press */
    CHECK("a page before the press latches nothing", o.start < 0);
    sfx_snap old = o;
    sfx_press(&g_s, SFX_ECHO, 20000, 4000, 5); /* a newer press */
    sfx_latch(&g_s, &old, 15000, 19000);       /* a reader still holding the first press */
    sfx_snap n;
    sfx_read(&g_s, &n, 5);
    CHECK("an older press cannot latch into a newer one", n.start < 0);
    CHECK("another track's press is ignored", !sfx_read(&g_s, &n, 6));
    /* a writer mid-update: the reader plays dry rather than use a half-written press */
    unsigned q = atomic_load(&g_s.seq);
    atomic_store(&g_s.seq, q | 1u);
    CHECK("mid-update: no snapshot", !sfx_read(&g_s, &n, 5));
    CHECK("mid-update: a second writer gives up", !sfx_press(&g_s, SFX_ECHO, 1, 1, 5));
    atomic_store(&g_s.seq, (q | 1u) + 1u);
    CHECK("after it: readable again", sfx_read(&g_s, &n, 5));
    sfx_release(&g_s, 30000);
    sfx_release(&g_s, 99999);
    sfx_read(&g_s, &n, 5);
    CHECK("a second release does not move the first", n.offq == 30000);
}

/* ---- threads: one writer, two readers ---- */
static sfx_slot g_t;
static atomic_int g_stop, g_bad;
static void *writer(void *a) {
    (void) a;
    for (int k = 1; k < 20000; k++) {
        /* every press keeps on == len * 3 and kind = parity: a torn snapshot breaks it */
        sfx_press(&g_t, (k & 1) ? SFX_ECHO : SFX_BRAKE, (long long) k * 300, (long long) k * 100, 9);
        if (k % 3 == 0)
            sfx_release(&g_t, (long long) k * 300 + 50);
    }
    atomic_store(&g_stop, 1);
    return 0;
}
static void *reader(void *a) {
    (void) a;
    float L, R;
    while (!atomic_load(&g_stop)) {
        sfx_snap o;
        if (!sfx_read(&g_t, &o, 9))
            continue;
        long long k = o.len / 100;
        if (o.on != o.len * 3 || o.kind != ((k & 1) ? SFX_ECHO : SFX_BRAKE) ||
            (o.offq >= 0 && o.offq != o.on + 50))
            atomic_store(&g_bad, 1);
        sfx_latch(&g_t, &o, o.on, o.on + 256);
        if (o.start >= 0 && (o.start < o.on || o.start >= o.on + 256 + 1000000))
            atomic_store(&g_bad, 1);
        long long i = o.on + 100;
        if (i < NF)
            sfx_sample(g_stem, NF, &o, i, &L, &R);
    }
    return 0;
}
static void test_threads(void) {
    memset(&g_t, 0, sizeof g_t);
    pthread_t w, r1, r2;
    pthread_create(&r1, 0, reader, 0);
    pthread_create(&r2, 0, reader, 0);
    pthread_create(&w, 0, writer, 0);
    pthread_join(w, 0);
    pthread_join(r1, 0);
    pthread_join(r2, 0);
    CHECK("threads: every snapshot is one whole press", !atomic_load(&g_bad));
}

/* An echo of the phrase from the press: the cut is one delay later, the stem plays on dry until
 * it, and what repeats is the audio from the press on, never from before it. */
static void test_echo_from_press(void) {
    const long long press = 50000, D = 11025;
    CHECK("cut: before the press = the press", sfx_echo_cut(press, D, 0) == press);
    CHECK("cut: from the press = one delay later", sfx_echo_cut(press, D, 1) == press + D);
    memset(&g_s, 0, sizeof g_s);
    const long long on = sfx_echo_cut(press, D, 1);
    CHECK("press published (from the press)", sfx_press(&g_s, SFX_ECHO_PHRASE, on, D, 7));
    sfx_snap o;
    CHECK("read (from the press)", sfx_read(&g_s, &o, 7));
    sfx_latch(&g_s, &o, press - 100, press + 4096); /* the page holding the press */
    CHECK("not latched before the cut", o.start < 0);
    sfx_latch(&g_s, &o, on - 100, on + 4096);
    CHECK("start latched at the cut", o.start == on);
    float L, R;
    int ok = 1;
    for (long long i = press; i < on; i += 31) {
        sfx_sample(g_stem, NF, &o, i, &L, &R);
        if (!feq(L, g_stem[i * 2]) || !feq(R, g_stem[i * 2 + 1]))
            ok = 0;
    }
    CHECK("dry from the press to the cut", ok);
    ok = 1;
    for (long long i = on + SFX_XF; i < on + D; i += 29) {
        sfx_sample(g_stem, NF, &o, i, &L, &R);
        if (!feq(L, SFX_FB * g_stem[(i - D) * 2]) || !feq(R, SFX_FB * g_stem[(i - D) * 2 + 1]))
            ok = 0;
    }
    CHECK("first repeat = the phrase from the press", ok);
    ok = 1;
    for (long long i = on + 2 * D + 7; i < on + 3 * D; i += 41) {
        sfx_sample(g_stem, NF, &o, i, &L, &R);
        float w = SFX_FB * SFX_FB * SFX_FB;
        if (!feq(L, w * g_stem[(i - 3 * D) * 2]))
            ok = 0;
    }
    CHECK("third repeat = the same phrase, three repeats down", ok);
    /* audio before the press never repeats: zero everything from the press on, the tail is silent */
    static float z[NF * 2];
    for (long i = 0; i < press; i++) {
        z[i * 2] = 1.0f;
        z[i * 2 + 1] = 1.0f;
    }
    float mx = 0;
    for (long long i = on + SFX_XF; i < on + 6 * D; i++) {
        sfx_sample(z, NF, &o, i, &L, &R);
        if (fabsf(L) > mx)
            mx = fabsf(L);
    }
    CHECK("audio before the press never repeats", mx == 0.0f);
}

int main(void) {
    ut_suite = "stem fx";
    fill_ramp();
    test_echo();
    test_echo_from_press();
    test_off();
    test_brake();
    test_pages();
    test_latches();
    test_threads();
    return ut_summary();
}
