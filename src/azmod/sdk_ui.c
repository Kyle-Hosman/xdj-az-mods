/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file sdk_ui.c
 * @brief Community mod services: banner, confirm, post_ui, thread_start
 *
 * "The UI thread" is the overlay thread: it runs the mods' tick, screens, surfaces and row
 * callbacks, and every callback below that a mod gets back (post_ui, the confirm answer).
 *
 * banner: EP147's caution box is message-thread only, so banners go through a bounded queue
 * that EP147's message thread drains from msg_tick. A banner shows at most 111 bytes.
 * post_ui: a second bounded queue, drained on the UI thread from community_tick. Both queues
 * are lock-free on both sides; a full queue refuses and logs, it never waits.
 *
 * confirm: one question at a time, from a mod or from the core (azcm_confirm: the Mod Menu's
 * removal, a handoff). It is drawn as the core's own surface on AZ_LAYER_TOP (sdk_surface.c),
 * over whatever is on screen, the Mod Menu included, and holds the browse knob, its click and
 * BACK while it is up. Cancel is focused first; BACK answers no. Every question that was taken
 * gets exactly one answer on the UI thread; a question asked while another is open is refused
 * (-1) and gets none.
 *
 * thread_start: a normal-priority (SCHED_OTHER) detached thread with an alternate signal stack
 * (azaltstack.h), named so a crash log says whose thread it was; at most 4 running per mod. The
 * core cannot stop a thread: one started by a mod that is later switched off or quarantined
 * runs until it returns, and the core only stops calling that mod's callbacks.
 *
 * #included by azmod.c after community.c.
 */

static struct sdq g_sd_bq;  /* banners: drained on EP147's message thread */
static struct sdq g_sd_pq;  /* post_ui: drained on the UI thread */
static pthread_once_t g_sd_q_once = PTHREAD_ONCE_INIT;
static void sd_q_init(void) {
    sdq_init(&g_sd_bq);
    sdq_init(&g_sd_pq);
}

/* az_core_v1.banner: EP147's caution box with the mod's text (at most 111 bytes) for ms
 * (<= 0 = the stock time). */
static void sd_banner(const char *id, const char *text, int ms) {
    cm_mod *c = cm_self(id, CM_RA(), "banner");
    if (!c || !text || !*text)
        return;
    pthread_once(&g_sd_q_once, sd_q_init);
    struct sd_job j;
    memset(&j, 0, sizeof j);
    j.kind = SDJ_BANNER;
    j.owner = (int) (c - g_cm);
    j.ms = ms;
    snprintf(j.text, sizeof j.text, "%s", text);
    if (sdq_push(&g_sd_bq, &j) != 0)
        cm_logf(c, 1, "banner dropped: too many requests waiting");
}

/* az_core_v1.post_ui: fn(user) on the UI thread soon, from any thread of the mod's. The
 * callback must be the mod's own code. */
static void sd_post_ui(const char *id, void (*fn)(void *user), void *user) {
    cm_mod *c = cm_self(id, CM_RA(), "post_ui");
    if (!c || !fn)
        return;
    if (!c->hi || (uintptr_t) fn < c->lo || (uintptr_t) fn >= c->hi) {
        cm_logf(c, 1, "post_ui refused: the callback is not in this mod's code");
        return;
    }
    pthread_once(&g_sd_q_once, sd_q_init);
    struct sd_job j;
    memset(&j, 0, sizeof j);
    j.kind = SDJ_POST;
    j.owner = (int) (c - g_cm);
    j.fn = fn;
    j.user = user;
    if (sdq_push(&g_sd_pq, &j) != 0)
        cm_logf(c, 1, "post_ui dropped: too many requests waiting");
}

/* Run up to 16 queued jobs: a mod that floods a queue cannot hold the thread draining it. */
static void sd_drain(struct sdq *q) {
    pthread_once(&g_sd_q_once, sd_q_init);
    struct sd_job j;
    for (int k = 0; k < 16 && sdq_pop(q, &j); k++) {
        if (j.owner < 0 || j.owner >= g_cm_n || !cm_running(&g_cm[j.owner]))
            continue; /* the mod was stopped meanwhile */
        if (j.kind == SDJ_BANNER)
            az_banner_box(j.text, NULL, j.ms);
        else if (j.kind == SDJ_POST && j.fn)
            j.fn(j.user);
    }
}
/* EP147's message thread, from msg_tick. */
static void sdk_ui_msg_drain(void) {
    sd_drain(&g_sd_bq);
}

