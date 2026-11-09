/* SPDX-License-Identifier: GPL-3.0-only */
/* row_set (cm_row_set, extracted out of guest/azmod/sdk_rows.c by run.sh, with community.c's
 * cm_called_by): it changes a Mod Menu row's value as if the user had, and takes no mod id, so:
 *   - a running mod sets its own row from its own code (also inside its init());
 *   - another mod's row, a row of a mod that is not running, an official row (no owner), or a
 *     call off the core's UI thread: refused, nothing changed, nothing saved. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "check.h"

enum {
    AZCM_LOADED = 1,
    AZCM_FAILED = 4
};
typedef struct {
    char id[33];
    uintptr_t lo, hi;
    int state;
} cm_mod;
static int cm_running(const cm_mod *c) { return __atomic_load_n(&c->state, __ATOMIC_ACQUIRE) == AZCM_LOADED; }
static cm_mod g_cm[2] = {{"mod-a", 0x1000, 0x2000, AZCM_LOADED}, {"mod-b", 0x3000, 0x4000, AZCM_LOADED}};
static int g_cm_loading = -1, g_cm_on_ui = 1;
static uintptr_t g_ra; /* the calling code's address, as CM_RA() would read it */
#define CM_RA() g_ra
static struct {
    int config_dirty;
} g_state;
static void dbg(const char *fmt, ...) { (void) fmt; }
static cm_mod *cm_find(const char *id) {
    for (int i = 0; i < 2; i++)
        if (id && !strcmp(g_cm[i].id, id))
            return &g_cm[i];
    return NULL;
}
/* rows: 0 an official row, 1 mod-a's, 2 mod-b's */
static int g_val[3], g_sets;
static const char *azf_owner(int item) { return item == 1 ? "mod-a" : item == 2 ? "mod-b"
                                                                                : ""; }
static int azf_row_set(int item, int v) {
    if (item < 1 || item > 2)
        return -1;
    g_val[item] = v;
    g_sets++;
    return 0;
}
#include SDKROWS_INC

static int unchanged(void) { return g_sets == 0 && !g_state.config_dirty; }
static void reset(void) { g_sets = 0, g_state.config_dirty = 0; }

int main(void) {
    ut_suite = "sdk: row_set";
    g_ra = 0x1800; /* inside mod-a's code */
    CHECK("its own row, from its own code: set and saved", cm_row_set(1, 1) == 0 && g_val[1] == 1 && g_state.config_dirty);
    reset();
    CHECK("another mod's row: refused, nothing changed", cm_row_set(2, 1) == -1 && g_val[2] == 0 && unchanged());
    CHECK("an official row: refused", cm_row_set(0, 1) == -1 && unchanged());
    g_ra = 0x9000; /* code no mod owns */
    CHECK("a call from outside the owner's code: refused", cm_row_set(1, 0) == -1 && g_val[1] == 1 && unchanged());
    g_ra = 0x1800;
    g_cm[0].state = AZCM_FAILED;
    CHECK("the owner not running: refused", cm_row_set(1, 0) == -1 && unchanged());
    g_cm_loading = 0;
    CHECK("inside the owner's own init(): set", cm_row_set(1, 0) == 0 && g_val[1] == 0);
    reset();
    g_cm_loading = -1;
    g_cm[0].state = AZCM_LOADED;
    g_cm_on_ui = 0;
    CHECK("off the UI thread: refused", cm_row_set(1, 1) == -1 && unchanged());
    g_cm_on_ui = 1;
    g_ra = 0x3800;
    CHECK("mod-b's own row from mod-b's code: set (CONTROL)", cm_row_set(2, 2) == 0 && g_val[2] == 2);
    return ut_summary();
}
