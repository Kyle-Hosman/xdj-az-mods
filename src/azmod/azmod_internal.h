/* SPDX-License-Identifier: GPL-3.0-only */
/* Declarations shared across the azmod translation unit: included only by azmod.c (a unity build),
 * so its statics are single variables. Never include it from menu.c or another separate TU. */

/* EP147's own deck-view mode (WaveformView +0x3a8: 0 = 2 DECK, 1 = 2/4 DECK, 2 = 4 DECK),
 * published by pl_tick on the message thread; -1 until the view has painted once. */
static _Atomic int g_wv_mode = -1;
#ifndef AZMOD_INTERNAL_H
#define AZMOD_INTERNAL_H

struct fx_img;
struct pl_addtgt;
struct plsnap;
struct sm_ent;
struct sm_tprep;
struct sm_w;

static int g_pad_mode_slip[AZMOD_STEM_DECKS];

static _Atomic int g_drum_arm;

static int g_drum_held[AZMOD_STEM_DECKS];

static int g_drum_hold[AZMOD_STEM_DECKS];
#define DRUM_HOLD_ON 1      /* pad 7 HOLD is on for this deck */
#define DRUM_HOLD_SUSTAIN 2 /* the X-PAD has played a roll under the hold */

static _Atomic unsigned g_caption_repaint;

static volatile unsigned g_drum_sel_led[AZMOD_STEM_DECKS];

static void (*g_drum_gate_fn)(int, int);

static void (*g_drum_gate_fn)(int, int);

static void (*g_drum_select_fn)(int, int);

static void (*g_drum_select_fn)(int, int);

static void (*g_drum_rate_fn)(int, int);

static void (*g_drum_rate_fn)(int, int);

static void (*g_drum_touch_fn)(int, int);

static void (*g_drum_touch_fn)(int, int);

static void (*g_drum_tone_fn)(int, int);

static void (*g_drum_tone_fn)(int, int);

static _Atomic int g_mic2_tone_tlm;

static _Atomic int g_stemeq_knobs[2];

static int g_xpad_pos_off;

static int g_xpad_touch_off;

static unsigned g_xpad_touch_msk;

static int g_xpad_span;

static int g_xpad_zone_off;

/* Beat FX arm bit: byte 0x54 bit 0x80 of the 128-byte input frame (beyond the 40-byte
 * INPUT MON window). Measured:
 *     button on/off/on/off : 54:04->84  54:84->04  54:04->84  54:84->04
 *     x-pad touch          : 54:04->84  (plus 55 and 57)
 * The strip asserts the same bit the Beat FX button does: the panel sends "arm FX"
 * outright, so swallowing the position byte and zone bitmap alone cannot suppress it.
 * 0x55 increments per zone crossing and 0x57 mirrors the position; both are
 * strip-derived, so they are restored from the untouched snapshot too. */
#define XPAD_FX_OFF 0x54

#define XPAD_FX_MSK 0x80

static _Atomic int g_xpad_raw;

static volatile int g_erp_active[AZMOD_STEM_DECKS];

static unsigned g_erp_last[AZMOD_STEM_DECKS];

static unsigned g_erp_mode_last[AZMOD_STEM_DECKS];

static int g_pad_mode_bl[AZMOD_STEM_DECKS];

static inline int stems_bl_mode(void);

static _Atomic int g_bl_vpage2[AZMOD_STEM_DECKS];

static inline int bl_swap(void);

static inline int bl_page2(int deck);

static _Atomic unsigned g_mode_seq;

static _Atomic unsigned g_mode_edge_seq[AZMOD_PAD_DECKS];

static _Atomic long g_mode_edge_ms[AZMOD_PAD_DECKS];

static _Atomic int g_mode_down[AZMOD_PAD_DECKS];

static int pad_mode_fresh(int side, int ld);

static _Atomic int g_bl_enter[AZMOD_PAD_DECKS];

static int stems_page(int side, int ld);

static int g_deck_sel[AZMOD_PAD_DECKS];

static unsigned g_sel_last[AZMOD_PAD_DECKS];

static volatile unsigned g_dbg_modebyte[AZMOD_STEM_DECKS];

static volatile unsigned g_dbg_padbits[AZMOD_STEM_DECKS];

static volatile unsigned g_dbg_padrise[AZMOD_STEM_DECKS];

static volatile unsigned g_dbg_reqraised[AZMOD_STEM_DECKS];

#define ERP_LED_PAD_OFF 30 /* byte offset of the 8x4 pad block in the LED frame */

#define ERP_LED_MODE_OFF 0x0e

#define MODE_J_BEATLOOP 1

#define BEATLOOP_LED_G 0xff

static _Atomic int g_bl_btn_green[2];

static struct seqblk g_pad_led[AZMOD_STEM_DECKS];

static uint32_t g_mode_rgbw[AZMOD_PADMODE_COUNT];

static uint32_t g_mode2_rgbw[AZMOD_PADMODE_COUNT];

static volatile unsigned char g_padcell_tlm[8][3];

static volatile unsigned char g_modecell_tlm[4][3];

static void load_config(azmod_state *s);

/* Row-rate probe (one-shot, `touch /tmp/azmod-bandrate.req`): new frames per second
 * shown by each screen row. Samples full-width one-pixel rows (rows, not columns: a thin
 * scrolling tick changes its row on every update but a column only when it crosses it).
 * A change counts as a frame only once held >= 4 ms, so half-drawn states do not count.
 * Writes /tmp/azmod-bandrate.txt; blocks the overlay thread 3 s (diagnostic only). */
#define BR_MAXROWS 128

static int read_modupdate(azmod_state *s);

static void write_modupdate_req(int want);