/* ---- confirm ---- */
static pthread_mutex_t g_sd_cmu = PTHREAD_MUTEX_INITIALIZER;
static struct sd_confirm g_sd_confirm; /* under g_sd_cmu */

/* The dialog on screen: UI thread only. */
static struct {
    int up;
    int sel; /* 0 = Cancel, 1 = yes */
    int owner;
    char title[48], text[240], yes[24];
    az_rect r, btn[2];
} g_sd_dlg;

static void sd_deliver(struct sc_answer a) {
    if (!a.fn)
        return;
    if (a.owner == -1 || (a.owner >= 0 && a.owner < g_cm_n && cm_running(&g_cm[a.owner])))
        a.fn(a.yes, a.user);
}

/* The dialog, painted by the core: a title, up to four lines of text and two buttons. */
static void sd_dlg_paint(az_draw *d, az_rect r, void *user) {
    (void) user;
    int sh = d->sh > 0 ? d->sh : 800, pad = sh * 20 / 800;
    int tpx = sh * 24 / 800, px = sh * 18 / 800, lh = px * 3 / 2;
    azcm_draw_rect(d, r.x, r.y, r.w, r.h, 2, azcm_color(AZ_COL_HI));
    int x = r.x + pad, y = r.y + pad, maxw = r.w - 2 * pad;
    azcm_draw_text(d, x, y, g_sd_dlg.title, tpx, azcm_color(AZ_COL_TEXT));
    y += tpx * 3 / 2 + pad / 2;
    const char *p = g_sd_dlg.text;
    for (int lines = 0; *p && lines < 4; lines++) { /* wrap at the last space that fits */
        char line[240];
        int n = 0, cut = 0;
        while (p[n] && p[n] != '\n' && n < (int) sizeof line - 1) {
            line[n] = p[n];
            line[n + 1] = 0;
            if (azcm_text_width(line, px) > maxw)
                break;
            if (p[n] == ' ')
                cut = n;
            n++;
        }
        if (p[n] && p[n] != '\n' && cut > 0)
            n = cut;
        if (n == 0)
            n = 1; /* one character wider than the dialog: show it, never stall */
        line[n] = 0;
        azcm_draw_text(d, x, y, line, px, azcm_color(AZ_COL_TEXT_DIM));
        y += lh;
        p += n;
        while (*p == ' ' || *p == '\n')
            p++;
    }
    const char *lbl[2] = {"Cancel", g_sd_dlg.yes};
    for (int i = 0; i < 2; i++) {
        az_rect b = g_sd_dlg.btn[i];
        int on = g_sd_dlg.sel == i;
        if (on)
            azcm_fill_rect(d, b.x, b.y, b.w, b.h, azcm_color(AZ_COL_HI));
        else
            azcm_draw_rect(d, b.x, b.y, b.w, b.h, 2, azcm_color(AZ_COL_TEXT_DIM));
        int tw = azcm_text_width(lbl[i], px);
        azcm_draw_text(d, b.x + (b.w - tw) / 2, b.y + (b.h - lh) / 2, lbl[i], px,
                       azcm_color(on ? AZ_COL_HI_TEXT : AZ_COL_TEXT));
    }
}

