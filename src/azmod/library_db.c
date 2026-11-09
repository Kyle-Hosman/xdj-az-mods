/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file library_db.c
 * @brief SQLite helpers for the library mods, and the per-stick MusicLibraryServer chain check.
 *
 * EP147 already links SQLite (SQLCipher), so sq_resolve() binds the sqlite3_* entry points
 * with dlsym(RTLD_DEFAULT) on first use. The wrappers run prepared statements with integer
 * parameters on a connection the caller already holds. There is one MusicLibraryServer per
 * stick, each on its own library thread; see mlserver_chain(). No state beyond g_sq.
 */
static int sq_resolve(void) {
    if (g_sq.get_autocommit)
        return 1;
    *(void **) &g_sq.prepare = dlsym(RTLD_DEFAULT, "sqlite3_prepare_v2");
    *(void **) &g_sq.bind_int64 = dlsym(RTLD_DEFAULT, "sqlite3_bind_int64");
    *(void **) &g_sq.step = dlsym(RTLD_DEFAULT, "sqlite3_step");
    *(void **) &g_sq.column_int64 = dlsym(RTLD_DEFAULT, "sqlite3_column_int64");
    *(void **) &g_sq.finalize = dlsym(RTLD_DEFAULT, "sqlite3_finalize");
    *(void **) &g_sq.column_text = dlsym(RTLD_DEFAULT, "sqlite3_column_text");
    void *ac = dlsym(RTLD_DEFAULT, "sqlite3_get_autocommit");
    if (!g_sq.prepare || !g_sq.bind_int64 || !g_sq.step || !g_sq.column_int64 || !g_sq.finalize ||
        !g_sq.column_text || !ac)
        return 0;
    *(void **) &g_sq.get_autocommit = ac; /* last: it is the "resolved" flag */
    return 1;
}
static int sq_row(void *conn, const char *sql, long long p1, long long p2, long long *out,
                  int ncol) {
    void *st = 0;
    if (g_sq.prepare(conn, sql, -1, &st, 0) != 0 || !st)
        return 0;
    g_sq.bind_int64(st, 1, p1);
    if (p2 >= 0)
        g_sq.bind_int64(st, 2, p2);
    int got = 0;
    if (g_sq.step(st) == SQLITE_ROW) {
        for (int i = 0; i < ncol; i++)
            out[i] = g_sq.column_int64(st, i);
        got = ncol;
    }
    g_sq.finalize(st);
    return got;
}
/* One TEXT column of one row, one integer parameter. 1 = got it (NUL-terminated,
 * truncated to cap), 0 = no row / NULL / error. */
static int sq_text(void *conn, const char *sql, long long p1, char *out, size_t cap) {
    void *st = 0;
    int got = 0;
    if (g_sq.prepare(conn, sql, -1, &st, 0) != 0 || !st)
        return 0;
    g_sq.bind_int64(st, 1, p1);
    if (g_sq.step(st) == SQLITE_ROW) {
        const unsigned char *t = g_sq.column_text(st, 0);
        if (t) {
            size_t k = 0;
            while (t[k] && k + 1 < cap) {
                out[k] = (char) t[k];
                k++;
            }
            out[k] = 0;
            if (t[k])
                utf8_cut(out); /* cut short: never half a character */
            got = 1;
        }
    }
    g_sq.finalize(st);
    return got;
}
/* "/media/usb/sdb1/PIONEER/rekordbox/exportLibrary.db" -> "/media/usb/sdb1" (or .PIONEER) */
static int cp_mount_of(void *conn, char *out, size_t n) {
    static const char *(*fn)(void *, const char *);
    if (!fn)
        *(void **) &fn = dlsym(RTLD_DEFAULT, "sqlite3_db_filename");
    out[0] = 0;
    const char *d = fn ? fn(conn, "main") : 0;
    const char *cut = d ? pd_cut(d, "rekordbox/") : 0;
    if (!cut || cut - d >= (long) n)
        return 0;
    snprintf(out, n, "%.*s", (int) (cut - d), d);
    return out[0] == '/';
}
/* One MusicLibraryServer per stick, each on its own library thread (the SqliteMountPoint
 * ctor 0x1297700 receives the media id; each server's [[disp+0x20]] is its own mount point).
 * The servers are separate heap objects, not an array: never walk from one to a neighbor,
 * or one stick's writer runs on another stick's thread. A request is taken only by the
 * server whose own database carries its media (mlserver_onmsg_hook). Every link is
 * vtable-checked at use. Returns 0 and fills svc/lib/db/media, or a negative code naming
 * the link that failed (the refusal codes mv_execute reports). Only ever called with the
 * server whose onMessage is running, on that server's thread: every link is a member of the
 * live server, read with plain loads (own_q). */
static int mlserver_chain(void *server, uintptr_t *svc_o, uintptr_t *lib_o, uintptr_t *db_o,
                          unsigned *media_o) {
    uintptr_t vt = 0, disp = 0, h = 0, svc = 0, lib = 0, db = 0, med = 0;
    if (!own_q((uintptr_t) server, &vt) || vt != MLSERVER_VPTR)
        return -1;
    if (!own_q((uintptr_t) server + MLSERVER_DISP, &disp) || !disp)
        return -2;
    if (!own_q(disp + MLDISP_HANDLER, &h) || !h)
        return -2;
    if (!own_q(h + MLHANDLER_INFOUPD, &svc) || !svc || !own_q(svc, &vt) || vt != INFOUPD_VPTR)
        return -3;
    if (!own_q(svc + INFOUPD_LIB, &lib) || !lib || !own_q(lib + MLLIB_SQLDB, &db) || !db ||
        !own_q(db, &vt) || vt != SQLDB_VPTR)
        return -4;
    if (!own_q(db + SQLDB_MEDIA_Q, &med) || !(med & 0xffff))
        return -5;
    *svc_o = svc;
    *lib_o = lib;
    *db_o = db;
    *media_o = (unsigned) (med & 0xffff);
    return 0;
}
/* One write with up to four integer parameters: 0 = SQLITE_DONE. */
static int sq_exec4(void *conn, const char *sql, long long a, long long b, long long c,
                    long long d) {
    void *st = 0;
    if (g_sq.prepare(conn, sql, -1, &st, 0) != 0 || !st)
        return -1;
    long long v[4] = {a, b, c, d};
    for (int i = 0; i < 4; i++)
        g_sq.bind_int64(st, i + 1, v[i]); /* past the last ?N: SQLITE_RANGE, ignored */
    int rc = g_sq.step(st);
    g_sq.finalize(st);
    return rc == 101 ? 0 : rc;
}
static int lp_exec(void *conn, const char *sql, FILE *lg) {
    void *st = 0;
    int rc = g_sq.prepare(conn, sql, -1, &st, 0);
    if (rc == 0 && st) {
        while ((rc = g_sq.step(st)) == SQLITE_ROW) {
        }
        rc = (rc == 101) ? 0 : rc;
    }
    if (st)
        g_sq.finalize(st);
    if (lg)
        fprintf(lg, "  exec rc=%d: %s\n", rc, sql);
    return rc;
}
