/* SPDX-License-Identifier: GPL-3.0-only */
/* The panel-bus read path (guest/azmod/bus_interpose.c, from "---- read(): the panel input
 * buses" to the end, plus crc16_x25), replayed against stand-ins for the rest of azmod.
 * busread-trace.sh extracts those regions, builds this file around them and runs a seeded
 * stream of frames and state changes: mixer, ErP and other buses, every frame length, valid
 * and corrupted CRCs, the menu, drag, drum roll and stems state shuffled, and overlay stalls
 * past the watchdog. It prints, per frame, what EP147 would read, every variable the read path
 * publishes and every call it makes. The trace's MD5 is recorded in busread-trace.md5: a
 * change to the read path that changes behavior changes the hash, on purpose, and the commit
 * that updates the hash says why. A pure refactor must leave it unchanged. */
#define _GNU_SOURCE
#define _Atomic /* single-threaded harness: the builtins need plain integers on clang */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define AZMOD_RELEASE 1
#define AZMOD_STEM_DECKS 4
#define AZMOD_PAD_DECKS 2
#define AZ_NBUS 4
#define MAX_GPIODRV_FDS 16
#define PLO_MAXROWS 64
#define DRAG_KIND_PLIST 1
#define NM_NAMING 1
#define XPAD_FX_OFF 0x54
#define XPAD_FX_MSK 0x80
#define AZ_PROBE_ON(p) 0
#define dbg(...) ((void) 0)
#define SRCROW_TRACE(...) ((void) 0)
#define NEXT(p, name) ((void) 0)
#include "bus_cfg.h"
#include "bl_flip.h"
#include "stem_fx_pads.h"
#include "drag_handoff.h"
#include "mm_resume.h"
#include "touch_read_guard.h" /* a failed read on the touch device (not in this stream) */

/* the community mods' filter chain (sdk_input.c): no mod registered, as in the shipped default */
#define AZ_BUS_ERP_LED 1
#define AZ_BUS_ERP_IN 2
#define AZ_BUS_MIXER_IN 3
#define AZ_BUS_MIXER_LED 4
#define AZ_BUS_DECK_IN 5
static int g_deckbus_fd[2] = {-1, -1}, g_touchin_fd = -1;
/* community filters: every mixer frame they are handed must carry a CRC (>= 98 bytes, checked
 * before the call); one that is not counts as a violation and fails the run (exit 3) */
static long g_filt_unchecked;
static int sdk_filter_run(int bus, unsigned char *f, int len) {
    (void) f;
    g_filt_unchecked += bus == 3 /* AZ_BUS_MIXER_IN */ && len < 98;
    return 0;
}
static void sdk_filter_raw(int bus, const unsigned char *f, int len) {
    (void) f;
    g_filt_unchecked += bus == 3 && len < 98;
}
static void sdk_filter_view(int bus, const unsigned char *f, int len) { (void) bus, (void) f, (void) len; }
static void sdk_touch_read(void *buf, ssize_t r) { (void) buf, (void) r; }
static inline int azmod_logical_deck(int side, int sel) { return side + (sel ? AZMOD_PAD_DECKS : 0); }

/* ---- the state read() shares with the rest of azmod ---- */
static int g_azm_inert = 0, g_mixer_fd = 10, g_erp_fd = 11, g_debug = 0;
static int g_bus_fd[AZ_NBUS] = {12, 13, 10, 14};
static const char *g_bus_tag[AZ_NBUS] = {"erp", "dA", "mix", "dB"};
static int g_gpiodrv_fds[MAX_GPIODRV_FDS], g_gpiodrv_last_level[MAX_GPIODRV_FDS], g_gpiodrv_fd_count;
static long g_gpiodrv_gpio[MAX_GPIODRV_FDS];
static int g_udev_fd[4] = {-1, -1, -1, -1}, g_hidinj_peer = -1;
static long g_hidinj_reads;
static volatile unsigned g_pad_req[AZMOD_STEM_DECKS], g_pad_solo_req[AZMOD_STEM_DECKS];
static unsigned g_erp_last8[AZMOD_STEM_DECKS];
static volatile int g_back_pending, g_click_pending, g_close_pending, g_enc_dir, g_menu_focused,
    g_menu_open, g_search_on, g_src_visible, g_srcrow_open, g_srcrow_want, g_wave_forcestems;
