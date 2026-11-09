/* SPDX-License-Identifier: GPL-3.0-only */
/* pdb_sync.c: keep a stick's export.pdb (rekordbox's "Device Library") consistent with
 * its exportLibrary.db (OneLibrary) after a cross-USB paste, so rekordbox shows no
 * "We have found a library inconsistency" page.
 *
 * The page fires when a OneLibrary track has no export.pdb track row; a
 * playlist-content difference alone does not. So after a paste:
 *   1. every OneLibrary track missing from this pdb but present in the source stick's
 *      pdb gets the source's own rekordbox-made row, its ids remapped (artist / album /
 *      genre / label / key reused by name, else a new row; an artwork row for the pasted
 *      art path), appended on the table's last page (a new page when full);
 *   2. a track this pdb already lists, whose OneLibrary art is a paste's copy (under
 *      /PIONEER/Artwork/azcp/) while its own art file is gone from the stick, is pointed at an
 *      artwork row for that copy (ps_paste_art);
 *   3. playlist_tree (7) and playlist_entries (8) are rewritten to mirror OneLibrary.
 * A track with no source row (added on the deck, or a source without export.pdb) is
 * skipped and logged; rows are never synthesized, so the page stays for it.
 *
 * Safety: export.pdb -> export.pdb.azbak once per insertion; the new file is built in
 * memory, re-parsed (every table chain walks, row counts as expected), then written whole
 * beside export.pdb and renamed over it (ps_replace: a pulled stick keeps the old file or the
 * new one, never a torn mix) and read back. Any failure keeps or puts back the original; the
 * OneLibrary edit stands. One sync at a time across every stick (g_ps_lock: each stick has
 * its own library thread, and the work buffers here are shared). Libc only, no EP147 calls,
 * so a host test compiles it natively.
 *
 * Layout (crate-digger / rekordcrate, checked against rekordbox 7's own pages): page header
 * 0x28; rows heap up from 0x28, row index down from the page end in groups of 16 {u16
 * ofs[16] reversed, u16 present, u16 dirty mask}; num_rows_small u8 @0x18 = slots
 * (deleted ones too); (u3 | u4<<8) @0x19 = present rows x 32; free/used @0x1c/0x1e;
 * u16 @0x20 = dirty-row count, u16 @0x22 = first dirty row (0x1fff/0x1fff = clean).
 * Track/artist/album u16 index_shift @2 = slot x 32.
 *
 * Dirty state (rekordbox 7.2.19's DeviceSQL page manager): TE_PSmgrPageInit leaves a new
 * page clean (0x1fff/0x1fff); every space request first runs TE_PSmgrClearDirtyBits, which
 * walks the dirty masks from group (first>>4) toward the page start, subtracting popcounts
 * until the count is exactly 0 (no other bound), then TE_PSmgrAddDirtyArea marks the
 * inserted row (count 1, first = that row). A count the masks cannot supply walks off the
 * page and crashes rekordbox. So every page written leaves exactly one dirty row, the last
 * one inserted, as rekordbox does after a one-row insert, and ps_walk_ok() refuses any
 * written page whose state the walk would not stop on. */
#include "pioneer_dir.h" /* PIONEER/ or .PIONEER/: where a stick's rekordbox export is */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <pthread.h>
#include <sys/ioctl.h>
#ifdef __linux__
#include <linux/fs.h> /* FIBMAP */
#endif

struct pdbs_pl {
    unsigned id, parent, seq, folder;
    const char *name;
}; /* OneLibrary playlist rows */
struct pdbs_item {
    unsigned pl;
    const char *path;
}; /* playlist_content, in order */
struct pdbs_track {
    const char *path, *art, *anlz;
}; /* every content row (+ image path, + analysisDataFilePath; either may be 0) */

#define PS_HDR 0x28
#define PS_MAXROWS 255   /* rows azmod puts on a page (our writes keep 0x19's slot bits 0) */
#define PS_MAXSLOTS 8192 /* slots a page can carry: 13 bits (rekordbox packs 284) */
enum {
    PT_TRACK = 0,
    PT_GENRE = 1,
    PT_ARTIST = 2,
    PT_ALBUM = 3,
    PT_LABEL = 4,
    PT_KEY = 5,
    PT_TREE = 7,
    PT_ENTRY = 8,
    PT_ART = 13
};

typedef struct {
    unsigned char *b;
    size_t n, cap;
    unsigned pl;
} ps_buf;
static unsigned ps_u16(const unsigned char *p) {
    return p[0] | p[1] << 8;
}
static unsigned ps_u32(const unsigned char *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (unsigned) p[3] << 24;
}
static void ps_w16(unsigned char *p, unsigned v) {
    p[0] = v;
    p[1] = v >> 8;
}
static void ps_w32(unsigned char *p, unsigned v) {
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

static int ps_load(const char *path, ps_buf *o) {
    memset(o, 0, sizeof *o);
    FILE *f = fopen(path, "rbe");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 4096 || n > (64 << 20)) {
        fclose(f);
        return -2;
    }
    o->b = malloc(n);
    o->cap = o->n = n;
    if (!o->b || fread(o->b, 1, n, f) != (size_t) n) {
        fclose(f);
        free(o->b);
        o->b = 0;
        return -3;
    }
    fclose(f);
    o->pl = ps_u32(o->b + 4);
    if (o->pl < 512 || o->pl > 65536 || n % o->pl || ps_u32(o->b + 8) > 64) {
        free(o->b);
        o->b = 0;
        return -4;
    }
    return 0;
}
static unsigned ps_ntab(const ps_buf *d) {
    return ps_u32(d->b + 8);
}
static unsigned char *ps_tab(const ps_buf *d,
                             unsigned typ) { /* {type, empty_candidate, first, last} */
    for (unsigned i = 0; i < ps_ntab(d); i++)
        if (ps_u32(d->b + 0x1c + 16 * i) == typ)
            return d->b + 0x1c + 16 * i;
    return 0;
}
static unsigned char *ps_page(const ps_buf *d, unsigned p) {
    return ((size_t) p + 1) * d->pl <= d->n ? d->b + (size_t) p * d->pl : 0; /* no u32 wrap */
}
/* the table's data pages, chain order; -1 on a broken chain */
static int ps_pages(const ps_buf *d, unsigned typ, unsigned *out, int max) {
    unsigned char *t = ps_tab(d, typ);
    if (!t)
        return -1;
    unsigned p = ps_u32(t + 8), last = ps_u32(t + 12);
    int n = 0;
    for (int guard = 0; guard <= (int) (d->n / d->pl); guard++) {
        unsigned char *pg = ps_page(d, p);
        if (!pg)
            return -1;
        if (ps_u32(pg + 8) == typ && !(pg[0x1b] & 0x40)) {
            if (n >= max)
                return -1;
            out[n++] = p;
        }
        if (p == last)
            return n;
        p = ps_u32(pg + 0xc);
    }
    return -1;
}
/* A page's slot count is 13 bits: 0x18 plus the low 5 bits of 0x19 (rekordcrate
 * PackedRowCounts; the present count is the 11 bits above, 0x19 >> 5 | 0x1a << 3).
 * rekordbox packs up to 284 slots on a playlist_entries page, so reading 0x18 alone
 * undercounts and an append would overwrite a live slot. azmod writes at most PS_MAXROWS (255)
 * per page, so 0x19's slot bits stay 0 in our own pages. */
static unsigned ps_slots(const unsigned char *pg) {
    return pg[0x18] | (pg[0x19] & 0x1f) << 8;
}
/* row offsets (absolute in page) of the present rows; -1 = an index entry points outside */
static int ps_rows(const ps_buf *d, const unsigned char *pg, unsigned *out) {
    int n = ps_slots(pg), k = 0;
    if ((unsigned) (n + 15) / 16 * 0x24 > d->pl - PS_HDR)
        return -1; /* more index groups than the page holds below its header */
    for (int i = 0; i < n; i++) {
        unsigned base = d->pl - (i / 16) * 0x24;
        if (ps_u16(pg + base - 4) >> (i % 16) & 1) {
            unsigned o = ps_u16(pg + base - 6 - 2 * (i % 16)) + PS_HDR;
            if (o >= d->pl)
                return -1;
            out[k++] = o;
        }
    }
    return k;
}
static unsigned ps_slen(const unsigned char *s) {
    return s[0] & 1 ? s[0] >> 1 : ps_u16(s + 1);
}
/* DeviceSQL string -> UTF-8. avail: the bytes from s to the end of its page (>= 1); a length
 * field that claims more is cut to the page. */