static void sd_dlg_close(int yes);
static int sd_dlg_input(const az_event *ev, void *user) {
    (void) user;
    if (!g_sd_dlg.up)
        return 1;
    int kind = 0, hit = -1;
    switch (ev->kind) {
    case AZ_EV_KNOB: kind = SC_IN_KNOB; break;
    case AZ_EV_CLICK: kind = SC_IN_CLICK; break;
    case AZ_EV_BACK: kind = SC_IN_BACK; break;
    case AZ_EV_TAP:
        kind = SC_IN_TAP;
        for (int i = 0; i < 2; i++) {
            az_rect b = g_sd_dlg.btn[i];
            if (ev->x >= b.x && ev->x < b.x + b.w && ev->y >= b.y && ev->y < b.y + b.h)
                hit = i;
        }
        break;
    default:
        return 1;
    }
    int was = g_sd_dlg.sel, ans = sc_input(&g_sd_dlg.sel, kind, ev->dir, hit);
    if (ans >= 0)
        sd_dlg_close(ans);
    else if (g_sd_dlg.sel != was)
        sdk_core_surface_redraw();
    return 1; /* BACK included: it answers no, and closing gives the knob back */
}

/* Lay the dialog out in the middle of the screen and put it up (UI thread). */
static int sd_dlg_open(void) {
    static const az_surface_v1 desc = {
        .size = sizeof(az_surface_v1), .on_paint = sd_dlg_paint, .on_input = sd_dlg_input};
    sdk_core_surface_init(&desc);
    int sw = g_sdk_ovc ? g_sdk_ovc->scr_w : 1280, sh = g_sdk_ovc ? g_sdk_ovc->scr_h : 800;
    int w = sw * 620 / 1280, h = sh * 330 / 800, pad = sh * 20 / 800;
    az_rect r = {(sw - w) / 2, (sh - h) / 2, w, h};
    int bw = sw * 200 / 1280, bh = sh * 52 / 800, by = r.y + h - pad - bh;
    g_sd_dlg.r = r;
    g_sd_dlg.btn[0] = (az_rect) {r.x + w - 2 * bw - 2 * pad, by, bw, bh}; /* Cancel */
    g_sd_dlg.btn[1] = (az_rect) {r.x + w - bw - pad, by, bw, bh};         /* yes */
    g_sd_dlg.sel = 0; /* Cancel first: a stray click never says yes */
    if (sdk_core_surface_show(r, 1) != 0)
        return -1;
    g_sd_dlg.up = 1;
    return 0;
}

/* End the question on screen with this answer (UI thread). */
static void sd_dlg_close(int yes) {
    if (g_sd_dlg.up) {
        g_sd_dlg.up = 0;
        sdk_core_surface_show(g_sd_dlg.r, 0);
    }
    pthread_mutex_lock(&g_sd_cmu);
    struct sc_answer a = sc_answer(&g_sd_confirm, yes);
    pthread_mutex_unlock(&g_sd_cmu);
    sd_deliver(a);
}

static int sd_confirm_take(int owner, const char *title, const char *text, const char *yes,
                           void (*fn)(int, void *), void *user) {
    pthread_mutex_lock(&g_sd_cmu);
    int rc = sc_request(&g_sd_confirm, owner, title, text, yes, fn, user);
    pthread_mutex_unlock(&g_sd_cmu);
    return rc;
}

/* az_core_v1.confirm: 0 = the question is taken (shown at the next UI tick), -1 = refused. */
static int sd_confirm(const char *id, const char *title, const char *text, const char *yes,
                      void (*fn)(int yes, void *user), void *user) {
    cm_mod *c = cm_self(id, CM_RA(), "confirm");
    if (!c || !fn)
        return -1;
    if (!c->hi || (uintptr_t) fn < c->lo || (uintptr_t) fn >= c->hi) {
        cm_logf(c, 1, "confirm refused: the callback is not in this mod's code");
        return -1;
    }
    if (sd_confirm_take((int) (c - g_cm), title, text, yes, fn, user) != 0) {
        cm_logf(c, 1, "confirm refused: another question is open");
        return -1;
    }
    return 0;
}

/* The core's own questions (menu.c, the handoff), through the same dialog. */
int azcm_confirm(const char *title, const char *text, const char *yes,
                 void (*fn)(int yes, void *user), void *user) {
    return sd_confirm_take(-1, title, text, yes, fn, user);
}

/* UI thread, at the top of every community_tick: show a waiting question. The core asks its own
 * (the SSH key approval, a mod's removal) through this dialog too, so it is serviced whether or
 * not any community mod is loaded or running. */