static _Atomic int g_bl_enter[AZMOD_PAD_DECKS], g_bl_vpage2[AZMOD_STEM_DECKS];
static _Atomic unsigned g_caption_repaint;
static volatile unsigned g_dbg_modebyte[4], g_dbg_padbits[4], g_dbg_padrise[4], g_dbg_reqraised[4];
static int g_deck_sel[AZMOD_PAD_DECKS];
static struct dh_slot g_dh = DH_SLOT_INIT;
static _Atomic int g_drag_committing, g_drag_into = -1, g_drag_into_dir, g_drag_into_disp = -1,
                                      g_drag_into_note, g_drag_kind, g_drag_on, g_drag_origin, g_drag_rows,
                                      g_drag_target, g_drum_arm;
static int g_drum_held[AZMOD_STEM_DECKS];
static int g_drum_hold[AZMOD_STEM_DECKS];
#define DRUM_HOLD_ON 1
#define DRUM_HOLD_SUSTAIN 2
static volatile unsigned g_drum_sel_led[AZMOD_STEM_DECKS];
static volatile int g_erp_active[AZMOD_STEM_DECKS];
static long g_erp_crc_ok, g_erp_crc_bad;
static unsigned g_erp_last[4], g_erp_mode_last[4], g_sel_last[AZMOD_PAD_DECKS];
static _Atomic int g_fxrow_zone = -1, g_knob_held, g_knob_raw, g_menu_top = 1, g_mic2_tone_tlm,
                   g_mode_down[AZMOD_PAD_DECKS], g_mp_submode, g_mp_subback, g_nm_cancel_req,
                   g_reorder_enabled, g_shift_down, g_srcrow_pin_ours, g_srcrow_repaint,
                   g_srcrow_unfocus, g_stemeq_knobs[2], g_xpad_raw;
static _Atomic unsigned g_kn_leak, g_kn_rel, g_kn_wdmax, g_kn_frames, g_mode_edge_seq[2], g_mode_seq;
static _Atomic long long g_knob_press_ms;
static _Atomic long g_mode_edge_ms[2], g_viewleave_n, g_wave_seen_ms = 99;
static int g_last_click_bit, g_last_back_bit, g_src_swallow, g_srcrow_active;
static int g_mm_resume; /* the Mod Menu to show again on SOURCE (mm_resume.h) */
static struct {
    _Atomic int state;
} g_nm;
static volatile unsigned g_overlay_beat;
static int g_pad_mode_bl[4], g_pad_mode_slip[4];
static unsigned char g_plo_folder[PLO_MAXROWS], g_plo_have[PLO_MAXROWS];
static int g_plo_n = PLO_MAXROWS;
static unsigned wd_last, wd_stall;
static _Atomic int g_sdk_focus, g_sdk_back_pending; /* no mod surface has focus here */

/* ---- the fuzzed environment ---- */
static uint64_t rng = 88172645463325252ull;
static unsigned rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (unsigned) rng;
}
static uint64_t g_cfg;
static int env_bl_swap, env_slip[4], env_stems_page[4], env_fresh[4];
static long long clock_ms;
static uint64_t bus_cfg(void) { return g_cfg; }
static int bl_swap(void) { return env_bl_swap; }
static int drum_mode_is_slip(int d, int ld) { return env_slip[(d * 7 + ld) & 3]; }
static int stems_page(int d, int ld) { return env_stems_page[(d * 5 + ld) & 3]; }
static int pad_mode_fresh(int d, int ld) { return env_fresh[(d + ld) & 3]; }
static long now_ms(void) { return (long) clock_ms; }
static long long drag_now_ms(void) { return clock_ms; }
static FILE *azlog_open(const char *p, const char *m) {
    (void) p, (void) m;
    return NULL;
}
#define drag_log(...) ((void) 0)
static int drag_remap_any(int nt) { return (nt * 3 + 1) % PLO_MAXROWS; }
static void drag_request_update(void) { printf(" C:drag_update"); }
static void drum_disarm_all(const char *why) { printf(" C:disarm_all(%s)", why); }
static void drum_disarm_others(int ld, const char *why) { printf(" C:disarm_others(%d,%s)", ld, why); }
static void f_gate(int a, int b) { printf(" C:gate(%d,%d)", a, b); }
static void f_select(int a, int b) { printf(" C:select(%d,%d)", a, b); }
static void f_rate(int a, int b) { printf(" C:rate(%d,%d)", a, b); }
static void f_touch(int a, int b) { printf(" C:touch(%d,%d)", a, b); }
static void f_tone(int a, int b) { printf(" C:tone(%d,%d)", a, b); }
/* clang-format off */
static void (*g_drum_gate_fn)(int, int) = f_gate, (*g_drum_select_fn)(int, int) = f_select,
    (*g_drum_rate_fn)(int, int) = f_rate, (*g_drum_touch_fn)(int, int) = f_touch,
    (*g_drum_tone_fn)(int, int) = f_tone;
