/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file mod_menu_overlay.c
 * @brief Mod Menu overlay thread: X11 windows, painting, input, Sys Info readouts and screen detection
 *
 * overlay_thread() is the single thread that owns g_state and the overlay's X windows. Its
 * state lives in one struct overlay_ctx. At startup it records the hook table and starts the
 * runtime parts of the mods (ov_start_mods), opens the display and creates the windows
 * (ov_open_display, ov_create_windows): the menu window, deck 2's stem strip, the source-row
 * icon, the OVERLAYS readouts and one stem-lane band per deck, all override-redirect, the
 * last three input-transparent. Then it loads the saved state (ov_load_state).
 *
 * Each 10 to 30 ms tick runs the same phases in order:
 *   ov_reload_banner   while the USB loader restarts EP147, keep its banner up and skip
 *   ov_poll_inputs     our windows' X events, then what the bus and EP147's threads handed over
 *   ov_update_state    publish settings to the bus threads and azstem, follow EP147's screens
 *                      and decks, run the menu's timers (ov_slow_tick every 5th tick)
 *   ov_place_windows   size, place, map and raise each window (ov_window_place/_show)
 *   ov_present         when dirty: recomposite with azmod_composite() and XPutImage each part
 *
 * Screen detection: SOURCE by correlating the title region against a reference bitmap
 * (title_match(), detect_source()), the deck screen from EP147's own paint hooks
 * (detect_waveform()), and the 4-deck layout from the tab highlight (detect_4deck()). The
 * CPU and RAM readers feed the OVERLAYS readouts; read_sysinfo() fills the Sys Info rows
 * (uptime, memory, version, address, SSH listener). handle_action() applies the last menu
 * action on this thread: reload request, Sys Info and Update Mods refresh, config save, and the
 * stems and playlist-editing flag updates.
 *
 * Entry points: overlay_thread(), handle_action(), read_sysinfo(), srcrow_activate_item(),
 * apply_menu_shape().
 */
/* SoC temperature in tenths of a degree C, or -1. The soc-thermal zone is located once (retried
 * every 30 s until found); the emulator has none, so /tmp/azmod-fake-temp stands in. */
static int cpu_temp_read(void) {
    static char path[64];
    static long next_scan = 0;
    if (!path[0] && now_ms() >= next_scan) {
        next_scan = now_ms() + 30000;
        for (int z = 0; z < 16 && !path[0]; z++) {
            char tp[64], ty[32] = "";
            snprintf(tp, sizeof tp, "/sys/class/thermal/thermal_zone%d/type", z);
            int fd = open(tp, O_RDONLY | O_CLOEXEC);
            if (fd < 0)
                continue;
            ssize_t n = read(fd, ty, sizeof ty - 1);
            close(fd);
            if (n > 0 && !strncmp(ty, "soc-thermal", 11))
                snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", z);
        }
    }
    const char *p = path[0] ? path : "/tmp/azmod-fake-temp";
    char b[16] = "";
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0)
        return -1;
    long mc = strtol(b, NULL, 10);
    if (mc <= 0 || mc > 200000)
        return -1; /* not a plausible reading */
    return (int) ((mc + 50) / 100);
}
/* CPU use across all cores: busy share of the jiffies since the last call, from /proc/stat's
 * aggregate line (idle + iowait count as not busy). The first call only primes the baseline. */
static int cpu_use_read(void) {
    static unsigned long long pb = 0, pt = 0;
    char b[256] = "";
    int fd = open("/proc/stat", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0)
        return -1;
    unsigned long long v[8] = {0};
    if (sscanf(b, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4],
               &v[5], &v[6], &v[7]) < 5)
        return -1;
    unsigned long long tot = 0;
    for (int i = 0; i < 8; i++)
        tot += v[i];
    unsigned long long busy = tot - v[3] - v[4];
    int r = -1;
    if (pt && tot > pt)
        r = (int) ((busy - pb) * 100 / (tot - pt));
    pb = busy;
    pt = tot;
    return r < 0 ? -1 : (r > 100 ? 100 : r);
}
/* The fastest core's clock in MHz, and whether it runs below its own maximum. cpuinfo_max_freq
 * never changes, so it is read once per core. The emulator has no cpufreq;
 * /tmp/azmod-fake-cpufreq ("cur max", kHz) stands in. */
static int cpu_clk_read(bool *throttled) {
    static long maxk[8];
    static int probed = 0;
    if (!probed) {
        probed = 1;
        for (int c = 0; c < 8; c++) {
            char p[80], b[24] = "";
            snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
            int fd = open(p, O_RDONLY | O_CLOEXEC);
            if (fd < 0)
                continue;
            if (read(fd, b, sizeof b - 1) > 0)
                maxk[c] = strtol(b, NULL, 10);
            close(fd);
        }
    }
    long best = -1, bestmax = 0;
    for (int c = 0; c < 8; c++) {
        if (!maxk[c])
            continue;
        char p[80], b[24] = "";
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", c);
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        ssize_t n = read(fd, b, sizeof b - 1);
        close(fd);
        if (n <= 0)
            continue;
        long cur = strtol(b, NULL, 10);
        if (cur > best) {
            best = cur;
            bestmax = maxk[c];
        }
    }
    if (best < 0) {
        char b[48] = "";
        long cur = 0, mx = 0;
        int fd = open("/tmp/azmod-fake-cpufreq", O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return -1;
        ssize_t n = read(fd, b, sizeof b - 1);
        close(fd);
        if (n <= 0 || sscanf(b, "%ld %ld", &cur, &mx) != 2 || cur <= 0)
            return -1;
        best = cur;
        bestmax = mx;
    }
    *throttled = bestmax > 0 && best < bestmax;
    return (int) (best / 1000);
}
/* RAM in use as a percentage: (MemTotal - MemAvailable) / MemTotal. MemAvailable is the
 * kernel's own estimate of what it could hand out without swapping. */
static int ram_use_read(void) {
    char b[512] = "";
    int fd = open("/proc/meminfo", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0)
        return -1;
    char *t = strstr(b, "MemTotal:"), *a = strstr(b, "MemAvailable:");
    if (!t || !a)
        return -1;
    long tot = strtol(t + 9, NULL, 10), av = strtol(a + 13, NULL, 10);
    if (tot <= 0 || av < 0 || av > tot)
        return -1;
    return (int) ((tot - av) * 100 / tot);
}
/* Fill the Sys Info rows (s->info_line[]) from /proc and the network interfaces. */
static void read_sysinfo(azmod_state *s) {
    FILE *f = fopen("/proc/uptime", "re");
    double up = 0;
    if (f) {
        if (fscanf(f, "%lf", &up) != 1)
            up = 0;
        fclose(f);
    }
    int hh = (int) up / 3600, mm = ((int) up % 3600) / 60, ss = (int) up % 60;
    snprintf(s->info_line[0], AZMOD_INFO_LINE_LEN, "Uptime %02d:%02d:%02d", hh, mm, ss);

    long total = 0, free_ = 0;
    FILE *mf = fopen("/proc/meminfo", "re");
    if (mf) {
        char line[128];
        while (fgets(line, sizeof line, mf)) {
            long v;
            if (sscanf(line, "MemTotal: %ld kB", &v) == 1)
                total = v;
            else if (sscanf(line, "MemFree: %ld kB", &v) == 1)
                free_ = v;
        }
        fclose(mf);
    }
    snprintf(s->info_line[1], AZMOD_INFO_LINE_LEN, "Mem Free %ld MB", free_ / 1024);
    snprintf(s->info_line[2], AZMOD_INFO_LINE_LEN, "Mem Total %ld MB", total / 1024);
    {
        const char *lv = azmod_live_version();
        if (lv[0])
            snprintf(s->info_line[3], AZMOD_INFO_LINE_LEN, "AZ-MODS v%s", lv);
        else
            snprintf(s->info_line[3], AZMOD_INFO_LINE_LEN, "AZ-MODS (version unknown)");
    }
    /* The deck's own address, so a user can reach it over SSH without a router page: the WiFi
     * interface (mlan0) first, else the first non-loopback IPv4 (the emulator has eth0). Then
     * whether anything listens on port 22, from /proc/net/tcp (local port 0016, state 0A). */
    char ip[24] = "";
    struct ifaddrs *ifa0 = NULL;
    if (getifaddrs(&ifa0) == 0) {
        for (int pass = 0; pass < 2 && !ip[0]; pass++)
            for (struct ifaddrs *a = ifa0; a; a = a->ifa_next) {
                if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !a->ifa_name)
                    continue;
                if (!strcmp(a->ifa_name, "lo"))
                    continue;
                if (pass == 0 && strcmp(a->ifa_name, "mlan0"))
                    continue;
                inet_ntop(AF_INET, &((struct sockaddr_in *) a->ifa_addr)->sin_addr, ip, sizeof ip);
                break;
            }
        freeifaddrs(ifa0);
    }
    int ssh = 0;
    FILE *tf = fopen("/proc/net/tcp", "re");
    if (tf) {
        char line[256];
        unsigned lp, st;
        while (fgets(line, sizeof line, tf))
            if (sscanf(line, " %*d: %*8x:%x %*8x:%*x %x", &lp, &st) == 2 && lp == 22 &&
                st == 0x0A) {
                ssh = 1;
                break;
            }
        fclose(tf);
    }
    snprintf(s->info_line[4], AZMOD_INFO_LINE_LEN, "IP %s  SSH %s", ip[0] ? ip : "none",
             ssh ? "on" : "off");
}
/* Does the screen's title region match a reference mask? Samples one pixel per grid cell and
 * scores the ink overlap against the mask (Jaccard). */
static int title_match(Display *dpy, Window root, const unsigned char *mask) {
    int W = g_fb.w > 0 ? g_fb.w : TITLE_REF_W, H = g_fb.h > 0 ? g_fb.h : TITLE_REF_H;
    int rx = SRCREF_X * W / TITLE_REF_W, ry = SRCREF_Y * H / TITLE_REF_H;
    int rw = SRCREF_CW * W / TITLE_REF_W, rh = SRCREF_CH * H / TITLE_REF_H;
    if (rw < SRCREF_W)
        rw = SRCREF_W; /* need >=1px per grid cell */
    if (rh < SRCREF_H)
        rh = SRCREF_H;
    XImage *im = XGetImage(dpy, root, rx, ry, rw, rh, AllPlanes, ZPixmap);
    if (!im)
        return 0;
    int inter = 0, uni = 0, cw = rw / SRCREF_W, ch = rh / SRCREF_H;
    for (int gy = 0; gy < SRCREF_H; gy++)
        for (int gx = 0; gx < SRCREF_W; gx++) {
            unsigned long p = XGetPixel(im, gx * cw + cw / 2, gy * ch + ch / 2);
            int r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
            /* Ink is white title text, except in Day Mode, where EP147 paints the title dark on
             * light. Without the flip the SOURCE screen reads as gone and the Mod Menu closes. */
            int lum = (r + g + b) / 3;
            int live = __atomic_load_n(&G_DM_FLIP, __ATOMIC_RELAXED) ? lum < 115 : lum > 140;
            int ref = mask[gy * SRCREF_W + gx];
            if (live || ref)
                uni++;
            if (live && ref)
                inter++;
        }
    XDestroyImage(im);
    return uni > 0 && (inter * 100 / uni) >= 45; /* Jaccard overlap >= 45% */
}
static int detect_source(Display *dpy, Window root) {
    if (getenv("AZMOD_FORCE_SOURCE"))
        return 1; /* test override */
    /* The opposite test override: forces "not SOURCE" so the menu visibility gate's close
     * branch can be exercised (every injectable navigation closes the menu through its event
     * path first). A /tmp flag, so it needs no EP147 restart and is gone after a reboot. */
    if (AZ_PROBE_ON("/tmp/azmod-force-nosource"))
        return 0;
    return title_match(dpy, root, SRCREF);
}
/* "Is the deck (WAVEFORM) screen up?", answered by EP147 itself rather than a screen scrape.
 * WaveformView::paint and the deck screen's own pad-mode reads (modeget_from_deck_screen) only
 * run while that view is up, so the hooks timestamp them (wave_mark) and "seen recently" means
 * visible. The stem strip shows only on this screen. */
static _Atomic long g_wave_seen_ms = 0; /* last WaveformView::paint / mode dispatch */
static int g_wave_hooked = 0;
static _Atomic int g_wave_vis = -1; /* its JUCE visibility: -1 unknown, 0/1 from setVisible */
static void wave_mark(void) {
    __atomic_store_n(&g_wave_seen_ms, now_ms(), __ATOMIC_RELAXED);
}
/* Is the 4-deck layout selected? EP147's own mode once the view has painted (any theme).
 * Before that, the pixel fallback: the stock blue accent of the highlighted tab, centroid
 * on the "4 DECK" tab (a theme recolors that accent, hence the native read first). */
static int detect_4deck(Display *dpy, Window root) {
    int wm = __atomic_load_n(&g_wv_mode, __ATOMIC_RELAXED);
    if (wm >= 0)
        return wm == 2;
    int W = g_fb.w > 0 ? g_fb.w : 1280, H = g_fb.h > 0 ? g_fb.h : 800;
    int rx = TABROW_X * W / 1280, ry = TABROW_Y * H / 800;
    int rw = TABROW_W * W / 1280, rh = TABROW_H * H / 800;
    XImage *im = XGetImage(dpy, root, rx, ry, rw, rh, AllPlanes, ZPixmap);
    if (!im)
        return 0;
    long sx = 0;
    int n = 0;
    for (int y = 0; y < rh; y++)
        for (int x = 0; x < rw; x++) {
            unsigned long p = XGetPixel(im, x, y);
            int r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
            if (b > 120 && b > r + 40 && b > g + 30) {
                sx += x;
                n++;
            } /* blue accent */
        }
    XDestroyImage(im);
    if (n < 8)
        return 0; /* no highlight found: assume 2-band */
    int cx = rx + (int) (sx / n);
    return cx > (TAB4_X - 24) * W / 1280; /* highlight sits on the "4 DECK" tab */
}
static int detect_waveform(Display *dpy, Window root) {
    if (getenv("AZMOD_FORCE_WAVEFORM"))
        return 1;
    /* "Painted recently" is the authority: the paint and mode-dispatch hooks stop the moment
     * the deck view goes away. An explicit setVisible(false) hides us one frame sooner, but a
     * setVisible(true) cannot keep us visible, because EP147 leaves the view for Browse or
     * Playlist without calling setVisible on this object. */
    if (g_wave_hooked) {
        if (__atomic_load_n(&g_wave_vis, __ATOMIC_RELAXED) == 0)
            return 0;
        return (now_ms() - __atomic_load_n(&g_wave_seen_ms, __ATOMIC_RELAXED)) < WAVE_SEEN_MS;
    }
    return title_match(dpy, root, WAVREF); /* hooks not installed: fall back to the title mask */
}
/* Shape the menu window so its right edge fades to clear: solid up to the content width, then
 * a Bayer-dithered block pattern. Built once and cached; X-Shape is 1-bit, so dithering is the
 * only way to fade. */
static void apply_menu_shape(Display *dpy, int scr, Window win, const azmod_fb *fb) {
    (void) scr;
    static Pixmap cached = 0;
    if (!cached) {
        int cw, ww;
        azmod_menu_fade_bounds(fb, &cw, &ww);
        int h = fb->h;
        if (ww <= 0 || h <= 0)
            return;
        cached = XCreatePixmap(dpy, win, ww, h, 1); /* 1 = present, 0 = hole */
        GC mgc = XCreateGC(dpy, cached, 0, NULL);
        XSetForeground(dpy, mgc, 0);
        XFillRectangle(dpy, cached, mgc, 0, 0, ww, h); /* holes */
        XSetForeground(dpy, mgc, 1);
        XFillRectangle(dpy, cached, mgc, 0, 0, cw, h); /* solid content */
        /* clang-format off */
        static const unsigned char bayer8[8][8] = {
            {0, 48, 12, 60, 3, 51, 15, 63}, {32, 16, 44, 28, 35, 19, 47, 31},
            {8, 56, 4, 52, 11, 59, 7, 55},  {40, 24, 36, 20, 43, 27, 39, 23},
            {2, 50, 14, 62, 1, 49, 13, 61}, {34, 18, 46, 30, 33, 17, 45, 29},
            {10, 58, 6, 54, 9, 57, 5, 53},  {42, 26, 38, 22, 41, 25, 37, 21}};
        /* clang-format on */
        int fade = ww - cw;
        if (fade < 1)
            fade = 1;
        for (int by = 0; by < h; by += SHAPE_BLK)
            for (int bx = cw; bx < ww; bx += SHAPE_BLK) {
                int alpha = ((bx - cw) * 64) / fade; /* 0 opaque .. 64 clear */
                if (bayer8[(by / SHAPE_BLK) & 7][((bx - cw) / SHAPE_BLK) & 7] >=
                    alpha) { /* present block */
                    int bw = (bx + SHAPE_BLK <= ww) ? SHAPE_BLK : (ww - bx);
                    int bh = (by + SHAPE_BLK <= h) ? SHAPE_BLK : (h - by);
                    XFillRectangle(dpy, cached, mgc, bx, by, bw, bh);
                }
            }
        XFreeGC(dpy, mgc);
    }
    XShapeCombineMask(dpy, win, ShapeBounding, 0, 0, cached, ShapeSet);
}
/* Geometry of the primary overlay window for the current mode. d1_strip_visible already folds in
 * the STEMS master and Deck 1's stems availability (2 = native tiles, mini-waveform row only). */
static void wanted_rect(const azmod_fb *fb, azmod_mode mode, int source_visible,
                        int d1_strip_visible, int *x, int *y, int *w, int *h) {
    (void) source_visible;
    if (mode == AZ_CLOSED) {
        if (g_state.nm_visible) { /* NEW PLAYLIST: the title-row bar */
            int bar[4], ca[4], cr[4];
            azmod_naming_rects(fb, bar, ca, cr);
            *x = bar[0];
            *y = bar[1];
            *w = bar[2];
            *h = bar[3];
        } else if (g_state.tb_visible) { /* our held banner: UNDO and/or DONE */
            if (g_state.tb_undo) {
                int dx, dy, dw, dh;
                azmod_tb_undo_rect(fb, x, y, w, h);
                if (g_state.tb_done_btn) {
                    azmod_tb_done_rect(fb, &dx, &dy, &dw, &dh);
                    *w = dx + dw - *x;
                }
            } else
                azmod_tb_done_rect(fb, x, y, w, h);
        } else if (d1_strip_visible == 2)
            azmod_stem_lanes_win_rect(fb, 0, x, y, w,
                                      h); /* native tiles: row 2 only (stem mini-waveforms) */
        else if (d1_strip_visible)
            azmod_stem_win_rect(fb, 0, x, y, w, h); /* Deck 1 title + strip on WAVEFORM */
        /* No overlay MOD MENU button: the native SOURCE-list row (srcrow hooks) is the only
         * entry point. */
        else {
            *x = -16;
            *y = -16;
            *w = 1;
            *h = 1;
        } /* else park window offscreen */
    }
    /* Open: focused means the full-screen menu (list left, settings right); otherwise the menu
     * docks on the SOURCE screen's right pane (opening from the native row has already called
     * azmod_set_docked(true)). */
    else if (azmod_docked() && __atomic_load_n(&g_menu_focused, __ATOMIC_RELAXED))
        azmod_menu_full_rect(fb, x, y, w, h);
    else
        azmod_menu_win_rect(fb, x, y, w, h);
}
/* The overlay buffer's ground before a composite: black, or the theme's ground in Day Mode
 * (azm_theme). A plain memset would leave every pixel azmod does not draw black on the light
 * screen, e.g. stems row 2 before the stem envelope is ready. */
