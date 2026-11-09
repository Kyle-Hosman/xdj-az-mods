/* SPDX-License-Identifier: GPL-3.0-only */
/* A track's vocal detection from its analysis file (guest/azmod/vd_anlz.h), over files built
 * here in the layout rekordbox exports: a plain and a masked PVDI section give the same frames
 * (the masked vector is real export data), a file without PVDI gives none, and a section or a
 * header that runs past the file, a count that disagrees with the section's length, a zero
 * hop or rate, or a level above 4 makes the file malformed. VDANLZ_HDR = the header under test. */
#include "check.h"
#include <stdlib.h>
#include <string.h>
#include VDANLZ_HDR

static uint8_t buf[4096];
static size_t blen;
static void put32(size_t o, uint32_t v) {
    buf[o] = (uint8_t) (v >> 24);
    buf[o + 1] = (uint8_t) (v >> 16);
    buf[o + 2] = (uint8_t) (v >> 8);
    buf[o + 3] = (uint8_t) v;
}
static void file_start(void) {
    memset(buf, 0, sizeof buf);
    memcpy(buf, "PMAI", 4);
    put32(4, 28);
    blen = 28;
}
static void file_end(void) { put32(8, (uint32_t) blen); }
static void other_section(const char *tag, uint32_t len) {
    memcpy(buf + blen, tag, 4);
    put32(blen + 4, 12);
    put32(blen + 8, len);
    blen += len;
}
static size_t pvdi(uint32_t hop, uint16_t rate, const uint8_t *v, uint32_t n) {
    size_t o = blen;
    memcpy(buf + o, "PVDI", 4);
    put32(o + 4, 24);
    put32(o + 8, 24 + n);
    put32(o + 12, hop);
    buf[o + 16] = (uint8_t) (rate >> 8);
    buf[o + 17] = (uint8_t) rate;
    buf[o + 19] = 1;
    put32(o + 20, n);
    memcpy(buf + o + 24, v, n);
    blen += 24 + n;
    return o;
}
static int rd(void *ctx, uint8_t *b, size_t n, uint64_t off) {
    (void) ctx;
    if (off > blen || n > blen - off)
        return 0;
    memcpy(b, buf + off, n);
    return 1;
}
static int parse(struct vd_track *t) {
    memset(t, 0, sizeof *t);
    return vd_anlz_read(rd, 0, blen, 1 << 20, t);
}

/* 57 frames of a real export, masked, starting at frame 551 (a multiple of 19), and the same
 * frames as rekordbox's own unmasked copy holds them */
static const uint8_t MASKED[57] = {
    0x1e, 0xdd, 0x1e, 0x19, 0x02, 0x19, 0x1b, 0x11, 0x19, 0x23, 0x18, 0x19, 0x24, 0x11, 0xfb,
    0x10, 0x1f, 0x2b, 0x17, 0x1c, 0xdf, 0x1c, 0x1b, 0x01, 0x1a, 0x18, 0x12, 0x1a, 0x20, 0x1b,
    0x1d, 0x20, 0x15, 0xff, 0x15, 0x1a, 0x2e, 0x11, 0x1a, 0xd9, 0x1a, 0x1d, 0x06, 0x1d, 0x1f,
    0x15, 0x1d, 0x27, 0x1c, 0x1d, 0x20, 0x15, 0xff, 0x15, 0x1a, 0x2e, 0x11};
static const uint8_t PLAIN[57] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 2,
                                  2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4,
                                  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};

