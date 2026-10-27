/* SPDX-License-Identifier: GPL-3.0-only */
/* ---- Stems: play 4 pre-decoded stems, toggling = muting a stem ---------- *
 * The deck's source PCM is replaced by the sum of the enabled stems, not
 * canceled against the mix. Stems are .ogg files on the USB, decoded off-RT to
 * interleaved stereo f32 @44100 per deck. dk->ctrl: bit7 (0x80) = engaged,
 * bits0-3 = enabled stems (1=vocals 2=other/INST 4=bass 8=drums), matching
 * azmod's VOX/INST/BASS/DRUM strip. Injection happens at the source read
 * (pbread_hook), so EP147's own DSP applies tempo/key/scratch to the stems. */
/* Bumped on every track change; invalidates that deck's cached deck->FGPR
 * mapping. A vtable check alone passes a reused allocation: deck B's new FGPR
 * landing at deck A's freed address would make A's mapping name B's object. */
static _Atomic unsigned g_deck_trackgen[NDECK];
static const int STEM_SLOT[NDECK] = {
    0, 1, 2, 3};                 /* audio slots (deck_slot) for Deck 1..4 */
static _Atomic int g_enable = 1; /* master on/off (azstem.enable); 0 = all decks pass through */

static deck_stems g_dk[NDECK];

/* ---- Stem offset vs EP147's own decode --------------------------------------
 * The stems and EP147's decode of the same file need not start on the same
 * sample. For a LAME MP3, EP147 keeps what ffmpeg/libsndfile strip: encoder delay
 * + 529 decoder delay + the 1152-sample Info frame (typically ~2257 samples,
 * 51 ms, stems early). With MASTER TEMPO on, that misalignment audibly smears the
 * stems (correlation with the original 0.16-0.78 unshifted vs 0.99 aligned).
 *
 * The offset depends on the file, so it is measured, never assumed: pbread_hook
 * copies EP147's original PCM for OFS_N source samples before injection
 * overwrites it, and the flusher cross-correlates that against the stem sum.
 * No confident peak -> offset 0. Results are per deck and tied to the load they
 * were measured on by g_ofs_epoch (bumped in free_stems). */
#define OFS_N 131072     /* ~3 s of source captured per attempt */
#define OFS_LAGMAX 8192  /* +/- search window (186 ms) */
#define OFS_DEC 16       /* coarse-search decimation */
#define OFS_MINCORR 0.80 /* fine peak needed to adopt a lag */
#define OFS_MARGIN 0.10  /* ... and its lead over the runner-up */
#define OFS_TRIES 6
#define NOSTEMOFS_PATH "/home/root/settings/azstem.nostemofs" /* escape hatch: never apply */
enum {
    OFS_IDLE,
    OFS_ARMED,
    OFS_CAPTURED,
    OFS_DONE
};
static _Atomic int g_ofs_state[NDECK];
static _Atomic unsigned g_ofs_epoch[NDECK];
static unsigned g_ofs_armed_epoch[NDECK];     /* flusher-only */
static int g_ofs_tries[NDECK];                /* flusher-only */
static long g_ofs_meas[NDECK];                /* flusher-only: the measured lag for this load */
static _Atomic long long g_ofs_base[NDECK];   /* source pos of cap[0]; -1 = unset */
static _Atomic long g_ofs_next[NDECK];        /* samples covered (flusher's count, for probestat) */
static float g_ofs_cap[NDECK][OFS_N];         /* original PCM, mono L+R */
static unsigned char g_ofs_cov[NDECK][OFS_N]; /* 1 = cap[i] written this attempt */
static int g_ofs_armticks[NDECK];             /* flusher-only: ticks since armed */
#define OFS_ARM_TICKS 150                     /* ~30 s to cover the window, else re-arm */
static _Atomic int g_nostemofs = 0;
/* Set once a song's stem files are matched, before decode. azmod's availability
 * is stem_frames>0 or pending, so the buttons appear while the decode runs. */
static _Atomic int g_stem_pending[NDECK];
/* One background decode per deck (loader_thread), so the flusher keeps serving
 * the other decks while this one decodes. */
static _Atomic int g_stem_loading[NDECK];
/* RT gate for a load in flight.
 *   g_gate_hold:  keeps dk->stem_frames at 0 until the load-time offset is known
 *                 (ofsload.c), so the stems never shift under a playing track.
 *   g_load_stale: the track was replaced mid-load. Its decode threads are still
 *                 writing the buffers, so only the loader may free them, after
 *                 joining (freeing earlier is a use-after-free write).
 *   g_load_again: asks stems_tick to load the current track once the stale load
 *                 has been cleaned up.
 * All three are guarded by g_adv_lock where they meet dk->stem_frames. */
static _Atomic int g_gate_hold[NDECK];
static _Atomic int g_load_stale[NDECK];
static _Atomic int g_load_again[NDECK];
static long g_ofs_load[NDECK]; /* the load-time offset in effect (probestat) */

/* Progressive (head-first) decode. Each stem decodes in 0.5 s chunks into a
 * pre-allocated buffer, and dk->stem_frames advances to the min decoded across
 * present stems, so injection starts within one chunk of load.
 * g_stem_dec[d][k]: frames decoded. g_stem_present[d]: slots still expected (a
 * failed decode clears its bit). g_adv_lock keeps the advance monotonic across
 * the decode threads. */
static _Atomic long g_stem_dec[NDECK][NSTEM];
static _Atomic unsigned g_stem_present[NDECK];
/* On-deck stem separation is too slow on this hardware; stems are always
 * prepared off-deck. */
static pthread_mutex_t g_adv_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- USB stems discovery + Ogg decode (all off-RT) ----------------------- *
 * The firmware mounts USB media at /media/usb/sd*. Per track folder:
 *   *Vocals*.ogg *Instruments*.ogg *Bass*.ogg *Drum*.ogg
 * matched case-insensitively by substring ("instruments" -> slot 1, other/INST).
 * Each .ogg is decoded to interleaved stereo f32 @44100 and mlock'd. */

static char g_stems_base[512]; /* discovered <stick>/STEMS dir, "" if none (shared) */

/* Per-deck gui::LoadState, captured by lsctor_hook (the ctor receives the deck).
 * Declared above the drumroll.c include because the beat grid hangs off the same
 * object: one ctor-rooted anchor serves both track identity and the grid. */
static _Atomic(void *) g_ls_self[NDECK];
/* LoadState lifetime. lsctor_hook publishes g_ls_self[d] only after the ctor has returned and
 * the object proved itself (vtable, magic, deck). A reader on another thread brackets its
 * loads with ls_enter / ls_leave: ls_enter counts the reader in and only then loads the slot;
 * lsdtor_hook, on the thread that destroys the object and before any of it is torn down,
 * unpublishes it, moves g_ls_gen[d] and then waits until no reader is inside. So a reader that
 * got a non-zero LoadState from ls_enter may load its fields with plain loads until ls_leave:
 * the destructor cannot start freeing it meanwhile. The bracket holds a handful of loads and
 * never blocks, so the destructor's wait is that long; and EP147 builds a LoadState once per
 * deck at start-up. Seq-cst on both sides: either the reader sees the slot empty, or the
 * destructor sees the reader. */
#define LS_VT EP_2ddb878 /* gui::LoadState primary vtable (RTTI); its ctor stores it at +0 */
#define LS_DECK_OFF 0x47c
#define LS_MAGIC_OFF 0x4a8
#define LS_MAGIC 0x5e715e28u
static _Atomic unsigned g_ls_gen[NDECK];
static _Atomic int g_ls_readers[NDECK];
static _Atomic unsigned g_ls_dtor_waits; /* destructor calls that found a reader inside */
/* 0 or the live LoadState of `deck`, vtable, magic and deck re-checked. Always pair with
 * ls_leave(deck), whatever this returned. */
static uintptr_t ls_enter(int deck) {
    if (deck < 0 || deck >= NDECK)
        return 0;
    atomic_fetch_add_explicit(&g_ls_readers[deck], 1, memory_order_seq_cst);
    uintptr_t ls = (uintptr_t) atomic_load_explicit(&g_ls_self[deck], memory_order_seq_cst);
    if (!ls || (ls & 7))
        return 0;
    uintptr_t vt = *(const volatile uintptr_t *) ls;
    unsigned magic = *(const volatile unsigned *) (ls + LS_MAGIC_OFF);
    unsigned d = *(const volatile unsigned *) (ls + LS_DECK_OFF);
    if (vt != (uintptr_t) LS_VT || magic != LS_MAGIC || d != (unsigned) deck)
        return 0;
    return ls;
}
static void ls_leave(int deck) {
    if (deck >= 0 && deck < NDECK)
        atomic_fetch_sub_explicit(&g_ls_readers[deck], 1, memory_order_release);
}
/* lsdtor_hook's side: wait for the readers ls_enter let in before the slot was cleared. */
static void ls_wait_readers(int deck) {
    /* Spin, yield, then sleep: if the destroying thread is real-time and the reader (the
     * flusher) was preempted on the same core, only a sleep gives it the CPU back. */
    unsigned spins = 0;
    while (atomic_load_explicit(&g_ls_readers[deck], memory_order_seq_cst) != 0) {
        if (!spins++)
            atomic_fetch_add_explicit(&g_ls_dtor_waits, 1, memory_order_relaxed);
        if (spins > 256) {
            struct timespec ts = {0, 20000};
            nanosleep(&ts, NULL);
        } else if (spins > 64)
            sched_yield();
    }
    atomic_thread_fence(memory_order_acquire); /* the readers' loads happen before the teardown */
}

