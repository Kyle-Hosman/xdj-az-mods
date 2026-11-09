/* SPDX-License-Identifier: GPL-3.0-only */
/* screen_community.c: Community Mods: the catalog and status files, the pages, the slide, the draw services a mod's page paints through.
 * Part of the Mod Menu's one translation unit: #included by menu.c, in the order listed there. */
/* ==== Community (Mod Menu > Community Mods) ============================================
 * Home: the master switch and the trust note in the header; the firmware tag, then three
 * tabs: Installed (the quarantine notice, then azcm_info rows), Browse (Home: shelves of tiles; All
 * mods or a category: the catalog as a grid of tiles in one of four orders: cm_browse.h) and Repositories (+ Add repository, Refresh). Pages: an installed mod (state, Turn back on, its
 * own screen), a catalog mod (Install / Update / Remove / Enable / Disable, progress), the
 * trust screen (first install, once), the Add-repository keyboard.
 * Reads: community.c through azcm_* (menu.h) and the fetcher's line files
 * (/tmp/az-community/catalog, /status). Writes: only /run/az-community.req (tmp + rename) and
 * the trust flag. Never the network, never a fork. Layout and hit-testing share one function
 * (cm_layout), so what is drawn is what is hit. Icons: azcm_icon (community_icons.c,
 * decoded by EP147's own JUCE PNG decoder); until one is ready a tile shows the first letter. */
#include <sys/stat.h>
#include "scroll_glide.h"
#include "cm_browse.h"
#include "cm_updall.h"
/* every control of one page: up to CM_NCAT tiles, or CM_NREPO repository rows with a Remove each,
 * plus the tabs and buttons; a control past this would be drawn but could not be touched */
#define CM_MAXCTL 256
#define CM_BAD 0x00FF4040u   /* failed / suspended: the menu's warning red */
#ifndef CM_CATALOG           /* the host tests point it at a fixture */
#define CM_CATALOG "/tmp/az-community/catalog"
#endif
#ifndef CM_STATUS            /* the host tests point it at a fixture */
#define CM_STATUS "/tmp/az-community/status"
#endif
#ifndef CM_NOW   /* the deck's clock, seconds since 1970 (a mod page's "updated N days ago"); the host tests pin it */
#define CM_NOW() ((long long) time(NULL))
#endif
#ifndef CM_REQ   /* the host tests point it at a temp file */
#define CM_REQ "/run/az-community.req"
#endif
#define CM_TRUSTED "/home/root/settings/azmod-community/.trusted"   /* confirmed once */
enum {
    CMC_RESUME = 1,
    CMC_KEEPOFF,
    CMC_MOD,
    CMC_UNSUSPEND,
    CMC_TAB,
    CMC_TILE,
    CMC_REFRESH,
    CMC_ADDREPO,
    CMC_DELREPO,
    CMC_ACT,
    CMC_APPLY,
    CMC_TRUST_OK,
    CMC_TRUST_NO,
    CMC_KEY,
    CMC_KB_OK,
    CMC_KB_NO,
    CMC_BACK,
    CMC_MODACT,
    CMC_INSTCAT,
    CMC_RELOAD_OK,
    CMC_RELOAD_NO,
    CMC_MODUPD,
    CMC_ALLOW,
    CMC_SORT,
    CMC_FCAT,
    CMC_STILE,   /* a tile on a Home shelf: idx = shelf kind << 8 | catalog index */
    CMC_SEEALL,  /* a shelf's "See all": idx = shelf kind */
    CMC_SORTBTN, /* Browse: the Sort button, opens the orders */
    CMC_SORTOFF, /* while the orders are open: everywhere else, closes them */
    CMC_SHOT,    /* a mod page's screenshot: idx = which; opens it large */
    CMC_SHOTZ,   /* the large screenshot: idx 0 closes it, 1 the previous, 2 the next */
    CMC_UPDALL,  /* Updates: Update all */
    CMC_UPDROW,  /* Updates: a row's Update: idx = catalog index */
    CMC_CHK      /* Installed: a row's checkbox (cm_chk) */
};
enum {
    CMA_INSTALL = 1,
    CMA_UPDATE,
    CMA_REMOVE,
    CMA_ENABLE,
    CMA_DISABLE
};
enum {
    CMV_HOME = 0,
    CMV_MOD,
    CMV_CAT,
    CMV_TRUST,
    CMV_KBD,
    CMV_RELOAD
};
typedef struct {
    int kind, idx, x, y, w, h;
} cm_ctl;
/* sized to the fetcher's caps (repo 256, firmware list up to 32 tags), so nothing is cut */
typedef struct {
    char repo[260], name[48], err[112];
    int ok, listed;
} cm_crepo;
/* a mod's screenshots: at most CM_NSHOT, each a path the fetcher names under CM_SHOTDIR
 * (/tmp/az-community/shots/<16 hex>.png) */
#define CM_NSHOT 3
#define CM_SHOTDIR "/tmp/az-community/shots/"
#define CM_SHOTPATH 48
/* held: the id is installed from another repository (its short name) or copied by hand ("-");
 * also/nalso: the first other repository offering the same id, and how many do (the fetcher shows
 * one line per id: an id is one folder, one settings file and one data folder on the deck) */
typedef struct {
    char repo[260], id[33], name[48], ver[24], author[48], summary[120], cat[24], fw[800], icon[160], inst[24], mc[16],
        desc[600], held[128], also[128], upd[12];
    int hooks, assets_kb, nalso;
    int counted;   /* the Mod Index has a download count for it (catalog field 21) */
    long long dl;  /* that count */
    long size;
    int beta;                          /* the author marks it as not proven stable yet (field 22) */
    int nshots;                        /* its screenshots: where the fetcher's `shots` writes them (field 23) */
    char shots[CM_NSHOT][CM_SHOTPATH];
} cm_cmod;
/* CM_NREPO: every repository the fetcher can list (64 from the Mod Index + 32 the user
 * added). CM_NCAT: the catalog's mod lines (the fetcher's CAT_MODS); a cm_cmod is ~2.2 KB, so
 * 128 of them are ~280 KB. Mods past it are counted (the fetcher's closing more|<n> line, plus
 * any line past the array) and the Browse tab says it shows only the first ones. */
#define CM_NREPO 96
#define CM_NCAT 128
static cm_crepo g_cr[CM_NREPO];
static int g_ncr, g_cr_more;
static cm_cmod g_cc[CM_NCAT];
static int g_ncc, g_cc_more;
static struct {
    char state[16], op[64], id[33], msg[112];
    long done, total;
} g_cs;
static int g_cm_view, g_cm_page = -1, g_cm_cat = -1, g_cm_tab;
static int g_cm_sel;                /* knob focus: index into the current page's controls */
static int g_cm_knob;               /* the knob moved focus last: show it (a touch hides it) */
static int g_cm_hfoc = -1;          /* the Installed / Updates tab: the knob's control (cm_code), or -1 */
static int g_cm_press = -1;         /* the control under a finger: lit from press to lift */
static int g_cm_scroll;             /* px the home page is scrolled (a drag or the knob) */
static int g_cm_smax;               /* how far the home page can scroll, from its last layout */
static struct glide g_cm_gl;        /* the home page's scroll in motion (scroll_glide.h): g_cm_scroll is what it shows */
static int g_cm_glpx;               /* the g_cm_scroll it last wrote; any other value was set from outside and stops it */
static int g_cm_nbody;              /* the last layout's controls before the header's (back square, Reload now) */
static int g_cm_resumed, g_cm_keptoff, g_cm_pending_off = -1, g_cm_unsusp[16];
static int g_cm_reload;             /* a request changed what loads: offer Reload */
static int g_cm_reloading;          /* Reload was pressed: the restart is coming (an error for it clears this) */
static int g_cm_allow_err = -1;     /* azcm index whose Allow could not be saved on the deck, or -1 */
static int g_cm_fresh;              /* the fetcher's status changed during this start (older lines are not shown) */
static unsigned g_cm_gen;           /* bumps when the catalog or the status file is re-read */
static unsigned g_cm_cgen, g_cm_sgen;   /* bump when the catalog / the status file is re-read */
static cmu_run g_cm_ua;             /* Updates: the Update all run (cm_updall.h) */
static int g_cm_zoom = -1;          /* the mod page's screenshot shown large, or -1 */
/* The Installed tab is split as the Mod Menu is: the list on the left, the selected mod's page on
 * the right. g_cm_isel: the selected row (CMC_MOD or CMC_INSTCAT << 16 | idx; -1 = the first);
 * g_cm_in: the knob is in the right half (a press on a row went in, BACK comes out), where it
 * drives the mod's own screen, or steps through the page's buttons (g_cm_rsel, a control index)
 * when the mod has none. g_cm_ient: the row whose screen was last entered (azcm_screen_enter). */
static int g_cm_isel = -1, g_cm_in, g_cm_rsel = -1, g_cm_ient = -1;
/* the open mod page's screenshots: which are on disk, checked when the page or g_cm_gen changes
 * (never per frame), and whether `shots` was asked for them on this visit */
static struct {
    int cat, asked, have[CM_NSHOT];
    unsigned gen, askgen; /* askgen: g_cm_gen when `shots` was sent (no answer yet while it holds) */
} g_cm_sh = {-1, 0, {0}, 0, 0};
static int g_cm_home_sel = -1, g_cm_home_scroll;   /* home focus, restored when a page goes back */
static int g_cm_reload_from;        /* the view the Reload confirm returns to */
static int g_cm_row = -1;           /* the main list's SCREEN row that opened this page (cm_open_row), or -1 */
static int g_cm_row_opening;        /* cm_open_row is firing CMC_MOD: keep the row's screen */
static int g_cm_row_pane, g_cm_row_sel;   /* where that row was: BACK returns there */
/* A destructive button (Remove) asks first, in the core's yes/no dialog (azcm_confirm: Cancel
 * focused, BACK = no). The request is captured when asked and sent only on yes. One question at a
 * time: while one is open a second press does nothing, so it can never retarget the first. */
static struct {
    int open;
    const char *fmt;
    char arg[260];
} g_cm_ask;
static void cm_req(const char *fmt, const char *a, const char *b);
static int cm_busy(void);
static void cm_ask_done(int yes, void *user) {
    (void) user;
    g_cm_ask.open = 0;
    if (yes && !cm_busy())
        cm_req(g_cm_ask.fmt, g_cm_ask.arg, 0);
}
static void cm_ask(const char *title, const char *text, const char *fmt, const char *arg) {
    if (g_cm_ask.open)
        return;
    g_cm_ask.fmt = fmt;
    snprintf(g_cm_ask.arg, sizeof g_cm_ask.arg, "%s", arg);
    g_cm_ask.open = azcm_confirm(title, text, "Remove", cm_ask_done, 0) == 0;
}
/* mods installed or updated this start: they load only after a Reload, never live */
static char g_cm_fresh_ids[8][33];
static int cm_fresh_install(const char *id) {
    for (int k = 0; k < 8; k++)
        if (!strcmp(g_cm_fresh_ids[k], id)) return 1;
    return 0;
}
static void cm_fresh_add(const char *id) {
    if (cm_fresh_install(id)) return;
    for (int k = 0; k < 8; k++)
        if (!g_cm_fresh_ids[k][0]) {
            snprintf(g_cm_fresh_ids[k], sizeof g_cm_fresh_ids[k], "%s", id);
            return;
        }
}
/* mods removed this start (the button and the Installed list say so until the reload) */
static char g_cm_removed[8][33];
static void cm_removed_add(const char *id) {
    int k = 0;
    for (; k < 8 && g_cm_removed[k][0] && strcmp(g_cm_removed[k], id); k++) {}
    if (k == 8) k = 7;
    snprintf(g_cm_removed[k], sizeof g_cm_removed[k], "%s", id);
}
static int cm_removed(const char *id) {
    for (int k = 0; k < 8 && g_cm_removed[k][0]; k++)
        if (!strcmp(g_cm_removed[k], id)) return 1;
    return 0;
}
/* enable/disable requests the fetcher finished this start: the button shows the pending state
 * (what loads after the reload), not the state the core loaded at start */
static struct {
    char id[33];
    int on;
} g_cm_pend[8];
static void cm_pend_set(const char *id, int on) {
    int k = 0;
    for (; k < 8 && g_cm_pend[k].id[0] && strcmp(g_cm_pend[k].id, id); k++) {}
    if (k == 8) k = 7;
    snprintf(g_cm_pend[k].id, sizeof g_cm_pend[k].id, "%s", id);
    g_cm_pend[k].on = on;
}
static int cm_pend_get(const char *id) { /* 1 on, 0 off, -1 nothing pending */
    for (int k = 0; k < 8 && g_cm_pend[k].id[0]; k++)
        if (!strcmp(g_cm_pend[k].id, id)) return g_cm_pend[k].on;
    return -1;
}
static void cm_pend_drop(const char *id) {   /* back to "nothing pending" (the entries stay packed) */
    for (int k = 0; k < 8 && g_cm_pend[k].id[0]; k++)
        if (!strcmp(g_cm_pend[k].id, id)) {
            for (; k < 7; k++) g_cm_pend[k] = g_cm_pend[k + 1];
            g_cm_pend[7].id[0] = 0;
            return;
        }
}
/* An Installed checkbox shows its new state at the tap: the fetcher takes a second or two to write
 * the USB, and its answer confirms it (done) or puts the old state back (error). Until that answer
 * (or CM_FLIP_MS), no other toggle is sent: the fetcher runs one request at a time and drops one
 * that comes while it works, which would leave a box showing a change nobody made. */
#define CM_FLIP_MS 5000
static struct {
    char id[33];
    int prev;   /* cm_pend_get before the tap */
    long at;    /* 0 = none in flight */
} g_cm_flip;
static char g_cm_kb[96];
static int g_cm_kbnum;
/* Browse: the view shown (CMB_HOME, CMB_ALL or a cm_browse.h category) and the order; for this
 * EP147 run. */
static int g_cm_fcat = CMB_HOME, g_cm_sort = CMB_SORT_NAME;
static int g_cm_sortpop;   /* Browse: the orders are open under the Sort button */
static long cm_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}
/* the glide follows g_cm_scroll when anything else set it (a reset, a tab, a page restored) */
static void cm_glide_sync(void) {
    if (g_cm_scroll == g_cm_glpx) return;
    glide_set(&g_cm_gl, (float) g_cm_scroll);
    g_cm_glpx = g_cm_scroll;
}
/* one frame of the glide; true while it moves */
static bool cm_glide_frame(void) {
    cm_glide_sync();
    if (!g_cm_gl.on) return false;
    glide_step(&g_cm_gl, cm_ms(), (float) g_cm_smax);
    g_cm_scroll = g_cm_glpx = glide_px(&g_cm_gl);
    return true;
}
/* ---- the fetcher's files ---------------------------------------------------------- */
static int cm_split(char *line, char **f, int max) {   /* '|' fields, in place */
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < max; p++)
        if (*p == '|') {
            *p = 0;
            f[n++] = p + 1;
        }
    return n;
}
static void cm_cpy(char *d, size_t n, const char *s) { snprintf(d, n, "%s", s ? s : ""); }
/* catalog field 23: space-separated screenshot paths. Only CM_SHOTDIR<hex>.png is taken, at
 * most CM_NSHOT of them; returns how many. */
static int cm_shot_paths(const char *v, char out[CM_NSHOT][CM_SHOTPATH]) {
    int k = 0;
    const size_t dl = sizeof CM_SHOTDIR - 1;
    while (v && *v && k < CM_NSHOT) {
        while (*v == ' ') v++;
        size_t l = strcspn(v, " ");
        if (!l) break;
        int ok = l > dl + 4 && l < CM_SHOTPATH && !strncmp(v, CM_SHOTDIR, dl) && !strncmp(v + l - 4, ".png", 4);
        for (size_t i = dl; ok && i < l - 4; i++) ok = (v[i] >= '0' && v[i] <= '9') || (v[i] >= 'a' && v[i] <= 'f');
        if (ok) {
            memcpy(out[k], v, l);
            out[k++][l] = 0;
        }
        v += l;
    }
    return k;
}
/* a mod's repository is in the Mod Index. A repository the catalog does not name counts as
 * not listed: the note it earns is the cautious answer. */
static int cm_repo_listed(const char *repo) {
    for (int i = 0; i < g_ncr; i++)
        if (!strcmp(g_cr[i].repo, repo)) return g_cr[i].listed;
    return 0;
}
#define CM_TRUST_NOTE "Made by other developers, not checked by AZ-MODS." /* the Installed tab's header */
/* between a mod page's author, category and version: a middle dot, as one byte (the menu font
 * draws one glyph per byte, Latin-1, so a UTF-8 dot would draw as two) */
#define CM_DOT "  \xb7  "
#define CM_UNLISTED "Not in the Mod Index"   /* the wording the SDK and index docs promise */
#define CM_HELD_FROM "A different mod with this ID is installed (from %s). Remove it first."
#define CM_HELD_HAND "A different mod with this ID is on the USB (copied there by hand). Remove it first."

