/* SPDX-License-Identifier: GPL-3.0-only */
/* ---- Vocal Indicator: where a track's vocals are, above each deck's overview waveform -------
 * Included by extras.c. It rides stem_marks.c's worker (vx_poll, from its loop), library
 * connections (sm_db_for) and lock (g_sm_mu), as Preview Hot Cues does.
 *
 * Data: the track's ANLZ0000.2EX, section PVDI (vd_anlz.h): one level 0..4 per ~46 ms. The
 * worker polls each deck's loaded track (azstem's deck snapshot: content_id and title), finds
 * its analysis path and length in the library of every stick that holds (content_id, title),
 * and reads the PVDI section. Two sticks that disagree, a track without PVDI or with an empty
 * one, a track without a length, or anything unreadable: rekordbox has no answer. The levels from
 * the track's vocals stem (vd_from_stem_env) are read too, once azstem holds its final envelope,
 * and show first; rekordbox's show when the track has no stems. Neither: nothing is drawn.
 *
 * Draw: from the deck panel's paintOverChildren (extras.c ht_paintover: PlayerInfo{2,4}
 * DeckWidget, its deck from the ctor's field). The panel's PlayerInfoWaveformWidget is the
 * child whose child holds the overview (gui::OverviewWaveform1200PointWidget, by vtable); the
 * strip spans that overview's x range, centered in the empty band between the lowest visible
 * widget above it and its top (the cue marker row). No such band (2/4 DECK and 4 DECK, where the
 * overview sits under the title bar): no strip. Column x of a w px overview covers
 * [x L / w, (x + 1) L / w) of a track L s long, and shows the strongest frame there; levels
 * 1..4 are one blue at rising opacity over the panel's own background (a deeper blue on a light
 * theme), level 0 is not drawn.
 *
 * Repaint: the title setter (every frame, per deck on screen) asks for one repaint of the panel
 * each time what the strip should show changes. */
#include "vd_anlz.h"
#include "../common/azdeckinfo.h"

#define VX_H 4         /* strip height, px */
#define VX_MIN_BAND 10 /* the empty band it needs above the cue markers */
#define VX_DASH 4      /* dash and gap, px: the overview's own dashed rows' rhythm */
#define VX_GAP 2
#define VX_RGB 0x5cb8ffu       /* over a dark ground */
#define VX_RGB_LIGHT 0x1060c8u /* over a light ground */
#define VX_MAXW 1280
#define VX_POLL_MS 250
#define VX_FILE_MAX (64u << 20)

static int vx_on(void) { return az_on(AZF_VOCAL_INDICATOR) && az_active(AZF_VOCAL_INDICATOR); }
static int vx_row_on(int f) { return vx_on() && az_on(f) && az_active(f); }
static int vx_prev_on(void) { return vx_row_on(AZF_VOCAL_IND_PREVIEW); }
static int vx_wave_on(void) { return vx_row_on(AZF_VOCAL_IND_WAVE); }

/* Under g_sm_mu. st: 0 nothing, 2 known, 3 unknown. gen changes with every new answer. */
static struct vx_deck {
    unsigned cid, gen;
    int st;
    char title[160];
    struct vd_track t;  /* rekordbox's */
    unsigned len_s;
    struct vd_track ts; /* from the vocals stem */
    unsigned len_ss;
} g_vx[AZMOD_STEM_DECKS];
/* g_sm_mu held: what deck d's strips show now, its levels and length; VD_NONE = nothing */
static int vx_show(int d, const struct vd_track **t, unsigned *len) {
    struct vx_deck *e = &g_vx[d];
    int src = vd_pick_source(e->st == 2 && e->t.n && e->len_s, e->ts.n && e->len_ss);
    if (t)
        *t = src == VD_STEM ? &e->ts : &e->t;
    if (len)
        *len = src == VD_STEM ? e->len_ss : e->len_s;
    return src;
}

