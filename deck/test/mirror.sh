#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# shellcheck shell=busybox
# shellcheck disable=SC2034  # captured values are read inside chk's single-quoted eval strings
# Test suite for az-usbload's MODS/ -> p7 mirror (cached first start):
# each `boot` = EP147 loaded whatever the p7 env says, then az-usbload compares the
# chosen stick's set with it. Run by test/run.sh in its alpine container
# (`test/run.sh mirror`).
# RESTART POLICY: a deck already running mods is never restarted by the loader; a change
# is staged and /run/az-reload-pending says "stick" (staged below). Only a deck whose first
# start was stock (no env), a boot with no mods stick (back to stock at once), MODS/mods.off,
# an uninstall or the user's apply restart.
set -u
. /stub/devflag
apk add -q dosfstools >/dev/null 2>&1 || { echo NO_DOSFSTOOLS; exit 1; }
export PATH=/stub:$PATH AZ_ACTIVE_WAIT=2
. /stub/fake-ep147
for f in az-usbload az-usbpick; do cp /p/usr/sbin/$f /usr/sbin/$f; done; chmod +x /usr/sbin/az-usbload
P7=/home/root/settings/az-usbload; ENVF=$P7/az-preload.env; mkdir -p /m /media/usb/sda1
chk() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fi; }
rm -f /img; dd if=/dev/zero of=/img bs=1M count=64 2>/dev/null; mkfs.vfat /img >/dev/null
lo=$(losetup -f); losetup "$lo" /img; trap 'umount /media/usb/sda1 /m 2>/dev/null; losetup -d $lo 2>/dev/null' EXIT; IFS=: read -r _maj _min < /sys/block/"${lo#/dev/}"/dev; mknod /dev/sda1 b "$_maj" "$_min"; echo "sda 1-1" > /tmp/ports
st() { umount /media/usb/sda1 2>/dev/null; mount /dev/sda1 /m; ( cd /m || exit 1; eval "$1" ); umount /m; mount -o ro /dev/sda1 /media/usb/sda1; }
unplug() { umount /media/usb/sda1 2>/dev/null; mv /dev/sda1 /dev.sda1.off; }
replug() { mv /dev.sda1.off /dev/sda1; mount -o ro /dev/sda1 /media/usb/sda1; }
boot() { rm -f /tmp/systemctl.calls /run/az-reload-pending; devflag; /usr/sbin/az-usbload
         grep -hE "keep |add |update |remove |skip |desired|same as|env written|bytes changed|gate|restart|FAILED|did NOT" /tmp/az-usbload.log | sed 's/^/    /'; }
restarted() { grep -q "restart EP147" /tmp/systemctl.calls 2>/dev/null; }
# a change on a deck already running mods: staged for the Reload, no restart
staged() { ! restarted && [ "$(head -n 1 /run/az-reload-pending 2>/dev/null)" = stick ] && grep -q "EP147 is running mods" /tmp/az-usbload.log; }
# no mods stick at boot on a deck running mods: back to stock now, nothing left pending
tostock() { restarted && [ ! -e /run/az-reload-pending ] && grep -q "no mods stick at boot -> stock now" /tmp/az-usbload.log; }
env_() { cat $ENVF 2>/dev/null; }
p7sum() { (cd $P7 2>/dev/null && find . -type f ! -name usbload.log | sort | xargs md5sum 2>/dev/null) | md5sum; }   # the loader's own capped log rotates by size
imgsum() { md5sum /img | cut -d' ' -f1; }
CORE="LD_PRELOAD=$P7/azmod-core.so:$P7/azstem.so"

echo "== a first boot with a mods stick: cached nothing -> write + restart"; st 'mkdir -p MODS; echo c1 > MODS/azmod-core.so; echo s1 > MODS/azstem.so; echo p1 > MODS/azmod-pads.so'; boot
chk "a env = core:azstem (plugin cached, not listed)" '[ "$(env_)" = "$CORE" ] && [ -e $P7/azmod-pads.so ]'; chk "a restarted" restarted
chk "a RELOADING banner flag was up at restart" 'grep -q banner-flag-present /tmp/systemctl.calls'
chk "a banner flag cleared afterwards" '[ ! -e /run/az-reloading ]'