static void cm_load(void) {   /* re-read catalog + status when they changed; at most 4x a second */
    static long last;
    static time_t cmt;
    static off_t csz;
    static time_t smt;
    static off_t ssz;
    static ino_t cino, sino;   /* the fetcher replaces each file (tmp + mv): a new inode is a new file */
    static int seen;           /* the status file as found at start belongs to an earlier start */
    long now = cm_ms();
    if (now - last < 250) return;
    last = now;
    struct stat st;
    if (stat(CM_CATALOG, &st) == 0) {
        if (st.st_mtime != cmt || st.st_size != csz || st.st_ino != cino) {
            cmt = st.st_mtime;
            csz = st.st_size;
            cino = st.st_ino;
            g_cm_gen++;
            g_cm_cgen++;
            g_ncr = g_ncc = g_cr_more = g_cc_more = 0;
            FILE *f = fopen(CM_CATALOG, "re");
            static char line[4096];
            int v1 = 0;
            while (f && fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = 0;
                if (!v1) {
                    v1 = !strcmp(line, "v1");
                    if (!v1) break;
                    continue;
                }   /* unknown version: show nothing */
                char *fl[32];   /* room past the fields read here, so a later one never runs into the last */
                int n = cm_split(line, fl, 32);
                if (n >= 6 && !strcmp(fl[0], "repo") && g_ncr >= CM_NREPO) g_cr_more++;
                else if (n >= 13 && !strcmp(fl[0], "mod") && g_ncc >= CM_NCAT) g_cc_more++;
                else if (n == 2 && !strcmp(fl[0], "more")) {
                    long k = strtol(fl[1], NULL, 10);
                    if (k > 0) g_cc_more += k > 100000 ? 100000 : (int) k;
                } else if (n >= 6 && !strcmp(fl[0], "repo")) {
                    cm_crepo *r = &g_cr[g_ncr++];
                    cm_cpy(r->repo, sizeof r->repo, fl[1]);
                    cm_cpy(r->name, sizeof r->name, fl[2]);
                    r->ok = atoi(fl[3]);
                    r->listed = atoi(fl[4]);
                    cm_cpy(r->err, sizeof r->err, fl[5]);
                } else if (n >= 13 && !strcmp(fl[0], "mod")) {
                    cm_cmod *m = &g_cc[g_ncc++];
                    cm_cpy(m->repo, sizeof m->repo, fl[1]);
                    cm_cpy(m->id, sizeof m->id, fl[2]);
                    cm_cpy(m->name, sizeof m->name, fl[3]);
                    cm_cpy(m->ver, sizeof m->ver, fl[4]);
                    cm_cpy(m->author, sizeof m->author, fl[5]);
                    cm_cpy(m->summary, sizeof m->summary, fl[6]);
                    cm_cpy(m->cat, sizeof m->cat, fl[7]);
                    m->hooks = atoi(fl[8]);
                    cm_cpy(m->fw, sizeof m->fw, fl[9]);
                    m->size = atol(fl[10]);
                    cm_cpy(m->icon, sizeof m->icon, fl[11]);
                    cm_cpy(m->inst, sizeof m->inst, fl[12]);
                    cm_cpy(m->mc, sizeof m->mc, n >= 14 ? fl[13] : ""); /* min_core (a fetcher without it: "") */
                    cm_cpy(m->desc, sizeof m->desc, n >= 15 ? fl[14] : "");
                    m->assets_kb = n >= 16 ? atoi(fl[15]) : 0;
                    cm_cpy(m->held, sizeof m->held, n >= 17 ? fl[16] : "");
                    cm_cpy(m->also, sizeof m->also, n >= 18 ? fl[17] : "");
                    m->nalso = n >= 19 ? atoi(fl[18]) : 0;
                    cm_cpy(m->upd, sizeof m->upd, n >= 20 && cmb_date_ok(fl[19]) ? fl[19] : ""); /* updated (a fetcher without it: "") */
                    m->counted = n >= 21 && cmb_dl_parse(fl[20], &m->dl); /* downloads (none, or not digits: no count) */
                    if (!m->counted) m->dl = 0;
                    m->beta = n >= 22 && !strcmp(fl[21], "1");
                    m->nshots = n >= 23 ? cm_shot_paths(fl[22], m->shots) : 0;
                }
            }
            if (f) fclose(f);
        }
    } else {
        g_ncr = g_ncc = g_cr_more = g_cc_more = 0;
        cmt = 0;
        csz = 0;
    }
    if (stat(CM_STATUS, &st) == 0) {
        if (st.st_mtime != smt || st.st_size != ssz || st.st_ino != sino) {
            smt = st.st_mtime;
            ssz = st.st_size;
            sino = st.st_ino;
            g_cm_gen++;
            g_cm_sgen++;
            memset(&g_cs, 0, sizeof g_cs);
            FILE *f = fopen(CM_STATUS, "re");
            char line[256];
            while (f && fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = 0;
                char *eq = strchr(line, '=');
                if (!eq) continue;
                *eq = 0;
                const char *v = eq + 1;
                if (!strcmp(line, "state")) cm_cpy(g_cs.state, sizeof g_cs.state, v);
                else if (!strcmp(line, "op")) cm_cpy(g_cs.op, sizeof g_cs.op, v);
                else if (!strcmp(line, "id")) cm_cpy(g_cs.id, sizeof g_cs.id, v);
                else if (!strcmp(line, "msg")) cm_cpy(g_cs.msg, sizeof g_cs.msg, v);
                else if (!strcmp(line, "done")) g_cs.done = atol(v);
                else if (!strcmp(line, "total")) g_cs.total = atol(v);
            }
            if (f) fclose(f);
            /* offer Reload only once a request has changed what loads: a refused install
             * (sha256 mismatch, no network, ...) leaves the stick as it was */
            if (!strcmp(g_cs.state, "done") && (!strcmp(g_cs.op, "install") || !strcmp(g_cs.op, "remove"))) g_cm_reload = 1;
            if (!strcmp(g_cs.state, "done") && g_cs.id[0] && (!strcmp(g_cs.op, "enable") || !strcmp(g_cs.op, "disable"))) {
                int on = !strcmp(g_cs.op, "enable");
                /* a mod with stop() changes now; the row then shows what it does, and the
                 * fetcher's "turns on after Reload" line is not shown */
                /* never for the status an earlier start left, nor for a mod installed or updated
                 * this start (the stage still holds what loaded at the start) */
                if (seen && !cm_fresh_install(g_cs.id) && azcm_live_set(g_cs.id, on)) {
                    cm_pend_drop(g_cs.id);
                    g_cs.msg[0] = 0;
                } else
                    cm_pend_set(g_cs.id, on);
            }
            if (!strcmp(g_cs.state, "done") && g_cs.id[0] && !strcmp(g_cs.op, "remove")) cm_removed_add(g_cs.id);
            if (!strcmp(g_cs.state, "done") && g_cs.id[0] && !strcmp(g_cs.op, "install")) cm_fresh_add(g_cs.id);
            if (!strcmp(g_cs.state, "done") && g_cs.id[0] && !strcmp(g_cs.op, "install"))
                for (int k = 0; k < 8; k++)
                    if (!strcmp(g_cm_removed[k], g_cs.id)) g_cm_removed[k][0] = 0;
            if (!strcmp(g_cs.state, "done") && !strcmp(g_cs.op, "addrepo")) g_cm_kb[0] = 0;   /* added: the draft is done */
            if (!strcmp(g_cs.state, "error") && !strcmp(g_cs.op, "apply")) g_cm_reloading = 0;   /* not applied: Reload again */
            if (g_cm_flip.at && (!strcmp(g_cs.op, "enable") || !strcmp(g_cs.op, "disable")) && !strcmp(g_cs.id, g_cm_flip.id) &&
                (!strcmp(g_cs.state, "done") || !strcmp(g_cs.state, "error"))) {
                if (!strcmp(g_cs.state, "error")) {   /* not written: the box shows what it showed before */
                    if (g_cm_flip.prev < 0) cm_pend_drop(g_cm_flip.id);
                    else cm_pend_set(g_cm_flip.id, g_cm_flip.prev);
                }
                g_cm_flip.at = 0;
            }
            if (seen) g_cm_fresh = 1;
        }
    } else {
        memset(&g_cs, 0, sizeof g_cs);
        smt = 0;
        ssz = 0;
    }
    seen = 1;
}
/* the fetcher is serving a request, or Update all is between two of its installs: send nothing */
static int cm_busy(void) {
    return g_cm_ua.on || (g_cs.state[0] && strcmp(g_cs.state, "idle") && strcmp(g_cs.state, "done") && strcmp(g_cs.state, "error"));
}
static void cm_req(const char *fmt, const char *a, const char *b) {   /* one request line, tmp + rename */
    char line[512];   /* the watcher reads up to 512 bytes; a request that does not fit is not sent */
    int n = snprintf(line, sizeof line, fmt, a ? a : "", b ? b : "");
    if (n < 0 || n >= (int) sizeof line) return;
    FILE *f = fopen(CM_REQ ".new", "we");
    if (!f) return;
    fprintf(f, "%s\n", line);
    fclose(f);
    rename(CM_REQ ".new", CM_REQ);
}
/* 1 = this core is new enough for the mod (no min_core, or min_core <= AZ_CORE_VERSION) */
static int cm_core_ok(const cm_cmod *m) { return azcm_min_core_ok(m->mc); }
static int cm_works(const cm_cmod *m) {   /* "any", or this deck's tag in the list */
    if (!strcmp(m->fw, "any")) return 1;
    const char *t = azcm_fwtag();
    size_t tl = strlen(t);
    if (!tl) return 0;
    for (const char *p = m->fw; *p;) {
        const char *e = strchr(p, ',');
        size_t l = e ? (size_t) (e - p) : strlen(p);
        if (l == tl && !strncmp(p, t, tl)) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}
static int cm_installed_idx(const char *id) {   /* the azcm index of an installed mod, or -1 */
    for (int i = 0; i < azcm_count(); i++) {
        azcm_info_t m;
        if (!azcm_info(i, &m) && !strcmp(m.id, id)) return i;
    }
    return -1;
}
/* The catalog entry offering a newer version (not merely a different one: a repository that went
 * back to an older release must not offer a downgrade as an update) of the installed mod `id` that
 * this deck can run, or -1. */
static int cm_update_idx(const char *id) {
    for (int k = 0; k < g_ncc; k++)
        if (!strcmp(g_cc[k].id, id) && g_cc[k].inst[0] && azcm_ver_newer(g_cc[k].ver, g_cc[k].inst))
            return cm_works(&g_cc[k]) && cm_core_ok(&g_cc[k]) ? k : -1;
    return -1;
}
/* Updates: the catalog entry an installed mod can update from (the Installed list's "Update
 * available"), or -1 */
static int cm_upd_cat(const char *id) {
    int k = cm_update_idx(id);
    return k >= 0 && !cm_removed(id) ? k : -1;
}
static int cm_upd_ok(const char *id, void *user) {
    (void) user;
    return cm_upd_cat(id) >= 0;
}
/* the catalog indices of every mod the Updates tab lists, in the Installed list's order (the
 * mods loaded at start, then those installed since); returns how many */
static int cm_upd_list(int *out) {
    int n = 0, nm = azcm_count();
    for (int i = 0; i < nm; i++) {
        azcm_info_t m;
        int k;
        if (!azcm_info(i, &m) && (k = cm_upd_cat(m.id)) >= 0) out[n++] = k;
    }
    for (int i = 0; i < g_ncc && n < CM_NCAT; i++)
        if (g_cc[i].inst[0] && cm_installed_idx(g_cc[i].id) < 0 && cm_upd_cat(g_cc[i].id) == i) out[n++] = i;
    return n;
}
/* Update all: one step of the run (cm_updall.h); sends the next install when it is due. Called
 * every overlay tick, with the menu open or not, so the run keeps going wherever the DJ is. */
static void cm_updall_tick(void) {
    if (!g_cm_ua.on) return;
    cm_load();
    cmu_in in = {g_cs.state, g_cs.op, g_cs.id, g_cm_sgen, g_cm_cgen, cm_ms()};
    if (cmu_step(&g_cm_ua, &in, cm_upd_ok, 0) != CMU_SEND) return;
    int k = cm_upd_cat(cmu_cur(&g_cm_ua));
    if (k >= 0) cm_req("install %s %s", g_cc[k].repo, g_cc[k].id);
}
static void cm_updall_start(void) {
    int idx[CM_NCAT], n = cm_upd_list(idx);
    const char *ids[CM_NCAT];
    for (int i = 0; i < n; i++) ids[i] = g_cc[idx[i]].id;
    cmu_start(&g_cm_ua, ids, n);
    cm_updall_tick();
}
void azmod_cm_bg_tick(void) { cm_updall_tick(); }
/* ---- small helpers ---------------------------------------------------------------- */
static void cm_reset(void) {
    g_cm_view = CMV_HOME;
    g_cm_page = g_cm_cat = -1;
    g_cm_sel = 0;
    g_cm_scroll = 0;
    g_cm_home_sel = -1;
    g_cm_row = -1;
    g_cm_sortpop = 0;
}
static int cm_off_now(void) { return g_cm_pending_off >= 0 ? g_cm_pending_off : azcm_all_off(); }
/* off by the stick's flag and not changed here: only the AZ-MODS app turns them back on */
static int cm_master_stick(void) {
    int off = cm_off_now();
    return off && azcm_off_by_stick() && off == azcm_all_off();
}
static const char *cm_state_text(const azcm_info_t *m, char *buf, int n, uint32_t *col) {
    /* a checkbox tap not yet answered: a mod with stop() changes at the answer, without a
     * reload, so "after Reload" waits for it (cm_load) */
    if (g_cm_flip.at && !strcmp(g_cm_flip.id, m->id) && cm_ms() - g_cm_flip.at <= CM_FLIP_MS) {
        *col = C_AVAIL;
        return cm_pend_get(m->id) == 0 ? "Turning off..." : "Turning on...";
    }
    switch (m->state) {
    case AZCM_LOADED:
        if (cm_removed(m->id)) {
            *col = C_AVAIL;
            return "Removed. Unloads after Reload.";
        }
        if (cm_pend_get(m->id) == 0) {
            *col = C_AVAIL;
            return "Disabled after Reload.";
        }
        *col = C_OK;
        return "Running";
    case AZCM_DISABLED:
        if (cm_removed(m->id)) {
            *col = C_AVAIL;
            return "Removed.";
        }
        if (cm_pend_get(m->id) == 1) {
            *col = C_AVAIL;
            return "Enabled after Reload.";
        }
        *col = C_TEXT_DIM;
        snprintf(buf, n, "%s", m->reason[0] ? m->reason : "Disabled");
        return buf;
    case AZCM_SUSPENDED:
        if (cm_removed(m->id)) {
            *col = C_AVAIL;
            return "Removed.";
        }
        if (cm_pend_get(m->id) == 1) {
            *col = C_AVAIL;
            return "Loads again after Reload.";
        }   /* Try again */
        if (azcm_suspended() && g_cm_keptoff) {
            *col = C_TEXT_DIM;
            return "Disabled (all community mods are disabled)";
        }
        if (azcm_suspended() && g_cm_resumed) {
            *col = C_AVAIL;
            return cm_off_now() ? "Stays disabled: community mods are disabled" : "Loads again after Reload.";
        }
        *col = CM_BAD;
        if (azcm_suspended()) snprintf(buf, n, "%s", "Not loaded");
        else if (strstr(m->reason, "while this mod was starting")) snprintf(buf, n, "Disabled: the deck stopped while it was starting");
        else if (strstr(m->reason, "crashed")) snprintf(buf, n, "Disabled: it crashed the deck");
        else snprintf(buf, n, "%s", m->reason[0] ? m->reason : "Disabled after a problem");
        return buf;
    case AZCM_FAILED:
        *col = CM_BAD;
        snprintf(buf, n, "Failed: %s", m->reason);
        return buf;
    case AZCM_NEW:       /* enabled on the USB, not yet approved on this deck */
        if (cm_removed(m->id)) {
            *col = C_AVAIL;
            return "Removed.";
        }
        if (m->allowed) {
            *col = C_AVAIL;
            return "Allowed. Loads after Reload.";
        }
        *col = C_HI;
        return "New mod found: allow?";
    default:
        if (cm_removed(m->id)) {
            *col = C_AVAIL;
            return "Removed.";
        }
        if (cm_pend_get(m->id) == 0) {
            *col = C_AVAIL;
            return "Disabled after Reload.";
        }
        *col = C_AVAIL;
        snprintf(buf, n, "Not loaded: %s", m->reason);
        return buf;
    }
}
/* turned off on its own after a crash (blame) or a stop while starting; the notice on the Installed tab covers the global case */
static int cm_crashed(const azcm_info_t *m);
/* 1 = the mod is turned on, whether or not it loaded. The core reads the Turn off mark before
 * any other check, so a mod refused at load (wrong firmware, newer AZ-MODS needed) or whose
 * init() failed is still turned on: its toggle says Disable, as the catalog page's does. */
static int cm_turned_on(const azcm_info_t *m) {
    return m->state == AZCM_LOADED || m->state == AZCM_REFUSED || m->state == AZCM_FAILED;
}
/* 1 = something on this screen changes only after a reload: an install or remove, a Turn on/off
 * that differs from what loaded, Try again, Load next time, or the master switch */
static int cm_need_reload(void) {
    if (g_cm_reload || cm_off_now() != azcm_all_off() || (g_cm_resumed && azcm_suspended())) return 1;
    for (int i = 0; i < azcm_count(); i++) {
        azcm_info_t m;
        if (azcm_info(i, &m)) continue;
        if (m.state == AZCM_NEW && m.allowed && !cm_removed(m.id)) return 1;   /* allowed: loads after the reload */
        int pend = cm_pend_get(m.id);
        if (pend == 1 && !cm_turned_on(&m)) return 1;   /* turned on, or Try again */
        if (pend == 0 && cm_turned_on(&m)) return 1;    /* turned off */
    }
    return 0;
}
static int cm_crashed(const azcm_info_t *m) { return m->state == AZCM_SUSPENDED && !azcm_suspended() && !strstr(m->reason, "community mods"); }
static const char *cm_cat_name(const char *id) {   /* the catalog's name for an id the core only knows by id */
    /* only the line of the installed copy names it: a line held by another installed copy is
     * another author's mod that merely shares the id */
    for (int i = 0; i < g_ncc; i++)
        if (!strcmp(g_cc[i].id, id) && g_cc[i].name[0] && !g_cc[i].held[0]) return g_cc[i].name;
    return id;
}
static int cm_status_shown(void) { return g_cs.state[0] && (g_cm_fresh || cm_busy()); }
/* the rail at the home page's left edge (cm_layout_rail) */
static int cm_rail_w(const azmod_fb *fb) { return fb->w * 112 / 1280; }
static void cm_box(const azmod_fb *fb, int *x, int *y, int *w, int *h) {   /* the right pane (full: the whole menu) under the header */
    if (g_cm_full) {
        *x = (g_cm_view == CMV_HOME ? cm_rail_w(fb) : 0) + fb->w / 40;   /* the home page: right of the rail */
        *w = fb->w - fb->w / 40 - *x;
        *y = dock_y(fb) + menu_hdrh(fb) + fb->h * 12 / 800;
        *h = dock_y(fb) + dock_h(fb) - *y - fb->h * 8 / 800;
        return;
    }
    *x = dock_x(fb) + fb->w / 80;
    *w = dock_w(fb) - fb->w / 40;
    *y = dock_y(fb) + menu_hdrh(fb) + fb->h * 10 / 800;
    *h = dock_y(fb) + dock_h(fb) - *y - fb->h * 8 / 800;
}
/* text cut to a width with "..." (the TTF draw does not clip) */
static int cm_tw(const char *s, int px);
static void cm_textw(const azmod_fb *fb, int x, int y, int w, int h, const char *s, int px, uint32_t c) {
    char t[256];
    snprintf(t, sizeof t, "%s", s ? s : "");
    if (w > 0 && cm_tw(t, px) > w) {
        size_t L = strlen(t);
        if (L > sizeof t - 4) L = sizeof t - 4;   /* room for "..." and its NUL */
        while (L > 0 && (t[L - 1] & 0xC0) == 0x80) L--;   /* never split a UTF-8 sequence */
        while (L > 0) {
            L--;
            while (L > 0 && (t[L] & 0xC0) == 0x80) L--;
            memcpy(t + L, "...", 4);
            if (cm_tw(t, px) <= w) break;
        }
    }
    c_text(fb, x, y, 0, h, t, px, c);
}
/* The header's breadcrumb: "Community Mods  >  <tab>" on the home page, then each page opened
 * from it ("  >  Beat Counter", "  >  Install"); the reload page stands alone ("  >  Reload").
 * When it is wider than w at px, the tab is left out first; cm_textw then ends it with "...". */
#define CM_CRUMB_MAX 192
#define CM_SEP "  >  "
static const char *const CM_TABNAME[4] = {"Installed", "Browse", "Repositories", "Updates"};
static void cm_crumbs(char *t, size_t n, int w, int px) {
    const char *page = 0, *sub = 0;
    azcm_info_t m;
    int tab = g_cm_tab >= 0 && g_cm_tab < 4 ? g_cm_tab : 0;
    switch (g_cm_view) {
    case CMV_MOD: page = !azcm_info(g_cm_page, &m) ? m.name : 0; break;
    case CMV_CAT: page = g_cm_cat >= 0 && g_cm_cat < g_ncc ? g_cc[g_cm_cat].name : 0; break;
    case CMV_TRUST:
        page = g_cm_cat >= 0 && g_cm_cat < g_ncc ? g_cc[g_cm_cat].name : 0;
        sub = "Install";
        break;
    case CMV_KBD: page = "Add repository"; break;
    case CMV_RELOAD: snprintf(t, n, "Community Mods" CM_SEP "Reload"); return;
    default: break;
    }
    for (int with_tab = 1; with_tab >= 0; with_tab--) {
        int k = snprintf(t, n, "Community Mods");
        if (with_tab && k >= 0 && (size_t) k < n)
            k += snprintf(t + k, n - (size_t) k, CM_SEP "%s", CM_TABNAME[tab]);
        if (page && page[0] && k >= 0 && (size_t) k < n)
            k += snprintf(t + k, n - (size_t) k, CM_SEP "%s", page);
        if (sub && k >= 0 && (size_t) k < n)
            snprintf(t + k, n - (size_t) k, CM_SEP "%s", sub);
        if (w <= 0 || cm_tw(t, px) <= w || (!page && !sub))
            return;
    }
}
static int cm_tw(const char *s, int px) {
    int w = azmod_ttf_menu_width ? azmod_ttf_menu_width(s, (int) strlen(s), px, 2) : 0;
    return w > 0 ? w : (int) strlen(s) * px / 2;
}
/* a button: filled orange (primary), outlined (secondary) or grayed (inactive) */
static void cm_button(const azmod_fb *fb, int x, int y, int w, int h, const char *t, int primary, int active, int foc) {
    if (primary && active) fill_rect(fb, x, y, w, h, C_HI);
    else draw_rect_outline(fb, x, y, w, h, 2, active ? C_CHECK_BOX : C_STEM_OFF_FG);
    if (foc) draw_rect_outline(fb, x - 5, y - 5, w + 10, h + 10, 4, C_TEXT);
    int px = menu_text_px(fb); /* the panes' text size, so a label reads like the text beside it */
    if (px > h * 62 / 100)
        px = h * 62 / 100;
    c_text(fb, x, y, w, h, t, px, primary && active ? C_HI_TEXT : active ? C_TEXT
                                                                         : C_STEM_OFF_FG);
}
/* working: eight dots on a circle of radius r around cx, cy, the lit one going round (8 steps a second) */
static void cm_spinner(const azmod_fb *fb, int cx, int cy, int r, uint32_t lit, uint32_t dim) {
    static const signed char U[8][2] = {{0, -100}, {71, -71}, {100, 0}, {71, 71}, {0, 100}, {-71, 71}, {-100, 0}, {-71, -71}};
    int step = (int) ((cm_ms() / 125) % 8), dr = r / 5 > 1 ? r / 5 : 1;
    for (int k = 0; k < 8; k++) {
        int age = (step - k + 8) % 8;   /* 0 = lit, then fading behind it */
        fill_disc(fb, cx + U[k][0] * (r - dr) / 100, cy + U[k][1] * (r - dr) / 100, age < 3 ? dr : dr * 2 / 3, age == 0 ? lit : dim);
    }
}
/* Reload, one square in one place (the header's top right, cm_layout_header): the Mod Menu's Reload
 * icon over "Reload", filled; once pressed, outlined, the icon replaced by the spinner, until the
 * deck restarts */
static void cm_reload_square(const azmod_fb *fb, int x, int y, int sz, int waiting, int foc) {
    int isz = sz * 46 / 100, lpx = sz * 22 / 100, iy = y + sz * 12 / 100;
    if (waiting) draw_rect_outline(fb, x, y, sz, sz, 2, C_CHECK_BOX);
    else fill_rect(fb, x, y, sz, sz, C_HI);
    if (foc) draw_rect_outline(fb, x - 5, y - 5, sz + 10, sz + 10, 4, C_TEXT);
    uint32_t ink = waiting ? C_TEXT_DIM : C_HI_TEXT;
    if (waiting) cm_spinner(fb, x + sz / 2, iy + isz / 2, isz / 2, C_HI, C_TEXT_DIM);
    else blit_icon(fb, x + (sz - isz) / 2, iy, isz, ink, IC_RELOAD);
    c_text(fb, x, iy + isz + sz * 4 / 100, sz, lpx * 3 / 2, "Reload", lpx, ink);
}
/* the mod's tile picture: its icon (decoded by EP147's JUCE on the message thread, community_icons.c)
 * once ready; nothing while its decode is queued; with no icon, or one that cannot be read, its
 * first letter on a color from its id. Not themed: an icon is the author's artwork. */
/* premultiplied ARGB pixels (w x h) over the framebuffer at x, y, clipped to it */
static void cm_blit_argb(const azmod_fb *fb, int x, int y, int w, int h, const uint32_t *ip) {
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= fb->h) continue;
        uint32_t *row = (uint32_t *) (fb->px + (long) yy * fb->stride);
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= fb->w) continue;
            uint32_t p = ip[j * w + i], a = p >> 24, d = row[xx], o = 0;
            for (int sh = 0; sh <= 16; sh += 8) {
                uint32_t c = ((p >> sh) & 0xff) + ((d >> sh) & 0xff) * (255 - a) / 255;
                o |= (c > 255 ? 255 : c) << sh;
            }
            row[xx] = o;
        }
    }
}
static void cm_icon(const azmod_fb *fb, int x, int y, int sz, const char *id, const char *name, const char *icon) {
    int pending = 0;
    const uint32_t *ip = icon && icon[0] ? azcm_icon(icon, sz, &pending) : NULL;
    if (pending)
        return;
    if (ip) {
        cm_blit_argb(fb, x, y, sz, sz, ip);
        return;
    }
    static const uint32_t pal[6] = {0x002A6FDBu, 0x00C2410Cu, 0x00198754u, 0x007E3AF2u, 0x00B02A5Bu, 0x00117A8Bu};
    unsigned h = 2166136261u;
    for (const char *p = id; *p; p++) h = (h ^ (unsigned char) *p) * 16777619u;
    fill_rect(fb, x, y, sz, sz, pal[h % 6]);
    /* clang-format off */
    char t[2] = { name[0] >= 'a' && name[0] <= 'z' ? (char)(name[0] - 32) : name[0] ? name[0] : '?', 0 };
    /* clang-format on */
    c_text(fb, x, y, sz, sz, t, sz * 55 / 100, 0x00FFFFFFu);
}
/* whether cm_progress draws anything: a finished job with no message (a clean refresh) has no line */
static int cm_progress_line(void) {
    if (!g_cs.state[0] || (!strcmp(g_cs.state, "done") && !g_cs.msg[0])) return 0;
    if (!strcmp(g_cs.state, "done") && (!strcmp(g_cs.op, "enable") || !strcmp(g_cs.op, "disable")) && g_cs.id[0]) {
        /* turned back to what loaded: "turns on after Reload" would announce a change that is not there */
        int ii = cm_installed_idx(g_cs.id);
        azcm_info_t m;
        if (ii >= 0 && !azcm_info(ii, &m) && (m.state == AZCM_LOADED) == !strcmp(g_cs.op, "enable")) return 0;
    }
    return 1;
}
static int g_cm_nobar;   /* the page draws the download bar itself, under its button (cm_layout_cat) */
static void cm_progress(const azmod_fb *fb, int x, int y, int w, int lh, int dpx) {   /* the status line + bar */
    if (!cm_progress_line()) return;
    /* clang-format off */
    uint32_t col = !strcmp(g_cs.state, "error") ? CM_BAD : !strcmp(g_cs.state, "done") ? C_OK : C_AVAIL;
    /* clang-format on */
    int dl = !strcmp(g_cs.state, "downloading"), bw = dl && g_cs.total > 0 && !g_cm_nobar ? w * 2 / 5 : 0;
    char t[160];
    if (dl) snprintf(t, sizeof t, "Downloading %s", g_cs.msg);
    else snprintf(t, sizeof t, "%s", g_cs.msg[0] ? g_cs.msg : g_cs.state);
    cm_textw(fb, x, y, w - bw - (bw ? lh : 0), lh, t, dpx, col);
    if (bw) {
        int bx = x + w - bw, by = y + lh / 3, bh = lh / 3;
        draw_rect_outline(fb, bx, by, bw, bh, 2, C_CHECK_BOX);   /* the track: the checkbox outline reads in Day and dark */
        fill_rect(fb, bx, by, (int) (bw * (g_cs.done > g_cs.total ? g_cs.total : g_cs.done) / g_cs.total), bh, C_HI);
    }
}
/* text word-wrapped to a width, at most maxl lines (the last one cut with "..."); returns the
 * lines used. Same answer whether drawing or not, so the layout pass and the draw agree. */
static int cm_wrap(const azmod_fb *fb, int x, int y, int w, int lh, const char *s, int px, uint32_t c, int maxl, int draw) {
    int lines = 0;
    const char *p = s ? s : "";
    while (*p && lines < maxl) {
        while (*p == ' ') p++;
        char t[256];
        int best = 0, i = 0;
        for (; p[i] && i < (int) sizeof t - 1; i++) {
            if (p[i] == ' ' || !p[i + 1]) {
                int L = p[i] == ' ' ? i : i + 1;
                memcpy(t, p, L);
                t[L] = 0;
                if (cm_tw(t, px) <= w) best = L;
                else break;
            }
        }
        if (!best || lines == maxl - 1) best = (int) strlen(p) < (int) sizeof t - 1 ? (int) strlen(p) : (int) sizeof t - 1;   /* last line / one long word: cut */
        memcpy(t, p, best);
        t[best] = 0;
        if (draw) cm_textw(fb, x, y + lines * lh, w, lh, t, px, c);
        lines++;
        p += best;
    }
    return lines ? lines : 1;
}
/* cm_wrap without the last-line cut: draws lines [skip, skip+maxl) of the wrapped text at y and
 * returns how many lines the whole text wraps to (the catalog page's scrolling description) */
