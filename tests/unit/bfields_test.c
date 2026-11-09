/* SPDX-License-Identifier: GPL-3.0-only */
/* Column Edit, any field (guest/azmod/browse_fields.c, browse_fields.h), against models of
 * EP147's track list header, its sub column button, the list's button listener, the sub column
 * list, the row ring and its cell painter, the Preview component, and a fake SQLite over fake
 * sticks:
 *   - the fields: each list token maps to its field (DEFAULT and anything else to none); each
 *     value reads as EP147 shows it (stars, mm:ss, kbps / VBR, a count, a color's dot + name,
 *     text cleaned to valid UTF-8 without control characters, cut at a character boundary);
 *   - the saved file: valid files parse, a damaged one sets nothing, the format reads back;
 *   - the columns that can show a field: Preview 1, artwork, #, BPM and Key on both builds,
 *     never the title or EP147's sub column; a saved id that cannot show one is ignored;
 *   - a pick: one of our buttons opens EP147's list through the list's own listener, with the
 *     list's current index the column's field for that call only; the pick that follows is that
 *     column's; EP147's own button's click makes it EP147's again; a stale pick is EP147's;
 *     Track picked gives the column its own data back;
 *   - the buttons after a layout: on the column's right, clear of the divider zones where the
 *     column has room, else flush with the divider; placed right to left so none covers the
 *     next, never past the header's left edge; hidden outside edit mode;
 *   - a press on one of our buttons within a divider's reach is the header's (browse_edit.c
 *     be_btn_*, stubbed here), its still release the button's click; any other press, and every
 *     press on EP147's own button, is the button's;
 *   - the field list: moved under the column's button from EP147's own place, kept within the
 *     header's left edge and the screen, the column's field selected; EP147's own button puts
 *     the place and EP147's row back;
 *   - a cell: EP147's painter draws the sub column with the row's sub item swapped for this
 *     column's value, and the row's own sub item is back afterwards; a compact list is EP147's;
 *     column 1 does not record the selected row; the Preview component draws the field instead
 *     of its waveform, and its own paint when the column shows no field; a touch on it showing
 *     a field is the row's (a copy of the event, moved just right of Preview), never the preview;
 *   - the worker (its own thread): a value from one stick, the same from two, unknown when two
 *     disagree; a stick whose library EP147 lets go of is let go of too;
 *   - with the Mod Menu's Column Edit row off, every column shows its own data;
 *   - Hide Column: the field list of a column that can be hidden (artwork, #, BPM, Key) gets one
 *     more row, added once through the popup's own add at the click (before EP147 shows the list),
 *     and loses it when Preview's, the title's or EP147's own button opens the list; a tap on it closes the popup with no row passed on
 *     and hides the column (browse_columns.c bc_set_mask, stubbed here); a tap on any other row,
 *     or on that index after EP147 rebuilt its rows, is EP147's; a hidden column's list marks
 *     the row; a field picked for a hidden column shows it again; no row without the tap hook;
 *   - a header built or destroyed is told to browse_columns.c (its visibility record).
 * BF_SRC = browse_fields.c under test (browse_fields.h beside it); BCID_INC = browse_columns.c's
 * bc_col_id; BCOL_INC = browse_columns.c's AZMOD_BCOL_* definitions; -I guest/azmod for libfd.h. */
#define _GNU_SOURCE
#include "check.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef SCHED_IDLE
#define SCHED_IDLE SCHED_OTHER
#endif
#ifdef __APPLE__
/* No /proc here: the file's count of open fds on a library reads /dev/fd, each fd's path from
 * F_GETPATH (what /proc/self/fd/N's link gives on Linux). */
static DIR *t_opendir(const char *p) { return opendir(strcmp(p, "/proc/self/fd") ? p : "/dev/fd"); }
static ssize_t t_readlink(const char *p, char *buf, size_t n) {
    static const char pre[] = "/proc/self/fd/";
    char tmp[PATH_MAX];
    if (strncmp(p, pre, sizeof pre - 1))
        return readlink(p, buf, n);
    if (fcntl(atoi(p + sizeof pre - 1), F_GETPATH, tmp) == -1)
        return -1;
    size_t k = strlen(tmp);
    if (k > n)
        k = n;
    memcpy(buf, tmp, k);
    return (ssize_t) k;
}
#define opendir t_opendir
#define readlink t_readlink
#endif

/* ---- the azmod pieces browse_fields.c uses ----------------------------------------------- */
#include BCOL_INC
static int g_v200 = 1;
static int ep_off(int v130, int v200) { return g_v200 ? v200 : v130; }
#include BCID_INC
static FILE *azlog_open(const char *p, const char *m) {
    (void) p;
    (void) m;
    return 0;
}
static int g_writes;
static int settings_write(const char *path, const char *buf, size_t len, int durable) {
    (void) durable;
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fwrite(buf, 1, len, f);
    fclose(f);
    g_writes++;
    return 0;
}
static int own_q(uintptr_t a, uintptr_t *out) {
    if (!a || (a & 7))
        return 0;
    memcpy(out, (void *) a, sizeof *out);
    return 1;
}
static int img_q(uintptr_t a, uintptr_t *out) { return own_q(a, out); }
static int own_str(uintptr_t p, char *out, int n) {
    if (!p || n <= 0)
        return 0;
    snprintf(out, (size_t) n, "%s", (const char *) p);
    return 1;
}
static uintptr_t bc_obj(uintptr_t p, uintptr_t vt) {
    uintptr_t v = 0;
    return p && !(p & 7) && own_q(p, &v) && v == vt ? p : 0;
}
static long long g_now = 100000; /* __atomic only: the worker reads it */
static long long drag_now_ms(void) { return __atomic_load_n(&g_now, __ATOMIC_ACQUIRE); }
static long now_ms(void) { return (long) drag_now_ms(); }
static void advance(long long ms) { __atomic_add_fetch(&g_now, ms, __ATOMIC_ACQ_REL); }
static int drag_is_widget(uintptr_t w) {
    (void) w;
    return 0;
}
static int on_msg_thread(void) { return 1; }
#define SM_HANDSOFF_MS 10000
#define SQLITE_ROW 100
#define REC_KIND_TRACK 4
#define PH_OWNER_OFF 0x280
static const char ONELIBRARY_KEY_SM[] = "key";
static unsigned sm_hash(const char *t) {
    unsigned h = 2166136261u;
    for (; *t; t++)
        h = (h ^ (unsigned char) *t) * 16777619u;
    return h;
}
static long long sm_file_sig(const char *p) {
    struct stat st;
    if (stat(p, &st))
        return 0;
    return (long long) st.st_mtime * 1000003LL + (long long) st.st_size;
}
static int sm_is_mounted(const char *m) {
    (void) m;
    return 1;
}
/* the list's reply page: title "T<n>" is track 100+n */
static int sm_lookup(int row, const char *title, unsigned *kind, unsigned *id) {
    (void) row;
    if (title[0] != 'T')
        return 0;
    *kind = REC_KIND_TRACK;
    *id = 100 + (unsigned) atoi(title + 1);
    return 1;
}
static int g_sm_on = 1;
/* install's worker: a thread that returns at once (the test runs the real worker itself) */
static void *noop_thread(void *a) { return a; }
static int az_thread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg) {
    (void) fn;
    return pthread_create(t, a, noop_thread, arg);
}
static int wref_track(_Atomic uintptr_t *base, int n, int off) {
    (void) base;
    (void) n;
    (void) off;
    return 1;
}
static int wref_set(_Atomic uintptr_t *slot, uintptr_t p) {
    atomic_store_explicit(slot, p, memory_order_release);
    return 1;
}
static uintptr_t wref_get(_Atomic uintptr_t *slot) { return atomic_load_explicit(slot, memory_order_acquire); }
static int g_repaints;
static void fake_repaint(uintptr_t c) {
    (void) c;
    g_repaints++;
}
#define RC_REPAINT ((uintptr_t) fake_repaint)
/* mp_sret2(out, fn, a0, a1): the fakes take the sret pointer first */
void mp_sret2(uintptr_t *out, uintptr_t fn, uintptr_t a0, uintptr_t a1) {
    ((void (*)(uintptr_t *, uintptr_t, uintptr_t)) fn)(out, a0, a1);
}
static int azm_install_hook(uintptr_t f, void *h, void **o) {
    (void) h;
    *o = (void *) f;
    return 1;
}
static int azm_patch_vtable(uintptr_t slot, void *h, void **o) {
    *o = *(void **) slot;
    *(void **) slot = h;
    return 1;
}
static int install_paintcell_slot(void) { return 1; }
/* browse_columns.c's side: Column Edit's state and columns, the headers' lifetimes */
static int g_ce = 1;
static int bc_ce_on(void) { return g_ce; }
static unsigned g_mask = AZMOD_BCOL_DEFAULT;
static int g_mask_sets;
static unsigned bc_mask(void) { return g_mask; }
static void bc_set_mask(unsigned m) {
    g_mask = m & 0xFFu;
    g_mask_sets++;
}
static int g_bc_ep_ok;
static uintptr_t g_built, g_gone;
static void bc_hdr_built(uintptr_t h) { g_built = h; }
static void bc_hdr_gone(uintptr_t h) { g_gone = h; }
static int (*g_bc_take_pick)(int idx);
static int (*g_tlr_field_paint)(void *model, void *g, int row, int col, int w, int h, int sel);

/* ---- the fake SQLite: a "connection" is the stick's number; a statement gives one row ------ */
#define NSTICK 2
static pthread_mutex_t g_fq_mu = PTHREAD_MUTEX_INITIALIZER; /* the sticks' rows, set by the test */
static const char *g_stick_val[NSTICK];                      /* column 1 (text) per stick */
static long long g_stick_num[NSTICK];
static int g_stick_hasnum[NSTICK];
static int g_open_n, g_close_n; /* __atomic only */
static char g_media[PATH_MAX];
static void set_stick(int k, const char *val, int hasnum, long long num) {
    pthread_mutex_lock(&g_fq_mu);
    g_stick_val[k] = val;
    g_stick_hasnum[k] = hasnum;
    g_stick_num[k] = num;
    pthread_mutex_unlock(&g_fq_mu);
}
struct fst {
    int stick, step;
    int hasnum;
    long long num;
    const char *val;
};
/* the stick's number from ".../stk<k>/PIONEER/..." */
static int stick_of(const char *uri) {
    const char *p = strstr(uri, "/PIONEER/");
    if (!p)
        return -1;
    const char *q = p;
    while (q > uri && q[-1] != '/')
        q--;
    return strncmp(q, "stk", 3) ? -1 : atoi(q + 3);
}
static int fq_open(const char *uri, void **db, int flags, const char *vfs) {
    (void) flags;
    (void) vfs;
    int k = stick_of(uri);
    if (k < 0 || k >= NSTICK)
        return 1;
    *db = (void *) (uintptr_t) (0x1000 + k);
    __atomic_add_fetch(&g_open_n, 1, __ATOMIC_ACQ_REL);
    return 0;
}
static int fq_key(void *db, const void *k, int n) {
    (void) db;
    (void) k;
    (void) n;
    return 0;
}
static int fq_close(void *db) {
    (void) db;
    __atomic_add_fetch(&g_close_n, 1, __ATOMIC_ACQ_REL);
    return 0;
}
static int fq_bind_text(void *st, int i, const char *t, int n, void *d) {
    (void) st;
    (void) i;
    (void) t;
    (void) n;
    (void) d;
    return 0;
}
static int fq_prepare(void *db, const char *sql, int n, void **st, const char **tail) {
    (void) sql;
    (void) n;
    (void) tail;
    struct fst *s = calloc(1, sizeof *s);
    if (!s)
        return 1;
    s->stick = (int) ((uintptr_t) db - 0x1000);
    pthread_mutex_lock(&g_fq_mu);
    s->val = g_stick_val[s->stick];
    s->hasnum = g_stick_hasnum[s->stick];
    s->num = g_stick_num[s->stick];
    pthread_mutex_unlock(&g_fq_mu);
    *st = s;
    return 0;
}
static int fq_bind_int64(void *st, int i, long long v) {
    (void) st;
    (void) i;
    (void) v;
    return 0;
}
static int fq_step(void *st) {
    struct fst *s = st;
    return s->step++ == 0 && (s->val || s->hasnum) ? SQLITE_ROW : 101;
}
static long long fq_col_int64(void *st, int c) {
    (void) c;
    return ((struct fst *) st)->num;
}
static const unsigned char *fq_col_text(void *st, int c) {
    struct fst *s = st;
    if (c == 0)
        return s->hasnum ? (const unsigned char *) "n" : 0;
    return (const unsigned char *) s->val;
}
static int fq_finalize(void *st) {
    free(st);
    return 0;
}
static struct {
    int (*prepare)(void *, const char *, int, void **, const char **);
    int (*bind_int64)(void *, int, long long);
    int (*step)(void *);
    long long (*column_int64)(void *, int);
    const unsigned char *(*column_text)(void *, int);
    int (*finalize)(void *);
} g_sq = {fq_prepare, fq_bind_int64, fq_step, fq_col_int64, fq_col_text, fq_finalize};
static struct {
    int (*open_v2)(const char *, void **, int, const char *);
    int (*key)(void *, const void *, int);
    int (*close)(void *);
    int (*bind_text)(void *, int, const char *, int, void *);
} g_smsq = {fq_open, fq_key, fq_close, fq_bind_text};

