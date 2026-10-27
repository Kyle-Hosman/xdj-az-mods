/* SPDX-License-Identifier: GPL-3.0-only */
/* The Clock's time zone list (screen_overlays.c, AZ_CLOCK_TZ): the Clock screen's Time Zone row opens
 * it instead of stepping to the next zone.
 *   - the rows: every zone once, under its region's heading, regions in order;
 *   - opening: the current zone is the selected row and on screen, with its check;
 *   - the knob: steps through the zones (headings skipped), the list scrolls to keep the
 *     selection in view, both ways and through both ends; a click picks the zone and goes back
 *     to the Clock screen on its row; BACK goes back without a change;
 *   - touch (the full-screen menu, decided on the release): a drag scrolls the list a whole row
 *     at a time and picks nothing, a tap on a zone picks it, a tap on a heading does nothing;
 *   - the scroll bar: drawn while the list overflows.
 * menu.c is #included (MENU_C) for its statics. */
/* the zone list without offsets: no dependence on the host's zone files or the date */
#define AZ_ZONEINFO "/nonexistent-zoneinfo"
#define AZ_USBROOT "/nonexistent-usb" /* no stick: the picker starts at the range's start */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "check.h"
#include MENU_C

/* ---- text: what was drawn and where ------------------------------------------------- */
static struct {
    int y, h;
    char s[64];
} g_txt[512];
static int g_ntxt;
int azmod_ttf_menu(const azmod_fb *fb, int x, int y, int w, int h, const char *s, int px, int track, int bold, uint32_t c) {
    (void) fb;
    (void) x;
    (void) w;
    (void) px;
    (void) track;
    (void) bold;
    (void) c;
    if (g_ntxt < 512) {
        g_txt[g_ntxt].y = y;
        g_txt[g_ntxt].h = h;
        snprintf(g_txt[g_ntxt].s, 64, "%s", s);
        g_ntxt++;
    }
    return 1;
}
int azmod_ttf_menu2(const azmod_fb *fb, int x, int y, int w, int h, const char *s, int px, int track, int bold,
                    uint32_t c, int split, uint32_t c2) {
    (void) split;
    (void) c2;
    return azmod_ttf_menu(fb, x, y, w, h, s, px, track, bold, c);
}
static int seen(const char *s) {
    for (int i = 0; i < g_ntxt; i++)
        if (!strcmp(g_txt[i].s, s)) return 1;
    return 0;
}

/* ---- a test theme: azm_theme must reach the scroll bar like every other pixel ------------ */
static int g_theme_on;
int azmod_dm_on(void) { return g_theme_on; }
uint32_t azmod_dm_map(uint32_t c) { return (c & 0xff000000u) | (0x00ffffffu - (c & 0x00ffffffu)); }
int azmod_dm_key(void) { return g_theme_on; }

/* ---- the community core: one loaded mod, "testmod", with a screen ------------------------ */
static int g_enter;
int azcm_count(void) { return 1; }
int azcm_info(int i, azcm_info_t *out) {
    if (i != 0) return -1;
    memset(out, 0, sizeof *out);
    snprintf(out->id, sizeof out->id, "testmod");
    snprintf(out->name, sizeof out->name, "Test Mod");
    out->state = AZCM_LOADED;
    out->has_screen = 1;
    return 0;
}
void azcm_screen_draw(int i, az_draw *d, az_rect area) {
    if (i == 0) azcm_draw_text(d, area.x + 10, area.y + 10, "TESTMOD SCREEN", 20, 0x00FFFFFFu);
}
int azcm_screen_input(int i, const az_event *ev) {
    (void) i;
    (void) ev;
    return 0;
}
void azcm_screen_enter(int i) {
    if (i == 0) g_enter++;
}