static int is_dir(const char *p) {
    struct stat st;
    return (stat(p, &st) == 0 && S_ISDIR(st.st_mode));
}
/* Stems base under <root>: <root>/STEMS (what AZ-MODS writes), only when it is not empty:
 * AZ-MODS creates it on every stick it sets up, so an empty STEMS/ is normal on a stick
 * without stems. Matched case-insensitively because FAT tooling may uppercase.
 * This picks the base for the availability scan only; a track load searches every
 * stick and ranks candidates (find_track_folder_any_c). */
static int dir_has_entries(const char *p) {
    DIR *d = opendir(p);
    if (!d)
        return 0;
    struct dirent *de;
    int n = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.')
            continue;
        n = 1;
        break;
    }
    closedir(d);
    return n;
}
/* <parent>/<name>, case-insensitive, directory only. */
static int child_named(const char *parent, const char *name, char *out, size_t outsz) {
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
static int root_has_stems(const char *root, char *out, size_t outsz) {
    char p[1024];
    if (child_named(root, "STEMS", p, sizeof p) && dir_has_entries(p)) {
        snprintf(out, outsz, "%s", p);
        return 1;
    }
    return 0;
}
static void rstrip(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t' ||
                 s[n - 1] == '/'))
        s[--n] = 0;
}
/* Locate a stems base dir: AZSTEM_STEMS_DIR env, then the azstem.stemsdir
 * control file (settings, then /tmp), then the mount roots one and two levels
 * deep (/media/usb/sdX, /run/media/<user>/<label>). Returns 1 and fills out. */
