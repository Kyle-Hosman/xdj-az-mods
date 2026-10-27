/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file bus_interpose.c
 * @brief libc interposers on the subucom panel buses: open, ioctl and read hooks that
 *        remember the bus file descriptors, rewrite LED frames and filter input frames
 *
 * open() records the descriptors of /dev/subucom_spi1.0 (ErP bus: deck buttons and pads),
 * /dev/subucom_spi3.0 (mixer/panel bus: knob, click, BACK, EQ knobs, the x-pad strip),
 * the other subucom devices and /dev/gpiodrv. The ioctl hook rewrites the 128-byte
 * ErP LED frame in flight (pad-mode colors, stem pad colors, Beat Loop button, drum roll
 * pad state) and recomputes its CRC-16/X25. The read hook runs on EP147's bus thread: it
 * turns pad presses into stem and drum-roll requests, maps the x-pad byte to roll rates,
 * decodes MIC 2 and channel 3/4 EQ knobs for azstem, drives the Mod Menu and playlist
 * reorder gestures from knob, click and BACK, and swallows the bits EP147 must not see,
 * re-stamping the CRC only on mixer frames that arrived with a valid one.
 *
 * Deck identity follows the logical deck (0..3) held by the pad-mode and drum-roll state,
 * not the physical side. A watchdog on the overlay thread's heartbeat releases swallowed
 * input if that thread stalls. read() is a dispatcher: each bus has its own handler
 * (bus_mixer_frame, bus_erp_frame), built from small named stages.
 *
 * open() tracks the descriptors by number, so close, dup2 and dup3 are interposed to
 * forget a tracked number before the kernel can hand it to another file (see FD
 * tracking). Outside EP147 every interposer is a plain pass-through (g_azm_inert).
 *
 * Entry points: open, close, dup2, dup3, lseek, recvmsg, ioctl, read.
 */
/* Descriptors community mods' filters read through (sdk_input.c): the two deck MCU buses
 * (jog platters, read-only for mods) and the touch panel. Tracked always, used only while a
 * mod has registered a filter for them. */
static int g_deckbus_fd[2] = {-1, -1}; /* /dev/subucom_spi2.0, /dev/subucom_spi5.0 */
static int g_touchin_fd = -1;          /* the touch panel (/dev/input/event*, char major 13) */
static int g_mixer_fd = -1; /* /dev/subucom_spi3.0, the panel input bus:
                             * knob position counter at byte 34 (CW +1, CCW -1),
                             * strobe pair bit at byte 6 (0x00/0x08 per detent),
                             * knob click at byte 31 bit 0. Not the ErP bus. */
static volatile unsigned g_pad_req[AZMOD_STEM_DECKS] = {
    0}; /* pads 1..4: bits 0..3 pending stem mute toggle */
static volatile unsigned g_pad_solo_req[AZMOD_STEM_DECKS] = {
    0}; /* pads 5..8: bits 0..3 pending stem solo */
/* Stem FX pads (stem_fx_pads.h): the overlay sets the mode and resolves azstem's two calls;
 * the presses in flight belong to this thread. */
static int g_stemfx_pads;                      /* __atomic: 1 = pads 5-8 are FX pads */
static unsigned (*g_sfx_on_fn)(int deck);      /* azstem_stem_fx_on, __atomic */
static int (*g_sfx_fn)(int deck, int fx, int on); /* azstem_stem_fx, __atomic */
static sfxp_hold g_sfxp[AZMOD_PAD_DECKS][SFXP_N];
#ifdef AZMOD_PROBES
/* Development builds: every Stem FX pad decision into a ring the overlay drains to
 * /tmp/azmod-stemfx.log while /home/root/settings/azmod-stemfx.log exists. One word each:
 * kind(8) | deck(8) | fx(8) | value(8). Kinds: 'F' a frame with a rise or a hold (value = rise
 * << 4 | down), 'P' press answer, 'R' release sent, 'O' on bits read. */
static unsigned g_sfxlog[256];
static unsigned g_sfxlog_w, g_sfxlog_r; /* w: bus thread (__atomic), r: overlay thread */
static void sfxlog(int kind, int deck, int fx, unsigned v) {
    unsigned w = __atomic_load_n(&g_sfxlog_w, __ATOMIC_RELAXED);
    __atomic_store_n(&g_sfxlog[w & 255u],
                     (unsigned) kind << 24 | (unsigned) (deck & 0xff) << 16 |
                         (unsigned) (fx & 0xff) << 8 | (v & 0xffu),
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_sfxlog_w, w + 1, __ATOMIC_RELEASE);
}
static unsigned sfx_on_logged(int deck) {
    unsigned (*f)(int) = __atomic_load_n(&g_sfx_on_fn, __ATOMIC_ACQUIRE);
    unsigned v = f ? f(deck) : 0;
    sfxlog('O', deck, 0, v);
    return v;
}
static int sfx_fx_logged(int deck, int fx, int on) {
    int (*f)(int, int, int) = __atomic_load_n(&g_sfx_fn, __ATOMIC_ACQUIRE);
    int r = f ? f(deck, fx, on) : 0;
    sfxlog(on ? 'P' : 'R', deck, fx, (unsigned) r);
    return r;
}
#endif
static unsigned g_erp_last8[AZMOD_STEM_DECKS] = {
    0}; /* interposer-only: last all-8 pad bits (edge) */
static int g_gpiodrv_fds[MAX_GPIODRV_FDS];
static long g_gpiodrv_gpio[MAX_GPIODRV_FDS];
static int g_gpiodrv_last_level[MAX_GPIODRV_FDS];
static int g_gpiodrv_fd_count = 0;
static int gpiodrv_slot(int fd) {
    for (int i = 0; i < g_gpiodrv_fd_count; i++)
        if (g_gpiodrv_fds[i] == fd)
            return i;
    int i = -1;
    for (int k = 0; k < g_gpiodrv_fd_count && i < 0; k++)
        if (g_gpiodrv_fds[k] < 0)
            i = k; /* a slot bus_fd_forget() freed */
    if (i < 0 && g_gpiodrv_fd_count < MAX_GPIODRV_FDS)
        i = g_gpiodrv_fd_count++;
    if (i >= 0) {
        g_gpiodrv_fds[i] = fd;
        g_gpiodrv_gpio[i] = -1;
        g_gpiodrv_last_level[i] = -1;
    }
    return i; /* -1: tiny fixed table full, real device only opens ~6 */
}
/* CRC-16/X25: poly 0x1021 reflected (0x8408), init 0xFFFF, xorout 0xFFFF
 * (check value 0x906E for ASCII "123456789"). EP147 drops frames whose CRC
 * does not match, so every frame azmod edits must be re-stamped: mixer input
 * [0:96] at 96/97, ErP input [0:28] at 28/29, ErP LED [0:0x78] at 0x78/0x79. */
static uint16_t crc16_x25(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : (crc >> 1);
    }
    return crc ^ 0xFFFF;
}
/* ---- FD tracking ------------------------------------------------------------
 * open() records the bus descriptors by number, and read() and ioctl() act on any
 * descriptor with that number: decode the mixer frame, swallow bits, re-stamp CRCs,
 * rewrite the LED frame. A number outlives its file. Once EP147 closes a bus fd, or
 * dup2/dup3 lands another file on that number, the next open, socket or pipe can get
 * the same number, and that unrelated file would be decoded and edited as a bus.
 *
 * So close, dup2 and dup3 forget the number before the kernel can reuse it, and an
 * open() that returns a tracked number for another path forgets it too (a close that
 * bypassed the hooks, e.g. a raw syscall). That costs the bus thread nothing: read()
 * still only compares integers, no fstat per frame, no lock. dup() and
 * fcntl(F_DUPFD*) always take a free number, so they cannot overwrite a tracked one;
 * a duplicate of a bus fd is not tracked (no decode, the safe direction).
 * Closes made inside libc (fclose, closedir, popen) bypass the PLT, but they close
 * descriptors libc opened for its own FILE/DIR objects, never one EP147 open()ed.
 * The stores are atomic so the RT bus thread sees -1 before the number is free. */
static void bus_fd_drop(int *slot, int fd) {
    if (__atomic_load_n(slot, __ATOMIC_RELAXED) == fd)
        __atomic_store_n(slot, -1, __ATOMIC_SEQ_CST);
}
static void bus_fd_forget(int fd) {
    if (fd < 0)
        return;
    bus_fd_drop(&g_mixer_fd, fd);
    bus_fd_drop(&g_erp_fd, fd);
    for (int i = 0; i < g_gpiodrv_fd_count; i++)
        bus_fd_drop(&g_gpiodrv_fds[i], fd);
    bus_fd_drop(&g_deckbus_fd[0], fd);
    bus_fd_drop(&g_deckbus_fd[1], fd);
    bus_fd_drop(&g_touchin_fd, fd);
    tg_forget(fd);
}
/* Called by open() with every descriptor it returns: a number that comes back from
 * open() is no longer whatever was tracked under it, so forget first, then record
 * it again only if this path is a tracked device. */
static void bus_fd_note(int fd, const char *path) {
    bus_fd_forget(fd);
    if (!path)
        return;
    tg_note(fd, path);
    if (strcmp(path, "/dev/subucom_spi3.0") == 0) {
        g_mixer_fd = fd; /* panel-input bus: knob position @34, click @31 */
        dbg("[%ld] open(%s) -> fd=%d (g_mixer_fd set)\n", (long) time(NULL), path, fd);
    }
    if (strcmp(path, "/dev/subucom_spi1.0") == 0) {
        g_erp_fd = fd; /* ErP bus: deck buttons, including the pads */
        dbg("[%ld] open(%s) -> fd=%d (g_erp_fd set)\n", (long) time(NULL), path, fd);
    }
    if (strcmp(path, "/dev/gpiodrv") == 0)
        gpiodrv_slot(fd);
    if (strcmp(path, "/dev/subucom_spi2.0") == 0)
        g_deckbus_fd[0] = fd;
    else if (strcmp(path, "/dev/subucom_spi5.0") == 0)
        g_deckbus_fd[1] = fd;
    else if (strncmp(path, "/dev/input/", 11) == 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && major(st.st_rdev) == 13)
            g_touchin_fd = fd;
    }
}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnonnull-compare"
int open(const char *path, int flags, ...) {
    NEXT(next_open, "open");
    if (g_azm_inert) {
        mode_t m0 = 0;
        if (flags & O_CREAT) {
            va_list ap;
            va_start(ap, flags);
            m0 = va_arg(ap, mode_t);
            va_end(ap);
        }
        return next_open(path, flags, m0); /* cloexec-ok: EP147's own open, its flags passed through */
    }
    mode_t m = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        m = va_arg(ap, mode_t);
        va_end(ap);
    }
    int fd = next_open(path, flags, m); /* cloexec-ok: EP147's own open, its flags passed through */
    if (fd >= 0)
        bus_fd_note(fd, path);
    return fd;
}
#pragma GCC diagnostic pop
int open64(const char *, int, ...) __attribute__((alias("open")));

/* close/dup2/dup3: forget a tracked number before it is freed (see FD tracking).
 * A dup2/dup3 that fails leaves newfd open but untracked: the bus decode stops,
 * which is the safe direction, and EP147 never dup2()s onto its own bus fds. */
