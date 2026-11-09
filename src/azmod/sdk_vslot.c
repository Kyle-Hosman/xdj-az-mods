/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file sdk_vslot.c
 * @brief Community mod service: claim_vslot, a virtual-method hook on one vtable slot
 *
 * The same rules as claim_hook (a mod that declares "hooks": true and lists this firmware, from
 * its own code, one owner per slot), plus what makes a vtable slot safe to swap: the slot is
 * an aligned pointer inside EP147's own read-only data, it currently holds a function in
 * EP147's code (so a slot already swapped by AZ-MODS or another mod is refused), and the new
 * function is inside the mod. The swap itself is azm_patch_vtable: the original is published
 * before the slot, and the page's protection is restored exactly (never left writable).
 * Included by community.c ahead of the services table.
 */
#define SDK_VSLOTS 64

static struct {
    uintptr_t slot;
    char who[33];
} g_sdk_vslot[SDK_VSLOTS];
static int g_sdk_vslot_n;

/* EP147's non-executable loaded segments (vtables live in .data.rel.ro). */
static struct {
    uintptr_t lo, hi;
} g_sdk_ep_data[8];
static int g_sdk_ep_data_n = -1;

static int sdk_ep_data_cb(struct dl_phdr_info *info, size_t sz, void *data) {
    (void) sz, (void) data;
    if (info->dlpi_name && info->dlpi_name[0])
        return 0; /* the main program only */
    for (int i = 0; i < info->dlpi_phnum && g_sdk_ep_data_n < 8; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD || (ph->p_flags & PF_X))
            continue;
        g_sdk_ep_data[g_sdk_ep_data_n].lo = info->dlpi_addr + ph->p_vaddr;
        g_sdk_ep_data[g_sdk_ep_data_n].hi = info->dlpi_addr + ph->p_vaddr + ph->p_memsz;
        g_sdk_ep_data_n++;
    }
    return 1;
}

static int sdk_in_ep_data(uintptr_t a) {
    if (g_sdk_ep_data_n < 0) {
        g_sdk_ep_data_n = 0;
        dl_iterate_phdr(sdk_ep_data_cb, NULL);
    }
    for (int i = 0; i < g_sdk_ep_data_n; i++)
        if (a >= g_sdk_ep_data[i].lo && a + sizeof(void *) <= g_sdk_ep_data[i].hi)
            return 1;
    return 0;
}

static int sdk_claim_vslot(const char *id, uintptr_t slot, void *fn, void **orig) {
    cm_mod *c = cm_find(id);
    if (!c)
        return -1;
    if (!cm_called_by(c, CM_RA())) {
        cm_logf(c, 1, "claim_vslot 0x%lx refused: " CM_OUTSIDE_CODE,
                (unsigned long) slot);
        return -1;
    }
    if (!cm_running(c) && (c - g_cm) != g_cm_loading) {
        cm_logf(c, 1, "claim_vslot 0x%lx refused: the mod is not loaded", (unsigned long) slot);
        return -1;
    }
    if (!c->hooks_ok) {
        cm_logf(c, 1, "claim_vslot 0x%lx refused: %s", (unsigned long) slot,
                c->any ? "a mod for \"any\" firmware may not patch EP147"
                       : "azmod.json does not declare \"hooks\": true");
        return -1;
    }
    const char *why = NULL;
    if (!orig || !fn)
        why = "no function or no place for the original";
    else if (slot & 7)
        why = "not an aligned pointer";
    else if (!sdk_in_ep_data(slot))
        why = "not inside EP147's data";
    else if (!cm_called_by(c, (uintptr_t) fn))
        why = "the new function is not inside this mod";
    else {
        uintptr_t cur = __atomic_load_n((uintptr_t *) slot, __ATOMIC_ACQUIRE);
        if (cur < g_cm_text_lo || cur >= g_cm_text_hi)
            why = "the slot does not hold a function in EP147's code (already swapped?)";
    }
    if (why) {
        cm_logf(c, 1, "claim_vslot 0x%lx refused: %s", (unsigned long) slot, why);
        return -1;
    }
    pthread_mutex_lock(&g_cm_mu);
    for (int i = 0; i < g_sdk_vslot_n; i++)
        if (g_sdk_vslot[i].slot == slot) {
            pthread_mutex_unlock(&g_cm_mu);
            cm_logf(c, 1, "claim_vslot 0x%lx refused: already claimed by %s", (unsigned long) slot,
                    g_sdk_vslot[i].who);
            return -1;
        }
    int rc = g_sdk_vslot_n < SDK_VSLOTS ? azm_patch_vtable(slot, fn, orig) : -1;
    if (rc == 1) {
        g_sdk_vslot[g_sdk_vslot_n].slot = slot;
        snprintf(g_sdk_vslot[g_sdk_vslot_n].who, sizeof g_sdk_vslot[0].who, "%s", c->id);
        g_sdk_vslot_n++;
    }
    pthread_mutex_unlock(&g_cm_mu);
    if (rc != 1) {
        cm_logf(c, 1, "claim_vslot 0x%lx refused: the slot could not be written", (unsigned long) slot);
        return -1;
    }
    c->nhooks++;
    __atomic_store_n(&c->sticky, 1, __ATOMIC_RELAXED); /* lasts until the next start: no live stop */
    cm_logf(c, 1, "claim_vslot 0x%lx ok", (unsigned long) slot);
    return 0;
}
