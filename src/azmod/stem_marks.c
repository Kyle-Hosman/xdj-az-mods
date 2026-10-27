/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file stem_marks.c
 * @brief Stem marks: per-row indicators in the browse list showing which tracks have stems.
 *
 * Row identity comes from the library boundary, not the display layer. Rows arrive
 * as 25-row pages through MusicLibraryRequestAgent; the page start row is taken from
 * the agent's request function (hooked via sm_req_hook), and the reply records
 * (kind, id, title) are cached per agent. A painted row is claimed only when its
 * displayed title equals the page record's title; otherwise nothing is drawn.
 *
 * Status (stems present, queued, working) is resolved against the track's own stick
 * under g_sm_mu, with a background worker doing file checks and a repaint-timer
 * tick on the message thread, the only thread that destroys JUCE widgets, repainting
 * rows whose status changed. Marks are shown while the Stems toggle is on; the
 * static marks also need the Stem Marks row (AZF_STEM_MARKS). A MAKE STEMS
 * job's queued/working animation shows regardless of that row. The whole feature is
 * on by default and is disabled by the file settings/azmod-stemsmark.off.
 */
/* ---- Row identity: MusicLibraryRequestAgent replies -------
 * The stems mark needs each visible row's track id, and gui::disp_data is display-only,
 * so identity is taken below it. Rows arrive from the library server in pages of 25
 * through gui::browser_logic::MusicLibraryRequestAgent (vtable 0x2dae640, RTTI), whose
 * seven listener slots (+0x10..+0x40) receive each reply as (this, ticket, payload).
 * Slot +0x20 feeds the stems mark; the logging probe is opt-in and budgeted
 * (azmod-mlreply.log). */
#define MLREPLY_VT EP_2dae640
#define MLREPLY_N 7
static void sm_page(void *self, void *ticket, void *payload);
/* The marks need no flag file to turn on (they follow the Stems toggle and the Stem Marks
 * row); the hooks install everywhere and this file is only an escape hatch. */
#define SM_OFF_FLAG "/home/root/settings/azmod-stemsmark.off"
#define MLREPLY_THUNK(n)                                                        \
    static void *mlreply_hook##n(void *s, void *a, void *b, void *c, void *d) { \
        return mlreply_common(n, s, a, b, c, d);                                \
    }
/* The page's start row is only on the request side (the reply has none):
 * UpdateEventBase::request(event, u8 flag, u32 start, u32 count) 0x1eaacd8, called with
 * start = [event+0xa8], count 25 (0x1e7bb54). After it runs, the agent's pending ticket
 * (+0x28 medium, +0x30 serial) is this request's; the probe finds the agent inside the
 * event by its vtable and logs serial -> start. */
#define MLREQ_FN EP_1eaacd8
static int g_sm_on, g_rs_on;
static int g_sm_on;
static void *(*g_orig_mlreply[MLREPLY_N])(void *, void *, void *, void *, void *);
static _Atomic int g_mlreply_budget[MLREPLY_N];
static int g_sm_on;
static void *mlreply_common(int slot, void *self, void *a1, void *a2, void *a3, void *a4) {
    if (slot == 2 && g_sm_on)
        sm_page(self, a1, a2);
    return g_orig_mlreply[slot](self, a1, a2, a3, a4);
}
MLREPLY_THUNK(0)
MLREPLY_THUNK(1)
MLREPLY_THUNK(2)
MLREPLY_THUNK(3)
MLREPLY_THUNK(4)
MLREPLY_THUNK(5)
MLREPLY_THUNK(6)
static void install_mlreply_probe(void) {
    int probe = AZ_PROBE_ON("/home/root/settings/azmod-mlreply.log");
    if (!probe && access(SM_OFF_FLAG, F_OK) == 0)
        return; /* the stems mark (default), or the probe */
    void *thunks[MLREPLY_N] = {(void *) mlreply_hook0, (void *) mlreply_hook1,
                               (void *) mlreply_hook2, (void *) mlreply_hook3,
                               (void *) mlreply_hook4, (void *) mlreply_hook5,
                               (void *) mlreply_hook6};
    int rc[MLREPLY_N];
    for (int i = 0; i < MLREPLY_N; i++) {
        __atomic_store_n(&g_mlreply_budget[i], !probe ? 0 : i == 2 ? 40
                                                                   : 3,
                         __ATOMIC_RELAXED);
        rc[i] = azm_patch_vtable(MLREPLY_VT + 0x10 + (uintptr_t) i * 8, thunks[i],
                                 (void **) &g_orig_mlreply[i]);
    }
    int rq = 0; /* the request probe is not in release builds */
    FILE *f = azlog_open("/tmp/azmod-mlreply.log", "a");
    if (f) {
        fprintf(f, "install request %lx rc=%d\n", (unsigned long) MLREQ_FN, rq);
        fprintf(
            f,
            "install MusicLibraryRequestAgent vt=%lx slots +0x10..+0x40 rc=%d,%d,%d,%d,%d,%d,%d\n",
            (unsigned long) MLREPLY_VT, rc[0], rc[1], rc[2], rc[3], rc[4], rc[5], rc[6]);
        fclose(f);
    }
}

/* ==== Stems mark: which browse row is which track ============================
 * Identity comes from the library boundary, never the display layer: rows arrive as
 * 25-row pages through MusicLibraryRequestAgent (vt 0x2dae640) reply slot +0x20 =
 * {u32 total; vector<rec>} (rec 0x70, 0x78 on 2.00), rec +0x08 u8 kind (4 track /
 * 8 playlist), +0x0c u32 id, +0x10 std::string title. The page's start row is an
 * argument of the agent's own request function FUN_01e678f0(agent, ..., start=x4,
 * count=w5, ...), which both known paths call (UpdateEventBase::request 0x1eaacd8 and
 * the list-open path 0x1ea82c0); it leaves the pending ticket at agent+0x28 (+0x30
 * serial). paintCell's ring slot gives the row; a row is claimed only when its displayed
 * title equals the page record's title (checked at use), otherwise nothing is drawn.
 * On by default; settings/azmod-stemsmark.off disables it. */
#define SM_AGENT_REQ EP_1e678f0
#define SM_PAINTCELL EP_1bdcf98
#define SM_AGENTS 6
#define SM_ROWS_MAX 60000
struct sm_rec {
    unsigned id, h;
    unsigned char kind;
};
static struct sm_agent {
    uintptr_t agent;
    unsigned serial[8], start[8];
    int nreq; /* recent requests */
    struct sm_rec *rows;
    unsigned total;
    long long used;
} g_sm[SM_AGENTS];
static int g_sm_on; /* installed */
/* shown only while the Stems toggle is on */
static int sm_shown(void) {
    return g_sm_on && az_on(AZF_STEMS);
}
/* Stems > Stem Marks: the static marks. A MAKE STEMS job's queued/working animation
 * shows either way (it is that feature's only progress display), so sm_shown() stays
 * the gate for the painters. */
static int sm_marks(void) {
    return sm_shown() && az_on(AZF_STEM_MARKS);
}
static pthread_mutex_t g_sm_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned sm_hash(const char *t) {
    unsigned h = 2166136261u;
    for (; t && *t; t++) {
        h ^= (unsigned char) *t;
        h *= 16777619u;
    }
    return h ? h : 1;
}
static struct sm_agent *sm_agent_for(uintptr_t a, int create) {
    struct sm_agent *free_ = 0, *old = &g_sm[0];
    for (int i = 0; i < SM_AGENTS; i++) {
        if (g_sm[i].agent == a)
            return &g_sm[i];
        if (!g_sm[i].agent && !free_)
            free_ = &g_sm[i];
        if (g_sm[i].used < old->used)
            old = &g_sm[i];
    }
    if (!create)
        return 0;
    struct sm_agent *e = free_ ? free_ : old;
    free(e->rows);
    memset(e, 0, sizeof *e);
    e->agent = a;
    return e;
}
static uint64_t (*g_sm_orig_req)(void *, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, void *,
                                 void *, uint32_t);
/* Reads only its own arguments, during EP147's call (own_q). */
static uint64_t sm_req_hook(void *agent, uint64_t tk, uint32_t flag, uint32_t x3, uint32_t start,
                            uint32_t count, void *a6, void *a7, uint32_t a8) {
    uint64_t r = g_sm_orig_req(agent, tk, flag, x3, start, count, a6, a7, a8);
    uintptr_t vt = 0, ser = 0;
    if ((int) r == 0 && agent && own_q((uintptr_t) agent, &vt) && vt == MLREPLY_VT + 0x10 &&
        own_q((uintptr_t) agent + 0x30, &ser)) {
        sm_lock();
        struct sm_agent *e = sm_agent_for((uintptr_t) agent, 1);
        int k = e->nreq++ % 8;
        e->serial[k] = (unsigned) ser;
        e->start[k] = start;
        e->used = drag_now_ms();
        sm_unlock();
        FILE *f = azlog_open("/tmp/azmod-stemsmark.log", "a");
        if (f) {
            fprintf(f, "REQ agent=%lx serial=%u start=%u count=%u\n",
                    (unsigned long) (uintptr_t) agent, (unsigned) ser, start, count);
            fclose(f);
        }
    }
    return r;
}
/* reply page (called from mlreply_common's slot +0x20 when the mark is on) */
static void sm_page_pl(uintptr_t agent, unsigned st, const struct sm_rec *recs, long n,
                       unsigned total);
/* The reply record is 0x70 bytes on 1.30 and 0x78 on 2.00: 2.00 appended one 8-byte member
 * (its payload destructor walks the vector with `add x19,x19,#0x78` where 1.30 has #0x70; every
 * member it destroys, +0x10..+0x60, is unmoved, so kind +8 / id +0xc / title +0x10 are too).
 * A wrong size fails the span check below and leaves the row tables empty. */
#define SM_REC ((uintptr_t) ep_off(0x70, 0x78))
/* The track list's title column id: 4 on 1.30, 5 on 2.00 (2.00 inserted a column, so every id
 * from 3 up moved by one): the track model's refreshComponentForCell (1.30 0x1bd8390 / 2.00
 * 0x1d41908) builds the IconHoldableScrollLabel title for `cmp w25,#0x4` on 1.30 and `#0x5` on
 * 2.00, and its last case is #7 vs #8. The two-pane PLAYLIST view's marks depend on it. */
