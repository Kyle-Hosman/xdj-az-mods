/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file browse_fields.c
 * @brief Column Edit, any field: the track number, BPM and Key columns of the full-width Browse
 * track lists can each show a library field instead (Genre, Comments, Rating, ...), drawn by
 * EP147's own sub column painter. Only while the Mod Menu's Column Edit row is on.
 *
 * EP147's track list header has one column whose field is chosen (the sub column, with its own
 * up-down button), and its rows carry that one field: each row record of the list's row ring
 * holds one sub item (a juce::String at +0x30 and a kind at +0x4c: 0x18..0x1f draws a color's
 * dot before the text). The other columns' painters draw fixed data (the number, BPM, key). So a
 * field on another column has no value in EP147's memory; this file reads it from the stick's
 * library and lets EP147 draw it:
 *   - the buttons: in Column Edit's edit mode each of those columns gets a second instance of
 *     EP147's sub column button (same class, same images, same name) at its right edge. Its click
 *     goes to the same list listener as EP147's own button's, so EP147 opens its own field list;
 *     the pick that follows is that column's (bf_take_pick, asked first by browse_columns.c's
 *     changeSubCateory hook) and is not passed to EP147. DEFAULT gives the column its own data
 *     back; Track (the title) is not offered as a column's field. On a column that can be hidden
 *     (browse_columns.c) the list gets one more row, Hide Column (b2_hide_row), and a pick on a
 *     hidden column shows it again;
 *   - the cells: paintCell for such a column calls EP147's painter for the sub column with the
 *     row's sub item swapped for this column's value, and puts it back (bf_paint; message thread);
 *     the title's cells are components, whose paint calls bf_paint instead (b2_title_paint_hook);
 *   - the names: the column's header shows the field's name (browse_edit.c names the columns);
 *   - sorting: a tap on such a column's header does not sort by the column's own data;
 *   - the values: the row's track (stem_marks.c's sm_lookup: the list's reply page, checked
 *     against the row's title) is looked up on a worker of its own, on its own read-only
 *     connections to each stick's library, kept while EP147 holds that library (libfd.h tells
 *     EP147's connections from azmod's). The text is EP147's format for the field
 *     (browse_fields.h). A row whose value is not known yet is drawn empty and repainted when it
 *     lands.
 *
 * Saved: settings/azmod-colfields (browse_fields.h b2_fields_parse). Kill switch:
 * settings/azmod-colfields.off. Entry points: install_browse_fields, bf_tick (message thread),
 * bf_paint (paintcell_hook), bf_take_pick (browse_columns.c), bf_after_layout and
 * bf_field_of (browse_edit.c).
 */
/* EP147 (v1.30 names; v2.00 in the address table):
 *   gui::browse::TrackListHeader ctor FUN_01bcff88(this, mode): adds its columns, then (mode 0)
 *     builds the sub column button: new(0x230), ctor FUN_01e06450(btn, &name, &image, &image2,
 *     &image3, 1, 1, 0.3f, 0.5f) with the image from FUN_01bb99f0(FUN_01bc41b0()) (sret x8) and
 *     two empty juce::Image, setSize(0x50, 0x3c), btn+0x1c0 = the 8 bytes at 0x2b7da80,
 *     addAndMakeVisible(header, btn, -1); stored at +0x138. Destructors: vtable slots 0 and 1;
 *     both delete +0x138 through its vtable slot 1.
 *   FUN_01bd03f0(header, listener): adds a listener to [header+0x138] (header mode 0 only).
 *   The list's button listener is TrackListWidget+0xb0 (vtable 0x2b7f6d8): slot 2 the click,
 *     called with the button's name; it acts only on EP147's own name String 0x3b8e9f8 (the text
 *     pointer is compared).
 *   juce::TableHeaderComponent: getIndexOfColumnId 0x2459540 (id, onlyVisible), getColumnPosition
 *     0x2459640 (index; Rectangle<int> sret x8). juce::Component: setBounds 0x24a0890
 *     (const Rectangle<int>&), setSize 0x249f200, addAndMakeVisible 0x249d6b0.
 *     juce::String(CharPointer_UTF8) 0x233c340 (this, &utf8), ~String 0x22fc600.
 *     juce::Image() 0x23b12b0, ~Image 0x23a45f0. operator new 0x424d40.
 *   gui::disp_data::BrowseDispData ctor FUN_01f3fb18; +0x70 its SubCategoryDispList (vtable
 *     0x29672e0): +0x08 the current index, +0x18 the tokens (juce::String array), +0x28 count.
 *   gui::browse::SubCategorySelectPopupWidget (the field list; kept once built). Its juce::Component
 *     is a secondary base at +0xb0 (offset-to-top -0xb0 in both builds), which is what the
 *     component tree holds; from the object's start: +0x58 its TableListBox, +0x60 the row labels (juce::String array: data, +0x68 allocated, +0x70
 *     count). Its list model at +0x30 (vtable 0x2bcc810): slot 2 getNumRows FUN_01c28db0 returns
 *     the label count; slot 18 paintCell draws label[row] (FUN_01c290c0: nothing past the
 *     count); slot 6 FUN_01c2a9d0(model, row), a row tapped: hides the popup (its vtable +0x60
 *     with 0), then, for row >= 0 only, tells its listener (+0x98, slot +0x18) the row, which
 *     reaches changeSubCateory with that index. The labels are set as a whole by FUN_01c29898
 *     when the sub column list changes, not when the popup opens; FUN_01c296d8(popup, &string)
 *     adds one (grows the array, copies the String), FUN_01c296b8(popup) updates the list's
 *     content (JUCE repaints only the rows whose index or selection changed, so the popup is
 *     repainted after it). Same layout and the same three functions in v2.00. */
#include "browse_fields.h"
#include "ht_tap.h"

#define B2_HDR_CTOR EP_1bcff88
#define B2_COLINDEX EP_2459540
#define B2_COLPOS EP_2459640
#define B2_SETBOUNDS EP_24a0890
#define B2_SETSIZE EP_249f200
#define B2_ADDCHILD EP_249d6b0
#define B2_STR_U8 EP_233c340 /* juce::String(CharPointer_UTF8): what EP147 builds its own texts with */
#define B2_STR_DTOR EP_22fc600
#define B2_IMG_CTOR EP_23b12b0
#define B2_IMG_DTOR EP_23a45f0
#define B2_NEW EP_424d40
#define B2_BTN_CTOR EP_1e06450
#define B2_BTN_RES EP_1bc41b0
#define B2_BTN_IMG EP_1bb99f0
#define B2_BTN_NAME EP_3b8e9f8
#define B2_BTN_D0 EP_2b7da80
#define B2_ADDLIS EP_1bd03f0
#define B2_WLIS_VT EP_2b7f6d8
#define B2_BDD_CTOR EP_1f3fb18
#define B2_BDD_VT EP_2dcc3a0
#define B2_SCL_VT EP_29672e0
#define B2_POP_ADD EP_1c296d8      /* the field list: add a row label (popup, const String &) */
#define B2_POP_UPDATE EP_1c296b8   /* the field list: its TableListBox updateContent (popup) */
#define B2_POP_MODEL_VT EP_2bcc810 /* the field list's list model (popup + B2_POP_MODEL) */
#define B2_POP_MODEL 0x30
#define B2_POP_CLICK 0x30 /* model slot 6: a row tapped */
#define B2_POP_LABELS 0x60
#define B2_POP_NLABELS 0x70
#define B2_HIDE_LABEL "Hide Column"
#define B2_HDR_BTN 0x138
#define B2_BTN_LIS 0x210 /* the button's listener array: data, +0x10 count */
#define B2_BTN_SIZE 0x230
#define B2_BTN_W 0x50
#define B2_BTN_H 0x3c
#define B2_BTN_D0_OFF 0x1c0
#define B2_ROW_STR 0x30  /* a row record: the sub item's juce::String */
#define B2_ROW_KIND 0x4c /* ... and its kind */
#ifndef B2_MEDIA
#define B2_MEDIA "/media/usb" /* where the sticks are mounted */
#endif
#ifndef BF_PATH
#define BF_PATH "/home/root/settings/azmod-colfields"
#define BF_OFF "/home/root/settings/azmod-colfields.off"
#endif
#define B2_HDR_MAX 8
#define BF_NBTN 6 /* the columns that can show a field: Preview, artwork, #, BPM, Key, the title */
#define B2_TL 64   /* title components remembered (a row each, over the lists on screen) */
#define B2_TL_PAST 2 /* the title's field painted this much wider than its text: no divider */
#define B2_CACHE 512
#define B2_Q 32
#define B2_DB_MAX 4
#define B2_ASK_MS 60000 /* a pick this long after a button's click is not that column's */

void mp_sret2(uintptr_t *out, uintptr_t fn, uintptr_t a0, uintptr_t a1); /* menu_popup.c */

