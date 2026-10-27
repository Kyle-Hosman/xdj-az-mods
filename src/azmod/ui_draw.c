/* SPDX-License-Identifier: GPL-3.0-only */
/* ui_draw.c: the drawing primitives: colors, pixels, rects, text, checkboxes, icons, rounded rects, the 5x7 font.
 * Part of the Mod Menu's one translation unit: #included by menu.c, in the order listed there. */
/* --- Colors (XRGB8888) ------------------------------------------------ */
#define C_ROW_BG 0x00202020u /* idle row: near-black like the native list */
#define C_TEXT 0x00FFFFFFu
#define C_TEXT_DIM 0x00808080u /* disabled mod label */
#define C_MENU_SCRIM \
    0x00282828u /* menu backdrop: EP147's own right-pane gray (USB1 source screen) */
#define C_DIVIDER 0x00404040u /* title/content separator */
#define C_HEADER_BG \
    0x001A1A1Au /* menu header band: darker than the scrim so the                                  \
                 * "MOD MENU" title reads as chrome, not a row */
/* One highlight style wherever a row is "current", selected by navigation or
 * mid tap-feedback: full-row orange fill, black text. */
#define C_HI 0x00FF8C00u
#define C_OK \
    0x0022E022u /* "Up to date" (Update Mods): the VOCAL stem green, so it matches the UI */
#define C_AVAIL \
    0x00FFD400u /* "Available vX" (Update Mods): yellow, a notice rather than the orange selection \
                 */
#define C_HI_TEXT 0x00000000u
#define C_CHECK_BOX 0x00808080u /* checkbox outline */
#define C_CHECK_ON 0x00FF8C00u  /* checkbox fill when enabled (orange = C_HI) */
/* Stem buttons, rekordbox-style: active = black fill with the stem color as
 * border and label; inactive = dark-gray fill with a light-gray border/label. */
#define C_STEM_ON_BG 0x00000000u
#define C_STEM_OFF_BG 0x00242424u
#define C_STEM_OFF_FG 0x008A8A8Au /* border + label when inactive */

static const uint8_t FONT[128][5]; /* defined at bottom */

static void put_px(const azmod_fb *fb, int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= fb->w || y >= fb->h)
        return;
    *(uint32_t *) (fb->px + (long) y * fb->stride + (long) x * 4) = azm_theme(c);
}

static void fill_rect(const azmod_fb *fb, int x, int y, int w, int h, uint32_t c);
/* Source-over blend of `c` at alpha a (0-255); the stem layers use it so the
 * lower ones show through instead of being painted out. */
static void blend_rect(const azmod_fb *fb, int x, int y, int w, int h, uint32_t c, int a) {
    if (a >= 255) {
        fill_rect(fb, x, y, w, h, c);
        return;
    }
    if (a <= 0)
        return;
    uint32_t tc = azm_theme(c); /* d is already themed: theme c once, store raw */
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) {
            if (i < 0 || j < 0 || i >= fb->w || j >= fb->h)
                continue;
            uint32_t *dp =
                (uint32_t *) ((char *) fb->px + (size_t) j * fb->stride + (size_t) i * 4);
            uint32_t d = *dp, o = 0;
            for (int sh = 0; sh <= 16; sh += 8) {
                int dv = (d >> sh) & 0xff, sv = (tc >> sh) & 0xff;
                o |= (uint32_t) (dv + ((sv - dv) * a) / 255) << sh;
            }
            *dp = o;
        }
}
static void fill_rect(const azmod_fb *fb, int x, int y, int w, int h, uint32_t c) {
    /* clip in 64-bit: a huge w or x (a community mod's "to the edge") must not wrap around */
    long long x0 = x, y0 = y, x1 = (long long) x + w, y1 = (long long) y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb->w) x1 = fb->w;
    if (y1 > fb->h) y1 = fb->h;
    if (x1 <= x0 || y1 <= y0)
        return;
    x = (int) x0;
    y = (int) y0;
    w = (int) (x1 - x0);
    h = (int) (y1 - y0);
    /* theme the color once per rectangle, not per pixel (put_px does it per pixel) */
    uint32_t tc = azm_theme(c);
    for (int j = y; j < y + h; j++) {
        uint32_t *row = (uint32_t *) (fb->px + (long) j * fb->stride) + x;
        for (int i = 0; i < w; i++)
            row[i] = tc;
    }
}

static void draw_text(const azmod_fb *fb, int x, int y, const char *s, int scale, uint32_t c);
#define C_SRC_AZ C_HI          /* the Mod Menu orange */
#define C_SRC_GRAY 0x00969696u /* rule/caption gray (150) */
void azmod_draw_srcrow_icon(const azmod_fb *fb, int x, int y, int w, int h, bool open) {
    /* The logo cell is always black: no open/highlight tint. */
    (void) open;
    fill_rect(fb, x, y, w, h, 0xFF000000);
    /* AZ / rule / MODS, drawn as text in a 282x238 box: AZ ink on rows 51-136,
     * the rule on 172-175 (x 49-233), the caption below it. Text is
     * resolution-independent, so nothing is rescaled (a bitmap reads as pixelated). */
    int lw = w, lh = w * 238 / 282;
    if (lh > h) {
        lh = h;
        lw = h * 282 / 238;
    }
    int lx = x + (w - lw) / 2, ly = y + (h - lh) / 2;
    int az_y = ly + lh * 51 / 238, az_h = lh * 86 / 238;
    /* MODS: 28 of 238 rows, directly under the rule, large enough to read at a glance. */
    int cap_y = ly + lh * 186 / 238,
        cap_h = lh * 28 / 238;
    if (!azmod_ttf_menu || !azmod_ttf_menu(fb, lx, az_y, lw, az_h, "AZ", az_h * 10 / 7, 0, 1 | 2,
                                           C_SRC_AZ)) /* bold, unhinted */
        draw_text(fb, lx + (lw - 11 * (az_h / 7)) / 2, az_y, "AZ", az_h / 7, C_SRC_AZ);
    int rule_h = lh * 4 / 238;
    if (rule_h < 1)
        rule_h = 1;
    fill_rect(fb, lx + lw * 49 / 282, ly + lh * 172 / 238, lw * 185 / 282, rule_h, C_SRC_GRAY);
    if (!azmod_ttf_menu || !azmod_ttf_menu(fb, lx, cap_y, lw, cap_h, "MODS", cap_h * 10 / 7, 1, 2,
                                           C_SRC_GRAY)) /* unhinted */
        draw_text(fb, lx + (lw - 23) / 2, cap_y, "MODS", 1, C_SRC_GRAY);
}

/* The standard enable/disable indicator: a square outline, filled with an
 * orange square when `on`. Top-left (x,y), side `sz`. `hi` = the row is
 * highlighted (orange fill), so switch to black ink to stay visible on it. */
