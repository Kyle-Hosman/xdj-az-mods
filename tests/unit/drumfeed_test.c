/* SPDX-License-Identifier: GPL-3.0-only */
/* drum_feed_bpm (extracted from azmod/stems_display.c by run.sh): the BPM azmod sends
 * the drum roll for a deck. A deck with a track keeps its own; an empty deck (BpmState live =
 * DBL_MAX, EP147's "no track") rolls at the master deck's BPM; no known master, or a master
 * without a valid BPM, sends 0 (azstem's 120). A loaded track whose BPM is out of range is
 * not empty and must never take the master's. */
#include <float.h>
#define AZMOD_STEM_DECKS 4
#include FEED_INC
#include "check.h"

int main(void) {
    ut_suite = "drum feed";
    const double E = DBL_MAX;
    CHECK("own BPM kept, master unknown", drum_feed_bpm(0, 128.0, -1, 0, 0.0) == 128.0f);
    CHECK("own BPM kept over the master's", drum_feed_bpm(0, 128.0, 1, 1, 100.0) == 128.0f);
    CHECK("empty deck 1, master deck 2 at 126 -> 126", drum_feed_bpm(0, E, 1, 1, 126.0) == 126.0f);
    CHECK("empty deck 4, master deck 1 at 93.5 -> 93.5", drum_feed_bpm(3, E, 0, 1, 93.5) == 93.5f);
    CHECK("empty deck, master unknown (-1) -> 0 (120)", drum_feed_bpm(0, E, -1, 0, 0.0) == 0.0f);
    CHECK("empty deck, master unknown even with a valid-looking BPM -> 0",
          drum_feed_bpm(0, E, -1, 1, 126.0) == 0.0f);
    CHECK("empty deck, master's BpmState unreadable -> 0", drum_feed_bpm(0, E, 1, 0, 126.0) == 0.0f);
    CHECK("empty deck, master index out of range -> 0", drum_feed_bpm(0, E, 4, 1, 126.0) == 0.0f);
    CHECK("empty deck, master is this same empty deck -> 0", drum_feed_bpm(2, E, 2, 1, E) == 0.0f);
    CHECK("empty deck, master ejected too -> 0", drum_feed_bpm(0, E, 1, 1, E) == 0.0f);
    CHECK("empty deck, master BPM out of range -> 0", drum_feed_bpm(0, E, 1, 1, 450.0) == 0.0f);
    CHECK("loaded track, BPM 0 -> 0, not the master's", drum_feed_bpm(0, 0.0, 1, 1, 126.0) == 0.0f);
    CHECK("loaded track, BPM 450 -> 0, not the master's", drum_feed_bpm(0, 450.0, 1, 1, 126.0) == 0.0f);
    CHECK("BPM 20 is out of range -> 0", drum_feed_bpm(0, 20.0, -1, 0, 0.0) == 0.0f);
    /* drum_feed_lead: whose grid places the roll (azstem_drum_follow) */
    CHECK("lead: a loaded deck follows itself", drum_feed_lead(0, 128.0, 1, 1, 126.0) == -1);
    CHECK("lead: a loaded deck with an out-of-range BPM follows itself",
          drum_feed_lead(0, 450.0, 1, 1, 126.0) == -1);
    CHECK("lead: empty deck 1 follows master deck 2", drum_feed_lead(0, E, 1, 1, 126.0) == 1);
    CHECK("lead: empty deck 4 follows master deck 1", drum_feed_lead(3, E, 0, 1, 93.5) == 0);
    CHECK("lead: no known master -> itself", drum_feed_lead(0, E, -1, 0, 0.0) == -1);
    CHECK("lead: master unreadable -> itself", drum_feed_lead(0, E, 1, 0, 126.0) == -1);
    CHECK("lead: master ejected too -> itself", drum_feed_lead(0, E, 1, 1, E) == -1);
    CHECK("lead: the master is this empty deck -> itself", drum_feed_lead(2, E, 2, 1, E) == -1);
    return ut_summary();
}
