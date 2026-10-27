/* SPDX-License-Identifier: GPL-3.0-only */
/* azmod menu: the Mod Menu's state and drawing interface. Hooks in azmod.c call into this.
 * menu.c also builds on the host for the unit tests (lanetest.c, the menudump goldens). */
#ifndef AZMOD_MENU_H
#define AZMOD_MENU_H
#include <stdint.h>
#include <stdbool.h>

/* Themes (mod 37: Day Mode and the dark themes) are in every build. Their hooks install at
 * every start and are inert while Themes is off (azmod.c daymode_wanted; kill switch
 * settings/azmod-themes.off). */
#define AZMOD_HAVE_DAYMODE 1

/* Framebuffer we composite into (EP147's mapped dumb buffer). XRGB8888. */
typedef struct {
    uint8_t *px; /* base pointer */
    int w, h;    /* pixels */
    int stride;  /* bytes per row */
} azmod_fb;

/* The Mod Menu's screens. Numbered in order, no holes: they live only in memory (nothing
 * persists or transmits them; the diagnostic logs print them). AZ_CLOSED is 0 (a zeroed state
 * is closed; features.def's "opens no screen"). */
typedef enum {
    AZ_CLOSED = 0,
    AZ_OPEN,
    AZ_INFO,
    AZ_COLORS,     /* pad-mode button colors: master toggle + one row per button */
    AZ_COLOR_PICK, /* swatch grid for the one button chosen on AZ_COLORS (color_sel) */
    AZ_UPDATE,     /* Update Mods: the on-deck mods updater's status + one-click update */
    AZ_OVERLAYS,   /* the deck-screen readouts: master toggle + one row per readout */
    AZ_COMMUNITY,  /* Community Mods: banner, master switch, installed list, a mod's page */
    AZ_CLOCK_TZ,   /* the Clock's time zones by region, opened from the Clock screen (screen_overlays.c) */
    AZ_CLOCK,      /* the Clock: its switch, time format and time zone (screen_overlays.c) */
    AZ_CLOCK_SET,  /* Set Time Manually: the date and time picker, opened from the Clock (screen_overlays.c) */
    AZ_BROWSE_VIEW, /* Browse View Mods: one row per Browse track list change (screen_overlays.c) */
} azmod_mode;
#define AZMOD_HAVE_CLOCK_TZ_LIST 1 /* AZ_CLOCK_TZ exists (the menu dump names it) */
#define AZMOD_HAVE_CLOCK_SCREEN 1  /* AZ_CLOCK exists (likewise) */
#define AZMOD_HAVE_CLOCK_SET 1     /* AZ_CLOCK_SET exists (likewise) */
#define AZMOD_HAVE_BROWSE_VIEW_SCREEN 1 /* AZ_BROWSE_VIEW exists (likewise) */
#define AZMOD_HAVE_MENU_GLIDE 1 /* the lists glide on g_glide_ms (the menu dump steps that clock) */

/* Deck count (bus/LED geometry). */
#define AZMOD_DECK_COUNT 4

/* Stem toggles: a 4-button strip (VOCAL/INST/BASS/DRUM) over a deck's waveform while a deck
 * screen is up. Tapping toggles a stem; azmod.c writes the per-deck mask (azstem.stems /
 * azstem.stems2: bit7 engaged, bits0-3 enabled). Deck 1's strip sits at the top of its
 * waveform, deck 2's at the bottom. Gated by the STEMS mod (az_on(AZF_STEMS)). */
/* Stem palette, 0xRRGGBB, in azstem's stem order (VOCAL, INST, BASS, DRUMS). The pad pills,
 * mini lanes, layered waveform and per-column recoloring all read it from here. */
#define AZMOD_STEM_PALETTE {0x22E022u, 0xFF3030u, 0xFF33CCu, 0x4060FFu}
#define AZMOD_STEM_COUNT 4
#define AZMOD_STEM_DECKS 4 /* stem-capable decks: Deck 1..4 (azstem engine is NDECK=4) */
#include <unistd.h>
/* Debug probes: a probe written as AZ_PROBE_ON(flag) is the constant 0 in a normal build,
 * so the compiler removes it and its flag file does nothing; build with -DAZMOD_PROBES to
 * make those flags live. azmod's diagnostic logs go through azlog_open(), which writes
 * nothing unless /home/root/settings/azmod-logs.on existed at start. Not everything is
 * behind those two gates: a few plain access()/open() test hooks under /tmp remain in
 * release builds (fake temperature and CPU-frequency inputs, touch-simulation and
 * Themes test triggers), and azstem writes small status files to /tmp unconditionally.
 * Feature flags and ".off" kill switches are plain access() by design. */
