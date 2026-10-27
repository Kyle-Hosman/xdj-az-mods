/* SPDX-License-Identifier: GPL-3.0-only */
/* Key in Track Title: the pure parts (guest/azmod/kt_layout.h), with the geometry the emulator
 * measured (2 DECK: title Label right edge 536, row right end 619, eye left 588, ON AIR band
 * 21..49; 4 DECK: 441, 519, 488, 21..39):
 *   - the key's box starts at the title Label's right edge, so the title never runs under it;
 *   - it ends KT_GAP left of the eye, or at the row's end less the pad without an eye;
 *   - no room, or no band, is no key;
 *   - the copy: a visible Label's text, none while it is hidden, none when it does not fit,
 *     and only a change reports a change;
 *   - freshness: the copy shows until KT_STALE_FRAMES title frames pass without an update;
 *   - the gate split: the eye follows Hide Track Title only, the key Key in Track Title only;
 *   - tap the key: the tap target is the key's box over the bar's height; a tap on the shown key
 *     is the key's whatever Hide Track Title's switch, never also the title's; a tap elsewhere on
 *     the bar is never the key's; nothing over the row takes it; the key shift panel sits above
 *     the title row, right-aligned with the key, inside the WAVEFORM view, never off-screen.
 * KTLAYOUT_HDR = the header under test. */
#include "check.h"
#include <stdio.h>
#include <string.h>
#include KTLAYOUT_HDR