/* ---- models of EP147's objects ------------------------------------------------------------ */
/* each vtable with a slot before it, as EP147's have (their type_info, read by the RTTI checks) */
static uintptr_t g_hdr_vtab[5], g_btn_vtab[9], g_list_vtab[5], g_bdd_vt[4], g_scl_vt[4], g_wlis_vt[4];
#define g_hdr_vt (g_hdr_vtab + 1)
#define g_btn_vt (g_btn_vtab + 1)
#define g_list_vt (g_list_vtab + 1)
#define BC_HDR_VT ((uintptr_t) g_hdr_vt)
#define TRACKLIST_WIDGET_VT ((uintptr_t) g_list_vt)
#define BC_HDR_MAX 8
static uintptr_t g_bc_hdr[BC_HDR_MAX];
static int g_bc_rowh[BC_HDR_MAX];
static uintptr_t bc_full_hdr(uintptr_t h) { return bc_obj(h, BC_HDR_VT); }
static int g_layouts;
static void bc_layout_hook(long hdr, int rowh) {
    (void) hdr;
    (void) rowh;
    g_layouts++;
}
static void (*g_o_bc_layout)(long, int) = bc_layout_hook;
/* a header: its bytes (vtable, +0x18 parent, +0x138 EP147's button), and its columns */
struct hdr {
    unsigned char b[0x200];
    int x[16], w[16], vis[16];
};
static struct hdr *H(uintptr_t h) { return (struct hdr *) h; }
struct btn {
    uintptr_t vt;
    unsigned char b[0x230 - 8];
};
static int g_btn_deleted;
static void fake_btn_delete(uintptr_t b) {
    g_btn_deleted++;
    free((void *) b);
}
static int fake_colindex(uintptr_t h, int id, int only_vis) {
    (void) only_vis;
    return id >= 0 && id < 16 && H(h)->vis[id] ? id : -1;
}
static void fake_colpos(uintptr_t *out, uintptr_t h, uintptr_t idx) {
    int r[4] = {H(h)->x[idx], 0, H(h)->w[idx], 60};
    memcpy(out, r, sizeof r);
}
/* the last bounds each button was given, by button */
#define NBOUNDS 64
static struct {
    uintptr_t c;
    int r[4];
} g_bounds[NBOUNDS];
static void fake_setbounds(uintptr_t c, const int *r) {
    int i = 0;
    while (i < NBOUNDS - 1 && g_bounds[i].c && g_bounds[i].c != c)
        i++;
    g_bounds[i].c = c;
    memcpy(g_bounds[i].r, r, sizeof g_bounds[i].r);
    memcpy((void *) (c + 0x20), r, 4 * sizeof(int)); /* juce::Component +0x20: x, y, width, height */
}
static const int *bounds_of(uintptr_t c) {
    for (int i = 0; i < NBOUNDS; i++)
        if (g_bounds[i].c == c)
            return g_bounds[i].r;
    return 0;
}
static void fake_setsize(uintptr_t c, int w, int h) {
    (void) c;
    (void) w;
    (void) h;
}
static int g_children;
static void fake_addchild(uintptr_t p, uintptr_t c, int z) {
    (void) p;
    (void) c;
    (void) z;
    g_children++;
}
static void fake_str_ctor(uintptr_t *out, const char **s) { *out = (uintptr_t) strdup(*s); }
static void fake_str_dtor(uintptr_t *s) { free((void *) *s); }
static void fake_img_ctor(uintptr_t *i) { *i = 0; }
static void fake_img_dtor(uintptr_t *i) { (void) i; }
static uintptr_t fake_new(size_t n) { return (uintptr_t) calloc(1, n); }
static uintptr_t g_btn_name_storage;
static void fake_btn_ctor(uintptr_t b, uintptr_t name, uintptr_t *i1, uintptr_t *i2, uintptr_t *i3, int a, int c,
                          float x, float y) {
    (void) name;
    (void) i1;
    (void) i2;
    (void) i3;
    (void) a;
    (void) c;
    (void) x;
    (void) y;
    ((struct btn *) b)->vt = (uintptr_t) g_btn_vt;
}
static uintptr_t fake_btn_res(void) { return 0; }
static void fake_btn_img(uintptr_t *out, uintptr_t a0, uintptr_t a1) {
    (void) a0;
    (void) a1;
    *out = 0;
}
static uint64_t g_btn_d0 = 0x1234;
/* a button's listener list: data at +0x210, count at +0x220 */
struct lislist {
    uintptr_t item[8];
};
static struct lislist g_lists[64];
static int g_nlists;
static void fake_addlis(uintptr_t hdr, uintptr_t lis) {
    uintptr_t btn = 0, data = 0, n = 0;
    own_q(hdr + 0x138, &btn);
    unsigned char *b = (unsigned char *) btn;
    memcpy(&data, b + 0x210, 8);
    memcpy(&n, b + 0x220, 8);
    if (!data) {
        data = (uintptr_t) &g_lists[g_nlists++];
        memcpy(b + 0x210, &data, 8);
    }
    ((struct lislist *) data)->item[n] = lis;
    n++;
    memcpy(b + 0x220, &n, 8);
}
/* BrowseDispData and its sub column list (+0x08 current index, +0x18 tokens, +0x28 count) */
static struct {
    uintptr_t vt;
    unsigned char b[0x100];
} g_bdd;
static struct {
    uintptr_t vt, cur, pad, data, pad2, n;
} g_scl;
static uintptr_t g_token_ptr[20];
/* the list's listener on EP147's button: slot 2 opens EP147's sub column list */
static int g_list_opened;
static void fake_wlis_click(uintptr_t self, void *name) {
    (void) self;
    (void) name;
    g_list_opened++;
}
static struct {
    uintptr_t vt;
} g_wlis;
/* the TrackListHeader ctor: mode 0 builds its own button at +0x138 with the list's listener */
static void fake_hdr_ctor(long hdr, int mode) {
    struct hdr *x = H((uintptr_t) hdr);
    memset(x, 0, sizeof *x);
    uintptr_t vt = (uintptr_t) g_hdr_vt;
    memcpy(x->b, &vt, 8);
    memcpy(x->b + 0x134, &mode, 4);
    if (mode == 0) {
        uintptr_t b = (uintptr_t) calloc(1, sizeof(struct btn));
        ((struct btn *) b)->vt = (uintptr_t) g_btn_vt;
        memcpy(x->b + 0x138, &b, 8);
        fake_addlis((uintptr_t) hdr, (uintptr_t) &g_wlis);
    }
}
static int g_hdr_dtor_n;
static void fake_hdr_dtor(long hdr) {
    struct hdr *x = H((uintptr_t) hdr);
    uintptr_t b = 0;
    memcpy(&b, x->b + 0x138, 8);
    free((void *) b);
    memset(x->b + 0x138, 0, 8);
    g_hdr_dtor_n++;
}
static void fake_bdd_ctor(long t, long a, long b, long c, long d, long e, long f, long g) {
    (void) a;
    (void) b;
    (void) c;
    (void) d;
    (void) e;
    (void) f;
    (void) g;
    uintptr_t vt = (uintptr_t) g_bdd_vt, sl = (uintptr_t) &g_scl;
    memcpy((void *) t, &vt, 8);
    memcpy((unsigned char *) t + 0x70, &sl, 8);
}
/* the cell painter: what it saw in the row's sub item */
static int g_paint_n, g_paint_col, g_paint_kind, g_paint_sel, g_paint_w;
static char g_paint_text[64];
static void fake_paint(void *model, void *g, int row, int col, int w, int h, int sel) {
    (void) g;
    (void) h;
    g_paint_w = w;
    uintptr_t self = (uintptr_t) model - 0x30, ring = 0, s = 0;
    memcpy(&ring, (void *) (self + 0x2a8), 8);
    uintptr_t rec = ring + (uintptr_t) (row % 0x1b) * 0x78;
    memcpy(&s, (void *) (rec + 0x30), 8);
    memcpy(&g_paint_kind, (void *) (rec + 0x4c), 4);
    g_paint_n++;
    g_paint_col = col;
    g_paint_sel = sel;
    snprintf(g_paint_text, sizeof g_paint_text, "%s", s ? (const char *) s : "(null)");
}
static void (*g_orig_paintcell)(void *, void *, int, int, int, int, int) = fake_paint;
/* the Preview component's own paint, in a vtable slot the file patches */
static int g_wave_own;
static void fake_wave_paint(void *self, void *g) {
    (void) self;
    (void) g;
    g_wave_own++;
}
/* the Preview component's own mouseDown / mouseDrag / mouseUp (slots 5, 6, 7) */
static int g_wave_mouse_n[3];
static long g_wave_mouse_e;
static void fake_wave_down(long self, long e) {
    (void) self;
    g_wave_mouse_n[0]++;
    g_wave_mouse_e = e;
}
static void fake_wave_drag(long self, long e) {
    (void) self;
    g_wave_mouse_n[1]++;
    g_wave_mouse_e = e;
}
static void fake_wave_up(long self, long e) {
    (void) self;
    g_wave_mouse_n[2]++;
    g_wave_mouse_e = e;
}
static uintptr_t g_ph_vt[32];
#define PH_VT ((uintptr_t) g_ph_vt)
#define PH_PAINT_SLOT (PH_VT + 26 * 8)
/* the title: the list model's refreshComponentForCell (slot 5), the label's and its text's paint */
static uintptr_t g_model_vt[8], g_hlabel_vt[32], g_sltext_vt[32];
#define REFRESHCELL_SLOT ((uintptr_t) &g_model_vt[5])
#define HLABEL_PAINT_SLOT ((uintptr_t) &g_hlabel_vt[26])
#define SLTEXT_PAINT_SLOT ((uintptr_t) &g_sltext_vt[26])
static int g_label_own, g_text_own;
static void *fake_refresh(void *model, int row, int col, int sel, void *existing) {
    (void) model, (void) row, (void) col, (void) sel;
    return existing;
}
static void fake_label_paint(void *self, void *g) {
    (void) self, (void) g;
    g_label_own++;
}
static void fake_text_paint(void *self, void *g) {
    (void) self, (void) g;
    g_text_own++;
}
static int install_hlabel_paint_slot(void) { return 1; }
/* the row (meow::TouchableTableListBox::RowComp): its mouse slots get Preview's touch */
static int g_row_mouse_n[3];
static long g_row_mouse_self;
static uint64_t g_row_ev[0x80 / 8];
static void row_mouse(int k, long self, void *e) {
    g_row_mouse_n[k]++;
    g_row_mouse_self = self;
    memcpy(g_row_ev, e, sizeof g_row_ev);
}
static void fake_row_down(long self, void *e) { row_mouse(0, self, e); }
static void fake_row_drag(long self, void *e) { row_mouse(1, self, e); }
static void fake_row_up(long self, void *e) { row_mouse(2, self, e); }
static uintptr_t g_rowcomp_vt[8] = {0, 0, 0, 0, 0, (uintptr_t) fake_row_down, (uintptr_t) fake_row_drag,
                                    (uintptr_t) fake_row_up};