static bool g_full; /* tentative definition (menu_layout.c): the full-screen menu is drawing */
static void draw_checkbox(const azmod_fb *fb, int x, int y, int sz, bool on, bool hi) {
    if (g_full)
        hi = false; /* full-screen menu: focus is an outline on the row, never inverted colors */
    uint32_t box = hi ? C_HI_TEXT : C_CHECK_BOX;
    uint32_t fill = hi ? C_HI_TEXT : C_CHECK_ON;
    int t = sz / 8;
    if (t < 2)
        t = 2;                                /* border never 1px: a scaled display can drop
                                               * 1px sides, leaving bare lines */
    fill_rect(fb, x, y, sz, t, box);          /* top */
    fill_rect(fb, x, y + sz - t, sz, t, box); /* bottom */
    fill_rect(fb, x, y, t, sz, box);          /* left */
    fill_rect(fb, x + sz - t, y, t, sz, box); /* right */
    if (on) {
        int pad = t + sz / 6;
        fill_rect(fb, x + pad, y + pad, sz - 2 * pad, sz - 2 * pad, fill);
    }
}

#include "menu_icons.h" /* the coverage maps blit_icon draws: the header's back arrow, the item icons */
_Static_assert(AZ_ROW_ICON_N == IC_N, "a community row icon is drawn like the official ones");
static void blit_icon(const azmod_fb *fb, int x, int y, int sz, uint32_t c,
                      const unsigned char *cov);

static int menu_rowh(const azmod_fb *fb);
/* Menu text size: 70% of the row height (24px in 34px rows), so the text and the
 * rows scale together when items are added or removed. */
static int menu_text_px(const azmod_fb *fb) {
    return g_full ? fb->h * 22 / 800 : menu_rowh(fb) * 7 / 10;
} /* full-screen: the pane label size */
static void draw_text(const azmod_fb *fb, int x, int y, const char *s, int scale, uint32_t c) {
    if (scale < 1)
        scale = 1;
    /* EP147's own face, antialiased (text.c), left-aligned in the bitmap font's
     * 7*scale cell so every existing layout keeps its geometry. The 5x7 bitmap
     * below is only the fallback when the .ttf cannot be opened. */
    /* bold=2 (unhinted): the light hinter snaps each glyph's baseline
     * independently, so letters in one word land on different rows. Unhinted
     * keeps one exact baseline. */
    if (azmod_ttf_menu && azmod_ttf_menu(fb, x, y, 0, 7 * scale, s, menu_text_px(fb), 0, 2, c))
        return; /* size follows the row height */
    for (; *s; s++) {
        unsigned ch = (unsigned char) *s;
        if (ch >= 'a' && ch <= 'z')
            ch -= 32; /* uppercase-only font */
        if (ch > 127) {
            x += 6 * scale;
            continue;
        }
        const uint8_t *g = FONT[ch];
        for (int col = 0; col < 5; col++)
            for (int row = 0; row < 7; row++)
                if (g[col] & (1u << row))
                    fill_rect(fb, x + col * scale, y + row * scale, scale, scale, c);
        x += 6 * scale;
    }
}
/* draw_text, centered in [x, x+w): the TTF path measures the real advance widths
 * (ttf_draw centers when w > 0); strlen*6*scale is only right for the 5x7 bitmap
 * fallback, not the proportional face. */
static void draw_text_centered(const azmod_fb *fb, int x, int y, int w, const char *s, int scale,
                               uint32_t c) {
    if (scale < 1)
        scale = 1;
    if (azmod_ttf_menu && azmod_ttf_menu(fb, x, y, w, 7 * scale, s, menu_text_px(fb), 0, 2, c))
        return;
    draw_text(fb, x + (w - (int) strlen(s) * 6 * scale) / 2, y, s, scale, c);
}
/* "X" close glyph: two crossing diagonals, centered at (cx,cy), half-size hs. */
static void draw_close_x(const azmod_fb *fb, int cx, int cy, int hs, uint32_t c) {
    for (int k = -hs; k <= hs; k++) {
        put_px(fb, cx + k, cy + k, c);
        put_px(fb, cx + k, cy - k, c);
        put_px(fb, cx + k + 1, cy + k, c);
        put_px(fb, cx + k + 1, cy - k, c);
    }
}
static bool in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && py >= y && px < x + w && py < y + h;
}

/* --- Menu-item icon glyphs -------------------------------------------- */
/* A quarter-brightness copy of an 0xRRGGBB color: grays the palette swatches
 * while the pad-color master enable is off, so the grid still reads as a palette. */
static uint32_t quarter_rgb(uint32_t c) {
    return ((c >> 2) & 0x003F3F3Fu);
}
static void draw_line(const azmod_fb *fb, int x0, int y0, int x1, int y1, int t, uint32_t c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    if (t < 1)
        t = 1;
    for (;;) {
        fill_rect(fb, x0 - t / 2, y0 - t / 2, t, t, c);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}
/* A solid disc: one horizontal span per row (stacked Bresenham rings of small
 * radius leave holes and read as a diamond). */
static void fill_disc(const azmod_fb *fb, int cx, int cy, int r, uint32_t c) {
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r)
            dx++;
        fill_rect(fb, cx - dx, cy + dy, 2 * dx + 1, 1, c);
    }
}
/* Outline circle (midpoint algorithm), radius r, centered (cx,cy). */
static void draw_circle(const azmod_fb *fb, int cx, int cy, int r, uint32_t c) {
    int x = r, y = 0, err = 0;
    while (x >= y) {
        put_px(fb, cx + x, cy + y, c);
        put_px(fb, cx + y, cy + x, c);
        put_px(fb, cx - y, cy + x, c);
        put_px(fb, cx - x, cy + y, c);
        put_px(fb, cx - x, cy - y, c);
        put_px(fb, cx - y, cy - x, c);
        put_px(fb, cx + y, cy - x, c);
        put_px(fb, cx + x, cy - y, c);
        y++;
        if (err <= 0)
            err += 2 * y + 1;
        if (err > 0) {
            x--;
            err -= 2 * x + 1;
        }
    }
}
static void draw_rect_outline(const azmod_fb *fb, int x, int y, int w, int h, int t, uint32_t c) {
    if (t < 1)
        t = 1;
    fill_rect(fb, x, y, w, t, c);
    fill_rect(fb, x, y + h - t, w, t, c);
    fill_rect(fb, x, y, t, h, c);
    fill_rect(fb, x + w - t, y, t, h, c);
}

/* Main-menu glyphs: CoreUI Icons Free and svgrepo.com icons (see NOTICE),
 * rasterized at build time into 96x96 8-bit coverage maps (menu_icons.h).
 * blit_icon box-filters a map down to the icon cell and blends the ink once with
 * the averaged coverage, as FreeType text is, so edges antialias at any row height. */
static void blend_px(const azmod_fb *fb, int x, int y, uint32_t c, unsigned a);
static void blit_icon(const azmod_fb *fb, int x, int y, int sz, uint32_t c,
                      const unsigned char *cov) {
    if (sz < 1)
        return;
    if (sz > IC_N)
        sz = IC_N; /* box filter only shrinks */
    unsigned sum[IC_N * IC_N] = {0}, cnt[IC_N * IC_N] = {0};
    for (int sy = 0; sy < IC_N; sy++)
        for (int sx = 0; sx < IC_N; sx++) {
            int d = (sy * sz / IC_N) * sz + sx * sz / IC_N;
            sum[d] += cov[sy * IC_N + sx];
            cnt[d]++;
        }
    for (int d = 0; d < sz * sz; d++)
        if (cnt[d])
            blend_px(fb, x + d % sz, y + d / sz, c, sum[d] / cnt[d]);
}
/* The native SOURCE-row chevron's geometry at 1280x800: 16x28 px, a 7.5 px
 * horizontal stroke, flat-cut ends and a pointed tip. Drawn to that shape at any
 * height (no native pixels copied) with 4x4 supersampled coverage and one blend
 * per pixel. */
