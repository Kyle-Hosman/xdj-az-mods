/* SPDX-License-Identifier: GPL-3.0-only */
/* Remember Waveform Zoom's saved text (guest/azmod/wave_zoom.h): azmod.conf's "wave.zoom" value,
 * so any bytes as far as the parser is concerned.
 *
 * The input is the value (cut at its first NUL, as azconf hands it over). An accepted value must
 * be one of the six levels and must be exactly the text that level formats as. The first four
 * bytes are also read as a live ratio: a ratio that formats must parse back to itself, and only a
 * level may be restored or saved. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "fuzz_common.h"
#include "wave_zoom.h"

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    if (n > 4096) return 0;
    char *s = malloc(n + 1);
    if (!s) return 0;
    memcpy(s, d, n);
    s[n] = 0;
    uint32_t v = wz_parse(s);
    char w[WZ_TEXT];
    if (v != WZ_NONE) {
        FZ_ASSERT(wz_level_ok(v));
        wz_format(w, sizeof w, v);
        FZ_ASSERT(!strcmp(w, s));
    }
    free(s);
    if (n >= 4) {
        uint32_t live = (uint32_t) d[0] | (uint32_t) d[1] << 8 | (uint32_t) d[2] << 16 | (uint32_t) d[3] << 24;
        wz_format(w, sizeof w, live);
        FZ_ASSERT(strlen(w) < sizeof w);
        FZ_ASSERT(w[0] ? wz_parse(w) == live && wz_level_ok(live) : !wz_level_ok(live));
        FZ_ASSERT(!wz_restore_ok(live, v) || (live == WZ_STOCK && wz_level_ok(v)));
        struct wz_watch ww;
        wz_watch_init(&ww);
        int saved = wz_watch_step(&ww, live, v, 0) | wz_watch_step(&ww, live, v, WZ_SETTLE_MS);
        FZ_ASSERT(!saved || (wz_level_ok(live) && live != v));
    }
    return 0;
}
