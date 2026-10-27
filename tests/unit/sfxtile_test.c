/* SPDX-License-Identifier: GPL-3.0-only */
/* sfx_tile_index (extracted from azmod/stems_display.c by run.sh): an FX tile's image array is
 * grown once by the four cyan Stem FX images, and the index of the asked effect's image is
 * returned. EP147's own entries are kept in place; a second call does not grow it again; an
 * array EP147 rebuilt (or a foreign one whose tail is not ours) is grown again; a bad array, a
 * bad effect or a decoder that is not ready yet gives -1. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fx_img {
    char c[17];
};
static const unsigned char SFX_TILE_VOCAL_ECHO[3] = {1, 2, 3}, SFX_TILE_INST_ECHO[4] = {4, 5, 6, 7},
                           SFX_TILE_INST_BRAKER[5] = {8, 9, 10, 11, 12}, SFX_TILE_DRUMS_ECHO[2] = {13, 14};
static int g_decoder_ready = 1, g_decodes;
static void fx_fns_resolve(void) {}
static struct fx_img fx_img_from_mem_impl(const void *data, int size) {
    struct fx_img im;
    memset(&im, 0, sizeof im);
    if (g_decoder_ready) {
        uintptr_t v = (uintptr_t) data + ((uintptr_t) size * 0x1000); /* one distinct handle per image */
        memcpy(im.c, &v, sizeof v);
        g_decodes++;
    }
    return im;
}
static void fx_img_assign_impl(void *dst, const void *src) { memcpy(dst, src, 8); }
static struct fx_img (*fx_img_from_mem)(const void *, int) = fx_img_from_mem_impl;
static void (*fx_img_assign)(void *, const void *) = fx_img_assign_impl;
static uintptr_t fx_img_val(const struct fx_img *im) {
    uintptr_t v;
    memcpy(&v, im->c, 8);
    return v;
}
#include SFX_INC

static int fails;
#define CHECK(c, ...)                                  \
    do {                                               \
        if (!(c)) {                                    \
            printf("FAIL %s:%d ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                       \
            printf("\n");                              \
            fails++;                                   \
        }                                              \
    } while (0)

/* An ImagesComponent stand-in: the array pointer at +0xd0, its count at +0xe0. */
static unsigned char g_comp[0x100] __attribute__((aligned(16)));
static uintptr_t *arr(void) { return *(uintptr_t **) (g_comp + 0xd0); }
static uint32_t cnt(void) { return *(uint32_t *) (g_comp + 0xe0); }
/* Every array seen: azmod leaves the block it replaced for a paint that may still read it,
 * so the stand-in EP147 frees them all at the end. */
static void *g_blocks[64];
static int g_nblocks;
static void seen(void *p) {
    if (!p)
        return;
    for (int i = 0; i < g_nblocks; i++)
        if (g_blocks[i] == p)
            return;
    if (g_nblocks < 64)
        g_blocks[g_nblocks++] = p;
}
static uintptr_t *native(uint32_t n) {
    uintptr_t *a = calloc(n, 8);
    for (uint32_t i = 0; i < n; i++)
        a[i] = 0x7000 + i * 8; /* EP147's own images */
    *(uintptr_t **) (g_comp + 0xd0) = a;
    *(uint32_t *) (g_comp + 0xe0) = n;
    seen(a);
    return a;
}
static int natives_kept(uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (arr()[i] != 0x7000 + i * 8)
            return 0;
    return 1;
}
static int tail_ours(void) {
    for (int i = 0; i < 4; i++)
        if (arr()[cnt() - 4 + i] != fx_img_val(&g_sfx_art[i]))
            return 0;
    return 1;
}

static int idx(long comp, int fx) {
    int r = sfx_tile_index(comp, fx);
    seen(arr());
    return r;
}

int main(void) {
    const long comp = (long) g_comp;

    g_decoder_ready = 0; /* the decoder not resolved yet: nothing grown, -1 */
    native(26);
    CHECK(idx(comp, 0) == -1 && cnt() == 26, "grew before the art decoded (cnt %u)", cnt());
    g_decoder_ready = 1;

    int i0 = idx(comp, 0); /* first use: grown 26 -> 30, VOCAL ECHO at 26 */
    CHECK(i0 == 26 && cnt() == 30, "first call: index %d count %u (want 26, 30)", i0, cnt());
    CHECK(natives_kept(26) && tail_ours(), "EP147's entries moved or the tail is not ours");
    int i3 = idx(comp, 3); /* again: no second growth */
    CHECK(i3 == 29 && cnt() == 30, "second call: index %d count %u (want 29, 30)", i3, cnt());
    for (int fx = 0; fx < 4; fx++)
        CHECK(arr()[idx(comp, fx)] == fx_img_val(&g_sfx_art[fx]), "fx %d points at another image", fx);
    CHECK(g_decodes == 4, "decoded %d times (want 4, once each)", g_decodes);

    native(26); /* EP147 rebuilt the array: grown again */
    CHECK(idx(comp, 2) == 28 && cnt() == 30 && tail_ours(), "rebuilt array not grown again (cnt %u)", cnt());

    native(30); /* a foreign 30-entry array: its tail is not ours, so it is grown, not taken */
    CHECK(idx(comp, 1) == 31 && cnt() == 34 && natives_kept(30), "foreign array taken as ours (cnt %u)", cnt());

    native(26);
    CHECK(idx(comp, 4) == -1 && idx(comp, -1) == -1 && cnt() == 26, "effect out of range accepted");
    *(uintptr_t *) (g_comp + 0xd0) = 0; /* no array */
    CHECK(idx(comp, 0) == -1, "null array accepted");
    native(300); /* an absurd count */
    CHECK(idx(comp, 0) == -1 && cnt() == 300, "count 300 accepted");
    native(3); /* too short to hold our tail */
    CHECK(idx(comp, 0) == -1 && cnt() == 3, "count 3 accepted");

    for (int i = 0; i < g_nblocks; i++)
        free(g_blocks[i]);
    printf("%s sfxtile\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
