/* SPDX-License-Identifier: GPL-3.0-only */
/* menu_full.c: the full-screen Mod Menu: the left list, the right pane, their touch and knob handling, the preview card.
 * Part of the Mod Menu's one translation unit: #included by menu.c, in the order listed there. */
/* ---- full-screen Mod Menu ----------------------------------------------------
 * While FOCUSED the menu covers the screen below EP147's title bar, laid out like
 * EP147's own SOURCE page: the top-level items on the left (one row each, a
 * checkbox where the item is a mod), the selected item's settings on the right.
 * The right pane is the docked pane (dock_x.., dock_w), so the sub-screens
 * (Overlays, Pad Colors, System Info, Updates) draw there.
 * Knob: turn = the left list, press = into the right pane, BACK = out again; at
 * the list, BACK hands the cursor back to EP147's SOURCE list (PREVIEW).
 * PREVIEW (MOD MENU hovered) shows a summary card in the right pane. */
/* g_full / g_full_mode (declared above draw_menu_header): this frame draws the
 * full-screen layout, and in which mode; set by the composite. */

void azmod_menu_full_rect(const azmod_fb *fb, int *x, int *y, int *w, int *h) {
    *x = 0;
    *y = dock_y(fb);
    *w = fb->w;
    *h = dock_h(fb);
}
static int c_tops(int *out) { /* the left list: visible top-level items */
    int ord[AZI_COUNT], no = menu_order(ord), n = 0;
    for (int p = 0; p < no; p++) {
        int it = ord[p];
        if (item_hidden(it) || !item_is_top(it))
            continue;
        out[n++] = it;
    }
    return n;
}

static bool c_descends(int it, int top) {
    return az_descends(it, top);
}

/* The right pane's rows for a left item: the mod itself, then its sub-settings in
 * menu order (the drum banks are one row: one CHOICE). Reload has
 * one row, its button. Sub-screen items have none here (their screen draws). */
static int c_detail(int left, int *out) {
    int n = 0;
    if (az_kind(left) == AZ_ROW_ACTION) { /* Reload: one row, its button */
        out[n++] = left;
        return n;
    }
    if (!c_has_box(left) && az_kind(left) != AZ_ROW_GROUP)
        return 0;
    if (az_kind(left) != AZ_ROW_GROUP)
        out[n++] = left; /* a group has no switch row */
#ifdef AZMOD_HAVE_DAYMODE
    if (left == AZF_THEMES) {
        for (int k = 0; k < C_THEME_N; k++)
            out[n++] = C_THEME_ROW0 + k;
        return n;
    }
#endif
    int ord[AZI_COUNT], no = menu_order(ord);
    for (int p = 0; p < no; p++) {
        int it = ord[p];
        /* a screen's own rows (Browse View Mods'): hidden everywhere but there, so only their own
         * flag counts here, as on the Overlays screen (overlays_rows) */
        bool own = az_screen(left) >= 0 && az_parent(it) == left;
        if (it == left || (own ? (az_row(it)->d.flags & AZ_ROW_HIDDEN) != 0 : item_hidden(it)))
            continue;
        if (c_descends(it, left))
            out[n++] = it;
    }
    return n;
}
static int c_lw(const azmod_fb *fb) {
    return dock_x(fb);
}
/* The left list's row height: the space divided by the official entries (capped). A mod's rows
 * never shrink it: past the bottom the list scrolls. */
static int c_lrowh(const azmod_fb *fb) {
    int t[AZI_COUNT], nt = c_tops(t), n = 0;
    for (int k = 0; k < nt; k++)
        n += !item_is_registered(t[k]);
    int avail = dock_h(fb) - menu_hdrh(fb), r = n ? avail / n : avail, mx = fb->h * 62 / 800;
    return r > mx ? mx : (r ? r : 1);
}
/* The left list and the right pane move as the Community Mods home page does (scroll_glide.h):
 * under the finger while it is down, on in a fling after a lift while moving, and a knob step
 * glides to the row it shows. g_lscroll and g_rscroll stay the offsets drawn and hit-tested;
 * beside each, the value its glide last wrote (g_lglpx, g_rglpx), so an offset set anywhere else
 * (a clamp, a pending show) takes over and stops the glide. The menu's tick moves them
 * (c_glide_tick); each list's range is the one its last clamp measured. */
#include "scroll_glide.h"
static struct glide g_lgl, g_rgl;
static int g_lglpx, g_rglpx, g_lglmax, g_rglmax;
static long c_glide_mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}
static long (*g_glide_ms)(void) = c_glide_mono_ms; /* the glides' clock (a test steps its own) */
/* the glide follows the offset when anything else set it */
static void c_glide_sync(struct glide *g, int *px, int v) {
    if (v == *px)
        return;
    glide_set(g, (float) v);
    *px = v;
}
/* put the offset at v at once, its glide stopped (a new pane, a group's screen opened or left) */
static void c_glide_put(struct glide *g, int *px, int *v, int to) {
    glide_set(g, (float) to);
    *v = *px = to;
}
/* one frame of a glide within [0, max]; true while it moves */
static bool c_glide_frame(struct glide *g, int *px, int *v, int max) {
    c_glide_sync(g, px, *v);
    if (!g->on)
        return false;
    glide_step(g, g_glide_ms(), (float) max);
    *v = *px = glide_px(g);
    return true;
}
/* The finger's point during a drag (phase 1) and at the lift (2), held within [0, max]: the list
 * is at the finger, and a lift while moving flings it on. Held first, so a finger past either
 * end adds no speed. */
static void c_glide_drag(struct glide *g, int *px, int *v, int to, int max, int phase) {
    long ms = g_glide_ms();
    if (to > max)
        to = max;
    if (to < 0)
        to = 0;
    c_glide_sync(g, px, *v);
    if (phase == 2)
        glide_release(g, (float) to, ms, GLIDE_STOP_V);
    else
        glide_drag(g, (float) to, ms);
    glide_step(g, ms, (float) max);
    *v = *px = glide_px(g);
}
/* A finger landing on a moving list stops it where it shows. True when it was flinging: that
 * touch only catches the list and fires nothing. */
static bool c_glide_catch(struct glide *g, int *px, int *v) {
    c_glide_sync(g, px, *v);
    bool fling = glide_flinging(g);
    glide_stop(g);
    *v = *px = glide_px(g);
    return fling;
}
/* Left list scroll, like the right pane's (g_rscroll): g_lscroll is the list's offset in
 * px, 0 = top, subtracted in c_left_rect, so the draw and every hit-test move together. A list
 * that fits never scrolls (c_lmax = 0). g_lshow asks the next input or draw to bring the
 * selected entry into view (azmod_menu_reset has no framebuffer to measure with). */
static int g_lscroll, g_lshow;
static int c_lview(const azmod_fb *fb) { /* the rows' viewport: under the header, to the bottom */
    return dock_h(fb) - menu_hdrh(fb);
}
static int c_lmax(const azmod_fb *fb) {
    int t[AZI_COUNT], over = c_tops(t) * c_lrowh(fb) - c_lview(fb);
    return over > 0 ? over : 0;
}
static void c_left_rect(const azmod_fb *fb, int k, int *x, int *y, int *w, int *h) {
    int rh = c_lrowh(fb);
    *x = 0;
    *w = c_lw(fb);
    *h = rh;
    *y = dock_y(fb) + menu_hdrh(fb) + k * rh - g_lscroll;
}
static void c_lclamp(const azmod_fb *fb) {
    int mx = g_lglmax = c_lmax(fb);
    if (g_lscroll > mx)
        g_lscroll = mx;
    if (g_lscroll < 0)
        g_lscroll = 0;
}
/* the knob moved the selection to entry k: scroll only as far as needed to show all of it */
static void c_lshow(const azmod_fb *fb, int k) {
    int rh = c_lrowh(fb), y = k * rh; /* in list coordinates */
    if (y < g_lscroll)
        g_lscroll = y;
    if (y + rh > g_lscroll + c_lview(fb))
        g_lscroll = y + rh - c_lview(fb);
    c_lclamp(fb);
}
/* before an input or a draw: the offset in bounds (rows can change), and a pending show done */
static void c_lsync(const azmod_state *s, const azmod_fb *fb) {
    if (g_lshow) {
        int t[AZI_COUNT], n = c_tops(t);
        for (int k = 0; k < n; k++)
            if (t[k] == s->left_sel)
                c_lshow(fb, k);
        g_lshow = 0;
    }
    c_lclamp(fb);
}
static void c_back_rect(const azmod_fb *fb, int *x, int *y, int *w, int *h) {
    int hh = menu_hdrh(fb), side = hh * 5 / 8;
    *w = *h = side;
    *x = fb->w / 40;
    *y = dock_y(fb) + (hh - side) / 2;
}
static int c_rrowh(const azmod_fb *fb) {
    return fb->h * 50 / 800;
}
/* Right pane, top to bottom: the switch that turns the mod on,
 * on its own band right under the header; the description; then "Options" and
 * the sub-settings. Row 0 is the switch (or Reload's button). */
