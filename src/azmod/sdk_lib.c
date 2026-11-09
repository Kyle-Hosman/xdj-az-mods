/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file sdk_lib.c
 * @brief Community mod services: the rekordbox library on a stick, read-only (lib_playlists,
 *        lib_tracks)
 *
 * Each call opens its own connection to the stick's exportLibrary.db, read-only and immutable
 * (no locks, so EP147's own writes are never made to wait), reads what it needs into a
 * snapshot, closes the connection, and only then calls the mod back. The mod's callback never
 * runs while the database is open. A write that lands during the read can leave the snapshot
 * inconsistent, so the file's size and time are checked before and after and the read is
 * retried once; a second change returns -1.
 *
 * stick 0 is the deck's USB 1 port, 1 is USB 2, resolved as the deck's own loader does (see
 * sdk_port_of_sysfs); a stick whose port cannot be told is not read. Runs on the caller's
 * thread (a mod thread), never on EP147's or the UI thread. Refusals go to the mod's log.
 * Included by community.c ahead of the services table.
 */
#include "pioneer_dir.h" /* PIONEER/ or .PIONEER/: where a stick's rekordbox export is */
#ifndef ONELIBRARY_KEY_SM
#include "onelibkey_sm.h"
#endif

#ifndef SDK_LIB_MEDIA
#define SDK_LIB_MEDIA "/media/usb" /* where the deck mounts USB sticks */
#endif
#ifndef SDK_LIB_MOUNTS
#define SDK_LIB_MOUNTS "/proc/mounts"
#endif
#ifndef SDK_LIB_SYSBLOCK
#define SDK_LIB_SYSBLOCK "/sys/class/block"
#endif
#define SDK_LIB_ROWS 20000 /* rows per call (azmod-mod.h states it): more are cut, and the call returns 1 */
#define SDK_LIB_STR 192    /* bytes kept per text field */

static struct {
    int (*open_v2)(const char *, void **, int, const char *);
    int (*key)(void *, const void *, int);
    int (*close)(void *);
    int (*prepare)(void *, const char *, int, void **, const char **);
    int (*bind_int64)(void *, int, long long);
    int (*step)(void *);
    long long (*col_int64)(void *, int);
    const unsigned char *(*col_text)(void *, int);
    int (*finalize)(void *);
    int ok;
} g_sdk_sq;

static int sdk_sq_resolve(void) {
    if (g_sdk_sq.ok)
        return g_sdk_sq.ok > 0;
    *(void **) &g_sdk_sq.open_v2 = dlsym(RTLD_DEFAULT, "sqlite3_open_v2");
    *(void **) &g_sdk_sq.key = dlsym(RTLD_DEFAULT, "sqlite3_key");
    *(void **) &g_sdk_sq.close = dlsym(RTLD_DEFAULT, "sqlite3_close");
    *(void **) &g_sdk_sq.prepare = dlsym(RTLD_DEFAULT, "sqlite3_prepare_v2");
    *(void **) &g_sdk_sq.bind_int64 = dlsym(RTLD_DEFAULT, "sqlite3_bind_int64");
    *(void **) &g_sdk_sq.step = dlsym(RTLD_DEFAULT, "sqlite3_step");
    *(void **) &g_sdk_sq.col_int64 = dlsym(RTLD_DEFAULT, "sqlite3_column_int64");
    *(void **) &g_sdk_sq.col_text = dlsym(RTLD_DEFAULT, "sqlite3_column_text");
    *(void **) &g_sdk_sq.finalize = dlsym(RTLD_DEFAULT, "sqlite3_finalize");
    g_sdk_sq.ok = (g_sdk_sq.open_v2 && g_sdk_sq.key && g_sdk_sq.close && g_sdk_sq.prepare &&
                   g_sdk_sq.bind_int64 && g_sdk_sq.step && g_sdk_sq.col_int64 &&
                   g_sdk_sq.col_text && g_sdk_sq.finalize)
                      ? 1
                      : -1;
    return g_sdk_sq.ok > 0;
}

/* ---- which stick is in which port -------------------------------------------------------
 * The same rule as the deck's az-usbpick port_of(), the one copy of the stock udev rule
 * (90-usb-caution.rules): the block device's sysfs path names the USB device two levels above
 * its SCSI host, ".../<bus>/<bus>:<cfg>/hostN/...". 1-1 and 3-1 are the USB 1 port, 2-1 and
 * 4-1 the USB 2 port. Anything else (a stick behind a hub, an emulator's second stick on one
 * bus) is unknown, and an unknown port is never guessed: that stick has no library here. */
static int sdk_port_of_sysfs(const char *real) {
    const char *h = real;
    while ((h = strstr(h, "/host")) && !(h[5] >= '0' && h[5] <= '9'))
        h++;
    if (!h)
        return 0;
    const char *b = h, *a; /* b: the "/" before <bus>:<cfg>; a: the "/" before <bus> */
    do
        b--;
    while (b > real && *b != '/');
    if (b <= real)
        return 0;
    a = b;
    do
        a--;
    while (a > real && *a != '/');
    if (*a != '/')
        return 0;
    size_t n = (size_t) (b - a - 1);
    char bus[16];
    if (n == 0 || n >= sizeof bus)
        return 0;
    memcpy(bus, a + 1, n);
    bus[n] = 0;
    if (!strcmp(bus, "1-1") || !strcmp(bus, "3-1"))
        return 1;
    if (!strcmp(bus, "2-1") || !strcmp(bus, "4-1"))
        return 2;
    return 0;
}

/* The mount point of the rekordbox library in the deck's USB port (stick 0 = USB 1, 1 = USB 2):
 * the one volume mounted under SDK_LIB_MEDIA from a device in that port that holds a library.
 * 0 = none, or two such volumes (which one EP147 shows is not known here, so neither). */
static int sdk_stick_mount(int stick, char *mount, size_t cap) {
    if (stick < 0 || stick > 1)
        return 0;
    FILE *f = fopen(SDK_LIB_MOUNTS, "re");
    if (!f)
        return 0;
    char line[512], found[PATH_MAX] = "";
    int hits = 0;
    size_t ml = strlen(SDK_LIB_MEDIA);
    while (fgets(line, sizeof line, f)) {
        char dev[128], mp[2048];
        if (sscanf(line, "%127s %2047s", dev, mp) != 2 || strncmp(dev, "/dev/", 5) != 0 ||
            strncmp(mp, SDK_LIB_MEDIA, ml) != 0 || mp[ml] != '/' || strchr(mp, '\\'))
            continue; /* escaped (unusual) mount points are skipped, not decoded */
        char sys[PATH_MAX], real[PATH_MAX], db[PATH_MAX];
        snprintf(sys, sizeof sys, "%s/%s", SDK_LIB_SYSBLOCK, dev + 5);
        if (!realpath(sys, real) || sdk_port_of_sysfs(real) != stick + 1)
            continue;
        snprintf(db, sizeof db, "%s/%s/rekordbox/exportLibrary.db", mp, pd_dir(mp));
        if (access(db, R_OK) != 0)
            continue;
        if (!hits || strcmp(found, mp) != 0) /* a volume mounted twice is still one */
            hits++;
        snprintf(found, sizeof found, "%s", mp);
    }
    fclose(f);
    if (hits != 1)
        return 0;
    return snprintf(mount, cap, "%s", found) < (int) cap;
}

static int sdk_db_sig(const char *db, long long *sig) {
    struct stat st;
    if (stat(db, &st) != 0)
        return 0;
    *sig = (long long) st.st_size * 1000003LL + (long long) st.st_mtime;
    return 1;
}

static void *sdk_db_open(const char *db) {
    char uri[300];
    if (snprintf(uri, sizeof uri, "file:%s?immutable=1", db) >= (int) sizeof uri)
        return NULL;
    void *h = NULL;
    if (g_sdk_sq.open_v2(uri, &h, 0x01 | 0x40 /* READONLY | URI */, NULL) != 0 || !h) {
        if (h)
            g_sdk_sq.close(h);
        return NULL;
    }
    g_sdk_sq.key(h, ONELIBRARY_KEY_SM, (int) sizeof ONELIBRARY_KEY_SM - 1);
    return h;
}

static void sdk_copy_col(void *st, int col, char *out) {
    const unsigned char *t = g_sdk_sq.col_text(st, col);
    snprintf(out, SDK_LIB_STR, "%s", t ? (const char *) t : "");
}

/* ---- playlists ---- */
struct sdk_pl {
    long long id, parent;
    int folder;
    char name[SDK_LIB_STR];
};

static int sdk_read_playlists(const char *db, struct sdk_pl **out, int *n) {
    void *h = sdk_db_open(db), *st = NULL;
    if (!h)
        return -1;
    int rc = -1, cap = 512, k = 0, trunc = 0;
    struct sdk_pl *v = malloc((size_t) cap * sizeof *v);
    if (v && g_sdk_sq.prepare(h, "SELECT playlist_id, playlist_id_parent, name, attribute FROM playlist "
                                 "ORDER BY playlist_id_parent, sequenceNo",
                              -1, &st, NULL) == 0 &&
        st) {
        int s;
        while ((s = g_sdk_sq.step(st)) == 100 /* SQLITE_ROW */) {
            if (k == cap) {
                if (cap >= SDK_LIB_ROWS) {
                    trunc = 1; /* at most SDK_LIB_ROWS: the copy is made inside EP147 */
                    break;
                }
                int ncap = cap * 2 < SDK_LIB_ROWS ? cap * 2 : SDK_LIB_ROWS;
                struct sdk_pl *nv = realloc(v, (size_t) ncap * sizeof *v);
                if (!nv) {
                    trunc = 1;
                    break;
                }
                v = nv;
                cap = ncap;
            }
            v[k].id = g_sdk_sq.col_int64(st, 0);
            v[k].parent = g_sdk_sq.col_int64(st, 1);
            sdk_copy_col(st, 2, v[k].name);
            v[k].folder = g_sdk_sq.col_int64(st, 3) == PL_ATTR_FOLDER;
            k++;
        }
        if (s == 101 /* SQLITE_DONE */)
            rc = 0;
        else if (trunc)
            rc = 1; /* the first k rows; the rest did not fit */
    }
    if (st)
        g_sdk_sq.finalize(st);
    g_sdk_sq.close(h);
    if (rc < 0) {
        free(v);
        return -1;
    }
    *out = v;
    *n = k;
    return rc;
}

/* The checks both services make before reading: the caller, its thread, its callback, the
 * stick. Returns the mod, or NULL with the reason in its log (cm_self logs its own). */
static cm_mod *sdk_lib_begin(const char *id, uintptr_t ra, const char *what, uintptr_t fn, int stick,
                             char *mount, size_t cap) {
    cm_mod *c = cm_self(id, ra, what);
    if (!c)
        return NULL;
    const char *why = NULL;
    if (g_cm_on_ui)
        why = "called on the UI thread (init, tick, a screen or surface); read from your own thread";
    else if (!fn || !cm_called_by(c, fn))
        why = "the callback is not inside this mod";
    else if (stick < 0 || stick > 1)
        why = "stick is 0 (USB 1) or 1 (USB 2)";
    else if (!sdk_sq_resolve())
        why = "the deck's SQLite is not available";
    if (why) {
        cm_logf(c, 1, "%s refused: %s", what, why);
        return NULL;
    }
    if (!sdk_stick_mount(stick, mount, cap)) {
        cm_logf(c, 0, "%s: no rekordbox library in USB %d", what, stick + 1);
        return NULL;
    }
    return c;
}

static int sdk_lib_playlists(const char *id, int stick,
                             int (*fn)(int pl_id, int parent, const char *name, int folder, void *user),
                             void *user) {
    char mount[PATH_MAX], db[PATH_MAX + 40];
    if (!sdk_lib_begin(id, CM_RA(), "lib_playlists", (uintptr_t) fn, stick, mount, sizeof mount))
        return -1;
    snprintf(db, sizeof db, "%s/%s/rekordbox/exportLibrary.db", mount, pd_dir(mount));
    struct sdk_pl *v = NULL;
    int n = 0, rr = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        long long a, b;
        if (!sdk_db_sig(db, &a))
            return -1;
        if ((rr = sdk_read_playlists(db, &v, &n)) < 0)
            return -1;
        if (sdk_db_sig(db, &b) && a == b)
            break;
        free(v); /* written while we read: once more, then give up */
        v = NULL;
        if (attempt)
            return -1;
    }
    for (int i = 0; i < n; i++)
        if (fn((int) v[i].id, (int) v[i].parent, v[i].name, v[i].folder, user))
            break;
    free(v);
    return rr; /* 1: the list was cut at SDK_LIB_ROWS */
}

