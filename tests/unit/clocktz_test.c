/* SPDX-License-Identifier: GPL-3.0-only */
/* The Clock readout's zone logic (guest/azmod/clock_tz.h): TZif parsing, the offset at a time,
 * and the 12/24-hour text. Real zone files from the IANA tz database
 * (tools/unit-tests/fixtures/zoneinfo) and files built here for the edge cases.
 *
 * Usage: clocktz_test <fixtures/zoneinfo dir> */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clock_tz.h"
#include "check.h"

static struct ctz g_z;

static size_t load(const char *dir, const char *name, unsigned char *b, size_t cap) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f)
        return 0;
    size_t n = fread(b, 1, cap, f);
    fclose(f);
    return n;
}
static int off_at(const struct ctz *z, int64_t t) { /* the offset, or 99999 = unknown */
    int32_t o;
    return ctz_offset(z, t, &o) == 0 ? o : 99999;
}

/* A version 2 file: a minimal v1 part, then times[n] (UTC), types (offsets), idx[n], footer. */
static size_t build(unsigned char *b, const int64_t *at, const unsigned char *ix, int n,
                    const int32_t *off, int nt, const char *footer) {
    size_t k = 0;
    memset(b, 0, 44);
    memcpy(b, "TZif2", 5);
    b[20 + 4 * 4 + 3] = 1; /* v1 typecnt 1 */
    b[20 + 5 * 4 + 3] = 4; /* v1 charcnt 4 */
    k = 44;
    memset(b + k, 0, 6 + 4); /* one type, "UTC\0" */
    k += 10;
    memset(b + k, 0, 44);
    memcpy(b + k, "TZif2", 5);
    b[k + 20 + 3 * 4 + 2] = (unsigned char) (n >> 8);
    b[k + 20 + 3 * 4 + 3] = (unsigned char) n;
    b[k + 20 + 4 * 4 + 3] = (unsigned char) nt;
    b[k + 20 + 5 * 4 + 3] = 4;
    k += 44;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < 8; j++)
            b[k++] = (unsigned char) ((uint64_t) at[i] >> (56 - 8 * j));
    for (int i = 0; i < n; i++)
        b[k++] = ix[i];
    for (int i = 0; i < nt; i++) {
        for (int j = 0; j < 4; j++)
            b[k++] = (unsigned char) ((uint32_t) off[i] >> (24 - 8 * j));
        b[k++] = 0;
        b[k++] = 0;
    }
    memcpy(b + k, "ABC", 4);
    k += 4;
    if (footer) {
        b[k++] = '\n';
        memcpy(b + k, footer, strlen(footer));
        k += strlen(footer);
        b[k++] = '\n';
    }
    return k;
}

/* US daylight time in a sample year: it begins on the second Sunday of March at 09:00 UTC
 * (2:00 MST) and ends on the first Sunday of November at 08:00 UTC. */
#define US_ON_2026 1772960400LL
#define US_OFF_2026 1793520000LL
#define JUL_2026 1782928800LL /* July 1 of that year, 18:00 UTC */
#define JAN_2026 1768500000LL /* January 15 of that year, 18:00 UTC */

