/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file wave_zoom.h
 * @brief Remember Waveform Zoom: the saved zoom's text, the levels EP147 can show, and when a
 *        zoom may be restored or saved (wave_zoom.c).
 *
 * EP147 keeps the detailed waveform's zoom as one ratio of two u16 in its static data, the
 * numerator first, shared by every deck. Its step function only ever leaves one of six
 * levels there: 2/1, 1/1, 1/2, 1/4 (the value it starts with), 1/8 and 1/16. Both halves are
 * read as one little-endian u32 (num | den << 16), the way they lie in memory.
 *
 * The saved value is text in azmod.conf, "<num>/<den>" with no spaces or leading zeros, and
 * one of the six levels; anything else reads as no saved value (WZ_NONE), never as a guess.
 *
 * Pure: no EP147 memory, no files, no threads, so the host tests compile it as is.
 */
#ifndef AZMOD_WAVE_ZOOM_H
#define AZMOD_WAVE_ZOOM_H
#include <stdint.h>
#include <stdio.h>

#define WZ_NONE 0u                 /* no saved value (0/0 is never a level) */
#define WZ_PACK(num, den) ((uint32_t) (num) | (uint32_t) (den) << 16)
#define WZ_STOCK WZ_PACK(1, 4)     /* EP147's start value */
#define WZ_NUM(v) ((unsigned) ((v) & 0xffffu))
#define WZ_DEN(v) ((unsigned) ((v) >> 16))
#define WZ_TEXT 12                 /* room for any "<u16>/<u16>" */
#define WZ_SETTLE_MS 1000L         /* a new zoom is saved once it has stayed this long */

/* One of the six levels EP147's step function produces. */
static inline int wz_level_ok(uint32_t v) {
    unsigned n = WZ_NUM(v), d = WZ_DEN(v);
    if (n == 2)
        return d == 1;
    return n == 1 && (d == 1 || d == 2 || d == 4 || d == 8 || d == 16);
}

/* "<num>/<den>" -> the packed level, or WZ_NONE (NULL, empty, malformed or not a level). */
static inline uint32_t wz_parse(const char *s) {
    unsigned part[2] = {0, 0};
    if (!s)
        return WZ_NONE;
    for (int k = 0; k < 2; k++) {
        int digits = 0;
        if (*s < '1' || *s > '9') /* no sign, no space, no leading zero */
            return WZ_NONE;
        while (*s >= '0' && *s <= '9') {
            if (++digits > 2) /* the largest part is 16 */
                return WZ_NONE;
            part[k] = part[k] * 10u + (unsigned) (*s++ - '0');
        }
        if (k == 0 && *s++ != '/')
            return WZ_NONE;
    }
    if (*s)
        return WZ_NONE;
    uint32_t v = WZ_PACK(part[0], part[1]);
    return wz_level_ok(v) ? v : WZ_NONE;
}

/* A level as the text wz_parse reads back; "" for anything that is not a level. */
static inline void wz_format(char *out, size_t n, uint32_t v) {
    if (!out || !n)
        return;
    if (wz_level_ok(v))
        snprintf(out, n, "%u/%u", WZ_NUM(v), WZ_DEN(v));
    else
        out[0] = 0;
}

/* At start-up: write `saved` over the live value only when the live value is EP147's start
 * value (so a wrong address, or a build that starts elsewhere, writes nothing) and `saved` is
 * one of the levels. */
static inline int wz_restore_ok(uint32_t live, uint32_t saved) {
    return live == WZ_STOCK && wz_level_ok(saved);
}

/* The saver's view of the live value, one reader thread. A value counts once it is a level and
 * has read the same for WZ_SETTLE_MS: a half-written pair or a knob still turning is never
 * saved, and a sweep through the levels is one save. */
struct wz_watch {
    uint32_t seen;  /* the value last read */
    long since_ms;  /* when it was first read */
};
static inline void wz_watch_init(struct wz_watch *w) {
    w->seen = WZ_NONE;
    w->since_ms = 0;
}
/* 1 = save `live` now: it is a level, it has settled and it is not the saved value. */
static inline int wz_watch_step(struct wz_watch *w, uint32_t live, uint32_t saved, long now_ms) {
    if (live != w->seen) {
        w->seen = live;
        w->since_ms = now_ms;
        return 0;
    }
    return wz_level_ok(live) && live != saved && now_ms - w->since_ms >= WZ_SETTLE_MS;
}
#endif
