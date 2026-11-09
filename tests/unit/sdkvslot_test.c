/* SPDX-License-Identifier: GPL-3.0-only */
/* claim_vslot for community mods (guest/azmod/sdk_vslot.c): a virtual-method hook on one
 * vtable slot. This binary stands in for EP147: a const table of function pointers in its own
 * read-only data plays the vtable, its code range plays EP147's. Questions asked:
 *   - a valid slot is swapped, the original handed back, and the page is read-only again;
 *   - refused: a mod without "hooks": true, a misaligned slot, a slot outside the program's
 *     data, a slot that holds no function in the program's code (already swapped), a
 *     replacement that is not the mod's own code, and a second claim of the same slot.
 * The controls take out the "holds a function" check and the one-owner check; each must fail.
 * VSLOT_INC = the sdk_vslot.c under test. Linux only. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "check.h"

#define AZCM_LOADED 3
typedef struct {
    char id[33];
    int state, hooks_ok, any, nhooks;
    int sticky;
} cm_mod;
static int cm_running(const cm_mod *c) { return __atomic_load_n(&c->state, __ATOMIC_ACQUIRE) == AZCM_LOADED; }
static cm_mod g_cm[1] = {{"mod-a", AZCM_LOADED, 1, 0, 0}};
static int g_cm_loading = -1;
static pthread_mutex_t g_cm_mu = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static uintptr_t g_cm_text_lo, g_cm_text_hi;
static uintptr_t g_mod_fn; /* the one function this test treats as the mod's code */
static cm_mod *cm_find(const char *id) { return id && !strcmp(id, "mod-a") ? &g_cm[0] : NULL; }
#define CM_RA() ((uintptr_t) 0)
static int cm_called_by(cm_mod *c, uintptr_t a) {
    (void) c;
    return a == 0 || a == g_mod_fn;
}
#define CM_OUTSIDE_CODE "called from outside the mod's code"
static void cm_logf(cm_mod *c, int force, const char *fmt, ...) { (void) c, (void) force, (void) fmt; }

/* the core's slot swap: unlock the page, publish the original, then the slot, lock it again */
static int azm_patch_vtable(uintptr_t slot_addr, void *hook, void **orig) {
    uintptr_t pg = slot_addr & ~(uintptr_t) (getpagesize() - 1);
    if (mprotect((void *) pg, getpagesize(), PROT_READ | PROT_WRITE))
        return -1;
    __atomic_store_n(orig, *(void **) slot_addr, __ATOMIC_RELAXED);
    __atomic_store_n((void **) slot_addr, hook, __ATOMIC_RELEASE);
    mprotect((void *) pg, getpagesize(), PROT_READ);
    return 1;
}

#include VSLOT_INC

static void m1(void) {}
static void m2(void) {}
static void hook(void) {}
static int some_data = 42;
/* the "vtables": const pointer tables, which a PIE puts in .data.rel.ro */
void (*const g_vt[4])(void) = {m1, m2, m1, m2};
void *const g_vt_bad[1] = {&some_data};

static int text_cb(struct dl_phdr_info *info, size_t sz, void *data) {
    (void) sz, (void) data;
    if (info->dlpi_name && info->dlpi_name[0])
        return 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_LOAD && (ph->p_flags & PF_X)) {
            g_cm_text_lo = info->dlpi_addr + ph->p_vaddr;
            g_cm_text_hi = g_cm_text_lo + ph->p_memsz;
        }
    }
    return 1;
}

/* what the slot holds now: g_vt is const, and GCC folds even a volatile read of a const object
 * to its initializer, so the address is hidden from the optimizer first */
static void *slot_now(int i) {
    uintptr_t a = (uintptr_t) &g_vt[i];
    __asm__ volatile("" : "+r"(a));
    return *(void *volatile *) a;
}

static int page_writable(uintptr_t a) {
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    int w = -1;
    while (f && fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char perm[8];
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perm) == 3 && a >= lo && a < hi)
            w = perm[1] == 'w';
    }
    if (f)
        fclose(f);
    return w;
}

int main(void) {
    ut_suite = "sdk: claim_vslot";
    dl_iterate_phdr(text_cb, NULL);
    g_mod_fn = (uintptr_t) hook;
    void *orig = NULL;
    uintptr_t slot = (uintptr_t) &g_vt[1];
    CHECK("precondition: the test vtable is in the program's read-only data", page_writable(slot) == 0);

    g_cm[0].hooks_ok = 0;
    CHECK("a mod without \"hooks\": true is refused", sdk_claim_vslot("mod-a", slot, (void *) hook, &orig) == -1);
    g_cm[0].hooks_ok = 1;
    CHECK("a misaligned slot is refused", sdk_claim_vslot("mod-a", slot + 4, (void *) hook, &orig) == -1);
    void *stack_slot = (void *) m1;
    CHECK("a slot outside the program's data is refused",
          sdk_claim_vslot("mod-a", (uintptr_t) &stack_slot, (void *) hook, &orig) == -1);
    CHECK("a slot holding no function in the program's code is refused",
          sdk_claim_vslot("mod-a", (uintptr_t) &g_vt_bad[0], (void *) hook, &orig) == -1);
    CHECK("a replacement that is not the mod's code is refused",
          sdk_claim_vslot("mod-a", slot, (void *) m2, &orig) == -1);
    CHECK("the slot is untouched by the refusals", slot_now(1) == (void *) m2);

    CHECK("a valid slot is claimed", sdk_claim_vslot("mod-a", slot, (void *) hook, &orig) == 0);
    CHECK("the slot now calls the mod and the original came back", slot_now(1) == (void *) hook && orig == (void *) m2);
    CHECK("the page is read-only again", page_writable(slot) == 0);
    void *orig2 = NULL;
    g_mod_fn = (uintptr_t) m1; /* a second claim of the slot, by code the check would accept */
    CHECK("a second claim of the same slot is refused",
          sdk_claim_vslot("mod-a", slot, (void *) m1, &orig2) == -1 && slot_now(1) == (void *) hook);
    return ut_summary();
}
