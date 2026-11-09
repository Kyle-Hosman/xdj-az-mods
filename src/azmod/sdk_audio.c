/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file sdk_audio.c
 * @brief Community mod service: audio_tap, read-only blocks of one deck's output, delivered on a
 *        core thread
 *
 * EP147 writes its output to ALSA as one 10-channel signed 32-bit stream: each deck's stereo
 * before the mixer (deck n on channels 2n and 2n+1; 8/9 unused). The XDJ-AZ mixes in hardware,
 * so there is no master to tap. snd_pcm_hw_params() is interposed to recognize that stream, and
 * snd_pcm_writei() copies the channel pair of each deck a mod listens to into that deck's ring
 * (common/aztap.h: one producer, no system call, a full ring drops the whole block). With no
 * listener, writei costs one atomic load per deck.
 * One tap thread (normal priority) drains the rings every 10 ms and calls each subscriber
 * with blocks of the size it asked for. A subscriber that takes too long only makes the ring
 * overflow; the audio thread never waits, and the lost frames are reported in the mod's log.
 * Included by community.c ahead of the services table.
 */
#include "../common/aztap.h"

#define SDK_TAPS 16             /* subscriptions, all mods */
#define SDK_TAPS_MOD 4          /* subscriptions per mod */
#define SDK_TAP_SRC 4           /* decks; channel pair 2d, 2d+1 of EP147's output */
#define SDK_TAP_MIX_CHANNELS 10 /* EP147's output stream */

static struct aztap_ring g_sdk_tap_ring[SDK_TAP_SRC];

static struct sdk_tap {
    cm_mod *c;
    int src;    /* the deck, index into g_sdk_tap_ring */
    int frames; /* block size the mod asked for */
    void (*fn)(const float *lr, int frames, void *user);
    void *user;
    float *acc; /* the block being filled, frames * 2 */
    int have;
    unsigned lost;     /* frames lost since the last log line */
    time_t lost_said;  /* when that line was written (one every 10 s at most) */
} g_sdk_tap[SDK_TAPS];
static int g_sdk_tap_n;
static pthread_mutex_t g_sdk_tap_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_sdk_tap_thread;
static __thread int g_sdk_on_tap_thread; /* audio_tap from a tap callback: refused, not a deadlock */

/* ---- EP147's ALSA output stream ---- */
static _Atomic(void *) g_sdk_mix_pcm; /* the 10-channel S32 playback stream */
/* snd_pcm_writei's next, resolved here rather than on the audio thread's first write: dlsym
 * takes the loader's lock, which a mod's dlopen may be holding at that moment. */
static long (*_Atomic g_sdk_writei_next)(void *, const void *, unsigned long);

int snd_pcm_hw_params(void *pcm, void *params) {
    static int (*next)(void *, void *);
    static int (*get_channels)(const void *, unsigned *);
    static int (*get_format)(const void *, int *);
    static int (*stream_of)(void *);
    if (!next) {
        *(void **) &next = dlsym(RTLD_NEXT, "snd_pcm_hw_params");
        *(void **) &get_channels = dlsym(RTLD_DEFAULT, "snd_pcm_hw_params_get_channels");
        *(void **) &get_format = dlsym(RTLD_DEFAULT, "snd_pcm_hw_params_get_format");
        *(void **) &stream_of = dlsym(RTLD_DEFAULT, "snd_pcm_stream");
    }
    if (!atomic_load_explicit(&g_sdk_writei_next, memory_order_relaxed)) {
        long (*w)(void *, const void *, unsigned long);
        *(void **) &w = dlsym(RTLD_NEXT, "snd_pcm_writei");
        atomic_store_explicit(&g_sdk_writei_next, w, memory_order_relaxed);
    }
    /* This stream's shape may be about to change (or the call may fail): it is not read as the
     * 10-channel output again unless this call configures it as that. */
    void *was = pcm;
    atomic_compare_exchange_strong(&g_sdk_mix_pcm, &was, NULL);
    if (!next)
        return -1;
    int rc = next(pcm, params);
    if (rc == 0 && !g_azm_inert && get_channels && get_format && stream_of) {
        unsigned ch = 0;
        int fmt = -1;
        if (get_channels(params, &ch) == 0 && get_format(params, &fmt) == 0 &&
            ch == SDK_TAP_MIX_CHANNELS && fmt == 10 /* SND_PCM_FORMAT_S32_LE */ &&
            stream_of(pcm) == 0 /* SND_PCM_STREAM_PLAYBACK */)
            atomic_store_explicit(&g_sdk_mix_pcm, pcm, memory_order_release);
    }
    return rc;
}

/* A closed stream is forgotten before its address can be reused by one of another shape. */
int snd_pcm_close(void *pcm) {
    static int (*next)(void *);
    if (!next)
        *(void **) &next = dlsym(RTLD_NEXT, "snd_pcm_close");
    void *want = pcm;
    atomic_compare_exchange_strong(&g_sdk_mix_pcm, &want, NULL);
    return next ? next(pcm) : -1;
}