static void ps_str(const unsigned char *s, size_t avail, char *out, size_t cap) {
    size_t o = 0;
    if (s[0] & 1) {
        unsigned n = s[0] >> 1 ? (s[0] >> 1) - 1 : 0;
        if (n > avail - 1)
            n = avail - 1;
        if (n > cap - 1)
            n = cap - 1;
        memcpy(out, s + 1, n);
        out[n] = 0;
        return;
    }
    if (avail < 4) {
        out[0] = 0;
        return;
    }
    unsigned n = ps_u16(s + 1);
    n = n >= 4 ? n - 4 : 0;
    if (n > avail - 4)
        n = avail - 4;
    if (s[0] != 0x90) {
        if (n > cap - 1)
            n = cap - 1;
        memcpy(out, s + 4, n);
        out[n] = 0;
        return;
    }
    for (unsigned i = 0; i + 1 < n && o + 4 < cap; i += 2) {
        unsigned c = ps_u16(s + 4 + i);
        if (c >= 0xd800 && c < 0xdc00 && i + 3 < n) {
            unsigned c2 = ps_u16(s + 6 + i);
            if (c2 >= 0xdc00 && c2 < 0xe000) {
                c = 0x10000 + ((c - 0xd800) << 10) + (c2 - 0xdc00);
                i += 2;
            }
        }
        if (c < 0x80)
            out[o++] = c;
        else if (c < 0x800) {
            out[o++] = 0xc0 | c >> 6;
            out[o++] = 0x80 | (c & 0x3f);
        } else if (c < 0x10000) {
            out[o++] = 0xe0 | c >> 12;
            out[o++] = 0x80 | (c >> 6 & 0x3f);
            out[o++] = 0x80 | (c & 0x3f);
        } else {
            out[o++] = 0xf0 | c >> 18;
            out[o++] = 0x80 | (c >> 12 & 0x3f);
            out[o++] = 0x80 | (c >> 6 & 0x3f);
            out[o++] = 0x80 | (c & 0x3f);
        }
    }
    out[o] = 0;
}
/* One UTF-8 sequence at s (avail bytes left): its code point and length (*k), or -1 when the
 * bytes are not one well-formed sequence (RFC 3629: shortest form, no surrogate, at most
 * U+10FFFF). *k is 1 for an ill-formed byte. */
static long ps_u8(const unsigned char *s, size_t avail, unsigned *k) {
    unsigned c = s[0], n = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2
                                      : (c & 0xf0) == 0xe0   ? 3
                                      : (c & 0xf8) == 0xf0   ? 4
                                                             : 0;
    static const unsigned long lo[5] = {0, 0, 0x80, 0x800, 0x10000};
    *k = 1;
    if (!n || n > avail)
        return -1;
    unsigned long v = n == 1 ? c : c & (0x7f >> n);
    for (unsigned i = 1; i < n; i++) {
        if ((s[i] & 0xc0) != 0x80)
            return -1;
        v = v << 6 | (s[i] & 0x3f);
    }
    if (v < lo[n] || v > 0x10ffff || (v >= 0xd800 && v < 0xe000))
        return -1;
    *k = n;
    return (long) v;
}
#define PS_ENC_CUT 1   /* the string did not fit: cut after its last whole character */
#define PS_ENC_FIXED 2 /* ill-formed UTF-8: each bad byte written as U+FFFD */
/* UTF-8 -> DeviceSQL: short ASCII under 126 bytes, else UTF-16LE (0x90), as the reference
 * writes. Never emits half a character: a string longer than cap is cut at a character
 * boundary, an incomplete sequence at its very end (a name cut byte-wise) is dropped, and any
 * other ill-formed byte becomes U+FFFD. flags (may be 0): PS_ENC_*. 0 = cap below 4. */
static size_t ps_enc(const char *s, unsigned char *out, size_t cap, unsigned *flags) {
    size_t L = strlen(s);
    unsigned fl = 0;
    int ascii = 1;
    for (size_t i = 0; i < L; i++)
        if ((unsigned char) s[i] >= 0x80)
            ascii = 0;
    if (ascii && L < 126 && L + 1 <= cap) {
        out[0] = (L + 1) << 1 | 1;
        memcpy(out + 1, s, L);
        if (flags)
            *flags = 0;
        return L + 1;
    }
    if (cap < 4)
        return 0;
    size_t o = 4;
    for (size_t i = 0; i < L;) {
        const unsigned char *u = (const unsigned char *) s + i;
        unsigned k;
        long c = ps_u8(u, L - i, &k);
        if (c < 0) {
            unsigned n = u[0] >= 0xf0 && u[0] < 0xf8 ? 4 : u[0] >= 0xe0 ? 3
                                                       : u[0] >= 0xc0   ? 2
                                                                        : 0;
            size_t j = 1;
            while (j < L - i && j < n && (u[j] & 0xc0) == 0x80)
                j++;
            if (n && i + j == L && j < n) {
                fl |= PS_ENC_CUT; /* the start of a character, cut off: dropped */
                break;
            }
            c = 0xfffd;
            fl |= PS_ENC_FIXED;
        }
        unsigned need = c >= 0x10000 ? 4 : 2;
        if (o + need > cap) {
            fl |= PS_ENC_CUT;
            break;
        }
        if (c >= 0x10000) {
            c -= 0x10000;
            ps_w16(out + o, 0xd800 | (unsigned) (c >> 10));
            ps_w16(out + o + 2, 0xdc00 | (unsigned) (c & 0x3ff));
        } else
            ps_w16(out + o, (unsigned) c);
        o += need;
        i += k;
    }
    out[0] = 0x90;
    ps_w16(out + 1, (unsigned) o);
    out[3] = 0;
    if (flags)
        *flags = fl;
    return o;
}

/* ---- path -> track id (open addressing; paths are long, tracks thousands) ---- */
struct ps_map {
    char **k;
    unsigned *v;
    unsigned cap, n;
};
static unsigned ps_hash(const char *s) {
    unsigned h = 2166136261u;
    while (*s)
        h = (h ^ (unsigned char) *s++) * 16777619u;
    return h;
}
static void ps_map_put(struct ps_map *m, const char *k, unsigned v) {
    if ((m->n + 1) * 2 > m->cap) {
        struct ps_map o = *m;
        m->cap = m->cap ? m->cap * 2 : 1024;
        m->n = 0;
        m->k = calloc(m->cap, sizeof *m->k);
        m->v = calloc(m->cap, sizeof *m->v);
        for (unsigned i = 0; i < o.cap; i++) {
            if (!o.k[i])
                continue;
            unsigned h = ps_hash(o.k[i]) & (m->cap - 1);
            while (m->k[h])
                h = (h + 1) & (m->cap - 1);
            m->k[h] = o.k[i];
            m->v[h] = o.v[i];
            m->n++;
        }
        free(o.k);
        free(o.v);
    }
    unsigned h = ps_hash(k) & (m->cap - 1);
    while (m->k[h]) {
        if (!strcmp(m->k[h], k)) {
            m->v[h] = v;
            return;
        }
        h = (h + 1) & (m->cap - 1);
    }
    m->k[h] = strdup(k);
    m->v[h] = v;
    m->n++;
}
static int ps_map_get(const struct ps_map *m, const char *k, unsigned *v) {
    if (!m->cap)
        return 0;
    unsigned h = ps_hash(k) & (m->cap - 1);
    while (m->k[h]) {
        if (!strcmp(m->k[h], k)) {
            if (v)
                *v = m->v[h];
            return 1;
        }
        h = (h + 1) & (m->cap - 1);
    }
    return 0;
}
static void ps_map_free(struct ps_map *m) {
    for (unsigned i = 0; i < m->cap; i++)
        free(m->k[i]);
    free(m->k);
    free(m->v);
    memset(m, 0, sizeof *m);
}