static int c_master_y(const azmod_fb *fb) {
    return dock_y(fb) + menu_hdrh(fb) + fb->h * 12 / 800;
}
static int c_desc_y(const azmod_fb *fb) {
    return c_master_y(fb) + c_rrowh(fb) + fb->h * 14 / 800;
}
static const char *c_desc(int it);
/* Descriptions: drawn at c_desc_px, one line every c_desc_lh, word-wrapped to the width their
 * place has (c_wrap). Every place grows to the lines its text needs, so none is ever cut. */
static int c_desc_px(const azmod_fb *fb) {
    return fb->h * 17 / 800;
}
static int c_desc_lh(const azmod_fb *fb) {
    return fb->h * 26 / 800;
}
/* the width of the pane's own description (and a GROUP's line above its members) */
static int c_pane_desc_w(const azmod_fb *fb) {
    return dock_w(fb) - fb->w / 20;
}
static int c_desc_lines(const azmod_fb *fb, int it) { /* the pane's lines, never fewer than 2 */
    int n = c_wrap_n(c_desc(it), c_desc_px(fb), c_pane_desc_w(fb));
    return n < 2 ? 2 : n;
}
/* the "Options" caption: below however many description lines the mod has */
static int c_opts_y(const azmod_fb *fb, int left) {
    return c_desc_y(fb) + c_desc_lines(fb, left) * c_desc_lh(fb) + fb->h * 18 / 800;
}
/* A GROUP's pane has no switch of its own: its description, then each
 * member as a row with its own description under the label. */
static int c_grow_h(const azmod_fb *fb) {
    return c_rrowh(fb) + fb->h * 30 / 800;
}
/* Right pane scroll: g_rscroll is the pane's offset in px, 0 = top; it is
 * subtracted in c_detail_rect, so the draw and every hit-test (rows, bank buttons,
 * choice buttons) move together. A pane whose rows fit never scrolls (c_rmax = 0). */
static int g_rscroll;
/* A group's own screen (Browse View Mods, from its Extras row) is drawn and driven as that group's
 * pane: its members with icons and descriptions, like Extras' rows. c_pane is the item whose rows
 * the right pane shows; the Extras pane's scroll is kept while that screen is up. */
static int g_rscroll_parent;
static bool c_group_screen(int mode) {
    return sub_mode(mode) && az_kind(sub_head(mode)) == AZ_ROW_GROUP;
}
static int c_pane(const azmod_state *s) {
    return c_group_screen(s->mode) ? sub_head(s->mode) : s->left_sel;
}
static bool c_rows_pane(const azmod_state *s) {
    return s->mode == AZ_OPEN || c_group_screen(s->mode);
}
static void c_group_screen_leave(const azmod_state *s) {
    if (c_group_screen(s->mode))
        c_glide_put(&g_rgl, &g_rglpx, &g_rscroll, g_rscroll_parent);
}
static void c_detail_rect0(const azmod_fb *fb, int left, int k, int *x, int *y, int *w, int *h);
static void c_detail_rect(const azmod_fb *fb, int left, int k, int *x, int *y, int *w, int *h) {
    c_detail_rect0(fb, left, k, x, y, w, h);
    *y -= g_rscroll;
}
static int c_detail(int left, int *out);
static int c_rmax(const azmod_fb *fb, int left) { /* how far the pane can scroll */
    int r[AZI_COUNT], m = c_detail(left, r);
    if (!m)
        return 0;
    int x, y, w, h;
    c_detail_rect0(fb, left, m - 1, &x, &y, &w, &h);
    int over = y + h + fb->h * 12 / 800 - (dock_y(fb) + dock_h(fb));
    return over > 0 ? over : 0;
}
static void c_rclamp(const azmod_fb *fb, int left) {
    int mx = g_rglmax = c_rmax(fb, left);
    if (g_rscroll > mx)
        g_rscroll = mx;
    if (g_rscroll < 0)
        g_rscroll = 0;
}
/* the knob moved the focus to row k: scroll only as far as needed to show all of it */
static void c_rshow(const azmod_fb *fb, int left, int k) {
    int x, y, w, h;
    c_detail_rect0(fb, left, k, &x, &y, &w, &h);
    int top = dock_y(fb) + menu_hdrh(fb) + fb->h * 12 / 800,
        bot = dock_y(fb) + dock_h(fb) - fb->h * 12 / 800;
    if (k == 0)
        g_rscroll = 0; /* the first row: the whole top, description too */
    else if (y - g_rscroll < top)
        g_rscroll = y - top;
    if (y + h - g_rscroll > bot)
        g_rscroll = y + h - bot;
    c_rclamp(fb, left);
}
/* ---- tall sub-option rows -------------------------------------------------------------------
 * A sub-option row with buttons (a CHOICE: two wide buttons, or a bank of more) is TALL, twice
 * c_grow_h, when its buttons would not fit beside its label: they then get a line of their own
 * under the label and its description, from the label's left edge. The rule is measured per row
 * from the label's and the values' widths (official and registered rows alike), so every row
 * that fits keeps the normal height and layout. Each button is wide enough for its row's
 * longest value at the button text size (item_btn_w). c_detail_rect0 stacks the rows by their
 * heights, so the draw, the scroll range, the knob's scrolling and every hit test follow. */
/* where row it's label starts in left's pane, from the row's left edge: the margin, the indent
 * under its parent, Extras' icon (as c_draw_right_body draws a sub-option row) */
static int c_label_dx(const azmod_fb *fb, int left, int it) {
    int dx = fb->w / 40;
    if (az_has_value(it) && it != left)
        dx += (az_depth(it) - az_depth(left) - 1) * fb->w / 32;
    if (az_has_value(it) && az_parent(it) >= 0 &&
        (az_row(az_parent(it))->d.flags & AZ_ROW_CHILD_ICONS))
        dx += c_rrowh(fb) * 55 / 100 + fb->w / 60;
    return dx;
}
/* The buttons of CHOICE row it in a row rw wide: n of them, bw x bh, gap apart, the first at x0
 * and top y, both from the row's top-left corner. Normal height: at the row's right end, two on
 * the label's line, a bank centered on the row. Tall: the line under the description. */
struct c_btns {
    int n, x0, y, bw, bh, gap;
};
static int c_row_extra(const azmod_fb *fb, int left, int it);
static void c_btns_at(const azmod_fb *fb, int left, int it, int rw, bool tall, struct c_btns *q) {
    int rh = c_rrowh(fb), gh = c_grow_h(fb);
    q->n = az_n_choices(it);
    if (q->n > 2) { /* a bank: square unless a value needs more width */
        q->bh = rh * 3 / 4;
        q->gap = q->bh / 3;
        q->bw = item_btn_w(it, q->bh, q->bh);
        q->y = (gh - q->bh) / 2;
    } else {
        q->bh = rh * 3 / 5;
        q->gap = fb->w * 10 / 1280;
        q->bw = item_btn_w(it, fb->w * 132 / 1280, q->bh);
        q->y = fb->h * 8 / 800;
    }
    int span = q->n * q->bw + (q->n - 1) * q->gap, right = rw - fb->w / 40;
    if (!tall) {
        q->x0 = right - span;
        return;
    }
    q->x0 = c_label_dx(fb, left, it);
    q->y = gh + (gh - q->bh) / 2 + c_row_extra(fb, left, it); /* under the whole description */
    if (q->n > 0 && span > right - q->x0) { /* more values than one line holds: narrower */
        int fit = (right - q->x0 - (q->n - 1) * q->gap) / q->n;
        q->bw = fit > 1 ? fit : 1;
    }
}
/* whether sub-option row it (not the pane's own switch row) is tall in left's pane */
static bool c_row_tall(const azmod_fb *fb, int left, int it) {
    if (it == left || az_kind(left) == AZ_ROW_GROUP || az_n_choices(it) < 2)
        return false;
    struct c_btns q;
    c_btns_at(fb, left, it, dock_w(fb), false, &q);
    int end = c_label_dx(fb, left, it) + menu_text_w(az_pane_label(it), c_rrowh(fb) * 44 / 100);
    return end + fb->w / 80 > q.x0;
}
/* the width row it's description has in left's pane: from the label's left edge to the row's
 * right margin, less what sits beside it (a box or a chevron; the buttons of a bank at the normal
 * height). A two-way choice's buttons are on the label's line and a tall row's on a line of their
 * own, so theirs runs to the margin. */
static int c_rowdesc_w(const azmod_fb *fb, int left, int it) {
    int rw = dock_w(fb), right = rw - fb->w / 40, box = fb->h * 20 / 800 + fb->h * 12 / 800;
#ifdef AZMOD_HAVE_DAYMODE
    if (c_is_theme(it))
        return right - fb->w / 40 - box;
#endif
    int lx = c_label_dx(fb, left, it);
    if (az_kind(left) == AZ_ROW_GROUP)
        return right - box - lx; /* a member: a box or a chevron at its right end */
    if (c_row_tall(fb, left, it) || az_n_choices(it) == 2)
        return right - lx;
    if (az_n_choices(it) > 2) { /* a bank beside it, the gap a box keeps */
        struct c_btns q;
        c_btns_at(fb, left, it, rw, false, &q);
        return q.x0 - fb->h * 12 / 800 - lx;
    }
    return right - box - lx;
}
static const char *c_rowdesc(int it);
/* how much taller than its layout's height row it is for its description: a row holds one line
 * of it (a tall row two, above its buttons); every further line adds a line's height */