static int (*next_close)(int);
int close(int fd) {
    NEXT(next_close, "close");
    if (!g_azm_inert)
        bus_fd_forget(fd);
    return next_close(fd);
}
static int (*next_dup2)(int, int);
int dup2(int oldfd, int newfd) { /* cloexec-ok: the interposer itself, EP147's own dup2 passed through */
    NEXT(next_dup2, "dup2");
    if (!g_azm_inert && oldfd != newfd) /* dup2(fd, fd) closes nothing */
        bus_fd_forget(newfd);
    return next_dup2(oldfd, newfd);
}
static int (*next_dup3)(int, int, int);
int dup3(int oldfd, int newfd, int flags) {
    NEXT(next_dup3, "dup3");
    if (!g_azm_inert && oldfd != newfd) /* dup3(fd, fd) is EINVAL, closes nothing */
        bus_fd_forget(newfd);
    return next_dup3(oldfd, newfd, flags);
}

/* lseek() on a tracked gpiodrv fd selects a GPIO (the offset is the GPIO
 * number); remember it so the debug level-change log can name the line. */
static off_t (*next_lseek)(int, off_t, int);
off_t lseek(int fd, off_t offset, int whence) {
    NEXT(next_lseek, "lseek");
    if (g_azm_inert)
        return next_lseek(fd, offset, whence);
    off_t rc = next_lseek(fd, offset, whence);
    if (rc >= 0 && whence == SEEK_SET) {
        int i = -1;
        for (int k = 0; k < g_gpiodrv_fd_count; k++)
            if (g_gpiodrv_fds[k] == fd) {
                i = k;
                break;
            }
        if (i >= 0 && g_gpiodrv_gpio[i] != offset) {
            g_gpiodrv_gpio[i] = offset;
            dbg("[%ld] lseek gpiodrv fd=%d -> gpio=%ld\n", (long) time(NULL), fd, (long) offset);
        }
    }
    return rc;
}
off_t lseek64(int, off_t, int) __attribute__((alias("lseek")));

/* ---- Input path logger (azmod-inputlog.on): knob/click input also reaches
 * EP147 as 32-byte type-0x41 messages via recvmsg on an internal unix socket.
 * Bounded log to /tmp/azmod-input.log. */
#include <sys/socket.h>
static ssize_t (*next_recvmsg)(int, struct msghdr *, int);

/* This socket is spin-polled: an ungated flat budget is spent on idle UI chatter
 * within about a minute of EP147 starting. Hence an opt-in flag, a large budget,
 * and change-only logging with the per-message 16-bit counter at bytes 2..3 masked
 * out (the meaningful payload, such as the step field at 12..13, is otherwise
 * static). One FILE* per process, flushed per line; never an fopen per message. */
static int g_inlog_on = -1;
ssize_t recvmsg(int fd, struct msghdr *msg, int flags) {
    NEXT(next_recvmsg, "recvmsg");
    if (g_azm_inert)
        return next_recvmsg(fd, msg, flags);
    ssize_t r = next_recvmsg(fd, msg, flags);
    /* EP147 only. Unlike the bus loggers this hook is not fd-gated, so every
     * process in the unit's cgroup reaches it, including wpa_supplicant's netlink
     * path. az_logsfx() is empty only in EP147, which keeps the scan out of WiFi. */
    if (g_inlog_on < 0)
        g_inlog_on = (*az_logsfx() == 0) && (AZ_PROBE_ON("/home/root/settings/azmod-inputlog.on"));
    if (g_inlog_on > 0 && r > 0 && msg && msg->msg_iov && msg->msg_iovlen >= 1) {
        unsigned char *b = (unsigned char *) msg->msg_iov[0].iov_base;
        size_t cap = msg->msg_iov[0].iov_len;
        size_t n = (size_t) r < cap ? (size_t) r : cap;
        if (b && n >= 16 && b[0] == 0x41) {
            static FILE *inf;
            static int budget = 40000, nline;
            /* change-only, counter masked: bytes 2..3 are a per-message counter */
            static unsigned char prev[64];
            static size_t prevn;
            static int have;
            unsigned char key[64];
            size_t kn = n < sizeof key ? n : sizeof key;
            memcpy(key, b, kn);
            key[2] = 0;
            if (kn > 3)
                key[3] = 0;
            int changed = !have || kn != prevn || memcmp(key, prev, kn) != 0;
            if (changed && budget > 0) {
                memcpy(prev, key, kn);
                prevn = kn;
                have = 1;
                if (!inf)
                    inf = azlog_open("/tmp/azmod-input.log", "w");
                if (inf) {
                    struct timespec ts;
                    clock_gettime(CLOCK_MONOTONIC, &ts);
                    fprintf(inf, "%ld.%03ld fd=%d n=%zd ", (long) ts.tv_sec, ts.tv_nsec / 1000000,
                            fd, r);
                    for (size_t k = 0; k < n && k < 48; k++)
                        fprintf(inf, "%02x", b[k]);
                    fputc('\n', inf);
                    fflush(inf);
                    nline++;
                    budget--;
                }
            }
        }
    }
    return r;
}

/* Drum roll pads 5-7 on an armed deck, in the LED frame's pad bank (4 bytes per pad, {R,G,B}
 * linear light): pads 5 and 6 dark (no function); pad 7 HOLD off dim blue, HOLD on flashing
 * blue, full and dark on the blink phase. */
static void drum_hold_led(unsigned char *pb, int hold_state, int blink_on) {
    for (int i = 4; i < 6; i++) {
        pb[i * 4 + 0] = 0x00;
        pb[i * 4 + 1] = 0x00;
        pb[i * 4 + 2] = 0x00;
    }
    unsigned char *px = pb + 6 * 4;
    px[0] = 0x00;
    px[1] = 0x00;
    px[2] = (hold_state & DRUM_HOLD_ON) ? (blink_on ? 0xFF : 0x00) : 0x22;
}

/* SUBUCOM message ioctl. On the ErP fd it rewrites the outgoing 128-byte LED
 * frame (pad-mode colors, drum roll pads, stem pads, Beat Loop button, community
 * mod edits) and re-stamps its CRC over [0:0x78]. The 16-byte arg is a descriptor
 * {u32 magic, u32 size, u64 data_ptr}; the frame lives at data_ptr, in-process. */
