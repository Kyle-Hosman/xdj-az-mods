/* SPDX-License-Identifier: GPL-3.0-only */
/* ---- Per-stem waveform envelope ------------------------------------------
 * azmod draws a mini-waveform under each stem toggle from a per-stem peak
 * envelope plus the playhead. Both live here, next to the decoded PCM and the
 * stem cursor; azmod binds azstem_wave() by dlsym (same process).
 * Peaks only, one fixed bucket count per track (AZW_N = 65536): EP147's own
 * waveform is 150 points/s, so this matches it for tracks up to ~7 minutes.
 * 256 KB per deck. */
static unsigned char g_env[NDECK][NSTEM][AZW_N];
static _Atomic int g_env_ready[NDECK];
/* Per-deck envelope generation, bumped whenever the content is replaced (cache
 * hit, first progressive publish, final build). azmod keys its peak cache on it
 * (azstem_env_gen) rather than on the ready flag's 0->1 edge, which it can miss
 * entirely if the view was elsewhere during a rebuild. */
static _Atomic unsigned g_env_gen[NDECK];
static _Atomic int g_env_final[NDECK]; /* 0 = progressive prefix, 1 = final build/cache */

/* ---- Progressive envelope ------------------------------------------------
 * Decoding the stems takes several seconds after a load and azmod has no curve
 * until an envelope exists, so the envelope fills incrementally as decoded chunks
 * land. This works because:
 *  1. The final length is known at open (stb_vorbis_stream_length_in_samples),
 *     so buckets map across the whole track from the first chunk and never move.
 *  2. azmod's attenuation is a ratio (enabled over all stems per bucket), so
 *     absolute normalization cancels: a partial envelope against a fixed
 *     reference is already correct.
 * Each bucket is filled once, in order, so the total cost equals one full build
 * (~240 ms on the deck). The unfilled tail stays uniform across stems, giving
 * att = enabled/4, a sane placeholder. build_env() rewrites everything at the end. */
static long g_env_frames[NDECK]; /* frames the cached/built/partial env describes */
static long g_env_total[NDECK];  /* final frames, from the OGG header */
static int g_env_filled[NDECK];  /* buckets computed so far */
/* Per load, under g_envp_lock: the first decoded stem seeds the progressive envelope once
 * (g_env_seeded), and leaves it alone when the load already published the cached one
 * (g_env_hit), which describes the whole track. */
static int g_env_seeded[NDECK];
static int g_env_hit[NDECK];
static pthread_mutex_t g_envp_lock = PTHREAD_MUTEX_INITIALIZER;
static void env_progress(int deck) {
    long total = g_env_total[deck];
    if (total <= 0)
        return;
    deck_stems *dk = &g_dk[deck];
    unsigned present = atomic_load_explicit(&g_stem_present[deck], memory_order_relaxed);
    long covered = -1;
    for (int k = 0; k < NSTEM; k++)
        if (present & (1u << k)) {
            long d = atomic_load_explicit(&g_stem_dec[deck][k], memory_order_relaxed);
            if (covered < 0 || d < covered)
                covered = d;
        }
    if (covered <= 0)
        return;
    int want = (int) ((double) covered / (double) total * AZW_N);
    if (want > AZW_N)
        want = AZW_N;
    pthread_mutex_lock(&g_envp_lock);
    int from = g_env_filled[deck];
    if (want > from) {
        for (int b = from; b < want; b++) {
            long i0 = (long) ((double) b * (double) total / AZW_N);
            long i1 = (long) ((double) (b + 1) * (double) total / AZW_N);
            if (i1 > covered)
                i1 = covered;
            long step = (i1 - i0) / 64;
            if (step < 1)
                step = 1;
            for (int k = 0; k < NSTEM; k++) {
                const float *nb = dk->stem[k];
                if (!nb || !(present & (1u << k))) {
                    g_env[deck][k][b] = 0;
                    continue;
                }
                float m = 0.f;
                for (long i = i0; i < i1; i += step) {
                    float a = nb[i * 2];
                    if (a < 0)
                        a = -a;
                    if (a > m)
                        m = a;
                }
                int val = (int) (m * 255.f + 0.5f); /* fixed reference: ratios cancel */
                g_env[deck][k][b] = (unsigned char) (val > 255 ? 255 : val);
            }
        }
        g_env_filled[deck] = want;
        if (g_env_frames[deck] <= 0)
            g_env_frames[deck] = total;
        /* Publish as soon as any region is real. The first publish of a load is
         * new content, so bump the generation. */
        if (!atomic_load_explicit(&g_env_ready[deck], memory_order_relaxed)) {
            atomic_store_explicit(&g_env_final[deck], 0, memory_order_relaxed);
            atomic_fetch_add_explicit(&g_env_gen[deck], 1, memory_order_release);
        }
        atomic_store_explicit(&g_env_ready[deck], 1, memory_order_relaxed);
    }
    pthread_mutex_unlock(&g_envp_lock);
}

