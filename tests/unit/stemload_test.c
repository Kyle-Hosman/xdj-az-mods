/* SPDX-License-Identifier: GPL-3.0-only */
/* The load side of the stems, extracted by run.sh: stems_mem_admit / stems_mem_done and
 * decode_stem_progressive (guest/azstem/stem_playback.c) and stb_vorbis's interleaved float read
 * (guest/azstem/stb_vorbis.c), against a scripted stream.
 *
 *   - admission: a set fits only above the floor, a deck's held stems count as free, and what
 *     another deck's load has yet to write counts as used, so overlapping loads cannot together
 *     take the memory each saw as free; several loaders at once never overcommit;
 *   - decode: mono, stereo and multichannel streams land as the same interleaved stereo; the
 *     stream handed over is closed once whatever happens; a stream that did not open drops its
 *     slot and leaves the others gating;
 *   - the interleaved read: every layout of file channels against asked channels, across decoded
 *     frame boundaries. */
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "check.h"

#define NDECK 4
#define NSTEM 4
#define AZW_N 16
#define AZ_EP147_RATE 44100
#define AZSTEM_MEM_FLOOR_MB 400

/* the scripted stream, with the fields stb's read uses */
#define BLOCK 1024
typedef struct {
    int channels;
    unsigned sample_rate;
    long total, pos;
    int closed;
    float *channel_buffers[16];
    int channel_buffer_start, channel_buffer_end;
} stb_vorbis;
typedef struct {
    unsigned int sample_rate;
    int channels;
} stb_vorbis_info;
static float sample_at(int ch, long i) {
    return (float) (ch + 1) + (float) (i % 4096) / 8192.0f;
}
static stb_vorbis *stream_new(int ch, unsigned rate, long total) {
    stb_vorbis *v = calloc(1, sizeof *v);
    v->channels = ch;
    v->sample_rate = rate;
    v->total = total;
    for (int c = 0; c < ch; c++)
        v->channel_buffers[c] = calloc(BLOCK, sizeof(float));
    return v;
}
static void stream_free(stb_vorbis *v) {
    for (int c = 0; c < v->channels; c++)
        free(v->channel_buffers[c]);
    free(v);
}
/* A short block now and then, as real packets have: the read must cross these. */
static int stb_vorbis_get_frame_float(stb_vorbis *f, int *channels, float ***output) {
    (void) channels;
    (void) output;
    long left = f->total - f->pos;
    int n = (f->pos % 3) ? BLOCK : BLOCK / 8;
    if (n > left)
        n = (int) left;
    for (int c = 0; c < f->channels; c++)
        for (int i = 0; i < n; i++)
            f->channel_buffers[c][i] = sample_at(c, f->pos + i);
    f->pos += n;
    f->channel_buffer_start = 0;
    f->channel_buffer_end = n;
    return n;
}
#include STBREAD_INC
static stb_vorbis_info stb_vorbis_get_info(stb_vorbis *v) {
    stb_vorbis_info in = {v->sample_rate, v->channels};
    return in;
}
static unsigned stb_vorbis_stream_length_in_samples(stb_vorbis *v) {
    return (unsigned) v->total;
}
static void stb_vorbis_close(stb_vorbis *v) {
    v->closed++;
}

typedef struct {
    const float *stem[NSTEM];
    size_t stem_bytes[NSTEM];
    int stem_mmap[NSTEM];
    long stem_frames;
} deck_stems;
static deck_stems g_dk[NDECK];
static pthread_mutex_t g_envp_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_env_seeded[NDECK], g_env_hit[NDECK], g_env_filled[NDECK];
static long g_env_total[NDECK];
static unsigned char g_env[NDECK][NSTEM][AZW_N];
static _Atomic long g_stem_dec[NDECK][NSTEM];
static _Atomic unsigned g_stem_present[NDECK];
static _Atomic int g_load_stale[NDECK];
static int g_advanced;
static void advance_stem_frames(int deck) {
    (void) deck;
    g_advanced++;
}
static long g_resampled;
static float *decode_ogg_stereo_44100(const char *path, long *out_frames, long max_frames) {
    (void) path;
    (void) max_frames;
    g_resampled++;
    *out_frames = 8;
    return calloc(8 * 2, sizeof(float));
}
static _Atomic long long g_fake_avail;
static long long mem_available_bytes(void) {
    return atomic_load(&g_fake_avail);
}
#include STEMLOAD_INC

#define MB (1LL << 20)

