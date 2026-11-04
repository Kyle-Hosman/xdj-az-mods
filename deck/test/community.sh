# SPDX-License-Identifier: GPL-3.0-only
set -u
# deck/lib/az-community against a local HTTPS fixture, run by the deck's own userland:
# the 2.01 rootfs (busybox 1.27.2, curl 8.14.1 + OpenSSL, bash 3.2 as /bin/sh) is mounted
# read-only at /lower and overlaid writable at /deck, so the rootfs itself is never
# modified. Every case runs twice: under the deck's
# `busybox ash` and under the deck's /bin/sh (bash), the shell the loader really uses.
#   stick: a real FAT image mounted RO at /media/usb/sda1 (the stock mount; the fetcher
#          must remount rw and put it back)       p7: a 61 MB tmpfs at /home/root/settings
#   the fetcher is the stripped copy from the staged MODS/deck (/d), i.e. what ships.
# Needs: /lower (run.sh mounts the rootfs when it finds one), python3 + openssl (apk).
[ -x /lower/bin/busybox ] || { echo "FAIL no deck rootfs mounted at /lower (set AZ_DECK_ROOTFS for run.sh)"; exit 1; }
apk add -q python3 openssl dosfstools >/dev/null 2>&1 || { echo "FAIL apk add python3 openssl dosfstools"; exit 1; }
[ -f /d/lib/az-community ] || { echo "FAIL no staged deck/lib/az-community"; exit 1; }

PASS=0; FAILN=0
chk() { if eval "$2"; then echo "PASS [$SHN] $1"; PASS=$((PASS+1)); else echo "FAIL [$SHN] $1"; FAILN=$((FAILN+1)); fi; }

# ---- the deck userland ----------------------------------------------------------
mkdir -p /ov /deck && mount -t tmpfs ov /ov && mkdir -p /ov/u /ov/w
mount -t overlay overlay -o lowerdir=/lower,upperdir=/ov/u,workdir=/ov/w /deck || { echo "FAIL overlay mount"; exit 1; }
mount --bind /dev /deck/dev; mount -t proc proc /deck/proc
mount -t tmpfs tmp /deck/tmp; mount -t tmpfs run /deck/run
mkdir -p /deck/home/root/settings && mount -t tmpfs -o size=61m p7 /deck/home/root/settings
mkdir -p /deck/home/root/settings/az-usbload
mkdir -p /deck/tmp/az-deck/lib /deck/tmp/fx
cp /d/lib/az-community /d/lib/az-conf.sh /deck/tmp/az-deck/lib/

# ---- the stick: FAT, stock-style read-only mount --------------------------------
dd if=/dev/zero of=/stick.img bs=1M count=96 2>/dev/null; mkfs.vfat /stick.img >/dev/null
LO=$(losetup -f); losetup "$LO" /stick.img
mkdir -p /deck/media/usb/sda1 /m
mount "$LO" /m && mkdir -p /m/MODS && echo core > /m/MODS/azmod.so && umount /m
mount -o ro "$LO" /deck/media/usb/sda1
cleanup() { umount /deck/media/usb/sda1 2>/dev/null; losetup -d "$LO" 2>/dev/null; kill "$SRV" 2>/dev/null; }
trap cleanup EXIT

# ---- the fixture: CA + server cert for 127.0.0.1, then HTTPS ----------------------
B=https://127.0.0.1:8443
python3 /t/community_fixture.py gen /srv "$B" || { echo "FAIL fixture gen"; exit 1; }
cd /tmp || exit
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj /CN=fixture-ca -keyout ca.key -out ca.pem 2>/dev/null
openssl req -newkey rsa:2048 -nodes -subj /CN=127.0.0.1 -keyout srv.key -out srv.csr 2>/dev/null
printf 'subjectAltName=IP:127.0.0.1\n' > san.ext
openssl x509 -req -in srv.csr -CA ca.pem -CAkey ca.key -CAcreateserial -days 2 -extfile san.ext -out srv.pem 2>/dev/null
cp ca.pem /deck/tmp/fx/ca.pem
python3 /t/community_fixture.py serve /srv 8443 /tmp/srv.pem /tmp/srv.key & SRV=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do wget -q -O /dev/null --no-check-certificate "$B/ping" 2>/dev/null && break; sleep 0.5; done
SKIPPED=$(sed -n 's/^skipped=//p' /srv/expect.txt)