static int cm_wrap_from(const azmod_fb *fb, int x, int y, int w, int lh, const char *s, int px, uint32_t c, int skip, int maxl, int draw) {
    int lines = 0;
    const char *p = s ? s : "";
    while (*p && lines < 256) {
        while (*p == ' ') p++;
        if (!*p) break;
        char t[256];
        int best = 0, i = 0;
        for (; p[i] && i < (int) sizeof t - 1; i++) {
            if (p[i] == ' ' || !p[i + 1]) {
                int L = p[i] == ' ' ? i : i + 1;
                memcpy(t, p, L);
                t[L] = 0;
                if (cm_tw(t, px) <= w) best = L;
                else break;
            }
        }
        if (!best) best = (int) strlen(p) < (int) sizeof t - 1 ? (int) strlen(p) : (int) sizeof t - 1;   /* one long word: cut */
        memcpy(t, p, best);
        t[best] = 0;
        if (draw && lines >= skip && lines < skip + maxl) cm_textw(fb, x, y + (lines - skip) * lh, w, lh, t, px, c);
        lines++;
        p += best;
    }
    return lines;
}
/* ---- the keyboard (Add repository): owner/repo or a URL -------------------------- */
/* Full screen: the geometry of EP147's own search keyboard (key 0.066 of the width at a
 * 0.072 pitch, rows 0.098 of the height with a 0.012 gap; DEL right on row 1, 123 left and
 * SPACE right on row 3), its three rows at the bottom of the full-screen area. */
#define CMK_KW 0.066f
#define CMK_PITCH 0.072f
#define CMK_RH 0.098f
#define CMK_GAP 0.012f
static void cm_kbf_rect(const azmod_fb *fb, int row, int k, int nk, int *x, int *y, int *w, int *h) {
    int bx, by, bw, bh;
    cm_box(fb, &bx, &by, &bw, &bh);
    (void) bx;
    (void) bw;
    int kh = (int) (CMK_RH * fb->h), kg = (int) (CMK_GAP * fb->h);
    int y0 = by + bh - 3 * kh - 2 * kg;
    *y = y0 + row * (kh + kg);
    *h = kh;
    if (k < 0) {   /* the specials: -1 DEL (row 0, right), -2 123/abc (row 2, left), -3 SPACE (row 2, right) */
        if (k == -1) {
            *w = (int) (CMK_KW * fb->w);
            *x = (int) (0.960f * fb->w) - *w;
        } else if (k == -2) {
            *w = (int) (CMK_KW * fb->w);
            *x = (int) (0.035f * fb->w);
        } else {
            *w = (int) (0.104f * fb->w);
            *x = (int) (0.960f * fb->w) - *w;
        }
        return;
    }
    float span = nk * CMK_PITCH - (CMK_PITCH - CMK_KW), st = (1.0f - span) / 2.0f;
    *x = (int) ((st + k * CMK_PITCH) * fb->w);
    *w = (int) (CMK_KW * fb->w);
}
static const char *CM_KB[2][4] = {{"qwertyuiop", "asdfghjkl", "zxcvbnm", ""}, {"1234567890", "-_./:@", "~+=?&#", ""}};
static void cm_kb_rect(const azmod_fb *fb, int row, int k, int nk, int *x, int *y, int *w, int *h) {
    int bx, by, bw, bh;
    cm_box(fb, &bx, &by, &bw, &bh);
    int kw = bw / 11, gap = kw / 8, ky = by + bh - 4 * (kw + gap);
    int rowx = bx + (bw - nk * kw) / 2;
    *x = rowx + k * kw + gap / 2;
    *y = ky + row * (kw + gap);
    *w = kw - gap;
    *h = kw;
}
/* ---- the layout --------------------------------------------------------------------- */
static az_rect g_cm_area;   /* the mod screen's area, set by cm_layout; w = 0: no screen shown */
/* the catalog page's description: the box cm_layout gave it (h = 0: none, or it fits), its first
 * shown line, its line height and how far it can scroll; for the drag */
static struct {
    az_rect r;
    int skip, lh, max, cat;
} g_cm_desc = {.cat = -1};
static struct {
    bool on;
    int y0, s0;
} g_cm_dd;
/* What cm_layout's parts share: where to draw and where to record the controls, the page
 * geometry, and the running y position and control count. */
struct cm_lay {
    azmod_state *s;
    const azmod_fb *fb;
    cm_ctl *ctl;
    int draw, bx, by, bw, bh, rh, lh, px, dpx, pad, bh3, y, n;
};
#define CM_CTL(k, i, X, Y, W, H) \
    do {                         \
        if (n < CM_MAXCTL) {     \
            ctl[n].kind = (k);   \
            ctl[n].idx = (i);    \
            ctl[n].x = (X);      \
            ctl[n].y = (Y);      \
            ctl[n].w = (W);      \
            ctl[n].h = (H);      \
            n++;                 \
        }                        \
    } while (0)
/* the home page shows no knob focus (the knob scrolls it) */
#define CM_FOC (g_full && s->pane && n < CM_MAXCTL && ((g_cm_knob && g_cm_view != CMV_HOME && g_cm_sel == n) || g_cm_press == n || (g_cm_knob && g_cm_view == CMV_HOME && g_cm_in && g_cm_rsel == n)))
/* the Installed and Updates tabs: the knob's focus, kept as the control's code (cm_code) */
#define CM_HF(k, i) (g_full && s->pane && g_cm_view == CMV_HOME && g_cm_knob && g_cm_hfoc == ((k) << 16 | ((i) & 0xffff)))

/* The notice on the Installed tab, under the tabs, while the core did not load community mods this
 * start (the deck restarted twice while they were starting, or crashed several times with them on).
 * Not loaded: the reason, the mods' names, Load next time / Keep off. After Load next time: that
 * they load again after a reload, and a Reload button that sends the same request as the header's
 * Reload now. After Keep off: that they stay disabled, and no buttons. The box is the same size in
 * all three, so nothing below it moves. */
static void cm_home_notloaded(struct cm_lay *lay) {
    azmod_state *s = lay->s;
    const azmod_fb *fb = lay->fb;
    cm_ctl *ctl = lay->ctl;
    int draw = lay->draw;
    int bx = lay->bx;
    int bw = lay->bw;
    int lh = lay->lh;
    int dpx = lay->dpx;
    int pad = lay->pad;
    int bh3 = lay->bh3;
    int y = lay->y;
    int n = lay->n;
    int bh2 = 3 * lh + bh3 + 2 * pad;
    int bw2 = fb->w * 190 / 1280, gy = y + pad / 2 + 3 * lh + pad / 2;
    if (draw) {
        fill_rect(fb, bx, y, bw, bh2, 0x00343434u);
        draw_rect_outline(fb, bx, y, bw, bh2, 2, g_cm_keptoff ? C_TEXT_DIM : g_cm_resumed ? C_CHECK_BOX
                                                                                          : CM_BAD);
        int ty = y + pad / 2;
        /* clang-format off */
        c_text(fb, bx + pad, ty, 0, lh, g_cm_keptoff ? "Community mods are disabled. They stay disabled after a reload."
                                        : !g_cm_resumed ? "Community mods were not loaded this time."
                                        : cm_off_now() ? "They are disabled below, so they stay disabled after a reload."
                                        : "They load again after a reload.", dpx, g_cm_resumed || g_cm_keptoff ? C_TEXT : CM_BAD);
        /* clang-format on */
        if (!g_cm_resumed && !g_cm_keptoff) {
            char why[112] = "The deck restarted twice while they were starting.";
            azcm_info_t m0;
            int cn = 0, cm = 0;
            if (azcm_count() > 0 && !azcm_info(0, &m0) && sscanf(m0.reason, "Suspended: the deck crashed %d times in %d minutes", &cn, &cm) == 2)
                snprintf(why, sizeof why, "The deck crashed %d times in %d minutes with them on.", cn, cm);   /* the core's reason */
            c_text(fb, bx + pad, ty + lh, 0, lh, why, dpx, C_TEXT_DIM);
        }
        char names[200] = "Not loaded: ";
        int nm = azcm_count();
        for (int i = 0; i < nm; i++) {
            azcm_info_t m;
            if (azcm_info(i, &m) || m.state != AZCM_SUSPENDED) continue;
            size_t L = strlen(names);
            snprintf(names + L, sizeof names - L, "%s%s", L > 12 ? ", " : "", cm_cat_name(m.id));
        }
        cm_textw(fb, bx + pad, ty + 2 * lh, bw - 2 * pad, lh, names, dpx, C_TEXT_DIM);
    }
    if (g_cm_keptoff) {
        /* nothing to press: the row stays empty */
    } else if (!g_cm_resumed) {
        for (int b = 0; b < 2; b++) {
            int qx = bx + pad + b * (bw2 + pad);
            if (draw) cm_button(fb, qx, gy, bw2, bh3, b == 0 ? "Load next time" : "Keep off", b == 0, 1,
                                CM_FOC || CM_HF(b == 0 ? CMC_RESUME : CMC_KEEPOFF, 0));
            CM_CTL(b == 0 ? CMC_RESUME : CMC_KEEPOFF, 0, qx, gy, bw2, bh3);
        }
    } else if (draw) {   /* Load next time pressed: Reload is the header's, top right, as everywhere */
        c_text(fb, bx + pad, gy, 0, bh3, "Press Reload at the top right to load them now.", dpx, C_TEXT_DIM);
    }
    y += bh2 + pad;
    lay->y = y;
    lay->n = n;
}

/* An Installed row's checkbox: CMC_CHK idx is an azcm index, or CM_CHK_CAT | a catalog index (a
 * mod installed since the start). 1 = the row has one (*on: what loads after a reload; id, when
 * not NULL, the mod's id), 0 = none: the mod's page offers no Enable / Disable either (removed,
 * waiting for Allow, disabled after a crash, community mods not loaded this start). */
#define CM_CHK_CAT 0x4000
static int cm_chk(int idx, char *id, int *on) {
    const char *mid;
    if (idx & CM_CHK_CAT) {
        int k = idx & ~CM_CHK_CAT;
        if (k < 0 || k >= g_ncc || !g_cc[k].inst[0] || cm_installed_idx(g_cc[k].id) >= 0 || cm_removed(g_cc[k].id)) return 0;
        int pend = cm_pend_get(g_cc[k].id);
        *on = pend >= 0 ? pend : 1;   /* as the catalog page's Enable / Disable */
        mid = g_cc[k].id;
    } else {
        azcm_info_t m;
        if (azcm_info(idx, &m) || m.state == AZCM_NEW || cm_removed(m.id) || cm_crashed(&m) || azcm_suspended()) return 0;
        int pend = cm_pend_get(m.id);
        *on = pend >= 0 ? pend : cm_turned_on(&m);   /* as the mod page's Enable / Disable */
        if (id) snprintf(id, 33, "%s", m.id);
        return 1;
    }
    if (id) snprintf(id, 33, "%s", mid);
    return 1;
}
/* where an Installed row puts the Mod Menu list's checkbox and > (menu_full.c's row geometry):
 * sz square boxes at the row's right edge, the > outermost, the checkbox to its left */
static void cm_row_geom(const azmod_fb *fb, int bx, int bw, int y, int h, int *sz, int *chx, int *cbx, int *cby) {
    *sz = fb->h * 20 / 800;
    *chx = bx + bw - fb->w / 40 - *sz;
    *cbx = *chx - fb->w / 40 - *sz;
    *cby = y + (h - *sz) / 2;
}
/* the checkbox's touch area: the row's height, from a box left of it to the gap before the > */
static void cm_chk_ctl(struct cm_lay *lay, int *np, int kind, int idx, int y, int h) {
    cm_ctl *ctl = lay->ctl;
    int n = *np, sz, chx, cbx, cby;
    cm_row_geom(lay->fb, lay->bx, lay->bw, y, h, &sz, &chx, &cbx, &cby);
    int x0 = cbx - sz;
    CM_CTL(kind, idx, x0, y, chx - lay->fb->w / 80 - x0, h);
    *np = n;
}
/* An Installed row, drawn as the Mod Menu's list rows: the mod's icon, its name and a line under
 * it, then the checkbox (box: on / off, draw_checkbox) to the left of the > (draw_native_chevron),
 * both at the list's size; "Update available" left of them. */
/* the split's selected row (cm_home_installed sets it per row): 1 = selected, the knob in the list
 * (the Mod Menu list's blue); 2 = selected, the knob in its page (that list's gray) */
static int g_cm_rowsel;
static void cm_inst_row(struct cm_lay *lay, int y, int h, int foc, const char *id, const char *name, const char *icon,
                        const char *title, const char *line, const char *stt, uint32_t sc, int upd, int box, int on) {
    const azmod_fb *fb = lay->fb;
    int bx = lay->bx, bw = lay->bw, rh = lay->rh, lh = lay->lh, pad = lay->pad, dpx = lay->dpx;
    if (g_cm_rowsel) fill_rect(fb, bx, y, bw, h, g_cm_rowsel == 1 ? 0x00007DE1u : 0x00505050u);   /* as menu_full.c's list */
    else {
        fill_rect(fb, bx, y, bw, h, foc ? C_FOCUS_BAND : 0x00343434u);
        if (foc) c_focus_ring(fb, bx, y, bw, h);
    }
    int ic = h * 70 / 100, tx = bx + pad + ic + pad;
    cm_icon(fb, bx + pad, y + (h - ic) / 2, ic, id, name, icon);
    int sz, chx, cbx, cby, chw;
    cm_row_geom(fb, bx, bw, y, h, &sz, &chx, &cbx, &cby);
    /* the text ends before the checkbox (and the update label): a narrow list cuts it with "..." */
    int tend = (box ? cbx : chx) - pad - (upd ? fb->w / 40 + cm_tw("Update available", dpx) : 0);
    if (tend - tx < 1) tend = tx + 1;   /* cm_textw takes w <= 0 as "uncut" */
    cm_textw(fb, tx, y + fb->h * 4 / 800, tend - tx, rh - fb->h * 8 / 800, title, lay->px, C_TEXT);
    int aw = line[0] ? cm_tw(line, dpx) : 0;
    if (aw > tend - tx) aw = tend - tx;
    if (g_cm_rowsel) sc = C_TEXT;   /* on the selection's fill, one ink, as the Mod Menu's rows */
    cm_textw(fb, tx, y + rh - fb->h * 10 / 800, aw, lh, line, dpx, g_cm_rowsel ? C_TEXT : C_TEXT_DIM);
    if (tend - tx - aw > 1) cm_textw(fb, tx + aw, y + rh - fb->h * 10 / 800, tend - tx - aw, lh, stt, dpx, sc);
    chw = sz * 16 / 28;
    draw_native_chevron(fb, chx + (sz - chw) / 2, cby, sz, C_TEXT);
    int rx = chx;
    if (box) {
        draw_checkbox(fb, cbx, cby, sz, on != 0, foc != 0);
        rx = cbx;
    }
    if (upd) c_text(fb, rx - fb->w / 40 - cm_tw("Update available", dpx), y, 0, h, "Update available", dpx, C_HI);
}

/* The Installed tab: what the core loaded this start, then what was installed since. */
static int cm_isplit(void);
static void cm_home_installed(struct cm_lay *lay) {
    azmod_state *s = lay->s;
    const azmod_fb *fb = lay->fb;
    cm_ctl *ctl = lay->ctl;
    int draw = lay->draw;
    int bx = lay->bx;
    int split = cm_isplit(), bw0 = lay->bw;
    if (split) lay->bw = lay->bw / 2 - lay->pad;   /* the list's half; the page is drawn beside it (cm_layout_split) */
    int bw = lay->bw, first = -1, selok = 0;
    int rh = lay->rh;
    int lh = lay->lh;
    int dpx = lay->dpx;
    int pad = lay->pad;
    int y = lay->y;
    int n = lay->n;
    if (azcm_suspended()) {
        lay->y = y;
        lay->n = n;
        cm_home_notloaded(lay);
        y = lay->y;
        n = lay->n;
    }
    int nm = azcm_count(), shown = nm;
    for (int i = 0; i < g_ncc; i++)
        if (g_cc[i].inst[0] && cm_installed_idx(g_cc[i].id) < 0) shown++;
    if (!shown && draw) c_text(fb, bx + pad, y, 0, rh, "No community mods are installed. Find some under Browse.", dpx, C_TEXT_DIM);
    for (int i = 0; i < nm; i++) {
        azcm_info_t m;
        if (azcm_info(i, &m)) continue;
        int code = CMC_MOD << 16 | i;
        if (first < 0) first = code;
        selok |= code == g_cm_isel;
        int h = rh + lh - fb->h * 6 / 800, foc = CM_FOC || CM_HF(CMC_MOD, i), on = 0, box = cm_chk(i, NULL, &on);
        g_cm_rowsel = split && code == g_cm_isel ? (g_cm_in ? 2 : 1) : 0;
        int ci = -1;
        for (int k = 0; k < g_ncc; k++)
            if (!strcmp(g_cc[k].id, m.id) && g_cc[k].inst[0]) {
                ci = k;
                break;
            }
        if (draw) {
            char t[128];
            snprintf(t, sizeof t, "%s  %s", strcmp(m.name, m.id) || ci < 0 ? m.name : g_cc[ci].name,
                     m.version[0] || ci < 0 ? m.version : g_cc[ci].inst);
            char st[160];
            uint32_t sc;
            const char *stt = cm_state_text(&m, st, sizeof st, &sc);
            char line[220];
            if (m.author[0]) snprintf(line, sizeof line, "by %s   ", m.author);
            else line[0] = 0;
            cm_inst_row(lay, y, h, foc, m.id, t, ci >= 0 ? g_cc[ci].icon : "", t, line, stt, sc,
                        cm_update_idx(m.id) >= 0 && !cm_removed(m.id), box, on);
        }
        if (box) cm_chk_ctl(lay, &n, CMC_CHK, i, y, h);
        CM_CTL(CMC_MOD, i, bx, y, bw, h);
        y += h + fb->h * 6 / 800;
    }
    for (int i = 0; i < g_ncc; i++) {   /* installed since this start: on the stick, loads after Reload */
        cm_cmod *c = &g_cc[i];
        if (!c->inst[0] || cm_installed_idx(c->id) >= 0) continue;
        int code = CMC_INSTCAT << 16 | i;
        if (first < 0) first = code;
        selok |= code == g_cm_isel;
        int h = rh + lh - fb->h * 6 / 800, foc = CM_FOC || CM_HF(CMC_INSTCAT, i), pend = cm_pend_get(c->id), on = 0;
        g_cm_rowsel = split && code == g_cm_isel ? (g_cm_in ? 2 : 1) : 0;
        int box = cm_chk(CM_CHK_CAT | i, NULL, &on);
        if (draw) {
            char t[128];
            snprintf(t, sizeof t, "%s  %s", c->name, c->inst);
            char line[220];
            snprintf(line, sizeof line, "%s%s%s", c->author[0] ? "by " : "", c->author, c->author[0] ? "   " : "");
            int alloff = cm_off_now();
            /* clang-format off */
            cm_inst_row(lay, y, h, foc, c->id, c->name, c->icon, t, line,
                        alloff ? "Installed. Community mods are disabled." : pend == 0 ? "Installed, disabled." : "Installed. Starts after Reload.",
                        alloff ? C_TEXT_DIM : C_AVAIL, cm_update_idx(c->id) >= 0 && !cm_removed(c->id), box, on);
            /* clang-format on */
        }
        if (box) cm_chk_ctl(lay, &n, CMC_CHK, CM_CHK_CAT | i, y, h);
        CM_CTL(CMC_INSTCAT, i, bx, y, bw, h);
        y += h + fb->h * 6 / 800;
    }
    g_cm_rowsel = 0;
    if (!selok) {   /* nothing selected yet, or the selected row is gone: the first one */
        g_cm_isel = first;
        g_cm_in = 0;
    }
    lay->bw = bw0;
    lay->y = y;
    lay->n = n;
}

/* the catalog as cm_browse.h sees it, one entry per g_cc line */
static cmb_mod g_cm_bm[CM_NCAT];
static void cm_browse_fill(void) {
    for (int i = 0; i < g_ncc; i++) {
        cm_cmod *m = &g_cc[i];
        g_cm_bm[i] = (cmb_mod) {m->name, m->cat, m->upd, m->inst[0] && !cm_removed(m->id), cm_works(m) && cm_core_ok(m), m->counted, m->dl};
    }
}
/* the download mark: a down arrow sized to a line of lh px, its left edge at x; returns its width */
static int cm_dl_mark(const azmod_fb *fb, int x, int y, int lh, uint32_t c, int draw) {
    int h = lh * 9 / 20, w = (h * 3 / 4) | 1, t = h / 7 > 1 ? h / 7 : 1, top = y + (lh - h) / 2, hh = w / 2 + 1;
    if (draw) {
        fill_rect(fb, x + (w - t) / 2, top, t, h - hh, c);
        for (int r = 0; r < hh; r++) fill_rect(fb, x + r, top + h - hh + r, w - 2 * r, 1, c);
    }
    return w;
}
/* A tile's corner marks, each a right triangle in a top corner, a quarter of the tile wide (the
 * icon is centered and narrower, so neither reaches it): top right, orange with the Repositories
 * icon, a mod from a repository the user added (its page says why); top left, blue with BETA, a
 * mod its author marks as not proven stable yet. What is drawn inside stays under the diagonal. */
#define CM_BETA_BG 0x003A5BE0u /* hue 228: outside the selection blues (195..220) a theme recolors */
/* "BETA" along the top-left mark's long edge, reading up to the right: drawn once into a small
 * buffer in EP147's face, then turned 45 degrees into the corner (bilinear, so the face's own
 * anti-aliasing is kept) in white, unthemed: white on the mark's blue in every theme. The size is
 * the largest whose box fits between the corner and the long edge. */