static int b2_sub_id(void) { return ep_off(5, 6); }
static void bf_log(const char *fmt, ...) {
    FILE *f = azlog_open("/tmp/azmod-colfields.log", "a");
    if (!f)
        return;
    fprintf(f, "[%ld] ", (long) time(NULL));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

/* ---- the choice ---------------------------------------------------------------------------- */
static int g_b2_on;    /* installed */
static int g_b2_ready; /* the header hooks are all in: headers may get the buttons (message thread) */
static unsigned char g_bf_field[B2_MAXID + 1]; /* each column's field, B2_NONE its own (__atomic only) */
static int g_b2_layout; /* the headers must lay out again (__atomic only) */
static int g_b2_refresh; /* the full-width lists must repaint (__atomic only) */
/* the column ids that can show a field: Preview 1, the title 4 / 5, the others browse_columns.c
 * bc_col_id. The sub column's field is EP147's own choice (its header's own button). */
static int bf_col(int i) {
    switch (i) {
    case 0: return 1;
    case 1: return bc_col_id(AZMOD_BCOL_ART);
    case 2: return bc_col_id(AZMOD_BCOL_NUM);
    case 3: return bc_col_id(AZMOD_BCOL_BPM);
    case 4: return bc_col_id(AZMOD_BCOL_KEY);
    case 5: return ep_off(4, 5);
    default: return 0;
    }
}
static int bf_swappable(int id) {
    for (int i = 0; i < BF_NBTN; i++)
        if (bf_col(i) == id)
            return 1;
    return 0;
}
/* the field column id shows, B2_NONE for its own data */
static int bf_field_of(int id) {
    if (!g_b2_on || !bc_ce_on() || id < 1 || id > B2_MAXID)
        return B2_NONE;
    return __atomic_load_n(&g_bf_field[id], __ATOMIC_ACQUIRE);
}
static void bf_save(void) {
    unsigned char f[B2_MAXID + 1];
    char buf[B2_FILE_MAX];
    for (int id = 0; id <= B2_MAXID; id++)
        f[id] = __atomic_load_n(&g_bf_field[id], __ATOMIC_ACQUIRE);
    int n = b2_fields_format(f, buf, sizeof buf);
    if (n >= 0)
        settings_write(BF_PATH, buf, (size_t) n, 1);
}
static void bf_load(void) {
    char buf[B2_FILE_MAX + 1];
    unsigned char f[B2_MAXID + 1];
    FILE *fp = fopen(BF_PATH, "re");
    if (!fp)
        return;
    size_t n = fread(buf, 1, sizeof buf, fp);
    fclose(fp);
    int rc = b2_fields_parse(buf, n, f);
    for (int id = 1; id <= B2_MAXID; id++)
        if (rc > 0 && bf_swappable(id))
            __atomic_store_n(&g_bf_field[id], f[id], __ATOMIC_RELEASE);
    bf_log("fields loaded: %d\n", rc);
}
static void bf_set_field(int id, int f) {
    if (!bf_swappable(id) || f < B2_NONE || f >= B2_N)
        return;
    __atomic_store_n(&g_bf_field[id], (unsigned char) (f == B2_TRACK ? B2_NONE : f), __ATOMIC_RELEASE);
    __atomic_store_n(&g_b2_layout, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_b2_refresh, 1, __ATOMIC_RELEASE);
    bf_save();
    bf_log("column %d: %s\n", id, B2_FIELDS[f == B2_TRACK ? B2_NONE : f].token);
}

/* every column back to its own data (Column Edit's RESET) */
static void bf_reset(void) {
    for (int id = 1; id <= B2_MAXID; id++)
        __atomic_store_n(&g_bf_field[id], (unsigned char) B2_NONE, __ATOMIC_RELEASE);
    __atomic_store_n(&g_b2_layout, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_b2_refresh, 1, __ATOMIC_RELEASE);
    bf_save();
    bf_log("fields reset\n");
}

/* ---- the values ------------------------------------------------------------------------- */
/* st: 1 queued, 2 known, 3 unknown. text / kind are written under g_b2_mu; jstr (the juce::String
 * made from text) only on the message thread. */
static struct b2_ent {
    unsigned cid, h;
    unsigned char field, st, made;
    int kind;
    long long used;
    char text[B2_TEXT_MAX];
    uintptr_t jstr;
} g_b2c[B2_CACHE];
static struct {
    unsigned cid, h;
    int field;
    char title[160];
} g_b2q[B2_Q];
static int g_b2q_n;
static pthread_mutex_t g_b2_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_b2_cv = PTHREAD_COND_INITIALIZER;
static __thread int g_b2_idle_thread;
static int g_b2_landed; /* a value landed: repaint the lists (__atomic only) */
/* The worker runs SCHED_IDLE; it leaves the idle class while it holds g_b2_mu, which the
 * painter also takes (the same rule as stem_marks.c's sm_lock). */
static void b2_lock(void) {
    if (g_b2_idle_thread) {
        struct sched_param sp = {0};
        pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
    }
    pthread_mutex_lock(&g_b2_mu);
}
static void b2_unlock(void) {
    pthread_mutex_unlock(&g_b2_mu);
    if (g_b2_idle_thread) {
        struct sched_param sp = {0};
        pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp);
    }
}
static void b2_str_free(uintptr_t *s) { /* message thread */
    if (*s)
        ((void (*)(uintptr_t *)) B2_STR_DTOR)(s);
    *s = 0;
}
static struct b2_ent *b2_ent(unsigned cid, unsigned h, int field, int create) { /* g_b2_mu held */
    struct b2_ent *old = &g_b2c[0];
    for (int i = 0; i < B2_CACHE; i++) {
        struct b2_ent *e = &g_b2c[i];
        if (e->st && e->cid == cid && e->h == h && e->field == field)
            return e;
        if (e->used < old->used)
            old = e;
    }
    if (!create)
        return 0;
    uintptr_t js = old->jstr; /* message thread: create is only asked by the painter */
    memset(old, 0, sizeof *old);
    old->jstr = js;
    b2_str_free(&old->jstr);
    old->cid = cid;
    old->h = h;
    old->field = (unsigned char) field;
    return old;
}
/* Message thread (a paint): the value of (cid, title) for the field, as a juce::String and a
 * kind; 0 while it is not known (queued once), or unknown. */
static int b2_get(unsigned cid, const char *title, int field, uintptr_t *jstr, int *kind) {
    unsigned h = sm_hash(title);
    int ok = 0;
    b2_lock();
    struct b2_ent *e = b2_ent(cid, h, field, 1);
    e->used = drag_now_ms();
    if (!e->st && g_b2q_n < B2_Q) {
        g_b2q[g_b2q_n].cid = cid;
        g_b2q[g_b2q_n].h = h;
        g_b2q[g_b2q_n].field = field;
        snprintf(g_b2q[g_b2q_n].title, sizeof g_b2q[g_b2q_n].title, "%s", title);
        g_b2q_n++;
        e->st = 1;
        pthread_cond_signal(&g_b2_cv);
    }
    if (e->st == 2) {
        if (!e->made) {
            const char *u8 = e->text;
            ((void (*)(uintptr_t *, const char **)) B2_STR_U8)(&e->jstr, &u8);
            e->made = 1;
        }
        *jstr = e->jstr;
        *kind = e->kind;
        ok = e->jstr != 0;
    }
    b2_unlock();
    return ok;
}
/* the stick connections: this worker's own, read-only and lock-free (immutable=1), kept while
 * EP147 holds that stick's library open; worker only */
static struct b2_db {
    char mount[128];
    void *db;
    long long sig, checked, handsoff;
    int others;
} g_b2db[B2_DB_MAX];
static void b2_db_close(struct b2_db *d, const char *why) {
    if (d->db) {
        libfd_del(libfd_fd_of(d->db));
        g_smsq.close(d->db);
        bf_log("DB closed %s: %s\n", d->mount, why);
    }
    d->db = 0;
    d->sig = 0;
}
/* EP147's own fds on path: every fd on it but the ones azmod's workers registered (libfd.h);
 * -1 when it cannot tell */
static int b2_fds_on(const char *path) {
    int n = 0;
    DIR *fd = opendir("/proc/self/fd");
    if (!fd)
        return -1;
    struct dirent *e;
    while ((e = readdir(fd))) {
        char lp[64], tgt[256];
        if (e->d_name[0] == '.' || snprintf(lp, sizeof lp, "/proc/self/fd/%s", e->d_name) >= (int) sizeof lp)
            continue;
        ssize_t k = readlink(lp, tgt, sizeof tgt - 1);
        if (k <= 0)
            continue;
        tgt[k] = 0;
        if (!strcmp(tgt, path) && !libfd_owner(atoi(e->d_name)))
            n++;
    }
    closedir(fd);
    return n;
}
/* 1 = EP147 holds this stick's library and no eject has begun (EP147 closes its connections
 * first when it ejects: from the first close, hands off for SM_HANDSOFF_MS) */
static int b2_usable(struct b2_db *d, const char *dbp) {
    long long now = drag_now_ms();
    if (now < d->handsoff)
        return 0;
    if (d->checked && now - d->checked < 250)
        return d->others > 0;
    int others = b2_fds_on(dbp);
    if (others < 0)
        return 0;
    if (d->checked && d->others > 0 && others < d->others) {
        d->handsoff = now + SM_HANDSOFF_MS;
        bf_log("%s: EP147's connections %d -> %d: hands off\n", d->mount, d->others, others);
    }
    d->others = others;
    d->checked = now;
    return others > 0 && now >= d->handsoff;
}
static void *b2_db_for(const char *mount) {
    char dbp[200], uri[240];
    if (snprintf(dbp, sizeof dbp, "%s/%s/rekordbox/exportLibrary.db", mount, pd_dir(mount)) >= (int) sizeof dbp)
        return 0;
    struct b2_db *d = 0;
    for (int i = 0; i < B2_DB_MAX && !d; i++)
        if (!strcmp(g_b2db[i].mount, mount))
            d = &g_b2db[i];
    for (int i = 0; i < B2_DB_MAX && !d; i++)
        if (!g_b2db[i].mount[0] || (!g_b2db[i].db && !sm_is_mounted(g_b2db[i].mount))) {
            d = &g_b2db[i];
            memset(d, 0, sizeof *d);
            snprintf(d->mount, sizeof d->mount, "%s", mount);
        }
    if (!d)
        return 0;
    if (!b2_usable(d, dbp)) {
        if (d->db)
            b2_db_close(d, "EP147 is letting go of it (eject)");
        return 0;
    }
    long long sig = sm_file_sig(dbp);
    if (d->db && d->sig == sig && sig)
        return d->db;
    if (d->db)
        b2_db_close(d, "the library changed");
    if (!sig)
        return 0;
    snprintf(uri, sizeof uri, "file:%s?immutable=1", dbp);
    void *db = 0;
    if (g_smsq.open_v2(uri, &db, 0x01 | 0x40 /* READONLY | URI */, 0) != 0 || !db) {
        if (db)
            g_smsq.close(db);
        return 0;
    }
    g_smsq.key(db, ONELIBRARY_KEY_SM, (int) sizeof ONELIBRARY_KEY_SM - 1);
    d->db = db;
    d->sig = sig;
    libfd_add(libfd_of_db(db, dbp), LIBFD_FIELDS);
    bf_log("DB open %s\n", dbp);
    return db;
}
/* between jobs: let go of a stick being ejected or gone */
static void b2_db_release(void) {
    for (int i = 0; i < B2_DB_MAX; i++) {
        struct b2_db *d = &g_b2db[i];
        char dbp[240];
        if (!d->db)
            continue;
        if (snprintf(dbp, sizeof dbp, "%.128s/%s/rekordbox/exportLibrary.db", d->mount, pd_dir(d->mount)) >= (int) sizeof dbp ||
            !b2_usable(d, dbp))
            b2_db_close(d, "EP147 is letting go of it (eject)");
    }
}
/* worker: the field's value for (cid, title) over every stick that holds it. 1 known (the
 * sticks agree), 0 unknown. */
