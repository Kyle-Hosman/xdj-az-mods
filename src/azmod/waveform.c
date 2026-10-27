/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file waveform.c
 * @brief Waveform, deck-tile overview and frame-rate mods for EP147's center display.
 *
 * Hooks EP147's own waveform path rather than drawing over it. The slot-6 point
 * accessors of the DetailedWaveform*DataProvider classes (3-band, RGB, blue) are
 * fed a per-track, per-stem-mask scaled copy of the point array, so stem mutes
 * reshape the native waveform; the deck-tile overview bar is scaled the same way.
 * The deck comes from the constructors' deck argument (WaveformWidgetBase+0x197F0,
 * SimpleWaveform2DeckWidget+0x98); a lookup that cannot answer means unknown.
 * Also forces the native STEMS pad grid (gated on azmod-wavestems.on).
 *
 * The 60 FPS options retarget EP147's 30 Hz limiters: the LinuxRepaintManager
 * re-arms, the MainDisplayUpdateTimer frame clock, DisplayRefreshCycleTimer,
 * the LocalPlayer position feed and list scrolling, plus Bright Grid.
 * Threading: JUCE message thread and paint thread. Kill switches are the
 * azmod-*.off files in /home/root/settings.
 *
 * Entry points: apply_ui60(), install_wave_scale(), wave_prov_report().
 */
/* ---- Force the native STEMS pad grid on the WAVEFORM center screen ----
 * PadStatusWidget (ctor 0x01df6240) builds 8 children at this+0xe0..0x110; its
 * mode switch FUN_01df6010 shows exactly one, chosen by the pad-mode enum from
 * FUN_01fa7a28(this+0xd0). this+0x110 is a PadStemWidget (ctor 0x01df7358),
 * shown only for enum 0xb/0x12 (stems basic/adv), which the AZ's pad-mode cycle
 * never reaches. This forces case 0xb: refresh (FUN_01df69f8), show 0x110, hide the
 * rest. Runs on the JUCE message thread, not the RT LED path. Gated on
 * /home/root/settings/azmod-wavestems.on. */
#define WAVE_CTOR EP_1df6240    /* PadStatusWidget::PadStatusWidget */
#define WAVE_STEMREF EP_1df69f8 /* stems tile refresh, arg = PadStemWidget* */
#define WAVE_STEM_OFF 0x110     /* this+0x110 = PadStemWidget child */
/* Pad-mode title caption: PadModeStatusWidget (vt 0x2d8d300) is the per-deck pad
 * grid. Its ctor 0x1df4a78 runs once per deck (from 0x1df3070) and stores the
 * deck's +0x10b0 at grid+0xc8 (+0x1160 at +0xd8). FUN_01df3ae0(grid) reads the
 * mode via WAVE_MODEGET(*(grid+0xc8)), decodes that mode's caption PNG into
 * grid+0x130 and repaints when the mode at +0xe8 changes. 0xb/0x12 = STEMS. */
#define WAVE_PAINT EP_19d6760 /* WaveformView::paint */
#define SW2D_PAINT \
    EP_1d6bc60 /* SimpleWaveform2DeckWidget::paint; its self+0x98 is the deck 0..3 */
#define SW2D_DECK_OFF \
    0x98 /* deck index inside SimpleWaveform2DeckWidget (verified 0..3) */

/* ---- Stem mutes reshape EP147's own waveform --------------------
 * Each DetailedWaveform*DataProvider (one per color mode) exposes the track as
 * N points. The drawer uses only vtable slot 6, getWavePoint(this, index, band,
 * sret out); out[0] is the column height byte, out[1..] a juce::String color.
 * Slot 7 (single point) is never called. Scaling out[0] scales exactly what
 * EP147 draws at any zoom, with its beat grid, cues and colors untouched;
 * index/count is the track position.
 *
 * Layout (same for all three modes):
 *   [this+0x08] atomic Content*, [this+0x14] int point count (whole track).  */
#define WAVE_PT6_3BAND EP_1b95e80
#define WAVE_PT6_RGB EP_1b948d0
#define WAVE_PT6_BLUE EP_1b949b8
#define WAVEPROV_CNT 0x14
/* Provider vtables: identify the color mode of a point-fn `self`. */
#define WP_VT_3BAND EP_2b72fa0
#define WP_VT_RGB EP_2b72f50
#define WP_VT_BLUE EP_2b72f00
/* One aligned 4-byte load from a live object (own_q's rule, ep_mem.h): the EP147 fields
 * read here are u32 (vtable low words, deck numbers), not always 8-aligned. */
static inline int own_u32(uintptr_t a, unsigned *out) {
    if (a < 0x10000 || a > 0x0000ffffffffffffUL || (a & 3))
        return 0;
    *out = *(const volatile unsigned *) a;
    return 1;
}
/* Widget vtables, by RTTI (vtable-8 -> typeinfo, typeinfo+8 -> mangled name):
 *   0x02b74558 = gui::DetailedWaveformFieldWidget          <- a different class
 *   0x02b67408 = gui::waveform::DetailedWaveformWidget<
 *                  appnd_trk_info::DetailedWaveform_RGB,
 *                  DetailedWaveformDrawer, DetailedWaveformDataProvider<..> >
 * 0x02b67408 is the one painting (update()'s `this`). The FieldWidget's
 * +0xA8 deck field does not carry over to it. */
#define WSCAN_VT EP_2b67408       /* gui::waveform::DetailedWaveformWidget<RGB> */
#define WSCAN_VT_FIELD EP_2b74558 /* gui::DetailedWaveformFieldWidget (a different class) */
/* Deck offset inside the RGB DetailedWaveformWidget:
 *   FUN_01b8f950 (WWB ctor wrapper): *(u32*)(WWB + 0x197F0) = param_4, the deck
 *   index (also passed to the per-deck accessor FUN_01fcc408).
 *   FUN_01b92d50 embeds the three color-mode widgets at WWB+0x90,
 *   WWB+0x87F8 (RGB) and WWB+0x10F60.
 *   => deck = *(u32*)(widget + 0x197F0 - 0x87F8)
 * Applies to the widget, never to the point fn's stack-temporary `self`. */
#define WIDGET_DECK_OFF 0x10FF8L
/* Deck-binding log (/tmp/azmod-wwb.log): the WWB ctor's census and the first lookups
 * that missed it. Budgeted: a miss repeats on every point of a frame. */
static _Atomic int g_wwb_log_budget = 64;
static void wwb_log(const char *fmt, ...) {
    if (__atomic_fetch_sub(&g_wwb_log_budget, 1, __ATOMIC_RELAXED) <= 0)
        return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    FILE *f = azlog_open("/tmp/azmod-wwb.log", "a");
    if (f) {
        fprintf(f, "[%6ld.%03ld] %s\n", ts.tv_sec % 100000, ts.tv_nsec / 1000000, buf);
        fclose(f);
    }
}
/* juce::LinuxComponentPeer::LinuxRepaintManager::timerCallback (typeinfo
 * 0x314bdb0 -> vtable 0x315e560, slot 2; the 3000 ms idle-stop matches upstream
 * JUCE). Counted for the rate diagnostics. */
#define JUCE_REPAINT_TIMERCB EP_24fb350

/* ---- 60Hz UI ----------------------------------------------------
 * LinuxRepaintManager restarts its coalescing timer with
 *     0x24fac4c:  mov w1, #0x21      ; 33 == 1000/30 == JUCE repaintTimerPeriod
 *     0x24fac50:  bl  0x239cda0      ; juce::Timer::startTimer(int)
 * That timer ticks 32/s even when nothing paints, so it is a global limiter.
 * Patching the immediate takes effect on the next restart and reverts as fast.
 * MOVZ Wd,#imm16 = 0x52800000 | imm<<5 | Rd. */
/* juce::Timer::startTimer(this, ms): jmax(1,interval) csinc, stores this+0x10. */
#define JUCE_STARTTIMER EP_239cda0
/* meow::TimerTaskBox::Impl: what actually drives DisplayRefreshCycleTimer. The
 * timer's own juce::Timer base is never queued (positionInQueue = -1, period 0);
 * it holds this box at +0x18, started at 33 ms once at init. Retarget only this
 * box, and only while the tmpfs flag is armed: widening to the whole 28-40 ms
 * band also hits 0x315db60 and saturates the message thread. */
#define TIMERTASKBOX_VT EP_2e67bb0
#define DISPREFRESH_VT \
    EP_29c98c0 /* gui::DisplayRefreshCycleTimer: its                                               \
                * timerCallback dirties the waveform rect.                                         \
                * Not self-scheduled: called from                                                  \
                * MainDisplayUpdateTimer::timerCallback's                                          \
                * refresh path (ret 0x212fbb4). Its own                                            \
                * juce::Timer base is unused. */
#define MAINDISP_VT EP_2e41538 /* device_adapter::MainDisplayUpdateTimer */
#define LOCALPLAYER_VT \
    EP_2da1c28 /* gui::player_info::LocalPlayer: per-deck position feed; 33 ms caps waveform content at 30 fps */
/* Two switches, so 60 FPS Waveforms and 60 FPS UI work independently:
 *   g_ui60_on    = the 60 Hz frame clock (MainDisplayUpdateTimer halving, the
 *                  divider, both LinuxRepaintManager re-arm sites). On when
 *                  either option is on; everything painting at 60 needs it.
 *   g_wave60_on  = waveform content at 60: its three 30 Hz feeds (LocalPlayer,
 *                  DisplayRefreshCycleTimer, TimerTaskBox) and Bright Grid.
 *                  Waveforms option only; without it the content moves at 30/s. */
static _Atomic int g_ui60_on = 0;
static _Atomic int g_wave60_on = 0;
static inline int ui60_clock_wanted(const azmod_state *s) {
    (void) s;
    return az_on(AZF_FPS60_WAVEFORMS) || az_on(AZF_FPS60_UI);
}
static void (*g_orig_starttimer)(long, int);
static int g_tmr_n = 0;
static pthread_mutex_t g_tmr_lk = PTHREAD_MUTEX_INITIALIZER;
/* The 60 Hz switch: halve the frame clock.
 *
 * MainDisplayUpdateTimer::timerCallback (0x212fb40) is frame-locked: it counts
 * frames from a panel-frame interval double at [this+0x58] (16.88 ms =
 * 1/59.24 s), refreshes only after two frames, and re-arms aligned to frame
 * boundaries. Timer or divider patches alone are re-absorbed by that loop.
 * Halving the interval makes "two frames" one real frame, so the stock code
 * delivers ~59 refreshes/s with no instruction patches.
 * self is this+8 (the juce::Timer subobject, vtable 0x2e41538), so the field is
 * [self+0x50]. Guard >12 halves exactly once (and again if EP147 restores it);
 * disarming doubles it back (<12 -> *2, guard >4). */
static _Atomic long g_mdut_self = 0;   /* Timer subobject, for /tmp/azmod-rates */
static _Atomic long g_mdut_fi_raw = 0; /* raw double bits of [this+0x58], for rates */
/* /tmp/azmod-disp60 arms 60 FPS for this boot (written by load_config; dies with
 * tmpfs). Latched rather than access()'d per startTimer: ~196 calls/s on the
 * message thread, the one that paints. While unarmed, re-check once per 256
 * calls (~1.3 s) so a manual `touch` still arms it. */
static int disp60_armed(void) {
    if (__atomic_load_n(&g_disp60_armed, __ATOMIC_RELAXED))
        return 1;
    static unsigned n;
    if ((++n & 0xffu) == 0 && access("/tmp/azmod-disp60", F_OK) == 0) {
        __atomic_store_n(&g_disp60_armed, 1, __ATOMIC_RELAXED);
        return 1;
    }
    return 0;
}
/* startTimer census (/tmp/azmod-timers): a mutex and a 96-entry scan per call,
 * so diagnostic only, enabled by azmod-timerlog.on (read once). */
static int g_timerlog = -1;
/* Period forced on every 60 FPS timer and re-arm site. Not a cap: paint is
 * already locked to the 59.24 Hz panel by the frame clock. 16 keeps the
 * position feed ahead of the panel; 17 (58.8/s) would repeat a frame every
 * ~2.4 s. settings/azmod-ui60.ms (14..20) overrides it. Read once. */
static int ui60_period_ms(void) {
    static int v = 0;
    if (!v) {
        int t, r = 16;
        FILE *uf = fopen("/home/root/settings/azmod-ui60.ms", "re");
        if (uf) {
            if (fscanf(uf, "%d", &t) == 1 && t >= 14 && t <= 20)
                r = t;
            fclose(uf);
        }
        v = r;
    }
    return v;
}
/* ---- 60 FPS list scrolling ------------------------------------------------
 * Active only while 60 FPS UI is on and the halved frame clock is live. Kill
 * switch: settings/azmod-scroll60.off.
 * (1) The post-paint re-arm in performAnyPendingRepaintsNow (startTimer(33) at
 *     0x24fac08, returns to 0x24fac14) is two frames; make it one.
 * (2) MainDisplayUpdateTimer's next-step scheduler (0x212f900, returns to
 *     0x212f988) restarts the running repaint timer, so a due paint slips a
 *     whole step. Skip that call when the timer is already due sooner; this only
 *     ever keeps an earlier deadline.
 * Both are needed: list drag 48-53 -> ~59/s, fling coast 35-49 -> 57-60/s. */
#define RM_VT EP_315e560 /* juce LinuxRepaintManager (Timer) */
#define RM_POSTPAINT_LR EP_24fac14
#define MDUT_NEXTSTEP_LR EP_212f988
static int g_scroll60_ok = 1;        /* cleared by settings/azmod-scroll60.off at install */
static _Atomic long g_rm_due_us = 0; /* when the repaint timer next fires (CLOCK_MONOTONIC us) */
static long scroll_now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000L + t.tv_nsec / 1000;
}
static int scroll60_on(void) {
    return g_scroll60_ok && az_on(AZF_FPS60_UI) &&
           __atomic_load_n(&g_ui60_on, __ATOMIC_RELAXED) &&
           __atomic_load_n(&g_disp60_armed, __ATOMIC_RELAXED);
}
/*  (3) Fling coasting: the touch list (vt 0x2e600a0) coasts on MultiTimer id 1,
 *      started by its release handler 0x21e4550 at [list+0x100] = 30 ms with
 *      per-tick physics (velocity px/tick at [+0x208/+0x210], friction [+0x108]
 *      = 0.9, tick 0x21e3510). At release we use 17 ms ticks, friction
 *      0.9^(17/30) and velocity *17/30: same distance and duration, twice the
 *      steps. Only stock values are replaced and only ours restored; the object
 *      is vtable-checked and touched only inside EP147's own call for it. */
#define LIST_VT EP_2e600a0
#define FLING_REL EP_21e4550
#define FLING_MS 17
static void (*g_orig_flrel)(long);
static void flrel_hook(long L) {
    static double fr_ours = 0;
    if (!fr_ours)
        fr_ours = pow(0.9, FLING_MS / 30.0);
    int ok = L > 0x10000 && (L & 7) == 0 && *(uintptr_t *) L == LIST_VT, on = ok && scroll60_on();
    int *per = (int *) (L + 0x100);
    double *fr = (double *) (L + 0x108);
    if (ok) {
        if (on && *per == 30 && *fr == 0.9) {
            *per = FLING_MS;
            *fr = fr_ours;
        } else if (!on && *per == FLING_MS && *fr == fr_ours) {
            *per = 30;
            *fr = 0.9;
        }
    }
    g_orig_flrel(L);
    if (on && *per == FLING_MS && *fr == fr_ours) {
        double *v = (double *) (L + 0x208);
        v[0] *= FLING_MS / 30.0;
        v[1] *= FLING_MS / 30.0;
    }
}
/*  (4) Paint-start stamp: MainDisplayUpdateTimer's gate (0x212f300) allows the
 *      next paint 2 half-steps (8.44 ms each) after "last paint" [base+0x80],
 *      which slot 0x21b5990 stamps at the end of performAnyPendingRepaintsNow
 *      (0x24fa6f0). A ~10 ms paint then always ends a step late (25.3 ms
 *      frames). Stamp the paint's start instead, only if EP147 just stored
 *      exactly `now`; object vtable-checked. Heavy lists: 38 -> 56-57/s. */
