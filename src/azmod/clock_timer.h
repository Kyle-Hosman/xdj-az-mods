/* SPDX-License-Identifier: GPL-3.0-only */
/* clock_timer.h: the Clock's countdown Timer, the pure part: the deadline, what the Clock's place
 * shows at a given moment (the time of day, the countdown, or the flashing 0:00), the popup's
 * buttons and what each does, and the popup's layout and hit test. No I/O, no allocation, no
 * system calls: the caller passes CLOCK_MONOTONIC milliseconds, so a change of the wall clock
 * or the time zone never moves a running timer.
 *
 * The deadline is the only state another thread may read (ct_view), so it is kept with
 * __atomic builtins; the popup belongs to the thread that takes the taps and draws it. */
#ifndef CLOCK_TIMER_H
#define CLOCK_TIMER_H
#include <stdint.h>
#include <stdio.h>

#define CT_MAX_S (10 * 3600 - 1) /* 9:59:59: the longest timer, one hour digit */
#define CT_RED_MS 60000          /* the last minute flashes red */
#define CT_DONE_MS 3000          /* how long 0:00 flashes once the timer runs out */
#define CT_FLASH_MS 500          /* red half of each second (1 Hz) */

struct ct_timer {
    int64_t end_ms; /* CLOCK_MONOTONIC deadline in ms; 0 = no timer. __atomic only */
};

enum ct_show {
    CT_SHOW_CLOCK, /* no timer: the time of day */
    CT_SHOW_COUNT, /* counting down */
    CT_SHOW_DONE,  /* ran out: 0:00, flashing red */
};

struct ct_view {
    enum ct_show show;
    int secs; /* the seconds shown (rounded up while counting; 0 when done) */
    int red;  /* draw in red this frame */
};