#define SM_TITLE_COL ep_off(4, 5)
/* a reply page just landed: the rows painted before it could not be identified and asked for
 * nothing, so sm_tick repaints what is on screen once */
static _Atomic int g_sm_pagein;
static _Atomic int g_sm_dirty; /* tentative definition; see the status cache below */
/* The agent, ticket and payload are the reply call's arguments: alive for this call (own_q). */
static void sm_page(void *self, void *ticket, void *payload) {
    uintptr_t t1 = 0, cnt = 0, vb = 0, ve = 0;
    if (!own_q((uintptr_t) ticket + 8, &t1) || !own_q((uintptr_t) payload, &cnt) ||
        !own_q((uintptr_t) payload + 8, &vb) || !own_q((uintptr_t) payload + 0x10, &ve))
        return;
    if (!vb || ve < vb || (ve - vb) % SM_REC) {
        FILE *f = azlog_open("/tmp/azmod-stemsmark.log",
                             "a"); /* log which check refused */
        if (f) {
            fprintf(f, "PAGE agent=%lx REFUSED span=%lu rec=%lu\n",
                    (unsigned long) (uintptr_t) self, (unsigned long) (ve - vb),
                    (unsigned long) SM_REC);
            fclose(f);
        }
        return;
    }
    long n = (long) ((ve - vb) / SM_REC);
    unsigned serial = (unsigned) (t1 & 0xffffffffu), total = (unsigned) (cnt & 0xffffffffu);
    sm_lock();
    struct sm_agent *e = sm_agent_for((uintptr_t) self, 0);
    int k = -1;
    for (int i = 0; e && i < 8; i++)
        if (e->serial[i] == serial) {
            k = i;
            break;
        }
    if (k < 0 || total > SM_ROWS_MAX) {
        sm_unlock();
        FILE *f = azlog_open("/tmp/azmod-stemsmark.log", "a");
        if (f) {
            fprintf(f, "PAGE agent=%lx serial=%u UNMATCHED (total %u)\n",
                    (unsigned long) (uintptr_t) self, serial, total);
            fclose(f);
        }
        return;
    }
    unsigned st = e->start[k];
    { /* a playlist page (its records are kind 8): its own table, never the agent's track rows */
        uintptr_t q0 = 0;
        if (n > 0 && own_q(vb + 8, &q0) && (q0 & 0xff) == 8) {
            struct sm_rec *recs = calloc((size_t) n, sizeof *recs);
            for (long i = 0; recs && i < n; i++) {
                uintptr_t r = vb + (uintptr_t) i * SM_REC, q1 = 0, sp = 0;
                char t[160] = "";
                own_q(r + 8, &q1);
                own_q(r + 0x10, &sp);
                if (sp > 0x10000)
                    own_str(sp, t, sizeof t);
                recs[i].kind = (unsigned char) (q1 & 0xff);
                recs[i].id = (unsigned) (q1 >> 32);
                recs[i].h = sm_hash(t);
            }
            if (recs)
                sm_page_pl((uintptr_t) self, st, recs, n, total);
            free(recs);
            __atomic_store_n(&g_sm_pagein, 1, __ATOMIC_RELEASE);
            __atomic_store_n(&g_sm_dirty, 1, __ATOMIC_RELEASE);
            e->used = drag_now_ms();
            sm_unlock();
            FILE *f = azlog_open("/tmp/azmod-stemsmark.log", "a");
            if (f) {
                fprintf(f, "PAGE(playlists) agent=%lx serial=%u start=%u n=%ld total=%u\n",
                        (unsigned long) (uintptr_t) self, serial, st, n, total);
                fclose(f);
            }
            return;
        }
    }
    if (!e->rows || e->total != total) {
        free(e->rows);
        e->rows = calloc(total ? total : 1, sizeof *e->rows);
        e->total = e->rows ? total : 0;
    }
    for (long i = 0; e->rows && i < n && st + i < e->total; i++) {
        uintptr_t r = vb + (uintptr_t) i * SM_REC, q1 = 0, sp = 0;
        char t[160] = "";
        own_q(r + 8, &q1);
        own_q(r + 0x10, &sp);
        if (sp > 0x10000)
            own_str(sp, t, sizeof t);
        e->rows[st + i].kind = (unsigned char) (q1 & 0xff);
        e->rows[st + i].id = (unsigned) (q1 >> 32);
        e->rows[st + i].h = sm_hash(t);
    }
    e->used = drag_now_ms();
    sm_unlock();
    __atomic_store_n(&g_sm_pagein, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_sm_dirty, 1, __ATOMIC_RELEASE);
    FILE *f = azlog_open("/tmp/azmod-stemsmark.log", "a");
    if (f) {
        fprintf(f, "PAGE agent=%lx serial=%u start=%u n=%ld total=%u\n",
                (unsigned long) (uintptr_t) self, serial, st, n, total);
        fclose(f);
    }
}
/* Playlist pages, kept apart: in the two-pane view one agent serves both panes, fetching
 * the playlist page and then the track page, so a shared table would let the tracks
 * overwrite the playlists. A page whose records are playlists (kind 8) lands here, per
 * agent; a track page cannot touch it. */
#define SM_PLAGENTS 4
static struct {
    uintptr_t agent;
    struct sm_rec *rows;
    unsigned total;
    long long used;
} g_smpl[SM_PLAGENTS];
static void sm_page_pl(uintptr_t agent, unsigned st, const struct sm_rec *recs, long n,
                       unsigned total) { /* g_sm_mu held */
    int k = -1, old = 0;
    for (int i = 0; i < SM_PLAGENTS; i++) {
        if (g_smpl[i].agent == agent) {
            k = i;
            break;
        }
        if (g_smpl[i].used < g_smpl[old].used)
            old = i;
    }
    if (k < 0) {
        k = old;
        free(g_smpl[k].rows);
        memset(&g_smpl[k], 0, sizeof g_smpl[k]);
        g_smpl[k].agent = agent;
    }
    if (!g_smpl[k].rows || g_smpl[k].total != total) {
        free(g_smpl[k].rows);
        g_smpl[k].rows = calloc(total ? total : 1, sizeof *g_smpl[k].rows);
        g_smpl[k].total = g_smpl[k].rows ? total : 0;
    }
    for (long i = 0; g_smpl[k].rows && i < n && st + i < g_smpl[k].total; i++)
        g_smpl[k].rows[st + i] = recs[i];
    g_smpl[k].used = drag_now_ms();
}
static int sm_lookup_pl(int row, const char *name, unsigned *id) {
    unsigned h = sm_hash(name);
    int hits = 0;
    sm_lock();
    for (int i = 0; i < SM_PLAGENTS; i++) {
        if (!g_smpl[i].rows || row < 0 || (unsigned) row >= g_smpl[i].total)
            continue;
        struct sm_rec *r = &g_smpl[i].rows[row];
        if (r->h == h && r->kind == 8) {
            if (!hits)
                *id = r->id;
            else if (r->id != *id)
                hits = 99;
            hits++;
        }
    }
    sm_unlock();
    return hits >= 1 && hits < 99;
}
/* the row's identity: kind + id, or 0 when no list's record matches its title */
static int sm_lookup(int row, const char *title, unsigned *kind, unsigned *id) {
    unsigned h = sm_hash(title);
    int hits = 0;
    sm_lock();
    for (int i = 0; i < SM_AGENTS; i++) {
        struct sm_agent *e = &g_sm[i];
        if (!e->rows || row < 0 || (unsigned) row >= e->total)
            continue;
        struct sm_rec *r = &e->rows[row];
        if (r->h == h && r->kind) {
            if (!hits) {
                *kind = r->kind;
                *id = r->id;
            } else if (r->id != *id || r->kind != *kind)
                hits = 99;
            hits++;
        }
    }
    sm_unlock();
    return hits >= 1 && hits < 99; /* two lists disagreeing = unknown */
}
/* ---- the status: does this row's file have stems? ------------------------
 * A track row gives content_id + title (sm_lookup). The stick is not taken from the list:
 * every mounted stick's exportLibrary.db is queried for (content_id, title), and each
 * hit's file is checked by azstem's own loader rule (azstem_has_stems). The mark is drawn
 * only when every candidate agrees it has stems; one stick holding the same id+title as a
 * different song, or a missing file, makes it unknown and nothing is drawn.
 * Our own read-only connection per stick (immutable=1: takes no lock, so it can never make
 * EP147's writer see SQLITE_BUSY), opened on a SCHED_IDLE worker and reopened when the file
 * changes. A torn read while EP147 writes fails decryption or the title check and reads as
 * unknown, never as a wrong answer. */
/* ONELIBRARY_KEY_SM, the static exportLibrary.db key. The public source ships its header as a
 * template that tools/extract-assets.py fills (BUILDING.md). */
