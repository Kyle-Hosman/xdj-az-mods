/* SPDX-License-Identifier: GPL-3.0-only */
/* The Mod Menu's settings at start-up and on a change (azmod.c load_settings / save_config,
 * extracted by run.sh) over the real menu.c (the feature table) and azconf.c:
 *   - first boot from 1.2.2's files: azmod.cfg migrates into azmod.conf, the old files are
 *     left exactly as they were;
 *   - the dual write: every save rewrites azmod.cfg in 1.2.2's format, byte for byte, from
 *     the table's legacy slots (a rollback to 1.2.2 reads current settings);
 *   - azmod.conf is the authority once it exists; a garbage one re-migrates from the
 *     (current) azmod.cfg; a NEWER version is read through azmod.cfg and never overwritten;
 *   - no file at all: the defaults, nothing written;
 *   - every write replaces the file whole (new inode, no .new left, a failed save keeps the
 *     old file);
 *   - a registered row's value and pin persist as "<mod>:<key>", and the key outlives the
 *     mod (reinstalling restores it);
 *   - ROLLBACK: an azmod.cfg whose bytes differ from the hash azmod.conf recorded was written
 *     by another build (1.2.2 after a rollback): its settings are migrated again over the
 *     conf, keys 1.2.2 does not know survive; an unchanged azmod.cfg changes nothing;
 *   - the conf's copy of azmod-btchannel is refreshed at every load;
 *   - the theme and the vinyl value are conf keys ("theme", "vinyl.brake"): saved, loaded,
 *     taken from the stick's file; a conf without them imports azmod-theme / -vinyl.v once;
 *   - the mods stick's azmods.cfg (handed over by the loader) wins over the deck copy, which it
 *     replaces; with none, the deck copy is used; one that does not load is refused (.bad)
 *     and neither file changes.
 * The line-1 rules of 1.2.2's file (widths, garbage) are the migration's: azconf's suite.
 * argv[1]: a scratch directory. */
#include "menu.h"
#include "azconf.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "check.h"

static char cfg_path[1024], conf_path[1024], settings_dir[1024], import_path[1024];
#define CONFIG_PATH cfg_path
#define CONF_PATH conf_path
#define IMPORT_PATH import_path
#define SETTINGS_DIR settings_dir
static int g_disp60_armed;
static char g_dbg[4096];
static void dbg(const char *fmt, ...) { /* azmod's log: kept for the checks */
    va_list ap;
    va_start(ap, fmt);
    size_t n = strlen(g_dbg);
    if (n < sizeof g_dbg - 1) vsnprintf(g_dbg + n, sizeof g_dbg - n, fmt, ap);
    va_end(ap);
}
static int ui60_clock_wanted(const azmod_state *s) {
    (void) s;
    return 0;
}
#include CFG_INC

static void put(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (content) fputs(content, f);
    fclose(f);
}
static char *slurp(const char *path) {
    static char buf[2][8192];
    static int k;
    char *b = buf[k ^= 1];
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(b, 1, sizeof buf[0] - 1, f) : 0;
    if (f) fclose(f);
    b[n] = 0;
    return b;
}
static void boot(azmod_state *s) { /* a fresh EP147: the early load, then the overlay's */
    g_settings_early = 0;
    load_settings_early();
    azmod_state_init(s);
    load_config(s);
}
static int line1_is(const char *want) {
    for (int i = 0; i < AZ_CFG_WIDTH; i++)
        if (az_cfg_char(i) != (az_cfg_retired(i) ? '0' : want[i])) return 0;
    return 1;
}
static ino_t ino(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 ? st.st_ino : 0;
}
static const char *subst(const char *src, const char *a, const char *b) { /* first a -> b */
    static char buf[2][8192];
    static int k;
    char *o = buf[k ^= 1];
    const char *p = strstr(src, a);
    if (!p) return snprintf(o, sizeof buf[0], "%s", src), o;
    snprintf(o, sizeof buf[0], "%.*s%s%s", (int) (p - src), src, b, p + strlen(a));
    return o;
}
static int has(const char *path, const char *needle) { return strstr(slurp(path), needle) != NULL; }

static const char *const MODE[] = {"slow", "fast", "off"};