/* ---- Envelope cache ------------------------------------------------------
 * The envelope (NSTEM*AZW_N = 256 KB) depends only on the track's stems, so it is
 * cached next to the stems on the USB, and a repeat load has its curve within
 * milliseconds. Never on p7: the 61 MB settings partition must not fill up.
 *
 * A cache hit does not touch stem_frames: azstem_frames() stays 0 until the audio
 * is really decoded, so azmod's UI and the RT hook never read a missing buffer.
 * Only the waveform arrives early. */
#define ENV_MAGIC "AZENV1\n"
#define ENV_MAX_FRAMES (2L * 3600 * 44100) /* two hours: no stems that long fit in memory */
static char g_env_dir[NDECK][1024]; /* stems folder for this deck's track */
static void env_cache_path(int deck, char *out, size_t n) {
    snprintf(out, n, "%s/azstem.env", g_env_dir[deck]);
}
static void env_cache_save(int deck) {
    if (!g_env_dir[deck][0])
        return;
    char path[1152], tmp[1160];
    env_cache_path(deck, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path); /* the stick can be pulled: whole or not at all */
    FILE *f = fopen(tmp, "wbe");
    if (!f) {
        logln("env: cache save failed for d%d (%s)", deck, path);
        return;
    }
    unsigned hdr[3] = {(unsigned) NSTEM, (unsigned) AZW_N, 0};
    long fr = g_env_frames[deck];
    int ok = fwrite(ENV_MAGIC, 1, sizeof ENV_MAGIC - 1, f) == sizeof ENV_MAGIC - 1 &&
             fwrite(hdr, sizeof hdr[0], 3, f) == 3 && fwrite(&fr, sizeof fr, 1, f) == 1 &&
             fwrite(g_env[deck], 1, sizeof g_env[deck], f) == sizeof g_env[deck];
    ok = fflush(f) == 0 && ok;
    ok = fsync(fileno(f)) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        logln("env: cache save failed for d%d (%s)", deck, path);
        return;
    }
    logln("env: cached %zu bytes for d%d -> %s", sizeof g_env[deck], deck, path);
}
/* Returns 1 and publishes the envelope if a valid cache exists for this folder. */
static int env_cache_load(int deck) {
    if (!g_env_dir[deck][0])
        return 0;
    char path[1152];
    env_cache_path(deck, path, sizeof path);
    FILE *f = fopen(path, "rbe");
    if (!f)
        return 0;
    char magic[sizeof ENV_MAGIC - 1];
    unsigned hdr[3];
    long fr = 0;
    int ok = fread(magic, 1, sizeof magic, f) == sizeof magic &&
             memcmp(magic, ENV_MAGIC, sizeof magic) == 0 && fread(hdr, sizeof hdr[0], 3, f) == 3 &&
             hdr[0] == (unsigned) NSTEM && hdr[1] == (unsigned) AZW_N &&
             fread(&fr, sizeof fr, 1, f) == 1 && fr > 0 && fr <= ENV_MAX_FRAMES &&
             fread(g_env[deck], 1, sizeof g_env[deck], f) == sizeof g_env[deck];
    fclose(f);
    if (!ok) {
        logln("env: cache REJECTED for d%d (%s)", deck, path);
        return 0;
    }
    g_env_frames[deck] = fr;
    atomic_store_explicit(&g_env_final[deck], 1, memory_order_relaxed);
    atomic_store_explicit(&g_env_ready[deck], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_env_gen[deck], 1, memory_order_release);
    logln("env: cache HIT for d%d frames=%ld -> waveform curve ready NOW", deck, fr);
    return 1;
}
/* Peak |L| per bucket over the whole track, all stems normalized by the loudest
 * one so relative levels survive (per-stem normalization would draw a quiet vocal
 * as large as the drums). Off-RT, once per load. */