#ifdef AZMOD_PROBES
#define AZ_PROBE_ON(p) (access((p), F_OK) == 0)
#else
#define AZ_PROBE_ON(p) 0
#endif

/* Physical PAD hardware decks. The ErP LED frame (128 B) and EP147's native HuiLed array
 * hold only two physical pad sets; decks 3/4 share those pads through the hardware
 * deck-select. Loops that index native per-deck memory (HuiLed arg0+0x110+d*0x108, LED frame
 * f+off+d*0x38, ErP read byte14+d*8) must bound to this, not AZMOD_STEM_DECKS: 4 there reads
 * and writes past the native 2-deck structures and crashes EP147. */
#define AZMOD_PAD_DECKS 2

/* Which logical deck a physical side is driving. side 0 -> deck 1 or 3, side 1
 * -> deck 2 or 4, indexing the 4-wide per-deck stem arrays. sel is that side's
 * deck-select state: 0 = primary, 1 = secondary (azmod.c tracks it off the ErP
 * deck-select bits; see g_deck_sel). */
static inline int azmod_logical_deck(int side, int sel) {
    return side + (sel ? AZMOD_PAD_DECKS : 0);
}

#include "pad_palette.h"
#define AZ_ACT_STEMS (-2) /* last_action signal (not a menu row): write per-deck masks */
/* FPS readout: the empty black band top-right of the deck screen (x 1090-1278, y 48-88 on
 * 1280x800). */
void azmod_fps_rect(const azmod_fb *fb, int *x, int *y, int *w, int *h);

/* Bitmap-font text for callers outside menu.c (the RESTARTING screen), which must not
 * depend on the TTF. */
void azmod_draw_text_at(const azmod_fb *fb, int x, int y, const char *s, int scale, uint32_t c);
/* Draw `s` centered in (x,y,w,h) at `px` pixel size in EP147's own UI TTF (text.c). Returns 0
 * if the font is not available; callers then fall back to the bitmap font. Weak so menu.c
 * links without text.c (the host tests). */
int azmod_ttf_center(const azmod_fb *fb, int x, int y, int w, int h, const char *s, int px,
                     int track, int bold, uint32_t c) __attribute__((weak));
/* Same, in the face EP147 draws its SOURCE list in (TsukuGoCustomPro-B): the Mod Menu's
 * text. w <= 0 left-aligns at x. */
int azmod_ttf_menu(const azmod_fb *fb, int x, int y, int w, int h, const char *s, int px, int track,
                   int bold, uint32_t c) __attribute__((weak));
/* Advance width in px of the first n bytes of s in the menu face (0 = no font). */
int azmod_ttf_menu_width(const char *s, int n, int px, int bold) __attribute__((weak));
/* azmod_ttf_menu with a color switch at byte `split` (glyphs from there on in c2) */
int azmod_ttf_menu2(const azmod_fb *fb, int x, int y, int w, int h, const char *s, int px,
                    int track, int bold, uint32_t c, int split, uint32_t c2) __attribute__((weak));

/* Mod Menu rows: features.def is the one list (ids AZF_* / AZI_*, values az_on / az_get,
 * the table, pins, the azmod.cfg mapping: azfeatures.h, included below). */
#define AZMOD_CHANGELOG_MAX 65536   /* bytes of CHANGELOG.txt kept (the rest is dropped) */
#define AZMOD_DRUM_BANKS 4
/* The version the deck is actually running: the mods set the loader staged this boot
 * (/tmp/az-deck/version = manifest.json's version), re-read every ~2 s so a Mod Menu update
 * shows the new number without a reload. "" when no set is staged. */
const char *azmod_live_version(void);
/* ---- community mods (community.c in azmod.c; the Mod Menu's Community pages read these) */
#include "azmod-mod.h"
#include "azfeatures.h"
#include "azcm.h"

#define AZMOD_INFO_LINES 5 /* uptime, mem free, mem total, AZ-MODS version, IP + SSH */
#define AZMOD_INFO_LINE_LEN \
    40 /* the longest line, "IP 255.255.255.255  SSH off", is 27 chars */
#define AZMOD_UPDATE_LINES 5 /* AZ_UPDATE text: installed / state / bar row / unused / button label */
#define AZMOD_UPDATE_LINE_LEN \
    128 /* line 1 carries a failure's whole reason; drawn fitted */