#define CM_BT_W 96
#define CM_BT_H 32
static void cm_beta_text(const azmod_fb *fb, int tx, int ty, int ce, int dpx) {
    static uint8_t buf[CM_BT_W * CM_BT_H * 4];
    const double r2 = 1.41421356237;
    int px = dpx, w = 0, h = 0;
    for (; px >= 8; px--) {   /* chord at the text's near edge >= its width; its far edge inside the mark */
        w = cm_tw("BETA", px) + 2, h = px + 2;
        double t = ce / r2 - h / 2.0 - 1;
        if (w <= CM_BT_W && h <= CM_BT_H && 2 * (t - h / 2.0) >= w) break;
    }
    if (px < 8) return;
    memset(buf, 0, sizeof buf);
    azmod_fb ofb = {buf, w, h, CM_BT_W * 4};
    c_text(&ofb, 0, 0, w, h, "BETA", px, C_HI);
    unsigned mx = 1;   /* the face's full ink, whatever the theme made of the color: coverage = value / mx */
    for (int i = 0; i < CM_BT_H * CM_BT_W; i++) {
        unsigned v = buf[i * 4] > buf[i * 4 + 1] ? buf[i * 4] : buf[i * 4 + 1];
        if (buf[i * 4 + 2] > v) v = buf[i * 4 + 2];
        buf[i * 4 + 3] = (uint8_t) v;
        if (v > mx) mx = v;
    }
    double t = ce / r2 - h / 2.0 - 1, cx = tx + t / r2, cy = ty + t / r2;
    for (int y = ty; y < ty + ce; y++) {
        if (y < 0 || y >= fb->h) continue;
        uint32_t *row = (uint32_t *) (fb->px + (long) y * fb->stride);
        for (int x = tx; x < tx + ce - (y - ty); x++) {
            if (x < 0 || x >= fb->w) continue;
            double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
            double u = (dx - dy) / r2 + w / 2.0 - 0.5, v = (dx + dy) / r2 + h / 2.0 - 0.5;
            if (u < -1 || v < -1 || u >= w || v >= h) continue;
            int u0 = (int) (u + 2) - 2, v0 = (int) (v + 2) - 2;   /* floor, for u, v >= -2 */
            double fu = u - u0, fv = v - v0, a = 0;
            for (int k = 0; k < 4; k++) {
                int su = u0 + (k & 1), sv = v0 + (k >> 1);
                if (su < 0 || sv < 0 || su >= w || sv >= h) continue;
                a += buf[(sv * CM_BT_W + su) * 4 + 3] * ((k & 1) ? fu : 1 - fu) * ((k >> 1) ? fv : 1 - fv);
            }
            unsigned al = (unsigned) (a * 255 / mx + 0.5);
            if (!al) continue;
            if (al > 255) al = 255;
            uint32_t d = row[x], o = 0;
            for (int sh = 0; sh <= 16; sh += 8) o |= ((((d >> sh) & 0xff) * (255 - al) + 255 * al) / 255) << sh;
            row[x] = o;
        }
    }
}
static void cm_corner_marks(const azmod_fb *fb, int tx, int ty, int tw, int dpx, const cm_cmod *m) {
    int ce = tw / 4, mg = ce / 14 + 1;
    if (!cm_repo_listed(m->repo)) {
        for (int r = 0; r < ce; r++) fill_rect(fb, tx + tw - ce + r, ty + r, ce - r, 1, C_HI);
        int isz = ce / 2 - mg;   /* its box's lower-left corner is on the diagonal: 2 * (mg + isz) = ce */
        if (isz >= 6) blit_icon(fb, tx + tw - mg - isz, ty + mg, isz, C_HI_TEXT, IC_REPOS);
    }
    if (m->beta) {
        for (int r = 0; r < ce; r++) fill_rect(fb, tx, ty + r, ce - r, 1, CM_BETA_BG);
        cm_beta_text(fb, tx, ty, ce, dpx);
    }
}
/* One mod's tile, in the grid and on a shelf: its icon, name, author, then its status (Installed,
 * Update available, ...) on the left and its download count on the right, when it has one. */
static void cm_tile(const azmod_fb *fb, int tx, int ty, int tw, int th, int ic, int lh, int pad, int dpx, const cm_cmod *m, int foc) {
    fill_rect(fb, tx, ty, tw, th, foc ? C_FOCUS_BAND : 0x00343434u);
    if (foc) c_focus_ring(fb, tx, ty, tw, th);
    cm_corner_marks(fb, tx, ty, tw, dpx, m);
    cm_icon(fb, tx + (tw - ic) / 2, ty + pad / 2, ic, m->id, m->name, m->icon);
    int yy = ty + pad / 2 + ic + pad / 3;
    cm_textw(fb, tx + pad / 2, yy, tw - pad, lh, m->name, dpx * 11 / 10, C_TEXT);
    cm_textw(fb, tx + pad / 2, yy + lh, tw - pad, lh, m->author, dpx, C_TEXT_DIM);
    const char *badge = NULL;
    uint32_t bc = 0;
    int ii = cm_installed_idx(m->id), pend = cm_pend_get(m->id);
    azcm_info_t im;
    int toff = pend == 0 || (pend < 0 && ii >= 0 && !azcm_info(ii, &im) && im.state != AZCM_LOADED);
    if (m->inst[0] && cm_removed(m->id)) {
        badge = "Removed";
        bc = C_TEXT_DIM;
    } else if (m->inst[0] && azcm_ver_newer(m->ver, m->inst) && cm_works(m) && cm_core_ok(m)) {
        badge = "Update available";
        bc = C_HI;
    } else if (m->inst[0] && azcm_ver_newer(m->ver, m->inst) && !cm_core_ok(m)) {
        badge = "Needs newer AZ-MODS";
        bc = C_AVAIL;
    } else if (m->inst[0] && toff) {
        badge = "Installed, disabled";
        bc = C_TEXT_DIM;
    } else if (m->inst[0]) {
        badge = "Installed";
        bc = C_OK;
    } else if (m->held[0]) {
        badge = "Its ID is in use";
        bc = C_AVAIL;
    } else if (!cm_works(m)) {
        badge = "Not for this deck";
        bc = C_AVAIL;
    } else if (!cm_core_ok(m)) {
        badge = "Needs newer AZ-MODS";
        bc = C_AVAIL;
    }
    int cw = 0;
    if (m->counted) {   /* no count: nothing, never 0 */
        char c[32];
        cmb_count_text(m->dl, c, sizeof c);
        int nw = cm_tw(c, dpx), mw = cm_dl_mark(fb, 0, 0, lh, 0, 0), cx = tx + tw - pad / 2 - nw;
        c_text(fb, cx, yy + 2 * lh, 0, lh, c, dpx, C_TEXT_DIM);
        cm_dl_mark(fb, cx - pad / 3 - mw, yy + 2 * lh, lh, C_TEXT_DIM, 1);
        cw = nw + mw + pad / 3 + pad / 2;
    }
    if (badge) cm_textw(fb, tx + pad / 2, yy + 2 * lh, tw - pad - cw, lh, badge, dpx, bc);
}
/* ---- Browse > Home: the shelves (cm_browse.h) ----------------------------------------
 * Each shown shelf is a title, "See all" and one row of the tiles that fit the grid's width (no
 * sideways scrolling: See all shows the rest). The knob scrolls the whole page, as on every tab
 * of the home page, and focuses nothing. */
/* Browse: the Sort button at the end of the view row: the order's icon and name, and a chevron (the
 * orders open under it, cm_layout_sortpop). Where it was laid out this pass, for the orders. */
static struct {
    int x, y, w, h, on;
} g_cm_sortr;
static void cm_sort_button(const azmod_fb *fb, int x, int y, int w, int h, int open, int foc) {
    draw_rect_outline(fb, x, y, w, h, 2, open ? C_HI : C_CHECK_BOX);
    if (foc) draw_rect_outline(fb, x - 5, y - 5, w + 10, h + 10, 4, C_TEXT);
    int px = menu_text_px(fb), isz = h * 75 / 100, csz = h * 50 / 100, m = h / 4;
    if (px > h * 62 / 100) px = h * 62 / 100;
    blit_icon(fb, x + m, y + (h - isz) / 2, isz, C_TEXT, IC_SORT);
    c_text(fb, x + m + isz + m / 2, y, 0, h, cmb_sort_label[g_cm_sort], px, C_TEXT);
    blit_icon(fb, x + w - m - csz, y + (h - csz) / 2, csz, open ? C_HI : C_TEXT_DIM, IC_CHEVRON_DOWN);
}
/* The Browse and Repositories tabs: the fetcher's progress while it works, then the tile grid or the repository list. */
static void cm_home_catalog(struct cm_lay *lay) {
    azmod_state *s = lay->s;
    const azmod_fb *fb = lay->fb;
    cm_ctl *ctl = lay->ctl;
    int draw = lay->draw;
    int bx = lay->bx;
    int bw = lay->bw;
    int rh = lay->rh;
    int lh = lay->lh;
    int px = lay->px;
    int dpx = lay->dpx;
    int pad = lay->pad;
    int bh3 = lay->bh3;
    int y = lay->y;
    int n = lay->n;
    if (cm_status_shown() && cm_progress_line()) {   /* Refresh is on the rail; what it is doing shows here */
        if (draw) cm_progress(fb, bx + pad, y, bw - 2 * pad, bh3, dpx);
        y += bh3 + pad;
    } else if (!g_ncc && !g_ncr) {
        if (draw) c_text(fb, bx + pad, y, 0, bh3, "Nothing loaded yet: connect the deck to WiFi, then open Community Mods again.", dpx, C_TEXT_DIM);
        y += bh3 + pad;
    }
    if (g_cm_tab == 1) {   /* the grid: 3 columns of tiles, 5 on the full screen */
        int cols = g_cm_full ? 5 : 3, gap = pad, tw = (bw - (cols - 1) * gap) / cols, ic = tw * 45 / 100, th = ic + 3 * lh + pad;
        if (!g_ncc && !cm_busy() && draw) {
            /* clang-format off */
            c_text(fb, bx + pad, y, 0, lh, g_ncr ? "No mods to show from your repositories yet."
                                                 : "No mod repositories yet. Add one under Repositories to see its mods here.", dpx, C_TEXT_DIM);
            /* clang-format on */
        }
        if (g_cc_more > 0) {   /* the catalog holds more than the menu shows: say so */
            if (draw) {
                char t[96];
                snprintf(t, sizeof t, "Showing the first %d of %d mods.", g_ncc, g_ncc + g_cc_more);
                c_text(fb, bx + pad, y, 0, lh, t, dpx, C_AVAIL);
            }
            y += lh + pad / 2;
        }
        /* the view row (Home, All mods, then the categories; Other only when it holds mods), then
         * Home's shelves or the mods the view shows, in the chosen order (cm_browse.h) */
        cmb_mod *bm = g_cm_bm;
        cm_browse_fill();
        g_cm_sortr.on = 0;
        if (g_ncc) {
            int cnt[CMB_NCAT + 1];
            cmb_counts(bm, g_ncc, cnt);
            /* the Sort button's place is kept on Home too (its shelves have their own orders), so
             * the view buttons never move between Home and the rest */
            int sw = fb->w * 180 / 1280, vw = bw - sw - pad;
            int other = cnt[CMB_OTHER] > 0 || g_cm_fcat == CMB_OTHER, nch = CMB_NCAT + 1 + other, cw = (vw - (nch - 1) * (pad / 2)) / nch;
            for (int c = CMB_HOME, k = 0; c < CMB_NCAT; c++) {
                if (c == CMB_OTHER && !other) continue;
                int qx = bx + k++ * (cw + pad / 2), nc = c == CMB_HOME ? 1 : cnt[c == CMB_ALL ? CMB_NCAT : c];
                if (draw) {
                    char t[32];
                    if (c == CMB_HOME) snprintf(t, sizeof t, "Home");
                    else snprintf(t, sizeof t, "%s  %d", c == CMB_ALL ? "All mods" : cmb_cat_label[c], nc);
                    cm_button(fb, qx, y, cw, bh3, t, g_cm_fcat == c, nc > 0 || g_cm_fcat == c, CM_FOC);
                }
                CM_CTL(CMC_FCAT, c, qx, y, cw, bh3);
            }
            if (g_cm_fcat != CMB_HOME) {
                int sx = bx + bw - sw;
                if (draw) cm_sort_button(fb, sx, y, sw, bh3, g_cm_sortpop, CM_FOC);
                CM_CTL(CMC_SORTBTN, 0, sx, y, sw, bh3);
                g_cm_sortr.x = sx, g_cm_sortr.y = y, g_cm_sortr.w = sw, g_cm_sortr.h = bh3, g_cm_sortr.on = 1;
            }
            y += bh3 + pad;
        }
        if (g_cm_fcat == CMB_HOME) {
            int tmp[CM_NCAT], tlh = lh * 3 / 2;
            cmb_shelf sh[CMB_NSHELF];
            int ns = cmb_shelves(bm, g_ncc, tmp, sh);
            if (g_ncc && !ns && draw)
                c_text(fb, bx + pad, y, 0, lh, "Nothing to show here yet. See All mods.", dpx, C_TEXT_DIM);
            for (int si = 0; si < ns; si++) {
                const cmb_shelf *h = &sh[si];
                int kind = h->kind, saw = cm_tw("See all  >", dpx) + pad, nf = h->n < cols ? h->n : cols;
                if (draw) {
                    c_text(fb, bx, y, 0, tlh, cmb_shelf_title(kind), dpx * 13 / 10, C_TEXT);
                    c_text(fb, bx + bw - saw + pad / 2, y, 0, tlh, "See all  >", dpx, C_HI);
                }
                CM_CTL(CMC_SEEALL, kind, bx + bw - saw, y, saw, tlh);
                y += tlh + pad / 3;
                for (int j = 0; j < nf; j++) {   /* the tiles that fit; See all shows the rest */
                    int i = h->idx[j], tx = bx + j * (tw + gap);
                    if (draw) cm_tile(fb, tx, y, tw, th, ic, lh, pad, dpx, &g_cc[i], 0);
                    CM_CTL(CMC_STILE, kind << 8 | i, tx, y, tw, th);
                }
                y += th + pad;
            }
            y += g_ncc ? lh : 0;
        } else {
            int order[CM_NCAT], no = cmb_view(bm, g_ncc, g_cm_fcat, g_cm_sort, order);
            if (g_ncc && !no && draw)
                c_text(fb, bx + pad, y, 0, lh, "No mods in this category yet.", dpx, C_TEXT_DIM);
            for (int o = 0; o < no; o++) {
                int i = order[o];
                int c = o % cols, tx = bx + c * (tw + gap), ty = y + (o / cols) * (th + gap), foc = CM_FOC;
                if (draw) cm_tile(fb, tx, ty, tw, th, ic, lh, pad, dpx, &g_cc[i], foc);
                CM_CTL(CMC_TILE, i, tx, ty, tw, th);
            }
            y += ((no + cols - 1) / cols) * (th + gap) + (g_ncc && !no ? lh : 0);
        }
    } else {   /* repositories: Add first so the list growing never moves it */
        int abw = fb->w * 220 / 1280;
        if (draw) cm_button(fb, bx, y, abw, bh3, "Add repository", 1, !cm_busy(), CM_FOC);
        CM_CTL(CMC_ADDREPO, 0, bx, y, abw, bh3);
        if (draw) c_text(fb, bx + abw + 2 * pad, y, 0, bh3, "A repository is a list of mods someone publishes. Add one by its address.", dpx, C_TEXT_DIM);
        y += bh3 + pad;
        for (int i = 0; i < g_ncr; i++) {
            cm_crepo *r = &g_cr[i];
            int h = rh + lh - fb->h * 6 / 800, rbw = fb->w * 120 / 1280;
            if (draw) {
                fill_rect(fb, bx, y, bw, h, 0x00343434u);
                c_text(fb, bx + pad, y + fb->h * 4 / 800, 0, rh - fb->h * 8 / 800, r->name[0] ? r->name : r->repo, px, C_TEXT);
                /* clang-format off */
                char t[240]; snprintf(t, sizeof t, "%s   %s", r->repo, r->ok ? (r->listed ? "Mod Index" : "Added by you") : r->err[0] ? r->err : "Could not be read");
                /* clang-format on */
                cm_textw(fb, bx + pad, y + rh - fb->h * 10 / 800, bw - 3 * pad - (r->listed ? 0 : rbw), lh, t, dpx, r->ok ? C_TEXT_DIM : CM_BAD);
            }
            if (!r->listed) {
                if (draw) cm_button(fb, bx + bw - pad - rbw, y + (h - bh3) / 2, rbw, bh3, "Remove", 0, !cm_busy(), CM_FOC);
                CM_CTL(CMC_DELREPO, i, bx + bw - pad - rbw, y + (h - bh3) / 2, rbw, bh3);
            }
            y += h + fb->h * 6 / 800;
        }
        if (g_cr_more > 0) {
            if (draw) {
                char t[96];
                snprintf(t, sizeof t, "Showing the first %d of %d repositories.", g_ncr, g_ncr + g_cr_more);
                c_text(fb, bx + pad, y, 0, lh, t, dpx, C_AVAIL);
            }
            y += lh;
        }
        y += pad;
    }
    lay->y = y;
    lay->n = n;
}

/* The Updates tab: every installed mod with an update this deck can take, Update all above them.
 * A row: the mod's icon and name, installed > offered version, and Update (the mod page's). */
static void cm_home_updates(struct cm_lay *lay) {
    azmod_state *s = lay->s;
    const azmod_fb *fb = lay->fb;
    cm_ctl *ctl = lay->ctl;
    int draw = lay->draw;
    int bx = lay->bx;
    int bw = lay->bw;
    int rh = lay->rh;
    int lh = lay->lh;
    int px = lay->px;
    int dpx = lay->dpx;
    int pad = lay->pad;
    int bh3 = lay->bh3;
    int y = lay->y;
    int n = lay->n;
    int idx[CM_NCAT], nu = cm_upd_list(idx);
    if (cm_status_shown() && cm_progress_line()) {   /* the install in progress, or how the last one ended */
        if (draw) cm_progress(fb, bx + pad, y, bw - 2 * pad, bh3, dpx);
        y += bh3 + pad;
    }
    if (g_cm_ua.result == CMU_QUIET) {
        if (draw) c_text(fb, bx + pad, y, 0, bh3, "Update all stopped: the installer did not answer.", dpx, CM_BAD);
        y += bh3 + pad;
    }
    if (nu || g_cm_ua.on) {
        char t[48];
        cmu_label(&g_cm_ua, nu, t, sizeof t);
        int w = cm_tw(t, menu_text_px(fb)) + 2 * pad;
        if (w < fb->w * 220 / 1280) w = fb->w * 220 / 1280;
        if (draw) cm_button(fb, bx, y, w, bh3, t, 1, !cm_busy(), CM_FOC || CM_HF(CMC_UPDALL, 0));
        CM_CTL(CMC_UPDALL, 0, bx, y, w, bh3);
        y += bh3 + pad;
    } else {
        if (draw) c_text(fb, bx + pad, y, 0, rh, "All your community mods are up to date.", dpx, C_TEXT_DIM);
        y += rh;
    }
    for (int j = 0; j < nu; j++) {
        const cm_cmod *c = &g_cc[idx[j]];
        int h = rh + lh - fb->h * 6 / 800, ic = h * 70 / 100, tx = bx + pad + ic + pad, ubw = fb->w * 140 / 1280;
        int cur = g_cm_ua.on && !strcmp(cmu_cur(&g_cm_ua), c->id);
        if (draw) {
            fill_rect(fb, bx, y, bw, h, 0x00343434u);
            cm_icon(fb, bx + pad, y + (h - ic) / 2, ic, c->id, c->name, c->icon);
            cm_textw(fb, tx, y + fb->h * 4 / 800, bw - (tx - bx) - ubw - 2 * pad, rh - fb->h * 8 / 800, c->name, px, C_TEXT);
            char v[64];
            snprintf(v, sizeof v, "%s > %s", c->inst, c->ver);
            c_text(fb, tx, y + rh - fb->h * 10 / 800, 0, lh, v, dpx, C_TEXT_DIM);
            cm_button(fb, bx + bw - pad - ubw, y + (h - bh3) / 2, ubw, bh3, cur ? "Updating..." : "Update", 0, !cm_busy(),
                      CM_FOC || CM_HF(CMC_UPDROW, idx[j]));
        }
        CM_CTL(CMC_UPDROW, idx[j], bx + bw - pad - ubw, y + (h - bh3) / 2, ubw, bh3);
        y += h + fb->h * 6 / 800;
    }
    lay->y = y;
    lay->n = n;
}

/* The home page: the tab the rail shows (cm_layout_rail). */
static void cm_layout_home(struct cm_lay *lay) {
    const azmod_fb *fb = lay->fb;
    int draw = lay->draw;
    int bx = lay->bx;
    int bw = lay->bw;
    int lh = lay->lh;
    int dpx = lay->dpx;
    int pad = lay->pad;
    int y = lay->y;
    int n = lay->n;
    {   /* a mod turned off on its own after a crash: say so first */
        int nm = azcm_count(), nc = 0;
        char who[48] = "";
        for (int i = 0; i < nm; i++) {
            azcm_info_t m;
            if (!azcm_info(i, &m) && cm_crashed(&m) && !(i < 16 && g_cm_unsusp[i]) && !cm_removed(m.id)) {
                if (!nc) snprintf(who, sizeof who, "%s", m.name);
                nc++;
            }
        }
        if (nc) {
            char t[200];
            if (nc == 1) snprintf(t, sizeof t, "%s stopped the deck, so it was disabled. Your other mods were not affected.", who);
            else snprintf(t, sizeof t, "%d mods stopped the deck, so they were disabled. Your other mods were not affected.", nc);
            if (draw) { cm_textw(fb, bx + pad, y, bw - 2 * pad, lh, t, dpx, CM_BAD); }
            y += lh;
            if (draw) c_text(fb, bx + pad, y, 0, lh, "Open it under Installed to try it again or remove it.", dpx, C_TEXT_DIM);
            y += lh + fb->h * 6 / 800;
        }
    }
    if (cm_master_stick()) {   /* off by the stick's flag: say why, and where they turn on again */
        if (draw) c_text(fb, bx + pad, y, 0, lh, "Community mods are disabled on this USB: enable them in the AZ-MODS app.", dpx, C_TEXT_DIM);
        y += lh + fb->h * 8 / 800;
    }
    if (g_cm_tab == 0) {
        lay->y = y;
        lay->n = n;
        cm_home_installed(lay);
        y = lay->y;
        n = lay->n;
    } else if (g_cm_tab == 3) {
        lay->y = y;
        lay->n = n;
        cm_home_updates(lay);
        y = lay->y;
        n = lay->n;
    } else {
        lay->y = y;
        lay->n = n;
        cm_home_catalog(lay);
        y = lay->y;
        n = lay->n;
    }
    lay->y = y;
    lay->n = n;
}

