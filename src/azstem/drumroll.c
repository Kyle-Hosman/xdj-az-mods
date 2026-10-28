/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file drumroll.c
 * @brief Sample-based drum-roll instrument. Included from azstem.c.
 *
 * `DRUM ROLLS/Bank N/` on the stick holds kick, snare, clap and hihat. In
 * SLIP-LOOP pad mode pad 8 arms the roll, pads 1-4 pick the sample and the
 * x-pad's 6 positions pick the rate (1 to 1/32 beat), synced to deck BPM.
 *
 * azmod owns input and calls the azstem_drum_* exports; this file owns
 * discovery, decoding, the voice scheduler and the mix.
 *
 * Threading: the mix runs on the audio thread (no allocation, syscalls or
 * locks); everything else runs on the flusher thread.
 */

#define DRUM_SLOTS 4 /* kick, snare, clap, hihat */
#define DRUM_BANKS 4 /* Bank 1..4, matching the four mod-menu rows */
/* Indexed by logical deck 0..3, not physical side, so decks 3 and 4 work too.
 * azmod sends azmod_logical_deck() from the pad path and the mix is entered
 * with deck_slot() (STEM_SLOT {0,1,2,3}): two names for the same index. */
#define DRUM_SIDES 4
#include "ep147rate.h"
#define DRUM_SR AZ_EP147_RATE /* the deck output rate the mix runs at */

static const char *DRUM_SLOT_NAMES[DRUM_SLOTS] = {"kick", "snare", "clap", "hihat"};
/* Roll rate in beats per hit, x-pad left -> right. 1 beat is the slowest. */
static const float DRUM_RATE_BEATS[6] = {1.0f, 0.5f, 0.25f, 0.125f, 0.0625f, 0.03125f};
#define DRUM_RATES ((int) (sizeof DRUM_RATE_BEATS / sizeof DRUM_RATE_BEATS[0]))

/* One decoded sample: interleaved stereo f32 at 44100. */
typedef struct {
    float *data;
    long frames;
} drum_sample;

/* The loaded bank. Published as a whole: the RT side reads g_drum_gen once,
 * then the pointers, so it never sees a half-swapped bank. */
static drum_sample g_drum_smp[DRUM_SLOTS];
static _Atomic int g_drum_gen = 0;    /* bumped after a bank swap */
static _Atomic int g_drum_bank = 1;   /* 1-based, matches the menu */
static _Atomic int g_drum_loaded = 0; /* 1 = at least one sample ready */
static char g_drum_base[1024];        /* .../DRUM ROLLS */
static _Atomic int g_drum_have_base = 0;
/* Identity of g_drum_base when it was resolved. A stick swap either removes the path
 * or, when the new stick mounts at the same path, gives the directory a new (dev, ino):
 * FAT and exFAT number inodes per mount. drum_service compares about once a second. */
static dev_t g_drum_base_dev;
static ino_t g_drum_base_ino;
static int g_drum_discover_skip = 0;   /* flusher ticks until the next discovery walk */
static int g_drum_check_skip = 0;      /* flusher ticks until the next base identity check */
#define DRUM_DISCOVER_TICKS 25         /* ~5 s at the flusher's 200 ms */
#define DRUM_BASE_CHECK_TICKS 5        /* ~1 s */
/* Audio-thread blocks inside drum_mix_block right now. The flusher waits for zero (with
 * g_drum_loaded cleared first, so a block entering after that touches nothing) before it
 * resets voices or frees a replaced bank. */
static _Atomic int g_drum_rt_inflight = 0;
#define DRUM_QUIESCE_MS 500
static _Atomic int g_drum_want_bank = 0;  /* flusher picks this up (0 = idle) */
static _Atomic int g_drum_req_bank = 0;   /* the bank azmod asked for while Drum Roll is on; 0 = not wanted */
static _Atomic int g_drum_banks_seen = 0; /* bitmask of banks found on the stick */

/* Per-deck performance state, written by azmod's input hooks (index = logical
 * deck 0..3, see DRUM_SIDES above). */
static _Atomic int g_drum_gate[DRUM_SIDES];    /* pad 8 held */
static _Atomic int g_drum_sel[DRUM_SIDES];     /* 0..3, pads 1-4 */
static _Atomic int g_drum_rate[DRUM_SIDES];    /* 0..5, x-pad position */
static _Atomic int g_drum_touch[DRUM_SIDES];   /* finger on the x-pad */
static _Atomic int g_drum_bpm_q16[DRUM_SIDES]; /* deck BPM, 16.16 fixed (0 = unknown) */

/* One voice per deck: a roll retriggers one sample, and the release fade
 * already covers the previous hit's tail. */
typedef struct {
    const float *data;
    long frames;
    long pos;    /* -1 = idle */
    double frac; /* sub-frame read position (pitch != 1) */
    int gen;     /* bank generation this pointer came from */
} drum_voice;
/* Drum tone: MIC 2 HI = volume, MIC 2 MID = pitch (10-bit knobs, read by azmod).
 * These are targets the mix glides to per sample, so a turn mid-hit never
 * clicks. Defaults are unity in case azmod never calls azstem_drum_tone. */
static _Atomic int g_drum_gain_k = 1000;   /* target gain  x1000 */
static _Atomic int g_drum_ratio_k = 65536; /* target rate  x65536 (1.0 = original pitch) */
static drum_voice g_drum_voice[DRUM_SIDES];
static long g_drum_next[DRUM_SIDES]; /* frames until the next retrigger */
/* Set on the gate's rising edge when beat sync has a grid: hold the first hit
 * until a fresh playhead supplies the anchor. EP147 refreshes the transport
 * position only every other call, so without this a press on a stale block
 * fires at once (g_drum_next = 0) and again when the anchor arrives: a double hit. */
static int g_drum_wait_anchor[DRUM_SIDES];
/* Output frames since this deck's last hit, saturating (starts saturated).
 * EP147's playhead does not sit at a constant offset from the output timeline,
 * so the grid correction needs this to tell a boundary still to come from one
 * just served. Without it a shrinking offset refires the same boundary (a flam
 * ~1 ms after the beat) and a growing one skips a boundary. */
#define DRUM_AGE_MAX (1L << 40)
static long g_drum_hit_age[DRUM_SIDES] = {[0 ... DRUM_SIDES - 1] = DRUM_AGE_MAX};
/* A boundary the playhead says was crossed at most this many output frames ago,
 * with no hit served for it, still fires (that late) while a roll is running;
 * capped at a quarter of the subdivision. ~5.8 ms: well inside what reads as on
 * the beat, and larger than a block-sized change in the playhead's offset. */
#define DRUM_SYNC_LATE_TOL 256
/* Beat sync needs a moving playhead. A paused deck's playhead is frozen and an empty
 * deck has none (-1), so a grid there cannot place a hit: the first-hit wait would hold
 * forever, and the roll would be silent on a paused or ejected deck. A deck whose
 * playhead has not changed for this many output frames rolls free-running at its BPM
 * instead. Same threshold as the stems' pause detection (PLL_PAUSE_LATENCY, ~0.1 s);
 * EP147 refreshes a playing deck's playhead every other block, far inside it. */
#define DRUM_SYNC_STALL 4410L
/* Output frames since this deck's playhead last changed, saturating; DRUM_AGE_MAX when
 * the playhead is unknown. RT only. */
static long g_drum_src_still[DRUM_SIDES] = {[0 ... DRUM_SIDES - 1] = DRUM_AGE_MAX};
static long long g_drum_prev_src[DRUM_SIDES];
/* The playhead the last beat-lock reading used, per side (its own, or its lead's). RT only. */
static long long g_drum_prev_sync[DRUM_SIDES];
/* An empty deck follows the master deck: azmod names the lead (azstem_drum_follow, -1 =
 * none), and the side's roll is placed on the lead's grid at the lead's playhead. Each
 * side's block publishes its playhead and whether it moves, for a follower to read. */
static _Atomic int g_drum_follow[DRUM_SIDES] = {[0 ... DRUM_SIDES - 1] = -1};
static _Atomic long long g_drum_src_pub[DRUM_SIDES] = {[0 ... DRUM_SIDES - 1] = -1};
static _Atomic int g_drum_moving_pub[DRUM_SIDES];