/* ---- the worker ---------------------------------------------------------------------------- */
static int vx_pread(void *ctx, uint8_t *b, size_t n, uint64_t off) { return ph_pread(ctx, b, n, off); }
/* the PVDI of one stick's .2EX beside the library's .DAT path: 1 found, 0 none, -1 unreadable */
static int vx_from_file(const char *mount, const char *adp, struct vd_track *t) {
    char p[700];
    size_t L = strlen(adp);
    if (L <= 4 || L + strlen(mount) >= sizeof p || strcasecmp(adp + L - 4, ".DAT"))
        return -1;
    snprintf(p, sizeof p, "%s%.*s.2EX", mount, (int) (L - 4), adp);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct stat sb;
    int r = fstat(fd, &sb) == 0 && sb.st_size > 0
                ? vd_anlz_read(vx_pread, &fd, (uint64_t) sb.st_size, VX_FILE_MAX, t)
                : -1;
    close(fd);
    return r;
}
/* (cid, title) on every stick: 1 known (t, len_s filled), 0 unknown */
static int vx_resolve(unsigned cid, const char *title, struct vd_track *t, unsigned *len_s,
                      FILE *lg) {
    DIR *rd = opendir("/media/usb");
    if (!rd)
        return 0;
    struct dirent *de;
    int hits = 0, bad = 0;
    struct vd_track a = {0};
    unsigned alen = 0;
    while ((de = readdir(rd)) && !bad) {
        if (de->d_name[0] == '.')
            continue;
        char mount[128];
        if (snprintf(mount, sizeof mount, "/media/usb/%s", de->d_name) >= (int) sizeof mount)
            continue;
        void *db = sm_db_for(mount, lg);
        if (!db)
            continue;
        void *st = 0;
        if (g_sq.prepare(db,
                         "SELECT analysisDataFilePath, length FROM content WHERE content_id = ?1 "
                         "AND title = ?2",
                         -1, &st, 0) != 0 ||
            !st) {
            bad++;
            break;
        }
        g_sq.bind_int64(st, 1, cid);
        g_smsq.bind_text(st, 2, title, -1, (void *) -1 /* SQLITE_TRANSIENT */);
        while (!bad && g_sq.step(st) == SQLITE_ROW) {
            const unsigned char *ap = g_sq.column_text(st, 0);
            long long secs = g_sq.column_int64(st, 1);
            struct vd_track b = {0};
            int k = ap && ap[0] == '/' && secs > 0 && secs < 24 * 3600
                        ? vx_from_file(mount, (const char *) ap, &b)
                        : -1;
            if (lg)
                fprintf(lg, "  vocal cid=%u %s%s -> %d n=%u\n", cid, mount,
                        ap ? (const char *) ap : "-", k, k == 1 ? b.n : 0);
            if (k != 1) {
                bad++;
            } else if (!hits) {
                a = b;
                alen = (unsigned) secs;
                hits++;
                continue;
            } else if (b.n != a.n || b.hop != a.hop || b.rate != a.rate || (unsigned) secs != alen ||
                       memcmp(b.v, a.v, a.n)) {
                bad++;
            }
            free(b.v);
        }
        g_sq.finalize(st);
    }
    closedir(rd);
    if (!hits || bad) {
        free(a.v);
        return 0;
    }
    *t = a;
    *len_s = alen;
    return 1;
}
static void vx_set(int d, unsigned cid, const char *title, int st, struct vd_track *t,
                   unsigned len_s) {
    uint8_t *old, *olds;
    sm_lock();
    struct vx_deck *e = &g_vx[d];
    old = e->t.v;
    olds = e->ts.v;
    memset(&e->ts, 0, sizeof e->ts);
    e->len_ss = 0;
    e->cid = cid;
    snprintf(e->title, sizeof e->title, "%s", title);
    e->st = st;
    if (t)
        e->t = *t;
    else
        memset(&e->t, 0, sizeof e->t);
    e->len_s = len_s;
    e->gen++;
    sm_unlock();
    free(old);
    free(olds);
}
/* The levels from deck d's stems; t = NULL, none. */
static void vx_set_stems(int d, struct vd_track *t, unsigned len_s) {
    sm_lock();
    struct vx_deck *e = &g_vx[d];
    uint8_t *old = e->ts.v;
    if (t)
        e->ts = *t;
    else
        memset(&e->ts, 0, sizeof e->ts);
    e->len_ss = len_s;
    e->gen++;
    sm_unlock();
    free(old);
}
/* worker: each deck's loaded track, at most every VX_POLL_MS. Off = everything let go. */
static FILE *g_vx_lg;
static int g_vx_budget = 400; /* log lines */
/* worker only: the envelope generation the levels from stems were built from + 1, 0 = none */
static unsigned g_vx_stemgen[AZMOD_STEM_DECKS];
#define VX_STEM_VOCALS 0 /* azstem's slots */
#define VX_STEM_OTHER 1
/* Deck d's levels from its stems, whenever their envelope appears or changes. */
static void vx_from_stems(int d, unsigned cid, FILE *lg) {
    static int (*env_fn)(int, unsigned, const unsigned char **, int *, int *, long *, int *,
                         unsigned *);
    static unsigned (*gen_fn)(int);
    if (!env_fn || !gen_fn) {
        *(void **) &env_fn = dlsym(RTLD_DEFAULT, "azstem_track_env");
        *(void **) &gen_fn = dlsym(RTLD_DEFAULT, "azstem_env_gen");
        if (!env_fn || !gen_fn)
            return;
    }
    const unsigned char *env = 0;
    int nb = 0, ns = 0, rate = 0;
    long frames = 0;
    unsigned gen = 0;
    if (!env_fn(d, cid, &env, &nb, &ns, &frames, &rate, &gen) || nb <= 0 || ns <= VX_STEM_OTHER) {
        if (g_vx_stemgen[d]) { /* its stems went */
            g_vx_stemgen[d] = 0;
            vx_set_stems(d, 0, 0);
        }
        return;
    }
    if (g_vx_stemgen[d] == gen + 1)
        return;
    struct vd_track t = {0};
    int ok = vd_from_stem_env(env + (size_t) VX_STEM_VOCALS * nb, env + (size_t) VX_STEM_OTHER * nb,
                              (uint32_t) nb, (uint64_t) frames, (uint32_t) rate, &t);
    if (gen_fn(d) != gen) { /* rebuilt under the copy: the next poll sees the new one */
        free(t.v);
        return;
    }
    g_vx_stemgen[d] = gen + 1;
    unsigned len = (unsigned) ((frames + rate / 2) / rate);
    if (lg)
        fprintf(lg, "deck %d cid=%u -> from stems ok=%d n=%u len=%u\n", d, cid, ok, t.n, len);
    vx_set_stems(d, ok ? &t : 0, ok ? len : 0);
}
static void vx_poll(void) {
    static long long at;
    static azstem_deck_snapshot_fn snap;
    if (drag_now_ms() - at < VX_POLL_MS)
        return;
    at = drag_now_ms();
    int on = vx_prev_on() || vx_wave_on();
    if (on && !snap)
        *(void **) &snap = dlsym(RTLD_DEFAULT, "azstem_deck_snapshot");
    for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
        struct azstem_deck_snap s;
        memset(&s, 0, sizeof s);
        s.size = (int) sizeof s;
        int known = on && snap && snap(d, &s) == 0;
        sm_lock();
        unsigned cid = g_vx[d].cid;
        int st = g_vx[d].st, same = known && s.content_id == cid && !strcmp(s.title, g_vx[d].title);
        sm_unlock();
        if (!known || s.loaded == 0 || !s.content_id || !s.title[0]) {
            if (st && (!on || s.loaded == 0))
                vx_set(d, 0, "", 0, 0, 0);
            continue;
        }
        if (s.loaded != 1)
            continue;
        if (same && st) {
            vx_from_stems(d, cid, g_vx_budget > 0 ? g_vx_lg : 0);
            continue;
        }
        g_vx_stemgen[d] = 0;
        if (st && !same)
            vx_set(d, 0, "", 0, 0, 0); /* another track: the old strip goes at once */
        if (!g_vx_lg && g_vx_budget > 0) {
            g_vx_lg = azlog_open("/tmp/azmod-vocalindicator.log", "a");
            if (g_vx_lg)
                setvbuf(g_vx_lg, NULL, _IOLBF, 0);
        }
        FILE *lg = g_vx_budget-- > 0 ? g_vx_lg : 0;
        struct vd_track t = {0};
        unsigned len = 0;
        int ok = vx_resolve(s.content_id, s.title, &t, &len, lg);
        if (lg)
            fprintf(lg, "deck %d cid=%u -> %s n=%u len=%u\n", d, s.content_id,
                    ok ? "known" : "unknown", t.n, len);
        if (ok && !t.n) { /* a section with no frames: nothing was detected or nothing was run */
            free(t.v);
            t.v = 0;
            ok = 0;
        }
        vx_set(d, s.content_id, s.title, ok ? 2 : 3, ok ? &t : 0, len);
        if (!ok)
            free(t.v);
        vx_from_stems(d, s.content_id, lg);
    }
}

