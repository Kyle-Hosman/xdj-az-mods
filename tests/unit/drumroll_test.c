/* SPDX-License-Identifier: GPL-3.0-only */
/* Drum Roll loading (guest/azstem/drumroll.c, #included whole by run.sh through
 * DRUM_INC, so the suite runs the production text):
 *
 *   wav      drum_decode_wav accepts PCM (1) and IEEE float (3), plain or as
 *            WAVE_FORMAT_EXTENSIBLE, at the bit depths the decoder reads, and refuses
 *            everything else (A-law, mu-law, ADPCM, odd depths) as a missing slot. Each
 *            sample is capped at DRUM_MAX_SECONDS.
 *   service  drum_service finds DRUM ROLLS again after the stick is removed or replaced
 *            (a path resolved once per EP147 run would miss a replaced stick).
 *   swap     drum_load_bank never frees a bank while an audio block is inside the mix.
 *   mix      with Beat Sync on and a grid, a roll on a paused or empty deck (frozen or
 *            unknown playhead) fires at once and repeats at its BPM, instead of waiting
 *            forever for a grid boundary the playhead will never cross; on a PLAYING deck
 *            the first hit still waits for the boundary.
 *
 * Each WAV case runs in a child process, so a decoder that crashes (an ADPCM
 * divide by zero, say) reports a FAIL line instead of ending the suite.
 *
 * argv[1]: scratch directory. */
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "check.h"

/* ---- what drumroll.c uses from the rest of azstem ---- */
#define NDECK 4
#define EP_2754518 0x2754518
#define EP_2798328 0x2798328
#define EP_22043f0 0
static _Atomic(void *) g_ls_self[NDECK];
static _Atomic unsigned g_deck_trackgen[NDECK];
/* stem_playback.c's LoadState bracket (tested in lifetime_test.c / meowgrid_test.c): here the
 * test's own g_ls_self is the LoadState, live for the whole test */
static uintptr_t ls_enter(int deck) { return (uintptr_t) atomic_load(&g_ls_self[deck]); }
static void ls_leave(int deck) { (void) deck; }
static void logln(const char *fmt, ...) {
    if (!getenv("UT_VERBOSE"))
        return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}
static int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
/* azstem.c's development-input opener, as the release build defines it: the drum test and
 * the BPM override (azstem.drumtest / .drumbpm) are never read */
#define dev_input_open(path) ((FILE *) NULL)
/* ... and its development-flag test: the hit log (azstem.drumhits) is never on */
#define dev_flag(path) 0
static double eq_knob_gain(int v) {
    (void) v;
    return 1.0;
}
static float *decode_ogg_stereo_44100(const char *path, long *out_frames, long max_frames) {
    (void) path;
    (void) max_frames;
    *out_frames = 0;
    return NULL;
}

/* free() as drumroll.c calls it: records the in-flight audio block count at the moment a
 * watched pointer is released. */
static void *ut_watch;
static int ut_watch_freed, ut_watch_inflight = -1;
/* the exports drumroll.c calls before defining them (azstem.c declares them) */
void azstem_drum_gate(int side, int on);
void azstem_drum_select(int side, int slot);
void azstem_drum_rate(int side, int idx);
void azstem_drum_touch(int side, int on);
void azstem_drum_bpm(int side, float bpm);
static void ut_free(void *p);
#define free(p) ut_free(p)
/* nanosleep() as drumroll.c calls it (drum_rt_quiesce's wait only): counted, so the swap test's
 * fake audio block can tell that the swap is waiting for it instead of guessing with a timer. */
static _Atomic long ut_dr_sleeps;
static int ut_nanosleep(const struct timespec *a, struct timespec *b);
#define nanosleep(a, b) ut_nanosleep(a, b)
#include DRUM_INC
#undef nanosleep
static int ut_nanosleep(const struct timespec *a, struct timespec *b) {
    atomic_fetch_add(&ut_dr_sleeps, 1);
    return nanosleep(a, b);
}
#undef free
static void ut_free(void *p) {
    if (p && p == ut_watch) {
        __atomic_store_n(&ut_watch_freed, 1, __ATOMIC_SEQ_CST);
        ut_watch_inflight = atomic_load(&g_drum_rt_inflight);
    }
    free(p);
}