static int b2_resolve(unsigned cid, const char *title, int field, char *out, size_t cap, int *kind) {
    DIR *rd = opendir(B2_MEDIA);
    if (!rd)
        return 0;
    struct dirent *de;
    int hits = 0, bad = 0;
    char got[B2_TEXT_MAX];
    int gk = 0;
    while ((de = readdir(rd)) && !bad) {
        char mount[128];
        if (de->d_name[0] == '.' || snprintf(mount, sizeof mount, "%s/%s", B2_MEDIA, de->d_name) >= (int) sizeof mount)
            continue;
        void *db = b2_db_for(mount);
        void *st = 0;
        if (!db)
            continue;
        if (g_sq.prepare(db, B2_FIELDS[field].sql, -1, &st, 0) != 0 || !st) {
            bad = 1; /* torn or undecryptable: unknown */
            continue;
        }
        g_sq.bind_int64(st, 1, cid);
        g_smsq.bind_text(st, 2, title, -1, (void *) -1 /* SQLITE_TRANSIENT */);
        while (g_sq.step(st) == SQLITE_ROW) {
            const unsigned char *num = g_sq.column_text(st, 0), *txt = g_sq.column_text(st, 1);
            char v[B2_TEXT_MAX];
            int k = 0;
            b2_format(field, num != 0, num ? g_sq.column_int64(st, 0) : 0, (const char *) txt, v, sizeof v, &k);
            if (!hits) {
                snprintf(got, sizeof got, "%s", v);
                gk = k;
            } else if (strcmp(got, v) || gk != k) {
                bad = 1; /* two sticks hold this id and title with different values */
            }
            hits++;
        }
        g_sq.finalize(st);
    }
    closedir(rd);
    if (!hits || bad)
        return 0;
    snprintf(out, cap, "%s", got);
    *kind = gk;
    return 1;
}
static int g_b2_stop; /* the worker returns (a test's end; __atomic only) */
static void *b2_worker(void *arg) {
    (void) arg;
    struct sched_param sp = {0};
    if (pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp) == 0)
        g_b2_idle_thread = 1;
    while (!__atomic_load_n(&g_b2_stop, __ATOMIC_ACQUIRE)) {
        unsigned cid = 0, h = 0;
        int field = 0;
        char title[160] = "";
        b2_lock();
        if (!g_b2q_n) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&g_b2_cv, &g_b2_mu, &ts);
        }
        if (g_b2q_n) {
            cid = g_b2q[0].cid;
            h = g_b2q[0].h;
            field = g_b2q[0].field;
            snprintf(title, sizeof title, "%s", g_b2q[0].title);
            memmove(&g_b2q[0], &g_b2q[1], sizeof g_b2q[0] * (size_t) --g_b2q_n);
        }
        b2_unlock();
        if (!cid || field <= B2_NONE || field >= B2_N) {
            b2_db_release();
            continue;
        }
        char v[B2_TEXT_MAX] = "";
        int k = 0, ok = b2_resolve(cid, title, field, v, sizeof v, &k);
        b2_lock();
        struct b2_ent *e = b2_ent(cid, h, field, 0);
        if (e && e->st == 1) {
            e->st = ok ? 2 : 3;
            snprintf(e->text, sizeof e->text, "%s", v);
            e->kind = k;
            __atomic_store_n(&g_b2_landed, 1, __ATOMIC_RELEASE);
        }
        b2_unlock();
    }
    return 0;
}
/* ---- the cells -------------------------------------------------------------------------- */
#define B2_LISTS 8
/* the track lists that drew this column: wref slots (ep_mem.h), cleared by the list's destructor */
static _Atomic uintptr_t g_b2_lists[B2_LISTS];
static int g_b2_lists_ok; /* the slots are tracked: a list may be remembered */
struct b2_swap {
    uintptr_t at; /* the row record, 0 when nothing is swapped */
    uint64_t str;
    int kind;
};
/* remember a list, for the repaint when a value lands and the refresh when a field is picked */
static void b2_note_list(uintptr_t self) {
    int seen = 0, free_i = -1;
    for (int i = 0; g_b2_lists_ok && i < B2_LISTS && !seen; i++) {
        uintptr_t l = wref_get(&g_b2_lists[i]);
        seen = l == self;
        if (free_i < 0 && !l)
            free_i = i;
    }
    if (g_b2_lists_ok && !seen && free_i >= 0)
        wref_set(&g_b2_lists[free_i], self);
}
static int b2_full_list(uintptr_t w);
static uintptr_t g_b2_sel_model;
static int g_b2_sel_row, g_b2_sel;
/* put the row's own sub item back (also when EP147's painter unwinds) */
static void b2_restore(struct b2_swap *s) {
    if (!s->at)
        return;
    *(uint64_t *) (s->at + B2_ROW_STR) = s->str;
    *(int *) (s->at + B2_ROW_KIND) = s->kind;
    s->at = 0;
}
/* Message thread: draw row `row`'s cell of column `col` with EP147's sub column painter when the
 * column shows a field. 1 when the cell was handled (drawn, or left empty on purpose), 0 when the
 * column shows its own data (EP147 paints it). */
static int bf_paint(void *model, void *g, int row, int col, int w, int h, int sel) {
    if (col != 1) {
        g_b2_sel_model = (uintptr_t) model;
        g_b2_sel_row = row;
        g_b2_sel = sel;
    }
    int field = bf_field_of(col);
    if (field == B2_NONE)
        return 0;
    uintptr_t self = (uintptr_t) model - 0x30, ring = 0, v = 0, tp = 0;
    if (!bc_obj(self, TRACKLIST_WIDGET_VT) || !b2_full_list(self))
        return 0; /* a compact pane: its columns show their own data */
    if (row < 0)
        return 1;
    if (!own_q(self + 0x2a8, &ring) || !ring || !own_q(self + 0x2c0, &v) || row >= (int) (v & 0xffffffff))
        return 1;
    uintptr_t rec = ring + (uintptr_t) (row % 0x1b) * 0x78;
    char tt[160];
    unsigned kind = 0, id = 0;
    if (!own_q(rec, &v) || (int) (v & 0xffffffff) != row || !own_q(rec + 0x70, &v) || (v & 0xff) ||
        !own_q(rec + 0x28, &tp) || tp < 0x10000 || !own_str(tp, tt, sizeof tt) || !tt[0] ||
        !sm_lookup(row, tt, &kind, &id) || kind != REC_KIND_TRACK || !id)
        return 1;
    b2_note_list(self);
    uintptr_t js = 0;
    int k = 0;
    if (!b2_get(id, tt, field, &js, &k))
        return 1; /* not known yet: empty, repainted when it lands */
    struct b2_swap sw __attribute__((cleanup(b2_restore))) = {0, 0, 0};
    sw.str = *(uint64_t *) (rec + B2_ROW_STR);
    sw.kind = *(int *) (rec + B2_ROW_KIND);
    sw.at = rec;
    *(uint64_t *) (rec + B2_ROW_STR) = js;
    *(int *) (rec + B2_ROW_KIND) = k;
    g_orig_paintcell(model, g, row, b2_sub_id(), w, h, sel);
    b2_restore(&sw);
    return 1;
}

/* ---- the header: a button on each column that can show a field ------------------------------ */
static struct b2_hdr {
    uintptr_t hdr, btn[BF_NBTN], btn_vt[BF_NBTN];
} g_b2h[B2_HDR_MAX];
/* the listener on a header's buttons: slot 2 is the click (this, &name); col 0 on EP147's own */
struct b2_lis {
    const void *vt;
    int col;
    uintptr_t hdr;
};
static struct b2_lis g_b2_lis[B2_HDR_MAX][BF_NBTN + 1];
static int g_bf_ask; /* the column whose button opened EP147's list, 0 none (__atomic only) */
static long long g_bf_ask_ms;
static void b2_lis_nop(void *self) { (void) self; }
static void b2_lis_click(struct b2_lis *self, void *name);
static void (*const g_b2_lis_vt[6])(void) = {
    (void (*)(void)) b2_lis_nop,
    (void (*)(void)) b2_lis_nop,
    (void (*)(void)) b2_lis_click,
    (void (*)(void)) b2_lis_nop,
    (void (*)(void)) b2_lis_nop,
    (void (*)(void)) b2_lis_nop,
};
static uintptr_t g_b2_bdd; /* BrowseDispData (__atomic only) */
/* EP147's sub column list (SubCategoryDispList of the BrowseDispData), or 0 */
static uintptr_t b2_sub_list(void) {
    uintptr_t bdd = bc_obj(__atomic_load_n(&g_b2_bdd, __ATOMIC_ACQUIRE), B2_BDD_VT), sl = 0;
    return bdd && own_q(bdd + 0x70, &sl) ? bc_obj(sl, B2_SCL_VT) : 0;
}
/* The index of field f's token in EP147's field list, or -1 */
static int b2_list_index(int f) {
    uintptr_t sl = b2_sub_list(), data = 0, n = 0, t = 0;
    char tok[48];
    if (!sl || !own_q(sl + 0x18, &data) || !own_q(sl + 0x28, &n) || f < B2_NONE || f >= B2_N)
        return -1;
    for (int i = 0; i < (int) (n & 0xffffffff) && i < 64; i++)
        if (own_q(data + 8 * (uintptr_t) i, &t) && t && own_str(t, tok, sizeof tok) &&
            !strcmp(tok, B2_FIELDS[f].token))
            return i;
    return -1;
}
/* a component's bounds in its parent (juce::Component +0x20: x, y, width, height) */
static int b2_bounds(uintptr_t c, int b[4]) {
    uintptr_t xy = 0, wh = 0;
    if (!c || (c & 7) || !own_q(c + 0x20, &xy) || !own_q(c + 0x28, &wh))
        return 0;
    b[0] = (int) (int32_t) xy, b[1] = (int) (int32_t) (xy >> 32), b[2] = (int) (int32_t) wh,
    b[3] = (int) (int32_t) (wh >> 32);
    return b[2] > 0 && b[3] > 0;
}
static struct b2_hdr *b2_of(uintptr_t h);
/* obj's class is `name` (its RTTI: [vtable - 8] the type_info, +8 its mangled name) */
static int b2_rtti_is(uintptr_t obj, const char *name) {
    uintptr_t vt = 0, ti = 0, np = 0;
    char got[80];
    return obj && !(obj & 7) && own_q(obj, &vt) && vt && !(vt & 7) && own_q(vt - 8, &ti) && ti && own_q(ti + 8, &np) &&
           own_str(np, got, sizeof got) && !strcmp(got, name);
}
/* EP147's field list just opened: the SubCategorySelectPopupWidget under the top-level component
 * of c (juce::Component +0x18 parent, +0x40 / +0x50 the children and their count), or 0 */
