/* SPDX-License-Identifier: GPL-3.0-only */
/* menu_composite.c: azmod_composite: draws the active screen, or the closed-state bars and stem UI.
 * Part of the Mod Menu's one translation unit: #included by menu.c, in the order listed there. */
/* Closed: the NEW PLAYLIST name field, the held banner, and the stem UI on the deck screens. */
static void compose_closed(azmod_state *s, const azmod_fb *fb) {
    /* No MOD MENU button here: the entry is the native SOURCE-list row from
     * the srcrow hooks. */
    if (s->nm_visible) { /* NEW PLAYLIST: name field + CANCEL + CREATE over the title row */
        int bar[4], ca[4], cr[4];
        azmod_naming_rects(fb, bar, ca, cr);
        fill_rect(fb, bar[0], bar[1], bar[2], bar[3],
                  0x00000000u); /* EP147's black title row */
        int fx = bar[0] + fb->w / 160, fy = bar[1] + bar[3] / 7, fh = bar[3] - 2 * (bar[3] / 7);
        int fw = ca[0] - fx - fb->w / 160;
        fill_rect(fb, fx, fy, fw, fh, 0x00555555u); /* the field, EP147's search-box look */
        fill_rect(fb, fx + 1, fy + 1, fw - 2, fh - 2, 0x00000000u);
        const char *t = azmod_naming_text();
        /* bold=2 (unhinted), as every Mod Menu label: the light hinter snaps each glyph's
         * outline to the pixel grid on its own, so within one name the letter bottoms land
         * on different rows. Unhinted glyphs share one exact baseline. The width calls use
         * the same flag, so the caret and the tail cut match what is drawn. */
        int px = fh * 55 / 100, tx = fx + fb->w / 64;
        if (t && t[0]) { /* left-aligned like a text field (w 0); a long name shows its tail */
            int room = fw - fb->w / 32, n = (int) strlen(t),
                cw = azmod_ttf_menu_width(t, n, px, 2);
            while (cw > room && *t) {
                do {
                    t++;
                    n--;
                } while (((unsigned char) *t & 0xc0) == 0x80); /* whole UTF-8 characters */
                cw = azmod_ttf_menu_width(t, n, px, 2);
            }
            azmod_ttf_menu(fb, tx, fy, 0, fh, t, px, 0, 2, 0x00ffffffu);
            fill_rect(fb, tx + cw + 3, fy + fh / 5, 2, fh * 3 / 5, 0x00ffffffu); /* caret */
        } else {
            azmod_ttf_menu(fb, tx + 6, fy, 0, fh,
                           azmod_naming_rename()   ? "New name"
                           : azmod_naming_folder() ? "New folder name"
                                                   : "New playlist name",
                           px, 0, 2, 0x00808080u);
            fill_rect(fb, tx, fy + fh / 5, 2, fh * 3 / 5, 0x00ffffffu);
        }
        fill_rect(fb, ca[0], ca[1], ca[2], ca[3],
                  0x003c3c3cu); /* CANCEL: EP147's idle row gray */
        azmod_ttf_menu(fb, ca[0], ca[1], ca[2], ca[3], "CANCEL", ca[3] * 45 / 100, 0, 2,
                       0x00ffffffu);
        fill_rect(fb, cr[0], cr[1], cr[2], cr[3],
                  0x00007de1u); /* CREATE: Pioneer's selection blue */
        azmod_ttf_menu(fb, cr[0], cr[1], cr[2], cr[3],
                       azmod_naming_rename() ? "RENAME" : "CREATE", cr[3] * 45 / 100, 0, 2,
                       0x00ffffffu);
        return;
    }
    if (s->tb_visible) { /* our held banner: UNDO (amber, left) and/or DONE (blue, right) */
        for (int b = 0; b < 2; b++) {
            if (b == 0 && !s->tb_undo)
                continue;
            if (b == 1 && s->tb_undo && !s->tb_done_btn)
                continue;
            int bx, by, bw, bh;
            if (b == 0)
                azmod_tb_undo_rect(fb, &bx, &by, &bw, &bh);
            else
                azmod_tb_done_rect(fb, &bx, &by, &bw, &bh);
            const char *lbl = b == 0 ? "UNDO" : "DONE";
            fill_rect(fb, bx, by, bw, bh,
                      b == 0 ? 0x00b25a00u
                             : 0x00007de1u); /* the drag's amber / Pioneer's blue */
            if (!azmod_ttf_center ||
                !azmod_ttf_center(fb, bx, by, bw, bh, lbl, (bh * 11) / 20, 0, 1, 0x00ffffffu)) {
                int bs = fb->h / 300;
                if (bs < 1)
                    bs = 1;
                draw_text(fb, bx + (bw - 4 * 6 * (bs + 1)) / 2, by + (bh - 7 * (bs + 1)) / 2,
                          lbl, bs + 1, 0x00ffffffu);
            }
        }
        return;
    }
    /* The stem-toggle strips show only on the WAVEFORM screen, only while the
     * STEMS mod (master) is enabled, and only for a deck whose loaded track
     * has stems (hidden on Browse/other views, or when the master is off). */
    if (!s->waveform_visible || !azmod_stems_engine(s))
        return;
    /* Native mode, 2-deck: the native tiles are the toggles, but EP147 has
     * no per-stem waveform, so the row-2 mini lanes stay ours (lanes only, no
     * pills).
     *
     * The lanes appear exactly where and when the native pad slots do, gated
     * on EP147's own signals:
     *   - waveform_visible (above) is wave_visible(): WaveformView::paint
     *     runs 32/s while the deck view is up and 0/s when it is not;
     *   - stems_available[d] is that deck's real track;
     *   - pad_beatloop[d] is that deck's real pad mode.
     * STEM_ROW2_Y_FRAC is the native grid's second row (y 530 @ 800). */
    if (azmod_stem_native && !s->four_deck) {
        /* Not gated on Stem Waves (retired): that mod recolors the big
         * band (it substitutes EP147's waveform point providers) and is a
         * separate feature from the row-2 mini lanes. */
        for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
            if (!azmod_native_lanes_drawn(s, d))
                continue; /* shared with the repaint */
            draw_stem_lanes_native(s, fb, d);
        }
        return;
    }
    /* 4-DECK native mode: azmod swaps each deck's source-cell image for its
     * stem strip (azmod.c stems4_tick), so drawing the overlay here would
     * paint over it. */
    if (azmod_stem_native)
        return;
    for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
        /* A deck whose loaded track has no stems gets no stem UI (no
         * buttons, dots, lanes or STEMS plate). azmod.c leaves its native
         * Beat Loop grid in place (the waveswitch/wavector hooks gate the
         * same way). */
        if (!s->stems_available[d])
            continue;
        if (s->four_deck) {
            /* The 4-DECK stand-in for the pill strip: a stem status readout
             * on the source cell, so it follows STEMS alone and needs
             * neither Stem Waves nor BEAT LOOP pad mode. */
            draw_stem_dots(s, fb, d);
            continue;
        }
        if (0) /* Stem Waves: retired slot 3, forced off (features.def) */
            draw_stem_lanes(s, fb, d);
        if (s->pad_beatloop[d])
            draw_stem_grid(s, fb, d);
    }
}