/* SELECT PLAYLIST banner's DONE button (menu.c draws/hit-tests, azmod.c acts). */
void azmod_tb_done_rect(const azmod_fb *fb, int *x, int *y, int *w, int *h);
void azmod_tb_done(void); /* azmod.c: forget every stick's target, drop the banner */
void azmod_tb_undo_rect(const azmod_fb *fb, int *x, int *y, int *w, int *h);
void azmod_tb_undo(void); /* azmod.c: undo the last playlist edit */

/* ticks the orange "pressed" flash holds after an item is tapped/clicked,
 * at azmod.c's ~30 Hz redraw rate (~200ms). */
#define AZMOD_FLASH_TICKS 6

typedef struct {
    azmod_mode mode;
    bool source_visible; /* set by the present-hook's screen detector */
    bool tb_visible;     /* SELECT PLAYLIST: our held strip is up, draw DONE over its right end */
    bool tb_undo;        /* ...and UNDO shows (the last edit's result line), left of DONE, amber */
    bool tb_done_btn;    /* DONE shows too (a clipboard or ADD TO shortcut to clear) */
    bool nm_visible;        /* naming a NEW PLAYLIST in EP147's search field: show CREATE */
    int menu_sel;           /* highlighted row in whichever screen is active */
    int menu_focused; /* docked menu has knob focus (FOCUSED); 0 = PREVIEW, no orange row */
    /* Full-screen Mod Menu: while FOCUSED the menu covers
     * the screen like EP147's own SOURCE page: the mods on the left, the selected
     * one's settings (or its sub-screen) on the right. */
    int pane;          /* knob focus: 0 = the left list, 1 = the right pane */
    int left_sel;      /* the left list's selected item (a top-level item id) */
    bool want_focus;   /* menu.c asks azmod.c to focus the menu (preview card tapped) */
    bool want_unfocus; /* ...or to hand the cursor back to the SOURCE list (header back) */
    int last_action;   /* item index activated this tick, else -1 (for hooks/log) */
    int pressed_item;  /* row flashing (tap feedback), -1 = none */
    int pressed_ticks; /* remaining flash ticks for pressed_item */
    bool config_dirty;    /* true for one call after a change; azmod.c persists + clears */
    /* the rows' values and pins live in azfeatures.c (az_on / az_get, az_pinned) */
    bool pinrows_on;      /* mirror of the azmod-pinrows.on flag (azmod.c sets it per tick)
                           * so menu.c can gate the pin glyph without a syscall. Off = feature disabled. */
    bool drum_no_samples; /* azstem_drum_available() == 0: no DRUM ROLLS bank on the
                           * stick, so the drum roll shows as having nothing to play. azmod.c sets it
                           * per tick so menu.c needs no syscall for it. */
    char update_line[AZMOD_UPDATE_LINES][AZMOD_UPDATE_LINE_LEN]; /* AZ_UPDATE text: azmod.c
        renders /tmp/az-modupdate/status (written by deck/lib/az-modupdate under the
        loader's watcher) into these; refreshed ~2/s while the screen is up */
    int update_uptodate; /* 1 when the status says the stick is up to date (drawn green) */
    int update_avail;    /* 1 when the channel has a newer set (status state=available); drawn as a
      red dot on the Update Mods row's icon and refreshed while the menu is open */
    int update_pct;      /* 0..100 progress of a running update (-1 = idle, no bar) */
    char mods_local[32]; /* installed mods version, "" = unknown (azmod.c, from the status) */
    char mods_remote[32]; /* the channel's newest version when the last check found one, else "" */
    /* CHANGELOG box on the Update Mods screen, under the button.
     * azmod.c loads /tmp/az-deck/CHANGELOG.txt (the staged MODS/deck, so it follows
     * the installed set, the no-stick cache and a deck-side update) each time the
     * screen opens and bumps changelog_gen; menu.c wraps it once per gen/width and
     * scrolls it by whole lines (knob turn, touch drag). Format: "# " section,
     * "## " version, "+ " new, "! " fixed, "^ " improved, "~ " changed. */
    char changelog[AZMOD_CHANGELOG_MAX];
    int changelog_len;
    unsigned changelog_gen;
    int changelog_scroll; /* scroll target, px from the top (knob / drag set it) */
    int changelog_pos;    /* scroll drawn, px; eases toward the target in azmod_tick */
    int update_busy;      /* 0 idle; 1 a CHECK, 2 an UPDATE, from the click until the updater
                           * reaches that request's final state (a check also ends at "available"); the
       bar stays up and only ever moves forward while this is set (no blink between the
       updater's check -> available -> downloading phases) */
    int update_want;      /* set by the screen's click: 1 = check, 2 = update; azmod.c writes
       /run/az-modupdate.req and clears it. The overlay never touches the network. */
    char info_line[AZMOD_INFO_LINES][AZMOD_INFO_LINE_LEN]; /* AZ_INFO text, filled
        by azmod.c (menu.c reads nothing from /proc) */
    /* System Info's Save Logs to USB button. A press sets savelogs_want; azmod.c writes
     * /run/az-savelogs.req and the loader's watcher runs deck/lib/az-savelogs, whose
     * /tmp/az-savelogs/status azmod.c renders into savelogs_line, polled while the screen is
     * up. savelogs_busy: from the press until that request's final state (a busy button takes
     * no press). savelogs_tone: 0 neutral, 1 saved, 2 refused, 3 failed. */
    int savelogs_want;
    int savelogs_busy;
    int savelogs_tone;
    char savelogs_line[AZMOD_UPDATE_LINE_LEN];
    bool stem_on[AZMOD_STEM_DECKS][AZMOD_STEM_COUNT];      /* per deck: which stems
             enabled (default all on); azmod.c writes azstem.stems/.stems2 on a deck
             screen. Reset to all-on when that deck loads a new track. */
    bool stems_available[AZMOD_STEM_DECKS];                /* per deck: azstem.so reports the
                       loaded track has stems (azstem.avail/.avail2/3/4). The no-stem gate
                       keys everything stem-shaped on this, ANDed with the STEMS
                       master (az_on(AZF_STEMS)): a false deck draws no stem UI (no strip,
                       buttons, dots, lanes, tiles or title), its physical pad LEDs go dark and
                       its BEAT LOOP pad-mode button LED stays native orange instead of green:
                       the at-a-glance "this track has no stems" state. Hit-testing skips it
                       too. */
    int cpu_temp_c10;         /* CPU (soc-thermal) in tenths of a degree C,
                               * read 1/s by azmod.c while mod 13 is on; < 0 = no sensor */
    int cpu_use_pct;          /* all-core busy %, 1/s; < 0 = not yet */
    int cpu_clk_mhz;          /* fastest core's current clock; < 0 = unknown */
    bool cpu_clk_throttled;   /* that core runs below its own max */
    int ram_use_pct;          /* (MemTotal - MemAvailable) / MemTotal; < 0 = unknown */
    char clock_text[16];      /* the Clock readout's text; "" = the time is not known yet */
    bool pledit_needs_reload; /* Playlist Editing is ON but its hooks were not installed at start */
    bool reload_pending;      /* the loader staged a set this EP147 has not loaded (/run/az-reload-pending) */
    int fps_paint, fps_ui;    /* measured 1/s by azmod.c: WaveformView
                               * paints (= actual on-screen FPS, the only number drawn) and JUCE
                               * repaint-timer ticks (diagnostic only; lives in /tmp/azmod-rates). */
    bool waveform_visible; /* on EP147's WAVEFORM (deck) screen; the stem strip shows only
                              here, never on Browse or other views */
    int drum_zone; /* x-pad zone 0..5, -1 = no finger (FX rate row) */
    bool four_deck; /* 4-DECK layout: compact left column, no strip */
    bool pad_beatloop[AZMOD_STEM_DECKS]; /* per deck: BEAT LOOP pad mode selected
        (tracked from the ERP mode-button edges by azmod.c). The strip sits ON
        the native pad-grid slots, so it is drawn only in that mode: any other
        pad mode owns those slots. */
    /* Pad-mode color picker (AZ_COLORS / AZ_COLOR_PICK): per pad-mode button,
     * the chosen palette index (-1 = leave native). Applies to both decks.
     * azmod.c reads these to recolor the ERP LED frame and persists them.
     * color_sel = the button whose grid AZ_COLOR_PICK is showing. */
    int8_t padmode_color[AZMOD_PADMODE_COUNT];
    int8_t padmode_color2[AZMOD_PADMODE_COUNT]; /* the same buttons on their secondary page */
    int color_sel;                              /* 0..AZMOD_PADPICK_COUNT-1 = 2*j + page */
    /* Master enable for the whole custom-pad-color feature. Off = azmod.c
     * publishes nothing and every button and pad shows EP147's own colors.
     * Row 0 of AZ_COLORS (a checkbox);
     * persisted on line 2 of azmod.cfg. */
    bool pad_colors_on;
} azmod_state;


