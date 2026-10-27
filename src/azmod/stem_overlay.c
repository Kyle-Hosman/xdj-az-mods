/* SPDX-License-Identifier: GPL-3.0-only */
/* stem_overlay.c: the deck-screen stem UI: strip and lane geometry, the region cache, the mini lanes, the dots and the pill grid.
 * Part of the Mod Menu's one translation unit: #included by menu.c, in the order listed there. */
/* --- env-override caching -------------------------------------------------
 * The strip/band helpers run dozens of times per composite tick and the
 * environment is fixed for the life of the process, so each AZMOD_* geometry
 * override is parsed once on first use. slot is a private index; returns 0 when
 * the variable is absent or invalid. */
static int env_rect_cached(const char *name, int slot, float v[4]) {
    static int st[8];
    static float val[8][4];
    if (slot < 0 || slot >= 8)
        return 0;
    if (!st[slot]) {
        const char *e = getenv(name);
        float a, b, c, d;
        if (e && sscanf(e, "%f,%f,%f,%f", &a, &b, &c, &d) == 4) {
            val[slot][0] = a;
            val[slot][1] = b;
            val[slot][2] = c;
            val[slot][3] = d;
            st[slot] = 1;
        } else
            st[slot] = -1;
    }
    if (st[slot] == 1) {
        v[0] = val[slot][0];
        v[1] = val[slot][1];
        v[2] = val[slot][2];
        v[3] = val[slot][3];
        return 1;
    }
    return 0;
}

/* Per-deck stem-toggle strip: sits exactly on the native pad-grid row-1 slots
 * so the buttons read as part of the grid. Defaults are those slot rects at
 * 1280x800: row y 502-521, deck1 slots x 10/167/324/481, deck2 x 650/807/964/1121,
 * each 148 wide with a 9px gap. AZMOD_STEMSTRIP / AZMOD_STEMSTRIP2 = "x,y,w,h"
 * (ratios) override each strip without a rebuild. */
#define STEM_GAP_FRAC 0.00710f    /* native inter-slot gap: 9px @ 1280 */
#define STEM_ROW2_Y_FRAC 0.66250f /* second (always-empty) slot row: y 530 @ 800 */
#define C_STEM_SLOT 0x00232323u   /* native empty-slot fill */
void azmod_stem_strip_rect(const azmod_fb *fb, int deck, int *x, int *y, int *w, int *h) {
    float sx, sy, sw, sh;
    sy = 0.62775f;
    sw = 0.4840f;
    sh = 0.0252f; /* same row + span on both decks */
    float o[4];
    if (!(deck & 1)) {
        sx = 0.0080f; /* decks 1 and 3: the left side's grid (2-DECK) */
        if (env_rect_cached("AZMOD_STEMSTRIP", 1, o)) {
            sx = o[0];
            sy = o[1];
            sw = o[2];
            sh = o[3];
        }
    } else {
        sx = 0.50820f;
        if (env_rect_cached("AZMOD_STEMSTRIP2", 2, o)) {
            sx = o[0];
            sy = o[1];
            sw = o[2];
            sh = o[3];
        }
    }
    *x = (int) (sx * fb->w);
    *y = (int) (sy * fb->h);
    *w = (int) (sw * fb->w);
    *h = (int) (sh * fb->h);
}
/* Native pad-mode title band at 1280x800: a 1px 0x232323
 * rule at y=489 spanning the deck's pad width, broken by the centered "BEAT LOOP"
 * caption (x 284-353, cap height ~9, color ~0x808080). We black-plate the
 * caption and write "STEMS" in its place, then repaint the rule underneath. */
#define STEM_TITLE_Y_FRAC 0.60400f /* plate top: 483 @ 800 (caption lands on the native 485-493)   \
                                    */
#define STEM_TITLE_W_FRAC 0.05800f /* plate width: 74 @ 1280 (covers "BEAT LOOP") */
#define STEM_TITLE_H_FRAC 0.01760f /* plate height: 14 @ 800 */
#define STEM_RULE_Y_FRAC 0.61140f  /* the 1px rule: 489 @ 800 */
#define C_STEM_RULE 0x00232323u
#define C_STEM_TITLE_FG 0x007D7D7Du /* sampled off the native "BEAT LOOP" caption */
void azmod_stem_title_rect(const azmod_fb *fb, int deck, int *x, int *y, int *w, int *h) {
    int sx, sy, sw, sh;
    azmod_stem_strip_rect(fb, deck, &sx, &sy, &sw, &sh);
    (void) sy;
    (void) sh;
    *w = (int) (STEM_TITLE_W_FRAC * fb->w);
    *h = (int) (STEM_TITLE_H_FRAC * fb->h);
    *x = sx + (sw - *w) / 2; /* the caption is centered on the pad area */
    *y = (int) (STEM_TITLE_Y_FRAC * fb->h);
}
/* Row 2 only: the window for the mini stem waveforms over the native tiles.
 * With native tiles the overlay must not cover the title or row 1 (it would paint
 * black over EP147's own tiles), so this returns only the second slot row;
 * azmod_stem_win_rect covers the whole pad container. */