static int (*next_ioctl)(int, unsigned long, ...);
int ioctl(int fd, unsigned long req, ...) {
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    NEXT(next_ioctl, "ioctl");
    if (g_azm_inert)
        return next_ioctl(fd, req, arg);
    /* Deck frames on their way to the jog displays: timed in the picture style only
     * (jog_display.c); observe-only, before the frame goes out. */
    if (arg && ((req >> 8) & 0xffu) == 0x70u) {
        if (fd == __atomic_load_n(&g_deckbus_fd[0], __ATOMIC_RELAXED))
            jog_tx_note(0, arg);
        else if (fd == __atomic_load_n(&g_deckbus_fd[1], __ATOMIC_RELAXED))
            jog_tx_note(1, arg);
    }
    /* ---- Panel ioctl probe (spi3.0 = g_mixer_fd) -------------------------------
     * Observe-only. Panel input, including BACK, arrives through read(); this
     * logs the mixer ioctl's buffer so that stays checkable. next_ioctl runs
     * first so any RX is filled. Type byte 0x70 = SUBUCOM message; descriptor
     * {?, u32 size @+4, u64 frame-ptr @+8}, as on the LED path. */
    if (fd == g_mixer_fd && arg && ((req >> 8) & 0xffu) == 0x70u) {
        { /* the mixer's LED frame as EP147 sends it: community mods may observe it */
            uint32_t tsize = 0;
            uint64_t tptr = 0;
            memcpy(&tsize, (unsigned char *) arg + 4, 4);
            memcpy(&tptr, (unsigned char *) arg + 8, 8);
            if (tptr > 0x1000 && tsize > 0 && tsize <= 256)
                sdk_filter_view(AZ_BUS_MIXER_LED, (const unsigned char *) (uintptr_t) tptr, (int) tsize);
        }
        int rc = next_ioctl(fd, req, arg);
        uint32_t size = 0;
        uint64_t dptr = 0;
        memcpy(&size, (unsigned char *) arg + 4, 4);
        memcpy(&dptr, (unsigned char *) arg + 8, 8);
        if (dptr > 0x1000 && size >= 35 && size <= 256) {
            unsigned char *f = (unsigned char *) (uintptr_t) dptr;
            /* azmod-srcrow.log: change-only log of panel bytes 31..34, so a BACK
             * press shows whether byte 0x21 ever arrives here. */
            if (srcrow_trace_on()) {
                static unsigned char pv[4];
                static int ph;
                if (!ph || pv[0] != f[0x1f] || pv[1] != f[0x20] || pv[2] != f[0x21] ||
                    pv[3] != f[0x22]) {
                    SRCROW_TRACE(80,
                                 "IOCTLPANEL: req=%#lx sz=%u b31=%02x b32=%02x b33=%02x b34=%02x\n",
                                 req, size, f[0x1f], f[0x20], f[0x21], f[0x22]);
                    pv[0] = f[0x1f];
                    pv[1] = f[0x20];
                    pv[2] = f[0x21];
                    pv[3] = f[0x22];
                    ph = 1;
                }
            }
            /* Measured on the deck: req 0x40107000 is the write-direction TX poll
             * and byte 0x21 stayed 00 through a held BACK that read() saw, so this
             * buffer does not carry panel buttons. */
        }
        return rc;
    }

    /* LED colors are rewritten here, on the ioctl, never through EP147's
     * in-process LED serializer/HuiLed hooks: those freeze the RT LED path.
     * Layout: pad RGB at ERP_LED_PAD_OFF (0x1e) + 4k, mode buttons at
     * ERP_LED_MODE_OFF (0x0e) + 4j; CRC-16/X25 over [0:0x78] at 0x78/0x79 covers
     * both, so any edit needs a re-stamp. */
    if (fd == g_erp_fd && arg && ((req >> 8) & 0xffu) == 0x70u &&
        ((req >> 16) & 0x3fffu) == 0x10u) {
        uint32_t size;
        uint64_t dptr;
        memcpy(&size, (unsigned char *) arg + 4, 4);
        memcpy(&dptr, (unsigned char *) arg + 8, 8);
        /* Both physical sides share one 128-byte LED frame; side 1 sits 0x38
         * after side 0. Edit whichever halves need it, then re-CRC once. */
        if (dptr && size >= 0x7a) {
            unsigned char *f = (unsigned char *) (uintptr_t) dptr;
            int wrote = 0;
            /* Pad colors: paint the DJ's mode-button picks on both sides, always
             * (independent of stems). Buttons with no pick stay native.
             *
             * Replace the hue, keep EP147's brightness. The live pad mode carries
             * its hue at full, the other three the same hue at ~4.7% (HOT CUE
             * 98ff9a -> 070c07, BEAT LOOP d16300 -> 0a0400; the bytes are linear
             * light). This loop is the first rewrite of the frame, so the cell is
             * still EP147's: dark stays dark, dim gets the pick at the stock
             * 12/255, lit gets it at full. */
            for (int j = 0; j < AZMOD_PADMODE_COUNT; j++) {
                /* each pick is one word: never a new red with an old green */
                uint32_t w1 = __atomic_load_n(&g_mode_rgbw[j], __ATOMIC_RELAXED),
                         w2 = __atomic_load_n(&g_mode2_rgbw[j], __ATOMIC_RELAXED);
                if (!(w1 & BC_RGB_SET) && !(w2 & BC_RGB_SET))
                    continue;
                for (int d = 0; d < AZMOD_PAD_DECKS; d++) { /* 128B LED frame: 2 physical decks */
                    unsigned char *m = f + ERP_LED_MODE_OFF + 4 * j + d * 0x38;
                    unsigned nmax = m[0] > m[1] ? m[0] : m[1];
                    if (m[2] > nmax)
                        nmax = m[2];
                    if (nmax == 0)
                        continue; /* EP147 has it dark: leave it */
                    /* Secondary page: EP147 switches all four buttons to lime
                     * (selected 9bff00, others 070c00). Lime = G is the max,
                     * B is 0, 0 < R < G, a shape no primary stock hue has (HOT CUE has
                     * B, BEAT LOOP has R > G). A lime cell takes the secondary-page
                     * pick (HOT CUE 2 / BEAT LOOP 2 / SLIP LOOP 2 / BEAT JUMP 2) with
                     * the same envelope, and stays lime when that page has no pick. */
                    int lime = m[1] >= m[0] && m[2] == 0 && m[0] > 0 && m[0] < m[1];
                    uint32_t w = lime ? w2 : w1;
                    if (!(w & BC_RGB_SET))
                        continue;
                    const unsigned char pk[3] = {(unsigned char) (w >> 16),
                                                 (unsigned char) (w >> 8), (unsigned char) w};
                    if (nmax >= 0x40) {
                        m[0] = pk[0];
                        m[1] = pk[1];
                        m[2] = pk[2];
                    } else
                        for (int c = 0; c < 3; c++)
                            m[c] = (unsigned char) ((pk[c] * 12u + 127u) / 255u);
                    wrote = 1;
                }
            }
            for (int d = 0; d < AZMOD_PAD_DECKS;
                 d++) { /* 128B LED frame: 2 physical decks (d*0x38 would overflow at d>=2) */
                /* The LED block is physical, but its state comes from the logical
                 * deck this side drives. act = stems master on and that deck has
                 * stems engaged. */
                int ld = azmod_logical_deck(d, g_deck_sel[d]);
                int act = __atomic_load_n(&g_erp_active[ld], __ATOMIC_RELAXED);
                /* Drum roll, armed in SLIP LOOP, owns pads 1-4 and 8 and is written
                 * first. Pads are 4 bytes each, {R,G,B,-}, in linear light (the
                 * lime below is sRGB 128 -> ~55 linear).
                 * drum_side is decided once, the same way the pad swallow decides
                 * it (drum, else stems), so the LEDs never show one surface while
                 * the pads act as the other. */
                int drum_side =
                    __atomic_load_n(&g_drum_arm, __ATOMIC_RELAXED) && drum_mode_is_slip(d, ld);
                /* The jog display shows this deck's stems exactly while its pads are the
                 * stem toggles (jog_display.c): the same answer the pad block below uses,
                 * published before the stems gate so a deck without stems reads 0. */
                __atomic_store_n(&g_jog_page[ld], act && !drum_side && stems_page(d, ld),
                                 __ATOMIC_RELAXED);
                if (drum_side) {
                    unsigned char *pb = f + ERP_LED_PAD_OFF + d * 0x38;
                    /* Latch and selection are keyed by the logical deck, the same
                     * key the pad-swallow path writes, so decks 3/4 show their own
                     * state. */
                    int on = __atomic_load_n(&g_drum_held[ld], __ATOMIC_RELAXED);
                    unsigned sel = g_drum_sel_led[ld] & 3u;
                    /* Blink phase for the selected sample pad: a frame counter, not
                     * a clock, because the bus path makes no syscalls. Raise or
                     * lower the shift to slow or speed the blink. */
                    static unsigned led_tick;
                    int blink_on =
                        ((++led_tick >> 5) & 1u) != 0; /* >>6 is too slow, >>4 too fast */
                    for (int i = 0; i < 4; i++) {
                        /* The selected sample blinks; the other three sit at a dim
                         * green (low values: the bytes are linear light). */
                        int full = ((unsigned) i == sel);
                        /* Not armed: write nothing and leave the pads stock. They
                         * are working SLIP LOOP pads until the deck is armed. */
                        if (!on)
                            continue;
                        /* Pulse bright <-> dim, never to black, so the selected pad
                         * does not read as disabled. */
                        if (full) {
                            pb[i * 4 + 0] = blink_on ? 0x37 : 0x06;
                            pb[i * 4 + 1] = blink_on ? 0xFF : 0x22;
                        } else {
                            pb[i * 4 + 0] = 0x06;
                            pb[i * 4 + 1] = 0x22;
                        }
                        pb[i * 4 + 2] = 0x00;
                    }
                    /* Armed: pads 5 and 6 dark, pad 7 is HOLD (drum_hold_led,
                     * drum_hold_pads). Unarmed they are left alone. */
                    if (on)
                        drum_hold_led(pb, __atomic_load_n(&g_drum_hold[ld], __ATOMIC_RELAXED), blink_on);
                    pb[7 * 4 + 0] = on ? 0x00 : 0xFF; /* pad 8: green on, red off */
                    pb[7 * 4 + 1] = on ? 0xFF : 0x00;
                    pb[7 * 4 + 2] = 0x00;
                    wrote = 1;
                }

                /* Stems surface only from here on. A deck without stems engaged is
                 * left fully native (orange BEAT LOOP button and pads). The drum
                 * roll block must stay above this gate: below it, a track with no
                 * stems would skip the drum-roll pad colors while the pad swallow,
                 * which is not stems-gated, kept working. */
                if (!act)
                    continue;
                /* BEAT LOOP button, stems green. One physical button serves both
                 * BEAT LOOP pages, so the page that earns the green depends on
                 * where stems live:
                 *  - page 1 (default): green unless EP147 is known on page 2, dim
                 *    while another mode is up; a Pad Colors pick still wins.
                 *  - BEAT LOOP 2: green only while EP147 is known on page 2, where
                 *    it beats a Pad Colors pick; otherwise native / Pad Colors. */
                {
                    int bl2 = stems_bl_mode() == 4;
                    int green =
                        bl2 ? stems_page(d, ld)
                            : (!(__atomic_load_n(&g_mode_rgbw[MODE_J_BEATLOOP], __ATOMIC_RELAXED) &
                                 BC_RGB_SET) &&
                               !bl_page2(ld));
                    /* bright while BEAT LOOP is the mode; EP147's own state (stems_page) counts
                     * too, so holding another mode button does not dim it early */
                    int bl_now = g_pad_mode_bl[d] || stems_page(d, ld);
                    if (green) {
                        unsigned char *m = f + ERP_LED_MODE_OFF + 4 * MODE_J_BEATLOOP + d * 0x38;
                        m[0] = 0x00;
                        m[1] = bl_now ? BEATLOOP_LED_G : (BEATLOOP_LED_G >> 3);
                        m[2] = 0x00;
                        wrote = 1;
                    }
                    __atomic_store_n(&g_bl_btn_green[d], green ? (bl_now ? 2 : 1) : 0,
                                     __ATOMIC_RELAXED);
                }
                /* Pad block: only on the stems page (pads = stem toggles), and only
                 * when drum roll does not own this side. Stem colors of the driven
                 * deck; a deck without stems never gets here (native pads). */
                if (!drum_side && stems_page(d, ld)) {
                    if (act) {
                        unsigned char *pb = f + ERP_LED_PAD_OFF + d * 0x38;
                        /* a whole block or the last whole one: the overlay rewrites it
                         * every tick, and this thread never waits for it */
                        static unsigned char good[AZMOD_STEM_DECKS][32];
                        unsigned char blk[32];
                        if (seqblk_read(&g_pad_led[ld], blk))
                            memcpy(good[ld], blk, 32);
                        memcpy(pb, good[ld], 32);
                        wrote = 1;
                        /* Log written bytes on change: the emulator has no physical
                         * LEDs, so this is how a deck switch can be seen there. */
                        static unsigned char lastpb[AZMOD_PAD_DECKS][32];
                        static int lastld[AZMOD_PAD_DECKS] = {-1, -1};
                        if (lastld[d] != ld || memcmp(lastpb[d], pb, 32) != 0) {
                            lastld[d] = ld;
                            memcpy(lastpb[d], pb, 32);
                            dbg("padled side=%d -> deck%d rgb %02x%02x%02x %02x%02x%02x "
                                "%02x%02x%02x %02x%02x%02x\n",
                                d, ld + 1, pb[0], pb[1], pb[2], pb[4], pb[5], pb[6], pb[8], pb[9],
                                pb[10], pb[12], pb[13], pb[14]);
                        }
                    }
                }
            }
            /* Community mods edit LED cells after the core's edits and before the single CRC
             * re-stamp below (a mod never computes a CRC). The LED frame is always sent;
             * AZ_FRAME_EATEN only stops later mods. */
            if (g_cm_live) {
                community_frame(AZ_BUS_ERP_LED, f, 0x78);
                wrote = 1;
            }
            if (sdk_filter_run(AZ_BUS_ERP_LED, f, 0x78))
                wrote = 1;
            for (int j = 0; j < 4; j++)
                for (int c = 0; c < 3; c++) /* telemetry, no syscalls */
                    g_modecell_tlm[j][c] = f[ERP_LED_MODE_OFF + 4 * j + c];
            for (int j = 0; j < 8; j++)
                for (int c = 0; c < 3; c++) /* side 0 pads, as sent */
                    g_padcell_tlm[j][c] = f[ERP_LED_PAD_OFF + 4 * j + c];
            if (wrote) {
                uint16_t crc = crc16_x25(f, 0x78); /* CRC over [0:0x78] */
                f[0x78] = (uint8_t) (crc & 0xff);
                f[0x79] = (uint8_t) (crc >> 8);
            }
        }
    }
    return next_ioctl(fd, req, arg);
}
/* ---- read(): the panel input buses ------------------------------------------
 * read() runs on EP147's bus threads for every frame of every subucom bus. It hands each
 * frame to one handler by bus: bus_mixer_frame() for the mixer/panel bus, bus_erp_frame()
 * for the ErP bus (deck buttons and pads). Each handler reads the frame, publishes what
 * azmod and azstem need with atomics, and clears the bits EP147 must not see.
 *
 * Rules for everything below (enforced by busread-lint.sh and busthread-lint.sh):
 *   - no system call, allocation or lock on the release path; logging goes through
 *     azlog_open(), which writes nothing unless diagnostics were switched on at start;
 *   - Mod Menu settings come only from the word the overlay thread publishes (bus_cfg());
 *   - a frame is re-stamped with a fresh CRC only if it arrived with a valid one: EP147
 *     drops a corrupted frame, and "repairing" it would make EP147 act on garbage. */

/* Development builds can capture raw bus traffic; release builds compile these to nothing. */
static inline void bus_capture_read(int fd, const void *buf, ssize_t r, size_t n, uintptr_t ra) {
    (void) fd, (void) buf, (void) r, (void) n, (void) ra;
}
static inline void bus_capture_mixer_in(const uint8_t *b, ssize_t r) {
    (void) b, (void) r;
}
static inline void bus_capture_mixer(const uint8_t *b, ssize_t r) {
    (void) b, (void) r;
}
static inline void bus_capture_xpad_fx(uint8_t *b, ssize_t r, const struct bus_xpad *xp,
                                       int touched, int have_zone) {
    (void) b, (void) r, (void) xp, (void) touched, (void) have_zone;
}
static inline void bus_trace_back(int depth, int pass, int at_top, int focused, int want) {
    (void) depth, (void) pass, (void) at_top, (void) focused, (void) want;
}

/* CRC-16/X25 of frame[0:len] against the two bytes that follow it. */
static inline int frame_crc_ok(const uint8_t *f, size_t len) {
    uint16_t c = crc16_x25(f, len);
    return f[len] == (uint8_t) (c & 0xff) && f[len + 1] == (uint8_t) (c >> 8);
}

/* Write the CRC-16/X25 of frame[0:len] into the two bytes that follow it. */
static inline void frame_crc_stamp(uint8_t *f, size_t len) {
    uint16_t c = crc16_x25(f, len);
    f[len] = (uint8_t) (c & 0xff);
    f[len + 1] = (uint8_t) (c >> 8);
}

/* Clear BACK (mixer byte 33 bit 7). With no other corner button held, byte 6 goes back to
 * its idle latch value, as the panel sends it with nothing pressed. */