#include "stem_overlay.h"

/* Zero the menu state and set its defaults. Call once before first use; azmod.c then loads
 * the saved settings (which also set every row value: azfeatures.c az_defaults first). */
void azmod_state_init(azmod_state *s);
/* The theme words azmod.conf's "theme" key may hold (screen_themes.c). */
__attribute__((visibility("hidden"))) int azmod_theme_keys(const char *const **keys);
/* The current theme: an index into azmod_theme_keys. azmod.c sets it at every settings load
 * and saves it as "theme"; the Themes picker sets it; day_mode.c's watcher applies it. */
extern __attribute__((visibility("hidden"))) int g_azmod_theme; /* __atomic builtins only */
/* The Clock's time zone: an index into the Overlays screen's list (screen_overlays.c), saved as
 * azmod.conf's "clock.tz" (the zone's name under /usr/share/zoneinfo). */
extern __attribute__((visibility("hidden"))) int g_azmod_clock_tz; /* __atomic builtins only */
/* clock_native.c: where the Clock takes EP147's readout's place (screen rect packed by
 * clock_native.h cn_rect_pack, 0 = nowhere), EP147's ink and ground there; __atomic only */
extern __attribute__((visibility("hidden"))) uint64_t g_azmod_clockn_rect;
extern __attribute__((visibility("hidden"))) uint32_t g_azmod_clockn_ink, g_azmod_clockn_ground;
/* Set Time Manually: the UTC time the menu asks the overlay thread to set, 0 = none (screen_overlays.c
 * writes it, mod_menu_overlay.c ov_poll_clock takes it); __atomic only */
