/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file azmod.c
 * @brief LD_PRELOAD companion that adds a Mod Menu and the mods to EP147.
 *
 * EP147 is an X11/JUCE app that receives touch as X pointer events, so the
 * overlay is a plain X client: a constructor spawns a thread that opens its own
 * X connection and creates an override-redirect window sized to exactly what is
 * drawn (see wanted_rect()): the small SOURCE-bar button when closed, the whole
 * screen for the menu, settings and info screens. Because the window sits on top
 * and matches what is drawn, a tap on the button lands on us, and a full-screen
 * page swallows every click meant for EP147 underneath. Pixels come from menu.c's
 * compositor into a full-screen off-screen buffer; only the wanted sub-rect is
 * blitted. Real window geometry (not XShape over a full-screen window) is
 * deliberate; wanted_rect() explains why.
 *
 * The browse knob (rotation and click, once a menu screen is open; the SOURCE-row
 * button itself is tap-only) and the physical BACK button are a separate input
 * path. EP147 reads them directly from /dev/subucom_spi3.0, so azmod interposes
 * open()/read() on that fd. Frame layout (stable when idle):
 * 128-byte frames, bytes[10:12] = browse position (u16 LE), byte[31] bit 0x01 =
 * knob click, byte[33] bit 0x80 = BACK, byte[6] = shared press latch (0x08 idle,
 * 0x00 pressed), CRC-16/X25 over [0:96] stored at [96:98].
 *
 * Threading is deliberately lock-free. EP147's bus thread runs the read()
 * interposer and an azmod thread runs the overlay. A mutex around azmod_state
 * stalls under contention between the ~15 Hz interposer and the ~30 Hz redraw,
 * so azmod_state is owned exclusively by the overlay thread; the interposer only
 * touches a few plain ints via __atomic ops (a single-producer/single-consumer
 * flag handoff that never blocks). Swallowing bus bits is driven by g_menu_open,
 * which mirrors `azmod_state.mode != AZ_CLOSED`. That is a correct swallow
 * predicate because once a menu screen is open, azmod_input/azmod_back always
 * consume click and back; the CLOSED-state row is tap-only and never sees them.
 *
 * Mod on/off state persists to /home/root/settings/azmod.cfg (ext4 on both the
 * emulator and the deck), so toggles survive a reboot.
 *
 * The same binary runs on the emulator and the deck. AZMOD_FORCE_SOURCE=1 forces
 * the button visible regardless of SOURCE-screen detection.
 */
#define _GNU_SOURCE
#include "menu.h"
#include "azconf.h"
#include "ep147addr.h" /* EP_<v130> -> this build's EP147 address (generated table) */
#include "ep147png.h"  /* the PNG images in this build's .rodata: where stick art may go */
#include "bus_cfg.h"   /* settings word, color words and LED blocks the bus threads read */
#include "bl_flip.h"   /* BEAT LOOP press: azmod's page flip or EP147's */
#include "stem_fx_pads.h" /* pads 5-8 as Stem FX pads: tap latches, hold is momentary */
#include "../common/azlogcap.h" /* every /tmp append is size-capped */
#include "../common/azprot.h"   /* every write into EP147's image: unlock, write, restore */
#include "../common/azspawn.h"  /* helpers start with posix_spawn: no fork of EP147, no shell */
#include "../common/azaltstack.h"    /* every thread we start has an alternate signal stack */
static volatile int g_azm_inert = 1; /* every interposer passes through until azmod_init
                                        has proven this is EP147 with a complete address
                                        table; the unit's other processes, and an unknown
                                        EP147 build, stay inert for good */
static int g_ep147_build;        /* EPB_* from ep147build.h, set in azmod_init */
#include "caption_strips.h"
#include "srcref.h"
#include "wavref.h"
#include "wave_att.h" /* the stem waveform height rule (stems, toggles, Stem Volume) */
#include <dirent.h>
#include <dlfcn.h>
#include <link.h> /* dl_iterate_phdr: EP147's image bounds (img_q) */
#include <linux/input.h>   /* struct input_event, ABS_MT_* (touch replay, dev builds) */
#include <sys/sysmacros.h> /* major() */
#include <math.h>
#include <sys/uio.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <float.h> /* DBL_MAX: a BpmState with no track */
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <termios.h> /* bt_uart_recover: the Bluetooth UART speed */
#include <poll.h>
#include <sys/timex.h> /* adjtimex: is the system clock synchronized (the Clock readout) */
#include "clock_tz.h"   /* the Clock readout's zone file and local time */
#include "clock_set.h"  /* Set Time Manually: when a set time may be shown */
#include <linux/rtc.h>  /* RTC_SET_TIME: Set Time Manually also sets the RTC */
#include "mm_resume.h"  /* reopen the Mod Menu when SOURCE comes back */
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/XShm.h> /* the overlay's frames through a shared segment */
#include <sys/shm.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/syscall.h>

/* Owned exclusively by the overlay thread. */
static azmod_state g_state;
static azmod_fb g_fb;
static int g_srcrow_active; /* native SOURCE-list row hook installed (see install_srcrow_hook) */
/* Native menu-item rows: while the docked menu is open, the Mod Menu
 * items render as SOURCE-list rows below our MOD MENU row. g_srcrow_menu is the
 * "item rows exist" flag: set in src_click_hook (the same click that opens the
 * menu, so the re-render it triggers already paints the items) and mirrored by
 * the overlay thread at every transition; wanted_rect also parks the overlay
 * window on it. g_srcrow_item is the selected item for knob-click activation;
 * g_srcrow_act hands an activation to the overlay thread so every g_state write
 * stays on azmod_input's thread. */
static volatile int g_srcrow_menu;
static volatile int g_srcrow_item;
static volatile int g_srcrow_act;

#define CONFIG_PATH "/home/root/settings/azmod.cfg"
/* /tmp (RAM, wiped on reboot), never /home/root/settings: screenshots are 3 MB
 * each, and settings is a 61 MB partition holding SETTINGS.DAT, cabinet.img and
 * the azmod overlay. Filling it wedges EP147's state writes and the deck loses
 * its BPM/analysis display. */
#define SHOTS_DIR "/tmp/azmod-shots"
/* Per-deck stem control masks, shared with azstem.so. Deck 1's name has no
 * suffix for compatibility. */
static const char *STEMS_CTRL_PATH[AZMOD_STEM_DECKS] = {
    "/home/root/settings/azstem.stems",
    "/home/root/settings/azstem.stems2",
    "/home/root/settings/azstem.stems3",
    "/home/root/settings/azstem.stems4",
};
#define ENABLE_CTRL_PATH "/home/root/settings/azstem.enable" /* master; STEMS mod toggle */
/* Replace a file on the settings partition whole: write <path>.new, rename it over
 * <path>. A reader, a full partition or a power cut then sees the old content or the
 * new one, never a truncated file (a truncated azmod.cfg resets every Mod Menu setting;
 * a reader of azstem.stems catching the empty moment of an in-place rewrite drops that
 * deck's stems for a poll). durable = 1 also fsyncs the file before the rename and the
 * directory after it, for settings a user would notice losing; 0 for runtime state
 * that is re-derived at every start. Returns 0 on success; on failure <path> is
 * untouched. */
static int settings_write(const char *path, const char *buf, size_t len, int durable) {
    char tmp[512];
    if (snprintf(tmp, sizeof tmp, "%s.new", path) >= (int) sizeof tmp)
        return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            break;
        off += (size_t) w;
    }
    int ok = off == len && (!durable || fsync(fd) == 0);
    if (close(fd) != 0)
        ok = 0;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    if (durable) {
        char dir[512];
        snprintf(dir, sizeof dir, "%s", path);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) {
            *slash = 0;
            int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (dfd >= 0) {
                fsync(dfd);
                close(dfd);
            }
        }
    }
    return 0;
}
/* Stem strips -> azstem.so, per deck. A deck's stems engage whenever a deck
 * screen is up (not SOURCE), the master (STEMS mod) is on, and that deck's
 * track has stems; on SOURCE / master-off the deck plays normally (value 0).
 * bit7 engaged | bits0-3 = enabled stems (VOCAL/INST/BASS/DRUM). */
static void write_stems(const azmod_state *s) {
    for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
        unsigned v = 0;
        if (azmod_stems_engine(s) && s->stems_available[d]) {
            v = 0x80u;
            for (int i = 0; i < AZMOD_STEM_COUNT; i++)
                if (s->stem_on[d][i])
                    v |= (1u << i);
        }
        char line[16];
        int n = snprintf(line, sizeof line, "%u\n", v);
        settings_write(STEMS_CTRL_PATH[d], line, (size_t) n, 0);
    }
}
/* Pad -> azstem stem mapping. azstem bit order: 0 vocals, 1 other (INST),
 * 2 bass, 3 drums. Pads 1-4 toggle one stem each (VOCAL/INST/BASS/DRUM, the
 * overlay strip order); pads 5-8 solo; mute-all lives in the menu. */
#define PAD_MUTEALL 0xfu /* no pad uses it; mod_menu_overlay.c still tests for it */
static const unsigned char PAD_TGL[4] = {0x1u, 0x2u, 0x4u, 0x8u};
static const int PAD_LEDSTEM[4] = {0, 1, 2, 3};

/* Which stems this deck's track has (bit k = slot k), from azstem_present();
 * 0 = unknown (nothing loaded yet, or an azstem without the export) and then
 * nothing is hidden. */
static unsigned (*g_stem_present_fn)(int deck);
static unsigned g_stem_pm[AZMOD_STEM_DECKS];
/* Build the 32-byte pad-LED block for deck d into out[32]. Entry = {R, G, B, 0};
 * brightness is the RGB magnitude (e.g. hot-cue green = 10 ff 00 00,
 * beat-loop orange = c1 63 00 00, idle = 08 0b 04 00). Pads 1-4 show the stem
 * color (full when on, dim when off); pads 5-8 are SOLO, white. */
/* Stem FX pads (stem_fx_pads.h): set by the overlay before it builds a deck's LEDs. */
static int g_sfx_led_mode;                       /* 1 = pads 5-8 are FX pads */
/* bit f = effect f on (azstem_stem_fx_on); __atomic: the message thread reads it for the tile art */
static unsigned g_sfx_led_on[AZMOD_STEM_DECKS];
static void build_pad_led(const azmod_state *s, int d, unsigned char *out) {
    /* Same per-stem colors as the overlay strip. */
    static const unsigned char RGB[AZMOD_STEM_COUNT][3] = {
        {0x00, 0xff, 0x00}, /* pad1 VOCAL  green  */
        {0xff, 0x00, 0x00}, /* pad2 INST   red    */
        {0xff, 0x00, 0xc0}, /* pad3 BASS   magenta */
        {0x00, 0x00, 0xff}, /* pad4 DRUMS  blue   */
    };
    for (int p = 0; p < AZMOD_STEM_COUNT; p++) { /* pads 1-4 */
        int on;
        if (PAD_LEDSTEM[p] < 0) {
            on = 0;
            for (int k = 0; k < AZMOD_STEM_COUNT; k++)
                on |= s->stem_on[d][k];
        } else
            on = s->stem_on[d][PAD_LEDSTEM[p]];
        unsigned char *e = out + p * 4;
        if (PAD_LEDSTEM[p] >= 0 && g_stem_pm[d] && !(g_stem_pm[d] >> PAD_LEDSTEM[p] & 1u)) {
            e[0] = e[1] = e[2] = e[3] = 0; /* no such stem: dark, the no-stems tell */
            continue;
        }
        e[0] = on ? RGB[p][0] : (unsigned char) (RGB[p][0] >> 3); /* R (dim = >>3) */
        e[1] = on ? RGB[p][1] : (unsigned char) (RGB[p][1] >> 3); /* G */
        e[2] = on ? RGB[p][2] : (unsigned char) (RGB[p][2] >> 3); /* B */
        e[3] = 0;
    }
    /* Pads 5-8 SOLO: bright white on the soloed stem (the only one on, which is
     * what a solo press leaves; pressing it again un-solos), dim on the rest,
     * dark where the track has no such stem. LED values are linear light, so
     * dim 0x14 (~1/12 of full) matches pads 1-4's >>3 step. */
    int non = 0, which = -1;
    for (int k = 0; k < AZMOD_STEM_COUNT; k++)
        if (s->stem_on[d][k]) {
            non++;
            which = k;
        }
    /* FX pads: the pad palette's SPRING green, bright while the effect is on and dim while
     * ready, dark where the track has no such stem. Pad order VOCAL ECHO, INST ECHO, INST BRAKE,
     * DRUMS ECHO. */
    static const int FX_STEM[4] = {0, 1, 1, 3};
    static const azmod_pad_color FX_PAL[AZMOD_PAD_PALETTE_COUNT] = AZMOD_PAD_PALETTE;
    const uint32_t fx_led = FX_PAL[17].led; /* 17 SPRING */
    for (int i = 0; g_sfx_led_mode && i < 4 && AZMOD_STEM_COUNT + i < 8; i++) {
        unsigned char *e = out + (AZMOD_STEM_COUNT + i) * 4;
        if (g_stem_pm[d] && !(g_stem_pm[d] >> FX_STEM[i] & 1u)) {
            e[0] = e[1] = e[2] = e[3] = 0;
            continue;
        }
        unsigned on = (__atomic_load_n(&g_sfx_led_on[d], __ATOMIC_RELAXED) >> i) & 1u;
        const unsigned sh = on ? 0 : 3; /* the dim step of pads 1-4 */
        e[0] = (unsigned char) (((fx_led >> 16) & 0xffu) >> sh);
        e[1] = (unsigned char) (((fx_led >> 8) & 0xffu) >> sh);
        e[2] = (unsigned char) ((fx_led & 0xffu) >> sh);
        e[3] = 0;
    }
    for (int i = 0; !g_sfx_led_mode && i < AZMOD_STEM_COUNT && AZMOD_STEM_COUNT + i < 8; i++) {
        unsigned char *e = out + (AZMOD_STEM_COUNT + i) * 4;
        if (g_stem_pm[d] && !(g_stem_pm[d] >> i & 1u)) {
            e[0] = e[1] = e[2] = e[3] = 0;
            continue;
        }
        unsigned char v = (non == 1 && which == i) ? 0xff : 0x14;
        e[0] = e[1] = e[2] = v;
        e[3] = 0;
    }
    for (int i = 2 * AZMOD_STEM_COUNT; i < 8; i++) {
        unsigned char *e = out + i * 4;
        e[0] = e[1] = e[2] = e[3] = 0;
    }
}

/* Mirror the stem engine (the Stems mod) to azstem.so's audio enable. */
static void write_enable(const azmod_state *s) {
    settings_write(ENABLE_CTRL_PATH, azmod_stems_engine(s) ? "1" : "0", 1, 0);
}

/* Playlist Editing (mod 18) -> the flag install_reorder() reads at EP147 start. */
/* Phase Meter (mod 36) has no gate flag; it is off by default. */
/* Day Mode's live switch. Defined here, not in day_mode.c, because azmod's own
 * screen detection reads it: title_match looks for dark ink while EP147 paints
 * light. Always 0 when day_mode.c (the only writer) is compiled out. */
static _Atomic int g_dm_on;
#ifdef AZMOD_HAVE_DAYMODE
static _Atomic int g_dm_flip; /* the active theme flips lightness (Day); 0 for the dark themes */
#define G_DM_FLIP g_dm_flip
#else
#define G_DM_FLIP \
    g_dm_on /* no themes: both are always 0, so this compiles to the themeless code */
#endif
#ifdef AZMOD_HAVE_DAYMODE
int azmod_dm_on(void) {
    return __atomic_load_n(&g_dm_on, __ATOMIC_RELAXED);
} /* menu.h azm_theme */
#endif
/* Themes' hooks install at every EP147 start so the switch is live without a
 * reload; when off, each hook is a load of g_dm_on and a pass-through. The art
 * recorder must be in before EP147 decodes its art, so this cannot wait until
 * Themes is turned on. Kill switch: settings/azmod-themes.off (no hooks at all;
 * the row still shows). */
static int daymode_wanted(void) {
    return access("/home/root/settings/azmod-themes.off", F_OK) != 0;
}
/* ---- performance: fast UI core (part of 60 FPS Waveforms) ------------------
 * EP147 paints on its message thread (tid == pid) and pins it to cpu0-3, the
 * A53s, where it saturates at ~83% delivering ~41 paints/s (two tracks
 * playing, 60 FPS on). On the A72 cpu5 the same thread reaches 60 paints/s (the
 * display rate) at ~50%, SoC +2 C. cpu4 is as fast but is isolated for audio
 * (isolcpus=4, JUCE ALSA FIFO 98), so it is left alone. cpu5 also carries Xorg
 * (RR 49) and the SPI bus threads (RT 99); both outrank the paint thread (RR 1).
 * Off restores the mask EP147 gave the thread. */
#define FASTUI_CPU 5
static long now_ms(void);
static inline int ui60_clock_wanted(const azmod_state *s); /* with g_ui60_on */
static FILE *azlog_open(const char *path, const char *mode);
static void fastui_log(const char *fmt, ...) {
    FILE *f = azlog_open("/tmp/azmod-perf.log", "a");
    if (!f)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}
/* cpu5 must be online and a Cortex-A72 (part 0xd08), or we do nothing. */
static int fastui_cpu_ok(void) {
    char b[8] = "";
    FILE *f = fopen("/sys/devices/system/cpu/cpu5/online", "re");
    if (!f || !fgets(b, sizeof b, f) || b[0] != '1') {
        if (f)
            fclose(f);
        return 0;
    }
    fclose(f);
    f = fopen("/proc/cpuinfo", "re");
    if (!f)
        return 0;
    char line[128];
    int proc = -1, ok = 0;
    while (fgets(line, sizeof line, f)) {
        int n;
        unsigned part;
        if (sscanf(line, "processor : %d", &n) == 1)
            proc = n;
        else if (proc == FASTUI_CPU && sscanf(line, "CPU part : %x", &part) == 1) {
            ok = part == 0xd08;
            break;
        }
    }
    fclose(f);
    return ok;
}
/* Clear the rest of cpu5 so the paint thread has it to itself. Stock start-up
 * pins Xorg (RR 49), EP147's SendManager (RT 50) and the spi master kthreads
 * (RT 99) to cpu5. Left there, they take 24% of cpu5 from the paint thread
 * (~3300 preemptions/s) and four decks stall at ~55 paints/s; moved to cpus 1-3,
 * four decks reach 58.8-59.9 paints/s with no audio or control impact.
 * The subucom_spi* kthreads stay: they are kernel-bound (PF_NO_SETAFFINITY).
 * Identity: Xorg by its exe, SendManager by thread name inside EP147, spi by
 * exact comm and the kthread flag. Each original mask is saved at first move
 * and restored when the feature goes off. */