#define ROWCOMP_VT ((uintptr_t) g_rowcomp_vt)
/* a juce::Component as the file reads it: +0x18 parent, +0x20 bounds, +0x40 / +0x50 children */
struct comp {
    uintptr_t vt, pad0[2], parent;
    int32_t x, y, w, h;
    uintptr_t pad1[2], children, pad2, count, pad3[5];
};
_Static_assert(__builtin_offsetof(struct comp, parent) == 0x18 && __builtin_offsetof(struct comp, x) == 0x20 &&
                   __builtin_offsetof(struct comp, children) == 0x40 && __builtin_offsetof(struct comp, count) == 0x50,
               "struct comp matches juce::Component's offsets");
/* RTTI: [vtable - 8] the type_info, +8 its mangled name */
struct tinfo {
    const void *vt;
    const char *name;
};
static const struct tinfo g_ti_popup = {0, "N3gui6browse28SubCategorySelectPopupWidgetE"},
                          g_ti_listbox = {0, "N4meow21TouchableTableListBoxE"}, g_ti_comp = {0, "N4juce9ComponentE"};
/* the popup's juce::Component is a secondary base at +0xb0: its vtable's offset-to-top is -0xb0 */
static uintptr_t g_popup_vt[3] = {(uintptr_t) (intptr_t) -0xb0, (uintptr_t) &g_ti_popup}, g_listbox_vt[2] = {(uintptr_t) &g_ti_listbox},
                 g_comp_vt[2] = {(uintptr_t) &g_ti_comp};
/* ListBox::selectRow(row, dontScroll, deselectOthers) */
static int g_select_n, g_select_row;
static uintptr_t g_select_lb;
static void fake_selectrow(uintptr_t lb, int row, int a, int b) {
    (void) a;
    (void) b;
    g_select_n++;
    g_select_lb = lb;
    g_select_row = row;
}
#define LISTBOX_SELECTROW ((uintptr_t) fake_selectrow)
/* the field list's popup: its row labels (+0x60 data, +0x70 count), its add and its update; its
 * list model's row tap (vtable slot 6) */
static int g_pop_updates, g_pop_adds;
static void fake_pop_add(uintptr_t pop, uintptr_t *str) {
    uintptr_t data = 0, c = (uintptr_t) strdup((const char *) *str);
    int32_t n = 0;
    memcpy(&data, (void *) (pop + 0x60), 8);
    memcpy(&n, (void *) (pop + 0x70), 4);
    data = (uintptr_t) realloc((void *) data, (size_t) (n + 1) * 8);
    memcpy((void *) (data + 8 * (uintptr_t) n), &c, 8);
    n++;
    memcpy((void *) (pop + 0x60), &data, 8);
    memcpy((void *) (pop + 0x70), &n, 4);
    g_pop_adds++;
}
static void fake_pop_update(uintptr_t pop) {
    (void) pop;
    g_pop_updates++;
}
static int g_pop_click_n, g_pop_click_row;
static void fake_pop_click(long model, int row) {
    (void) model;
    g_pop_click_n++;
    g_pop_click_row = row;
}
static uintptr_t g_popmodel_vt[8] = {0, 0, 0, 0, 0, 0, (uintptr_t) fake_pop_click};

static volatile uintptr_t vEP_1bcff88 = (uintptr_t) fake_hdr_ctor;
#define EP_1bcff88 vEP_1bcff88
static volatile uintptr_t vEP_2459540 = (uintptr_t) fake_colindex;
#define EP_2459540 vEP_2459540
static volatile uintptr_t vEP_2459640 = (uintptr_t) fake_colpos;
#define EP_2459640 vEP_2459640
static volatile uintptr_t vEP_24a0890 = (uintptr_t) fake_setbounds;
#define EP_24a0890 vEP_24a0890
static volatile uintptr_t vEP_249f200 = (uintptr_t) fake_setsize;
#define EP_249f200 vEP_249f200
static volatile uintptr_t vEP_249d6b0 = (uintptr_t) fake_addchild;
#define EP_249d6b0 vEP_249d6b0
static volatile uintptr_t vEP_233c340 = (uintptr_t) fake_str_ctor;
#define EP_233c340 vEP_233c340
static volatile uintptr_t vEP_22fc600 = (uintptr_t) fake_str_dtor;
#define EP_22fc600 vEP_22fc600
static volatile uintptr_t vEP_23b12b0 = (uintptr_t) fake_img_ctor;
#define EP_23b12b0 vEP_23b12b0
static volatile uintptr_t vEP_23a45f0 = (uintptr_t) fake_img_dtor;
#define EP_23a45f0 vEP_23a45f0
static volatile uintptr_t vEP_424d40 = (uintptr_t) fake_new;
#define EP_424d40 vEP_424d40
static volatile uintptr_t vEP_1e06450 = (uintptr_t) fake_btn_ctor;
#define EP_1e06450 vEP_1e06450
static volatile uintptr_t vEP_1bc41b0 = (uintptr_t) fake_btn_res;
#define EP_1bc41b0 vEP_1bc41b0
static volatile uintptr_t vEP_1bb99f0 = (uintptr_t) fake_btn_img;
#define EP_1bb99f0 vEP_1bb99f0
#define EP_3b8e9f8 ((uintptr_t) &g_btn_name_storage)
#define EP_2b7da80 ((uintptr_t) &g_btn_d0)
static volatile uintptr_t vEP_1bd03f0 = (uintptr_t) fake_addlis;
#define EP_1bd03f0 vEP_1bd03f0
#define EP_2b7f6d8 ((uintptr_t) g_wlis_vt)
static volatile uintptr_t vEP_1f3fb18 = (uintptr_t) fake_bdd_ctor;
#define EP_1f3fb18 vEP_1f3fb18
#define EP_2dcc3a0 ((uintptr_t) g_bdd_vt)
#define EP_29672e0 ((uintptr_t) g_scl_vt)
static volatile uintptr_t vEP_1c296d8 = (uintptr_t) fake_pop_add;
#define EP_1c296d8 vEP_1c296d8
static volatile uintptr_t vEP_1c296b8 = (uintptr_t) fake_pop_update;
#define EP_1c296b8 vEP_1c296b8
#define EP_2bcc810 ((uintptr_t) g_popmodel_vt)
static char g_bf_path[PATH_MAX], g_bf_off[PATH_MAX];
#define BF_PATH g_bf_path
#define BF_OFF g_bf_off
#define B2_MEDIA g_media
#include "libfd.h"
#include "pioneer_dir.h" /* pd_dir: azmod.c includes it before browse_fields.c */
#include BF_SRC

/* browse_edit.c's side of a press on a field button: what it was asked, what it answers */
static int g_be_press_n, g_be_press_x, g_be_drag_n, g_be_drag_x, g_be_release_n;
static int g_be_take = 1, g_be_moved;
static uintptr_t g_be_h;
static int be_btn_press(uintptr_t h, int x) {
    g_be_press_n++;
    g_be_h = h;
    g_be_press_x = x;
    return g_be_take;
}
static void be_btn_drag(uintptr_t h, int x) {
    g_be_drag_n++;
    g_be_h = h;
    g_be_drag_x = x;
}
static int be_btn_release(uintptr_t h) {
    g_be_release_n++;
    g_be_h = h;
    return g_be_moved;
}
/* browse_edit.c's edit mode, and its opening of EP147's own field list */
static int g_editing, g_open_fields_n;
static int be_editing(void) { return g_editing; }
static void be_open_fields(uintptr_t h) {
    (void) h;
    g_open_fields_n++;
}
/* the button class's original mouse slots, and its mouseExit (slot 4) */
static int g_btn_mouse_n[3], g_btn_exit_n;
static void fake_btn_exit(long b, long e) {
    (void) b;
    (void) e;
    g_btn_exit_n++;
}
static void fake_btn_down(long b, long e) {
    (void) b;
    (void) e;
    g_btn_mouse_n[0]++;
}
static void fake_btn_drag(long b, long e) {
    (void) b;
    (void) e;
    g_btn_mouse_n[1]++;
}
static void fake_btn_up(long b, long e) {
    (void) b;
    (void) e;
    g_btn_mouse_n[2]++;
}

/* ---- helpers -------------------------------------------------------------------------- */
static int fmt(int field, int has, long long num, const char *text, char *out, int *kind) {
    return b2_format(field, has, num, text, out, 64, kind);
}
static int valid_utf8_clean(const char *s) {
    size_t n = strlen(s);
    for (size_t i = 0; i < n;) {
        size_t k = b2_utf8_len((const unsigned char *) s + i, n - i);
        if (!k || (k == 1 && ((unsigned char) s[i] < 0x20 || s[i] == 0x7f)))
            return 0;
        i += k;
    }
    return 1;
}
static int parse(const char *s, unsigned char f[B2_MAXID + 1]) { return b2_fields_parse(s, strlen(s), f); }
static int all_none(const unsigned char f[B2_MAXID + 1]) {
    for (int i = 0; i <= B2_MAXID; i++)
        if (f[i] != B2_NONE)
            return 0;
    return 1;
}
static void read_file(const char *p, char *out, size_t cap) {
    out[0] = 0;
    FILE *f = fopen(p, "r");
    if (!f)
        return;
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = 0;
    fclose(f);
}
static int field_now(int id) { return __atomic_load_n(&g_bf_field[id], __ATOMIC_ACQUIRE); }
static uintptr_t g_hdrs[4];
static uintptr_t new_header(int k, int mode) {
    g_hdrs[k] = (uintptr_t) calloc(1, sizeof(struct hdr));
    b2_hdr_ctor_hook((long) g_hdrs[k], mode);
    return g_hdrs[k];
}
/* (cid, title, field)'s entry state and text, read under the file's lock */
static int ent_state(unsigned cid, const char *title, int field, char *text, int *kind) {
    int st = 0;
    b2_lock();
    struct b2_ent *e = b2_ent(cid, sm_hash(title), field, 0);
    if (e) {
        st = e->st;
        if (text)
            snprintf(text, B2_TEXT_MAX, "%s", e->text);
        if (kind)
            *kind = e->kind;
    }
    b2_unlock();
    return st;
}
static void make_known(unsigned cid, const char *title, int field, const char *text, int kind) {
    b2_lock();
    struct b2_ent *e = b2_ent(cid, sm_hash(title), field, 0);
    if (e) {
        e->st = 2;
        snprintf(e->text, sizeof e->text, "%s", text);
        e->kind = kind;
    }
    b2_unlock();
}
static void make_stick(int k, int *fd) {
    char p[PATH_MAX], cmd[PATH_MAX + 32];
    snprintf(p, sizeof p, "%s/stk%d/PIONEER/rekordbox", g_media, k);
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", p);
    if (system(cmd) != 0)
        return;
    strcat(p, "/exportLibrary.db");
    FILE *f = fopen(p, "w");
    if (f) {
        fputs("db", f);
        fclose(f);
    }
    *fd = open(p, O_RDONLY); /* EP147's own connection */
}
/* wait for the worker's answer for (cid, title, field): 2 known, 3 unknown, 0 timed out */
static int wait_answer(unsigned cid, const char *title, int field) {
    for (int i = 0; i < 500; i++) {
        uintptr_t js = 0;
        int k = 0;
        b2_get(cid, title, field, &js, &k);
        int st = ent_state(cid, title, field, 0, 0);
        if (st == 2 || st == 3)
            return st;
        usleep(10000);
    }
    return 0;
}

