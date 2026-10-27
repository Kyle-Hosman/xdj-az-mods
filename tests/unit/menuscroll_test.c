/* SPDX-License-Identifier: GPL-3.0-only */
/* The Mod Menu's lists with more rows than fit (20 rows registered by two mods): the rows keep
 * the height the official rows give and the list SCROLLS, with EP147's own scroll bar.
 *   - row height: the full menu's left list and the compact list unchanged by the mods' rows; the
 *     Updates screen (which hangs from the compact row height) does not move;
 *   - the knob: the selection fully visible after every step, both ways, through both ends;
 *   - the offset: never negative, never past the end, no blank tail at the end;
 *   - touch: a drag scrolls and never selects or toggles (the deck's stream: press, release, no
 *     motion; and with motion), a tap selects, a tap on a checkbox toggles;
 *   - the scroll bar: hidden while the list fits, its thumb proportional, inside the track, at
 *     the top at offset 0 and at the bottom at the end, through the active theme's map;
 *   - a registered row's icon while scrolled: on its own row, never over the header, never past
 *     the list's bottom edge;
 *   - a mod's page opened from its row (knob and tap) and closed: the list comes back on the same
 *     entry, pane and offset;
 *   - the preview card: whole lines only, "+N more" when the rows do not fit.
 * menu.c is #included (MENU_C) for its statics; the community core is this file's stand-in
 * (azcm_*; lanetest.c's are renamed away by run.sh), and so are the theme hooks (azmod_dm_*). */
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
static long g_t = 1000;
/* the lists' glides run on the menu's clock here, which the test steps */
static long glide_clock(void) { return g_mnow; }
/* the menu's tick at 60 Hz until both lists' glides have stopped */
static void glide_settle(azmod_state *s) {
    for (int k = 0; k < 600 && (g_lgl.on || g_rgl.on); k++) {
        azmod_menu_clock(g_mnow + 16);
        azmod_tick(s);
    }
    if (g_t < g_mnow) g_t = g_mnow;
}
static void glide_tick(azmod_state *s) {
    azmod_menu_clock(g_mnow + 16);
    azmod_tick(s);
}
static void knob_step(azmod_state *s, int d, bool click) { azmod_input(s, &g_fb, 0, 0, false, d, click); }
/* a knob step, then its glide to the end */
static void knob(azmod_state *s, int d, bool click) {
    knob_step(s, d, click);
    glide_settle(s);
}
/* a touch as the deck sends it: press, then release (no motion between) */
static int touch(azmod_state *s, int x0, int y0, int x1, int y1, long ms) {
    g_t += 1000;
    azmod_menu_clock(g_t);
    azmod_input(s, &g_fb, x0, y0, true, 0, false);
    azmod_menu_clock(g_t + ms);
    return azmod_pane_drag(s, &g_fb, x1, y1, 2);
}
/* the same with motion events on the way (the emulator's stream), 10 ms apart; the lift comes
 * lift_ms after the last one, at the same point */