void azmod_stem_lanes_win_rect(const azmod_fb *fb, int deck, int *x, int *y, int *w, int *h) {
    int sx, sy, sw, sh;
    azmod_stem_strip_rect(fb, deck, &sx, &sy, &sw, &sh);
    (void) sy;
    *x = sx;
    *y = (int) (STEM_ROW2_Y_FRAC * fb->h);
    *w = sw;
    *h = sh;
}
void azmod_stem_win_rect(const azmod_fb *fb, int deck, int *x, int *y, int *w, int *h) {
    int sx, sy, sw, sh;
    azmod_stem_strip_rect(fb, deck, &sx, &sy, &sw, &sh);
    (void) sy;
    int tx, ty, tw, th;
    azmod_stem_title_rect(fb, deck, &tx, &ty, &tw, &th);
    (void) tx;
    (void) tw;
    (void) th;
    /* Title band down through the empty second slot row: azmod.c blanks the
     * whole native pad container, so our window has to cover all of it. */
    *x = sx;
    *y = ty;
    *w = sw;
    *h = (int) (STEM_ROW2_Y_FRAC * fb->h) + sh - ty;
}
/* Slot i of the second (native-empty) pad row, where that stem's
 * mini-waveform lane is drawn. Same columns as the buttons, one row down. */
void azmod_stem_lane_rect(const azmod_fb *fb, int deck, int i, int *x, int *y, int *w, int *h) {
    azmod_stem_btn_rect(fb, deck, i, x, y, w, h);
    *y = (int) (STEM_ROW2_Y_FRAC * fb->h);
}
/* Slot i of the strip. The gap is the native one (fixed px ratio); deriving it
 * from sw/COUNT drifts by the 4th slot and breaks the alignment. */
void azmod_stem_btn_rect(const azmod_fb *fb, int deck, int i, int *x, int *y, int *w, int *h) {
    int sx, sy, sw, sh;
    azmod_stem_strip_rect(fb, deck, &sx, &sy, &sw, &sh);
    int gap = (int) (STEM_GAP_FRAC * fb->w);
    if (gap < 1)
        gap = 1;
    int bw = (sw - gap * (AZMOD_STEM_COUNT - 1)) / AZMOD_STEM_COUNT;
    *x = sx + i * (bw + gap);
    *y = sy;
    *w = bw;
    *h = sh;
}

/* --- static-region cache ---------------------------------------------------
 * The stem grid (title plate + pill buttons) and the 4-deck dots change only
 * when a stem toggles or a deck's stems availability flips, but composite runs
 * on every dirty tick (~33/s while a track plays) and round_rect plus the TTF
 * labels are expensive. Render once into an offscreen buffer keyed by a
 * signature and blit; lane waveforms and playheads still draw every tick. */
typedef struct {
    unsigned sig;   /* content signature; also acts as the "valid" flag */
    int x, y, w, h; /* screen rect the cache covers */
    uint32_t *px;   /* w*h pixels, w*4 stride */
} region_cache;

/* 1 = stale: caller renders into the cache (an azmod_fb whose origin is the
 * region's top-left, so every draw uses (x - region_x, y - region_y)), then
 * rc_blit. 0 = cache hit: caller only calls rc_blit. -1 = no buffer: caller draws
 * straight into fb. (x and y: the cache buffer has the region's stride, so
 * absolute-x draws would wrap into the next row and skew the strip.) */
static int rc_prepare(region_cache *c, int x, int y, int w, int h, unsigned sig) {
    if (w <= 0 || h <= 0)
        return -1;
    /* cached pixels were drawn in one palette, so the theme is part of the key (the
     * ground color alone does not tell themes apart: several share a black ground) */
    sig ^= azm_theme_key() * 0x9E3779B1u;
    if (c->sig != sig || c->x != x || c->y != y || c->w != w || c->h != h) {
        if (!c->px || c->w != w || c->h != h) {
            uint32_t *np = malloc((size_t) w * h * 4);
            if (!np)
                return -1;
            free(c->px);
            c->px = np;
            c->w = w;
            c->h = h;
        }
        c->x = x;
        c->y = y;
        c->sig = sig;
        return 1;
    }
    return 0;
}
static void rc_blit(const region_cache *c, const azmod_fb *fb) {
    if (!c->px || !c->sig)
        return;
    int x0 = c->x < 0 ? 0 : c->x, x1 = c->x + c->w > fb->w ? fb->w : c->x + c->w;
    if (x0 >= x1)
        return;
    for (int j = 0; j < c->h; j++) {
        int y = c->y + j;
        if (y < 0 || y >= fb->h)
            continue;
        memcpy((char *) fb->px + (size_t) y * fb->stride + (size_t) x0 * 4,
               (const char *) c->px + (size_t) j * ((size_t) c->w * 4) + (size_t) (x0 - c->x) * 4,
               (size_t) (x1 - x0) * 4);
    }
}