#define DRUM_FADE_IN 16   /* frames: kills the click on retrigger */
#define DRUM_FADE_OUT 192 /* frames: kills the click when a hit is cut short */

/* ---- WAV loading ----------------------------------------------------- *
 * A bank may hold WAV or OGG; OGG goes through azstem's vorbis decoder.
 * WAV: PCM 8/16/24/32-bit or IEEE float 32/64 (also as WAVE_FORMAT_EXTENSIBLE with a
 * PCM or float subformat), any channel count (the first two are used), any rate,
 * linear-resampled to 44100 (inaudible on a drum one-shot). Every other format code
 * (ADPCM, A-law, mu-law, MP3 in WAV) and every other bit depth is refused, so the slot
 * reads as missing instead of playing noise or silence.
 *
 * DRUM_MAX_SECONDS caps each sample. The slowest roll rate is one beat, 1.5 s at the
 * 40 BPM floor, so anything past that only ever plays as the release tail; 8 s covers
 * the longest natural cymbal or reverb tail and bounds a bank at about 11 MB (4 slots
 * x 8 s x 44100 x 2 ch x 4 B) inside EP147's process. A longer file is truncated at
 * the cap, never rejected (the mix fades the last DRUM_FADE_OUT frames, so the cut is
 * clean), and only the capped part is read from the stick. */
#define DRUM_MAX_SECONDS 8
#define DRUM_MAX_FRAMES ((long) DRUM_MAX_SECONDS * DRUM_SR)
#define DRUM_WAV_MIN_RATE 4000
#define DRUM_WAV_MAX_RATE 384000
#define DRUM_WAV_MAX_CHANS 32
static unsigned drum_rd_u32(const unsigned char *p) {
    return (unsigned) p[0] | ((unsigned) p[1] << 8) | ((unsigned) p[2] << 16) |
           ((unsigned) p[3] << 24);
}
static unsigned drum_rd_u16(const unsigned char *p) {
    return (unsigned) p[0] | ((unsigned) p[1] << 8);
}
/* One sample frame -> float in -1..1. The caller has already validated (bits, isfloat). */
static float drum_pcm_to_f(const unsigned char *p, int bits, int isfloat) {
    if (isfloat) {
        if (bits == 32) {
            float f;
            memcpy(&f, p, 4);
            return f;
        }
        double d;
        memcpy(&d, p, 8);
        return (float) d;
    }
    switch (bits) {
    case 8:
        return ((int) p[0] - 128) / 128.f; /* WAV 8-bit is unsigned */
    case 16: {
        short v;
        memcpy(&v, p, 2);
        return v / 32768.f;
    }
    case 24: {
        int v = (int) ((unsigned) p[0] << 8 | (unsigned) p[1] << 16 | (unsigned) p[2] << 24);
        return (float) (v >> 8) / 8388608.f;
    } /* sign-extend via >> */
    default: { /* 32 */
        int v;
        memcpy(&v, p, 4);
        return v / 2147483648.f;
    }
    }
}
/* The format a `fmt ` chunk describes, if the decoder accepts it: 1 with *isfloat set,
 * else 0. KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT are {0000000N-0000-0010-8000-00aa00389b71}. */
static int drum_wav_format_ok(const unsigned char *fmt, unsigned len, int *isfloat) {
    static const unsigned char ks_tail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                              0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
    if (len < 16)
        return 0;
    unsigned tag = drum_rd_u16(fmt);
    unsigned bits = drum_rd_u16(fmt + 14);
    if (tag == 0xFFFE) { /* WAVE_FORMAT_EXTENSIBLE: the real format is the subformat GUID */
        if (len < 40 || drum_rd_u16(fmt + 16) < 22 || memcmp(fmt + 26, ks_tail, sizeof ks_tail))
            return 0;
        tag = drum_rd_u16(fmt + 24);
    }
    if (tag == 1) {
        *isfloat = 0;
        return bits == 8 || bits == 16 || bits == 24 || bits == 32;
    }
    if (tag == 3) {
        *isfloat = 1;
        return bits == 32 || bits == 64;
    }
    return 0;
}
static float *drum_decode_wav(const char *path, long *out_frames) {
    *out_frames = 0;
    FILE *f = fopen(path, "rbe");
    if (!f)
        return NULL;
    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fclose(f);
        return NULL;
    }
    int chans = 0, bits = 0, isfloat = 0, have_fmt = 0;
    long rate = 0;
    unsigned char *raw = NULL;
    size_t rawlen = 0;
    for (;;) {
        unsigned char ch[8];
        if (fread(ch, 1, 8, f) != 8)
            break;
        unsigned sz = drum_rd_u32(ch + 4);
        if (!memcmp(ch, "fmt ", 4)) {
            unsigned char fmt[40];
            unsigned want = sz < sizeof fmt ? sz : (unsigned) sizeof fmt;
            if (fread(fmt, 1, want, f) != want)
                break;
            if (!drum_wav_format_ok(fmt, want, &isfloat))
                break; /* not a format this decoder reads: the slot stays empty */
            chans = (int) drum_rd_u16(fmt + 2);
            rate = (long) drum_rd_u32(fmt + 4);
            bits = (int) drum_rd_u16(fmt + 14);
            if (chans < 1 || chans > DRUM_WAV_MAX_CHANS || rate < DRUM_WAV_MIN_RATE ||
                rate > DRUM_WAV_MAX_RATE)
                break;
            have_fmt = 1;
            if (fseek(f, (long) (sz - want) + (long) (sz & 1), SEEK_CUR))
                break;
        } else if (!memcmp(ch, "data", 4)) {
            if (!have_fmt)
                break; /* data before fmt: refused */
            /* Read at most the capped part; a header that claims more than the file
             * holds (streaming writers) reads what is there. */
            size_t bpf = (size_t) (bits / 8) * (size_t) chans;
            long cap_in = (long) ((double) DRUM_MAX_FRAMES * (double) rate / DRUM_SR) + 2;
            size_t maxlen = (size_t) cap_in * bpf;
            rawlen = sz < maxlen ? sz : maxlen;
            raw = (unsigned char *) malloc(rawlen ? rawlen : 1);
            if (!raw)
                break;
            rawlen = fread(raw, 1, rawlen, f);
            break; /* data is the last chunk we need */
        } else {
            if (fseek(f, (long) sz + (long) (sz & 1), SEEK_CUR)) /* chunks are word-aligned */
                break;
        }
    }
    fclose(f);
    if (!raw || !have_fmt) {
        free(raw);
        return NULL;
    }
    int bypf = bits / 8; /* >= 1: drum_wav_format_ok admits only 8/16/24/32/64 */
    long in_frames = (long) (rawlen / ((size_t) bypf * (size_t) chans));
    if (in_frames <= 0) {
        free(raw);
        return NULL;
    }
    long out = rate == DRUM_SR ? in_frames : (long) ((double) in_frames * DRUM_SR / (double) rate);
    if (out > DRUM_MAX_FRAMES)
        out = DRUM_MAX_FRAMES;
    if (out <= 0) {
        free(raw);
        return NULL;
    }
    float *buf = (float *) calloc((size_t) out * 2, sizeof(float));
    if (!buf) {
        free(raw);
        return NULL;
    }
    for (long i = 0; i < out; i++) {
        double src = rate == DRUM_SR ? (double) i : (double) i * (double) rate / DRUM_SR;
        long i0 = (long) src;
        double fr = src - (double) i0;
        long i1 = i0 + 1 < in_frames ? i0 + 1 : in_frames - 1;
        if (i0 >= in_frames)
            i0 = in_frames - 1;
        for (int c = 0; c < 2; c++) {
            int sc = c < chans ? c : chans - 1; /* mono -> both sides */
            float a = drum_pcm_to_f(raw + (size_t) (i0 * chans + sc) * bypf, bits, isfloat);
            float b = drum_pcm_to_f(raw + (size_t) (i1 * chans + sc) * bypf, bits, isfloat);
            buf[i * 2 + c] = a + (float) fr * (b - a);
        }
    }
    free(raw);
    *out_frames = out;
    return buf;
}