#define PS_MAXPAGES 4096
/* every track: path -> id (v), and optionally path -> page*65536+offset (loc) */
static int ps_tracks(const ps_buf *d, struct ps_map *ids, struct ps_map *loc) {
    static unsigned pg[PS_MAXPAGES], ro[PS_MAXSLOTS];
    int np = ps_pages(d, PT_TRACK, pg, PS_MAXPAGES);
    if (np < 0)
        return -1;
    char path[1024];
    for (int i = 0; i < np; i++) {
        unsigned char *p = ps_page(d, pg[i]);
        int nr = ps_rows(d, p, ro);
        if (nr < 0)
            return -1;
        for (int r = 0; r < nr; r++) {
            unsigned o = ro[r];
            if (o + 0x88 > d->pl)
                return -1;
            unsigned so = ps_u16(p + o + 0x5e + 40);
            if (o + so >= d->pl)
                return -1;
            ps_str(p + o + so, d->pl - (o + so), path, sizeof path);
            if (ids)
                ps_map_put(ids, path, ps_u32(p + o + 0x48));
            if (loc)
                ps_map_put(loc, path, pg[i] << 16 | o);
        }
    }
    return 0;
}
static int ps_count(const ps_buf *d, unsigned typ) {
    static unsigned pg[PS_MAXPAGES], ro[PS_MAXSLOTS];
    int np = ps_pages(d, typ, pg, PS_MAXPAGES), n = 0;
    if (np < 0)
        return -1;
    for (int i = 0; i < np; i++) {
        int r = ps_rows(d, ps_page(d, pg[i]), ro);
        if (r < 0)
            return -1;
        n += r;
    }
    return n;
}
/* named row: its id and name ("name\x1f<artist id>" for albums) */
struct ps_named {
    unsigned id;
    char name[300];
};
static int ps_names(const ps_buf *d, unsigned typ, struct ps_named **out) {
    static unsigned pg[PS_MAXPAGES], ro[PS_MAXSLOTS];
    int np = ps_pages(d, typ, pg, PS_MAXPAGES), n = 0, cap = 0;
    *out = 0;
    if (np < 0)
        return -1;
    for (int i = 0; i < np; i++) {
        unsigned char *p = ps_page(d, pg[i]);
        int nr = ps_rows(d, p, ro);
        if (nr < 0)
            return -1;
        for (int r = 0; r < nr; r++) {
            const unsigned char *x = p + ro[r];
            unsigned id, so;
            char nm[256];
            /* the fixed fields read below: genre/label/art 4, key 8, artist 12, album 0x18 */
            unsigned fixed = typ == PT_KEY ? 8 : typ == PT_ARTIST ? 12
                                             : typ == PT_ALBUM    ? 0x18
                                                                  : 4;
            if (ro[r] + fixed > d->pl)
                return -1;
            if (typ == PT_GENRE || typ == PT_LABEL || typ == PT_ART) {
                id = ps_u32(x);
                so = 4;
            } else if (typ == PT_KEY) {
                id = ps_u32(x);
                so = 8;
            } else if (typ == PT_ARTIST) {
                id = ps_u32(x + 4);
                so = ps_u16(x) & 4 ? ps_u16(x + 10) : x[9];
            } else {
                id = ps_u32(x + 12);
                so = ps_u16(x) & 4 ? ps_u16(x + 0x16) : x[0x15];
            }
            if (ro[r] + so >= d->pl)
                return -1;
            ps_str(x + so, d->pl - (ro[r] + so), nm, sizeof nm);
            if (n == cap) {
                cap = cap ? cap * 2 : 64;
                *out = realloc(*out, cap * sizeof **out);
            }
            (*out)[n].id = id;
            if (typ == PT_ALBUM)
                snprintf((*out)[n].name, sizeof(*out)[n].name, "%s\x1f%u", nm, ps_u32(x + 8));
            else
                snprintf((*out)[n].name, sizeof(*out)[n].name, "%s", nm);
            n++;
        }
    }
    return n;
}

/* ---- writing ---- */
static int ps_grow(ps_buf *d, size_t n) {
    if (n <= d->n)
        return 0;
    if (n > d->cap) {
        size_t c = d->cap * 2 > n ? d->cap * 2 : n;
        unsigned char *b = realloc(d->b, c);
        if (!b)
            return -1;
        d->b = b;
        d->cap = c;
    }
    memset(d->b + d->n, 0, n - d->n);
    d->n = n;
    return 0;
}
/* dirty state. @0x20/@0x22 keep bits 14/15 and clear bit 13, as rekordbox writes them */
static void ps_set_dirty(unsigned char *pg, unsigned cnt, unsigned first) {
    ps_w16(pg + 0x20, (ps_u16(pg + 0x20) & 0xc000) | (cnt & 0x1fff));
    ps_w16(pg + 0x22, (ps_u16(pg + 0x22) & 0xc000) | (first & 0x1fff));
}
static void ps_clear_dirty(const ps_buf *d, unsigned char *pg, unsigned slots) {
    for (unsigned g = 0; g < (slots + 15) / 16; g++)
        ps_w16(pg + d->pl - g * 0x24 - 2, 0);
    ps_set_dirty(pg, 0x1fff, 0x1fff);
}
static unsigned ps_popc(unsigned x) {
    unsigned c = 0;
    while (x) {
        c += x & 1;
        x >>= 1;
    }
    return c;
}
/* rekordbox's TE_PSmgrClearDirtyBits walk, read-only: 1 if it would stop inside the page's own
 * row groups (or not run at all), 0 if it would run past them: into the row heap, the header,
 * then memory below the page (the rekordbox 7.2.19 crash). */
static int ps_walk_ok(const ps_buf *d, const unsigned char *pg) {
    unsigned cnt = ps_u16(pg + 0x20) & 0x3fff, g = (ps_u16(pg + 0x22) & 0x3fff) >> 4;
    unsigned ng = (ps_slots(pg) + 15) / 16;
    if (cnt == 0x1fff)
        return 1;
    while (cnt) {
        if (g >= ng)
            return 0;
        cnt = (cnt - ps_popc(ps_u16(pg + d->pl - g * 0x24 - 2))) & 0x3fff;
        g++;
    }
    return 1;
}
/* the table's empty_candidate becomes its new last page (the old last page already chains
 * to it); the header's next_unused_page becomes the new empty_candidate */
