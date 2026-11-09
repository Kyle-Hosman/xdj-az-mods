/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file library_edits.c
 * @brief On-stick library editing: cross-stick copy, playlist edits with undo, library backup, and the library spool.
 *
 * Every database change runs on the target stick's library thread, on EP147's own
 * SQLite connection (no eject). The message thread records one pending operation
 * (MV_OP_*) and mv_execute() runs it there. Journaled operations push an entry onto an
 * undo stack that jr_undo() replays in reverse. After a write, lib_invalidate() drops
 * EP147's list caches so the open screen re-reads in place.
 *
 * Cross-stick copy: a SCHED_IDLE worker copies audio, ANLZ files and artwork to the
 * target stick, then writes the rows as an SQL spool in <stick>/PIONEER/azspool/ that
 * ls_service() applies on that stick's library thread. lib_backup() keeps paired
 * database and export.pdb backups before the first write after each insertion; a
 * library that fails PRAGMA quick_check is never copied over a backup, and editing is
 * refused.
 *
 * Entry points: cp_request(), cp_request_tagged(), cp_request_playlist(), mv_execute(),
 * mv_writes(), ls_service().
 */
/* ---- cross-stick copy ------------------------------------
 * Copies a track (or a playlist, or the Tag List) to another stick: the files
 * (audio, its three ANLZ files, which carry the cues, and artwork), then the
 * library rows through the target stick's spool.
 *   1. message thread arms MV_OP_CPREAD for the source stick;
 *   2. the source's library thread reads the rows (cp_read, read-only);
 *   3. a SCHED_IDLE worker copies each file to "<dst>.azcp" and renames it (a
 *      reader never sees half a file), writes the spool (.tmp + rename) and
 *      waits for the spool service to apply it;
 *   4. the message thread tells the DJ.
 * Rules: the audio keeps its path, and an audio file already on the target with a
 * different size refuses the copy (never overwrite, never guess). ANLZ files that
 * clash move to a fresh folder (cp_copy_track). Artwork goes under
 * /PIONEER/Artwork/azcp/ with the source media in the name, since artwork names are
 * per-stick sequences and would collide. NULL fields stay NULL: the source row is
 * rekordbox's own, so its shape is valid. */
enum {
    CPF_TITLE,
    CPF_TFS,
    CPF_SUB,
    CPF_BPM,
    CPF_LEN,
    CPF_TRACKNO,
    CPF_DISCNO,
    CPF_ART,
    CPF_REMIX,
    CPF_ORIG,
    CPF_COMP,
    CPF_LYR,
    CPF_ALBUM,
    CPF_ALBART,
    CPF_ALBIMG,
    CPF_GENRE,
    CPF_LABEL,
    CPF_KEY,
    CPF_COLOR,
    CPF_IMG,
    CPF_COMMENT,
    CPF_RATING,
    CPF_YEAR,
    CPF_RELDATE,
    CPF_CREATED,
    CPF_ADDED,
    CPF_PATH,
    CPF_FNAME,
    CPF_FSIZE,
    CPF_FTYPE,
    CPF_BITRATE,
    CPF_BITDEPTH,
    CPF_SRATE,
    CPF_ISRC,
    CPF_HCAUTO,
    CPF_ANLZ,
    CPF_ABITS,
    CPF_MDB,
    CPF_MCID,
    CPF_CLINK,
    CPF_N
};
static const char CP_SELECT[] =
    "SELECT c.title,c.titleForSearch,c.subtitle,c.bpmx100,c.length,c.trackNo,c.discNo,"
    "a1.name,a2.name,a3.name,a4.name,a5.name,al.name,aa.name,ai.path,g.name,l.name,k.name,"
    "c.color_id,im.path,c.djComment,c.rating,c.releaseYear,c.releaseDate,c.dateCreated,c.dateAdded,"
    "c.path,c.fileName,c.fileSize,c.fileType,c.bitrate,c.bitDepth,c.samplingRate,c.isrc,"
    "c.isHotCueAutoLoadOn,c.analysisDataFilePath,c.analysedBits,c.masterDbId,c.masterContentId,c."
    "contentLink FROM content c "
    "LEFT JOIN artist a1 ON a1.artist_id=c.artist_id_artist LEFT JOIN artist a2 ON "
    "a2.artist_id=c.artist_id_remixer "
    "LEFT JOIN artist a3 ON a3.artist_id=c.artist_id_originalArtist LEFT JOIN artist a4 ON "
    "a4.artist_id=c.artist_id_composer "
    "LEFT JOIN artist a5 ON a5.artist_id=c.artist_id_lyricist LEFT JOIN album al ON "
    "al.album_id=c.album_id "
    "LEFT JOIN artist aa ON aa.artist_id=al.artist_id LEFT JOIN image ai ON "
    "ai.image_id=al.image_id "
    "LEFT JOIN genre g ON g.genre_id=c.genre_id LEFT JOIN label l ON l.label_id=c.label_id "
    "LEFT JOIN key k ON k.key_id=c.key_id LEFT JOIN image im ON im.image_id=c.image_id "
    "WHERE c.content_id=?1";
enum {
    CP_IDLE,
    CP_COPYING,
    CP_DONE
};
enum {
    CPK_TRACK,
    CPK_PLAYLIST,
    CPK_TAGGED /* the Tag List's tracks (g_tag_cids) into one playlist */,
    CPK_FOLDER /* a folder: every folder and list inside it, and every list's tracks */
};
#define CP_MAX_TRACKS 300 /* one spool = one transaction; ~3 KB SQL per track, under its 1 MB cap  \
                           */
struct cp_trk {
    char f[CPF_N][256];
    unsigned char isnull[CPF_N];
    char img_dst[200], albimg_dst[200];
    char anlz_dst[256]; /* the .DAT path for content when the source's is taken on the target */
    int node;           /* CPK_FOLDER: the g_cp.node entry (a list) this track goes into */
};
/* CPK_FOLDER: one row of the copied tree. Entry 0 is the folder itself; the others follow
 * breadth-first, so a parent always comes before its children. */
struct cp_node {
    char name[PL_TGT_NAME_MAX];
    int parent; /* index of the parent entry; -1 for entry 0 */
    int folder; /* 1 a folder, 0 a list */
    int seq;    /* 1..N among its siblings, in the source's display order */
};
static struct {
    _Atomic int state;
    int kind;
    unsigned src_media, cid, src_pl, dst_pl, dst_attr;
    char dst_name[PL_TGT_NAME_MAX], src_pl_name[PL_TGT_NAME_MAX], src_mount[128], dst_mount[128];
    struct cp_trk *trk; /* heap, filled on the source's library thread */
    int ntrk;
    struct cp_node *node; /* CPK_FOLDER: heap, filled with trk */
    int nnode;
    _Atomic int done_n; /* tracks whose files are copied (progress) */
    int rc;
} g_cp;
/* Some stick may hold a spool no copy is waiting for: one left by a copy that gave up
 * waiting (-58), or by an EP147 restart before it was applied. 1 at start, so the first
 * spool scan looks once; ls_tick clears it when no spool is left. */
static _Atomic int g_ls_leftover = 1;
/* source library thread: one track's row + names into t (read only). */
static int cp_read_one(uintptr_t conn, unsigned cid, struct cp_trk *t) {
    static int (*coltype)(void *, int);
    if (!coltype)
        *(void **) &coltype = dlsym(RTLD_DEFAULT, "sqlite3_column_type");
    if (!coltype)
        return -40;
    void *st = 0;
    int rc = -42;
    if (g_sq.prepare((void *) conn, CP_SELECT, -1, &st, 0) != 0 || !st)
        return -42;
    g_sq.bind_int64(st, 1, cid);
    if (g_sq.step(st) == SQLITE_ROW) {
        for (int i = 0; i < CPF_N; i++) {
            t->isnull[i] = coltype(st, i) == 5; /* SQLITE_NULL */
            const unsigned char *x = g_sq.column_text(st, i);
            snprintf(t->f[i], sizeof t->f[i], "%s", (!t->isnull[i] && x) ? (const char *) x : "");
        }
        rc = t->f[CPF_PATH][0] == '/' ? 0 : -43;
    }
    g_sq.finalize(st);
    return rc;
}
/* source library thread, CPK_FOLDER: the folder's tree into g_cp.node (breadth-first, each
 * level in EP147's display order: sequenceNo, then playlist_id) and every list's tracks, in
 * order, into cids, with tnode[k] = the entry track k goes into. Folders and plain lists only:
 * any other kind of list holds no playlist_content to copy, so the copy is refused whole
 * rather than arriving without it. An empty folder or list is copied as it is. */
static int cp_read_folder(uintptr_t conn, unsigned *cids, int *tnode, int *n_out) {
    long long at[1] = {0};
    if (sq_row((void *) conn, "SELECT attribute FROM playlist WHERE playlist_id = ?1", g_cp.src_pl,
               -1, at, 1) != 1)
        return -44;
    if (at[0] != PL_ATTR_FOLDER)
        return -45;
    if (!sq_text((void *) conn, "SELECT name FROM playlist WHERE playlist_id = ?1", g_cp.src_pl,
                 g_cp.src_pl_name, sizeof g_cp.src_pl_name) ||
        !g_cp.src_pl_name[0])
        return -44;
    struct cp_node *nd = calloc(FD_MAX, sizeof *nd);
    unsigned *src = calloc(FD_MAX, sizeof *src);
    if (!nd || !src) {
        free(nd);
        free(src);
        return -48;
    }
    int nn = 1, n = 0, rc = 0;
    src[0] = g_cp.src_pl;
    nd[0].parent = -1;
    nd[0].folder = 1;
    utf8_copy(nd[0].name, sizeof nd[0].name, g_cp.src_pl_name);
    for (int q = 0; q < nn && !rc; q++) {
        void *st = 0;
        if (g_sq.prepare((void *) conn,
                         nd[q].folder
                             ? "SELECT playlist_id, attribute, name FROM playlist WHERE "
                               "playlist_id_parent = ?1 ORDER BY sequenceNo, playlist_id"
                             : "SELECT content_id FROM playlist_content WHERE playlist_id = ?1 "
                               "ORDER BY sequenceNo",
                         -1, &st, 0) != 0 ||
            !st) {
            rc = -42;
            break;
        }
        g_sq.bind_int64(st, 1, src[q]);
        int seq = 0;
        while (!rc && g_sq.step(st) == SQLITE_ROW) {
            if (!nd[q].folder) {
                if (n >= CP_MAX_TRACKS)
                    rc = -46;
                else {
                    cids[n] = (unsigned) g_sq.column_int64(st, 0);
                    tnode[n++] = q;
                }
                continue;
            }
            long long a = g_sq.column_int64(st, 1);
            if (a != PL_ATTR_LIST && a != PL_ATTR_FOLDER)
                rc = -45;
            else if (nn >= FD_MAX)
                rc = -49;
            else {
                const unsigned char *x = g_sq.column_text(st, 2);
                src[nn] = (unsigned) g_sq.column_int64(st, 0);
                nd[nn].parent = q;
                nd[nn].folder = a == PL_ATTR_FOLDER;
                nd[nn].seq = ++seq;
                utf8_copy(nd[nn].name, sizeof nd[nn].name, x ? (const char *) x : "");
                nn++;
            }
        }
        g_sq.finalize(st);
    }
    free(src);
    if (rc) {
        free(nd);
        return rc;
    }
    g_cp.node = nd;
    g_cp.nnode = nn;
    *n_out = n;
    return 0;
}
/* source library thread: the track, or the playlist's name + every track in order, or a
 * folder's whole tree (cp_read_folder). */
static int cp_read(uintptr_t conn, FILE *lg) {
    if (!cp_mount_of((void *) conn, g_cp.src_mount, sizeof g_cp.src_mount))
        return -41;
    unsigned cids[CP_MAX_TRACKS];
    int tnode[CP_MAX_TRACKS];
    int n = 0;
    if (g_cp.kind == CPK_FOLDER) {
        int rc = cp_read_folder(conn, cids, tnode, &n);
        if (rc)
            return rc;
    } else if (g_cp.kind == CPK_TRACK)
        cids[n++] = g_cp.cid;
    else if (g_cp.kind == CPK_TAGGED) { /* read at COPY time on this same stick */
        for (int i = 0; i < g_tag_n && n < CP_MAX_TRACKS; i++)
            cids[n++] = g_tag_cids[i];
        if (!n)
            return -47;
    } else {
        long long at[1] = {0};
        if (sq_row((void *) conn, "SELECT attribute FROM playlist WHERE playlist_id = ?1",
                   g_cp.src_pl, -1, at, 1) != 1)
            return -44;
        if (at[0] != PL_ATTR_LIST)
            return -45; /* a folder / smart list: not a list of tracks */
        if (!sq_text((void *) conn, "SELECT name FROM playlist WHERE playlist_id = ?1", g_cp.src_pl,
                     g_cp.src_pl_name, sizeof g_cp.src_pl_name) ||
            !g_cp.src_pl_name[0])
            return -44;
        void *st = 0;
        if (g_sq.prepare(
                (void *) conn,
                "SELECT content_id FROM playlist_content WHERE playlist_id=?1 ORDER BY sequenceNo",
                -1, &st, 0) != 0 ||
            !st)
            return -42;
        g_sq.bind_int64(st, 1, g_cp.src_pl);
        int over = 0;
        while (g_sq.step(st) == SQLITE_ROW) {
            if (n < CP_MAX_TRACKS)
                cids[n++] = (unsigned) g_sq.column_int64(st, 0);
            else
                over = 1;
        }
        g_sq.finalize(st);
        if (over)
            return -46;
        if (!n)
            return -47;
    }
    g_cp.trk = calloc((size_t) (n ? n : 1), sizeof *g_cp.trk); /* a folder may hold no track */
    int rc = g_cp.trk ? 0 : -48;
    for (int k = 0; k < n && !rc; k++) {
        rc = cp_read_one(conn, cids[k], &g_cp.trk[k]);
        g_cp.trk[k].node = g_cp.kind == CPK_FOLDER ? tnode[k] : 0;
        if (rc && lg)
            fprintf(lg, "CPREAD cid=%u rc=%d\n", cids[k], rc);
    }
    if (rc) {
        free(g_cp.trk);
        g_cp.trk = 0;
        free(g_cp.node);
        g_cp.node = 0;
        g_cp.nnode = 0;
        return rc;
    }
    g_cp.ntrk = n;
    if (lg)
        fprintf(lg, "CPREAD kind=%d n=%d nodes=%d src=%s pl=%u \"%s\"\n", g_cp.kind, n, g_cp.nnode,
                g_cp.src_mount, g_cp.src_pl, g_cp.src_pl_name);
    return 0;
}
static int cp_mkdirs(const char *path) { /* mkdir -p of path's directory */
    char b[512];
    snprintf(b, sizeof b, "%s", path);
    for (char *p = b + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(b, 0777);
            *p = '/';
        }
    return 0;
}
/* 0 copied or already there (same size); <0 refused/failed. Never overwrites. */
static int cp_file(const char *src, const char *dst) {
    struct stat ss, ds;
    if (stat(src, &ss) != 0)
        return -1;
    if (stat(dst, &ds) == 0)
        return ds.st_size == ss.st_size ? 0 : -2;
    cp_mkdirs(dst);
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.azcp", dst);
    int in = open(src, O_RDONLY | O_CLOEXEC),
        out = in >= 0 ? open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666) : -1;
    if (in < 0 || out < 0) {
        if (in >= 0)
            close(in);
        return -3;
    }
    static char buf[1 << 16];
    ssize_t r;
    int ok = 1;
    while ((r = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t) r) != r) {
            ok = 0;
            break;
        }
    if (r < 0)
        ok = 0;
    if (fsync(out) != 0)
        ok = 0;
    close(in);
    close(out);
    if (!ok || rename(tmp, dst) != 0) {
        unlink(tmp);
        return -4;
    }
    return 0;
}
/* Byte comparison for analysis files: two tracks' ANLZ0000.DAT can share a size, and
 * reusing the target's would give the copy another track's waveform, beatgrid and cues. */
static int cp_same_bytes(const char *a, const char *b) {
    struct stat sa, sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0 || sa.st_size != sb.st_size)
        return 0;
    FILE *fa = fopen(a, "rbe"), *fb = fa ? fopen(b, "rbe") : 0;
    int same = fa && fb;
    static char ba[1 << 15], bb[1 << 15];
    size_t na, nb;
    while (same && (na = fread(ba, 1, sizeof ba, fa)) > 0) {
        nb = fread(bb, 1, sizeof bb, fb);
        same = na == nb && !memcmp(ba, bb, na);
    }
    if (fa)
        fclose(fa);
    if (fb)
        fclose(fb);
    return same;
}
/* SQL literal for a spool line: NULL, or quoted once; ';' -> char(59); control bytes but tab ->
 * ' ' (a line break would split the line, and ls_apply_buf rejects a line holding 0x01). */
static void cp_lit(char **o, char *end, const char *v, int isnull) {
    if (isnull) {
        int n = snprintf(*o, end - *o, "NULL");
        *o += n > 0 && *o + n < end ? n : 0;
        return;
    }
    if (*o < end)
        *(*o)++ = '\'';
    for (const char *p = v; *p && *o < end - 16; p++) {
        if (*p == '\'') {
            *(*o)++ = '\'';
            *(*o)++ = '\'';
        } else if (*p == ';') {
            memcpy(*o, "'||char(59)||'", 14);
            *o += 14;
        } else if ((unsigned char) *p < 0x20 && *p != '\t') /* lines, and the applier's 0x01 mark */
            *(*o)++ = ' ';
        else
            *(*o)++ = *p;
    }
    if (*o < end)
        *(*o)++ = '\'';
}
static void cp_put(char **o, char *end, const char *s) {
    size_t n = strlen(s);
    if (*o + n < end) {
        memcpy(*o, s, n);
        *o += n;
    }
}
/* the artwork path on the target: /PIONEER/Artwork/azcp/<srcmedia>_<name> */
static void cp_art_dst(const char *src, char *out, size_t n) {
    const char *b = strrchr(src, '/');
    b = b ? b + 1 : src;
    snprintf(out, n, "/PIONEER/Artwork/azcp/%04x_%s", g_cp.src_media, b);
}
/* "<x>.jpg" -> "<x>_m.jpg" (EP147's larger copy, stored beside it) */
static void cp_m_variant(const char *p, char *out, size_t n) {
    const char *dot = strrchr(p, '.');
    if (!dot) {
        snprintf(out, n, "%s_m", p);
        return;
    }
    snprintf(out, n, "%.*s_m%s", (int) (dot - p), p, dot);
}
/* "-- AZMOD art\t<content path>\t<art path>": a spool comment naming the art a copy wrote
 * for a track (cp_track_sql, ls_art_fix) */
#define LS_ART_TAG "-- AZMOD art\t"
/* One track's rows (artists, refs, images, album, content) + its playlist_content
 * row into playlist `plsel` (an SQL expression naming the destination playlist).
 * dedupe 1: no row when the track is already in that playlist (a track pasted onto an
 * existing list). dedupe 0: always a row, for the new list a pasted playlist becomes,
 * which must hold every entry of the source, repeats included. */