static void test_admit(void) {
    long long a, p;
    atomic_store(&g_fake_avail, 2000 * MB);
    CHECK("a set above the floor is admitted", stems_mem_admit(0, 1000 * MB, 0, &a, &p));
    stems_mem_done(0);
    CHECK("exactly at the floor is admitted", stems_mem_admit(0, 1600 * MB, 0, &a, &p));
    stems_mem_done(0);
    CHECK("a set that would go below the floor is refused",
          !stems_mem_admit(0, 1601 * MB, 0, &a, &p));
    CHECK("this deck's held stems count as free", stems_mem_admit(0, 1800 * MB, 300 * MB, &a, &p));
    stems_mem_done(0);
    atomic_store(&g_fake_avail, -1);
    CHECK("MemAvailable unreadable is a refusal", !stems_mem_admit(0, MB, 0, &a, &p));
    stems_mem_done(0);

    /* overlapping loads: MemAvailable has not moved, nothing is written yet */
    atomic_store(&g_fake_avail, 2000 * MB);
    CHECK("first of two overlapping loads is admitted", stems_mem_admit(0, 1000 * MB, 0, &a, &p));
    CHECK("the second sees the first's unwritten memory as used",
          !stems_mem_admit(1, 1000 * MB, 0, &a, &p) && p == 1000 * MB);
    /* 750 MB written by deck 0: MemAvailable drops by as much and only the rest stays reserved */
    atomic_store(&g_stem_dec[0][0], 750 * MB / 8);
    atomic_store(&g_fake_avail, 1250 * MB);
    CHECK("written memory stops being counted twice",
          stems_mem_admit(1, 600 * MB, 0, &a, &p) && p == 250 * MB);
    CHECK("the reservation of a deck ends with its load", (stems_mem_done(0), stems_mem_done(1),
                                                           stems_mem_admit(2, 800 * MB, 0, &a, &p)));
    CHECK("nothing else is reserved then", p == 0);
    stems_mem_done(2);

    /* a deck's counts from its previous track are not this load's progress */
    atomic_store(&g_fake_avail, 2000 * MB);
    atomic_store(&g_stem_dec[3][0], 500 * MB / 8);
    CHECK("admitted", stems_mem_admit(3, 1000 * MB, 0, &a, &p));
    CHECK("a previous track's counts do not hide a new load's need",
          !stems_mem_admit(1, 1000 * MB, 0, &a, &p) && p == 1000 * MB);
    stems_mem_done(3);
    for (int d = 0; d < NDECK; d++)
        for (int k = 0; k < NSTEM; k++)
            atomic_store(&g_stem_dec[d][k], 0);
}

/* Four loaders admit, "write" their set in steps (MemAvailable falls as they do, and only then
 * does the count of written frames rise, as in the decode) and finish. Only one set fits above
 * the floor, and a holder waits for someone to be refused, so loads really overlap: whatever the
 * interleaving, what they have admitted never exceeds that memory. */
#define MT_TOTAL (900 * MB)
static _Atomic long long g_mt_admitted;
static _Atomic int g_mt_over, g_mt_ok, g_mt_refused;
static void *mt_loader(void *arg) {
    int d = (int) (long) arg;
    for (int r = 0; r < 300; r++) {
        long long need = 400 * MB, a, p;
        if (!stems_mem_admit(d, need, 0, &a, &p)) {
            atomic_fetch_add(&g_mt_refused, 1);
            sched_yield();
            continue;
        }
        atomic_fetch_add(&g_mt_ok, 1);
        if (atomic_fetch_add(&g_mt_admitted, need) + need > MT_TOTAL - 400 * MB)
            atomic_fetch_add(&g_mt_over, 1);
        int seen = atomic_load(&g_mt_refused);
        for (int w = 0; w < 200 && atomic_load(&g_mt_refused) == seen; w++)
            usleep(50);
        for (long long done = 0; done < need; done += 100 * MB) {
            atomic_fetch_sub(&g_fake_avail, 100 * MB);
            atomic_store(&g_stem_dec[d][0], (done + 100 * MB) / 8);
        }
        atomic_fetch_sub(&g_mt_admitted, need);
        stems_mem_done(d);
        atomic_fetch_add(&g_fake_avail, need);
    }
    return NULL;
}
static void test_admit_threads(void) {
    atomic_store(&g_fake_avail, MT_TOTAL);
    pthread_t th[NDECK];
    for (long d = 0; d < NDECK; d++)
        pthread_create(&th[d], NULL, mt_loader, (void *) d);
    for (int d = 0; d < NDECK; d++)
        pthread_join(th[d], NULL);
    printf("      %d admitted, %d refused\n", atomic_load(&g_mt_ok), atomic_load(&g_mt_refused));
    CHECK("loaders at once: both outcomes happened",
          atomic_load(&g_mt_ok) > 0 && atomic_load(&g_mt_refused) > 0);
    CHECK("loaders at once: never more admitted than fits above the floor",
          atomic_load(&g_mt_over) == 0);
    for (int d = 0; d < NDECK; d++)
        atomic_store(&g_stem_dec[d][0], 0);
}