#define FASTUI_AUX_MAX 16
static struct {
    pid_t tid;
    cpu_set_t orig;
} g_fu_aux[FASTUI_AUX_MAX];
static int g_fu_aux_n;
static int fu_comm_is(const char *dir, const char *want) {
    char p[96], c[32] = "";
    snprintf(p, sizeof p, "%s/comm", dir);
    FILE *f = fopen(p, "re");
    if (!f)
        return 0;
    if (!fgets(c, sizeof c, f))
        c[0] = 0;
    fclose(f);
    c[strcspn(c, "\n")] = 0;
    return !strcmp(c, want);
}
static int fu_is_kthread(pid_t pid) {
    char p[64], b[512];
    snprintf(p, sizeof p, "/proc/%d/stat", (int) pid);
    FILE *f = fopen(p, "re");
    if (!f)
        return 0;
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    char *q = strrchr(b, ')');
    if (!q)
        return 0;
    unsigned long flags = 0;
    int field = 2;
    char *sv = NULL;
    for (char *t = strtok_r(q + 1, " ", &sv); t; t = strtok_r(NULL, " ", &sv))
        if (++field == 9) {
            flags = strtoul(t, NULL, 10);
            break;
        }
    return (flags & 0x00200000UL) != 0; /* PF_KTHREAD */
}
static int fu_collect(pid_t *out, int max) {
    int n = 0;
    char dir[64];
    DIR *d = opendir("/proc");
    struct dirent *e;
    if (!d)
        return 0;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9')
            continue;
        pid_t pid = (pid_t) atoi(e->d_name);
        snprintf(dir, sizeof dir, "/proc/%d", (int) pid);
        char exe[128] = "";
        char lp[80];
        snprintf(lp, sizeof lp, "%s/exe", dir);
        ssize_t k = readlink(lp, exe, sizeof exe - 1);
        if (k > 0)
            exe[k] = 0;
        size_t el = strlen(exe);
        if (el >= 5 && !strcmp(exe + el - 5, "/Xorg")) {
            out[n++] = pid;
            continue;
        }
        if (k <= 0 &&
            (fu_comm_is(dir, "spi1") || fu_comm_is(dir, "spi2") || fu_comm_is(dir, "spi3") ||
             fu_comm_is(dir, "spi5")) &&
            fu_is_kthread(pid))
            out[n++] = pid;
    }
    closedir(d);
    snprintf(dir, sizeof dir, "/proc/%d/task", (int) getpid());
    if ((d = opendir(dir))) {
        while ((e = readdir(d)) && n < max) {
            if (e->d_name[0] < '1' || e->d_name[0] > '9')
                continue;
            char td[96];
            if (snprintf(td, sizeof td, "%s/%s", dir, e->d_name) >= (int) sizeof td)
                continue; /* never act on a truncated path */
            if (fu_comm_is(td, "SendManager"))
                out[n++] = (pid_t) atoi(e->d_name);
        }
        closedir(d);
    }
    return n;
}
static void fu_aux_apply(void) { /* move off cpu5 (re-asserted each 5 s) */
    pid_t t[FASTUI_AUX_MAX];
    int n = fu_collect(t, FASTUI_AUX_MAX);
    cpu_set_t want;
    CPU_ZERO(&want);
    CPU_SET(1, &want);
    CPU_SET(2, &want);
    CPU_SET(3, &want);
    for (int i = 0; i < n; i++) {
        int k;
        for (k = 0; k < g_fu_aux_n; k++)
            if (g_fu_aux[k].tid == t[i])
                break;
        cpu_set_t cur;
        CPU_ZERO(&cur);
        if (sched_getaffinity(t[i], sizeof cur, &cur) != 0)
            continue;
        if (k == g_fu_aux_n) {
            if (g_fu_aux_n >= FASTUI_AUX_MAX)
                continue;
            g_fu_aux[k].tid = t[i];
            g_fu_aux[k].orig = cur;
            g_fu_aux_n++;
        }
        if (CPU_EQUAL(&cur, &want))
            continue;
        int rc = sched_setaffinity(t[i], sizeof want, &want);
        fastui_log("fastui: tid %d -> cpu1-3 rc=%d\n", (int) t[i], rc);
    }
}
static void fu_aux_restore(void) {
    for (int k = 0; k < g_fu_aux_n; k++) {
        int rc = sched_setaffinity(g_fu_aux[k].tid, sizeof g_fu_aux[k].orig, &g_fu_aux[k].orig);
        fastui_log("fastui: tid %d -> its own mask rc=%d\n", (int) g_fu_aux[k].tid, rc);
    }
    g_fu_aux_n = 0;
}
static void fastui_tick(azmod_state *s, int *dirty) {
    static int checked, cpu_ok, applied;
    static cpu_set_t orig;
    static long next;
    pid_t main_tid = getpid(); /* the message thread's tid == the pid */
    if (!checked) {
        checked = 1;
        cpu_ok = fastui_cpu_ok();
        CPU_ZERO(&orig);
        if (sched_getaffinity(main_tid, sizeof orig, &orig) != 0)
            cpu_ok = 0;
        fastui_log("fastui: cpu%d usable=%d, EP147's own mask has %d cpus\n", FASTUI_CPU, cpu_ok,
                   CPU_COUNT(&orig));
    }
    char note[80] = "";
    long now = now_ms();
    /* The fast-core placement has no toggle of its own; it follows 60 FPS. */
    if (ui60_clock_wanted(s)) { /* either 60 FPS toggle: both paint on this thread */
        if (!cpu_ok)
            snprintf(note, sizeof note, "cpu%d is not an online A72: left alone", FASTUI_CPU);
        else {
            if (!applied || now >= next) { /* re-check every 5 s that it held */
                next = now + 5000;
                cpu_set_t cur;
                CPU_ZERO(&cur);
                if (sched_getaffinity(main_tid, sizeof cur, &cur) == 0 &&
                    !(CPU_COUNT(&cur) == 1 && CPU_ISSET(FASTUI_CPU, &cur))) {
                    cpu_set_t want;
                    CPU_ZERO(&want);
                    CPU_SET(FASTUI_CPU, &want);
                    int rc = sched_setaffinity(main_tid, sizeof want, &want);
                    fastui_log("fastui: paint thread -> cpu%d rc=%d%s\n", FASTUI_CPU, rc,
                               applied ? " (re-asserted)" : "");
                }
                fu_aux_apply();
                applied = 1;
            }
            snprintf(note, sizeof note, "UI paints on cpu%d (fast core)", FASTUI_CPU);
        }
    } else {
        if (applied) {
            int rc = sched_setaffinity(main_tid, sizeof orig, &orig);
            fastui_log("fastui: paint thread -> EP147's own mask rc=%d\n", rc);
            fu_aux_restore();
            applied = 0;
        }
        if (cpu_ok)
            snprintf(note, sizeof note, "UI paints on cpu0-3 (stock)");
    }
    {
        static char last[80];
        if (strcmp(note, last)) {
            snprintf(last, sizeof last, "%s", note);
            fastui_log("fastui: %s\n", note);
        }
    }
    (void) dirty;
}

/* Debug-only logging, gated on the /tmp/azmod-debug probe flag (always off in
 * release builds). */
static int g_debug = -1; /* -1=unchecked, 0=off, 1=on */

/* ---- per-process log paths ------------------------------------
 * azmod is preloaded into every process in EP147.service's cgroup: LD_PRELOAD
 * comes from systemd drop-ins on the unit, and Environment= applies unit-wide
 * (apl_start.sh, EP147, edb_streamd, wpa_supplicant, wpa_cli and a respawning
 * `sh -c ping` / `ping` pair). The constructors are guarded on
 * /proc/self/comm == "EP147", so no threads, hooks or scans run elsewhere, and
 * g_azm_inert stays 1 outside EP147, so every interposer (open, close, dup2,
 * dup3, lseek, recvmsg, ioctl, read, sqlite3_*) is a plain pass-through there.
 * edb_streamd (EP147's exec'd child) is the only other process holding the
 * subucom fds, and only by inheritance (it never open()s them itself); sqlcipher is
 * mapped only in EP147.
 *
 * EP147 keeps the plain filenames the tooling expects; every other process gets
 * a ".<comm>" suffix so a stray writer shows up as its own file. */
static const char *az_logsfx(void) {
    static char sfx[48];
    static int init;
    if (!init) {
        init = 1;
        sfx[0] = 0;
        char comm[32] = {0};
        FILE *cf = fopen("/proc/self/comm", "re");
        if (cf) {
            if (fgets(comm, sizeof comm, cf)) {
                char *nl = strchr(comm, '\n');
                if (nl)
                    *nl = 0;
            }
            fclose(cf);
        }
        if (strcmp(comm, "EP147") != 0)
            snprintf(sfx, sizeof sfx, ".%s", comm[0] ? comm : "unknown");
    }
    return sfx;
}
/* Every azmod diagnostic opens through here, so this is where the size ceiling
 * lives. A runaway writer can fill tmpfs and silently swallow every other log.
 * Past the cap an append open truncates the file, says so, and carries on;
 * 256 KB holds hours of a change-only diagnostic. One fseek/ftell per open is
 * fine because these are event writers, not per-frame ones. */
#define AZLOG_MAX_BYTES (256L * 1024)
/* Diagnostics are off unless settings/azmod-logs.on exists at start, so users'
 * decks do no debug logging; every caller handles NULL. Functional /tmp state
 * (azmod-disp60, the crash reporter's own fd) does not go through here. The flag
 * is checked once, since turning logs on is a decision taken before a reload. */
#define AZLOG_FLAG "/home/root/settings/azmod-logs.on"
static int g_azlog_on = -1;
static FILE *azlog_open(const char *base, const char *mode) {
    if (g_azlog_on < 0)
        g_azlog_on = access(AZLOG_FLAG, F_OK) == 0;
    if (!g_azlog_on)
        return 0;
    const char *sfx = az_logsfx();
    char path[320];
    snprintf(path, sizeof path, "%s%s", base, sfx);
    /* every fd azmod holds inside EP147 is close-on-exec: EP147 forks children
     * (sh -c ping ...) that would otherwise inherit it. The wrapper adds the 'e'
     * so its callers pass plain "a"/"w". */
    char cmode[8];
    snprintf(cmode, sizeof cmode, "%se", mode);
    FILE *f = fopen(path, cmode); /* cloexec-ok: cmode = mode + "e" */
    if (!f || mode[0] != 'a')
        return f;
    /* fopen("a") leaves ftell() at 0 on this libc until the first write, so the
     * size must be read after an explicit seek. */
    if (fseek(f, 0, SEEK_END) == 0) {
        long sz = ftell(f);
        if (sz > AZLOG_MAX_BYTES) {
            fclose(f);
            f = fopen(path, "we");
            if (f)
                fprintf(f, "--- log truncated at %ld bytes ---\n", sz);
        }
    }
    return f;
}