#define C_STEM_LANE_OFF 0x00404040u /* muted stem: still drawn, in dead gray */
#define C_STEM_LANE_HEAD 0x00FFFFFFu

/* Resolved by azmod.c from azstem.so (same process); NULL when azstem isn't
 * loaded, which means no lanes. See azstem.c azstem_wave(). */
int (*azmod_stem_wave)(int deck, const unsigned char **env, int *n, int *nstem, float *pos);
/* Per-deck envelope identity (azstem_env_gen), resolved like the above. NULL =
 * an older azstem.so without the export: the have-edge latch is the fallback. */
unsigned (*azmod_stem_env_gen)(int deck);
int (*azmod_stem_env_final)(
    int deck); /* defined here: menu.c's lane gate reads it; azmod.c dlsym's it.
                  Without this definition the reference is an unresolved data
                  relocation and azmod.so fails to LD_PRELOAD. */

/* Peak per lane column. The lane maps the whole track onto its width, so the
 * peaks are constant for the life of the envelope; only the playhead moves.
 * Computed once per (envelope, lane width) instead of rescanning every tick.
 * Invalidation: azstem's envelope generation when exported, else the have-latch
 * (azstem_wave() returns 0 throughout every rebuild, so a 0->1 edge means new
 * content). Lanes wider than LANE_W_MAX rescan per draw. */
#define LANE_W_MAX 512
static unsigned char g_lane_pk[AZMOD_STEM_DECKS][AZMOD_STEM_COUNT][LANE_W_MAX];
static int g_lane_pk_w[AZMOD_STEM_DECKS];   /* width the peaks were built at; 0 = none */
static int g_lane_pk_gen[AZMOD_STEM_DECKS]; /* envelope generation they belong to */
static int g_lane_gen[AZMOD_STEM_DECKS];    /* bumped on each have 0->1 transition */
static int g_lane_was_have[AZMOD_STEM_DECKS];

static int lane_peak_at(const unsigned char *env, int n, int px, int w) {
    int b0 = (int) ((long) px * n / w), b1 = (int) ((long) (px + 1) * n / w);
    if (b1 <= b0)
        b1 = b0 + 1;
    if (b1 > n)
        b1 = n;
    int m = 0;
    for (int q = b0; q < b1; q++)
        if (env[q] > m)
            m = env[q];
    return m;
}
/* Build one deck's per-column peaks, each stem scaled against its own peak.
 *
 * A whole track across ~148 px puts ~440 envelope buckets in every column, so
 * a max-per-column overview of a dense stem saturates into a flat strip (the big
 * band avoids this by zooming to the playhead window via azmod_wave_zoom).
 * Scaling each stem to its own peak spends the full lane height on that stem's
 * range; it gives up loudness comparison between stems, which 19 px lanes do
 * not convey anyway. */
static void lane_peaks_build(int deck, const unsigned char *env, int n, int w, unsigned gen) {
    if (w <= 0 || w > LANE_W_MAX || n <= 0)
        return;
    for (int i = 0; i < AZMOD_STEM_COUNT; i++) {
        const unsigned char *lane = env + (size_t) i * n;
        int mx = 0, mn = 255;
        for (int px = 0; px < w; px++) {
            int v = lane_peak_at(lane, n, px, w);
            g_lane_pk[deck][i][px] = (unsigned char) v;
            if (v > mx)
                mx = v;
            if (v < mn)
                mn = v;
        }
        /* Scale by the peak, keeping zero at zero. Not a min-max stretch: the
         * deck's envelope often tops out near 128 with a narrow span (e.g. a vocal
         * at 88..128), and stretching 40 levels across the height quantizes into
         * plateaus while subtracting min drags quiet passages to zero. Dividing by
         * the peak keeps the shape: typically 2x on the deck, a no-op where data
         * reaches full scale. Gain capped at 4x so a near-silent lane stays quiet. */
        if (mx > 24) {
            int gain = 255 / mx;
            if (gain > 4)
                gain = 4;
            if (gain > 1) {
                for (int px = 0; px < w; px++) {
                    int v = g_lane_pk[deck][i][px] * gain;
                    g_lane_pk[deck][i][px] = (unsigned char) (v > 255 ? 255 : v);
                }
            }
        }
    }
    g_lane_pk_w[deck] = w;
    g_lane_pk_gen[deck] = (int) gen;
}