static int find_stems_base(char *out, size_t outsz) {
    const char *e = getenv("AZSTEM_STEMS_DIR");
    if (e && *e) {
        if (is_dir(e)) {
            snprintf(out, outsz, "%s", e);
            return 1;
        }
        if (root_has_stems(e, out, outsz))
            return 1;
    }
    FILE *cf = fopen("/home/root/settings/azstem.stemsdir", "re");
    if (!cf)
        cf = fopen("/tmp/azstem.stemsdir", "re");
    if (cf) {
        char b[1024];
        if (fgets(b, sizeof b, cf)) {
            rstrip(b);
            if (b[0]) {
                if (is_dir(b)) {
                    snprintf(out, outsz, "%s", b);
                    fclose(cf);
                    return 1;
                }
                if (root_has_stems(b, out, outsz)) {
                    fclose(cf);
                    return 1;
                }
            }
        }
        fclose(cf);
    }
    static const char *roots[] = {"/media/usb", "/run/media", "/mnt", "/media"};
    for (size_t r = 0; r < sizeof roots / sizeof roots[0]; r++) {
        if (root_has_stems(roots[r], out, outsz))
            return 1; /* base at root itself */
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
            if (root_has_stems(sub, out, outsz)) {
                closedir(d);
                return 1;
            } /* one level */
            DIR *d2 = opendir(sub);
            if (!d2)
                continue; /* two levels */
            struct dirent *de2;
            while ((de2 = readdir(d2))) {
                if (de2->d_name[0] == '.')
                    continue;
                char sub2[1600];
                snprintf(sub2, sizeof sub2, "%s/%s", sub, de2->d_name);
                if (is_dir(sub2) && root_has_stems(sub2, out, outsz)) {
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

/* Map a stem filename to a slot by substring: vocals=0, instruments/inst=1,
 * bass=2, drums/drum=3. Returns -1 if none match. */
static int slot_for_filename(const char *name) {
    if (strcasestr(name, "vocal"))
        return 0;
    if (strcasestr(name, "instrument") || strcasestr(name, "inst") || strcasestr(name, "other"))
        return 1;
    if (strcasestr(name, "bass"))
        return 2;
    if (strcasestr(name, "drum"))
        return 3;
    return -1;
}
static int has_ogg_ext(const char *name) {
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".ogg") == 0;
}

/* Decode one .ogg to a malloc'd interleaved stereo f32 buffer @44100. Mono is
 * duplicated; >2 channels keep the first two; other rates are linearly
 * resampled. max_frames > 0 decodes only the first max_frames output frames (the
 * drum roll's per-sample cap); 0 decodes the whole stream. Returns NULL on any
 * failure, including one to allocate the resample buffer: a native-rate buffer
 * returned as 44100 would play at the wrong pitch and speed. Off-RT. */
static float *decode_ogg_stereo_44100(const char *path, long *out_frames, long max_frames) {
    *out_frames = 0;
    int err = 0;
    stb_vorbis *v = stb_vorbis_open_filename(path, &err, NULL);
    if (!v)
        return NULL;
    stb_vorbis_info info = stb_vorbis_get_info(v);
    int ch = info.channels;
    unsigned sr = info.sample_rate;
    unsigned total = stb_vorbis_stream_length_in_samples(v); /* frames */
    if (ch < 1 || sr == 0 || total == 0 || total > (0x7fffffffu / (unsigned) (ch < 1 ? 1 : ch))) {
        stb_vorbis_close(v);
        return NULL;
    }
    if (max_frames > 0) {
        double cap = (double) max_frames * (double) sr / AZ_EP147_RATE + 2.0; /* native frames */
        if ((double) total > cap)
            total = (unsigned) cap;
    }
    float *native = malloc((size_t) total * (size_t) ch * sizeof(float));
    if (!native) {
        stb_vorbis_close(v);
        return NULL;
    }
    int got =
        stb_vorbis_get_samples_float_interleaved(v, ch, native, (int) (total * (unsigned) ch));
    stb_vorbis_close(v);
    if (got <= 0) {
        free(native);
        return NULL;
    }
    long nfr = got;

    /* -> interleaved stereo at the native rate */
    float *st = malloc((size_t) nfr * 2 * sizeof(float));
    if (!st) {
        free(native);
        return NULL;
    }
    for (long i = 0; i < nfr; i++) {
        float L, R;
        if (ch == 1) {
            L = R = native[i];
        } else {
            L = native[i * ch];
            R = native[i * ch + 1];
        }
        st[i * 2] = L;
        st[i * 2 + 1] = R;
    }
    free(native);

    /* -> 44100 (linear resample) if needed */
    if (sr != AZ_EP147_RATE && sr > 0) {
        double step = (double) sr / AZ_EP147_RATE;
        long rfr = (long) ((double) nfr * AZ_EP147_RATE / (double) sr);
        if (rfr < 1)
            rfr = 1;
        if (max_frames > 0 && rfr > max_frames)
            rfr = max_frames;
        float *rs = malloc((size_t) rfr * 2 * sizeof(float));
        if (!rs) { /* never hand back the native-rate buffer as 44100 */
            free(st);
            return NULL;
        }
        for (long t = 0; t < rfr; t++) {
            double sp = (double) t * step;
            long i0 = (long) sp;
            double f = sp - (double) i0;
            long i1 = i0 + 1;
            if (i1 >= nfr)
                i1 = nfr - 1;
            if (i0 >= nfr)
                i0 = nfr - 1;
            rs[t * 2] = st[i0 * 2] + (float) f * (st[i1 * 2] - st[i0 * 2]);
            rs[t * 2 + 1] = st[i0 * 2 + 1] + (float) f * (st[i1 * 2 + 1] - st[i0 * 2 + 1]);
        }
        free(st);
        st = rs;
        nfr = rfr;
    } else if (max_frames > 0 && nfr > max_frames) {
        nfr = max_frames; /* 44100 source: the extra decoded frames are not used */
    }
    *out_frames = nfr;
    return st;
}

/* Advance dk->stem_frames to the min frames decoded across present stems, after
 * every chunk. Monotonic and fenced so the RT hook never reads past PCM that is
 * written and locked. Off-RT. */
static void env_progress(int deck); /* progressive envelope, defined below */
static void advance_stem_frames(int deck) {
    deck_stems *dk = &g_dk[deck];
    unsigned present = atomic_load_explicit(&g_stem_present[deck], memory_order_relaxed);
    long m = -1;
    for (int k = 0; k < NSTEM; k++)
        if (present & (1u << k)) {
            long d = atomic_load_explicit(&g_stem_dec[deck][k], memory_order_relaxed);
            if (m < 0 || d < m)
                m = d;
        }
    if (m <= 0)
        return;
    pthread_mutex_lock(&g_adv_lock);
    if (!atomic_load_explicit(&g_gate_hold[deck], memory_order_acquire) && m > dk->stem_frames) {
        __sync_synchronize();
        dk->stem_frames = m;
    }
    pthread_mutex_unlock(&g_adv_lock);
    env_progress(deck); /* fill the envelope for the region now decoded */
}

/* Progressive decode of one stem into the deck's live buffer: at 44100, decode
 * in 0.5 s chunks, mlock and expose each as it lands. Other rates fall back to
 * the whole-file resampling decoder. On failure the slot leaves g_stem_present
 * so it stops gating the min. Off-RT. */
#define PROG_CHUNK (AZ_EP147_RATE / 2) /* 0.5 s */
typedef struct {
    int deck;
    int slot;
    char path[1600];
} pjob_t;
static void decode_stem_progressive(int deck, int slot, const char *path) {
    deck_stems *dk = &g_dk[deck];
    int err = 0;
    stb_vorbis *v = stb_vorbis_open_filename(path, &err, NULL);
    if (!v)
        goto fail;
    stb_vorbis_info info = stb_vorbis_get_info(v);
    int ch = info.channels;
    unsigned sr = info.sample_rate;
    unsigned total = stb_vorbis_stream_length_in_samples(v);
    if (ch < 1 || total == 0 || total > (0x7fffffffu / 2u)) {
        stb_vorbis_close(v);
        goto fail;
    }

    if (sr != AZ_EP147_RATE) {
        /* uncommon: full decode + resample via the existing path, then expose whole */
        stb_vorbis_close(v);
        long fr = 0;
        float *buf = decode_ogg_stereo_44100(path, &fr, 0);
        if (!buf || fr <= 0) {
            if (buf)
                free(buf);
            goto fail;
        }
        mlock(buf, (size_t) fr * 2 * sizeof(float));
        dk->stem_bytes[slot] = (size_t) fr * 2 * sizeof(float);
        dk->stem_mmap[slot] = 0;
        __sync_synchronize();
        dk->stem[slot] = buf;
        atomic_store_explicit(&g_stem_dec[deck][slot], fr, memory_order_relaxed);
        advance_stem_frames(deck);
        return;
    }

    /* Seed the progressive envelope with the final length and a uniform lane, so the
     * undecoded tail reads att = on_count/NSTEM, not silence: once per load (the first
     * stem), again if a later stem is longer. A cached envelope is whole and stays as it is. */
    pthread_mutex_lock(&g_envp_lock);
    if (!g_env_seeded[deck] || (!g_env_hit[deck] && (long) total > g_env_total[deck])) {
        g_env_seeded[deck] = 1;
        g_env_total[deck] = (long) total;
        g_env_filled[deck] = g_env_hit[deck] ? AZW_N : 0;
        if (!g_env_hit[deck])
            memset(g_env[deck], 128, sizeof g_env[deck]);
    }
    pthread_mutex_unlock(&g_envp_lock);
    long cap = (long) total;
    float *buf = malloc((size_t) cap * 2 * sizeof(float)); /* tail is never read past stem_frames */
    if (!buf) {
        stb_vorbis_close(v);
        goto fail;
    }
    dk->stem_bytes[slot] = (size_t) cap * 2 * sizeof(float);
    dk->stem_mmap[slot] = 0;
    __sync_synchronize();
    dk->stem[slot] = buf; /* stem_frames still 0 -> RT ignores it */

    float *tmp = malloc((size_t) PROG_CHUNK * (size_t) ch * sizeof(float));
    if (!tmp) {
        /* buf is already live in dk->stem[slot]: NULL it before dropping the slot,
         * or the RT hook sums garbage once the other stems open the gate. */
        dk->stem[slot] = NULL;
        __sync_synchronize();
        dk->stem_bytes[slot] = 0;
        free(buf);
        stb_vorbis_close(v);
        goto fail;
    }
    long done = 0;
    while (done < cap) {
        long want = cap - done;
        if (want > PROG_CHUNK)
            want = PROG_CHUNK;
        int got = stb_vorbis_get_samples_float_interleaved(v, ch, tmp, (int) (want * (long) ch));
        if (got <= 0)
            break;
        for (long i = 0; i < got; i++) {
            float L, R;
            if (ch == 1) {
                L = R = tmp[i];
            } else {
                L = tmp[i * ch];
                R = tmp[i * ch + 1];
            }
            buf[(done + i) * 2] = L;
            buf[(done + i) * 2 + 1] = R;
        }
        mlock(&buf[done * 2], (size_t) got * 2 * sizeof(float)); /* lock what the RT will read */
        __sync_synchronize();
        done += got;
        atomic_store_explicit(&g_stem_dec[deck][slot], done, memory_order_relaxed);
        advance_stem_frames(deck);
        if (got < want)
            break; /* short read = EOF */
        /* The track was replaced: stop here. The loader discards these buffers once every
         * decode thread has joined, and the next track's load waits for that join. */
        if (atomic_load_explicit(&g_load_stale[deck], memory_order_acquire))
            break;
    }
    free(tmp);
    stb_vorbis_close(v);
    return;
fail:
    atomic_fetch_and_explicit(&g_stem_present[deck], ~(1u << slot), memory_order_relaxed);
    advance_stem_frames(deck);
}
/* Drum-roll sampler: included here because it uses is_dir, logln and
 * decode_ogg_stereo_44100. */
#include "drumroll.c"

static void *dth_prog(void *arg) {
    pjob_t *j = arg;
    /* Below EP147's own normal-priority threads (its message thread draws the screen):
     * a load decodes every stem at once, and at nice 0 they took most of the free cores
     * from it for the whole decode. Decoding still runs well ahead of the playhead. */
    setpriority(PRIO_PROCESS, (id_t) syscall(SYS_gettid), 10);
    decode_stem_progressive(j->deck, j->slot, j->path);
    return NULL;
}

/* Match rank of a stems folder name against the key (loaded file's basename
 * minus extension); -1 = not this track's. Never a bare prefix: "Basic" must not
 * take "Basic (Extended Mix)"'s stems. Accepted forms, exactly what writers produce:
 *   0  <key>
 *   1  <key>_           a name the stick cannot store as written (trailing
 *                       dot/space, CON/NUL/...)
 *   2  <key>-xxxxxxxx   8-hex id: AZ-MODS, for a different song with the same file
 *                       name (told apart by source.json)
 * Case-insensitive, like FAT/exFAT. */
static int stem_folder_rank(const char *name, const char *key) {
    size_t kl = strlen(key);
    if (!kl || strncasecmp(name, key, kl))
        return -1;
    const char *t = name + kl;
    if (!*t)
        return 0;
    if (t[0] == '_' && !t[1])
        return 1;
    if (t[0] == '-' && strlen(t) == 9) {
        for (int i = 1; i < 9; i++)
            if (!isxdigit((unsigned char) t[i]))
                return -1;
        return 2;
    }
    return -1;
}

/* A JSON string value starting at `s` (just past the opening quote) into `out`
 * as UTF-8. Handles the escapes Python's json.dumps emits for a path (\" \\ \/
 * and \uXXXX, surrogate pairs included). 0 = anything else: not comparable. */
static int json_str(const char *s, char *out, size_t n) {
    size_t o = 0;
    for (; *s && *s != '"'; s++) {
        unsigned c = (unsigned char) *s;
        if (c == '\\') {
            s++;
            if (*s == '"' || *s == '\\' || *s == '/')
                c = (unsigned char) *s;
            else if (*s == 'u') {
                char h[5] = {0};
                if (strnlen(s + 1, 4) < 4) /* the string ends inside the escape */
                    return 0;
                memcpy(h, s + 1, 4);
                if (strspn(h, "0123456789abcdefABCDEF") != 4)
                    return 0;
                c = (unsigned) strtoul(h, NULL, 16);
                s += 4;
                if (c >= 0xd800 && c < 0xdc00) {
                    if (s[1] != '\\' || s[2] != 'u' || strnlen(s + 3, 4) < 4)
                        return 0;
                    memcpy(h, s + 3, 4);
                    if (strspn(h, "0123456789abcdefABCDEF") != 4)
                        return 0;
                    unsigned lo = (unsigned) strtoul(h, NULL, 16);
                    if (lo < 0xdc00 || lo >= 0xe000)
                        return 0;
                    c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
                    s += 6;
                }
                if (c == 0) /* \u0000 would cut the path short */
                    return 0;
                char u[4];
                int k;
                if (c < 0x80) {
                    u[0] = (char) c;
                    k = 1;
                } else if (c < 0x800) {
                    u[0] = (char) (0xc0 | c >> 6);
                    u[1] = (char) (0x80 | (c & 0x3f));
                    k = 2;
                } else if (c < 0x10000) {
                    u[0] = (char) (0xe0 | c >> 12);
                    u[1] = (char) (0x80 | (c >> 6 & 0x3f));
                    u[2] = (char) (0x80 | (c & 0x3f));
                    k = 3;
                } else {
                    u[0] = (char) (0xf0 | c >> 18);
                    u[1] = (char) (0x80 | (c >> 12 & 0x3f));
                    u[2] = (char) (0x80 | (c >> 6 & 0x3f));
                    u[3] = (char) (0x80 | (c & 0x3f));
                    k = 4;
                }
                if (o + k >= n)
                    return 0;
                memcpy(out + o, u, k);
                o += k;
                continue;
            } else
                return 0;
        }
        if (o + 1 >= n)
            return 0;
        out[o++] = (char) c;
    }
    if (*s != '"')
        return 0;
    out[o] = 0;
    return 1;
}

/* Cheap audio fingerprint: FNV-1a 64 over "<size>\n" plus 64 KiB at the start,
 * middle (size/2 - 32 KiB) and end; no decode. Size alone is not enough:
 * equal-length CBR files share it. Must match AZ-MODS library.fingerprint(). */
typedef struct {
    const char *path;
    long long size;
    int fp_state;
    unsigned long long fp;
} srcid_t;
static int file_fingerprint(srcid_t *s) {
    if (s->fp_state)
        return s->fp_state > 0;
    s->fp_state = -1;
    FILE *f = fopen(s->path, "rbe");
    if (!f)
        return 0;
    unsigned char *buf = malloc(1 << 16); /* heap, not TLS: EP147 has many threads */
    if (!buf) {
        fclose(f);
        return 0;
    }
    unsigned long long h = 0xcbf29ce484222325ULL;
    char hdr[32];
    int hl = snprintf(hdr, sizeof hdr, "%lld\n", s->size);
    for (int i = 0; i < hl; i++) {
        h ^= (unsigned char) hdr[i];
        h *= 0x100000001b3ULL;
    }
    long long half = s->size / 2 - (1 << 15), tail = s->size - (1 << 16);
    long long offs[3] = {0, half > 0 ? half : 0, tail > 0 ? tail : 0};
    for (int k = 0; k < 3; k++) {
        if (fseeko(f, (off_t) offs[k], SEEK_SET)) {
            free(buf);
            fclose(f);
            return 0;
        }
        size_t n = fread(buf, 1, 1 << 16, f);
        for (size_t i = 0; i < n; i++) {
            h ^= buf[i];
            h *= 0x100000001b3ULL;
        }
    }
    free(buf);
    fclose(f);
    s->fp = h;
    s->fp_state = 1;
    return 1;
}

/* source.json's "fp" value at q: exactly "fnv1a64:" and 16 hex digits in quotes, as AZ-MODS
 * writes it. strtoull alone would also take a sign, spaces or a 0x prefix. */
static int source_fp(const char *q, unsigned long long *fp) {
    if (strncmp(q, "\"fnv1a64:", 9))
        return 0;
    for (int i = 9; i < 25; i++)
        if (!isxdigit((unsigned char) q[i]))
            return 0;
    if (q[25] != '"')
        return 0;
    *fp = strtoull(q + 9, NULL, 16);
    return 1;
}

/* Which file a stems folder was made from. Two songs with the same file name in
 * different folders share a key, so the name alone cannot tell their stems
 * apart. AZ-MODS writes <folder>/source.json:
 *   {"file": "<path relative to the stick>", "size": <bytes>, "fp": "fnv1a64:<hex>", ...}
 * Returns  1  made from `src`: same path (ASCII case ignored) and size; else the
 *             fingerprint decides when present (equal = same audio at any path);
 *             else (no "fp") same size or same path.
 *          0  unknown: no source.json, no usable field, or `src` unreadable.
 *             Loads by name.
 *         -1  made from a different file: never load it.
 * Called only for folders whose name already matched. Off-RT.
 * Must match AZ-MODS library.same_source(). */
static int stem_folder_identity(const char *folder, srcid_t *src) {
    if (!src->path || !src->path[0] || src->size < 0)
        return 0;
    char p[1700];
    snprintf(p, sizeof p, "%s/source.json", folder);
    FILE *f = fopen(p, "re");
    if (!f)
        return 0;
    char b[4096];
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    int have_size = 0, have_file = 0, have_fp = 0;
    long long size = -1;
    unsigned long long fp = 0;
    const char *q = strstr(b, "\"size\"");
    if (q && (q = strchr(q + 6, ':'))) {
        char *e;
        size = strtoll(q + 1, &e, 10);
        have_size = e != q + 1;
    }
    char file[1024] = "";
    q = strstr(b, "\"file\"");
    if (q && (q = strchr(q + 6, ':'))) {
        q++;
        while (*q == ' ')
            q++;
        if (*q == '"' && !json_str(q + 1, file, sizeof file))
            return 0; /* present but unreadable */
        have_file = *q == '"' && file[0];
    }
    q = strstr(b, "\"fp\"");
    if (q && (q = strchr(q + 4, ':'))) {
        q++;
        while (*q == ' ')
            q++;
        have_fp = source_fp(q, &fp);
    }
    int same_path = 0;
    if (have_file) {
        size_t fl = strlen(file), sl = strlen(src->path);
        same_path =
            sl > fl && src->path[sl - fl - 1] == '/' && !strcasecmp(src->path + sl - fl, file);
    }
    if (same_path && have_size && size == src->size)
        return 1;
    if (have_fp) {
        if (!file_fingerprint(src))
            return 0;
        return src->fp == fp ? 1 : -1;
    }
    if ((have_size && size == src->size) || same_path)
        return 1;
    return (have_size || have_file) ? -1 : 0;
}

/* Cached name lists of the stems bases, used only by azstem_has_stems so the
 * browser's stem marks are fast: rereading a ~670-entry FAT directory cost
 * 10-15 ms per track. A list is re-read when the directory's mtime/size changes
 * (FAT bumps it on add/remove) or after 60 s. Deck loads always read the
 * directory directly. */
#define NL_BASES 8
static struct {
    char base[1024];
    long long sig;
    long t;
    int n;
    char **names;
} g_nl[NL_BASES];
static pthread_mutex_t g_nl_mu = PTHREAD_MUTEX_INITIALIZER;
static long nl_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}
/* g_nl_mu held. The cached names of `base`, or NULL when it cannot be read. */
static char **nl_get(const char *base, int *n) {
    struct stat st;
    if (stat(base, &st))
        return 0;
    long long sig = (long long) st.st_mtime * 1000003LL + (long long) st.st_size;
    int k = -1, old = 0;
    for (int i = 0; i < NL_BASES; i++) {
        if (!strcmp(g_nl[i].base, base)) {
            k = i;
            break;
        }
        if (g_nl[i].t < g_nl[old].t)
            old = i;
    }
    if (k >= 0 && g_nl[k].sig == sig && nl_now() - g_nl[k].t < 60) {
        *n = g_nl[k].n;
        return g_nl[k].names;
    }
    if (k < 0)
        k = old;
    for (int i = 0; i < g_nl[k].n; i++)
        free(g_nl[k].names[i]);
    free(g_nl[k].names);
    g_nl[k].names = 0;
    g_nl[k].n = 0;
    g_nl[k].base[0] = 0;
    DIR *d = opendir(base);
    if (!d)
        return 0;
    int cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.')
            continue;
        if (g_nl[k].n == cap) {
            int nc = cap ? cap * 2 : 256;
            char **nn = realloc(g_nl[k].names, (size_t) nc * sizeof *nn);
            if (!nn)
                break;
            g_nl[k].names = nn;
            cap = nc;
        }
        if (!(g_nl[k].names[g_nl[k].n] = strdup(de->d_name)))
            break;
        g_nl[k].n++;
    }
    closedir(d);
    snprintf(g_nl[k].base, sizeof g_nl[k].base, "%s", base);
    g_nl[k].sig = sig;
    g_nl[k].t = nl_now();
    *n = g_nl[k].n;
    return g_nl[k].names;
}
static int find_track_folder_any_c(const char *track, const char *src, char *folder, size_t sz,
                                   int *ident, int cached);
static int find_track_folder_any(const char *track, const char *src, char *folder, size_t sz,
                                 int *ident) {
    return find_track_folder_any_c(track, src, folder, sz, ident, 0);
}
/* Find the stems folder for `track` in every mounted stick's STEMS/.
 * A folder made from a different file is never taken (stem_folder_identity).
 * Best candidate wins by, in order:
 *   1. made from this file (source.json) before unknown;
 *   2. on the same stick as the track's file `src`, when known;
 *   3. match rank (stem_folder_rank).
 * Read-only. Off-RT. Returns 1 and fills `folder`. */
static int find_track_folder_any_c(const char *track, const char *src, char *folder, size_t sz,
                                   int *ident, int cached) {
    folder[0] = 0;
    *ident = 0;
    if (!track || !track[0])
        return 0;
    static const char *sroots[] = {"/media/usb", "/run/media", "/mnt", "/media"};
    int best = 1 << 30;
    struct stat sst;
    srcid_t sid = {src, (src && src[0] && stat(src, &sst) == 0) ? (long long) sst.st_size : -1, 0,
                   0};
    if (sid.size < 0)
        logln("ogg: identity of '%s' unchecked (file '%s' unknown)", track, src ? src : "");
    for (size_t r = 0; r < sizeof sroots / sizeof sroots[0]; r++) {
        DIR *rd = opendir(sroots[r]);
        if (!rd)
            continue;
        struct dirent *rde;
        while ((rde = readdir(rd))) {
            if (rde->d_name[0] == '.')
                continue;
            char stick[512];
            snprintf(stick, sizeof stick, "%s/%s", sroots[r], rde->d_name);
            size_t sl = strlen(stick);
            int other = !(src && !strncmp(src, stick, sl) && src[sl] == '/');
            char base[1024];
            if (!child_named(stick, "STEMS", base, sizeof base))
                continue;
            DIR *d = 0;
            struct dirent *de;
            char **nl = 0;
            int nn = 0, ni = 0;
            if (cached) {
                pthread_mutex_lock(&g_nl_mu);
                nl = nl_get(base, &nn);
                if (!nl) {
                    pthread_mutex_unlock(&g_nl_mu);
                    continue;
                }
            } else if (!(d = opendir(base)))
                continue;
            for (;;) {
                const char *dn;
                if (cached) {
                    if (ni >= nn)
                        break;
                    dn = nl[ni++];
                } else {
                    if (!(de = readdir(d)))
                        break;
                    dn = de->d_name;
                }
                if (dn[0] == '.')
                    continue;
                int rank = stem_folder_rank(dn, track);
                if (rank < 0)
                    continue;
                char p[1600];
                snprintf(p, sizeof p, "%s/%s", base, dn);
                if (!is_dir(p))
                    continue;
                int id = stem_folder_identity(p, &sid);
                if (id < 0) {
                    logln("ogg: reject folder '%s' for '%s': made from a different file", p,
                          track);
                    continue;
                }
                int score = (id ? 0 : 1000) + other * 100 + rank * 10;
                if (score >= best)
                    continue;
                best = score;
                *ident = id;
                snprintf(folder, sz, "%s", p);
            }
            if (cached)
                pthread_mutex_unlock(&g_nl_mu);
            else
                closedir(d);
        }
        closedir(rd);
    }
    /* g_stems_base covers a base outside the mount roots above (env / control file) */
    if (!folder[0] && g_stems_base[0]) {
        DIR *d = 0;
        struct dirent *de;
        char **nl = 0;
        int nn = 0, ni = 0, ok;
        if (cached) {
            pthread_mutex_lock(&g_nl_mu);
            nl = nl_get(g_stems_base, &nn);
            ok = nl != 0;
            if (!ok)
                pthread_mutex_unlock(&g_nl_mu);
        } else
            ok = (d = opendir(g_stems_base)) != 0;
        if (ok) {
            for (;;) {
                const char *dn;
                if (cached) {
                    if (ni >= nn)
                        break;
                    dn = nl[ni++];
                } else {
                    if (!(de = readdir(d)))
                        break;
                    dn = de->d_name;
                }
                if (dn[0] == '.')
                    continue;
                int rank = stem_folder_rank(dn, track);
                if (rank < 0)
                    continue;
                char p[1600];
                snprintf(p, sizeof p, "%s/%s", g_stems_base, dn);
                if (!is_dir(p))
                    continue;
                int id = stem_folder_identity(p, &sid);
                if (id < 0) {
                    logln("ogg: reject folder '%s' for '%s': made from a different file", p, track);
                    continue;
                }
                int score = (id ? 0 : 1000) + rank * 10;
                if (score < best) {
                    best = score;
                    *ident = id;
                    snprintf(folder, sz, "%s", p);
                }
            }
            if (cached)
                pthread_mutex_unlock(&g_nl_mu);
            else
                closedir(d);
        }
    }
    return folder[0] != 0;
}

/* Exported (dlsym) for azmod's browser stems mark: would this file load stems?
 * Uses the same rules as a deck load, so the mark cannot disagree with playback.
 * 1 = folder found, 0 = none, -1 = file missing (unknown).
 * Blocking filesystem work: call from a worker, never a paint or RT path. */
int azstem_has_stems(const char *abs_path);
int azstem_has_stems(const char *abs_path) {
    if (!abs_path || abs_path[0] != '/' || access(abs_path, F_OK) != 0)
        return -1;
    char key[256];
    const char *base = strrchr(abs_path, '/') + 1;
    snprintf(key, sizeof key, "%s", base);
    char *dot = strrchr(key, '.');
    if (dot)
        *dot = 0;
    char folder[1024];
    int ident;
    return find_track_folder_any_c(key, abs_path, folder, sizeof folder, &ident, 1)
               ? 1
               : 0; /* cached name lists */
}

/* Exported for azmod's stem pads and tiles: which stems the deck's track has,
 * bit k = slot k (0 vocals, 1 other, 2 bass, 3 drums), i.e. found and not failed
 * to decode. 0 = unknown or none; the caller then hides nothing. Atomic loads
 * only: safe from any thread. */
unsigned azstem_present(int deck);
unsigned azstem_present(int deck) {
    if (deck < 0 || deck >= NDECK)
        return 0;
    /* The mask is not cleared on eject or track change, so trust it only while
     * stems are available (the same test azmod's strips use). */
    if (!atomic_load_explicit(&g_stem_pending[deck], memory_order_acquire) &&
        g_dk[deck].stem_frames <= 0)
        return 0;
    return atomic_load_explicit(&g_stem_present[deck], memory_order_relaxed) & 0xFu;
}

/* Millisecond clock for per-phase load timing (logln stamps whole seconds). */
static long ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}
static void ofs_load_measure(int deck, const char *track, const char *src,
                             unsigned long long skey); /* ofsload.c */