/* savelogs.c: System Info > Save Logs to USB */
static int read_savelogs(azmod_state *s);
static void savelogs_request(azmod_state *s);

static _Atomic long g_wave_seen_ms;

static _Atomic long g_viewleave_n;

static int read_avail(azmod_state *s);

static int reset_stems_on_track_change(azmod_state *s);

static void read_changelog(azmod_state *s);

/* inline_hooks.c: azm_install_hook(addr, hook, &orig) records the call site's hook expression
 * in the install table (/tmp/azmod-hooktable) */
static int azm_install_hook_at(uintptr_t FUNC, void *hook, void **orig, const char *name);
#define azm_install_hook(f, h, o) azm_install_hook_at((f), (h), (o), #h)
static void azm_hooks_go_late(void);
static void azm_hooks_dump(const char *path);
#define AZM_HOOKTABLE_PATH "/tmp/azmod-hooktable"

typedef void (*src_setvis_fn)(void *c, int visible);

static volatile int g_srcrow_open;

static volatile int g_srcrow_close;

static volatile int g_srcrow_n;

static volatile int g_srcrow_want;

static volatile int g_src_visible;

static _Atomic int g_menu_top;

static _Atomic int g_srcrow_repaint;

static _Atomic int g_srcrow_unfocus;

static _Atomic int g_srcrow_pin_ours;

static _Atomic unsigned g_drain_calls;

static void menu_drop_intent(void);

static int srcrow_trace_on(void);

#define SRCROW_TRACE(budget, ...)                                                    \
    do {                                                                             \
        static _Atomic int _b = (budget);                                            \
        if (srcrow_trace_on() && __atomic_fetch_sub(&_b, 1, __ATOMIC_RELAXED) > 0) { \
            FILE *_lf = azlog_open("/tmp/azmod-srcrow.log", "a");                    \
            if (_lf) {                                                               \
                fprintf(_lf, __VA_ARGS__);                                           \
                fclose(_lf);                                                         \
            }                                                                        \
        }                                                                            \
    } while (0)

static volatile int g_tbl_cand_n;

static void *g_tbl;

static volatile int g_slw_ctor_ok;

/* Pinned rows: pinned menu items appear as SOURCE-list rows below MOD MENU while the menu is
 * closed. They are synthetic (indices past numUsed, tap-only: the knob clamps at EP147's
 * table) and limited to rows with a value (az_pinnable). Gated on azmod-pinrows.on. */
#define PINROWS_FLAG "/home/root/settings/azmod-pinrows.on"

static src_setvis_fn g_o_shortcut_setvis;

static _Atomic unsigned g_kn_eaten;

static void install_srcrow_hook(void);

#include "ep_mem.h" /* own_q / img_q / own_str / wref: how azmod reads EP147's memory */
static int install_compdtor_hook(void); /* azmod.c: juce::Component destructor -> wref */
static void ep_img_init(void);          /* azmod.c: EP147 image bounds for img_q */

#define WAVE_SWITCH EP_1df6010  /* pad-mode -> which child widget is visible */

#define WAVE_MODEGET EP_1fa7a28 /* pad-mode enum getter: FUN_01fa7a28(*(this+0xd0)) */

#define BJ_TILE_RA \
    EP_1deb250 /* modeget's return address in PadBeatJumpWidget's refresh 0x1deb208 */

#define TITLE_UPDATE EP_1df3ae0

#define TITLE_UPDATE_END EP_1df4908

#define PADMODESTATUS_VT EP_2d8d300

#define BPMSTATE_LIVE 0xa0       /* double: tempo-adjusted BPM */

static unsigned long g_bpmstate[AZMOD_STEM_DECKS]; /* stems_display.c: __atomic access only */
static int bpmstate_read(int d, double *live, double *track); /* stems_display.c: 1 = alive, deck d's */

static void install_bpmstate_hook(void);
static _Atomic int g_master_deck; /* stems_display.c: master deck 0..3, -1 = none / unknown */
static void install_master_hook(void);
static float drum_feed_bpm(int d, double live, int master, int master_ok, double master_live);
static int drum_feed_lead(int d, double live, int master, int master_ok, double master_live);

static _Atomic int g_ui60_on;

static _Atomic int g_wave60_on;

/* Record the Timer object's vtable, not just lr: the ~33 ms timers re-arm from inside
 * their own timerCallback via a tail call, so lr is always the generic TimerThread
 * dispatch site. The vtable resolves to a class name offline via RTTI. */
static struct {
    uintptr_t vt;
    long lr;
    int ms;
    long n;
} g_tmr[96];

static int g_tmr_n;

static pthread_mutex_t g_tmr_lk;

static _Atomic long g_mdut_self;

static _Atomic long g_mdut_fi_raw;

static int scroll60_on(void);

static _Atomic long g_tick_iv;

static _Atomic long g_disp_n;

static _Atomic int g_disp_seen, g_disp_want, g_disp_armed, g_disp_self_lo;

static _Atomic int g_disp_ints[40];

static long g_disp_bt[10];

static int g_disp_btn;

static _Atomic long g_disp_pass;

static _Atomic long g_feed_frame_n, g_feed_pass_n;

static int feedlock_on(void);

static void apply_ui60(int on);

static _Atomic int g_visdbg;

/* A community mod's surface holds the browse knob, its click and BACK (sdk_surface.c sets it on
 * the overlay thread; the mixer read path freezes and swallows for it as for the Mod Menu). BACK
 * edges seen while it is set go to g_sdk_back_pending, not to the menu. */
static _Atomic int g_sdk_focus;
static _Atomic int g_sdk_back_pending;
static _Atomic int g_browseview_shown;