static int ps_new_page(ps_buf *d, unsigned typ) {
    unsigned char *t = ps_tab(d, typ);
    if (!t)
        return -1;
    unsigned ec = ps_u32(t + 4), first = ps_u32(t + 8), last = ps_u32(t + 12),
             nu = ps_u32(d->b + 0x0c);
    unsigned char *lp = ps_page(d, last);
    if (!lp || ps_u32(lp + 0xc) != ec || ec >= 1u << 20)
        return -1;
    unsigned seq = ps_u32(lp + 0x10), flags = lp[0x1b];
    if (ps_grow(d, (size_t) (ec + 1) * d->pl))
        return -1;
    t = ps_tab(d, typ); /* d->b may have moved */
    unsigned char *pg = ps_page(d, ec);
    memset(pg, 0, d->pl);
    ps_w32(pg + 4, ec);
    ps_w32(pg + 8, typ);
    ps_w32(pg + 0xc, nu);
    ps_w32(pg + 0x10, seq);
    pg[0x1b] = flags & ~0x10u;
    ps_w16(pg + 0x1c, d->pl - PS_HDR);
    ps_w16(pg + 0x20, 0x1fff);
    ps_w16(pg + 0x22, 0x1fff); /* clean, as TE_PSmgrPageInit leaves it */
    ps_w32(t + 4, nu);
    ps_w32(t + 12, ec);
    (void) first;
    ps_w32(d->b + 0x0c, nu + 1);
    return (int) ec;
}
/* append one row on the table's last page (a new page when full); index_shift = slot*32 */
static int ps_append(ps_buf *d, unsigned typ, const unsigned char *row, size_t len, int shift) {
    static unsigned pgs[PS_MAXPAGES];
    int np = ps_pages(d, typ, pgs, PS_MAXPAGES);
    if (np <= 0)
        return -1;
    unsigned p = pgs[np - 1];
    unsigned char *pg = ps_page(d, p);
    unsigned n = ps_slots(pg), used = ps_u16(pg + 0x1e), al = (len + 3) & ~3u;
    if (ps_u16(pg + 0x1c) < al + 2 + (n % 16 == 0 ? 4 : 0) || n >= PS_MAXROWS) {
        int q = ps_new_page(d, typ);
        if (q < 0)
            return -1;
        p = q;
        pg = ps_page(d, p);
        n = 0;
        used = 0;
    }
    if (PS_HDR + used + al + 2 * (n + 1) + 4 * ((n + 16) / 16) > d->pl)
        return -1;
    memcpy(pg + PS_HDR + used, row, len);
    memset(pg + PS_HDR + used + len, 0, al - len);
    if (shift)
        ps_w16(pg + PS_HDR + used + 2, n * 32);
    unsigned g = n / 16, k = n % 16, base = d->pl - g * 0x24;
    ps_clear_dirty(d, pg, n + 1); /* TE_PSmgrClearDirtyBits, every group */
    ps_w16(pg + base - 6 - 2 * k, used);
    ps_w16(pg + base - 4, ps_u16(pg + base - 4) | 1u << k);
    ps_w16(pg + base - 2, 1u << k);
    n++;
    used += al;
    unsigned present = 0;
    for (unsigned gg = 0; gg < (n + 15) / 16; gg++) {
        unsigned f = ps_u16(pg + d->pl - gg * 0x24 - 4);
        while (f) {
            present += f & 1;
            f >>= 1;
        }
    }
    unsigned idx = 2 * n + 4 * ((n + 15) / 16);
    pg[0x18] = n;
    ps_w16(pg + 0x19, present * 32);
    ps_w16(pg + 0x1c, d->pl - PS_HDR - used - idx);
    ps_w16(pg + 0x1e, used);
    ps_set_dirty(pg, 1, n - 1); /* TE_PSmgrAddDirtyArea: the new row */
    return (int) p;
}
/* rewrite one page with rows (the reference's pack_page) */
static void ps_pack(ps_buf *d, unsigned char *pg, unsigned char **rows, size_t *lens, int n) {
    memset(pg + PS_HDR, 0, d->pl - PS_HDR);
    unsigned used = 0, ofs[PS_MAXROWS];
    for (int i = 0; i < n; i++) {
        ofs[i] = used;
        memcpy(pg + PS_HDR + used, rows[i], lens[i]);
        used += (lens[i] + 3) & ~3u;
    }
    for (int g = 0; g < (n + 15) / 16; g++) {
        unsigned base = d->pl - g * 0x24;
        int m = n - 16 * g < 16 ? n - 16 * g : 16;
        for (int k = 0; k < m; k++)
            ps_w16(pg + base - 6 - 2 * k, ofs[16 * g + k]);
        ps_w16(pg + base - 4, (1u << m) - 1);
        ps_w16(pg + base - 2, g == (n - 1) / 16 ? 1u << (m - 1) : 0);
    }
    pg[0x18] = n;
    ps_w16(pg + 0x19, n * 32);
    ps_w16(pg + 0x1c, d->pl - PS_HDR - used - (2 * n + 4 * ((n + 15) / 16)));
    ps_w16(pg + 0x1e, used);
    /* The dirty state of a page rekordbox filled row by row: only the last row dirty. The
     * count must be rewritten too: an old count (4, 24, 284...) against a one-bit mask crashes
     * rekordbox 7.2.19. A packed page has no holes, so flag 0x10 is clear, as ClearDirtyBits
     * leaves a page whose slots == rows. */
    if (n)
        ps_set_dirty(pg, 1, n - 1);
    else
        ps_set_dirty(pg, 0x1fff, 0x1fff);
    pg[0x1b] &= ~0x10u;
}
/* A surplus page when a table shrank below its page count: one deleted slot and no present row,
 * flag 0x10 (0x34), clean dirty state: how rekordbox leaves a page whose rows it deleted (its own
 * export: a playlist_tree page with 3 slots / 0 present, listed in the index page's free list).
 * Not a 0-slot page: a 0-slot data page sends rekordbox's DeviceSQL open into a runaway allocation
 * (TE_IndexCreate -> TE_SmgrGetNext). The tombstone page passes the open-time page checks
 * rekordbox 7.2.19 runs (ps_open_ok below). */
static void ps_tombstone(ps_buf *d, unsigned char *pg, unsigned rowlen) {
    unsigned used = (rowlen + 3) & ~3u;
    memset(pg + PS_HDR, 0, d->pl - PS_HDR); /* slot 0 = offset 0, present 0, dirty 0 */
    pg[0x18] = 1;
    ps_w16(pg + 0x19, 0);
    ps_w16(pg + 0x1c, d->pl - PS_HDR - used - 6);
    ps_w16(pg + 0x1e, used);
    ps_set_dirty(pg, 0x1fff, 0x1fff);
    pg[0x1b] |= 0x10;
}
/* The table's index page keeps a free list: u32 entries (page << 3) from 0x3c, count at 0x26 and
 * 0x38, naming the data pages that have deleted slots; rekordbox's DeviceSQL reuses space from it
 * (TE_PSmgrGetReusedSpace). On every rekordbox-made file it lists exactly the pages with holes. A
 * rewritten table changes which pages have holes, so the list is rebuilt to match; left stale,
 * rekordbox's next delete lists a page twice (seen after a rekordbox Delete Playlist). */
static int ps_free_list(ps_buf *d, unsigned typ) {
    unsigned char *t = ps_tab(d, typ);
    if (!t)
        return -1;
    unsigned char *ip = ps_page(d, ps_u32(t + 8));
    if (!ip || !(ip[0x1b] & 0x40))
        return -1;
    static unsigned pgs[PS_MAXPAGES];
    int np = ps_pages(d, typ, pgs, PS_MAXPAGES), nh = 0;
    if (np < 0)
        return -1;
    unsigned cap = ps_u16(ip + 0x24), old = ps_u16(ip + 0x38);
    if (cap > (d->pl - 0x3c) / 4)
        return -1;
    for (int i = 0; i < np; i++) {
        unsigned char *x = ps_page(d, pgs[i]);
        unsigned present = (x[0x19] >> 5) | x[0x1a] << 3;
        if (ps_slots(x) != present) {
            if ((unsigned) nh >= cap)
                return -1;
            ps_w32(ip + 0x3c + 4 * nh++, pgs[i] << 3);
        }
    }
    for (unsigned k = nh; k < old && k < cap; k++)
        ps_w32(ip + 0x3c + 4 * k, 0x1ffffff8);
    ps_w16(ip + 0x26, nh);
    ps_w16(ip + 0x38, nh);
    ps_w16(ip + 0x20, 0x1fff);
    ps_w16(ip + 0x22, 0x1fff);
    return 0;
}
/* Rewrite a whole table: fill its pages in chain order, adding pages when needed.
 * Never leave a 0-slot data page: one in playlist_tree or playlist_entries sends rekordbox 7
 * into a runaway allocation (78 GB, "out of application memory") as soon as it opens the stick.
 * With at least as many rows as pages, every page keeps >= 1 row (the dense layout). With fewer
 * rows than pages (playlists deleted, a big playlist cleared) the rows are packed from the first
 * page and each surplus page becomes a tombstone (ps_tombstone). Refusing that case instead would freeze the Device Library at its
 * last mirrored state for good. tomb = the tombstone's dummy row length. Then the index page's
 * free list is rebuilt (ps_free_list). */
static int ps_rewrite(ps_buf *d, unsigned typ, unsigned char **rows, size_t *lens, int n,
                      unsigned tomb) {
    static unsigned pgs[PS_MAXPAGES];
    int np = ps_pages(d, typ, pgs, PS_MAXPAGES);
    if (np <= 0)
        return -1;
    int spread = n >= np, np0 = np;
    int i = 0, pi = 0;
    while (i < n) {
        if (pi >= np) {
            int q = ps_new_page(d, typ);
            if (q < 0)
                return -1;
            pgs[np++] = q;
        }
        int k = 0;
        unsigned used = 0;
        while (i + k < n && k < PS_MAXROWS &&
               (!spread || n - (i + k + 1) >= np0 - pi - 1)) { /* >=1 row per remaining page */
            unsigned al = (lens[i + k] + 3) & ~3u, m = k + 1;
            if (PS_HDR + used + al + 2 * m + 4 * ((m + 15) / 16) > d->pl)
                break;
            used += al;
            k++;
        }
        if (k == 0)
            return -1; /* a row larger than a page */
        ps_pack(d, ps_page(d, pgs[pi]), rows + i, lens + i, k);
        i += k;
        pi++;
    }
    for (; pi < np; pi++)
        ps_tombstone(d, ps_page(d, pgs[pi]), tomb);
    return ps_free_list(d, typ);
}

