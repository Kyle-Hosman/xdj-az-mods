/* SPDX-License-Identifier: GPL-3.0-only */
/* menu_list.c: the main list, built from the feature table: which rows show, their order, row geometry, pins, the compact list's rows.
 * Part of the Mod Menu's one translation unit: #included by menu.c, in the order listed there. */
/* Which rows the menu shows: the table decides (azfeatures.c az_hidden: hidden state rows, the
 * Overlays readouts that live on their own screen, rows azmod.c shows at start). */
static bool item_hidden(int i) {
    return az_hidden(i);
}
/* A row a community mod registered (register_row), not one of the menu's own. */
static bool item_is_registered(int i) {
    return i >= AZI_REG0 && i < AZI_ACT0;
}

static int menu_gap_count(void); /* below, next to menu_order */
#define MENU_ROWH_MIN_ROWS 14    /* minimum row budget: caps the text size */
/* Row height. The full-screen menu uses a fixed row so menu text is one size
 * everywhere; the compact menu (and the Updates screen layout, whose changelog box
 * hangs from it) divides the space by the drawn rows. */
static int menu_rowh_list(const azmod_fb *fb);
static int menu_rowh(const azmod_fb *fb) {
    if (g_full)
        return fb->h * 46 / 800;
    return menu_rowh_list(fb);
}
static int menu_rowh_list(const azmod_fb *fb) {
    /* Budget the official rows actually drawn (hidden items take none; the drum banks are one
     * row), at least MENU_ROWH_MIN_ROWS, plus a quarter row for every gap between top-level
     * groups and half a row of bottom margin, in the space under the 70px header. A mod's
     * registered rows are not budgeted: they never shrink the rows (nor move the Updates
     * screen, which hangs from this height); past the bottom the list scrolls (g_cscroll). */
    int rows = 0;
    for (int i = 0; i < AZI_COUNT; i++)
        if (!item_hidden(i) && !item_is_registered(i))
            rows++;
    if (rows < MENU_ROWH_MIN_ROWS)
        rows = MENU_ROWH_MIN_ROWS;
    int avail = (g_docked ? dock_h(fb) : fb->h) - menu_hdrh(fb);
    int r = avail * 4 / (4 * rows + menu_gap_count() + 2);
    return r ? r : 1;
}
/* Pin glyph + its tappable rect: the docked header's top-right corner (free when
 * docked; the close-X only exists undocked). Shared by the composite (draw) and
 * azmod_input (hit-test) so they never drift. */
static void pin_rect(const azmod_fb *fb, int *x, int *y, int *w, int *h) {
    int rowh = menu_rowh(fb);
    int hw = g_docked ? dock_w(fb) : menu_content_w(fb);
    *w = rowh;
    *h = menu_hdrh(fb);
    *x = menu_bx(fb) + hw - rowh;
    *y = menu_by(fb);
}
/* A map-pin: ring head + a needle. Bright = the highlighted item is pinned. */
static void draw_pin_glyph(const azmod_fb *fb, int cx, int cy, int r, uint32_t c) {
    draw_circle(fb, cx, cy - r, r, c);
    fill_rect(fb, cx - 1, cy - r, 2, 2 * r + 1, c);
}
/* The main list, built from the table: features.def's rows in their order, the registered
 * rows, then the utilities (azfeatures.c az_display_order); hidden rows are skipped by the
 * callers below. One loop builds it, so a new row needs a line in features.def and nothing
 * here. */
static int menu_order(int *out) {
    return az_display_order(out);
}
/* A top-level option (a left-list entry): no parent. Pad Colors and Overlays are rows of
 * Extras (their parent), so they are not. */
static bool item_is_top(int i) {
    return az_parent(i) < 0;
}
/* A CHOICE drawn as a row of square buttons (the drum banks: more than two options), and
 * one drawn as two wide buttons ("Stems on BEAT LOOP 2", Mark style, the knob grab). */
static bool item_is_bank(int i) {
    return az_n_choices(i) > 2;
}
/* Item i's visual row, and how many group gaps sit above it: every top-level
 * option after the first starts a new group with a small gap, so each option's
 * sub-items read as grouped with it. */