static void (*g_orig_waveswitch)(long);

static void (*g_orig_wavector)(long, long *, unsigned);

static volatile int g_wave_forcestems;

static volatile int g_force_truemode;

static int dg_slip_enum;

static _Atomic int g_true_mode;

static int drum_mode_is_slip(int d, int ld);

static void drum_disarm_all(const char *why);

static void drum_disarm_others(int keep, const char *why);

static _Atomic long g_stem_container[AZMOD_STEM_DECKS];

static _Atomic long g_stem_model;

/* Deck identity by stored field: per-deck offsets of the mode object and tile ds. */
#define PERDECK_MODEOBJ 0x10b0L

#define PERDECK_TILEDS 0x1160L

static int deck_for_modeobj(long p);

static _Atomic long g_tile_w[AZMOD_STEM_DECKS][8];

static _Atomic int g_cap_budget;

#define PERDECK_STRIDE 0x1400L

static int perdeck_table_ready(void);

static int deck_for_perdeck_field(long p, long off);

#define PADTILE_VT EP_2d8f880 /* PadStatusWidget vtable; stems_display.c */
#define SLOT7_MAX 4           /* four grids observed; extra ones recycle entry 0 */

static _Atomic long g_slot7_w[SLOT7_MAX];

static _Atomic long g_padgrid_ds[SLOT7_MAX];

static _Atomic long g_slip_tile[AZMOD_STEM_DECKS][8];

#define SLIP_WIDGET_VT EP_2d8d4d8                     /* gui::waveform::PadSlipLoopWidget (RTTI) */

static int (*g_orig_modeget)(long);

static int (*g_orig_modeget)(long);

#define PAD_REPAINT \
    EP_2488570 /* repaint(ImagesComponent*): the call the                                           \
                * refresh makes right after writing the                                            \
                * index; the write alone draws nothing */

#define IMAGESCOMP_VT EP_29e9540 /* gui::ImagesComponent primary vtable */

/* The SLOT-7 tile's child is not an ImagesComponent. By RTTI (vt-8 -> typeinfo, +8 ->
 * name; EP147 is non-PIE, file offset = VA - 0x400000):
 *     0x2d8f880 = gui::waveform::PadStatusWidget   (the tile itself)
 *     0x2d84b60 = gui::waveform::PadCueWidget      (its [+0xf0] child)
 *     0x29e9540 = gui::ImagesComponent             (what the stems path uses)
 * So the drum path has its own type check; the stems check (comp_is_valid) rejects these
 * tiles. Both classes are juce::Components, which is all juce::Component::repaint()
 * requires. */
#define PADCUE_VT EP_2d84b60 /* gui::waveform::PadCueWidget (RTTI) */

static int slot7_repaint_all(void);

#define FXDIAG_MAX 32


#include "fxart.h"

#include "stems4art.h" /* 4-DECK source-cell stem strips (generated art) */

/* Stem state on the jog display (jog_display.c, jog_stems_logic.h), per deck, atomics only:
 * g_jog_src = jog_word(engaged, 1, stem mask), written by the overlay thread;
 * g_jog_page = the deck's pads are its stem toggles, written by the ErP LED path. */
#include "jog_stems_logic.h"
static unsigned g_jog_src[AZMOD_STEM_DECKS];
static int g_jog_page[AZMOD_STEM_DECKS];
static void install_jogstems(void);
static void jogstems_tick(void);              /* picture style: decode, then the timing log */
static void jog_tx_note(int bus, const void *arg); /* picture style: time artwork transfers */

static int g_fxroll_art_ready;

static _Atomic int g_fxrow_zone;

/* The sret ABI: juce::Image is an 8-byte object but non-trivially constructed, so the
 * loader returns it through memory: caller-provided buffer in x8, data in x0, size in w1.
 * A >16-byte struct return makes GCC use exactly that ABI; passing the buffer as an
 * explicit parameter would put it in x0 and crash. */
struct fx_img {
    char c[17];
};

static void (*fx_img_assign)(void *dst, const void *src);

static struct fx_img g_fxart_name[AZMOD_STEM_DECKS];

static void fxrow_restore(long self);

static void fxrow_apply_tick(void);

static int tilew_alive(long tw);

static void stem_tiles_apply(int deck);

static double g_ncol_hint;

static int g_four_deck;

static void wave_zoom_tick(void);

static _Atomic int g_ov_refresh;

static long g_erp_crc_ok, g_erp_crc_bad;

static unsigned g_root_pass;

static _Atomic long g_upd_skipped;

static long g_up_ns[AZMOD_STEM_DECKS], g_up_n[AZMOD_STEM_DECKS], g_up_max[AZMOD_STEM_DECKS];

static long g_up_gap_ns[AZMOD_STEM_DECKS];

static long g_up_bt[AZMOD_STEM_DECKS][12];

static int g_up_btn[AZMOD_STEM_DECKS];

static _Atomic int g_bg_move_ms, g_bg_ramp_ms;

static _Atomic int g_bg_gain_k, g_bg_od_k, g_bg_gray;

static _Atomic long g_bg_steps, g_bg_builds;

static void brightgrid_poll_cfg(void);

static int g_gridaa_hooked;

static _Atomic long g_gridaa_rows, g_gridaa_calls, g_gridaa_ns;

static int g_wp_fast;

static int g_wp_cmp;

static unsigned long g_wpc_n[3], g_wpc_bad[3];

static int g_wave_lanes;

static int g_wave_noid;

static int g_ws_forcepp;

static _Atomic long g_ws_n[3], g_ws_rns[3];

static _Atomic long g_ws_ns, g_ws_builds, g_ws_resync, g_ws_nodeck;