/* clang-format on */
/* Stem FX pads: azstem's calls are present and the mode is off, as with Stem FX = Waveforms
 * + Solo, so the trace is the solo one (stem_fx_pads.h has its own suite). */
static unsigned f_sfx_on(int d) {
    printf(" C:sfx_on(%d)", d);
    return 0;
}
static int f_sfx(int d, int f, int on) {
    printf(" C:sfx(%d,%d,%d)", d, f, on);
    return 0;
}
static int g_stemfx_pads;
static unsigned (*g_sfx_on_fn)(int) = f_sfx_on;
static int (*g_sfx_fn)(int, int, int) = f_sfx;
static sfxp_hold g_sfxp[AZMOD_PAD_DECKS][SFXP_N];

static uint8_t frame[160];
static size_t frame_len;
static ssize_t fake_read(int fd, void *buf, size_t n) {
    (void) fd;
    size_t m = frame_len < n ? frame_len : n;
    memcpy(buf, frame, m);
    return (ssize_t) m;
}
static ssize_t (*next_read)(int, void *, size_t) = fake_read;

#include CRC_INC
#define read bus_read
#include BI_INC
#undef read

static void stamp(uint8_t *f, size_t len) {
    uint16_t c = crc16_x25(f, len);
    f[len] = (uint8_t) c, f[len + 1] = (uint8_t) (c >> 8);
}
static uint8_t biased(void) { /* button bytes: mostly 0, single bits, sometimes anything */
    unsigned k = rnd() % 8;
    return k < 4 ? 0 : k < 7 ? (uint8_t) (1u << (rnd() % 8))
                             : (uint8_t) rnd();
}
/* clang-format off */
#define P(name) printf(" " #name "=%lld", (long long) (name))
#define PA(name, n) do { printf(" " #name "="); for (int _i = 0; _i < (n); _i++) printf("%lld,", (long long) (name)[_i]); } while (0)
/* clang-format on */