/* ---- bank discovery -------------------------------------------------- */

/* Case-insensitive: the AZ's FAT tooling uppercases names on the stick, so
 * "DRUM ROLLS", "Drum Rolls" and "drum rolls" must all be found. */
static int drum_dir_named(const char *parent, const char *name, char *out, size_t outsz) {
    DIR *d = opendir(parent);
    if (!d)
        return 0;
    struct dirent *de;
    int found = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.')
            continue;
        if (strcasecmp(de->d_name, name))
            continue;
        snprintf(out, outsz, "%s/%s", parent, de->d_name);
        if (is_dir(out))
            found = 1;
        break;
    }
    closedir(d);
    return found;
}
/* Locate `DRUM ROLLS` on a mounted stick. Mirrors find_stems_base's roots and
 * depth so a stick that works for stems works for drum rolls. */
static int drum_find_base(char *out, size_t outsz) {
    const char *e = getenv("AZSTEM_DRUM_DIR");
    if (e && *e && is_dir(e)) {
        snprintf(out, outsz, "%s", e);
        return 1;
    }
    /* Not /mnt: that is the internal eMMC library, which never holds DRUM ROLLS. */
    static const char *roots[] = {"/media/usb", "/run/media", "/media"};
    for (size_t r = 0; r < sizeof roots / sizeof roots[0]; r++) {
        if (drum_dir_named(roots[r], "DRUM ROLLS", out, outsz))
            return 1;
        DIR *d = opendir(roots[r]);
        if (!d)
            continue;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.')
                continue;
            char sub[1024];
            snprintf(sub, sizeof sub, "%s/%s", roots[r], de->d_name);
            if (!is_dir(sub))
                continue;
            if (drum_dir_named(sub, "DRUM ROLLS", out, outsz)) {
                closedir(d);
                return 1;
            }
            DIR *d2 = opendir(sub);
            if (!d2)
                continue;
            struct dirent *de2;
            while ((de2 = readdir(d2))) {
                if (de2->d_name[0] == '.')
                    continue;
                char sub2[1600];
                snprintf(sub2, sizeof sub2, "%s/%s", sub, de2->d_name);
                if (is_dir(sub2) && drum_dir_named(sub2, "DRUM ROLLS", out, outsz)) {
                    closedir(d2);
                    closedir(d);
                    return 1;
                }
            }
            closedir(d2);
        }
        closedir(d);
    }
    return 0;
}
/* "hihat" also matches "hi-hat"/"hi hat"; everything is substring + case
 * insensitive so "01 Kick.wav" and "KICK.WAV" both land in slot 0. */
static int drum_slot_for_name(const char *fn) {
    char low[256];
    size_t i = 0;
    for (; fn[i] && i < sizeof low - 1; i++)
        low[i] = (char) ((fn[i] >= 'A' && fn[i] <= 'Z') ? fn[i] + 32 : fn[i]);
    low[i] = 0;
    /* strip separators so "hi-hat" and "hi_hat" reduce to "hihat" */
    char flat[256];
    size_t n = 0;
    for (size_t j = 0; low[j] && n < sizeof flat - 1; j++)
        if (low[j] != '-' && low[j] != '_' && low[j] != ' ')
            flat[n++] = low[j];
    flat[n] = 0;
    if (strstr(flat, "kick"))
        return 0;
    if (strstr(flat, "snare"))
        return 1;
    if (strstr(flat, "clap"))
        return 2;
    if (strstr(flat, "hihat") || strstr(flat, "hat"))
        return 3;
    return -1;
}
static int drum_is_audio(const char *fn) {
    const char *d = strrchr(fn, '.');
    return d && (!strcasecmp(d, ".wav") || !strcasecmp(d, ".ogg"));
}
/* Which banks exist, as a bitmask (bit 0 = Bank 1). Lets the menu gray out
 * rows for banks that are not on the stick instead of failing on tap. */
static void drum_scan_banks(void) {
    int mask = 0;
    if (atomic_load_explicit(&g_drum_have_base, memory_order_relaxed)) {
        for (int b = 1; b <= DRUM_BANKS; b++) {
            char want[64], path[1200];
            snprintf(want, sizeof want, "Bank %d", b);
            if (drum_dir_named(g_drum_base, want, path, sizeof path))
                mask |= 1 << (b - 1);
        }
    }
    atomic_store_explicit(&g_drum_banks_seen, mask, memory_order_relaxed);
}

/* Wait (flusher only) until no audio block is inside drum_mix_block. The caller has
 * already cleared g_drum_loaded with a seq_cst store; drum_mix_block increments
 * g_drum_rt_inflight (seq_cst) before it loads g_drum_loaded (seq_cst), so a block
 * either is counted here or sees the gate closed and touches nothing. 1 = quiet. */
static int drum_rt_quiesce(void) {
    for (int ms = 0; ms < DRUM_QUIESCE_MS; ms++) {
        if (atomic_load_explicit(&g_drum_rt_inflight, memory_order_seq_cst) == 0)
            return 1;
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
    return atomic_load_explicit(&g_drum_rt_inflight, memory_order_seq_cst) == 0;
}

/* Load one bank's four samples. Flusher thread only.
 * Close the RT gate, wait for the audio thread to leave the mix (drum_rt_quiesce),
 * reset the voices, swap the pointers, bump the generation, reopen the gate, then
 * free the old buffers: no voice can still hold one. */
static void drum_load_bank(int bank) {
    char dir[1200], want[64];
    snprintf(want, sizeof want, "Bank %d", bank);
    if (!atomic_load_explicit(&g_drum_have_base, memory_order_relaxed))
        return;
    if (!drum_dir_named(g_drum_base, want, dir, sizeof dir)) {
        logln("drum: bank %d not found under %s", bank, g_drum_base);
        return;
    }
    drum_sample fresh[DRUM_SLOTS];
    memset(fresh, 0, sizeof fresh);
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *de;
    /* Log which filter rejected each file (extension, slot name, duplicate,
     * decode): "no usable samples" alone cannot say which one fired. */
    int n_seen = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.')
            continue;
        n_seen++;
        if (!drum_is_audio(de->d_name)) {
            logln("drum: bank %d skip '%s' -- not .wav/.ogg", bank, de->d_name);
            continue;
        }
        int slot = drum_slot_for_name(de->d_name);
        if (slot < 0) {
            logln("drum: bank %d skip '%s' -- name matches no slot "
                  "(needs kick/snare/clap/hihat as a substring)",
                  bank, de->d_name);
            continue;
        }
        if (fresh[slot].data) {
            logln("drum: bank %d skip '%s' -- %s already filled (first match wins)", bank,
                  de->d_name, DRUM_SLOT_NAMES[slot]);
            continue;
        }
        char path[1600];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        long fr = 0;
        float *buf = strcasecmp(strrchr(de->d_name, '.'), ".ogg") == 0
                         ? decode_ogg_stereo_44100(path, &fr, DRUM_MAX_FRAMES)
                         : drum_decode_wav(path, &fr);
        if (buf && fr > 0) {
            fresh[slot].data = buf;
            fresh[slot].frames = fr;
            logln("drum: bank %d %s = %s (%ld frames)", bank, DRUM_SLOT_NAMES[slot], de->d_name,
                  fr);
        } else {
            logln("drum: bank %d skip '%s' -- DECODE FAILED (buf=%p frames=%ld)", bank, de->d_name,
                  (void *) buf, fr);
            free(buf);
        }
    }
    closedir(d);
    int any = 0;
    for (int i = 0; i < DRUM_SLOTS; i++)
        if (fresh[i].data)
            any = 1;
    if (!any) {
        logln("drum: bank %d has no usable samples (%d non-dot entries in %s "
              "-- see the per-file reasons above)",
              bank, n_seen, dir);
        return;
    }

    /* Close the RT gate, then wait until no audio block is inside the mix: a block
     * already running read g_drum_loaded before the store and may still be playing
     * from the old pointers. Only then reset the voices and swap. If the audio thread
     * does not leave within the bound, keep the old bank and drop the new one: nothing
     * the audio thread can see is freed. */
    atomic_store_explicit(&g_drum_loaded, 0, memory_order_seq_cst);
    if (!drum_rt_quiesce()) {
        logln("drum: bank %d NOT swapped -- the audio thread stayed in the mix for %d ms "
              "(keeping the loaded bank)",
              bank, DRUM_QUIESCE_MS);
        for (int i = 0; i < DRUM_SLOTS; i++)
            free(fresh[i].data);
        if (g_drum_smp[0].data || g_drum_smp[1].data || g_drum_smp[2].data || g_drum_smp[3].data)
            atomic_store_explicit(&g_drum_loaded, 1, memory_order_release);
        return;
    }
    for (int s = 0; s < DRUM_SIDES; s++) {
        g_drum_voice[s].pos = -1;
        g_drum_voice[s].data = NULL;
    }
    drum_sample old[DRUM_SLOTS];
    memcpy(old, g_drum_smp, sizeof old);
    memcpy(g_drum_smp, fresh, sizeof g_drum_smp);
    atomic_fetch_add_explicit(&g_drum_gen, 1, memory_order_release);
    atomic_store_explicit(&g_drum_bank, bank, memory_order_release);
    atomic_store_explicit(&g_drum_loaded, 1, memory_order_release);
    for (int i = 0; i < DRUM_SLOTS; i++)
        free(old[i].data);
    logln("drum: bank %d active", bank);
}

