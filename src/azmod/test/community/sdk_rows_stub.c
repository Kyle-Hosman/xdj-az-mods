/* SPDX-License-Identifier: GPL-3.0-only */
/* Stand-ins for the row services (sdk_rows.c) in the community host's test: those need the
 * feature table and the settings file, which this harness does not have. Every call refuses. */
static int cm_register_row(const char *id, const az_row_v1 *r) {
    (void) id;
    (void) r;
    return -1;
}
static int cm_row_value(int handle) {
    (void) handle;
    return -1;
}
static int cm_row_set(int handle, int value) {
    (void) handle;
    (void) value;
    return -1;
}
static int cm_row_lookup(const char *key) {
    (void) key;
    return -1;
}
static void sdk_rows_tick(void) {}
static void sdk_rows_forget(cm_mod *c) { (void) c; }