#define W 1280
#define H 800
static uint8_t g_px[W * H * 4];
static azmod_fb g_fb = {g_px, W, H, W * 4};
static uint32_t px_at(int x, int y) { return *(uint32_t *) (g_px + (size_t) y * W * 4 + (size_t) x * 4) & 0xffffffu; }
static void draw(azmod_state *s) {
    memset(g_px, 0, sizeof g_px);
    g_ntxt = 0;
    azmod_composite(s, &g_fb);
}
static void knob(azmod_state *s, int d, bool click) { azmod_input(s, &g_fb, 0, 0, false, d, click); }
static long g_t = 1000;
/* a touch as the deck sends it: press, then release (no motion between) */
static int touch(azmod_state *s, int x0, int y0, int x1, int y1, long ms) {
    g_t += 1000;
    azmod_menu_clock(g_t);
    azmod_input(s, &g_fb, x0, y0, true, 0, false);
    azmod_menu_clock(g_t + ms);
    return azmod_pane_drag(s, &g_fb, x1, y1, 2);
}
/* the same with motion events on the way (the emulator's stream) */
static int touch_moving(azmod_state *s, int x0, int y0, int x1, int y1) {
    g_t += 1000;
    azmod_menu_clock(g_t);
    azmod_input(s, &g_fb, x0, y0, true, 0, false);
    for (int k = 1; k <= 4; k++) {
        azmod_menu_clock(g_t + 10 * k);
        azmod_pane_drag(s, &g_fb, x0 + (x1 - x0) * k / 4, y0 + (y1 - y0) * k / 4, 1);
    }
    azmod_menu_clock(g_t + 50);
    return azmod_pane_drag(s, &g_fb, x1, y1, 2);
}

/* the Clock screen of the full menu, on its Time Zone row */
static void to_tz_row(azmod_state *s) {
    azmod_set_docked(true);
    s->menu_focused = 1;
    azmod_menu_reset(s);
    s->mode = AZ_CLOCK;
    s->left_sel = AZF_EXTRAS;
    s->pane = 1;
    int ovr[OVERLAYS_MAX], n = overlays_rows(AZF_CLOCK, ovr);
    s->menu_sel = 0;
    for (int r = 0; r < n; r++)
        if (ovr[r] == C_TZ_ROW) s->menu_sel = r;
    draw(s);
}
static int tz_row_index(void) {
    int ovr[OVERLAYS_MAX], n = overlays_rows(AZF_CLOCK, ovr);
    for (int r = 0; r < n; r++)
        if (ovr[r] == C_TZ_ROW) return r;
    return -1;
}
/* the screen rect of list row r while it shows, or 0 */
static int row_rect(int r, int *x, int *y, int *w, int *h) {
    int v = tzl_visible(&g_fb);
    if (r < g_tzl_first || r >= g_tzl_first + v) return 0;
    colors_row_rect(&g_fb, r - g_tzl_first, x, y, w, h);
    return 1;
}
static int set_row_index(void) {
    int ovr[OVERLAYS_MAX], n = overlays_rows(AZF_CLOCK, ovr);
    for (int r = 0; r < n; r++)
        if (ovr[r] == C_SET_ROW) return r;
    return -1;
}
static int64_t set_req(void) { return __atomic_exchange_n(&g_azmod_clock_set_req, 0, __ATOMIC_ACQ_REL); }
static void tap_box(azmod_state *s, int row, int plus) {
    int mx, pxx, by, sz, vx, vw;
    cs_boxes(&g_fb, row, &mx, &pxx, &by, &sz, &vx, &vw);
    int x = (plus ? pxx : mx) + sz / 2;
    touch(s, x, by + sz / 2, x, by + sz / 2, 60);
}
static int zone(void) { return __atomic_load_n(&g_azmod_clock_tz, __ATOMIC_RELAXED); }
static void set_zone(int k) { __atomic_store_n(&g_azmod_clock_tz, k, __ATOMIC_RELAXED); }