/* 1 while g_drum_base is still the directory drum_service resolved: it exists and
 * has the same (dev, ino). One stat(), on the flusher. */
static int drum_base_same(void) {
    struct stat st;
    return stat(g_drum_base, &st) == 0 && S_ISDIR(st.st_mode) && st.st_dev == g_drum_base_dev &&
           st.st_ino == g_drum_base_ino;
}

/* Called from the 200 ms flusher loop. Finds the stick, notices when it goes away or
 * is swapped (about once a second), honors a bank change requested by the menu, and
 * keeps re-scanning (~5 s) until a stick shows up. A loaded bank stays in memory when
 * its stick goes: it is RAM, and the roll keeps working. */
static void drum_service(void) {
    if (atomic_load_explicit(&g_drum_have_base, memory_order_relaxed)) {
        if (g_drum_check_skip > 0) {
            g_drum_check_skip--;
        } else {
            g_drum_check_skip = DRUM_BASE_CHECK_TICKS;
            if (!drum_base_same()) {
                logln("drum: %s is gone or now another stick's -- searching again", g_drum_base);
                atomic_store_explicit(&g_drum_have_base, 0, memory_order_release);
                atomic_store_explicit(&g_drum_banks_seen, 0, memory_order_relaxed);
                g_drum_discover_skip = 0; /* look again now, not in 5 s */
            }
        }
    }
    if (!atomic_load_explicit(&g_drum_have_base, memory_order_relaxed)) {
        /* Throttle discovery to ~5 s, like find_stems_base: a stick without
         * DRUM ROLLS/ would otherwise be walked 5x/s forever. */
        if (g_drum_discover_skip > 0) {
            g_drum_discover_skip--;
            return;
        }
        g_drum_discover_skip = DRUM_DISCOVER_TICKS;
        char base[1024];
        struct stat st;
        if (drum_find_base(base, sizeof base) && stat(base, &st) == 0) {
            snprintf(g_drum_base, sizeof g_drum_base, "%s", base);
            g_drum_base_dev = st.st_dev;
            g_drum_base_ino = st.st_ino;
            g_drum_check_skip = DRUM_BASE_CHECK_TICKS;
            atomic_store_explicit(&g_drum_have_base, 1, memory_order_release);
            logln("drum: base = %s", g_drum_base);
            drum_scan_banks();
            /* decode only for a Drum Roll that is on: a bank's open files would hold up
             * a USB STOP. This also reloads the bank from a stick that replaced the
             * previous one. */
            int req = atomic_load_explicit(&g_drum_req_bank, memory_order_relaxed);
            if (req && !atomic_load_explicit(&g_drum_want_bank, memory_order_relaxed))
                atomic_store_explicit(&g_drum_want_bank, req, memory_order_relaxed);
        } else {
            return;
        }
    }
    int want = atomic_exchange_explicit(&g_drum_want_bank, 0, memory_order_acq_rel);
    if (want >= 1 && want <= DRUM_BANKS)
        drum_load_bank(want);

    /* Manual BPM override, for testing without a loaded track. azmod feeds each
     * deck its live BPM ~4x/s from the gui::BpmState ctor (0x01f5da20, deck in
     * w1); this file overrides that feed on all decks every 200 ms, so it logs
     * on set and clear to make a forgotten override visible.
     *   echo 137.9 > /home/root/settings/azstem.drumbpm   (override all decks)
     *   rm         /home/root/settings/azstem.drumbpm     (back to per-deck)
     * Development builds only: a release never reads it (dev_input_open). */
    {
        static int ovr_last = 0;
        int ovr_now = 0;
        FILE *bf = dev_input_open("/home/root/settings/azstem.drumbpm");
        if (bf) {
            float v = 0.f;
            if (fscanf(bf, "%f", &v) == 1 && v > 0.f) {
                ovr_now = 1;
                for (int s = 0; s < DRUM_SIDES; s++)
                    azstem_drum_bpm(s, v);
                if (!ovr_last)
                    logln("drumbpm: MANUAL OVERRIDE %.2f on all %d decks "
                          "-- azmod's per-deck BPM feed is being ignored "
                          "(rm /home/root/settings/azstem.drumbpm to restore)",
                          (double) v, DRUM_SIDES);
            }
            fclose(bf);
        }
        if (ovr_last && !ovr_now)
            logln("drumbpm: manual override cleared -- per-deck BPM feed back in charge");
        ovr_last = ovr_now;
    }

    /* Test hook: drive the voices without azmod.
     *   echo "<side> <sel> <rate> <gate>" > /home/root/settings/azstem.drumtest */
    {
        FILE *tf = dev_input_open("/home/root/settings/azstem.drumtest");
        if (tf) {
            int side = 0, sel = 0, rate = 2, gate = 0;
            if (fscanf(tf, "%d %d %d %d", &side, &sel, &rate, &gate) >= 1) {
                azstem_drum_select(side, sel);
                azstem_drum_rate(side, rate);
                azstem_drum_gate(side, gate); /* gate clears touch: gate first */
                azstem_drum_touch(side, gate);
            }
            fclose(tf);
        }
    }
}

void azstem_drum_gate(int side, int on);
void azstem_drum_select(int side, int slot);
void azstem_drum_rate(int side, int idx);
void azstem_drum_touch(int side, int on);
void azstem_drum_bpm(int side, float bpm);
void azstem_drum_sync(int on);

/* ---- Beat sync: the grid, the anchor, and the RT phase lock ------------- *
 *
 * The grid is read from EP147's memory, rooted in the gui::LoadState ctor
 * (0x1f9c498, deck in w4): +0x170 AnalyzedDataProvider<BeatGrid> -> +0x10
 * UpdatableSharedObject<BeatGrid> -> +0x28 Content. It matches the track's PQTZ
 * beat for beat and is in 44100 Hz source samples, despite the 96000 hardcoded
 * in the builder at 0x1e166b0. A second producer (Quantize_RequestTicket, 0x17b0380
 * on 1.30) converts each beat's microseconds with a sample rate it is handed at run
 * time. So the grid's rate is not assumed: drum_grid_native measures it from the
 * cells' own tempo field and converts the grid to the playhead's time base.
 *
 * Time base: play_S is source samples at AZ_EP147_RATE (PQTZ time_ms * 44.1), tempo
 * does not enter it, and its origin offset is zero (residual +22
 * samples, 0.001 beat). So phase is exact in source time.
 *
 * Threading: the flusher publishes two numbers, an anchor and a beat length,
 * under a seqlock; the RT side does one fmod and skips a block rather than read
 * a torn grid. */

/* Correction for unmeasured cases (non-44100 sources, variable-tempo grids),
 * not a normal tuning knob: the origin measures zero. Source samples, may be
 * negative. Read on the flusher only. */