static int (*g_stem_frames)(int deck);

static int (*g_stem_frames)(int deck);

static void wave_prov_report(void);

static int g_attzero;

static int build_atten(int deck, const int gq[AZMOD_STEM_COUNT]);

static void ov_forget_all(void);

static void wave_repaint_if_needed(void);

static void anlz_log(const char *fmt, ...);


static int azm_patch_vtable(uintptr_t slot_addr, void *hook, void **orig);

#define BDD_VT EP_2dcc3a0

#define BDD_ROWS 0x80

#define BDD_ROWCOUNT 0x90

#define BDD_TRACKINFO 0x78

#define BROWSE_FILLPANE EP_1f45660

static __thread int g_in_grabcell;

static int g_rowprobe;

static _Atomic int g_drag_on;

#define DRAG_KIND_PLIST 1 /* a playlist/folder row in the PLAYLIST category list */

static _Atomic int g_drag_kind;

static _Atomic int g_drag_into;

static _Atomic int g_drag_into_disp;

static _Atomic int g_drag_into_dir;

static _Atomic int g_drag_into_note;

static _Atomic int g_tl_showing;

static _Atomic int g_drag_rows;

static _Atomic int g_drag_origin;

static _Atomic int g_drag_target;

static _Atomic int g_drag_committing;

/* How the open drag ends: the bus read thread records it, the message tick performs it
 * (drag_handoff_drain). */
#include "drag_handoff.h"
static struct dh_slot g_dh = DH_SLOT_INIT;
static void drag_handoff_drain(void);

static _Atomic int g_knob_held;

static _Atomic int g_shift_down;

static _Atomic long long g_knob_press_ms;

static _Atomic int g_knob_raw;

static _Atomic int g_mp_submode, g_mp_subback;

static int drag_remap(int display_row);

static int drag_is_widget(uintptr_t w);

static int drag_remap_any(int display_row);

static void drag_clear(void);

static void drag_request_update(void);

static _Atomic unsigned long g_hl_widget;

static _Atomic int g_hl_rows, g_hl_sel;

static _Atomic int g_reorder_enabled;

static int g_reorder_forced_off;

static int g_reorder_hooks;

static long long drag_now_ms(void);

static void drag_log(const char *fmt, ...);

struct plsnap {
    unsigned playlist_id, content_id, row, media; /* playlist_id 0 = not eligible */
    unsigned sort_block; /* a USB playlist's tracks, but not "#" ascending */
    /* ADD, from any browse list (0 = none): the selected track row, and
     * the selected PLAYLIST row on the playlist list. Same media rule. */
    unsigned any_cid, any_row, sel_pl, any_media;
    unsigned sel_folder; /* sel_pl is a FOLDER row (kind 1), not a list */
    char sel_title[96];  /* the selected row's displayed name (record +0x18) */
    unsigned sel_row;    /* the display row of sel_pl in the playlist list (record +4) */
    unsigned open_pl;    /* the playlist whose track list is open, any sort */
    /* the Browse FOLDER view (category 17): the selected entry, relative to its stick, and the
     * folder a PASTE there lands in (the selected folder, or a selected file's folder) */
    unsigned fsel_media;
    unsigned fsel_dir_id; /* the folder entry id of fsel_dir (0 = the stick's top, or unknown) */
    int fsel_kind;        /* 1 a folder, 4 a file, 0 none */
    char fsel_path[512], fsel_dir[512];
};

static struct plsnap plsnap_get(void);

static _Atomic long long g_lsfind_ms;

enum {
    MV_OP_MOVE,
    MV_OP_REMOVE,
    MV_OP_ADD,
    MV_OP_TARGET,
    MV_OP_CPREAD,
    MV_OP_RSREAD,
    MV_OP_NEWPL,
    MV_OP_PLCOPY /* same-stick playlist -> target folder */,
    MV_OP_PLMOVE /* same-stick playlist re-parented into the target folder */,
    MV_OP_PLORDER /* knob drag: a playlist/folder row to another position among its siblings */,
    MV_OP_PASTE /* never armed: the PASTE router's own refusals, for pl_edit_notify */,
    MV_OP_PLINTO /* knob drag dropped ON a folder row: the row goes inside it */,
    MV_OP_UNDO /* put back the rows the last edit changed (the journal) */,
    MV_OP_TAGREAD /* TAG LIST > COPY: read the tagged tracks (EP147's own Tag List cache) */,
    MV_OP_ADDN /* PASTE of the tagged tracks: append them all to one playlist */,
    MV_OP_MKREAD /* the marked (selected) playlist's position and neighbors, from the DB */,
    MV_OP_RENAME /* RENAME a playlist / folder (the naming keyboard) */,
    MV_OP_FDCOUNT /* REMOVE on a folder: what is inside it, for the confirmation */,
    MV_OP_FDEL /* ...confirmed: the folder and everything in it, one transaction, journaled */,
};

/* the Tag List read by MV_OP_TAGREAD (see CLIP_TAGGED) */
#define TAG_MAX 100

static int plrenew_kick(void);

#define SQLDB_CONN 0x38       /* the sqlite3* itself */

#define SQLITE_MAGIC_Q 0x70   /* SQLITE_MAGIC_OPEN, as sqlite checks it */

#define SQLITE_MAGIC_OPEN 0xa029a697u

#define PLUPD_VT EP_284ab30 /* music_library::SqlitePlaylistUpdater */

#define PLCACHE_DROP_PL EP_1243a58 /* (cache, playlist_id), as remove */

#define PLCACHE_NOTE \
    EP_12441d8 /* (cache, 5): ListCacheCollector::removeHierarchyCache(u16 category);              \
                * drops the kind-0x22 caches of that category */