/* One stem's peak envelope drawn center-out in (x,y,w,h), plus the playhead.
 * pk = precomputed per-column peaks for this width, or NULL to scan env. */
static void draw_stem_lane(const azmod_fb *fb, int x, int y, int w, int h, const unsigned char *pk,
                           const unsigned char *env, int n, float pos, uint32_t col) {
    int mid = y + h / 2, half = (h - 2) / 2;
    if (half < 1)
        half = 1;
    for (int px = 0; px < w; px++) {
        int m = pk ? pk[px] : lane_peak_at(env, n, px, w);
        int a = m * half / 255;
        if (a < 1)
            a = 1;
        fill_rect(fb, x + px, mid - a, 1, a * 2, col);
    }
    int hx = x + (int) (pos * (float) (w - 1) + 0.5f);
    fill_rect(fb, hx, y, 1, h, C_STEM_LANE_HEAD);
}

/* The deck's waveform band: the area between the two beat-tick rows at
 * 1280x800, x 176-1066 with the playhead at x 395 (measure mid-track: at
 * position 0 nothing is drawn left of the playhead). Deck 1 y 104-232, deck 2
 * y 300-430. Overridable with AZMOD_WAVEBAND / AZMOD_WAVEBAND2 = "x,y,w,h"
 * fractions. 2-deck layouts only: 4 DECK moves these (dots are shown there). */
void azmod_waveband_rect(const azmod_fb *fb, int deck, int *x, int *y, int *w, int *h) {
    float bx = 0.1375f, bw = 0.6953f, by, bh = 0.1600f; /* 176..1066 */
    float o[4];
    if (!(deck & 1)) {
        by = 0.1300f;
        if (env_rect_cached("AZMOD_WAVEBAND", 3, o)) /* decks 1/3: the upper band (2-DECK) */ {
            bx = o[0];
            by = o[1];
            bw = o[2];
            bh = o[3];
        }
    } else {
        by = 0.3750f;
        if (env_rect_cached("AZMOD_WAVEBAND2", 4, o)) {
            bx = o[0];
            by = o[1];
            bw = o[2];
            bh = o[3];
        }
    }
    *x = (int) (bx * fb->w);
    *y = (int) (by * fb->h);
    *w = (int) (bw * fb->w);
    *h = (int) (bh * fb->h);
}

/* 4-DECK layout: the deck's source cell in the left column ("USB1"), taken
 * over with four stem dots.
 *
 * 4 DECK stacks the rows DECK 3, 1, 2, 4 top-to-bottom. Source-cell tops at
 * 1280x800: y 80 / 202 / 324 / 446 in row order (122 pitch, 38 tall). DY below
 * is indexed by deck, so it reads 202, 324, 80, 446. */
void azmod_stem_dots_rect(const azmod_fb *fb, int deck, int *x, int *y, int *w, int *h) {
    /* Stop short of the eject icon (x 148-159 at 1280x800): it stays visible
     * and, since our window is input-transparent, still tappable. */
    static const float DY[AZMOD_STEM_DECKS] = {0.2525f, 0.4050f, 0.1000f, 0.5575f};
    float dx = 0.0f, dw = 0.1125f, dh = 0.0475f;
    if (deck < 0 || deck >= AZMOD_STEM_DECKS)
        deck = 0;
    float dy = DY[deck];
    float o[4];
    /* Live nudge overrides exist for decks 1/2 only; 3/4 sit on the same
     * uniform grid. */
    if (deck == 0) {
        if (env_rect_cached("AZMOD_STEMDOTS", 5, o)) {
            dx = o[0];
            dy = o[1];
            dw = o[2];
            dh = o[3];
        }
    } else if (deck == 1) {
        if (env_rect_cached("AZMOD_STEMDOTS2", 6, o)) {
            dx = o[0];
            dy = o[1];
            dw = o[2];
            dh = o[3];
        }
    }
    *x = (int) (dx * fb->w);
    *y = (int) (dy * fb->h);
    *w = (int) (dw * fb->w);
    *h = (int) (dh * fb->h);
}

/* Mini stem buttons over the deck's source cell: the same rounded pills as the
 * full strip, sized for a 168x38 cell. Background is the cell's own gray
 * (C_STEM_SLOT = #232323) so the takeover blends in. Indicator only: the window
 * is input-transparent, so the eject button underneath still takes taps and the
 * pads do the toggling. Static per stem mask, so region-cached. */
