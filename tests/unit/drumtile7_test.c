/* SPDX-License-Identifier: GPL-3.0-only */
/* drum_tiles_enforce + hold_tile_ready (extracted from azmod/drum_roll_display.c by run.sh):
 * while a deck's drum roll is armed, tile 7 of that deck's grid shows the appended HOLD art
 * (index 18), or HOLD on (19) while that deck's HOLD is on; tiles 5 and 6 show the blank (17).
 * The art is appended by growing the tile's own 18-entry image array to 20, only on a component
 * with the ImagesComponent vtable, and the index is written only while entries 18/19 are ours.
 * Disarmed, an unknown deck, or a foreign array: tile 7 never gets 18/19.
 * own_qv is faked: a read succeeds only inside the objects made here. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define AZMOD_STEM_DECKS 4
#define SLOT7_MAX 4
#define PADTILE_VT ((uintptr_t) 0x2d8f880)
#define SLIP_WIDGET_VT ((uintptr_t) 0x2d8d4d8)
#define IMAGESCOMP_VT ((uintptr_t) 0x29e9540)
#define DRUM_HOLD_ON 1
static int g_drum_arm = 1;
static int g_drum_held[AZMOD_STEM_DECKS], g_drum_hold[AZMOD_STEM_DECKS];
static volatile unsigned g_drum_sel_led[AZMOD_STEM_DECKS];
static long g_padgrid_ds[SLOT7_MAX];
static long g_slip_tile[AZMOD_STEM_DECKS][8];

static unsigned char *g_lo[256];
static size_t g_len[256];
static int g_nobj;
static void *obj(size_t n) {
    unsigned char *p = aligned_alloc(16, (n + 15) & ~(size_t) 15);
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
static void own_qv(int n, const uintptr_t *a, uintptr_t *out, int *ok) {
    for (int i = 0; i < n; i++) {
        out[i] = 0;
        ok[i] = !(a[i] & 7) && mapped(a[i]);
        if (ok[i])
            memcpy(&out[i], (void *) a[i], 8);
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
static int g_repaints;
static void fake_repaint(long tw) {
    (void) tw;
    g_repaints++;
}
#define PAD_REPAINT fake_repaint
static void slip34_apply(long comp, long tw, int native) {
    (void) comp;
    (void) tw;
    (void) native;
}
struct fx_img {
    char c[17];
};
static const unsigned char HOLD_TILE_OFF[4] = {1, 2, 3, 4};
static const unsigned char HOLD_TILE_ON[4] = {5, 6, 7, 8};
static int g_decoder_ready = 1, g_decodes;
static struct fx_img (*fx_img_from_mem)(const void *data, int size);
static void (*fx_img_assign)(void *dst, const void *src);
static struct fx_img fake_from_mem(const void *data, int size) {
    struct fx_img r;
    memset(&r, 0, sizeof r);
    (void) size;
    g_decodes++;
    if (g_decoder_ready) {
        uintptr_t v = 0x7000000 + (uintptr_t) ((const unsigned char *) data)[0] * 0x100;
        memcpy(r.c, &v, 8);
    }
    return r;
}
static void fake_assign(void *dst, const void *src) { memcpy(dst, src, 8); }
static void fx_fns_resolve(void) {
    fx_img_from_mem = fake_from_mem;
    fx_img_assign = fake_assign;
}
static uintptr_t fx_img_val(const struct fx_img *im) {
    uintptr_t v;
    memcpy(&v, im->c, 8);
    return v;
}
#define IMG_OFF ((uintptr_t) 0x7000100)
#define IMG_ON ((uintptr_t) 0x7000500)

#include TILE_INC

static int fails;
#define CHECK(c, ...)                     \
    do {                                  \
        if (!(c)) {                       \
            printf("FAIL  " __VA_ARGS__); \
            printf("\n");                 \
            fails++;                      \
        }                                 \
    } while (0)

struct tile {
    unsigned char *tw, *child, *model;
};
static struct tile T[AZMOD_STEM_DECKS][8];
static uintptr_t *arr_of(struct tile *t) { return *(uintptr_t **) (t->model + 0xd0); }
/* EP147 swapping in another array: the component owns its array and frees the one it drops. The
 * pool's arrays are the test's own; one that azmod grew is malloc'd, as EP147's are. */