static void draw_native_chevron(const azmod_fb *fb, int x, int y, int h, uint32_t c) {
    const float NH = 28.0f, NW = 16.0f, NS = 7.5f, N0 = 0.5f;
    float k = (NW - N0) / (NH / 2), sc = NH / (float) h;
    int w = (int) ((float) h * NW / NH + 0.999f);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int hit = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float ny = ((float) j + ((float) sy + 0.5f) / 4) * sc;
                    float nx = ((float) i + ((float) sx + 0.5f) / 4) * sc;
                    float t = ny <= NH / 2 ? ny : NH - ny; /* distance from the nearer end */
                    float xr = k * t + N0, xl = xr - NS;
                    if (xl < 0)
                        xl = 0; /* the flat-cut end */
                    hit += (nx >= xl && nx <= xr);
                }
            if (hit)
                blend_px(fb, x + i, y + j, c, (unsigned) (hit * 255 / 16));
        }
}
/* The stem mark the playlist rows show (stemsmark.h, "3S-12": four 12 x 4 tiles,
 * 3 px apart, a 1 px dark edge: 14 x 27), in the stem colors, scaled to the icon
 * height (the Mod Menu's Stems icon is that mark). Flat rectangles. */
static void draw_stemmark(const azmod_fb *fb, int x, int y, int sz) {
    static const uint32_t SC[AZMOD_STEM_COUNT] = AZMOD_STEM_PALETTE;
    const uint32_t edge = 0x00141414u; /* 0x00RRGGBB, like the palette */
    float s = (float) sz / 27.0f;
    int w = (int) (14 * s + 0.5f), x0 = x + (sz - w) / 2;
    for (int k = 0; k < 4 && k < AZMOD_STEM_COUNT; k++) {
        int ty0 = y + (int) ((k * 7) * s + 0.5f),
            ty1 = y + (int) ((k * 7 + 6) * s + 0.5f); /* edge box: 1 + 4 + 1 */
        int e = (int) (s + 0.5f);
        if (e < 1)
            e = 1;
        fill_rect(fb, x0, ty0, w, ty1 - ty0, edge);
        fill_rect(fb, x0 + e, ty0 + e, w - 2 * e, ty1 - ty0 - 2 * e, SC[k]);
    }
}
/* Anti-aliased line art for the procedural icons: the union of round-capped
 * segments (r < 0) and rings (k = 1), 4x4 supersampled, squared distances only
 * (no libm). One pass over the box, so overlapping strokes never double-blend. */
struct aa_shape {
    int k;
    float a, b, c2, d, r;
}; /* k 0: seg (a,b)-(c2,d) half-width r; k 1: ring center (a,b) radius c2, half-width r */
static void aa_icon(const azmod_fb *fb, int x, int y, int sz, const struct aa_shape *sh, int n,
                    uint32_t col) {
    for (int py = y; py < y + sz; py++)
        for (int px = x; px < x + sz; px++) {
            int hit = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float fx = (float) px + ((float) sx + 0.5f) / 4,
                          fy = (float) py + ((float) sy + 0.5f) / 4;
                    for (int i = 0; i < n; i++) {
                        const struct aa_shape *e = &sh[i];
                        if (e->k == 0) {
                            float dx = e->c2 - e->a, dy = e->d - e->b, l2 = dx * dx + dy * dy;
                            float t = l2 > 0 ? ((fx - e->a) * dx + (fy - e->b) * dy) / l2 : 0;
                            if (t < 0)
                                t = 0;
                            if (t > 1)
                                t = 1;
                            float ex = e->a + t * dx - fx, ey = e->b + t * dy - fy;
                            if (ex * ex + ey * ey <= e->r * e->r) {
                                hit++;
                                break;
                            }
                        } else {
                            float ex = fx - e->a, ey = fy - e->b, d2 = ex * ex + ey * ey;
                            float lo = e->c2 - e->r, hi = e->c2 + e->r;
                            if (d2 >= (lo > 0 ? lo * lo : 0) && d2 <= hi * hi) {
                                hit++;
                                break;
                            }
                        }
                    }
                }
            if (hit)
                blend_px(fb, px, py, col, (unsigned) (hit * 255 / 16));
        }
}
/* One glyph per main-menu item (its AZF_* / AZI_* item id),
 * drawn in a square box (x,y,sz,sz). Every frame asks for the same icons again, and the
 * anti-aliased shapes cost more than the rest of a frame together, so draw_item_icon serves
 * them from icon_cache below; this is the drawing itself. */