static region_cache g_dots_rc[AZMOD_STEM_DECKS];
static void dots_static(const azmod_state *s, const azmod_fb *gfb, const azmod_fb *dfb, int deck,
                        int ox, int oy) {
    static const uint32_t SC[AZMOD_STEM_COUNT] = AZMOD_STEM_PALETTE;
    static const char *SL[AZMOD_STEM_COUNT] = {"VO", "IN", "BA", "DR"};
    int bx, by, bw, bh;
    azmod_stem_dots_rect(gfb, deck, &bx, &by, &bw, &bh);
    fill_rect(dfb, bx - ox, by - oy, bw, bh, C_STEM_SLOT); /* the cell's own gray */
    int gap = bw / 40;
    if (gap < 2)
        gap = 2;
    int mx = bw / 28;
    if (mx < 2)
        mx = 2; /* side margin */
    int w = (bw - 2 * mx - gap * (AZMOD_STEM_COUNT - 1)) / AZMOD_STEM_COUNT;
    int h = bh * 7 / 10;
    if (h < 6)
        h = 6;
    int y = by + (bh - h) / 2;
    if (w < 6)
        return;
    for (int i = 0; i < AZMOD_STEM_COUNT; i++) {
        int x = bx + mx + i * (w + gap);
        bool on = s->stem_on[deck][i];
        int r = h / 6;
        if (r < 2)
            r = 2;
        int t = h / 10;
        if (t < 1)
            t = 1;
        uint32_t edge = on ? SC[i] : C_STEM_OFF_FG;
        round_rect(dfb, x - ox, y - oy, w, h, r, t, on ? C_STEM_ON_BG : C_STEM_OFF_BG, edge);
        if (!azmod_ttf_center ||
            !azmod_ttf_center(dfb, x - ox, y - oy, w, h, SL[i], (h * 11) / 20, 0, 0, edge)) {
            int bs = h / 10;
            if (bs < 1)
                bs = 1;
            int tw = 2 * 6 * bs;
            draw_text(dfb, x + (w - tw) / 2 - ox, y + (h - 7 * bs) / 2 - oy, SL[i], bs, edge);
        }
    }
}
static void draw_stem_dots(azmod_state *s, const azmod_fb *fb, int deck) {
    int bx, by, bw, bh;
    azmod_stem_dots_rect(fb, deck, &bx, &by, &bw, &bh);
    unsigned mask = 0;
    for (int i = 0; i < AZMOD_STEM_COUNT; i++)
        mask |= (unsigned) (s->stem_on[deck][i] ? 1 : 0) << i;
    region_cache *c = &g_dots_rc[deck];
    int rc = rc_prepare(c, bx, by, bw, bh, mask << 1 | 1u);
    if (rc > 0) {
        uint32_t ground = azm_theme(0); /* black, or white in Day Mode (see draw_stem_grid) */
        if (ground) {
            for (size_t k = 0; k < (size_t) c->w * (size_t) c->h; k++)
                c->px[k] = ground;
        } else
            memset(c->px, 0, (size_t) c->w * (size_t) c->h * 4);
        azmod_fb cfb = {.px = (uint8_t *) c->px, .w = c->w, .h = c->h, .stride = c->w * 4};
        dots_static(s, fb, &cfb, deck, bx, by);
    }
    if (rc >= 0)
        rc_blit(c, fb);
    else
        dots_static(s, fb, fb, deck, 0, 0);
}

/* Stem Layers: the four stems drawn over each other in the deck's own waveform
 * band, all sharing one center line: drums at the back, then bass, then inst,
 * with vocals painted last so it always reads on top. A muted stem is
 * not drawn, so its color disappears from the pile.
 *
 * Columns map to track position from the zoom model azmod.c learns in the
 * waveform data hook (points per column + where the playhead sits), so the
 * layers track EP147's zoom and scrolling without ever reading its zoom. */
