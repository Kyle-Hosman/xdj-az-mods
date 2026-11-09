/* SPDX-License-Identifier: GPL-3.0-only */
/* audio_tap for community mods (guest/azmod/sdk_audio.c), from EP147's 10-channel output to a
 * mod's callback. The real file is built against stand-ins for the community host; the stream
 * EP147 would open is a fake handle, and each sample carries its frame and channel number.
 * Questions asked:
 *   - a mod listening to deck n gets exactly channels 2n and 2n+1, in blocks of the size it
 *     asked for, on the tap thread;
 *   - writes to any other stream, and decks nobody listens to, are not copied;
 *   - after EP147 closes the stream, writes to a handle at the same address are not read as
 *     10-channel audio (a reused address may be a stream of another shape);
 *   - a stream configured again (snd_pcm_hw_params on the same handle) is not read as 10-channel
 *     audio unless the new configuration is that: its buffers may now be of another shape;
 *   - a callback that takes a long time never holds up audio_tap on another thread;
 *   - 4 taps per mod and 16 in all;
 *   - refused: a deck outside 0..3, a block size outside 64..8192, a callback that is not the
 *     mod's, and audio_tap called from inside a tap callback (it would deadlock).
 * The controls read deck 1's channels for every deck, keep a closed stream, keep a re-configured
 * stream, and call the callbacks under the subscribers' lock; each must fail.
 * AUDIO_INC = the sdk_audio.c under test. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "check.h"
#include "azmod-mod.h"

#define AZMOD_STEM_DECKS 4
#define AZCM_LOADED 1
typedef struct {
    char id[33];
    int state;
    int sticky;
} cm_mod;
static int cm_running(const cm_mod *c) { return __atomic_load_n(&c->state, __ATOMIC_ACQUIRE) == AZCM_LOADED; }
/* clang-format off */
static cm_mod g_cm[5] = {{"mod-a", AZCM_LOADED}, {"mod-b", AZCM_LOADED}, {"mod-c", AZCM_LOADED},
                         {"mod-d", AZCM_LOADED}, {"mod-e", AZCM_LOADED}};
/* clang-format on */
static int g_azm_inert;
static char g_last_log[256];
static cm_mod *cm_self(const char *id, uintptr_t ra, const char *what) {
    (void) ra, (void) what;
    for (int i = 0; id && i < 5; i++)
        if (!strcmp(id, g_cm[i].id))
            return &g_cm[i];
    return NULL;
}
static void on_nothing(const float *lr, int frames, void *user) { (void) lr, (void) frames, (void) user; }
#define CM_RA() ((uintptr_t) 0)
static uintptr_t g_not_mine; /* the one callback address treated as outside the mod */
static int cm_called_by(cm_mod *c, uintptr_t a) {
    (void) c;
    return a != g_not_mine;
}
static void cm_logf(cm_mod *c, int force, const char *fmt, ...) {
    (void) c, (void) force;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last_log, sizeof g_last_log, fmt, ap);
    va_end(ap);
}
static int sdk_detach(void *(*fn)(void *), void *arg) {
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &a, fn, arg);
    pthread_attr_destroy(&a);
    return rc;
}

#include AUDIO_INC

static _Atomic int g_hold, g_in_cb; /* g_hold: callbacks wait inside until it clears */

/* what one callback saw */
struct seen {
    _Atomic int blocks, frames_bad, size_bad;
    _Atomic int reentry; /* audio_tap from inside the callback: its result + 2 (0 = not tried) */
    int deck, want_frames;
    _Atomic unsigned next_frame;
};
static void on_block(const float *lr, int frames, void *user) {
    struct seen *s = user;
    if (atomic_load(&g_hold)) {
        atomic_store(&g_in_cb, 1);
        while (atomic_load(&g_hold))
            usleep(1000);
    }
    if (frames != s->want_frames)
        atomic_fetch_add(&s->size_bad, 1);
    for (int i = 0; i < frames; i++) {
        /* sample = (frame * 16 + channel) / 2^31 * 2^20, see push_frames */
        long l = lrintf(lr[i * 2] * 2048.0f), r = lrintf(lr[i * 2 + 1] * 2048.0f);
        if (l % 16 != 2 * s->deck || r % 16 != 2 * s->deck + 1 || l / 16 != r / 16)
            atomic_fetch_add(&s->frames_bad, 1);
    }
    atomic_fetch_add(&s->blocks, 1);
    if (!atomic_load(&s->reentry))
        atomic_store(&s->reentry, sdk_audio_tap("mod-a", AZ_TAP_DECK(0), 64, on_block, s) + 2);
}

/* one writei of n frames: channel c of frame f = (f * 16 + c) << 20 (exact in a float) */
static int32_t g_buf[1024 * SDK_TAP_MIX_CHANNELS];
static unsigned g_frame;
static void push_frames(void *pcm, int n) {
    for (int i = 0; i < n; i++, g_frame++)
        for (int c = 0; c < SDK_TAP_MIX_CHANNELS; c++)
            g_buf[i * SDK_TAP_MIX_CHANNELS + c] = (int32_t) (((g_frame & 0x7F) * 16 + (unsigned) c) << 20);
    snd_pcm_writei(pcm, g_buf, (unsigned long) n);
}

static _Atomic int g_reg_done;
static void *register_late(void *arg) { /* a registration while a callback is busy */
    (void) arg;
    sdk_audio_tap("mod-e", AZ_TAP_DECK(0), 256, on_nothing, NULL);
    atomic_store(&g_reg_done, 1);
    return NULL;
}

