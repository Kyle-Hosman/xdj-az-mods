/* SPDX-License-Identifier: GPL-3.0-only */
/* The Mod Menu as a user meets it, dumped for a set of azmod.cfg files: the settings each
 * file loads to, and every screen a knob walk reaches (full-screen menu: every left entry
 * and every row of its pane; the preview card; the compact list), plus what one knob press
 * on each row does to the settings, and what a tap on each left-list checkbox does.
 *
 * Each frame is recorded as the text drawn (position, size, color, string: azmod_ttf_menu is
 * captured) and a hash of every PIXEL drawn (boxes, fills, icons, chevrons). The dump is
 * compared byte for byte against a golden produced by the code before the feature-table
 * refactor (tools/unit-tests/menudump.golden-*), so it is a behavioral proof that does not
 * depend on how the menu is implemented: only the public entry points are driven
 * (azmod_input / azmod_back / azmod_composite / azmod_menu_reset), and the settings are
 * loaded through the real start-up path.
 *
 * Old/new adapters: the few lines that name the settings storage are under MENUDUMP_NEW.
 * argv[1]: scratch path for the cfg file. Output on stdout. */
/* the zone list without offsets: no dependence on the host's zone files or the date */
#define AZ_ZONEINFO "/nonexistent-zoneinfo"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include MENU_C

/* ---- captured text ---------------------------------------------------------------- */
static char g_txt[1 << 18];
static size_t g_txt_n;
static void txt_add(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_txt + g_txt_n, sizeof g_txt - g_txt_n, fmt, ap);
    va_end(ap);
    if (n > 0 && g_txt_n + (size_t) n < sizeof g_txt)
        g_txt_n += (size_t) n;
}
/* Text whose box lies wholly outside the buffer is never seen, so it is not part of a frame: a
 * pane taller than the screen may draw its rows past the bottom edge, or skip them. */
static int txt_in_buffer(const azmod_fb *fb, int y, int h) {
    return !fb || (y < fb->h && y + (h > 0 ? h : 1) > 0);
}
int azmod_ttf_menu(const azmod_fb *fb, int x, int y, int w, int h, const char *s, int px, int track,
                   int bold, uint32_t c) {
    (void) track;
    if (!txt_in_buffer(fb, y, h))
        return 1;
    txt_add("  T %d,%d %dx%d px%d b%d #%06x \"%s\"\n", x, y, w, h, px, bold, c & 0xffffffu, s);
    return 1;
}
int azmod_ttf_menu2(const azmod_fb *fb, int x, int y, int w, int h, const char *s, int px, int track,
                    int bold, uint32_t c, int split, uint32_t c2) {
    (void) c2;
    if (!txt_in_buffer(fb, y, h))
        return 1;
    txt_add("  T2 split%d ", split);
    return azmod_ttf_menu(fb, x, y, w, h, s, px, track, bold, c);
}

/* ---- the settings loader's collaborators (azmod.c) ---------------------------------- */
static char cfg_path[1024], conf_path[1024], settings_dir[1024];
#define CONFIG_PATH cfg_path
#define CONF_PATH conf_path
#define SETTINGS_DIR settings_dir
static int g_disp60_armed;
static void dbg(const char *fmt, ...) { (void) fmt; }
static int ui60_clock_wanted(const azmod_state *s) {
    (void) s;
    return 0;
}
#ifndef MENUDUMP_NEW
static void pledit_apply(const azmod_state *s) { (void) s; }
#endif
static void save_config(const azmod_state *s);
#ifdef MENUDUMP_NEW
#include "azconf.h"
#endif
#include CFG_INC

static azmod_fb g_fb; /* below */
/* ---- adapters: the only lines that name the settings storage ------------------------ */
#define NSLOTS 39 /* azmod.cfg line 1 as 1.2.2 writes it */
/* Retired slots (no feature after the refactor; 1-3 forced off): printed as R in both. */
static int retired_slot(int i) {
    return i == 1 || i == 2 || i == 3 || i == 12 || i == 20 || i == 22 || i == 23 || i == 26;
}
#ifndef MENUDUMP_NEW
static int slot_value(const azmod_state *s, int i) { return s->mod_enabled[i]; }
static unsigned long long pins_of(const azmod_state *s) { return s->pinned_mask; }
/* mod_menu_overlay.c, first overlay tick after the load (before any menu frame): the bank
 * radio with no bank on selects Bank 1 (and saves). */
