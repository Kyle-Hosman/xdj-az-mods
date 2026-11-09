/* SPDX-License-Identifier: GPL-3.0-only */
/* day_mode.c: Day mode and color themes for EP147. #included by pad_leds.c (azmod.c's unit).
 *
 * Themes are applied at the native layer (EP147 itself paints with the theme's
 * colors), never as a framebuffer inversion or tint. Switches live, with no EP147
 * restart: every hook reads g_dm_on.
 *
 * Where EP147's colors come from, and the lever for each:
 *   - every solid fill and text color: the software renderer's setFill (vtable
 *     slot), handed a remapped copy;
 *   - decoded PNG art: recorded at decode, recolored in place lazily, reversibly;
 *   - small text images it renders itself: drawn through JUCE's alpha fill in ink;
 *   - main waveforms: the rasterizer's per-column background color (a copy);
 *   - beat rulers: their background field, swapped during makeGridImage only;
 *   - overviews: additive pixels made opaque for each draw, then put back.
 * The map (dm_map) flips the lightness of grays only and leaves saturated
 * colors (blue selection, red/orange state, waveform colors) alone.
 *
 * Hooks are installed at every start so the switch is live; each passes straight
 * through while g_dm_on is 0. Kill switch: settings/azmod-themes.off (azmod.c
 * daymode_wanted) installs nothing.
 */
#ifdef AZMOD_HAVE_DAYMODE
#include <X11/Xatom.h> /* XA_CARDINAL: finding EP147's window by _NET_WM_PID */

/* ---- EP147 addresses (v1.30, md5 3518275e), kept in one place. --------------- */
#define DM_GFX_SETCOLOUR \
    EP_23af140 /* juce::Graphics::setColour(Graphics*, const Colour*): called                      \
                    directly (under the bypass) for our own fills */
/* The color lever: LowLevelGraphicsSoftwareRenderer::setFill (EP147's only renderer).
 * Every solid fill reaches it: Graphics::setColour, fillAll(Colour), and
 * Image::clear, which talks to the context directly and so bypasses any
 * Graphics-level hook. vtable (RTTI
 * N4juce32LowLevelGraphicsSoftwareRendererE): slot 18 (+0x90) = 0x23f1370, the
 * same slot order drawImageTransformed uses for +0x50/+0x60/+0x68/+0x70/+0x78/+0xa8.
 * juce::FillType (0x30 B, from setFill's copy): +0x00 Colour, +0x08 gradient*,
 * +0x10 image (ImagePixelData*), +0x18 AffineTransform. */
#define DM_SWR_VTABLE EP_3142bc0
#define DM_SWR_SETFILL_OFF 0x90
#define DM_SWR_SETFILL EP_23f1370 /* expected slot value: checked before patching */
#define DM_FILLTYPE_SIZE 0x30
/* Text ink: colors used as text get a darker shade of the same hue at >= 4.5:1 on
 * white and on the light panel gray; fills stay stock. A glyph is drawn by the renderer's drawGlyph(int, AffineTransform&)
 * (slot 29 +0xe8; slots 27/28 are setFont/getFont, 30 drawTextLayout), which
 * paints with the context state's current fill: SoftwareRendererSavedState* at
 * ctx+8, its FillType at +0x30 (color), +0x38 gradient*, +0x40 image (read
 * from setFill's copy). The hook swaps that color for the glyph and puts it back,
 * read live rather than tracked so saveState/restoreState cannot desync it. */
#define DM_SWR_DRAWGLYPH_OFF 0xe8
#define DM_SWR_DRAWGLYPH EP_23fde20 /* expected slot value: checked before patching */
#define DM_STATE_FILL 0x30
/* Gray text on a colored fill (hot cue chip letters, the MASTER / +-10 tags, the
 * blue cursor row): the gray flip would turn black letters white (1.37:1 on the
 * green chip). The rectangle-fill hooks record, per thread, recent opaque fills
 * (color + bounds); a glyph whose origin lies inside a colored one is drawn black
 * when black reads at >= 4.5:1 on that fill (see dm_on_fill).
 * fillRect(Rectangle<int>&, bool) slot
 * 21 (+0xa8), fillRect(Rectangle<float>&) slot 22 (+0xb0), rect at x1. */
#define DM_SWR_FILLRECT_I_OFF 0xa8
#define DM_SWR_FILLRECT_I EP_2402ff0
#define DM_SWR_FILLRECT_F_OFF 0xb0
#define DM_SWR_FILLRECT_F EP_2401dd0
#define DM_SWR_FILLPATH_OFF 0xc0
#define DM_SWR_FILLPATH EP_23fe810
#define DM_FT_GRADIENT 0x08
#define DM_FT_IMAGE 0x10
/* JUCE component image caches (Component::setBufferedToImage) blit the pixels they
 * rendered last time and an X repaint does not invalidate them, so each must be
 * invalidated after a switch or it keeps the old palette. vtable (RTTI
 * N4juce28StandardCachedComponentImageE): slot 2 (+0x10) paint(Graphics&),
 * slot 3 (+0x18) invalidateAll() (clears validArea: everything re-renders). */
#define DM_SCCI_INVALIDATE_SLOT 3 /* (the paint slot itself is hooked once, by track_list_rows.c) */

#define DM_GFX_DRAWIMAGE_T \
    EP_23b04e0 /* juce::Graphics::drawImageTransformed(Graphics*, const Image*,                    \
                    const AffineTransform*, bool fillAlphaChannelWithCurrentBrush):                \
                    every image draw lands here (drawImage -> getClippedImage -> here).            \
                    fill=true: saveState, clipToImageAlpha, fillRect(clip), restore */
/* juce::ImagePixelData (read from getClippedImage 0x23de100): +0x0c pixelFormat
 * (1 RGB, 2 ARGB, 3 SingleChannel), +0x10 width, +0x14 height. SubsectionPixelData
 * (getClippedImage's result): +0x48 source ImagePixelData*, +0x50 area {x,y,w,h}. */
#define DM_IPD_FORMAT 0x0c
#define DM_IPD_W 0x10
#define DM_IPD_H 0x14
/* SoftwarePixelData (vtable below):
 * +0x48 pixel buffer, +0x50 pixelStride (4 = ARGB, premultiplied BGRA bytes),
 * +0x54 lineStride. */
#define DM_SPD_VTABLE EP_3142d48
#define DM_SPD_DATA 0x48
#define DM_SPD_PSTR 0x50
#define DM_SPD_LSTR 0x54
/* The main waveform rasterizer: the only writer of the 1220x132 RGB image
 * (store at 0x1dfef08). (this, const Column *cols, int width, int height) ->
 * juce::Image via x8. Columns: a fixed 1220 x 28 B array; per column three 4-byte
 * cells {B,G,R,height}: inner color, outer color, then the background color at +8,
 * painted beyond the waveform's height. That background is data, not a fill, so no
 * fill hook sees it. Prologue mov/sub/stp/mov: hookable. */
#define DM_WF_RASTER EP_1dfed18
#define DM_WF_COLS 1220
#define DM_WF_COL_SIZE 28
#define DM_WF_COL_BG 8
/* The 3BAND rasterizer (store at 0x1dff6fc). Same signature and column array, but
 * layered cells: the background is drawn from more than one cell (see
 * dm_wf3_columns). Prologue mov/sub/stp/mov: hookable. BLUE renders through the RGB
 * rasterizer above. */
#define DM_WF3_RASTER EP_1dff558
/* The beat rulers above and below each waveform (1220x15 ARGB, only writer
 * 0x1dfffd8). (this, x1, w2, w3, w4, w5, float s0) -> x0.
 * It fills row 0 with the background color at this+0xc (ARGB u32), stamps the
 * ticks, then copies row 0 down. prologue stp/mov/stp/mov: hookable. */
/* (the function itself is waveform.c's GRID_MAKE, 0x1dffe10) */
#define DM_RULER_BG 0x0c
/* EP147 also loads ~764 UI images from files (/home/root/pdj/DynamicLoad/: Browse,
 * Waveform/PlayerInfo, Utility, Popup, ...) through this loader, not getFromMemory.
 * (File) -> Image via x8, 520 call sites; prologue stp/mov/stp/adrp: hookable. */
#define DM_IMAGECACHE_GETFROMFILE EP_23ee2a0
#define DM_IMAGECACHE_GETFROMMEMORY \
    EP_23ed7c0 /* juce::ImageCache::getFromMemory(const void*, int)                                \
                   -> Image via x8; every embedded PNG is decoded here.                            \
                   prologue stp/mov/stp/adrp: hookable (adrp relocated) */

/* Live switch: the hooks stay installed and read g_dm_on on every call; the
 * watcher thread below flips it from this file and makes EP147 repaint. The Mod
 * Menu toggle drives the same file. */
#define DAYMODE_ON_PATH "/home/root/settings/azmod-daymode.on"
#define DAYMODE_OFF_PATH "/tmp/azmod-daymode.off" /* dev only: overrides both, for A/B timing */

static _Atomic long g_dm_calls;

/* The lightness map for grays. A straight flip (255 - L) would make every black
 * ground pure white. Instead dark surfaces (L <= DM_SURF: ground, panels, tiles)
 * map into a light band that keeps stock's depth order: the black ground becomes
 * DM_GROUND and anything raised above it gets lighter, up to white, so the screen
 * reads as light-gray ground with white cards. Everything brighter (text, lines,
 * icons) is flipped, so white text becomes dark. */
#define DM_GROUND 0xdc /* stock pad tile gray #232323, flipped */
#define DM_SHADOW_PCT 30 /* Day: a textured button's drop shadow, share of its stock alpha */
#define DM_SURF 0x40   /* the lightest stock surface gray; text and lines sit well above it */
#define DM_HILITE 0x78 /* stock highlights (selected rows #64646e, pressed keys) up to here */
#define DM_HI_TOP 0xd8 /* ...map to a light gray that black text reads on: 0x41 -> 0xd7 */
/* Three bands with disjoint outputs, so a mapped gray can always be traced back to
 * its stock lightness (dm_unL): surfaces -> [0xdc, 0xff], highlights -> [0xa0, 0xd7],
 * ink -> [0x00, 0x73]. The glyph hook needs that: setFill maps every color before
 * EP147 knows whether it will fill a box or draw text with it. */
static int dm_L(int L) {
    if (L <= DM_SURF)
        return DM_GROUND + (L * (255 - DM_GROUND) + DM_SURF / 2) / DM_SURF;
    if (L <= DM_HILITE)
        return DM_HI_TOP - (L - DM_SURF);
    return (255 - L) * DM_GROUND /
           255; /* ink: scaled so it keeps its contrast on the gray ground */
}
/* stock lightness of a mapped gray; ink stays ink (returns -1) */
static int dm_unL(int v) {
    if (v >= DM_GROUND)
        return ((v - DM_GROUND) * DM_SURF + (255 - DM_GROUND) / 2) / (255 - DM_GROUND);
    if (v >= DM_HI_TOP - (DM_HILITE - DM_SURF) && v < DM_HI_TOP)
        return DM_HI_TOP - v + DM_SURF;
    return -1;
}
/* text ink for a stock gray L: flipped, scaled for the ground, and never lighter than
 * a mid gray that still reads (dim stock labels sit barely above their background) */
static int dm_text_L(int L) {
    int v = (255 - L) * DM_GROUND / 255;
    return v > 0x60 ? 0x60 : v;
} /* 0x60: 4.4:1 on the ground */

/* ---- Themes: the same hooks, a different map. A theme either flips lightness
 * (Day: light ground, dark ink, everything above) or keeps stock's lightness and
 * tints it: dark themes leave every contrast EP147 designed intact, so none of
 * Day's ink machinery runs for them. Grays map along a ramp
 * ground (L=0) -> surface (L=DM_SURF) -> text (L=255); an optional accent swap moves
 * EP147's selection blues (hue 195..220, saturated, mid lightness) to the theme's hue,
 * lightness kept. Deck and waveform colors are outside that band and keep their meaning.
 * Chosen by g_azmod_theme (menu.h; azmod.conf's "theme"; absent = day). */
static double dm_hue2rgb(double p, double q, double t);
static double dm_lum(double r, double g, double b);
struct dm_theme {
    const char *name;
    int flip;
    uint32_t ground, surface, text;
    int accent_hue;
};
static const struct dm_theme g_dm_themes[] = {
    {"day", 1, 0, 0, 0, -1},
    {"azmods", 0, 0x000000, 0x2e2a27, 0xf4efe9, 28},   /* black, warm charcoal, orange */
    {"midnight", 0, 0x050b18, 0x16233f, 0xe6eeff, -1}, /* deep navy, slate blue, ice white */
    {"ember", 0, 0x0d0605, 0x2f1a16, 0xfbeee8, 8},     /* near-black red, oxblood, coral */
    {"forest", 0, 0x040d08, 0x15291d, 0xe8f5ec, 150},  /* pine black, moss, mint */
};
#define DM_NTHEMES ((int) (sizeof g_dm_themes / sizeof g_dm_themes[0]))
static _Atomic int g_dm_theme; /* index into g_dm_themes, read at switch-on */
static inline const struct dm_theme *dm_th(void) {
    return &g_dm_themes[__atomic_load_n(&g_dm_theme, __ATOMIC_RELAXED)];
}
static inline int dm_flip_on(void) {
    return dm_th()->flip;
}
static uint32_t dm_lerp3(uint32_t a, uint32_t b, int t, int n) {
    uint32_t o = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int ca = (a >> sh) & 0xff, cb = (b >> sh) & 0xff;
        o |= (uint32_t) (ca + (cb - ca) * t / n) << sh;
    }
    return o;
}
/* a stock gray of lightness L under the current theme, as 0xRRGGBB */
static uint32_t dm_gray(int L) {
    const struct dm_theme *t = dm_th();
    if (t->flip) {
        uint32_t v = (uint32_t) dm_L(L);
        return (v << 16) | (v << 8) | v;
    }
    if (L <= DM_SURF)
        return dm_lerp3(t->ground, t->surface, L, DM_SURF);
    return dm_lerp3(t->surface, t->text, L - DM_SURF, 255 - DM_SURF);
}
/* EP147's selection blues -> the theme's accent hue (HSL, lightness + saturation kept) */
static uint32_t dm_accent(uint32_t argb) {
    int hue = dm_th()->accent_hue;
    if (hue < 0)
        return argb;
    double r = ((argb >> 16) & 0xff) / 255.0, g = ((argb >> 8) & 0xff) / 255.0,
           b = (argb & 0xff) / 255.0;
    double mx = fmax(r, fmax(g, b)), mn = fmin(r, fmin(g, b)), l = (mx + mn) / 2, d = mx - mn;
    if (d < 0.25)
        return argb;
    double sat = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
    double h = mx == r ? fmod((g - b) / d + 6, 6) : mx == g ? (b - r) / d + 2
                                                            : (r - g) / d + 4;
    h *= 60;
    if (h < 195 || h > 222 || sat < 0.6 || l < 0.12 || l > 0.68)
        return argb;
    if ((argb & 0xffffff) == 0x28a0ff)
        return argb; /* the Phase Meter's deck-blue ruler: a deck color, not an accent */
    /* Match the stock blue's luminance, not its HSL lightness, so every text color
     * EP147 draws on it (white labels, the loaded track's green title) keeps stock's
     * contrast on the accent. Bisect the HSL lightness to the target. */
    double target = dm_lum(r * 255, g * 255, b * 255), lo = 0, hi = 1, hh = hue / 360.0, rr = 0,
           gg = 0, bb = 0;
    for (int it = 0; it < 18; it++) {
        double lm = (lo + hi) / 2, q = lm < 0.5 ? lm * (1 + sat) : lm + sat - lm * sat,
               pp = 2 * lm - q;
        rr = dm_hue2rgb(pp, q, hh + 1.0 / 3);
        gg = dm_hue2rgb(pp, q, hh);
        bb = dm_hue2rgb(pp, q, hh - 1.0 / 3);
        if (dm_lum(rr * 255, gg * 255, bb * 255) < target)
            lo = lm;
        else
            hi = lm;
    }
    return (argb & 0xff000000u) | ((uint32_t) (rr * 255 + 0.5) << 16) |
           ((uint32_t) (gg * 255 + 0.5) << 8) | (uint32_t) (bb * 255 + 0.5);
}

/* juce::Colour is a packed ARGB u32. Grays (channels within 12 of each other)
 * get the lightness map; everything else passes through untouched. The gray's
 * small channel spread is kept by moving all three channels by the same amount. */
static uint32_t dm_map_compute(uint32_t argb) {
    uint32_t a = argb >> 24, r = (argb >> 16) & 0xff, g = (argb >> 8) & 0xff, b = argb & 0xff;
    uint32_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    uint32_t mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (mx - mn > 12)
        return dm_accent(argb); /* 12, not 8: the selected row is #64646e */
    if (!dm_flip_on())
        return (a << 24) | dm_gray((int) g);
    int d = dm_L((int) g) - (int) g;
    int R = (int) r + d, G = (int) g + d, B = (int) b + d;
    R = R < 0 ? 0 : R > 255 ? 255
                            : R;
    G = G < 0 ? 0 : G > 255 ? 255
                            : G;
    B = B < 0 ? 0 : B > 255 ? 255
                            : B;
    return (a << 24) | ((uint32_t) R << 16) | ((uint32_t) G << 8) | (uint32_t) B;
}

