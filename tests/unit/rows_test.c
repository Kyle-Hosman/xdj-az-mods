/* SPDX-License-Identifier: GPL-3.0-only */
/* Registered Mod Menu rows (azfeatures.c azf_register, behind the SDK's register_row), driven through the same menu code as the official rows:
 *   - one row of each kind (TOGGLE, GROUP, ACTION, SCREEN, CHOICE of 2 and of 3) plus a child
 *     grayed by its parent: they list right under Community Mods, before the other utilities, are
 *     drawn by the full-screen menu, and a knob press does to them what it does to an
 *     official row of their kind;
 *   - row_value: TOGGLE = own and every parent, CHOICE = the index, -1 = a bad handle;
 *     row_set: only a registered row; row_lookup: an official row by its key, read-only;
 *   - refusals: a bad key, an unknown kind (a newer SDK: skipped and logged), a label over
 *     31 bytes, a parent that is not this mod's own earlier GROUP/TOGGLE (never an official
 *     row), the limits (16 per mod, 128 in all);
 *   - on_change runs after the save (az_fire_changes), with the value;
 *   - az_row_v1 (azmod-mod.h) -> az_feature_v2 is a field copy, size-versioned;
 *   - a row's icon is copied at registration and drawn like an official row's.
 * menu.c is #included (MENU_C) for its statics; the text drawn is captured. */
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "check.h"
#include MENU_C

static char g_seen[1 << 16];
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
static int seen(const char *s) {
    char k[96];
    snprintf(k, sizeof k, "%s\n", s);
    return strstr(g_seen, k) != NULL;
}
#define W 1280
#define H 800
static uint8_t g_px[W * H * 4];
static azmod_fb g_fb = {g_px, W, H, W * 4};
static void draw(azmod_state *s) {
    memset(g_px, 0, sizeof g_px);
    g_seen_n = 0;
    g_seen[0] = 0;
    azmod_composite(s, &g_fb);
}
static void knob(azmod_state *s, int d, bool click) { azmod_input(s, &g_fb, 0, 0, false, d, click); }
static char g_log[2048];
static void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t n = strlen(g_log);
    vsnprintf(g_log + n, sizeof g_log - n, fmt, ap);
    va_end(ap);
}

static int g_go, g_changed_v = -9, g_changed_n;
static void go(void *u) {
    (void) u;
    g_go++;
}
static void changed(int v, void *u) {
    (void) u;
    g_changed_v = v;
    g_changed_n++;
}
static const char *const MODE[] = {"Dry", "Wet"};
static const char *const BANK[] = {"A", "B", "C"};
static const struct az_screen_v1 *const PAGE = (const struct az_screen_v1 *) 0x1; /* opaque */

/* select a left entry by label with the knob, from the first entry */
/* lit pixels of an item's icon drawn into a clean 48 px box */
static int icon_ink(int item) {
    memset(g_px, 0, sizeof g_px);
    draw_item_icon(&g_fb, 10, 10, 48, item, 0x00ffffffu);
    int n = 0;
    for (int y = 10; y < 58; y++)
        for (int x = 10; x < 58; x++)
            n += g_px[(y * W + x) * 4] != 0;
    return n;
}
static int select_left(azmod_state *s, const char *label) {
    g_last_left = -1;
    azmod_menu_reset(s);
    int first = s->left_sel;
    for (int k = 0; k < 64; k++) {
        if (az_label(s->left_sel) && !strcmp(az_label(s->left_sel), label)) return 1;
        knob(s, 1, false);
        if (s->left_sel == first) break;
    }
    return 0;
}
/* knob into the pane and onto the row with this item */
static int select_row(azmod_state *s, int item) {
    knob(s, 0, true);
    int r0 = s->menu_sel;
    for (int k = 0; k < 32; k++) {
        if (s->menu_sel == item) return 1;
        knob(s, 1, false);
        if (s->menu_sel == r0) break;
    }
    return 0;
}