static void build_env(int deck) {
    const deck_stems *dk = &g_dk[deck];
    const float *nb[NSTEM];
    long nf[NSTEM];
    for (int k = 0; k < NSTEM; k++) {
        nb[k] = dk->stem[k];
        nf[k] = dk->stem_frames;
    }
    atomic_store_explicit(&g_env_ready[deck], 0, memory_order_relaxed);
    memset(g_env[deck], 0, sizeof g_env[deck]);
    /* 1 MB: heap, not stack, and several decks can build concurrently. */
    float (*pk)[AZW_N] = calloc(NSTEM, sizeof *pk);
    if (!pk)
        return;
    float gmax = 0.f;
    for (int k = 0; k < NSTEM; k++) {
        if (!nb[k] || nf[k] <= 0)
            continue;
        for (int b = 0; b < AZW_N; b++) {
            long i0 = (long) ((double) b * (double) nf[k] / AZW_N);
            long i1 = (long) ((double) (b + 1) * (double) nf[k] / AZW_N);
            if (i1 > nf[k])
                i1 = nf[k];
            long step = (i1 - i0) / 64;
            if (step < 1)
                step = 1; /* sample, don't scan */
            float m = 0.f;
            for (long i = i0; i < i1; i += step) {
                float a = nb[k][i * 2];
                if (a < 0)
                    a = -a;
                if (a > m)
                    m = a;
            }
            pk[k][b] = m;
            if (m > gmax)
                gmax = m;
        }
    }
    if (gmax <= 0.f) {
        free(pk);
        return;
    }
    for (int k = 0; k < NSTEM; k++)
        for (int b = 0; b < AZW_N; b++) {
            int v = (int) (pk[k][b] / gmax * 255.f + 0.5f);
            g_env[deck][k][b] = (unsigned char) (v > 255 ? 255 : v);
        }
    free(pk);
    g_env_frames[deck] = dk->stem_frames;
    atomic_store_explicit(&g_env_final[deck], 1, memory_order_relaxed);
    atomic_store_explicit(&g_env_ready[deck], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_env_gen[deck], 1, memory_order_release);
    env_cache_save(deck);
}

/* Exported for azmod.so. Fills *env with NSTEM*AZW_N bytes (stem-major, 0-255
 * peak), *n with the bucket count and *pos with the playhead in 0..1.
 * Returns 0 while this deck has no decoded stems. */
int azstem_wave(int deck, const unsigned char **env, int *n, int *nstem, float *pos);
/* Per-deck envelope generation (see g_env_gen); azmod rebuilds its peak cache
 * when it changes. Acquire here, release on every bump: azmod reads the envelope
 * without g_envp_lock, so seeing the new gen before the new bytes would cache the
 * previous track's peaks under the new key. */
unsigned azstem_env_gen(int deck);
unsigned azstem_env_gen(int deck) {
    if (deck < 0 || deck >= NDECK)
        return 0;
    return atomic_load_explicit(&g_env_gen[deck], memory_order_acquire);
}
/* 1 once the envelope is final (full build or cache hit), 0 while progressive.
 * azmod waits for it so a load never renders the all-128 seed as a flat band. */