static _Atomic int g_disp60_armed; /* defined with starttimer_hook */
static void dbg(const char *fmt, ...) {
    if (g_debug < 0)
        g_debug = AZ_PROBE_ON("/tmp/azmod-debug") ? 1 : 0; /* release: off */
    if (!g_debug)
        return;
    FILE *f = azlog_open("/tmp/azmod-debug.log", "a");
    if (!f)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

/* ---- lock-free handoff from the mixer-bus interposer thread ---- *
 * Producer (interposer thread): only ever stores into these.
 * Consumer (overlay thread): drains (exchange-to-zero) once per tick. */
static volatile int g_enc_dir = 0; /* signed detents since the previous drain; a count, not a
                                    * direction, so a fast turn keeps every detent */
static volatile int g_click_pending = 0;
static volatile int g_back_pending = 0;
/* The screen-select buttons above the display (SOURCE / BROWSE / TAG LIST /
 * PLAYLIST / SEARCH / MENU) share one mixer-bus byte; pressing any of them means
 * the user is leaving, so the mod menu closes. The press is not swallowed, so
 * EP147 still switches screens underneath. */
static volatile int g_close_pending = 0;
static int
    g_src_swallow; /* bus thread only: SOURCE closed the focused menu; drop it until released */
/* What the Mod Menu showed when SOURCE was left, to show again when it comes back
 * (mm_resume.h levels). Written by the bus thread (screen-select presses) and the JUCE
 * message thread (SOURCE view hidden / shown, a real source picked); __atomic only. */
static int g_mm_resume;
/* A resume asked for the page the menu showed, not just its category (mm_resume.h
 * mmr_page_*): set by the message thread, taken by the overlay thread; __atomic only. */
static int g_mm_page_armed;
static volatile int g_search_on = 0; /* SEARCH screen-select bit live */
static volatile int g_menu_open =
    0; /* mirrors g_state.mode != AZ_CLOSED; overlay thread writes it */
/* Two-pane focus: g_menu_open = visible (PREVIEW or FOCUSED, menu docked on the
 * right). g_menu_focused = FOCUSED only: azmod owns the knob (anchor freeze, and
 * dir/click go to azmod_input). In PREVIEW the menu shows but EP147 keeps the
 * knob on the left list, so hovering never freezes the bus. */
static volatile int g_menu_focused = 0;
/* Overlay-thread heartbeat. g_menu_open gates the interposer's swallowing of
 * CLICK/BACK and only the overlay thread writes it, so if that thread stalls or
 * dies with the menu open, the interposer would eat every press forever while
 * touch keeps working (EP147 handles touch itself). The interposer therefore
 * treats a stale heartbeat as menu-closed and stops holding input. */
static volatile unsigned g_overlay_beat = 0;
static unsigned wd_last, wd_stall; /* watchdog state, read() thread only */
/* Knob-ownership diagnostics (scrolling up out of the Mod Menu): the bus thread
 * only counts (no syscalls); the overlay thread logs them when logs are on. */
static _Atomic unsigned g_kn_leak, g_kn_rel, g_kn_wdmax, g_kn_frames;

/* ---- erp-bus (/dev/subucom_spi1.0) pad -> stem handoff ---- *
 * Pads ride the ErP bus as 128-byte event frames: byte24 == 0x01 marks an event,
 * the per-button bitmask is at byte14 for deck 1 and byte22 for deck 2 (+8
 * stride), bits 0..3 = PAD1..PAD4, and bytes 28-29 hold CRC-16/X25 over [0:28].
 * Pads 1..4 toggle stems VOCAL/INST/BASS/DRUM. The interposer sets a per-deck
 * toggle request on the rising edge; the overlay flips g_state.stem_on and
 * republishes. The interposer swallows pads 1..4 (clears the bits and fixes the
 * CRC, leaving pads 5..8) only while that deck's stems are engaged, which the
 * overlay publishes into g_erp_active (same predicate as write_stems). */
static int g_erp_fd = -1; /* set in open() */
/* The Mod Menu settings the bus threads act on, as one word (bus_cfg.h). Written only by the
 * overlay thread (bus_cfg_publish), read once per frame by read()/ioctl(). */
static uint64_t g_bus_cfg = BC_DEFAULT; /* only ever touched with __atomic builtins */
static inline uint64_t bus_cfg(void) {
    return __atomic_load_n(&g_bus_cfg, __ATOMIC_ACQUIRE);
}
#include "azmod_internal.h"
#include "touch_read_guard.h"
#include "bus_interpose.c"
#include "drum_roll_display.c"

/* ---- USB mod loader --------------------------------------------------------
 * The threat model is at the call site in the startup path. In short: a mod
 * from a stranger's stick may change EP147's assets and nothing else.
 *
 * The firmware tag a mod must name. An asset address is only meaningful for the
 * EP147 build it was extracted from, so a new base firmware needs a new tag;
 * existing mods then stop applying instead of writing into whatever now
 * occupies that address. */
static const char *azmod_fw_tag(void) {
    return g_ep147_build == EPB_V200 ? "v200" : "v135";
}
/* A struct offset that moved between EP147 builds (the address tables cover
 * addresses, not layouts). Only a known build reaches this; an unknown one leaves
 * azmod inert, so there is no third case. Each use names the instruction that
 * reads the field. */
static int ep_off(int v130, int v200) {
    return g_ep147_build == EPB_V200 ? v200 : v130;
}
/* Both functions above answer for exactly two builds. Adding a row to EPB_KNOWN without
 * giving them a case would hand the new build 1.30's offsets and asset tag. */
_Static_assert(sizeof EPB_KNOWN / sizeof *EPB_KNOWN == 2,
               "a new EP147 build: give ep_off() and azmod_fw_tag() a case for it first");
#define AZMOD_FW_TAG azmod_fw_tag()

static void usbmod_log(const char *fmt, ...) {
    /* The only writer, append only: a file opened "a" here and "w" elsewhere
     * would erase its own evidence. */
    FILE *f = azlog_open("/tmp/azmod-usbmods.log", "a");
    if (!f)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

static int az_is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* EP147's .rodata range, read from the binary's ELF section headers, not from
 * /proc/self/maps. The linker packs .rodata into an executable segment:
 *     02a94000-02d92000 r-xp   <- .rodata lives here, mapped executable
 *     03b1f000-03b25000 r--p   <- 24 KB; .init_array and friends
 * so "the read-only non-executable mapping" contains none of the assets, and a
 * loader bounded by it would refuse every real mod. The section headers say
 * .rodata is 0x25c0aa0 + 0xebf688, flagged A-- (allocated, not writable, not
 * executable), regardless of how the segment around it was merged. */
static int az_rodata_range(uintptr_t *lo, uintptr_t *hi) {
    static const char *paths[] = {"/home/root/pdj/EP147", "/usr/pdj/EP147"};
    for (size_t pi = 0; pi < sizeof paths / sizeof paths[0]; pi++) {
        FILE *f = fopen(paths[pi], "rbe");
        if (!f)
            continue;
        unsigned char eh[64];
        if (fread(eh, 1, sizeof eh, f) != sizeof eh ||
            memcmp(eh,
                   "\x7f"
                   "ELF",
                   4) ||
            eh[4] != 2) {
            fclose(f);
            continue;
        }
        uint64_t shoff;
        uint16_t shentsize, shnum, shstrndx;
        memcpy(&shoff, eh + 0x28, 8);
        memcpy(&shentsize, eh + 0x3a, 2);
        memcpy(&shnum, eh + 0x3c, 2);
        memcpy(&shstrndx, eh + 0x3e, 2);
        if (!shnum || shentsize < 64 || shstrndx >= shnum) {
            fclose(f);
            continue;
        }

        /* section header for the section-name string table */
        unsigned char sh[64];
        if (fseek(f, (long) (shoff + (uint64_t) shstrndx * shentsize), SEEK_SET) ||
            fread(sh, 1, 64, f) != 64) {
            fclose(f);
            continue;
        }
        uint64_t stroff, strsize;
        memcpy(&stroff, sh + 0x18, 8);
        memcpy(&strsize, sh + 0x20, 8);
        if (strsize == 0 || strsize > (1u << 20)) {
            fclose(f);
            continue;
        }
        char *strs = malloc((size_t) strsize);
        if (!strs) {
            fclose(f);
            continue;
        }
        if (fseek(f, (long) stroff, SEEK_SET) || fread(strs, 1, (size_t) strsize, f) != strsize) {
            free(strs);
            fclose(f);
            continue;
        }

        int ok = 0;
        for (uint16_t i = 0; i < shnum && !ok; i++) {
            if (fseek(f, (long) (shoff + (uint64_t) i * shentsize), SEEK_SET))
                break;
            if (fread(sh, 1, 64, f) != 64)
                break;
            uint32_t nameoff;
            uint64_t flags, addr, size;
            memcpy(&nameoff, sh + 0x00, 4);
            memcpy(&flags, sh + 0x08, 8);
            memcpy(&addr, sh + 0x10, 8);
            memcpy(&size, sh + 0x20, 8);
            if (nameoff >= strsize)
                continue;
            if (strcmp(strs + nameoff, ".rodata"))
                continue;
            if (flags & 0x1)
                break; /* writable: not the section we want */
            if (!addr || !size)
                break;
            *lo = (uintptr_t) addr;
            *hi = (uintptr_t) (addr + size);
            ok = 1;
        }
        free(strs);
        fclose(f);
        if (ok)
            return 1;
    }
    return 0;
}

/* Where art from a stick may go. Bounding writes to .rodata is not enough: it holds the PNGs
 * but also vtables and other read-only data, and overwriting a vtable entry redirects a virtual
 * call. Checking that the target "looks like a PNG" is not enough either, because that check
 * reads live memory: one blob could plant a fake PNG with oversized chunk lengths inside a real
 * image, and the next blob, aimed at it, would be measured past the image into whatever follows.
 *
 * So a target must be the start of one of this build's PNG images, listed in ep147png.h (built
 * from the pristine binary by tools/ep147-pngtable.py), and the blob must be a PNG no longer
 * than that image. An unknown build has no images, so everything is refused.
 * Returns the image's length, or 0 = refused. */
static const unsigned char AZ_PNG_SIG[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
static size_t az_art_target_len(uintptr_t va, const unsigned char *blob, size_t n) {
    uint32_t len = ep147png_len(g_ep147_build, va);
    if (!len || n < 8 || n > len || memcmp(blob, AZ_PNG_SIG, 8))
        return 0;
    return len;
}

static int usb_dir_named(const char *parent, const char *name, char *out, size_t outsz) {
    DIR *d = opendir(parent);
    if (!d)
        return 0;
    struct dirent *de;
    int found = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.')
            continue;
        if (strcasecmp(de->d_name, name))
            continue;
        snprintf(out, outsz, "%s/%s", parent, de->d_name);
        if (az_is_dir(out))
            found = 1;
        break;
    }
    closedir(d);
    return found;
}

/* Same roots and depth as azstem's drum_find_base, so a stick the deck already
 * finds DRUM ROLLS on is a stick it finds MODS on. */
static int usbmods_find(char *out, size_t outsz) {
    const char *e = getenv("AZMOD_MODS_DIR");
    if (e && *e && az_is_dir(e)) {
        snprintf(out, outsz, "%s", e);
        return 1;
    }
    static const char *roots[] = {"/media/usb", "/run/media", "/mnt", "/media"};
    for (size_t r = 0; r < sizeof roots / sizeof roots[0]; r++) {
        if (usb_dir_named(roots[r], "MODS", out, outsz))
            return 1;
        DIR *d = opendir(roots[r]);
        if (!d)
            continue;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.')
                continue;
            char sub[1024];
            snprintf(sub, sizeof sub, "%s/%s", roots[r], de->d_name);
            if (!az_is_dir(sub))
                continue;
            if (usb_dir_named(sub, "MODS", out, outsz)) {
                closedir(d);
                return 1;
            }
            DIR *d2 = opendir(sub);
            if (!d2)
                continue;
            struct dirent *de2;
            while ((de2 = readdir(d2))) {
                if (de2->d_name[0] == '.')
                    continue;
                char sub2[1600];
                snprintf(sub2, sizeof sub2, "%s/%s", sub, de2->d_name);
                if (az_is_dir(sub2) && usb_dir_named(sub2, "MODS", out, outsz)) {
                    closedir(d2);
                    closedir(d);
                    return 1;
                }
            }
            closedir(d2);
        }
        closedir(d);
    }
    return 0;
}

/* mod.txt: `target=<tag>`, one key per line, everything else ignored. */
static int usbmod_target_ok(const char *dir) {
    char path[1200];
    snprintf(path, sizeof path, "%s/mod.txt", dir);
    FILE *f = fopen(path, "re");
    if (!f) {
        usbmod_log("  refused: no mod.txt\n");
        return 0;
    }
    char line[256];
    int ok = 0, saw = 0;
    while (fgets(line, sizeof line, f)) {
        char *v = strchr(line, '=');
        if (!v || strncasecmp(line, "target", 6))
            continue;
        v++;
        while (*v == ' ' || *v == '\t')
            v++;
        char *e = v + strlen(v);
        while (e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '))
            *--e = 0;
        saw = 1;
        ok = strcmp(v, AZMOD_FW_TAG) == 0;
        if (!ok)
            usbmod_log("  refused: built for '%s', this deck is '%s'\n", v, AZMOD_FW_TAG);
        break;
    }
    fclose(f);
    if (!saw)
        usbmod_log("  refused: mod.txt has no target=\n");
    return ok;
}

static int usbmod_apply(const char *dir, uintptr_t lo, uintptr_t hi) {
    int applied = 0;
    for (int i = 0; i < 4096; i++) {
        char bf[1300], af[1300];
        snprintf(bf, sizeof bf, "%s/%d.bin", dir, i);
        snprintf(af, sizeof af, "%s/%d.addr", dir, i);
        FILE *fh = fopen(af, "re");
        if (!fh)
            break;
        unsigned long va = 0;
        int got = fscanf(fh, "%lx", &va);
        fclose(fh);
        if (got != 1)
            break;
        fh = fopen(bf, "rbe");
        if (!fh)
            continue;
        static unsigned char buf[65536];
        size_t n = fread(buf, 1, sizeof buf, fh);
        fclose(fh);
        if (n == 0)
            continue;

        /* The blob must lie wholly inside .rodata, and replace one of this build's
         * images from its first byte, with a PNG that fits it (az_art_target_len). */
        if (va < lo || va >= hi || (uintptr_t) (va + n) > hi) {
            usbmod_log("  blob %d REFUSED: %#lx+%zu outside .rodata [%#lx,%#lx)\n", i, va, n,
                       (unsigned long) lo, (unsigned long) hi);
            continue;
        }
        size_t orig = az_art_target_len((uintptr_t) va, buf, n);
        if (!orig) {
            usbmod_log(
                "  blob %d REFUSED: %#lx+%zu is not a PNG replacing one of EP147 %s's images "
                "from its start, within its length\n",
                i, va, n, ep147_build_name(g_ep147_build));
            continue;
        }
        /* Write access added for the copy only; the pages get back exactly the
         * protection they had (.rodata may share the r-x segment with the code). */
        azprot_t pt;
        if (azprot_unlock((uintptr_t) va, n, &pt) != 0) {
            usbmod_log("  blob %d failed: cannot make %#lx+%zu writable\n", i, va, n);
            continue;
        }
        memcpy((void *) (uintptr_t) va, buf, n);
        if (azprot_relock(&pt) != 0)
            usbmod_log("  blob %d: page protection not restored (%s)\n", i, strerror(errno));
        applied++;
    }
    return applied;
}

static void usbmods_load(void) {
    /* Log the bound first, unconditionally. Otherwise "refused everything
     * because the .rodata range was not found" and "nothing on the stick to do"
     * produce the same empty log. */
    uintptr_t lo = 0, hi = 0;
    int have_range = az_rodata_range(&lo, &hi);
    if (have_range)
        usbmod_log("rodata=[%#lx,%#lx) fw=%s\n", (unsigned long) lo, (unsigned long) hi,
                   AZMOD_FW_TAG);
    else
        usbmod_log("rodata NOT IDENTIFIED -- every mod will be refused "
                   "(no bound, no safe write). fw=%s\n",
                   AZMOD_FW_TAG);

    char mods[1024];
    if (!usbmods_find(mods, sizeof mods)) {
        usbmod_log("no MODS folder found on any mounted stick\n");
        return;
    }
    usbmod_log("MODS: %s\n", mods);
    if (!have_range)
        return;

    DIR *d = opendir(mods);
    if (!d)
        return;
    struct dirent *de;
    int mods_ok = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.')
            continue;
        char sub[1200];
        snprintf(sub, sizeof sub, "%s/%s", mods, de->d_name);
        if (!az_is_dir(sub))
            continue;
        usbmod_log("mod '%s':\n", de->d_name);
        if (!usbmod_target_ok(sub))
            continue;
        int n = usbmod_apply(sub, lo, hi);
        usbmod_log("  applied %d blob(s)\n", n);
        if (n)
            mods_ok++;
    }
    closedir(d);
    usbmod_log("%d mod(s) applied\n", mods_ok);
}

static volatile int g_erp_active[AZMOD_STEM_DECKS] = {0}; /* stems engaged -> act + swallow */
/* No-stems indicator: the STEMS master is on but that deck's track has no
 * stems. The interposer then leaves the deck's BEAT LOOP button native orange
 * (no green) and paints its pad block dark while in Beat Loop, the at-a-glance
 * "this track has no stems" state. */
static volatile int g_erp_stemless[AZMOD_STEM_DECKS] = {0};
static unsigned g_erp_last[AZMOD_STEM_DECKS] = {0};      /* interposer-only: last pad bits (edge) */
static unsigned g_erp_mode_last[AZMOD_STEM_DECKS] = {0}; /* interposer-only: last mode-btn bits */
static int g_pad_mode_bl[AZMOD_STEM_DECKS] = {0};        /* interposer-only: 1 = Beat Loop button */
/* BEAT LOOP has two pages and the ERP mode byte reports only the button, so
 * g_pad_mode_bl is also true on page 2. EP147's own per-deck mode
 * (modeget_hook -> g_true_mode_deck, stored m+1) tells the pages apart:
 * 3 = page 1, 4 = page 2. Stems own one page (stems_bl_mode). Unknown (never
 * dispatched) is treated as the stems page: only a known other page turns the
 * stems surface off. */
static _Atomic int g_true_mode_deck[AZMOD_STEM_DECKS];
/* Which page stems take over is a setting (mod 21 "Stems on BEAT LOOP 2"):
 * EP147 mode 3 (page 1, the default) or 4 (BEAT LOOP 2). Every consumer asks
 * these helpers, never a literal 3/4, so the page is one decision. */
static inline int stems_bl_mode(void) { /* any thread: the published word, never g_state */
    return (bus_cfg() & BC_STEMSBL2) ? 4 : 3;
}
/* The BEAT LOOP swap (whenever mod 21 is off): EP147 stays in mode 3 on both
 * pages and azmod keeps the page itself, per logical deck; the ERP hook swallows
 * the page-flip press and flips this instead. 1 = the user is on page 2, which
 * shows EP147's genuine page 1 (the stems takeover stands down). Written by the
 * ERP hook, read by the stems gates and the re-dispatch. */
static _Atomic int g_bl_vpage2[AZMOD_STEM_DECKS];
/* The swap is the default, since most users never use BEAT LOOP 2: stems on
 * BEAT LOOP, BEAT LOOP's own loop pads on BEAT LOOP 2, whenever "Stems on BEAT
 * LOOP 2" (mod 21) is off. */
static inline int bl_swap(void) {
    return !(bus_cfg() & BC_STEMSBL2);
}
static inline int bl_on_native_vpage(int deck) {
    return bl_swap() && deck >= 0 && deck < AZMOD_STEM_DECKS &&
           __atomic_load_n(&g_bl_vpage2[deck], __ATOMIC_RELAXED);
}
/* 1 = this deck is known to be on the BEAT LOOP page stems do not own: EP147's
 * other page, or (with the swap) azmod's own page 2. */
static inline int bl_page2(int deck) {
    return (deck >= 0 && deck < AZMOD_STEM_DECKS &&
            __atomic_load_n(&g_true_mode_deck[deck], __ATOMIC_RELAXED) ==
                (7 - stems_bl_mode()) + 1) ||
           bl_on_native_vpage(deck);
}
/* Is the stems surface up on this side's pads (LEDs, pad colors, pad swallow,
 * lane strip, the Beat Loop 2 green)? One answer for all of them.
 *
 * The button flag flips on the press edge while EP147 changes mode on the
 * release, so the button alone would show stems during a hold that ends on a
 * native page. EP147's own mode (g_true_mode_deck) is the truth, but only while
 * fresh: the getter runs when the pad grid is painted, so off the deck screen
 * (BROWSE) it keeps its last value through any number of presses. So:
 *   - published after this side's last mode-button edge: EP147 decides, and
 *     only the stems page (stems_bl_mode) is stems;
 *   - not yet (a mode button is down, or went up < 400 ms ago): EP147 has not
 *     committed, so never guess ahead of it;
 *   - stale beyond that (off the deck screen): the button-based answer.
 * The edge is taken from the ERP byte after azmod's own page-flip swallow, so
 * a press EP147 never sees (mod 21 off, bl_hold) does not wait for a mode
 * change that will not come. */
static _Atomic unsigned g_mode_seq;                       /* bumped per mode-button edge */
static _Atomic unsigned g_mode_edge_seq[AZMOD_PAD_DECKS]; /* g_mode_seq at this side's last edge */
static _Atomic long g_mode_edge_ms[AZMOD_PAD_DECKS];
static _Atomic int g_mode_down[AZMOD_PAD_DECKS]; /* a mode button EP147 sees is held */
static _Atomic unsigned
    g_tm_seq[AZMOD_STEM_DECKS]; /* g_mode_seq when this deck's mode was last read */
/* EP147's pad mode for logical deck `ld`, judged against this side's last
 * mode-button edge: > 0 = fresh (mode + 1, published after that edge);
 * 0 = not committed (a mode button is down, or went up < 400 ms ago);
 * -1 = stale (off the deck screen the getter does not run). The one answer to
 * "which pad mode is this side in", shared by stems_page() and
 * drum_mode_is_slip(). */
static int pad_mode_fresh(int side, int ld) {
    /* seq before mode: the getter stores mode then seq, so a seq this new
     * guarantees the mode is at least as new. */
    unsigned ts = __atomic_load_n(&g_tm_seq[ld], __ATOMIC_ACQUIRE);
    int tm = __atomic_load_n(&g_true_mode_deck[ld], __ATOMIC_ACQUIRE);
    if (tm && (int) (ts - __atomic_load_n(&g_mode_edge_seq[side], __ATOMIC_ACQUIRE)) >= 0)
        return tm;
    if (__atomic_load_n(&g_mode_down[side], __ATOMIC_RELAXED) ||
        now_ms() - __atomic_load_n(&g_mode_edge_ms[side], __ATOMIC_RELAXED) < 400)
        return 0;
    return -1;
}
/* The last BEAT LOOP press on this side entered it from another mode (no SHIFT),
 * so EP147 lands on BEAT LOOP's first page; known before its own getter catches up. */
static _Atomic int g_bl_enter[AZMOD_PAD_DECKS];
/* Its last answer, per side, for while a mode button is held (below). */
static _Atomic int g_sp_last[AZMOD_PAD_DECKS];
static int stems_page(int side, int ld) {
    if (side < 0 || side >= AZMOD_PAD_DECKS || ld < 0 || ld >= AZMOD_STEM_DECKS)
        return 0;
    int fm = pad_mode_fresh(side, ld);
    /* A mode button is held: EP147 changes pad mode on the release, so nothing
     * has changed yet; keep the last answer. Dropping the stems surface on the
     * press would show EP147's own all-orange BEAT LOOP pads for the whole hold. */
    if (fm == 0 && __atomic_load_n(&g_mode_down[side], __ATOMIC_RELAXED))
        return __atomic_load_n(&g_sp_last[side], __ATOMIC_RELAXED);
    int r;
    /* EP147's own fresh mode first. azmod's button guess (g_pad_mode_bl) changes
     * on the press, while EP147 keeps reporting BEAT LOOP until the release, so
     * the guess only stands in when EP147's report is not fresh. */
    if (fm > 0)
        r = fm == stems_bl_mode() + 1 && !bl_on_native_vpage(ld);
    else if (!g_pad_mode_bl[side] || bl_on_native_vpage(ld))
        r = 0;
    /* Released, not committed yet. An entry into BEAT LOOP from another mode lands
     * on its first page, so with stems there it is the stems page now (otherwise
     * EP147's orange pads flash for ~20 ms). A second press inside BEAT LOOP
     * flips pages, so that one waits. */
    else if (fm == 0)
        r = __atomic_load_n(&g_bl_enter[side], __ATOMIC_RELAXED) && stems_bl_mode() != 4 &&
            !bl_page2(ld);
    /* Stale. Stems on page 1 (default): entering BEAT LOOP lands on stems.
     * Stems on BEAT LOOP 2: entering lands on EP147's native page 1, so only a
     * page EP147 itself reported as the stems page counts. */
    else
        r = stems_bl_mode() == 4 ? __atomic_load_n(&g_true_mode_deck[ld], __ATOMIC_ACQUIRE) == 4 + 1
                                 : !bl_page2(ld);
    __atomic_store_n(&g_sp_last[side], r, __ATOMIC_RELAXED);
    return r;
}
/* Which logical deck each physical side is driving, so a stem-pad press lands
 * on the right deck's stem mask. Byte layout (EP147's deck badge: blue = primary,
 * white = secondary):
 *     side A: byte 0x08 bit0 = primaryDeckSelect   byte 0x09 bit0 = secondaryDeckSelect
 *     side B: byte 0x10 bit0 = primaryDeckSelect   byte 0x11 bit0 = secondaryDeckSelect
 * i.e. base = 8 + side*8, primary at base+0 bit0, secondary at base+1 bit0. The
 * sides are independent, and the buttons SELECT rather than toggle. Both bits
 * pass through: azmod only observes them.
 *
 * Tracking by observation means the bus-read thread never dereferences an EP147
 * heap object. EP147 starts both sides on their primary deck and azmod lives in
 * its process, so 0 is the correct initial state and a restart re-syncs. */
static int g_deck_sel[AZMOD_PAD_DECKS] = {0}; /* 0 = primary (deck 1/2), 1 = secondary (deck 3/4) */
static unsigned g_sel_last[AZMOD_PAD_DECKS] = {0}; /* edge state for the two select bits */
/* Diagnostics recorded by the ErP interposer and logged by the overlay thread.
 * Never do file I/O in the interposer: it runs inside read(), and fopen would
 * re-enter it. */
static volatile unsigned g_dbg_modebyte[AZMOD_STEM_DECKS];  /* raw pad-mode byte */
static volatile unsigned g_dbg_padbits[AZMOD_STEM_DECKS];   /* raw pad bits */
static volatile unsigned g_dbg_padrise[AZMOD_STEM_DECKS];   /* last rising edge seen */
static volatile unsigned g_dbg_reqraised[AZMOD_STEM_DECKS]; /* cumulative pad_req raised */
#include "pad_leds.c"
static void vs_save_flush(void); /* vinyl_slider.c: the overlay thread writes the saved value */
static void wz_tick(long now); /* wave_zoom.c: Remember Waveform Zoom's saver, overlay thread */
static void load_config(azmod_state *s); /* the settings block, below */
static int settings_import_pending(void);  /* likewise */
#include "link_members.c" /* PRO DJ LINK player numbers present (Stem Volume) */
#include "mod_menu_overlay.c"
/* ==== settings: azmod.conf (the authority) + azmod.cfg (1.2.2's file, dual-written) =======
 *
 * Boot (load_settings):
 *   - azmod.conf valid: it is the authority.
 *   - otherwise (absent, invalid, or a newer version, which is never overwritten): migrate
 *     1.2.2's files (azmod.cfg line 1 by the slot -> key map of features.def's cfg column,
 *     lines 2-3, the flag files, azmod-theme / -vinyl.v / -btchannel; azconf.c holds the
 *     rules, the old files are never touched), apply that, and save azmod.conf;
 *   - neither file: the defaults, nothing written;
 *   - a valid azmod.conf without a "theme" or "vinyl.brake" key (written before they moved
 *     into it): that key is imported once from azmod-theme / azmod-vinyl.v.
 * Save (save_config): azmod.conf, and azmod.cfg exactly as 1.2.2 wrote it (dual write) so a
 * rollback to 1.2.2 and any tool that reads line 1 keep working. Retired slots are
 * written '0'. The theme (g_azmod_theme) and the vinyl value (g_vb_milli) are azmod.conf
 * keys: applied at every load, captured at every save; azmod-theme and azmod-vinyl.v are
 * no longer written. azmod-btchannel stays a per-deck file; azmod.conf carries a copy
 * (migrated, refreshed at each save).
 * Overlay (UI) thread only, after azmod_init's early load (constructor, before any hook and
 * before the overlay thread exists). */
#ifndef CONF_PATH /* the host tests point both at a scratch directory */
#define CONF_PATH "/home/root/settings/azmod.conf"
#define SETTINGS_DIR "/home/root/settings"
#endif
#ifndef IMPORT_PATH /* the host tests point it at a scratch directory */
/* The mods stick's MODS/azmods.cfg, handed over by the loader (az-usbload) when the stick's
 * file changed since the last sync. It wins over the deck copy. RAM, so a reboot drops it;
 * the loader hands it over again. A file azmod cannot load is renamed .bad (kept, so the
 * loader never writes the deck copy back over the stick's file). */
#define IMPORT_PATH "/run/azmods-cfg.import"
#endif
/* the overlay's 1 Hz poll: a hand-over waiting */
static int settings_import_pending(void) {
    return access(IMPORT_PATH, F_OK) == 0;
}
static struct azconf *g_conf;  /* the settings as last loaded / saved */
static int g_conf_readonly;    /* a newer build's azmod.conf: read through azmod.cfg, never written */
static int g_settings_early;   /* load_settings_early has run: the values are loaded */
/* Fine Vinyl Speed Adjust's value (vinyl_brake.c, which initialises it to -1): set here at
 * every load from "vinyl.brake", read at every save. */
static _Atomic int g_vb_milli;
static void conf_apply_small(const struct azconf *c); /* theme + vinyl.brake, below */
/* Remember Waveform Zoom's saved level (wave_zoom.c): set here at every load from "wave.zoom",
 * written back at every save. WZ_NONE = none. __atomic builtins only. */
#include "wave_zoom.h"
static uint32_t g_wz_saved = WZ_NONE;
static void wz_conf_apply(const struct azconf *c) { /* absent or not a level: none */
    __atomic_store_n(&g_wz_saved, wz_parse(azconf_get(c, "wave.zoom")), __ATOMIC_RELAXED);
}
static void wz_conf_capture(struct azconf *c) { /* a conf that never had one gets no key */
    char w[WZ_TEXT];
    wz_format(w, sizeof w, __atomic_load_n(&g_wz_saved, __ATOMIC_RELAXED));
    if (w[0] || azconf_get(c, "wave.zoom"))
        azconf_set(c, "wave.zoom", w);
}
static void azconf_dbg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char line[512];
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    dbg("%s", line);
}
/* features.def -> azconf_migrate_legacy's map: the key of each azmod.cfg slot, the radio set
 * (the drum banks) and the rules' slot numbers, all from the table */