static int c_row_extra(const azmod_fb *fb, int left, int it) {
    int n = c_wrap_n(c_rowdesc(it), c_desc_px(fb), c_rowdesc_w(fb, left, it)),
        cap = c_row_tall(fb, left, it) ? 2 : 1;
    return n > cap ? (n - cap) * c_desc_lh(fb) : 0;
}
/* sub-option row it's height in left's pane: the normal height or a tall row's, plus its
 * description's further lines */
static int c_row_h(const azmod_fb *fb, int left, int it) {
    int gh = c_grow_h(fb);
    return (c_row_tall(fb, left, it) ? 2 * gh : gh) + c_row_extra(fb, left, it);
}
/* a GROUP pane's first member row: under the pane's description, however many lines it has */
static int c_group_y0(const azmod_fb *fb, int left) {
    int n = c_wrap_n(c_desc(left), c_desc_px(fb), c_pane_desc_w(fb));
    return c_master_y(fb) + fb->h * 44 / 800 + (n > 1 ? (n - 1) * c_desc_lh(fb) : 0);
}
static void c_detail_rect0(const azmod_fb *fb, int left, int k, int *x, int *y, int *w, int *h) {
    *x = dock_x(fb);
    *w = dock_w(fb);
    *h = c_rrowh(fb);
    int r[AZI_COUNT], m, gh = c_grow_h(fb), gap = fb->h * 6 / 800;
    if (az_kind(left) == AZ_ROW_GROUP) {
        m = c_detail(left, r);
        *h = gh + (k < m ? c_row_extra(fb, left, r[k]) : 0);
        *y = c_group_y0(fb, left) + k * (gh + gap);
        for (int j = 0; j < k && j < m; j++)
            *y += c_row_extra(fb, left, r[j]);
        return;
    }
    if (k == 0) {
        *y = c_master_y(fb);
        return;
    }
    /* label + its description; the rows above by their own heights */
    m = c_detail(left, r);
    *h = k < m ? c_row_h(fb, left, r[k]) : gh;
    *y = c_opts_y(fb, left) + fb->h * 32 / 800 + (k - 1) * (gh + gap);
    for (int j = 1; j < k && j < m; j++)
        *y += c_row_h(fb, left, r[j]) - gh;
}
/* The knob moved the selection to entry k (the left list) or the focus to row k (left's pane):
 * glide to the offset c_lshow / c_rshow would set. Quick detents add up from where the glide is
 * heading, so the offset they measure from is the glide's goal. */
static void c_lglide_show(const azmod_fb *fb, int k) {
    c_glide_sync(&g_lgl, &g_lglpx, g_lscroll);
    int shown = g_lscroll;
    if (g_lgl.on)
        g_lscroll = (int) (g_lgl.goal + 0.5f);
    c_lshow(fb, k);
    int to = g_lscroll;
    g_lscroll = shown;
    glide_to(&g_lgl, (float) to, g_glide_ms());
}
static void c_rglide_show(const azmod_fb *fb, int left, int k) {
    c_glide_sync(&g_rgl, &g_rglpx, g_rscroll);
    int shown = g_rscroll;
    if (g_rgl.on)
        g_rscroll = (int) (g_rgl.goal + 0.5f);
    c_rshow(fb, left, k);
    int to = g_rscroll;
    g_rscroll = shown;
    glide_to(&g_rgl, (float) to, g_glide_ms());
}
/* The menu's tick: a frame of each list's glide; true while one moves. The right pane's glide
 * stops when the pane no longer shows rows. */
static bool c_glide_tick(const azmod_state *s) {
    if (!c_rows_pane(s)) {
        c_glide_sync(&g_rgl, &g_rglpx, g_rscroll);
        glide_stop(&g_rgl);
    }
    bool l = c_glide_frame(&g_lgl, &g_lglpx, &g_lscroll, g_lglmax);
    bool r = c_glide_frame(&g_rgl, &g_rglpx, &g_rscroll, g_rglmax);
    return l || r;
}
/* Controls sit at the right end of a sub-option row, centered on the whole block
 * (label + description), like the checkboxes. */
/* Button b of a bank row (a CHOICE of more than two: the drum banks) in left's pane. */
static void c_bank_rect(const azmod_fb *fb, int left, int row, int it, int b, int *x, int *y, int *w,
                        int *h) {
    int rx, ry, rw, rh;
    struct c_btns q;
    c_detail_rect(fb, left, row, &rx, &ry, &rw, &rh);
    bool tall = c_row_tall(fb, left, it);
    c_btns_at(fb, left, it, rw, tall, &q);
    *x = rx + q.x0 + b * (q.bw + q.gap);
    *y = ry + (tall ? q.y : (rh - q.bh) / 2);
    *w = q.bw;
    *h = q.bh;
}
/* A two-way CHOICE instead of a checkbox, for settings where neither side is
 * "off": exactly one of the two buttons lit (the row's choices, features.def).
 * [BEAT LOOP][BEAT LOOP 2], [PRESS][SHIFT + PRESS], [Color][Mono]. */
static bool c_is_choice(int it) {
    return az_n_choices(it) == 2;
}
static const char *c_choice_label(int it, int b) {
    return az_choice_label(it, b);
}
/* on the label line, so the description below gets the full width; a tall row's on its own line */
static void c_choice_rect(const azmod_fb *fb, int left, int row, int it, int b, int *x, int *y,
                          int *w, int *h) {
    int rx, ry, rw, rh;
    struct c_btns q;
    c_detail_rect(fb, left, row, &rx, &ry, &rw, &rh);
    c_btns_at(fb, left, it, rw, c_row_tall(fb, left, it), &q);
    *x = rx + q.x0 + b * (q.bw + q.gap);
    *y = ry + q.y;
    *w = q.bw;
    *h = q.bh;
}

/* A row's description (features.def help): under a left entry's header in its pane, or on the
 * line under a member row. */
static const char *c_desc(int it) {
    const struct az_row *r = az_row(it);
    return r && r->d.help ? r->d.help : "";
}
static const char *c_rowdesc(int it) {
#ifdef AZMOD_HAVE_DAYMODE
    if (c_is_theme(it))
        return C_THEME_DESC[it - C_THEME_ROW0];
#endif
    return c_desc(it);
}
/* The last left-list category, for this EP147 run only (leave the Mod Menu on Themes,
 * come back, it opens on Themes). In memory, never persisted: a fresh start
 * opens on the first entry. -1 = nothing chosen yet. */
static int g_last_left = -1;
/* The mode the right pane shows for a left item, with no row focused. */
static void c_select(azmod_state *s, int it) {
    if (it != s->left_sel)
        c_glide_put(&g_rgl, &g_rglpx, &g_rscroll, 0);
    s->left_sel = it;
    s->pane = 0;
    s->menu_sel = -1;
    g_last_left = it;
    /* clang-format off */
    if      (sub_screen(it)) s->mode = sub_screen(it);
    else if (it == AZI_PAD_COLORS)  { s->mode = AZ_COLORS; s->color_sel = 0; }
    else if (it == AZI_COMMUNITY) { s->mode = AZ_COMMUNITY; cm_reset(); g_cm_full = 0; g_cm_dir_reset(); }   /* the summary card; the row opens the full screen */
    else if (it == AZI_SYSINFO) { s->mode = AZ_INFO;   s->last_action = AZI_SYSINFO; }
    else if (it == AZI_UPDATE)  { s->mode = AZ_UPDATE; s->last_action = AZI_UPDATE; }
    else
        s->mode = AZ_OPEN;
    /* clang-format on */
}
void azmod_menu_reset(azmod_state *s) { /* back to the last category, else the first item */
    int t[AZI_COUNT], n = c_tops(t), pick = 0;
    for (int k = 0; k < n; k++)
        if (t[k] == g_last_left) {
            pick = k;
            break;
        } /* still a visible row? */
    if (n) {
        c_select(s, t[pick]);
        g_lshow = 1; /* the next input or draw scrolls the list to it */
    } else {
        s->mode = AZ_OPEN;
        s->pane = 0;
        s->left_sel = -1;
        s->menu_sel = -1;
    }
}
bool azmod_menu_at_top(const azmod_state *s) {
    if (s->mode == AZ_CLOSED)
        return false;
    if (!g_docked)
        return s->mode == AZ_OPEN;
    return s->pane == 0;
}
static void c_enter(azmod_state *s) {         /* knob press on the left list */
    if (s->left_sel == AZI_COMMUNITY) {
        cm_full_open(s);
        return;
    }   /* its pane is the full screen */
    if (cm_open_row(s, s->left_sel)) return;                         /* a mod's SCREEN row: its page */
    s->pane = 1;
    if (s->mode == AZ_OPEN) {
        int r[AZI_COUNT];
        s->menu_sel = c_detail(s->left_sel, r) ? r[0] : -1;
    } else
        s->menu_sel = 0; /* a sub-screen's first row */
}
static void c_toggle(azmod_state *s, int it) { /* a mod's checkbox, left or right */
    if (!c_has_box(it) || !az_active(it))
        return; /* grayed: inert */
    az_toggle(it);
    s->config_dirty = true;
    flash_and_activate(s, it);
}
/* A GROUP's left checkbox (60 FPS): the row's on_activate (azfeatures.c azf_fps60_bypass). */
static void c_group_tap(azmod_state *s, int it) {
    const struct az_row *r = az_row(it);
    if (!r || !r->d.on_activate)
        return;
    r->d.on_activate(r->d.user);
    s->config_dirty = true;
    flash_and_activate(s, it);
}
/* A GROUP with a left checkbox is lit while any of its TOGGLE members is on. */
static bool c_group_lit(int it) {
    int ord[AZI_COUNT], n = menu_order(ord);
    for (int p = 0; p < n; p++)
        if (az_parent(ord[p]) == it && az_kind(ord[p]) == AZ_ROW_TOGGLE &&
            !(az_row(ord[p])->d.flags & AZ_ROW_HIDDEN) && az_on(ord[p]))
            return true;
    return false;
}
/* A left entry with a checkbox: a row with a value, a GROUP with on_activate (60 FPS), and
 * Community Mods (the community master switch: code, it is not a row value). */