echo "== b same stick next boot -> NOTHING (no write, no restart)"; c0=$(p7sum); i0=$(imgsum); boot
chk "b no restart" '! restarted'; chk "b p7 untouched" '[ "$c0" = "$(p7sum)" ]'; chk "b stick untouched" '[ "$i0" = "$(imgsum)" ]'

echo "== c same name, new bytes -> staged (a deck running mods is not restarted)"; st 'echo c2 > MODS/azmod-core.so'; c0=$(p7sum); boot
chk "c CONTROL: p7sum detects the cache change" '[ "$c0" != "$(p7sum)" ]'; chk "c staged, no restart" staged; chk "c env unchanged" '[ "$(env_)" = "$CORE" ]'

echo "== d plugin removed -> staged"; st 'rm MODS/azmod-pads.so'; boot
chk "d dropped from cache" '[ ! -e $P7/azmod-pads.so ]'; chk "d staged, no restart" staged

echo "== d2 mods.on present (app default) -> still enabled, nothing changes"; st 'touch MODS/mods.on'; c0=$(p7sum); boot
chk "d2 no restart" '! restarted'; chk "d2 p7 untouched" '[ "$c0" = "$(p7sum)" ]'
echo "== d3 both mods.on and mods.off -> off wins"; st 'touch MODS/mods.off'; boot
chk "d3 env stock" '[ -z "$(env_)" ]'; chk "d3 restarted" restarted
st 'rm -f MODS/mods.off MODS/mods.on'; boot >/dev/null
echo "== e mods.off -> env emptied + restart to stock; cache kept"; st 'touch MODS/mods.off'; boot
chk "e env stock" '[ -z "$(env_)" ]'; chk "e restarted" restarted; chk "e cache kept" '[ -e $P7/azmod-core.so ]'
echo "== e2 mods.off again -> nothing"; c0=$(p7sum); boot
chk "e2 no restart" '! restarted'; chk "e2 p7 untouched" '[ "$c0" = "$(p7sum)" ]'

echo "== f mods.off removed -> modded again"; st 'rm MODS/mods.off'; boot
chk "f env core" '[ "$(env_)" = "$CORE" ]'; chk "f restarted" restarted

echo "== g power-cycle with a no-MODS/ stick (no mods stick) -> stock env written and restarted to stock now; then nothing"; st 'rm -rf MODS'; i0=$(imgsum); boot
chk "g env stock" '[ -z "$(env_)" ]'; chk "g back to stock now" tostock; chk "g stick untouched" '[ "$i0" = "$(imgsum)" ]'
c0=$(p7sum); boot; chk "g2 no restart" '! restarted'; chk "g2 p7 untouched" '[ "$c0" = "$(p7sum)" ]'

echo "== h modded, then NO stick -> stock env written and restarted to stock now (no USB at boot = stock); then nothing"; st 'mkdir -p MODS; echo c2 > MODS/azmod-core.so; echo s1 > MODS/azstem.so'; boot; chk "h1 modded" '[ "$(env_)" = "$CORE" ]'
unplug; boot; chk "h2 env stock" '[ -z "$(env_)" ]'; chk "h2 back to stock now" tostock
c0=$(p7sum); boot; chk "h3 no restart" '! restarted'; chk "h3 p7 untouched" '[ "$c0" = "$(p7sum)" ]'; replug

echo "== i modded, then MODS/ with only ssh files (not a mods stick) -> stock now"; boot; st 'rm -f MODS/*.so; echo "ssh-ed25519 AAAAx k" > MODS/authorized_keys'; boot
chk "i env stock" '[ -z "$(env_)" ]'; chk "i back to stock now" tostock