#define DRUM_SYNCOFF_PATH "/home/root/settings/azstem.drumsyncoffset"

typedef struct {
    _Atomic unsigned seq; /* even = stable, odd = mid-write (seqlock) */
    /* The fields are atomics, read and written relaxed: the seqlock's fences order them, and a
     * reader that overlaps a write reads a value it then discards instead of racing on it. */
    _Atomic long long anchor_src; /* source sample of the grid's first beat */
    _Atomic double beat_src;      /* source samples per beat */
    _Atomic int valid;            /* 0 = no usable grid -> free-running */
} drum_grid;
static drum_grid g_drum_grid[DRUM_SIDES];
static _Atomic int g_drum_sync_on = 0;        /* azmod owns this; power-on default off */
static _Atomic int g_grid_demand = 0;         /* a community mod asked for beat phases */
static _Atomic long g_drum_sync_off = 0;      /* the tunable, source samples */
static _Atomic int g_sfx_mode;                /* stem_fx.c: Stem FX on pads 5-8 reads the grid */
static unsigned g_drum_grid_seen[DRUM_SIDES]; /* last track generation parsed */
static unsigned g_drum_grid_told[DRUM_SIDES]; /* last generation logged as having no grid */
static int g_drum_grid_init = 0;

/* First hit on press: 1 = fire immediately and lock from the next subdivision;
 * 0 = wait for the next subdivision boundary. Wait is the choice: an off-grid
 * hit sounds worse than a late one (up to ~460 ms at the slowest rate). */
#define DRUM_SYNC_FIRST_HIT_IMMEDIATE 0

/* One writer (the flusher). The release fence after the odd store keeps the field stores
 * from becoming visible before it: a release on the odd store itself would order only what
 * came before it. Readers pair it with an acquire fence before their second look at seq. */
static void drum_grid_publish(int side, int valid, long long anchor, double beat) {
    drum_grid *g = &g_drum_grid[side];
    unsigned s = atomic_load_explicit(&g->seq, memory_order_relaxed);
    atomic_store_explicit(&g->seq, s + 1, memory_order_relaxed); /* -> odd */
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&g->anchor_src, anchor, memory_order_relaxed);
    atomic_store_explicit(&g->beat_src, beat, memory_order_relaxed);
    atomic_store_explicit(&g->valid, valid, memory_order_relaxed);
    atomic_store_explicit(&g->seq, s + 2, memory_order_release); /* -> even, after every field */
}

/* ---- EP147's own beat grid, per deck, ctor-rooted ------------------------
 * EP147 has already parsed the grid, so read its copy: walking the library for
 * ANLZ0000.DAT can take over a minute per track change on a large library.
 *
 *   LoadState (g_ls_self[deck], captured from the ctor that receives the deck)
 *     +0x170  AnalyzedDataProvider<BeatGrid>   (embedded)
 *       +0x10 -> UpdatableSharedObject<BeatGrid>     (a slot: 0 until the analysis lands)
 *         +0x28 -> BeatGrid::Content                 (a slot: replaced when the grid changes)
 *                    +0x28 Cell[] {int64 pos; double bpm}   +0x30 nbeats
 *
 * cell.pos is a source sample at the rate its producer used (44100 on every grid
 * measured so far, the same base as play_S); cell.bpm is the beat's tempo.
 *
 * Both objects are meow reference-counted (count at +0x0c, guard 'XOCR' at +0x10, owner at
 * +0x18) and both slots are swapped by EP147 while other threads read them, so they are read
 * the way EP147's own readers read a swapped object (0x1492ce0 on 1.30): load the slot, check
 * the type, take a reference by compare-and-swap only while the count is above zero, check the
 * guard again, use, release; the release that drops the count to zero destroys the object
 * through its owner, as EP147 does. Held by our reference, the Content cannot be freed or
 * replaced under the copy, so the count, the cells pointer and the cells all come from one
 * grid. The LoadState slot itself is read inside ls_enter / ls_leave (stem_playback.c), so the
 * LoadState cannot be destroyed between loading it and taking the reference. */
#define BG_PROV_OFF 0x180UL /* LoadState + provider(0x170) + 0x10 */
#define BG_USO_VT EP_2754518
#define BG_CONTENT_OFF 0x28UL
#define BG_CONTENT_VT \
    EP_2798328               /* vtable symbol 0x2798318 + 0x10: a live                             \
                              * object points past offset-to-top and                               \
                              * typeinfo.                                                          \
                              */
#define BG_XOCR 0x52434f58U /* 'XOCR' meow guard at +0x10 */
#define BG_CELLS_OFF 0x28UL
#define BG_NBEATS_OFF 0x30UL
#define MEOW_RC_OFF 0x0cUL
#define MEOW_GUARD_OFF 0x10UL
#define MEOW_OWNER_OFF 0x18UL
#define MEOW_DESTROY EP_22043f0 /* (owner, object): what EP147 calls when a release reaches 0 */

/* The sample rate a grid's positions are in, from the grid itself: K beats span
 * (pos[K-1] - pos[0]) samples and sum(60 / bpm[i]) seconds. Returns the rate EP147 can
 * have used (44.1, 48, 88.2 or 96 kHz) when the measurement is within 1% of it, else 0
 * = unknown: a grid whose tempo field and spacing disagree is not trusted. */
#define BG_RATE_CELLS 64
static long bg_grid_rate(const unsigned char *cells, int k) {
    static const long rates[] = {44100, 48000, 88200, 96000};
    if (k < 2)
        return 0;
    double secs = 0.0;
    long long p0 = 0, pl = 0;
    memcpy(&p0, cells, 8);
    memcpy(&pl, cells + (size_t) (k - 1) * 16, 8);
    for (int i = 0; i < k - 1; i++) {
        double bpm = 0.0;
        memcpy(&bpm, cells + (size_t) i * 16 + 8, 8);
        if (!(bpm >= 20.0 && bpm <= 999.0))
            return 0; /* not a tempo (NaN fails too) */
        secs += 60.0 / bpm;
    }
    if (pl <= p0 || secs <= 0.0)
        return 0;
    double est = (double) (pl - p0) / secs;
    for (size_t r = 0; r < sizeof rates / sizeof rates[0]; r++)
        if (est > (double) rates[r] * 0.99 && est < (double) rates[r] * 1.01)
            return rates[r];
    return 0;
}

static int bg_ptr_ok(uintptr_t p) {
    return p >= 0x10000 && p <= 0x0000ffffffffffffUL && !(p & 7);
}
/* EP147's acquire on a meow object (0x6b0c98 on 1.30): guard, count above zero, compare-and-swap
 * +1, guard again. 1 = we hold a reference. A guard that changed after the increment is left as
 * EP147 leaves it (the object is being torn down; touching its count again would be worse). */
static int meow_acquire(uintptr_t o) {
    if (!bg_ptr_ok(o))
        return 0;
    if (*(const volatile uint32_t *) (o + MEOW_GUARD_OFF) != BG_XOCR)
        return 0;
    _Atomic int *rc = (_Atomic int *) (o + MEOW_RC_OFF);
    int c = atomic_load_explicit(rc, memory_order_acquire);
    do {
        if (c <= 0)
            return 0; /* already on its way out */
    } while (!atomic_compare_exchange_weak_explicit(rc, &c, c + 1, memory_order_acq_rel,
                                                    memory_order_acquire));
    return *(const volatile uint32_t *) (o + MEOW_GUARD_OFF) == BG_XOCR;
}
/* EP147's release (0x6b0db8 on 1.30) and what its callers do with a count that reaches zero:
 * destroy the object through its owner. */
