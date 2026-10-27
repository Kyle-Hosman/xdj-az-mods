/* SPDX-License-Identifier: GPL-3.0-only */
/* The Clock's countdown Timer (guest/azmod/clock_timer.h): what the Clock's place shows at each
 * moment (the countdown rounded up, the last minute flashing red at 1 Hz, 0:00 flashing for
 * CT_DONE_MS, then the clock), the popup's buttons (add, Start, Reset, Stop, Close), the text
 * format, the layout and hit test, and one writer and one reader thread on the deadline (run
 * under ThreadSanitizer). CT_HDR = the header under test. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include CT_HDR

static const char *fmt(int s) {
    static char b[16];
    ct_format(s, b, sizeof b);
    return b;
}

/* the reader thread: views the deadline the writer keeps changing */
static struct ct_timer g_rt;
static int g_stop;
static long g_seen;
static void *reader(void *arg) {
    (void) arg;
    long n = 0;
    while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
        struct ct_view v = ct_view(&g_rt, 1000);
        n += v.show != CT_SHOW_CLOCK;
    }
    __atomic_store_n(&g_seen, n, __ATOMIC_RELEASE);
    return NULL;
}

int main(int argc, char **argv) {
    ut_suite = "clock timer";
    long iters = argc > 1 ? atol(argv[1]) : 20000;

    CHECK("m:ss under an hour", !strcmp(fmt(0), "0:00") && !strcmp(fmt(65), "1:05") &&
                                    !strcmp(fmt(3599), "59:59"));
    CHECK("h:mm:ss from one hour", !strcmp(fmt(3600), "1:00:00") && !strcmp(fmt(5 * 3600 + 61), "5:01:01"));
    CHECK("the text is clamped to 9:59:59", !strcmp(fmt(CT_MAX_S + 500), "9:59:59") && !strcmp(fmt(-3), "0:00"));

    struct ct_timer t = {0};
    struct ct_view v = ct_view(&t, 5000);
    CHECK("no timer shows the clock", v.show == CT_SHOW_CLOCK && !v.red);

    int64_t now = 100000;
    ct_start(&t, now, 5 * 60);
    v = ct_view(&t, now);
    CHECK("a started timer shows its full duration", v.show == CT_SHOW_COUNT && v.secs == 300 && !v.red);
    v = ct_view(&t, now + 1);
    CHECK("the shown seconds round up", v.secs == 300);
    v = ct_view(&t, now + 1000);
    CHECK("one second later, one second less", v.secs == 299);
    v = ct_view(&t, now + 239999);
    CHECK("61 s left is not red", v.secs == 61 && !v.red);
    v = ct_view(&t, now + 240000);
    CHECK("60 s left: the first half of the second is red", v.secs == 60 && v.red);
    v = ct_view(&t, now + 240499);
    CHECK("still red 499 ms into the second", v.secs == 60 && v.red);
    v = ct_view(&t, now + 240500);
    CHECK("the second half of the second is not red", v.secs == 60 && !v.red);
    v = ct_view(&t, now + 299999);
    CHECK("1 ms left shows 0:01, not red", v.show == CT_SHOW_COUNT && v.secs == 1 && !v.red);
    {
        int on = 0, flips = 0, last = -1;
        for (int64_t ms = now + 240000; ms < now + 300000; ms += 10) {
            int r = ct_view(&t, ms).red;
            on += r;
            flips += last >= 0 && r != last;
            last = r;
        }
        CHECK("the last minute is red half the time", on == 3000);
        CHECK("and flips twice a second (1 Hz)", flips == 119);
    }
    v = ct_view(&t, now + 300000);
    CHECK("at zero: 0:00 flashing, red first", v.show == CT_SHOW_DONE && v.secs == 0 && v.red);
    v = ct_view(&t, now + 300000 + 600);
    CHECK("then not red", v.show == CT_SHOW_DONE && !v.red);
    v = ct_view(&t, now + 300000 + CT_DONE_MS - 1);
    CHECK("0:00 stays for the whole done time", v.show == CT_SHOW_DONE);
    v = ct_view(&t, now + 300000 + CT_DONE_MS);
    CHECK("then the clock comes back", v.show == CT_SHOW_CLOCK && !v.red);
    CHECK("a timer past its end is not running", !ct_running(&t, now + 300000));

    ct_start(&t, now, 0);
    CHECK("starting 0 s changes nothing", ct_end(&t) == now + 300000);
    ct_start(&t, now, CT_MAX_S + 100);
    CHECK("a start is capped at 9:59:59", ct_view(&t, now).secs == CT_MAX_S);
    ct_stop(&t);
    CHECK("stop shows the clock at once", ct_view(&t, now).show == CT_SHOW_CLOCK);
    __atomic_store_n(&t.end_ms, now + (int64_t) (CT_MAX_S + 10) * 1000, __ATOMIC_RELAXED);
    CHECK("an impossible deadline shows the clock", ct_view(&t, now).show == CT_SHOW_CLOCK);

    /* the popup */
    struct ct_popup p = {0};
    ct_stop(&t);
    CHECK("a press on a closed popup does nothing", !ct_popup_press(&p, &t, CT_B_ADD5, now));
    ct_popup_open(&p, &t, now);
    CHECK("opened with no timer: setting, 0:00", p.open && !p.running && ct_popup_secs(&p, &t, now) == 0);
    CHECK("Start and Reset are disabled at 0:00", !ct_button_enabled(&p, CT_B_START) && !ct_button_enabled(&p, CT_B_RESET));
    CHECK("Start at 0:00 does nothing", !ct_popup_press(&p, &t, CT_B_START, now) && p.open && ct_end(&t) == 0);
    ct_popup_press(&p, &t, CT_B_ADD5, now);
    ct_popup_press(&p, &t, CT_B_ADD1, now);
    CHECK("+5 then +1 is 6:00", ct_popup_secs(&p, &t, now) == 360 && !strcmp(fmt(360), "6:00"));
    CHECK("setting does not start anything", ct_end(&t) == 0);
    ct_popup_press(&p, &t, CT_B_RESET, now);
    CHECK("Reset goes back to 0:00", p.open && ct_popup_secs(&p, &t, now) == 0);
    ct_popup_press(&p, &t, CT_B_ADD10, now);
    ct_popup_press(&p, &t, CT_B_CLOSE, now);
    CHECK("Close without Start changes nothing", !p.open && ct_end(&t) == 0);
    ct_popup_open(&p, &t, now);
    CHECK("reopened, the draft starts over", ct_popup_secs(&p, &t, now) == 0);
    for (int i = 0; i < 30; i++)
        ct_popup_press(&p, &t, CT_B_ADD30, now);
    CHECK("adding stops at 9:59:59", p.draft_s == CT_MAX_S);
    ct_popup_press(&p, &t, CT_B_RESET, now);
    ct_popup_press(&p, &t, CT_B_ADD30, now);
    ct_popup_press(&p, &t, CT_B_ADD30, now);
    CHECK("an hour shows h:mm:ss", !strcmp(fmt(ct_popup_secs(&p, &t, now)), "1:00:00"));
    ct_popup_press(&p, &t, CT_B_RESET, now);
    ct_popup_press(&p, &t, CT_B_ADD1, now);
    CHECK("Start runs the set time and closes", ct_popup_press(&p, &t, CT_B_START, now) && !p.open &&
                                                    ct_view(&t, now).secs == 60 && ct_view(&t, now).red);

    ct_popup_open(&p, &t, now + 10000);
    CHECK("opened while running: shows the remaining time", p.running && ct_popup_secs(&p, &t, now + 10000) == 50);
    CHECK("running: Reset is not shown, Stop is", !ct_button_shown(&p, CT_B_RESET) &&
                                                      !strcmp(ct_button_label(&p, CT_B_START), "Stop"));
    ct_popup_press(&p, &t, CT_B_ADD5, now + 10000);
    CHECK("an add extends the running timer", ct_view(&t, now + 10000).secs == 350 && p.open);
    CHECK("Reset cannot be pressed while running", !ct_popup_press(&p, &t, CT_B_RESET, now + 10000));
    ct_popup_press(&p, &t, CT_B_CLOSE, now + 10000);
    CHECK("Close leaves it running", ct_running(&t, now + 10000) && !p.open);
    ct_popup_open(&p, &t, now + 11000);
    ct_popup_press(&p, &t, CT_B_START, now + 11000);
    CHECK("Stop ends it and closes", !p.open && ct_view(&t, now + 11000).show == CT_SHOW_CLOCK);

    ct_start(&t, now, 2);
    ct_popup_open(&p, &t, now);
    ct_popup_sync(&p, &t, now + 2500);
    CHECK("a countdown that runs out under the popup turns it into a setter",
          p.open && !p.running && ct_popup_secs(&p, &t, now + 2500) == 0);
    ct_popup_press(&p, &t, CT_B_CLOSE, now + 2500);
    ct_start(&t, now, 2);
    ct_popup_open(&p, &t, now);
    CHECK("an add after it ran out sets, not extends",
          ct_popup_press(&p, &t, CT_B_ADD1, now + 2500) && !p.running && p.draft_s == 60 &&
              ct_view(&t, now + 2500).show == CT_SHOW_DONE);
    ct_popup_press(&p, &t, CT_B_CLOSE, now + 2500);
    ct_start(&t, now, CT_MAX_S - 30);
    CHECK("an extend stops at 9:59:59 remaining",
          ct_extend(&t, now, 30 * 60) && ct_view(&t, now).secs == CT_MAX_S);
    ct_stop(&t);
    CHECK("nothing to extend when idle", !ct_extend(&t, now, 60) && ct_end(&t) == 0);

    /* layout: the deck's Clock rect (1129, 23, 105 x 20) on 1280 x 800 */
    struct ct_geom g;
    memset(&g, 0, sizeof g);
    CHECK("lays out on the deck screen", ct_layout(1129, 23, 105, 20, 1280, 800, 0, &g));
    CHECK("under the Clock, right edges together", g.box.y >= 43 && g.box.x + g.box.w == 1234);
    CHECK("on the screen", g.box.x >= 0 && g.box.y + g.box.h <= 800);
    {
        int ok = 1;
        for (int a = 0; a < CT_B_N; a++)
            for (int b = a + 1; b < CT_B_N; b++) {
                struct ct_rect *A = &g.btn[a], *B = &g.btn[b];
                if (A->w && B->w && A->x < B->x + B->w && B->x < A->x + A->w && A->y < B->y + B->h && B->y < A->y + A->h)
                    ok = 0;
            }
        for (int b = 0; b < CT_B_N; b++)
            if (g.btn[b].x < g.box.x || g.btn[b].x + g.btn[b].w > g.box.x + g.box.w ||
                g.btn[b].y + g.btn[b].h > g.box.y + g.box.h || g.btn[b].h < 44)
                ok = 0;
        CHECK("buttons inside the box, apart, at least 44 px tall", ok);
    }
    p = (struct ct_popup) {1, 0, 0};
    {
        int ok = 1;
        for (int b = 0; b < CT_B_N; b++) {
            struct ct_rect *r = &g.btn[b];
            ok &= ct_hit(&g, &p, r->x + r->w / 2, r->y + r->h / 2) == b;
            ok &= ct_hit(&g, &p, r->x, r->y) == b && ct_hit(&g, &p, r->x + r->w - 1, r->y + r->h - 1) == b;
            ok &= ct_hit(&g, &p, r->x + r->w, r->y + r->h) != b;
        }
        CHECK("each button's own rect hits it, its edges included", ok);
    }
    CHECK("the title is no button", ct_hit(&g, &p, g.title.x + 5, g.title.y + 5) == -1);
    CHECK("outside the popup is no button", ct_hit(&g, &p, 10, 700) == -1);
    ct_layout(1129, 23, 105, 20, 1280, 800, 1, &g);
    p.running = 1;
    CHECK("running: two action buttons, Reset nowhere",
          g.btn[CT_B_RESET].w == 0 && ct_hit(&g, &p, g.btn[CT_B_CLOSE].x + 3, g.btn[CT_B_CLOSE].y + 3) == CT_B_CLOSE);
    CHECK("a Clock at the screen's right edge keeps the popup on it",
          ct_layout(1200, 23, 80, 20, 1280, 800, 0, &g) && g.box.x + g.box.w <= 1280);
    CHECK("a Clock at the left edge keeps the popup on it",
          ct_layout(0, 23, 80, 20, 1280, 800, 0, &g) && g.box.x == 0);
    CHECK("a screen too small for it: nothing", !ct_layout(0, 0, 80, 20, 300, 800, 0, &g));
    CHECK("no Clock rect: nothing", !ct_layout(0, 0, 0, 0, 1280, 800, 0, &g));

    /* one writer, one reader on the deadline */
    pthread_t th;
    pthread_create(&th, NULL, reader, NULL);
    for (long i = 0; i < iters; i++) {
        ct_start(&g_rt, 1000, (int) (i % 7) + 1);
        ct_extend(&g_rt, 1000, 60);
        ct_stop(&g_rt);
    }
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
    pthread_join(th, NULL);
    CHECK("a reader beside the writer sees only whole deadlines", ct_view(&g_rt, 1000).show == CT_SHOW_CLOCK);

    return ut_summary();
}