/* The Updates screen. */
static void compose_updates(azmod_state *s, const azmod_fb *fb, int rowh, int scale) {
    /* Layout: upd_band (screen_update.c). Left, two status lines (update_line[0] installed,
     * [1] the state); right, the button (update_line[4]) centered on them; under both, the
     * progress row, kept while idle so the changelog box below never moves. */
    draw_menu_header(fb, rowh, scale, "Updates");
    upd_geo g;
    upd_band(fb, &g);
    int px = upd_text_px(fb);
    char t0[256], t1[256], t2[256] = "";
    int w0 = c_fit_str(t0, sizeof t0, s->update_line[0], px, g.tw);
    int w1 = c_fit_str(t1, sizeof t1, s->update_line[1], px, g.tw), w2 = 0;
    bool bar = s->update_busy || s->update_pct >= 0;
    /* a failure's reason that does not fit beside the button carries on in the progress
     * row (free: a failure is a final state, the bar is gone), full width, cut if longer */
    {
        const char *l1 = s->update_line[1];
        int n = (int) strlen(l1);
        if (!bar && !strncmp(l1, "Failed", 6) && azmod_ttf_menu_width &&
            azmod_ttf_menu_width(l1, n, px, 2) > g.tw) {
            int cut = 0;
            for (int i = 1; i < n; i++)
                if (l1[i] == ' ' && azmod_ttf_menu_width(l1, i, px, 2) <= g.tw)
                    cut = i;
            if (cut) {
                snprintf(t1, sizeof t1, "%.*s", cut, l1);
                w1 = azmod_ttf_menu_width(t1, cut, px, 2);
                w2 = c_fit_str(t2, sizeof t2, l1 + cut + 1, px, g.bx + g.bw - g.lx);
            }
        }
    }
    /* line 1: Installed */
    if (!(azmod_ttf_menu && azmod_ttf_menu(fb, g.lx, g.top, 0, g.lh, t0, px, 0, 2, C_TEXT)))
        draw_text(fb, g.lx, g.top + (g.lh - 7 * scale) / 2, t0, scale, C_TEXT);
    /* line 2: the state. Available: the version in the notice yellow; Up to
     * date green; Failed red with its reason dim; the rest dim */
    {
        int ly = g.top + g.lh, split = -1;
        uint32_t c1 = C_TEXT_DIM, c2 = C_TEXT_DIM;
        const char *v = strstr(t1, " v");
        if (s->update_avail && v) {
            c1 = C_TEXT;
            c2 = C_AVAIL;
            split = (int) (v - t1) + 1;
        } else if (s->update_uptodate)
            c1 = C_READOUT_OK;
        else if (!strncmp(t1, "Failed", 6)) {
            c1 = C_BAD;
            c2 = C_TEXT_DIM;
            split = t1[6] == ':' ? 7 : -1;
        }
        if (split >= 0
                ? !(azmod_ttf_menu2 &&
                    azmod_ttf_menu2(fb, g.lx, ly, 0, g.lh, t1, px, 0, 2, c1, split, c2))
                : !(azmod_ttf_menu && azmod_ttf_menu(fb, g.lx, ly, 0, g.lh, t1, px, 0, 2, c1)))
            draw_text(fb, g.lx, ly + (g.lh - 7 * scale) / 2, t1, scale, c1);
    }
    if (t2[0] &&
        !(azmod_ttf_menu && azmod_ttf_menu(fb, g.lx, g.py, 0, g.ph, t2, px, 0, 2, C_TEXT_DIM)))
        draw_text(fb, g.lx, g.py + (g.ph - 7 * scale) / 2, t2, scale, C_TEXT_DIM);
    /* the progress row: only while an update runs (its room is always kept) */
    if (bar) {
        int pct = s->update_pct < 0 ? 0 : s->update_pct;
        char pt[16];
        snprintf(pt, sizeof pt, "%d%%", pct);
        int ptw = azmod_ttf_menu_width ? azmod_ttf_menu_width("100%", 4, px, 2) : 4 * 6 * scale;
        int bw = g.bx + g.bw - g.lx - ptw - fb->w / 60, bh = g.ph * 3 / 4,
            by = g.py + (g.ph - bh) / 2;
        draw_rect_outline(fb, g.lx, by, bw, bh, 1, C_CHECK_BOX);
        int fw = (bw - 4) * pct / 100;
        if (fw > 0)
            fill_rect(fb, g.lx + 2, by + 2, fw, bh - 4, C_HI);
        if (!(azmod_ttf_menu &&
              azmod_ttf_menu(fb, g.lx + bw + fb->w / 60, g.py, 0, g.ph, pt, px, 0, 2, C_TEXT)))
            draw_text(fb, g.lx + bw + fb->w / 60, g.py + (g.ph - 7 * scale) / 2, pt, scale,
                      C_TEXT);
    }
    {
        const char *lbl = s->update_line[4];
        bool on = !s->update_busy;
        /* the same pill in every state; only the colors change (orange when it can be
         * pressed, dark gray while busy) */
        round_rect(fb, g.bx, g.by, g.bw, g.bh, g.bh / 2, 2, on ? C_HI : C_ROW_BG,
                   on ? C_HI : C_CHECK_BOX);
        if (!(azmod_ttf_menu && azmod_ttf_menu(fb, g.bx, g.by, g.bw, g.bh, lbl, upd_btn_px(fb),
                                               0, 2, on ? C_HI_TEXT : C_TEXT_DIM)))
            draw_text_centered(fb, g.bx, g.by + (g.bh - 7 * scale) / 2, g.bw, lbl, scale,
                               on ? C_HI_TEXT : C_TEXT_DIM);
    }
    /* settings/azmod-logs.on: the rects, once per change, and whether any text reaches the
     * button (the no-overlap proof) */
    {
        static char last[2 * 256 + 8];
        char key[sizeof last];
        snprintf(key, sizeof key, "%s|%s|%.40s|%d", t0, t1, t2, g_full);
        if (strcmp(key, last) && access("/home/root/settings/azmod-logs.on", F_OK) == 0) {
            memcpy(last, key, sizeof last);
#define UPD_HIT(x, y, w, h) \
    ((w) > 0 && (x) < g.bx + g.bw && g.bx < (x) + (w) && (y) < g.by + g.bh && g.by < (y) + (h))
            int ov = UPD_HIT(g.lx, g.top, w0, g.lh) || UPD_HIT(g.lx, g.top + g.lh, w1, g.lh) ||
                     UPD_HIT(g.lx, g.py, w2, g.ph);
#undef UPD_HIT
            FILE *f = azlog_fopen_capped("/tmp/azmod-updband.log", AZLOG_CAP_BYTES);
            if (f) {
                fprintf(f,
                        "full=%d px=%d line1 [%d,%d %dx%d] \"%s\" | line2 [%d,%d %dx%d] \"%s\" "
                        "| row3 [%d,%d %dx%d] \"%s\" | button [%d,%d %dx%d] | %s\n",
                        g_full, px, g.lx, g.top, w0, g.lh, t0, g.lx, g.top + g.lh, w1, g.lh, t1,
                        g.lx, g.py, w2, g.ph, t2, g.bx, g.by, g.bw, g.bh,
                        ov ? "OVERLAP" : "no overlap");
                fclose(f);
            }
        }
    }
    draw_changelog(s, fb);
}