/* Identity of a stem set for the offset cache: size and mtime of each present stem file,
 * in slot order (FNV-1a). 0 = a file could not be read, and nothing is cached. */
static unsigned long long stems_key(char paths[NSTEM][1600]) {
    unsigned long long h = 1469598103934665603ULL;
    for (int k = 0; k < NSTEM; k++) {
        long long v[3] = {k, -1, -1};
        if (paths[k][0]) {
            struct stat st;
            if (stat(paths[k], &st) != 0)
                return 0;
            v[1] = (long long) st.st_size;
            v[2] = (long long) st.st_mtime;
        }
        const unsigned char *b = (const unsigned char *) v;
        for (size_t i = 0; i < sizeof v; i++)
            h = (h ^ b[i]) * 1099511628211ULL;
    }
    return h ? h : 1;
}
/* RAM that must remain free after a deck's stems are allocated. */
#ifndef AZSTEM_MEM_FLOOR_MB
#define AZSTEM_MEM_FLOOR_MB 400
#endif
/* MemAvailable in bytes, -1 if unreadable (stems are then refused). */
static long long mem_available_bytes(void) {
    FILE *f = fopen("/proc/meminfo", "re");
    if (!f)
        return -1;
    char ln[128];
    long long kb = -1;
    while (fgets(ln, sizeof ln, f))
        if (sscanf(ln, "MemAvailable: %lld kB", &kb) == 1)
            break;
    fclose(f);
    return kb < 0 ? -1 : kb << 10;
}
/* Find the stems folder for this deck's track, check the memory floor, then
 * decode its .ogg stems progressively on parallel threads. Returns 1 when stems
 * are playable, 0 otherwise (the caller frees and shows no stems). Off-RT, on a
 * loader thread. */
