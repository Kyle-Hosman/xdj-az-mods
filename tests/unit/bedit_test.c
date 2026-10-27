/* SPDX-License-Identifier: GPL-3.0-only */
/* Column Edit, the pure part (guest/azmod/browse_edit.h):
 *   - the width file: written widths read back the same; any malformed token, duplicate id, id
 *     or width out of range, or oversized file rejects the whole file and sets nothing;
 *   - the divider hit test: the nearest divider within BE_ZONE, never the header's right edge,
 *     never the title, the left column first and the right one (sign -1) when the left cannot be
 *     resized, hidden columns skipped;
 *   - a drag keeps the title at BE_TITLE_MIN and the column at BE_MINW;
 *   - saved widths fitted into a header: the title takes the rest, saved columns give way from
 *     the right when the title would go below BE_TITLE_MIN, EP147's widths stay when there is no
 *     room even then, the total width never changes;
 *   - a tap sorts outside edit mode; in edit mode the sub column picks a field, a fixed column
 *     toggles, anything else does nothing;
 *   - the search bar narrowed: its clear button and the buttons beside it move left by the room
 *     given up, the text editor and the inner widget shrink by it, from EP147's places or from
 *     an earlier shift, and back; anything else, or a shift out of range, is left alone. */
#include "check.h"
#include <string.h>
#include "browse_edit.h"

#define BCHECK(cond, name) CHECK(name, cond)

static int total(const struct be_col *c, int n) {
    int t = 0;
    for (int i = 0; i < n; i++)
        if (c[i].visible)
            t += c[i].width;
    return t;
}