static void overlay_first_tick(azmod_state *s) {
    int bank = 0;
    for (int b = 0; b < AZMOD_DRUM_BANKS; b++)
        if (s->mod_enabled[AZMOD_ACTION_DRUMB1 + b]) {
            bank = b + 1;
            break;
        }
    if (!bank) s->mod_enabled[AZMOD_ACTION_DRUMB1] = 1;
}
#define LEFT_RESET() (g_last_left = -1, g_rscroll = 0)
static void before_load(void) {}
/* what handle_action (mod_menu_overlay.c) reacts to */
static const char *act_name(int a) {
    return a < 0 ? "none" : a == AZMOD_ACTION_RELOAD ? "reload"
                        : a == AZMOD_ACTION_SYSINFO  ? "sysinfo"
                        : a == AZMOD_ACTION_UPDATE   ? "update"
                        : a == AZMOD_ACTION_STEMS    ? "stems"
                                                     : "row";
}
static void bank_rect(int b, int *x, int *y, int *w, int *h) { azmod_item_rect(&g_fb, AZMOD_ACTION_DRUMB1 + b, x, y, w, h); }
/* mod_menu_overlay.c at start: the rows azmod.c shows on a user deck */
#define START_VISIBILITY() (azmod_set_phasemeter_visible(true), azmod_set_daymode_visible(true))
#else
#include MENUDUMP_NEW
#endif

static void load(const char *content, azmod_state *s) {
    before_load();
    if (content) {
        FILE *f = fopen(cfg_path, "wb");
        fputs(content, f);
        fclose(f);
    } else
        unlink(cfg_path);
    azmod_state_init(s);
    load_config(s);
    overlay_first_tick(s);
}
/* screens by name: their numbers are not part of the behavior */
static const char *mode_name(int m) {
    switch (m) {
    case AZ_CLOSED: return "closed";
    case AZ_OPEN: return "open";
    case AZ_INFO: return "info";
    case AZ_COLORS: return "colors";
    case AZ_COLOR_PICK: return "colorpick";
    case AZ_UPDATE: return "update";
    case AZ_OVERLAYS: return "overlays";
    case AZ_COMMUNITY: return "community";
#ifdef AZMOD_HAVE_CLOCK_TZ_LIST /* the Clock's zone list: likewise */
    case AZ_CLOCK_TZ: return "clocktz";
#endif
#ifdef AZMOD_HAVE_CLOCK_SCREEN /* the Clock's own screen: likewise */
    case AZ_CLOCK: return "clock";
#endif
#ifdef AZMOD_HAVE_CLOCK_SET /* Set Time Manually: likewise */
    case AZ_CLOCK_SET: return "clockset";
#endif
#ifdef AZMOD_HAVE_BROWSE_VIEW_SCREEN /* Browse View Mods' screen: likewise */
    case AZ_BROWSE_VIEW: return "browseview";
#endif
    default: return "?";
    }
}
static void dump_state(const char *what, const azmod_state *s) {
    printf("  %s: ", what);
    for (int i = 0; i < NSLOTS; i++)
        putchar(retired_slot(i) ? 'R' : slot_value(s, i) ? '1'
                                                         : '0');
    printf(" pins=%llu pad=%d", pins_of(s), s->pad_colors_on);
    for (int j = 0; j < AZMOD_PADMODE_COUNT; j++)
        printf(" %d/%d", s->padmode_color[j], s->padmode_color2[j]);
    printf(" mode=%s pane=%d act=%s dirty=%d\n", mode_name((int) s->mode), s->pane, act_name(s->last_action), s->config_dirty);
}

/* ---- frames --------------------------------------------------------------------------- */
#define W 1280
#define H 800
static uint8_t g_px[W * H * 4];
static azmod_fb g_fb = {g_px, W, H, W * 4};
static unsigned long long fnv(const uint8_t *p, size_t n) {
    unsigned long long h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}
/* The text of a frame in a fixed order: the order rows are drawn in is not behavior (text
 * never overlaps; each line carries its position), and the table draws in display order where
 * 1.2.2 drew in mod-number order. */