#define RM_PAINTNOW EP_24fa6f0
static void (*g_orig_rmpaint)(long);
static void rmpaint_hook(long rm) {
    long ts = __atomic_load_n(&g_mdut_self, __ATOMIC_RELAXED);
    int on = scroll60_on() && ts > 0x10000 && !(ts & 7) && *(uintptr_t *) ts == MAINDISP_VT;
    char *b = (char *) (ts - 8);
    double t0 = 0, before = 0;
    if (on) {
        double fi = *(double *) (b + 0x58);
        if (!(fi > 4.0 && fi < 12.0))
            on = 0;
    } /* halved clock only */
    if (on) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        t0 = t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
        before = *(double *) (b + 0x80);
    }
    g_orig_rmpaint(rm);
    if (on && *(uintptr_t *) ts == MAINDISP_VT) {
        double last = *(double *) (b + 0x80), now = *(double *) (b + 0x70);
        if (last != before && last == now && t0 <= last && last - t0 < 100.0)
            *(double *) (b + 0x80) = t0;
    }
}
static void starttimer_hook(long self, int ms) {
    uintptr_t lr;
    __asm__ volatile("mov %0, x30" : "=r"(lr));
    uintptr_t vt = (self > 0x10000) ? *(uintptr_t *) self : 0;
    if (vt == RM_VT) {
        long now = scroll_now_us();
        if (lr == RM_POSTPAINT_LR && ms == 33 && scroll60_on())
            ms = ui60_period_ms();
        if (lr == MDUT_NEXTSTEP_LR && scroll60_on() && *(int *) (self + 0x10) > 0) {
            long due = __atomic_load_n(&g_rm_due_us, __ATOMIC_RELAXED);
            if (due > now - 50000 && due < now + (long) ms * 1000)
                return; /* keep the sooner deadline */
        }
        __atomic_store_n(&g_rm_due_us, now + (long) (ms > 0 ? ms : 1) * 1000, __ATOMIC_RELAXED);
    }
    if (vt == MAINDISP_VT) {
        __atomic_store_n(&g_mdut_self, self, __ATOMIC_RELAXED);
        double *fi = (double *) (self + 0x50);
        __atomic_store_n(&g_mdut_fi_raw, *(long *) fi, __ATOMIC_RELAXED);
        if (__atomic_load_n(&g_ui60_on, __ATOMIC_RELAXED) && disp60_armed()) {
            if (*fi > 12.0)
                *fi *= 0.5; /* 16.88 -> 8.44: stock math now 60Hz */
        } else if (*fi < 12.0 && *fi > 4.0) {
            *fi *= 2.0; /* disarm: restore the real frame clock */
        }
    }
    /* Retarget the ~30 Hz timers, identified by vtable (they re-arm from their
     * own timerCallback, so lr is always the generic dispatch). The 28-40 ms
     * population is four classes:
     *   0x2e41538 device_adapter::MainDisplayUpdateTimer  (self-scheduling)
     *   0x315e560 juce::...::LinuxRepaintManager          (2nd site 0x24f6b40)
     *   0x2da1c28 gui::player_info::LocalPlayer           (per-deck, repeating)
     *   0x2e67bb0 meow::TimerTaskBox::Impl
     * Do not widen this to the whole band: forcing unrelated timers (0x315db60,
     * ~2650 starts each) to 16 ms saturates the message thread and drops the
     * repaint timer back to 32/s. */
    int ui60_ms = ui60_period_ms();
    if (vt == MAINDISP_VT && ms >= 28 && ms <= 40 && __atomic_load_n(&g_ui60_on, __ATOMIC_RELAXED))
        ms = ui60_ms; /* the frame clock: either toggle */
    if ((vt == LOCALPLAYER_VT || vt == DISPREFRESH_VT) && ms >= 28 && ms <= 40 &&
        __atomic_load_n(&g_ui60_on, __ATOMIC_RELAXED) &&
        __atomic_load_n(&g_wave60_on, __ATOMIC_RELAXED))
        ms = ui60_ms; /* waveform content: 60 FPS Waveforms only */
    /* The waveform's driver: also gated on the tmpfs flag, so it never arms from
     * persisted config and a reboot always disarms it. */
    if (vt == TIMERTASKBOX_VT && ms >= 28 && ms <= 40 &&
        __atomic_load_n(&g_ui60_on, __ATOMIC_RELAXED) &&
        __atomic_load_n(&g_wave60_on, __ATOMIC_RELAXED) && disp60_armed())
        ms = ui60_ms;
    if (g_timerlog < 0)
        g_timerlog = (AZ_PROBE_ON("/home/root/settings/azmod-timerlog.on"));
    if (g_timerlog) {
        pthread_mutex_lock(&g_tmr_lk);
        int i;
        for (i = 0; i < g_tmr_n; i++)
            if (g_tmr[i].vt == vt && g_tmr[i].ms == ms) {
                g_tmr[i].n++;
                break;
            }
        if (i == g_tmr_n && g_tmr_n < 96) {
            g_tmr[g_tmr_n].vt = vt;
            g_tmr[g_tmr_n].lr = (long) lr;
            g_tmr[g_tmr_n].ms = ms;
            g_tmr[g_tmr_n].n = 1;
            g_tmr_n++;
        }
        pthread_mutex_unlock(&g_tmr_lk);
    }
    g_orig_starttimer(self, ms);
}
/* ---- Position feed lock: LocalPlayer ticks once per displayed frame -------
 * gui::player_info::LocalPlayer::timerCallback (vtable slot 2) samples the
 * deck's playhead for the waveform. On its own juce::Timer it free-runs at
 * 62.5/s against the 59.24 Hz frame clock, so every few frames the waveform
 * moves two steps or none. While 60 FPS Waveforms is live, its own tick only
 * records the instance, and the frame's refresh path (disprefresh_hook, same
 * message thread, before the waveform is dirtied) runs the original once per
 * frame. The feed samples the playhead, so a per-frame call moves the waveform
 * by elapsed time, not by a fixed step.
 *
 * Entries are validated (alignment, exact vtable) at every use and zeroed when
 * invalid. An entry whose own timer has not ticked for FEED_IDLE_MAX frames is
 * dropped, so a stopped feed stays stopped. If frames stop running it, its own
 * tick runs the original again after FEED_OWN_MAX ticks, so the feed never
 * stalls. Lock off: the original runs from its own timer and the table is
 * unused. Kill switch: settings/azmod-feedlock.off (read once at install). */
#define LOCALPLAYER_TIMERCB EP_1e27d28
#define FEED_MAX 8
#define FEED_OWN_MAX 3  /* own ticks without a frame run before passing through */
#define FEED_IDLE_MAX 8 /* frame runs without an own tick before the entry is dropped */
static void (*g_orig_lptick)(long);
static int g_feedlock_ok = 1; /* cleared by settings/azmod-feedlock.off at install */
static struct {
    _Atomic long self;
    _Atomic int own;  /* own-timer ticks since the last frame run */
    _Atomic int idle; /* frame runs since the last own-timer tick */
} g_feed[FEED_MAX];
static _Atomic long g_feed_frame_n = 0, g_feed_pass_n = 0; /* diagnostics */
static int feedlock_on(void) {
    return g_feedlock_ok && __atomic_load_n(&g_ui60_on, __ATOMIC_RELAXED) &&
           __atomic_load_n(&g_wave60_on, __ATOMIC_RELAXED) &&
           __atomic_load_n(&g_disp60_armed, __ATOMIC_RELAXED);
}
static int feed_valid(long p) {
    return p > 0x10000 && (p & 7) == 0 && *(uintptr_t *) p == LOCALPLAYER_VT;
}
static void lptick_hook(long self) {
    if (!feedlock_on() || !feed_valid(self)) {
        g_orig_lptick(self);
        return;
    }
    int i, freei = -1;
    for (i = 0; i < FEED_MAX; i++) {
        long p = __atomic_load_n(&g_feed[i].self, __ATOMIC_RELAXED);
        if (p == self)
            break;
        if (!p && freei < 0)
            freei = i;
    }
    if (i == FEED_MAX) {
        if (freei < 0) { /* table full: this instance keeps its own timer */
            g_orig_lptick(self);
            return;
        }
        i = freei;
        __atomic_store_n(&g_feed[i].own, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_feed[i].self, self, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&g_feed[i].idle, 0, __ATOMIC_RELAXED);
    if (__atomic_add_fetch(&g_feed[i].own, 1, __ATOMIC_RELAXED) >= FEED_OWN_MAX) {
        __atomic_store_n(&g_feed[i].own, 0, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_feed_pass_n, 1, __ATOMIC_RELAXED);
        g_orig_lptick(self); /* no frame has run it lately */
    }
}
/* Once per frame, from disprefresh_hook. Never calls startTimer. */
static void feed_run_frame(void) {
    void (*orig)(long) = __atomic_load_n(&g_orig_lptick, __ATOMIC_ACQUIRE);
    if (!feedlock_on() || !orig)
        return;
    for (int i = 0; i < FEED_MAX; i++) {
        long p = __atomic_load_n(&g_feed[i].self, __ATOMIC_RELAXED);
        if (!p)
            continue;
        if (!feed_valid(p) || __atomic_add_fetch(&g_feed[i].idle, 1, __ATOMIC_RELAXED) > FEED_IDLE_MAX) {
            __atomic_store_n(&g_feed[i].self, 0, __ATOMIC_RELAXED);
            continue;
        }
        __atomic_store_n(&g_feed[i].own, 0, __ATOMIC_RELAXED);
        orig(p);
    }
    __atomic_fetch_add(&g_feed_frame_n, 1, __ATOMIC_RELAXED);
}
/* ---- The waveform limiter: gui::DisplayRefreshCycleTimer -----------------
 * vtable 0x29c98c0, timerCallback 0x1987038. Dirties the waveform rect
 * 170,89,900x162 at 32/s, which caps WaveformView::paint.
 *
 * Its prologue's 4th instruction is `adrp x19, 0x3b81000`, out of adrp range
 * from an mmap'd trampoline; azm_install_hook materializes it as movz/movk.
 *
 * Never call startTimer() from in here: it unlinks and relinks the timer while
 * TimerThread::callTimers iterates the list, which corrupts it and crash-loops
 * EP147. */
/* MainDisplayUpdateTimer's frame gate, a divider over the frame clock:
 *   212fb74: ldr  d1, [x19, #0x58]   ; frame interval = 16.88ms (59.24 Hz panel)
 *   212fb80: fdiv  d4, d4, d1         ; frames elapsed now
 *   212fb88: fcvtms w1, d4            ; floor
 *   212fb90: sub  w0, w1, w0         ; ... minus frames at last refresh
 *   212fb94: cmp  w0, #0x1
 *   212fb98: b.le -> skip            ; refresh only after two frames -> 29.6 Hz
 * Patching the compare alone changes nothing visible (the re-arm still lands
 * every other frame); it is paired with the interval halving in starttimer_hook.
 *   CMP Wn,#imm = SUBS WZR,Wn,#imm = 0x71000000 | imm<<10 | Rn<<5 | 0x1f
 *   #1 -> 0x7100041f      #0 -> 0x7100001f                                    */
#define UI60_DIV_SITE EP_212fb94
#define UI60_DIV_30HZ 0x7100041fu /* cmp w0, #1: every other frame */
#define UI60_DIV_60HZ 0x7100001fu /* cmp w0, #0: every frame       */
static _Atomic long g_tick_iv = 0;
#define DISPREFRESH_TIMERCB EP_1987038
#define JUCE_TIMER_PERIOD_OFF 0x10 /* startTimer: str w2,[x21,#0x10] */
static void (*g_orig_disprefresh)(long);
static _Atomic long g_disp_n = 0;
/* Diagnostics: what the callback sees (period field, wanted period, arm state). */
static _Atomic int g_disp_seen = -1, g_disp_want = -1, g_disp_armed = -1, g_disp_self_lo = 0;
/* Raw dump of the object's first 0xA0 bytes. +0x18 is a second vptr: the
 * TimerTaskBox subobject of a multiply-inherited object. */
static _Atomic int g_disp_ints[40];
static long g_disp_bt[10];
static int g_disp_btn = 0;           /* backtrace of our caller */
static _Atomic long g_disp_pass = 0; /* calls passed through to EP147 */
static void disprefresh_hook(long self) {
    __atomic_fetch_add(&g_disp_n, 1, __ATOMIC_RELAXED);
    /* 60 FPS UI without 60 FPS Waveforms: the 60 Hz frame clock alone drives
     * this at ~60/s, rebuilding the waveform at 60 for nothing. Keep stock
     * cadence: pass at most one call per ~25 ms (every other tick). */
    if (__atomic_load_n(&g_ui60_on, __ATOMIC_RELAXED) &&
        !__atomic_load_n(&g_wave60_on, __ATOMIC_RELAXED)) {
        static long last_us;
        long now = scroll_now_us();
        if (now - last_us < 25000)
            return;
        last_us = now;
    }
    __atomic_fetch_add(&g_disp_pass, 1, __ATOMIC_RELAXED);
    /* Menu toggle and tmpfs flag: a reboot always disarms it. */
    static int armed = -1;
    if (armed < 0)
        armed = (access("/tmp/azmod-disp60", F_OK) == 0);
    int want = (armed && __atomic_load_n(&g_ui60_on, __ATOMIC_RELAXED) &&
                __atomic_load_n(&g_wave60_on, __ATOMIC_RELAXED))
                   ? 16
                   : 33;
    /* Read-only telemetry: this+0x10 (timerPeriodMs) is inert because the
     * juce::Timer base is never queued. The rate is set by the TimerTaskBox at
     * +0x18 (see starttimer_hook). */
    int *period = (int *) (self + JUCE_TIMER_PERIOD_OFF);
    __atomic_store_n(&g_disp_seen, *period, __ATOMIC_RELAXED);
    __atomic_store_n(&g_disp_want, want, __ATOMIC_RELAXED);
    __atomic_store_n(&g_disp_armed, armed, __ATOMIC_RELAXED);
    __atomic_store_n(&g_disp_self_lo, (int) (self & 0xFFFFFF), __ATOMIC_RELAXED);
    for (int k = 0; k < 40; k++)
        __atomic_store_n(&g_disp_ints[k], *(int *) (self + k * 4), __ATOMIC_RELAXED);
    /* Record the caller chain once (diagnostic). */
    if (g_disp_btn == 0) {
        uintptr_t fp;
        __asm__ volatile("mov %0, x29" : "=r"(fp));
        long f = (long) fp;
        int k = 0;
        while (k < 10 && f > 0x10000) {
            long v = *(long *) (f + 8);
            if (v > 0x400000 && v < 0x4000000)
                g_disp_bt[k++] = v;
            long nf = *(long *) f;
            if (nf <= f)
                break;
            f = nf;
        }
        g_disp_btn = k;
    }
    feed_run_frame(); /* the position first, then the waveform is dirtied */
    g_orig_disprefresh(self);
}
#define UI60_SITE EP_24fac4c
#define UI60_INSN_30HZ 0x52800421u /* mov w1, #33 */
#define UI60_INSN_60HZ \
    (0x52800001u | ((uint32_t) ui60_period_ms() << 5)) /* mov w1, #<period> */
#define UI60_SITE2 \
    EP_24f6b34 /* 2nd LinuxRepaintManager re-arm: the mov w1,#33 feeding the tail-branch at        \
                  0x24f6b40 */
static void apply_ui60(int on) {
    __atomic_store_n(&g_ui60_on, on ? 1 : 0, __ATOMIC_RELAXED);
    /* The frame divider, also gated on the tmpfs flag so a reboot disarms it. */
    if (access("/tmp/azmod-disp60", F_OK) == 0) {
        uint32_t *dsite = (uint32_t *) UI60_DIV_SITE;
        uint32_t wantd = on ? UI60_DIV_60HZ : UI60_DIV_30HZ;
        uint32_t expect = on ? UI60_DIV_30HZ : UI60_DIV_60HZ;
        if (*dsite == expect) { /* read back before writing */
            azprot_t pt;
            if (azprot_unlock((uintptr_t) dsite, 4, &pt) == 0) {
                __atomic_store_n(dsite, wantd, __ATOMIC_RELEASE);
                __builtin___clear_cache((char *) dsite, (char *) dsite + 4);
                if (azprot_relock(&pt) != 0)
                    dbg("ui60: code page left writable (mprotect failed)\n");
                dbg("ui60: frame divider -> %s\n", on ? "every frame" : "every other frame");
            }
        }
    } /* g_ui60_on above gates the timer rewrites in starttimer_hook */
    /* Both LinuxRepaintManager re-arm sites. Patch only if the site holds exactly
     * the expected instruction, so a different firmware is never blind-written. */
    const uintptr_t SITES[2] = {UI60_SITE, UI60_SITE2};
    for (int s = 0; s < 2; s++) {
        uint32_t *site = (uint32_t *) SITES[s];
        if (*site != (on ? UI60_INSN_30HZ : UI60_INSN_60HZ))
            continue;
        azprot_t pt;
        if (azprot_unlock((uintptr_t) site, 4, &pt) != 0)
            continue;
        __atomic_store_n(site, on ? UI60_INSN_60HZ : UI60_INSN_30HZ, __ATOMIC_RELEASE);
        __builtin___clear_cache((char *) site, (char *) site + 4);
        if (azprot_relock(&pt) != 0)
            dbg("ui60: code page left writable (mprotect failed)\n");
        dbg("ui60: site%d %s (now %08x)\n", s, on ? "60Hz" : "30Hz", *site);
    }
}
static void (*g_orig_wavepaint)(long, long);
static void wavepaint_hook(long self, long g) {
    wave_mark();
    /* Never call stem_tiles_rebuild() from here (it runs from rtick_hook): it
     * destroys and rebuilds pad-grid children, so inside a paint traversal
     * EP147 walks freed children and SIGSEGVs (garbled waveforms first). */
    drum_tiles_enforce(); /* selected sample / armed pad 8 -> art index */
    stem_selfpoke();
    /* ov_refresh_if_needed() runs from rtick_hook so a stem toggle reaches the
     * deck-strip previews on every screen, not only while this view paints. */
    wave_repaint_if_needed(); /* main waveform, same thread, same trigger */
    /* First paint identifies the object; setvis_hook then tracks it. A paint
     * also proves visibility, covering routes other than setVisible. */
    __atomic_store_n(&g_waveview, self, __ATOMIC_RELAXED);
    __atomic_store_n(&g_wave_vis, 1, __ATOMIC_RELAXED); /* re-arm after an explicit hide */
    __atomic_fetch_add(&g_paint_n, 1, __ATOMIC_RELAXED);
    g_orig_wavepaint(self, g);
}
static void (*g_orig_waveswitch)(long);

/* Stem tiles change art by writing an image index into [ImagesComponent+0xc8]
 * and repainting, which needs an ImagesComponent at [tile+0xf0]. Tiles are built
 * once: a pad-mode switch re-skins them and constructs no new widget. */
/* ---- Beat FX panel (waveform right column): native structure ----
 *   - The panel is image art, not text (the DELAY/ECHO strings in .rodata are
 *     debug/HUI only).
 *   - Name art: Waveform/Fx/beatfx/img_beatfx_*.png (white) and
 *     img_beatfx_blue_*.png (active), 176x26. Pointer table of all 28 at
 *     0x3b35728: 14 blue then 14 white, FX-enum order (delay echo filter
 *     flanger helix mobius phaser pingpong reverb ROLL spiral trans
 *     tripetfilter tripletroll); ROLL = blue idx 9, white idx 23.
 *   - Value art: Waveform/Fx/beatfx/val/img_effect_*.png, 58x43, table at
 *     0x3b34e38 as (bright, dim) pairs.
 *   - BeatFxParamWidget ctor 0x1de01d8 creates gui::ImagesComponent children
 *     (vtable 0x29e9540, size 0xf0) loaded from those tables.
 *     ImagesComponent: [+0xc8]=current index, [+0xd0]=decoded image array,
 *     [+0xe0]=count; same index mechanism as the stem tiles.
 * A development build can also dump each embedded ImagesComponent from the five
 * FX-panel ctor hooks (opt-in: settings/azmod-fxdiag, read at EP147 start). */
#define BEATFX_PARAM_CTOR EP_1de01d8
#define BEATFX_TIME_CTOR EP_1de1020
#define BEATFX_LENGTH_CTOR EP_1ddd8d8
#define FXWIDGET_CTOR EP_1dea048
#define FXBODY_CTOR EP_1de7b20
#define FX_NAME_ART_TBL EP_3b35728
#define FX_VAL_ART_TBL 0x3b34e38UL

#define FXDIAG_CTOR_BODY(TAG) /* empty: the ImagesComponent capture is development-only */
/* The provider classes' color modes, in the order of the per-kind tables below. The point
 * shims (AZM_WP6_SHIM) pass these values as immediates, so they are fixed. */
enum {
    WSK_RGB,
    WSK_BLUE,
    WSK_3BAND,
    WSK_N
};
_Static_assert(WSK_RGB == 0 && WSK_BLUE == 1 && WSK_3BAND == 2 && WSK_N == 3,
               "AZM_WP6_SHIM passes the color mode as an immediate");
static unsigned long WSK_VT[WSK_N]; /* provider vtables, filled by ep147_late_init */
/* Test-only (AZMOD_PROBES builds, /tmp/azmod-waveid.off): every waveform deck lookup answers
 * unknown, to show that a waveform without a deck is drawn stock rather than with another
 * deck's stems. Written by the overlay's slow tick. */
static int g_wave_noid;
/* Zoom model for stem lanes, one per [deck][color mode]. From one full redraw we learn
 * points-per-column and where the playhead sits in the band; the live playhead then comes
 * from azstem, so the lanes follow scrolling and zoom without reading EP147's zoom.
 *
 * Keyed by the deck that is rendering, never by provider: a provider is a stack temporary,
 * so two decks' providers routinely share one address, and its point count says nothing
 * about which deck it serves (two decks can hold the same track). The color modes keep
 * separate models because they have different point counts for one track. */
typedef struct {
    /* Sampled per point by azm_wp_scale on the message thread: plain ints with one writer;
     * the poll thread reads and resets them racily, and a torn sample only skips one
     * estimate. */
    int cnt;      /* points asked for since the last tick */
    int min, max; /* their index range (valid while cnt > 0) */
    int total;    /* the provider's point count: the whole track */
    int mul;      /* ATT_N / total in 16.16, the attenuation index without a divide */
    /* Poll thread (wave_zoom_tick) only, apart from `ok`. */
    int active;                       /* points asked for in the last tick: who is on screen */
    int pd_min, pd_max, pd_n, pd_age; /* a full redraw, parked until azstem has the stems */
    float prevpos;                    /* playhead at the last anchor attempt */
    double ppc;                       /* track points per column */
    double phcol;                     /* playhead position, in columns */
    double ncol;                      /* columns in the band */
    double wtotal;                    /* the point count the model was measured against */
    _Atomic int ok;                   /* the model above is usable */
} wzoom;
static wzoom g_wz[AZMOD_STEM_DECKS][WSK_N];
static double g_ncol_hint = 0; /* band width in px, from the rect */
static int g_four_deck = 0;    /* 4-DECK layout: band rects unknown */
static void wave_zoom_tick(void) {
    /* In 4 DECK the band rect is unknown, so skip sampling. The 2-band model stays valid
     * across the layout switch (same rect, same zoom), so it is kept, not cleared. */
    if (g_four_deck)
        return;
    for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
        for (int k = 0; k < WSK_N; k++) {
            wzoom *z = &g_wz[d][k];
            int c = z->cnt;
            int mn = z->min;
            int mx = z->max;
            z->cnt = 0;
            z->active = c; /* who is drawing right now */
            /* EP147 redraws the whole band every frame (~55k points per tick), so one
             * tick's index range spans the band. Park it until azstem has the stems. */
            if (c >= FULLREDRAW && mx > mn) {
                z->pd_min = mn;
                z->pd_max = mx;
                z->pd_n = c;
                z->pd_age = 0;
            }
            if (z->pd_n <= 0)
                continue; /* nothing parked */
            /* Hold it while azstem decodes (seconds at track load); the playhead jump
             * check below guards the anchor's validity. */
            if (++z->pd_age > 1800) { /* ~60 s */
                z->pd_n = 0;
                continue;
            }
            const unsigned char *env;
            int nb, nst;
            float pos = 0;
            if (!azmod_stem_wave || !azmod_stem_wave(d, &env, &nb, &nst, &pos))
                continue; /* stays parked */
            mn = z->pd_min;
            mx = z->pd_max;
            z->pd_n = 0;
            if (g_ncol_hint < 2)
                continue;
            double ppc = (double) (mx - mn) / (g_ncol_hint - 1);
            if (ppc <= 0)
                continue;
            double total = z->total;
            z->ppc = ppc;
            z->ncol = g_ncol_hint;
            z->wtotal = total;
            /* Anchor unless the playhead jumped (< 2 columns since last tick). Normal
             * playback moves well under a column per tick, so the model can re-anchor
             * while playing; a preview-bar drag is rejected. */
            float dp = pos - z->prevpos;
            if (dp < 0)
                dp = -dp;
            z->prevpos = pos;
            if ((double) dp * total < ppc * 2.0 && total > 0) {
                z->phcol = ((double) pos * total - mn) / ppc;
                __atomic_store_n(&z->ok, 1, __ATOMIC_RELEASE);
            }
        }
    }
}
/* The color mode actually on screen for this deck: the one that asked for the most points
 * in the last tick. -1 = no model for this deck (never another deck's). */