static char *cp_track_sql(char *o, char *end, const struct cp_trk *t, const char *plsel,
                          const char *plguard, int dedupe) {
    const char *img_dst = t->img_dst, *albimg_dst = t->albimg_dst;
#define LIT(i) cp_lit(&o, end, t->f[i], t->isnull[i])
#define PUT(s) cp_put(&o, end, s)
    const int names[] = {CPF_ART, CPF_REMIX, CPF_ORIG, CPF_COMP, CPF_LYR, CPF_ALBART};
    for (unsigned k = 0; k < sizeof names / sizeof names[0]; k++) {
        int i = names[k];
        if (t->isnull[i] || !t->f[i][0])
            continue;
        PUT("INSERT INTO artist (artist_id,name) SELECT (SELECT COALESCE(MAX(artist_id),0)+1 FROM "
            "artist),");
        LIT(i);
        PUT(" WHERE NOT EXISTS (SELECT 1 FROM artist WHERE name=");
        LIT(i);
        PUT(");\n");
    }
    struct {
        int f;
        const char *t, *id;
    } refs[] = {{CPF_GENRE, "genre", "genre_id"},
                {CPF_LABEL, "label", "label_id"},
                {CPF_KEY, "key", "key_id"}};
    for (unsigned k = 0; k < 3; k++) {
        int i = refs[k].f;
        if (t->isnull[i] || !t->f[i][0])
            continue;
        char h[160];
        snprintf(h, sizeof h,
                 "INSERT INTO %s (%s,name) SELECT (SELECT COALESCE(MAX(%s),0)+1 FROM %s),",
                 refs[k].t, refs[k].id, refs[k].id, refs[k].t);
        PUT(h);
        LIT(i);
        snprintf(h, sizeof h, " WHERE NOT EXISTS (SELECT 1 FROM %s WHERE name=", refs[k].t);
        PUT(h);
        LIT(i);
        PUT(");\n");
    }
    const char *imgs[2] = {img_dst, albimg_dst};
    for (int k = 0; k < 2; k++) {
        if (!imgs[k] || !imgs[k][0])
            continue;
        PUT("INSERT INTO image (image_id,path) SELECT (SELECT COALESCE(MAX(image_id),0)+1 FROM "
            "image),");
        cp_lit(&o, end, imgs[k], 0);
        PUT(" WHERE NOT EXISTS (SELECT 1 FROM image WHERE path=");
        cp_lit(&o, end, imgs[k], 0);
        PUT(");\n");
    }
/* subqueries by natural key: ids are computed on the target at apply time */
#define SUBQ(tbl, idc, col, i)                                  \
    do {                                                        \
        if (t->isnull[i] || !t->f[i][0])                        \
            PUT("NULL");                                        \
        else {                                                  \
            PUT("(SELECT " idc " FROM " tbl " WHERE " col "="); \
            LIT(i);                                             \
            PUT(" LIMIT 1)");                                   \
        }                                                       \
    } while (0)
#define SUBQ_S(tbl, idc, col, str)                              \
    do {                                                        \
        if (!(str) || !(str)[0])                                \
            PUT("NULL");                                        \
        else {                                                  \
            PUT("(SELECT " idc " FROM " tbl " WHERE " col "="); \
            cp_lit(&o, end, str, 0);                            \
            PUT(" LIMIT 1)");                                   \
        }                                                       \
    } while (0)
    if (!t->isnull[CPF_ALBUM] && t->f[CPF_ALBUM][0]) {
        PUT("INSERT INTO album (album_id,name,artist_id,image_id,isComplation) SELECT (SELECT "
            "COALESCE(MAX(album_id),0)+1 FROM album),");
        LIT(CPF_ALBUM);
        PUT(",");
        SUBQ("artist", "artist_id", "name", CPF_ALBART);
        PUT(",");
        SUBQ_S("image", "image_id", "path", albimg_dst);
        PUT(",0 WHERE NOT EXISTS (SELECT 1 FROM album WHERE name=");
        LIT(CPF_ALBUM);
        PUT(");\n");
    }
    PUT("INSERT INTO content "
        "(content_id,title,titleForSearch,subtitle,bpmx100,length,trackNo,discNo,"
        "artist_id_artist,artist_id_remixer,artist_id_originalArtist,artist_id_composer,artist_id_"
        "lyricist,"
        "album_id,genre_id,label_id,key_id,color_id,image_id,djComment,rating,releaseYear,"
        "releaseDate,"
        "dateCreated,dateAdded,path,fileName,fileSize,fileType,bitrate,bitDepth,samplingRate,isrc,"
        "isHotCueAutoLoadOn,analysisDataFilePath,analysedBits,masterDbId,masterContentId,"
        "contentLink) SELECT (SELECT COALESCE(MAX(content_id),0)+1 FROM content),");
    const int plain1[] = {CPF_TITLE, CPF_TFS, CPF_SUB, CPF_BPM, CPF_LEN, CPF_TRACKNO, CPF_DISCNO};
    for (unsigned k = 0; k < 7; k++) {
        LIT(plain1[k]);
        PUT(",");
    }
    SUBQ("artist", "artist_id", "name", CPF_ART);
    PUT(",");
    SUBQ("artist", "artist_id", "name", CPF_REMIX);
    PUT(",");
    SUBQ("artist", "artist_id", "name", CPF_ORIG);
    PUT(",");
    SUBQ("artist", "artist_id", "name", CPF_COMP);
    PUT(",");
    SUBQ("artist", "artist_id", "name", CPF_LYR);
    PUT(",");
    SUBQ("album", "album_id", "name", CPF_ALBUM);
    PUT(",");
    SUBQ("genre", "genre_id", "name", CPF_GENRE);
    PUT(",");
    SUBQ("label", "label_id", "name", CPF_LABEL);
    PUT(",");
    SUBQ("key", "key_id", "name", CPF_KEY);
    PUT(",");
    LIT(CPF_COLOR);
    PUT(",");
    SUBQ_S("image", "image_id", "path", img_dst);
    PUT(",");
    /* Copied as-is. CPF_ABITS (analysedBits): left NULL, rekordbox calls the track
     * "analyzed by an older version". CPF_MDB/CPF_MCID are rekordbox's link
     * to its collection, so Import Playlist matches the track instead of copying it. */
    /* clang-format off */
    const int plain2[] = {
        CPF_COMMENT, CPF_RATING, CPF_YEAR,  CPF_RELDATE, CPF_CREATED,  CPF_ADDED, CPF_PATH,
        CPF_FNAME,   CPF_FSIZE,  CPF_FTYPE, CPF_BITRATE, CPF_BITDEPTH, CPF_SRATE, CPF_ISRC,
        CPF_HCAUTO,  CPF_ANLZ,   CPF_ABITS, CPF_MDB,     CPF_MCID,
        CPF_CLINK};
    /* clang-format on */
    for (unsigned k = 0; k < sizeof plain2 / sizeof plain2[0]; k++) {
        if (plain2[k] == CPF_ANLZ && t->anlz_dst[0])
            cp_lit(&o, end, t->anlz_dst, 0); /* moved: see cp_copy_track */
        else
            LIT(plain2[k]);
        if (k + 1 < sizeof plain2 / sizeof plain2[0])
            PUT(",");
    }
    PUT(" WHERE NOT EXISTS (SELECT 1 FROM content WHERE path=");
    LIT(CPF_PATH);
    PUT(");\n");
    /* the content row above is new only for a path the target lacks; for one it has, this
     * line hands the copied art to ls_art_fix. A tab or line break would split it: no line. */
    if (img_dst[0] && !strpbrk(t->f[CPF_PATH], "\t\r\n\x01") && !strpbrk(img_dst, "\t\r\n\x01")) {
        PUT(LS_ART_TAG);
        PUT(t->f[CPF_PATH]);
        PUT("\t");
        PUT(img_dst);
        PUT("\n");
    }
    PUT("INSERT INTO playlist_content (playlist_id,content_id,sequenceNo) SELECT p.playlist_id,"
        "(SELECT content_id FROM content WHERE path=");
    LIT(CPF_PATH);
    PUT(" LIMIT 1),(SELECT COALESCE(MAX(sequenceNo),0)+1 FROM playlist_content WHERE "
        "playlist_id=p.playlist_id) "
        "FROM playlist p WHERE p.playlist_id=");
    PUT(plsel);
    PUT(" AND ");
    PUT(plguard);
    if (dedupe) {
        PUT(" AND NOT EXISTS (SELECT 1 FROM playlist_content pc WHERE pc.playlist_id=p.playlist_id "
            "AND pc.content_id="
            "(SELECT content_id FROM content WHERE path=");
        LIT(CPF_PATH);
        PUT(" LIMIT 1))");
    }
    PUT(";\n");
#undef LIT
#undef PUT
#undef SUBQ
#undef SUBQ_S
    return o;
}
/* A new playlist row's id on the target: past every existing one and never below 16385, the
 * range EP147's own new playlists use. */
#define CP_NEW_PLID "(SELECT MAX(16384,COALESCE(MAX(playlist_id),0))+1 FROM playlist)"
/* CPK_FOLDER: every folder and list inside the copied folder (g_cp.node 1..nnode-1, parents
 * first), under the new row of its parent. Entry i carries image_id mark - i until the
 * spool's last statement, so each row finds its parent by that mark alone: a row whose
 * parent was not written is not written either. Names are the source's (a new folder has no
 * other children to clash with). */
static char *cp_folder_sql(char *o, char *end, long long mark) {
    char q[200];
    for (int i = 1; i < g_cp.nnode; i++) {
        const struct cp_node *nd = &g_cp.node[i];
        cp_put(&o, end,
               "INSERT INTO playlist "
               "(playlist_id,sequenceNo,name,image_id,attribute,playlist_id_parent) SELECT " CP_NEW_PLID
               ",");
        snprintf(q, sizeof q, "%d,", nd->seq);
        cp_put(&o, end, q);
        cp_lit(&o, end, nd->name, 0);
        snprintf(q, sizeof q, ",%lld,%d,p.playlist_id FROM playlist p WHERE p.image_id=%lld;\n",
                 mark - i, nd->folder ? PL_ATTR_FOLDER : PL_ATTR_LIST, mark - nd->parent);
        cp_put(&o, end, q);
    }
    return o;
}
/* The whole spool: the destination playlist (created inside a target FOLDER, or the
 * target list itself), then every track in order. One file = one transaction.
 * A pasted PLAYLIST is always a new list, named like the source or "<name> (1)",
 * "(2)" ... when that name is taken at that level, as rekordbox's device import does
 * (it never merges two lists). The free name is picked inside the spool's own
 * transaction, since only the destination knows its names. The new row carries a
 * negative image_id unique to this spool so the track rows find exactly it; the last
 * statement resets image_id to 0. No free name, or the folder gone: no list and no
 * playlist_content rows are written. A pasted FOLDER is the same new row as a folder, then
 * its tree (cp_folder_sql), then each list's tracks into that list's new row. */
static int cp_build_spool(char *buf, size_t cap) {
    char *o = buf, *end = buf + cap;
    char plsel[600], plguard[600], q[300];
    long long mark = 0;
    /* the comment names the source stick, so a spool applied after the copy gave up
     * waiting still brings that stick's export.pdb rows along (ls_pdb_sync) */
    cp_put(&o, end, "-- AZMOD cross-stick copy from ");
    cp_put(&o, end, g_cp.src_mount);
    cp_put(&o, end, "\n");
    char *g = plguard, *ge = plguard + sizeof plguard;
    if (g_cp.dst_attr == PL_ATTR_FOLDER) {
        /* the folder must still be the one picked: id and name and a folder. Folder 0
         * is the stick's top level (PASTE next to a top-level row): nothing to guard. */
        if (g_cp.dst_pl) {
            snprintf(
                q, sizeof q,
                "EXISTS (SELECT 1 FROM playlist WHERE playlist_id=%u AND attribute=%d AND name=",
                g_cp.dst_pl, PL_ATTR_FOLDER);
            cp_put(&g, ge, q);
            cp_lit(&g, ge, g_cp.dst_name, 0);
            cp_put(&g, ge, ")");
            *g = 0;
        } else
            snprintf(plguard, sizeof plguard, "1=1");
        char pf[32];
        snprintf(pf, sizeof pf, "%u", g_cp.dst_pl);
        mark = -(long long) (drag_now_ms() % 2000000000LL) - 1;
        char mk[32];
        snprintf(mk, sizeof mk, "%lld", mark);
/* N(k): k = 0 -> the name, else the name || ' (k)' */
#define CP_NK()                                      \
    do {                                             \
        cp_put(&o, end, "CASE k WHEN 0 THEN ");      \
        cp_lit(&o, end, g_cp.src_pl_name, 0);        \
        cp_put(&o, end, " ELSE ");                   \
        cp_lit(&o, end, g_cp.src_pl_name, 0);        \
        cp_put(&o, end, " || ' (' || k || ')' END"); \
    } while (0)
        cp_put(&o, end,
               "INSERT INTO playlist "
               "(playlist_id,sequenceNo,name,image_id,attribute,playlist_id_parent) SELECT ");
        cp_put(&o, end, g_cp.kind == CPK_FOLDER ? CP_NEW_PLID : "(SELECT COALESCE(MAX(playlist_id),0)+1 FROM playlist)");
        cp_put(&o, end,
               ",(SELECT COALESCE(MAX(sequenceNo),0)+1 FROM playlist WHERE playlist_id_parent=");
        cp_put(&o, end, pf);
        cp_put(&o, end, "),nm,");
        cp_put(&o, end, mk);
        cp_put(&o, end, g_cp.kind == CPK_FOLDER ? ",1," : ",0,");
        cp_put(&o, end, pf);
        cp_put(&o, end,
               " FROM (SELECT (WITH RECURSIVE n(k) AS (SELECT 0 UNION ALL SELECT k+1 FROM n WHERE "
               "k<99) SELECT ");
        CP_NK();
        cp_put(&o, end,
               " FROM n WHERE NOT EXISTS (SELECT 1 FROM playlist WHERE playlist_id_parent=");
        cp_put(&o, end, pf);
        cp_put(&o, end, " AND name=");
        CP_NK();
        cp_put(&o, end, ") ORDER BY k LIMIT 1) AS nm) WHERE nm IS NOT NULL AND ");
        cp_put(&o, end, plguard);
        cp_put(&o, end, ";\n");
#undef CP_NK
        /* the destination = the row this spool just made */
        snprintf(plsel, sizeof plsel, "(SELECT playlist_id FROM playlist WHERE image_id=%s)", mk);
        snprintf(plguard, sizeof plguard, "1=1");
        if (g_cp.kind == CPK_FOLDER)
            o = cp_folder_sql(o, end, mark);
    } else {
        snprintf(plsel, sizeof plsel, "%u", g_cp.dst_pl);
        cp_put(&g, ge, "p.name=");
        cp_lit(&g, ge, g_cp.dst_name, 0);
        *g = 0;
    }
    for (int k = 0; k < g_cp.ntrk; k++) {
        if (g_cp.kind == CPK_FOLDER) /* each track into its own list's new row */
            snprintf(plsel, sizeof plsel, "(SELECT playlist_id FROM playlist WHERE image_id=%lld)",
                     mark - g_cp.trk[k].node);
        o = cp_track_sql(o, end, &g_cp.trk[k], plsel, plguard, !mark); /* mark: a new list */
    }
    if (mark && g_cp.kind == CPK_FOLDER) {
        snprintf(q, sizeof q,
                 "UPDATE playlist SET image_id=0 WHERE image_id BETWEEN %lld AND %lld;\n",
                 mark - (g_cp.nnode - 1), mark);
        cp_put(&o, end, q);
    } else if (mark) {
        snprintf(q, sizeof q, "UPDATE playlist SET image_id=0 WHERE image_id=%lld;\n", mark);
        cp_put(&o, end, q);
    }
    if (o >= end - 1)
        return -1;
    *o = 0;
    return (int) (o - buf);
}
static int cp_copy_track(struct cp_trk *t, FILE *lg) {
    char s[600], d[600];
    snprintf(s, sizeof s, "%s%s", g_cp.src_mount, t->f[CPF_PATH]);
    snprintf(d, sizeof d, "%s%s", g_cp.dst_mount, t->f[CPF_PATH]);
    int rc = cp_file(s, d);
    if (lg)
        fprintf(lg, "audio %s -> %s rc=%d\n", s, d, rc);
    if (rc == -2)
        return -50; /* a different file already has that path */
    if (rc)
        return -51;
    t->anlz_dst[0] = 0;
    if (!t->isnull[CPF_ANLZ] &&
        t->f[CPF_ANLZ][0]) { /* the three ANLZ files (their cues ride along) */
        static const char *ext[3] = {"DAT", "EXT", "2EX"};
        const char *src = t->f[CPF_ANLZ];
        size_t L = strlen(src);
        if (L <= 4)
            goto anlz_done;
        /* rekordbox names USBANLZ/Pxxx/xxxxxxxx folders per stick, so another track
         * can own this exact path on the target. Same bytes = this track's, reuse; a
         * different file = take a fresh sibling folder and point the new content row
         * at it. Never overwrite. */
        char dpath[256];
        snprintf(dpath, sizeof dpath, "%s", src);
        for (int tries = 0; tries < 64; tries++) {
            int clash = 0;
            for (int e = 0; e < 3 && !clash; e++) {
                pd_file(s, sizeof s, g_cp.src_mount, src, 3, ext[e]); /* PIONEER/ is .PIONEER/ on */
                pd_file(d, sizeof d, g_cp.dst_mount, dpath, 3, ext[e]); /* an HFS+ stick */
                struct stat ds;
                if (stat(d, &ds) == 0 && !cp_same_bytes(s, d))
                    clash = 1;
            }
            if (!clash)
                break;
            /* .../USBANLZ/Pxxx/<8 hex>/ANLZ0000.DAT -> .../USBANLZ/Pxxx/<new 8 hex>/ANLZ0000.DAT */
            const char *leaf = strrchr(src, '/'), *dir = leaf;
            while (dir > src && dir[-1] != '/')
                dir--;
            if (!leaf || dir <= src) {
                if (lg)
                    fprintf(lg, "anlz %s: clash, no folder to vary\n", src);
                return -52;
            }
            unsigned h = 2166136261u;
            for (const char *c = src; *c; c++)
                h = (h ^ (unsigned char) *c) * 16777619u;
            h ^= g_cp.src_media * 2654435761u + (unsigned) tries * 40503u;
            snprintf(dpath, sizeof dpath, "%.*s%08X%s", (int) (dir - src), src, h, leaf);
            if (lg)
                fprintf(lg, "anlz %s: taken by another track on the target -> %s\n", src, dpath);
        }
        for (int e = 0; e < 3; e++) {
            pd_file(s, sizeof s, g_cp.src_mount, src, 3, ext[e]);
            pd_file(d, sizeof d, g_cp.dst_mount, dpath, 3, ext[e]);
            struct stat ds;
            int r = stat(d, &ds) == 0 ? (cp_same_bytes(s, d) ? 0 : -2) : cp_file(s, d);
            if (lg)
                fprintf(lg, "anlz %s rc=%d\n", d, r);
            if (r == -2)
                return -52; /* still taken: 64 names all used */
            if (r == -1 && e > 0)
                continue; /* an older export may lack .2EX */
            if (r)
                return -53;
        }
        if (strcmp(dpath, src))
            snprintf(t->anlz_dst, sizeof t->anlz_dst, "%s", dpath);
    }
anlz_done:;
    const int artf[2] = {CPF_IMG, CPF_ALBIMG};
    char *artd[2] = {t->img_dst, t->albimg_dst};
    for (int k = 0; k < 2; k++) { /* artwork (+ _m), collision-proof names */
        artd[k][0] = 0;
        if (t->isnull[artf[k]] || !t->f[artf[k]][0])
            continue;
        cp_art_dst(t->f[artf[k]], artd[k], 200);
        snprintf(s, sizeof s, "%s%s", g_cp.src_mount, t->f[artf[k]]);
        snprintf(d, sizeof d, "%s%s", g_cp.dst_mount, artd[k]);
        int r = cp_file(s, d);
        char sm[600], dm[600], t1[400], t2[400];
        cp_m_variant(t->f[artf[k]], t1, sizeof t1);
        cp_m_variant(artd[k], t2, sizeof t2);
        snprintf(sm, sizeof sm, "%s%s", g_cp.src_mount, t1);
        snprintf(dm, sizeof dm, "%s%s", g_cp.dst_mount, t2);
        cp_file(sm, dm);
        if (r)
            artd[k][0] = 0; /* no art rather than a broken reference */
    }
    return 0;
}
/* A spool, whole or not at all: <path>.tmp written and synced, then renamed. 0 = in place; -1 =
 * nothing left (a short spool on a full or pulled stick would apply the rows before the cut). */