static void fb_ground(uint8_t *buf, int W, int H) {
    uint32_t g = azm_theme(0);
    if (!g) {
        memset(buf, 0, (size_t) W * H * 4);
        return;
    }
    uint32_t *p = (uint32_t *) buf;
    for (size_t k = 0, n = (size_t) W * H; k < n; k++)
        p[k] = g;
}
/* Paint the "RELOADING..." banner over EP147 on our own window, not the root (the root sits
 * under EP147's windows and would never be seen). Resizes win to full screen, drops the menu
 * fade shape so it is opaque, raises it, blits and XSyncs. No internal sleep: callers repaint
 * it repeatedly to keep it on top until EP147 goes down. */
static void show_reloading(Display *dpy, Window win, GC gc, XImage *img, uint8_t *buf, int W,
                           int H) {
    azmod_fb fb = {.px = buf, .w = W, .h = H, .stride = W * 4};
    fb_ground(buf, W, H); /* opaque background (black; white in Day Mode) */
    const char *msg = "RELOADING...";
    int px = H / 14; /* ~57px at 800 */
    if (!azmod_ttf_center || !azmod_ttf_center(&fb, 0, 0, W, H, msg, px, 2, 1, 0x00FFFFFFu)) {
        int sc = H / 90;
        if (sc < 2)
            sc = 2; /* bitmap-font fallback */
        int tw = (int) strlen(msg) * 6 * sc;
        azmod_draw_text_at(&fb, (W - tw) / 2, (H - 7 * sc) / 2, msg, sc, 0x00FFFFFFu);
    }
    XMoveResizeWindow(dpy, win, 0, 0, (unsigned) W, (unsigned) H); /* full screen */
    XShapeCombineMask(dpy, win, ShapeBounding, 0, 0, None,
                      ShapeSet); /* drop the fade shape: opaque */
    XMapRaised(dpy, win);
    XPutImage(dpy, win, gc, img, 0, 0, 0, 0, (unsigned) W, (unsigned) H);
    XRaiseWindow(dpy, win);
    XSync(dpy, False);
}
/* Activate a menu item chosen on the native SOURCE-list row (touch tap or knob click, handed
 * over via g_srcrow_act by the JUCE and interposer threads). Mirrors the activate branch of
 * azmod_input() in menu.c; running it here keeps every g_state write on the overlay thread. */
static void srcrow_activate_item(int j) {
    if (j < 0 || j >= AZI_COUNT)
        return;
    g_state.menu_sel = j;
    if (az_has_value(j)) { /* a mod toggle */
        if (!az_active(j))
            return;                    /* grayed sub-setting: inert */
        az_toggle(j); /* keeps mods 21/22 exclusive */
        g_state.config_dirty = true;   /* handle_action persists + applies */
        g_state.last_action = j;
        g_state.pressed_item = j;
        g_state.pressed_ticks = AZMOD_FLASH_TICKS;
    } else if (j == AZI_SYSINFO) {
        g_state.mode = AZ_INFO;
        g_state.last_action = j;
    } else if (j == AZI_COMMUNITY) {
        g_state.mode = AZ_COMMUNITY;
    } /* Community opens its own screen; it must never fall through to RELOAD */
    else if (j == AZI_UPDATE) {
        g_state.mode = AZ_UPDATE;
        g_state.last_action = j;
    } else { /* RELOAD */
        g_state.last_action = j;
        g_state.pressed_item = j;
        g_state.pressed_ticks = AZMOD_FLASH_TICKS;
    }
}
/* With azmod-logs.on: one line per touch event the menu window receives, recording what the
 * deck actually sent and what the menu made of it. */
static int g_touchsim; /* test replay of the deck's touch stream, see ButtonPress */
static void menu_touch_log(const char *what, const XEvent *ev) {
    if (!g_state.menu_focused && g_state.mode != AZ_UPDATE)
        return;
    FILE *f = azlog_open("/tmp/azmod-menutouch.log", "a");
    if (!f)
        return;
    int x = ev->type == MotionNotify ? ev->xmotion.x_root : ev->xbutton.x_root;
    int y = ev->type == MotionNotify ? ev->xmotion.y_root : ev->xbutton.y_root;
    /* age: ms from the server's timestamp to now (both CLOCK_MONOTONIC ms, the server's in 32
     * bits): how long the event waited before it was handled */
    Time et = ev->type == MotionNotify ? ev->xmotion.time : ev->xbutton.time;
    unsigned age = (unsigned) ((uint32_t) now_ms() - (uint32_t) et);
    fprintf(f, "%ld %s win=0x%lx send=%d btn=%u x=%d y=%d mode=%d age=%u -> %s\n", now_ms(), what,
            (unsigned long) ev->xany.window, (int) ev->xany.send_event,
            ev->type == MotionNotify ? 0u : ev->xbutton.button, x, y, (int) g_state.mode, age,
            azmod_menu_last_hit());
    fclose(f);
}
/* React to whatever azmod_input() just fired: request a reload, refresh Sys Info or Update Mods,
 * persist and apply toggles. Overlay thread only. */
static void handle_action(Display *dpy, Window root, int W, int H, int *pending_reload) {
    (void) dpy;
    (void) root;
    (void) W;
    (void) H;
    if (g_state.last_action == AZI_RELOAD) {
        azcm_reload_requested(); /* settings survive the reload; this start counts as healthy */
        *pending_reload = 1;
    }
    if (g_state.last_action == AZI_SYSINFO) {
        read_sysinfo(&g_state);
        read_savelogs(&g_state);
    }
    if (g_state.savelogs_want) /* System Info's Save Logs to USB button */
        savelogs_request(&g_state);
    if (g_state.last_action == AZI_UPDATE) {
        read_modupdate(&g_state);
        read_changelog(&g_state);
    }
    if (g_state.update_want) { /* the Update Mods screen's button */
        write_modupdate_req(g_state.update_want);
        g_state.update_want = 0;
        snprintf(g_state.update_line[4], AZMOD_UPDATE_LINE_LEN, "Please wait");
    }
    if (g_state.last_action == AZ_ACT_STEMS)
        write_stems(&g_state);
    if (g_state.config_dirty) { /* a mod toggle or a pad-color pick */
        save_config(&g_state);
        write_enable(&g_state);        /* STEMS mod = master enable */
        write_stems(&g_state);         /* engage/disengage both decks now */
        publish_mode_colors(&g_state); /* pad-mode picker -> LED interposer */
    }
}
/* Is EP147's grid-adjust view showing? Mirrored from its own show/hide call. */
static _Atomic int g_grid_adjust;
static void setgridadj_hook(void *view, int show) {
    g_orig_setgridadj(view, show);
    __atomic_store_n(&g_grid_adjust, (show & 0xff) != 0, __ATOMIC_RELAXED);
}
/* The Mod Menu's reload: `systemctl daemon-reload`, then `systemctl restart EP147.service`
 * only if daemon-reload exited 0. Its own thread, so the banner loop never waits. */
static void *az_reload_run(void *arg) {
    (void) arg;
    static char *const reload_argv[] = {"systemctl", "daemon-reload", NULL};
    static char *const restart_argv[] = {"systemctl", "restart", "EP147.service", NULL};
    int st = 0;
    pid_t p = azspawn(reload_argv, 0);
    if (p < 0) {
        dbg("reload: systemctl daemon-reload did not start (%s)\n", strerror(errno));
        return NULL;
    }
    while (waitpid(p, &st, 0) < 0) {
        if (errno != EINTR) {
            st = 0; /* ECHILD: SIGCHLD ignored, the status is gone; go on as the shell would not know either */
            break;
        }
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        dbg("reload: systemctl daemon-reload failed (status %d): no restart\n", st);
        return NULL;
    }
    p = azspawn(restart_argv, 0);
    if (p < 0) {
        dbg("reload: systemctl restart did not start (%s)\n", strerror(errno));
        return NULL;
    }
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) {
    }
    return NULL;
}
/* The bus threads' settings word (bus_cfg.h), packed from what the Mod Menu and the x-pad
 * config hold now. Overlay thread only; stored only when it changed. Called after each config
 * load and at the top of every overlay tick, so a toggle reaches the bus within one tick. */
static void bus_cfg_publish(void) {
    struct bus_xpad x = {g_xpad_pos_off, g_xpad_touch_off, (int) g_xpad_touch_msk, g_xpad_span,
                         g_xpad_zone_off};
    uint64_t c = bus_cfg_pack(az_on(AZF_PLAYLIST_EDITING),
                              az_on(AZF_STEM_PADS_PAGE),
                              __atomic_load_n(&g_reorder_hooks, __ATOMIC_RELAXED),
                              __atomic_load_n(&g_reorder_forced_off, __ATOMIC_RELAXED), &x);
    if (c != __atomic_load_n(&g_bus_cfg, __ATOMIC_RELAXED))
        __atomic_store_n(&g_bus_cfg, c, __ATOMIC_RELEASE);
}
/* Every hook that needs nothing from a running EP147, installed from azmod's constructor
 * (azmod_init), before EP147's main: no EP147 thread exists yet, so no patched function can be
 * executing while it is rewritten, and the hooks are in place before EP147 first calls them.
 * Installing these from the overlay thread raced EP147's start. Nothing here
 * may call into EP147 or wait: what needs a running EP147 (the hide-title juce::String, X, the
 * USB asset mods) stays on the overlay thread. The table of what each install did is written to
 * AZM_HOOKTABLE_PATH by the overlay thread. */
static void install_browse_fields(void); /* browse_fields.c */
static void install_static_hooks(void) {
    /* First: cached component pointers (wref) are cleared by this hook, so it must be in place
     * before EP147 builds the first component. Without it no component may be cached. */
    g_compdtor_ok = install_compdtor_hook() > 0;
    /* No LED serializer/getColor inline hooks: they froze EP147's real-time LED path. Pad LED
     * stem colors go through the ioctl override, driven by the stems-engaged state.
     * Production features default ON; each has an *.off opt-out for the emulator and debugging. */
    azmod_stem_native = (access("/home/root/settings/azmod-stemnative.off", F_OK) != 0);
    g_attzero = (AZ_PROBE_ON("/home/root/settings/azmod-attzero.on"));
    if (access("/home/root/settings/azmod-srcrow.off", F_OK) != 0)
        install_srcrow_hook(); /* the MOD MENU row: default ON */
    install_scrntrace_hook();  /* observe-only; silent without azmod-scrntrace.on */
    install_wavestems_hook();  /* always installed; forcing is driven by arm_stems (STEMS master) */
    install_jogstems();        /* stem state on the jog display (off: azmod-jogstems.off) */
    install_qrlogin_guard();   /* stock EP147 crash: null deref in the QR cloud login path */
    install_bpmstate_hook();   /* per-deck live BPM for the drum roll */
    install_master_hook();     /* master deck: Phase Meter and the drum roll; before either */
    install_link_members();    /* PRO DJ LINK players 3/4 own CH3/CH4: Stem Volume stands down */
    install_stemsmark();     /* stems mark in browse rows (off: azmod-stemsmark.off) */
    {
        int ph = install_preview_hotcues(); /* hot cue marks on the PREVIEW column; needs the above */
        FILE *pf = azlog_open("/tmp/azmod-stemsmark.log", "a");
        if (pf) {
            fprintf(pf, "install preview hot cues: %d\n", ph);
            fclose(pf);
        }
    }
    install_browse_fields(); /* Column Edit: fields on the columns; needs the above */
    install_hidetitle();     /* Hide Track Title (mod 35): inert while the mod is off */
    install_phasemeter();    /* Phase Meter (mod 36): data hooks; drawing is gated on the mod */
    if (daymode_wanted()) {
        install_daymode();
        install_scci_hook();
    }                        /* Themes: inert while off */
    install_mlreply_probe(); /* opt-in: library reply pages -> row identity (azmod-mlreply.log / the
                                mark) */
    install_reorder();        /* always in; inert while Playlist Editing is off */
    install_folderpreview();  /* a selected folder's preview fills (off: azmod-folderpreview.off) */
    install_remotestems();    /* always in; MENU > MAKE STEMS follows the Remote Stems row */
    install_rowcache();       /* buffered list rows (smooth touch scroll) */
}
/* One of the overlay's own X windows, with the geometry it was last given. x/y/w/h start at
 * -1 so the first placement always reaches the server. */
struct ov_window {
    Window id;
    int wanted; /* this tick: the window should be on screen */
    int mapped;
    int x, y, w, h;
};

/* Everything the overlay thread keeps from one tick to the next. One instance lives on the
 * overlay thread's stack for the life of the process; nothing else touches it. */
#include "touch_seq.h"
#include "ht_tap.h"
struct overlay_ctx {
    Display *dpy;
    int scr;
    Window root;
    int scr_w, scr_h;
    GC gc;
    uint8_t *buf; /* the full-screen software framebuffer behind g_fb */
    struct touch_seq touch; /* every button event on our windows, in order (overlay thread) */
    XImage *img;  /* wraps buf for XPutImage */
    XImage *shimg;          /* wraps buf when buf is a shared segment (ov_shm_create), else NULL */
    XShmSegmentInfo shm;
    int shm_busy;           /* a shared put may still be unread by the server (ov_shm_wait) */

    struct ov_window menu;     /* the Mod Menu, docked panel and stem strip (deck 1) */
    int menu_shaped;           /* the menu's fade-to-clear X-Shape mask is applied */
    struct ov_window strip2;   /* deck 2's stem strip */
    struct ov_window srcicon;  /* the AZ icon in the native MOD MENU source row */
    struct ov_window readouts; /* the OVERLAYS readouts (FPS and the rest) */
    struct ov_window clockn;   /* the Clock, in the place of EP147's recording time readout */
    struct ov_window clocktap; /* the Clock's tap target (input only): opens its Timer popup */
    struct ov_window ctpop;    /* the Clock's Timer popup */
    struct ht_gest ct_gest;    /* taps on those two (a whole tap, never a press or a drag) */
    int ct_key;                /* what the Clock's place last showed (azmod_clockn_key) */
    struct ov_window lane[AZMOD_STEM_DECKS]; /* one stem-lane band per deck */

    int need_raise;     /* something covered us: raise our windows once this tick */
    int dirty;          /* recomposite and repaint this tick */
    int det_tick, stem_tick, wave_tick; /* tick counters for the throttled blocks */
    int pending_reload; /* RELOAD was tapped: fire once its press flash is over */
    long left_src_ms;   /* when a screen button last closed the docked menu */
    int reloading_was;  /* /run/az-reloading was seen last tick (edge detect) */
};

static void ov_window_init(struct ov_window *w) {
    w->id = None;
    w->wanted = 0;
    w->mapped = 0;
    w->x = w->y = w->w = w->h = -1;
}
/* Community mods' surfaces and foreign windows (sdk_surface.c, included by community.c). */
static void sdk_overlay_attach(struct overlay_ctx *c);
static void sdk_overlay_tick(struct overlay_ctx *c);
static int sdk_overlay_x_event(struct overlay_ctx *c, XEvent *ev, int touch);
static int sdk_overlay_knob(int dir, int click);
static void sdk_overlay_place(struct overlay_ctx *c, int layer);
static void sdk_overlay_place_xwin(struct overlay_ctx *c);
static void sdk_overlay_present(struct overlay_ctx *c, int all);
static int sdk_any_redraw(void);

#include "ov_clip.h" /* a window's part of the buffer, clipped as XPutImage clips */
/* ---- frames through a shared segment (MIT-SHM) ----------------------------------------------
 * A plain XPutImage copies the frame through the X socket: ~33 ms for the full-screen menu on the
 * deck, half of a frame. EP147 sends its own frames through shared memory, and so does the
 * overlay when the server offers it: the composite buffer IS the segment, and a put is ~3.6 ms
 * (both measured on the deck, put + XSync). The server reads the segment when it handles the
 * request, so nothing may draw into the buffer while a put is unread: ov_shm_wait before every
 * write that follows one. Without the extension, or when the attach fails, the plain path: the
 * same pixels. */
static int (*g_ov_prev_xerr)(Display *, XErrorEvent *);
static Display *g_ov_shm_dpy;           /* the attach in flight: its display and request serial */
static unsigned long g_ov_shm_serial;
static int g_ov_shm_err;
static Display *g_ov_dpy;  /* the overlay's display, once its segment is attached */
static int g_ov_shm_major; /* MIT-SHM's request code on it */
static int g_ov_shm_broken; /* the server refused a shared put: the plain path from now on */
/* X calls report errors here from any thread of the process (EP147's own included). Ours: the
 * attach's error, and any refused shared request on the overlay's display, which turns the
 * shared path off instead of ending EP147 (Xlib's default handler exits). Every other error
 * goes to the handler that was installed before. */
static int ov_shm_xerr(Display *d, XErrorEvent *e) {
    if (d == __atomic_load_n(&g_ov_shm_dpy, __ATOMIC_ACQUIRE) &&
        e->serial == __atomic_load_n(&g_ov_shm_serial, __ATOMIC_RELAXED)) {
        __atomic_store_n(&g_ov_shm_err, 1, __ATOMIC_RELAXED);
        return 0;
    }
    int major = __atomic_load_n(&g_ov_shm_major, __ATOMIC_RELAXED);
    if (d == __atomic_load_n(&g_ov_dpy, __ATOMIC_ACQUIRE) && major && e->request_code == major) {
        __atomic_store_n(&g_ov_shm_broken, 1, __ATOMIC_RELAXED);
        return 0;
    }
    return g_ov_prev_xerr ? g_ov_prev_xerr(d, e) : 0;
}
static uint8_t *ov_shm_create(struct overlay_ctx *c) {
    if (!XShmQueryExtension(c->dpy))
        return NULL;
    XImage *im = XShmCreateImage(c->dpy, DefaultVisual(c->dpy, c->scr), DefaultDepth(c->dpy, c->scr), ZPixmap,
                                 NULL, &c->shm, (unsigned) c->scr_w, (unsigned) c->scr_h);
    if (!im)
        return NULL;
    if (im->bits_per_pixel != 32 || im->bytes_per_line != c->scr_w * 4) { /* not g_fb's layout */
        XDestroyImage(im);
        return NULL;
    }
    c->shm.shmid = shmget(IPC_PRIVATE, (size_t) c->scr_w * c->scr_h * 4, IPC_CREAT | 0600);
    if (c->shm.shmid < 0) {
        XDestroyImage(im);
        return NULL;
    }
    c->shm.shmaddr = shmat(c->shm.shmid, NULL, 0);
    c->shm.readOnly = True;
    int ok = c->shm.shmaddr != (char *) -1;
    if (ok) {
        XSync(c->dpy, False); /* every earlier request's errors are someone else's */
        static int installed;
        if (!installed) {
            g_ov_prev_xerr = XSetErrorHandler(ov_shm_xerr);
            installed = 1;
        }
        __atomic_store_n(&g_ov_shm_err, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_ov_shm_serial, NextRequest(c->dpy), __ATOMIC_RELAXED);
        __atomic_store_n(&g_ov_shm_dpy, c->dpy, __ATOMIC_RELEASE);
        ok = XShmAttach(c->dpy, &c->shm) && (XSync(c->dpy, False), !__atomic_load_n(&g_ov_shm_err, __ATOMIC_RELAXED));
        __atomic_store_n(&g_ov_shm_dpy, NULL, __ATOMIC_RELEASE);
    }
    shmctl(c->shm.shmid, IPC_RMID, NULL); /* freed with the last attach (ours and the server's) */
    if (!ok) {
        if (c->shm.shmaddr != (char *) -1)
            shmdt(c->shm.shmaddr);
        XDestroyImage(im);
        return NULL;
    }
    im->data = c->shm.shmaddr;
    c->shimg = im;
    int op, ev, er;
    if (XQueryExtension(c->dpy, "MIT-SHM", &op, &ev, &er))
        __atomic_store_n(&g_ov_shm_major, op, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ov_dpy, c->dpy, __ATOMIC_RELEASE);
    memset(c->shm.shmaddr, 0, (size_t) c->scr_w * c->scr_h * 4);
    return (uint8_t *) c->shm.shmaddr;
}
/* until the server has read every shared put so far: before any write into the buffer */
static void ov_shm_wait(struct overlay_ctx *c) {
    if (c->shm_busy) {
        XSync(c->dpy, False);
        c->shm_busy = 0;
    }
}
/* one window's part of the buffer to the screen: the part of its rectangle that is in the
 * buffer (a parked window's is not), as XPutImage would clip it (ov_clip.h) */