#define INFOUPD_CACHE 0x18

static struct {
    int (*prepare)(void *, const char *, int, void **, const char **);
    int (*bind_int64)(void *, int, long long);
    int (*step)(void *);
    long long (*column_int64)(void *, int);
    const unsigned char *(*column_text)(void *, int);
    int (*finalize)(void *);
    int (*get_autocommit)(void *);
} g_sq;

static int sq_resolve(void);

static int sq_row(void *conn, const char *sql, long long p1, long long p2, long long *out,
                  int ncol);

static int sq_text(void *conn, const char *sql, long long p1, char *out, size_t cap);

/* the per-stick add target (see pl_request_set_target, below) */
#define PL_TGT_NAME_MAX 96

struct pl_addtgt {
    unsigned pl, media, attr;
    char name[PL_TGT_NAME_MAX];
    char mount[128];
};

/* the playlist-list drag's snapshot of every displayed name (plist_snapshot_names) */
#define PLO_MAXROWS 512

static int g_plo_n;

static unsigned char g_plo_folder[PLO_MAXROWS];

static unsigned char g_plo_have[PLO_MAXROWS];

#define PL_ATTR_FOLDER 1

static int cp_mount_of(void *conn, char *out, size_t n);

static int cp_request(unsigned src_media, unsigned cid, struct pl_addtgt *t);

static int cp_request_playlist(unsigned src_media, unsigned src_pl, struct pl_addtgt *t);

#define RS_MSG_MS 5000                                    /* = PL_MSG_READ, defined further down */

static int rs_read(uintptr_t conn, FILE *lg);

static int mlserver_chain(void *server, uintptr_t *svc_o, uintptr_t *lib_o, uintptr_t *db_o,
                          unsigned *media_o);

static int sq_exec4(void *conn, const char *sql, long long a, long long b, long long c,
                    long long d);

static void jr_scope_parent(void *c, long long parent);

static void jr_scope_pc(void *c, long long pl);

static int mv_execute(void *server, FILE *lg);

static int lp_exec(void *conn, const char *sql, FILE *lg);

static int ls_pdb_sync_root(void *conn, const char *root, const char *src, const char *why,
                            FILE *lg);

static int lib_backup(void *conn, FILE *lg);

static _Atomic int g_ls_armed;

static void ls_service(void *server);

enum {
    NM_IDLE,
    NM_NAMING,
    NM_CREATING
};

static struct {
    _Atomic int state;
    unsigned media, ref_pl, folder, rename_pl;
    char name[256], oldname[256];
    unsigned long fl_w;
    int fl_row;
} g_nm; /* folder: NEW FOLDER; rename_pl: RENAME; fl_*: where the new row lands */

static _Atomic int g_nm_create_req, g_nm_cancel_req, g_nm_dirty;

static int g_sm_on, g_rs_on;

#define TRACKLIST_WIDGET_VT EP_2b7f408

static struct plsnap g_grab;

#define CLIP_TRACK 1

#define CLIP_LIST 2

#define CLIP_FOLDER 3

#define CLIP_TAGGED 4 /* the Tag List's tracks (g_tag_cids), read at COPY time */

#define CLIP_FILES 5 /* a folder or file in the Browse FOLDER view (path), copied as files */

static struct {
    int kind, move;
    unsigned media, id, src_pl, src_seq;
    char name[PL_TGT_NAME_MAX];
    char path[512]; /* CLIP_FILES: relative to its stick */
} g_clip;

static void fc_mount_cleanup(const char *mount); /* folder_copy.c */
static void fc_refresh_service(void *server);
static void fc_tick(void);
static int fc_request(unsigned src_media, const char *src_rel, unsigned dst_media,
                      const char *dst_dir, unsigned dst_id, const char *name);

static int g_paste_pending;

static int g_paste_remove;

#define FL_MS 1300

#define FL_HOLD 400 /* full strength first: EP147 re-lists ~0.35 s after the write lands */

static struct {
    unsigned long w;
    int row, plist;
    long long t0;
} g_fl;

static struct {
    unsigned long w;
    int row, plist, cutrow;
} g_fl_paste; /* where a paste lands (cutrow: a cut playlist's row in the same list) */

static struct {
    int on;
    unsigned id, media;
    int rows;
    char prev[64], name[64], next[64];
} g_cg;

static _Atomic unsigned long g_cg_w;

static _Atomic int g_cg_row;

static _Atomic unsigned long g_cg_tw;

static int g_paste_cutmove;

static struct pl_addtgt *pl_target_for(unsigned media);

/* Playlist-editing messages go in EP147's gray top strip, one line each, saying what
 * happened (and for the target, what to do next); the centered box is kept for EP147's
 * own OK / CANCEL confirmations. A successful MOVE says nothing: the list shows it.
 * The hold line lasts the whole drag; the drop, cancel or refusal replaces it with its
 * own short result (az_banner_strip outranks our own current strip). */
#define PL_MSG_BRIEF 2000 /* a result the list already shows: Added, Removed, Moved */
#define PL_MSG_READ 5000  /* something to read and act on: errors, the target, hints */
#define PL_MSG_HOLD 60000 /* the drag instructions, until the drag ends */

static void pl_edit_notify(int op, int rc);

static int pl_request_remove(void);

static int pl_request_add(unsigned pl, unsigned cid, unsigned media, const char *name);

static _Atomic int g_tb_screen_ok, g_tb_up, g_tb_done;

/* UNDO window: after an edit that left a journal, its result line is held in the
 * strip for UNDO_MS with UNDO where DONE sits (the same button, g_tb_undo_mode). */