static int cp_spool_write(const char *tmp, const char *path, const char *sql) {
    FILE *f = fopen(tmp, "we");
    if (!f)
        return -1;
    int ok = fputs(sql, f) >= 0;
    ok = fflush(f) == 0 && ok;
    ok = fsync(fileno(f)) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}
static void *cp_worker(void *arg) {
    (void) arg;
    struct sched_param sp = {0};
    pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp);
    FILE *lg = azlog_open("/tmp/azmod-copy.log", "a");
    if (lg)
        setvbuf(lg, NULL, _IONBF, 0);
    if (lg)
        fprintf(lg, "COPY kind=%d n=%d %s -> %s pl=%u attr=%u \"%s\"\n", g_cp.kind, g_cp.ntrk,
                g_cp.src_mount, g_cp.dst_mount, g_cp.dst_pl, g_cp.dst_attr, g_cp.dst_name);
    char *sql = 0;
    for (int k = 0; k < g_cp.ntrk; k++) {
        int rc = cp_copy_track(&g_cp.trk[k], lg);
        if (rc) {
            g_cp.rc = rc;
            goto out;
        } /* nothing is written to the library */
        __atomic_store_n(&g_cp.done_n, k + 1, __ATOMIC_RELEASE);
    }
    size_t cap = (size_t) g_cp.ntrk * 4096 + (size_t) g_cp.nnode * 512 + 8192;
    sql = malloc(cap);
    if (!sql || cp_build_spool(sql, cap) < 0) {
        g_cp.rc = -54;
        goto out;
    }
    char sp_dir[200], spath[300], sptmp[320];
    snprintf(sp_dir, sizeof sp_dir, "%s/PIONEER/azspool/", g_cp.dst_mount);
    cp_mkdirs(sp_dir);
    snprintf(spath, sizeof spath, "%sazcp-%04x-%u-%lld.sql", sp_dir, g_cp.src_media,
             g_cp.kind == CPK_TRACK ? g_cp.cid : g_cp.src_pl, drag_now_ms());
    snprintf(sptmp, sizeof sptmp, "%s.tmp", spath);
    if (cp_spool_write(sptmp, spath, sql) != 0) {
        g_cp.rc = -55;
        goto out;
    }
    if (lg)
        fprintf(lg, "spool %s (%zu bytes)\n", spath, strlen(sql));
    for (int waited = 0; waited < 300 && access(spath, F_OK) == 0; waited++)
        usleep(100000);
    char bad[340];
    snprintf(bad, sizeof bad, "%s.failed", spath);
    if (access(bad, F_OK) == 0) {
        g_cp.rc = -56;
        goto out;
    }
    snprintf(bad, sizeof bad, "%s.rejected", spath);
    if (access(bad, F_OK) == 0) {
        g_cp.rc = -57;
        goto out;
    }
    /* -58: the rows are written but not yet applied (the stick's library thread was busy
     * for the whole wait). The spool stays where it is and ls_tick keeps servicing it until
     * it is applied, so the paste finishes on its own. */
    g_cp.rc = access(spath, F_OK) == 0 ? -58 : 0;
    if (g_cp.rc == -58)
        __atomic_store_n(&g_ls_leftover, 1, __ATOMIC_RELEASE);
out:
    free(sql);
    free(g_cp.trk);
    g_cp.trk = 0;
    free(g_cp.node);
    g_cp.node = 0;
    if (lg) {
        fprintf(lg, "DONE rc=%d\n", g_cp.rc);
        fclose(lg);
    }
    __atomic_store_n(&g_cp.state, CP_DONE, __ATOMIC_RELEASE);
    return NULL;
}
/* message thread: 0 when no copy is reading or copying, so g_cp may be set up. The callers
 * check it before they write any g_cp field, since a running copy reads them. */
static int cp_busy(void) {
    if (__atomic_load_n(&g_cp.state, __ATOMIC_ACQUIRE) != CP_IDLE)
        return -2;
    if (__atomic_load_n(&g_drag_on, __ATOMIC_RELAXED))
        return -1;
    if (__atomic_load_n(&g_mv_state, __ATOMIC_ACQUIRE) != MV_IDLE)
        return -2;
    return 0;
}
/* message thread */
static int cp_arm(unsigned src_media, struct pl_addtgt *t) {
    int busy = cp_busy();
    if (busy)
        return busy;
    free(g_cp.trk);
    g_cp.trk = 0;
    g_cp.ntrk = 0;
    free(g_cp.node);
    g_cp.node = 0;
    g_cp.nnode = 0;
    g_cp.done_n = 0;
    g_cp.rc = 0;
    g_cp.src_media = src_media;
    g_cp.dst_pl = t->pl;
    g_cp.dst_attr = t->attr;
    utf8_copy(g_cp.dst_name, sizeof g_cp.dst_name, t->name);
    snprintf(g_cp.dst_mount, sizeof g_cp.dst_mount, "%s", t->mount);
    g_mv.op = MV_OP_CPREAD;
    g_mv.pl = 0;
    g_mv.media = src_media;
    g_mv.from = g_mv.to = 0;
    utf8_copy(g_mv.name, sizeof g_mv.name, t->name);
    g_mv.deadline = drag_now_ms() + MV_DEADLINE_MS;
    g_mv.rc = 0;
    __atomic_store_n(&g_mv_state, MV_ARMED, __ATOMIC_RELEASE);
    __atomic_store_n(&g_mv_kick, 1, __ATOMIC_RELEASE);
    return 0;
}
static int cp_request(unsigned src_media, unsigned cid, struct pl_addtgt *t) {
    if (t->attr == PL_ATTR_FOLDER)
        return -60; /* a track needs a playlist, not a folder */
    int busy = cp_busy();
    if (busy)
        return busy;
    g_cp.kind = CPK_TRACK;
    g_cp.cid = cid;
    g_cp.src_pl = 0;
    g_cp.src_pl_name[0] = 0;
    g_mv.cid = cid;
    int rc = cp_arm(src_media, t);
    drag_log("COPY track armed rc=%d: cid=%u media %04x -> pl=%u \"%s\" at %s\n", rc, cid,
             src_media, t->pl, t->name, t->mount);
    return rc;
}
static int cp_request_tagged(unsigned src_media, struct pl_addtgt *t) {
    if (t->attr == PL_ATTR_FOLDER)
        return -60;
    int busy = cp_busy();
    if (busy)
        return busy;
    g_cp.kind = CPK_TAGGED;
    g_cp.cid = 0;
    g_cp.src_pl = 0;
    g_cp.src_pl_name[0] = 0;
    g_mv.cid = 0;
    int rc = cp_arm(src_media, t);
    drag_log("COPY tagged armed rc=%d: %d tracks media %04x -> pl=%u \"%s\" at %s\n", rc, g_tag_n,
             src_media, t->pl, t->name, t->mount);
    return rc;
}
static int cp_request_playlist(unsigned src_media, unsigned src_pl, struct pl_addtgt *t) {
    int busy = cp_busy();
    if (busy)
        return busy;
    g_cp.kind = CPK_PLAYLIST;
    g_cp.cid = 0;
    g_cp.src_pl = src_pl;
    g_cp.src_pl_name[0] = 0;
    g_mv.cid = 0;
    int rc = cp_arm(src_media, t);
    drag_log("COPY playlist armed rc=%d: pl %u media %04x -> %s %u \"%s\" at %s\n", rc, src_pl,
             src_media, t->attr == PL_ATTR_FOLDER ? "folder" : "list", t->pl, t->name, t->mount);
    return rc;
}
/* A whole folder onto the other stick: a new folder in t (a folder, 0 = the top level) with
 * the same tree inside it and every list's tracks. */
static int cp_request_folder(unsigned src_media, unsigned src_pl, struct pl_addtgt *t) {
    if (t->attr != PL_ATTR_FOLDER)
        return -60;
    int busy = cp_busy();
    if (busy)
        return busy;
    g_cp.kind = CPK_FOLDER;
    g_cp.cid = 0;
    g_cp.src_pl = src_pl;
    g_cp.src_pl_name[0] = 0;
    g_mv.cid = 0;
    int rc = cp_arm(src_media, t);
    drag_log("COPY folder armed rc=%d: pl %u media %04x -> folder %u \"%s\" at %s\n", rc, src_pl,
             src_media, t->pl, t->name, t->mount);
    return rc;
}
static int mv_add(uintptr_t svc, uintptr_t lib, uintptr_t conn, FILE *lg) {
    long long r[4] = {0, 0, 0, 0};
    if (sq_row((void *) conn, "SELECT attribute FROM playlist WHERE playlist_id = ?1", g_mv.pl, -1,
               r, 1) != 1)
        return -20; /* no such playlist */
    if (lg)
        fprintf(lg, "ADD target pl=%u attribute=%lld\n", g_mv.pl, r[0]);
    if (r[0] != PL_ATTR_LIST)
        return -21; /* folder / other */
    if (sq_row((void *) conn, "SELECT COUNT(*) FROM content WHERE content_id = ?1", g_mv.cid, -1, r,
               1) != 1 ||
        r[0] != 1)
        return -22; /* no such track */
    /* Ids are per stick and another stick can sit in the same slot, so the target
     * must still carry the name it had when it was set. Duplicate tracks are allowed,
     * as in rekordbox. */
    if (g_mv.name[0]) {
        char nm[96];
        if (!sq_text((void *) conn, "SELECT name FROM playlist WHERE playlist_id = ?1", g_mv.pl, nm,
                     sizeof nm) ||
            strcmp(nm, g_mv.name) != 0)
            return -26;
    }
    long long a[4] = {0, 0, 0, 0};
    if (sq_row((void *) conn,
               "SELECT COUNT(*), MIN(sequenceNo), MAX(sequenceNo), "
               "COUNT(DISTINCT sequenceNo) FROM playlist_content WHERE playlist_id = ?1",
               g_mv.pl, -1, a, 4) != 4)
        return -9;
    long long n = a[0];
    if (n > 0 && (a[1] != 1 || a[2] != n || a[3] != n))
        return -10;
    /* Row key first, with no accessor held, in stock 0x1271070's order.
     * 0x126eab0 returns the std::vector by value: AAPCS64 passes the 24-byte result
     * pointer in x8, svc in x0, n in x1. Declaring it void(out, svc, n) reads the
     * stack as the InformationUpdater and crashes EP147. */
    struct pl_keyvec {
        char *b, *e, *cap;
    };
    struct pl_keyvec keys = ((struct pl_keyvec (*)(uintptr_t, unsigned long)) PL_KEYS_FN)(svc, 1);
    if (lg)
        fprintf(lg, "ADD step: keys %p..%p\n", keys.b, keys.e);
    /* EP147's accessor chain, exactly as remove walks it, each link checked. */
    uintptr_t dbo = 0, vt = 0;
    int rc = -4;
    if (!own_q(lib + MLLIB_SQLDB, &dbo) || !dbo || !own_q(dbo, &vt) || !vt)
        goto free_keys;
    if (lg)
        fprintf(lg, "ADD step: accessor from db %lx vt %lx\n", (unsigned long) dbo,
                (unsigned long) vt);
    void *acc = ((void *(*) (void *) )(*(void ***) dbo)[0x20 / 8])((void *) dbo);
    if (!acc)
        goto free_keys;
    if (lg)
        fprintf(lg, "ADD step: acc %p vt %p; updater\n", acc, *(void **) acc);
    rc = -24;
    void *upd = ((void *(*) (void *) )(*(void ***) acc)[0x58 / 8])(acc);
    if (lg)
        fprintf(lg, "ADD step: upd %p vt %lx\n", upd,
                upd && own_q((uintptr_t) upd, &vt) ? (unsigned long) vt : 0UL);
    if (upd && own_q((uintptr_t) upd, &vt) && vt == PLUPD_VT) {
        if (keys.b && keys.e - keys.b == 16) {
            if (lg)
                fprintf(lg, "ADD verified: pl=%u n=%lld cid %u -> seq %lld\n", g_mv.pl, n, g_mv.cid,
                        n + 1);
            rc = ((int (*)(void *, void *, unsigned, unsigned, int))(*(
                void ***) upd)[PLUPD_ADD_SLOT / 8])(upd, keys.b, g_mv.pl, g_mv.cid, (int) (n + 1));
            if (lg)
                fprintf(lg, "ADD step: +0x18 rc=%d\n", rc);
        } else
            rc = -25;
    }
    if (upd)
        ((void (*)(void *))(*(void ***) upd)[1])(upd);
    ((void (*)(void *))(*(void ***) acc)[1])(acc);
free_keys:
    for (char *k = keys.b; k && k < keys.e; k += 16)
        ((void (*)(void *)) PL_UUID_DTOR)(k);
    if (keys.b) {
        static void (*del)(void *);
        if (!del)
            *(void **) &del = dlsym(RTLD_DEFAULT, "_ZdlPv");
        if (del)
            del(keys.b);
    }
    if (rc != 0)
        return rc;
    /* Read it back: one row more, 1..N+1, and N+1 is this track. */
    long long b[4] = {0, 0, 0, 0}, c[1] = {0};
    if (sq_row((void *) conn,
               "SELECT COUNT(*), MIN(sequenceNo), MAX(sequenceNo), "
               "COUNT(DISTINCT sequenceNo) FROM playlist_content WHERE playlist_id = ?1",
               g_mv.pl, -1, b, 4) != 4 ||
        b[0] != n + 1 || b[1] != 1 || b[2] != n + 1 || b[3] != n + 1)
        return -13;
    if (sq_row((void *) conn,
               "SELECT content_id FROM playlist_content "
               "WHERE playlist_id = ?1 AND sequenceNo = ?2",
               g_mv.pl, n + 1, c, 1) != 1 ||
        c[0] != g_mv.cid)
        return -13;
    uintptr_t cache = 0;
    if (own_q(svc + INFOUPD_CACHE, &cache) && cache) {
        ((void (*)(uintptr_t, unsigned)) PLCACHE_DROP_PL)(cache, g_mv.pl);
        ((void (*)(uintptr_t, int)) PLCACHE_NOTE)(cache, 5);
    }
    return 0;
}
/* ==== Undo journal ========================================
 * Every playlist edit first records the rows it can touch (the track lists of the
 * playlists involved and the children of the folders involved, 0 = top level) and,
 * once committed, the same rows again. UNDO requires the rows to still be exactly
 * what the edit left, then restores the "before" rows in one transaction and reads
 * them back. Anything changed since: refused, untouched. A track MOVE (ADD + REMOVE)
 * is one entry. The library thread writes g_jr; the message thread reads only
 * `ready` and `media`. */
struct jpl {
    long long id, seq, img, attr, parent;
    char name[PL_TGT_NAME_MAX];
};
struct jpc {
    long long pl, cid, seq;
};
#define JR_MAXSC 512 /* a folder delete scopes every playlist inside it */
static struct {
    _Atomic int ready;
    int keep;
    unsigned media;
    long long sc_pc[JR_MAXSC];
    int n_pc;
    long long sc_par[JR_MAXSC];
    int n_par;
    struct jpl *bpl, *apl;
    int nbpl, napl, cbpl, capl;
    struct jpc *bpc, *apc;
    int nbpc, napc, cbpc, capc;
} g_jr;
static int jr_bad; /* a scope overflowed or a read failed: no undo for this edit */
/* Multi-level undo stack. g_jr records the edit in flight; each committed edit's
 * journal is pushed here (newest last, JS_MAX deep, oldest dropped). Entries are
 * undone strictly newest first, so an older entry's "after" is what the newer one's
 * restore puts back. An entry whose rows changed outside azmod (EP147, rekordbox) is
 * dropped with a message, so the ones under it stay reachable. Not kept across a
 * restart. The library thread pushes and pops; the message thread reads g_js_n and
 * the descriptions. */