/* EP147's audio thread: no system call, allocation or lock. */
long snd_pcm_writei(void *pcm, const void *buf, unsigned long n) {
    long (*next)(void *, const void *, unsigned long) =
        atomic_load_explicit(&g_sdk_writei_next, memory_order_relaxed);
    if (!next) { /* a stream configured before azmod was loaded: resolve it once */
        *(void **) &next = dlsym(RTLD_NEXT, "snd_pcm_writei");
        atomic_store_explicit(&g_sdk_writei_next, next, memory_order_relaxed);
    }
    if (buf && n <= (1u << 16) && pcm == atomic_load_explicit(&g_sdk_mix_pcm, memory_order_acquire))
        for (int d = 0; d < SDK_TAP_SRC; d++)
            if (aztap_wanted(&g_sdk_tap_ring[d]))
                aztap_push_s32(&g_sdk_tap_ring[d], (const int32_t *) buf + 2 * d, SDK_TAP_MIX_CHANNELS, (int) n);
    return next ? next(pcm, buf, n) : -1;
}

/* ---- the tap thread ---- */
static void sdk_tap_deliver(struct sdk_tap *t, const float *lr, int n) {
    while (n > 0) {
        int k = t->frames - t->have < n ? t->frames - t->have : n;
        memcpy(t->acc + (size_t) t->have * 2, lr, (size_t) k * 2 * sizeof(float));
        t->have += k;
        lr += (size_t) k * 2;
        n -= k;
        if (t->have == t->frames) {
            t->fn(t->acc, t->frames, t->user);
            t->have = 0;
        }
    }
}

static void *sdk_tap_run(void *arg) {
    (void) arg;
    static float buf[AZTAP_FRAMES * 2];
    g_sdk_on_tap_thread = 1;
    unsigned lost_seen[SDK_TAP_SRC] = {0};
    for (;;) {
        usleep(10000);
        for (int s = 0; s < SDK_TAP_SRC; s++) {
            /* The ring and the subscriber count under the lock; the mods' callbacks outside it, so
             * a slow callback never holds up audio_tap on another thread (init runs on the UI
             * thread). Subscribers are only ever appended, each one complete before the count
             * that covers it is published under this lock, and only this thread touches their
             * block state, so entries below `subs` are safe to use unlocked. */
            pthread_mutex_lock(&g_sdk_tap_mu);
            struct aztap_ring *q = &g_sdk_tap_ring[s];
            if (!aztap_wanted(q)) {
                pthread_mutex_unlock(&g_sdk_tap_mu);
                continue;
            }
            int n = aztap_pop(q, buf, AZTAP_FRAMES);
            unsigned lost = atomic_load_explicit(&q->dropped, memory_order_relaxed);
            int subs = g_sdk_tap_n;
            pthread_mutex_unlock(&g_sdk_tap_mu);
            for (int i = 0; i < subs; i++) {
                struct sdk_tap *t = &g_sdk_tap[i];
                if (t->src != s || !cm_running(t->c))
                    continue; /* a mod refused after it subscribed gets nothing */
                if (lost != lost_seen[s]) {
                    t->have = 0; /* a gap: start a fresh block */
                    t->lost += lost - lost_seen[s];
                }
                if (t->lost && time(NULL) - t->lost_said >= 10) {
                    cm_logf(t->c, 0, "audio_tap: %u frames of deck %d lost (a callback is too slow)",
                            t->lost, s + 1);
                    t->lost = 0;
                    t->lost_said = time(NULL);
                }
                if (n)
                    sdk_tap_deliver(t, buf, n);
            }
            lost_seen[s] = lost;
        }
    }
    return NULL;
}

static int sdk_audio_tap(const char *id, int source, int frames,
                         void (*fn)(const float *lr, int frames, void *user), void *user) {
    cm_mod *c = cm_self(id, CM_RA(), "audio_tap");
    if (!c)
        return -1;
    int s = source >= 0 && source < SDK_TAP_SRC && source < AZMOD_STEM_DECKS ? source : -1;
    const char *why = NULL;
    if (g_sdk_on_tap_thread)
        why = "called from an audio_tap callback";
    else if (!fn || !cm_called_by(c, (uintptr_t) fn))
        why = "the callback is not inside this mod";
    else if (s < 0)
        why = "no such deck (AZ_TAP_DECK(0..3))";
    else if (frames < 64 || frames > 8192)
        why = "frames must be 64..8192";
    if (why) {
        cm_logf(c, 1, "audio_tap refused: %s", why);
        return -1;
    }
    float *acc = malloc((size_t) frames * 2 * sizeof(float));
    pthread_mutex_lock(&g_sdk_tap_mu);
    int rc = -1, mine = 0, full = g_sdk_tap_n >= SDK_TAPS;
    for (int i = 0; i < g_sdk_tap_n; i++)
        mine += g_sdk_tap[i].c == c;
    if (acc && mine < SDK_TAPS_MOD && !full &&
        (g_sdk_tap_thread || (g_sdk_tap_thread = sdk_detach(sdk_tap_run, NULL) == 0))) {
        g_sdk_tap[g_sdk_tap_n++] = (struct sdk_tap) {c, s, frames, fn, user, acc, 0, 0, 0};
        if (!aztap_wanted(&g_sdk_tap_ring[s])) /* the deck's first listener: start it empty */
            aztap_set_wanted(&g_sdk_tap_ring[s], 1);
        rc = 0;
        __atomic_store_n(&c->sticky, 1, __ATOMIC_RELAXED); /* the tap thread holds its entry: no live stop */
    }
    pthread_mutex_unlock(&g_sdk_tap_mu);
    if (rc) {
        cm_logf(c, 1, "audio_tap refused: %s",
                !acc                   ? "out of memory"
                : mine >= SDK_TAPS_MOD ? "this mod already has 4 taps"
                : full                 ? "16 taps are running across all mods"
                                       : "no tap thread");
        free(acc);
    }
    return rc;
}
