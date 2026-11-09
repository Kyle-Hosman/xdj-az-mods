/* SPDX-License-Identifier: GPL-3.0-only */
/* azfeatures.c: the Mod Menu's row table (features.def + registered rows), its values, pins
 * and the azmod.cfg slot mapping. Pure logic: no syscalls; the only allocation is a copy of a
 * registered row's icon. #included by menu.c
 * (so every build of menu.c, the host tests included, carries it). */
#include <stdlib.h>
#include <string.h>

/* ---- the official rows ----------------------------------------------------------------- */
/* clang-format off */
/* GCC 7 false positive: every field below is designated, but a compound literal as a
 * designator's value (AZ_CHOICES) makes it report the fields after it as missing. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static const struct az_row AZ_OFFICIAL[AZF_COUNT] = {
#define AZ_FEATURE(E, KEY, LABEL, PARENT, KIND, FLAGS, CFG, ...)                                  \
    [AZF_##E] = {.d = {.size = sizeof(az_feature_v2), .key = KEY, .label = LABEL, .kind = KIND,     \
                       .flags = FLAGS},                                                             \
                 .parent = AZF_##PARENT, .cfg = CFG, __VA_ARGS__},
#define AZ_ACTION(...)
#define AZ_REGISTERED_ROWS()
#define AZ_RETIRED(n)
#include "features.def"
#undef AZ_FEATURE
#undef AZ_ACTION
#undef AZ_REGISTERED_ROWS
#undef AZ_RETIRED
};
#pragma GCC diagnostic pop
static const struct az_row AZ_ACTIONS[AZA_COUNT] = {
#define AZ_FEATURE(...)
#define AZ_ACTION(E, LABEL, PARENT, KIND, FLAGS, SCREEN, ...)                                     \
    [AZA_##E] = {.d = {.size = sizeof(az_feature_v2), .label = LABEL, .kind = KIND, .flags = FLAGS}, \
                 .parent = AZF_##PARENT, .cfg = -1, .screen = SCREEN, __VA_ARGS__},
#define AZ_REGISTERED_ROWS()
#define AZ_RETIRED(n)
#include "features.def"
#undef AZ_FEATURE
#undef AZ_ACTION
#undef AZ_REGISTERED_ROWS
#undef AZ_RETIRED
};
/* display order: official features and utilities as features.def lists them; R = the
 * registered rows' place */
enum { AZ_ORD_F, AZ_ORD_A, AZ_ORD_R };
static const struct { unsigned char what; short id; } AZ_ORDER[] = {
#define AZ_FEATURE(E, ...) {AZ_ORD_F, AZF_##E},
#define AZ_ACTION(E, ...) {AZ_ORD_A, AZA_##E},
#define AZ_REGISTERED_ROWS() {AZ_ORD_R, 0},
#define AZ_RETIRED(n)
#include "features.def"
#undef AZ_FEATURE
#undef AZ_ACTION
#undef AZ_REGISTERED_ROWS
#undef AZ_RETIRED
};
/* clang-format on */
static const signed char AZ_RETIRED_SLOTS[] = {
#define AZ_FEATURE(...)
#define AZ_ACTION(...)
#define AZ_REGISTERED_ROWS()
#define AZ_RETIRED(n) n,
#include "features.def"
#undef AZ_FEATURE
#undef AZ_ACTION
#undef AZ_REGISTERED_ROWS
#undef AZ_RETIRED
};
_Static_assert(AZF_COUNT + AZA_COUNT <= 127, "AZ_ORDER ids are shorts; items fit");