static int cmp_line(const void *a, const void *b) { return strcmp(*(char *const *) a, *(char *const *) b); }
static void sort_lines(char *t, size_t n) {
    static char copy[sizeof g_txt];
    static char *line[8192];
    int k = 0;
    memcpy(copy, t, n + 1);
    for (char *p = copy; *p && k < 8192;) {
        char *e = strchr(p, '\n');
        line[k++] = p;
        *e = 0;
        p = e + 1;
    }
    qsort(line, (size_t) k, sizeof line[0], cmp_line);
    size_t o = 0;
    for (int i = 0; i < k; i++)
        o += (size_t) sprintf(t + o, "%s\n", line[i]);
}
static void frame(const char *what, azmod_state *s) {
    memset(g_px, 0, sizeof g_px);
    g_txt_n = 0;
    g_txt[0] = 0;
    azmod_composite(s, &g_fb);
    sort_lines(g_txt, g_txt_n);
    printf(" FRAME %s mode=%s pane=%d px=%016llx text=%016llx\n", what, mode_name((int) s->mode), s->pane,
           fnv(g_px, sizeof g_px), fnv((const uint8_t *) g_txt, g_txt_n));
    /* the text as a set difference against the previous frame: compact, and readable in a diff */
    static char prev[sizeof g_txt];
    for (const char *l = g_txt; *l;) {
        const char *e = strchr(l, '\n');
        size_t n = (size_t) (e - l) + 1;
        char key[1024];
        if (n >= sizeof key) n = sizeof key - 1;
        memcpy(key, l, n);
        key[n] = 0;
        if (!strstr(prev, key)) printf("+%s", key + 1);
        l = e + 1;
    }
    for (const char *l = prev; *l;) {
        const char *e = strchr(l, '\n');
        size_t n = (size_t) (e - l) + 1;
        char key[1024];
        if (n >= sizeof key) n = sizeof key - 1;
        memcpy(key, l, n);
        key[n] = 0;
        if (!strstr(g_txt, key)) printf("-%s", key + 1);
        l = e + 1;
    }
    memcpy(prev, g_txt, g_txt_n + 1);
}
/* the lists' glides run on a clock the dump steps: each frame is a knob step's resting place */
static long g_glide_now = 1000;
static long glide_clock(void) { return g_glide_now; }
static void knob(azmod_state *s, int d, bool click) {
#ifdef AZMOD_HAVE_MENU_GLIDE
    g_glide_ms = glide_clock;
#endif
    azmod_input(s, &g_fb, 0, 0, false, d, click);
#ifdef AZMOD_HAVE_MENU_GLIDE
    for (int k = 0; k < 600 && (g_lgl.on || g_rgl.on); k++) {
        g_glide_now += 16;
        c_glide_tick(s);
    }
#endif
}