static const char *g_slot_key[AZ_CFG_WIDTH];
static int g_radio_slots[8], g_def_on[AZ_CFG_WIDTH], g_force_on[AZ_CFG_WIDTH], g_retired[AZ_CFG_WIDTH],
    g_int_slots[AZ_CFG_WIDTH];
static struct azconf_radio g_radio;
static struct azconf_legacy_defaults g_legacy;
static void legacy_map(void) {
    int nd = 0, nf = 0, nr = 0, ni = 0;
    memset(g_slot_key, 0, sizeof g_slot_key);
    for (int i = 0; i < AZF_COUNT; i++) {
        const struct az_row *r = az_row(i);
        if (r->cfg < 0 || !az_has_value(i))
            continue;
        if (r->radio_n) {
            g_radio.key = r->d.key;
            g_radio.n = r->radio_n < 8 ? r->radio_n : 8;
            for (int k = 0; k < g_radio.n; k++)
                g_radio_slots[k] = r->cfg + k;
            g_radio.slots = g_radio_slots;
            continue;
        }
        g_slot_key[r->cfg] = r->d.key;
        if (r->d.kind == AZ_ROW_CHOICE) /* saved as its index (conf_capture): migrate it so too */
            g_int_slots[ni++] = r->cfg;
        if (r->d.flags & AZ_ROW_DEFAULT_ON)
            g_def_on[nd++] = r->cfg;
        if (r->d.flags & AZ_ROW_FORCE_ON)
            g_force_on[nf++] = r->cfg;
    }
    for (int k = 0; k < AZ_CFG_WIDTH; k++)
        if (az_cfg_retired(k))
            g_retired[nr++] = k;
    const char *const *themes;
    int nt = azmod_theme_keys(&themes);
    /* clang-format off */
    g_legacy = (struct azconf_legacy_defaults){
        .default_on = g_def_on, .n_default_on = nd, .force_on = g_force_on, .n_force_on = nf,
        .force_off = g_retired, .n_force_off = nr,
        .slot_fps = az_row(AZF_OVERLAY_FPS)->cfg, .slot_overlays = az_row(AZF_OVERLAYS)->cfg,
        .slot_pledit = az_row(AZF_PLAYLIST_EDITING)->cfg,
        .slot_remotestems = az_row(AZF_REMOTE_STEMS)->cfg, .slot_ui60ui = az_row(AZF_FPS60_UI)->cfg,
        .palette_count = AZMOD_PAD_PALETTE_COUNT, .themes = themes, .n_themes = nt,
        .radio = &g_radio, .n_radio = g_radio.key ? 1 : 0, .int_slots = g_int_slots,
        .n_int_slots = ni};
    /* clang-format on */
}
/* One row's saved value: a TOGGLE is on/off; a CHOICE its index (a 2-way CHOICE migrated
 * from an azmod.cfg char reads as on = 1, off = 0). Absent or out of range = the default. */
static int conf_value(const struct azconf *c, int item, const char *key) {
    const struct az_row *r = az_row(item);
    if (r->d.kind == AZ_ROW_CHOICE) {
        int n = r->d.n_choices, b = azconf_get_bool(c, key, -1);
        long v = azconf_get_int(c, key, -1, 0, n - 1);
        if (v >= 0)
            return (int) v;
        if (b >= 0 && n == 2)
            return b;
        return r->d.default_choice;
    }
    int def = (r->d.flags & AZ_ROW_DEFAULT_ON) ? 1 : 0;
    return azconf_get_bool(c, key, def) != 0;
}
static int8_t conf_pad(const struct azconf *c, const char *key) {
    const char *v = azconf_get(c, key);
    if (!v || !strcmp(v, "native"))
        return -1;
    long k = azconf_get_int(c, key, -1, 0, AZMOD_PAD_PALETTE_COUNT - 1);
    return (int8_t) k;
}
/* the conf's "pinned" list: is key in it */
static int conf_pinned(const struct azconf *c, const char *key) {
    const char *v = azconf_get(c, "pinned");
    size_t n = strlen(key);
    for (const char *p = v; p && *p;) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t) (e - p) : strlen(p);
        if (len == n && !strncmp(p, key, n))
            return 1;
        p = e ? e + 1 : p + len;
    }
    return 0;
}
static void conf_apply(const struct azconf *c, azmod_state *s) {
    char key[64];
    for (int i = 0; i < AZF_COUNT; i++)
        if (azf_conf_key(i, key, sizeof key))
            __atomic_store_n(&az_val[i], conf_value(c, i, key), __ATOMIC_RELAXED);
    az_pins_clear();
    for (int i = 0; i < AZF_COUNT; i++)
        if (azf_conf_key(i, key, sizeof key) && conf_pinned(c, key))
            az_pin_toggle(i);
    az_after_load(); /* forced rows; the 60 FPS memory's "everything" stored as empty */
    conf_apply_small(c);
    if (!s)
        return;
    s->pad_colors_on = azconf_get_bool(c, "padcolor.on", 1) != 0;
    for (int j = 0; j < AZMOD_PADMODE_COUNT; j++) {
        snprintf(key, sizeof key, "padcolor.%d", j);
        s->padmode_color[j] = conf_pad(c, key);
        snprintf(key, sizeof key, "padcolor2.%d", j);
        s->padmode_color2[j] = conf_pad(c, key);
    }
}
/* the small files' current values into the conf's copy (they stay authoritative) */
static FILE *settings_open(const char *name) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", SETTINGS_DIR, name);
    return fopen(p, "re");
}
/* One small file's value into the conf's copy: the value when the file holds a valid one,
 * else empty (the file is absent or unreadable: azmod reads the default). The files stay
 * authoritative; the copy is refreshed at every load and save, so it is never stale. */
static void conf_copy_file(struct azconf *c, const char *key, const char *file,
                           int (*valid)(const char *)) {
    char w[32] = {0};
    FILE *f = settings_open(file);
    int ok = f && fscanf(f, "%31s", w) == 1 && valid(w);
    if (f)
        fclose(f);
    if (ok)
        azconf_set(c, key, w);
    else if (azconf_get(c, key))
        azconf_set(c, key, "");
}
static int theme_ok(const char *w) {
    const char *const *themes;
    int nt = azmod_theme_keys(&themes);
    for (int k = 0; k < nt; k++)
        if (!strcmp(w, themes[k]))
            return 1;
    return 0;
}
static int vinyl_ok(const char *w) { /* "0.xx" or "1.00" */
    return (w[0] == '0' && w[1] == '.' && w[2] >= '0' && w[2] <= '9' && w[3] >= '0' && w[3] <= '9' &&
            !w[4]) ||
           !strcmp(w, "1.00");
}
static int btchannel_ok(const char *w) {
    return !strcmp(w, "3") || !strcmp(w, "4");
}
static void conf_refresh_files(struct azconf *c) {
    conf_copy_file(c, "bluetooth.channel", "azmod-btchannel", btchannel_ok);
}
/* The theme, the vinyl value and the clock's time zone from the conf: absent or invalid = day / the
 * stock values / UTC. */
static void conf_apply_small(const struct azconf *c) {
    const char *const *themes;
    int nt = azmod_theme_keys(&themes), th = 0;
    const char *v = azconf_get(c, "theme");
    for (int k = 0; v && k < nt; k++)
        if (!strcmp(v, themes[k]))
            th = k;
    __atomic_store_n(&g_azmod_theme, th, __ATOMIC_RELAXED);
    v = azconf_get(c, "vinyl.brake");
    int m = v && vinyl_ok(v) ? (int) (strtod(v, NULL) * 1000.0 + 0.5) : -1;
    g_vb_milli = m; /* _Atomic */
    int tz = azmod_clock_tz_index(azconf_get(c, "clock.tz")); /* absent or unknown: UTC */
    __atomic_store_n(&g_azmod_clock_tz, tz < 0 ? 0 : tz, __ATOMIC_RELAXED);
    wz_conf_apply(c);
}
static void conf_capture_small(struct azconf *c) {
    const char *const *themes;
    int nt = azmod_theme_keys(&themes), th = __atomic_load_n(&g_azmod_theme, __ATOMIC_RELAXED);
    azconf_set(c, "theme", themes[th >= 0 && th < nt ? th : 0]);
    int m = g_vb_milli;
    char w[16] = "";
    if (m >= 0 && m <= 1000)
        snprintf(w, sizeof w, "%.2f", (double) m / 1000.0);
    azconf_set(c, "vinyl.brake", w); /* empty = never set: the stock values */
    azconf_set(c, "clock.tz", azmod_clock_tz_zone(__atomic_load_n(&g_azmod_clock_tz, __ATOMIC_RELAXED)));
    wz_conf_capture(c);
}
/* A conf written before the theme and the vinyl value moved into it: each key it lacks is
 * imported once from its old file (when that holds a valid value). The key is then present
 * (the next save writes it), so the file is never read again. */
static void conf_import_small(struct azconf *c) {
    char w[32];
    static const struct {
        const char *key, *file;
        int (*ok)(const char *);
    } k[] = {{"theme", "azmod-theme", theme_ok}, {"vinyl.brake", "azmod-vinyl.v", vinyl_ok}};
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        if (azconf_get(c, k[i].key))
            continue;
        FILE *f = settings_open(k[i].file);
        w[0] = 0;
        if (f && fscanf(f, "%31s", w) == 1 && k[i].ok(w)) {
            azconf_set(c, k[i].key, w);
            dbg("cfg: %s imported into azmod.conf from %s\n", k[i].key, k[i].file);
        }
        if (f)
            fclose(f);
    }
}
/* Rollback safety: azmod.conf keeps "legacy.cfg-hash": the hash of the azmod.cfg bytes this
 * build last wrote (or migrated from). An azmod.cfg with other bytes was written by another
 * build since: 1.2.2 after a rollback. Its settings are the newest for every key 1.2.2 knows,
 * so those keys are migrated again over the conf; every other key (rows 1.2.2 does not have,
 * "<mod>:<key>", pins of rows without a legacy slot) is kept. */
#define LEGACY_HASH_KEY "legacy.cfg-hash"
static int legacy_cfg_hash(char out[17]) { /* 0 = no azmod.cfg */
    FILE *f = fopen(CONFIG_PATH, "re");
    if (!f)
        return 0;
    unsigned long long h = 1469598103934665603ull;
    int ch;
    while ((ch = fgetc(f)) != EOF) {
        h ^= (unsigned char) ch;
        h *= 1099511628211ull;
    }
    fclose(f);
    snprintf(out, 17, "%016llx", h);
    return 1;
}
static void hash_bytes(const char *b, size_t n, char out[17]) {
    unsigned long long h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char) b[i];
        h *= 1099511628211ull;
    }
    snprintf(out, 17, "%016llx", h);
}
static int is_legacy_key(const char *k, size_t n) {
    for (int i = 0; i < AZ_CFG_WIDTH; i++)
        if (g_slot_key[i] && strlen(g_slot_key[i]) == n && !strncmp(g_slot_key[i], k, n))
            return 1;
    return g_radio.key && strlen(g_radio.key) == n && !strncmp(g_radio.key, k, n);
}
static void relegacy(struct azconf *c, const char *hash) {
    struct azconf *m = azconf_new();
    if (!m)
        return;
    if (azconf_migrate_legacy(m, SETTINGS_DIR, g_slot_key, AZ_CFG_WIDTH, &g_legacy) == 0) {
        char key[32];
        const char *v;
        for (int i = 0; i < AZ_CFG_WIDTH; i++)
            if (g_slot_key[i] && (v = azconf_get(m, g_slot_key[i])))
                azconf_set(c, g_slot_key[i], v);
        if (g_radio.key && (v = azconf_get(m, g_radio.key)))
            azconf_set(c, g_radio.key, v);
        if ((v = azconf_get(m, "padcolor.on")))
            azconf_set(c, "padcolor.on", v);
        if ((v = azconf_get(m, "theme"))) /* what 1.2.2 showed, from its files */
            azconf_set(c, "theme", v);
        if ((v = azconf_get(m, "vinyl.brake")))
            azconf_set(c, "vinyl.brake", v);
        for (int j = 0; j < AZMOD_PADMODE_COUNT; j++) {
            snprintf(key, sizeof key, "padcolor.%d", j);
            if ((v = azconf_get(m, key)))
                azconf_set(c, key, v);
            snprintf(key, sizeof key, "padcolor2.%d", j);
            if ((v = azconf_get(m, key)))
                azconf_set(c, key, v);
        }
        /* pins: 1.2.2's legacy ones, plus every pin it cannot hold */
        char pins[1024];
        size_t np = 0;
        pins[0] = 0;
        for (int pass = 0; pass < 2; pass++) {
            const char *list = azconf_get(pass ? c : m, "pinned");
            for (const char *p = list; p && *p;) {
                const char *e = strchr(p, ',');
                size_t len = e ? (size_t) (e - p) : strlen(p);
                if (len && (pass == 0 || !is_legacy_key(p, len)) && np + len + 2 < sizeof pins) {
                    if (np)
                        pins[np++] = ',';
                    memcpy(pins + np, p, len);
                    np += len;
                    pins[np] = 0;
                }
                p = e ? e + 1 : p + len;
            }
        }
        if (np || azconf_get(c, "pinned"))
            azconf_set(c, "pinned", pins);
        azconf_set(c, LEGACY_HASH_KEY, hash);
        dbg("cfg: azmod.cfg was rewritten by another build (a rollback?): its settings migrated "
            "over azmod.conf, the keys it does not know kept\n");
    }
    azconf_free(m);
}
static void conf_capture(struct azconf *c, const azmod_state *s) {
    char key[64], pins[1024];
    size_t np = 0;
    pins[0] = 0;
    for (int i = 0; i < AZ_VAL_COUNT; i++) {
        if (!azf_conf_key(i, key, sizeof key))
            continue;
        if (az_kind(i) == AZ_ROW_CHOICE)
            azconf_set_int(c, key, az_get(i));
        else
            azconf_set_bool(c, key, az_on(i));
        if (az_pinned(i) && np + strlen(key) + 2 < sizeof pins)
            np += (size_t) snprintf(pins + np, sizeof pins - np, "%s%s", np ? "," : "", key);
    }
    /* a pinned row of a mod that is not loaded now keeps its place in the list */
    for (const char *p = azconf_get(c, "pinned"); p && *p;) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t) (e - p) : strlen(p);
        char k[64];
        if (len < sizeof k && memchr(p, ':', len)) {
            memcpy(k, p, len);
            k[len] = 0;
            int live = 0;
            for (int i = AZI_REG0; i < AZ_VAL_COUNT && !live; i++)
                live = azf_conf_key(i, key, sizeof key) && !strcmp(key, k);
            if (!live && np + len + 2 < sizeof pins)
                np += (size_t) snprintf(pins + np, sizeof pins - np, "%s%s", np ? "," : "", k);
        }
        p = e ? e + 1 : p + len;
    }
    if (np || azconf_get(c, "pinned"))
        azconf_set(c, "pinned", pins);
    azconf_set_bool(c, "padcolor.on", s->pad_colors_on);
    for (int j = 0; j < AZMOD_PADMODE_COUNT; j++) {
        snprintf(key, sizeof key, "padcolor.%d", j);
        if (s->padmode_color[j] < 0) azconf_set(c, key, "native");
        else azconf_set_int(c, key, s->padmode_color[j]);
        snprintf(key, sizeof key, "padcolor2.%d", j);
        if (s->padmode_color2[j] < 0) azconf_set(c, key, "native");
        else azconf_set_int(c, key, s->padmode_color2[j]);
    }
    conf_capture_small(c);
    conf_refresh_files(c);
}
/* s == NULL: the early load (row values only, nothing written) */
static void save_config(const azmod_state *s);
static void load_settings(azmod_state *s) {
    if (!g_settings_early)
        az_defaults(); /* the first load starts from the table's defaults */
    azconf_log = azconf_dbg;
    if (!g_legacy.radio)
        legacy_map();
    struct azconf *c = azconf_new();
    if (!c)
        return;
    int r = azconf_load(c, CONF_PATH), migrated = 0, imported = 0;
    if (settings_import_pending()) { /* the stick's file wins over the deck copy */
        struct azconf *im = azconf_new();
        if (im && azconf_load(im, IMPORT_PATH) == 0) {
            azconf_free(c);
            c = im;
            r = 0;
            imported = 1;
        } else {
            azconf_free(im);
            if (s) {
                char bad[512];
                snprintf(bad, sizeof bad, "%s.bad", IMPORT_PATH);
                rename(IMPORT_PATH, bad);
                dbg("cfg: the stick's azmods.cfg does not load: refused, the deck copy is kept\n");
            }
        }
    }
    char cur[17];
    int have_cfg = legacy_cfg_hash(cur);
    if (r == 0 && have_cfg && !imported) {
        const char *was = azconf_get(c, LEGACY_HASH_KEY);
        if (was && strcmp(was, cur))
            relegacy(c, cur); /* saved below (the full load) */
    }
    if (r == 0 && !imported)
        conf_import_small(c); /* once: a conf from before theme / vinyl.brake moved in */
    if (r != 0) {
        /* -1 absent, -2 garbage (re-migrates from an azmod.cfg that the dual write kept
         * current), -3 a newer build's file: read its dual-written azmod.cfg, keep the conf */
        migrated = azconf_migrate_legacy(c, SETTINGS_DIR, g_slot_key, AZ_CFG_WIDTH, &g_legacy) == 0;
        if (!migrated && r == -3)
            dbg("cfg: azmod.conf is a newer version and there is no azmod.cfg: defaults\n");
    }
    conf_apply(c, s);
    if (!s) {
        azconf_free(c);
        return;
    }
    azconf_free(g_conf);
    g_conf = c;
    g_conf_readonly = r == -3;
    if (imported) {
        /* registered rows (community mods' settings) loaded before this: their keys too */
        char key[64];
        for (int i = AZI_REG0; i < AZ_VAL_COUNT; i++)
            if (azf_conf_key(i, key, sizeof key) && azconf_get(g_conf, key))
                az_set(i, conf_value(g_conf, i, key));
        conf_refresh_files(g_conf);
        save_config(s); /* azmod.cfg (dual write) and its hash, from the stick's settings */
        if (azconf_save(g_conf, CONF_PATH) == 0) {
            unlink(IMPORT_PATH);
            dbg("cfg: settings imported from the stick's azmods.cfg\n");
        } else {
            char bad[512];
            snprintf(bad, sizeof bad, "%s.bad", IMPORT_PATH);
            rename(IMPORT_PATH, bad);
            dbg("cfg: the stick's azmods.cfg could not be written to the deck copy: kept as .bad\n");
        }
    }
    if (migrated && !g_conf_readonly) {
        dbg("cfg: azmod.conf migrated from azmod.cfg (the old files are left as they are)\n");
        conf_capture(g_conf, s); /* the rows' keys as this build writes them */
        if (have_cfg)
            azconf_set(g_conf, LEGACY_HASH_KEY, cur); /* migrated from exactly these bytes */
    }
    if (!g_conf_readonly && !imported) {
        conf_refresh_files(g_conf); /* the small files' copies, never stale */
        /* a migration, a rollback's re-migration, a healed or refreshed conf */
        if (azconf_dirty(g_conf) && azconf_save(g_conf, CONF_PATH) != 0)
            dbg("cfg: azmod.conf not written (no settings partition?)\n");
    }
    /* 60 FPS on at boot arms the frame-clock halving until the next restart (paints
     * 32 -> 64/s, main thread at 71% of one core). The tmpfs flag dies on
     * reboot, so a boot with the toggle off (or a corrupt cfg) never arms it,
     * and turning the toggle off disarms at once because starttimer_hook
     * checks g_ui60_on on every fire. */
    if (ui60_clock_wanted(s)) {
        FILE *f = fopen("/tmp/azmod-disp60", "we"); /* state, not a log: never gated */
        if (f)
            fclose(f);
        __atomic_store_n(&g_disp60_armed, 1, __ATOMIC_RELAXED);
    }
}
/* azmod_init, before any hook installs: the row values the start-up hooks read (Playlist
 * Editing, 60 FPS UI), from the same files by the same rules, so they agree with the menu. */