static int wave_active_kind(int deck) {
    if (deck < 0 || deck >= AZMOD_STEM_DECKS)
        return -1;
    int best = -1, bestc = 0;
    for (int k = 0; k < WSK_N; k++) {
        const wzoom *z = &g_wz[deck][k];
        if (!__atomic_load_n(&z->ok, __ATOMIC_ACQUIRE))
            continue;
        if (z->active > bestc || best < 0) {
            bestc = z->active;
            best = k;
        }
    }
    return best;
}
int azmod_wave_zoom(int deck, double *ppc, double *phcol, double *ncol, double *total) {
    int k = wave_active_kind(deck);
    if (k < 0)
        return 0;
    const wzoom *z = &g_wz[deck][k];
    *ppc = z->ppc;
    *phcol = z->phcol;
    *ncol = z->ncol;
    *total = z->wtotal;
    return (*ppc > 0 && *total > 0);
}

/* ---- Deck-tile overview bar (the preview under the track title) ----------
 * Not fed by the slot-6 provider. FUN_01dfb328 (the overview draw) reads it
 * inline: ldar[this+0x80] -> ldar[obj+0x28] = Content, Content+0x28 = element
 * array, Content+0x30 = count (1200), 12 bytes per element. Bytes 0-2 and 6-8
 * are color, 4-5 and 10-11 heights.
 *
 * It renders once per track load into a cached image, so a stem toggle
 * re-renders it from the paint hook (message thread) with our scaled copy;
 * EP147's shared array is never left modified. */
#define WAVE_OVERVIEW EP_1dfb328
/* FUN_01dfc710, the only caller: setOverview(this, &sharedObj, mode, width).
 * Passing the object it already holds takes the fast path (no refcounting),
 * re-renders and calls the follow-up at 0x1dfb068 that puts the image on
 * screen. Calling the draw alone does not. */
#define WAVE_OV_SET EP_1dfc710
typedef void (*ovset_fn)(long self, long *objslot, int mode, int width);
#define OV_STRIDE 12
#define OV_MAX 2048
#define OV_COL_A 0 /* color bytes */
#define OV_COL_B 6
#define OV_HGT_A 4 /* height bytes */
#define OV_HGT_B 10
static void (*g_orig_ov)(long);
#define OVINST_MAX 24 /* several tile sets exist (2-deck / 4-deck layouts) */
/* Per overview widget. `snap`/`scaled` are our own buffers: EP147's array is never
 * written (see ov_scaled_for). */
static struct {
    long self;
    long content;
    int cnt;
    int deck;
    const unsigned char *src;
    int scnt;
    unsigned gen;
    int color;
    unsigned char *snap, *scaled;
} g_ovi[OVINST_MAX];
static _Atomic int g_ovi_n = 0;
static _Atomic int g_ov_refresh = 0; /* bit d: deck d's overview tiles need a re-render */
static _Atomic long g_ov_calls = 0;
static _Atomic long g_ov_applied = 0;
/* ErP (deck-bus) frames by CRC on arrival. Single writer (the bus read), read
 * racily for /tmp/azmod-waveprov. */
static long g_erp_crc_ok, g_erp_crc_bad;

/* Waveform-provider diagnostics. Written only on the JUCE message thread
 * (azm_wp_scale), read racily by the poll thread for /tmp/azmod-waveprov. Plain
 * longs on purpose: this is an ~800k/s hot path where an atomic RMW per point
 * is too costly, and a torn read only prints a stale number. */
static unsigned long g_wp_calls = 0;                /* azm_wp_scale entered */
static unsigned long g_wp_updhit = 0;               /* deck came from the live update() hook */
static unsigned long g_wp_cthit = 0;                /* deck came from the Content table */
static unsigned long g_wp_gateoff = 0;              /*  ... returned: all waveform mods off */
static unsigned long g_wp_nocnt = 0;                /*  ... returned: provider cnt <= 0 */
static unsigned long g_wp_nodeck = 0;               /*  ... returned: deck unknown */
static unsigned long g_wp_gated[AZMOD_STEM_DECKS];  /* deck known, scaling gated off */
static unsigned long g_wp_scaled[AZMOD_STEM_DECKS]; /* points actually scaled, per deck */

/* Deck of the SimpleWaveform2DeckWidget currently painting (its self+0x98), for
 * the waveprov report. -1 = not inside a per-deck widget paint. */
static _Atomic int g_cur_paint_deck = -1;
static _Atomic int g_sw2d_last_deck = -1; /* diag: last deck seen at +0x98 */
static _Atomic long g_sw2d_paints = 0;    /* diag: sw2d paint calls */

/* SimpleWaveform2DeckWidget::paint hook: publishes the widget's deck
 * (self+0x98) for every azm_wp_scale call made during the paint, then restores
 * `prev` so a nested deck-widget paint cannot leak a deck. */
static void (*g_orig_sw2d_paint)(long, long);
/* ---- The deck binding: DetailedWaveformWidget::update -> deck ------------
 *  FUN_01b8f950 (WWB ctor wrapper): *(u32*)(WWB + 0x197F0) = param_4, the deck
 *      index (also passed to the per-deck accessor FUN_01fcc408); then calls
 *      FUN_01b92d50 on the same object.
 *  FUN_01b92d50 embeds the three color-mode widgets inside WWB:
 *          FUN_01b962e8(this + 0x12)   -> WWB + 0x00090
 *          FUN_01b96638(this + 0x10ff) -> WWB + 0x087F8   <- the RGB widget
 *          FUN_01b96988(this + 0x21ec) -> WWB + 0x10F60
 *  => deck = *(u32*)(rgb_widget + 0x10FF8)      (0x197F0 - 0x87F8)
 * azm_upd_shim wraps update()'s only caller FUN_01b93a28, which holds the WWB,
 * so the deck is a compare against the ctor's key table (g_wkey), with
 * azm_upd_deck_of() for a widget pointer (vtable 0x02b67408). Hooking update()
 * itself crashes EP147. The point path then pays one relaxed atomic load.
 *
 * Must be an ASM shim, not a C hook: a C hook cannot preserve v0-v7 and the
 * real arity and return type are unknown. The shim saves and restores x0-x7,
 * x8 (sret) and q0-q7 around both helper calls, as AZM_WP6_SHIM does for the
 * point functions. */
/* Exact widget / juce::Component pointers -> deck, published by the WWB ctor. */
static struct {
    unsigned long ptr;
    int deck;
} g_wkey[48];
static _Atomic int g_wkey_n = 0;
/* WWB base -> deck, for range lookups: an object inside a per-deck WWB
 * (~0x199a0 bytes) identifies its deck by address alone. */
#define WWB_SPAN 0x1A000L
static struct {
    unsigned long base;
    int deck;
} g_wwbbase[8];
static _Atomic int g_wwbbase_n = 0;
/* Entry b's three color-mode widgets (WWB_WIDGETS order = WAVEW_MODES order), each as its
 * juce::Component (widget + WIDGET_COMPONENT_OFF) in a wref slot: the component's destructor
 * clears the slot before the WWB's memory goes back, so g_wwbbase[b] is used only through a
 * live widget (wwb_widget_live). Written with g_wwbbase[b] by wwbctor_hook. */
static _Atomic uintptr_t g_wwb_comp[8][3];
#define WAVE_UPDATE EP_1b97d98 /* DetailedWaveformWidget<RGB>::update (hooking it crashes EP147)   \
                                */
#define WWB_UPDATE_CALLER EP_1b93a28 /* its only caller; holds the WWB */
static void *g_orig_wavupd;
static _Atomic int g_upd_deck = -1; /* deck of the update() running right now */
static _Atomic long g_upd_calls = 0;
static _Atomic int g_upd_bound = 0;
static _Atomic int g_upd_log = 8;
/* widget -> deck memo, negatives included. WWB+0x197F0 is written once at
 * construction, so a widget's deck is fixed for its life. */
static struct {
    unsigned long ptr;
    int deck;
} g_updmemo[16];
static _Atomic int g_updmemo_n = 0;
static int azm_upd_deck_of(long q) {
    if (q < 0x1000000 || (q & 7))
        return -1;
    int n = __atomic_load_n(&g_updmemo_n, __ATOMIC_RELAXED);
    for (int i = 0; i < n; i++)
        if (g_updmemo[i].ptr == (unsigned long) q)
            return g_updmemo[i].deck;
    /* q is x0 of FUN_01b93a28, inside EP147's own call: `this`, the live WWB (its caller
     * 0x1b94430 passes x20, which the function uses as an object). Its deck field is read
     * only once the vtable shows it is the RGB widget, so the offset is inside it. */
    int deck = -1;
    unsigned vt = 0, d = 0;
    if (own_u32((uintptr_t) q, &vt) && vt == (unsigned) WSCAN_VT &&
        own_u32((uintptr_t) (q + WIDGET_DECK_OFF), &d) && d < (unsigned) AZMOD_STEM_DECKS)
        deck = (int) d;
    if (__atomic_fetch_sub(&g_upd_log, 1, __ATOMIC_RELAXED) > 0)
        wwb_log("upd: widget=%lx vt=%#x +%#lx -> deck=%d", (unsigned long) q, vt,
                (unsigned long) WIDGET_DECK_OFF, deck);
    if (n < (int) (sizeof g_updmemo / sizeof g_updmemo[0])) {
        g_updmemo[n].ptr = (unsigned long) q;
        g_updmemo[n].deck = deck;
        __atomic_store_n(&g_updmemo_n, n + 1, __ATOMIC_RELAXED);
    }
    return deck;
}
/* Called by the shim with x0, x1 and s0. Returns the previous deck so the shim
 * can restore it on exit (nested updates). */
int azm_upd_enter(long a0, long a1, float zoom);
void azm_upd_exit(int prev);
/* azm_upd_enter checks both x0 and x1 against the key table: Ghidra shows a
 * spurious leading parameter, so which register holds the object is uncertain.
 * Two pointer compares remove the ambiguity. */