/* the full-screen menu, as the knob walks it */
static void walk_full(azmod_state *s) {
    azmod_set_docked(true);
    s->menu_focused = 1;
    s->mode = AZ_OPEN;
    LEFT_RESET();
    azmod_menu_reset(s);
    int first = s->left_sel;
    for (int k = 0; k < 64; k++) {
        char nm[32];
        snprintf(nm, sizeof nm, "full.left%d", k);
        frame(nm, s);
        if (s->mode == AZ_OPEN || s->mode == AZ_OVERLAYS) {
            int left = s->left_sel;
            knob(s, 0, true); /* into the pane */
            int r0 = s->menu_sel;
            for (int j = 0; j < 32; j++) {
                snprintf(nm, sizeof nm, "full.left%d.row%d", k, j);
                frame(nm, s);
                knob(s, 1, false);
                if (s->menu_sel == r0) break;
            }
            azmod_back(s); /* back to the list */
            if (s->left_sel != left || s->pane != 0) printf("  !! back did not return to the list\n");
        }
        knob(s, 1, false);
        if (s->left_sel == first) break;
    }
}
/* one knob press on every row of every pane, each from a fresh load */
static void press_all(const char *cfg, azmod_state *s) {
    for (int k = 0; k < 64; k++) {
        load(cfg, s);
        azmod_set_docked(true);
        s->menu_focused = 1;
        s->mode = AZ_OPEN;
        LEFT_RESET();
        azmod_menu_reset(s);
        int first = s->left_sel;
        for (int i = 0; i < k; i++) knob(s, 1, false);
        if (k && s->left_sel == first) break;
        if (s->mode != AZ_OPEN && s->mode != AZ_OVERLAYS) continue;
        for (int j = 0; j < 32; j++) {
            load(cfg, s);
            s->menu_focused = 1;
            s->mode = AZ_OPEN;
            LEFT_RESET();
            azmod_menu_reset(s);
            for (int i = 0; i < k; i++) knob(s, 1, false);
            knob(s, 0, true);
            int r0 = s->menu_sel;
            for (int i = 0; i < j; i++) knob(s, 1, false);
            if (j && s->menu_sel == r0) break;
            knob(s, 0, true); /* the press */
            char nm[48];
            snprintf(nm, sizeof nm, "press left%d row%d", k, j);
            dump_state(nm, s);
            if (j == 0 || s->mode == AZ_OPEN) {
                snprintf(nm, sizeof nm, "after.left%d.row%d", k, j);
                frame(nm, s);
            }
#ifdef AZMOD_HAVE_BROWSE_VIEW_SCREEN
            if (s->mode == AZ_BROWSE_VIEW) { /* its screen in the full-screen menu: rows, knob, BACK */
                frame("browse.in", s);
                for (int i = 0; i < 3; i++) {
                    knob(s, 1, false);
                    snprintf(nm, sizeof nm, "browse.knob%d", i);
                    frame(nm, s);
                }
                azmod_back(s);
                frame("browse.back", s);
            }
#endif
        }
    }
}
/* a tap on every left-list checkbox (press + release on it), each from a fresh load */
static void tap_left_boxes(const char *cfg, azmod_state *s) {
    for (int k = 0; k < 64; k++) {
        load(cfg, s);
        azmod_set_docked(true);
        s->menu_focused = 1;
        s->mode = AZ_OPEN;
        LEFT_RESET();
        azmod_menu_reset(s);
        int first = s->left_sel;
        for (int i = 0; i < k; i++) knob(s, 1, false);
        if (k && s->left_sel == first) break;
        int x, y, w, h;
        c_left_rect(&g_fb, k, &x, &y, &w, &h);
        int gap = h / 14 < 2 ? 2 : h / 14, ih = h - gap, chh = ih * 2 / 5, chw = chh * 16 / 28;
        int bs = g_fb.h * 20 / 800, bx = x + w - g_fb.w / 40 - chw - g_fb.w / 30 - bs, by = y + (ih - bs) / 2;
        int tx = bx + bs / 2, ty = by + bs / 2;
        azmod_menu_clock(1000 + k * 1000);
        azmod_input(s, &g_fb, tx, ty, true, 0, false);
        azmod_menu_clock(1000 + k * 1000 + 50);
        int r = azmod_pane_drag(s, &g_fb, tx, ty, 2);
        char nm[48];
        snprintf(nm, sizeof nm, "tap left%d box (fired=%d)", k, r == 2);
        dump_state(nm, s);
    }
}
static void walk_compact(azmod_state *s) {
    azmod_set_docked(false);
    s->menu_focused = 1;
    s->mode = AZ_OPEN;
    s->menu_sel = 0;
    int first = s->menu_sel;
    for (int k = 0; k < 64; k++) {
        char nm[32];
        snprintf(nm, sizeof nm, "compact%d", k);
        frame(nm, s);
        knob(s, 1, false);
        if (s->menu_sel == first) break;
    }
    azmod_set_docked(true);
}

/* compact list: one knob press on every stop, then a tap on every drum-bank button, each
 * from a fresh load */