#include "onelibkey_sm.h"
#define SM_Q 32
static struct {
    unsigned cid, h, media;
    long long at; /* queued then (ms) */
    char title[160];
} g_smq[SM_Q]; /* media: playlists only */
static int g_smq_n;
static pthread_cond_t g_sm_cv = PTHREAD_COND_INITIALIZER;
static _Atomic int g_sm_dirty; /* a status landed: repaint the list */
static _Atomic int g_sm_world; /* stems/library changed: repaint what is on screen */
static struct sm_ent *sm_ent(unsigned cid, unsigned h, int create) { /* g_sm_mu held */
    unsigned i = (cid * 2654435761u ^ h) % SM_CACHE;
    for (int n = 0; n < 8; n++, i = (i + 1) % SM_CACHE) {
        struct sm_ent *e = &g_smc[i];
        if (e->st && e->cid == cid && e->h == h)
            return e;
        if (!e->st) {
            if (!create)
                return 0;
            e->cid = cid;
            e->h = h;
            e->stale = 0;
            e->queued = 0;
            return e;
        }
    }
    if (!create)
        return 0;
    struct sm_ent *e = &g_smc[(cid * 2654435761u ^ h) % SM_CACHE]; /* full probe run: reuse */
    e->cid = cid;
    e->h = h;
    e->st = 0;
    e->stale = 0;
    e->queued = 0;
    return e;
}
/* paint thread: the status to draw (0 = nothing); queues the row when unknown */
static int sm_status_mb(unsigned cid, const char *title, unsigned media, int *await, int *busy) {
    unsigned h = sm_hash(title) ^ (media * 2654435761u);
    int st; /* a playlist is per stick */
    sm_lock();
    struct sm_ent *e = sm_ent(cid, h, 1);
    if (!e->st)
        e->st = SM_ST_PENDING;
    if ((e->st == SM_ST_PENDING || e->stale) && !e->queued) {
        /* A full queue never drops a track row: opening a folder queues its playlists
         * (1-5 s each to resolve) and a scroll paints dozens of rows. The oldest playlist
         * makes room for a track (the oldest track only when there is none); an evicted
         * entry is re-queued the next time its row paints. */
        int pl = (cid & SM_PLF) != 0, at = -1;
        if (g_smq_n < SM_Q)
            at = g_smq_n++;
        else {
            for (int i = 0; i < SM_Q && at < 0; i++)
                if (g_smq[i].cid & SM_PLF)
                    at = i;
            if (at < 0 && !pl)
                at = 0;
            if (at >= 0) {
                struct sm_ent *o = sm_ent(g_smq[at].cid, g_smq[at].h, 0);
                if (o)
                    o->queued = 0;
                memmove(&g_smq[at], &g_smq[at + 1], (size_t) (SM_Q - 1 - at) * sizeof g_smq[0]);
                at = SM_Q - 1;
            }
        }
        if (at >= 0) {
            g_smq[at].cid = cid;
            g_smq[at].h = h;
            g_smq[at].media = media;
            g_smq[at].at = drag_now_ms();
            snprintf(g_smq[at].title, sizeof g_smq[0].title, "%s", title);
            e->queued = 1;
            pthread_cond_signal(&g_sm_cv);
        }
    }
    st = e->st;
    *await = e->queued || e->st == SM_ST_PENDING || e->stale; /* an answer is coming: repaint then */
    if (busy)
        *busy = e->busy;
    sm_unlock();
    return st;
}
static int sm_status_m(unsigned cid, const char *title, unsigned media, int *await) {
    return sm_status_mb(cid, title, media, await, 0);
}
/* A playlist's track count, if the stems mark has resolved it (-1: not known yet). The
 * MENU's MAKE STEMS reads it; never queues a lookup of its own. */
static int sm_pl_count(unsigned pl, const char *name, unsigned media) {
    unsigned h = sm_hash(name) ^ (media * 2654435761u);
    int n = -1;
    sm_lock();
    struct sm_ent *e = sm_ent(pl | SM_PLF, h, 0);
    if (e && e->st && e->st != SM_ST_PENDING && !e->stale)
        n = e->ntrk;
    sm_unlock();
    return n;
}
static int sm_status(unsigned cid, const char *title, int *await) {
    return sm_status_m(cid, title, 0, await);
}
/* For MENU's CANCEL STEMS: is this track (content_id) in the Remote Stems queue, and does
 * this playlist hold a queued track. A cid alone can repeat across sticks, so this only
 * picks the label; the cancel itself acts on the row's own stick (rs_cancel_reqs). */
static int sm_pending(unsigned cid, const char *title, int *ph);
static int sm_queued_cid(unsigned cid);
static int sm_pl_busy(unsigned pl, const char *name, unsigned media) {
    unsigned h = sm_hash(name) ^ (media * 2654435761u);
    int b = 0;
    sm_lock();
    struct sm_ent *e = sm_ent(pl | SM_PLF, h, 0);
    if (e && e->st && e->st != SM_ST_PENDING)
        b = e->busy;
    sm_unlock();
    return b;
}

#define SM_MOUNTS 4
static struct sm_db {
    char mount[128];
    void *db;
    long long sig;
} g_smdb[SM_MOUNTS];
static struct {
    int (*open_v2)(const char *, void **, int, const char *);
    int (*key)(void *, const void *, int);
    int (*close)(void *);
    int (*bind_text)(void *, int, const char *, int, void *);
    int (*threadsafe)(void);
} g_smsq;
static long long sm_file_sig(const char *p) {
    struct stat st;
    if (stat(p, &st))
        return 0;
    return (long long) st.st_mtime * 1000003LL + (long long) st.st_size;
}
/* Per-path answer cache, worker only.
 * A playlist answer costs one azstem_has_stems per track (~26 ms each on the deck, cold
 * FAT lookups of long paths), and the same tracks sit in many playlists. The answer
 * depends only on the file and the stems folders, so it is cached by path and dropped
 * only when a STEMS folder or the set of sticks changes (sm_stems_sig), not on
 * every library write (EP147 writes play history to the DB on track loads). A missing
 * file (-1) is never cached: a download can add it. */
