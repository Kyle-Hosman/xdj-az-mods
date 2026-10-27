/* SPDX-License-Identifier: GPL-3.0-only */
/* Per-deck track generation (guest/azstem/stem_playback.c stems_tick + deck_track_changed),
 * extracted at run time (TICK_INC) and driven tick by tick with the rest of azstem stubbed:
 *   - a confirmed new track bumps the deck's generation once and refreshes its identity;
 *   - an EJECT (the name empty for EJECT_EMPTY_TICKS ticks) bumps it too and refreshes the
 *     identity (empty), so the drum roll's grid, the auto key-shift mapping and azmod's
 *     toggle reset all see the deck change;
 *   - a transient probe miss (empty for fewer ticks) changes nothing;
 *   - the other decks' generations never move;
 *   - azmod sees the new name and the new generation together (the bump comes before the
 *     curtrack publish), so it resets the deck's toggles once, not twice;
 *   - with no stems base (no stick holds a STEMS folder) the generation still moves on load
 *     and eject, nothing is loaded, and the deck's track loads once a base turns up. */
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "check.h"

#define NDECK 4
#define MAX_DECKS 8
typedef struct {
    char loaded_track[256];
    char track_path[512];
    long stem_frames;
} deck_stems;
static deck_stems g_dk[NDECK];
static _Atomic(void *) g_deck_self[MAX_DECKS];
static const int STEM_SLOT[NDECK] = {0, 1, 2, 3};
static _Atomic unsigned g_deck_trackgen[NDECK];
static _Atomic int g_stem_pending[NDECK], g_stem_loading[NDECK], g_load_again[NDECK];
static char g_stems_base[512] = "/media/usb/STEMS";
static int g_eject_empty[NDECK], g_load_deferred[NDECK];
static char g_pending_name[NDECK][512], g_loaded_src[NDECK][600], g_pending_src[NDECK][600];
#define EJECT_EMPTY_TICKS 8

static const char *g_name[NDECK]; /* what EP147 shows on each deck this tick */
static int g_publish_seen_gen[NDECK];
static unsigned g_gen_at_publish[NDECK];
/* azstem.c's development-input test, as the release build defines it (azstem.vdbg unread) */
#define dev_flag(path) 0
static void logln(const char *fmt, ...) { (void) fmt; }
static void find_stems_base(char *b, size_t n) {
    (void) b;
    (void) n;
}
static void load_stems_f32(int d) { (void) d; }
static const char *get_loaded_track_name(int d) { return g_name[d] ? g_name[d] : ""; }
static void loaded_track_path(int d, char *out, size_t n) {
    snprintf(out, n, "/media/usb/%s.mp3", g_name[d] ? g_name[d] : "");
    if (!g_name[d] || !g_name[d][0]) out[0] = 0;
}
static void drop_stems(int d) { g_dk[d].stem_frames = 0; }
static int g_starts[NDECK]; /* start_load calls per deck */
static void start_load(int d, const char *tr) {
    (void) tr;
    g_starts[d]++;
}
static void publish_src(int d) { (void) d; }
static void publish_deck_file(int d, const char *what, const char *v) {
    (void) v;
    if (!strcmp(what, "curtrack")) {
        g_gen_at_publish[d] = atomic_load(&g_deck_trackgen[d]);
        g_publish_seen_gen[d] = 1;
    }
}
#include TICK_INC

static unsigned gen(int d) { return atomic_load(&g_deck_trackgen[d]); }
static void ticks(int n) {
    while (n-- > 0) stems_tick();
}

int main(void) {
    ut_suite = "trackgen";
    g_name[0] = "Track A";
    ticks(3); /* pending, then confirmed */
    unsigned g1 = gen(0);
    CHECK("load: generation bumped once", g1 == 1);
    CHECK("load: azmod sees the new generation with the new name",
          g_publish_seen_gen[0] && g_gen_at_publish[0] == g1);

    g_name[0] = "";
    ticks(EJECT_EMPTY_TICKS - 2);
    CHECK("a short probe miss: no change", gen(0) == g1 && (int) gen(0) == 1 && g_dk[0].loaded_track[0]);
    g_name[0] = "Track A";
    ticks(2);
    CHECK("the miss recovered: still the same track", gen(0) == g1 && (int) gen(0) == 1);

    g_name[0] = "";
    ticks(EJECT_EMPTY_TICKS + 1);
    CHECK("eject: generation bumped", gen(0) == g1 + 1);
    CHECK("eject: generation bumped (to empty)", (int) gen(0) == 2);
    CHECK("eject: the deck's track state is cleared", !g_dk[0].loaded_track[0] && !g_loaded_src[0][0]);
    ticks(20);
    CHECK("an empty deck stays put", gen(0) == g1 + 1 && (int) gen(0) == 2);

    g_name[0] = "Track A"; /* the same track again: a new load all the same */
    ticks(3);
    CHECK("reloading the same track after an eject bumps again", gen(0) == g1 + 2 && (int) gen(0) == 3);
    CHECK("other decks never moved", gen(1) == 0 && gen(2) == 0 && gen(3) == 0 &&
                                         !(int) gen(1) && !(int) gen(2) && !(int) gen(3));
    CHECK("with a base, a new track starts one load", g_starts[0] == 2);

    /* No stems base: identity still runs on deck 1 (index 1), nothing loads. */
    g_stems_base[0] = 0;
    g_name[1] = "Track B";
    ticks(3);
    CHECK("no stems base: a load bumps the generation", gen(1) == 1);
    CHECK("no stems base: azmod sees the new name and generation together",
          g_publish_seen_gen[1] && g_gen_at_publish[1] == 1);
    CHECK("no stems base: nothing is loaded", g_starts[1] == 0);
    g_name[1] = "Track C";
    ticks(3);
    CHECK("no stems base: a second track bumps again", gen(1) == 2 && g_starts[1] == 0);
    snprintf(g_stems_base, sizeof g_stems_base, "/media/usb/STEMS");
    ticks(1);
    CHECK("a base turns up: the loaded track's stems load", g_starts[1] == 1);
    ticks(10);
    CHECK("a base turns up: one load only, no new generation", g_starts[1] == 1 && gen(1) == 2);
    g_stems_base[0] = 0;
    g_name[1] = "";
    ticks(EJECT_EMPTY_TICKS + 1);
    CHECK("no stems base: an eject bumps the generation", gen(1) == 3);
    g_name[1] = "Track C";
    ticks(3);
    snprintf(g_stems_base, sizeof g_stems_base, "/media/usb/STEMS");
    ticks(2);
    CHECK("eject and reload without a base, then a base: one load for it", g_starts[1] == 2 && gen(1) == 4);
    return ut_summary();
}