static void draw_item_icon_raw(const azmod_fb *fb, int x, int y, int sz, int item, uint32_t c) {
    int t = sz / 8;
    if (t < 1)
        t = 1;
    int cx = x + sz / 2, cy = y + sz / 2;
    switch (item) {
    case AZF_STEMS:
        draw_stemmark(fb, x, y, sz);
        (void) c;
        break; /* the playlist stem mark */
    case AZF_FPS60_WAVEFORMS:
        blit_icon(fb, x, y, sz, c, IC_UI60);
        break; /* bolt */
    case AZF_FPS60_UI:
        blit_icon(fb, x, y, sz, c, IC_UI60);
        break; /* bolt: the pair reads as one group */
    case AZF_FPS60:
        blit_icon(fb, x, y, sz, c, IC_UI60);
        break;                      /* bolt: the 60 FPS group */
    case AZF_EXTRAS: { /* three sliders: small settings */
        float S = (float) sz, X = (float) x, Y = (float) y, w = S * 0.045f, r = S * 0.10f;
        struct aa_shape sh[6] = {
            {0, X + S * .14f, Y + S * .26f, X + S * .86f, Y + S * .26f, w},
            {0, X + S * .36f, Y + S * .26f, X + S * .36f, Y + S * .26f, r},
            {0, X + S * .14f, Y + S * .50f, X + S * .86f, Y + S * .50f, w},
            {0, X + S * .66f, Y + S * .50f, X + S * .66f, Y + S * .50f, r},
            {0, X + S * .14f, Y + S * .74f, X + S * .86f, Y + S * .74f, w},
            {0, X + S * .46f, Y + S * .74f, X + S * .46f, Y + S * .74f, r},
        };
        aa_icon(fb, x, y, sz, sh, 6, c);
        break;
    }
    case AZF_FINE_VINYL_SPEED: { /* a record: ring + label dot */
        float S = (float) sz, cx2 = (float) x + S / 2, cy2 = (float) y + S / 2;
        struct aa_shape sh[2] = {{1, cx2, cy2, S * 0.38f, 0, S * 0.055f},
                                 {0, cx2, cy2, cx2, cy2, S * 0.11f}};
        aa_icon(fb, x, y, sz, sh, 2, c);
        break;
    }
    case AZF_OVERLAYS:
        blit_icon(fb, x, y, sz, c, IC_FPS);
        break; /* speedometer */
    case AZF_CLOCK: { /* a clock face: ring + two hands */
        float S = (float) sz, cx2 = (float) x + S / 2, cy2 = (float) y + S / 2;
        struct aa_shape sh[3] = {{1, cx2, cy2, S * 0.38f, 0, S * 0.055f},
                                 {0, cx2, cy2, cx2, cy2 - S * 0.24f, S * 0.07f},
                                 {0, cx2, cy2, cx2 + S * 0.17f, cy2, S * 0.07f}};
        aa_icon(fb, x, y, sz, sh, 3, c);
        break;
    }
    case AZI_RELOAD:
        blit_icon(fb, x, y, sz, c, IC_RELOAD);
        break;
    case AZI_UPDATE:
        blit_icon(fb, x, y, sz, c, IC_UPDATE);
        break; /* arrow into a tray */
    case AZI_SYSINFO:
        blit_icon(fb, x, y, sz, c, IC_SYSINFO);
        break;
    case AZF_DRUM_ROLL:
        blit_icon(fb, x, y, sz, c, IC_DRUM);
        break;
    case AZF_PLAYLIST_GRAB:  /* same mark as its parent: it is how that mod's move starts */
    case AZF_PLAYLIST_EDITING: { /* a list with an up/down arrow = reorder */
        int lx = x + sz / 10, lw = sz * 52 / 100, th = sz / 9;
        if (th < 2)
            th = 2;
        for (int k = 0; k < 3; k++) /* three list lines */
            fill_rect(fb, lx, y + sz * (22 + 26 * k) / 100, lw, th, c);
        int ax = x + sz * 78 / 100, top = y + sz / 10, bot = y + sz - sz / 10;
        fill_rect(fb, ax - th / 2, top + th, th, bot - top - 2 * th, c); /* shaft */
        for (int k = 0; k <= sz / 7; k++) {                              /* arrow heads */
            fill_rect(fb, ax - k, top + k, 2 * k + 1, 1, c);
            fill_rect(fb, ax - k, bot - k, 2 * k + 1, 1, c);
        }
        break;
    }
    case AZF_BEAT_JUMP_2_FIRST: { /* two right chevrons into a bar = jump ahead */
        float S = (float) sz, X = (float) x, Y = (float) y, w = S * 0.06f;
        float t = Y + S * 0.22f, b = Y + S * 0.78f, m = Y + S * 0.50f;
        struct aa_shape sh[5] = {
            {0, X + S * .12f, t, X + S * .34f, m, w},
            {0, X + S * .34f, m, X + S * .12f, b, w},
            {0, X + S * .42f, t, X + S * .64f, m, w},
            {0, X + S * .64f, m, X + S * .42f, b, w},
            {0, X + S * .80f, t, X + S * .80f, b, w},
        };
        aa_icon(fb, x, y, sz, sh, 5, c);
        break;
    }
    case AZF_BT_AUTO_CONNECT: /* the Bluetooth rune */
    case AZF_BT_REMEMBER_CHANNEL: {
        float S = (float) sz, X = (float) x, Y = (float) y, w = S * 0.06f;
        float cx = X + S * .48f, t = Y + S * .12f, b = Y + S * .88f, l = X + S * .28f,
              r = X + S * .70f, u = Y + S * .31f, d = Y + S * .69f;
        struct aa_shape sh[5] = {
            {0, cx, t, cx, b, w},
            {0, cx, t, r, u, w},
            {0, r, u, l, d, w},
            {0, l, u, r, d, w},
            {0, r, d, cx, b, w},
        };
        aa_icon(fb, x, y, sz, sh, 5, c);
        break;
    }
    case AZF_PREVIEW_HOT_CUES: { /* a plain waveform with three hot cue lines standing above it */
        static const float P[14] = {.15f, .45f, .30f, .70f, .50f, .85f, .55f, 1.0f, .60f, .80f, .40f, .65f, .35f, .20f};
        float S = (float) sz, mid = S * .63f, half = S * .25f;
        for (int px = 0; px < sz; px++) { /* the waveform: one column per pixel, its top and bottom edges anti-aliased */
            float f = (((float) px + .5f) / S - .10f) / .80f;
            if (f < 0 || f > 1)
                continue;
            float p = f * 13;
            int k = (int) p;
            float t = p - (float) k, h = (P[k] * (1 - t) + P[k < 13 ? k + 1 : 13] * t) * half;
            float top = mid - h, bot = mid + h;
            for (int py = (int) top; py <= (int) bot && py < sz; py++) {
                float cov = (bot < (float) (py + 1) ? bot : (float) (py + 1)) - (top > (float) py ? top : (float) py);
                if (cov > 0)
                    blend_px(fb, x + px, y + py, c, (uint8_t) (cov > 1 ? 255 : cov * 255));
            }
        }
        float X = (float) x, Y = (float) y, w = S * .025f, lt = Y + S * .14f, lb = Y + mid;
        struct aa_shape sh[3] = {
            {0, X + S * .24f, lt, X + S * .24f, lb, w},
            {0, X + S * .50f, lt, X + S * .50f, lb, w},
            {0, X + S * .78f, lt, X + S * .78f, lb, w},
        };
        aa_icon(fb, x, y, sz, sh, 3, c);
        break;
    }
    case AZF_EXTENDED_LEAD_IN: { /* a platter, the track's start and a long arrow back */
        float S = (float) sz, X = (float) x, Y = (float) y, w = S * 0.05f;
        float ay = Y + S * .84f, tip = X + S * .12f, h = S * .10f;
        struct aa_shape sh[5] = {
            {1, X + S * .42f, Y + S * .40f, S * .24f, 0, w},
            {0, X + S * .86f, Y + S * .14f, X + S * .86f, Y + S * .70f, w},
            {0, X + S * .86f, ay, tip, ay, w},
            {0, tip, ay, tip + h, ay - h, w},
            {0, tip, ay, tip + h, ay + h, w},
        };
        aa_icon(fb, x, y, sz, sh, 5, c);
        break;
    }
    case AZF_CAMELOT_KEYS: { /* a key wheel: two rings and four spokes (Camelot Keys) */
        float S = (float) sz, X = (float) x, Y = (float) y, w = S * 0.05f;
        float cx = X + S * .50f, cy = Y + S * .50f, R = S * .36f, r = S * .16f;
        struct aa_shape sh[6] = {
            {1, cx, cy, R, 0, w},
            {1, cx, cy, r, 0, w},
            {0, cx, cy - R, cx, cy - r, w},
            {0, cx, cy + r, cx, cy + R, w},
            {0, cx - R, cy, cx - r, cy, w},
            {0, cx + r, cy, cx + R, cy, w},
        };
        aa_icon(fb, x, y, sz, sh, 6, c);
        break;
    }
    case AZF_WAVE_ZOOM: { /* a magnifier with a plus (Remember Waveform Zoom) */
        float S = (float) sz, X = (float) x, Y = (float) y, w = S * 0.05f;
        float cx = X + S * .42f, cy = Y + S * .42f, R = S * .26f, p = S * .12f;
        struct aa_shape sh[4] = {
            {1, cx, cy, R, 0, w},
            {0, X + S * .61f, Y + S * .61f, X + S * .86f, Y + S * .86f, w * 1.6f},
            {0, cx - p, cy, cx + p, cy, w},
            {0, cx, cy - p, cx, cy + p, w},
        };
        aa_icon(fb, x, y, sz, sh, 4, c);
        break;
    }
    case AZF_PHASE_METER: { /* two rulers with ticks, offset (Phase Meter) */
        float S = (float) sz, X = (float) x, Y = (float) y, w = S * 0.05f;
        float l = X + S * .10f, r = X + S * .90f, ya = Y + S * .36f, yb = Y + S * .68f,
              t = S * .10f;
        struct aa_shape sh[8] = {
            {0, l, ya, r, ya, w},
            {0, l, yb, r, yb, w},
            {0, X + S * .22f, ya - t, X + S * .22f, ya + t, w},
            {0, X + S * .50f, ya - t, X + S * .50f, ya + t, w},
            {0, X + S * .78f, ya - t, X + S * .78f, ya + t, w},
            {0, X + S * .30f, yb - t, X + S * .30f, yb + t, w},
            {0, X + S * .58f, yb - t, X + S * .58f, yb + t, w},
            {0, X + S * .86f, yb - t, X + S * .86f, yb + t, w},
        };
        aa_icon(fb, x, y, sz, sh, 8, c);
        break;
    }
    case AZF_THEMES: { /* a sun (Day Mode): a ring and eight rays */
        float S = (float) sz, X = (float) x, Y = (float) y, w = S * 0.05f, cx = X + S * .50f,
              cy = Y + S * .50f;
        float r0 = S * .27f, r1 = S * .40f;
        static const float dx[8] = {1, .7071f, 0, -.7071f, -1, -.7071f, 0, .7071f};
        static const float dy[8] = {0, .7071f, 1, .7071f, 0, -.7071f, -1, -.7071f};
        struct aa_shape sh[9];
        for (int k = 0; k < 8; k++)
            sh[k] = (struct aa_shape) {
                0, cx + dx[k] * r0, cy + dy[k] * r0, cx + dx[k] * r1, cy + dy[k] * r1, w};
        sh[8] = (struct aa_shape) {1, cx, cy, S * .15f, 0, w};
        aa_icon(fb, x, y, sz, sh, 9, c);
        break;
    }
    case AZF_DECK_KEY_TITLE: { /* a title bar: the title's line, a key mark at its end */
        float S = (float) sz, X = (float) x, Y = (float) y, w = S * 0.05f;
        float l = X + S * .10f, r = X + S * .90f, t = Y + S * .30f, b = Y + S * .70f,
              m = Y + S * .50f;
        struct aa_shape sh[6] = {
            {0, l, t, r, t, w},
            {0, l, b, r, b, w},
            {0, l, t, l, b, w},
            {0, r, t, r, b, w},
            {0, X + S * .20f, m, X + S * .52f, m, w},
            {1, X + S * .72f, m, S * .08f, 0, w},
        };
        aa_icon(fb, x, y, sz, sh, 6, c);
        break;
    }
    case AZF_HIDE_TRACK_TITLE: { /* an eye (Hide Track Title) */
        float S = (float) sz, X = (float) x, Y = (float) y, w = S * 0.05f;
        float l = X + S * .10f, r = X + S * .90f, m = Y + S * .50f, cx = X + S * .50f;
        float t = Y + S * .26f, b = Y + S * .74f, ql = X + S * .30f, qr = X + S * .70f;
        struct aa_shape sh[7] = {
            {0, l, m, ql, t, w},
            {0, ql, t, qr, t, w},
            {0, qr, t, r, m, w},
            {0, l, m, ql, b, w},
            {0, ql, b, qr, b, w},
            {0, qr, b, r, m, w},
            {1, cx, m, S * .06f, 0, S * .07f}, /* pupil: a ring that fills its own center */
        };
        aa_icon(fb, x, y, sz, sh, 7, c);
        break;
    }
    case AZI_COMMUNITY: { /* the desktop app's Community icon: three outlined
                                    * squares and a plus in the fourth slot. Its 20-unit
                                    * viewBox (ink 2.5..17.5) maps onto the 0.14..0.86 span the
                                    * other line icons use; segment caps round the corners. */
        float S = (float) sz, X = (float) x, Y = (float) y, w = S * 0.045f;
#define CMU(u) ((u - 2.5f) / 15.f * 0.72f + 0.14f)
        struct aa_shape sh[14];
        int n = 0;
        const float sq[3][2] = {{2.5f, 2.5f}, {11.5f, 2.5f}, {2.5f, 11.5f}};
        for (int k = 0; k < 3; k++) {
            float x0 = X + S * CMU(sq[k][0]), y0 = Y + S * CMU(sq[k][1]);
            float x1 = X + S * CMU(sq[k][0] + 6.f), y1 = Y + S * CMU(sq[k][1] + 6.f);
            sh[n++] = (struct aa_shape) {0, x0, y0, x1, y0, w};
            sh[n++] = (struct aa_shape) {0, x1, y0, x1, y1, w};
            sh[n++] = (struct aa_shape) {0, x1, y1, x0, y1, w};
            sh[n++] = (struct aa_shape) {0, x0, y1, x0, y0, w};
        }
        sh[n++] = (struct aa_shape) {
            0, X + S * CMU(14.5f), Y + S * CMU(11.5f), X + S * CMU(14.5f), Y + S * CMU(17.5f), w};
        sh[n++] = (struct aa_shape) {
            0, X + S * CMU(11.5f), Y + S * CMU(14.5f), X + S * CMU(17.5f), Y + S * CMU(14.5f), w};
#undef CMU
        aa_icon(fb, x, y, sz, sh, n, c);
        break;
    }
    case AZF_BROWSE_VIEW: { /* a track list: three rows, each an artwork square and a line */
        float S = (float) sz, X = (float) x, Y = (float) y, w = S * 0.045f, r = S * 0.07f;
        struct aa_shape sh[6] = {
            {0, X + S * .20f, Y + S * .28f, X + S * .20f, Y + S * .28f, r},
            {0, X + S * .38f, Y + S * .28f, X + S * .86f, Y + S * .28f, w},
            {0, X + S * .20f, Y + S * .50f, X + S * .20f, Y + S * .50f, r},
            {0, X + S * .38f, Y + S * .50f, X + S * .86f, Y + S * .50f, w},
            {0, X + S * .20f, Y + S * .72f, X + S * .20f, Y + S * .72f, r},
            {0, X + S * .38f, Y + S * .72f, X + S * .86f, Y + S * .72f, w},
        };
        aa_icon(fb, x, y, sz, sh, 6, c);
        break;
    }
    case AZF_COLUMN_EDIT: { /* a table: an outline split into three columns */
        float S = (float) sz, X = (float) x, Y = (float) y, w = S * 0.05f;
        float l = X + S * .14f, r = X + S * .86f, t = Y + S * .22f, b = Y + S * .78f;
        struct aa_shape sh[7] = {
            {0, l, t, r, t, w},
            {0, l, b, r, b, w},
            {0, l, t, l, b, w},
            {0, r, t, r, b, w},
            {0, l, Y + S * .36f, r, Y + S * .36f, w},
            {0, X + S * .38f, t, X + S * .38f, b, w},
            {0, X + S * .62f, t, X + S * .62f, b, w},
        };
        aa_icon(fb, x, y, sz, sh, 7, c);
        break;
    }
    case AZI_PAD_COLORS: { /* a 2x2 of color squares = palette */
        static const uint32_t g[4] = {0xFF2020u, 0xFFD400u, 0x22E022u, 0x2040FFu};
        int inset =
            sz * 19 /
            100; /* ink ~62% of the cell, like the glyph icons */
        x += inset;
        y += inset;
        sz -= 2 * inset;
        t = sz / 8;
        if (t < 1)
            t = 1;
        int q = sz / 2 - t;
        fill_rect(fb, x, y, q, q, g[0]);
        fill_rect(fb, x + sz - q, y, q, q, g[1]);
        fill_rect(fb, x, y + sz - q, q, q, g[2]);
        fill_rect(fb, x + sz - q, y + sz - q, q, q, g[3]);
        break;
    }
    default: { /* a community mod's row: its own icon, or (none given) a square with a plus, the
                * Community Mods icon's own strokes on the same span */
        const struct az_row *r = item >= AZI_REG0 && item < AZI_ACT0 ? az_row(item) : NULL;
        if (r && r->d.icon) {
            blit_icon(fb, x, y, sz, c, r->d.icon);
            break;
        }
        if (!r)
            break;
        float S = (float) sz, X = (float) x, Y = (float) y, w = S * 0.045f, a = 0.14f, b = 0.86f, m = 0.5f, p = 0.17f;
        struct aa_shape sh[6] = {
            {0, X + S * a, Y + S * a, X + S * b, Y + S * a, w}, {0, X + S * b, Y + S * a, X + S * b, Y + S * b, w}, {0, X + S * b, Y + S * b, X + S * a, Y + S * b, w}, {0, X + S * a, Y + S * b, X + S * a, Y + S * a, w}, {0, X + S * m, Y + S * (m - p), X + S * m, Y + S * (m + p), w}, {0, X + S * (m - p), Y + S * m, X + S * (m + p), Y + S * m, w}};
        aa_icon(fb, x, y, sz, sh, 6, c);
        break;
    }
    }
    (void) cx;
    (void) cy;
}