int main(void) {
    ut_suite = "key in title";
    struct kt_rect r = {0, 0, 0, 0};
    /* 2 DECK, eye shown */
    CHECK("2 DECK with the eye: a box", kt_key_rect(536, 588, 619, 7, 21, 28, &r));
    CHECK("2 DECK with the eye: starts at the title's right edge", r.x == 536);
    CHECK("2 DECK with the eye: ends KT_GAP left of the eye", r.x + r.w == 588 - KT_GAP);
    CHECK("2 DECK with the eye: the ON AIR band", r.y == 21 && r.h == 28);
    /* 2 DECK, no eye: the eye's place */
    CHECK("2 DECK without the eye: a box", kt_key_rect(536, -1, 619, 7, 21, 28, &r));
    CHECK("2 DECK without the eye: ends at the row less the pad", r.x + r.w == 612);
    /* 4 DECK */
    CHECK("4 DECK with the eye: a box", kt_key_rect(441, 488, 519, 7, 21, 18, &r));
    CHECK("4 DECK with the eye: never left of the title", r.x >= 441 && r.w == 488 - KT_GAP - 441);
    /* no room */
    CHECK("a title Label reaching the eye: no key", !kt_key_rect(580, 588, 619, 7, 21, 28, &r));
    CHECK("a box just under the minimum: no key", !kt_key_rect(588 - KT_GAP - KT_MIN_W + 1, 588, 619, 7, 21, 28, &r));
    CHECK("a box at the minimum: a key", kt_key_rect(588 - KT_GAP - KT_MIN_W, 588, 619, 7, 21, 28, &r));
    CHECK("no band: no key", !kt_key_rect(536, 588, 619, 7, 21, 0, &r));
    CHECK("an eye at 0 is an eye, not 'none'", !kt_key_rect(536, 0, 619, 7, 21, 28, &r));

    /* the copy */
    char t[KT_TEXT_MAX] = "";
    CHECK("a visible key is copied", kt_store(t, "Fm", 1) && !strcmp(t, "Fm"));
    CHECK("the same key again is no change", !kt_store(t, "Fm", 1));
    CHECK("a Camelot key replaces it", kt_store(t, "4A", 1) && !strcmp(t, "4A"));
    CHECK("a hidden Label is no key (its placeholder is not shown)", kt_store(t, "F#m", 0) && !strcmp(t, ""));
    CHECK("still hidden: no change", !kt_store(t, "F#m", 0));
    CHECK("no text: no key", !kt_store(t, NULL, 1) && !strcmp(t, ""));
    char longt[KT_TEXT_MAX + 4];
    memset(longt, 'x', sizeof longt - 1);
    longt[sizeof longt - 1] = 0;
    kt_store(t, "12B", 1);
    CHECK("a text that does not fit is no key, never a cut one", kt_store(t, longt, 1) && !strcmp(t, ""));

    /* freshness */
    unsigned age = 0;
    CHECK("fresh: shown", !strcmp(kt_want(1, age, "Fm"), "Fm"));
    for (int i = 0; i < KT_STALE_FRAMES - 1; i++)
        kt_age_tick(&age);
    CHECK("one frame before stale: shown", !strcmp(kt_want(1, age, "Fm"), "Fm"));
    kt_age_tick(&age);
    CHECK("stale: nothing", !strcmp(kt_want(1, age, "Fm"), ""));
    for (int i = 0; i < 1000; i++)
        kt_age_tick(&age);
    CHECK("the age saturates", age == KT_STALE_FRAMES);
    age = 0; /* the KeyWidget update */
    CHECK("an update makes it fresh again", !strcmp(kt_want(1, age, "Fm"), "Fm"));
    CHECK("switch off: nothing", !strcmp(kt_want(0, 0, "Fm"), ""));
    CHECK("no copy: nothing", !strcmp(kt_want(1, 0, NULL), ""));

    /* the gate split */
    struct kt_plan p = kt_plan_for(0, 1, 1, "Fm");
    CHECK("Hide Track Title off, key on: the key alone", !p.eye && p.key);
    p = kt_plan_for(1, 1, 0, "Fm");
    CHECK("Hide Track Title on, key off: the eye alone", p.eye && !p.key);
    p = kt_plan_for(1, 1, 1, "Fm");
    CHECK("both on: both", p.eye && p.key);
    p = kt_plan_for(1, 0, 1, "Fm");
    CHECK("no title on the deck: no eye, the key still shows", !p.eye && p.key);
    p = kt_plan_for(1, 1, 1, "");
    CHECK("no key: the eye only", p.eye && !p.key);
    p = kt_plan_for(0, 0, 0, "Fm");
    CHECK("both off: nothing", !p.eye && !p.key);

    /* tap the key: the target (2 DECK deck 1: row at 10,561; key box 536..612, bar 0..50) */
    struct kt_rect box = {536, 21, 76, 28};
    int hit[4];
    kt_hit_rect(&box, 10, 561, 0, 50, hit);
    CHECK("the tap target starts at the key's box", hit[0] == 546 && hit[2] == 76);
    CHECK("the tap target spans the bar's height", hit[1] == 561 && hit[3] == 50);
    CHECK("a point on the key is on the target", kt_in(hit, 600, 590));
    CHECK("the key's right edge is outside (half-open)", !kt_in(hit, 622, 590));
    CHECK("the title's text is not on the target", !kt_in(hit, 300, 590));
    CHECK("below the bar is not on the target", !kt_in(hit, 600, 611));
    int none[4] = {0, 0, 0, 0};
    CHECK("an empty target holds nothing", !kt_in(none, 0, 0));

    /* whose tap */
    CHECK("key tap, Hide Track Title off: the key's", kt_tap_owner(1, 1, 1, 1, 0, 1) == KT_TAP_KEY);
    CHECK("key tap, Hide Track Title on: the key's, never the title's",
          kt_tap_owner(1, 1, 1, 1, 1, 1) == KT_TAP_KEY);
    CHECK("title tap, Hide Track Title on: the title's", kt_tap_owner(1, 1, 0, 1, 1, 1) == KT_TAP_TITLE);
    CHECK("title tap, Hide Track Title off: nobody's", kt_tap_owner(1, 1, 0, 1, 0, 1) == KT_TAP_NONE);
    CHECK("switch off: a tap on the key's place is not the key's",
          kt_tap_owner(0, 1, 1, 1, 0, 1) == KT_TAP_NONE);
    CHECK("switch off, Hide Track Title on: that tap is the title's as before",
          kt_tap_owner(0, 1, 1, 1, 1, 1) == KT_TAP_TITLE);
    CHECK("no key drawn: not the key's", kt_tap_owner(1, 0, 1, 1, 0, 1) == KT_TAP_NONE);
    CHECK("something over the key: nobody's", kt_tap_owner(1, 1, 1, 0, 1, 1) == KT_TAP_NONE);
    CHECK("off the bar: nobody's", kt_tap_owner(1, 1, 0, 1, 1, 0) == KT_TAP_NONE);

    /* the panel: 370 x 126 (2 DECK) / 117 (4 DECK) in the 1280 x 800 view */
    int view[4] = {0, 0, 1280, 800}, x = 0, y = 0;
    CHECK("2 DECK deck 1: a place", kt_panel_at(370, 126, 622, 561, 611, view, KT_PANEL_TOP, &x, &y));
    CHECK("2 DECK deck 1: right-aligned with the key", x == 622 - 370);
    CHECK("2 DECK deck 1: just above the title row", y == 561 - KT_PANEL_GAP - 126);
    CHECK("2 DECK deck 2: a place", kt_panel_at(370, 126, 1262, 561, 611, view, KT_PANEL_TOP, &x, &y));
    CHECK("2 DECK deck 2: inside the screen", x >= 0 && x + 370 <= 1280);
    CHECK("a key near the right edge: pulled in, never off-screen",
          kt_panel_at(370, 117, 1290, 561, 601, view, KT_PANEL_TOP, &x, &y) && x + 370 == 1280);
    CHECK("a key near the left edge: pulled in, never off-screen",
          kt_panel_at(370, 117, 100, 561, 601, view, KT_PANEL_TOP, &x, &y) && x == 0);
    CHECK("no room above (row under the header): below the row",
          kt_panel_at(370, 117, 600, 120, 160, view, KT_PANEL_TOP, &x, &y) &&
              y == 160 + KT_PANEL_GAP);
    CHECK("never over the header", kt_panel_at(370, 117, 600, 150, 790, view, KT_PANEL_TOP, &x, &y) &&
                                       y >= KT_PANEL_TOP && y + 117 <= 800);
    CHECK("a panel wider than the view: no place",
          !kt_panel_at(1300, 117, 600, 561, 601, view, KT_PANEL_TOP, &x, &y));
    CHECK("an empty panel: no place", !kt_panel_at(0, 117, 600, 561, 601, view, KT_PANEL_TOP, &x, &y));
    return ut_summary();
}