int main(void) {
    ut_suite = "browse fields";
    snprintf(g_bf_path, sizeof g_bf_path, "/tmp/bft-fields.%d", (int) getpid());
    snprintf(g_bf_off, sizeof g_bf_off, "/tmp/bft-off.%d", (int) getpid());
    char mt[] = "/tmp/bft-media.XXXXXX";
    if (!mkdtemp(mt) || !realpath(mt, g_media)) {
        printf("FAIL  browse fields: no scratch directory\n");
        return 1;
    }
    unlink(g_bf_path);
    unlink(g_bf_off);
    g_wlis.vt = (uintptr_t) g_wlis_vt;
    g_wlis_vt[2] = (uintptr_t) fake_wlis_click;
    g_btn_vt[1] = (uintptr_t) fake_btn_delete;
    g_hdr_vt[0] = (uintptr_t) fake_hdr_dtor;
    g_hdr_vt[1] = (uintptr_t) fake_hdr_dtor;
    g_scl.vt = (uintptr_t) g_scl_vt;
    g_btn_vt[4] = (uintptr_t) fake_btn_exit;
    g_btn_vt[5] = (uintptr_t) fake_btn_down;
    g_btn_vt[6] = (uintptr_t) fake_btn_drag;
    g_btn_vt[7] = (uintptr_t) fake_btn_up;
    g_ph_vt[5] = (uintptr_t) fake_wave_down;
    g_ph_vt[6] = (uintptr_t) fake_wave_drag;
    g_ph_vt[7] = (uintptr_t) fake_wave_up;
    g_ph_vt[26] = (uintptr_t) fake_wave_paint;
    g_model_vt[5] = (uintptr_t) fake_refresh;
    g_hlabel_vt[26] = (uintptr_t) fake_label_paint;
    g_sltext_vt[26] = (uintptr_t) fake_text_paint;
    static const char *const toks[] = {"DEFAULT", "TRACK", "ARTIST", "ALBUM", "RATING", "GENRE",
                                       "COMMENTS", "TIME", "REMIXER", "LABEL", "ORIGINAL ARTIST",
                                       "BITRATE", "COLOR", "DJ PLAY COUNT", "DATE ADDED", "YEAR"};
    for (int i = 0; i < 16; i++)
        g_token_ptr[i] = (uintptr_t) toks[i];
    g_scl.data = (uintptr_t) g_token_ptr;
    g_scl.n = 16;
    char o[64];
    int k = 0;

    /* ---- the fields ---- */
    int all = 1;
    for (int i = 1; i < B2_N; i++)
        all &= b2_field_of_token(toks[i]) == i;
    CHECK("every list token names its field", all);
    CHECK("DEFAULT, an unknown token, a lower-case one, NULL: no field",
          b2_field_of_token("DEFAULT") == B2_NONE && b2_field_of_token("YEAR") == B2_NONE &&
              b2_field_of_token("genre") == B2_NONE && b2_field_of_token(0) == B2_NONE);
    fmt(B2_RATING, 1, 2, 0, o, &k);
    CHECK("rating 2: two filled stars of five",
          !strcmp(o, "\xe2\x98\x85 \xe2\x98\x85 \xe2\x98\x86 \xe2\x98\x86 \xe2\x98\x86") && k == 0);
    char z[64];
    fmt(B2_RATING, 0, 0, 0, o, &k);
    fmt(B2_RATING, 1, 0, 0, z, &k);
    CHECK("rating NULL reads as 0; 9 as 5; -1 as 0",
          !strcmp(o, z) && !strcmp(o, B2_STARS[0]) && (fmt(B2_RATING, 1, 9, 0, o, &k), !strcmp(o, B2_STARS[5])) &&
              (fmt(B2_RATING, 1, -1, 0, o, &k), !strcmp(o, B2_STARS[0])));
    fmt(B2_TIME, 1, 112, 0, o, &k);
    CHECK("time 112 s: 01:52", !strcmp(o, "01:52"));
    fmt(B2_TIME, 1, 6000, 0, o, &k);
    CHECK("time 6000 s: 100:00 (minutes are not wrapped)", !strcmp(o, "100:00"));
    CHECK("time NULL, negative, absurd: empty",
          (fmt(B2_TIME, 0, 0, 0, o, &k), !o[0]) && (fmt(B2_TIME, 1, -5, 0, o, &k), !o[0]) &&
              (fmt(B2_TIME, 1, 1LL << 40, 0, o, &k), !o[0]));
    CHECK("bitrate 320: 320kbps; NULL, 0 and absurd: VBR",
          (fmt(B2_BITRATE, 1, 320, 0, o, &k), !strcmp(o, "320kbps")) &&
              (fmt(B2_BITRATE, 0, 0, 0, o, &k), !strcmp(o, "VBR")) &&
              (fmt(B2_BITRATE, 1, 0, 0, o, &k), !strcmp(o, "VBR")) &&
              (fmt(B2_BITRATE, 1, 1LL << 40, 0, o, &k), !strcmp(o, "VBR")));
    CHECK("DJ play count 7: 7; NULL and negative: 0",
          (fmt(B2_PLAYS, 1, 7, 0, o, &k), !strcmp(o, "7")) && (fmt(B2_PLAYS, 0, 0, 0, o, &k), !strcmp(o, "0")) &&
              (fmt(B2_PLAYS, 1, -3, 0, o, &k), !strcmp(o, "0")));
    fmt(B2_COLOR, 1, 1, "Pink", o, &k);
    CHECK("color 1 Pink: its name, kind 0x18 (EP147 draws the dot)", !strcmp(o, "Pink") && k == 0x18);
    fmt(B2_COLOR, 1, 8, "Purple", o, &k);
    CHECK("color 8: kind 0x1f", k == 0x1f && !strcmp(o, "Purple"));
    CHECK("no color, color 0, color 9: empty, no dot",
          (fmt(B2_COLOR, 0, 0, "x", o, &k), !o[0] && !k) && (fmt(B2_COLOR, 1, 0, "x", o, &k), !o[0] && !k) &&
              (fmt(B2_COLOR, 1, 9, "x", o, &k), !o[0] && !k));
    fmt(B2_GENRE, 0, 0, "UK Garage / Bassline", o, &k);
    CHECK("a text field: the text as it is", !strcmp(o, "UK Garage / Bassline") && k == 0);
    fmt(B2_ARTIST, 0, 0, 0, o, &k);
    CHECK("a NULL text: empty", !o[0]);
    fmt(B2_COMMENTS, 0, 0, "a\tb\nc\x7f", o, &k);
    CHECK("control characters shown as ?", !strcmp(o, "a?b?c?"));
    fmt(B2_COMMENTS, 0, 0, "x\xc3(\xe2\x82y\xed\xa0\x80\xc0\xaf\xf5\x80", o, &k);
    CHECK("invalid UTF-8 (truncated, surrogate, overlong, past U+10FFFF) shown as ?, valid kept",
          valid_utf8_clean(o) && o[0] == 'x' && strchr(o, 'y'));
    CHECK("an overlong form is invalid: 0xc0 0xaf, 0xe0 0x80 0xaf",
          b2_utf8_len((const unsigned char *) "\xc0\xaf", 2) == 0 &&
              b2_utf8_len((const unsigned char *) "\xe0\x80\xaf", 3) == 0 &&
              b2_utf8_len((const unsigned char *) "\xf0\x9f\x8e\xb5", 3) == 0 &&
              b2_utf8_len((const unsigned char *) "\xf0\x9f\x8e\xb5", 4) == 4);
    fmt(B2_COMMENTS, 0, 0, "Caf\xc3\xa9 \xe2\x98\x85 \xf0\x9f\x8e\xb5", o, &k);
    CHECK("valid UTF-8 kept byte for byte", !strcmp(o, "Caf\xc3\xa9 \xe2\x98\x85 \xf0\x9f\x8e\xb5"));
    char longs[600];
    for (int i = 0; i < 199; i++)
        memcpy(longs + 3 * i, "\xe2\x98\x85", 3);
    longs[597] = 0;
    char big[B2_TEXT_MAX + 32];
    b2_format(B2_COMMENTS, 0, 0, longs, big, sizeof big, &k);
    CHECK("a long text cut under B2_TEXT_MAX at a character boundary",
          strlen(big) < B2_TEXT_MAX && strlen(big) % 3 == 0 && valid_utf8_clean(big));
    b2_clean_text(longs, o, 20);
    CHECK("a small buffer: cut at a character boundary within it", strlen(o) == 18 && valid_utf8_clean(o));
    o[0] = 'q';
    b2_clean_text("abc", o, 0);
    CHECK("a zero-size buffer: nothing written", o[0] == 'q');
    CHECK("no field, or a field past the table: nothing to show",
          b2_format(B2_NONE, 0, 0, "x", o, 64, &k) == 0 && b2_format(B2_N, 0, 0, "x", o, 64, &k) == 0 && !o[0]);
    CHECK("a buffer under 16 bytes: nothing to show", b2_format(B2_GENRE, 0, 0, "x", o, 8, &k) == 0 && !o[0]);

    /* ---- the saved file ---- */
    unsigned char f[B2_MAXID + 1], r[B2_MAXID + 1];
    CHECK("two columns: both set", parse("3 GENRE\n7 COMMENTS\n", f) == 2 && f[3] == B2_GENRE && f[7] == B2_COMMENTS);
    CHECK("CR LF and blank lines, no final newline: read",
          parse("\r\n1 RATING\r\n\r\n10 DJ PLAY COUNT", f) == 2 && f[1] == B2_RATING && f[10] == B2_PLAYS);
    CHECK("an empty file: nothing set", parse("", f) == 0 && all_none(f));
    CHECK("an id given twice: rejected, nothing set", parse("3 GENRE\n3 ALBUM\n", f) == -1 && all_none(f));
    CHECK("id 0, id 11, id 999: rejected", parse("0 GENRE\n", f) == -1 && parse("11 GENRE\n", f) == -1 &&
                                               parse("999 GENRE\n", f) == -1 && all_none(f));
    CHECK("a four-digit id: rejected", parse("0003 GENRE\n", f) == -1);
    CHECK("DEFAULT or TRACK as a column's field: rejected",
          parse("3 DEFAULT\n", f) == -1 && parse("3 TRACK\n", f) == -1 && all_none(f));
    CHECK("an unknown token, a lower-case one: rejected", parse("3 YEAR\n", f) == -1 && parse("3 genre\n", f) == -1);
    CHECK("no space, no id, two spaces: rejected",
          parse("3GENRE\n", f) == -1 && parse(" GENRE\n", f) == -1 && parse("3  GENRE\n", f) == -1);
    CHECK("an id at the end of the file: rejected", parse("3", f) == -1);
    char lt[80];
    memset(lt, 'A', sizeof lt);
    memcpy(lt, "3 ", 2);
    lt[2 + 31] = 0;
    CHECK("a token of 31 bytes: rejected as too long", parse(lt, f) == -1);
    char of[B2_FILE_MAX + 16];
    memset(of, '\n', sizeof of);
    memcpy(of, "3 GENRE\n", 8);
    CHECK("a file at B2_FILE_MAX: read; one byte over: rejected",
          b2_fields_parse(of, B2_FILE_MAX, f) == 1 && b2_fields_parse(of, B2_FILE_MAX + 1, f) == -1 && all_none(f));
    parse("1 TIME\n2 BITRATE\n3 COLOR\n7 ORIGINAL ARTIST\n8 DATE ADDED\n", f);
    char buf[B2_FILE_MAX];
    int len = b2_fields_format(f, buf, sizeof buf);
    CHECK("format, then parse: the same fields",
          len > 0 && b2_fields_parse(buf, (size_t) len, r) == 5 && !memcmp(f, r, sizeof f));
    CHECK("format: one line per field, by id", !strcmp(buf, "1 TIME\n2 BITRATE\n3 COLOR\n7 ORIGINAL ARTIST\n8 DATE ADDED\n"));
    f[4] = B2_TRACK;
    f[5] = B2_N;
    len = b2_fields_format(f, buf, sizeof buf);
    CHECK("format leaves out TRACK and a field past the table", b2_fields_parse(buf, (size_t) len, r) == 5 && r[4] == 0 && r[5] == 0);
    CHECK("format into a buffer too small: -1", b2_fields_format(f, buf, 10) == -1 && b2_fields_format(f, buf, 0) == -1);

    /* ---- the columns that can show a field ---- */
    g_v200 = 1;
    CHECK("v2.00: Preview 1, artwork 2, # 3, BPM 7, Key 8, the title 5",
          bf_col(0) == 1 && bf_col(1) == 2 && bf_col(2) == 3 && bf_col(3) == 7 && bf_col(4) == 8 && bf_col(5) == 5 &&
              bf_col(6) == 0);
    CHECK("v2.00: those swappable; the status icons (4), the sub column (6), 0 and 9 not",
          bf_swappable(1) && bf_swappable(2) && bf_swappable(3) && bf_swappable(7) && bf_swappable(8) &&
              bf_swappable(5) && !bf_swappable(4) && !bf_swappable(b2_sub_id()) && !bf_swappable(0) &&
              !bf_swappable(9) && b2_sub_id() == 6);
    g_v200 = 0;
    CHECK("v1.30: the title 4, the sub column 5 not swappable", bf_col(5) == 4 && bf_swappable(4) && !bf_swappable(5));
    g_v200 = 1;
    g_v200 = 0;
    CHECK("v1.30: BPM 6, Key 7 swappable; the sub column (5) and 8 not",
          bf_swappable(6) && bf_swappable(7) && !bf_swappable(5) && !bf_swappable(8) && b2_sub_id() == 5 &&
              bf_swappable(1) && bf_swappable(2) && bf_swappable(3));
    g_v200 = 1;

    /* ---- installed: the saved fields loaded, ids that cannot show one ignored ---- */
    FILE *sf = fopen(g_bf_path, "w");
    if (sf) {
        fputs("3 GENRE\n6 ALBUM\n5 LABEL\n8 TIME\n", sf);
        fclose(sf);
    }
    CHECK("off before install: no column shows a field", bf_field_of(3) == B2_NONE);
    install_browse_fields();
    CHECK("installed: on, the pick and the painter handed over",
          g_b2_on && g_b2_ready && g_bc_take_pick == bf_take_pick && g_tlr_field_paint == bf_paint);
    CHECK("loaded: # Genre, Key Time, the title Label; the sub column ignored",
          bf_field_of(3) == B2_GENRE && bf_field_of(8) == B2_TIME && field_now(6) == B2_NONE &&
              field_now(5) == B2_LABEL);
    bf_set_field(5, B2_NONE);
    CHECK("bf_field_of outside 1..B2_MAXID: none", bf_field_of(0) == B2_NONE && bf_field_of(B2_MAXID + 1) == B2_NONE);

    /* ---- bf_set_field ---- */
    g_layouts = 0;
    bf_set_field(2, B2_RATING);
    char saved[B2_FILE_MAX + 1];
    read_file(g_bf_path, saved, sizeof saved);
    CHECK("a field set: shown, saved, layout and repaint asked",
          bf_field_of(2) == B2_RATING && !strcmp(saved, "2 RATING\n3 GENRE\n8 TIME\n") &&
              __atomic_load_n(&g_b2_layout, __ATOMIC_ACQUIRE) && __atomic_load_n(&g_b2_refresh, __ATOMIC_ACQUIRE));
    bf_set_field(2, B2_TRACK);
    CHECK("Track set: the column's own data (none)", field_now(2) == B2_NONE);
    bf_set_field(6, B2_GENRE);
    bf_set_field(3, B2_N);
    bf_set_field(3, -1);
    CHECK("the sub column, a field past the table, a negative one: nothing changed",
          field_now(6) == B2_NONE && field_now(3) == B2_GENRE);
    bf_tick();
    CHECK("bf_tick: the asked layout runs once", __atomic_load_n(&g_b2_layout, __ATOMIC_ACQUIRE) == 0);

    /* ---- headers: built before the hooks are in, a compact pane, a full-width one ---- */
    g_b2_ready = 0;
    uintptr_t h0 = new_header(0, 0);
    CHECK("a header built before the hooks are all in: no buttons", !b2_of(h0));
    g_b2_ready = 1;
    uintptr_t hp = new_header(1, 1);
    CHECK("a compact pane's header: no buttons", !b2_of(hp));
    int kids = g_children;
    uintptr_t h = new_header(2, 0);
    CHECK("a header built: browse_columns.c told", g_built == h);
    struct b2_hdr *e = b2_of(h);
    int nb = 0;
    for (int i = 0; e && i < BF_NBTN; i++)
        nb += e->btn[i] && e->btn_vt[i] == (uintptr_t) g_btn_vt;
    CHECK("a full-width header: five buttons on it", e && nb == BF_NBTN && g_children == kids + BF_NBTN);
    uintptr_t own = 0, data = 0, n = 0;
    own_q(h + 0x138, &own);
    own_q(own + 0x210, &data);
    own_q(own + 0x220, &n);
    int slot = e ? (int) (e - g_b2h) : 0;
    CHECK("EP147's button: the list's listener and ours (column 0)",
          n == 2 && ((struct lislist *) data)->item[1] == (uintptr_t) &g_b2_lis[slot][0] && g_b2_lis[slot][0].col == 0);
    own_q(e->btn[2] + 0x210, &data);
    own_q(e->btn[2] + 0x220, &n);
    CHECK("the # button: our listener, column 3",
          n == 1 && ((struct lislist *) data)->item[0] == (uintptr_t) &g_b2_lis[slot][3] && g_b2_lis[slot][3].col == 3);

    /* ---- the buttons after a layout ---- */
    {
        int px[3], pw[3], pwant[3] = {1, 1, 1}, pb[3];
        /* columns given out of their x order: placed right to left all the same */
        px[0] = 700, pw[0] = 200; /* wide: inset by the zone */
        px[1] = 100, pw[1] = 52;  /* narrow: flush with its divider */
        px[2] = 400, pw[2] = 110; /* room for the button and two zones: inset */
        b2_place(px, pw, pwant, 3, 12, pb);
        CHECK("b2_place: a column with room: its button on its right, the zone clear of the divider",
              pb[0] == 700 + 200 - 12 - B2_BTN_W && pb[2] == 400 + 110 - 12 - B2_BTN_W);
        CHECK("b2_place: a narrow column's button flush with its divider", pb[1] == 100 + 52 - B2_BTN_W);
        px[0] = 152, pw[0] = 60; /* flush at 132: the one to its left must not cover it */
        b2_place(px, pw, pwant, 3, 12, pb);
        CHECK("b2_place: no button covers its right neighbour",
              pb[0] == 132 && pb[1] == 132 - B2_BTN_W && pb[1] + B2_BTN_W <= pb[0]);
        px[1] = 0, pw[1] = 40;
        b2_place(px, pw, pwant, 3, 12, pb);
        CHECK("b2_place: never past the header's left edge", pb[1] == 0);
        pwant[2] = 0;
        pw[0] = 0;
        b2_place(px, pw, pwant, 3, 12, pb);
        CHECK("b2_place: no button where none is wanted or the column has no width", pb[2] == -1 && pb[0] == -1 &&
                                                                                         pb[1] == 0);
    }
    for (int id = 1; id < 16; id++) {
        H(h)->vis[id] = 1;
        H(h)->x[id] = id * 100;
        H(h)->w[id] = 100;
    }
    H(h)->w[3] = 40; /* # narrow */
    H(h)->vis[8] = 0;
    bf_after_layout(h, 1, 10);
    const int *b1 = bounds_of(e->btn[0]), *b2 = bounds_of(e->btn[1]), *b3 = bounds_of(e->btn[2]),
              *b7 = bounds_of(e->btn[3]), *b8 = bounds_of(e->btn[4]);
    CHECK("edit mode: Key's column hidden, no button; BPM's inset clear of the zone",
          b8 && b8[2] == 0 && b7 && b7[0] == 700 + 100 - 10 - B2_BTN_W && b7[2] == B2_BTN_W && b7[3] == B2_BTN_H);
    CHECK("... #'s flush with its divider, artwork's and Preview's pushed left of their neighbours",
          b3 && b3[0] == 300 + 40 - B2_BTN_W && b2 && b2[0] == b3[0] - B2_BTN_W && b1 && b1[0] == b2[0] - B2_BTN_W &&
              b1[2] == B2_BTN_W);
    bf_after_layout(h, 0, 10);
    b1 = bounds_of(e->btn[0]);
    CHECK("outside edit mode: no room for any", b1 && b1[2] == 0 && bounds_of(e->btn[3])[2] == 0);
    {
        uintptr_t own = 0;
        own_q(h + 0x138, &own);
        int ob[4] = {560, 0, 80, 60}; /* EP147's own button on the title (500, 100 wide) */
        memcpy((void *) (own + 0x20), ob, sizeof ob);
        bf_after_layout(h, 1, 10);
        const int *b5 = bounds_of(e->btn[5]);
        CHECK("the title's button: left of EP147's own one on the title, never over it",
              b5 && b5[2] == B2_BTN_W && b5[0] == 560 - B2_BTN_W && b5[0] + B2_BTN_W <= 560);
        ob[0] = 1100;
        memcpy((void *) (own + 0x20), ob, sizeof ob);
        bf_after_layout(h, 1, 10);
        b5 = bounds_of(e->btn[5]);
        CHECK("... EP147's own one elsewhere: at the title's right, inset", b5 && b5[0] == 500 + 100 - 10 - B2_BTN_W);
    }

    /* ---- a pick ---- */
    fake_bdd_ctor((long) &g_bdd, 0, 0, 0, 0, 0, 0, 0);
    b2_bdd_ctor_hook((long) &g_bdd, 0, 0, 0, 0, 0, 0, 0);
    CHECK("b2_list_index: a field's place in EP147's list; DEFAULT 0; out of range -1",
          b2_list_index(B2_COMMENTS) == 6 && b2_sub_list() == (uintptr_t) &g_scl && b2_list_index(B2_NONE) == 0 &&
              b2_list_index(B2_N) == -1);
    g_scl.n = 5;
    CHECK("b2_list_index: a field past the list's count: -1", b2_list_index(B2_COMMENTS) == -1);
    g_scl.n = 16;
    g_scl.cur = 4; /* EP147's own sub column shows Rating */
    b2_lis_click(&g_b2_lis[slot][0], 0);
    CHECK("EP147's own button: its pick is EP147's", bf_take_pick(5) == 0 && field_now(3) == B2_GENRE);
    int opened = g_list_opened;
    b2_lis_click(&g_b2_lis[slot][3], 0);
    CHECK("the # button: EP147's list opened through the list's own listener", g_list_opened == opened + 1);
    CHECK("... EP147's sub column list index left as it was", (int) (uint32_t) g_scl.cur == 4);
    CHECK("... the pick that follows is the column's: Comments, not EP147's",
          bf_take_pick(6) == 1 && field_now(3) == B2_COMMENTS);
    CHECK("... and only that one", bf_take_pick(7) == 0 && field_now(3) == B2_COMMENTS);
    b2_lis_click(&g_b2_lis[slot][1], 0);
    b2_lis_click(&g_b2_lis[slot][0], 0);
    CHECK("our button, then EP147's: the pick is EP147's", bf_take_pick(5) == 0 && field_now(2) == B2_NONE);
    b2_lis_click(&g_b2_lis[slot][3], 0);
    advance(B2_ASK_MS + 1);
    CHECK("a pick past B2_ASK_MS after our click: EP147's", bf_take_pick(5) == 0 && field_now(3) == B2_COMMENTS);
    b2_lis_click(&g_b2_lis[slot][3], 0);
    advance(B2_ASK_MS - 1);
    CHECK("a pick just inside B2_ASK_MS: the column's", bf_take_pick(7) == 1 && field_now(3) == B2_TIME);
    b2_lis_click(&g_b2_lis[slot][3], 0);
    CHECK("a token no column shows: taken, nothing changed", bf_take_pick(15) == 1 && field_now(3) == B2_TIME);
    b2_lis_click(&g_b2_lis[slot][3], 0);
    CHECK("an index past the list: taken (never EP147's), nothing changed",
          bf_take_pick(40) == 1 && field_now(3) == B2_TIME);
    b2_lis_click(&g_b2_lis[slot][3], 0);
    CHECK("Track picked: the column's own data", bf_take_pick(1) == 1 && field_now(3) == B2_NONE);
    b2_lis_click(&g_b2_lis[slot][4], 0);
    CHECK("the BPM button: Genre on column 7", bf_take_pick(5) == 1 && field_now(7) == B2_GENRE);
    b2_lis_click(&g_b2_lis[slot][4], 0);
    CHECK("DEFAULT picked: the column's own data", bf_take_pick(0) == 1 && field_now(7) == B2_NONE);

    /* ---- a cell ---- */
    static uint64_t list64[0x80], ring64[27 * 0x78 / 8], list2_64[0x80];
    /* the screen: the top-level component, the list's ListBox under it, the header under that */
    static struct comp screen, tlb;
    screen.vt = (uintptr_t) &g_comp_vt[1];
    tlb.vt = 0x77;
    tlb.parent = (uintptr_t) &screen;
    unsigned char *list = (unsigned char *) list64, *ring = (unsigned char *) ring64;
    uintptr_t lvt = (uintptr_t) g_list_vt, rp = (uintptr_t) ring, nrows = 20, lbp = (uintptr_t) &tlb, lbvt = 0x77;
    memcpy(list, &lvt, 8);
    memcpy(list + 0x48, &lbp, 8);
    memcpy(list + 0x2a8, &rp, 8);
    memcpy(list + 0x2c0, &nrows, 8);
    memcpy(H(h)->b + 0x18, &lbp, 8); /* the header's parent is the list's ListBox */
    static const char *titles[5] = {"T0", "T1", "T2", "T3", "T4"};
    const char *own_sub = "own sub item";
    for (int rr = 0; rr < 5; rr++) {
        unsigned char *rec = ring + rr * 0x78;
        int kd = 0x18;
        uintptr_t tp = (uintptr_t) titles[rr], sp = (uintptr_t) own_sub;
        memcpy(rec, &rr, 4);
        memcpy(rec + 0x28, &tp, 8);
        memcpy(rec + 0x30, &sp, 8);
        memcpy(rec + 0x4c, &kd, 4);
    }
    void *model = list + 0x30;
    bf_set_field(3, B2_GENRE);
    CHECK("a column showing its own data: EP147 paints it", bf_paint(model, 0, 1, 2, 300, 52, 0) == 0);
    g_paint_n = 0;
    CHECK("a row not known yet: handled, not drawn, queued",
          bf_paint(model, 0, 1, 3, 300, 52, 1) == 1 && g_paint_n == 0 && g_b2q_n == 1 &&
              ent_state(101, "T1", B2_GENRE, 0, 0) == 1);
    CHECK("column 3 records the row painted and its selection", g_b2_sel_row == 1 && g_b2_sel == 1 &&
                                                                    g_b2_sel_model == (uintptr_t) model);
    g_ce = 0;
    g_paint_n = 0;
    CHECK("Column Edit off: the column's own data, EP147 paints it",
          bf_field_of(3) == B2_NONE && bf_paint(model, 0, 1, 3, 300, 52, 1) == 0 && g_paint_n == 0);
    g_ce = 1;
    CHECK("... on again: the field is back", bf_field_of(3) == B2_GENRE);
    make_known(101, "T1", B2_GENRE, "Deep House", 0);
    bf_paint(model, 0, 1, 3, 300, 52, 1);
    uintptr_t after = 0;
    int kafter = 0;
    memcpy(&after, ring + 0x78 + 0x30, 8);
    memcpy(&kafter, ring + 0x78 + 0x4c, 4);
    CHECK("a known row: EP147's painter draws the sub column with the column's value",
          g_paint_n == 1 && g_paint_col == 6 && !strcmp(g_paint_text, "Deep House") && g_paint_kind == 0 &&
              g_paint_sel == 1);
    CHECK("... and the row's own sub item is back afterwards", after == (uintptr_t) own_sub && kafter == 0x18);
    g_v200 = 0;
    bf_paint(model, 0, 1, 3, 300, 52, 0);
    CHECK("v1.30: the sub column painter is column 5", g_paint_col == 5);
    g_v200 = 1;
    int seen = 0;
    for (int i = 0; i < B2_LISTS; i++)
        seen += wref_get(&g_b2_lists[i]) == (uintptr_t) list;
    CHECK("the list is remembered once for the repaint", seen == 1);
    /* a compact list: no full-width header is its ListBox's parent */
    static uint64_t lbc64[2];
    uintptr_t lbc = (uintptr_t) lbc64;
    memcpy(lbc64, &lbvt, 8);
    memcpy(list2_64, list64, sizeof list64);
    memcpy((unsigned char *) list2_64 + 0x48, &lbc, 8);
    g_paint_n = 0;
    CHECK("a compact list: EP147 paints its own data", bf_paint((unsigned char *) list2_64 + 0x30, 0, 1, 3, 300, 52, 0) == 0 &&
                                                           g_paint_n == 0);
    titles[2] = "x2";
    uintptr_t tp2 = (uintptr_t) titles[2];
    memcpy(ring + 2 * 0x78 + 0x28, &tp2, 8);
    g_paint_n = 0;
    CHECK("a row that is not a track of the reply page: handled, not drawn",
          bf_paint(model, 0, 2, 3, 300, 52, 0) == 1 && g_paint_n == 0);
    int r7 = 7;
    memcpy(ring + 3 * 0x78, &r7, 4);
    CHECK("a ring slot holding another row: not drawn", bf_paint(model, 0, 3, 3, 300, 52, 0) == 1 && g_paint_n == 0);
    CHECK("a row past the list's count: not drawn", bf_paint(model, 0, 25, 3, 300, 52, 0) == 1 && g_paint_n == 0);

    /* Preview: column 1 through the component's paint */
    bf_set_field(1, B2_COMMENTS);
    bf_paint(model, 0, 4, 3, 300, 52, 1); /* the row's other cells: row 4 selected */
    g_b2_sel_row = 4;
    bf_paint(model, 0, 4, 1, 300, 52, 0);
    CHECK("column 1 does not record the selected row", g_b2_sel_row == 4 && g_b2_sel == 1);
    make_known(104, "T4", B2_COMMENTS, "warm-up", 0);
    static uint64_t wave64[0x110 / 8];
    unsigned char *wave = (unsigned char *) wave64;
    uintptr_t mp = (uintptr_t) list + PH_OWNER_OFF, wh = (52ULL << 32) | 160, wrow = 4;
    memcpy(wave + 0xc8, &mp, 8);
    memcpy(wave + 0x28, &wh, 8);
    memcpy(wave + 0xf8, &wrow, 8);
    void (*wave_paint)(void *, void *) = (void (*)(void *, void *)) g_ph_vt[26];
    g_wave_own = 0;
    g_paint_n = 0;
    wave_paint(wave, 0);
    CHECK("Preview showing a field: drawn by the sub column painter, the waveform not painted",
          wave_paint == b2_wave_paint_hook && g_wave_own == 0 && g_paint_n == 1 && !strcmp(g_paint_text, "warm-up") &&
              g_paint_sel == 1);
    bf_set_field(1, B2_NONE);
    g_paint_n = 0;
    wave_paint(wave, 0);
    CHECK("Preview showing its own data: the waveform's own paint", g_wave_own == 1 && g_paint_n == 0);
    bf_set_field(1, B2_COMMENTS);
    uintptr_t mp2 = (uintptr_t) list2_64 + PH_OWNER_OFF;
    memcpy(wave + 0xc8, &mp2, 8); /* a row of the compact list */
    wave_paint(wave, 0);
    CHECK("a compact list's Preview: its own paint", g_wave_own == 2);

    /* ---- the title column: a component, its text painted as the field ---- */
    {
        static uint64_t lab64[0x100 / 8], txt64[0x100 / 8], lab2_64[0x100 / 8], txt2_64[0x100 / 8];
        uintptr_t lab = (uintptr_t) lab64, txt = (uintptr_t) txt64, lab2 = (uintptr_t) lab2_64,
                  txt2 = (uintptr_t) txt2_64, twh = (48ULL << 32) | 600;
        memcpy(txt64 + 3, &lab, 8); /* +0x18 parent */
        memcpy(txt2_64 + 3, &lab2, 8);
        memcpy(txt64 + 5, &twh, 8); /* +0x28 width, height */
        memcpy(txt2_64 + 5, &twh, 8);
        void *(*refresh)(void *, int, int, int, void *) = (void *(*) (void *, int, int, int, void *) ) g_model_vt[5];
        void (*lpaint)(void *, void *) = (void (*)(void *, void *)) g_hlabel_vt[26],
                                    (*tpaint)(void *, void *) = (void (*)(void *, void *)) g_sltext_vt[26];
        CHECK("the title: refreshComponentForCell, the label's and the text's paint chained",
              refresh == b2_refresh_hook && g_o_b2_refresh == fake_refresh && lpaint == b2_title_paint_hook &&
                  tpaint == b2_title_text_hook && g_o_b2_title_text == fake_text_paint);
        CHECK("... the component EP147 built is returned", refresh(model, 4, 5, 0, (void *) lab) == (void *) lab);
        refresh(model, 3, 4, 0, (void *) lab2); /* another column: not the title's */
        bf_set_field(5, B2_COMMENTS);
        g_label_own = g_text_own = g_paint_n = 0;
        lpaint((void *) lab, 0);
        tpaint((void *) txt, 0);
        CHECK("the title showing a field: the label paints itself, its text is the row's field",
              g_label_own == 1 && g_text_own == 0 && g_paint_n == 1 && !strcmp(g_paint_text, "warm-up") &&
                  g_paint_w == 600 + B2_TL_PAST);
        lpaint((void *) lab2, 0);
        tpaint((void *) txt2, 0);
        CHECK("a label never registered as a title: its own text", g_label_own == 2 && g_text_own == 1 && g_paint_n == 1);
        lpaint((void *) lab, 0);
        tpaint((void *) txt2, 0);
        CHECK("another label's text: its own", g_text_own == 2 && g_paint_n == 1);
        refresh((unsigned char *) list2_64 + 0x30, 4, 5, 0, (void *) lab2); /* a compact list's title */
        lpaint((void *) lab2, 0);
        tpaint((void *) txt2, 0);
        CHECK("a compact list's title: its own text", g_text_own == 3 && g_paint_n == 1);
        bf_set_field(5, B2_NONE);
        lpaint((void *) lab, 0);
        tpaint((void *) txt, 0);
        CHECK("the title showing its own data: its own text", g_text_own == 4 && g_paint_n == 1);
    }
    bf_set_field(1, B2_NONE);

    /* repaints */
    g_repaints = 0;
    __atomic_store_n(&g_b2_refresh, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_b2_landed, 1, __ATOMIC_RELEASE);
    bf_tick();
    CHECK("a value landed: the remembered list's rows repaint", g_repaints == 1);
    for (int i = 0; i < B2_LISTS; i++)
        if (wref_get(&g_b2_lists[i]) == (uintptr_t) list)
            wref_set(&g_b2_lists[i], 0);
    __atomic_store_n(&g_b2_landed, 1, __ATOMIC_RELEASE);
    bf_tick();
    CHECK("a list since destroyed: no repaint", g_repaints == 1);

    /* ---- the field list moved under the column's button ---- */
    CHECK("b2_pop_dx: as asked; clamped to the screen's right; to the header's left edge",
          b2_pop_dx(100, 380, 50, 0, 1280) == 50 && b2_pop_dx(800, 380, 200, 0, 1280) == 100 &&
              b2_pop_dx(800, 380, -900, 0, 1280) == -800 && b2_pop_dx(800, 380, -900, 30, 1280) == -770);
    bf_after_layout(h, 1, 10); /* edit mode: # at 260, BPM at 710, each 80 wide */
    uintptr_t ownb = 0;
    own_q(h + 0x138, &ownb);
    int own_bounds[4] = {1100, 0, 80, 60};
    memcpy((void *) (ownb + 0x20), own_bounds, sizeof own_bounds);
    static uint64_t popfull[0x200 / 8]; /* the popup object: +0x30 its list model, +0x60 its labels */
#define popup (*(struct comp *) ((unsigned char *) popfull + 0xb0))
    static struct comp plist;
    popfull[0x30 / 8] = (uint64_t) (uintptr_t) g_popmodel_vt;
    static uintptr_t popkids[1], scrkids[1];
    popup.vt = (uintptr_t) &g_popup_vt[2];
    popup.x = 0, popup.y = 0, popup.w = 1280, popup.h = 800;
    plist.vt = (uintptr_t) &g_listbox_vt[1];
    plist.x = 800, plist.y = 60, plist.w = 380, plist.h = 600; /* right-aligned under EP147's button */
    popkids[0] = (uintptr_t) &plist;
    popup.children = (uintptr_t) popkids;
    popup.count = 1;
    {
        static const char *const rows[] = {"Default", "Track", "Artist", "Album", "Rating"};
        uintptr_t *lab = calloc(8, sizeof *lab);
        int32_t nl = 5;
        for (int i = 0; i < 5; i++)
            lab[i] = (uintptr_t) strdup(rows[i]);
        uintptr_t lp = (uintptr_t) lab;
        memcpy((unsigned char *) popfull + 0x60, &lp, 8);
        memcpy((unsigned char *) popfull + 0x70, &nl, 4);
    }
    scrkids[0] = (uintptr_t) &popup;
    screen.children = (uintptr_t) scrkids;
    screen.count = 0; /* EP147 builds the popup after the click */
    g_scl.cur = 4;
    bf_set_field(3, B2_GENRE);
    __atomic_store_n(&g_b2_layout, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_b2_refresh, 0, __ATOMIC_RELEASE);
    b2_lis_click(&g_b2_lis[slot][3], 0);
    CHECK("our button's click: the move asked (our right less EP147's, the column's field's row)",
          g_b2_pop.hdr == h && g_b2_pop.dx == 340 - 1180 && g_b2_pop.own_r == 1180 && g_b2_pop.row == 5);
    g_select_n = 0;
    bf_tick();
    CHECK("no popup yet: nothing moved, asked again at the next tick",
          g_b2_pop.hdr == h && popup.x == 0 && g_select_n == 0);
    advance(B2_POP_MS + 1);
    bf_tick();
    CHECK("no popup within B2_POP_MS: the ask dropped", g_b2_pop.hdr == 0 && popup.x == 0);
    screen.count = 1;
    b2_lis_click(&g_b2_lis[slot][4], 0); /* BPM, showing its own data */
    bf_tick();
    CHECK("the BPM list: the popup moved by our button's offset, wider by as much, DEFAULT selected",
          popup.x == -390 && popup.w == 1280 + 390 && popup.y == 0 && popup.h == 800 && g_select_n == 1 &&
              g_select_lb == (uintptr_t) &plist && g_select_row == 0 && g_b2_pop.hdr == 0);
    b2_lis_click(&g_b2_lis[slot][3], 0);
    bf_tick();
    CHECK("the # list next: moved from EP147's own place, not from the last move; kept at the header's left edge",
          popup.x == -800 && popup.w == 1280 + 800 && g_select_row == 5 && g_select_n == 2);
    b2_lis_click(&g_b2_lis[slot][0], 0);
    CHECK("EP147's own button: the popup back where EP147 put it, EP147's row selected",
          popup.x == 0 && popup.w == 1280 && g_select_n == 3 && g_select_row == 4 && !wref_get(&g_b2_popw[0]));

    /* ---- Hide Column: one more row in the field list ---- */
    {
        int32_t pn = 0;
        uintptr_t pdata = 0;
#define POPN() (memcpy(&pn, (unsigned char *) popfull + 0x70, 4), pn)
#define POPLABEL(i) (memcpy(&pdata, (unsigned char *) popfull + 0x60, 8), ((const char **) pdata)[i])
        CHECK("EP147's own list: its five rows only, ours taken off", POPN() == 5);
        g_pop_updates = g_pop_adds = 0;
        g_repaints = 0;
        b2_lis_click(&g_b2_lis[slot][4], 0); /* BPM */
        CHECK("BPM's list: Hide Column added at the click (before EP147 shows the list), as its last row",
              POPN() == 6 && !strcmp(POPLABEL(5), "Hide Column") && g_pop_adds == 1);
        CHECK("... the list's content updated and the popup repainted", g_pop_updates == 1 && g_repaints == 1);
        bf_tick();
        CHECK("... the tick after: nothing more", POPN() == 6 && g_pop_adds == 1 && g_pop_updates == 1);
        b2_lis_click(&g_b2_lis[slot][4], 0);
        bf_tick();
        CHECK("... opened again: added once", POPN() == 6 && g_pop_adds == 1 && g_pop_updates == 1);
        b2_lis_click(&g_b2_lis[slot][1], 0); /* Preview: not a column that can be hidden */
        CHECK("Preview's list: the row taken off at the click", POPN() == 5 && g_pop_updates == 2);
        bf_tick();
        CHECK("... and not added back", POPN() == 5 && g_pop_adds == 1);
        b2_lis_click(&g_b2_lis[slot][6], 0); /* the title */
        bf_tick();
        CHECK("the title's list: no row", POPN() == 5 && g_pop_adds == 1);
        b2_lis_click(&g_b2_lis[slot][3], 0); /* # */
        CHECK("#'s list, the popup followed: added at the click", POPN() == 6 && g_pop_adds == 2);
        void (*pclick)(long, int) = (void (*)(long, int)) g_popmodel_vt[6];
        long pmodel = (long) ((uintptr_t) popfull + 0x30);
        CHECK("the list model's row tap hooked", pclick == b2_pop_click && g_o_b2_pop_click == fake_pop_click);
        g_pop_click_n = 0;
        pclick(pmodel, 2);
        CHECK("a tap on a field's row: EP147's, with its row", g_pop_click_n == 1 && g_pop_click_row == 2);
        b2_lis_click(&g_b2_lis[slot][3], 0);
        g_mask = AZMOD_BCOL_DEFAULT;
        g_mask_sets = g_pop_click_n = 0;
        pclick(pmodel, 5);
        CHECK("a tap on Hide Column: the popup closed, no row passed on", g_pop_click_n == 1 && g_pop_click_row == -1);
        CHECK("... # hidden, as set by the DJ", g_mask_sets == 1 && g_mask == (AZMOD_BCOL_DEFAULT | AZMOD_BCOL_SET(AZMOD_BCOL_NUM)));
        CHECK("... the column's ask used up: no pick follows as #'s", !bf_take_pick(5) && field_now(3) == B2_GENRE);
        b2_lis_click(&g_b2_lis[slot][3], 0);
        g_select_n = 0;
        bf_tick();
        CHECK("a hidden column's list: Hide Column marked", g_select_n == 1 && g_select_row == 5);
        g_mask_sets = 0;
        CHECK("a field picked for the hidden column (Artist): taken", bf_take_pick(2) == 1);
        CHECK("... the column shows it, and is shown again",
              field_now(3) == B2_ARTIST && g_mask_sets == 1 &&
                  g_mask == (AZMOD_BCOL_DEFAULT | AZMOD_BCOL_NUM | AZMOD_BCOL_SET(AZMOD_BCOL_NUM)));
        b2_lis_click(&g_b2_lis[slot][3], 0);
        g_mask_sets = 0;
        bf_take_pick(5); /* Genre, on a shown column */
        CHECK("a pick on a shown column: its columns unchanged", field_now(3) == B2_GENRE && g_mask_sets == 0);
        b2_lis_click(&g_b2_lis[slot][3], 0);
        CHECK("the row still there", POPN() == 6);
        char *own = (char *) POPLABEL(5);
        ((const char **) pdata)[5] = "Genre"; /* EP147 rebuilt its rows: that index is one of its own */
        g_pop_click_n = g_mask_sets = 0;
        pclick(pmodel, 5);
        CHECK("rows EP147 rebuilt: a tap on that index is EP147's", g_pop_click_row == 5 && g_mask_sets == 0);
        ((const char **) pdata)[5] = own;
        b2_lis_click(&g_b2_lis[slot][3], 0);
        b2_lis_click(&g_b2_lis[slot][0], 0);
        CHECK("EP147's own button: Hide Column taken off", POPN() == 5);
        g_b2_hide_ok = 0;
        b2_lis_click(&g_b2_lis[slot][3], 0);
        bf_tick();
        CHECK("without the tap hook: no row", POPN() == 5);
        g_b2_hide_ok = 1;
        b2_lis_click(&g_b2_lis[slot][0], 0);
        g_mask = AZMOD_BCOL_DEFAULT;
#undef POPN
#undef POPLABEL
#undef popup
    }

    /* ---- a press on a field button ---- */
    bf_after_layout(h, 1, 10);
    uintptr_t btn3 = e->btn[2];
    static uint64_t bev[0x80 / 8];
    int ex = 5;
    memcpy((unsigned char *) bev + 8, &ex, 4);
    void (*bdown)(long, long) = (void (*)(long, long)) g_btn_vt[5], (*bdrag)(long, long) = (void (*)(long, long)) g_btn_vt[6],
                        (*bup)(long, long) = (void (*)(long, long)) g_btn_vt[7];
    CHECK("the button class's mouse slots patched once, the originals kept",
          g_b2_btn_ok && bdown == b2_btn_down_hook && bdrag == b2_btn_drag_hook && bup == b2_btn_up_hook &&
              g_o_b2_btn_mouse[0] == fake_btn_down && g_o_b2_btn_mouse[2] == fake_btn_up);
    g_be_take = 1;
    g_be_moved = 1;
    memset(g_btn_mouse_n, 0, sizeof g_btn_mouse_n);
    opened = g_list_opened;
    bdown((long) btn3, (long) bev);
    CHECK("a press on ours within a divider's reach: the header's, at its header x",
          g_be_press_n == 1 && g_be_press_x == 260 + 5 && g_be_h == h && g_btn_mouse_n[0] == 0);
    ex = 9;
    memcpy((unsigned char *) bev + 8, &ex, 4);
    bdrag((long) btn3, (long) bev);
    bup((long) btn3, (long) bev);
    CHECK("... its drag and release the header's too; a release that moved opens nothing",
          g_be_drag_n == 1 && g_be_drag_x == 269 && g_be_release_n == 1 && g_btn_mouse_n[1] == 0 &&
              g_btn_mouse_n[2] == 0 && g_list_opened == opened);
    ex = 5;
    memcpy((unsigned char *) bev + 8, &ex, 4);
    bdown((long) btn3, (long) bev);
    ex = -40; /* the touch moved left, off the button: its coordinates are negative */
    memcpy((unsigned char *) bev + 8, &ex, 4);
    bdrag((long) btn3, (long) bev);
    bup((long) btn3, (long) bev);
    CHECK("a drag that leaves the button to its left: still the header's, at header x 220, and its release",
          g_be_drag_n == 2 && g_be_drag_x == 260 - 40 && g_be_release_n == 2 && g_btn_mouse_n[1] == 0 &&
              g_btn_mouse_n[2] == 0 && g_list_opened == opened);
    ex = 9;
    memcpy((unsigned char *) bev + 8, &ex, 4);
    g_be_moved = 0;
    __atomic_store_n(&g_bf_ask, 0, __ATOMIC_RELEASE);
    bdown((long) btn3, (long) bev);
    bup((long) btn3, (long) bev);
    CHECK("a still release: the column's list opens, as the button's click",
          g_be_release_n == 3 && g_list_opened == opened + 1 && __atomic_load_n(&g_bf_ask, __ATOMIC_ACQUIRE) == 3);
    bf_take_pick(0);
    g_be_take = 0;
    __atomic_store_n(&g_bf_ask, 0, __ATOMIC_RELEASE);
    opened = g_list_opened;
    bdown((long) btn3, (long) bev);
    CHECK("a press no divider takes: nothing opens on the press, EP147's own mouse not called",
          g_be_press_n == 4 && g_list_opened == opened && g_btn_mouse_n[0] == 0);
    advance(100);
    bup((long) btn3, (long) bev);
    CHECK("... the tap opens the column's list on the lift", g_list_opened == opened + 1 &&
                                                                 __atomic_load_n(&g_bf_ask, __ATOMIC_ACQUIRE) == 3 && g_btn_mouse_n[2] == 0 && g_be_release_n == 3);
    bf_take_pick(0);
    opened = g_list_opened;
    ex = 5;
    memcpy((unsigned char *) bev + 8, &ex, 4);
    bdown((long) btn3, (long) bev);
    ex = 40;
    memcpy((unsigned char *) bev + 8, &ex, 4);
    bdrag((long) btn3, (long) bev);
    bup((long) btn3, (long) bev);
    CHECK("a touch that moves on the button: nothing opens", g_list_opened == opened && g_btn_mouse_n[1] == 0);
    ex = 5;
    memcpy((unsigned char *) bev + 8, &ex, 4);
    bdown((long) btn3, (long) bev);
    advance(HT_TAP_MAX_MS + 1);
    bup((long) btn3, (long) bev);
    CHECK("a press held past a tap: nothing opens", g_list_opened == opened);
    bdown((long) btn3, (long) bev);
    advance(TOUCH_SEQ_GAP_MS + 50);
    ex = 60;
    memcpy((unsigned char *) bev + 8, &ex, 4);
    bdrag((long) btn3, (long) bev);
    bup((long) btn3, (long) bev);
    CHECK("a drag after a pause is still the same touch: nothing opens", g_list_opened == opened);
    b2_gest(0, 400, 20); /* a touch that starts on the header (browse_edit.c feeds its events) */
    b2_gest(1, 300, 20);
    ex = 5;
    memcpy((unsigned char *) bev + 8, &ex, 4);
    bdown((long) btn3, (long) bev); /* the deck's press at a step of the move, now on the button */
    bup((long) btn3, (long) bev);
    CHECK("a touch that started on the header and lifts on the button: nothing opens", g_list_opened == opened);
    g_be_take = 1;
    g_be_moved = 1;
    bdown((long) btn3, (long) bev);
    bup((long) btn3, (long) bev);
    CHECK("a still touch the header counts as its drag: nothing opens", g_list_opened == opened);
    g_be_moved = 0;
    CHECK("EP147's mouse never called for ours", g_btn_mouse_n[0] == 0 && g_btn_mouse_n[1] == 0 &&
                                                     g_btn_mouse_n[2] == 0);
    int exits = g_btn_exit_n;
    bdown((long) btn3, (long) bev);
    bup((long) btn3, (long) bev);
    CHECK("... the button's pressed look put back on every lift (its mouseExit)", g_btn_exit_n == exits + 1);
    g_be_take = 1;
    int presses = g_be_press_n;
    bdown((long) ownb, (long) bev);
    bdrag((long) ownb, (long) bev);
    bup((long) ownb, (long) bev);
    CHECK("a press on EP147's own button outside edit mode: never the header's, the button's own",
          g_be_press_n == presses && g_btn_mouse_n[0] == 1 && g_btn_mouse_n[1] == 1 && g_btn_mouse_n[2] == 1);
    g_editing = 1;
    g_be_moved = 1;
    bdown((long) ownb, (long) bev);
    ex = 40;
    memcpy((unsigned char *) bev + 8, &ex, 4);
    bdrag((long) ownb, (long) bev);
    bup((long) ownb, (long) bev);
    CHECK("editing, a press on EP147's own button within a divider's reach: the header's drag, no list",
          g_be_press_n == presses + 1 && g_be_press_x == 1100 + 5 && g_be_drag_n == 3 && g_btn_mouse_n[0] == 1 &&
              g_btn_mouse_n[2] == 1 && g_open_fields_n == 0);
    g_be_moved = 0;
    g_be_take = 0;
    __atomic_store_n(&g_bf_ask, 4, __ATOMIC_RELEASE);
    ex = 5;
    memcpy((unsigned char *) bev + 8, &ex, 4);
    bdown((long) ownb, (long) bev);
    CHECK("editing, a press on EP147's own button no divider takes: nothing on the press",
          g_open_fields_n == 0 && g_btn_mouse_n[0] == 1);
    bup((long) ownb, (long) bev);
    CHECK("... a tap: EP147's list opens on the lift, as EP147's own (no column's ask)",
          g_open_fields_n == 1 && __atomic_load_n(&g_bf_ask, __ATOMIC_ACQUIRE) == 0 && g_btn_mouse_n[2] == 1);
    bdown((long) ownb, (long) bev);
    ex = 60;
    memcpy((unsigned char *) bev + 8, &ex, 4);
    bdrag((long) ownb, (long) bev);
    bup((long) ownb, (long) bev);
    CHECK("... a touch that moves on it: no list", g_open_fields_n == 1);
    g_editing = 0;
    g_be_take = 1;

    /* ---- a touch on Preview ---- */
    static struct comp rowc;
    rowc.vt = ROWCOMP_VT;
    uintptr_t rowp = (uintptr_t) &rowc, wavep = (uintptr_t) wave;
    int wxy[2] = {10, 3};
    memcpy(wave + 0x18, &rowp, 8);
    memcpy(wave + 0x20, wxy, sizeof wxy);
    memcpy(wave + 0xc8, &mp, 8);
    static uint64_t wev[0x80 / 8], wev0[0x80 / 8];
    unsigned char *wb = (unsigned char *) wev;
    float fp[2] = {5.0f, 7.0f};
    int ixy[2] = {5, 7};
    memcpy(wb, fp, sizeof fp);
    memcpy(wb + 8, ixy, sizeof ixy);
    memcpy(wb + 0x28, &wavep, 8);
    memcpy(wb + 0x30, &wavep, 8);
    memcpy(wb + 0x50, fp, sizeof fp);
    memcpy(wev0, wev, sizeof wev);
    void (*wdown)(long, long) = (void (*)(long, long)) g_ph_vt[5], (*wdrag)(long, long) = (void (*)(long, long)) g_ph_vt[6],
                        (*wup)(long, long) = (void (*)(long, long)) g_ph_vt[7];
    bf_set_field(1, B2_ARTIST);
    memset(g_wave_mouse_n, 0, sizeof g_wave_mouse_n);
    wdown((long) wave, (long) wev);
    {
        const unsigned char *rb = (const unsigned char *) g_row_ev;
        float rf[2], rm[2];
        int ri[2];
        uintptr_t c28 = 0, c30 = 0;
        memcpy(rf, rb, sizeof rf);
        memcpy(ri, rb + 8, sizeof ri);
        memcpy(rm, rb + 0x50, sizeof rm);
        memcpy(&c28, rb + 0x28, 8);
        memcpy(&c30, rb + 0x30, 8);
        CHECK("Preview showing a field: no preview, the row's mouseDown instead",
              g_wave_mouse_n[0] == 0 && g_row_mouse_n[0] == 1 && g_row_mouse_self == (long) rowp);
        CHECK("... with the row as the event's components", c28 == rowp && c30 == rowp);
        CHECK("... the tap just right of Preview's column, y by the component's",
              ri[0] == 10 + 160 + 1 && ri[1] == 7 + 3 && rf[0] == 171.0f && rf[1] == 10.0f && rm[0] == 171.0f &&
                  rm[1] == 10.0f);
        CHECK("... the original event untouched", !memcmp(wev, wev0, sizeof wev));
    }
    wdrag((long) wave, (long) wev);
    wup((long) wave, (long) wev);
    CHECK("... its drag and release the row's too", g_row_mouse_n[1] == 1 && g_row_mouse_n[2] == 1 &&
                                                        g_wave_mouse_n[1] == 0 && g_wave_mouse_n[2] == 0);
    bf_set_field(1, B2_NONE);
    wdown((long) wave, (long) wev);
    wup((long) wave, (long) wev);
    CHECK("Preview showing its own data: the preview's own touch, the event as it came",
          g_wave_mouse_n[0] == 1 && g_wave_mouse_n[2] == 1 && g_wave_mouse_e == (long) wev && g_row_mouse_n[0] == 1);
    bf_set_field(1, B2_ARTIST);
    memcpy(wave + 0xc8, &mp2, 8);
    wdown((long) wave, (long) wev);
    CHECK("a compact list's Preview: the preview's own touch", g_wave_mouse_n[0] == 2 && g_row_mouse_n[0] == 1);
    memcpy(wave + 0xc8, &mp, 8);
    memcpy(wave + 0x18, &lbp, 8);
    wdown((long) wave, (long) wev);
    CHECK("a Preview whose parent is not a row: the preview's own touch",
          g_wave_mouse_n[0] == 3 && g_row_mouse_n[0] == 1);
    bf_set_field(1, B2_NONE);

    /* ---- the header goes: our buttons first ---- */
    int dels = g_btn_deleted;
    b2_hdr_dtor0((long) h);
    CHECK("the header's destructor: our five buttons deleted, the entry cleared, EP147's ran",
          g_btn_deleted == dels + BF_NBTN && !b2_of(h) && g_hdr_dtor_n == 1);
    CHECK("... and browse_columns.c told", g_gone == h);

    /* ---- the worker, on its own thread, over two fake sticks ---- */
    b2_lock();
    g_b2q_n = 0; /* the cells' jobs: not the worker's to answer here */
    b2_unlock();
    int fd0 = -1, fd1 = -1;
    make_stick(0, &fd0);
    make_stick(1, &fd1);
    set_stick(0, "4A", 0, 0);
    set_stick(1, 0, 0, 0);
    __atomic_store_n(&g_b2_landed, 0, __ATOMIC_RELEASE);
    pthread_t wt;
    pthread_create(&wt, 0, b2_worker, 0);
    char tx[B2_TEXT_MAX];
    int kk = 0;
    CHECK("one stick holds it: known, its value",
          wait_answer(110, "T10", B2_COMMENTS) == 2 && (ent_state(110, "T10", B2_COMMENTS, tx, 0), !strcmp(tx, "4A")) &&
              __atomic_load_n(&g_b2_landed, __ATOMIC_ACQUIRE) == 1);
    set_stick(1, "4A", 0, 0);
    CHECK("two sticks agree: known", wait_answer(111, "T11", B2_COMMENTS) == 2);
    set_stick(1, "5B", 0, 0);
    CHECK("two sticks disagree: unknown", wait_answer(112, "T12", B2_COMMENTS) == 3);
    set_stick(0, "Pink", 1, 1);
    set_stick(1, "Pink", 1, 1);
    CHECK("a color: known with its dot",
          wait_answer(113, "T13", B2_COLOR) == 2 && (ent_state(113, "T13", B2_COLOR, tx, &kk), kk == 0x18) &&
              !strcmp(tx, "Pink"));
    uintptr_t js = 0;
    CHECK("... and the painter gets it as a string", b2_get(113, "T13", B2_COLOR, &js, &kk) == 1 && js &&
                                                         !strcmp((const char *) js, "Pink") && kk == 0x18);
    CHECK("the connections stay open between rows (one per stick)", __atomic_load_n(&g_open_n, __ATOMIC_ACQUIRE) == 2);
    close(fd1); /* EP147 lets go of stick 1 (an eject) */
    advance(1000);
    set_stick(0, "only0", 0, 0);
    set_stick(1, 0, 0, 0);
    int st = wait_answer(114, "T14", B2_COMMENTS);
    ent_state(114, "T14", B2_COMMENTS, tx, 0);
    CHECK("stick 1 let go of: its connection closed, stick 0 still answers",
          st == 2 && !strcmp(tx, "only0") && __atomic_load_n(&g_close_n, __ATOMIC_ACQUIRE) >= 1);
    __atomic_store_n(&g_b2_stop, 1, __ATOMIC_RELEASE);
    b2_lock();
    pthread_cond_signal(&g_b2_cv);
    b2_unlock();
    pthread_join(wt, 0);
    close(fd0);

    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_media);
    if (system(cmd) != 0)
        fprintf(stderr, "cleanup failed\n");
    unlink(g_bf_path);
    for (int i = 0; i < 4; i++)
        if (g_hdrs[i] && g_hdrs[i] != h) {
            fake_hdr_dtor((long) g_hdrs[i]);
            free((void *) g_hdrs[i]);
        }
    free((void *) h);
    return ut_summary();
}