/* ---- WAV writer ---- */
static const char *dir;
static void le16(FILE *f, unsigned v) {
    fputc(v & 255, f);
    fputc((v >> 8) & 255, f);
}
static void le32(FILE *f, unsigned v) {
    le16(f, v & 0xffff);
    le16(f, v >> 16);
}
static const unsigned char KS_TAIL[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                          0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
typedef struct {
    unsigned tag, chans, rate, bits;
    unsigned ext_sub;     /* nonzero: WAVE_FORMAT_EXTENSIBLE with this subformat */
    int ext_badguid;      /* EXTENSIBLE with a GUID tail that is not KSDATAFORMAT's */
    int ext_cb;           /* cbSize to write (default 22) */
    unsigned fmt_len;     /* fmt chunk size override (0 = natural) */
    long frames;          /* frames of data */
    long claim_frames;    /* data size the header claims (0 = frames) */
    int data_first;       /* write data before fmt */
} wavspec;
static void write_wav(const char *path, wavspec w) {
    FILE *f = fopen(path, "wb");
    unsigned bpf = (w.bits ? (w.bits + 7) / 8 : 1) * (w.chans ? w.chans : 1);
    if (w.tag == 2)
        bpf = 1; /* ADPCM: arbitrary bytes */
    unsigned dlen = (unsigned) w.frames * bpf;
    unsigned claim = w.claim_frames ? (unsigned) w.claim_frames * bpf : dlen;
    unsigned flen = w.ext_sub ? 40 : 16;
    if (w.fmt_len)
        flen = w.fmt_len;
    fwrite("RIFF", 1, 4, f);
    le32(f, 4 + 8 + flen + (flen & 1) + 8 + dlen);
    fwrite("WAVE", 1, 4, f);
    fwrite("LIST", 1, 4, f);
    le32(f, 4);
    fwrite("INFO", 1, 4, f); /* a chunk to skip */
    for (int pass = 0; pass < 2; pass++) {
        int is_data = (pass == 0) == (w.data_first != 0);
        if (!is_data) {
            fwrite("fmt ", 1, 4, f);
            le32(f, flen);
            le16(f, w.ext_sub ? 0xFFFE : w.tag);
            le16(f, w.chans);
            le32(f, w.rate);
            le32(f, w.rate * bpf);
            le16(f, bpf);
            le16(f, w.bits);
            unsigned written = 16;
            if (w.ext_sub) {
                le16(f, w.ext_cb ? (unsigned) w.ext_cb : 22);
                le16(f, w.bits);
                le32(f, 0);
                le16(f, w.ext_sub);
                unsigned char t[14];
                memcpy(t, KS_TAIL, 14);
                if (w.ext_badguid)
                    t[13] ^= 0xff;
                fwrite(t, 1, 14, f);
                written = 40;
            }
            while (written < flen) {
                fputc(0, f);
                written++;
            }
            if (flen & 1)
                fputc(0, f); /* pad byte */
        } else {
            fwrite("data", 1, 4, f);
            le32(f, claim);
            int isf = (w.ext_sub ? w.ext_sub : w.tag) == 3;
            if (isf && w.bits == 32) {
                float v = 0.25f;
                for (long i = 0; i < (long) dlen / 4; i++)
                    fwrite(&v, 4, 1, f);
            } else if (isf && w.bits == 64) {
                double v = 0.25;
                for (long i = 0; i < (long) dlen / 8; i++)
                    fwrite(&v, 8, 1, f);
            } else {
                for (long i = 0; i < (long) dlen; i++)
                    fputc((int) ((i * 7 + 3) & 0x7f), f); /* nonzero, below full scale */
            }
        }
    }
    fclose(f);
}

/* Runs one decode in a child. Returns frames (>= 0), -1 for NULL, -2 for a crash. */
static long decode_in_child(const char *path) {
    int fd[2];
    if (pipe(fd))
        return -3;
    pid_t pid = fork();
    if (pid == 0) {
        close(fd[0]);
        long fr = 0;
        float *b = drum_decode_wav(path, &fr);
        long r = b ? fr : -1;
        if (b) {
            int finite = 1;
            for (long i = 0; i < fr * 2; i++)
                if (!isfinite(b[i]) || b[i] > 4.f || b[i] < -4.f)
                    finite = 0;
            if (!finite)
                r = -4;
        }
        if (write(fd[1], &r, sizeof r) != sizeof r)
            _exit(3);
        _exit(0);
    }
    close(fd[1]);
    long r = -2;
    if (read(fd[0], &r, sizeof r) != sizeof r)
        r = -2;
    close(fd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st))
        r = -2;
    return r;
}
static long wav_case(const char *name, wavspec w) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s.wav", dir, name);
    write_wav(p, w);
    long r = decode_in_child(p);
    if (getenv("UT_VERBOSE"))
        printf("      %s -> %ld\n", name, r);
    return r;
}

