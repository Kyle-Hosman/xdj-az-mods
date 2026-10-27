/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file browse_edit.h
 * @brief Column Edit, the pure part: the saved column widths (text form), which column a
 * touch on the header resizes, the widths fitted into the header, and what a header tap does.
 * No EP147 access; browse_edit.c feeds it what it reads from the header.
 *
 * A width file is text: "<column id>:<width>" pairs separated by spaces or newlines, at most
 * BE_FILE_MAX bytes. Any token that is not exactly that, or an id or width out of range, rejects
 * the whole file (nothing applies): a damaged file must never give a column a stray width.
 */
#ifndef BROWSE_EDIT_H
#define BROWSE_EDIT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define BE_MAXID 10       /* column ids 1..BE_MAXID (EP147 uses 1..8) */
#define BE_MINW 40        /* the narrowest a column can be dragged */
#define BE_MAXW 900       /* the widest a saved width can be */
#define BE_TITLE_MIN 100  /* the title column keeps at least this, as EP147's layout does */
#define BE_ZONE 20        /* a touch this close to a divider grabs it */
#define BE_FILE_MAX 256
#define BE_MAXCOLS 16     /* columns read from one header */

struct be_widths {
    int16_t w[BE_MAXID + 1]; /* 0: EP147's own width */
};

struct be_col {
    int id, width, visible;
};

/* Parse a width file. Returns the number of widths set (0..BE_MAXID), or -1 with *o all zero. */
static inline int be_parse(const char *s, size_t n, struct be_widths *o) {
    int count = 0;
    size_t i = 0;
    for (int k = 0; k <= BE_MAXID; k++)
        o->w[k] = 0;
    if (n > BE_FILE_MAX)
        goto bad;
    while (i < n) {
        char c = s[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            i++;
            continue;
        }
        long id = 0, w = 0;
        int dig = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9' && dig < 4) {
            id = id * 10 + (s[i++] - '0');
            dig++;
        }
        if (!dig || i >= n || s[i] != ':')
            goto bad;
        i++;
        dig = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9' && dig < 4) {
            w = w * 10 + (s[i++] - '0');
            dig++;
        }
        if (!dig || (i < n && s[i] != ' ' && s[i] != '\n' && s[i] != '\r' && s[i] != '\t'))
            goto bad;
        if (id < 1 || id > BE_MAXID || w < BE_MINW || w > BE_MAXW || o->w[id])
            goto bad;
        o->w[id] = (int16_t) w;
        count++;
    }
    return count;
bad:
    for (int k = 0; k <= BE_MAXID; k++)
        o->w[k] = 0;
    return -1;
}

/* The width file for *w (a trailing newline). Returns its length, or -1 if cap is too small. */
static inline int be_format(const struct be_widths *w, char *buf, size_t cap) {
    size_t len = 0;
    for (int id = 1; id <= BE_MAXID; id++) {
        if (w->w[id] < BE_MINW || w->w[id] > BE_MAXW)
            continue;
        int k = snprintf(buf + len, cap - len, "%s%d:%d", len ? " " : "", id, w->w[id]);
        if (k < 0 || (size_t) k >= cap - len)
            return -1;
        len += (size_t) k;
    }
    if (len + 2 > cap)
        return -1;
    buf[len++] = '\n';
    buf[len] = 0;
    return (int) len;
}

/* The column a touch at x resizes, through the divider within BE_ZONE of x (the nearest one).
 * Only the dividers between two visible columns count, never the header's right edge. The
 * column to the left of the divider is resized when it can be (sign +1: wider as the touch
 * moves right); otherwise the one to its right (sign -1). When both neighbors can be resized,
 * *partner is the right one and the two trade width (be_trade_width), so the divider follows
 * the touch and no other column moves; otherwise *partner is 0 and the title takes the
 * difference. The title is never resized: it takes what the others leave. `resizable` has
 * bit id set for the columns that can be resized. Returns the column id, or 0 for none. */