int azstem_env_final(int deck);
int azstem_env_final(int deck) {
    if (deck < 0 || deck >= NDECK)
        return 0;
    return atomic_load_explicit(&g_env_final[deck], memory_order_acquire);
}
/* Usable frames of this deck's stems (0 = none). azmod derives the duration from
 * it to tell EP147's two waveform providers apart (count == duration * 150/s). */
int azstem_frames(int deck);
int azstem_frames(int deck) {
    if (deck < 0 || deck >= NDECK)
        return 0;
    long fr = g_dk[deck].stem_frames;
    return fr > 0 ? (int) fr : 0;
}
/* Bumped on every confirmed track change, including a different file with the
 * same name (stems_tick compares name or path), and on an eject. azmod resets that
 * deck's stem toggles on it; azstem.curtrack alone carries only the name. */
unsigned azstem_trackgen(int deck);
unsigned azstem_trackgen(int deck) {
    if (deck < 0 || deck >= NDECK)
        return 0;
    return atomic_load_explicit(&g_deck_trackgen[deck], memory_order_relaxed);
}
/* The value of azstem.avail<N> as last published: 0, 1, or -1 before the first publish. */
int azstem_avail(int deck);
int azstem_avail(int deck) {
    if (deck < 0 || deck >= NDECK)
        return -1;
    return atomic_load_explicit(&g_avail_pub[deck], memory_order_acquire);
}
/* Bumped after each write of azstem.curtrack<N>: unchanged means the file is unchanged. */
unsigned azstem_curtrack_seq(int deck);
unsigned azstem_curtrack_seq(int deck) {
    if (deck < 0 || deck >= NDECK)
        return 0;
    return atomic_load_explicit(&g_curtrack_seq[deck], memory_order_acquire);
}
/* The final envelope of deck's stems, only when they are the stems of track content_id: *env as
 * azstem_wave's, *frames its length at AZ_EP147_RATE, *gen its generation (reread azstem_env_gen
 * after copying: a change means the copy cannot be trusted). 1 = filled, 0 = no such envelope. */
int azstem_track_env(int deck, unsigned content_id, const unsigned char **env, int *n, int *nstem,
                     long *frames, int *rate, unsigned *gen);
int azstem_track_env(int deck, unsigned content_id, const unsigned char **env, int *n, int *nstem,
                     long *frames, int *rate, unsigned *gen) {
    if (deck < 0 || deck >= NDECK || !content_id)
        return 0;
    *gen = atomic_load_explicit(&g_env_gen[deck], memory_order_acquire);
    if (atomic_load_explicit(&g_env_cid[deck], memory_order_acquire) != content_id ||
        !atomic_load_explicit(&g_env_ready[deck], memory_order_acquire) ||
        !atomic_load_explicit(&g_env_final[deck], memory_order_acquire))
        return 0;
    long fr = g_env_frames[deck];
    if (fr <= 0)
        return 0;
    *env = &g_env[deck][0][0];
    *n = AZW_N;
    *nstem = NSTEM;
    *frames = fr;
    *rate = AZ_EP147_RATE;
    return 1;
}
int azstem_wave(int deck, const unsigned char **env, int *n, int *nstem, float *pos) {
    if (deck < 0 || deck >= NDECK)
        return 0;
    if (!atomic_load_explicit(&g_env_ready[deck], memory_order_relaxed))
        return 0;
    /* The envelope's frame count: after a cache hit stem_frames may still be 0. */
    long fr = g_env_frames[deck];
    if (fr <= 0)
        fr = g_dk[deck].stem_frames;
    if (fr <= 0)
        return 0;
    double p = g_dk[deck].cur / (double) fr;
    if (p < 0)
        p = 0;
    else if (p > 1)
        p = 1;
    *env = &g_env[deck][0][0];
    *n = AZW_N;
    *nstem = NSTEM;
    *pos = (float) p;
    return 1;
}

