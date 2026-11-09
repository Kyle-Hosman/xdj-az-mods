/* SPDX-License-Identifier: GPL-3.0-only */
/* The community mods' frame and touch filters (guest/azmod/sdk_input.c), built natively against
 * stand-ins for the community host. Each case asks one question a mod author relies on:
 * registration only from init(), edits reach EP147, AZ_FRAME_EATEN stops the chain, raw and
 * observe-only buses hand out copies, a filter that keeps overrunning is switched off, and a
 * touch is offered once, when the finger lands, then belongs to whoever took it until it lifts.
 * run.sh also builds it from mutated copies of sdk_input.c; each must fail. */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include "check.h"

#include <sys/time.h>
#include "azmod-mod.h"

/* the input_event layout and codes sdk_input.c reads (linux/input.h) */
#ifdef __linux__
#include <linux/input.h>
#else
struct input_event {
    struct timeval time;
    unsigned short type, code;
    int value;
};
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_ABS 0x03
#define SYN_REPORT 0
#define BTN_TOUCH 0x14a
#define ABS_MT_SLOT 0x2f
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#define ABS_MT_TRACKING_ID 0x39
#endif

/* ---- the community host, as much of it as sdk_input.c uses ---- */
#define CM_MAX 8
#define AZCM_LOADED 1
typedef struct {
    char id[33];
    int state;
    int logs;
    char last[200];
    int inflight;
} cm_mod;
/* as community.c: a call into the mod from another thread only while it runs */
static int cm_enter(cm_mod *c) {
    if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) != AZCM_LOADED)
        return 0;
    __atomic_add_fetch(&c->inflight, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&c->state, __ATOMIC_SEQ_CST) == AZCM_LOADED)
        return 1;
    __atomic_sub_fetch(&c->inflight, 1, __ATOMIC_RELEASE);
    return 0;
}
static void cm_leave(cm_mod *c) { __atomic_sub_fetch(&c->inflight, 1, __ATOMIC_RELEASE); }
static cm_mod g_mods[2] = {{"mod-a", AZCM_LOADED}, {"mod-b", AZCM_LOADED}};
static __thread int g_cm_in_frame;
static int g_sdk_in_init;
#define CM_RA() ((uintptr_t) 0)
static cm_mod *cm_self(const char *id, uintptr_t ra, const char *what) {
    (void) ra, (void) what;
    for (int i = 0; i < 2; i++)
        if (!strcmp(g_mods[i].id, id))
            return &g_mods[i];
    return NULL;
}
static void cm_logf(cm_mod *c, int force, const char *fmt, ...) {
    (void) force;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->last, sizeof c->last, fmt, ap);
    va_end(ap);
    c->logs++;
}

#include SDK_INPUT_C

/* ---- filters under test ---- */
static int calls_a, calls_b, saw_len;
static int f_edit(int bus, unsigned char *f, int len, void *u) {
    (void) bus, (void) u;
    calls_a++;
    saw_len = len;
    f[0] = 0x42;
    return AZ_FRAME_OK;
}
static int f_eat(int bus, unsigned char *f, int len, void *u) {
    (void) bus, (void) f, (void) len, (void) u;
    calls_a++;
    return AZ_FRAME_EATEN;
}
static int f_count(int bus, unsigned char *f, int len, void *u) {
    (void) bus, (void) f, (void) len, (void) u;
    calls_b++;
    return AZ_FRAME_OK;
}
static int f_scribble(int bus, unsigned char *f, int len, void *u) {
    (void) bus, (void) u;
    calls_b++;
    memset(f, 0xEE, (size_t) len);
    return AZ_FRAME_EATEN;
}
static int f_slow(int bus, unsigned char *f, int len, void *u) {
    (void) bus, (void) f, (void) len, (void) u;
    calls_a++;
    struct timespec t = {0, 300000}; /* 300 us: over the 200 us budget */
    nanosleep(&t, NULL);
    return AZ_FRAME_OK;
}

static void reset(void) {
    memset(g_sdk_ff, 0, sizeof g_sdk_ff);
    g_sdk_ff_n = 0;
    g_sdk_ff_bus = 0;
    memset(g_sdk_tf, 0, sizeof g_sdk_tf);
    g_sdk_tf_n = 0;
    g_sdk_touch_owner = -1;
    g_sdk_touch_decided = g_sdk_touch_down = g_sdk_touch_slot = 0;
    calls_a = calls_b = saw_len = 0;
    g_mods[0].logs = g_mods[1].logs = 0;
}

