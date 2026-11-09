/* SPDX-License-Identifier: GPL-3.0-only */
/* ---- Preview Hot Cues: hot cue marks on the Browse TRACK list's PREVIEW waveforms ------
 * Included by stem_marks.c: it rides that file's row identity (the list's reply page,
 * title-checked against EP147's row ring), its library connections and worker, and its row
 * repaint (prow + sm_tick).
 *
 * EP147 draws a row's PREVIEW cell with a gui::PreviewableWaveform component
 * (refreshComponentForCell, column 1): its paint stretches the row's preview image over the
 * component's full width, so a time t of a track of length L is at x = w * t / L, where the
 * playhead line is drawn too. The marks are drawn after that paint, into the same Graphics:
 * one thin bar per hot cue, in its color (EP147's own setColour and fillRect).
 *
 * Hot cues are not in the library database (its cue table is empty in rekordbox exports);
 * they are in the track's analysis file. The worker reads them: the row's analysis path and
 * length from the library of each stick that holds (content_id, title), then the .EXT file's
 * colored list, or the .DAT file's (hc_anlz.h). Nothing is read on the message thread; a
 * row whose answer is not known yet is drawn without marks and repainted when it lands.
 *
 * EP147, gui::PreviewableWaveform (vt 0x2d994e8, ctor 0x1e03428, paint = slot 26 0x1e02d18):
 *   +0x28 / +0x2c width / height; +0xc8 the owning list's margins (two ints: x, y), passed
 *   to the ctor as TrackListWidget + 0x280; +0xd0 the image is drawn unscaled (no marks
 *   then); +0x108 the loading bar is drawn instead of the image (no marks); +0xf8 the row. */
#include "hc_anlz.h"

#define PH_VT EP_2d994e8
#define PH_PAINT_SLOT (PH_VT + 26 * 8)
#define PH_PAINT_ORIG EP_1e02d18
#define PH_OWNER_OFF 0x280 /* the list's margins inside TrackListWidget */
#define PH_CACHE 512
#define PH_Q 32
#define PH_FILE_MAX (16 << 20) /* an analysis file larger than this is not read */
#define PH_DEFAULT_RGB 0x28e214u /* a hot cue with no stored color: green */

static int g_ph_on; /* installed */
static int ph_shown(void) {
    return g_ph_on && az_on(AZF_PREVIEW_HOT_CUES) && az_active(AZF_PREVIEW_HOT_CUES);
}
/* The answers, under g_sm_mu. st: 1 queued, 2 known (n may be 0), 3 unknown */
static struct ph_ent {
    unsigned cid, h;
    unsigned char st, n, stale;
    long long used;
    uint16_t frac[HC_MAX]; /* where in the track, 0..65535 */
    uint32_t rgb[HC_MAX];
} g_phc[PH_CACHE];
static struct {
    unsigned cid, h;
    long long at; /* queued then (ms) */
    char title[160];
} g_phq[PH_Q];
static int g_phq_n;
static struct ph_ent *ph_ent(unsigned cid, unsigned h, int create) { /* g_sm_mu held */
    struct ph_ent *old = &g_phc[0];
    for (int i = 0; i < PH_CACHE; i++) {
        if (g_phc[i].st && g_phc[i].cid == cid && g_phc[i].h == h)
            return &g_phc[i];
        if (g_phc[i].used < old->used)
            old = &g_phc[i];
    }
    if (!create)
        return 0;
    memset(old, 0, sizeof *old);
    old->cid = cid;
    old->h = h;
    return old;
}
/* Message thread (a paint): the marks of (cid, title): their count (0 = none or unknown), or
 * -1 while the first answer is on its way. A new row is queued once; a stale answer is still
 * drawn while it is checked again. */