/* Memoized: menu.c and text.c theme every pixel they draw, and an accent color costs
 * an 18-step bisection of 3 pow() each (a Mod Menu frame: 3.7 ms -> 32.5 ms uncached).
 * A UI uses a few dozen colors, so a small per-thread direct-mapped cache keyed by
 * (color, theme) answers almost every call. EP147's setFill path shares it. */
static uint32_t dm_map(uint32_t argb) {
    static __thread struct {
        uint32_t in, out;
        int th;
    } cache[256];
    int th = __atomic_load_n(&g_dm_theme, __ATOMIC_RELAXED);
    unsigned k = ((argb * 2654435761u) >> 24) & 255;
    if (cache[k].in == argb && cache[k].th == th + 1)
        return cache[k].out;
    uint32_t o = dm_map_compute(argb);
    cache[k].in = argb;
    cache[k].out = o;
    cache[k].th = th + 1;
    return o;
}

/* grays only, never the accent swap: waveform color cells keep their meaning */
static uint32_t dm_map_gray(uint32_t argb) {
    uint32_t r = (argb >> 16) & 0xff, g = (argb >> 8) & 0xff, b = argb & 0xff;
    uint32_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b),
             mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    return mx - mn > 12 ? argb : dm_map(argb);
}

/* Our own fills (the dark wells, the dark ink) must not be remapped: they set
 * this per thread around their Graphics::setColour. */
static __thread int t_dm_bypass;

/* the last plain color EP147 set on this thread, before the map (dm_fillpath_hook) */
static __thread uint32_t t_dm_stock;
static void (*g_dm_orig_setfill)(void *ctx, const void *fill);
static void dm_setfill_hook(void *ctx, const void *fill) {
    __atomic_add_fetch(&g_dm_calls, 1, __ATOMIC_RELAXED);
    const unsigned char *f = fill;
    if (!__atomic_load_n(&g_dm_on, __ATOMIC_RELAXED) || t_dm_bypass ||
        *(void *const *) (f + DM_FT_GRADIENT) || *(void *const *) (f + DM_FT_IMAGE)) {
        g_dm_orig_setfill(ctx, fill); /* off, our own fill, or a gradient/image fill */
        return;
    }
    /* A plain color: hand EP147 a remapped copy, never touch the caller's object.
     * With no gradient and no image there is nothing ref-counted inside, so a byte
     * copy is a valid FillType. */
    unsigned char tmp[DM_FILLTYPE_SIZE] __attribute__((aligned(16)));
    memcpy(tmp, f, DM_FILLTYPE_SIZE);
    t_dm_stock = *(const uint32_t *) f;
    *(uint32_t *) tmp = dm_map(t_dm_stock);
    g_dm_orig_setfill(ctx, tmp);
}

/* Each cache re-renders once per switch: a side table remembers the switch
 * generation each cache last painted in (paint thread only; a stale entry for a
 * freed cache costs one extra invalidate if the address is reused, never more). */
static _Atomic unsigned g_dm_gen;
#define DM_CACHE_N 1024
static struct {
    void *c;
    unsigned gen;
} g_dm_cache[DM_CACHE_N];
/* Called first thing in azmod's single StandardCachedComponentImage::paint hook
 * (track_list_rows.c scci_paint_hook, shared with the list row cache). Do not patch the slot
 * here too: the second installer would fail its expected-value check. Paint thread
 * only. */
static void dm_scci_pre(void *self) {
    unsigned gen = __atomic_load_n(&g_dm_gen, __ATOMIC_RELAXED);
    if (!gen)
        return; /* never switched: stock */
    unsigned i = (unsigned) (((uintptr_t) self >> 4) * 2654435761u) & (DM_CACHE_N - 1);
    for (int n = 0; n < 8; n++, i = (i + 1) & (DM_CACHE_N - 1)) {
        if (g_dm_cache[i].c == self || !g_dm_cache[i].c || n == 7) {
            if (g_dm_cache[i].c != self || g_dm_cache[i].gen != gen) {
                void (**vt)(void *) = *(void (***)(void *)) self;
                vt[DM_SCCI_INVALIDATE_SLOT](self); /* its own invalidateAll */
                g_dm_cache[i].c = self;
                g_dm_cache[i].gen = gen;
            }
            return;
        }
    }
}

/* The ink shade of a color (ARGB): grays pass through (the map already flips them);
 * a color already at >= 4.5:1 on both grounds passes through; otherwise its HLS
 * lightness is stepped down until it passes on both. Cached: a UI uses a handful of
 * colors. */
static double dm_lin(double c) {
    c /= 255.0;
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
static double dm_lum(double r, double g, double b) {
    return 0.2126 * dm_lin(r) + 0.7152 * dm_lin(g) + 0.0722 * dm_lin(b);
}
static double dm_contrast(double l1, double l2) {
    return l1 > l2 ? (l1 + 0.05) / (l2 + 0.05) : (l2 + 0.05) / (l1 + 0.05);
}
/* Text on a colored fill (fills keep their stock color): black when black reads at
 * 4.5:1 (the blue cursor row), else white, the stock pairing that dark fills such as
 * the navy USB 1 source tile need. under == 0 means no fill. */
static double dm_lum_u32(uint32_t c);
static uint32_t dm_on_fill(uint32_t under, uint32_t mapped, uint32_t ink) {
    if (!under)
        return ink;
    double lu = dm_lum_u32(under);
    if (dm_contrast(0.0, lu) >= 4.5)
        return mapped & 0xff000000u;
    /* White only to restore stock white text (a gray mapped into the ink band from a
     * stock lightness >= 0xc0), and only where white reads: a dark fill can be recorded
     * under text actually drawn over something else (the bottom deck strips). */
    uint32_t r = (mapped >> 16) & 0xff, g = (mapped >> 8) & 0xff, b = mapped & 0xff;
    uint32_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b),
             mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int stockL = 255 - (int) g * 255 / DM_GROUND;
    if (mx - mn <= 12 && g <= 0x73 && stockL >= 0xc0 && dm_contrast(1.0, lu) >= 4.5)
        return (mapped & 0xff000000u) | 0x00ffffffu;
    return ink;
}
static double dm_hue2rgb(double p, double q, double t) {
    if (t < 0)
        t += 1;
    if (t > 1)
        t -= 1;
    if (t < 1.0 / 6)
        return p + (q - p) * 6 * t;
    if (t < 0.5)
        return q;
    if (t < 2.0 / 3)
        return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}
static uint32_t dm_ink_compute(uint32_t argb) {
    double r = (argb >> 16) & 0xff, g = (argb >> 8) & 0xff, b = argb & 0xff;
    const double LW = 1.0, LP = dm_lum(0xd8, 0xd8, 0xd8),
                 T = 4.55; /* panel grays measure #d8-#dc: the darkest, + a hair over 4.5 */
    if (dm_contrast(dm_lum(r, g, b), LW) >= T && dm_contrast(dm_lum(r, g, b), LP) >= T)
        return argb;
    double rr = r / 255, gg = g / 255, bb = b / 255;
    double mx = fmax(rr, fmax(gg, bb)), mn = fmin(rr, fmin(gg, bb)), h = 0, sat = 0,
           l = (mx + mn) / 2;
    if (mx > mn) {
        double dd = mx - mn;
        sat = l > 0.5 ? dd / (2 - mx - mn) : dd / (mx + mn);
        h = mx == rr   ? (gg - bb) / dd + (gg < bb ? 6 : 0)
            : mx == gg ? (bb - rr) / dd + 2
                       : (rr - gg) / dd + 4;
        h /= 6;
    }
    for (; l > 0; l -= 0.005) {
        double q = l < 0.5 ? l * (1 + sat) : l + sat - l * sat, pp = 2 * l - q;
        double R = round(dm_hue2rgb(pp, q, h + 1.0 / 3) * 255),
               G = round(dm_hue2rgb(pp, q, h) * 255),
               Bv = round(dm_hue2rgb(pp, q, h - 1.0 / 3) * 255);
        double L = dm_lum(R, G, Bv);
        if (dm_contrast(L, LW) >= T && dm_contrast(L, LP) >= T)
            return (argb & 0xff000000u) | ((uint32_t) R << 16) | ((uint32_t) G << 8) |
                   (uint32_t) Bv;
    }
    return argb & 0xff000000u;
}
static uint32_t dm_ink(uint32_t argb) {
    uint32_t r = (argb >> 16) & 0xff, g = (argb >> 8) & 0xff, b = argb & 0xff;
    uint32_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b),
             mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (mx - mn <= 8)
        return argb; /* gray: the map already flipped it */
    static __thread struct {
        uint32_t in, out;
    } cache[64];
    unsigned k = ((argb * 2654435761u) >> 26) & 63;
    if (cache[k].in == argb && argb)
        return cache[k].out;
    uint32_t o = dm_ink_compute(argb);
    cache[k].in = argb;
    cache[k].out = o;
    return o;
}
uint32_t azmod_dm_ink(uint32_t c) {
    return __atomic_load_n(&g_dm_on, __ATOMIC_RELAXED) && dm_flip_on() ? dm_ink(c) : c;
}
/* azmod's own UI (menu_theme.h azm_theme): the same map as EP147's fills */
uint32_t azmod_dm_map(uint32_t c) {
    return __atomic_load_n(&g_dm_on, __ATOMIC_RELAXED) ? dm_map(c) : c;
}
/* The active palette as a cache key: 0 = Themes off, else 1 + the theme (menu_theme.h azm_theme_key). */
int azmod_dm_key(void) {
    return __atomic_load_n(&g_dm_on, __ATOMIC_RELAXED)
               ? 1 + __atomic_load_n(&g_dm_theme, __ATOMIC_RELAXED)
               : 0;
}

static _Atomic int g_dm_gcensus; /* DEV: /tmp/azmod-dm-gcensus logs the next glyph fills */
/* The last 64 opaque fills on this thread, in the renderer's absolute coordinates.
 * fillRect and drawGlyph get component-local coordinates, and every list row and
 * cell is its own component at 0,0, so local coordinates would match a ground from
 * whichever component filled last. Gray fills are kept too (col 0): the topmost fill
 * under a glyph is its ground, so a stale colored fill from an earlier row or frame
 * is covered, not believed. */
#define DM_SATFILL_N 64
/* SoftwareRendererSavedState's TranslationOrTransform (offsets as used by the glyph census): the
 * component offset Point<int> at +0x20/+0x24, isOnlyTranslated at +0x28. */
#define DM_STATE_OFFSET 0x20
#define DM_STATE_ONLYXLATE 0x28
/* local -> absolute; 0 = transformed (scaled/rotated): no answer, the caller keeps stock ink */
static int dm_abs(void *ctx, float *x, float *y) {
    if (!ctx || *(uintptr_t *) ctx != (uintptr_t) DM_SWR_VTABLE)
        return 0; /* the image path's g->context: check it is one */
    unsigned char *st = *(unsigned char **) ((unsigned char *) ctx + 8);
    if (!st || !st[DM_STATE_ONLYXLATE])
        return 0;
    *x += (float) *(int *) (st + DM_STATE_OFFSET);
    *y += (float) *(int *) (st + DM_STATE_OFFSET + 4);
    return 1;
}
static __thread struct {
    void *ctx;
    float x0, y0, x1, y1;
    uint32_t col;
    int valid;
    int light; /* a stock light bar kept light (dm_fillpath_hook): text on it keeps its ink */
    unsigned gen;
} t_dm_satfills[DM_SATFILL_N];
static __thread unsigned t_dm_fill_gen; /* every rectangle fill, gray or not */
static __thread unsigned t_dm_satfill_i;
static void dm_note_fill(void *ctx, float x, float y, float w, float h) {
    unsigned char *st = *(unsigned char **) ((unsigned char *) ctx + 8);
    t_dm_fill_gen++;
    if (!st || *(void **) (st + DM_STATE_FILL + DM_FT_GRADIENT) ||
        *(void **) (st + DM_STATE_FILL + DM_FT_IMAGE))
        return;
    uint32_t c = *(uint32_t *) (st + DM_STATE_FILL);
    uint32_t r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
    uint32_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b),
             mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if ((c >> 24) < 0xc0)
        return; /* translucent: not a ground */
    if (mx - mn <= 8)
        c = 0; /* gray: a ground, not a colored one */
    if (c && __atomic_load_n(&g_dm_gcensus, __ATOMIC_RELAXED) > 0) { /* DEV */
        FILE *f = azlog_open("/tmp/azmod-dm-gcensus.log", "a");
        if (f) {
            fprintf(f, "satfill ctx %p col %08x rect %.0f,%.0f %.0fx%.0f\n", ctx, c, x, y, w, h);
            fclose(f);
        }
    }
    if (w < 6 || h < 6)
        return; /* a dot or a rule: not a ground for text */
    if (!dm_abs(ctx, &x, &y))
        return;
    unsigned i = (t_dm_satfill_i++) % DM_SATFILL_N;
    t_dm_satfills[i].ctx = ctx;
    t_dm_satfills[i].col = c;
    t_dm_satfills[i].valid = 1;
    t_dm_satfills[i].light = 0;
    t_dm_satfills[i].gen = t_dm_fill_gen;
    t_dm_satfills[i].x0 = x;
    t_dm_satfills[i].y0 = y;
    t_dm_satfills[i].x1 = x + w;
    t_dm_satfills[i].y1 = y + h;
}
/* the colored fill containing the point, only if no other fill came after it: text
 * drawn straight onto it, not onto something painted over it later */
static int dm_satfill_is_last(void *ctx, float x, float y) {
    if (!dm_abs(ctx, &x, &y))
        return 0;
    for (unsigned k = 0; k < DM_SATFILL_N; k++) {
        unsigned i = (t_dm_satfill_i - 1 - k) % DM_SATFILL_N;
        if (t_dm_satfills[i].valid && t_dm_satfills[i].ctx == ctx && x >= t_dm_satfills[i].x0 &&
            x < t_dm_satfills[i].x1 && y > t_dm_satfills[i].y0 && y <= t_dm_satfills[i].y1 + 2)
            return t_dm_satfills[i].col && t_dm_satfills[i].gen == t_dm_fill_gen;
    }
    return 0;
}
/* the colored fill that is the most recent fill containing the point, or 0 */
static uint32_t dm_satfill_at(void *ctx, float x, float y) {
    if (!dm_abs(ctx, &x, &y))
        return 0;
    for (unsigned k = 0; k < DM_SATFILL_N; k++) {
        unsigned i = (t_dm_satfill_i - 1 - k) % DM_SATFILL_N;
        if (t_dm_satfills[i].valid && t_dm_satfills[i].ctx == ctx && x >= t_dm_satfills[i].x0 &&
            x < t_dm_satfills[i].x1 && y > t_dm_satfills[i].y0 && y <= t_dm_satfills[i].y1 + 2)
            return t_dm_satfills[i].col;
    }
    return 0;
}
/* 1 = the most recent fill under the point is a stock light bar kept light */
static int dm_lightbar_at(void *ctx, float x, float y) {
    if (!dm_abs(ctx, &x, &y))
        return 0;
    for (unsigned k = 0; k < DM_SATFILL_N; k++) {
        unsigned i = (t_dm_satfill_i - 1 - k) % DM_SATFILL_N;
        if (t_dm_satfills[i].valid && t_dm_satfills[i].ctx == ctx && x >= t_dm_satfills[i].x0 &&
            x < t_dm_satfills[i].x1 && y > t_dm_satfills[i].y0 && y <= t_dm_satfills[i].y1 + 2)
            return t_dm_satfills[i].light;
    }
    return 0;
}
/* EP147's dialog title bar (OK/CANCEL boxes, its own and azmod's REMOVE PLAYLIST /
 * REMOVE FOLDER) is a filled path in light gray (0xafafaf) holding a black title. The
 * gray map treats every light gray as ink, so the bar would go dark (0x454545) and the
 * title, mapped as dark text, would become an unreadable dim 0x60 on it. A light surface stays a
 * light surface: a bar-shaped path (wide, 20..80 px tall: never the dialog's outline,
 * never an icon) in an opaque light stock gray is filled with that stock gray, and
 * text drawn on it keeps its stock lightness (black, as stock). Path::bounds is
 * {xMin, xMax, yMin, yMax} at +0x18 (checked on both supported builds). */