/* Per-deck stem control masks (written by azmod; bit 7 engaged, bits 0-3
 * enabled) and the master enable. Deck 1's file name has no suffix. */
static const char *STEMS_PATH[NDECK] = {
    "/home/root/settings/azstem.stems",
    "/home/root/settings/azstem.stems2",
    "/home/root/settings/azstem.stems3",
    "/home/root/settings/azstem.stems4",
};
#define ENABLE_PATH "/home/root/settings/azstem.enable" /* "0" disables all stems */

/* Deck playhead: the deck object holds a pointer at +OFF_POS_PTR to a transport
 * object whose int64 at +OFF_POS is the current source sample position
 * (44100 Hz, same as the stems). */
#define OFF_POS_PTR 416
#define OFF_POS 88
/* Bidirectional cursor PLL (source frames @44100). The playhead refreshes every
 * other hook call, ~128 frames per step while playing. Reverse motion is clean
 * (backspin steps of -368..-824), so the cursor follows it and stems
 * reverse with the deck on backspins and scratches. Only a playhead that does
 * not move at all for PLL_PAUSE_LATENCY counts as paused. */
#define PLL_SEEK_JUMP \
    4096L /* |jump| beyond this = a seek: snap, don't feed the rate                                \
           * (backspins peak near 824) */
#define PLL_GARBAGE \
    (1L << 40)                  /* |jump| beyond this = a garbage read of the transport            \
                                 * pointer mid-track-load (observed, rare) */
#define PLL_PAUSE_LATENCY 4410L /* ~0.1 s with the playhead not moving at all means paused */
#define PLL_RATE_MIN 0.02       /* |rate| outside this band is not believable playback */
#define PLL_RATE_MAX 8.0

/* The same transport object (deck+OFF_POS_PTR) holds at +40 a char* to the loaded file's
 * full UTF-8 path. Nothing reads it: the track identity comes from LoadedTrackInfo, copied
 * inside EP147's own update (deck_track_map.c). */

static void publish_deck_file(int deck, const char *stem, const char *val);

/* The one gain curve for every mixer EQ knob azstem reads (10-bit, 12 o'clock =
 * 512): unity with a dead band (506..518) so center is exact, squared toward
 * silence on the left, up to +6 dB on the right. Used by Drum Roll volume
 * (MIC 2 HI) and Stem Volume (channel 3/4 EQ). */
static double eq_knob_gain(int v10) {
    if (v10 < 0)
        v10 = 0;
    else if (v10 > 1023)
        v10 = 1023;
    if (v10 >= 506 && v10 <= 518)
        return 1.0;
    if (v10 < 512) {
        double x = v10 / 512.0;
        return x * x;
    }
    return pow(2.0, (v10 - 512) / 511.0); /* 0 .. +6 dB */
}
/* Stem Volume: per deck, per stem gain x1000. azmod feeds channel 3's EQ for deck
 * 1 and channel 4's for deck 2 (HI = vocals, MID = inst and bass, LOW = drums) and
 * sends -1 (unity) while the mod is off. Every audible stem sum applies it
 * (stem_mix, gpitch, pbread_hook, fread_hook, stemsrc_read); the offset
 * correlators (ofs_stem_mono*) must not, since they match the original. At 1000
 * the product is bit-exact, so the mod off changes nothing. */
static _Atomic int g_sgain_k[NDECK][NSTEM] = {[0 ... NDECK - 1] = {[0 ... NSTEM - 1] = 1000}};
static inline void stem_gains(int deck, float *sg) {
    for (int k = 0; k < NSTEM; k++)
        sg[k] =
            (deck >= 0 && deck < NDECK)
                ? (float) atomic_load_explicit(&g_sgain_k[deck][k], memory_order_relaxed) * 0.001f
                : 1.0f;
}
/* All stems on plays the original: the stems never sum back to the record
 * (htdemucs_ft residual ~-26 dB, mostly phase in the lows). With all four stems
 * present and on and Stem Volume at unity, the injection leaves EP147's decoded
 * PCM in place, so the original plays through the same native path. Any stem off
 * or any knob off center selects the stem sum again. azstem.allsum forces the
 * stem sum; it is read off-RT by read_mute on the flusher. */
