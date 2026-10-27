/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file wave_zoom.c
 * @brief Remember Waveform Zoom: the detailed waveform's zoom survives a restart.
 *
 * EP147 keeps the zoom of the detailed waveform as one static ratio, shared by every deck, and
 * starts every process at 1/4: it never saves it, so each restart (a power cycle too) shows
 * the stock zoom again. With Extras > Remember Waveform Zoom on:
 *   - the overlay thread reads the ratio on every tick and, once a new level has settled
 *     (wave_zoom.h, WZ_SETTLE_MS), saves it as azmod.conf's "wave.zoom" through the same
 *     durable writer as every other setting: one save per change, never one per frame, and
 *     never on EP147's own threads;
 *   - azmod_init writes the saved level back once, after the early settings load and before
 *     EP147's main, when no other thread exists yet. It writes only while the ratio still
 *     reads as EP147's start value and the saved text is one of the six levels.
 * EP147's own zoom control keeps stepping from the restored level: the value is the one its
 * step function reads and writes. Off, nothing is read or written and the zoom is stock.
 *
 * Entry points: wz_restore, wz_tick.
 */
/* EP147, gui::detailed_waveform::DispStateHolder (a gui::IHuiControllable):
 *   - the zoom: u16 numerator at DAT_03b32488 and u16 denominator right after it, initialised
 *     to 1/4 in .data. Static, so no constructor receives a deck: one zoom for all decks;
 *   - the step function, vtable slot +0x20 FUN_01b98b38(this, ?, delta), loads and stores
 *     only that pair; a reset elsewhere stores 1/4 into it as one 32-bit word. */
#define WZ_RATIO EP_3b32488
/* g_wz_saved, the level azmod.conf holds, and its load and save (wz_conf_apply,
 * wz_conf_capture) live with the other settings in azmod.c. */
static struct wz_watch g_wz_watch; /* overlay thread only */

static void wz_log(const char *fmt, ...) {
    FILE *f = azlog_open("/tmp/azmod-wavezoom.log", "a");
    if (!f)
        return;
    fprintf(f, "[%ld] ", (long) time(NULL));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}
/* The live ratio (num | den << 16), or 0 with *ok = 0 on a build without the address. The
 * pair lies in EP147's static data, mapped for the life of the process (img_q). */
static uint32_t wz_live(int *ok) {
    uintptr_t q = 0;
    *ok = WZ_RATIO && img_q(WZ_RATIO, &q);
    return *ok ? (uint32_t) q : 0;
}
/* azmod_init, after load_settings_early, on EP147's main thread before its main: no other
 * thread reads or writes the ratio yet. */
static void wz_restore(void) {
    if (!az_value(AZF_WAVE_ZOOM))
        return;
    int ok = 0;
    uint32_t live = wz_live(&ok), saved = __atomic_load_n(&g_wz_saved, __ATOMIC_RELAXED);
    char want[WZ_TEXT];
    wz_format(want, sizeof want, saved);
    if (!ok) {
        wz_log("restore: no zoom address for this EP147 build: nothing written\n");
        return;
    }
    if (!wz_restore_ok(live, saved)) {
        wz_log("restore: live %u/%u, saved \"%s\": nothing written\n", WZ_NUM(live), WZ_DEN(live), want);
        return;
    }
    *(volatile uint32_t *) WZ_RATIO = saved;
    int ok2 = 0;
    uint32_t now = wz_live(&ok2);
    wz_log("restore: live %u/%u -> %s, reads %u/%u\n", WZ_NUM(live), WZ_DEN(live), want, WZ_NUM(now),
           WZ_DEN(now));
}
/* The overlay thread, every tick: one load; a save only when a new level has settled. */
static void wz_tick(long now) {
    if (!az_value(AZF_WAVE_ZOOM)) {
        wz_watch_init(&g_wz_watch); /* turned on later: the value settles again first */
        return;
    }
    int ok = 0;
    uint32_t live = wz_live(&ok);
    if (!ok || !wz_watch_step(&g_wz_watch, live, __atomic_load_n(&g_wz_saved, __ATOMIC_RELAXED), now))
        return;
    __atomic_store_n(&g_wz_saved, live, __ATOMIC_RELAXED);
    save_config(&g_state);
    char w[WZ_TEXT];
    wz_format(w, sizeof w, live);
    wz_log("saved %s\n", w);
}