static void draw_stem_lanes(azmod_state *s, const azmod_fb *fb, int deck) {
    static const uint32_t SC[AZMOD_STEM_COUNT] = AZMOD_STEM_PALETTE;
    /* Back to front: DRUMS, BASS, INST, VOCAL (vocals ends up on top). */
    static const int ORDER[AZMOD_STEM_COUNT] = {3, 2, 1, 0};
    const unsigned char *env = 0;
    int nb = 0, nst = 0;
    float pos = 0.f;
    if (!azmod_stem_wave || !azmod_stem_wave(deck, &env, &nb, &nst, &pos) ||
        nst < AZMOD_STEM_COUNT || nb <= 0)
        return;
    double ppc = 0, phcol = 0, ncol = 0, tot = 0;
    if (!azmod_wave_zoom(deck, &ppc, &phcol, &ncol, &tot) || ppc <= 0 || ncol <= 1 || tot <= 0)
        return;

    int bx, by, bw, bh;
    azmod_waveband_rect(fb, deck, &bx, &by, &bw, &bh);
    fill_rect(fb, bx, by, bw, bh, 0x00000000u); /* own the band */
    int mid = by + bh / 2;
    int half = (bh - 2) / 2;
    if (half < 1)
        half = 1;
    /* Semi-transparent so overlapping stems mix instead of the top one painting
     * the rest out (bass lives under drums and inst). AZMOD_LANE_ALPHA (1-255)
     * overrides it; 255 is opaque. Parsed once: this runs per draw. */
    static int alpha = -1;
    if (alpha < 0) {
        alpha = 150;
        const char *e = getenv("AZMOD_LANE_ALPHA");
        if (e) {
            int v = atoi(e);
            if (v > 0 && v <= 255)
                alpha = v;
        }
    }
    for (int o = 0; o < AZMOD_STEM_COUNT; o++) {
        int i = ORDER[o];
        if (!s->stem_on[deck][i])
            continue; /* muted: not drawn at all */
        for (int px = 0; px < bw; px++) {
            /* Column -> track position: playhead live from azstem, scale from
             * the last full redraw. */
            double col = (double) px * ncol / bw;
            double t = pos + (col - phcol) * ppc / tot;
            double t2 = pos + (col + 1.0 - phcol) * ppc / tot;
            if (t2 <= 0.0 || t >= 1.0)
                continue; /* before/after the track */
            /* Peak over the column's span, not a single sample (like EP147's own
             * range accessor): nearest-sampling thousands of buckets into a few
             * hundred pixels drops most transients. */
            int b0 = (int) (t * nb), b1 = (int) (t2 * nb);
            if (b0 < 0)
                b0 = 0;
            if (b1 > nb)
                b1 = nb;
            if (b1 <= b0)
                b1 = b0 + 1;
            const unsigned char *lane = env + (size_t) i * nb;
            int m = 0;
            for (int q = b0; q < b1; q++)
                if (lane[q] > m)
                    m = lane[q];
            int a = m * half / 255;
            if (a < 1)
                continue; /* silence adds nothing */
            blend_rect(fb, bx + px, mid - a, 1, a * 2, SC[i], alpha);
        }
    }
}

/* Draw one deck's 4 stem buttons as rounded rects sitting on the native pad
 * slots. VOCAL green, INST red, BASS magenta, DRUMS blue (same palette as the
 * pad LEDs, see azmod.c build_pad_led). Static per stem mask -> region-cached;
 * only the row-2 lanes (live playhead) redraw per tick. Callers gate on
 * stems_available (azmod_composite); a no-stem deck draws nothing here. */
static region_cache g_grid_rc[AZMOD_STEM_DECKS];
static void grid_static(const azmod_state *s, const azmod_fb *gfb, const azmod_fb *dfb, int deck,
                        int ox, int oy) {
    static const char *SL[AZMOD_STEM_COUNT] = {"VOCAL", "INST", "BASS", "DRUMS"};
    static const uint32_t SC[AZMOD_STEM_COUNT] = AZMOD_STEM_PALETTE;
    /* Title band. Our window spans it, so repaint the native rule ourselves,
     * then plate over the "BEAT LOOP" caption and write "STEMS" in its place. */
    int wx, wy, ww, wh;
    azmod_stem_win_rect(gfb, deck, &wx, &wy, &ww, &wh);
    (void) wy;
    (void) wh;
    int tx, ty, tw, th;
    azmod_stem_title_rect(gfb, deck, &tx, &ty, &tw, &th);
    fill_rect(dfb, wx - ox, (int) (STEM_RULE_Y_FRAC * gfb->h) - oy, ww, 1, C_STEM_RULE);
    fill_rect(dfb, tx - ox, ty - oy, tw, th, 0x00000000u);
    /* Sized a hair over the native caption's 9px caps and synthetically bolded
     * (the rootfs only ships the DemiBold Latin face), +1px tracking to match
     * its letter-spacing. Measured against a real "BEAT LOOP". */
    if (!azmod_ttf_center || !azmod_ttf_center(dfb, tx - ox, ty - oy, tw, th, "STEMS",
                                               (th * 11) / 14, 1, 1, C_STEM_TITLE_FG))
        draw_text(dfb, tx + (tw - 5 * 6) / 2 - ox, ty + (th - 7) / 2 - oy, "STEMS", 1,
                  C_STEM_TITLE_FG);

    /* Row 1: the four stem toggles. (No-stem decks never reach this:
     * azmod_composite skips them, and the native Beat Loop grid owns the
     * pad area instead.) */
    for (int i = 0; i < AZMOD_STEM_COUNT; i++) {
        int x, y, w, h;
        azmod_stem_btn_rect(gfb, deck, i, &x, &y, &w, &h);
        bool on = s->stem_on[deck][i];
        int r = h / 6;
        if (r < 2)
            r = 2; /* rounded rect, not a pill */
        int t = h / 10;
        if (t < 1)
            t = 1;
        uint32_t edge = on ? SC[i] : C_STEM_OFF_FG;
        round_rect(dfb, x - ox, y - oy, w, h, r, t, on ? C_STEM_ON_BG : C_STEM_OFF_BG, edge);
        uint32_t tc = edge; /* label always matches the border */
        /* Prefer EP147's real UI typeface so the pills read as native chrome;
         * the 5x7 bitmap font is the fallback when the TTF isn't there. */
        if (!azmod_ttf_center ||
            !azmod_ttf_center(dfb, x - ox, y - oy, w, h, SL[i], (h * 11) / 20, 0, 0, tc)) {
            int bscale = h / 10;
            if (bscale < 1)
                bscale = 1;
            int tw2 = (int) strlen(SL[i]) * 6 * bscale; /* center variable-len label */
            draw_text(dfb, x + (w - tw2) / 2 - ox, y + (h - 7 * bscale) / 2 - oy, SL[i], bscale,
                      tc);
        }
    }
}
/* Row 2 only, inside the native tiles 5-8. Same envelope, peak cache and muted
 * graying as draw_stem_grid, without the toggle pills: the native tiles 1-4 are
 * the toggles. */