static int item_slot(int i, int *gaps) {
    int ord[AZI_COUNT], n = menu_order(ord), row = -1, g = -1;
    for (int p = 0; p < n; p++) {
        int it = ord[p];
        if (item_hidden(it)) { /* takes no row, no gap */
            if (it == i) {
                *gaps = g < 0 ? 0 : g;
                return row < 0 ? 0 : row;
            }
            continue;
        }
        row++;
        if (item_is_top(it))
            g++;
        if (it == i) {
            *gaps = g < 0 ? 0 : g;
            return row;
        }
    }
    *gaps = 0;
    return 0;
}
static int menu_gap_count(void) { /* the official groups' gaps: the row-height budget */
    int ord[AZI_COUNT], n = menu_order(ord), tops = 0;
    for (int p = 0; p < n; p++)
        if (!item_hidden(ord[p]) && item_is_top(ord[p]) && !item_is_registered(ord[p]))
            tops++;
    return tops > 0 ? tops - 1 : 0;
}

/* Compact list scroll (the list drawn when the menu is not docked), like the full menu's lists:
 * g_cscroll is the offset in px, subtracted in azmod_item_rect, so the draw and every hit-test
 * move together; 0 while the rows fit. */
static int g_cscroll;
void azmod_item_rect(const azmod_fb *fb, int i, int *x, int *y, int *w, int *h) {
    int rowh = menu_rowh(fb);
    int bx = menu_bx(fb), bw = g_docked ? dock_w(fb) : menu_content_w(fb);
    *x = bx;
    *w = bw;
    *h = rowh;
    int gaps;
    int row = item_slot(i, &gaps);
    *y = menu_by(fb) + menu_hdrh(fb) + rowh * row +
         gaps * (rowh / 4) - g_cscroll; /* below the header; group gaps; the scroll */
}
/* the compact list's viewport: under the header, to the bottom of the screen (or the pane) */
static void c_cview(const azmod_fb *fb, int *top, int *bot) {
    *top = menu_by(fb) + menu_hdrh(fb);
    *bot = g_docked ? menu_by(fb) + dock_h(fb) : fb->h;
}
/* the last drawn row's bottom, from the viewport's top */
static int c_cend(const azmod_fb *fb) {
    int top, bot, end = 0;
    c_cview(fb, &top, &bot);
    for (int i = 0; i < AZI_COUNT; i++) {
        if (item_hidden(i))
            continue;
        int x, y, w, h;
        azmod_item_rect(fb, i, &x, &y, &w, &h);
        if (y + g_cscroll + h - top > end)
            end = y + g_cscroll + h - top;
    }
    return end;
}
/* the bottom margin under the last row: half a row while that fits (the official rows' layout,
 * pinned by the menudump goldens), and once the list overflows anyway, the version footer's band too (menu_composite.c
 * draws it bottom - 8 - its text height), so the last row scrolled to the end is never under it */
static int c_cfoot(const azmod_fb *fb) {
    int top, bot, half = menu_rowh(fb) / 2;
    c_cview(fb, &top, &bot);
    if (c_cend(fb) + half <= bot - top)
        return half;
    int vpx = fb->h / 36;
    if (vpx < 8)
        vpx = 8;
    int foot = 8 + vpx * 5 / 4 + 4;
    return foot > half ? foot : half;
}
/* its content, from the viewport's top: the last drawn row plus the bottom margin (c_cfoot) */
static int c_ccontent(const azmod_fb *fb) { return c_cend(fb) + c_cfoot(fb); }
static int c_cmax(const azmod_fb *fb) {
    int top, bot, over;
    c_cview(fb, &top, &bot);
    over = c_ccontent(fb) - (bot - top);
    return over > 0 ? over : 0;
}
static void c_cclamp(const azmod_fb *fb) {
    int mx = c_cmax(fb);
    if (g_cscroll > mx)
        g_cscroll = mx;
    if (g_cscroll < 0)
        g_cscroll = 0;
}
/* the knob moved to item i: scroll only as far as needed to show all of its row, above the bottom
 * margin (c_cfoot, the version footer's), so the last row reaches exactly the end */
static void c_cshow(const azmod_fb *fb, int i) {
    int top, bot, x, y, w, h;
    c_cview(fb, &top, &bot);
    bot -= c_cfoot(fb);
    azmod_item_rect(fb, i, &x, &y, &w, &h);
    if (y < top)
        g_cscroll -= top - y;
    if (y + h > bot)
        g_cscroll += y + h - bot;
    c_cclamp(fb);
}
/* Button b of a bank row (item_is_bank) in the compact list: square tiles after the
 * indented label and one space. */