static int load_stems_ogg(int deck, const char *track, const char *src) {
    if (!track || !track[0])
        return 0;
    long t_entry = ms_now(), t_folder = 0, t_cache = 0, t_scan = 0, t_dec = 0;

    /* Search every mounted stick: a track's stems may live on another USB. */
    char folder[1024];
    int ident;
    if (!find_track_folder_any(track, src, folder, sizeof folder, &ident)) {
        logln("ogg: no folder for '%s' under any USB stems base", track);
        return 0;
    }
    t_folder = ms_now();
    logln("ogg: matched folder '%s' for '%s' (file '%s') identity=%s  [folder search %ldms]",
          folder, track, src ? src : "?", ident ? "this-file" : "unknown", t_folder - t_entry);
    /* Publish a cached envelope before decoding so the waveform is live at once. */
    snprintf(g_env_dir[deck], sizeof g_env_dir[deck], "%s", folder);
    int env_hit = env_cache_load(deck);
    t_cache = ms_now();
    struct dirent *de; /* reused by the ogg-file scan below */

    t_scan = ms_now();
    /* map the 4 .ogg files to slots */
    char paths[NSTEM][1600];
    for (int k = 0; k < NSTEM; k++)
        paths[k][0] = 0;
    DIR *fd = opendir(folder);
    if (!fd)
        return 0;
    while ((de = readdir(fd))) {
        /* Skip dot files before the extension test: macOS writes AppleDouble
         * sidecars on exFAT (`._vocals.ogg`), which pass both tests below, and
         * the first readdir match per slot wins, so a slot could bind to a 4 KB
         * metadata stub and play nothing. */
        if (de->d_name[0] == '.') {
            logln("ogg: skip dotfile '%s'", de->d_name);
            continue;
        }
        if (!has_ogg_ext(de->d_name)) {
            logln("ogg: skip non-ogg '%s'", de->d_name);
            continue;
        }
        int slot = slot_for_filename(de->d_name);
        logln("ogg: file '%s' -> slot %d", de->d_name, slot);
        if (slot >= 0 && !paths[slot][0])
            snprintf(paths[slot], sizeof paths[slot], "%s/%s", folder, de->d_name);
    }
    closedir(fd);

    /* Memory floor, checked before any allocation: decoded f32 stems are mlock'd
     * (8 B/frame/stem), and four long tracks can exhaust a 3.85 GB deck. Not
     * enough room -> the track plays without stems. This deck's current stems
     * count as free (released before the decode). Two decks loading at once each
     * see the other's untouched malloc pages as free; the floor is the margin. */
    {
        long long need = 0, held = 0, avail = mem_available_bytes();
        for (int k = 0; k < NSTEM; k++) {
            held += (long long) g_dk[deck].stem_bytes[k];
            if (!paths[k][0])
                continue;
            int err = 0;
            stb_vorbis *v = stb_vorbis_open_filename(paths[k], &err, NULL);
            if (!v)
                continue;
            stb_vorbis_info in = stb_vorbis_get_info(v);
            long long fr = stb_vorbis_stream_length_in_samples(v);
            if (in.sample_rate > 0 && in.sample_rate != AZ_EP147_RATE)
                fr = fr * AZ_EP147_RATE / in.sample_rate;
            stb_vorbis_close(v);
            need += fr * 2 * (long long) sizeof(float);
        }
        if (avail < 0 || avail + held - need < ((long long) AZSTEM_MEM_FLOOR_MB << 20)) {
            logln("stems REFUSED d%d for '%s': need %lld MB, MemAvailable %lld MB (+%lld MB held), "
                  "floor %d MB -- playing without stems",
                  deck, track, need >> 20, avail >> 20, held >> 20, AZSTEM_MEM_FLOOR_MB);
            return 0;
        }
    }

    /* Publish availability now: the decode below takes seconds and azmod's
     * strips must not wait for it. */
    {
        int any = 0;
        unsigned found = 0;
        for (int k = 0; k < NSTEM; k++)
            if (paths[k][0]) {
                any = 1;
                found |= 1u << k;
            }
        /* Mask before pending: azstem_present() trusts the mask whenever pending
         * is set, so this load's pending must never pair with the old mask. */
        atomic_store_explicit(&g_stem_present[deck], found, memory_order_relaxed);
        atomic_store_explicit(&g_stem_pending[deck], any, memory_order_release);
        if (any)
            publish_deck_file(deck, "avail", "1");
    }

    /* Drop the old stems first (RT passes native audio meanwhile), then decode
     * each present stem in parallel (RK3399: 6 cores). */
    free_stems(deck);
    /* free_stems unpublishes the envelope; a cached one loaded above is this track's, so it
     * stays published through the decode, and the decode leaves it as it is. */
    pthread_mutex_lock(&g_envp_lock);
    g_env_seeded[deck] = 0;
    g_env_hit[deck] = env_hit;
    pthread_mutex_unlock(&g_envp_lock);
    if (env_hit)
        atomic_store_explicit(&g_env_ready[deck], 1, memory_order_release);
    deck_stems *dk = &g_dk[deck];
    unsigned present = 0;
    for (int k = 0; k < NSTEM; k++) {
        atomic_store_explicit(&g_stem_dec[deck][k], 0, memory_order_relaxed);
        if (paths[k][0])
            present |= (1u << k);
    }
    atomic_store_explicit(&g_stem_present[deck], present, memory_order_relaxed);
    if (!present) {
        atomic_store_explicit(&g_stem_pending[deck], 0, memory_order_relaxed);
        publish_deck_file(deck, "avail", "0");
        logln("stems: no .ogg slots in '%s'", folder);
        return 0;
    }
    /* Hold the RT gate until the load-time offset is known (ofsload.c): decode
     * proceeds, but stem_frames stays 0 and EP147's own audio plays meanwhile. */
    atomic_store_explicit(&g_gate_hold[deck], 1, memory_order_release);
    {
        pjob_t job[NSTEM];
        pthread_t th[NSTEM];
        int nth = 0;
        for (int k = 0; k < NSTEM; k++) {
            if (!paths[k][0])
                continue;
            job[nth].deck = deck;
            job[nth].slot = k;
            snprintf(job[nth].path, sizeof job[nth].path, "%s", paths[k]);
            if (az_thread_create(&th[nth], NULL, dth_prog, &job[nth]) != 0)
                decode_stem_progressive(deck, k, paths[k]); /* thread exhausted: decode inline */
            else
                nth++;
        }
        ofs_load_measure(deck, track, src, stems_key(paths)); /* always releases the hold */
        for (int i = 0; i < nth; i++)
            pthread_join(th[i], NULL);
    }
    t_dec = ms_now();
    for (int k = 0; k < NSTEM; k++)
        logln("ogg: decode slot %d -> %s frames=%ld", k, dk->stem[k] ? "ok" : "(none)",
              atomic_load_explicit(&g_stem_dec[deck][k], memory_order_relaxed));
    long minfr = dk->stem_frames; /* advanced progressively during the decode above */
    if (minfr <= 0) {
        free_stems(deck);
        atomic_store_explicit(&g_stem_pending[deck], 0, memory_order_relaxed);
        publish_deck_file(deck, "avail", "0");
        logln("stems: no decodable .ogg in '%s'", folder);
        return 0;
    }
    /* A replaced track's decode stops early: its envelope would describe part of the track,
     * so it is neither built nor cached. The loader discards the buffers. */
    if (atomic_load_explicit(&g_load_stale[deck], memory_order_acquire))
        return 0;
    atomic_store_explicit(&g_stem_pending[deck], 0, memory_order_relaxed);
    /* Full-buffer peak lanes, every decode joined. Not after a cache hit: the cached
     * envelope is already this one, and a rebuild unpublishes it while it runs. */
    if (!env_hit)
        build_env(deck);
    {
        long t_env = ms_now();
        logln("PHASES d%d: folder=%ldms cache=%ldms scan=%ldms decode=%ldms env=%ldms TOTAL=%ldms",
              deck, t_folder - t_entry, t_cache - t_folder, t_scan ? t_scan - t_cache : 0,
              t_dec - (t_scan ? t_scan : t_cache), t_env - t_dec, t_env - t_entry);
    }
    return 1;
}