/* ---- the detailed waveform's scroll (message thread, from waveform.c azm_upd_enter) ----- */
/* EP147's update of deck d's detailed waveform, every frame it is built: the playhead in ms of
 * track time and the zoom (the zoom level over the deck's tempo). EP147 scrolls the image so
 * that x = (t - pos) * 0.15 * zoom from its playhead line, t in ms. */
static long long g_vx_wpos[AZMOD_STEM_DECKS];
static float g_vx_wzoom[AZMOD_STEM_DECKS];
static long g_vx_wwb[AZMOD_STEM_DECKS];
static void vx_upd(int d, long wwb, long pos_p, float zoom) {
    if (!pos_p || (pos_p & 7))
        return;
    g_vx_wpos[d] = *(volatile long long *) pos_p;
    g_vx_wzoom[d] = zoom;
    g_vx_wwb[d] = wwb;
}
/* ---- the paint (message thread) ------------------------------------------------------------ */
static void vx_bar(void *g, int x, int y, int w, int h, uint32_t argb) {
#ifdef AZMOD_HAVE_DAYMODE
    t_dm_bypass = 1; /* blended over whatever the theme painted */
#endif
    ((void (*)(void *, uint32_t *)) EP_23af140)(g, &argb);
    ((void (*)(void *, int, int, int, int)) EP_23af680)(g, x, y, w, h);
#ifdef AZMOD_HAVE_DAYMODE
    t_dm_bypass = 0;
#endif
}
static struct {
    unsigned gen;
    int w, valid; /* valid: col[] holds deck gen's levels for width w */
    uint8_t col[VX_MAXW];
} g_vx_cols[AZMOD_STEM_DECKS];
/* what the last repaint was asked for: gen + 1, 0 = nothing. One request per change is enough:
 * JUCE merges dirty areas, so the paint that follows covers the whole panel whatever partial
 * paints come between, and a panel that is not showing is painted whole when it shows. */