static void (*g_meow_destroy_fn)(void *, void *);
static void meow_release(uintptr_t o) {
    if (*(const volatile uint32_t *) (o + MEOW_GUARD_OFF) != BG_XOCR)
        return;
    _Atomic int *rc = (_Atomic int *) (o + MEOW_RC_OFF);
    if (atomic_load_explicit(rc, memory_order_acquire) <= 0)
        return;
    if (atomic_fetch_sub_explicit(rc, 1, memory_order_acq_rel) == 1) {
        void (*destroy)(void *, void *) = g_meow_destroy_fn;
        if (destroy)
            destroy(*(void *const volatile *) (o + MEOW_OWNER_OFF), (void *) o);
    }
}
/* Load a swapped slot and take a reference on what it holds, if it is of type `vt`. */
static uintptr_t meow_take(uintptr_t slot, uintptr_t vt) {
    uintptr_t o = atomic_load_explicit((_Atomic uintptr_t *) slot, memory_order_acquire);
    if (!bg_ptr_ok(o) || *(const volatile uintptr_t *) o != vt)
        return 0;
    return meow_acquire(o) ? o : 0;
}

/* 1 with anchor+beat filled, else 0 = unknown (empty, ejected, or ~2 s
 * mid-load: all read uso==0). No debounce here; the caller's eject handling
 * covers it. */
static int drum_grid_native(int deck, long long *anchor, double *beat) {
    if (!g_meow_destroy_fn)
        return 0; /* without EP147's release path a reference could not be dropped */
    uintptr_t ls = ls_enter(deck); /* the LoadState cannot be destroyed until ls_leave */
    uintptr_t uso = ls ? meow_take(ls + BG_PROV_OFF, (uintptr_t) BG_USO_VT) : 0;
    ls_leave(deck);
    if (!uso)
        return 0;
    uintptr_t content = meow_take(uso + BG_CONTENT_OFF, (uintptr_t) BG_CONTENT_VT);
    meow_release(uso);
    if (!content)
        return 0;
    int ok = 0;
    long rate = 0;
    long long p0 = 0, plast = 0;
    int32_t n = *(const volatile int32_t *) (content + BG_NBEATS_OFF);
    uintptr_t cells = *(const volatile uintptr_t *) (content + BG_CELLS_OFF);
    if (n >= 2 && n <= (1 << 20) && bg_ptr_ok(cells)) {
        /* meow's own red zones, so a plausible-but-wrong pointer is refused. */
        int zones = 1;
        for (int i = 0; i < 16 && zones; i++)
            zones = *(const volatile unsigned char *) (cells - 16 + (uintptr_t) i) == 0xAF &&
                    *(const volatile unsigned char *) (cells + (uintptr_t) n * 16 + (uintptr_t) i) ==
                        0xEF;
        if (zones) {
            unsigned char head[BG_RATE_CELLS * 16];
            int k = n < BG_RATE_CELLS ? n : BG_RATE_CELLS;
            memcpy(head, (const void *) cells, (size_t) k * 16);
            memcpy(&p0, head, 8);
            long long p1 = 0;
            memcpy(&p1, head + 16, 8);
            memcpy(&plast, (const void *) (cells + (uintptr_t) (n - 1) * 16), 8);
            /* Beat length from the span, not from cells[1]-cells[0]: per-beat jitter is
             * -41..+18 samples, and averaging over the whole track removes it. */
            if (p1 > p0 && plast > p0) {
                rate = bg_grid_rate(head, k); /* the grid's own rate, then into play_S's base */
                ok = 1;
            }
        }
    }
    meow_release(content);
    if (!ok)
        return 0;
    if (!rate) {
        logln("drumsync: d%d grid tempo and spacing disagree -- not used", deck);
        return 0;
    }
    double to_play = (double) AZ_EP147_RATE / (double) rate;
    *anchor = (long long) ((double) p0 * to_play + (p0 >= 0 ? 0.5 : -0.5));
    *beat = (double) (plast - p0) / (double) (n - 1) * to_play;
    return (*beat > 1000.0 && *beat < 200000.0); /* 13 - 2600 BPM: sanity, not a guess */
}

/* Flusher: reload each deck's grid when its track changes. Invalidate first so
 * the previous track's grid is never trusted; log every refusal with its reason. */
static void drum_grid_service(void) {
    {
        FILE *f = fopen(DRUM_SYNCOFF_PATH, "re");
        long v = 0;
        if (f) {
            if (fscanf(f, "%ld", &v) != 1)
                v = 0;
            fclose(f);
        }
        atomic_store_explicit(&g_drum_sync_off, v, memory_order_relaxed);
    }

    if (!g_drum_grid_init) {
        g_drum_grid_init = 1;
        for (int d = 0; d < DRUM_SIDES; d++)
            g_drum_grid_seen[d] =
                atomic_load_explicit(&g_deck_trackgen[d], memory_order_relaxed) - 1u;
    }
    /* No work while nothing reads the grid: Beat Sync off, no community mod asking for beat
     * phases and the stem pads not FX pads. The track generation is deliberately not consumed
     * then, so turning any of them on picks up the tracks already loaded. The grid drives
     * Drum Roll only while Beat Sync is on (drum_sync_frames_to_next checks it first). */
    if (!atomic_load_explicit(&g_drum_sync_on, memory_order_relaxed) &&
        !atomic_load_explicit(&g_grid_demand, memory_order_relaxed) &&
        !atomic_load_explicit(&g_sfx_mode, memory_order_relaxed))
        return;

    for (int d = 0; d < DRUM_SIDES && d < NDECK; d++) {
        unsigned gen = atomic_load_explicit(&g_deck_trackgen[d], memory_order_relaxed);
        if (gen == g_drum_grid_seen[d])
            continue;
        drum_grid_publish(d, 0, 0, 0.0); /* invalidate first */

        /* EP147's in-memory grid: microseconds, no file I/O. */
        long long anchor = 0;
        double beat = 0.0;
        if (!drum_grid_native(d, &anchor, &beat)) {
            /* Unknown (empty, ejected or mid-load): the grid stays invalid and the deck
             * free-runs. The generation stays unconsumed, so the next tick reads again: the
             * analysis often lands after the track change that announced it. */
            if (g_drum_grid_told[d] != gen + 1u) {
                g_drum_grid_told[d] = gen + 1u;
                logln("drumsync: d%d no native grid yet -- FREE-RUNNING", d);
            }
            continue;
        }
        g_drum_grid_seen[d] = gen;
        drum_grid_publish(d, 1, anchor, beat);
        logln("drumsync: d%d LOCKED  anchor=%lld src  beat=%.1f src (%.2f BPM)  from EP147", d,
              anchor, beat, (double) DRUM_SR * 60.0 / beat);
    }
}

/* Set by azmod, which publishes the persisted state at start-up; until then
 * sync is off. */
void azstem_drum_sync(int on) {
    int v = on ? 1 : 0;
    if (atomic_exchange_explicit(&g_drum_sync_on, v, memory_order_relaxed) != v)
        logln("drumsync: %s",
              v ? "ON (hits locked to the track's beat grid)" : "OFF (free-running)");
}

/* ---- RT: voice scheduler + additive mix ------------------------------- *
 * Additive and post-DSP: one-shots need no tempo or key following, and adding
 * cannot clobber the track.
 */
static long drum_period_frames(int side) {
    int ri = atomic_load_explicit(&g_drum_rate[side], memory_order_relaxed);
    if (ri < 0)
        ri = 0;
    else if (ri >= DRUM_RATES)
        ri = DRUM_RATES - 1;
    int q = atomic_load_explicit(&g_drum_bpm_q16[side], memory_order_relaxed);
    double bpm = q > 0 ? (double) q / 65536.0 : 120.0; /* unknown BPM -> 120 */
    if (bpm < 40.0)
        bpm = 40.0;
    else if (bpm > 220.0)
        bpm = 220.0;
    double beat = 60.0 / bpm; /* seconds per beat */
    double per = beat * (double) DRUM_RATE_BEATS[ri];
    long fr = (long) (per * DRUM_SR + 0.5);
    return fr < 64 ? 64 : fr; /* never faster than ~1.5 ms */
}
/* Is a usable grid available for this deck now? drum_sync_frames_to_next
 * returns -1 both for "no grid" and for "stale playhead"; the first-hit wait
 * must tell them apart, or a track with no grid would stay silent forever
 * instead of free-running. */
