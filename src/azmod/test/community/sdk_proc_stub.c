/* SPDX-License-Identifier: GPL-3.0-only */
/* Stand-ins for the process, library, audio and vtable-slot services (sdk_proc.c, sdk_lib.c,
 * sdk_audio.c, sdk_vslot.c) in the community host's test: those need EP147's audio stream,
 * systemd and a stick, which this harness does not have. Every service refuses. */
static int (*g_sdk_confirm_core)(const char *title, const char *text, const char *yes,
                                 void (*fn)(int yes, void *user), void *user);
static int sdk_launch(const char *id, const char *path, const char *const argv[],
                      const az_launch_opts *opts) {
    (void) id;
    (void) path;
    (void) argv;
    (void) opts;
    return -1;
}
static int sdk_launch_alive(const char *id, int pid) {
    (void) id;
    (void) pid;
    return 0;
}
static int sdk_launch_stop(const char *id, int pid) {
    (void) id;
    (void) pid;
    return -1;
}
static int sdk_handoff(const char *id, const char *path, const char *const argv[]) {
    (void) id;
    (void) path;
    (void) argv;
    return -1;
}
static void sdk_reload(const char *id) { (void) id; }
static int sdk_lib_playlists(const char *id, int stick, int (*fn)(int, int, const char *, int, void *), void *u) {
    (void) id;
    (void) stick;
    (void) fn;
    (void) u;
    return -1;
}
static int sdk_lib_tracks(const char *id, int stick, int pl, int (*fn)(const az_track *, void *), void *u) {
    (void) id;
    (void) stick;
    (void) pl;
    (void) fn;
    (void) u;
    return -1;
}
static int sdk_audio_tap(const char *id, int source, int frames, void (*fn)(const float *, int, void *), void *u) {
    (void) id;
    (void) source;
    (void) frames;
    (void) fn;
    (void) u;
    return -1;
}
static int sdk_lib_spool_kick(const char *id, int stick) {
    (void) id;
    (void) stick;
    return -1;
}
static int sdk_claim_vslot(const char *id, uintptr_t slot, void *fn, void **orig) {
    (void) id;
    (void) slot;
    (void) fn;
    (void) orig;
    return -1;
}
static int sdk_proc_forget(const char *id) {
    (void) id;
    return 0;
}