static inline void mixer_clear_back(uint8_t *b) {
    b[33] &= (uint8_t) ~0x80;
    if (!b[33])
        b[6] = 0x08;
}

/* AZMOD_DEBUG: log gpiodrv level changes (not the idle re-polls), so a capture shows which
 * lines move. */
static void gpiodrv_note(int fd, const void *buf, ssize_t r) {
    if (!g_debug || r <= 0)
        return;
    for (int i = 0; i < g_gpiodrv_fd_count; i++) {
        if (g_gpiodrv_fds[i] != fd)
            continue;
        int lvl = ((const uint8_t *) buf)[0];
        if (lvl != g_gpiodrv_last_level[i]) {
            g_gpiodrv_last_level[i] = lvl;
            dbg("[%ld] gpiodrv fd=%d gpio=%ld level=%d\n", (long) time(NULL), fd,
                g_gpiodrv_gpio[i], lvl);
        }
        break;
    }
}

/* ---- Mixer bus: knobs read for azstem ---------------------------------------- */

/* Drum tone: MIC 2 HI = drum volume, MID = drum pitch. 10-bit knobs, top 8 bits at
 * 0x47/0x48, low 2 bits in 0x4b. Read-only (the mixer still applies them to MIC 2).
 * Change-only; azstem does two atomic stores, no I/O on this path. */
static void mixer_drum_tone(const uint8_t *mb, ssize_t r) {
    static int last_hi = -1, last_mid = -1;
    void (*tone_fn)(int, int) = __atomic_load_n(&g_drum_tone_fn, __ATOMIC_ACQUIRE);
    if (r < 0x4c || !tone_fn)
        return;
    int hi = (mb[0x47] << 2) | ((mb[0x4b] >> 6) & 3);
    int mid = (mb[0x48] << 2) | ((mb[0x4b] >> 4) & 3);
    if (hi != last_hi || mid != last_mid) {
        last_hi = hi;
        last_mid = mid;
        __atomic_store_n(&g_mic2_tone_tlm, (hi << 16) | mid, __ATOMIC_RELAXED);
        tone_fn(hi, mid);
    }
}

/* Stem volume on CH 3/4 EQ: in 2-deck use channels 3 and 4 are idle, so their EQ knobs set
 * deck 1's and deck 2's stem volumes: HI = vocals, MID = inst and bass, LOW = drums
 * (azstem_stem_eq). A channel stays at unity until its knobs change after EP147 starts:
 * right after a restart the knobs can read 0 untouched, which would start every stem
 * silent.
 *
 * Measured on the deck: each channel is a 5-byte group TRIM, HI, MID, LOW + one byte of low
 * bits (CH3 0x2e-0x32, CH4 0x33-0x37). Low-bit pairs: TRIM 7-6, HI 5-4, MID 3-2, LOW 1-0
 * (one pair lower than MIC 2, where HI is the top pair). */
static void mixer_stem_eq(const uint8_t *mb, ssize_t r) {
    static const struct {
        int hi, mid, low, lo, shi, smid, slow;
    } CH[2] = {
        {0x2f, 0x30, 0x31, 0x32, 4, 2, 0}, /* channel 3 -> deck 1 */
        {0x34, 0x35, 0x36, 0x37, 4, 2, 0}, /* channel 4 -> deck 2 */
    };
    static int last_v[2] = {-1, -1}, moved[2];
    for (int c = 0; c < 2; c++) {
        if (CH[c].hi < 0 || CH[c].mid < 0 || CH[c].low < 0 || CH[c].lo < 0 || r <= CH[c].hi ||
            r <= CH[c].mid || r <= CH[c].low || r <= CH[c].lo)
            continue;
        int v = (((mb[CH[c].hi] << 2) | ((mb[CH[c].lo] >> CH[c].shi) & 3)) << 20) |
                (((mb[CH[c].mid] << 2) | ((mb[CH[c].lo] >> CH[c].smid) & 3)) << 10) |
                ((mb[CH[c].low] << 2) | ((mb[CH[c].lo] >> CH[c].slow) & 3));
        if (v != last_v[c]) {
            if (last_v[c] >= 0)
                moved[c] = 1; /* the first frame seen is not a move */
            last_v[c] = v;
            if (moved[c])
                __atomic_store_n(&g_stemeq_knobs[c], v, __ATOMIC_RELAXED);
        }
    }
}

/* Reaching for the FX section exits drum roll: moving the FX select knob or pressing FX
 * on/off. Both live in mixer byte 0x16:
 *     bit 0        = FX ON/OFF, momentary   (10->11->10 per press)
 *     high nibble  = FX SELECT position, 4-bit Gray code, absolute
 *                    (3,2,6,7,5,4,c,d,f,e,a,b,9 across one sweep)
 * (0x55's high nibble is the same knob as linear 0..15, and 0x54 bit 0x80 the MCU's
 * FX-active latch; one byte answering both is cheapest here.) Gray code means any change is
 * a movement, so no wrap arithmetic. drum_disarm_all returns at once when nothing is
 * latched. */
static void mixer_fx_disarm(const uint8_t *mb, ssize_t r) {
    static int fx_have = 0;
    static uint8_t fx_last = 0;
    if (r <= 0x16)
        return;
    uint8_t fx = mb[0x16];
    if (fx_have) {
        if ((fx & 0x01u) && !(fx_last & 0x01u))
            drum_disarm_all("fx on/off");
        else if ((fx & 0xF0u) != (fx_last & 0xF0u))
            drum_disarm_all("fx select");
    }
    fx_last = fx;
    fx_have = 1;
}

/* ---- Mixer bus: the browse knob ----------------------------------------------
 * Byte 34 is an absolute position counter (CW +1, CCW -1 per detent). Frames stream
 * continuously, so freezing one frame is useless; an override must be sticky, rewriting
 * byte 34 on every frame. */
static uint8_t knob_last;   /* byte 34 as last seen */
static int knob_seen;       /* knob_last is valid */
static uint8_t knob_anchor; /* the position EP147 sees while the menu holds the knob */
static int knob_anchored;   /* the anchor is applied to every frame */
static int knob_was_hold;

/* Track the knob, publish its detents for the Mod Menu, and decide whether the menu holds
 * it (EP147 then sees a frozen byte 34). Returns the hold; *delta gets this frame's signed
 * detents. */
static int mixer_browse_knob(const uint8_t *mb, int *delta) {
    uint8_t raw = mb[34];
    int d = 0;
    if (knob_seen) {
        d = (int) (int8_t) (uint8_t) (raw - knob_last); /* signed detents, wrap-safe */
        if (d) {
            int acc = __atomic_add_fetch(&g_enc_dir, d, __ATOMIC_RELAXED);
            if (acc > 16 || acc < -16)
                __atomic_store_n(&g_enc_dir, acc > 0 ? 16 : -16, __ATOMIC_RELAXED);
        }
    }
    knob_last = raw;
    knob_seen = 1;
    int menu_open_m = __atomic_load_n(&g_menu_open, __ATOMIC_RELAXED);
    int focused_m = __atomic_load_n(&g_menu_focused, __ATOMIC_RELAXED);
    /* Freeze EP147's source list while the docked menu is focused: EP147 sees a constant
     * byte 34, so its list does not scroll while azmod drives the menu from the detents.
     * Never apply an offset instead; a non-zero byte-34 offset makes EP147's own position
     * tracking erratic. A plain freeze costs a one-frame jump on release, absorbed by
     * re-selecting our row on the focus-release edge (g_srcrow_pin_ours), so the user lands
     * on MOD MENU. The watchdog (wd_stall) releases the freeze if the overlay stalls. */
    int sdk_hold = __atomic_load_n(&g_sdk_focus, __ATOMIC_ACQUIRE); /* a mod's surface */
    int hold = ((focused_m && menu_open_m) || sdk_hold) && wd_stall <= 200u;
    if (focused_m && menu_open_m) { /* diagnostics: does EP147 see the knob? */
        __atomic_add_fetch(&g_kn_frames, 1, __ATOMIC_RELAXED);
        if (wd_stall > __atomic_load_n(&g_kn_wdmax, __ATOMIC_RELAXED))
            __atomic_store_n(&g_kn_wdmax, wd_stall, __ATOMIC_RELAXED);
        if (knob_was_hold && !hold)
            __atomic_add_fetch(&g_kn_rel, 1, __ATOMIC_RELAXED);
        if (!hold && d)
            __atomic_add_fetch(&g_kn_leak, (unsigned) (d < 0 ? -d : d), __ATOMIC_RELAXED);
    }
    if (knob_was_hold && !hold)
        __atomic_store_n(&g_srcrow_pin_ours, 1, __ATOMIC_RELEASE);
    knob_was_hold = hold;
    if (hold && !knob_anchored) { /* freeze at the current position */
        knob_anchor = raw;
        knob_anchored = 1;
    }
    if (!hold)
        knob_anchored = 0;
    if (d)
        SRCROW_TRACE(40, "KNOB:  delta=%d focused=%d anchored=%d want=%d wd_stall=%u\n", d,
                     focused_m, knob_anchored, __atomic_load_n(&g_srcrow_want, __ATOMIC_RELAXED),
                     wd_stall);
    *delta = d;
    return hold;
}

/* ---- Mixer bus: the playlist reorder gesture -----------------------------------
 * Runs after the knob logic and only when that is not holding the knob, so the two never
 * fight over byte 34. */

/* A playlist drag moves one detent at a time: a step that would carry the row past a folder
 * stops on it first; the next step in that direction passes it, the other way backs off it. */
static void drag_step_playlist(int delta) {
    int n = __atomic_load_n(&g_drag_rows, __ATOMIC_RELAXED),
        o = __atomic_load_n(&g_drag_origin, __ATOMIC_RELAXED);
    int changed = 0;
    for (int k = 0; k < (delta < 0 ? -delta : delta); k++) {
        int dir = delta < 0 ? -1 : 1, t = __atomic_load_n(&g_drag_target, __ATOMIC_RELAXED);
        int into = __atomic_load_n(&g_drag_into, __ATOMIC_RELAXED);
        if (into >= 0) {
            __atomic_store_n(&g_drag_into, -1, __ATOMIC_RELAXED);
            __atomic_store_n(&g_drag_into_disp, -1, __ATOMIC_RELAXED);
            if (dir == __atomic_load_n(&g_drag_into_dir, __ATOMIC_RELAXED))
                __atomic_store_n(&g_drag_target, t + dir, __ATOMIC_RELAXED);
            changed = 1;
            continue;
        }
        int nt = t + dir;
        if (nt < 0 || (n > 0 && nt > n - 1))
            break;
        int d = drag_remap_any(nt); /* the row the held one would pass */
        if (d >= 0 && d < g_plo_n && d != o && __atomic_load_n(&g_plo_have[d], __ATOMIC_ACQUIRE) &&
            g_plo_folder[d]) {
            __atomic_store_n(&g_drag_into_dir, dir, __ATOMIC_RELAXED);
            __atomic_store_n(&g_drag_into_disp, nt, __ATOMIC_RELAXED);
            __atomic_store_n(&g_drag_into, d, __ATOMIC_RELAXED);
        } else
            __atomic_store_n(&g_drag_target, nt, __ATOMIC_RELAXED);
        changed = 1;
    }
    if (changed) {
        __atomic_store_n(&g_drag_into_note, 1, __ATOMIC_RELEASE);
        drag_request_update();
    }
}