#define SM_PC 16384
static struct {
    unsigned long long h;
    signed char r;
} g_smpc[SM_PC];
static long g_smpc_hit, g_smpc_miss, g_smpc_us; /* worker only: telemetry for the playlist line */
static int sm_has_stems(const char *abs) {
    unsigned long long h = 0xcbf29ce484222325ULL;
    for (const unsigned char *q = (const unsigned char *) abs; *q; q++) {
        h ^= *q;
        h *= 0x100000001b3ULL;
    }
    if (!h)
        h = 1;
    unsigned i = (unsigned) (h % SM_PC), freei = SM_PC;
    for (int n = 0; n < 8; n++, i = (i + 1) % SM_PC) {
        if (g_smpc[i].h == h) {
            g_smpc_hit++;
            return g_smpc[i].r;
        }
        if (!g_smpc[i].h) {
            freei = i;
            break;
        }
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int r = g_has_stems_fn(abs);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    g_smpc_miss++;
    g_smpc_us += (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000;
    if (r >= 0) {
        if (freei == SM_PC)
            freei = (unsigned) (h % SM_PC); /* full probe run: reuse */
        g_smpc[freei].h = h;
        g_smpc[freei].r = (signed char) r;
    }
    return r;
}
static long long sm_stems_sig(void) {
    DIR *rd = opendir("/media/usb");
    if (!rd)
        return 0;
    struct dirent *de;
    long long s = 0;
    while ((de = readdir(rd))) {
        if (de->d_name[0] == '.')
            continue;
        char p[300];
        s = s * 31 + sm_hash(de->d_name);
        snprintf(p, sizeof p, "/media/usb/%s/STEMS", de->d_name);
        s = s * 31 + sm_file_sig(p);
    }
    closedir(rd);
    return s;
}
/* An eject waits for this worker. The deck's eject script polls lsof up to 20 times, a second
 * apart, until EP147 holds no file on the stick, and only then unmounts. Our connection and every
 * file this worker opens are EP147 fds, so a cached connection (or a worker still resolving
 * that stick's rows: source.json identity reads the audio file) would hold every eject for
 * the full 20 s. EP147 closes its own connection to the stick's library first when it
 * ejects, so this worker touches a stick only while EP147 holds that library open: fds on
 * the path, minus ours. Re-checked at most 4 times a second per stick. */
static int sm_fds_on(const char *path) {
    int n = 0;
    DIR *fd = opendir("/proc/self/fd");
    if (!fd)
        return -1;
    struct dirent *e;
    while ((e = readdir(fd))) {
        char lp[64], tgt[256];
        if (e->d_name[0] == '.')
            continue;
        if (snprintf(lp, sizeof lp, "/proc/self/fd/%s", e->d_name) >= (int) sizeof lp)
            continue; /* never act on a truncated path */
        ssize_t k = readlink(lp, tgt, sizeof tgt - 1);
        if (k <= 0)
            continue;
        tgt[k] = 0;
        if (!strcmp(tgt, path) && libfd_owner(atoi(e->d_name)) != LIBFD_FIELDS)
            n++; /* browse_fields.c's connection is not EP147's */
    }
    closedir(fd);
    return n;
}
static void sm_db_close(struct sm_db *d, const char *why, FILE *lg) {
    if (d->db) {
        libfd_del(libfd_fd_of(d->db));
        g_smsq.close(d->db);
    }
    if (lg && d->db)
        fprintf(lg, "DB closed %s: %s\n", d->mount, why);
    d->db = 0;
    d->sig = 0;
    d->mount[0] = 0;
}
/* Per stick (by mount path): how many of EP147's own connections were open at the last look,
 * and a hands-off deadline. EP147 closes its library connections one by one when it ejects, and
 * the first close is the earliest sign: from then on, for SM_HANDSOFF_MS, this worker keeps away
 * from that stick entirely (no connection, no row work, so no file of ours is open while the
 * eject script polls). A stick EP147 stopped using, or one this worker has not needed for
 * SM_IDLE_MS, also loses our connection. Reopening is not cheap: the cipher derives the key on
 * the first read of a new connection (~0.8 s per stick on the deck), so the connection is kept
 * while the DJ browses; an eject is caught by EP147's own closes above, not by this timeout. */
#define SM_HANDSOFF_MS 10000
#define SM_IDLE_MS 30000
static struct sm_ejw {
    char mount[128];
    int others;                /* EP147's connections at the last look, -1 = not looked yet */
    long long at, handsoff, used;
} g_smej[SM_MOUNTS];
/* 1 when a filesystem is mounted at exactly this path (read only when a watch slot is needed) */
static int sm_is_mounted(const char *mount) {
    FILE *m = fopen("/proc/self/mounts", "re");
    if (!m)
        return 1; /* cannot tell: treat it as present, so its slot is kept */
    char line[512], dir[256];
    int found = 0;
    while (!found && fgets(line, sizeof line, m))
        if (sscanf(line, "%*s %255s", dir) == 1 && !strcmp(dir, mount))
            found = 1;
    fclose(m);
    return found;
}
static struct sm_ejw *sm_watch_of(const char *mount) {
    struct sm_ejw *f = 0;
    for (int i = 0; i < SM_MOUNTS; i++) {
        if (!strcmp(g_smej[i].mount, mount))
            return &g_smej[i];
        if (!f && !g_smej[i].mount[0])
            f = &g_smej[i];
    }
    if (!f) { /* all slots named: reuse one whose stick is gone (mount names change on re-insert) */
        long long now = drag_now_ms();
        for (int i = 0; i < SM_MOUNTS && !f; i++)
            if (now >= g_smej[i].handsoff && !sm_is_mounted(g_smej[i].mount))
                f = &g_smej[i];
    }
    if (!f)
        return NULL; /* every slot is a stick still in the deck: unknown, leave this one alone */
    memset(f, 0, sizeof *f);
    snprintf(f->mount, sizeof f->mount, "%s", mount);
    f->others = -1;
    return f;
}
/* 1 = this worker may use the stick now: EP147 holds its library and no eject has started */
static int sm_stick_usable(const char *mount, int ours) {
    struct sm_ejw *w = sm_watch_of(mount);
    long long now = drag_now_ms();
    if (!w)
        return 0;
    if (now < w->handsoff)
        return 0;
    if (w->others >= 0 && now - w->at < 250)
        return w->others > 0;
    char dbp[200];
    snprintf(dbp, sizeof dbp, "%s/PIONEER/rekordbox/exportLibrary.db", mount);
    int n = sm_fds_on(dbp);
    if (n < 0)
        return 0; /* cannot tell whether EP147 still holds it: unknown, so leave the stick alone */
    int others = n - ours;
    if (w->others > 0 && others < w->others)
        w->handsoff = now + SM_HANDSOFF_MS; /* EP147 began closing it: an eject */
    w->others = others;
    w->at = now;
    return others > 0 && now >= w->handsoff;
}
/* between requests: drop our connection to a stick being ejected, let go of, or idle */
static void sm_db_release_unused(FILE *lg) {
    long long now = drag_now_ms();
    for (int i = 0; i < SM_MOUNTS; i++) {
        struct sm_db *d = &g_smdb[i];
        if (!d->db)
            continue;
        struct sm_ejw *w = sm_watch_of(d->mount);
        if (!w || !sm_stick_usable(d->mount, 1))
            sm_db_close(d, "EP147 is letting go of it (eject)", lg);
        else if (now - w->used > SM_IDLE_MS)
            sm_db_close(d, "idle", lg);
    }
}
/* the stick's own library, read-only and lock-free; 0 when not openable */
static int g_sm_dbpin; /* worker only: a playlist statement is stepping */
static void *sm_db_for(const char *mount, FILE *lg) {
    char dbp[200], uri[240];
    snprintf(dbp, sizeof dbp, "%s/PIONEER/rekordbox/exportLibrary.db", mount);
    long long sig = sm_file_sig(dbp);
    struct sm_db *d = 0, *freed = 0;
    for (int i = 0; i < SM_MOUNTS; i++) {
        if (!strcmp(g_smdb[i].mount, mount)) {
            d = &g_smdb[i];
            break;
        }
        if (!g_smdb[i].mount[0] && !freed)
            freed = &g_smdb[i];
    }
    if (d && d->db && g_sm_dbpin)
        return d->db; /* pinned: a statement is live on it */
    if (!sm_stick_usable(mount, d && d->db ? 1 : 0)) { /* not EP147's now, or an eject began */
        if (d && d->db)
            sm_db_close(d, "EP147 is letting go of it (eject)", lg);
        return 0;
    }
    {
        struct sm_ejw *w = sm_watch_of(mount);
        if (w) w->used = drag_now_ms();
    }
    if (d && d->db && d->sig == sig && sig)
        return d->db;
    if (d && d->db) {
        libfd_del(libfd_fd_of(d->db));
        g_smsq.close(d->db);
        d->db = 0;
    }
    if (!sig)
        return 0;
    if (!d) {
        if (!freed)
            return 0;
        d = freed;
        snprintf(d->mount, sizeof d->mount, "%s", mount);
    }
    snprintf(uri, sizeof uri, "file:%s?immutable=1", dbp);
    void *db = 0;
    if (g_smsq.open_v2(uri, &db, 0x01 | 0x40 /* READONLY | URI */, 0) != 0 || !db) {
        if (db)
            g_smsq.close(db);
        if (lg)
            fprintf(lg, "DB open failed: %s\n", dbp);
        return 0;
    }
    long long t0 = drag_now_ms();
    g_smsq.key(db, ONELIBRARY_KEY_SM, (int) sizeof ONELIBRARY_KEY_SM - 1);
    long long t1 = drag_now_ms();
    { /* the first read: where the cipher's key derivation is paid */
        void *st = 0;
        if (g_sq.prepare(db, "SELECT count(*) FROM sqlite_master", -1, &st, 0) == 0 && st) {
            g_sq.step(st);
            g_sq.finalize(st);
        }
    }
    d->db = db;
    d->sig = sig;
    libfd_add(libfd_of_db(db, dbp), LIBFD_STEMS);
    if (lg)
        fprintf(lg, "DB open %s (key %lld ms, first read %lld ms)\n", dbp, t1 - t0, drag_now_ms() - t1);
    return db;
}
/* worker: 1 stems / 0 none / -1 unknown, over every stick holding (cid, title) */
static int sm_resolve(unsigned cid, const char *title, FILE *lg) {
    DIR *rd = opendir("/media/usb");
    if (!rd)
        return -1;
    struct dirent *de;
    int n = 0, yes = 0, no = 0, unk = 0;
    while ((de = readdir(rd))) {
        if (de->d_name[0] == '.')
            continue;
        char mount[128];
        if (snprintf(mount, sizeof mount, "/media/usb/%s", de->d_name) >= (int) sizeof mount)
            continue; /* a mount name too long for the buffer is skipped, never truncated */
        void *db = sm_db_for(mount, lg);
        if (!db)
            continue;
        void *st = 0;
        if (g_sq.prepare(db, "SELECT path FROM content WHERE content_id = ?1 AND title = ?2", -1,
                         &st, 0) != 0 ||
            !st) {
            unk++;
            continue;
        } /* torn / undecryptable: unknown */
        g_sq.bind_int64(st, 1, cid);
        g_smsq.bind_text(st, 2, title, -1, (void *) -1 /* SQLITE_TRANSIENT */);
        while (g_sq.step(st) == SQLITE_ROW) {
            const unsigned char *pp = g_sq.column_text(st, 0);
            if (!pp || pp[0] != '/') {
                unk++;
                continue;
            }
            char abs[700];
            snprintf(abs, sizeof abs, "%s%s", mount, (const char *) pp);
            int r = sm_has_stems(abs);
            n++;
            if (r > 0)
                yes++;
            else if (r == 0)
                no++;
            else
                unk++;
            if (lg)
                fprintf(lg, "  cid=%u %s -> %d\n", cid, abs, r);
        }
        g_sq.finalize(st);
    }
    closedir(rd);
    if (!n || unk || (yes && no))
        return -1;
    return yes ? 1 : 0;
}
/* worker: a playlist row (or a folder: everything under it), on the one stick being
 * browsed (media -> mount), whose library must hold (playlist_id, name). Each track is
 * checked by azstem's own rule. 3 = every track has stems, 2 = some, 1 = none (or empty),
 * -1 = unknown (no known mount, no matching playlist, a query failed, a file missing). */
static int sm_pending(unsigned cid, const char *title, int *ph);
static void sm_serve_tracks(FILE *lg);
/* busy: 2 = a track in it is being stemmed now, 1 = only queued, 0 = neither */
static int sm_resolve_playlist(unsigned pl, const char *name, unsigned media, FILE *lg, int *busy,
                               int *count) {
    /* One stick: the one being browsed (media -> mount, from its library server) */
    char mount[128];
    if (!media_mount(media, mount, sizeof mount)) {
        if (lg)
            fprintf(lg, "  playlist %u: media %04x has no known mount\n", pl, media);
        return -1;
    }
    int verdict = 0, sticks = 0;
    for (int once = 0; once < 1; once++) {
        void *db = sm_db_for(mount, lg);
        if (!db)
            continue;
        void *st = 0;
        int match = 0;
        if (g_sq.prepare(db, "SELECT name FROM playlist WHERE playlist_id = ?1", -1, &st, 0) == 0 &&
            st) {
            g_sq.bind_int64(st, 1, pl);
            if (g_sq.step(st) == SQLITE_ROW) {
                const unsigned char *n = g_sq.column_text(st, 0);
                match = n && !strcmp((const char *) n, name);
            }
            g_sq.finalize(st);
        }
        if (!match)
            continue;
        st = 0;
        if (g_sq.prepare(db,
                         "WITH RECURSIVE sub(id) AS (SELECT ?1 UNION SELECT p.playlist_id FROM "
                         "playlist p JOIN sub ON p.playlist_id_parent = sub.id) "
                         "SELECT DISTINCT c.path, c.content_id, c.title FROM playlist_content pc "
                         "JOIN sub ON pc.playlist_id = sub.id "
                         "JOIN content c ON c.content_id = pc.content_id",
                         -1, &st, 0) != 0 ||
            !st)
            return -1;
        g_sq.bind_int64(st, 1, pl);
        int n = 0, yes = 0, unk = 0;
        long h0 = g_smpc_hit, m0 = g_smpc_miss, u0 = g_smpc_us, w0 = drag_now_ms();
        g_sm_dbpin = 1;
        while (g_sq.step(st) == SQLITE_ROW) {
            /* a big playlist takes seconds (189 tracks: ~5 s on the deck), so the track
             * rows on screen are answered in between, not after it */
            sm_serve_tracks(lg);
            const unsigned char *pp = g_sq.column_text(st, 0), *tt = g_sq.column_text(st, 2);
            if (tt && __atomic_load_n(&g_rs_on, __ATOMIC_RELAXED) && *busy < 2) { /* the queue's identity: (content_id, title) */
                int pd = sm_pending((unsigned) g_sq.column_int64(st, 1), (const char *) tt, 0);
                if (pd >= 0)
                    *busy = 2;
                else if (pd == -1)
                    *busy = 1;
            }
            if (!pp || pp[0] != '/') {
                unk++;
                continue;
            }
            char abs[700];
            snprintf(abs, sizeof abs, "%s%s", mount, (const char *) pp);
            int r = sm_has_stems(abs);
            n++;
            if (r > 0)
                yes++;
            else if (r < 0)
                unk++;
        }
        g_sm_dbpin = 0;
        g_sq.finalize(st);
        int v = unk ? -1 : !n     ? 1
                       : yes == n ? 3
                       : yes      ? 2
                                  : 1;
        if (count)
            *count = n + unk; /* the MENU's MAKE STEMS shows it (gray "<n> TRACKS") */
        if (lg)
            fprintf(lg,
                    "  playlist %u \"%s\" on %s: %d tracks, %d with stems, %d unknown -> %d  [%lld "
                    "ms; cache %ld hit / %ld miss, %ld ms in has_stems]\n",
                    pl, name, mount, n, yes, unk, v, drag_now_ms() - w0, g_smpc_hit - h0,
                    g_smpc_miss - m0, (g_smpc_us - u0) / 1000);
        if (v < 0)
            return -1;
        sticks++;
        verdict = v;
    }
    return sticks ? verdict : -1;
}
/* what changes a status: a library rewrite, or a stems folder added/removed */
static long long sm_world_sig(void) {
    DIR *rd = opendir("/media/usb");
    if (!rd)
        return 0;
    struct dirent *de;
    long long s = 0;
    while ((de = readdir(rd))) {
        if (de->d_name[0] == '.')
            continue;
        char p[300];
        s = s * 31 + sm_hash(de->d_name);
        /* the name is already in the signature; a path that does not fit is not read */
        if (snprintf(p, sizeof p, "/media/usb/%s/PIONEER/rekordbox/exportLibrary.db", de->d_name) <
            (int) sizeof p)
            s = s * 31 + sm_file_sig(p);
        snprintf(p, sizeof p, "/media/usb/%s/STEMS", de->d_name);
        s = s * 31 + sm_file_sig(p);
    }
    closedir(rd);
    return s;
}
static _Atomic long long g_sm_anim_ms; /* an animated row painted then */
/* g_sm_mu held: take the next request, track rows before playlists, the newest first
 * (the rows on screen now, not the ones a scroll already left). tracks_only: the
 * playlist loop's call, which must not start a second playlist. */
static long long g_sm_pop_at; /* worker only: when the request just taken was queued */
static int sm_pop(unsigned *cid, unsigned *h, unsigned *media, char *title, size_t tsz,
                  int tracks_only) {
    int at = -1;
    for (int i = g_smq_n - 1; i >= 0 && at < 0; i--)
        if (!(g_smq[i].cid & SM_PLF))
            at = i;
    if (at < 0 && !tracks_only)
        at = g_smq_n - 1;
    if (at < 0)
        return 0;
    *cid = g_smq[at].cid;
    *h = g_smq[at].h;
    *media = g_smq[at].media;
    g_sm_pop_at = g_smq[at].at;
    snprintf(title, tsz, "%s", g_smq[at].title);
    memmove(&g_smq[at], &g_smq[at + 1], (size_t) (g_smq_n - 1 - at) * sizeof g_smq[0]);
    g_smq_n--;
    return 1;
}
static int g_sm_budget = 2000; /* worker log lines */
static void sm_serve(unsigned cid, unsigned h, unsigned media, const char *title, FILE *lg) {
    FILE *l = g_sm_budget-- > 0 ? lg : 0;
    long long t0 = drag_now_ms(), wait = t0 - g_sm_pop_at;
    int pl = (cid & SM_PLF) != 0;
    int busy = 0, count = -1;
    int r = pl ? sm_resolve_playlist(cid & ~SM_PLF, title, media, l, &busy, &count)
               : sm_resolve(cid, title, l);
    sm_lock();
    struct sm_ent *e = sm_ent(cid, h, 1);
    if (pl)
        e->st = r == 3 ? SM_ST_STEMS : r == 2 ? SM_ST_PART
                                   : r == 1   ? SM_ST_NONE
                                              : SM_ST_UNKNOWN;
    else
        e->st = r > 0 ? SM_ST_STEMS : r == 0 ? SM_ST_NONE
                                             : SM_ST_UNKNOWN;
    e->stale = 0;
    e->queued = 0;
    if (pl) {
        e->busy = (unsigned char) busy;
        e->ntrk = count;
    }
    sm_unlock();
    if (l)
        fprintf(l, "RES t=%lld cid=%u wait=%lld res=%lld \"%s\" -> %s\n", drag_now_ms() % 100000, cid,
                wait, drag_now_ms() - t0, title,
                pl ? (r == 3   ? "ALL"
                      : r == 2 ? "SOME"
                      : r == 1 ? "none"
                               : "unknown")
                   : (r > 0    ? "STEMS"
                      : r == 0 ? "none"
                               : "unknown"));
    /* every answer repaints the rows still waiting: an unchanged re-check is the last
     * event a row that was evicted from a full queue may get */
    __atomic_store_n(&g_sm_dirty, 1, __ATOMIC_RELEASE);
}
/* Preview Hot Cues (preview_hotcues.c, included at the end) ride this worker */
static int ph_queued(void);
static int ph_serve_one(FILE *lg);
static void ph_world_changed(void);
/* The rows on screen, between the steps of a playlist scan: track rows' stems, then their hot
 * cues. Both belong to rows being painted now; a playlist row's scan takes seconds. */
static void sm_serve_tracks(FILE *lg) {
    for (;;) {
        unsigned cid = 0, h = 0, media = 0;
        char title[160];
        sm_lock();
        int got = sm_pop(&cid, &h, &media, title, sizeof title, 1);
        sm_unlock();
        if (got) {
            sm_serve(cid, h, media, title, lg);
            continue;
        }
        if (!ph_serve_one(lg))
            return;
    }
}
/* worker: one request, the rows on screen first: a track row's stems, then a track row's hot
 * cues, and only then a playlist row (whose scan takes seconds and serves the other two
 * between its steps, sm_serve_tracks). 1 = served one */
static int sm_work_one(FILE *lg) {
    unsigned cid = 0, h = 0, media = 0;
    char title[160] = "";
    sm_lock();
    int got = g_smq_n && sm_pop(&cid, &h, &media, title, sizeof title, 1);
    sm_unlock();
    if (!got) {
        if (ph_serve_one(lg))
            return 1;
        sm_lock();
        got = g_smq_n && sm_pop(&cid, &h, &media, title, sizeof title, 0);
        sm_unlock();
    }
    if (!got)
        return 0;
    sm_serve(cid, h, media, title, lg);
    return 1;
}
static void *sm_worker(void *arg) {
    (void) arg;
    struct sched_param sp = {0};
    if (pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp) == 0)
        g_sm_idle_thread = 1; /* sm_lock() leaves the idle class while it holds g_sm_mu */
    FILE *lg = azlog_open("/tmp/azmod-stemsmark.log", "a");
    if (lg)
        setvbuf(lg, NULL, _IOLBF, 0);
    long long sig = sm_world_sig(), sig_at = drag_now_ms(), ssig = sm_stems_sig();
    for (;;) {
        sm_lock();
        if (!g_smq_n && !ph_queued()) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&g_sm_cv, &g_sm_mu, &ts);
        }
        sm_unlock();
        {
            static long long pend_at;
            if (drag_now_ms() - pend_at >= 1000) {
                pend_at = drag_now_ms();
                sm_pend_scan();
            }
        }
        if (drag_now_ms() - sig_at > 3000) {
            long long s = sm_world_sig();
            sig_at = drag_now_ms();
            long long ss = sm_stems_sig();
            if (ss != ssig) {
                ssig = ss;
                memset(g_smpc, 0, sizeof g_smpc);
            }               /* stems folders changed */
            if (s != sig) { /* keep drawing the old answer until re-checked */
                sig = s;
                sm_lock();
                for (int i = 0; i < SM_CACHE; i++)
                    if (g_smc[i].st)
                        g_smc[i].stale = 1;
                ph_world_changed();
                sm_unlock();
                __atomic_store_n(&g_sm_world, 1, __ATOMIC_RELEASE);
                __atomic_store_n(&g_sm_dirty, 1, __ATOMIC_RELEASE);
                if (lg && g_sm_budget-- > 0)
                    fprintf(lg, "WORLD changed: statuses marked stale\n");
            }
        }
        sm_db_release_unused(lg);
        sm_work_one(lg);
    }
    return 0;
}
/* Liveness of the browse list widgets the marks remember between paints. juce::Component is
 * a virtual base of gui::WidgetBase, so inside these widgets it sits at the virtual-base
 * offset their vtable carries at -0x18 (TrackListWidget 0x2e0, HierarchyListWidget 0x268,
 * the same on 1.30 and 2.00); that is the pointer juce::Component's destructor receives, so
 * it is the comp_off of their wref slots. A widget is used again only while its slot still
 * holds it, i.e. its destructor has not run (ep_mem.h), and only on the message thread. */