/* An installed mod's page. */
static int cm_layout_mod(struct cm_lay *lay) {
    azmod_state *s = lay->s;
    const azmod_fb *fb = lay->fb;
    cm_ctl *ctl = lay->ctl;
    int draw = lay->draw;
    int bx = lay->bx;
    int by = lay->by;
    int bw = lay->bw;
    int bh = lay->bh;
    int lh = lay->lh;
    int dpx = lay->dpx;
    int pad = lay->pad;
    int bh3 = lay->bh3;
    int y = lay->y;
    int n = lay->n;
    azcm_info_t m;
    if (azcm_info(g_cm_page, &m)) {
        g_cm_view = CMV_HOME;
        return 0;
    }
    char t[200];
    if (draw) {
        if (m.author[0]) snprintf(t, sizeof t, "v%s  by %s", m.version, m.author);
        else snprintf(t, sizeof t, "%s", m.version[0] ? m.version : m.id);
        c_text(fb, bx + pad, y, 0, lh, t, dpx, C_TEXT_DIM);
        char st[160];
        uint32_t sc;
        const char *stt = cm_state_text(&m, st, sizeof st, &sc);
        if (!(cm_crashed(&m) && cm_pend_get(m.id) != 1)) c_text(fb, bx + pad, y + lh, 0, lh, stt, dpx, sc);
        if (m.hooks) c_text(fb, bx + pad, y + 2 * lh, 0, lh, "Changes the deck software itself (uses hooks).", dpx, C_TEXT_DIM);
    }
    y += (m.hooks ? 3 : 2) * lh;
    y += cm_wrap(fb, bx + pad, y, bw - 2 * pad, lh, m.summary, dpx, C_TEXT, g_cm_full ? 2 : 1, draw) * lh + pad / 2;
    if (m.state == AZCM_NEW && !cm_removed(m.id)) {
        /* found turned on on the USB, but this deck has not approved this exact mod file: put
         * there by the AZ-MODS app, copied by hand, or a different build of a mod approved before */
        char t[320];
        if (m.from[0]) snprintf(t, sizeof t, "Enabled on this USB, from %s, but not yet allowed on this deck. It runs with full access to your deck: allow it only if you trust where it came from.", m.from);
        else snprintf(t, sizeof t, "Enabled on this USB, copied there by hand (no record of where it came from), but not yet allowed on this deck. It runs with full access to your deck: allow it only if you trust it.");
        y += cm_wrap(fb, bx + pad, y, bw - 2 * pad, lh, t, dpx, C_TEXT, 3, draw) * lh + pad / 2;
        int bw2 = fb->w * 200 / 1280, x = bx + pad, busy = cm_busy();
        if (!m.allowed) {
            if (draw) cm_button(fb, x, y, bw2, bh3, "Allow", 1, 1, CM_FOC);
            CM_CTL(CMC_ALLOW, g_cm_page, x, y, bw2, bh3);
            x += bw2 + pad;
        }
        if (draw) cm_button(fb, x, y, bw2, bh3, "Remove from USB", 0, !busy, CM_FOC);
        CM_CTL(CMC_MODACT, CMA_REMOVE, x, y, bw2, bh3);
        y += bh3 + pad;
        if (g_cm_allow_err == g_cm_page) {
            if (draw) c_text(fb, bx + pad, y, 0, lh, "Could not save that on the deck. Try again.", dpx, CM_BAD);
            y += lh + pad / 2;
        }
        if (draw && cm_status_shown() && g_cs.id[0] && !strcmp(g_cs.id, m.id)) cm_progress(fb, bx + pad, y, bw - 2 * pad, lh, dpx);
    } else {   /* the actions, on every installed page: Try again (after a crash) or Enable / Disable, and Remove */
        int bw2 = fb->w * 200 / 1280, x = bx + pad, busy = cm_busy(), gone = cm_removed(m.id);
        int crashed = cm_crashed(&m), tried = (g_cm_page < 16 && g_cm_unsusp[g_cm_page]) || cm_pend_get(m.id) == 1;
        if (crashed && draw && !tried) {
            char t[200];
            snprintf(t, sizeof t, "%s %s, so it was disabled. Your other mods were not affected.", m.name,
                     strstr(m.reason, "while this mod was starting") ? "stopped the deck while it was starting" : "crashed the deck");
            cm_wrap(fb, bx + pad, y, bw - 2 * pad, lh, t, dpx, CM_BAD, 2, 1);
        }
        if (crashed && !tried) y += 2 * lh + pad / 2;
        if (!gone) {
            int up = cm_update_idx(m.id);
            if (up >= 0) {
                char ul[40];
                snprintf(ul, sizeof ul, "Update to %s", g_cc[up].ver);
                if (draw) cm_button(fb, x, y, bw2, bh3, ul, 1, !busy, CM_FOC);
                CM_CTL(CMC_MODUPD, up, x, y, bw2, bh3);
                x += bw2 + pad;
            }
            if (crashed && !tried) {
                if (draw) cm_button(fb, x, y, bw2, bh3, "Try again", 1, 1, CM_FOC);
                CM_CTL(CMC_UNSUSPEND, g_cm_page, x, y, bw2, bh3);
                x += bw2 + pad;
            } else if (crashed) {
                /* Try again pressed: the state line above says "Loads again after Reload." */
            } else if (!azcm_suspended()) {
                int pend = cm_pend_get(m.id), on = pend >= 0 ? pend : cm_turned_on(&m);
                if (draw) cm_button(fb, x, y, bw2, bh3, on ? "Disable" : "Enable", 0, !busy, CM_FOC);
                CM_CTL(CMC_MODACT, on ? CMA_DISABLE : CMA_ENABLE, x, y, bw2, bh3);
                x += bw2 + pad;
            }
            if (draw) cm_button(fb, x, y, bw2, bh3, "Remove from USB", 0, !busy, CM_FOC);
            CM_CTL(CMC_MODACT, CMA_REMOVE, x, y, bw2, bh3);
        } else if (draw) c_text(fb, x, y, 0, bh3, "Removed. Unloads after Reload.", dpx, C_AVAIL);
        y += bh3 + pad;
        if (draw && cm_status_shown() && g_cs.id[0] && !strcmp(g_cs.id, m.id)) cm_progress(fb, bx + pad, y, bw - 2 * pad, lh, dpx);
        if (cm_status_shown() && g_cs.id[0] && !strcmp(g_cs.id, m.id)) y += lh + pad / 2;
    }
    if (m.state == AZCM_LOADED && m.has_screen) {   /* the mod's own screen, under its info */
        if (draw) fill_rect(fb, bx, y, bw, 1, C_DIVIDER);
        y += pad / 2;
        az_rect a = {bx, y, bw, by + bh - y};
        g_cm_area = a;
        if (draw) {
            az_draw d;
            azcm_draw_init(&d, fb, a);
            azcm_screen_draw(g_cm_page, &d, a);
        }
    } else if (m.state == AZCM_LOADED && draw) c_text(fb, bx + pad, y, 0, lh, "This mod has no settings page.", dpx, C_TEXT_DIM);
    lay->y = y;
    lay->n = n;
    return 1;
}

/* A catalog mod's page. */
/* the open page's screenshots: which are on disk (re-checked only when the page or g_cm_gen
 * changes), and one `shots` request per visit for the missing ones, sent once the fetcher is free */
static void cm_shots_sync(const cm_cmod *m) {
    if (g_cm_sh.cat != g_cm_cat || g_cm_sh.gen != g_cm_gen) {
        if (g_cm_sh.cat != g_cm_cat) g_cm_sh.asked = 0;
        g_cm_sh.cat = g_cm_cat;
        g_cm_sh.gen = g_cm_gen;
        for (int i = 0; i < CM_NSHOT; i++) g_cm_sh.have[i] = i < m->nshots && access(m->shots[i], R_OK) == 0;
    }
    int missing = 0;
    for (int i = 0; i < m->nshots; i++) missing |= !g_cm_sh.have[i];
    if (missing && !g_cm_sh.asked && !cm_busy()) {
        cm_req("shots %s %s", m->repo, m->id);
        g_cm_sh.asked = 1;
        g_cm_sh.askgen = g_cm_gen;
    }
}
/* a screenshot to show: on disk, or (while its download may still come) a place kept for it */
static int cm_shot_slot(const cm_cmod *m, int i) {
    return i < m->nshots && (g_cm_sh.have[i] || !g_cm_sh.asked || cm_busy() || g_cm_gen == g_cm_sh.askgen);
}
static int cm_layout_cat(struct cm_lay *lay) {
    azmod_state *s = lay->s;
    const azmod_fb *fb = lay->fb;
    cm_ctl *ctl = lay->ctl;
    int draw = lay->draw;
    int bx = lay->bx;
    int by = lay->by;
    int bw = lay->bw;
    int bh = lay->bh;
    int rh = lay->rh;
    int lh = lay->lh;
    int dpx = lay->dpx;
    int pad = lay->pad;
    int bh3 = lay->bh3;
    int y = lay->y;
    int n = lay->n;
    if (g_cm_cat < 0 || g_cm_cat >= g_ncc) {
        g_cm_view = CMV_HOME;
        return 0;
    }
    cm_cmod *m = &g_cc[g_cm_cat];
    cm_shots_sync(m);
    if (g_cm_zoom >= 0 && (g_cm_zoom >= m->nshots || !g_cm_sh.have[g_cm_zoom])) g_cm_zoom = -1;
    if (g_cm_zoom >= 0) {   /* one screenshot over the page: the sides step through them, the middle closes it */
        int zw = bw / 5, nh = 0, at = 0;
        for (int i = 0; i < m->nshots; i++)
            if (g_cm_sh.have[i]) at += i < g_cm_zoom, nh++;
        if (draw) {
            int ow = 0, oh = 0, pend = 0;
            fill_rect(fb, bx, by, bw, bh, 0x00101010u);
            const uint32_t *px = azcm_shot(m->shots[g_cm_zoom], bw - 2 * pad, bh - 2 * lh, &ow, &oh, &pend);
            if (px) cm_blit_argb(fb, bx + (bw - ow) / 2, by + pad / 2 + (bh - 2 * lh - oh) / 2, ow, oh, px);
            char t[48];
            if (nh > 1) snprintf(t, sizeof t, "<   %d of %d   >", at + 1, nh);
            else snprintf(t, sizeof t, "Tap to close");
            c_text(fb, bx, by + bh - lh - pad / 2, bw, lh, t, dpx, C_TEXT_DIM);
        }
        if (nh > 1) {
            CM_CTL(CMC_SHOTZ, 1, bx, by, zw, bh);
            CM_CTL(CMC_SHOTZ, 2, bx + bw - zw, by, zw, bh);
        }
        CM_CTL(CMC_SHOTZ, 0, bx + (nh > 1 ? zw : 0), by, bw - (nh > 1 ? 2 * zw : 0), bh);
        lay->y = by + bh;
        lay->n = n;
        return 1;
    }
    int ic = g_cm_full ? rh * 3 : rh * 2;
    int works = cm_works(m), core = cm_core_ok(m), ii = cm_installed_idx(m->id);
    int tx = bx + 2 * pad + ic, tw = bx + bw - pad - tx, ly = y;
    char t[240];
    /* the header: the icon, then the name, "author  .  category  .  vX, updated N days ago", the
     * download count (none: no line) and the repository it comes from */
    if (draw) {
        cm_icon(fb, bx + pad, y, ic, m->id, m->name, m->icon);
        cm_textw(fb, tx, y, tw, rh, m->name, lay->px * 13 / 10, C_TEXT);
    }
    ly += rh;
    {
        char up[48];
        int k = 0;
        cmb_updated_text(m->upd, CM_NOW(), up, sizeof up);
        t[0] = 0;
        if (m->author[0]) k = snprintf(t, sizeof t, "%s", m->author);
        if (m->cat[0] && k >= 0 && (size_t) k < sizeof t)
            k += snprintf(t + k, sizeof t - (size_t) k, "%s%s", k ? CM_DOT : "", cmb_cat_label[cmb_cat_of(m->cat)]);
        if (k >= 0 && (size_t) k < sizeof t) snprintf(t + k, sizeof t - (size_t) k, "%sv%s%s%s", k ? CM_DOT : "", m->ver, up[0] ? ", " : "", up);
        if (draw) cm_textw(fb, tx, ly, tw, lh, t, dpx, C_TEXT_DIM);
        ly += lh;
    }
    if (m->counted) {
        char c[32];
        cmb_count_text(m->dl, c, sizeof c);
        snprintf(t, sizeof t, "%s %s", c, m->dl == 1 ? "download" : "downloads");
        if (draw) {
            int mw = cm_dl_mark(fb, tx, ly, lh, C_TEXT, 1);
            c_text(fb, tx + mw + pad / 2, ly, 0, lh, t, dpx, C_TEXT);
        }
        ly += lh;
    }
    /* where it comes from; a repository the user added says it is not reviewed for the Mod Index */
    snprintf(t, sizeof t, "from %s%s", m->repo, cm_repo_listed(m->repo) ? "" : CM_DOT CM_UNLISTED);
    if (draw) cm_textw(fb, tx, ly, tw, lh, t, dpx, C_TEXT_DIM);
    ly += lh;
    /* the buttons along the icon's bottom edge (under the header when it runs longer), each as
     * wide as its label needs */
    int busy = cm_busy(), x = tx, yb = y + ic - bh3 > ly + pad / 2 ? y + ic - bh3 : ly + pad / 2, bpx = menu_text_px(fb), ubx = -1, ubw = 0;
    if (bpx > bh3 * 62 / 100) bpx = bh3 * 62 / 100;
#define CM_BTN(lbl, act, prim, act_ok)                                                 \
    do {                                                                               \
        int w_ = cm_tw(lbl, bpx) + 2 * pad;                                            \
        if (w_ < fb->w * 140 / 1280) w_ = fb->w * 140 / 1280;                          \
        if (draw) cm_button(fb, x, yb, w_, bh3, lbl, prim, (act_ok) && !busy, CM_FOC); \
        CM_CTL(CMC_ACT, act, x, yb, w_, bh3);                                          \
        if ((act) == CMA_INSTALL || (act) == CMA_UPDATE) ubx = x, ubw = w_;            \
        x += w_ + pad;                                                                 \
    } while (0)
    if (!m->inst[0]) CM_BTN("Install", CMA_INSTALL, 1, works && core && !m->held[0]);
    else {
        if (azcm_ver_newer(m->ver, m->inst)) {
            char ul[48];
            snprintf(ul, sizeof ul, "Update to %s", m->ver);
            CM_BTN(ul, CMA_UPDATE, 1, works && core);
        }
        azcm_info_t im;
        int pend = cm_pend_get(m->id);
        int off = pend >= 0 ? !pend : ii >= 0 && !azcm_info(ii, &im) && im.state == AZCM_DISABLED;
        if (!cm_removed(m->id)) {
            CM_BTN(off ? "Enable" : "Disable", off ? CMA_ENABLE : CMA_DISABLE, 0, 1);
            CM_BTN("Remove", CMA_REMOVE, 0, 1);
        }
    }
#undef CM_BTN
    if (m->inst[0] && draw) {
        snprintf(t, sizeof t, "Installed: v%s", m->inst);
        c_text(fb, x, yb, 0, bh3, t, dpx, C_OK);
    }
    /* its download: the bar right under the Install / Update button pressed, as wide as it */
    int mine = g_cs.id[0] && !strcmp(g_cs.id, m->id), bar = mine && ubx >= 0 && !strcmp(g_cs.state, "downloading") && g_cs.total > 0;
    if (bar && draw) {
        int by2 = yb + bh3 + pad / 4, bh2 = pad / 3 > 2 ? pad / 3 : 2;
        long done = g_cs.done > g_cs.total ? g_cs.total : g_cs.done;
        draw_rect_outline(fb, ubx, by2, ubw, bh2 + 2, 1, C_CHECK_BOX);
        fill_rect(fb, ubx + 1, by2 + 1, (int) ((ubw - 2) * done / g_cs.total), bh2, C_HI);
    }
    y = (yb + bh3 > y + ic ? yb + bh3 : y + ic) + pad / 2 + (bar ? pad / 2 : 0);
    /* this mod's status line, on a line of its own taken only while there is something to say */
    if (cm_status_shown() && mine) {
        g_cm_nobar = bar;
        if (draw) cm_progress(fb, bx + pad, y, bw - 2 * pad, lh, dpx);
        g_cm_nobar = 0;
        y += lh + pad / 2;
    }
    {   /* the summary (wrapped), then what decides whether it installs here: one per line */
        int nl = cm_wrap(fb, bx + pad, y, bw - 2 * pad, lh, m->summary, dpx, C_TEXT, g_cm_full ? 3 : 2, draw), k = 0;
        int ty = y + nl * lh;
        if (!works || !core) {   /* only what stops it here gets a line */
            if (draw) {
                char wt[80];
                if (!works) snprintf(wt, sizeof wt, "Not built for this deck's firmware.");
                else snprintf(wt, sizeof wt, "Needs AZ-MODS %s or newer (this deck has %s).", m->mc, AZ_CORE_VERSION);
                c_text(fb, bx + pad, ty + k * lh, 0, lh, wt, dpx, CM_BAD);
            }
            k++;
        }
        if (m->beta) {   /* its author marks it as not proven stable yet */
            if (draw) cm_textw(fb, bx + pad, ty + k * lh, bw - 2 * pad, lh, "BETA: This mod may not be stable yet.", dpx, CM_BETA_BG);
            k++;
        }
        if (m->held[0]) {   /* another mod already has this id on the stick: it must go first */
            if (!strcmp(m->held, "-")) snprintf(t, sizeof t, "%s", CM_HELD_HAND);
            else snprintf(t, sizeof t, CM_HELD_FROM, m->held);
            if (draw) cm_textw(fb, bx + pad, ty + k * lh, bw - 2 * pad, lh, t, dpx, CM_BAD);
            k++;
        }
        if (m->nalso > 0 && m->also[0]) {   /* others offer the same id: only one can be installed */
            if (m->nalso > 1) snprintf(t, sizeof t, "%s and %d more also offer a mod with this ID. Only one can be installed.", m->also, m->nalso - 1);
            else snprintf(t, sizeof t, "%s also offers a mod with this ID. Only one can be installed.", m->also);
            if (draw) cm_textw(fb, bx + pad, ty + k * lh, bw - 2 * pad, lh, t, dpx, C_TEXT_DIM);
            k++;
        }
        y = ty + k * lh + pad / 2;
    }
    {   /* the screenshots, in a row; one tapped opens large */
        int sh = g_cm_full ? fb->h * 150 / 800 : fb->h * 96 / 800, sw = sh * 16 / 10, sx = bx + pad, any = 0;
        for (int i = 0; i < m->nshots; i++) {
            if (!cm_shot_slot(m, i)) continue;
            if (draw) {
                int ow = 0, oh = 0, pend = 0;
                const uint32_t *px = g_cm_sh.have[i] ? azcm_shot(m->shots[i], sw, sh, &ow, &oh, &pend) : NULL;
                fill_rect(fb, sx, y, sw, sh, C_ROW_BG);
                if (px) cm_blit_argb(fb, sx + (sw - ow) / 2, y + (sh - oh) / 2, ow, oh, px);
            }
            if (g_cm_sh.have[i]) CM_CTL(CMC_SHOT, i, sx, y, sw, sh);
            sx += sw + pad;
            any = 1;
        }
        if (any) y += sh + pad;
    }
    /* the body: the description and Permissions (what it touches, its firmware, its size); on the
     * full screen Permissions stands beside the description, else above it */
    int dw = bw - 2 * pad;
    {
        int rx = bx + pad, ry = y, k = 0;
        if (g_cm_full) {
            dw = (bw - 2 * pad) * 64 / 100;
            rx = bx + pad + dw + 2 * pad;
        }
        int rw = bx + bw - pad - rx;
        if (g_cm_full) {
            if (draw) c_text(fb, rx, ry, 0, lh, "Permissions", dpx * 12 / 10, C_TEXT);
            k++;
            if (draw) cm_textw(fb, rx, ry + k * lh, rw, lh, m->hooks ? "Changes the deck software itself (uses hooks)" : "Uses only AZ-MODS features (no hooks)", dpx, C_TEXT_DIM);
            k++;
            snprintf(t, sizeof t, "Firmware: %s", m->fw[0] ? m->fw : "?");
            if (draw) cm_textw(fb, rx, ry + k * lh, rw, lh, t, dpx, C_TEXT_DIM);
            k++;
        } else if (m->hooks) {
            if (draw) c_text(fb, rx, ry + k * lh, 0, lh, "Changes the deck software itself (uses hooks).", dpx, C_TEXT_DIM);
            k++;
        }
        if (m->size > 0) {
            snprintf(t, sizeof t, "Download: %ld KB", (m->size + 1023) / 1024);
            if (draw) cm_textw(fb, rx, ry + k * lh, rw, lh, t, dpx, C_TEXT_DIM);
            k++;
        }
        if (m->assets_kb > 0) {
            if (m->assets_kb >= 1024) snprintf(t, sizeof t, "Keeps %.1f MB of files on your USB stick.", m->assets_kb / 1024.0);
            else snprintf(t, sizeof t, "Keeps %d KB of files on your USB stick.", m->assets_kb);
            if (draw) cm_textw(fb, rx, ry + k * lh, rw, lh, t, dpx, C_TEXT_DIM);
            k++;
        }
        if (!g_cm_full) y += k * lh + (k ? pad / 2 : 0);
    }
    g_cm_desc.r.h = 0;
    if (g_cm_desc.cat != g_cm_cat) {
        g_cm_desc.cat = g_cm_cat;
        g_cm_desc.skip = 0;
    }
    if (m->desc[0]) {   /* the author's longer description, in the space that is left; drag to scroll */
        int vis = (by + bh - y) / lh, tw2 = dw;
        if (vis > 0) {
            int n = cm_wrap_from(fb, 0, 0, tw2, lh, m->desc, dpx, 0, 0, 0, 0), sbw = 0;
            if (n > vis) {   /* it does not fit: leave room for the scroll bar and wrap again */
                sbw = fb->w / 160;
                tw2 -= sbw + pad / 2;
                n = cm_wrap_from(fb, 0, 0, tw2, lh, m->desc, dpx, 0, 0, 0, 0);
            }
            int max = n > vis ? n - vis : 0;
            if (g_cm_desc.skip > max) g_cm_desc.skip = max;
            if (g_cm_desc.skip < 0) g_cm_desc.skip = 0;
            cm_wrap_from(fb, bx + pad, y, tw2, lh, m->desc, dpx, C_TEXT, g_cm_desc.skip, vis, draw);
            if (max) {
                az_rect r = {bx + pad, y, dw, vis * lh};
                g_cm_desc.r = r;
                g_cm_desc.lh = lh;
                g_cm_desc.max = max;
                if (draw) {   /* the description's scroll bar: track and thumb */
                    int sx = bx + pad + dw - sbw, th = r.h * vis / n, ty = r.y + (r.h - th) * g_cm_desc.skip / max;
                    fill_rect(fb, sx, r.y, sbw, r.h, C_ROW_BG);
                    fill_rect(fb, sx, ty, sbw, th, C_CHECK_BOX);
                }
            }
        }
    }
    lay->y = y;
    lay->n = n;
    return 1;
}

/* the Installed tab, full screen, with something installed: the list and the page side by side */
static int cm_isplit(void) { return g_cm_full && g_cm_view == CMV_HOME && g_cm_tab == 0; }
/* the right half of the split: the selected row's page, at the top of the box (it does not scroll
 * with the list), as the Mod Menu shows a row's page beside its list. A loaded mod gets its page
 * (cm_layout_mod: info, buttons, its own screen), a mod installed since this start its catalog
 * page. Entering a mod's screen happens here, once per selection. */
static void cm_layout_split(struct cm_lay *lay) {
    if (g_cm_isel < 0) return;
    const azmod_fb *fb = lay->fb;
    int kind = g_cm_isel >> 16, idx = g_cm_isel & 0xffff, half = lay->bw / 2;
    struct cm_lay r = *lay;
    r.bx = lay->bx + half + lay->pad;
    r.bw = lay->bw - half - lay->pad;
    r.y = r.by;
    if (lay->draw) fill_rect(fb, r.bx - lay->pad / 2, r.by, 1, r.bh, C_DIVIDER);
    if (kind == CMC_MOD) {
        g_cm_page = idx;
        azcm_info_t m;
        if (!azcm_info(idx, &m)) {   /* its name on top: the full page has it in the header instead */
            if (lay->draw) cm_textw(fb, r.bx + r.pad, r.y, r.bw - 2 * r.pad, r.rh, m.name, r.px * 13 / 10, C_TEXT);
            r.y += r.rh;
        }
        if (g_cm_ient != g_cm_isel && !azcm_info(idx, &m) && m.state == AZCM_LOADED && m.has_screen) {
            g_cm_ient = g_cm_isel;
            azcm_screen_enter(idx);
        }
        cm_layout_mod(&r);
        g_cm_view = CMV_HOME;   /* cm_layout_mod returns to home when its mod is gone; this view stays home */
    } else if (kind == CMC_INSTCAT) {
        g_cm_cat = idx;
        cm_layout_cat(&r);
        g_cm_view = CMV_HOME;
    }
    lay->n = r.n;
}
/* the selected mod has its own screen: the knob and taps in its area go to it */
static int cm_split_screen(void) {
    azcm_info_t m;
    return cm_isplit() && g_cm_isel >= 0 && (g_cm_isel >> 16) == CMC_MOD && !azcm_info(g_cm_isel & 0xffff, &m) &&
           m.state == AZCM_LOADED && m.has_screen;
}
/* a row chosen (a tap, or the knob moving onto it): its page on the right; the page it leaves
 * keeps its settings (written now, as leaving a mod's page does) */
static void cm_split_select(int code) {
    if (code == g_cm_isel) return;
    azcm_flush_settings();
    g_cm_isel = code;
    g_cm_in = 0;
    g_cm_rsel = -1;
    g_cm_zoom = -1;
}

