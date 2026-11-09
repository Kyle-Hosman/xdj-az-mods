/* SPDX-License-Identifier: GPL-3.0-only */
/* Self-check for the per-stem waveform lanes, compiled against the real menu.c.
 * Host-native, asserts only, no framework. From the directory that holds menu.c:
 *   cc -w -I. -o lanetest menu.c <path>/lanetest.c && ./lanetest */
#include "menu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* text.c isn't linked here (no freetype): always "unavailable" so menu.c falls
 * back to its bitmap font. */
int azmod_ttf_center(const azmod_fb *fb, int x, int y, int w, int h,
                     const char *s, int px, int track, int bold, uint32_t c) {
    (void) fb;
    (void) x;
    (void) y;
    (void) w;
    (void) h;
    (void) s;
    (void) px;
    (void) track;
    (void) bold;
    (void) c;
    return 0;
}

/* Same for the Mod Menu face. Both are weak in menu.h, but a macOS link still
 * wants a definition, so stub it rather than rely on weak-import behavior. */
int azmod_ttf_menu(const azmod_fb *fb, int x, int y, int w, int h,
                   const char *s, int px, int track, int bold, uint32_t c) {
    (void) fb;
    (void) x;
    (void) y;
    (void) w;
    (void) h;
    (void) s;
    (void) px;
    (void) track;
    (void) bold;
    (void) c;
    return 0;
}

/* azmod.c defines this (on unless azmod-stemnative.off). 0 = the overlay stem strip draws,
 * which is the path the lane asserts below were written against. */
volatile int azmod_stem_native = 0;

/* azmod.c owns the column map; the lane test doesn't exercise STEM LANES. */
int azmod_wave_zoom(int deck, double *ppc, double *phcol, double *ncol, double *total) {
    (void) deck;
    (void) ppc;
    (void) phcol;
    (void) ncol;
    (void) total;
    return 0;
}
/* Themes (day_mode.c) and the version string (azmod.c) are outside the lane test:
 * Themes off, so every color passes through unchanged. */
int azmod_dm_on(void) { return 0; }
uint32_t azmod_dm_map(uint32_t c) { return c; }
int azmod_dm_key(void) { return 0; }
const char *azmod_live_version(void) { return "test"; }
/* Playlist naming / editing (azmod.c) and the TTF renderer (text.c, FreeType) are
 * not linked into the lane test; the lane asserts never reach them. */
void azmod_naming_cancel(void) {}
void azmod_naming_create(void) {}
int azmod_naming_folder(void) { return 0; }
int azmod_naming_rename(void) { return 0; }
const char *azmod_naming_text(void) { return ""; }
/* community.c (in azmod.c): no community mods in the host tests */
int azcm_count(void) { return 0; }
int azcm_info(int i, azcm_info_t *out) { /* no mod is installed (azcm_count is 0): every index is out of range */
    (void) i;
    (void) out;
    return -1;
}
int azcm_suspended(void) { return 0; }
int azcm_all_off(void) { return 0; }
int azcm_off_by_stick(void) { return 0; }
const char *azcm_fwtag(void) { return ""; }
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
void azcm_page_screen(int i, const az_screen_v1 *scr) {
    (void) i;
    (void) scr;
}
int azcm_row_target(const char *owner, const az_screen_v1 *row_scr) {
    (void) owner;
    return row_scr != NULL;
}
void azcm_resume(void) {}
void azcm_flush_settings(void) {}
int azcm_live_set(const char *id, int on) {
    (void) id;
    (void) on;
    return 0;
}
int azcm_confirm(const char *title, const char *text, const char *yes,
                 void (*fn)(int yes, void *user), void *user) {
    (void) title;
    (void) text;
    (void) yes;
    (void) fn;
    (void) user;
    return -1; /* no dialog on the host: nothing is asked, so nothing is sent */
}
void azcm_reload_requested(void) {}
void azcm_unsuspend(const char *id) { (void) id; }
void azcm_set_all_off(int off) { (void) off; }
int azcm_allow(int i) {
    (void) i;
    return -1;
}
int azcm_new_count(void) { return 0; }
const uint32_t *azcm_icon(const char *path, int sz, int *pending) {
    (void) path;
    (void) sz;
    *pending = 0;
    return NULL;
}
const uint32_t *azcm_shot(const char *path, int bw, int bh, int *ow, int *oh, int *pending) {
    (void) path;
    (void) bw;
    (void) bh;
    *ow = *oh = 0;
    *pending = 0;
    return NULL;
}
unsigned azcm_icons_done(void) {
    return 0;
}
int azcm_min_core_ok(const char *mc) {
    (void) mc;
    return 1;
}
int azcm_ver_newer(const char *a, const char *b) {
    (void) a;
    (void) b;
    return 0;
}
int azcm_any_loaded(void) { return 0; }
void azmod_tb_done(void) {}
void azmod_tb_undo(void) {}
int azmod_ttf_menu_width(const char *s, int n, int px, int bold) {
    (void) s;
    (void) px;
    (void) bold;
    return n * 8;
}
int azmod_ttf_menu2(const azmod_fb *fb, int x, int y, int w, int h, const char *s, int px,
                    int track, int bold, uint32_t c, int split, uint32_t c2) {
    (void) fb;
    (void) x;
    (void) y;
    (void) w;
    (void) h;
    (void) s;
    (void) px;
    (void) track;
    (void) bold;
    (void) c;
    (void) split;
    (void) c2;
    return 0;
}
static unsigned char g_env[AZMOD_STEM_COUNT][256];
static float g_pos = 0.5f;
static int stub_wave(int deck, const unsigned char **env, int *n, int *nstem, float *pos) {
    (void) deck;
    *env = &g_env[0][0];
    *n = 256;
    *nstem = AZMOD_STEM_COUNT;
    *pos = g_pos;
    return 1;
}
static uint32_t px_at(const azmod_fb *fb, int x, int y) {
    return *(uint32_t *) ((char *) fb->px + (size_t) y * fb->stride + (size_t) x * 4) & 0xFFFFFFu;
}