#define JS_MAX 32
struct jentry {
    unsigned media;
    char desc[96];
    long long at;
    long long *sc_pc;
    int n_pc;
    long long *sc_par;
    int n_par;
    struct jpl *bpl, *apl;
    int nbpl, napl;
    struct jpc *bpc, *apc;
    int nbpc, napc;
};
static struct jentry g_js[JS_MAX];
static _Atomic int g_js_n;
static char g_jr_desc[96];   /* what the edit in flight does ("removing \"X\"") */
static char g_js_undone[96]; /* what the last UNDO undid (pl_edit_notify) */
static void js_free(struct jentry *e) {
    free(e->sc_pc);
    free(e->sc_par);
    free(e->bpl);
    free(e->apl);
    free(e->bpc);
    free(e->apc);
    memset(e, 0, sizeof *e);
}
static void *js_dup(const void *p, size_t n) {
    if (!n)
        return 0;
    void *q = malloc(n);
    if (q)
        memcpy(q, p, n);
    return q;
}
static void jr_clear(void) {
    __atomic_store_n(&g_jr.ready, 0, __ATOMIC_RELEASE);
    g_jr.n_pc = g_jr.n_par = g_jr.nbpl = g_jr.napl = g_jr.nbpc = g_jr.napc = 0;
    jr_bad = 0;
}
static int jr_pl_has(struct jpl *v, int n, long long id) {
    for (int i = 0; i < n; i++)
        if (v[i].id == id)
            return 1;
    return 0;
}
static int jr_read_pl(void *c, long long parent, struct jpl **v, int *n, int *cap) {
    void *st = 0;
    if (g_sq.prepare(c,
                     "SELECT playlist_id, sequenceNo, image_id, attribute, playlist_id_parent, "
                     "name FROM playlist "
                     "WHERE playlist_id_parent = ?1",
                     -1, &st, 0) != 0 ||
        !st)
        return -1;
    g_sq.bind_int64(st, 1, parent);
    while (g_sq.step(st) == 100) {
        long long id = g_sq.column_int64(st, 0);
        if (jr_pl_has(*v, *n, id))
            continue;
        if (*n >= *cap) {
            int nc = *cap ? *cap * 2 : 64;
            void *nv = realloc(*v, (size_t) nc * sizeof **v);
            if (!nv) {
                g_sq.finalize(st);
                return -1;
            }
            *v = nv;
            *cap = nc;
        }
        struct jpl *r = &(*v)[(*n)++];
        r->id = id;
        r->seq = g_sq.column_int64(st, 1);
        r->img = g_sq.column_int64(st, 2);
        r->attr = g_sq.column_int64(st, 3);
        r->parent = g_sq.column_int64(st, 4);
        const unsigned char *t = g_sq.column_text(st, 5);
        utf8_copy(r->name, sizeof r->name, t ? (const char *) t : "");
    }
    g_sq.finalize(st);
    return 0;
}
static int jr_read_pc(void *c, long long pl, struct jpc **v, int *n, int *cap) {
    void *st = 0;
    if (g_sq.prepare(c,
                     "SELECT content_id, sequenceNo FROM playlist_content WHERE playlist_id = ?1",
                     -1, &st, 0) != 0 ||
        !st)
        return -1;
    g_sq.bind_int64(st, 1, pl);
    while (g_sq.step(st) == 100) {
        if (*n >= *cap) {
            int nc = *cap ? *cap * 2 : 256;
            void *nv = realloc(*v, (size_t) nc * sizeof **v);
            if (!nv) {
                g_sq.finalize(st);
                return -1;
            }
            *v = nv;
            *cap = nc;
        }
        struct jpc *r = &(*v)[(*n)++];
        r->pl = pl;
        r->cid = g_sq.column_int64(st, 0);
        r->seq = g_sq.column_int64(st, 1);
    }
    g_sq.finalize(st);
    return 0;
}
/* scopes: record the "before" rows now (called before the write) */
static void jr_scope_parent(void *c, long long parent) {
    for (int i = 0; i < g_jr.n_par; i++)
        if (g_jr.sc_par[i] == parent)
            return;
    if (g_jr.n_par >= JR_MAXSC) {
        jr_bad = 1;
        return;
    }
    g_jr.sc_par[g_jr.n_par++] = parent;
    if (jr_read_pl(c, parent, &g_jr.bpl, &g_jr.nbpl, &g_jr.cbpl))
        jr_bad = 1;
}
static void jr_scope_pc(void *c, long long pl) {
    for (int i = 0; i < g_jr.n_pc; i++)
        if (g_jr.sc_pc[i] == pl)
            return;
    if (g_jr.n_pc >= JR_MAXSC) {
        jr_bad = 1;
        return;
    }
    g_jr.sc_pc[g_jr.n_pc++] = pl;
    if (c && jr_read_pc(c, pl, &g_jr.bpc, &g_jr.nbpc, &g_jr.cbpc))
        jr_bad = 1;
}
/* the rows of every scope as they are now */
static int jr_read_now(void *c, struct jpl **pl, int *npl, int *cpl, struct jpc **pc, int *npc,
                       int *cpc) {
    *npl = *npc = 0;
    for (int i = 0; i < g_jr.n_par; i++)
        if (jr_read_pl(c, g_jr.sc_par[i], pl, npl, cpl))
            return -1;
    for (int i = 0; i < g_jr.n_pc; i++)
        if (jr_read_pc(c, g_jr.sc_pc[i], pc, npc, cpc))
            return -1;
    return 0;
}
/* after a committed edit: a playlist row that is new (in no "before" scope) had
 * no tracks before, so its track list joins the scope with an empty before */
static void js_push(int cont);
static void jr_after(void *c, int cont) {
    if (jr_bad) {
        jr_clear();
        return;
    }
    int dummy = 0;
    (void) dummy;
    struct jpl *t = 0;
    int nt = 0, ct = 0;
    for (int i = 0; i < g_jr.n_par; i++)
        if (jr_read_pl(c, g_jr.sc_par[i], &t, &nt, &ct)) {
            free(t);
            jr_clear();
            return;
        }
    for (int i = 0; i < nt; i++)
        if (!jr_pl_has(g_jr.bpl, g_jr.nbpl, t[i].id))
            jr_scope_pc(0, t[i].id); /* new: empty before */
    free(t);
    if (jr_bad ||
        jr_read_now(c, &g_jr.apl, &g_jr.napl, &g_jr.capl, &g_jr.apc, &g_jr.napc, &g_jr.capc)) {
        jr_clear();
        return;
    }
    __atomic_store_n(&g_jr.ready, 1, __ATOMIC_RELEASE);
    js_push(cont);
}
/* library thread: the recorder's journal onto the stack. cont = the second step of a
 * MOVE (ADD, then REMOVE): it replaces the ADD's entry, so one UNDO takes the MOVE back. */
static void js_push(int cont) {
    int n = __atomic_load_n(&g_js_n, __ATOMIC_ACQUIRE);
    if (cont && n > 0)
        js_free(&g_js[--n]);
    if (n == JS_MAX) {
        js_free(&g_js[0]);
        memmove(&g_js[0], &g_js[1], (JS_MAX - 1) * sizeof g_js[0]);
        memset(&g_js[JS_MAX - 1], 0, sizeof g_js[0]);
        n--;
    }
    struct jentry *e = &g_js[n];
    e->media = g_jr.media;
    e->at = drag_now_ms();
    snprintf(e->desc, sizeof e->desc, "%s", g_jr_desc);
    e->n_pc = g_jr.n_pc;
    e->sc_pc = js_dup(g_jr.sc_pc, (size_t) g_jr.n_pc * sizeof *g_jr.sc_pc);
    e->n_par = g_jr.n_par;
    e->sc_par = js_dup(g_jr.sc_par, (size_t) g_jr.n_par * sizeof *g_jr.sc_par);
    e->nbpl = g_jr.nbpl;
    e->bpl = js_dup(g_jr.bpl, (size_t) g_jr.nbpl * sizeof *g_jr.bpl);
    e->napl = g_jr.napl;
    e->apl = js_dup(g_jr.apl, (size_t) g_jr.napl * sizeof *g_jr.apl);
    e->nbpc = g_jr.nbpc;
    e->bpc = js_dup(g_jr.bpc, (size_t) g_jr.nbpc * sizeof *g_jr.bpc);
    e->napc = g_jr.napc;
    e->apc = js_dup(g_jr.apc, (size_t) g_jr.napc * sizeof *g_jr.apc);
    if ((g_jr.n_pc && !e->sc_pc) || (g_jr.n_par && !e->sc_par) || (g_jr.nbpl && !e->bpl) ||
        (g_jr.napl && !e->apl) || (g_jr.nbpc && !e->bpc) || (g_jr.napc && !e->apc)) {
        js_free(e);
        return;
    } /* out of memory: this edit has no undo */
    __atomic_store_n(&g_js_n, n + 1, __ATOMIC_RELEASE);
}
static int jr_cmp_pl(const void *a, const void *b) {
    const struct jpl *x = a, *y = b;
    return x->id < y->id ? -1 : x->id > y->id;
}
static int jr_cmp_pc(const void *a, const void *b) {
    const struct jpc *x = a, *y = b;
    if (x->pl != y->pl)
        return x->pl < y->pl ? -1 : 1;
    if (x->seq != y->seq)
        return x->seq < y->seq ? -1 : 1;
    return x->cid < y->cid ? -1 : x->cid > y->cid;
}
static int jr_same(struct jpl *a, int na, struct jpl *b, int nb, struct jpc *c, int nc,
                   struct jpc *d, int nd) {
    if (na != nb || nc != nd)
        return 0;
    qsort(a, na, sizeof *a, jr_cmp_pl);
    qsort(b, nb, sizeof *b, jr_cmp_pl);
    qsort(c, nc, sizeof *c, jr_cmp_pc);
    qsort(d, nd, sizeof *d, jr_cmp_pc);
    for (int i = 0; i < na; i++)
        if (a[i].id != b[i].id || a[i].seq != b[i].seq || a[i].img != b[i].img ||
            a[i].attr != b[i].attr || a[i].parent != b[i].parent || strcmp(a[i].name, b[i].name))
            return 0;
    for (int i = 0; i < nc; i++)
        if (c[i].pl != d[i].pl || c[i].cid != d[i].cid || c[i].seq != d[i].seq)
            return 0;
    return 1;
}
static void lib_invalidate(uintptr_t cache, void *conn, FILE *lg);
static int lp_exec(void *conn, const char *sql, FILE *lg);
static int sq_exec4(void *conn, const char *sql, long long a, long long b, long long c,
                    long long d);
/* the rows of an entry's scopes as they are now (jr_read_now, for a stored entry) */
static int js_read_now(void *c, struct jentry *e, struct jpl **pl, int *npl, int *cpl,
                       struct jpc **pc, int *npc, int *cpc) {
    *npl = *npc = 0;
    for (int i = 0; i < e->n_par; i++)
        if (jr_read_pl(c, e->sc_par[i], pl, npl, cpl))
            return -1;
    for (int i = 0; i < e->n_pc; i++)
        if (jr_read_pc(c, e->sc_pc[i], pc, npc, cpc))
            return -1;
    return 0;
}
static int jr_undo(uintptr_t conn, uintptr_t cache, FILE *lg) {
    void *c = (void *) conn;
    static int (*bind_text)(void *, int, const char *, int, void *);
    if (!bind_text)
        *(void **) &bind_text = dlsym(RTLD_DEFAULT, "sqlite3_bind_text");
    if (!bind_text || !cache)
        return -40;
    int nst = __atomic_load_n(&g_js_n, __ATOMIC_ACQUIRE);
    if (nst < 1)
        return -82;
    struct jentry *e = &g_js[nst - 1];
    snprintf(g_js_undone, sizeof g_js_undone, "%s", e->desc);
    struct jpl *npl = 0;
    struct jpc *npc = 0;
    int nn = 0, cn = 0, mn = 0, cm = 0, rc = -1;
    if (js_read_now(c, e, &npl, &nn, &cn, &npc, &mn, &cm))
        goto out;
    if (!jr_same(npl, nn, e->apl, e->napl, npc, mn, e->apc, e->napc)) {
        rc = -80;
        goto out;
    } /* changed since */
    rc = lp_exec(c, "BEGIN", lg);
    for (int i = 0; !rc && i < e->napl; i++)
        rc = sq_exec4(c, "DELETE FROM playlist WHERE playlist_id = ?1", e->apl[i].id, 0, 0, 0);
    for (int i = 0; !rc && i < e->n_pc; i++)
        rc = sq_exec4(c, "DELETE FROM playlist_content WHERE playlist_id = ?1", e->sc_pc[i], 0, 0,
                      0);
    for (int i = 0; !rc && i < e->nbpl; i++) {
        void *st = 0;
        struct jpl *r = &e->bpl[i];
        if (g_sq.prepare(c,
                         "INSERT INTO playlist (playlist_id, sequenceNo, name, image_id, "
                         "attribute, playlist_id_parent) "
                         "VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
                         -1, &st, 0) != 0 ||
            !st) {
            rc = -42;
            break;
        }
        g_sq.bind_int64(st, 1, r->id);
        g_sq.bind_int64(st, 2, r->seq);
        bind_text(st, 3, r->name, -1, (void *) -1);
        g_sq.bind_int64(st, 4, r->img);
        g_sq.bind_int64(st, 5, r->attr);
        g_sq.bind_int64(st, 6, r->parent);
        rc = g_sq.step(st) == 101 ? 0 : -43;
        g_sq.finalize(st);
    }
    for (int i = 0; !rc && i < e->nbpc; i++)
        rc = sq_exec4(c,
                      "INSERT INTO playlist_content (playlist_id, content_id, sequenceNo) VALUES "
                      "(?1, ?2, ?3)",
                      e->bpc[i].pl, e->bpc[i].cid, e->bpc[i].seq, 0);
    if (!rc) { /* read back: exactly the before rows */
        if (js_read_now(c, e, &npl, &nn, &cn, &npc, &mn, &cm) ||
            !jr_same(npl, nn, e->bpl, e->nbpl, npc, mn, e->bpc, e->nbpc))
            rc = -81;
    }
    lp_exec(c, rc ? "ROLLBACK" : "COMMIT", lg);
out:
    if (lg)
        fprintf(lg, "UNDO [%d] \"%s\" rc=%d (rows: playlist %d -> %d, tracks %d -> %d)\n", nst,
                e->desc, rc, e->napl, e->nbpl, e->napc, e->nbpc);
    if (!rc)
        lib_invalidate(cache, c, lg);
    if (!rc || rc == -80) { /* undone, or no longer undoable: off the stack either way */
        js_free(e);
        __atomic_store_n(&g_js_n, nst - 1, __ATOMIC_RELEASE);
        jr_clear();
    }
    free(npl);
    free(npc);
    return rc > 0 ? -43 : rc;
}
static char g_pc_src[PL_TGT_NAME_MAX]; /* the source's name, for the message */
static int plo_place_after(void *c, long long parent, long long id, long long after);
static int plo_dest_ok(void *c, long long dest, const char *name);
/* COPY of a folder (same stick): the tree, breadth-first, every row a new id with its
 * parent remapped, every list's tracks; the new top-level folder named like a playlist
 * copy ("(copy)" when the name is taken) and placed like one. One transaction, read back.
 * Every new folder / list is scoped in the undo journal before its rows exist (empty
 * scopes), so UNDO takes the whole copy away. */
static int mv_fcopy(void *c, uintptr_t cache, FILE *lg) {
    static long long src[FD_MAX], dst[FD_MAX], srcpar[FD_MAX];
    static unsigned char isf[FD_MAX];
    int n = 0, rc;
    src[n] = g_mv.from;
    srcpar[n] = -1;
    isf[n++] = 1;
    for (int q = 0; q < n; q++) {
        if (!isf[q])
            continue;
        void *st = 0;
        if (g_sq.prepare(
                c, "SELECT playlist_id, attribute FROM playlist WHERE playlist_id_parent = ?1", -1,
                &st, 0) != 0 ||
            !st)
            return -42;
        g_sq.bind_int64(st, 1, src[q]);
        while (g_sq.step(st) == 100) {
            if (n >= FD_MAX) {
                g_sq.finalize(st);
                return -46;
            }
            src[n] = g_sq.column_int64(st, 0);
            isf[n] = g_sq.column_int64(st, 1) == PL_ATTR_FOLDER;
            srcpar[n++] = q;
        }
        g_sq.finalize(st);
    }
    for (int i = 0; i < n; i++)
        if (isf[i] && src[i] == (long long) g_mv.pl)
            return -63; /* into itself / inside itself */
    if (g_mv.pl == g_mv.from)
        return -63;
    /* the top folder's name: "(copy)" if a FOLDER of that name is already there */
    long long dup[1] = {0};
    char sfx[24] = "", q[320];
    if (sq_row(c,
               "SELECT COUNT(*) FROM playlist WHERE playlist_id_parent = ?1 "
               "AND name = (SELECT name FROM playlist WHERE playlist_id = ?2)",
               g_mv.pl, g_mv.from, dup, 1) != 1)
        return -9;
    for (int k = 1; dup[0] && k <= 99; k++) { /* rekordbox's " (1)", as mv_plcopy */
        snprintf(sfx, sizeof sfx, " (%d)", k);
        snprintf(q, sizeof q,
                 "SELECT COUNT(*) FROM playlist WHERE playlist_id_parent = ?1 "
                 "AND name = (SELECT name FROM playlist WHERE playlist_id = ?2) || '%s'",
                 sfx);
        if (sq_row(c, q, g_mv.pl, g_mv.from, dup, 1) != 1)
            return -9;
    }
    if (dup[0])
        return -61;
    {
        size_t L = strlen(g_pc_src);
        snprintf(g_pc_src + L, sizeof g_pc_src - L, "%s", sfx);
    }
    long long nid[1] = {0}, seq[1] = {0};
    if (sq_row(c, "SELECT MAX(16384, COALESCE(MAX(playlist_id), 0)) + 1 FROM playlist", 0, -1, nid,
               1) != 1 ||
        sq_row(
            c,
            "SELECT COALESCE(MAX(sequenceNo), 0) + 1 FROM playlist WHERE playlist_id_parent = ?1",
            g_mv.pl, -1, seq, 1) != 1)
        return -41;
    for (int i = 0; i < n; i++)
        dst[i] = nid[0] + i;
    jr_scope_parent(c, g_mv.pl); /* undo: where the copy lands ... */
    for (int i = 0; i < n; i++) {
        if (isf[i])
            jr_scope_parent(c, dst[i]);
        else
            jr_scope_pc(c, dst[i]);
    } /* ... and everything new */
    rc = lp_exec(c, "BEGIN", lg);
    for (int i = 0; !rc && i < n; i++) {
        if (i == 0) {
            snprintf(q, sizeof q,
                     "INSERT INTO playlist (playlist_id, sequenceNo, name, image_id, attribute, "
                     "playlist_id_parent) "
                     "SELECT ?1, ?2, name || '%s', 0, 1, ?3 FROM playlist WHERE playlist_id = ?4",
                     sfx);
            rc = sq_exec4(c, q, dst[0], seq[0], g_mv.pl, src[0]);
        } else
            rc = sq_exec4(c,
                          "INSERT INTO playlist (playlist_id, sequenceNo, name, image_id, "
                          "attribute, playlist_id_parent) "
                          "SELECT ?1, sequenceNo, name, image_id, attribute, ?2 FROM playlist "
                          "WHERE playlist_id = ?3",
                          dst[i], dst[srcpar[i]], src[i], 0);
        if (!rc && !isf[i])
            rc = sq_exec4(
                c,
                "INSERT INTO playlist_content (playlist_id, content_id, sequenceNo) "
                "SELECT ?1, content_id, sequenceNo FROM playlist_content WHERE playlist_id = ?2",
                dst[i], src[i], 0, 0);
    }
    /* read back: every new row under its new parent, every list with its source's track count */
    for (int i = 0; !rc && i < n; i++) {
        long long a[2] = {0, 0}, b[1] = {0};
        if (sq_row(c,
                   "SELECT COUNT(*), MAX(playlist_id_parent) FROM playlist WHERE playlist_id = ?1",
                   dst[i], -1, a, 2) != 2 ||
            a[0] != 1 || a[1] != (i ? dst[srcpar[i]] : (long long) g_mv.pl))
            rc = -44;
        if (!rc && !isf[i]) {
            long long x[1] = {0};
            if (sq_row(c, "SELECT COUNT(*) FROM playlist_content WHERE playlist_id = ?1", dst[i],
                       -1, b, 1) != 1 ||
                sq_row(c, "SELECT COUNT(*) FROM playlist_content WHERE playlist_id = ?1", src[i],
                       -1, x, 1) != 1 ||
                b[0] != x[0])
                rc = -44;
        }
    }
    if (!rc)
        rc = plo_place_after(c, g_mv.pl, dst[0], g_mv.to);
    lp_exec(c, rc ? "ROLLBACK" : "COMMIT", lg);
    if (lg)
        fprintf(lg, "FCOPY %u \"%s\" -> %lld in %u: %d rows rc=%d\n", g_mv.from, g_pc_src, dst[0],
                g_mv.pl, n, rc);
    if (rc)
        return rc > 0 ? -43 : rc;
    lib_invalidate(cache, c, lg);
    return 0;
}
/* Same-stick COPY of playlist g_mv.from into folder g_mv.pl: a new list with the
 * source's name (" (1)", " (2)" ... when taken) and its tracks in order; rows only,
 * the files are already on this stick. A folder source goes to mv_fcopy. One
 * transaction, read back before COMMIT. Library thread. */