/* re-parse the whole file: every chain walks, every row index entry is in range */
static int ps_verify(const ps_buf *d, int want_tree, int want_ent, int want_trk, FILE *lg) {
    static unsigned pg[PS_MAXPAGES], ro[PS_MAXSLOTS];
    /* every page belongs to at most one chain, and no table's empty_candidate is in use
     * (a new page is taken from it: a shared one would overwrite another table) */
    unsigned np_all = d->n / d->pl;
    unsigned char *own = calloc(np_all + 1, 1);
    if (!own)
        return -1;
    for (unsigned i = 0; i < ps_ntab(d); i++) {
        const unsigned char *t = d->b + 0x1c + 16 * i;
        unsigned p = ps_u32(t + 8), last = ps_u32(t + 12);
        for (unsigned guard = 0; guard <= np_all; guard++) {
            if (p >= np_all || own[p]) {
                if (lg)
                    fprintf(lg, "PDB verify: page %u shared or out of range\n", p);
                free(own);
                return -1;
            }
            own[p] = 1;
            if (p == last)
                break;
            p = ps_u32(d->b + (size_t) p * d->pl + 0xc);
        }
    }
    for (unsigned i = 0; i < ps_ntab(d); i++) {
        unsigned ec = ps_u32(d->b + 0x1c + 16 * i + 4);
        if (ec < np_all && own[ec]) {
            if (lg)
                fprintf(lg, "PDB verify: empty_candidate %u is in use\n", ec);
            free(own);
            return -1;
        }
    }
    free(own);
    for (unsigned i = 0; i < ps_ntab(d); i++) {
        unsigned typ = ps_u32(d->b + 0x1c + 16 * i), tot = 0;
        int np = ps_pages(d, typ, pg, PS_MAXPAGES);
        if (np < 0) {
            if (lg)
                fprintf(lg, "PDB verify: table %u chain broken\n", typ);
            return -1;
        }
        for (int p = 0; p < np; p++) {
            const unsigned char *x = ps_page(d, pg[p]);
            if (ps_u32(x + 4) != pg[p] || ps_u32(x + 8) != typ) {
                if (lg)
                    fprintf(lg, "PDB verify: page %u header\n", pg[p]);
                return -1;
            }
            int nr = ps_rows(d, x, ro);
            if (nr < 0 || PS_HDR + ps_u16(x + 0x1e) > d->pl) {
                if (lg)
                    fprintf(lg, "PDB verify: page %u rows\n", pg[p]);
                return -1;
            }
            if ((typ == PT_TREE || typ == PT_ENTRY) && ps_slots(x) == 0) {
                if (lg)
                    fprintf(lg,
                            "PDB verify: table %u page %u is a 0-slot data page (rekordbox "
                            "runaway)\n",
                            typ, pg[p]);
                return -1;
            }
            tot += nr;
        }
        if ((typ == PT_TREE && (int) tot != want_tree) ||
            (typ == PT_ENTRY && (int) tot != want_ent) ||
            (typ == PT_TRACK && (int) tot != want_trk)) {
            if (lg)
                fprintf(lg, "PDB verify: table %u has %u rows, expected %d/%d/%d\n", typ, tot,
                        want_tree, want_ent, want_trk);
            return -1;
        }
    }
    return 0;
}
/* The page checks rekordbox 7.2.19's DeviceSQL runs when it opens a Device Library
 * (TE_DPgm_Open -> TE_Dpgm_CheckPageData / CheckPageHeader / CheckPsmgrHeader, read from the
 * symbolled macOS binary). Any failure makes rekordbox say "Device library is corrupted." and
 * offer to delete it, so the file is never written when one fails. 0 = all pass. */
static int ps_open_ok(const ps_buf *d, FILE *lg) {
    unsigned ntab = ps_ntab(d), nu = ps_u32(d->b + 0x0c), hseq = ps_u32(d->b + 0x14),
             h18 = ps_u32(d->b + 0x18), np = d->n / d->pl;
    unsigned lim = d->pl <= 0x1000 ? 0x1fe : d->pl <= 0x2000 ? 0x3fe
                                                             : 0x7fe;
    for (unsigned t = 0; t < ntab; t++) {
        const unsigned char *e = d->b + 0x1c + 16 * t;
        unsigned typ = ps_u32(e), ec = ps_u32(e + 4), p = ps_u32(e + 8), last = ps_u32(e + 12);
        for (unsigned guard = 0; guard <= np; guard++) {
            const unsigned char *x = ps_page(d, p);
            const char *why = 0;
            if (!x)
                why = "chain leaves the file";
            else {
                unsigned idx = ps_u32(x + 4), nx = ps_u32(x + 0xc), w = ps_u32(x + 0x18),
                         dw = ps_u32(x + 0x20);
                if (idx != p && idx)
                    why = "page index field";
                else if (idx >= nu)
                    why = "page index >= next_unused";
                else if (ps_u32(x + 8) > ntab)
                    why = "page type > table count";
                else if (nx >= nu && nx != 0x3ffffff)
                    why = "next page >= next_unused";
                else if (ps_u32(x + 0x14) > h18)
                    why = "page @0x14 > header @0x18";
                else if ((h18 == 0 || h18 == ps_u32(x + 0x14)) && ps_u32(x + 0x10) > hseq)
                    why = "page sequence > header sequence";
                else if (nx > ec && nx != 0x3ffffff)
                    why = "next page > empty_candidate";
                else if ((w >> 29) != 0 && (w >> 29) != 2 &&
                         ((w & 0x1fff) > lim || (w >> 13 & 0x1fff) > lim ||
                          (dw & 0x3fff) > 0x1fff || (dw >> 16 & 0x3fff) > 0x1fff))
                    why = "row counts / dirty fields out of range";
                if (!why && (nx == 0 || nx == 0x3ffffff || p == last || nx >= nu))
                    break;
                p = nx;
            }
            if (why) {
                if (lg)
                    fprintf(lg, "PDB verify: table %u page %u: %s (rekordbox: corrupted)\n", typ, p,
                            why);
                return -1;
            }
        }
    }
    return 0;
}
static int ps_pwrite_all(int fd, const unsigned char *b, size_t n, off_t at) {
    size_t o = 0;
    while (o < n) {
        ssize_t w = pwrite(fd, b + o, n - o, at + o);
        if (w <= 0)
            return -1;
        o += w;
    }
    return 0;
}
static int ps_write_file(const char *path, const unsigned char *b,
                         size_t n) { /* whole file (the backup) */
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    struct stat st;
    int rc = ps_pwrite_all(fd, b, n, 0);
    if (!rc && fstat(fd, &st) == 0 && (size_t) st.st_size > n)
        rc = ftruncate(fd, n); /* only if it shrank */
    if (!rc)
        rc = fsync(fd);
    close(fd);
    return rc;
}
/* export.pdb replaced whole, never rewritten in place: a stick pulled (or a power loss) during
 * an in-place write leaves pages of two versions under the real name, the torn file a player
 * then freezes on. The complete new file is written to export.pdb.aznew beside it and fsynced,
 * then renamed over export.pdb, so the name holds the old file or the new one, never a mix.
 *   FAT (vfat, the deck's kernel 4.4): a rename over an existing file reuses the target's
 *   directory slot (vfat_rename: new_i_pos = the target's i_pos), so there is no moment
 *   without an export.pdb. The ordering below keeps every intermediate state on the media a
 *   whole file: (1) the old file is held open, so its clusters stay allocated; (2) the new file
 *   is written and fsynced (data, FAT chain, its own entry); (3) rename; (4) fsync of the
 *   directory: the temporary entry is erased while the slot still names the old clusters;
 *   (5) fsync of the new file: the slot now names the new clusters (fat_write_inode);
 *   (6) the old file is closed (its clusters freed) and the new one fsynced again. A pull
 *   between steps leaves the old or the new file whole, at worst plus unreferenced clusters
 *   (lost space a disk check reclaims). The new file is not closed before (6): the mount's
 *   "flush" option writes a file's inode back when a write handle is closed.
 *   exFAT (fuse-exfat): its rename erases the target entry, then writes the new one, then
 *   erases the temporary one; a pull inside that call can leave only export.pdb.aznew, which
 *   ps_recover() puts back under the real name at the next sync.
 * A stale export.pdb.aznew byte-identical to export.pdb may share its clusters (a FAT rename
 * cut between its directory writes): freeing it would free export.pdb's, so it is left alone
 * unless the media proves the two are separate (FIBMAP). 0 = replaced and read back; -2 =
 * replaced but the read-back differs; other < 0 = not replaced (export.pdb as it was). */