static void test_wav(void) {
    ut_suite = "drum wav";
    CHECK("PCM 16 stereo 44100 decodes all frames",
          wav_case("pcm16", (wavspec) {1, 2, 44100, 16, .frames = 1000}) == 1000);
    CHECK("PCM 8 mono decodes", wav_case("pcm8", (wavspec) {1, 1, 44100, 8, .frames = 500}) == 500);
    CHECK("PCM 24 decodes", wav_case("pcm24", (wavspec) {1, 2, 44100, 24, .frames = 300}) == 300);
    CHECK("PCM 32 decodes", wav_case("pcm32", (wavspec) {1, 1, 44100, 32, .frames = 300}) == 300);
    CHECK("float 32 decodes", wav_case("f32", (wavspec) {3, 2, 44100, 32, .frames = 300}) == 300);
    CHECK("float 64 decodes", wav_case("f64", (wavspec) {3, 1, 44100, 64, .frames = 300}) == 300);
    CHECK("EXTENSIBLE PCM 24 decodes",
          wav_case("ext24", (wavspec) {1, 2, 44100, 24, .ext_sub = 1, .frames = 300}) == 300);
    CHECK("EXTENSIBLE float 32 decodes",
          wav_case("extf", (wavspec) {3, 2, 44100, 32, .ext_sub = 3, .frames = 300}) == 300);
    CHECK("48 kHz resampled to 44100",
          wav_case("r48", (wavspec) {1, 1, 48000, 16, .frames = 4800}) == 4410);
    CHECK("odd fmt chunk size (17 + pad byte) still finds data",
          wav_case("fmt17", (wavspec) {1, 1, 44100, 16, .fmt_len = 17, .frames = 100}) == 100);
    CHECK("data size claimed past the end: decodes what is there",
          wav_case("short", (wavspec) {1, 1, 44100, 16, .frames = 200, .claim_frames = 900000}) == 200);

    CHECK("A-law (6) refused", wav_case("alaw", (wavspec) {6, 1, 8000, 8, .frames = 800}) == -1);
    CHECK("mu-law (7) refused", wav_case("ulaw", (wavspec) {7, 1, 8000, 8, .frames = 800}) == -1);
    CHECK("IMA ADPCM (0x11, 4-bit) refused, no crash",
          wav_case("adpcm", (wavspec) {0x11, 1, 44100, 4, .frames = 800}) == -1);
    CHECK("MS ADPCM (2, 4-bit) refused, no crash",
          wav_case("msadpcm", (wavspec) {2, 2, 44100, 4, .frames = 800}) == -1);
    CHECK("PCM 4-bit refused, no crash", wav_case("pcm4", (wavspec) {1, 1, 44100, 4, .frames = 100}) == -1);
    CHECK("PCM 12-bit refused (was silence)",
          wav_case("pcm12", (wavspec) {1, 1, 44100, 12, .frames = 100}) == -1);
    CHECK("PCM 20-bit refused", wav_case("pcm20", (wavspec) {1, 2, 44100, 20, .frames = 100}) == -1);
    CHECK("float 16 refused", wav_case("f16", (wavspec) {3, 1, 44100, 16, .frames = 100}) == -1);
    CHECK("EXTENSIBLE with an A-law subformat refused",
          wav_case("extalaw", (wavspec) {6, 1, 44100, 8, .ext_sub = 6, .frames = 100}) == -1);
    CHECK("EXTENSIBLE with a foreign GUID refused",
          wav_case("extguid", (wavspec) {1, 1, 44100, 16, .ext_sub = 1, .ext_badguid = 1, .frames = 100}) == -1);
    CHECK("EXTENSIBLE with cbSize < 22 refused",
          wav_case("extcb", (wavspec) {1, 1, 44100, 16, .ext_sub = 1, .ext_cb = 2, .frames = 100}) == -1);
    CHECK("0 channels refused", wav_case("ch0", (wavspec) {1, 0, 44100, 16, .frames = 100}) == -1);
    CHECK("0 Hz refused", wav_case("sr0", (wavspec) {1, 1, 0, 16, .frames = 100}) == -1);
    CHECK("data before fmt refused",
          wav_case("datafirst", (wavspec) {1, 1, 44100, 16, .frames = 100, .data_first = 1}) == -1);

    long cap = (long) DRUM_MAX_SECONDS * 44100;
    CHECK("30 s at 44100 capped at DRUM_MAX_SECONDS",
          wav_case("long", (wavspec) {1, 1, 44100, 16, .frames = 30L * 44100}) == cap);
    CHECK("20 s at 48000 capped at DRUM_MAX_SECONDS (after resampling)",
          wav_case("long48", (wavspec) {1, 2, 48000, 16, .frames = 20L * 48000}) == cap);
    CHECK("DRUM_MAX_SECONDS is between 2 and 30", DRUM_MAX_SECONDS >= 2 && DRUM_MAX_SECONDS <= 30);
}

