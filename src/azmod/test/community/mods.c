/* SPDX-License-Identifier: GPL-3.0-only */
/* Test mods for the host harness. Built once per -DMOD_<KIND>; see run.sh. */
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include "azmod-mod.h"
#ifndef MOD_ID
#define MOD_ID "good-any"
#endif
#ifndef FWTAG
#define FWTAG "any"
#endif
static const az_core_v1 *C;
#ifdef MOD_CRASHLIB
#include <stdio.h>
#endif
#ifdef MOD_CTORCRASH
__attribute__((constructor)) static void ctor_crash(void) {
    volatile int *z = (volatile int *) 0;
    *z = 5;
}   /* dies inside dlopen */
#endif
#ifdef MOD_CTORMARK   /* proves whether any of the mod's code ran: dlopen runs this before init() */
#include <fcntl.h>
#include <unistd.h>
__attribute__((constructor)) static void ctor_mark(void) {
    int fd = open("/tmp/cmtest/ran-" MOD_ID, O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) close(fd);
}
#endif
static int ticks;
#ifdef MOD_THREADS
#include <pthread.h>
#include <stdio.h>
static volatile int worker_done;
static void *worker(void *a) {
    (void) a;
    for (int i = 0; i < 20000; i++) {
        char v[32];
        snprintf(v, sizeof v, "w%d", i);
        C->cfg_set(MOD_ID, "wn", i);
        C->cfg_set_str(MOD_ID, "ws", v);
        C->data_dir(MOD_ID);
        C->assets_dir(MOD_ID);
        if ((i & 1023) == 0) C->log(MOD_ID, "worker %d", i);
    }
    __atomic_store_n(&worker_done, 1, __ATOMIC_RELEASE);
    return 0;
}
#endif
#ifdef MOD_KEEPDRAW
static az_draw *kept_d;   /* a mod bug: the paint context kept past on_draw */
#endif
static void draw(az_draw *d, az_rect a, void *u) {
    (void) u;
    (void) a;
    C->draw_header(d, "t");
#ifdef MOD_KEEPDRAW
    kept_d = d;
#endif
}
static int input(const az_event *e, void *u) {
    (void) e;
    (void) u;
    return 0;
}
static const az_screen_v1 SCR = {sizeof(az_screen_v1), draw, input, NULL, NULL};
#if defined(MOD_STREDGE) || defined(MOD_DECKS)
static int utf8_whole(const char *s) { /* 1 = well-formed UTF-8 to the end (no cut sequence) */
    for (const unsigned char *p = (const unsigned char *) s; *p;) {
        int n = *p < 0x80 ? 1 : (*p & 0xe0) == 0xc0 ? 2
                            : (*p & 0xf0) == 0xe0   ? 3
                            : (*p & 0xf8) == 0xf0   ? 4
                                                    : 0;
        if (!n) return 0;
        for (int i = 1; i < n; i++)
            if ((p[i] & 0xc0) != 0x80) return 0;
        p += n;
    }
    return 1;
}
#endif
#ifdef MOD_LOGRACE
#include <pthread.h>
#include <time.h>
/* a thread of the mod's own that only logs, for two seconds, while the core ticks: nothing it
 * does orders it against the core's tick, so a plain variable the two share is a data race */
static void *logger(void *a) {
    (void) a;
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        C->log(MOD_ID, "logger");
        clock_gettime(CLOCK_MONOTONIC, &t);
    } while (t.tv_sec - t0.tv_sec < 2);
    return NULL;
}
#endif
#ifdef MOD_LIVE
/* turned off and on while running: every call the core makes into it must fall between init()
 * and stop(). The harness reads these through dlsym. */