static int ps_same_bytes(const char *a, const char *b) {
    int fa = open(a, O_RDONLY | O_CLOEXEC), fb = fa >= 0 ? open(b, O_RDONLY | O_CLOEXEC) : -1;
    int same = fa >= 0 && fb >= 0;
    struct stat sa, sb;
    if (same && (fstat(fa, &sa) || fstat(fb, &sb) || sa.st_size != sb.st_size))
        same = 0;
    unsigned char x[4096], y[4096];
    for (off_t at = 0; same && at < sa.st_size; at += sizeof x) {
        ssize_t n = pread(fa, x, sizeof x, at);
        same = n > 0 && pread(fb, y, (size_t) n, at) == n && !memcmp(x, y, (size_t) n);
    }
#ifdef FIBMAP
    int blk_a = 0, blk_b = 0; /* logical block 0 of each: two files never share it */
    if (same && !ioctl(fa, FIBMAP, &blk_a) && !ioctl(fb, FIBMAP, &blk_b) && blk_a && blk_b &&
        blk_a != blk_b)
        same = 0;
#endif
    if (fa >= 0)
        close(fa);
    if (fb >= 0)
        close(fb);
    return same;
}
static int ps_dir_of(const char *path, char *dir, size_t n) {
    const char *sl = strrchr(path, '/');
    if (!sl || (size_t) (sl - path) >= n)
        return -1;
    snprintf(dir, n, "%.*s", sl == path ? 1 : (int) (sl - path), path);
    return 0;
}
static int ps_replace(const char *path, const unsigned char *b, size_t n) {
    char tmp[640], dir[600];
    struct stat st;
    if ((size_t) snprintf(tmp, sizeof tmp, "%s.aznew", path) >= sizeof tmp ||
        ps_dir_of(path, dir, sizeof dir))
        return -1;
    if (lstat(tmp, &st) == 0 && (ps_same_bytes(tmp, path) || unlink(tmp)))
        return -5; /* see above: never free what export.pdb may still use */
    int old = open(path, O_RDONLY | O_CLOEXEC); /* (1) */
    int fd = open(tmp, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644), dfd = -1, rc = -1;
    if (fd < 0)
        goto fail;
    if (ps_pwrite_all(fd, b, n, 0) || fsync(fd)) /* (2) */
        goto fail;
    dfd = open(dir, O_RDONLY | O_CLOEXEC);
    if (rename(tmp, path)) /* (3) */
        goto fail;
    rc = (dfd >= 0 ? fsync(dfd) : 0) || fsync(fd) ? -3 : 0; /* (4) (5) */
    if (old >= 0)
        close(old); /* (6) */
    old = -1;
    if (fsync(fd))
        rc = -3;
    if (!rc) { /* read back what is now under the name */
        unsigned char *chk = malloc(n ? n : 1);
        rc = !chk || pread(fd, chk, n, 0) != (ssize_t) n || memcmp(chk, b, n) ? -2 : 0;
        free(chk);
    } else
        rc = -2; /* renamed: the name holds the new file, its durability unconfirmed */
    close(fd);
    if (dfd >= 0)
        close(dfd);
    return rc;
fail:
    if (fd >= 0) {
        close(fd);
        unlink(tmp); /* never renamed: shares nothing */
    }
    if (old >= 0)
        close(old);
    if (dfd >= 0)
        close(dfd);
    return rc;
}
/* At the start of a sync: an export.pdb missing while export.pdb.aznew is there and a whole
 * Device Library (it parses, every chain walks) = a replace cut inside an exFAT rename; the
 * new file is put back under the real name. 1 = recovered, 0 = nothing to do, -1 = failed. */
static int ps_open_ok(const ps_buf *d, FILE *lg);
static int ps_recover(const char *path, FILE *lg) {
    char tmp[640];
    struct stat st;
    ps_buf t;
    if ((size_t) snprintf(tmp, sizeof tmp, "%s.aznew", path) >= sizeof tmp ||
        lstat(path, &st) == 0 || errno != ENOENT || lstat(tmp, &st) != 0)
        return 0;
    if (ps_load(tmp, &t))
        return -1;
    int ok = !ps_open_ok(&t, lg);
    free(t.b);
    if (!ok || rename(tmp, path))
        return -1;
    if (lg)
        fprintf(lg, "PDB: %s was missing; the complete %s put back\n", path, tmp);
    return 1;
}

#define PS_TRK_ANLZ 14 /* the track row's string 14: analyze_path */
/* Track row string k (of 21; u16 offsets at 0x5e) set to v: the row's bytes before its first
 * string are kept, then every string in index order, copied byte for byte, k re-encoded.
 * 0 = unchanged (k already v), 1 = rewritten (*len updated), -1 = the row cannot be rebuilt. */
static int ps_row_set_str(unsigned char *r, unsigned *len, size_t cap, int k, const char *v) {
    unsigned off[21], lo = *len;
    for (int j = 0; j < 21; j++) {
        off[j] = ps_u16(r + 0x5e + 2 * j);
        if (off[j] < 0x88 || off[j] >= *len || off[j] + ps_slen(r + off[j]) > *len)
            return -1;
        if (off[j] < lo)
            lo = off[j];
    }
    char cur[1024];
    ps_str(r + off[k], *len - off[k], cur, sizeof cur);
    if (!strcmp(cur, v))
        return 0;
    unsigned char nb[4096], enc[1024];
    unsigned fl = 0;
    size_t el = ps_enc(v, enc, sizeof enc, &fl);
    if (!el || fl || cap > sizeof nb)
        return -1; /* a path that does not fit whole is not the path */
    size_t o = lo;
    memcpy(nb, r, lo);
    for (int j = 0; j < 21; j++) {
        const unsigned char *src = j == k ? enc : r + off[j];
        size_t n = j == k ? el : ps_slen(r + off[j]);
        if (o + n > cap)
            return -1;
        ps_w16(nb + 0x5e + 2 * j, (unsigned) o);
        memcpy(nb + o, src, n);
        o += n;
    }
    memcpy(r, nb, o);
    *len = (unsigned) o;
    return 1;
}
static const int PS_REF_OFF[] = {0x0c, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x3c, 0x40, 0x44};
static const unsigned PS_REF_TYP[] = {PT_ARTIST, PT_ART, PT_KEY, PT_ARTIST, PT_LABEL,
                                      PT_ARTIST, PT_GENRE, PT_ALBUM, PT_ARTIST};

/* id of the dst row named nm in table typ, appending a minimal new row when none matches */
static int ps_ensure(ps_buf *d, unsigned typ, const char *nm, unsigned album_artist, unsigned *id) {
    struct ps_named *v = 0;
    int n = ps_names(d, typ, &v);
    if (n < 0) {
        free(v);
        return -1;
    }
    /* compared as ps_names reads it back: the name as this row would store it */
    unsigned char enc[700];
    size_t EL = ps_enc(nm, enc, sizeof enc - 22, 0);
    char key[300], stored[256];
    if (!EL) {
        free(v);
        return -1;
    }
    ps_str(enc, EL, stored, sizeof stored);
    if (typ == PT_ALBUM)
        snprintf(key, sizeof key, "%s\x1f%u", stored, album_artist);
    else
        snprintf(key, sizeof key, "%s", stored);
    unsigned mx = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(v[i].name, key)) {
            *id = v[i].id;
            free(v);
            return 0;
        }
        if (v[i].id > mx)
            mx = v[i].id;
    }
    free(v);
    unsigned char r[700];
    size_t h, L;
    unsigned nid = mx + 1;
    switch (typ) {
    case PT_ARTIST:
        ps_w16(r, 0x60);
        ps_w16(r + 2, 0);
        ps_w32(r + 4, nid);
        r[8] = 3;
        r[9] = 10;
        h = 10;
        break;
    case PT_ALBUM:
        ps_w16(r, 0x80);
        ps_w16(r + 2, 0);
        ps_w32(r + 4, 0);
        ps_w32(r + 8, album_artist);
        ps_w32(r + 12, nid);
        ps_w32(r + 16, 0);
        r[20] = 3;
        r[21] = 22;
        h = 22;
        break;
    case PT_KEY:
        ps_w32(r, nid);
        ps_w32(r + 4, nid);
        h = 8;
        break;
    default:
        ps_w32(r, nid);
        h = 4;
        break;
    }
    L = EL;
    memcpy(r + h, enc, L);
    if (ps_append(d, typ, r, h + L, typ == PT_ARTIST || typ == PT_ALBUM) < 0)
        return -1;
    *id = nid;
    return 0;
}
static const char *ps_name_of(struct ps_named *v, int n, unsigned id) {
    for (int i = 0; i < n; i++)
        if (v[i].id == id)
            return v[i].name;
    return 0;
}