/* ---- touch helpers ---- */
static int t_takes, t_owner_reports, t_offers, t_last_down = -1, t_take_from_x = 1000;
static int f_touch(const az_touch *t, void *u) {
    (void) u;
    t_offers++;
    t_last_down = t->down;
    if (t->x >= t_take_from_x) {
        t_takes++;
        return AZ_TOUCH_TAKE;
    }
    return AZ_TOUCH_PASS;
}
static int f_touch_owner(const az_touch *t, void *u) { /* counts every call while it owns */
    (void) u;
    t_owner_reports++;
    t_last_down = t->down;
    return t->x >= t_take_from_x ? AZ_TOUCH_TAKE : AZ_TOUCH_PASS;
}
static int f_touch_slow(const az_touch *t, void *u) { /* takes everything, too slowly */
    (void) t, (void) u;
    t_owner_reports++;
    struct timespec ts = {0, 300000};
    nanosleep(&ts, NULL);
    return AZ_TOUCH_TAKE;
}
static int touch_frame(int x, int down, struct input_event ev[4]) {
    memset(ev, 0, sizeof(struct input_event) * 4);
    ev[0].type = EV_ABS, ev[0].code = ABS_MT_POSITION_X, ev[0].value = x;
    ev[1].type = EV_ABS, ev[1].code = ABS_MT_POSITION_Y, ev[1].value = 100;
    ev[2].type = EV_ABS, ev[2].code = ABS_MT_TRACKING_ID, ev[2].value = down ? 7 : -1;
    ev[3].type = EV_SYN, ev[3].code = SYN_REPORT;
    sdk_touch_read(ev, (ssize_t) sizeof(struct input_event) * 4);
    return ev[0].type == EV_SYN; /* 1 = blanked for EP147 */
}