/* ends as soon as the blocks are there; the 30 s cap only ends a tap that delivers nothing */
static int wait_blocks(struct seen *s, int n) {
    for (int i = 0; i < 3000 && atomic_load(&s->blocks) < n; i++)
        usleep(10000);
    return atomic_load(&s->blocks) >= n;
}

int main(void) {
    ut_suite = "sdk: audio_tap";
    static char ep_pcm[64], other_pcm[64];
    struct seen d2 = {.deck = 1, .want_frames = 256}, bad = {0};
    CHECK("a deck outside 0..3 is refused", sdk_audio_tap("mod-a", 4, 256, on_block, &bad) == -1 &&
                                                sdk_audio_tap("mod-a", -1, 256, on_block, &bad) == -1);
    CHECK("a block size outside 64..8192 is refused", sdk_audio_tap("mod-a", 0, 32, on_block, &bad) == -1 &&
                                                          sdk_audio_tap("mod-a", 0, 9000, on_block, &bad) == -1);
    g_not_mine = (uintptr_t) on_block;
    CHECK("a callback that is not the mod's is refused", sdk_audio_tap("mod-a", 0, 256, on_block, &bad) == -1);
    g_not_mine = 0;

    atomic_store(&g_sdk_mix_pcm, (void *) ep_pcm); /* what snd_pcm_hw_params recognizes */
    push_frames(ep_pcm, 512);
    CHECK("with nobody listening nothing is copied", atomic_load(&g_sdk_tap_ring[1].w) == 0);

    CHECK("deck 2 subscribed", sdk_audio_tap("mod-a", AZ_TAP_DECK(1), 256, on_block, &d2) == 0);
    for (int k = 0; k < 8; k++) {
        push_frames(ep_pcm, 128);
        usleep(2000);
    }
    CHECK("blocks arrive on the tap thread", wait_blocks(&d2, 4));
    CHECK("each block is the size the mod asked for", atomic_load(&d2.size_bad) == 0);
    CHECK("deck 2's block holds channels 2 and 3, frame-aligned", atomic_load(&d2.frames_bad) == 0);
    CHECK("decks nobody listens to are not copied", atomic_load(&g_sdk_tap_ring[0].w) == 0 &&
                                                        atomic_load(&g_sdk_tap_ring[2].w) == 0 &&
                                                        atomic_load(&g_sdk_tap_ring[3].w) == 0);
    CHECK("audio_tap from inside a tap callback is refused (no deadlock)", atomic_load(&d2.reentry) == 1);

    unsigned w = atomic_load(&g_sdk_tap_ring[1].w);
    push_frames(other_pcm, 128);
    CHECK("another stream's writes are not copied", atomic_load(&g_sdk_tap_ring[1].w) == w);
    snd_pcm_close(ep_pcm);
    push_frames(ep_pcm, 128); /* a new stream of another shape at the same address */
    CHECK("after the stream closes, its address is no longer read", atomic_load(&g_sdk_tap_ring[1].w) == w);
    atomic_store(&g_sdk_mix_pcm, (void *) ep_pcm);
    snd_pcm_hw_params(ep_pcm, NULL); /* configured again: as what is not known here */
    push_frames(ep_pcm, 128);
    CHECK("a stream configured again is no longer read as the 10-channel output",
          atomic_load(&g_sdk_tap_ring[1].w) == w);

    /* limits: 4 taps per mod, 16 in all (mod-a has 1 so far) */
    int ok = 1;
    for (int k = 0; k < 3; k++)
        ok &= sdk_audio_tap("mod-a", AZ_TAP_DECK(k), 256, on_nothing, NULL) == 0;
    CHECK("a mod may have 4 taps", ok);
    CHECK("a fifth from the same mod is refused, in its log",
          sdk_audio_tap("mod-a", AZ_TAP_DECK(3), 256, on_nothing, NULL) == -1 && strstr(g_last_log, "already has 4"));
    ok = 1;
    for (int m = 1; m < 4; m++)
        for (int k = 0; k < 4; k++)
            ok &= sdk_audio_tap(g_cm[m].id, AZ_TAP_DECK(k), 256, on_nothing, NULL) == 0;
    CHECK("other mods have taps of their own, up to 16 in all", ok);
    CHECK("the 17th, from a mod with none yet, is refused, in its log",
          sdk_audio_tap("mod-e", AZ_TAP_DECK(0), 256, on_nothing, NULL) == -1 && strstr(g_last_log, "16 taps"));

    /* A callback busy for a long time: audio_tap on another thread still answers at once. The
     * callback is parked inside (until g_hold clears) and nothing else takes the subscribers'
     * lock, so whether the tap thread holds it across callbacks is decided by one trylock, not
     * by how long a registration takes. The push loop ends when a callback is inside; its 30 s
     * cap only ends a tap that never calls back. */
    atomic_store(&g_sdk_mix_pcm, (void *) ep_pcm);
    atomic_store(&g_hold, 1);
    for (int k = 0; k < 3000 && !atomic_load(&g_in_cb); k++) {
        push_frames(ep_pcm, 128);
        usleep(10000);
    }
    int in_cb = atomic_load(&g_in_cb);
    CHECK("a callback is busy (the precondition)", in_cb);
    int lock_free = in_cb && pthread_mutex_trylock(&g_sdk_tap_mu) == 0;
    if (lock_free)
        pthread_mutex_unlock(&g_sdk_tap_mu);
    CHECK("the subscribers' lock is free while a callback is busy", lock_free);
    if (lock_free) { /* so a registration on another thread runs straight through */
        pthread_t th;
        pthread_create(&th, NULL, register_late, NULL);
        pthread_join(th, NULL);
        CHECK("audio_tap answers while a callback is busy", atomic_load(&g_reg_done));
    }
    atomic_store(&g_hold, 0);
    return ut_summary();
}