#define PS_AZCP_ART "/PIONEER/Artwork/azcp/" /* where a cross-stick paste writes its art */
/* 1 = rel names a non-empty regular file on the stick at root */
static int ps_file_ok(const char *root, const char *rel) {
    char p[1200];
    struct stat st;
    if (!rel || rel[0] != '/' || (size_t) snprintf(p, sizeof p, "%s%s", root, rel) >= sizeof p)
        return 0;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}
/* Step 2: the art a paste copied, for a track this pdb already lists. A paste reuses the
 * target's own content row for a track the target already has, and points that row at the
 * copied art when its own art file is missing (ls_art_fix); this track row must follow, or
 * the Device Library still shows none. Only art under PS_AZCP_ART is considered: export.pdb
 * names its own art files (a<n>.jpg) apart from OneLibrary's (b<n>.jpg), so the two paths
 * differ on every rekordbox-made stick and are never mirrored in general. A row whose own
 * art file is on the stick keeps it. loc: path -> row of the tracks this pdb had before step
 * 1 (a row step 1 appended already carries the art). Returns the rows changed, or -1. */
static int ps_paste_art(ps_buf *d, const char *root, const struct ps_map *loc,
                        const struct pdbs_track *trk, int ntrk) {
    struct ps_named *av = 0;
    int nav = -1, fixed = 0;
    for (int i = 0; i < ntrk; i++) {
        unsigned at, nid;
        if (!trk[i].art || strncmp(trk[i].art, PS_AZCP_ART, sizeof PS_AZCP_ART - 1) ||
            !ps_map_get(loc, trk[i].path, &at))
            continue;
        unsigned cur = ps_u32(ps_page(d, at >> 16) + (at & 0xffff) + 0x1c);
        if (nav < 0 && (nav = ps_names(d, PT_ART, &av)) < 0)
            goto fail;
        const char *cp = cur ? ps_name_of(av, nav, cur) : 0;
        if ((cp && !strcmp(cp, trk[i].art)) || (cp && ps_file_ok(root, cp)) ||
            !ps_file_ok(root, trk[i].art))
            continue; /* already, its own art is there, or the copy is not */
        if (ps_ensure(d, PT_ART, trk[i].art, 0, &nid))
            goto fail;
        ps_w32(ps_page(d, at >> 16) + (at & 0xffff) + 0x1c, nid); /* ps_ensure may move d->b */
        fixed++;
        /* av names the new row too: two OneLibrary rows with one path reach this row twice */
        struct ps_named *g = realloc(av, (size_t) (nav + 1) * sizeof *av);
        if (!g)
            goto fail;
        av = g;
        av[nav].id = nid;
        snprintf(av[nav].name, sizeof av[nav].name, "%s", trk[i].art);
        nav++;
    }
    free(av);
    return fixed;
fail:
    free(av);
    return -1;
}

/* the whole step. root: the stick ("/media/usb/sda"); src_root: the paste's source stick
 * or 0. 0 = export.pdb updated; >0 = nothing to do; <0 = failed, original kept / put back. */