static unsigned g_vx_asked[AZMOD_STEM_DECKS];

static uintptr_t vx_full_vt(uintptr_t c) { /* the primary vtable of a child's full object, or 0 */
    uintptr_t v = *(volatile uintptr_t *) c;
    if (!ht_ptr_ok(v))
        return 0;
    uintptr_t full = c + (uintptr_t) *(volatile long *) (v - 0x10);
    return ht_ptr_ok(full) ? *(volatile uintptr_t *) full : 0;
}
static int vx_kids(uintptr_t c, uintptr_t **kids) {
    uintptr_t k = *(volatile uintptr_t *) (c + 0x40); /* childComponentList.data */
    int n = *(volatile int *) (c + 0x50);             /* .numUsed */
    if (!ht_ptr_ok(k) || n <= 0 || n > 64)
        return 0;
    *kids = (uintptr_t *) k;
    return n;
}
/* The strip's rect in the panel's coordinates: the overview's x range, centered in the band
 * above the waveform group. 1, or 0 when the panel has no overview or no band for it. */
static int vx_geom(uintptr_t panel, int r[4]) {
    uintptr_t *kids, *k2, *k3;
    int n = vx_kids(panel, &kids), gi = -1, ox = 0, ow = 0;
    for (int i = 0; i < n && gi < 0; i++) {
        uintptr_t g = kids[i];
        int n2 = ht_ptr_ok(g) ? vx_kids(g, &k2) : 0;
        for (int j = 0; j < n2 && gi < 0; j++) {
            uintptr_t w = k2[j];
            int n3 = ht_ptr_ok(w) ? vx_kids(w, &k3) : 0;
            for (int m = 0; m < n3; m++)
                if (ht_ptr_ok(k3[m]) && vx_full_vt(k3[m]) == OV_WIDGET_VT) {
                    gi = i;
                    ox = *(volatile int *) (g + 0x20) + *(volatile int *) (w + 0x20);
                    ow = *(volatile int *) (w + 0x28);
                    break;
                }
        }
    }
    if (gi < 0 || ow <= 0 || ow > VX_MAXW)
        return 0;
    int gy = *(volatile int *) (kids[gi] + 0x24), top = 0;
    for (int i = 0; i < n; i++) {
        uintptr_t c = kids[i];
        if (i == gi || !ht_ptr_ok(c) || !(*(volatile uint32_t *) (c + 0xc0) & 2))
            continue; /* hidden overlays (input select, analyzing) do not take the band */
        int cx = *(volatile int *) (c + 0x20), cy = *(volatile int *) (c + 0x24),
            cw = *(volatile int *) (c + 0x28), ch = *(volatile int *) (c + 0x2c);
        if (cx >= ox + ow || cx + cw <= ox || cy >= gy)
            continue; /* not above the overview */
        if (cy + ch > gy)
            return 0; /* reaches into the group: no band */
        if (cy + ch > top)
            top = cy + ch;
    }
    if (gy - top < VX_MIN_BAND)
        return 0;
    r[0] = ox;
    r[1] = top + (gy - top - VX_H) / 2;
    r[2] = ow;
    r[3] = VX_H;
    return 1;
}
/* deck d's levels for a w px strip into g_vx_cols[d]; 1 = something to draw */
static int vx_cols(int d, int w) {
    sm_lock();
    struct vx_deck *e = &g_vx[d];
    const struct vd_track *t;
    unsigned len;
    int ok = vx_prev_on() && vx_show(d, &t, &len) != VD_NONE;
    if (ok && !(g_vx_cols[d].valid && g_vx_cols[d].gen == e->gen && g_vx_cols[d].w == w)) {
        vd_columns(t, len, g_vx_cols[d].col, w);
        g_vx_cols[d].gen = e->gen;
        g_vx_cols[d].w = w;
        g_vx_cols[d].valid = 1;
    }
    sm_unlock();
    return ok;
}
/* the panel's paintOverChildren, deck d (extras.c ht_paintover: panel and deck checked) */
static void vx_strip(void *g, int x, int y, int w, int h, const uint8_t *col);
static void vx_paint(void *g, uintptr_t panel, int d) {
    int r[4];
    if (d < 0 || d >= AZMOD_STEM_DECKS || !vx_geom(panel, r) || !vx_cols(d, r[2]))
        return;
    uint8_t dash[VX_MAXW];
    for (int x = 0; x < r[2]; x++) /* dashes on a fixed rhythm */
        dash[x] = (x % (VX_DASH + VX_GAP)) < VX_DASH ? g_vx_cols[d].col[x] : 0;
    vx_strip(g, r[0], r[1], r[2], r[3], dash);
}
static unsigned g_vx_wasked[AZMOD_STEM_DECKS]; /* as g_vx_asked, for the lanes */
static int vx_wave_want(int d) { /* g_sm_mu held */
    return vx_wave_on() && vx_show(d, 0, 0) != VD_NONE ? (int) g_vx[d].gen + 1 : 0;
}
/* the title setter, deck d on screen (message thread): repaint the panel when what the strip
 * should show changes */
