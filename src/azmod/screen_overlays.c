/* SPDX-License-Identifier: GPL-3.0-only */
/* screen_overlays.c: Overlays (AZ_OVERLAYS): its rows, and the deck-screen readouts it switches (FPS, CPU, RAM, temperature, clock).
 * Part of the Mod Menu's one translation unit: #included by menu.c, in the order listed there. */
/* The rows of an Extras row's own screen (AZ_OVERLAYS, AZ_CLOCK, AZ_BROWSE_VIEW): a switch first
 * (a group has none), then the table's rows under it, in their order. */
#define OVERLAYS_MAX 16
/* The Clock's time zone: a pseudo row (not a table row; its value is azmod.conf's "clock.tz",
 * like the theme), listed right after the Clock's time format. */
#define C_TZ_ROW 0x300
_Static_assert(C_TZ_ROW > AZI_COUNT, "the time zone pseudo row must not collide with item ids");
/* Set Time Manually: a pseudo row after the time zone; it opens the picker (AZ_CLOCK_SET). */
#define C_SET_ROW 0x301
/* the row a screen belongs to: Overlays, the Clock or Browse View Mods */
static int sub_head(int mode) {
    return mode == AZ_CLOCK ? AZF_CLOCK : mode == AZ_BROWSE_VIEW ? AZF_BROWSE_VIEW
                                                                 : AZF_OVERLAYS;
}
/* the screen a row opens, if it is one of these */
static int sub_screen(int it) {
    return it == AZF_CLOCK ? AZ_CLOCK : it == AZF_OVERLAYS  ? AZ_OVERLAYS
                                    : it == AZF_BROWSE_VIEW ? AZ_BROWSE_VIEW
                                                            : 0;
}
/* is this one of those screens */
static bool sub_mode(int mode) {
    return mode == AZ_OVERLAYS || mode == AZ_CLOCK || mode == AZ_BROWSE_VIEW;
}
static int overlays_rows(int head, int *out) {
    int ord[AZI_COUNT], n = menu_order(ord), m = 0;
    if (az_kind(head) == AZ_ROW_TOGGLE)
        out[m++] = head;
    for (int p = 0; p < n && m < OVERLAYS_MAX; p++)
        if (az_parent(ord[p]) == head && !(az_row(ord[p])->d.flags & AZ_ROW_HIDDEN)) {
            out[m++] = ord[p];
            if (ord[p] == AZF_CLOCK_SECONDS && m + 1 < OVERLAYS_MAX) {
                out[m++] = C_TZ_ROW;
                out[m++] = C_SET_ROW;
            }
        }
    return m;
}

/* The Clock's time zones by region, each region west to east: one for each common offset, with
 * the zone's own daylight-saving rules from the deck's /usr/share/zoneinfo. Index 0 (UTC) is the
 * default. azmod.conf keeps the zone's name, never its index, so the order is free. */
enum {
    TZR_UTC,
    TZR_AMERICAS,
    TZR_EUROPE,
    TZR_AFRICA,
    TZR_ASIA,
    TZR_PACIFIC,
    TZR_N
};
static const char *const C_TZ_REGION[TZR_N] = {"UTC", "Americas", "Europe", "Africa", "Asia",
                                               "Australia and Pacific"};
static const struct {
    const char *zone, *name;
    int region;
} C_TZ[] = {
    {"Etc/UTC", "UTC", TZR_UTC},
    {"America/Anchorage", "Anchorage", TZR_AMERICAS},
    {"America/Los_Angeles", "Los Angeles", TZR_AMERICAS},
    {"America/Phoenix", "Phoenix", TZR_AMERICAS},
    {"America/Denver", "Denver", TZR_AMERICAS},
    {"America/Chicago", "Chicago", TZR_AMERICAS},
    {"America/Mexico_City", "Mexico City", TZR_AMERICAS},
    {"America/New_York", "New York", TZR_AMERICAS},
    {"America/Halifax", "Halifax", TZR_AMERICAS},
    {"America/Sao_Paulo", "Sao Paulo", TZR_AMERICAS},
    {"America/Argentina/Buenos_Aires", "Buenos Aires", TZR_AMERICAS},
    {"Atlantic/Azores", "Azores", TZR_EUROPE},
    {"Europe/London", "London", TZR_EUROPE},
    {"Europe/Lisbon", "Lisbon", TZR_EUROPE},
    {"Europe/Paris", "Paris", TZR_EUROPE},
    {"Europe/Berlin", "Berlin", TZR_EUROPE},
    {"Europe/Amsterdam", "Amsterdam", TZR_EUROPE},
    {"Europe/Athens", "Athens", TZR_EUROPE},
    {"Europe/Istanbul", "Istanbul", TZR_EUROPE},
    {"Europe/Moscow", "Moscow", TZR_EUROPE},
    {"Africa/Lagos", "Lagos", TZR_AFRICA},
    {"Africa/Johannesburg", "Johannesburg", TZR_AFRICA},
    {"Africa/Cairo", "Cairo", TZR_AFRICA},
    {"Asia/Dubai", "Dubai", TZR_ASIA},
    {"Asia/Karachi", "Karachi", TZR_ASIA},
    {"Asia/Kolkata", "India", TZR_ASIA},
    {"Asia/Bangkok", "Bangkok", TZR_ASIA},
    {"Asia/Singapore", "Singapore", TZR_ASIA},
    {"Asia/Shanghai", "China", TZR_ASIA},
    {"Asia/Tokyo", "Tokyo", TZR_ASIA},
    {"Asia/Seoul", "Seoul", TZR_ASIA},
    {"Pacific/Honolulu", "Honolulu", TZR_PACIFIC},
    {"Australia/Perth", "Perth", TZR_PACIFIC},
    {"Australia/Brisbane", "Brisbane", TZR_PACIFIC},
    {"Australia/Adelaide", "Adelaide", TZR_PACIFIC},
    {"Australia/Sydney", "Sydney", TZR_PACIFIC},
    {"Pacific/Auckland", "Auckland", TZR_PACIFIC},
};
#define C_TZ_N ((int) (sizeof C_TZ / sizeof C_TZ[0]))
int g_azmod_clock_tz; /* menu.h: an index into C_TZ; __atomic builtins only */
int azmod_clock_tz_index(const char *zone) {
    for (int k = 0; zone && k < C_TZ_N; k++)
        if (!strcmp(zone, C_TZ[k].zone))
            return k;
    return -1;
}
const char *azmod_clock_tz_zone(int k) {
    return k >= 0 && k < C_TZ_N ? C_TZ[k].zone : C_TZ[0].zone;
}
static int clock_tz_get(void) {
    int k = __atomic_load_n(&g_azmod_clock_tz, __ATOMIC_RELAXED);
    return k >= 0 && k < C_TZ_N ? k : 0;
}