static void ov_put(struct overlay_ctx *c, Window w, int x, int y, int ww, int hh) {
    struct ov_part p;
    if (!ov_clip(x, y, ww, hh, c->scr_w, c->scr_h, &p))
        return;
    if (c->shimg && __atomic_load_n(&g_ov_shm_broken, __ATOMIC_RELAXED)) {
        dbg("overlay: the server refused a shared put: frames through the socket from now on\n");
        c->shimg = NULL; /* the segment stays the buffer; plain puts read it like any memory */
    }
    if (c->shimg) {
        XShmPutImage(c->dpy, w, c->gc, c->shimg, p.sx, p.sy, p.dx, p.dy, (unsigned) p.w, (unsigned) p.h, False);
        c->shm_busy = 1;
    } else
        XPutImage(c->dpy, w, c->gc, c->img, p.sx, p.sy, p.dx, p.dy, (unsigned) p.w, (unsigned) p.h);
}

/* Connect to the X server, waiting up to 30 s for it, and set up the framebuffer. */
static int ov_open_display(struct overlay_ctx *c) {
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 300 && !c->dpy; i++) {
        c->dpy = XOpenDisplay(NULL);
        if (!c->dpy)
            usleep(100000);
    }
    if (!c->dpy)
        return 0;
    c->scr = DefaultScreen(c->dpy);
    c->scr_w = DisplayWidth(c->dpy, c->scr);
    c->scr_h = DisplayHeight(c->dpy, c->scr);
    c->root = RootWindow(c->dpy, c->scr);
    c->need_raise = 1; /* latched by VisibilityNotify; see ov_drain_x_events() */
    c->dirty = 1;
    return 1;
}

/* An override-redirect window parked off screen; input-transparent windows get an empty
 * input shape so taps fall through to EP147 underneath. */
static Window ov_create_window(struct overlay_ctx *c, XSetWindowAttributes *a, int x, int y,
                               int w, int h, int input_transparent) {
    Window id =
        XCreateWindow(c->dpy, c->root, x, y, w, h, 0, CopyFromParent, InputOutput, CopyFromParent,
                      CWOverrideRedirect | CWBackPixmap | CWEventMask, a);
    if (input_transparent)
        XShapeCombineRectangles(c->dpy, id, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);
    return id;
}

static void ov_create_windows(struct overlay_ctx *c) {
    XSetWindowAttributes a;
    memset(&a, 0, sizeof a);
    /* No background (None, not black): X keeps what is on screen under a newly mapped or grown
     * window until our XPutImage lands. A background pixel is cleared on every map/resize, and a
     * full-screen put lands in strips, so a black background flashes for a frame in Day Mode. */
    a.override_redirect = True;
    a.background_pixmap = None;
    a.event_mask = ButtonPressMask | ExposureMask | VisibilityChangeMask;

    /* The menu window is created full-screen but not mapped: the loop resizes it to
     * wanted_rect() before the first map, so EP147 never sees a full-screen
     * override-redirect window from us. */
    ov_window_init(&c->menu);
    c->menu.id = ov_create_window(c, &a, 0, 0, c->scr_w, c->scr_h, 0);
    c->menu.x = 0;
    c->menu.y = 0;
    c->gc = XCreateGC(c->dpy, c->menu.id, 0, NULL);
    c->shimg = NULL;
    c->shm_busy = 0;
    c->buf = ov_shm_create(c);
    dbg("overlay: frames %s\n", c->buf ? "through a shared segment (MIT-SHM)" : "through the socket (no MIT-SHM)");
    if (!c->buf)
        c->buf = calloc(1, (size_t) c->scr_w * c->scr_h * 4);
    c->img = XCreateImage(c->dpy, DefaultVisual(c->dpy, c->scr), DefaultDepth(c->dpy, c->scr),
                          ZPixmap, 0, (char *) c->buf, c->scr_w, c->scr_h, 32, c->scr_w * 4);
    g_fb.px = c->buf;
    g_fb.w = c->scr_w;
    g_fb.h = c->scr_h;
    g_fb.stride = c->scr_w * 4;

    /* Deck 2's stem strip. The menu window is sized to exactly one strip (anything larger
     * would black out the waveform underneath), and deck 2's strip is far from deck 1's. Same
     * display connection, so its ButtonPress events arrive in the one event loop and
     * azmod_input() hit-tests by absolute coordinates. */
    ov_window_init(&c->strip2);
    c->strip2.id = ov_create_window(c, &a, -16, -16, 1, 1, 0);
    /* The icon cell of the native "MOD MENU" source row: taps go to EP147's own row. */
    ov_window_init(&c->srcicon);
    c->srcicon.id = ov_create_window(c, &a, -16, -16, 1, 1, 1);
    /* The OVERLAYS readouts, top right of the deck screen. */
    ov_window_init(&c->readouts);
    c->readouts.id = ov_create_window(c, &a, -16, -16, 1, 1, 1);
    /* The Clock when it sits top left. */
    ov_window_init(&c->clockn);
    c->clockn.id = ov_create_window(c, &a, -16, -16, 1, 1, 1);
    /* The Clock's tap target: an input-only window over the Clock, a little taller than its
     * 20 px text for a finger. It takes the taps there, so EP147's readout under it (which
     * opens MIXER SETTINGS) does not get them. And the Clock's Timer popup. Both take press
     * and release: the Timer acts on a whole tap. */
    {
        XSetWindowAttributes ia;
        memset(&ia, 0, sizeof ia);
        ia.override_redirect = True;
        ia.event_mask = ButtonPressMask | ButtonReleaseMask;
        ov_window_init(&c->clocktap);
        c->clocktap.id = XCreateWindow(c->dpy, c->root, -16, -16, 1, 1, 0, 0, InputOnly, CopyFromParent,
                                       CWOverrideRedirect | CWEventMask, &ia);
    }
    ov_window_init(&c->ctpop);
    c->ctpop.id = ov_create_window(c, &a, -16, -16, 1, 1, 0);
    XSelectInput(c->dpy, c->ctpop.id, ButtonPressMask | ButtonReleaseMask | ExposureMask | VisibilityChangeMask);
    c->ct_key = -1;
    /* Stem lanes: one window per deck's waveform band, input-transparent so the waveform
     * underneath still takes needle-drop touches. There must be one per AZMOD_STEM_DECKS: the
     * paint loop uses lane[d] for every deck, and an uncreated entry is a BadWindow, which
     * makes Xlib exit() and takes EP147 down. */
    for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
        ov_window_init(&c->lane[d]);
        c->lane[d].id = ov_create_window(c, &a, -16, -16, 1, 1, 1);
    }
}

/* Saved settings, community mods, and everything published from them at startup. */
static void ov_load_state(void) {
    azmod_state_init(&g_state);
    load_config(&g_state);
    bus_cfg_publish();
    community_init(); /* community mods: load the enabled ones (no-op with none enabled) */
    azm_hooks_dump(AZM_HOOKTABLE_PATH); /* again: community hooks (late, island only) */
    publish_mode_colors(&g_state); /* pad-mode picker: seed the LED interposer from saved colors */
    write_enable(&g_state);        /* publish master enable to azstem.so on startup */
    mkdir(SHOTS_DIR, 0755);          /* best-effort; EEXIST is fine */
}

/* While the USB loader is restarting EP147, keep its banner up and skip the tick (returns 1).
 * When the flag clears without a restart, force the menu window to be laid out again. */
static int ov_reload_banner(struct overlay_ctx *c) {
    /* The USB loader is about to restart EP147: it creates /run/az-reloading (tmpfs) and
     * waits ~1.5 s. Keep the RELOADING banner painted (each repaint re-raises it) until we
     * die with the restart. The loader owns both the flag and the restart. */
    if (access("/run/az-reloading", F_OK) == 0) {
        ov_shm_wait(c); /* the banner draws into the buffer */
        show_reloading(c->dpy, c->menu.id, c->gc, c->img, c->buf, c->scr_w, c->scr_h);
        c->reloading_was = 1;
        usleep(30000);
        return 1;
    }
    if (c->reloading_was) {
        /* The flag cleared without a restart. show_reloading left the menu window full-screen, unshaped
         * and mapped, so the cached geometry and shape are stale: force a full re-layout or
         * the banner stays up. */
        c->reloading_was = 0;
        c->menu.w = -1;
        c->menu.h = -1;
        c->menu_shaped = 0;
        c->menu.mapped = 0;
        c->dirty = 1;
    }
    return 0;
}


/* Motion and release are selected only while a drag can matter (the Update Mods changelog,
 * the full-screen menu's right pane), so no other screen's touch handling changes. */
static void ov_select_input(struct overlay_ctx *c) {
    static long sel_mask = -1;
    long want_mask = ButtonPressMask | ExposureMask | VisibilityChangeMask;
    if (g_state.mode == AZ_UPDATE ||
        g_state.menu_focused) /* + the full-screen menu's right pane (azmod_pane_drag) */
        want_mask |= ButtonReleaseMask | ButtonMotionMask | PointerMotionMask;
    if (want_mask != sel_mask) {
        XSelectInput(c->dpy, c->menu.id, want_mask);
        sel_mask = want_mask;
    }
}

/* A tap that took the menu back to the top (the header BACK square on the press; a left-list
 * row, selected on the RELEASE) must leave the same state as a physical depth-2 BACK on the bus
 * thread: cursor in the menu, top published now, row repainted. Otherwise the next physical
 * BACK is judged at the wrong depth. */
static void ov_touch_reached_top(azmod_mode before, int before_top) {
    if (before != AZ_CLOSED && !before_top && azmod_menu_at_top(&g_state)) {
        __atomic_store_n(&g_menu_focused, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&g_menu_top, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&g_srcrow_repaint, 1, __ATOMIC_RELEASE);
    }
}
static void ov_on_press(struct overlay_ctx *c, XEvent *ev) {
    /* x_root/y_root are screen-absolute (x/y are window-relative), which is
     * azmod_input()'s coordinate space whatever the window geometry. */
    dbg("[%ld] ButtonPress local=(%d,%d) root=(%d,%d) cur=(%d,%d,%d,%d) mode=%d\n",
        (long) time(NULL), ev->xbutton.x, ev->xbutton.y, ev->xbutton.x_root,
        ev->xbutton.y_root, c->menu.x, c->menu.y, c->menu.w, c->menu.h, g_state.mode);
    azmod_mode before = g_state.mode;
    int before_top = azmod_menu_at_top(&g_state);
    /* Test, logs flag only: make the emulator's touch stream look like the deck's.
     * MotionNotify can be missing (/tmp/azmod-touchsim-nomotion); releases can be
     * dropped too (/tmp/azmod-touchsim-norelease). Read once per press. */
    g_touchsim = g_azlog_on > 0
                     ? (access("/tmp/azmod-touchsim-nomotion", F_OK) == 0) |
                           (access("/tmp/azmod-touchsim-norelease", F_OK) == 0) << 1
                     : 0;
    azmod_menu_clock(now_ms());
    azmod_input(&g_state, &g_fb, ev->xbutton.x_root, ev->xbutton.y_root, true, 0, false);
    menu_touch_log("press", ev);
    /* The full-screen menu's two cursor moves, done exactly as a tap on our SOURCE
     * row does them (src_click_hook): the preview card asks for focus, the header
     * back square hands the cursor back. */
    if (g_state.want_focus) {
        g_state.want_focus = false;
        __atomic_store_n(&g_srcrow_want, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&g_menu_focused, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&g_menu_open, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&g_srcrow_open, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&g_srcrow_repaint, 1, __ATOMIC_RELEASE);
    }
    if (g_state.want_unfocus) {
        g_state.want_unfocus = false;
        __atomic_store_n(&g_menu_focused, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_srcrow_unfocus, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&g_srcrow_repaint, 1, __ATOMIC_RELEASE);
    }
    ov_touch_reached_top(before, before_top);
    handle_action(c->dpy, c->root, c->scr_w, c->scr_h, &c->pending_reload);
    c->dirty = 1;
}

static void ov_on_motion(struct overlay_ctx *c, XEvent *ev) {
    /* only the last motion of a burst matters */
    XEvent nx;
    while (XCheckTypedWindowEvent(c->dpy, ev->xmotion.window, MotionNotify, &nx))
        *ev = nx;
    if (azmod_changelog_drag(&g_state, &g_fb, ev->xmotion.y_root, 1))
        c->dirty = 1;
    if (azmod_cm_drag(&g_state, &g_fb, ev->xmotion.y_root, 1))
        c->dirty = 1;
    if (g_touchsim & 1) {
        menu_touch_log("motion(sim-dropped)", ev);
        return;
    }
    azmod_menu_clock(now_ms());
    if (azmod_pane_drag(&g_state, &g_fb, ev->xmotion.x_root, ev->xmotion.y_root, 1))
        c->dirty = 1;
    menu_touch_log("motion", ev);
}

/* A press while the finger is still down: on the deck, the finger moved there (touch_seq.h).
 * It is a drag step for every gesture in progress and never a new touch. */
static void ov_on_press_move(struct overlay_ctx *c, XEvent *ev) {
    if (azmod_changelog_drag(&g_state, &g_fb, ev->xbutton.y_root, 1))
        c->dirty = 1;
    if (azmod_cm_drag(&g_state, &g_fb, ev->xbutton.y_root, 1))
        c->dirty = 1;
    azmod_menu_clock(now_ms());
    if (azmod_pane_drag(&g_state, &g_fb, ev->xbutton.x_root, ev->xbutton.y_root, 1))
        c->dirty = 1;
    menu_touch_log("press(move)", ev);
}

static void ov_on_release(struct overlay_ctx *c, XEvent *ev) {
    if (g_touchsim & 2) {
        menu_touch_log("release(sim-dropped)", ev);
        return;
    }
    /* The last step of a move can be the release itself, so its position is a drag step
     * too. */
    if (azmod_changelog_drag(&g_state, &g_fb, ev->xbutton.y_root, 1))
        c->dirty = 1;
    if (azmod_changelog_drag(&g_state, &g_fb, ev->xbutton.y_root, 2))
        c->dirty = 1;
    if (azmod_cm_drag(&g_state, &g_fb, ev->xbutton.y_root, 1))
        c->dirty = 1;
    azmod_cm_drag(&g_state, &g_fb, ev->xbutton.y_root, 2);
    azmod_menu_clock(now_ms());
    azmod_mode before = g_state.mode;
    int before_top = azmod_menu_at_top(&g_state);
    int pd = azmod_pane_drag(&g_state, &g_fb, ev->xbutton.x_root, ev->xbutton.y_root, 2);
    menu_touch_log("release", ev);
    if (pd)
        c->dirty = 1;
    if (pd == 2) { /* a tap that fired on release */
        ov_touch_reached_top(before, before_top);
        handle_action(c->dpy, c->root, c->scr_w, c->scr_h, &c->pending_reload);
    }
}

/* A button event on the Clock's tap target or its Timer popup (returns 1: taken). Only a whole
 * tap acts (ht_tap.h), at the press point: on the Clock it opens or closes the popup, on the
 * popup it presses the button there. */
static int ov_clock_timer_event(struct overlay_ctx *c, XEvent *ev) {
    if (ev->type != ButtonPress && ev->type != ButtonRelease)
        return 0;
    Window w = ev->xbutton.window;
    if (w == None || (w != c->clocktap.id && w != c->ctpop.id))
        return 0;
    int tx, ty;
    if (!ht_gest_feed(&c->ct_gest, ev->type == ButtonPress, ev->xbutton.button, ev->xbutton.x_root,
                      ev->xbutton.y_root, now_ms(), &tx, &ty))
        return 1;
    if (c->clocktap.mapped && tx >= c->clocktap.x && tx < c->clocktap.x + c->clocktap.w &&
        ty >= c->clocktap.y && ty < c->clocktap.y + c->clocktap.h)
        azmod_clockn_tap(&g_state, &g_fb);
    else
        azmod_ctpop_tap(&g_fb, tx, ty);
    c->dirty = 1;
    return 1;
}

/* Taps, drags and visibility changes on our windows, drained first so a tap this tick shows
 * in this tick's draw. */
static void ov_drain_x_events(struct overlay_ctx *c) {
    ov_select_input(c);
    while (XPending(c->dpy)) {
        XEvent ev;
        XNextEvent(c->dpy, &ev);
        /* one touch sequence for all our windows: a finger that lands on one and moves onto
         * another is still one touch */
        int touch = ev.type == ButtonPress || ev.type == ButtonRelease
                        ? touch_seq_feed(&c->touch, ev.type == ButtonPress, ev.xbutton.button, now_ms())
                        : TOUCH_OTHER;
        if (ov_clock_timer_event(c, &ev))
            continue; /* the Clock or its Timer popup */
        if (sdk_overlay_x_event(c, &ev, touch))
            continue; /* a community mod's surface */
        if (ev.type == ButtonPress && touch == TOUCH_MOVE) {
            ov_on_press_move(c, &ev);
        } else if (ev.type == ButtonPress) {
            ov_on_press(c, &ev);
        } else if (ev.type == MotionNotify) {
            ov_on_motion(c, &ev);
        } else if (ev.type == ButtonRelease) {
            ov_on_release(c, &ev);
        } else if (ev.type == Expose) {
            c->dirty = 1;
        } else if (ev.type == VisibilityNotify) {
            /* EP147 repaints continuously (waveforms, meters), which flips our visibility
             * state at 100+ Hz on the deck. Raising on every flip fights EP147 for the top
             * of the stack and flickers, so only latch it; the tick raises at most once per
             * pass. */
            if (ev.xvisibility.state != VisibilityUnobscured)
                c->need_raise = 1;
        }
    }
}

/* Arriving at Browse hides whatever deck-screen or SOURCE surface was up. */
static void ov_on_browse_view(struct overlay_ctx *c) {
    /* Instant hide on arriving at Browse (see g_browseview_shown). BrowseView shows on the
     * way in from either SOURCE or the deck screen, so clear whichever was up. */
    if (__atomic_exchange_n(&g_browseview_shown, 0, __ATOMIC_ACQ_REL)) {
        if (g_state.source_visible) {
            g_state.source_visible = 0;
            write_stems(&g_state);
            c->dirty = 1;
        }
        if (g_state.waveform_visible) {
            g_state.waveform_visible = 0;
            c->dirty = 1;
            ov_forget_all();
        }
    }
}

/* The page a resume brings back (mm_resume.h mmr_page_*): every close the overlay makes keeps
 * the mode it ends, and the open after a resume shows that mode again instead of the reset;
 * the selection, the panes, their scroll and the Community Mods view are left as they were by
 * the close. Overlay thread. */
static int g_mm_page = MMR_PAGE_NONE;
static void ov_menu_close(void) {
    g_mm_page = mmr_page_keep(g_mm_page, (int) g_state.mode);
    g_state.mode = AZ_CLOSED;
}
/* An open: the kept page when a resume armed it (returns 1), else the last category as before. */
static int ov_menu_open_page(void) {
    int page = mmr_page_take(&g_mm_page_armed, g_mm_page);
    if (page == MMR_PAGE_NONE) {
        azmod_menu_reset(&g_state);
        return 0;
    }
    g_state.mode = (azmod_mode) page;
    return 1;
}
/* Apply what the bus and EP147's threads handed over since the last tick: knob turns and
 * clicks, BACK, a native MOD MENU row item, a screen button that closed the menu, and a
 * theme change. */
