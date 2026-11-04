/* SPDX-License-Identifier: GPL-3.0-only */
/* menu_state.c: the menu state: init, the stems gate, the tap flash, BACK (one level up).
 * Part of the Mod Menu's one translation unit: #included by menu.c, in the order listed there. */

bool azmod_stems_engine(const azmod_state *s) {
    (void) s;
    return az_on(AZF_STEMS);
}

void azmod_state_init(azmod_state *s) {
    memset(s, 0, sizeof(*s));
    s->pressed_item = -1;
    s->last_action = -1;
    s->update_pct = -1;
    /* The rows' values and pins are not here: azfeatures.c holds them, and the settings load
     * (azmod.c load_config) sets them, defaults first. Resetting them here would show other
     * threads the defaults for a moment between the early load and this init. */
    s->cpu_temp_c10 = -1; /* no reading yet: the readouts show --, not 0 */
    s->cpu_use_pct = -1;
    s->cpu_clk_mhz = -1;
    s->ram_use_pct = -1;
    for (int d = 0; d < AZMOD_STEM_DECKS; d++) /* all stems on */
        for (int i = 0; i < AZMOD_STEM_COUNT; i++)
            s->stem_on[d][i] = true;
    for (int j = 0; j < AZMOD_PADMODE_COUNT; j++)
        s->padmode_color[j] = s->padmode_color2[j] = -1; /* native */
    s->pad_colors_on = true; /* master enable; individual buttons still default to native */
    s->color_sel = 0;
}

/* Set the flash + fire the activation. Shared by both screens' activate paths. */
static void flash_and_activate(azmod_state *s, int i) {
    s->last_action = i;
    s->pressed_item = i;
    s->pressed_ticks = AZMOD_FLASH_TICKS;
}

static void cm_reset(void);
static int g_cm_press; /* defined in screen_community.c */
static bool cm_back(void);
static bool cm_input(azmod_state *s, const azmod_fb *fb, int tx, int ty, bool touched, int d,
                     bool click);
static void cm_draw(azmod_state *s, const azmod_fb *fb);
/* Community Mods full screen: the section slides in from the right over the whole
 * Mod Menu; BACK slides it out. g_cm_full = open (or opening); the slide itself is
 * cm_anim_* in screen_community.c. */
static int g_cm_full;
static void cm_full_open(azmod_state *s);
static void cm_full_close(azmod_state *s);
static bool cm_animating(void);
static int cm_off_now(void);
static void cm_master_toggle(void);
/* the preview card's own Reload square (screen_community.c cm_draw_card): its rect, and the
 * tap that sends the same reload request the full screen's Reload does */
static void cm_card_reload_rect(const azmod_fb *fb, int *x, int *y, int *sz);
static int cm_card_rshow(void);
static int cm_card_tap_reload(void);
static void g_cm_dir_reset(void);
static bool cm_open_row(azmod_state *s, int it); /* a mod's SCREEN row: its page (screen_community.c) */
/* the Community Mods home page's touch gesture, decided by azmod_pane_drag (screen_community.c) */
static bool cm_home_live(const azmod_state *s);
static bool cm_home_scroll_to(const azmod_fb *fb, int v, int phase);
static bool cm_home_nav(int ctl);
static bool cm_home_reload(int ctl);
static int cm_home_ctl_at(azmod_state *s, const azmod_fb *fb, int tx, int ty);
static void cm_home_fire(azmod_state *s, const azmod_fb *fb, int ctl);
/* the Clock's zone list (screen_overlays.c): back to the Clock; its rows under the finger */
static void clock_tz_close(azmod_state *s);
static void clock_set_close(azmod_state *s); /* Set Time Manually: back to the Clock, no change */
static void clock_tz_pick(azmod_state *s, int row);
static int tzl_row_at(const azmod_fb *fb, int tx, int ty);
static int tzl_rowh(const azmod_fb *fb);
static void tzl_set_first(const azmod_fb *fb, int f);
static int g_tzl_first;
/* the row an Overlays, Clock or Browse View Mods screen belongs to; the screen such a row opens;
 * whether a mode is one of them (screen_overlays.c) */
static int sub_head(int mode);
static int sub_screen(int it);
static bool sub_mode(int mode);
/* leaving a group's screen: the Extras pane's scroll as it was (menu_full.c) */
static void c_group_screen_leave(const azmod_state *s);
/* Pad Colors (or another screen) opened from its Extras row: BACK returns to the Extras pane with
 * that row focused, not to the left column. AZ_COLOR_PICK -> AZ_COLORS stays its own step. */
static bool c_back_to_extras(azmod_state *s) {
    if ((s->mode != AZ_COLORS && !sub_mode(s->mode)) || s->left_sel != AZF_EXTRAS)
        return false;
    s->menu_sel = s->mode == AZ_COLORS ? AZI_PAD_COLORS : sub_head(s->mode);
    c_group_screen_leave(s);
    s->mode = AZ_OPEN;
    s->pane = 1;
    return true;
}
bool azmod_back(azmod_state *s) {
    if (g_docked && s->menu_focused && s->mode != AZ_CLOSED) {   /* full-screen menu */
        if (s->mode == AZ_COLOR_PICK) {
            s->mode = AZ_COLORS;
            s->menu_sel = colors_sel_to_row(s->color_sel);
            return true;
        }
        if (s->mode == AZ_CLOCK_TZ) { /* the zone list -> the Clock */
            clock_tz_close(s);
            return true;
        }
        if (s->mode == AZ_CLOCK_SET) { /* the time picker -> the Clock */
            clock_set_close(s);
            return true;
        }
        if (s->mode == AZ_COMMUNITY && g_cm_full) {               /* full screen: a page -> its list -> out */
            if (cm_animating()) return true;
            if (!cm_back()) cm_full_close(s);
            return true;
        }
        if (c_back_to_extras(s)) return true;                     /* Pad Colors -> Extras */
        if (s->pane) {
            c_select(s, s->left_sel);
            return true;
        }   /* right pane -> the list */
        return false;                                             /* the list: azmod.c unfocuses */
    }
    switch (s->mode) {
    case AZ_OPEN:
        s->mode = AZ_CLOSED;
        return true;
    case AZ_COLOR_PICK:
        s->mode = AZ_COLORS;
        s->menu_sel = colors_sel_to_row(s->color_sel);
        return true;
    case AZ_CLOCK_TZ:
        clock_tz_close(s);
        return true;
    case AZ_CLOCK_SET:
        clock_set_close(s);
        return true;
    case AZ_OVERLAYS:
    case AZ_CLOCK:
    case AZ_BROWSE_VIEW:
        s->menu_sel = sub_head(s->mode);
        s->mode = AZ_OPEN;
        return true;
    case AZ_COLORS:
        s->mode = AZ_OPEN;
        s->menu_sel = AZI_PAD_COLORS;
        return true; /* back to its Extras row */
    case AZ_INFO:
    case AZ_UPDATE:
    case AZ_COMMUNITY:
        s->mode = AZ_OPEN;
        s->menu_sel = 0;
        return true;
    default:
        return false; /* AZ_CLOSED: nothing open, let EP147 handle it */
    }
}

/* Sub-screens have no rows: the deck's physical BACK button leaves them
 * (azmod_back). Returns true so EP147 never sees the input underneath while one
 * is open. */
static bool input_subscreen(azmod_state *s, const azmod_fb *fb, int touch_x, int touch_y,
                            bool touched, int enc_delta, bool enc_click) {
    (void) s;
    (void) fb;
    (void) touch_x;
    (void) touch_y;
    (void) touched;
    (void) enc_delta;
    (void) enc_click;
    return true;
}
