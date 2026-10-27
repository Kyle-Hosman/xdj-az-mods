/* SPDX-License-Identifier: GPL-3.0-only */
/* azmod menu: the Mod Menu's model and software compositor. Mostly pure logic: the parts that
 * touch files (screen_themes.c, screen_community.c) say so in their headers.
 * One coordinate system: framebuffer pixels. Geometry helpers (azmod_row_rect,
 * azmod_item_rect) are shared by hit-testing and drawing so they never drift. */
#include "menu.h"
#include "../common/azlogcap.h" /* every /tmp append is size-capped */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include "clock_tz.h"     /* the zone list's offsets (screen_overlays.c) */
#include "clock_set.h"    /* Set Time Manually (screen_overlays.c) */
#include "clock_native.h" /* the Clock's place in EP147's readout (screen_overlays.c) */
#include "clock_timer.h"  /* the Clock's countdown Timer (screen_overlays.c) */
#include "azfeatures.c" /* the row table: pure logic, in every build of menu.c */

/* The Mod Menu is one translation unit: the parts below are #included in this order, each
 * using only what the ones above it define. A part never #includes another part, so a test
 * can swap one for a mutated copy placed next to a copy of this file. */
#include "ui_draw.c"
#include "menu_layout.c"
#include "menu_list.c"
#include "screen_colors.c"
#include "menu_state.c"
#include "screen_update.c"
#include "changelog.c"
#include "screen_themes.c"
#include "menu_full.c"
#include "screen_community.c"
#include "screen_overlays.c"
#include "screen_sysinfo.c"
#include "stem_overlay.c"
#include "menu_input.c"
#include "menu_composite.c"
