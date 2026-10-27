/* SPDX-License-Identifier: GPL-3.0-only */
/* Stem FX pads (guest/azmod/stem_fx_pads.h): a quick press latches, a held press is momentary,
 * a press while on turns it off, a refused press leaves nothing behind, and a release ends the
 * effect on the deck it was pressed on. SFXP_INC = the header under test. */
#include "check.h"
#include <string.h>
#include SFXP_INC

static unsigned g_on[4];          /* the engine's answer: bit f = effect f on */
static int g_refuse;              /* the engine refuses presses */
static int g_calls, g_last_deck, g_last_fx, g_last_on;
static unsigned is_on(int deck) { return g_on[deck]; }
static int fx(int deck, int f, int on) {
    g_calls++;
    g_last_deck = deck;
    g_last_fx = f;
    g_last_on = on;
    if (on) {
        if (g_refuse)
            return 0;
        g_on[deck] |= 1u << f;
        return 1;
    }
    g_on[deck] &= ~(1u << f);
    return 0;
}
static sfxp_hold h[SFXP_N];
static void reset(void) {
    memset(g_on, 0, sizeof g_on);
    g_refuse = g_calls = 0;
    sfxp_init(h);
}
/* press pad f on deck ld at t0, release at t1 */
static void tap(int ld, int f, long long t0, long long t1) {
    sfxp_frame(h, ld, 1u << f, 1u << f, t0, is_on, fx);
    sfxp_frame(h, ld, 0, 1u << f, (t0 + t1) / 2, is_on, fx);
    sfxp_frame(h, ld, 0, 0, t1, is_on, fx);
}

int main(void) {
    ut_suite = "stem fx pads";
    reset();
    tap(0, 1, 1000, 1200);
    CHECK("quick press latches on", g_on[0] == 2u);
    tap(0, 1, 3000, 3100);
    CHECK("next press turns it off", g_on[0] == 0u);

    reset();
    tap(1, 0, 1000, 1000 + SFXP_HOLD_MS);
    CHECK("held press is momentary: off on release", g_on[1] == 0u && g_calls == 2);
    reset();
    tap(1, 0, 1000, 1000 + SFXP_HOLD_MS - 1);
    CHECK("just under the hold: latched", g_on[1] == 1u);

    reset();
    tap(0, 2, 0, 100); /* latched */
    g_calls = 0;
    tap(0, 2, 5000, 5000 + 3 * SFXP_HOLD_MS); /* a long press while on */
    CHECK("press while on turns it off; its release does nothing", g_on[0] == 0u && g_calls == 1);

    reset();
    g_refuse = 1;
    tap(0, 3, 0, 5000);
    CHECK("refused press: nothing on, release sends nothing", g_on[0] == 0u && g_calls == 1);

    reset();
    sfxp_frame(h, 0, 1u << 0, 1u, 0, is_on, fx); /* hold on deck 1 ... */
    sfxp_frame(h, 2, 0, 1u, 800, is_on, fx);     /* ... deck select moves to deck 3 */
    sfxp_frame(h, 2, 0, 0, 4000, is_on, fx);     /* release after the hold */
    CHECK("release ends the effect on the deck it was pressed on", g_on[0] == 0u && g_last_deck == 0);

    reset();
    sfxp_frame(h, 0, 0x5u, 0x5u, 0, is_on, fx); /* two pads at once */
    sfxp_frame(h, 0, 0, 0x1u, 200, is_on, fx);  /* pad 5 still held, pad 7 released quickly */
    sfxp_frame(h, 0, 0, 0, 2000, is_on, fx);    /* pad 5 released after the hold */
    CHECK("pads are independent", g_on[0] == 0x4u);

    reset();
    sfxp_frame(h, -1, 1u, 1u, 0, is_on, fx);
    CHECK("unknown deck: nothing", g_calls == 0);
    return ut_summary();
}
