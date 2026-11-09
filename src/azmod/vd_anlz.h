/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file vd_anlz.h
 * @brief A track's vocal detection from its rekordbox analysis file (ANLZ0000.2EX), for the
 * Vocal Indicator on the deck panel.
 *
 * Section PVDI, big-endian like every ANLZ section: the tag, the header length (24), the total
 * length, then u32 hop, u16 sample rate, u16 (1), u32 count, then count bytes, one per frame of
 * hop samples: 0 = no vocal .. 4 = strong. Frame i starts at i * hop / rate seconds. A set top
 * bit in the hop word marks a masked body (exports): byte i is XORed with VD_KEY[i % 19], and
 * the hop is the low 31 bits. A track analysed without vocal detection has no PVDI section.
 *
 * Pure parsing of untrusted bytes: every length is checked against what is left, nothing reads
 * past the input, and a malformed section gives -1 (fuzzed by tools/fuzz vdanlz).
 */
#ifndef AZMOD_VD_ANLZ_H
#define AZMOD_VD_ANLZ_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VD_MAX_LEVEL 4
#define VD_MAX_FRAMES (1u << 20) /* about 13 hours at a 1024-sample hop */
#define VD_HDR 24u

static const uint8_t VD_KEY[19] = {0x1e, 0xdd, 0x1e, 0x19, 0x02, 0x19, 0x1b, 0x11, 0x19, 0x23,
                                   0x18, 0x19, 0x24, 0x11, 0xfb, 0x11, 0x1e, 0x2a, 0x15};

static inline uint32_t vd_be32(const uint8_t *p) {
    return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}

struct vd_track {
    uint8_t *v;         /* n frames, each 0..VD_MAX_LEVEL (malloc'd) */
    uint32_t n;
    uint32_t hop, rate; /* frame i starts at i * hop / rate seconds */
};

/* One PVDI section, s[0, len) (header included), decoded in place: its frames start at
 * s + VD_HDR. The frame count, or -1 when malformed. */
static inline int vd_section(uint8_t *s, size_t len, uint32_t *hop, uint32_t *rate) {
    if (len < VD_HDR || vd_be32(s + 4) != VD_HDR || vd_be32(s + 8) != len)
        return -1;
    uint32_t h = vd_be32(s + 12), r = (uint32_t) s[16] << 8 | s[17], n = vd_be32(s + 20);
    int masked = (h & 0x80000000u) != 0;
    h &= 0x7fffffffu;
    if (!h || !r || n > VD_MAX_FRAMES || n != len - VD_HDR)
        return -1;
    uint8_t *v = s + VD_HDR;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t x = masked ? (uint8_t) (v[i] ^ VD_KEY[i % 19]) : v[i];
        if (x > VD_MAX_LEVEL)
            return -1;
        v[i] = x;
    }
    *hop = h;
    *rate = r;
    return (int) n;
}

/* The vocal frames of an ANLZ file of `size` bytes (at most max), reading only its section
 * headers and the PVDI section. rd(ctx, buf, n, off) reads n bytes at off, 1 when all of them
 * were read. 1 = found (t filled, t->v to free), 0 = the file has no PVDI section, -1 = the file
 * is malformed, unreadable or larger than max. */