int main(int argc, char **argv) {
    ut_suite = "settings";
    if (argc != 2) return 2;
    snprintf(settings_dir, sizeof settings_dir, "%s", argv[1]);
    mkdir(settings_dir, 0755);
    snprintf(cfg_path, sizeof cfg_path, "%s/azmod.cfg", settings_dir);
    snprintf(conf_path, sizeof conf_path, "%s/azmod.conf", settings_dir);
    snprintf(import_path, sizeof import_path, "%s/azmods-cfg.import", settings_dir);
    azmod_state s;
    const char *DECK_LINE = "100011110001011001110000100011000111111";
    char file[512];
    snprintf(file, sizeof file, "%s\n3 -1 27 0 1 -1 5 -1 12\n0\n", DECK_LINE);

    /* ---- first boot from 1.2.2's files */
    unlink(conf_path);
    put(cfg_path, file);
    ino_t i0 = ino(cfg_path);
    boot(&s);
    CHECK("1.2.2 azmod.cfg: every slot loads into its row", line1_is(DECK_LINE));
    CHECK("1.2.2 azmod.cfg: the drum bank is the first '1' of slots 7-10 (Bank 1)", az_get(AZF_DRUM_BANK) == 0);
    /* clang-format off */
    CHECK("1.2.2 azmod.cfg: line 2 pad colors", s.padmode_color[0] == 3 && s.padmode_color[1] == -1 &&
          s.padmode_color[2] == 27 && s.padmode_color[3] == 0 && s.padmode_color2[1] == 5 &&
          s.padmode_color2[3] == 12 && s.pad_colors_on);
    /* clang-format on */
    CHECK("migration wrote azmod.conf", has(conf_path, "stems = on"));
    CHECK("migration left azmod.cfg byte-identical and in place", !strcmp(slurp(cfg_path), file) && ino(cfg_path) == i0);
    CHECK("azmod.conf: retired slots have no key", !has(conf_path, "stem-tint"));

    /* ---- the dual write */
    save_config(&s);
    CHECK("save: azmod.cfg is 1.2.2's format, byte for byte", !strcmp(slurp(cfg_path), file));
    CHECK("save: azmod.cfg replaced whole (new inode)", ino(cfg_path) != i0);
    {
        char nw[1100];
        snprintf(nw, sizeof nw, "%s.new", cfg_path);
        CHECK("save: no .new left behind", access(nw, F_OK) != 0);
        mkdir(nw, 0755); /* the temp path is taken: the save cannot complete */
        az_set(AZF_STEMS, 0);
        save_config(&s);
        CHECK("a save that fails keeps the previous azmod.cfg intact", !strcmp(slurp(cfg_path), file));
        rmdir(nw);
        az_set(AZF_STEMS, 1);
    }
    az_set(AZF_DRUM_BANK, 2);
    save_config(&s);
    CHECK("save: the drum bank one-hot at slots 7-10", !strncmp(slurp(cfg_path) + 7, "0010", 4));
    CHECK("save: azmod.conf carries the bank as an index", has(conf_path, "drum-roll-bank = 2"));

    /* ---- azmod.conf is the authority; an unchanged azmod.cfg changes nothing */
    {
        char conf[4096], cfg[4096];
        char *p = slurp(conf_path), *q = strstr(p, "stem-volume-eq = off");
        CHECK("azmod.conf records the hash of the azmod.cfg it wrote", has(conf_path, "legacy.cfg-hash = "));
        if (q) memcpy(q, "stem-volume-eq = on ", 20); /* an edit of the conf alone (same length) */
        put(conf_path, p);
        snprintf(conf, sizeof conf, "%s", slurp(conf_path));
        snprintf(cfg, sizeof cfg, "%s", slurp(cfg_path));
        ino_t ic = ino(conf_path), ig = ino(cfg_path);
        g_dbg[0] = 0;
        boot(&s);
        CHECK("second boot: azmod.conf is the authority", q && az_on(AZF_STEM_EQ) && az_on(AZF_STEMS) && az_get(AZF_DRUM_BANK) == 2);
        CHECK("second boot: pad colors from azmod.conf", s.padmode_color[2] == 27 && s.padmode_color2[3] == 12);
        CHECK("unchanged azmod.cfg: no re-migration", !strstr(g_dbg, "rewritten by another build"));
        /* clang-format off */
        CHECK("unchanged azmod.cfg: neither file rewritten", ino(conf_path) == ic && ino(cfg_path) == ig &&
              !strcmp(slurp(conf_path), conf) && !strcmp(slurp(cfg_path), cfg));
        /* clang-format on */
    }

    /* ---- ROLLBACK: 1.2.2 rewrote azmod.cfg; keys it does not know survive */
    {
        az_feature_v2 a = {.size = sizeof a, .key = "glow", .label = "Glow", .kind = AZ_ROW_TOGGLE};
        int ha = register_row_internal("rbmod", &a);
        az_set(ha, 1);
        az_pin_toggle(ha);
        az_pin_toggle(AZF_STEMS);
        save_config(&s); /* this build: Stems on, Drum Roll off, a mod's row on and pinned */
        /* clang-format off */
        CHECK("rollback setup: the mod's key and pins saved", has(conf_path, "rbmod:glow = on") &&
              has(conf_path, "pinned = stems,rbmod:glow"));
        /* clang-format on */
        /* 1.2.2 runs, the DJ turns Drum Roll on with Bank 4 and Stems off, unpins Stems; it
         * rewrites azmod.cfg (its own format) and never touches azmod.conf */
        char line[64];
        snprintf(line, sizeof line, "%s", DECK_LINE);
        line[0] = '0';
        line[6] = '1';
        line[7] = '0';
        line[10] = '1';
        snprintf(file, sizeof file, "%s\n-1 -1 -1 -1 1 -1 -1 -1 -1\n0\n", line);
        put(cfg_path, file);
        azf_reset_registered();
        g_dbg[0] = 0;
        boot(&s);
        ha = register_row_internal("rbmod", &a);
        CHECK("rollback: azmod.cfg's settings win (Stems off, Drum Roll on, Bank 4)",
              !az_on(AZF_STEMS) && az_on(AZF_DRUM_ROLL) && az_get(AZF_DRUM_BANK) == 3);
        CHECK("rollback: azmod.cfg's pad colors win", s.padmode_color[2] == -1 && s.padmode_color2[3] == -1);
        CHECK("rollback: a key 1.2.2 does not know survives (a mod's row)", az_get(ha) == 1 && has(conf_path, "rbmod:glow = on"));
        CHECK("rollback: 1.2.2's pins (none) plus the pins it cannot hold", az_pinned(ha) && !az_pinned(AZF_STEMS));
        CHECK("rollback: logged", strstr(g_dbg, "rewritten by another build") != NULL);
        { /* the re-migrated conf is what a save writes: a 2-way CHOICE as its index, not on/off */
            char *re = strdup(slurp(conf_path));
            save_config(&s);
            CHECK("rollback: a save right after the re-migration leaves azmod.conf byte-identical",
                  re && !strcmp(slurp(conf_path), re));
            free(re);
        }
        CHECK("rollback: azmod.conf saved with the new hash: the next start does not re-migrate",
              (g_dbg[0] = 0, boot(&s), !strstr(g_dbg, "rewritten by another build")));
        azf_reset_registered();
        snprintf(file, sizeof file, "%s\n3 -1 27 0 1 -1 5 -1 12\n0\n", DECK_LINE);
    }

    /* ---- azmod-btchannel's copy is refreshed at every load (it stays a per-deck file) */
    {
        char p[1100];
        snprintf(p, sizeof p, "%s/azmod-btchannel", settings_dir);
        put(p, "3\n");
        boot(&s);
        CHECK("bluetooth.channel copied at load", has(conf_path, "bluetooth.channel = 3"));
        unlink(p);
        boot(&s);
        CHECK("a removed file's copy is emptied, never stale", has(conf_path, "bluetooth.channel = \n") || has(conf_path, "bluetooth.channel =\n"));
    }

    /* ---- the theme and the vinyl value are azmod.conf keys: saved, loaded, follow the stick */
    {
        char p[1100];
        __atomic_store_n(&g_azmod_theme, 3, __ATOMIC_RELAXED); /* ember */
        g_vb_milli = 420;
        save_config(&s);
        CHECK("theme / vinyl.brake saved in azmod.conf", has(conf_path, "theme = ember") && has(conf_path, "vinyl.brake = 0.42"));
        snprintf(p, sizeof p, "%s/azmod-theme", settings_dir);
        CHECK("theme: azmod-theme not written", access(p, F_OK) != 0);
        snprintf(p, sizeof p, "%s/azmod-vinyl.v", settings_dir);
        CHECK("vinyl: azmod-vinyl.v not written", access(p, F_OK) != 0);
        __atomic_store_n(&g_azmod_theme, 0, __ATOMIC_RELAXED);
        g_vb_milli = -1;
        boot(&s);
        CHECK("theme / vinyl.brake loaded from azmod.conf", g_azmod_theme == 3 && g_vb_milli == 420);
        char im[8192];
        const char *t = subst(slurp(conf_path), "theme = ember", "theme = forest");
        snprintf(im, sizeof im, "%s", subst(t, "vinyl.brake = 0.42", "vinyl.brake = 0.30"));
        put(import_path, im);
        boot(&s);
        CHECK("the stick's theme / vinyl.brake win (they follow the stick)", g_azmod_theme == 4 && g_vb_milli == 300);
        CHECK("the stick's theme / vinyl.brake kept in the deck copy", has(conf_path, "theme = forest") && has(conf_path, "vinyl.brake = 0.30"));
    }

    /* ---- Remember Waveform Zoom's level is an azmod.conf key: none until one is saved */
    {
        char *keep = strdup(slurp(conf_path));
        __atomic_store_n(&g_wz_saved, WZ_NONE, __ATOMIC_RELAXED);
        save_config(&s);
        CHECK("wave.zoom: no key while no level was ever saved", !has(conf_path, "wave.zoom"));
        __atomic_store_n(&g_wz_saved, WZ_PACK(1, 16), __ATOMIC_RELAXED);
        save_config(&s);
        CHECK("wave.zoom: a saved level is written as its text", has(conf_path, "wave.zoom = 1/16\n"));
        __atomic_store_n(&g_wz_saved, WZ_NONE, __ATOMIC_RELAXED);
        boot(&s);
        CHECK("wave.zoom: loaded from azmod.conf", __atomic_load_n(&g_wz_saved, __ATOMIC_RELAXED) == WZ_PACK(1, 16));
        put(conf_path, subst(slurp(conf_path), "wave.zoom = 1/16", "wave.zoom = 2/1"));
        boot(&s);
        CHECK("wave.zoom: 2/1 loaded", __atomic_load_n(&g_wz_saved, __ATOMIC_RELAXED) == WZ_PACK(2, 1));
        put(conf_path, subst(slurp(conf_path), "wave.zoom = 2/1", "wave.zoom = 1/32"));
        boot(&s);
        CHECK("wave.zoom: a value that is not a level loads as none",
              __atomic_load_n(&g_wz_saved, __ATOMIC_RELAXED) == WZ_NONE);
        put(conf_path, keep);
        free(keep);
        boot(&s);
    }

    /* ---- migration: a conf from before the move imports the old files once, then ignores them */
    {
        char p[1100], *keep = strdup(slurp(conf_path));
        put(conf_path, "# azmod.conf\nversion = 1\nstems = on\n");
        snprintf(p, sizeof p, "%s/azmod-theme", settings_dir);
        put(p, "midnight\n");
        snprintf(p, sizeof p, "%s/azmod-vinyl.v", settings_dir);
        put(p, "0.25\n");
        boot(&s);
        CHECK("migration: azmod-theme / azmod-vinyl.v applied", g_azmod_theme == 2 && g_vb_milli == 250);
        CHECK("migration: azmod-theme / azmod-vinyl.v imported into the conf", has(conf_path, "theme = midnight") && has(conf_path, "vinyl.brake = 0.25"));
        snprintf(p, sizeof p, "%s/azmod-theme", settings_dir);
        put(p, "ember\n");
        boot(&s);
        CHECK("migration: once only (a later azmod-theme is not read)", g_azmod_theme == 2 && has(conf_path, "theme = midnight"));
        unlink(p);
        snprintf(p, sizeof p, "%s/azmod-vinyl.v", settings_dir);
        unlink(p);
        put(conf_path, keep);
        free(keep);
        boot(&s);
    }

    /* ---- a garbage azmod.conf re-migrates from azmod.cfg */
    put(cfg_path, file);
    put(conf_path, "\x01\x02 not a conf\n");
    boot(&s);
    CHECK("garbage azmod.conf: re-migrated from azmod.cfg", line1_is(DECK_LINE) && az_get(AZF_DRUM_BANK) == 0);
    CHECK("garbage azmod.conf: rewritten valid", has(conf_path, "version = 1"));

    /* ---- a newer build's azmod.conf: read through azmod.cfg, never overwritten */
    put(conf_path, "# azmod.conf\nversion = 2\nstems = off\nnew-thing = 7\n");
    char before[2048];
    snprintf(before, sizeof before, "%s", slurp(conf_path));
    boot(&s);
    CHECK("newer azmod.conf: settings come from azmod.cfg", line1_is(DECK_LINE));
    az_toggle(AZF_PHASE_METER);
    save_config(&s);
    CHECK("newer azmod.conf: never overwritten, not at load, not at a save", !strcmp(slurp(conf_path), before));
    CHECK("newer azmod.conf: azmod.cfg still written", slurp(cfg_path)[36] == '0');

    /* ---- nothing at all: the defaults, nothing written */
    unlink(conf_path);
    unlink(cfg_path);
    boot(&s);
    CHECK("no files: defaults (Remote Stems and Stem Marks on, the rest off)",
          az_on(AZF_REMOTE_STEMS) && az_on(AZF_STEM_MARKS) && !az_on(AZF_STEMS) && az_get(AZF_DRUM_BANK) == 0);
    CHECK("no files: nothing written", access(conf_path, F_OK) != 0 && access(cfg_path, F_OK) != 0);

    /* ---- a registered row persists as <mod>:<key>, and its key outlives the mod */
    put(cfg_path, file);
    boot(&s);
    az_feature_v2 a = {.size = sizeof a, .key = "glow", .label = "Glow", .kind = AZ_ROW_TOGGLE};
    /* clang-format off */
    az_feature_v2 b = {.size = sizeof b, .key = "speed", .label = "Speed", .kind = AZ_ROW_CHOICE,
                       .choices = MODE, .n_choices = 3, .default_choice = 2};
    /* clang-format on */
    int ha = register_row_internal("glowmod", &a), hb = register_row_internal("glowmod", &b);
    CHECK("registered rows: handles after the official rows", ha == AZI_REG0 && hb == ha + 1);
    CHECK("registered rows: defaults (off, choice 2)", az_get(ha) == 0 && az_get(hb) == 2);
    az_set(ha, 1);
    az_set(hb, 1);
    az_pin_toggle(hb);
    save_config(&s);
    CHECK("registered rows saved as <mod>:<key>", has(conf_path, "glowmod:glow = on") && has(conf_path, "glowmod:speed = 1"));
    CHECK("registered pin saved by its key", has(conf_path, "pinned = glowmod:speed") || has(conf_path, ",glowmod:speed"));
    CHECK("the dual-written azmod.cfg has no trace of them", !strcmp(slurp(cfg_path), file));
    /* the mod is removed: the next start registers nothing; a save keeps its keys */
    azf_reset_registered();
    boot(&s);
    save_config(&s);
    CHECK("removed mod: its keys are kept in azmod.conf", has(conf_path, "glowmod:glow = on"));
    /* reinstalled: the DJ's values come back */
    azf_reset_registered();
    boot(&s);
    ha = register_row_internal("glowmod", &a);
    hb = register_row_internal("glowmod", &b);
    CHECK("reinstalled mod: its values and pin restored", az_get(ha) == 1 && az_get(hb) == 1 && az_pinned(hb));

    /* ---- the mods stick's azmods.cfg: the stick wins, no stick uses the copy, junk refused */
    {
        char bad[1100], deck[8192];
        snprintf(bad, sizeof bad, "%s.bad", import_path);
        az_set(AZF_STEMS, 1);
        save_config(&s);
        unlink(import_path);
        boot(&s);
        CHECK("no stick file: the deck copy is used", az_on(AZF_STEMS));
        put(import_path, "# azmods.cfg\nversion = 1\nstems = off\nglowmod:glow = off\ncommunity-mods.glowmod = on\n");
        azf_reset_registered();
        boot(&s);
        ha = register_row_internal("glowmod", &a);
        CHECK("the stick's file wins over the deck copy", !az_on(AZF_STEMS) && az_get(ha) == 0);
        CHECK("the stick's file replaces the deck copy (keys azmod does not own kept)",
              has(conf_path, "stems = off") && has(conf_path, "community-mods.glowmod = on"));
        CHECK("the hand-over is consumed", access(import_path, F_OK) != 0 && access(bad, F_OK) != 0);
        boot(&s);
        CHECK("the next start reads the imported settings from the deck copy", !az_on(AZF_STEMS));
        snprintf(deck, sizeof deck, "%s", slurp(conf_path));
        put(import_path, "stems = on\n"); /* no version line: not a settings file */
        boot(&s);
        CHECK("a malformed stick file is refused: the deck copy's settings stay", !az_on(AZF_STEMS));
        CHECK("a malformed stick file is refused: the deck copy is untouched", !strcmp(slurp(conf_path), deck));
        CHECK("a malformed stick file is kept as .bad, not deleted", access(bad, F_OK) == 0 && has(bad, "stems = on"));
        unlink(bad);
    }
    return ut_summary();
}