static int touch_moving_lift(azmod_state *s, int x0, int y0, int x1, int y1, long lift_ms) {
    g_t += 1000;
    azmod_menu_clock(g_t);
    azmod_input(s, &g_fb, x0, y0, true, 0, false);
    for (int k = 1; k <= 4; k++) {
        azmod_menu_clock(g_t + 10 * k);
        azmod_pane_drag(s, &g_fb, x0 + (x1 - x0) * k / 4, y0 + (y1 - y0) * k / 4, 1);
    }
    azmod_menu_clock(g_t + 40 + lift_ms);
    return azmod_pane_drag(s, &g_fb, x1, y1, 2);
}
static int touch_moving(azmod_state *s, int x0, int y0, int x1, int y1) {
    return touch_moving_lift(s, x0, y0, x1, y1, 10);
}
static void settle(azmod_state *s) {
    for (int k = 0; k < 400 && (azmod_tick(s), cm_animating()); k++) {
        draw(s);
        struct timespec t = {0, 5 * 1000000L};
        nanosleep(&t, NULL);
    }
    draw(s);
}
static int vtop(void) { return dock_y(&g_fb) + menu_hdrh(&g_fb); }
static int vbot(void) { return dock_y(&g_fb) + dock_h(&g_fb); }
static int idx_of(int it) {
    int t[AZI_COUNT], n = c_tops(t);
    for (int k = 0; k < n; k++)
        if (t[k] == it) return k;
    return -1;
}
static int sel_visible(const azmod_state *s) {
    int x, y, w, h;
    c_left_rect(&g_fb, idx_of(s->left_sel), &x, &y, &w, &h);
    return y >= vtop() && y + h <= vbot();
}
static int bounds_ok(void) { return g_lscroll >= 0 && g_lscroll <= c_lmax(&g_fb); }
/* the drawn thumb: the rows of column lw-5 that differ from the row background at lw-12 */
static int thumb(int *y0, int *len) {
    int lw = c_lw(&g_fb), n = 0, first = -1, last = -1;
    for (int y = vtop(); y < vbot(); y++)
        if (px_at(lw - 5, y) != px_at(lw - 12, y)) {
            if (first < 0) first = y;
            last = y;
            n++;
        }
    *y0 = first;
    *len = first < 0 ? 0 : last - first + 1;
    return n;
}
/* every value a tap could change */
static int g_snap[AZI_COUNT];
static void snap(void) {
    for (int i = 0; i < AZI_COUNT; i++) g_snap[i] = az_has_value(i) ? az_get(i) : 0;
}
static int unchanged(void) {
    for (int i = 0; i < AZI_COUNT; i++)
        if ((az_has_value(i) ? az_get(i) : 0) != g_snap[i]) return 0;
    return 1;
}
static void open_full(azmod_state *s) {
    azmod_set_docked(true);
    s->menu_focused = 1;
    s->mode = AZ_OPEN;
    g_last_left = -1;
    azmod_menu_reset(s);
}

static const az_screen_v1 PAGE = {sizeof PAGE, NULL, NULL, NULL, NULL};
static unsigned char g_icon[AZ_ROW_ICON_N * AZ_ROW_ICON_N];