static int mv_plcopy(uintptr_t conn, uintptr_t cache, FILE *lg) {
    void *c = (void *) conn;
    char nm[PL_TGT_NAME_MAX] = "";
    long long at[1] = {-1}, n[1] = {0}, dup[1] = {0}, nid[1] = {0}, seq[1] = {0}, b[2] = {0, 0};
    g_pc_src[0] = 0;
    if (!cache)
        return -40;
    if (!plo_dest_ok(c, g_mv.pl, g_mv.name))
        return -26; /* not the folder picked */
    if (sq_row(c, "SELECT attribute FROM playlist WHERE playlist_id = ?1", g_mv.from, -1, at, 1) !=
            1 ||
        !sq_text(c, "SELECT name FROM playlist WHERE playlist_id = ?1", g_mv.from, g_pc_src,
                 sizeof g_pc_src) ||
        !g_pc_src[0])
        return -45;
    if (at[0] == PL_ATTR_FOLDER)
        return mv_fcopy(c, cache, lg); /* a whole folder */
    if (at[0] != PL_ATTR_LIST)
        return -45;
    if (sq_row(c, "SELECT COUNT(*) FROM playlist_content WHERE playlist_id = ?1", g_mv.from, -1, n,
               1) != 1)
        return -9;
    (void) nm;
    if (sq_row(c,
               "SELECT COUNT(*) FROM playlist WHERE playlist_id_parent = ?1 "
               "AND name = (SELECT name FROM playlist WHERE playlist_id = ?2)",
               g_mv.pl, g_mv.from, dup, 1) != 1)
        return -9;
    /* Name taken (always, for a copy pasted next to its original): "<name> (1)",
     * "(2)" ... as rekordbox's device import names a copy; rekordbox refuses two items
     * with one name. A fixed ASCII literal. */
    char sfx[24] = "", q[320];
    for (int k = 1; dup[0] && k <= 99; k++) {
        snprintf(sfx, sizeof sfx, " (%d)", k);
        snprintf(q, sizeof q,
                 "SELECT COUNT(*) FROM playlist WHERE playlist_id_parent = ?1 "
                 "AND name = (SELECT name FROM playlist WHERE playlist_id = ?2) || '%s'",
                 sfx);
        if (sq_row(c, q, g_mv.pl, g_mv.from, dup, 1) != 1)
            return -9;
    }
    if (dup[0])
        return -61;
    {
        size_t L = strlen(g_pc_src);
        snprintf(g_pc_src + L, sizeof g_pc_src - L, "%s", sfx);
    }                            /* the name it gets */
    jr_scope_parent(c, g_mv.pl); /* undo: the list it lands in */
    if (sq_row(c, "SELECT MAX(16384, COALESCE(MAX(playlist_id), 0)) + 1 FROM playlist", 0, -1, nid,
               1) != 1 ||
        sq_row(
            c,
            "SELECT COALESCE(MAX(sequenceNo), 0) + 1 FROM playlist WHERE playlist_id_parent = ?1",
            g_mv.pl, -1, seq, 1) != 1)
        return -41;
    int rc = lp_exec(c, "BEGIN", lg);
    snprintf(q, sizeof q,
             "INSERT INTO playlist (playlist_id, sequenceNo, name, image_id, attribute, "
             "playlist_id_parent) "
             "SELECT ?1, ?2, name || '%s', 0, 0, ?3 FROM playlist WHERE playlist_id = ?4",
             sfx);
    if (!rc)
        rc = sq_exec4(c, q, nid[0], seq[0], g_mv.pl, g_mv.from);
    if (!rc)
        rc = sq_exec4(
            c,
            "INSERT INTO playlist_content (playlist_id, content_id, sequenceNo) "
            "SELECT ?1, content_id, sequenceNo FROM playlist_content WHERE playlist_id = ?2",
            nid[0], g_mv.from, 0, 0);
    /* read back inside the transaction: same name, same parent, every row */
    if (!rc &&
        (!sq_text(c, "SELECT name FROM playlist WHERE playlist_id = ?1", nid[0], nm, sizeof nm) ||
         strcmp(nm, g_pc_src) ||
         sq_row(c,
                "SELECT COUNT(*), (SELECT playlist_id_parent FROM playlist WHERE playlist_id = ?1) "
                "FROM playlist_content WHERE playlist_id = ?1",
                nid[0], -1, b, 2) != 2 ||
         b[0] != n[0] || b[1] != (long long) g_mv.pl))
        rc = -44;
    if (!rc)
        rc = plo_place_after(c, g_mv.pl, nid[0], g_mv.to); /* PASTE next to a row: right below it */
    lp_exec(c, rc ? "ROLLBACK" : "COMMIT", lg);
    if (lg)
        fprintf(lg, "PLCOPY %u \"%s\" -> %lld in folder %u after %u (%lld tracks) rc=%d\n",
                g_mv.from, g_pc_src, nid[0], g_mv.pl, g_mv.to, n[0], rc);
    if (rc)
        return rc > 0 ? -43 : rc;
    g_mv.n = (int) n[0];
    lib_invalidate(cache, c, lg);
    return 0;
}
/* Renumber a folder's children 1..N in the order EP147 shows them (sequenceNo,
 * then playlist_id): order kept, ties and gaps gone. Inside the caller's
 * transaction. 0 = done and read back. */
static int plo_renumber(void *c, long long parent) {
    static long long ids[1024];
    void *st = 0;
    int n = 0, rc = 0;
    if (g_sq.prepare(c,
                     "SELECT playlist_id FROM playlist WHERE playlist_id_parent = ?1 ORDER BY "
                     "sequenceNo, playlist_id",
                     -1, &st, 0) != 0 ||
        !st)
        return -9;
    g_sq.bind_int64(st, 1, parent);
    while (g_sq.step(st) == 100) {
        if (n >= 1024) {
            g_sq.finalize(st);
            return -46;
        }
        ids[n++] = g_sq.column_int64(st, 0);
    }
    g_sq.finalize(st);
    for (int i = 0; !rc && i < n; i++)
        rc = sq_exec4(c, "UPDATE playlist SET sequenceNo = ?1 WHERE playlist_id = ?2", i + 1,
                      ids[i], 0, 0);
    if (rc)
        return rc;
    long long a[4] = {0, 0, 0, 0};
    if (sq_row(c,
               "SELECT COUNT(*), COALESCE(MIN(sequenceNo), 1), COALESCE(MAX(sequenceNo), 0), "
               "COUNT(DISTINCT sequenceNo) "
               "FROM playlist WHERE playlist_id_parent = ?1",
               parent, -1, a, 4) != 4 ||
        a[0] != n || (n && (a[1] != 1 || a[2] != n)) || a[3] != n)
        return -44;
    return 0;
}
/* Put `id` right after `after` among parent's children (EP147's display order),
 * renumber 1..N. after == 0: leave it where it is (the end). Inside the caller's
 * transaction. */
static int plo_place_after(void *c, long long parent, long long id, long long after) {
    if (!after)
        return 0;
    static long long ids[1024];
    void *st = 0;
    int n = 0, rc = 0, have = 0;
    if (g_sq.prepare(c,
                     "SELECT playlist_id FROM playlist WHERE playlist_id_parent = ?1 ORDER BY "
                     "sequenceNo, playlist_id",
                     -1, &st, 0) != 0 ||
        !st)
        return -9;
    g_sq.bind_int64(st, 1, parent);
    while (g_sq.step(st) == 100) {
        long long x = g_sq.column_int64(st, 0);
        if (x == id)
            continue; /* re-inserted after `after` */
        if (n >= 1022) {
            g_sq.finalize(st);
            return -46;
        }
        ids[n++] = x;
        if (x == after) {
            ids[n++] = id;
            have = 1;
        }
    }
    g_sq.finalize(st);
    if (!have)
        return -26; /* the row it was pasted next to is gone */
    for (int i = 0; !rc && i < n; i++)
        rc = sq_exec4(c, "UPDATE playlist SET sequenceNo = ?1 WHERE playlist_id = ?2", i + 1,
                      ids[i], 0, 0);
    return rc;
}
/* Is `folder` (or any folder above it) == `id`? A folder must never go inside itself. */
static int plo_is_under(void *c, long long folder, long long id) {
    for (int d = 0; folder > 0 && d < 64; d++) {
        if (folder == id)
            return 1;
        long long p[1] = {0};
        if (sq_row(c, "SELECT playlist_id_parent FROM playlist WHERE playlist_id = ?1", folder, -1,
                   p, 1) != 1)
            return 1; /* unknown: refuse */
        folder = p[0];
    }
    return folder > 0; /* too deep: refuse */
}
/* The destination folder of a PASTE: `dest` 0 = the top level (no row to check);
 * otherwise it must still be the folder picked (attribute + name). */
static int plo_dest_ok(void *c, long long dest, const char *name) {
    if (!dest)
        return 1;
    char nm[PL_TGT_NAME_MAX] = "";
    long long at[1] = {-1};
    return sq_row(c, "SELECT attribute FROM playlist WHERE playlist_id = ?1", dest, -1, at, 1) ==
               1 &&
           at[0] == PL_ATTR_FOLDER &&
           sq_text(c, "SELECT name FROM playlist WHERE playlist_id = ?1", dest, nm, sizeof nm) &&
           !strcmp(nm, name);
}
/* Same-stick move of playlist or folder g_mv.from into folder g_mv.pl. Nothing is
 * copied or deleted: the row gets the folder as its parent and the next position
 * there, and the siblings it left are renumbered, so its id, tracks and anything
 * keyed by it are untouched. Refused: not the folder picked, a name clash, a folder
 * into itself, or already there with no paste position. One transaction, read back
 * before COMMIT. Library thread. */
static int mv_plmove(uintptr_t conn, uintptr_t cache, FILE *lg) {
    void *c = (void *) conn;
    char nm[PL_TGT_NAME_MAX] = "";
    long long at[1] = {-1}, dup[1] = {0}, seq[1] = {0}, old[2] = {0, 0}, n[1] = {0},
              b[3] = {0, 0, 0};
    g_pc_src[0] = 0;
    if (!cache)
        return -40;
    (void) nm;
    if (!plo_dest_ok(c, g_mv.pl, g_mv.name))
        return -26; /* not the folder picked */
    if (sq_row(c, "SELECT attribute FROM playlist WHERE playlist_id = ?1", g_mv.from, -1, at, 1) !=
            1 ||
        (at[0] != PL_ATTR_LIST && at[0] != PL_ATTR_FOLDER) ||
        !sq_text(c, "SELECT name FROM playlist WHERE playlist_id = ?1", g_mv.from, g_pc_src,
                 sizeof g_pc_src) ||
        !g_pc_src[0])
        return -45;
    long long kind = at[0];
    if (kind == PL_ATTR_FOLDER && plo_is_under(c, g_mv.pl, g_mv.from))
        return -63; /* into itself */
    if (sq_row(c, "SELECT playlist_id_parent, sequenceNo FROM playlist WHERE playlist_id = ?1",
               g_mv.from, -1, old, 2) != 2)
        return -9;
    int same = old[0] == (long long) g_mv.pl;
    if (same && !g_mv.to)
        return -62; /* already in that folder */
    if (sq_row(c, "SELECT COUNT(*) FROM playlist_content WHERE playlist_id = ?1", g_mv.from, -1, n,
               1) != 1)
        return -9;
    /* rekordbox 7 refuses any two items (playlist or folder) with one name in a folder */
    if (!same && (sq_row(c,
                         "SELECT COUNT(*) FROM playlist WHERE playlist_id_parent = ?1 "
                         "AND name = (SELECT name FROM playlist WHERE playlist_id = ?2)",
                         g_mv.pl, g_mv.from, dup, 1) != 1))
        return -9;
    if (!same && dup[0])
        return -61;
    jr_scope_parent(c, old[0]);
    jr_scope_parent(c, g_mv.pl); /* undo: the list it leaves and the one it joins */
    if (sq_row(
            c,
            "SELECT COALESCE(MAX(sequenceNo), 0) + 1 FROM playlist WHERE playlist_id_parent = ?1",
            g_mv.pl, -1, seq, 1) != 1)
        return -41;
    int rc = lp_exec(c, "BEGIN", lg);
    if (!rc)
        rc = sq_exec4(
            c,
            "UPDATE playlist SET playlist_id_parent = ?1, sequenceNo = ?2 WHERE playlist_id = ?3",
            g_mv.pl, seq[0], g_mv.from, 0);
    /* the list it left: 1..N in display order (real sticks carry ties and gaps, so
     * shifting the later rows down by one is not enough) */
    if (!rc)
        rc = plo_renumber(c, old[0]);
    if (!rc)
        rc = plo_place_after(c, g_mv.pl, g_mv.from,
                             g_mv.to); /* PASTE next to a row: right below it */
    /* read back inside the transaction: the new parent, the same tracks */
    if (!rc && (sq_row(c,
                       "SELECT playlist_id_parent, sequenceNo, (SELECT COUNT(*) FROM "
                       "playlist_content WHERE playlist_id = ?1) "
                       "FROM playlist WHERE playlist_id = ?1",
                       g_mv.from, -1, b, 3) != 3 ||
                b[0] != (long long) g_mv.pl || b[2] != n[0]))
        rc = -44;
    lp_exec(c, rc ? "ROLLBACK" : "COMMIT", lg);
    if (lg)
        fprintf(
            lg,
            "PLMOVE %u \"%s\" parent %lld/#%lld -> folder %u/#%lld after %u (%lld tracks) rc=%d\n",
            g_mv.from, g_pc_src, old[0], old[1], g_mv.pl, b[1], g_mv.to, n[0], rc);
    if (rc)
        return rc > 0 ? -43 : rc;
    g_mv.n = (int) n[0];
    lib_invalidate(cache, c, lg);
    return 0;
}
/* Knob drag of a playlist/folder row: display row g_mv.from -> g_mv.to (1-based)
 * among the siblings of playlist g_mv.pl. EP147 has no writer for this. Stored
 * sequenceNo is not the display position (sticks carry ties and gaps); EP147 shows
 * `ORDER BY sequenceNo, playlist_id`. So:
 *   Before  read the siblings in that order; their names must spell the rows on
 *           screen (g_plo_names, read at the grab) and the id at `from` must be the
 *           grabbed one. That, not arithmetic, proves display row r = this sibling.
 *   Write   every sibling gets sequenceNo 1..N in the new order, in one
 *           transaction, which also normalizes ties and gaps.
 *   After   1..N distinct, and the names spell the permuted snapshot. Else ROLLBACK.
 * Library thread. */
static int plo_src(int d, int from, int to) { /* display row d (0-based) shows snapshot row src */
    if (from == to)
        return d;
    if (d == to - 1)
        return from - 1;
    if (to > from && d >= from - 1 && d < to - 1)
        return d + 1;
    if (to < from && d > to - 1 && d <= from - 1)
        return d - 1;
    return d;
}
/* the siblings in EP147's display order: ids[] and a names-match against the
 * snapshot permuted by (from, to). Returns the row count, or -1. */
static int plo_read(void *c, long long parent, long long *ids, int cap, int from, int to,
                    int *names_ok) {
    void *st = 0;
    int r = 0, ok = 1;
    if (g_sq.prepare(c,
                     "SELECT playlist_id, name FROM playlist WHERE playlist_id_parent = ?1 "
                     "ORDER BY sequenceNo, playlist_id",
                     -1, &st, 0) != 0 ||
        !st)
        return -1;
    g_sq.bind_int64(st, 1, parent);
    while (g_sq.step(st) == 100) {
        const unsigned char *t = g_sq.column_text(st, 1);
        int src = plo_src(r, from, to);
        if (r < cap)
            ids[r] = g_sq.column_int64(st, 0);
        if (r >= g_plo_n || src < 0 || src >= g_plo_n || !t ||
            (g_plo_have[src] && strncmp((const char *) t, g_plo_names[src], PL_TGT_NAME_MAX - 1)))
            ok = 0;
        r++;
    }
    g_sq.finalize(st);
    *names_ok = ok && r == g_plo_n;
    return r;
}
static int mv_plorder(uintptr_t conn, uintptr_t cache, FILE *lg) {
    void *c = (void *) conn;
    static long long ids[PLO_MAXROWS], after[PLO_MAXROWS];
    long long par[1] = {0}, a[4] = {0, 0, 0, 0};
    int from = (int) g_mv.from, to = (int) g_mv.to, n = g_plo_n, ok = 0;
    if (!cache)
        return -40;
    if (!g_sq.column_int64)
        return -7;
    if (sq_row(c, "SELECT playlist_id_parent FROM playlist WHERE playlist_id = ?1", g_mv.pl, -1,
               par, 1) != 1)
        return -20;
    if (n < 2 || n > PLO_MAXROWS || to < 1 || to > n || from < 1 || from > n)
        return -11;
    int got = plo_read(c, par[0], ids, PLO_MAXROWS, from, from, &ok);
    if (lg)
        fprintf(lg,
                "PLORDER check: parent %lld siblings %d (screen %d) names %s, id at #%d = %lld "
                "(want %u)\n",
                par[0], got, n, ok ? "match" : "DIFFER", from, got >= from ? ids[from - 1] : -1,
                g_mv.pl);
    if (got != n || !ok)
        return -12; /* the screen is not these siblings */
    if (ids[from - 1] != (long long) g_mv.pl)
        return -11;             /* not the row grabbed */
    jr_scope_parent(c, par[0]); /* undo: this list's order */
    int rc = lp_exec(c, "BEGIN", lg);
    for (int d = 0; !rc && d < n; d++)
        rc = sq_exec4(c, "UPDATE playlist SET sequenceNo = ?1 WHERE playlist_id = ?2", d + 1,
                      ids[plo_src(d, from, to)], 0, 0);
    if (!rc) {
        int got2 = plo_read(c, par[0], after, PLO_MAXROWS, from, to, &ok);
        if (got2 != n || !ok || after[to - 1] != (long long) g_mv.pl ||
            sq_row(c,
                   "SELECT COUNT(*), MIN(sequenceNo), MAX(sequenceNo), COUNT(DISTINCT sequenceNo) "
                   "FROM playlist "
                   "WHERE playlist_id_parent = ?1",
                   par[0], -1, a, 4) != 4 ||
            a[0] != n || a[1] != 1 || a[2] != n || a[3] != n)
            rc = -44;
    }
    lp_exec(c, rc ? "ROLLBACK" : "COMMIT", lg);
    if (lg)
        fprintf(lg, "PLORDER %u (parent %lld) #%d -> #%d of %d rc=%d\n", g_mv.pl, par[0], from, to,
                n, rc);
    if (rc)
        return rc > 0 ? -43 : rc;
    lib_invalidate(cache, c, lg);
    return 0;
}
/* A knob drag dropped on a folder: the siblings must still spell the snapshot (so
 * position `to` is the folder the DJ saw highlighted, and `from` the row grabbed);
 * then it is a mv_plmove into that folder. Library thread. */
