/* SPDX-License-Identifier: GPL-3.0-only */
/* Where a stick's rekordbox export is (guest/azmod/pioneer_dir.h), as EP147 decides it: .PIONEER on
 * a stick the mount table lists as hfsplus, PIONEER on any other filesystem, the last mount of a
 * mount point counting; and the mount point cut back out of a path under PIONEER/ or .PIONEER/.
 * PIONEERDIR_HDR = the header under test. */
#include "check.h"
#define PD_PURE
#include PIONEERDIR_HDR

static const char *MOUNTS =
    "/dev/root / ext4 rw,relatime 0 0\n"
    "/dev/sda1 /media/usb/sda1 vfat rw,relatime,fmask=0022 0 0\n"
    "/dev/sdb1 /media/usb/sdb1 hfsplus rw,relatime,umask=22 0 0\n"
    "/dev/sdc1 /media/usb/sdc1 exfat rw 0 0\n"
    "/dev/sdd1 /media/usb/sdd1 hfsplus rw 0 0\n"
    "/dev/sdd2 /media/usb/sdd1 vfat rw 0 0\n"
    "/dev/sde1 /media/usb/sde1 hfsplusx rw 0 0\n"
    "/dev/sdf1 /media/usb/sdf1 hfsplus";

int main(void) {
    CHECK("vfat: PIONEER", !pd_is_hfsplus(MOUNTS, "/media/usb/sda1"));
    CHECK("hfsplus: .PIONEER", pd_is_hfsplus(MOUNTS, "/media/usb/sdb1"));
    CHECK("exfat: PIONEER", !pd_is_hfsplus(MOUNTS, "/media/usb/sdc1"));
    CHECK("a mount point mounted again: the last mount counts", !pd_is_hfsplus(MOUNTS, "/media/usb/sdd1"));
    CHECK("a filesystem name that only starts with hfsplus is not HFS+", !pd_is_hfsplus(MOUNTS, "/media/usb/sde1"));
    CHECK("the table's last line, without options or a newline", pd_is_hfsplus(MOUNTS, "/media/usb/sdf1"));
    CHECK("a mount point that only starts like another is not it", !pd_is_hfsplus(MOUNTS, "/media/usb/sdb"));
    CHECK("an unknown mount point: PIONEER", !pd_is_hfsplus(MOUNTS, "/media/usb/zz9"));
    CHECK("an empty table: PIONEER", !pd_is_hfsplus("", "/media/usb/sdb1"));

    const char *a = "/media/usb/sdb1/PIONEER/rekordbox/exportLibrary.db";
    const char *b = "/media/usb/sdb1/.PIONEER/rekordbox/exportLibrary.db";
    CHECK("cut: PIONEER/rekordbox/", pd_cut(a, "rekordbox/") == a + 15);
    CHECK("cut: .PIONEER/rekordbox/", pd_cut(b, "rekordbox/") == b + 15);
    CHECK("cut: another folder under PIONEER is not rekordbox/", !pd_cut("/m/s/PIONEER/USBANLZ/x", "rekordbox/"));
    CHECK("cut: a folder named like PIONEER is not it", !pd_cut("/m/s/XPIONEER/rekordbox/x", "rekordbox/"));
    CHECK("cut: no path", !pd_cut(0, "rekordbox/"));
    char o[200];
    pd_file_in(o, sizeof o, "/m/s", ".PIONEER", "/PIONEER/USBANLZ/P001/0000ABCD/ANLZ0000.DAT", 3, "EXT");
    CHECK("stored: /PIONEER/... is in .PIONEER on HFS+, its extension replaced",
          !strcmp(o, "/m/s/.PIONEER/USBANLZ/P001/0000ABCD/ANLZ0000.EXT"));
    pd_file_in(o, sizeof o, "/m/s", "PIONEER", "/PIONEER/USBANLZ/x.DAT", 0, "");
    CHECK("stored: /PIONEER/... stays in PIONEER on FAT", !strcmp(o, "/m/s/PIONEER/USBANLZ/x.DAT"));
    pd_file_in(o, sizeof o, "/m/s", ".PIONEER", "/.PIONEER/USBANLZ/x.DAT", 0, "");
    CHECK("stored: a path already under /.PIONEER/ as written", !strcmp(o, "/m/s/.PIONEER/USBANLZ/x.DAT"));
    pd_file_in(o, sizeof o, "/m/s", ".PIONEER", "/Contents/a/b.mp3", 0, "");
    CHECK("stored: a path outside PIONEER as written", !strcmp(o, "/m/s/Contents/a/b.mp3"));
    pd_file_in(o, sizeof o, "/m/s", ".PIONEER", "/PIONEERX/a", 0, "");
    CHECK("stored: a folder only starting with PIONEER as written", !strcmp(o, "/m/s/PIONEERX/a"));
    return ut_summary();
}