/* ---- icon cache ----------------------------------------------------------------------------
 * An official icon is a pure function of its item, size and ink, the theme's palette, and the
 * pixels under it. Drawn on a band of one color (every menu row), its finished pixels are a
 * function of that color too, so they are kept and copied back the next time the same icon is
 * asked for on the same color: the same pixels, without the shapes. Not kept: a community mod's
 * icon (its own bitmap, which the mod can change), an icon clipped by the buffer's edge, and one
 * on anything but a single color; those draw every time. Overlay thread only, like every caller
 * of the menu's drawing (azmod_composite). */
#define IC_CACHE_N 48    /* entries: every row icon of the left list and the largest pane */
#define IC_CACHE_MAX 96  /* px: larger icons draw every time */
static struct {
    int item, sz;
    uint32_t ink, ground;
    unsigned theme;
    unsigned long used; /* 0 = empty */
    uint32_t px[IC_CACHE_MAX * IC_CACHE_MAX];
} *g_icache;
static unsigned long g_icache_clock;
static uint32_t *ic_row(const azmod_fb *fb, int x, int y) {
    return (uint32_t *) (fb->px + (size_t) y * fb->stride + (size_t) x * 4);
}
/* the one color under the box, or 0 with *ok false when it is not one color */
static uint32_t ic_ground(const azmod_fb *fb, int x, int y, int sz, bool *ok) {
    uint32_t g = *ic_row(fb, x, y);
    for (int j = 0; j < sz; j++) {
        const uint32_t *r = ic_row(fb, x, y + j);
        for (int i = 0; i < sz; i++)
            if (r[i] != g) {
                *ok = false;
                return 0;
            }
    }
    *ok = true;
    return g;
}
static void draw_item_icon(const azmod_fb *fb, int x, int y, int sz, int item, uint32_t c) {
    if (sz <= 0 || x >= fb->w || y >= fb->h || x + sz <= 0 || y + sz <= 0)
        return; /* wholly outside the buffer: every pixel would be clipped, after all the shapes */
    bool ok = !(item >= AZI_REG0 && item < AZI_ACT0) && sz > 0 && sz <= IC_CACHE_MAX && x >= 0 &&
              y >= 0 && x + sz <= fb->w && y + sz <= fb->h;
    uint32_t ground = ok ? ic_ground(fb, x, y, sz, &ok) : 0;
    if (ok && !g_icache)
        g_icache = calloc(IC_CACHE_N, sizeof *g_icache);
    if (!ok || !g_icache) {
        draw_item_icon_raw(fb, x, y, sz, item, c);
        return;
    }
    unsigned theme = azm_theme_key();
    int slot = 0;
    for (int k = 0; k < IC_CACHE_N; k++) {
        if (g_icache[k].used && g_icache[k].item == item && g_icache[k].sz == sz && g_icache[k].ink == c &&
            g_icache[k].ground == ground && g_icache[k].theme == theme) {
            g_icache[k].used = ++g_icache_clock;
            for (int j = 0; j < sz; j++)
                memcpy(ic_row(fb, x, y + j), g_icache[k].px + (size_t) j * sz, (size_t) sz * 4);
            return;
        }
        if (g_icache[k].used < g_icache[slot].used)
            slot = k; /* an empty entry, else the least recently used */
    }
    draw_item_icon_raw(fb, x, y, sz, item, c);
    g_icache[slot].item = item;
    g_icache[slot].sz = sz;
    g_icache[slot].ink = c;
    g_icache[slot].ground = ground;
    g_icache[slot].theme = theme;
    g_icache[slot].used = ++g_icache_clock;
    for (int j = 0; j < sz; j++)
        memcpy(g_icache[slot].px + (size_t) j * sz, ic_row(fb, x, y + j), (size_t) sz * 4);
}