/* ---- tracks ---- */
struct sdk_tr {
    long long id, bpm100, len;
    char title[SDK_LIB_STR], artist[SDK_LIB_STR], album[SDK_LIB_STR], key[16], path[SDK_LIB_STR];
};

static int sdk_read_tracks(const char *db, int playlist, struct sdk_tr **out, int *n) {
    static const char *const q_all =
        "SELECT c.content_id, c.title, a.name, al.name, k.name, c.path, c.bpmx100, c.length "
        "FROM content c LEFT JOIN artist a ON a.artist_id = c.artist_id_artist "
        "LEFT JOIN album al ON al.album_id = c.album_id LEFT JOIN key k ON k.key_id = c.key_id "
        "ORDER BY c.content_id";
    static const char *const q_pl =
        "SELECT c.content_id, c.title, a.name, al.name, k.name, c.path, c.bpmx100, c.length "
        "FROM playlist_content pc JOIN content c ON c.content_id = pc.content_id "
        "LEFT JOIN artist a ON a.artist_id = c.artist_id_artist "
        "LEFT JOIN album al ON al.album_id = c.album_id LEFT JOIN key k ON k.key_id = c.key_id "
        "WHERE pc.playlist_id = ?1 ORDER BY pc.sequenceNo";
    void *h = sdk_db_open(db), *st = NULL;
    if (!h)
        return -1;
    int rc = -1, cap = 256, k = 0, trunc = 0;
    struct sdk_tr *v = malloc((size_t) cap * sizeof *v);
    if (v && g_sdk_sq.prepare(h, playlist ? q_pl : q_all, -1, &st, NULL) == 0 && st) {
        if (playlist)
            g_sdk_sq.bind_int64(st, 1, playlist);
        int s;
        while ((s = g_sdk_sq.step(st)) == 100) {
            if (k == cap) {
                if (cap >= SDK_LIB_ROWS) {
                    trunc = 1; /* at most SDK_LIB_ROWS: the copy is made inside EP147 */
                    break;
                }
                int ncap = cap * 2 < SDK_LIB_ROWS ? cap * 2 : SDK_LIB_ROWS;
                struct sdk_tr *nv = realloc(v, (size_t) ncap * sizeof *v);
                if (!nv) {
                    trunc = 1;
                    break;
                }
                v = nv;
                cap = ncap;
            }
            struct sdk_tr *t = &v[k++];
            t->id = g_sdk_sq.col_int64(st, 0);
            sdk_copy_col(st, 1, t->title);
            sdk_copy_col(st, 2, t->artist);
            sdk_copy_col(st, 3, t->album);
            char kb[SDK_LIB_STR];
            sdk_copy_col(st, 4, kb);
            snprintf(t->key, sizeof t->key, "%s", kb);
            sdk_copy_col(st, 5, t->path);
            t->bpm100 = g_sdk_sq.col_int64(st, 6);
            t->len = g_sdk_sq.col_int64(st, 7);
        }
        if (s == 101 /* SQLITE_DONE */)
            rc = 0;
        else if (trunc)
            rc = 1; /* the first k rows; the rest did not fit */
    }
    if (st)
        g_sdk_sq.finalize(st);
    g_sdk_sq.close(h);
    if (rc < 0) {
        free(v);
        return -1;
    }
    *out = v;
    *n = k;
    return rc;
}