/* ---- the zone list (AZ_CLOCK_TZ), opened from the Clock screen's Time Zone row --------------
 * Its rows: each region's name (a heading, not selectable), then its zones. The current zone is
 * marked; the knob moves through the zones and a click picks one, a tap on a zone picks it, and a
 * drag (full-screen menu) scrolls the list a whole row at a time. A pick sets the zone and goes
 * back to the Clock screen; BACK goes back without changing it. */
#define TZL_MAX (C_TZ_N + TZR_N)
/* row r: a zone index (>= 0), or -1 - region for a heading; returns the row count */
static int tzl_rows(int *row) {
    int n = 0;
    for (int g = 0; g < TZR_N; g++) {
        int head = 0;
        for (int k = 0; k < C_TZ_N; k++) {
            if (C_TZ[k].region != g)
                continue;
            if (!head++)
                row[n++] = -1 - g;
            row[n++] = k;
        }
    }
    return n;
}
static int tzl_row_of(int zone) {
    int row[TZL_MAX], n = tzl_rows(row);
    for (int r = 0; r < n; r++)
        if (row[r] == zone)
            return r;
    return -1;
}
static int g_tzl_first; /* the first row shown (UI thread) */
static int tzl_rowh(const azmod_fb *fb) {
    int x, y, w, h;
    colors_row_rect(fb, 0, &x, &y, &w, &h);
    return h;
}
/* rows that fit between the header and the bottom of the menu */
static int tzl_visible(const azmod_fb *fb) {
    int x, y, w, h;
    colors_row_rect(fb, 0, &x, &y, &w, &h);
    int bot = g_docked ? dock_y(fb) + dock_h(fb) : fb->h;
    int v = h > 0 ? (bot - y) / h : 1;
    return v < 1 ? 1 : v;
}
static int tzl_max_first(const azmod_fb *fb) {
    int row[TZL_MAX], n = tzl_rows(row), m = n - tzl_visible(fb);
    return m > 0 ? m : 0;
}
static void tzl_set_first(const azmod_fb *fb, int f) {
    int m = tzl_max_first(fb);
    g_tzl_first = f < 0 ? 0 : f > m ? m
                                    : f;
}
/* scroll so row r shows, with its region's heading when that fits too */
static void tzl_show(const azmod_fb *fb, int r) {
    int v = tzl_visible(fb);
    if (r - 1 < g_tzl_first)
        tzl_set_first(fb, r - 1);
    else if (r >= g_tzl_first + v)
        tzl_set_first(fb, r - v + 1);
}
/* Each zone's UTC offset in effect now, daylight saving applied (Denver UTC-6 in summer, UTC-7 in
 * winter): read from the deck's zone files when the list opens (never per frame). A zone whose file
 * cannot be read or answered shows no offset. */
#ifndef AZ_ZONEINFO
#define AZ_ZONEINFO "/usr/share/zoneinfo"
#endif
static int32_t g_tzl_off[C_TZ_N];
static unsigned char g_tzl_off_ok[C_TZ_N];
static void tzl_load_offsets(void) {
    static unsigned char buf[1 << 16];
    static struct ctz z;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    for (int k = 0; k < C_TZ_N; k++) {
        char p[160];
        g_tzl_off_ok[k] = 0;
        snprintf(p, sizeof p, AZ_ZONEINFO "/%s", C_TZ[k].zone);
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        ssize_t n = read(fd, buf, sizeof buf);
        close(fd);
        g_tzl_off_ok[k] =
            n > 0 && ctz_parse(buf, (size_t) n, &z) == 0 && ctz_offset(&z, (int64_t) ts.tv_sec, &g_tzl_off[k]) == 0;
    }
}
static void clock_tz_open(azmod_state *s, const azmod_fb *fb) {
    tzl_load_offsets();
    s->mode = AZ_CLOCK_TZ;
    s->pane = 1;
    s->menu_sel = tzl_row_of(clock_tz_get());
    g_tzl_first = 0;
    tzl_show(fb, s->menu_sel);
}
/* back to the Clock screen, on its Time Zone row */
static void clock_tz_close(azmod_state *s) {
    int ovr[OVERLAYS_MAX], n = overlays_rows(AZF_CLOCK, ovr);
    s->mode = AZ_CLOCK;
    s->menu_sel = 0;
    for (int r = 0; r < n; r++)
        if (ovr[r] == C_TZ_ROW)
            s->menu_sel = r;
}
static void clock_tz_pick(azmod_state *s, int row) {
    int rows[TZL_MAX], n = tzl_rows(rows);
    if (row < 0 || row >= n || rows[row] < 0)
        return; /* a heading or nothing: no zone */
    __atomic_store_n(&g_azmod_clock_tz, rows[row], __ATOMIC_RELAXED);
    s->config_dirty = true; /* azmod.c saves it to azmod.conf */
    clock_tz_close(s);
}
/* the list row under a point, or -1 */
static int tzl_row_at(const azmod_fb *fb, int tx, int ty) {
    int row[TZL_MAX], n = tzl_rows(row), v = tzl_visible(fb);
    for (int i = 0; i < v && g_tzl_first + i < n; i++) {
        int x, y, w, h;
        colors_row_rect(fb, i, &x, &y, &w, &h);
        if (in_rect(tx, ty, x, y, w, h))
            return g_tzl_first + i;
    }
    return -1;
}