/* ---- registered rows ------------------------------------------------------------------- */
#define AZ_REG_KEY 25   /* 24 chars + NUL */
#define AZ_REG_LABEL 32 /* 31 bytes + NUL */
#define AZ_REG_HELP 160
#define AZ_REG_CHOICES 8
struct az_reg {
    struct az_row r;
    char owner[33];
    char key[AZ_REG_KEY], label[AZ_REG_LABEL], help[AZ_REG_HELP];
    char choice[AZ_REG_CHOICES][AZ_REG_LABEL];
    const char *choice_p[AZ_REG_CHOICES];
    unsigned char *icon; /* the mod's icon, copied (AZ_ROW_ICON_N squared), or NULL */
    int retired;         /* its mod stopped (azf_retire_owner); the same key registered again revives it */
};
static struct az_reg g_reg[AZ_REG_MAX];
static int g_nreg;               /* registered rows so far (append only) */
static unsigned char g_shown[AZI_COUNT]; /* AZ_ROW_SHOW_GATED rows switched on */
static unsigned char g_changed[AZ_REG_MAX]; /* registered rows whose on_change is due */
void (*azf_log)(const char *fmt, ...);

int az_val[AZ_VAL_COUNT];
int az_eff[AZ_VAL_COUNT];
static uint64_t g_pins[AZ_PIN_WORDS];

const struct az_row *az_row(int item) {
    if (item >= 0 && item < AZF_COUNT)
        return &AZ_OFFICIAL[item];
    if (item >= AZI_REG0 && item < AZI_REG0 + g_nreg)
        return &g_reg[item - AZI_REG0].r;
    if (item >= AZI_ACT0 && item < AZI_COUNT)
        return &AZ_ACTIONS[item - AZI_ACT0];
    return NULL;
}
const char *az_label(int item) {
    const struct az_row *r = az_row(item);
    return r ? r->d.label : NULL;
}
const char *az_pane_label(int item) {
    const struct az_row *r = az_row(item);
    return r ? (r->pane_label ? r->pane_label : r->d.label) : NULL;
}
int az_kind(int item) {
    const struct az_row *r = az_row(item);
    return r ? r->d.kind : -1;
}
int az_parent(int item) {
    const struct az_row *r = az_row(item);
    return r ? r->parent : AZF_NONE;
}
int az_depth(int item) {
    int d = 0;
    for (int p = az_parent(item); p >= 0; p = az_parent(p))
        d++;
    return d;
}
int az_screen(int item) {
    const struct az_row *r = az_row(item);
    return r && r->screen > 0 ? r->screen : -1; /* 0 (AZ_CLOSED) = opens none */
}
bool az_has_value(int item) {
    int k = az_kind(item);
    return item < AZ_VAL_COUNT && (k == AZ_ROW_TOGGLE || k == AZ_ROW_CHOICE);
}
bool az_hidden(int item) {
    const struct az_row *r = az_row(item);
    if (!r)
        return true;
    if (r->d.flags & AZ_ROW_HIDDEN)
        return true;
    if ((r->d.flags & AZ_ROW_SHOW_GATED) && !g_shown[item])
        return true;
    /* a row's children live on the screen it opens (the Overlays readouts) */
    if (r->parent >= 0 && az_screen(r->parent) >= 0)
        return true;
    return false;
}
int (*azf_screen_target)(int item);
bool az_active(int item) {
    if (item >= AZI_REG0 && item < AZI_ACT0 && az_kind(item) == AZ_ROW_SCREEN && azf_screen_target &&
        !azf_screen_target(item))
        return false; /* nothing to open */
    for (int p = az_parent(item); p >= 0; p = az_parent(p))
        if (az_kind(p) == AZ_ROW_TOGGLE && !az_on(p))
            return false; /* a GROUP parent has no switch */
    for (int p = item; p >= 0; p = az_parent(p)) { /* a registered row's own availability */
        const struct az_row *r = az_row(p);
        if (p >= AZI_REG0 && p < AZI_ACT0 && r && r->d.available && !r->d.available(r->d.user))
            return false;
    }
    return true;
}
bool az_descends(int item, int top) {
    for (int q = az_parent(item); q >= 0; q = az_parent(q))
        if (q == top)
            return true;
    return false;
}
int az_n_choices(int item) {
    const struct az_row *r = az_row(item);
    return r && r->d.kind == AZ_ROW_CHOICE ? r->d.n_choices : 0;
}
const char *az_choice_label(int item, int k) {
    const struct az_row *r = az_row(item);
    return r && r->d.kind == AZ_ROW_CHOICE && k >= 0 && k < r->d.n_choices ? r->d.choices[k] : "";
}
void az_row_show(int item, bool shown) {
    if (item >= 0 && item < AZI_COUNT)
        g_shown[item] = shown;
}
int az_display_order(int *out) {
    int n = 0;
    for (size_t p = 0; p < sizeof AZ_ORDER / sizeof AZ_ORDER[0]; p++) {
        if (AZ_ORDER[p].what == AZ_ORD_F)
            out[n++] = AZ_ORDER[p].id;
        else if (AZ_ORDER[p].what == AZ_ORD_A)
            out[n++] = AZI_ACT0 + AZ_ORDER[p].id;
        else
            for (int k = 0; k < g_nreg; k++)
                out[n++] = AZI_REG0 + k;
    }
    return n;
}

