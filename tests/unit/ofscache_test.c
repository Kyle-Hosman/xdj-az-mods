/* SPDX-License-Identifier: GPL-3.0-only */
/* The stem offset cache, <stems folder>/azstem.ofs (ofs_cache_parse / _load / _save, extracted out
 * of guest/azstem/ofsload.c by run.sh), and stems_key (guest/azstem/stem_playback.c). A cached lag
 * opens the stems at load without measuring, so a wrong one would play every stem out of time:
 *   - saved and loaded back for the same source and stem set: the same lag;
 *   - a different source size or mtime, a different stem set, or a lag outside the search window:
 *     a miss;
 *   - any other content (a cut line, an extra field, a sign or digit where none belongs, a value too
 *     large for its type, another magic): a miss;
 *   - a save that cannot finish leaves the previous file whole and no temporary file behind;
 *   - stems_key changes when a stem file changes and is 0 when one cannot be read.
 * argv[1] = a scratch directory. */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "check.h"

#define NDECK 4
#define NSTEM 4
#define OFS_LAGMAX 8192
static char g_env_dir[NDECK][1024];
#include OFSCACHE_INC

static int parse(const char *line, long *lag) {
    return ofs_cache_parse(line, 1000, 2000, 3000ULL, lag);
}
static void put(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

int main(int argc, char **argv) {
    ut_suite = "stem offset cache";
    if (argc < 2)
        return 2;
    mkdir(argv[1], 0755);
    long lag = 0;

    CHECK("a line for this source and stem set is taken", parse("AZOFS2 1000 2000 3000 1042\n", &lag) && lag == 1042);
    CHECK("a negative lag is taken", parse("AZOFS2 1000 2000 3000 -77\n", &lag) && lag == -77);
    CHECK("the window's edges are taken",
          parse("AZOFS2 1000 2000 3000 8192\n", &lag) && parse("AZOFS2 1000 2000 3000 -8192\n", &lag));
    lag = 5;
    CHECK("a lag past the window is a miss, and *lag is untouched",
          !parse("AZOFS2 1000 2000 3000 8193\n", &lag) && !parse("AZOFS2 1000 2000 3000 -8193\n", &lag) &&
              lag == 5);
    CHECK("another source size is a miss", !parse("AZOFS2 1001 2000 3000 1042\n", &lag));
    CHECK("another source mtime is a miss", !parse("AZOFS2 1000 2001 3000 1042\n", &lag));
    CHECK("another stem set is a miss", !parse("AZOFS2 1000 2000 3001 1042\n", &lag));
    CHECK("another magic is a miss", !parse("AZOFS1 1000 2000 3000 1042\n", &lag) &&
                                         !parse("AZOFS2X 1000 2000 3000 1042\n", &lag) &&
                                         !parse("azofs2 1000 2000 3000 1042\n", &lag));
    CHECK("a line without its newline is a miss", !parse("AZOFS2 1000 2000 3000 1042", &lag));
    CHECK("an extra field is a miss", !parse("AZOFS2 1000 2000 3000 1042 9\n", &lag));
    CHECK("a missing field is a miss", !parse("AZOFS2 1000 2000 1042\n", &lag));
    CHECK("text after the newline is a miss", !parse("AZOFS2 1000 2000 3000 1042\nx", &lag));
    CHECK("two spaces, a tab, a plus sign or a leading space is a miss",
          !parse("AZOFS2 1000  2000 3000 1042\n", &lag) && !parse("AZOFS2 1000\t2000 3000 1042\n", &lag) &&
              !parse("AZOFS2 +1000 2000 3000 1042\n", &lag) && !parse(" AZOFS2 1000 2000 3000 1042\n", &lag));
    CHECK("a sign on the stems key is a miss", !parse("AZOFS2 1000 2000 -3000 1042\n", &lag));
    CHECK("a value too large for its type is a miss",
          !parse("AZOFS2 99999999999999999999 2000 3000 1042\n", &lag) &&
              !parse("AZOFS2 1000 2000 99999999999999999999 1042\n", &lag) &&
              !parse("AZOFS2 1000 2000 3000 99999999999999999999\n", &lag));
    CHECK("an empty line is a miss", !parse("", &lag) && !parse("\n", &lag));
    CHECK("the largest stems key is taken",
          ofs_cache_parse("AZOFS2 1 2 18446744073709551615 0\n", 1, 2, 18446744073709551615ULL, &lag));

    /* save and load through the deck's folder */
    snprintf(g_env_dir[2], sizeof g_env_dir[2], "%s", argv[1]);
    char path[1200], tmp[1300];
    ofs_cache_path(2, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    unlink(path);
    CHECK("no file: a miss", !ofs_cache_load(2, 1000, 2000, 3000ULL, &lag));
    ofs_cache_save(2, 1000, 2000, 3000ULL, -1042);
    lag = 0;
    CHECK("saved and loaded back: the same lag", ofs_cache_load(2, 1000, 2000, 3000ULL, &lag) && lag == -1042);
    CHECK("no temporary file is left", access(tmp, F_OK) != 0);
    CHECK("the saved file is a miss for another stem set", !ofs_cache_load(2, 1000, 2000, 3001ULL, &lag));
    put(path, "AZOFS2 1000 2000 30");
    CHECK("a cut file is a miss", !ofs_cache_load(2, 1000, 2000, 3000ULL, &lag));
    CHECK("no folder for the deck: nothing loaded", !ofs_cache_load(3, 1000, 2000, 3000ULL, &lag));

    /* a save that cannot finish: the temporary name is taken by a directory */
    ofs_cache_save(2, 1000, 2000, 3000ULL, 7);
    mkdir(tmp, 0755);
    ofs_cache_save(2, 1000, 2000, 3000ULL, 9);
    lag = 0;
    CHECK("a save that cannot finish leaves the previous file whole",
          ofs_cache_load(2, 1000, 2000, 3000ULL, &lag) && lag == 7);
    rmdir(tmp);

    /* stems_key */
    char paths[NSTEM][1600];
    memset(paths, 0, sizeof paths);
    snprintf(paths[0], sizeof paths[0], "%s/vocals.ogg", argv[1]);
    snprintf(paths[3], sizeof paths[3], "%s/drums.ogg", argv[1]);
    put(paths[0], "v");
    put(paths[3], "dd");
    unsigned long long k1 = stems_key(paths);
    CHECK("a stem set has a nonzero key", k1 != 0);
    CHECK("the same files give the same key", stems_key(paths) == k1);
    put(paths[3], "ddd");
    unsigned long long k2 = stems_key(paths);
    CHECK("a stem file of another size changes the key", k2 != 0 && k2 != k1);
    char swapped[NSTEM][1600];
    memset(swapped, 0, sizeof swapped);
    memcpy(swapped[1], paths[0], sizeof paths[0]);
    memcpy(swapped[3], paths[3], sizeof paths[3]);
    CHECK("the same file in another slot changes the key", stems_key(swapped) != k2);
    unlink(paths[3]);
    CHECK("a stem file that cannot be read: key 0, nothing cached", stems_key(paths) == 0);
    unlink(paths[0]);
    unlink(path);
    return ut_summary();
}