int main(void) {
    ut_suite = "menu scroll";
    az_defaults();
    azmod_state s;
    azmod_state_init(&s);
    g_glide_ms = glide_clock;

    /* ---- before any mod row: today's geometry, and no scroll bar ---- */
    open_full(&s);
    int rh0 = c_lrowh(&g_fb), n0;
    {
        int t[AZI_COUNT];
        n0 = c_tops(t);
    }
    upd_geo u0;
    int cx0, cy0, cw0, ch0;
    g_full = 1;
    upd_band(&g_fb, &u0);
    changelog_rect(&g_fb, &cx0, &cy0, &cw0, &ch0);
    azmod_set_docked(false);
    g_full = 0;
    int crh0 = menu_rowh_list(&g_fb);
    azmod_set_docked(true);
    draw(&s);
    int ty, tl;
    CHECK("no mod rows: the list fits, no scroll bar", c_lmax(&g_fb) == 0 && thumb(&ty, &tl) == 0);
    s.menu_focused = 0;
    draw(&s);
    s.menu_focused = 1;
    int more0 = 0;
    const char *mt0 = "";
    for (int i = 0; i < g_ntxt; i++)
        if (g_txt[i].s[0] == '+') {
            more0++;
            mt0 = g_txt[i].s;
        }
    /* the official rows are two more than the card holds (Column Edit and Preview Hot Cues live
     * on Browse View Mods' own screen, like the Overlays readouts; Key in Track Title and
     * Remember Waveform Zoom are the rows past the card), so its last line that fits says
     * "+3 more"; the registered rows below overflow it further */
    CHECK("no mod rows: the official rows end the preview card with \"+3 more\"",
          more0 == 1 && !strcmp(mt0, "+3 more"));

    /* ---- 20 registered rows: 16 from one mod (one with an icon), 4 from "testmod" (a SCREEN) ---- */
    memset(g_icon, 255, sizeof g_icon);
    az_feature_v2 f;
    char keys[16][8], labels[16][16];
    int first_reg = -1, icon_row = -1, page = -1;
    for (int i = 0; i < 16; i++) {
        snprintf(keys[i], 8, "r%d", i);
        snprintf(labels[i], 16, "Mod Row %d", i);
        /* clang-format off */
        f = (az_feature_v2){.size = sizeof f, .key = keys[i], .label = labels[i], .kind = AZ_ROW_TOGGLE,
                            .icon = i == 13 ? g_icon : NULL};
        /* clang-format on */
        int it = azf_register("bigmod", &f);
        if (i == 0) first_reg = it;
        if (i == 13) icon_row = it;
    }
    f = (az_feature_v2) {.size = sizeof f, .key = "a", .label = "Test A", .kind = AZ_ROW_TOGGLE};
    azf_register("testmod", &f);
    f = (az_feature_v2) {.size = sizeof f, .key = "b", .label = "Test B", .kind = AZ_ROW_TOGGLE};
    azf_register("testmod", &f);
    f = (az_feature_v2) {.size = sizeof f, .key = "c", .label = "Test C", .kind = AZ_ROW_TOGGLE};
    azf_register("testmod", &f);
    f = (az_feature_v2) {.size = sizeof f, .key = "page", .label = "Test Page", .kind = AZ_ROW_SCREEN, .screen = &PAGE};
    page = azf_register("testmod", &f);
    int tops[AZI_COUNT], n = c_tops(tops);
    CHECK("20 rows registered, all in the left list", first_reg >= AZI_REG0 && icon_row > 0 && page > 0 && n == n0 + 20);

    /* ---- row heights ---- */
    CHECK("left list: the row height is the official rows' (no shrinking)", c_lrowh(&g_fb) == rh0);
    CHECK("left list: it overflows, by exactly the rows past the viewport",
          c_lmax(&g_fb) == n * rh0 - c_lview(&g_fb) && c_lmax(&g_fb) > 0);
    azmod_set_docked(false);
    g_full = 0;
    CHECK("compact list: the row height is the official rows'", menu_rowh_list(&g_fb) == crh0);
    azmod_set_docked(true);
    g_full = 1;
    {
        upd_geo u1;
        int cx, cy, cw, ch;
        upd_band(&g_fb, &u1);
        changelog_rect(&g_fb, &cx, &cy, &cw, &ch);
        CHECK("the Updates screen does not move", !memcmp(&u0, &u1, sizeof u0) && cx == cx0 && cy == cy0 && cw == cw0 && ch == ch0);
    }

    /* ---- the knob, both ways, through both ends ---- */
    open_full(&s);
    draw(&s);
    CHECK("opened at the top", g_lscroll == 0 && s.left_sel == tops[0]);
    int ok_vis = 1, ok_b = 1, ok_top = 1, ok_end = 1, steps = 0;
    for (int d = 1; d >= -1; d -= 2)
        for (int k = 0; k < 2 * n + 1; k++) {
            knob(&s, d, false);
            steps++;
            ok_vis &= sel_visible(&s);
            ok_b &= bounds_ok();
            if (s.left_sel == tops[0]) ok_top &= g_lscroll == 0;
            if (s.left_sel == tops[n - 1]) ok_end &= g_lscroll == c_lmax(&g_fb);
        }
    CHECK("knob: the selection fully visible after every step, both ways", ok_vis && steps == 4 * n + 2);
    CHECK("knob: the offset never negative, never past the end", ok_b);
    CHECK("knob: the first entry shows the top", ok_top);
    CHECK("knob: the last entry shows the end", ok_end);
    while (s.left_sel != tops[n - 1]) knob(&s, -1, false);
    {
        int x, y, w, h;
        c_left_rect(&g_fb, n - 1, &x, &y, &w, &h);
        CHECK("at the end: the last row ends at the list's bottom (no blank tail)", y + h == vbot());
    }

    /* ---- the scroll bar ---- */
    int lw = c_lw(&g_fb), area = c_lview(&g_fb) - 4, content = n * rh0, view = c_lview(&g_fb);
    draw(&s);
    int got = thumb(&ty, &tl);
    CHECK("at the end: the thumb ends at the track's end", got == tl && ty + tl == vtop() + 1 + area - 1);
    knob(&s, 1, false); /* wraps to the top */
    draw(&s);
    got = thumb(&ty, &tl);
    long want = ((long) area * view + content / 2) / content - 2;
    CHECK("at the top: the thumb starts 2 px under the header", got == tl && ty == vtop() + 2);
    CHECK("the thumb's length is proportional (visible / total)", tl == want && tl > 0 && tl < area);
    CHECK("the thumb: 5 px wide, 2 px inside the list's right edge",
          px_at(lw - 7, ty + 10) != px_at(lw - 12, ty + 10) && px_at(lw - 3, ty + 10) != px_at(lw - 12, ty + 10) &&
              px_at(lw - 8, ty + 10) == px_at(lw - 12, ty + 10) && px_at(lw - 2, ty + 10) == px_at(lw - 12, ty + 10));
    { /* gray 0x808080 at alpha 160 over the row */
        uint32_t bg = px_at(lw - 12, ty + 10), p = px_at(lw - 5, ty + 10), e = 0;
        for (int sh = 0; sh <= 16; sh += 8) e |= (((0x80u * 160) + ((bg >> sh) & 0xff) * 95) / 255) << sh;
        CHECK("the thumb: gray 0x808080 at alpha 160 over the rows", p == e);
    }
    g_lscroll = c_lmax(&g_fb) / 2;
    draw(&s);
    got = thumb(&ty, &tl);
    CHECK("half way: the thumb half way down its track",
          got == tl && abs((ty - 1 - (vtop() + 1)) - (area - (tl + 2)) / 2) <= 1);
    g_theme_on = 1;
    draw(&s);
    {
        uint32_t bg = px_at(lw - 12, ty + 10), p = px_at(lw - 5, ty + 10), c = azmod_dm_map(VSB_RGB) & 0xffffff, e = 0;
        for (int sh = 0; sh <= 16; sh += 8) e |= ((((c >> sh) & 0xff) * 160 + ((bg >> sh) & 0xff) * 95) / 255) << sh;
        CHECK("theme: the thumb's gray goes through the theme's map", p == e && thumb(&ty, &tl) > 0);
    }
    g_theme_on = 0;

    /* ---- touch ---- */
    g_lscroll = 0;
    draw(&s);
    int sel0 = s.left_sel, x, y, w, h;
    c_left_rect(&g_fb, 3, &x, &y, &w, &h);
    snap();
    int r = touch(&s, 200, y + h / 2, 200, y + h / 2 - 300, 120);
    CHECK("drag (no motion, the deck): the list scrolls with the finger", g_lscroll == 300);
    CHECK("drag: nothing selected, nothing toggled", s.left_sel == sel0 && unchanged() && r != 2);
    r = touch_moving(&s, 200, y + h / 2, 200, y + h / 2 + 200);
    CHECK("drag (with motion): the list scrolls back by the finger", g_lscroll == 100 && s.left_sel == sel0 && unchanged() && r != 2);
    r = touch(&s, 100, vtop() + 30, 400, vtop() + 30, 80);
    CHECK("a sideways drag along one row: no select", s.left_sel == sel0 && unchanged() && r != 2);
    {
        int k = idx_of(first_reg), bx, by, bs;
        g_lscroll = 0;
        c_lshow(&g_fb, k);
        CHECK("a registered row has a checkbox", c_left_box(&g_fb, first_reg, k, &bx, &by, &bs));
        int sc = g_lscroll;
        r = touch(&s, bx + bs / 2, by + bs / 2, bx + bs / 2, by + bs / 2 - 150, 80);
        CHECK("a drag that starts on a checkbox scrolls and does not toggle", unchanged() && g_lscroll == sc + 150 && r != 2);
        g_lscroll = sc;
        r = touch(&s, bx + bs / 2 - 14, by + bs / 2, bx + bs / 2 + 14, by + bs / 2, 80);
        CHECK("a 28 px drag across a checkbox does not toggle", unchanged() && r != 2);
        r = touch(&s, bx + bs / 2, by + bs / 2, bx + bs / 2 + 2, by + bs / 2 + 1, 80);
        CHECK("a tap on that checkbox toggles it (the other answer)", r == 2 && az_get(first_reg) != g_snap[first_reg] && s.left_sel == first_reg);
        az_toggle(first_reg);
    }
    {
        int k = idx_of(tops[n0 + 2]);
        c_lshow(&g_fb, k);
        c_left_rect(&g_fb, k, &x, &y, &w, &h);
        r = touch(&s, 150, y + h / 2, 151, y + h / 2 + 2, 600);
        CHECK("a tap on a row selects it (also a slow one)", r == 2 && s.left_sel == tops[k] && s.pane == 0);
    }

    /* ---- an icon row while scrolled ---- */
    {
        int k = idx_of(icon_row);
        g_lscroll = 0;
        draw(&s);
        uint8_t *hdr0 = malloc((size_t) W * 4 * menu_hdrh(&g_fb));
        for (int yy = 0; yy < menu_hdrh(&g_fb); yy++) memcpy(hdr0 + (size_t) yy * W * 4, g_px + (size_t) (dock_y(&g_fb) + yy) * W * 4, (size_t) lw * 4);
        int icw = W * 88 / 1280, ok_hdr = 1, ok_bot = 1, ok_row = 1;
        for (int pos = 0; pos < 3; pos++) {
          /* the icon row fully in view, half under the header, half past the bottom */
            int ry = k * rh0;
            g_lscroll = pos == 0 ? ry - 2 * rh0 : pos == 1 ? ry + rh0 / 2
                                                           : ry + rh0 / 2 - c_lview(&g_fb);
            c_lclamp(&g_fb);
            draw(&s);
            for (int yy = 0; yy < menu_hdrh(&g_fb); yy++)
                ok_hdr &= !memcmp(hdr0 + (size_t) yy * W * 4, g_px + (size_t) (dock_y(&g_fb) + yy) * W * 4, (size_t) lw * 4);
            for (int yy = vbot(); yy < H; yy++)
                for (int xx = 0; xx < lw; xx++) ok_bot &= px_at(xx, yy) == 0;
            c_left_rect(&g_fb, k, &x, &y, &w, &h);
            int ih = h - (h / 14 < 2 ? 2 : h / 14), isz = ih * 3 / 5, iy = y + (ih - isz) / 2 + isz / 2;
            /* a point the solid test icon covers and a placeholder (a square with a plus, on the
             * rows around it) leaves empty: off the plus's arms, inside the square's outline */
            int qx = icw / 2 + isz / 4, qy = iy - isz / 4;
            if (pos == 0) ok_row &= px_at(qx, qy) == C_TEXT && px_at(qx, qy - rh0) != C_TEXT && px_at(qx, qy + rh0) != C_TEXT;
        }
        free(hdr0);
        CHECK("icon: drawn on its own row (and not on its neighbors)", ok_row);
        CHECK("icon: never drawn over the header (the header is the same at every offset)", ok_hdr);
        CHECK("icon: nothing drawn past the list's bottom edge", ok_bot);
    }

    /* ---- a mod's page from its row, and back ---- */
    open_full(&s);
    while (s.left_sel != page) knob(&s, 1, false);
    int keep = g_lscroll, e0 = g_enter;
    CHECK("the page row is scrolled into view", keep > 0 && sel_visible(&s));
    knob(&s, 0, true);
    settle(&s);
    CHECK("knob press: the mod's page opens", s.mode == AZ_COMMUNITY && g_cm_full && g_enter == e0 + 1 && seen("TESTMOD SCREEN"));
    azmod_back(&s);
    settle(&s);
    CHECK("BACK: on the same entry, pane and offset", s.mode == AZ_OPEN && !g_cm_full && s.left_sel == page && s.pane == 0 && g_lscroll == keep);
    {
        int k = idx_of(page);
        c_left_rect(&g_fb, k, &x, &y, &w, &h);
        r = touch(&s, 150, y + h / 2, 150, y + h / 2, 60);
        settle(&s);
        CHECK("tap: the mod's page opens", s.mode == AZ_COMMUNITY && g_cm_full && g_enter == e0 + 2);
        cm_ctl ctl[CM_MAXCTL];
        int nc = cm_layout(&s, &g_fb, ctl, 0);
        touch(&s, ctl[nc - 1].x + ctl[nc - 1].w / 2, ctl[nc - 1].y + ctl[nc - 1].h / 2, ctl[nc - 1].x + ctl[nc - 1].w / 2, ctl[nc - 1].y + ctl[nc - 1].h / 2, 60);
        settle(&s);
        CHECK("back square: on the same entry and offset", s.mode == AZ_OPEN && !g_cm_full && s.left_sel == page && g_lscroll == keep);
    }

    /* ---- the compact list ---- */
    azmod_set_docked(false);
    g_full = 0;
    s.mode = AZ_OPEN;
    s.menu_sel = 0;
    draw(&s);
    int vt, vb, cok = 1, cb = 1, cend = 0;
    c_cview(&g_fb, &vt, &vb);
    CHECK("compact: it overflows", c_cmax(&g_fb) > 0);
    for (int d = 1; d >= -1; d -= 2)
        for (int k = 0; k < 140; k++) {
            knob(&s, d, false);
            azmod_item_rect(&g_fb, s.menu_sel, &x, &y, &w, &h);
            cok &= y >= vt && y + h <= vb;
            cb &= g_cscroll >= 0 && g_cscroll <= c_cmax(&g_fb);
            cend |= g_cscroll == c_cmax(&g_fb);
        }
    CHECK("compact knob: the selection fully visible after every step, both ways", cok);
    CHECK("compact knob: the offset in bounds, and it reaches the end", cb && cend);
    draw(&s);
    {
        int got2 = 0, cwid = menu_content_w(&g_fb);
        for (int yy = vt; yy < vb; yy++) got2 += px_at(cwid - 5, yy) != px_at(cwid - 1, yy) && px_at(cwid - 5, yy) != 0;
        CHECK("compact: the scroll bar shows", got2 > 0);
    }
    /* scrolled to the end, no row reaches into the version footer (drawn bottom-right over the list) */
    {
        g_cscroll = c_cmax(&g_fb);
        draw(&s);
        int fy = -1, clash = 0;
        for (int i = 0; i < g_ntxt; i++)
            if (!strncmp(g_txt[i].s, "AZ-MODS", 7)) fy = g_txt[i].y;
        for (int i = 0; i < AZI_COUNT; i++) {
            if (item_hidden(i)) continue;
            azmod_item_rect(&g_fb, i, &x, &y, &w, &h);
            if (y < vb && y + h > vt) clash |= fy >= 0 && y + h > fy;
        }
        CHECK("compact end: the footer is drawn and no row reaches under it", fy >= 0 && !clash);
    }
    azmod_set_docked(true);

    /* ---- the glide (scroll_glide.h, as on the Community Mods home page) ---- */
    open_full(&s);
    draw(&s);
    {
        int lrh = c_lrowh(&g_fb), ly = vtop() + 8 * lrh + lrh / 2, sel = s.left_sel;
        /* the left list: a lift while moving flings it on */
        g_lscroll = 0;
        snap();
        r = touch_moving(&s, 200, ly, 200, ly - 200);
        int at = g_lscroll;
        CHECK("glide: on a moving lift the list is at the finger, nothing selected",
              at == 200 && s.left_sel == sel && unchanged() && r != 2 && g_lgl.on);
        glide_settle(&s);
        CHECK("glide: a moving lift flings the list on; it stops, in range", g_lscroll > at && bounds_ok() && !g_lgl.on);
        /* a finger that stopped before lifting: no fling (the other answer) */
        g_lscroll = 0;
        r = touch_moving_lift(&s, 200, ly, 200, ly - 200, 200);
        glide_settle(&s);
        CHECK("glide: a finger that stopped before the lift does not fling", g_lscroll == 200 && r != 2);
        /* a finger on a flinging list catches it and selects nothing */
        g_lscroll = 0;
        touch_moving(&s, 200, ly, 200, ly - 200);
        glide_tick(&s);
        int caught = g_lscroll, tx = 200, ty = vtop() + 2 * lrh + lrh / 2;
        r = touch(&s, tx, ty, tx, ty, 60);
        glide_settle(&s);
        CHECK("glide: a tap on a flinging list stops it where it shows and selects nothing",
              caught > 200 && g_lscroll == caught && s.left_sel == sel && unchanged() && r != 2);
        r = touch(&s, tx, ty, tx, ty, 60);
        CHECK("glide: the next tap there selects (the other answer)", r == 2 && s.left_sel != sel);
        /* a knob step glides to the row it shows */
        open_full(&s);
        draw(&s);
        knob_step(&s, -1, false); /* wraps to the last entry: the end of the list */
        CHECK("glide: a knob step has not jumped; it glides", g_lscroll == 0 && g_lgl.on);
        glide_settle(&s);
        CHECK("glide: the knob's glide ends with the selection in view, at the end",
              sel_visible(&s) && g_lscroll == c_lmax(&g_fb) && !g_lgl.on);

        /* the right pane (Extras): a fling, a catch, and a new pane at its top */
        open_full(&s);
        c_select(&s, AZF_EXTRAS);
        draw(&s);
        int rx = c_lw(&g_fb) + 300, ry = vtop() + (vbot() - vtop()) * 3 / 4, rmx = c_rmax(&g_fb, AZF_EXTRAS);
        CHECK("glide: the Extras pane scrolls here", rmx > 0);
        int span = rmx < 200 ? rmx / 2 : 100;
        snap();
        r = touch_moving(&s, rx, ry, rx, ry - span);
        at = g_rscroll;
        glide_settle(&s);
        CHECK("glide: the right pane flings on after a moving lift; it stops, in range",
              at == span && g_rscroll > at && g_rscroll <= rmx && !g_rgl.on && unchanged() && r != 2);
        c_glide_put(&g_rgl, &g_rglpx, &g_rscroll, 0);
        touch_moving(&s, rx, ry, rx, ry - span);
        glide_tick(&s);
        caught = g_rscroll;
        r = touch(&s, rx, ry, rx, ry, 60);
        glide_settle(&s);
        CHECK("glide: a tap on the flinging pane stops it and fires nothing",
              g_rscroll == caught && unchanged() && r != 2);
        c_glide_put(&g_rgl, &g_rglpx, &g_rscroll, 0);
        touch_moving(&s, rx, ry, rx, ry - span);
        c_select(&s, tops[0] == AZF_EXTRAS ? tops[1] : tops[0]);
        c_select(&s, AZF_EXTRAS);
        glide_settle(&s);
        CHECK("glide: another pane stops the fling; coming back, the pane is at its top", g_rscroll == 0 && !g_rgl.on);
    }

    /* ---- Reload: a press under a second restarts, one held a full second is taken back ---- */
    {
        open_full(&s);
        draw(&s);
        int k = idx_of(AZI_RELOAD), rx, ry, rw, rh;
        c_lshow(&g_fb, k);
        c_left_rect(&g_fb, k, &x, &y, &w, &h);
        touch(&s, 150, y + h / 2, 150, y + h / 2, 60);
        draw(&s);
        int r0[AZI_COUNT], m = c_detail(c_pane(&s), r0), rk = -1;
        for (int i = 0; i < m; i++)
            if (r0[i] == AZI_RELOAD) rk = i;
        CHECK("Reload: its pane is open, with its button", s.left_sel == AZI_RELOAD && rk >= 0);
        c_detail_rect(&g_fb, c_pane(&s), rk, &rx, &ry, &rw, &rh);
        long holds[] = {60, 700, 999, 1000, 1500};
        int want[] = {1, 1, 1, 0, 0}, ok = 1;
        for (int i = 0; i < 5; i++) {
            s.last_action = -1;
            touch(&s, rx + rw / 2, ry + rh / 2, rx + rw / 2, ry + rh / 2, holds[i]);
            ok &= (s.last_action == AZI_RELOAD) == want[i];
        }
        CHECK("Reload: held 60, 700, 999 ms restarts; 1000 and 1500 ms do nothing", ok);
    }

    /* ---- the preview card ---- */
    s.menu_focused = 0;
    s.mode = AZ_OPEN;
    draw(&s);
    int past = 0, more = 0;
    for (int i = 0; i < g_ntxt; i++) {
        past |= g_txt[i].y + g_txt[i].h > vbot();
        more |= !strncmp(g_txt[i].s, "+", 1) && strstr(g_txt[i].s, " more");
    }
    CHECK("preview: no line past the pane, and a \"+N more\" line", !past && more);
    return ut_summary();
}