/* The one-time trust question. */
static void cm_layout_trust(struct cm_lay *lay) {
    azmod_state *s = lay->s;
    const azmod_fb *fb = lay->fb;
    cm_ctl *ctl = lay->ctl;
    int draw = lay->draw;
    int bx = lay->bx;
    int lh = lay->lh;
    int dpx = lay->dpx;
    int pad = lay->pad;
    int bh3 = lay->bh3;
    int y = lay->y;
    int n = lay->n;
    static const char *L[] = {"", "",
                              "Community mods are made by other developers, not by AZ-MODS.",
                              "AZ-MODS does not check them. They run inside the deck software",
                              "with full access to your deck, and a faulty one can stop the music.",
                              "", "Only install mods whose source you trust. You can turn every",
                              "community mod off at any time under Community Mods."};
    char first[120];
    snprintf(first, sizeof first, "Before you install %s, your first community mod:",
             g_cm_cat >= 0 && g_cm_cat < g_ncc ? g_cc[g_cm_cat].name : "it");
    for (int i = 0; i < (int) (sizeof L / sizeof *L); i++)
        if (draw) c_text(fb, bx + pad, y + i * lh, 0, lh, i ? L[i] : first, dpx, i ? C_TEXT : C_HI);
    y += 9 * lh;
    int bw2 = fb->w * 220 / 1280;
    if (draw) cm_button(fb, bx + pad, y, bw2, bh3, "I understand, install", 1, 1, CM_FOC);
    CM_CTL(CMC_TRUST_OK, 0, bx + pad, y, bw2, bh3);
    if (draw) cm_button(fb, bx + 2 * pad + bw2, y, bw2 / 2, bh3, "Cancel", 0, 1, CM_FOC);
    CM_CTL(CMC_TRUST_NO, 0, bx + 2 * pad + bw2, y, bw2 / 2, bh3);
    lay->y = y;
    lay->n = n;
}

/* The Reload question, asked only while a track is loaded (or that cannot be told). */
static void cm_layout_reload(struct cm_lay *lay) {
    azmod_state *s = lay->s;
    const azmod_fb *fb = lay->fb;
    cm_ctl *ctl = lay->ctl;
    int draw = lay->draw;
    int bx = lay->bx;
    int lh = lay->lh;
    int dpx = lay->dpx;
    int pad = lay->pad;
    int bh3 = lay->bh3;
    int y = lay->y;
    int n = lay->n;
    if (draw) {
        c_text(fb, bx + pad, y, 0, lh, "Reload now?", dpx * 6 / 5, C_HI);
        c_text(fb, bx + pad, y + 2 * lh, 0, lh, "Reload restarts the deck software. Music stops, and loaded tracks", dpx, C_TEXT);
        c_text(fb, bx + pad, y + 3 * lh, 0, lh, "are unloaded, for about 10 seconds.", dpx, C_TEXT);
    }
    y += 5 * lh;
    int bw2 = fb->w * 200 / 1280;
    if (draw) cm_button(fb, bx + pad, y, bw2, bh3, "Not now", 0, 1, CM_FOC);
    CM_CTL(CMC_RELOAD_NO, 0, bx + pad, y, bw2, bh3);
    if (draw) cm_button(fb, bx + 2 * pad + bw2, y, bw2, bh3, "Reload", 1, 1, CM_FOC);
    CM_CTL(CMC_RELOAD_OK, 0, bx + 2 * pad + bw2, y, bw2, bh3);
    lay->y = y;
    lay->n = n;
}

/* Add repository: the field and the keyboard. Full screen: a big field and the deck's keyboard. */
static void cm_layout_kbd_full(struct cm_lay *lay) {
    azmod_state *s = lay->s;
    const azmod_fb *fb = lay->fb;
    cm_ctl *ctl = lay->ctl;
    int draw = lay->draw;
    int bx = lay->bx;
    int bw = lay->bw;
    int rh = lay->rh;
    int lh = lay->lh;
    int dpx = lay->dpx;
    int pad = lay->pad;
    int bh3 = lay->bh3;
    int y = lay->y;
    int n = lay->n;
    int fh = rh * 13 / 10, fpx = rh * 52 / 100, bw2 = fb->w * 150 / 1280;
    if (draw) {
        c_text(fb, bx + pad, y, 0, lh, "Type owner/repo, a github.com address, or an https:// address ending in azmods-repo.json:", dpx, C_TEXT_DIM);
        int fw = bw - 2 * pad - 2 * (bw2 + pad);
        fill_rect(fb, bx + pad, y + lh + pad / 2, fw, fh, 0x00343434u);
        /* the end of what was typed stays in view: cut from the left */
        char t[110];
        snprintf(t, sizeof t, "%s_", g_cm_kb);
        const char *v = t;
        while (*v && cm_tw(v, fpx) > fw - 2 * pad) v++;
        c_text(fb, bx + 2 * pad, y + lh + pad / 2, 0, fh, v, fpx, C_TEXT);
    }
    int ky = y + lh + pad / 2 + (fh - bh3) / 2, ax = bx + bw - pad - 2 * bw2 - pad;
    if (draw) cm_button(fb, ax, ky, bw2, bh3, "Add", 1, g_cm_kb[0] != 0, CM_FOC);
    CM_CTL(CMC_KB_OK, 0, ax, ky, bw2, bh3);
    if (draw) cm_button(fb, ax + bw2 + pad, ky, bw2, bh3, "Cancel", 0, 1, CM_FOC);
    CM_CTL(CMC_KB_NO, 0, ax + bw2 + pad, ky, bw2, bh3);
    /* whole pieces of an address, one press each */
    {
        static const char *SC[3] = {"https://", "github.com/", "/azmods-repo.json"};
        int sy = y + lh + pad / 2 + fh + pad, sx = bx + pad;
        for (int k = 0; k < 3; k++) {
            int sw = cm_tw(SC[k], dpx) + 3 * pad;
            if (draw) cm_button(fb, sx, sy, sw, bh3, SC[k], 0, 1, CM_FOC);
            CM_CTL(CMC_KEY, 0x110 + k, sx, sy, sw, bh3);
            sx += sw + pad;
        }
    }
    for (int r = 0; r < 3; r++) {
        const char *keys = CM_KB[g_cm_kbnum][r];
        int nk = (int) strlen(keys);
        for (int k = -3; k < nk; k++) {
            if (k < 0 && !((k == -1 && r == 0) || ((k == -2 || k == -3) && r == 2))) continue;
            int x, yy, w, h;
            cm_kbf_rect(fb, r, k, nk, &x, &yy, &w, &h);
            char lab[8];
            int code;
            if (k >= 0) {
                lab[0] = keys[k];
                lab[1] = 0;
                code = (unsigned char) keys[k];
            } else if (k == -1) {
                snprintf(lab, sizeof lab, "del");
                code = 0x102;
            } else if (k == -2) {
                snprintf(lab, sizeof lab, "%s", g_cm_kbnum ? "abc" : "123");
                code = 0x100;
            } else {
                snprintf(lab, sizeof lab, "space");
                code = 0x101;
            }
            if (draw) {
                int foc = CM_FOC;
                fill_rect(fb, x, yy, w, h, foc ? C_FOCUS_BAND : 0x00343434u);
                if (foc) c_focus_ring(fb, x, yy, w, h);
                c_text(fb, x, yy, w, h, lab, k >= 0 ? h * 42 / 100 : h * 30 / 100, C_TEXT);
            }
            CM_CTL(CMC_KEY, code, x, yy, w, h);
        }
    }
    lay->y = y;
    lay->n = n;
}

/* Add repository: the field and the keyboard. */
static void cm_layout_kbd(struct cm_lay *lay) {
    azmod_state *s = lay->s;
    const azmod_fb *fb = lay->fb;
    cm_ctl *ctl = lay->ctl;
    int draw = lay->draw;
    int bx = lay->bx;
    int bw = lay->bw;
    int rh = lay->rh;
    int lh = lay->lh;
    int px = lay->px;
    int dpx = lay->dpx;
    int pad = lay->pad;
    int bh3 = lay->bh3;
    int y = lay->y;
    int n = lay->n;
    if (draw) {
        c_text(fb, bx + pad, y, 0, lh, "Type owner/repo, or a github.com address:", dpx, C_TEXT_DIM);
        fill_rect(fb, bx + pad, y + lh, bw - 2 * pad, rh, 0x00343434u);
        char t[110];
        snprintf(t, sizeof t, "%s_", g_cm_kb);
        c_text(fb, bx + 2 * pad, y + lh, 0, rh, t, px, C_TEXT);
    }
    int bw2 = fb->w * 130 / 1280, ky = y + lh + rh + pad;
    if (draw) cm_button(fb, bx + pad, ky, bw2, bh3, "Add", 1, g_cm_kb[0] != 0, CM_FOC);
    CM_CTL(CMC_KB_OK, 0, bx + pad, ky, bw2, bh3);
    if (draw) cm_button(fb, bx + 2 * pad + bw2, ky, bw2, bh3, "Cancel", 0, 1, CM_FOC);
    CM_CTL(CMC_KB_NO, 0, bx + 2 * pad + bw2, ky, bw2, bh3);
    for (int r = 0; r < 4; r++) {   /* rows 0-2 keys; row 3: 123/abc, space, del */
        const char *keys = CM_KB[g_cm_kbnum][r];
        int nk = r < 3 ? (int) strlen(keys) : 3;
        for (int k = 0; k < nk; k++) {
            int x, yy, w, h;
            cm_kb_rect(fb, r, k, r < 3 ? nk : 3, &x, &yy, &w, &h);
            if (r == 3) {
                int kw = w;
                w = kw * 3;
                x = bx + (bw - 3 * (kw * 3 + kw / 4)) / 2 + k * (kw * 3 + kw / 4);
            }
            char lab[8];
            int code;
            if (r < 3) {
                lab[0] = keys[k];
                lab[1] = 0;
                code = (unsigned char) keys[k];
            } else {
                static const char *SP[3] = {"123", "space", "del"};
                snprintf(lab, sizeof lab, "%s", k == 0 && g_cm_kbnum ? "abc" : SP[k]);
                code = 0x100 + k;
            }
            if (draw) {
                int foc = CM_FOC;
                fill_rect(fb, x, yy, w, h, foc ? C_FOCUS_BAND : 0x00343434u);
                if (foc) c_focus_ring(fb, x, yy, w, h);
                c_text(fb, x, yy, w, h, lab, h * 45 / 100, C_TEXT);
            }
            CM_CTL(CMC_KEY, code, x, yy, w, h);
        }
    }
    lay->y = y;
    lay->n = n;
}

/* The home page's rail, at its left edge under the header: the Installed, Browse and Repositories
 * tabs, each an icon over its name, and Refresh at the bottom. It stays put while the page scrolls,
 * and like the header its controls come after the page's (cm_home_ctl_at tries them first). */
static void cm_layout_rail(struct cm_lay *lay) {
    azmod_state *s = lay->s;
    const azmod_fb *fb = lay->fb;
    cm_ctl *ctl = lay->ctl;
    int draw = lay->draw;
    int lh = lay->lh;
    int dpx = lay->dpx;
    int pad = lay->pad;
    int n = lay->n;
    static const unsigned char *const ICS[4] = {IC_INSTALLED, IC_BROWSE, IC_REPOS, IC_UPDATE};
    int upd[CM_NCAT], nu = cm_upd_list(upd);
    char ul[24];
    snprintf(ul, sizeof ul, nu ? "Updates %d" : "Updates", nu);   /* the count while there are updates */
    int top = dock_y(fb) + menu_hdrh(fb), bot = dock_y(fb) + dock_h(fb), rw = cm_rail_w(fb);
    int ch = fb->h * 92 / 800, isz = ch * 38 / 100, lpx = dpx * 85 / 100, iy = ch * 20 / 100;
    if (draw) {
        fill_rect(fb, 0, top, rw, bot - top, C_HEADER_BG);
        fill_rect(fb, rw - 2, top, 2, bot - top, C_DIVIDER);
    }
    /* 0-2 the tabs from the top, 3 Updates at the bottom, 4 above it a refresh under way (no button:
     * opening the screen refreshes) */
    for (int t = 0; t < 5; t++) {
        int cy = t < 3 ? top + t * ch : t == 3 ? bot - ch - pad
                                               : bot - 2 * ch - pad,
            on = t < 4 && g_cm_tab == t,
            live = t < 4 || !cm_busy();
        if (t == 4 && (live || strcmp(g_cs.op, "refresh"))) continue;
        if (draw) {
            int foc = CM_FOC;
            if (on || foc) fill_rect(fb, 0, cy, rw - 2, ch, on ? 0x00343434u : C_FOCUS_BAND);
            if (on) fill_rect(fb, 0, cy, fb->w * 4 / 1280, ch, C_HI);
            uint32_t c = on ? C_HI : live ? C_TEXT
                                          : C_TEXT_DIM;
            int refr = t == 4 && !live && !strcmp(g_cs.op, "refresh");   /* Refresh running: it says so, turning */
            if (refr) cm_spinner(fb, rw / 2, cy + iy + isz / 2, isz / 2, C_HI, C_TEXT_DIM);
            else blit_icon(fb, (rw - isz) / 2, cy + iy, isz, c, t < 4 ? ICS[t] : IC_RELOAD);
            c_text(fb, 0, cy + iy + isz, rw - 2, lh, t == 3 ? ul : t < 4 ? CM_TABNAME[t]
                                                               : live    ? "Refresh"
                                                               : refr    ? "Refreshing..."
                                                                         : "Working...",
                   lpx, on || refr ? C_HI : live ? C_TEXT
                                                 : C_TEXT_DIM);
        }
        if (t < 4) CM_CTL(CMC_TAB, t, 0, cy, rw - 2, ch);
    }
    lay->n = n;
}
/* Browse: the orders, open under the Sort button (over the page, above it when there is no room
 * below). The current one is lit; a tap on one picks it, a tap anywhere else only closes them. */
static void cm_layout_sortpop(struct cm_lay *lay) {
    azmod_state *s = lay->s;
    const azmod_fb *fb = lay->fb;
    cm_ctl *ctl = lay->ctl;
    int draw = lay->draw;
    int by = lay->by;
    int bh = lay->bh;
    int n = lay->n;
    int ih = g_cm_sortr.h, ph = CMB_NSORT * ih + 4, px = menu_text_px(fb), x = g_cm_sortr.x, w = g_cm_sortr.w;
    int y = g_cm_sortr.y + g_cm_sortr.h + 4;
    if (y + ph > by + bh) y = g_cm_sortr.y - 4 - ph;
    if (px > ih * 62 / 100) px = ih * 62 / 100;
    if (draw) {
        fill_rect(fb, x, y, w, ph, 0x00343434u);
        draw_rect_outline(fb, x, y, w, ph, 2, C_CHECK_BOX);
    }
    for (int k = 0; k < CMB_NSORT; k++) {
        int iy = y + 2 + k * ih, on = g_cm_sort == k;
        if (draw) {
            int foc = CM_FOC;
            if (on || foc) fill_rect(fb, x + 2, iy, w - 4, ih, on ? C_HI : C_FOCUS_BAND);
            c_text(fb, x + ih / 2, iy, 0, ih, cmb_sort_label[k], px, on ? C_HI_TEXT : C_TEXT);
        }
        CM_CTL(CMC_SORT, k, x, iy, w, ih);
    }
    CM_CTL(CMC_SORTOFF, 0, 0, dock_y(fb), fb->w, dock_h(fb));
    lay->n = n;
}

/* The full-screen header band: the back square, the section's icon, the page title and the Reload note;
 * on the home page also the master switch and the trust note. */
static void cm_layout_header(struct cm_lay *lay) {
    azmod_state *s = lay->s;
    const azmod_fb *fb = lay->fb;
    cm_ctl *ctl = lay->ctl;
    int draw = lay->draw;
    int dpx = lay->dpx;
    int pad = lay->pad;
    int n = lay->n;
    int top = dock_y(fb), hh = menu_hdrh(fb), side = hh * 5 / 8, qx = fb->w / 40, qy = top + (hh - side) / 2;
    int isz = hh * 45 / 100, ix = qx + side + fb->w / 50, tx = ix + isz + fb->w / 60, tright = fb->w - fb->w / 40, tpx = hh * 32 / 100;
    int tw_title = 0;
    int rshow = g_cm_view != CMV_RELOAD && (g_cm_reloading || (cm_need_reload() && !cm_busy())), rsz = hh * 84 / 100;
    if (rshow)   /* leave room for the note + the Reload square */
        tright -= rsz + 2 * pad + cm_tw("Changes take effect after a reload", dpx);
    int nmods = g_ncc + (g_cc_more > 0 ? g_cc_more : 0);
    char cnt[32] = "";
    int cntx = 0;
    if (g_cm_view == CMV_HOME && g_cm_tab != 0 && nmods > 0) {   /* Browse, Repositories, Updates: the catalog's size, right-aligned */
        snprintf(cnt, sizeof cnt, nmods == 1 ? "1 mod" : "%d mods", nmods);
        int cw = cm_tw(cnt, dpx);
        cntx = tright - cw;
        tright -= cw + 2 * pad;
    }
    if (draw) {
        fill_rect(fb, 0, top, fb->w, hh, C_HEADER_BG);
        if (cnt[0]) c_text(fb, cntx, top, 0, hh, cnt, dpx, C_TEXT_DIM);   /* over the band, not under it */
        draw_rect_outline(fb, qx, qy, side, side, 2, CM_FOC ? C_HI : C_CHECK_BOX);
        int asz = side * 3 / 5;
        blit_icon(fb, qx + (side - asz) / 2, qy + (side - asz) / 2, asz, C_TEXT, IC_BACK);
        draw_item_icon(fb, ix, top + (hh - isz) / 2, isz, AZI_COMMUNITY, C_TEXT);
        char t[CM_CRUMB_MAX];
        cm_crumbs(t, sizeof t, tright - tx - pad, tpx);
        cm_textw(fb, tx, top, tright - tx - pad, hh, t, tpx, C_TEXT);
        tw_title = cm_tw(t, tpx);
        fill_rect(fb, 0, top + hh - 2, fb->w, 2, C_DIVIDER);
    }
    if (g_cm_view == CMV_HOME && g_cm_tab == 0 && draw) {   /* Installed: the trust note beside the title */
        int spx = dpx * 70 / 100, nx = tx + tw_title + fb->w / 40;
        if (tright - pad - nx >= cm_tw("Made by...", spx))   /* else no room beside the title */
            cm_textw(fb, nx, top, tright - pad - nx, hh, CM_TRUST_NOTE, spx, C_TEXT_DIM);
    }
    if (rshow) {   /* something changed what loads: Reload, always this one place */
        int rx = fb->w - fb->w / 40 - rsz, ry = top + (hh - rsz) / 2;
        if (draw) {
            const char *a = g_cm_reloading ? "Please wait..." : "Changes take effect after a reload";
            c_text(fb, rx - pad - cm_tw(a, dpx), top, 0, hh, a, dpx, g_cm_reloading ? C_TEXT : C_AVAIL);
            cm_reload_square(fb, rx, ry, rsz, g_cm_reloading, !g_cm_reloading && CM_FOC);
        }
        if (!g_cm_reloading) CM_CTL(CMC_APPLY, 0, rx, ry, rsz, rsz);
    }
    CM_CTL(CMC_BACK, 0, qx, qy, side, side);
    lay->n = n;
}

/* The home page scrolls through content px in a view px tall: how far it can go, and an offset
 * kept inside that. */
static int cm_scroll_max(int content, int view) { return view > 0 && content > view ? content - view : 0; }
static int cm_scroll_clamp(int v, int max) {
    if (v > max) v = max;
    return v < 0 ? 0 : v;
}
static int cm_layout(azmod_state *s, const azmod_fb *fb, cm_ctl *ctl, int draw) {
    int bx, by, bw, bh;
    cm_box(fb, &bx, &by, &bw, &bh);
    int rh = c_rrowh(fb), lh = fb->h * 26 / 800, px = rh * 44 / 100, dpx = fb->h * 17 / 800, pad = fb->w / 80;
    int y = by - (g_cm_view == CMV_HOME ? g_cm_scroll : 0), n = 0, bh3 = rh * 3 / 5;
    struct cm_lay lay = {s, fb, ctl, draw, bx, by, bw, bh, rh, lh, px, dpx, pad, bh3, y, n};
    if (g_cm_view == CMV_HOME) {
        cm_layout_home(&lay);
        int content = lay.y - y;   /* the whole page, from its top edge to its last row */
        g_cm_smax = cm_scroll_max(content, bh);
        /* EP147's own list scroll bar in the right margin, only while the page overflows (ui_draw.c draw_vsb);
         * split, at the list's own edge */
        if (draw) draw_vsb(fb, cm_isplit() ? bx + bw / 2 - pad / 2 : bx + bw + pad, by, bh, content, bh, g_cm_scroll);
        if (cm_isplit()) cm_layout_split(&lay);
    } else if (g_cm_view == CMV_MOD) {
        if (!cm_layout_mod(&lay))
            return 0;
    } else if (g_cm_view == CMV_CAT) {
        if (!cm_layout_cat(&lay))
            return 0;
    } else if (g_cm_view == CMV_TRUST) {
        cm_layout_trust(&lay);
    } else if (g_cm_view == CMV_RELOAD) {
        cm_layout_reload(&lay);
    } else if (g_cm_view == CMV_KBD && g_cm_full) {
        cm_layout_kbd_full(&lay);
    } else if (g_cm_view == CMV_KBD) {
        cm_layout_kbd(&lay);
    }
    g_cm_nbody = lay.n;
    if (g_cm_full && g_cm_view == CMV_HOME) {
        if (g_cm_sortpop && g_cm_sortr.on) cm_layout_sortpop(&lay);
        cm_layout_rail(&lay);
    }
    if (g_cm_full)
        cm_layout_header(&lay);
    return lay.n;
}
#undef CM_CTL
#undef CM_FOC
#undef CM_HF
/* MOD MENU > Community Mods selected but not open: the preview page in the right pane, laid
 * out like the other rows' pages (a title row, one line of help, then its rows). The master
 * On/Off is the row's own checkbox outside this card; this page only reports it. */
static int cm_card_rshow(void) { return g_cm_reloading || cm_need_reload(); }
/* The card's own Reload square (top right of its header), shown only while rshow. Same rect
 * on every draw, so a tap and the draw agree. */