static void ov_take_menu_handoffs(struct overlay_ctx *c) {
    /* drain the knob/back handoff from the mixer-bus interposer thread */
    int dir = __atomic_exchange_n(&g_enc_dir, 0, __ATOMIC_ACQ_REL);
    int ck = __atomic_exchange_n(&g_click_pending, 0, __ATOMIC_ACQ_REL);
    int bk = __atomic_exchange_n(&g_back_pending, 0, __ATOMIC_ACQ_REL);
    int mfocused = __atomic_load_n(&g_menu_focused, __ATOMIC_RELAXED);
    { /* Theme changed: azmod's own windows repaint in the new palette (azm_theme) */
        static int dm_seen = -1;
        int dm_now =
            (int) azm_theme_key(); /* the palette, not just on/off: a theme change too */
        if (dm_now != dm_seen) {
            dm_seen = dm_now;
            c->dirty = 1;
        }
    }
    int act = __atomic_exchange_n(&g_srcrow_act, 0, __ATOMIC_ACQ_REL);
    if (act) {
        srcrow_activate_item(act - 1);
        handle_action(c->dpy, c->root, c->scr_w, c->scr_h,
                      &c->pending_reload); /* persist toggles, fire RELOAD, refresh screens */
        c->dirty = 1;
    }
    /* A community mod's surface with focus takes the knob first. Otherwise the knob drives
     * the menu only when FOCUSED; in PREVIEW it belongs to EP147's left list (the read()
     * boundary handles hover). */
    if (sdk_overlay_knob(dir, ck)) {
        dir = ck = 0;
    } else if (mfocused && (dir || ck)) {
        azmod_input(&g_state, &g_fb, 0, 0, false, dir, ck != 0);
        handle_action(c->dpy, c->root, c->scr_w, c->scr_h, &c->pending_reload);
        c->dirty = 1;
    }
    if (bk) { /* BACK: FOCUSED submenu -> top; FOCUSED top -> PREVIEW (cursor back to the
               * source list, like a native menu). In PREVIEW it does nothing: the menu
               * stays while MOD MENU is the selected source. */
        /* bk is the depth the bus thread decided, and it already moved focus. Only the
         * menu-side half is left. */
        if (bk == 2 && g_state.mode != AZ_CLOSED && !azmod_menu_at_top(&g_state)) {
            azmod_back(&g_state); /* submenu -> top, cursor stays in the menu */
            __atomic_store_n(&g_srcrow_repaint, 1, __ATOMIC_RELEASE); /* row -> gray */
            c->dirty = 1;
        } else if (bk == 1) {
            if (g_state.mode != AZ_CLOSED)
                azmod_menu_reset(&g_state); /* preview shows the card */
            __atomic_store_n(&g_srcrow_repaint, 1, __ATOMIC_RELEASE); /* row -> blue */
            c->dirty = 1;
        }
    }
    if (__atomic_exchange_n(&g_close_pending, 0, __ATOMIC_ACQ_REL) &&
        g_state.mode != AZ_CLOSED) {
        ov_menu_close();
        menu_drop_intent();
        c->dirty = 1; /* left via a screen button */
        /* We are also leaving SOURCE: say so now. source_visible is a 1-in-5 title sample
         * and can lag ~150 ms. A source-list refresh in that window (src_rows_hook's RESYNC;
         * EP147's selection is still our row) sets want=1 again, and the PREVIEW open below
         * would re-dock the menu over the deck screen until the visibility gate shut it: a
         * visible flash. The left_src_ms hold-off stops a title still reading SOURCE from
         * turning it back on. */
        if (g_state.source_visible) {
            g_state.source_visible = 0;
            write_stems(&g_state);
        }
        c->left_src_ms = now_ms();
    }
}

/* Publish the menu's open/top state for the bus thread, and keep the Update Mods screen and
 * its badge current. */
static void ov_publish_menu_state(struct overlay_ctx *c) {
    __atomic_store_n(&g_menu_open, g_state.mode != AZ_CLOSED, __ATOMIC_RELAXED);
    __atomic_store_n(&g_menu_top, azmod_menu_at_top(&g_state), __ATOMIC_RELAXED);
    if (g_state.mode == AZ_UPDATE) { /* live status while the screen is up, ~2/s */
        static unsigned utk;
        if ((++utk % 15u) == 0 && read_modupdate(&g_state))
            c->dirty = 1;
    } else if (g_state.mode != AZ_CLOSED) { /* menu up: keep the red badge honest, ~1/s */
        static unsigned stk;
        if (g_state.mode == AZ_INFO && (++stk % 15u) == 0 && read_savelogs(&g_state))
            c->dirty = 1; /* Save Logs to USB's status, ~2/s while System Info is up */
        static unsigned btk;
        if ((++btk % 30u) == 0) {
            int was = g_state.update_avail;
            read_modupdate(&g_state);
            if (was != g_state.update_avail)
                c->dirty = 1;
        }
    }
}

/* Knob ownership diagnostics (azmod-logs.on): once a second, only when something moved. */
static void ov_log_knob(void) {
    /* knob ownership diagnostics (azmod-logs.on): once a second, only when something moved */
    static long kn_last, tick_last, tick_max;
    static unsigned kn_f0;
    static int want_last = -1;
    long kn_now = drag_now_ms();
    if (tick_last && kn_now - tick_last > tick_max)
        tick_max = kn_now - tick_last; /* the longest overlay tick */
    tick_last = kn_now;
    int want_now = __atomic_load_n(&g_srcrow_want, __ATOMIC_RELAXED);
    unsigned fr = __atomic_load_n(&g_kn_frames, __ATOMIC_RELAXED);
    if ((kn_now - kn_last >= 1000 && fr != kn_f0) || want_now != want_last) {
        FILE *kf = azlog_open("/tmp/azmod-knob.log", "a");
        if (kf) {
            fprintf(
                kf,
                "t=%ld tick_max=%ldms focused=%d open=%d want=%d mode=%d left=%d frames=%u "
                "wd_max=%u releases=%u leaked_detents=%u src_turns_eaten=%u\n",
                kn_now, tick_max, __atomic_load_n(&g_menu_focused, __ATOMIC_RELAXED),
                g_state.mode != AZ_CLOSED, want_now, (int) g_state.mode, g_state.left_sel,
                fr, __atomic_load_n(&g_kn_wdmax, __ATOMIC_RELAXED),
                __atomic_load_n(&g_kn_rel, __ATOMIC_RELAXED),
                __atomic_load_n(&g_kn_leak, __ATOMIC_RELAXED),
                __atomic_load_n(&g_kn_eaten, __ATOMIC_RELAXED));
            fclose(kf);
        }
        kn_last = kn_now;
        kn_f0 = fr;
        want_last = want_now;
        tick_max = 0;
    }
}

/* Source-row state for diagnostics (azmod-logs.on), about twice a second. */
static void ov_log_srcrow_state(void) {
    static unsigned srtk;
    if ((srtk++ & 31u) ==
        0) { /* srcrow diag ~2/s, overlay thread only (safe: not the JUCE/read hot paths) */
        void *tbl = __atomic_load_n(&g_tbl, __ATOMIC_ACQUIRE);
        /* State is rewritten ("w") ~2/s, so it must stay in its own file: truncating the
         * event log it sits beside would erase the CLICK/SEL/ROWTAP lines within ~500 ms
         * and make working hooks look dead. */
        FILE *sf = azlog_open("/tmp/azmod-srcrow-state.log", "w");
        if (sf) {
            /* g_tbl is EP147's TableListBox, cached across calls and freed when the
             * source list closes. This thread has no lifetime guarantee for it, so a
             * release build logs only the pointer (-1 for its size and item count);
             * development builds read them through the dev-only fault-free reader. */
            int tw = -1, th = -1, ti = -1;
            fprintf(sf,
                    "active=%d ctor_ok=%d cand_n=%d tbl=%p menu=%d n=%d want=%d mode=%d "
                    "msel=%d foc=%d | tbl_w=%d tbl_h=%d items=%d\n",
                    g_srcrow_active, g_slw_ctor_ok,
                    __atomic_load_n(&g_tbl_cand_n, __ATOMIC_RELAXED), tbl,
                    __atomic_load_n(&g_srcrow_menu, __ATOMIC_RELAXED),
                    __atomic_load_n(&g_srcrow_n, __ATOMIC_RELAXED),
                    __atomic_load_n(&g_srcrow_want, __ATOMIC_RELAXED), g_state.mode,
                    g_state.menu_sel, __atomic_load_n(&g_menu_focused, __ATOMIC_RELAXED),
                    tw, th, ti);
            fclose(sf);
        }
    }
}

/* Arm or disarm the stems surfaces when the STEMS master switch changes. */
static void ov_sync_stems_master(void) {
    /* The STEMS master (mod 0) is the single switch for every layer: arm the center
     * PadStemWidget on its edge. Per-deck availability gates within that surface:
     * g_erp_active/g_erp_stemless below decide pads and LEDs per deck, and azstem applies
     * the masks only where it has stem audio. */
    static int last_master = -1;
    if (azmod_stems_engine(&g_state) != last_master) {
        last_master = azmod_stems_engine(&g_state);
        arm_stems(last_master);
    }
}

/* azstem's Drum Roll and Stem Volume entry points, resolved on first use: azstem.so may load
 * after us, and a missing symbol means "feature absent", never a crash. */
static void ov_resolve_drum_exports(void) {
    if (g_drum_gate_fn)
        return;
    __atomic_store_n((void **) &g_drum_gate_fn, dlsym(RTLD_DEFAULT, "azstem_drum_gate"),
                     __ATOMIC_RELEASE); /* read by the bus */
    __atomic_store_n((void **) &g_drum_select_fn,
                     dlsym(RTLD_DEFAULT, "azstem_drum_select"), /* read by the bus */
                     __ATOMIC_RELEASE);
    __atomic_store_n((void **) &g_drum_rate_fn,
                     dlsym(RTLD_DEFAULT, "azstem_drum_rate"), /* read by the bus */
                     __ATOMIC_RELEASE);
    __atomic_store_n((void **) &g_drum_touch_fn,
                     dlsym(RTLD_DEFAULT, "azstem_drum_touch"), /* read by the bus */
                     __ATOMIC_RELEASE);
    *(void **) &g_drum_bank_fn = dlsym(RTLD_DEFAULT, "azstem_drum_bank");
    *(void **) &g_drum_avail_fn = dlsym(RTLD_DEFAULT, "azstem_drum_available");
    *(void **) &g_drum_bpm_fn = dlsym(RTLD_DEFAULT, "azstem_drum_bpm");
    /* absent in older azstem: an empty deck rolls at the master's BPM, off its beat */
    *(void **) &g_drum_follow_fn = dlsym(RTLD_DEFAULT, "azstem_drum_follow");
    *(void **) &g_drum_sync_fn = dlsym(RTLD_DEFAULT, "azstem_drum_sync");
    __atomic_store_n((void **) &g_drum_tone_fn,
                     dlsym(RTLD_DEFAULT, "azstem_drum_tone"), /* read by the bus */
                     __ATOMIC_RELEASE); /* absent in older azstem: no tone control */
    *(void **) &g_stem_eq_fn = dlsym(
        RTLD_DEFAULT, "azstem_stem_eq"); /* absent in older azstem: no stem volume */
    *(void **) &g_deck_has_track_fn = dlsym(RTLD_DEFAULT, "azstem_deck_has_track");
    *(void **) &g_stem_gain_fn = dlsym(
        RTLD_DEFAULT, "azstem_stem_gain"); /* absent in older azstem: waveforms at unity */
    *(void **) &g_sfx_config_fn = dlsym(RTLD_DEFAULT, "azstem_stem_fx_config");
    __atomic_store_n((void **) &g_sfx_on_fn, dlsym(RTLD_DEFAULT, "azstem_stem_fx_on"),
                     __ATOMIC_RELEASE); /* read by the bus */
    __atomic_store_n((void **) &g_sfx_fn, dlsym(RTLD_DEFAULT, "azstem_stem_fx"),
                     __ATOMIC_RELEASE); /* read by the bus; absent in older azstem: solo pads */
}

/* Stem FX (Stem FX = FX): the mode to the bus, mode and lengths to azstem when they
 * change, and each deck's on bits for the LEDs. Overlay thread. */
static void ov_sync_stem_fx(void) {
    static int last = -1;
    static const int ECHO_Q[4] = {1, 2, 3, 4};  /* quarter beats */
    static const int BRAKE_Q[4] = {2, 4, 8, 16};
    int mode = az_on(AZF_STEMS) && az_get(AZF_STEM_FX) == 1 && g_sfx_fn && g_sfx_on_fn &&
               g_sfx_config_fn;
    int e = az_get(AZF_STEM_FX_ECHO), b = az_get(AZF_STEM_FX_BRAKE);
    int key = mode | (e & 3) << 1 | (b & 3) << 3;
    if (key != last) {
        last = key;
        if (g_sfx_config_fn)
            g_sfx_config_fn(mode, ECHO_Q[e & 3], BRAKE_Q[b & 3]);
        __atomic_store_n(&g_stemfx_pads, mode, __ATOMIC_RELEASE);
    }
    g_sfx_led_mode = mode;
#ifdef AZMOD_PROBES
    if (AZ_PROBE_ON("/home/root/settings/azmod-stemfx.log")) {
        unsigned w = __atomic_load_n(&g_sfxlog_w, __ATOMIC_ACQUIRE);
        if (w - g_sfxlog_r > 256u)
            g_sfxlog_r = w - 256u;
        FILE *f = g_sfxlog_r != w ? azlog_open("/tmp/azmod-stemfx.log", "a") : NULL;
        for (; f && g_sfxlog_r != w; g_sfxlog_r++) {
            unsigned e = __atomic_load_n(&g_sfxlog[g_sfxlog_r & 255u], __ATOMIC_RELAXED);
            fprintf(f, "%ld %c d%d %d v=%02x\n", now_ms(), (char) (e >> 24), (int) (e >> 16 & 0xff),
                    (int) (e >> 8 & 0xff), e & 0xffu);
        }
        if (f)
            fclose(f);
    } else
        g_sfxlog_r = __atomic_load_n(&g_sfxlog_w, __ATOMIC_ACQUIRE);
#endif
    for (int d = 0; d < AZMOD_STEM_DECKS; d++)
        __atomic_store_n(&g_sfx_led_on[d], mode ? g_sfx_on_fn(d) & 0xFu : 0u, __ATOMIC_RELAXED);
}

static void ov_sync_stem_volume(void) {
    /* Stem Volume (mod 32): the one sender to azstem_stem_eq. The bus thread only decodes
     * (g_stemeq_knobs). Sending from here makes switching the mod off by touch take
     * effect at once; the mixer bus is event-driven and may send no frame for minutes.
     * Mod off, or a channel not moved since start, means unity. */
    if (g_stem_eq_fn) {
        static int sent[2] = {-2, -2};
        int seq_on = az_on(AZF_STEM_EQ) && az_active(AZF_STEM_EQ);
        /* Channels 3/4 in use, by deck 3 or 4 or by PRO DJ LINK player 3 or 4: they own that
         * EQ. Stems go back to unity on both channels until both are free. Unknown (no azstem
         * export, no member watch) stands down too; never guess "free" (link_members.h). */
        static int busy_last = -1;
        int busy = stemvol_stand_down(g_deck_has_track_fn != NULL,
                                      g_deck_has_track_fn && g_deck_has_track_fn(2),
                                      g_deck_has_track_fn && g_deck_has_track_fn(3), lm_watch_ok(),
                                      lm_players());
        if (busy != busy_last) {
            busy_last = busy;
            FILE *bf = azlog_open("/tmp/azmod-bt.log", "a");
            if (bf) {
                fprintf(bf, "[%ld] stem volume: channels 3/4 %s (link players %llx)\n",
                        (long) time(NULL),
                        busy ? "in use (or unknown) -> EQ is stock"
                             : "free -> CH3/CH4 EQ set stem volumes",
                        (unsigned long long) lm_players());
                fclose(bf);
            }
        }
        if (busy)
            seq_on = 0;
        for (int ch = 0; ch < 2; ch++) {
            int want = seq_on ? __atomic_load_n(&g_stemeq_knobs[ch], __ATOMIC_RELAXED) : -1;
            if (want == sent[ch])
                continue;
            sent[ch] = want;
            if (want < 0)
                g_stem_eq_fn(ch, -1, -1, -1);
            else
                g_stem_eq_fn(ch, (want >> 20) & 1023, (want >> 10) & 1023, want & 1023);
            __atomic_store_n(&g_stemeq_tlm[ch], want, __ATOMIC_RELAXED);
        }
    }
}

static void ov_feed_drum_bpm(void) {
    /* Per-deck live BPM to azstem, so the roll runs at the track's tempo instead of a
     * fixed 120.
     *  - This thread only, every 8th tick (a few per second): BPM is low-rate, and
     *    nothing here may touch the render or audio paths.
     *  - Change-only: an unchanged BPM sends nothing.
     *  - bpmstate_read (stems_display.c): the BpmState is pinned by a weak reference
     *    taken in its ctor, and a read counts only while its use count is above zero,
     *    so a torn-down BpmState reads as unknown, never as freed memory.
     *  - EP147 spells "no track" as DBL_MAX here, which would overflow azstem's 16.16
     *    fixed point. drum_feed_bpm sends an empty deck the MASTER deck's BPM, and
     *    anything else outside 20..400 BPM as 0.0f, drumroll.c's "unknown" (120).
     *  - The same empty deck is told to follow the master (drum_feed_lead): its roll is then
     *    placed on the master's beat grid, at the master's playhead. */
    static unsigned btk = 0;
    static float last_bpm[AZMOD_STEM_DECKS] = {0};
    static int last_lead[AZMOD_STEM_DECKS] = {[0 ... AZMOD_STEM_DECKS - 1] = -1};
    if (g_drum_bpm_fn && (btk++ & 7u) == 0) {
        double live[AZMOD_STEM_DECKS];
        int ok[AZMOD_STEM_DECKS] = {0};
        for (int d = 0; d < AZMOD_STEM_DECKS; d++)
            ok[d] = bpmstate_read(d, &live[d], NULL); /* 0: unknown, send nothing */
        int m = __atomic_load_n(&g_master_deck, __ATOMIC_RELAXED);
        int m_ok = m >= 0 && m < AZMOD_STEM_DECKS && ok[m];
        for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
            if (!ok[d])
                continue;
            float bpm = drum_feed_bpm(d, live[d], m, m_ok, m_ok ? live[m] : 0.0);
            int lead = drum_feed_lead(d, live[d], m, m_ok, m_ok ? live[m] : 0.0);
            if (g_drum_follow_fn && lead != last_lead[d]) {
                last_lead[d] = lead;
                g_drum_follow_fn(d, lead);
                FILE *lf = azlog_open("/tmp/azmod-drumbpm.log", "a");
                if (lf) {
                    if (lead < 0)
                        fprintf(lf, "follow: deck%d -> its own grid\n", d + 1);
                    else
                        fprintf(lf, "follow: deck%d -> deck%d (master)\n", d + 1, lead + 1);
                    fclose(lf);
                }
            }
            if (bpm != last_bpm[d]) {
                last_bpm[d] = bpm;
                g_drum_bpm_fn(d, bpm);
                FILE *bf = azlog_open("/tmp/azmod-drumbpm.log", "a");
                if (bf) {
                    fprintf(bf,
                            "feed: deck%d(ld=%d) live=%.6g master=deck%d -> "
                            "azstem_drum_bpm(%d, %.4f)%s\n",
                            d + 1, d, live[d], m + 1, d, bpm,
                            bpm == 0.0f          ? "  [-> drumroll 120 fallback]"
                            : live[d] == DBL_MAX ? "  [no track -> master's BPM]"
                                                 : "");
                    fclose(bf);
                } /* change-only, flushed every line */
            }
        }
    }
}

/* Drum Roll and Stem Volume: publish the arm to the bus interposer and the selected bank, beat
 * sync and per-deck BPM to azstem. */