#define DM_PATH_BOUNDS 0x18
#define DM_BAR_MIN_W 120
#define DM_BAR_MIN_H 20
#define DM_BAR_MAX_H 80
#define DM_BAR_MIN_L 0x90
static void (*g_dm_orig_fillpath)(void *ctx, const void *path, const void *xf);
static void dm_fillpath_hook(void *ctx, const void *path, const void *xf) {
    if (__atomic_load_n(&g_dm_on, __ATOMIC_RELAXED) && !t_dm_bypass && dm_flip_on() && path &&
        xf) {
        unsigned char *st = *(unsigned char **) ((unsigned char *) ctx + 8);
        uint32_t s0 = t_dm_stock, r = (s0 >> 16) & 0xff, g = (s0 >> 8) & 0xff, b = s0 & 0xff;
        uint32_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b),
                 mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        const float *pb = (const float *) ((const unsigned char *) path + DM_PATH_BOUNDS);
        const float *m = xf;
        if (st && !*(void **) (st + DM_STATE_FILL + DM_FT_GRADIENT) &&
            !*(void **) (st + DM_STATE_FILL + DM_FT_IMAGE) && (s0 >> 24) >= 0xc0 && mx - mn <= 8 &&
            g >= DM_BAR_MIN_L && *(uint32_t *) (st + DM_STATE_FILL) == dm_map(s0) &&
            m[0] == 1.f && m[1] == 0.f && m[3] == 0.f && m[4] == 1.f &&
            pb[1] - pb[0] >= DM_BAR_MIN_W && pb[3] - pb[2] >= DM_BAR_MIN_H &&
            pb[3] - pb[2] <= DM_BAR_MAX_H) {
            float x = pb[0] + m[2], y = pb[2] + m[5];
            uint32_t *col = (uint32_t *) (st + DM_STATE_FILL), keep = *col;
            if (dm_abs(ctx, &x, &y)) {
                unsigned i = (t_dm_satfill_i++) % DM_SATFILL_N;
                t_dm_fill_gen++;
                t_dm_satfills[i].ctx = ctx;
                t_dm_satfills[i].col = 0;
                t_dm_satfills[i].valid = 1;
                t_dm_satfills[i].light = 1;
                t_dm_satfills[i].gen = t_dm_fill_gen;
                t_dm_satfills[i].x0 = x;
                t_dm_satfills[i].y0 = y;
                t_dm_satfills[i].x1 = x + (pb[1] - pb[0]);
                t_dm_satfills[i].y1 = y + (pb[3] - pb[2]);
            }
            *col = s0;
            g_dm_orig_fillpath(ctx, path, xf);
            *col = keep;
            return;
        }
    }
    g_dm_orig_fillpath(ctx, path, xf);
}
static void (*g_dm_orig_fillrect_i)(void *ctx, const int *r, int replace);
static void dm_fillrect_i_hook(void *ctx, const int *r, int replace) {
    if (__atomic_load_n(&g_dm_on, __ATOMIC_RELAXED) && r)
        dm_note_fill(ctx, r[0], r[1], r[2], r[3]);
    g_dm_orig_fillrect_i(ctx, r, replace);
}
static void (*g_dm_orig_fillrect_f)(void *ctx, const float *r);
static void dm_fillrect_f_hook(void *ctx, const float *r) {
    if (__atomic_load_n(&g_dm_on, __ATOMIC_RELAXED) && r)
        dm_note_fill(ctx, r[0], r[1], r[2], r[3]);
    g_dm_orig_fillrect_f(ctx, r);
}
/* One renderer vtable slot, only when it holds the function this was read from. The swap
 * itself is azm_patch_vtable's: the original is stored before the slot, and the slot store is
 * one release-ordered aligned word, so a paint already running sees the old pointer or the
 * hook, and the hook never sees an empty original. */
static int dm_patch_slot(uintptr_t vt_off, uintptr_t expect, void *hook, void **orig) {
    void **slot = (void **) (DM_SWR_VTABLE + vt_off);
    if ((uintptr_t) *slot != expect)
        return -2;
    return azm_patch_vtable((uintptr_t) slot, hook, orig);
}
static double dm_lum_u32(uint32_t c) {
    return dm_lum((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
}

static void (*g_dm_orig_drawglyph)(void *ctx, int glyph, const void *xf);
static void dm_drawglyph_hook(void *ctx, int glyph, const void *xf) {
    if (__atomic_load_n(&g_dm_gcensus, __ATOMIC_RELAXED) > 0) {
        __atomic_sub_fetch(&g_dm_gcensus, 1, __ATOMIC_RELAXED);
        unsigned char *st = *(unsigned char **) ((unsigned char *) ctx + 8);
        FILE *f = azlog_open("/tmp/azmod-dm-gcensus.log", "a");
        if (f && st) {
            const float *mm = xf;
            fprintf(f, "glyph %d ctx %p col %08x at %.1f,%.1f under %08x grad %p img %p\n", glyph,
                    ctx, *(uint32_t *) (st + DM_STATE_FILL), mm ? mm[2] : -1.f, mm ? mm[5] : -1.f,
                    mm ? dm_satfill_at(ctx, mm[2], mm[5]) : 0,
                    *(void **) (st + DM_STATE_FILL + DM_FT_GRADIENT),
                    *(void **) (st + DM_STATE_FILL + DM_FT_IMAGE));
        }
        if (f)
            fclose(f);
    }
    if (__atomic_load_n(&g_dm_on, __ATOMIC_RELAXED) && !t_dm_bypass &&
        dm_flip_on()) { /* ink: light themes only */
        unsigned char *st = *(unsigned char **) ((unsigned char *) ctx + 8);
        if (st && !*(void **) (st + DM_STATE_FILL + DM_FT_GRADIENT) &&
            !*(void **) (st + DM_STATE_FILL + DM_FT_IMAGE)) {
            uint32_t *col = (uint32_t *) (st + DM_STATE_FILL), keep = *col, ink = dm_ink(keep);
            uint32_t cr = (keep >> 16) & 0xff, cg = (keep >> 8) & 0xff, cb = keep & 0xff;
            uint32_t cmx = cr > cg ? (cr > cb ? cr : cb) : (cg > cb ? cg : cb),
                     cmn = cr < cg ? (cr < cb ? cr : cb) : (cg < cb ? cg : cb);
            const float *m = xf;
            /* Dim gray text (stock disabled labels, #3c on #23) sits in the surface band,
             * so setFill lifted it to near-white like the ground under it. Text is always
             * ink: undo the lift (exact, since dm_L is linear on the band and flipped ink
             * never reaches DM_GROUND) and flip it, no lighter than a readable mid gray. */
            if (cmx - cmn <= 12 && dm_unL((int) cg) >= 0) {
                uint32_t v = (uint32_t) dm_text_L(dm_unL((int) cg));
                ink = (keep & 0xff000000u) | (v << 16) | (v << 8) | v;
            }
            /* Any text on a colored fill, gray or colored (the loaded track's green
             * title on the blue cursor row), reads black when black reaches 4.5:1. */
            uint32_t under = m ? dm_satfill_at(ctx, m[2], m[5]) : 0;
            if (m && !under && cmx - cmn <= 12 && dm_unL((int) cg) >= 0 &&
                dm_lightbar_at(ctx, m[2], m[5])) {
                /* text on a stock light bar kept light (dm_fillpath_hook): its stock gray */
                uint32_t v = (uint32_t) dm_unL((int) cg);
                ink = (keep & 0xff000000u) | (v << 16) | (v << 8) | v;
            }
            /* Colored text on a colored fill (a played track's green title and BPM on the blue
             * cursor row) keeps the color Day gives it everywhere else, dm_ink(stock): the
             * same green as the row's own play arrow and as the row off the cursor ((0,109,0)
             * arrow, (0,110,0) text; stock lime there read as another color).
             * Gray text goes black or stock white. */
            if (under && cmx - cmn <= 48) { /* text on a colored fill: black, or stock white */
                uint32_t o = dm_on_fill(under, keep, ink);
                if ((o & 0xffffff) != 0xffffff || dm_satfill_is_last(ctx, m[2], m[5]))
                    ink = o;
            }
            if (ink != keep) {
                *col = ink;
                g_dm_orig_drawglyph(ctx, glyph, xf);
                *col = keep;
                return;
            }
        }
    }
    g_dm_orig_drawglyph(ctx, glyph, xf);
}
static int dm_hook_drawglyph(void) {
    return dm_patch_slot(DM_SWR_DRAWGLYPH_OFF, DM_SWR_DRAWGLYPH, (void *) dm_drawglyph_hook,
                         (void **) &g_dm_orig_drawglyph);
}

static int dm_hook_setfill(void) {
    /* -2 = not a supported build: no patch */
    return dm_patch_slot(DM_SWR_SETFILL_OFF, DM_SWR_SETFILL, (void *) dm_setfill_hook,
                         (void **) &g_dm_orig_setfill);
}

/* Graphics::setColour with our color taken literally (no remap). */
static void dm_set_color_raw(void *g, uint32_t argb) {
    t_dm_bypass = 1;
    ((void (*)(void *, const uint32_t *)) DM_GFX_SETCOLOUR)(g, &argb);
    t_dm_bypass = 0;
}

/* ---- Art: EP147's decoded PNGs, recolored in place, lazily, reversibly. ------
 * ImageCache::getFromMemory is hooked from azmod's constructor (before EP147
 * decodes anything) and every software image it returns is recorded with its
 * pixel pointer and size. At draw time (drawImageTransformed, on EP147's paint
 * thread) an image whose state differs from the switch has its gray pixels flipped
 * in place: premultiplied v' = a - v, exactly involutive, so switching off restores the
 * original bytes. Images with real color (pads, logos, accents) are classified once
 * and never touched. Entries are re-checked against the live pixel pointer + size
 * before every flip, so a freed image whose memory is reused is dropped, never
 * corrupted. */
#define DM_ART_SLOTS 8192 /* power of two; ~2.8k PNGs in v1.30 */
#define DM_GRAY_TOL 6     /* channel spread still counted as gray */
enum {
    DM_K_UNKNOWN = 0,
    DM_K_GRAY,
    DM_K_COLOR
};
struct dm_art {
    void *_Atomic ipd;
    void *data;
    int w, h, kind, flipped, bpp;
    unsigned char *orig;
    uint32_t ink;
    int grayonly, tint, perpixel, th, noart;
}; /* bpp 4 = ARGB, 3 = RGB; orig: RGB bytes kept while themed; ink: see dm_classify; perpixel:
      azmod's multi-hue art (4-DECK strips), themed per pixel (dm_mark_perpixel); th: the theme the
      bytes are themed for (valid while flipped) */
static struct dm_art g_dm_art[DM_ART_SLOTS];
static _Atomic int g_dm_art_n;

static unsigned dm_hash(void *p) {
    uintptr_t x = (uintptr_t) p >> 4;
    x ^= x >> 17;
    x *= 0x9E3779B1u;
    return (unsigned) x & (DM_ART_SLOTS - 1);
}

/* Called right after getFromMemory returns: `img` is the returned juce::Image. */
__attribute__((visibility("hidden"))) void dm_record_art(void **img) {
    if (!img || !*img)
        return;
    unsigned char *b = (unsigned char *) *img;
    /* ARGB (format 2, 4 B/px) and RGB (format 1, 3 B/px): a PNG without an alpha
     * channel decodes to RGB (e.g. the drum-roll pad tiles). */
    int fmt = *(int *) (b + DM_IPD_FORMAT), ps = *(int *) (b + DM_SPD_PSTR);
    if (*(uintptr_t *) b != DM_SPD_VTABLE || !((fmt == 2 && ps == 4) || (fmt == 1 && ps == 3)))
        return;
    void *ipd = *img;
    for (unsigned i = dm_hash(ipd), n = 0; n < DM_ART_SLOTS;
         i = (i + 1) & (DM_ART_SLOTS - 1), n++) {
        void *cur = __atomic_load_n(&g_dm_art[i].ipd, __ATOMIC_ACQUIRE);
        if (cur == ipd)
            return;
        if (cur == NULL) {
            g_dm_art[i].data = *(void **) (b + DM_SPD_DATA);
            g_dm_art[i].w = *(int *) (b + DM_IPD_W);
            g_dm_art[i].h = *(int *) (b + DM_IPD_H);
            g_dm_art[i].kind = DM_K_UNKNOWN;
            g_dm_art[i].flipped = 0;
            g_dm_art[i].bpp = ps;
            g_dm_art[i].orig = NULL;
            g_dm_art[i].ink = 0;
            g_dm_art[i].grayonly = 0;
            g_dm_art[i].tint = 0;
            g_dm_art[i].perpixel = 0;
            g_dm_art[i].noart = 0;
            void *expect = NULL;
            if (__atomic_compare_exchange_n(&g_dm_art[i].ipd, &expect, ipd, 0, __ATOMIC_RELEASE,
                                            __ATOMIC_RELAXED)) {
                __atomic_add_fetch(&g_dm_art_n, 1, __ATOMIC_RELAXED);
                return;
            }
        }
    }
}

static struct dm_art *dm_find_art(void *ipd) {
    for (unsigned i = dm_hash(ipd), n = 0; n < DM_ART_SLOTS;
         i = (i + 1) & (DM_ART_SLOTS - 1), n++) {
        void *cur = __atomic_load_n(&g_dm_art[i].ipd, __ATOMIC_ACQUIRE);
        if (cur == ipd)
            return &g_dm_art[i];
        if (cur == NULL)
            return NULL;
    }
    return NULL;
}
/* The entry for a live image: dm_find_art plus identity. Entries are never removed when
 * EP147 frees an image (a track's artwork goes on every load), so a new image can sit at
 * a freed one's address and inherit its entry: a deck's overview strip would then draw in
 * the old art's single ink, and a flip would write into the freed buffer. An entry whose
 * pixel buffer or size is not this image's belongs to the earlier image: NULL (the first
 * 16 logged). */
static struct dm_art *dm_find_art_live(void *ipd) {
    struct dm_art *e = dm_find_art(ipd);
    if (!e)
        return NULL;
    unsigned char *b = ipd;
    if (e->data == *(void **) (b + DM_SPD_DATA) && e->w == *(int *) (b + DM_IPD_W) &&
        e->h == *(int *) (b + DM_IPD_H))
        return e;
    static int logged; /* paint thread: the first few only, never a file open per draw */
    if (logged < 16) {
        logged++;
        FILE *f = azlog_open("/tmp/azmod-daymode.log", "a");
        if (f) {
            fprintf(f, "art entry at a reused address ignored: %p entry %dx%d kind %d, image %dx%d\n",
                    ipd, e->w, e->h, e->kind, *(int *) (b + DM_IPD_W), *(int *) (b + DM_IPD_H));
            fclose(f);
        }
    }
    return NULL;
}
/* azmod's own runtime images (menu_popup's name labels and panel title): decoded
 * uncached, then glyphs composed in, then recorded here. Unlike ImageCache art they are
 * released and rebuilt, so an entry may already sit at this address from an earlier
 * image: it is stale by definition (a new one was just made), so it is reset in place
 * (same slot, saved bytes freed) instead of inheriting "already themed". Same thread
 * as every flip (EP147's message/paint thread). */
static void dm_record_own_art(void **img) {
    if (!img || !*img)
        return;
    struct dm_art *e = dm_find_art(*img);
    if (!e) {
        dm_record_art(img);
        return;
    }
    unsigned char *b = (unsigned char *) *img;
    int fmt = *(int *) (b + DM_IPD_FORMAT), ps = *(int *) (b + DM_SPD_PSTR);
    if (*(uintptr_t *) b != DM_SPD_VTABLE || !((fmt == 2 && ps == 4) || (fmt == 1 && ps == 3))) {
        __atomic_store_n(&e->ipd, (void *) 1, __ATOMIC_RELEASE); /* not ours to theme: tombstone */
        return;
    }
    free(e->orig);
    e->orig = NULL;
    e->data = *(void **) (b + DM_SPD_DATA);
    e->w = *(int *) (b + DM_IPD_W);
    e->h = *(int *) (b + DM_IPD_H);
    e->kind = DM_K_UNKNOWN;
    e->flipped = 0;
    e->bpp = ps;
    e->ink = 0;
    e->grayonly = e->tint = e->perpixel = e->noart = 0;
}

/* Mark azmod's own multi-hue art (the 4-DECK stem strips) to be themed per pixel:
 * the single-ground/single-ink tint (dm_flip_argb_tint) would collapse the four
 * stem hues to one. Called from stems_display.c stems4_tick after the strips decode. */
static void azmod_dm_mark_perpixel(void *ipd) {
    struct dm_art *e = ipd ? dm_find_art_live(ipd) : NULL;
    if (e)
        e->perpixel = 1;
}

static int dm_is_gray(const unsigned char *p) { /* premultiplied B,G,R,A */
    int mx = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]);
    int mn = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) : (p[1] < p[2] ? p[1] : p[2]);
    return mx - mn <= DM_GRAY_TOL;
}

static int dm_rgb_ground(const unsigned char *px, int n, int gc[3], int gn[3]);
/* EP147's "NO ARTWORK" placeholder (a dark vinyl disc: black corners, #323232 disc,
 * #191919 label; drawn at many sizes) stands in for artwork, and artwork is never
 * themed. Its whole picture lives in the surface band (L <= 0x40), which every theme
 * compresses into a narrow range, so theming it would leave a blank square. */