/* ---- values ----------------------------------------------------------------------------- */
static void az_publish(void) { /* row_value for every row: one store each */
    for (int i = 0; i < AZ_VAL_COUNT; i++) {
        int k = az_kind(i), v = 0;
        if (k == AZ_ROW_CHOICE)
            v = az_get(i);
        else if (k == AZ_ROW_TOGGLE) {
            v = az_get(i) != 0;
            for (int p = az_parent(i); v && p >= 0; p = az_parent(p))
                if (az_kind(p) == AZ_ROW_TOGGLE && !az_get(p))
                    v = 0;
        }
        __atomic_store_n(&az_eff[i], v, __ATOMIC_RELAXED);
    }
}
static int az_clamp(int id, int v) {
    if (az_kind(id) == AZ_ROW_CHOICE) {
        int n = az_n_choices(id);
        return v >= 0 && v < n ? v : az_row(id)->d.default_choice;
    }
    return v != 0;
}
void az_set(int id, int v) {
    if (!az_has_value(id))
        return;
    v = az_clamp(id, v);
    if ((az_row(id)->d.flags & AZ_ROW_FORCE_ON) && az_kind(id) == AZ_ROW_TOGGLE)
        v = 1;
    if (az_get(id) == v)
        return;
    __atomic_store_n(&az_val[id], v, __ATOMIC_RELAXED);
    if (id >= AZI_REG0)
        g_changed[id - AZI_REG0] = 1;
    az_publish();
}
void az_toggle(int id) {
    if (!az_has_value(id))
        return;
    const struct az_row *r = az_row(id);
    int v = r->d.kind == AZ_ROW_CHOICE ? (az_get(id) + 1) % az_n_choices(id) : !az_get(id);
    az_set(id, v);
    if (id < AZF_COUNT && r->d.on_change) /* an official row's hook runs now (it only moves
                                             other values); a mod's after the save */
        r->d.on_change(az_get(id), r->d.user);
}
static int az_default_of(int id) {
    const struct az_row *r = az_row(id);
    if (!r)
        return 0;
    if (r->d.kind == AZ_ROW_CHOICE)
        return r->d.default_choice;
    return (r->d.flags & (AZ_ROW_DEFAULT_ON | AZ_ROW_FORCE_ON)) ? 1 : 0;
}
void az_defaults(void) {
    for (int i = 0; i < AZ_VAL_COUNT; i++)
        __atomic_store_n(&az_val[i], az_has_value(i) ? az_default_of(i) : 0, __ATOMIC_RELAXED);
    az_publish();
}
void az_apply_forced(void) {
    for (int i = 0; i < AZ_VAL_COUNT; i++)
        if (az_row(i) && (az_row(i)->d.flags & AZ_ROW_FORCE_ON))
            __atomic_store_n(&az_val[i], 1, __ATOMIC_RELAXED);
    az_publish();
}
void az_fire_changes(void) {
    for (int k = 0; k < g_nreg; k++)
        if (g_changed[k]) {
            g_changed[k] = 0;
            if (g_reg[k].r.d.on_change)
                g_reg[k].r.d.on_change(az_get(AZI_REG0 + k), g_reg[k].r.d.user);
        }
}

/* ---- named callbacks of features.def ----------------------------------------------------- */
/* 60 FPS is a group with no switch of its own, but its box is drawn lit while either member is
 * on, so a tap on it reads as a switch. It is a bypass: lit -> save which members are on and
 * switch them off; dark -> restore exactly the saved ones, or both if nothing is saved. */