int main(int argc, char **argv) {
    long iters = argc > 1 ? atol(argv[1]) : 200000;
    for (long it = 0; it < iters; it++) {
        clock_ms += rnd() % 40;
        /* change the surroundings now and then */
        if (rnd() % 16 == 0) {
            g_menu_open = rnd() & 1, g_menu_focused = rnd() & 1, g_srcrow_want = rnd() & 1;
            g_src_visible = rnd() & 1, g_srcrow_active = rnd() & 1, g_menu_top = rnd() & 1;
            g_mp_submode = (rnd() % 4) == 0, g_nm.state = (rnd() % 5) == 0;
            g_drag_on = rnd() & 1, g_drag_kind = rnd() & 1, g_drag_committing = (rnd() % 4) == 0;
            g_drag_rows = (int) (rnd() % 60), g_drag_target = (int) (rnd() % 60), g_drum_arm = rnd() & 1;
            for (int i = 0; i < 4; i++) {
                g_drum_held[i] = (rnd() % 3) == 0, g_erp_active[i] = rnd() & 1;
                env_slip[i] = rnd() & 1, env_stems_page[i] = rnd() & 1, env_fresh[i] = (int) (rnd() % 6);
            }
            env_bl_swap = rnd() & 1, g_wave_forcestems = rnd() & 1;
            for (int i = 0; i < PLO_MAXROWS; i++)
                g_plo_have[i] = rnd() & 1, g_plo_folder[i] = (rnd() % 4) == 0;
            struct bus_xpad x = {(int) (rnd() % 0x60), (rnd() % 3) ? -1 : (int) (rnd() % 0x60),
                                 (int) (rnd() % 256), 2 + (int) (rnd() % 255),
                                 (rnd() % 2) ? -1 : (int) (rnd() % 0x60)};
            g_cfg = bus_cfg_pack(rnd() & 1, rnd() & 1, rnd() & 1, (rnd() % 4) == 0, &x);
            struct dh_intent taken;
            if (dh_take(&g_dh, &taken)) /* the message tick performs the recorded end */
                printf(" T:take(%d,%d,%d,%d,%d)", taken.op, taken.origin, taken.target, taken.into, taken.into_disp);
            if (rnd() % 2)
                dh_open(&g_dh);
            g_drag_committing = (rnd() % 8) == 0;
        }
        static int stall_left;
        if (!stall_left && rnd() % 2000 == 0)
            stall_left = 600 + (int) (rnd() % 900); /* the overlay thread stalls for a while */
        if (stall_left)
            stall_left--;
        else if (rnd() % 3)
            g_overlay_beat++; /* the overlay is alive most of the time */
        int fd;
        unsigned pick = rnd() % 10;
        if (pick < 5) { /* mixer */
            fd = g_mixer_fd;
            static const size_t lens[] = {0, 11, 12, 34, 35, 60, 76, 97, 98, 128};
            frame_len = lens[rnd() % 10];
            for (size_t i = 0; i < frame_len; i++)
                frame[i] = (i >= 0x2e && i <= 0x4b) || i == 34 || i == 0x16 ? (uint8_t) rnd() : biased();
            if (frame_len >= 98 && rnd() % 10)
                stamp(frame, 96);
        } else if (pick < 9) { /* ErP */
            fd = g_erp_fd;
            frame_len = rnd() % 8 ? 30 : (size_t) (rnd() % 40);
            for (size_t i = 0; i < frame_len; i++)
                frame[i] = biased();
            if (frame_len > 24)
                frame[24] = rnd() % 10 ? (uint8_t) (0x01 | (rnd() & 0x06)) : (uint8_t) rnd();
            if (frame_len >= 30 && rnd() % 10)
                stamp(frame, 28);
        } else { /* any other fd */
            fd = 20 + (int) (rnd() % 3);
            frame_len = rnd() % 64;
            for (size_t i = 0; i < frame_len; i++)
                frame[i] = (uint8_t) rnd();
        }
        uint8_t out[160];
        memset(out, 0xAA, sizeof out);
        printf("%ld fd=%d", it, fd);
        ssize_t r = bus_read(fd, out, sizeof out);
        printf(" r=%zd out=", r);
        for (ssize_t i = 0; i < r; i++)
            printf("%02x", out[i]);
        /* clang-format off */
        P(g_back_pending); P(g_click_pending); P(g_close_pending); P(g_enc_dir); P(g_menu_focused);
        P(g_menu_open); P(g_search_on); P(g_srcrow_open); P(g_menu_top); P(g_mp_subback);
        P(g_nm_cancel_req); P(g_reorder_enabled); P(g_shift_down); P(g_srcrow_pin_ours);
        P(g_srcrow_repaint); P(g_srcrow_unfocus); P(g_xpad_raw); P(g_fxrow_zone); P(g_knob_held);
        P(g_knob_raw); P(g_knob_press_ms); P(g_mic2_tone_tlm); P(g_kn_leak); P(g_kn_rel);
        P(g_kn_wdmax); P(g_kn_frames); P(g_mode_seq); P(g_viewleave_n); P(g_wave_seen_ms);
        P(g_drag_target); P(g_drag_into); P(g_drag_into_dir); P(g_drag_into_disp); P(g_drag_into_note);
        P(g_caption_repaint); P(g_erp_crc_ok); P(g_erp_crc_bad); P(wd_stall); P(g_src_swallow);
        P(g_last_click_bit); P(g_last_back_bit); P(g_dh.state); P(g_dh.it.op); P(g_dh.it.origin); P(g_dh.it.target); P(g_dh.it.into); P(g_dh.posted); P(g_dh.refused); P(g_dh.full);
        PA(g_pad_req, 4); PA(g_pad_solo_req, 4); PA(g_bl_enter, 2); PA(g_bl_vpage2, 4);
        PA(g_deck_sel, 2); PA(g_drum_held, 4); PA(g_drum_hold, 4); PA(g_drum_sel_led, 4); PA(g_pad_mode_bl, 4);
        PA(g_pad_mode_slip, 4); PA(g_mode_down, 2); PA(g_mode_edge_ms, 2); PA(g_mode_edge_seq, 2);
        PA(g_stemeq_knobs, 2); PA(g_erp_last, 4); PA(g_erp_last8, 4); PA(g_dbg_reqraised, 4);
        /* clang-format on */
        putchar('\n');
        /* the overlay thread drains the screen-button close: a frame's line shows whether that
         * frame raised one, not whether any frame ever did */
        g_close_pending = 0;
    }
    if (g_filt_unchecked) {
        fprintf(stderr, "busread-trace: %ld mixer frames without a CRC reached community filters\n",
                g_filt_unchecked);
        return 3;
    }
    return 0;
}
