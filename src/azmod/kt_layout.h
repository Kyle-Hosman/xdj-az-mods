/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file kt_layout.h
 * @brief Key in Track Title, the pure parts: where a deck's key goes in its title row, which
 * text the row shows, and what the shared paint hook draws for each feature.
 *
 * The title row belongs to a deck's PlayerInfo widget. Its title Label ends before the row's
 * right end (2 DECK: the label stops at x 536 of a 620 wide row), so the key fits between the
 * title and the row's right end without the title ever running under it. With Hide Track
 * Title's eye showing, the key sits left of the eye, on the eye's band below ON AIR.
 *
 * The key's text is the deck's own tray KEY field (gui::waveform::KeyWidget's Label), copied
 * each time EP147 updates that widget. The copy is used only while it stays fresh: the title
 * setter runs once per frame for each deck on screen and ages it, the KeyWidget update resets
 * the age, so a widget that stops being updated stops showing a key. An empty text means draw
 * nothing.
 *
 * A tap on the key opens that deck's key shift panel (EP147's own, the one the tray's KEY row
 * arrow opens): kt_hit_rect is the tap target, kt_tap_owner decides whose tap it is (the key's,
 * the title's, or nobody's), kt_panel_at places the panel above the deck's title row.
 *
 * Self-contained so the host tests compile it as is (ktlayout_test.c).
 */
#ifndef AZMOD_KT_LAYOUT_H
#define AZMOD_KT_LAYOUT_H
#include <string.h>

#define KT_TEXT_MAX 16      /* a key text with its NUL ("12A", "F#m", "Abm") */
#define KT_STALE_FRAMES 30  /* title frames without a KeyWidget update: the copy is not used */
#define KT_GAP 8            /* between the key's right edge and the eye */
#define KT_MIN_W 16         /* narrower than this: no key */

struct kt_rect {
    int x, y, w, h;
};

/* The key's box in the row's coordinates, right-justified text inside it. title_right = the
 * title Label's right edge, eye_left = the eye's left edge or -1 when no eye is drawn,
 * row_right = the row's right end, pad_right = the gap to it. ty / th = the band the key is
 * centred in: the eye's, from the ON AIR badge's bottom to the bar's bottom, so the badge never
 * covers the key (the title Label's right part lies under the badge in every layout).
 * 1 = r is set; 0 = no room, draw nothing. */
static inline int kt_key_rect(int title_right, int eye_left, int row_right, int pad_right, int ty,
                              int th, struct kt_rect *r) {
    int right = eye_left >= 0 ? eye_left - KT_GAP : row_right - pad_right;
    if (right - title_right < KT_MIN_W || th <= 0)
        return 0;
    r->x = title_right;
    r->y = ty;
    r->w = right - title_right;
    r->h = th;
    return 1;
}

/* The KeyWidget's Label as EP147 left it: its text while it is visible, else none. Copies into
 * dst (KT_TEXT_MAX bytes) and returns 1 when that changed what dst held. A text that does not
 * fit is no key (never a cut one). */
static inline int kt_store(char *dst, const char *text, int visible) {
    const char *t = visible && text && strlen(text) < KT_TEXT_MAX ? text : "";
    if (!strcmp(dst, t))
        return 0;
    strcpy(dst, t);
    return 1;
}

/* One title frame for a deck: ages its copy (saturating). */
static inline void kt_age_tick(unsigned *age) {
    if (*age < KT_STALE_FRAMES)
        (*age)++;
}

/* The text the row shows: the copy while the switch is on and the copy is fresh, else none. */
static inline const char *kt_want(int on, unsigned age, const char *text) {
    return on && age < KT_STALE_FRAMES && text ? text : "";
}

/* What the shared paintOverChildren hook draws, each feature behind its own switch only:
 * the eye needs Hide Track Title on and a title on that deck; the key needs Key in Track Title
 * on and a key to show. Neither depends on the other's switch. */
struct kt_plan {
    int eye, key;
};
static inline struct kt_plan kt_plan_for(int ht_on, int has_title, int kt_on, const char *want) {
    struct kt_plan p;
    p.eye = ht_on && has_title;
    p.key = kt_on && want && *want;
    return p;
}

/* ---- tap the key: the deck's key shift panel ---- */
#define KT_PANEL_GAP 4    /* between the panel and the title row */
#define KT_PANEL_TOP 80   /* the WAVEFORM header ends here: the panel stays below it */
#define KT_PEND_FRAMES 10 /* title frames a claimed tap waits for the deck's KeyWidget update */

/* 1 when (x, y) lies inside r = {x, y, w, h} (an empty r holds nothing) */
static inline int kt_in(const int r[4], int x, int y) {
    return r[2] > 0 && r[3] > 0 && x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3];
}

/* The tap target in screen coordinates: the key's box, across the bar's full height (the ON AIR
 * band above the key holds no control). ox / oy = the row's screen origin; bar_top / bar_bottom
 * in the row's coordinates. */
static inline void kt_hit_rect(const struct kt_rect *box, int ox, int oy, int bar_top,
                               int bar_bottom, int hit[4]) {
    hit[0] = ox + box->x;
    hit[1] = oy + bar_top;
    hit[2] = box->w;
    hit[3] = bar_bottom - bar_top;
}

/* Whose tap on a title row: KT_TAP_KEY opens the panel, KT_TAP_TITLE is Hide Track Title's.
 * The key's needs Key in Track Title on, the key drawn, the point on the key, and JUCE's hit test
 * giving the point to the deck's own widget (nothing shown over it); Hide Track Title's switch
 * plays no part in it. A tap on the key is never also the title's, and a tap elsewhere on the bar
 * is never the key's. */
enum {
    KT_TAP_NONE,
    KT_TAP_KEY,
    KT_TAP_TITLE
};
static inline int kt_tap_owner(int kt_on, int key_shown, int in_key, int juce_ok, int ht_on,
                               int in_bar) {
    if (kt_on && key_shown && in_key)
        return juce_ok ? KT_TAP_KEY : KT_TAP_NONE;
    return ht_on && in_bar && juce_ok ? KT_TAP_TITLE : KT_TAP_NONE;
}

/* Where the w x h panel goes, in screen coordinates: right-aligned with the key (key_right), just
 * above the title row (bar_top), or just below the row (bar_bottom) when it does not fit above;
 * always inside area = {x, y, w, h} (the WAVEFORM view) and below top_min. 0 = no place. */
static inline int kt_panel_at(int w, int h, int key_right, int bar_top, int bar_bottom,
                              const int area[4], int top_min, int *x, int *y) {
    int lo = area[1] > top_min ? area[1] : top_min, hi = area[1] + area[3];
    if (w <= 0 || h <= 0 || w > area[2] || h > hi - lo)
        return 0;
    int px = key_right - w, py = bar_top - KT_PANEL_GAP - h;
    if (py < lo)
        py = bar_bottom + KT_PANEL_GAP;
    if (py + h > hi)
        py = hi - h;
    if (py < lo)
        py = lo;
    if (px + w > area[0] + area[2])
        px = area[0] + area[2] - w;
    if (px < area[0])
        px = area[0];
    *x = px;
    *y = py;
    return 1;
}
#endif