/* Pad Colors: the master toggle and one row per pad-mode button. */
static void compose_pad_colors(azmod_state *s, const azmod_fb *fb, int rowh, int scale) {
    static const azmod_pad_color PAL[AZMOD_PAD_PALETTE_COUNT] = AZMOD_PAD_PALETTE;
    bool on = s->pad_colors_on;
    draw_menu_header(fb, rowh, scale, "Pad Colors");
    /* row 0: the master enable, drawn exactly like a mod toggle (label + checkbox) */
    draw_colors_row(s, fb, COLORS_ROW_MASTER, rowh, "Enable", scale,
                    C_TEXT);
    {
        int rx, ry, rw, rh;
        colors_row_rect(fb, COLORS_ROW_MASTER, &rx, &ry, &rw, &rh);
        int sz = g_full ? fb->h * 20 / 800 : rh / 2,
            cbx = rx + rw - sz - (g_full ? fb->w / 40 : rh / 3), cby = ry + (rh - sz) / 2;
        bool hi = s->menu_sel == COLORS_ROW_MASTER ||
                  (s->pressed_item == COLORS_ROW_MASTER && s->pressed_ticks > 0);
        draw_checkbox(fb, cbx, cby, sz, on, hi);
    }
    /* rows 1-4: the primary modes; a separator; rows 5-8: the secondary modes:
     * label, a chip of its color, and the sub-screen chevron where a mod's
     * checkbox sits. Grayed while the master is off (the picks are kept). */
    for (int k = 0; k < 2; k++) { /* below Enable, above the secondaries */
        int sx, sy, sw, sh;
        pc_row_rect(fb, k ? COLORS_ROW_SECOND : 1, &sx, &sy, &sw, &sh);
        int ly = sy - sh / 4; /* centered in the half-row gap */
        fill_rect(fb, sx + fb->w / 40, ly, sw - fb->w / 20, 1, C_DIVIDER);
    }
    for (int row = 1; row < COLORS_ROWS; row++) {
        int i = colors_row_to_sel(row);
        draw_pc_row(s, fb, row, rowh, PICK_LABEL[i], scale,
                    on ? C_TEXT : C_STEM_OFF_FG); /* locked while Enable is off */
        int rx, ry, rw, rh;
        pc_row_rect(fb, row, &rx, &ry, &rw, &rh);
        bool hi = s->menu_sel == row;
        uint32_t cc = (hi && !g_full) ? C_HI_TEXT : (on ? C_TEXT : C_STEM_OFF_FG);
        int sz = g_full ? fb->h * 20 / 800 : rh / 2,
            cbx = rx + rw - sz - (g_full ? fb->w / 40 : rh / 3), cby = ry + (rh - sz) / 2;
        int chw = sz * 16 / 28; /* the native source-row chevron's proportions */
        draw_native_chevron(fb, cbx + (sz - chw) / 2, cby, sz, cc);
        int chip = sz, cx = cbx - chip - rh / 3, cy = cby;
        int pi = *pick_slot(s, i);
        if (pi >= 0 && pi < AZMOD_PAD_PALETTE_COUNT)
            fill_rect(fb, cx, cy, chip, chip,
                      on ? PAL[pi].swatch : quarter_rgb(PAL[pi].swatch));
        else /* native: the grid's slashed cell */
            draw_line(fb, cx + chip / 5, cy + chip / 5, cx + chip - chip / 5,
                      cy + chip - chip / 5, 1, cc);
        draw_rect_outline(fb, cx, cy, chip, chip, 1, cc);
    }
}

