/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file sdk_surface.c
 * @brief The community-mod drawing services (azmod-mod.h, "added in core 1.2.3"): view(),
 *        surfaces, another program's window, images and lines, and pointer injection.
 *
 * Included by community.c, after the mod records it needs and before the service table. All
 * X11 work happens on the overlay thread, on the overlay's own display connection: a mod's
 * calls only record what it wants, and the overlay's stages (mod_menu_overlay.c) apply it,
 * place and stack the windows, and repaint them.
 *
 * Stacking, bottom to top: EP147, AZ_LAYER_OVER_EP147 surfaces, the core's own windows (the Mod
 * Menu, stem strips and lanes, readouts), foreign windows shown with xwin_show, and
 * AZ_LAYER_TOP surfaces.
 *
 * Each surface paints into its own buffer, so a mod can never draw over the Mod Menu or another
 * mod's surface. A surface is shown only while its mod is loaded and EP147 is on one of the
 * surface's views. Calls from the wrong thread or with bad arguments are refused with a line
 * in the mod's log; nothing here can block the bus thread.
 *
 * Another program's window can vanish at any moment, and Xlib's default reaction to an error
 * is to end the process. The first time a mod uses a foreign window, sdk_xerr is installed: it
 * absorbs errors on the overlay's connection while a foreign-window call is in flight and
 * passes everything else to the handler that was there before (EP147's).
 */
#include <X11/Xatom.h>
#include "sdk_surface_logic.h"

/* the overlay thread and its context (set by sdk_overlay_attach) */
static pthread_t g_sdk_ov_tid;
static int g_sdk_ov_tid_set;
static struct overlay_ctx *g_sdk_ovc;

static _Atomic int g_sdk_view; /* AZ_VIEW_*, published every overlay tick */

struct az_surface {
    int used, mod;           /* mod: index in g_cm */
    az_surface_v1 d;         /* the caller's descriptor, at this core's size */
    az_rect r;               /* where the mod placed it (clamped) */
    int show;                /* the mod wants it shown */
    int redraw;              /* repaint before the next present */
    struct ov_window w;      /* its X window (created on the overlay thread) */
    uint8_t *buf;            /* its pixels, w.w * w.h * 4 */
    XImage *img;             /* wraps buf */
    int bw, bh;              /* buf's size */
    struct sdk_press press;  /* the touch in progress */
};
/* The mods' surfaces, then one of the core's own (SDK_SURF_CORE: the confirm dialog). A mod's
 * handle is range-checked against SDK_SURF_MAX, so no mod can reach the core's. */
#define SDK_SURF_CORE SDK_SURF_MAX
#define SDK_SURF_ALL (SDK_SURF_MAX + 1)
#define SDK_MOD_CORE (-2) /* the owner of the core's surface: always live */
static struct az_surface g_sdk_surf[SDK_SURF_ALL];
static struct az_surface *g_sdk_focus_surf; /* has the browse knob (overlay thread) */

struct sdk_xwin {
    int used, mod, on;
    unsigned long xid;
    az_rect r;
    int keep;   /* r has no size: the window keeps its own, only its position is set */
    int raised; /* stacked above the core's windows since it was last shown */
};
static struct sdk_xwin g_sdk_xwin[SDK_XWIN_MAX];

static struct sdk_pq g_sdk_pq;
static _Atomic int g_sdk_pq_ready; /* the queue is set up and XTest can deliver it */
static _Atomic int g_sdk_cursor_req = -1; /* -1 none, 0 hide, 1 show */

/* ---- checks ------------------------------------------------------------------------------ */

static int sdk_on_overlay(void) {
    return g_sdk_ov_tid_set && pthread_equal(pthread_self(), g_sdk_ov_tid);
}
static int sdk_mod_live(int mod) {
    return mod == SDK_MOD_CORE || (mod >= 0 && mod < g_cm_n &&
                                   (cm_running(&g_cm[mod]) || mod == g_cm_loading));
}
/* The calling mod, refused (and logged once per service) unless it is this thread's. */
static cm_mod *sdk_self(const char *id, uintptr_t ra, const char *what, int overlay_only) {
    cm_mod *c = cm_self(id, ra, what);
    if (!c)
        return NULL;
    if (overlay_only && !sdk_on_overlay()) {
        cm_logf(c, 1, "%s refused: call it from init(), tick() or a screen or surface callback",
                what);
        return NULL;
    }
    return c;
}
static struct az_surface *sdk_surf_of(az_surface *s, const char *what) {
    struct az_surface *p = (struct az_surface *) s;
    if (!p || p < g_sdk_surf || p >= g_sdk_surf + SDK_SURF_MAX ||
        ((uintptr_t) p - (uintptr_t) g_sdk_surf) % sizeof *p || !p->used) {
        dbg("sdk: %s with a handle that is not a surface\n", what);
        return NULL;
    }
    if (!sdk_on_overlay()) {
        cm_logf(&g_cm[p->mod], 1, "%s refused: call it from tick() or a surface callback", what);
        return NULL;
    }
    return p;
}

/* ---- X errors from other programs' windows ------------------------------------------------ */

static int (*g_sdk_prev_xerr)(Display *, XErrorEvent *);
static int g_sdk_xerr_set;
static volatile int g_sdk_xerr_inflight, g_sdk_xerr;
static int sdk_xerr(Display *d, XErrorEvent *e) {
    if (g_sdk_ovc && d == g_sdk_ovc->dpy && g_sdk_xerr_inflight) {
        g_sdk_xerr = e->error_code ? e->error_code : 1;
        return 0;
    }
    return g_sdk_prev_xerr ? g_sdk_prev_xerr(d, e) : 0;
}
/* Bracket every call that touches another program's window. */
static void sdk_xwin_begin(void) {
    if (!g_sdk_xerr_set) {
        g_sdk_prev_xerr = XSetErrorHandler(sdk_xerr);
        g_sdk_xerr_set = 1;
    }
    g_sdk_xerr = 0;
    g_sdk_xerr_inflight = 1;
}
/* 0 = the calls since sdk_xwin_begin went through */
static int sdk_xwin_end(Display *dpy) {
    XSync(dpy, False);
    g_sdk_xerr_inflight = 0;
    return g_sdk_xerr;
}

/* ---- view ------------------------------------------------------------------------------------ */

static int sdk_view(void) {
    return __atomic_load_n(&g_sdk_view, __ATOMIC_RELAXED);
}

/* ---- surfaces: the services ------------------------------------------------------------------ */

static az_surface *sdk_surface_create(const char *id, const az_surface_v1 *s) {
    cm_mod *c = sdk_self(id, CM_RA(), "surface_create", 1);
    if (!c)
        return NULL;
    __atomic_store_n(&c->sticky, 1, __ATOMIC_RELAXED); /* lasts until the next start: no live stop */
    int mod = (int) (c - g_cm);
    if (mod != g_cm_loading) {
        cm_logf(c, 1, "surface_create refused: only from init()");
        return NULL;
    }
    if (!sdk_surface_desc_ok(s)) {
        cm_logf(c, 1, "surface_create refused: the descriptor needs size, a layer, on_paint and "
                      "at least one view");
        return NULL;
    }
    int used[SDK_SURF_MAX], owner[SDK_SURF_MAX];
    for (int i = 0; i < SDK_SURF_MAX; i++) {
        used[i] = g_sdk_surf[i].used;
        owner[i] = g_sdk_surf[i].mod;
    }
    int k = sdk_surface_slot(used, owner, mod);
    if (k < 0) {
        if (k == -1)
            cm_logf(c, 1, "surface_create refused: a mod may have %d surfaces", SDK_SURF_PER_MOD);
        else
            cm_logf(c, 1, "surface_create refused: all %d surface slots are taken", SDK_SURF_MAX);
        return NULL;
    }
    struct az_surface *p = &g_sdk_surf[k];
    memset(p, 0, sizeof *p);
    p->used = 1;
    p->mod = mod;
    sdk_surface_desc_copy(&p->d, s);
    p->r = (az_rect) {0, 0, 1, 1};
    ov_window_init(&p->w);
    return (az_surface *) p;
}
static void sdk_surface_place(az_surface *s, az_rect r) {
    struct az_surface *p = sdk_surf_of(s, "surface_place");
    if (!p)
        return;
    int sw = g_sdk_ovc ? g_sdk_ovc->scr_w : SDK_SURF_MAX_PX;
    int sh = g_sdk_ovc ? g_sdk_ovc->scr_h : SDK_SURF_MAX_PY;
    az_rect nr = sdk_rect_clamp(r, sw, sh);
    if (nr.x != p->r.x || nr.y != p->r.y || nr.w != p->r.w || nr.h != p->r.h) {
        p->r = nr;
        p->redraw = 1;
    }
}
static void sdk_surface_show(az_surface *s, int on) {
    struct az_surface *p = sdk_surf_of(s, "surface_show");
    if (!p)
        return;
    if (p->show != (on != 0))
        p->redraw = 1;
    p->show = on != 0;
    if (!p->show && g_sdk_focus_surf == p) {
        g_sdk_focus_surf = NULL;
        __atomic_store_n(&g_sdk_focus, 0, __ATOMIC_RELEASE);
    }
}
static void sdk_surface_redraw(az_surface *s) {
    struct az_surface *p = sdk_surf_of(s, "surface_redraw");
    if (p)
        p->redraw = 1;
}
/* Focus = the browse knob and its click, held from EP147 the way the docked Mod Menu holds
 * them (the bus thread freezes the list and swallows the click and BACK while g_sdk_focus).
 * One surface at a time; BACK that its on_input does not handle gives the knob back. */
static int sdk_surface_focus(az_surface *s, int on) {
    struct az_surface *p = sdk_surf_of(s, "surface_focus");
    if (!p)
        return 0;
    if (!on) {
        if (g_sdk_focus_surf == p) {
            g_sdk_focus_surf = NULL;
            __atomic_store_n(&g_sdk_focus, 0, __ATOMIC_RELEASE);
        }
        return 1;
    }
    if (!p->show || !p->d.on_input) {
        cm_logf(&g_cm[p->mod], 1, "surface_focus refused: the surface is %s",
                !p->show ? "hidden" : "without on_input");
        return 0;
    }
    if (g_state.mode != AZ_CLOSED)
        return 0; /* the Mod Menu has the knob while it is open */
    if (g_sdk_focus_surf && g_sdk_focus_surf != p)
        return 0; /* another surface has it until it gives it back */
    g_sdk_focus_surf = p;
    __atomic_exchange_n(&g_enc_dir, 0, __ATOMIC_ACQ_REL); /* no turn from before the focus */
    __atomic_store_n(&g_sdk_back_pending, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_sdk_focus, 1, __ATOMIC_RELEASE);
    return 1;
}

/* ---- drawing helpers ----------------------------------------------------------------------- */

static int sdk_draw_image(az_draw *d, int x, int y, const unsigned char *rgba, int w, int h) {
    if (!rgba || !sdk_on_overlay() || !cm_paint_live(d, "draw_image"))
        return -1;
    /* in 64 bits, then held to the drawing helpers' range: x - ox must not overflow */
    long long lx = (long long) x - d->ox, ly = (long long) y - d->oy;
    lx = lx < -32768 ? -32768 : lx > 32767 ? 32767
                                           : lx;
    ly = ly < -32768 ? -32768 : ly > 32767 ? 32767
                                           : ly;
    sdk_blend_image(d->fb.px, d->fb.w, d->fb.h, d->fb.stride, (int) lx, (int) ly, rgba, w, h);
    return 0;
}
struct sdk_line_ctx {
    az_draw *d;
    uint32_t rgb;
};
static void sdk_line_plot(int x, int y, int size, void *user) {
    struct sdk_line_ctx *l = user;
    azcm_fill_rect(l->d, x, y, size, size, l->rgb);
}
static void sdk_draw_line(az_draw *d, int x0, int y0, int x1, int y1, int width, uint32_t rgb) {
    if (!sdk_on_overlay() || !cm_paint_live(d, "draw_line"))
        return;
    struct sdk_line_ctx l = {d, rgb};
    sdk_line_walk(x0, y0, x1, y1, width, sdk_line_plot, &l);
}

/* ---- pointer --------------------------------------------------------------------------------- */

static int (*p_xtest_motion)(Display *, int, int, int, unsigned long);
static int (*p_xtest_button)(Display *, unsigned int, int, unsigned long);
static int sdk_xtest_load(void) {
    static int tried;
    if (p_xtest_motion && p_xtest_button)
        return 1;
    if (tried)
        return 0;
    tried = 1;
    void *h = dlopen("libXtst.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!h)
        return 0;
    *(void **) &p_xtest_motion = dlsym(h, "XTestFakeMotionEvent");
    *(void **) &p_xtest_button = dlsym(h, "XTestFakeButtonEvent");
    return p_xtest_motion && p_xtest_button;
}
/* Any thread (a touch filter runs on the bus thread): only a queue post, no system call. */
static int sdk_pointer_inject(int x, int y, int button, int down) {
    if (!__atomic_load_n(&g_sdk_pq_ready, __ATOMIC_ACQUIRE) || button < 0 || button > 3)
        return -1;
    struct sdk_pev ev = {x, y, button, down != 0};
    return sdk_pq_post(&g_sdk_pq, &ev) ? 0 : -1;
}
static void sdk_pointer_cursor(int visible) {
    __atomic_store_n(&g_sdk_cursor_req, visible ? 1 : 0, __ATOMIC_RELEASE);
}
static void sdk_cursor_apply(Display *dpy) {
    static int tried, hidden;
    static void (*hide_fn)(Display *, Window), (*show_fn)(Display *, Window);
    int want = __atomic_exchange_n(&g_sdk_cursor_req, -1, __ATOMIC_ACQ_REL);
    if (want < 0)
        return;
    if (!tried) {
        tried = 1;
        void *h = dlopen("libXfixes.so.3", RTLD_NOW | RTLD_LOCAL);
        if (h) {
            *(void **) &hide_fn = dlsym(h, "XFixesHideCursor");
            *(void **) &show_fn = dlsym(h, "XFixesShowCursor");
        }
    }
    if (!hide_fn || !show_fn || (want == 0) == hidden)
        return;
    (want ? show_fn : hide_fn)(dpy, DefaultRootWindow(dpy));
    hidden = !want;
}
/* Overlay thread: deliver the queued pointer events. The queue only opens once XTest has
 * loaded (sdk_overlay_attach), so pointer_inject refuses on a deck without libXtst. */
static void sdk_pointer_drain(Display *dpy) {
    if (!__atomic_load_n(&g_sdk_pq_ready, __ATOMIC_ACQUIRE))
        return;
    struct sdk_pev ev;
    int n = 0;
    while (n < SDK_PQ_N && sdk_pq_take(&g_sdk_pq, &ev)) {
        p_xtest_motion(dpy, -1, ev.x, ev.y, 0);
        if (ev.button)
            p_xtest_button(dpy, (unsigned) ev.button, ev.down ? True : False, 0);
        n++;
    }
}

/* ---- foreign windows ----------------------------------------------------------------------- */

static int sdk_xwin_owned(const char *id, unsigned long xid);
static int sdk_xwin_show(const char *id, unsigned long xid, az_rect r, int on) {
    cm_mod *c = sdk_self(id, CM_RA(), "xwin_show", 1);
    if (!c || !xid)
        return -1;
    __atomic_store_n(&c->sticky, 1, __ATOMIC_RELAXED); /* lasts until the next start: no live stop */
    int mod = (int) (c - g_cm), noop;
    struct sdk_xw_ent t[SDK_XWIN_MAX];
    for (int i = 0; i < SDK_XWIN_MAX; i++)
        t[i] = (struct sdk_xw_ent) {g_sdk_xwin[i].used, g_sdk_xwin[i].mod, g_sdk_xwin[i].xid};
    /* a window that is not shown yet must belong to a program this mod launched: a wrong xid
     * must never move, resize or lower EP147's own windows */
    int shown = 0;
    for (int i = 0; i < SDK_XWIN_MAX; i++)
        shown |= t[i].used && t[i].xid == xid;
    int owned = shown || !on || sdk_xwin_owned(c->id, xid);
    int k = sdk_xwin_admit(t, SDK_XWIN_MAX, mod, xid, on, owned, &noop);
    if (noop)
        return 0;
    if (k < 0) {
        if (k == SDK_XW_OTHER)
            cm_logf(c, 1, "xwin_show refused: another mod shows window 0x%lx", xid);
        else if (k == SDK_XW_NOTOWNED)
            cm_logf(c, 1, "xwin_show refused: window 0x%lx is not a program this mod launched",
                    xid);
        else
            cm_logf(c, 1, "xwin_show refused: %d foreign windows are shown already",
                    SDK_XWIN_MAX);
        return -1;
    }
    struct sdk_xwin *w = &g_sdk_xwin[k];
    int sw = g_sdk_ovc ? g_sdk_ovc->scr_w : SDK_SURF_MAX_PX;
    int sh = g_sdk_ovc ? g_sdk_ovc->scr_h : SDK_SURF_MAX_PY;
    w->used = 1;
    w->mod = mod;
    w->xid = xid;
    w->r = sdk_xwin_rect(r, sw, sh, &w->keep);
    if (on && !w->on)
        w->raised = 0;
    w->on = on != 0;
    return 0;
}
/* Is this top-level window owned by `pid` (its _NET_WM_PID)? Windows that do not say are not. */
static int sdk_xwin_pid_is(Display *dpy, Window w, Atom pid_atom, int pid) {
    Atom type;
    int fmt;
    unsigned long n = 0, after = 0;
    unsigned char *v = NULL;
    int ok = 0;
    if (XGetWindowProperty(dpy, w, pid_atom, 0, 1, False, XA_CARDINAL, &type, &fmt, &n, &after,
                           &v) == Success &&
        v && n == 1 && fmt == 32)
        ok = (int) *(unsigned long *) v == pid;
    if (v)
        XFree(v);
    return ok;
}
/* Overlay thread: the window's _NET_WM_PID names a program mod `id` launched and that runs. */
static int sdk_xwin_owned(const char *id, unsigned long xid) {
    if (!g_sdk_ovc)
        return 0;
    Display *dpy = g_sdk_ovc->dpy;
    Atom pid_atom = XInternAtom(dpy, "_NET_WM_PID", True);
    if (pid_atom == None)
        return 0;
    Atom type;
    int fmt;
    unsigned long n = 0, after = 0;
    unsigned char *v = NULL;
    long pid = 0;
    sdk_xwin_begin();
    if (XGetWindowProperty(dpy, (Window) xid, pid_atom, 0, 1, False, XA_CARDINAL, &type, &fmt, &n,
                           &after, &v) == Success &&
        v && n == 1 && fmt == 32)
        pid = (long) *(unsigned long *) v;
    if (v)
        XFree(v);
    if (sdk_xwin_end(dpy))
        return 0; /* the window is gone */
    return pid > 0 && pid < 0x7fffffff && sdk_proc_owns(id, (int) pid);
}
static int sdk_xwin_find(const char *id, int pid, const char *prefix, int min_w, int min_h,
                         unsigned long *xid) {
    cm_mod *c = sdk_self(id, CM_RA(), "xwin_find", 1);
    if (!c || !xid || pid <= 0 || !g_sdk_ovc)
        return -1;
    *xid = 0;
    if (!sdk_proc_owns(c->id, pid)) { /* only a program this mod launched (sdk_proc.c) */
        cm_logf(c, 1, "xwin_find refused: pid %d was not launched by this mod", pid);
        return -1;
    }
    Display *dpy = g_sdk_ovc->dpy;
    Atom pid_atom = XInternAtom(dpy, "_NET_WM_PID", True);
    if (pid_atom == None)
        return -1;
    size_t plen = prefix ? strlen(prefix) : 0;
    Window r, p, *ch = NULL, best = 0;
    unsigned n = 0;
    long area = 0;
    sdk_xwin_begin();
    if (XQueryTree(dpy, g_sdk_ovc->root, &r, &p, &ch, &n)) {
        for (unsigned i = 0; i < n; i++) {
            if (!sdk_xwin_pid_is(dpy, ch[i], pid_atom, pid))
                continue;
            char *nm = NULL;
            int named = XFetchName(dpy, ch[i], &nm) && nm;
            int match = !plen || (named && strncmp(nm, prefix, plen) == 0);
            if (nm)
                XFree(nm);
            XWindowAttributes wa;
            if (!match || !XGetWindowAttributes(dpy, ch[i], &wa) ||
                wa.map_state == IsUnmapped || wa.width < min_w || wa.height < min_h)
                continue;
            long a2 = (long) wa.width * wa.height;
            if (a2 > area) {
                area = a2;
                best = ch[i];
            }
        }
        if (ch)
            XFree(ch);
    }
    if (sdk_xwin_end(dpy) || !best)
        return -1; /* nothing yet, or a window vanished during the walk: try again later */
    *xid = best;
    return 0;
}
static int sdk_xwin_geometry(const char *id, unsigned long xid, az_rect *out) {
    cm_mod *c = sdk_self(id, CM_RA(), "xwin_geometry", 1);
    if (!c || !xid || !out || !g_sdk_ovc)
        return -1;
    XWindowAttributes wa;
    sdk_xwin_begin();
    int got = XGetWindowAttributes(g_sdk_ovc->dpy, (Window) xid, &wa);
    if (sdk_xwin_end(g_sdk_ovc->dpy) || !got)
        return -1;
    *out = (az_rect) {wa.x, wa.y, wa.width, wa.height};
    return 0;
}

/* ---- the overlay's side -------------------------------------------------------------------- */

/* Called once by overlay_thread when its display is open. */
static void sdk_overlay_attach(struct overlay_ctx *c) {
    g_sdk_ov_tid = pthread_self();
    g_sdk_ov_tid_set = 1;
    g_sdk_ovc = c;
    sdk_pq_init(&g_sdk_pq);
    __atomic_store_n(&g_sdk_pq_ready, sdk_xtest_load(), __ATOMIC_RELEASE);
}

/* Every tick: publish the view, and send long presses. */
static void sdk_overlay_tick(struct overlay_ctx *c) {
    int v = sdk_view_compute(g_state.mode != AZ_CLOSED, g_state.waveform_visible,
                             __atomic_load_n(&g_search_on, __ATOMIC_RELAXED),
                             g_state.source_visible, __atomic_load_n(&g_bv_vis, __ATOMIC_ACQUIRE));
    __atomic_store_n(&g_sdk_view, v, __ATOMIC_RELAXED);
    long long now = now_ms();
    for (int i = 0; i < SDK_SURF_ALL; i++) {
        struct az_surface *p = &g_sdk_surf[i];
        if (!p->used || !sdk_mod_live(p->mod) || !p->d.on_input)
            continue;
        if (sdk_press_tick(&p->press, now) == SDK_PRESS_LONG) {
            az_event ev = {AZ_EV_LONG, 0, p->press.x, p->press.y};
            p->d.on_input(&ev, p->d.user);
        }
    }
    sdk_cursor_apply(c->dpy);
    sdk_pointer_drain(c->dpy);
}

/* Free a surface's pixels (the XImage must not free them: they are ours). */
static void sdk_surf_free_buf(struct az_surface *p) {
    if (p->img) {
        p->img->data = NULL;
        XDestroyImage(p->img);
        p->img = NULL;
    }
    free(p->buf);
    p->buf = NULL;
    p->bw = p->bh = 0;
}

/* Place, map and stack one layer of surfaces. OVER_EP147 runs before the core's windows are
 * placed, TOP after the foreign windows, so each layer is raised in order. */
static void sdk_overlay_place(struct overlay_ctx *c, int layer) {
    int view = __atomic_load_n(&g_sdk_view, __ATOMIC_RELAXED);
    for (int i = 0; i < SDK_SURF_ALL; i++) {
        struct az_surface *p = &g_sdk_surf[i];
        if (!p->used || p->d.layer != layer)
            continue;
        int want = p->show && sdk_mod_live(p->mod) && (p->d.views & AZ_VIEW_BIT(view));
        if (want && p->w.id == None) {
            XSetWindowAttributes a;
            memset(&a, 0, sizeof a);
            a.override_redirect = True;
            a.background_pixmap = None;
            a.event_mask = ButtonPressMask | ButtonReleaseMask | ExposureMask;
            p->w.id = ov_create_window(c, &a, -16, -16, 1, 1, p->d.on_input == NULL);
        }
        if (p->w.id == None)
            continue;
        p->w.wanted = want;
        if (want) {
            if (p->r.w != p->w.w || p->r.h != p->w.h)
                p->redraw = 1;
            ov_window_place(c, &p->w, p->r.x, p->r.y, p->r.w, p->r.h);
            if (!p->w.mapped)
                c->need_raise = 1; /* everything above this layer goes back on top */
        } else if (g_sdk_focus_surf == p) {
            g_sdk_focus_surf = NULL;
            __atomic_store_n(&g_sdk_focus, 0, __ATOMIC_RELEASE);
        }
        ov_window_show(c, &p->w);
    }
}

/* Foreign windows: above the core's windows, below TOP surfaces. */
static void sdk_overlay_place_xwin(struct overlay_ctx *c) {
    for (int i = 0; i < SDK_XWIN_MAX; i++) {
        struct sdk_xwin *w = &g_sdk_xwin[i];
        if (!w->used)
            continue;
        if (!sdk_mod_live(w->mod))
            w->on = 0; /* its mod is gone: lower the window once, then forget it */
        sdk_xwin_begin();
        if (w->on) {
            if (w->keep) /* the program sizes its own window (a sidebar, a resize after login) */
                XMoveWindow(c->dpy, (Window) w->xid, w->r.x, w->r.y);
            else
                XMoveResizeWindow(c->dpy, (Window) w->xid, w->r.x, w->r.y, w->r.w, w->r.h);
            if (!w->raised || c->need_raise) {
                XMapRaised(c->dpy, (Window) w->xid);
                w->raised = 1;
                c->need_raise = 1; /* TOP surfaces go above it */
            }
        } else if (w->raised) {
            XLowerWindow(c->dpy, (Window) w->xid);
            w->raised = 0;
        }
        if (sdk_xwin_end(c->dpy)) {
            cm_logf(&g_cm[w->mod], 0, "xwin_show: window 0x%lx is gone", w->xid);
            memset(w, 0, sizeof *w);
            continue;
        }
        if (!w->on)
            w->used = 0;
    }
}

/* Repaint and copy every visible surface that needs it (Expose, a resize, the mod's
 * surface_redraw, or a full repaint of the overlay). */
static void sdk_overlay_present(struct overlay_ctx *c, int all) {
    for (int i = 0; i < SDK_SURF_ALL; i++) {
        struct az_surface *p = &g_sdk_surf[i];
        if (!p->used || !p->w.wanted || !p->w.mapped || !(all || p->redraw))
            continue;
        int w = p->w.w, h = p->w.h;
        if (w <= 0 || h <= 0)
            continue;
        if (w != p->bw || h != p->bh) {
            sdk_surf_free_buf(p);
            p->buf = calloc(1, (size_t) w * h * 4);
            if (!p->buf)
                continue;
            p->img = XCreateImage(c->dpy, DefaultVisual(c->dpy, c->scr), DefaultDepth(c->dpy, c->scr),
                                  ZPixmap, 0, (char *) p->buf, w, h, 32, w * 4);
            if (!p->img) {
                sdk_surf_free_buf(p);
                continue;
            }
            p->bw = w;
            p->bh = h;
        }
        az_draw d = {{p->buf, w, h, w * 4}, p->r.x, p->r.y, c->scr_h};
        azcm_fill_rect(&d, p->r.x, p->r.y, w, h, azcm_color(AZ_COL_BG));
        cm_paint_begin(&d);
        p->d.on_paint(&d, p->r, p->d.user);
        cm_paint_end();
        XPutImage(c->dpy, p->w.id, c->gc, p->img, 0, 0, 0, 0, w, h);
        p->redraw = 0;
    }
}
static int sdk_any_redraw(void) {
    for (int i = 0; i < SDK_SURF_ALL; i++)
        if (g_sdk_surf[i].used && g_sdk_surf[i].redraw && g_sdk_surf[i].w.mapped)
            return 1;
    return 0;
}

/* A press, release or Expose on one of the surfaces' windows: 1 = it was ours. Coordinates are
 * screen pixels, as the mod draws. touch = what the event is in the touch sequence (touch_seq.h):
 * a press that is the finger moving delivers nothing, and once it moves past the slop the touch
 * is no long press. */
static int sdk_overlay_x_event(struct overlay_ctx *c, XEvent *ev, int touch) {
    (void) c;
    for (int i = 0; i < SDK_SURF_ALL; i++) {
        struct az_surface *p = &g_sdk_surf[i];
        if (!p->used || p->w.id == None || ev->xany.window != p->w.id)
            continue;
        if (ev->type == Expose) {
            p->redraw = 1;
            return 1;
        }
        if (!sdk_mod_live(p->mod) || !p->d.on_input)
            return 1;
        if (ev->type == ButtonPress && touch == TOUCH_MOVE) {
            sdk_press_move(&p->press, ev->xbutton.x_root, ev->xbutton.y_root);
        } else if (ev->type == ButtonPress) {
            int x = ev->xbutton.x_root, y = ev->xbutton.y_root;
            sdk_press_begin(&p->press, x, y, now_ms());
            az_event down = {AZ_EV_DOWN, 0, x, y}, tap = {AZ_EV_TAP, 0, x, y};
            p->d.on_input(&down, p->d.user);
            p->d.on_input(&tap, p->d.user);
        } else if (ev->type == ButtonRelease) {
            if (sdk_press_end(&p->press)) {
                az_event up = {AZ_EV_UP, 0, ev->xbutton.x_root, ev->xbutton.y_root};
                p->d.on_input(&up, p->d.user);
            }
        }
        return 1;
    }
    return 0;
}

/* The knob, its click and BACK, while a surface has focus: 1 = they went to the surface (the
 * Mod Menu does not see them). */
static int sdk_overlay_knob(int dir, int click) {
    struct az_surface *p = g_sdk_focus_surf;
    if (!p)
        return 0;
    int back = __atomic_exchange_n(&g_sdk_back_pending, 0, __ATOMIC_ACQ_REL);
    if (!p->used || !p->w.mapped || !sdk_mod_live(p->mod) || !p->d.on_input) {
        g_sdk_focus_surf = NULL;
        __atomic_store_n(&g_sdk_focus, 0, __ATOMIC_RELEASE);
        return 0;
    }
    if (dir) {
        az_event ev = {AZ_EV_KNOB, dir < 0 ? -1 : 1, 0, 0};
        for (int k = dir < 0 ? -dir : dir; k > 0; k--)
            p->d.on_input(&ev, p->d.user);
    }
    if (click) {
        az_event ev = {AZ_EV_CLICK, 0, 0, 0};
        p->d.on_input(&ev, p->d.user);
    }
    if (back) {
        az_event ev = {AZ_EV_BACK, 0, 0, 0};
        if (!p->d.on_input(&ev, p->d.user)) { /* not handled: BACK gives the knob back */
            g_sdk_focus_surf = NULL;
            __atomic_store_n(&g_sdk_focus, 0, __ATOMIC_RELEASE);
        }
    }
    return 1;
}

/* ---- the core's own surface (the confirm dialog, sdk_ui.c) ---------------------------------
 * Overlay thread only. It is on AZ_LAYER_TOP and on every view, and while it is shown it holds
 * the browse knob, its click and BACK, over the Mod Menu too (the knob reaches surfaces first,
 * and the mixer read path swallows for g_sdk_focus whatever is open). Whoever had the knob gets
 * it back when it is hidden, if it can still take it. */
static struct az_surface *g_sdk_core_prev_focus;
static void sdk_core_surface_init(const az_surface_v1 *d) {
    struct az_surface *p = &g_sdk_surf[SDK_SURF_CORE];
    if (p->used)
        return;
    memset(p, 0, sizeof *p);
    p->used = 1;
    p->mod = SDK_MOD_CORE;
    sdk_surface_desc_copy(&p->d, d);
    p->d.layer = AZ_LAYER_TOP;
    p->d.views = ~0u;
    p->r = (az_rect) {0, 0, 1, 1};
    ov_window_init(&p->w);
}
/* 0 = shown with the knob; -1 = the overlay is not up. */
static int sdk_core_surface_show(az_rect r, int on) {
    struct az_surface *p = &g_sdk_surf[SDK_SURF_CORE];
    if (!p->used || !g_sdk_ovc || !sdk_on_overlay())
        return -1;
    if (on) {
        p->r = sdk_rect_clamp(r, g_sdk_ovc->scr_w, g_sdk_ovc->scr_h);
        p->show = 1;
        p->redraw = 1;
        if (g_sdk_focus_surf != p)
            g_sdk_core_prev_focus = g_sdk_focus_surf;
        g_sdk_focus_surf = p;
        __atomic_exchange_n(&g_enc_dir, 0, __ATOMIC_ACQ_REL); /* no turn from before it */
        __atomic_store_n(&g_sdk_back_pending, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_sdk_focus, 1, __ATOMIC_RELEASE);
        return 0;
    }
    p->show = 0;
    p->redraw = 1;
    if (g_sdk_focus_surf == p) {
        struct az_surface *q = g_sdk_core_prev_focus;
        int back = q && q->used && q->show && q->w.wanted && sdk_mod_live(q->mod) &&
                   q->d.on_input && g_state.mode == AZ_CLOSED;
        g_sdk_focus_surf = back ? q : NULL;
        __atomic_store_n(&g_sdk_focus, back, __ATOMIC_RELEASE);
    }
    g_sdk_core_prev_focus = NULL;
    return 0;
}
static void sdk_core_surface_redraw(void) {
    g_sdk_surf[SDK_SURF_CORE].redraw = 1;
}
static int sdk_core_can_show(void) {
    return g_sdk_ovc != NULL && sdk_on_overlay();
}