void azf_fps60_bypass(void *user) {
    (void) user;
    if (az_on(AZF_FPS60_WAVEFORMS) || az_on(AZF_FPS60_UI)) {
        /* "every member on" is stored as an empty memory: the restore below turns both on
         * for it anyway, so off -> on -> off returns to the very same bytes */
        bool all = az_on(AZF_FPS60_WAVEFORMS) && az_on(AZF_FPS60_UI);
        az_set(AZF_FPS60_SAVED_WAVEFORMS, !all && az_on(AZF_FPS60_WAVEFORMS));
        az_set(AZF_FPS60_SAVED_UI, !all && az_on(AZF_FPS60_UI));
        az_set(AZF_FPS60_WAVEFORMS, 0);
        az_set(AZF_FPS60_UI, 0);
    } else {
        bool saved = az_on(AZF_FPS60_SAVED_WAVEFORMS) || az_on(AZF_FPS60_SAVED_UI);
        az_set(AZF_FPS60_WAVEFORMS, saved ? az_on(AZF_FPS60_SAVED_WAVEFORMS) : 1);
        az_set(AZF_FPS60_UI, saved ? az_on(AZF_FPS60_SAVED_UI) : 1);
        az_set(AZF_FPS60_SAVED_WAVEFORMS, 0);
        az_set(AZF_FPS60_SAVED_UI, 0);
    }
}
/* A saved "every member" (1.2.2 stored it as both bits) is the empty memory: same
 * behavior, and it round-trips. */
static void azf_fps60_normalize(void) {
    if (az_on(AZF_FPS60_SAVED_WAVEFORMS) && az_on(AZF_FPS60_SAVED_UI)) {
        __atomic_store_n(&az_val[AZF_FPS60_SAVED_WAVEFORMS], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&az_val[AZF_FPS60_SAVED_UI], 0, __ATOMIC_RELAXED);
    }
}
void az_after_load(void) {
    azf_fps60_normalize();
    az_apply_forced(); /* publishes */
}
/* A 60 FPS member chosen by hand replaces whatever the group switch saved. */
void azf_fps60_member_changed(int value, void *user) {
    (void) value;
    (void) user;
    az_set(AZF_FPS60_SAVED_WAVEFORMS, 0);
    az_set(AZF_FPS60_SAVED_UI, 0);
}

/* ---- pins: one bit per item, any number of rows (a u64 held 64 at most) ----------------- */
bool az_pinned(int item) {
    if (item < 0 || item >= AZI_COUNT)
        return false;
    return (__atomic_load_n(&g_pins[item >> 6], __ATOMIC_RELAXED) >> (item & 63)) & 1;
}
bool az_pinnable(int item) {
    return az_has_value(item);
}
void az_pin_toggle(int item) {
    if (az_pinnable(item))
        __atomic_xor_fetch(&g_pins[item >> 6], 1ull << (item & 63), __ATOMIC_RELAXED);
}
void az_pins_clear(void) {
    for (int w = 0; w < AZ_PIN_WORDS; w++)
        __atomic_store_n(&g_pins[w], 0, __ATOMIC_RELAXED);
}
unsigned long long az_pins_legacy(void) {
    unsigned long long m = 0;
    for (int i = 0; i < AZF_COUNT; i++)
        if (az_pinned(i) && AZ_OFFICIAL[i].cfg >= 0)
            m |= 1ull << AZ_OFFICIAL[i].cfg; /* a radio: its first slot */
    return m;
}
void az_pins_from_legacy(unsigned long long mask) {
    az_pins_clear();
    for (int i = 0; i < AZF_COUNT; i++) {
        const struct az_row *r = &AZ_OFFICIAL[i];
        if (r->cfg < 0 || !az_pinnable(i))
            continue;
        int n = r->radio_n ? r->radio_n : 1;
        for (int k = 0; k < n; k++)
            if (mask >> (r->cfg + k) & 1) {
                az_pin_toggle(i);
                break;
            }
    }
}
int az_pinned_items(int *out, int max) {
    int ord[AZI_COUNT], n = az_display_order(ord), m = 0;
    for (int p = 0; p < n && m < max; p++)
        if (az_pinned(ord[p]))
            out[m++] = ord[p];
    return m;
}

