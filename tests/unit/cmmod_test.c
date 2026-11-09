/* SPDX-License-Identifier: GPL-3.0-only */
/* An installed community mod's page (menu.c, CMV_MOD) for each load state:
 *   - a mod that loaded offers Disable; one disabled offers Enable;
 *   - a mod REFUSED at load (here: it needs a newer AZ-MODS) or whose init() FAILED is still
 *     enabled (the core reads the Disable mark first), so its page shows the reason and
 *     offers Disable, never an Enable that would only send "enable" for a mod already enabled;
 *   - once Disable is pressed on a refused mod its page says "Disabled after Reload.";
 *   - a reload is asked for only when an Enable / Disable differs from what is set.
 * menu.c is #included (MENU_C) so its statics are visible; azcm_count / azcm_info are this
 * file's, so each case sets the one installed mod's state. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "check.h"
#include MENU_C

static char g_seen[1 << 14];
static size_t g_seen_n;
int azmod_ttf_menu(const azmod_fb *fb, int x, int y, int w, int h, const char *s, int px, int track, int bold, uint32_t c) {
    (void) fb;
    (void) x;
    (void) y;
    (void) w;
    (void) h;
    (void) px;
    (void) track;
    (void) bold;
    (void) c;
    size_t L = strlen(s);
    if (g_seen_n + L + 2 < sizeof g_seen) {
        memcpy(g_seen + g_seen_n, s, L);
        g_seen_n += L;
        g_seen[g_seen_n++] = '\n';
        g_seen[g_seen_n] = 0;
    }
    return 1;
}
int azmod_ttf_menu2(const azmod_fb *fb, int x, int y, int w, int h, const char *s, int px, int track, int bold,
                    uint32_t c, int split, uint32_t c2) {
    (void) split;
    (void) c2;
    return azmod_ttf_menu(fb, x, y, w, h, s, px, track, bold, c);
}
static int seen(const char *s) { return strstr(g_seen, s) != NULL; }
/* a whole drawn text (a button's label), not a part of a longer one ("Disable" in "Disabled") */
static int seen_line(const char *s) {
    char k[128];
    size_t n = strlen(s);
    snprintf(k, sizeof k, "\n%s\n", s);
    return (!strncmp(g_seen, s, n) && g_seen[n] == '\n') || strstr(g_seen, k) != NULL;
}

static azcm_info_t g_mod;
int azcm_count(void) { return 1; }
int azcm_info(int i, azcm_info_t *out) {
    if (i != 0) return -1;
    *out = g_mod;
    return 0;
}

#define W 1280
#define H 800
static uint8_t g_px[W * H * 4];
static azmod_fb g_fb = {g_px, W, H, W * 4};
static azmod_state g_s;
static cm_ctl g_ctl[CM_MAXCTL];
static int g_nctl;

static void page(int state, const char *reason) {
    memset(&g_mod, 0, sizeof g_mod);
    strcpy(g_mod.id, "testmod");
    strcpy(g_mod.name, "Test Mod");
    strcpy(g_mod.version, "1.0.0");
    strcpy(g_mod.author, "t");
    strcpy(g_mod.summary, "s");
    snprintf(g_mod.reason, sizeof g_mod.reason, "%s", reason);
    g_mod.state = state;
    memset(g_px, 0, sizeof g_px);
    g_seen_n = 0;
    g_seen[0] = 0;
    g_cm_full = 1;
    g_cm_view = CMV_MOD;
    g_cm_page = 0;
    g_cm_scroll = 0;
    g_nctl = cm_layout(&g_s, &g_fb, g_ctl, 1);
}
static int toggle(void) {   /* the Enable / Disable control's action, 0 = none */
    for (int i = 0; i < g_nctl; i++)
        if (g_ctl[i].kind == CMC_MODACT && (g_ctl[i].idx == CMA_ENABLE || g_ctl[i].idx == CMA_DISABLE)) return g_ctl[i].idx;
    return 0;
}

int main(void) {
    ut_suite = "installed mod page";
    page(AZCM_LOADED, "");
    CHECK("CONTROL: a mod that loaded offers Disable", seen_line("Disable") && !seen_line("Enable") && toggle() == CMA_DISABLE);
    page(AZCM_DISABLED, "Disabled");
    CHECK("CONTROL: a disabled mod offers Enable", seen_line("Enable") && !seen_line("Disable") && toggle() == CMA_ENABLE);
    page(AZCM_REFUSED, "Needs AZ-MODS 9.9.9 or newer");
    CHECK("a refused mod's page shows why it did not load", seen("Not loaded: Needs AZ-MODS 9.9.9 or newer"));
    CHECK("a refused mod is enabled: it offers Disable, not Enable", seen_line("Disable") && !seen_line("Enable") && toggle() == CMA_DISABLE);
    CHECK("a refused mod with nothing pressed needs no reload", !cm_need_reload());
    page(AZCM_FAILED, "Its init() returned -1");
    CHECK("a mod whose init() failed offers Disable, not Enable", seen_line("Disable") && !seen_line("Enable") && toggle() == CMA_DISABLE);
    cm_pend_set("testmod", 0);
    page(AZCM_REFUSED, "Needs AZ-MODS 9.9.9 or newer");
    CHECK("Disable pressed on a refused mod: \"Disabled after Reload.\" and Enable offered", seen("Disabled after Reload.") && toggle() == CMA_ENABLE);
    CHECK("Disable pressed on a refused mod asks for a reload", cm_need_reload());
    cm_pend_set("testmod", 1);
    page(AZCM_REFUSED, "Needs AZ-MODS 9.9.9 or newer");
    CHECK("turned back on before a reload: nothing left to reload, reason shown again", !cm_need_reload() && seen("Not loaded: Needs AZ-MODS"));
    page(AZCM_DISABLED, "Disabled");
    CHECK("CONTROL: Enable pressed on a disabled mod asks for a reload", cm_need_reload() && seen("Enabled after Reload."));

    ut_suite = "installed checkbox tap";
    char buf[128];
    uint32_t col;
    page(AZCM_LOADED, "");
    cm_pend_set("testmod", 0); /* the tap shows its new state at once (CMC_CHK) */
    snprintf(g_cm_flip.id, sizeof g_cm_flip.id, "testmod");
    g_cm_flip.at = cm_ms() ? cm_ms() : 1;
    CHECK("turned off, no answer yet: \"Turning off...\", not \"after Reload\"",
          !strcmp(cm_state_text(&g_mod, buf, sizeof buf, &col), "Turning off..."));
    cm_pend_set("testmod", 1);
    page(AZCM_DISABLED, "Disabled");
    CHECK("turned on, no answer yet: \"Turning on...\"", !strcmp(cm_state_text(&g_mod, buf, sizeof buf, &col), "Turning on..."));
    snprintf(g_cm_flip.id, sizeof g_cm_flip.id, "othermod");
    CHECK("another mod's tap leaves this one's text", !strcmp(cm_state_text(&g_mod, buf, sizeof buf, &col), "Enabled after Reload."));
    snprintf(g_cm_flip.id, sizeof g_cm_flip.id, "testmod");
    g_cm_flip.at = cm_ms() - CM_FLIP_MS - 1;
    CHECK("no answer within CM_FLIP_MS: the pending text again", !strcmp(cm_state_text(&g_mod, buf, sizeof buf, &col), "Enabled after Reload."));
    g_cm_flip.at = 0;
    CHECK("answered and kept for a reload: \"Enabled after Reload.\"",
          !strcmp(cm_state_text(&g_mod, buf, sizeof buf, &col), "Enabled after Reload."));
    return ut_summary();
}