static int ph_get(unsigned cid, const char *title, uint16_t *frac, uint32_t *rgb) {
    unsigned h = sm_hash(title);
    int n;
    sm_lock();
    struct ph_ent *e = ph_ent(cid, h, 1);
    e->used = drag_now_ms();
    int ask = !e->st || ((e->st == 2 || e->st == 3) && e->stale);
    if (ask && g_phq_n == PH_Q) {
        /* full (a scroll): the oldest request makes room, its row asks again when it paints */
        struct ph_ent *o = ph_ent(g_phq[0].cid, g_phq[0].h, 0);
        if (o && o->st == 1)
            o->st = 0;
        else if (o)
            o->stale = 1;
        memmove(&g_phq[0], &g_phq[1], (size_t) (PH_Q - 1) * sizeof g_phq[0]);
        g_phq_n--;
    }
    if (ask) {
        g_phq[g_phq_n].cid = cid;
        g_phq[g_phq_n].h = h;
        g_phq[g_phq_n].at = drag_now_ms();
        snprintf(g_phq[g_phq_n].title, sizeof g_phq[g_phq_n].title, "%s", title);
        g_phq_n++;
        if (!e->st)
            e->st = 1;
        e->stale = 0;
        pthread_cond_signal(&g_sm_cv);
    }
    if (e->st == 2) {
        n = e->n;
        memcpy(frac, e->frac, sizeof e->frac);
        memcpy(rgb, e->rgb, sizeof e->rgb);
    } else {
        n = e->st == 3 ? 0 : -1;
    }
    sm_unlock();
    return n;
}
static int ph_queued(void) { /* g_sm_mu held */
    return g_phq_n;
}
/* worker: n bytes of the file at off, 1 when all of them were read */
static int ph_pread(void *ctx, uint8_t *b, size_t n, uint64_t off) {
    int fd = *(int *) ctx;
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, b + got, n - got, (off_t) (off + got));
        if (r <= 0)
            return 0;
        got += (size_t) r;
    }
    return 1;
}
/* worker: the cue sections of an analysis file (hc_anlz_pick), or 0 */
static uint8_t *ph_read(const char *path, size_t *n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    struct stat sb;
    uint8_t *b = 0;
    if (fstat(fd, &sb) == 0 && sb.st_size > 0)
        b = hc_anlz_pick(ph_pread, &fd, (uint64_t) sb.st_size, PH_FILE_MAX, n);
    close(fd);
    return b;
}
/* worker: the hot cues of one stick's copy: the .EXT's colored list, else the .DAT's.
 * count, or -1 when neither file reads as an analysis file */
static int ph_from_files(const char *mount, const char *adp, struct hc_cue *cues) {
    char p[700];
    int r = -1;
    size_t L = strlen(adp);
    if (L > 4 && !strcasecmp(adp + L - 4, ".DAT") &&
        pd_file(p, sizeof p, mount, adp, 4, ".EXT") < (int) sizeof p) {
        size_t n = 0;
        uint8_t *b = ph_read(p, &n);
        if (b) {
            r = hc_anlz_hot_cues(b, n, cues);
            free(b);
        }
    }
    if (r < 0 && pd_file(p, sizeof p, mount, adp, 0, "") < (int) sizeof p) {
        size_t n = 0;
        uint8_t *b = ph_read(p, &n);
        if (b) {
            r = hc_anlz_hot_cues(b, n, cues);
            free(b);
        }
    }
    return r;
}
/* worker: (cid, title) on every stick: 1 known (into e's fields), 0 unknown. Two sticks with
 * the same (cid, title) but different hot cues: unknown, never one of them. */