/* Deck-4 double build: in the 4-DECK layout the screen-root update FUN_019dd430
 * builds all four waveforms, then rebuilds [root+0x420] (deck 4) a second time
 * (0x19dd818..0x19dd830, unconditional when layout == 2), ~1 ms per frame.
 * Count root passes and let each deck's FUN_01b93a28 run once per pass; the
 * widget's own listeners still run. Outside a root pass nothing is skipped.
 * Kill switch: settings/azmod-dedupe4.off. */
#define WAVEVIEW_UPDATE EP_19dd430
static void (*g_orig_wvupd)(long);
static unsigned g_root_pass;
static int g_in_root;
static unsigned g_built_pass[AZMOD_STEM_DECKS];
static int g_upd_skip
    __attribute__((used)); /* read by azm_upd_shim (asm); static = no PLT/GOT reloc */
static _Atomic long g_upd_skipped;
static void wvupd_hook(long self) {
    g_root_pass++;
    if (!g_root_pass)
        g_root_pass = 1;
    g_in_root = 1;
    g_orig_wvupd(self);
    g_in_root = 0;
}
static void gridaa_enter(long wwb, long pos_p, float zoom);
/* Update profile: per deck, FUN_01b93a28's duration, the gap since the previous
 * build ended, and one backtrace. Message thread only; published to
 * /tmp/azmod-updprof by the 1 Hz rates block. */
static long g_up_t0, g_up_lastend;
static int g_up_deck = -1;
static long g_up_ns[AZMOD_STEM_DECKS], g_up_n[AZMOD_STEM_DECKS], g_up_max[AZMOD_STEM_DECKS];
static long g_up_gap_ns[AZMOD_STEM_DECKS];
static long g_up_bt[AZMOD_STEM_DECKS][12];
static int g_up_btn[AZMOD_STEM_DECKS];
static long up_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000L + t.tv_nsec;
}
int azm_upd_enter(long a0, long a1, float zoom) {
    __atomic_fetch_add(&g_upd_calls, 1, __ATOMIC_RELAXED);
    gridaa_enter(a0, a1, zoom);
    int prev = __atomic_load_n(&g_upd_deck, __ATOMIC_RELAXED);
    int d = -1;
    int kn = __atomic_load_n(&g_wkey_n, __ATOMIC_RELAXED);
    for (int i = 0; i < kn && d < 0; i++) {
        if (g_wkey[i].ptr == (unsigned long) a0 || g_wkey[i].ptr == (unsigned long) a1)
            d = g_wkey[i].deck;
    }
    if (d < 0)
        d = azm_upd_deck_of(a0); /* update()-style: widget in x0 */
    if (d >= 0) {
        __atomic_store_n(&g_upd_deck, d, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_upd_bound, 1, __ATOMIC_RELAXED);
    }
    if (d >= 0 && d < AZMOD_STEM_DECKS && g_in_root && g_orig_wvupd) {
        if (g_built_pass[d] == g_root_pass) {
            g_upd_skip = 1;
            __atomic_fetch_add(&g_upd_skipped, 1, __ATOMIC_RELAXED);
            g_up_deck = -1;
            return prev;
        }
        g_built_pass[d] = g_root_pass;
    }
    if (d >= 0 && d < AZMOD_STEM_DECKS) {
        long t = up_now_ns();
        if (g_up_lastend)
            g_up_gap_ns[d] += t - g_up_lastend;
        g_up_t0 = t;
        g_up_deck = d;
        if (!g_up_btn[d]) {
            uintptr_t fp;
            __asm__ volatile("mov %0, x29" : "=r"(fp));
            long f = (long) fp;
            int k = 0;
            while (k < 12 && f > 0x10000 && !(f & 7)) {
                long v = *(long *) (f + 8);
                g_up_bt[d][k++] = v;
                long nf = *(long *) f;
                if (nf <= f)
                    break;
                f = nf;
            }
            g_up_btn[d] = k;
        }
    } else
        g_up_deck = -1;
    return prev;
}
/* ---- Bright Grid (part of 60 FPS Waveforms, no menu row of its own) -------
 * update() (0x1b97d98) truncates the scroll position to whole pixels before
 * makeGridImage (0x1dffe10) rasterizes the beat ticks:
 *     off  = (int)((float)*pos * 0.15f * zoom) - center      (fcvtzs, 0x1b98000)
 * so at default zoom a 1-px tick steps on ~38 of 60 frames, which on this panel
 * reads dimmer and flickery although the pixels are stock. Bright Grid keeps
 * every position stock and brightens the ticks while the deck moves: a gain on
 * moving frames, plus an overdrive factor on frames where the ticks stepped
 * (tuned 1.1 / 1.3). Position and zoom come from FUN_01b93a28 (x1 = int64*
 * position, s0 = zoom) via azm_upd_shim; all on the message thread inside that
 * call (g_gridaa_ok is cleared on exit). Kill switch: settings/azmod-gridaa.off. */
/* "Moving" = position changed within g_bg_move_ms; the boost fades over
 * g_bg_ramp_ms. The audio position changes every playing frame, so ~2 frames
 * of hold is enough. Tunable via fields 4-5 of azmod-brightgrid.cfg. */
static _Atomic int g_bg_move_ms = 40, g_bg_ramp_ms = 60;
/* Tuning override: settings/azmod-brightgrid.cfg = "<gain> <overdrive> <gray>
 * [move_ms ramp_ms]", read by brightgrid_poll_cfg (never the paint thread).
 * gain = moving-tick brightness; overdrive = extra factor on step frames;
 * gray = opaque level behind the tick rows (0 = off). */
#define BG_CFG_PATH "/home/root/settings/azmod-brightgrid.cfg"
#define BG_GAIN_DEF 1.1f /* tuned on the deck */
#define BG_OD_DEF 1.3f
static _Atomic int g_bg_gain_k = 1100, g_bg_od_k = 1300, g_bg_gray = 0; /* x1000, x1000, 0..96 */
static _Atomic long g_bg_steps, g_bg_builds;                            /* step-frame telemetry */
static void brightgrid_read_cfg(void) {
    FILE *f = fopen(BG_CFG_PATH, "re");
    float g = BG_GAIN_DEF, od = BG_OD_DEF;
    int gray = 0, mv = 40, rp = 60;
    if (f) {
        if (fscanf(f, "%f %f %d %d %d", &g, &od, &gray, &mv, &rp) < 1) {
            g = BG_GAIN_DEF;
            od = BG_OD_DEF;
            gray = 0;
        }
        fclose(f);
    }
    if (mv < 17 || mv > 1000)
        mv = 40; /* >= one frame, or every frame reads "stopped" */
    if (rp < 1 || rp > 2000)
        rp = 60;
    __atomic_store_n(&g_bg_move_ms, mv, __ATOMIC_RELAXED);
    __atomic_store_n(&g_bg_ramp_ms, rp, __ATOMIC_RELAXED);
    if (!(g >= 1.0f && g <= 2.0f))
        g = BG_GAIN_DEF;
    if (!(od >= 1.0f && od <= 2.5f))
        od = BG_OD_DEF;
    if (gray < 0 || gray > 96)
        gray = 0;
    __atomic_store_n(&g_bg_gain_k, (int) (g * 1000 + 0.5f), __ATOMIC_RELAXED);
    __atomic_store_n(&g_bg_od_k, (int) (od * 1000 + 0.5f), __ATOMIC_RELAXED);
    __atomic_store_n(&g_bg_gray, gray, __ATOMIC_RELAXED);
}
/* Once a second from the overlay's 1 Hz poll, whatever the logs flag says: one stat,
 * and the file is parsed only when it appears, changes or goes away. */
