/* SPDX-License-Identifier: GPL-3.0-only */
/* Remember Waveform Zoom (guest/azmod/wave_zoom.h): the saved text, the levels EP147's step
 * function produces, when a saved zoom may be written over the live one at start-up, and when
 * a new live zoom is saved. */
#include <string.h>
#include WZ_HDR
#include "check.h"

/* the six levels, as EP147's step function leaves them, zoomed in to zoomed out */
static const uint32_t LEVELS[6] = {WZ_PACK(2, 1), WZ_PACK(1, 1), WZ_PACK(1, 2),
                                   WZ_PACK(1, 4), WZ_PACK(1, 8), WZ_PACK(1, 16)};
static const char *const TEXT[6] = {"2/1", "1/1", "1/2", "1/4", "1/8", "1/16"};

int main(void) {
    ut_suite = "remember wave zoom";
    /* ---- levels */
    int all = 1;
    for (int k = 0; k < 6; k++)
        all &= wz_level_ok(LEVELS[k]);
    CHECK("the six levels are levels", all);
    int others = 0;
    for (unsigned n = 0; n <= 40; n++)
        for (unsigned d = 0; d <= 40; d++) {
            int known = 0;
            for (int k = 0; k < 6; k++)
                known |= WZ_PACK(n, d) == LEVELS[k];
            others += !known && wz_level_ok(WZ_PACK(n, d));
        }
    CHECK("no other num/den up to 40/40 is a level (4/1, 2/2, 1/32, 0/4 ...)", others == 0);
    CHECK("the stock value is 1/4, bytes 01 00 04 00 as one little-endian word",
          WZ_STOCK == 0x00040001u && wz_level_ok(WZ_STOCK));
    CHECK("none is not a level", !wz_level_ok(WZ_NONE));
    CHECK("a den with high bits set is not a level", !wz_level_ok(WZ_PACK(1, 16) | 0x80000000u));

    /* ---- text */
    all = 1;
    for (int k = 0; k < 6; k++) {
        char w[WZ_TEXT];
        wz_format(w, sizeof w, LEVELS[k]);
        all &= !strcmp(w, TEXT[k]) && wz_parse(TEXT[k]) == LEVELS[k];
    }
    CHECK("each level formats as its text and parses back", all);
    {
        char w[WZ_TEXT] = "x";
        wz_format(w, sizeof w, WZ_PACK(1, 32));
        CHECK("a value that is not a level formats as empty", w[0] == 0);
        char tiny[4] = "xyz";
        wz_format(tiny, sizeof tiny, WZ_PACK(1, 16));
        CHECK("a short buffer is cut and terminated", !strcmp(tiny, "1/1"));
    }
    static const char *const BAD[] = {
        NULL,
        "",
        "1",
        "1/",
        "/4",
        "1/32",
        "4/1",
        "2/2",
        "0/4",
        "01/4",
        "1/04",
        "1/4x",
        "1/4 ",
        " 1/4",
        "+1/4",
        "-1/4",
        "1//4",
        "1/4/1",
        "001/16",
        "1/016",
        "1/1600",
        "16",
        "0x1/4",
        "1.0/4",
        "1/4\n",
    };
    int refused = 1;
    for (size_t k = 0; k < sizeof BAD / sizeof BAD[0]; k++)
        refused &= wz_parse(BAD[k]) == WZ_NONE;
    CHECK("malformed text and values that are not levels parse as none", refused);
    CHECK("a leading zero is refused", wz_parse("01/4") == WZ_NONE && wz_parse("1/04") == WZ_NONE);
    CHECK("text after the level is refused", wz_parse("1/4x") == WZ_NONE && wz_parse("1/16\n") == WZ_NONE);
    CHECK("a ratio that is not a level is refused", wz_parse("1/32") == WZ_NONE && wz_parse("4/1") == WZ_NONE);

    /* ---- restore at start-up */
    all = 1;
    for (int k = 0; k < 6; k++)
        all &= wz_restore_ok(WZ_STOCK, LEVELS[k]);
    CHECK("over the stock 1/4, any saved level is restored", all);
    CHECK("nothing saved: nothing restored", !wz_restore_ok(WZ_STOCK, WZ_NONE));
    CHECK("a saved value that is not a level: nothing restored", !wz_restore_ok(WZ_STOCK, WZ_PACK(1, 32)));
    int over = 0;
    for (int k = 0; k < 6; k++)
        over += LEVELS[k] != WZ_STOCK && wz_restore_ok(LEVELS[k], WZ_PACK(1, 16));
    CHECK("a live zoom that is not the stock 1/4 is never written over", over == 0);
    CHECK("a live value that is not a level (a wrong address) is never written over",
          !wz_restore_ok(0, WZ_PACK(1, 16)) && !wz_restore_ok(0xdeadbeefu, WZ_PACK(2, 1)));

    /* ---- saving a new zoom */
    {
        struct wz_watch w;
        wz_watch_init(&w);
        uint32_t saved = WZ_PACK(1, 4);
        CHECK("the first read only starts the clock", !wz_watch_step(&w, WZ_PACK(1, 16), saved, 1000));
        CHECK("not saved before it settles", !wz_watch_step(&w, WZ_PACK(1, 16), saved, 1000 + WZ_SETTLE_MS - 1));
        CHECK("saved once it has settled", wz_watch_step(&w, WZ_PACK(1, 16), saved, 1000 + WZ_SETTLE_MS));
        saved = WZ_PACK(1, 16);
        CHECK("the saved level is not saved again", !wz_watch_step(&w, WZ_PACK(1, 16), saved, 9000));
        /* a sweep through the levels: every step restarts the clock */
        long t = 10000;
        int early = 0;
        for (int k = 0; k < 6; k++, t += WZ_SETTLE_MS / 4)
            early += wz_watch_step(&w, LEVELS[k], saved, t);
        CHECK("a sweep that ends on the saved level saves nothing",
              early == 0 && !wz_watch_step(&w, LEVELS[5], saved, t + 5000));
        early = 0;
        for (int k = 5; k >= 0; k--, t += WZ_SETTLE_MS / 4)
            early += wz_watch_step(&w, LEVELS[k], saved, t);
        CHECK("a sweep to 2/1: nothing saved while it moves", early == 0);
        CHECK("then 2/1 once it has settled", wz_watch_step(&w, WZ_PACK(2, 1), saved, t + WZ_SETTLE_MS));
        /* a half-written pair (the num of one level, the den of another) is not a level */
        wz_watch_init(&w);
        CHECK("a torn pair 2/16 is never saved",
              !wz_watch_step(&w, WZ_PACK(2, 16), saved, 0) && !wz_watch_step(&w, WZ_PACK(2, 16), saved, 60000));
        wz_watch_init(&w);
        CHECK("a value that is not a level is never saved (a wrong address)",
              !wz_watch_step(&w, 0x12345678u, saved, 0) && !wz_watch_step(&w, 0x12345678u, saved, 60000));
        /* a value seen for one read in between restarts the clock */
        wz_watch_init(&w);
        wz_watch_step(&w, WZ_PACK(1, 8), saved, 0);
        wz_watch_step(&w, WZ_PACK(1, 1), saved, 900);
        CHECK("a value seen in between restarts the settle time",
              !wz_watch_step(&w, WZ_PACK(1, 8), saved, 1000) && !wz_watch_step(&w, WZ_PACK(1, 8), saved, 1999) &&
                  wz_watch_step(&w, WZ_PACK(1, 8), saved, 2000));
        wz_watch_init(&w);
        CHECK("nothing saved yet: a settled stock 1/4 is saved",
              !wz_watch_step(&w, WZ_STOCK, WZ_NONE, 0) && wz_watch_step(&w, WZ_STOCK, WZ_NONE, WZ_SETTLE_MS));
    }
    return ut_summary();
}