static int sdk_lib_tracks(const char *id, int stick, int playlist_id,
                          int (*fn)(const az_track *t, void *user), void *user) {
    char mount[PATH_MAX], db[PATH_MAX + 40];
    if (playlist_id < 0) {
        cm_mod *c = cm_self(id, CM_RA(), "lib_tracks");
        if (c)
            cm_logf(c, 1, "lib_tracks refused: playlist_id %d (0 = every track)", playlist_id);
        return -1;
    }
    if (!sdk_lib_begin(id, CM_RA(), "lib_tracks", (uintptr_t) fn, stick, mount, sizeof mount))
        return -1;
    snprintf(db, sizeof db, "%s/%s/rekordbox/exportLibrary.db", mount, pd_dir(mount));
    struct sdk_tr *v = NULL;
    int n = 0, rr = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        long long a, b;
        if (!sdk_db_sig(db, &a) || (rr = sdk_read_tracks(db, playlist_id, &v, &n)) < 0)
            return -1;
        if (sdk_db_sig(db, &b) && a == b)
            break;
        free(v);
        v = NULL;
        if (attempt)
            return -1;
    }
    for (int i = 0; i < n; i++) {
        char abs[SDK_LIB_STR + 96];
        snprintf(abs, sizeof abs, "%s%s", mount, v[i].path);
        az_track t = {sizeof t, (uint32_t) v[i].id, v[i].title, v[i].artist, v[i].album,
                      v[i].key, abs, (double) v[i].bpm100 / 100.0, (double) v[i].len};
        if (fn(&t, user))
            break;
    }
    free(v);
    return rr; /* 1: the list was cut at SDK_LIB_ROWS */
}