/* ---- azmod.cfg line 1: char i = slot i ------------------------------------------------- */
int az_item_of_cfg(int slot) {
    for (int i = 0; i < AZF_COUNT; i++) {
        const struct az_row *r = &AZ_OFFICIAL[i];
        if (r->cfg >= 0 && slot >= r->cfg && slot < r->cfg + (r->radio_n ? r->radio_n : 1))
            return i;
    }
    return -1;
}
bool az_cfg_retired(int slot) {
    for (size_t k = 0; k < sizeof AZ_RETIRED_SLOTS; k++)
        if (AZ_RETIRED_SLOTS[k] == slot)
            return true;
    return false;
}
int az_cfg_char(int slot) {
    int i = az_item_of_cfg(slot);
    if (i < 0)
        return '0';
    const struct az_row *r = &AZ_OFFICIAL[i];
    if (r->radio_n)
        return az_get(i) == slot - r->cfg ? '1' : '0';
    return az_get(i) ? '1' : '0';
}
/* ---- registration ------------------------------------------------------------------------ */
static int az_key_ok(const char *k) {
    size_t n = k ? strlen(k) : 0;
    if (n < 1 || n > 24)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!((k[i] >= 'a' && k[i] <= 'z') || (k[i] >= '0' && k[i] <= '9') || k[i] == '-'))
            return 0;
    return 1;
}
static void az_logf(const char *mod, const char *key, const char *why) {
    if (azf_log)
        azf_log("rows: %s:%s refused: %s\n", mod ? mod : "?", key ? key : "?", why);
}
static void az_cpy(char *d, size_t n, const char *s) {
    size_t k = 0;
    if (s)
        for (; s[k] && k + 1 < n; k++)
            d[k] = s[k];
    d[k] = 0;
}
/* A mod's row: validated, its strings copied, appended after the official rows. The mod's
 * own key, the parent rule (one of its earlier GROUP/TOGGLE rows, never an official row) and
 * the limits are checked here; the SDK entry point is a thin wrapper (azf_from_row_v1). An
 * unknown kind (a newer SDK) is skipped and logged, never a crash. Its value is saved in
 * azmod.conf as "<mod>:<key>"; that key is kept when the mod is removed or fails to load, so
 * reinstalling it restores the DJ's settings. */