static bool c_left_has_box(int it) {
    const struct az_row *r = az_row(it);
    return (c_has_box(it) && az_kind(it) == AZ_ROW_TOGGLE) ||
           (r && r->d.kind == AZ_ROW_GROUP && r->d.on_activate) || it == AZI_COMMUNITY;
}
static void c_bank(azmod_state *s, int it, int pick) { /* a bank radio: exactly one live */
    if (!az_active(it))
        return;
    az_set(it, pick);
    s->config_dirty = true;
    flash_and_activate(s, it);
}
static void c_activate(azmod_state *s, int it) { /* a right-pane row by knob press */
    if (cm_open_row(s, it)) /* a mod's SCREEN row: its page */
        return;
    if (az_kind(it) == AZ_ROW_ACTION) { /* Reload; a mod's action runs its callback */
        const struct az_row *r = az_row(it);
        if (it >= AZI_REG0 && it < AZI_ACT0 && r->d.on_activate && az_active(it))
            r->d.on_activate(r->d.user);
        flash_and_activate(s, it);
        return;
    }
    if (it == AZI_PAD_COLORS) { /* Extras > Pad Colors: its screen, in the right pane */
        s->mode = AZ_COLORS;
        s->color_sel = 0;
        s->pane = 1;
        s->menu_sel = 0;
        return;
    }
    if (sub_screen(it)) { /* Extras > Overlays, Clock or Browse View Mods: its screen, likewise */
        s->mode = sub_screen(it);
        s->pane = 1;
        s->menu_sel = 0;
        if (c_group_screen(s->mode)) { /* a group's screen: its first member, the pane at its top */
            int r[AZI_COUNT];
            s->menu_sel = c_detail(it, r) ? r[0] : -1;
            g_rscroll_parent = g_rscroll;
            c_glide_put(&g_rgl, &g_rglpx, &g_rscroll, 0);
        }
        return;
    }
#ifdef AZMOD_HAVE_DAYMODE
    if (c_is_theme(it)) {
        c_theme_pick(s, it - C_THEME_ROW0);
        return;
    }
#endif
    if (item_is_bank(it)) { /* the knob cycles the bank */
        c_bank(s, it, (az_get(it) + 1) % az_n_choices(it));
        return;
    }
    c_toggle(s, it);
}
/* Touch acts on controls only. A touch changes something only when the press and the
 * release both land on the same control (a checkbox, a bank / choice button,
 * Reload; padded for a finger), less than ~12 px apart and within 400 ms (a Reload button:
 * under 1 s, so only a press held a full second is taken back). Never
 * on the press: on the deck EP147's touch decoder 0x212c6f0 XSendEvent()s a ButtonPress
 * when a finger lands AND at each step while it moves, and a ButtonRelease when it lifts.
 * The overlay turns a press while the finger is down into a drag step (touch_seq.h,
 * phase 1 here), so the gesture keeps its first press point. Movement is also judged
 * from the release's own coordinates, never only from steps or motion events. In a left list that scrolls a row is a control too
 * (selecting it, c_ctl_at 0x20000), so a drag that scrolls the list never selects; being
 * navigation, it is not held to the 400 ms. A list that fits selects on the press (no press there
 * can become a scroll). */
#define C_TAP_SLOP_800 12
#define C_TZ_CTL 0x40000 /* a row of the Clock's zone list (row index below) */
#define C_TAP_MS 400
#define C_RELOAD_HOLD_MS 1000 /* a Reload button: a press held this long is taken back */
static struct {
    bool on, moved, cm; /* cm: a press on the Community Mods home page (screen_community.c) */
    int x0, y0, s0, l0, ctl; /* s0 / l0: the right pane's (or the Community page's) and the left list's offset at the press */
    long t0;
} g_rdrag;
static long g_mnow; /* ms, set by azmod.c before each touch event */
void azmod_menu_clock(long ms) {
    g_mnow = ms;
}
static const char *g_mhit = ""; /* the last touch decision, for azmod.c's log */
/* A press that the release decides (azmod_pane_drag): the Mod Menu's lists and the Community Mods
 * home page both start their gesture here. ctl is the control under the press, or -1. */
static void c_press(int tx, int ty, int ctl, bool cm, int s0) {
    g_rdrag.on = true;
    g_rdrag.moved = false;
    g_rdrag.cm = cm;
    g_rdrag.x0 = tx;
    g_rdrag.y0 = ty;
    g_rdrag.s0 = s0;
    g_rdrag.l0 = g_lscroll;
    g_rdrag.t0 = g_mnow;
    g_rdrag.ctl = ctl;
    g_mhit = ctl >= 0 ? "press:control" : "press:no-control";
}
const char *azmod_menu_last_hit(void) {
    return g_mhit;
}
/* the left list's checkbox, exactly where c_draw_left draws it */
static bool c_left_box(const azmod_fb *fb, int it, int k, int *bx, int *by, int *bs) {
    if (!c_left_has_box(it)) return false;
    int x, y, w, h;
    c_left_rect(fb, k, &x, &y, &w, &h);
    int gap = h / 14 < 2 ? 2 : h / 14, ih = h - gap, chh = ih * 2 / 5, chw = chh * 16 / 28;
    *bs = fb->h * 20 / 800;
    *bx = x + w - fb->w / 40 - chw - fb->w / 30 - *bs;
    *by = y + (ih - *bs) / 2;
    return true;
}
/* The control under a point: -1 none; 0x10000|k = left row k's checkbox; (k << 8) | part
 * for right-pane row k, part 0 = its checkbox (or Reload's row, a button), 1+b = bank or
 * choice button b. Every rect is the drawn one (scroll included) plus a finger pad. */