/* ---- the stick ---- */
static char stick[1100], drums[1200];
static void sh(const char *fmt, ...) {
    char cmd[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    if (system(cmd) != 0)
        printf("      (command failed: %s)\n", cmd);
}
static void make_stick(long kick_frames, int bank2) {
    char p[1600];
    sh("mkdir -p '%s/Bank 1'", drums);
    snprintf(p, sizeof p, "%s/Bank 1/kick.wav", drums);
    write_wav(p, (wavspec) {1, 1, 44100, 16, .frames = kick_frames});
    if (bank2) {
        sh("mkdir -p '%s/Bank 2'", drums);
        snprintf(p, sizeof p, "%s/Bank 2/snare.wav", drums);
        write_wav(p, (wavspec) {1, 1, 44100, 16, .frames = 777});
    }
}
static int tick_until(int max, int (*cond)(void)) {
    for (int i = 0; i < max; i++) {
        drum_service();
        if (cond())
            return i + 1;
    }
    return 0;
}
static int c_found1(void) { return azstem_drum_available() == 1 && g_drum_smp[0].frames == 1000; }
static int c_swapped(void) { return azstem_drum_available() == 3 && g_drum_smp[0].frames == 2000; }
static int c_gone(void) { return azstem_drum_available() == 0; }
static int c_back(void) { return azstem_drum_available() == 3; }
static int c_never(void) { return 0; }
static int c_kick3000(void) { return g_drum_smp[0].frames == 3000; }

static void test_service(void) {
    ut_suite = "drum service";
    snprintf(stick, sizeof stick, "%s/stick", dir);
    snprintf(drums, sizeof drums, "%s/DRUM ROLLS", stick);
    sh("rm -rf '%s' '%s.old'", stick, stick);
    make_stick(1000, 0);
    setenv("AZSTEM_DRUM_DIR", drums, 1);
    azstem_drum_bank(1); /* Drum Roll on, Bank 1 */
    CHECK("finds DRUM ROLLS and loads Bank 1", tick_until(5, c_found1) > 0);

    /* Swap: a different stick at the same path (new directory, new inode) with a longer
     * kick and a Bank 2. 40 ticks = 8 s of flusher time. */
    sh("mv '%s' '%s.old'", stick, stick);
    make_stick(2000, 1);
    int t = tick_until(40, c_swapped);
    CHECK("a stick swapped at the same path is found again (banks re-scanned)", t > 0);
    CHECK("...and the selected bank is reloaded from the new stick", g_drum_smp[0].frames == 2000);
    CHECK("...within about a second (<= 7 ticks)", t > 0 && t <= 7);

    sh("rm -rf '%s'", stick);
    t = tick_until(10, c_gone);
    CHECK("stick removed: no banks reported", t > 0);
    CHECK("...and the loaded bank stays in memory", g_drum_smp[0].frames == 2000 && g_drum_smp[0].data);

    make_stick(2000, 1);
    t = tick_until(40, c_back);
    CHECK("stick back: found again", t > 0);
    CHECK("unchanged stick: no repeated discovery walk (base kept for 50 ticks)",
          tick_until(50, c_gone) == 0 && atomic_load(&g_drum_have_base) == 1);

    /* Drum Roll off (bank 0): a stick found again decodes nothing (a bank's open files hold up
     * the stick's eject), and turning it on loads the bank from that stick */
    azstem_drum_bank(0);
    sh("rm -rf '%s'", stick);
    tick_until(10, c_gone);
    make_stick(3000, 1);
    t = tick_until(40, c_back);
    tick_until(10, c_never);
    CHECK("Drum Roll off: a stick found again is scanned but nothing is decoded",
          t > 0 && g_drum_smp[0].frames == 2000);
    azstem_drum_bank(1);
    CHECK("... and turned on, Bank 1 is loaded from that stick (CONTROL)", tick_until(10, c_kick3000) > 0);
}

/* ---- the swap ----
 * The fake audio block stays inside the mix until the swap has reached a decided state, never
 * for a fixed time, so neither outcome depends on how long loading a bank takes:
 *   HOLD_UNTIL_WAIT  until the watched bank was freed (a failure: freed while inside) or the
 *                    swap sleeps in drum_rt_quiesce with the block inside (it waits, as it must);
 *   HOLD_UNTIL_DONE  until drum_load_bank returned (the swap had to give up, or freed early).
 * 10 s bounds a hang either way. */
enum {
    HOLD_UNTIL_WAIT,
    HOLD_UNTIL_DONE
};
static _Atomic int hold_mode, holding, swap_done;
static _Atomic long hold_sleeps0;
static long dr_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}
static void *fake_audio_block(void *arg) {
    (void) arg;
    atomic_fetch_add(&g_drum_rt_inflight, 1); /* inside drum_mix_block */
    atomic_store(&hold_sleeps0, atomic_load(&ut_dr_sleeps));
    atomic_store(&holding, 1);
    long t_end = dr_now_ms() + 10000;
    while (dr_now_ms() < t_end) {
        if (atomic_load(&hold_mode) == HOLD_UNTIL_WAIT
                ? __atomic_load_n(&ut_watch_freed, __ATOMIC_SEQ_CST) ||
                      atomic_load(&ut_dr_sleeps) != atomic_load(&hold_sleeps0)
                : atomic_load(&swap_done))
            break;
        struct timespec ts = {0, 100000};
        nanosleep(&ts, NULL);
    }
    atomic_fetch_sub(&g_drum_rt_inflight, 1);
    return NULL;
}
static void test_swap(void) {
    ut_suite = "drum swap";
    /* Bank 1 is loaded (kick, 2000 frames) by test_service. */
    void *old = g_drum_smp[0].data;
    CHECK("setup: a bank is loaded", old != NULL && atomic_load(&g_drum_loaded) == 1);

    /* An audio block inside the mix while Bank 2 loads: it leaves once the swap waits for it. */
    ut_watch = old;
    ut_watch_freed = 0;
    ut_watch_inflight = -1;
    atomic_store(&hold_mode, HOLD_UNTIL_WAIT);
    atomic_store(&holding, 0);
    atomic_store(&swap_done, 0);
    pthread_t th;
    pthread_create(&th, NULL, fake_audio_block, NULL);
    while (!atomic_load(&holding));
    drum_load_bank(2);
    atomic_store(&swap_done, 1);
    pthread_join(th, NULL);
    CHECK("the replaced bank is freed", ut_watch_freed == 1);
    CHECK("...only after the audio block left the mix", ut_watch_inflight == 0);
    CHECK("Bank 2 is active", atomic_load(&g_drum_bank) == 2 && g_drum_smp[1].frames == 777);

    /* A block that never leaves while the swap runs: after DRUM_QUIESCE_MS the swap is
     * abandoned and nothing the audio thread can see is freed. */
    void *cur = g_drum_smp[1].data;
    ut_watch = cur;
    ut_watch_freed = 0;
    atomic_store(&hold_mode, HOLD_UNTIL_DONE);
    atomic_store(&holding, 0);
    atomic_store(&swap_done, 0);
    pthread_create(&th, NULL, fake_audio_block, NULL);
    while (!atomic_load(&holding));
    drum_load_bank(1);
    atomic_store(&swap_done, 1);
    pthread_join(th, NULL);
    CHECK("a swap that cannot quiesce frees nothing in use", ut_watch_freed == 0);
    CHECK("...keeps the loaded bank and reopens the gate",
          atomic_load(&g_drum_bank) == 2 && g_drum_smp[1].data == cur && atomic_load(&g_drum_loaded) == 1);
    ut_watch = NULL;
}