static void draw_stem_lanes_native(azmod_state *s, const azmod_fb *fb, int deck) {
    static const uint32_t SC[AZMOD_STEM_COUNT] = AZMOD_STEM_PALETTE;
    const unsigned char *env = 0;
    int nb = 0, nst = 0;
    float pos = 0.f;
    int have = azmod_stem_wave && azmod_stem_wave(deck, &env, &nb, &nst, &pos) &&
               nst >= AZMOD_STEM_COUNT && nb > 0;
    /* The have-latch must update on both edges before any early return (as in
     * draw_stem_grid): a return while have == 0 would leave it stuck at 1, so
     * later 0->1 edges, and with them the peak-cache rebuild, would never fire. */
    if (have && !g_lane_was_have[deck])
        g_lane_gen[deck]++; /* envelope content changed */
    g_lane_was_have[deck] = have;
    if (!have)
        return;
    /* The progressive envelope's tail is azstem's all-128 seed, which draws as a
     * flat band. Wait for the final envelope: azstem sets the flag before the
     * generation bump, so the first frame drawn is the complete curve. */
    if (azmod_stem_env_final && !azmod_stem_env_final(deck))
        return; /* no envelope yet: leave the tile blank */
    /* Primary invalidation is azstem's per-deck envelope generation, not an
     * edge: a track loaded while this view is elsewhere swaps the envelope with
     * no draw running, so no latch edge is seen. gen changes on every content
     * swap (cache publish, progressive publish, final build). */
    unsigned gen =
        azmod_stem_env_gen ? (unsigned) azmod_stem_env_gen(deck) : (unsigned) g_lane_gen[deck];
    /* Opt-in diagnostic: a lane row frozen on a stale envelope looks like a
     * working draw. In an AZMOD_PROBES build, one line per second per deck with the
     * invalidation inputs while /home/root/settings/azmod-lanedbg exists (checked at
     * most once a second). The throttle is per deck so one deck cannot use every slot. */
    {
        static long lane_dbg_next[AZMOD_STEM_DECKS];
        long nowt = (long) time(NULL);
        if (nowt >= lane_dbg_next[deck] && AZ_PROBE_ON("/home/root/settings/azmod-lanedbg")) {
            lane_dbg_next[deck] = nowt + 1;
            FILE *lf = azlog_fopen_capped("/tmp/azmod-lane.log", AZLOG_CAP_BYTES);
            if (lf) {
                fprintf(lf, "d%d have=%d gen=%u pk_gen=%d pk_w=%d envgen=%p\n", deck, have, gen,
                        g_lane_pk_gen[deck], g_lane_pk_w[deck], (void *) azmod_stem_env_gen);
                fclose(lf);
            }
        }
    }
    for (int i = 0; i < AZMOD_STEM_COUNT; i++) {
        int x, y, w, h;
        azmod_stem_lane_rect(fb, deck, i, &x, &y, &w, &h);
        /* No inset: the amplitude comes from the height (half = (h - 2) / 2) and
         * the row is only ~19 px, so an inset would flatten dense stems into bars.
         *
         * Fill first: tiles 5-8 still carry EP147's VOCAL ECHO / INST ECHO /
         * INST BRAKER / DRUMS ECHO art, because EP147 re-asserts that index on
         * every refresh (see stem_tile_fix_one). Filling covers the label with no
         * race to fight. */
        if (w <= 0 || h <= 0)
            continue;
        fill_rect(fb, x, y, w, h, C_STEM_SLOT);
        if (g_lane_pk_w[deck] != w || g_lane_pk_gen[deck] != (int) gen)
            lane_peaks_build(deck, env, nb, w, gen);
        const unsigned char *pk =
            (g_lane_pk_w[deck] == w && g_lane_pk_gen[deck] == (int) gen) ? g_lane_pk[deck][i] : 0;
        draw_stem_lane(fb, x, y, w, h, pk, env + (size_t) i * nb, nb, pos,
                       s->stem_on[deck][i] ? SC[i] : C_STEM_LANE_OFF);
    }
}
static void draw_stem_grid(azmod_state *s, const azmod_fb *fb, int deck) {
    static const uint32_t SC[AZMOD_STEM_COUNT] = AZMOD_STEM_PALETTE;
    /* Static part (title band + row-1 buttons) via the region cache: sig is
     * the stem mask, so it only re-rasterizes on a real toggle. */
    int wx, wy, ww, wh;
    azmod_stem_win_rect(fb, deck, &wx, &wy, &ww, &wh);
    (void) wy;
    (void) wh;
    int tx, ty, tw, th;
    azmod_stem_title_rect(fb, deck, &tx, &ty, &tw, &th);
    (void) tx;
    (void) tw;
    (void) th;
    int sx, sy, sw, sh;
    azmod_stem_strip_rect(fb, deck, &sx, &sy, &sw, &sh);
    unsigned mask = 0;
    for (int i = 0; i < AZMOD_STEM_COUNT; i++)
        mask |= (unsigned) (s->stem_on[deck][i] ? 1 : 0) << i;
    region_cache *c = &g_grid_rc[deck];
    int rc = rc_prepare(c, wx, ty, ww, (sy + sh) - ty, mask << 1);
    if (rc > 0) {
        /* the ground goes through azm_theme (black, or the light theme's ground), so
         * a raw memset is only right when it is black */
        uint32_t ground = azm_theme(0);
        if (ground) {
            for (size_t k = 0; k < (size_t) c->w * (size_t) c->h; k++)
                c->px[k] = ground;
        } else
            memset(c->px, 0, (size_t) c->w * (size_t) c->h * 4);
        azmod_fb cfb = {.px = (uint8_t *) c->px, .w = c->w, .h = c->h, .stride = c->w * 4};
        grid_static(s, fb, &cfb, deck, wx, ty);
    }
    if (rc >= 0)
        rc_blit(c, fb);
    else
        grid_static(s, fb, fb, deck, 0, 0); /* no cache buffer: draw direct */
    /* Row 2's ground, drawn explicitly: with Stem Waves off no lane covers it and
     * the overlay window's black background would show on a light theme. */
    fill_rect(fb, sx, (int) (STEM_ROW2_Y_FRAC * fb->h), sw, sh,
              0x00000000u); /* row 2, not the strip (row 1) */

    /* Row 2 = each stem's own mini-waveform, directly under its toggle: muting
     * a stem grays out its waveform. An empty slot until azstem has decoded this
     * deck's stems. */
    const unsigned char *env = 0;
    int nb = 0, nst = 0;
    float pos = 0.f;
    int have = azmod_stem_wave && azmod_stem_wave(deck, &env, &nb, &nst, &pos) &&
               nst >= AZMOD_STEM_COUNT && nb > 0;
    if (have && !g_lane_was_have[deck])
        g_lane_gen[deck]++; /* envelope content changed */
    g_lane_was_have[deck] = have;
    unsigned gen =
        azmod_stem_env_gen ? (unsigned) azmod_stem_env_gen(deck) : (unsigned) g_lane_gen[deck];
    for (int i = 0; i < AZMOD_STEM_COUNT; i++) {
        int x, y, w, h;
        azmod_stem_lane_rect(fb, deck, i, &x, &y, &w, &h);
        fill_rect(fb, x, y, w, h, C_STEM_SLOT);
        if (!have)
            continue;
        if (g_lane_pk_w[deck] != w || g_lane_pk_gen[deck] != (int) gen)
            lane_peaks_build(deck, env, nb, w, gen); /* same width for every lane */
        const unsigned char *pk =
            (g_lane_pk_w[deck] == w && g_lane_pk_gen[deck] == (int) gen) ? g_lane_pk[deck][i] : 0;
        draw_stem_lane(fb, x, y, w, h, pk, env + (size_t) i * nb, nb, pos,
                       s->stem_on[deck][i] ? SC[i] : C_STEM_LANE_OFF);
    }
}

bool azmod_native_lanes_drawn(const azmod_state *s, int d) {
    if (d < 0 || d >= AZMOD_STEM_DECKS)
        return false;
    if (s->mode != AZ_CLOSED)
        return false;
    /* Stem FX = FX: tiles 5-8 are Stem FX pads and keep EP147's own FX art. */
    return s->waveform_visible && azmod_stems_engine(s) && azmod_stem_native && !s->four_deck &&
           s->stems_available[d] && s->pad_beatloop[d] && az_get(AZF_STEM_FX) != 1;
}