echo "== i2 AppleDouble sidecars are not mods"; st 'mkdir -p MODS; echo c > MODS/azmod-core.so; printf "\0\5\26\7" > MODS/._azmod-core.so; printf x > MODS/._azmod.so'; boot
chk "i2 env = core only, sidecars not cached" '[ "$(env_)" = "LD_PRELOAD=$P7/azmod-core.so" ] && ! ls -a $P7 | grep -q "^\._"'
st 'rm -f MODS/azmod-core.so MODS/._*'
echo "== j legacy azmod.so + azstem.so"; st 'echo L > MODS/azmod.so; echo s > MODS/azstem.so'; boot
chk "j env = azmod.so:azstem.so" '[ "$(env_)" = "LD_PRELOAD=$P7/azmod.so:$P7/azstem.so" ]'

echo "== k core AND legacy -> core wins"; st 'echo c > MODS/azmod-core.so'; boot
chk "k env = core:azstem" '[ "$(env_)" = "$CORE" ]'

echo "== l azstem.so with no core -> none"; st 'rm -f MODS/*.so; echo s > MODS/azstem.so'; boot
chk "l env stock" '[ -z "$(env_)" ]'

echo "== m pngpatch changed -> staged"; st 'echo c > MODS/azmod-core.so; mkdir -p MODS/azmod-pngpatch; echo p > MODS/azmod-pngpatch/0.bin'; boot; boot
st 'echo q > MODS/azmod-pngpatch/0.bin'; boot
chk "m updated + staged" '[ "$(cat $P7/azmod-pngpatch/0.bin)" = q ] && staged'
echo "== n pngpatch unchanged -> nothing"; boot; chk "n no restart" '! restarted'
echo "== o pngpatch removed -> staged"; st 'rm -rf MODS/azmod-pngpatch'; boot
chk "o removed + staged" '[ ! -e $P7/azmod-pngpatch ] && staged'

echo "== r failed restart (a deck whose first start was stock) -> marker; next same-set boot clears it"; st 'echo c3 > MODS/azmod-core.so'; rm -f $ENVF; touch /tmp/stub.inactive; boot; rm -f /tmp/stub.inactive
chk "r marker written" '[ -e /media/usb/sda1/MODS/.pending-powercycle ]'; chk "r mount back to ro" 'grep " /media/usb/sda1 " /proc/mounts | grep -q "[ ,]ro[ ,]"'
boot; chk "r marker cleared, no restart" '[ ! -e /media/usb/sda1/MODS/.pending-powercycle ] && ! restarted'

echo "== s gate never satisfied (first start stock) -> env written, NO restart"; st 'echo c4 > MODS/azmod-core.so'; rm -f $ENVF; echo starting > /tmp/stub.sysstate; AZ_GATE_MAX=1 boot; rm -f /tmp/stub.sysstate
chk "s no restart" '! restarted'; chk "s env written for next boot" '[ "$(env_)" = "$CORE" ]' ; chk "s logged" 'grep -q "gate NOT satisfied" /tmp/az-usbload.log'

echo "== t EP147 failed state -> restart without the gate"; st 'echo c5 > MODS/azmod-core.so'; echo starting > /tmp/stub.sysstate; touch /tmp/stub.failed; AZ_GATE_MAX=30 boot; rm -f /tmp/stub.sysstate /tmp/stub.failed
chk "t restarted immediately" 'restarted && grep -q "failed state" /tmp/az-usbload.log && ! grep -q "gate NOT" /tmp/az-usbload.log'

echo "== v1 .deck-info: the deck tells the stick which firmware it is"
echo 1.32 > /etc/az-mods-release
st 'rm -f MODS/.deck-info'; boot
chk "v1 wrote .deck-info" '[ -f /media/usb/sda1/MODS/.deck-info ]'
chk "v1 names the firmware" 'grep -q "^firmware=1.32$" /media/usb/sda1/MODS/.deck-info'

echo "== v2 written even with mods OFF (the app still needs to know the firmware)"
st 'rm -f MODS/.deck-info; touch MODS/mods.off'; boot
chk "v2 wrote .deck-info with mods off" 'grep -q "^firmware=1.32$" /media/usb/sda1/MODS/.deck-info'
st 'rm -f MODS/mods.off'

