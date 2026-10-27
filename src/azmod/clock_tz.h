/* SPDX-License-Identifier: GPL-3.0-only */
/* clock_tz.h: local time for the Clock readout from a TZif zone file (RFC 8536). Pure logic:
 * the caller reads the file; no I/O, no allocation, no global state.
 *
 * Only the version 2+ part is used (64-bit transition times). A time past the file's last
 * transition is answered only when the footer's POSIX rule has no daylight-saving part:
 * otherwise the offset would be a guess, and the clock shows no time rather than a wrong one. */
#ifndef CLOCK_TZ_H
#define CLOCK_TZ_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define CTZ_MAX_TRANS 1200
#define CTZ_MAX_TYPES 32
#define CTZ_MAX_OFF (26 * 3600) /* the widest UT offset any zone has used, with margin */

struct ctz {
    int n_trans, n_types;
    int64_t at[CTZ_MAX_TRANS];       /* transition times, strictly increasing */
    unsigned char idx[CTZ_MAX_TRANS]; /* the type in force from at[i] */
    int32_t utoff[CTZ_MAX_TYPES];
    int rule_dst; /* the footer rule has a DST part (or is missing): after at[last] is unknown */
};

static inline uint64_t ctz_be(const unsigned char *p, int nb) {
    uint64_t v = 0;
    for (int i = 0; i < nb; i++)
        v = v << 8 | p[i];
    return v;
}

/* The six counts of a header at b (44 bytes): isut, isstd, leap, time, type, char. */
static inline void ctz_counts(const unsigned char *b, uint64_t c[6]) {
    for (int i = 0; i < 6; i++)
        c[i] = ctz_be(b + 20 + 4 * i, 4);
}
/* Bytes of the data block after a header, with times of tsz bytes. */
static inline uint64_t ctz_block(const uint64_t c[6], int tsz) {
    return c[3] * (uint64_t) (tsz + 1) + c[4] * 6 + c[5] + c[2] * (uint64_t) (tsz + 4) + c[1] + c[0];
}

/* A POSIX TZ rule (the footer, without its newlines) has a DST part when anything follows the
 * standard name and offset. A rule this cannot read counts as DST (unknown). */
static inline int ctz_rule_has_dst(const char *s, size_t n) {
    size_t i = 0;
    if (i < n && s[i] == '<') {
        while (i < n && s[i] != '>')
            i++;
        if (i == n)
            return 1;
        i++;
    } else {
        size_t st = i;
        while (i < n && ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z')))
            i++;
        if (i - st < 3)
            return 1;
    }
    if (i < n && (s[i] == '+' || s[i] == '-'))
        i++;
    size_t d0 = i;
    while (i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == ':'))
        i++;
    if (i == d0)
        return 1;
    return i != n;
}