void azmod_draw_text_at(const azmod_fb *fb, int x, int y, const char *s, int scale, uint32_t c) {
    draw_text(fb, x, y, s, scale, c);
}

static void c_text(const azmod_fb *fb, int x, int y, int w, int h, const char *s, int px,
                   uint32_t c) {
    if (azmod_ttf_menu && azmod_ttf_menu(fb, x, y, w, h, s, px, 0, 2, c))
        return;
    int sc = px / 8;
    if (sc < 1)
        sc = 1;
    draw_text(fb, x, y + (h - 7 * sc) / 2, s, sc, c);
}
/* The text size of the value in a Mod Menu choice or bank button (official rows and community
 * mods' rows alike): 40% of the button's height, smaller when it would not fit inside the button
 * with a margin of an eighth of its width on each side, never below a quarter of its height. */
static int c_btn_px(int w, int h, const char *s) {
    int px = h * 40 / 100, room = w - w / 4, n = (int) strlen(s);
    while (azmod_ttf_menu_width && px > h / 4 && azmod_ttf_menu_width(s, n, px, 2) > room)
        px--;
    return px;
}
static void c_btn_text(const azmod_fb *fb, int x, int y, int w, int h, const char *s, uint32_t c) {
    c_text(fb, x, y, w, h, s, c_btn_px(w, h, s), c);
}
static void c_box(const azmod_fb *fb, int x, int y, int sz, bool on, uint32_t edge, uint32_t fill) {
    int t = sz / 8;
    if (t < 2)
        t = 2;
    draw_rect_outline(fb, x, y, sz, sz, t, edge);
    if (on) {
        int pad = t + sz / 6;
        fill_rect(fb, x + pad, y + pad, sz - 2 * pad, sz - 2 * pad, fill);
    }
}