/* Defined with the LoadedTrackInfo code below. */
static int g_lti_on = 0; /* native route authoritative (azstem.nativeid) */
static int lti_native_name(int deck, char *out, size_t n);

/* Loaded-track name for `deck` (basename minus extension), or NULL if unknown.
 * Native LoadedTrackInfo route when armed (the copy taken inside EP147's own update);
 * otherwise, in development builds, the path scan's track_path, and for deck 1 only the
 * azstem.track control file. Off-RT (flusher). */
static const char *get_loaded_track_name(int deck) {
    static char name[NDECK][256];
    char *nm = name[deck];
    nm[0] = 0;
    /* When armed, the native route is authoritative: unknown means no stems for
     * this deck. Never fall through to the scan, which can answer with another
     * deck's track; the scan is only a whole-route mode for when the native route
     * is absent. */
    if (g_lti_on) {
        if (lti_native_name(deck, nm, 256))
            return nm;
        return NULL;
    }
    const char *tp = g_dk[deck].track_path;
    /* track_path is refreshed each tick by the probe. */
    if (tp[0]) {
        const char *base = strrchr(tp, '/');
        base = base ? base + 1 : tp;
        snprintf(nm, 256, "%s", base);
        char *dot = strrchr(nm, '.');
        if (dot)
            *dot = 0; /* strip extension */
        if (dev_flag("/home/root/settings/azstem.vdbg"))
            logln("GLTN d%d path='%s' name='%s'", deck, tp, nm);
    }
    if (!nm[0] && deck == 0) { /* Deck 1 fallback: azstem.track control file */
        FILE *f = fopen("/home/root/settings/azstem.track", "re");
        if (!f)
            f = fopen("/tmp/azstem.track", "re");
        if (f) {
            if (fgets(nm, 256, f)) {
            }
            fclose(f);
            rstrip(nm);
        }
    }
    return nm[0] ? nm : NULL;
}

/* Replace a control file azmod polls: write <path>.new, rename it over <path>, so
 * azmod never reads the empty moment of an in-place rewrite (an empty curtrack reads
 * as "no track", and the next read as a track change that resets the deck's stem
 * toggles). Not fsynced: azstem re-publishes these at every start. 0 = written. */
static int publish_write(const char *path, const char *val) {
    char tmp[200];
    if (snprintf(tmp, sizeof tmp, "%s.new", path) >= (int) sizeof tmp)
        return -1;
    FILE *f = fopen(tmp, "we");
    if (!f)
        return -1;
    fputs(val, f);
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}
/* Publish a per-deck control file (avail / curtrack). Failures are logged: on a
 * full settings partition a silent failure means azmod never sees the track
 * change and never resets the stem toggles. */
static void publish_deck_file(int deck, const char *stem, const char *val) {
    char pth[160];
    /* Suffix: deck 0 = "", decks 1..3 = "2".."4", matching STEMS_PATH[] and
     * azmod's STEMS_CTRL_PATH[]. */
    char suf[12] = ""; /* room for any int: the compiler cannot see deck <= 3 */
    if (deck > 0)
        snprintf(suf, sizeof suf, "%d", deck + 1);
    snprintf(pth, sizeof pth, "/home/root/settings/azstem.%s%s", stem, suf);
    char line[600];
    snprintf(line, sizeof line, "%s\n", val);
    if (publish_write(pth, line) != 0)
        logln("publish FAILED %s (disk full?)", pth);
}

/* Publish the loaded file per deck to /tmp/azstem.cursrcN for deck-side tools
 * (e.g. deciding whether a stick can be replugged). EP147 keeps no fd open on a
 * loaded track, so /proc/<pid>/fd cannot answer this. /tmp because it is live
 * state that must not survive a boot. Written only on change. Empty = no track
 * or unknown; readers must treat that as unknown on a deck showing a track. */
static char g_loaded_src[NDECK][600]; /* tentative decl; defined with its notes below */
static void publish_src(int deck) {
    static char last[NDECK][600];
    static int init[NDECK];
    if (deck < 0 || deck >= NDECK)
        return;
    if (init[deck] && strcmp(last[deck], g_loaded_src[deck]) == 0)
        return;
    char pth[64], tmp[72];
    snprintf(pth, sizeof pth, "/tmp/azstem.cursrc%d", deck + 1);
    snprintf(tmp, sizeof tmp, "%s.new", pth);
    FILE *f = fopen(tmp, "we");
    if (!f)
        return;
    fprintf(f, "%s\n", g_loaded_src[deck]);
    if (fclose(f) != 0 || rename(tmp, pth) != 0)
        return;
    snprintf(last[deck], sizeof last[deck], "%s", g_loaded_src[deck]);
    init[deck] = 1;
}

