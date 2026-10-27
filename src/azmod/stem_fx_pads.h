/* SPDX-License-Identifier: GPL-3.0-only */
/* stem_fx_pads.h: pads 5-8 as Stem FX pads (VOCAL ECHO, INST ECHO, INST BRAKE, DRUMS ECHO).
 *
 *   a quick press (shorter than SFXP_HOLD_MS) latches the effect on; the next press turns it off;
 *   a press held SFXP_HOLD_MS or longer is momentary: the effect starts on the press and stops on
 *   the release.
 *
 * Whether an effect is on is azstem's answer (is_on), never a copy kept here, so a track change
 * or a refused press can never leave a pad showing an effect that is not playing. A press is
 * remembered per physical pad set with the logical deck it was made on, so the release ends the
 * effect it started even if the deck select, the pad mode or the Mod Menu changed while it was
 * held: call sfxp_frame on every frame, with rise = 0 outside the FX pads.
 *
 * Header-only, no azmod dependencies, so the unit tests compile it as it ships. */
#ifndef AZMOD_STEM_FX_PADS_H
#define AZMOD_STEM_FX_PADS_H

#define SFXP_HOLD_MS 400
#define SFXP_N 4

typedef struct {
    int ld;       /* logical deck of the press that started an effect, -1 = none */
    long long t0; /* when, ms */
} sfxp_hold;

typedef unsigned (*sfxp_on_fn)(int deck);              /* bit f = effect f is on */
typedef int (*sfxp_fx_fn)(int deck, int fx, int on);   /* press 1 = engaged */

static inline void sfxp_init(sfxp_hold h[SFXP_N]) {
    for (int f = 0; f < SFXP_N; f++) {
        h[f].ld = -1;
        h[f].t0 = 0;
    }
}

/* One frame of one pad set. rise / down: bit f = pad 5+f pressed this frame / held now. ld = the
 * logical deck the pads drive now (only used for a rise). */
static inline void sfxp_frame(sfxp_hold h[SFXP_N], int ld, unsigned rise, unsigned down,
                              long long now, sfxp_on_fn is_on, sfxp_fx_fn fx) {
    for (int f = 0; f < SFXP_N; f++) {
        unsigned b = 1u << f;
        if (h[f].ld >= 0 && !(down & b)) { /* release of a press that started an effect */
            if (now - h[f].t0 >= SFXP_HOLD_MS)
                fx(h[f].ld, f, 0); /* held: momentary */
            h[f].ld = -1;          /* quick: stays latched */
        }
        if (!(rise & b) || ld < 0)
            continue;
        if (h[f].ld >= 0) /* a second rise without a release seen: end that press first */
            h[f].ld = -1;
        if (is_on(ld) & b) {
            fx(ld, f, 0); /* a press while on turns it off; its release does nothing */
            continue;
        }
        if (fx(ld, f, 1)) {
            h[f].ld = ld;
            h[f].t0 = now;
        }
    }
}
#endif