echo "== v3 no version file on the deck -> says unknown, never lies"
rm -f /etc/az-mods-release
st 'rm -f MODS/.deck-info'; boot
chk "v3 firmware=unknown" 'grep -q "^firmware=unknown$" /media/usb/sda1/MODS/.deck-info'
echo 1.32 > /etc/az-mods-release
echo "== v4 fwtag= from core's loaded file when it is already there at boot; none without it"
chk "v4 CONTROL: no loaded file -> no fwtag line" '! grep -q "^fwtag=" /media/usb/sda1/MODS/.deck-info'
mkdir -p /tmp/azmod-community; printf 'fw=ep147-00112233aabbccdd\nsuspended=0\n' > /tmp/azmod-community/loaded; boot >/dev/null
chk "v4 fwtag line written" 'grep -qx "fwtag=ep147-00112233aabbccdd" /media/usb/sda1/MODS/.deck-info'
echo "== v5 a boot before core has written the file keeps the stick's line (no churn)"
rm -f /tmp/azmod-community/loaded; i0=$(imgsum); boot >/dev/null
chk "v5 line kept, stick untouched" 'grep -qx "fwtag=ep147-00112233aabbccdd" /media/usb/sda1/MODS/.deck-info && [ "$i0" = "$(imgsum)" ]'