#define ALLSUM_PATH "/home/root/settings/azstem.allsum"
static _Atomic int g_allsum = 0; /* 1 = all-on plays the stem sum */
/* Switching between the stem sum and the original is crossfaded: they differ
 * mostly in phase, so a hard cut leaves a +9..+16 dB high-frequency transient.
 * The first injected read after a mode flip blends over XF_ORIG frames from the
 * old mode to the new. Per deck, RT-only state. */
#define XF_ORIG 1024              /* ~23 ms at 44.1 kHz */
static int g_orig_prev[NDECK];    /* last mode written: 0 sum, 1 original */
static unsigned g_em_prev[NDECK]; /* last stem mask written (any toggle blends too) */
static inline int stems_play_original(const deck_stems *dk, unsigned em, const float *sg) {
    if (em != 0x0Fu || atomic_load_explicit(&g_allsum, memory_order_relaxed))
        return 0;
    for (int k = 0; k < NSTEM; k++)
        if (!dk->stem[k] || sg[k] != 1.0f)
            return 0;
    return 1;
}
/* Is a track loaded on this deck (the track name the stems follow; an eject counts
 * after EJECT_EMPTY_TICKS)? azmod's Stem Volume stands down while deck 3 or 4 has a
 * track, since channels 3/4 are then real deck channels. Only maintained while the
 * stick has a stems tree. */
int azstem_deck_has_track(int deck);
int azstem_deck_has_track(int deck) {
    if (deck < 0 || deck >= NDECK)
        return 0;
    return ((volatile char *) g_dk[deck].loaded_track)[0] != 0;
}
/* The track name this deck's stems follow (file name without extension), for the
 * community mod API's deck_title(). Returns its length; 0 = none or unknown. Only
 * maintained while the stick has a stems tree. */
int azstem_deck_track_name(int deck, char *buf, int n);
int azstem_deck_track_name(int deck, char *buf, int n) {
    if (!buf || n <= 0)
        return 0;
    buf[0] = 0;
    if (deck < 0 || deck >= NDECK)
        return 0;
    char tmp[sizeof g_dk[0].loaded_track];
    memcpy(tmp, (const void *) g_dk[deck].loaded_track, sizeof tmp); /* a racy copy is fine: */
    tmp[sizeof tmp - 1] = 0;                                          /* always NUL-terminated */
    snprintf(buf, (size_t) n, "%s", tmp);
    return (int) strlen(buf);
}
void azstem_stem_eq(int deck, int hi10, int mid10, int low10);
void azstem_stem_eq(int deck, int hi10, int mid10, int low10) {
    if (deck < 0 || deck >= NDECK)
        return;
    int g[NSTEM] = {1000, 1000, 1000, 1000};
    if (hi10 >= 0 && mid10 >= 0 && low10 >= 0) {
        g[0] = (int) (eq_knob_gain(hi10) * 1000.0 + 0.5);  /* vocals */
        g[1] = (int) (eq_knob_gain(mid10) * 1000.0 + 0.5); /* inst   */
        g[2] = g[1];                                       /* bass: MID too */
        g[3] = (int) (eq_knob_gain(low10) * 1000.0 + 0.5); /* drums  */
    }
    for (int k = 0; k < NSTEM; k++)
        atomic_store_explicit(&g_sgain_k[deck][k], g[k], memory_order_relaxed);
}
/* The Stem Volume gain one stem of this deck plays at, x1000 (1000 = unity, also for
 * an unknown deck or stem). azmod scales that stem's share of the waveform by it. */