typedef int (*vd_read_fn)(void *ctx, uint8_t *buf, size_t n, uint64_t off);
static inline int vd_anlz_read(vd_read_fn rd, void *ctx, uint64_t size, uint64_t max,
                               struct vd_track *t) {
    uint8_t h[12];
    if (size < 12 || size > max || !rd(ctx, h, 12, 0) || h[0] != 'P' || h[1] != 'M' ||
        h[2] != 'A' || h[3] != 'I')
        return -1;
    uint64_t o = vd_be32(h + 4);
    if (o < 12 || o > size)
        return -1;
    while (size - o >= 12) {
        if (!rd(ctx, h, 12, o))
            return -1;
        uint64_t hl = vd_be32(h + 4), len = vd_be32(h + 8);
        if (len < 12 || len > size - o || hl < 12 || hl > len)
            return -1;
        if (h[0] == 'P' && h[1] == 'V' && h[2] == 'D' && h[3] == 'I') {
            if (len > VD_HDR + VD_MAX_FRAMES)
                return -1;
            uint8_t *s = (uint8_t *) malloc((size_t) len);
            if (!s || !rd(ctx, s, (size_t) len, o)) {
                free(s);
                return -1;
            }
            int n = vd_section(s, (size_t) len, &t->hop, &t->rate);
            if (n < 0) {
                free(s);
                return -1;
            }
            memmove(s, s + VD_HDR, (size_t) n);
            t->v = s;
            t->n = (uint32_t) n;
            return 1;
        }
        o += len;
    }
    return size == o ? 0 : -1;
}
/* Levels from a track's stems when its analysis file has none, at rekordbox's frame rate. voc and
 * oth are nb peaks (0..255, of the track's loudest stem) of the vocals and other stems over a
 * track of `frames` samples at src_rate. A frame is vocal when its peak clears VD_STEM_FLOOR and
 * stands up to the other stem's (a stem split leaves some of the mix in vocals); gaps up to
 * VD_STEM_GAP frames close, and runs under VD_STEM_MIN_RUN frames go, as breaths and bleed. Level
 * from the vocal's volume over about a second around the frame, against the track's loudest: a
 * single peak nearly always reaches the top, and a column shows its strongest frame, so peaks
 * alone would draw every vocal at full strength. 2, 4.5 and 8 dB down are the level steps.
 * 1 = t filled (t->v to free), 0 = none. */
#define VD_STEM_HOP 1024u
#define VD_STEM_RATE 22050u
#define VD_STEM_FLOOR 16
#define VD_STEM_GAP 11u     /* about 0.5 s */
#define VD_STEM_MIN_RUN 22u /* about 1 s */
#define VD_STEM_SMOOTH 11u  /* frames on each side of the volume's window */
static inline int vd_from_stem_env(const uint8_t *voc, const uint8_t *oth, uint32_t nb,
                                   uint64_t frames, uint32_t src_rate, struct vd_track *t) {
    if (!nb || !frames || !src_rate || frames > (1ull << 32))
        return 0;
    uint64_t n64 = frames * VD_STEM_RATE / ((uint64_t) src_rate * VD_STEM_HOP);
    if (!n64 || n64 > VD_MAX_FRAMES)
        return 0;
    uint32_t n = (uint32_t) n64;
    uint8_t *v = (uint8_t *) calloc(n, 3);
    if (!v)
        return 0;
    uint8_t *pv = v + n, *po = v + 2 * (size_t) n;
    uint64_t den = (uint64_t) nb * src_rate * VD_STEM_HOP;
    for (uint32_t b = 0; b < nb; b++) {
        uint64_t i = (uint64_t) b * frames * VD_STEM_RATE / den;
        if (i >= n)
            i = n - 1;
        if (voc[b] > pv[i])
            pv[i] = voc[b];
        if (oth[b] > po[i])
            po[i] = oth[b];
    }
    for (uint32_t i = 0; i < n; i++) {
        v[i] = pv[i] >= VD_STEM_FLOOR && pv[i] * 4u >= po[i] * 3u;
    }
    for (uint32_t i = 0, last = n; i < n; i++) { /* close the short gaps */
        if (!v[i])
            continue;
        if (last < n && i - last - 1 <= VD_STEM_GAP)
            memset(v + last + 1, 1, i - last - 1);
        last = i;
    }
    for (uint32_t i = 0; i < n;) { /* drop the short runs */
        if (!v[i]) {
            i++;
            continue;
        }
        uint32_t e = i;
        while (e < n && v[e])
            e++;
        if (e - i < VD_STEM_MIN_RUN)
            memset(v + i, 0, e - i);
        i = e;
    }
    uint64_t top = 0;
    for (int pass = 0; pass < 2; pass++) { /* the loudest vocal's sum, then each frame against it */
        uint64_t sum = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (i + VD_STEM_SMOOTH < n)
                sum += pv[i + VD_STEM_SMOOTH];
            if (!i)
                for (uint32_t k = 0; k < VD_STEM_SMOOTH && k < n; k++)
                    sum += pv[k];
            if (i > VD_STEM_SMOOTH)
                sum -= pv[i - VD_STEM_SMOOTH - 1];
            if (!v[i])
                continue;
            if (!pass) {
                if (sum > top)
                    top = sum;
            } else {
                v[i] = sum * 1000 >= top * 794 ? 4 : sum * 1000 >= top * 596 ? 3
                                                 : sum * 1000 >= top * 398   ? 2
                                                                             : 1;
            }
        }
    }
    uint8_t *s = (uint8_t *) realloc(v, n); /* the working copies go */
    if (s)
        v = s;
    t->v = v;
    t->n = n;
    t->hop = VD_STEM_HOP;
    t->rate = VD_STEM_RATE;
    return 1;
}
/* Which levels a strip shows: the stems', else rekordbox's. */
enum {
    VD_NONE,
    VD_RB,
    VD_STEM
};
static inline int vd_pick_source(int has_rb, int has_stem) {
    if (has_stem)
        return VD_STEM;
    if (has_rb)
        return VD_RB;
    return VD_NONE;
}
/* The strongest level in each of w columns spanning a track len_s seconds long: column x covers
 * [x * len_s / w, (x + 1) * len_s / w) s, and frame i starts at i * hop / rate s. Frames past
 * the track's end are left out. */