static int sm_vbase_comp_off(uintptr_t vt) {
    uintptr_t v = 0;
    if (!vt || !img_q(vt - 0x18, &v) || (intptr_t) v <= 0 || v >= 0x4000 || (v & 7))
        return -1;
    return (int) v;
}
static _Atomic uintptr_t g_smw_live[SM_NW]; /* g_smw[i].w while that list is alive */
#define SM_TL 64
static _Atomic uintptr_t g_tl_live[SM_TL]; /* g_tl[i].w while that list is alive */
#define HLW_N 8
static _Atomic uintptr_t g_hlw_live[HLW_N]; /* hierarchy widgets seen alive (hlw_live_note) */
static int g_sm_live_ok;                      /* the three ranges above are tracked */
static void sm_live_init(void) {
    int tl = sm_vbase_comp_off(TRACKLIST_WIDGET_VT), hw = sm_vbase_comp_off(HIER_WIDGET_VT);
    g_sm_live_ok = tl >= 0 && hw >= 0 && wref_track(g_smw_live, SM_NW, tl) &&
                   wref_track(g_tl_live, SM_TL, tl) && wref_track(g_hlw_live, HLW_N, hw);
}
/* A hierarchy widget EP147 is calling us about right now (during its own call, message
 * thread) is alive: remember it until its destructor runs. */