static void brightgrid_poll_cfg(void) {
    static struct {
        int read, present;
        long long ino, size, sec, nsec;
    } seen;
    struct stat st;
    int present = stat(BG_CFG_PATH, &st) == 0;
    if (seen.read && present == seen.present &&
        (!present || (seen.ino == (long long) st.st_ino && seen.size == (long long) st.st_size &&
                      seen.sec == (long long) st.st_mtim.tv_sec &&
                      seen.nsec == (long long) st.st_mtim.tv_nsec)))
        return;
    seen.read = 1;
    seen.present = present;
    if (present) {
        seen.ino = (long long) st.st_ino;
        seen.size = (long long) st.st_size;
        seen.sec = (long long) st.st_mtim.tv_sec;
        seen.nsec = (long long) st.st_mtim.tv_nsec;
    }
    brightgrid_read_cfg();
}
static float g_gridaa_gain = 1.0f;
static int g_gridaa_gray;   /* this build's strip gray (0 = none) */
static int g_gridaa_hooked; /* Bright Grid active: hook installed and its kill switch absent */
static int g_gridaa_ok;     /* 1 only inside FUN_01b93a28 */
static _Atomic long g_gridaa_rows, g_gridaa_calls, g_gridaa_ns;
static void gridaa_enter(long wwb, long pos_p, float zoom) {
    g_gridaa_ok = 0;
    if (!__atomic_load_n(&g_wave60_on, __ATOMIC_RELAXED))
        return; /* Bright Grid: 60 FPS Waveforms only */
    /* A light theme draws dark ticks on a light ruler, where brightening washes
     * them out; only dark themes (or Themes off) get Bright Grid. */
    if (__atomic_load_n(&g_dm_on, __ATOMIC_RELAXED) &&
        __atomic_load_n(&G_DM_FLIP, __ATOMIC_RELAXED))
        return;
    int n = __atomic_load_n(&g_wwbbase_n, __ATOMIC_RELAXED), wi = -1;
    for (int i = 0; i < n && i < 8; i++)
        if (g_wwbbase[i].base == (unsigned long) wwb) {
            wi = i;
            break;
        }
    if (wi < 0 || pos_p < 0x10000 || (pos_p & 7))
        return; /* the caller's shape only */
    /* Positions stay stock. While this widget moves, brighten the ticks
     * (g_bg_gain_k), ramped over g_bg_ramp_ms both ways so start/stop never
     * pops. State is per widget slot (g_wwbbase): a stale slot costs one ramp. */
    long pos = *(const long *) pos_p;
    static long last_pos[8], moved_ms[8], ramp_ms[8];
    static int last_ioff[8];
    /* The whole-pixel offset the ticks are built from; a change = ticks step. */
    int stepped = 0;
    {
        float sp = (float) pos * 0.15f * zoom;
        if (sp > -1e8f && sp < 1e8f) {
            int io = (int) sp;
            stepped = io != last_ioff[wi];
            last_ioff[wi] = io;
        }
    }
    __atomic_fetch_add(&g_bg_builds, 1, __ATOMIC_RELAXED);
    if (stepped)
        __atomic_fetch_add(&g_bg_steps, 1, __ATOMIC_RELAXED);
    static float lvl[8];
    long now = now_ms();
    if (pos != last_pos[wi]) {
        last_pos[wi] = pos;
        moved_ms[wi] = now;
    }
    float target =
        (now - moved_ms[wi] <= __atomic_load_n(&g_bg_move_ms, __ATOMIC_RELAXED)) ? 1.0f : 0.0f;
    long dt = ramp_ms[wi] ? now - ramp_ms[wi] : 0;
    ramp_ms[wi] = now;
    if (dt < 0 || dt > 1000)
        dt = 0;
    float step = (float) dt / (float) __atomic_load_n(&g_bg_ramp_ms, __ATOMIC_RELAXED);
    if (lvl[wi] < target) {
        lvl[wi] += step;
        if (lvl[wi] > target)
            lvl[wi] = target;
    } else if (lvl[wi] > target) {
        lvl[wi] -= step;
        if (lvl[wi] < target)
            lvl[wi] = target;
    }
    float G = __atomic_load_n(&g_bg_gain_k, __ATOMIC_RELAXED) / 1000.0f;
    float OD = __atomic_load_n(&g_bg_od_k, __ATOMIC_RELAXED) / 1000.0f;
    int gray = __atomic_load_n(&g_bg_gray, __ATOMIC_RELAXED);
    float gain = 1.0f + (G - 1.0f) * lvl[wi];
    if (stepped && lvl[wi] > 0)
        gain *= 1.0f + (OD - 1.0f) * lvl[wi];
    if (gain < 1.001f && !gray)
        return; /* still, no strip: stock */
    g_gridaa_gain = gain;
    g_gridaa_gray = gray;
    g_gridaa_ok = 1;
}
void azm_upd_exit(int prev) {
    g_gridaa_ok = 0;
    g_upd_skip = 0;
    if (g_up_deck >= 0) {
        long t = up_now_ns(), dt = t - g_up_t0;
        int d = g_up_deck;
        g_up_ns[d] += dt;
        g_up_n[d]++;
        if (dt > g_up_max[d])
            g_up_max[d] = dt;
        g_up_lastend = t;
        g_up_deck = -1;
    }
    __atomic_store_n(&g_upd_deck, prev, __ATOMIC_RELAXED);
}
#include "brightgrid.h" /* the pixel rule gridmake_hook applies */
#define GRID_MAKE EP_1dffe10 /* gridMaker.makeGridImage, 3 callers (Blue/RGB/3Band update) */
static int *(*g_orig_gridmake)(float, int *, long *, int, int, int, int);
static int *gridmake_hook(float zoom, int *gm, long *content, int off, int h, int a6, int a7) {
    uint32_t dm_keep; /* Day Mode: ruler background (day_mode.c) */
    int dm = dm_ruler_begin(gm, &dm_keep);
    int *r = g_orig_gridmake(zoom, gm, content, off, h, a6, a7);
    /* a theme's ruler background (0 = stock's black): Bright Grid never brightens it */
    uint32_t ruler_bg = dm ? dm_ruler_end(gm, dm_keep) : 0;
    if (!g_gridaa_ok || !g_gridaa_hooked)
        return r; /* Bright Grid: its own kill switch */
    /* gm+0x30 is its juce::Image::BitmapData: data, format, lineStride,
     * pixelStride, width, height (makeGridImage writes width/height there and
     * memcpy's rows at data + y*lineStride). ARGB, premultiplied. */
    const char *b = (const char *) gm;
    uint8_t *data = *(uint8_t *const *) (b + 0x30);
    int fmt = *(const int *) (b + 0x38), ls = *(const int *) (b + 0x3c),
        ps = *(const int *) (b + 0x40);
    int w = *(const int *) (b + 0x44), hh = *(const int *) (b + 0x48);
    if (!data || fmt != 2 || ps != 4 || w < 2 || w > 8192 || w != gm[0] || hh < 1 || hh > 4096 ||
        ls < w * 4)
        return r;
    float g = g_gridaa_gain;
    int gray = g_gridaa_gray;
    if (!(g >= 1.0f && g < 6.0f))
        return r;
    if (g < 1.001f && !gray)
        return r;
    uint32_t graypx = gray ? (0xffu << 24) | ((uint32_t) gray * 0x010101u) : 0;
    uint8_t lut[256];
    bg_build_lut(lut, g);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    /* makeGridImage draws one or two distinct rows and copies them down, so only
     * process a row that differs from the one above and copy the result
     * otherwise. The ticks are brightened; the ruler's background never is
     * (brightgrid.h). */
    static uint32_t prev_orig[8192], prev_new[8192];
    size_t rb = (size_t) w * 4;
    for (int y = 0; y < hh; y++) {
        uint32_t *row = (uint32_t *) (data + (long) y * ls);
        if (y > 0 && memcmp(row, prev_orig, rb) == 0) {
            memcpy(row, prev_new, rb);
            continue;
        }
        memcpy(prev_orig, row, rb);
        bg_row(row, w, lut, ruler_bg, graypx);
        memcpy(prev_new, row, rb);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    __atomic_fetch_add(&g_gridaa_ns,
                       (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec),
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_gridaa_calls, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_gridaa_rows, hh, __ATOMIC_RELAXED);
    return r;
}
/* x0-x7 + x8(sret) + q0-q7 are saved across both helper calls, and the original's
 * return registers (x0,x1,q0,q1) are preserved across azm_upd_exit. */
__asm__(".text\n.align 2\n"
        ".globl azm_upd_shim\n"
        "azm_upd_shim:\n"
        "   stp x29, x30, [sp, #-256]!\n"
        "   mov x29, sp\n"
        "   stp x19, x20, [sp, #16]\n"
        "   stp x0,  x1,  [sp, #32]\n"
        "   stp x2,  x3,  [sp, #48]\n"
        "   stp x4,  x5,  [sp, #64]\n"
        "   stp x6,  x7,  [sp, #80]\n"
        "   str x8,       [sp, #96]\n"
        "   stp q0,  q1,  [sp, #112]\n"
        "   stp q2,  q3,  [sp, #144]\n"
        "   stp q4,  q5,  [sp, #176]\n"
        "   stp q6,  q7,  [sp, #208]\n"
        "   bl  azm_upd_enter\n" /* x0 is still update()'s arg1 */
        "   mov w19, w0\n"
        "   adrp x20, g_upd_skip\n"
        "   ldr  w20, [x20, :lo12:g_upd_skip]\n"
        "   cbnz w20, 1f\n" /* deck already built this pass */
        "   ldp x0,  x1,  [sp, #32]\n"
        "   ldp x2,  x3,  [sp, #48]\n"
        "   ldp x4,  x5,  [sp, #64]\n"
        "   ldp x6,  x7,  [sp, #80]\n"
        "   ldr x8,       [sp, #96]\n"
        "   ldp q0,  q1,  [sp, #112]\n"
        "   ldp q2,  q3,  [sp, #144]\n"
        "   ldp q4,  q5,  [sp, #176]\n"
        "   ldp q6,  q7,  [sp, #208]\n"
        "   adrp x20, g_orig_wavupd\n"
        "   add  x20, x20, :lo12:g_orig_wavupd\n"
        "   ldr  x20, [x20]\n"
        "   blr  x20\n"
        "   stp x0, x1, [sp, #32]\n" /* preserve the original's return */
        "   stp q0, q1, [sp, #112]\n"
        "1: mov w0, w19\n"
        "   bl  azm_upd_exit\n"
        "   ldp x0, x1, [sp, #32]\n"
        "   ldp q0, q1, [sp, #112]\n"
        "   ldp x19, x20, [sp, #16]\n"
        "   ldp x29, x30, [sp], #256\n"
        "   ret\n");
void azm_upd_shim(void);
/* ---- WWB constructor hook: every deck's widgets, at construction ----------
 *   FUN_01b8f950(WWB, .., .., deck, ..)   <- the deck is an argument
 * Runs once per deck (about four calls per process). On return the three
 * embedded color-mode widgets and their Content pointers are populated:
 *   WWB+0x00090  WWB+0x087F8 (RGB)  WWB+0x10F60      widget+0x8688 = Content
 * Publishes WWB base -> deck (g_wwbbase), exact widget/Component/WWB pointers
 * -> deck (g_wkey) and Content -> deck (g_ctmap), so the hot path binds with
 * pointer compares and zero syscalls. */
#define WWB_CTOR EP_1b8f950
#define WIDGET_CONTENT 0x8688L       /* param_5[0] parked here by FUN_01b96638 */
#define WIDGET_COMPONENT_OFF 0x86A0L /* juce::Component base sub-object in the widget */
static const long WWB_WIDGETS[3] = {0x00090L, 0x087F8L, 0x10F60L};
static void (*g_orig_wwbctor)(long, long, long, int, long);
static struct {
    unsigned long content;
    int deck;
} g_ctmap[16];
static _Atomic int g_ctmap_n = 0;
static _Atomic int g_wwbctor_calls = 0;
static void wwbctor_hook(long self, long a2, long a3, int deck, long a5) {
    g_orig_wwbctor(self, a2, a3, deck, a5); /* widgets+Content exist only after */
    __atomic_fetch_add(&g_wwbctor_calls, 1, __ATOMIC_RELAXED);
    /* self: the WWB its constructor just built, in this call: every field read below lies
     * inside it (0x197F0 < its ~0x199a0 bytes; the three widgets at WWB_WIDGETS[]). */
    unsigned wwbdeck = 0;
    own_u32((uintptr_t) (self + 0x197F0), &wwbdeck); /* cross-check the arg landed */
    {
        int b = __atomic_load_n(&g_wwbbase_n, __ATOMIC_RELAXED);
        if (deck >= 0 && deck < AZMOD_STEM_DECKS &&
            b < (int) (sizeof g_wwbbase / sizeof g_wwbbase[0])) {
            g_wwbbase[b].base = (unsigned long) self;
            g_wwbbase[b].deck = deck;
            disp_wref_init();
            for (int m = 0; m < 3; m++) /* not tracked (no destructor hook): stays 0, unknown */
                wref_set(&g_wwb_comp[b][m], (uintptr_t) (self + WWB_WIDGETS[m] + WIDGET_COMPONENT_OFF));
            __atomic_store_n(&g_wwbbase_n, b + 1, __ATOMIC_RELAXED);
        }
    }
    for (int i = 0; i < 3; i++) {
        long w = self + WWB_WIDGETS[i];
        unsigned vt = 0;
        uintptr_t content = 0;
        own_u32((uintptr_t) w, &vt);
        own_q((uintptr_t) (w + WIDGET_CONTENT), &content);
        wwb_log("wwbctor: deck=%d(arg) %u(+0x197F0) widget[%#lx]=%lx vt=%#x content=%lx", deck,
                wwbdeck, (unsigned long) WWB_WIDGETS[i], (unsigned long) w, vt, content);
        int n = __atomic_load_n(&g_ctmap_n, __ATOMIC_RELAXED);
        if (content && deck >= 0 && deck < AZMOD_STEM_DECKS &&
            n < (int) (sizeof g_ctmap / sizeof g_ctmap[0])) {
            g_ctmap[n].content = content;
            g_ctmap[n].deck = deck;
            __atomic_store_n(&g_ctmap_n, n + 1, __ATOMIC_RELAXED);
        }
        /* Exact pointers a hook may see: the widget, its juce::Component base
         * sub-object, and the WWB itself. */
        int k = __atomic_load_n(&g_wkey_n, __ATOMIC_RELAXED);
        if (deck >= 0 && deck < AZMOD_STEM_DECKS &&
            k + 2 < (int) (sizeof g_wkey / sizeof g_wkey[0])) {
            g_wkey[k].ptr = (unsigned long) w;
            g_wkey[k].deck = deck;
            g_wkey[k + 1].ptr = (unsigned long) (w + WIDGET_COMPONENT_OFF);
            g_wkey[k + 1].deck = deck;
            g_wkey[k + 2].ptr = (unsigned long) self;
            g_wkey[k + 2].deck = deck;
            __atomic_store_n(&g_wkey_n, k + 3, __ATOMIC_RELAXED); /* self = the WWB */
        }
    }
}
/* Provider -> deck by Content pointer (provider+0x8) against the ctor's table.
 * No syscalls; -1 when no entry matches. */
static _Atomic int g_ctprobe = 8;
static int wwb_deck_by_content(long self) {
    int n = __atomic_load_n(&g_ctmap_n, __ATOMIC_RELAXED);
    if (n <= 0)
        return -1;
    unsigned long c = *(volatile unsigned long *) (self + 8);
    for (int i = 0; i < n; i++)
        if (g_ctmap[i].content == c)
            return g_ctmap[i].deck;
    if (__atomic_fetch_sub(&g_ctprobe, 1, __ATOMIC_RELAXED) > 0)
        wwb_log("ctmatch: provider self=%lx content(+8)=%lx NOT in table(n=%d) first=%lx",
                (unsigned long) self, c, n, n ? g_ctmap[0].content : 0UL);
    return -1;
}
/* ---- Widget paint hook (not installed; see azmod.c) -----------------------
 * DetailedWaveformWidget<Mode> multiply inherits; its juce::Component base is at
 * widget+0x86A0 (slot 12 == Component::setVisible 0x0249daf0). Slot 26 (+0xD0)
 * is paint, shared by all three modes at 0x01e071a0:
 *
 *     Blue  primary vt 0x02b66d98  @ WWB+0x00090   Component vt 0x02b66f68
 *     RGB   primary vt 0x02b67408  @ WWB+0x087F8   Component vt 0x02b675d8
 *     3Band primary vt 0x02b67a78  @ WWB+0x10F60   Component vt 0x02b67c48
 *
 * paint's `this` is the Component, so widget = this - 0x86A0 and the deck is
 * the mode's offset to WWB+0x197F0; both candidates are validated against the
 * primary vtable (real method or thunk). 0x01e071a0 is shared with many other
 * components and never called for these widgets, which update() drives. */
#define WAVEW_PAINT EP_1e071a0 /* DetailedWaveformWidget<*>::paint */
static struct {
    unsigned long vt;
    long deckoff;
    long wwboff;
} WAVEW_MODES[3] = {
    /* .vt filled by ep147_late_init */
    {0 /* EP_2b66d98 */, 0x19760L, 0x00090L}, /* Blue  @ WWB+0x00090 : 0x197F0-0x00090 */
    {0 /* EP_2b67408 */, 0x10FF8L, 0x087F8L}, /* RGB   @ WWB+0x087F8 : 0x197F0-0x087F8 */
    {0 /* EP_2b67a78 */, 0x08890L, 0x10F60L}, /* 3Band @ WWB+0x10F60 : 0x197F0-0x10F60 */
};
/* Message thread only: g_wwbbase entry b's color-mode widget m, or 0 = unknown. Alive because
 * its wref slot is still set (juce::Component's destructor clears it), and of the expected
 * class at this use: aligned, primary vtable = WAVEW_MODES[m].vt. A slot that fails the check
 * is cleared. A live embedded widget also means its WWB (g_wwbbase[b].base) is alive. */
static uintptr_t wwb_widget_live(int b, int m) {
    if (b < 0 || b >= 8 || m < 0 || m >= 3 || !on_msg_thread())
        return 0;
    uintptr_t comp = wref_get(&g_wwb_comp[b][m]), vt = 0;
    if (!comp || !WAVEW_MODES[m].vt) /* no vtable to check yet: unknown, the slot kept */
        return 0;
    uintptr_t w = comp - (uintptr_t) WIDGET_COMPONENT_OFF;
    if (!own_q(w, &vt) || vt != WAVEW_MODES[m].vt) {
        wref_set(&g_wwb_comp[b][m], 0);
        return 0;
    }
    return w;
}
static void (*g_orig_wavewpaint)(long, long);
static _Atomic long g_wpaint_calls = 0;
static struct {
    unsigned long self;
    int deck;
} g_wpmemo[16]; /* incl. negatives */
static _Atomic int g_wpmemo_n = 0;
static int wavew_deck_of(long self) {
    int n = __atomic_load_n(&g_wpmemo_n, __ATOMIC_RELAXED);
    for (int i = 0; i < n; i++)
        if (g_wpmemo[i].self == (unsigned long) self)
            return g_wpmemo[i].deck;
    /* exact pointers published by the WWB ctor: pure compares, no syscalls */
    int kn = __atomic_load_n(&g_wkey_n, __ATOMIC_RELAXED);
    for (int i = 0; i < kn; i++)
        if (g_wkey[i].ptr == (unsigned long) self) {
            if (n < (int) (sizeof g_wpmemo / sizeof g_wpmemo[0])) {
                g_wpmemo[n].self = (unsigned long) self;
                g_wpmemo[n].deck = g_wkey[i].deck;
                __atomic_store_n(&g_wpmemo_n, n + 1, __ATOMIC_RELAXED);
            }
            return g_wkey[i].deck;
        }
    /* The key table above already holds every widget and its juce::Component sub-object, as
     * published by the WWB ctor; this paint function is shared with many other components,
     * so in a release build a miss is unknown. The hook is not installed (see azmod.c). */
    int deck = -1;
    if (n < (int) (sizeof g_wpmemo / sizeof g_wpmemo[0])) {
        g_wpmemo[n].self = (unsigned long) self;
        g_wpmemo[n].deck = deck;
        __atomic_store_n(&g_wpmemo_n, n + 1, __ATOMIC_RELAXED);
    }
    return deck;
}
static void wavewpaint_hook(long self, long g) {
    __atomic_fetch_add(&g_wpaint_calls, 1, __ATOMIC_RELAXED);
    int d = wavew_deck_of(self);
    int prev = __atomic_load_n(&g_upd_deck, __ATOMIC_RELAXED);
    if (d >= 0)
        __atomic_store_n(&g_upd_deck, d, __ATOMIC_RELAXED);
    g_orig_wavewpaint(self, g);
    __atomic_store_n(&g_upd_deck, prev, __ATOMIC_RELAXED); /* nested paints restore */
}
static void sw2d_paint_hook(long self, long g) {
    __atomic_fetch_add(&g_sw2d_paints, 1, __ATOMIC_RELAXED);
    int d = *(volatile int *) (self + SW2D_DECK_OFF);
    __atomic_store_n(&g_sw2d_last_deck, d, __ATOMIC_RELAXED);
    int prev = __atomic_load_n(&g_cur_paint_deck, __ATOMIC_RELAXED);
    if (d >= 0 && d < AZMOD_STEM_DECKS)
        __atomic_store_n(&g_cur_paint_deck, d, __ATOMIC_RELAXED);
    g_orig_sw2d_paint(self, g);
    __atomic_store_n(&g_cur_paint_deck, prev, __ATOMIC_RELAXED);
}

/* The deck of the FUN_01b93a28 running right now (azm_upd_shim): the deck whose waveform
 * EP147 is building in this call. One relaxed load; -1 = not inside one. */
static inline int wave_upd_deck(void) {
    if (g_wave_noid)
        return -1;
    int d = __atomic_load_n(&g_upd_deck, __ATOMIC_RELAXED);
    return (d >= 0 && d < AZMOD_STEM_DECKS) ? d : -1;
}

/* Hot path: once per waveform point (~800k/s) on the JUCE message thread, for the
 * provider `self` that EP147 just called. Keep it branch-light and syscall-free; a
 * single access() here costs a whole core. `kind` is the accessor's color mode
 * (WSK_*), passed by its shim. */
void azm_wp_scale(long self, int idx, unsigned char *out, int band, int kind);
void azm_wp_scale(long self, int idx, unsigned char *out, int band, int kind) {
    (void) band;
    g_wp_calls++; /* plain: single writer, this thread */
    /* One relaxed load when every waveform mod is off (the common case). */
    if (!g_wave_any) {
        g_wp_gateoff++;
        return;
    }
    /* The deck, by identity only: the update() running now, else the provider's Content
     * against the WWB ctor's table. Unknown means this point is left as EP147 drew it and
     * feeds no zoom model; a provider's address or point count never stands in for a deck. */
    int deck = wave_upd_deck();
    if (deck >= 0)
        g_wp_updhit++;
    else if (!g_wave_noid && (deck = wwb_deck_by_content(self)) >= 0 && deck < AZMOD_STEM_DECKS)
        g_wp_cthit++;
    else {
        g_wp_nodeck++;
        return;
    }
    if ((unsigned) kind >= WSK_N) {
        g_wp_nodeck++;
        return;
    }
    /* The point count changes per track. Read it only here, never from the poll thread:
     * this is the one context where the object is provably alive (it called us). */
    int cnt = *(volatile int *) (self + WAVEPROV_CNT);
    if (cnt <= 0) {
        g_wp_nocnt++;
        return;
    }
    wzoom *z = &g_wz[deck][kind];
    if (cnt != z->total) { /* a new track on this deck: the old model no longer applies */
        z->total = cnt;
        z->mul = (int) (((long) ATT_N << 16) / cnt);
        __atomic_store_n(&z->ok, 0, __ATOMIC_RELAXED);
    }
    /* Zoom sampling: plain ints, single writer (this thread); see wzoom. */
    if (z->cnt++ == 0)
        z->min = z->max = idx;
    else if (idx < z->min)
        z->min = idx;
    else if (idx > z->max)
        z->max = idx;
    if (!g_wave_scale || !__atomic_load_n(&g_att_have[deck], __ATOMIC_RELAXED)) {
        g_wp_gated[deck]++;
        return;
    }
    g_wp_scaled[deck]++; /* this deck is genuinely reflecting */
    /* Every stem on and no color: out[0]*256>>8 == out[0], so skip the LUT. */
    if (!g_wave_color && __atomic_load_n(&g_att_ident[deck], __ATOMIC_RELAXED))
        return;
    /* idx -> LUT slot without a 64-bit divide: .mul is ATT_N/cnt in 16.16. */
    long b = ((long) idx * z->mul) >> 16;
    if (b < 0)
        b = 0;
    else if (b >= ATT_N)
        b = ATT_N - 1;
    out[0] = (unsigned char) ((out[0] * __atomic_load_n(&g_att[deck][b], __ATOMIC_RELAXED)) >> 8);
    /* out+1 is a juce::Colour, 4 bytes B,G,R,A (ctor 0x23a5080), unaligned, so
     * write bytewise. Zero-height columns (the flat center line beside the
     * track) keep EP147's own color. */
    if (out[0] && g_wave_color) {
        unsigned c = __atomic_load_n(&g_col[deck][b], __ATOMIC_RELAXED);
        if (c) {
            out[1] = (unsigned char) c;
            out[2] = (unsigned char) (c >> 8);
            out[3] = (unsigned char) (c >> 16);
            out[4] = 0xff;
        }
    }
}

/* The accessor returns its result through x8 (sret), which C cannot express,
 * so each hook is a shim: stash x8/x0/x1, call the original with x8 restored,
 * then hand the filled-in buffer to azm_wp_scale with the accessor's color mode
 * (KIND, a WSK_* value) in w4. x0-x7 are passed through untouched, and an sret
 * function also returns the buffer in x0. */
static void *g_orig_wp6_3band, *g_orig_wp6_rgb, *g_orig_wp6_blue;
#define AZM_WP6_SHIM(NAME, ORIG, KIND)            \
    __asm__(".text\n.align 2\n"                   \
            ".globl " #NAME "\n" #NAME ":\n"      \
            "   stp x29, x30, [sp, #-64]!\n"      \
            "   mov x29, sp\n"                    \
            "   stp x19, x20, [sp, #16]\n"        \
            "   stp x21, x22, [sp, #32]\n"        \
            "   str x23, [sp, #48]\n"             \
            "   mov x19, x8\n"                    \
            "   mov x20, x0\n"                    \
            "   mov w21, w1\n"                    \
            "   mov w23, w2\n"                    \
            "   adrp x22, " #ORIG "\n"            \
            "   add  x22, x22, :lo12:" #ORIG "\n" \
            "   ldr  x22, [x22]\n"                \
            "   mov  x8, x19\n"                   \
            "   blr  x22\n"                       \
            "   mov  x0, x20\n"                   \
            "   mov  w1, w21\n"                   \
            "   mov  x2, x19\n"                   \
            "   mov  w3, w23\n"                   \
            "   mov  w4, #" #KIND "\n"            \
            "   bl   azm_wp_scale\n"              \
            "   mov  x0, x19\n"                   \
            "   ldr  x23, [sp, #48]\n"            \
            "   ldp  x21, x22, [sp, #32]\n"       \
            "   ldp  x19, x20, [sp, #16]\n"       \
            "   ldp  x29, x30, [sp], #64\n"       \
            "   ret\n")
AZM_WP6_SHIM(azm_wp6_3band, g_orig_wp6_3band, 2); /* WSK_3BAND */
AZM_WP6_SHIM(azm_wp6_rgb, g_orig_wp6_rgb, 0);       /* WSK_RGB */
AZM_WP6_SHIM(azm_wp6_blue, g_orig_wp6_blue, 1);     /* WSK_BLUE */
void azm_wp6_3band(void);
void azm_wp6_rgb(void);
void azm_wp6_blue(void);

/* ---- Stem height scaling once per track/mask, not once per point ----------
 * The point accessors are pure reads of one packed array, [[provider+8]+0x28]
 * (Content+0x28), called from renderWaveform_new 0x1e00dd0(.., x3 = provider,
 * .., 2 stack args) in the three DetailedWaveformWidget::update()s:
 *
 *     RGB   0x1b948d0  2 B/pt   height = (b[1] >> 1) & 0x1f
 *     Blue  0x1b949b8  1 B/pt   height = b >> 3        (color = b & 7)
 *     3Band 0x1b95e80  6 B/pt   height = b[2 + band]   (color from b[0..1] only)
 *
 * So keep a copy of that array with the heights pre-scaled (rebuilt when the
 * track or stem mask changes), and for the duration of the render call point
 * the provider's Content at a stand-in whose +0x28 is the copy. The result is
 * bit-for-bit what azm_wp_scale produces; color bits are untouched. The
 * provider is restored before render returns, so nothing outside this one
 * synchronous call sees the stand-in.
 *
 * Stem tint and stem lanes still need every point, so while either is on the
 * provider's vtable is swapped for a copy whose slot 6 is the per-point shim.
 * Kill switch: settings/azmod-wscale.off installs global point hooks instead. */
#define WAVE_RENDER EP_1e00dd0
/* ---- Fused point accessors: EP147's slot-6 accessors, re-implemented --------
 * The stock accessors make five out-of-line juce::Colour calls per point for a
 * few loads and shifts. These are exact C versions, taken from the instructions
 * (the decompile garbles the index test):
 *
 *   valid   RGB/Blue: flag(+0x10) != 0 && a4 <= 0 && 0 <= idx < cnt(+0x14)
 *           3Band:    (flag & (idx >= 0)) && idx < cnt, then band < (u16 >> 12)
 *   data    arr = [[prov+8] (ldar) + 0x28]
 *   RGB     2 B/pt: h = (b1>>1)&0x1f; Colour(r=(b0&7)<<5, g=((b0>>3)&7)<<5,
 *                   b=((u16>>6)&7)<<5) = bytes b,g,r,0xff (ctor 0x23a5080)
 *   Blue    1 B/pt: h = v>>3; color = the u32 at 0x3b8dcb8 + 8*(v&7)
 *   3Band   6 B/pt: h = (band&0xff) < 3 ? b[2+(band&0xff)] : 0; color =
 *                   FUN_01b95d00: nibble (u16 >> 4*band) & 0xf -> table at 0x3b8dcb8
 *   invalid h = 0, color = 0 (default ctor)
 *   out     out[0] = h, out[1..4] = color, returned through x8 (sret)
 *
 * Reached only through the provider vtable copy installed for one render call
 * (never an entry patch), reading whatever Content the provider holds then, so
 * they inherit the scaled copy. A null Content returns 0 and the stub
 * tail-calls EP147's accessor, which raises its fatal "p != nullptr" as stock.
 * Compare mode (settings/azmod-wpcmp.on) also runs EP147's accessor and counts
 * byte mismatches (zero across all three modes). Kill switch azmod-wpfast.off. */
#define WP_COLTAB EP_3b8dcb8
static const unsigned char WP3_NIB_OFF[16] = {/* 3Band nibble -> table offset */
                                              0x48, 0x40, 0x78, 0x70, 0x68, 0x60, 0x58, 0x48,
                                              0x50, 0x48, 0x48, 0x48, 0x48, 0x48, 0x48, 0x48};
static int g_wp_fast = 1;                      /* settings/azmod-wpfast.off -> EP147's accessors */
static int g_wp_cmp = 0;                       /* settings/azmod-wpcmp.on */
static unsigned long g_wpc_n[3], g_wpc_bad[3]; /* message thread only */
static _Atomic int g_wpc_log = 20;
static void *g_wp_real[3] __attribute__((used)); /* filled by ep147_late_init */
/* x0 = fn, x1 = out (x8), x2 = provider, w3 = idx, w4 = a4 */
__asm__(".text\n.align 2\n.globl azm_call_sret\nazm_call_sret:\n"
        "   stp x29, x30, [sp, #-16]!\n   mov x29, sp\n"
        "   mov x16, x0\n   mov x8, x1\n   mov x0, x2\n   mov w1, w3\n   mov w2, w4\n"
        "   blr x16\n   ldp x29, x30, [sp], #16\n   ret\n");
void azm_call_sret(void *fn, unsigned char *out, long prov, int idx, int a4);
static void wp_check(int k, const unsigned char *ours, long prov, int idx, int a4) {
    unsigned char ref[8];
    azm_call_sret(g_wp_real[k], ref, prov, idx, a4);
    g_wpc_n[k]++;
    if (memcmp(ours, ref, 5)) {
        g_wpc_bad[k]++;
        if (__atomic_fetch_sub(&g_wpc_log, 1, __ATOMIC_RELAXED) > 0) {
            FILE *f = azlog_open("/tmp/azmod-wpcmp.log", "a");
            if (f) {
                fprintf(
                    f, "k=%d idx=%d a4=%d ours=%02x %02x%02x%02x%02x ep147=%02x %02x%02x%02x%02x\n",
                    k, idx, a4, ours[0], ours[1], ours[2], ours[3], ours[4], ref[0], ref[1], ref[2],
                    ref[3], ref[4]);
                fclose(f);
            }
        }
    }
}
static inline void wp_put(unsigned char *out, unsigned h, unsigned col) {
    out[0] = (unsigned char) h;
    memcpy(out + 1, &col, 4);
}
int azm_fast_rgb(unsigned char *out, long prov, int idx, int a4);
int azm_fast_rgb(unsigned char *out, long prov, int idx, int a4) {
    unsigned h = 0, col = 0;
    if (*(volatile unsigned char *) (prov + 0x10) && a4 <= 0 && idx >= 0 &&
        idx < *(volatile int *) (prov + 0x14)) {
        long c = __atomic_load_n((long *) (prov + 8), __ATOMIC_ACQUIRE);
        if (!c)
            return 0;
        const unsigned char *p = *(const unsigned char **) (c + 0x28) + 2 * (long) idx;
        unsigned b0 = p[0], b1 = p[1];
        h = (b1 >> 1) & 0x1f;
        col = ((((b0 | (b1 << 8)) >> 6) & 7) << 5) | ((((b0 >> 3) & 7) << 5) << 8) |
              (((b0 & 7) << 5) << 16) | 0xff000000u;
    }
    wp_put(out, h, col);
    if (g_wp_cmp)
        wp_check(0, out, prov, idx, a4);
    return 1;
}
int azm_fast_blue(unsigned char *out, long prov, int idx, int a4);
int azm_fast_blue(unsigned char *out, long prov, int idx, int a4) {
    unsigned h = 0, col = 0;
    if (*(volatile unsigned char *) (prov + 0x10) && a4 <= 0 && idx >= 0 &&
        idx < *(volatile int *) (prov + 0x14)) {
        long c = __atomic_load_n((long *) (prov + 8), __ATOMIC_ACQUIRE);
        if (!c)
            return 0;
        unsigned v = (*(const unsigned char **) (c + 0x28))[idx];
        h = v >> 3;
        col = *(const unsigned *) (WP_COLTAB + 8 * (v & 7));
    }
    wp_put(out, h, col);
    if (g_wp_cmp)
        wp_check(1, out, prov, idx, a4);
    return 1;
}
int azm_fast_3band(unsigned char *out, long prov, int idx, int band);
int azm_fast_3band(unsigned char *out, long prov, int idx, int band) {
    unsigned h = 0, col = 0;
    if ((*(volatile unsigned char *) (prov + 0x10) & ((unsigned) ~idx >> 31)) &&
        idx < *(volatile int *) (prov + 0x14)) {
        long c = __atomic_load_n((long *) (prov + 8), __ATOMIC_ACQUIRE);
        if (!c)
            return 0;
        const unsigned char *p = *(const unsigned char **) (c + 0x28) + 6 * (long) idx;
        unsigned u = p[0] | (p[1] << 8), bb = (unsigned) band & 0xff;
        if (band < (int) (u >> 12)) {
            h = bb < 3 ? p[2 + bb] : 0;
            col = *(const unsigned *) (WP_COLTAB +
                                       (bb < 3 ? WP3_NIB_OFF[(u >> (bb * 4)) & 0xf] : 0x48));
        }
    }
    wp_put(out, h, col);
    if (g_wp_cmp)
        wp_check(2, out, prov, idx, band);
    return 1;
}
/* sret stub: x8 = out, x0 = provider, w1 = idx, w2 = a4. The C body returns 0
 * only for a null Content; then the original registers go to EP147's accessor. */
#define AZM_FAST_STUB(NAME, CFN, K)                                                            \
    __asm__(".text\n.align 2\n.globl " #NAME "\n" #NAME ":\n"                                  \
            "   stp x29, x30, [sp, #-48]!\n   mov x29, sp\n"                                   \
            "   stp x0, x1, [sp, #16]\n   stp x2, x8, [sp, #32]\n"                             \
            "   mov x3, x2\n   mov w2, w1\n   mov x1, x0\n   mov x0, x8\n"                     \
            "   bl " #CFN "\n"                                                                 \
            "   cbz w0, 1f\n"                                                                  \
            "   ldr x0, [sp, #40]\n   ldp x29, x30, [sp], #48\n   ret\n"                       \
            "1: ldp x0, x1, [sp, #16]\n   ldp x2, x8, [sp, #32]\n   ldp x29, x30, [sp], #48\n" \
            "   adrp x16, g_wp_real\n   add x16, x16, :lo12:g_wp_real\n"                       \
            "   ldr x16, [x16, #" #K "*8]\n   br x16\n")
AZM_FAST_STUB(azm_fast6_rgb, azm_fast_rgb, 0);
AZM_FAST_STUB(azm_fast6_blue, azm_fast_blue, 1);
AZM_FAST_STUB(azm_fast6_3band, azm_fast_3band, 2);
void azm_fast6_rgb(void);
void azm_fast6_blue(void);
void azm_fast6_3band(void);
static const int WSK_STRIDE[WSK_N] = {2, 1, 6};
static void *g_orig_render;
typedef struct {
    const unsigned char *src; /* EP147's array this copy was built from */
    int cnt;
    unsigned gen;
    unsigned char *pristine; /* src as it was when scaled (re-checked in slices) */
    unsigned char *scaled;
    size_t cap;
    int chk;      /* next point the slice re-check starts at */
    long fake[6]; /* stand-in Content: the accessors read only +0x28 */
} wscache;
static wscache g_wsc[AZMOD_STEM_DECKS][WSK_N];       /* message thread only */
static _Atomic unsigned g_att_gen[AZMOD_STEM_DECKS]; /* bumped by build_atten */
/* Provider vtable copies: [0] offset-to-top, [1] typeinfo, [2..9] slots 0..7.
 * g_wvt: slot 6 = the per-point scaling shim (TINT / LANES); g_wvt_fast: slot 6 =
 * the fused accessor (or EP147's own with azmod-wpfast.off). */
static long g_wvt[WSK_N][10], g_wvt_fast[WSK_N][10];
static int g_wave_lanes = 0; /* STEM LANES on: needs the per-point sampling */
static int g_ws_forcepp = 0; /* /tmp/azmod-wscale.pp: A/B against the per-point path */
static _Atomic long g_ws_n[3],
    g_ws_rns[3]; /* renders + their ns, by path: 0 stock 1 copy 2 point */
static _Atomic long g_ws_ns, g_ws_builds, g_ws_resync, g_ws_nodeck;
static long ws_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000L + t.tv_nsec;
}
static void wsc_scale(int kind, const unsigned short *att, const unsigned char *s, unsigned char *d,
                      int i0, int i1, int mul) {
    for (int i = i0; i < i1; i++) {
        long b = ((long) i * mul) >> 16; /* exactly azm_wp_scale's index */
        if (b < 0)
            b = 0;
        else if (b >= ATT_N)
            b = ATT_N - 1;
        unsigned a = __atomic_load_n(&att[b], __ATOMIC_RELAXED); /* rebuilt concurrently, see build_atten */
        if (kind == WSK_RGB) {
            unsigned v = s[2 * i + 1], h = (((v >> 1) & 0x1f) * a) >> 8;
            d[2 * i] = s[2 * i];
            d[2 * i + 1] = (unsigned char) ((v & ~0x3eu) | (h << 1));
        } else if (kind == WSK_BLUE) {
            unsigned v = s[i];
            d[i] = (unsigned char) (((((v >> 3) * a) >> 8) << 3) | (v & 7));
        } else {
            const unsigned char *p = s + 6 * i;
            unsigned char *q = d + 6 * i;
            q[0] = p[0];
            q[1] = p[1];
            q[5] = p[5];
            q[2] = (unsigned char) ((p[2] * a) >> 8);
            q[3] = (unsigned char) ((p[3] * a) >> 8);
            q[4] = (unsigned char) ((p[4] * a) >> 8);
        }
    }
}
/* save[0] = real Content (0 = untouched), save[1] = real vptr (0 = untouched),
 * save[2] = start ns, save[3] = path. Lives on the shim's frame: reentrant. */
void azm_rw_enter(long prov, long *save);
void azm_rw_enter(long prov, long *save) {
    save[0] = save[1] = save[2] = save[3] = 0;
    long t0 = ws_now_ns();
    long vt = *(volatile long *) prov;
    int kind = -1;
    for (int k = 0; k < WSK_N; k++)
        if (vt == (long) WSK_VT[k])
            kind = k;
    if (kind < 0)
        goto out;
    int deck = wave_upd_deck();
    int dok = deck >= 0;
    /* Per-point scaling only where a mod needs every point, and only for a
     * known deck. */
    if (g_wave_any && dok && (g_wave_lanes || g_wave_color || g_ws_forcepp)) {
        *(volatile long *) prov = (long) &g_wvt[kind][2];
        save[1] = vt;
        save[3] = 2;
        goto out;
    }
    /* Everyone else, stems or not, gets the fused accessor. */
    *(volatile long *) prov = (long) &g_wvt_fast[kind][2];
    save[1] = vt;
    if (!g_wave_any)
        goto out;
    /* Unknown deck: heights stay stock. Never a default deck. */
    if (!dok) {
        __atomic_fetch_add(&g_ws_nodeck, 1, __ATOMIC_RELAXED);
        goto out;
    }
    /* Same gates azm_wp_scale applies before touching a height. */
    if (!g_wave_scale || !__atomic_load_n(&g_att_have[deck], __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&g_att_ident[deck], __ATOMIC_RELAXED))
        goto out;
    long content = *(volatile long *) (prov + 8);
    int cnt = *(volatile int *) (prov + WAVEPROV_CNT);
    if (content < 0x10000 || (content & 7) || cnt <= 0)
        goto out;
    const unsigned char *src = *(const unsigned char *volatile *) (content + 0x28);
    if (!src)
        goto out;
    wscache *c = &g_wsc[deck][kind];
    int st = WSK_STRIDE[kind];
    int mul = (int) (((long) ATT_N << 16) / cnt);
    unsigned gen = __atomic_load_n(&g_att_gen[deck], __ATOMIC_ACQUIRE);
    if (c->src != src || c->cnt != cnt || c->gen != gen) {
        size_t need = (size_t) cnt * st;
        if (c->cap < need) {
            unsigned char *p = malloc(need), *s = malloc(need);
            if (!p || !s) {
                free(p);
                free(s);
                goto out;
            }
            free(c->pristine);
            free(c->scaled);
            c->pristine = p;
            c->scaled = s;
            c->cap = need;
        }
        memcpy(c->pristine, src, need);
        wsc_scale(kind, g_att[deck], c->pristine, c->scaled, 0, cnt, mul);
        c->src = src;
        c->cnt = cnt;
        c->chk = 0;
        /* build_atten rewrote the curve while we read it: rebuild next frame. */
        c->gen = (__atomic_load_n(&g_att_gen[deck], __ATOMIC_ACQUIRE) == gen) ? gen : gen - 1;
        __atomic_fetch_add(&g_ws_builds, 1, __ATOMIC_RELAXED);
    } else {
        /* The array can change under the same pointer (a track analyzed while
         * loaded): re-check 1/64 per render, so a change lands within ~64 frames. */
        int n = cnt / 64 + 1, i0 = c->chk;
        if (i0 + n > cnt)
            n = cnt - i0;
        size_t o = (size_t) i0 * st, len = (size_t) n * st;
        if (memcmp(src + o, c->pristine + o, len)) {
            memcpy(c->pristine + o, src + o, len);
            wsc_scale(kind, g_att[deck], c->pristine, c->scaled, i0, i0 + n, mul);
            __atomic_fetch_add(&g_ws_resync, 1, __ATOMIC_RELAXED);
        }
        c->chk = (i0 + n >= cnt) ? 0 : i0 + n;
    }
    memcpy(c->fake, (const void *) content, 0x28);
    c->fake[5] = (long) c->scaled; /* +0x28 */
    save[0] = content;
    save[3] = 1;
    *(volatile long *) (prov + 8) = (long) c->fake;
out:
    __atomic_fetch_add(&g_ws_ns, ws_now_ns() - t0, __ATOMIC_RELAXED);
    save[2] = ws_now_ns();
}
void azm_rw_exit(long prov, long *save);
void azm_rw_exit(long prov, long *save) {
    if (save[0])
        *(volatile long *) (prov + 8) = save[0];
    if (save[1])
        *(volatile long *) prov = save[1];
    if (save[2]) {
        int p = (int) save[3];
        __atomic_fetch_add(&g_ws_n[p], 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_ws_rns[p], ws_now_ns() - save[2], __ATOMIC_RELAXED);
    }
}
/* renderWaveform_new takes 8 register args + 2 stack args ([sp], [sp+8] at the
 * call: strb at 0x1b98140/0x1b9814c). x0-x8 and q0-q7 are restored before the
 * original, the two stack slots are re-pushed below our frame, and x0/x1/q0/q1
 * of its return are preserved across the exit call. */
__asm__(".text\n.align 2\n"
        ".globl azm_rw_shim\n"
        "azm_rw_shim:\n"
        "   stp x29, x30, [sp, #-304]!\n"
        "   mov x29, sp\n"
        "   stp x19, x20, [sp, #16]\n"
        "   stp x0,  x1,  [sp, #32]\n"
        "   stp x2,  x3,  [sp, #48]\n"
        "   stp x4,  x5,  [sp, #64]\n"
        "   stp x6,  x7,  [sp, #80]\n"
        "   str x8,       [sp, #96]\n"
        "   stp q0,  q1,  [sp, #112]\n"
        "   stp q2,  q3,  [sp, #144]\n"
        "   stp q4,  q5,  [sp, #176]\n"
        "   stp q6,  q7,  [sp, #208]\n"
        "   mov x19, x3\n" /* the provider */
        "   mov x0,  x3\n"
        "   add x1,  sp, #240\n" /* save[4] */
        "   bl  azm_rw_enter\n"
        "   ldp x0,  x1,  [sp, #32]\n"
        "   ldp x2,  x3,  [sp, #48]\n"
        "   ldp x4,  x5,  [sp, #64]\n"
        "   ldp x6,  x7,  [sp, #80]\n"
        "   ldr x8,       [sp, #96]\n"
        "   ldp q0,  q1,  [sp, #112]\n"
        "   ldp q2,  q3,  [sp, #144]\n"
        "   ldp q4,  q5,  [sp, #176]\n"
        "   ldp q6,  q7,  [sp, #208]\n"
        "   sub sp,  sp,  #16\n"
        "   ldr x20, [x29, #304]\n" /* caller's [sp]   */
        "   str x20, [sp]\n"
        "   ldr x20, [x29, #312]\n" /* caller's [sp+8] */
        "   str x20, [sp, #8]\n"
        "   adrp x20, g_orig_render\n"
        "   add  x20, x20, :lo12:g_orig_render\n"
        "   ldr  x20, [x20]\n"
        "   blr  x20\n"
        "   add sp,  sp,  #16\n"
        "   stp x0,  x1,  [sp, #32]\n"
        "   stp q0,  q1,  [sp, #112]\n"
        "   mov x0,  x19\n"
        "   add x1,  sp, #240\n"
        "   bl  azm_rw_exit\n"
        "   ldp x0,  x1,  [sp, #32]\n"
        "   ldp q0,  q1,  [sp, #112]\n"
        "   ldp x19, x20, [sp, #16]\n"
        "   ldp x29, x30, [sp], #304\n"
        "   ret\n");
void azm_rw_shim(void);
/* 1 = render hook in place, 0 = global point hooks (kill switch / refused). */
static int install_wave_scale(void) {
    g_wp_fast = access("/home/root/settings/azmod-wpfast.off", F_OK) != 0;
    g_wp_cmp = AZ_PROBE_ON("/home/root/settings/azmod-wpcmp.on");
    if (access("/home/root/settings/azmod-wscale.off", F_OK) != 0) {
        /* The scaling shim's "original" is the fused accessor (or EP147's). */
        g_orig_wp6_rgb = g_wp_fast ? (void *) azm_fast6_rgb : (void *) WAVE_PT6_RGB;
        g_orig_wp6_blue = g_wp_fast ? (void *) azm_fast6_blue : (void *) WAVE_PT6_BLUE;
        g_orig_wp6_3band = g_wp_fast ? (void *) azm_fast6_3band : (void *) WAVE_PT6_3BAND;
        void *shim[WSK_N] = {(void *) azm_wp6_rgb, (void *) azm_wp6_blue, (void *) azm_wp6_3band};
        void *fast[WSK_N] = {(void *) azm_fast6_rgb, (void *) azm_fast6_blue,
                             (void *) azm_fast6_3band};
        for (int k = 0; k < WSK_N; k++) {
            const long *vt = (const long *) (WSK_VT[k] - 16);
            for (int i = 0; i < 10; i++)
                g_wvt[k][i] = g_wvt_fast[k][i] = vt[i];
            g_wvt[k][2 + 6] = (long) shim[k];
            if (g_wp_fast)
                g_wvt_fast[k][2 + 6] = (long) fast[k];
        }
        if (azm_install_hook(WAVE_RENDER, (void *) azm_rw_shim, &g_orig_render) == 1)
            return 1;
    }
    azm_install_hook(WAVE_PT6_3BAND, (void *) azm_wp6_3band, &g_orig_wp6_3band);
    azm_install_hook(WAVE_PT6_RGB, (void *) azm_wp6_rgb, &g_orig_wp6_rgb);
    azm_install_hook(WAVE_PT6_BLUE, (void *) azm_wp6_blue, &g_orig_wp6_blue);
    return 0;
}

/* azstem.so: usable stem frames for a deck (0 = none). */
static int (*g_stem_frames)(int deck);
/* /tmp/azmod-waveprov: the waveform hooks' counters and zoom models, rewritten each slow
 * tick of the overlay (only with azmod-logs.on). */
static void wave_prov_report(void) {
    FILE *f = azlog_open("/tmp/azmod-waveprov", "w");
    if (f) {
        {
            int on = __atomic_load_n(&g_ovi_n, __ATOMIC_ACQUIRE);
            fprintf(f, "overview: calls=%ld applied=%ld instances=%d\n",
                    __atomic_load_n(&g_ov_calls, __ATOMIC_RELAXED),
                    __atomic_load_n(&g_ov_applied, __ATOMIC_RELAXED), on);
            fprintf(f, "erp crc: ok=%ld bad=%ld\n", g_erp_crc_ok, g_erp_crc_bad);
            for (int i = 0; i < on; i++) {
                long s = g_ovi[i].self;
                /* Diagnostic: ints around self (SimpleWaveform2DeckWidget keeps
                 * its deck at +0x98). */
                int pr[8];
                const int off[8] = {0x80, 0x88, 0x90, 0x94, 0x98, 0x9c, 0xa0, 0xb0};
                for (int q = 0; q < 8; q++)
                    pr[q] = s ? *(volatile int *) (s + off[q]) : -999;
                fprintf(f,
                        "  ov %d: self=%#lx cnt=%d deck=%d  probe[80=%d 88=%d 90=%d 94=%d 98=%d "
                        "9c=%d a0=%d b0=%d]\n",
                        i, (unsigned long) s, g_ovi[i].cnt, g_ovi[i].deck, pr[0], pr[1], pr[2],
                        pr[3], pr[4], pr[5], pr[6], pr[7]);
            }
        }
        static const char *const kname[WSK_N] = {"rgb", "blue", "3band"};
        for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
            double ppc = 0, ph = 0, nc = 0, tt = 0;
            fprintf(f, "zoom d%d: ok=%d kind=%d ppc=%.3f phcol=%.1f ncol=%.0f total=%.0f\n", d,
                    azmod_wave_zoom(d, &ppc, &ph, &nc, &tt), wave_active_kind(d), ppc, ph, nc, tt);
            for (int k = 0; k < WSK_N; k++) {
                const wzoom *z = &g_wz[d][k];
                if (!z->total && !z->active && !z->pd_n)
                    continue;
                fprintf(f,
                        "  d%d %s: active=%d ok=%d ppc=%.3f total=%d pend=%d/%d age=%d\n", d,
                        kname[k], z->active, __atomic_load_n(&z->ok, __ATOMIC_RELAXED), z->ppc,
                        z->total, z->pd_n, z->pd_max - z->pd_min, z->pd_age);
            }
        }
        /* Why a deck is or is not reflecting: calls>0 = the scaling hook fires,
         * scaled[d]>0 = deck d reflects, the other counters name the exit. */
        fprintf(f, "calls=%lu gateoff=%lu nocnt=%lu nodeck=%lu noid=%d\n", g_wp_calls,
                g_wp_gateoff, g_wp_nocnt, g_wp_nodeck, g_wave_noid);
        fprintf(f, "wpaint: calls=%ld memo=%d  (widget paint -> deck context)\n",
                __atomic_load_n(&g_wpaint_calls, __ATOMIC_RELAXED),
                __atomic_load_n(&g_wpmemo_n, __ATOMIC_RELAXED));
        fprintf(f, "wwbctor: calls=%d ctmap=%d  (Content -> deck table built at construction)\n",
                __atomic_load_n(&g_wwbctor_calls, __ATOMIC_RELAXED),
                __atomic_load_n(&g_ctmap_n, __ATOMIC_RELAXED));
        fprintf(f,
                "upd: calls=%ld bound=%d wp_updhit=%lu wp_cthit=%lu  (deck via the live "
                "DetailedWaveformWidget::update hook / the Content table)\n",
                __atomic_load_n(&g_upd_calls, __ATOMIC_RELAXED),
                __atomic_load_n(&g_upd_bound, __ATOMIC_RELAXED), g_wp_updhit, g_wp_cthit);
        fprintf(f, "sw2d: paints=%ld last_deck=%d cur_deck=%d  (widget deck-index binding)\n",
                __atomic_load_n(&g_sw2d_paints, __ATOMIC_RELAXED),
                __atomic_load_n(&g_sw2d_last_deck, __ATOMIC_RELAXED),
                __atomic_load_n(&g_cur_paint_deck, __ATOMIC_RELAXED));
        for (int d = 0; d < AZMOD_STEM_DECKS; d++)
            fprintf(f, "  deck %d: scaled=%lu gated=%lu\n", d, g_wp_scaled[d], g_wp_gated[d]);
        for (int d = 0; d < AZMOD_STEM_DECKS; d++)
            fprintf(f, "deck %d: frames=%d att=%d\n", d, g_stem_frames ? g_stem_frames(d) : 0,
                    __atomic_load_n(&g_att_have[d], __ATOMIC_RELAXED));
        fclose(f);
    }
}

