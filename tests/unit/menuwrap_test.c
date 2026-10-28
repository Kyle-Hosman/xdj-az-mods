/* SPDX-License-Identifier: GPL-3.0-only */
/* Mod Menu descriptions wrap to the width they have and are never cut:
 *   - every pane (each left entry, a group's screen, the themes): the pane's own description and
 *     every row's description are drawn whole (no byte lost, no "..." added), each line inside
 *     its row, clear of the row's controls (box, chevron, buttons) and of the right margin;
 *   - a row grows to the lines its description needs; rows stack without overlapping, and the
 *     scroll range reaches the last row;
 *   - a tall row's buttons sit under its whole description (Stems > Echo Length: three lines);
 *   - a registered mod's long descriptions (pane, toggle row, tall choice row, one word wider
 *     than the row) follow the same rules.
 * The face's width is this file's: argv[1] bytes a pixel (8 by default), so the run can model a
 * face wider than the menu's own. menu.c is #included (MENU_C) for its statics; the community
 * core and the theme hooks are stand-ins, as in menutall_test.c. */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "check.h"
#include MENU_C

static int g_bw = 8;
int azmod_ttf_menu_width(const char *s, int n, int px, int bold) {
    (void) s;
    (void) px;
    (void) bold;
    return n * g_bw;
}
/* ---- text: what was drawn, where and at what size ---------------------------------------- */
static struct {
    int x, y, w, h, px;
    char s[256];
} g_txt[1024];
static int g_ntxt;
int azmod_ttf_menu(const azmod_fb *fb, int x, int y, int w, int h, const char *s, int px, int track, int bold, uint32_t c) {
    (void) fb;
    (void) track;
    (void) bold;
    (void) c;
    if (g_ntxt < 1024) {
        g_txt[g_ntxt].x = x;
        g_txt[g_ntxt].y = y;
        g_txt[g_ntxt].w = w;
        g_txt[g_ntxt].h = h;
        g_txt[g_ntxt].px = px;
        snprintf(g_txt[g_ntxt].s, sizeof g_txt[g_ntxt].s, "%s", s);
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
static void draw(azmod_state *s) {
    memset(g_px, 0, sizeof g_px);
    g_ntxt = 0;
    azmod_composite(s, &g_fb);
}
/* s without its whitespace: what a wrap may move but never drop */
static void squeeze(char *out, size_t n, const char *s) {
    size_t k = 0;
    for (; *s && k + 1 < n; s++)
        if (!isspace((unsigned char) *s)) out[k++] = *s;
    out[k] = 0;
}
struct box {
    int x, y, w, h;
};
static bool hits(struct box a, struct box b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
/* the description lines drawn in [y0, y1) from x0 on, top to bottom, joined; 0 = none */
static int lines_in(int x0, int y0, int y1, struct box *out, int max, char *joined, size_t jn) {
    int n = 0, dpx = c_desc_px(&g_fb);
    joined[0] = 0;
    for (int i = 0; i < g_ntxt; i++) {
        if (g_txt[i].px != dpx || g_txt[i].y < y0 || g_txt[i].y >= y1 || g_txt[i].x < x0) continue;
        if (n < max)
            out[n] = (struct box) {g_txt[i].x, g_txt[i].y,
                                   azmod_ttf_menu_width(g_txt[i].s, (int) strlen(g_txt[i].s), dpx, 2), g_txt[i].h};
        n++;
        strncat(joined, g_txt[i].s, jn - strlen(joined) - 1);
        strncat(joined, " ", jn - strlen(joined) - 1);
    }
    return n;
}
static int g_bad_lost, g_bad_place, g_bad_ctl, g_bad_stack, g_rows, g_wrapped;
static char g_first_bad[256];
static void bad(int *ctr, const char *what, int it) {
    (*ctr)++;
    if (!g_first_bad[0]) snprintf(g_first_bad, sizeof g_first_bad, "%s: %s", what, az_label(it) ? az_label(it) : "?");
}
static bool same_text(const char *drawn, const char *want) {
    char a[2048], b[2048];
    squeeze(a, sizeof a, drawn);
    squeeze(b, sizeof b, want);
    return !strcmp(a, b) && (strstr(want, "...") || !strstr(drawn, "..."));
}
static void scroll_to(int left, int y) {
    int top = dock_y(&g_fb) + menu_hdrh(&g_fb) + g_fb.h * 12 / 800, mx = c_rmax(&g_fb, left);
    g_rscroll = y - top < 0 ? 0 : (y - top > mx ? mx : y - top);
}
/* one pane, as s shows it: its description and every row's */
static void check_pane(azmod_state *s, int P) {
    const azmod_fb *fb = &g_fb;
    int r[AZI_COUNT], m = c_detail(P, r), lh = c_desc_lh(fb), right = dock_x(fb) + dock_w(fb) - fb->w / 40;
    bool group = az_kind(P) == AZ_ROW_GROUP;
    struct box ln[32];
    char joined[2048];
    /* the pane's description: between its place and the first row (or "Options") */
    g_rscroll = 0;
    draw(s);
    int py0 = group ? c_master_y(fb) : c_desc_y(fb), py1 = group ? c_group_y0(fb, P) : c_opts_y(fb, P);
    int n = lines_in(dock_x(fb), py0, py1, ln, 32, joined, sizeof joined);
    if (!same_text(joined, c_desc(P))) bad(&g_bad_lost, "pane description not drawn whole", P);
    for (int i = 0; i < n && i < 32; i++)
        if (ln[i].x + ln[i].w > right || ln[i].y + lh > py1) bad(&g_bad_place, "pane description line outside its place", P);
    /* the rows */
    int py = 0, ph = 0, gap = fb->h * 6 / 800;
    for (int k = group ? 0 : 1; k < m; k++) {
        int it = r[k], x, y, w, h;
        c_detail_rect0(fb, P, k, &x, &y, &w, &h);
        if (k > (group ? 0 : 1) && y < py + ph + gap) bad(&g_bad_stack, "row overlaps the one above", it);
        py = y;
        ph = h;
        scroll_to(P, y);
        draw(s);
        y -= g_rscroll;
        struct box row = {x + fb->w / 80, y, w - fb->w / 40, h};
        n = lines_in(x, y, y + h, ln, 32, joined, sizeof joined);
        g_rows++;
        if (n > 1) g_wrapped++;
        if (!same_text(joined, c_rowdesc(it))) bad(&g_bad_lost, "row description not drawn whole", it);
        /* the controls the lines must stay clear of */
        struct box ctl[16];
        int nc = 0, sz = fb->h * 20 / 800;
        if (!group && c_is_choice(it))
            for (int b = 0; b < 2; b++) {
                c_choice_rect(fb, P, k, it, b, &ctl[nc].x, &ctl[nc].y, &ctl[nc].w, &ctl[nc].h);
                nc++;
            }
        else if (!group && item_is_bank(it))
            for (int b = 0; b < az_n_choices(it) && nc < 16; b++) {
                c_bank_rect(fb, P, k, it, b, &ctl[nc].x, &ctl[nc].y, &ctl[nc].w, &ctl[nc].h);
                nc++;
            }
        else
            ctl[nc++] = (struct box) {x + w - fb->w / 40 - sz, y + (h - sz) / 2, sz, sz}; /* box or chevron */
        for (int i = 0; i < n && i < 32; i++) {
            struct box t = {ln[i].x, ln[i].y, ln[i].w, lh};
            if (t.x + t.w > right || t.y + t.h > row.y + row.h) bad(&g_bad_place, "line outside its row", it);
            for (int c = 0; c < nc; c++)
                if (hits(t, ctl[c])) bad(&g_bad_ctl, "line over a control", it);
        }
    }
    if (m) {
        int x, y, w, h;
        c_detail_rect0(fb, P, m - 1, &x, &y, &w, &h);
        int want = y + h + fb->h * 12 / 800 - (dock_y(fb) + dock_h(fb));
        if (c_rmax(fb, P) != (want > 0 ? want : 0)) bad(&g_bad_stack, "scroll range misses the last row", P);
    }
}
static void check_all(azmod_state *s, const char *tag) {
    g_bad_lost = g_bad_place = g_bad_ctl = g_bad_stack = g_rows = g_wrapped = 0;
    g_first_bad[0] = 0;
    int t[AZI_COUNT], nt = c_tops(t), panes = 0;
    for (int k = 0; k <= nt; k++) {
        int P = k < nt ? t[k] : AZF_BROWSE_VIEW; /* the left entries, then a group's own screen */
        c_select(s, P);
        if (!c_rows_pane(s)) continue;
        int r[AZI_COUNT];
        if (!c_detail(P, r)) continue;
        check_pane(s, P);
        panes++;
    }
    char msg[512];
    snprintf(msg, sizeof msg, "%s: %d panes, %d rows (%d wrapped): every description drawn whole%s%s", tag, panes, g_rows,
             g_wrapped, g_bad_lost ? "; first: " : "", g_bad_lost ? g_first_bad : "");
    CHECK(msg, panes >= 8 && g_bad_lost == 0);
    snprintf(msg, sizeof msg, "%s: every line inside its row and the right margin%s%s", tag, g_bad_place ? "; first: " : "",
             g_bad_place ? g_first_bad : "");
    CHECK(msg, g_bad_place == 0);
    snprintf(msg, sizeof msg, "%s: no line over a box, a chevron or a button%s%s", tag, g_bad_ctl ? "; first: " : "",
             g_bad_ctl ? g_first_bad : "");
    CHECK(msg, g_bad_ctl == 0);
    snprintf(msg, sizeof msg, "%s: rows stack without overlap, the scroll range reaches the last%s%s", tag,
             g_bad_stack ? "; first: " : "", g_bad_stack ? g_first_bad : "");
    CHECK(msg, g_bad_stack == 0);
}

static const char *const LONGV[] = {"Value one", "Value two", "Value three", "Value four", "Value five", "Value six"};

int main(int argc, char **argv) {
    if (argc > 1) g_bw = atoi(argv[1]);
    if (g_bw < 1 || g_bw > 40) g_bw = 8;
    ut_suite = "menu description wrap";
    az_defaults();
    azmod_state s;
    azmod_state_init(&s);
    az_set(AZF_STEMS, 1);
    azmod_set_docked(true);
    s.menu_focused = 1;
    s.mode = AZ_OPEN;
    g_last_left = -1;
    azmod_menu_reset(&s);
    char tag[64];
    snprintf(tag, sizeof tag, "official rows, %d px a byte", g_bw);
    check_all(&s, tag);

    /* ---- Echo Length: three lines, its buttons under all of them ---- */
    {
        const int L = AZF_STEMS;
        int r[AZI_COUNT], m = c_detail(L, r), ke = -1;
        for (int k = 0; k < m; k++)
            if (r[k] == AZF_STEM_FX_ECHO) ke = k;
        c_select(&s, L);
        int x, y, w, h, qx, qy, qw, qh;
        c_detail_rect0(&g_fb, L, ke, &x, &y, &w, &h);
        scroll_to(L, y);
        draw(&s);
        y -= g_rscroll;
        struct box ln[8];
        char joined[1024];
        int n = lines_in(x, y, y + h, ln, 8, joined, sizeof joined), lh = c_desc_lh(&g_fb);
        c_bank_rect(&g_fb, L, ke, AZF_STEM_FX_ECHO, 0, &qx, &qy, &qw, &qh);
        CHECK("Echo Length is a tall row", ke > 0 && c_row_tall(&g_fb, L, AZF_STEM_FX_ECHO));
        CHECK("Echo Length's description: at least its three lines, the last above the buttons",
              n >= 3 && n <= 8 && ln[n - 1].y + lh <= qy && qy + qh <= y + h);
        if (g_bw == 8) /* each of its lines fits the row at this face: one row line each */
            CHECK("Echo Length's three lines, as written",
                  n == 3 && !strcmp(joined, "Time between repeats, in beats. Negative: repeats what played just before the press. "
                                            "Positive: repeats what plays after the press. "));
    }

    /* ---- a registered mod's long descriptions ---- */
    {
        az_feature_v2 f = {.size = sizeof f, .key = "top", .label = "Wrap Test", .kind = AZ_ROW_TOGGLE, .flags = AZ_ROW_DEFAULT_ON, .help = "A pane description long enough to need several lines at the pane's width, so the "
                                                                                                                                            "Options caption and every row below it move down by the lines it takes."};
        int top = azf_register("wrapmod", &f);
        f = (az_feature_v2) {.size = sizeof f, .key = "tog", .label = "Long toggle", .kind = AZ_ROW_TOGGLE, .parent = "top", .help = "A toggle row whose description runs well past one line of the row, so the row "
                                                                                                                                     "grows and its box stays centered beside the whole block of text."};
        int tg = azf_register("wrapmod", &f);
        f = (az_feature_v2) {.size = sizeof f, .key = "long", .label = "Long values:", .kind = AZ_ROW_CHOICE, .parent = "top", .choices = LONGV, .n_choices = 6, .default_choice = 0, .help = "A tall choice row with a description of three lines or more.\nIts buttons must sit "
                                                                                                                                                                                              "under every line of it, never over the text, however many it takes."};
        int lo = azf_register("wrapmod", &f);
        f = (az_feature_v2) {.size = sizeof f, .key = "word", .label = "One word", .kind = AZ_ROW_TOGGLE, .parent = "top", .help = "Word: ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef"};
        int wd = azf_register("wrapmod", &f);
        CHECK("the test mod's rows are registered", top >= AZI_REG0 && tg > top && lo > tg && wd > lo);
        c_select(&s, top);
        g_bad_lost = g_bad_place = g_bad_ctl = g_bad_stack = g_rows = g_wrapped = 0;
        g_first_bad[0] = 0;
        check_pane(&s, top);
        CHECK("registered: the pane's and every row's long description drawn whole", g_bad_lost == 0);
        CHECK("registered: every line inside its row, none over a control, rows stacked",
              g_bad_place == 0 && g_bad_ctl == 0 && g_bad_stack == 0);
        CHECK("registered: the long rows wrap (more than one line each)", g_wrapped >= 3);
        int gh = c_grow_h(&g_fb);
        CHECK("registered: a wrapped toggle row is taller than one line's row, the tall row taller than two",
              c_row_h(&g_fb, top, tg) > gh && c_row_h(&g_fb, top, lo) > 2 * gh);
        CHECK("registered: the pane's own description pushes Options down past two lines",
              c_desc_lines(&g_fb, top) > 2 && c_opts_y(&g_fb, top) > c_desc_y(&g_fb) + 2 * c_desc_lh(&g_fb));
        /* the word wider than the row is broken, every byte kept, each piece within the row */
        int w = c_rowdesc_w(&g_fb, top, wd);
        CHECK("registered: one word wider than the row is broken into pieces that fit",
              c_wrap_n(c_rowdesc(wd), c_desc_px(&g_fb), w) >= 2);
    }

    /* ---- the wrap itself ---- */
    {
        int n = c_wrap("aaa bbb ccc\n\nddd", 0, 7 * g_bw, NULL, NULL);
        CHECK("c_wrap: breaks at the last space that fits, keeps an empty line, counts each", n == 4);
        CHECK("c_wrap: an empty text has no lines, no width breaks only at a newline",
              c_wrap("", 0, 100, NULL, NULL) == 0 && c_wrap("a b c d e f g h", 0, 0, NULL, NULL) == 1);
    }
    return ut_summary();
}