int azf_register(const char *mod_id, const az_feature_v2 *in) {
    if (!in || !mod_id || !az_key_ok(mod_id)) {
        az_logf(mod_id, NULL, "bad mod id");
        return -1;
    }
    az_feature_v2 f;
    memset(&f, 0, sizeof f);
    if (in->size < (int) (offsetof(az_feature_v2, flags) + sizeof(unsigned))) {
        az_logf(mod_id, NULL, "descriptor too small");
        return -1;
    }
    memcpy(&f, in, (size_t) in->size < sizeof f ? (size_t) in->size : sizeof f);
    f.size = sizeof f;
    if (!az_key_ok(f.key)) {
        az_logf(mod_id, f.key, "bad key ([a-z0-9-], 1..24)");
        return -1;
    }
    if (f.kind < AZ_ROW_TOGGLE || f.kind > AZ_ROW_CHOICE) {
        az_logf(mod_id, f.key, "unknown kind (skipped)");
        return -1;
    }
    if (!f.label || !f.label[0] || strlen(f.label) > 31) {
        az_logf(mod_id, f.key, "label empty or over 31 bytes");
        return -1;
    }
    if (f.kind == AZ_ROW_CHOICE &&
        (f.n_choices < 2 || f.n_choices > AZ_REG_CHOICES || !f.choices || f.default_choice < 0 ||
         f.default_choice >= f.n_choices)) {
        az_logf(mod_id, f.key, "a CHOICE needs 2..8 choices and a default among them");
        return -1;
    }
    if (f.kind == AZ_ROW_ACTION && !f.on_activate) {
        az_logf(mod_id, f.key, "an ACTION needs on_activate");
        return -1;
    }
    int mine = 0, parent = AZF_NONE, slot = -1;
    for (int k = 0; k < g_nreg; k++) {
        if (strcmp(g_reg[k].owner, mod_id))
            continue;
        mine++;
        if (!strcmp(g_reg[k].key, f.key)) {
            if (g_reg[k].retired) { /* its mod was turned off and is starting again: same row */
                slot = k;
                continue;
            }
            az_logf(mod_id, f.key, "key already registered");
            return -1;
        }
        if (f.parent && !g_reg[k].retired && !strcmp(g_reg[k].key, f.parent) &&
            (g_reg[k].r.d.kind == AZ_ROW_GROUP || g_reg[k].r.d.kind == AZ_ROW_TOGGLE))
            parent = AZI_REG0 + k;
    }
    if (f.parent && parent == AZF_NONE) {
        az_logf(mod_id, f.key, "parent is not one of this mod's earlier GROUP/TOGGLE rows");
        return -1;
    }
    if (slot < 0 && (mine >= AZ_REG_PER_MOD || g_nreg >= AZ_REG_MAX)) {
        az_logf(mod_id, f.key, "over the row limit (16 per mod, 128 in all)");
        return -1;
    }
    struct az_reg *g = &g_reg[slot >= 0 ? slot : g_nreg];
    free(g->icon);
    memset(g, 0, sizeof *g);
    az_cpy(g->owner, sizeof g->owner, mod_id);
    az_cpy(g->key, sizeof g->key, f.key);
    az_cpy(g->label, sizeof g->label, f.label);
    az_cpy(g->help, sizeof g->help, f.help);
    g->r.d = f;
    g->r.d.flags = f.flags & AZ_ROW_DEFAULT_ON; /* the built-in flags are not a mod's */
    g->r.d.key = g->key;
    g->r.d.label = g->label;
    g->r.d.help = f.help ? g->help : NULL;
    g->r.d.parent = NULL;
    g->r.d.icon = NULL;
    if (f.icon) { /* copied: the mod's buffer may be anything, the menu draws this one */
        g->icon = malloc((size_t) AZ_ROW_ICON_N * AZ_ROW_ICON_N);
        if (g->icon) {
            memcpy(g->icon, f.icon, (size_t) AZ_ROW_ICON_N * AZ_ROW_ICON_N);
            g->r.d.icon = g->icon;
        }
    }
    if (f.kind == AZ_ROW_CHOICE) {
        for (int k = 0; k < f.n_choices; k++) {
            az_cpy(g->choice[k], sizeof g->choice[k], f.choices[k]);
            g->choice_p[k] = g->choice[k];
        }
        g->r.d.choices = g->choice_p;
    } else
        g->r.d.choices = NULL;
    g->r.parent = parent;
    g->r.cfg = -1;
    if (slot >= 0) { /* revived: its handle and the value it had stay */
        az_publish();
        return AZI_REG0 + slot;
    }
    int id = AZI_REG0 + g_nreg;
    g_nreg++;
    __atomic_store_n(&az_val[id], az_default_of(id), __ATOMIC_RELAXED);
    az_publish();
    return id;
}
/* A mod that is not running (its init() failed after it registered rows, or it was stopped):
 * its rows leave the menu and none of their callbacks is called again. Their values and saved
 * keys stay. UI thread. Returns the rows retired now (0 when there were none, or already). */
int azf_retire_owner(const char *mod_id) {
    int n = 0;
    for (int k = 0; mod_id && k < g_nreg; k++) {
        struct az_reg *g = &g_reg[k];
        if (strcmp(g->owner, mod_id) || (g->r.d.flags & AZ_ROW_HIDDEN))
            continue;
        g->r.d.flags |= AZ_ROW_HIDDEN;
        g->retired = 1;
        g->r.d.available = NULL;
        g->r.d.on_change = NULL;
        g->r.d.on_activate = NULL;
        g->r.d.screen = NULL;
        g_changed[k] = 0;
        n++;
    }
    if (n)
        az_publish();
    return n;
}
/* Forget every registered row (a start with no mods loaded; the host tests). Their saved
 * keys stay in azmod.conf. UI thread, before anything holds a handle. */