#define B2_POPUP_RTTI "N3gui6browse28SubCategorySelectPopupWidgetE"
static uintptr_t b2_find_popup(uintptr_t c) {
    uintptr_t top = c, p = 0, q[256];
    for (int d = 0; d < 32 && own_q(top + 0x18, &p) && p && !(p & 7); d++)
        top = p;
    int head = 0, tail = 0, seen = 0;
    q[tail++] = top;
    while (head < tail && seen++ < 4096) {
        uintptr_t n = q[head++], data = 0, cnt = 0, ch = 0;
        if (b2_rtti_is(n, B2_POPUP_RTTI))
            return n;
        if (!own_q(n + 0x40, &data) || !own_q(n + 0x50, &cnt) || !data || (data & 7))
            continue;
        int k = (int) (int32_t) cnt;
        for (int i = 0; i < k && i < 512; i++) {
            if (!own_q(data + 8 * (uintptr_t) i, &ch) || !ch || (ch & 7))
                continue;
            if (tail == (int) (sizeof q / sizeof q[0])) /* full: a ring of the newest */
                head = tail = 0;
            q[tail++] = ch;
        }
    }
    return 0;
}
/* the field list to move once it is up: under our button, as EP147 places it under its own
 * (dx: our button's right edge less EP147's own, in the header). (message thread) */
#define B2_POP_MS 1500
#define B2_POPLIST_RTTI "N4meow21TouchableTableListBoxE"
static struct {
    uintptr_t hdr;
    int dx, own_r, row; /* row: the field's row in the list, -1 unknown */
    long long until;
} g_b2_pop;
/* The popup as EP147 placed it, while one is moved: EP147 keeps the one popup and does not
 * place it again, so a move is made from these bounds and EP147's own button puts them back.
 * The slot is a wref (ep_mem.h): the popup's destructor clears it. (message thread) */
static _Atomic uintptr_t g_b2_popw[1];
static int g_b2_popw_ok, g_b2_pop_orig[4];
static uintptr_t b2_sub_list(void);

/* ---- Hide Column: one more row in EP147's field list --------------------------------------
 * Added through the popup's own add when one of our buttons on a column that can be hidden opens
 * the list, and taken off when any other button opens it. Both happen at the tap, before EP147
 * shows the list: its rows are laid out when it is shown, and a row added to a list on screen
 * appears only the next time it opens. A tap on the row is ours (b2_pop_click): the popup closes
 * as for a tap outside its rows and the column is hidden; EP147 never gets that row's index. The
 * row is only trusted while the popup is the one g_b2_hidew follows (its destructor clears that)
 * and its last label is ours. (message thread) */
static struct {
    uintptr_t pop;
    int idx, col;
} g_b2_hide;
static int g_b2_hide_ok; /* the tap on the row is ours (install_browse_fields) */
static _Atomic uintptr_t g_b2_hidew[1]; /* the popup's component that has our row (a wref) */
/* the column bit of column id (browse_columns.c), 0 for a column that cannot be hidden */
static unsigned b2_col_bit(int id) {
    for (unsigned b = 1; b <= AZMOD_BCOL_ALL; b <<= 1)
        if (bc_col_id(b) == id)
            return b;
    return 0;
}
/* column id is hidden by the DJ */
static int b2_col_hidden(int id) {
    unsigned b = b2_col_bit(id), m = bc_mask();
    return b && (AZMOD_BCOL_SET_OF(m) & b) && !(m & b);
}
/* the popup's row labels: their array and count; 0 when not readable */
static int b2_pop_labels(uintptr_t pop, uintptr_t *data, int *n) {
    uintptr_t d = 0, c = 0;
    if (!own_q(pop + B2_POP_LABELS, &d) || !own_q(pop + B2_POP_NLABELS, &c))
        return 0;
    *data = d;
    *n = (int) (int32_t) c;
    return *n >= 0 && *n <= 64 && (d || !*n) && !(d & 7);
}
/* The popup object whose juce::Component c is (c + the offset-to-top of c's vtable), checked by
 * its list model's vtable; 0 for anything else. */
static uintptr_t b2_pop_obj(uintptr_t c) {
    uintptr_t vt = 0, ott = 0, m = 0;
    if (!c || !b2_rtti_is(c, B2_POPUP_RTTI) || !own_q(c, &vt) || !own_q(vt - 16, &ott))
        return 0;
    int64_t off = (int64_t) ott;
    if (off > 0 || off < -0x400 || (off & 7))
        return 0;
    uintptr_t obj = c + (uintptr_t) off;
    return own_q(obj + B2_POP_MODEL, &m) && m == B2_POP_MODEL_VT ? obj : 0;
}
/* our row is the last one of the popup object pop, the one g_b2_hidew follows */
static int b2_hide_present(uintptr_t pop) {
    uintptr_t data = 0, t = 0;
    int n = 0;
    char tok[24];
    return pop && g_b2_hide.pop == pop && b2_pop_obj(wref_get(&g_b2_hidew[0])) == pop &&
           b2_pop_labels(pop, &data, &n) && n > 0 && g_b2_hide.idx == n - 1 &&
           own_q(data + 8 * (uintptr_t) (n - 1), &t) && t && own_str(t, tok, sizeof tok) && !strcmp(tok, B2_HIDE_LABEL);
}
/* The popup whose component is c gets our row for column col (a column that can be hidden), or
 * loses it (col 0). */
static void b2_hide_row(uintptr_t c, int col) {
    uintptr_t data = 0, pop = b2_pop_obj(c);
    int n = 0, have = b2_hide_present(pop), want = g_b2_hide_ok && col && b2_col_bit(col) != 0;
    if (!pop || !b2_pop_labels(pop, &data, &n))
        return;
    if (have && want) {
        g_b2_hide.col = col;
        return;
    }
    if (have) { /* the last label goes: its String released, the count one less */
        ((void (*)(uintptr_t)) B2_STR_DTOR)(data + 8 * (uintptr_t) (n - 1));
        *(int32_t *) (pop + B2_POP_NLABELS) = n - 1;
        memset(&g_b2_hide, 0, sizeof g_b2_hide);
        ((void (*)(uintptr_t)) B2_POP_UPDATE)(pop);
        ((void (*)(uintptr_t)) RC_REPAINT)(c); /* a row whose index did not change is not repainted */
        bf_log("Hide Column row taken off\n");
    } else if (want && (wref_get(&g_b2_hidew[0]) == c || wref_set(&g_b2_hidew[0], c))) {
        const char *text = B2_HIDE_LABEL;
        uintptr_t str = 0;
        ((void (*)(uintptr_t *, const char **)) B2_STR_U8)(&str, &text);
        ((void (*)(uintptr_t, uintptr_t *)) B2_POP_ADD)(pop, &str);
        ((void (*)(uintptr_t *)) B2_STR_DTOR)(&str);
        g_b2_hide.pop = pop;
        g_b2_hide.idx = n;
        g_b2_hide.col = col;
        ((void (*)(uintptr_t)) B2_POP_UPDATE)(pop);
        ((void (*)(uintptr_t)) RC_REPAINT)(c);
        bf_log("Hide Column row %d for column %d\n", n, col);
    }
}
/* the column's own hide: hidden in every full-width list until shown again or RESET */
static void b2_hide_col(int col) {
    unsigned b = b2_col_bit(col), m = bc_mask();
    if (!b)
        return;
    bc_set_mask((m & ~b) | AZMOD_BCOL_SET(b));
    bf_log("column %d hidden\n", col);
}
/* the list model's row tap: our row hides its column, any other is EP147's */
static void (*g_o_b2_pop_click)(long, int);
static void b2_pop_click(long model, int row) {
    uintptr_t pop = (uintptr_t) model - B2_POP_MODEL;
    if (row >= 0 && b2_hide_present(pop) && row == g_b2_hide.idx) {
        int col = g_b2_hide.col;
        __atomic_store_n(&g_bf_ask, 0, __ATOMIC_RELEASE);
        g_o_b2_pop_click(model, -1); /* the popup closes, no row is passed on */
        b2_hide_col(col);
        return;
    }
    g_o_b2_pop_click(model, row);
}

static void b2_pop_restore(void) {
    uintptr_t pop = wref_get(&g_b2_popw[0]), data = 0, cnt = 0, lb = 0, sl = 0, cur = 0;
    if (pop && b2_rtti_is(pop, B2_POPUP_RTTI)) {
        ((void (*)(uintptr_t, const int *)) B2_SETBOUNDS)(pop, g_b2_pop_orig);
        /* the row EP147 marks: the sub column list's current index (EP147 does not select it
         * again when the list opens) */
        if (own_q(pop + 0x40, &data) && own_q(pop + 0x50, &cnt) && (int) (int32_t) cnt == 1 && data &&
            own_q(data, &lb) && b2_rtti_is(lb, B2_POPLIST_RTTI) && (sl = b2_sub_list()) != 0 && own_q(sl + 8, &cur))
            ((void (*)(uintptr_t, int, int, int)) LISTBOX_SELECTROW)(lb, (int) (int32_t) cur, 1, 1);
    }
    wref_set(&g_b2_popw[0], 0);
}
/* How far the list at x, w wide moves: by dx, but never left of the header's left edge nor past
 * the right of a screen sw wide. */
