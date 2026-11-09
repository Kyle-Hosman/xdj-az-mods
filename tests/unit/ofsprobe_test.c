/* SPDX-License-Identifier: GPL-3.0-only */
/* The MP3 probe behind the stem offset (mp3_probe, extracted out of guest/azstem/ofsload.c by
 * run.sh). EP147 starts an MP3 one frame later when its Info/Xing frame has the channel mode of
 * the first audio frame, so a wrong answer here plays every stem 26 ms out of time:
 *   - the frame is added on equal modes, not on the vendor string or any other tag field;
 *   - it is 576 samples for MPEG-2 and 1152 for MPEG-1;
 *   - no Info/Xing frame: nothing is added;
 *   - the first frame is found behind padding and junk, however long, within the scan;
 *   - a sync pattern with no following frame is not a frame.
 * The sample rate of a lossless source (lossless_rate, m4a_alac_rate) picks its resampler delay:
 *   - FLAC, WAV behind other chunks, AIFF and AIFC, ALAC in MP4: the file's rate;
 *   - a cut file or anything else: 0, which applies nothing.
 * argv[1] = a scratch directory. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "check.h"
#include OFSPROBE_INC

static char g_path[512];
static unsigned char g_buf[600000];
static size_t g_n;

static void put(const void *p, size_t n) {
    memcpy(g_buf + g_n, p, n);
    g_n += n;
}
/* One frame at 256 kbit/s 44.1 kHz (835 bytes) or 96 kbit/s 22.05 kHz (313 bytes). `mode` goes in
 * the header; `tag` = "Info", "Xing" or NULL, with `vendor` at its usual place. */