/* Any other list moves the held row by the whole turn, clamped to the list. */
static void drag_step_list(int delta) {
    int t = __atomic_load_n(&g_drag_target, __ATOMIC_RELAXED) + delta;
    int n = __atomic_load_n(&g_drag_rows, __ATOMIC_RELAXED);
    if (t < 0)
        t = 0;
    if (n > 0 && t > n - 1)
        t = n - 1;
    if (t != __atomic_load_n(&g_drag_target, __ATOMIC_RELAXED)) {
        __atomic_store_n(&g_drag_target, t, __ATOMIC_RELAXED);
        drag_request_update();
    }
}

static void mixer_reorder_gesture(uint8_t *mb, int delta) {
    static int press_bit = 0;
    static long long press_started = 0;
    static int took_knob = 0;    /* we are swallowing this press */
    static int back_swallow = 0; /* latched until BACK is released */
    uint8_t raw = mb[34];
    int bit_now = (mb[31] & 0x01) != 0;
    /* While a commit is with the library thread the preview stays up but the gesture is
     * over: no turns, clicks or BACKs act on it. The same holds from the moment this thread
     * records a drop or a BACK (g_dh closed) until the message tick performs it. g_drag_on is
     * published (release) after the grab reopens g_dh, so the acquire here sees it. */
    int drag_open = __atomic_load_n(&g_drag_on, __ATOMIC_ACQUIRE);
    unsigned dh_seen = dh_state(&g_dh); /* the drag this frame may end */
    int committing = __atomic_load_n(&g_drag_committing, __ATOMIC_RELAXED) || !dh_is_open(dh_seen);
    int dragging = drag_open && !committing;

    if (bit_now && !press_bit) { /* press begins */
        press_started = drag_now_ms();
        /* A press is normally EP147's: its knob handler (knobev_hook) turns a hold into the
         * grab at its 500 ms long press, and a short press stays its own click. Only a press
         * made while a row is held is ours (the drop): swallowed at the level from frame one,
         * because a bit going 0 mid-press reads to EP147 as a click. */
        took_knob = dragging;
        drag_log("PRESS dragging=%d committing=%d raw34=%u delta=%d -> %s\n", dragging,
                 committing, (unsigned) raw, delta, took_knob ? "drop (ours)" : "EP147");
        /* The drop commits on the press, not the release, saving the click's 100-600 ms: the
         * whole press is swallowed, so nothing EP147 does depends on when we act. The press
         * that grabbed never gets here; it began while dragging was 0. This thread only
         * records the drop, with the rows as they are now; the message tick performs it
         * (drag_handoff_drain): a banner and EP147's list calls do not belong on the bus read
         * thread. */
        if (dragging) {
            struct dh_intent it = {DH_COMMIT, __atomic_load_n(&g_drag_origin, __ATOMIC_RELAXED),
                                   __atomic_load_n(&g_drag_target, __ATOMIC_RELAXED),
                                   __atomic_load_n(&g_drag_into, __ATOMIC_RELAXED),
                                   __atomic_load_n(&g_drag_into_disp, __ATOMIC_RELAXED)};
            dh_post(&g_dh, dh_seen, &it);
        }
        __atomic_store_n(&g_knob_press_ms, press_started, __ATOMIC_RELAXED);
        __atomic_store_n(&g_knob_held, 0, __ATOMIC_RELEASE); /* knobev_hook arms the grab */
    }
    __atomic_store_n(&g_knob_raw, (int) raw, __ATOMIC_RELAXED);
    /* The grab is not decided here: a knob held still sends nothing new, so a grab decided
     * per frame would wait for a turn. drag_try_grab() on the message tick decides it from
     * g_knob_held / g_knob_press_ms; this path only swallows. */
    if (took_knob && bit_now)
        mb[31] &= (uint8_t) ~0x01; /* level swallow, from frame one */
    if (!bit_now && press_bit) {   /* release */
        took_knob = 0;
        __atomic_store_n(&g_knob_held, 0, __ATOMIC_RELEASE);
    }
    press_bit = bit_now;

    /* No byte-34 freeze: EP147 sees every detent, and knobturn_hook consumes them at its own
     * turn handler while a row is held, so its cursor never moves under the drag and owes
     * nothing afterwards. `ended`: a drop recorded on this very frame. Its rows are already
     * in the record, so this frame's detents no longer move the row. */
    int ended = dh_closed(&g_dh);
    if (__atomic_load_n(&g_drag_on, __ATOMIC_RELAXED) && !committing) {
        if (ended) {
            /* no turns after the drop */
        } else if (delta && __atomic_load_n(&g_drag_kind, __ATOMIC_RELAXED) == DRAG_KIND_PLIST) {
            drag_step_playlist(delta);
        } else if (delta) {
            drag_step_list(delta);
        }
        if (mb[33] & 0x80) {  /* BACK cancels */
            back_swallow = 1; /* keep swallowing until RELEASE */
            /* Recorded for the message tick. Refused when this drag's end is already
             * decided (a drop on this same frame): the drop stands. */
            struct dh_intent it = {DH_CANCEL, __atomic_load_n(&g_drag_origin, __ATOMIC_RELAXED),
                                   -1, -1, -1};
            dh_post(&g_dh, dh_seen, &it);
        }
    }
    if ((mb[33] & 0x80) && !back_swallow && __atomic_load_n(&g_mp_submode, __ATOMIC_ACQUIRE)) {
        back_swallow = 1; /* a submenu row: BACK = the parent row */
        __atomic_store_n(&g_mp_subback, 1, __ATOMIC_RELEASE);
    }
    /* The swallow must outlive the condition that started it. Cancelling clears the drag on
     * the first BACK frame, but a real press is held for dozens of frames; a swallow
     * conditioned on "is dragging" would let the rest reach EP147 and the screen would go up
     * a level. So it is latched until the bit is released. */
    if (back_swallow) {
        if (mb[33] & 0x80)
            mixer_clear_back(mb);
        else
            back_swallow = 0;
    }
}

/* ---- Drum roll HOLD (pad 7) -----------------------------------------------------
 * Per logical deck, in g_drum_hold: DRUM_HOLD_ON while the hold is on, plus
 * DRUM_HOLD_SUSTAIN once the X-PAD has played the roll under it. A sustained roll keeps
 * sounding after the finger leaves the strip, at the rate latched from the last touched
 * frame. The ErP reader turns the hold on and off (drum_hold_pads); the mixer reader only
 * adds SUSTAIN, with a compare-and-swap, so it can never revive a hold the ErP reader has
 * just cleared. The two bits are defined in azmod_internal.h. */

/* ErP reader. Pad 7 rising on an armed deck toggles its hold; pads 5 and 6 do nothing. A hold
 * belongs to the deck: it is cleared when the deck is not armed, and when this pad set moves
 * to another deck (on the deck it drove before). Returns a bit per deck whose hold changed. */
static unsigned drum_hold_pads(int d, int ld, int armed, unsigned rise8) {
    static int last_ld[AZMOD_PAD_DECKS] = {-1, -1};
    if (d < 0 || d >= AZMOD_PAD_DECKS || ld < 0 || ld >= AZMOD_STEM_DECKS)
        return 0;
    unsigned changed = 0;
    const int prev = last_ld[d];
    last_ld[d] = ld;
    if (prev >= 0 && prev != ld && __atomic_exchange_n(&g_drum_hold[prev], 0, __ATOMIC_RELAXED))
        changed |= 1u << prev;
    if (!armed) {
        if (__atomic_exchange_n(&g_drum_hold[ld], 0, __ATOMIC_RELAXED))
            changed |= 1u << ld;
    } else if (rise8 & 0x40u) {
        if (!__atomic_exchange_n(&g_drum_hold[ld], 0, __ATOMIC_RELAXED))
            __atomic_store_n(&g_drum_hold[ld], DRUM_HOLD_ON, __ATOMIC_RELAXED);
        changed |= 1u << ld;
    }
    return changed;
}

/* Mixer reader: the touch to report for an armed deck. The strip touched under a hold
 * sustains the roll; a sustained roll sounds with the finger up. A hold alone, with no roll
 * played under it, never starts one. */
