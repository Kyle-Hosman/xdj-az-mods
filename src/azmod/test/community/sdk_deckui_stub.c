/* SPDX-License-Identifier: GPL-3.0-only */
/* Stand-ins for the deck and UI services (sdk_deck.c, sdk_ui.c) in the community host's test:
 * those need azstem, EP147's message thread and the overlay, which this harness does not have.
 * Every service answers as it would before any of them is up. */
static int sd_deck_info(int deck, az_deck_info *out) {
    (void) deck;
    (void) out;
    return -1;
}
static int sd_deck_grid(int deck, double *b, double *bar) {
    (void) deck;
    (void) b;
    (void) bar;
    return -1;
}
static int sd_on_deck_event(const char *id, void (*fn)(int, int, void *), void *u) {
    (void) id;
    (void) fn;
    (void) u;
    return -1;
}
static void sd_banner(const char *id, const char *t, int ms) {
    (void) id;
    (void) t;
    (void) ms;
}
static int sd_confirm(const char *id, const char *t, const char *x, const char *y, void (*fn)(int, void *), void *u) {
    (void) id;
    (void) t;
    (void) x;
    (void) y;
    (void) fn;
    (void) u;
    return -1;
}
static void sd_post_ui(const char *id, void (*fn)(void *), void *u) {
    (void) id;
    (void) fn;
    (void) u;
}
static int sd_thread_start(const char *id, const char *n, void *(*fn)(void *), void *a) {
    (void) id;
    (void) n;
    (void) fn;
    (void) a;
    return -1;
}
static void sdk_deck_tick(long now) { (void) now; }
static void sdk_ui_tick(void) {}
static void sdk_confirm_tick(void) {}
int azcm_confirm(const char *t, const char *x, const char *y, void (*fn)(int, void *), void *u) {
    (void) t;
    (void) x;
    (void) y;
    (void) fn;
    (void) u;
    return -1;
}
static void sdk_deck_forget(int owner) { (void) owner; }
static int sd_threads_of(int owner) {
    (void) owner;
    return 0;
}