static int live_alive;
/* the harness's counters (it exports them; a mod exports nothing but its entry) */
extern int az_live_probe[4] __attribute__((weak));
static int live_dummy[4];
#define LP (az_live_probe ? az_live_probe : live_dummy)
#define az_test_inits (LP[0])
#define az_test_stops (LP[1])
#define az_test_overlap (LP[2])
#define az_test_filtered (LP[3])
static void live_check_in(void) {
    if (!__atomic_load_n(&live_alive, __ATOMIC_SEQ_CST)) __atomic_add_fetch(&az_test_overlap, 1, __ATOMIC_RELAXED);
}
static void live_check_out(void) {   /* still running at the end of the call: stop() did not run meanwhile */
    for (volatile int i = 0; i < 2000; i++) {}
    live_check_in();
}
static int live_filter(int bus, unsigned char *f, int len, void *user) {
    (void) bus;
    (void) f;
    (void) len;
    (void) user;
    live_check_in();
    __atomic_add_fetch(&az_test_filtered, 1, __ATOMIC_RELAXED);
    live_check_out();
    return AZ_FRAME_OK;
}
static void stop(void) {
    __atomic_store_n(&live_alive, 0, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&az_test_stops, 1, __ATOMIC_RELAXED);
#ifdef MOD_TAILSTOP
    C->cfg_set(MOD_ID, "stopped", 1); /* the last statement: a tail call at -O2 */
#endif
}
#define MOD_STOP stop
#endif
static int init(const az_core_v1 *core) {
    C = core;
#ifdef MOD_LIVE
    __atomic_add_fetch(&az_test_inits, 1, __ATOMIC_RELAXED);
    if (C->frame_filter(MOD_ID, 1u << AZ_BUS_ERP_LED, live_filter, NULL) != 0) return 7;
    __atomic_store_n(&live_alive, 1, __ATOMIC_SEQ_CST);
#endif
    C->log(MOD_ID, "init on %s", C->fw_tag());
#ifdef MOD_INITFAIL
    return 3;
#endif
#ifdef MOD_INITHANG
    for (;;) {
        volatile int spin = 0;
        (void) spin;
    }         /* freezes inside init(): no signal */
#endif
#ifdef MOD_INITCRASH
    {
        volatile int *z = (volatile int *) 0;
        *z = 3;
    }       /* dies inside init(), before blame is armed */
#endif
#ifdef MOD_CLAIM
    /* the harness puts 4 addresses in this mod's cfg (a<i>h / a<i>l = high / low 32 bits) */
    for (int i = 0; i < 4; i++) {
        char kh[8], kl[8], kr[8];
        kh[0] = 'a';
        kh[1] = (char) ('0' + i);
        kh[2] = 'h';
        kh[3] = 0;
        memcpy(kl, kh, 8);
        kl[2] = 'l';
        memcpy(kr, kh, 8);
        kr[0] = 'r';
        kr[2] = 0;
        uintptr_t a = ((uintptr_t) (uint32_t) C->cfg_get(MOD_ID, kh, 0) << 32) | (uint32_t) C->cfg_get(MOD_ID, kl, 0);
        void *orig;
        C->cfg_set(MOD_ID, kr, a ? C->claim_hook(MOD_ID, a, (void *) init, &orig) : 99);
    }
#endif
#ifdef MOD_FILES
    /* text settings + the USB folders; results go back through cfg for run.sh */
    if (AZ_HAS(C, cfg_set_str) && AZ_HAS(C, data_dir) && AZ_HAS(C, assets_dir)) {
        char buf[160];
        C->cfg_set_str(MOD_ID, "name", "Hello\nworld\x01!");      /* control bytes dropped */
        C->cfg_get_str(MOD_ID, "name", buf, sizeof buf, "none");
        C->cfg_set(MOD_ID, "str_ok", !strcmp(buf, "Helloworld!"));
        C->cfg_get_str(MOD_ID, "missing", buf, sizeof buf, "dflt");
        C->cfg_set(MOD_ID, "def_ok", !strcmp(buf, "dflt"));
        const char *d = C->data_dir(MOD_ID), *a = C->assets_dir(MOD_ID);
        C->cfg_set_str(MOD_ID, "data", d ? d : "NULL");
        C->cfg_set_str(MOD_ID, "assets", a ? a : "NULL");
        C->cfg_set(MOD_ID, "has_api", 1);
    }
#endif
#ifdef MOD_STREDGE
    /* text settings at their limits: results go back through cfg for run.sh */
    {
        char e2[256] = "", b[200];
        for (int i = 0; i < 100; i++) strcat(e2, "\xc3\xa9"); /* 200 bytes of 2-byte characters */
        C->cfg_set_str(MOD_ID, "long", e2);
        int n = C->cfg_get_str(MOD_ID, "long", b, sizeof b, "");
        C->cfg_set(MOD_ID, "long_len", n);
        C->cfg_set(MOD_ID, "long_whole", utf8_whole(b));
        C->cfg_set_str(MOD_ID, "short", "\xc3\xa9\xc3\xa9\xc3\xa9");
        n = C->cfg_get_str(MOD_ID, "short", b, 6, "");
        C->cfg_set(MOD_ID, "cut_len", n);
        C->cfg_set(MOD_ID, "cut_whole", utf8_whole(b));
        b[0] = 'X';
        C->cfg_set(MOD_ID, "n1", C->cfg_get_str(MOD_ID, "short", b, 1, "") == 0 && !b[0]);
        b[0] = 'X';
        C->cfg_set(MOD_ID, "n0", C->cfg_get_str(MOD_ID, "short", b, 0, "") == 0 && b[0] == 'X');
        C->cfg_set_str(MOD_ID, "a\rb", "cr");
        C->cfg_get_str(MOD_ID, "a\rb", b, sizeof b, "dflt");
        C->cfg_set(MOD_ID, "crkey_refused", !strcmp(b, "dflt"));
        C->cfg_set_str(MOD_ID, "k32-0123456789abcdef0123456789ab", "x"); /* 32 bytes: one too many */
        C->cfg_get_str(MOD_ID, "k32-0123456789abcdef0123456789ab", b, sizeof b, "dflt");
        C->cfg_set(MOD_ID, "longkey_refused", !strcmp(b, "dflt"));
        for (int i = 0; i < 16; i++) {
            char k[8] = {'s', (char) ('a' + i), 0};
            C->cfg_set_str(MOD_ID, k, "v");
        }
        C->cfg_get_str(MOD_ID, "sp", b, sizeof b, "dflt"); /* the 17th text key after long/short/a */
        C->cfg_set(MOD_ID, "over_dropped", !strcmp(b, "dflt"));
    }
#endif
#ifdef MOD_LOGRACE
    {
        pthread_t lt;
        pthread_create(&lt, 0, logger, 0);
        pthread_detach(lt);
    }
#endif
#ifdef MOD_THREADS
    {
        pthread_t t;
        pthread_create(&t, 0, worker, 0);
        pthread_detach(t);
    }
#endif
#ifdef MOD_IMPERSONATE
    /* acting as another mod: both must be refused (the core checks the caller's code range) */
    {
        void *orig;
        uintptr_t a = ((uintptr_t) (uint32_t) C->cfg_get(MOD_ID, "a0h", 0) << 32) | (uint32_t) C->cfg_get(MOD_ID, "a0l", 0);
        C->cfg_set(MOD_ID, "as_other_claim", C->claim_hook(MOD_IMPERSONATE, a, (void *) init, &orig));
        C->cfg_set(MOD_IMPERSONATE, "written_by_other", 1);
      /* and as a mod that is installed but never loaded (it has no code to call from) */
        C->cfg_set(MOD_ID, "other_cfg", C->cfg_get(MOD_IMPERSONATE, "planted", -1));
        C->log(MOD_IMPERSONATE, "IMPOSTER-LINE");
    }
#endif
    C->cfg_set(MOD_ID, "loaded", C->cfg_get(MOD_ID, "loaded", 0) + 1);
    C->register_screen(MOD_ID, &SCR);
    return 0;
}
#ifdef MOD_FILES
#include <stdio.h>
#include <stdlib.h>
static const char *kept;   /* a mod that (wrongly) keeps the data_dir pointer */
#endif
static void tick(void) {
    if (++ticks == 10) C->cfg_set(MOD_ID, "ticked", 1);
#ifdef MOD_KEEPDRAW
    if (kept_d) C->draw_header(kept_d, "stale");                    /* refused: the paint is over */
#endif
#ifdef MOD_HAMMER
    C->cfg_set(MOD_ID, "every_tick", ticks);                     /* a value that changes non-stop */
#endif
#ifdef MOD_LATE
    if (ticks == 5) {                         /* init() is over: both are init-only services */
        void *orig;
        uintptr_t a = ((uintptr_t) (uint32_t) C->cfg_get(MOD_ID, "a0h", 0) << 32) | (uint32_t) C->cfg_get(MOD_ID, "a0l", 0);
        C->cfg_set(MOD_ID, "late_claim", C->claim_hook(MOD_ID, a, (void *) init, &orig));
        C->cfg_set(MOD_ID, "late_screen", C->register_screen(MOD_ID, &SCR));
    }
#endif
#ifdef MOD_THREADS
    if (ticks % 3 == 0) C->cfg_set(MOD_ID, "tk", ticks);         /* the tick thread writes too */
    if (__atomic_load_n(&worker_done, __ATOMIC_ACQUIRE) == 1) {
        char b[32];
        C->cfg_get_str(MOD_ID, "ws", b, sizeof b, "");
        C->cfg_set(MOD_ID, "worker_ok", C->cfg_get(MOD_ID, "wn", -1) == 19999 && !strcmp(b, "w19999"));
        __atomic_store_n(&worker_done, 2, __ATOMIC_RELEASE);
    }
#endif
#ifdef MOD_FILES
    if (ticks == 20 && AZ_HAS(C, data_dir)) kept = C->data_dir(MOD_ID);
    if (ticks == 30 && getenv("CM_TEST_EJECT")) {                  /* simulate the eject */
        char away[300];
        snprintf(away, sizeof away, "%s.away", getenv("CM_TEST_EJECT"));
        rename(getenv("CM_TEST_EJECT"), away);
    }
    if (ticks == 31 && getenv("CM_TEST_EJECT") && AZ_HAS(C, data_dir)) {  /* asked again at once */
        const char *soon = C->data_dir(MOD_ID);
        C->cfg_set_str(MOD_ID, "soon", soon ? soon : "NULL");
    }
    if (ticks == 600 && AZ_HAS(C, data_dir)) {                     /* > the core's 2 s re-check */
        const char *now = C->data_dir(MOD_ID);
        C->cfg_set_str(MOD_ID, "after", now ? now : "NULL");
        C->cfg_set_str(MOD_ID, "kept", kept ? (kept[0] ? kept : "EMPTY") : "NULL");
    }
#endif
#ifdef MOD_DECKS
    if (ticks == 5) {                                              /* the read-only deck state */
        char t[64];
        C->cfg_set(MOD_ID, "loaded0", C->deck_loaded(0));
        C->cfg_set(MOD_ID, "loaded1", C->deck_loaded(1));
        C->cfg_set(MOD_ID, "title0_len", C->deck_title(0, t, sizeof t));
        C->cfg_set_str(MOD_ID, "title0", t[0] ? t : "NONE");
        C->cfg_set(MOD_ID, "title1_len", C->deck_title(1, t, sizeof t));
        int n6 = C->deck_title(0, t, 6); /* a title cut to a 6-byte buffer */
        C->cfg_set(MOD_ID, "title6_len", n6);
        C->cfg_set(MOD_ID, "title6_whole", utf8_whole(t) && (int) strlen(t) == n6);
        t[0] = 'X';
        C->cfg_set(MOD_ID, "title_n1", C->deck_title(0, t, 1) == 0 && !t[0]);
        C->cfg_set(MOD_ID, "title_bad", C->deck_title(-1, t, sizeof t) == 0 && C->deck_title(4, t, sizeof t) == 0);
    }
#endif
#ifdef MOD_CRASH
    if (ticks == 20) {
        volatile int *z = (volatile int *) 0;
        *z = 1;
    }   /* a mod bug: NULL write */
#endif
#ifdef MOD_CRASHLIB
    if (ticks == 20) {
        char *volatile bad = (char *) 16;              /* a bad pointer handed to libc: */
        snprintf(bad, 64, "%s %d", "deep inside libc", ticks);
    }   /* it crashes in vfprintf */
#endif
}
static int frame(int bus, unsigned char *f, int len) {
    (void) bus;
    (void) len;
#ifdef MOD_LIVE
    live_check_in();
    struct timespec ts = {0, 5000000}; /* a 5 ms call: a live stop that does not wait for it overlaps it */
    nanosleep(&ts, NULL);
    live_check_out();
#endif
    f[0] = 42;
#ifdef MOD_FRAMELOG
    C->log(MOD_ID, "logged from on_frame");                    /* queued, written by the next tick */
    C->cfg_set(MOD_ID, "from_frame", 1);                       /* refused: never block the bus */
#endif
    return AZ_FRAME_OK;
}
static const char *const TAGS[] = {FWTAG, NULL};
#ifndef MOD_ABI
#define MOD_ABI AZ_MOD_ABI
#endif
#ifndef MOD_SIZE
#define MOD_SIZE sizeof(az_mod_v1)
#endif
#ifdef MOD_NOINIT
#define MOD_INIT NULL
#else
#define MOD_INIT init
#endif
#ifndef MOD_STOP
#define MOD_STOP NULL
#endif
static const az_mod_v1 MOD = {MOD_ABI, MOD_SIZE, MOD_ID, TAGS, MOD_INIT, tick, frame, MOD_STOP};
#ifdef MOD_UNDEF
void az_test_symbol_nobody_defines(void); /* dlopen (RTLD_NOW) cannot resolve it */
#endif
#ifndef MOD_NOENTRY
const az_mod_v1 *az_mod_v1_entry(void) {
#ifdef MOD_UNDEF
    az_test_symbol_nobody_defines();
#endif
#ifdef MOD_NULLENTRY
    return NULL;
#endif
    return &MOD;
}
#endif
#ifdef MOD_EXTRA
__attribute__((visibility("default"))) int helper_left_visible(void) { return 1; }
#endif
#ifdef MOD_EXPORTREAD
__attribute__((visibility("default"))) ssize_t read(int fd, void *b, size_t n) {
    (void) fd;
    (void) b;
    (void) n;
    return -1;
}
#endif