static int mv_plmove(uintptr_t conn, uintptr_t cache, FILE *lg);
static int mv_plinto(uintptr_t conn, uintptr_t cache, FILE *lg) {
    void *c = (void *) conn;
    static long long ids[PLO_MAXROWS];
    long long par[1] = {0}, at[1] = {-1};
    int from = (int) g_mv.from, to = (int) g_mv.to, n = g_plo_n, ok = 0;
    if (!g_sq.column_int64)
        return -7;
    if (n < 2 || from < 1 || from > n || to < 1 || to > n || from == to)
        return -11;
    if (sq_row(c, "SELECT playlist_id_parent FROM playlist WHERE playlist_id = ?1", g_mv.pl, -1,
               par, 1) != 1)
        return -20;
    int got = plo_read(c, par[0], ids, PLO_MAXROWS, from, from, &ok);
    if (got != n || !ok || ids[from - 1] != (long long) g_mv.pl)
        return -12;
    long long folder = ids[to - 1];
    if (sq_row(c, "SELECT attribute FROM playlist WHERE playlist_id = ?1", folder, -1, at, 1) !=
            1 ||
        at[0] != PL_ATTR_FOLDER)
        return -12;
    if (!sq_text(c, "SELECT name FROM playlist WHERE playlist_id = ?1", folder, g_mv.name,
                 sizeof g_mv.name))
        return -20;
    if (lg)
        fprintf(lg, "PLINTO %u #%d -> folder %lld \"%s\" (#%d)\n", g_mv.pl, from, folder, g_mv.name,
                to);
    g_mv.from = g_mv.pl;
    g_mv.pl = (unsigned) folder;
    g_mv.to = 0; /* mv_plmove's fields: source, dest, after */
    return mv_plmove(conn, cache, lg);
}
static int mv_execute_op(void *server, FILE *lg, uintptr_t *conn_out);
/* Ops recorded in the undo journal. The track-list edits are scoped in
 * mv_execute_op; the tree edits scope themselves. */
static int mv_journaled(int op) {
    return op == MV_OP_ADD || op == MV_OP_ADDN || op == MV_OP_REMOVE || op == MV_OP_MOVE ||
           op == MV_OP_NEWPL || op == MV_OP_PLCOPY || op == MV_OP_PLMOVE || op == MV_OP_PLORDER ||
           op == MV_OP_PLINTO || op == MV_OP_RENAME || op == MV_OP_FDEL;
}
static void js_describe(int op) {
    const char *n = op == MV_OP_FDEL ? g_fd.name : g_mv.name;
    char q[80] = "";
    if (n && n[0])
        snprintf(q, sizeof q, " \"%.60s\"", n);
    switch (op) {
    case MV_OP_FDEL:
        snprintf(g_jr_desc, sizeof g_jr_desc, "Remove%s", q);
        break;
    case MV_OP_NEWPL:
        snprintf(g_jr_desc, sizeof g_jr_desc, "New%s", q);
        break;
    case MV_OP_RENAME:
        snprintf(g_jr_desc, sizeof g_jr_desc, "Rename to%s", q);
        break;
    case MV_OP_ADD:
        snprintf(g_jr_desc, sizeof g_jr_desc, "Add to%s", q);
        break;
    case MV_OP_ADDN:
        snprintf(g_jr_desc, sizeof g_jr_desc, "Add tracks to%s", q);
        break;
    case MV_OP_REMOVE:
        snprintf(g_jr_desc, sizeof g_jr_desc, "Remove track");
        break;
    case MV_OP_MOVE:
        snprintf(g_jr_desc, sizeof g_jr_desc, "Move track");
        break;
    case MV_OP_PLCOPY:
        snprintf(g_jr_desc, sizeof g_jr_desc, "Paste");
        break;
    case MV_OP_PLMOVE:
    case MV_OP_PLINTO:
    case MV_OP_PLORDER:
        snprintf(g_jr_desc, sizeof g_jr_desc, "Move playlist");
        break;
    default:
        snprintf(g_jr_desc, sizeof g_jr_desc, "Edit");
        break;
    }
}
static int mv_execute(void *server, FILE *lg) {
    int op = g_mv.op, j = mv_journaled(op), cont = j && g_jr.keep;
    if (j) {
        if (!g_jr.keep) {
            jr_clear();
            js_describe(op);
        } else
            __atomic_store_n(&g_jr.ready, 0, __ATOMIC_RELEASE);
        g_jr.media = g_mv.media;
    }
    if (j && cont && op == MV_OP_REMOVE)
        snprintf(g_jr_desc, sizeof g_jr_desc, "Move track"); /* a CUT/MOVE chain: one entry */
    uintptr_t conn = 0;
    int rc = mv_execute_op(server, lg, &conn);
    if (j) {
        if ((rc == 0 || (op == MV_OP_ADDN && g_tag_added > 0)) && conn)
            jr_after((void *) conn, cont);
        else if (!g_jr.keep)
            jr_clear();
        if (op != MV_OP_ADD)
            g_jr.keep = 0; /* a MOVE chain ends with its REMOVE */
    }
    return rc;
}
/* A playlist whose tracks are not numbered 1..N (duplicates, gaps, a stray
 * 18470936): renumber 1..N in (sequenceNo, rowid) order, the order rekordbox 7
 * renumbers to on quit, in one transaction, read back. Runs inside the edit's undo
 * journal. A REMOVE / MOVE still re-proves the displayed track's position, so a tie
 * EP147 shows in another order is refused, never guessed. 0 = already fine or fixed. */
static int plc_normalize(void *c, long long pl, FILE *lg) {
    long long a[4] = {0, 0, 0, 0};
    if (sq_row(c,
               "SELECT COUNT(*), MIN(sequenceNo), MAX(sequenceNo), COUNT(DISTINCT sequenceNo) FROM "
               "playlist_content "
               "WHERE playlist_id = ?1",
               pl, -1, a, 4) != 4)
        return -9;
    long long n = a[0];
    if (n == 0 || (a[1] == 1 && a[2] == n && a[3] == n))
        return 0;
    static long long rid[8192];
    if (n > 8192)
        return -46;
    void *st = 0;
    int k = 0, rc;
    if (g_sq.prepare(
            c,
            "SELECT rowid FROM playlist_content WHERE playlist_id = ?1 ORDER BY sequenceNo, rowid",
            -1, &st, 0) != 0 ||
        !st)
        return -9;
    g_sq.bind_int64(st, 1, pl);
    while (k < n && g_sq.step(st) == 100)
        rid[k++] = g_sq.column_int64(st, 0);
    g_sq.finalize(st);
    if (k != n)
        return -9;
    rc = lp_exec(c, "BEGIN", lg);
    for (int i = 0; !rc && i < k; i++)
        rc = sq_exec4(c, "UPDATE playlist_content SET sequenceNo = ?1 WHERE rowid = ?2", i + 1,
                      rid[i], 0, 0);
    long long b[4] = {0, 0, 0, 0};
    if (!rc && (sq_row(c,
                       "SELECT COUNT(*), MIN(sequenceNo), MAX(sequenceNo), COUNT(DISTINCT "
                       "sequenceNo) FROM playlist_content "
                       "WHERE playlist_id = ?1",
                       pl, -1, b, 4) != 4 ||
                b[0] != n || b[1] != 1 || b[2] != n || b[3] != n))
        rc = -44;
    lp_exec(c, rc ? "ROLLBACK" : "COMMIT", lg);
    if (lg)
        fprintf(
            lg,
            "RENUMBER pl=%lld: %lld tracks, seq was %lld..%lld (%lld distinct) -> 1..%lld rc=%d\n",
            pl, n, a[1], a[2], a[3], n, rc);
    return rc > 0 ? -43 : rc;
}
#define TLC_VT EP_2854578       /* music_library::TrackListCache (RTTI) */
#define TLC_ENTRY_FN EP_1224270 /* (cache, i, 0, 0) -> {x0 content_id, x1 skip byte} */
struct tlc_ent {
    uint64_t a, b;
};
/* Library thread: the Tag List's tracks into g_tag_cids. <0 = nothing read. Runs inside the
 * server's own onMessage (mlserver_onmsg_hook), on its thread: the chain from `server` down to
 * the cache is the server's own members, read with plain loads (own_q) and vtable-checked. */
static int tag_read(void *server, uintptr_t conn, FILE *lg) {
    uintptr_t disp = 0, h = 0, cm = 0, ab = 0, ae = 0, o = 0, lc = 0, vt = 0, fn = 0;
    g_tag_n = g_tag_skipped = 0;
    if (!own_q((uintptr_t) server + MLSERVER_DISP, &disp) || !own_q(disp + MLDISP_HANDLER, &h) ||
        !own_q(h + 16, &cm) || !cm || !own_q(cm + 80, &ab) || !own_q(cm + 88, &ae) || !ab ||
        ae <= ab || (ae - ab) % 24)
        return -91;
    if (!own_q(ab, &o) || !o || !own_q(o + 80, &lc) || !lc || (lc & 7) || !own_q(lc, &vt) ||
        vt != TLC_VT || !own_q(vt + 0x28, &fn) || !fn) {
        if (lg)
            fprintf(lg, "TAGREAD: no Tag List cache (slot 0 %lx cache %lx vt %lx)\n",
                    (unsigned long) o, (unsigned long) lc, (unsigned long) vt);
        return -91; /* the TAG LIST screen has not loaded it */
    }
    unsigned cnt = ((unsigned (*)(uintptr_t, int, int)) fn)(lc, 0, 0);
    if (lg)
        fprintf(lg, "TAGREAD: cache %lx count %u (screen %d)\n", (unsigned long) lc, cnt,
                g_tag_want);
    if (!cnt)
        return -90;
    if (cnt > TAG_MAX)
        return -92;
    if ((int) cnt != g_tag_want)
        return -93; /* not the list the DJ is looking at: refuse, never guess */
    for (unsigned i = 0; i < cnt; i++) {
        struct tlc_ent e =
            ((struct tlc_ent (*)(uintptr_t, unsigned, int, int)) TLC_ENTRY_FN)(lc, i, 0, 0);
        if (e.b & 0xff) {
            g_tag_skipped++;
            continue;
        } /* as stock: not this stick's */
        unsigned cid = (unsigned) (e.a & 0xffffffffu);
        long long r[1] = {0};
        if (sq_row((void *) conn, "SELECT COUNT(*) FROM content WHERE content_id = ?1", cid, -1, r,
                   1) != 1 ||
            r[0] != 1) {
            if (lg)
                fprintf(lg, "TAGREAD: entry %u cid %u not on this stick\n", i, cid);
            return -94;
        }
        g_tag_cids[g_tag_n++] = cid;
    }
    if (lg) {
        fprintf(lg, "TAGREAD: %d tracks:", g_tag_n);
        for (int i = 0; i < g_tag_n; i++)
            fprintf(lg, " %u", g_tag_cids[i]);
        fprintf(lg, " (%d skipped)\n", g_tag_skipped);
    }
    return g_tag_n ? 0 : -95;
}
/* Library thread: where the selected playlist sits in its list, in EP147's display
 * order (ORDER BY sequenceNo, playlist_id), and its neighbors' names. */
static int mk_read(uintptr_t conn, FILE *lg) {
    long long par[1] = {0};
    memset(&g_mkr, 0, sizeof g_mkr);
    if (sq_row((void *) conn, "SELECT playlist_id_parent FROM playlist WHERE playlist_id = ?1",
               g_mv.pl, -1, par, 1) != 1)
        return -20;
    void *st = 0;
    int k = 0, at = -1;
    char prev[64] = "", cur[64];
    if (g_sq.prepare((void *) conn,
                     "SELECT playlist_id, name FROM playlist WHERE playlist_id_parent = ?1 ORDER "
                     "BY sequenceNo, playlist_id",
                     -1, &st, 0) != 0 ||
        !st)
        return -9;
    g_sq.bind_int64(st, 1, par[0]);
    while (g_sq.step(st) == 100) {
        long long id = g_sq.column_int64(st, 0);
        const unsigned char *t = g_sq.column_text(st, 1);
        snprintf(cur, sizeof cur, "%s", t ? (const char *) t : "");
        if (at >= 0 && k == at + 1)
            utf8_copy(g_mkr.next, sizeof g_mkr.next, cur);
        if (id == g_mv.pl) {
            at = k;
            utf8_copy(g_mkr.name, sizeof g_mkr.name, cur);
            snprintf(g_mkr.prev, sizeof g_mkr.prev, "%s", prev);
        }
        snprintf(prev, sizeof prev, "%s", cur);
        k++;
    }
    g_sq.finalize(st);
    if (at < 0)
        return -20;
    g_mkr.rows = k;
    if (lg)
        fprintf(lg, "MKREAD pl=%u: row %d/%d \"%s\" between \"%s\" and \"%s\"\n", g_mv.pl, at, k,
                g_mkr.name, g_mkr.prev, g_mkr.next);
    return 0;
}
/* ---- library backup: before the first edit after each insertion -------------
 * An edit must never cost anyone their library. Before the first write to a stick's
 * exportLibrary.db after it was mounted, copy it over the older of two slots,
 * PIONEER/rekordbox/exportLibrary-BACKUP.db and -BACKUP-2.db (in place: see lb_copy),
 * so the newer backup is never touched. A restore = delete exportLibrary.db and every
 * exportLibrary.db-* (a stale journal would be applied to the restored file), then
 * rename the backup.
 *   - PRAGMA quick_check first: a failing library never overwrites a good backup,
 *     and the edit is refused (LB_BAD).
 *   - The copy is taken under a read transaction on EP147's own connection with no
 *     transaction of ours open (autocommit checked by the caller): no writer can
 *     commit mid-copy.
 *   - The copy is hashed as read, fsynced, read back and hashed again. A slot
 *     interrupted mid-write fails the check (the desktop restore refuses it) and is
 *     the slot the next backup overwrites.
 *   - Not enough room or any step failing: nothing changes and the edit is refused
 *     (LB_FAIL). No edit without a backup.
 * "Mounted" = the db file's (st_dev, st_ino): a re-insert or another stick is a new
 * pair. An EP147 restart also takes a new one (the two generations cover it).
 * Paired with export.pdb: the same moment's export.pdb goes into the matching slot
 * (export-BACKUP.pdb / export-BACKUP-2.pdb), so a restore never pairs one moment's
 * library with another's Device Library (rekordbox's "library inconsistency"). The
 * library thread is the only writer of export.pdb (pdbsync), so it cannot change
 * between the two copies. No export.pdb, or its copy failing: that slot's .pdb is
 * removed (a stale half is worse than none), and a failed copy refuses the edit.
 * The desktop app restores the pair. Library thread only. Logs to the caller's log. */
#define LB_BAD (-150)
#define LB_FAIL (-151)
#include <sys/statvfs.h>
static struct {
    char path[300];
    dev_t dev;
    ino_t ino;
} g_lb[4];
static uint64_t lb_fnv(uint64_t h, const unsigned char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}
/* Copy src over dst in place: no O_TRUNC, no rename. On FAT those are several
 * metadata writes, and a stick pulled mid-rotation can break both backups and leave
 * a FAT error that makes the deck mount the stick read-only. Writing over the slot's
 * existing clusters changes no FAT entry unless the file grows or shrinks, and
 * touches nothing of the other slot. */
static int lb_copy(const char *src, const char *dst, off_t want, FILE *lg) {
    int in = open(src, O_RDONLY | O_CLOEXEC), out = -1, rc = -1;
    unsigned char buf[1 << 14]; /* per call: two sticks' library threads can back up at once */
    uint64_t h1 = 1469598103934665603ull, h2 = h1;
    off_t got = 0;
    if (in < 0)
        goto done;
    out = open(dst, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if (out < 0)
        goto done;
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n < 0)
            goto done;
        if (n == 0)
            break;
        h1 = lb_fnv(h1, buf, (size_t) n);
        for (ssize_t w = 0; w < n;) {
            ssize_t k = pwrite(out, buf + w, (size_t) (n - w), got + w);
            if (k <= 0)
                goto done;
            w += k;
        }
        got += n;
    }
    if (got != want)
        goto done;
    {
        struct stat os;
        if (fstat(out, &os) == 0 && os.st_size > want && ftruncate(out, want) != 0)
            goto done;
    } /* only when it shrank */
    if (fsync(out) != 0)
        goto done;
    {
        off_t back = 0; /* read back what landed */
        for (ssize_t n; (n = pread(out, buf, sizeof buf, back)) > 0;) {
            h2 = lb_fnv(h2, buf, (size_t) n);
            back += n;
        }
        if (back != want || h1 != h2)
            goto done;
    }
    rc = 0;
done:
    if (lg)
        fprintf(lg, "BACKUP copy %s -> %s: %lld of %lld bytes, hash %s rc=%d\n", src, dst,
                (long long) got, (long long) want, h1 == h2 ? "match" : "MISMATCH", rc);
    if (in >= 0)
        close(in);
    if (out >= 0)
        close(out);
    return rc;
}
/* A slot that cannot be a complete SQLCipher file (missing, empty, not whole pages)
 * is the one to write; else the older of the two. */