static void vx_title(uintptr_t panel, int d) {
    if (d < 0 || d >= AZMOD_STEM_DECKS || !ht_ptr_ok(panel))
        return;
    sm_lock();
    unsigned want = vx_prev_on() && vx_show(d, 0, 0) != VD_NONE ? g_vx[d].gen + 1 : 0;
    sm_unlock();
    if (want != g_vx_asked[d]) {
        g_vx_asked[d] = want;
        ((void (*)(void *)) JUCE_COMPONENT_REPAINT)((void *) panel);
    }
    sm_lock();
    unsigned wwant = (unsigned) vx_wave_want(d);
    sm_unlock();
    if (wwant != g_vx_wasked[d]) { /* the lanes: the WaveformView this panel sits in */
        uintptr_t v = panel;
        for (int k = 0; k < 8 && ht_ptr_ok(v) && *(volatile uintptr_t *) v != PM_VIEW_VT; k++)
            v = *(volatile uintptr_t *) (v + 0x18);
        if (ht_ptr_ok(v) && *(volatile uintptr_t *) v == PM_VIEW_VT) {
            g_vx_wasked[d] = wwant;
            ((void (*)(void *)) JUCE_COMPONENT_REPAINT)((void *) v);
        }
    }
}

/* ---- above the detailed waveform (WaveformView's paintOverChildren, extras.c pm_poc_hook) - */
#define VX_WTOP 25 /* in the gap between the lane's top row of beat ticks (22 px) and the waveform */
/* deck d's level for each of w screen columns from x0, with EP147's playhead line at px */
static int vx_wave_cols(int d, int x0, int w, int px, uint8_t *col) {
    float zoom = g_vx_wzoom[d];
    if (!(zoom > 0.0f) || w <= 0 || w > VX_MAXW)
        return 0;
    sm_lock();
    int ok = vx_wave_want(d) != 0;
    if (ok) {
        const struct vd_track *t;
        vx_show(d, &t, 0);
        vd_wave_columns(t, (double) g_vx_wpos[d], 0.15 * (double) zoom, x0 - px, col, w);
    }
    sm_unlock();
    return ok;
}
static void vx_strip(void *g, int x, int y, int w, int h, const uint8_t *col) {
    static const uint8_t alpha_dark[VD_MAX_LEVEL + 1] = {0, 0x55, 0x8c, 0xc4, 0xff};
    static const uint8_t alpha_light[VD_MAX_LEVEL + 1] = {0, 0x70, 0xa0, 0xd0, 0xff};
    int light = 0;
#ifdef AZMOD_HAVE_DAYMODE
    light = __atomic_load_n(&g_dm_on, __ATOMIC_RELAXED) && dm_flip_on();
#endif
    const uint8_t *alpha = light ? alpha_light : alpha_dark;
    uint32_t rgb = light ? VX_RGB_LIGHT : VX_RGB;
    for (int s = 0; s < w;) {
        int lv = col[s], e = s + 1;
        while (e < w && col[e] == lv)
            e++;
        if (lv)
            vx_bar(g, x + s, y, e - s, h, (uint32_t) alpha[lv] << 24 | rgb);
        s = e;
    }
}
/* the view's paintOverChildren: each deck's lane on screen, a strip along its top, clear of the
 * view's other visible children (the deck panels, the Beat FX panel) */