static int b2_pop_dx(int x, int w, int dx, int left, int sw) {
    if (x + dx + w > sw)
        dx = sw - w - x;
    if (x + dx < left)
        dx = left - x;
    return dx;
}
/* our button for column col on header h, 0 if none */
static uintptr_t b2_button_of(uintptr_t h, int col) {
    struct b2_hdr *e = b2_of(h);
    for (int i = 0; e && i < BF_NBTN; i++)
        if (bf_col(i) == col && e->btn[i] && bc_obj(e->btn[i], e->btn_vt[i]))
            return e->btn[i];
    return 0;
}
static void b2_lis_click(struct b2_lis *self, void *name) {
    if (!self->col) { /* EP147's own button: the pick that follows is EP147's, its list where EP147 put it */
        __atomic_store_n(&g_bf_ask, 0, __ATOMIC_RELEASE);
        b2_hide_row(wref_get(&g_b2_hidew[0]), 0); /* and its rows as EP147 made them */
        b2_pop_restore();
        return;
    }
    uintptr_t hdr = bc_obj(self->hdr, BC_HDR_VT), btn = 0, data = 0, n = 0, l = 0, lvt = 0, fn = 0;
    if (!hdr || !own_q(hdr + B2_HDR_BTN, &btn) || !btn || (btn & 7) || !own_q(btn + B2_BTN_LIS, &data) ||
        !own_q(btn + B2_BTN_LIS + 0x10, &n))
        return;
    /* the list's own listener on EP147's button: the same click opens EP147's field list. Our
     * buttons carry EP147's own name String (B2_BTN_NAME), which is what the listener checks. */
    for (int i = 0; i < (int) (n & 0xffffffff) && i < 16; i++) {
        if (!own_q(data + 8 * (uintptr_t) i, &l) || !l || (l & 7) || !own_q(l, &lvt) || lvt != B2_WLIS_VT ||
            !img_q(lvt + 0x10, &fn) || !fn)
            continue;
        __atomic_store_n(&g_bf_ask, self->col, __ATOMIC_RELEASE);
        g_bf_ask_ms = drag_now_ms();
        uintptr_t pc = b2_find_popup(hdr); /* the list EP147 keeps once built: its rows before it shows */
        b2_hide_row(pc, self->col);
        /* the list marks the field this column shows: EP147 builds it from its current index
         * during the call, so that index is this column's for the call only */
        int k = b2_list_index(bf_field_of(self->col));
        int ob[4], nb[4], mv = b2_bounds(btn, ob) && b2_bounds(b2_button_of(hdr, self->col), nb);
        ((void (*)(uintptr_t, void *)) fn)(l, name);
        if (!pc) { /* the first time: built by that call */
            pc = b2_find_popup(hdr);
            b2_hide_row(pc, self->col);
            bf_log("column %d: list built by the click: %s\n", self->col, pc ? "found" : "not found");
        }
        /* EP147 builds its list a moment later, under its own button: bf_tick moves it under ours */
        if (mv) {
            g_b2_pop.hdr = hdr;
            g_b2_pop.dx = nb[0] + nb[2] - (ob[0] + ob[2]);
            g_b2_pop.own_r = ob[0] + ob[2];
            g_b2_pop.row = k;
            g_b2_pop.until = drag_now_ms() + B2_POP_MS;
        }
        bf_log("column %d: list opened\n", self->col);
        return;
    }
}
static void (*g_o_b2_bdd_ctor)(long, long, long, long, long, long, long, long);
static void b2_bdd_ctor_hook(long t, long a, long b, long c, long d, long e, long f, long g) {
    g_o_b2_bdd_ctor(t, a, b, c, d, e, f, g);
    __atomic_store_n(&g_b2_bdd, (uintptr_t) t, __ATOMIC_RELEASE);
}
/* browse_columns.c's changeSubCateory hook asks first: a pick one of our buttons opened is that
 * column's, never EP147's. 1 = taken. (message thread) */
static int bf_take_pick(int idx) {
    int col = __atomic_exchange_n(&g_bf_ask, 0, __ATOMIC_ACQ_REL);
    if (!g_b2_on || !col || drag_now_ms() - g_bf_ask_ms > B2_ASK_MS)
        return 0;
    uintptr_t bdd = bc_obj(__atomic_load_n(&g_b2_bdd, __ATOMIC_ACQUIRE), B2_BDD_VT), sl = 0, data = 0, n = 0, s = 0;
    char tok[48] = "";
    if (!bdd || !own_q(bdd + 0x70, &sl) || !bc_obj(sl, B2_SCL_VT) || !own_q(sl + 0x18, &data) ||
        !own_q(sl + 0x28, &n) || idx < 0 || idx >= (int) (n & 0xffffffff) ||
        !own_q(data + 8 * (uintptr_t) idx, &s) || !s || !own_str(s, tok, sizeof tok)) {
        bf_log("pick %d: the field list is not readable, nothing changed\n", idx);
        return 1; /* the DJ asked for this column: never hand it to EP147's */
    }
    int f = b2_field_of_token(tok);
    if (f == B2_NONE && strcmp(tok, B2_FIELDS[B2_NONE].token)) {
        bf_log("pick %d: \"%s\" is not a field a column shows\n", idx, tok);
        return 1;
    }
    bf_set_field(col, f);
    if (b2_col_hidden(col)) /* a field picked for a hidden column: shown again */
        bc_set_mask(bc_mask() | b2_col_bit(col) | AZMOD_BCOL_SET(b2_col_bit(col)));
    return 1;
}
/* a second instance of EP147's sub column button, built as the header builds its own */
static uintptr_t b2_make_button(uintptr_t hdr) {
    uintptr_t res = ((uintptr_t (*)(void)) B2_BTN_RES)(), img = 0, e1 = 0, e2 = 0, d0 = 0;
    if (!img_q(B2_BTN_D0, &d0))
        return 0;
    mp_sret2(&img, B2_BTN_IMG, res, 0);
    ((void (*)(uintptr_t *)) B2_IMG_CTOR)(&e1);
    ((void (*)(uintptr_t *)) B2_IMG_CTOR)(&e2);
    uintptr_t btn = ((uintptr_t (*)(size_t)) B2_NEW)(B2_BTN_SIZE);
    ((void (*)(uintptr_t, uintptr_t, uintptr_t *, uintptr_t *, uintptr_t *, int, int, float, float)) B2_BTN_CTOR)(
        btn, B2_BTN_NAME, &img, &e1, &e2, 1, 1, 0.3f, 0.5f);
    ((void (*)(uintptr_t *)) B2_IMG_DTOR)(&e2);
    ((void (*)(uintptr_t *)) B2_IMG_DTOR)(&e1);
    ((void (*)(uintptr_t, int, int)) B2_SETSIZE)(btn, B2_BTN_W, B2_BTN_H);
    *(uint64_t *) (btn + B2_BTN_D0_OFF) = d0;
    ((void (*)(uintptr_t, uintptr_t, int)) B2_ADDCHILD)(hdr, btn, -1);
    ((void (*)(uintptr_t *)) B2_IMG_DTOR)(&img);
    return btn;
}
static void b2_hide_button(uintptr_t btn) {
    int b[4] = {0, 0, 0, 0}; /* no room: shown only on a column, in edit mode */
    ((void (*)(uintptr_t, const int *)) B2_SETBOUNDS)(btn, b);
}
static void (*g_o_b2_hdr_ctor)(long, int);
static void b2_patch_btn_mouse(uintptr_t vt);
static void b2_hdr_ctor_hook(long hdr, int mode) {
    g_o_b2_hdr_ctor(hdr, mode);
    uintptr_t h = (uintptr_t) hdr, own = 0;
    if (bc_obj(h, BC_HDR_VT))
        bc_hdr_built(h);
    if (!g_b2_ready || mode != 0 || !bc_obj(h, BC_HDR_VT) || !own_q(h + B2_HDR_BTN, &own) || !own)
        return; /* a compact pane, or no button of its own */
    int slot = -1;
    for (int i = 0; i < B2_HDR_MAX && slot < 0; i++)
        if (!g_b2h[i].hdr || !bc_obj(g_b2h[i].hdr, BC_HDR_VT))
            slot = i;
    if (slot < 0)
        return;
    struct b2_hdr e;
    memset(&e, 0, sizeof e);
    e.hdr = h;
    g_b2_lis[slot][0] = (struct b2_lis) {g_b2_lis_vt, 0, h};
    ((void (*)(uintptr_t, void *)) B2_ADDLIS)(h, &g_b2_lis[slot][0]);
    for (int i = 0; i < BF_NBTN; i++) {
        uintptr_t btn = b2_make_button(h), bvt = 0;
        if (!btn || !own_q(btn, &bvt) || !bvt)
            break;
        b2_hide_button(btn);
        /* a stand-in header for EP147's add-listener (it takes [x+0x138], mode 0 at +0x134) */
        uint64_t fake[0x148 / 8] = {0};
        fake[B2_HDR_BTN / 8] = btn;
        g_b2_lis[slot][i + 1] = (struct b2_lis) {g_b2_lis_vt, bf_col(i), h};
        ((void (*)(void *, void *)) B2_ADDLIS)(fake, &g_b2_lis[slot][i + 1]);
        e.btn[i] = btn;
        e.btn_vt[i] = bvt;
    }
    b2_patch_btn_mouse(e.btn_vt[0]);
    g_b2h[slot] = e;
    int nb = 0;
    while (nb < BF_NBTN && e.btn[nb])
        nb++;
    bf_log("header %lx: %d buttons\n", (unsigned long) h, nb);
}
/* ---- our buttons' mouse ---------------------------------------------------------------------
 * A button acts only on a whole tap, on the lift (ht_tap.h, the rule the Mod Menu and Hide Track
 * Title apply): a touch that moved past the slop, was held too long, or was a divider's drag opens
 * nothing. The header's own touches feed the same gesture (b2_gest), so a drag that starts on the
 * header and ends on a button is a move there too.
 * A button may sit over a divider's touch zone (b2_place), so a press on one of ours goes to
 * browse_edit.c first: when a divider is in reach, the press, its drag and its release are the
 * header's (be_btn_*). EP147's own mouse handling of the class is not called for ours: its click
 * would come on any release over the button. While editing, the header's own sub column button
 * (EP147's) is handled the same way, since it too reaches into the divider zone at its right; a
 * tap on it opens EP147's list as its click does. Outside edit mode, and on every other button of
 * the class, the button's own. The class's mouseDown / mouseDrag / mouseUp slots (juce::Component
 * vtable 5, 6, 7) are patched once, when the first of ours is built. (message thread) */