static void item_bank_rect(const azmod_fb *fb, int i, int b, int *x, int *y, int *w, int *h) {
    azmod_item_rect(fb, i, x, y, w, h);
    int rowh = *h, sc = fb->h / 220;
    if (sc < 1)
        sc = 1; /* draw_text scale, as the composite uses */
    int x0 = menu_bx(fb) + fb->w / 12 + fb->w / 26 + ((int) strlen(az_pane_label(i)) + 1) * 6 * sc;
    int side = rowh - rowh / 4, gap = rowh / 3;
    *x = x0 + b * (side + gap);
    *w = side; /* square tiles */
    *y += rowh / 8;
    *h = side;
}

/* a row with a value of its own (TOGGLE or CHOICE) that does not open a screen */
static bool c_has_box(int it) {
    return az_has_value(it) && az_screen(it) < 0;
}

static void draw_row(azmod_state *s, const azmod_fb *fb, int i, int rowh, const char *label,
                     int scale, uint32_t normal_color) {
    int x, y, w, h;
    azmod_item_rect(fb, i, &x, &y, &w, &h);
    /* Docked mod menu (AZ_OPEN): the orange row highlight shows only while the knob
     * focus is in the pane; on hover (preview) no row is highlighted. Other screens
     * highlight normally. The tap-feedback flash is unconditional. */
    bool hi = (((s->mode != AZ_OPEN) || s->menu_focused) && s->menu_sel == i) ||
              (s->pressed_item == i && s->pressed_ticks > 0);
    if (hi)
        fill_rect(fb, x, y, w, h, C_HI);
    /* Sub-settings sit indented under their parent and gray out when it is off.
     * That is the main menu's mod tree only: sub-screens reuse draw_row with their
     * own row indices, which must not pick up the mod at the same index. */
    bool tree = (s->mode == AZ_OPEN) && c_has_box(i); /* rows with a value of their own */
    int indent = tree ? az_depth(i) * (fb->w / 26) : 0;
    uint32_t col = normal_color;
    if (tree && !az_active(i))
        col = C_STEM_OFF_FG;
    draw_text(fb, x + fb->w / 12 + indent, y + (rowh - 7 * scale) / 2, label, scale,
              hi ? C_HI_TEXT : col);
}

/* A drum-roll bank as a touch button in the shared row under DRUM ROLL: filled
 * orange = the live bank (radio group), outlined otherwise, grayed while DRUM
 * ROLL is off; flashes orange on tap like a row does. */
static void draw_bank_button(azmod_state *s, const azmod_fb *fb, int i, int scale) {
    bool active = az_active(i);
    /* The row is one knob stop: highlighted while the knob sits on it. */
    bool row_hi = s->menu_focused && s->menu_sel == i;
    int rowh = menu_rowh(fb), rx0, ry, rw0, rh0;
    azmod_item_rect(fb, i, &rx0, &ry, &rw0, &rh0);
    { /* the row, once: its highlight and its label, indented under its parent */
        int rx = menu_bx(fb), rw = g_docked ? dock_w(fb) : menu_content_w(fb);
        if (row_hi)
            fill_rect(fb, rx, ry, rw, rowh, C_HI);
        draw_text(fb, rx + fb->w / 12 + az_depth(i) * (fb->w / 26), ry + (rowh - 7 * scale) / 2,
                  az_pane_label(i), scale, row_hi ? C_HI_TEXT : (active ? C_TEXT : C_STEM_OFF_FG));
    }
    for (int b = 0; b < az_n_choices(i); b++) {
        int x, y, w, h;
        item_bank_rect(fb, i, b, &x, &y, &w, &h);
        bool lit = active && az_get(i) == b; /* a press always lands on the live bank */
        uint32_t on_bg = row_hi ? C_HI_TEXT : C_HI, on_fg = row_hi ? C_HI : C_HI_TEXT;
        uint32_t off_c = row_hi ? C_HI_TEXT : (active ? C_CHECK_BOX : C_STEM_OFF_FG);
        if (lit)
            fill_rect(fb, x, y, w, h, on_bg);
        else
            draw_rect_outline(fb, x, y, w, h, 2, off_c);
        const char *t = az_choice_label(i, b);
        uint32_t tc = lit ? on_fg : (row_hi ? C_HI_TEXT : (active ? C_TEXT : C_STEM_OFF_FG));
        if (!azmod_ttf_menu ||
            !azmod_ttf_menu(fb, x, y, w, h, t, c_btn_px(w, h, t), 0, 2, tc)) /* centered */
            draw_text(fb, x + (w - 5 * scale) / 2, y + (h - 7 * scale) / 2, t, scale, tc);
    }
}