static int ph_resolve(unsigned cid, const char *title, unsigned *n_out, uint16_t *frac, uint32_t *rgb,
                      FILE *lg) {
    DIR *rd = opendir("/media/usb");
    if (!rd)
        return 0;
    struct dirent *de;
    int hits = 0, bad = 0;
    unsigned n0 = 0;
    uint16_t f0[HC_MAX];
    uint32_t c0[HC_MAX];
    while ((de = readdir(rd))) {
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
            continue;
        }
        g_sq.bind_int64(st, 1, cid);
        g_smsq.bind_text(st, 2, title, -1, (void *) -1 /* SQLITE_TRANSIENT */);
        while (g_sq.step(st) == SQLITE_ROW) {
            const unsigned char *ap = g_sq.column_text(st, 0);
            long long secs = g_sq.column_int64(st, 1);
            struct hc_cue cues[HC_MAX];
            int k = ap && ap[0] == '/' && secs > 0 && secs < 24 * 3600
                        ? ph_from_files(mount, (const char *) ap, cues)
                        : -1;
            if (lg)
                fprintf(lg, "  hot cues cid=%u %s%s -> %d\n", cid, mount, ap ? (const char *) ap : "-", k);
            if (k < 0) {
                bad++;
                continue;
            }
            unsigned n = 0;
            uint16_t f[HC_MAX];
            uint32_t c[HC_MAX];
            for (int i = 0; i < k; i++) {
                int x = hc_x(cues[i].ms, (uint32_t) (secs * 1000), 65536);
                if (x < 0)
                    continue;
                f[n] = (uint16_t) x;
                c[n] = cues[i].rgb ? cues[i].rgb : PH_DEFAULT_RGB;
                n++;
            }
            if (!hits) {
                n0 = n;
                memcpy(f0, f, sizeof f);
                memcpy(c0, c, sizeof c);
            } else if (n != n0 || memcmp(f, f0, n * sizeof f[0]) || memcmp(c, c0, n * sizeof c[0])) {
                bad++;
            }
            hits++;
        }
        g_sq.finalize(st);
    }
    closedir(rd);
    if (!hits || bad)
        return 0;
    *n_out = n0;
    memcpy(frac, f0, sizeof f0);
    memcpy(rgb, c0, sizeof c0);
    return 1;
}
/* worker: one queued row, if any. 1 = served one */
static int g_ph_budget = 4000; /* worker log lines */
static int ph_serve_one(FILE *lg) {
    unsigned cid = 0, h = 0;
    long long at = 0;
    int qn = 0, sqn = 0;
    char title[160];
    sm_lock();
    if (g_phq_n) { /* the newest first: the rows on screen now, not the ones a scroll left */
        int k = g_phq_n - 1;
        cid = g_phq[k].cid;
        h = g_phq[k].h;
        at = g_phq[k].at;
        qn = g_phq_n;
        sqn = g_smq_n;
        snprintf(title, sizeof title, "%s", g_phq[k].title);
        g_phq_n--;
    }
    sm_unlock();
    if (!cid)
        return 0;
    unsigned n = 0;
    uint16_t frac[HC_MAX] = {0};
    uint32_t rgb[HC_MAX] = {0};
    FILE *l = g_ph_budget-- > 0 ? lg : 0;
    long long t0 = drag_now_ms();
    int ok = ph_resolve(cid, title, &n, frac, rgb, l);
    if (l)
        fprintf(l, "HC t=%lld cid=%u wait=%lld res=%lld q=%d smq=%d -> %s %u\n", t0 % 100000, cid,
                t0 - at, drag_now_ms() - t0, qn, sqn, ok ? "ok" : "unknown", n);
    sm_lock();
    struct ph_ent *e = ph_ent(cid, h, 1);
    e->st = ok ? 2 : 3;
    e->n = (unsigned char) n;
    memcpy(e->frac, frac, sizeof frac);
    memcpy(e->rgb, rgb, sizeof rgb);
    e->used = drag_now_ms();
    sm_unlock();
    __atomic_store_n(&g_sm_dirty, 1, __ATOMIC_RELEASE); /* sm_tick repaints the rows waiting */
    return 1;
}
/* worker: the library or a stick changed: every answer is checked again when next drawn */
static void ph_world_changed(void) { /* g_sm_mu held */
    for (int i = 0; i < PH_CACHE; i++)
        if (g_phc[i].st == 2 || g_phc[i].st == 3)
            g_phc[i].stale = 1;
}