static int dm_is_noart(const struct dm_art *e, const unsigned char *ipd) {
    /* Recognized by structure at any size, with tolerance: opaque gray; a dark center
     * label; a lighter ring ~3/10 of the size out, the same in all four directions;
     * corners at least as dark as the label. */
    if (e->bpp != 4 || e->w < 16 || e->h < 16 || e->w * 10 > e->h * 17 || e->h * 10 > e->w * 17)
        return 0;
    const unsigned char *d = e->data;
    int ls = *(int *) (ipd + DM_SPD_LSTR);
    int cx = e->w / 2, cy = e->h / 2, r = (e->w < e->h ? e->w : e->h) * 3 / 10;
    /* clang-format off */
    int pts[9][2] = {{cx, cy}, {cx - r, cy},  {cx + r, cy},  {cx, cy - r},        {cx, cy + r},
                     {1, 1},   {e->w - 2, 1}, {1, e->h - 2}, {e->w - 2, e->h - 2}};
    /* clang-format on */
    int L[9];
    for (int k = 0; k < 9; k++) {
        const unsigned char *p = d + (long) pts[k][1] * ls + pts[k][0] * 4;
        if (p[3] < 250 || abs(p[0] - p[2]) > 6 || abs(p[0] - p[1]) > 6)
            return 0; /* opaque gray */
        L[k] = p[1];
    }
    if (L[0] < 0x0c || L[0] > 0x28)
        return 0; /* the label */
    int rmin = 255, rmax = 0;
    for (int k = 1; k <= 4; k++) {
        if (L[k] < rmin)
            rmin = L[k];
        if (L[k] > rmax)
            rmax = L[k];
    }
    if (rmin < 0x26 || rmax > 0x44 || rmax - rmin > 8 || rmin - L[0] < 0x0c)
        return 0; /* the disc ring */
    for (int k = 5; k <= 8; k++)
        if (L[k] > L[0] + 8)
            return 0; /* the dark sleeve */
    return 1;
}
static void dm_classify(struct dm_art *e, unsigned char *ipd) {
    unsigned char *d = e->data;
    int ls = *(int *) (ipd + DM_SPD_LSTR);
    e->noart = dm_is_noart(e, ipd);
    if (e->noart &&
        access("/home/root/settings/azmod-logs.on", F_OK) == 0) { /* which sizes it caught */
        FILE *f = azlog_open("/tmp/azmod-dm-noart.log", "a");
        if (f) {
            fprintf(f, "no-artwork placeholder %dx%d: never themed\n", e->w, e->h);
            fclose(f);
        }
    }
    long opaque = 0, color = 0;
    for (int y = 0; y < e->h; y++)
        for (int x = 0; x < e->w; x++) {
            unsigned char *p = d + (long) y * ls + x * e->bpp;
            if (e->bpp == 4 && p[3] < 16)
                continue; /* RGB is opaque */
            opaque++;
            if (!dm_is_gray(p))
                color++;
        }
    /* dm_flip only flips an image's gray pixels, so "gray kind" means "has grays
     * worth flipping". Only art that is mostly color (> 35% of opaque pixels) is left
     * whole; gray art with color accents (a blue-bordered black tile) still flips. */
    e->kind = (opaque && color * 100 > opaque * 35) ? DM_K_COLOR : DM_K_GRAY;
    if (e->kind == DM_K_COLOR) { /* a dark-tint ground (the stem tiles): flippable */
        unsigned char *tmp = malloc((size_t) e->w * 3 * e->h);
        if (tmp) {
            int n = 0;
            for (int y = 0; y < e->h; y++)
                for (int x = 0; x < e->w; x++) {
                    unsigned char *q = d + (long) y * ls + x * e->bpp;
                    if (e->bpp == 4) {
                        if (q[3] < 0xc0)
                            continue;
                        for (int c = 0; c < 3; c++) {
                            int v = q[c] * 255 / q[3];
                            tmp[n * 3 + c] = v > 255 ? 255 : v;
                        }
                    } else
                        memcpy(tmp + n * 3, q, 3);
                    n++;
                }
            int gc[3], gn[3];
            int mx = 0;
            /* Classify by the ground's shape, not dm_rgb_ground's return value: that
             * answers "does this theme move it", which would tie the classification to
             * whichever theme was active at first draw. */
            if (n) {
                dm_rgb_ground(tmp, n, gc, gn);
                mx = gc[0] > gc[1] ? (gc[0] > gc[2] ? gc[0] : gc[2])
                                   : (gc[1] > gc[2] ? gc[1] : gc[2]);
            }
            if (n && ((gc[0] < gc[1] ? (gc[0] < gc[2] ? gc[0] : gc[2])
                                     : (gc[1] < gc[2] ? gc[1] : gc[2])) >= mx - 12 ||
                      mx <= 0x50)) {
                int mn = gc[0] < gc[1] ? (gc[0] < gc[2] ? gc[0] : gc[2])
                                       : (gc[1] < gc[2] ? gc[1] : gc[2]);
                if (e->bpp == 3)
                    e->kind = DM_K_GRAY;
                else if (mx - mn > 12)
                    e->tint = 1; /* ARGB tint ground, recorded under any theme so a later
                                  * live switch to Day still lightens it (dm_art_daykind
                                  * treats tint art as gray kind) */
            }
            free(tmp);
        }
    }
    /* Pure gray ARGB art that is text-shaped (under 55% covered): text, icons. A gray
     * button with an opaque face is excluded, or inking it on a colored fill would
     * make a solid black square. */
    e->grayonly = opaque && !color && e->bpp == 4 && opaque * 100 < (long) e->w * e->h * 55;
    /* Colored text art: the deck tile's orange BPM digits, WIDE, QUANTIZE, Q are
     * file-loaded PNGs that drawGlyph never sees. One color + text-shaped (strokes
     * cover < 45% of the box) -> e->ink, and the draw hook paints it through the alpha
     * fill in the ink shade. The art's pixels are never touched. ARGB only (RGB has
     * no alpha). */
    e->ink = 0;
    if (e->kind == DM_K_COLOR && !e->tint && e->bpp == 4 && opaque) {
        long tot = (long) e->w * e->h, na = 0, acc[3] = {0, 0, 0};
        int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
        for (int y = 0; y < e->h; y++)
            for (int x = 0; x < e->w; x++) {
                unsigned char *p = d + (long) y * ls + x * 4;
                if (p[3] < 160)
                    continue;
                for (int c = 0; c < 3; c++) {
                    int v = p[c] * 255 / p[3];
                    if (v < mn[c])
                        mn[c] = v;
                    if (v > mx[c])
                        mx[c] = v;
                    acc[c] += v;
                }
                na++;
            }
        /* One color: glyph cores within 48 per channel (observed spreads reach 44).
         * Text size: the hot cue letter chips (16x18, a fill that stays stock) are
         * below 24 px on both sides, every label seen above. */
        int one = na > 0 && (e->w >= 24 || e->h >= 24);
        for (int c = 0; c < 3 && one; c++)
            if (mx[c] - mn[c] > 48)
                one = 0;
        if (access("/tmp/azmod-dm-inklog", F_OK) == 0) {
            FILE *lf = azlog_open("/tmp/azmod-dm-inklog.log", "a");
            if (lf) {
                fprintf(lf, "art %dx%d opaque %ld/%ld na %ld one %d spread %d/%d/%d\n", e->w, e->h,
                        opaque, tot, na, one, mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]);
                fclose(lf);
            }
        }
        if (one && opaque * 100 < tot * 45) {
            uint32_t col = 0xff000000u | ((uint32_t) (acc[2] / na) << 16) |
                           ((uint32_t) (acc[1] / na) << 8) | (uint32_t) (acc[0] / na);
            uint32_t ink = dm_ink(col);
            if (ink != col)
                e->ink = ink;
        }
    }
}

/* The ground of RGB art: its commonest color (4-bit-per-channel bins, then the mean
 * of that bin), and where it goes under the theme. A gray ground takes the lightness
 * map; a dark tint ground (the stem pad tiles' #230606 / navy / plum: max channel
 * <= 0x50) becomes a light tint of the same hue in light themes, so the stem color
 * coding survives. Returns 0 when the ground should not move: leave that art alone. */
static int dm_rgb_ground(const unsigned char *px, int n, int gc[3], int gn[3]) {
    static unsigned short bins[4096];
    memset(bins, 0, sizeof bins);
    for (int i = 0; i < n; i++) {
        const unsigned char *p = px + i * 3;
        bins[((p[2] >> 4) << 8) | ((p[1] >> 4) << 4) | (p[0] >> 4)]++;
    }
    int best = 0;
    for (int k = 1; k < 4096; k++)
        if (bins[k] > bins[best])
            best = k;
    long acc[3] = {0, 0, 0}, na = 0;
    for (int i = 0; i < n; i++) {
        const unsigned char *p = px + i * 3;
        if ((((p[2] >> 4) << 8) | ((p[1] >> 4) << 4) | (p[0] >> 4)) != best)
            continue;
        for (int c = 0; c < 3; c++)
            acc[c] += p[c];
        na++;
    }
    for (int c = 0; c < 3; c++)
        gc[c] = na ? (int) (acc[c] / na) : 0; /* B, G, R */
    int mx = gc[0] > gc[1] ? (gc[0] > gc[2] ? gc[0] : gc[2]) : (gc[1] > gc[2] ? gc[1] : gc[2]);
    int mn = gc[0] < gc[1] ? (gc[0] < gc[2] ? gc[0] : gc[2]) : (gc[1] < gc[2] ? gc[1] : gc[2]);
    if (mx - mn <= 12) {
        if (!dm_flip_on()) {
            uint32_t t = dm_gray(gc[1]);
            gn[0] = t & 0xff;
            gn[1] = (t >> 8) & 0xff;
            gn[2] = (t >> 16) & 0xff;
            return 1;
        }
        int d = dm_L(gc[1]) - gc[1];
        for (int c = 0; c < 3; c++) {
            int v = gc[c] + d;
            gn[c] = v < 0 ? 0 : v > 255 ? 255
                                        : v;
        }
        return 1;
    }
    if (mx > 0x50 || !dm_flip_on())
        return 0; /* dark tints: light themes only (dark ones keep them) */
    for (int c = 0; c < 3; c++) {
        int v = 0xf0 - (mx - gc[c]) * 2;
        gn[c] = v < 0x90 ? 0x90 : v;
    }
    return 1;
}

/* RGB art is flattened: text was anti-aliased onto the dark ground before saving,
 * so its edge pixels are blends of ink and ground, and a per-pixel gray flip would
 * speckle them. Instead the ground is swapped: Gc = the ground color, T = the pixel
 * farthest from it (the ink); each pixel's coverage a = |p - Gc| / |T - Gc| (max
 * channel), and it moves by (1 - a) * (Gn - Gc). Not exactly invertible, so the
 * original bytes are kept and switching off restores them verbatim. */
static int dm_flip_rgb(struct dm_art *e, unsigned char *ipd) {
    unsigned char *d = e->data;
    int ls = *(int *) (ipd + DM_SPD_LSTR), rb = e->w * 3;
    if (!e->flipped) { /* -> themed */
        if (!e->orig && !(e->orig = malloc((size_t) rb * e->h)))
            return 0;
        for (int y = 0; y < e->h; y++)
            memcpy(e->orig + (size_t) y * rb, d + (long) y * ls, rb);
        int gc[3], gn[3];
        if (!dm_rgb_ground(e->orig, e->w * e->h, gc, gn)) {
            e->flipped = 1;
            return 1;
        } /* bright ground: as is */
        int dt = 0;
        for (int i = 0; i < e->w * e->h; i++) {
            unsigned char *p = e->orig + i * 3;
            for (int c = 0; c < 3; c++) {
                int dd = abs((int) p[c] - gc[c]);
                if (dd > dt)
                    dt = dd;
            }
        }
        /* The ink: the pixel farthest from the ground, given its 4.5:1 shade when it
         * is a color (bright labels on the light ground would not read otherwise). */
        unsigned char tc[3] = {0, 0, 0};
        int tbest = -1;
        for (int i = 0; i < e->w * e->h; i++) {
            unsigned char *pp = e->orig + i * 3;
            int dd = 0;
            for (int c = 0; c < 3; c++) {
                int v = abs((int) pp[c] - gc[c]);
                if (v > dd)
                    dd = v;
            }
            if (dd > tbest) {
                tbest = dd;
                memcpy(tc, pp, 3);
            }
        }
        uint32_t tcol = 0xff000000u | ((uint32_t) tc[2] << 16) | ((uint32_t) tc[1] << 8) | tc[0];
        uint32_t tink =
            dm_flip_on() ? dm_ink(tcol) : tcol; /* dark themes: the stock ink reads as is */
        /* gray ink on a light ground must turn dark; a ground-only move keeps white
         * text white */
        if (dm_flip_on() && tink == tcol && abs((int) tc[0] - (int) tc[1]) <= 12 &&
            abs((int) tc[1] - (int) tc[2]) <= 12 && tc[1] > DM_HILITE) {
            int v = dm_text_L(tc[1]);
            tink = 0xff000000u | ((uint32_t) v << 16) | ((uint32_t) v << 8) | (uint32_t) v;
        }
        int ti[3] = {(int) (tink & 0xff), (int) ((tink >> 8) & 0xff),
                     (int) ((tink >> 16) & 0xff)}; /* B, G, R */
        for (int y = 0; y < e->h; y++) {
            unsigned char *o = e->orig + (size_t) y * rb, *q = d + (long) y * ls;
            for (int x = 0; x < e->w; x++) {
                int dmax = 0;
                for (int c = 0; c < 3; c++) {
                    int dd = abs((int) o[x * 3 + c] - gc[c]);
                    if (dd > dmax)
                        dmax = dd;
                }
                int keep = dt >= 16 ? (dmax * 256) / dt : 0;
                if (keep > 256)
                    keep = 256; /* coverage a, x256; no ink = all ground */
                for (int c = 0; c < 3; c++) {
                    int v = tink != tcol ? (keep * ti[c] + (256 - keep) * gn[c]) / 256
                                         : o[x * 3 + c] + ((256 - keep) * (gn[c] - gc[c])) / 256;
                    q[x * 3 + c] = (unsigned char) (v < 0 ? 0 : v > 255 ? 255
                                                                        : v);
                }
            }
        }
    } else if (e->orig) { /* -> stock: the original bytes */
        for (int y = 0; y < e->h; y++)
            memcpy(d + (long) y * ls, e->orig + (size_t) y * rb, rb);
    }
    e->flipped = !e->flipped;
    return 1;
}

/* ARGB art on a dark tint ground (the stem pad tiles, 148x20): the RGB treatment on
 * un-premultiplied color (ground to a light tint, ink by coverage to its 4.5:1
 * shade), re-premultiplied with each pixel's own alpha. Original bytes kept. */
static void dm_flip_argb_tint(struct dm_art *e, unsigned char *ipd) {
    unsigned char *d = e->data;
    int ls = *(int *) (ipd + DM_SPD_LSTR), rb = e->w * 4;
    if (e->flipped) {
        if (e->orig)
            for (int y = 0; y < e->h; y++)
                memcpy(d + (long) y * ls, e->orig + (size_t) y * rb, rb);
        e->flipped = 0;
        return;
    }
    if (!e->orig && !(e->orig = malloc((size_t) rb * e->h)))
        return;
    for (int y = 0; y < e->h; y++)
        memcpy(e->orig + (size_t) y * rb, d + (long) y * ls, rb);
    int n = 0;
    unsigned char *tmp = malloc((size_t) e->w * e->h * 3);
    if (!tmp)
        return;
    for (int i = 0; i < e->w * e->h; i++) {
        unsigned char *q = e->orig + i * 4;
        if (q[3] < 0xc0)
            continue;
        for (int c = 0; c < 3; c++) {
            int v = q[c] * 255 / q[3];
            tmp[n * 3 + c] = v > 255 ? 255 : v;
        }
        n++;
    }
    int gc[3], gn[3];
    if (!n || !dm_rgb_ground(tmp, n, gc, gn)) {
        free(tmp);
        e->flipped = 1;
        return;
    }
    int dt = 0;
    unsigned char tc[3] = {0, 0, 0};
    for (int i = 0; i < n; i++) {
        int dd = 0;
        for (int c = 0; c < 3; c++) {
            int v = abs((int) tmp[i * 3 + c] - gc[c]);
            if (v > dd)
                dd = v;
        }
        if (dd > dt) {
            dt = dd;
            memcpy(tc, tmp + i * 3, 3);
        }
    }
    free(tmp);
    uint32_t tcol = 0xff000000u | ((uint32_t) tc[2] << 16) | ((uint32_t) tc[1] << 8) | tc[0],
             tink = dm_ink(tcol);
    int ti[3] = {(int) (tink & 0xff), (int) ((tink >> 8) & 0xff), (int) ((tink >> 16) & 0xff)};
    for (int y = 0; y < e->h; y++)
        for (int x = 0; x < e->w; x++) {
            unsigned char *o = e->orig + (size_t) y * rb + x * 4, *q = d + (long) y * ls + x * 4;
            int a = o[3];
            if (!a)
                continue;
            int u[3], dmax = 0;
            for (int c = 0; c < 3; c++) {
                u[c] = o[c] * 255 / a;
                if (u[c] > 255)
                    u[c] = 255;
                int dd = abs(u[c] - gc[c]);
                if (dd > dmax)
                    dmax = dd;
            }
            int keep = dt >= 16 ? (dmax * 256) / dt : 0;
            if (keep > 256)
                keep = 256;
            for (int c = 0; c < 3; c++) {
                int v = (keep * ti[c] + (256 - keep) * gn[c]) / 256;
                v = v < 0 ? 0 : v > 255 ? 255
                                        : v;
                q[c] = (unsigned char) (v * a / 255);
            }
        }
    e->flipped = 1;
}

/* Dark themes keep lightness, so art is themed pixel by pixel with no ground/ink
 * analysis: grays along the theme ramp, selection blues to the accent, every other
 * color as it is. Premultiplied ARGB is un-premultiplied for the map. Original bytes
 * kept (large art too: the ramp is not self-inverse). */