static void ep147_set_array(struct tile *t, uintptr_t *a, uint32_t n) {
    uintptr_t *old = arr_of(t);
    int pooled = 0;
    for (int i = 0; i < g_nobj; i++)
        pooled |= (unsigned char *) old == g_lo[i];
    if (!pooled)
        free(old);
    *(uintptr_t **) (t->model + 0xd0) = a;
    *(uint32_t *) (t->model + 0xe0) = n;
}
static uint32_t cnt_of(struct tile *t) { return *(uint32_t *) (t->model + 0xe0); }
static int idx_of(struct tile *t) { return *(int *) (t->model + 0xc8); }
static void set_idx(struct tile *t, int i) { *(int *) (t->model + 0xc8) = i; }

/* grid g of deck `deck` (-1: unknown), eight live tiles with stock 18-entry arrays */
static void make_grid(int g, int deck) {
    long ds = 0x5000 + g * 0x100;
    g_padgrid_ds[g] = ds;
    if (deck >= 0)
        g_ds_of_deck[deck] = ds;
    for (int s = 0; s < 8; s++) {
        struct tile *t = &T[g][s];
        t->tw = obj(0x200);
        t->child = obj(0x100);
        t->model = obj(0x100);
        uintptr_t *arr = obj(18 * 8);
        for (int i = 0; i < 18; i++)
            arr[i] = 0x6000000 + (uintptr_t) i * 0x100;
        *(uintptr_t *) t->tw = PADTILE_VT;
        *(uintptr_t *) (t->tw + 0x100) = (uintptr_t) t->child;
        *(uintptr_t *) t->child = SLIP_WIDGET_VT;
        *(uintptr_t *) (t->child + 0xf0) = (uintptr_t) t->model;
        *(uintptr_t *) t->model = IMAGESCOMP_VT;
        *(uintptr_t **) (t->model + 0xd0) = arr;
        *(uint32_t *) (t->model + 0xe0) = 18;
        set_idx(t, 2 * s + 1);
        g_slip_tile[g][s] = (long) t->tw;
    }
}
static int stock_entries(struct tile *t) {
    for (int i = 0; i < 18; i++)
        if (arr_of(t)[i] != 0x6000000 + (uintptr_t) i * 0x100)
            return 0;
    return 1;
}