static int drum_sync_grid_ready(int side) {
    if (!atomic_load_explicit(&g_drum_sync_on, memory_order_relaxed))
        return 0;
    drum_grid *g = &g_drum_grid[side];
    unsigned s1 = atomic_load_explicit(&g->seq, memory_order_acquire);
    if (s1 & 1u)
        return 0;
    int valid = atomic_load_explicit(&g->valid, memory_order_relaxed);
    double beat = atomic_load_explicit(&g->beat_src, memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire); /* the fields before the second look at seq */
    unsigned s2 = atomic_load_explicit(&g->seq, memory_order_relaxed);
    return (s1 == s2) && valid && beat >= 1.0;
}
/* RT, once per block: output frames until the next grid subdivision, or -1 with
 * no usable grid (the caller then free-runs). Phase is computed in source time,
 * where tempo does not enter; `period` is the output interval at the live BPM,
 * so the fraction of a subdivision left maps directly onto it.
 * RT-safe: atomic loads and one fmod. */
static long drum_sync_frames_to_next(int grid_side, int side, long long src_pos, long period) {
    if (!atomic_load_explicit(&g_drum_sync_on, memory_order_relaxed))
        return -1;
    if (src_pos < 0)
        return -1;
    drum_grid *g = &g_drum_grid[grid_side]; /* the side's own grid, or its lead's */
    unsigned s1 = atomic_load_explicit(&g->seq, memory_order_acquire);
    if (s1 & 1u)
        return -1; /* mid-write: skip this block */
    long long anchor = atomic_load_explicit(&g->anchor_src, memory_order_relaxed);
    double beat = atomic_load_explicit(&g->beat_src, memory_order_relaxed);
    int valid = atomic_load_explicit(&g->valid, memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire); /* the fields before the second look at seq */
    unsigned s2 = atomic_load_explicit(&g->seq, memory_order_relaxed);
    if (s1 != s2 || !valid || beat < 1.0)
        return -1; /* torn or no grid */

    int ri = atomic_load_explicit(&g_drum_rate[side], memory_order_relaxed);
    if (ri < 0)
        ri = 0;
    else if (ri >= DRUM_RATES)
        ri = DRUM_RATES - 1;
    double sub = beat * (double) DRUM_RATE_BEATS[ri]; /* source samples per hit */
    if (sub < 1.0)
        return -1;

    double rel =
        (double) (src_pos - anchor - atomic_load_explicit(&g_drum_sync_off, memory_order_relaxed));
    double ph = fmod(rel, sub);
    if (ph < 0)
        ph += sub;                  /* before the first beat */
    double frac = (sub - ph) / sub; /* 0..1 of a subdivision left */
    long fr = (long) (frac * (double) period + 0.5);
    if (fr < 0)
        fr = 0;
    if (fr > period)
        fr = period;
    return fr;
}


static void drum_mix_block_in(int side, float **chans, int nch, int ns, long long src_pos) {
    if (side < 0 || side >= DRUM_SIDES)
        return;
    /* seq_cst, after the seq_cst increment in drum_mix_block: see drum_rt_quiesce */
    if (!atomic_load_explicit(&g_drum_loaded, memory_order_seq_cst))
        return;
    drum_voice *v = &g_drum_voice[side];
    int gen = atomic_load_explicit(&g_drum_gen, memory_order_acquire);
    int gate = atomic_load_explicit(&g_drum_gate[side], memory_order_relaxed) &&
               atomic_load_explicit(&g_drum_touch[side], memory_order_relaxed);

    if (!gate) {
        /* release: let the tail finish, then idle. No new retriggers. */
        g_drum_next[side] = 0;
    }
    long period = drum_period_frames(side);

    /* Is this deck's transport moving? Tracked whether or not the roll is gated, so a
     * press on a deck that has been paused for a while knows it at once. */
    if (src_pos < 0)
        g_drum_src_still[side] = DRUM_AGE_MAX;
    else if (src_pos != g_drum_prev_src[side])
        g_drum_src_still[side] = 0;
    else if (g_drum_src_still[side] < DRUM_AGE_MAX)
        g_drum_src_still[side] += ns;
    atomic_store_explicit(&g_drum_src_pub[side], src_pos, memory_order_relaxed);
    atomic_store_explicit(&g_drum_moving_pub[side], g_drum_src_still[side] < DRUM_SYNC_STALL,
                          memory_order_relaxed);
    /* Whose grid and playhead place the hits: this deck's, or, for an empty deck, the master
     * deck's (its lead). The lead's playhead is the one its last block published: at most a
     * block old, and re-read on every block like a deck's own. */
    int grid_side = side;
    long long sync_src = src_pos;
    int moving = g_drum_src_still[side] < DRUM_SYNC_STALL;
    int lead = atomic_load_explicit(&g_drum_follow[side], memory_order_relaxed);
    if (lead >= 0 && lead < DRUM_SIDES && lead != side) {
        grid_side = lead;
        sync_src = atomic_load_explicit(&g_drum_src_pub[lead], memory_order_relaxed);
        moving = atomic_load_explicit(&g_drum_moving_pub[lead], memory_order_relaxed);
    }
    /* The grid can place hits only on a moving deck (DRUM_SYNC_STALL). */
    int sync_ready = moving && drum_sync_grid_ready(grid_side);

    /* Beat sync: re-anchor the countdown to the grid once per block (~1.5 ms),
     * which also follows tempo moves and jogs. With sync off or no grid,
     * `tonext` is -1 and the free-running path is untouched.
     *
     * On the gate's rising edge, with a grid on a moving deck, the first hit waits for
     * the next boundary (DRUM_SYNC_FIRST_HIT_IMMEDIATE is 0); with no grid, or on a
     * paused or empty deck, it fires at once. */
    {
        static int prev_gate[DRUM_SIDES];
        long long *prev_src = g_drum_prev_src;
        /* Re-anchor only on a fresh playhead. EP147 refreshes it every other
         * call; re-anchoring on a stale one recomputes the same `tonext`, stalls
         * the countdown and makes the roll alternate one and three subdivisions.
         * Between refreshes the countdown free-runs at `period`. */
        long tonext = (gate && sync_src != g_drum_prev_sync[side])
                          ? drum_sync_frames_to_next(grid_side, side, sync_src, period)
                          : -1;
        if (gate && !prev_gate[side]) { /* rising edge */
            if (DRUM_SYNC_FIRST_HIT_IMMEDIATE || !sync_ready) {
                g_drum_wait_anchor[side] = 0; /* free-running: fire now */
                g_drum_next[side] = 0;
            } else {
                g_drum_wait_anchor[side] = 1; /* hold until the anchor lands */
            }
        }
        if (tonext >= 0) {
            /* One hit per boundary. `tonext` names the next boundary as this
             * reading sees it; the hit age says whether a hit already served it.
             *  - the last hit sits within half a subdivision of that boundary:
             *    it was fired early against an earlier reading, so the boundary
             *    is served and the next one is the target;
             *  - the reading has just crossed a boundary (by at most the late
             *    tolerance) whose turn came while the previous hit served the one
             *    before it: that boundary was skipped, so fire it now.
             * A fresh press has no recent hit, so neither case applies and the
             * first hit still waits for the next boundary. */
            long age = g_drum_hit_age[side];
            long passed = period - tonext; /* frames since the boundary just crossed */
            long tol = period / 4 < DRUM_SYNC_LATE_TOL ? period / 4 : DRUM_SYNC_LATE_TOL;
            if (age + tonext < period / 2)
                tonext += period;
            else if (passed <= tol && age - passed > period / 2 &&
                     age - passed < period + period / 2)
                tonext = 0;
            g_drum_next[side] = tonext;
            g_drum_wait_anchor[side] = 0;
        } else if (g_drum_wait_anchor[side] && !sync_ready) {
            g_drum_wait_anchor[side] = 0; /* grid vanished or the deck stopped
                                           * mid-wait: never hang silent */
        }
        if (!gate)
            g_drum_wait_anchor[side] = 0;
        if (src_pos >= 0)
            prev_src[side] = src_pos;
        if (sync_src >= 0)
            g_drum_prev_sync[side] = sync_src;
        prev_gate[side] = gate;
    }

    for (int s = 0; s < ns; s++) {
        if (gate) {
            if (g_drum_next[side] <= 0 && !g_drum_wait_anchor[side]) {
                int sel = atomic_load_explicit(&g_drum_sel[side], memory_order_relaxed);
                if (sel < 0)
                    sel = 0;
                else if (sel >= DRUM_SLOTS)
                    sel = DRUM_SLOTS - 1;
                if (g_drum_smp[sel].data && g_drum_smp[sel].frames > 0) {
                    v->data = g_drum_smp[sel].data;
                    v->frames = g_drum_smp[sel].frames;
                    v->pos = 0;
                    v->frac = 0.0;
                    v->gen = gen;
                }
                g_drum_next[side] = period;
                g_drum_hit_age[side] = 0;
            }
            g_drum_next[side]--;
        }
        if (g_drum_hit_age[side] < DRUM_AGE_MAX)
            g_drum_hit_age[side]++;
        if (v->pos < 0 || !v->data)
            continue;
        if (v->gen != gen) {
            v->pos = -1;
            v->data = NULL;
            continue;
        } /* bank swapped */
        long p = v->pos;
        if (p >= v->frames) {
            v->pos = -1;
            continue;
        }
        float g = 1.f;
        if (p < DRUM_FADE_IN)
            g = (float) (p + 1) / (float) DRUM_FADE_IN;
        long left = v->frames - p;
        if (left < DRUM_FADE_OUT)
            g *= (float) left / (float) DRUM_FADE_OUT;
        /* cut the tail cleanly when the next hit is about to land */
        if (gate && g_drum_next[side] < DRUM_FADE_OUT)
            g *= (float) (g_drum_next[side] + 1) / (float) DRUM_FADE_OUT;
        /* Tone: glide gain and rate toward the knob targets (~5 ms), then read at
         * a fractional position. A rate above 1 plays higher and shorter. */
        {
            static float sm_gain[DRUM_SIDES] = {1.f, 1.f, 1.f, 1.f},
                         sm_rate[DRUM_SIDES] = {1.f, 1.f, 1.f, 1.f};
            float tg = (float) atomic_load_explicit(&g_drum_gain_k, memory_order_relaxed) / 1000.f;
            float tr =
                (float) atomic_load_explicit(&g_drum_ratio_k, memory_order_relaxed) / 65536.f;
            sm_gain[side] += (tg - sm_gain[side]) * 0.005f;
            sm_rate[side] += (tr - sm_rate[side]) * 0.005f;
            g *= sm_gain[side];
            long p1 = p + 1 < v->frames ? p + 1 : p;
            float fr = (float) v->frac;
            float l = v->data[p * 2] + (v->data[p1 * 2] - v->data[p * 2]) * fr;
            float r = v->data[p * 2 + 1] + (v->data[p1 * 2 + 1] - v->data[p * 2 + 1]) * fr;
            chans[0][s] += l * g;
            if (nch > 1 && chans[1])
                chans[1][s] += r * g;
            double nf = v->frac + (double) sm_rate[side];
            long adv = (long) nf;
            v->frac = nf - (double) adv;
            v->pos = p + adv;
        }
    }
}