static void ov_sync_drum_roll(void) {
    static int last_drum = -1, last_bank = -1;
    ov_resolve_drum_exports();
    ov_sync_stem_volume();
    ov_sync_stem_fx();
    int on = az_on(AZF_DRUM_ROLL) ? 1 : 0;
    if (on != last_drum) {
        last_drum = on;
        __atomic_store_n(&g_drum_arm, on, __ATOMIC_RELEASE);
        if (!on)
            drum_disarm_all("mod off"); /* disarm: kill any roll */
        dbg("drumroll: %s\n", on ? "ARMED" : "off");
    }
    /* Flag a stick with no samples; otherwise an empty bank arms, lights and displays
     * exactly like a loaded one and plays nothing. */
    g_state.drum_no_samples = g_drum_avail_fn ? (g_drum_avail_fn() == 0) : false;

    /* The live bank: one CHOICE, always a bank (features.def; the settings load maps an
     * old file's radio slots onto it: first on wins, none = Bank 1). */
    int bank = az_get(AZF_DRUM_BANK) + 1;
    /* the bank is loaded only while Drum Roll is on (0 = off: azstem keeps the stick's
     * files closed, so a USB STOP never waits on them) */
    int want_bank = on ? bank : 0;
    if (want_bank != last_bank) {
        last_bank = want_bank;
        if (g_drum_bank_fn)
            g_drum_bank_fn(want_bank);
        dbg("drumroll: bank %d\n", want_bank);
    }

    /* Beat Sync (mod 11): roll hits lock to the track's beat grid instead of free-running
     * from the pad press. An independent toggle under DRUM ROLL. Change-only like the
     * arm and bank, with last_sync starting at -1 so the persisted state is published
     * once at startup rather than relying on azstem's power-on default (sync off, which
     * matches mod 11's state_init default). Without the azstem_drum_sync export the row
     * still toggles and persists; it sends nothing. */
    static int last_sync = -1;
    int sync = az_on(AZF_BEAT_SYNC) ? 1 : 0;
    if (sync != last_sync) {
        last_sync = sync;
        if (g_drum_sync_fn)
            g_drum_sync_fn(sync);
        dbg("drumroll: beat sync %d%s\n", sync,
            g_drum_sync_fn ? "" : " (no azstem_drum_sync export)");
    }
    ov_feed_drum_bpm();
}

/* 60 FPS: publish the waveform switch, and arm or disarm the 60 Hz UI clock when it changes. */
static void ov_sync_ui60(void) {
    __atomic_store_n(&g_wave60_on, az_on(AZF_FPS60_WAVEFORMS) ? 1 : 0,
                     __ATOMIC_RELAXED);
    static int last_ui60 = -1;
    if (ui60_clock_wanted(&g_state) != last_ui60) {
        last_ui60 = ui60_clock_wanted(&g_state);
        /* Arm on the toggle, not only at startup: the 60 FPS path is also gated on
         * /tmp/azmod-disp60, which load_config writes only from a persisted "on", so a
         * first enable from the menu would otherwise do nothing until a reload. The flag
         * lives in tmpfs, so a reboot still disarms it. */
        if (last_ui60) {
            FILE *af = fopen("/tmp/azmod-disp60", "we");
            if (af)
                fclose(af);
            __atomic_store_n(&g_disp60_armed, 1, __ATOMIC_RELAXED);
        }
        apply_ui60(last_ui60);
    }
}

/* One deck's stem pads: whether they are engaged, their LEDs, and pad presses (toggle,
 * mute-all, solo) applied to its stem mask. */
static void ov_sync_deck_pads(struct overlay_ctx *c, int d) {
    /* Engaged = the STEMS master is on and this deck's loaded track has stems. Not gated
     * on the deck screen: browsing for the next track does not stop the current one, so
     * the pads stay stem-mapped and stem-colored. A master-on deck without stems
     * publishes stemless instead: native pad routing, dark pads and a native orange
     * BEAT LOOP button as the indicator. */
    int engaged = azmod_stems_engine(&g_state) && g_state.stems_available[d];
    __atomic_store_n(&g_src_visible, g_state.source_visible ? 1 : 0,
                     __ATOMIC_RELAXED); /* for the knob-boundary interposer */
    __atomic_store_n(&g_erp_active[d], engaged, __ATOMIC_RELAXED);
    __atomic_store_n(&g_erp_stemless[d],
                     azmod_stems_engine(&g_state) && !g_state.stems_available[d],
                     __ATOMIC_RELAXED);
    {
        unsigned mask = 0; /* the jog display's copy (jog_display.c) */
        for (int i = 0; i < AZMOD_STEM_COUNT && i < 4; i++)
            if (g_state.stem_on[d][i])
                mask |= 1u << i;
        __atomic_store_n(&g_jog_src[d], jog_word(engaged, 1, mask), __ATOMIC_RELAXED);
    }
    {
        unsigned char blk[32];
        if (engaged)
            build_pad_led(&g_state, d, blk);
        else
            memset(blk, 0, sizeof blk);   /* no stems -> pads dark */
        seqblk_write(&g_pad_led[d], blk); /* the bus copies it whole or not at all */
    }
    stem_tiles_apply(d); /* native tile art follows stem_on[] */
    unsigned req = __atomic_exchange_n(&g_pad_req[d], 0, __ATOMIC_ACQ_REL); /* pads 1-4 */
    if (req) {
        for (int p = 0; p < AZMOD_STEM_COUNT; p++) {
            if (!(req & (1u << p)))
                continue;
            if (PAD_TGL[p] == PAD_MUTEALL) { /* pad4 MUTE/SOLO: silence all / restore */
                int anyon = 0;
                for (int k = 0; k < AZMOD_STEM_COUNT; k++)
                    anyon |= g_state.stem_on[d][k];
                for (int k = 0; k < AZMOD_STEM_COUNT; k++)
                    g_state.stem_on[d][k] = anyon ? false : true;
            } else { /* toggle the pad's stem(s) */
                for (int k = 0; k < AZMOD_STEM_COUNT; k++)
                    if (PAD_TGL[p] & (1u << k))
                        g_state.stem_on[d][k] = !g_state.stem_on[d][k];
            }
        }
        write_stems(&g_state);
        c->dirty = 1;
    }
    /* Diagnostic: `echo "<deck> <mask>" > /tmp/azmod-stemmask` sets a deck's stem mask
     * (bit k = stem k on) exactly as the pads do. The emulator's scripted pad presses
     * reach EP147 but not this toggle path. Read only on deck 0's pass. */
    if (d == 0 && AZ_PROBE_ON("/tmp/azmod-stemmask")) {
        FILE *mf = fopen("/tmp/azmod-stemmask", "re");
        int md = -1;
        unsigned mm = 0;
        if (mf) {
            if (fscanf(mf, "%d %u", &md, &mm) != 2)
                md = -1;
            fclose(mf);
        }
        unlink("/tmp/azmod-stemmask");
        if (md >= 0 && md < AZMOD_STEM_DECKS) {
            for (int k = 0; k < AZMOD_STEM_COUNT; k++)
                g_state.stem_on[md][k] = (mm >> k) & 1u;
            write_stems(&g_state);
            c->dirty = 1;
        }
    }
    unsigned solo =
        __atomic_exchange_n(&g_pad_solo_req[d], 0, __ATOMIC_ACQ_REL); /* pads 5-8: solo */
    if (solo) {
        int i = __builtin_ctz(solo); /* lowest requested stem */
        int only_i = g_state.stem_on[d][i];
        for (int k = 0; k < AZMOD_STEM_COUNT; k++)
            if (k != i && g_state.stem_on[d][k])
                only_i = 0;
        for (int k = 0; k < AZMOD_STEM_COUNT; k++)
            g_state.stem_on[d][k] = only_i ? true : (k == i);
        write_stems(&g_state); /* solo i, or un-solo (all on) if already solo */
        c->dirty = 1;
    }
    /* A stem the track does not have (missing file, failed decode) is never on: the tile
     * shows off, the LED is dark, and a press does nothing. */
    g_stem_pm[d] = g_stem_present_fn ? g_stem_present_fn(d) & 0xFu : 0;
    if (g_stem_pm[d]) {
        int ch = 0;
        for (int k = 0; k < AZMOD_STEM_COUNT; k++)
            if (!(g_stem_pm[d] >> k & 1u) && g_state.stem_on[d][k]) {
                g_state.stem_on[d][k] = false;
                ch = 1;
            }
        if (ch) {
            write_stems(&g_state);
            c->dirty = 1;
        }
    }
}

/* Every deck's stem pads, each tick. */
static void ov_sync_stem_pads(struct overlay_ctx *c) {
    for (int d = 0; d < AZMOD_STEM_DECKS; d++)
        ov_sync_deck_pads(c, d);
}


/* RELOAD: once the tap's press flash is over, show the banner and restart EP147. */
static void ov_fire_reload(struct overlay_ctx *c) {
    /* Fire the reload once the press flash has finished, so the tap is visibly felt.
     * Restarting EP147.service loads the mods fresh from disk. */
    if (!c->pending_reload || g_state.pressed_item >= 0)
        return;
    c->pending_reload = 0;
    /* Hold RELOADING on screen for ~1 s, re-raising every 50 ms so EP147's repaints
     * cannot bury it, then daemon-reload and restart. */
    for (int k = 0; k < 20; k++) {
        ov_shm_wait(c); /* the banner draws into the buffer */
        show_reloading(c->dpy, c->menu.id, c->gc, c->img, c->buf, c->scr_w, c->scr_h);
        usleep(50000);
    }
    /* daemon-reload first. The boot hooks touch EP147.service's drop-ins after systemd
     * loaded them (NeedDaemonReload=yes), and then a bare `systemctl restart` deadlocks
     * behind `sysinit.target start`: EP147 never returns, black screen, no WiFi, dead
     * power button, power-cycle only.
     * The pair runs on its own thread (az_reload_run), exactly as the backgrounded
     * `daemon-reload && restart` did: restart only after daemon-reload exits 0, and this
     * loop never waits on either. Both start with posix_spawn: no fork, no shell. */
    {
        pthread_t th;
        pthread_attr_t at;
        int started = 0;
        if (pthread_attr_init(&at) == 0) {
            started = pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED) == 0 &&
                      az_thread_create(&th, &at, az_reload_run, NULL) == 0;
            pthread_attr_destroy(&at);
        }
        if (!started)
            dbg("reload: no thread for systemctl (%s)\n", strerror(errno));
    }
    /* Keep RELOADING up until EP147 is torn down; going back to the loop would draw the
     * still-open menu over the banner for the seconds EP147 takes to stop. This thread
     * dies with the process; the 30 s bound only matters if the restart never happens. */
    for (int k = 0; k < 600; k++) {
        ov_shm_wait(c); /* the banner draws into the buffer */
        show_reloading(c->dpy, c->menu.id, c->gc, c->img, c->buf, c->scr_w, c->scr_h);
        usleep(50000);
    }
    c->dirty = 1;
}

/* The native MOD MENU source row's focus states: hover shows the docked preview, a tap
 * focuses it, leaving the row hides it. */
static void ov_srcrow_focus(struct overlay_ctx *c) {
    /* The native SOURCE-list "MOD MENU" row, a two-pane focus state machine:
     *  HOVER (g_srcrow_want) -> PREVIEW: menu docked on the right, the knob stays with
     *  EP147's left list. CLICK (g_srcrow_open) -> FOCUSED: azmod takes the knob.
     *  Selection leaves our row (want cleared) while not focused -> hide. */
    int mm_want = __atomic_load_n(&g_srcrow_want, __ATOMIC_RELAXED);
    int resumed = 0; /* this tick's open brought back a resumed page: the focus below keeps it */
    /* PREVIEW opens only on the SOURCE screen. g_srcrow_want mirrors EP147's source-list
     * selection, and that selection outlives the screen: every later refresh of the
     * list (USB STOP, a library write, a device change) runs src_rows_hook's RESYNC,
     * finds our row selected and sets want=1 on whatever screen is up. Without the
     * screen check the menu would open over Browse or the deck screen and be shut by
     * the visibility gate, over and over while the list keeps refreshing.
     *
     * want stays an honest mirror (returning to SOURCE with MOD MENU still selected
     * shows the menu); only the open waits for the screen. The screen buttons clear
     * source_visible immediately ("left via a screen button"). */
    if (mm_want && g_state.mode == AZ_CLOSED &&
        g_state.source_visible) { /* PREVIEW: show on hover */
        azmod_set_docked(true);
        resumed = ov_menu_open_page();
        c->dirty = 1;
        __atomic_store_n(&g_srcrow_menu, 1, __ATOMIC_RELEASE); /* native item rows exist */
    }
    if (__atomic_exchange_n(&g_srcrow_unfocus, 0, __ATOMIC_ACQ_REL)) {
        /* Gray row tapped: cursor back to it. Stay in PREVIEW (want kept, menu docked, no
         * focus) and collapse any submenu to the top. The bus thread's ownership test
         * (focused && menu_open) now fails, so the knob returns to EP147's left list. */
        if (g_state.mode != AZ_CLOSED)
            azmod_menu_reset(&g_state);
        __atomic_store_n(&g_menu_focused, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_menu_top, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&g_srcrow_repaint, 1, __ATOMIC_RELEASE);
        c->dirty = 1;
    }
    if (__atomic_exchange_n(&g_srcrow_open, 0, __ATOMIC_ACQ_REL)) {
        if (g_state.mode == AZ_CLOSED) {
            azmod_set_docked(true);
            g_state.mode = AZ_OPEN;
        } /* TAP opens a closed docked menu */
        __atomic_store_n(&g_srcrow_menu, 1, __ATOMIC_RELEASE); /* native item rows exist */
        __atomic_store_n(
            &g_menu_focused, 1,
            __ATOMIC_RELAXED); /* FOCUSED so hover-hide (below) cannot close it */
        __atomic_store_n(&g_srcrow_repaint, 1,
                         __ATOMIC_RELEASE); /* and EP147 must re-render our row */
        /* Drop any pending close. Open and close drain in the same tick, open first, so a
         * close raised before this open (selecting a real source sets g_srcrow_close)
         * would fire just below and shut the menu within one frame, before anything
         * renders. Such a close refers to a state this open replaced, so it is stale; a
         * close raised after still arrives on the next tick. */
        __atomic_store_n(&g_srcrow_close, 0, __ATOMIC_RELAXED);
        if (!resumed)
            ov_menu_open_page();
        c->dirty = 1;
        /* Open-path trace (azmod-srcrow.log): what the overlay decided (mode,
         * source_visible, the wanted_rect() result, mapped). It records that this path
         * ran, not that anything appeared on screen. Budgeted, overlay thread only. */
        {
            static _Atomic int oplog = 20;
            if (__atomic_fetch_sub(&oplog, 1, __ATOMIC_RELAXED) > 0 &&
                AZ_PROBE_ON("/home/root/settings/azmod-srcrow.log")) {
                int rx, ry, rw, rh;
                wanted_rect(&g_fb, g_state.mode, g_state.source_visible, 0, &rx, &ry, &rw,
                            &rh);
                FILE *lf = azlog_open("/tmp/azmod-srcrow.log", "a");
                if (lf) {
                    fprintf(lf,
                            "OPEN: drain#%u mode=%d srcvis=%d docked_rect=%d,%d %dx%d "
                            "mapped=%d fb=%dx%d\n",
                            __atomic_load_n(&g_drain_calls, __ATOMIC_RELAXED),
                            (int) g_state.mode, g_state.source_visible, rx, ry, rw, rh,
                            c->menu.mapped, g_fb.w, g_fb.h);
                    fclose(lf);
                }
            }
        }
    }
    int mm_focused = __atomic_load_n(&g_menu_focused, __ATOMIC_RELAXED);
    if (!mm_want && !mm_focused && azmod_docked() && g_state.mode != AZ_CLOSED) {
        ov_menu_close();
        c->dirty = 1; /* hover off + not focused -> hide */
    }
    if (__atomic_exchange_n(&g_srcrow_close, 0, __ATOMIC_ACQ_REL) && azmod_docked() &&
        g_state.mode != AZ_CLOSED) {
        ov_menu_close();
        __atomic_store_n(&g_menu_focused, 0, __ATOMIC_RELAXED);
        c->dirty = 1;
    }
    if (g_state.mode == AZ_CLOSED && azmod_docked()) {
        azmod_set_docked(false);
        __atomic_store_n(&g_menu_focused, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_srcrow_want, 0, __ATOMIC_RELEASE);
    }
}

/* Close the docked menu if EP147 left the SOURCE view on its own. */
static void ov_docked_gate(struct overlay_ctx *c) {
    /* Visibility gate: the docked menu exists only on the SOURCE view while MOD MENU is the
     * selected source. Event closes (screen tabs, source taps, SEL) cover the known paths;
     * this covers EP147 changing view on its own. detect_source() reads only the top-left
     * title (22,16 160x40), which the docked pane never covers. Two consecutive misses
     * (~300 ms) are required so a transition frame cannot shut a menu in use. */
    if (g_state.mode == AZ_CLOSED || !azmod_docked())
        return;
    static int gate_miss, gate_last = -1; /* consecutive misses, the last sample */
    static unsigned gate_tick;
    int want = __atomic_load_n(&g_srcrow_want, __ATOMIC_RELAXED);
    if ((++gate_tick % 5) == 0) {
        int src = detect_waveform(c->dpy, c->root) ? 0 : detect_source(c->dpy, c->root);
        gate_miss = src ? 0 : gate_miss + 1;
        if (src != gate_last && AZ_PROBE_ON("/home/root/settings/azmod-srcrow.log")) {
            FILE *lf = azlog_open("/tmp/azmod-srcrow.log", "a");
            if (lf) {
                fprintf(lf, "GATE: open src=%d want=%d miss=%d mode=%d\n", src, want,
                        gate_miss, (int) g_state.mode);
                fclose(lf);
            }
        }
        gate_last = src;
    }
    if (!want || gate_miss >= 2) {
        if (AZ_PROBE_ON("/home/root/settings/azmod-srcrow.log")) {
            FILE *lf = azlog_open("/tmp/azmod-srcrow.log", "a");
            if (lf) {
                fprintf(lf, "GATE: CLOSE want=%d miss=%d\n", want, gate_miss);
                fclose(lf);
            }
        }
        ov_menu_close();
        menu_drop_intent();
        azmod_set_docked(false);
        gate_miss = 0;
        gate_last = -1;
        c->dirty = 1;
    }
}

/* Track whether the deck screen is up, and whether it shows 2 or 4 decks. */
static void ov_detect_deck_view(struct overlay_ctx *c) {
    /* detect_waveform() is an atomic read of the paint timestamp, so it runs every tick and
     * in every mode. It must not sit under the menu-closed guard (that guard exists for
     * detect_source()'s screen read): SOURCE can re-open the docked menu before the paint
     * timeout clears, and waveform_visible would then stay 1 while the menu is open, leaving
     * every deck-view surface (readouts, stem strip) drawn over SOURCE. detect_4deck's screen
     * read still runs only while the deck view is up. */
    int wv = detect_waveform(c->dpy, c->root);
    if (wv != g_state.waveform_visible) {
        g_state.waveform_visible = wv;
        c->dirty = 1;
        ov_forget_all(); /* deck view came or went: those widgets may be gone */
        if (wv)
            g_four_deck = detect_4deck(c->dpy, c->root);
        g_state.four_deck = g_four_deck;
    }
    /* The layout can change while the deck view stays up (the tabs are right there), so
     * re-sample periodically: one small XGetImage, never a per-frame grab, which stalls
     * EP147. */
    if (g_state.waveform_visible && (c->det_tick % 30) == 0) {
        int f4 = detect_4deck(c->dpy, c->root);
        if (f4 != g_four_deck) {
            g_four_deck = f4;
            g_state.four_deck = f4;
            c->dirty = 1;
        }
    }
}