int main(int argc, char **argv) {
    ut_suite = "clock_tz";
    static unsigned char b[1 << 16], c[1 << 16];
    const char *dir = argc > 1 ? argv[1] : "tools/unit-tests/fixtures/zoneinfo";
    char t[32];

    size_t n = load(dir, "America_Denver", b, sizeof b);
    CHECK("Denver: fixture read", n > 1000);
    CHECK("Denver: parses", ctz_parse(b, n, &g_z) == 0);
    CHECK("Denver: footer rule has DST", g_z.rule_dst == 1);
    CHECK("Denver: July is MDT (-6 h)", off_at(&g_z, JUL_2026) == -6 * 3600);
    CHECK("Denver: January is MST (-7 h)", off_at(&g_z, JAN_2026) == -7 * 3600);
    CHECK("Denver: 1 s before DST begins is MST", off_at(&g_z, US_ON_2026 - 1) == -7 * 3600);
    CHECK("Denver: the instant DST begins is MDT", off_at(&g_z, US_ON_2026) == -6 * 3600);
    CHECK("Denver: 1 s before DST ends is MDT", off_at(&g_z, US_OFF_2026 - 1) == -6 * 3600);
    CHECK("Denver: the instant DST ends is MST", off_at(&g_z, US_OFF_2026) == -7 * 3600);
    CHECK("Denver: past the last transition (2040) is unknown", off_at(&g_z, 2208988800LL) == 99999);

    n = load(dir, "Europe_Berlin", b, sizeof b);
    CHECK("Berlin: parses", n > 1000 && ctz_parse(b, n, &g_z) == 0);
    CHECK("Berlin: July is CEST (+2 h)", off_at(&g_z, JUL_2026) == 2 * 3600);
    CHECK("Berlin: January is CET (+1 h)", off_at(&g_z, JAN_2026) == 3600);

    n = load(dir, "Australia_Sydney", b, sizeof b);
    CHECK("Sydney: parses", n > 1000 && ctz_parse(b, n, &g_z) == 0);
    CHECK("Sydney: July is AEST (+10 h)", off_at(&g_z, JUL_2026) == 10 * 3600);
    CHECK("Sydney: January is AEDT (+11 h)", off_at(&g_z, JAN_2026) == 11 * 3600);

    n = load(dir, "Asia_Kolkata", b, sizeof b);
    CHECK("Kolkata: parses", n > 100 && ctz_parse(b, n, &g_z) == 0);
    CHECK("Kolkata: footer has no DST", g_z.rule_dst == 0);
    CHECK("Kolkata: 2026 is +5:30 (past its last transition, rule without DST)",
          off_at(&g_z, JUL_2026) == 5 * 3600 + 1800);

    n = load(dir, "Etc_UTC", b, sizeof b);
    CHECK("UTC: parses", n > 40 && ctz_parse(b, n, &g_z) == 0);
    CHECK("UTC: offset 0 in 2026 (past its one transition, rule UTC0)", off_at(&g_z, JUL_2026) == 0);

    /* A slim file: US rules stored only up to 2007, the rest left to the footer. */
    int64_t at[2] = {1173603600LL, 1194163200LL}; /* the first year of the current US rule: its March and November changes */
    unsigned char ix[2] = {1, 0};
    int32_t off[2] = {-7 * 3600, -6 * 3600};
    n = build(b, at, ix, 2, off, 2, "MST7MDT,M3.2.0,M11.1.0");
    CHECK("slim: parses", ctz_parse(b, n, &g_z) == 0);
    CHECK("slim: inside its transitions is known", off_at(&g_z, 1180000000LL) == -6 * 3600);
    CHECK("slim + DST rule: 2026 is unknown, not a guess", off_at(&g_z, JUL_2026) == 99999);
    n = build(b, at, ix, 2, off, 2, "MST7");
    CHECK("slim + rule without DST: 2026 is the last type",
          ctz_parse(b, n, &g_z) == 0 && off_at(&g_z, JUL_2026) == -7 * 3600);
    n = build(b, at, ix, 2, off, 2, "<-03>3");
    CHECK("slim + quoted name without DST: known",
          ctz_parse(b, n, &g_z) == 0 && g_z.rule_dst == 0);
    n = build(b, at, ix, 2, off, 2, "<-03>3<-02>,M3.5.0,M10.5.0");
    CHECK("slim + quoted name with DST: unknown", ctz_parse(b, n, &g_z) == 0 && g_z.rule_dst == 1);
    n = build(b, at, ix, 2, off, 2, NULL);
    CHECK("slim + no footer: 2026 is unknown", ctz_parse(b, n, &g_z) == 0 && off_at(&g_z, JUL_2026) == 99999);

    /* refused */
    int64_t back[2] = {1194163200LL, 1173603600LL};
    n = build(b, back, ix, 2, off, 2, "MST7");
    CHECK("times out of order: refused", ctz_parse(b, n, &g_z) == -1);
    unsigned char bad[2] = {0, 2};
    n = build(b, at, bad, 2, off, 2, "MST7");
    CHECK("type index out of range: refused", ctz_parse(b, n, &g_z) == -1);
    int32_t wild[2] = {-7 * 3600, 30 * 3600};
    n = build(b, at, ix, 2, wild, 2, "MST7");
    CHECK("offset beyond 26 h: refused", ctz_parse(b, n, &g_z) == -1);
    n = build(b, at, ix, 2, off, 2, "MST7");
    b[4] = 0;
    CHECK("version 1 only: refused", ctz_parse(b, n, &g_z) == -1);
    b[4] = '2';
    b[0] = 'X';
    CHECK("bad magic: refused", ctz_parse(b, n, &g_z) == -1);

    /* every truncation of a real file: refused, or (footer cut) never answers past the end */
    n = load(dir, "America_Denver", c, sizeof c);
    int bad_trunc = 0;
    for (size_t k = 0; k < n; k++) {
        int r = ctz_parse(c, k, &g_z);
        if (r == 0 && off_at(&g_z, 2208988800LL) != 99999)
            bad_trunc++;
        if (r != 0 && (g_z.n_trans || g_z.n_types))
            bad_trunc++;
    }
    CHECK("Denver truncated at every length: refused or unknown past the end", bad_trunc == 0);

    ctz_format(t, sizeof t, JUL_2026, -6 * 3600, 0, 0);
    CHECK("12-hour: 18:00 UTC in MDT is 12:00 PM", !strcmp(t, "12:00 PM"));
    ctz_format(t, sizeof t, JUL_2026 + 12 * 3600, -6 * 3600, 0, 0);
    CHECK("12-hour: midnight is 12:00 AM", !strcmp(t, "12:00 AM"));
    ctz_format(t, sizeof t, JUL_2026 + 65 * 60, -6 * 3600, 0, 0);
    CHECK("12-hour: 1:05 PM", !strcmp(t, "1:05 PM"));
    ctz_format(t, sizeof t, JUL_2026 + 65 * 60, -6 * 3600, 1, 0);
    CHECK("24-hour: 13:05", !strcmp(t, "13:05"));
    ctz_format(t, sizeof t, JUL_2026 + 12 * 3600, -6 * 3600, 1, 0);
    CHECK("24-hour: midnight is 00:00", !strcmp(t, "00:00"));

    /* seconds on: h:mm:ss with the 12-hour marker, HH:MM:SS in 24-hour */
    ctz_format(t, sizeof t, JUL_2026 + 12 * 3600 + 5, -6 * 3600, 0, 1);
    CHECK("12-hour seconds: 12:00:05 AM at midnight", !strcmp(t, "12:00:05 AM"));
    ctz_format(t, sizeof t, JUL_2026, -6 * 3600, 0, 1);
    CHECK("12-hour seconds: 12:00:00 PM at noon", !strcmp(t, "12:00:00 PM"));
    ctz_format(t, sizeof t, JUL_2026 + 6 * 3600 + 59 * 60 + 59, -6 * 3600, 0, 1);
    CHECK("12-hour seconds: 6:59:59 PM", !strcmp(t, "6:59:59 PM"));
    ctz_format(t, sizeof t, JUL_2026 + 12 * 3600 - 1, -6 * 3600, 0, 1);
    CHECK("12-hour seconds: 11:59:59 PM", !strcmp(t, "11:59:59 PM"));
    ctz_format(t, sizeof t, JUL_2026 + 12 * 3600 + 5, -6 * 3600, 1, 1);
    CHECK("24-hour seconds: 00:00:05 at midnight", !strcmp(t, "00:00:05"));
    ctz_format(t, sizeof t, JUL_2026 + 12 * 3600 - 1, -6 * 3600, 1, 1);
    CHECK("24-hour seconds: 23:59:59", !strcmp(t, "23:59:59"));
    ctz_format(t, sizeof t, JUL_2026 + 65 * 60 + 9, -6 * 3600, 0, 1);
    CHECK("12-hour seconds: 1:05:09 PM", !strcmp(t, "1:05:09 PM"));
    ctz_format(t, sizeof t, JUL_2026 + 65 * 60 + 9, -6 * 3600, 0, 0);
    CHECK("seconds off is the text without them", !strcmp(t, "1:05 PM"));
    {
        /* the longest text, with the " UTC" the UTC zone appends, fits the readout's buffer */
        char u[16];
        ctz_format(u, sizeof u, JUL_2026 - 6 * 3600 + 59 * 60 + 59, 0, 0, 1);
        strncat(u, " UTC", sizeof u - strlen(u) - 1);
        CHECK("longest seconds text plus UTC fits the 16-byte readout", !strcmp(u, "12:59:59 PM UTC"));
    }

    /* the zone list's offsets */
    ctz_utc_label(t, sizeof t, -7 * 3600);
    CHECK("offset label: UTC-7", !strcmp(t, "UTC-7"));
    ctz_utc_label(t, sizeof t, 5 * 3600 + 30 * 60);
    CHECK("offset label: UTC+5:30", !strcmp(t, "UTC+5:30"));
    ctz_utc_label(t, sizeof t, -(3 * 3600 + 30 * 60));
    CHECK("offset label: UTC-3:30", !strcmp(t, "UTC-3:30"));
    ctz_utc_label(t, sizeof t, 13 * 3600 + 45 * 60);
    CHECK("offset label: UTC+13:45", !strcmp(t, "UTC+13:45"));
    ctz_utc_label(t, sizeof t, 0);
    CHECK("offset label: UTC+0", !strcmp(t, "UTC+0"));
    ctz_utc_label(t, sizeof t, CTZ_MAX_OFF + 1);
    CHECK("offset label: out of range is UTC?", !strcmp(t, "UTC?"));
    {
        int32_t off = 0;
        n = load(dir, "America_Denver", b, sizeof b);
        CHECK("offset in effect: Denver in July is UTC-6 (daylight saving applied)",
              ctz_parse(b, n, &g_z) == 0 && ctz_offset(&g_z, JUL_2026, &off) == 0 &&
                  (ctz_utc_label(t, sizeof t, off), !strcmp(t, "UTC-6")));
        CHECK("offset in effect: Denver in January is UTC-7",
              ctz_offset(&g_z, JUL_2026 - 180LL * 86400, &off) == 0 &&
                  (ctz_utc_label(t, sizeof t, off), !strcmp(t, "UTC-7")));
    }
    return ut_summary();
}