int main(void) {
    ut_suite = "clock zone list";
    az_defaults();
    azmod_state s;
    azmod_state_init(&s);
    int row[TZL_MAX], n = tzl_rows(row);

    /* ---- the rows ---- */
    int once = 1, order = 1, heads = 0, last = -1, under = 1, cur_region = -1;
    for (int k = 0; k < C_TZ_N; k++) {
        int c = 0;
        for (int r = 0; r < n; r++) c += row[r] == k;
        once &= c == 1;
    }
    for (int r = 0; r < n; r++) {
        if (row[r] < 0) {
            heads++;
            order &= -1 - row[r] > last;
            last = cur_region = -1 - row[r];
        } else
            under &= C_TZ[row[r]].region == cur_region;
    }
    CHECK("every zone is listed once", once && n == C_TZ_N + TZR_N);
    CHECK("each region once, in order, its zones under its heading", heads == TZR_N && order && under && row[0] == -1 - TZR_UTC);

    /* ---- the Clock's own screen, under Extras ---- */
    {
        int cr[OVERLAYS_MAX], cn = overlays_rows(AZF_CLOCK, cr), orow[OVERLAYS_MAX], on = overlays_rows(AZF_OVERLAYS, orow), clean = 1;
        for (int r = 0; r < on; r++)
            clean &= orow[r] != AZF_CLOCK && orow[r] != AZF_CLOCK_24H && orow[r] != AZF_CLOCK_SECONDS && orow[r] != C_TZ_ROW;
        CHECK("the Clock row is Extras' and opens its own screen", az_parent(AZF_CLOCK) == AZF_EXTRAS && sub_screen(AZF_CLOCK) == AZ_CLOCK);
        CHECK("its screen: the switch, the time format, the seconds, the time zone, the time", cn == 5 && cr[0] == AZF_CLOCK && cr[1] == AZF_CLOCK_24H && cr[2] == AZF_CLOCK_SECONDS && cr[3] == C_TZ_ROW && cr[4] == C_SET_ROW);
        CHECK("Overlays has none of the Clock's rows", clean);
    }

    /* ---- opening ---- */
    int denver = azmod_clock_tz_index("America/Denver");
    set_zone(denver);
    to_tz_row(&s);
    CHECK("the Clock screen shows the zone's name on its row", seen("Denver"));
    knob(&s, 0, true);
    draw(&s);
    int x, y, w, h;
    CHECK("the row opens the list (the zone does not change)", s.mode == AZ_CLOCK_TZ && zone() == denver);
    CHECK("the current zone is selected and shows", row[s.menu_sel] == denver && row_rect(s.menu_sel, &x, &y, &w, &h));
    CHECK("the list's headings show", seen("UTC") && seen("Americas") && seen("Time Zone"));

    /* ---- the knob ---- */
    int skips = 1, shows = 1, wrapped = 0, prev = s.menu_sel;
    for (int k = 0; k < 2 * C_TZ_N; k++) {
        knob(&s, 1, false);
        skips &= row[s.menu_sel] >= 0;
        shows &= row_rect(s.menu_sel, &x, &y, &w, &h);
        wrapped |= s.menu_sel < prev;
        prev = s.menu_sel;
    }
    for (int k = 0; k < 2 * C_TZ_N; k++) {
        knob(&s, -1, false);
        skips &= row[s.menu_sel] >= 0;
        shows &= row_rect(s.menu_sel, &x, &y, &w, &h);
    }
    CHECK("the knob steps through the zones only, the selection always on screen, through both ends", skips && shows && wrapped);
    for (int k = 0; k < 2 * n && (s.menu_sel < 0 || s.menu_sel >= n || row[s.menu_sel] != azmod_clock_tz_index("Europe/London")); k++)
        knob(&s, 1, false);
    knob(&s, 0, true);
    CHECK("a click picks the zone, saved, back on the Clock's Time Zone row",
          zone() == azmod_clock_tz_index("Europe/London") && s.mode == AZ_CLOCK && s.menu_sel == tz_row_index() &&
              s.config_dirty);
    knob(&s, 0, true);
    knob(&s, 1, false);
    azmod_back(&s);
    CHECK("BACK goes back to the Clock without a change", s.mode == AZ_CLOCK && s.menu_sel == tz_row_index() &&
                                                              zone() == azmod_clock_tz_index("Europe/London"));

    /* ---- touch, the full-screen menu ---- */
    set_zone(0);
    to_tz_row(&s);
    knob(&s, 0, true);
    draw(&s);
    CHECK("UTC current: the list starts at the top", g_tzl_first == 0);
    int vis = tzl_visible(&g_fb), rh = tzl_rowh(&g_fb);
    CHECK("the list overflows the screen", n > vis);
    colors_row_rect(&g_fb, 1, &x, &y, &w, &h);
    int cx = x + w / 2, cy = y + h / 2;
    int r = touch(&s, cx, cy + 3 * rh, cx, cy, 80); /* three rows up */
    CHECK("a drag scrolls the list whole rows and picks nothing", g_tzl_first == 3 && s.mode == AZ_CLOCK_TZ && zone() == 0 && r == 1);
    touch_moving(&s, cx, cy + 2 * rh, cx, cy);
    CHECK("with motion: likewise", g_tzl_first == 5 && s.mode == AZ_CLOCK_TZ && zone() == 0);
    touch(&s, cx, cy, cx, cy - 100 * rh, 80);
    CHECK("never past the end", g_tzl_first == n - vis);
    touch(&s, cx, cy, cx, cy + 100 * rh, 80);
    CHECK("never before the top", g_tzl_first == 0);
    /* a heading: row 1 is Americas' (row 0 is UTC's heading, row 1 its zone...): find one */
    int hr = -1;
    for (int k = 1; k < vis; k++)
        if (row[k] < 0) {
            hr = k;
            break;
        }
    row_rect(hr, &x, &y, &w, &h);
    r = touch(&s, x + w / 2, y + h / 2, x + w / 2, y + h / 2, 60);
    CHECK("a tap on a heading picks nothing", s.mode == AZ_CLOCK_TZ && zone() == 0);
    row_rect(hr + 1, &x, &y, &w, &h);
    r = touch(&s, x + w / 2, y + h / 2, x + w / 2, y + h / 2, 60);
    CHECK("a tap on a zone picks it and goes back to the Clock", r == 2 && zone() == row[hr + 1] && s.mode == AZ_CLOCK);

    /* ---- the scroll bar ---- */
    set_zone(0);
    to_tz_row(&s);
    knob(&s, 0, true);
    draw(&s);
    colors_row_rect(&g_fb, 0, &x, &y, &w, &h);
    int bar = 0;
    for (int yy = y; yy < y + vis * h; yy++) bar += px_at(x + w - 4, yy) != px_at(x + w - 30, yy);
    CHECK("the scroll bar is drawn while the list overflows", bar > 8);

    /* ---- Set Time Manually ---- */
    set_zone(0);
    to_tz_row(&s);
    s.clock_text[0] = 0;
    draw(&s);
    CHECK("the Clock's last row: Set Time Manually, Not set while the time is not known",
          seen("Set Time Manually") && seen("Not set"));
    snprintf(s.clock_text, sizeof s.clock_text, "7:36 PM");
    draw(&s);
    CHECK("... and the time once it is", seen("7:36 PM") && !seen("Not set"));
    s.clock_text[0] = 0;
    knob(&s, 1, false);
    knob(&s, 0, true);
    CHECK("its row opens the picker, at the range's start without a time or a stick",
          s.mode == AZ_CLOCK_SET && g_cset.v[CS_YEAR] == CS_YEAR_MIN && g_cset.v[CS_MONTH] == 1 && g_cset.v[CS_DAY] == 1 &&
              g_cset.v[CS_HOUR] == 0 && g_cset.v[CS_MINUTE] == 0);
    draw(&s);
    CHECK("its rows show, with the zone the time is in", seen("Year") && seen("Minute") && seen("Set Time") && seen("UTC"));
    knob(&s, 0, true);
    knob(&s, 2, false);
    CHECK("a press edits the row: the knob steps its value", g_cset.v[CS_YEAR] == CS_YEAR_MIN + 2 && s.menu_sel == 0);
    knob(&s, 0, true);
    knob(&s, 1, false);
    CHECK("a second press ends editing: the knob moves to the next row", s.menu_sel == 1 && g_cset.v[CS_YEAR] == CS_YEAR_MIN + 2);
    tap_box(&s, CS_MINUTE, 1);
    tap_box(&s, CS_MINUTE, 1);
    tap_box(&s, CS_HOUR, 0);
    CHECK("a tap on plus or minus steps that row once", g_cset.v[CS_MINUTE] == 2 && g_cset.v[CS_HOUR] == 23 && s.mode == AZ_CLOCK_SET);
    set_req();
    azmod_back(&s);
    CHECK("BACK goes back to the Clock's row and sets nothing", s.mode == AZ_CLOCK && s.menu_sel == set_row_index() && set_req() == 0);
    knob(&s, 0, true);
    knob(&s, 1, false);
    knob(&s, 0, true); /* edit the month */
    knob(&s, 1, false);
    knob(&s, 0, true);
    knob(&s, -2, false); /* Month -> Year -> Set Time (wraps) */
    knob(&s, 0, true);
    CHECK("Set Time hands over the UTC time and goes back to the Clock",
          set_req() == CS_T_MIN + 31 * 86400 && s.mode == AZ_CLOCK && s.menu_sel == set_row_index());
    set_zone(azmod_clock_tz_index("America/Denver")); /* no zone files here: the zone cannot answer */
    knob(&s, 0, true);
    draw(&s);
    knob(&s, -1, false);
    knob(&s, 0, true);
    CHECK("a zone that cannot be read: shown, and Set Time sets nothing", seen("Zone unavailable") && set_req() == 0 && s.mode == AZ_CLOCK_SET);
    set_zone(0);
    return ut_summary();
}
