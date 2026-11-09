/* SPDX-License-Identifier: GPL-3.0-only */
/* The source-track parsers azstem runs on every MP3 / M4A track loaded with stems
 * (guest/azstem/ofsload.c up to ofs_stems_decoded, extracted by tools/fuzz/lib.sh): mp3_probe
 * (ID3v2 skip, frame header, Info/Xing tag) and m4a_decode_native (the MP4 box walk: moov,
 * trak, stbl, stsd/mp4a/esds, elst, the sample table). The track is any file on the stick.
 *
 * fdk-aac is EP147's own decoder, bound at run time; here it is a stand-in that accepts every
 * access unit and reports 1024 stereo frames at 44.1 kHz, so the sample-table walk runs to the
 * end. lossless_rate and m4a_alac_rate read the same file. libmpg123 is left unbound (its decode is not ours to fuzz).
 * Input byte 0: even = an .m4a file, odd = an .mp3 file; the rest is the file. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "fuzz_common.h"
#include "ep147rate.h"

static void logln(const char *fmt, ...) { (void) fmt; }
#include MP4_INC

static void *fk_open(int t, unsigned l) {
    (void) t, (void) l;
    return (void *) &g_fdk;
}
static int fk_config(void *h, unsigned char **a, const unsigned *n) {
    (void) h, (void) a, (void) n;
    return 0;
}
static int fk_fill(void *h, unsigned char **b, const unsigned *n, unsigned *v) {
    (void) h, (void) b, (void) n;
    *v = 0;
    return 0;
}
static int fk_decode(void *h, short *pcm, int n, unsigned fl) {
    (void) h, (void) fl;
    memset(pcm, 0, (size_t) n * sizeof *pcm < 2048 * 2 * sizeof *pcm ? (size_t) n * sizeof *pcm : 2048 * 2 * sizeof *pcm);
    return 0;
}
static const int *fk_info(void *h) {
    (void) h;
    static int si[32] = {44100, 1024, 2};
    return si;
}
static int fk_param(void *h, int p, int v) {
    (void) h, (void) p, (void) v;
    return 0;
}
static void fk_close(void *h) { (void) h; }

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    if (n < 1 || n > (1 << 20)) return 0;
    if (!g_fdk.bound) {
        g_fdk.open = fk_open, g_fdk.config_raw = fk_config, g_fdk.fill = fk_fill, g_fdk.decode = fk_decode;
        g_fdk.info = fk_info, g_fdk.set_param = fk_param, g_fdk.close = fk_close;
        g_fdk.bound = 1;
        g_m123.bound = -1;
    }
    if (d[0] & 1) {
        int rate = -1;
        char vendor[16];
        long frame = -1, first = -1;
        int cls = mp3_probe(fz_file("track.mp3", d + 1, n - 1), &rate, vendor, sizeof vendor, &frame,
                            &first);
        if (getenv("FZ_VERBOSE")) fprintf(stderr, "mp3: cls=%d rate=%d vendor=%s\n", cls, rate, vendor);
        FZ_ASSERT(cls >= MP3_CLS_NONE && cls <= MP3_CLS_NOTAG && strlen(vendor) < sizeof vendor);
        FZ_ASSERT((frame == 0 || frame == 576 || frame == 1152) && first >= 0);
        int lr = lossless_rate(fz_file("track.flac", d + 1, n - 1));
        FZ_ASSERT(lr >= 0 && lr <= 768000);
        return 0;
    }
    FZ_ASSERT(m4a_alac_rate(fz_file("track.m4a", d + 1, n - 1)) >= 0);
    int rate, aot, sbr;
    char desc[96];
    long got = -1;
    float *nat = m4a_decode_native(fz_file("track.m4a", d + 1, n - 1), 44100, &rate, &aot, &sbr, desc, sizeof desc, &got);
    if (getenv("FZ_VERBOSE")) fprintf(stderr, "m4a: got=%ld rate=%d aot=%d desc=%s\n", got, rate, aot, desc);
    FZ_ASSERT(nat ? got > 0 : got == 0);
    FZ_ASSERT(strlen(desc) < sizeof desc);
    free(nat);
    return 0;
}