static void dm_theme_px(struct dm_art *e, unsigned char *ipd) {
    unsigned char *d = e->data;
    int ls = *(int *) (ipd + DM_SPD_LSTR), bp = e->bpp, rb = e->w * bp;
    if (e->flipped) {
        if (e->orig)
            for (int y = 0; y < e->h; y++)
                memcpy(d + (long) y * ls, e->orig + (size_t) y * rb, rb);
        e->flipped = 0;
        return;
    }
    if (!e->orig && !(e->orig = malloc((size_t) rb * e->h)))
        return;
    for (int y = 0; y < e->h; y++)
        memcpy(e->orig + (size_t) y * rb, d + (long) y * ls, rb);
    for (int y = 0; y < e->h; y++)
        for (int x = 0; x < e->w; x++) {
            unsigned char *q = d + (long) y * ls + x * bp;
            int a = bp == 4 ? q[3] : 255;
            if (!a)
                continue;
            int u[3];
            for (int c = 0; c < 3; c++) {
                u[c] = q[c] * 255 / a;
                if (u[c] > 255)
                    u[c] = 255;
            }
            uint32_t in =
                0xff000000u | ((uint32_t) u[2] << 16) | ((uint32_t) u[1] << 8) | (uint32_t) u[0];
            uint32_t out = dm_map(in);
            if (out == in)
                continue;
            for (int c = 0; c < 3; c++)
                q[c] = (unsigned char) (((out >> (c * 8)) & 0xff) * a / 255);
        }
    e->flipped = 1;
}

/* The 4-DECK stem strips pack four stem hues in one ARGB image, so the
 * single-ground/single-ink flatten would paint every pill the same color. Theme per
 * pixel instead: a gray OFF-pill pixel takes the lightness flip; a dark saturated
 * pixel (a pill interior) becomes a pale tint of its own hue (as dm_rgb_ground does
 * for dark tints); a bright saturated pixel (a border or letter) takes its hue's
 * 4.5:1 ink. Original bytes kept; switching off restores them verbatim. Dark themes use
 * dm_theme_px instead (hues already correct). */
#define DM_STRIP_DARK 0x60 /* max channel <= this = a pill interior; borders/letters are ~0xcf+ */
static void dm_flip_strip(struct dm_art *e, unsigned char *ipd) {
    unsigned char *d = e->data;
    int ls = *(int *) (ipd + DM_SPD_LSTR), rb = e->w * 4;
    if (e->flipped) {
        if (e->orig)
            for (int y = 0; y < e->h; y++)
                memcpy(d + (long) y * ls, e->orig + (size_t) y * rb, rb);
        e->flipped = 0;
        return;
    }
    if (!e->orig && !(e->orig = malloc((size_t) rb * e->h)))
        return;
    for (int y = 0; y < e->h; y++)
        memcpy(e->orig + (size_t) y * rb, d + (long) y * ls, rb);
    for (int y = 0; y < e->h; y++)
        for (int x = 0; x < e->w; x++) {
            unsigned char *o = e->orig + (size_t) y * rb + x * 4, *q = d + (long) y * ls + x * 4;
            int a = o[3];
            if (!a) {
                q[0] = q[1] = q[2] = q[3] = 0;
                continue;
            }
            int u[3];
            for (int c = 0; c < 3; c++) {
                u[c] = o[c] * 255 / a;
                if (u[c] > 255)
                    u[c] = 255;
            } /* B,G,R un-premult */
            int mx = u[0] > u[1] ? (u[0] > u[2] ? u[0] : u[2]) : (u[1] > u[2] ? u[1] : u[2]);
            int mn = u[0] < u[1] ? (u[0] < u[2] ? u[0] : u[2]) : (u[1] < u[2] ? u[1] : u[2]);
            int out[3];          /* B,G,R */
            if (mx - mn <= 12) { /* gray (OFF-pill ground + its white text): flip */
                uint32_t m = dm_map(0xff000000u | ((uint32_t) u[2] << 16) | ((uint32_t) u[1] << 8) |
                                    (uint32_t) u[0]);
                out[0] = m & 0xff;
                out[1] = (m >> 8) & 0xff;
                out[2] = (m >> 16) & 0xff;
            } else if (mx <=
                       DM_STRIP_DARK) { /* dark tint = pill interior -> pale tint of its hue */
                for (int c = 0; c < 3; c++) {
                    int v = 0xf0 - (mx - u[c]) * 2;
                    out[c] = v < 0x90 ? 0x90 : v > 255 ? 255
                                                       : v;
                }
            } else { /* bright saturated = border/letter -> its hue's ink */
                uint32_t ink = dm_ink(0xff000000u | ((uint32_t) u[2] << 16) |
                                      ((uint32_t) u[1] << 8) | (uint32_t) u[0]);
                out[0] = ink & 0xff;
                out[1] = (ink >> 8) & 0xff;
                out[2] = (ink >> 16) & 0xff;
            }
            for (int c = 0; c < 3; c++) {
                int v = out[c] * a / 255;
                q[c] = (unsigned char) (v < 0 ? 0 : v > 255 ? 255
                                                            : v);
            }
            q[3] = (unsigned char) a;
        }
    e->flipped = 1;
}

/* The art as Day (a flip theme) sees it: tint-ground art is themed like gray art. */
static int dm_art_daykind(const struct dm_art *e) {
    return e->tint ? DM_K_GRAY : e->kind;
}
/* Back to EP147's bytes, whichever theme themed them. Every themed path keeps the
 * original bytes except Day's big art (> 1 MB), which used the self-inverse gray flip
 * (a - v), undone by applying it again. Dispatch on how the art was themed (e->th),
 * never on the current theme, or a live theme-to-theme switch takes the wrong path. */
static void dm_restore(struct dm_art *e, unsigned char *ipd) {
    if (!e->flipped)
        return;
    unsigned char *d = e->data;
    int ls = *(int *) (ipd + DM_SPD_LSTR), rb = e->w * e->bpp;
    if (e->orig)
        for (int y = 0; y < e->h; y++)
            memcpy(d + (long) y * ls, e->orig + (size_t) y * rb, rb);
    else if (e->th >= 0 && e->th < DM_NTHEMES && g_dm_themes[e->th].flip)
        for (int y = 0; y < e->h; y++)
            for (int x = 0; x < e->w; x++) {
                unsigned char *p = d + (long) y * ls + x * e->bpp;
                if ((e->bpp == 4 && p[3] == 0) || !dm_is_gray(p))
                    continue;
                int a = e->bpp == 4 ? p[3] : 255;
                for (int c = 0; c < 3; c++)
                    p[c] = (unsigned char) (a - (int) p[c]);
            }
    e->flipped = 0;
}
static void dm_flip(struct dm_art *e, unsigned char *ipd) {
    if (e->perpixel) {
        if (dm_flip_on())
            dm_flip_strip(e, ipd);
        else
            dm_theme_px(e, ipd);
        return;
    }
    if (!dm_flip_on()) {
        dm_theme_px(e, ipd);
        return;
    }
    if (e->bpp == 3) {
        dm_flip_rgb(e, ipd);
        return;
    }
    if (e->tint) {
        dm_flip_argb_tint(e, ipd);
        return;
    }
    unsigned char *d = e->data;
    int ls = *(int *) (ipd + DM_SPD_LSTR), rb = e->w * e->bpp;
    /* dm_L is not its own inverse, so ARGB art keeps its original bytes while themed
     * and switching off restores them verbatim. Art too big to copy (over 1 MB) uses the
     * self-inverse flip (a - v) instead: exact both ways, white ground instead of gray. */
    int keep = (size_t) rb * e->h <= (1u << 20);
    if (!e->flipped && keep && !e->orig)
        e->orig = malloc((size_t) rb * e->h);
    if (e->flipped && e->orig) { /* -> stock */
        for (int y = 0; y < e->h; y++)
            memcpy(d + (long) y * ls, e->orig + (size_t) y * rb, rb);
        e->flipped = 0;
        return;
    }
    int G = 0;
    int I = 0; /* the dim ink's gray, 0 = none (set with G, used just below) */
    /* A textured face (the browse MENU buttons: a checker of #18/#23/#32 under baked-in
     * labels) is not split into surface and ink gray by gray. That split would collapse label
     * edges into a plateau (dm_text_L floors every ink below #b5 at #60, so an edge from the
     * face to a #ee label becomes one gray, then reverses: #5d -> #60 but #60 -> #72), make
     * halo grays lighter than the face, and keep the checker as a dot pattern on the light
     * face: the labels would read as stepped ("pixelated"). Instead the face becomes one light gray
     * (T_F, the mean of what its checker grays map to, so its brightness is unchanged) and
     * every label pixel is coverage over the face: t = (L - face) / (its stroke's own ink -
     * face), the ink being the lightest gray within 2 px in the stock copy. A stroke's core
     * gets that ink's Day gray (dm_text_L, so a dim label keeps its readable floor) and its
     * edge blends linearly from the face, as alpha antialiasing does. */
    int T_on = 0, T_lo = 0, T_hi = 0, T_F = 0;
    if (!e->flipped && e->orig) {
        for (int y = 0; y < e->h; y++)
            memcpy(e->orig + (size_t) y * rb, d + (long) y * ls, rb);
        /* The art's ground: its commonest opaque gray. Grays near it are surfaces
         * (lifted by dm_L); anything clearly lighter is ink, even inside the surface
         * band (a dim #3c label on a #23 button), or it would vanish with the ground.
         * Ink is flipped, never lighter than a readable mid gray. */
        static unsigned hist[256];
        memset(hist, 0, sizeof hist);
        for (int y = 0; y < e->h; y++)
            for (int x = 0; x < e->w; x++) {
                unsigned char *q = d + (long) y * ls + x * e->bpp;
                if (q[3] >= 0xc0 && dm_is_gray(q))
                    hist[q[1] * 255 / q[3] > 255 ? 255 : q[1] * 255 / q[3]]++;
            }
        for (int v = 1; v < 256; v++)
            if (hist[v] > hist[G])
                G = v;
        /* A ground is a surface: long horizontal runs of its gray. Text-only art has
         * none (its commonest gray is its own ink). Judged by mean run length, not
         * coverage: a real title bar can cover under 40% of its art. No ground: dark
         * grays are ink. */
        /* textured: surface grays that are each a real share of the art and clearly
         * distinct (a checker button face is #18 / #23 / #32). The REC dot (#18, lightly
         * shaded, 20 px) is not: lifted as a surface it would go near-white. */
        long tot = 0;
        for (int v = 0; v < 256; v++)
            tot += hist[v];
        int sg_lo = 256, sg_hi = -1; /* the substantial surface grays' span */
        for (int v = 0; v <= DM_SURF; v++)
            if (tot && hist[v] * 10 >= tot) {
                sg_lo = v < sg_lo ? v : sg_lo;
                sg_hi = v;
            }
        /* a real texture spans distinct grays (0x18..0x32 on the button face); a shaded
         * one-gray icon spans a step or two, and is small */
        int textured = G <= DM_SURF && sg_hi - sg_lo >= 0x08 && e->w >= 64;
        long runs = 0, runpx = 0;
        for (int y = 0; y < e->h; y++) {
            int run = 0;
            for (int x = 0; x <= e->w; x++) {
                unsigned char *q = d + (long) y * ls + x * e->bpp;
                int Lq = x < e->w && q[3] ? (int) (q[1] * 255 / q[3]) : 0;
                /* a dark ground may be textured: the browse MENU buttons' face is a fine
                 * checker of #18 / #23 / #32, so no single gray runs long and the whole face
                 * would read as ink (flat #60 in Day, the disabled label lost on it). On a
                 * textured ground any surface-band gray continues the ground's run. */
                int hit = x < e->w && q[3] >= 0xc0 && dm_is_gray(q) &&
                          (abs(Lq - G) <= 4 || (textured && Lq <= DM_SURF));
                if (hit)
                    run++;
                else if (run) {
                    runs++;
                    runpx += run;
                    run = 0;
                }
            }
        }
        if (!runs || runpx < runs * 12)
            G = -0x20;
        /* Dim ink on a ground (a disabled label: #5a on #23): its anti-aliased edge runs
         * from the ground to the ink, and a hard surface/ink split at G + 0x10 would cut it
         * into jagged steps. Find the ink's own gray (its commonest above that line) and blend
         * the edge from the surface value to the ink value across that span. */
        I = 0;
        if (G >= 0)
            for (int v = G + 0x11; v <= DM_HILITE; v++)
                if (hist[v] > (I ? hist[I] : 0))
                    I = v;
        if (textured && G >= 0 && sg_hi < DM_HILITE) {
            long n = 0, sum = 0;
            for (int v = sg_lo; v <= sg_hi; v++) {
                n += hist[v];
                sum += (long) hist[v] * dm_L(v);
            }
            if (n) {
                T_on = 1;
                T_lo = sg_lo;
                T_hi = sg_hi;
                T_F = (int) ((sum + n / 2) / n);
            }
        }
    }
    for (int y = 0; y < e->h; y++)
        for (int x = 0; x < e->w; x++) {
            unsigned char *p = d + (long) y * ls + x * e->bpp;
            if ((e->bpp == 4 && p[3] == 0) || !dm_is_gray(p))
                continue;
            int a = e->bpp == 4 ? p[3] : 255; /* premultiplied: every channel <= a; RGB: opaque */
            int Lg = (int) p[1] * 255 / a;
            if (Lg > 255)
                Lg = 255;
            int ink = e->orig && dm_flip_on() && Lg <= DM_HILITE && Lg > G + 0x10;
            if (T_on && e->orig) { /* the textured face (see T_on) */
                int L = Lg, M;
                if (L < T_lo && a < 0xf0) {
                    /* the button's own drop shadow: translucent black around the face. Stock
                     * draws it for a dark panel, where it is a few levels deep; mapped like a
                     * surface it would become a faint light gray and the buttons would read flat
                     * on the light panel. Keep it black ink in alpha only (RGB 0), at DM_SHADOW_PCT of
                     * its stock alpha, so it reads as a soft shadow on the light ground. */
                    p[0] = p[1] = p[2] = 0;
                    p[3] = (unsigned char) (a * DM_SHADOW_PCT / 100);
                    continue;
                }
                if (L < T_lo)
                    M = dm_L(L) < T_F ? dm_L(L) : T_F; /* the labels' drop shadow, below the face */
                else if (L <= T_hi)
                    M = T_F;
                else {
                    int ink = L; /* the stroke's ink: the lightest gray within 2 px (stock copy) */
                    for (int yy = y - 2; yy <= y + 2; yy++)
                        for (int xx = x - 2; xx <= x + 2; xx++) {
                            if (yy < 0 || yy >= e->h || xx < 0 || xx >= e->w)
                                continue;
                            const unsigned char *q = e->orig + (size_t) yy * rb + xx * e->bpp;
                            if (q[3] < 0xc0 || !dm_is_gray(q))
                                continue;
                            int Lq = q[1] * 255 / q[3];
                            ink = Lq > ink ? (Lq > 255 ? 255 : Lq) : ink;
                        }
                    int vi = dm_text_L(ink);
                    M = T_F + (vi - T_F) * (L - T_hi) / (ink - T_hi);
                }
                int v = M * a / 255;
                p[0] = p[1] = p[2] = (unsigned char) (v < 0 ? 0 : v > a ? a
                                                                        : v);
                continue;
            }
            for (int c = 0; c < 3; c++) {
                int v;
                if (e->orig) {
                    int L = (int) p[c] * 255 / a;
                    if (L > 255)
                        L = 255;
                    int L0 = G + 0x10;
                    int M = ink && I > L0 && L <= I
                                ? dm_L(L0) + (dm_text_L(I) - dm_L(L0)) * (L - L0) / (I - L0)
                            : ink ? dm_text_L(L)
                            : dm_flip_on()
                                ? dm_L(L)
                                : (int) ((dm_gray(L) >> (c * 8)) & 0xff); /* B,G,R = c 0,1,2 */
                    v = M * a / 255;
                } else
                    v = dm_flip_on() ? a - (int) p[c] : (int) p[c];
                p[c] = (unsigned char) (v < 0 ? 0 : v > a ? a
                                                          : v);
            }
        }
    e->flipped = !e->flipped;
}

static void (*g_dm_orig_drawimg_t)(void *g, void **img, const void *xf, int fill);
/* ---- Additive images (the overviews): rendered for a black ground ------------
 * The deck-tile overview (gui::OverviewWaveform1200PointWidget) is a cached ARGB
 * image (590x44 in 2-DECK) whose strokes are mostly additive: premultiplied color
 * with alpha near 0, so over black they show full color and over white they add up
 * to white and vanish.
 *
 * Recognized by that property, not by who drew it, so there is no dependency on
 * azmod's overview hook (ov_hook). Drawn from a private converted copy: alpha raised
 * to the brightest channel, the same color now covering the ground instead of
 * adding to it. EP147's image is only read, never
 * written, so no other thread can ever see a changed byte. The copy is drawn
 * through a stand-in pixel-data object: the image's own SoftwarePixelData bytes
 * with the pixel pointer/strides aimed at the copy and the reference count pinned
 * high so JUCE can never free it. A read-only draw only calls the vtable's
 * initializeBitmapData, which reads +0x48/+0x50/+0x54 (layout above).
 *
 * Lifetime: the stand-in and the converted buffer are one static object and one
 * static buffer, used only on the paint thread inside one
 * drawImageTransformed call (fill=false: the software renderer reads the source
 * through a local read-only BitmapData and keeps no Image). Nothing may retain
 * it past the call, and that is checked, not assumed: retaining a juce::Image
 * means taking a reference, so the count is compared with the pin after every
 * draw. If it ever differs, the path is switched off for the process and the
 * stand-in and buffer are abandoned (never written, never freed again), so a
 * retained reference can never see changed or freed bytes.
 * Layout: only an object whose vtable is SoftwarePixelData's (exact compare, no
 * other ImagePixelData subclass) with format 2 and pixel stride 4 is copied;
 * anything else draws stock. */
