/* SPDX-License-Identifier: GPL-3.0-only */
/* The 60 FPS waveform position feed lock (guest/azmod/waveform.c lptick_hook, feed_run_frame),
 * extracted and built against stand-in globals and stand-in feed objects.
 *
 * The rules under test:
 *   - lock off: every own-timer tick runs the original, and a frame runs nothing;
 *   - lock on: own-timer ticks only record the instance, and each frame runs the original once
 *     per recorded instance, so over a free-running 62.5/s timer against a 59.24 Hz frame clock
 *     the feed runs exactly once per frame;
 *   - an entry is validated (alignment, exact vtable) when the frame uses it, and zeroed when
 *     invalid, never called;
 *   - an entry whose own timer stopped is dropped after FEED_IDLE_MAX frames;
 *   - with no frames running it, the own timer runs the original every FEED_OWN_MAX ticks;
 *   - a full table leaves the extra instance on its own timer;
 *   - the switches are toggled from another thread (ThreadSanitizer case).
 * run.sh builds mutated copies: no validation at use, the original run on every own tick, no
 * idle drop, no own-timer fallback, and a plain read of a cross-thread switch. Each must fail. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "check.h"

#define LOCALPLAYER_VT 0x31545b8UL
#define OTHER_VT 0x2e67bb0UL
static int g_ui60_on, g_wave60_on, g_disp60_armed; /* _Atomic in azmod */

#define NOBJ 12
static uintptr_t g_obj[NOBJ][4] __attribute__((aligned(16)));
static long g_calls[NOBJ], g_bad_calls;
static void orig_stub(long self) {
    for (int k = 0; k < NOBJ; k++)
        if (self == (long) g_obj[k]) {
            __atomic_fetch_add(&g_calls[k], 1, __ATOMIC_RELAXED);
            if (g_obj[k][0] != LOCALPLAYER_VT)
                __atomic_fetch_add(&g_bad_calls, 1, __ATOMIC_RELAXED);
            return;
        }
    __atomic_fetch_add(&g_bad_calls, 1, __ATOMIC_RELAXED);
}

#include FEEDLOCK_INC

static void reset(int on) {
    memset(g_feed, 0, sizeof g_feed);
    memset(g_calls, 0, sizeof g_calls);
    g_bad_calls = 0;
    for (int k = 0; k < NOBJ; k++)
        g_obj[k][0] = LOCALPLAYER_VT;
    __atomic_store_n(&g_ui60_on, on, __ATOMIC_RELAXED);
    __atomic_store_n(&g_wave60_on, on, __ATOMIC_RELAXED);
    __atomic_store_n(&g_disp60_armed, 1, __ATOMIC_RELAXED);
}
static long O(int k) { return (long) g_obj[k]; }
static int in_table(int k) {
    for (int i = 0; i < FEED_MAX; i++)
        if (__atomic_load_n(&g_feed[i].self, __ATOMIC_RELAXED) == O(k))
            return 1;
    return 0;
}

/* Two decks' feeds on 16 ms timers against a 16.88 ms frame clock for `secs`; returns frames. */
static long run_clocks(double secs, int nobj) {
    double tt = 0, tf = 0;
    long frames = 0;
    while (tt < secs * 1000 || tf < secs * 1000) {
        if (tt <= tf) {
            for (int k = 0; k < nobj; k++)
                lptick_hook(O(k));
            tt += 16.0;
        } else {
            feed_run_frame();
            frames++;
            tf += 16.88;
        }
    }
    return frames;
}

static int g_stop;
static void *toggler(void *a) {
    (void) a;
    for (int n = 0; !__atomic_load_n(&g_stop, __ATOMIC_RELAXED); n++) {
        __atomic_store_n(&g_wave60_on, n & 1, __ATOMIC_RELAXED);
        __atomic_store_n(&g_ui60_on, (n >> 1) & 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

int main(void) {
    ut_suite = "feed lock";
    g_orig_lptick = orig_stub;

    reset(0);
    for (int n = 0; n < 10; n++)
        lptick_hook(O(0));
    feed_run_frame();
    CHECK("lock off: each own tick runs the original", g_calls[0] == 10);
    CHECK("lock off: nothing recorded", !in_table(0));

    reset(1);
    long frames = run_clocks(10.0, 2);
    CHECK("lock on: deck 1 feed runs exactly once per frame", g_calls[0] == frames);
    CHECK("lock on: deck 2 feed runs exactly once per frame", g_calls[1] == frames);
    CHECK("lock on: no own-timer pass-through at 62.5/s vs 59.24 Hz", g_feed_pass_n == 0);

    reset(1);
    lptick_hook(O(0));
    g_obj[0][0] = OTHER_VT;
    feed_run_frame();
    CHECK("invalid at use: not called", g_calls[0] == 0 && g_bad_calls == 0);
    CHECK("invalid at use: entry zeroed", !in_table(0));
    g_obj[0][0] = LOCALPLAYER_VT;

    reset(1);
    lptick_hook(O(0));
    for (int n = 0; n < 20; n++)
        feed_run_frame();
    CHECK("own timer stopped: run for FEED_IDLE_MAX frames, then dropped",
          g_calls[0] == FEED_IDLE_MAX && !in_table(0));

    reset(1);
    for (int n = 0; n < 9; n++)
        lptick_hook(O(0));
    CHECK("no frames: the own timer runs it every FEED_OWN_MAX ticks", g_calls[0] == 9 / FEED_OWN_MAX);

    reset(1);
    for (int k = 0; k <= FEED_MAX; k++)
        lptick_hook(O(k));
    CHECK("table full: the extra instance keeps its own timer", g_calls[FEED_MAX] == 1 && !in_table(FEED_MAX));
    for (int k = 0; k < FEED_MAX; k++)
        if (g_calls[k])
            CHECK("table full: recorded instances not run by their own tick", 0);

    reset(1);
    lptick_hook(O(0));
    __atomic_store_n(&g_wave60_on, 0, __ATOMIC_RELAXED);
    feed_run_frame();
    lptick_hook(O(0));
    CHECK("turned off: the frame runs nothing, the own tick runs it", g_calls[0] == 1);
    __atomic_store_n(&g_wave60_on, 1, __ATOMIC_RELAXED);
    lptick_hook(O(0));
    feed_run_frame();
    CHECK("on again: locked to the frame", g_calls[0] == 2);

    reset(1);
    lptick_hook(O(0));
    g_disp60_armed = 0;
    feed_run_frame();
    CHECK("not armed for this boot: the frame runs nothing", g_calls[0] == 0);

    reset(1);
    __atomic_store_n(&g_stop, 0, __ATOMIC_RELAXED);
    pthread_t th;
    pthread_create(&th, NULL, toggler, NULL);
    run_clocks(5.0, 2);
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELAXED);
    pthread_join(th, NULL);
    CHECK("switches toggled from another thread: only valid feeds called", g_bad_calls == 0);

    return ut_summary();
}