/* Follow track loads and ejects: which stems each deck has, and a fresh stem mask per track. */
static void ov_track_stems(struct overlay_ctx *c) {
    /* Stem state tracking runs in every mode and must not inherit the menu-closed guard
     * below (that guard is for detect_source()'s readback). Under it, with the menu open, an
     * eject left the deck's mask stale and a reload of the same track kept the previous
     * track's stem mutes. Own counter, because det_tick only advances while closed. */
    if ((++c->stem_tick % 10) == 0) { /* 8 small file reads per 10 ticks (every 100-300 ms) */
        if (read_avail(&g_state)) {
            write_stems(&g_state);
            c->dirty = 1;
        } /* per-deck strip visibility follows loaded track */
        if (reset_stems_on_track_change(&g_state))
            c->dirty = 1; /* new track -> that deck's toggles back to all-on */
    }
}

/* SOURCE detection while the docked menu is up. */
static void ov_detect_source_docked(struct overlay_ctx *c) {
    /* SOURCE detection while the menu is docked. The block below runs only while CLOSED, so
     * without this nothing sets source_visible back to 1 with the docked preview up, and the
     * row's AZ icon (the srcicon window) stays hidden. The CLOSED gate exists so a full-screen menu is never
     * sampled as EP147's title; the docked pane is x>=640, y>=70 and the title sample is
     * x 22..182, y 16..56, so docked is safe (the same condition the icon uses). */
    if (g_state.mode == AZ_CLOSED || !azmod_docked())
        return;
    static unsigned det_tick_docked;
    if ((++det_tick_docked % 5) == 0) {
        int vis = g_state.waveform_visible ? 0 : detect_source(c->dpy, c->root);
        if (vis != g_state.source_visible) {
            g_state.source_visible = vis;
            write_stems(&g_state);
            c->dirty = 1;
        }
    }
}

/* The title row's surfaces: the NEW PLAYLIST naming bar and the SELECT PLAYLIST banner. */
static void ov_sync_title_row(struct overlay_ctx *c) {
    int nm = azmod_naming_playlist(); /* NEW PLAYLIST naming: the title-row bar */
    if ((bool) nm != g_state.nm_visible) {
        g_state.nm_visible = nm;
        c->dirty = 1;
        c->need_raise = 1;
    }
    if (azmod_naming_dirty())
        c->dirty = 1;
    /* SELECT PLAYLIST banner: where it may show (pl_tb_tick), DONE only over it */
    int ok = !g_state.waveform_visible && !g_state.source_visible &&
             !__atomic_load_n(&g_search_on, __ATOMIC_RELAXED) &&
             !g_state.nm_visible; /* NEW PLAYLIST naming owns the title row */
    __atomic_store_n(&g_tb_screen_ok, ok, __ATOMIC_RELAXED);
    int up = ok && __atomic_load_n(&g_tb_up, __ATOMIC_RELAXED);
    if ((bool) up != g_state.tb_visible) {
        g_state.tb_visible = up;
        c->dirty = 1;
        c->need_raise = 1;
    }
    int um = __atomic_load_n(&g_tb_undo_mode, __ATOMIC_RELAXED) != 0;
    if ((bool) um != g_state.tb_undo) {
        g_state.tb_undo = um;
        c->dirty = 1;
        c->need_raise = 1;
    }
    int db = __atomic_load_n(&g_tb_done_btn, __ATOMIC_RELAXED) != 0;
    if ((bool) db != g_state.tb_done_btn) {
        g_state.tb_done_btn = db;
        c->dirty = 1;
        c->need_raise = 1;
    }
}

/* SOURCE detection and the title-row surfaces while the menu is closed, every 5th tick. */
static void ov_detect_source_closed(struct overlay_ctx *c) {
    if (g_state.mode != AZ_CLOSED || (++c->det_tick % 5) != 0)
        return;
    /* detect_source() XGetImages the title area, a synchronous readback that stalls the
     * X server. Skip it while the deck view is up: we cannot be on SOURCE then, and that
     * is where the deck spends most of its time. */
    int vis = g_state.waveform_visible ? 0 : detect_source(c->dpy, c->root);
    if (vis && now_ms() - c->left_src_ms < 300)
        vis = 0; /* just left SOURCE by a screen button: the old title may linger */
    if (vis != g_state.source_visible) {
        g_state.source_visible = vis;
        write_stems(&g_state);
        c->dirty = 1;
        if (AZ_PROBE_ON("/home/root/settings/azmod-srcrow.log")) { /* the gate's signal,
                                                                      seen while closed */
            FILE *lf = azlog_open("/tmp/azmod-srcrow.log", "a");
            if (lf) {
                fprintf(lf, "GATE: closed src=%d wave=%d\n", vis, g_state.waveform_visible);
                fclose(lf);
            }
        }
    }
    ov_sync_title_row(c);
}

/* One second's counts behind the rates log. */
struct ov_rate_window {
    long paint, ui, disp; /* EP147 paints, repaint-timer ticks and display refreshes */
    long window_ms;       /* the window they were counted over, about 1000 ms */
    double secs;          /* window_ms in seconds, for per-second rates */
};

/* The rates log: display and 60 FPS state, composite cost. */
static void ov_rates_display(FILE *rf, const struct ov_rate_window *d) {
    fprintf(rf, "paint=%.1f/s over %ldms (raw %ld)\n", d->paint / d->secs,
            d->window_ms, d->paint);
    fprintf(rf,
            "paint=%ld/s repaintTimer=%ld/s dispRefresh=%ld/s wv=%d\n"
            "disp: period_seen=%d want=%d armed=%d ui60=%d(1 clock, 2 wave) "
            "self=..%06x\n",
            d->paint, d->ui, d->disp, g_state.waveform_visible,
            __atomic_load_n(&g_disp_seen, __ATOMIC_RELAXED),
            __atomic_load_n(&g_disp_want, __ATOMIC_RELAXED),
            __atomic_load_n(&g_disp_armed, __ATOMIC_RELAXED),
            __atomic_load_n(&g_ui60_on, __ATOMIC_RELAXED) |
                (__atomic_load_n(&g_wave60_on, __ATOMIC_RELAXED) << 1),
            (unsigned) __atomic_load_n(&g_disp_self_lo, __ATOMIC_RELAXED));
    {
        long raw = __atomic_load_n(&g_mdut_fi_raw, __ATOMIC_RELAXED);
        double fiv;
        memcpy(&fiv, &raw, 8);
        fprintf(rf, "mdut: self=0x%lx fi=%g\n",
                __atomic_load_n(&g_mdut_self, __ATOMIC_RELAXED), fiv);
    }
    {
        static long pp;
        long p = __atomic_load_n(&g_disp_pass, __ATOMIC_RELAXED);
        fprintf(rf, "ui60: waveform limiter passed %ld/s\n", p - pp);
        pp = p;
    }
    {
        static long pf, pq;
        long f = __atomic_load_n(&g_feed_frame_n, __ATOMIC_RELAXED),
             q = __atomic_load_n(&g_feed_pass_n, __ATOMIC_RELAXED);
        fprintf(rf, "ui60: feed lock %s, frame runs %ld/s, own-timer pass %ld/s\n",
                feedlock_on() ? "on" : "off", f - pf, q - pq);
        pf = f;
        pq = q;
    }
    fprintf(rf,
            "ui60: clock=%d wave=%d scroll60=%d ui-start-parts=%d  (mod4=%d "
            "mod19=%d)\n",
            __atomic_load_n(&g_ui60_on, __ATOMIC_RELAXED),
            __atomic_load_n(&g_wave60_on, __ATOMIC_RELAXED), scroll60_on(),
            g_ui60ui_installed, az_on(AZF_FPS60_WAVEFORMS),
            az_on(AZF_FPS60_UI));
    {
        long cn = __atomic_exchange_n(&g_comp_n, 0, __ATOMIC_RELAXED),
             cu = __atomic_exchange_n(&g_comp_us, 0, __ATOMIC_RELAXED),
             cm = __atomic_exchange_n(&g_comp_max, 0, __ATOMIC_RELAXED);
        fprintf(rf, "composite: %ld frames, avg %ld us, max %ld us\n", cn,
                cn ? cu / cn : 0, cm);
    }
    fprintf(rf, "frame interval = %ld us\n",
            __atomic_load_n(&g_tick_iv, __ATOMIC_RELAXED));
}

/* The rates log: Drum Roll tone, Stem Volume, pad and mode-button colors, BEAT LOOP paging,
 * brightened grid lines. */
static void ov_rates_pads(FILE *rf) {
    static long pgc = 0, pns = 0;
    long gaa_calls = __atomic_load_n(&g_gridaa_calls, __ATOMIC_RELAXED);
    long gn = __atomic_load_n(&g_gridaa_ns, __ATOMIC_RELAXED);
    {
        int tt = __atomic_load_n(&g_mic2_tone_tlm, __ATOMIC_RELAXED);
        fprintf(rf, "drumtone: fn=%d mic2 hi=%d mid=%d\n",
                g_drum_tone_fn != NULL, tt < 0 ? -1 : tt >> 16,
                tt < 0 ? -1 : tt & 0xffff);
    }
    {
        int ch3 = __atomic_load_n(&g_stemeq_tlm[0], __ATOMIC_RELAXED),
            ch4 = __atomic_load_n(&g_stemeq_tlm[1], __ATOMIC_RELAXED);
        fprintf(
            rf,
            "stemeq: fn=%d mod=%d ch3 %d/%d/%d ch4 %d/%d/%d (hi/mid/low, "
            "-1 = unity)\n",
            g_stem_eq_fn != NULL, az_on(AZF_STEM_EQ),
            ch3 < 0 ? -1 : (ch3 >> 20) & 1023, ch3 < 0 ? -1 : (ch3 >> 10) & 1023,
            ch3 < 0 ? -1 : ch3 & 1023, ch4 < 0 ? -1 : (ch4 >> 20) & 1023,
            ch4 < 0 ? -1 : (ch4 >> 10) & 1023, ch4 < 0 ? -1 : ch4 & 1023);
    }
    fprintf(
        rf,
        "modecells side0: HC %02x%02x%02x BL %02x%02x%02x SL %02x%02x%02x "
        "BJ %02x%02x%02x\n",
        g_modecell_tlm[0][0], g_modecell_tlm[0][1], g_modecell_tlm[0][2],
        g_modecell_tlm[1][0], g_modecell_tlm[1][1], g_modecell_tlm[1][2],
        g_modecell_tlm[2][0], g_modecell_tlm[2][1], g_modecell_tlm[2][2],
        g_modecell_tlm[3][0], g_modecell_tlm[3][1], g_modecell_tlm[3][2]);
    {
        struct plsnap ps = plsnap_get();
        fprintf(
            rf,
            "plsel: hl_widget=%lx hl_sel=%d hl_rows=%d snap sel_pl=%u "
            "sel_row=%u pl=%u any_cid=%u lsfind_age=%lldms tl_showing=%d\n",
            __atomic_load_n(&g_hl_widget, __ATOMIC_RELAXED),
            __atomic_load_n(&g_hl_sel, __ATOMIC_RELAXED),
            __atomic_load_n(&g_hl_rows, __ATOMIC_RELAXED), ps.sel_pl,
            ps.sel_row, ps.playlist_id, ps.any_cid,
            drag_now_ms() - __atomic_load_n(&g_lsfind_ms, __ATOMIC_RELAXED),
            __atomic_load_n(&g_tl_showing, __ATOMIC_RELAXED));
    }
    fprintf(rf, "padcells side0:");
    for (int j = 0; j < 8; j++)
        fprintf(rf, " %02x%02x%02x", g_padcell_tlm[j][0],
                g_padcell_tlm[j][1], g_padcell_tlm[j][2]);
    fprintf(rf, "\n");
    fprintf(rf, "blswap: on=%d vp=%d %d %d %d\n", bl_swap(),
            __atomic_load_n(&g_bl_vpage2[0], __ATOMIC_RELAXED),
            __atomic_load_n(&g_bl_vpage2[1], __ATOMIC_RELAXED),
            __atomic_load_n(&g_bl_vpage2[2], __ATOMIC_RELAXED),
            __atomic_load_n(&g_bl_vpage2[3], __ATOMIC_RELAXED));
    fprintf(
        rf,
        "blbtn: bl2=%d side0 green=%d mode=%d  side1 green=%d mode=%d\n",
        stems_bl_mode() == 4,
        __atomic_load_n(&g_bl_btn_green[0], __ATOMIC_RELAXED),
        __atomic_load_n(&g_true_mode_deck[0], __ATOMIC_RELAXED) - 1,
        __atomic_load_n(&g_bl_btn_green[1], __ATOMIC_RELAXED),
        __atomic_load_n(&g_true_mode_deck[1], __ATOMIC_RELAXED) - 1);
    fprintf(rf,
            "slip34: state=%d drum=%d  (tile 8 native 3/4 while Drum Roll "
            "is off: 1 decoded, -1 refused: art not stock, 0 untried)\n",
            g_slip34_state, __atomic_load_n(&g_drum_arm, __ATOMIC_RELAXED));
    for (int sd = 0; sd < AZMOD_PAD_DECKS;
         sd++) { /* stems_page() inputs: tells "stale" and "down" apart
                    from "no" */
        int sl = azmod_logical_deck(sd, g_deck_sel[sd]);
        fprintf(rf,
                "stemspage side%d: deck%d page=%d drumslip=%d btn=%d tm=%d "
                "fresh=%d down=%d edge_age=%ldms\n",
                sd, sl + 1, stems_page(sd, sl), drum_mode_is_slip(sd, sl),
                g_pad_mode_bl[sd],
                __atomic_load_n(&g_true_mode_deck[sl], __ATOMIC_RELAXED) -
                    1,
                (int) (__atomic_load_n(&g_tm_seq[sl], __ATOMIC_ACQUIRE) -
                       __atomic_load_n(&g_mode_edge_seq[sd],
                                       __ATOMIC_ACQUIRE)) >= 0,
                __atomic_load_n(&g_mode_down[sd], __ATOMIC_RELAXED),
                now_ms() -
                    __atomic_load_n(&g_mode_edge_ms[sd], __ATOMIC_RELAXED));
    }
    {
        static long ps = 0, pb = 0;
        long st = __atomic_load_n(&g_bg_steps, __ATOMIC_RELAXED),
             bu = __atomic_load_n(&g_bg_builds, __ATOMIC_RELAXED);
        fprintf(rf,
                "brightgrid: gain=%.2f od=%.2f gray=%d move=%dms ramp=%dms "
                "steps=%ld/s builds=%ld/s\n",
                __atomic_load_n(&g_bg_gain_k, __ATOMIC_RELAXED) / 1000.0,
                __atomic_load_n(&g_bg_od_k, __ATOMIC_RELAXED) / 1000.0,
                __atomic_load_n(&g_bg_gray, __ATOMIC_RELAXED),
                __atomic_load_n(&g_bg_move_ms, __ATOMIC_RELAXED),
                __atomic_load_n(&g_bg_ramp_ms, __ATOMIC_RELAXED), st - ps,
                bu - pb);
        ps = st;
        pb = bu;
    }
    fprintf(
        rf,
        "gridaa: hook=%d brightened=%ld/s cost=%ldus/s rows=%ld total\n",
        g_gridaa_hooked, gaa_calls - pgc, (gn - pns) / 1000,
        __atomic_load_n(&g_gridaa_rows, __ATOMIC_RELAXED));
    pgc = gaa_calls;
    pns = gn;
}

/* The rates log: the waveform scaler's cost per path. */
static void ov_rates_wscale(FILE *rf) {
    static long pn[3], pr[3], pws = 0;
    long ws = __atomic_load_n(&g_ws_ns, __ATOMIC_RELAXED);
    fprintf(rf, "wscale: own=%ldus/s builds=%ld resync=%ld nodeck=%ld |",
            (ws - pws) / 1000,
            __atomic_load_n(&g_ws_builds, __ATOMIC_RELAXED),
            __atomic_load_n(&g_ws_resync, __ATOMIC_RELAXED),
            __atomic_load_n(&g_ws_nodeck, __ATOMIC_RELAXED));
    pws = ws;
    static const char *nm[3] = {"stock", "copy", "point"};
    for (int k = 0; k < 3; k++) {
        long n = __atomic_load_n(&g_ws_n[k], __ATOMIC_RELAXED),
             r = __atomic_load_n(&g_ws_rns[k], __ATOMIC_RELAXED);
        fprintf(rf, " %s %ld/s %ldus/s", nm[k], n - pn[k],
                (r - pr[k]) / 1000);
        pn[k] = n;
        pr[k] = r;
    }
    fprintf(rf, " | fast=%d cmp=%d", g_wp_fast, g_wp_cmp);
    for (int k = 0; k < 3; k++)
        fprintf(rf, " k%d %lu/%lu bad", k, g_wpc_bad[k], g_wpc_n[k]);
    fprintf(rf, "\n");
}

/* The rates log: what drives the display refresh. */
static void ov_rates_disp(FILE *rf) {
    fprintf(rf, "disp caller chain:");
    for (int k = 0; k < g_disp_btn; k++)
        fprintf(rf, " 0x%08lx", g_disp_bt[k]);
    fprintf(rf, "\ndisp ints (offset: value) -- hunting 33/30:\n");
    for (int k = 0; k < 40; k++) {
        int v = __atomic_load_n(&g_disp_ints[k], __ATOMIC_RELAXED);
        if (v > 0 && v < 1000)
            fprintf(rf, "  +0x%02x = %d%s\n", k * 4, v,
                    (v == 33 || v == 30 || v == 32 || v == 31)
                        ? "   <<< CANDIDATE"
                        : "");
    }
}

/* Once a second: paint and UI rates for the FPS readout, and the rates log (azmod-logs.on). */
static void ov_measure_rates(struct overlay_ctx *c) {
    static long pl = 0, prt = 0, pt = 0, pd = 0;
    long nowt = now_ms();
    if (nowt - pt < 1000)
        return;
    long np = __atomic_load_n(&g_paint_n, __ATOMIC_RELAXED);
    long nr = __atomic_load_n(&g_rtick_n, __ATOMIC_RELAXED);
    long nd = __atomic_load_n(&g_disp_n, __ATOMIC_RELAXED);
    FILE *rf = azlog_open("/tmp/azmod-rates", "w");
    /* Normalize to a real second: the window is 1000 ms plus up to one tick group,
     * so a raw count overstates the rate (61-62 for a 59.24 Hz panel). */
    double secs = (nowt - pt) > 0 ? (nowt - pt) / 1000.0 : 1.0;
    if (rf) {
        struct ov_rate_window w = {np - pl, nr - prt, nd - pd, nowt - pt, secs};
        ov_rates_display(rf, &w);
        ov_rates_pads(rf);
        ov_rates_wscale(rf);
        ov_rates_disp(rf);
        fclose(rf);
    }
    pd = nd;
    g_state.fps_paint = (int) ((np - pl) / secs + 0.5);
    g_state.fps_ui = (int) ((nr - prt) / secs + 0.5);
    if (azmod_stat_on(&g_state, AZF_OVERLAY_FPS))
        c->dirty = 1; /* repaint the readout */
    pl = np;
    prt = nr;
    pt = nowt;
}

/* Once a second: Fast UI Core, the row cache, the Bright Grid tuning file, and the Reload
 * row's "Reload to apply." */
static void ov_poll_1hz(struct overlay_ctx *c) {
    static long pt = 0;
    long nowt = now_ms(); /* Performance > Fast UI Core, 1 Hz */
    if (nowt - pt >= 1000) {
        pt = nowt;
        fastui_tick(&g_state, &c->dirty);
        rowcache_tick();
        brightgrid_poll_cfg();
        /* the loader holds a staged set back while a track is loaded (the restart policy in
         * az-usbload): the Reload row says "Reload to apply." */
        /* the loader handed over the mods stick's azmods.cfg: it wins, applied as a menu
         * change would be */
        if (settings_import_pending()) {
            load_config(&g_state);
            c->dirty = 1;
        }
        bool rp = access("/run/az-reload-pending", F_OK) == 0;
        if (rp != g_state.reload_pending) {
            g_state.reload_pending = rp;
            c->dirty = 1;
        }
    }
}