#define DM_CTX_SAVESTATE (0x70 / 8)    /* LowLevelGraphicsContext vtable slots, as used by */
#define DM_CTX_RESTORESTATE (0x78 / 8) /* drawImageTransformed 0x23b04e0 */
#define DM_IPD_REFCOUNT 0x08           /* juce::ReferenceCountedObject::refCount */
#define DM_SPD_SIZE 0x58               /* SoftwarePixelData ends after lineStride (+0x54) */
#define DM_ADD_MIN_W 100               /* candidates: ARGB, >= 100x16, at most 2048x512 */
#define DM_ADD_MIN_H 16

#define DM_ADD_PIN 0x40000000
static _Atomic int g_dm_add_off; /* != 0: a draw retained the stand-in (the refcount it saw) */
static _Atomic long g_dm_add_draws;
static _Atomic int g_dm_add_fake; /* DEV: /tmp/azmod-dm-fakeretain, tests the check above */
/* DEV census: /tmp/azmod-dm-stat logs the pixel make-up of the next big non-art images */
static _Atomic int g_dm_stat;
static void dm_stat_img(unsigned char *b) {
    if (*(uintptr_t *) b != DM_SPD_VTABLE || *(int *) (b + DM_IPD_FORMAT) != 2)
        return;
    int w = *(int *) (b + DM_IPD_W), h = *(int *) (b + DM_IPD_H), ls = *(int *) (b + DM_SPD_LSTR);
    const uint8_t *d = *(const uint8_t **) (b + DM_SPD_DATA);
    if (!d || w < 100 || h < 16 || w > 2048 || h > 512 || w == 1220)
        return;
    __atomic_sub_fetch(&g_dm_stat, 1, __ATOMIC_RELAXED);
    long n = 0, clear = 0, opblack = 0, add = 0, normal = 0, color = 0;
    for (int y = 0; y < h; y += 2)
        for (int x = 0; x < w; x += 2) {
            uint32_t p = ((const uint32_t *) (d + (long) y * ls))[x], a = p >> 24,
                     r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, bl = p & 0xff;
            uint32_t m = r > g ? (r > bl ? r : bl) : (g > bl ? g : bl),
                     mn = r < g ? (r < bl ? r : bl) : (g < bl ? g : bl);
            n++;
            if (a < 8 && m < 8)
                clear++;
            else if (a > 200 && m < 24)
                opblack++;
            else if (m > a + 16)
                add++;
            else
                normal++;
            if (m - mn > 40)
                color++;
        }
    FILE *f = azlog_open("/tmp/azmod-dm-stat.log", "a");
    if (f) {
        fprintf(f, "%dx%d n=%ld clear=%ld opaque-black=%ld additive=%ld normal=%ld colorful=%ld\n",
                w, h, n, clear, opblack, add, normal, color);
        fclose(f);
    }
}
/* What ground an image was rendered for, from its pixels (sampled, <= ~2000 px):
 *   DM_G_ADDITIVE  the deck overviews: transparent, strokes mostly color > alpha;
 *   DM_G_BLACK     the browse preview thumbnails (1200x128: no transparency,
 *                  30-70% opaque black ground, the rest opaque colored bars);
 *                  the black is painted into the image.
 * Both are light over black. Drawn on white, each pixel's alpha becomes its
 * brightest channel (ADDITIVE: only where that is higher; BLACK: always, so the
 * black ground goes transparent and dark edges fade into the white). */
enum {
    DM_G_NONE,
    DM_G_ADDITIVE,
    DM_G_BLACK
};
static int dm_ground_of(unsigned char *b) {
    if (*(uintptr_t *) b != DM_SPD_VTABLE || *(int *) (b + DM_IPD_FORMAT) != 2 ||
        *(int *) (b + DM_SPD_PSTR) != 4)
        return DM_G_NONE;
    int w = *(int *) (b + DM_IPD_W), h = *(int *) (b + DM_IPD_H), ls = *(int *) (b + DM_SPD_LSTR);
    const uint8_t *d = *(const uint8_t **) (b + DM_SPD_DATA);
    if (!d || w < DM_ADD_MIN_W || h < DM_ADD_MIN_H || w > 2048 || h > 512 || ls < w * 4)
        return DM_G_NONE;
    int step = 1;
    while ((long) (w / step) * (h / step) > 2000)
        step++;
    int n = 0, vis = 0, add = 0, clear = 0, opblack = 0, color = 0;
    for (int y = 0; y < h; y += step) {
        const uint32_t *row = (const uint32_t *) (d + (long) y * ls);
        for (int x = 0; x < w; x += step) {
            uint32_t p = row[x], a = p >> 24, r = (p >> 16) & 0xff, g = (p >> 8) & 0xff,
                     bl = p & 0xff;
            uint32_t m = r > g ? (r > bl ? r : bl) : (g > bl ? g : bl);
            uint32_t mn = r < g ? (r < bl ? r : bl) : (g < bl ? g : bl);
            n++;
            if (a < 8 && m < 8)
                clear++;
            if (a > 200 && m < 24)
                opblack++;
            if (m - mn > 40)
                color++;
            if (m < 24)
                continue;
            vis++;
            if (m > a + 16)
                add++;
        }
    }
    if (vis >= 32 && add * 4 >= vis)
        return DM_G_ADDITIVE;
    if (n && clear * 20 < n && opblack * 5 >= n && color * 20 >= n)
        return DM_G_BLACK; /* >=20% black, >=5% color, <5% clear */
    return DM_G_NONE;
}

/* Converted copies, cached: a thumbnail is 153k px and a list draws ~10 of them per
 * repaint. Key = image object + pixel buffer + size + kind + a 64-px fingerprint
 * (EP147 can re-render into the same buffer); 256 px. Paint thread only. */
#define DM_GC_N 16
struct dm_gc {
    void *ipd;
    const void *data;
    int w, h, kind;
    uint32_t fp;
    uint32_t *buf;
    size_t cap;
    unsigned age;
};
static struct dm_gc g_dm_gc[DM_GC_N];
static unsigned g_dm_gc_tick;
static uint32_t dm_fingerprint(const uint8_t *d, int w, int h, int ls) {
    uint32_t f = 2166136261u;
    for (int k = 0; k < 256; k++) {
        int x = (int) ((uint64_t) ((uint32_t) k * 2654435761u) % (unsigned) w), y = (k * 7919) % h;
        f = (f ^ ((const uint32_t *) (d + (long) y * ls))[x]) * 16777619u;
    }
    return f;
}

/* A deck overview strip (590x44 in 2-DECK, 373x38, ...): wide and short. Its kind
 * must not be judged from content: on 2.0x firmware EP147 builds a new, partly filled
 * overview image every ~50 ms while analysis streams in, so a content-based verdict
 * changes from image to image and the overview flickers. Strips are always drawn
 * with the BLACK conversion (see dm_draw_additive). Never an ink cell. */
static _Atomic int g_dm_strip_off;   /* settings/azmod-ovstrip.off, polled by dm_watch */
static _Atomic int g_dm_stripcensus; /* DEV: /tmp/azmod-dm-stripcensus logs strip judgements */
static int dm_ink_of(void *ipd, uint32_t *ink_argb);
static int dm_is_strip(const unsigned char *b) {
    if (*(uintptr_t *) b != DM_SPD_VTABLE || *(int *) (b + DM_IPD_FORMAT) != 2 ||
        *(int *) (b + DM_SPD_PSTR) != 4)
        return 0;
    int w = *(int *) (b + DM_IPD_W), h = *(int *) (b + DM_IPD_H);
    if (__atomic_load_n(&g_dm_strip_off, __ATOMIC_RELAXED))
        return 0; /* A/B kill switch: settings/azmod-ovstrip.off */
    /* the overviews: 590x44 (2-DECK), 373x38; not the 1220x15 ruler on the waveform
     * view, which is drawn raw */
    return w >= 200 && w <= 1000 && h >= 24 && h <= 64 && w >= 5 * h &&
           *(void **) (b + DM_SPD_DATA);
}
/* A strip's neutral strokes on a light theme. Some overviews are drawn white or near
 * white (a whole overview of (255,255,255) and (238,255,255) after a load), which reads on black and vanishes on a light ground. A premultiplied pixel
 * whose color is unsaturated (saturation under DM_NEUTRAL_SAT) is mixed toward the dark
 * neutral DM_NEUTRAL_INK at its own alpha, fully up to half that; colored strokes (RGB
 * and 3-band bands, the played region's dimmed copies) are untouched. Per pixel, so
 * nothing about it can flicker. */
#define DM_NEUTRAL_SAT 64  /* of 255 */
#define DM_NEUTRAL_INK 48
static uint32_t dm_strip_neutral(uint32_t p) {
    uint32_t a = p >> 24, r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
    uint32_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (!mx)
        return p;
    uint32_t sat = (mx - mn) * 255 / mx;
    if (sat >= DM_NEUTRAL_SAT)
        return p;
    /* weight of 64: full up to half of DM_NEUTRAL_SAT (white and its faint tints), then a
     * ramp to none; the ink is premultiplied by the pixel's alpha */
    uint32_t w = sat <= DM_NEUTRAL_SAT / 2 ? DM_NEUTRAL_SAT : 2 * (DM_NEUTRAL_SAT - sat),
             d = DM_NEUTRAL_INK * a / 255;
    r = (r * (DM_NEUTRAL_SAT - w) + d * w) / DM_NEUTRAL_SAT;
    g = (g * (DM_NEUTRAL_SAT - w) + d * w) / DM_NEUTRAL_SAT;
    b = (b * (DM_NEUTRAL_SAT - w) + d * w) / DM_NEUTRAL_SAT;
    return (a << 24) | (r << 16) | (g << 8) | b;
}
/* Paint thread only (drawImageTransformed). Returns 0: caller draws normally. */
static int dm_draw_additive(void *g, void **img, const void *xf, int fill) {
    if (__atomic_load_n(&g_dm_add_off, __ATOMIC_RELAXED))
        return 0;
    unsigned char *b = *img;
    int strip = dm_is_strip(b);
    /* A strip is drawn as it looks on black, with the theme's ground in place of the
     * black: every pixel's alpha becomes its brightest channel (the BLACK conversion).
     * Overlapping overview bands are opaque, darker premultiplied colors that read as
     * the bright sum on black; the ADDITIVE conversion would leave them opaque and dark.
     * Same input pixel, same output, so it cannot flicker as the layers fill in. */
    int kind = strip ? DM_G_BLACK : dm_ground_of(b);
    if (strip && __atomic_load_n(&g_dm_stripcensus, __ATOMIC_RELAXED) >
                     0) { /* DEV: the ink/ground judgement a strip would get without the BLACK rule */
        static const void *lastb;
        static int lastold = -1;
        uint32_t ink;
        int old = dm_ink_of(b, &ink) ? 9 : dm_ground_of(b);
        if (b != lastb || old != lastold) {
            __atomic_sub_fetch(&g_dm_stripcensus, 1, __ATOMIC_RELAXED);
            struct timespec tq;
            clock_gettime(CLOCK_MONOTONIC, &tq);
            FILE *f = azlog_open("/tmp/azmod-dm-stripcensus.log", "a");
            if (f) {
                fprintf(f, "%ld.%03ld img %p %dx%d old-judgement %s now %s\n", (long) tq.tv_sec,
                        tq.tv_nsec / 1000000, (void *) b, *(int *) (b + DM_IPD_W),
                        *(int *) (b + DM_IPD_H),
                        old == 9               ? "INK"
                        : old == DM_G_BLACK    ? "BLACK"
                        : old == DM_G_ADDITIVE ? "ADDITIVE"
                                               : "raw",
                        kind == DM_G_BLACK ? "BLACK(on-black)" : "ADDITIVE");
                fclose(f);
            }
            lastb = b;
            lastold = old;
        }
    }
    if (!kind)
        return 0;
    int w = *(int *) (b + DM_IPD_W), h = *(int *) (b + DM_IPD_H), ls = *(int *) (b + DM_SPD_LSTR);
    const uint8_t *src = *(const uint8_t **) (b + DM_SPD_DATA);
    /* Only the static thumbnails are cached: an overview changes as the deck plays
     * (its played region grays), which a sampled fingerprint could miss, so it is
     * converted on every draw (26k px) in its own buffer. */
    static struct dm_gc live;
    uint32_t fp = kind == DM_G_BLACK ? dm_fingerprint(src, w, h, ls) : 0;
    struct dm_gc *e = NULL, *old = &g_dm_gc[0];
    if (kind != DM_G_BLACK || strip) {
        old = &live;
        live.ipd = NULL;
    } /* always a miss: overviews change as they play/fill */
    else
        for (int i = 0; i < DM_GC_N; i++) {
            struct dm_gc *c = &g_dm_gc[i];
            if (c->ipd == b && c->data == src && c->w == w && c->h == h && c->kind == kind &&
                c->fp == fp) {
                e = c;
                break;
            }
            if (c->age < old->age)
                old = c;
        }
    if (!e) { /* convert into the oldest slot */
        e = old;
        size_t n = (size_t) w * h;
        if (n > e->cap) {
            uint32_t *k = realloc(e->buf, n * 4);
            if (!k)
                return 0;
            e->buf = k;
            e->cap = n;
        }
        for (int y = 0; y < h; y++) {
            const uint32_t *row = (const uint32_t *) (src + (long) y * ls);
            uint32_t *out = e->buf + (size_t) y * w;
            for (int x = 0; x < w; x++) {
                uint32_t p = row[x], a = p >> 24, r = (p >> 16) & 0xff, gg = (p >> 8) & 0xff,
                         bl = p & 0xff;
                uint32_t m = r > gg ? (r > bl ? r : bl) : (gg > bl ? gg : bl);
                out[x] = (kind == DM_G_BLACK || m > a) ? (m << 24) | (p & 0x00ffffffu) : p;
                if (strip)
                    out[x] = dm_strip_neutral(out[x]);
            }
        }
        e->ipd = b;
        e->data = src;
        e->w = w;
        e->h = h;
        e->kind = kind;
        e->fp = fp;
    }
    e->age = ++g_dm_gc_tick;
    static unsigned char *stand_in;
    if (!stand_in && !(stand_in = aligned_alloc(16, 64 * ((DM_SPD_SIZE + 63) / 64))))
        return 0;
    memcpy(stand_in, b, DM_SPD_SIZE);
    *(int *) (stand_in + DM_IPD_REFCOUNT) = DM_ADD_PIN; /* never reaches 0: never freed */
    *(void **) (stand_in + DM_SPD_DATA) = e->buf;
    *(int *) (stand_in + DM_SPD_LSTR) = w * 4;
    void *stand_in_image = stand_in; /* a juce::Image is one Ptr */
    g_dm_orig_drawimg_t(g, &stand_in_image, xf, fill);
    __atomic_add_fetch(&g_dm_add_draws, 1, __ATOMIC_RELAXED);
    if (__atomic_exchange_n(&g_dm_add_fake, 0,
                            __ATOMIC_RELAXED)) /* DEV: simulate a retained Image */
        __atomic_add_fetch((int *) (stand_in + DM_IPD_REFCOUNT), 1, __ATOMIC_RELAXED);
    int rc = __atomic_load_n((int *) (stand_in + DM_IPD_REFCOUNT), __ATOMIC_RELAXED);
    if (rc != DM_ADD_PIN) { /* something kept a reference */
        __atomic_store_n(&g_dm_add_off, rc, __ATOMIC_RELAXED);
        stand_in = NULL;                    /* abandon it and every cached copy: */
        memset(g_dm_gc, 0, sizeof g_dm_gc); /* never written or freed again */
        memset(&live, 0, sizeof live);
    }
    return 1;
}

/* ---- Runtime ink: small images EP147 renders itself (text cells) -------------
 * Some text is pre-rendered into images at runtime (the deck panel's "USB1" cell
 * 170x41, the empty hot-cue letters 18x16): not decoded art, so dm_flip never sees
 * them, and they are re-rendered, so flipping in place would desync. Instead, when
 * such an image is pure gray/white ink, draw it through JUCE's own alpha fill
 * (drawImageTransformed's fill=true path) with the remapped ink color: nothing
 * mutated, nothing to undo. Only images up to DM_INK_MAX_W wide: the 1220-px beat
 * ruler sits over the dark waveforms. */
#define DM_INK_MAX_W 400
/* Classification cache (paint thread only): 256 direct-mapped entries keyed by the
 * image object, its pixel buffer + size, and a 32-px content fingerprint, since
 * EP147 re-renders text cells into the same buffer (the hot cue letters).
 * Re-sampling every small image on every draw costs measurable paint rate. */