static void sdk_confirm_tick(void) {
    struct sc_answer a;
    pthread_mutex_lock(&g_sd_cmu);
    int show = sc_tick(&g_sd_confirm, sdk_core_can_show(), &a);
    if (show) {
        g_sd_dlg.owner = g_sd_confirm.owner;
        snprintf(g_sd_dlg.title, sizeof g_sd_dlg.title, "%s", g_sd_confirm.title);
        snprintf(g_sd_dlg.text, sizeof g_sd_dlg.text, "%s", g_sd_confirm.text);
        snprintf(g_sd_dlg.yes, sizeof g_sd_dlg.yes, "%s", g_sd_confirm.yes);
    }
    pthread_mutex_unlock(&g_sd_cmu);
    sd_deliver(a);
    if (show && sd_dlg_open() != 0)
        sd_dlg_close(0); /* could not be put up: no */
    /* the asking mod was switched off under its question: the dialog goes, no answer is run */
    if (g_sd_dlg.up && g_sd_dlg.owner >= 0 &&
        (g_sd_dlg.owner >= g_cm_n || !cm_running(&g_cm[g_sd_dlg.owner])))
        sd_dlg_close(0);
}
/* UI thread, from community_tick while mods run: their post_ui callbacks. */
static void sdk_ui_tick(void) {
    sd_drain(&g_sd_pq);
}

/* ---- thread_start ---- */
static _Atomic int g_sd_threads[CM_MAX]; /* running threads per mod (at most SD_MAX_THREADS) */
static int sd_threads_of(int owner) { return atomic_load(&g_sd_threads[owner]); }
struct sd_start {
    void *(*fn)(void *);
    void *arg;
    int owner;
    char name[16];
};
static void *sd_thread_entry(void *p) {
    struct sd_start s = *(struct sd_start *) p;
    free(p);
    pthread_setname_np(pthread_self(), s.name);
    void *r = s.fn(s.arg);
    atomic_fetch_sub_explicit(&g_sd_threads[s.owner], 1, memory_order_relaxed);
    return r;
}

/* az_core_v1.thread_start: 0 = started. */
static int sd_thread_start(const char *id, const char *name, void *(*fn)(void *), void *arg) {
    cm_mod *c = cm_self(id, CM_RA(), "thread_start");
    if (!c || !fn)
        return -1;
    int owner = (int) (c - g_cm);
    int loaded = cm_running(c) || owner == g_cm_loading;
    /* the slot is claimed before the check, so two of the mod's threads asking at once can
     * never both pass with one slot left */
    int before = atomic_fetch_add_explicit(&g_sd_threads[owner], 1, memory_order_relaxed);
    int why = sd_thread_check(g_cm_in_frame, loaded, (uintptr_t) fn, c->lo, c->hi, before);
    if (why != SDT_OK) {
        atomic_fetch_sub_explicit(&g_sd_threads[owner], 1, memory_order_relaxed);
        static const char *const r[] = {"", "from on_frame", "the mod is not loaded",
                                        "the entry point is not in this mod's code",
                                        "too many threads (at most 4 running)"};
        cm_logf(c, 1, "thread_start refused: %s", r[why]);
        return -1;
    }
    struct sd_start *s = malloc(sizeof *s);
    if (!s) {
        atomic_fetch_sub_explicit(&g_sd_threads[owner], 1, memory_order_relaxed);
        return -1;
    }
    s->fn = fn;
    s->arg = arg;
    s->owner = owner;
    snprintf(s->name, sizeof s->name, "%s", name && *name ? name : c->id);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED); /* never EP147's real-time class */
    pthread_attr_setschedpolicy(&at, SCHED_OTHER);
    struct sched_param sp = {0};
    pthread_attr_setschedparam(&at, &sp);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    int rc = az_thread_create(&t, &at, sd_thread_entry, s);
    pthread_attr_destroy(&at);
    if (rc) {
        atomic_fetch_sub_explicit(&g_sd_threads[owner], 1, memory_order_relaxed);
        free(s);
        cm_logf(c, 1, "thread_start failed: %s", strerror(rc));
        return -1;
    }
    return 0;
}