__attribute__((unused)) static void hlw_live_note(uintptr_t w) {
    if (!g_sm_live_ok || !w || !on_msg_thread())
        return;
    int freei = -1;
    for (int i = 0; i < HLW_N; i++) {
        uintptr_t v = wref_get(&g_hlw_live[i]);
        if (v == w)
            return;
        if (!v && freei < 0)
            freei = i;
    }
    static unsigned rr;
    wref_set(&g_hlw_live[freei >= 0 ? freei : (int) (rr++ % HLW_N)], w);
}
static int hlw_is_live(uintptr_t w) {
    if (!w || !on_msg_thread())
        return 0;
    for (int i = 0; i < HLW_N; i++)
        if (wref_get(&g_hlw_live[i]) == w)
            return 1;
    return 0;
}
static struct sm_w *sm_wslot(unsigned long w) {
    struct sm_w *old = &g_smw[0];
    for (int i = 0; i < SM_NW; i++) {
        if (g_smw[i].w == w)
            return &g_smw[i];
        if (g_smw[i].ms < old->ms)
            old = &g_smw[i];
    }
    memset(old, 0, sizeof *old);
    old->w = w;
    wref_set(&g_smw_live[old - g_smw], (uintptr_t) w); /* the caller holds it alive: a paint */
    old->lastcol = -1;
    old->pr = -1;
    old->pc = -1;
    return old;
}
static void sm_row_add(int *set, int row) { /* set of rows (row + 1), no duplicates */
    int freei = -1;
    for (int i = 0; i < SM_PROWS; i++) {
        if (set[i] == row + 1)
            return;
        if (!set[i] && freei < 0)
            freei = i;
    }
    if (freei >= 0)
        set[freei] = row + 1;
}
static void sm_tick(void) {
    if (!g_sm_on)
        return;
    { /* the Stems toggle flipped: repaint what is on screen so the marks appear / go */
        static int last = -1;
        int now = sm_shown() * 2 + sm_marks() +
                  smk_variant() * 4; /* Stems, Stem Marks or the mark style / theme ink flipped */
        if (now != last) {
            if (last >= 0) {
                __atomic_store_n(&g_sm_world, 1, __ATOMIC_RELEASE);
                __atomic_store_n(&g_sm_dirty, 1, __ATOMIC_RELEASE);
            }
            last = now;
        }
    }
    /* the stemming animation: ~12 fps, only while an animated row painted lately */
    static long long next_anim;
    long long tnow = drag_now_ms();
    int anim = tnow - __atomic_load_n(&g_sm_anim_ms, __ATOMIC_RELAXED) < 1500 && tnow >= next_anim;
    int dirty = __atomic_load_n(&g_sm_dirty, __ATOMIC_ACQUIRE);
    if (!dirty && !anim)
        return;
    if (anim)
        next_anim = tnow + 80;
    if (dirty)
        __atomic_store_n(&g_sm_dirty, 0, __ATOMIC_RELEASE);
    int world = dirty ? __atomic_exchange_n(&g_sm_world, 0, __ATOMIC_ACQ_REL) : 0;
    int pagein = dirty ? __atomic_exchange_n(&g_sm_pagein, 0, __ATOMIC_ACQ_REL) : 0;
    if (dirty)
        sm_repaint_labels(world ||
                          pagein); /* playlist rows (a page landing: every registered row once) */
    if (anim)
        sm_animate_labels();
    void (*repaint_row)(void *, int) = (void (*)(void *, int)) LISTBOX_REPAINTROW;
    static int logs = 200;
    for (int k = 0; k < SM_NW; k++) {
        struct sm_w *e = &g_smw[k];
        if (!e->w)
            continue;
        /* Liveness: the list's wref slot still holds it, so its destructor has not run (this
         * is the message thread, where widgets are destroyed). Then its identity: the vtable,
         * and the ListBox at [w+0x48] (a member of the live list) with the vptr it had when
         * the list painted. Failing either: forget it. */
        uintptr_t vt = 0, lb = 0, lbvt = 0, v = 0;
        if (!on_msg_thread() || wref_get(&g_smw_live[k]) != (uintptr_t) e->w || (e->w & 7) ||
            !own_q(e->w, &vt) || vt != TRACKLIST_WIDGET_VT ||
            !own_q(e->w + 0x48, &lb) || lb != e->lb || !lb || (lb & 7) || !own_q(lb, &lbvt) ||
            lbvt != e->lbvt) {
            FILE *f = logs-- > 0 ? azlog_open("/tmp/azmod-stemsmark.log", "a") : 0;
            if (f) {
                fprintf(f, "TICK list %lx gone (vt=%lx lb=%lx)\n", e->w, (unsigned long) vt,
                        (unsigned long) lb);
                fclose(f);
            }
            memset(e, 0, sizeof *e);
            wref_set(&g_smw_live[k], 0);
            continue;
        }
        int nr = own_q(e->w + 0x2c0, &v) ? (int) (v & 0xffffffff) : 0;
        if (pagein)
            ((void (*)(void *)) RC_REPAINT)(
                (void *) lb); /* live, vtable-checked above: its rows re-paint once */
        for (int i = 0; anim && i < SM_PROWS; i++) {
            int r = e->arow[i] - 1;
            if (r >= 0 && r < nr)
                repaint_row((void *) lb, r);
        }
        if (!dirty)
            continue;
        int n = 0;
        for (int i = 0; i < SM_PROWS; i++) {
            int r = e->prow[i] - 1;
            e->prow[i] = 0;
            if (r >= 0 && r < nr) {
                repaint_row((void *) lb, r);
                n++;
            }
            /* a world change: the rows on screen re-paint, find their entry stale,
             * re-queue, and come back through prow with the new answer */
            r = world ? e->vrow[i] - 1 : -1;
            if (r >= 0 && r < nr) {
                repaint_row((void *) lb, r);
                n++;
            }
        }
        FILE *f = n && logs-- > 0 ? azlog_open("/tmp/azmod-stemsmark.log", "a") : 0;
        if (f) {
            fprintf(f, "TICK list %lx: repaint %d rows\n", e->w, n);
            fclose(f);
        }
    }
}
/* Where: the right edge of the rightmost column. The columns are the DJ's choice, so
 * the column is learned, not assumed: juce's row paints its cells left to right, so the
 * last cell of a full row paint is the rightmost one.
 * A clipped repaint paints fewer cells, so only the widest sequence seen counts.
 * Per list: two lists on screen have different columns. Message thread only. */
static void sm_learn_lastcol(struct sm_w *e, int row, int col) {
    if (row != e->pr) {
        if (e->pc >= 0 && e->n >= 2 && e->n >= e->best) {
            e->best = e->n;
            e->lastcol = e->pc;
        } /* >= 2: a lone-cell repaint (artwork arriving) is not a row */
        e->pr = row;
        e->n = 0;
    }
    e->n++;
    e->pc = col;
}
/* Stems > Stem Marks > Mark style: 0 = the color mark, 1 = Mono
 * white (dark themes, Themes off), 2 = Mono black (a light theme: Day, the same test as
 * Bright Grid's stand-down). Every painter goes through smk_draw, so every mark follows it:
 * track rows, the title-column rows, playlist rows and the MAKE STEMS animation. */
static int smk_variant(void) {
    if (!az_on(AZF_STEM_MARK_STYLE))
        return 0;
    return (__atomic_load_n(&g_dm_on, __ATOMIC_RELAXED) &&
            __atomic_load_n(&G_DM_FLIP, __ATOMIC_RELAXED))
               ? 2
               : 1;
}
static uintptr_t g_smk_img[3][SMK_I_N];
/* The animation frame for a job in phase ph at time t, five
 * SMK_STEP_MS frames a cycle, all starting from every tile dim:
 *   SM_PH_WT  every tile dim, still                   (queued, or sent and waiting its turn)
 *   SM_PH_UP  the same: dim and still                 (uploading; no upload
 *             animation, it reads smoother to stay dim until the stems are being made)
 *   SM_PH_ST  1, 2, 3, 4 tiles lit                     (being stemmed)
 *   SM_PH_DN  one tile lit at a time, top to bottom    (the stems coming back)
 * then the full mark once the stems are in place. */
static int smk_anim(int ph, long long t) {
    if (ph == SM_PH_WT || ph == SM_PH_UP)
        return SMK_I_FILL0; /* waiting or uploading: dim and still */
    int step = (int) ((t / SMK_STEP_MS) % 5);
    if (step == 0)
        return SMK_I_FILL0;
    if (ph == SM_PH_DN)
        return SMK_I_ONE0 + (step - 1);
    return SMK_I_FILL0 + (step <= SMK_FILL_STEPS ? step : SMK_FILL_STEPS);
}
static void smk_draw(void *g, int which, int x, int y) {
    if (which < 0 || which >= SMK_I_N)
        return;
    int v = smk_variant();
    if (!g_smk_img[v][which]) {
        const unsigned char *d = v == 1                ? SMK_MW[which]
                                 : v == 2              ? SMK_MK[which]
                                 : which == SMK_I_FULL ? SMK_FULL
                                 : which == SMK_I_PART ? SMK_PARTIAL
                                 : which >= SMK_I_ONE0 ? SMK_ONE[which - SMK_I_ONE0]
                                                       : SMK_FILL[which - SMK_I_FILL0];
        int n = v == 1                ? (int) SMK_MW_LEN[which]
                : v == 2              ? (int) SMK_MK_LEN[which]
                : which == SMK_I_FULL ? (int) sizeof SMK_FULL
                : which == SMK_I_PART ? (int) sizeof SMK_PARTIAL
                : which >= SMK_I_ONE0 ? (int) SMK_ONE_LEN[which - SMK_I_ONE0]
                                      : (int) SMK_FILL_LEN[which - SMK_I_FILL0];
        uintptr_t img = 0, q = 0;
        mp_image_from_memory(&img, d, n);
        if (!img || !own_q(img + 0x10, &q) || (int) (uint32_t) q != SMK_W ||
            (int) (uint32_t) (q >> 32) != SMK_H)
            return;
        g_smk_img[v][which] = img;
    }
    uintptr_t holder = g_smk_img[v][which]; /* a juce::Image is its pixel-data pointer */
#ifdef AZMOD_HAVE_DAYMODE
    t_dm_bypass = v != 0; /* Mono is already in the theme's ink (day_mode.c) */
#endif
    ((void (*)(void *, uintptr_t *, int, int, int, int, int, int, int, int, int)) EP_23de290)(
        g, &holder, x, y, SMK_W, SMK_H, 0, 0, SMK_W, SMK_H, 0);
#ifdef AZMOD_HAVE_DAYMODE
    t_dm_bypass = 0;
#endif
}
/* The rightmost visible column, from the list's own header (gui::browse::TrackListHeader at
 * widget+0xd0): EP147's column ids run left to right, so it is the highest visible id. 0 when
 * the header cannot be read. Browse Columns changes the columns while the list is up, and
 * a row painted with the old layout would teach the learned column a wrong answer. */