static inline int be_divider(const struct be_col *c, int n, int x, unsigned resizable, int title_id,
                             int *sign, int *partner) {
    int best = 0, bestd = BE_ZONE + 1, edge = 0, prev = -1;
    *sign = 0;
    *partner = 0;
    for (int i = 0; i < n; i++) {
        if (!c[i].visible)
            continue;
        if (prev >= 0) { /* the divider between c[prev] and c[i] is at edge */
            int d = x > edge ? x - edge : edge - x;
            if (d < bestd) {
                int l = c[prev].id, r = c[i].id, s = 0, id = 0;
                int lok = l != title_id && l >= 1 && l <= BE_MAXID && (resizable >> l & 1u);
                int rok = r != title_id && r >= 1 && r <= BE_MAXID && (resizable >> r & 1u);
                if (lok) {
                    id = l;
                    s = 1;
                } else if (rok) {
                    id = r;
                    s = -1;
                }
                if (id) {
                    best = id;
                    bestd = d;
                    *sign = s;
                    *partner = lok && rok ? r : 0;
                }
            }
        }
        edge += c[i].width;
        prev = i;
    }
    return best;
}

/* The dividers a touch can drag, as be_divider finds them: the x of each divider between two visible
 * columns of which one (not the title) can be resized, left to right, at most cap. Returns the
 * count. Edit mode marks these on the header. */
static inline int be_handles(const struct be_col *c, int n, unsigned resizable, int title_id, int *xs,
                             int cap) {
    int k = 0, edge = 0, prev = -1;
    for (int i = 0; i < n; i++) {
        if (!c[i].visible)
            continue;
        if (prev >= 0 && k < cap) {
            int l = c[prev].id, r = c[i].id;
            if ((l != title_id && l >= 1 && l <= BE_MAXID && (resizable >> l & 1u)) ||
                (r != title_id && r >= 1 && r <= BE_MAXID && (resizable >> r & 1u)))
                xs[k++] = edge;
        }
        edge += c[i].width;
        prev = i;
    }
    return k;
}

/* A drag: the width for a column that was w0 wide when the title was title0 wide, after the
 * touch moved dx. The title keeps BE_TITLE_MIN, the column BE_MINW. */
static inline int be_drag_width(int w0, int title0, int dx, int sign) {
    long w = (long) w0 + (long) sign * dx;
    long max = (long) w0 + title0 - BE_TITLE_MIN;
    if (max > BE_MAXW)
        max = BE_MAXW;
    if (w > max)
        w = max;
    if (w < BE_MINW)
        w = BE_MINW;
    return (int) w;
}

/* A drag on a divider between two resizable columns: the width for the column that was w0 wide,
 * its neighbor across the divider was p0 wide, after the touch moved dx; *pw gets the neighbor's.
 * The two keep their sum, each at least BE_MINW and at most BE_MAXW; two too narrow for that
 * keep their widths. */
static inline int be_trade_width(int w0, int p0, int dx, int sign, int *pw) {
    long sum = (long) w0 + p0, w = (long) w0 + (long) sign * dx;
    if (sum < 2 * BE_MINW) {
        *pw = p0;
        return w0;
    }
    long lo = sum - BE_MAXW, hi = sum - BE_MINW;
    if (lo < BE_MINW)
        lo = BE_MINW;
    if (hi > BE_MAXW)
        hi = BE_MAXW;
    if (w > hi)
        w = hi;
    if (w < lo)
        w = lo;
    *pw = (int) (sum - w);
    return (int) w;
}

/* The saved widths fitted into the header EP147 just laid out: each visible column with a saved
 * width takes it, and the title takes the rest. Where that would leave the title below
 * BE_TITLE_MIN, the saved columns give way from the right (never below BE_MINW); with no room
 * even then, EP147's widths stay. Returns 1 if a width changed. */
static inline int be_fit(struct be_col *c, int n, const struct be_widths *u, int title_id) {
    int total = 0, ti = -1, changed = 0;
    if (n > BE_MAXCOLS)
        n = BE_MAXCOLS;
    for (int i = 0; i < n; i++) {
        if (!c[i].visible)
            continue;
        total += c[i].width;
        if (c[i].id == title_id)
            ti = i;
    }
    if (ti < 0)
        return 0;
    int want[BE_MAXCOLS];
    for (int i = 0; i < n; i++) {
        int id = c[i].id;
        want[i] = c[i].width;
        if (c[i].visible && i != ti && id >= 1 && id <= BE_MAXID && u->w[id] >= BE_MINW && u->w[id] <= BE_MAXW)
            want[i] = u->w[id];
    }
    int others = 0;
    for (int i = 0; i < n; i++)
        if (c[i].visible && i != ti)
            others += want[i];
    for (int i = n - 1; i >= 0 && total - others < BE_TITLE_MIN; i--) {
        int id = c[i].id;
        if (!c[i].visible || i == ti || id < 1 || id > BE_MAXID || !u->w[id])
            continue;
        int give = want[i] - BE_MINW, need = BE_TITLE_MIN - (total - others);
        if (give > need)
            give = need;
        if (give > 0) {
            want[i] -= give;
            others -= give;
        }
    }
    if (total - others < BE_TITLE_MIN)
        return 0; /* no room: EP147's widths stay */
    for (int i = 0; i < n; i++) {
        int w = i == ti ? total - others : want[i];
        if (c[i].visible && c[i].width != w) {
            c[i].width = w;
            changed = 1;
        }
    }
    return changed;
}