/* Overlays, the Clock or Browse View Mods: the switch (a group has none) and one row per setting
 * under it. */
static void compose_overlays(azmod_state *s, const azmod_fb *fb, int rowh, int scale) {
    int head = sub_head(s->mode);
    bool on = az_kind(head) != AZ_ROW_TOGGLE || az_on(head);
    draw_menu_header(fb, rowh, scale, az_label(head));
    int ovr[OVERLAYS_MAX], OVERLAYS_ROWS = overlays_rows(head, ovr);
    for (int r = 0; r < OVERLAYS_ROWS; r++) {
        /* the rows under the switch gray while it is off (their picks are kept) */
        uint32_t ink = (ovr[r] == head || on) ? C_TEXT : C_TEXT_DIM;
        bool icons = (az_row(head)->d.flags & AZ_ROW_CHILD_ICONS) && ovr[r] != head;
        draw_colors_row(s, fb, r, rowh, icons ? "" : overlays_label(ovr[r]), scale, ink);
        int rx, ry, rw, rh;
        colors_row_rect(fb, r, &rx, &ry, &rw, &rh);
        int sz = g_full ? fb->h * 20 / 800 : rh / 2,
            cbx = rx + rw - sz - (g_full ? fb->w / 40 : rh / 3), cby = ry + (rh - sz) / 2;
        bool hi = s->menu_sel == r || (s->pressed_item == r && s->pressed_ticks > 0);
        if (icons) { /* the group's rows wear the icons Extras' rows do, the label right of it */
            uint32_t tc = (hi && !g_full) ? C_HI_TEXT : ink;
            int isz = rh * 55 / 100, ix = rx + sub_lx(fb);
            draw_item_icon(fb, ix, ry + (rh - isz) / 2, isz, ovr[r], tc);
            draw_text(fb, ix + isz + fb->w / 60, ry + (rh - 7 * scale) / 2, overlays_label(ovr[r]), scale, tc);
        }
        const char *val = overlays_value(s, ovr[r]);
        if (!val) {
            draw_checkbox(fb, cbx, cby, sz, az_on(ovr[r]), hi);
            continue;
        }
        /* a choice or the time zone: its value, right-aligned where the checkbox would be */
        int px = menu_text_px(fb), right = cbx + sz, cy = ry + (rh - 7 * scale) / 2;
        int tw = azmod_ttf_menu_width ? azmod_ttf_menu_width(val, (int) strlen(val), px, 2) : 0;
        uint32_t vc = (hi && !g_full) ? C_HI_TEXT : ink;
        if (tw <= 0 || !azmod_ttf_menu || !azmod_ttf_menu(fb, right - tw, cy, 0, 7 * scale, val, px, 0, 2, vc))
            draw_text(fb, right - (int) strlen(val) * 6 * scale, cy, val, scale, vc);
    }
}