int main(void) {
    ut_suite = "registered rows";
    azf_log = logf_;
    az_defaults();
    az_feature_v2 f;
#define ROW(...) (f = (az_feature_v2) {.size = sizeof f, __VA_ARGS__}, azf_register("testmod", &f))
    int fx = ROW(.key = "fx", .label = "FX Bus", .kind = AZ_ROW_TOGGLE, .help = "A test switch.", .on_change = changed);
    int wet = ROW(.key = "fx-wet", .label = "Wet Path", .kind = AZ_ROW_TOGGLE, .parent = "fx", .flags = AZ_ROW_DEFAULT_ON, .help = "Child of FX Bus.");
    int mode = ROW(.key = "fx-mode", .label = "Mode", .kind = AZ_ROW_CHOICE, .parent = "fx", .choices = MODE, .n_choices = 2);
    int bank = ROW(.key = "fx-bank", .label = "Bank", .kind = AZ_ROW_CHOICE, .parent = "fx", .choices = BANK, .n_choices = 3, .default_choice = 1);
    int grp = ROW(.key = "tools", .label = "Tools", .kind = AZ_ROW_GROUP, .help = "A test group.");
    int toola = ROW(.key = "tool-a", .label = "Tool A", .kind = AZ_ROW_TOGGLE, .parent = "tools");
    int act = ROW(.key = "go", .label = "Go", .kind = AZ_ROW_ACTION, .on_activate = go);
    int scr = ROW(.key = "page", .label = "Page", .kind = AZ_ROW_SCREEN, .screen = PAGE);
    CHECK("eight rows registered, in order after the official ones",
          fx == AZI_REG0 && wet == fx + 1 && mode == fx + 2 && bank == fx + 3 && grp == fx + 4 &&
              toola == fx + 5 && act == fx + 6 && scr == fx + 7);

    /* refusals */
    g_log[0] = 0;
    CHECK("refused: a parent that is an official row", ROW(.key = "x1", .label = "X", .kind = AZ_ROW_TOGGLE, .parent = "stems") < 0);
    CHECK("refused: a parent that is this mod's ACTION", ROW(.key = "x2", .label = "X", .kind = AZ_ROW_TOGGLE, .parent = "go") < 0);
    CHECK("refused: a parent of another mod", (f = (az_feature_v2) {.size = sizeof f, .key = "x3", .label = "X", .kind = AZ_ROW_TOGGLE, .parent = "fx"}, azf_register("othermod", &f)) < 0);
    CHECK("refused: a bad key", ROW(.key = "Bad Key", .label = "X", .kind = AZ_ROW_TOGGLE) < 0);
    CHECK("refused: the same key twice", ROW(.key = "fx", .label = "X", .kind = AZ_ROW_TOGGLE) < 0);
    CHECK("refused: a label over 31 bytes", ROW(.key = "x4", .label = "abcdefghijklmnopqrstuvwxyz012345", .kind = AZ_ROW_TOGGLE) < 0);
    CHECK("refused: a CHOICE with 9 options", ROW(.key = "x5", .label = "X", .kind = AZ_ROW_CHOICE, .choices = BANK, .n_choices = 9) < 0);
    CHECK("skipped and logged: an unknown kind (a newer SDK)", ROW(.key = "x6", .label = "X", .kind = 42) < 0 && strstr(g_log, "unknown kind"));
    /* size versioning: a descriptor that ends after `flags` registers, the rest zero */
    struct {
        int size;
        const char *key, *label;
        int kind;
        unsigned flags;
    } small = {
        sizeof small, "small", "Small", AZ_ROW_TOGGLE, AZ_ROW_DEFAULT_ON};
    int sm = azf_register("smallmod", (const az_feature_v2 *) &small);
    CHECK("an older, smaller descriptor registers; unknown fields are zero",
          sm > scr && az_on(sm) && az_row(sm)->d.help == NULL && az_parent(sm) < 0);
    /* the per-mod limit */
    int last = 0, n = 0;
    char keys[20][8];
    for (int k = 0; k < 20; k++) {
        snprintf(keys[k], sizeof keys[k], "k%d", k);
        f = (az_feature_v2) {.size = sizeof f, .key = keys[k], .label = "K", .kind = AZ_ROW_TOGGLE};
        last = azf_register("bulkmod", &f);
        n += last >= 0;
    }
    CHECK("16 rows per mod, the 17th refused", n == 16 && last < 0);

    /* values */
    CHECK("defaults: TOGGLE off / DEFAULT_ON on, CHOICE default_choice", az_get(fx) == 0 && az_get(wet) == 1 && az_get(mode) == 0 && az_get(bank) == 1);
    CHECK("row_value: a child of an OFF toggle reads 0 (own AND parents)", azf_row_value(wet) == 0);
    CHECK("row_set: a registered row", azf_row_set(fx, 1) == 0 && azf_row_value(fx) == 1 && azf_row_value(wet) == 1);
    CHECK("row_value: CHOICE = the index", azf_row_value(bank) == 1);
    CHECK("row_set: out of range refused", azf_row_set(bank, 3) < 0 && azf_row_set(fx, 2) < 0);
    CHECK("row_value: a bad handle is -1", azf_row_value(-5) == -1 && azf_row_value(AZI_COUNT) == -1 && azf_row_value(grp) == -1);
    int st = azf_row_lookup("stems");
    CHECK("row_lookup: an official row by key", st == AZF_STEMS);
    az_set(AZF_STEMS, 1);
    CHECK("row_lookup: its value readable", azf_row_value(st) == 1);
    CHECK("row_set: an official row is refused (read-only to a mod)", azf_row_set(st, 0) < 0 && az_on(AZF_STEMS));
    CHECK("row_lookup: hidden state rows, groups and unknown keys are not offered",
          azf_row_lookup("fps60-saved-ui") < 0 && azf_row_lookup("extras") < 0 && azf_row_lookup("nope") < 0);
    CHECK("row_lookup: a registered key is not an official one", azf_row_lookup("fx") < 0);
    azf_row_set(fx, 0);

    /* the menu: same path as the official rows */
    azmod_state s;
    azmod_state_init(&s);
    azmod_set_docked(true);
    s.menu_focused = 1;
    s.mode = AZ_OPEN;
    int tops[AZI_COUNT], nt = c_tops(tops), pos_fx = -1, pos_comm = -1, pos_them = -1;
    for (int k = 0; k < nt; k++) {
        if (tops[k] == fx) pos_fx = k;
        if (tops[k] == AZI_COMMUNITY) pos_comm = k;
        if (tops[k] == AZF_THEMES) pos_them = k;
    }
    CHECK("left list: the mod's top rows right under Community Mods (after every official row)",
          pos_fx > pos_them && pos_fx == pos_comm + 1 && tops[pos_fx + 1] == grp && tops[pos_fx + 2] == act &&
              tops[pos_fx + 3] == scr);
    CHECK("left list: a child is not a left entry", !c_tops(tops) || (({ int in = 0; for (int k = 0; k < nt; k++) in |= tops[k] == wet; in; }) == 0));
    CHECK("knob reaches FX Bus", select_left(&s, "FX Bus"));
    draw(&s);
    CHECK("FX Bus pane drawn: header, description, Enable, its options",
          seen("FX Bus") && seen("A test switch.") && seen("Enable") && seen("Wet Path") && seen("Mode") && seen("Bank"));
    CHECK("its CHOICE of 2 draws two buttons, of 3 three", seen("Dry") && seen("Wet") && seen("A") && seen("B") && seen("C"));
    CHECK("FX Bus has a left checkbox (a TOGGLE)", c_left_has_box(fx));
    /* parent off: the child is grayed and inert */
    CHECK("knob onto Wet Path", select_row(&s, wet));
    knob(&s, 0, true);
    CHECK("parent off: pressing the child changes nothing", az_get(wet) == 1 && !s.config_dirty);
    azmod_back(&s);
    CHECK("knob onto Enable", select_left(&s, "FX Bus") && select_row(&s, fx));
    knob(&s, 0, true);
    CHECK("Enable pressed: FX Bus on, the change marked for saving", az_on(fx) && s.config_dirty);
    CHECK("on_change waits for the save", g_changed_n == 0);
    az_fire_changes();
    CHECK("on_change after the save, with the value", g_changed_n == 1 && g_changed_v == 1);
    azmod_back(&s);
    CHECK("parent on: pressing the child toggles it", select_left(&s, "FX Bus") && select_row(&s, wet) &&
                                                          (knob(&s, 0, true), az_get(wet) == 0));
    azmod_back(&s);
    CHECK("CHOICE of 2: a press moves to the other option", select_left(&s, "FX Bus") && select_row(&s, mode) &&
                                                                (knob(&s, 0, true), az_get(mode) == 1));
    azmod_back(&s);
    /* clang-format off */
    CHECK("CHOICE of 3: a press cycles", select_left(&s, "FX Bus") && select_row(&s, bank) &&
          (knob(&s, 0, true), az_get(bank) == 2) && (knob(&s, 0, true), az_get(bank) == 0));
    /* clang-format on */
    azmod_back(&s);
    /* a GROUP: its members in its pane */
    CHECK("knob reaches Tools", select_left(&s, "Tools"));
    draw(&s);
    CHECK("Tools pane: its description and member", seen("A test group.") && seen("Tool A"));
    CHECK("a GROUP has no left checkbox without on_activate", !c_left_has_box(grp));
    CHECK("the GROUP member toggles", select_row(&s, toola) && (knob(&s, 0, true), az_on(toola)));
    azmod_back(&s);
    /* an ACTION: its button runs on_activate */
    CHECK("knob reaches Go", select_left(&s, "Go"));
    draw(&s);
    CHECK("Go pane: its button", seen("Go"));
    CHECK("the ACTION runs on_activate once", select_row(&s, act) && (knob(&s, 0, true), g_go == 1) && s.last_action == act);
    azmod_back(&s);
    /* a SCREEN: a left entry (its screen comes with register_screen) */
    CHECK("knob reaches Page", select_left(&s, "Page") && s.mode == AZ_OPEN);

    /* az_row_v1 -> az_feature_v2: a field copy */
    /* clang-format off */
    az_row_v1 r = {.size = sizeof r, .key = "v1", .label = "V1", .kind = AZ_ROW_CHOICE, .flags = 0,
                   .parent = "fx", .help = "h", .choices = MODE, .n_choices = 2, .default_choice = 1,
                   .available = NULL, .on_change = changed, .on_activate = go, .user = &r};
    /* clang-format on */
    az_feature_v2 g;
    CHECK("az_row_v1 -> az_feature_v2: every field copied",
          azf_from_row_v1(&r, &g) == 0 && g.size == sizeof g && g.key == r.key && g.label == r.label &&
              g.kind == r.kind && g.parent == r.parent && g.help == r.help && g.choices == r.choices &&
              g.n_choices == 2 && g.default_choice == 1 && g.on_change == changed && g.on_activate == go && g.user == &r);
    r.size = (int) offsetof(az_row_v1, parent);
    CHECK("az_row_v1 -> az_feature_v2: an older, shorter struct leaves the rest zero",
          azf_from_row_v1(&r, &g) == 0 && g.parent == NULL && g.choices == NULL && g.user == NULL && g.kind == AZ_ROW_CHOICE);

    /* row icons: copied at registration, drawn by the menu like an official row's */
    static unsigned char ic[AZ_ROW_ICON_N * AZ_ROW_ICON_N];
    memset(ic, 255, sizeof ic);
    az_feature_v2 fi = {.size = sizeof fi, .key = "ic", .label = "Icon", .kind = AZ_ROW_TOGGLE, .icon = ic};
    int ici = azf_register("iconmod", &fi);
    az_feature_v2 fn = {.size = sizeof fn, .key = "noic", .label = "No icon", .kind = AZ_ROW_TOGGLE};
    int nic = azf_register("iconmod", &fn);
    memset(ic, 0, sizeof ic); /* the mod's buffer changes afterwards: the menu keeps its copy */
    CHECK("a registered row's icon is drawn, from the core's copy (a full block)", ici >= 0 && icon_ink(ici) > 2000);
    CHECK("a registered row without an icon draws the placeholder (a square with a plus: some ink, far less than a full icon)",
          nic >= 0 && icon_ink(nic) > 100 && icon_ink(nic) < icon_ink(ici) / 2);

    /* a mod that is not running (init() failed after register_row): its rows leave the menu
     * and none of its callbacks is called again; its values stay */
    /* clang-format off */
    az_feature_v2 fd1 = {.size = sizeof fd1, .key = "dead", .label = "Dead Switch", .kind = AZ_ROW_TOGGLE,
                         .on_change = changed, .available = NULL};
    /* clang-format on */
    int dsw = azf_register("deadmod", &fd1);
    az_feature_v2 fd2 = {.size = sizeof fd2, .key = "dead-go", .label = "Dead Go", .kind = AZ_ROW_ACTION, .on_activate = go};
    int dgo = azf_register("deadmod", &fd2);
    azf_row_set(dsw, 1);
    CHECK("retire: the not-running mod's rows were listed", select_left(&s, "Dead Switch") && select_left(&s, "Dead Go"));
    CHECK("retire: both rows retired", azf_retire_owner("deadmod") == 2 && azf_retire_owner("deadmod") == 0);
    CHECK("retire: they leave the menu", az_hidden(dsw) && az_hidden(dgo) && !select_left(&s, "Dead Switch") && !select_left(&s, "Dead Go"));
    CHECK("retire: no callback is left to call", !az_row(dsw)->d.on_change && !az_row(dgo)->d.on_activate && !az_row(dsw)->d.available);
    CHECK("retire: the value is kept", azf_row_value(dsw) == 1);
    CHECK("retire: another mod's rows stay", !az_hidden(fx) && select_left(&s, "FX Bus"));

    /* the same mod turned on again (its init() registers the same rows): the rows come back
     * with their handles and values, without taking new slots */
    int dsw2 = azf_register("deadmod", &fd1);
    int dgo2 = azf_register("deadmod", &fd2);
    CHECK("revive: the same handles", dsw2 == dsw && dgo2 == dgo);
    CHECK("revive: listed again, callbacks back", !az_hidden(dsw) && select_left(&s, "Dead Switch") &&
                                                      az_row(dsw)->d.on_change == changed && az_row(dgo)->d.on_activate == go);
    CHECK("revive: the value kept through it", azf_row_value(dsw) == 1);
    CHECK("revive: a running row's key is still refused", azf_register("deadmod", &fd1) < 0);
    CHECK("revive: retired again and revived again, still the same handle",
          azf_retire_owner("deadmod") == 2 && azf_register("deadmod", &fd1) == dsw);
    return ut_summary();
}