static int be_btn_press(uintptr_t h, int x); /* browse_edit.c */
static void be_btn_drag(uintptr_t h, int x);
static int be_btn_release(uintptr_t h);
static int be_editing(void);
static void be_open_fields(uintptr_t h);
static void (*g_o_b2_btn_mouse[3])(long, long);
static uintptr_t g_b2_btn_vt; /* the class whose mouse slots are patched */
static uintptr_t g_b2_fwd;    /* our button whose press a divider took */
static int g_b2_btn_ok;       /* all three slots patched: presses may be the header's */
static struct ht_gest g_b2_gest; /* the touch on a header or one of our buttons */
/* One mouse event at header (x, y): ev 0 press, 1 drag, 2 release. A drag is a move of the touch
 * that is down, never a new one, so it is fed at the touch's own time. 1 when a release completes
 * a tap. */
static int b2_gest(int ev, int x, int y) {
    int tx, ty;
    long ms = ev == 1 && g_b2_gest.down ? g_b2_gest.seq.last : now_ms();
    return ht_gest_feed(&g_b2_gest, ev != 2, 1, x, y, ms, &tx, &ty);
}
/* the header of our button btn (or, while editing, of EP147's own sub column button) and the slot
 * of its listener; 0 for any other button */
static uintptr_t b2_hdr_of_btn(uintptr_t btn, struct b2_lis **lis) {
    for (int k = 0; k < B2_HDR_MAX; k++) {
        if (!g_b2h[k].hdr || !bc_obj(g_b2h[k].hdr, BC_HDR_VT))
            continue;
        for (int i = 0; i < BF_NBTN; i++)
            if (g_b2h[k].btn[i] == btn) {
                *lis = &g_b2_lis[k][i + 1];
                return g_b2h[k].hdr;
            }
        uintptr_t own = 0;
        if (be_editing() && own_q(g_b2h[k].hdr + B2_HDR_BTN, &own) && own == btn) {
            *lis = &g_b2_lis[k][0];
            return g_b2h[k].hdr;
        }
    }
    return 0;
}
static int b2_me_x(long e) { /* juce::MouseEvent x, in the event component's coordinates */
    uintptr_t q = 0;
    return own_q((uintptr_t) e + 8, &q) ? (int) (int32_t) q : -1;
}
/* the event at header coordinates for our button btn; 0 if unknown. A drag that leaves the
 * button has coordinates outside it, negative ones included. */
static int b2_hdr_xy(uintptr_t btn, long e, int *x, int *y) {
    int b[4];
    uintptr_t q = 0;
    if (!own_q((uintptr_t) e + 8, &q) || !b2_bounds(btn, b))
        return 0;
    *x = b[0] + (int) (int32_t) q;
    *y = b[1] + (int) (int32_t) (q >> 32);
    return 1;
}
static void b2_btn_down_hook(long btn, long e) {
    struct b2_lis *lis = 0;
    uintptr_t h = g_b2_btn_ok ? b2_hdr_of_btn((uintptr_t) btn, &lis) : 0;
    int x = -1, y = -1;
    if (!h || !b2_hdr_xy((uintptr_t) btn, e, &x, &y)) {
        g_o_b2_btn_mouse[0](btn, e);
        return;
    }
    b2_gest(0, x, y);
    g_b2_fwd = be_btn_press(h, x) ? (uintptr_t) btn : 0;
}
static void b2_btn_drag_hook(long btn, long e) {
    struct b2_lis *lis = 0;
    uintptr_t h = g_b2_btn_ok ? b2_hdr_of_btn((uintptr_t) btn, &lis) : 0;
    int x = -1, y = -1;
    if (!h || !b2_hdr_xy((uintptr_t) btn, e, &x, &y)) {
        g_o_b2_btn_mouse[1](btn, e);
        return;
    }
    b2_gest(1, x, y);
    if (g_b2_fwd == (uintptr_t) btn)
        be_btn_drag(h, x);
}
static void b2_btn_up_hook(long btn, long e) {
    struct b2_lis *lis = 0;
    uintptr_t h = g_b2_btn_ok ? b2_hdr_of_btn((uintptr_t) btn, &lis) : 0;
    int x = -1, y = -1;
    if (!h || !lis || !b2_hdr_xy((uintptr_t) btn, e, &x, &y)) {
        if (g_b2_fwd == (uintptr_t) btn) { /* a drag this button started ends here too */
            if (h)
                be_btn_release(h);
            g_b2_fwd = 0;
            __atomic_store_n(&g_b2_layout, 1, __ATOMIC_RELEASE);
        }
        g_o_b2_btn_mouse[2](btn, e);
        return;
    }
    int tap = b2_gest(2, x, y), dragged = 0;
    if (g_b2_fwd == (uintptr_t) btn) {
        dragged = be_btn_release(h);
        __atomic_store_n(&g_b2_layout, 1, __ATOMIC_RELEASE); /* the button back on its column */
    }
    g_b2_fwd = 0;
    /* juce::Button takes its pressed look from the mouse, not from its mouseDown: the class's
     * mouseExit (slot 4) puts it back to normal without a click */
    uintptr_t vt = 0, fn = 0;
    if (own_q((uintptr_t) btn, &vt) && vt == g_b2_btn_vt && img_q(vt + 4 * 8, &fn) && fn)
        ((void (*)(long, long)) fn)(btn, e);
    if (!tap || dragged)
        return;
    b2_lis_click(lis, (void *) B2_BTN_NAME);
    if (!lis->col)
        be_open_fields(h); /* EP147's own button: its click, which opens EP147's list */
}
static void b2_patch_btn_mouse(uintptr_t vt) {
    static void *const hook[3] = {(void *) b2_btn_down_hook, (void *) b2_btn_drag_hook, (void *) b2_btn_up_hook};
    if (g_b2_btn_vt || !vt)
        return;
    g_b2_btn_vt = vt;
    int ok = 1;
    for (int k = 0; k < 3 && ok; k++)
        ok = azm_patch_vtable(vt + (uintptr_t) (5 + k) * 8, hook[k], (void **) &g_o_b2_btn_mouse[k]) == 1;
    g_b2_btn_ok = ok;
    bf_log("button mouse slots on %lx: %s\n", (unsigned long) vt, ok ? "patched" : "FAILED");
}
/* the header goes: our buttons first (Component's destructor takes them off the header) */
static void (*g_o_b2_hdr_dtor[2])(long);
static void b2_hdr_gone(long hdr) {
    bc_hdr_gone((uintptr_t) hdr);
    for (int i = 0; i < B2_HDR_MAX; i++) {
        struct b2_hdr *e = &g_b2h[i];
        if (e->hdr != (uintptr_t) hdr)
            continue;
        for (int k = 0; k < BF_NBTN; k++) {
            uintptr_t vt = 0, del = 0;
            if (e->btn[k] && own_q(e->btn[k], &vt) && vt == e->btn_vt[k] && own_q(vt + 8, &del) && del)
                ((void (*)(uintptr_t)) del)(e->btn[k]);
        }
        memset(e, 0, sizeof *e);
    }
}
static void b2_hdr_dtor0(long hdr) {
    b2_hdr_gone(hdr);
    g_o_b2_hdr_dtor[0](hdr);
}
static void b2_hdr_dtor1(long hdr) {
    b2_hdr_gone(hdr);
    g_o_b2_hdr_dtor[1](hdr);
}
static struct b2_hdr *b2_of(uintptr_t h) {
    for (int i = 0; i < B2_HDR_MAX; i++)
        if (g_b2h[i].hdr == h && h)
            return &g_b2h[i];
    return 0;
}
/* w's list has one of the full-width headers that got buttons: such a header's parent
 * (juce::Component +0x18) is the list's ListBox (TrackListWidget +0x48). (message thread) */
static int b2_full_list(uintptr_t w) {
    uintptr_t lb = 0, par = 0;
    if (!own_q(w + 0x48, &lb) || !lb)
        return 0;
    for (int i = 0; i < B2_HDR_MAX; i++)
        if (g_b2h[i].hdr && own_q(g_b2h[i].hdr + 0x18, &par) && par == lb)
            return 1;
    return 0;
}
/* Preview (column 1) is drawn by a gui::PreviewableWaveform component of the row, over the
 * cell. When the column shows a field, its paint draws the field instead, with EP147's sub column
 * painter into the component's Graphics (the component covers the cell exactly: same origin,
 * width and height). The component stays EP147's; only its paint is replaced. (message thread)
 *   PreviewableWaveform (preview_hotcues.c): +0x28 width, +0x2c height, +0xc8 the owning list's
 *   margins (TrackListWidget + 0x280), +0xf8 the row. */
/* A touch on Preview while it shows a field is a touch on the row, as on a cell without a
 * component: the component's own mouseDown / mouseDrag / mouseUp (juce::Component vtable 5, 6, 7),
 * which play EP147's preview from the waveform, are skipped, and the row (its parent,
 * meow::TouchableTableListBox::RowComp, vtable-checked) gets the event as its own: a copy with the
 * row as the event and original component, moved to the row's coordinates just right of Preview.
 *   juce::MouseEvent: +0x00 position (two floats), +0x08 x, +0x0c y, +0x28 eventComponent,
 *   +0x30 originalComponent, +0x50 mouseDownPosition (two floats); B2_ME_SIZE covers it.
 * (message thread) */
