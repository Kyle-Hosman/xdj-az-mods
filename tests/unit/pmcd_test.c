/* SPDX-License-Identifier: GPL-3.0-only */
/* The Phase Meter's memory cue countdown text (guest/azmod/extras.c pm_cd_fmt, extracted): the
 * beats to the next memory cue as the deck shows them, bars and beat ("BB.b"), and "--.-" for
 * none or anything past what a player counts. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "check.h"
#include PMCD_SRC

static int is(uint32_t n, const char *want) {
    char t[8];
    memset(t, 'x', sizeof t);
    pm_cd_fmt(n, t);
    return memchr(t, 0, sizeof t) && !strcmp(t, want);
}

int main(void) {
    ut_suite = "phase meter countdown";
    CHECK("at the cue: 00.0", is(0, "00.0"));
    CHECK("one beat before: 00.1", is(1, "00.1"));
    CHECK("four beats before: 00.4", is(4, "00.4"));
    CHECK("five beats before: 01.1", is(5, "01.1"));
    CHECK("15 beats: 03.3", is(15, "03.3"));
    CHECK("16 beats: 03.4", is(16, "03.4"));
    CHECK("24 beats: 05.4 (EP147's own reading)", is(24, "05.4"));
    CHECK("the most a player counts (256): 63.4", is(256, "63.4"));
    CHECK("past that: none", is(257, "--.-"));
    CHECK("the status's no cue (511): none", is(511, "--.-"));
    CHECK("EP147's none (0x80000000): none", is(0x80000000u, "--.-"));
    CHECK("the largest value: none, in bounds", is(0xffffffffu, "--.-"));
    return ut_summary();
}
