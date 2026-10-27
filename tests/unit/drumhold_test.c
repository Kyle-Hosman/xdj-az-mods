/* SPDX-License-Identifier: GPL-3.0-only */
/* Drum roll HOLD on pad 7 (guest/azmod/bus_interpose.c drum_hold_pads, drum_hold_touch,
 * drum_hold_led and mixer_xpad_drum, extracted by run.sh through HOLD_INC, so the suite runs the
 * production text):
 *
 *   pads     on an armed deck pad 7 toggles HOLD; pads 5 and 6 do nothing; an unarmed deck
 *            never holds
 *   latch    the roll rate follows the X-PAD only while it is touched: the untouched position
 *            byte (0xFF, the fastest zone) never sets it, so the last touched zone stays
 *   sustain  HOLD alone never plays; a roll the X-PAD played under HOLD keeps sounding after the
 *            finger lifts, at the latched rate; touching again changes the rate; HOLD off stops it
 *   deck     HOLD is per deck: another armed deck is unaffected; a deck select or a disarm clears
 *            the hold of the deck it belonged to
 *   mode     another pad mode keeps HOLD and the held roll plays on; back on the drum roll pads
 *            HOLD still shows and pad 7 stops it; pad 7 of another pad mode is not HOLD
 *            (erp_drum_hold)
 *   no frame HOLD off ends a held roll without another mixer frame (the buses are event-driven:
 *            no finger, no frame); a finger still on the X-PAD keeps it until it lifts
 *   led      pad 7 dim blue with HOLD off, flashing blue (full and dark) with HOLD on
 *   threads  the ErP reader toggles HOLD while the mixer reader plays and lifts the strip (TSan);
 *            a hold the ErP reader cleared is never revived by the mixer */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "bus_cfg.h"

#define AZMOD_PAD_DECKS 2
#define AZMOD_STEM_DECKS 4
#define DRUM_HOLD_ON 1
#define DRUM_HOLD_SUSTAIN 2
static int g_drum_hold[AZMOD_STEM_DECKS];
static int g_drum_held[AZMOD_STEM_DECKS];
static int g_drum_arm = 1; /* _Atomic in azmod; plain here, as clang's __atomic builtins need */
static int g_xpad_raw = -1;
static int g_fxrow_zone = -1;
static int g_xpad_finger;
static _Atomic int g_rate[AZMOD_STEM_DECKS], g_touch[AZMOD_STEM_DECKS], g_rate_calls;
static void f_rate(int ld, int idx) {
    atomic_store(&g_rate[ld], idx);
    atomic_fetch_add(&g_rate_calls, 1);
}
static void f_touch(int ld, int on) { atomic_store(&g_touch[ld], on); }
static void (*g_drum_rate_fn)(int, int) = f_rate;
static void (*g_drum_touch_fn)(int, int) = f_touch;
static FILE *azlog_open(const char *path, const char *mode) {
    (void) path, (void) mode;
    return NULL;
}
static inline void bus_capture_xpad_fx(uint8_t *b, ssize_t r, const struct bus_xpad *xp,
                                       int touched, int have_zone) {
    (void) b, (void) r, (void) xp, (void) touched, (void) have_zone;
}
#include HOLD_INC

