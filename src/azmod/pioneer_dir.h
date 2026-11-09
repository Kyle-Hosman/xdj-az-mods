/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file pioneer_dir.h
 * @brief The folder a stick's rekordbox export is in, named the way EP147 names it.
 *
 * rekordbox exports into PIONEER/ on most sticks and into the hidden .PIONEER/ on a stick
 * formatted HFS+. EP147 picks the name from the stick's filesystem alone, never by looking for
 * either folder: its media info reference (MountMediaInfoReference, 1.30 0xdd5ad8) records the
 * filesystem from the mount (EP147's table: vfat 1, exfat 2, hfsplus 3, nfs 4), and the path
 * builder at 1.30 0x181d758 takes ".PIONEER" when it is 3 (0xdd5f08) and "PIONEER" otherwise.
 * pd_dir() asks the kernel's mount table the same question.
 *
 * pd_is_hfsplus and pd_cut are pure (the host tests compile them); pd_dir reads /proc/mounts.
 */
#ifndef AZMOD_PIONEER_DIR_H
#define AZMOD_PIONEER_DIR_H
#include <stdio.h>
#include <string.h>

#define PD_DIR "PIONEER"
#define PD_DIR_HFS ".PIONEER"

/* 1 when the mount table text `mounts` (/proc/mounts) mounts `mount` with filesystem hfsplus.
 * The last line for that mount point wins, as the kernel stacks mounts. Fields are separated by
 * single spaces; a mount point with an escaped character never equals `mount` here. */
static inline int pd_is_hfsplus(const char *mounts, const char *mount) {
    size_t ml = strlen(mount);
    int hfs = 0;
    for (const char *l = mounts; l && *l;) {
        const char *e = strchr(l, '\n');
        size_t n = e ? (size_t) (e - l) : strlen(l);
        const char *mp = (const char *) memchr(l, ' ', n);
        if (mp) {
            mp++;
            size_t rest = n - (size_t) (mp - l);
            const char *fs = (const char *) memchr(mp, ' ', rest);
            if (fs && (size_t) (fs - mp) == ml && !memcmp(mp, mount, ml)) {
                fs++;
                size_t left = n - (size_t) (fs - l);
                const char *fe = (const char *) memchr(fs, ' ', left);
                size_t fl = fe ? (size_t) (fe - fs) : left;
                hfs = fl == 7 && !memcmp(fs, "hfsplus", 7);
            }
        }
        l = e ? e + 1 : 0;
    }
    return hfs;
}

/* In `path`, the '/' that starts "/PIONEER/<rest>" or "/.PIONEER/<rest>" (the first one), or 0:
 * what comes before it is the stick's mount point. */
static inline const char *pd_cut(const char *path, const char *rest) {
    size_t rl = strlen(rest);
    for (const char *p = path ? strchr(path, '/') : 0; p; p = strchr(p + 1, '/')) {
        const char *q = p + 1 + (p[1] == '.');
        if (!strncmp(q, "PIONEER/", 8) && !strncmp(q + 8, rest, rl))
            return p;
    }
    return 0;
}

/* The file a path stored in a stick's library names, its last `drop` characters replaced by
 * `suffix`, into out (snprintf's result): a stored "/PIONEER/<rest>" is <rest> inside the export
 * folder `dir` (EP147 reads it so: on an HFS+ stick whose library says /PIONEER/USBANLZ/... it
 * finds the analysis files in .PIONEER/USBANLZ/...); anything else is mount + stored. */
static inline int pd_file_in(char *out, size_t n, const char *mount, const char *dir,
                             const char *stored, size_t drop, const char *suffix) {
    size_t L = strlen(stored);
    if (drop > L)
        drop = L;
    if (!strncmp(stored, "/" PD_DIR "/", sizeof PD_DIR + 1) && L - drop >= sizeof PD_DIR + 1)
        return snprintf(out, n, "%s/%s%.*s%s", mount, dir, (int) (L - drop - sizeof PD_DIR),
                        stored + sizeof PD_DIR, suffix);
    return snprintf(out, n, "%s%.*s%s", mount, (int) (L - drop), stored, suffix);
}

#ifndef PD_PURE
/* The export folder's name on the stick mounted at `mount`: ".PIONEER" on HFS+, else "PIONEER".
 * Reads the mount table (its first 16 KiB, far more than a deck's), so it is for code that already
 * does file I/O, never a paint path. */
static const char *pd_dir(const char *mount) {
    char t[16384];
    FILE *f = fopen("/proc/mounts", "re");
    if (!f)
        return PD_DIR;
    size_t n = fread(t, 1, sizeof t - 1, f);
    fclose(f);
    t[n] = 0;
    return pd_is_hfsplus(t, mount) ? PD_DIR_HFS : PD_DIR;
}
/* pd_file_in for the stick mounted at `mount`, with its export folder from the mount table */
static int pd_file(char *out, size_t n, const char *mount, const char *stored, size_t drop,
                   const char *suffix) {
    return pd_file_in(out, n, mount, pd_dir(mount), stored, drop, suffix);
}
#endif
#endif