#define UNDO_MS 10000

static _Atomic int g_tb_undo_mode;

static _Atomic int g_tb_undo_req, g_tb_done_btn;

static struct {
    int on;
    unsigned media;
    long long at;
} g_tagclr; /* clear-after-paste, deferred (tag_clear_tick) */

static void pl_undo_offer(const char *msg);

static _Atomic int g_tag_touched;

/* UNDO of a tag paste re-tags its tracks through EP147's own addTrack (slot +0x10).
 * A trackid::TrackID is a plain 16-byte value {ManagementSystemID (8), content_id (4),
 * 0 (4)} (addTrack 0x9d6ea0 checks bytes 0, 4, 6 and 8 and copies it with one ldp), so
 * building one owns nothing. The MSID is the one captured from EP147. */
static struct {
    int on;
    unsigned media;
    int n;
    unsigned cids[TAG_MAX];
} g_retag;

static struct {
    unsigned media;
    uint64_t msid;
} g_tc_msid[4];

static void tag_clear_tick(int undo_pressed);

static int g_tag_stems;

/* EP147's TaglistEditor (vt TAGEDIT_VT), captured where it receives a tag: slot +0x10
 * addTrack(self, const TrackID&) (fired by TAG TRACK) and +0x30 clearTaglist(self, const
 * ManagementSystemID&) (fired by REMOVE ALL TRACKS, same self). Both arguments start with
 * the stick's 8-byte ManagementSystemID ({1, 0x00010102} for media 0102). After the
 * tagged tracks are pasted, the Tag List is cleared through +0x30 exactly as REMOVE ALL
 * TRACKS does. Message thread. */
#define TAGEDIT_ADD_SLOT (TAGEDIT_VT + 0x10)

#define TAGEDIT_CLEAR_SLOT (TAGEDIT_VT + 0x30)

static int tag_clear(unsigned media);

static int pl_request_set_target(void);


static int tracklist_selected_row(void);

static int pl_request_add_selected(void);

static void drag_commit(const struct dh_intent *it);

static void drag_cancel(void);

/* The cell Components are cached, so remapping refreshComponentForCell is not enough
 * on its own: juce only calls it when the table thinks its rows changed, and a plain
 * repaint reuses the existing Components (the painted highlight moves, the titles don't).
 *
 * juce::ListBox::updateContent() re-asks the model: `[widget+0x48]` is the TableListBox,
 * and 0x1ba2df0 tail-calls 0x21e48b0 with it, which sets a "needs update" byte at +0x10d
 * and calls getNumRows (model+0x10).
 *
 * Called once per target change, never per frame: it is a real relayout, and it runs
 * from inside a paint, so it is also guarded against re-entering itself. */
#define TRACKLIST_MODEL_VT EP_2b7f588 /* the TableListBoxModel subobject */

static void drag_capture_widget(void *model);

#define LISTBOX_REPAINTROW EP_21e7540 /* juce::ListBox::repaintRow(int) */

#define VIEWPORT_SETVIEWPOS EP_21e3440

static void drag_scroll_to(uintptr_t lb, int row);

static void drag_update_content(void);

#define ROWPAINT_ADDR EP_1bcfb78

/* paintCell is model slot +0x90 (0x1bdddc0 = `sub x0,x0,#0x30; b 0x1bdcf98`), not
 * +0x20: +0x18/+0x20 are forwarders of another interface (`br [vt+0x88/+0x90]`) that are
 * never called. The real paintCell 0x1bdcf98 draws "#", art, BPM and Key from the row
 * ring by `row`; refreshComponentForCell builds components only for columns 1 (preview)
 * and 4 (title). Its first instruction is a tbnz, so it cannot be inline-hooked; the slot
 * is patched instead. juce's TableListBox RowComp::paint (0x21ea4xx) calls model vt+0x88
 * (paintRowBackground) then vt+0x90 (paintCell). */
#define PAINTCELL_SLOT (TRACKLIST_MODEL_VT + 0x90)

#define REFRESHCELL_SLOT (TRACKLIST_MODEL_VT + 0x28) /* model slot 5 */

static void paintcell_hook(void *model, void *g, int row, int col, int w, int h, int sel);

static void mp_anim_tick(void); /* menu_popup.c: the submenu slide (and its BACK) */

#define PLUPD_INS (PLUPD_VT + 0x18) /* insert track into playlist */

#define PLUPD_MOVE_SLOT 0x50

/* Updater lifetime: the updater is transient, so a pointer captured from a call is a
 * use-after-free later. Do not keep it: keep its factory's owner, and make a fresh one
 * per move.
 *
 * The factory is 0x11881d8:
 *     mov x20, x0           ; the owner
 *     mov x0, #0x18 ; bl operator new(24)
 *     ldr x1, [x20, #0x10]  ; the owner's db handle
 *     str <0x284ab30>, [x0], #8 ; vptr
 *     bl 0x1187f70          ; base ctor(rest, handle)
 *     ret x19               ; the new updater
 * and the object is heap-allocated, so it must be destroyed. Vtable slot +0x00
 * is the plain destructor; slot +0x08 is the deleting one (it ends
 * `mov x1,#0x18` into operator delete), which is the one that matches `new`.
 *
 * The owner is the long-lived end (it holds the database handle) but gets the same
 * treatment: remember its vptr at capture and require the same vptr at use. A stored
 * pointer is not proof the object lives. */
#define PLUPD_FACTORY EP_11881d8