PING=$B/ping
# run the fetcher exactly as the watcher does: one argument, the request line
run() {
    # shellcheck disable=SC2086 # the shell is a command and its argument ("/bin/busybox ash")
    chroot /deck /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin AZ_MNT=/media/usb/sda1 AZ_MODS=/media/usb/sda1/MODS \
        AZ_STAGE=/home/root/settings/az-usbload AZ_COMMUNITY_LIST="$B/list.json" AZ_COMMUNITY_PING="$PING" \
        AZ_COMMUNITY_ICON_BUDGET=60 AZ_COMMUNITY_ICON_CAP="${ICAP:-8388608}" CURL_CA_BUNDLE=/tmp/fx/ca.pem $SHC /tmp/az-deck/lib/az-community "$*"
}
S=/deck/tmp/az-community/status; CAT=/deck/tmp/az-community/catalog
STK=/deck/media/usb/sda1/MODS; COMM=$STK/community-mods
st() { sed -n "s/^$1=//p" "$S"; }
# catalog lines (az-community CATALOG): repo|text|name|ok|listed|error
#   mod|repo|id|name|version|author|summary|category|hooks|firmware|size|icon|installed|
#       min_core|description|assets_kb|held|also|nalso|updated|downloads|beta|shots
rf() { awk -F'|' -v u="$1" -v k="$2" '$1=="repo"&&$2==u{ print $k; exit }' "$CAT"; }
mf() { awk -F'|' -v id="$1" -v k="$2" -v u="${3:-}" '$1=="mod"&&$3==id&&(u==""||$2==u){ print $k; exit }' "$CAT"; }
nrec() { awk -F'|' -v id="$1" -v u="$2" '$1=="mod"&&$3==id&&$2==u{ n++ } END{ print n+0 }' "$CAT"; }
fields_ok() { awk -F'|' 'NR==1{ if($0!="v1") bad=1; next } $1=="repo"{ if(NF!=6) bad=1; next } $1=="mod"{ if(NF!=23||more) bad=1; next } $1=="more"{ if(NF!=2||$2 !~ /^[1-9][0-9]*$/||more) bad=1; more=1; next } { bad=1 } END{ exit bad }' "$CAT"; }
stick_ro() { grep " /deck/media/usb/sda1 " /proc/mounts | grep -q "[ ,]ro[ ,]"; }
nothing_half() { ! for x in "$COMM"/.[!.]*; do printf '%s\n' "${x##*/}"; done | grep -qE '^\.[a-z0-9-]+\.(new|old)$'; }
# a mod's switch in MODS/azmods.cfg (community-mods.<id> = on|off)
ckey() { tr -d '\r' < "$STK/azmods.cfg" 2>/dev/null | sed -n "s/^community-mods\.$1 = //p" | tail -1; }
enabled_has() { [ "$(ckey "$1")" = on ]; }
is_off_key() { [ "$(ckey "$1")" = off ]; }
outside_stick() { (cd /deck/media/usb/sda1 && find . | grep -v '^\./MODS/community-mods' | grep -vx '\./MODS/azmods\.cfg' | sort | md5sum); }
# change MODS/community-mods on the stick by hand (rw, then the stock ro mount again)
rwst() { umount /deck/media/usb/sda1; mount "$LO" /m; ( cd /m/MODS/community-mods && eval "$1" ); umount /m; mount -o ro "$LO" /deck/media/usb/sda1; }
clog() { cat /deck/tmp/az-community/log.old /deck/tmp/az-community/log 2>/dev/null; }
# p7 apart from the one file the fetcher may write (the approved list, its lock and its tmp)
p7_other() { (cd /deck/home/root/settings && find . | grep -vx './azmod-community\|./azmod-community/approved\|./azmod-community/\.approved\.lock' | sort | md5sum); }
AP=/deck/home/root/settings/azmod-community/approved
pkgsha() { unzip -p "/srv/pkg/$1-$2.azmod" "$1.so" | sha256sum | cut -d' ' -f1; }   # $1 id $2 version
appr() { grep "^$1 " "$AP" 2>/dev/null; }
appr_fmt() { [ -f "$AP" ] && ! grep -qvE '^[a-z0-9-]{3,32} [0-9a-f]{64}$' "$AP" && [ "$(cut -d' ' -f1 "$AP" | sort | uniq -d)" = "" ]; }
R1=$B/r1/azmods-repo.json; R2N=$B/nolen/r2/azmods-repo.json
# shellcheck disable=SC2034 # read by the eval'd assertions below
TAB=$(printf '\t')
for SHC in "/bin/busybox ash" "/bin/sh"; do
SHN=${SHC##*/}; SHN=${SHN%% *}; [ "$SHN" = busybox ] && SHN=ash
echo "== [$SHN] fresh state"
rm -rf /deck/tmp/az-community
umount /deck/media/usb/sda1; mount "$LO" /m; rm -rf /m/MODS/community-mods /m/MODS/azmods.cfg; mkdir -p /m/MODS/community-mods
printf '%s\n' "$R2N" > /m/MODS/community-mods/repos.txt; umount /m; mount -o ro "$LO" /deck/media/usb/sda1
rm -rf /deck/home/root/settings/azmod-community   # the deck's approved list starts empty for each shell
# shellcheck disable=SC2034 # read by the eval'd assertions below
P7_BEFORE=$(p7_other) OUT0=$(outside_stick)
cp /srv/r1/azmods-repo.json /srv/r1/cur.json 2>/dev/null; cp /srv/r1/azmods-repo.json /srv/r1/orig.json

echo "== [$SHN] refresh: Mod Index + user repo, good and broken repos in the SAME round"
t0=$(date +%s); run refresh; t1=$(date +%s)
chk "refresh ends done" '[ "$(st state)" = done ] && [ "$(st op)" = refresh ]'
chk "refresh message names only problems: the unreadable repositories, no mod count (the screen counts the catalog)" '! st msg | grep -q Found && st msg | grep -q "^[0-9]* of [0-9]* repositories could not be read\.$" && [ "$(grep -c "^mod|" $CAT)" -gt 0 ]'
chk "status carries stamp= (seconds since boot) and msg last" '[ -n "$(st stamp)" ] && tail -1 $S | grep -q ^msg='
chk "catalog is 6.1: v1 first, repo lines 6 fields, mod lines 22 fields, nothing else" 'fields_ok'
chk "beta: 1 for beta true (shots-mod), 0 without it (good-mod) and for a non-boolean \"yes\" (shots2-mod) -- CONTROL" '[ "$(mf shots-mod 22)" = 1 ] && [ "$(mf good-mod 22 $R1)" = 0 ] && [ "$(mf shots2-mod 22)" = 0 ]'
chk "screenshots: three local paths under /tmp/az-community/shots for shots-mod; none without (good-mod) -- CONTROL" '[ "$(mf shots-mod 23 | tr " " "\n" | grep -c "^/tmp/az-community/shots/[0-9a-f]\{16\}\.png$")" = 3 ] && [ "$(mf shots-mod 23 | tr " " "\n" | sort -u | wc -l)" = 3 ] && [ -z "$(mf good-mod 23 $R1)" ]'
chk "a screenshot list with an http:// URL, or more than 3, drops the screenshots, never the mod" '[ -z "$(mf shots2-mod 23)" ] && [ -z "$(mf shots4-mod 23)" ] && [ "$(mf shots2-mod 5)" = 1.0.0 ] && [ "$(mf shots4-mod 5)" = 1.0.0 ]'
chk "a refresh downloads no screenshots" '! ls /deck/tmp/az-community/shots/*.png >/dev/null 2>&1'
run shots "$R1" shots-mod
chk "shots: done, op shots, id shots-mod, no message" '[ "$(st state)" = done ] && [ "$(st op)" = shots ] && [ "$(st id)" = shots-mod ] && [ -z "$(st msg)" ]'
chk "shots: the PNG is at the catalog's first path (same bytes); the non-PNG and the one over 1 MB are not -- CONTROL" 'p=$(mf shots-mod 23); cmp -s /deck$(echo $p | cut -d" " -f1) /srv/shots/s1.png && [ ! -e /deck$(echo $p | cut -d" " -f2) ] && [ ! -e /deck$(echo $p | cut -d" " -f3) ] && clog | grep -q "screenshot refused or failed: .*notpng" && clog | grep -q "screenshot refused or failed: .*big.png"'
chk "shots: nothing partial left, nothing written to the stick" '! ls /deck/tmp/az-community/shots/*.part >/dev/null 2>&1 && stick_ro && [ "$(outside_stick)" = "$OUT0" ]'
run shots "$R1" 'bad;id'
chk "shots: a bad id is refused" '[ "$(st state)" = error ]'
chk "min_core carried in field 14 (core-mod 9.0.0), empty when the repository file has none (good-mod) -- CONTROL" '[ "$(mf core-mod 14 $R1)" = 9.0.0 ] && [ -z "$(mf good-mod 14 $R1)" ]'
chk "a malformed min_core skips the entry" '! grep -q "^mod|[^|]*|badcore-mod|" $CAT'
chk "a version that is not MAJOR.MINOR.PATCH (1.2) skips the entry (CONTROL: good-mod 1.0.0 is listed)" '! grep -q "^mod|[^|]*|badver-mod|" $CAT && [ "$(mf good-mod 5 $R1)" = 1.0.0 ] && clog | grep -q "skipped entry: SKIP.*version"'
chk "a catalog under the cap has no more| line" '! grep -q "^more|" $CAT'
chk "r1 ok=1 listed=1, no error" '[ "$(rf $R1 4)" = 1 ] && [ "$(rf $R1 5)" = 1 ] && [ -z "$(rf $R1 6)" ]'
chk "r1 name decoded from \\u00e9" '[ "$(rf $R1 3)" = "Fixture R1 é" ]'
chk "r1: the $SKIPPED invalid entries were skipped (logged)" '[ "$(grep -c "skipped entry" /deck/tmp/az-community/log)" -ge $SKIPPED ]'
chk "user repo (chunked, no Content-Length) ok=1 listed=0 -- CONTROL for nolen/big" '[ "$(rf $R2N 4)" = 1 ] && [ "$(rf $R2N 5)" = 0 ]'
chk "index > 256 KB refused (Content-Length)" '[ "$(rf $B/big/azmods-repo.json 4)" = 0 ] && rf $B/big/azmods-repo.json 6 | grep -q 256'
chk "index > 256 KB refused (NO Content-Length: cap holds mid-transfer)" '[ "$(rf $B/nolen/big/azmods-repo.json 4)" = 0 ]'
chk "broken JSON repo -> error, no mods" '[ "$(rf $B/broken/azmods-repo.json 4)" = 0 ] && rf $B/broken/azmods-repo.json 6 | grep -q JSON && ! grep -q "^mod|$B/broken/" $CAT'
chk "not-a-repo JSON -> error" '[ "$(rf $B/notrepo/azmods-repo.json 4)" = 0 ]'
chk "list entries that are not repos (space, http://) never reach the catalog" '! grep -q "^repo|http://\|^repo|not" $CAT'
chk "a | inside a value is stripped, a long UTF-8 name capped at 47 bytes, still valid UTF-8" 'n=$(mf other-mod 4); [ ${#n} -le 47 ] && ! printf "%s" "$n" | grep -q "|" && printf "%s" "$n" | python3 -c "import sys; sys.stdin.buffer.read().decode(\"utf-8\")"'
chk "an unknown category is dropped, a known one kept" '[ -z "$(mf other-mod 8)" ] && [ "$(mf good-mod 8 $R1)" = visual ]'
# the fixture's well-formed date, read from the served index so the test names no date itself
# shellcheck disable=SC2034  # UPD is read inside chk's single-quoted eval string
UPD=$(sed -n 's/.*"updated": "\([0-9]\{4\}-[0-9]\{2\}-[0-9]\{2\}\)".*/\1/p' /srv/r1/azmods-repo.json | head -1)
chk "the index's updated date is field 20; CONTROL: a malformed one (one-digit month and day) is empty" '[ -n "$UPD" ] && [ "$(mf good-mod 20 $R1)" = "$UPD" ] && [ -z "$(mf dd-mod 20)" ] && [ "$(mf any-mod 20)" = "$UPD" ]'
chk "the download count is field 21 (good-mod 1840, any-mod 0); CONTROL: a mod stats.json has no count for is empty" '[ "$(mf good-mod 21 $R1)" = 1840 ] && [ "$(mf any-mod 21)" = 0 ] && [ -z "$(mf case-mod 21)" ]'
chk "a damaged count (a string, 14 digits, negative, a fraction) is dropped, not guessed" '[ -z "$(mf dd-mod 21)" ] && [ -z "$(mf core-mod 21)" ] && [ -z "$(mf nofw-mod 21)" ] && [ -z "$(mf small-mod 21)" ]'
chk "a repository added by hand never shows a count, even one stats.json lists (other-mod 99)" '[ -z "$(mf other-mod 21)" ] && [ "$(rf $R2N 5)" = 0 ]'
chk "the counts are kept on the stick (MODS/community-mods/.stats; the hand-added one kept, never shown), the stick read-only again" '[ "$(grep -c . $COMM/.stats)" = 3 ] && grep -q "${TAB}good-mod${TAB}1840\$" $COMM/.stats && grep -q "${TAB}other-mod${TAB}99\$" $COMM/.stats && stick_ro && nothing_half'
chk "good-mod listed with its fields" '[ "$(mf good-mod 5 $R1)" = 1.0.0 ] && [ "$(mf good-mod 6 $R1)" = fixture ] && [ "$(mf good-mod 9 $R1)" = 0 ] && [ "$(mf good-mod 10 $R1)" = ep147-679e1b451e5fe82b ] && [ "$(mf good-mod 11 $R1)" -gt 0 ] && [ -z "$(mf good-mod 13 $R1)" ]'
chk "good-mod icon: a local PNG under /tmp/az-community/icons" 'f=$(mf good-mod 12 $R1); case "$f" in /tmp/az-community/icons/*.png) ;; *) false ;; esac && [ "$(head -c 8 /deck$f | od -An -tx1 | tr -d " \n")" = 89504e470d0a1a0a ]'
chk "any-mod listed with firmware any; its non-PNG icon refused (icon empty)" '[ "$(mf any-mod 10)" = any ] && [ -z "$(mf any-mod 12)" ]'
chk "skipped entries absent (bad id, >16 MB, no firmware, http package, any+hooks)" '! grep -qE "^mod\|[^|]*\|(Bad_Id|huge-mod|nofwidx-mod|http-mod|anyhooksidx-mod)\|" $CAT'
# good-mod is in r1 twice (one skipped) and in r2 (the user's repository) at 2.0.0: one line per id
chk "good-mod: one catalog line for the id, from r1 (the Mod Index beats a repository the user added)" '[ "$(grep -c "^mod|[^|]*|good-mod|" $CAT)" = 1 ] && [ "$(nrec good-mod $R1)" = 1 ] && [ "$(nrec good-mod $R2N)" = 0 ]'
chk "good-mod's line names the other repository offering the id (also=r2, nalso=1), and nothing holds it" '[ "$(mf good-mod 18 $R1)" = "$R2N" ] && [ "$(mf good-mod 19 $R1)" = 1 ] && [ -z "$(mf good-mod 17 $R1)" ]'
chk "CONTROL: a mod no other repository offers carries no conflict (other-mod: also empty, nalso 0)" '[ -z "$(mf other-mod 18)" ] && [ "$(mf other-mod 19)" = 0 ] && [ "$(nrec other-mod $R2N)" = 1 ]'
chk "winners: good-mod -> r1, other-mod -> r2" 'grep -qx "good-mod	$R1" /deck/tmp/az-community/cache/winners && grep -qx "other-mod	$R2N" /deck/tmp/az-community/cache/winners'
run install "$R2N" good-mod
chk "the same id from the repository the catalog does not offer is refused, nothing written" '[ "$(st state)" = error ] && st msg | grep -q "offers a mod with the same ID" && [ ! -e $COMM/good-mod ] && nothing_half && ! enabled_has good-mod'
chk "refresh took < 60 s" '[ $((t1 - t0)) -lt 60 ]'

echo "== [$SHN] install good-mod (the happy path)"
run install "$R1" good-mod
chk "status done, op install, id good-mod" '[ "$(st state)" = done ] && [ "$(st op)" = install ] && [ "$(st id)" = good-mod ]'
chk "install message: the mod's name and what to do next" 'st msg | grep -q "^Good Mod 1.0.0 installed. Press Reload now"'
chk "files on the stick" '[ -f $COMM/good-mod/azmod.json ] && [ -f $COMM/good-mod/good-mod.so ] && [ -f $COMM/good-mod/icon.png ] && [ -f $COMM/good-mod/shots/1.png ]'
chk ".so bytes are the package bytes" 'unzip -p /srv/pkg/good-mod-1.0.0.azmod good-mod.so | cmp -s - $COMM/good-mod/good-mod.so'
chk ".origin names the repo + version + sha" 'grep -qx "repo=$R1" $COMM/good-mod/.origin && grep -qx "version=1.0.0" $COMM/good-mod/.origin && grep -q "^sha256=[0-9a-f]\{64\}$" $COMM/good-mod/.origin'
chk "enabled lists good-mod" 'enabled_has good-mod'
chk "stick back to read-only" 'stick_ro'
chk "no half-installed leftovers" 'nothing_half'
chk "catalog: installed=1.0.0 on the r1 record" '[ "$(mf good-mod 13 $R1)" = 1.0.0 ]'
chk "a deck install approves exactly the installed .so (id + its sha256 on the deck's list)" '[ "$(appr good-mod)" = "good-mod $(pkgsha good-mod 1.0.0)" ]'
chk "the approved list is in core's format: <id> <64 hex>, one line per id" 'appr_fmt'
chk "the fetcher wrote nothing else on p7 (the loader mirrors the stage)" '[ "$(p7_other)" = "$P7_BEFORE" ]'
chk "the approval is logged" 'clog | grep -q "approved good-mod $(pkgsha good-mod 1.0.0)"'

echo "== [$SHN] refused installs leave nothing behind"
refused() {   # $1 id, $2 grep -E for msg (chk evals with ITS OWN $1/$2: copy them)
    # shellcheck disable=SC2034 # read by the eval'd assertions below
    RID=$1 RWHY=$2
    run install "$R1" "$RID"
    chk "$RID refused: $(st msg | cut -c1-70)" '[ "$(st state)" = error ] && st msg | grep -qiE "$RWHY" && [ ! -e $COMM/$RID ] && nothing_half && ! enabled_has $RID && stick_ro'
}
refused badsha-mod "sha256"
refused small-mod "larger than the repository file"
refused trav-mod "absolute or \.\. path"
refused datadir-mod "has a data/ folder"
refused abs-mod "absolute or \.\. path"
refused mid-trav-mod "absolute or \.\. path"
refused nosrc-mod "source"
refused api2-mod "api 2"
refused nofw-mod "firmware"
refused anyhooks-mod "cannot claim hooks"
refused idmis-mod "id is not"
refused vermis-mod "repository file says 1.0.0"
refused badjson-mod "azmod.json is not valid JSON"
refused noso-mod "no noso-mod.so"
refused zip64-mod "streamed zip|too large"
refused space-mod "spaces"
refused hidden-mod "hidden"
refused offsw-mod "its own offsw-mod.off"
refused bomb-mod "16 MB|too large"
refused notzip-mod "not a readable zip"
refused mcshort-mod "min_core must be MAJOR.MINOR.PATCH"
refused bigman-mod "larger than 16383 bytes"
run install "$R1" badver-mod
chk "a version that is not MAJOR.MINOR.PATCH cannot be installed" '[ "$(st state)" = error ] && st msg | grep -q "no longer listed" && [ ! -e $COMM/badver-mod ]'
for RID in mcok-mod edgeman-mod; do
    run install "$R1" $RID
    chk "CONTROL: $RID installs (min_core 1.0.0 / azmod.json of exactly 16383 bytes)" '[ "$(st state)" = done ] && [ -f $COMM/$RID/$RID.so ]'
    run remove $RID
done
refused case-mod "differ only in case"
run install "$R1" assets-mod
chk "CONTROL: a package with assets/ installs, assets on the stick" '[ "$(st state)" = done ] && [ "$(cat $COMM/assets-mod/assets/kits/kick.wav)" = "$(printf "K%.0s" $(seq 100))" ]'
run remove assets-mod
chk "no traversal target exists anywhere" '! find /deck/tmp /deck/media /deck/home /ov/u -name "*evil*" 2>/dev/null | grep -q .'
run install "$R1" huge-mod
chk "an entry skipped at refresh cannot be installed" '[ "$(st state)" = error ] && st msg | grep -q "no longer listed"'
run install "$R1" ../etc
chk "a bad id is refused before anything" '[ "$(st state)" = error ] && st msg | grep -q "bad mod id"'
run install "$R1" dd-mod
chk "a data-descriptor zip (Finder style) installs, bytes exact" '[ "$(st state)" = done ] && unzip -p /srv/pkg/dd-mod-1.0.0.azmod dd-mod.so | cmp -s - $COMM/dd-mod/dd-mod.so'
run remove dd-mod
chk "CONTROL: good-mod still installed and enabled after all the refusals" '[ -f $COMM/good-mod/good-mod.so ] && enabled_has good-mod'

echo "== [$SHN] reserved ids (names the fetcher keeps in MODS/community-mods) are refused"
chk "a repository file entry with id 'enabled' is skipped at refresh" '! grep -q "^mod|[^|]*|enabled|" $CAT'
run install "$R1" enabled
chk "install enabled -> bad mod id, the enabled list intact" '[ "$(st state)" = error ] && st msg | grep -q "bad mod id" && [ -f $STK/azmods.cfg ] && enabled_has good-mod && nothing_half'
run remove enabled
chk "remove enabled -> bad mod id, the enabled list intact" '[ "$(st state)" = error ] && st msg | grep -q "bad mod id" && [ -f $STK/azmods.cfg ] && enabled_has good-mod'
for RID in azmod azstem az-mods azmods; do
    run disable $RID; chk "disable $RID -> bad mod id" '[ "$(st state)" = error ] && st msg | grep -q "bad mod id"'
done
run disable enabled-plus
chk "CONTROL: a longer id that only starts with a reserved one is not refused as bad" '[ "$(st state)" = error ] && st msg | grep -q "not installed"'

echo "== [$SHN] same id from another repository is blocked"
chk "catalog: r2's good-mod has no line of its own, r1's carries the installed version" '[ "$(nrec good-mod $R2N)" = 0 ] && [ "$(mf good-mod 13 $R1)" = 1.0.0 ] && [ -z "$(mf good-mod 17 $R1)" ]'
run install "$R2N" good-mod
chk "install from r2 refused with the installed mod's name and author, r1 copy intact" '[ "$(st state)" = error ] && [ "$(st msg)" = "Good Mod by fixture already uses this ID. Remove it first." ] && grep -qx "repo=$R1" $COMM/good-mod/.origin && unzip -p /srv/pkg/good-mod-1.0.0.azmod good-mod.so | cmp -s - $COMM/good-mod/good-mod.so'
# the installed copy decides which line shows: as if good-mod had come from r2 (the user's
# repository) before the Mod Index offered the same id
rwst 'cp good-mod/.origin /tmp/gm.origin; sed -i "s#^repo=.*#repo='"$R2N"'#" good-mod/.origin'; run catalog
chk "installed from r2: the one line is r2's, with the installed version; r1's is counted as also" '[ "$(nrec good-mod $R2N)" = 1 ] && [ "$(nrec good-mod $R1)" = 0 ] && [ "$(mf good-mod 13 $R2N)" = 1.0.0 ] && [ "$(mf good-mod 18 $R2N)" = "$R1" ] && [ -z "$(mf good-mod 17 $R2N)" ]'
chk "installed from r2: the update offered is r2's 2.0.0, never r1's" '[ "$(mf good-mod 5 $R2N)" = 2.0.0 ]'
run install "$R1" good-mod
chk "installed from r2: r1 (in the Mod Index) cannot replace it either" '[ "$(st state)" = error ] && st msg | grep -q "already uses this ID" && grep -qx "repo=$R2N" $COMM/good-mod/.origin'
# installed from a repository no longer in the set: the id is held, the line says by whom
rwst 'sed -i "s#^repo=.*#repo=https://example.com/gone/azmods-repo.json#" good-mod/.origin'; run catalog
chk "installed from a repository that is gone: r1's line shows, held=that repository, no installed version" '[ "$(nrec good-mod $R1)" = 1 ] && [ "$(mf good-mod 17 $R1)" = example.com/gone ] && [ -z "$(mf good-mod 13 $R1)" ]'
run install "$R1" good-mod
chk "... and installing r1's is refused naming the installed one" '[ "$(st state)" = error ] && st msg | grep -q "^Good Mod by fixture already uses this ID" && grep -q "^repo=https://example.com/gone/" $COMM/good-mod/.origin'
rwst 'mv good-mod/.origin /tmp/gm.away'; run catalog
chk "a folder copied by hand (no .origin): held=-" '[ "$(mf good-mod 17 $R1)" = "-" ] && [ -z "$(mf good-mod 13 $R1)" ]'
rwst 'cp /tmp/gm.origin good-mod/.origin'; run catalog
chk "CONTROL: back to the r1 record: installed again, nothing held" '[ "$(mf good-mod 13 $R1)" = 1.0.0 ] && [ -z "$(mf good-mod 17 $R1)" ] && fields_ok'
run install "$R2N" other-mod
chk "other-mod from the no-Content-Length repo installs" '[ "$(st state)" = done ] && [ -f $COMM/other-mod/other-mod.so ] && enabled_has other-mod'

echo "== [$SHN] disable / enable"
run disable good-mod
chk "disable: community.good-mod = off in azmods.cfg" '[ "$(st state)" = done ] && is_off_key good-mod && stick_ro'
chk "disable: other-mod untouched" 'enabled_has other-mod'
chk "disable message: the name from azmod.json, after Reload" '[ "$(st msg)" = "Good Mod turns off after Reload." ]'
echo "== [$SHN] update while disabled keeps it disabled"
cp /srv/r1/azmods-repo.v2.json /srv/r1/azmods-repo.json; run refresh
chk "catalog: 1.0.1 offered while 1.0.0 is installed" '[ "$(mf good-mod 5 $R1)" = 1.0.1 ] && [ "$(mf good-mod 13 $R1)" = 1.0.0 ]'
rwst 'mkdir -p good-mod/data good-mod/.data@0123456789abcdef; echo mine > good-mod/data/save.txt; echo local > good-mod/.data.owner; echo other > good-mod/.data@0123456789abcdef/save.txt'
run install "$R1" good-mod
chk "updated to 1.0.1, still disabled" '[ "$(st state)" = done ] && grep -qx version=1.0.1 $COMM/good-mod/.origin && is_off_key good-mod && st msg | grep -q "stays disabled"'
chk "the new .so is 1.0.1" 'unzip -p /srv/pkg/good-mod-1.0.1.azmod good-mod.so | cmp -s - $COMM/good-mod/good-mod.so'
chk "the update keeps the mod's data/, its owner stamp and the set-aside copy (CONTROL: the .so above was replaced)" '[ "$(cat $COMM/good-mod/data/save.txt)" = mine ] && [ "$(cat $COMM/good-mod/.data.owner)" = local ] && [ "$(cat $COMM/good-mod/.data@0123456789abcdef/save.txt)" = other ] && nothing_half'
chk "a deck update keeps it approved: the id's line now holds the 1.0.1 .so's sha256" '[ "$(appr good-mod)" = "good-mod $(pkgsha good-mod 1.0.1)" ] && [ "$(pkgsha good-mod 1.0.1)" != "$(pkgsha good-mod 1.0.0)" ]'
chk "... and the 1.0.0 hash is gone (one line per id: the old build would ask again)" '! grep -q " $(pkgsha good-mod 1.0.0)$" $AP && appr_fmt'
# shellcheck disable=SC3044 # 'enable' is the az-community operation, an argument to run
run enable good-mod
chk "enable: community.good-mod = on again" '[ "$(st state)" = done ] && enabled_has good-mod'
chk "enable message" '[ "$(st msg)" = "Good Mod turns on after Reload." ]'
echo "== [$SHN] a broken update leaves the installed version intact"
cp /srv/r1/azmods-repo.v3.json /srv/r1/azmods-repo.json; run refresh; run install "$R1" good-mod
chk "1.0.2 refused (sha), 1.0.1 still there and enabled" '[ "$(st state)" = error ] && grep -qx version=1.0.1 $COMM/good-mod/.origin && unzip -p /srv/pkg/good-mod-1.0.1.azmod good-mod.so | cmp -s - $COMM/good-mod/good-mod.so && enabled_has good-mod && nothing_half'
chk "a refused update approves nothing: the line still holds 1.0.1" '[ "$(appr good-mod)" = "good-mod $(pkgsha good-mod 1.0.1)" ]'
cp /srv/r1/orig.json /srv/r1/azmods-repo.json

echo "== [$SHN] an interrupted swap is cleaned up, never loaded"
umount /deck/media/usb/sda1; mount "$LO" /m; mkdir -p /m/MODS/community-mods/.ghost-mod.new; echo x > /m/MODS/community-mods/.ghost-mod.new/f; umount /m; mount -o ro "$LO" /deck/media/usb/sda1
run catalog
chk "leftover .ghost-mod.new removed, stick ro" '[ ! -e $COMM/.ghost-mod.new ] && stick_ro'

echo "== [$SHN] an install or remove cut off at any point leaves the mod (old or new), never missing"
rm -rf /tmp/gm.save; rwst 'cp -r good-mod /tmp/gm.save'
gm_reset() { rwst 'rm -rf good-mod .good-mod.new .good-mod.old .good-mod.del; cp -r /tmp/gm.save good-mod'; }
is_old() { [ -f $COMM/good-mod/good-mod.so ] && [ ! -e $COMM/good-mod/marker ] && grep -qx version=1.0.1 $COMM/good-mod/.origin; }
is_new() { [ -f $COMM/good-mod/good-mod.so ] && [ "$(cat $COMM/good-mod/marker 2>/dev/null)" = new ] && [ -f $COMM/good-mod/.origin ]; }
gm_clean() { ! for x in "$COMM"/.[!.]*; do printf '%s\n' "${x##*/}"; done | grep -qE '^\.good-mod\.(new|old|del)$' && stick_ro; }
NEWC='cp -r good-mod .good-mod.new; echo new > .good-mod.new/marker'   # a complete new copy (.origin written)
gm_reset; rwst 'cp -r good-mod .good-mod.new; rm .good-mod.new/.origin .good-mod.new/good-mod.so'; run catalog
chk "cut during the copy: old version kept, partial copy cleaned" 'is_old && gm_clean'
gm_reset; rwst "$NEWC"; run catalog
chk "cut after the copy verified, before the swap: old version kept" 'is_old && gm_clean'
gm_reset; rwst "$NEWC; mv good-mod .good-mod.old"; run catalog
chk "cut between the two mv's: the install is finished (new version)" 'is_new && gm_clean && enabled_has good-mod'
gm_reset; rwst "$NEWC; mv good-mod .good-mod.old; mv .good-mod.new good-mod; rm .good-mod.old/good-mod.so"; run catalog
chk "cut while deleting the old copy: new version kept" 'is_new && gm_clean'
gm_reset; rwst 'mv good-mod .good-mod.old'; run catalog
chk "remove cut before the enabled list changed: the mod is put back" 'is_old && gm_clean && enabled_has good-mod'
gm_reset; rwst 'mv good-mod .good-mod.del; rm .good-mod.del/good-mod.so'; run catalog
chk "remove cut while deleting: the half copy is never put back" '[ ! -e $COMM/good-mod ] && gm_clean'
gm_reset; rwst 'echo junk > .enabled.new'; run catalog
chk "a leftover .enabled.new (a file, a reserved name) is deleted, never moved in" '[ ! -e $COMM/.enabled.new ] && enabled_has good-mod && is_old'
chk "CONTROL: both recoveries were logged" 'clog | grep -q "recovered good-mod: finished the interrupted install" && clog | grep -q "recovered good-mod: put the previous copy back"'
rm -rf /tmp/gm.save

echo "== [$SHN] remove"
run remove other-mod
chk "remove: dir gone, not enabled, catalog says so" '[ "$(st state)" = done ] && [ ! -e $COMM/other-mod ] && ! enabled_has other-mod && [ -z "$(mf other-mod 13)" ] && stick_ro'
chk "remove message names the mod" '[ "$(st msg)" = "Other Mod removed from the USB." ]'
chk "remove drops the id from the approved list" '[ -z "$(appr other-mod)" ] && appr_fmt'
chk "CONTROL: other-mod was approved before the remove, good-mod still is" 'clog | grep -q "approved other-mod " && [ "$(appr good-mod)" = "good-mod $(pkgsha good-mod 1.0.1)" ]'
run remove other-mod
chk "remove again: not installed" '[ "$(st state)" = error ] && st msg | grep -q "not installed"'

echo "== [$SHN] addrepo / delrepo"
run addrepo someone/az-mods
chk "owner/name kept as typed in repos.txt, shown as typed in the catalog" 'grep -qx "someone/az-mods" $COMM/repos.txt && [ "$(rf someone/az-mods 4)" = 0 ]'
chk "the unreachable new repo is reported, the rest of the catalog stands" '[ "$(st state)" = error ] && st msg | grep -q "someone/az-mods" && [ -n "$(mf good-mod 3 $R1)" ]'
run addrepo https://github.com/someone/az-mods.git
chk "github.com URL of the same repo -> already added" '[ "$(st state)" = done ] && st msg | grep -q "already" && [ "$(grep -c someone/az-mods $COMM/repos.txt)" = 1 ]'
run addrepo Someone/AZ-Mods
chk "the same GitHub repository in other letter case -> already added (GitHub ignores case)" '[ "$(st state)" = done ] && st msg | grep -q "already" && [ "$(grep -ci someone/az-mods $COMM/repos.txt)" = 1 ]'
chk "... and the catalog lists it once" '[ "$(grep -ci "^repo|someone/az-mods|" $CAT)" = 1 ]'
for bad in "../../etc" "http://example.com/azmods-repo.json" "https://example.com/x.json" "a;b/c" "file:///etc/passwd"; do
    run addrepo "$bad"; chk "addrepo refuses [$bad]" '[ "$(st state)" = error ] && st msg | grep -qiE "not a repository|must start with https"'
done
run addrepo "http://example.com/x/azmods-repo.json"
chk "an http:// address says https is required" '[ "$(st state)" = error ] && st msg | grep -q "must start with https://"'
run addrepo "$B/nothing-here/azmods-repo.json"
chk "a repository that answers 404 is reported as not found and NOT kept" '[ "$(st state)" = error ] && st msg | grep -q "Not found" && ! grep -q nothing-here $COMM/repos.txt'
chk "CONTROL: the unreachable repository above (no answer) IS kept" 'grep -qx "someone/az-mods" $COMM/repos.txt'
run delrepo https://github.com/someone/az-mods
chk "delrepo by another spelling removes it" '[ "$(st state)" = done ] && ! grep -q someone $COMM/repos.txt && grep -qx "$R2N" $COMM/repos.txt'
run delrepo someone/az-mods
chk "delrepo of an absent repo -> error" '[ "$(st state)" = error ]'
run addrepo "$B/stall/r2/azmods-repo.json"; t0=$(date +%s); run refresh; t1=$(date +%s)
chk "a stalled repo is abandoned by the 10 s stall timeout (refresh $((t1 - t0)) s < 40 s)" '[ $((t1 - t0)) -lt 40 ] && [ "$(rf $B/stall/r2/azmods-repo.json 4)" = 0 ]'
chk "CONTROL: the other repos of that round are fine" '[ "$(rf $R1 4)" = 1 ] && [ "$(rf $R2N 4)" = 1 ]'
run delrepo "$B/stall/r2/azmods-repo.json"

echo "== [$SHN] more mods than the catalog holds: the first 128, installed ones first, and a more| line"
NBEFORE=$(grep -c "^mod|" $CAT)
chk "CONTROL: before the big repository, every mod is in the catalog (no more| line)" '! grep -q "^more|" $CAT && fields_ok'
MANY=$B/many/azmods-repo.json
run addrepo "$MANY"; run install "$MANY" many-139; run refresh
chk "the catalog holds exactly 128 mod lines" '[ "$(grep -c "^mod|" $CAT)" = 128 ] && fields_ok'
chk "more| counts the rest ($NBEFORE + 140 - 128)" '[ "$(sed -n "s/^more|//p" $CAT)" = $((NBEFORE + 140 - 128)) ]'
chk "the installed mod from the end of the big repository is kept, with its installed version" '[ "$(mf many-139 13 $MANY)" = 1.0.0 ]'
chk "CONTROL: an uninstalled mod from the end of it is cut" '[ -z "$(mf many-138 3 $MANY)" ] && [ -n "$(mf many-000 3 $MANY)" ]'
chk "the refresh message counts nothing (the screen says it shows the first 128 from the more| line)" '! st msg | grep -q "Found\|showing"'
run remove many-139; run delrepo "$MANY"; run refresh
chk "CONTROL: without it, no more| line again" '! grep -q "^more|" $CAT && [ "$(grep -c "^mod|" $CAT)" = "$NBEFORE" ] && fields_ok'

echo "== [$SHN] community-mods = off is reported"
umount /deck/media/usb/sda1; mount "$LO" /m; cp /m/MODS/azmods.cfg /m/cfg.keep; echo "community-mods = off" >> /m/MODS/azmods.cfg; umount /m; mount -o ro "$LO" /deck/media/usb/sda1
run catalog
chk "status off=1" '[ "$(st off)" = 1 ]'
umount /deck/media/usb/sda1; mount "$LO" /m; mv /m/cfg.keep /m/MODS/azmods.cfg; umount /m; mount -o ro "$LO" /deck/media/usb/sda1

echo "== [$SHN] apply / settle"
umount /deck/media/usb/sda1; mount "$LO" /m; mv /m/MODS/azmod.so /m/MODS/azmod.so.away; umount /m; mount -o ro "$LO" /deck/media/usb/sda1
run apply
chk "apply on a stick without the core -> error, nothing to reload" '[ "$(st state)" = error ] && st msg | grep -q "no AZ-MODS"'
umount /deck/media/usb/sda1; mount "$LO" /m; mv /m/MODS/azmod.so.away /m/MODS/azmod.so; umount /m; mount -o ro "$LO" /deck/media/usb/sda1
run apply
chk "apply -> state=reloading (the watcher re-runs the loader)" '[ "$(st state)" = reloading ] && [ "$(st op)" = apply ]'
rm -rf /deck/home/root/settings/az-usbload/community
run settle
chk "settle with nothing staged (the loader did not mirror) -> error, not 'applied'" '[ "$(st state)" = error ] && st msg | grep -q "not applied"'
run apply
mkdir -p /deck/home/root/settings/az-usbload/community   # what the loader's community_mirror leaves
run settle
chk "settle -> done, no message (CONTROL: staged; the Installed list shows the result)" '[ "$(st state)" = done ] && [ "$(st op)" = apply ] && [ -z "$(st msg)" ]'
rm -rf /deck/home/root/settings/az-usbload/community   # the test's stand-in, not the fetcher's
run settle
chk "settle when not reloading changes nothing" '[ "$(st state)" = done ] && [ "$(st op)" = apply ] && [ -z "$(st msg)" ]'

echo "== [$SHN] offline"
PING=https://127.0.0.1:9/ping; run refresh; PING=$B/ping
chk "offline -> error + no internet, last catalog kept" '[ "$(st state)" = error ] && st msg | grep -q "No internet" && [ -n "$(mf good-mod 3 $R1)" ] && fields_ok'
run install "$R1" any-mod
chk "CONTROL: online again, any-mod installs" '[ "$(st state)" = done ] && [ -f $COMM/any-mod/any-mod.so ]'
echo "== [$SHN] the approved list cannot be written (core holds its lock throughout)"
run remove any-mod
chroot /deck /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin flock /home/root/settings/azmod-community/.approved.lock sleep 9 & HOLD=$!
sleep 1; run install "$R1" any-mod; wait $HOLD
chk "installed, nothing approved, and the message says to allow it on the deck" '[ "$(st state)" = done ] && [ -f $COMM/any-mod/any-mod.so ] && [ -z "$(appr any-mod)" ] && st msg | grep -q "press Allow" && clog | grep -q "could not record any-mod"'
run install "$R1" any-mod
chk "CONTROL: the same install with the lock free approves it" '[ "$(st state)" = done ] && [ "$(appr any-mod)" = "any-mod $(pkgsha any-mod 1.0.0)" ] && st msg | grep -q "Press Reload now"'

echo "== [$SHN] the curated list is not published (404) -> no curated repos, not an error"
mv /srv/list.json /srv/list.json.off; run refresh
chk "404 list: refresh done, no listed repos, user repo still there" '[ "$(st state)" = done ] && ! awk -F"|" "\$1==\"repo\"&&\$5==1{f=1} END{exit !f}" $CAT && [ "$(rf $R2N 4)" = 1 ]'
mv /srv/list.json.off /srv/list.json; run refresh
chk "CONTROL: list back -> r1 listed again" '[ "$(rf $R1 5)" = 1 ]'

echo "== [$SHN] download counts: offline, missing, changed, a damaged stick copy"
SC=/deck/tmp/az-community/cache/stats.tsv
rm -f $SC; PING=https://127.0.0.1:9/ping; run refresh; PING=$B/ping
chk "a new boot offline: the stick's last counts are shown" '[ "$(st state)" = error ] && [ "$(mf good-mod 21 $R1)" = 1840 ] && fields_ok'
mv /srv/stats.json /srv/stats.json.off; run refresh
chk "stats.json not reachable: refresh done, the last counts kept" '[ "$(st state)" = done ] && [ "$(mf good-mod 21 $R1)" = 1840 ] && clog | grep -q "download counts: download failed"'
rm -f $SC; rwst 'rm -f .stats'; run refresh
chk "none ever fetched: no count at all, never 0" '[ -z "$(mf good-mod 21 $R1)" ] && [ -z "$(mf any-mod 21)" ] && fields_ok'
cp /srv/stats.v2.json /srv/stats.json; run refresh
chk "a changed stats.json: the new counts, on the stick too" '[ "$(mf good-mod 21 $R1)" = 1841 ] && [ "$(mf any-mod 21)" = 3 ] && grep -q "${TAB}good-mod${TAB}1841\$" $COMM/.stats && stick_ro'
rwst 'printf "x\ty\tz\n%s\tany-mod\t-1\n%s\tdd-mod\t0012\n" "$(head -1 .stats | cut -f1)" "$(head -1 .stats | cut -f1)" >> .stats'
rm -f $SC; run catalog
chk "a damaged line on the stick is dropped, the good ones read (leading zeros trimmed)" '[ "$(mf good-mod 21 $R1)" = 1841 ] && [ "$(mf any-mod 21)" = 3 ] && [ "$(mf dd-mod 21)" = 12 ] && ! grep -q "^x" $SC'
echo '{"azmods_stats": 2, "mods": {}}' > /srv/stats.json; run refresh
chk "a stats.json of another format is refused, the last counts kept" '[ "$(mf good-mod 21 $R1)" = 1841 ] && clog | grep -q "not azmods_stats 1"'
mv /srv/stats.json.off /srv/stats.json; run refresh
chk "CONTROL: the first stats.json again" '[ "$(mf good-mod 21 $R1)" = 1840 ] && [ "$(mf any-mod 21)" = 0 ] && [ -z "$(mf dd-mod 21)" ]'

echo "== [$SHN] download counts: damaged stats.json (community_statsfuzz.py, fixed seeds)"
# every count in the catalog is digits (at most 13) or nothing; every kept line is "key TAB id TAB count"
counts_ok() { awk -F'|' '$1=="mod"&&$21!=""&&$21!~/^[0-9]+$/{ bad=1 } $1=="mod"&&length($21)>13{ bad=1 } END{ exit bad }' "$CAT" &&
    awk -F "$TAB" 'NF!=3||$1!~/^https:\/\/[!-~]+$/||$2!~/^[a-z0-9-]+$/||$3!~/^[0-9]+$/{ bad=1 } END{ exit bad || NR > 16384 }' "$SC"; }
cp /srv/stats.json /srv/stats.good
FZBAD=0; FZKEPT=0
for seed in $(seq 0 47); do
    python3 /t/community_statsfuzz.py /srv/stats.good /srv/stats.json "$seed"
    run refresh
    if ! { [ "$(st state)" = "done" ] && fields_ok && counts_ok && [ -z "$(mf other-mod 21)" ] && stick_ro && nothing_half; }; then
        FZBAD=$((FZBAD + 1)); echo "  seed $seed: state=$(st state) msg=$(st msg | cut -c1-60)"
    fi
    # the last good counts survive a file that is refused as a whole
    [ "$(mf good-mod 21 $R1)" = 1840 ] && FZKEPT=$((FZKEPT + 1))
    cp /srv/stats.good /srv/stats.json; run refresh
done
chk "48 damaged stats.json: every refresh done, the catalog well formed, each count digits or nothing, never a hand-added repository's" '[ "$FZBAD" = 0 ]'
chk "the refused ones keep the last counts (CONTROL: at least the 16 cut short, nested too deep, over the cap or not the format)" '[ "$FZKEPT" -ge 16 ]'
python3 /t/community_statsfuzz.py /srv/stats.good /srv/stats.json 6; echo "test: over-cap stats.json" >> /deck/tmp/az-community/log; run refresh
chk "a stats.json over 256 KB is refused, the last counts kept" '[ "$(st state)" = done ] && [ "$(mf good-mod 21 $R1)" = 1840 ] && clog | sed -n "/^test: over-cap stats.json/,\$p" | grep -q "download counts: download failed"'
python3 /t/community_statsfuzz.py /srv/stats.good /srv/stats.json 7; run refresh
chk "a stats.json of thousands of entries is read within the bounds, the listed counts still right" '[ "$(st state)" = done ] && counts_ok && [ "$(wc -l < $SC)" -gt 1000 ] && [ "$(mf good-mod 21 $R1)" = 1840 ]'
cp /srv/stats.good /srv/stats.json; rm -f /srv/stats.good; run refresh
chk "CONTROL: the first stats.json again after the damaged ones" '[ "$(mf good-mod 21 $R1)" = 1840 ] && [ "$(mf any-mod 21)" = 0 ] && [ "$(wc -l < $SC)" -lt 10 ] && counts_ok'

echo "== [$SHN] /tmp/az-community stays bounded (RAM)"
I=/deck/tmp/az-community/icons; C=/deck/tmp/az-community/cache
head -c 1000 /dev/zero > $I/0123456789abcdef.png; : > $I/fedcba9876543210.part
echo x > $C/0011223344556677.mods; echo x > $C/0011223344556677.json
run refresh
chk "icons the catalog does not show are deleted, a stale .part too" '[ ! -e $I/0123456789abcdef.png ] && [ ! -e $I/fedcba9876543210.part ]'
# shellcheck disable=SC2034 # read by the eval'd assertions below
GI=/deck$(mf good-mod 12 $R1) OI=/deck$(mf other-mod 12)
chk "CONTROL: the icons the catalog shows are kept" '[ -f "$GI" ] && [ -f "$OI" ]'
chk "no raw index or flat file is kept after a refresh" '! ls $C | grep -qE "[.](json|flat)$"'
chk "a repository no longer in the set loses its cache" '[ ! -e $C/0011223344556677.mods ]'
chk "CONTROL: a current repository keeps its parsed cache" '[ -f $C/$(printf %s "$R1" | sha256sum | cut -c1-16).mods ]'
touch -t 202001010000 "$GI"; ITOT=$(cat $I/* | wc -c); ICAP=$((ITOT - 1)); run refresh; ICAP=
chk "over the icon cap: the oldest icon goes first, the others stay" '[ ! -e "$GI" ] && [ -f "$OI" ] && [ $(cat $I/* | wc -c) -lt $ITOT ]'
chk "the catalog no longer points at the dropped icon" '[ -z "$(mf good-mod 12 $R1)" ]'
run refresh
chk "CONTROL: under the default cap it is fetched again" '[ -f "$GI" ] && [ "$(mf good-mod 12 $R1)" = "${GI#/deck}" ]'

echo "== [$SHN] icons are kept on the stick: after a reboot (/tmp cleared) they are not downloaded again"
# shellcheck disable=SC2034 # read by the eval'd assertions below
SI=$COMM/.icons
run refresh
chk "every icon in /tmp is also on the stick (MODS/community-mods/.icons), same name, same bytes" 'n=0; for f in $I/*.png; do cmp -s "$f" "$SI/${f##*/}" || exit 1; n=$((n + 1)); done; [ $n -gt 0 ]'
chk "the stick is back on its read-only mount after keeping them" 'stick_ro'
rm -rf /deck/tmp/az-community; mv /srv/icons /srv/icons.off; run refresh
# shellcheck disable=SC2034 # read by the eval'd assertions below
GI=/deck$(mf good-mod 12 $R1)
chk "a reboot, then a refresh with every icon URL unreachable: the catalog still shows them" '[ -f "$GI" ] && [ -f "/deck$(mf other-mod 12)" ]'
chk "the icon shown is the stick's copy" 'cmp -s "$GI" "$SI/${GI##*/}"'
rwst 'rm -rf .icons'; rm -rf /deck/tmp/az-community; run refresh
chk "CONTROL: no copies on the stick and the URLs unreachable: no icon" '[ -z "$(mf good-mod 12 $R1)" ] && [ -z "$(mf other-mod 12)" ]'
mv /srv/icons.off /srv/icons; run refresh
chk "CONTROL: the URLs back: downloaded, and kept on the stick again" 'g=$(mf good-mod 12 $R1); [ -n "$g" ] && [ -f "$SI/${g##*/}" ]'
# a new version of a mod: its icon gets a new name (fetched again); the old name leaves both places
OLDI=$(mf good-mod 12 $R1); OLDI=${OLDI##*/}
cp /srv/r1/azmods-repo.json /tmp/r1.json.bak
python3 -c 'import json,sys; p=sys.argv[1]; d=json.load(open(p)); [m.update(version="1.0.1") for m in d["mods"] if m.get("id")=="good-mod" and m.get("version")=="1.0.0"]; json.dump(d, open(p,"w"))' /srv/r1/azmods-repo.json
run refresh
# shellcheck disable=SC2034 # read by the eval'd assertions below
NEWI=$(mf good-mod 12 $R1); NEWI=${NEWI##*/}
chk "a new mod version: its icon has a new name, in /tmp and on the stick; the old name is gone from both" '[ "$(mf good-mod 5 $R1)" = 1.0.1 ] && [ -n "$NEWI" ] && [ "$NEWI" != "$OLDI" ] && [ -f "$I/$NEWI" ] && [ -f "$SI/$NEWI" ] && [ ! -e "$I/$OLDI" ] && [ ! -e "$SI/$OLDI" ]'
cp /tmp/r1.json.bak /srv/r1/azmods-repo.json; run refresh

echo "== [$SHN] bad requests"
run frobnicate; chk "unknown request -> error" '[ "$(st state)" = error ] && st msg | grep -q unknown'
run install "$R1"; chk "install with a missing arg -> usage" '[ "$(st state)" = error ] && st msg | grep -q usage'
run 'install * good-mod'; chk "a glob in a request is not expanded" '[ "$(st state)" = error ] && st msg | grep -q "bad repository"'
# shellcheck disable=SC2086 # the shell is a command and its argument ("/bin/busybox ash")
chroot /deck /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin $SHC /tmp/az-deck/lib/az-community install x good-mod
chk "no stick -> error, nothing written" '[ "$(st state)" = error ]'

echo "== [$SHN] it wrote nowhere else"
chk "p7: nothing but the approved list written by the fetcher" '[ "$(p7_other)" = "$P7_BEFORE" ]'
chk "CONTROL: p7_other sees any other p7 file" 'mkdir -p /deck/home/root/settings/azmod-community; touch /deck/home/root/settings/azmod-community/x; [ "$(p7_other)" != "$P7_BEFORE" ]; r=$?; rm -f /deck/home/root/settings/azmod-community/x; [ $r = 0 ]'
chk "stick: nothing changed outside MODS/community-mods and MODS/azmods.cfg" '[ "$(outside_stick)" = "$OUT0" ]'
chk "rootfs: only our own setup files changed" '[ -z "$(cd /ov/u && find . -type f | grep -v "^\./media/\|^\./home/root/settings" )" ]'
chk "/tmp: only az-community/ (plus the test setup)" '[ -z "$(cd /deck/tmp && ls -A | grep -vx "az-community\|az-deck\|fx")" ]'
# util-linux mount (the deck's /usr/bin/mount) records the stick remount in /run/mount/utab:
# that is mount's own bookkeeping (az-modupdate's remount does the same), not a fetcher write
chk "/run: nothing but mount's own utab" '[ -z "$(cd /deck/run && find . -type f | grep -vx ./mount/utab)" ]'
done

echo "== totals: $PASS passed, $FAILN failed"