/* 1 = the control (c_ctl_at's) is the Reload row's button */
static bool c_is_reload(azmod_state *s, int ctl) {
    if (ctl < 0 || ctl >= 0x10000 || (ctl & 0xff) || !c_rows_pane(s))
        return false;
    int r[AZI_COUNT], m = c_detail(c_pane(s), r), k = ctl >> 8;
    return k < m && r[k] == AZI_RELOAD;
}
static int c_ctl_at(azmod_state *s, const azmod_fb *fb, int tx, int ty) {
    int pad = fb->h * 16 / 800;
    if (tx < c_lw(fb)) {
        int tops[AZI_COUNT], n = c_tops(tops);
        if (ty < dock_y(fb) + menu_hdrh(fb) || ty >= dock_y(fb) + dock_h(fb))
            return -1; /* the header covers scrolled rows; nothing below the list */
        for (int k = 0; k < n; k++) {
            int bx, by, bs;
            if (c_left_box(fb, tops[k], k, &bx, &by, &bs) &&
                in_rect(tx, ty, bx - pad, by - pad, bs + 2 * pad, bs + 2 * pad))
                return 0x10000 | k;
        }
        for (int k = 0; k < n && c_lmax(fb) > 0; k++) { /* a scrolling list: the row itself is a control (selecting it) */
            int x, y, w, h;
            c_left_rect(fb, k, &x, &y, &w, &h);
            if (in_rect(tx, ty, x, y, w, h))
                return 0x20000 | k;
        }
        return -1;
    }
    if (!c_rows_pane(s))
        return -1;
    int pn = c_pane(s), top = dock_y(fb) + menu_hdrh(fb);
    if (ty < top)
        return -1; /* the header covers scrolled rows */
    int r[AZI_COUNT], m = c_detail(pn, r);
    for (int k = 0; k < m; k++) {
        int x, y, w, h;
        c_detail_rect(fb, pn, k, &x, &y, &w, &h);
        if (!in_rect(tx, ty, x, y - pad, w, h + 2 * pad))
            continue;
        int it = r[k];
        if (az_kind(it) == AZ_ROW_ACTION) { /* Reload: the row is its button */
            if (in_rect(tx, ty, x, y, w, h))
                return k << 8;
            continue;
        }
        if (item_is_bank(it)) {
            for (int b = 0; b < az_n_choices(it); b++) {
                int qx, qy, qw, qh;
                c_bank_rect(fb, pn, k, it, b, &qx, &qy, &qw, &qh);
                if (in_rect(tx, ty, qx - 4, qy - 4, qw + 8, qh + 8))
                    return (k << 8) | (1 + b);
            }
            continue;
        }
        if (c_is_choice(it)) {
            for (int b = 0; b < 2; b++) {
                int qx, qy, qw, qh;
                c_choice_rect(fb, pn, k, it, b, &qx, &qy, &qw, &qh);
                if (in_rect(tx, ty, qx - 4, qy - 4, qw + 8, qh + 8))
                    return (k << 8) | (1 + b);
            }
            continue;
        }
#ifdef AZMOD_HAVE_DAYMODE
        if (c_is_theme(it)) {
            if (in_rect(tx, ty, x, y, w, h))
                return k << 8;
            continue;
        } /* the whole row */
#endif
        if (az_screen(it) >= 0) { /* Pad Colors, Overlays */
            if (in_rect(tx, ty, x, y, w, h))
                return k << 8;
            continue;
        } /* opens: the whole row */
        if (c_has_box(it)) {
            int sz = fb->h * 20 / 800, bx = x + w - fb->w / 40 - sz, by = y + (h - sz) / 2;
            if (in_rect(tx, ty, bx - pad, by - pad, sz + 2 * pad, sz + 2 * pad))
                return k << 8;
        }
    }
    return -1;
}
static void c_fire(azmod_state *s, int ctl) {
    if (ctl & 0x20000) { /* a left row: select it; Community Mods and a mod's SCREEN row open */
        int tops[AZI_COUNT], n = c_tops(tops), k = ctl & 0xffff;
        if (k >= n)
            return;
        c_select(s, tops[k]);
        if (tops[k] == AZI_COMMUNITY)
            cm_full_open(s);
        else
            cm_open_row(s, tops[k]);
        return;
    }
    if (ctl & 0x10000) { /* a left checkbox */
        int tops[AZI_COUNT], n = c_tops(tops), k = ctl & 0xffff;
        if (k >= n)
            return;
        c_select(s, tops[k]);
        if (az_kind(tops[k]) == AZ_ROW_GROUP) c_group_tap(s, tops[k]);
        else if (tops[k] == AZI_COMMUNITY) cm_master_toggle();
        else c_toggle(s, tops[k]);
        return;
    }
    int r[AZI_COUNT], m = c_detail(c_pane(s), r), k = ctl >> 8, part = ctl & 0xff;
    if (k >= m)
        return;
    int it = r[k];
    s->pane = 1;
    s->menu_sel = it;
    if (part == 0) {
        c_activate(s, it);
        return;
    }
    if (item_is_bank(it)) {
        c_bank(s, it, part - 1);
        return;
    }
    if (c_is_choice(it) && az_active(it)) {
        az_set(it, part - 1);
        s->config_dirty = true;
        flash_and_activate(s, it);
    }
}
/* 1 = handled; 0 = the right pane shows a sub-screen and its own handler (below)
 * takes this input. */
static int c_input(azmod_state *s, const azmod_fb *fb, int tx, int ty, bool touched, int d, bool click) {
    if (s->mode == AZ_COMMUNITY && (g_cm_full || cm_animating())) return 0;   /* the full screen: cm_input */
    int tops[AZI_COUNT], n = c_tops(tops);
    c_lsync(s, fb);
    if (touched) {
        int bx, by, bw, bh;
        c_back_rect(fb, &bx, &by, &bw, &bh);
        if (in_rect(tx, ty, bx - 8, by - 8, bw + 16, bh + 16)) {
            if (s->mode == AZ_CLOCK_TZ) { /* the zone list -> the Clock */
                clock_tz_close(s);
                return 1;
            }
            if (s->mode == AZ_CLOCK_SET) { /* the time picker -> the Clock */
                clock_set_close(s);
                return 1;
            }
            if (c_back_to_extras(s))
                return 1;
            if (s->pane)
                c_select(s, s->left_sel);
            else
                s->want_unfocus = true;
            return 1;
        }
        if (tx < c_lw(fb) && c_lmax(fb) == 0) {   /* a list that fits: a press can only be a tap */
            for (int k = 0; k < n; k++) {
                int x, y, w, h;
                c_left_rect(fb, k, &x, &y, &w, &h);
                if (!in_rect(tx, ty, x, y, w, h)) continue;
                c_select(s, tops[k]);            /* navigation only: its checkbox is a control (release) */
                if (tops[k] == AZI_COMMUNITY) {   /* the row opens the full screen; its box stays a control */
                    int bx, by, bs, p = fb->h * 16 / 800;
                    if (!(c_left_box(fb, tops[k], k, &bx, &by, &bs) && in_rect(tx, ty, bx - p, by - p, bs + 2 * p, bs + 2 * p))) {
                        cm_full_open(s);
                        return 1;
                    }
                }
                if (cm_open_row(s, tops[k])) return 1;   /* a mod's SCREEN row opens its page, like Community Mods */
                break;
            }
        } else if (tx < c_lw(fb)) {
            /* A list that scrolls (a mod's rows made it overflow) acts on the release, like every
             * control (azmod_pane_drag): a tap on a row selects it (Community Mods and a mod's
             * SCREEN row then open), a tap on its checkbox toggles, and a drag past the slop
             * scrolls the list and does neither. */
        } else if (s->mode == AZ_CLOCK_TZ) {
            /* the zone list scrolls, so a touch on it is decided on the release, like a list
             * that scrolls: a tap on a zone picks it, a drag scrolls (azmod_pane_drag); its
             * header's BACK square acts on the press (azmod_input) */
            int bx, by, bw, bh;
            back_rect(fb, &bx, &by, &bw, &bh);
            if (in_rect(tx, ty, bx - 8, by - 8, bw + 16, bh + 16)) {
                s->pane = 1;
                return 0;
            }
            int r = tzl_row_at(fb, tx, ty);
            c_press(tx, ty, r >= 0 ? C_TZ_CTL | r : -1, false, g_tzl_first);
            return 1;
        } else if (s->mode == AZ_COMMUNITY && !g_cm_full) {
            /* the preview card (Community Mods selected but not open): its own Reload square,
             * top right, is a control even though the card itself is not entered yet; any
             * other tap in the pane opens the full screen as before. */
            int rx, ry, rsz, p = fb->h * 10 / 800;
            cm_card_reload_rect(fb, &rx, &ry, &rsz);
            if (cm_card_rshow() && in_rect(tx, ty, rx - p, ry - p, rsz + 2 * p, rsz + 2 * p)) {
                if (cm_card_tap_reload())
                    return 1;
            }
            s->pane = 1;
            return 0;
        } else if (!c_rows_pane(s)) {
            s->pane = 1;
            return 0;
        }
        int ctl = c_ctl_at(s, fb, tx, ty);
        bool caught = tx < c_lw(fb) ? c_glide_catch(&g_lgl, &g_lglpx, &g_lscroll)
                                    : c_glide_catch(&g_rgl, &g_rglpx, &g_rscroll);
        c_press(tx, ty, caught ? -1 : ctl, false, g_rscroll); /* a flinging list is only caught */
        return 1;
    }
    if (s->pane == 0) {
        if (d && n) {
            int k = 0;
            while (k < n && tops[k] != s->left_sel)
                k++;
            if (k >= n)
                k = 0;
            else
                k = (k + (d > 0 ? 1 : -1) + n) % n;
            c_select(s, tops[k]);
            c_lglide_show(fb, k); /* the list scrolls to keep the selection in view */
        }
        if (click)
            c_enter(s);
        return 1;
    }
    if (!c_rows_pane(s))
        return 0;
    int pn = c_pane(s), r[AZI_COUNT], m = c_detail(pn, r);
    if (d && m) {
        int k = 0;
        while (k < m && r[k] != s->menu_sel)
            k++;
        if (k >= m)
            k = 0;
        else
            k = (k + (d > 0 ? 1 : -1) + m) % m;
        s->menu_sel = r[k];
        c_rglide_show(fb, pn, k);
    }
    if (click && s->menu_sel >= 0)
        c_activate(s, s->menu_sel);
    return 1;
}
/* The menu's touch gesture after the press (motion = phase 1, release = 2). Returns 0
 * nothing to do, 1 redraw, 2 redraw and a control fired (the caller runs handle_action, as
 * after a press). Past the slop it is a drag: a scrolling right pane, the left list or the
 * Community Mods home page follows the finger (on the deck the release may be the only point
 * after the press; see above). */
