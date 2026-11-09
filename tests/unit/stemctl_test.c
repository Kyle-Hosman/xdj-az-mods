/* SPDX-License-Identifier: GPL-3.0-only */
/* The stem toggles' two inputs (src/azstem/azstem.c, extracted at run time as STEMCTL_INC):
 * azmod's direct call and the file poll.
 *   - with no direct call the files are the input, an absent master file means enabled;
 *   - a change of the enabled stems raises the deck's evict edge, a change of the engaged bit alone
 *     does not, and the first value seen is a baseline, not an edge;
 *   - the direct call takes effect at once and the poll never overwrites what it took over, with
 *     the other decks still following their files;
 *   - once everything was handed over the poll reads nothing;
 *   - a deck out of range changes nothing;
 *   - a poller running through a burst of direct calls leaves the last value in place (run under
 *     ThreadSanitizer in the sanitizer pass). */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "check.h"

#define NDECK 4
static struct {
    _Atomic unsigned ctrl;
} g_dk[NDECK];
static _Atomic int g_enable = 1;
static _Atomic unsigned g_evict_req;
static char STEMS_PATH[NDECK][256];
static char ENABLE_PATH[256];

#include STEMCTL_INC

static void put(const char *path, const char *v) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(v, f);
        fclose(f);
    }
}
static unsigned ctrl(int d) { return atomic_load(&g_dk[d].ctrl); }
static _Atomic int g_stop;
static void *poller(void *arg) {
    (void) arg;
    while (!atomic_load(&g_stop))
        read_stems();
    return NULL;
}

int main(void) {
    ut_suite = "stem control";
    char dir[] = "/tmp/az-stemctl.XXXXXX";
    if (!mkdtemp(dir))
        return 2;
    for (int d = 0; d < NDECK; d++)
        snprintf(STEMS_PATH[d], sizeof STEMS_PATH[d], "%s/stems%d", dir, d);
    snprintf(ENABLE_PATH, sizeof ENABLE_PATH, "%s/enable", dir);

    put(STEMS_PATH[0], "143\n");
    put(STEMS_PATH[1], "129\n");
    atomic_store(&g_enable, 0);
    read_stems();
    CHECK("poll: deck 1 from its file", ctrl(0) == 143);
    CHECK("poll: deck 2 from its file", ctrl(1) == 129);
    CHECK("poll: no file reads as 0", ctrl(2) == 0);
    CHECK("poll: no master file means enabled", atomic_load(&g_enable) == 1);
    CHECK("poll: the first values are no edge", atomic_load(&g_evict_req) == 0);

    put(STEMS_PATH[0], "15\n");
    read_stems();
    CHECK("poll: only the engaged bit moved, no edge", atomic_load(&g_evict_req) == 0);
    put(STEMS_PATH[0], "14\n");
    read_stems();
    CHECK("poll: a stem toggled, deck 1's edge", atomic_load(&g_evict_req) == 1u);
    atomic_store(&g_evict_req, 0);
    put(ENABLE_PATH, "0");
    read_stems();
    CHECK("poll: the master off from its file", atomic_load(&g_enable) == 0);
    put(ENABLE_PATH, "1");

    azstem_ctl_set(0, 0x83);
    CHECK("direct: at once", ctrl(0) == 0x83);
    CHECK("direct: a stem toggled, deck 1's edge", atomic_load(&g_evict_req) == 1u);
    atomic_store(&g_evict_req, 0);
    put(STEMS_PATH[0], "143\n");
    put(STEMS_PATH[1], "130\n");
    read_stems();
    CHECK("direct: the poll leaves that deck alone", ctrl(0) == 0x83);
    CHECK("direct: another deck still follows its file", ctrl(1) == 130);
    CHECK("direct: the stale file raises no edge", (atomic_load(&g_evict_req) & 1u) == 0);
    atomic_store(&g_evict_req, 0);

    azstem_ctl_set(4, 0x81);
    azstem_ctl_set(-2, 0x81);
    CHECK("a deck out of range: nothing", ctrl(0) == 0x83 && ctrl(1) == 130 && ctrl(2) == 0 &&
                                              ctrl(3) == 0 && atomic_load(&g_evict_req) == 0);

    azstem_ctl_set(-1, 0);
    CHECK("direct: the master", atomic_load(&g_enable) == 0);
    put(ENABLE_PATH, "1");
    read_stems();
    CHECK("direct: the poll leaves the master alone", atomic_load(&g_enable) == 0);
    azstem_ctl_set(1, 0x8f);
    azstem_ctl_set(2, 0x8f);
    azstem_ctl_set(3, 0x8f);
    for (int d = 0; d < NDECK; d++)
        put(STEMS_PATH[d], "1\n");
    read_stems();
    CHECK("all handed over: the poll changes nothing",
          ctrl(0) == 0x83 && ctrl(1) == 0x8f && ctrl(2) == 0x8f && ctrl(3) == 0x8f &&
              atomic_load(&g_enable) == 0);
    CHECK("all handed over: a poll is a load",
          atomic_load(&g_ctl_direct) == CTL_ALL_BITS);

    /* The race the lock is for: a poller that read a file while the first direct calls land. Fresh
     * state per round: the handover is one way, so each round forgets it. */
    pthread_t t;
    atomic_store(&g_stop, 0);
    atomic_store(&g_ctl_direct, 0);
    if (pthread_create(&t, NULL, poller, NULL) != 0)
        return 2;
    usleep(2000);
    for (unsigned i = 0; i < 2000; i++) {
        azstem_ctl_set((int) (i % NDECK), 0x80u | (i & 0xFu));
        azstem_ctl_set(-1, i & 1u);
    }
    azstem_ctl_set(0, 0x85);
    azstem_ctl_set(1, 0x86);
    azstem_ctl_set(2, 0x87);
    azstem_ctl_set(3, 0x88);
    azstem_ctl_set(-1, 1);
    usleep(2000);
    atomic_store(&g_stop, 1);
    pthread_join(t, NULL);
    CHECK("poller and direct calls: the last direct values stand", ctrl(0) == 0x85 && ctrl(1) == 0x86 &&
                                                                       ctrl(2) == 0x87 && ctrl(3) == 0x88 &&
                                                                       atomic_load(&g_enable) == 1);
    for (int d = 0; d < NDECK; d++)
        unlink(STEMS_PATH[d]);
    unlink(ENABLE_PATH);
    rmdir(dir);
    return ut_summary();
}