# ---- MODS/community-mods/: mirrored like azmod-pngpatch/ ---------------------------------------
CS=$P7/community
# baseline: case t trips the crash-loop guard on purpose and marks that .so set, so every
# later boot of the same set stays stock (by design). New core bytes = a new set.
st 'rm -f MODS/mods.off MODS/.mods-disabled-crashloop; echo cw > MODS/azmod-core.so; echo s > MODS/azstem.so'; boot >/dev/null
chk "w0 baseline: modded" '[ "$(env_)" = "$CORE" ]'
echo "== w1 community/ added -> mirrored to the stage, staged for the Reload"
st 'mkdir -p MODS/community-mods/x-mod; echo m1 > MODS/community-mods/x-mod/x-mod.so; echo "{}" > MODS/community-mods/x-mod/azmod.json; echo x-mod > MODS/community-mods/enabled'; boot
chk "w1 stage has the plugin + enabled" '[ "$(cat $CS/x-mod/x-mod.so)" = m1 ] && [ "$(cat $CS/enabled)" = x-mod ]'; chk "w1 staged, no restart" staged
echo "== w2 unchanged -> nothing"; c0=$(p7sum); boot
chk "w2 no restart" '! restarted'; chk "w2 p7 untouched" '[ "$c0" = "$(p7sum)" ]'
echo "== w3 AppleDouble + the fetcher's transient .<id>.new -> not copied, not a change"
st 'printf "\0\5\26\7" > MODS/community-mods/x-mod/._x-mod.so; mkdir -p MODS/community-mods/.y-mod.new; echo half > MODS/community-mods/.y-mod.new/y-mod.so'; c0=$(p7sum); boot
chk "w3 no restart" '! restarted'; chk "w3 p7 untouched" '[ "$c0" = "$(p7sum)" ]'
echo "== w4 CONTROL for w3: a real byte change in the same tree -> staged, and the junk is still not copied"
st 'echo m2 > MODS/community-mods/x-mod/x-mod.so'; boot
chk "w4 updated + staged" '[ "$(cat $CS/x-mod/x-mod.so)" = m2 ] && staged'
chk "w4 no ._ file and no .y-mod.new in the stage" '[ ! -e $CS/x-mod/._x-mod.so ] && [ ! -e $CS/.y-mod.new ]'
echo "== w4b a mod's assets/ stays on the stick: not copied, not a change"
st 'mkdir -p MODS/community-mods/x-mod/assets; echo sample > MODS/community-mods/x-mod/assets/kick.wav'; c0=$(p7sum); boot
chk "w4b no restart, p7 untouched, no assets/ in the stage" '! restarted && [ "$c0" = "$(p7sum)" ] && [ ! -e $CS/x-mod/assets ]'
st 'echo m2b > MODS/community-mods/x-mod/x-mod.so'; boot
chk "w4c CONTROL: a code change beside assets/ mirrors, still without assets/" '[ "$(cat $CS/x-mod/x-mod.so)" = m2b ] && staged && [ ! -e $CS/x-mod/assets ]'
st 'echo m2 > MODS/community-mods/x-mod/x-mod.so; rm -rf MODS/community-mods/x-mod/assets'; boot >/dev/null
echo "== w4d a mod's data/ (its own files, with the owner stamp and a set-aside copy) stays on the stick: not copied, not a change"
st 'mkdir -p MODS/community-mods/x-mod/data MODS/community-mods/x-mod/.data@0123456789abcdef; echo save > MODS/community-mods/x-mod/data/save.txt; echo local > MODS/community-mods/x-mod/.data.owner; echo old > MODS/community-mods/x-mod/.data@0123456789abcdef/save.txt'; c0=$(p7sum); boot
chk "w4d no restart, p7 untouched, no data/ in the stage" '! restarted && [ "$c0" = "$(p7sum)" ] && [ ! -e $CS/x-mod/data ] && [ ! -e $CS/x-mod/.data.owner ] && ! ls -a $CS/x-mod | grep -q "^\.data@"'
st 'echo m2d > MODS/community-mods/x-mod/x-mod.so'; boot
chk "w4e CONTROL: a code change beside data/ mirrors, still without data/" '[ "$(cat $CS/x-mod/x-mod.so)" = m2d ] && staged && [ ! -e $CS/x-mod/data ]'
st 'echo m2 > MODS/community-mods/x-mod/x-mod.so; rm -rf MODS/community-mods/x-mod/data MODS/community-mods/x-mod/.data*'; boot >/dev/null
echo "== w5 community-mods.off -> the stage's community/ removed, marker written, staged"
st 'touch MODS/community-mods.off'; boot
chk "w5 stage community/ gone, community-mods.off marker" '[ ! -e $CS ] && [ -e $P7/community-mods.off ]'; chk "w5 staged, no restart" staged
chk "w5 official mods untouched" '[ "$(env_)" = "$CORE" ]'
c0=$(p7sum); boot; chk "w5b again -> nothing" '! restarted && [ "$c0" = "$(p7sum)" ]'
echo "== w6 community-mods.off removed -> mirrored back, marker gone, staged"
st 'rm -f MODS/community-mods.off'; boot
chk "w6 back" '[ "$(cat $CS/x-mod/x-mod.so)" = m2 ] && [ ! -e $P7/community-mods.off ] && staged'
echo "== w7 p7 reserve would be breached -> NOT mirrored, the old stage copy stays"
st 'echo m3 > MODS/community-mods/x-mod/x-mod.so'; AZ_COMMUNITY_P7_RESERVE_KB=999999999 boot
chk "w7 old copy kept, logged" '[ "$(cat $CS/x-mod/x-mod.so)" = m2 ] && grep -q "FAILED community/: needs" /tmp/az-usbload.log && [ ! -e $P7/community.new ]'
boot; chk "w7b CONTROL: with the default reserve it mirrors" '[ "$(cat $CS/x-mod/x-mod.so)" = m3 ] && staged'
echo "== w8 community/ removed from the stick -> removed from the stage, staged"
st 'rm -rf MODS/community-mods'; boot
chk "w8 gone + staged" '[ ! -e $CS ] && staged'
echo "== w9 mods.off -> the community stage copy is kept like the rest of the cache"
st 'mkdir -p MODS/community-mods/x-mod; echo m4 > MODS/community-mods/x-mod/x-mod.so'; boot; st 'touch MODS/mods.off'; boot
chk "w9 env stock, community cache kept" '[ -z "$(env_)" ] && [ "$(cat $CS/x-mod/x-mod.so)" = m4 ]'
st 'rm -f MODS/mods.off'