static void cm_card_reload_rect(const azmod_fb *fb, int *rx, int *ry, int *rsz) {
    int top = dock_y(fb), hh = menu_hdrh(fb), w0 = dock_w(fb), pad = fb->w / 40;
    *rsz = hh * 84 / 100;   /* the header's own Reload square size (cm_layout_header) */
    *rx = dock_x(fb) + w0 - pad - *rsz;
    *ry = top + (hh - *rsz) / 2;
}
static void cm_draw_card(azmod_state *s, const azmod_fb *fb) {
    (void) s;
    cm_load();
    int x0 = dock_x(fb), top = dock_y(fb), hh = menu_hdrh(fb), w0 = dock_w(fb), bot = top + dock_h(fb);
    int pad = fb->w / 40, lh = fb->h * 26 / 800, dpx = fb->h * 17 / 800, rh = c_rrowh(fb), px = rh * 44 / 100;
    int rshow = cm_card_rshow(), rx, ry, rsz;
    cm_card_reload_rect(fb, &rx, &ry, &rsz);
    fill_rect(fb, x0, top, w0, dock_h(fb), C_MENU_SCRIM);
    fill_rect(fb, x0, top, w0, hh, C_HEADER_BG);
    int tright = x0 + w0 - pad - (rshow ? rsz + pad : 0);
    if (rshow) {   /* Reload, as at the top right of the full screen: the note beside the square */
        const char *a = g_cm_reloading ? "Please wait..." : "Changes take effect after a reload";
        int aw = cm_tw(a, dpx);
        c_text(fb, rx - pad / 2 - aw, top, 0, hh, a, dpx, g_cm_reloading ? C_TEXT : C_AVAIL);
        tright = rx - pad - aw;
        cm_reload_square(fb, rx, ry, rsz, g_cm_reloading, 0);
    }
    cm_textw(fb, x0 + pad, top, tright - (x0 + pad), hh, "Community Mods", hh * 34 / 100, C_TEXT);
    fill_rect(fb, x0, top + hh - 2, w0, 2, C_DIVIDER);
    int y = top + hh + fb->h * 14 / 800;
    cm_textw(fb, x0 + pad, y, w0 - 2 * pad, lh, "Mods made by other developers for your deck.", dpx, C_TEXT_DIM);
    y += lh + fb->h * 14 / 800;
    /* the master state: the row's own checkbox is the control; this just names it */
    int off = cm_off_now();
    c_text(fb, x0 + pad, y, 0, rh, "Community Mods", px, C_TEXT);
    {
        const char *ostr = off ? "Off" : "On";
        c_text(fb, x0 + w0 - pad - cm_tw(ostr, px), y, 0, rh, ostr, px, off ? C_TEXT_DIM : C_OK);
    }
    y += rh + fb->h * 10 / 800;
    /* the enabled mods: icon, name, state in a word or two (cm_state_text: the same words the
     * Installed tab shows for the same mod) */
    int ic = rh, nm = azcm_count();
    for (int i = 0; i < nm && y + ic <= bot - lh; i++) {
        azcm_info_t m;
        if (azcm_info(i, &m) || !cm_turned_on(&m)) continue;
        char buf[96];
        uint32_t col = C_TEXT;
        const char *st = cm_state_text(&m, buf, sizeof buf, &col);
        int tx = x0 + pad + ic + pad, tw = x0 + w0 - pad - tx, ci = -1;
        for (int k = 0; k < g_ncc; k++)
            if (!strcmp(g_cc[k].id, m.id) && g_cc[k].inst[0]) {
                ci = k;
                break;
            }
        cm_icon(fb, x0 + pad, y, ic, m.id, m.name, ci >= 0 ? g_cc[ci].icon : "");
        cm_textw(fb, tx, y, tw, ic / 2, m.name, dpx, C_TEXT);
        cm_textw(fb, tx, y + ic / 2, tw, ic / 2, st, dpx * 85 / 100, col);
        y += ic + fb->h * 8 / 800;
    }
    /* one line for what needs attention, only when there is something */
    if (y + lh <= bot) {
        int nn = azcm_new_count(), upd = 0;
        for (int i = 0; i < nm; i++) {
            azcm_info_t m;
            if (!azcm_info(i, &m) && cm_update_idx(m.id) >= 0 && !cm_removed(m.id)) upd++;
        }
        char t[96];
        if (nn)
            snprintf(t, sizeof t, nn == 1 ? "1 new mod found: allow it in Community Mods." : "%d new mods found: allow them in Community Mods.", nn);
        else if (upd)
            snprintf(t, sizeof t, upd == 1 ? "1 update available." : "%d updates available.", upd);
        else
            t[0] = 0;
        if (t[0]) c_text(fb, x0 + pad, y, 0, lh, t, dpx, C_HI);
    }
}
static void cm_draw(azmod_state *s, const azmod_fb *fb) {
    cm_load();
    if (!g_cm_full) {
        cm_draw_card(s, fb);
        return;
    }
    /* the whole menu area: the content, then its header (drawn last by the layout) */
    fill_rect(fb, 0, dock_y(fb), fb->w, dock_h(fb), C_MENU_SCRIM);
    cm_ctl ctl[CM_MAXCTL];
    int n = cm_layout(s, fb, ctl, 0);
    if (g_cm_sel >= n) g_cm_sel = n ? n - 1 : 0;
    if (g_cm_view == CMV_HOME) g_cm_scroll = cm_scroll_clamp(g_cm_scroll, g_cm_smax);   /* the page may have shrunk */
    cm_layout(s, fb, ctl, 1);
}
/* ---- the slide: in from the right on open, out to the right on BACK ------
 * ~220 ms, ease-out, one step per overlay tick (azmod_tick asks for a redraw while it runs;
 * no sleep, no loop). The full view is rendered once into an offscreen copy when the slide
 * starts, so a frame costs one pass of the Mod Menu under it plus a row copy. A frame that
 * comes too late or draws too slowly ends the slide at once (the end state, never a stall).
 * Input is held only while it runs. settings/azmod-logs.on: each slide's frame times go to
 * /tmp/azmod-cmslide.log. */
#define CM_SLIDE_MS 220
#define CM_SLIDE_LATE 90      /* ms between two slide frames that ends it */
#define CM_SLIDE_SLOW 40      /* ms one slide frame may take to draw */
static long g_cm_t0, g_cm_last;
static int g_cm_dir;      /* +1 opening, -1 closing, 0 idle */
static uint8_t *g_cm_off;
static int g_cm_offsz;           /* the full view, rendered once */
static int g_cm_nfr, g_cm_cut;
static short g_cm_fr[32][2]; /* frame: ms since start, draw ms */
static bool cm_animating(void) { return g_cm_dir != 0; }
static void g_cm_dir_reset(void) { g_cm_dir = 0; }
static void cm_slide_start(azmod_state *s, int dir) {
    g_cm_dir = dir;
    g_cm_t0 = g_cm_last = cm_ms();
    g_cm_nfr = 0;
    g_cm_cut = 0;
    (void) s;
}
/* Opening the full screen refreshes the catalog, at most once per CM_AUTO_MS (and always the first
 * time after a start): no Refresh button to find, and the fetcher is not kept busy (installs wait
 * while it works) by every visit. Offline, the fetcher keeps the last catalog and says so. */
#define CM_AUTO_MS (10L * 60 * 1000)
static long g_cm_auto_at;   /* cm_ms() of the last automatic refresh asked for, 0 = none yet */
static void cm_auto_refresh(void) {
    /* the last refresh did not end clean (no internet, a repository or the Mod Index unread): the
     * next opening tries again at once, so fixing WiFi and reopening is enough */
    int retry = !strcmp(g_cs.op, "refresh") && (!strcmp(g_cs.state, "error") || g_cs.msg[0]);
    if (cm_busy() || (!retry && g_cm_auto_at && cm_ms() - g_cm_auto_at < CM_AUTO_MS)) return;
    cm_req("refresh", 0, 0);
    g_cm_auto_at = cm_ms();
    if (!g_cm_auto_at) g_cm_auto_at = 1;
}
static void cm_full_open(azmod_state *s) {
    s->left_sel = AZI_COMMUNITY;
    s->mode = AZ_COMMUNITY;
    s->pane = 1;
    /* a row's own screen ends with its page: opened here, the split shows the mod's own page
     * (cm_open_row sets the row's screen again after this, when that is what opens) */
    azcm_page_screen(-1, NULL);
    g_cm_ient = -1;
    if (g_cm_full && !g_cm_dir) return;
    if (!g_cm_full) cm_reset();
    g_cm_full = 1;
    cm_auto_refresh();
    cm_slide_start(s, 1);
}
static void cm_full_close(azmod_state *s) {
    if (g_cm_full) cm_slide_start(s, -1);
}
static void cm_slide_end(azmod_state *s) {
    if (access("/home/root/settings/azmod-logs.on", F_OK) == 0) {
        FILE *f = azlog_fopen_capped("/tmp/azmod-cmslide.log", AZLOG_CAP_BYTES);
        if (f) {
            fprintf(f, "%s %d frames%s:", g_cm_dir > 0 ? "open" : "close", g_cm_nfr, g_cm_cut ? " (cut short: a late or slow frame)" : "");
            for (int i = 0; i < g_cm_nfr && i < 32; i++) fprintf(f, " %d/%dms", g_cm_fr[i][0], g_cm_fr[i][1]);
            fprintf(f, "\n");
            fclose(f);
        }
    }
    if (g_cm_dir < 0 && g_cm_row >= 0) {   /* back on the Mod Menu, on the SCREEN row that opened the page */
        g_cm_full = 0;
        c_select(s, s->left_sel);
        s->pane = g_cm_row_pane;
        s->menu_sel = g_cm_row_sel;
        g_cm_row = -1;
    } else if (g_cm_dir < 0) {
        g_cm_full = 0;
        s->pane = 0;
        s->mode = AZ_COMMUNITY;
    }   /* back on the Mod Menu, Community Mods row */
    g_cm_dir = 0;
}
/* one slide frame: the Mod Menu (left list + the card), then the full view shifted right by off */
static void cm_slide_draw(azmod_state *s, const azmod_fb *fb) {
    long now = cm_ms(), el = now - g_cm_t0;
    if (now - g_cm_last > CM_SLIDE_LATE && g_cm_nfr) g_cm_cut = 1;
    int need = fb->stride * fb->h;
    if (!g_cm_off || g_cm_offsz != need) {
        free(g_cm_off);
        g_cm_off = malloc(need);
        g_cm_offsz = g_cm_off ? need : 0;
        g_cm_nfr = 0;
    }
    if (!g_cm_off) g_cm_cut = 1;
    if (!g_cm_cut && g_cm_nfr == 0) {   /* render the full view once */
        azmod_fb o = {g_cm_off, fb->w, fb->h, fb->stride};
        cm_draw(s, &o);
    }
    float t = el >= CM_SLIDE_MS || g_cm_cut ? 1.f : (float) el / CM_SLIDE_MS, e = 1.f - (1.f - t) * (1.f - t) * (1.f - t);
    float shown = g_cm_dir > 0 ? e : 1.f - e;           /* share of the full view on screen */
    int off = (int) ((1.f - shown) * fb->w);
    if (off > 0) {
        g_cm_full = 0;
        c_draw_left(s, fb);
        if (g_cm_row >= 0) c_draw_right(s, fb);
        else cm_draw_card(s, fb);
        g_cm_full = 1;
    }
    if (off < fb->w && g_cm_off)
        for (int y = dock_y(fb); y < dock_y(fb) + dock_h(fb) && y < fb->h; y++)
            memcpy(fb->px + (long) y * fb->stride + (long) off * 4, g_cm_off + (long) y * fb->stride, (size_t) (fb->w - off) * 4);
    long dt = cm_ms() - now;
    if (dt > CM_SLIDE_SLOW) g_cm_cut = 1;
    if (g_cm_nfr < 32) {
        g_cm_fr[g_cm_nfr][0] = (short) el;
        g_cm_fr[g_cm_nfr][1] = (short) dt;
    }
    g_cm_nfr++;
    g_cm_last = now;
    if (t >= 1.f) cm_slide_end(s);
}
/* the page's mod screen takes the input while it is open (when it has one) */
static int cm_page_screen(void) {
    azcm_info_t m;
    return g_cm_view == CMV_MOD && !azcm_info(g_cm_page, &m) && m.state == AZCM_LOADED && m.has_screen;
}
static void cm_open(int view) {
    if (view != CMV_MOD) azcm_page_screen(-1, NULL);   /* a row's own screen lasts for its page only */
    if (g_cm_view == CMV_HOME && view != CMV_HOME) {
        g_cm_home_sel = g_cm_sel;
        g_cm_home_scroll = g_cm_scroll;
    }
    if (view == CMV_HOME && g_cm_home_sel >= 0) {
        g_cm_sel = g_cm_home_sel;
        g_cm_scroll = g_cm_home_scroll;
        g_cm_home_sel = -1;
    } else g_cm_sel = 0;   /* the first control of a page is its main action, never the back square */
    g_cm_zoom = -1;
    g_cm_ient = -1;   /* back on the split, its mod's screen is entered again */
    if (view != CMV_CAT && view != CMV_TRUST) g_cm_sh.cat = -1;   /* a new visit asks for its screenshots again */
    g_cm_view = view;
}
/* The row's checkbox: written at once (the core reads it at its next start, whatever restarts
 * it), and shown pending until then; Reload is offered, never sent from here. */
static void cm_master_toggle(void) {
    int off = !cm_off_now();
    azcm_set_all_off(off);
    g_cm_pending_off = off;
}
/* The reload request, the same from the header's Reload square and the preview card's: shared
 * so one press anywhere shows "Please wait..." everywhere (g_cm_reloading). */
static void cm_do_apply_reload(void) {
    cmu_cancel(&g_cm_ua);   /* a reload ends Update all */
    azcm_reload_requested();
    cm_req("apply", 0, 0);
    g_cm_reload = 0;
    g_cm_reloading = 1;   /* g_cm_resumed stays: the core is still not loading them until the restart */
}
/* The preview card's own Reload square (cm_draw_card): 1 = handled. Inert once already sent,
 * until the restart or an error brings it back. */
static int cm_card_tap_reload(void) {
    if (g_cm_reloading || !cm_card_rshow()) return 0;
    cm_do_apply_reload();
    return 1;
}
static azmod_state *g_cm_s;   /* for CMC_BACK (cm_fire has no state); set by cm_input */
static void cm_fire(const cm_ctl *c) {
    /* clang-format off */
    switch (c->kind) {
    case CMC_RESUME:  azcm_resume(); g_cm_resumed = 1; g_cm_sel = 0; break;
    case CMC_KEEPOFF: azcm_set_all_off(1); g_cm_pending_off = 1; g_cm_keptoff = 1; g_cm_sel = 0; break;
    case CMC_BACK:    if (g_cm_s && !cm_back()) cm_full_close(g_cm_s); break;
    case CMC_MOD:     if (cm_isplit() && !g_cm_row_opening) { cm_split_select(CMC_MOD << 16 | c->idx); break; }   /* the split: its page on the right */
                      if (!g_cm_row_opening) azcm_page_screen(-1, NULL);   /* from the list: the mod's own page */
                      g_cm_page = c->idx; cm_open(CMV_MOD); if (cm_page_screen()) azcm_screen_enter(g_cm_page); break;
    case CMC_UNSUSPEND: { azcm_info_t m; if (!azcm_info(c->idx, &m)) { azcm_unsuspend(m.id); cm_pend_set(m.id, 1); }
                          if (c->idx < 16) g_cm_unsusp[c->idx] = 1;
                          g_cm_sel = 0; } break;
    case CMC_TAB:     if (g_cm_tab == 0 && c->idx != 0) azcm_flush_settings();   /* leaving the split's page */
                      g_cm_in = 0; g_cm_ient = -1;
                      g_cm_tab = c->idx; g_cm_hfoc = -1; g_cm_scroll = 0; g_cm_sortpop = 0; if (c->idx && !g_ncc && !g_ncr && !cm_busy()) cm_req("refresh", 0, 0); break;
    case CMC_TILE:    g_cm_cat = c->idx; cm_open(CMV_CAT); break;
    case CMC_REFRESH: if (!cm_busy()) cm_req("refresh", 0, 0); break;
    case CMC_ADDREPO: if (!cm_busy()) { g_cm_kbnum = 0; cm_open(CMV_KBD); } break;   /* the draft is kept until a repository is added */
    case CMC_DELREPO:
        if (!cm_busy() && c->idx < g_ncr)
            cm_ask("Remove repository?", g_cr[c->idx].name[0] ? g_cr[c->idx].name : g_cr[c->idx].repo,
                   "delrepo %s", g_cr[c->idx].repo);
        break;
    case CMC_APPLY:   if (azcm_any_loaded()) { g_cm_reload_from = g_cm_view; cm_open(CMV_RELOAD); g_cm_sel = 0; break; }
                      /* fall through - nothing is playing, reload now */
    case CMC_RELOAD_OK:
        cm_do_apply_reload();
        if (g_cm_view == CMV_RELOAD) cm_open(g_cm_reload_from);
        break;
    case CMC_RELOAD_NO: cm_open(g_cm_reload_from); break;
    case CMC_ALLOW:   g_cm_allow_err = azcm_allow(c->idx) == 0 ? -1 : c->idx; g_cm_sel = 0; break;
    case CMC_INSTCAT: if (cm_isplit()) { cm_split_select(CMC_INSTCAT << 16 | c->idx); break; }
                      g_cm_cat = c->idx; cm_open(CMV_CAT); break;
    case CMC_MODUPD: {   /* Update on an installed mod's page: the same install the catalog page sends */
        azcm_info_t m; int k;   /* looked up again: the catalog may have been re-read since the draw */
        if (cm_busy() || azcm_info(g_cm_page, &m) || (k = cm_update_idx(m.id)) < 0) break;
        cm_req("install %s %s", g_cc[k].repo, g_cc[k].id);
    } break;
    case CMC_MODACT: {
        azcm_info_t m; if (cm_busy() || azcm_info(g_cm_page, &m)) break;
        if (c->idx == CMA_REMOVE) { char t[96]; snprintf(t, sizeof t, "%s is deleted from this USB.", m.name[0] ? m.name : m.id);
                                    cm_ask("Remove mod?", t, "remove %s", m.id); }
        else cm_req(c->idx == CMA_ENABLE ? "enable %s" : "disable %s", m.id, 0);
    } break;
    case CMC_ACT: {
        if (cm_busy() || g_cm_cat < 0 || g_cm_cat >= g_ncc) break;
        cm_cmod *m = &g_cc[g_cm_cat];
        if ((c->idx == CMA_INSTALL || c->idx == CMA_UPDATE) && !(cm_works(m) && cm_core_ok(m))) break;
        if (c->idx == CMA_INSTALL && m->held[0]) break;   /* another mod holds the id: the page says so */
        if (c->idx == CMA_INSTALL && access(CM_TRUSTED, F_OK) != 0) { cm_open(CMV_TRUST); g_cm_sel = 1; break; }   /* focus on Cancel */
        if (c->idx == CMA_INSTALL || c->idx == CMA_UPDATE) cm_req("install %s %s", m->repo, m->id);
        else if (c->idx == CMA_REMOVE)  { char t[96]; snprintf(t, sizeof t, "%s is deleted from this USB.", m->name[0] ? m->name : m->id);
                                          cm_ask("Remove mod?", t, "remove %s", m->id); }
        else if (c->idx == CMA_ENABLE)  cm_req("enable %s", m->id, 0);
        else if (c->idx == CMA_DISABLE) cm_req("disable %s", m->id, 0);
    } break;
    case CMC_TRUST_OK: {
        mkdir("/home/root/settings/azmod-community", 0755);
        FILE *f = fopen(CM_TRUSTED, "we"); if (f) fclose(f);
        if (g_cm_cat >= 0 && g_cm_cat < g_ncc) { cm_req("install %s %s", g_cc[g_cm_cat].repo, g_cc[g_cm_cat].id); }
        cm_open(CMV_CAT);
    } break;
    case CMC_TRUST_NO: cm_open(CMV_CAT); break;
    case CMC_KEY: {   /* into the repository address */
        char *kb = g_cm_kb;
        size_t cap = sizeof g_cm_kb, L = strlen(kb);
        if (c->idx == 0x100) g_cm_kbnum = !g_cm_kbnum;
        else if (c->idx == 0x101) { if (L + 1 < cap && L) { kb[L] = ' '; kb[L + 1] = 0; } }
        else if (c->idx == 0x102) { if (L) kb[L - 1] = 0; }
        else if (c->idx >= 0x110 && c->idx <= 0x112) {
            static const char *SC[3] = { "https://", "github.com/", "/azmods-repo.json" };
            snprintf(kb + L, cap - L, "%s", SC[c->idx - 0x110]);
        }
        else if (L + 1 < cap) { kb[L] = (char)c->idx; kb[L + 1] = 0; }
    } break;
    case CMC_KB_OK:
        if (g_cm_kb[0]) { cm_req("addrepo %s", g_cm_kb, 0); g_cm_tab = 2; cm_open(CMV_HOME); }
        break;
    case CMC_KB_NO:   cm_open(CMV_HOME); break;
    case CMC_SORT:    g_cm_sort = c->idx; g_cm_scroll = 0; g_cm_sortpop = 0; break;
    case CMC_SORTBTN: g_cm_sortpop = !g_cm_sortpop; break;
    case CMC_SORTOFF: g_cm_sortpop = 0; break;
    case CMC_FCAT:    g_cm_fcat = c->idx; g_cm_scroll = 0; g_cm_sortpop = 0; break;
    case CMC_STILE:   g_cm_cat = c->idx & 0xff; cm_open(CMV_CAT); break;
    case CMC_SHOT:    if (c->idx >= 0 && c->idx < CM_NSHOT && g_cm_sh.have[c->idx]) g_cm_zoom = c->idx; break;
    case CMC_SHOTZ:
        if (c->idx == 0 || g_cm_zoom < 0) g_cm_zoom = -1;
        else for (int k = 1; k < CM_NSHOT; k++) {   /* the previous / next one on disk, wrapping */
            int z = (g_cm_zoom + (c->idx == 2 ? k : CM_NSHOT - k)) % CM_NSHOT;
            if (g_cm_sh.have[z]) { g_cm_zoom = z; break; }
        }
        break;
    case CMC_UPDALL:  if (!cm_busy()) cm_updall_start(); break;
    case CMC_CHK: {   /* an Installed row's checkbox: the mod page's Enable / Disable */
        char id[33]; int on;
        if (g_cm_flip.at && cm_ms() - g_cm_flip.at > CM_FLIP_MS) g_cm_flip.at = 0;   /* no answer: stop waiting for it */
        if (!cm_busy() && !g_cm_flip.at && cm_chk(c->idx, id, &on)) {
            snprintf(g_cm_flip.id, sizeof g_cm_flip.id, "%s", id);
            g_cm_flip.prev = cm_pend_get(id);
            g_cm_flip.at = cm_ms();
            if (!g_cm_flip.at) g_cm_flip.at = 1;
            cm_pend_set(id, !on);   /* shown at once */
            cm_req(on ? "disable %s" : "enable %s", id, 0);
        }
    } break;
    case CMC_UPDROW:  /* a row's Update: the same install as the mod page's, looked up again */
        if (!cm_busy() && c->idx >= 0 && c->idx < g_ncc && cm_upd_cat(g_cc[c->idx].id) == c->idx) {
            g_cm_ua.result = CMU_OFF;
            cm_req("install %s %s", g_cc[c->idx].repo, g_cc[c->idx].id);
        }
        break;
    case CMC_SEEALL:  g_cm_fcat = cmb_shelf_cat(c->idx); g_cm_sort = cmb_shelf_sort(c->idx); g_cm_scroll = 0; break;
    }
    /* clang-format on */
}
/* A mod's SCREEN row in the Mod Menu's main list (register_row, AZ_ROW_SCREEN) opens that mod's
 * page through the same path as Community Mods > Installed > <mod>: the full screen
 * (cm_full_open) and cm_fire's CMC_MOD, so the page is the mod's registered screen with the same
 * header, input and BACK handling. BACK from it returns to the row (cm_back, cm_slide_end), not to
 * the Mod Index. false: not a registered SCREEN row, or its mod is not installed (the caller
 * then handles the row like any other). */