/* The list: a heading in the dim ink, a zone with a check where the current one is. */
static void compose_clock_tz(azmod_state *s, const azmod_fb *fb, int rowh, int scale) {
    draw_menu_header(fb, rowh, scale, "Time Zone");
    int row[TZL_MAX], n = tzl_rows(row), v = tzl_visible(fb), cur = clock_tz_get();
    tzl_set_first(fb, g_tzl_first); /* the list's length or the screen may have changed */
    for (int i = 0; i < v && g_tzl_first + i < n; i++) {
        int r = g_tzl_first + i, x, y, w, h;
        colors_row_rect(fb, i, &x, &y, &w, &h);
        if (row[r] < 0) { /* a region's heading */
            int px = menu_text_px(fb) * 4 / 5;
            c_text(fb, x + sub_lx(fb), y, 0, h, C_TZ_REGION[-1 - row[r]], px, C_TEXT_DIM);
            continue;
        }
        bool hi = s->menu_sel == r || (s->pressed_item == r && s->pressed_ticks > 0);
        if (hi && g_full) {
            fill_rect(fb, x + fb->w / 80, y, w - fb->w / 40, h, 0x00484848u);
            draw_rect_outline(fb, x + fb->w / 80, y, w - fb->w / 40, h, 2, C_HI);
        } else if (hi)
            fill_rect(fb, x, y, w, h, C_HI);
        uint32_t ink = hi && !g_full ? C_HI_TEXT : C_TEXT;
        char label[64], off[16] = "";
        if (row[r] > 0 && g_tzl_off_ok[row[r]]) /* not on UTC itself */
            ctz_utc_label(off, sizeof off, g_tzl_off[row[r]]);
        snprintf(label, sizeof label, off[0] ? "%s (%s)" : "%s", C_TZ[row[r]].name, off);
        draw_text(fb, x + sub_lx(fb) + fb->w / 60, y + (h - 7 * scale) / 2, label, scale, ink);
        int sz = g_full ? fb->h * 20 / 800 : h / 2,
            cbx = x + w - sz - (g_full ? fb->w / 40 : h / 3), cby = y + (h - sz) / 2;
        draw_checkbox(fb, cbx, cby, sz, row[r] == cur, hi);
    }
    if (n > v) { /* EP147's own list scroll bar while the zones overflow */
        int x, y, w, h;
        colors_row_rect(fb, 0, &x, &y, &w, &h);
        draw_vsb(fb, x + w, y, v * h, n * h, v * h, g_tzl_first * h);
    }
}
/* The knob steps through the zones (headings skipped) and a click picks one; a touch on a zone
 * picks it (the full-screen menu decides a touch on its release instead: azmod_pane_drag). */
static bool input_clock_tz(azmod_state *s, const azmod_fb *fb, int touch_x, int touch_y,
                           bool touched, int enc_delta, bool enc_click) {
    int row[TZL_MAX], n = tzl_rows(row);
    if (enc_delta && n) {
        int r = s->menu_sel >= 0 && s->menu_sel < n ? s->menu_sel : 0, st = enc_delta > 0 ? 1 : -1;
        for (int k = 0; k < n; k++) { /* the next zone that way, wrapping */
            r = (r + st + n) % n;
            if (row[r] >= 0)
                break;
        }
        s->menu_sel = r;
        tzl_show(fb, r);
    }
    if (touched) {
        int r = tzl_row_at(fb, touch_x, touch_y);
        if (r >= 0 && row[r] >= 0) {
            clock_tz_pick(s, r);
            return true;
        }
    }
    if (enc_click)
        clock_tz_pick(s, s->menu_sel);
    return true; /* consume everything while the screen is up */
}

/* ---- Set Time Manually (AZ_CLOCK_SET), opened from the Clock screen ------------------------
 * Rows: Year, Month, Day, Hour, Minute (local time in the Clock's zone), then Set Time. A value
 * row has a minus and a plus square: a tap on one steps the value. The knob moves between rows;
 * a press on a value row starts editing it (the knob then steps the value) and a second press
 * ends that. Set Time hands the UTC time to the overlay thread (g_azmod_clock_set_req), which
 * sets the system time and the RTC (mod_menu_overlay.c ov_poll_clock), and goes back to the
 * Clock screen; BACK goes back without a change. The picker starts at the time when it is known,
 * else at the newest file time on a stick (the last export), else at the range's start
 * (clock_set.h cs_seed). */