/* A one-line description in a fixed-height row: left-aligned at x, and when wider
 * than maxw, cut at a word and ended with "..." (the menu face is drawn byte by
 * byte, so three dots, not the U+2026 glyph). Never wraps: the rows have one line
 * for it. Every pane description goes through here. */
/* s fitted to maxw into t: unchanged, or cut at a word + "..." (returns its width) */
static int c_fit_str(char *t, size_t tn, const char *s, int px, int maxw) {
    int n = (int) strlen(s);
    snprintf(t, tn, "%s", s);
    if (!azmod_ttf_menu_width || maxw <= 0)
        return 0;
    int w = azmod_ttf_menu_width(s, n, px, 2);
    if (w <= maxw)
        return w;
    if (n > (int) tn - 4)
        n = (int) tn - 4;
    int dots = azmod_ttf_menu_width("...", 3, px, 2), cut = 0;
    for (int i = 1; i <= n; i++) /* the longest prefix ending at a word that fits */
        if ((s[i] == ' ' || s[i] == 0) && azmod_ttf_menu_width(s, i, px, 2) + dots <= maxw)
            cut = i;
    if (!cut)
        /* clang-format off */
        for (cut = n; cut > 0 && azmod_ttf_menu_width(s, cut, px, 2) + dots > maxw; cut--)
            ; /* one long word */
        /* clang-format on */
    while (cut > 0 && (s[cut - 1] == ',' || s[cut - 1] == ':' || s[cut - 1] == ' '))
        cut--;
    memcpy(t, s, (size_t) cut);
    memcpy(t + cut, "...", 4);
    return azmod_ttf_menu_width(t, (int) strlen(t), px, 2);
}
static void c_text_fit(const azmod_fb *fb, int x, int y, int maxw, int h, const char *s, int px,
                       uint32_t c) {
    char t[256];
    c_fit_str(t, sizeof t, s, px, maxw);
    c_text(fb, x, y, 0, h, t, px, c);
}
static void c_draw_lines(const azmod_fb *fb, int x, int y, const char *t, int px, int lh,
                         uint32_t c, int maxw) {
    char line[160];
    while (*t) {
        const char *e = strchr(t, '\n');
        int len = e ? (int) (e - t) : (int) strlen(t);
        if (len > (int) sizeof line - 1)
            len = sizeof line - 1;
        memcpy(line, t, len);
        line[len] = 0;
        c_text_fit(fb, x, y, maxw, lh, line, px, c);
        y += lh;
        t += len;
        if (*t == '\n')
            t++;
    }
}