static void load_settings_early(void) {
    load_settings(NULL);
    g_settings_early = 1;
}
static void load_config(azmod_state *s) {
    load_settings(s);
}
/* A registered row's saved value and pin ("<mod>:<key>"), when it registers (after the load).
 * The SDK's register_row (sdk_rows.c) wraps it. Kept keys of removed mods stay in the file
 * untouched (azconf keeps unknown keys). */
static int register_row_internal(const char *mod_id, const az_feature_v2 *f) {
    int item = azf_register(mod_id, f);
    char key[64];
    if (item >= 0 && g_conf && azf_conf_key(item, key, sizeof key)) {
        if (azconf_get(g_conf, key))
            az_set(item, conf_value(g_conf, item, key));
        if (conf_pinned(g_conf, key) && !az_pinned(item))
            az_pin_toggle(item);
    }
    return item;
}
/* Written whole (settings_write / azconf_save, durable): a power cut mid-save leaves the
 * previous file, never a truncated one. Best-effort: with no settings partition mounted
 * yet, nothing is written. azmod.cfg is 1.2.2's format byte for byte (line 1 by legacy slot,
 * line 2 pad colors, line 3 the pin mask by slot). */
static void save_config(const azmod_state *s) {
    char buf[64 + AZ_CFG_WIDTH + 24 * AZMOD_PADMODE_COUNT];
    size_t n = 0;
    for (int i = 0; i < AZ_CFG_WIDTH; i++) /* line 1: 1.2.2's positional line (dual write) */
        buf[n++] = (char) az_cfg_char(i);
    buf[n++] = '\n';
    for (int j = 0; j < AZMOD_PADMODE_COUNT; j++) /* line 2: pad colors */
        n += (size_t) snprintf(buf + n, sizeof buf - n, "%d ", s->padmode_color[j]);
    n += (size_t) snprintf(buf + n, sizeof buf - n, "%d", s->pad_colors_on ? 1 : 0); /* master */
    for (int j = 0; j < AZMOD_PADMODE_COUNT; j++) /* ...then the secondary pages */
        n += (size_t) snprintf(buf + n, sizeof buf - n, " %d", s->padmode_color2[j]);
    n += (size_t) snprintf(buf + n, sizeof buf - n, "\n%llu\n",
                           az_pins_legacy()); /* line 3: pin mask (bit = slot) */
    /* azmod.cfg first, then azmod.conf with that file's hash: a power cut between the two
     * leaves an azmod.cfg newer than the hash, which the next start takes as a rollback and
     * re-migrates from the newest settings, so nothing is lost */
    int cfg_ok = n < sizeof buf && settings_write(CONFIG_PATH, buf, n, 1) == 0;
    if (g_conf && !g_conf_readonly) {
        conf_capture(g_conf, s);
        if (cfg_ok) {
            char h[17];
            hash_bytes(buf, n, h);
            azconf_set(g_conf, LEGACY_HASH_KEY, h);
        }
        if (azconf_dirty(g_conf))
            azconf_save(g_conf, CONF_PATH);
    }
    az_fire_changes(); /* registered rows' on_change: after the save */
}
#include "updates.c"
#include "savelogs.c"
static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#include "stems_display.c"
#include "inline_hooks.c"
#include "source_list_entry.c"
/* juce::Component's destructor (the complete-object destructor, slot 0 of the vtable whose
 * type_info names juce::Component). Every component's destructor chain ends here, on the
 * message thread, so it is where a cached component pointer stops being valid: clear the wref
 * slots that hold it before the original returns the memory (ep_mem.h). */
#define JUCE_COMPONENT_DTOR EP_2491ed0
static void (*g_orig_compdtor)(void *);
static _Atomic unsigned g_compdtor_n, g_compdtor_offmsg;
static void comp_dtor_hook(void *self) {
    wref_on_destroy((uintptr_t) self);
    atomic_fetch_add_explicit(&g_compdtor_n, 1, memory_order_relaxed);
    if (!on_msg_thread())
        atomic_fetch_add_explicit(&g_compdtor_offmsg, 1, memory_order_relaxed); /* telemetry */
    g_orig_compdtor(self);
}
/* Constructor phase, first of all: before any component a wref slot could hold exists. */
static int install_compdtor_hook(void) {
    return azm_install_hook(JUCE_COMPONENT_DTOR, (void *) comp_dtor_hook, (void **) &g_orig_compdtor);
}
/* EP147's image bounds for img_q: the main program's PT_LOAD segments. */
static int ep_img_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void) size;
    (void) data;
    if (info->dlpi_name && info->dlpi_name[0])
        return 0; /* the main program comes first, with an empty name */
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD)
            continue;
        uintptr_t a = (uintptr_t) info->dlpi_addr + ph->p_vaddr;
        if (a < lo)
            lo = a;
        if (a + ph->p_memsz > hi)
            hi = a + ph->p_memsz;
    }
    if (lo < hi) {
        g_ep_img_lo = lo;
        g_ep_img_hi = hi;
    }
    return 1;
}
static void ep_img_init(void) {
    dl_iterate_phdr(ep_img_cb, NULL);
}
#include "waveform.c"
static void drum_tile_repaint_if_needed(void);    /* defined with the tile code below */
static void sm_tick(void);                        /* Browse stems mark, defined below */
static void drum_caption_repaint_if_needed(void); /* defined with the caption swap below */
static void fxroll_apply(void);                   /* defined below; Beat FX panel takeover */
static void fxbars_tick(void);                    /* defined below; drum roll VOLUME/PITCH bars */
static void install_vinyl_slider(void);           /* vinyl_slider.c */
static void install_qrlogin_guard(void);          /* defined with the crash reporter */
static void
stems_avail_redispatch(void);          /* defined below; needs g_stem_container + waveswitch_hook */
static void stems4_tick(void);         /* 4-DECK source-cell stem strips; defined with the fx art */
static void banner_harness_tick(void); /* banner.c; test harness, opt-in azmod-banner.on */
static void rowcache_settle_tick(void); /* list row memo: one refresh once scrolling stops */
static void pl_tb_tick(void); /* SELECT PLAYLIST banner + DONE, defined with the add target */
static void ov_refresh_if_needed(void);  /* deck-tile preview, defined below */
static void pm_paint(long self, long g); /* Phase Meter, defined with its hooks */
static void pm_tick(void);
static void pl_tick(void);
static void wave_repaint_if_needed(void); /* main waveform repaint, defined below */
static void stem_selfpoke(void);          /* tile self-poke, defined below */
static void drum_tiles_enforce(void);

/* JUCE Component::setVisible, the same function wave_setvis() calls through
 * the vtable (slot 12 / +0x60). Hooked globally; the cost for every other
 * component is a pointer compare. */
#define JUCE_SETVISIBLE EP_249daf0
#define SEARCHVIEW_VT \
    EP_29daee8 /* gui::browse::SearchView primary vtable (RTTI) */
/* gui::browse::BrowseView primary vtable (RTTI "N3gui6browse10BrowseViewE").
 * Its slot 12 is the shared juce::Component::setVisible (== JUCE_SETVISIBLE),
 * so it rides this hook without a vtable patch. setVisible(true) fires reliably
 * and only on arrival at Browse, from both SOURCE and WAVEFORM, which makes it
 * the instant signal for g_browseview_shown: the overlay drains it once per
 * tick to clear source_visible/waveform_visible within one tick, instead of
 * waiting out the slow detect_source()/detect_waveform() polls (~400 ms). */
#define BROWSEVIEW_VT EP_29d67a0
#define PLAYLISTVIEW_VT \
    EP_29da0e8 /* gui::browse::PlaylistView: the PLAYLIST tab's own screen (RTTI) */
/* NEW PLAYLIST naming in BROWSE: the live SearchView / BrowseView (setvis_hook) */
static void nm_view_changed(int search_shown); /* defined with NEW PLAYLIST */
static void (*g_orig_wavector)(long, long *, unsigned);
/* Set briefly to make modeget report the true pad mode, so a dispatch runs the
 * non-stems case; flipping back and re-dispatching rebuilds the stems children,
 * which is the only way they re-read the art table (the decoded image is cached
 * at construction). */
static volatile int g_force_truemode = 0;
/* Pad-mode enum, read from the dispatcher's jump table. WAVE_SWITCH calls the
 * mode getter, `cmp w0,#0x14`, then indexes a 21-byte table at 0x2d8f818 whose
 * entries are (target-0x1df6128)/4; each target loads one child widget:
 *
 *     1,2,13,17 -> +0xf0  PadCueWidget (HOT CUE)
 *     3,4       -> +0xe0  PadBeatLoopWidget
 *     5,6       -> +0xe8  PadBeatJumpWidget
 *     7         -> +0x100 PadSlipLoopWidget      <- SLIP LOOP
 *     8         -> +0xf8  PadKeyShiftWidget      <- KEY SHIFT
 *     11,18     -> +0x110 PadStemWidget
 *     rest      -> +0x108 PadBlankWidget
 *
 * dg_slip_enum can still be overridden from a file. */
#define DG_SLIP_ENUM_DEFAULT 7
#define DG_KEYSHIFT_ENUM 8
static int dg_slip_enum = DG_SLIP_ENUM_DEFAULT;
static _Atomic int g_true_mode; /* modeget_hook publishes the un-hijacked mode */
/* EP147's own pad mode per deck, published by modeget_hook. Stored as m+1 so
 * 0 means "not seen yet" (0 is a real mode, PadBlankWidget). g_true_mode alone
 * cannot answer a per-deck question: it is last-writer-wins across every deck
 * and the pad-title field. */
static _Atomic int g_true_mode_deck[AZMOD_STEM_DECKS];
static _Atomic long
    g_stem_container[AZMOD_STEM_DECKS]; /* pad container per deck: re-dispatch target */
static _Atomic long g_stem_model;       /* tile 0's ImagesComponent = [widget+0xf0] */
/* Deck identity for the pad tiles. The refresh visits every deck's tiles with
 * the same slot numbers, so per-slot state must also be keyed by deck.
 *
 * wavector_hook's third argument is not a deck index: it runs 0..7, the pad
 * slot (PadStatusWidget, vptr 0x02d8f880, stores it at +0xC8). Never build
 * per-deck logic on that argument or on +0xC8.
 *
 * The per-deck identity that does hold is the deckstate pointer
 * *(long*)(self+0xd0): constant across each group of 8 widgets and distinct
 * between groups. There can be more than one live set (eight deckstates in two
 * clusters of four, 0x780 apart: 4 decks x 2 construction passes), so a
 * ds->deck map cannot be first-seen-wins. The ds is copied from ctx+0x10b0, so
 * the deck number comes from the ctx argument. A track's length or a waveform
 * provider's point count is no substitute: two decks can hold the same track. */
static int deck_for_perdeck_field(long p, long off);
static int perdeck_table_ready(void);
/* Diagnostics for the ds->deck census. Both budgets re-arm in wavector_hook
 * (rebuilt tiles are new state worth sampling); a fixed budget would be spent
 * during construction at boot and never sample a deck loaded afterwards. */
static void ds_anchor_scan(long tds);
static _Atomic int g_cap_budget = 64;
static void (*g_orig_rtick)(long);
/* JUCE repaint-timer tick: the message thread, ~32/s, independent of the SOURCE
 * list. Pending focus-change repaints are drained here rather than in
 * getNumRows (src_rows_hook), because EP147 stops calling getNumRows the
 * instant a knob click lands: its own activate() bounds-checks our synthetic
 * row away (getRow() -> NULL), so it has no reason to repaint. Costs nothing
 * when idle; srcrow_repaint_if_needed() returns at once unless a focus change
 * set the flag. */
static void rtick_hook(long self) {
    /* 60 FPS list scrolling: a repeating JUCE timer re-counts its period on every fire
     * without calling startTimer, so refresh the repaint timer's deadline here. */
    if ((self & 7) == 0 && *(uintptr_t *) self == RM_VT && *(int *) (self + 0x10) > 0)
        __atomic_store_n(&g_rm_due_us, scroll_now_us() + (long) *(int *) (self + 0x10) * 1000,
                         __ATOMIC_RELAXED);
    /* Structural work belongs here, not in a paint hook: this is the repaint
     * timer callback, on the message thread but outside any paint traversal. */
    stems_avail_redispatch(); /* eject / no-stem track takes the grid back */
    stems4_tick();            /* 4-DECK: each deck's source cell shows its own stem strip */
    jogstems_tick();          /* jog display picture style: pictures, timing log */
    sm_tick();               /* opt-in: Browse stems mark, repaint once statuses land */
    ov_refresh_if_needed();   /* stem toggle -> every deck preview (deck view and BROWSE strips) */
    pm_tick();                /* Phase Meter: repaint its band so it moves every frame */
    pl_tick(); /* PHASE layout (2/4 DECK tapped again): table + panels, re-asserted */
    fxroll_apply();
    fxbars_tick();     /* drum roll VOLUME/PITCH bars on the Beat FX panel */
    __atomic_fetch_add(&g_rtick_n, 1, __ATOMIC_RELAXED);
    srcrow_repaint_if_needed(g_srcrow_model); /* NULL before the first getNumRows: a no-op */
    drum_tile_repaint_if_needed();
    drum_caption_repaint_if_needed();
    banner_harness_tick();
    pl_tb_tick(); /* SELECT PLAYLIST banner + DONE */
    rowcache_settle_tick();
    g_orig_rtick(self);
}
static void (*g_orig_setvis)(long, int);
static _Atomic int g_visdbg = 0; /* /home/root/settings/azmod-visdbg */
static _Atomic int g_browseview_shown = 0;
static _Atomic unsigned long g_sv_obj, g_bv_obj;
static _Atomic int g_bv_vis;
static void setvis_hook(long self, int on) {
    /* SearchView visible == on the search screen; unlike the byte-32
     * screen-button bits this also catches touch entry. */
    if (self > 0x10000 && *(unsigned long *) self == SEARCHVIEW_VT)
        __atomic_store_n(&g_search_on, on ? 1 : 0, __ATOMIC_RELAXED);
    if (self > 0x10000 && *(unsigned long *) self == SEARCHVIEW_VT) {
        __atomic_store_n(&g_sv_obj, (unsigned long) self, __ATOMIC_RELEASE);
        if (on)
            nm_view_changed(1); /* Search opening: its keyboard must be home first */
    }
    if (self > 0x10000 && *(unsigned long *) self == BROWSEVIEW_VT) {
        __atomic_store_n(&g_bv_obj, (unsigned long) self, __ATOMIC_RELEASE);
        __atomic_store_n(&g_bv_vis, on ? 1 : 0, __ATOMIC_RELEASE);
        if (!on)
            nm_view_changed(0); /* leaving Browse: naming ends */
    }
    if (self > 0x10000 && on && *(unsigned long *) self == BROWSEVIEW_VT)
        __atomic_store_n(&g_browseview_shown, 1, __ATOMIC_RELEASE);
    if (self == __atomic_load_n(&g_waveview, __ATOMIC_RELAXED))
        __atomic_store_n(&g_wave_vis, on ? 1 : 0, __ATOMIC_RELAXED);
    /* Opt-in census of large components' visibility changes (EP147 never calls
     * setVisible on the WaveformView itself when switching screens). Every
     * setVisible is ~96,000 calls in a few seconds, so only view-container-sized
     * components are reported, from their bounds (x@+0x20 y@+0x24 w@+0x28
     * h@+0x2c). */
    if (__atomic_load_n(&g_visdbg, __ATOMIC_RELAXED) && self > 0x10000) {
        /* w and h share one 8-aligned qword at self+0x28 (w low, h high);
         * self+0x2c alone would fail own_q's alignment check. */
        uintptr_t wh = 0;
        if (own_q((uintptr_t) self + 0x28, &wh)) { /* setVisible's own `this` */
            int W = (int) (uint32_t) wh, H = (int) (uint32_t) (wh >> 32);
            if (W >= 400 || H >= 300)
                dbg("SETVIS-BIG self=%p vt=%p on=%d %dx%d\n", (void *) self, *(void **) self, on, W,
                    H);
        }
    }
    g_orig_setvis(self, on);
}
#include "beat_fx_panel.c"
#include "beat_jump.c"
#include "vinyl_brake.c"
#include "crash_guards.c"
#include "track_list_rows.c"
#include "folder_copy.h" /* the FOLDER-view copy plan (playlist_editor.c reads its names) */
#include "playlist_editor.c"
#include "library_db.c"
#include "library_edits.c"
#include "remote_stems.c"
#include "libfd.h" /* the library fds azmod's own workers hold, by owner */
#include "stem_marks.c"
#include "folder_preview.c" /* a selected folder's preview pane fills (2.00 never publishes its page) */