/* gui::waveform::DeckInfoFieldWidget and the DeckInfo{2,4}DeckFieldWidget inside it: containers
 * as wide as the deck info area (584 px) that draw nothing themselves, only their panels. A
 * panel (DeckInfo{2,4}DeckWidget) is as wide too but paints its own background in x 0..172 only
 * (its body and the divider at 170, EP147's paint and extras.c pl_panel_paint_hook alike). */
#define VX_DIF_VT EP_2d0b700
#define VX_DIF2_VT EP_2cee580
#define VX_DIF4_VT EP_2cfe3a8
#define VX_PANEL_W 172
static void vx_cut_rect(const int cr[4], int px, int *xa, int *xb) {
    if (cr[0] + cr[2] <= px) {
        if (cr[0] + cr[2] > *xa)
            *xa = cr[0] + cr[2];
    } else if (cr[0] >= px) {
        if (cr[0] < *xb)
            *xb = cr[0];
    } else {
        *xb = *xa; /* over the playhead: no strip */
    }
}
/* Narrow [*xa, *xb) of the strip at row y to what component c (visible) leaves of it: whatever
 * c draws left of the playhead px moves the start, right of it the end. The deck info
 * containers draw nothing of their own and a panel only its 172 px: both are looked into (up
 * to `deep` levels) for what their children draw; anything else counts by its bounds. */