/* The swatch grid for one pad-mode button. */
static void compose_color_pick(azmod_state *s, const azmod_fb *fb, int rowh, int scale) {
    static const azmod_pad_color PAL[AZMOD_PAD_PALETTE_COUNT] = AZMOD_PAD_PALETTE;
    bool on = s->pad_colors_on;
    int sel = (s->color_sel >= 0 && s->color_sel < AZMOD_PADPICK_COUNT) ? s->color_sel : 0;
    uint32_t ink = on ? C_TEXT : C_TEXT_DIM;
    draw_menu_header(fb, rowh, scale, PICK_LABEL[sel]);
    int cur = colors_cell_of(*pick_slot(s, sel));
    for (int k = 0; k < AZMOD_PAD_SWATCH_CELLS; k++) {
        int sx, sy, sw, sh;
        colors_swatch_rect(fb, k, &sx, &sy, &sw, &sh);
        int in = sw / 10;
        if (in < 3)
            in = 3; /* inset: room for the rings */
        if (k < AZMOD_PAD_PALETTE_COUNT) {
            uint32_t c = PAL[colors_index_of_cell(k)].swatch;
            fill_rect(fb, sx + in, sy + in, sw - 2 * in, sh - 2 * in, on ? c : quarter_rgb(c));
        } else { /* NATIVE: hollow cell with a slash */
            draw_rect_outline(fb, sx + in, sy + in, sw - 2 * in, sh - 2 * in, 1, ink);
            draw_line(fb, sx + sw / 4, sy + sh / 4, sx + sw - sw / 4, sy + sh - sh / 4, 1, ink);
        }
        /* the assigned cell wears a thick orange ring; the knob's cell a white one */
        if (k == cur)
            draw_rect_outline(fb, sx + in / 2, sy + in / 2, sw - in, sh - in, 3, C_HI);
        if (k == s->menu_sel)
            draw_rect_outline(fb, sx, sy, sw, sh, 2, C_TEXT);
    }
}