int main(void) {
    ut_suite = "browse edit";
    struct be_widths w, r;
    char buf[BE_FILE_MAX + 8];

    /* ---- the file */
    memset(&w, 0, sizeof w);
    w.w[3] = 60;
    w.w[6] = 345;
    w.w[8] = 120;
    int len = be_format(&w, buf, sizeof buf);
    BCHECK(len > 0 && !strcmp(buf, "3:60 6:345 8:120\n"), "format: ids in order, newline");
    BCHECK(be_parse(buf, (size_t) len, &r) == 3 && !memcmp(&r, &w, sizeof w), "round trip");
    BCHECK(be_parse("", 0, &r) == 0, "empty file: nothing set");
    BCHECK(be_parse(" 6:200\n\n", 8, &r) == 1 && r.w[6] == 200, "whitespace around a pair");
    static const char *const bad[] = {"6:200x", "6;200", ":200", "6:", "0:200", "11:200", "6:39",
                                      "6:901", "6:200 6:300", "6:200 7", "-6:200", "6:2000",
                                      "66666:200", "6:+200", "6:200\x01"};
    for (unsigned k = 0; k < sizeof bad / sizeof *bad; k++) {
        r.w[6] = 77;
        int rc = be_parse(bad[k], strlen(bad[k]), &r);
        int clear = 1;
        for (int i = 0; i <= BE_MAXID; i++)
            clear &= r.w[i] == 0;
        BCHECK(rc == -1 && clear, "rejected whole, nothing set");
    }
    BCHECK(be_parse("3:60 6:200", 10, &r) == 2, "two pairs, no trailing newline");
    {
        char big[BE_FILE_MAX + 2];
        memset(big, ' ', sizeof big);
        memcpy(big, "6:200", 5);
        BCHECK(be_parse(big, sizeof big, &r) == -1, "oversized file rejected");
        BCHECK(be_parse(big, BE_FILE_MAX, &r) == 1, "a file of exactly BE_FILE_MAX is read");
    }
    BCHECK(be_format(&w, buf, 8) == -1, "format refuses a short buffer");

    /* ---- the divider hit test: v1.30 full-width list, artwork 52 | title 4 | sub 5 | BPM 6 | Key 7 */
    struct be_col c[] = {{1, 0, 0}, {2, 52, 1}, {3, 78, 0}, {4, 500, 1}, {5, 345, 1}, {6, 110, 1}, {7, 90, 1}};
    int n = (int) (sizeof c / sizeof *c);
    unsigned rs = 1u << 3 | 1u << 5 | 1u << 6 | 1u << 7; /* #, sub, BPM, Key */
    int sign = 9, part = 9;
    /* edges: 52 (art|title), 552 (title|sub), 897 (sub|BPM), 1007 (BPM|Key), 1097 (header end) */
    BCHECK(be_divider(c, n, 552, rs, 4, &sign, &part) == 5 && sign == -1, "title|sub: the sub column, from its left");
    BCHECK(be_divider(c, n, 552, rs | 1u << 4, 4, &sign, &part) == 5 && sign == -1, "the title is never resized, even if marked");
    BCHECK(part == 0, "title|sub: no partner, the title takes the difference");
    BCHECK(be_divider(c, n, 897 + 12, rs, 4, &sign, &part) == 5 && sign == 1 && part == 6,
           "sub|BPM: the sub column, trading with BPM");
    BCHECK(be_divider(c, n, 1007 - 20, rs, 4, &sign, &part) == 6 && sign == 1 && part == 7,
           "BPM|Key at the zone's edge, trading with Key");
    BCHECK(be_divider(c, n, 1007 - 21, rs, 4, &sign, &part) == 0 && sign == 0, "just outside the zone: none");
    BCHECK(be_divider(c, n, 1096, rs, 4, &sign, &part) == 0, "the header's right edge is not a divider");
    BCHECK(be_divider(c, n, 52, rs, 4, &sign, &part) == 0, "art|title: neither can be resized");
    BCHECK(be_divider(c, n, 700, rs, 4, &sign, &part) == 0, "the middle of a column: none");
    {
        int xs[BE_MAXCOLS];
        int k = be_handles(c, n, rs, 4, xs, BE_MAXCOLS);
        BCHECK(k == 3 && xs[0] == 552 && xs[1] == 897 && xs[2] == 1007,
               "handles: title|sub, sub|BPM, BPM|Key; not art|title, not the header's end");
        int same = 1;
        for (int x = -30; x < 1130; x++) {
            int near = 0;
            for (int i = 0; i < k; i++)
                near |= x >= xs[i] - BE_ZONE && x <= xs[i] + BE_ZONE;
            same &= near == (be_divider(c, n, x, rs, 4, &sign, &part) != 0);
        }
        BCHECK(same, "a touch drags exactly where a handle is marked, at every x");
        k = be_handles(c, n, rs | 1u << 2, 4, xs, BE_MAXCOLS);
        BCHECK(k == 4 && xs[0] == 52, "artwork resizable: art|title marked too");
        BCHECK(be_handles(c, n, rs, 4, xs, 2) == 2, "at most cap handles");
        BCHECK(be_handles(c, n, 0, 4, xs, BE_MAXCOLS) == 0, "nothing resizable: no handles");
    }
    c[2].visible = 1; /* # shown: art 52 | # 78 | title 500: edges 52, 130, 630 ... */
    BCHECK(be_divider(c, n, 54, rs, 4, &sign, &part) == 3 && sign == -1, "art|#: # from its left");
    BCHECK(be_divider(c, n, 128, rs, 4, &sign, &part) == 3 && sign == 1, "#|title: #");
    c[2].visible = 0;
    {
        struct be_col d[] = {{6, 100, 1}, {7, 100, 1}};
        BCHECK(be_divider(d, 2, 105, rs, 4, &sign, &part) == 6 && sign == 1, "the nearest of two candidates");
        BCHECK(be_divider(d, 2, 95, rs, 4, &sign, &part) == 6, "either side of a divider");
    }

    /* ---- a drag */
    BCHECK(be_drag_width(110, 500, 30, 1) == 140, "wider to the right");
    BCHECK(be_drag_width(110, 500, 30, -1) == 80, "sign -1: narrower to the right");
    BCHECK(be_drag_width(110, 500, -100, 1) == BE_MINW, "never below BE_MINW");
    BCHECK(be_drag_width(110, 500, 1000, 1) == 110 + 500 - BE_TITLE_MIN, "the title keeps BE_TITLE_MIN");
    BCHECK(be_drag_width(800, 900, 1000, 1) == BE_MAXW, "never above BE_MAXW");
    {
        int pw = 0;
        BCHECK(be_trade_width(110, 90, -30, 1, &pw) == 80 && pw == 120, "trade: left 30, the right one grows");
        BCHECK(be_trade_width(110, 90, 30, 1, &pw) == 140 && pw == 60, "trade: right 30, the right one shrinks");
        BCHECK(be_trade_width(110, 90, -500, 1, &pw) == BE_MINW && pw == 200 - BE_MINW, "trade: left stops at BE_MINW");
        BCHECK(be_trade_width(110, 90, 500, 1, &pw) == 200 - BE_MINW && pw == BE_MINW, "trade: right stops at BE_MINW");
        BCHECK(be_trade_width(800, 800, 500, 1, &pw) == BE_MAXW && pw == 700, "trade: never above BE_MAXW");
        BCHECK(be_trade_width(30, 40, 10, 1, &pw) == 30 && pw == 40, "trade: two too narrow stay");
    }
    {
        /* the right edge, as the list shows it: art 52 | title | (sub) | BPM 110 | Key 90, header 1097.
         * Each divider right of the title dragged 30 left, then 30 right, then RESET: the divider follows
         * the touch, only its two neighbors change, the header's end and the title stay put. */
        struct be_col e0[] = {{2, 52, 1}, {4, 500, 1}, {5, 345, 0}, {6, 110, 1}, {7, 90, 1}};
        int ok = 1;
        for (int sub = 0; sub <= 1; sub++) {
            struct be_col e[5];
            memcpy(e, e0, sizeof e);
            e[2].visible = sub;
            if (!sub)
                e[1].width += 345;
            int ndiv = sub ? 2 : 1;
            for (int k = 0; k < ndiv; k++) {
                int L = 4 - ndiv + k, R = L + 1, xd = 0; /* the divider between e[L] and e[R] */
                for (int i = 0; i <= L; i++)
                    xd += e[i].visible ? e[i].width : 0;
                for (int dx = -30; dx <= 30; dx += 60) {
                    struct be_col f[5];
                    struct be_widths u;
                    memcpy(f, e, sizeof f);
                    memset(&u, 0, sizeof u);
                    int s = 0, pt = 0, id = be_divider(f, 5, xd + 3, rs, 4, &s, &pt), pw = 0;
                    ok &= id == e[L].id && pt == e[R].id;
                    if (!id || !pt)
                        continue;
                    u.w[id] = (int16_t) be_trade_width(e[L].width, e[R].width, dx, s, &pw);
                    u.w[pt] = (int16_t) pw;
                    be_fit(f, 5, &u, 4);
                    int nx = 0;
                    for (int i = 0; i <= L; i++)
                        nx += f[i].visible ? f[i].width : 0;
                    ok &= nx == xd + dx && f[R].width == e[R].width - dx && f[1].width == e[1].width &&
                          total(f, 5) == total(e, 5);
                    for (int i = 0; i < 5; i++)
                        ok &= i == L || i == R || f[i].width == e[i].width;
                    memset(&u, 0, sizeof u); /* RESET: EP147's widths back */
                    memcpy(f, e, sizeof f);
                    ok &= !be_fit(f, 5, &u, 4) && !memcmp(f, e, sizeof f);
                }
            }
        }
        BCHECK(ok, "right of the title, each divider follows the touch and only its neighbors change (sub off and on)");
    }

    /* ---- the fit */
    memset(&w, 0, sizeof w);
    w.w[6] = 200;
    w.w[5] = 250;
    int t0 = total(c, n);
    BCHECK(be_fit(c, n, &w, 4) == 1 && c[5].width == 200 && c[4].width == 250 && c[3].width == 505 &&
               c[1].width == 52 && total(c, n) == t0,
           "saved widths taken, the title takes the rest, total kept");
    BCHECK(be_fit(c, n, &w, 4) == 0, "fitting again changes nothing");
    w.w[2] = 99; /* art: no saved width is honoured unless the column is visible and not the title */
    w.w[4] = 300;
    c[3].width = 505;
    be_fit(c, n, &w, 4);
    BCHECK(c[3].width == 505 - 47 && c[1].width == 99, "the title's own saved width is ignored");
    w.w[2] = 0;
    w.w[4] = 0;
    {
        struct be_col e[] = {{4, 300, 1}, {5, 100, 1}, {6, 100, 1}};
        struct be_widths u;
        memset(&u, 0, sizeof u);
        u.w[5] = 200;
        u.w[6] = 250; /* would leave the title 50 */
        BCHECK(be_fit(e, 3, &u, 4) == 1 && e[0].width == BE_TITLE_MIN && e[1].width == 200 && e[2].width == 200 &&
                   total(e, 3) == 500,
               "the rightmost saved column gives way first");
        struct be_col f[] = {{4, 150, 1}, {5, 100, 1}};
        u.w[5] = 900;
        BCHECK(be_fit(f, 2, &u, 4) == 1 && f[0].width == BE_TITLE_MIN && f[1].width == 150, "a wide one shrinks");
        struct be_col g[] = {{4, 100, 1}, {5, 20, 1}, {6, 20, 1}};
        u.w[5] = 60;
        u.w[6] = 60;
        BCHECK(be_fit(g, 3, &u, 4) == 0 && g[0].width == 100 && g[1].width == 20, "no room: EP147's widths stay");
        struct be_col h[] = {{5, 100, 1}, {6, 100, 1}};
        BCHECK(be_fit(h, 2, &u, 4) == 0 && h[0].width == 100, "no title column: nothing changes");
    }

    /* ---- a tap */
    unsigned fixed = 1u << 2 | 1u << 3 | 1u << 6 | 1u << 7;
    BCHECK(be_tap(0, 6, fixed, 5) == BE_TAP_SORT && be_tap(0, 5, fixed, 5) == BE_TAP_SORT, "not editing: EP147 sorts");
    BCHECK(be_tap(1, 5, fixed, 5) == BE_TAP_FIELD, "editing, the sub column: its field");
    BCHECK(be_tap(1, 6, fixed, 5) == BE_TAP_TOGGLE && be_tap(1, 2, fixed, 5) == BE_TAP_TOGGLE, "editing, fixed: toggle");
    BCHECK(be_tap(1, 4, fixed, 5) == BE_TAP_NONE && be_tap(1, 0, fixed, 5) == BE_TAP_NONE &&
               be_tap(1, 99, fixed, 5) == BE_TAP_NONE,
           "editing, the title or no column: nothing");
    /* ---- the search bar's children */
    {
        int o[4];
        const int x0[4] = {511, 13, 25, 24}, ed0[4] = {56, 9, 448, 40}, in0[4] = {0, 0, 544, 50};
        BCHECK(be_search_kid(x0, 118, o) && o[0] == 393 && o[1] == 13 && o[2] == 25 && o[3] == 24,
               "search: the clear button moves left by the room given up");
        const int x1[4] = {393, 13, 25, 24};
        BCHECK(be_search_kid(x1, 236, o) && o[0] == 275 && o[2] == 25, "search: from one shift to two");
        BCHECK(be_search_kid(x1, 0, o) && o[0] == 511, "search: back to EP147's place");
        BCHECK(be_search_kid(ed0, 118, o) && o[0] == 56 && o[2] == 330 && o[3] == 40,
               "search: the text editor loses the room, stays where it starts");
        const int ed2[4] = {56, 9, 212, 40};
        BCHECK(be_search_kid(ed2, 0, o) && o[2] == 448, "search: the editor back to its width");
        BCHECK(be_search_kid(in0, 118, o) && o[0] == 0 && o[2] == 426 && o[3] == 50, "search: an inner widget shrinks");
        const int tb[4] = {466, 0, 78, 50}, te[4] = {0, 9, 426, 40};
        BCHECK(be_search_kid(tb, 118, o) && o[0] == 348 && o[2] == 78, "search: the track filter's button moves");
        BCHECK(be_search_kid(te, 118, o) && o[0] == 0 && o[2] == 308, "search: the track filter's editor shrinks");
        const int odd[4] = {510, 13, 25, 24}, odd2[4] = {511, 13, 25, 24 + 1};
        BCHECK(!be_search_kid(odd, 118, o) && !be_search_kid(odd2, 118, o), "search: a child not at a stock place is left");
        BCHECK(!be_search_kid(x0, 354, o) && !be_search_kid(x0, -1, o), "search: a shift out of range is refused");
    }
    return ut_summary();
}