struct dm_inkc {
    void *ipd, *data;
    int w, h, ok;
    uint32_t ink, fp;
};
static struct dm_inkc g_dm_inkc[256];
static int dm_ink_classify(void *ipd, uint32_t *ink_argb);
static int dm_ink_of(void *ipd, uint32_t *ink_argb) {
    unsigned char *b = ipd;
    if (*(uintptr_t *) b != DM_SPD_VTABLE)
        return 0;
    void *data = *(void **) (b + DM_SPD_DATA);
    int w = *(int *) (b + DM_IPD_W), h = *(int *) (b + DM_IPD_H), ls = *(int *) (b + DM_SPD_LSTR);
    if (!data || w <= 0 || h <= 0 || w > DM_INK_MAX_W || ls < w * 4)
        return 0;
    uint32_t fp = 2166136261u;
    for (int k = 0; k < 32; k++) {
        int x = (int) (((uint32_t) k * 2654435761u) % (unsigned) w), y = (k * 7919) % h;
        fp = (fp ^ ((const uint32_t *) ((const uint8_t *) data + (long) y * ls))[x]) * 16777619u;
    }
    struct dm_inkc *c = &g_dm_inkc[(((uintptr_t) ipd >> 4) ^ ((uintptr_t) data >> 6)) & 255];
    if (c->ipd == ipd && c->data == data && c->w == w && c->h == h && c->fp == fp) {
        *ink_argb = c->ink;
        return c->ok;
    }
    uint32_t ink = 0;
    int ok = dm_ink_classify(ipd, &ink);
    c->ipd = ipd;
    c->data = data;
    c->w = w;
    c->h = h;
    c->ok = ok;
    c->ink = ink;
    c->fp = fp;
    *ink_argb = ink;
    return ok;
}
static long dm_ink_opaque_skips; /* telemetry: opaque images kept out of the ink path */
static int dm_ink_classify(void *ipd, uint32_t *ink_argb) {
    unsigned char *b = ipd;
    if (*(uintptr_t *) b != DM_SPD_VTABLE || *(int *) (b + DM_IPD_FORMAT) != 2 ||
        *(int *) (b + DM_SPD_PSTR) != 4)
        return 0;
    int w = *(int *) (b + DM_IPD_W), h = *(int *) (b + DM_IPD_H), ls = *(int *) (b + DM_SPD_LSTR);
    if (w <= 0 || h <= 0 || w > DM_INK_MAX_W)
        return 0;
    unsigned char *d = *(unsigned char **) (b + DM_SPD_DATA);
    long n = 0, sum = 0, cov = 0, tot = 0, gray = 1;
    int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
    long acc[3] = {0, 0, 0}, na = 0;
    for (int y = 0; y < h; y += 2)
        for (int x = 0; x < w; x += 2) {
            unsigned char *p = d + (long) y * ls + x * 4;
            tot++;
            if (p[3] < 64)
                continue;
            cov++;
            if (!dm_is_gray(p))
                gray = 0;
            sum += p[2] * 255 / p[3]; /* un-premultiplied lightness */
            n++;
            if (p[3] >= 160) { /* glyph cores: the ink's own color */
                for (int c = 0; c < 3; c++) {
                    int v = p[c] * 255 / p[3];
                    if (v < mn[c])
                        mn[c] = v;
                    if (v > mx[c])
                        mx[c] = v;
                    acc[c] += v;
                }
                na++;
            }
        }
    if (!n)
        return 0;
    /* An (almost) fully opaque image is a picture, never a text cell: drawn through the
     * alpha fill it would become a flat rectangle of the ink color (e.g. the small
     * no-artwork disc EP147 scales for track rows). */
    if (cov * 100 >= tot * 90) {
        if (++dm_ink_opaque_skips <= 60 &&
            access("/home/root/settings/azmod-logs.on", F_OK) ==
                0) { /* what was kept out (classification is cached) */
            FILE *f = azlog_open("/tmp/azmod-dm-inkskip.log", "a");
            if (f) {
                fprintf(f, "opaque %dx%d gray %ld L %ld\n", w, h, gray, n ? sum / n : 0);
                fclose(f);
            }
        }
        return 0;
    }
    if (gray) {
        int L = (int) (sum / n);
        if (L < 128)
            return 0; /* dark ink already reads on light */
        *ink_argb = dm_map(0xff000000u | (L << 16) | (L << 8) | L);
        return 1;
    }
    /* Colored text cell (the deck tile's orange BPM, WIDE, QUANTIZE, Q ...): one ink
     * color, and text-shaped (strokes cover under 45% of the box). A colored shape
     * (cue triangle, chip) covers more and stays stock, like every fill. */
    if (!na || cov * 100 >= tot * 45)
        return 0;
    for (int c = 0; c < 3; c++)
        if (mx[c] - mn[c] > 24)
            return 0; /* more than one color: art */
    uint32_t col = 0xff000000u | ((uint32_t) (acc[2] / na) << 16) |
                   ((uint32_t) (acc[1] / na) << 8) | (uint32_t) (acc[0] / na);
    uint32_t ink = dm_ink(col);
    if (ink == col)
        return 0; /* already reads at 4.5:1 */
    *ink_argb = ink;
    return 1;
}

static void dm_drawimg_t_body(void *g, void **img, const void *xf, int fill);
static _Atomic int g_dm_ever_on; /* set before g_dm_on first goes to 1, never cleared */
/* An image paints over whatever fill was under it (the deck strips draw a colored
 * fill, then opaque art on top), so it counts as a later "fill" for
 * dm_satfill_is_last, after it is drawn, since its own ink decision looks under it. */
static __thread int
    t_dm_keep_gen; /* this draw restored stock white on its tile: not a new ground */
static void dm_drawimg_t_hook(void *g, void **img, const void *xf, int fill) {
    /* No theme was on since EP147 started: no art is themed, so nothing to restore or look up. */
    if (!__atomic_load_n(&g_dm_ever_on, __ATOMIC_RELAXED)) {
        g_dm_orig_drawimg_t(g, img, xf, fill);
        return;
    }
    t_dm_keep_gen = 0;
    dm_drawimg_t_body(g, img, xf, fill);
    if (!t_dm_keep_gen)
        t_dm_fill_gen++;
}
static void dm_drawimg_t_body(void *g, void **img, const void *xf, int fill) {
    /* azmod's own image that already carries the theme's ink (the Mono stem mark, whose
     * color smk_draw picks itself): drawn as is, never flipped or drawn as an ink cell
     * in the current fill, which could be the row's own ground color. */
    if (t_dm_bypass) {
        g_dm_orig_drawimg_t(g, img, xf, fill);
        return;
    }
    if (img && *img) {
        struct dm_art *e = dm_find_art_live(*img);
        int on = __atomic_load_n(&g_dm_on, __ATOMIC_RELAXED);
        int flip = on && dm_flip_on();
        if (!e && flip && !fill) {
            uint32_t ink;
            if (!dm_is_strip(*img) &&
                dm_ink_of(*img, &ink)) { /* a strip is never an ink cell: dm_draw_additive */
                void **ctx = *(void ***) g;
                void (**vt)(void *) = *(void (***)(void *)) ctx;
                /* a gray text image on a colored fill: black where black reads, as for
                 * glyphs */
                uint32_t ir = (ink >> 16) & 0xff, ig = (ink >> 8) & 0xff, ib = ink & 0xff;
                const float *m = xf;
                if (__atomic_load_n(&g_dm_gcensus, __ATOMIC_RELAXED) > 0 && m) { /* DEV */
                    FILE *f = azlog_open("/tmp/azmod-dm-gcensus.log", "a");
                    if (f) {
                        fprintf(f, "inkimg ctx %p at %.0f,%.0f ink %08x under %08x\n", (void *) ctx,
                                m[2], m[5], ink, dm_satfill_at(ctx, m[2], m[5] + 1));
                        fclose(f);
                    }
                }
                if (m && ir == ig && ig == ib) {
                    uint32_t under = dm_satfill_at(ctx, m[2], m[5] + 1);
                    if (under) {
                        uint32_t o = dm_on_fill(under, ink, ink);
                        if ((o & 0xffffff) != 0xffffff || dm_satfill_is_last(ctx, m[2], m[5] + 1))
                            ink = o;
                    }
                }
                vt[DM_CTX_SAVESTATE](ctx);
                dm_set_color_raw(g, ink);
                g_dm_orig_drawimg_t(g, img, xf, 1);
                vt[DM_CTX_RESTORESTATE](ctx);
                return;
            }
        }
        if (on && __atomic_load_n(&g_dm_stat, __ATOMIC_RELAXED) > 0) { /* DEV census: art too */
            unsigned char *bb = *img;
            int w = *(int *) (bb + DM_IPD_W), h = *(int *) (bb + DM_IPD_H);
            if (w >= 500 && w <= 700 && h >= 10 && h <= 40) {
                __atomic_sub_fetch(&g_dm_stat, 1, __ATOMIC_RELAXED);
                uint32_t px0 = 0, pxc = 0;
                if (*(uintptr_t *) bb == DM_SPD_VTABLE && *(void **) (bb + DM_SPD_DATA)) {
                    const uint8_t *dd = *(const uint8_t **) (bb + DM_SPD_DATA);
                    int ls = *(int *) (bb + DM_SPD_LSTR);
                    px0 = ((const uint32_t *) (dd + (long) (h / 2) * ls))[3];
                    pxc = ((const uint32_t *) (dd + (long) (h / 2) * ls))[w / 2];
                }
                FILE *f = azlog_open("/tmp/azmod-dm-stat.log", "a");
                if (f) {
                    fprintf(f,
                            "img %p %dx%d vt %s fmt %d art %d kind %d flipped %d fill %d px(3,mid) "
                            "%08x px(mid) %08x\n",
                            (void *) bb, w, h, *(uintptr_t *) bb == DM_SPD_VTABLE ? "SPD" : "other",
                            *(int *) (bb + DM_IPD_FORMAT), e != NULL, e ? e->kind : -1,
                            e ? e->flipped : -1, fill, px0, pxc);
                    fclose(f);
                }
            }
        }
        if (!e && on && !fill && __atomic_load_n(&g_dm_stat, __ATOMIC_RELAXED) > 0)
            dm_stat_img(*img); /* DEV census */
        if (!e && flip && !fill && dm_draw_additive(g, img, xf, fill))
            return; /* light themes: dark-ground art (fill=true: alpha is a mask, leave it) */
        int want = __atomic_load_n(&g_dm_on, __ATOMIC_RELAXED);
        if (e && want && !fill && e->kind == DM_K_UNKNOWN) { /* classify now, so ink is known */
            unsigned char *bb = (unsigned char *) *img;
            if (*(void **) (bb + DM_SPD_DATA) == e->data && *(int *) (bb + DM_IPD_W) == e->w &&
                *(int *) (bb + DM_IPD_H) == e->h)
                dm_classify(e, bb);
        }
        if (e && want && flip && !fill && e->kind == DM_K_COLOR && e->ink &&
            !e->perpixel) { /* colored text art: inked */
            void **ctx = *(void ***) g;
            void (**vt)(void *) = *(void (***)(void *)) ctx;
            vt[DM_CTX_SAVESTATE](ctx);
            dm_set_color_raw(g, e->ink);
            g_dm_orig_drawimg_t(g, img, xf, 1);
            vt[DM_CTX_RESTORESTATE](ctx);
            return;
        }
        if (e && want && flip && !fill && e->grayonly && xf &&
            !e->perpixel) { /* gray art on a colored fill: black */
            void **ctx = *(void ***) g;
            const float *m = xf;
            uint32_t under = dm_satfill_at(ctx, m[2], m[5] + 1);
            int last = under && dm_satfill_is_last(ctx, m[2], m[5] + 1);
            if (__atomic_load_n(&g_dm_gcensus, __ATOMIC_RELAXED) > 0) { /* DEV */
                FILE *f = azlog_open("/tmp/azmod-dm-gcensus.log", "a");
                if (f) {
                    fprintf(f, "grayart %dx%d ctx %p at %.0f,%.0f under %08x last %d\n", e->w, e->h,
                            (void *) ctx, m[2], m[5], under, last);
                    fclose(f);
                }
            }
            uint32_t ink = 0;
            if (under && dm_contrast(0.0, dm_lum_u32(under)) >= 4.5)
                ink = 0xff000000u;
            /* Dark colored fill where black does not read (the SOURCE list's navy device
             * tiles): keep the stock white, as dm_on_fill does for text, and only when that
             * fill is the last thing drawn there. The tile's label is drawn next onto the
             * same tile, so t_dm_keep_gen tells the hook not to count this as a new ground. */
            else if (last && dm_contrast(1.0, dm_lum_u32(under)) >= 4.5) {
                ink = 0xffffffffu;
                t_dm_keep_gen = 1;
            }
            if (ink) {
                void (**vt)(void *) = *(void (***)(void *)) ctx;
                vt[DM_CTX_SAVESTATE](ctx);
                dm_set_color_raw(g, ink);
                g_dm_orig_drawimg_t(g, img, xf, 1);
                vt[DM_CTX_RESTORESTATE](ctx);
                return;
            }
        }
        /* The art's state is (flipped, th): restore whenever it was themed for anything
         * but the wanted theme (off, or another theme), then theme it for the wanted one.
         * Restoring never depends on the current theme (dm_restore). */
        int th = __atomic_load_n(&g_dm_theme, __ATOMIC_RELAXED);
        if (e && e->kind == DM_K_UNKNOWN &&
            want) { /* classify before deciding: the no-artwork disc is never themed */
            unsigned char *bb = (unsigned char *) *img;
            if (*(void **) (bb + DM_SPD_DATA) == e->data && *(int *) (bb + DM_IPD_W) == e->w &&
                *(int *) (bb + DM_IPD_H) == e->h)
                dm_classify(e, bb);
        }
        if (e && e->noart && !e->flipped)
            e = NULL; /* stock bytes, drawn as they are */
        if (e && (e->flipped ? (!want || e->th != th || e->noart) : want)) {
            unsigned char *b = (unsigned char *) *img;
            if (*(void **) (b + DM_SPD_DATA) != e->data || *(int *) (b + DM_IPD_W) != e->w ||
                *(int *) (b + DM_IPD_H) != e->h) {
                __atomic_store_n(&e->ipd, (void *) 1,
                                 __ATOMIC_RELEASE); /* reused memory: tombstone, never touch */
            } else {
                dm_restore(e, b);
                if (want && !e->noart) {
                    if (e->kind == DM_K_UNKNOWN)
                        dm_classify(e, b);
                    if (!dm_flip_on() || dm_art_daykind(e) == DM_K_GRAY || e->perpixel) {
                        dm_flip(e, b);
                        if (e->flipped)
                            e->th = th;
                    }
                }
            }
        }
    }
    g_dm_orig_drawimg_t(g, img, xf, fill);
}

/* getFromMemory returns its Image through x8 (the C++ indirect result), which a
 * C hook cannot see: this wrapper keeps the slot, calls the original with every
 * argument register untouched, then records the result. */
/* GCC 7 (the azmod-build toolchain) ignores `naked` on aarch64, so the wrapper is
 * a top-level asm function; hidden visibility keeps its PC-relative references
 * legal in a shared object. */
__attribute__((visibility("hidden"))) void *g_dm_orig_getfromfile;
__attribute__((visibility("hidden"))) void dm_getfromfile_hook(void);
__asm__(".text\n"
        ".p2align 2\n"
        ".hidden dm_getfromfile_hook\n"
        ".type dm_getfromfile_hook, %function\n"
        "dm_getfromfile_hook:\n"
        "    stp x29, x30, [sp, #-32]!\n"
        "    mov x29, sp\n"
        "    str x8, [sp, #16]\n"
        "    adrp x9, g_dm_orig_getfromfile\n"
        "    ldr x9, [x9, :lo12:g_dm_orig_getfromfile]\n"
        "    blr x9\n"
        "    ldr x0, [sp, #16]\n"
        "    bl dm_record_art\n"
        "    ldp x29, x30, [sp], #32\n"
        "    ret\n"
        ".size dm_getfromfile_hook, .-dm_getfromfile_hook\n");
__attribute__((visibility("hidden"))) void *g_dm_orig_getfrommem;
__attribute__((visibility("hidden"))) void dm_getfrommem_hook(void);
__asm__(".text\n"
        ".p2align 2\n"
        ".hidden dm_getfrommem_hook\n"
        ".type dm_getfrommem_hook, %function\n"
        "dm_getfrommem_hook:\n"
        "    stp x29, x30, [sp, #-32]!\n"
        "    mov x29, sp\n"
        "    str x8, [sp, #16]\n"
        "    adrp x9, g_dm_orig_getfrommem\n"
        "    ldr x9, [x9, :lo12:g_dm_orig_getfrommem]\n"
        "    blr x9\n"
        "    ldr x0, [sp, #16]\n"
        "    bl dm_record_art\n"
        "    ldp x29, x30, [sp], #32\n"
        "    ret\n"
        ".size dm_getfrommem_hook, .-dm_getfrommem_hook\n");

/* ---- Waveform background: EP147 rasterizes from a remapped copy -------------
 * EP147's column array is never written (it may be cached and redrawn), so
 * switching off is exact. One copy per thread, 34 KB, only while a theme is on. */
static const unsigned char *dm_wf_columns_at(const unsigned char *cols, int bg_off) {
    static __thread unsigned char *buf;
    if (!__atomic_load_n(&g_dm_on, __ATOMIC_RELAXED) || !cols)
        return cols;
    if (!buf && !(buf = malloc(DM_WF_COLS * DM_WF_COL_SIZE)))
        return cols;
    memcpy(buf, cols, DM_WF_COLS * DM_WF_COL_SIZE);
    for (int i = 0; i < DM_WF_COLS; i++) {
        unsigned char *c = buf + i * DM_WF_COL_SIZE + bg_off; /* B, G, R */
        uint32_t m =
            dm_map_gray(0xff000000u | ((uint32_t) c[2] << 16) | ((uint32_t) c[1] << 8) | c[0]);
        c[0] = m & 0xff;
        c[1] = (m >> 8) & 0xff;
        c[2] = (m >> 16) & 0xff;
    }
    return buf;
}
__attribute__((visibility("hidden"))) const unsigned char *
dm_wf_columns(const unsigned char *cols) {
    return dm_wf_columns_at(cols, DM_WF_COL_BG);
}
/* 3BAND: layered cells, and the black ground sits in more than one of them (at least
 * +0xc and +0x18), so remapping one cell leaves black stripes. All seven cells
 * {B,G,R,height} get the gray map: the black ground turns light, the white high band
 * turns dark, and the blue/orange band colors pass through. A remapped copy, as for
 * RGB. */