/* ---- 5. the message thread: JUCE's own timer dispatch ---------------------
 * juce::Timer::TimerThread::CallTimersMessage::messageCallback (vptr 0x3141a38,
 * slot +0x10 = 0x239dc40) runs on the message thread every time a timer is due,
 * and EP147 polls its own input from a timer, so this ticks constantly. It is
 * the one place azmod may touch juce components (updateContent) and EP147's GUI
 * objects. The fast path is atomics only: no syscalls, no logging. */
#define JUCE_CALLTIMERS_SLOT (EP_3141a38 + 0x10)
static void (*g_orig_calltimers)(void *);
static void drag_update_content(void);
static struct plsnap g_grab;
static void drag_cancel(void);
static int az_banner_strip(const char *text, int dur_ms); /* banner.c */
static int az_banner_box(const char *l1, const char *l2, int dur_ms);
#define AZ_BANNER_HOLD INT_MIN /* banner.c: strip stays until replaced or cleared */
enum {
    BN_NONE,
    BN_OURS,
    BN_OURS_HELD,
    BN_STOCK
};
static int az_banner_strip_state(void);
static int az_banner_strip_clear(void);
#include "playlist_actions.c"
#include "folder_copy.c" /* COPY / PASTE in the Browse FOLDER view, to the other USB */
/* The clipboard (Copy, Paste and Move). MENU > COPY or MOVE on a row puts it
 * here; MENU > PASTE on a highlighted row puts it there: into a folder; a track
 * into a playlist; a playlist/folder right below a playlist row, in that row's
 * list (which is also how anything reaches the top level). MOVE stays on one
 * stick; COPY crosses sticks (the library spool). Message thread. */
#define CLIP_NONE 0
/* TAG LIST > MENU > COPY (n TAGGED), then PASTE on a playlist. EP147 keeps the
 * Tag List in a music_library::TrackListCache held in slot 0 of the library
 * handler's list caches ([[handler+16]+80], 0x18 stride, the object's cache at
 * +80). It exists only while the TAG LIST screen has loaded it (any tag change
 * drops it), which is why stock offers CREATE PLAYLIST only from that screen,
 * and why COPY is too. Read exactly as stock 0x1271070 does: count =
 * vt+0x28(cache,0,0); entry = 0x1224270(cache,i,0,0) -> x0 content_id, and x1's
 * low byte set means skip (not this stick's). */
static int
    g_paste_pending; /* a PASTE is waiting for its destination row to resolve (MV_OP_TARGET) */
static int g_paste_remove; /* a track MOVE: the ADD landed, now REMOVE it from where it came from */
static int g_paste_removing; /* that REMOVE is in flight (no selection pin, its own message) */
/* Landing flash: edits are otherwise silent, so the row an edit landed on (a
 * drop, a paste, a SELECT) glows in the accent color and fades over FL_MS.
 * Painted by rowpaint_hook on track rows and hlabel_paint_hook on playlist rows
 * (their labels cover the row background), driven by msg_tick, which repaints
 * only that row while it fades. plist = the row is on the playlist list
 * (HierarchyListWidget) rather than a track list. */
/* Cut ghost: a cut row wears a dashed accent outline until it is pasted (or the
 * clipboard clears). Not a dim: a track row's title is its own transparent
 * component painted after the row, so no fill from these hooks covers its text.
 * A playlist/folder row is found the way the SELECT mark is, by its name between
 * its neighbors (cg_tick); a track row is the cut position in the cut playlist,
 * re-proved by its title in EP147's own row cache at paint. */
static _Atomic unsigned long
    g_cg_tw; /* the track list that last painted a ghost (repainted when it ends) */
static int cg_track_row(uintptr_t tw, int row);
static long long drag_now_ms(void);
static int g_paste_cutmove;  /* a CUT pasted into its own playlist: the MOVE in flight */
static int g_paste_inflight; /* a PASTE's write is armed: on success the clipboard empties (one
                              * copy, one paste; nothing else clears it) */
static int plist_name_at(uintptr_t w, int r, char *out, size_t osz);
static void nm_done(int rc);                /* NEW PLAYLIST: its own message + state */
static void pl_undo_offer(const char *msg); /* defined with the banner */
static int drag_num_rows(void);
static int tracklist_selected_row(void);
/* UNDO lives in the MENU panel (a small button top right); the gray strip only
 * says what happened. */
static void tag_clear_tick(int undo_pressed);
/* Clear-after-paste is deferred so UNDO can give the tags back: the Tag List is
 * cleared only when the paste's UNDO window closes unused, and only if nobody
 * touched the tags since (a tag added meanwhile would be lost). Re-adding tags
 * ourselves would mean building EP147 TrackIDs from copied bytes that own heap
 * memory, so never. */
static _Atomic int g_tag_touched;
static int tag_clear(unsigned media);
static void mp_fd_confirm(uintptr_t ctrl, const char *name, int nlists,
                          int nfolders); /* menu_popup.c */
/* TAG LIST > COPY (n TAGGED): read the Tag List on its stick's library thread.
 * `rows` = the TAG LIST rows on screen; the read must find exactly that many. */
static int g_tag_stems; /* this TAGREAD is for MAKE STEMS, not COPY */
static int rs_request_tagged(unsigned media);
/* Is the track list actually on screen? juce::Component::isShowing(), read
 * rather than called: every ancestor visible, and the top one on the desktop.
 * EP147 does not hide the browse view when it switches to WAVEFORM; it detaches
 * it (BrowseView 0x29d67a0's parent goes null) with every visible flag still
 * set, so the widget, its ListBox and their selection live on behind the
 * waveform, where a knob hold would grab a row nobody can see.
 * Component: parentComponent +0x18, flags +0xc0, bit 0 hasHeavyweightPeer (only
 * the root 0x2968228 has it), bit 1 visible. Chain, list up:
 *   ListBox 2e60988:26 > 2b7f8c0:2 > 2d9a588:2002 > BrowseView 29d67a0:2002
 *   > 29cd220:2022 > root 2968228:2427
 * and on WAVEFORM it ends at BrowseView 29d67a0:2 (no parent, no peer).
 * Anything unreadable = not showing: the gesture then leaves the knob alone. */
static int lb_showing(uintptr_t c);
/* juce::Component::isShowing, read: every level visible up to a top with a peer. Message
 * thread only, and `c` must be a component the caller holds live for this call (a hook
 * argument, or a wref slot): each parent is reached through EP147's own field of a live
 * child, so plain loads. Off the message thread: not showing. */
static int lb_showing(uintptr_t c) {
    if (!on_msg_thread())
        return 0;
    for (int depth = 0; depth < 32; depth++) {
        uintptr_t a[2] = {c + 0xc0, c + 0x18}, v[2] = {0, 0};
        int ok[2] = {0, 0};
        if (c < 0x10000 || (c & 7))
            return 0;
        own_qv(2, a, v, ok);
        if (!ok[0] || !ok[1] || !(v[0] & 2))
            return 0; /* this level is hidden */
        if (!v[1])
            return (v[0] & 1) != 0; /* top: on the desktop, or detached */
        c = v[1];
    }
    return 0;
}
/* Message-thread tick for library editing: the landing flash (fl_tick), drag
 * settle and validity, the selection copies the bus thread reads, and the
 * completion of every queued library operation (copy, paste, move, undo, tag
 * reads). */
