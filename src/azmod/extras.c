/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file extras.c
 * @brief Mod Menu extras: Hide Track Title, Key in Track Title and the Phase Meter (with its
 * Phase Layout).
 *
 * Hide Track Title patches the Label::setText vtable slots of gui::waveform::TrackInfo{2,4}DeckWidget
 * and SimpleWaveform{2,4}DeckWidget (the BROWSE strips), plus the PlayerInfo{2,4}DeckWidget
 * paintOverChildren slot that draws the eye icon, all on the JUCE message thread. The deck comes
 * from the parent PlayerInfo widget (+0x128) or the strip's ctor field (+0x98), checked against
 * the expected vtable at use; an unresolved deck leaves the title alone. A tap on the track-info
 * bar is recorded from an XNextEvent GOT hook and claimed from the per-frame setText call.
 * Gated by AZF_HIDE_TRACK_TITLE; the hooks stay installed and are inert while the mod is off.
 *
 * Key in Track Title draws each deck's key, copied from the WAVEFORM tray's KEY widget after
 * EP147 updates it, at the right end of the same title row, from the same paintOverChildren hook
 * as the eye. Each feature there is behind its own switch (AZF_DECK_KEY_TITLE for the key).
 *
 * Phase Meter inline-hooks LocalPlayer::update (per-deck bar position and tempo, kept in per-deck
 * atomics), reads the master deck from g_master_deck (stems_display.c), and paints the band from a
 * WaveformView paintOverChildren hook, repainting it through the internal area-repaint function.
 * Phase Layout rewrites WaveformView's layout table and calls its relayout so the deck waveforms
 * shrink and free the space above them for the meter. Gated by AZF_PHASE_METER.
 *
 * The grid-adjust setter address (WV_SET_GRIDADJ) is defined here; its hook lives in azmod.c.
 *
 * Entry points: install_hidetitle() (Key in Track Title's hook too), install_phasemeter(), plus
 * the per-frame pm_tick() and pl_tick() on the message thread.
 */
/* ---- Hide Track Title (Extras) -------
 * With the mod on, each deck's track-info bar shows an eye; a tap on the bar hides that
 * deck's title (eye struck through) on WAVEFORM and on BROWSE's deck strips, a second tap
 * shows it again. Off = every title shown, no eye, taps ignored.
 *
 * Where the title is set. WAVEFORM: gui::waveform::TrackInfo{2,4}DeckWidget, slot 14
 * (0x1d9fb68 / 0x1da09b0) = Label::setText(this+0x168, text), called every frame, so
 * swapping the text there always wins and showing again needs no saved title. BROWSE
 * strips: SimpleWaveform{2,4}DeckWidget slot 13 (0x1d6bff0 / 0x1d6f280) =
 * Label::setText(this+0x200), also re-set continually.
 * Which deck: the TrackInfo widget's parent PlayerInfo{2,4}DeckWidget stores its ctor's
 * deck at +0x128; a strip's SimpleWaveformWidgetBase ctor (0x1d73598) stores it at +0x98.
 * Resolved at use behind the primary vtable; anything else = -1 = title left alone.
 * Every hook is a method EP147 is calling on `self` on the message thread, so self and its
 * parent tree are alive for the call: plain reads behind alignment, null and vtable checks,
 * no syscalls on this per-frame path, nothing cached between calls. */
#include "eyeicon.h"
#include "ht_tap.h"
#include "ht_cover.h"
#define HT_VT2 EP_2d37158 /* TrackInfo2DeckWidget fns */
#define HT_VT4 EP_2d387d0 /* TrackInfo4DeckWidget fns */
/* Firmware 2.00 inserted one virtual before setText in TrackInfo{2,4}DeckWidget and
 * SimpleWaveform{2,4}DeckWidget, so the slot is one later there. ht_patch_if's equality
 * check cannot catch a wrong index if ORIG was read from that same wrong slot, so the slot
 * and the ORIG address must move together, here and in the version offset table. */
#define HT_SLOT2 (HT_VT2 + ep_off(14, 15) * 8)
#define HT_SLOT4 (HT_VT4 + ep_off(14, 15) * 8)
#define HT_ORIG2 EP_1d9fb68
#define HT_ORIG4 EP_1da09b0
#define HT_COMP_OFF \
    ep_off(0x170,   \
           0x1d8) /* the widget's juce::Component subobject (offset-to-top of its 6th vtable) */
/* its Component vtable (fns 0x2d37408 / 0x2d38a80): slots 2-8 = the empty MouseListener
 * stubs (0x242d0a0), 12 = setVisible, 26 = paint (0x1d9fb78), 27 = paintOverChildren (empty) */
#define HT_CVT2 EP_2d37408
#define HT_CVT4 EP_2d38a80
#define HT_EMPTY_STUB EP_242d0a0
#define HT_PI2D_VT EP_2d3ca00 /* gui::waveform::PlayerInfo2DeckWidget */
#define HT_PI4D_VT EP_2d3d9b8 /* gui::waveform::PlayerInfo4DeckWidget */
#define HT_DECK_OFF 0x128
#define HT_SW2_VT EP_2ce30d0 /* SimpleWaveform2DeckWidget fns (BROWSE strip) */
#define HT_SW4_VT EP_2ce7c50
#define HT_SW_SLOT2 (HT_SW2_VT + ep_off(13, 14) * 8)
#define HT_SW_SLOT4 (HT_SW4_VT + ep_off(13, 14) * 8)
#define HT_SW_ORIG2 EP_1d6bff0
#define HT_SW_ORIG4 EP_1d6f280
#define HT_SW_DECK 0x98
static void (*g_ht_orig2)(void *, void *), (*g_ht_orig4)(void *, void *);
static void (*g_ht_osw2)(void *, void *), (*g_ht_osw4)(void *, void *);
static void (*g_ht_opoc2)(void *, void *), (*g_ht_opoc4)(void *, void *);
static void *g_ht_empty;              /* a juce::String "" (the static empty, never freed) */
/* g_ht_empty is built by EP147's juce::String ctor, so it is made on the overlay thread after
 * EP147 has started (ht_runtime_init), not in the constructor where the hooks install. Until
 * this is set, a hook passes EP147's own string through. */
static _Atomic int g_ht_empty_ok;
static inline int ht_empty_ok(void) {
    return __atomic_load_n(&g_ht_empty_ok, __ATOMIC_ACQUIRE);
}
static _Atomic unsigned g_ht_hide;    /* bit d = deck d's title hidden */
static unsigned g_ht_has, g_ht_drawn; /* message thread: deck has a title / its eye is on screen */
static uintptr_t g_ht_eye[2];         /* juce::Image, from ImageCache: 0 open, 1 shut */
static int ht_on(void) {
    return az_on(AZF_HIDE_TRACK_TITLE) &&
           az_active(AZF_HIDE_TRACK_TITLE);
}
static inline int ht_ptr_ok(uintptr_t p) {
    return p >= 0x10000 && p <= 0x0000ffffffffffffUL && !(p & 7);
}
/* deck of a TrackInfo widget (live: EP147 is calling it), or -1 */
static int ht_deck(uintptr_t widget) {
    if (!ht_ptr_ok(widget))
        return -1;
    uintptr_t par =
        *(volatile uintptr_t *) (widget + HT_COMP_OFF + 0x18); /* juce parentComponent */
    if (!ht_ptr_ok(par))
        return -1;
    uintptr_t pvt = *(volatile uintptr_t *) par;
    if (!ht_ptr_ok(pvt))
        return -1;
    uintptr_t full = par + (uintptr_t) *(volatile long *) (pvt - 0x10); /* offset-to-top */
    if (!ht_ptr_ok(full))
        return -1;
    uintptr_t fvt = *(volatile uintptr_t *) full;
    if (fvt != HT_PI2D_VT && fvt != HT_PI4D_VT)
        return -1;
    int k = *(volatile int *) (full + HT_DECK_OFF);
    return k >= 0 && k < 4 ? k : -1;
}
/* Tap: every touch reaches JUCE as an X ButtonPress read through XNextEvent (on the hardware
 * EP147's gt928 decoder 0x212c6f0 XSendEvent()s it; under the emulator the X server delivers it).
 * Only EP147's own GOT slot for XNextEvent is hooked, so azmod's overlay loop and other
 * processes are unaffected. EP147 is partial-RELRO with lazy binding: .got.plt is plain
 * writable data, so this is a pointer store with no mprotect (azm_patch_vtable's PROT_READ
 * would break every later lazy bind on that page). The original comes from dlsym, never
 * the slot: an unbound slot holds the resolver stub, which would rebind over us.
 * Only a whole tap counts (ht_tap.h: press and release close together, one finger), so a
 * drag, a resting hand or a brush that starts on a title bar toggles nothing. EP147 reads its
 * X events on the JUCE message thread, and this hook sees each one before EP147 handles it:
 * at the press, JUCE's own hit test says which deck's PlayerInfo widget gets it
 * (ht_cover.h), so a press on a popup drawn over a title bar (EJECT's question, which closes
 * on a press outside its box) belongs to no deck. The tap's press point is recorded when it
 * lifts; the deck's setTitle call (message thread, every frame, holding the live widget)
 * hit-tests it and claims it once (CAS on the seq), only when the press was that deck's. A
 * tap expires after HT_TAP_TTL_MS so one made where no panel shows (BROWSE) is not claimed
 * later when WAVEFORM comes back. */
#define HT_GOT_XNEXTEVENT EP_3b25a60
#define HT_TAP_TTL_MS 150 /* setTitle runs every frame while the panel shows */
static int (*g_ht_xnext)(Display *, XEvent *);
static _Atomic unsigned long g_ht_tap; /* seq << 32 | y << 16 | x */
static _Atomic unsigned g_ht_tap_done; /* last seq claimed */
static _Atomic long g_ht_tap_ms;
static struct ht_gest g_ht_gest; /* under g_ht_gest_mu */
static pthread_mutex_t g_ht_gest_mu = PTHREAD_MUTEX_INITIALIZER;
/* each deck's TrackInfo component as ht_title last saw it (vtable-checked at use), the decks
 * whose bar got the last first-finger press, and that mask for the tap it completed */
static uintptr_t g_ht_bar[HT_COVER_DECKS]; /* __atomic only */
static _Atomic unsigned g_ht_press_mask, g_ht_tap_mask;
static unsigned ht_press_mask(int x, int y);
static int ht_xnext_hook(Display *dpy, XEvent *ev) {
    int r = g_ht_xnext(dpy, ev);
    if (ev && (ev->type == ButtonPress || ev->type == ButtonRelease)) {
        int x = ev->xbutton.x_root, y = ev->xbutton.y_root, tx, ty;
        long ms = now_ms();
        if (ev->type == ButtonPress && ev->xbutton.button == 1) /* before EP147 handles it */
            __atomic_store_n(&g_ht_press_mask, ht_on() ? ht_press_mask(x, y) : 0u, __ATOMIC_RELAXED);
        pthread_mutex_lock(&g_ht_gest_mu);
        int tap = ht_gest_feed(&g_ht_gest, ev->type == ButtonPress, ev->xbutton.button, x, y, ms,
                               &tx, &ty);
        pthread_mutex_unlock(&g_ht_gest_mu);
        if (tap && tx >= 0 && ty >= 0 && tx < 65536 && ty < 65536) {
            unsigned long seq = (__atomic_load_n(&g_ht_tap, __ATOMIC_RELAXED) >> 32) + 1;
            __atomic_store_n(&g_ht_tap_ms, ms, __ATOMIC_RELAXED);
            __atomic_store_n(&g_ht_tap_mask, __atomic_load_n(&g_ht_press_mask, __ATOMIC_RELAXED),
                             __ATOMIC_RELAXED);
            __atomic_store_n(&g_ht_tap, seq << 32 | (unsigned long) ty << 16 | (unsigned long) tx,
                             __ATOMIC_RELEASE);
        }
    }
    return r;
}
static int ht_install_xnext(void) {
    uintptr_t cur = 0;
    void *real = dlsym(RTLD_DEFAULT, "XNextEvent");
    if (!real || real == (void *) ht_xnext_hook || !img_q(HT_GOT_XNEXTEVENT, &cur) || !cur)
        return -2;
    g_ht_xnext = (int (*)(Display *, XEvent *)) real;
    __atomic_store_n((void **) HT_GOT_XNEXTEVENT, (void *) ht_xnext_hook, __ATOMIC_RELEASE);
    return 1;
}
/* on-screen rect of a live component: bounds (+0x20 x,y +0x28 w,h) summed up the parent
 * chain (+0x18); 0 unless the visible flag (+0xc0 bit 1) is set on every level */
static int ht_screen_rect(uintptr_t comp, int r[4]) {
    uintptr_t c = comp;
    int sx = 0, sy = 0;
    for (int depth = 0; c && depth < 32; depth++) {
        if (!ht_ptr_ok(c) || !(*(volatile uint32_t *) (c + 0xc0) & 2))
            return 0;
        sx += *(volatile int *) (c + 0x20);
        sy += *(volatile int *) (c + 0x24);
        c = *(volatile uintptr_t *) (c + 0x18);
    }
    if (c)
        return 0;
    r[0] = sx;
    r[1] = sy;
    r[2] = *(volatile int *) (comp + 0x28);
    r[3] = *(volatile int *) (comp + 0x2c);
    return 1;
}
/* The title row inside the parent PlayerInfo{2,4}DeckWidget. The eye belongs at the right
 * end of the title bar, not at the TrackInfo widget's own right edge (which stops near the
 * middle). Row = the TrackInfo child's bounds in parent coords; its right end = the parent's
 * width, or the left edge of the nearest sibling to the right on the same row (4 DECK: the
 * BPM box), visible or not, never the ON AIR indicator. Children = JUCE childComponentList:
 * data +0x40, numAllocated +0x48, numUsed +0x50, each vtable-checked. Called only from
 * EP147's own per-frame / paint calls on live objects; nothing cached. */
#define HT_PI2D_CVT EP_2d3cc28    /* PlayerInfo2DeckWidget Component fns */
#define HT_ONAIR_VT EP_2d21548    /* gui::waveform::OnAirWidget: row's top-right, 71 x 20 */
#define HT_DECKBOX2_VT EP_2d1b678 /* the DECK n box, 2 DECK (1,1 54 x 48 = the bar's height) */
#define HT_DECKBOX4_VT EP_2d1dbe8 /* the DECK n box, 4 DECK (1,1 44 x 38) */
/* the eye's ink inside its 26 x 16 image (eyeicon.h), antialiased edges included:
 * 24 x 14 with 1 px clear on every side */
#define HT_INK_L 1
#define HT_INK_T 1
#define HT_INK_W 24
#define HT_INK_H 14
#define HT_PAD_RIGHT 7 /* ink to the bar's right edge, every layout */
#define HT_PI4D_CVT EP_2d3dbe0
/* JUCE's hit test, the one that routes the mouse: the deepest visible component under a point
 * that wants it (hitTest), children front to back. Called only, never hooked (its first
 * instructions include a PC-relative tbz). */
#define HT_COMPONENT_AT EP_2482f30 /* juce::Component::getComponentAt(const Point<int> &) */
static uintptr_t ht_parent(uintptr_t c) {
    uintptr_t p = *(volatile uintptr_t *) (c + 0x18); /* juce parentComponent */
    return ht_ptr_ok(p) ? p : 0;
}
/* the component under screen point (x, y), asked of the top-level component `root` */
static uintptr_t ht_at(uintptr_t root, int x, int y) {
    int pt[2] = {x - *(volatile int *) (root + 0x20), y - *(volatile int *) (root + 0x24)};
    uintptr_t hit =
        (uintptr_t) ((void *(*) (void *, const int *) ) HT_COMPONENT_AT)((void *) root, pt);
    return ht_ptr_ok(hit) ? hit : 0;
}
/* The decks whose PlayerInfo widget gets a press at (x, y), asked before EP147 handles it.
 * Message thread (EP147 reads X events there); every widget vtable-checked at use, a deck
 * whose bar ht_title has not seen gets nothing. */
static unsigned ht_press_mask(int x, int y) {
    uintptr_t owner[HT_COVER_DECKS] = {0}, root = 0;
    for (int d = 0; d < HT_COVER_DECKS; d++) {
        uintptr_t c = __atomic_load_n(&g_ht_bar[d], __ATOMIC_RELAXED), par;
        if (!ht_ptr_ok(c) ||
            (*(volatile uintptr_t *) c != HT_CVT2 && *(volatile uintptr_t *) c != HT_CVT4))
            continue;
        par = ht_parent(c);
        if (!par || (*(volatile uintptr_t *) par != HT_PI2D_CVT &&
                     *(volatile uintptr_t *) par != HT_PI4D_CVT))
            continue;
        owner[d] = par;
        if (!root) { /* the top-level component: every bar is in the one window */
            root = par;
            for (int i = 0; i < HT_COVER_DEPTH && ht_parent(root); i++)
                root = ht_parent(root);
        }
    }
    return ht_cover_mask(owner, root, x, y, ht_at, ht_parent);
}
static int ht_row_ex(uintptr_t parcomp, int row[4], uintptr_t *ticomp, int bar[3]);
static int ht_row(uintptr_t parcomp, int row[4], uintptr_t *ticomp) {
    return ht_row_ex(parcomp, row, ticomp, 0);
}
/* bar[] = { top, bottom (exclusive), ON AIR bottom (exclusive) } in parent coordinates */
static int ht_row_ex(uintptr_t parcomp, int row[4], uintptr_t *ticomp, int bar[3]) {
    if (!ht_ptr_ok(parcomp))
        return 0;
    uintptr_t pv = *(volatile uintptr_t *) parcomp;
    if (pv != HT_PI2D_CVT && pv != HT_PI4D_CVT)
        return 0;
    uintptr_t kids =
        *(volatile uintptr_t *) (parcomp + 0x40); /* childComponentList.data */
    int n = *(volatile int *) (parcomp + 0x50);   /* .numUsed (+0x48 = numAllocated) */
    if (!ht_ptr_ok(kids) || n <= 0 || n > 256)
        return 0;
    uintptr_t ti = 0;
    for (int i = 0; i < n; i++) {
        uintptr_t c = *(volatile uintptr_t *) (kids + 8 * i);
        if (ht_ptr_ok(c) &&
            (*(volatile uintptr_t *) c == HT_CVT2 || *(volatile uintptr_t *) c == HT_CVT4)) {
            ti = c;
            break;
        }
    }
    if (!ti)
        return 0;
    int x = *(volatile int *) (ti + 0x20), y = *(volatile int *) (ti + 0x24),
        h = *(volatile int *) (ti + 0x2c);
    int right = *(volatile int *) (parcomp + 0x28);
    for (int i = 0; i < n; i++) {
        uintptr_t c = *(volatile uintptr_t *) (kids + 8 * i);
        /* Fixed geometry, so the eye does not move when ON AIR shows: every sibling counts
         * whether visible or not, except ON AIR, which sits above the eye. */
        if (c == ti || !ht_ptr_ok(c) || *(volatile uintptr_t *) c == HT_ONAIR_VT)
            continue;
        int cx = *(volatile int *) (c + 0x20), cy = *(volatile int *) (c + 0x24),
            ch = *(volatile int *) (c + 0x2c);
        if (cx > x + 40 && cx < right && cy < y + h && cy + ch > y)
            right = cx; /* a sibling on this row */
    }
    if (bar) { /* the bar = the DECK n box's span; ON AIR sits in its top-right */
        bar[0] = y;
        bar[1] = y + h;
        bar[2] = y;
        for (int i = 0; i < n; i++) {
            uintptr_t c = *(volatile uintptr_t *) (kids + 8 * i);
            if (!ht_ptr_ok(c))
                continue;
            uintptr_t cv = *(volatile uintptr_t *) c;
            int cy = *(volatile int *) (c + 0x24), ch = *(volatile int *) (c + 0x2c);
            if (cv == HT_DECKBOX2_VT || cv == HT_DECKBOX4_VT) {
                bar[0] = cy;
                bar[1] = cy + ch;
            } else if (cv == HT_ONAIR_VT)
                bar[2] = cy + ch;
        }
        if (right == *(volatile int *) (parcomp + 0x28))
            right--; /* the bar's 1 px edge at the parent's right */
    }
    row[0] = x;
    row[1] = y;
    row[2] = right - x;
    row[3] = h;
    if (ticomp)
        *ticomp = ti;
    return row[2] > EYE_W + 20;
}
/* the WAVEFORM title, every frame: claim a tap on this bar, keep the eye in step, swap */
static int ht_title(void *self, void *str) {
    if (!ht_on()) {
        if (__atomic_load_n(&g_ht_hide, __ATOMIC_RELAXED))
            __atomic_store_n(&g_ht_hide, 0, __ATOMIC_RELAXED);
        if (g_ht_drawn) {
            g_ht_drawn = 0;
            uintptr_t par = *(volatile uintptr_t *) ((char *) self + HT_COMP_OFF + 0x18);
            if (ht_ptr_ok(par))
                ((void (*)(void *)) JUCE_COMPONENT_REPAINT)((void *) par);
        }
        return 0;
    }
    int d = ht_deck((uintptr_t) self);
    if (d < 0)
        return 0;
    uintptr_t comp = (uintptr_t) self + HT_COMP_OFF, sp = 0;
    __atomic_store_n(&g_ht_bar[d], comp, __ATOMIC_RELAXED);
    if (str && ht_ptr_ok((uintptr_t) str))
        sp = *(volatile uintptr_t *) str; /* juce::String = its char* */
    unsigned bit = 1u << d, has = sp && *(const char *) sp ? bit : 0;
    g_ht_has = (g_ht_has & ~bit) | has;
    unsigned long t = __atomic_load_n(&g_ht_tap, __ATOMIC_ACQUIRE);
    unsigned seq = (unsigned) (t >> 32), done = __atomic_load_n(&g_ht_tap_done, __ATOMIC_RELAXED);
    if (seq != done && now_ms() - __atomic_load_n(&g_ht_tap_ms, __ATOMIC_RELAXED) > HT_TAP_TTL_MS) {
        __atomic_compare_exchange_n(&g_ht_tap_done, &done, seq, 0, __ATOMIC_RELAXED,
                                    __ATOMIC_RELAXED); /* stale: nobody's */
    } else if (seq != done && has) {
        int r[4], row[4], x = (int) (t & 0xffff), y = (int) (t >> 16 & 0xffff);
        uintptr_t par = *(volatile uintptr_t *) (comp + 0x18);
        if (ht_screen_rect(comp, r) && ht_row(par, row, 0)) {
            r[2] = row[2];
        } /* the whole bar, out to the eye */
        else
            r[2] = 0;
        if (r[2] > 0 && x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3] &&
            (__atomic_load_n(&g_ht_tap_mask, __ATOMIC_RELAXED) & bit) && /* the bar got it */
            __atomic_compare_exchange_n(&g_ht_tap_done, &done, seq, 0, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
            __atomic_xor_fetch(&g_ht_hide, bit, __ATOMIC_RELAXED);
            ((void (*)(void *)) JUCE_COMPONENT_REPAINT)((void *) par); /* the eye is the parent's */
        }
    }
    if ((g_ht_drawn & bit) != has) { /* eye on / off */
        uintptr_t par = *(volatile uintptr_t *) (comp + 0x18);
        if (ht_ptr_ok(par))
            ((void (*)(void *)) JUCE_COMPONENT_REPAINT)((void *) par);
    }
    return (__atomic_load_n(&g_ht_hide, __ATOMIC_RELAXED) & bit) != 0;
}
/* The title setters are shared by two features, each behind its own switch: Key in Track
 * Title's per-frame step (kt_title: ages the key's copy, repaints the row when what it shows
 * changes) and Hide Track Title's (ht_title: the tap, the eye, the swap). */
static void kt_title(void *self);
static void ht_hook2(void *self, void *str) {
    kt_title(self);
    int hide = ht_title(self, str); /* always: it also draws the eye and takes the tap */
    g_ht_orig2(self, hide && ht_empty_ok() ? (void *) &g_ht_empty : str);
}
static void ht_hook4(void *self, void *str) {
    kt_title(self);
    int hide = ht_title(self, str);
    g_ht_orig4(self, hide && ht_empty_ok() ? (void *) &g_ht_empty : str);
}
/* the BROWSE strips: swap only (the tap and the eye live on the WAVEFORM bar) */
static void ht_sw(void *self, void *str, void (*orig)(void *, void *)) {
    int hid = 0;
    if (ht_on() && ht_ptr_ok((uintptr_t) self)) {
        uintptr_t vt = *(volatile uintptr_t *) self;
        int d = (vt == HT_SW2_VT || vt == HT_SW4_VT)
                    ? *(volatile int *) ((char *) self + HT_SW_DECK)
                    : -1;
        hid = d >= 0 && d < 4 && (__atomic_load_n(&g_ht_hide, __ATOMIC_RELAXED) >> d & 1u);
    }
    orig(self, hid && ht_empty_ok() ? (void *) &g_ht_empty : str);
}
static void ht_sw2(void *s, void *t) {
    ht_sw(s, t, g_ht_osw2);
}
static void ht_sw4(void *s, void *t) {
    ht_sw(s, t, g_ht_osw4);
}
/* ---- Key in Track Title (Extras) -------
 * Each deck's key at the right end of its title row (left of Hide Track Title's eye when that
 * shows), so the WAVEFORM screen's left tray, where EP147 shows KEY, can stay collapsed.
 *
 * The text is the tray's own: gui::waveform::KeyWidget (vt KT_KEYW_VT) writes the deck's key
 * into its juce::Label (+0x140) in KT_KEYW_UPDATE, with the deck's key notation and Extras >
 * Camelot Keys applied there (key_pane.c hooks the formatter that function calls). EP147 calls
 * it about once a frame for each deck of the showing layout, also while the tray is collapsed
 * (measured on the emulator: a track loaded with the tray collapsed updated it). An inline
 * hook copies the Label's text after each call, while the Label is visible (without a track
 * EP147 hides it and leaves a placeholder text in it). The deck is the KeyWidget's parent's:
 * gui::waveform::DeckInfo{2,4}DeckWidget stores its ctor's deck argument at +0xd0 (both
 * builds: str w3 of the ctor, called with 0..3). Anything else is no deck and copies nothing.
 *
 * The drawing is EP147's own text drawing with the title Label's own font (+0x178) and text
 * colour (findColour textColourId, at half alpha while the Label is disabled, as JUCE's
 * LookAndFeel_V2::drawLabel 0x2475210 does): juce::Graphics setColour, setFont and
 * drawFittedText, right-justified and centred in the eye's band below ON AIR, inside saveState
 * / restoreState. The String it draws is a per-deck juce::String assigned from the Label's own
 * (a reference count, no allocation) when the text changes, never in paint.
 *
 * Threads: every function here runs on the JUCE message thread (EP147's widget updates, its
 * title setters and its paint), except kt_runtime_init (the overlay thread, once, before the
 * release store of g_kt_ok that the message thread acquires). g_kt is message thread only. */
#include "kt_layout.h"
#define KT_KEYW_UPDATE EP_1d76f18 /* KeyWidget: writes the tray KEY Label (see the V200 basis) */
#define KT_KEYW_VT EP_2cedcf8     /* gui::waveform::KeyWidget */
#define KT_DI2_VT EP_2cfe0f0      /* gui::waveform::DeckInfo2DeckWidget */
#define KT_DI4_VT EP_2cfe5c0      /* gui::waveform::DeckInfo4DeckWidget */
#define KT_DI_DECK 0xd0
#define KT_KEYW_LABEL 0x140
#define KT_LABEL_VT EP_31592e0 /* juce::Label */
#define KT_LABEL_TEXT 0x170    /* juce::Label lastTextValue (the String setText stores) */
#define KT_LABEL_FONT 0x178    /* juce::Label font */
#define KT_TITLE_LABEL ep_off(0x168, 0x1d0) /* TrackInfo{2,4}DeckWidget: its title Label */
#define KT_FINDCOLOUR EP_2471200 /* juce::Component::findColour(id), Colour through x8 */
#define KT_ISENABLED EP_2437090  /* juce::Component::isEnabled() */
#define KT_MULALPHA EP_23a54e0   /* juce::Colour::withMultipliedAlpha(float), result through x8 */
#define KT_TEXT_COLOUR_ID 0x1000281 /* juce::Label::textColourId */
#define KT_SETCOLOUR EP_23af140     /* juce::Graphics::setColour(Colour) */
#define KT_SETFONT EP_23af590       /* juce::Graphics::setFont(const Font &) */
#define KT_DRAWFITTED EP_23eb200 /* juce::Graphics::drawFittedText(String, Rectangle<int>, Justification, int, float) */
#define KT_STR_ASSIGN EP_230f5c0 /* juce::String::operator=(const String &) */
#define KT_JUST_RIGHT 0x22       /* juce::Justification::centredRight */
#define KT_MIN_SCALE 0.8f        /* a key squeezes at most this far, then it is cut */
#define KT_LOG_CHANGES 64
static int kt_on(void) {
    return az_on(AZF_DECK_KEY_TITLE) && az_active(AZF_DECK_KEY_TITLE);
}
/* Tap the key: a tap on the key drawn in a deck's title row opens that deck's key shift panel,
 * gui::performance::KeyShiftWidget, EP147's own (the one the tray's KEY row arrow opens). Each
 * DeckInfo{2,4}DeckWidget owns one (+0x1f0 / +0x1b0) next to its KeyWidget (+0x210 / +0x1b8),
 * and its mouseDown (vtable slot 5) is the whole open path: a press inside the KeyWidget, the
 * KeyWidget's arrow enabled (KT_ARROW_OK), then Component::setVisible(!isVisible) on the
 * panel's juce::Component (a virtual base at +0x70). EP147 refreshes the open panel each frame
 * from the same widget and closes it itself when the deck has no track.
 *
 * With the tray collapsed EP147 gives DeckInfo4DeckFieldWidget, the tray's body, a width of 0,
 * which clips the panel away. So the open moves the panel's Component into the WAVEFORM view
 * (gui::waveform::WaveformView, the ancestor whose vtable is KT_VIEW_VT) with JUCE's own
 * removeChildComponent / addChildComponent, the two calls Component::addAndMakeVisible makes,
 * places it above the deck's title row, then shows it. Once it closes (a second tap on the key,
 * the tray's arrow, EP147's own close), the switch goes off, or the deck's widget stops being
 * updated (another layout shows), it goes back to its own parent at its own index and bounds,
 * so EP147's tray sees it exactly as it left it.
 *
 * The tap: Hide Track Title's XNextEvent hook records every whole tap (it calls no EP147 code
 * for this feature). The deck's title setter, before Hide Track Title's step, claims a tap on
 * the key's tap target when kt_tap_owner gives it to the key (JUCE's hit test asked there, on
 * the message thread), so Hide Track Title never sees it; a tap elsewhere on the bar stays Hide
 * Track Title's. The claim is acted on in the deck's next KeyWidget update, which holds the
 * live DeckInfo widget. Every object is vtable-checked at use, the cached ones included. */
#define KT_DI2_PANEL 0x1f0        /* DeckInfo2DeckWidget: its KeyShiftWidget */
#define KT_DI2_KEYW 0x210         /* DeckInfo2DeckWidget: its KeyWidget */
#define KT_DI4_PANEL 0x1b0        /* DeckInfo4DeckWidget: its KeyShiftWidget */
#define KT_DI4_KEYW 0x1b8         /* DeckInfo4DeckWidget: its KeyWidget */
#define KT_PANEL_VT EP_2b88870    /* gui::performance::KeyShiftWidget */
#define KT_PANEL_CVT EP_2b88a48   /* its juce::Component (offset-to-top -0x70) */
#define KT_PANEL_COMP 0x70        /* the Component within the KeyShiftWidget */
#define KT_ARROW_OK EP_1d77468    /* KeyWidget: its KEY row arrow is enabled */
#define KT_SETVISIBLE EP_249daf0  /* juce::Component::setVisible(bool), the Component's slot 12 */
#define KT_REMOVE_CHILD EP_249a4b0 /* juce::Component::removeChildComponent(Component *) */
#define KT_ADD_CHILD EP_2485140   /* juce::Component::addChildComponent(child, zOrder) */
#define KT_SETBOUNDS EP_249efe0   /* juce::Component::setBounds(x, y, w, h) */
#define KT_VIEW_VT EP_29e9790     /* gui::waveform::WaveformView */
static struct {
    char text[KT_TEXT_MAX];  /* the tray's key as last copied ("" = none) */
    char drawn[KT_TEXT_MAX]; /* what the row's last paint drew */
    unsigned age;            /* title frames since the copy */
    uintptr_t str;           /* a juce::String (its text pointer) holding text */
    /* tap the key */
    int hit[4];              /* the tap target on screen, valid while drawn[0] */
    int key_right, bar_top, bar_bottom; /* screen: the key's right edge, the bar's span */
    unsigned pend;           /* a claimed tap: title frames waiting (0 = none) */
    uintptr_t keyw_par;      /* the DeckInfo widget whose KeyWidget last updated this deck */
    uintptr_t moved, moved_par; /* the panel moved into the view, and its own parent */
    int moved_z, moved_b[4]; /* its index and bounds in that parent */
} g_kt[4];
static _Atomic int g_kt_ok; /* the Strings are built */
static void (*g_kt_okeyw)(void *);
static int g_kt_logged;
#ifdef SK_HAVE_HOOK
/* Calls fn(a, b) with x8 = out, the C++ indirect result, and s0 = f for a float argument. */
__attribute__((visibility("hidden"))) void kt_call_x8(void *fn, void *out, uintptr_t a, uintptr_t b,
                                                      float f);
__asm__(".text\n"
        ".p2align 2\n" SK_FN(kt_call_x8) "    stp x29, x30, [sp, #-16]!\n"
                                         "    mov x29, sp\n"
                                         "    mov x9, x0\n"
                                         "    mov x8, x1\n"
                                         "    mov x0, x2\n"
                                         "    mov x1, x3\n"
                                         "    blr x9\n"
                                         "    ldp x29, x30, [sp], #16\n"
                                         "    ret\n" SK_END(kt_call_x8));
#endif
/* p when it is an aligned object whose vtable is vt, else 0 */
static uintptr_t kt_obj(uintptr_t p, uintptr_t vt) {
    return ht_ptr_ok(p) && *(volatile uintptr_t *) p == vt ? p : 0;
}
static int kt_visible(uintptr_t c) {
    return (*(volatile uint32_t *) (c + 0xc0) & 2) != 0;
}
static void kt_set_visible(uintptr_t c, int on) {
    ((void (*)(uintptr_t, int)) KT_SETVISIBLE)(c, on);
}
/* A DeckInfo{2,4}DeckWidget's key shift panel Component, and its KeyWidget in *kw: 0 unless
 * par is one, the panel is a KeyShiftWidget whose Component is where its vtable says, and that
 * Component's setVisible slot is JUCE's. */
static uintptr_t kt_panel_of(uintptr_t par, uintptr_t *kw) {
    int di2 = kt_obj(par, KT_DI2_VT) != 0;
    if (!di2 && !kt_obj(par, KT_DI4_VT))
        return 0;
    uintptr_t ksw = kt_obj(*(volatile uintptr_t *) (par + (di2 ? KT_DI2_PANEL : KT_DI4_PANEL)),
                           KT_PANEL_VT);
    if (!ksw || *(volatile long *) (KT_PANEL_VT - 0x18) != KT_PANEL_COMP) /* its vbase offset */
        return 0;
    uintptr_t c = kt_obj(ksw + KT_PANEL_COMP, KT_PANEL_CVT);
    if (!c || *(volatile uintptr_t *) (KT_PANEL_CVT + 12 * 8) != KT_SETVISIBLE)
        return 0;
    *kw = kt_obj(*(volatile uintptr_t *) (par + (di2 ? KT_DI2_KEYW : KT_DI4_KEYW)), KT_KEYW_VT);
    return *kw ? c : 0;
}
/* c's index in parent p's child list (data +0x40, numUsed +0x50), or -1 */
static int kt_child_index(uintptr_t p, uintptr_t c) {
    uintptr_t kids = *(volatile uintptr_t *) (p + 0x40);
    int n = *(volatile int *) (p + 0x50);
    if (!ht_ptr_ok(kids) || n <= 0 || n > 256)
        return -1;
    for (int i = 0; i < n; i++)
        if (*(volatile uintptr_t *) (kids + 8 * i) == c)
            return i;
    return -1;
}
static void kt_log(const char *what, int d, int x, int y) {
    static int n;
    if (n >= KT_LOG_CHANGES)
        return;
    n++;
    FILE *f = azlog_open("/tmp/azmod-hidetitle.log", "a");
    if (f) {
        fprintf(f, "key tap: deck %d %s %d,%d\n", d + 1, what, x, y);
        fclose(f);
    }
}
/* Deck d's panel back home, unless it should stay open in the view: it is still open, the
 * switch is on, and the deck's own widget is still the one EP147 updates. Message thread,
 * from the title setter (every frame) and from a close. */
static void kt_panel_home(int d) {
    uintptr_t c = g_kt[d].moved, op = g_kt[d].moved_par, kw = 0;
    if (!c)
        return;
    if (kt_panel_of(op, &kw) != c) { /* not the panel it was: nothing to put back */
        g_kt[d].moved = 0;
        return;
    }
    if (kt_visible(c) && kt_on() && g_kt[d].age < KT_STALE_FRAMES && g_kt[d].keyw_par == op)
        return;
    if (kt_visible(c))
        kt_set_visible(c, 0);
    uintptr_t cur = *(volatile uintptr_t *) (c + 0x18);
    if (cur != op) {
        if (ht_ptr_ok(cur))
            ((void (*)(uintptr_t, uintptr_t)) KT_REMOVE_CHILD)(cur, c);
        ((void (*)(uintptr_t, uintptr_t, int)) KT_ADD_CHILD)(op, c, g_kt[d].moved_z);
    }
    ((void (*)(uintptr_t, int, int, int, int)) KT_SETBOUNDS)(c, g_kt[d].moved_b[0], g_kt[d].moved_b[1],
                                                             g_kt[d].moved_b[2], g_kt[d].moved_b[3]);
    g_kt[d].moved = 0;
    kt_log("panel home", d, g_kt[d].moved_b[0], g_kt[d].moved_b[1]);
}
/* A claimed tap on deck d's key, from the deck's KeyWidget update (kw, its DeckInfo par): close
 * the panel when it is open, else open it above the title row the way the tray's arrow does
 * (same gate, same setVisible), moved into the WAVEFORM view so a collapsed tray cannot clip it. */
static void kt_panel_toggle(int d, uintptr_t kw, uintptr_t par) {
    uintptr_t own = 0, c = kt_panel_of(par, &own);
    if (!c || own != kw)
        return;
    if (kt_visible(c)) {
        kt_set_visible(c, 0);
        kt_panel_home(d);
        kt_log("closed", d, 0, 0);
        return;
    }
    if (!g_kt[d].drawn[0] || !((int (*)(uintptr_t)) KT_ARROW_OK)(kw))
        return;
    if (g_kt[d].moved) /* a stale move of this deck's other layout: home first */
        kt_panel_home(d);
    if (g_kt[d].moved || *(volatile uintptr_t *) (c + 0x18) != par)
        return;
    uintptr_t view = par;
    for (int i = 0; i < 8 && view && !kt_obj(view, KT_VIEW_VT); i++)
        view = ht_parent(view);
    int area[4], x, y, z = kt_child_index(par, c);
    int w = *(volatile int *) (c + 0x28), h = *(volatile int *) (c + 0x2c);
    if (!kt_obj(view, KT_VIEW_VT) || z < 0 || !ht_screen_rect(view, area) ||
        !kt_panel_at(w, h, g_kt[d].key_right, g_kt[d].bar_top, g_kt[d].bar_bottom, area,
                     KT_PANEL_TOP, &x, &y))
        return;
    g_kt[d].moved_z = z;
    for (int i = 0; i < 4; i++)
        g_kt[d].moved_b[i] = *(volatile int *) (c + 0x20 + 4 * i);
    ((void (*)(uintptr_t, uintptr_t)) KT_REMOVE_CHILD)(par, c);
    ((void (*)(uintptr_t, uintptr_t, int)) KT_ADD_CHILD)(view, c, -1);
    ((void (*)(uintptr_t, int, int, int, int)) KT_SETBOUNDS)(c, x - area[0], y - area[1], w, h);
    g_kt[d].moved = c;
    g_kt[d].moved_par = par;
    kt_set_visible(c, 1);
    kt_log("opened", d, x, y);
}
/* KT_KEYW_UPDATE, after EP147's own: copy the deck's key as the tray now shows it */
static void kt_keyw_hook(void *self) {
    g_kt_okeyw(self);
    if (!__atomic_load_n(&g_kt_ok, __ATOMIC_ACQUIRE) || !kt_on())
        return;
    uintptr_t kw = kt_obj((uintptr_t) self, KT_KEYW_VT), par, lab;
    if (!kw)
        return;
    par = *(volatile uintptr_t *) (kw + 0x18); /* juce parentComponent */
    if (!kt_obj(par, KT_DI2_VT) && !kt_obj(par, KT_DI4_VT))
        return;
    int d = *(volatile int *) (par + KT_DI_DECK);
    if (d < 0 || d >= 4)
        return;
    g_kt[d].keyw_par = par;
    if (g_kt[d].pend) { /* a tap on the key, claimed by the title setter */
        g_kt[d].pend = 0;
        kt_panel_toggle(d, kw, par);
    }
    lab = kt_obj(*(volatile uintptr_t *) (kw + KT_KEYW_LABEL), KT_LABEL_VT);
    if (!lab)
        return;
    char t[KT_TEXT_MAX + 1];
    uintptr_t sp = *(volatile uintptr_t *) (lab + KT_LABEL_TEXT);
    int vis = (*(volatile uint32_t *) (lab + 0xc0) & 2) != 0, ok = own_str(sp, t, sizeof t);
    g_kt[d].age = 0;
    if (!kt_store(g_kt[d].text, ok ? t : "", vis))
        return;
    if (g_kt[d].text[0])
        ((void (*)(uintptr_t *, uintptr_t)) KT_STR_ASSIGN)(&g_kt[d].str, lab + KT_LABEL_TEXT);
    if (g_kt_logged < KT_LOG_CHANGES) {
        g_kt_logged++;
        FILE *f = azlog_open("/tmp/azmod-hidetitle.log", "a");
        if (f) {
            fprintf(f, "key title: deck %d key \"%s\" (widget %#lx)\n", d + 1, g_kt[d].text,
                    (unsigned long) kw);
            fclose(f);
        }
    }
}
/* A whole tap on deck d's key (self = its TrackInfo widget, live: EP147 is calling its title
 * setter): claim it before Hide Track Title's step can, and leave it for the deck's next KeyWidget
 * update. A tap not on the drawn key, or one JUCE gives to something shown over the row, is not
 * the key's. */
static void kt_tap_key(void *self, int d) {
    unsigned long t = __atomic_load_n(&g_ht_tap, __ATOMIC_ACQUIRE);
    unsigned seq = (unsigned) (t >> 32), done = __atomic_load_n(&g_ht_tap_done, __ATOMIC_RELAXED);
    if (seq == done || now_ms() - __atomic_load_n(&g_ht_tap_ms, __ATOMIC_RELAXED) > HT_TAP_TTL_MS)
        return;
    int x = (int) (t & 0xffff), y = (int) (t >> 16 & 0xffff), on = kt_on();
    int shown = g_kt[d].drawn[0] != 0, in_key = shown && kt_in(g_kt[d].hit, x, y);
    if (!on || !in_key)
        return;
    uintptr_t comp = (uintptr_t) self + HT_COMP_OFF, par = ht_parent(comp), root = par;
    if (!par || (*(volatile uintptr_t *) par != HT_PI2D_CVT && *(volatile uintptr_t *) par != HT_PI4D_CVT))
        return;
    for (int i = 0; i < HT_COVER_DEPTH && ht_parent(root); i++)
        root = ht_parent(root);
    uintptr_t owner[HT_COVER_DECKS] = {0};
    owner[d] = par;
    int juce_ok = (ht_cover_mask(owner, root, x, y, ht_at, ht_parent) >> d) & 1u;
    if (kt_tap_owner(on, shown, in_key, juce_ok, ht_on(), 1) != KT_TAP_KEY)
        return;
    if (__atomic_compare_exchange_n(&g_ht_tap_done, &done, seq, 0, __ATOMIC_RELAXED,
                                    __ATOMIC_RELAXED)) {
        g_kt[d].pend = 1;
        kt_log("tap", d, x, y);
    }
}
/* the title setter, every frame for each deck on screen: age the copy, take a tap on the key,
 * put a closed panel back, and repaint the row when what it should show is not what it drew */
static void kt_title(void *self) {
    if (!__atomic_load_n(&g_kt_ok, __ATOMIC_ACQUIRE))
        return;
    int d = ht_deck((uintptr_t) self);
    if (d < 0)
        return;
    kt_age_tick(&g_kt[d].age);
    kt_tap_key(self, d);
    if (g_kt[d].pend && ++g_kt[d].pend > KT_PEND_FRAMES)
        g_kt[d].pend = 0; /* the deck's KeyWidget did not update: let it go */
    kt_panel_home(d);
    const char *want = kt_want(kt_on(), g_kt[d].age, g_kt[d].text);
    if (strcmp(want, g_kt[d].drawn)) {
        uintptr_t par = *(volatile uintptr_t *) ((char *) self + HT_COMP_OFF + 0x18);
        if (ht_ptr_ok(par))
            ((void (*)(void *)) JUCE_COMPONENT_REPAINT)((void *) par); /* the key is the parent's */
    }
}
/* The key in deck d's row, from the PlayerInfo component's paint (EP147 is painting it, so
 * its TrackInfo child ti and that child's title Label are alive): row[] from ht_row_ex,
 * bar[] from ht_row_ex, eye_left = the eye's left edge or -1. Called only when kt_plan_for says
 * the key shows. The key's box runs from the title Label's right edge (so the title, which EP147
 * fits inside its Label, never runs under the key) to the eye or the row's end; vertically it is
 * the eye's band, between ON AIR's bottom and the bar's bottom, so it never runs under the ON AIR
 * badge and does not move when the badge shows. */
static void kt_paint(void *g, int d, const int row[4], const int bar[3], uintptr_t ti, int eye_left) {
#ifdef SK_HAVE_HOOK
    if (!g_kt[d].text[0] || !g_kt[d].str || !ht_ptr_ok(ti))
        return;
    uintptr_t lab =
        kt_obj(*(volatile uintptr_t *) (ti - HT_COMP_OFF + KT_TITLE_LABEL), KT_LABEL_VT);
    if (!lab || *(volatile uintptr_t *) (lab + 0x18) != ti) /* the title Label, ti's child */
        return;
    int lx = *(volatile int *) (ti + 0x20) + *(volatile int *) (lab + 0x20),
        lw = *(volatile int *) (lab + 0x28);
    struct kt_rect r;
    if (!kt_key_rect(lx + lw, eye_left, row[0] + row[2], HT_PAD_RIGHT, bar[2], bar[1] - bar[2], &r))
        return;
    uint32_t col[2] = {0, 0}, dim[2] = {0, 0};
    kt_call_x8((void *) KT_FINDCOLOUR, col, lab, KT_TEXT_COLOUR_ID, 0.f);
    if (!((int (*)(uintptr_t)) KT_ISENABLED)(lab)) {
        kt_call_x8((void *) KT_MULALPHA, dim, (uintptr_t) col, 0, 0.5f);
        col[0] = dim[0];
    }
    int rect[4] = {r.x, r.y, r.w, r.h}, just = KT_JUST_RIGHT;
    ((void (*)(void *)) JUCE_G_SAVESTATE)(g);
    ((void (*)(void *, const uint32_t *)) KT_SETCOLOUR)(g, col);
    ((void (*)(void *, uintptr_t)) KT_SETFONT)(g, lab + KT_LABEL_FONT);
    ((void (*)(void *, const uintptr_t *, const int *, const int *, int, float)) KT_DRAWFITTED)(
        g, &g_kt[d].str, rect, &just, 1, KT_MIN_SCALE);
    ((void (*)(void *)) JUCE_G_RESTORESTATE)(g);
    strcpy(g_kt[d].drawn, g_kt[d].text);
    int sr[4]; /* the tap target and the panel's anchor, on screen (the row is ti's parent) */
    uintptr_t rowc = *(volatile uintptr_t *) (ti + 0x18);
    if (ht_ptr_ok(rowc) && ht_screen_rect(rowc, sr)) {
        kt_hit_rect(&r, sr[0], sr[1], bar[0], bar[1], g_kt[d].hit);
        g_kt[d].key_right = sr[0] + r.x + r.w;
        g_kt[d].bar_top = sr[1] + bar[0];
        g_kt[d].bar_bottom = sr[1] + bar[1];
    } else
        g_kt[d].hit[2] = 0;
#else
    (void) g, (void) d, (void) row, (void) bar, (void) ti, (void) eye_left;
#endif
}
/* Overlay thread, once EP147 runs: the four Strings start as "" (EP147's String ctor) */
static void kt_runtime_init(void) {
    if (__atomic_load_n(&g_kt_ok, __ATOMIC_ACQUIRE))
        return;
    for (int d = 0; d < 4; d++)
        ((void (*)(uintptr_t *, const char *)) JSTR_FROM_CSTR)(&g_kt[d].str, "");
    __atomic_store_n(&g_kt_ok, 1, __ATOMIC_RELEASE);
}
static int install_keytitle(void) {
    uintptr_t cur = 0;
    if (!img_q(KT_KEYW_VT, &cur) || !cur) /* the vtable is in EP147's image */
        return -2;
    return azm_install_hook(KT_KEYW_UPDATE, (void *) kt_keyw_hook, (void **) &g_kt_okeyw);
}
/* The eye (Hide Track Title only, with a title on deck d): at the right end of the title row of
 * deck d's PlayerInfo component, row[] and bar[] from ht_row_ex. Returns the eye's left
 * edge in the row's coordinates, or -1 when it draws none. */
static int ht_eye(void *g, int d, const int row[4], const int bar[3]) {
    unsigned bit = 1u << d;
    int k = (__atomic_load_n(&g_ht_hide, __ATOMIC_RELAXED) & bit) ? 1 : 0;
    if (!g_ht_eye[k]) {
        uintptr_t img = 0, q = 0;
        mp_image_from_memory(&img, k ? EYE_SHUT : EYE_OPEN,
                             k ? (int) sizeof EYE_SHUT : (int) sizeof EYE_OPEN);
        /* our own juce::Image, just decoded and held by this call: a plain load */
        if (!img || !own_q(img + 0x10, &q) || (int) (uint32_t) q != EYE_W ||
            (int) (uint32_t) (q >> 32) != EYE_H)
            return -1;
        g_ht_eye[k] = img;
    }
    /* Vertical: the space between ON AIR's bottom and the bar's bottom, less the ink, split
     * equally above and below. Right gap: HT_PAD_RIGHT in every layout. */
    int pad = (bar[1] - bar[2] - HT_INK_H) / 2;
    if (pad < 1)
        pad = 1;
    int ey = bar[2] + pad - HT_INK_T;
    int ex = row[0] + row[2] - HT_PAD_RIGHT - HT_INK_W - HT_INK_L;
    uintptr_t holder = g_ht_eye[k]; /* a juce::Image is its pixel-data pointer */
    ((void (*)(void *, uintptr_t *, int, int, int, int, int, int, int, int, int)) EP_23de290)(
        g, &holder, ex, ey, EYE_W, EYE_H, 0, 0, EYE_W, EYE_H, 0);
    g_ht_drawn |= bit;
    return ex + HT_INK_L;
}
/* PlayerInfo{2,4}DeckWidget's paintOverChildren, after EP147's own (empty) one: the one hook
 * the two title row features share. It resolves the deck and the row, then each feature draws
 * behind its own switch only (kt_plan_for): the eye with Hide Track Title on, the key with Key
 * in Track Title on, left of the eye when both show. With Hide Track Title off the key takes
 * the eye's place at the row's right end. */
static void kt_paint(void *g, int d, const int row[4], const int bar[3], uintptr_t ti, int eye_left);
static void ht_paintover(void *comp, void *g) {
    if (!g || !ht_ptr_ok((uintptr_t) comp))
        return;
    uintptr_t pv = *(volatile uintptr_t *) comp;
    uintptr_t full =
        (uintptr_t) comp + (uintptr_t) *(volatile long *) (pv - 0x10); /* offset-to-top */
    if (!ht_ptr_ok(full))
        return;
    uintptr_t fvt = *(volatile uintptr_t *) full;
    if (fvt != HT_PI2D_VT && fvt != HT_PI4D_VT)
        return;
    int d = *(volatile int *) (full + HT_DECK_OFF), row[4], bar[3];
    if (d < 0 || d >= 4)
        return;
    uintptr_t ti = 0;
    int have_row = ht_row_ex((uintptr_t) comp, row, &ti, bar);
    int ht = ht_on(), kt = kt_on();
    struct kt_plan plan =
        kt_plan_for(ht, (g_ht_has >> d) & 1u, kt, kt_want(kt, g_kt[d].age, g_kt[d].text));
    g_kt[d].drawn[0] = 0; /* kt_paint sets it to what it draws */
    if (!have_row) {
        if (ht)
            g_ht_drawn &= ~(1u << d);
        return;
    }
    if (ht && !plan.eye)
        g_ht_drawn &= ~(1u << d);
    int eye_left = plan.eye ? ht_eye(g, d, row, bar) : -1;
    if (plan.key)
        kt_paint(g, d, row, bar, ti, eye_left);
}
static void ht_poc2(void *c, void *g) {
    g_ht_opoc2(c, g);
    ht_paintover(c, g);
}
static void ht_poc4(void *c, void *g) {
    g_ht_opoc4(c, g);
    ht_paintover(c, g);
}
static int ht_patch_if(uintptr_t slot, uintptr_t want, void *hook, void **orig) {
    uintptr_t cur = 0;
    if (!img_q(slot, &cur) || cur != want) /* a vtable slot in EP147's image */
        return -2; /* not the expected function: leave it stock */
    return azm_patch_vtable(slot, hook, orig);
}
/* Always installed (inert while the mod is off), so the Mod Menu switch acts at once. */
/* Overlay thread, once: the empty juce::String the title hooks substitute. */
static void ht_runtime_init(void) {
    kt_runtime_init(); /* Key in Track Title's Strings: the same moment, its own state */
    if (ht_empty_ok())
        return;
    ((void (*)(void *, const char *)) JSTR_FROM_CSTR)(&g_ht_empty, "");
    __atomic_store_n(&g_ht_empty_ok, 1, __ATOMIC_RELEASE);
}
/* Constructor phase (install_static_hooks): slot patches only; no EP147 code is called. */
static void install_hidetitle(void) {
    int a = ht_patch_if(HT_SLOT2, HT_ORIG2, (void *) ht_hook2, (void **) &g_ht_orig2);
    int b = ht_patch_if(HT_SLOT4, HT_ORIG4, (void *) ht_hook4, (void **) &g_ht_orig4);
    int p2 =
        ht_patch_if(HT_PI2D_CVT + 27 * 8, HT_EMPTY_STUB, (void *) ht_poc2, (void **) &g_ht_opoc2);
    int p4 =
        ht_patch_if(HT_PI4D_CVT + 27 * 8, HT_EMPTY_STUB, (void *) ht_poc4, (void **) &g_ht_opoc4);
    int s2 = ht_patch_if(HT_SW_SLOT2, HT_SW_ORIG2, (void *) ht_sw2, (void **) &g_ht_osw2);
    int s4 = ht_patch_if(HT_SW_SLOT4, HT_SW_ORIG4, (void *) ht_sw4, (void **) &g_ht_osw4);
    int t = (a == 1 || b == 1) ? ht_install_xnext() : -9;
    int k = (a == 1 || b == 1) && (p2 == 1 || p4 == 1) ? install_keytitle() : -9;
    FILE *f = azlog_open("/tmp/azmod-hidetitle.log", "a");
    if (f) {
        fprintf(f, "install hidetitle: title %d/%d eye %d/%d strips %d/%d tap %d key %d\n", a, b, p2,
                p4, s2, s4, t, k);
        fclose(f);
    }
}

/* ---- Phase Meter (mod 36, Extras) -----------------------------------------------------
 * 2 DECK and 2/4 DECK: two bar rulers in the band above the two deck waveforms (freed by the
 * Phase Layout below), the upper waveform's deck on top. The master deck's ruler is orange,
 * the other blue; white beat ticks, a red downbeat, scrolling under the playhead. In phase =
 * the downbeats line up.
 *
 * Data = EP147's own phase meter (usecase::phase_meter), which runs with PRO DJ LINK off:
 *   LocalPlayer::update(PhDat_Set&, PlyState) 0xaa9a48, ~16/s per deck instance; deck
 *   index at +0x20 (ctor's PlayerID); +0xf0 = {u32 bar position, ms of real time; u32 BPM
 *   x100}. Inline-hooked: other bases reach it through thunks that call it directly, so
 *   a vtable-slot hook would miss callers. The master deck is g_master_deck, from
 *   MasterPlayer::onExchangedMasterPost (install_master_hook, shared with the drum roll).
 * Draw: WaveformView's own paintOverChildren, after EP147's. The decks are the two showing
 * waveform widgets (WWB census, mode widget vtable checked at use). Repaint: the band only
 * (0x2485d40 = internal repaint of an area, what Component::repaint() calls), from rtick. */
#define PM_LP_VT EP_26e1b68 /* LocalPlayer primary fns */
#define PM_LP_UPDATE EP_aa9a48
#define PM_REPAINT_AREA EP_2485d40 /* (Component*, const Rectangle<int>*) */
/* Geometry inside the deck waveform widget (1220 wide at x 40; only x 170..1085 shows between
 * the deck panel and the Beat FX panel): the band runs between the < > buttons. */
#define PM_X0 172  /* widget-local: screen x 212 */
#define PM_X1 1010 /* widget-local: screen x 1050 */
static long (*g_pm_olp)(void *, void *, int);
static _Atomic unsigned long g_pm_bar[4]; /* bpm100 << 32 | bar position ms */
static _Atomic long g_pm_t[4];            /* now_ms() of that sample */
static _Atomic int g_pm_moving[4];        /* bar position advanced since the last sample */
static _Atomic long g_pm_tr[4]; /* LocalPlayer +0xf8: track time (ms) of that same sample */
static long pm_lp_hook(void *self, void *set, int st) {
    long r = g_pm_olp(self, set, st);
    if (self && !((uintptr_t) self & 7) && *(volatile uintptr_t *) self == PM_LP_VT) {
        int d = *(volatile int *) ((char *) self + 0x20);
        if (d >= 0 && d < 4) {
            unsigned long v = *(volatile unsigned long *) ((char *) self + 0xf0);
            unsigned long old = __atomic_load_n(&g_pm_bar[d], __ATOMIC_RELAXED);
            __atomic_store_n(&g_pm_moving[d], (unsigned) v != (unsigned) old, __ATOMIC_RELAXED);
            __atomic_store_n(&g_pm_t[d], now_ms(), __ATOMIC_RELAXED);
            __atomic_store_n(&g_pm_tr[d], (long) *(volatile unsigned *) ((char *) self + 0xf8),
                             __ATOMIC_RELAXED);
            __atomic_store_n(&g_pm_bar[d], v, __ATOMIC_RELEASE);
        }
    }
    return r;
}
static int pm_on(void) {
    return az_on(AZF_PHASE_METER) &&
           az_active(AZF_PHASE_METER);
}
/* Each deck's waveform sits in a container (vt 0x2b6f460, 1220 x 178 in 2 DECK: top beat
 * ruler, waveform, bottom ruler + cue markers) whose children include EP147's playhead line
 * (vt 0x2b668b8, 5 px wide), read live so the meter follows it. The container's parent
 * (vt 0x2b747e0) also holds the loop-length readout (vt 0x2b63770), which shows inside the
 * band while a loop is active and is never painted over. pm_wavebox returns the container,
 * 0 if none is showing. */
#define PM_WAVEBOX_VT EP_2b6f460
#define PM_PLAYHEAD_VT EP_2b668b8
#define PM_WAVEOUT_VT EP_2b747e0
#define PM_LOOPLBL_VT EP_2b63770
static uintptr_t pm_child(uintptr_t comp, uintptr_t vt, int vis_only) {
    uintptr_t kids = *(volatile uintptr_t *) (comp + 0x40);
    int n = *(volatile int *) (comp + 0x50);
    for (int k = 0; ht_ptr_ok(kids) && k < n && k < 64; k++) {
        uintptr_t c = *(volatile uintptr_t *) (kids + 8 * k);
        if (ht_ptr_ok(c) && *(volatile uintptr_t *) c == vt &&
            (!vis_only || (*(volatile uint32_t *) (c + 0xc0) & 2)))
            return c;
    }
    return 0;
}
/* TRACK INFO (the WAVEFORM screen's top-right button): EP147 shows its panel by making
 * WaveformView's gui::LinkInfoWidget child (1280 x 800) visible, and hides it again on close.
 * That child's visible bit follows the panel; the view's other children do not change when it
 * opens or closes. The panel covers the whole waveform area, so the meter must not draw while
 * it is up. */
#define PM_INFO_VT EP_2cd4b28
static int pm_trackinfo_open(uintptr_t view) {
    return ht_ptr_ok(view) && pm_child(view, PM_INFO_VT, 1) != 0;
}
static uintptr_t pm_wavebox(int d, int r[4]) {
    int nb = __atomic_load_n(&g_wwbbase_n, __ATOMIC_RELAXED);
    for (int i = 0; i < nb && i < 8; i++) {
        if (g_wwbbase[i].deck != d)
            continue;
        for (int m = 0; m < 3; m++) {
            uintptr_t w = wwb_widget_live(i, m), t[4]; /* live + vtable checked at use */
            if (!w)
                continue;
            if (!ht_screen_rect(w + WIDGET_COMPONENT_OFF, (int *) t))
                continue;
            uintptr_t box = *(volatile uintptr_t *) (w + WIDGET_COMPONENT_OFF + 0x18);
            if (!ht_ptr_ok(box) || *(volatile uintptr_t *) box != PM_WAVEBOX_VT ||
                !ht_screen_rect(box, r))
                continue;
            return box;
        }
    }
    return 0;
}
/* Message thread: the last WaveformView painted (the juce::Component its paintOverChildren
 * received) and its vtable. g_pm_view is a wref slot: the component's destructor clears it. */
static _Atomic uintptr_t g_pm_view;
static uintptr_t g_pm_view_vt;
static int g_pm_band[4][4]; /* bands in view coordinates (x, y, w, h) */
static int g_pm_nband;      /* how many: 0 = none */
static void pm_fill(void *g, int x, int y, int w, int h, unsigned rgb) {
    unsigned char col[8] = {rgb & 0xff, rgb >> 8 & 0xff, rgb >> 16 & 0xff, 0xff, 0, 0, 0,
                            0}; /* B,G,R,A */
    ((void (*)(void *, void *)) SETCOLOUR_ADDR)(g, col);
    g_juce_fillrect(g, x, y, w, h);
}
/* Every mark must stay inside the band: pm_tick repaints only the band, so a pixel drawn
 * outside it is never erased and leaves trails. */
/* Smooth position. LocalPlayer::update publishes only every 64 ms, so the meter evaluates
 * EP147's beat grid at the waveform's own position every frame, which also follows scratches
 * and pauses:
 * - waveform position: the WWB position getter, vtable 0x2b6f208 slot +0x50 (0x1b8e580, a
 *   pure read: display position in track ms), what WaveformWidgetBase::update passes to
 *   updateWaveform. Called on the message thread, where EP147 calls it, for the showing WWB
 *   of that deck (deck from the WWB ctor, vtable checked at use);
 * - each 64 ms sample is a grid anchor: LocalPlayer +0xf8 track ms and +0xf0 bar position,
 *   read together, so no timestamps need matching;
 * - bar length in track ms (differs from the real-time bar by the tempo; mixing the two
 *   drifts) = dtrack / dfraction between two anchors 0.3-0.7 s of track apart.
 * fraction = anchor fraction + (waveform ms - anchor ms) / bar length. Nothing accumulates,
 * so nothing drifts. */
#define PM_WWB_VT EP_2b6f208
#define PM_WWB_POS 0x50 /* slot: int64 display position (track ms) */
static struct {
    long tr;
    double fr;
} g_pm_anc[4][32];
static int g_pm_ancn[4];
static long g_pm_anct[4];
static double g_pm_bartr[4]; /* bar length in track ms, 0 = not measured yet */
static int g_pm_barmiss[4];  /* estimates in a row that disagree (> 1 %) */
static int pm_wpos(int d, long *out) {
    int nb = __atomic_load_n(&g_wwbbase_n, __ATOMIC_RELAXED);
    for (int i = 0; i < nb && i < 8; i++) {
        if (g_wwbbase[i].deck != d)
            continue;
        uintptr_t w = (uintptr_t) g_wwbbase[i].base, vt = 0;
        int t[4], show = 0;
        for (int m = 0; m < 3 && !show; m++) {
            uintptr_t ww = wwb_widget_live(i, m); /* live + vtable checked at use */
            show = ww && ht_screen_rect(ww + WIDGET_COMPONENT_OFF, t);
        }
        /* a live embedded widget means its WWB is alive: its own class, checked at this use */
        if (!show || !own_q(w, &vt) || vt != PM_WWB_VT)
            continue;
        *out = ((long (*)(uintptr_t)) * (volatile uintptr_t *) (PM_WWB_VT + PM_WWB_POS))(w);
        return 1;
    }
    return 0;
}
/* Deck d's bar phase, beats [0, 4), from EP147's own phase meter (LocalPlayer) evaluated at the
 * deck's waveform position: 1 = known (a tempo in range). */
static int pm_local_phase(int d, double *out) {
    unsigned long v = __atomic_load_n(&g_pm_bar[d], __ATOMIC_ACQUIRE);
    unsigned bpm100 = (unsigned) (v >> 32), pos = (unsigned) v;
    if (bpm100 < 2000 || bpm100 > 40000)
        return 0; /* 20-400 BPM, else empty / unknown */
    double beat_ms = 6000000.0 / bpm100, bar_ms = 4 * beat_ms;
    double p = pos;
    long wp;
    long tr = __atomic_load_n(&g_pm_tr[d], __ATOMIC_RELAXED),
         ts = __atomic_load_n(&g_pm_t[d], __ATOMIC_RELAXED);
    double fr = fmod((double) pos / bar_ms, 1.0);
    if (ts != g_pm_anct[d]) { /* a new anchor */
        g_pm_anct[d] = ts;
        int n = g_pm_ancn[d]++;
        g_pm_anc[d][n & 31].tr = tr;
        g_pm_anc[d][n & 31].fr = fr;
        for (int k = 1; k < 32 && k <= n; k++) { /* an older one 300-700 ms of track away */
            int i = (n - k) & 31;
            long dtr = tr - g_pm_anc[d][i].tr;
            if (labs(dtr) < 300)
                continue;
            if (labs(dtr) > 700)
                break;
            double df = fr - g_pm_anc[d][i].fr;
            df -= floor(df + 0.5); /* to (-0.5, 0.5] */
            if (fabs(df) < 0.05)
                break;
            double L = dtr / df;
            if (L > 0.5 * bar_ms && L < 2.0 * bar_ms) {
                double c = g_pm_bartr[d];
                if (c <= 0)
                    g_pm_bartr[d] = L;
                else if (fabs(L - c) < 0.01 * c) {
                    g_pm_bartr[d] = 0.8 * c + 0.2 * L;
                    g_pm_barmiss[d] = 0;
                } /* refine */
                else if (++g_pm_barmiss[d] >= 3) {
                    g_pm_bartr[d] = L;
                    g_pm_barmiss[d] = 0;
                } /* 3 in a row: a new track */
            }
            break;
        }
    }
    if (pm_wpos(d, &wp)) {
        double L = g_pm_bartr[d] > 0 ? g_pm_bartr[d] : bar_ms; /* until measured: tempo 0 % */
        p = (fr + (double) (wp - tr) / L) * bar_ms;
    } else if (__atomic_load_n(&g_pm_moving[d], __ATOMIC_RELAXED))
        p += (double) (now_ms() - ts); /* no waveform showing: extrapolate in real time */
    p = fmod(p, bar_ms);
    if (p < 0)
        p += bar_ms;
    *out = p / beat_ms;
    return 1;
}
/* One ruler at bar phase `phase` (beats [0, 4)): the line, then ten beats across (CDJ-style),
 * a red downbeat centered on the line (height rh), white beat ticks dh long from the line
 * toward the middle (in = +1 down, -1 up, 0 both ways). The playhead (px) is at the phase.
 * Ticks are tw px wide (3 in the 2-deck band, 2 in the 4 DECK band), a downbeat one wider. */
static void pm_ruler_at(void *g, double phase, int x0, int w, int y, int px, int col, int rh,
                        int dh, int in, int tw) {
    double ppb = w / 10.0;
    pm_fill(g, x0, y, w, 2, col);
    for (int k = -12; k <= 12; k++) {
        int x = px + (int) lround((k - phase) * ppb);
        if (x < x0 + 2 || x > x0 + w - 4)
            continue;
        if (((k % 4) + 4) % 4 == 0)
            pm_fill(g, x - tw / 2 - 1, y + 1 - rh / 2, tw + 1, rh, 0xE62828); /* line = y, y+1 */
        else if (in > 0)
            pm_fill(g, x - tw / 2, y, tw, dh, 0xFFFFFF); /* from the line, toward the middle */
        else if (in < 0)
            pm_fill(g, x - tw / 2, y + 2 - dh, tw, dh, 0xFFFFFF);
        else
            pm_fill(g, x - tw / 2, y + 2 - dh, tw, 2 * dh - 2, 0xFFFFFF);
    }
}
/* Deck d has a track: its waveform widget is registered (pm_wavebox), which happens once a
 * track loads. An empty deck's phase is not a track's, so its ruler stays a bare line. */
static int pm_loaded(int d) {
    int r[4];
    return pm_wavebox(d, r) != 0;
}
/* Deck d's ruler (2 DECK and 2/4 DECK): the bare line while it has no track or its phase is
 * unknown. */
static void pm_ruler(void *g, int d, int x0, int w, int y, int px, int col, int rh, int dh,
                     int in) {
    double phase;
    if (pm_loaded(d) && pm_local_phase(d, &phase))
        pm_ruler_at(g, phase, x0, w, y, px, col, rh, dh, in, 3);
    else
        pm_fill(g, x0, y, w, 2, col);
}
/* ---- Phase Layout ----------------------------------------------------------------------
 * Both deck waveforms shorter, with the space above them freed for the Phase Meter.
 * EP147's WAVEFORM layout is data: WaveformView+0x190 = 2-deck field height, +0x198/+0x19c =
 * the two rows' y; its relayout 0x19dd430(view) (2 DECK and 2/4 DECK share the entries)
 * setBounds() the four DetailedWaveformFieldWidgets from it, and they re-render at that size.
 * Once per message-thread tick, compute the table the current mode wants (phase in 2 DECK =
 * mode 0 or 2/4 DECK = mode 1 while the Phase Meter is on); if the live table differs, write
 * it and call the relayout. That one check covers a mode switch, the mod's toggle and a
 * rebuilt view. The left DECK panels are not in that table: they are moved with setBounds to
 * match, re-asserted the same way, with stock bounds kept to restore. */
#define PL_RELAYOUT EP_19dd430  /* WaveformView relayout(this) */
#define PL_SETBOUNDS EP_249efe0 /* juce::Component::setBounds(this, x, y, w, h) */
#define PL_MODE 0x3a8           /* WaveformView: 0 = 2 DECK, 1 = 2/4 DECK, 2 = 4 DECK */
#define PL_VIEW_VT EP_29e9790
#define PL_HDR_VT EP_2c5ad08     /* view child: the status area */
#define PL_HDRBTN_VT EP_2c5bb40  /*   its child holding the mode buttons */
#define PL_IMGBTN_VT EP_29674c8  /* the image buttons (mode buttons, < >) */
#define PL_PANELS_VT EP_2d0b700  /* [view+0x428]: left deck panel container */
#define PL_PANELS2_VT EP_2cee580 /*   its child holding the 2-deck panels */
#define PL_PANEL_VT EP_2cfe0f0   /*   a 2-deck DECK panel */
#define PL_FXSIDE_VT EP_2d7a800  /* view child at the right: holds ">" */
#define PL_H 176                 /* phase field height (stock 198) */
#define PL_Y0 130                /* phase rows (stock 81 / 286) */
#define PL_Y1 308
#define PL_BAND_Y 72        /* the meter band: y 72 .. top waveform - 2 */
static uintptr_t g_pl_view; /* message thread: the WaveformView last painted */
static int g_pl_active;     /* the phase table is live in the view */
static int g_pl_stock[3];   /* the stock table, captured before the first write */
static int pl_view_ok(uintptr_t v) {
    return v && ht_ptr_ok(v) && *(volatile uintptr_t *) v == PL_VIEW_VT;
}
static uintptr_t pl_kidv(uintptr_t c, uintptr_t vt, int nth,
                         int vis_only) { /* nth child with that vtable */
    if (!ht_ptr_ok(c))
        return 0;
    uintptr_t kids = *(volatile uintptr_t *) (c + 0x40);
    int n = *(volatile int *) (c + 0x50);
    for (int k = 0; ht_ptr_ok(kids) && k < n && k < 64; k++) {
        uintptr_t x = *(volatile uintptr_t *) (kids + 8 * k);
        if (!ht_ptr_ok(x) || *(volatile uintptr_t *) x != vt)
            continue;
        if (vis_only && !(*(volatile uint32_t *) (x + 0xc0) & 2))
            continue;
        if (nth-- == 0)
            return x;
    }
    return 0;
}
static uintptr_t pl_kid(uintptr_t c, uintptr_t vt, int nth) {
    return pl_kidv(c, vt, nth, 0);
}
#define PL_NMOVED 160 /* 2 DECK: 4; 4 DECK: panels, number boxes, children, lane boxes */
static struct {
    uintptr_t c, vt;
    int b[4];
} g_pl_moved[PL_NMOVED]; /* stock bounds of what we moved */
static void pl_bounds(uintptr_t c, int x, int y, int w, int h) {
    if (*(volatile int *) (c + 0x20) == x && *(volatile int *) (c + 0x24) == y &&
        *(volatile int *) (c + 0x28) == w && *(volatile int *) (c + 0x2c) == h)
        return;
    ((void (*)(uintptr_t, int, int, int, int)) PL_SETBOUNDS)(c, x, y, w, h);
}
/* c's stock bounds (x, y, w, h), remembered when it is first moved (on = 1: what it has then is
 * stock); 0 when it was never moved, or no slot is free. *slot = where they are kept. */
static int *pl_stock(uintptr_t c, int on, int *slot) {
    if (!c)
        return 0;
    int i, f = -1;
    for (i = 0; i < PL_NMOVED; i++) {
        if (g_pl_moved[i].c == c && g_pl_moved[i].vt == *(volatile uintptr_t *) c)
            break;
        if (f < 0 && (!g_pl_moved[i].c || !ht_ptr_ok(g_pl_moved[i].c) ||
                      *(volatile uintptr_t *) g_pl_moved[i].c != g_pl_moved[i].vt))
            f = i;
    }
    if (i == PL_NMOVED) {
        if (!on || f < 0)
            return 0;
        i = f;
        g_pl_moved[i].c = c;
        g_pl_moved[i].vt = *(volatile uintptr_t *) c;
        for (int k = 0; k < 4; k++)
            g_pl_moved[i].b[k] = *(volatile int *) (c + 0x20 + 4 * k);
    }
    *slot = i;
    return g_pl_moved[i].b;
}
/* c back to its stock bounds, if it was moved */
static void pl_restore(uintptr_t c) {
    int i, *b = pl_stock(c, 0, &i);
    if (!b)
        return;
    pl_bounds(c, b[0], b[1], b[2], b[3]);
    g_pl_moved[i].c = 0;
}
/* move c by (dy, dh) from its stock y and height, or back to them (on = 0). Its x and width
 * stay EP147's own: collapsing the left panels narrows their field and moves the number boxes
 * (x and width), and a layout that wrote those back would undo the collapse. Only a sideways
 * move (dx != 0, out of sight) owns x, and puts it back with the rest. */
static void pl_move(uintptr_t c, int on, int dx, int dy, int dh) {
    int i, *b = pl_stock(c, on, &i);
    if (!b)
        return;
    int x = *(volatile int *) (c + 0x20), w = *(volatile int *) (c + 0x28);
    if (dx)
        x = on ? b[0] + dx : b[0];
    if (on) {
        pl_bounds(c, x, b[1] + dy, w, b[3] + dh);
        return;
    }
    pl_bounds(c, x, b[1], w, b[3]);
    g_pl_moved[i].c = 0;
}
/* The shorter phase panels (178 px, stock 200) cut the 4th row (loop / Q, panel y 159..200)
 * in half, so any visible child whose bottom is below the panel's live height is hidden with
 * juce::Component::setVisible, re-asserted each tick (EP147 may show it again), and exactly
 * those are shown again when the layout goes off. Each remembered child is checked by
 * alignment + its own vtable before use. */
static struct {
    uintptr_t c, vt;
} g_pl_hid[8];
static void pl_rows(uintptr_t p, int on) {
    if (!on) {
        for (int i = 0; i < 8; i++) {
            uintptr_t c = g_pl_hid[i].c;
            if (!c)
                continue;
            g_pl_hid[i].c = 0;
            if ((c & 7) || !ht_ptr_ok(c) || *(volatile uintptr_t *) c != g_pl_hid[i].vt)
                continue; /* gone: nothing to restore */
            ((void (*)(uintptr_t, int)) JUCE_SETVISIBLE)(c, 1);
        }
        return;
    }
    if (!p || !ht_ptr_ok(p))
        return;
    int ph = *(volatile int *) (p + 0x2c);
    uintptr_t kids = *(volatile uintptr_t *) (p + 0x40);
    int n = *(volatile int *) (p + 0x50);
    for (int k = 0; ht_ptr_ok(kids) && k < n && k < 32; k++) {
        uintptr_t c = *(volatile uintptr_t *) (kids + 8 * k);
        if ((c & 7) || !ht_ptr_ok(c) || !(*(volatile uint32_t *) (c + 0xc0) & 2))
            continue;
        if (*(volatile int *) (c + 0x24) + *(volatile int *) (c + 0x2c) <= ph)
            continue; /* fits */
        int i, f = -1;
        for (i = 0; i < 8; i++) {
            if (g_pl_hid[i].c == c)
                break;
            if (f < 0 && !g_pl_hid[i].c)
                f = i;
        }
        if (i == 8) {
            if (f < 0)
                continue;
            g_pl_hid[f].c = c;
            g_pl_hid[f].vt = *(volatile uintptr_t *) c;
        }
        ((void (*)(uintptr_t, int)) JUCE_SETVISIBLE)(c, 0);
    }
}
/* 4 DECK: the Phase Meter's band and Compact Track Info.
 * The 4-deck entries of the same table (+0x194 field height, +0x1a0..+0x1ac the four rows' y)
 * hold the lanes; EP147 sizes each lane's field from them every frame. A stock lane is 114 px:
 * its waveform box (92 px) at the top, 22 px empty below it, 8 px to the next lane.
 * The meter: the four decks' rulers together in a band above all lanes, where 2 DECK has its
 * meter. The first lane moves down PM4T_DOWN px and the lanes sit PM4T_PITCH apart.
 * Compact Track Info: BROWSE's one-row deck strip (ci_tick) replaces the two-row track info at
 * the bottom, and the lanes share the space down to it, CI_PITCH px each (CI_TOP_PITCH with the
 * meter's band): taller fields, and taller waveforms (pl_lane: each lane box's render height).
 * Wherever the lanes move, each left DECK panel (gui::waveform::DeckInfo4DeckWidget: the source,
 * KEY and BEAT JUMP rows in 114 px) and the deck number box beside it (DeckSetup4DeckWidget)
 * move with their lane and take its height, each of their children moved and sized in
 * proportion, so every row stays; their containers grow with them.
 * While the meter shows (the 4 DECK band, or the 2 DECK band with the phase layout), EP147's own
 * countdown in each field (gui::detailed_waveform::CueCountdownWidget, shown for the selected
 * deck of each side) moves out of sight: the meter's rows show each deck's own. Everything moved keeps its stock bounds, which come back with the stock table. */
#define PM4_SHIFT 22    /* how much each field shrinks under the meter */
#define PM4T_DOWN 70    /* the meter: how far the first lane moves down */
#define PM4T_PITCH 104  /* the meter: lane to lane (stock 122) */
#define PM4T_PANEL 98   /* the meter: a left panel's and a number box's height (stock 114) */
#define CI_PITCH 154    /* Compact Track Info: lane to lane, down to the strip */
#define CI_TOP_PITCH 136 /* Compact Track Info with the meter's band */
#define CI_GAP 6        /* between a lane and the next one's panel */
#define CI_RENDER_LESS 16 /* a lane box renders its waveform this much shorter than itself */
#define PM4_CDW_VT EP_2b62268    /* gui::detailed_waveform::CueCountdownWidget (its Component) */
#define PM4_PANELS_VT EP_2cfe3a8 /* [view+0x428] child: gui::waveform::DeckInfo4DeckFieldWidget */
#define PM4_PANEL_VT EP_2cfe5c0  /*   a left DECK panel: DeckInfo4DeckWidget */
#define PM4_NUMS_VT EP_2d0dc08   /* [view+0x428] child: DeckSetup4DeckFieldWidget */
#define PM4_NUM_VT EP_2d0ddc0    /*   a deck number box: DeckSetup4DeckWidget */
static int g_pl_active4;   /* the meter's 4-deck layout live in the view: 0 none, 1 the band */
static int g_pl_stock4[5]; /* the stock 4-deck height and rows, captured before the first write */
static int g_pm4_row[4];   /* the four lanes' y, top to bottom, as laid out now */
static int g_pm4_pitch;    /* and lane to lane */
static int ci_on(void) {
    return az_on(AZF_COMPACT_INFO) && az_active(AZF_COMPACT_INFO);
}
/* One 4-deck layout, from the stock table: the field height and rows (the table), where each
 * lane's left panel goes in its container (py, ph; ph 0 = stock), and the lanes' render height
 * (0 = stock). */
struct pl4 {
    int f, row[4], py[4], ph, render, pitch;
};
static void pl_layout4(int meter, int compact, struct pl4 *L) {
    const int *s = g_pl_stock4;
    memset(L, 0, sizeof *L);
    L->f = s[0];
    L->pitch = s[2] - s[1];
    for (int i = 0; i < 4; i++)
        L->row[i] = s[1 + i];
    if (!compact) {
        if (meter) {
            L->f = s[0] - PM4_SHIFT;
            L->pitch = PM4T_PITCH;
            L->ph = PM4T_PANEL;
            for (int i = 0; i < 4; i++) {
                L->row[i] = s[1] + PM4T_DOWN + i * PM4T_PITCH;
                L->py[i] = PM4T_DOWN + i * PM4T_PITCH;
            }
        }
        return;
    }
    int pitch = meter ? CI_TOP_PITCH : CI_PITCH, down = meter ? PM4T_DOWN : 0;
    L->pitch = pitch;
    L->f = pitch - CI_GAP - (meter ? 0 : 2);
    L->ph = L->f;
    L->render = L->f - CI_RENDER_LESS;
    for (int i = 0; i < 4; i++) {
        L->row[i] = s[1] + down + i * pitch;
        L->py[i] = down + i * pitch;
    }
}
/* 4 DECK Order 1 2 3 4: 1 = decks 1 2 3 4 top to bottom, 0 = EP147's own 3 1 2 4 */
static int pl_order(void) {
    return az_on(AZF_DECK_ORDER_4DECK) && az_active(AZF_DECK_ORDER_4DECK);
}
/* deck d's lane, 0 = top. Everything in a lane is found by its deck, never by where it is:
 * the view keeps deck d's waveform field, DeckInfo4DeckFieldWidget its left panel and
 * DeckSetup4DeckFieldWidget its number box each in a member of their own (both builds, from
 * the constructors; WaveformView::mouseDown hit-tests the fields the same way). */
static int pl_lane_of(int d) {
    static const int stock[4] = {1, 2, 0, 3};
    return d < 0 || d > 3 ? -1 : pl_order() == 1 ? d
                                                 : stock[d];
}
static const int PL_FIELD_OFF[4] = {0x3f0, 0x3f8, 0x418, 0x420}; /* WaveformView: deck d's field */
static const int PL_NUM_OFF[4] = {0xd0, 0xd8, 0xc8, 0xe0}; /* DeckSetup4DeckFieldWidget: its box */
/* deck d's DetailedWaveformFieldWidget Component in view v (a virtual base of the member's
 * object, at the offset its vtable names), 0 = none */
static uintptr_t pl_field(uintptr_t v, int d) {
    uintptr_t o = *(volatile uintptr_t *) (v + PL_FIELD_OFF[d]), vt;
    intptr_t vb = 0;
    if ((o & 7) || !ht_ptr_ok(o) || !(vt = *(volatile uintptr_t *) o) ||
        !img_q(vt - 0x18, (uintptr_t *) &vb) || vb <= 0 || vb > 0x4000)
        return 0;
    uintptr_t c = o + (uintptr_t) vb;
    return ht_ptr_ok(c) && *(volatile uintptr_t *) c == PM_WAVEOUT_VT ? c : 0;
}
static int pl_scale(int v, int from, int to) {
    return (int) lround((double) v * to / from);
}
/* A gui::ImagesComponent paints its image with drawImageAt(image, 0, 0) (1.30 0x19e2d68), at
 * the image's own size, so a number box's frame art (the whole box high) would only be cut off
 * by a shorter box: it keeps its bounds and is scaled to the box's height instead, with
 * juce::Component::setTransform (an AffineTransform {1, 0, 0, 0, sy, 0}; identity = none). */
#define PL_SETTRANSFORM EP_2487a80
static void pl_yscale(uintptr_t c, float sy) {
    const float m[6] = {1.0f, 0.0f, 0.0f, 0.0f, sy, 0.0f};
    ((void (*)(uintptr_t, const float *)) PL_SETTRANSFORM)(c, m);
}
/* A left panel or number box c to lane's place: in layout L (L->ph != 0) resized, its children
 * in proportion; else, when 4 DECK Order puts it in another lane, only moved to that lane's
 * stock place (a stock place is that lane's stock row less the first row's). Otherwise c and
 * its children back to stock. Each child's stock is kept before c is resized, as c's own
 * layout may run on the resize; the children are set after it. */
static void pl_squeeze(uintptr_t c, const struct pl4 *L, int lane) {
    int slot, want = L->ph > 0 || pl_order() != 0, *b = pl_stock(c, want, &slot), j = 0;
    if (!b)
        return;
    int s[4] = {b[0], b[1], b[2], b[3]};
    while (j < 4 && s[1] != g_pl_stock4[1 + j] - g_pl_stock4[1]) /* a stock lane's place */
        j++;
    int ty = L->ph > 0 ? L->py[lane] : g_pl_stock4[1 + lane] - g_pl_stock4[1];
    int on = want && j < 4 && s[3] > 40 && lane >= 0 && lane < 4 && (L->ph > 0 || ty != s[1]);
    int sq = on && L->ph > 0; /* squeezed: children in proportion; else only moved */
    uintptr_t kids = *(volatile uintptr_t *) (c + 0x40);
    int n = *(volatile int *) (c + 0x50);
    if (!ht_ptr_ok(kids))
        n = 0;
    if (sq)
        for (int k = 0; k < n && k < 32; k++) {
            uintptr_t x = *(volatile uintptr_t *) (kids + 8 * k);
            if (!(x & 7) && ht_ptr_ok(x))
                pl_stock(x, 1, &slot);
        }
    if (on)
        pl_bounds(c, s[0], ty, s[2], sq ? L->ph : s[3]);
    else
        pl_restore(c);
    on = sq;
    for (int k = 0; k < n && k < 32; k++) {
        uintptr_t x = *(volatile uintptr_t *) (kids + 8 * k);
        int *xb;
        if ((x & 7) || !ht_ptr_ok(x))
            continue;
        int art = *(volatile uintptr_t *) x == IMAGESCOMP_VT;
        if (!on) {
            if (art || pl_stock(x, 0, &slot))
                pl_yscale(x, 1.0f);
            pl_restore(x);
        } else if ((xb = pl_stock(x, 1, &slot))) {
            /* scaled, not cut: frame art, and a child as tall as the panel in a shorter panel
             * (KEY SHIFT's three buttons open over the panel's rows, 117 px from its top) */
            int whole = xb[1] == 0 && (art ? xb[3] == s[3] : xb[3] >= s[3] && L->ph < s[3]);
            pl_yscale(x, whole ? (float) L->ph / (float) s[3] : 1.0f);
            if (whole)
                pl_bounds(x, xb[0], 0, xb[2], xb[3]);
            else
                pl_bounds(x, xb[0], pl_scale(xb[1], s[3], L->ph), xb[2],
                          pl_scale(xb[3], s[3], L->ph));
        }
    }
}
/* A left DECK panel's background is its own paint (DeckInfo4DeckWidget slot 26, 1.30 0x1d7f848):
 * five fills at fixed heights for its stock 114 px (a black divider at x 170, the 0x252525 body
 * 117 high, the 0x232323 source row 41 high and two 0x323232 lines at y 38 and 76), so a panel
 * pl_squeeze has made taller or shorter keeps a stock-sized background under rows that moved.
 * While a panel is not at its stock height the same fills are drawn scaled to its height, as its
 * rows are; at stock height EP147's own paint runs. Message thread (paint), as pl_squeeze. */
#define PL_PANEL_PAINT EP_1d7f848
#define PL_PANEL_H 114
static void (*g_pl_opaint)(uintptr_t, void *);
static void pl_panel_paint_hook(uintptr_t c, void *g) {
    int slot, *b = pl_stock(c, 0, &slot), h = *(volatile int *) (c + 0x2c);
    if (!b || b[3] != PL_PANEL_H || h == PL_PANEL_H || h < 40) {
        g_pl_opaint(c, g);
        return;
    }
    pm_fill(g, 170, 0, 2, h, 0x000000);
    pm_fill(g, 0, 0, 170, pl_scale(117, PL_PANEL_H, h), 0x252525);
    pm_fill(g, 0, 0, 170, pl_scale(41, PL_PANEL_H, h), 0x232323);
    pm_fill(g, 0, pl_scale(38, PL_PANEL_H, h), 170, 2, 0x323232);
    pm_fill(g, 0, pl_scale(76, PL_PANEL_H, h), 170, 2, 0x323232);
}
/* ---- a lane's waveform box -------------------------------------------------------------
 * A lane's height on screen does not size its waveform: each DetailedWaveformWidget paints a
 * pre-rendered image (drawImageAt, 1.30 0x1e071a0) whose height is the lane box object's
 * (gui::detailed_waveform::own_deck::WaveformWidget) +0x5c, which its constructors set (box
 * height - 16: 76 in 4 DECK) and which is handed to the waveform's update every frame; EP147
 * draws the same lanes taller in 2 DECK and 2/4 DECK, so only what is still ours goes back. With a
 * taller lane the box, its lower cue marker row, its beat measure and its playhead take the
 * lane's height, and +0x5c the render height, never more than the waveform widget's own
 * height - 16 (the image it renders into is that high). The box's stock height and +0x5c
 * come back with the stock layout. */
#define OD_BOX_CVT EP_2b6f460  /* own_deck::WaveformWidget: its juce::Component's vtable */
#define OD_BOX_VT EP_2b6f208   /*   the object's primary vtable */
#define OD_RENDER_H 0x5c       /*   the waveform's render height */
#define OD_UNDER_VT EP_2b6ca28 /* own_deck::UnderCueMarkerCollectWidget (its Component) */
#define OD_BEAT_VT EP_2b60cf8  /* detailed_waveform::BeatMeasure (its Component) */
#define OD_DW_BLUE EP_2b66f68  /* the three DetailedWaveformWidgets (their Components) */
#define OD_DW_RGB EP_2b675d8
#define OD_DW_3B EP_2b67c48
#define PL_NBOX 8
static struct {
    uintptr_t box, vt; /* the box's Component and its vtable */
    int render;        /* its stock render height */
    int mine, h;       /* the render height and box height we wrote (0 = none) */
} g_pl_box[PL_NBOX];
/* the box object behind a box Component, by the vtable's offset-to-top; 0 = not one */
static uintptr_t pl_box_obj(uintptr_t box) {
    uintptr_t vt = *(volatile uintptr_t *) box;
    if (vt != OD_BOX_CVT)
        return 0;
    intptr_t off = *(const intptr_t *) (vt - 16);
    uintptr_t o = box + off;
    if (off >= 0 || !ht_ptr_ok(o) || *(volatile uintptr_t *) o != OD_BOX_VT)
        return 0;
    return o;
}
static int pl_box_slot(uintptr_t box, int add) {
    int f = -1;
    for (int i = 0; i < PL_NBOX; i++) {
        if (g_pl_box[i].box == box && g_pl_box[i].vt == *(volatile uintptr_t *) box)
            return i;
        if (f < 0 && (!g_pl_box[i].box || !ht_ptr_ok(g_pl_box[i].box) ||
                      *(volatile uintptr_t *) g_pl_box[i].box != g_pl_box[i].vt))
            f = i;
    }
    if (!add || f < 0)
        return -1;
    uintptr_t o = pl_box_obj(box);
    int r = o ? *(volatile int *) (o + OD_RENDER_H) : 0;
    if (r < 20 || r > 400)
        return -1; /* not a render height this could be */
    g_pl_box[f].box = box;
    g_pl_box[f].vt = *(volatile uintptr_t *) box;
    g_pl_box[f].render = r;
    g_pl_box[f].mine = g_pl_box[f].h = 0;
    return f;
}
/* field fw's box to height h with render height render; h 0 = back to stock, but only what is
 * still ours: EP147 gives the same boxes other geometry and render heights in 2 DECK and 2/4
 * DECK, and those stay; then the record is dropped. */
static void pl_lane(uintptr_t fw, int h, int render) {
    uintptr_t box = pl_kid(fw, OD_BOX_CVT, 0), o = box ? pl_box_obj(box) : 0;
    if (!o)
        return;
    int i = pl_box_slot(box, h > 0), slot;
    uintptr_t kids = *(volatile uintptr_t *) (box + 0x40);
    int n = ht_ptr_ok(kids) ? *(volatile int *) (box + 0x50) : 0, dwh = 0;
    for (int k = 0; k < n && k < 16; k++) {
        uintptr_t c = *(volatile uintptr_t *) (kids + 8 * k);
        if (!ht_ptr_ok(c))
            continue;
        uintptr_t cv = *(volatile uintptr_t *) c;
        if (cv == OD_DW_BLUE || cv == OD_DW_RGB || cv == OD_DW_3B)
            dwh = *(volatile int *) (c + 0x2c);
    }
    int *bb = i >= 0 ? pl_stock(box, h > 0, &slot) : 0;
    if (h <= 0 || i < 0 || !bb || dwh < 40) { /* back to stock, what is still ours */
        if (i < 0)
            return;
        int still = g_pl_box[i].h && *(volatile int *) (box + 0x2c) == g_pl_box[i].h;
        if (g_pl_box[i].mine && *(volatile int *) (o + OD_RENDER_H) == g_pl_box[i].mine)
            *(volatile int *) (o + OD_RENDER_H) = g_pl_box[i].render;
        for (int k = 0; k < n && k < 16; k++) {
            uintptr_t c = *(volatile uintptr_t *) (kids + 8 * k);
            int *cb = pl_stock(c, 0, &slot);
            if (still)
                pl_restore(c);
            else if (cb)
                g_pl_moved[slot].c = 0;
        }
        int *xb = pl_stock(box, 0, &slot);
        if (still)
            pl_restore(box);
        else if (xb)
            g_pl_moved[slot].c = 0;
        g_pl_box[i].box = 0;
        return;
    }
    if (render > dwh - CI_RENDER_LESS)
        render = dwh - CI_RENDER_LESS;
    pl_bounds(box, bb[0], bb[1], bb[2], h);
    g_pl_box[i].h = h;
    for (int k = 0; k < n && k < 16; k++) {
        uintptr_t c = *(volatile uintptr_t *) (kids + 8 * k);
        int *cb;
        if (!ht_ptr_ok(c) || !(cb = pl_stock(c, 1, &slot)))
            continue;
        uintptr_t cv = *(volatile uintptr_t *) c;
        if (cv == OD_UNDER_VT || cv == OD_BEAT_VT) /* the rows at the box's foot stay there */
            pl_bounds(c, cb[0], h - (bb[3] - cb[1]), cb[2], cb[3]);
        else if (cv == PM_PLAYHEAD_VT)
            pl_bounds(c, cb[0], cb[1], cb[2], h);
    }
    if (*(volatile int *) (o + OD_RENDER_H) != render)
        *(volatile int *) (o + OD_RENDER_H) = render;
    g_pl_box[i].mine = render;
}
/* ---- Compact Track Info: BROWSE's one-row deck strip on WAVEFORM ------------------------
 * BROWSE's gui::waveform::SimpleWaveformFieldWidget (the four decks' cells in one row, 105 px)
 * is a child of gui::browse::BrowseView. Each screen's view stays alive while another shows
 * (it is only detached from the main window), so the strip is found among BrowseView's
 * children: BrowseView as setvis_hook last saw it (EP147 shows it at start-up, on SOURCE),
 * a tracked pointer (ep_mem.h wref), else through the main window while BROWSE shows. Until
 * the strip is found 4 DECK keeps its stock track info. While 4 DECK shows
 * with the row on, the strip moves into the WAVEFORM view at the bottom with JUCE's own
 * removeChildComponent / addChildComponent (Key in Track Title's calls) and the view's
 * two-row track info (gui::waveform::PlayerInfoFieldWidget) hides; EP147 keeps updating the
 * strip there. EP147 moves the same strip into SOURCE's view and back to BrowseView as those
 * screens show. Any other screen, mode or the row off: if the strip is still where we put it,
 * it goes back to where we took it from (index and bounds); if EP147 has moved it since, it is
 * left there. The track info shows again. Every object is vtable-checked at use. */
#define CI_STRIP_VT EP_2ceae40   /* gui::waveform::SimpleWaveformFieldWidget */
#define CI_BROWSE_VT EP_29d67a0  /* gui::browse::BrowseView */
#define CI_INFO_VT EP_2d3ddc8    /* gui::waveform::PlayerInfoFieldWidget (the two-row track info) */
#define CI_WIN_VT EP_2968228     /* device_adapter::DefaultMainWindow */
static struct {
    uintptr_t win;      /* the main window (vt CI_WIN_VT) */
    uintptr_t strip, par; /* the strip and its BrowseView */
    int z, b[4];        /* its index there and its bounds */
    uintptr_t hid;      /* the track info we hid, 0 = none */
    uintptr_t in;       /* the WaveformView we moved the strip into, 0 = none */
    uintptr_t from, fvt; /* where it was then (EP147 moves it between screens) and its vtable */
    int fz, fb[4];      /* its index and bounds there */
} g_ci;
static int g_ci_bv_ok; /* g_bv_obj is tracked (install_phasemeter): readable on the message thread */
static int ci_ok(uintptr_t c, uintptr_t vt) {
    return c && ht_ptr_ok(c) && *(volatile uintptr_t *) c == vt;
}
/* the strip among BrowseView bv's children: 1 = found */
static int ci_find(uintptr_t bv) {
    uintptr_t k2 = *(volatile uintptr_t *) (bv + 0x40);
    int n2 = *(volatile int *) (bv + 0x50);
    for (int z = 0; ht_ptr_ok(k2) && z < n2 && z < 64; z++) {
        uintptr_t s = *(volatile uintptr_t *) (k2 + 8 * z);
        if (!ci_ok(s, CI_STRIP_VT))
            continue;
        g_ci.strip = s;
        g_ci.par = bv;
        g_ci.z = z;
        for (int i = 0; i < 4; i++)
            g_ci.b[i] = *(volatile int *) (s + 0x20 + 4 * i);
        return 1;
    }
    return 0;
}
/* the strip: under BrowseView as setvis_hook saw it (tracked, so a non-zero value on the message
 * thread is a live component), else under the main window (from the WAVEFORM view's parents
 * while it shows), BrowseView among its children's children while BROWSE shows */
static int ci_strip(uintptr_t v) {
    if (ci_ok(g_ci.strip, CI_STRIP_VT) && ci_ok(g_ci.par, CI_BROWSE_VT))
        return 1;
    g_ci.strip = 0;
    uintptr_t bv = __atomic_load_n(&g_ci_bv_ok, __ATOMIC_ACQUIRE) ? __atomic_load_n(&g_bv_obj, __ATOMIC_ACQUIRE) : 0;
    if (ci_ok(bv, CI_BROWSE_VT) && ci_find(bv))
        return 1;
    if (pl_view_ok(v)) {
        uintptr_t t = v;
        for (int i = 0; i < 8 && ht_ptr_ok(*(volatile uintptr_t *) (t + 0x18)); i++)
            t = *(volatile uintptr_t *) (t + 0x18);
        if (ci_ok(t, CI_WIN_VT))
            g_ci.win = t;
    }
    if (!ci_ok(g_ci.win, CI_WIN_VT))
        return 0;
    uintptr_t k1 = *(volatile uintptr_t *) (g_ci.win + 0x40);
    int n1 = *(volatile int *) (g_ci.win + 0x50);
    for (int a = 0; ht_ptr_ok(k1) && a < n1 && a < 16; a++) {
        uintptr_t c = *(volatile uintptr_t *) (k1 + 8 * a);
        if (!ht_ptr_ok(c))
            continue;
        bv = ci_ok(c, CI_BROWSE_VT) ? c : pl_kid(c, CI_BROWSE_VT, 0);
        if (bv && ci_find(bv))
            return 1;
    }
    return 0;
}
/* s's index among parent p's children, -1 = not there */
static int ci_index(uintptr_t p, uintptr_t s) {
    uintptr_t k = *(volatile uintptr_t *) (p + 0x40);
    int n = *(volatile int *) (p + 0x50);
    for (int z = 0; ht_ptr_ok(k) && z < n && z < 64; z++)
        if (*(volatile uintptr_t *) (k + 8 * z) == s)
            return z;
    return -1;
}
/* the strip into the view (on), or back where we took it from (off), the track info hidden /
 * shown. EP147 itself moves the one strip between BROWSE and SOURCE, so it goes back only while
 * it is still in the view we put it in: once EP147 has moved it, it is EP147's again. */
static void ci_place(uintptr_t v, int on) {
    uintptr_t s = g_ci.strip;
    if (!ci_ok(s, CI_STRIP_VT) || !ci_ok(g_ci.par, CI_BROWSE_VT))
        on = 0;
    uintptr_t cur = ci_ok(s, CI_STRIP_VT) ? *(volatile uintptr_t *) (s + 0x18) : 0;
    if (on) {
        if (cur != v) {
            g_ci.from = g_ci.fvt = 0;
            if (ht_ptr_ok(cur) && !(cur & 7) && (g_ci.fz = ci_index(cur, s)) >= 0) {
                g_ci.from = cur;
                g_ci.fvt = *(volatile uintptr_t *) cur;
                for (int i = 0; i < 4; i++)
                    g_ci.fb[i] = *(volatile int *) (s + 0x20 + 4 * i);
                ((void (*)(uintptr_t, uintptr_t)) KT_REMOVE_CHILD)(cur, s);
            }
            ((void (*)(uintptr_t, uintptr_t, int)) KT_ADD_CHILD)(v, s, -1);
            g_ci.in = v;
        }
        pl_bounds(s, g_ci.b[0], *(volatile int *) (v + 0x2c) - g_ci.b[3], g_ci.b[2], g_ci.b[3]);
        if (!kt_visible(s))
            kt_set_visible(s, 1);
        uintptr_t info = pl_kid(v, CI_INFO_VT, 0);
        if (info && kt_visible(info)) {
            kt_set_visible(info, 0);
            g_ci.hid = info;
        }
        return;
    }
    if (g_ci.in && ci_ok(s, CI_STRIP_VT) && cur == g_ci.in) {
        int back = ci_ok(g_ci.from, g_ci.fvt); /* else BrowseView, its home at start-up */
        ((void (*)(uintptr_t, uintptr_t)) KT_REMOVE_CHILD)(cur, s);
        if (back) {
            ((void (*)(uintptr_t, uintptr_t, int)) KT_ADD_CHILD)(g_ci.from, s, g_ci.fz);
            pl_bounds(s, g_ci.fb[0], g_ci.fb[1], g_ci.fb[2], g_ci.fb[3]);
        } else if (ci_ok(g_ci.par, CI_BROWSE_VT)) {
            ((void (*)(uintptr_t, uintptr_t, int)) KT_ADD_CHILD)(g_ci.par, s, g_ci.z);
            pl_bounds(s, g_ci.b[0], g_ci.b[1], g_ci.b[2], g_ci.b[3]);
        }
    }
    g_ci.in = 0;
    if (g_ci.hid) {
        if (ci_ok(g_ci.hid, CI_INFO_VT) && !kt_visible(g_ci.hid))
            kt_set_visible(g_ci.hid, 1);
        g_ci.hid = 0;
    }
}
/* ---- Compact Track Info: tap a deck's key in the strip ---------------------------------
 * The strip shows each deck's key in its cell (gui::waveform::SimpleWaveform4DeckWidget, a
 * juce::Label right of the title). A whole tap on it (Hide Track Title's XNextEvent record,
 * claimed once) opens that deck's key shift panel, EP147's own KeyShiftWidget from the deck's
 * left DECK panel, through the same gate as the panel's KEY row arrow (KT_ARROW_OK, then
 * setVisible), floated into the WAVEFORM view above the cell as Key in Track Title floats it
 * above a title row, so a collapsed tray cannot clip it. A second tap closes it. It goes back to
 * its own panel, index and bounds once it closes (EP147 closes it itself when the deck has no
 * track), or the strip leaves 4 DECK. Decks by identity: the strip's 4-deck field keeps deck
 * d's cell at +0xc8 + 8 d, DeckInfo4DeckFieldWidget its panel at +0xe8 + 8 d (both builds, from
 * their constructors), and the panel names its deck at +0xd0. */
#define CI_SW4_VT EP_2ce7020  /* gui::waveform::SimpleWaveform4DeckFieldWidget: the four cells */
#define CI_CELL_VT EP_2ce7c50 /* gui::waveform::SimpleWaveform4DeckWidget, its primary vtable */
#define CI_CELL0 0xc8         /* the field: deck d's cell */
#define CI_PANEL0 0xe8        /* DeckInfo4DeckFieldWidget: deck d's DeckInfo4DeckWidget */
static int ci_cell_key(void) {
    return ep_off(0x208, 0x270); /* a cell's key juce::Label */
}
static struct {
    uintptr_t c, par; /* the panel we floated (0 = none) and its DeckInfo4DeckWidget */
    int z, b[4];      /* its index and bounds there */
} g_ci_ks;
/* the floated panel back to its DeckInfo widget, unless keep and it is still open */
static void ci_ks_home(int keep) {
    uintptr_t c = g_ci_ks.c, par = g_ci_ks.par, kw = 0;
    if (!c)
        return;
    if (kt_panel_of(par, &kw) != c) { /* not the panel it was: nothing to put back */
        g_ci_ks.c = 0;
        return;
    }
    if (keep && kt_visible(c))
        return;
    if (kt_visible(c))
        kt_set_visible(c, 0);
    uintptr_t cur = *(volatile uintptr_t *) (c + 0x18);
    if (cur != par) {
        if (ht_ptr_ok(cur))
            ((void (*)(uintptr_t, uintptr_t)) KT_REMOVE_CHILD)(cur, c);
        ((void (*)(uintptr_t, uintptr_t, int)) KT_ADD_CHILD)(par, c, g_ci_ks.z);
    }
    pl_bounds(c, g_ci_ks.b[0], g_ci_ks.b[1], g_ci_ks.b[2], g_ci_ks.b[3]);
    g_ci_ks.c = 0;
}
/* deck d's key shift panel: closed if open, else opened above the key at r (screen) */
static void ci_ks_toggle(uintptr_t v, int d, const int r[4], int cell_top) {
    uintptr_t pc = *(volatile uintptr_t *) (v + 0x428), kw = 0;
    if (!ht_ptr_ok(pc) || *(volatile uintptr_t *) pc != PL_PANELS_VT)
        return;
    uintptr_t panels = pl_kid(pc, PM4_PANELS_VT, 0);
    uintptr_t par = panels ? kt_obj(*(volatile uintptr_t *) (panels + CI_PANEL0 + 8 * d), KT_DI4_VT) : 0;
    if (!par || *(volatile int *) (par + KT_DI_DECK) != d)
        return;
    uintptr_t c = kt_panel_of(par, &kw);
    if (!c)
        return;
    if (kt_visible(c)) { /* ours or opened by the tray's arrow: a tap on the key closes it */
        kt_set_visible(c, 0);
        if (g_ci_ks.c == c)
            ci_ks_home(0);
        return;
    }
    if (g_ci_ks.c)
        ci_ks_home(0);
    int area[4], x, y, z = kt_child_index(par, c);
    int w = *(volatile int *) (c + 0x28), h = *(volatile int *) (c + 0x2c);
    if (g_ci_ks.c || *(volatile uintptr_t *) (c + 0x18) != par || z < 0 ||
        !((int (*)(uintptr_t)) KT_ARROW_OK)(kw) || !ht_screen_rect(v, area) ||
        !kt_panel_at(w, h, r[0] + r[2], cell_top, r[1] + r[3], area, KT_PANEL_TOP, &x, &y))
        return;
    g_ci_ks.z = z;
    for (int i = 0; i < 4; i++)
        g_ci_ks.b[i] = *(volatile int *) (c + 0x20 + 4 * i);
    ((void (*)(uintptr_t, uintptr_t)) KT_REMOVE_CHILD)(par, c);
    ((void (*)(uintptr_t, uintptr_t, int)) KT_ADD_CHILD)(v, c, -1);
    pl_bounds(c, x - area[0], y - area[1], w, h);
    g_ci_ks.c = c;
    g_ci_ks.par = par;
    kt_set_visible(c, 1);
}
/* Every tick from pl_tick4: the floated panel home when it should go, then a tap on a key */
static void ci_key_tap(uintptr_t v, int compact) {
    ci_ks_home(compact);
    if (!compact)
        return;
    unsigned long t = __atomic_load_n(&g_ht_tap, __ATOMIC_ACQUIRE);
    unsigned seq = (unsigned) (t >> 32), done = __atomic_load_n(&g_ht_tap_done, __ATOMIC_RELAXED);
    if (seq == done || now_ms() - __atomic_load_n(&g_ht_tap_ms, __ATOMIC_RELAXED) > HT_TAP_TTL_MS)
        return;
    int x = (int) (t & 0xffff), y = (int) (t >> 16 & 0xffff);
    uintptr_t field = pl_kid(g_ci.strip, CI_SW4_VT, 0), root = v;
    if (!field)
        return;
    for (int i = 0; i < HT_COVER_DEPTH && ht_parent(root); i++)
        root = ht_parent(root);
    for (int d = 0; d < 4; d++) {
        uintptr_t cell = kt_obj(*(volatile uintptr_t *) (field + CI_CELL0 + 8 * d), CI_CELL_VT);
        uintptr_t key = cell ? kt_obj(*(volatile uintptr_t *) (cell + ci_cell_key()), KT_LABEL_VT) : 0;
        int r[4], cr[4];
        if (!key || !kt_visible(key) || !ht_screen_rect(key, r) || x < r[0] || x >= r[0] + r[2] ||
            y < r[1] || y >= r[1] + r[3])
            continue;
        uintptr_t owner = ht_parent(key); /* the cell's Component: nothing shown over it */
        if (!ht_hit_in(ht_at(root, x, y), owner, ht_parent) || !ht_screen_rect(owner, cr))
            return;
        if (__atomic_compare_exchange_n(&g_ht_tap_done, &done, seq, 0, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
            ci_ks_toggle(v, d, r, cr[1]);
        return;
    }
}
static void pl_tick4(uintptr_t v, int mode) {
    int on4 = mode == 2 && ht_ptr_ok(*(volatile uintptr_t *) (v + 0x18)); /* 4 DECK, on screen */
    int meter = on4 && pm_on(); /* the band above the lanes */
    int have = ci_on() && ci_strip(v); /* looked for while BROWSE shows, too */
    int compact = on4 && have;
    volatile int *t = (volatile int *) (v + 0x190);
    int cur[5] = {t[1], t[4], t[5], t[6], t[7]};
    /* ours = the table one of our layouts writes from the stock we kept; else it is stock */
    int ours = 0;
    struct pl4 L;
    for (int m = 0; m < 2 && g_pl_stock4[0] && !ours; m++)
        for (int c = 0; c < 2 && !ours; c++) {
            if (!m && !c)
                continue;
            pl_layout4(m, c, &L);
            ours = cur[0] == L.f && cur[1] == L.row[0] && cur[2] == L.row[1] &&
                   cur[3] == L.row[2] && cur[4] == L.row[3];
        }
    if (!ours)
        memcpy(g_pl_stock4, cur, sizeof cur);
    if (g_pl_stock4[0] <= PM4_SHIFT + 80) {
        ci_key_tap(v, 0);
        ci_place(v, 0);
        return; /* never seen a stock table this could come from: touch nothing */
    }
    /* a layout only on a stock table like EP147's: rows in order at least PM4T_PITCH apart,
     * the last lane ending no lower than it did */
    int fits = 1;
    for (int i = 1; i < 4; i++)
        fits = fits && g_pl_stock4[1 + i] - g_pl_stock4[i] >= PM4T_PITCH;
    if (!fits)
        meter = 0, compact = 0;
    pl_layout4(meter, compact, &L);
    if (!compact && L.row[3] + L.f > g_pl_stock4[4] + g_pl_stock4[0])
        pl_layout4(meter = 0, 0, &L); /* the band would end below the stock lanes */
    int wv[5] = {L.f, L.row[0], L.row[1], L.row[2], L.row[3]};
    if (memcmp(cur, wv, sizeof cur)) {
        t[1] = wv[0];
        t[4] = wv[1];
        t[5] = wv[2];
        t[6] = wv[3];
        t[7] = wv[4];
    }
    memcpy(g_pm4_row, L.row, sizeof g_pm4_row);
    g_pm4_pitch = L.pitch;
    ci_place(v, compact);
    ci_key_tap(v, compact);
    /* EP147 reads the 4-deck entries only when it enters 4 DECK (its relayout lays out the
     * 2-deck rows), so in 4 DECK each deck's field is set to its lane's row here (4 DECK
     * Order: pl_lane_of). */
    uintptr_t fws[4];
    int nf = 0;
    for (int d = 0; d < 4; d++) {
        uintptr_t fw = pl_field(v, d);
        fws[d] = fw && mode == 2 && (*(volatile uint32_t *) (fw + 0xc0) & 2) ? fw : 0;
        nf += fws[d] != 0;
        if (fw) {
            pl_move(pl_kid(fw, PM4_CDW_VT, 0), meter || (mode != 2 && pm_on() && g_pl_active), -4000, 0, 0);
            if (!compact)
                pl_lane(fw, 0, 0);
        }
    }
    for (int d = 0; nf == 4 && d < 4; d++) {
        pl_bounds(fws[d], *(volatile int *) (fws[d] + 0x20), wv[1 + pl_lane_of(d)],
                  *(volatile int *) (fws[d] + 0x28), wv[0]);
        if (compact)
            pl_lane(fws[d], L.f, L.render);
    }
    uintptr_t pc = *(volatile uintptr_t *) (v + 0x428);
    if (pc && ht_ptr_ok(pc) && *(volatile uintptr_t *) pc == PL_PANELS_VT) {
        uintptr_t panels = pl_kid(pc, PM4_PANELS_VT, 0), nums = pl_kid(pc, PM4_NUMS_VT, 0);
        /* the containers reach down to the strip while the lanes do */
        int slot, *pb = pl_stock(pc, compact, &slot);
        int grow = compact && pb ? *(volatile int *) (v + 0x2c) - g_ci.b[3] - (pb[1] + pb[3]) : 0;
        pl_move(pc, compact && grow > 0, 0, 0, grow);
        pl_move(panels, compact && grow > 0, 0, 0, grow);
        pl_move(nums, compact && grow > 0, 0, 0, grow);
        for (int d = 0; d < 4; d++) { /* each deck's panel and box, by the member that holds it */
            uintptr_t p = panels ? kt_obj(*(volatile uintptr_t *) (panels + CI_PANEL0 + 8 * d), PM4_PANEL_VT) : 0;
            uintptr_t b = nums ? kt_obj(*(volatile uintptr_t *) (nums + PL_NUM_OFF[d]), PM4_NUM_VT) : 0;
            pl_squeeze(p, &L, pl_lane_of(d));
            pl_squeeze(b, &L, pl_lane_of(d));
        }
    }
    g_pl_active4 = meter;
}
static void pl_tick(void) {
    uintptr_t v = g_pl_view;
    if (!pl_view_ok(v)) {
        g_pl_view = 0;
        return;
    }
    int mode = *(volatile int *) (v + PL_MODE);
    if (mode >= 0 && mode <= 2)
        __atomic_store_n(&g_wv_mode, mode, __ATOMIC_RELAXED);
    pl_tick4(v, mode);
    int want = pm_on() && (mode == 0 || mode == 1); /* 2 DECK and 2/4 DECK */
    volatile int *t = (volatile int *) (v + 0x190);
    int cur_h = t[0], cur_a = t[2], cur_b = t[3];
    if (!(cur_h == PL_H && cur_a == PL_Y0 && cur_b == PL_Y1)) {
        g_pl_stock[0] = cur_h;
        g_pl_stock[1] = cur_a;
        g_pl_stock[2] = cur_b;
    }
    if (!g_pl_stock[0])
        return; /* never seen stock: touch nothing */
    int wh = want ? PL_H : g_pl_stock[0], wa = want ? PL_Y0 : g_pl_stock[1],
        wb = want ? PL_Y1 : g_pl_stock[2];
    if (cur_h != wh || cur_a != wa || cur_b != wb) {
        t[0] = wh;
        t[2] = wa;
        t[3] = wb;
        if (mode != 2)
            ((void (*)(uintptr_t)) PL_RELAYOUT)(v); /* EP147 re-lays-out itself */
    }
    /* left panels with their rows */
    uintptr_t q =
        pl_kid(*(volatile uintptr_t *) (v + 0x428) &&
                       *(volatile uintptr_t *) *(volatile uintptr_t *) (v + 0x428) == PL_PANELS_VT
                   ? *(volatile uintptr_t *) (v + 0x428)
                   : 0,
               PL_PANELS2_VT, 0);
    uintptr_t p0 = pl_kid(q, PL_PANEL_VT, 0), p1 = pl_kid(q, PL_PANEL_VT, 1);
    if (p0 && p1 && *(volatile int *) (p1 + 0x24) < *(volatile int *) (p0 + 0x24)) {
        uintptr_t x = p0;
        p0 = p1;
        p1 = x;
    }
    int dh = PL_H - (g_pl_stock[0] ? g_pl_stock[0] : PL_H);
    pl_move(p0, want, 0, PL_Y0 - g_pl_stock[1], dh);
    pl_move(p1, want, 0, PL_Y1 - g_pl_stock[2], dh);
    if (want) {
        pl_rows(p0, 1);
        pl_rows(p1, 1);
    } else
        pl_rows(0, 0);
    uintptr_t pc = *(volatile uintptr_t *) (v + 0x428);
    uintptr_t lt = (pc && ht_ptr_ok(pc) && *(volatile uintptr_t *) pc == PL_PANELS_VT)
                       ? pl_kidv(pc, PL_IMGBTN_VT, 0, 1)
                       : 0; /* the visible one: the container holds hidden ones too */
    uintptr_t gt = pl_kidv(pl_kid(v, PL_FXSIDE_VT, 0), PL_IMGBTN_VT, 0, 1);
    /* < > stay where EP147 puts them; on = 0 only restores one moved earlier */
    if (lt)
        pl_move(lt, 0, 0, 0, 0);
    if (gt)
        pl_move(gt, 0, 0, 0, 0);
    g_pl_active = want;
}
/* ---- Phase Meter in 4 DECK -------------------------------------------------------------
 * The four decks' meters together in a band above the lanes (pl_tick4 makes the room,
 * pm4_paint_top draws it): one row per lane in the lanes' order, each like the CDJ-3000's ruler,
 * ten beats across with red downbeats and white beat ticks, and a white line at EP147's playhead
 * through the band. On the left of each row the deck's number (an orange box for the master) and
 * its memory cue countdown, red under four bars to its cue, "--.-" when there is none or it is
 * not known. A lane that shows a PRO DJ LINK player (Link Waveforms) has that player's phase and
 * countdown; an external master not shown in a lane, its beat packets.
 *
 * Countdowns of the AZ's own decks: EP147's own gui::CueCountdown of each deck model (+0x160,
 * beats to the next memory cue, 0x80000000 none), read after EP147's per-deck model update
 * (inline hook on that update: model +0 is its deck index, CueState at +0x1040, its
 * CueCountdown at +0x20, both checked by their shared_ptr blocks).
 * Text: EP147's own UI font (its font factory, "PDJ-TsukuGoCustom Pro" at a height) with
 * juce::Graphics drawFittedText, the Strings and Fonts built once on the message thread. */
#define PM_MODEL_UPDATE EP_1fc9a80     /* gui deck model: the per-deck update (model, PlayerState) */
#define PM_CUESTATE_CTRL_VT EP_2dedd88 /* the shared_ptr block of gui::CueState (object +0x10) */
#define PM_COUNTDOWN_CTRL_VT EP_2dd5300 /* the shared_ptr block of gui::CueCountdown (+0x10) */
#define PM_FONT_GET EP_1f4a5b0 /* EP147's UI font at a height: (float), juce::Font through x8 */
#define PM_CUE_NONE 0x80000000u
#define PM_CUE_NEAR 15 /* beats: a countdown is red under four bars (the CDJ-3000's 03.3) */
static _Atomic uint32_t g_pm_cue[4] = {PM_CUE_NONE, PM_CUE_NONE, PM_CUE_NONE, PM_CUE_NONE};
static long (*g_pm_omodel)(void *, void *);
static long pm_model_hook(void *model, void *ps) {
    long r = g_pm_omodel(model, ps);
    uintptr_t m = (uintptr_t) model, idx = 0, cs = 0, csc = 0, cd = 0, cdc = 0, cvt = 0, v = 0;
    if (!pm_on() || !own_q(m, &idx) || (uint32_t) idx > 3)
        return r;
    uint32_t val = PM_CUE_NONE;
    if (own_q(m + 0x1040, &cs) && own_q(m + 0x1048, &csc) && own_q(csc, &cvt) &&
        cvt == PM_CUESTATE_CTRL_VT && cs == csc + 0x10 && own_q(cs + 0x20, &cd) &&
        own_q(cs + 0x28, &cdc) && own_q(cdc, &cvt) && cvt == PM_COUNTDOWN_CTRL_VT &&
        cd == cdc + 0x10 && own_q(cd + 0x160, &v))
        val = (uint32_t) v;
    __atomic_store_n(&g_pm_cue[(uint32_t) idx], val, __ATOMIC_RELAXED);
    return r;
}
/* "BB.b" as the deck shows a countdown of n beats: 0 = "00.0", else bars (n - 1) / 4 and beat
 * (n - 1) % 4 + 1; anything past what a deck counts (PM_CD_MAX) is none, "--.-". */
#define PM_CD_MAX 256
static void pm_cd_fmt(uint32_t n, char t[8]) {
    if (n > PM_CD_MAX)
        strcpy(t, "--.-");
    else if (n == 0)
        strcpy(t, "00.0");
    else
        snprintf(t, 8, "%02u.%u", (n - 1) / 4, (n - 1) % 4 + 1);
}
static uintptr_t g_pm_cdstr[PM_CD_MAX + 2]; /* [n] = "BB.b", [PM_CD_MAX + 1] = "--.-" */
static uintptr_t g_pm_bars, g_pm_digit[10];
static uintptr_t g_pm_font[2][4]; /* juce::Font: the countdowns, "Bars" and the deck numbers */
static float const g_pm_font_h[2] = {15.0f, 12.0f};
static int g_pm4_ok; /* message thread: the Strings and Fonts are built */
static void pm4_init(void) {
#ifdef SK_HAVE_HOOK
    if (g_pm4_ok)
        return;
    char t[8];
    for (uint32_t n = 0; n <= PM_CD_MAX + 1; n++) {
        pm_cd_fmt(n, t);
        ((void (*)(uintptr_t *, const char *)) JSTR_FROM_CSTR)(&g_pm_cdstr[n], t);
    }
    ((void (*)(uintptr_t *, const char *)) JSTR_FROM_CSTR)(&g_pm_bars, "Bars");
    for (int i = 0; i < 10; i++) {
        t[0] = (char) ('0' + i);
        t[1] = 0;
        ((void (*)(uintptr_t *, const char *)) JSTR_FROM_CSTR)(&g_pm_digit[i], t);
    }
    for (int i = 0; i < 2; i++)
        kt_call_x8((void *) PM_FONT_GET, g_pm_font[i], 0, 0, g_pm_font_h[i]);
    g_pm4_ok = 1;
#endif
}
static uintptr_t *pm_cd_text(uint32_t n) {
    return &g_pm_cdstr[n <= PM_CD_MAX ? n : PM_CD_MAX + 1];
}
static void pm_text(void *g, int font, const uintptr_t *str, int x, int y, int w, int h, int just,
                    unsigned rgb) {
    unsigned char col[8] = {rgb & 0xff, rgb >> 8 & 0xff, rgb >> 16 & 0xff, 0xff, 0, 0, 0, 0};
    int rect[4] = {x, y, w, h};
    ((void (*)(void *, void *)) SETCOLOUR_ADDR)(g, col);
    ((void (*)(void *, uintptr_t)) KT_SETFONT)(g, (uintptr_t) g_pm_font[font]);
    ((void (*)(void *, const uintptr_t *, const int *, const int *, int, float)) KT_DRAWFITTED)(
        g, str, rect, &just, 1, 1.0f);
}
#define PM_ORANGE 0xFF8C00
#define PM_BLUE 0x28A0FF
#define PM_RED 0xE62828
#define PM4_X0 222   /* view x: the 4 DECK rulers, as wide as the 2 DECK meter's */
#define PM4_X1 1040
#define PM_X_TEXT (PM4_X0 - 90) /* view x: a row's deck number, countdown and "Bars" (85 px), just left of its ruler as on the CDJ */
#define PM4T_BAND_Y 74 /* Top: view y of the band's first row (the header ends at 70) */
#define PM4T_ROW 18    /* Top: one deck's row in the band */
/* The master: its deck (-1 = none), its number, its bar phase (known = 1) and its memory cue
 * countdown. */
struct pm4_master {
    int lane, num, known;
    double ph;
    uint32_t cd;
};
static void pm4_master(struct pm4_master *M) {
    int m = __atomic_load_n(&g_master_deck, __ATOMIC_RELAXED);
    M->lane = m;
    M->num = m + 1;
    M->ph = 0;
    M->known = m >= 0 && pm_local_phase(m, &M->ph);
    M->cd = m >= 0 ? __atomic_load_n(&g_pm_cue[m], __ATOMIC_RELAXED) : PM_CUE_NONE;
}
/* Deck d's own memory cue countdown. */
static uint32_t pm4_cue(int d) {
    return __atomic_load_n(&g_pm_cue[d], __ATOMIC_RELAXED);
}
/* EP147's playhead x in deck d's waveform box (view coordinates), -1 = none */
/* Deck d's waveform box in WaveformView v and its screen rect r: the one its waveform widget
 * registered (pm_wavebox), else, with no track loaded (no widget yet), the box in the deck's
 * own field while that field shows, so an empty deck still gets its (empty) ruler. 0 = none. */
static uintptr_t pm_deckbox(uintptr_t v, int d, int r[4]) {
    uintptr_t box = pm_wavebox(d, r), fw;
    if (box || !pl_view_ok(v) || !(fw = pl_field(v, d)) || !(*(volatile uint32_t *) (fw + 0xc0) & 2))
        return box;
    box = pl_kid(fw, PM_WAVEBOX_VT, 0);
    return box && (*(volatile uint32_t *) (box + 0xc0) & 2) && ht_screen_rect(box, r) ? box : 0;
}
static int pm4_playhead(uintptr_t v, int d, const int rv[4], int r[4]) {
    uintptr_t box = pm_deckbox(v, d, r);
    uintptr_t ph = box ? pm_child(box, PM_PLAYHEAD_VT, 1) : 0;
    if (!ph)
        return -1;
    return r[0] - rv[0] + *(volatile int *) (ph + 0x20) + *(volatile int *) (ph + 0x28) / 2;
}
/* A row's label at view x tx, centered on y: deck d's number (an orange box for the master),
 * its memory cue countdown and "Bars" (red under four bars, else col). Nothing until the
 * Strings and Fonts are built. */
static void pm_row_label(void *g, int tx, int y, int d, int ism, unsigned col) {
    if (!g_pm4_ok || d < 0 || d > 3)
        return;
    uint32_t cd = pm4_cue(d);
    pm_fill(g, tx, y - 6, 13, 13, ism ? PM_ORANGE : 0x505050);
    pm_text(g, 1, &g_pm_digit[d + 1], tx, y - 6, 13, 13, 0x24, ism ? 0x000000 : 0xFFFFFF);
    unsigned c = cd <= PM_CUE_NEAR ? PM_RED : col;
    pm_text(g, 0, pm_cd_text(cd), tx + 15, y - 7, 35, 16, 0x22, c);
    pm_text(g, 1, &g_pm_bars, tx + 53, y - 5, 32, 13, 0x21, c);
}
/* 4 DECK: one row per lane in the band above all lanes, in the lanes' order: its label
 * (pm_row_label) just left of its ruler, and the ruler, as wide as the 2 DECK meter's, the
 * master's in orange; ten beats across, red downbeats, and EP147's playhead line through the
 * band. As on the CDJ, where the two rulers' beat ticks reach toward each other, each row's
 * ticks point toward the master's row (down from above it, up from below it, both ways on
 * the master's own row; down when no row is the master's). A lane whose deck is not known
 * has an empty row. */
static void pm4_paint_top(uintptr_t v, void *g, const int rv[4], int rr[4][4]) {
    struct pm4_master M;
    pm4_master(&M);
    int x0 = PM4_X0 - rv[0], w = PM4_X1 - PM4_X0, top = PM4T_BAND_Y - rv[1], line = -1;
    int row[4], px[4], jm = -1;
    if (top < 0)
        return;
    for (int d = 0; d < 4; d++) { /* each deck's row (-1 = none) first: the master's sets the ticks */
        int ly = rr[d][1] - rv[1], j = 0;
        px[d] = pm4_playhead(v, d, rv, rr[d]);
        row[d] = -1;
        if (px[d] < x0 + 4 || px[d] > x0 + w - 4)
            continue;
        while (j < 4 && abs(ly - g_pm4_row[j]) >= g_pm4_pitch / 2)
            j++;
        if (j < 4)
            row[d] = j;
        if (j < 4 && d == M.lane)
            jm = j;
    }
    ((void (*)(void *)) JUCE_G_SAVESTATE)(g);
    for (int d = 0; d < 4; d++) {
        int j = row[d];
        if (j < 0)
            continue;
        int y = top + j * PM4T_ROW, ism = d == M.lane;
        int in = jm < 0 || j < jm ? +1 : j > jm ? -1
                                                : 0;
        unsigned col = ism ? PM_ORANGE : PM_BLUE;
        double oph;
        line = px[d];
        if (pm_loaded(d) && pm_local_phase(d, &oph))
            pm_ruler_at(g, oph, x0, w, y + 8, px[d], col, 14, 6, in, 2);
        else
            pm_fill(g, x0, y + 8, w, 2, col);
        pm_row_label(g, PM_X_TEXT - rv[0], y + 8, d, ism, col);
    }
    if (line >= 0) {
        pm_fill(g, line - 1, top, 2, 4 * PM4T_ROW, 0xFFFFFF);
        int *b = g_pm_band[g_pm_nband++];
        b[0] = PM_X_TEXT - rv[0];
        b[1] = top;
        b[2] = PM4_X1 - PM_X_TEXT;
        b[3] = 4 * PM4T_ROW;
    }
    ((void (*)(void *)) JUCE_G_RESTORESTATE)(g);
}
static void pm_paint(long self, long g) {
    if (pl_view_ok((uintptr_t) self))
        g_pl_view = (uintptr_t) self;
    g_pm_nband = 0;
    if (!pm_on() || !g || pm_trackinfo_open((uintptr_t) self))
        return; /* the band stays 0: pm_tick stops repainting it */
    /* Decks: the two waveforms on screen, each by its deck (pm_deckbox: loaded or not):
     * 1/2 in 2 DECK, 1/2 or 3/4 in 2/4 DECK. Anything but exactly two showing
     * (4 DECK, a single view): draw nothing. Top ruler = the upper waveform's deck. */
    int rr[4][4], rv[4], dk[2], n = 0;
    for (int d = 0; d < 4; d++)
        if (pm_deckbox((uintptr_t) self, d, rr[d])) {
            if (n < 2)
                dk[n] = d;
            n++;
        }
    if (n >= 1 && g_pl_active4) { /* 4 DECK: a row for each lane whose deck is known */
        if (!ht_screen_rect((uintptr_t) self, rv))
            return;
        g_pm_view_vt = *(volatile uintptr_t *) self;
        wref_set(&g_pm_view, (uintptr_t) self);
        pm4_paint_top((uintptr_t) self, (void *) g, rv, rr);
        return;
    }
    if (n != 2)
        return;
    if (rr[dk[1]][1] < rr[dk[0]][1]) {
        int t = dk[0];
        dk[0] = dk[1];
        dk[1] = t;
    }
    int da = dk[0], db = dk[1], *r0 = rr[da], *r1 = rr[db];
    uintptr_t b0 = pm_deckbox((uintptr_t) self, da, r0);
    if (!b0)
        return;
    if (r1[1] <= r0[1] || r0[0] != r1[0] || r0[2] != r1[2] || r0[3] < 150)
        return; /* 2 DECK: stacked, full height */
    if (!ht_screen_rect((uintptr_t) self, rv))
        return;
    if (!g_pl_active)
        return; /* only position: the band above (phase layout) */
    int top = PL_BAND_Y, h = r0[1] - 2 - top;
    if (h < 20 || r0[2] < PM_X1)
        return;
    uintptr_t ph = pm_child(b0, PM_PLAYHEAD_VT, 1);
    if (!ph)
        return;
    int px = r0[0] - rv[0] + *(volatile int *) (ph + 0x20) +
             *(volatile int *) (ph + 0x28) / 2; /* EP147's playhead, live */
    int x0 = r0[0] - rv[0] + PM_X0, w = PM_X1 - PM_X0, y0 = top - rv[1];
    if (g_pl_active) {
        x0 += 10;
        w -= 20;
    } /* clear of the < > buttons */
    int mid = y0 + h / 2;
    g_pm_view_vt = *(volatile uintptr_t *) self;
    wref_set(&g_pm_view, (uintptr_t) self);
    g_pm_band[0][0] = PM_X_TEXT - rv[0]; /* from the rows' labels to the rulers' right end */
    g_pm_band[0][1] = y0;
    g_pm_band[0][2] = x0 + w - g_pm_band[0][0];
    g_pm_band[0][3] = h;
    g_pm_nband = 1;
    ((void (*)(void *)) JUCE_G_SAVESTATE)((void *) g);
    uintptr_t out = *(volatile uintptr_t *) (b0 + 0x18), lbl = 0;
    int lr[4];
    if (ht_ptr_ok(out) && *(volatile uintptr_t *) out == PM_WAVEOUT_VT)
        lbl = pm_child(out, PM_LOOPLBL_VT, 1);
    /* Keep the loop readout readable. LoopIcon is always marked visible and draws through its
     * children (icon, length label, image), all hidden with no loop, so clip out only the
     * visible children. */
    if (lbl && ht_screen_rect(lbl, lr)) {
        uintptr_t kids = *(volatile uintptr_t *) (lbl + 0x40);
        int n = *(volatile int *) (lbl + 0x50);
        for (int k = 0; ht_ptr_ok(kids) && k < n && k < 16; k++) {
            uintptr_t c = *(volatile uintptr_t *) (kids + 8 * k);
            if (!ht_ptr_ok(c) || !(*(volatile uint32_t *) (c + 0xc0) & 2))
                continue;
            int ex[4] = {lr[0] - rv[0] + *(volatile int *) (c + 0x20) - 3,
                         lr[1] - rv[1] + *(volatile int *) (c + 0x24),
                         *(volatile int *) (c + 0x28) + 6, *(volatile int *) (c + 0x2c)};
            ((void (*)(void *, const int *)) JUCE_G_EXCLUDECLIP)((void *) g, ex);
        }
    }
    int m = __atomic_load_n(&g_master_deck, __ATOMIC_RELAXED);
    /* Each red downbeat is centered on its ruler line; white beat ticks stand only on the
     * middle side of the line, as long as the red tick's inner half. The rulers sit far enough
     * from the center that their marks never overlap. */
    int RH = 22, DH = RH / 2 + 1;
    if (h < 2 * RH + 10) {
        RH = ((h - 10) / 2) & ~1;
        DH = RH / 2 + 1;
    }
    if (RH < 6)
        return;
    int ro = h / 2 - RH / 2 - 3; /* rulers' distance from the center */
    if (ro < RH / 2 + 1)
        ro = RH / 2 + 1;
    pm_ruler((void *) g, da, x0, w, mid - ro, px, m == da ? 0xFF8C00 : 0x28A0FF, RH, DH, +1);
    pm_ruler((void *) g, db, x0, w, mid + ro, px, m == db ? 0xFF8C00 : 0x28A0FF, RH, DH, -1);
    pm_fill((void *) g, px - 1, mid - ro - 4, 2, 2 * ro + 8, 0xFFFFFF);
    pm_row_label((void *) g, PM_X_TEXT - rv[0], mid - ro, da, m == da, m == da ? PM_ORANGE : PM_BLUE);
    pm_row_label((void *) g, PM_X_TEXT - rv[0], mid + ro, db, m == db, m == db ? PM_ORANGE : PM_BLUE);
    ((void (*)(void *)) JUCE_G_RESTORESTATE)((void *) g);
}
/* rtick (message thread): mark the band dirty so it moves every frame */
static void pm_tick(void) {
    if (pm_on())
        pm4_init(); /* the rows' labels, 2 DECK and 4 DECK */
    uintptr_t view = wref_get(&g_pm_view); /* 0 once its destructor ran */
    if (!pm_on() || g_pm_nband <= 0 || !ht_ptr_ok(view))
        return;
    uintptr_t vt = 0;
    if (!own_q(view, &vt) || vt != g_pm_view_vt) {
        wref_set(&g_pm_view, 0);
        return;
    } /* live (wref) but no longer the view painted: forget it */
    for (int i = 0; i < g_pm_nband && i < 4; i++) {
        int rect[4] = {g_pm_band[i][0], g_pm_band[i][1], g_pm_band[i][2], g_pm_band[i][3]};
        ((void (*)(void *, const int *)) PM_REPAINT_AREA)((void *) view, rect);
    }
}
/* WaveformView's own paintOverChildren (Component fns 0x29e9790, slot 27 = 0x21c81c0). The
 * view's children paint over anything drawn from WaveformView::paint; drawing here lands on
 * top of them. */
#define PM_VIEW_VT EP_29e9790
#define PM_VIEW_POC EP_21c81c0
static void (*g_pm_opoc)(void *, void *);
static void pm_poc_hook(void *self, void *g) {
    g_pm_opoc(self, g);
    pm_paint((long) self, (long) g);
}
static void install_phasemeter(void) {
    uintptr_t cur = 0;
    int p = -2;
    wref_track(&g_pm_view, 1, 0); /* without the destructor hook pm_tick never repaints */
    /* Compact Track Info finds the strip under it */
    __atomic_store_n(&g_ci_bv_ok, wref_track(&g_bv_obj, 1, 0) == 1, __ATOMIC_RELEASE);
    /* installed for everyone; pm_on() (the Mod Menu toggle) gates every paint and layout write */
    if (img_q(PM_VIEW_VT + 27 * 8, &cur) && cur == PM_VIEW_POC)
        p = azm_patch_vtable(PM_VIEW_VT + 27 * 8, (void *) pm_poc_hook, (void **) &g_pm_opoc);
    int a = azm_install_hook(PM_LP_UPDATE, (void *) pm_lp_hook, (void **) &g_pm_olp);
    int c = azm_install_hook(PM_MODEL_UPDATE, (void *) pm_model_hook, (void **) &g_pm_omodel);
    int x = ht_install_xnext(); /* tap hook; not used by the meter, harmless if already in */
    int pp = -2;
    if (img_q(PM4_PANEL_VT + 26 * 8, &cur) && cur == PL_PANEL_PAINT)
        pp = azm_patch_vtable(PM4_PANEL_VT + 26 * 8, (void *) pl_panel_paint_hook,
                              (void **) &g_pl_opaint);
    FILE *f = azlog_open("/tmp/azmod-phase.log", "a");
    if (f) {
        fprintf(f,
                "install phasemeter: LocalPlayer.update %d view.paintOverChildren %d xnext %d "
                "model.update %d panel.paint %d\n",
                a, p, x, c, pp);
        fclose(f);
    }
}



/* Grid adjust: WaveformView's grid-adjust panel is shown and hidden by one setter,
 * 0x19d7b60(WaveformView*, bool show), which toggles the panel's widgets ([view+0x410],
 * [view+0x408]) and re-lays the deck areas. Every open and close goes through it (the
 * posted showGridAdjustWidget(bool) task, vptr 0x29e9e50, ends in it on both branches), so
 * hooking it records EP147's own state; no pointer is kept. */
#define WV_SET_GRIDADJ EP_19d7b60