static int fails;
#define CHECK(c, ...)                                  \
    do {                                               \
        if (!(c)) {                                    \
            printf("FAIL %s:%d ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                       \
            printf("\n");                              \
            fails++;                                   \
        }                                              \
    } while (0)

/* One mixer frame: a finger at pos (zone byte set), or lifted (position 0xFF, zone 0, as the
 * panel sends with no finger). */
static void strip(int pos) {
    uint8_t b[128];
    memset(b, 0, sizeof b);
    b[XPAD_DEFAULT_POS] = pos < 0 ? 0xFF : (uint8_t) pos;
    b[XPAD_DEFAULT_ZONE] = pos < 0 ? 0x00 : 0x10;
    mixer_xpad_drum(b, (ssize_t) sizeof b, BC_DEFAULT);
}
#define LIFT (-1)
static int hold(int ld) { return __atomic_load_n(&g_drum_hold[ld], __ATOMIC_RELAXED); }
static int touch(int ld) { return atomic_load(&g_touch[ld]); }
static int rate(int ld) { return atomic_load(&g_rate[ld]); }

static void reset(void) {
    for (int d = 0; d < AZMOD_PAD_DECKS; d++)
        drum_hold_pads(d, d, 0, 0); /* forget which deck each side drove */
    for (int ld = 0; ld < AZMOD_STEM_DECKS; ld++) {
        __atomic_store_n(&g_drum_hold[ld], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_drum_held[ld], 0, __ATOMIC_RELAXED);
        atomic_store(&g_rate[ld], -1);
        atomic_store(&g_touch[ld], -1);
    }
}
static void arm(int ld) { __atomic_store_n(&g_drum_held[ld], 1, __ATOMIC_RELAXED); }

static _Atomic int g_stop;
static void *mixer(void *arg) {
    (void) arg;
    unsigned n = 0;
    while (!atomic_load(&g_stop))
        strip((++n & 1) ? 50 : LIFT);
    return NULL;
}

int main(void) {
    unsigned c;

    reset(); /* pads: 5 and 6 inert, 7 toggles, only while armed */
    arm(0);
    c = drum_hold_pads(0, 0, 1, 0x10);
    CHECK(c == 0 && hold(0) == 0, "pad 5 acted: changed %x hold %d", c, hold(0));
    c = drum_hold_pads(0, 0, 1, 0x20);
    CHECK(c == 0 && hold(0) == 0, "pad 6 acted: changed %x hold %d", c, hold(0));
    c = drum_hold_pads(0, 0, 1, 0x8F);
    CHECK(c == 0 && hold(0) == 0, "pads 1-4 + 8 acted: changed %x hold %d", c, hold(0));
    c = drum_hold_pads(0, 0, 1, 0x40);
    CHECK(c == 1u && hold(0) == DRUM_HOLD_ON, "pad 7 did not turn HOLD on: %x %d", c, hold(0));
    c = drum_hold_pads(0, 0, 1, 0x40);
    CHECK(c == 1u && hold(0) == 0, "pad 7 again did not turn HOLD off: %x %d", c, hold(0));
    reset();
    c = drum_hold_pads(1, 1, 0, 0x40);
    CHECK(c == 0 && hold(1) == 0, "unarmed pad 7 held: %x %d", c, hold(1));

    reset(); /* latch: no hold, the strip alone; lift never moves the rate */
    arm(0);
    strip(6); /* zone 0 */
    CHECK(rate(0) == 0 && touch(0) == 1, "touch zone 0: rate %d touch %d", rate(0), touch(0));
    strip(LIFT);
    CHECK(touch(0) == 0, "lift without HOLD kept the roll (touch %d)", touch(0));
    CHECK(rate(0) == 0, "lift moved the rate to %d (the untouched byte read as a zone)", rate(0));
    strip(74); /* zone 3 */
    CHECK(rate(0) == 3, "touch zone 3: rate %d", rate(0));

    reset(); /* sustain: HOLD alone never plays */
    arm(0);
    drum_hold_pads(0, 0, 1, 0x40);
    for (int i = 0; i < 3; i++)
        strip(LIFT);
    CHECK(touch(0) == 0, "HOLD played without a strip touch (touch %d)", touch(0));
    strip(28); /* zone 1 under HOLD */
    CHECK(touch(0) == 1 && rate(0) == 1, "touch under HOLD: touch %d rate %d", touch(0), rate(0));
    for (int i = 0; i < 3; i++)
        strip(LIFT);
    CHECK(touch(0) == 1, "HOLD did not keep the roll after the lift (touch %d)", touch(0));
    CHECK(rate(0) == 1, "HOLD rate after the lift %d, want the latched 1", rate(0));
    strip(98); /* zone 4 */
    strip(LIFT);
    CHECK(touch(0) == 1 && rate(0) == 4, "retouch under HOLD: touch %d rate %d (want 1, 4)", touch(0),
          rate(0));
    drum_hold_pads(0, 0, 1, 0x40); /* HOLD off */
    strip(LIFT);
    CHECK(touch(0) == 0 && hold(0) == 0, "HOLD off did not stop the roll: touch %d hold %d", touch(0),
          hold(0));
    strip(50);
    drum_hold_pads(0, 0, 1, 0x40); /* HOLD on with a finger already down */
    strip(50);
    strip(LIFT);
    CHECK(touch(0) == 1, "HOLD turned on under a finger did not keep the roll (touch %d)", touch(0));

    reset(); /* deck: HOLD is per deck */
    arm(0);
    arm(1);
    drum_hold_pads(0, 0, 1, 0x40);
    strip(50);
    strip(LIFT);
    CHECK(touch(0) == 1 && touch(1) == 0, "per deck: deck 1 touch %d, deck 2 touch %d (want 1, 0)",
          touch(0), touch(1));
    CHECK(hold(1) == 0, "deck 2 got deck 1's HOLD (%d)", hold(1));
    drum_hold_pads(1, 1, 1, 0x40);
    CHECK(hold(0) != 0 && hold(1) == DRUM_HOLD_ON, "side 2's pad 7 moved deck 1: %d %d", hold(0), hold(1));

    reset(); /* deck: a select clears the hold of the deck the side drove */
    arm(0);
    drum_hold_pads(0, 0, 1, 0x40);
    strip(50);
    c = drum_hold_pads(0, 2, 1, 0); /* side 1 now drives deck 3 */
    CHECK(hold(0) == 0 && (c & 1u), "deck select left deck 1's HOLD %d (changed %x)", hold(0), c);
    CHECK(hold(2) == 0, "deck select gave deck 3 a HOLD (%d)", hold(2));
    strip(LIFT);
    CHECK(touch(0) == 0, "deck 1 kept rolling after a deck select (touch %d)", touch(0));

    reset(); /* deck: a disarm clears it, and the re-arm does not bring it back */
    arm(3);
    drum_hold_pads(1, 3, 1, 0x40);
    __atomic_store_n(&g_drum_held[3], 0, __ATOMIC_RELAXED);
    c = drum_hold_pads(1, 3, 0, 0);
    CHECK(hold(3) == 0 && c == 8u, "disarm left HOLD %d (changed %x)", hold(3), c);
    arm(3);
    strip(LIFT);
    CHECK(touch(3) == 0, "re-arm rolled from an old HOLD (touch %d)", touch(3));

    reset(); /* mode: another pad mode keeps a held roll */
    arm(0);
    erp_drum_hold(0, 0, 1, 0x40);
    strip(50);
    strip(LIFT);
    CHECK(touch(0) == 1, "HOLD through erp_drum_hold did not sustain (touch %d)", touch(0));
    erp_drum_hold(0, 0, 0, 0); /* another pad mode on side 1 */
    CHECK(hold(0) != 0, "another pad mode cleared HOLD");
    strip(LIFT);
    CHECK(touch(0) == 1, "the held roll stopped in another pad mode (touch %d)", touch(0));
    erp_drum_hold(0, 0, 0, 0x40); /* pad 7 in another pad mode is not HOLD */
    CHECK(hold(0) != 0, "pad 7 in another pad mode toggled HOLD off");
    erp_drum_hold(0, 0, 1, 0); /* back to SLIP LOOP: HOLD still on */
    strip(LIFT);
    CHECK(hold(0) != 0 && touch(0) == 1, "back in SLIP LOOP HOLD was lost (%d, touch %d)", hold(0),
          touch(0));
    erp_drum_hold(0, 0, 1, 0x40); /* pad 7 there stops it */
    strip(LIFT);
    CHECK(hold(0) == 0 && touch(0) == 0, "pad 7 back in SLIP LOOP did not stop the roll (%d, touch %d)",
          hold(0), touch(0));

    reset(); /* no frame: HOLD off ends the roll with no mixer frame after it */
    arm(0);
    erp_drum_hold(0, 0, 1, 0x40);
    strip(50);
    strip(80); /* a new rate */
    strip(LIFT);
    CHECK(touch(0) == 1, "the held roll did not sustain (touch %d)", touch(0));
    erp_drum_hold(0, 0, 1, 0x40); /* HOLD off, and no mixer frame follows */
    CHECK(hold(0) == 0 && touch(0) == 0, "HOLD off left the roll playing until a touch (%d, touch %d)",
          hold(0), touch(0));
    erp_drum_hold(0, 0, 1, 0x40); /* HOLD on again */
    strip(50);
    erp_drum_hold(0, 0, 1, 0x40); /* off with the finger down: the finger keeps the roll */
    CHECK(touch(0) == 1, "HOLD off cut a roll under the finger (touch %d)", touch(0));
    strip(LIFT);
    CHECK(touch(0) == 0, "the lift after HOLD off kept the roll (touch %d)", touch(0));

    reset(); /* out of range: nothing */
    CHECK(drum_hold_pads(2, 0, 1, 0x40) == 0 && hold(0) == 0, "side 3 accepted");
    CHECK(drum_hold_pads(0, 4, 1, 0x40) == 0, "deck 5 accepted");
    CHECK(drum_hold_touch(4, 0) == 0 && drum_hold_touch(-1, 1) == 1, "deck out of range");

    { /* led: the pad bank, 4 bytes per pad; pads 5 and 6 dark, pad 7 blue */
        unsigned char pb[32], on[32], off[32];
        memset(pb, 0x55, sizeof pb);
        drum_hold_led(pb, 0, 1);
        const unsigned char *px = pb + 6 * 4;
        CHECK(px[0] == 0 && px[1] == 0 && px[2] == 0x22, "HOLD off: %02x %02x %02x", px[0], px[1], px[2]);
        for (int i = 4; i < 6; i++)
            CHECK(pb[i * 4] == 0 && pb[i * 4 + 1] == 0 && pb[i * 4 + 2] == 0, "pad %d lit: %02x %02x %02x",
                  i + 1, pb[i * 4], pb[i * 4 + 1], pb[i * 4 + 2]);
        CHECK(pb[0] == 0x55 && pb[3 * 4 + 2] == 0x55 && pb[7 * 4] == 0x55, "pads 1-4 or 8 written");
        drum_hold_led(pb, 0, 0);
        CHECK(px[2] == 0x22, "HOLD off blinked: %02x", px[2]);
        memset(on, 0x55, sizeof on);
        memset(off, 0x55, sizeof off);
        drum_hold_led(on, DRUM_HOLD_ON, 1);
        drum_hold_led(off, DRUM_HOLD_ON | DRUM_HOLD_SUSTAIN, 0);
        CHECK(on[24] == 0 && on[25] == 0 && on[26] == 0xFF, "HOLD on, lit phase: %02x %02x %02x", on[24],
              on[25], on[26]);
        CHECK(off[24] == 0 && off[25] == 0 && off[26] == 0x00, "HOLD on, dark phase: %02x %02x %02x",
              off[24], off[25], off[26]);
        CHECK(on[16] == 0 && on[20] == 0 && off[17] == 0 && off[21] == 0, "pads 5/6 lit while HOLD is on");
    }

    reset(); /* the mixer never sets a hold of its own */
    arm(1);
    strip(50);
    CHECK(hold(1) == 0, "a strip touch with no HOLD set one (%d)", hold(1));

    reset(); /* threads */
    arm(1);
    pthread_t th;
    pthread_create(&th, NULL, mixer, NULL);
    for (int i = 0; i < 20000; i++)
        drum_hold_pads(1, 1, 1, (i & 1) ? 0x40 : 0x00);
    atomic_store(&g_stop, 1);
    pthread_join(th, NULL);
    if (hold(1))
        drum_hold_pads(1, 1, 1, 0x40);
    strip(LIFT);
    CHECK(hold(1) == 0 && touch(1) == 0, "after the threads: hold %d touch %d", hold(1), touch(1));

    printf("%s drumhold\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
