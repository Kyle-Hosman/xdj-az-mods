/* SPDX-License-Identifier: GPL-3.0-only */
/* Tall sub-option rows in the Mod Menu: a CHOICE row whose buttons do not fit beside its label
 * gets twice the row height, its buttons on a line of their own under the label.
 *   - which rows are tall: Stems > Echo Length (eight values) is; every other row keeps the
 *     normal height, and the rows that fit keep the old geometry exactly (rows, banks, choices);
 *   - the stacking: rows below a tall row move down by its extra height, rows above do not move;
 *   - the buttons: inside the row's second line, from the label's left edge, apart, each wide
 *     enough for its value at the button text size (no shrinking);
 *   - the pane: the scroll range covers the tall row, the knob scrolls each row fully into view
 *     both ways, a tap on each button picks that value, a knob press cycles it, the focus band
 *     covers the whole tall row;
 *   - a registered row (a community mod's CHOICE) follows the same measured rule;
 *   - the compact list: the tall bank row takes two rows, its tiles on the second.
 * menu.c is #included (MENU_C) for its statics; the community core and the theme hooks are this
 * file's stand-ins (lanetest.c's are renamed away by run.sh), and the face's width is
 * lanetest.c's (8 px a byte). */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "check.h"
#include MENU_C

/* ---- text: what was drawn, where and at what size ---------------------------------------- */
static struct {
    int x, y, w, h, px;
    char s[64];
} g_txt[512];
static int g_ntxt;
int azmod_ttf_menu(const azmod_fb *fb, int x, int y, int w, int h, const char *s, int px, int track, int bold, uint32_t c) {
    (void) fb;
    (void) track;
    (void) bold;
    (void) c;
    if (g_ntxt < 512) {
        g_txt[g_ntxt].x = x;
        g_txt[g_ntxt].y = y;
        g_txt[g_ntxt].w = w;
        g_txt[g_ntxt].h = h;
        g_txt[g_ntxt].px = px;
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
int azmod_dm_on(void) { return 0; }
uint32_t azmod_dm_map(uint32_t c) { return c; }
int azmod_dm_key(void) { return 0; }
int azcm_count(void) { return 0; }
int azcm_info(int i, azcm_info_t *out) {
    (void) i;
    (void) out;
    return -1;
}
void azcm_screen_draw(int i, az_draw *d, az_rect area) {
    (void) i;
    (void) d;
    (void) area;
}
int azcm_screen_input(int i, const az_event *ev) {
    (void) i;
    (void) ev;
    return 0;
}
void azcm_screen_enter(int i) { (void) i; }

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
static long glide_clock(void) { return g_mnow; }
static void glide_settle(azmod_state *s) {
    for (int k = 0; k < 600 && (g_lgl.on || g_rgl.on); k++) {
        azmod_menu_clock(g_mnow + 16);
        azmod_tick(s);
    }
    if (g_t < g_mnow) g_t = g_mnow;
}
static void knob(azmod_state *s, int d, bool click) {
    azmod_input(s, &g_fb, 0, 0, false, d, click);
    glide_settle(s);
}
/* a tap as the deck sends it: press, then release at the same point */
static void tap(azmod_state *s, int x, int y) {
    g_t += 1000;
    azmod_menu_clock(g_t);
    azmod_input(s, &g_fb, x, y, true, 0, false);
    azmod_menu_clock(g_t + 60);
    azmod_pane_drag(s, &g_fb, x, y, 2);
}
static int row_of(int left, int it) {
    int r[AZI_COUNT], m = c_detail(left, r);
    for (int k = 0; k < m; k++)
        if (r[k] == it) return k;
    return -1;
}
/* the geometry before tall rows: every sub-option row c_grow_h, stacked by index */
static int old_row_y(int left, int k) {
    return c_opts_y(&g_fb, left) + g_fb.h * 32 / 800 + (k - 1) * (c_grow_h(&g_fb) + g_fb.h * 6 / 800);
}
static void old_bank_rect(int left, int k, int nb, int b, int *x, int *y, int *w) {
    int rx = dock_x(&g_fb), rw = dock_w(&g_fb), ry = old_row_y(left, k) - g_rscroll, rh = c_grow_h(&g_fb);
    int side = c_rrowh(&g_fb) * 3 / 4, gap = side / 3;
    *x = rx + rw - g_fb.w / 40 - nb * side - (nb - 1) * gap + b * (side + gap);
    *y = ry + (rh - side) / 2;
    *w = side;
}
static void old_choice_rect(int left, int k, int b, int *x, int *y, int *w) {
    int rx = dock_x(&g_fb), rw = dock_w(&g_fb), ry = old_row_y(left, k) - g_rscroll;
    int bw = g_fb.w * 132 / 1280, gap = g_fb.w * 10 / 1280;
    *x = rx + rw - g_fb.w / 40 - 2 * bw - gap + b * (bw + gap);
    *y = ry + g_fb.h * 8 / 800;
    *w = bw;
}
/* the rows above row k grow by their descriptions' further lines (menuwrap_test.c covers those) */
static int ext_above(int left, int k) {
    int r[AZI_COUNT], m = c_detail(left, r), e = 0;
    for (int j = 1; j < k && j < m; j++) e += c_row_extra(&g_fb, left, r[j]);
    return e;
}
static const char *const LONGV[] = {"Value one", "Value two", "Value three", "Value four", "Value five", "Value six"};
static const char *const SHORTV[] = {"A", "B", "C"};

int main(void) {
    ut_suite = "menu tall rows";
    az_defaults();
    azmod_state s;
    azmod_state_init(&s);
    g_glide_ms = glide_clock;
    az_set(AZF_STEMS, 1);
    azmod_set_docked(true);
    s.menu_focused = 1;
    s.mode = AZ_OPEN;
    g_last_left = -1;
    azmod_menu_reset(&s);
    c_select(&s, AZF_STEMS);
    draw(&s);

    const int L = AZF_STEMS, gh = c_grow_h(&g_fb), gap = g_fb.h * 6 / 800;
    int r[AZI_COUNT], m = c_detail(L, r), ke = row_of(L, AZF_STEM_FX_ECHO), kb = row_of(L, AZF_STEM_FX_BRAKE);
    CHECK("the Stems pane holds Echo Length and Brake Length", ke > 0 && kb > 0 && m > kb);

    /* ---- which rows are tall ---- */
    {
        int x, y, w, h, ok = 1;
        for (int k = 1; k < m; k++) {
            c_detail_rect0(&g_fb, L, k, &x, &y, &w, &h);
            if (c_row_tall(&g_fb, L, r[k]) != (k == ke) || h != (k == ke ? 2 * gh : gh) + c_row_extra(&g_fb, L, r[k])) ok = 0;
        }
        CHECK("Echo Length (eight values) is twice the row height, every other row the normal height (plus its description's further lines)", ok);
    }
    /* ---- stacking ---- */
    {
        int x, y, w, h, above = 1, below = 1, chain = 1, py = 0, ph = 0;
        for (int k = 1; k < m; k++) {
            c_detail_rect0(&g_fb, L, k, &x, &y, &w, &h);
            if (k <= ke && y != old_row_y(L, k) + ext_above(L, k)) above = 0;
            if (k > ke && y != old_row_y(L, k) + gh + ext_above(L, k)) below = 0;
            if (k > 1 && y != py + ph + gap) chain = 0;
            py = y;
            ph = h;
        }
        CHECK("rows down to the tall row are where they were; each below moves down by one row height (both by the description lines above)", above && below);
        CHECK("every row starts the normal gap below the one above it", chain);
    }
    /* ---- the tall row's buttons ---- */
    {
        int x, y, w, h, ok = 1, fit = 1, order = 1, px = 1, lx = 0;
        c_detail_rect(&g_fb, L, ke, &x, &y, &w, &h);
        int n = az_n_choices(AZF_STEM_FX_ECHO), prev = -1;
        lx = x + c_label_dx(&g_fb, L, AZF_STEM_FX_ECHO);
        for (int b = 0; b < n; b++) {
            int qx, qy, qw, qh;
            c_bank_rect(&g_fb, L, ke, AZF_STEM_FX_ECHO, b, &qx, &qy, &qw, &qh);
            if (qy < y + gh || qy + qh > y + h || qx < x || qx + qw > x + w - g_fb.w / 40) ok = 0;
            if (b == 0 && qx != lx) ok = 0;
            if (qx <= prev) order = 0;
            prev = qx + qw;
            const char *v = az_choice_label(AZF_STEM_FX_ECHO, b);
            if (qw - qw / 4 < azmod_ttf_menu_width(v, (int) strlen(v), qh * 40 / 100, 2)) fit = 0;
            if (c_btn_px(qw, qh, v) != qh * 40 / 100) px = 0;
        }
        CHECK("Echo Length's buttons: on the row's second line, from the label's left edge, inside the row", ok);
        CHECK("Echo Length's buttons: left to right, apart", order);
        CHECK("Echo Length's buttons: each value fits at the button text size (no shrinking)", fit && px);
        int lw = c_label_dx(&g_fb, L, AZF_STEM_FX_ECHO) + menu_text_w(az_pane_label(AZF_STEM_FX_ECHO), c_rrowh(&g_fb) * 44 / 100);
        struct c_btns q;
        c_btns_at(&g_fb, L, AZF_STEM_FX_ECHO, w, false, &q);
        CHECK("Echo Length is tall because its buttons would overlap its label at the normal height", lw + g_fb.w / 80 > q.x0);
    }
    /* ---- the rows that fit: the old geometry exactly ---- */
    {
        int ok = 1, nb = az_n_choices(AZF_STEM_FX_BRAKE);
        for (int b = 0; b < nb; b++) {
            int qx, qy, qw, qh, ox, oy, ow;
            c_bank_rect(&g_fb, L, kb, AZF_STEM_FX_BRAKE, b, &qx, &qy, &qw, &qh);
            old_bank_rect(L, kb, nb, b, &ox, &oy, &ow);
            /* below the tall row: one row lower, centered on its own block (its description may wrap
             * beside the buttons, menuwrap_test.c) */
            int side = c_rrowh(&g_fb) * 3 / 4, ry = old_row_y(L, kb) - g_rscroll + gh + ext_above(L, kb);
            if (qx != ox || qy != ry + (gh + c_row_extra(&g_fb, L, AZF_STEM_FX_BRAKE) - side) / 2 || qw != ow) ok = 0;
            (void) oy;
        }
        CHECK("Brake Length (four values): the old buttons, one row lower", ok);
        int kc = -1;
        for (int k = 1; k < m; k++)
            if (az_n_choices(r[k]) == 2 && k < ke) kc = k;
        ok = kc > 0;
        for (int b = 0; kc > 0 && b < 2; b++) {
            int qx, qy, qw, qh, ox, oy, ow;
            c_choice_rect(&g_fb, L, kc, r[kc], b, &qx, &qy, &qw, &qh);
            old_choice_rect(L, kc, b, &ox, &oy, &ow);
            if (qx != ox || qy != oy || qw != ow) ok = 0;
        }
        CHECK("a two-way choice above it: the old buttons, unmoved", ok);
        c_select(&s, AZF_DRUM_ROLL);
        int kd = row_of(AZF_DRUM_ROLL, AZF_DRUM_BANK), nd = az_n_choices(AZF_DRUM_BANK), x, y, w, h;
        c_detail_rect0(&g_fb, AZF_DRUM_ROLL, kd, &x, &y, &w, &h);
        ok = kd > 0 && h == gh && y == old_row_y(AZF_DRUM_ROLL, kd) && !ext_above(AZF_DRUM_ROLL, kd);
        for (int b = 0; ok && b < nd; b++) {
            int qx, qy, qw, qh, ox, oy, ow;
            c_bank_rect(&g_fb, AZF_DRUM_ROLL, kd, AZF_DRUM_BANK, b, &qx, &qy, &qw, &qh);
            old_bank_rect(AZF_DRUM_ROLL, kd, nd, b, &ox, &oy, &ow);
            if (qx != ox || qy != oy || qw != ow) ok = 0;
        }
        CHECK("Drum Roll's Bank: the normal height and the old buttons", ok);
        c_select(&s, AZF_STEMS);
    }
    /* ---- the pane: scroll range, knob, taps ---- */
    {
        int x, y, w, h;
        c_detail_rect0(&g_fb, L, m - 1, &x, &y, &w, &h);
        int want = y + h + g_fb.h * 12 / 800 - (dock_y(&g_fb) + dock_h(&g_fb));
        CHECK("the scroll range reaches the last row, the tall row's height included", c_rmax(&g_fb, L) == (want > 0 ? want : 0) && want > 0);
        knob(&s, 0, true); /* into the pane */
        int top = dock_y(&g_fb) + menu_hdrh(&g_fb) + g_fb.h * 12 / 800, bot = dock_y(&g_fb) + dock_h(&g_fb) - g_fb.h * 12 / 800;
        int vis = 1, seen_e = 0, steps = 0;
        for (int i = 0; i < 2 * m + 4; i++) {
            knob(&s, i < m + 2 ? 1 : -1, false);
            int k = row_of(L, s.menu_sel);
            if (k < 0) continue;
            steps++;
            c_detail_rect(&g_fb, L, k, &x, &y, &w, &h);
            if (k > 0 && (y < top || y + h > bot)) vis = 0;
            if (k == ke) seen_e++;
        }
        CHECK("the knob walks the pane both ways and every row, the tall one too, is fully in view", vis && seen_e >= 2 && steps > m);
        CHECK("the offset stays in range", g_rscroll >= 0 && g_rscroll <= c_rmax(&g_fb, L));
        /* the knob on Echo Length: in view, then a press cycles its value */
        while (s.menu_sel != AZF_STEM_FX_ECHO) knob(&s, row_of(L, s.menu_sel) < ke ? 1 : -1, false);
        int v0 = az_get(AZF_STEM_FX_ECHO);
        knob(&s, 0, true);
        CHECK("a knob press on Echo Length cycles its value", az_get(AZF_STEM_FX_ECHO) == (v0 + 1) % 8);
        draw(&s);
        c_detail_rect(&g_fb, L, ke, &x, &y, &w, &h);
        uint32_t band = px_at(x + w / 2, y + 2);
        CHECK("the focus band covers the whole tall row", px_at(x + w / 2, y + h - 3) == band && px_at(x + w / 2, y + h + 2) != band);
        int fit = 1, nseen = 0;
        for (int i = 0; i < g_ntxt; i++)
            for (int b = 0; b < 8; b++)
                if (!strcmp(g_txt[i].s, az_choice_label(AZF_STEM_FX_ECHO, b)) && g_txt[i].y >= y + gh && g_txt[i].y < y + h) {
                    nseen++;
                    if (g_txt[i].px != g_txt[i].h * 40 / 100) fit = 0;
                }
        CHECK("the eight values are drawn on the second line at the button text size", nseen == 8 && fit);
        int ok = 1;
        for (int b = 7; b >= 0; b--) {
            int qx, qy, qw, qh;
            c_bank_rect(&g_fb, L, ke, AZF_STEM_FX_ECHO, b, &qx, &qy, &qw, &qh);
            s.config_dirty = false;
            tap(&s, qx + qw / 2, qy + qh / 2);
            if (az_get(AZF_STEM_FX_ECHO) != b || !s.config_dirty) ok = 0;
        }
        CHECK("a tap on each of Echo Length's eight buttons picks that value (and saves)", ok);
        int qx, qy, qw, qh, v = az_get(AZF_STEM_FX_ECHO);
        c_bank_rect(&g_fb, L, ke, AZF_STEM_FX_ECHO, 3, &qx, &qy, &qw, &qh);
        tap(&s, qx + qw / 2, y + gh / 2); /* the label's line above button 3 */
        CHECK("a tap on the label's line picks nothing", az_get(AZF_STEM_FX_ECHO) == v);
        knob(&s, 0, false);
    }
    /* ---- a registered row follows the same rule ---- */
    {
        az_feature_v2 f = {.size = sizeof f, .key = "top", .label = "Tall Test", .kind = AZ_ROW_TOGGLE, .flags = AZ_ROW_DEFAULT_ON};
        int top = azf_register("tallmod", &f);
        f = (az_feature_v2) {.size = sizeof f, .key = "long", .label = "Long values:", .kind = AZ_ROW_CHOICE, .parent = "top", .choices = LONGV, .n_choices = 6, .default_choice = 0};
        int lo = azf_register("tallmod", &f);
        f = (az_feature_v2) {.size = sizeof f, .key = "short", .label = "Short values:", .kind = AZ_ROW_CHOICE, .parent = "top", .choices = SHORTV, .n_choices = 3, .default_choice = 0};
        int sh = azf_register("tallmod", &f);
        int kl = row_of(top, lo), ks = row_of(top, sh), x, y, w, h, xs, ys, ws, hs;
        c_detail_rect0(&g_fb, top, kl, &x, &y, &w, &h);
        c_detail_rect0(&g_fb, top, ks, &xs, &ys, &ws, &hs);
        CHECK("a registered CHOICE with long values is tall, one with short values is not",
              top >= AZI_REG0 && kl > 0 && ks > kl && c_row_tall(&g_fb, top, lo) && !c_row_tall(&g_fb, top, sh) &&
                  h == 2 * gh && hs == gh && ys == y + h + gap);
    }
    /* ---- the compact list ---- */
    {
        azmod_set_docked(false);
        s.menu_focused = 0;
        s.mode = AZ_OPEN;
        draw(&s);
        int rowh = menu_rowh(&g_fb), x, y, w, h, nx, ny, nw, nh;
        azmod_item_rect(&g_fb, AZF_STEM_FX_ECHO, &x, &y, &w, &h);
        azmod_item_rect(&g_fb, AZF_STEM_FX_BRAKE, &nx, &ny, &nw, &nh);
        int ok = h == 2 * rowh && ny == y + h, in = 1, prev = -1;
        for (int b = 0; b < 8; b++) {
            int qx, qy, qw, qh;
            item_bank_rect(&g_fb, AZF_STEM_FX_ECHO, b, &qx, &qy, &qw, &qh);
            if (qy < y + rowh || qy + qh > y + h || qx <= prev || qx + qw > x + w) in = 0;
            prev = qx + qw;
        }
        CHECK("compact list: Echo Length takes two rows, the next row starts under it", ok);
        CHECK("compact list: its tiles on the second row, left to right, inside the list", in);
        azmod_set_docked(true);
    }
    return ut_summary();
}