/* ---- lib_spool_kick: the library spool, applied on request ----------------------------------- */
#ifndef SDK_LIB_CURSRC_DIR
#define SDK_LIB_CURSRC_DIR "/tmp"  /* azstem publishes each deck's loaded file there */
#endif
#define SDK_LIB_DECKS 4

/* The file deck d has loaded, as azstem publishes it; "" = none or unknown. */
static void sdk_deck_source(int d, char *out, size_t cap) {
    char p[PATH_MAX];
    out[0] = 0;
    snprintf(p, sizeof p, "%s/azstem.cursrc%d", SDK_LIB_CURSRC_DIR, d + 1);
    FILE *f = fopen(p, "re");
    if (!f)
        return;
    if (!fgets(out, (int) cap, f))
        out[0] = 0;
    fclose(f);
    out[strcspn(out, "\n")] = 0;
}

/* 1 = the volume holding `path` (the SDK_LIB_MEDIA/<name> it sits under) has a *.sql spool */
static int sdk_spool_waits(const char *path) {
    size_t ml = strlen(SDK_LIB_MEDIA);
    if (strncmp(path, SDK_LIB_MEDIA, ml) != 0 || path[ml] != '/')
        return 1; /* not on a stick it knows: assume the worst */
    const char *end = strchr(path + ml + 1, '/');
    if (!end)
        return 1;
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%.*s/PIONEER/azspool", (int) (end - path), path);
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    int any = 0;
    struct dirent *e;
    while (!any && (e = readdir(d))) {
        size_t k = strlen(e->d_name);
        any = e->d_name[0] != '.' && k > 4 && !strcmp(e->d_name + k - 4, ".sql");
    }
    closedir(d);
    return any;
}