/* The updater and its owner (music_library::SqlitePlaylist) are per-operation
 * temporaries, so capture the durable end instead: the service EP147 registers once in
 * its object map and reaches through meow::MappedObjPtr.
 *
 * meow::ObjectMap<0,true>::regObject(const EasyID&, Type*) is instantiated per
 * type; the instantiation for music_library::IPlaylistUpdateProvider is at
 * 0x7a01e0, identified by the __PRETTY_FUNCTION__ string its assert path
 * references (0x25fe5a8, the only code reference in the binary). x1 is the
 * Type*: the body does `mov x28, x1` and returns it.
 *
 * Its 4th instruction is an adrp, which is fine: azm_install_hook declines PC-relative
 * branches, adr and literal loads, and relocates or materializes an adrp.
 *
 * Registration happens once at start-up, so the pointer is the long-lived one. It is
 * still validated at use: long-lived by design is not a guarantee across a stick eject. */
#define PLPROV_REGOBJ EP_7a01e0

/* Refreshing the list: writing the DB does not redraw it; that is a separate mechanism.
 *
 * usecase::library_edit::PlaylistRenewalNoticeHandler's vtable has two destructors and
 * one virtual, 0x9d1ae8, its IPlaylistRenewalListener notification: it reads `this+8`
 * (the listener it forwards to), bails if that is null, and posts work. The stock
 * Tag List -> CREATE PLAYLIST flow uses it to refresh the playlist screen, so `this` is
 * captured there and the same call is made after our own move. */
#define PLRENEW_FN EP_9d1ae8

/* The native refresh, one level above the populator:
 *   fillPane 0x1f45660 has exactly one caller, 0x1f47494, inside 0x1f47470.
 *   0x1f47470 has no BL callers, so it is virtual; the qword sits in
 *   gui::disp_data::BrowseDispDataCreator's vtable at slot 3 (+0x18).
 * Its body is four instructions of intent:
 *     x3 = this->[0x10]        ; the BrowseDispData
 *     x3->[0xb0] += 1          ; bump a generation counter
 *     bl fillPane(this, 0, serial, dispdata)
 *     b  0x1f43870             ; tail-call, the notify/repaint half
 * so `creator->vt[3](creator)` is a one-argument "rebuild pane 0 and tell the
 * UI". The sibling at 0x1f474a8 is the same for pane 1 (w1 = 1).
 *
 * The creator is fillPane's own first argument, so it is captured there.
 *
 * Re-entrancy: the refresh repaints, our tick runs from a paint, so a guard is
 * required or the first move recurses until the stack ends. */
#define BROWSE_CREATOR_VT EP_2dcc760

#define FILLPANE_FN EP_1f45660


static int install_scci_hook(void);

static void rowcache_tick(void);

static int g_ui60ui_installed;

static void install_rowcache(void);

static int install_library_hooks(void);


static void install_remotestems(void);

static void install_reorder(void);

static void install_folderpreview(void);

static void install_mlreply_probe(void);

static int sm_shown(void);

static int sm_marks(void);

static pthread_mutex_t g_sm_mu;
/* g_sm_mu is taken by EP147's message thread (row painters) and by the stems-mark worker,
 * which runs SCHED_IDLE. An idle-class thread preempted while holding the lock would park
 * the painter behind it for as long as the CPU stays busy, so the worker leaves the idle
 * class for exactly the time it holds the lock. Every g_sm_mu user goes through these. */
static __thread int g_sm_idle_thread; /* set by the worker after it enters SCHED_IDLE */
static inline void sm_lock(void) {
    if (g_sm_idle_thread) {
        struct sched_param sp = {0};
        pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
    }
    pthread_mutex_lock(&g_sm_mu);
}
static inline void sm_unlock(void) {
    pthread_mutex_unlock(&g_sm_mu);
    if (g_sm_idle_thread) {
        struct sched_param sp = {0};
        pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp);
    }
}

static unsigned sm_hash(const char *t);

static _Atomic int g_sm_dirty;

static int sm_lookup_pl(int row, const char *name, unsigned *id);

enum {
    SM_ST_PENDING = 1,
    SM_ST_NONE,
    SM_ST_STEMS,
    SM_ST_UNKNOWN,
    SM_ST_PART
};

#define SM_PLF 0x80000000u /* cache/queue key bit: a PLAYLIST id, not a content id */

#define SM_CACHE 2048

static struct sm_ent {
    unsigned cid, h;
    unsigned char st, stale, queued, busy;
    int ntrk;
} g_smc[SM_CACHE]; /* busy, ntrk: playlists, see sm_resolve_playlist */

#define SM_PROWS 32            /* rows painted while their status was pending */

static _Atomic int g_sm_world;

static struct sm_ent *sm_ent(unsigned cid, unsigned h, int create);

static int sm_status_mb(unsigned cid, const char *title, unsigned media, int *await, int *busy);

/* ph (from the helper's job file, one animation per phase): SM_PH_UP uploading (or still
 * queued), SM_PH_ST being stemmed, SM_PH_DN the stems coming back to the stick. A job file
 * without ph means running = stemming. */
enum {
    SM_PH_UP,
    SM_PH_ST,
    SM_PH_DN,
    SM_PH_WT
}; /* WT: queued, or sent and waiting its turn */

static _Atomic long long g_sm_anim_ms;

#include "stemsmark.h"

enum {
    SMK_I_FULL,
    SMK_I_PART,
    SMK_I_FILL0,
    SMK_I_ONE0 = SMK_I_FILL0 + SMK_FILL_STEPS + 1,
    SMK_I_N = SMK_I_ONE0 + 4
};

static int smk_anim(int ph, long long t);

static void smk_draw(void *g, int which, int x, int y);

static void sm_after_paint(void *self, void *g, int row, int col, int w, int h);

static int sm_mark_row(struct sm_w *e, uintptr_t self, void *g, int row, int x0, int y0);