__attribute__((visibility("hidden"))) const unsigned char *
dm_wf3_columns(const unsigned char *cols) {
    static __thread unsigned char *buf;
    if (!__atomic_load_n(&g_dm_on, __ATOMIC_RELAXED) || !cols)
        return cols;
    if (!buf && !(buf = malloc(DM_WF_COLS * DM_WF_COL_SIZE)))
        return cols;
    memcpy(buf, cols, DM_WF_COLS * DM_WF_COL_SIZE);
    for (int i = 0; i < DM_WF_COLS; i++) {
        for (int k = 0; k < DM_WF_COL_SIZE; k += 4) {
            unsigned char *c = buf + i * DM_WF_COL_SIZE + k; /* B, G, R, height */
            uint32_t m =
                dm_map_gray(0xff000000u | ((uint32_t) c[2] << 16) | ((uint32_t) c[1] << 8) | c[0]);
            c[0] = m & 0xff;
            c[1] = (m >> 8) & 0xff;
            c[2] = (m >> 16) & 0xff;
        }
    }
    return buf;
}
/* The rulers (makeGridImage): azmod already hooks that function for Bright Grid
 * (gridmake_hook, waveform.c), so it calls these around the original instead of a
 * second hook stacking on the same entry. The background color is swapped for
 * the duration of the call only; EP147's object is unchanged afterwards. With the
 * Bright Grid kill switch (settings/azmod-gridaa.off) the rulers stay dark. */
static int dm_ruler_begin(void *gm, uint32_t *keep) {
    if (!__atomic_load_n(&g_dm_on, __ATOMIC_RELAXED) || !gm)
        return 0;
    uint32_t *bg = (uint32_t *) ((unsigned char *) gm + DM_RULER_BG);
    *keep = *bg;
    *bg = dm_map(*keep);
    return 1;
}
/* The gray beat ticks are pixels written inside the call (light gray: fine on black,
 * faint on white), so they are flipped in the finished image: every gray pixel that
 * is not the background, premultiplied v' = a - v. The image is rebuilt from scratch
 * on every call (row 0 filled, then copied down), so this never double-flips.
 * Layout as gridmake_hook reads it: gm+0x30 BitmapData {data, fmt, lineStride,
 * pixelStride, width, height}. Rows repeat, so each distinct row is done once.
 * Returns the background the image was filled with, so Bright Grid leaves it alone. */
static uint32_t dm_ruler_end(void *gm, uint32_t keep) {
    unsigned char *b = gm;
    uint32_t bg = *(uint32_t *) (b + DM_RULER_BG); /* the swapped-in background */
    *(uint32_t *) (b + DM_RULER_BG) = keep;
    if (!dm_flip_on())
        return bg; /* dark themes: light ticks already read */
    uint8_t *data = *(uint8_t **) (b + 0x30);
    int fmt = *(int *) (b + 0x38), ls = *(int *) (b + 0x3c), ps = *(int *) (b + 0x40);
    int w = *(int *) (b + 0x44), h = *(int *) (b + 0x48);
    if (!data || fmt != 2 || ps != 4 || w < 1 || w > 4096 || h < 1 || h > 256 || ls < w * 4)
        return bg;
    static uint32_t prev_orig[4096], prev_new[4096];
    size_t rb = (size_t) w * 4;
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *) (data + (long) y * ls);
        if (y > 0 && memcmp(row, prev_orig, rb) == 0) {
            memcpy(row, prev_new, rb);
            continue;
        }
        memcpy(prev_orig, row, rb);
        for (int x = 0; x < w; x++) {
            uint32_t p = row[x];
            if (p == bg)
                continue;
            uint32_t a = p >> 24, r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, bl = p & 0xff;
            uint32_t mx = r > g ? (r > bl ? r : bl) : (g > bl ? g : bl);
            uint32_t mn = r < g ? (r < bl ? r : bl) : (g < bl ? g : bl);
            if (mx - mn > 8 || r > a || g > a || bl > a)
                continue; /* colored tick, or not premultiplied */
            row[x] = (a << 24) | ((a - r) << 16) | ((a - g) << 8) | (a - bl);
        }
        memcpy(prev_new, row, rb);
    }
    return bg;
}
/* Tail-calls the original with x1 swapped and x0..x8 otherwise intact (x8 carries
 * the returned Image), so the original returns straight to EP147's caller. */
__attribute__((visibility("hidden"))) void *g_dm_orig_wfraster;
__attribute__((visibility("hidden"))) void dm_wfraster_hook(void);
__asm__(".text\n"
        ".p2align 2\n"
        ".hidden dm_wfraster_hook\n"
        ".type dm_wfraster_hook, %function\n"
        "dm_wfraster_hook:\n"
        "    stp x29, x30, [sp, #-96]!\n"
        "    mov x29, sp\n"
        "    stp x0, x1, [sp, #16]\n"
        "    stp x2, x3, [sp, #32]\n"
        "    stp x4, x5, [sp, #48]\n"
        "    stp x6, x7, [sp, #64]\n"
        "    str x8, [sp, #80]\n"
        "    mov x0, x1\n"
        "    bl dm_wf_columns\n"
        "    mov x9, x0\n"
        "    ldp x0, x1, [sp, #16]\n"
        "    ldp x2, x3, [sp, #32]\n"
        "    ldp x4, x5, [sp, #48]\n"
        "    ldp x6, x7, [sp, #64]\n"
        "    ldr x8, [sp, #80]\n"
        "    mov x1, x9\n"
        "    ldp x29, x30, [sp], #96\n"
        "    adrp x10, g_dm_orig_wfraster\n"
        "    ldr x10, [x10, :lo12:g_dm_orig_wfraster]\n"
        "    br x10\n"
        ".size dm_wfraster_hook, .-dm_wfraster_hook\n");
__attribute__((visibility("hidden"))) void *g_dm_orig_wf3raster;
__attribute__((visibility("hidden"))) void dm_wf3raster_hook(void);
__asm__(".text\n"
        ".p2align 2\n"
        ".hidden dm_wf3raster_hook\n"
        ".type dm_wf3raster_hook, %function\n"
        "dm_wf3raster_hook:\n"
        "    stp x29, x30, [sp, #-96]!\n"
        "    mov x29, sp\n"
        "    stp x0, x1, [sp, #16]\n"
        "    stp x2, x3, [sp, #32]\n"
        "    stp x4, x5, [sp, #48]\n"
        "    stp x6, x7, [sp, #64]\n"
        "    str x8, [sp, #80]\n"
        "    mov x0, x1\n"
        "    bl dm_wf3_columns\n"
        "    mov x9, x0\n"
        "    ldp x0, x1, [sp, #16]\n"
        "    ldp x2, x3, [sp, #32]\n"
        "    ldp x4, x5, [sp, #48]\n"
        "    ldp x6, x7, [sp, #64]\n"
        "    ldr x8, [sp, #80]\n"
        "    mov x1, x9\n"
        "    ldp x29, x30, [sp], #96\n"
        "    adrp x10, g_dm_orig_wf3raster\n"
        "    ldr x10, [x10, :lo12:g_dm_orig_wf3raster]\n"
        "    br x10\n"
        ".size dm_wf3raster_hook, .-dm_wf3raster_hook\n");

/* Constructor-time half: must run before EP147 decodes its art. */
static void install_daymode_early(void) {
    if (!daymode_wanted())
        return; /* kill switch: no hook (azmod.c) */
    int rc = azm_install_hook(DM_IMAGECACHE_GETFROMMEMORY, (void *) dm_getfrommem_hook,
                              &g_dm_orig_getfrommem);
    int rcf = azm_install_hook(DM_IMAGECACHE_GETFROMFILE, (void *) dm_getfromfile_hook,
                               &g_dm_orig_getfromfile);
    if (rcf != 1)
        rc = rc * 10 + rcf; /* one log field: 1 = both installed */
    FILE *f = azlog_open("/tmp/azmod-daymode.log", "a");
    if (f) {
        fprintf(f, "install daymode early: ImageCache getFromMemory + getFromFile rc=%d\n", rc);
        fclose(f);
    }
}

/* EP147's own top-level X window: the root child whose _NET_WM_PID is us. */
static Window dm_find_ep147_window(Display *d) {
    Atom pid_atom = XInternAtom(d, "_NET_WM_PID", True);
    if (pid_atom == None)
        return 0;
    Window r, p, *ch = NULL, best = 0;
    unsigned n = 0;
    if (!XQueryTree(d, DefaultRootWindow(d), &r, &p, &ch, &n))
        return 0;
    for (unsigned i = 0; i < n && !best; i++) {
        Atom type;
        int fmt;
        unsigned long nitems, after;
        unsigned char *prop = NULL;
        if (XGetWindowProperty(d, ch[i], pid_atom, 0, 1, False, XA_CARDINAL, &type, &fmt, &nitems,
                               &after, &prop) == Success &&
            prop) {
            if (nitems == 1 && *(unsigned long *) prop == (unsigned long) getpid()) {
                XWindowAttributes wa;
                if (XGetWindowAttributes(d, ch[i], &wa) && wa.map_state == IsViewable &&
                    wa.width >= 600)
                    best = ch[i];
            }
            XFree(prop);
        }
    }
    if (ch)
        XFree(ch);
    return best;
}

/* Watcher: polls DAYMODE_ON_PATH twice a second; on a change flips g_dm_on and
 * re-exposes EP147's whole window so JUCE repaints everything with the new
 * palette. Its own X connection, never EP147's; no EP147 locks taken. */
static void *dm_watch(void *arg) {
    (void) arg;
    Display *d = NULL;
    Window w = 0;
    for (;;) {
        /* the Mod Menu row, or the file for scripted tests */
        int want = (az_on(AZF_THEMES) &&
                    az_active(AZF_THEMES)) ||
                   access(DAYMODE_ON_PATH, F_OK) == 0;
        if (access(DAYMODE_OFF_PATH, F_OK) == 0)
            want = 0; /* dev: force off (A/B measurements) */
        { /* the theme: g_azmod_theme names one (day if unknown). A change while on goes
           * off for one tick so art re-themes as it is next drawn. */
            int th = 0;
            const char *const *keys;
            int nk = azmod_theme_keys(&keys), cur = __atomic_load_n(&g_azmod_theme, __ATOMIC_RELAXED);
            if (cur >= 0 && cur < nk)
                for (int k = 0; k < DM_NTHEMES; k++)
                    if (!strcmp(keys[cur], g_dm_themes[k].name))
                        th = k;
            if (th != __atomic_load_n(&g_dm_theme, __ATOMIC_RELAXED)) {
                if (__atomic_load_n(&g_dm_on, __ATOMIC_RELAXED))
                    want = 0; /* off first; on again next tick */
                else
                    __atomic_store_n(&g_dm_theme, th, __ATOMIC_RELAXED);
            }
        }
        if (want != __atomic_load_n(&g_dm_on, __ATOMIC_RELAXED)) {
            if (want)
                __atomic_store_n(&g_dm_ever_on, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&g_dm_flip, want && dm_flip_on(), __ATOMIC_RELAXED);
            __atomic_store_n(&g_dm_on, want, __ATOMIC_RELAXED);
            __atomic_add_fetch(&g_dm_gen, 1,
                               __ATOMIC_RELAXED); /* every component cache re-renders once */
            if (!d)
                d = XOpenDisplay(NULL);
            if (d && !w)
                w = dm_find_ep147_window(d);
            if (d && w) {
                XClearArea(d, w, 0, 0, 0, 0, True);
                XFlush(d);
            }
            FILE *f = azlog_open("/tmp/azmod-daymode.log", "a");
            if (f) {
                fprintf(f,
                        "daymode %s theme %s (window 0x%lx, setFill calls so far %ld, art images "
                        "recorded %d)\n",
                        want ? "ON" : "OFF", dm_th()->name, (unsigned long) w,
                        (long) __atomic_load_n(&g_dm_calls, __ATOMIC_RELAXED),
                        __atomic_load_n(&g_dm_art_n, __ATOMIC_RELAXED));
                fclose(f);
            }
            f = azlog_open("/tmp/azmod-daymode.log", "a");
            if (f) {
                fprintf(f, "  additive draws %ld, retained-reference refcount %d (0 = none ever)\n",
                        (long) __atomic_load_n(&g_dm_add_draws, __ATOMIC_RELAXED),
                        __atomic_load_n(&g_dm_add_off, __ATOMIC_RELAXED));
                fclose(f);
            }
        }
        if (AZ_PROBE_ON("/tmp/azmod-dm-repaint")) { /* DEV control: the switch's repaint, no switch */
            unlink("/tmp/azmod-dm-repaint");
            if (!d)
                d = XOpenDisplay(NULL);
            if (d && !w)
                w = dm_find_ep147_window(d);
            if (d && w) {
                XClearArea(d, w, 0, 0, 0, 0, True);
                XFlush(d);
            }
        }
        if (AZ_PROBE_ON("/tmp/azmod-dm-stat")) {
            unlink("/tmp/azmod-dm-stat");
            __atomic_store_n(&g_dm_stat, 120, __ATOMIC_RELAXED);
        }
        __atomic_store_n(&g_dm_strip_off,
                         access("/home/root/settings/azmod-ovstrip.off", F_OK) == 0,
                         __ATOMIC_RELAXED);
        if (AZ_PROBE_ON("/tmp/azmod-dm-stripcensus")) {
            unlink("/tmp/azmod-dm-stripcensus");
            __atomic_store_n(&g_dm_stripcensus, 400, __ATOMIC_RELAXED);
        }
        if (AZ_PROBE_ON("/tmp/azmod-dm-gcensus")) {
            unlink("/tmp/azmod-dm-gcensus");
            __atomic_store_n(&g_dm_gcensus, 3000, __ATOMIC_RELAXED);
        }
        if (AZ_PROBE_ON("/tmp/azmod-dm-fakeretain")) {
            unlink("/tmp/azmod-dm-fakeretain");
            __atomic_store_n(&g_dm_add_fake, 1, __ATOMIC_RELAXED);
        }
        usleep(500 * 1000);
    }
    return NULL;
}

static void install_daymode(void) {
    if (!daymode_wanted())
        return; /* kill switch: no hook (azmod.c) */
    int rc1 = dm_hook_setfill();
    int rcg = dm_hook_drawglyph();
    if (rcg == 1) {
        int fi = dm_patch_slot(DM_SWR_FILLRECT_I_OFF, DM_SWR_FILLRECT_I,
                               (void *) dm_fillrect_i_hook, (void **) &g_dm_orig_fillrect_i);
        int ff = dm_patch_slot(DM_SWR_FILLRECT_F_OFF, DM_SWR_FILLRECT_F,
                               (void *) dm_fillrect_f_hook, (void **) &g_dm_orig_fillrect_f);
        int fp = dm_patch_slot(DM_SWR_FILLPATH_OFF, DM_SWR_FILLPATH, (void *) dm_fillpath_hook,
                               (void **) &g_dm_orig_fillpath);
        if (fi != 1 || ff != 1 || fp != 1)
            rcg = 1000 + fi * 100 + ff * 10 + fp;
    }
    if (rcg != 1)
        rc1 = rc1 * 10 + rcg; /* one log field: 1 = setFill and drawGlyph both */
    int rc4 = azm_install_hook(DM_WF_RASTER, (void *) dm_wfraster_hook, &g_dm_orig_wfraster);
    int rc43 = azm_install_hook(DM_WF3_RASTER, (void *) dm_wf3raster_hook, &g_dm_orig_wf3raster);
    if (rc43 != 1)
        rc4 = rc4 * 10 + rc43; /* one log field: 1 = RGB and 3BAND rasterizers */
    int rc3 = azm_install_hook(DM_GFX_DRAWIMAGE_T, (void *) dm_drawimg_t_hook,
                               (void **) &g_dm_orig_drawimg_t);
    {
        FILE *f3 = azlog_open("/tmp/azmod-daymode.log", "a");
        if (f3) {
            fprintf(f3, "waveform rasteriser rc=%d, drawImageTransformed hook rc=%d\n", rc4, rc3);
            fclose(f3);
        }
    }
    FILE *f = azlog_open("/tmp/azmod-daymode.log", "a");
    if (f) {
        fprintf(f, "install daymode (live, gray flip): renderer setFill rc=%d\n", rc1);
        fclose(f);
    }
    pthread_t t;
    if (az_thread_create(&t, NULL, dm_watch, NULL) == 0) /* t is unset when it fails */
        pthread_detach(t);
}

#else
static void install_daymode(void) {
}
static void install_daymode_early(void) {
}
static void dm_scci_pre(void *self) {
    (void) self;
}
static int dm_ruler_begin(void *gm, uint32_t *keep) {
    (void) gm;
    (void) keep;
    return 0;
}
static uint32_t dm_ruler_end(void *gm, uint32_t keep) {
    (void) gm;
    (void) keep;
    return 0;
}
#endif