/* Parse a whole zone file. 0 = ok, -1 = not a version 2+ TZif file this can use. */
static inline int ctz_parse(const unsigned char *b, size_t n, struct ctz *z) {
    uint64_t c[6];
    z->n_trans = z->n_types = 0;
    z->rule_dst = 1;
    if (n < 44 || b[0] != 'T' || b[1] != 'Z' || b[2] != 'i' || b[3] != 'f' || b[4] < '2')
        return -1;
    ctz_counts(b, c);
    uint64_t skip = 44 + ctz_block(c, 4);
    if (skip + 44 > n)
        return -1;
    const unsigned char *h = b + skip;
    if (h[0] != 'T' || h[1] != 'Z' || h[2] != 'i' || h[3] != 'f')
        return -1;
    ctz_counts(h, c);
    if (c[3] > CTZ_MAX_TRANS || c[4] < 1 || c[4] > CTZ_MAX_TYPES || c[0] > c[4] || c[1] > c[4] ||
        c[2] > 1000 || c[5] > 1000)
        return -1;
    uint64_t len = ctz_block(c, 8);
    if (len > n - skip - 44)
        return -1;
    const unsigned char *p = h + 44, *ix = p + c[3] * 8, *ty = ix + c[3];
    for (uint64_t i = 0; i < c[4]; i++) {
        int32_t off = (int32_t) (uint32_t) ctz_be(ty + 6 * i, 4);
        if (off < -CTZ_MAX_OFF || off > CTZ_MAX_OFF || ty[6 * i + 4] > 1 || ty[6 * i + 5] >= c[5])
            return -1;
        z->utoff[i] = off;
    }
    for (uint64_t i = 0; i < c[3]; i++) {
        int64_t t = (int64_t) ctz_be(p + 8 * i, 8);
        if (ix[i] >= c[4] || (i > 0 && t <= z->at[i - 1]))
            return -1;
        z->at[i] = t;
        z->idx[i] = ix[i];
    }
    z->n_trans = (int) c[3];
    z->n_types = (int) c[4];
    /* the footer: "\n<rule>\n" right after the data block */
    const unsigned char *f = p + len, *end = b + n;
    if (f < end && *f == '\n') {
        const unsigned char *q = f + 1;
        while (q < end && *q != '\n')
            q++;
        if (q < end)
            z->rule_dst = ctz_rule_has_dst((const char *) f + 1, (size_t) (q - f - 1));
    }
    return 0;
}

/* The UT offset at UTC time t: 0 and *off set, or -1 = not known from this file. */
static inline int ctz_offset(const struct ctz *z, int64_t t, int32_t *off) {
    if (z->n_types < 1)
        return -1;
    if (z->n_trans == 0 || t < z->at[0]) {
        if (z->n_trans == 0 && z->rule_dst)
            return -1;
        *off = z->utoff[0];
        return 0;
    }
    int lo = 0, hi = z->n_trans - 1; /* the last at[i] <= t */
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (z->at[mid] <= t)
            lo = mid;
        else
            hi = mid - 1;
    }
    if (lo == z->n_trans - 1 && z->rule_dst)
        return -1;
    *off = z->utoff[z->idx[lo]];
    return 0;
}

/* "1:05 PM" (12-hour) or "13:05" (24-hour) for UTC time t at offset off; with secs,
 * "1:05:09 PM" or "13:05:09". */
static inline void ctz_format(char *out, size_t n, int64_t t, int32_t off, int h24, int secs) {
    struct tm tm;
    time_t lt = (time_t) ((t < -(1LL << 40) || t > (1LL << 40)) ? 0 : t + off);
    if (t < -(1LL << 40) || t > (1LL << 40) || !gmtime_r(&lt, &tm)) {
        snprintf(out, n, "--:--");
        return;
    }
    int h12 = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
    const char *ap = tm.tm_hour < 12 ? "AM" : "PM";
    if (h24 && secs)
        snprintf(out, n, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    else if (h24)
        snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
    else if (secs)
        snprintf(out, n, "%d:%02d:%02d %s", h12, tm.tm_min, tm.tm_sec, ap);
    else
        snprintf(out, n, "%d:%02d %s", h12, tm.tm_min, ap);
}
/* The offset as the zone list shows it: "UTC-7", "UTC+5:30", "UTC+0". Minutes only where the
 * offset has them; an offset no zone has (past CTZ_MAX_OFF) is "UTC?". */
static inline void ctz_utc_label(char *out, size_t n, int32_t off) {
    if (off < -CTZ_MAX_OFF || off > CTZ_MAX_OFF) {
        snprintf(out, n, "UTC?");
        return;
    }
    int32_t a = off < 0 ? -off : off, h = a / 3600, m = a % 3600 / 60;
    if (m)
        snprintf(out, n, "UTC%c%d:%02d", off < 0 ? '-' : '+', (int) h, (int) m);
    else
        snprintf(out, n, "UTC%c%d", off < 0 ? '-' : '+', (int) h);
}
#endif