/* The main list: the rows, the header over them, the pin glyph, the scroll bar and the version footer. */
static void compose_main_list(azmod_state *s, const azmod_fb *fb, int rowh, int scale) {
/* AZ_OPEN: the rows, then the same persistent header as every sub-screen on top of them,
 * so rows scrolled up slide under it (g_cscroll; while the rows fit nothing overlaps). */
    c_cclamp(fb); /* rows can change (a flag, a reload) */
    int vtop, vbot;
    c_cview(fb, &vtop, &vbot);
    for (int i = 0; i < AZI_COUNT; i++) {
        if (item_hidden(i))
            continue;
        {
            int rx, ry, rw, rh;
            azmod_item_rect(fb, i, &rx, &ry, &rw, &rh);
            if (ry + rh <= vtop || ry >= vbot)
                continue; /* scrolled out of view */
        }
        if (item_is_bank(i)) {
            draw_bank_button(s, fb, i, scale);
            continue;
        }
        const char *label = az_label(i);
    /* An armed drum roll with no samples on the stick would look identical
     * to a working one; the row itself is the only place to say so. */
        if (i == AZF_DRUM_ROLL && s->drum_no_samples)
            label = "Drum Roll - no samples on USB";
        if (i == AZF_PLAYLIST_EDITING && s->pledit_needs_reload)
            label = "Playlist Editing - Reload to apply";
        draw_row(s, fb, i, rowh, label, scale, C_TEXT);
        {
            int rx, ry, rw, rh;
            azmod_item_rect(fb, i, &rx, &ry, &rw, &rh);
            bool ihi = (s->menu_focused && s->menu_sel == i) ||
                       (s->pressed_item == i && s->pressed_ticks > 0);
            int isz = rh * 3 / 4;
        /* label, icon and checkbox are all centered on the row, so they line up
         * with each other and with the highlight band */
            draw_item_icon(fb, rx + fb->w / 40, ry + (rh - isz) / 2, isz, i,
                           ihi ? C_HI_TEXT : C_TEXT);
        /* Update Mods: a red dot on the icon's top-left corner while the channel
         * has a newer set (azmod.c keeps update_avail fresh while the menu is up). */
            if (i == AZI_UPDATE && s->update_avail) {
                int r = isz / 5;
                if (r < 4)
                    r = 4;
                int cx = rx + fb->w / 40 + r, cy = ry + (rh - isz) / 2 + r;
                fill_disc(fb, cx, cy, r + 1,
                          0x00000000u); /* 1px dark ring: reads on both backgrounds */
                fill_disc(fb, cx, cy, r, 0x00FF3B30u);
            }
        }
    /* Toggles show the standard checkbox indicator (right of the row) instead
     * of "ON"/"OFF" text: filled orange when enabled. */
        int on = -1;
        if (c_has_box(i))
            on = az_on(i); /* mods get a checkbox */
        if (on >= 0) {
            int rx, ry, rw, rh;
            azmod_item_rect(fb, i, &rx, &ry, &rw, &rh);
            int sz = g_full ? fb->h * 20 / 800 : rh / 2,
                cbx = rx + rw - sz - (g_full ? fb->w / 40 : rh / 3), cby = ry + (rh - sz) / 2;
            bool hi = (s->menu_focused && s->menu_sel == i) ||
                      (s->pressed_item == i && s->pressed_ticks > 0);
            draw_checkbox(fb, cbx, cby, sz, on != 0 && az_active(i), hi);
        }
    /* Items that open a sub-screen get a ">" where a mod's checkbox sits. */
        if (az_screen(i) >= 0 && !(az_row(i)->d.flags & AZ_ROW_NO_CHEVRON)) {
            int rx, ry, rw, rh;
            azmod_item_rect(fb, i, &rx, &ry, &rw, &rh);
            int sz = g_full ? fb->h * 20 / 800 : rh / 2,
                cbx = rx + rw - sz - (g_full ? fb->w / 40 : rh / 3), cby = ry + (rh - sz) / 2;
            bool hi = (s->menu_focused && s->menu_sel == i) ||
                      (s->pressed_item == i && s->pressed_ticks > 0);
            uint32_t cc = hi ? C_HI_TEXT : C_TEXT;
        /* the native source-row chevron's proportions, as tall as the checkbox */
            int chw = sz * 16 / 28;
            draw_native_chevron(fb, cbx + (sz - chw) / 2, cby, sz, cc);
        }
    }
    draw_menu_header(fb, rowh, scale, NULL);
    if (g_docked && s->pinrows_on) { /* pin toggle glyph, docked header top-right */
        int px, py, pw, ph;
        pin_rect(fb, &px, &py, &pw, &ph);
        bool pinnable = az_pinnable(s->menu_sel);
        bool pinned = pinnable && az_pinned(s->menu_sel);
        draw_pin_glyph(fb, px + pw / 2, py + ph / 2, pw / 6,
                       pinned ? C_HI : (pinnable ? C_TEXT_DIM : C_DIVIDER));
    }
/* EP147's own list scroll bar, only while the rows overflow (ui_draw.c draw_vsb) */
    draw_vsb(fb, menu_bx(fb) + (g_docked ? dock_w(fb) : menu_content_w(fb)), vtop, vbot - vtop,
             c_ccontent(fb), vbot - vtop, g_cscroll);
/* Version footer, bottom-right of the main menu only (not sub-screens): small,
 * dim, native face. Right edge ~14px in, baseline ~8px up. */
    {
        char ver[48];
        const char *lv = azmod_live_version();
        if (lv[0])
            snprintf(ver, sizeof ver, "AZ-MODS v%s", lv);
        else
            snprintf(ver, sizeof ver, "AZ-MODS"); /* unknown: no number, never a stale one */
        int right = menu_bx(fb) + (g_docked ? dock_w(fb) : menu_content_w(fb));
        int bottom = g_docked ? (menu_by(fb) + dock_h(fb)) : fb->h;
        int vpx = fb->h / 36;
        if (vpx < 8)
            vpx = 8; /* one step under the row labels */
        int estw =
            (int) strlen(ver) * vpx * 55 / 100; /* ~0.55em avg advance, for the right anchor */
        int vh = vpx * 5 / 4;
        int vx = right - 14 - estw, vy = bottom - 8 - vh;
        if (!azmod_ttf_menu || !azmod_ttf_menu(fb, vx, vy, estw, vh, ver, vpx, 0, 2, C_TEXT_DIM))
            draw_text(fb, right - 14 - (int) strlen(ver) * 6 * scale, bottom - 8 - 7 * scale, ver,
                      scale, C_TEXT_DIM);
    }
}