/* Per track position, the fraction of the four stems' peak still audible, each
 * stem at its pad toggle and its Stem Volume gain (wave_att.h). It scales the
 * native waveform height, so muting drums or turning their knob down flattens
 * the drum hits where they are. Broadband: one factor for all bands (per-band would need
 * 3-band envelopes from azstem). */
/* VOCAL / INST / BASS / DRUMS in azstem's order; same palette as the pad LEDs
 * and the overlay's stem strip (stem_overlay.c). */
static const unsigned STEM_RGB[AZMOD_STEM_COUNT] = AZMOD_STEM_PALETTE;
static int g_attzero = 0; /* azmod-attzero.on: diagnostic, see below */
static _Atomic int g_att_log = 40;
/* Returns 1 when the deck's curve changed (rebuilt, or dropped because the stems went away). */
static int build_atten(int deck, const int gq[AZMOD_STEM_COUNT]) {
    const unsigned char *env = 0;
    int n = 0, nst = 0;
    float pos = 0;
    if (!azmod_stem_wave || !azmod_stem_wave(deck, &env, &n, &nst, &pos) ||
        nst < AZMOD_STEM_COUNT || n <= 0) {
        /* The curve going away must repaint too, or a stemless track loaded
         * after a muted one keeps the old muted shape in its cached preview. */
        int had = __atomic_exchange_n(&g_att_have[deck], 0, __ATOMIC_ACQ_REL);
        if (!had)
            return 0; /* nothing to publish: the retry while azstem builds the envelope */
        for (int i = 0; i < ATT_N; i++)
            __atomic_store_n(&g_col[deck][i], 0u, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_att_gen[deck], 1, __ATOMIC_RELEASE);
        __atomic_store_n(&g_wave_dirty[deck], 1, __ATOMIC_RELEASE);
        return 1;
    }
    unsigned on = 0;
    for (int k = 0; k < AZMOD_STEM_COUNT; k++)
        if (g_state.stem_on[deck][k])
            on |= 1u << k;
    /* Height: the audible share of the stems' peak, each stem at its Stem Volume gain
     * (wave_att.h). Color: the loudest audible stem; a muted or silenced stem colors
     * nothing. Element stores are atomic: the message thread reads these curves while
     * they are rebuilt, and g_att_gen tells it to read them again. */
    for (int i = 0; i < ATT_N; i++) {
        int b = (int) ((long) i * n / ATT_N);
        unsigned v[AZMOD_STEM_COUNT];
        for (int k = 0; k < AZMOD_STEM_COUNT; k++)
            v[k] = env[(size_t) k * n + b];
        /* Diagnostic (azmod-attzero.on, a test aid): force the curve to zero
         * to prove the height writes reach the visible waveform. */
        unsigned short a = g_attzero ? 0 : watt_value(v, on, gq);
        __atomic_store_n(&g_att[deck][i], a, __ATOMIC_RELAXED);
        int ck = watt_color_stem(v, on, gq);
        __atomic_store_n(&g_col[deck][i], ck < 0 ? 0u : STEM_RGB[ck], __ATOMIC_RELAXED);
    }
    /* Diagnostic: mask, gains and sample curve values per rebuild. Budgeted;
     * overlay thread only. */
    if (__atomic_fetch_sub(&g_att_log, 1, __ATOMIC_RELAXED) > 0)
        wwb_log("atten: d%d mask=%x gq=%d/%d/%d/%d att[64]=%u att[512]=%u att[900]=%u", deck, on,
                gq[0], gq[1], gq[2], gq[3], __atomic_load_n(&g_att[deck][64], __ATOMIC_RELAXED),
                __atomic_load_n(&g_att[deck][512], __ATOMIC_RELAXED),
                __atomic_load_n(&g_att[deck][900], __ATOMIC_RELAXED));
    __atomic_store_n(&g_att_ident[deck], !g_attzero && watt_identity(on, gq), __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_att_gen[deck], 1, __ATOMIC_RELEASE); /* scaled copies rebuild */
    __atomic_store_n(&g_att_have[deck], 1, __ATOMIC_RELEASE);
    /* The overview tiles are refreshed by the caller (ov_rebuild_attenuation), on their own
     * cadence: EP147 re-renders each tile. */
    /* ...and the main waveform, which otherwise shows new heights only when
     * EP147 re-renders on its own schedule (seconds later).
     * wave_repaint_if_needed() handles it on the message thread. */
    __atomic_store_n(&g_wave_dirty[deck], 1, __ATOMIC_RELEASE);
    return 1;
}