static int lb_slot_ok(const char *p, struct stat *st) {
    return stat(p, st) == 0 && st->st_size >= 4096 && st->st_size % 1024 == 0;
}
static int lib_backup_one(void *conn, FILE *lg) {
    char path[300] = "";
    {
        void *st = 0; /* EP147's own file for this connection */
        if (g_sq.prepare(conn, "PRAGMA database_list", -1, &st, 0) != 0 || !st)
            return LB_FAIL;
        while (g_sq.step(st) == SQLITE_ROW) {
            const unsigned char *nm = g_sq.column_text(st, 1), *fn = g_sq.column_text(st, 2);
            if (nm && fn && !strcmp((const char *) nm, "main")) {
                snprintf(path, sizeof path, "%s", (const char *) fn);
                break;
            }
        }
        g_sq.finalize(st);
    }
    size_t L = strlen(path);
    const char *tail = "/exportLibrary.db";
    if (L < strlen(tail) || strcmp(path + L - strlen(tail), tail)) {
        if (lg)
            fprintf(lg, "BACKUP: not a stick library (%s): refused\n", path);
        return LB_FAIL;
    }
    struct stat sb;
    if (stat(path, &sb) != 0)
        return LB_FAIL;
    int slot = -1;
    for (int i = 0; i < 4; i++)
        if (!strcmp(g_lb[i].path, path)) {
            slot = i;
            break;
        }
    if (slot >= 0 && g_lb[slot].dev == sb.st_dev && g_lb[slot].ino == sb.st_ino)
        return 0; /* this insertion: done */
    /* 1. the library itself must be sound before it may replace a backup */
    char ok[32] = "";
    {
        void *st = 0;
        if (g_sq.prepare(conn, "PRAGMA quick_check", -1, &st, 0) == 0 && st) {
            if (g_sq.step(st) == SQLITE_ROW) {
                const unsigned char *t = g_sq.column_text(st, 0);
                if (t)
                    snprintf(ok, sizeof ok, "%s", (const char *) t);
            }
            g_sq.finalize(st);
        }
    }
    if (strcmp(ok, "ok")) {
        if (lg)
            fprintf(lg, "BACKUP: quick_check says \"%s\" on %s: backups kept, editing refused\n",
                    ok, path);
        return LB_BAD;
    }
    /* 2. a WAL with frames in it means the main file alone is not the library */
    char side[320];
    struct stat ws;
    snprintf(side, sizeof side, "%s-wal", path);
    if (stat(side, &ws) == 0 && ws.st_size > 0) {
        lp_exec(conn, "PRAGMA wal_checkpoint(TRUNCATE)", lg);
        if (stat(side, &ws) == 0 && ws.st_size > 0) {
            if (lg)
                fprintf(lg, "BACKUP: %s still holds %lld bytes after a checkpoint: refused\n", side,
                        (long long) ws.st_size);
            return LB_FAIL;
        }
    }
    /* 3. room for the copy, with margin */
    struct statvfs vs;
    char pdb[340];
    struct stat sp;
    int have_pdb;
    snprintf(pdb, sizeof pdb, "%.*s/export.pdb", (int) (L - strlen(tail)), path);
    have_pdb = stat(pdb, &sp) == 0 && sp.st_size > 0;
    unsigned long long need = (unsigned long long) sb.st_size * 2 +
                              (have_pdb ? (unsigned long long) sp.st_size * 2 : 0) + (1 << 20);
    if (statvfs(path, &vs) != 0 || (unsigned long long) vs.f_bavail * vs.f_frsize < need) {
        if (lg)
            fprintf(lg, "BACKUP: not enough free space for %llu bytes: refused\n", need);
        return LB_FAIL;
    }
    char dir[300], b1[340], b2[340], old_tmp[340];
    snprintf(dir, sizeof dir, "%.*s", (int) (L - strlen(tail)), path);
    snprintf(b1, sizeof b1, "%s/exportLibrary-BACKUP.db", dir);
    snprintf(b2, sizeof b2, "%s/exportLibrary-BACKUP-2.db", dir);
    snprintf(old_tmp, sizeof old_tmp, "%s/exportLibrary-BACKUP.db.tmp", dir);
    unlink(old_tmp); /* a temp an older tmp + rename backup may have left */
    /* 4. which slot: a broken one, else the older one, never the newer good one */
    struct stat s1, s2;
    int ok1 = lb_slot_ok(b1, &s1), ok2 = lb_slot_ok(b2, &s2);
    const char *dst = !ok1 ? b1 : !ok2 ? b2
                                       : (s1.st_mtime <= s2.st_mtime ? b1 : b2);
    /* 5. copy under a read transaction: SHARED lock held, nobody commits meanwhile */
    int rc = lp_exec(conn, "BEGIN", lg);
    long long cnt[1] = {0};
    if (!rc && sq_row(conn, "SELECT COUNT(*) FROM sqlite_master", 0, -1, cnt, 1) != 1)
        rc = -1;
    if (!rc) {
        if (stat(path, &sb) != 0)
            rc = -1; /* the size under the lock */
        else
            rc = lb_copy(path, dst, sb.st_size, lg);
    }
    lp_exec(conn, "COMMIT", lg);
    if (rc)
        return LB_FAIL; /* the other slot is untouched */
    {                   /* the same moment's export.pdb into the matching slot (see the header) */
        char pdst[340];
        snprintf(pdst, sizeof pdst, "%s/%s", dir,
                 dst == b1 ? "export-BACKUP.pdb" : "export-BACKUP-2.pdb");
        if (have_pdb && stat(pdb, &sp) == 0 && sp.st_size > 0) {
            if (lb_copy(pdb, pdst, sp.st_size, lg)) {
                unlink(pdst);
                if (lg)
                    fprintf(
                        lg,
                        "BACKUP: %s -> %s FAILED: that slot holds no export.pdb, editing refused\n",
                        pdb, pdst);
                return LB_FAIL;
            }
        } else
            unlink(pdst); /* no export.pdb now: no stale half in this slot */
    }
    {
        int dfd = open(dir, O_RDONLY | O_CLOEXEC);
        if (dfd >= 0) {
            fsync(dfd);
            close(dfd);
        }
    }
    if (slot < 0) {
        for (int i = 0; i < 4; i++)
            if (!g_lb[i].path[0]) {
                slot = i;
                break;
            }
    }
    if (slot < 0)
        slot = 0;
    snprintf(g_lb[slot].path, sizeof g_lb[slot].path, "%s", path);
    g_lb[slot].dev = sb.st_dev;
    g_lb[slot].ino = sb.st_ino;
    if (lg)
        fprintf(lg, "BACKUP: %s -> %s (%lld bytes)%s; the other slot kept\n", path, dst,
                (long long) sb.st_size,
                have_pdb ? " + export.pdb into the matching slot"
                         : " (no export.pdb on this stick)");
    return 0;
}
/* g_lb (which slot each library last wrote) is shared by every stick's library thread: one
 * backup at a time. */
static pthread_mutex_t g_lb_lock = PTHREAD_MUTEX_INITIALIZER;
static int lib_backup(void *conn, FILE *lg) {
    pthread_mutex_lock(&g_lb_lock);
    int rc = lib_backup_one(conn, lg);
    pthread_mutex_unlock(&g_lb_lock);
    return rc;
}
static int mv_writes(int op) {
    return mv_journaled(op) || op == MV_OP_UNDO;
}
static int mv_execute_op(void *server, FILE *lg, uintptr_t *conn_out) {
    uintptr_t svc = 0, lib = 0, db = 0, conn = 0, magic = 0;
    unsigned media = 0;
    int ch = mlserver_chain(server, &svc, &lib, &db, &media);
    if (ch)
        return ch;
    /* Must be the same stick the playlist was browsed on. The onmsg hook already
     * filters by server; this re-checks it at the point of use. */
    if (media != g_mv.media) {
        if (lg)
            fprintf(lg, "MEDIA MISMATCH: want %04x, this server's db %lx carries %04x\n",
                    g_mv.media, (unsigned long) db, media);
        return -5;
    }
    if (!own_q(db + SQLDB_CONN, &conn) || !conn || !own_q(conn + SQLITE_MAGIC_Q, &magic) ||
        (unsigned) (magic & 0xffffffff) != SQLITE_MAGIC_OPEN)
        return -6;
    if (!sq_resolve())
        return -7;
    if (g_sq.get_autocommit((void *) conn) != 1)
        return -8;
    *conn_out = conn;
    if (mv_writes(g_mv.op)) { /* no edit without a backup, before any write (renumber too) */
        int br = lib_backup((void *) conn, lg);
        if (br)
            return br;
    }
    if (mv_journaled(g_mv.op)) { /* undo scope of the track-list edits and NEW PLAYLIST/FOLDER */
        if (g_mv.op == MV_OP_ADD || g_mv.op == MV_OP_ADDN || g_mv.op == MV_OP_REMOVE ||
            g_mv.op == MV_OP_MOVE) {
            jr_scope_pc((void *) conn, g_mv.pl);
            int nr = plc_normalize((void *) conn, g_mv.pl,
                                   lg); /* odd numbering: repair, as rekordbox does */
            if (nr)
                return nr;
        }
    }
    if (g_mv.op == MV_OP_UNDO) {
        uintptr_t cache = 0;
        own_q(svc + INFOUPD_CACHE, &cache);
        return jr_undo(conn, cache, lg);
    }
    if (g_mv.op == MV_OP_ADD)
        return mv_add(svc, lib, conn, lg);
    if (g_mv.op == MV_OP_TAGREAD)
        return tag_read(server, conn, lg);
    if (g_mv.op == MV_OP_MKREAD)
        return mk_read(conn, lg);
    if (g_mv.op == MV_OP_ADDN) { /* each one exactly as a single ADD: proved, written, read back */
        int rc = 0;
        g_tag_added = 0;
        /* All or nothing: every track must be on this stick before the first is
         * written, so a missing track never leaves a partial edit behind. */
        for (int i = 0; i < g_tag_n; i++) {
            long long r[1] = {0};
            if (sq_row((void *) conn, "SELECT COUNT(*) FROM content WHERE content_id = ?1",
                       g_tag_cids[i], -1, r, 1) != 1 ||
                r[0] != 1) {
                if (lg)
                    fprintf(lg, "ADDN pl=%u: track %u (#%d) is not on this stick: nothing added\n",
                            g_mv.pl, g_tag_cids[i], i + 1);
                return -94;
            }
        }
        for (int i = 0; i < g_tag_n && !rc; i++) {
            g_mv.cid = g_tag_cids[i];
            rc = mv_add(svc, lib, conn, lg);
            if (!rc)
                g_tag_added++;
        }
        if (lg)
            fprintf(lg, "ADDN pl=%u: %d of %d added rc=%d\n", g_mv.pl, g_tag_added, g_tag_n, rc);
        return rc;
    }
    if (g_mv.op == MV_OP_CPREAD)
        return cp_read(conn, lg);
    if (g_mv.op == MV_OP_RSREAD)
        return rs_read(conn, lg);
    if (g_mv.op == MV_OP_NEWPL) {
        uintptr_t cache = 0;
        own_q(svc + INFOUPD_CACHE, &cache);
        return nl_create(conn, cache, lg);
    }
    if (g_mv.op == MV_OP_RENAME) {
        uintptr_t cache = 0;
        own_q(svc + INFOUPD_CACHE, &cache);
        return nl_rename(conn, cache, lg);
    }
    if (g_mv.op == MV_OP_FDCOUNT)
        return fd_collect(conn, lg);
    if (g_mv.op == MV_OP_FDEL) {
        uintptr_t cache = 0;
        own_q(svc + INFOUPD_CACHE, &cache);
        return fd_delete(conn, cache, lg);
    }
    if (g_mv.op == MV_OP_PLCOPY) {
        uintptr_t cache = 0;
        own_q(svc + INFOUPD_CACHE, &cache);
        return mv_plcopy(conn, cache, lg);
    }
    if (g_mv.op == MV_OP_PLMOVE) {
        uintptr_t cache = 0;
        own_q(svc + INFOUPD_CACHE, &cache);
        return mv_plmove(conn, cache, lg);
    }
    if (g_mv.op == MV_OP_PLORDER) {
        uintptr_t cache = 0;
        own_q(svc + INFOUPD_CACHE, &cache);
        return mv_plorder(conn, cache, lg);
    }
    if (g_mv.op == MV_OP_PLINTO) {
        uintptr_t cache = 0;
        own_q(svc + INFOUPD_CACHE, &cache);
        return mv_plinto(conn, cache, lg);
    }
    if (g_mv.op == MV_OP_TARGET) { /* read-only: is it a plain list, and its name */
        cp_mount_of((void *) conn, g_mv.mount,
                    sizeof g_mv.mount); /* for a cross-stick copy later */
        long long at[1] = {0};
        if (sq_row((void *) conn, "SELECT attribute FROM playlist WHERE playlist_id = ?1", g_mv.pl,
                   -1, at, 1) != 1)
            return -20;
        /* a folder may be a target too: playlists copied from another USB land in it */
        if (at[0] != PL_ATTR_LIST && at[0] != PL_ATTR_FOLDER)
            return -21;
        g_mv.n = at[0]; /* the attribute, for pl_target_store */
        if (!sq_text((void *) conn, "SELECT name FROM playlist WHERE playlist_id = ?1", g_mv.pl,
                     g_mv.name, sizeof g_mv.name) ||
            !g_mv.name[0])
            return -20;
        {
            long long pp[1] = {0}; /* where "next to this row" is (PASTE of a playlist) */
            g_mv.parent = 0;
            g_mv.pname[0] = 0;
            if (sq_row((void *) conn,
                       "SELECT playlist_id_parent FROM playlist WHERE playlist_id = ?1", g_mv.pl,
                       -1, pp, 1) == 1 &&
                pp[0] > 0) {
                g_mv.parent = (unsigned) pp[0];
                if (!sq_text((void *) conn, "SELECT name FROM playlist WHERE playlist_id = ?1",
                             g_mv.parent, g_mv.pname, sizeof g_mv.pname))
                    return -20;
            }
        }
        if (lg)
            fprintf(lg, "TARGET pl=%u \"%s\" parent %u \"%s\"\n", g_mv.pl, g_mv.name, g_mv.parent,
                    g_mv.pname);
        return 0;
    }
    long long a[4] = {0, 0, 0, 0};
    if (sq_row((void *) conn,
               "SELECT COUNT(*), MIN(sequenceNo), MAX(sequenceNo), "
               "COUNT(DISTINCT sequenceNo) FROM playlist_content WHERE playlist_id = ?1",
               g_mv.pl, -1, a, 4) != 4)
        return -9;
    long long n = a[0];
    /* a reorder needs two rows; a REMOVE (also the second half of a track MOVE) may
     * take the last one */
    if (n < (g_mv.op == MV_OP_MOVE ? 2 : 1) || a[1] != 1 || a[2] != n || a[3] != n)
        return -10;
    if (g_mv.from < 1 || g_mv.from > n)
        return -11;
    if (g_mv.op == MV_OP_MOVE && (g_mv.to < 1 || g_mv.to > n || g_mv.from == g_mv.to))
        return -11;
    long long cid[1] = {0};
    if (sq_row((void *) conn,
               "SELECT content_id FROM playlist_content "
               "WHERE playlist_id = ?1 AND sequenceNo = ?2",
               g_mv.pl, g_mv.from, cid, 1) != 1 ||
        cid[0] != g_mv.cid)
        return -12;
    if (g_mv.op == MV_OP_REMOVE) {
        if (lg)
            fprintf(lg, "REMOVE verified: pl=%u n=%lld seq %u holds cid %u\n", g_mv.pl, n,
                    g_mv.from, g_mv.cid);
        g_mv.n = n; /* the message thread re-selects from it */
        int (*rm)(void *, unsigned, unsigned, unsigned) =
            (int (*)(void *, unsigned, unsigned, unsigned)) PL_REMOVE_TRACK;
        int rc = rm((void *) svc, g_mv.pl, g_mv.cid, g_mv.from);
        if (rc != 0)
            return rc;
        /* Read it back: one row fewer, and still 1..N with no gaps or duplicates. */
        long long b[4] = {0, 0, 0, 0};
        if (sq_row((void *) conn,
                   "SELECT COUNT(*), MIN(sequenceNo), MAX(sequenceNo), "
                   "COUNT(DISTINCT sequenceNo) FROM playlist_content WHERE playlist_id = ?1",
                   g_mv.pl, -1, b, 4) != 4)
            return -13;
        if (b[0] != n - 1 || (b[0] > 0 && (b[1] != 1 || b[2] != b[0] || b[3] != b[0])))
            return -13;
        return 0;
    }
    if (lg)
        fprintf(lg, "COMMIT verified: pl=%u n=%lld seq %u holds cid %u; moving to %u\n", g_mv.pl, n,
                g_mv.from, g_mv.cid, g_mv.to);
    int (*mv)(void *, unsigned, unsigned, unsigned, unsigned) =
        (int (*)(void *, unsigned, unsigned, unsigned, unsigned)) PL_MOVE_TRACK;
    int rc = mv((void *) svc, g_mv.pl, g_mv.cid, g_mv.from, g_mv.to);
    if (rc != 0)
        return rc;
    /* Read it back: the track must now sit at `to`. */
    cid[0] = 0;
    if (sq_row((void *) conn,
               "SELECT content_id FROM playlist_content "
               "WHERE playlist_id = ?1 AND sequenceNo = ?2",
               g_mv.pl, g_mv.to, cid, 1) != 1 ||
        cid[0] != g_mv.cid)
        return -13;
    return 0;
}
/* ---- library spool: new rows for a stick, applied in place (no eject) -------
 * A writer (cross-stick copy) puts its files first, then its rows as SQL
 * in <stick>/PIONEER/azspool/<name>.sql (.tmp + rename). Each stick's library thread
 * finds its own spool from EP147's database filename and applies the file as one
 * transaction on EP147's own connection at a message boundary (EP147's own writes run
 * on this thread, so nothing interleaves; SQLCipher and locking stay EP147's), then
 * drops the list caches so the screen re-reads in place. Runs while a cross-stick copy
 * is in flight and, after one that stopped waiting, until its spool is applied (also once
 * at start, for a spool an EP147 restart left); otherwise idle.
 * A spool line is one statement ending in ';': INSERT [OR IGNORE] INTO or UPDATE on
 * the library tables; blank and '--' lines are skipped ("-- AZMOD art" lines name a
 * copied track's art, for ls_art_fix after the statements). Anything else rejects the
 * whole file (renamed .rejected); a failed statement rolls the whole file back
 * (renamed .failed); applied = deleted. Log: /tmp/azmod-libspool.log. */
#define LC_DROP_PLLIST EP_12439e8 /* removePlaylistListCache(u32 parent) */
#define LC_DROP_ALBUMS EP_1243ba8
#define LC_DROP_ARTISTS EP_1243c18
#define LC_DROP_GENRES EP_1243c88
#define LC_DROP_LABELS EP_1243cf8
#define LC_DROP_KEYS EP_1243d68
#define LC_DROP_RATINGS EP_1243dd8
#define LC_DROP_BPMS EP_1243e48
#define LC_DROP_ALLRATE EP_1243eb8
#define LC_DROP_ALLKEY EP_1243f28
#define LC_DROP_ALLBPM EP_1243f98
/* Every list a new track can appear in. Cache drops only: EP147 re-reads what
 * is on screen, so dropping one too many costs a re-query, never a wrong row. */
static void lib_invalidate(uintptr_t cache, void *conn, FILE *lg) {
    const uintptr_t whole[] = {LC_DROP_TRACKS, LC_DROP_ALBUMS, LC_DROP_ARTISTS, LC_DROP_GENRES,
                               LC_DROP_LABELS, LC_DROP_KEYS, LC_DROP_RATINGS, LC_DROP_BPMS,
                               LC_DROP_ALLRATE, LC_DROP_ALLKEY, LC_DROP_ALLBPM};
    for (size_t i = 0; i < sizeof whole / sizeof whole[0]; i++)
        ((void (*)(uintptr_t)) whole[i])(cache);
    int npl = 0;
    void *st = 0;
    if (g_sq.prepare(conn, "SELECT playlist_id, playlist_id_parent FROM playlist", -1, &st, 0) ==
            0 &&
        st) {
        while (g_sq.step(st) == SQLITE_ROW) {
            ((void (*)(uintptr_t, unsigned)) PLCACHE_DROP_PL)(cache,
                                                              (unsigned) g_sq.column_int64(st, 0));
            ((void (*)(uintptr_t, unsigned)) LC_DROP_PLLIST)(cache,
                                                             (unsigned) g_sq.column_int64(st, 1));
            npl++;
        }
    }
    if (st)
        g_sq.finalize(st);
    ((void (*)(uintptr_t, unsigned)) LC_DROP_PLLIST)(cache, 0); /* the root playlist list */
    ((void (*)(uintptr_t, int)) PLCACHE_NOTE)(cache, 5); /* the playlist hierarchy */
    if (lg)
        fprintf(lg, "  invalidated: 11 whole lists, %d playlist(s), hierarchy 5\n", npl);
}
/* clang-format off */
static const char *const LS_TABLES[] = {"content", "artist", "album",    "genre",
                                        "key",     "image",  "playlist", "playlist_content",
                                        "label",   0};
/* clang-format on */
/* 1 = the statement is one the spool may carry */
static int ls_allowed(const char *l) {
    const char *t = 0;
    if (!strncmp(l, "INSERT INTO ", 12))
        t = l + 12;
    else if (!strncmp(l, "INSERT OR IGNORE INTO ", 22))
        t = l + 22;
    else if (!strncmp(l, "UPDATE ", 7))
        t = l + 7;
    if (!t)
        return 0;
    for (int i = 0; LS_TABLES[i]; i++) {
        size_t n = strlen(LS_TABLES[i]);
        if (!strncmp(t, LS_TABLES[i], n) && (t[n] == ' ' || t[n] == '('))
            return 1;
    }
    return 0;
}
static const char *(*g_sq_db_filename)(void *, const char *);
#include "pdb_sync.c" /* export.pdb kept consistent after a cross-USB paste (libc only) */
/* After a spool's statements, inside its transaction, on the stick's library thread: each
 * LS_ART_TAG line names a pasted track's path and the art the copy wrote for it. A track the
 * target already had keeps the target's own content row (cp_track_sql inserts one only for a
 * new path), so it shows whatever art that row names, and on a stick whose artwork files are
 * gone that is a missing file. Every content row with the path whose art is unset, names no
 * image row, or names a file not on this stick is pointed at the copied art; a row whose own
 * art file is on the stick keeps it. Only art under PS_AZCP_ART (where the copy writes) and
 * already on the stick is taken. buf: n bytes, lines ended by '\n', '\0' or
 * 0x01. Returns the rows changed, or -1 when a statement failed (the rows before it stand). */