int azmod_pane_drag(azmod_state *s, const azmod_fb *fb, int x, int y, int phase) {
    if (phase == 2 && g_cm_press >= 0) { /* Community: the pressed control unlights on lift */
        g_cm_press = -1;
        if (!g_rdrag.on)
            return 1;
    }
    if (!g_rdrag.on) {
        if (phase == 2)
            g_mhit = "release:no-press";
        return 0;
    }
    if (!g_docked || !s->menu_focused) {
        g_rdrag.on = false;
        g_mhit = "drop:menu-closed";
        return 0;
    }
    bool cm = g_rdrag.cm;
    if (cm && !cm_home_live(s)) { /* the page changed or closed under the finger */
        g_rdrag.on = false;
        g_mhit = "drop:community-left";
        return 0;
    }
    int dx = x - g_rdrag.x0, dy = y - g_rdrag.y0, slop = fb->h * C_TAP_SLOP_800 / 800;
    if (dx > slop || dx < -slop || dy > slop || dy < -slop)
        g_rdrag.moved = true;
    int was = g_rscroll, lwas = g_lscroll;
    bool cmoved = false;
    bool scrolls = !cm && c_rows_pane(s) && g_rdrag.x0 >= c_lw(fb) && c_rmax(fb, c_pane(s)) > 0;
    if (g_rdrag.moved && scrolls) {
        int mx = g_rglmax = c_rmax(fb, c_pane(s)), to = g_rdrag.s0 - dy;
        c_glide_drag(&g_rgl, &g_rglpx, &g_rscroll, to, mx, phase);
    }
    if (g_rdrag.moved && !cm && g_rdrag.x0 < c_lw(fb)) { /* the left list follows the finger */
        int mx = g_lglmax = c_lmax(fb), to = g_rdrag.l0 - dy;
        c_glide_drag(&g_lgl, &g_lglpx, &g_lscroll, to, mx, phase);
    }
    if (g_rdrag.moved && cm) /* the Community Mods home page follows the finger */
        cmoved = cm_home_scroll_to(fb, g_rdrag.s0 - dy, phase);
    bool tz = !cm && s->mode == AZ_CLOCK_TZ, tzmoved = false;
    if (g_rdrag.moved && tz) { /* the zone list follows the finger, a whole row at a time */
        int f0 = g_tzl_first, rh = tzl_rowh(fb);
        tzl_set_first(fb, g_rdrag.s0 - (rh > 0 ? dy / rh : 0));
        tzmoved = g_tzl_first != f0;
    }
    if (phase == 1) {
        g_mhit = g_rdrag.moved ? "motion:drag" : "motion:still";
        return g_rscroll != was || g_lscroll != lwas || cmoved || tzmoved;
    }
    g_rdrag.on = false;
    if (g_rdrag.moved) {
        g_mhit = "release:drag";
        return 1;
    }
    /* navigation (a left row; on the Community page a tab, a tile, a mod row, the back square)
     * is not held to the 400 ms */
    bool nav = g_rdrag.ctl >= 0 && (cm ? cm_home_nav(g_rdrag.ctl) : (g_rdrag.ctl & (0x20000 | C_TZ_CTL)) != 0);
    bool reload = g_rdrag.ctl >= 0 && (cm ? cm_home_reload(g_rdrag.ctl) : !tz && c_is_reload(s, g_rdrag.ctl));
    long held = g_mnow - g_rdrag.t0;
    if (!nav && (reload ? held >= C_RELOAD_HOLD_MS : held > C_TAP_MS)) {
        g_mhit = "release:too-slow";
        return g_rscroll != was;
    }
    if (g_rdrag.ctl < 0) {
        g_mhit = "release:no-control";
        return 0;
    }
    int tr = tz ? tzl_row_at(fb, x, y) : -1;
    int at = cm ? cm_home_ctl_at(s, fb, x, y) : tz ? (tr >= 0 ? C_TZ_CTL | tr : -1)
                                                   : c_ctl_at(s, fb, x, y);
    if (at != g_rdrag.ctl) {
        g_mhit = "release:off-control";
        return 0;
    }
    if (tz)
        clock_tz_pick(s, g_rdrag.ctl & 0xffff);
    else if (cm)
        cm_home_fire(s, fb, g_rdrag.ctl);
    else
        c_fire(s, g_rdrag.ctl);
    g_mhit = "release:FIRED";
    return 2;
}

static void c_draw_left(azmod_state *s, const azmod_fb *fb) {
    int lw = c_lw(fb), top = dock_y(fb), hh = menu_hdrh(fb);
    c_lsync(s, fb); /* rows can change (a flag, a reload); a pending show */
    fill_rect(fb, 0, top, lw, dock_h(fb), 0x00000000u);
    int tops[AZI_COUNT], n = c_tops(tops);
    int icw = fb->w * 88 / 1280;
    /* rows draw clipped to the list: nothing past its bottom edge (geometry still from fb) */
    azmod_fb cf = *fb;
    cf.h = top + dock_h(fb) < fb->h ? top + dock_h(fb) : fb->h;
    for (int k = 0; k < n; k++) {
        int it = tops[k], x, y, w, h;
        c_left_rect(fb, k, &x, &y, &w, &h);
        if (y + h <= top + hh || y >= top + dock_h(fb))
            continue; /* scrolled out of view */
        bool sel = it == s->left_sel;
        int gap = h / 14 < 2 ? 2 : h / 14, ih = h - gap;
        fill_rect(&cf, x, y, icw, ih, sel ? 0x00143C66u : 0x00181818u);
        fill_rect(&cf, x + icw + 2, y, w - icw - 2, ih,
                  sel ? (s->pane == 0 ? 0x00007DE1u : 0x00505050u) : 0x00202020u);
        int isz = ih * 3 / 5;
        draw_item_icon(&cf, x + (icw - isz) / 2, y + (ih - isz) / 2, isz, it, C_TEXT);
        if (it == AZI_UPDATE && s->update_avail) {
            int rr = isz / 5;
            if (rr < 4)
                rr = 4;
            int cx = x + (icw - isz) / 2 + rr, cy = y + (ih - isz) / 2 + rr;
            fill_disc(&cf, cx, cy, rr + 1, 0x00000000u);
            fill_disc(&cf, cx, cy, rr, 0x00FF3B30u);
        }
        int px = ih * 46 / 100;
        if (px > fb->h * 26 / 800)
            px = fb->h * 26 / 800;
        c_text(&cf, x + icw + fb->w / 45, y, 0, ih, az_label(it), px, C_TEXT);
        int chh = ih * 2 / 5, chw = chh * 16 / 28;
        draw_native_chevron(&cf, x + w - fb->w / 40 - chw, y + (ih - chh) / 2, chh, C_TEXT);
        if (c_left_has_box(it)) {
            int sz = fb->h * 20 / 800, cbx = x + w - fb->w / 40 - chw - fb->w / 30 - sz, cby = y + (ih - sz) / 2;   /* = every checkbox */
            /* 60 FPS (a group, no switch of its own): lit while either member is on.
             * Community Mods: the community master switch, shown at its pending value
             * (azcm_set_all_off applies at the next start; the full screen says so) */
            bool on = az_kind(it) == AZ_ROW_GROUP ? c_group_lit(it)
                      : it == AZI_COMMUNITY       ? !cm_off_now()
                                                  : az_on(it) && az_active(it);
            if (sel && s->pane == 0) c_box(&cf, cbx, cby, sz, on, C_TEXT, C_TEXT);
            else c_box(&cf, cbx, cby, sz, on, on ? C_CHECK_ON : C_CHECK_BOX, C_CHECK_ON);
        }
    }
    /* the header goes on last: rows scrolled up slide under it */
    fill_rect(fb, 0, top, lw, hh, C_HEADER_BG);
    int bx, by, bw, bh;
    c_back_rect(fb, &bx, &by, &bw, &bh);
    draw_rect_outline(fb, bx, by, bw, bh, 2, C_CHECK_BOX);
    int asz = bw * 3 / 5;
    blit_icon(fb, bx + (bw - asz) / 2, by + (bh - asz) / 2, asz, C_TEXT, IC_BACK);
    int tpx = hh * 32 / 100;
    c_text(fb, bx + bw + fb->w / 50, top, 0, hh, "MOD MENU", tpx, C_TEXT);
    {
        char ver[48];
        const char *lv = azmod_live_version();
        if (lv[0])
            snprintf(ver, sizeof ver, "AZ-MODS v%s", lv);
        else
            snprintf(ver, sizeof ver, "AZ-MODS");
        int vpx = hh * 24 / 100,
            vw = azmod_ttf_menu_width ? azmod_ttf_menu_width(ver, (int) strlen(ver), vpx, 2) : 0;
        if (vw > 0)
            c_text(fb, lw - vw - fb->w / 40, top, 0, hh, ver, vpx, C_TEXT_DIM);
    }
    fill_rect(fb, 0, top + hh - 2, lw, 2, C_DIVIDER);
    /* EP147's own list scroll bar, only while the list overflows (ui_draw.c draw_vsb) */
    draw_vsb(fb, lw, top + hh, c_lview(fb), n * c_lrowh(fb), c_lview(fb), g_lscroll);
}