/* Sets the spool service's "something may be waiting" flag (library_edits.c ls_tick), the one a
 * copy that stopped waiting sets: the service then looks at every stick every 2 s and applies
 * each stick's spool on that stick's library thread, under its own rules, until none is left.
 * No new write path. The service applies every stick's spool once it runs, so this is refused
 * while a deck plays a track from the asked stick, from any stick with a spool waiting, or from
 * a stick it cannot place: applying changes the library under it. */
static int sdk_lib_spool_kick(const char *id, int stick) {
    cm_mod *c = cm_self(id, CM_RA(), "lib_spool_kick");
    if (!c)
        return -1;
    static long long (*obs)(int);
    if (!obs)
        *(void **) &obs = dlsym(RTLD_DEFAULT, "azstem_play_obs");
    char mount[PATH_MAX];
    const char *why = NULL;
    if (g_cm_on_ui)
        why = "called on the UI thread (init, tick, a screen or surface); call it from your own thread";
    else if (stick < 0 || stick > 1)
        why = "stick is 0 (USB 1) or 1 (USB 2)";
    else if (!obs)
        why = "the decks' playback state is not available";
    else if (!sdk_stick_mount(stick, mount, sizeof mount))
        why = "no rekordbox library in that USB port";
    if (why) {
        cm_logf(c, 1, "lib_spool_kick refused: %s", why);
        return -1;
    }
    long long before[SDK_LIB_DECKS];
    for (int d = 0; d < SDK_LIB_DECKS; d++)
        before[d] = obs(d);
    usleep(250000);
    size_t ml = strlen(mount);
    for (int d = 0; d < SDK_LIB_DECKS; d++) {
        if (before[d] < 0 || obs(d) == before[d])
            continue; /* not playing */
        char src[PATH_MAX];
        sdk_deck_source(d, src, sizeof src);
        if (!src[0] || (strncmp(src, mount, ml) == 0 && src[ml] == '/') || sdk_spool_waits(src)) {
            cm_logf(c, 1, "lib_spool_kick refused: deck %d is playing a track %s", d + 1,
                    !src[0]                        ? "from a USB it cannot place"
                    : strncmp(src, mount, ml) == 0 ? "from that USB"
                                                   : "from a USB with a spool waiting");
            return -1;
        }
    }
    __atomic_store_n(&g_ls_leftover, 1, __ATOMIC_RELEASE);
    cm_logf(c, 0, "lib_spool_kick: USB %d's library spool will be applied", stick + 1);
    return 0;
}