static int sm_hdr_lastcol(uintptr_t widget) {
    uintptr_t hdr = 0, vt = 0;
    if (!own_q(widget + 0xd0, &hdr) || !hdr || (hdr & 7) || !own_q(hdr, &vt) || vt != EP_2b7d8d0)
        return 0;
    for (int id = ep_off(7, 8); id >= 1; id--)
        if (((int (*)(long, int)) EP_24597e0)((long) hdr, id) & 0xff)
            return id;
    return 0;
}
/* The room the mark takes at the right of the column it is drawn on: paintcell_hook has EP147
 * paint that column's cells this much narrower, so a right-aligned value (BPM) ends before the
 * mark instead of under it. 0 for any other cell. (message thread) */
#define SM_RESERVE_GAP 4
static int sm_reserve_w(void *model, int col, int w, int h) {
    uintptr_t self = (uintptr_t) model - 0x30, vt = 0;
    if (!g_sm_on || !model || col == SM_TITLE_COL || w < 70 || h < 30 || !sm_shown() || !own_q(self, &vt) ||
        vt != TRACKLIST_WIDGET_VT)
        return 0;
    int hc = sm_hdr_lastcol(self);
    return hc > 0 && col == hc ? SMK_INSET_TRACK + SMK_W + SM_RESERVE_GAP : 0;
}
static void sm_after_paint(void *self, void *g, int row, int col, int w, int h) {
    if (row < 0 || !sm_shown())
        return;
    uintptr_t vt = 0, ring = 0;
    int nrows = 0;
    if (!own_q((uintptr_t) self, &vt) || vt != TRACKLIST_WIDGET_VT)
        return;
    struct sm_w *e = sm_wslot((unsigned long) (uintptr_t) self);
    e->ms = drag_now_ms();
    sm_learn_lastcol(e, row, col);
    if (w >= 70 && h >= 30) {
        int hc = sm_hdr_lastcol((uintptr_t) self);
        if (hc > 0)
            e->lastcol = hc;
    }
    /* >= 70 px: juce calls paintCell only for columns without a component, so in a list
     * whose rightmost column is the title (a component), as in the two-pane PLAYLIST
     * view's track pane, the "rightmost painted cell" is the 52 px artwork. Key is 90 px.
     * Those rows are marked through the title component instead (sm_title_prep). */
    if (col != e->lastcol || col == SM_TITLE_COL || w < 70 || h < 30)
        return; /* the title: a component, painted only transiently */
    (void) ring;
    (void) nrows;
    sm_mark_row(e, (uintptr_t) self, g, row, w - SMK_INSET_TRACK - SMK_W, (h - SMK_H) / 2);
}
/* the mark for track-list row `row` of widget `self`, drawn at (x0, y0) of `g`: the
 * row's identity (EP147's row ring + the list's reply page, title-checked), its
 * status (queued when unknown, the row repainted when the answer lands), and the
 * stemming animation. Message thread (a paint). Shared by the rightmost painted
 * cell and the title component (sm_title_paint). */
static int sm_mark_row(struct sm_w *e, uintptr_t self, void *g, int row, int x0, int y0) {
    uintptr_t ring = 0;
    int nrows = 0;
    if (!own_q((uintptr_t) self + 0x2a8, &ring) || !ring)
        return 0;
    {
        uintptr_t v = 0;
        if (!own_q((uintptr_t) self + 0x2c0, &v))
            return 0;
        nrows = (int) (v & 0xffffffff);
    }
    int st = 0;
    char tt[160] = "";
    unsigned kind = 0, id = 0;
    uintptr_t slot = ring + (uintptr_t) (row % 0x1b) * 0x78, v = 0, tp = 0;
    if (row >= nrows)
        return 0;
    if (!own_q(slot, &v) || (int) (v & 0xffffffff) != row)
        return 0; /* EP147's own guards */
    if (!own_q(slot + 0x70, &v) || (v & 0xff))
        return 0;
    if (!own_q(slot + 0x28, &tp) || tp < 0x10000 || !own_str(tp, tt, sizeof tt) || !tt[0])
        return 0;
    if (!sm_lookup(row, tt, &kind, &id) || kind != REC_KIND_TRACK || !id)
        return 0; /* title check */
    {
        uintptr_t lb = 0, lbvt = 0; /* self is alive: EP147 is painting it */
        if (own_q((uintptr_t) self + 0x48, &lb) && lb && !(lb & 7) && own_q(lb, &lbvt) && lbvt) {
            e->lb = lb;
            e->lbvt = lbvt;
        }
        int await = 0;
        e->vrow[row % SM_PROWS] = row + 1;
        if (sm_marks()) { /* marks off: no lookup at all */
            st = sm_status(id, tt, &await);
            if (await)
                sm_row_add(e->prow, row); /* repaint this row when its answer lands */
        }
    }
    /* In the Remote Stems queue: animated from the moment it is queued, one animation per
     * phase (smk_anim). sm_tick repaints these rows (~12 fps). */
    int ph = SM_PH_WT;
    int pend = (__atomic_load_n(&g_rs_on, __ATOMIC_RELAXED) && id) ? sm_pending(id, tt, &ph) : -2;
    if (pend > -2) {
        long long t = drag_now_ms();
        if (ph != SM_PH_WT &&
            ph != SM_PH_UP) { /* a still (waiting/uploading) row needs no ~12 fps repaint: */
            e->arow[row % SM_PROWS] = row + 1; /* its phase changing repaints it (g_sm_dirty) */
            __atomic_store_n(&g_sm_anim_ms, t, __ATOMIC_RELAXED);
        } else if (e->arow[row % SM_PROWS] == row + 1)
            e->arow[row % SM_PROWS] = 0;
        if (g)
            smk_draw(g, smk_anim(ph, t), x0, y0);
        return 1;
    }
    if (e->arow[row % SM_PROWS] == row + 1)
        e->arow[row % SM_PROWS] = 0; /* done animating */
    if (st != SM_ST_STEMS)
        return 0;
    if (g)
        smk_draw(g, SMK_I_FULL, x0, y0);
    return 1;
}
/* ---- Track rows whose rightmost column is the title.
 * The two-pane PLAYLIST view's track pane is art + title, and the title is a component
 * (refreshComponentForCell builds column SM_TITLE_COL), so paintCell's rightmost cell
 * there is the 52 px art and sm_after_paint does not draw. The title is a
 * gui::IconHoldableScrollLabel, the same class as the playlist rows' labels, whose paint
 * hlabel_paint_hook already wraps; so each title component is registered here and the
 * mark is drawn after its paint, at its right edge, when it reaches the row's right edge. */
static struct {
    _Atomic unsigned long comp, w;
    _Atomic int row;
} g_tl[SM_TL]; /* message thread writes */
static void *(*g_orig_sm_refresh)(void *, int, int, int, void *);
static void *sm_refresh_hook(void *model, int row, int col, int sel, void *existing) {
    void *c = g_orig_sm_refresh(model, row, col, sel, existing);
    /* model is the list's TableListBoxModel base (+0x30), EP147's own call: own_q */
    uintptr_t w = (uintptr_t) model - 0x30, vt = 0;
    if (col != SM_TITLE_COL || !c || !own_q(w, &vt) || vt != TRACKLIST_WIDGET_VT)
        return c;
    int free_i = -1, hit = -1;
    for (int i = 0; i < SM_TL; i++) {
        unsigned long cc = __atomic_load_n(&g_tl[i].comp, __ATOMIC_RELAXED);
        if (cc == (unsigned long) (uintptr_t) c) {
            hit = i;
            break;
        }
        if (!cc && free_i < 0)
            free_i = i;
    }
    static unsigned rr;
    int i = hit >= 0 ? hit : free_i >= 0 ? free_i
                                         : (int) (rr++ % SM_TL);
    __atomic_store_n(&g_tl[i].comp, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_tl[i].w, (unsigned long) w, __ATOMIC_RELAXED);
    wref_set(&g_tl_live[i], w); /* until the list's destructor runs */
    __atomic_store_n(&g_tl[i].row, drag_is_widget(w) ? -1 : row,
                     __ATOMIC_RELAXED); /* a drag remaps rows: no mark */
    __atomic_store_n(&g_tl[i].comp, (unsigned long) (uintptr_t) c, __ATOMIC_RELEASE);
    return c;
}
static int sm_title_prep(void *self, struct sm_tprep *t) {
    uintptr_t w = 0;
    int row = -1;
    for (int i = 0; i < SM_TL; i++)
        if (__atomic_load_n(&g_tl[i].comp, __ATOMIC_ACQUIRE) == (unsigned long) (uintptr_t) self) {
            w = __atomic_load_n(&g_tl[i].w, __ATOMIC_RELAXED);
            row = __atomic_load_n(&g_tl[i].row, __ATOMIC_RELAXED);
            if (wref_get(&g_tl_live[i]) != w || !on_msg_thread())
                return 0; /* the list was destroyed since it built this title: unknown */
            break;
        }
    if (!w)
        return 0;
    uintptr_t vt = 0, lb = 0, p = (uintptr_t) self, par = 0, bb = 0, pb = 0, xy = 0;
    if (row < 0 || (w & 7) || !own_q(w, &vt) || vt != TRACKLIST_WIDGET_VT ||
        !own_q(w + 0x48, &lb) || !lb)
        return 1;
    int own = 0;
    for (int d = 0; d < 6 && p; d++) {
        if (!own_q(p + 0x18, &p))
            break;
        if (p == lb) {
            own = 1;
            break;
        }
    }
    if (!own)
        return 1;
    /* only where the title is the row's right end: in the TRACK view the Key column is,
     * and sm_after_paint draws there */
    if (!own_q((uintptr_t) self + 0x18, &par) || !par || !own_q((uintptr_t) self + 0x20, &xy) ||
        !own_q((uintptr_t) self + 0x28, &bb) || !own_q(par + 0x28, &pb))
        return 1;
    int lx = (int) (xy & 0xffffffff), lw = (int) (bb & 0xffffffff), lh = (int) (bb >> 32),
        pw = (int) (pb & 0xffffffff);
    if (lw < 120 || lh < 30 || lw > 4000 || lh > 400 || pw <= 0 || lx + lw < pw - 24)
        return 1;
    struct sm_w *e = sm_wslot((unsigned long) w);
    e->ms = drag_now_ms();
    t->e = e;
    t->w = w;
    t->row = row;
    t->lw = lw;
    t->lh = lh;
    t->x0 = lw - SMK_INSET_TRACK - SMK_W;
    t->y0 = (lh - SMK_H) / 2;
    return sm_mark_row(e, w, 0, row, t->x0, t->y0) ? 2 : 1;
}
/* juce::Graphics (EP147 0x23aexxx, read off the binary: the saveStatePending
 * byte at +0x10, context slots 0x70 saveState / 0x78 restoreState / 0x40
 * excludeClipRectangle, matching JUCE 6's LowLevelGraphicsContext; fillRect at 0xa8
 * is the one azmod already calls) */