int main(void) {
    azmod_fb fb = {.w = 1280, .h = 800, .stride = 1280 * 4};
    fb.px = calloc(1, (size_t) fb.stride * fb.h);
    assert(fb.px);
    memset(g_env, 200, sizeof g_env);           /* flat, loud envelope */

    azmod_state s;
    azmod_state_init(&s);
    s.waveform_visible = true;
    az_set(AZF_STEMS, true);
    s.pad_beatloop[0] = true;
    s.stems_available[0] = true;

    int lx, ly, lw, lh;
    azmod_stem_lane_rect(&fb, 0, 0, &lx, &ly, &lw, &lh);
    assert(lw > 8 && lh > 4 && lx + lw <= fb.w && ly + lh <= fb.h);

    /* 1. No azstem loaded -> the row stays the plain empty native slot. */
    azmod_stem_wave = 0;
    azmod_composite(&s, &fb);
    assert(px_at(&fb, lx + 2, ly + lh / 2) == 0x00232323u);

    /* 2. With an envelope: an enabled stem draws in its own color, a muted one
     *    in dead gray. That difference is the feature. */
    azmod_stem_wave = stub_wave;
    memset(fb.px, 0, (size_t) fb.stride * fb.h);
    azmod_composite(&s, &fb);
    uint32_t on = px_at(&fb, lx + 2, ly + lh / 2);
    s.stem_on[0][0] = false;
    memset(fb.px, 0, (size_t) fb.stride * fb.h);
    azmod_composite(&s, &fb);
    uint32_t off = px_at(&fb, lx + 2, ly + lh / 2);
    assert(on == 0x0022E022u);                  /* VOCAL green (same as its pad LED) */
    assert(off == 0x00404040u);                 /* muted: still drawn, in gray */

    /* 3. Muting one stem must not touch its neighbors' lanes. */
    int nx, ny, nw, nh;
    azmod_stem_lane_rect(&fb, 0, 1, &nx, &ny, &nw, &nh);
    assert(px_at(&fb, nx + 2, ny + nh / 2) == 0x00FF3030u);   /* INST red, still on */

    /* 4. A silent stretch collapses to the 1px center line, not a full block. */
    memset(g_env[0], 0, sizeof g_env[0]);
    s.stem_on[0][0] = true;
    memset(fb.px, 0, (size_t) fb.stride * fb.h);
    azmod_composite(&s, &fb);
    assert(px_at(&fb, lx + 2, ly + lh / 2) == 0x0022E022u);   /* center drawn */
    assert(px_at(&fb, lx + 2, ly + 1) == 0x00232323u);        /* top is bare slot */

    /* 5. The playhead sits inside the lane and tracks pos. */
    g_pos = 0.0f;
    memset(fb.px, 0, (size_t) fb.stride * fb.h);
    azmod_composite(&s, &fb);
    assert(px_at(&fb, lx, ly + lh / 2) == 0x00FFFFFFu);
    g_pos = 1.0f;
    memset(fb.px, 0, (size_t) fb.stride * fb.h);
    azmod_composite(&s, &fb);
    assert(px_at(&fb, lx + lw - 1, ly + lh / 2) == 0x00FFFFFFu);

    /* 6. Grid region cache: the button band must be pixel-identical whether
     *    freshly rasterized (first composite after a state change) or served
     *    from the cache (every composite after), and a stem toggle must
     *    invalidate it. */
    {
        int bx, by, bw, bh;
        azmod_stem_btn_rect(&fb, 0, 1, &bx, &by, &bw, &bh);
        unsigned char *snap = malloc((size_t) fb.stride * fb.h);
        assert(snap);
        memset(fb.px, 0, (size_t) fb.stride * fb.h);
        azmod_composite(&s, &fb);   /* fresh render */
        memcpy(snap, fb.px, (size_t) fb.stride * fb.h);
        memset(fb.px, 0, (size_t) fb.stride * fb.h);
        azmod_composite(&s, &fb);   /* cache hit */
        assert(memcmp(snap, fb.px, (size_t) fb.stride * fb.h) == 0);
        assert(px_at(&fb, bx + 1, by + bh / 2) == 0x00FF3030u);  /* INST border, on */
        s.stem_on[0][1] = false;                                                /* toggle -> invalidate */
        memset(fb.px, 0, (size_t) fb.stride * fb.h);
        azmod_composite(&s, &fb);
        assert(px_at(&fb, bx + 1, by + bh / 2) == 0x008A8A8Au);  /* grayed border */
        free(snap);
    }

    free(fb.px);
    printf("azmod stem-lane test: all checks passed\n");
    return 0;
}