/* ---- the mix: Beat Sync on a paused, empty and playing deck ---- */
#define MIX_NS 512
static float mix_l[MIX_NS], mix_r[MIX_NS];
/* One audio block for `side` at source position `src`; returns the block's peak. */
static float mix_block(int side, long long src) {
    memset(mix_l, 0, sizeof mix_l);
    memset(mix_r, 0, sizeof mix_r);
    float *ch[2] = {mix_l, mix_r};
    drum_mix_block(side, ch, 2, MIX_NS, src);
    float pk = 0.f;
    for (int i = 0; i < MIX_NS; i++)
        if (fabsf(mix_l[i]) > pk)
            pk = fabsf(mix_l[i]);
    return pk;
}
static void mix_press(int side, int rate) {
    azstem_drum_select(side, 0);
    azstem_drum_rate(side, rate);
    azstem_drum_gate(side, 1); /* gate clears touch: gate first */
    azstem_drum_touch(side, 1);
}
static void test_mix(void) {
    ut_suite = "drum mix";
    /* A known sample in slot 0: 2000 frames at 0.5, so a hit is audible for 4 blocks. */
    static float smp[2000 * 2];
    for (int i = 0; i < 2000 * 2; i++)
        smp[i] = 0.5f;
    atomic_store(&g_drum_loaded, 0);
    g_drum_smp[0].data = smp;
    g_drum_smp[0].frames = 2000;
    atomic_fetch_add(&g_drum_gen, 1);
    atomic_store(&g_drum_loaded, 1);
    azstem_drum_sync(1);
    azstem_drum_tone(512, 512); /* unity */
    const double beat = 22050.0;  /* 120 BPM grid from source sample 0 */
    for (int s = 0; s < DRUM_SIDES; s++) {
        azstem_drum_bpm(s, 120.f);
        drum_grid_publish(s, 1, 0, beat);
    }
    /* rate 2 = 1/4 beat at 120 BPM: a hit every 5513 output frames */
    const long period = 5513;

    /* side 0: track loaded, PAUSED for a while (frozen playhead), then pressed. */
    const long long frozen = 100000;
    for (int b = 0; b < 20; b++)
        mix_block(0, frozen);
    mix_press(0, 2);
    float pk[16];
    CHECK("setup: 1/4 beat at 120 BPM is 5513 frames", drum_period_frames(0) == period);
    for (int b = 0; b < 16; b++)
        pk[b] = mix_block(0, frozen);
    CHECK("paused deck: the first hit sounds at once", pk[0] > 0.4f);
    int gap_silent = 1;
    for (int b = 5; b < (int) (period / MIX_NS); b++)
        if (pk[b] != 0.f)
            gap_silent = 0;
    CHECK("paused deck: silent between hits (one-shot ended)", gap_silent);
    CHECK("paused deck: the roll repeats one period later, at the deck's BPM",
          pk[period / MIX_NS] > 0.4f);
    azstem_drum_gate(0, 0);
    for (int b = 0; b < 8; b++)
        mix_block(0, frozen);

    /* side 0 again: playing, paused, pressed straight after the pause at the slowest rate
     * (next boundary ~a beat away): the press may wait while the playhead could still be
     * moving, but not past the stall threshold. */
    long long pos = 22050 * 8 + 100; /* just past a beat */
    for (int b = 0; b < 8; b++, pos += MIX_NS)
        mix_block(0, pos);
    pos -= MIX_NS; /* the playhead stops where the last block left it */
    mix_press(0, 0);
    int first = -1;
    for (int b = 0; b < 40 && first < 0; b++)
        if (mix_block(0, pos) > 0.4f)
            first = b;
    CHECK("just paused: the held hit fires within the stall window, not at the far boundary",
          first >= 0 && (long) first * MIX_NS <= DRUM_SYNC_STALL + MIX_NS);
    azstem_drum_gate(0, 0);

    /* side 1: EJECTED deck. The previous track's grid is still published (the eject does
     * not change the track generation) and the playhead is unknown (-1). */
    for (int b = 0; b < 4; b++)
        mix_block(1, -1);
    mix_press(1, 2);
    CHECK("empty deck, stale grid: the first hit sounds at once", mix_block(1, -1) > 0.4f);
    azstem_drum_gate(1, 0);

    /* side 2: PLAYING deck. EP147 refreshes the playhead every other block, so the
     * reported position is the true one on even blocks and the previous one on odd
     * blocks; the press lands on a stale (odd) block, as half of real presses do. The
     * first hit must wait for the next grid boundary. */
    long long t0 = 22050 * 4 + 100; /* true position at block 0 */
    int blk = 0;
#define SIDE2_SRC(b) (t0 + (long long) MIX_NS * ((b) - ((b) & 1)))
    for (; blk < 5; blk++)
        mix_block(2, SIDE2_SRC(blk));
    double sub = beat * 0.25;
    long long tpress = t0 + (long long) MIX_NS * blk; /* blk is odd: a stale block */
    double ph = fmod((double) tpress, sub);
    int want_blk = (int) ((sub - ph) / MIX_NS); /* blocks from the press to the boundary */
    mix_press(2, 2);
    int hit = -1;
    for (int b = 0; b < 16; b++, blk++)
        if (mix_block(2, SIDE2_SRC(blk)) > 0.f && hit < 0)
            hit = b;
#undef SIDE2_SRC
    CHECK("playing deck: setup: the press is on a stale block, the boundary blocks away",
          (blk - 16) % 2 == 1 && want_blk > 1);
    CHECK("playing deck: the first hit is not fired at the press (it waits for the grid)",
          hit != 0);
    CHECK("playing deck: ...and lands in the block holding the next grid boundary",
          hit == want_blk);
    azstem_drum_gate(2, 0);

    /* side 3: no grid at all (never loaded): free-running, fires at once. */
    drum_grid_publish(3, 0, 0, 0.0);
    for (int b = 0; b < 4; b++)
        mix_block(3, -1);
    mix_press(3, 2);
    CHECK("no grid: the first hit sounds at once", mix_block(3, -1) > 0.4f);
    float early = 0.f; /* after the fade-in: the tone glide starts at unity on every side */
    for (int i = DRUM_FADE_IN; i < DRUM_FADE_IN + 32; i++)
        early = fabsf(mix_l[i]) > early ? fabsf(mix_l[i]) : early;
    CHECK("deck 4's first hit is at full level from its first samples (its tone glide starts at unity)",
          early > 0.45f);
    azstem_drum_gate(3, 0);

    /* side 2: a steady roll on a PLAYING deck whose playhead is refreshed every other block and
     * whose offset from the output moves by 200 frames between refreshes (on time, early, on
     * time, late; inside the late tolerance):
     * one hit per grid boundary, never two close
     * together (a flam), none skipped. Hits are counted from the mix's hit record, which
     * only the development build has. */
#ifdef DRUM_HITN
    {
        long long tpos = 22050 * 40; /* on a beat */
        long long rep = tpos;
        int hits = 0, minsep = 1 << 30, last = -1;
        atomic_store(&g_drum_hitlog, 1);
        mix_press(2, 2);
        const int nblk = 400;
        for (int b = 0; b < nblk; b++, tpos += MIX_NS) {
            if (!(b & 1))
                rep = tpos + ((b & 6) == 2 ? -200 : (b & 6) == 6 ? 200
                                                                 : 0);
            unsigned w0 = atomic_load(&g_drum_hitw);
            mix_block(2, rep);
            unsigned nw = atomic_load(&g_drum_hitw) - w0;
            if (nw > 1)
                minsep = 0;
            if (nw) {
                if (last >= 0 && (b - last) * MIX_NS < minsep)
                    minsep = (b - last) * MIX_NS;
                last = b;
                hits += (int) nw;
            }
        }
        azstem_drum_gate(2, 0);
        atomic_store(&g_drum_hitlog, 0);
        double span = (double) nblk * MIX_NS / sub; /* boundaries crossed while held */
        printf("      steady roll: %d hits over %.1f grid boundaries, closest %d frames apart\n", hits, span, minsep);
        CHECK("steady roll: one hit per grid boundary (none doubled, none skipped)",
              hits >= (int) span - 1 && hits <= (int) span + 1);
        CHECK("steady roll: never two hits closer than half a subdivision (no flam)", minsep > (int) (sub / 2));
    }
#else
    printf("SKIP  drumroll: steady roll (the hit record is development-only)\n");
#endif
}

