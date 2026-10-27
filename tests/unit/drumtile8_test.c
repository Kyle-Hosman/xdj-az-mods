/* SPDX-License-Identifier: GPL-3.0-only */
/* drum8_index_apply (extracted from azmod/drum_roll_display.c by run.sh): the DRUM ROLL
 * tile-8 red/green index is written into EP147's ImagesComponent only through a chain proven
 * at use: the slot-7 tile captured in its ctor still has the PadStatusWidget vtable, its stem
 * child is still the one captured, the child's component has the ImagesComponent vtable and
 * holds enough images, and the tile's deck is known. Anything else writes nothing.
 * own_q is faked: a read succeeds only inside the objects made here (the live ones), so a
 * read outside them shows up as a refused read instead of a fault. g_slot7_w stands for the
 * wref slot the tile's destructor clears (wref_get is a plain load of it). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define AZMOD_STEM_DECKS 4
#define SLOT7_MAX 4
#define PADTILE_VT ((uintptr_t) 0x2d8f880)
#define IMAGESCOMP_VT ((uintptr_t) 0x29e9540)
static long g_slot7_w[SLOT7_MAX];
static int g_drum_held[AZMOD_STEM_DECKS], g_drum_arm;
static unsigned char *g_lo[16];
static size_t g_len[16];
static int g_nobj;
static void *obj(size_t n) {
    unsigned char *p = aligned_alloc(16, n);
    memset(p, 0, n);
    g_lo[g_nobj] = p;
    g_len[g_nobj++] = n;
    return p;
}
static int mapped(uintptr_t a) {
    for (int i = 0; i < g_nobj; i++)
        if (a >= (uintptr_t) g_lo[i] && a + 8 <= (uintptr_t) g_lo[i] + g_len[i])
            return 1;
    return 0;
}
static int own_q(uintptr_t a, uintptr_t *out) {
    if ((a & 7) || !mapped(a))
        return 0;
    memcpy(out, (void *) a, 8);
    return 1;
}
static void own_qv(int n, const uintptr_t *a, uintptr_t *out, int *ok) {
    for (int i = 0; i < n; i++) {
        out[i] = 0;
        ok[i] = own_q(a[i], &out[i]);
    }
}
#define WSLOT(p) (p)
static uintptr_t wref_get(long *slot) { return (uintptr_t) *slot; }
static long g_ds_of_deck[AZMOD_STEM_DECKS];
static int deck_for_modeobj(long p) {
    for (int d = 0; d < AZMOD_STEM_DECKS; d++)
        if (p && g_ds_of_deck[d] == p)
            return d;
    return -1;
}
static FILE *azlog_open(const char *p, const char *m) {
    (void) p;
    (void) m;
    return NULL;
}
#define DGRID_GRIDS 4
/* drum_roll_display.c's own state, plain here (its _Atomic spelling is GCC-only with
 * __atomic builtins); the function under test is the extracted one */
static long g_dg8_stem[DGRID_GRIDS];
static int g_drum_tile_repaint;
static unsigned g_dg8_idx_refused;
static int g_dg8_idx_logged = -1;
static unsigned g_tiles_poke_req; /* stems_display.c's per-deck refresh requests */
#include TILE_INC
#include "check.h"

static unsigned char *w, *stem, *comp;
static void put(unsigned char *o, int off, uintptr_t v) { memcpy(o + off, &v, 8); }
static int idx(void) {
    int v;
    memcpy(&v, comp + 0xc8, 4);
    return v;
}
static void reset(void) {
    put(w, 0, PADTILE_VT);
    put(w, 0x110, (uintptr_t) stem);
    put(w, 0xd0, 0x7000);
    put(stem, 0xf0, (uintptr_t) comp);
    put(comp, 0, IMAGESCOMP_VT);
    uint32_t cnt = 0x1a;
    memcpy(comp + 0xe0, &cnt, 4);
    int start = 0x19;
    memcpy(comp + 0xc8, &start, 4);
    g_slot7_w[0] = (long) w;
    g_dg8_stem[0] = (long) stem;
    g_ds_of_deck[1] = 0x7000;
    memset(g_drum_held, 0, sizeof g_drum_held);
    g_drum_arm = 1;
    g_drum_tile_repaint = 0;
    g_tiles_poke_req = 0;
}

