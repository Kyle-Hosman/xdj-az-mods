/* SPDX-License-Identifier: GPL-3.0-only */
/* screen_update.c: Update Mods (AZ_UPDATE): the status band and its button.
 * Part of the Mod Menu's one translation unit: #included by menu.c, in the order listed there. */
/* The updates band: one text size (the pane label size, menu_text_px) for both
 * status lines, the button label and the percent. Status on the left (line 1
 * "Installed: vX", line 2 the state, a failure's reason merged in), the button on
 * the right, centered on the two lines; a third row under them for the progress bar,
 * always reserved so the changelog under it never jumps. Every left string is cut
 * to end a gap before the button (c_fit_str). One geometry function, shared by the
 * draw, the touch test and the changelog box. */
static int upd_text_px(const azmod_fb *fb) {
    return menu_text_px(fb);
}
static int upd_btn_px(const azmod_fb *fb) {
    return menu_text_px(fb);
}
typedef struct {
    int lx, top, lh, tw, bx, by, bw, bh, py, ph;
} upd_geo; /* tw: the left text's width */
static void upd_band(const azmod_fb *fb, upd_geo *g) {
    int px = upd_text_px(fb), cw = g_docked ? dock_w(fb) : menu_content_w(fb), pad = fb->w / 40;
    int scale = fb->h / 220;
    if (scale < 1)
        scale = 1;
    g->lx = menu_bx(fb) + sub_lx(fb);
    g->top = menu_by(fb) + menu_hdrh(fb) + fb->h * 14 / 800;
    g->lh = px * 17 / 10;
    g->bh = px * 2;
    if (g->bh > 2 * g->lh - 4)
        g->bh = 2 * g->lh - 4;
    int widest = 0;
    static const char *const L[] = {"Check for updates", "Update now", "Please wait"};
    for (int i = 0; i < 3; i++) {
        int w = azmod_ttf_menu_width ? azmod_ttf_menu_width(L[i], (int) strlen(L[i]), px, 2) : 0;
        if (w <= 0)
            w = (int) strlen(L[i]) * 6 * scale; /* bitmap fallback */
        if (w > widest)
            widest = w;
    }
    g->bw = widest + g->bh * 2; /* a round end plus air each side */
    if (g->bw > cw / 2)
        g->bw = cw / 2;
    g->bx = menu_bx(fb) + cw - pad - g->bw;
    g->by = g->top + (2 * g->lh - g->bh) / 2;
    g->tw = g->bx - pad - g->lx;
    if (g->tw < 0)
        g->tw = 0; /* ends a gap before the button */
    g->py = g->top + 2 * g->lh + g->lh / 6;
    g->ph = g->lh * 4 / 5; /* the progress row (bar 2x) */
}
static void update_button_rect(const azmod_fb *fb, int *bx, int *by, int *bw, int *bh) {
    upd_geo g;
    upd_band(fb, &g);
    *bx = g.bx;
    *by = g.by;
    *bw = g.bw;
    *bh = g.bh;
}
