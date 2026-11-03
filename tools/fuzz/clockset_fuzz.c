/* SPDX-License-Identifier: GPL-3.0-only */
/* The Clock's manual set record (guest/azmod/clock_set.h cs_rec_parse): a small file in the
 * deck's settings, so any bytes as far as the parser is concerned.
 *
 * Input bytes 0-7 are the file's modification time, byte 8 picks the probe times; the rest is the
 * file. An accepted record must hold a boot id of the right length and characters (or none, for a
 * marker), an anchor and offset in range, write back to the same bytes (a record with a boot id),
 * and give a shown time inside the picker's range whenever cs_shown answers without NTP. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "fuzz_common.h"
#include "clock_set.h"

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    if (n < 9 || n > 4096) return 0;
    int64_t mtime;
    memcpy(&mtime, d, 8);
    unsigned sel = d[8];
    d += 9;
    n -= 9;
    struct cs_rec r;
    int k = cs_rec_parse((const char *) d, n, mtime, &r);
    FZ_ASSERT(k == 0 || k == -1);
    FZ_ASSERT(k == 0 ? r.has == 1 : r.has == 0);
    if (k != 0) return 0;
    size_t bl = strlen(r.boot);
    FZ_ASSERT(bl == 0 || bl == CS_BOOT_LEN);
    for (size_t i = 0; i < bl; i++)
        FZ_ASSERT((r.boot[i] >= '0' && r.boot[i] <= '9') || (r.boot[i] >= 'a' && r.boot[i] <= 'f') || r.boot[i] == '-');
    FZ_ASSERT(r.anchor >= 0 && r.anchor <= CS_ANCHOR_MAX);
    FZ_ASSERT(r.off >= -CS_OFF_MAX && r.off <= CS_OFF_MAX);
    if (bl) {
        char line[CS_REC_MAX];
        int m = cs_rec_format(line, sizeof line, r.boot, r.anchor, r.anchor + r.off);
        FZ_ASSERT(m > 0);
        struct cs_rec q;
        FZ_ASSERT(cs_rec_parse(line, (size_t) m, 0, &q) == 0 && q.anchor == r.anchor && q.off == r.off &&
                  !strcmp(q.boot, r.boot));
    } else {
        FZ_ASSERT(r.off == 0 && r.anchor == mtime);
    }
    int64_t probes[5] = {0, r.anchor, r.anchor - CS_SLACK - 1, r.anchor + (int64_t) sel * 3600, CS_ANCHOR_MAX};
    for (int i = 0; i < 5; i++)
        for (int b = 0; b < 3; b++) {
            int64_t t = 0, bt = b == 0 ? 0 : b == 1 ? r.anchor
                                                    : probes[i] - 60;
            if (cs_shown(0, &r, sel & 1, probes[i], bt, &t))
                FZ_ASSERT(t >= CS_T_MIN && t == probes[i] + r.off);
        }
    return 0;
}