static int stereo_ok(const float *b, long n, int ch) {
    for (long i = 0; i < n; i++) {
        float l = sample_at(0, i), r = ch == 1 ? l : sample_at(1, i);
        if (b[i * 2] != l || b[i * 2 + 1] != r)
            return 0;
    }
    return 1;
}
static void test_decode(void) {
    static const int layouts[] = {1, 2, 3, 6};
    long total = PROG_CHUNK * 2 + 123;
    for (unsigned u = 0; u < sizeof layouts / sizeof layouts[0]; u++) {
        int ch = layouts[u];
        char name[96];
        stb_vorbis *v = stream_new(ch, AZ_EP147_RATE, total);
        atomic_store(&g_stem_present[0], 1u);
        decode_stem_progressive(0, 0, "x", v);
        snprintf(name, sizeof name, "%d channel(s): every frame, interleaved stereo", ch);
        CHECK(name, atomic_load(&g_stem_dec[0][0]) == total && g_dk[0].stem[0] &&
                        stereo_ok(g_dk[0].stem[0], total, ch));
        snprintf(name, sizeof name, "%d channel(s): the stream closed once", ch);
        CHECK(name, v->closed == 1);
        CHECK("buffer size recorded", g_dk[0].stem_bytes[0] == (size_t) total * 2 * sizeof(float));
        free((void *) g_dk[0].stem[0]);
        g_dk[0].stem[0] = NULL;
        stream_free(v);
    }
    /* the stream ends before its header's length */
    stb_vorbis *v = stream_new(2, AZ_EP147_RATE, total);
    v->total = PROG_CHUNK + 5;
    stb_vorbis_info in = stb_vorbis_get_info(v);
    (void) in;
    decode_stem_progressive(0, 0, "x", v);
    CHECK("short stream: the frames it has", atomic_load(&g_stem_dec[0][0]) == PROG_CHUNK + 5);
    free((void *) g_dk[0].stem[0]);
    g_dk[0].stem[0] = NULL;
    stream_free(v);

    /* a replaced track stops after the chunk in hand */
    v = stream_new(2, AZ_EP147_RATE, total);
    atomic_store(&g_load_stale[0], 1);
    decode_stem_progressive(0, 0, "x", v);
    atomic_store(&g_load_stale[0], 0);
    CHECK("stale load: stopped after one chunk, stream closed",
          atomic_load(&g_stem_dec[0][0]) == PROG_CHUNK && v->closed == 1);
    free((void *) g_dk[0].stem[0]);
    g_dk[0].stem[0] = NULL;
    stream_free(v);

    /* another rate: the whole-file resampler, the stream closed first */
    v = stream_new(2, 48000, total);
    long rs = g_resampled;
    decode_stem_progressive(0, 0, "x", v);
    CHECK("other rate: the resampling decoder, stream closed",
          g_resampled == rs + 1 && v->closed == 1 && atomic_load(&g_stem_dec[0][0]) == 8);
    free((void *) g_dk[0].stem[0]);
    g_dk[0].stem[0] = NULL;
    stream_free(v);

    /* a stream that did not open */
    atomic_store(&g_stem_present[0], 0xFu);
    decode_stem_progressive(0, 2, "x", NULL);
    CHECK("no stream: its slot drops out, the others stay",
          atomic_load(&g_stem_present[0]) == (0xFu & ~4u) && !g_dk[0].stem[2]);
}

static void test_read_layouts(void) {
    long total = 5000;
    float *b = malloc((size_t) total * 8 * sizeof(float));
    struct {
        int file, asked;
    } t[] = {{1, 1}, {1, 2}, {2, 1}, {2, 2}, {2, 3}, {3, 2}, {6, 2}};
    for (unsigned u = 0; u < sizeof t / sizeof t[0]; u++) {
        stb_vorbis *v = stream_new(t[u].file, AZ_EP147_RATE, total);
        /* ask in pieces, so the read starts and ends in the middle of decoded blocks */
        long got = 0;
        int piece = 701;
        for (;;) {
            int n = stb_vorbis_get_samples_float_interleaved(v, t[u].asked, b + got * t[u].asked,
                                                             piece * t[u].asked);
            got += n;
            if (n < piece)
                break;
        }
        int bad = got != total;
        for (long i = 0; !bad && i < total; i++)
            for (int c = 0; c < t[u].asked; c++) {
                float want = c < t[u].file ? sample_at(c, i) : 0.f;
                if (b[i * t[u].asked + c] != want)
                    bad = 1;
            }
        char name[64];
        snprintf(name, sizeof name, "read: %d file channels as %d", t[u].file, t[u].asked);
        CHECK(name, !bad);
        stream_free(v);
    }
    free(b);
}

int main(void) {
    ut_suite = "stem load";
    test_admit();
    test_admit_threads();
    test_decode();
    test_read_layouts();
    return ut_summary();
}