int main(void) {
    struct vd_track t;

    file_start();
    other_section("PPTH", 40);
    pvdi(1024, 22050, PLAIN, 57);
    other_section("PSSI", 20);
    file_end();
    CHECK("a plain PVDI section is found after other sections", parse(&t) == 1);
    CHECK("plain: count, hop and rate", t.n == 57 && t.hop == 1024 && t.rate == 22050);
    CHECK("plain: the frames as stored", t.v && !memcmp(t.v, PLAIN, 57));
    free(t.v);

    file_start();
    pvdi(0x80000000u | 1024, 22050, MASKED, 57);
    file_end();
    CHECK("a masked PVDI section is found", parse(&t) == 1);
    CHECK("masked: the hop without the mask bit", t.hop == 1024);
    CHECK("masked: decodes to rekordbox's own copy", t.n == 57 && t.v && !memcmp(t.v, PLAIN, 57));
    free(t.v);

    file_start();
    other_section("PPTH", 40);
    file_end();
    CHECK("a file without PVDI has no frames", parse(&t) == 0);

    file_start();
    pvdi(1024, 22050, PLAIN, 0);
    file_end();
    CHECK("an empty PVDI section is found, with no frames", parse(&t) == 1 && t.n == 0);
    free(t.v);

    file_start();
    size_t o = pvdi(1024, 22050, PLAIN, 57);
    file_end();
    put32(o + 8, 24 + 58);
    CHECK("a section longer than the file is malformed", parse(&t) == -1);

    file_start();
    o = pvdi(1024, 22050, PLAIN, 57);
    file_end();
    put32(o + 20, 58);
    CHECK("a count past the section's end is malformed", parse(&t) == -1);
    put32(o + 20, 56);
    CHECK("a count short of the section's end is malformed", parse(&t) == -1);

    file_start();
    o = pvdi(1024, 22050, PLAIN, 57);
    file_end();
    put32(o + 4, 20);
    CHECK("a header that is not 24 bytes is malformed", parse(&t) == -1);

    file_start();
    pvdi(0x80000000u, 22050, PLAIN, 57);
    file_end();
    CHECK("a zero hop is malformed", parse(&t) == -1);
    file_start();
    pvdi(1024, 0, PLAIN, 57);
    file_end();
    CHECK("a zero rate is malformed", parse(&t) == -1);

    uint8_t five[3] = {0, 5, 0};
    file_start();
    pvdi(1024, 22050, five, 3);
    file_end();
    CHECK("a level above 4 is malformed", parse(&t) == -1);

    file_start();
    pvdi(0x80000000u | 1024, 22050, PLAIN, 57);
    file_end();
    CHECK("plain frames marked masked decode out of range", parse(&t) == -1);

    file_start();
    pvdi(1024, 22050, PLAIN, 57);
    file_end();
    buf[0] = 'X';
    CHECK("a file that is not PMAI is malformed", parse(&t) == -1);

    file_start();
    pvdi(1024, 22050, PLAIN, 57);
    file_end();
    blen -= 5;
    CHECK("a truncated file is malformed", parse(&t) == -1);

    file_start();
    pvdi(1024, 22050, PLAIN, 57);
    file_end();
    CHECK("a file larger than the limit is not read", vd_anlz_read(rd, 0, blen, blen - 1, &t) == -1);
    { /* columns: 10 frames of 1 s each (hop 22050 at 22050 Hz) */
        uint8_t v[10] = {0, 1, 0, 4, 2, 2, 0, 0, 3, 1}, col[20];
        struct vd_track c = {v, 10, 22050, 22050};
        vd_columns(&c, 10, col, 10);
        CHECK("columns: one frame per column, as stored", !memcmp(col, v, 10));
        vd_columns(&c, 10, col, 5);
        static const uint8_t five[5] = {1, 4, 2, 0, 3};
        CHECK("columns: two frames per column, the stronger one", !memcmp(col, five, 5));
        vd_columns(&c, 5, col, 10);
        static const uint8_t half[10] = {0, 0, 1, 0, 0, 0, 4, 0, 2, 0};
        CHECK("columns: a 5 s track: frame i at column 2i, frames past its end left out",
              !memcmp(col, half, 10));
        memset(col, 7, sizeof col);
        vd_columns(&c, 0, col, 10);
        CHECK("columns: no length, nothing drawn", !col[0] && !col[9]);
    }
    { /* a scrolling waveform: 10 frames of 1 s each, 1 px per 100 ms */
        uint8_t v[10] = {0, 1, 0, 4, 2, 2, 0, 0, 3, 1}, col[40];
        struct vd_track c = {v, 10, 22050, 22050};
        vd_wave_columns(&c, 0, 0.01, -10, col, 40); /* playhead at column 10, t = 0 */
        int before = 0, ok = 1;
        for (int x = 0; x < 10; x++)
            before |= col[x];
        for (int x = 10; x < 40; x++)
            ok &= col[x] == v[(x - 10) / 10];
        CHECK("waveform: nothing before the track's start", !before);
        CHECK("waveform: each second over its 10 columns from the playhead", ok);
        vd_wave_columns(&c, 3000, 0.01, -10, col, 40); /* 3 s in */
        CHECK("waveform: the playhead column shows the frame playing", col[10] == 4 && col[9] == 0);
        vd_wave_columns(&c, 9500, 0.01, -10, col, 40);
        CHECK("waveform: past the last frame nothing", col[14] == 1 && col[15] == 0 && col[39] == 0);
        vd_wave_columns(&c, 0, 0.0005, 0, col, 4); /* 2 s per column: the stronger frame */
        CHECK("waveform: zoomed out, a column shows its strongest frame",
              col[0] == 1 && col[1] == 4 && col[2] == 2 && col[3] == 0);
    }
    { /* levels from a stem envelope: one peak per frame */
        static uint8_t voc[1024], oth[1024];
        memset(voc + 100, 200, 100);       /* a long vocal */
        memset(voc + 300, 200, 11);        /* a blip */
        memset(voc + 400, 200, 20);        /* two runs with a short gap... */
        memset(voc + 430, 200, 20);
        memset(voc + 500, 100, 100);       /* the other stem is much louder */
        memset(oth + 500, 200, 100);
        memset(voc + 620, 10, 100);        /* under the floor */
        memset(voc + 740, 170, 40);        /* 85% of the loudest vocal */
        memset(voc + 800, 130, 40);        /* 65% */
        memset(voc + 860, 100, 40);        /* 50% */
        memset(voc + 920, 60, 40);         /* 30% */
        struct vd_track t = {0};
        CHECK("stems: a filled track", vd_from_stem_env(voc, oth, 1024, 1024 * 1024, 22050, &t));
        CHECK("stems: frame count and rate", t.n == 1024 && t.hop == 1024 && t.rate == 22050);
        CHECK("stems: a long vocal, exactly its frames",
              !t.v[99] && t.v[100] && t.v[150] == 4 && t.v[199] && !t.v[200]);
        CHECK("stems: a phrase fades in and out at its edges", t.v[100] == 2 && t.v[199] == 2);
        CHECK("stems: a blip under the shortest run goes", !t.v[305]);
        CHECK("stems: a short gap closes", t.v[425] != 0);
        CHECK("stems: a stem the other drowns is not a vocal", !t.v[550]);
        CHECK("stems: under the floor is not a vocal", !t.v[670]);
        CHECK("stems: level follows the peak against the loudest",
              t.v[760] == 4 && t.v[820] == 3 && t.v[880] == 2 && t.v[940] == 1);
        free(t.v);
        static uint8_t none[1024];
        struct vd_track q = {0};
        int r = vd_from_stem_env(none, none, 1024, 1024 * 1024, 22050, &q);
        int any = 0;
        for (uint32_t i = 0; r && i < q.n; i++)
            any |= q.v[i];
        CHECK("stems: silence, no frames marked", r && !any);
        free(q.v);
        struct vd_track z = {0};
        CHECK("stems: no length, no answer", !vd_from_stem_env(voc, oth, 1024, 0, 22050, &z));
        CHECK("stems: no rate, no answer", !vd_from_stem_env(voc, oth, 1024, 1024, 0, &z));
        /* a track at 44100 with several buckets per frame: the strongest one counts */
        static uint8_t v2[4096], o2[4096];
        memset(v2 + 800, 200, 400);
        struct vd_track b = {0};
        CHECK("stems: buckets to frames", vd_from_stem_env(v2, o2, 4096, 2048ull * 1024, 44100, &b) &&
                                              b.n == 1024 && !b.v[199] && b.v[200] &&
                                              b.v[250] == 4 && b.v[299] && !b.v[300]);
        free(b.v);
    }
    { /* which levels a strip shows */
        CHECK("source: stems first", vd_pick_source(1, 1) == VD_STEM);
        CHECK("source: rekordbox when there are no stems", vd_pick_source(1, 0) == VD_RB);
        CHECK("source: stems when rekordbox has nothing", vd_pick_source(0, 1) == VD_STEM);
        CHECK("source: neither, nothing", vd_pick_source(0, 0) == VD_NONE);
    }
    return ut_summary();
}