/* EP147 starts with every deck empty, but avail/curtrack live on p7 and survive
 * a restart. azmod primes its track-change detector from them, so reloading the
 * pre-restart track would read as "unchanged" and keep stale stem mutes. Reset
 * them at startup. azstem.stems* is azmod's toggle mask, not ours to clear. */
static void clear_published_deck_state(void) {
    for (int d = 0; d < NDECK; d++) {
        publish_deck_file(d, "curtrack", "");
        publish_deck_file(d, "avail", "0");
        publish_src(d);
    }
}

/* Decode a deck's newly-matched song off the flusher thread (see stems_tick). */
typedef struct {
    int deck;
    char track[256];
    char src[600];
} loadjob_t;
static void *loader_thread(void *arg) {
    loadjob_t *j = arg;
    int ok = load_stems_ogg(j->deck, j->track, j->src) > 0;
    if (atomic_load_explicit(&g_load_stale[j->deck], memory_order_acquire)) {
        /* Track replaced mid-load. The decode threads have joined, so the buffers
         * may be freed now (and only now); stems_tick then loads the new track. */
        int d = j->deck;
        free_stems(d);
        logln("stems d%d for '%s' DISCARDED: the track changed during the load", d, j->track);
        free(j);
        pthread_mutex_lock(&g_adv_lock);
        atomic_store_explicit(&g_load_stale[d], 0, memory_order_release);
        atomic_store_explicit(&g_gate_hold[d], 0, memory_order_release);
        pthread_mutex_unlock(&g_adv_lock);
        atomic_store_explicit(&g_load_again[d], 1, memory_order_release);
        atomic_store_explicit(&g_stem_loading[d], 0, memory_order_release);
        return NULL;
    }
    if (ok) {
        snprintf(g_dk[j->deck].loaded_track, sizeof g_dk[j->deck].loaded_track, "%s", j->track);
        logln("stems loaded d%d for '%s': frames=%ld", j->deck, g_dk[j->deck].loaded_track,
              g_dk[j->deck].stem_frames);
    } else {
        free_stems(j->deck);
        snprintf(g_dk[j->deck].loaded_track, sizeof g_dk[j->deck].loaded_track, "%s", j->track);
        logln("no stems d%d for '%s' -- passthrough", j->deck, g_dk[j->deck].loaded_track);
    }
    int d = j->deck; /* read before the free: j is not valid after it */
    free(j);
    atomic_store_explicit(&g_stem_loading[d], 0, memory_order_relaxed);
    return NULL;
}

/* Eject debounce: consecutive empty-name ticks while a track was loaded.
 * ~200 ms/tick, so EJECT_EMPTY_TICKS (8) ~= 1.6 s. */
static int g_eject_empty[NDECK] = {0};
/* Candidate new track per deck, awaiting a second confirming tick (stems_tick). */
static char g_pending_name[NDECK][512];
/* The file behind loaded_track and the pending name. The name alone is not the
 * track: two songs in different folders can share it, and A's stems would keep
 * playing under B. */
static char g_loaded_src[NDECK][600];
static char g_pending_src[NDECK][600];
/* A track identified while no stems base was known: load its stems once one is found. */
static int g_load_deferred[NDECK];
#define EJECT_EMPTY_TICKS 8
/* The deck's track identity changed: a confirmed new track or an eject. Every per-deck
 * consumer keyed on the generation (azmod's stem toggle reset, the drum roll's grid, the
 * auto key-shift FGPR mapping) must see both, so both paths come through here. */
static void deck_track_changed(int deck) {
    atomic_fetch_add_explicit(&g_deck_trackgen[deck], 1, memory_order_relaxed);
}
static int lti_read(int deck, unsigned char id16[16], char *title, size_t tn, char *artist,
                    size_t an);

/* Drop a deck's stems from stems_tick. With a load in flight its decode threads
 * are still writing the buffers, so freeing here would be a use-after-free:
 * close the gate, mark the load stale, and let loader_thread free after join. */
static void drop_stems(int deck) {
    if (!atomic_load_explicit(&g_stem_loading[deck], memory_order_acquire)) {
        free_stems(deck);
        return;
    }
    deck_stems *dk = &g_dk[deck];
    pthread_mutex_lock(&g_adv_lock);
    atomic_store_explicit(&g_load_stale[deck], 1, memory_order_release);
    atomic_store_explicit(&g_gate_hold[deck], 1, memory_order_release);
    dk->stem_frames = 0;
    __sync_synchronize();
    pthread_mutex_unlock(&g_adv_lock);
    atomic_store_explicit(&dk->stem_off, 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_ofs_epoch[deck], 1, memory_order_release);
    atomic_store_explicit(&g_env_ready[deck], 0, memory_order_relaxed);
    logln("stems_tick: d%d track replaced while its stems load -- gate closed, loader will free",
          deck);
}

static int lti_native_path(int deck, char *out, size_t n); /* LoadedTrackInfo route, below */
/* Full path of the deck's file. Same authority rule as get_loaded_track_name:
 * when the native route is armed it is the only answer; empty = unknown. */
static void loaded_track_path(int deck, char *out, size_t n) {
    out[0] = 0;
    if (g_lti_on) {
        if (!lti_native_path(deck, out, n))
            out[0] = 0;
        return;
    }
    snprintf(out, n, "%s", g_dk[deck].track_path);
}

static void start_load(int deck, const char *tr) {
    loadjob_t *j = malloc(sizeof *j);
    if (!j)
        return;
    j->deck = deck;
    snprintf(j->track, sizeof j->track, "%s", tr);
    loaded_track_path(deck, j->src, sizeof j->src);
    atomic_store_explicit(&g_stem_loading[deck], 1, memory_order_relaxed);
    pthread_t th;
    if (az_thread_create(&th, NULL, loader_thread, j) == 0)
        pthread_detach(th);
    else {
        loader_thread(j);
    } /* thread exhausted: decode inline */
}