/* Knob focus in the full-screen menu: a lighter band plus a thin orange outline;
 * text and controls keep their normal colors (a whole orange row is hard to read). */
#define C_FOCUS_BAND 0x00484848u
static void c_focus_ring(const azmod_fb *fb, int x, int y, int w, int h) {
    draw_rect_outline(fb, x, y, w, h, 2, C_HI);
}
static void c_draw_group(azmod_state *s,
                         const azmod_fb *fb) { /* a group's members, each described */
    int x0 = dock_x(fb), w0 = dock_w(fb), it0 = c_pane(s);
    int dpx = c_desc_px(fb), lh = c_desc_lh(fb);
    c_draw_wrapped(fb, x0 + fb->w / 40, c_master_y(fb) - g_rscroll, c_desc(it0), dpx, lh,
                   C_TEXT_DIM, c_pane_desc_w(fb));
    int r[AZI_COUNT], m = c_detail(it0, r), rh = c_rrowh(fb), rpx = rh * 44 / 100;
    for (int k = 0; k < m; k++) {
        int it = r[k], x, y, w, h;
        c_detail_rect(fb, it0, k, &x, &y, &w, &h);
        if (y >= fb->h || y + h <= 0)
            continue; /* wholly outside the buffer (a pane taller than the screen): nothing of it shows */
        bool fl = s->pressed_item == it && s->pressed_ticks > 0;
        bool foc = (s->pane == 1 && s->menu_sel == it) || fl, hi = false;
        fill_rect(fb, x + fb->w / 80, y, w - fb->w / 40, h, foc ? C_FOCUS_BAND : 0x00343434u);
        if (foc)
            c_focus_ring(fb, x + fb->w / 80, y, w - fb->w / 40, h);
        bool active = az_screen(it) >= 0 || az_active(it); /* a screen row is never grayed */
        uint32_t ink = hi ? C_HI_TEXT : (active ? C_TEXT : C_STEM_OFF_FG);
        /* Extras' rows carry the icons the overview shows beside them. A GROUP's pane
         * is painted here, never in c_draw_right_body, so the icons are drawn here. */
        int icx = 0;
        if (az_row(it0)->d.flags & AZ_ROW_CHILD_ICONS) { /* beside the label: the row's first line */
            int isz = rh * 55 / 100, bh = h > c_grow_h(fb) ? c_grow_h(fb) : h;
            draw_item_icon(fb, x + fb->w / 40, y + (bh - isz) / 2, isz, it, ink);
            icx = isz + fb->w / 60;
        }
        c_text(fb, x + fb->w / 40 + icx, y + fb->h * 4 / 800, 0, rh - fb->h * 8 / 800,
               az_label(it), rpx, ink);
        c_draw_wrapped(fb, x + fb->w / 40 + icx, y + rh - fb->h * 10 / 800, c_rowdesc(it), dpx, lh,
                       hi ? C_HI_TEXT : C_TEXT_DIM, c_rowdesc_w(fb, it0, it));
        int sz = fb->h * 20 / 800;
        if (az_screen(it) >= 0) { /* opens a screen: the chevron where a box sits */
            int chw = sz * 16 / 28;
            draw_native_chevron(fb, x + w - fb->w / 40 - sz + (sz - chw) / 2, y + (h - sz) / 2, sz,
                                C_TEXT);
        } else
            draw_checkbox(fb, x + w - fb->w / 40 - sz, y + (h - sz) / 2, sz,
                          az_on(it) && active, hi);
    }
    (void) w0;
}
static void c_draw_right_body(azmod_state *s, const azmod_fb *fb);
static void c_draw_right(azmod_state *s, const azmod_fb *fb) { /* a mod's settings, or Reload */
    int x0 = dock_x(fb), top = dock_y(fb), hh = menu_hdrh(fb), w0 = dock_w(fb);
    fill_rect(fb, x0, top, w0, dock_h(fb), C_MENU_SCRIM);
    int it0 = c_pane(s);
    c_rclamp(fb, it0); /* rows can change (a flag, a reload) */
    c_draw_right_body(s, fb);
    /* the header goes on last: rows scrolled up slide under it */
    fill_rect(fb, x0, top, w0, hh, C_HEADER_BG);
    c_text(fb, x0 + fb->w / 40, top, 0, hh, az_label(it0), hh * 34 / 100, C_TEXT);
    fill_rect(fb, x0, top + hh - 2, w0, 2, C_DIVIDER);
    const char *note = 0;
    if (it0 == AZF_DRUM_ROLL && s->drum_no_samples)
        note = "No drum roll samples on this USB.";
    if (it0 == AZF_PLAYLIST_EDITING && s->pledit_needs_reload)
        note = "Reload to apply.";
    if (it0 == AZI_RELOAD && s->reload_pending)
        note = "Reload to apply.";
    if (note) { /* a notice: right end of the header, in yellow, off the description's lines */
        int dpx = fb->h * 17 / 800;
        int nw = azmod_ttf_menu_width ? azmod_ttf_menu_width(note, (int) strlen(note), dpx, 2) : 0;
        c_text(fb, x0 + w0 - fb->w / 40 - nw, top, 0, hh, note, dpx, C_AVAIL);
    }
    int mx = c_rmax(fb, it0);
    if (mx > 0) { /* position bar, the changelog box's look: a track and a thumb */
        int sbw = fb->w / 320 + 3, ty0 = top + hh, th0 = dock_h(fb) - hh, view = th0;
        int th = th0 * view / (view + mx), ty = ty0 + (th0 - th) * g_rscroll / mx;
        fill_rect(fb, x0 + w0 - sbw - 2, ty0, sbw, th0, C_ROW_BG);
        fill_rect(fb, x0 + w0 - sbw - 2, ty, sbw, th, C_CHECK_BOX);
    }
}
static void c_draw_right_body(azmod_state *s, const azmod_fb *fb) {
    int x0 = dock_x(fb), w0 = dock_w(fb), it0 = c_pane(s);
    int dpx = c_desc_px(fb), lh = c_desc_lh(fb), dy = c_desc_y(fb) - g_rscroll;
    if (az_kind(it0) == AZ_ROW_GROUP) {
        c_draw_group(s, fb);
        return;
    }
    {
        int mx, my, mw, mh;
        c_detail_rect(fb, it0, 0, &mx, &my, &mw, &mh); /* the switch's band */
        fill_rect(fb, mx + fb->w / 80, my, mw - fb->w / 40, mh, 0x00343434u);
        if (it0 >= AZI_REG0 && it0 < AZI_ACT0 && az_kind(it0) == AZ_ROW_SCREEN) {
            /* a mod's SCREEN row: its label and the chevron it opens with, as in the list */
            int sz = fb->h * 20 / 800, chw = sz * 16 / 28;
            uint32_t ink = az_active(it0) ? C_TEXT : C_STEM_OFF_FG;
            c_text(fb, mx + fb->w / 40, my, 0, mh, az_pane_label(it0), c_rrowh(fb) * 44 / 100, ink);
            draw_native_chevron(fb, mx + mw - fb->w / 40 - sz + (sz - chw) / 2, my + (mh - sz) / 2,
                                sz, ink);
        }
    }
    c_draw_wrapped(fb, x0 + fb->w / 40, dy, c_desc(it0), dpx, lh, C_TEXT_DIM, c_pane_desc_w(fb));
    int r[AZI_COUNT], m = c_detail(it0, r);
    int rpx = c_rrowh(fb) * 44 / 100;
    if (m > 1) {
        int oy = c_opts_y(fb, it0) - g_rscroll;
#ifdef AZMOD_HAVE_DAYMODE
        c_text(fb, x0 + fb->w / 40, oy, 0, fb->h * 24 / 800,
               it0 == AZF_THEMES ? "THEME" : "OPTIONS", fb->h * 14 / 800, C_TEXT_DIM);
#else
        c_text(fb, x0 + fb->w / 40, oy, 0, fb->h * 24 / 800, "OPTIONS", fb->h * 14 / 800,
               C_TEXT_DIM);
#endif
        fill_rect(fb, x0 + fb->w / 40, oy + fb->h * 26 / 800, w0 - fb->w / 20, 1, C_DIVIDER);
    }
    for (int k = 0; k < m; k++) {
        int it = r[k], x, y, w, h;
        c_detail_rect(fb, it0, k, &x, &y, &w, &h);
        if (y >= fb->h || y + h <= 0)
            continue; /* wholly outside the buffer (a pane taller than the screen): nothing of it shows */
        bool fl = s->pressed_item == it && s->pressed_ticks > 0;
        bool foc = (s->pane == 1 && s->menu_sel == it) || (fl && !item_is_bank(it)),
             hi = false;
#ifdef AZMOD_HAVE_DAYMODE
        if (c_is_theme(it)) { /* a theme: name, description, radio */
            int t = it - C_THEME_ROW0, rh = c_rrowh(fb);
            fill_rect(fb, x + fb->w / 80, y, w - fb->w / 40, h, foc ? C_FOCUS_BAND : 0x00343434u);
            if (foc)
                c_focus_ring(fb, x + fb->w / 80, y, w - fb->w / 40, h);
            bool on = az_on(AZF_THEMES) && c_theme_get() == t;
            c_text(fb, x + fb->w / 40, y + fb->h * 4 / 800, 0, rh - fb->h * 8 / 800,
                   C_THEME_NAME[t], rpx, C_TEXT);
            c_draw_wrapped(fb, x + fb->w / 40, y + rh - fb->h * 10 / 800, c_rowdesc(it), dpx, lh,
                           C_TEXT_DIM, c_rowdesc_w(fb, it0, it));
            int sz = fb->h * 20 / 800;
            draw_checkbox(fb, x + w - fb->w / 40 - sz, y + (h - sz) / 2, sz, on, false);
            continue;
        }
#endif
        if (k > 0 || foc)
            fill_rect(fb, x + fb->w / 80, y, w - fb->w / 40, h,
                      foc ? C_FOCUS_BAND : 0x00343434u); /* option block */
        if (foc)
            c_focus_ring(fb, x + fb->w / 80, y, w - fb->w / 40, h);
        bool active = az_active(it);
        uint32_t ink = hi ? C_HI_TEXT : (active ? C_TEXT : C_STEM_OFF_FG);
        int indent = (az_has_value(it) && it != it0)
                         ? (az_depth(it) - az_depth(it0) - 1) * fb->w / 32
                         : 0;
        /* the left entry's own switch is "Enable" (like Pad Colors'); an action's button and
         * every option carry their pane label (features.def .pane_label, else the label) */
        const char *lab = k == 0 && az_kind(it) != AZ_ROW_ACTION ? "Enable" : az_pane_label(it);
        /* Extras' options get the same icons the overview shows beside them: centered
         * on the row, the label and its description move right by the icon's width. */
        int icx = 0;
        if (k > 0 && az_has_value(it) && az_parent(it) >= 0 &&
            (az_row(az_parent(it))->d.flags & AZ_ROW_CHILD_ICONS)) {
            int isz = c_rrowh(fb) * 55 / 100, bh = h > c_grow_h(fb) ? c_grow_h(fb) : h;
            draw_item_icon(fb, x + fb->w / 40 + indent, y + (bh - isz) / 2, isz, it,
                           hi ? C_HI_TEXT : ink); /* a tall row's: beside the label */
            icx = isz + fb->w / 60;
        }
        if (k == 0)
            c_text(fb, x + fb->w / 40 + indent, y, 0, h, lab, rpx, ink);
        else { /* label, and its description under it */
            int rh = c_rrowh(fb);
            c_text(fb, x + fb->w / 40 + indent + icx, y + fb->h * 4 / 800, 0, rh - fb->h * 8 / 800,
                   lab, rpx, ink);
            /* wrapped to the width the row gives it; the row is as tall as its lines */
            c_draw_wrapped(fb, x + fb->w / 40 + indent + icx, y + rh - fb->h * 10 / 800, c_rowdesc(it),
                           dpx, lh, hi ? C_HI_TEXT : C_TEXT_DIM, c_rowdesc_w(fb, it0, it));
        }
        if (c_is_choice(it)) { /* two buttons, one lit */
            for (int b = 0; b < 2; b++) {
                int qx, qy, qw, qh;
                c_choice_rect(fb, it0, k, it, b, &qx, &qy, &qw, &qh);
                bool lit = active && az_get(it) == b;
                uint32_t on_bg = hi ? C_HI_TEXT : C_HI, on_fg = hi ? C_HI : C_HI_TEXT;
                if (lit)
                    fill_rect(fb, qx, qy, qw, qh, on_bg);
                else
                    draw_rect_outline(fb, qx, qy, qw, qh, 2,
                                      hi ? C_HI_TEXT : (active ? C_CHECK_BOX : C_STEM_OFF_FG));
                c_btn_text(fb, qx, qy, qw, qh, c_choice_label(it, b), lit ? on_fg : ink);
            }
            continue;
        }
        if (item_is_bank(it)) {
            for (int b = 0; b < az_n_choices(it); b++) {
                int qx, qy, qw, qh;
                c_bank_rect(fb, it0, k, it, b, &qx, &qy, &qw, &qh);
                bool lit = active && az_get(it) == b; /* a press always lands on the live bank */
                uint32_t on_bg = hi ? C_HI_TEXT : C_HI, on_fg = hi ? C_HI : C_HI_TEXT;
                if (lit)
                    fill_rect(fb, qx, qy, qw, qh, on_bg);
                else
                    draw_rect_outline(fb, qx, qy, qw, qh, 2,
                                      hi ? C_HI_TEXT : (active ? C_CHECK_BOX : C_STEM_OFF_FG));
                c_btn_text(fb, qx, qy, qw, qh, az_choice_label(it, b), lit ? on_fg : ink);
            }
        } else if (c_has_box(it)) {
            int sz = fb->h * 20 / 800; /* one size for every checkbox */
            draw_checkbox(fb, x + w - fb->w / 40 - sz, y + (h - sz) / 2, sz,
                          az_on(it) && active, hi);
        }
    }
}