/* The OVERLAYS readouts' values: temperature, CPU use and clock, RAM. */
static void ov_poll_readouts(struct overlay_ctx *c) {
    /* The OVERLAYS readouts: once a second, each read only while shown, and a repaint only
     * when a shown value changes. */
    static long tt = 0;
    long nowt = now_ms();
    if (azmod_stats_any(&g_state) && nowt - tt >= 1000) {
        tt = nowt;
        if (azmod_stat_on(&g_state, AZF_OVERLAY_CPU_TEMP)) {
            int v = cpu_temp_read(), o = g_state.cpu_temp_c10;
            if ((v < 0) != (o < 0) || (v >= 0 && (v + 5) / 10 != (o + 5) / 10)) {
                g_state.cpu_temp_c10 = v;
                c->dirty = 1;
            }
        }
        if (azmod_stat_on(&g_state, AZF_OVERLAY_CPU_USAGE)) {
            int v = cpu_use_read();
            if (v != g_state.cpu_use_pct) {
                g_state.cpu_use_pct = v;
                c->dirty = 1;
            }
        }
        if (azmod_stat_on(&g_state, AZF_OVERLAY_CPU_CLOCK)) {
            bool th = false;
            int v = cpu_clk_read(&th);
            if (v / 100 != g_state.cpu_clk_mhz / 100 ||
                th != g_state.cpu_clk_throttled) {
                g_state.cpu_clk_mhz = v;
                g_state.cpu_clk_throttled = th;
                c->dirty = 1;
            }
        }
        if (azmod_stat_on(&g_state, AZF_OVERLAY_RAM)) {
            int v = ram_use_read();
            if (v != g_state.ram_use_pct) {
                g_state.ram_use_pct = v;
                c->dirty = 1;
            }
        }
    }
}

/* The Clock readout's text, once a second. It stays "" (drawn as --:--, "Not set" in the menu)
 * until the system clock is synchronized, by the rule timedatectl uses (systemd ntp_synced):
 * the kernel's maximum error, from adjtimex(), under 16 s. It is at that limit from boot (the
 * deck's RTC does not keep time through a power-off) until systemd-timesyncd's first NTP answer,
 * and again after any settimeofday(); without NTP updates it grows by 500 ppm and passes 16 s
 * after about 9 hours. A time set by hand (Set Time Manually) is shown too, as long as the
 * clock has not fallen back below it: the set leaves an empty file whose modification time is
 * the time set (CLOCK_SET_MARK), and a power-off that resets the RTC puts the clock before it
 * (clock_set.h cs_valid). NTP, when it answers, sets the time over it.
 * The zone file is read when the zone changes; a zone that cannot be read or answered
 * (clock_tz.h) also leaves the text empty. */
static void ov_clock_log(const char *fmt, ...) {
    FILE *f = azlog_open("/tmp/azmod-clock.log", "a");
    if (!f)
        return;
    fprintf(f, "[%ld] ", (long) time(NULL));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}
#define CLOCK_SET_MARK "/home/root/settings/azmod-clock-set"
/* Set Time Manually: the system time, the RTC (UTC, as the kernel reads it at boot) and the
 * marker; the time of the marker, or 0 when the time could not be set. */
static int64_t ov_clock_set(int64_t t) {
    struct timespec ts = {(time_t) t, 0};
    if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
        ov_clock_log("set time %lld: clock_settime failed (errno %d)\n", (long long) t, errno);
        return 0;
    }
    struct tm tm;
    time_t tt = (time_t) t;
    int rtc = -1, fd = open("/dev/rtc0", O_RDONLY | O_CLOEXEC);
    if (fd >= 0 && gmtime_r(&tt, &tm)) {
        struct rtc_time r;
        memset(&r, 0, sizeof r);
        r.tm_sec = tm.tm_sec, r.tm_min = tm.tm_min, r.tm_hour = tm.tm_hour;
        r.tm_mday = tm.tm_mday, r.tm_mon = tm.tm_mon, r.tm_year = tm.tm_year;
        rtc = ioctl(fd, RTC_SET_TIME, &r);
    }
    if (fd >= 0)
        close(fd);
    int64_t mark = 0;
    int mf = open(CLOCK_SET_MARK, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    struct stat st;
    if (mf >= 0) {
        if (futimens(mf, NULL) == 0 && fstat(mf, &st) == 0)
            mark = (int64_t) st.st_mtime;
        close(mf);
    }
    ov_clock_log("set time %lld: system ok, rtc %s, marker %lld\n", (long long) t, rtc == 0 ? "ok" : "failed",
                 (long long) mark);
    return mark ? mark : t;
}
static void ov_poll_clock(struct overlay_ctx *c) {
    static long tt;
    static int loaded = -1, zone_ok, last_sync = -1;
    static int64_t mark = -1;
    static struct ctz z;
    static unsigned char zbuf[1 << 16];
    int64_t req = __atomic_exchange_n(&g_azmod_clock_set_req, 0, __ATOMIC_ACQ_REL);
    if (mark < 0) { /* the last manual set, once per run */
        struct stat st;
        mark = stat(CLOCK_SET_MARK, &st) == 0 ? (int64_t) st.st_mtime : 0;
    }
    if (req)
        mark = ov_clock_set(req);
    long nowt = now_ms();
    if (!req && nowt - tt < 1000)
        return;
    tt = nowt;
    int k = __atomic_load_n(&g_azmod_clock_tz, __ATOMIC_RELAXED);
    if (k != loaded) {
        char p[160];
        loaded = k;
        zone_ok = 0;
        snprintf(p, sizeof p, "/usr/share/zoneinfo/%s", azmod_clock_tz_zone(k));
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t n = read(fd, zbuf, sizeof zbuf);
            close(fd);
            zone_ok = n > 0 && ctz_parse(zbuf, (size_t) n, &z) == 0;
        }
        ov_clock_log("zone %s: %s\n", azmod_clock_tz_zone(k), zone_ok ? "ok" : "unusable");
    }
    struct timex tx;
    memset(&tx, 0, sizeof tx);
    int st = adjtimex(&tx), sync = st >= 0 && tx.maxerror < 16000000L;
    if (sync != last_sync) {
        last_sync = sync;
        ov_clock_log("system clock %s (adjtimex %d, status 0x%x, maxerror %ld us)\n",
                     sync ? "synchronized" : "not synchronized", st, (unsigned) tx.status, (long) tx.maxerror);
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    char t[sizeof g_state.clock_text] = "";
    int32_t off;
    if (cs_valid(sync, mark, (int64_t) ts.tv_sec) && zone_ok && ctz_offset(&z, (int64_t) ts.tv_sec, &off) == 0) {
        ctz_format(t, sizeof t, (int64_t) ts.tv_sec, off, az_get(AZF_CLOCK_24H) == 1,
                   az_get(AZF_CLOCK_SECONDS) == 1);
        if (k == 0)
            strncat(t, " UTC", sizeof t - strlen(t) - 1);
    }
    if (strcmp(t, g_state.clock_text)) {
        memcpy(g_state.clock_text, t, sizeof t);
        c->dirty = 1;
    }
}

/* Once a second: EP147's paint rate to the debug log. */
static void ov_log_paint_rate(void) {
    static long pl = 0;
    static long pt = 0;
    long nowt = now_ms();
    if (nowt - pt >= 1000) {
        long n = __atomic_load_n(&g_paint_n, __ATOMIC_RELAXED);
        dbg("PAINTRATE %ld/s wave_vis=%d wv=%d src=%d\n", n - pl,
            __atomic_load_n(&g_wave_vis, __ATOMIC_RELAXED), g_state.waveform_visible,
            g_state.source_visible);
        pl = n;
        pt = nowt;
    }
}

/* The waveform hooks' inputs: azstem's stem exports, the mute and tint switches mirrored for
 * the hot path, the waveform providers and the zoom model. */
static void ov_sync_wave_hooks(void) {
    /* Per-stem waveform lanes: azstem.so owns the decoded PCM and publishes the envelopes;
     * we draw them (menu.c draw_stem_lane). Resolved here, not in our constructor, because
     * azstem.so is preloaded after us and is not yet in the global scope at ctor time. */
    if (!azmod_stem_wave)
        *(void **) &azmod_stem_wave = dlsym(RTLD_DEFAULT, "azstem_wave");
    /* Optional: without it the have-edge latch stays the invalidation signal. */
    if (!azmod_stem_env_gen)
        *(void **) &azmod_stem_env_gen = dlsym(RTLD_DEFAULT, "azstem_env_gen");
    /* Which stems each deck's track has (the stem pads' mask above). */
    if (!g_stem_present_fn)
        *(void **) &g_stem_present_fn = dlsym(RTLD_DEFAULT, "azstem_present");
    if (!g_stem_frames)
        *(void **) &g_stem_frames = dlsym(RTLD_DEFAULT, "azstem_frames");
    /* Waveform stem muting: on with the STEMS engine unless
     * /home/root/settings/azmod-wavemute.off exists. The LUT is 1024 entries per deck, so it
     * is rebuilt only when a mask or a track changes (below). */
    __atomic_store_n(&g_wavemute,
                     azmod_stems_engine(&g_state) &&
                         access("/home/root/settings/azmod-wavemute.off", F_OK) != 0,
                     __ATOMIC_RELAXED);
    /* The split-band debug view (azmod-wavesplit.on) compares our half with EP147's, so it
     * suppresses the tint itself; tinted, both halves use the stem palette. */
    __atomic_store_n(&g_wavecolor,
                     0 /* Stem Tint: retired slot 1, forced off (features.def) */ &&
                         !AZ_PROBE_ON("/home/root/settings/azmod-wavesplit.on"),
                     __ATOMIC_RELAXED);
    /* Hot-path mirrors, so the ~800k calls/s hook reads plain ints. */
    {
        static int last_gate = -1; /* Stems master / tint flipped: re-render */
        int gate = __atomic_load_n(&g_wavemute, __ATOMIC_RELAXED) |
                   (__atomic_load_n(&g_wavecolor, __ATOMIC_RELAXED) << 1);
        if (gate != last_gate) {
            if (last_gate >= 0) /* every deck's tiles */
                __atomic_fetch_or(&g_ov_refresh, (1 << AZMOD_STEM_DECKS) - 1, __ATOMIC_RELEASE);
            last_gate = gate;
        }
    }
    g_wave_scale = __atomic_load_n(&g_wavemute, __ATOMIC_RELAXED);
    g_wave_color = g_wave_scale && __atomic_load_n(&g_wavecolor, __ATOMIC_RELAXED);
    g_wave_lanes = 0; /* Stem Waves: retired slot 3, forced off (features.def) */
    g_wave_any = g_wave_scale || g_wave_color || g_wave_lanes;
    g_ws_forcepp = AZ_PROBE_ON("/tmp/azmod-wscale.pp");
    g_wave_noid = AZ_PROBE_ON("/tmp/azmod-waveid.off");
    wave_prov_report();
    {
        int lx, ly, lw, lh;
        azmod_waveband_rect(&g_fb, 0, &lx, &ly, &lw, &lh);
        g_ncol_hint = lw;
    }
    wave_zoom_tick();
}

/* Rebuild a deck's waveform attenuation table when its stem mask, its track or a Stem Volume
 * gain changes. Runs on every overlay tick, so a knob turn is followed at the tick rate; a gain
 * sweep is rate limited per deck (watt_should_build), and its overview tiles on a slower
 * cadence (watt_ov_step). A pad toggle or a track change rebuilds and refreshes on this tick;
 * a load's decode progress alone rebuilds at most every WATT_PROG_MIN_MS (watt_progress_due).
 * Overlay thread only. */
static void ov_rebuild_attenuation(void) {
    static unsigned sig[AZMOD_STEM_DECKS];
    static unsigned long long gsig[AZMOD_STEM_DECKS];
    static long last_ms[AZMOD_STEM_DECKS], ov_last_ms[AZMOD_STEM_DECKS];
    static int ov_pend[AZMOD_STEM_DECKS];
    static long fsig[AZMOD_STEM_DECKS];
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long now_ms = (long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
        /* The key: the pad toggles, whether stems are loaded, and the envelope's identity (a new
         * track or a finished build). The decode's growing frame count is not in it. */
        long fr = g_stem_frames ? g_stem_frames(d) : 0;
        unsigned eg = azmod_stem_env_gen ? azmod_stem_env_gen(d) : 0;
        unsigned cur = (fr > 0 ? 1u << 4 : 0) | ((eg & 0x07ffffffu) << 5);
        for (int k = 0; k < AZMOD_STEM_COUNT; k++)
            if (g_state.stem_on[d][k])
                cur |= 1u << k;
        /* The gains this deck's stems play at; unity without the azstem export. */
        int gq[AZMOD_STEM_COUNT];
        for (int k = 0; k < AZMOD_STEM_COUNT; k++)
            gq[k] = g_stem_gain_fn ? watt_gain_q(g_stem_gain_fn(d, k)) : WATT_GQ_ONE;
        unsigned long long gcur = watt_gain_key(gq);
        int have = __atomic_load_n(&g_att_have[d], __ATOMIC_RELAXED);
        int built = 0, keyed = cur != sig[d] || (cur && !have); /* not a gain-only change */
        if (watt_should_build(cur, gcur, sig[d], gsig[d], have, now_ms, last_ms[d]) ||
            watt_progress_due(fr, fsig[d], now_ms, last_ms[d])) {
            sig[d] = cur;
            gsig[d] = gcur;
            fsig[d] = fr;
            last_ms[d] = now_ms;
            built = build_atten(d, gq);
        }
        if (watt_ov_step(built, keyed, &ov_pend[d], &ov_last_ms[d], now_ms))
            __atomic_fetch_or(&g_ov_refresh, 1 << d, __ATOMIC_RELEASE);
    }
}

/* Which decks show the BEAT LOOP pad grid the stem strip sits over. */
static void ov_sync_pad_beatloop(struct overlay_ctx *c) {
    for (int d = 0; d < AZMOD_STEM_DECKS;
         d++) { /* strip only over the BEAT LOOP pad grid */
        /* d is a logical deck. Its grid is on physical side d & 1 (decks 1/3 left, 2/4
         * right), and only while that side's deck select shows d, so decks 3/4 get their
         * row in 2-deck mode and deck 1's lanes never sit over deck 3's grid. */
        int side = d & 1;
        bool bl = azmod_logical_deck(side, g_deck_sel[side]) == d && stems_page(side, d);
        if (bl != g_state.pad_beatloop[d]) {
            g_state.pad_beatloop[d] = bl;
            c->dirty = 1;
        }
    }
}

/* Every 5th tick, in every mode: rates and readouts, the waveform hooks' mirrors, the
 * attenuation tables and the stem-strip placement. */
static void ov_slow_tick(struct overlay_ctx *c) {
    /* None of the work below is specific to the closed menu (FPS readout,
     * wave_prov_report, wave_zoom_tick, the pad_beatloop mirror), so it runs in every mode
     * on its own throttle; under the closed guard it would freeze while the menu is open. */
    if ((++c->wave_tick % 5) != 0)
        return;
    __atomic_store_n(&g_visdbg, AZ_PROBE_ON("/home/root/settings/azmod-visdbg"),
                     __ATOMIC_RELAXED);
    ov_measure_rates(c);
    g_state.pledit_needs_reload =
        az_on(AZF_PLAYLIST_EDITING) && !g_reorder_hooks;
    ov_poll_1hz(c);
    ov_poll_readouts(c);
    ov_poll_clock(c);
    ov_log_paint_rate();
    ov_sync_wave_hooks();
    ov_sync_pad_beatloop(c);
}

/* Move and resize one of our windows, only when its geometry changed. */
static void ov_window_place(struct overlay_ctx *c, struct ov_window *w, int x, int y, int ww,
                            int wh) {
    if (x == w->x && y == w->y && ww == w->w && wh == w->h)
        return;
    XMoveResizeWindow(c->dpy, w->id, x, y, ww, wh);
    w->x = x;
    w->y = y;
    w->w = ww;
    w->h = wh;
    c->dirty = 1;
}

/* Map or unmap a window to match w->wanted. A mapped window is raised only when something
 * covered us (need_raise): every restack Exposes EP147's window underneath, and raising every
 * tick re-Exposes it so often that Browse and Playlist cannot populate their lists. */
static void ov_window_show(struct overlay_ctx *c, struct ov_window *w) {
    if (w->wanted) {
        if (!w->mapped) {
            XMapRaised(c->dpy, w->id);
            w->mapped = 1;
            c->dirty = 1;
        } else if (c->need_raise)
            XRaiseWindow(c->dpy, w->id);
    } else if (w->mapped) {
        XUnmapWindow(c->dpy, w->id);
        w->mapped = 0;
    }
}


/* Where the stem strips go: 0 = nowhere, 1 = the overlay strip (title band through row 2),
 * 2 = row 2 only, over the native tiles, for the mini stem waveforms. *side1 is the left
 * side's (the menu window carries it), *side2 the right side's. */
static void ov_strip_visibility(int *side1, int *side2) {
    /* Per-deck strip visibility folds in the STEMS master and that deck's stems
     * availability; it only applies in the closed WAVEFORM state. */
    int wave_closed = (g_state.mode == AZ_CLOSED && g_state.waveform_visible &&
                       azmod_stems_engine(&g_state));
    /* The strip shows only over a Beat Loop grid whose deck has stems; a stemless deck keeps
     * its native grid, and a window left mapped there paints a black rectangle over the
     * native pads.
     *   1 = the overlay strip (title band through row 2).
     *   2 = native tiles: map over row 2 only, for the mini stem waveforms. Those four
     *       slots are the FX placeholders the lanes sit in, so nothing native is covered.
     * Never under EP147's GRID ADJUST panel, which opens over exactly these pad rows;
     * EP147 hides its own stem tiles there, and this hides ours with them. */
    int gridadj = __atomic_load_n(&g_grid_adjust, __ATOMIC_RELAXED);
    /* per physical side: the deck each side is showing (1 or 3 left, 2 or 4 right) */
    int sd1 = azmod_logical_deck(0, g_deck_sel[0]),
        sd2 = azmod_logical_deck(1, g_deck_sel[1]);
    int d1base = wave_closed && !gridadj && g_state.pad_beatloop[sd1] &&
                 g_state.stems_available[sd1] && !g_four_deck;
    int d2base = wave_closed && !gridadj && g_state.pad_beatloop[sd2] &&
                 g_state.stems_available[sd2] && !g_four_deck;
    /* Stem FX = FX: row 2 is EP147's own FX tiles, so the lanes window is not mapped
     * (azmod_native_lanes_drawn draws no lanes then either; a mapped window would paint its
     * ground over the tiles). */
    int lanes = az_get(AZF_STEM_FX) != 1;
    *side1 = d1base ? (azmod_stem_native ? (lanes ? 2 : 0) : 1) : 0;
    *side2 = d2base ? (azmod_stem_native ? (lanes ? 2 : 0) : 1) : 0;
}

/* The menu window: always mapped; wanted_rect() parks it off screen when unused. It also
 * carries the left side's stem strip (strip_vis). */
