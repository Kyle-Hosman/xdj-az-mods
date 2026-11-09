/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file sdk_rows.c
 * @brief Community mod services: Mod Menu rows (register_row, row_value, row_set, row_lookup)
 *
 * A mod's rows join the feature table (azfeatures.c) after the official ones, so the menu,
 * the settings file and the pins treat them exactly as official rows: the value is saved as
 * "<id>:<key>" in azmod.conf, restored when the row registers, and kept when the mod goes.
 * register_row is init()-only (the menu's list is fixed once the mods are loaded); row_value
 * and row_lookup are one atomic load or a table walk with no lock, safe from any thread;
 * row_set is the UI thread's and saves like a tap on the row would.
 *
 * Included by community.c (it uses the community host's mod records and azmod.c's settings).
 */

/* a SCREEN row opens its own screen, else its mod's page: with neither it is grayed (azfeatures.c) */
static int cm_row_has_target(int item) {
    const struct az_row *r = az_row(item);
    return azcm_row_target(azf_owner(item), r ? r->d.screen : NULL);
}

static int cm_register_row(const char *id, const az_row_v1 *r) {
    cm_mod *c = cm_self(id, CM_RA(), "register_row");
    if (!c)
        return -1;
    if (!g_sdk_in_init || (int) (c - g_cm) != g_cm_loading) {
        cm_logf(c, 1, "register_row refused: call it from init()");
        return -1;
    }
    az_feature_v2 f;
    if (azf_from_row_v1(r, &f) != 0) {
        cm_logf(c, 1, "register_row refused: the row's size is too small");
        return -1;
    }
    /* the callbacks run on the core's UI thread: they must be this mod's own code, like every
     * other callback a mod hands the core */
    uintptr_t cb[3] = {(uintptr_t) f.available, (uintptr_t) f.on_change, (uintptr_t) f.on_activate};
    for (int i = 0; i < 3; i++)
        if (cb[i] && (!c->hi || cb[i] < c->lo || cb[i] >= c->hi)) {
            cm_logf(c, 1, "register_row refused for \"%s\": a callback is not in this mod's code",
                    r->key ? r->key : "");
            return -1;
        }
    if (f.kind == AZ_ROW_SCREEN && f.screen) { /* the screen it opens: complete, and this mod's */
        const az_screen_v1 *sc = f.screen;
        uintptr_t fn[3] = {(uintptr_t) sc->on_draw, (uintptr_t) sc->on_input, (uintptr_t) sc->on_enter};
        int bad = sc->size < (int) (offsetof(az_screen_v1, on_input) + sizeof sc->on_input) || !sc->on_draw;
        for (int i = 0; i < 3 && !bad; i++)
            bad = fn[i] && (!c->hi || fn[i] < c->lo || fn[i] >= c->hi);
        if (bad) {
            cm_logf(c, 1, "register_row refused for \"%s\": its screen is incomplete or not in this "
                          "mod's code",
                    r->key ? r->key : "");
            return -1;
        }
    }
    azf_screen_target = cm_row_has_target;
    int h = register_row_internal(c->id, &f);
    if (h < 0)
        cm_logf(c, 1, "register_row refused for \"%s\" (bad key, label, kind or parent, or over "
                      "the limit of 16 rows per mod)",
                r->key ? r->key : "");
    return h;
}

static int cm_row_value(int handle) {
    return azf_row_value(handle);
}

static int cm_row_set(int handle, int value) {
    if (!g_cm_on_ui)
        return -1; /* the menu and the settings file are the UI thread's */
    /* a mod sets only its own rows, while it runs: row_set takes no id, so the row's owner must
     * be the mod whose code called (an official row has no owner and is refused) */
    cm_mod *c = cm_find(azf_owner(handle));
    if (!c || !cm_called_by(c, CM_RA()) ||
        (!cm_running(c) && (int) (c - g_cm) != g_cm_loading)) {
        dbg("community: row_set %d refused: not a running mod's own row, from its own code\n", handle);
        return -1;
    }
    if (azf_row_set(handle, value) != 0)
        return -1;
    g_state.config_dirty = true; /* saved, and on_change run, as after a tap on the row */
    return 0;
}

static int cm_row_lookup(const char *official_key) {
    return azf_row_lookup(official_key);
}

/* UI thread, every tick: the rows of a mod that is not running leave the menu, and none of their
 * callbacks is called again (an init() that failed after register_row, a mod stopped). Only when
 * the set of running mods changes. */
static void sdk_rows_tick(void) {
    static int seen_n = -1, seen_live = -1;
    if (g_cm_n == seen_n && cm_nlive() == seen_live)
        return;
    seen_n = g_cm_n;
    seen_live = cm_nlive();
    for (int i = 0; i < g_cm_n; i++)
        if (!cm_running(&g_cm[i]) && i != g_cm_loading && azf_retire_owner(g_cm[i].id) > 0)
            cm_logf(&g_cm[i], 1, "its Mod Menu rows are hidden: the mod is not running");
}
/* A mod turned off live: its rows leave the menu now (init() registers them again, same rows). */
static void sdk_rows_forget(cm_mod *c) {
    if (azf_retire_owner(c->id) > 0)
        cm_logf(c, 1, "its Mod Menu rows are hidden: the mod is not running");
}