int main(void) {
    ut_suite = "drum tile 8";
    w = obj(0x200);
    stem = obj(0x200);
    comp = obj(0x200);

    reset();
    g_drum_held[1] = 1;
    drum8_index_apply();
    CHECK("deck 2 latched: tile 8 index -> green (0x18), repaint raised", idx() == 0x18 && g_drum_tile_repaint == 1);
    g_drum_held[1] = 0;
    drum8_index_apply();
    CHECK("deck 2 released: tile 8 index -> red (0x19)", idx() == 0x19);
    CHECK("released: deck 2's stem tiles refreshed (Stem FX shows again)", g_tiles_poke_req == 2u);
    g_tiles_poke_req = 0;
    drum8_index_apply();
    CHECK("released once: no second refresh", g_tiles_poke_req == 0);

    /* Never latched: the slot is the stems page's DRUMS ECHO tile, and Stem FX shows an effect
     * that is on with its pressed art (0x18). The drum roll must not write it back. */
    reset();
    {
        int pressed = 0x18;
        memcpy(comp + 0xc8, &pressed, 4);
    }
    for (int t = 0; t < 3; t++)
        drum8_index_apply();
    CHECK("not latched: the stems page's tile is left as Stem FX set it", idx() == 0x18 &&
                                                                              !g_drum_tile_repaint &&
                                                                              !g_tiles_poke_req);

    reset();
    g_drum_held[1] = 1;
    put(w, 0, 0x1234560); /* the tile was freed and its memory reused */
    drum8_index_apply();
    CHECK("tile no longer a PadStatusWidget: nothing written", idx() == 0x19 && !g_drum_tile_repaint);

    reset();
    g_drum_held[1] = 1;
    {   /* the tile rebuilt its stem child: a complete, valid new chain the capture never saw */
        unsigned char *stem2 = obj(0x200), *comp2 = obj(0x200);
        put(stem2, 0xf0, (uintptr_t) comp2);
        put(comp2, 0, IMAGESCOMP_VT);
        uint32_t cnt = 0x1a;
        int start = 0x19, v2;
        memcpy(comp2 + 0xe0, &cnt, 4);
        memcpy(comp2 + 0xc8, &start, 4);
        put(w, 0x110, (uintptr_t) stem2);
        drum8_index_apply();
        memcpy(&v2, comp2 + 0xc8, 4);
        CHECK("stem child no longer the one captured: nothing written, old or new", idx() == 0x19 && v2 == 0x19);
    }

    reset();
    g_drum_held[1] = 1;
    put(comp, 0, 0x29e9548);
    drum8_index_apply();
    CHECK("component no longer an ImagesComponent: nothing written", idx() == 0x19);

    reset();
    g_drum_held[1] = 1;
    put(stem, 0xf0, 0x10); /* a component pointer outside any object */
    drum8_index_apply();
    CHECK("component pointer unreadable: nothing written", idx() == 0x19);

    reset();
    uint32_t small = 0x18;
    memcpy(comp + 0xe0, &small, 4);
    g_drum_held[1] = 1;
    drum8_index_apply();
    CHECK("image count too small for the index: refused, nothing written",
          idx() == 0x19 && g_dg8_idx_refused >= 1);

    reset();
    g_ds_of_deck[1] = 0; /* the tile's deck is not known */
    g_drum_held[0] = 1;  /* while another deck is latched */
    drum8_index_apply();
    CHECK("unknown deck: tile left alone, never another deck's latch", idx() == 0x19);

    reset();
    g_slot7_w[0] = 0;
    g_drum_held[1] = 1;
    drum8_index_apply();
    CHECK("no tile captured, or its destructor cleared the wref slot: nothing written", idx() == 0x19);
    return ut_summary();
}