static void c_draw_preview(azmod_state *s, const azmod_fb *fb) { /* MOD MENU hovered */
    (void) s;
    int x0 = dock_x(fb), top = dock_y(fb), hh = menu_hdrh(fb), w0 = dock_w(fb);
    fill_rect(fb, x0, top, w0, dock_h(fb), C_MENU_SCRIM);
    fill_rect(fb, x0, top, w0, hh, C_HEADER_BG);
    c_text(fb, x0 + fb->w / 40, top, 0, hh, "MOD MENU", hh * 34 / 100, C_TEXT);
    {
        char ver[48];
        const char *lv = azmod_live_version();
        if (lv[0])
            snprintf(ver, sizeof ver, "AZ-MODS v%s", lv);
        else
            snprintf(ver, sizeof ver, "AZ-MODS");
        int vpx = hh * 24 / 100,
            vw = azmod_ttf_menu_width ? azmod_ttf_menu_width(ver, (int) strlen(ver), vpx, 2) : 0;
        if (vw > 0)
            c_text(fb, x0 + w0 - vw - fb->w / 40, top, 0, hh, ver, vpx, C_TEXT_DIM);
    }
    fill_rect(fb, x0, top + hh - 2, w0, 2, C_DIVIDER);
    int lh = fb->h * 26 / 800, px = fb->h * 17 / 800, y = top + hh + fb->h * 12 / 800;
    c_text(fb, x0 + fb->w / 40, y, 0, lh, "Tap MOD MENU again or press the knob to open.", px,
           C_TEXT_DIM);
    y += lh + fb->h * 14 / 800;
    int rh = fb->h * 34 / 800, rpx = rh * 48 / 100;
    int ord[AZI_COUNT], no = menu_order(ord), lines[AZI_COUNT], nl = 0;
    for (int p = 0; p < no; p++) { /* every switch: top-level mods and group members */
        int it = ord[p];
        if (item_hidden(it) || !c_has_box(it) || az_kind(it) != AZ_ROW_TOGGLE)
            continue;
        int par = az_parent(it);
        if (par >= 0 && az_kind(par) != AZ_ROW_GROUP)
            continue;
        lines[nl++] = it;
    }
    /* A card, not a list (the knob is still on EP147's SOURCE list): whole lines only, and when
     * a mod's rows make it longer than the pane, the last line that fits says how many more. */
    int fit = (top + dock_h(fb) - y) / rh, show = nl > fit ? (fit > 0 ? fit - 1 : 0) : nl;
    for (int q = 0; q < show; q++) {
        int it = lines[q];
        int isz = rh * 3 / 5;
        draw_item_icon(fb, x0 + fb->w / 40, y + (rh - isz) / 2, isz, it, C_TEXT);
        c_text(fb, x0 + fb->w / 40 + isz + fb->w / 60, y, 0, rh, az_label(it), rpx, C_TEXT);
        bool on = az_on(it);
        const char *st = on ? "On" : "Off";
        int sw = azmod_ttf_menu_width ? azmod_ttf_menu_width(st, (int) strlen(st), rpx, 2) : 0;
        c_text(fb, x0 + w0 - fb->w / 40 - sw, y, 0, rh, st, rpx, on ? C_HI : C_TEXT_DIM);
        y += rh;
    }
    if (show < nl && fit > 0) {
        char more[32];
        snprintf(more, sizeof more, "+%d more", nl - show);
        c_text(fb, x0 + fb->w / 40, y, 0, rh, more, rpx, C_TEXT_DIM);
    }
}