static void vx_cut(uintptr_t c, int y, int px, int *xa, int *xb, int deep) {
    int cr[4];
    if (!ht_ptr_ok(c) || !(*(volatile uint32_t *) (c + 0xc0) & 2) || !ht_screen_rect(c, cr) ||
        cr[1] >= y + VX_H || cr[1] + cr[3] <= y || cr[0] >= *xb || cr[0] + cr[2] <= *xa)
        return;
    uintptr_t v = vx_full_vt(c);
    int container = v == VX_DIF_VT || v == VX_DIF2_VT || v == VX_DIF4_VT;
    int panel = v == KT_DI2_VT || v == KT_DI4_VT;
    if (deep <= 0 || !(container || panel)) {
        vx_cut_rect(cr, px, xa, xb);
        return;
    }
    if (panel) {
        int own[4] = {cr[0], cr[1], cr[2] < VX_PANEL_W ? cr[2] : VX_PANEL_W, cr[3]};
        vx_cut_rect(own, px, xa, xb);
    }
    uintptr_t kids = *(volatile uintptr_t *) (c + 0x40);
    int n = *(volatile int *) (c + 0x50);
    for (int k = 0; ht_ptr_ok(kids) && k < n && k < 64; k++)
        vx_cut(*(volatile uintptr_t *) (kids + 8 * k), y, px, xa, xb, deep - 1);
}
static void vx_wave_paint(uintptr_t view, void *g) {
    int rv[4];
    if (!g || !vx_wave_on() || pm_trackinfo_open(view) || !ht_screen_rect(view, rv))
        return;
    uintptr_t kids = *(volatile uintptr_t *) (view + 0x40);
    int nk = *(volatile int *) (view + 0x50);
    if (!ht_ptr_ok(kids) || nk <= 0 || nk > 64)
        return;
    static uint8_t col[VX_MAXW];
    for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
        int r[4];
        uintptr_t box = pm_deckbox(view, d, r);
        uintptr_t ph = box ? pm_child(box, PM_PLAYHEAD_VT, 1) : 0;
        if (!ph || r[2] <= 0 || r[2] > VX_MAXW || r[3] < VX_WTOP + VX_H)
            continue;
        int px = r[0] + *(volatile int *) (ph + 0x20) + *(volatile int *) (ph + 0x28) / 2;
        int y = r[1] + VX_WTOP, xa = r[0], xb = r[0] + r[2];
        uintptr_t lane = box; /* the view's child that holds this lane */
        for (int k = 0; k < 8 && ht_ptr_ok(lane); k++) { /* and inside every level of it: the */
            int ar[4];                                    /* lane's field (1030 wide) clips it */
            if (ht_screen_rect(lane, ar)) {
                if (ar[0] > xa)
                    xa = ar[0];
                if (ar[0] + ar[2] < xb)
                    xb = ar[0] + ar[2];
            }
            uintptr_t up = *(volatile uintptr_t *) (lane + 0x18);
            if (up == view)
                break;
            lane = up;
        }
        for (int k = 0; k < nk; k++) { /* a visible sibling over the strip cuts it short */
            uintptr_t c = *(volatile uintptr_t *) (kids + 8 * k);
            if (c != lane)
                vx_cut(c, y, px, &xa, &xb, 4);
        }
        if (xb - xa < 8)
            continue;
        if (vx_wave_cols(d, xa, xb - xa, px, col))
            vx_strip(g, xa - rv[0], y - rv[1], xb - xa, VX_H, col);
    }
}