extern __attribute__((visibility("hidden"))) int64_t g_azmod_clock_set_req;
__attribute__((visibility("hidden"))) int azmod_clock_tz_index(const char *zone); /* -1 unknown */
__attribute__((visibility("hidden"))) const char *azmod_clock_tz_zone(int k);     /* UTC if out of range */

/* Input event. touch_* in framebuffer pixels. enc_delta = browse-encoder steps
 * this tick (from the mixer-bus position counter, see azmod.c); enc_click = knob
 * pushed. The SOURCE-row button (AZ_CLOSED) is tap-only: enc_delta/enc_click are
 * ignored there; the knob only navigates once a menu
 * screen is already open. Returns true if azmod consumed the event (hook must
 * swallow it from EP147). */
bool azmod_input(azmod_state *s, const azmod_fb *fb, int touch_x, int touch_y, bool touched,
                 int enc_delta, bool enc_click);

/* Physical BACK button: one level up (a sub-screen -> Open -> Closed), same
 * as the on-screen X. Returns true if azmod handled it (hook must swallow
 * it from EP147); false when the menu is closed (EP147 gets its native Back). */
bool azmod_back(azmod_state *s);

/* Touch drag inside the Update screen's changelog box, fed from azmod.c's X
 * events: phase 0 = press, 1 = motion, 2 = release; y in framebuffer pixels.
 * Returns true while a drag the box owns is in progress. */
bool azmod_changelog_drag(azmod_state *s, const azmod_fb *fb, int y, int phase);
bool azmod_cm_drag(azmod_state *s, const azmod_fb *fb, int y, int phase);
/* Community Mods: Update all's next step, every overlay tick (the menu open or closed) */
__attribute__((visibility("hidden"))) void azmod_cm_bg_tick(void);
void azmod_menu_clock(long ms);        /* ms now, before each touch event (tap timing) */
const char *azmod_menu_last_hit(void); /* the last touch decision, for the touch log */
int azmod_pane_drag(
    azmod_state *s, const azmod_fb *fb, int x, int y,
    int phase); /* the Mod Menu right pane: 0 / 1 redraw / 2 redraw + a row tapped */

/* Draw the active menu screen (nothing while AZ_CLOSED). */
void azmod_composite(azmod_state *s, const azmod_fb *fb);
void azmod_draw_stats(const azmod_state *s, const azmod_fb *fb); /* the OVERLAYS readouts */
/* A readout is shown when its own toggle and the OVERLAYS master are on. */
bool azmod_stat_on(const azmod_state *s, int mod);
bool azmod_stats_any(const azmod_state *s);
/* The readout band actually used: the enabled readouts packed right-aligned
 * against the FPS slot's right edge. */