# ---- the community quarantine counter (core: settings/azmod-community/.starting) ----------
QS=/home/root/settings/azmod-community/.starting; mkdir -p ${QS%/*}
echo "== y1 the loader's own restart clears .starting (a deliberate restart is not a crash)"
echo 1 > $QS; st 'echo m5 > MODS/community-mods/x-mod/x-mod.so'; export AZ_FORCE_RELOAD=1; boot; unset AZ_FORCE_RELOAD
chk "y1 restarted (the user's apply), .starting removed" 'restarted && [ ! -e $QS ]'
echo "== y2 CONTROL: a boot with no restart leaves .starting alone"
echo 1 > $QS; boot; chk "y2 no restart, .starting kept" '! restarted && [ "$(cat $QS)" = 1 ]'
echo "== y3 CONTROL: the crash-loop guard's restart (a crash response) keeps .starting"
st 'echo c7 > MODS/azmod-core.so'; touch /tmp/stub.failed; boot; rm -f /tmp/stub.failed
chk "y3 guard restarted, .starting kept" 'restarted && grep -q "crash-loop guard" /tmp/az-usbload.log && [ "$(cat $QS)" = 1 ]'
st 'rm -f MODS/.mods-disabled-crashloop; echo cw3 > MODS/azmod-core.so'; boot >/dev/null; rm -f $QS

# ---- assets/ must never reach p7, not even for a moment ----------------------------------
# A 20 MB stage, two mods of 0.5 MB code + 12 MB assets/ each, a 4 MB reserve: the code fits
# with room to spare, a copy that took assets/ first and pruned after does not.
cp -a $P7 /p7.save; mount -t tmpfs -o size=20m p7t $P7; cp -a /p7.save/. $P7/
echo "== w10 code fits, assets/ would not: mirrored, no assets/ in the stage, nothing FAILED"
st 'rm -rf MODS/community-mods; for m in a-mod b-mod; do mkdir -p MODS/community-mods/$m/assets; head -c 524288 /dev/urandom > MODS/community-mods/$m/$m.so; head -c 12582912 /dev/zero > MODS/community-mods/$m/assets/big.bin; done; echo a-mod > MODS/community-mods/enabled'
AZ_COMMUNITY_P7_RESERVE_KB=4096 boot
chk "w10 both mods mirrored without assets/, staged" '[ -s $CS/a-mod/a-mod.so ] && [ -s $CS/b-mod/b-mod.so ] && [ ! -e $CS/a-mod/assets ] && [ ! -e $CS/b-mod/assets ] && staged && ! grep -q FAILED /tmp/az-usbload.log'
chk "w10 the stage copy verifies against the stick" 'cmp -s $CS/b-mod/b-mod.so /media/usb/sda1/MODS/community-mods/b-mod/b-mod.so && [ ! -e $P7/community.new ]'
echo "== w11 CONTROL: a tree that genuinely does not fit the reserve is still refused"
st 'rm -rf MODS/community-mods/*/assets; head -c 16777216 /dev/zero > MODS/community-mods/a-mod/big.dat'
AZ_COMMUNITY_P7_RESERVE_KB=4096 boot
chk "w11 refused by the reserve, old copy kept, nothing half-copied" 'grep -q "FAILED community/: needs" /tmp/az-usbload.log && [ ! -e $CS/a-mod/big.dat ] && [ -s $CS/a-mod/a-mod.so ] && [ ! -e $P7/community.new ]'
echo "== f1/f2 Reload now with an unchanged stage (a settings-only change) still restarts"
st 'rm -f MODS/community-mods/a-mod/big.dat'; boot >/dev/null; boot
chk "f1 CONTROL: a boot with nothing changed does not restart" 'grep -q "same as loaded" /tmp/az-usbload.log && ! restarted'
export AZ_FORCE_RELOAD=1; boot; unset AZ_FORCE_RELOAD   # exported: VAR=x func does not reach a child in ash
chk "f2 an explicit apply restarts EP147 though the stage is the same" '! grep -q "same as loaded" /tmp/az-usbload.log && restarted'   # the loader starts its log fresh each boot
restarted || tail -25 /tmp/az-usbload.log | sed 's/^/    f2 log: /'
umount $P7; rm -rf /p7.save