static void drag_scroll_to(uintptr_t lb, int row);
static void drag_end_local(void);
static void msg_tick(void) {
    sdk_ui_msg_drain(); /* banners and post_ui calls the community mods queued */
    drag_handoff_drain(); /* a drop or BACK the bus thread recorded: before anything reads the drag */
    fl_tick();
    if (__atomic_load_n(&g_drag_settle, __ATOMIC_ACQUIRE)) {
        int fresh = drag_data_new(), late = drag_now_ms() - g_drag_settle_t0 > 2000;
        if (fresh || late) {
            drag_log("SETTLE %s after %lld ms\n", fresh ? "list caught up" : "timed out",
                     drag_now_ms() - g_drag_settle_t0);
            __atomic_store_n(&g_edit_done_ms, drag_now_ms(), __ATOMIC_RELAXED);
            drag_clear();
            drag_update_content();
        }
    }
    /* The bus thread cannot call into JUCE, so the press decision reads a copy of
     * the ListBox selection. 20 Hz and only while Playlist Editing is on: each
     * read is a few preads, never per frame. -1 when no track list is up. */
    if (__atomic_load_n(&g_reorder_enabled, __ATOMIC_RELAXED)) {
        static long long next_sel;
        long long now = drag_now_ms();
        if (now >= next_sel) {
            next_sel = now + 50;
            int showing = tracklist_showing();
            __atomic_store_n(&g_tl_showing, showing, __ATOMIC_RELAXED);
            __atomic_store_n(&g_sel_row_lb, showing ? tracklist_selected_row_any() : -1,
                             __ATOMIC_RELAXED);
        }
    }
    if (__atomic_load_n(&g_reorder_enabled, __ATOMIC_RELAXED))
        plist_tick(); /* 20 Hz: the playlist list's copies */
    drag_try_grab();
    if (__atomic_load_n(&g_reorder_enabled, __ATOMIC_RELAXED)) {
    }
    ls_tick();      /* library spool: opt-in, every 2 s */
    rs_tick();      /* remote stems: opt-in, one stat a second */
    nm_tick();      /* NEW PLAYLIST naming (Playlist Editing) */
    pst_tick();     /* export.pdb after a stock playlist write: due syncs wake the library */
    /* The list must still be the one that was grabbed: same playlist on the same
     * stick (per EP147's own hierarchy), same row count. Anything else (a touch on
     * another playlist, a stick pulled, a re-sort, or the list leaving the screen,
     * where a drop would move a row nobody can see) ends the drag. Nothing has
     * been written yet, so that is always safe. */
    if (__atomic_load_n(&g_drag_on, __ATOMIC_RELAXED) &&
        !__atomic_load_n(&g_drag_committing, __ATOMIC_RELAXED) &&
        __atomic_load_n(&g_drag_kind, __ATOMIC_RELAXED) == DRAG_KIND_PLIST) {
        struct plsnap sn = plsnap_get();
        uintptr_t w = __atomic_load_n(&g_drag_widget, __ATOMIC_RELAXED), lb = 0;
        if (!plist_widget_ok(w, &lb) || !lb_showing(lb)) {
            drag_log("DRAG dropped: the playlist list left the screen\n");
            drag_end_local();
        } else if (sn.sel_pl != g_grab.sel_pl || sn.any_media != g_grab.any_media ||
                   plist_rows(w) != __atomic_load_n(&g_drag_rows, __ATOMIC_RELAXED) ||
                   !plist_refresh_names(w)) {
            drag_log("DRAG dropped: the playlist list changed (pl %u->%u media %04x->%04x rows "
                     "%d->%d)\n",
                     g_grab.sel_pl, sn.sel_pl, g_grab.any_media, sn.any_media,
                     __atomic_load_n(&g_drag_rows, __ATOMIC_RELAXED), plist_rows(w));
            drag_end_local();
        }
    } else if (__atomic_load_n(&g_drag_on, __ATOMIC_RELAXED) &&
               !__atomic_load_n(&g_drag_committing, __ATOMIC_RELAXED)) {
        struct plsnap sn = plsnap_get();
        int rows = drag_num_rows();
        if (!tracklist_showing()) {
            drag_log("DRAG dropped: the list left the screen\n");
            drag_end_local();
        } else if (sn.playlist_id != g_grab.playlist_id || sn.media != g_grab.media ||
                   rows != __atomic_load_n(&g_drag_rows, __ATOMIC_RELAXED)) {
            drag_log("DRAG dropped: the list changed (pl %u->%u media %04x->%04x rows %d->%d)\n",
                     g_grab.playlist_id, sn.playlist_id, g_grab.media, sn.media,
                     __atomic_load_n(&g_drag_rows, __ATOMIC_RELAXED), rows);
            drag_end_local();
        }
    }
    if (__atomic_exchange_n(&g_drag_into_note, 0, __ATOMIC_ACQ_REL) &&
        __atomic_load_n(&g_drag_on, __ATOMIC_RELAXED) &&
        !__atomic_load_n(&g_drag_committing, __ATOMIC_RELAXED) &&
        !dh_closed(&g_dh)) { /* not once a drop or BACK is recorded */
        int f = __atomic_load_n(&g_drag_into, __ATOMIC_RELAXED);
        static int shown = -2;
        if (f != shown) {
            shown = f;
            if (f >= 0 && f < g_plo_n) {
                char b[160];
                snprintf(b, sizeof b, "Press to drop into %s", pl_name_short(g_plo_names[f]));
                az_banner_strip(b, PL_MSG_HOLD);
            } else
                az_banner_strip("Turn to move \xc2\xb7 Press to drop \xc2\xb7 BACK to cancel",
                                PL_MSG_HOLD);
        }
    }
    if (__atomic_exchange_n(&g_drag_want_update, 0, __ATOMIC_ACQ_REL))
        drag_update_content();
    if (__atomic_exchange_n(&g_mv_kick, 0, __ATOMIC_ACQ_REL) && !plrenew_kick())
        drag_log("COMMIT: no renewal handler to wake the library thread\n");
    if (__atomic_load_n(&g_cp.state, __ATOMIC_ACQUIRE) == CP_COPYING &&
        g_cp.ntrk > 1) { /* progress */
        static int shown = -1;
        static long long at;
        int k = __atomic_load_n(&g_cp.done_n, __ATOMIC_ACQUIRE);
        if (k != shown && drag_now_ms() - at > 800) {
            shown = k;
            at = drag_now_ms();
            pl_edit_notify(MV_OP_CPREAD, 2);
        }
    }
    fc_tick(); /* a FOLDER-view copy: progress and result */
    if (__atomic_load_n(&g_cp.state, __ATOMIC_ACQUIRE) ==
        CP_DONE) { /* a cross-stick copy finished */
        int rc = g_cp.rc;
        __atomic_store_n(&g_cp.state, CP_IDLE, __ATOMIC_RELEASE);
        drag_log("COPY done rc=%d\n", rc);
        pl_edit_notify(MV_OP_CPREAD, rc);
        if (g_paste_inflight) {
            if (rc == 0) {
                memset(&g_clip, 0, sizeof g_clip);
                fl_start(g_fl_paste.w, g_fl_paste.row, g_fl_paste.plist);
            }
            g_paste_inflight = 0;
        }
        if (rc == 0 && g_cp.kind == CPK_TAGGED) { /* no UNDO across sticks: clear the Tag List
                                                     now */
            g_tagclr.media = g_clip.media;
            g_tagclr.at = 0;
            g_tagclr.on = 1;
            __atomic_store_n(&g_tag_touched, 0, __ATOMIC_RELEASE);
            memset(&g_clip, 0, sizeof g_clip);
        }
    }
    int st = __atomic_load_n(&g_mv_state, __ATOMIC_ACQUIRE);
    if (st == MV_ARMED && drag_now_ms() > g_mv.deadline) {
        int want = MV_ARMED, op = g_mv.op;
        if (__atomic_compare_exchange_n(&g_mv_state, &want, MV_IDLE, 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            drag_log("COMMIT expired: the library thread never picked it up. NOTHING WRITTEN\n");
            if (op == MV_OP_MKREAD)
                return; /* a background read: nothing to tell, nothing to unpin */
            if (op == MV_OP_FDCOUNT) {
                pl_edit_notify(MV_OP_FDEL, -100);
                return;
            }
            pl_edit_notify(op, -100);
            drag_pin(__atomic_load_n(&g_drag_origin, __ATOMIC_RELAXED));
            if (g_fl.w == __atomic_load_n(&g_drag_widget, __ATOMIC_RELAXED))
                __atomic_store_n(&g_fl.w, 0, __ATOMIC_RELEASE);
            drag_clear();
            drag_update_content();
        }
    } else if (st == MV_DONE) {
        __atomic_store_n(&g_edit_done_ms, drag_now_ms(), __ATOMIC_RELAXED);
        /* Success or refusal, drop the preview: on success EP147 has already
         * re-queried and draws the new order itself; on refusal the real order
         * is the old one. Either way the screen shows the database. */
        int op = g_mv.op, rc = g_mv.rc; /* read before IDLE frees the slot */
        int paste = op == MV_OP_TARGET && g_paste_pending;
        if (paste)
            g_paste_pending = 0;
        else if (op == MV_OP_TARGET && rc == 0) {
            pl_target_store(g_mv.media, g_mv.pl, g_mv.name, g_mv.mount, (unsigned) g_mv.n);
        }
        __atomic_store_n(&g_mv_state, MV_IDLE, __ATOMIC_RELEASE);
        if (paste) { /* the destination is known: route the PASTE */
            int pop = MV_OP_PASTE, prc = rc ? rc : pl_paste_route(&pop);
            drag_log("PASTE route rc=%d op=%d\n", prc, pop);
            if (prc < 0) {
                g_paste_remove = 0;
                pl_edit_notify(rc ? MV_OP_PASTE : pop, rc ? -3 : prc);
            } else
                g_paste_inflight = 1;
            return; /* its own result arrives on a later tick */
        }
        if (g_paste_removing && op == MV_OP_REMOVE) { /* the second half of a track MOVE */
            g_paste_removing = 0;
            g_paste_inflight = 0;
            if (rc == 0) {
                memset(&g_clip, 0, sizeof g_clip);
                fl_start(g_fl_paste.w, g_fl_paste.row, g_fl_paste.plist);
                if (__atomic_load_n(&g_jr.ready, __ATOMIC_ACQUIRE))
                    pl_undo_offer("Moved");
                else
                    az_banner_strip("Moved", PL_MSG_BRIEF);
            } else
                az_banner_strip("Added \xc2\xb7 couldn't remove it from the old playlist",
                                PL_MSG_READ);
            drag_log("PASTE move: remove rc=%d\n", rc);
            return;
        }
        if (op == MV_OP_RSREAD && rc == 0)
            rc = rs_enqueue(); /* the row is read: queue it on its stick */
        if (op == MV_OP_CPREAD &&
            rc == 0) { /* the row is read: copy on a worker, off this thread */
            pthread_t th;
            __atomic_store_n(&g_cp.state, CP_COPYING, __ATOMIC_RELEASE);
            if (az_thread_create(&th, NULL, cp_worker, NULL) == 0) {
                pthread_detach(th);
                rc = 1;
            } else {
                __atomic_store_n(&g_cp.state, CP_IDLE, __ATOMIC_RELEASE);
                rc = -59;
            }
        }
        if (op == MV_OP_FDCOUNT) { /* the count is in: ask, in EP147's own box */
            if (rc == 0)
                mp_fd_confirm(g_fd.ctrl, g_fd.name, g_fd.nlists, g_fd.nfolders);
            else
                pl_edit_notify(MV_OP_FDEL, rc);
            return;
        }
        if (op == MV_OP_FDEL && rc == 0) { /* gone: nothing may still point into it */
            pl_target_forget_deleted(g_fd.media, g_fd.folder, 1);
        }
        if (op == MV_OP_MKREAD) { /* silent: refresh the mark's fingerprint */
            if (rc == 0 && g_mk.on && g_mk.pl == g_mv.pl) {
                g_mk.rows = g_mkr.rows;
                snprintf(g_mk.prev, sizeof g_mk.prev, "%s", g_mkr.prev);
                utf8_copy(g_mk.name, sizeof g_mk.name, g_mkr.name);
                utf8_copy(g_mk.next, sizeof g_mk.next, g_mkr.next);
            }
            return;
        }
        if (op == MV_OP_TAGREAD &&
            g_tag_stems) { /* MAKE STEMS on the Tag List: the tags are read; queue them */
            g_tag_stems = 0;
            int r2 = rc == 0 ? rs_request_tagged(g_mv.media) : rc;
            drag_log("MAKE STEMS Tag List: read rc=%d, %d tracks -> request rc=%d\n", rc, g_tag_n,
                     r2);
            if (r2 < 0)
                pl_edit_notify(MV_OP_RSREAD, r2);
            return;
        }
        if (op == MV_OP_TAGREAD && rc == 0) { /* COPY (n TAGGED): the clipboard holds them now */
            memset(&g_clip, 0, sizeof g_clip);
            g_clip.kind = CLIP_TAGGED;
            g_clip.media = g_mv.media;
            snprintf(g_clip.name, sizeof g_clip.name, "%d tagged tracks", g_tag_n);
        }
        pl_edit_notify(op, rc);
        if (op == MV_OP_ADDN && rc == 0) { /* pasted: clear the Tag List now; UNDO re-tags them */
            g_tagclr.media = g_mv.media;
            g_tagclr.at = 0;
            g_tagclr.on = 1;
            __atomic_store_n(&g_tag_touched, 0, __ATOMIC_RELEASE);
            g_retag.on = 1;
            g_retag.media = g_mv.media;
            g_retag.n = g_tag_n;
            memcpy(g_retag.cids, g_tag_cids, sizeof(unsigned) * (size_t) g_tag_n);
            memset(&g_clip, 0, sizeof g_clip);
        } else if (op == MV_OP_UNDO && rc == 0 &&
                   g_retag.on) { /* the paste was undone: tag its tracks again */
            tag_readd();
        }
        if (op != MV_OP_ADDN && op != MV_OP_UNDO && mv_journaled(op) && rc == 0)
            g_retag.on = 0; /* UNDO now means that edit */
        if (op == MV_OP_PLMOVE && rc == 0 && g_clip.move)
            memset(&g_clip, 0, sizeof g_clip); /* a MOVE happens once */
        if (op == MV_OP_MOVE && g_paste_cutmove) {
            g_paste_cutmove = 0; /* a CUT pasted into its own playlist */
            if (rc == 0) {
                memset(&g_clip, 0, sizeof g_clip);
                fl_start(g_fl_paste.w, (int) g_mv.to - 1, 0);
            }
        }
        if (g_paste_inflight && op != MV_OP_TARGET && op != MV_OP_MKREAD) {
            if (op == MV_OP_CPREAD) {
                if (rc < 0)
                    g_paste_inflight = 0;
            }                           /* success: at CP_DONE */
            else if (!g_paste_remove) { /* a CUT's ADD continues as its REMOVE */
                if (rc == 0) {
                    memset(&g_clip, 0, sizeof g_clip);
                    drag_log("CLIP cleared: pasted\n");
                    fl_start(g_fl_paste.w, g_fl_paste.row, g_fl_paste.plist);
                }
                g_paste_inflight = 0;
            }
        }
        if (op == MV_OP_ADD &&
            g_paste_remove) { /* track MOVE: now take it out of where it came from */
            g_paste_remove = 0;
            if (rc == 0 && __atomic_load_n(&g_mv_state, __ATOMIC_ACQUIRE) == MV_IDLE) {
                g_mv.op = MV_OP_REMOVE;
                g_mv.pl = g_clip.src_pl;
                g_mv.cid = g_clip.id;
                g_mv.media = g_clip.media;
                g_mv.from = g_clip.src_seq;
                g_mv.to = 0;
                g_mv.deadline = drag_now_ms() + MV_DEADLINE_MS;
                g_mv.rc = 0;
                g_paste_removing = 1;
                g_jr.keep =
                    1; /* the REMOVE continues the ADD's journal: one UNDO for the whole MOVE */
                __atomic_store_n(&g_mv_state, MV_ARMED, __ATOMIC_RELEASE);
                __atomic_store_n(&g_mv_kick, 1, __ATOMIC_RELEASE);
                drag_log("PASTE move: REMOVE armed pl=%u cid=%u seq %u\n", g_mv.pl, g_mv.cid,
                         g_mv.from);
            }
        }
        if (op == MV_OP_MOVE || op == MV_OP_PLORDER || op == MV_OP_PLINTO) {
            int dragged = __atomic_load_n(&g_drag_on, __ATOMIC_RELAXED) && !g_paste_cutmove;
            if (rc != 0 && dragged && g_fl.w == __atomic_load_n(&g_drag_widget, __ATOMIC_RELAXED))
                __atomic_store_n(&g_fl.w, 0, __ATOMIC_RELEASE); /* it did not land: no glow */
            drag_pin(rc == 0 ? __atomic_load_n(&g_drag_target, __ATOMIC_RELAXED)
                             : __atomic_load_n(&g_drag_origin, __ATOMIC_RELAXED));
            if (rc == 0 && dragged) { /* keep the preview until the list catches up */
                g_drag_settle_t0 = drag_now_ms();
                __atomic_store_n(&g_drag_settle, 1, __ATOMIC_RELEASE);
            } else {
                drag_clear();
                drag_update_content();
            }
        }
        /* A REMOVE has no preview to drop: EP147's own refresh redraws the list,
         * but puts the selection on row 1, so a DJ pruning a long playlist would
         * lose their place. Keep it where the removed track was (its successor
         * now), or on the new last row when the last one went. Uses the
         * database's count before the write, not the list's, which may not have
         * refreshed yet. */
        if (op == MV_OP_REMOVE && rc == 0 && g_mv.n > 1) {
            long long r = (long long) g_mv.from - 1, last = g_mv.n - 2;
            drag_pin((int) (r > last ? last : r));
        }
    }
    int pin = __atomic_load_n(&g_drag_pin_row, __ATOMIC_ACQUIRE);
    if (pin >= 0) {
        /* First EP147's own cursor: it needs no widget, and must never be skipped,
         * because knobturn_hook consumes turns until it has run. */
        if (__atomic_exchange_n(&g_cursor_sync, 0, __ATOMIC_ACQ_REL))
            knob_cursor_to(pin);
        int plk = __atomic_load_n(&g_drag_kind, __ATOMIC_RELAXED) == DRAG_KIND_PLIST;
        unsigned long w = plk ? __atomic_load_n(&g_drag_widget, __ATOMIC_RELAXED)
                              : __atomic_load_n(&g_tracklist_widget, __ATOMIC_RELAXED);
        uintptr_t vt = 0, lb = 0;
        /* msg_tick is the message thread; both widget caches are wref slots
         * (playlist_editor.c), so a non-zero one is a live component */
        if (!w || !own_q(w, &vt) || vt != (plk ? HIER_WIDGET_VT : TRACKLIST_WIDGET_VT) ||
            !own_q(w + 0x48, &lb) || lb < 0x10000) {
            __atomic_store_n(&g_drag_pin_row, -1, __ATOMIC_RELAXED);
        } else {
            /* Hold it for the whole window, not just once: the order in which
             * EP147 applies the unfreeze jump relative to our selectRow is not
             * known, and re-asserting for DRAG_PIN_MS makes it irrelevant. Cost:
             * a knob turn inside that window after a drop is undone. */
            int sel = ((int (*)(void *, int)) LISTBOX_GETSELECTED)((void *) lb, 0);
            if (sel != pin) {
                ((void (*)(void *, int, int, int)) LISTBOX_SELECTROW)((void *) lb, pin, 0, 1);
                drag_log("PIN selection %d -> %d\n", sel, pin);
            }
            if (drag_now_ms() > __atomic_load_n(&g_drag_pin_deadline, __ATOMIC_RELAXED))
                __atomic_store_n(&g_drag_pin_row, -1, __ATOMIC_RELAXED);
        }
    }
}
#include "bluetooth.c"
#include "font_size.c"
#include "streaming_key.c"
#include "key_pane.c"
#include "spin_back.c"
#include "browse_columns.c" /* PREVIEW and the sub column remembered; Column Edit's columns */
#include "clock_native.c" /* the Clock in the place of EP147's status bar readout */
static void bf_tick(void); /* browse_fields.c */
static void calltimers_hook(void *self) {
    msg_tick();
    bt_mods_tick();
    bc_tick();
    cn_tick();
    bf_tick();
    sk_tick();
    mp_anim_tick();
    cm_icons_tick();
    /* installed at run time from any thread: never branch to an original not yet visible */
    void (*o)(void *) = __atomic_load_n(&g_orig_calltimers, __ATOMIC_ACQUIRE);
    if (o)
        o(self);
}
/* The message tick alone: JUCE's callTimers slot -> calltimers_hook (msg_tick,
 * Bluetooth, the MENU popup's slide, the Community tiles' icons). It has its own
 * once-only installer so Community Mods can have the tick without the library
 * hooks. It is patched once, by whoever asks first, and every later caller gets
 * that result; a second patch would read calltimers_hook back as the "original"
 * and chain it to itself. Callable at runtime from any thread: the original is
 * stored before the slot (release), and the slot store is one aligned word. */
static int install_msgtick_hook(const char *who) {
    static _Atomic int state; /* 0 not yet, 1 installing, 2 ok, 3 failed */
    int z = 0;
    if (__atomic_compare_exchange_n(&state, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        void **slot = (void **) JUCE_CALLTIMERS_SLOT;
        azprot_t pt;
        int ok = slot && azprot_unlock((uintptr_t) slot, sizeof *slot, &pt) == 0;
        if (ok) {
            __atomic_store_n((void **) &g_orig_calltimers, *slot, __ATOMIC_RELEASE);
            __atomic_store_n(slot, (void *) calltimers_hook, __ATOMIC_RELEASE);
            if (azprot_relock(&pt) != 0)
                drag_log("msgtick hook: vtable page left writable (mprotect failed)\n");
        }
        __atomic_store_n(&state, ok ? 2 : 3, __ATOMIC_RELEASE);
        drag_log("msgtick hook: installed by %s -> %s\n", who, ok ? "OK" : "FAILED");
        return ok;
    }
    int st;
    while ((st = __atomic_load_n(&state, __ATOMIC_ACQUIRE)) == 1)
        sched_yield();
    drag_log("msgtick hook: %s asked, already %s\n", who, st == 2 ? "installed" : "failed");
    return st == 2;
}
/* The three hooks the library thread and the message tick ride on: the renewal
 * handler's ctor (plrenew_kick's wake-up), MusicLibraryServer's onMessage
 * (library-thread work at a message boundary) and JUCE's timer dispatch
 * (msg_tick). Shared by Playlist Editing and the library spool, so neither
 * depends on the other being on. Installed once, since patching a vtable slot
 * twice would chain the hook to itself. Returns 1 when all took. Every body they
 * run is inert without its own feature's state: the drag and g_mv paths need a
 * grab or an armed request, which only Playlist Editing makes; the spool and the
 * probe check their own flags. */
static int install_library_hooks(void) {
    static int done = 0, ok = 0;
    if (done)
        return ok;
    done = 1;
    int b =
        azm_install_hook(PLRENEW_CTOR, (void *) plrenew_ctor_hook, (void **) &g_orig_plrenew_ctor);
    int c = azm_patch_vtable(MLSERVER_ONMSG_SLOT, (void *) mlserver_onmsg_hook,
                             (void **) &g_orig_mlserver_onmsg);
    int d = install_msgtick_hook("library hooks");
    ok = (b == 1 && c == 1 && d == 1);
    drag_log("library hooks: renewal=%d library=%d msgtick=%d -> %s\n", b, c, d,
             ok ? "OK" : "DECLINED");
    return ok;
}
#include "extras.c"
static void (*g_orig_setgridadj)(void *, int);
/* Constructor phase (install_static_hooks): EP147's code is mapped before any constructor runs
 * and none of it has executed yet, so there is nothing to wait for. */
static void install_wavestems_hook(void) {
    int a = azm_install_hook(WAVE_SWITCH, (void *) waveswitch_hook, (void **) &g_orig_waveswitch);
    azm_install_hook(
        WV_SET_GRIDADJ, (void *) setgridadj_hook,
        (void **) &g_orig_setgridadj); /* hide our pad-row overlays under GRID ADJUST */
    int b = azm_install_hook(WAVE_CTOR, (void *) wavector_hook, (void **) &g_orig_wavector);
    azm_install_hook(S4_CTOR, (void *) s4ctor_hook,
                     (void **) &g_orig_s4ctor); /* 4-DECK source cell: deck from the ctor arg */
    int c = azm_install_hook(WAVE_MODEGET, (void *) modeget_hook, (void **) &g_orig_modeget);
    azm_install_hook(TITLE_UPDATE, (void *) titleupd_hook,
                     (void **) &g_orig_titleupd); /* per-deck title refresh */
    /* requestAnalyze is half of native track identity (TrackID -> path, read by
     * azstem), so it installs unconditionally; behind a diagnostic flag it would
     * silently do nothing on a clean install. */
    {
        static int reqan_installed;
        if (!reqan_installed) {
            reqan_installed = 1;
            int ar = azm_install_hook(EP_c85838, (void *) reqan_hook, (void **) &g_orig_reqan);
            dbg("reqan (identity) hook: %d\n", ar);
        }
    }
    int sr = 0;
    /* stemref_hook is half of the native stem tiles (stem_tile_fix_one runs only
     * inside it; stem_tiles_poke returns without it), so it is unconditional.
     * azmod-stemref.on is not read. */
    sr = azm_install_hook(WAVE_STEMREF, (void *) stemref_hook, (void **) &g_orig_stemref);
    g_orig_stemref_fwd = g_orig_stemref; /* Track-1 diag poke path */
    /* The WAVE_PAINT hook is not the wave recolor path (that rides the wp6
     * waveform point-fn shims); it carries the drum-roll tile enforce
     * (drum_tiles_enforce) and the FPS paint counter (g_paint_n++). So it is
     * installed unconditionally, and azmod-nowave.on does not affect it (that
     * flag still gates the waveform hooks further down). */
    int nowave = AZ_PROBE_ON("/home/root/settings/azmod-nowave.on");
    if (nowave)
        dbg("azmod-nowave.on set: NO-OP (wavepaint_hook carries drum art + FPS paint counter, not "
            "the wave recolor; always installed)\n");
    int d = azm_install_hook(WAVE_PAINT, (void *) wavepaint_hook, (void **) &g_orig_wavepaint);
    /* Beat FX panel hooks. The capture ctors are pure observers (logging gated
     * on /home/root/settings/azmod-fxdiag) and run once per view build. */
    {
        int f1 = azm_install_hook(BEATFX_PARAM_CTOR, (void *) fxparam_ctor_hook,
                                  (void **) &g_orig_fxparam_ctor);
        int f2 = azm_install_hook(BEATFX_TIME_CTOR, (void *) fxtime_ctor_hook,
                                  (void **) &g_orig_fxtime_ctor);
        int f3 = azm_install_hook(BEATFX_LENGTH_CTOR, (void *) fxlength_ctor_hook,
                                  (void **) &g_orig_fxlength_ctor);
        int f4 = azm_install_hook(FXWIDGET_CTOR, (void *) fxwidget_ctor_hook,
                                  (void **) &g_orig_fxwidget_ctor);
        int f5 =
            azm_install_hook(FXBODY_CTOR, (void *) fxbody_ctor_hook, (void **) &g_orig_fxbody_ctor);
        int f6 = azm_install_hook(FX_NAME_UPDATE, (void *) fxname_update_hook,
                                  (void **) &g_orig_fxname_update);
        /* The X-PAD PARAMETER row: owner ctor (capture) + per-type strip
         * writer (the hook that owns the index while drum roll is armed). */
        int f7 =
            azm_install_hook(FX_ROW_CTOR, (void *) fxrow_ctor_hook, (void **) &g_orig_fxrow_ctor);
        int f8 = azm_install_hook(FX_ROW_UPDATE, (void *) fxrow_update_hook,
                                  (void **) &g_orig_fxrow_update);
        int f9 = azm_install_hook(FX_ROW_UPDATE2, (void *) fxrow_update2_hook,
                                  (void **) &g_orig_fxrow_update2);
        /* drum roll VOLUME/PITCH bars: the REVERB bar widget, repurposed */
        int fa =
            azm_install_hook(FXB_LAYOUT, (void *) fxb_layout_hook, (void **) &g_orig_fxb_layout);
        int fb = azm_install_hook(FXB_PAINT, (void *) fxb_paint_hook, (void **) &g_orig_fxb_paint);
        /* drum roll: the X-PAD row shows the finger line for every FX while armed */
        int fz = azm_install_hook(FXR_ZONE, (void *) fxr_zone_hook, (void **) &g_orig_fxr_zone);
        int fh =
            azm_install_hook(FXH_WRITER, (void *) fxh_writer_hook, (void **) &g_orig_fxh_writer);
        azm_install_hook(FXH_CTOR, (void *) fxh_ctor_hook, (void **) &g_orig_fxh_ctor);
        azm_install_hook(FXH_WRITER2, (void *) fxh_writer2_hook, (void **) &g_orig_fxh_writer2);
        dbg("fx header hook %d\n", fh);
        dbg("fxrow zone hook %d\n", fz);
        /* BEAT JUMP 2 First (mod 27): the pad tasks swap page lengths (never the BEAT
         * JUMP buttons), and the jog display shows the swapped lengths; off = untouched. */
        {
            int bp = azm_install_hook(BJ_PADPRESS_RUN, (void *) bj_padpress_hook,
                                      (void **) &g_orig_bj_padpress);
            int br = azm_install_hook(BJ_PADREL_RUN, (void *) bj_padrel_hook,
                                      (void **) &g_orig_bj_padrel);
            int bj = azm_install_hook(BJ_JOGINFO_STORE, (void *) bj_joginfo_hook,
                                      (void **) &g_orig_bj_joginfo);
            dbg("bj2 pad-task hooks press %d release %d jog %d\n", bp, br, bj);
        }
        /* Stepless vinyl brake: the SHORTCUT slider (vinyl_slider.c). Always installed;
         * it passes EP147's value through untouched until the slider has been used. */
        {
            install_vb_dtor(); /* before the setter can record an adjustor */
            int vb = azm_install_hook(VB_SETTER, (void *) vb_set_hook, (void **) &g_orig_vb_set);
            g_vb_hooked = vb == 1;
            install_vinyl_slider();
            dbg("vinyl brake hook %d\n", vb);
        }
        dbg("fxpanel diag hooks: %d %d %d %d %d name=%d rowctor=%d row=%d row2=%d bars=%d,%d\n",
            f1, f2, f3, f4, f5, f6, f7, f8, f9, fa, fb);
    }
    /* ~4 calls for the life of the process (one per deck); not a hot path. */
    int wc = azm_install_hook(WWB_CTOR, (void *) wwbctor_hook, (void **) &g_orig_wwbctor);
    /* Not installed: DetailedWaveformWidget<*>::paint (0x01e071a0) is shared with
     * many other components and is never called for these widgets (the parent
     * drives them through update()), so hooking it only costs preads. */
    int wp2 = 0;
    (void) wavewpaint_hook;
    (void) g_orig_wavewpaint;
    /* FUN_01b93a28 is the only caller of update() (FUN_01b97d98) and holds the
     * WWB itself: FUN_01b97d98(param_1, param_2 + 0x10ff, ..) where param_2+0x10ff
     * is WWB+0x87F8, the RGB widget. Hooking the caller instead of update() keeps
     * the deck lookup to two pointer compares against the ctor's key table, and
     * avoids trampolining update() itself, which crashes EP147. */
    int uc = azm_install_hook(WWB_UPDATE_CALLER, (void *) azm_upd_shim, (void **) &g_orig_wavupd);
    int dd = 0;
    if (access("/home/root/settings/azmod-dedupe4.off", F_OK) != 0)
        dd = azm_install_hook(WAVEVIEW_UPDATE, (void *) wvupd_hook, (void **) &g_orig_wvupd);
    dbg("dedupe4: screen-root hook=%d\n", dd);
    /* makeGridImage is shared: Bright Grid brightens the ticks, Day Mode themes the
     * ruler background. Installed when either needs it; each feature's work inside
     * gridmake_hook is gated on its own flag, so neither depends on the other
     * (g_gridaa_hooked = Bright Grid's own switch, Day Mode reads g_dm_on). */
    int gridaa_off = access("/home/root/settings/azmod-gridaa.off", F_OK) == 0;
    int ga = 0;
    if (!gridaa_off || daymode_wanted())
        ga = azm_install_hook(GRID_MAKE, (void *) gridmake_hook, (void **) &g_orig_gridmake);
    g_gridaa_hooked = ga == 1 && !gridaa_off;
    dbg("gridaa: makeGridImage hook=%d\n", ga);
    dbg("wavestems: wwbctor=%d wpaint=%d updcaller=%d\n", wc, wp2, uc);
    /* Probe only, off by default: hooking update() directly crashes EP147, even
     * as a fully register-preserving asm shim. Kept to re-check the binding the
     * caller hook above relies on; arm it on the emulator only, never in a
     * shipped build. */
    int uh = 0;
    if (AZ_PROBE_ON("/home/root/settings/azmod-updhook.on"))
        uh = azm_install_hook(WAVE_UPDATE, (void *) azm_upd_shim, (void **) &g_orig_wavupd);
    /* ---- waveform kill switch: /home/root/settings/azmod-nowave.on ---------
     * A diagnostic for isolating an EP147 SIGSEGV. A hook on EP147's paint is in
     * every paint stack by construction, so a backtrace alone cannot say whether
     * azmod caused a fault or merely wrapped it. With this flag, the waveform-path
     * hooks below are skipped and EP147 paints its waveforms by itself; if the
     * crash persists, azmod's waveform code is cleared. Costs: no per-stem mini
     * waveforms, no preview/overview re-render, no tile art re-apply. Never ship
     * with it set. */
    int wp = 0;
    if (!nowave) {
        azm_install_hook(SW2D_PAINT, (void *) sw2d_paint_hook,
                         (void **) &g_orig_sw2d_paint); /* per-deck paint -> deck ctx */
        azm_install_hook(WAVE_OVERVIEW, (void *) ov_hook, (void **) &g_orig_ov);
        /* preview/overview -> deck: the owner parks the deck, the tile ctor records
         * (tile + 0x280) -> deck. ~4 calls each per process, never a hot path. */
        az_install_segv(); /* backtrace on EP147 SIGSEGV; observes only */
        azm_install_hook(OV_TILE_OWNER, (void *) tileowner_hook, (void **) &g_orig_tileowner);
        azm_install_hook(OV_TILE_CTOR, (void *) tilector_hook, (void **) &g_orig_tilector);
        /* Stem height scaling: the render hook (1) or the point hooks (0). */
        wp = install_wave_scale();
    } /* end azmod-nowave.on guard */
    int e = azm_install_hook(JUCE_SETVISIBLE, (void *) setvis_hook, (void **) &g_orig_setvis);
    int f = azm_install_hook(JUCE_REPAINT_TIMERCB, (void *) rtick_hook, (void **) &g_orig_rtick);
    g_scroll60_ok = access("/home/root/settings/azmod-scroll60.off", F_OK) != 0;
    int g =
        azm_install_hook(JUCE_STARTTIMER, (void *) starttimer_hook, (void **) &g_orig_starttimer);
    int fl = g_scroll60_ok
                 ? azm_install_hook(FLING_REL, (void *) flrel_hook, (void **) &g_orig_flrel)
                 : -2;
    int ps = g_scroll60_ok
                 ? azm_install_hook(RM_PAINTNOW, (void *) rmpaint_hook, (void **) &g_orig_rmpaint)
                 : -2;
    {
        FILE *sf = azlog_open("/tmp/azmod-scroll60.log", "w");
        if (sf) {
            fprintf(
                sf,
                "scroll60: %s starttimer=%d fling=%d paintstamp=%d rowcache+slowdrag at start=%d\n",
                g_scroll60_ok ? "on" : "KILLED (settings/azmod-scroll60.off)", g, fl, ps,
                ui60ui_at_start());
            fclose(sf);
        }
    }
    int rp = 0; /* the peer-repaint census is not installed (it frame-walked a 131/s path): logged as 0 */
    int dr = azm_install_hook(DISPREFRESH_TIMERCB, (void *) disprefresh_hook,
                              (void **) &g_orig_disprefresh);
    int dt = 0;
    g_feedlock_ok = access("/home/root/settings/azmod-feedlock.off", F_OK) != 0;
    int lp = g_feedlock_ok ? azm_install_hook(LOCALPLAYER_TIMERCB, (void *) lptick_hook,
                                              (void **) &g_orig_lptick)
                           : -2;
    {
        FILE *hf = azlog_open("/tmp/azmod-hooks2", "w");
        if (hf) {
            fprintf(hf, "starttimer=%d peerrepaint=%d disprefresh=%d divpatch=%d feedlock=%d\n", g, rp,
                    dr, dt, lp);
            fclose(hf);
        }
    }
    g_wave_hooked = (d == 1) || (c == 1);
    if (e != 1)
        __atomic_store_n(&g_wave_vis, -1, __ATOMIC_RELAXED); /* stay on the fallback */
    g_setvis_hooked = (e == 1);
    dbg("wavestems: switch=%d ctor=%d modeget=%d paint=%d setvis=%d rtick=%d stemref=%d "
        "update=%d\n",
        a, b, c, d, e, f, sr, uh);
    {
        FILE *hf = azlog_open("/tmp/azmod-hooks", "w");
        if (hf) {
            fprintf(hf, "switch=%d ctor=%d modeget=%d paint=%d setvis=%d rtick=%d wavept6=%d\n", a,
                    b, c, d, e, f, wp);
            fclose(hf);
        }
    }
}
/* pngpatch runs synchronously in the constructor, before EP147's main can build
 * the UI and decode art. Applied from the overlay thread it would race the UI's
 * first decode, and whichever art lost the race would display unpatched. */
static void pngpatch_apply(void) {
    /* Where the art is, in priority order. The release USB loader (az-usbload)
     * mirrors the stick's MODS/azmod-pngpatch into its own p7 cache,
     * /home/root/settings/az-usbload, not into /home/root/settings. The loader's
     * cache comes first: it is the art set that shipped with this .so. Missing
     * it leaves the stem tiles native blue and the DRUM ROLL / SLIP LOOP captions
     * absent. */
    static const char *const PPDIRS[] = {
        "/home/root/settings/az-usbload/azmod-pngpatch", /* release: staged from the stick */
        "/home/root/settings/azmod-pngpatch",            /* hand/overlay deploy, emulator */
        "/tmp/azmod-pngpatch",                           /* dev iteration (wiped on reboot) */
    };
    const char *ppdir = NULL;
    for (unsigned d = 0; d < sizeof PPDIRS / sizeof *PPDIRS; d++)
        if (access(PPDIRS[d], F_OK) == 0) {
            ppdir = PPDIRS[d];
            break;
        }
    if (!ppdir) {
        /* Log it: a silent no-op here would read exactly like "the art is fine". */
        dbg("pngpatch: NO art dir found (looked in %s | %s | %s) -- native art kept\n", PPDIRS[0],
            PPDIRS[1], PPDIRS[2]);
        return;
    }
    /* Addresses are per EP147 build. N.bin is shared; N.addr is its 1.30 address
     * and N.addr.<build> (e.g. N.addr.2.00) its address in that build. All or
     * nothing: a set missing any address for the running build is skipped whole,
     * because another build's addresses overwrite live data and crash-loop EP147.
     * Every write also passes the same guards as usbmod_apply: inside .rodata, at
     * the start of one of this build's images (ep147png.h), with a PNG that fits it. */
    {
        char sfx[16] = "";
        if (g_ep147_build != EPB_V130)
            snprintf(sfx, sizeof sfx, ".%s", ep147_build_name(g_ep147_build));
        uintptr_t lo = 0, hi = 0;
        if (!az_rodata_range(&lo, &hi)) {
            dbg("pngpatch: .rodata not identified -- skipped\n");
            return;
        }
        int nb = 0;
        for (;; nb++) {
            char af[160], bf[160];
            snprintf(bf, sizeof bf, "%s/%d.bin", ppdir, nb);
            if (access(bf, F_OK) != 0)
                break;
            snprintf(af, sizeof af, "%s/%d.addr%s", ppdir, nb, sfx);
            if (access(af, F_OK) != 0) {
                dbg("pngpatch: %s has no %d.addr%s for EP147 %s -- whole set skipped\n", ppdir, nb,
                    sfx, ep147_build_name(g_ep147_build));
                return;
            }
        }
        int applied = 0, refused = 0;
        for (int i = 0; i < nb; i++) {
            char bf[160], af[160];
            snprintf(bf, sizeof bf, "%s/%d.bin", ppdir, i);
            snprintf(af, sizeof af, "%s/%d.addr%s", ppdir, i, sfx);
            FILE *fh = fopen(af, "re");
            unsigned long va = 0;
            if (!fh || fscanf(fh, "%lx", &va) != 1) {
                if (fh)
                    fclose(fh);
                refused++;
                continue;
            }
            fclose(fh);
            static unsigned char buf[65536];
            fh = fopen(bf, "rbe");
            if (!fh) {
                refused++;
                continue;
            }
            size_t n = fread(buf, 1, sizeof buf, fh);
            fclose(fh);
            size_t orig = (va >= lo && va < hi && (uintptr_t) (va + n) <= hi)
                              ? az_art_target_len((uintptr_t) va, buf, n)
                              : 0;
            if (!orig) {
                dbg("pngpatch: blob %d REFUSED at %#lx (n=%zu orig=%zu)\n", i, va, n, orig);
                refused++;
                continue;
            }
            azprot_t pt;
            if (azprot_unlock((uintptr_t) va, n, &pt) != 0) {
                dbg("pngpatch: blob %d REFUSED: %#lx+%zu cannot be made writable\n", i, va, n);
                refused++;
                continue;
            }
            memcpy((void *) (uintptr_t) va, buf, n);
            if (azprot_relock(&pt) != 0)
                dbg("pngpatch: blob %d: page protection not restored\n", i);
            applied++;
        }
        dbg("pngpatch: %d blobs applied, %d refused, from %s (EP147 %s)\n", applied, refused, ppdir,
            ep147_build_name(g_ep147_build));
    }
}

/* ==================================================================== *
 * Browse knob (rotation + click) + physical BACK: interpose open()/read()
 * on /dev/subucom_spi3.0, the mixer bus EP147 reads directly. Runs on
 * EP147's own thread. It never reads g_state: the Mod Menu settings it acts on
 * come from the overlay's published word (bus_cfg.h), and it publishes through the
 * atomics above.
 * ==================================================================== */

static int (*next_open)(const char *, int, ...);
static ssize_t (*next_read)(int, void *, size_t);


/* Owned by the interposer (EP147's own thread calls read() serially on one fd),
 * so no atomics are needed. */
static int g_last_click_bit = 0, g_last_back_bit = 0;

#include "banner.c"       /* az_banner_strip / az_banner_box: text in EP147's own caution UI */
#include "probe_menu.c"   /* opt-in azmod-menuprobe.on: who reads the browse MENU model */
#include "touch_accum.c"  /* opt-in azmod-touchaccum.on: slow touch drags scroll */
#include "menu_popup.c"   /* playlist editing in EP147's own MENU panel */
#include "browse_fields.c" /* Column Edit: a library field on a column */
#include "browse_edit.c"  /* Column Edit: the COLUMN EDIT button and its edit mode */
#include "vinyl_slider.c" /* SHORTCUT: Vinyl Speed Adjust Time as a stepless slider */
#include "wave_zoom.c"    /* Remember Waveform Zoom */
#include "jog_display.c"  /* stem state on the jog display */
#include "onelibrary_key_emulator.c"
#include "community.c"       /* community mod host */
#include "sdk_input.c"       /* frame and touch filters for community mods */
#include "sdk_deck.c"        /* community mod services: deck state and events */
#include "sdk_ui.c"          /* community mod services: banner, confirm, post_ui, threads */
#include "ssh_approve.c"     /* remote access for a stick's SSH key, allowed on the deck */
#include "community_icons.c" /* the Community tiles' icons, decoded by EP147's JUCE on the message thread */
/* LD_PRELOAD is set at the service level, so azmod.so is loaded into every
 * process apl_start.sh starts (apl_start.sh, EP147, edb_streamd, ...). Each
 * constructor would otherwise spawn its own overlay thread and override-redirect
 * window, fighting for the same screen position: the button flickers as windows
 * raise themselves, and a tap can land on a phantom instance's window. Only the
 * EP147 process may spawn the overlay. */
static int is_ep147(void) {
    char comm[64] = {0};
    FILE *f = fopen("/proc/self/comm", "re");
    if (f) {
        if (fgets(comm, sizeof comm, f)) {
        }
        fclose(f);
    }
    char *nl = strchr(comm, '\n');
    if (nl)
        *nl = 0;
    return strcmp(comm, "EP147") == 0;
}
static void ep147_late_init(void) { /* EP147 addresses that cannot be constant initializers */
    WAVEW_MODES[0].vt = EP_2b66d98;
    WAVEW_MODES[1].vt = EP_2b67408;
    WAVEW_MODES[2].vt = EP_2b67a78;
    WSK_VT[WSK_RGB] = WP_VT_RGB;
    WSK_VT[WSK_BLUE] = WP_VT_BLUE;
    WSK_VT[WSK_3BAND] = WP_VT_3BAND;
    g_wp_real[0] = (void *) EP_1b948d0;
    g_wp_real[1] = (void *) EP_1b949b8;
    g_wp_real[2] = (void *) EP_1b95e80;
}
__attribute__((constructor)) static void azmod_init(void) {
    if (!is_ep147())
        return;
    { /* Which EP147? Exact md5 identity; anything without a complete table stays inert. */
        char md5[33];
        long sz = 0;
        int b = ep147_identify("/proc/self/exe", md5, &sz);
        int ok = ep147_addr_init(b);
        FILE *f = azlog_open("/tmp/azmod-build.log", "a");
        if (f) {
            fprintf(f, "[%ld] EP147 %s md5=%s size=%ld -> %s\n", (long) time(NULL),
                    ep147_build_name(b), md5, sz,
                    ok ? "mods ON" : "INERT (unknown EP147 build or incomplete address table)");
            fclose(f);
        }
        if (!ok)
            return; /* g_azm_inert stays 1 */
        g_ep147_build = b;
        ep147_late_init();
        ep_img_init();     /* img_q: EP147's own image bounds */
        msg_thread_note(); /* this constructor runs on EP147's main thread = JUCE's message thread */
        g_azm_inert = 0;
    }
    bt_uart_recover(); /* before EP147 can open /dev/ttyS0 (synchronous, <= ~1 s) */
    install_browse_columns();  /* PREVIEW and the sub column remembered; Column Edit's columns */
    install_browse_edit();     /* Column Edit (after browse_columns.c: it hooks its layout) */
    install_clock_native();    /* the Clock in the place of EP147's status bar readout */
    install_bt_mods(); /* Bluetooth Auto-Connect / Remember Selected Channel: inert
                          while off */
    install_font_size(); /* Remember Font Size: before EP147 loads its settings */
    install_streaming_key(); /* streaming keys in Camelot */
    install_key_pane();  /* the deck screen's key displays in Camelot */
    install_spin_back(); /* Extended Lead-In: inert while off */
    /* The reporter's handlers are SA_ONSTACK. This is EP147's main thread (JUCE's message
     * thread): give it an alternate stack so a stack overflow there is still recorded. Our
     * own threads get theirs from az_thread_create. */
    azalt_this_thread(0);
    install_crash_reporter(); /* early: a start-up crash must be caught too */
    {
        pthread_t ct;
        if (az_thread_create(&ct, NULL, cr_watch, NULL) == 0)
            pthread_detach(ct);
    }
    load_settings_early(); /* the row values, before any hook that reads one installs */
    wz_restore();          /* Remember Waveform Zoom: the saved zoom, before EP147's main */
    install_banner();        /* unconditional, inert; must precede EP147Application::initialise */
    install_daymode_early(); /* Themes: record EP147's art as it decodes (off only
                                with azmod-themes.off) */
    install_menu_probe();    /* opt-in azmod-menuprobe.on */
    install_touchaccum();
    install_menu_popup(); /* gated on the Playlist Editing mod at use */
    pngpatch_apply();
    /* Every remaining hook, still before EP147's main (mod_menu_overlay.c). */
    install_static_hooks();
    /* EP147 starts its threads once this constructor returns: from here an inline hook must be
     * the one-store island B, and a 16-byte patch is refused (inline_hooks.c). */
    azm_hooks_go_late();
    pthread_t t;
    if (az_thread_create(&t, NULL, overlay_thread, NULL) == 0)
        pthread_detach(t);
}