/* ---- an empty deck following the master deck ----
 * Side 0 is the master: playing, with a grid. Side 1 is empty (playhead -1, no grid of its
 * own) and follows side 0 (azstem_drum_follow, which azmod sends an empty deck). Each
 * "callback" renders the master's block first, then the follower's, as the decks are rendered
 * in turn; the follower reads the playhead the master's block just published. */
static void test_follow(void) {
    ut_suite = "drum follow";
    const double beat = 22050.0, sub = beat * 0.25; /* 120 BPM grid, rate 2 = 1/4 beat */
    azstem_drum_sync(1);
    azstem_drum_bpm(0, 120.f);
    azstem_drum_bpm(1, 120.f);
    drum_grid_publish(0, 1, 0, beat);
    drum_grid_publish(1, 0, 0, 0.0); /* the empty deck has no grid of its own */
    azstem_drum_gate(0, 0);
    azstem_drum_gate(1, 0);

    azstem_drum_follow(1, 1);
    CHECK("a deck told to follow itself follows nobody", atomic_load(&g_drum_follow[1]) == -1);
    azstem_drum_follow(1, 7);
    CHECK("a lead out of range follows nobody", atomic_load(&g_drum_follow[1]) == -1);
    azstem_drum_follow(1, 0);
    CHECK("an empty deck follows the master", atomic_load(&g_drum_follow[1]) == 0);

    /* the master playing, the empty deck pressed off the beat. EP147 refreshes the master's
     * playhead every other block, so it reports the true position on even blocks and the
     * previous one on odd blocks; the press lands on a stale (odd) block, where only the
     * master's moving playhead says to wait for its grid. */
    long long pos = (long long) beat * 6 + 700; /* true position */
    int blk = 0;
#define MASTER_SRC(p, b) ((p) - ((b) & 1 ? MIX_NS : 0))
    for (; blk < 7; blk++, pos += MIX_NS) {
        mix_block(0, MASTER_SRC(pos, blk));
        mix_block(1, -1);
    }
    mix_press(1, 2);
    double ph = fmod((double) pos, sub);
    int want = (int) ((sub - ph) / MIX_NS); /* blocks from the press to the master's boundary */
    int hit = -1, second = -1;
    CHECK("follow: setup: the press lands on a stale master block", blk & 1);
    for (int b = 0; b < 40; b++, blk++, pos += MIX_NS) {
        mix_block(0, MASTER_SRC(pos, blk));
        if (mix_block(1, -1) > 0.4f) {
            if (hit < 0)
                hit = b;
            else if (second < 0 && b > hit + 4)
                second = b;
        }
    }
    CHECK("follow: setup: the press is blocks away from the master's next boundary", want > 1);
    CHECK("empty deck following the master: the first hit is not fired at the press", hit != 0);
    CHECK("...and lands in the block holding the master's next grid boundary", hit == want);
    CHECK("...and the next one a subdivision later, on the master's grid",
          second == (int) ((sub * 2 - ph) / MIX_NS));
#undef MASTER_SRC
    azstem_drum_gate(1, 0);
    for (int b = 0; b < 8; b++, pos += MIX_NS) {
        mix_block(0, pos);
        mix_block(1, -1);
    }

    /* the master paused: nothing to lock to, the follower rolls free at once */
    for (int b = 0; b < 20; b++) {
        mix_block(0, pos);
        mix_block(1, -1);
    }
    mix_press(1, 2);
    mix_block(0, pos);
    CHECK("master paused: the empty deck's first hit sounds at once", mix_block(1, -1) > 0.4f);
    azstem_drum_gate(1, 0);

    /* follow cleared (a track loaded on it, or no master): its own grid, here none */
    azstem_drum_follow(1, -1);
    for (int b = 0; b < 8; b++, pos += MIX_NS) {
        mix_block(0, pos);
        mix_block(1, -1);
    }
    mix_press(1, 2);
    mix_block(0, pos);
    CHECK("follow cleared: back to its own (no) grid, the first hit at once",
          mix_block(1, -1) > 0.4f);
    azstem_drum_gate(1, 0);
    for (int b = 0; b < 8; b++)
        mix_block(1, -1);

    /* the master's blocks stop running while the follower waits for its boundary: the
     * follower sees the lead's playhead stand still and, after the stall window, fires */
    azstem_drum_follow(1, 0);
    for (int b = 0; b < 8; b++, pos += MIX_NS) {
        mix_block(0, pos);
        mix_block(1, -1);
    }
    mix_press(1, 2);
    int fired = -1;
    for (int b = 0; b < 40 && fired < 0; b++)
        if (mix_block(1, -1) > 0.4f)
            fired = b;
    CHECK("master's blocks stopped: setup: the press waited for the master's grid", fired != 0);
    CHECK("...and the first hit sounds within the stall window, never hangs silent",
          fired > 0 && (long) fired * MIX_NS <= DRUM_SYNC_STALL + MIX_NS);
    azstem_drum_gate(1, 0);

    /* follow set on a master that moved, then paused, while nobody followed it: the
     * follower's first reading of a new lead (one block before the press) is no evidence
     * of motion, so the press fires at once instead of waiting out the stall window */
    azstem_drum_follow(1, -1);
    for (int b = 0; b < 4; b++, pos += MIX_NS) {
        mix_block(0, pos);
        mix_block(1, -1);
    }
    for (int b = 0; b < 20; b++) {
        mix_block(0, pos);
        mix_block(1, -1);
    }
    azstem_drum_follow(1, 0);
    mix_block(0, pos);
    mix_block(1, -1);
    mix_press(1, 2);
    mix_block(0, pos);
    CHECK("follow set on a paused master, pressed at once: the first hit sounds at once",
          mix_block(1, -1) > 0.4f);
    azstem_drum_gate(1, 0);
    azstem_drum_follow(1, -1);
    for (int b = 0; b < 8; b++)
        mix_block(1, -1);
}