static inline void vd_columns(const struct vd_track *t, uint32_t len_s, uint8_t *col, int w) {
    if (w <= 0)
        return;
    memset(col, 0, (size_t) w);
    if (!t->rate || !len_s)
        return;
    uint64_t per = (uint64_t) t->hop * (uint64_t) w, den = (uint64_t) t->rate * len_s;
    for (uint32_t i = 0; i < t->n; i++) {
        uint64_t x = (uint64_t) i * per / den;
        if (x >= (uint64_t) w)
            break;
        if (t->v[i] > col[x])
            col[x] = t->v[i];
    }
}
/* The strongest level in each of w columns of a scrolling waveform: the playhead (pos_ms of
 * track time) is at column -dx0, and ppm pixels stand for one ms, so column x covers
 * [pos_ms + (x + dx0) / ppm, pos_ms + (x + dx0 + 1) / ppm) ms. Before the track and past its
 * last frame: 0. */
static inline void vd_wave_columns(const struct vd_track *t, double pos_ms, double ppm, int dx0,
                                   uint8_t *col, int w) {
    if (w <= 0)
        return;
    memset(col, 0, (size_t) w);
    if (!(ppm > 0) || !t->hop || !t->rate || !t->n)
        return;
    double fpm = (double) t->rate / ((double) t->hop * 1000.0); /* frames per ms */
    for (int x = 0; x < w; x++) {
        double t0 = pos_ms + (x + dx0) / ppm, t1 = t0 + 1.0 / ppm;
        if (t1 <= 0)
            continue;
        double f0 = t0 * fpm, f1 = t1 * fpm;
        if (f0 >= (double) t->n)
            break;
        uint32_t i0 = f0 > 0 ? (uint32_t) f0 : 0;
        double last = f1 - 1e-9; /* the frames that start before the column's end */
        uint32_t i1 = last < (double) t->n ? (uint32_t) last : t->n - 1;
        if (i1 < i0)
            i1 = i0;
        uint8_t lv = 0;
        for (uint32_t i = i0; i <= i1; i++)
            if (t->v[i] > lv)
                lv = t->v[i];
        col[x] = lv;
    }
}
#endif
