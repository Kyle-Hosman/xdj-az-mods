/* SPDX-License-Identifier: GPL-3.0-only */
/* The Clock readout's zone file parser (guest/azmod/clock_tz.h): a file from
 * /usr/share/zoneinfo, so any bytes as far as the parser is concerned.
 *
 * Input byte 0 picks the probe times; the rest is the file. An accepted file must have its
 * transitions in order and in bounds, every type index and offset in range, and every offset
 * answered must be one of the file's types; ctz_format must always write a short string. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "fuzz_common.h"
#include "clock_tz.h"

static struct ctz fz_z;

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    if (n < 1 || n > (1 << 18)) return 0;
    unsigned sel = d[0];
    d++;
    n--;
    int r = ctz_parse(d, n, &fz_z);
    FZ_ASSERT(r == 0 || r == -1);
    if (r != 0) return 0;
    FZ_ASSERT(fz_z.n_trans >= 0 && fz_z.n_trans <= CTZ_MAX_TRANS);
    FZ_ASSERT(fz_z.n_types >= 1 && fz_z.n_types <= CTZ_MAX_TYPES);
    for (int i = 0; i < fz_z.n_trans; i++) {
        FZ_ASSERT(fz_z.idx[i] < fz_z.n_types);
        FZ_ASSERT(i == 0 || fz_z.at[i] > fz_z.at[i - 1]);
    }
    for (int i = 0; i < fz_z.n_types; i++)
        FZ_ASSERT(fz_z.utoff[i] >= -CTZ_MAX_OFF && fz_z.utoff[i] <= CTZ_MAX_OFF);
    int64_t probes[6] = {INT64_MIN, -1, 0, 1782928800LL + (int64_t) sel * 86400 * 7, INT64_MAX,
                         fz_z.n_trans ? fz_z.at[sel % fz_z.n_trans] : 0};
    for (int k = 0; k < 6; k++) {
        int32_t off = 0;
        if (ctz_offset(&fz_z, probes[k], &off) == 0) {
            int found = 0;
            for (int i = 0; i < fz_z.n_types; i++)
                found |= fz_z.utoff[i] == off;
            FZ_ASSERT(found);
        }
        char t[16];
        ctz_format(t, sizeof t, probes[k], off, sel & 1, sel & 2);
        FZ_ASSERT(strlen(t) >= 5 && strlen(t) < sizeof t);
    }
    return 0;
}
