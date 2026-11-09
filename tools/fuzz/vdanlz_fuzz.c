/* SPDX-License-Identifier: GPL-3.0-only */
/* A track's vocal detection from its analysis file on the stick (guest/azmod/vd_anlz.h), read
 * by the Vocal Indicator worker from any .2EX file a library row points at.
 *
 * The input is the file. The result is -1, 0 or 1; with 1, every frame is a level 0..4, the
 * count fits the input and the hop and rate are not zero, and both strips' columns (the
 * preview's and the scrolling waveform's, at any width, length, position and zoom) stay 0..4. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "fuzz_common.h"
#include VDANLZ_HDR

struct fz_mem {
    const uint8_t *d;
    size_t n;
};
static int fz_read(void *ctx, uint8_t *b, size_t n, uint64_t off) {
    const struct fz_mem *m = (const struct fz_mem *) ctx;
    if (off > m->n || n > m->n - off)
        return 0;
    memcpy(b, m->d + off, n);
    return 1;
}
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    struct fz_mem m = {d, n};
    struct vd_track t;
    memset(&t, 0xa5, sizeof t);
    int r = vd_anlz_read(fz_read, &m, n, 1 << 24, &t);
    FZ_ASSERT(r >= -1 && r <= 1);
    if (r == 1) {
        FZ_ASSERT(t.n <= n && t.n <= VD_MAX_FRAMES);
        FZ_ASSERT(t.hop && t.hop <= 0x7fffffffu && t.rate);
        for (uint32_t i = 0; i < t.n; i++)
            FZ_ASSERT(t.v[i] <= VD_MAX_LEVEL);
        /* both strips, at a position, zoom and width taken from the input's first bytes */
        uint8_t col[1280];
        int w = n >= 2 ? (int) (d[0] << 8 | d[1]) % 1281 : 0;
        double pos = n >= 4 ? (double) (int16_t) (d[2] << 8 | d[3]) * 64.0 : 0;
        double ppm = n >= 5 ? (d[4] + 1) / 512.0 : 0.01;
        vd_columns(&t, n >= 6 ? d[5] : 1, col, w);
        for (int x = 0; x < w; x++)
            FZ_ASSERT(col[x] <= VD_MAX_LEVEL);
        vd_wave_columns(&t, pos, ppm, -(w / 2), col, w);
        for (int x = 0; x < w; x++)
            FZ_ASSERT(col[x] <= VD_MAX_LEVEL);
        free(t.v);
    }
    return 0;
}