static int drum_hold_touch(int ld, int touched) {
    if (ld < 0 || ld >= AZMOD_STEM_DECKS)
        return touched;
    int h = __atomic_load_n(&g_drum_hold[ld], __ATOMIC_RELAXED);
    if (touched) {
        if (h == DRUM_HOLD_ON)
            __atomic_compare_exchange_n(&g_drum_hold[ld], &h, DRUM_HOLD_ON | DRUM_HOLD_SUSTAIN, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        return 1;
    }
    return h == (DRUM_HOLD_ON | DRUM_HOLD_SUSTAIN);
}

/* ---- Mixer bus: x-pad drum roll -------------------------------------------------
 * Map the strip byte to one of six roll rates. Only with the mod enabled; the strip bytes
 * themselves are never modified. There is one strip, so the rate goes to whichever logical
 * deck holds the latch (at most one, given the exclusive arm).
 *
 * No Beat FX swallow. The strip also arms the selected Beat FX, but mixer bytes
 * 0x1c/0x1d/0x1e and 0x54/0x55/0x57 are parsed fields on the same parser (0x1d6eab0) that
 * owns `browse` (slot 128): writing them continuously takes out the browse knob and the
 * buttons above the screen for as long as drum roll is armed, and clearing 0x54 on touched
 * frames does not stop the FX from engaging. Do not write these bytes. The DJ turns the FX
 * knob down for a dry roll, or layers FX over it. */
/* The X-PAD touched in the last mixer frame (__atomic). The ErP reader reads it to end a held
 * roll whose HOLD went off while no finger is down (erp_drum_hold). */
static int g_xpad_finger;

static void mixer_xpad_drum(uint8_t *b, ssize_t r, uint64_t cfg) {
    struct bus_xpad xp; /* the x-pad bytes, one combination (azmod-xpad is read field by field) */
    bus_cfg_xpad(cfg, &xp);
    if (!__atomic_load_n(&g_drum_arm, __ATOMIC_RELAXED) || xp.pos >= (int) r)
        return;
    unsigned raw = b[xp.pos];
    int have_zone = xp.zone >= 0 && xp.zone < (int) r;
    /* Prefer the zone bitmap as the touch tell: it is per-frame and clears on release. Byte
     * 0x1c latches (0 -> 1 on the first touch, never back), so it would read as permanently
     * held. */
    int touched = have_zone                             ? b[xp.zone] != 0
                  : xp.touch >= 0 && xp.touch < (int) r ? (b[xp.touch] & xp.mask) != 0
                                                        : raw != 0;
    __atomic_store_n(&g_xpad_raw, (int) raw, __ATOMIC_RELAXED);
    __atomic_store_n(&g_xpad_finger, touched, __ATOMIC_RELAXED);
    unsigned span = (unsigned) xp.span; /* 7-bit strip: 0..124 */
    if (raw >= span)
        raw = span - 1;
    int idx = (int) ((raw * 6u) / span); /* 6 zones, left -> right */
    if (idx > 5)
        idx = 5;
    int any_held = 0;
    void (*rate_fn)(int, int) = __atomic_load_n(&g_drum_rate_fn, __ATOMIC_ACQUIRE);
    void (*touch_fn)(int, int) = __atomic_load_n(&g_drum_touch_fn, __ATOMIC_ACQUIRE);
    for (int ld = 0; ld < AZMOD_STEM_DECKS; ld++) {
        if (!__atomic_load_n(&g_drum_held[ld], __ATOMIC_RELAXED))
            continue;
        any_held = 1;
        /* The rate follows the finger only while it is on the strip. With no finger the
         * panel reports the position byte as 0xFF, which reads as the fastest zone, so an
         * untouched frame never sets the rate: the last touched zone stays latched. */
        if (rate_fn && touched)
            rate_fn(ld, idx);
        if (touch_fn) {
            const int on = drum_hold_touch(ld, touched);
            touch_fn(ld, on);
            /* A sustained roll reported while the ErP reader turns HOLD off: look again after
             * the report, so whichever reader reports last reports the end (erp_drum_hold). */
            if (on && !touched) {
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
                if (!drum_hold_touch(ld, 0))
                    touch_fn(ld, 0);
            }
        }
    }
    /* the row art follows the finger: zone while touched, idle strip when not
     * (fxrow_apply_tick turns this into a strip index, change-only) */
    __atomic_store_n(&g_fxrow_zone, (any_held && touched) ? idx : -1, __ATOMIC_RELAXED);
    if (any_held)
        bus_capture_xpad_fx(b, r, &xp, touched, have_zone);
}

/* Watchdog: no syscall, no clock, just the overlay thread's heartbeat. If it has not moved
 * for ~200 bus frames (about a second) the thread is stalled: release the input we would
 * otherwise swallow and drop the knob anchor. Failing open means a stuck overlay costs the
 * mod menu, not the deck's physical controls. Returns whether the menu counts as open. */
static int mixer_watchdog(void) {
    int menu_open = __atomic_load_n(&g_menu_open, __ATOMIC_RELAXED);
    unsigned beat = __atomic_load_n(&g_overlay_beat, __ATOMIC_RELAXED);
    if (beat != wd_last) {
        wd_last = beat;
        wd_stall = 0;
    } else if (wd_stall < 100000u)
        wd_stall++;
    if (wd_stall > 200u)
        menu_open = 0;
    return menu_open;
}

/* ---- Mixer bus: the buttons that drive the Mod Menu --------------------------- */

/* Screen-select buttons (byte 32): SOURCE 0x80, BROWSE 0x40, TAG LIST 0x20, PLAYLIST 0x10,
 * SEARCH 0x08. Any rising bit closes the menu. */
static void mixer_screen_buttons(const uint8_t *b, int menu_open) {
    static uint8_t last_tab;
    uint8_t tab = b[32];
    uint8_t rise = tab & (uint8_t) ~last_tab;
    /* Before the focus changes below: SOURCE forgets a menu to reopen, another screen button
     * remembers what is up now (mm_resume.h). */
    if (rise)
        mmr_press(&g_mm_resume, rise, menu_open, __atomic_load_n(&g_srcrow_want, __ATOMIC_RELAXED),
                  __atomic_load_n(&g_menu_focused, __ATOMIC_RELAXED));
    /* SOURCE while the full-screen Mod Menu is focused: the menu is on the SOURCE view, and
     * EP147's SOURCE-on-SOURCE goes to BROWSE. Hand the cursor back to the source list (the
     * header-back path) and swallow the press while it is held (g_src_swallow). */
    if (rise == 0x80 && menu_open && __atomic_load_n(&g_menu_focused, __ATOMIC_RELAXED)) {
        g_src_swallow = 1;
        __atomic_store_n(&g_menu_focused, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_srcrow_unfocus, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&g_srcrow_repaint, 1, __ATOMIC_RELEASE);
        rise = 0;
    }
    if (rise) {
        /* SOURCE alone with the menu hidden is the way back to it: nothing to close, and a
         * close still pending when SOURCE reopens a remembered menu would shut it again. */
        if (rise != 0x80 || menu_open)
            __atomic_store_n(&g_close_pending, 1, __ATOMIC_RELAXED);
        /* Drop the deck-view latch at once. wave_visible() is a staleness test
         * (WaveformView::paint runs 32/s while the deck view is up), so leaving the view
         * would otherwise leave the mini waveforms up for WAVE_SEEN_MS plus an overlay tick,
         * ~130 ms. This press is seen before EP147 acts on it, earlier than any JUCE
         * callback. Safe: every screen-select press leaves the deck view, and if one did
         * not, the next paint (<= 31 ms) re-marks it. Touch-driven view changes still rely
         * on the staleness window. Lowering WAVE_SEEN_MS further would make the overlay
         * blink whenever EP147 drops a frame. */
        __atomic_store_n(&g_wave_seen_ms, 0, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_viewleave_n, 1, __ATOMIC_RELAXED);
    }
    if (tab != last_tab) {
        /* SEARCH latch: set on a SEARCH press, cleared by any other screen-button press; a
         * release (tab 0) keeps it. */
        if (tab & 0x08)
            __atomic_store_n(&g_search_on, 1, __ATOMIC_RELAXED);
        else if (tab & 0xf0)
            __atomic_store_n(&g_search_on, 0, __ATOMIC_RELAXED);
    }
    last_tab = tab;
}

/* Level-based suppression, not edge-based: a physical BACK or knob click is held for dozens
 * of bus frames (~130 ms), and EP147 acts on every frame still carrying the bit. (The
 * emulator's injected press is a 1-2 frame pulse, so an edge-only clear would look fine
 * there.) While the docked menu is up (focused, or preview with our row the selected
 * source), clear BACK on every frame; while our row is EP147's selection, clear the knob
 * click on every frame (EP147's activate of our empty MediaID would leave SOURCE). The
 * edges were read before these clears and still drive the menu. Otherwise both buttons pass
 * through untouched. Returns whether the frame changed. */
static bool mixer_menu_swallow(uint8_t *b, int menu_open, int focused, int want) {
    bool mutated = false;
    int menu_up = menu_open && (focused || want);
    if (menu_up && (b[33] & 0x80)) {
        mixer_clear_back(b);
        mutated = true;
    }
    if (menu_open && want && (b[31] & 0x01)) {
        b[31] &= (uint8_t) ~0x01;
        mutated = true;
    }
    if (g_src_swallow) { /* the SOURCE press that closed the menu */
        if (b[32] & 0x80) {
            b[32] &= (uint8_t) ~0x80;
            if (!b[32] && !b[33])
                b[6] = 0x08; /* nothing else held -> idle latch */
            mutated = true;
        } else
            g_src_swallow = 0; /* released */
    }
    return mutated;
}

/* BACK, decided in one place by cursor depth, here on the bus thread, because this is where
 * EP147's copy of the button is kept or dropped and the two must agree.
 *   depth 2  submenu (PAD COLORS / SYS INFO / INPUT MON / ...): swallow; overlay goes to
 *            the top menu, cursor stays in it.
 *   depth 1  docked menu up (focused, or preview with our row the selected source): EP147's
 *            cursor never left its list, so a BACK it saw would leave the list for BROWSE.
 *            Swallow. Focused -> preview; preview stays while MOD MENU is the selected
 *            source.
 *   depth 0  our row is not selected: native, passed through.
 * The top/focus flags advance here too, so a quick second BACK is judged by the new depth.
 * If the overlay's publish races that, the worst case is two BACKs counting as one, never a
 * BACK leaking to EP147. The byte itself is cleared level-wise by mixer_menu_swallow. */
static void mixer_menu_back(int focused, int want) {
    int at_top = __atomic_load_n(&g_menu_top, __ATOMIC_RELAXED);
    int depth = !at_top ? 2 : ((focused || want) ? 1 : 0);
    int pass = 0;
    if (depth == 2) {
        __atomic_store_n(&g_menu_top, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&g_menu_focused, 1, __ATOMIC_RELAXED);
    } else if (depth == 1) {
        if (focused) /* focused -> preview; preview stays */
            __atomic_store_n(&g_menu_focused, 0, __ATOMIC_RELAXED);
    } else {
        pass = 1;
    }
    if (depth)
        __atomic_store_n(&g_back_pending, depth, __ATOMIC_RELAXED);
    bus_trace_back(depth, pass, at_top, focused, want);
}

/* NEW PLAYLIST naming in Browse: the hardware BACK is CANCEL, and EP147 must not also see it
 * (it would go up a level under the keyboard). Level-based, latched until release, like the
 * other BACK swallows. Returns whether the frame changed. */
static bool mixer_naming_back(uint8_t *b) {
    static int nm_back;
    if (__atomic_load_n(&g_nm.state, __ATOMIC_ACQUIRE) == NM_NAMING && (b[33] & 0x80) && !nm_back) {
        nm_back = 1;
        __atomic_store_n(&g_nm_cancel_req, 1, __ATOMIC_RELEASE);
    }
    if (!nm_back)
        return false;
    if (b[33] & 0x80) {
        mixer_clear_back(b);
        return true;
    }
    nm_back = 0;
    return false;
}

static void mixer_menu_buttons(uint8_t *b, int crc_in, int menu_open) {
    int click_bit = (b[31] & 0x01) != 0;
    int click_edge = click_bit && !g_last_click_bit;
    g_last_click_bit = click_bit;
    if (click_edge)
        __atomic_store_n(&g_click_pending, 1, __ATOMIC_RELAXED);
    mixer_screen_buttons(b, menu_open);

    int back_bit = (b[33] & 0x80) != 0;
    int back_edge = back_bit && !g_last_back_bit;
    g_last_back_bit = back_bit;

    int focused = __atomic_load_n(&g_menu_focused, __ATOMIC_RELAXED);
    int want = __atomic_load_n(&g_srcrow_want, __ATOMIC_RELAXED);
    bool mutated = mixer_menu_swallow(b, menu_open, focused, want);
    /* A community mod's surface holds the knob (sdk_surface.c): its click and BACK go to the
     * surface, never to EP147 or the menu, for as long as the overlay is alive. */
    if (__atomic_load_n(&g_sdk_focus, __ATOMIC_ACQUIRE) && wd_stall <= 200u) {
        if (b[31] & 0x01) {
            b[31] &= (uint8_t) ~0x01;
            mutated = true;
        }
        if (b[33] & 0x80) {
            mixer_clear_back(b);
            mutated = true;
        }
        if (back_edge)
            __atomic_store_n(&g_sdk_back_pending, 1, __ATOMIC_RELEASE);
        if (mutated && crc_in)
            frame_crc_stamp(b, 96);
        return;
    }
    /* Knob click while EP147's selection sits on our row (want) and the menu is not yet
     * focused: focus the docked menu. The click is swallowed from EP147, whose activate of
     * our empty MediaID would leave SOURCE for BROWSE. Focus and menu_open are set here, not
     * a tick later on the overlay thread, because the repaint that turns the row gray reads
     * them (as the tap path does). Only on SOURCE (g_src_visible): g_srcrow_want mirrors
     * EP147's selection and stays 1 off SOURCE (src_rows_hook re-syncs it on any source model
     * refresh), so without this a click in BROWSE would pop the full-screen menu up briefly
     * and eat the held click frames. The preview open path waits for source_visible the same
     * way. A focused click keeps g_click_pending, so the overlay drain activates the selected
     * item via azmod_input. */
    if (click_edge && !focused && __atomic_load_n(&g_src_visible, __ATOMIC_RELAXED) &&
        __atomic_load_n(&g_srcrow_want, __ATOMIC_RELAXED) &&
        __atomic_load_n(&g_srcrow_active, __ATOMIC_RELAXED)) {
        __atomic_store_n(&g_srcrow_open, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&g_menu_focused, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&g_menu_open, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&g_srcrow_repaint, 1, __ATOMIC_RELEASE); /* row -> gray */
        __atomic_store_n(&g_click_pending, 0, __ATOMIC_RELAXED);
    }
    if (menu_open && back_edge)
        mixer_menu_back(focused, want);
    if (mixer_naming_back(b))
        mutated = true;
    if (mutated && crc_in)
        frame_crc_stamp(b, 96);
    if (g_debug)
        dbg("[%ld] click_bit=%d click_edge=%d back_bit=%d back_edge=%d menu_open=%d enc_dir=%d "
            "b6=%02x b31=%02x b33=%02x\n",
            (long) time(NULL), click_bit, click_edge, back_bit, back_edge, menu_open,
            __atomic_load_n(&g_enc_dir, __ATOMIC_RELAXED), b[6], b[31], b[33]);
}

/* The mixer/panel bus frame: 96 bytes + CRC-16/X25 at 96/97. */
static void bus_mixer_frame(uint8_t *b, ssize_t r, uint64_t cfg) {
    /* Was this frame's CRC valid as it arrived? Every re-stamp is gated on it. Computed
     * before azmod touches a byte. */
    int crc_in = r >= 98 && frame_crc_ok(b, 96);
    bus_capture_mixer_in(b, r);
    /* A mixer frame with a bad CRC is not input. With a PC attached the panel delivers
     * 128-byte all-0xFF reads every few seconds, all CRC-invalid; EP147 drops them. Parsed as
     * buttons, byte 33 = ff is BACK + TAG TRACK + LOAD 1-4 all pressed and byte 34 = ff a
     * knob jump. Worse, a swallow would re-stamp a valid CRC onto the frame and EP147 would
     * act on every button at once. So a frame that fails the CRC is passed through untouched
     * and never read. */
    if (r >= 98 && !crc_in)
        return;
    if (crc_in) /* community mods see only checked input (a short frame carries no CRC) */
        sdk_filter_raw(AZ_BUS_MIXER_IN, b, (int) r);
    if (r >= 35) {
        mixer_drum_tone(b, r);
        mixer_stem_eq(b, r);
        mixer_fx_disarm(b, r);
        int delta;
        int hold = mixer_browse_knob(b, &delta);
        /* The Mod Menu's Playlist Editing toggle is the switch, re-evaluated on every mixer
         * frame: off takes effect at once (the hooks stay, inert); on needs the hooks, which
         * install only at EP147 start (see pledit_needs_reload). */
        __atomic_store_n(&g_reorder_enabled, bus_cfg_reorder(cfg), __ATOMIC_RELAXED);
        if (__atomic_load_n(&g_reorder_enabled, __ATOMIC_RELAXED) && !hold)
            mixer_reorder_gesture(b, delta);
        if (knob_anchored)
            b[34] = knob_anchor; /* sticky freeze; otherwise byte 34 is passthrough */
        /* Re-stamp after any rewrite above (the byte-34 freeze, the reorder click and BACK
         * clears). EP147 silently drops a frame whose CRC does not match, so without this a
         * frozen knob would cut off the whole mixer bus. Recomputing over an unchanged frame
         * is a no-op. */
        if (crc_in)
            frame_crc_stamp(b, 96);
    }
    if (r < 12)
        return;
    bus_capture_mixer(b, r);
    mixer_xpad_drum(b, r, cfg);
    int menu_open = mixer_watchdog();
    if (r >= 34)
        mixer_menu_buttons(b, crc_in, menu_open);
    if (crc_in && sdk_filter_run(AZ_BUS_MIXER_IN, b, (int) r)) /* community mods, after the core */
        frame_crc_stamp(b, 96);
}

/* ---- ErP bus: deck buttons and pads ------------------------------------------- */

/* Pad-mode buttons, per physical side: BEAT LOOP=0x02 enters beat loop; HOT CUE=0x01,
 * SLIP LOOP=0x04 and BEAT JUMP=0x08 leave it. Returns the rising bits. */
static unsigned erp_pad_mode(const uint8_t *eb, int d, int mode_off) {
    g_dbg_modebyte[d] = eb[mode_off];
    unsigned mrise = eb[mode_off] & ~g_erp_mode_last[d];
    g_erp_mode_last[d] = eb[mode_off];
    if (mrise & 0x02u) {
        __atomic_store_n(&g_bl_enter[d],
                         !g_pad_mode_bl[d] && !__atomic_load_n(&g_shift_down, __ATOMIC_RELAXED),
                         __ATOMIC_RELAXED);
        g_pad_mode_bl[d] = 1;
    } else if (mrise & (0x01u | 0x04u | 0x08u)) {
        g_pad_mode_bl[d] = 0;
        __atomic_store_n(&g_bl_enter[d], 0, __ATOMIC_RELAXED);
    }
    /* SLIP LOOP (bit 2) is only a seed; drum_mode_is_slip prefers EP147's own per-deck mode.
     * Bit 2 cannot be the gate: KEY SHIFT, this button's secondary, reports the same 0x04,
     * and a mode EP147 restores without a press never shows here. */
    if (mrise & 0x04u)
        g_pad_mode_slip[d] = 1;
    else if (mrise & (0x01u | 0x02u | 0x08u))
        g_pad_mode_slip[d] = 0;
    return mrise;
}

/* Deck select: rising edge on either bit of this side's pair (bytes 8/9 + d*8, bit 0),
 * tracked together so one press cannot be missed. Returns the logical deck this side now
 * drives, indexing the 4-wide stem arrays: side 0 -> deck 1 or 3, side 1 -> deck 2 or 4.
 * Pad mode stays per physical side, where the button is. Known gap: if EP147 restores a
 * different pad mode on a deck switch, the button-derived g_pad_mode_bl does not learn it. */
static int erp_deck_select(const uint8_t *eb, int d) {
    int sel_off = 8 + d * 8;
    unsigned selbits = (eb[sel_off] & 0x01u) | ((eb[sel_off + 1] & 0x01u) << 1);
    unsigned selrise = selbits & ~g_sel_last[d];
    g_sel_last[d] = selbits;
    if (selrise & 0x01u)
        g_deck_sel[d] = 0; /* primaryDeckSelect   -> deck 1/2 */
    else if (selrise & 0x02u)
        g_deck_sel[d] = 1; /* secondaryDeckSelect -> deck 3/4 */
    int ld = azmod_logical_deck(d, g_deck_sel[d]);
    if (selrise)
        dbg("deckselect side=%d sel=%d -> logical deck %d\n", d, g_deck_sel[d], ld + 1);
    return ld;
}

/* BEAT LOOP 1 -> BEAT LOOP 2: the second BEAT LOOP press flips azmod's page instead of
 * EP147's, so EP147 stays in mode 3 and page 2 is its genuine page 1 (loop lengths, tiles,
 * caption and actions all native). Only when EP147 is known in mode 3 on this deck and the
 * deck has stems engaged; unknown, or a deck without stems = native. Any other entry into BEAT LOOP passes through and lands on page 1 = stems. The
 * flip is on the rising edge; the swallow is level, per physical side, because EP147 acts
 * on any frame still carrying the bit. */
static void erp_beat_loop_page(uint8_t *eb, int d, int ld, int mode_off, unsigned mrise,
                               int crc_in) {
    static int bl_hold[AZMOD_PAD_DECKS];
    if (mrise & 0x02u) {
        /* Fresh mode only (pad_mode_fresh): the mode getter does not run off the deck
         * screen, so a raw read can be stale and would eat a real BEAT LOOP press as a page
         * flip. Stale or uncommitted = pass the press to EP147. */
        int tm = (ld >= 0 && ld < AZMOD_STEM_DECKS) ? pad_mode_fresh(d, ld) : 0;
        /* Only a deck whose stems page is up: without stems, BEAT LOOP's first page is EP147's
         * own and the press must reach EP147 to move it to its second page (bl_flip.h). */
        int st = ld >= 0 && ld < AZMOD_STEM_DECKS && __atomic_load_n(&g_erp_active[ld], __ATOMIC_RELAXED);
        if (bl_flip_swallow(bl_swap(), __atomic_load_n(&g_wave_forcestems, __ATOMIC_ACQUIRE), st, tm)) {
            __atomic_store_n(&g_bl_vpage2[ld], !__atomic_load_n(&g_bl_vpage2[ld], __ATOMIC_RELAXED),
                             __ATOMIC_RELAXED);
            bl_hold[d] = 1;
        } else if (ld >= 0 && ld < AZMOD_STEM_DECKS)
            __atomic_store_n(&g_bl_vpage2[ld], 0, __ATOMIC_RELAXED); /* EP147 takes the press */
    } else if ((mrise & (0x01u | 0x04u | 0x08u)) && ld >= 0 && ld < AZMOD_STEM_DECKS)
        __atomic_store_n(&g_bl_vpage2[ld], 0, __ATOMIC_RELAXED); /* left BEAT LOOP */
    if (!(eb[mode_off] & 0x02u))
        bl_hold[d] = 0;
    if (bl_hold[d] && crc_in) {
        eb[mode_off] &= (uint8_t) ~0x02u;
        frame_crc_stamp(eb, 28);
    }
}

/* stems_page() freshness: every edge of a mode button EP147 will see (read after the
 * page-flip swallow, so a swallowed press does not count). Press and release: EP147 switches
 * mode on the release. */
static void erp_mode_edges(const uint8_t *eb, int d, int mode_off) {
    static unsigned seen[AZMOD_PAD_DECKS];
    unsigned lvl = eb[mode_off] & 0x0Fu;
    if (lvl != seen[d]) {
        seen[d] = lvl;
        unsigned s = __atomic_add_fetch(&g_mode_seq, 1, __ATOMIC_ACQ_REL);
        __atomic_store_n(&g_mode_edge_ms[d], now_ms(), __ATOMIC_RELAXED);
        __atomic_store_n(&g_mode_edge_seq[d], s, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&g_mode_down[d], lvl != 0, __ATOMIC_RELAXED);
}

/* Pad 7 HOLD for one pad set, every ErP frame whatever the pad mode. Only a drum roll pad set
 * (drum) can toggle it. Another pad mode keeps it, so a held roll plays on under the other pads
 * and pad 7 stops it on the way back; a deck select or a disarm (which the mod off is) clears
 * it. The mixer reader turns the hold into the roll's touch. */
static void erp_drum_hold(int d, int ld, int drum, unsigned rise8) {
    if (ld < 0 || ld >= AZMOD_STEM_DECKS)
        return;
    const int held = __atomic_load_n(&g_drum_held[ld], __ATOMIC_RELAXED);
    const unsigned hc = drum_hold_pads(d, ld, held, drum ? rise8 : 0u);
    if (!hc)
        return;
    /* The ErP and mixer buses are event-driven: with no finger on the X-PAD no mixer frame
     * follows, and the mixer reader would report the end of a held roll only at the next
     * touch. A hold that went off on an armed deck ends its roll here; the mixer reader looks
     * again after its own report, so the end is never lost to a frame in flight. */
    void (*touch_fn)(int, int) = __atomic_load_n(&g_drum_touch_fn, __ATOMIC_ACQUIRE);
    if (touch_fn && !__atomic_load_n(&g_xpad_finger, __ATOMIC_RELAXED)) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        for (int k = 0; k < AZMOD_STEM_DECKS; k++)
            if ((hc & (1u << k)) && __atomic_load_n(&g_drum_held[k], __ATOMIC_RELAXED) &&
                !__atomic_load_n(&g_drum_hold[k], __ATOMIC_RELAXED))
                touch_fn(k, 0);
    }
    static int hlog = 40; /* __atomic builtins only */
    if (__atomic_fetch_sub(&hlog, 1, __ATOMIC_RELAXED) <= 0)
        return;
    FILE *f = azlog_open("/tmp/azmod-drumtile.log", "a");
    if (!f)
        return;
    for (int k = 0; k < AZMOD_STEM_DECKS; k++)
        if (hc & (1u << k))
            fprintf(f, "HOLD ld=%d hold=%d (side %d on ld=%d)\n", k,
                    __atomic_load_n(&g_drum_hold[k], __ATOMIC_RELAXED), d, ld);
    fclose(f);
}

/* Drum roll: the mod enabled and SLIP LOOP. Pad 8 arms the instrument and pads 1-4 pick the
 * sample, swallowed so the native slip-loop actions never fire underneath. */
static void erp_pads_drum_roll(uint8_t *eb, int d, int ld, int off, int mode_off,
                               unsigned padbits, unsigned rise8, int crc_in) {
    /* Pad 8 latches per logical deck and the arm is exclusive across decks 1-4: arming one
     * disarms the others. Keyed by ld, not the physical side, so decks 3/4 can arm. Rising
     * edge only, so a held pad does not oscillate. */
    if (rise8 & 0x80u) {
        int nval = !__atomic_load_n(&g_drum_held[ld], __ATOMIC_RELAXED);
        if (nval)
            drum_disarm_others(ld, "pad8 arm");
        __atomic_store_n(&g_drum_held[ld], nval, __ATOMIC_RELAXED);
        __atomic_fetch_or(&g_caption_repaint, 1u << ld, __ATOMIC_RELEASE);
        /* Bounded flip log: one press must be exactly one flip, and a multi-flip must name
         * its source. */
        {
            static _Atomic int llog = 40;
            if (__atomic_fetch_sub(&llog, 1, __ATOMIC_RELAXED) > 0) {
                FILE *f = azlog_open("/tmp/azmod-drumtile.log", "a");
                if (f) {
                    fprintf(f, "LATCH ld=%d held=%d rise8=%02x padbits=%02x mode=%02x slip=%d\n",
                            ld, __atomic_load_n(&g_drum_held[ld], __ATOMIC_RELAXED), rise8,
                            padbits, eb[mode_off], g_pad_mode_slip[d]);
                    fclose(f);
                }
            }
        }
        void (*gate)(int, int) = __atomic_load_n(&g_drum_gate_fn, __ATOMIC_ACQUIRE);
        if (gate)
            gate(ld, __atomic_load_n(&g_drum_held[ld], __ATOMIC_RELAXED));
    }
    /* Sample select only counts while this deck is armed; unarmed, a pad 1-4 press is a
     * native SLIP LOOP action. */
    if (__atomic_load_n(&g_drum_held[ld], __ATOMIC_RELAXED) && (rise8 & 0x0Fu)) {
        int sel = (rise8 & 0x01u) ? 0 : (rise8 & 0x02u) ? 1
                                    : (rise8 & 0x04u)   ? 2
                                                        : 3;
        g_drum_sel_led[ld] = (unsigned) sel;
        void (*select)(int, int) = __atomic_load_n(&g_drum_select_fn, __ATOMIC_ACQUIRE);
        if (select)
            select(ld, sel);
    }
    /* Pad 8 always (the arm toggle); pads 1-7 (samples, HOLD, and the inert 5 and 6) only while
     * this deck is armed, the same condition the LEDs use, so unarmed pads never look stock and do nothing. Because pad 8
     * never reaches EP147, its DRUM ROLL tile never draws its pressed art. Passing the bit
     * through is not an option: EP147 would fire its own 3/4 slip loop on it. The armed state
     * is shown by the FX panel overlay (d56vis) instead. */
    unsigned swallow = 0x80u | (__atomic_load_n(&g_drum_held[ld], __ATOMIC_RELAXED) ? 0x7Fu : 0u);
    if ((padbits & swallow) && crc_in) {
        eb[off] &= (uint8_t) ~swallow;
        frame_crc_stamp(eb, 28);
    }
}

/* Stems: pads 1-4 toggle mute, pads 5-8 solo vocals / inst / bass / drums; all eight are
 * swallowed so beat loop never fires underneath. Pad 8 is the drums solo here whatever the
 * Drum Roll mod is set to: drum roll owns pad 8 only in SLIP LOOP, which is decided before
 * this. Do not gate this on g_drum_arm. */
static void erp_pads_stems(uint8_t *eb, int ld, int off, unsigned padbits, unsigned rise8,
                           int fxpads, int crc_in) {
    if (rise8 & 0x0Fu) {
        g_dbg_reqraised[ld] |= rise8 & 0x0Fu;
        __atomic_or_fetch(&g_pad_req[ld], rise8 & 0x0Fu, __ATOMIC_ACQ_REL);
    }
    unsigned solo8 = fxpads ? 0u : rise8 & 0xF0u; /* FX pads: erp_side hands 5-8 to sfxp_frame */
    if (solo8)
        __atomic_or_fetch(&g_pad_solo_req[ld], (solo8 >> 4) & 0x0Fu, __ATOMIC_ACQ_REL);
    if (padbits && crc_in) {
        eb[off] &= (uint8_t) ~0xFFu;
        frame_crc_stamp(eb, 28);
    }
}

/* One physical pad set (side d) of an ErP frame. */
static void erp_side(uint8_t *eb, int d, int crc_in) {
    int mode_off = 13 + d * 8; /* byte13 deck0, byte21 deck1 */
    int off = 14 + d * 8;      /* byte14 deck0, byte22 deck1 */
    unsigned mrise = erp_pad_mode(eb, d, mode_off);
    int ld = erp_deck_select(eb, d);
    erp_beat_loop_page(eb, d, ld, mode_off, mrise, crc_in);
    erp_mode_edges(eb, d, mode_off);
    /* Deck select does not move the drum roll armed state: the latch belongs to the deck, not
     * the pad set. After a select these pads show and drive the new deck; the old deck stays
     * armed but silent, because a roll sounds only while armed and the x-pad is touched
     * (drumroll.c: `gate = g_drum_gate[d] && g_drum_touch[d]`), and touches now go to the new
     * deck. Do not add a latch transfer. */
    unsigned padbits = eb[off] & 0xFFu; /* PAD1..PAD8 (bits 0-3 pads1-4, bits 4-7 pads5-8) */
    unsigned rise8 = padbits & ~g_erp_last8[d];
    g_erp_last8[d] = padbits;
    g_erp_last[d] = padbits & 0x0Fu;
    if (rise8)
        g_dbg_padrise[d] = rise8;
    g_dbg_padbits[d] = padbits;
    /* Pads belong to drum roll in SLIP LOOP with the mod on, to stems when stems are engaged
     * and this deck shows the stems page; every other pad mode stays native. */
#ifdef AZMOD_PROBES
    {
        static unsigned last_hi[AZMOD_PAD_DECKS];
        if (((padbits ^ last_hi[d]) & 0xF0u) != 0) {
            last_hi[d] = padbits;
            int dr = __atomic_load_n(&g_drum_arm, __ATOMIC_RELAXED) && drum_mode_is_slip(d, ld);
            int act = ld >= 0 && ld < AZMOD_STEM_DECKS && __atomic_load_n(&g_erp_active[ld], __ATOMIC_RELAXED);
            int sp = act && stems_page(d, ld);
            int fm = __atomic_load_n(&g_stemfx_pads, __ATOMIC_RELAXED);
            sfxlog('B', ld, d, (padbits & 0xF0u) | (unsigned) (dr << 3 | act << 2 | sp << 1 | fm));
        }
    }
#endif
    unsigned (*fx_on)(int) = __atomic_load_n(&g_sfx_on_fn, __ATOMIC_ACQUIRE);
    int (*fx)(int, int, int) = __atomic_load_n(&g_sfx_fn, __ATOMIC_ACQUIRE);
    int fxpads = fx_on && fx && __atomic_load_n(&g_stemfx_pads, __ATOMIC_RELAXED);
    unsigned fxrise = 0;
    const int drum = __atomic_load_n(&g_drum_arm, __ATOMIC_RELAXED) && drum_mode_is_slip(d, ld);
    erp_drum_hold(d, ld, drum, rise8);
    if (drum)
        erp_pads_drum_roll(eb, d, ld, off, mode_off, padbits, rise8, crc_in);
    else if (__atomic_load_n(&g_erp_active[ld], __ATOMIC_RELAXED) && stems_page(d, ld)) {
        erp_pads_stems(eb, ld, off, padbits, rise8, fxpads, crc_in);
        if (fxpads)
            fxrise = (rise8 >> 4) & 0x0Fu;
    }
    /* Every frame, whatever the pads do now: a held FX pad's release must reach the effect it
     * started. The clock is read only while a press is new or held. */
    if (fx_on && fx) {
        static int init;
        if (!init) {
            for (int s = 0; s < AZMOD_PAD_DECKS; s++)
                sfxp_init(g_sfxp[s]);
            init = 1;
        }
        int held = 0;
        for (int f = 0; f < SFXP_N; f++)
            held |= g_sfxp[d][f].ld >= 0;
        if (fxrise || held) {
#ifdef AZMOD_PROBES
            sfxlog('F', ld, d, fxrise << 4 | ((padbits >> 4) & 0x0Fu));
            fx_on = sfx_on_logged;
            fx = sfx_fx_logged;
#endif
            sfxp_frame(g_sfxp[d], ld < AZMOD_STEM_DECKS ? ld : -1, fxrise, (padbits >> 4) & 0x0Fu,
                       (long long) now_ms(), fx_on, fx);
        }
    }
}

/* The ErP input frame: 28 bytes + CRC-16/X25 at 28/29. Byte 24 (0x18) is the event marker
 * 0x01, but it also carries the USB STOP buttons (STOP 1 = 0x02, STOP 2 = 0x04), so an exact
 * == 0x01 test would ignore every frame while a USB STOP is held. Exactly those two bits are
 * masked; anything else still disqualifies the frame. */
static void bus_erp_frame(uint8_t *eb, ssize_t r) {
    if (r < 30 || (eb[24] & (uint8_t) ~0x06u) != 0x01)
        return;
    /* A frame EP147 drops (bad CRC) must not move azmod's view of the buttons either: one
     * corrupt frame inside a held press would read as release + press, and a phantom second
     * BEAT LOOP edge would flip to BEAT LOOP 2 while EP147 saw one continuous hold. */
    int crc_in = frame_crc_ok(eb, 28);
    if (crc_in)
        g_erp_crc_ok++;
    else
        g_erp_crc_bad++;
    if (!crc_in)
        return;
    /* SHIFT: deck A byte 0x09 bit 7, deck B byte 0x11 bit 7 (field table). For "SHIFT + Hold
     * to Move". */
    __atomic_store_n(&g_shift_down, ((eb[0x09] | eb[0x11]) & 0x80) != 0, __ATOMIC_RELAXED);
    sdk_filter_raw(AZ_BUS_ERP_IN, eb, (int) r);
    for (int d = 0; d < AZMOD_PAD_DECKS; d++) /* 2 physical pad sets (byte14 + d*8) */
        erp_side(eb, d, crc_in);
    if (sdk_filter_run(AZ_BUS_ERP_IN, eb, (int) r)) /* community mods, after the core */
        frame_crc_stamp(eb, 28);
}

ssize_t read(int fd, void *buf, size_t n) {
    NEXT(next_read, "read");
    if (g_azm_inert)
        return next_read(fd, buf, n);
    ssize_t r = next_read(fd, buf, n);
    /* The Mod Menu settings for this frame, all from one published word (bus_cfg.h): the
     * overlay thread may change them at any moment, and this thread must not see half of a
     * change or wait for one. */
    const uint64_t cfg = bus_cfg();
    bus_capture_read(fd, buf, r, n, (uintptr_t) __builtin_return_address(0));
    if (g_debug < 0)
        g_debug = getenv("AZMOD_DEBUG") ? 1 : 0;
    gpiodrv_note(fd, buf, r);
    if (r < 0) /* a failed read: EP147's touch reader cannot take the -1 (touch_read_guard.h) */
        return tg_result(fd, r);
    if (r == 0 || fd < 0)
        return r;
    if (fd == g_mixer_fd)
        bus_mixer_frame(buf, r, cfg);
    else if (fd == g_erp_fd)
        bus_erp_frame(buf, r);
    else if (fd == g_deckbus_fd[0] || fd == g_deckbus_fd[1])
        sdk_filter_view(AZ_BUS_DECK_IN, buf, (int) r);
    else if (fd == g_touchin_fd)
        sdk_touch_read(buf, r);
    return r;
}