#define B2_ME_SIZE 0x80
static void (*g_o_b2_wave_mouse[3])(long, long);
static int g_b2_wave_fwd; /* telemetry: presses given to the row */
static uintptr_t b2_wave_row(long self) {
    uintptr_t c = (uintptr_t) self, mp = 0, par = 0, pvt = 0;
    if (bf_field_of(1) == B2_NONE || !own_q(c + 0xc8, &mp) || !mp || (mp & 3))
        return 0;
    uintptr_t w = mp - PH_OWNER_OFF;
    if ((w & 7) || !bc_obj(w, TRACKLIST_WIDGET_VT) || !b2_full_list(w) || !own_q(c + 0x18, &par) || !par ||
        (par & 7) || !own_q(par, &pvt) || pvt != ROWCOMP_VT)
        return 0;
    return par;
}
/* e as the row sees it: copied into ev, the row the event component, positions moved by (dx, dy) */
static int b2_event_for(long e, uintptr_t row, int dx, int dy, uint64_t ev[B2_ME_SIZE / 8]) {
    for (int i = 0; i < B2_ME_SIZE / 8; i++)
        if (!own_q((uintptr_t) e + 8 * (uintptr_t) i, (uintptr_t *) &ev[i]))
            return 0;
    unsigned char *b = (unsigned char *) ev;
    float f[2];
    int xy[2];
    memcpy(f, b, sizeof f), f[0] += (float) dx, f[1] += (float) dy, memcpy(b, f, sizeof f);
    memcpy(xy, b + 8, sizeof xy), xy[0] += dx, xy[1] += dy, memcpy(b + 8, xy, sizeof xy);
    memcpy(f, b + 0x50, sizeof f), f[0] += (float) dx, f[1] += (float) dy, memcpy(b + 0x50, f, sizeof f);
    memcpy(b + 0x28, &row, sizeof row);
    memcpy(b + 0x30, &row, sizeof row);
    return 1;
}
static void b2_wave_mouse(int k, long self, long e) {
    uintptr_t row = b2_wave_row(self), fn = 0;
    uint64_t ev[B2_ME_SIZE / 8];
    int cb[4];
    if (!row) {
        g_o_b2_wave_mouse[k](self, e);
        return;
    }
    /* the field is shown: never the preview, the row's touch when the event can be made */
    /* moved right past Preview's own column too: the row's tap goes to the model's cellClicked for
     * the column under it, and EP147 answers one on Preview only through the waveform */
    if (!img_q(ROWCOMP_VT + (uintptr_t) (5 + k) * 8, &fn) || !fn || !b2_bounds((uintptr_t) self, cb) ||
        !b2_event_for(e, row, cb[0] + cb[2] - b2_me_x(e) + 1, cb[1], ev))
        return;
    if (k == 0)
        bf_log("preview touch: the row's (%d)\n", ++g_b2_wave_fwd);
    ((void (*)(long, void *)) fn)((long) row, ev);
}
static void b2_wave_down(long self, long e) { b2_wave_mouse(0, self, e); }
static void b2_wave_drag(long self, long e) { b2_wave_mouse(1, self, e); }
static void b2_wave_up(long self, long e) { b2_wave_mouse(2, self, e); }
/* the row paintCell painted last, and whether it was selected: g_b2_sel_model, g_b2_sel_row,
 * g_b2_sel (declared above bf_paint) */
static void (*g_o_b2_wave_paint)(void *, void *);
static void b2_wave_paint_hook(void *self, void *g) {
    uintptr_t c = (uintptr_t) self, mp = 0, v = 0, wh = 0;
    if (bf_field_of(1) == B2_NONE || !on_msg_thread() || !own_q(c + 0xc8, &mp) || !mp || (mp & 3) ||
        !own_q(c + 0x28, &wh) || !own_q(c + 0xf8, &v)) {
        g_o_b2_wave_paint(self, g);
        return;
    }
    uintptr_t w = mp - PH_OWNER_OFF;
    int row = (int) (v & 0xffffffff), cw = (int) (wh & 0xffffffff), ch = (int) (wh >> 32);
    if ((w & 7) || !bc_obj(w, TRACKLIST_WIDGET_VT) || drag_is_widget(w) || !b2_full_list(w) || cw <= 0 || ch <= 0) {
        g_o_b2_wave_paint(self, g);
        return;
    }
    /* RowComp paints its cells before its child components: the row's other cells just went */
    int sel = g_b2_sel_model == w + 0x30 && g_b2_sel_row == row ? g_b2_sel : 0;
    bf_paint((void *) (w + 0x30), g, row, 1, cw, ch, sel);
}
/* ---- the title column ----------------------------------------------------------------------
 * The title is a component (gui::IconHoldableScrollLabel, built by refreshComponentForCell), so
 * JUCE never asks paintCell for it. Each one built for a full-width list is remembered here with
 * its list and row. The label paints itself (its icon) as always; while the title shows a field,
 * its text, a gui::ScrollLabel child painted right after it, is the field instead, drawn by
 * EP147's sub column painter in the text's place. Touches stay the label's. (message thread) */
static struct {
    uintptr_t comp, w;
    int row;
} g_b2_tl[B2_TL];
static _Atomic uintptr_t g_b2_tl_live[B2_TL]; /* the lists: wref slots, cleared by their destructor */
static int g_b2_tl_ok;
static struct {
    uintptr_t label, w; /* the label painted last, while its title shows a field: its list */
    int row;
} g_b2_tl_sub;
static void *(*g_o_b2_refresh)(void *, int, int, int, void *);
static void *b2_refresh_hook(void *model, int row, int col, int sel, void *existing) {
    void *c = g_o_b2_refresh(model, row, col, sel, existing);
    uintptr_t w = (uintptr_t) model - 0x30;
    if (col != bf_col(5) || !c || !g_b2_tl_ok || !bc_obj(w, TRACKLIST_WIDGET_VT))
        return c;
    int slot = -1;
    for (int i = 0; i < B2_TL && slot < 0; i++)
        if (g_b2_tl[i].comp == (uintptr_t) c)
            slot = i;
    for (int i = 0; i < B2_TL && slot < 0; i++)
        if (!g_b2_tl[i].comp || !wref_get(&g_b2_tl_live[i]))
            slot = i;
    if (slot < 0) {
        static unsigned rr;
        slot = (int) (rr++ % B2_TL);
    }
    g_b2_tl[slot].comp = (uintptr_t) c;
    g_b2_tl[slot].w = w;
    g_b2_tl[slot].row = drag_is_widget(w) ? -1 : row; /* a drag remaps rows: its own text */
    wref_set(&g_b2_tl_live[slot], w);
    return c;
}
/* the list and row of title label c while the title shows a field there, 0 otherwise */
static uintptr_t b2_title_of(uintptr_t c, int *row) {
    if (!g_b2_on || !c || bf_field_of(bf_col(5)) == B2_NONE || !on_msg_thread())
        return 0;
    for (int i = 0; i < B2_TL; i++)
        if (g_b2_tl[i].comp == c) {
            uintptr_t w = g_b2_tl[i].w;
            if (wref_get(&g_b2_tl_live[i]) != w || g_b2_tl[i].row < 0 || !bc_obj(w, TRACKLIST_WIDGET_VT) ||
                drag_is_widget(w) || !b2_full_list(w))
                return 0;
            *row = g_b2_tl[i].row;
            return w;
        }
    return 0;
}
static void (*g_o_b2_title_paint)(void *, void *);
static void b2_title_paint_hook(void *self, void *g) {
    int row = -1;
    uintptr_t w = b2_title_of((uintptr_t) self, &row);
    g_b2_tl_sub.label = w ? (uintptr_t) self : 0;
    g_b2_tl_sub.w = w;
    g_b2_tl_sub.row = row;
    g_o_b2_title_paint(self, g);
}
static void (*g_o_b2_title_text)(void *, void *);
static void b2_title_text_hook(void *self, void *g) {
    uintptr_t par = 0, wh = 0, w = g_b2_tl_sub.w;
    int row = g_b2_tl_sub.row;
    if (!g_b2_tl_sub.label || !on_msg_thread() || !own_q((uintptr_t) self + 0x18, &par) ||
        par != g_b2_tl_sub.label || !own_q((uintptr_t) self + 0x28, &wh) || (int) (wh & 0xffffffff) <= 0 ||
        (int) (wh >> 32) <= 0) {
        g_o_b2_title_text(self, g);
        return;
    }
    int sel = g_b2_sel_model == w + 0x30 && g_b2_sel_row == row ? g_b2_sel : 0;
    /* the painter ends a cell with its divider: past the text's right edge, where the title has none */
    bf_paint((void *) (w + 0x30), g, row, bf_col(5), (int) (wh & 0xffffffff) + B2_TL_PAST, (int) (wh >> 32), sel);
}

/* the rectangle of column `id` in the header (0 when it is not shown) */
static int b2_colrect(uintptr_t h, int id, int r[4]) {
    int idx = ((int (*)(uintptr_t, int, int)) B2_COLINDEX)(h, id, 1);
    uintptr_t out[2] = {0, 0};
    if (idx < 0)
        return 0;
    mp_sret2(out, B2_COLPOS, h, (uintptr_t) idx);
    memcpy(r, out, 4 * sizeof(int));
    return 1;
}
/* The field buttons' x, for n columns at x[i], w[i] wide (want[i] 0: no button there). Each sits
 * at its column's right: zone px clear of the divider when the column has room for that, else
 * flush with the divider (a press near the divider is still the divider's: b2_btn_down_hook).
 * Placed right to left, a button never covers the one to its right, nor goes past the header's
 * left edge. bx[i] is -1 where there is no button. */
static void b2_place(const int *x, const int *w, const int *want, int n, int zone, int *bx) {
    int done[BF_NBTN] = {0}, limit = 1 << 30;
    for (int k = 0; k < n && k < BF_NBTN; k++) {
        int r = -1; /* the rightmost column not placed yet */
        for (int i = 0; i < n && i < BF_NBTN; i++)
            if (!done[i] && (r < 0 || x[i] > x[r]))
                r = i;
        done[r] = 1;
        bx[r] = -1;
        if (!want[r] || w[r] <= 0)
            continue;
        int b = w[r] - 2 * zone >= B2_BTN_W ? x[r] + w[r] - zone - B2_BTN_W : x[r] + w[r] - B2_BTN_W;
        if (b + B2_BTN_W > limit)
            b = limit - B2_BTN_W;
        if (b < 0)
            b = 0;
        bx[r] = b;
        limit = b;
    }
}
/* browse_edit.c, after a full-width header's layout: in edit mode a button on each column that
 * can show a field, otherwise none */