static bool cm_open_row(azmod_state *s, int it) {
    if (it < AZI_REG0 || it >= AZI_ACT0 || az_kind(it) != AZ_ROW_SCREEN)
        return false;
    int idx = cm_installed_idx(azf_owner(it));
    if (idx < 0)
        return false;
    const struct az_row *r = az_row(it);
    const az_screen_v1 *own = r ? r->d.screen : NULL;
    if (!azcm_row_target(azf_owner(it), own))
        return true;           /* nothing to open: the row is grayed and does nothing */
    int left = s->left_sel, pane = s->pane, sel = s->menu_sel;
    cm_full_open(s);           /* resets the Community view (and g_cm_row) when it opens */
    s->left_sel = left;        /* the left list keeps the row lit, under the page and in the slide */
    g_cm_row = it;
    g_cm_row_pane = pane;
    g_cm_row_sel = sel;
    azcm_page_screen(idx, own); /* the row's own screen; NULL = the mod's page */
    g_cm_row_opening = 1;
    cm_ctl c = {CMC_MOD, idx, 0, 0, 0, 0};
    cm_fire(&c);
    g_cm_row_opening = 0;
    return true;
}
static bool cm_back(void) {
    if (g_cm_view == CMV_HOME && g_cm_sortpop) {   /* the orders are open: BACK closes them first */
        g_cm_sortpop = 0;
        return true;
    }
    if (cm_isplit() && g_cm_in) {   /* in the split's page: the mod's screen may take BACK, else out to the list */
        if (cm_split_screen()) {
            az_event ev = {AZ_EV_BACK, 0, 0, 0};
            if (azcm_screen_input(g_cm_page, &ev)) return true;
        }
        g_cm_in = 0;
        g_cm_rsel = -1;
        return true;
    }
    if (g_cm_view == CMV_HOME) return false;
    if (g_cm_view == CMV_CAT && g_cm_zoom >= 0) {   /* a large screenshot: BACK closes it first */
        g_cm_zoom = -1;
        return true;
    }
    if (cm_page_screen()) {
        az_event ev = {AZ_EV_BACK, 0, 0, 0};
        if (azcm_screen_input(g_cm_page, &ev)) return true;
        azcm_flush_settings();
    }   /* leaving a mod's page: its settings are saved now */
    if (g_cm_view == CMV_TRUST) cm_open(CMV_CAT);
    else if (g_cm_view == CMV_RELOAD) cm_open(g_cm_reload_from);
    else if (g_cm_row >= 0 && g_cm_view == CMV_MOD) return false;   /* opened from its row: out to the row */
    else cm_open(CMV_HOME);
    return true;
}
static bool cm_list_knob(azmod_state *s, const azmod_fb *fb, int d, bool click);
static bool cm_input(azmod_state *s, const azmod_fb *fb, int tx, int ty, bool touched, int d, bool click) {
    cm_load();
    g_cm_s = s;
    if (cm_animating()) return true;                   /* held only while the slide runs (~220 ms) */
    if (!g_cm_full) {                                   /* the card: a tap or a press opens; a turn goes back to the list */
        if (touched || click) cm_full_open(s);
        else if (d) s->pane = 0;
        return true;
    }
    g_cm_area.w = 0;
    cm_ctl ctl[CM_MAXCTL];
    int n = cm_layout(s, fb, ctl, 0);
    if (cm_page_screen()) {   /* controls above the screen first, then the mod */
        az_event ev = {0, 0, 0, 0};
        if (touched) {
            for (int k = 0; k < n; k++)
                if (in_rect(tx, ty, ctl[k].x, ctl[k].y, ctl[k].w, ctl[k].h)) {
                    cm_fire(&ctl[k]);
                    return true;
                }
            /* the mod hears only taps inside the area it paints: not the header, its info or the buttons' gaps */
            if (g_cm_area.w > 0 && in_rect(tx, ty, g_cm_area.x, g_cm_area.y, g_cm_area.w, g_cm_area.h)) {
                ev.kind = AZ_EV_TAP;
                ev.x = tx;
                ev.y = ty;
            }
        } else if (d) {
            ev.kind = AZ_EV_KNOB;
            ev.dir = d > 0 ? 1 : -1;
        } else if (click) ev.kind = AZ_EV_CLICK;
        if (ev.kind) azcm_screen_input(g_cm_page, &ev);
        return true;
    }
    if (touched && cm_split_screen() && g_cm_area.w > 0 && in_rect(tx, ty, g_cm_area.x, g_cm_area.y, g_cm_area.w, g_cm_area.h)) {
        int hit = 0;   /* the page's own buttons first, as on the full page */
        for (int k = 0; k < n && !hit; k++) hit = in_rect(tx, ty, ctl[k].x, ctl[k].y, ctl[k].w, ctl[k].h);
        if (!hit) {
            az_event ev = {0, 0, 0, 0};
            ev.kind = AZ_EV_TAP;
            ev.x = tx;
            ev.y = ty;
            azcm_screen_input(g_cm_page, &ev);
            return true;
        }
    }
    if (g_cm_view == CMV_HOME) {
        /* The home page works by touch: a press is decided on its release (azmod_pane_drag, as in
         * the Mod Menu's lists), so a drag scrolls the page and never opens or toggles anything.
         * The knob scrolls the page a row per detent and focuses nothing; its click does nothing
         * here (no control is focused for it to press). */
        if (touched) {
            int ctl = cm_home_ctl_at(s, fb, tx, ty);
            g_cm_knob = 0;   /* a touch hides the knob's focus */
            cm_glide_sync();
            if (glide_flinging(&g_cm_gl)) {   /* a finger on a flinging list stops it and opens nothing */
                glide_stop(&g_cm_gl);
                g_cm_scroll = g_cm_glpx = glide_px(&g_cm_gl);
                if (ty >= dock_y(fb) + menu_hdrh(fb)) ctl = -1;   /* the header's controls still work */
            }
            c_press(tx, ty, ctl, true, g_cm_scroll);
        } else if (d && g_cm_sortpop) {   /* the orders are open: a turn only closes them */
            g_cm_sortpop = 0;
        } else if ((d || click) && cm_list_knob(s, fb, d, click)) {
            /* the Installed and Updates tabs: the knob moves the focus, its press fires it */
        } else if (d) {   /* a row per detent, gliding; quick detents add up from where it is heading */
            cm_glide_sync();
            float from = g_cm_gl.on ? g_cm_gl.goal : (float) g_cm_scroll;
            glide_to(&g_cm_gl, (float) cm_scroll_clamp((int) (from + 0.5f) + d * c_rrowh(fb), g_cm_smax), cm_ms());
            cm_glide_frame();
        }
        return true;
    }
    if (d && n && !g_cm_knob) { /* after a touch, the first detent shows the focus where it is */
        g_cm_knob = 1;
        d = 0;
    }
    if (click && n)
        g_cm_knob = 1;
    if (d && n && g_cm_view == CMV_CAT && g_cm_desc.r.h > 0) {   /* past the last button the knob scrolls the description */
        int nk = n;
        if (nk > 1 && ctl[nk - 1].kind == CMC_BACK) nk--;
        if (g_cm_sel == nk - 1 && (d > 0 || g_cm_desc.skip > 0)) {
            int ns = g_cm_desc.skip + d * 2;
            g_cm_desc.skip = ns < 0 ? 0 : ns > g_cm_desc.max ? g_cm_desc.max
                                                             : ns;
            return true;
        }
    }
    if (d && n) {
        int nk = n;   /* the back square is the last control and the knob skips it (the BACK button goes back) */
        if (nk > 1 && ctl[nk - 1].kind == CMC_BACK) nk--;
        g_cm_sel = ((g_cm_sel + d) % nk + nk) % nk;   /* every detent counts (a fast turn arrives as one delta) */
    }
    if (click && n && g_cm_sel < n) cm_fire(&ctl[g_cm_sel]);
    if (touched && g_cm_view == CMV_CAT && g_cm_desc.r.h > 0 &&
        in_rect(tx, ty, g_cm_desc.r.x, g_cm_desc.r.y, g_cm_desc.r.w, g_cm_desc.r.h)) {   /* a drag starts here */
        g_cm_dd.on = true;
        g_cm_dd.y0 = ty;
        g_cm_dd.s0 = g_cm_desc.skip;
        return true;
    }
    if (touched) {
        int pad = g_cm_view == CMV_KBD ? 0 : fb->h * 8 / 800;
        for (int k = 0; k < n; k++)
            if (in_rect(tx, ty, ctl[k].x - pad, ctl[k].y - pad, ctl[k].w + 2 * pad, ctl[k].h + 2 * pad)) {
                int v = g_cm_view, pg = g_cm_page, tab = g_cm_tab;
                g_cm_sel = k;
                g_cm_knob = 0;
                g_cm_press = k;   /* lit until the finger lifts (azmod_pane_drag, phase 2) */
                cm_fire(&ctl[k]);
                if (g_cm_view != v || g_cm_page != pg || g_cm_tab != tab)
                    g_cm_press = -1; /* a new page: index k is some other control there */
                break;
            }
    }
    return true;
}
/* ---- the home page's touch gesture (azmod_pane_drag decides it, c_press starts it) ----
 * A control is named by its kind and index ((kind << 16) | idx), never by its place in the
 * layout, so a release judged against a fresh layout can only fire the control that was pressed. */
static int cm_code(const cm_ctl *c) { return c->kind << 16 | (c->idx & 0xffff); }
/* The Installed and Updates tabs: the knob moves the focus a control at a time (the rows, their
 * buttons; never a checkbox, which a touch toggles) and scrolls it into view, as the Mod Menu's
 * list does; its press fires the focused control (a row opens its page). After a touch, the first
 * detent or press only shows the focus. false: not one of these tabs, or nothing to focus (the
 * knob then scrolls the page). */
/* the knob on the split, as on the Mod Menu: in the list a turn moves the selection (its page
 * follows) and a press goes into the page; there a turn and a press go to the mod's own screen, or
 * step through and press the page's buttons when it has none. BACK comes out (cm_back). */
static bool cm_split_knob(azmod_state *s, const azmod_fb *fb, int d, bool click) {
    cm_ctl ctl[CM_MAXCTL];
    int n = cm_layout(s, fb, ctl, 0), nb = g_cm_nbody < n ? g_cm_nbody : n, list[CM_MAXCTL], nl = 0, cur = -1;
    int bx, by, bw, bh, pad = fb->h * 8 / 800;
    cm_box(fb, &bx, &by, &bw, &bh);
    int rx = bx + bw / 2;
    if (g_cm_in) {
        if (cm_split_screen()) {
            az_event ev = {0, 0, 0, 0};
            if (d) {
                ev.kind = AZ_EV_KNOB;
                ev.dir = d > 0 ? 1 : -1;
            } else if (click) ev.kind = AZ_EV_CLICK;
            if (ev.kind) azcm_screen_input(g_cm_page, &ev);
            return true;
        }
        for (int k = 0; k < nb; k++)
            if (ctl[k].x >= rx && ctl[k].kind != CMC_CHK) {
                if (k == g_cm_rsel) cur = nl;
                list[nl++] = k;
            }
        if (!nl) {   /* nothing to press there: back in the list */
            g_cm_in = 0;
            return true;
        }
        if (click && cur >= 0) {
            g_cm_s = s;
            cm_fire(&ctl[list[cur]]);
            return true;
        }
        if (cur < 0) cur = 0;
        else if (d) cur = cur + d < 0 ? 0 : cur + d >= nl ? nl - 1
                                                          : cur + d;
        g_cm_knob = 1;
        g_cm_rsel = list[cur];
        return true;
    }
    for (int k = 0; k < nb; k++) {   /* the list's controls (the checkboxes are touched, not stepped) */
        if (ctl[k].x >= rx || ctl[k].kind == CMC_CHK) continue;
        if (cm_code(&ctl[k]) == g_cm_hfoc || (g_cm_hfoc < 0 && cm_code(&ctl[k]) == g_cm_isel)) cur = nl;
        list[nl++] = k;
    }
    if (!nl) return false;
    if (click) {
        if (g_cm_knob && cur >= 0) {
            const cm_ctl *c = &ctl[list[cur]];
            if (c->kind == CMC_MOD || c->kind == CMC_INSTCAT) {   /* into the page */
                g_cm_in = 1;
                g_cm_rsel = -1;
                if (!cm_split_screen())
                    for (int k = 0; k < nb; k++)
                        if (ctl[k].x >= rx && ctl[k].kind != CMC_CHK) {
                            g_cm_rsel = k;
                            break;
                        }
            } else {
                g_cm_s = s;
                cm_fire(c);
            }
        }
        return true;
    }
    if (!g_cm_knob || cur < 0) cur = cur < 0 ? 0 : cur;   /* the first detent after a touch shows where it is */
    else if (d) cur = cur + d < 0 ? 0 : cur + d >= nl ? nl - 1
                                                      : cur + d;
    g_cm_knob = 1;
    const cm_ctl *c = &ctl[list[cur]];
    g_cm_hfoc = cm_code(c);
    if (c->kind == CMC_MOD || c->kind == CMC_INSTCAT) cm_split_select(cm_code(c));
    int to = g_cm_scroll;
    if (c->y < by) to -= by - c->y + pad;
    else if (c->y + c->h > by + bh) to += c->y + c->h - (by + bh) + pad;
    if (to != g_cm_scroll) {
        cm_glide_sync();
        glide_to(&g_cm_gl, (float) cm_scroll_clamp(to, g_cm_smax), cm_ms());
        cm_glide_frame();
    }
    return true;
}
static bool cm_list_knob(azmod_state *s, const azmod_fb *fb, int d, bool click) {
    if (g_cm_tab == 0 && cm_isplit()) return cm_split_knob(s, fb, d, click);
    if (g_cm_tab != 0 && g_cm_tab != 3) return false;
    cm_ctl ctl[CM_MAXCTL];
    int n = cm_layout(s, fb, ctl, 0), nb = g_cm_nbody < n ? g_cm_nbody : n, list[CM_MAXCTL], nl = 0, cur = -1;
    for (int k = 0; k < nb; k++) {
        if (ctl[k].kind == CMC_CHK) continue;
        if (cm_code(&ctl[k]) == g_cm_hfoc) cur = nl;
        list[nl++] = k;
    }
    if (!nl) return false;
    if (click && g_cm_knob && cur >= 0) {
        g_cm_s = s;
        cm_fire(&ctl[list[cur]]);
        return true;
    }
    if (!g_cm_knob || cur < 0) cur = cur < 0 ? 0 : cur;
    else if (d) cur = cur + d < 0 ? 0 : cur + d >= nl ? nl - 1
                                                      : cur + d;
    g_cm_knob = 1;
    g_cm_hfoc = cm_code(&ctl[list[cur]]);
    int bx, by, bw, bh, pad = fb->h * 8 / 800;
    cm_box(fb, &bx, &by, &bw, &bh);
    const cm_ctl *c = &ctl[list[cur]];
    int to = g_cm_scroll;
    if (c->y < by) to -= by - c->y + pad;
    else if (c->y + c->h > by + bh) to += c->y + c->h - (by + bh) + pad;
    if (to != g_cm_scroll) {
        cm_glide_sync();
        glide_to(&g_cm_gl, (float) cm_scroll_clamp(to, g_cm_smax), cm_ms());
        cm_glide_frame();
    }
    return true;
}
/* the gesture still belongs to the page it started on */
static bool cm_home_live(const azmod_state *s) {
    return s->mode == AZ_COMMUNITY && g_cm_full && !cm_animating() && g_cm_view == CMV_HOME;
}
/* The finger's point during a drag (phase 1) and at the lift (2): the page is at the finger, and
 * a lift while moving flings it on (any speed a fling would not stop at at once). */
static bool cm_home_scroll_to(const azmod_fb *fb, int v, int phase) {
    (void) fb;
    int was = g_cm_scroll;
    cm_glide_sync();
    if (phase == 2) glide_release(&g_cm_gl, (float) v, cm_ms(), GLIDE_STOP_V);
    else glide_drag(&g_cm_gl, (float) v, cm_ms());
    cm_glide_frame();
    if (g_cm_scroll != was) g_cm_sortpop = 0;   /* the page moved under the orders: close them */
    return g_cm_scroll != was;
}
/* opening a page or switching a tab: a slow tap still counts (as a Mod Menu row's does) */
static bool cm_home_nav(int ctl) {
    int k = ctl >> 16;
    return k == CMC_TAB || k == CMC_TILE || k == CMC_STILE || k == CMC_SEEALL || k == CMC_MOD || k == CMC_INSTCAT || k == CMC_BACK ||
           k == CMC_SORTBTN || k == CMC_SORT || k == CMC_SORTOFF;
}
/* Reload (the not-loaded notice's, the header's Reload now): a press under 1 s restarts */
static bool cm_home_reload(int ctl) { return (ctl >> 16) == CMC_APPLY; }
/* The control under a point on the home page, padded for a finger, or -1. The header's controls
 * first; the page's only where the page shows (the header covers what scrolled up under it, and
 * nothing below the menu's bottom edge is drawn). Lays the page out, which also refreshes
 * g_cm_smax. */
static int cm_home_ctl_at(azmod_state *s, const azmod_fb *fb, int tx, int ty) {
    cm_ctl ctl[CM_MAXCTL];
    int n = cm_layout(s, fb, ctl, 0), nb = g_cm_nbody < n ? g_cm_nbody : n;
    /* a control the point is inside wins over a neighbour's finger margin (the rail's cells touch) */
    for (int pass = 0; pass < 2; pass++) {
        int pad = pass ? fb->h * 8 / 800 : 0;
        for (int k = nb; k < n; k++)
            if (in_rect(tx, ty, ctl[k].x - pad, ctl[k].y - pad, ctl[k].w + 2 * pad, ctl[k].h + 2 * pad))
                return cm_code(&ctl[k]);
    }
    if (ty < dock_y(fb) + menu_hdrh(fb) || ty >= dock_y(fb) + dock_h(fb))
        return -1;
    for (int pass = 0; pass < 2; pass++) {
        int pad = pass ? fb->h * 8 / 800 : 0;
        for (int k = 0; k < nb; k++)
            if (in_rect(tx, ty, ctl[k].x - pad, ctl[k].y - pad, ctl[k].w + 2 * pad, ctl[k].h + 2 * pad))
                return cm_code(&ctl[k]);
    }
    return -1;
}
/* a tap on the home page: fire the control with that code in the current layout */
static void cm_home_fire(azmod_state *s, const azmod_fb *fb, int code) {
    cm_ctl ctl[CM_MAXCTL];
    int n = cm_layout(s, fb, ctl, 0);
    g_cm_s = s;
    for (int k = 0; k < n; k++)
        if (cm_code(&ctl[k]) == code) {
            cm_fire(&ctl[k]);
            return;
        }
}
/* Community > a catalog mod's page: the description follows the finger in whole lines. Same
 * contract as azmod_changelog_drag (cm_input starts it; 1 = a step, 2 = the release). */
bool azmod_cm_drag(azmod_state *s, const azmod_fb *fb, int y, int phase) {
    (void) fb;
    if (!g_cm_dd.on) return false;
    if (s->mode != AZ_COMMUNITY || g_cm_view != CMV_CAT || g_cm_desc.r.h <= 0 || g_cm_desc.lh <= 0) {
        g_cm_dd.on = false;
        return false;
    }
    if (phase == 2) {
        g_cm_dd.on = false;
        return false;
    }
    int ns = g_cm_dd.s0 + (g_cm_dd.y0 - y) / g_cm_desc.lh;
    if (ns < 0) ns = 0;
    if (ns > g_cm_desc.max) ns = g_cm_desc.max;
    if (ns == g_cm_desc.skip) return false;
    g_cm_desc.skip = ns;
    return true;
}
/* while Community is open: redraw twice a second so the fetcher's status and catalog show up,
 * and at once when an icon finishes, so a tile's picture appears as soon as it is decoded */
static bool cm_tick(azmod_state *s) {
    static int t;
    static unsigned icons;
    if (s->mode != AZ_COMMUNITY) {
        g_cm_dir = 0;
        return false;
    }
    if (g_cm_dir) return true;                          /* the slide: a frame every tick */
    if (g_cm_full && g_cm_view == CMV_HOME && cm_glide_frame()) return true;   /* the scroll in motion */
    unsigned n = azcm_icons_done();
    if (n != icons) {
        icons = n;
        return true;
    }
    if (g_cm_reloading || (cm_busy() && !strcmp(g_cs.op, "refresh"))) return ++t % 3 == 0;   /* a spinner turns */
    return ++t % 15 == 0;
}

/* ---- community mods: the draw helpers a mod's page paints through (azmod-mod.h) --------
 * A mod draws in absolute device pixels; the context is a framebuffer view whose origin is
 * the mod's area, so every write clips to the area for free (as the changelog box does) and
 * a mod can never paint outside the rectangle the core handed it. */
void azcm_draw_init(az_draw *d, const azmod_fb *fb, az_rect a) {
    if (a.x < 0) {
        a.w += a.x;
        a.x = 0;
    }
    if (a.y < 0) {
        a.h += a.y;
        a.y = 0;
    }
    if (a.x + a.w > fb->w) a.w = fb->w - a.x;
    if (a.y + a.h > fb->h) a.h = fb->h - a.y;
    if (a.w < 0) a.w = 0;
    if (a.h < 0) a.h = 0;
    d->fb = *fb;
    d->fb.px = fb->px + (long) a.y * fb->stride + (long) a.x * 4;
    d->fb.w = a.w;
    d->fb.h = a.h;
    d->ox = a.x;
    d->oy = a.y;
    d->sh = fb->h;
}
/* Values from a community mod: clamp before any arithmetic, so nothing can overflow. */
static int cm_clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi
                                                                          : v; }
#define CM_XY(v) cm_clampi((v), -32768, 32767)
#define CM_WH(v) cm_clampi((v), 0, 32767)
void azcm_fill_rect(az_draw *d, int x, int y, int w, int h, uint32_t rgb) {
    if (d) fill_rect(&d->fb, CM_XY(x) - d->ox, CM_XY(y) - d->oy, CM_WH(w), CM_WH(h), rgb & 0x00FFFFFFu);
}
void azcm_draw_rect(az_draw *d, int x, int y, int w, int h, int t, uint32_t rgb) {
    if (!d || t <= 0) return;
    x = CM_XY(x);
    y = CM_XY(y);
    w = CM_WH(w);
    h = CM_WH(h);
    t = CM_WH(t);
    azcm_fill_rect(d, x, y, w, t, rgb);
    azcm_fill_rect(d, x, y + h - t, w, t, rgb);
    azcm_fill_rect(d, x, y, t, h, rgb);
    azcm_fill_rect(d, x + w - t, y, t, h, rgb);
}
void azcm_draw_text(az_draw *d, int x, int y, const char *s, int px, uint32_t rgb) {
    if (!d || !s) return;
    if (px < 8) px = 8;
    else if (px > 96) px = 96;
    x = CM_XY(x);
    y = CM_XY(y);
    int h = px * 3 / 2;
    if (azmod_ttf_menu && azmod_ttf_menu(&d->fb, x - d->ox, y - d->oy, 0, h, s, px, 0, 2, rgb & 0x00FFFFFFu)) return;
    draw_text(&d->fb, x - d->ox, y - d->oy + (h - 7) / 2, s, 1, rgb & 0x00FFFFFFu);
}
int azcm_text_width(const char *s, int px) {
    if (!s) return 0;
    if (px < 8) px = 8; /* as draw_text draws it: the face renders glyphs at this size */
    else if (px > 96) px = 96;
    int n = (int) strlen(s), w = azmod_ttf_menu_width ? azmod_ttf_menu_width(s, n, px, 2) : 0;
    return w > 0 ? w : n * px / 2;
}
void azcm_draw_header(az_draw *d, const char *title) {
    if (!d || !title) return;
    int rh = azcm_row_height(d), px = rh * 45 / 100;
    if (px < 12) px = 12;
    int pad = px / 2;
    if (azmod_ttf_menu) azmod_ttf_menu(&d->fb, pad, 0, 0, rh, title, px, 0, 1 | 2, C_HI);
    else draw_text(&d->fb, pad, (rh - 7) / 2, title, 1, C_HI);
    fill_rect(&d->fb, pad, rh - 2, d->fb.w - 2 * pad, 1, C_CHECK_BOX);
}
void azcm_draw_checkbox(az_draw *d, int x, int y, int size, int on, int hi) {
    if (d) draw_checkbox(&d->fb, CM_XY(x) - d->ox, CM_XY(y) - d->oy, cm_clampi(size, 1, 512), on != 0, hi != 0);
}
uint32_t azcm_color(int role) {
    /* clang-format off */
    switch (role) {
    case AZ_COL_TEXT:     return C_TEXT;
    case AZ_COL_TEXT_DIM: return C_TEXT_DIM;
    case AZ_COL_HI:       return C_HI;
    case AZ_COL_HI_TEXT:  return C_HI_TEXT;
    case AZ_COL_BG:       return C_ROW_BG;
    default:              return C_TEXT;
    }
    /* clang-format on */
}
/* the right pane's row height (c_rrowh), from the screen height: the clipped fb is only the
 * mod's area, far too small to size rows from */
int azcm_row_height(az_draw *d) { return d && d->sh > 0 ? d->sh * 50 / 800 : 50; }