/* Re-run EP147's own "overview changed" path (setOverview with the object it
 * already holds) so the bar re-renders. Message thread only. */
static void ov_repaint(long self) {
    long obj = *(volatile long *) (self + 0x80);
    if (!obj)
        return;
    long slot = obj;
    int mode = *(volatile unsigned char *) (self + 0x60);
    int width = *(volatile int *) (self + 0x64);
    ((ovset_fn) WAVE_OV_SET)(self, &slot, mode, width);
}

/* The overview is scaled in a copy, never in EP147's array: that array is per
 * track and shared by the overview widgets of every layout, so writing into it
 * and restoring later poisons other widgets' snapshots until EP147 restarts.
 *
 * Same shape as the main waveform (azm_rw_enter): ov_hook wraps the only render
 * of this widget (0x1dfb328, called only from setOverview 0x1dfc710), points
 * content+0x28 at the scaled copy for that call and puts EP147's pointer back.
 * The copy rebuilds when the array, count, curve generation, tint or the
 * array's bytes change (EP147 re-analyzing in place). Unknown deck -> stock.
 * Message thread only (ov_hook, build_atten). */
static unsigned char *ov_scaled_for(int i, const unsigned char *arr, int cnt) {
    int deck = g_ovi[i].deck;
    if (deck < 0 || deck >= AZMOD_STEM_DECKS || !arr || cnt <= 0 || cnt > OV_MAX)
        return 0;
    if (!__atomic_load_n(&g_wavemute, __ATOMIC_RELAXED))
        return 0;
    if (!__atomic_load_n(&g_att_have[deck], __ATOMIC_ACQUIRE))
        return 0;
    int color = __atomic_load_n(&g_wavecolor, __ATOMIC_RELAXED);
    if (__atomic_load_n(&g_att_ident[deck], __ATOMIC_RELAXED) && !color)
        return 0; /* all stems on: stock */
    if (!g_ovi[i].snap) {
        unsigned char *a = malloc(OV_MAX * OV_STRIDE), *b = malloc(OV_MAX * OV_STRIDE);
        if (!a || !b) {
            free(a);
            free(b);
            return 0;
        }
        g_ovi[i].snap = a;
        g_ovi[i].scaled = b;
        g_ovi[i].src = 0;
    }
    size_t len = (size_t) cnt * OV_STRIDE;
    unsigned gen = __atomic_load_n(&g_att_gen[deck], __ATOMIC_ACQUIRE);
    if (g_ovi[i].src != arr || g_ovi[i].scnt != cnt || g_ovi[i].gen != gen ||
        g_ovi[i].color != color || memcmp(g_ovi[i].snap, arr, len)) {
        memcpy(g_ovi[i].snap, arr, len);
        for (int k = 0; k < cnt; k++) {
            long b = (long) k * ATT_N / cnt;
            unsigned a = __atomic_load_n(&g_att[deck][b], __ATOMIC_RELAXED);
            const unsigned char *p = g_ovi[i].snap + (size_t) k * OV_STRIDE;
            unsigned char *e = g_ovi[i].scaled + (size_t) k * OV_STRIDE;
            memcpy(e, p, OV_STRIDE);
            e[OV_HGT_A] = (unsigned char) ((p[OV_HGT_A] * a) >> 8);
            e[OV_HGT_B] = (unsigned char) ((p[OV_HGT_B] * a) >> 8);
            unsigned c = color ? __atomic_load_n(&g_col[deck][b], __ATOMIC_RELAXED) : 0;
            if (c) {
                e[OV_COL_A + 0] = (unsigned char) (c >> 16);
                e[OV_COL_A + 1] = (unsigned char) (c >> 8);
                e[OV_COL_A + 2] = (unsigned char) c;
                e[OV_COL_B + 0] = (unsigned char) (c >> 16);
                e[OV_COL_B + 1] = (unsigned char) (c >> 8);
                e[OV_COL_B + 2] = (unsigned char) c;
            }
        }
        g_ovi[i].src = arr;
        g_ovi[i].scnt = cnt;
        g_ovi[i].color = color;
        g_ovi[i].gen = (__atomic_load_n(&g_att_gen[deck], __ATOMIC_ACQUIRE) == gen) ? gen : gen - 1;
    }
    return g_ovi[i].scaled;
}