/* What a touch that did not drag does on a header column. */
enum {
    BE_TAP_SORT,
    BE_TAP_NONE,
    BE_TAP_FIELD,
    BE_TAP_TOGGLE
};
/* `fixed` has bit id set for the columns that can be switched on and off, sub_id is the column
 * whose field EP147's own list picks. Outside edit mode a tap is EP147's (it sorts). */
static inline int be_tap(int editing, int id, unsigned fixed, int sub_id) {
    if (!editing)
        return BE_TAP_SORT;
    if (id == sub_id)
        return BE_TAP_FIELD;
    if (id >= 1 && id <= BE_MAXID && (fixed >> id & 1u))
        return BE_TAP_TOGGLE;
    return BE_TAP_NONE;
}

/* The search bar's inner widgets (one for the library search, one for the track filter) and
 * their children are placed once by EP147 with literal bounds, and nothing places them again when
 * the bar is resized: a narrower bar only clips its right end, where the clear button is. When
 * the bar gives up `sh` pixels, an inner widget loses sh of its width, the buttons at its right
 * end move left by sh and the text editor loses sh of its width. */
#define BE_SEARCH_W 544 /* the bar's stock width */
#define BE_SEARCH_H 50
#define BE_SEARCH_STEP 118 /* the bar only ever gives up a multiple of this */
#define BE_SEARCH_STEPS 2
enum {
    BE_SK_STRETCH,
    BE_SK_MOVE
};
static const struct {
    int16_t x, y, w, h;
    uint8_t kind;
} BE_SEARCH_KIDS[] = {
    {0, 0, BE_SEARCH_W, BE_SEARCH_H, BE_SK_STRETCH}, /* an inner widget */
    {511, 13, 25, 24, BE_SK_MOVE},                   /* library search: the clear button */
    {56, 9, 448, 40, BE_SK_STRETCH},                 /* library search: the text editor */
    {427, 13, 25, 24, BE_SK_MOVE},                   /* track filter: its two buttons */
    {466, 0, 78, 50, BE_SK_MOVE},
    {0, 9, 426, 40, BE_SK_STRETCH}, /* track filter: the text editor */
};
/* r: a child's bounds now, as EP147 placed it or as an earlier call moved it. Returns 1 with
 * out set to its bounds for a bar narrowed by sh, or 0 when r is none of the stock children at
 * any of the bar's widths (it is then left as it is). */
static inline int be_search_kid(const int r[4], int sh, int out[4]) {
    if (sh < 0 || sh > BE_SEARCH_STEP * BE_SEARCH_STEPS)
        return 0;
    for (size_t i = 0; i < sizeof BE_SEARCH_KIDS / sizeof BE_SEARCH_KIDS[0]; i++)
        for (int k = 0; k <= BE_SEARCH_STEPS; k++) {
            int was = k * BE_SEARCH_STEP, mv = BE_SEARCH_KIDS[i].kind == BE_SK_MOVE;
            if (r[0] != BE_SEARCH_KIDS[i].x - (mv ? was : 0) || r[1] != BE_SEARCH_KIDS[i].y ||
                r[2] != BE_SEARCH_KIDS[i].w - (mv ? 0 : was) || r[3] != BE_SEARCH_KIDS[i].h)
                continue;
            out[0] = BE_SEARCH_KIDS[i].x - (mv ? sh : 0);
            out[1] = BE_SEARCH_KIDS[i].y;
            out[2] = BE_SEARCH_KIDS[i].w - (mv ? 0 : sh);
            out[3] = BE_SEARCH_KIDS[i].h;
            return 1;
        }
    return 0;
}

#endif