/* RT entry: counted, so a bank swap can wait for the audio thread to leave. */
static void drum_mix_block(int side, float **chans, int nch, int ns, long long src_pos) {
    atomic_fetch_add_explicit(&g_drum_rt_inflight, 1, memory_order_seq_cst);
    drum_mix_block_in(side, chans, nch, ns, src_pos);
    atomic_fetch_sub_explicit(&g_drum_rt_inflight, 1, memory_order_release);
}

/* ---- exported control surface (called by azmod, in-process) ----------- *
 * Plain atomic stores: azmod calls these from its bus interposer, which must not
 * do file I/O, and a 200 ms file channel is too slow for a pad press.
 */
int azstem_drum_available(void);
void azstem_drum_tone(int hi10, int mid10);
/* MIC 2 HI / MID (10-bit, center 512) -> drum volume / pitch, all decks.
 * HI: eq_knob_gain (center unity, left silent, right +6 dB). MID: center = original pitch,
 * +-12 semitones at the ends, with a dead band so 12 o'clock is exact. */
void azstem_drum_tone(int hi10, int mid10) {
    if (hi10 < 0)
        hi10 = 0;
    else if (hi10 > 1023)
        hi10 = 1023;
    if (mid10 < 0)
        mid10 = 0;
    else if (mid10 > 1023)
        mid10 = 1023;
    double g = eq_knob_gain(hi10); /* azstem.c: shared with Stem Volume */
    double semis = (mid10 >= 506 && mid10 <= 518) ? 0.0 : (mid10 - 512) / 511.0 * 12.0;
    double ratio = pow(2.0, semis / 12.0);
    atomic_store_explicit(&g_drum_gain_k, (int) (g * 1000.0 + 0.5), memory_order_relaxed);
    atomic_store_explicit(&g_drum_ratio_k, (int) (ratio * 65536.0 + 0.5), memory_order_relaxed);
}
void azstem_drum_bank(int bank);
void azstem_drum_gate(int side, int on);
void azstem_drum_select(int side, int slot);
void azstem_drum_rate(int side, int idx);
void azstem_drum_touch(int side, int on);
void azstem_drum_bpm(int side, float bpm);
void azstem_drum_follow(int side, int lead);

/* bit0..3 = banks present on the stick; 0 = no DRUM ROLLS folder found. */
int azstem_drum_available(void) {
    return atomic_load_explicit(&g_drum_banks_seen, memory_order_relaxed);
}
/* bank 1..DRUM_BANKS: Drum Roll is on with that bank (load it now, and again on the next stick);
 * 0: Drum Roll is off (nothing is loaded at the next stick discovery). */
void azstem_drum_bank(int bank) {
    if (bank < 0 || bank > DRUM_BANKS)
        return;
    atomic_store_explicit(&g_drum_req_bank, bank, memory_order_relaxed);
    if (bank)
        atomic_store_explicit(&g_drum_want_bank, bank, memory_order_relaxed);
}
void azstem_drum_gate(int side, int on) {
    if (side < 0 || side >= DRUM_SIDES)
        return;
    /* Arm/disarm forgets the finger: azmod delivers touch only to armed decks,
     * so a release while disarmed is never seen and the re-arm would roll with
     * nobody touching. A finger still down re-sends on its next frame. */
    atomic_store_explicit(&g_drum_touch[side], 0, memory_order_relaxed);
    atomic_store_explicit(&g_drum_gate[side], on ? 1 : 0, memory_order_relaxed);
}
void azstem_drum_select(int side, int slot) {
    if (side < 0 || side >= DRUM_SIDES || slot < 0 || slot >= DRUM_SLOTS)
        return;
    atomic_store_explicit(&g_drum_sel[side], slot, memory_order_relaxed);
}
void azstem_drum_rate(int side, int idx) {
    if (side < 0 || side >= DRUM_SIDES)
        return;
    if (idx < 0)
        idx = 0;
    else if (idx >= DRUM_RATES)
        idx = DRUM_RATES - 1;
    atomic_store_explicit(&g_drum_rate[side], idx, memory_order_relaxed);
}
void azstem_drum_touch(int side, int on) {
    if (side < 0 || side >= DRUM_SIDES)
        return;
    atomic_store_explicit(&g_drum_touch[side], on ? 1 : 0, memory_order_relaxed);
}
void azstem_drum_bpm(int side, float bpm) {
    if (side < 0 || side >= DRUM_SIDES)
        return;
    atomic_store_explicit(&g_drum_bpm_q16[side], bpm > 0.f ? (int) (bpm * 65536.f) : 0,
                          memory_order_relaxed);
}
/* lead 0..3: this side's roll is placed on that deck's beat grid at that deck's playhead
 * (azmod sends an empty deck the master deck); -1, itself or out of range: its own. */
void azstem_drum_follow(int side, int lead) {
    if (side < 0 || side >= DRUM_SIDES)
        return;
    if (lead < 0 || lead >= DRUM_SIDES || lead == side)
        lead = -1;
    atomic_store_explicit(&g_drum_follow[side], lead, memory_order_relaxed);
}