int main(void) {
    ut_suite = "sdk input";
    sdk_timing_init();
    unsigned char f[128], keep[128];
    unsigned erp = 1u << AZ_BUS_ERP_IN, deck = 1u << AZ_BUS_DECK_IN;

    /* registration */
    reset();
    CHECK("frame_filter outside init() is refused", cm_frame_filter("mod-a", erp, f_edit, NULL) == -1);
    g_sdk_in_init = 1;
    CHECK("frame_filter from init() is accepted", cm_frame_filter("mod-a", erp, f_edit, NULL) == 0);
    CHECK("an unknown bus in the mask is refused", cm_frame_filter("mod-a", 1u << 9, f_edit, NULL) == -1);
    CHECK("a mask with no bus is refused", cm_frame_filter("mod-a", AZ_FILTER_RAW, f_edit, NULL) == -1);
    CHECK("another mod's id is refused", cm_frame_filter("no-such", erp, f_edit, NULL) == -1);
    int over = 0;
    for (int i = 0; i < SDK_FILTER_MAX + 2; i++)
        if (cm_frame_filter("mod-b", erp, f_count, NULL) == -1)
            over++;
    CHECK("registrations past the table size are refused", over == 3);
    g_sdk_in_init = 0;

    /* editing, eating, buses */
    reset();
    g_sdk_in_init = 1;
    cm_frame_filter("mod-a", erp, f_edit, NULL);
    cm_frame_filter("mod-b", erp, f_count, NULL);
    g_sdk_in_init = 0;
    memset(f, 0, sizeof f);
    CHECK("no filter on a bus: nothing runs", sdk_filter_run(AZ_BUS_MIXER_IN, f, 98) == 0 && calls_a == 0);
    CHECK("filters run in load order and the edit stays in the frame",
          sdk_filter_run(AZ_BUS_ERP_IN, f, 30) == 1 && f[0] == 0x42 && calls_a == 1 && calls_b == 1);
    CHECK("the filter sees the frame's length", saw_len == 30);
    CHECK("the bus flag is cleared after the chain", g_cm_in_frame == 0);
    reset();
    g_sdk_in_init = 1;
    cm_frame_filter("mod-a", erp, f_eat, NULL);
    cm_frame_filter("mod-b", erp, f_count, NULL);
    g_sdk_in_init = 0;
    CHECK("AZ_FRAME_EATEN stops the chain", sdk_filter_run(AZ_BUS_ERP_IN, f, 30) == 2 && calls_b == 0);
    g_mods[0].state = 0;
    CHECK("a mod that is not loaded is skipped", sdk_filter_run(AZ_BUS_ERP_IN, f, 30) == 1 && calls_b == 1);
    g_mods[0].state = AZCM_LOADED;

    /* copies: the raw stage and the observe-only buses */
    reset();
    g_sdk_in_init = 1;
    cm_frame_filter("mod-b", erp | AZ_FILTER_RAW, f_scribble, NULL);
    cm_frame_filter("mod-b", deck, f_scribble, NULL);
    g_sdk_in_init = 0;
    for (int i = 0; i < 128; i++)
        f[i] = keep[i] = (unsigned char) i;
    sdk_filter_raw(AZ_BUS_ERP_IN, f, 30);
    CHECK("a raw filter runs and cannot change the frame", calls_b == 1 && !memcmp(f, keep, 30));
    CHECK("a raw filter does not run in the editing chain", sdk_filter_run(AZ_BUS_ERP_IN, f, 30) == 0);
    sdk_filter_view(AZ_BUS_DECK_IN, f, 40);
    CHECK("an observe-only bus hands every filter a copy", calls_b == 2 && !memcmp(f, keep, 40));

    /* a filter that keeps overrunning is switched off */
    reset();
    g_sdk_in_init = 1;
    cm_frame_filter("mod-a", erp, f_slow, NULL);
    cm_frame_filter("mod-b", erp, f_count, NULL);
    g_sdk_in_init = 0;
    for (int i = 0; i < SDK_FILTER_STRIKES + 3; i++)
        sdk_filter_run(AZ_BUS_ERP_IN, f, 30);
    CHECK("a slow filter is switched off after its strikes", calls_a == SDK_FILTER_STRIKES);
    CHECK("the other mods' filters keep running", calls_b == SDK_FILTER_STRIKES + 3);
    CHECK("the switch-off is logged to the slow mod", g_mods[0].logs == 1 && strstr(g_mods[0].last, "switched off"));

    /* touch */
    struct input_event ev[4];
    reset();
    g_sdk_in_init = 1;
    CHECK("touch_filter from init() is accepted", cm_touch_filter("mod-a", f_touch, NULL) == 0);
    g_sdk_in_init = 0;
    t_offers = t_takes = 0;
    int blank = touch_frame(10, 1, ev);
    CHECK("a touch that is passed reaches EP147 untouched", !blank && t_offers == 1);
    touch_frame(1500, 1, ev); /* the same finger slides into the taking area */
    CHECK("a passed touch is not offered again while the finger stays down", t_offers == 1);
    touch_frame(1500, 0, ev); /* lift */
    blank = touch_frame(1500, 1, ev);
    CHECK("after the lift, the next touch is offered and taken", t_offers == 2 && t_takes == 1 && blank);
    reset();
    g_sdk_in_init = 1;
    cm_touch_filter("mod-a", f_touch_owner, NULL);
    g_sdk_in_init = 0;
    t_owner_reports = 0;
    touch_frame(1500, 1, ev);
    blank = touch_frame(20, 1, ev); /* the owner keeps it even where it would pass */
    CHECK("a taken touch stays the owner's and is blanked to the end", blank && t_owner_reports == 2);
    blank = touch_frame(20, 0, ev);
    CHECK("the owner sees the lift, and EP147 does not", blank && t_owner_reports == 3 && t_last_down == 0);
    blank = touch_frame(20, 1, ev);
    CHECK("the next touch, passed, goes to EP147", !blank);
    touch_frame(20, 0, ev);

    /* an owner switched off, or whose mod stopped, mid-touch: not called again, and EP147 still
     * never gets the rest of that touch */
    reset();
    g_sdk_in_init = 1;
    cm_touch_filter("mod-a", f_touch_owner, NULL);
    g_sdk_in_init = 0;
    t_owner_reports = 0;
    touch_frame(1500, 1, ev); /* taken */
    __atomic_store_n(&g_sdk_tf[0].off, 1, __ATOMIC_RELEASE);
    blank = touch_frame(1500, 1, ev);
    CHECK("a switched-off owner is not called again mid-touch", t_owner_reports == 1);
    CHECK("its touch stays blanked for EP147 until it lifts", blank && touch_frame(1500, 0, ev));
    reset();
    g_sdk_in_init = 1;
    cm_touch_filter("mod-a", f_touch_owner, NULL);
    g_sdk_in_init = 0;
    t_owner_reports = 0;
    touch_frame(1500, 1, ev);
    g_mods[0].state = 0; /* the mod is no longer loaded */
    touch_frame(1500, 1, ev);
    CHECK("an owner whose mod stopped is not called again", t_owner_reports == 1);
    touch_frame(1500, 0, ev);
    g_mods[0].state = AZCM_LOADED;

    /* a slow owner is timed and switched off like any filter */
    reset();
    g_sdk_in_init = 1;
    cm_touch_filter("mod-a", f_touch_slow, NULL);
    g_sdk_in_init = 0;
    t_owner_reports = 0;
    for (int i = 0; i < SDK_FILTER_STRIKES + 4; i++)
        touch_frame(1500, 1, ev);
    CHECK("a slow owner is switched off after its strikes",
          __atomic_load_n(&g_sdk_tf[0].off, __ATOMIC_ACQUIRE) && t_owner_reports == SDK_FILTER_STRIKES);
    touch_frame(1500, 0, ev);
    return ut_summary();
}