static int pdbs_sync_one(const char *root, const char *src_root, const struct pdbs_pl *pl, int npl,
                         const struct pdbs_item *it, int nit, const struct pdbs_track *trk, int ntrk,
                         FILE *lg) {
    char path[600], bak[620];
    snprintf(path, sizeof path, "%s/%s/rekordbox/export.pdb", root, pd_dir(root));
    ps_buf d, s = {0};
    struct ps_map dids = {0}, sloc = {0}, dloc = {0};
    struct ps_named *sn[PT_ART + 1] = {0};
    int snn[PT_ART + 1] = {0};
    int rc = -1, added = 0, nosrc = 0;
    if (ps_recover(path, lg) < 0 && lg)
        fprintf(lg, "PDB: %s missing and %s.aznew not usable\n", path, path);
    if (ps_load(path, &d)) {
        if (lg)
            fprintf(lg, "PDB: no readable %s: skipped\n", path);
        return 1;
    }
    unsigned char **rows = 0;
    size_t *lens = 0;
    size_t on = d.n;
    unsigned char *orig = malloc(on); /* what is on the stick now */
    if (!orig) {
        free(d.b);
        return -1;
    }
    memcpy(orig, d.b, on);
    if (ps_tracks(&d, &dids, &dloc)) {
        if (lg)
            fprintf(lg, "PDB: %s track table unreadable: skipped\n", path);
        goto out;
    }
    int base_trk = ps_count(&d, PT_TRACK);
    if (base_trk < 0)
        goto out;
    /* 1. the source's own rows for OneLibrary tracks this pdb lacks */
    if (src_root) {
        char sp[600];
        snprintf(sp, sizeof sp, "%s/%s/rekordbox/export.pdb", src_root, pd_dir(src_root));
        if (ps_load(sp, &s) || s.pl != d.pl || ps_tracks(&s, 0, &sloc)) {
            if (lg)
                fprintf(lg, "PDB: source %s unreadable: no rows to copy\n", sp);
            free(s.b);
            s.b = 0;
        } else {
            static const unsigned T[] = {PT_GENRE, PT_ARTIST, PT_ALBUM, PT_LABEL, PT_KEY};
            for (int i = 0; i < 5; i++)
                if ((snn[T[i]] = ps_names(&s, T[i], &sn[T[i]])) < 0)
                    goto out;
        }
    }
    for (int i = 0; i < ntrk; i++) {
        unsigned loc, sid;
        if (ps_map_get(&dids, trk[i].path, 0))
            continue;
        if (!s.b || !ps_map_get(&sloc, trk[i].path, &loc)) {
            nosrc++;
            continue;
        }
        const unsigned char *sp = ps_page(&s, loc >> 16) + (loc & 0xffff);
        unsigned len = 0;
        for (int k = 0; k < 21; k++) {
            unsigned f = ps_u16(sp + 0x5e + 2 * k);
            if ((loc & 0xffff) + f + 3 > s.pl)
                goto out; /* a string header past the source page */
            unsigned e = f + ps_slen(sp + f);
            if (e > len)
                len = e;
        }
        if ((loc & 0xffff) + len > s.pl || len > 4000)
            goto out;
        unsigned char r[4096];
        memcpy(r, sp, len);
        /* the analysis files: where this paste put them. A paste moves them to a fresh folder
         * when the source's folder is another track's on this stick (cp_copy_track); the
         * source row still names the source's folder, which here holds that other track's
         * waveform, beat grid and cues. A row whose path cannot be set is not copied. */
        if (trk[i].anlz && trk[i].anlz[0]) {
            int ar = ps_row_set_str(r, &len, sizeof r, PS_TRK_ANLZ, trk[i].anlz);
            if (ar < 0) {
                if (lg)
                    fprintf(lg, "PDB: %s: analysis path not settable: row not copied\n",
                            trk[i].path);
                nosrc++;
                continue;
            }
        }
        for (int k = 0; k < 9; k++) {
            unsigned typ = PS_REF_TYP[k], nid = 0;
            sid = ps_u32(r + PS_REF_OFF[k]);
            if (!sid && typ != PT_ART) /* art: OneLibrary's, also where the source row has none */
                continue;
            if (typ == PT_ART) {
                if (trk[i].art && trk[i].art[0] && ps_ensure(&d, PT_ART, trk[i].art, 0, &nid))
                    goto out;
            } else if (typ == PT_ALBUM) {
                const char *an = ps_name_of(sn[PT_ALBUM], snn[PT_ALBUM], sid);
                if (!an)
                    goto out;
                char nm[300];
                snprintf(nm, sizeof nm, "%s", an);
                char *cut = strchr(nm, '\x1f');
                unsigned sa = cut ? (unsigned) atoi(cut + 1) : 0;
                if (cut)
                    *cut = 0;
                unsigned da = 0;
                if (sa) {
                    const char *ar = ps_name_of(sn[PT_ARTIST], snn[PT_ARTIST], sa);
                    if (!ar || ps_ensure(&d, PT_ARTIST, ar, 0, &da))
                        goto out;
                }
                if (ps_ensure(&d, PT_ALBUM, nm, da, &nid))
                    goto out;
            } else {
                const char *nm = ps_name_of(sn[typ], snn[typ], sid);
                if (!nm || ps_ensure(&d, typ, nm, 0, &nid))
                    goto out;
            }
            ps_w32(r + PS_REF_OFF[k], nid);
        }
        unsigned mx = 0;
        for (unsigned h = 0; h < dids.cap; h++)
            if (dids.k[h] && dids.v[h] > mx)
                mx = dids.v[h];
        ps_w32(r + 0x48, mx + 1);
        if (ps_append(&d, PT_TRACK, r, len, 1) < 0)
            goto out;
        ps_map_put(&dids, trk[i].path, mx + 1);
        added++;
    }
    /* 2. a paste's art for tracks this pdb already had */
    int arts = ps_paste_art(&d, root, &dloc, trk, ntrk);
    if (arts < 0)
        goto out;
    /* 3. playlist_tree + playlist_entries mirror OneLibrary (sort = 0-based position under the
     * parent) */
    rows = calloc(npl + nit + 1, sizeof *rows);
    lens = calloc(npl + nit + 1, sizeof *lens);
    int *ord = malloc((npl + 1) * sizeof *ord);
    if (!rows || !lens || !ord) {
        free(ord);
        goto out;
    }
    for (int i = 0; i < npl; i++)
        ord[i] = i;
    for (int i = 1; i < npl; i++)
        for (int j = i; j > 0 && pl[ord[j - 1]].id > pl[ord[j]].id; j--) {
            int t = ord[j];
            ord[j] = ord[j - 1];
            ord[j - 1] = t;
        }
    for (int i = 0; i < npl; i++) {
        const struct pdbs_pl *p = &pl[ord[i]];
        unsigned rank = 0;
        for (int j = 0; j < npl; j++)
            if (pl[j].parent == p->parent &&
                (pl[j].seq < p->seq || (pl[j].seq == p->seq && pl[j].id < p->id)))
                rank++;
        rows[i] = malloc(20 + 1100);
        if (!rows[i]) {
            free(ord);
            goto out;
        }
        ps_w32(rows[i], p->parent);
        ps_w32(rows[i] + 4, 0);
        ps_w32(rows[i] + 8, rank);
        ps_w32(rows[i] + 12, p->id);
        ps_w32(rows[i] + 16, p->folder ? 1 : 0);
        unsigned fl = 0;
        size_t L = ps_enc(p->name ? p->name : "", rows[i] + 20, 1100, &fl);
        if (fl && lg) /* written as it fits; one name never stops the sync */
            fprintf(lg, "PDB: playlist %u's name %s in export.pdb\n", p->id,
                    fl & PS_ENC_CUT ? "cut at a character boundary" : "repaired (ill-formed UTF-8)");
        lens[i] = 20 + L;
    }
    free(ord);
    {
        if (ps_rewrite(&d, PT_TREE, rows, lens, npl, 0x18))
            goto out;
    }
    int ne = 0;
    unsigned cur = 0, k = 0;
    for (int i = 0; i < nit; i++) {
        unsigned tid;
        if (!ps_map_get(&dids, it[i].path, &tid))
            continue;
        if (it[i].pl != cur) {
            cur = it[i].pl;
            k = 0;
        }
        unsigned char *e = rows[npl + ne] = malloc(12);
        if (!e)
            goto out;
        ps_w32(e, ++k);
        ps_w32(e + 4, tid);
        ps_w32(e + 8, it[i].pl);
        lens[npl + ne] = 12;
        ne++;
    }
    {
        if (ps_rewrite(&d, PT_ENTRY, rows + npl, lens + npl, ne, 12))
            goto out;
    }
    /* 4. re-parse, back up (once per insertion), then the whole file replaced (ps_replace:
     *    export.pdb.aznew written and renamed over it) */
    if (ps_verify(&d, npl, ne, base_trk + added, lg) || ps_open_ok(&d, lg))
        goto out;
    /* every data page we are about to write must pass rekordbox's dirty-bits walk: a page
     * that fails it crashes rekordbox the next time rekordbox writes to that page */
    for (size_t p = 1; p < d.n / d.pl; p++) {
        const unsigned char *x = d.b + p * d.pl;
        if ((x[0x1b] & 0x40) || ((p + 1) * d.pl <= on && !memcmp(x, orig + p * d.pl, d.pl)))
            continue;
        if (!ps_walk_ok(&d, x)) {
            if (lg)
                fprintf(
                    lg,
                    "PDB verify: page %zu (table %u) dirty count %u from row %u is not supplied by "
                    "its dirty masks (rekordbox 7.2.19 crash): export.pdb left as it was\n",
                    p, ps_u32(x + 8), ps_u16(x + 0x20) & 0x3fff, ps_u16(x + 0x22) & 0x3fff);
            goto out;
        }
    }
    if (d.n == on && !memcmp(d.b, orig, on)) {
        rc = 1;
        if (lg)
            fprintf(lg, "PDB: %s already consistent\n", path);
        goto out;
    }
    {
        snprintf(bak, sizeof bak, "%s.azbak", path);
        static char done_key[8][620];
        static dev_t done_dev[8];
        static ino_t done_ino[8];
        char db[620];
        struct stat st;
        snprintf(db, sizeof db, "%s/%s/rekordbox/exportLibrary.db", root, pd_dir(root));
        int have = 0, slot = 0;
        if (stat(db, &st) == 0)
            for (int i = 0; i < 8; i++) {
                if (!strcmp(done_key[i], bak) && done_dev[i] == st.st_dev &&
                    done_ino[i] == st.st_ino)
                    have = 1;
                if (!done_key[i][0])
                    slot = i;
            }
        if (!have) {
            if (ps_write_file(bak, orig, on)) {
                if (lg)
                    fprintf(lg, "PDB: backup %s failed: nothing written\n", bak);
                goto out;
            }
            if (stat(db, &st) == 0) {
                snprintf(done_key[slot], sizeof done_key[slot], "%s", bak);
                done_dev[slot] = st.st_dev;
                done_ino[slot] = st.st_ino;
            }
        }
    }
    {
        int w = ps_replace(path, d.b, d.n);
        if (w) {
            if (lg)
                fprintf(lg, "PDB: replacing %s failed (%d)%s\n", path, w,
                        w == -2 ? ": putting the original back" : ": export.pdb as it was");
            if (w == -2)
                ps_replace(path, orig, on);
            goto out;
        }
    }
    if (lg)
        fprintf(lg,
                "PDB: %s: %d track row(s) copied from the source, %d without a source row (popup "
                "stays for those); %d given a paste's artwork; %d playlists, %d entries\n",
                path, added, nosrc, arts, npl, ne);
    rc = 0;
out:
    if (rc < 0 && lg)
        fprintf(lg, "PDB: %s NOT updated (OneLibrary edit stands)\n", path);
    if (rows)
        for (int i = 0; i < npl + nit + 1; i++)
            free(rows[i]);
    free(rows);
    free(lens);
    free(d.b);
    free(s.b);
    free(orig);
    ps_map_free(&dids);
    ps_map_free(&sloc);
    ps_map_free(&dloc);
    for (int i = 0; i <= PT_ART; i++)
        free(sn[i]);
    return rc;
}
/* Every stick has its own library thread, and the work buffers above (ps_tracks, ps_pages,
 * ps_append, ...) and the backup record are shared: one sync at a time. A sync takes well
 * under a second; the other stick's thread waits for it. */
static pthread_mutex_t g_ps_lock = PTHREAD_MUTEX_INITIALIZER;
static int pdbs_sync(const char *root, const char *src_root, const struct pdbs_pl *pl, int npl,
                     const struct pdbs_item *it, int nit, const struct pdbs_track *trk, int ntrk,
                     FILE *lg) {
    pthread_mutex_lock(&g_ps_lock);
    int rc = pdbs_sync_one(root, src_root, pl, npl, it, nit, trk, ntrk, lg);
    pthread_mutex_unlock(&g_ps_lock);
    return rc;
}
