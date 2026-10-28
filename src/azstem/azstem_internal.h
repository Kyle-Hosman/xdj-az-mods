/* SPDX-License-Identifier: GPL-3.0-only */
/* Declarations shared across the azstem translation unit. */
#ifndef AZSTEM_INTERNAL_H
#define AZSTEM_INTERNAL_H

struct fgscope;

#define NSTEM 4

#define NDECK 4 /* stem-capable decks: Deck 1..4 */

/* Everything the stems engine tracks for one deck (one per NDECK). */
typedef struct {
    const float *stem[NSTEM]; /* interleaved stereo @44100 f32, or NULL */
    size_t stem_bytes[NSTEM]; /* byte length (for munlock/free) */
    int stem_mmap[NSTEM];     /* 1 = mmap'd (f32 fallback), 0 = malloc'd (ogg) */
    long stem_frames;         /* usable frames (min across stems); RT gate */
    _Atomic unsigned ctrl;    /* 0x80 engaged | bits0-3 enabled stems */
    char loaded_track[256];   /* song folder currently decoded ("" = none) */
    char track_path[512];     /* loaded-track path, from the probe hand-off */
    /* sync PLL state (RT-thread-only) */
    double cur;              /* stem cursor (source frames) */
    long prevS;              /* last playhead value (probe/diag only) */
    long anchorS;            /* playhead high-water mark (last forward reference) */
    int sinceA;              /* output frames since the last new high */
    double rate;             /* EMA frames-per-output-frame (pitch), signed */
    double rd_next, rd_rate; /* where the previous block's read would have
                              * continued; used to declick a jump */
    int rd_valid;
    int paused;            /* 1 = no new high for PLL_PAUSE_LATENCY -> mute */
    _Atomic long stem_off; /* measured stem offset (samples): the injection plays
                            * stem[source index - stem_off]. 0 = unmeasured. */
} deck_stems;

#define AZW_N 65536

static unsigned char g_env[NDECK][NSTEM][AZW_N];

static _Atomic int g_env_ready[NDECK];

static long g_env_total[NDECK];

static int g_env_filled[NDECK];

static int g_env_seeded[NDECK];

static int g_env_hit[NDECK];

static pthread_mutex_t g_envp_lock;

static char g_env_dir[NDECK][1024];

static int env_cache_load(int deck);

static void build_env(int deck);

static double eq_knob_gain(int v10);

static inline void stem_gains(int deck, float *sg);

static inline int stems_play_original(const deck_stems *dk, unsigned em, const float *sg);

static void free_stems(int d);

static void load_stems_f32(int d);

void azstem_drum_bpm(int side, float bpm);

void azstem_drum_follow(int side, int lead);

void azstem_drum_select(int side, int slot);

void azstem_drum_rate(int side, int idx);

void azstem_drum_gate(int side, int on);

void azstem_drum_touch(int side, int on);

static void publish_deck_file(int deck, const char *stem, const char *val);

typedef struct {
    float L, R;
} Float2;

static _Atomic int g_fgprtag_on;

static _Atomic long g_fgprtag_hits;

static _Atomic long g_fgprtag_miss;

struct fgscope {
    uintptr_t save_ctx;
    int save_deck;
};

static inline void fg_enter(struct fgscope *sc, uintptr_t self);

static inline void fg_leave(struct fgscope *sc);

static _Atomic int g_testtone_on;

#define FROBS_PATH "/home/root/settings/azstem.frobs"

#define FRINJECT_PATH "/home/root/settings/azstem.frinject"

static _Atomic long long g_inj_start, g_inj_cnt, g_inj_adv;

static _Atomic long long g_play_S[NDECK];

static _Atomic long long g_gap_min, g_gap_max, g_gap_absmin;

static _Atomic uintptr_t g_ra_caller;

static _Atomic long long g_op_pos;

static _Atomic long g_pitch_bufpos, g_pitch_buflim;

static _Atomic uintptr_t g_op_caller;

static _Atomic unsigned g_pbread_hits;

typedef int (*fread_fn)(void *self, long long a1, Float2 *dst, long long a3);

static int fread_hook(void *self, long long a1, Float2 *dst, long long a3);

static _Atomic int g_nativekey_subst;

static _Atomic int g_operate_hits;

static _Atomic int g_key_ratio_bits;

static _Atomic uintptr_t g_pitch_self;

static _Atomic long g_ts_src, g_ts_vtbl, g_ts_buf, g_ts_ws, g_ts_fill;

static _Atomic int g_ts_cap;

/* Evict-on-toggle: while pitched, resident pages still hold the old stem mask,
 * so a toggle would lag ~10 s. EP147's clearBufferedSource (PageBuffer vtable
 * +0xa8 wrapper @0x783fe0; async, posts to the PageFiller worker, so off-RT safe)
 * evicts them and they reload through our PageBuffer::read injection with the new
 * mask. Gated by EVICTKEY_PATH: absent = off; content '0' passes flag=0, else 1. */
#define EVICTKEY_PATH "/home/root/settings/azstem.evictkey"

#define PB_CLEARSRC_ADDR EP_783fe0

#define PB_VTABLE EP_25f9c00

typedef void *(*pb_clearsrc_fn)(void *out, long pb, void *srcid, unsigned char flag);

static _Atomic unsigned g_evict_req;

static _Atomic int g_evict_calls;

static _Atomic(unsigned long long) g_evict_sid0, g_evict_sid1;

#define FGPR_SRC_OFF 0x162998UL /* FGPR+this = raw source object pointer */

#define FGPR_RATIO_OFF 0x448UL  /* FGPR+this = key ratio float (1.0 = no shift) */

static _Atomic uintptr_t g_fgpr_self;

static _Atomic uintptr_t g_fgpr_srcobj;

static _Atomic uintptr_t g_fgpr_readfn;

static _Atomic int g_fgpr_setsrc_hits;

static _Atomic int g_fgpr_read_hits;

static _Atomic uintptr_t g_fgpr_read_self;

#define FGPR_VTABLE EP_27ba108
/* DjPlayerUnit -> its KeyControlFGPR, all built inside the DjPlayerUnit ctor (key_shift.c). */
#define AK_VT_PBU EP_289f798 /* PlayerBasisUnit */
#define AK_VT_MGR EP_27b8a50 /* CascadedTimeStretchManager */
#define AK_SUB 0xe0UL        /* mgr+0x48 -> IKeyController sub-iface; FGPR = ptr - 0xe0 */
#define IDMAP_PBU_OFF 0x180UL /* DjPlayerUnit -> PlayerBasisUnit (ctor 0x14a2560, param_1[0x30]) */
#define IDMAP_MGR_OFF 0x10UL  /* PlayerBasisUnit -> CascadedTimeStretchManager */
#define IDMAP_SUB_OFF 0x48UL  /* mgr -> IKeyController sub-iface */

#define NPROBE 8

static uintptr_t g_probe_addr[NPROBE];

static int g_probe_installed[NPROBE];

static _Atomic long g_probe_hits[NPROBE];

static _Atomic uintptr_t g_rs_caller;

static _Atomic long g_rs_pos, g_rs_size;

static _Atomic long g_pbread_tls_set, g_pbread_tls_unset;

#endif /* AZSTEM_INTERNAL_H */