/* sm_title_prep, called by hlabel_paint_hook before the label paints: 0 = not a registered
 * track title, 1 = a title with no mark, 2 = a title that gets one (*t holds where).
 * Ownership is checked at use:
 * the label must sit under the widget's ListBox (a freed title's address can be reused by
 * any label). */
struct sm_tprep {
    struct sm_w *e;
    uintptr_t w;
    int row, x0, y0, lw, lh;
};

static int sm_title_prep(void *self, struct sm_tprep *t);

static uintptr_t g_tclip_label;

static int g_tclip_x;

static int cg_track_row(uintptr_t tw, int row);

static void install_stemsmark(void);
static int install_preview_hotcues(void); /* preview_hotcues.c, via stem_marks.c */

static int install_hlabel_paint_slot(void);

static void install_hidetitle(void);
static void ht_runtime_init(void);

static void install_phasemeter(void);

static void (*g_orig_setgridadj)(void *, int);

static void install_wavestems_hook(void);

static void arm_stems(int on);

static int (*next_open)(const char *, int, ...);

static int (*next_open)(const char *, int, ...);

static ssize_t (*next_read)(int, void *, size_t);

static ssize_t (*next_read)(int, void *, size_t);

#define NEXT(p, name)                   \
    do {                                \
        if (!p)                         \
            p = dlsym(RTLD_NEXT, name); \
    } while (0)

/* Browse-knob rotation is not on spi3.0 (that field is dead) and not in the
 * edb_daemon.sock text events (the "up"/"down"/"-59" reads there are unrelated network
 * traffic).
 *
 * It comes from /dev/gpiodrv (kernel/drivers/gpio/gpiodrv.c), a char device where the
 * fd's lseek offset selects the GPIO line: `lseek(fd,gpio,SEEK_SET); read(fd,buf,1)`
 * returns that pin's level. EP147 opens several fds and lseeks each to a different gpio
 * (on the deck: 3/9/10/69/154/156); the quadrature A/B pair is among them but not yet
 * identified. Telling the lines apart requires tracking the lseek'd gpio per fd.
 *
 * Not wired to g_enc_dir: debug-only capture of per-gpio level changes. Real deck only;
 * the emulator's ep122_shim stubs gpiodrv as one shared pipe with no per-gpio lseek. */
#define MAX_GPIODRV_FDS 16

static int g_last_click_bit, g_last_back_bit;

extern int g_hidinj_peer, g_hidinj_reads;

static int az_banner_strip(const char *text, int dur_ms);

static int az_banner_strip_clear(void);

static void install_qrlogin_guard(void);

void hidinj_read_note(ssize_t r, size_t asked, const unsigned char *b, uintptr_t ra);

static void install_scrntrace_hook(void);

#define AZ_NBUS 4
#define BROWSE_SETROWDATA EP_1bdb7e0
#define TAGEDIT_VT EP_26a4838 /* usecase::library_edit::TaglistEditor */
#define LC_DROP_TRACKS \
    EP_1243b38 /* ListCacheCollector::removeTrackListCache(): no caller in EP147 */
static void lib_invalidate(uintptr_t cache, void *conn,
                           FILE *lg); /* the library spool's full set, below */
static int install_drag_view(void);

/* community.c: the community mod host (included near the end of azmod.c) */
static int g_cm_live; /* mods with state LOADED: the LED-frame dispatch is skipped at 0 */
static void community_init(void);
static void community_tick(void);
static void ssh_approve_tick(void); /* ssh_approve.c */
static int community_redraw_wanted(void);
static int community_frame(int bus, unsigned char *f, int len);
static int sdk_filter_run(int bus, unsigned char *f, int len);   /* sdk_input.c */
static void sdk_filter_raw(int bus, const unsigned char *f, int len);
static void sdk_filter_view(int bus, const unsigned char *f, int len);
static void sdk_touch_read(void *buf, ssize_t r);
static void cm_icons_tick(void); /* community_icons.c: one tile icon decoded per tick */
static const char *cm_blame_fault(uintptr_t pc, uintptr_t lr, uintptr_t fp); /* the crash reporter names a faulting mod (its id, or NULL) */

/* stem_marks.c: a HierarchyListWidget EP147 is calling us about right now (message thread,
 * during its call) is alive; the playlist-row marks repaint only widgets noted here. */
static void hlw_live_note(uintptr_t w);
/* sdk_ui.c: EP147's message thread runs what the community mods queued (msg_tick). */
static void sdk_ui_msg_drain(void);
/* Community mod services (community.c, sdk_*.c) the rest of the unit uses. */
static __thread int g_cm_on_ui;                    /* this is the overlay (UI) thread */
/* g_sdk_confirm_core: the core-internal yes/no dialog handoff asks through. The deck-ui
 * services set it at startup to their confirm without the table entry's mod-caller checks
 * (the caller and fn here are the core's own code, which those checks would refuse). Same
 * dialog and rules as az_core_v1.confirm: returns 0 when shown, then calls fn exactly once on
 * the UI thread with the answer (BACK = no); -1 when refused (another question is open), and
 * fn is not called. While it is NULL, handoff is refused: the DJ must agree before EP147
 * stops, and there is no way to ask. */
static int (*g_sdk_confirm_core)(const char *title, const char *text, const char *yes,
                                 void (*fn)(int yes, void *user), void *user);
static int g_sdk_reload_req;                       /* __atomic: a mod asked for a Reload (sdk_proc.c) */
static int sdk_proc_owns(const char *id, int pid); /* pid was launched by mod id and runs (sdk_proc.c) */

#endif /* AZMOD_INTERNAL_H */