void azf_reset_registered(void) {
    for (int k = 0; k < g_nreg; k++) {
        __atomic_store_n(&az_val[AZI_REG0 + k], 0, __ATOMIC_RELAXED);
        free(g_reg[k].icon);
        g_reg[k].icon = NULL;
    }
    for (int i = AZI_REG0; i < AZI_ACT0; i++)
        if (az_pinned(i))
            az_pin_toggle(i);
    g_nreg = 0;
    memset(g_changed, 0, sizeof g_changed);
    az_publish();
}
/* The SDK's az_row_v1 -> the core's descriptor: a field-for-field copy of what the caller's
 * struct holds (r->size; a field it does not know stays zero). register_row (sdk_rows.c) is
 * this plus azf_register. */
int azf_from_row_v1(const struct az_row_v1 *r, az_feature_v2 *f) {
    memset(f, 0, sizeof *f);
    if (!r || r->size < (int) (offsetof(az_row_v1, flags) + sizeof r->flags))
        return -1;
#define AZ_V1_HAS(field) ((size_t) r->size >= offsetof(az_row_v1, field) + sizeof r->field)
    f->size = sizeof *f;
    f->key = r->key;
    f->label = r->label;
    f->kind = r->kind;
    f->flags = r->flags;
    if (AZ_V1_HAS(parent)) f->parent = r->parent;
    if (AZ_V1_HAS(help)) f->help = r->help;
    if (AZ_V1_HAS(icon)) f->icon = r->icon;
    if (AZ_V1_HAS(choices)) f->choices = r->choices;
    if (AZ_V1_HAS(n_choices)) f->n_choices = r->n_choices;
    if (AZ_V1_HAS(default_choice)) f->default_choice = r->default_choice;
    if (AZ_V1_HAS(screen)) f->screen = r->screen;
    if (AZ_V1_HAS(available)) f->available = r->available;
    if (AZ_V1_HAS(on_change)) f->on_change = r->on_change;
    if (AZ_V1_HAS(on_activate)) f->on_activate = r->on_activate;
    if (AZ_V1_HAS(user)) f->user = r->user;
#undef AZ_V1_HAS
    return 0;
}
_Static_assert(sizeof(az_row_v1) == sizeof(az_feature_v2), "az_row_v1 and az_feature_v2 are the same shape");
int azf_row_value(int handle) {
    if (handle < 0 || handle >= AZ_VAL_COUNT || !az_has_value(handle))
        return -1;
    return az_value(handle);
}
int azf_row_set(int handle, int value) {
    if (handle < AZI_REG0 || handle >= AZI_REG0 + g_nreg || !az_has_value(handle))
        return -1; /* an official row (row_lookup's handles) is read-only to a mod */
    if (az_kind(handle) == AZ_ROW_CHOICE ? (value < 0 || value >= az_n_choices(handle))
                                         : (value != 0 && value != 1))
        return -1;
    az_set(handle, value);
    return 0;
}
int azf_row_lookup(const char *key) {
    if (!key)
        return -1;
    for (int i = 0; i < AZF_COUNT; i++)
        if (!strcmp(AZ_OFFICIAL[i].d.key, key) && az_has_value(i) &&
            !(AZ_OFFICIAL[i].d.flags & AZ_ROW_HIDDEN))
            return i;
    return -1;
}
const char *azf_owner(int item) {
    return item >= AZI_REG0 && item < AZI_REG0 + g_nreg ? g_reg[item - AZI_REG0].owner : "";
}
int azf_conf_key(int item, char *buf, int n) {
    const struct az_row *r = az_row(item);
    if (!r || !az_has_value(item) || !r->d.key)
        return 0;
    if (item >= AZI_REG0)
        snprintf(buf, (size_t) n, "%s:%s", azf_owner(item), r->d.key);
    else
        snprintf(buf, (size_t) n, "%s", r->d.key);
    return 1;
}