static void ov_place_menu(struct overlay_ctx *c, int strip_vis) {
    int wx, wy, ww, wh;
    wanted_rect(&g_fb, g_state.mode, g_state.source_visible, strip_vis, &wx, &wy, &ww, &wh);
    if (wx != c->menu.x || wy != c->menu.y || ww != c->menu.w || wh != c->menu.h) {
        dbg("[%ld] resize (%d,%d,%d,%d) -> (%d,%d,%d,%d) mode=%d fb=%dx%d\n",
            (long) time(NULL), c->menu.x, c->menu.y, c->menu.w, c->menu.h, wx, wy, ww, wh, g_state.mode,
            g_fb.w, g_fb.h);
        XMoveResizeWindow(c->dpy, c->menu.id, wx, wy, ww, wh);
        c->menu.x = wx;
        c->menu.y = wy;
        c->menu.w = ww;
        c->menu.h = wh;
        c->dirty = 1; /* geometry changed -> must repaint even if content didn't */
        /* And raise. A geometry change takes over a new piece of screen, and whatever
         * EP147 has there is already above us: the menu window is mapped once and never unmapped,
         * so XMapRaised only raised it on that first map. Without this, the docked menu
         * opens at the right rect but underneath EP147 on the deck (the emulator's
         * stacking happens to differ, so it renders there).
         *
         * Throttled to one raise per 8 ticks: if any input to wanted_rect ever
         * oscillated, an unthrottled raise would restack every tick, and each restack
         * Exposes EP147's window (an Expose storm can starve the CPU badly enough to
         * lose SSH). A real open still raises immediately. */
        {
            static unsigned last_raise_tick;
            unsigned now = __atomic_load_n(&g_overlay_beat, __ATOMIC_RELAXED);
            if (now - last_raise_tick >= 8u) {
                c->need_raise = 1;
                last_raise_tick = now;
            }
        }
    }
    c->menu.wanted = 1;
    ov_window_show(c, &c->menu);

    /* Fade-to-clear shape while an undocked menu is open; a full rectangle otherwise. Never
     * while docked: the cached mask is built at one width and would clip the full-screen
     * menu. */
    if (g_state.mode != AZ_CLOSED && !azmod_docked()) {
        if (!c->menu_shaped) {
            apply_menu_shape(c->dpy, c->scr, c->menu.id, &g_fb);
            c->menu_shaped = 1;
        }
    } else if (c->menu_shaped) {
        XShapeCombineMask(c->dpy, c->menu.id, ShapeBounding, 0, 0, None, ShapeSet);
        c->menu_shaped = 0;
    }
}

/* Deck 2's stem strip: its own window, shown only when that side's deck has stems. */
static void ov_place_strip2(struct overlay_ctx *c, int strip_vis) {
    int x = -16, y = -16, w = 1, h = 1; /* parked off screen while unwanted */
    if (strip_vis == 2)
        azmod_stem_lanes_win_rect(&g_fb, 1, &x, &y, &w, &h);
    else if (strip_vis)
        azmod_stem_win_rect(&g_fb, 1, &x, &y, &w, &h);
    c->strip2.wanted = strip_vis != 0;
    ov_window_place(c, &c->strip2, x, y, w, h);
    ov_window_show(c, &c->strip2);
}

/* The MOD MENU row's icon cell: shown on SOURCE while the native row is live. */
static void ov_place_srcicon(struct overlay_ctx *c) {
    int n3 = __atomic_load_n(&g_srcrow_n, __ATOMIC_RELAXED);
    int d3vis =
        g_srcrow_active && g_state.source_visible && n3 >= 0 &&
        (g_state.mode == AZ_CLOSED || azmod_docked()) &&
        !(g_state.mode != AZ_CLOSED &&
          __atomic_load_n(&g_menu_focused,
                          __ATOMIC_RELAXED)); /* the full-screen menu covers the row */
    int x = -16, y = -16, w = 1, h = 1; /* parked off screen while unwanted */
    if (d3vis) {
        x = 0;
        y = (int) (0.0875f * g_fb.h) + (int) (0.1125f * g_fb.h) * n3;
        w = (int) (0.0703f * g_fb.w);
        h = (int) (0.1125f * g_fb.h);
    }
    c->srcicon.wanted = d3vis;
    ov_window_place(c, &c->srcicon, x, y, w, h);
    ov_window_show(c, &c->srcicon);
}

/* The OVERLAYS readouts (FPS and the rest) share one band, shown only on the deck view; the
 * Clock has its own window, in the place of EP147's recording time readout. */
static void ov_place_readouts(struct overlay_ctx *c) {
    int x = -16, y = -16, w = 1, h = 1; /* parked off screen while unwanted */
    c->readouts.wanted = azmod_stats_any(&g_state) && g_state.waveform_visible;
    if (c->readouts.wanted)
        azmod_stats_rect(&g_state, &g_fb, &x, &y, &w, &h);
    ov_window_place(c, &c->readouts, x, y, w, h);
    ov_window_show(c, &c->readouts);
    x = y = -16, w = h = 1;
    c->clockn.wanted = azmod_clockn_on(&g_state); /* every screen: the status bar is on all of them */
    if (c->clockn.wanted)
        azmod_clockn_rect(&g_fb, &x, &y, &w, &h);
    ov_window_place(c, &c->clockn, x, y, w, h);
    ov_window_show(c, &c->clockn);
    /* the Clock's tap target: its rect, 14 px taller above and below (the status bar) */
    c->clocktap.wanted = c->clockn.wanted;
    if (c->clocktap.wanted) {
        int ty = y - 14 < 0 ? 0 : y - 14;
        ov_window_place(c, &c->clocktap, x, ty, w, y + h + 14 - ty);
    } else
        ov_window_place(c, &c->clocktap, -16, -16, 1, 1);
    ov_window_show(c, &c->clocktap);
    x = y = -16, w = h = 1;
    c->ctpop.wanted = azmod_ctpop_rect(&g_state, &g_fb, &x, &y, &w, &h);
    ov_window_place(c, &c->ctpop, x, y, w, h);
    ov_window_show(c, &c->ctpop);
    int key = azmod_clockn_key(); /* the countdown's seconds and its red flash */
    if (key != c->ct_key) {
        c->ct_key = key;
        c->dirty = 1;
    }
}

/* One deck's stem-lane window: only while the deck screen is up, STEMS is on and that deck
 * has stems. Also asks for a repaint when a visible lane's playhead has moved enough. */
static void ov_place_lane(struct overlay_ctx *c, int d) {
    double zppc = 0, zph = 0, zncol = 0, ztot = 0;
    int zok = azmod_wave_zoom(d, &zppc, &zph, &zncol, &ztot);
    /* In 4-deck the mini stem buttons are a status readout and follow STEMS alone.
     * Otherwise this window carries the STEM LANES drawing, which also needs that mod
     * and a zoom model. */
    int lanes_on = 0; /* Stem Waves: retired slot 3, forced off (features.def) */
    c->lane[d].wanted = g_state.mode == AZ_CLOSED && g_state.waveform_visible &&
                        azmod_stems_engine(&g_state) && g_state.stems_available[d] &&
               /* 2-deck: only the deck its side is showing (decks 1/3 share the
                * upper band, 2/4 the lower), as for pad_beatloop above */
                        (g_four_deck || azmod_logical_deck(d & 1, g_deck_sel[d & 1]) == d) &&
               /* 4-deck under native stems: the readout is the deck's own
                * source-cell image (stems4_tick), so this window must not be
                * mapped at all, or it paints an empty black rectangle over the
                * native strip. */
                        (g_four_deck ? !azmod_stem_native : (lanes_on && zok));
    int lx = -16, ly = -16, lw = 1, lh = 1; /* parked off screen while unwanted */
    if (c->lane[d].wanted) {
        if (g_four_deck)
            azmod_stem_dots_rect(&g_fb, d, &lx, &ly, &lw, &lh);
        else
            azmod_waveband_rect(&g_fb, d, &lx, &ly, &lw, &lh);
    }
    ov_window_place(c, &c->lane[d], lx, ly, lw, lh);
    ov_window_show(c, &c->lane[d]);
    if (c->lane[d].wanted) {
        /* The lanes scroll with the track, but a full recomposite every tick is 4 MB of
         * memset for nothing while paused. Repaint once the playhead moves half a
         * column. */
        static float lastpos[AZMOD_STEM_DECKS];
        const unsigned char *env;
        int nb, nst;
        float pos = 0;
        static int stale[AZMOD_STEM_DECKS];
        if (!g_four_deck && azmod_stem_wave &&
            azmod_stem_wave(d, &env, &nb, &nst, &pos) && ztot > 0 && zppc > 0) {
            double moved = (double) (pos - lastpos[d]) * ztot;
            if (moved < 0)
                moved = -moved;
            /* Half a column, or 8 ticks regardless: a bad ppc must never freeze the
             * overlay. */
            if (moved >= zppc * 0.5 || ++stale[d] >= 8) {
                lastpos[d] = pos;
                stale[d] = 0;
                c->dirty = 1;
            }
        } else
            c->dirty = 1;
    }
    /* The row-2 native mini lanes carry a playhead too. They must repaint on their own
     * predicate, not the block above, which is gated on the STEMWAVES mod and a zoom
     * model that the native lanes do not depend on. Repaint once the playhead moves a
     * whole pixel; a paused deck costs nothing. */
    if (azmod_native_lanes_drawn(&g_state, d) && azmod_stem_wave) {
        static float nl_last[AZMOD_STEM_DECKS];
        const unsigned char *nenv;
        int nnb, nnst;
        float npos = 0;
        int lx2, ly2, lw2, lh2;
        azmod_stem_lane_rect(&g_fb, d, 0, &lx2, &ly2, &lw2, &lh2);
        if (lw2 > 0 && azmod_stem_wave(d, &nenv, &nnb, &nnst, &npos)) {
            float px = (npos - nl_last[d]) * (float) lw2;
            if (px >= 1.0f || px <= -1.0f) {
                nl_last[d] = npos;
                c->dirty = 1;
            }
        }
    }
}

/* Size, place, map and raise every window for this tick. */
static void ov_place_windows(struct overlay_ctx *c) {
    int strip1, strip2;
    sdk_overlay_place(c, AZ_LAYER_OVER_EP147); /* below everything of ours */
    ov_strip_visibility(&strip1, &strip2);
    ov_place_menu(c, strip1);
    ov_place_strip2(c, strip2);
    ov_place_srcicon(c);
    ov_place_readouts(c);
    for (int d = 0; d < AZMOD_STEM_DECKS; d++)
        ov_place_lane(c, d);
    sdk_overlay_place_xwin(c);           /* other programs' windows, above ours */
    sdk_overlay_place(c, AZ_LAYER_TOP);  /* above everything */
}

/* Recomposite the framebuffer and copy each mapped window's part of it to the screen. */
static void ov_present(struct overlay_ctx *c) {
    struct timespec ct0, ct1;
    clock_gettime(CLOCK_MONOTONIC, &ct0);
    ov_shm_wait(c); /* the last frame's puts are read before this one overwrites them */
    fb_ground(c->buf, c->scr_w, c->scr_h);
    g_state.menu_focused = __atomic_load_n(
        &g_menu_focused,
        __ATOMIC_RELAXED); /* orange highlight only when focus is in the pane */
    g_state.pinrows_on = (access(PINROWS_FLAG, F_OK) ==
                          0); /* pin feature gate, mirrored for menu.c + pin_count */
    azmod_composite(&g_state, &g_fb);
    clock_gettime(CLOCK_MONOTONIC, &ct1);
    {
        long us =
            (ct1.tv_sec - ct0.tv_sec) * 1000000L + (ct1.tv_nsec - ct0.tv_nsec) / 1000;
        __atomic_add_fetch(&g_comp_n, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_comp_us, us, __ATOMIC_RELAXED);
        if (us > __atomic_load_n(&g_comp_max, __ATOMIC_RELAXED))
            __atomic_store_n(&g_comp_max, us, __ATOMIC_RELAXED);
    }
    /* Each window is sized to exactly what is visible; copy its sub-rect out of the
     * full-screen software buffer. */
    ov_put(c, c->menu.id, c->menu.x, c->menu.y, c->menu.w, c->menu.h);
    if (c->strip2.wanted && c->strip2.mapped)
        ov_put(c, c->strip2.id, c->strip2.x, c->strip2.y, c->strip2.w, c->strip2.h);
    for (int d = 0; d < AZMOD_STEM_DECKS; d++)
        if (c->lane[d].wanted && c->lane[d].mapped)
            ov_put(c, c->lane[d].id, c->lane[d].x, c->lane[d].y, c->lane[d].w, c->lane[d].h);
    /* the parts below draw into the buffer after the puts above: the server reads those first */
    if (c->srcicon.wanted && c->srcicon.mapped) {
        ov_shm_wait(c);
        azmod_draw_srcrow_icon(&g_fb, c->srcicon.x, c->srcicon.y, c->srcicon.w, c->srcicon.h,
                               azmod_docked() && g_state.mode != AZ_CLOSED);
        ov_put(c, c->srcicon.id, c->srcicon.x, c->srcicon.y, c->srcicon.w, c->srcicon.h);
    }
    if (c->readouts.wanted && c->readouts.mapped) {
        ov_shm_wait(c);
        azmod_draw_stats(&g_state, &g_fb);
        ov_put(c, c->readouts.id, c->readouts.x, c->readouts.y, c->readouts.w, c->readouts.h);
    }
    if (c->clockn.wanted && c->clockn.mapped) {
        ov_shm_wait(c);
        azmod_draw_clockn(&g_state, &g_fb);
        ov_put(c, c->clockn.id, c->clockn.x, c->clockn.y, c->clockn.w, c->clockn.h);
    }
    if (c->ctpop.wanted && c->ctpop.mapped) {
        ov_shm_wait(c);
        azmod_draw_ctpop(&g_fb);
        ov_put(c, c->ctpop.id, c->ctpop.x, c->ctpop.y, c->ctpop.w, c->ctpop.h);
    }
}

/* Startup work that needs EP147 running: the hook table, Hide Track Title's runtime part,
 * the optional USB asset mods and the X-PAD settings. */
static void ov_start_mods(void) {
    /* the hooks went in from the constructor (install_static_hooks); record what each did */
    azm_hooks_dump(AZM_HOOKTABLE_PATH);
    ht_runtime_init(); /* Hide Track Title's empty juce::String: EP147 is running now */
    /* USB mod loader (azmod-usbmods.on): asset mods a user can share by dropping a folder on
     * the music stick.
     *
     *   <stick>/MODS/<mod name>/
     *       mod.txt          target=<fw tag>   (required; refused on mismatch)
     *       0.bin 0.addr     asset blob + its vaddr, the same pair format as
     *       1.bin 1.addr     pngpatch (applied in the constructor, azmod.c)
     *
     * A vaddr read from a stranger's stick is a write-anywhere primitive, so a USB mod is held
     * to these rules (the first two apply to the pngpatch set as well, which is staged from
     * the stick too):
     *
     *   1. Art only. Every target must be the first byte of one of this build's PNG images
     *      (ep147png.h, generated from the pristine binary), and the blob a PNG that fits it.
     *      A mod can change what the deck draws; it cannot touch code, vtables or any other
     *      data. Anything else is refused and logged.
     *   2. Fully contained. va + length must stay inside that image and inside .rodata.
     *   3. Version gated. mod.txt must name the firmware it was built for; an address means
     *      something only in one EP147 build.
     *   4. Opt-in. No flag, no USB mods.
     *
     * The mapping is made read-only again afterwards, and PROT_EXEC is never requested. */
    if (AZ_PROBE_ON("/home/root/settings/azmod-usbmods.on"))
        usbmods_load();

    xpad_load_config();
    if (AZ_PROBE_ON("/home/root/settings/azmod-wavestems.on"))
        __atomic_store_n(&g_wave_forcestems, 1,
                         __ATOMIC_RELEASE); /* standalone center-only test override */
}

/* One tick's input: our windows' X events, and what the bus and EP147's threads handed over. */
static void ov_poll_inputs(struct overlay_ctx *c) {
    ov_drain_x_events(c);
    ov_on_browse_view(c);
    ov_take_menu_handoffs(c);
    ov_publish_menu_state(c);
}

/* One tick's state: publish the menu's settings to the bus threads and azstem, follow EP147's
 * screens and decks, and run the menu's own timers. */
static void ov_update_state(struct overlay_ctx *c) {
    __atomic_add_fetch(&g_overlay_beat, 1, __ATOMIC_RELAXED); /* watchdog tick */
    ov_log_knob();
    ov_log_srcrow_state();
    ov_sync_stems_master();
    ov_sync_drum_roll();
    ov_sync_ui60();
    ov_sync_stem_pads(c);
    if (g_state.mode != AZ_CLOSED && azmod_tick(&g_state))
        c->dirty = 1; /* the menu's own animations */
    if (__atomic_exchange_n(&g_sdk_reload_req, 0, __ATOMIC_ACQ_REL)) { /* a mod's reload() */
        azcm_reload_requested();
        c->pending_reload = 1;
    }
    ov_fire_reload(c);
    ov_srcrow_focus(c);
    ov_docked_gate(c);
    ov_detect_deck_view(c);
    community_tick(); /* community mods: ~30 Hz tick, settings flush, quarantine timer */
    azmod_cm_bg_tick(); /* Community Mods: Update all's next install */
    ssh_approve_tick(); /* a stick's SSH key waiting to be allowed on the deck */
    sdk_overlay_tick(c); /* their view, long presses and injected pointer events */
    if (community_redraw_wanted())
        c->dirty = 1;
    ov_track_stems(c);
    ov_detect_source_docked(c);
    ov_detect_source_closed(c);
    ov_rebuild_attenuation(); /* every tick: a Stem Volume knob is followed at the tick rate */
    ov_slow_tick(c);
}

/* The adaptive tick. The tick adds to WAVE_SEEN_MS on every view change, so poll at 10 ms
 * while the deck view is up (a touch-driven view change is noticed within a frame) and while
 * the focused menu owns the knob (a detent waits for this tick); 30 ms elsewhere. The loop
 * redraws only when dirty, so extra wakeups are a cheap flag check. */
static useconds_t ov_tick_us(void) {
    return (g_state.waveform_visible ||
            (g_state.mode != AZ_CLOSED && __atomic_load_n(&g_menu_focused, __ATOMIC_RELAXED)))
               ? 10000
               : 30000;
}

/* Waits for the next tick: the tick's time, or less when an X event (a touch) arrives on our
 * connection, so a finger's step is drawn as soon as it is reported instead of after the rest
 * of the tick. A tick is never shorter than OV_MIN_TICK_US, however busy the connection. */
#define OV_MIN_TICK_US 4000
static void ov_wait(struct overlay_ctx *c, useconds_t us, long start_ms) {
    if (us > OV_MIN_TICK_US)
        usleep(OV_MIN_TICK_US);
    else {
        usleep(us);
        return;
    }
    if (XEventsQueued(c->dpy, QueuedAlready) > 0)
        return;
    long left = (long) (us / 1000) - (now_ms() - start_ms);
    if (left <= 0)
        return;
    struct pollfd p = {.fd = ConnectionNumber(c->dpy), .events = POLLIN};
    poll(&p, 1, (int) left);
}

/* The Mod Menu's thread: owns g_state and every overlay window. */
static void *overlay_thread(void *arg) {
    (void) arg;
    g_cm_on_ui = 1; /* community mods' init, tick and paint run here: no stick reads (sdk_lib.c) */
    ov_start_mods();
    struct overlay_ctx ctx;
    struct overlay_ctx *c = &ctx;
    if (!ov_open_display(c))
        return NULL;
    sdk_overlay_attach(c); /* before the mods' init(), which may create surfaces */
    ov_create_windows(c);
    ov_load_state();
    for (;;) {
        long tick_start = now_ms();
        bus_cfg_publish(); /* this tick's Mod Menu settings -> the bus threads */
        vs_save_flush(); /* before any continue: a Vinyl Speed Adjust value posted by
                          * the touch handler on EP147's message thread */
        wz_tick(tick_start); /* Remember Waveform Zoom: saves a new zoom once it settles */
        if (ov_reload_banner(c))
            continue;
        ov_poll_inputs(c);
        ov_update_state(c);
        ov_place_windows(c);
        if (c->dirty)
            ov_present(c);
        if (c->dirty || sdk_any_redraw())
            sdk_overlay_present(c, c->dirty);
        c->need_raise = 0;
        XFlush(c->dpy);
        c->dirty = 0;
        ov_wait(c, ov_tick_us(), tick_start);
    }
    return NULL;
}