/* ---- the paint -------------------------------------------------------------------------- */
static void ph_bar(void *g, int x, int y, int w, int h, uint32_t rgb) {
    uint32_t argb = 0xff000000u | rgb;
#ifdef AZMOD_HAVE_DAYMODE
    t_dm_bypass = 1; /* the cue's own color, whatever the theme */
#endif
    ((void (*)(void *, uint32_t *)) EP_23af140)(g, &argb);
    ((void (*)(void *, int, int, int, int)) EP_23af680)(g, x, y, w, h);
#ifdef AZMOD_HAVE_DAYMODE
    t_dm_bypass = 0;
#endif
}
static void (*g_orig_ph_paint)(void *, void *);
static void ph_paint_hook(void *self, void *g) {
    g_orig_ph_paint(self, g);
    if (!ph_shown() || !on_msg_thread())
        return;
    uintptr_t c = (uintptr_t) self, v = 0, mp = 0, wvt = 0;
    if (!own_q(c + 0x28, &v))
        return;
    int w = (int) (v & 0xffffffff), h = (int) (v >> 32);
    if (w < 8 || h < 4 || !own_q(c + 0xd0, &v) || (v & 0xff) || !own_q(c + 0x108, &v) || (v & 0xff))
        return; /* unscaled image or the loading bar: no time axis */
    int y0 = 0, hh = h;
    if (!own_q(c + 0xc8, &mp) || !mp || (mp & 3))
        return;
    uintptr_t widget = mp - PH_OWNER_OFF;
    if ((widget & 7) || !own_q(widget, &wvt) || wvt != TRACKLIST_WIDGET_VT || drag_is_widget(widget))
        return; /* only a track list's previews; a drag remaps rows */
    if (own_q(mp, &v)) {
        y0 = (int) (v >> 32); /* EP147's paint: the image at y = margin, height h - 2 * margin */
        hh = h - 2 * y0;
    }
    if (hh < 2 || y0 < 0)
        return;
    if (!own_q(c + 0xf8, &v))
        return;
    int row = (int) (v & 0xffffffff);
    /* the row's identity, exactly as the stem marks check it */
    uintptr_t ring = 0, slot, tp = 0;
    int nrows;
    char tt[160] = "";
    unsigned kind = 0, id = 0;
    if (row < 0 || !own_q(widget + 0x2a8, &ring) || !ring || !own_q(widget + 0x2c0, &v))
        return;
    nrows = (int) (v & 0xffffffff);
    slot = ring + (uintptr_t) (row % 0x1b) * 0x78;
    if (row >= nrows || !own_q(slot, &v) || (int) (v & 0xffffffff) != row || !own_q(slot + 0x70, &v) ||
        (v & 0xff) || !own_q(slot + 0x28, &tp) || tp < 0x10000 || !own_str(tp, tt, sizeof tt) || !tt[0])
        return;
    if (!sm_lookup(row, tt, &kind, &id) || kind != REC_KIND_TRACK || !id)
        return;
    uint16_t frac[HC_MAX];
    uint32_t rgb[HC_MAX];
    int n = ph_get(id, tt, frac, rgb);
    if (n < 0) { /* not known yet: this row repaints when the answer lands */
        struct sm_w *e = sm_wslot((unsigned long) widget);
        uintptr_t lb = 0, lbvt = 0;
        if (own_q(widget + 0x48, &lb) && lb && !(lb & 7) && own_q(lb, &lbvt) && lbvt) {
            e->lb = lb;
            e->lbvt = lbvt;
            e->ms = drag_now_ms();
            sm_row_add(e->prow, row);
        }
        return;
    }
    int bw = w >= 120 ? 2 : 1;
    for (int i = 0; i < n && i < HC_MAX; i++) {
        int x = hc_x(frac[i], 65536, w);
        if (x < 0)
            continue;
        if (x > w - bw)
            x = w - bw;
        ph_bar(g, x, y0, bw, hh, rgb[i]);
    }
}
static int install_preview_hotcues(void) {
    uintptr_t cur = 0;
    if (!g_sm_on || !img_q(PH_PAINT_SLOT, &cur) || cur != PH_PAINT_ORIG)
        return -1; /* rides the stem marks' row identity; the slot must hold EP147's paint */
    int r = azm_patch_vtable(PH_PAINT_SLOT, (void *) ph_paint_hook, (void **) &g_orig_ph_paint);
    g_ph_on = r == 1;
    return r;
}