static void press_compact(const char *cfg, azmod_state *s) {
    for (int k = 0; k < 64; k++) {
        load(cfg, s);
        azmod_set_docked(false);
        s->menu_focused = 1;
        s->mode = AZ_OPEN;
        s->menu_sel = 0;
        int first = s->menu_sel;
        for (int i = 0; i < k; i++) knob(s, 1, false);
        if (k && s->menu_sel == first) break;
        knob(s, 0, true);
        char nm[48];
        snprintf(nm, sizeof nm, "compact press %d", k);
        dump_state(nm, s);
        snprintf(nm, sizeof nm, "after.compact%d", k);
        frame(nm, s);
    }
    for (int b = 0; b < 4; b++) {
        load(cfg, s);
        azmod_set_docked(false);
        s->menu_focused = 1;
        s->mode = AZ_OPEN;
        s->menu_sel = 0;
        int x, y, w, h;
        bank_rect(b, &x, &y, &w, &h);
        azmod_input(s, &g_fb, x + w / 2, y + h / 2, true, 0, false);
        char nm[48];
        snprintf(nm, sizeof nm, "compact tap bank %d", b + 1);
        dump_state(nm, s);
        snprintf(nm, sizeof nm, "after.compactbank%d", b + 1);
        frame(nm, s);
    }
    azmod_set_docked(true);
}

static const char *const DECK_LINE = "100011110001011001110000100011000111111";
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    snprintf(cfg_path, sizeof cfg_path, "%s", argv[1]);
    snprintf(settings_dir, sizeof settings_dir, "%s", argv[1]);
    {
        char *sl = strrchr(settings_dir, '/');
        if (sl) *sl = 0;
        else strcpy(settings_dir, ".");
    }
    snprintf(conf_path, sizeof conf_path, "%s/azmod.conf", settings_dir);
    START_VISIBILITY();
    static char cfgs[16][256], names[16][32];
    int nc = 0;
    char z[40], o[40];
    memset(z, '0', 39);
    z[39] = 0;
    memset(o, '1', 39);
    o[39] = 0;
#define ADD(nm, ...) (snprintf(names[nc], 32, "%s", nm), snprintf(cfgs[nc], 256, __VA_ARGS__), nc++)
    ADD("realdeck", "%s\n3 -1 27 0 1 -1 5 -1 12\n0\n", DECK_LINE);
    ADD("zeros", "%s\n", z);
    ADD("ones", "%s\n-1 -1 -1 -1 0\n0\n", o);
    ADD("width13", "%.13s\n", DECK_LINE);
    ADD("width38", "%.38s\n-1 -1 -1 -1 1\n", DECK_LINE);
    for (int b = 0; b < 4; b++) { /* each drum bank, Drum Roll on */
        char l[40];
        memcpy(l, DECK_LINE, 40);
        for (int i = 0; i < 4; i++) l[7 + i] = i == b ? '1' : '0';
        char nm[32];
        snprintf(nm, sizeof nm, "bank%d", b + 1);
        ADD(nm, "%s\n", l);
    }
    {
        char l[40];
        memcpy(l, DECK_LINE, 40);
        l[7] = '0';
        l[8] = l[10] = '1';
        l[9] = '0';
        ADD("bank-two-set", "%s\n", l);
    }
    {
        char l[40];
        memcpy(l, DECK_LINE, 40);
        for (int i = 7; i <= 10; i++) l[i] = '0';
        ADD("bank-none", "%s\n", l);
    }
    ADD("garbage", "1\n3 3 3 3 1\n");
    /* the defaults: no file */
    names[nc][0] = 0;
    snprintf(names[nc], 32, "nofile");
    cfgs[nc][0] = 0;
    nc++;
    ADD("pins", "%s\n-1 -1 -1 -1 1\n%llu\n", DECK_LINE,
        (1ull << 0) | (1ull << 6) | (1ull << 18) | (1ull << 37));
    static azmod_state s;
    for (int c = 0; c < nc; c++) {
        const char *cfg = cfgs[c][0] ? cfgs[c] : NULL;
        printf("# config %s\n", names[c]);
        load(cfg, &s);
        dump_state("loaded", &s);
        if (!strcmp(names[c], "pins")) s.pinrows_on = true;
        walk_full(&s);
        load(cfg, &s);
        if (!strcmp(names[c], "pins")) s.pinrows_on = true;
        azmod_set_docked(true);
        s.mode = AZ_OPEN;
        s.menu_focused = 0;
        frame("preview", &s);
        walk_compact(&s);
        press_all(cfg, &s);
        press_compact(cfg, &s);
        tap_left_boxes(cfg, &s);
    }
    return 0;
}