static void stems_tick(void) {
    /* Discover the stems base at most every ~5 s: with no stems present the walk
     * can cover a large library, and this runs every 200 ms. Cached once found. */
    if (!g_stems_base[0]) {
        static int discover_skip = 0;
        if (discover_skip <= 0) {
            find_stems_base(g_stems_base, sizeof g_stems_base);
            discover_skip = 25;
        } else
            discover_skip--;
    }
    int vdbg = dev_flag("/home/root/settings/azstem.vdbg");
    for (int deck = 0; deck < NDECK; deck++) {
        deck_stems *dk = &g_dk[deck];
        if (vdbg) {
            void *sf = atomic_load_explicit(&g_deck_self[STEM_SLOT[deck]], memory_order_relaxed);
            logln("STICK d%d base='%s' self=%p frames=%ld gl='%s'", deck, g_stems_base, sf,
                  dk->stem_frames, dk->loaded_track);
        }
        /* No stems base: track identity still runs below, since the drum roll's grid,
         * the auto key-shift mapping and azmod's toggle reset key on the generation
         * whether or not the stick has stems. Only stem loading waits for a base. */
        int have_base = g_stems_base[0] != 0;
        /* no USB tree: emulator f32 fallback (Deck 1 only) */
        if (!have_base && deck == 0 && dk->stem_frames == 0) {
            load_stems_f32(0);
            if (dk->stem_frames > 0)
                logln("stems(f32 fallback) loaded: frames=%ld", dk->stem_frames);
        }
        const char *tr = get_loaded_track_name(deck);
        if (!tr || !tr[0]) {
            /* Empty name = a transient probe miss or a real eject. A miss recovers
             * within a tick or two; ~1.6 s empty while a track was loaded is an
             * eject: free the stems and clear availability, which removes azmod's
             * stem indicators. */
            if (dk->loaded_track[0] && ++g_eject_empty[deck] >= EJECT_EMPTY_TICKS) {
                logln("stems_tick: d%d EJECT (name empty %d ticks) -- free + clear avail", deck,
                      g_eject_empty[deck]);
                if (have_base)
                    drop_stems(deck);
                atomic_store_explicit(&g_stem_pending[deck], 0, memory_order_relaxed);
                g_load_deferred[deck] = 0;
                dk->loaded_track[0] = 0;
                g_loaded_src[deck][0] = 0;
                deck_track_changed(deck); /* generation + identity: ejected */
                /* Clear the published name too, or reloading the same track
                 * reads as "unchanged" to azmod and its toggles never reset. */
                publish_deck_file(deck, "curtrack", "");
                publish_src(deck);
                g_eject_empty[deck] = 0;
            }
            continue; /* unknown/ejected: nothing more to do this tick */
        }
        g_eject_empty[deck] = 0; /* a real name resets the eject debounce */
        /* Same name and same file = same track. An unreadable path (either side
         * empty) compares by name alone, so it never causes reload churn. */
        char src[600];
        loaded_track_path(deck, src, sizeof src);
        int same_file = !src[0] || !g_loaded_src[deck][0] || strcmp(src, g_loaded_src[deck]) == 0;
        if (strcmp(tr, dk->loaded_track) == 0 && same_file) {
            g_pending_name[deck][0] = 0;
            if (!g_loaded_src[deck][0] && src[0]) { /* path readable only now: record it */
                snprintf(g_loaded_src[deck], sizeof g_loaded_src[deck], "%s", src);
                publish_src(deck);
            }
            /* a load discarded because the track changed under it: start this track's */
            if (!atomic_load_explicit(&g_stem_loading[deck], memory_order_acquire) &&
                atomic_exchange_explicit(&g_load_again[deck], 0, memory_order_acq_rel))
                start_load(deck, tr);
            /* identified before a stems base existed: the base is here now */
            else if (have_base && g_load_deferred[deck] &&
                     !atomic_load_explicit(&g_stem_loading[deck], memory_order_acquire)) {
                g_load_deferred[deck] = 0;
                start_load(deck, tr);
            }
            continue;
        }

        /* Confirm a change across two ticks before acting on it. On the scan
         * route, probe_strings takes the first audio path within two pointer
         * levels of the deck object, which can transiently be another deck's
         * track. Requiring the same new name on two consecutive ~200 ms ticks
         * rejects that at a cost of one tick. The native route (g_lti_on) does
         * not have this problem. */
        if (strcmp(tr, g_pending_name[deck]) != 0 || strcmp(src, g_pending_src[deck]) != 0) {
            snprintf(g_pending_name[deck], sizeof g_pending_name[deck], "%s", tr);
            snprintf(g_pending_src[deck], sizeof g_pending_src[deck], "%s", src);
            logln("stems_tick: d%d track change '%s' -> '%s' PENDING (needs 2nd tick)", deck,
                  dk->loaded_track, tr);
            continue; /* unconfirmed: act on it next tick */
        }
        g_pending_name[deck][0] = 0;
        logln("stems_tick: d%d track changed '%s' -> '%s'", deck, dk->loaded_track, tr);
        if (!strcmp(tr, dk->loaded_track))
            logln("stems_tick: d%d same name, different file '%s' -> '%s'", deck,
                  g_loaded_src[deck], src);
        snprintf(g_loaded_src[deck], sizeof g_loaded_src[deck], "%s", src);
        /* before the publish below: azmod must see the new name and the new generation
         * together, or it resets the deck's stem toggles twice */
        deck_track_changed(deck);
        if (!have_base) {
            /* identity only: publish it, and load the stems if a base turns up */
            publish_deck_file(deck, "curtrack", tr);
            publish_src(deck);
            snprintf(dk->loaded_track, sizeof dk->loaded_track, "%s", tr);
            g_load_deferred[deck] = 1;
            continue;
        }
        g_load_deferred[deck] = 0;

        /* Drop the old stems now, or the RT hook keeps rendering them against the
         * new track's playhead until the new decode lands. Without buffers it
         * passes native audio through. drop_stems, not free_stems: a load may
         * still be writing these buffers. */
        drop_stems(deck);
        atomic_store_explicit(&g_stem_pending[deck], 0, memory_order_relaxed);
        publish_deck_file(deck, "avail", "0");

        /* Publish before the in-flight-load check: azmod resets the deck's toggles
         * to all-on when this file changes. */
        publish_deck_file(deck, "curtrack", tr);
        publish_src(deck);
        snprintf(dk->loaded_track, sizeof dk->loaded_track, "%s", tr);
        /* A load still in flight was just marked stale by drop_stems; the loader
         * raises g_load_again when it has freed, and the branch above restarts. */
        if (atomic_load_explicit(&g_stem_loading[deck], memory_order_relaxed))
            continue;
        /* Decode on a worker: blocking here would stall every deck's polling. */
        start_load(deck, tr);
    }
}
static _Atomic int g_installed = 0; /* 1 ok, -1 declined, 0 not tried */

/* Slot for this audio unit, by identity only: a DjPlayerUnit goes in its own
 * deck's slot (seeded by its ctor or read via ap_deck_of). A unit without a
 * readable identity (AudioRecorderUnits, or moved offsets) gets -1 and every
 * caller leaves it untouched. Lock-free. */
static int deck_slot(void *self) {
    for (int i = 0; i < MAX_DECKS; i++)
        if (atomic_load_explicit(&g_deck_self[i], memory_order_relaxed) == self)
            return i;
    {
        int d = ap_deck_of(self);
        if (d >= 0 && d < NDECK) {
            int s = STEM_SLOT[d];
            void *exp = NULL;
            if (atomic_compare_exchange_strong_explicit(
                    &g_deck_self[s], &exp, self, memory_order_relaxed, memory_order_relaxed)) {
                atomic_fetch_or_explicit(&g_ap_seen, 1u << d, memory_order_relaxed);
                return s;
            }
            if (atomic_load_explicit(&g_deck_self[s], memory_order_relaxed) == self) {
                atomic_fetch_or_explicit(&g_ap_seen, 1u << d, memory_order_relaxed);
                return s;
            }
            /* This deck's slot is already held by a different object: record the
             * conflict (it is logged) and treat the unit as unknown. */
            atomic_fetch_or_explicit(&g_ap_conflict, 1u << d, memory_order_relaxed);
        }
    }
    /* Unknown: never mixed, muted or measured as some other deck. */
    return -1;
}

/* ---- decode-level fileRead hook ----------------------------------------- */
static fread_fn g_orig_fread = NULL;
static uintptr_t g_fread_addr = 0;
static _Atomic int g_frobs_on = 0, g_frinject_on = 0;
struct frobs {
    void *self;
    long long a1, a3;
    int ret;
    float s0L, s0R, sNL, sNR;
};
static struct frobs g_frring[128];
static _Atomic unsigned g_frw = 0;
static unsigned g_frr = 0;

static int fread_hook(void *self, long long a1, Float2 *dst, long long a3) {
    int ret = g_orig_fread(self, a1, dst, a3);
    /* Test-tone / stems injection at raw-decode level, so the SRC key-shifts it. */
    if (atomic_load_explicit(&g_frinject_on, memory_order_relaxed) && dst) {
        long long n = a3;
        if (n < 1)
            n = a1;
        if (n < 1)
            n = 0;
        if (n > 100000)
            n = 100000;
        deck_stems *dk = &g_dk[0];
        unsigned scc = atomic_load_explicit(&dk->ctrl, memory_order_relaxed);
        long nf = dk->stem_frames;
        unsigned em = scc & 0x0Fu;
        int tone = atomic_load_explicit(&g_testtone_on, memory_order_relaxed);
        float sg[NSTEM];
        stem_gains(0, sg);
        long long base = a1; /* assume a1 = source sample position */
        if (!tone && stems_play_original(dk, em, sg))
            n = 0; /* all stems on: the original, as in pbread_hook */
        for (long long i = 0; i < n; i++) {
            long long idx = base + i;
            float L = 0.f, R = 0.f;
            if (tone) {
                float c = 1000.0f * (float) idx / (float) AZ_EP147_RATE;
                c -= floorf(c);
                L = R = 0.30f * sinf(6.2831853f * c);
            } else if ((scc & 0x80u) && nf > 0 && idx >= 0 && idx < nf) {
                for (int k = 0; k < NSTEM; k++)
                    if ((em & (1u << k)) && dk->stem[k]) {
                        const float *g = dk->stem[k];
                        L += sg[k] * g[idx * 2];
                        R += sg[k] * g[idx * 2 + 1];
                    }
            }
            dst[i].L = L;
            dst[i].R = R;
        }
    }
    if (atomic_load_explicit(&g_frobs_on, memory_order_relaxed)) {
        unsigned w = atomic_fetch_add_explicit(&g_frw, 1, memory_order_relaxed) & 127u;
        struct frobs *e = &g_frring[w];
        e->self = self;
        e->a1 = a1;
        e->a3 = a3;
        e->ret = ret;
        long long n = a3;
        if (n < 1)
            n = a1;
        if (n < 1)
            n = 1;
        if (n > 100000)
            n = 100000;
        if (dst) {
            e->s0L = dst[0].L;
            e->s0R = dst[0].R;
            e->sNL = dst[n - 1].L;
            e->sNR = dst[n - 1].R;
        }
    }
    return ret;
}
static void frobs_flush(void) {
    atomic_store_explicit(&g_frobs_on, dev_flag(FROBS_PATH), memory_order_relaxed);
    atomic_store_explicit(&g_frinject_on, dev_flag(FRINJECT_PATH), memory_order_relaxed);
    unsigned w = atomic_load_explicit(&g_frw, memory_order_acquire);
    if (!atomic_load_explicit(&g_frobs_on, memory_order_relaxed)) {
        g_frr = w;
        return;
    }
    int em = 0;
    while (g_frr != w && em < 12) {
        struct frobs *e = &g_frring[g_frr & 127u];
        logln("FRREAD self=%p a1=%lld a3=%lld ret=%d s0=%.4f,%.4f sN=%.4f,%.4f", e->self, e->a1,
              e->a3, e->ret, e->s0L, e->s0R, e->sNL, e->sNR);
        g_frr++;
        em++;
    }
    if ((unsigned) (w - g_frr) > 128u)
        g_frr = w;
}