/* ---- the native grid: its sample rate measured from its own tempo field ----
 * Fake EP147 objects in ordinary memory: a LoadState whose +0x180 slot holds an
 * UpdatableSharedObject, whose +0x28 slot holds the Content, each with a meow count (+0x0c)
 * and guard (+0x10), as drum_grid_native expects (its reference protocol is meowgrid_test's). */
static unsigned char *g_mem;
#define G_LS ((uintptr_t) g_mem + 0x1000UL)
#define G_USO ((uintptr_t) g_mem + 0x2000UL)
#define G_CONTENT ((uintptr_t) g_mem + 0x3000UL)
#define G_CELLS ((uintptr_t) g_mem + 0x4010UL)
static void g_put(int fd, uintptr_t a, const void *p, size_t n) {
    (void) fd;
    memcpy((void *) a, p, n);
}
static void g_q(int fd, uintptr_t a, unsigned long long v) { g_put(fd, a, &v, 8); }
static void g_destroy(void *owner, void *o) {
    (void) owner;
    (void) o;
}
/* n beats: beat i at anchor + i * spacing samples, tempo field bpm (one value for all). */
static void g_write_grid(int fd, int n, double anchor, double spacing, double bpm) {
    unsigned char z[16];
    int one = 1;
    g_q(fd, G_LS + 0x180, G_USO);
    g_q(fd, G_USO, EP_2754518);
    g_put(fd, G_USO + 0x0c, &one, 4);
    g_q(fd, G_USO + 0x10, 0x52434f58UL);
    g_q(fd, G_USO + 0x28, G_CONTENT);
    g_q(fd, G_CONTENT, EP_2798328);
    g_put(fd, G_CONTENT + 0x0c, &one, 4);
    g_q(fd, G_CONTENT + 0x10, 0x52434f58UL);
    g_q(fd, G_CONTENT + 0x28, G_CELLS);
    int32_t nn = n;
    g_put(fd, G_CONTENT + 0x30, &nn, 4);
    memset(z, 0xAF, 16);
    g_put(fd, G_CELLS - 16, z, 16);
    for (int i = 0; i < n; i++) {
        long long pos = (long long) (anchor + spacing * i + 0.5);
        g_put(fd, G_CELLS + (unsigned long) i * 16, &pos, 8);
        g_put(fd, G_CELLS + (unsigned long) i * 16 + 8, &bpm, 8);
    }
    memset(z, 0xEF, 16);
    g_put(fd, G_CELLS + (unsigned long) n * 16, z, 16);
}
static void test_grid_rate(void) {
    ut_suite = "drum grid rate";
    g_mem = aligned_alloc(16, 0x10000);
    memset(g_mem, 0, 0x10000);
    int fd = -1;
    g_meow_destroy_fn = g_destroy;
    atomic_store(&g_ls_self[0], (void *) G_LS);
    long long anchor = -1;
    double beat = -1.0;

    /* 44.1 kHz grid (the rate of every grid seen on the hardware): published as it is. */
    g_write_grid(fd, 200, 1000.0, 22050.0, 120.0);
    int ok = drum_grid_native(0, &anchor, &beat);
    CHECK("44.1 kHz grid at 120 BPM: accepted", ok);
    CHECK("...anchor and beat unchanged (1000, 22050)", anchor == 1000 && fabs(beat - 22050.0) < 0.01);

    /* 48 kHz grid (a producer handed 48000): converted to the playhead's 44.1 kHz base. */
    g_write_grid(fd, 200, 2000.0, 24000.0, 120.0);
    anchor = -1;
    beat = -1.0;
    ok = drum_grid_native(0, &anchor, &beat);
    CHECK("48 kHz grid at 120 BPM: accepted", ok);
    CHECK("...beat is 22050 playhead samples (120 BPM at 44.1 kHz), not 24000",
          fabs(beat - 22050.0) < 0.01);
    CHECK("...anchor 2000 @48k is 1838 @44.1k", anchor == 1838);

    /* 96 kHz, a 128 BPM grid: a known rate, converted too. */
    g_write_grid(fd, 100, 0.0, 96000.0 * 60.0 / 128.0, 128.0);
    ok = drum_grid_native(0, &anchor, &beat);
    CHECK("96 kHz grid at 128 BPM: beat = 20671.875 playhead samples",
          ok && fabs(beat - 44100.0 * 60.0 / 128.0) < 0.01);

    /* Spacing and tempo field disagree (44.1 kHz spacing, 100 BPM field: 36750 Hz). */
    g_write_grid(fd, 200, 0.0, 22050.0, 100.0);
    CHECK("tempo field and spacing disagree: refused (UNKNOWN, free-run)",
          drum_grid_native(0, &anchor, &beat) == 0);
    /* No tempo at all. */
    g_write_grid(fd, 200, 0.0, 22050.0, 0.0);
    CHECK("tempo field 0: refused", drum_grid_native(0, &anchor, &beat) == 0);
    double nan = 0.0 / 0.0;
    g_write_grid(fd, 200, 0.0, 22050.0, nan);
    CHECK("tempo field NaN: refused", drum_grid_native(0, &anchor, &beat) == 0);

    atomic_store(&g_ls_self[0], NULL);
    g_meow_destroy_fn = NULL;
    free(g_mem);
}