/* Alpha-blend `c` over the pixel at (x,y) with 0..255 coverage. */
static void blend_px(const azmod_fb *fb, int x, int y, uint32_t c, unsigned a) {
    if (x < 0 || y < 0 || x >= fb->w || y >= fb->h || !a)
        return;
    uint32_t *p = (uint32_t *) (fb->px + (long) y * fb->stride + (long) x * 4);
    c = azm_theme(c);
    uint32_t d = *p;
    unsigned ia = 255u - a;
    unsigned r = (((c >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * ia) / 255u;
    unsigned g = (((c >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * ia) / 255u;
    unsigned b = ((c & 0xFF) * a + (d & 0xFF) * ia) / 255u;
    *p = (r << 16) | (g << 8) | b;
}
/* Is (px,py) inside the rounded rect (x,y,w,h,r)? Clamp the point into the
 * straight core and measure against the corner radius (the standard test). */
static bool rr_in(float px, float py, float x, float y, float w, float h, float r) {
    float cx = px < x + r ? x + r : (px > x + w - r ? x + w - r : px);
    float cy = py < y + r ? y + r : (py > y + h - r ? y + h - r : py);
    float dx = px - cx, dy = py - cy;
    return dx * dx + dy * dy <= r * r;
}
/* Coverage of (x,y,w,h,r) over pixel (ix,iy), 4x4 supersampled -> 0..16. */
static int rr_cov(int ix, int iy, float x, float y, float w, float h, float r) {
    int n = 0;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++)
            if (rr_in((float) ix + (sx + 0.5f) / 4.0f, (float) iy + (sy + 0.5f) / 4.0f, x, y, w, h,
                      r))
                n++;
    return n;
}
/* Anti-aliased rounded rect: `t`-px `outline` around a `fill` interior, corner
 * radius `r`. The corner arcs are supersampled (16 point tests per pixel, cheap
 * at button sizes) because hard corner pixels read as pixelated at this scale. */
static void round_rect(const azmod_fb *fb, int x, int y, int w, int h, int r, int t, uint32_t fill,
                       uint32_t outline) {
    float fr = (float) r, ir = (float) (r - t) > 0.0f ? (float) (r - t) : 0.0f;
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            int co = rr_cov(i, j, (float) x, (float) y, (float) w, (float) h, fr);
            if (!co)
                continue;
            int ci = rr_cov(i, j, (float) (x + t), (float) (y + t), (float) (w - 2 * t),
                            (float) (h - 2 * t), ir);
            /* interior first (only where it actually covers), then the ring */
            if (ci)
                blend_px(fb, i, j, fill, (unsigned) (ci * 255 / 16));
            if (co > ci)
                blend_px(fb, i, j, outline, (unsigned) ((co - ci) * 255 / 16));
        }
    }
}

/* EP147's own list scroll bar (its browse list), measured from screenshots of the track list at
 * 1280x800: stock colors (47 tracks, scrolled mid-list) and the Day theme (99 tracks, at the top).
 *   - a thumb only, no track drawn; 5 px wide, its right edge 2 px inside the list's right edge;
 *   - rounded ends (the two end rows antialiased, VSB_END below);
 *   - gray 0x808080 at alpha 160 over the rows (0x505050 on black, 0x5f5f5f on 0x282828);
 *     under a theme the gray goes through the theme map like every EP147 fill (Day: 0x6d,
 *     0x969696 on the 0xdcdcdc rows, measured on the deck);
 *   - length and position as JUCE's ScrollBar: a thumb area 4 px shorter than the list,
 *     starting 1 px below its top; size = area * visible / total; the thumb drawn 1 px inside
 *     each end of that. Fits both shots: 99 rows -> 61 px, 47 rows -> 131 px, top at list+2.
 * Not measured (no image shows it): the minimum thumb length (VSB_MIN_800 is ours) and the gap
 * at the bottom end (it follows from the same model). Shown only while the list overflows. */
#define VSB_RGB 0x00808080u
#define VSB_ALPHA 160
#define VSB_MIN_800 16
static const unsigned char VSB_END[2][2][5] = {
                                                /* alpha of the end rows, outermost first */
    {{52, 141, 160, 141, 52}, {142, 160, 160, 160, 142}}, /* top */
    {{52, 141, 160, 141, 52}, {141, 160, 160, 160, 141}}, /* bottom */
};
/* The thumb for a list whose rows occupy [top, top+h) and end at x = right (exclusive), showing
 * `view` px of `content` px from offset `off`. false (nothing to draw) when it all fits. */
static bool vsb_geom(const azmod_fb *fb, int right, int top, int h, int content, int view, int off,
                     int *x, int *y, int *w, int *len) {
    if (content <= view || view <= 0)
        return false;
    int s4 = fb->h * 4 / 800, area = h - s4, mn = fb->h * VSB_MIN_800 / 800, max = content - view;
    if (area < 8)
        return false;
    int size = (int) (((long long) area * view + content / 2) / content);
    if (size < mn)
        size = mn;
    if (size > area)
        size = area;
    if (off < 0)
        off = 0;
    if (off > max)
        off = max;
    int start = top + s4 / 4 + (int) (((long long) (area - size) * off + max / 2) / max);
    *w = fb->w * 5 / 1280 < 1 ? 1 : fb->w * 5 / 1280;
    *x = right - fb->w * 2 / 1280 - *w;
    *y = start + 1;
    *len = size - 2;
    return true;
}
static void draw_vsb(const azmod_fb *fb, int right, int top, int h, int content, int view, int off) {
    int x, y, w, len;
    if (!vsb_geom(fb, right, top, h, content, view, off, &x, &y, &w, &len))
        return;
    for (int j = 0; j < len; j++) {
        int e = j < 2 ? 0 : j >= len - 2 ? 1
                                         : -1,
            r = e == 0 ? j : len - 1 - j;
        for (int i = 0; i < w; i++)
            blend_px(fb, x + i, y + j, VSB_RGB, (e >= 0 && w == 5) ? VSB_END[e][r][i] : VSB_ALPHA);
    }
}

/* --- 5x7 uppercase font (column-major, bit N = row N from top). Only glyphs
 * used by labels are filled; add a glyph when a new label needs one. */
static const uint8_t FONT[128][5] = {
    ['A'] = {0x7E, 0x11, 0x11, 0x11, 0x7E},
    ['B'] = {0x7F, 0x49, 0x49, 0x49, 0x36},
    ['C'] = {0x3E, 0x41, 0x41, 0x41, 0x22},
    ['D'] = {0x7F, 0x41, 0x41, 0x22, 0x1C},
    ['E'] = {0x7F, 0x49, 0x49, 0x49, 0x41},
    ['F'] = {0x7F, 0x09, 0x09, 0x09, 0x01},
    ['G'] = {0x3E, 0x41, 0x49, 0x49, 0x7A},
    ['H'] = {0x7F, 0x08, 0x08, 0x08, 0x7F},
    ['I'] = {0x41, 0x41, 0x7F, 0x41, 0x41},
    ['J'] = {0x20, 0x40, 0x41, 0x3F, 0x01},
    ['K'] = {0x7F, 0x08, 0x14, 0x22, 0x41},
    ['L'] = {0x7F, 0x40, 0x40, 0x40, 0x40},
    ['M'] = {0x7F, 0x02, 0x0C, 0x02, 0x7F},
    ['N'] = {0x7F, 0x04, 0x08, 0x10, 0x7F},
    ['O'] = {0x3E, 0x41, 0x41, 0x41, 0x3E},
    ['P'] = {0x7F, 0x09, 0x09, 0x09, 0x06},
    ['R'] = {0x7F, 0x09, 0x19, 0x29, 0x46},
    ['S'] = {0x26, 0x49, 0x49, 0x49, 0x32},
    ['T'] = {0x01, 0x01, 0x7F, 0x01, 0x01},
    ['U'] = {0x3F, 0x40, 0x40, 0x40, 0x3F},
    ['V'] = {0x1F, 0x20, 0x40, 0x20, 0x1F},
    ['X'] = {0x63, 0x14, 0x08, 0x14, 0x63},
    ['Y'] = {0x07, 0x08, 0x70, 0x08, 0x07},
    [' '] = {0, 0, 0, 0, 0},
    /* Q, W, Z */
    ['Q'] = {0x3E, 0x41, 0x51, 0x21, 0x5E},
    ['W'] = {0x3F, 0x40, 0x38, 0x40, 0x3F},
    ['Z'] = {0x61, 0x51, 0x49, 0x45, 0x43},
    ['0'] = {0x3E, 0x51, 0x49, 0x45, 0x3E},
    ['1'] = {0x00, 0x42, 0x7F, 0x40, 0x00},
    ['2'] = {0x42, 0x61, 0x51, 0x49, 0x46},
    ['3'] = {0x21, 0x41, 0x45, 0x4B, 0x31},
    ['4'] = {0x18, 0x14, 0x12, 0x7F, 0x10},
    ['5'] = {0x27, 0x45, 0x45, 0x45, 0x39},
    ['6'] = {0x3C, 0x4A, 0x49, 0x49, 0x30},
    ['7'] = {0x01, 0x71, 0x09, 0x05, 0x03},
    ['8'] = {0x36, 0x49, 0x49, 0x49, 0x36},
    ['9'] = {0x06, 0x49, 0x49, 0x29, 0x1E},
    [':'] = {0x00, 0x00, 0x36, 0x00, 0x00},
    /* symbols for the on-screen keyboard's 123 layout (standard 5x7 set, same bit
     * order as the digits above) */
    ['-'] = {0x08, 0x08, 0x08, 0x08, 0x08},
    ['/'] = {0x20, 0x10, 0x08, 0x04, 0x02},
    [';'] = {0x00, 0x56, 0x36, 0x00, 0x00},
    ['('] = {0x00, 0x1C, 0x22, 0x41, 0x00},
    [')'] = {0x00, 0x41, 0x22, 0x1C, 0x00},
    ['$'] = {0x24, 0x2A, 0x7F, 0x2A, 0x12},
    ['&'] = {0x36, 0x49, 0x55, 0x22, 0x50},
    ['@'] = {0x32, 0x49, 0x79, 0x41, 0x3E},
    ['.'] = {0x00, 0x60, 0x60, 0x00, 0x00},
    [','] = {0x00, 0x50, 0x30, 0x00, 0x00},
    ['?'] = {0x02, 0x01, 0x51, 0x09, 0x06},
    ['!'] = {0x00, 0x00, 0x5F, 0x00, 0x00},
    ['#'] = {0x14, 0x7F, 0x14, 0x7F, 0x14},
    ['%'] = {0x23, 0x13, 0x08, 0x64, 0x62},
    ['+'] = {0x08, 0x08, 0x3E, 0x08, 0x08},
};