int azstem_stem_gain(int deck, int stem);
int azstem_stem_gain(int deck, int stem) {
    if (deck < 0 || deck >= NDECK || stem < 0 || stem >= NSTEM)
        return 1000;
    return atomic_load_explicit(&g_sgain_k[deck][stem], memory_order_relaxed);
}

/* Sum the enabled stems at a fractional source position (linear interpolation).
 * Separate so the declicker can evaluate two trajectories per sample. */
static inline void stem_mix(const deck_stems *dk, unsigned em, long nf, double fpos, float *L,
                            float *R) {
    long i0 = (long) floor(fpos); /* floor, not trunc: fpos goes negative when a
                                   * backspin runs past the start of the track */
    double fr = fpos - (double) i0;
    long a = i0 % nf;
    if (a < 0)
        a += nf;
    long b = a + 1;
    if (b >= nf)
        b = 0;
    float Lo = 0.f, Ro = 0.f, sg[NSTEM];
    stem_gains((int) (dk - g_dk), sg);
    for (int k = 0; k < NSTEM; k++)
        if ((em & (1u << k)) && dk->stem[k]) {
            const float *g = dk->stem[k];
            Lo += sg[k] * (g[a * 2] + (float) fr * (g[b * 2] - g[a * 2]));
            Ro += sg[k] * (g[a * 2 + 1] + (float) fr * (g[b * 2 + 1] - g[a * 2 + 1]));
        }
    *L = Lo;
    *R = Ro;
}

/* Free/unmap all stem buffers. Off-RT only. Order matters: close the RT gate
 * (stem_frames = 0) first, NULL each slot before releasing its memory, and fence
 * between, so a hook still running never dereferences freed memory. */
static void free_stems(int d) {
    deck_stems *dk = &g_dk[d];
    atomic_store_explicit(&dk->stem_off, 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_ofs_epoch[d], 1, memory_order_release);
    atomic_store_explicit(&g_ofs_state[d], OFS_IDLE, memory_order_release);
    dk->rd_valid = 0;
    atomic_store_explicit(&g_env_ready[d], 0, memory_order_relaxed);
    dk->stem_frames = 0;
    __sync_synchronize();
    for (int k = 0; k < NSTEM; k++) {
        if (!dk->stem[k])
            continue;
        void *p = (void *) dk->stem[k];
        size_t n = dk->stem_bytes[k];
        dk->stem[k] = NULL;
        __sync_synchronize();
        if (n) {
            munlock(p, n);
            if (dk->stem_mmap[k])
                munmap(p, n);
            else
                free(p);
        }
        dk->stem_bytes[k] = 0;
        dk->stem_mmap[k] = 0;
    }
}

/* Emulator test path: mmap the four pre-decoded raw f32 stems from /mnt/stems.
 * Used only when the USB has no stems tree. Off-RT; deck 1 only. */
static void load_stems_f32(int d) {
    deck_stems *dk = &g_dk[d];
    static const char *names[NSTEM] = {"vocals", "other", "bass", "drums"};
    long minfr = -1;
    for (int k = 0; k < NSTEM; k++) {
        char path[128];
        snprintf(path, sizeof path, "/mnt/stems/%s.f32", names[k]);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            dk->stem[k] = NULL;
            continue;
        }
        off_t sz = lseek(fd, 0, SEEK_END);
        void *m = (sz > 0) ? mmap(NULL, (size_t) sz, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
        close(fd);
        if (m == MAP_FAILED) {
            dk->stem[k] = NULL;
            continue;
        }
        mlock(m, (size_t) sz); /* keep resident: no page faults on the RT thread */
        dk->stem[k] = (const float *) m;
        dk->stem_bytes[k] = (size_t) sz;
        dk->stem_mmap[k] = 1;
        long fr = (long) (sz / (2 * sizeof(float)));
        if (minfr < 0 || fr < minfr)
            minfr = fr;
    }
    dk->stem_frames = (minfr < 0) ? 0 : minfr;
    build_env(d); /* the .f32 fallback feeds the lanes too */
}