#ifndef AZ_USBROOT
#define AZ_USBROOT "/media/usb"
#endif
#define CS_SET_ROWN CS_NFIELD /* the Set Time row */
int64_t g_azmod_clock_set_req; /* menu.h: the UTC time to set, 0 = none; __atomic only */
static struct cs_fields g_cset;
static bool g_cset_edit;
static struct ctz g_cset_z;
static int g_cset_zone; /* 1 = g_cset_z answers, 0 = UTC itself, -1 = the zone cannot be read */
static const char *const CS_LABEL[CS_NFIELD + 1] = {"Year", "Month", "Day", "Hour", "Minute", "Set Time"};
/* The newest modification time of a stick's export (rekordbox's files, else the PIONEER folder) */
static int64_t cs_newest_stick(void) {
    static const char *const REL[] = {"PIONEER/rekordbox/exportLibrary.db", "PIONEER/rekordbox/export.pdb", "PIONEER"};
    int64_t best = 0;
    DIR *d = opendir(AZ_USBROOT);
    if (!d)
        return 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.')
            continue;
        for (unsigned k = 0; k < sizeof REL / sizeof REL[0]; k++) {
            char p[512];
            struct stat st;
            snprintf(p, sizeof p, AZ_USBROOT "/%s/%s", e->d_name, REL[k]);
            if (stat(p, &st) == 0 && (int64_t) st.st_mtime > best)
                best = (int64_t) st.st_mtime;
        }
    }
    closedir(d);
    return best;
}
static int32_t cs_off_at(int64_t t) {
    int32_t off = 0;
    if (g_cset_zone != 1 || ctz_offset(&g_cset_z, t, &off) != 0)
        off = 0;
    return off;
}
static void clock_set_open(azmod_state *s) {
    static unsigned char zb[1 << 16];
    int k = clock_tz_get();
    g_cset_zone = k == 0 ? 0 : -1;
    if (k) {
        char p[160];
        snprintf(p, sizeof p, AZ_ZONEINFO "/%s", C_TZ[k].zone);
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t n = read(fd, zb, sizeof zb);
            close(fd);
            if (n > 0 && ctz_parse(zb, (size_t) n, &g_cset_z) == 0)
                g_cset_zone = 1;
        }
    }
    int64_t t = cs_seed(s->clock_text[0] != 0, (int64_t) time(NULL), s->clock_text[0] ? 0 : cs_newest_stick());
    cs_from_utc(t, cs_off_at(t), &g_cset);
    g_cset_edit = false;
    s->mode = AZ_CLOCK_SET;
    s->pane = 1;
    s->menu_sel = 0;
}
/* back to the Clock screen, on its Set Time Manually row */
static void clock_set_close(azmod_state *s) {
    int ovr[OVERLAYS_MAX], n = overlays_rows(AZF_CLOCK, ovr);
    s->mode = AZ_CLOCK;
    s->menu_sel = 0;
    for (int r = 0; r < n; r++)
        if (ovr[r] == C_SET_ROW)
            s->menu_sel = r;
    g_cset_edit = false;
}
static void clock_set_apply(azmod_state *s) {
    int64_t t;
    if (g_cset_zone < 0 || cs_to_utc(g_cset_zone ? &g_cset_z : NULL, &g_cset, &t) != 0 || t < CS_T_MIN)
        return; /* the zone cannot say what UTC time this is: nothing is set */
    __atomic_store_n(&g_azmod_clock_set_req, t, __ATOMIC_RELEASE);
    clock_set_close(s);
}
static void cs_value_text(char *out, size_t n, int k) {
    static const char *const MON[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int v = g_cset.v[k];
    if (k == CS_MONTH)
        snprintf(out, n, "%s", MON[(v - 1) % 12]);
    else if (k == CS_HOUR && az_get(AZF_CLOCK_24H) != 1)
        snprintf(out, n, "%d %s", v % 12 ? v % 12 : 12, v < 12 ? "AM" : "PM");
    else
        snprintf(out, n, k == CS_MINUTE || k == CS_HOUR ? "%02d" : "%d", v);
}
/* A value row's squares: minus, the value between them, plus (where a checkbox sits) */
static void cs_boxes(const azmod_fb *fb, int row, int *mx, int *px, int *by, int *sz, int *vx, int *vw) {
    int x, y, w, h;
    colors_row_rect(fb, row, &x, &y, &w, &h);
    *sz = g_full ? fb->h * 28 / 800 : h * 3 / 4;
    *px = x + w - *sz - (g_full ? fb->w / 40 : h / 3);
    *vw = g_full ? fb->w * 120 / 1280 : h * 3;
    *vx = *px - fb->w / 120 - *vw;
    *mx = *vx - fb->w / 120 - *sz;
    *by = y + (h - *sz) / 2;
}
static void cs_sign(const azmod_fb *fb, int x, int y, int sz, bool plus, uint32_t c) {
    int t = sz / 12 < 2 ? 2 : sz / 12, l = sz * 5 / 9;
    draw_rect_outline(fb, x, y, sz, sz, 2, C_CHECK_BOX);
    fill_rect(fb, x + (sz - l) / 2, y + (sz - t) / 2, l, t, c);
    if (plus)
        fill_rect(fb, x + (sz - t) / 2, y + (sz - l) / 2, t, l, c);
}
static void compose_clock_set(azmod_state *s, const azmod_fb *fb, int rowh, int scale) {
    draw_menu_header(fb, rowh, scale, "Set Time Manually");
    int px = menu_text_px(fb);
    for (int r = 0; r <= CS_SET_ROWN; r++) {
        bool dim = r == CS_SET_ROWN && g_cset_zone < 0;
        draw_colors_row(s, fb, r, rowh, CS_LABEL[r], scale, dim ? C_TEXT_DIM : C_TEXT);
        bool hi = s->menu_sel == r || (s->pressed_item == r && s->pressed_ticks > 0);
        uint32_t ink = hi && !g_full ? C_HI_TEXT : C_TEXT;
        char v[32];
        int mx, pxx, by, sz, vx, vw;
        cs_boxes(fb, r, &mx, &pxx, &by, &sz, &vx, &vw);
        if (r == CS_SET_ROWN) { /* the zone the time is in, where the squares sit */
            snprintf(v, sizeof v, "%s", g_cset_zone < 0 ? "Zone unavailable" : C_TZ[clock_tz_get()].name);
            int tw = azmod_ttf_menu_width ? azmod_ttf_menu_width(v, (int) strlen(v), px, 2) : 0;
            int cy = by + (sz - 7 * scale) / 2;
            if (tw <= 0 || !azmod_ttf_menu || !azmod_ttf_menu(fb, pxx + sz - tw, cy, 0, 7 * scale, v, px, 0, 2, C_TEXT_DIM))
                draw_text(fb, pxx + sz - (int) strlen(v) * 6 * scale, cy, v, scale, C_TEXT_DIM);
            continue;
        }
        cs_sign(fb, mx, by, sz, false, ink);
        cs_sign(fb, pxx, by, sz, true, ink);
        if (g_cset_edit && s->menu_sel == r) /* editing: the knob steps this value */
            draw_rect_outline(fb, vx, by, vw, sz, 2, C_HI);
        cs_value_text(v, sizeof v, r);
        int tw = azmod_ttf_menu_width ? azmod_ttf_menu_width(v, (int) strlen(v), px, 2) : 0, cy = by + (sz - 7 * scale) / 2;
        if (tw <= 0 || !azmod_ttf_menu || !azmod_ttf_menu(fb, vx + (vw - tw) / 2, cy, 0, 7 * scale, v, px, 0, 2, ink))
            draw_text(fb, vx + (vw - (int) strlen(v) * 6 * scale) / 2, cy, v, scale, ink);
    }
}
static bool input_clock_set(azmod_state *s, const azmod_fb *fb, int touch_x, int touch_y, bool touched,
                            int enc_delta, bool enc_click) {
    int n = CS_SET_ROWN + 1;
    if (s->menu_sel < 0 || s->menu_sel >= n)
        s->menu_sel = 0;
    if (enc_delta) {
        if (g_cset_edit && s->menu_sel < CS_SET_ROWN)
            cs_step(&g_cset, s->menu_sel, enc_delta);
        else
            s->menu_sel = (s->menu_sel + (enc_delta > 0 ? 1 : -1) + n) % n;
    }
    if (touched) {
        for (int r = 0; r < n; r++) {
            int x, y, w, h, mx, pxx, by, sz, vx, vw, pad = fb->h * 8 / 800;
            colors_row_rect(fb, r, &x, &y, &w, &h);
            if (!in_rect(touch_x, touch_y, x, y, w, h))
                continue;
            cs_boxes(fb, r, &mx, &pxx, &by, &sz, &vx, &vw);
            g_cset_edit = false;
            s->menu_sel = r;
            if (r == CS_SET_ROWN)
                clock_set_apply(s);
            else if (in_rect(touch_x, touch_y, mx - pad, y, sz + 2 * pad, h))
                cs_step(&g_cset, r, -1);
            else if (in_rect(touch_x, touch_y, pxx - pad, y, sz + 2 * pad, h))
                cs_step(&g_cset, r, 1);
            s->pressed_item = r;
            s->pressed_ticks = AZMOD_FLASH_TICKS;
            return true;
        }
    }
    if (enc_click) {
        if (s->menu_sel == CS_SET_ROWN)
            clock_set_apply(s);
        else
            g_cset_edit = !g_cset_edit;
    }
    return true; /* consume everything while the screen is up */
}

static const char *overlays_label(int it) {
    if (az_screen(it) >= 0) /* the switch at the top: the row that opened the screen */
        return it == AZF_CLOCK ? "Show Clock" : "Show Overlays";
    return it == C_TZ_ROW ? "Time Zone" : it == C_SET_ROW ? "Set Time Manually"
                                                          : az_label(it);
}
/* The value a row shows instead of a checkbox (a choice, the time zone, the time), or NULL. */
static const char *overlays_value(const azmod_state *s, int it) {
    if (it == C_TZ_ROW)
        return C_TZ[clock_tz_get()].name;
    if (it == C_SET_ROW) /* the time as the readout shows it, or that it is not set */
        return s->clock_text[0] ? s->clock_text : "Not set";
    if (az_kind(it) == AZ_ROW_CHOICE)
        return az_choice_label(it, az_get(it));
    return NULL;
}

#define FPS_X_FRAC 0.89060f /* 1140 @ 1280 */
#define FPS_Y_FRAC 0.07000f /*   56 @ 800  */
#define FPS_W_FRAC 0.10310f /*  132 */
#define FPS_H_FRAC 0.03250f /*   26 */
void azmod_fps_rect(const azmod_fb *fb, int *x, int *y, int *w, int *h) {
    *x = (int) (FPS_X_FRAC * fb->w);
    *y = (int) (FPS_Y_FRAC * fb->h);
    *w = (int) (FPS_W_FRAC * fb->w);
    *h = (int) (FPS_H_FRAC * fb->h);
}
/* On-screen FPS = WaveformView paints per second (the waveform is the only
 * animating surface; when it is not up the counter reads 0 by design). The JUCE
 * repaint-timer rate is a pipeline diagnostic, not an FPS (it runs ~2x paint),
 * so it is not shown here; it is in /tmp/azmod-rates as repaintTimer. */
static void draw_readout(const azmod_fb *fb, int x, int y, int w, int h, const char *t,
                         uint32_t col) {
    if (!azmod_ttf_center || !azmod_ttf_center(fb, x, y, w, h, t, (h * 3) / 5, 0, 0, col)) {
        int sc = h / 10;
        if (sc < 1)
            sc = 1;
        int tw = (int) strlen(t) * 6 * sc;
        draw_text(fb, x + (w - tw) / 2, y + (h - 7 * sc) / 2, t, sc, col);
    }
}
static void draw_fps_text(const azmod_state *s, const azmod_fb *fb, int x, int y, int w, int h) {
    char t[32];
    snprintf(t, sizeof t, "%d FPS", s->fps_paint);
    draw_readout(fb, x, y, w, h, t,
                 (s->fps_paint >= 50) ? 0x0030C040u : C_TEXT_DIM); /* green from 50 FPS up */
}
/* CPU temperature, whole degrees. Green while cool, yellow from 70 C, red from
 * 95 C, still before the RK3399's own passive throttle at
 * 105 C (the deck's soc-thermal trips: 105/110 passive, 115 critical). "\xB0"
 * is the degree sign: the TTF path loads glyphs by Latin-1 byte. */
#define TEMP_WARN_C10 700
#define TEMP_HOT_C10 950
#define C_READOUT_OK 0x0030C040u /* the readouts' green, a shade apart from ui_draw.c's C_OK */
#define C_WARN C_AVAIL
#define C_BAD 0x00FF4040u
static uint32_t level_col(int v, int warn, int bad) {
    return v >= bad ? C_BAD : v >= warn ? C_WARN
                                        : C_READOUT_OK;
}
static void draw_temp_text(const azmod_state *s, const azmod_fb *fb, int x, int y, int w, int h) {
    char t[16];
    if (s->cpu_temp_c10 < 0) {
        draw_readout(fb, x, y, w, h,
                     "--\xB0"
                     "C",
                     C_TEXT_DIM);
        return;
    }
    snprintf(t, sizeof t,
             "%d\xB0"
             "C",
             (s->cpu_temp_c10 + 5) / 10);
    draw_readout(fb, x, y, w, h, t, level_col(s->cpu_temp_c10, TEMP_WARN_C10, TEMP_HOT_C10));
}
static void draw_pct_text(const azmod_fb *fb, int x, int y, int w, int h, const char *tag, int pct,
                          int warn, int bad) {
    char t[16];
    if (pct < 0) {
        snprintf(t, sizeof t, "%s --%%", tag);
        draw_readout(fb, x, y, w, h, t, C_TEXT_DIM);
        return;
    }
    snprintf(t, sizeof t, "%s %d%%", tag, pct);
    draw_readout(fb, x, y, w, h, t, level_col(pct, warn, bad));
}
/* The fastest core's clock. The deck runs the performance governor (1.8 GHz
 * big / 1.416 GHz little, pinned), so this only moves when the SoC throttles,
 * which is what makes it worth showing: yellow whenever that core is below its
 * own maximum. */
static void draw_clk_text(const azmod_state *s, const azmod_fb *fb, int x, int y, int w, int h) {
    char t[16];
    if (s->cpu_clk_mhz < 0) {
        draw_readout(fb, x, y, w, h, "-.- GHz", C_TEXT_DIM);
        return;
    }
    int d = (s->cpu_clk_mhz + 50) / 100; /* tenths of a GHz */
    snprintf(t, sizeof t, "%d.%d GHz", d / 10, d % 10);
    draw_readout(fb, x, y, w, h, t, s->cpu_clk_throttled ? C_WARN : C_READOUT_OK);
}
/* The Clock (mod_menu_overlay.c ov_poll_clock fills clock_text once a second; "--:--" until the
 * system clock is known to be synchronized, so it never shows a wrong time), in the place of
 * EP147's recording time readout: clock_native.c hides that label while EP147 is not recording
 * and publishes its screen rect, ink and ground. The text is drawn as EP147 draws the label: its
 * own face (TsukuGo-B, the menu face) at 13 px, left-aligned inside the label's 5 px border. */
uint64_t g_azmod_clockn_rect; /* menu.h: written by clock_native.c; __atomic only */
uint32_t g_azmod_clockn_ink, g_azmod_clockn_ground;
#define CLOCKN_PX 13
#define CLOCKN_BORDER 5
#define C_CT_RED 0x00FF0000u /* EP147's red (its "Not Loaded." text, measured on the stock theme) */
static struct ct_timer g_ct;
static long ct_now_ms(void);
bool azmod_clockn_on(const azmod_state *s) {
    return azmod_stat_on(s, AZF_CLOCK) && __atomic_load_n(&g_azmod_clockn_rect, __ATOMIC_ACQUIRE) != 0;
}
/* The width the widest time of the current format takes, the border on both sides included: its
 * widest digit in every place, two hour digits, AM or PM, " UTC" when the zone is UTC (as
 * mod_menu_overlay.c ov_poll_clock writes it). It depends on the format only, so the window keeps
 * one size while the time runs. 0 = no font. Overlay thread only (its one caller). */
static int clockn_text_w(void) {
    static int key = -1, cw;
    int h24 = az_get(AZF_CLOCK_24H) == 1, sec = az_get(AZF_CLOCK_SECONDS) == 1, utc = clock_tz_get() == 0;
    int k = h24 | sec << 1 | utc << 2;
    if (k == key)
        return cw;
    cw = 0;
    if (azmod_ttf_menu_width) {
        char d = '0', t[24];
        int best = 0;
        for (char c = '0'; c <= '9'; c++) {
            int dw = azmod_ttf_menu_width(&c, 1, CLOCKN_PX, 0);
            if (dw > best)
                best = dw, d = c;
        }
        for (int pm = 0; pm < (h24 ? 1 : 2); pm++) {
            int n = snprintf(t, sizeof t, "%c%c:%c%c", d, d, d, d);
            if (sec)
                n += snprintf(t + n, sizeof t - (size_t) n, ":%c%c", d, d);
            snprintf(t + n, sizeof t - (size_t) n, "%s%s", h24 ? "" : pm ? " PM"
                                                                         : " AM",
                     utc ? " UTC" : "");
            int tw = azmod_ttf_menu_width(t, (int) strlen(t), CLOCKN_PX, 0);
            if (tw > 0 && tw + 2 * CLOCKN_BORDER > cw)
                cw = tw + 2 * CLOCKN_BORDER;
        }
        /* the Timer's countdown in the same place: its longest text, h:mm:ss */
        snprintf(t, sizeof t, "%c:%c%c:%c%c", d, d, d, d, d);
        int tw = azmod_ttf_menu_width(t, (int) strlen(t), CLOCKN_PX, 0);
        if (tw > 0 && tw + 2 * CLOCKN_BORDER > cw)
            cw = tw + 2 * CLOCKN_BORDER;
    }
    key = k;
    return cw;
}
/* EP147's label rect, widened to the right (never past the screen edge) when the time needs more
 * room than the label's 68 px: seconds with AM/PM, or the UTC suffix. */
void azmod_clockn_rect(const azmod_fb *fb, int *x, int *y, int *w, int *h) {
    if (!cn_rect_unpack(__atomic_load_n(&g_azmod_clockn_rect, __ATOMIC_ACQUIRE), x, y, w, h)) {
        *x = *y = -16, *w = *h = 1;
        return;
    }
    int need = clockn_text_w();
    if (need > *w)
        *w = need < fb->w - *x ? need : fb->w - *x > *w ? fb->w - *x
                                                        : *w;
}
void azmod_draw_clockn(const azmod_state *s, const azmod_fb *fb) {
    int x, y, w, h;
    azmod_clockn_rect(fb, &x, &y, &w, &h);
    uint32_t ink = __atomic_load_n(&g_azmod_clockn_ink, __ATOMIC_RELAXED) & 0xffffffu,
             ground = __atomic_load_n(&g_azmod_clockn_ground, __ATOMIC_RELAXED) & 0xffffffu;
    fill_rect(fb, x, y, w, h, ground);
    const char *t = s->clock_text[0] ? s->clock_text : "--:--";
    char cd[16];
    struct ct_view v = ct_view(&g_ct, ct_now_ms());
    if (v.show != CT_SHOW_CLOCK) { /* the Timer's countdown replaces the time of day */
        ct_format(v.secs, cd, sizeof cd);
        t = cd;
        if (v.red)
            ink = C_CT_RED;
    }
    if (!azmod_ttf_menu || !azmod_ttf_menu(fb, x + CLOCKN_BORDER, y, 0, h, t, CLOCKN_PX, 0, 0, ink))
        draw_text(fb, x + CLOCKN_BORDER, y + (h - 7) / 2, t, 1, ink);
}

/* The Clock's countdown Timer (clock_timer.h). A tap on the Clock opens a small popup under it
 * (add 1, 5, 10 or 30 minutes, Start, Reset, Close; over a running countdown the adds extend it
 * and Stop ends it). While it runs, the countdown is drawn in the Clock's place, in its face and
 * size; the last minute flashes EP147's red, and at zero 0:00 flashes for CT_DONE_MS before the
 * time of day comes back. Kept in CLOCK_MONOTONIC, so setting the clock or the zone never moves
 * it; it lives in memory only (an EP147 restart ends it). Overlay thread only: the taps arrive in
 * its event loop and it draws. */
static struct ct_popup g_ctp;
static long ct_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
int azmod_clockn_key(void) {
    struct ct_view v = ct_view(&g_ct, ct_now_ms());
    return v.show == CT_SHOW_CLOCK ? -1 : v.secs * 4 + (int) v.show * 2 + v.red;
}
void azmod_clockn_tap(const azmod_state *s, const azmod_fb *fb) {
    (void) fb;
    if (g_ctp.open)
        g_ctp.open = 0; /* a second tap on the Clock closes it, as Close does */
    else if (azmod_clockn_on(s))
        ct_popup_open(&g_ctp, &g_ct, ct_now_ms());
}
/* the popup's layout under the Clock, 0 when it is not shown (closed, the Clock gone, no room) */
static int ct_geom_now(const azmod_fb *fb, struct ct_geom *g) {
    int x, y, w, h;
    if (!cn_rect_unpack(__atomic_load_n(&g_azmod_clockn_rect, __ATOMIC_ACQUIRE), &x, &y, &w, &h))
        return 0;
    azmod_clockn_rect(fb, &x, &y, &w, &h);
    return ct_layout(x, y, w, h, fb->w, fb->h, g_ctp.running, g);
}
int azmod_ctpop_rect(const azmod_state *s, const azmod_fb *fb, int *x, int *y, int *w, int *h) {
    struct ct_geom g;
    if (g_ctp.open && !azmod_clockn_on(s))
        g_ctp.open = 0; /* the Clock went (recording, a banner, switched off): so does its popup */
    ct_popup_sync(&g_ctp, &g_ct, ct_now_ms());
    if (!g_ctp.open || !ct_geom_now(fb, &g))
        return 0;
    *x = g.box.x;
    *y = g.box.y;
    *w = g.box.w;
    *h = g.box.h;
    return 1;
}
void azmod_ctpop_tap(const azmod_fb *fb, int x, int y) {
    struct ct_geom g;
    if (g_ctp.open && ct_geom_now(fb, &g))
        ct_popup_press(&g_ctp, &g_ct, ct_hit(&g, &g_ctp, x, y), ct_now_ms());
}
void azmod_draw_ctpop(const azmod_fb *fb) {
    struct ct_geom g;
    if (!g_ctp.open || !ct_geom_now(fb, &g))
        return;
    long now = ct_now_ms();
    /* The Mod Menu's look: its pane gray, a header band with a divider under the title, and its
     * square choice buttons (an orange fill for the one action that is lit). */
    int band = g.value.y - g.box.y; /* the header band: the box's top padding and the title row */
    fill_rect(fb, g.box.x, g.box.y, g.box.w, g.box.h, C_MENU_SCRIM);
    fill_rect(fb, g.box.x, g.box.y, g.box.w, band, C_HEADER_BG);
    fill_rect(fb, g.box.x, g.box.y + band - 2, g.box.w, 2, C_DIVIDER);
    draw_rect_outline(fb, g.box.x, g.box.y, g.box.w, g.box.h, 2, C_CHECK_BOX);
    if (!azmod_ttf_menu ||
        !azmod_ttf_menu(fb, g.title.x, g.box.y, 0, band, "Timer", g.px_title, 0, 2, C_TEXT))
        draw_text(fb, g.title.x, g.box.y + (band - 14) / 2, "Timer", 2, C_TEXT);
    char t[16];
    ct_format(ct_popup_secs(&g_ctp, &g_ct, now), t, sizeof t);
    if (!azmod_ttf_menu || !azmod_ttf_menu(fb, g.value.x, g.value.y, g.value.w, g.value.h, t, g.px_value, 0, 2, C_TEXT))
        draw_text_centered(fb, g.value.x, g.value.y + (g.value.h - 28) / 2, g.value.w, t, 4, C_TEXT);
    for (int b = 0; b < CT_B_N; b++) {
        if (!ct_button_shown(&g_ctp, b))
            continue;
        const struct ct_rect *r = &g.btn[b];
        int on = ct_button_enabled(&g_ctp, b), primary = b == CT_B_START;
        int lit = primary && on;
        uint32_t ink = lit ? C_HI_TEXT : on ? C_TEXT
                                            : C_STEM_OFF_FG;
        if (lit)
            fill_rect(fb, r->x, r->y, r->w, r->h, C_HI);
        else
            draw_rect_outline(fb, r->x, r->y, r->w, r->h, 2, on ? C_CHECK_BOX : C_STEM_OFF_FG);
        const char *l = ct_button_label(&g_ctp, b);
        if (!azmod_ttf_menu || !azmod_ttf_menu(fb, r->x, r->y, r->w, r->h, l, g.px_btn, 0, 2, ink))
            draw_text_centered(fb, r->x, r->y + (r->h - 14) / 2, r->w, l, 2, ink);
    }
}

/* The readouts, left to right, and each one's slot width (fraction of 1280),
 * packed against the FPS slot's right edge (x 1272). Only the ones switched on
 * take room. All five need x 788-1272; y 50-84 carries no native ink from x 400
 * rightward in every deck layout (the playhead line is at x 395). */
static const struct {
    int mod;
    float wfrac;
} STATS[] = {
    {AZF_OVERLAY_CPU_CLOCK, 0.0781f}, /* 100: "1.8 GHz" */
    {AZF_OVERLAY_CPU_USAGE, 0.0813f}, /* 104: "CPU 37%" */
    {AZF_OVERLAY_RAM, 0.0813f},    /* 104: "RAM 45%" */
    {AZF_OVERLAY_CPU_TEMP, 0.0594f},   /*  76: "59 C" */
    {AZF_OVERLAY_FPS, 0.0781f},    /* 100: "60 FPS" */
};
#define STATS_N ((int) (sizeof STATS / sizeof STATS[0]))
bool azmod_stat_on(const azmod_state *s, int mod) {
    (void) s; /* the API takes the state; the switches are global */
    return az_on(mod) && az_active(mod);
}
/* A slot of the readout band (the Clock has its own place: EP147's readout) */
static bool stats_slot_on(const azmod_state *s, int mod) { return azmod_stat_on(s, mod); }
bool azmod_stats_any(const azmod_state *s) {
    for (int k = 0; k < STATS_N; k++)
        if (stats_slot_on(s, STATS[k].mod))
            return true;
    return false;
}
void azmod_stats_rect(const azmod_state *s, const azmod_fb *fb, int *x, int *y, int *w, int *h) {
    int fx, fy, fw, fh;
    azmod_fps_rect(fb, &fx, &fy, &fw, &fh);
    int total = 0;
    for (int k = 0; k < STATS_N; k++)
        if (stats_slot_on(s, STATS[k].mod))
            total += (int) (STATS[k].wfrac * fb->w);
    if (total < 1)
        total = 1;
    *x = fx + fw - total;
    *y = fy;
    *w = total;
    *h = fh;
}

void azmod_draw_stats(const azmod_state *s, const azmod_fb *fb) {
    int x, y, w, h;
    azmod_stats_rect(s, fb, &x, &y, &w, &h);
    fill_rect(fb, x, y, w, h, 0x00000000u);
    for (int k = 0; k < STATS_N; k++) {
        int m = STATS[k].mod;
        if (!stats_slot_on(s, m))
            continue;
        int sw = (int) (STATS[k].wfrac * fb->w);
        switch (m) {
        case AZF_OVERLAY_CPU_CLOCK:
            draw_clk_text(s, fb, x, y, sw, h);
            break;
        case AZF_OVERLAY_CPU_USAGE:
            draw_pct_text(fb, x, y, sw, h, "CPU", s->cpu_use_pct, 70, 90);
            break;
        case AZF_OVERLAY_RAM:
            draw_pct_text(fb, x, y, sw, h, "RAM", s->ram_use_pct, 75, 90);
            break;
        case AZF_OVERLAY_CPU_TEMP:
            draw_temp_text(s, fb, x, y, sw, h);
            break;
        case AZF_OVERLAY_FPS:
            draw_fps_text(s, fb, x, y, sw, h);
            break;
        }
        x += sw;
    }
}