/* "m:ss", or "h:mm:ss" from one hour; s is clamped to 0..CT_MAX_S */
static inline void ct_format(int s, char *out, size_t n) {
    if (s < 0)
        s = 0;
    if (s > CT_MAX_S)
        s = CT_MAX_S;
    if (s >= 3600)
        snprintf(out, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else
        snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

static inline int64_t ct_end(const struct ct_timer *t) {
    return __atomic_load_n(&t->end_ms, __ATOMIC_ACQUIRE);
}

/* What the Clock's place shows at now_ms. A deadline more than CT_MAX_S ahead (a clock that went
 * backwards cannot happen with CLOCK_MONOTONIC, but the value is checked anyway) shows the clock. */
static inline struct ct_view ct_view(const struct ct_timer *t, int64_t now_ms) {
    struct ct_view v = {CT_SHOW_CLOCK, 0, 0};
    int64_t end = ct_end(t);
    if (end <= 0)
        return v;
    int64_t rem = end - now_ms;
    if (rem > (int64_t) CT_MAX_S * 1000)
        return v;
    if (rem > 0) {
        v.show = CT_SHOW_COUNT;
        v.secs = (int) ((rem + 999) / 1000);
        /* the first half of each shown second is red: the color changes with the digits */
        v.red = rem <= CT_RED_MS && (rem - 1) % 1000 >= 1000 - CT_FLASH_MS;
        return v;
    }
    int64_t past = -rem;
    if (past < CT_DONE_MS) {
        v.show = CT_SHOW_DONE;
        v.red = past % 1000 < CT_FLASH_MS;
    }
    return v;
}

/* Is a countdown running (not the 0:00 flash, not idle) */
static inline int ct_running(const struct ct_timer *t, int64_t now_ms) {
    return ct_view(t, now_ms).show == CT_SHOW_COUNT;
}

static inline void ct_start(struct ct_timer *t, int64_t now_ms, int secs) {
    if (secs <= 0)
        return;
    if (secs > CT_MAX_S)
        secs = CT_MAX_S;
    __atomic_store_n(&t->end_ms, now_ms + (int64_t) secs * 1000, __ATOMIC_RELEASE);
}

static inline void ct_stop(struct ct_timer *t) {
    __atomic_store_n(&t->end_ms, 0, __ATOMIC_RELEASE);
}

/* Adds secs to a running countdown, never past CT_MAX_S remaining. 0 = nothing was running. */
static inline int ct_extend(struct ct_timer *t, int64_t now_ms, int secs) {
    int64_t end = ct_end(t);
    if (end <= 0 || end - now_ms <= 0)
        return 0;
    int64_t ne = end + (int64_t) secs * 1000, cap = now_ms + (int64_t) CT_MAX_S * 1000;
    if (ne > cap)
        ne = cap;
    /* a deadline that moved meanwhile (stopped, or the countdown ran out) is left as it is */
    return __atomic_compare_exchange_n(&t->end_ms, &end, ne, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* ---- the popup ----------------------------------------------------------------------------- */
enum ct_button {
    CT_B_ADD1,
    CT_B_ADD5,
    CT_B_ADD10,
    CT_B_ADD30,
    CT_B_START, /* setting: Start; running: Stop */
    CT_B_RESET, /* setting only */
    CT_B_CLOSE,
    CT_B_N,
};
static const int CT_ADD_MIN[4] = {1, 5, 10, 30};

struct ct_popup {
    int open;
    int running; /* opened over a running countdown: the add buttons extend it, Stop ends it */
    int draft_s; /* setting: the duration Start would run */
};

/* Opens the popup over whatever the timer is doing: a running countdown shows its remaining
 * time; anything else starts a new duration at 0. */
static inline void ct_popup_open(struct ct_popup *p, const struct ct_timer *t, int64_t now_ms) {
    p->open = 1;
    p->running = ct_running(t, now_ms);
    p->draft_s = 0;
}

/* Called each frame while open: a countdown that ran out (or was stopped elsewhere) under the
 * popup turns it into a fresh setter. */
static inline void ct_popup_sync(struct ct_popup *p, const struct ct_timer *t, int64_t now_ms) {
    if (p->open && p->running && !ct_running(t, now_ms)) {
        p->running = 0;
        p->draft_s = 0;
    }
}

/* The duration the popup shows */
static inline int ct_popup_secs(const struct ct_popup *p, const struct ct_timer *t, int64_t now_ms) {
    if (p->running) {
        struct ct_view v = ct_view(t, now_ms);
        return v.show == CT_SHOW_COUNT ? v.secs : 0;
    }
    return p->draft_s;
}

/* Is button b shown, and can it be pressed */
static inline int ct_button_shown(const struct ct_popup *p, int b) {
    return b >= 0 && b < CT_B_N && !(b == CT_B_RESET && p->running);
}
static inline int ct_button_enabled(const struct ct_popup *p, int b) {
    if (!ct_button_shown(p, b))
        return 0;
    if (!p->running && (b == CT_B_START || b == CT_B_RESET))
        return p->draft_s > 0;
    return 1;
}

/* Presses button b. Returns 1 when something changed (redraw). Close, Start and Stop close the
 * popup; Close never touches the timer. */
static inline int ct_popup_press(struct ct_popup *p, struct ct_timer *t, int b, int64_t now_ms) {
    if (!p->open || !ct_button_enabled(p, b))
        return 0;
    ct_popup_sync(p, t, now_ms);
    switch (b) {
    case CT_B_ADD1:
    case CT_B_ADD5:
    case CT_B_ADD10:
    case CT_B_ADD30: {
        int add = CT_ADD_MIN[b - CT_B_ADD1] * 60;
        if (p->running)
            return ct_extend(t, now_ms, add);
        p->draft_s = p->draft_s + add > CT_MAX_S ? CT_MAX_S : p->draft_s + add;
        return 1;
    }
    case CT_B_START:
        if (p->running)
            ct_stop(t);
        else if (p->draft_s > 0)
            ct_start(t, now_ms, p->draft_s);
        else
            return 0;
        p->open = 0;
        return 1;
    case CT_B_RESET:
        p->draft_s = 0;
        return 1;
    case CT_B_CLOSE:
        p->open = 0;
        return 1;
    default:
        return 0;
    }
}

/* ---- layout ---------------------------------------------------------------------------------- */
struct ct_rect {
    int x, y, w, h;
};
struct ct_geom {
    struct ct_rect box;             /* the whole popup */
    struct ct_rect title, value;    /* "Timer" and the duration */
    struct ct_rect btn[CT_B_N];     /* every button's rect (shown or not) */
    int px_title, px_value, px_btn; /* text sizes */
};

/* Sizes at 1280 x 800, scaled to the screen */
#define CT_W 470
#define CT_PAD 16
#define CT_GAP 10
#define CT_HEAD 44
#define CT_VALUE 64
#define CT_BTN_H 60

static inline int ct_sc(int v, int scr_h) {
    return v * scr_h / 800;
}

/* The popup under the Clock's rect (cx, cy, cw, ch), its right edge at the Clock's right edge,
 * kept on the screen. 0 when the screen cannot hold it (then nothing is shown). */
static inline int ct_layout(int cx, int cy, int cw, int ch, int scr_w, int scr_h, int running,
                            struct ct_geom *g) {
    if (scr_w <= 0 || scr_h <= 0 || cw <= 0 || ch <= 0)
        return 0;
    int pad = ct_sc(CT_PAD, scr_h), gap = ct_sc(CT_GAP, scr_h), bh = ct_sc(CT_BTN_H, scr_h);
    int w = ct_sc(CT_W, scr_h), head = ct_sc(CT_HEAD, scr_h), val = ct_sc(CT_VALUE, scr_h);
    int h = pad + head + val + gap + bh + gap + bh + pad;
    if (w > scr_w || h > scr_h)
        return 0;
    int x = cx + cw - w, y = cy + ch + ct_sc(14, scr_h);
    if (x + w > scr_w)
        x = scr_w - w;
    if (x < 0)
        x = 0;
    if (y + h > scr_h)
        y = scr_h - h;
    if (y < 0)
        y = 0;
    g->box = (struct ct_rect) {x, y, w, h};
    int iw = w - 2 * pad;
    g->title = (struct ct_rect) {x + pad, y + pad, iw, head};
    g->value = (struct ct_rect) {x + pad, y + pad + head, iw, val};
    int r1 = y + pad + head + val + gap, r2 = r1 + bh + gap;
    int aw = (iw - 3 * gap) / 4;
    for (int i = 0; i < 4; i++)
        g->btn[CT_B_ADD1 + i] = (struct ct_rect) {x + pad + i * (aw + gap), r1, aw, bh};
    /* the action row: Start | Reset | Close while setting, Stop | Close while running */
    int n = running ? 2 : 3, bw = (iw - (n - 1) * gap) / n;
    g->btn[CT_B_START] = (struct ct_rect) {x + pad, r2, bw, bh};
    g->btn[CT_B_RESET] = running ? (struct ct_rect) {0, 0, 0, 0} : (struct ct_rect) {x + pad + bw + gap, r2, bw, bh};
    g->btn[CT_B_CLOSE] = (struct ct_rect) {x + pad + (n - 1) * (bw + gap), r2, bw, bh};
    g->px_title = ct_sc(22, scr_h);
    g->px_value = ct_sc(40, scr_h);
    g->px_btn = ct_sc(22, scr_h);
    return 1;
}

static inline int ct_in(const struct ct_rect *r, int x, int y) {
    return r->w > 0 && x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

/* The shown button under (x, y), -1 = none */
static inline int ct_hit(const struct ct_geom *g, const struct ct_popup *p, int x, int y) {
    for (int b = 0; b < CT_B_N; b++)
        if (ct_button_shown(p, b) && ct_in(&g->btn[b], x, y))
            return b;
    return -1;
}

static inline const char *ct_button_label(const struct ct_popup *p, int b) {
    static const char *const L[CT_B_N] = {"+1 min", "+5 min", "+10 min", "+30 min", "Start", "Reset", "Close"};
    if (b == CT_B_START && p->running)
        return "Stop";
    return b >= 0 && b < CT_B_N ? L[b] : "";
}

#endif