/* Overview widgets die with the deck view, so captured pointers would dangle.
 * Forget them on every visibility change; ov_hook re-registers on the next
 * draw. Copy buffers are kept and reused by slot. */
static void ov_forget_all(void) {
    int n = __atomic_load_n(&g_ovi_n, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        g_ovi[i].self = 0;
        g_ovi[i].content = 0;
        g_ovi[i].deck = -1;
        g_ovi[i].cnt = 0;
        g_ovi[i].src = 0;
    }
    __atomic_store_n(&g_ovi_n, 0, __ATOMIC_RELEASE);
}

/* ---- Overview -> deck, by constructor identity ----------------------------
 *   gui::player_info_waveform::WaveformWidget  (the deck TILE, vtable 0x02ba0488)
 *     ctor FUN_01c192a0(tile, ..) embeds the preview widget:
 *        FUN_01dfcc70(param_1 + 0x50, ..)   -> tile + 0x280
 *        = gui::OverviewWaveform1200PointWidget (vtable 0x02b9eb88), whose draw
 *          FUN_01dfb328 is what ov_hook wraps.
 *   The tile's owner FUN_01c1fad0(.., .., param_3, .., param_5) builds it:
 *        lVar10 = *param_5;                       // the per-deck object
 *        FUN_01c192a0(tile, .., lVar10+0x1050, lVar10+0x1000, lVar10+0x1010)
 *   with param_3 = the player number (1-based byte).
 * The owner calls the tile ctor synchronously, so the owner hook parks the deck
 * and the tile ctor hook records (tile + 0x280) -> deck. ~4 calls per process.
 * Content matching cannot work: every overview instance reads the same object
 * at self+0x80. */
#define OV_WIDGET_VT EP_2b9eb88  /* gui::OverviewWaveform1200PointWidget */
#define OV_TILE_CTOR EP_1c192a0  /* player_info_waveform::WaveformWidget ctor */
#define OV_TILE_OWNER EP_1c1fad0 /* builds the tile; param_3 = deck */
#define OV_IN_TILE 0x280L        /* preview widget's offset inside the tile */
static struct {
    unsigned long self;
    int deck;
} g_ovtile[24]; /* several tile sets exist (2-deck / 4-deck layouts) */
/* The same overview widgets as wref slots (ep_mem.h; OverviewWaveform1200PointWidget has its
 * juce::Component at 0): g_ovtile keeps the identity for lookups, these say whether the widget
 * still exists. Registered by disp_wref_init. */
static _Atomic uintptr_t g_ovtile_w[24];
/* Every cached display widget range, registered once (ep_mem.h wref_track). All of these
 * classes have juce::Component at offset 0 (RTTI: PadStatusWidget, PadModeStatusWidget,
 * PadSlipLoopWidget, ImagesComponent, PadCueWidget, DeckInfo4DeckWidget, XPadParamWidget,
 * XPadWidget, BeatFxParamWidget, OverviewWaveform1200PointWidget), so comp_off is 0. Called
 * lazily from each capture; without the destructor hook nothing registers, wref_set refuses,
 * and every one of these features finds no widget (it does nothing). */
static void disp_wref_init(void) {
    static _Atomic int done;
    if (__atomic_load_n(&done, __ATOMIC_ACQUIRE))
        return;
    int ok = wref_track(WSLOT(g_tile_w), AZMOD_STEM_DECKS * 8, 0);
    ok &= wref_track(WSLOT(g_slot7_w), SLOT7_MAX, 0);
    ok &= wref_track(WSLOT(g_slip_tile), AZMOD_STEM_DECKS * 8, 0);
    ok &= wref_track(WSLOT(g_title_grid), AZMOD_STEM_DECKS, 0);
    ok &= wref_track(WSLOT(g_stem_container), AZMOD_STEM_DECKS, 0);
    ok &= wref_track(WSLOT(g_s4_parent), AZMOD_STEM_DECKS, 0);
    ok &= wref_track(&g_fxrow_owner, 1, 0);
    ok &= wref_track(&g_fxparam_w, 1, 0);
    ok &= wref_track(&g_fxh_parent, 1, 0);
    ok &= wref_track(&g_wwb_comp[0][0], 8 * 3, 0); /* the WWBs' color-mode widgets */
    /* OverviewWaveform1200PointWidget inherits juce::Component virtually: the Component the
     * destructor receives sits at the vbase offset stored at vtable-0x18 (0x170 on 1.30 and
     * 2.00), read from EP147's image here. No plausible offset: the range is not registered
     * and the overview repaint never caches a widget (unknown). */
    {
        uintptr_t vbo = 0;
        if (img_q((uintptr_t) OV_WIDGET_VT - 0x18, &vbo) && vbo > 0 && vbo < 0x10000)
            ok &= wref_track(g_ovtile_w, 24, (int) vbo);
    }
    if (ok)
        __atomic_store_n(&done, 1, __ATOMIC_RELEASE); /* else retried at the next capture */
}
static _Atomic int g_ovtile_n = 0;
static _Atomic int g_tilector_deck = -1; /* deck of the tile being built now */
static void (*g_orig_tilector)(long, long, long, long, long);
static void (*g_orig_tileowner)(long, long, long, long, long);
static void tilector_hook(long self, long a2, long a3, long a4, long a5) {
    int d = __atomic_load_n(&g_tilector_deck, __ATOMIC_RELAXED);
    if (d >= 0 && d < AZMOD_STEM_DECKS) {
        int n = __atomic_load_n(&g_ovtile_n, __ATOMIC_RELAXED);
        if (n < (int) (sizeof g_ovtile / sizeof g_ovtile[0])) {
            g_ovtile[n].self = (unsigned long) (self + OV_IN_TILE);
            g_ovtile[n].deck = d;
            disp_wref_init();
            wref_set(&g_ovtile_w[n], (uintptr_t) (self + OV_IN_TILE));
            __atomic_store_n(&g_ovtile_n, n + 1, __ATOMIC_RELAXED);
            wwb_log("ovtile: tile=%lx overview=%lx deck=%d", (unsigned long) self,
                    (unsigned long) (self + OV_IN_TILE), d);
        }
    }
    g_orig_tilector(self, a2, a3, a4, a5);
}
static _Atomic int g_tileraw_log = 10;
static void tileowner_hook(long a1, long a2, long deck, long a4, long a5) {
    int prev = __atomic_load_n(&g_tilector_deck, __ATOMIC_RELAXED);
    int raw = (int) (deck & 0xff);
    /* param_3 is a 1-based player number, not a deck index. */
    int d = (raw >= 1 && raw <= AZMOD_STEM_DECKS) ? raw - 1 : -1;
    /* File the per-deck block for the deck census (deck_for_perdeck_field): this hook has
     * the deck and the block together, inside EP147's call (*param_5 is live here).
     * perdeck_capture keeps it only as block d of a PlayerInfoState it has pinned. */
    uintptr_t perdeck = 0;
    if (a5)
        own_q((uintptr_t) a5, &perdeck);
    int filed = d >= 0 && perdeck ? perdeck_capture((long) perdeck, d) : 0;
    if (__atomic_fetch_sub(&g_tileraw_log, 1, __ATOMIC_RELAXED) > 0) {
        /* *param_5 is the per-deck object (same +0x1000/+0x1010 offsets as
         * FUN_01b8f950's). Logged only. */
        wwb_log("tileowner: raw param_3=%d -> deck=%d perdeck_obj=%lx (+0x1000=%lx) filed=%d", raw,
                d, (unsigned long) perdeck, (unsigned long) (perdeck ? perdeck + 0x1000 : 0), filed);
    }
    __atomic_store_n(&g_tilector_deck, d, __ATOMIC_RELAXED);
    g_orig_tileowner(a1, a2, deck, a4, a5);
    __atomic_store_n(&g_tilector_deck, prev, __ATOMIC_RELAXED);
}
/* Pure table lookup on the overview `self` ov_hook already has. No syscalls. */
static int ov_deck_via_tile(long self) {
    int n = __atomic_load_n(&g_ovtile_n, __ATOMIC_RELAXED);
    for (int i = 0; i < n; i++)
        if (g_ovtile[i].self == (unsigned long) self)
            return g_ovtile[i].deck;
    return -1;
}
static _Atomic int g_ovprobe = 12;
static void ov_hook(long self) {
    __atomic_fetch_add(&g_ov_calls, 1, __ATOMIC_RELAXED);
    long obj = *(volatile long *) (self + 0x80);
    /* Budgeted log of the first few overview draws and their tile binding. */
    if (__atomic_fetch_sub(&g_ovprobe, 1, __ATOMIC_RELAXED) > 0) {
        wwb_log("ovprobe: self=%lx obj=%lx tile=%lx -> deck=%d (table n=%d)", (unsigned long) self,
                (unsigned long) obj, (unsigned long) (self - OV_IN_TILE), ov_deck_via_tile(self),
                __atomic_load_n(&g_ovtile_n, __ATOMIC_RELAXED));
    }
    long content = obj ? *(volatile long *) (obj + 0x28) : 0;
    unsigned char *arr = content ? *(unsigned char **) (content + 0x28) : 0;
    int cnt = content ? *(volatile int *) (content + 0x30) : 0;
    int n = __atomic_load_n(&g_ovi_n, __ATOMIC_ACQUIRE), slot = -1;
    for (int i = 0; i < n; i++)
        if (g_ovi[i].self == self) {
            slot = i;
            break;
        }
    if (slot < 0 && n < OVINST_MAX) {
        slot = n;
        g_ovi[n].self = self;
        g_ovi[n].content = 0;
        g_ovi[n].deck = -1;
        __atomic_store_n(&g_ovi_n, n + 1, __ATOMIC_RELEASE);
    }
    if (slot < 0 || !content) {
        g_orig_ov(self);
        return;
    }
    g_ovi[slot].content = content;
    g_ovi[slot].cnt = cnt;
    /* Deck by identity (tile ctor census) only; unknown means stock. */
    g_ovi[slot].deck = ov_deck_via_tile(self);
    unsigned char *sc = ov_scaled_for(slot, arr, cnt);
    if (!sc) {
        g_orig_ov(self);
        return;
    }
    unsigned char **ap = (unsigned char **) (content + 0x28);
    *ap = sc; /* for this render only */
    __atomic_fetch_add(&g_ov_applied, 1, __ATOMIC_RELAXED);
    g_orig_ov(self);
    if (*ap == sc)
        *ap = arr; /* EP147 replaced it mid-render: keep theirs */
}
/* Find (or create) the g_ovi slot for a census overview widget, keyed by self. */
static int ov_slot_for(long self, int deck) {
    int n = __atomic_load_n(&g_ovi_n, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++)
        if (g_ovi[i].self == self) {
            if (g_ovi[i].deck < 0)
                g_ovi[i].deck = deck;
            return i;
        }
    if (n >= OVINST_MAX)
        return -1;
    g_ovi[n].self = self;
    g_ovi[n].deck = deck;
    g_ovi[n].content = 0;
    g_ovi[n].cnt = 0;
    g_ovi[n].src = 0;
    __atomic_store_n(&g_ovi_n, n + 1, __ATOMIC_RELEASE);
    return n;
}
/* Repaint a deck's waveform widgets after its attenuation curve changed.
 * JUCE message thread only (called from wavepaint_hook). Each color-mode
 * widget's juce::Component base is at WWB + mode offset + 0x86A0. All three are
 * repainted because the mode can change unannounced and a hidden repaint is
 * cheap. Runs only on the tick after a stem toggle. */
#define JUCE_COMPONENT_REPAINT EP_2488570 /* juce::Component::repaint(this) */
static void wave_repaint_if_needed(void) {
    int nb = __atomic_load_n(&g_wwbbase_n, __ATOMIC_RELAXED);
    if (nb <= 0 || !on_msg_thread())
        return;
    for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
        if (!__atomic_exchange_n(&g_wave_dirty[d], 0, __ATOMIC_ACQ_REL))
            continue;
        for (int i = 0; i < nb && i < 8; i++) {
            if (g_wwbbase[i].deck != d)
                continue;
            for (int m = 0; m < 3; m++) {
                uintptr_t w = wwb_widget_live(i, m); /* a destroyed WWB's widgets: skipped */
                if (w)
                    ((void (*)(long)) JUCE_COMPONENT_REPAINT)((long) (w + WIDGET_COMPONENT_OFF));
            }
        }
    }
}
/* Called from rtick_hook (message thread) after a toggle. Driven by the tile
 * census, not by which widgets drew last: an overview renders once per track
 * load into a cached image, so earlier-loaded decks never draw again. Content
 * pointers are re-read live, so track changes are picked up too. */
static void ov_refresh_if_needed(void) {
    int decks = __atomic_exchange_n(&g_ov_refresh, 0, __ATOMIC_ACQ_REL);
    if (!decks)
        return;
    int nt = __atomic_load_n(&g_ovtile_n, __ATOMIC_RELAXED);
    for (int t = 0; t < nt; t++) {
        /* Tile widgets are torn down on a view change; the widget's destructor clears its
         * wref slot, so a non-zero slot here (message thread) is a live widget. */
        long self = (long) wref_get(&g_ovtile_w[t]);
        int deck = g_ovtile[t].deck;
        if (!self || deck < 0 || deck >= AZMOD_STEM_DECKS || !(decks & (1 << deck)))
            continue;
        {
            unsigned vt = 0;
            if (!own_u32((uintptr_t) self, &vt) || vt != (unsigned) OV_WIDGET_VT)
                continue;
        }
        long obj = *(volatile long *) (self + 0x80);
        long content = obj ? *(volatile long *) (obj + 0x28) : 0;
        unsigned char *arr = content ? *(unsigned char **) (content + 0x28) : 0;
        int cnt = content ? *(volatile int *) (content + 0x30) : 0;
        if (!arr || cnt <= 0 || cnt > OV_MAX)
            continue;
        if (ov_slot_for(self, deck) < 0)
            continue;
        ov_repaint(self); /* re-renders through ov_hook */
    }
}