static void frame(int mpeg1, int mode, const char *tag, const char *vendor) {
    unsigned char f[835];
    size_t size = mpeg1 ? 835 : 313;
    memset(f, 0, sizeof f);
    f[0] = 0xFF;
    f[1] = mpeg1 ? 0xFB : 0xF3;
    f[2] = mpeg1 ? 0xD0 : 0xA0;
    f[3] = (unsigned char) (mode << 6);
    if (tag) {
        size_t at = 4 + (mpeg1 ? (mode == 3 ? 17 : 32) : (mode == 3 ? 9 : 17));
        memcpy(f + at, tag, 4);
        if (vendor)
            memcpy(f + at + 120, vendor, strlen(vendor));
    }
    put(f, size);
}
static void id3(size_t size, int footer) {
    unsigned char h[10] = {'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0};
    if (footer)
        h[5] = 0x10;
    h[6] = (size >> 21) & 127;
    h[7] = (size >> 14) & 127;
    h[8] = (size >> 7) & 127;
    h[9] = size & 127;
    put(h, 10);
    memset(g_buf + g_n, 'x', size + (footer ? 10 : 0));
    g_n += size + (footer ? 10 : 0);
}
static long g_first;
static int probe(int *rate, long *fr, char *vendor) {
    FILE *f = fopen(g_path, "wb");
    fwrite(g_buf, 1, g_n, f);
    fclose(f);
    g_n = 0;
    return mp3_probe(g_path, rate, vendor, 16, fr, &g_first);
}
static void flush(void) {
    FILE *f = fopen(g_path, "wb");
    fwrite(g_buf, 1, g_n, f);
    fclose(f);
    g_n = 0;
}
static int rate_of(void) {
    flush();
    return lossless_rate(g_path);
}
static int alac_of(void) {
    flush();
    return m4a_alac_rate(g_path);
}
static void aiff(const unsigned char *rate80, const char *form) {
    unsigned char a[12 + 8 + 18] = {'F', 'O', 'R', 'M', 0, 0, 0, 0};
    memcpy(a + 8, form, 4);
    memcpy(a + 12, "COMM", 4);
    a[19] = 18;
    memcpy(a + 28, rate80, 10);
    put(a, sizeof a);
}
static size_t box(unsigned char *o, const char *t, const unsigned char *body, size_t n) {
    size_t sz = n + 8;
    o[0] = 0, o[1] = 0, o[2] = (unsigned char) (sz >> 8), o[3] = (unsigned char) sz;
    memcpy(o + 4, t, 4);
    memmove(o + 8, body, n);
    return sz;
}
/* moov/trak/mdia/minf/stbl/stsd/<entry>; the entry's rate at +24 */
static void alac(int yes) {
    unsigned char a[512], b[512];
    size_t n;
    memset(a, 0, sizeof a);
    a[24] = 48000 >> 8, a[25] = 48000 & 0xff;
    n = box(b, yes ? "alac" : "mp4a", a, 28);
    memset(a, 0, 8);
    a[7] = 1;
    memcpy(a + 8, b, n);
    n = box(b, "stsd", a, n + 8);
    static const char *const up[] = {"stbl", "minf", "mdia", "trak", "moov"};
    for (int i = 0; i < 5; i++) {
        memcpy(a, b, n);
        n = box(b, up[i], a, n);
    }
    put(b, n);
}
#define STEREO 0
#define JOINT 1
#define MONO 3

int main(int argc, char **argv) {
    ut_suite = "stem offset MP3 probe";
    snprintf(g_path, sizeof g_path, "%s/t.mp3", argc > 1 ? argv[1] : ".");
    int rate, c;
    long fr;
    char v[16];

    frame(1, JOINT, "Info", "LAME3.100"), frame(1, JOINT, NULL, NULL), frame(1, JOINT, NULL, NULL);
    c = probe(&rate, &fr, v);
    CHECK("joint Info, joint audio: one frame", c == MP3_CLS_INFO && rate == 44100 && fr == 1152 && !strcmp(v, "LAME3.100"));

    frame(1, STEREO, "Info", "Lavc61.19"), frame(1, JOINT, NULL, NULL);
    c = probe(&rate, &fr, v);
    CHECK("stereo Info, joint audio: no frame", c == MP3_CLS_INFO && fr == 0 && !strcmp(v, "Lavc61.19"));

    frame(1, JOINT, "Info", "Lavc61.19"), frame(1, JOINT, NULL, NULL);
    c = probe(&rate, &fr, v);
    CHECK("the vendor does not decide: Lavc with equal modes gets the frame", fr == 1152);
    frame(1, STEREO, "Info", "LAME3.100"), frame(1, JOINT, NULL, NULL);
    c = probe(&rate, &fr, v);
    CHECK("the vendor does not decide: LAME with other modes gets none", fr == 0);
    frame(1, JOINT, "Info", NULL), frame(1, JOINT, NULL, NULL);
    c = probe(&rate, &fr, v);
    CHECK("no vendor at all: equal modes get the frame", fr == 1152 && !v[0]);

    frame(1, STEREO, "Info", "x"), frame(1, STEREO, NULL, NULL);
    c = probe(&rate, &fr, v);
    CHECK("stereo Info, stereo audio: one frame", fr == 1152);
    frame(1, MONO, "Info", "Lavc61.19"), frame(1, MONO, NULL, NULL);
    c = probe(&rate, &fr, v);
    CHECK("mono Info, mono audio: one frame, tag at the mono offset", c == MP3_CLS_INFO && fr == 1152);
    frame(1, JOINT, "Xing", "LAME3.100"), frame(1, JOINT, NULL, NULL);
    c = probe(&rate, &fr, v);
    CHECK("Xing counts as Info", c == MP3_CLS_XING && fr == 1152);

    frame(0, JOINT, "Info", "LAME3.100"), frame(0, JOINT, NULL, NULL);
    c = probe(&rate, &fr, v);
    CHECK("MPEG-2: a frame is 576 samples", c == MP3_CLS_INFO && rate == 22050 && fr == 576);
    frame(1, JOINT, NULL, NULL), frame(1, JOINT, NULL, NULL);
    c = probe(&rate, &fr, v);
    CHECK("no Info frame: nothing added", c == MP3_CLS_NOTAG && fr == 0);

    id3(5000, 0), memset(g_buf + g_n, 0, 100000), g_n += 100000;
    frame(1, JOINT, "Info", "LAME3.100"), frame(1, JOINT, NULL, NULL);
    c = probe(&rate, &fr, v);
    CHECK("100 KB of padding after the tag: found, at its offset", c == MP3_CLS_INFO && fr == 1152 && g_first == 105010);
    id3(1000, 1), frame(1, JOINT, "Info", "LAME3.100"), frame(1, JOINT, NULL, NULL);
    c = probe(&rate, &fr, v);
    CHECK("an ID3 footer is skipped", c == MP3_CLS_INFO && fr == 1152 && g_first == 1020);

    {   /* a sync pattern in junk, no frame where it says the next one is */
        unsigned char j[4] = {0xFF, 0xFB, 0xD0, 0x00};
        put(j, 4);
        memset(g_buf + g_n, 0x11, 2000), g_n += 2000;
        frame(1, JOINT, "Info", "LAME3.100"), frame(1, JOINT, NULL, NULL);
        c = probe(&rate, &fr, v);
        CHECK("a sync pattern in junk is not the first frame", c == MP3_CLS_INFO && fr == 1152);
    }
    {   /* an Info frame, then a gap: the audio behind it must not read as a file with no tag */
        frame(1, JOINT, "Info", "LAME3.100");
        memset(g_buf + g_n, 0x11, 500), g_n += 500;
        frame(1, JOINT, NULL, NULL), frame(1, JOINT, NULL, NULL);
        c = probe(&rate, &fr, v);
        CHECK("an Info frame with a gap after it: no class", c == MP3_CLS_NONE && fr == 0);
    }
    frame(1, JOINT, "Info", "LAME3.100");
    c = probe(&rate, &fr, v);
    CHECK("a lone frame with nothing after it: not a frame", c == MP3_CLS_NONE);
    memset(g_buf, 0, 5000), g_n = 5000;
    c = probe(&rate, &fr, v);
    CHECK("no frame in the file", c == MP3_CLS_NONE && rate == 0 && fr == 0);

    /* each measured against EP147's real audio; a rate not measured has none */
    {
        static const long pinned[][2] = {{44100, 0}, {48000, 27}, {22050, 59}, {24000, 54}, {32000, 41}, {88200, 15}, {96000, 14}};
        int ok = 1;
        for (size_t i = 0; i < sizeof pinned / sizeof pinned[0]; i++) {
            const ofs_cal_t *cal = ofs_cal_for((int) pinned[i][0]);
            ok = ok && cal && cal->cal44 == pinned[i][1];
        }
        CHECK("the resampler delay of every measured rate", ok);
        CHECK("no delay for a rate never measured", !ofs_cal_for(16000) && !ofs_cal_for(12000) && !ofs_cal_for(0));
    }
    static const unsigned char aiff441[10] = {0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0};
    static const unsigned char aiff48[10] = {0x40, 0x0E, 0xBB, 0x80, 0, 0, 0, 0, 0, 0};
    static const unsigned char aiff96[10] = {0x40, 0x0F, 0xBB, 0x80, 0, 0, 0, 0, 0, 0};
    /* FLAC: magic, block header, STREAMINFO with 20 bits of rate at byte 10 */
    {
        unsigned char f[42] = {'f', 'L', 'a', 'C', 0, 0, 0, 34};
        unsigned r = 48000;
        f[18] = (unsigned char) (r >> 12), f[19] = (unsigned char) (r >> 4), f[20] = (unsigned char) (r << 4);
        put(f, sizeof f);
        CHECK("FLAC 48000", rate_of() == 48000);
        put(f, 15);
        CHECK("cut FLAC", rate_of() == 0);
    }
    { /* WAV: a LIST chunk of odd size first, then fmt */
        unsigned char w[12 + 8 + 5 + 1 + 8 + 16] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E',
                                                    'L', 'I', 'S', 'T', 5, 0, 0, 0};
        memcpy(w + 26, "fmt ", 4);
        w[30] = 16, w[34] = 1, w[36] = 2;
        w[38] = 0x00, w[39] = 0x77, w[40] = 0x01; /* 96000 */
        put(w, sizeof w);
        CHECK("WAV behind a LIST chunk", rate_of() == 96000);
        put(w, 30);
        CHECK("cut WAV", rate_of() == 0);
    }
    aiff(aiff441, "AIFF");
    CHECK("AIFF 44100", rate_of() == 44100);
    aiff(aiff48, "AIFC");
    CHECK("AIFC 48000", rate_of() == 48000);
    aiff(aiff96, "AIFF");
    CHECK("AIFF 96000", rate_of() == 96000);
    {
        unsigned char neg[10] = {0xC0, 0x0E, 0xAC, 0x44};
        aiff(neg, "AIFF");
        CHECK("a negative AIFF rate", rate_of() == 0);
        unsigned char huge[10] = {0x7F, 0xFF, 0xAC, 0x44};
        aiff(huge, "AIFF");
        CHECK("an AIFF rate out of range", rate_of() == 0);
    }
    memset(g_buf, 0xFF, 4000), g_n = 4000;
    CHECK("not a lossless file", rate_of() == 0);
    alac(1);
    CHECK("ALAC 48000", alac_of() == 48000);
    alac(0);
    CHECK("an MP4 with no ALAC track", alac_of() == 0);
    remove(g_path);
    return ut_summary();
}