/* Beat Sync's grid is published by the flusher while the audio thread reads it (seqlock). Two
 * grids A and B are published alternately as fast as possible; every answer the reader accepts
 * must be A's or B's, never one computed from A's anchor with B's beat (or the reverse). The
 * mixed answers are computed first and must differ from A's and B's, or the probe could not see
 * a tear. Runs for GRID_MS. This exercises the real code under load; it is not the control for
 * the fences (a fence-less copy tears only when the CPU happens to reorder inside the window):
 * seqlock-lint.sh checks the ordering in the source, and run.sh runs it on the fence-less copy. */
#ifndef GRID_MS
#define GRID_MS 1500
#endif
static const long long GA_ANCHOR = 0, GB_ANCHOR = 333;
static const double GA_BEAT = 1000.0, GB_BEAT = 7919.0;
#define GRID_POS 123457LL
#define GRID_PERIOD 1000000L
static atomic_int g_grid_stop;
static void *grid_writer(void *a) {
    (void) a;
    for (unsigned long i = 0; !atomic_load_explicit(&g_grid_stop, memory_order_relaxed); i++) {
        if (i & 1)
            drum_grid_publish(0, 1, GB_ANCHOR, GB_BEAT);
        else
            drum_grid_publish(0, 1, GA_ANCHOR, GA_BEAT);
    }
    return NULL;
}
static long grid_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}
static void test_grid(void) {
    ut_suite = "drum grid";
    atomic_store(&g_drum_sync_on, 1);
    atomic_store(&g_drum_sync_off, 0);
    atomic_store(&g_drum_rate[0], 0);
    long fa, fb, fx, fy;
    drum_grid_publish(0, 1, GA_ANCHOR, GA_BEAT);
    fa = drum_sync_frames_to_next(0, 0, GRID_POS, GRID_PERIOD);
    drum_grid_publish(0, 1, GB_ANCHOR, GB_BEAT);
    fb = drum_sync_frames_to_next(0, 0, GRID_POS, GRID_PERIOD);
    drum_grid_publish(0, 1, GA_ANCHOR, GB_BEAT);
    fx = drum_sync_frames_to_next(0, 0, GRID_POS, GRID_PERIOD);
    drum_grid_publish(0, 1, GB_ANCHOR, GA_BEAT);
    fy = drum_sync_frames_to_next(0, 0, GRID_POS, GRID_PERIOD);
    CHECK("the two grids and both mixes give four different answers",
          fa >= 0 && fb >= 0 && fx >= 0 && fy >= 0 && fa != fb && fx != fa && fx != fb &&
              fy != fa && fy != fb);
    drum_grid_publish(0, 1, GA_ANCHOR, GA_BEAT);
    atomic_store(&g_grid_stop, 0);
    pthread_t th;
    pthread_create(&th, NULL, grid_writer, NULL);
    long reads = 0, whole = 0, torn = 0, t_end = grid_now_ms() + GRID_MS;
    while (grid_now_ms() < t_end) {
        for (int k = 0; k < 1000; k++) {
            long f = drum_sync_frames_to_next(0, 0, GRID_POS, GRID_PERIOD);
            reads++;
            if (f < 0)
                continue;
            whole++;
            if (f != fa && f != fb)
                torn++;
        }
    }
    atomic_store(&g_grid_stop, 1);
    pthread_join(th, NULL);
    char m[160];
    snprintf(m, sizeof m, "%ld reads, %ld accepted, %ld from a mix of two grids", reads, whole,
             torn);
    CHECK(m, whole > 1000 && torn == 0);
    atomic_store(&g_drum_sync_on, 0);
}

int main(int argc, char **argv) {
    if (argc < 2)
        return 2;
    dir = argv[1];
    setvbuf(stdout, NULL, _IONBF, 0);
    test_wav();
    test_service();
    test_swap();
    test_mix();
    test_follow();
    test_grid_rate();
    test_grid();
    return ut_summary();
}