static void bf_after_layout(uintptr_t h, int editing, int zone) {
    struct b2_hdr *e = g_b2_on ? b2_of(h) : 0;
    if (!e)
        return;
    int x[BF_NBTN], w[BF_NBTN], want[BF_NBTN], bx[BF_NBTN];
    for (int i = 0; i < BF_NBTN; i++) {
        int r[4] = {0, 0, 0, 0};
        want[i] = editing && e->btn[i] && bc_obj(e->btn[i], e->btn_vt[i]) && b2_colrect(h, bf_col(i), r);
        x[i] = r[0];
        w[i] = r[2];
        /* the title: EP147's own sub column button sits at its right while the sub column is
         * hidden; ours goes left of it */
        int ob[4];
        uintptr_t own = 0;
        if (i == 5 && want[i] && own_q(h + B2_HDR_BTN, &own) && b2_bounds(own, ob) && ob[0] > x[i] &&
            ob[0] < x[i] + w[i])
            w[i] = ob[0] - x[i];
    }
    b2_place(x, w, want, BF_NBTN, zone, bx);
    for (int i = 0; i < BF_NBTN; i++) {
        if (!e->btn[i] || !bc_obj(e->btn[i], e->btn_vt[i]))
            continue;
        /* the button a drag started on stays where the press found it until the release (a hidden
         * one would never get its mouseUp); the release lays the header out again */
        if (e->btn[i] == g_b2_fwd) {
            if (bx[i] < 0)
                bf_log("pressed button %d kept while its column is too narrow\n", i);
            continue;
        }
        if (bx[i] >= 0) {
            int b[4] = {bx[i], 0, B2_BTN_W, B2_BTN_H};
            ((void (*)(uintptr_t, const int *)) B2_SETBOUNDS)(e->btn[i], b);
        } else {
            b2_hide_button(e->btn[i]);
        }
    }
}

/* ---- message thread ---------------------------------------------------------------------- */
static void bf_tick(void) {
    if (!g_b2_on)
        return;
    if (g_b2_pop.hdr) {
        /* EP147's list: a full-screen SubCategorySelectPopupWidget holding the list (its child,
         * a meow::TouchableTableListBox), right-aligned under EP147's own button */
        uintptr_t h = bc_obj(g_b2_pop.hdr, BC_HDR_VT), pop = h ? b2_find_popup(h) : 0, data = 0, cnt = 0, lb = 0;
        int lbb[4];
        int pb[4];
        if (pop && wref_get(&g_b2_popw[0]) == pop)
            memcpy(pb, g_b2_pop_orig, sizeof pb); /* moved before: from EP147's own place */
        else if (g_b2_popw_ok && pop && b2_bounds(pop, pb) && wref_set(&g_b2_popw[0], pop))
            memcpy(g_b2_pop_orig, pb, sizeof pb);
        else
            pop = 0; /* not trackable: left where EP147 put it */
        if (pop && g_b2_pop.own_r && own_q(pop + 0x40, &data) && own_q(pop + 0x50, &cnt) &&
            (int) (int32_t) cnt == 1 && data && own_q(data, &lb) && b2_rtti_is(lb, B2_POPLIST_RTTI) &&
            b2_bounds(lb, lbb)) {
            /* the popup paints the list's frame itself, so the popup moves, list and all; moved
             * left it grows by as much, so it still covers the screen (a touch outside the list
             * closes it) */
            int left = lbb[0] + lbb[2] - g_b2_pop.own_r; /* the header's left edge, in the popup */
            int d = b2_pop_dx(lbb[0], lbb[2], g_b2_pop.dx, left, pb[2]);
            int nb[4] = {pb[0] + d, pb[1], d < 0 ? pb[2] - d : pb[2], pb[3]};
            ((void (*)(uintptr_t, const int *)) B2_SETBOUNDS)(pop, nb);
            int col = __atomic_load_n(&g_bf_ask, __ATOMIC_ACQUIRE);
            b2_hide_row(pop, col);
            if (b2_col_hidden(col) && b2_hide_present(b2_pop_obj(pop)))
                g_b2_pop.row = g_b2_hide.idx; /* a hidden column: its Hide Column row is marked */
            /* the list marks its selected row: EP147 selected the sub column's own; this column's
             * field instead (selectRow(row, dontScroll, deselectOthers), as EP147 selects rows) */
            if (g_b2_pop.row >= 0)
                ((void (*)(uintptr_t, int, int, int)) LISTBOX_SELECTROW)(lb, g_b2_pop.row, 1, 1);
            bf_log("field list moved by %d, row %d\n", d, g_b2_pop.row);
        }
        if (pop || !h || drag_now_ms() > g_b2_pop.until)
            g_b2_pop.hdr = 0;
    }
    if (__atomic_exchange_n(&g_b2_layout, 0, __ATOMIC_ACQ_REL))
        for (int i = 0; i < BC_HDR_MAX; i++) {
            uintptr_t h = bc_full_hdr(g_bc_hdr[i]);
            if (h)
                bc_layout_hook((long) h, g_bc_rowh[i]);
        }
    if (__atomic_exchange_n(&g_b2_refresh, 0, __ATOMIC_ACQ_REL))
        for (int i = 0; i < B2_HDR_MAX; i++) {
            /* a header's parent is its list's ListBox: its rows and their components repaint */
            uintptr_t lb = 0, lbvt = 0;
            if (g_b2h[i].hdr && bc_obj(g_b2h[i].hdr, BC_HDR_VT) && own_q(g_b2h[i].hdr + 0x18, &lb) && lb &&
                !(lb & 7) && own_q(lb, &lbvt) && lbvt)
                ((void (*)(uintptr_t)) RC_REPAINT)(lb);
        }
    if (__atomic_exchange_n(&g_b2_landed, 0, __ATOMIC_ACQ_REL))
        for (int i = 0; i < B2_LISTS; i++) {
            /* a live list (its wref slot still holds it), its ListBox at +0x48: the rows re-paint */
            uintptr_t w = wref_get(&g_b2_lists[i]), lb = 0, lbvt = 0;
            if (w && bc_obj(w, TRACKLIST_WIDGET_VT) && own_q(w + 0x48, &lb) && lb && !(lb & 7) && own_q(lb, &lbvt) &&
                lbvt)
                ((void (*)(uintptr_t)) RC_REPAINT)(lb);
        }
}
static void install_browse_fields(void) {
    /* Column Edit names the columns and shows the buttons: without it, no fields */
    int edit = access("/home/root/settings/azmod-coledit.off", F_OK) != 0;
    if (access(BF_OFF, F_OK) == 0 || !edit || !g_sm_on || !g_o_bc_layout) {
        bf_log("fields: off (kill switch %d, column edit %d, stem marks %d, layout %d)\n", access(BF_OFF, F_OK) == 0,
               edit, g_sm_on, g_o_bc_layout != 0);
        return;
    }
    bf_load();
    g_b2_lists_ok = wref_track(g_b2_lists, B2_LISTS, 0) == 1;
    g_b2_popw_ok = wref_track(g_b2_popw, 1, 0) == 1;
    int hidew = wref_track(g_b2_hidew, 1, 0) == 1;
    int a = azm_install_hook(B2_HDR_CTOR, (void *) b2_hdr_ctor_hook, (void **) &g_o_b2_hdr_ctor);
    int d0 = a == 1 && azm_patch_vtable(BC_HDR_VT, (void *) b2_hdr_dtor0, (void **) &g_o_b2_hdr_dtor[0]) == 1;
    int d1 = d0 && azm_patch_vtable(BC_HDR_VT + 8, (void *) b2_hdr_dtor1, (void **) &g_o_b2_hdr_dtor[1]) == 1;
    /* browse_columns.c's record of EP147's column visibility: the lifetimes are followed */
    __atomic_store_n(&g_bc_ep_ok, d1, __ATOMIC_RELEASE);
    int b = d1 && azm_install_hook(B2_BDD_CTOR, (void *) b2_bdd_ctor_hook, (void **) &g_o_b2_bdd_ctor) == 1;
    g_b2_ready = b; /* the buttons only with their destructor and the pick in */
    int p = b && install_paintcell_slot() == 1;
    int r = p && azm_patch_vtable(PH_PAINT_SLOT, (void *) b2_wave_paint_hook, (void **) &g_o_b2_wave_paint) == 1;
    static void *const wm[3] = {(void *) b2_wave_down, (void *) b2_wave_drag, (void *) b2_wave_up};
    for (int k = 0; k < 3 && r; k++) /* without all three, no fields at all */
        r = azm_patch_vtable(PH_VT + (uintptr_t) (5 + k) * 8, wm[k], (void **) &g_o_b2_wave_mouse[k]) == 1;
    /* the title: after the label paint slot's own users (install_hlabel_paint_slot runs once, and
     * refuses a slot that is no longer EP147's) */
    install_hlabel_paint_slot();
    g_b2_tl_ok = wref_track(g_b2_tl_live, B2_TL, 0) == 1;
    int tl = r && g_b2_tl_ok &&
             azm_patch_vtable(REFRESHCELL_SLOT, (void *) b2_refresh_hook, (void **) &g_o_b2_refresh) == 1 &&
             azm_patch_vtable(HLABEL_PAINT_SLOT, (void *) b2_title_paint_hook, (void **) &g_o_b2_title_paint) == 1 &&
             azm_patch_vtable(SLTEXT_PAINT_SLOT, (void *) b2_title_text_hook, (void **) &g_o_b2_title_text) == 1;
    r = r && tl; /* the title's button only with its paint */
    /* the Hide Column row only with its tap */
    int hc = r && hidew && B2_POP_ADD && B2_POP_UPDATE &&
             azm_patch_vtable(B2_POP_MODEL_VT + B2_POP_CLICK, (void *) b2_pop_click, (void **) &g_o_b2_pop_click) == 1;
    g_b2_hide_ok = hc;
    pthread_t t;
    g_b2_on = r && az_thread_create(&t, NULL, b2_worker, NULL) == 0;
    if (g_b2_on) {
        pthread_detach(t);
        g_bc_take_pick = bf_take_pick;
        g_tlr_field_paint = bf_paint;
    }
    bf_log("fields: header=%d dtors=%d,%d lists=%d paint=%d preview=%d title=%d hide=%d -> %s\n", a, d0, d1, b, p, r,
           tl, hc, g_b2_on ? "ON" : "OFF");
}