#define JUCE_G_SAVESTATE EP_23aeef0
#define JUCE_G_RESTORESTATE EP_23aef40
#define JUCE_G_EXCLUDECLIP EP_23aee40 /* excludeClipRegion(const Rectangle<int>&) */
/* the title's text: gui::ScrollLabel (vt 0x2bcdec0, RTTI-checked), paint slot 26 */
#define SLTEXT_VT EP_2bcdec0
#define SLTEXT_PAINT_SLOT (SLTEXT_VT + 26 * 8)
#define SLTEXT_PAINT_ORIG EP_1c348c8
static uintptr_t g_tclip_label;
static int g_tclip_x; /* message thread: set by the label's paint */
static void (*g_orig_sltext_paint)(void *, void *);
static void sltext_paint_hook(void *self, void *g) {
    uintptr_t lab = g_tclip_label, par = 0, kid = 0, xy = 0, bb = 0;
    /* the label's ScrollLabel children, matched by parent, this frame only (the live one is
     * not [label+0xb8]: on the deck the paints come from a sibling) */
    (void) kid;
    if (!lab || !own_q((uintptr_t) self + 0x18, &par) || par != lab ||
        !own_q((uintptr_t) self + 0x20, &xy) || !own_q((uintptr_t) self + 0x28, &bb)) {
        g_orig_sltext_paint(self, g);
        return;
    }
    int cx = (int) (xy & 0xffffffff), cw = (int) (bb & 0xffffffff), ch = (int) (bb >> 32);
    int x = g_tclip_x - cx; /* the mark's left edge, in the child's coordinates */
    if (x < 0 || x >= cw || ch <= 0) {
        g_orig_sltext_paint(self, g);
        return;
    }
    int r[4] = {x, 0, cw - x, ch};
    ((void (*)(void *)) JUCE_G_SAVESTATE)(g);
    ((void (*)(void *, const int *)) JUCE_G_EXCLUDECLIP)(g, r);
    g_orig_sltext_paint(self, g);
    ((void (*)(void *)) JUCE_G_RESTORESTATE)(g);
}
/* sm_repaint_labels, message thread (sm_tick): repaint playlist labels whose answer
 * landed, or all registered ones on a world change. A row is repainted only while its
 * hierarchy widget and ListBox still pass the vtable check (hl_listbox). */
static int hl_listbox(unsigned long w,
                      uintptr_t *lb) { /* the hierarchy widget's ListBox, vtable-checked */
    uintptr_t vt = 0, lbvt = 0; /* alive: noted during EP147's call, destructor not run since */
    return w && !(w & 7) && hlw_is_live(w) && own_q(w, &vt) && vt == HIER_WIDGET_VT &&
           own_q(w + 0x48, lb) && *lb && !(*lb & 7) && own_q(*lb, &lbvt) &&
           lbvt == __atomic_load_n(&g_hl_lb_vt, __ATOMIC_ACQUIRE);
}
static void sm_repaint_labels(int world) {
    void (*repaint_row)(void *, int) = (void (*)(void *, int)) LISTBOX_REPAINTROW;
    uintptr_t lb = 0;
    for (int i = 0; i < SM_PROWS; i++) {
        unsigned long w = __atomic_exchange_n(&g_sm_lpend[i].w, 0, __ATOMIC_ACQ_REL);
        int row = __atomic_load_n(&g_sm_lpend[i].row, __ATOMIC_RELAXED);
        if (w && row >= 0 && hl_listbox(w, &lb))
            repaint_row((void *) lb, row);
    }
    for (int i = 0; world && i < SM_HL; i++) { /* a world change: every registered row */
        unsigned long w = __atomic_load_n(&g_hl[i].w, __ATOMIC_ACQUIRE);
        int row = __atomic_load_n(&g_hl[i].row, __ATOMIC_RELAXED);
        if (__atomic_load_n(&g_hl[i].comp, __ATOMIC_ACQUIRE) && row >= 0 && hl_listbox(w, &lb))
            repaint_row((void *) lb, row);
    }
}
/* the playlist rows animating: each is repainted once and dropped; painting it busy
 * again puts it back, so a row stops when its job ends or it leaves the screen */
static void sm_animate_labels(void) {
    void (*repaint_row)(void *, int) = (void (*)(void *, int)) LISTBOX_REPAINTROW;
    uintptr_t lb = 0;
    for (int i = 0; i < SM_PROWS; i++) {
        unsigned long w = __atomic_exchange_n(&g_sm_lanim[i].w, 0, __ATOMIC_ACQ_REL);
        int row = __atomic_load_n(&g_sm_lanim[i].row, __ATOMIC_RELAXED);
        if (w && row >= 0 && hl_listbox(w, &lb))
            repaint_row((void *) lb, row);
    }
}
static void install_stemsmark(void) {
    if (access(SM_OFF_FLAG, F_OK) == 0)
        return; /* escape hatch only; on = the Stems toggle */
    sm_live_init(); /* constructor phase: the destructor hook is already in */
    *(void **) &g_smsq.open_v2 = dlsym(RTLD_DEFAULT, "sqlite3_open_v2");
    *(void **) &g_smsq.key = dlsym(RTLD_DEFAULT, "sqlite3_key");
    *(void **) &g_smsq.close = dlsym(RTLD_DEFAULT, "sqlite3_close");
    *(void **) &g_smsq.bind_text = dlsym(RTLD_DEFAULT, "sqlite3_bind_text");
    *(void **) &g_smsq.threadsafe = dlsym(RTLD_DEFAULT, "sqlite3_threadsafe");
    *(void **) &g_has_stems_fn = dlsym(RTLD_DEFAULT, "azstem_has_stems");
    /* Playlist rows need which stick is browsed (selection hooks -> plsnap) and its
     * mount (library hooks -> mm_note); both shared and installed once, here directly
     * rather than through Playlist Editing or MAKE STEMS, which may be off */
    int libh = install_library_hooks(), selh = install_selection_hooks();
    int sq = sq_resolve() && g_smsq.open_v2 && g_smsq.key && g_smsq.close && g_smsq.bind_text &&
             g_smsq.threadsafe && g_smsq.threadsafe() != 0;
    int a = 0, b = 0;
    if (sq && g_has_stems_fn) {
        a = azm_install_hook(SM_AGENT_REQ, (void *) sm_req_hook, (void **) &g_sm_orig_req);
        b = install_paintcell_slot();
        /* playlist rows: the label refresh and paint slots, both shared with Playlist
         * Editing and installed once by whichever feature asks first */
        int hp = -9, lp = -9, tp = -9, tc = -9;
        if (libh == 1 && selh == 1) {
            lp = install_hlabel_paint_slot();
            hp = install_hier_refresh_slot();
            /* track titles ride the same label paint; the refresh slot chains (it saves
             * the current value, so the drag view / row memo stay behind it) */
            if (lp == 1)
                tp = azm_patch_vtable(REFRESHCELL_SLOT, (void *) sm_refresh_hook,
                                      (void **) &g_orig_sm_refresh);
            uintptr_t sc = 0; /* the titles' text clip: only on the expected function */
            if (tp == 1 && img_q(SLTEXT_PAINT_SLOT, &sc) && sc == SLTEXT_PAINT_ORIG)
                tc = azm_patch_vtable(SLTEXT_PAINT_SLOT, (void *) sltext_paint_hook,
                                      (void **) &g_orig_sltext_paint);
        }
        FILE *hf = azlog_open("/tmp/azmod-stemsmark.log", "a");
        if (hf) {
            fprintf(hf, "install playlist rows: refresh=%d paint=%d track titles=%d text clip=%d\n",
                    hp, lp, tp, tc);
            fclose(hf);
        }
    }
    pthread_t t;
    g_sm_on = a == 1 && b == 1 && az_thread_create(&t, NULL, sm_worker, NULL) == 0;
    if (g_sm_on)
        pthread_detach(t);
    FILE *f = azlog_open("/tmp/azmod-stemsmark.log", "a");
    if (f) {
        fprintf(f, "install stemsmark: sqlite=%d azstem=%d request=%d paintCell=%d -> %s\n", sq,
                g_has_stems_fn != 0, a, b, g_sm_on ? "ON" : "OFF");
        fclose(f);
    }
}
#include "preview_hotcues.c"