int main(void) {
    /* grid 0 = deck 2, grid 1 = deck 1: each grid shows its own deck, never grid order */
    make_grid(0, 1);
    make_grid(1, 0);
    make_grid(2, -1);

    /* 1. disarmed: tile 7 stays native, its array is never grown */
    drum_tiles_enforce();
    CHECK(idx_of(&T[0][6]) == 13 && cnt_of(&T[0][6]) == 18, "disarmed: tile 7 idx %d cnt %u",
          idx_of(&T[0][6]), cnt_of(&T[0][6]));

    /* 2. deck 2 armed, HOLD off: tile 7 -> 18, array grown to 20 with our art, rest kept */
    g_drum_held[1] = 1;
    drum_tiles_enforce();
    CHECK(idx_of(&T[0][6]) == 18, "armed HOLD off: tile 7 idx %d, want 18", idx_of(&T[0][6]));
    CHECK(cnt_of(&T[0][6]) == 20 && arr_of(&T[0][6])[18] == IMG_OFF &&
              arr_of(&T[0][6])[19] == IMG_ON && stock_entries(&T[0][6]),
          "armed: tile 7 array not grown to the stock 18 + HOLD, HOLD on (cnt %u)",
          cnt_of(&T[0][6]));
    CHECK(idx_of(&T[0][4]) == 17 && idx_of(&T[0][5]) == 17, "armed: tiles 5/6 idx %d/%d, want 17",
          idx_of(&T[0][4]), idx_of(&T[0][5]));
    CHECK(cnt_of(&T[0][4]) == 18 && cnt_of(&T[0][5]) == 18, "armed: tiles 5/6 arrays grown");
    CHECK(idx_of(&T[1][6]) == 13 && cnt_of(&T[1][6]) == 18,
          "deck 1 (disarmed) tile 7 changed by deck 2's arm: idx %d", idx_of(&T[1][6]));

    /* 3. deck 2 HOLD on -> 19; the array is not grown twice */
    uintptr_t *grown = arr_of(&T[0][6]);
    g_drum_hold[1] = DRUM_HOLD_ON;
    drum_tiles_enforce();
    CHECK(idx_of(&T[0][6]) == 19, "HOLD on: tile 7 idx %d, want 19", idx_of(&T[0][6]));
    CHECK(arr_of(&T[0][6]) == grown && cnt_of(&T[0][6]) == 20, "HOLD on: array regrown");

    /* 4. both decks armed, only deck 2 HOLD on: each grid shows its own deck's HOLD */
    g_drum_held[0] = 1;
    drum_tiles_enforce();
    CHECK(idx_of(&T[1][6]) == 18, "deck 1 HOLD off shows %d, want 18 (deck 2 is on)",
          idx_of(&T[1][6]));
    CHECK(idx_of(&T[0][6]) == 19, "deck 2 HOLD on shows %d", idx_of(&T[0][6]));
    g_drum_hold[1] = 0;
    g_drum_hold[0] = DRUM_HOLD_ON;
    drum_tiles_enforce();
    CHECK(idx_of(&T[1][6]) == 19 && idx_of(&T[0][6]) == 18,
          "deck 1 on / deck 2 off shows %d / %d, want 19 / 18", idx_of(&T[1][6]),
          idx_of(&T[0][6]));

    /* 5. EP147 rewrites the index (refresh): re-asserted on the next paint */
    set_idx(&T[1][6], 13);
    drum_tiles_enforce();
    CHECK(idx_of(&T[1][6]) == 19, "rewritten index not re-asserted: %d", idx_of(&T[1][6]));

    /* 6. EP147 rebuilds the array (stock 18 again): re-grown */
    uintptr_t *fresh = obj(18 * 8);
    for (int i = 0; i < 18; i++)
        fresh[i] = 0x6000000 + (uintptr_t) i * 0x100;
    ep147_set_array(&T[1][6], fresh, 18);
    drum_tiles_enforce();
    CHECK(cnt_of(&T[1][6]) == 20 && arr_of(&T[1][6])[19] == IMG_ON && idx_of(&T[1][6]) == 19,
          "rebuilt array not re-grown (cnt %u idx %d)", cnt_of(&T[1][6]), idx_of(&T[1][6]));

    /* 7. the component is not an ImagesComponent: nothing grown, the blank shown */
    *(uintptr_t *) (T[1][6].model) = 0x1234;
    ep147_set_array(&T[1][6], fresh = obj(18 * 8), 18);
    drum_tiles_enforce();
    CHECK(cnt_of(&T[1][6]) == 18, "non-ImagesComponent array grown");
    CHECK(idx_of(&T[1][6]) == 17, "non-ImagesComponent: idx %d, want the blank 17",
          idx_of(&T[1][6]));
    *(uintptr_t *) (T[1][6].model) = IMAGESCOMP_VT;

    /* 8. a 20-entry array that is not ours (e.g. tile 8's): left alone, the blank shown */
    uintptr_t *foreign = obj(20 * 8);
    for (int i = 0; i < 20; i++)
        foreign[i] = 0x6100000 + (uintptr_t) i;
    ep147_set_array(&T[1][6], foreign, 20);
    drum_tiles_enforce();
    CHECK(foreign[18] == 0x6100012 && foreign[19] == 0x6100013, "foreign array entries written");
    CHECK(idx_of(&T[1][6]) == 17, "foreign array: idx %d, want the blank 17", idx_of(&T[1][6]));

    /* 9. unknown deck: grid 2 untouched even with every deck armed and HOLD on */
    for (int d = 0; d < AZMOD_STEM_DECKS; d++)
        g_drum_held[d] = 1, g_drum_hold[d] = DRUM_HOLD_ON;
    drum_tiles_enforce();
    CHECK(idx_of(&T[2][6]) == 13 && cnt_of(&T[2][6]) == 18, "unknown deck: tile 7 idx %d cnt %u",
          idx_of(&T[2][6]), cnt_of(&T[2][6]));

    /* 10. a dead tile (vtable gone): nothing read through it */
    *(uintptr_t *) (T[0][6].tw) = 0;
    set_idx(&T[0][6], 13);
    drum_tiles_enforce();
    CHECK(idx_of(&T[0][6]) == 13, "dead tile written: idx %d", idx_of(&T[0][6]));

    if (fails) {
        printf("FAIL  drum tile 7: %d check(s)\n", fails);
        return 1;
    }
    printf("PASS  drum tile 7: HOLD art per deck, grown only on our ImagesComponent arrays\n");
    return 0;
}