void azmod_composite(azmod_state *s, const azmod_fb *fb) {
    if (s->mode == AZ_CLOSED) {
        compose_closed(s, fb);
        return;
    }

    g_full = g_docked && s->menu_focused;
    g_full_mode = s->mode;
    if (g_docked && !s->menu_focused) {
        c_draw_preview(s, fb);
        return;
    }
    menu_backdrop(fb); /* compact left scrim + gradient fade into EP147's UI */
    int scale = fb->h / 220;
    if (scale < 1)
        scale = 1;
    int rowh = menu_rowh(fb);
    if (g_full) {
        if (s->mode == AZ_COMMUNITY && g_cm_full && !cm_animating()) {
            /* full-screen Community Mods paints the whole menu area opaque: the left list under it
             * would never be seen, so only its rows are kept current */
            c_lsync(s, fb);
            cm_draw(s, fb);
            return;
        }
        c_draw_left(s, fb);
        if (c_rows_pane(s)) { /* a mod's settings, or a group's screen (Browse View Mods) */
            c_draw_right(s, fb);
            return;
        }
        if (s->mode == AZ_COMMUNITY) {
            if (cm_animating()) cm_slide_draw(s, fb);
            else cm_draw(s, fb);
            return;
        }
        /* a sub-screen: drawn in the right pane below. */
    }

    if (s->mode == AZ_COMMUNITY) { /* not docked: Community Mods lives in the full-screen menu */
        draw_menu_header(fb, rowh, scale, "Community Mods");
        return;
    }
    if (s->mode == AZ_INFO) {
        compose_info(s, fb, rowh, scale);
        return;
    }
    if (s->mode == AZ_UPDATE) {
        compose_updates(s, fb, rowh, scale);
        return;
    }

    if (s->mode == AZ_COLORS) {
        compose_pad_colors(s, fb, rowh, scale);
        return;
    }

    if (sub_mode(s->mode)) {
        compose_overlays(s, fb, rowh, scale);
        return;
    }

    if (s->mode == AZ_COLOR_PICK) {
        compose_color_pick(s, fb, rowh, scale);
        return;
    }

    if (s->mode == AZ_CLOCK_TZ) {
        compose_clock_tz(s, fb, rowh, scale);
        return;
    }

    if (s->mode == AZ_CLOCK_SET) {
        compose_clock_set(s, fb, rowh, scale);
        return;
    }

    compose_main_list(s, fb, rowh, scale);
}