void azmod_stats_rect(const azmod_state *s, const azmod_fb *fb, int *x, int *y, int *w, int *h);
/* The Clock in the place of EP147's recording time readout (clock_native.c publishes it):
 * whether it shows, its rect, its drawing (the overlay's own window). */
__attribute__((visibility("hidden"))) bool azmod_clockn_on(const azmod_state *s);
__attribute__((visibility("hidden"))) void azmod_clockn_rect(const azmod_fb *fb, int *x, int *y, int *w, int *h);
__attribute__((visibility("hidden"))) void azmod_draw_clockn(const azmod_state *s, const azmod_fb *fb);
/* The Clock's countdown Timer (clock_timer.h), overlay thread only: a tap on the Clock opens or
 * closes its popup; the popup's rect (0 = not shown), a tap on it, its drawing; a key that changes
 * whenever the Clock's place must be drawn again (the countdown's seconds and its red). */
__attribute__((visibility("hidden"))) void azmod_clockn_tap(const azmod_state *s, const azmod_fb *fb);
__attribute__((visibility("hidden"))) int azmod_ctpop_rect(const azmod_state *s, const azmod_fb *fb, int *x, int *y, int *w, int *h);
__attribute__((visibility("hidden"))) void azmod_ctpop_tap(const azmod_fb *fb, int x, int y);
__attribute__((visibility("hidden"))) void azmod_draw_ctpop(const azmod_fb *fb);
__attribute__((visibility("hidden"))) int azmod_clockn_key(void);

/* A screen's row rect, i in [0..N]. */
void azmod_item_rect(const azmod_fb *fb, int i, int *x, int *y, int *w, int *h);

/* Full-screen menu (FOCUSED): the window rect, whether the knob is at the menu's
 * top level (the left list), and a reset to the first item. */
void azmod_menu_full_rect(const azmod_fb *fb, int *x, int *y, int *w, int *h);
bool azmod_menu_at_top(const azmod_state *s);
void azmod_menu_reset(azmod_state *s);
/* Overlay-window rect for an open menu screen (compact left strip). azmod.c
 * sizes the override-redirect window to this so EP147 shows through on the right. */
void azmod_menu_win_rect(const azmod_fb *fb, int *x, int *y, int *w, int *h);

/* Fade-strip bounds: solid content ends at *content_w, window at *win_w. azmod.c
 * dithers an X-Shape mask across [content_w,win_w) to fade the menu to clear. */
void azmod_menu_fade_bounds(const azmod_fb *fb, int *content_w, int *win_w);
/* Docked layout: the menu lives in EP147's SOURCE-screen right pane (the
 * per-source detail area, 640x563 @ 640,140 on 1280x800) instead of the
 * full-height left strip; used when opened from the native "MOD MENU"
 * source row (azmod.c install_srcrow_hook). Affects every rect above. */
void azmod_set_docked(bool on);
int azmod_naming_playlist(void);            /* azmod.c: NEW PLAYLIST naming is on screen */
void azmod_naming_create(void);             /* azmod.c: the CREATE tap */
void azmod_naming_cancel(void);             /* azmod.c: the CANCEL tap */
const char *azmod_naming_text(void);        /* azmod.c: the name typed so far */
int azmod_naming_folder(void);              /* azmod.c: the name is for a NEW FOLDER */
int azmod_naming_rename(void);              /* azmod.c: RENAME (not create) */
int azmod_naming_dirty(void);               /* azmod.c: the name changed since last asked */
/* NEW PLAYLIST's bar over EP147's title row, and its two buttons (shared: draw + touch) */
void azmod_naming_rects(const azmod_fb *fb, int *bar, int *cancel, int *create);
/* The stem engine is on (the Stems mod): the one gate for the stem pads, strips and audio. */
bool azmod_stems_engine(const azmod_state *s);
/* The native SOURCE-list "MOD MENU" row's icon cell (left 90 px of the row,
 * like the USB/Cloud icons): draws bg + a MOD glyph into fb at (x,y,w,h).
 * `open` = our pane is showing (row is the selected one -> EP147-blue). */
void azmod_draw_srcrow_icon(const azmod_fb *fb, int x, int y, int w, int h, bool open);

#include "menu_theme.h"
bool azmod_docked(void);

/* Advance the tap-flash animation by one redraw tick. Call every azmod.c loop
 * iteration; returns true when a redraw is needed (flash active or just ended). */
bool azmod_tick(azmod_state *s);

#endif