static int ls_art_fix(void *conn, const char *root, const char *buf, size_t n, FILE *lg) {
    static void *bind_sym; /* every stick's library thread runs this */
    void *f = __atomic_load_n(&bind_sym, __ATOMIC_ACQUIRE);
    if (!f && (f = dlsym(RTLD_DEFAULT, "sqlite3_bind_text")))
        __atomic_store_n(&bind_sym, f, __ATOMIC_RELEASE);
    int (*bind_text)(void *, int, const char *, int, void *);
    *(void **) &bind_text = f;
    if (!bind_text)
        return -1;
    int fixed = 0;
    for (const char *l = buf, *e; l < buf + n; l = e + 1) {
        for (e = l; e < buf + n && *e && *e != '\n' && *e != 0x01;)
            e++;
        size_t tl = sizeof LS_ART_TAG - 1;
        if ((size_t) (e - l) <= tl || memcmp(l, LS_ART_TAG, tl))
            continue;
        const char *p = l + tl, *tab = memchr(p, '\t', (size_t) (e - p));
        char path[512], art[256];
        if (!tab || tab == p || (size_t) (tab - p) >= sizeof path ||
            (size_t) (e - tab - 1) >= sizeof art)
            continue;
        snprintf(path, sizeof path, "%.*s", (int) (tab - p), p);
        snprintf(art, sizeof art, "%.*s", (int) (e - tab - 1), tab + 1);
        if (path[0] != '/' || strncmp(art, PS_AZCP_ART, sizeof PS_AZCP_ART - 1) ||
            strstr(art, "/../") || strchr(art, '\t') || !ps_file_ok(root, art))
            continue;
        void *st = 0;
        long long nid = 0, ids[16];
        int nids = 0;
        if (g_sq.prepare(conn, "SELECT image_id FROM image WHERE path = ?1 LIMIT 1", -1, &st, 0) ||
            !st)
            return -1;
        bind_text(st, 1, art, -1, (void *) -1);
        if (g_sq.step(st) == SQLITE_ROW)
            nid = g_sq.column_int64(st, 0);
        g_sq.finalize(st);
        st = 0;
        if (nid <= 0)
            continue; /* the spool's image row is not there: nothing to point at */
        if (g_sq.prepare(conn,
                         "SELECT c.content_id, i.path FROM content c LEFT JOIN image i ON "
                         "i.image_id = c.image_id WHERE c.path = ?1",
                         -1, &st, 0) ||
            !st)
            return -1;
        bind_text(st, 1, path, -1, (void *) -1);
        while (nids < 16 && g_sq.step(st) == SQLITE_ROW) {
            const unsigned char *cur = g_sq.column_text(st, 1);
            if (!ps_file_ok(root, cur ? (const char *) cur : 0))
                ids[nids++] = g_sq.column_int64(st, 0);
        }
        g_sq.finalize(st);
        for (int k = 0; k < nids; k++) {
            st = 0;
            if (g_sq.prepare(conn, "UPDATE content SET image_id = ?1 WHERE content_id = ?2", -1,
                             &st, 0) ||
                !st)
                return -1;
            g_sq.bind_int64(st, 1, nid);
            g_sq.bind_int64(st, 2, ids[k]);
            int rc = g_sq.step(st);
            g_sq.finalize(st);
            if (rc != 101)
                return -1;
            fixed++;
            if (lg)
                fprintf(lg, "  art: content %lld (%s) -> image %lld (%s)\n", ids[k], path, nid, art);
        }
    }
    return fixed;
}
static char *ls_dup(const unsigned char *s) {
    return strdup(s ? (const char *) s : "");
}
/* After a cross-USB paste's spool COMMITs, on this library thread: bring the stick's
 * export.pdb in line with what was just written, so rekordbox shows no "library
 * inconsistency" page. Runs before the spool file is deleted, since that is what
 * reports the paste done. Any failure leaves export.pdb untouched: the OneLibrary
 * paste stands and only the warning page remains.
 * root: the stick ("/media/usb/sda"); src: a paste's source stick, or 0 (a same-stick
 * edit only re-mirrors the playlists). Returns pdbs_sync's result (0 updated, >0 nothing
 * to do), or -1 when the library could not be read. */
static int ls_pdb_sync_root(void *conn, const char *root, const char *src, const char *why,
                            FILE *lg) {
    const char *b0 = why;
    struct pdbs_pl *pl = 0;
    struct pdbs_item *it = 0;
    struct pdbs_track *tr = 0;
    int npl = 0, nit = 0, ntr = 0, cpl = 0, cit = 0, ctr = 0, bad = 0;
    void *st = 0;
    if (g_sq.prepare(
            conn,
            "SELECT playlist_id, playlist_id_parent, sequenceNo, attribute, name FROM playlist", -1,
            &st, 0) == 0 &&
        st) {
        while (!bad && g_sq.step(st) == SQLITE_ROW) {
            if (npl == cpl) {
                void *n = realloc(pl, (cpl = cpl ? cpl * 2 : 64) * sizeof *pl);
                if (!n) {
                    bad = 1;
                    break;
                }
                pl = n;
            }
            pl[npl++] = (struct pdbs_pl) {
                (unsigned) g_sq.column_int64(st, 0), (unsigned) g_sq.column_int64(st, 1),
                (unsigned) g_sq.column_int64(st, 2), g_sq.column_int64(st, 3) == PL_ATTR_FOLDER,
                ls_dup(g_sq.column_text(st, 4))};
            if (!pl[npl - 1].name)
                bad = 1; /* out of memory: counted, so the cleanup below frees the row */
        }
        g_sq.finalize(st);
        st = 0;
    } else
        bad = 1;
    if (!bad &&
        g_sq.prepare(conn,
                     "SELECT pc.playlist_id, c.path FROM playlist_content pc JOIN content c USING "
                     "(content_id) "
                     "ORDER BY pc.playlist_id, pc.sequenceNo, pc.rowid",
                     -1, &st, 0) == 0 &&
        st) {
        while (!bad && g_sq.step(st) == SQLITE_ROW) {
            if (nit == cit) {
                void *n = realloc(it, (cit = cit ? cit * 2 : 256) * sizeof *it);
                if (!n) {
                    bad = 1;
                    break;
                }
                it = n;
            }
            it[nit++] = (struct pdbs_item) {(unsigned) g_sq.column_int64(st, 0),
                                            ls_dup(g_sq.column_text(st, 1))};
            if (!it[nit - 1].path)
                bad = 1;
        }
        g_sq.finalize(st);
        st = 0;
    } else
        bad = 1;
    if (!bad &&
        g_sq.prepare(
            conn,
            "SELECT c.path, i.path, c.analysisDataFilePath FROM content c LEFT JOIN image i ON "
            "i.image_id = c.image_id",
            -1,
            &st, 0) == 0 &&
        st) {
        while (!bad && g_sq.step(st) == SQLITE_ROW) {
            if (ntr == ctr) {
                void *n = realloc(tr, (ctr = ctr ? ctr * 2 : 256) * sizeof *tr);
                if (!n) {
                    bad = 1;
                    break;
                }
                tr = n;
            }
            tr[ntr++] = (struct pdbs_track) {ls_dup(g_sq.column_text(st, 0)),
                                             ls_dup(g_sq.column_text(st, 1)),
                                             ls_dup(g_sq.column_text(st, 2))};
            if (!tr[ntr - 1].path || !tr[ntr - 1].art || !tr[ntr - 1].anlz)
                bad = 1;
        }
        g_sq.finalize(st);
        st = 0;
    } else
        bad = 1;
    if (lg)
        fprintf(lg, "PDB: %s after %s (source %s)\n", root, b0, src ? src : "none");
    int rc = -1;
    if (!bad && ntr)
        rc = pdbs_sync(root, src, pl, npl, it, nit, tr, ntr, lg);
    else if (lg)
        fprintf(lg, "PDB: library read failed: export.pdb untouched\n");
    for (int i = 0; i < npl; i++)
        free((void *) pl[i].name);
    for (int i = 0; i < nit; i++)
        free((void *) it[i].path);
    for (int i = 0; i < ntr; i++) {
        free((void *) tr[i].path);
        free((void *) tr[i].art);
        free((void *) tr[i].anlz);
    }
    free(pl);
    free(it);
    free(tr);
    return rc;
}
/* hint: the source stick the spool's first line names ("" when it names none). Used
 * when no copy to this stick is in flight: a spool applied after its copy stopped waiting. */
static void ls_pdb_sync(void *conn, const char *spool, const char *hint, FILE *lg) {
    const char *b = strrchr(spool, '/'), *cut = strstr(spool, "/PIONEER/azspool/");
    if (!b || strncmp(b + 1, "azcp-", 5) || !cut)
        return; /* not a cross-stick copy's spool: no source rows */
    char root[300];
    snprintf(root, sizeof root, "%.*s", (int) (cut - spool), spool);
    const char *src = 0;
    if (__atomic_load_n(&g_cp.state, __ATOMIC_ACQUIRE) == CP_COPYING &&
        !strcmp(g_cp.dst_mount, root))
        src = g_cp.src_mount;
    else if (hint[0] && strcmp(hint, root))
        src = hint;
    ls_pdb_sync_root(conn, root, src, b + 1, lg);
}
/* The source stick a cross-stick spool names on its first line, into out (else "").
 * Only a plain "/media/usb/<name>" is taken: the file sits on a stick anyone can write,
 * and the path is only ever read from (its export.pdb). */
static void ls_source_hint(const char *buf, char *out, size_t n) {
    static const char tag[] = "-- AZMOD cross-stick copy from /media/usb/";
    out[0] = 0;
    if (strncmp(buf, tag, sizeof tag - 1))
        return;
    const char *name = buf + sizeof tag - 1;
    size_t k = strcspn(name, "\r\n");
    if (!k || k > 64 || memchr(name, '/', k) || (name[0] == '.'))
        return;
    snprintf(out, n, "/media/usb/%.*s", (int) k, name);
}
#define LS_SPOOL_CAP (1 << 20) /* 1 MB cap per spool */
/* One spool file, on the library thread, read into buf (LS_SPOOL_CAP bytes: per call, since
 * every stick's library thread applies its own). 0 = applied (deleted); <0 = not applied. */
static int ls_apply_buf(void *conn, uintptr_t cache, const char *path, char *buf, FILE *lg) {
    FILE *f = fopen(path, "re");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, LS_SPOOL_CAP - 1, f);
    fclose(f);
    buf[n] = 0;
    char bad[700], hint[96];
    ls_source_hint(buf, hint, sizeof hint);
    if (n >= LS_SPOOL_CAP - 1) {
        snprintf(bad, sizeof bad, "%s.rejected", path);
        rename(path, bad);
        if (lg)
            fprintf(lg, "REJECTED %s: over 1 MB\n", path);
        return -2;
    }
    if (memchr(buf, 0, n)) { /* both passes stop at a NUL: what follows would be dropped unread */
        snprintf(bad, sizeof bad, "%s.rejected", path);
        rename(path, bad);
        if (lg)
            fprintf(lg, "REJECTED %s: a NUL byte\n", path);
        return -3;
    }
    /* pass 1: every line must be allowed; nothing is written if one is not */
    int nst = 0;
    for (char *l = buf; l && *l;) {
        char *e = strchr(l, '\n');
        if (e)
            *e = 0;
        while (*l == ' ' || *l == '\t' || *l == '\r')
            l++;
        size_t k = strlen(l);
        while (k && (l[k - 1] == '\r' || l[k - 1] == ' '))
            k--; /* measured only: a NUL written here would end pass 2's line early (strchr) */
        if (*l && strncmp(l, "--", 2)) {
            /* 0x01 is pass 2's line mark: one inside a line would run what follows it unchecked */
            if (!ls_allowed(l) || k < 2 || l[k - 1] != ';' || memchr(l, ';', k - 1) ||
                memchr(l, 0x01, k)) {
                if (lg)
                    fprintf(lg, "REJECTED %s: not allowed: %.120s\n", path, l);
                snprintf(bad, sizeof bad, "%s.rejected", path);
                rename(path, bad);
                return -3;
            }
            nst++;
        }
        if (e) {
            *e = 0x01; /* mark the line end for pass 2 */
            l = e + 1;
        } else
            break;
    }
    /* pass 2: one transaction, after the stick's backup (cross-USB paste lands here) */
    if (g_sq.get_autocommit(conn) != 1)
        return -4;              /* EP147 mid-transaction: the spool stays, next tick */
    if (lib_backup(conn, lg)) { /* no backup = nothing applied; .failed reports it (cp_*) */
        snprintf(bad, sizeof bad, "%s.failed", path);
        rename(path, bad);
        if (lg)
            fprintf(lg, "FAILED %s: no backup of this library, nothing applied\n", path);
        return -4;
    }
    int rc = lp_exec(conn, "BEGIN", 0), done = 0;
    for (char *l = buf; !rc && l && *l;) {
        char *e = strchr(l, 0x01);
        if (e)
            *e = 0;
        while (*l == ' ' || *l == '\t' || *l == '\r')
            l++;
        if (*l && strncmp(l, "--", 2)) {
            rc = lp_exec(conn, l, 0);
            if (!rc)
                done++;
            else if (lg)
                fprintf(lg, "FAILED rc=%d at: %.160s\n", rc, l);
        }
        l = e ? e + 1 : 0;
    }
    if (!rc) { /* a pasted track the stick already had: the copied art when its own is gone */
        const char *cut = strstr(path, "/PIONEER/azspool/");
        char root[600];
        snprintf(root, sizeof root, "%.*s", cut ? (int) (cut - path) : 0, path);
        if (cut && ls_art_fix(conn, root, buf, n, lg) < 0 && lg)
            fprintf(lg, "  art: a statement failed; the rows before it and the spool's stand\n");
    }
    lp_exec(conn, rc ? "ROLLBACK" : "COMMIT", 0);
    if (rc) {
        snprintf(bad, sizeof bad, "%s.failed", path);
        rename(path, bad);
        if (lg)
            fprintf(lg, "ROLLED BACK %s (%d/%d statements ran)\n", path, done, nst);
        return -4;
    }
    if (lg)
        fprintf(lg, "APPLIED %s: %d statement(s)\n", path, nst);
    ls_pdb_sync(conn, path, hint, lg); /* before the unlink: that reports the paste done */
    unlink(path);
    lib_invalidate(cache, conn, lg);
    return 0;
}
static int ls_apply_file(void *conn, uintptr_t cache, const char *path, FILE *lg) {
    char *buf = malloc(LS_SPOOL_CAP);
    if (!buf)
        return -1; /* the spool stays: next tick */
    int rc = ls_apply_buf(conn, cache, path, buf, lg);
    free(buf);
    return rc;
}
static _Atomic int g_ls_armed; /* message tick: some stick has a spool */
/* Library thread: this stick's spool dir, from EP147's own database path. */
static void ls_service(void *server) {
    uintptr_t svc = 0, lib = 0, db = 0, conn = 0, magic = 0, cache = 0;
    unsigned media = 0;
    if (mlserver_chain(server, &svc, &lib, &db, &media))
        return;
    if (!own_q(db + SQLDB_CONN, &conn) || !conn || !own_q(conn + SQLITE_MAGIC_Q, &magic) ||
        (unsigned) (magic & 0xffffffff) != SQLITE_MAGIC_OPEN)
        return;
    if (!sq_resolve())
        return;
    if (!g_sq_db_filename)
        *(void **) &g_sq_db_filename = dlsym(RTLD_DEFAULT, "sqlite3_db_filename");
    if (!g_sq_db_filename || g_sq.get_autocommit((void *) conn) != 1)
        return; /* EP147 mid-transaction: next message */
    if (!own_q(svc + INFOUPD_CACHE, &cache) || !cache)
        return;
    const char *dbp = g_sq_db_filename((void *) conn, "main");
    const char *cut = dbp ? pd_cut(dbp, "rekordbox/") : 0;
    if (!cut)
        return;
    char dir[600];
    snprintf(dir, sizeof dir, "%.*s/PIONEER/azspool", (int) (cut - dbp), dbp);
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *de;
    FILE *lg = 0;
    while ((de = readdir(d))) {
        size_t k = strlen(de->d_name);
        if (de->d_name[0] == '.' || k < 5 || strcmp(de->d_name + k - 4, ".sql"))
            continue;
        if (!lg) {
            lg = azlog_open("/tmp/azmod-libspool.log", "a");
            if (lg)
                setvbuf(lg, NULL, _IONBF, 0);
        }
        char p[900];
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        if (lg)
            fprintf(lg, "stick %04x (%s): %s\n", media, dbp, de->d_name);
        ls_apply_file((void *) conn, cache, p, lg);
    }
    closedir(d);
    if (lg)
        fclose(lg);
}
/* Polls every stick's PIONEER/azspool for a *.sql (every 250 ms while a copy is in
 * flight, every 2 s while a spool is left over), then arms and wakes
 * the library threads; each takes only its own stick's (ls_service). Disarms when none
 * is left. Message thread; with no copy in flight and nothing left over it returns before
 * touching the disk. */
static void ls_tick(void) {
    static long long next;
    long long now = drag_now_ms();
    if (now < next)
        return;
    /* A cross-stick COPY / PASTE writes its rows as a spool and waits for this
     * service, so it must run while one is in flight, and after a copy that stopped
     * waiting (g_ls_leftover) until its spool is applied. Otherwise it idles. */
    int copying = __atomic_load_n(&g_cp.state, __ATOMIC_ACQUIRE) == CP_COPYING;
    int leftover = __atomic_load_n(&g_ls_leftover, __ATOMIC_ACQUIRE);
    next = now + (copying ? 250 : 2000);
    if (!copying && !leftover) {
        __atomic_store_n(&g_ls_armed, 0, __ATOMIC_RELEASE);
        return;
    }
    int any = 0;
    DIR *u = opendir("/media/usb");
    struct dirent *e;
    while (u && !any && (e = readdir(u))) {
        if (e->d_name[0] == '.')
            continue;
        char p[300];
        snprintf(p, sizeof p, "/media/usb/%s/PIONEER/azspool", e->d_name);
        DIR *d = opendir(p);
        struct dirent *f;
        while (d && !any && (f = readdir(d))) {
            size_t k = strlen(f->d_name);
            any = f->d_name[0] != '.' && k > 4 && !strcmp(f->d_name + k - 4, ".sql");
        }
        if (d)
            closedir(d);
    }
    if (u)
        closedir(u);
    __atomic_store_n(&g_ls_armed, any, __ATOMIC_RELEASE);
    if (any)
        plrenew_kick();
    else if (leftover && !copying)
        /* nothing left on any stick: applied. A copy that times out after this point
         * sets it again (before its CP_DONE), so no leftover is lost. */
        __atomic_store_n(&g_ls_leftover, 0, __ATOMIC_RELEASE);
}
