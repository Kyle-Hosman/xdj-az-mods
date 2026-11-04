#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# shellcheck shell=busybox
# shellcheck disable=SC2034  # captured values are read inside chk's single-quoted eval strings
# Test suite for az-usbload's RESTART POLICY: EP147 is never restarted by the loader over
# a loaded track, and never at all on a deck already running mods (r13-r15: the set is
# staged and the Mod Menu says Reload to apply). A fresh install's first boot may restart
# early in boot; anything later needs proof that every deck is empty (azstem's
# /tmp/az-decks); a mods update never restarts.
# Run by test/run.sh in its alpine container (`test/run.sh restart`).
#
# Every "does not restart" case has a twin in the same run that differs in one input and
# does restart, so the instrument (the systemctl stub's call log) is shown able to see a
# restart: r1 for r2/r7, r3c for r3/r4/r5/r6, r10c for r10, r13c for r13/r14, r16 for r13 and
# r16c (no stick vs a stick, the boot's pass vs a re-run); rc2 removes the running-mods rule
# and r13's input then restarts; rc3 removes the no-stick rule and r16's input then holds.
set -u
. /stub/devflag
apk add -q dosfstools >/dev/null 2>&1 || { echo NO_DOSFSTOOLS; exit 1; }
export PATH=/stub:$PATH AZ_ACTIVE_WAIT=2 AZ_EP147_SETTLE=0 AZ_RENOTIFY_DELAY1=0 AZ_RENOTIFY_DELAY2=0
export AZ_DEFER_POLL=1 AZ_DECKS_SETTLE=1
for f in az-usbload az-usbpick; do cp /p/usr/sbin/$f /usr/sbin/$f; done; chmod +x /usr/sbin/az-usbload
mkdir -p /home/root/scripts; cp /p/home/root/scripts/usb-renotify.sh /home/root/scripts/
P7=/home/root/settings/az-usbload; mkdir -p /m /media/usb/sda1 /run
chk() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fi; }
rm -f /img; dd if=/dev/zero of=/img bs=1M count=16 2>/dev/null; mkfs.vfat /img >/dev/null
mount /img /m; mkdir /m/MODS; echo core > /m/MODS/azmod-core.so; echo audio > /m/t.mp3; echo db > /m/export.db; umount /m
lo=$(losetup -f); losetup "$lo" /img; IFS=: read -r _maj _min < /sys/block/"${lo#/dev/}"/dev; mknod /dev/sda1 b "$_maj" "$_min"; echo "sda 1-1" > /tmp/ports
mount -o ro /dev/sda1 /media/usb/sda1

# a fake EP147 (comm HuiProcessor, so the gate passes), optionally holding a file open
ep147() { if [ -n "${1:-}" ]; then sh -c "exec 3<$1; echo -n HuiProcessor > /proc/self/comm; while :; do sleep 1; done" &
          else sh -c 'echo -n HuiProcessor > /proc/self/comm; while :; do sleep 1; done' & fi
          echo $! > /tmp/stub.ep147pid; sleep 0.2; }
up() { cut -d' ' -f1 /proc/uptime; }
# what azstem publishes: mark <decks> [pid] [uptime]
mark() { printf 'azdecks 1\npid %s\nup %s\ndecks %s\n' "${2:-$(cat /tmp/stub.ep147pid)}" "${3:-$(up)}" "$1" > /tmp/az-decks.new && mv /tmp/az-decks.new /tmp/az-decks; }
# azstem keeps the file fresh: rewrite it every 0.5 s from /tmp/mark.state, for the current pid
marker_loop() { ( while :; do mark "$(cat /tmp/mark.state)"; sleep 0.5; done ) & ML=$!; }
reset() { kill ${ML:+"$ML"} 2>/dev/null; ML=
          while IFS= read -r p; do kill "$p" 2>/dev/null; done < /tmp/stub.ep147pid 2>/dev/null
          pkill -f 'HuiProcessor' 2>/dev/null
          rm -rf $P7 /tmp/az-usbload.log /tmp/systemctl.calls /tmp/stub.* /tmp/az-decks /tmp/mark.state /run/az-reload-pending /run/az-reloading /run/az-usbload.*
          mkdir -p /var; echo -n up > /var/net_mlan0_state.dat; }
boot() { devflag; timeout 120 /usr/sbin/az-usbload >/dev/null 2>&1; grep -hE "restart policy|NOT now|deferred|staged|gate NOT|daemon-reload \+ restart" /tmp/az-usbload.log | sed 's/^/    /'; }
restarted() { grep -q "restart EP147.service" /tmp/systemctl.calls 2>/dev/null; }
lg() { grep -q "$1" /tmp/az-usbload.log; }

echo "== r1 CONTROL: boot window, stock EP147 (no decks file) -> the boot-time restart happens"
reset; ep147
AZ_BOOT_WINDOW=1000000000 boot
chk "r1 restarted" restarted
chk "r1 logged as the early-boot restart" 'lg "restart policy: early in boot .*decks unknown -> restarting"'
chk "r1 nothing left pending" '[ ! -e /run/az-reload-pending ]'

echo "== r2 boot window, azstem says a track is loaded -> NO restart; once every deck is empty -> restart"
reset; ep147; echo LEEE > /tmp/mark.state; marker_loop
( sleep 4; echo EEEE > /tmp/mark.state ) &
AZ_BOOT_WINDOW=1000000000 AZ_DEFER_MAX=30 boot
chk "r2 held back while loaded" 'lg "restart wanted but NOT now: a track is loaded"'
chk "r2 then restarted once every deck was empty" 'restarted && lg "deferred restart: every deck is empty -> applying"'
chk "r2 exactly one restart" '[ "$(grep -c "restart EP147.service" /tmp/systemctl.calls)" = 1 ]'
chk "r2 the restart came AFTER the decks emptied (never while loaded)" '[ "$(grep -n "NOT now" /tmp/az-usbload.log | cut -d: -f1)" -lt "$(grep -n "daemon-reload + restart" /tmp/az-usbload.log | cut -d: -f1)" ]'
chk "r2 pending cleared by the restart" '[ ! -e /run/az-reload-pending ]'

echo "== r3 after the boot window, no decks file (stock EP147 / unknown) -> NO restart, ever"
reset; ep147
AZ_BOOT_WINDOW=0 AZ_DEFER_MAX=4 boot
chk "r3 not restarted" '! restarted'
chk "r3 held back as unknown" 'lg "NOT now: decks unknown: no proof every deck is empty"'
chk "r3 pending marker says a stick change waits" '[ "$(head -n 1 /run/az-reload-pending)" = stick ]'
chk "r3 env written for the next boot" 'grep -q "LD_PRELOAD=$P7/azmod-core.so" $P7/az-preload.env'

echo "== r3c CONTROL: after the boot window, every deck proven empty -> restarts"
reset; ep147; echo EEEE > /tmp/mark.state; marker_loop
AZ_BOOT_WINDOW=0 AZ_DEFER_MAX=30 boot
chk "r3c restarted on proof" 'restarted && lg "restart policy: every deck is empty -> restarting"'

echo "== r4 a STALE decks file (azstem stopped writing it) is not proof"
reset; ep147; mark EEEE "" "$(awk -v u="$(up)" 'BEGIN{printf "%.1f", u-60}')"
AZ_BOOT_WINDOW=0 AZ_DEFER_MAX=3 boot
chk "r4 not restarted" '! restarted && lg "decks unknown"'

echo "== r5 a decks file from ANOTHER EP147 (pid) is not proof"
reset; ep147; mark EEEE 99999
AZ_BOOT_WINDOW=0 AZ_DEFER_MAX=3 boot
chk "r5 not restarted" '! restarted && lg "decks unknown"'

echo "== r6 a deck azstem cannot read (U) is not empty"
reset; ep147; echo EEUE > /tmp/mark.state; marker_loop
AZ_BOOT_WINDOW=0 AZ_DEFER_MAX=3 boot
chk "r6 not restarted" '! restarted && lg "decks unknown"'

echo "== r7 boot window, EP147 holds an audio file open on the stick -> loaded, NO restart"
reset; ep147 /media/usb/sda1/t.mp3
AZ_BOOT_WINDOW=1000000000 AZ_DEFER_MAX=3 boot
chk "r7 not restarted" '! restarted && lg "NOT now: a track is loaded"'
echo "== r7b CONTROL: the library database open (not audio) does not count as a loaded track"
reset; ep147 /media/usb/sda1/export.db
AZ_BOOT_WINDOW=1000000000 boot
chk "r7b restarted" restarted

echo "== r8 waiting, then EP147 restarts WITHOUT us (the Mod Menu's Reload) -> the wait ends, no second restart"
reset; ep147; echo LEEE > /tmp/mark.state; marker_loop
( sleep 3; kill "$(cat /tmp/stub.ep147pid)"; sh -c 'echo -n HuiProcessor > /proc/self/comm; while :; do sleep 1; done' & echo $! > /tmp/stub.ep147pid ) &
AZ_BOOT_WINDOW=1000000000 AZ_DEFER_MAX=30 boot
chk "r8 the loader never restarted" '! restarted'
chk "r8 the wait ended on the new pid" 'lg "deferred restart: EP147 restarted without us"'
chk "r8 pending cleared" '[ ! -e /run/az-reload-pending ]'

echo "== r9 the decks empty only for a moment (a song change) -> NO restart"
reset; ep147; echo LEEE > /tmp/mark.state; marker_loop
# empty for 1.6 s (longer than the 1 s poll, so it is seen; shorter than the 3 s settle)
( sleep 3; echo EEEE > /tmp/mark.state; sleep 1.6; echo ELEE > /tmp/mark.state ) &
AZ_BOOT_WINDOW=1000000000 AZ_DECKS_SETTLE=3 AZ_DEFER_MAX=12 boot
chk "r9 not restarted" '! restarted'
chk "r9 the emptiness was seen and did not hold" 'lg "the decks were empty only briefly"'

echo "== r10 a MODS UPDATE re-run (AZ_RERUN, no community apply) -> staged, NO restart, pending=update"
reset; ep147; echo EEEE > /tmp/mark.state; marker_loop
AZ_RERUN=1 AZ_BOOT_WINDOW=1000000000 boot
chk "r10 not restarted (even with every deck empty and inside the boot window)" '! restarted'
chk "r10 staged + logged" 'lg "mods update staged" && grep -q "LD_PRELOAD=$P7/azmod-core.so" $P7/az-preload.env'
chk "r10 pending says update" '[ "$(head -n 1 /run/az-reload-pending)" = update ]'
echo "== r10c CONTROL: the same re-run as a community apply (the user's Reload now) restarts"
reset; ep147; echo LEEE > /tmp/mark.state; marker_loop
AZ_RERUN=1 AZ_FORCE_RELOAD=1 AZ_BOOT_WINDOW=0 boot
chk "r10c restarted (a user action, even with a track loaded)" 'restarted && lg "a community apply"'

echo "== r11 --post-start (any EP147 start after the loader ran) clears the pending marker"
reset; touch /run/az-usbload.booted; pending() { printf 'update\nx\n' > /run/az-reload-pending; }; pending
AZ_ACTIVE_WAIT=1 /usr/sbin/az-usbload --post-start
chk "r11 cleared" '[ ! -e /run/az-reload-pending ]'
pending; rm -f /run/az-usbload.booted; /usr/sbin/az-usbload --post-start
chk "r11b CONTROL: the boot's first start (before the loader) leaves it" '[ -e /run/az-reload-pending ]'

echo "== r12 a track loaded during the RELOADING banner second stops the restart"
reset; ep147; echo EEEE > /tmp/mark.state; marker_loop
( i=0; while [ ! -e /run/az-reloading ] && [ $i -lt 300 ]; do sleep 0.1; i=$((i+1)); done; echo LEEE > /tmp/mark.state ) &
AZ_BOOT_WINDOW=1000000000 AZ_DEFER_MAX=4 boot
chk "r12 not restarted" '! restarted && lg "a track was loaded just before the restart"'
chk "r12 banner flag removed" '[ ! -e /run/az-reloading ]'

echo "== r13 a deck ALREADY RUNNING mods, the stick brings a different set -> staged, NO restart, pending=stick"
# EP147's first start loaded an older set (the env names it); every deck empty, inside the
# boot window: the only thing that may stop a restart now is that mods are already running.
reset; ep147; echo EEEE > /tmp/mark.state; marker_loop
mkdir -p $P7; echo old > $P7/azmod-old.so; echo "LD_PRELOAD=$P7/azmod-old.so" > $P7/az-preload.env
AZ_BOOT_WINDOW=1000000000 boot
chk "r13 not restarted" '! restarted'
chk "r13 logged: running mods, staged" 'lg "restart policy: EP147 is running mods .* -> NOT restarting"'
chk "r13 pending says stick (the Mod Menu Reload row shows Reload to apply)" '[ "$(head -n 1 /run/az-reload-pending)" = stick ]'
chk "r13 the env names the stick's set for the Reload / next boot" 'grep -q "LD_PRELOAD=$P7/azmod-core.so" $P7/az-preload.env'
chk "r13 the new .so is staged" '[ "$(cat $P7/azmod-core.so)" = core ]'
chk "r13 no deferred-restart watcher (nothing restarts later by itself)" '! lg "deferred restart"'
echo "== r13c CONTROL: the same inputs on a FRESH install (no set was running) -> the one early restart"
reset; ep147; echo EEEE > /tmp/mark.state; marker_loop
AZ_BOOT_WINDOW=1000000000 boot
chk "r13c restarted" 'restarted && lg "restart policy: early in boot"'

echo "== r14 same set, but the stick's .so BYTES changed (an update copied by the app) -> staged, NO restart"
reset; ep147; echo EEEE > /tmp/mark.state; marker_loop
mkdir -p $P7; echo older-bytes > $P7/azmod-core.so; echo "LD_PRELOAD=$P7/azmod-core.so" > $P7/az-preload.env
AZ_BOOT_WINDOW=1000000000 boot
chk "r14 not restarted" '! restarted && lg "EP147 is running mods"'
chk "r14 the new bytes are staged" '[ "$(cat $P7/azmod-core.so)" = core ]'
chk "r14 pending says stick" '[ "$(head -n 1 /run/az-reload-pending)" = stick ]'

echo "== r15 a deck running mods, the user's community apply (AZ_FORCE_RELOAD) -> restarts (the user asked)"
reset; ep147; echo EEEE > /tmp/mark.state; marker_loop
mkdir -p $P7; echo old > $P7/azmod-old.so; echo "LD_PRELOAD=$P7/azmod-old.so" > $P7/az-preload.env
AZ_RERUN=1 AZ_FORCE_RELOAD=1 AZ_BOOT_WINDOW=0 boot
chk "r15 restarted" 'restarted && lg "a community apply"'

echo "== r16 a deck running mods boots with NO mods stick, every deck empty -> back to stock now"
# r13's state (an older set running), only the stick is gone: no USB at boot means stock.
reset; ep147; echo EEEE > /tmp/mark.state; marker_loop
mkdir -p $P7; echo old > $P7/azmod-old.so; echo "LD_PRELOAD=$P7/azmod-old.so" > $P7/az-preload.env
umount /media/usb/sda1; mv /dev/sda1 /dev.sda1.off
AZ_BOOT_WINDOW=1000000000 boot
chk "r16 restarted" restarted
chk "r16 logged: no mods stick at boot" 'lg "restart policy: no mods stick at boot -> stock now"'
chk "r16 the env is stock" '[ -z "$(cat $P7/az-preload.env)" ]'
chk "r16 nothing left pending" '[ ! -e /run/az-reload-pending ]'
echo "== r16b the same with a track loaded -> NO restart while it is loaded; once every deck is empty -> stock"
reset; ep147; echo LEEE > /tmp/mark.state; marker_loop
mkdir -p $P7; echo old > $P7/azmod-old.so; echo "LD_PRELOAD=$P7/azmod-old.so" > $P7/az-preload.env
# the decks empty only once the loader has held back (the no-stick pick alone waits ~10 s)
( i=0; until grep -q "NOT now" /tmp/az-usbload.log 2>/dev/null || [ $i -ge 600 ]; do sleep 0.2; i=$((i + 1)); done; sleep 1; echo EEEE > /tmp/mark.state ) &
AZ_BOOT_WINDOW=1000000000 AZ_DEFER_MAX=30 boot
chk "r16b held back while loaded" 'lg "restart wanted but NOT now: a track is loaded"'
chk "r16b then restarted once every deck was empty" 'restarted && lg "deferred restart: every deck is empty -> applying"'
chk "r16b the restart came AFTER the decks emptied" '[ "$(grep -n "NOT now" /tmp/az-usbload.log | cut -d: -f1)" -lt "$(grep -n "daemon-reload + restart" /tmp/az-usbload.log | cut -d: -f1)" ]'
echo "== r16c a mods update re-run with no stick on a deck running mods -> staged, NO restart (only the boot's own pass)"
reset; ep147; echo EEEE > /tmp/mark.state; marker_loop
mkdir -p $P7; echo old > $P7/azmod-old.so; echo "LD_PRELOAD=$P7/azmod-old.so" > $P7/az-preload.env
AZ_RERUN=1 AZ_BOOT_WINDOW=1000000000 boot
chk "r16c not restarted" '! restarted && ! lg "no mods stick at boot"'
mv /dev.sda1.off /dev/sda1; mount -o ro /dev/sda1 /media/usb/sda1

echo "== rc2 CONTROL: the loader with the running-mods rule cut out restarts r13's input"
cp /usr/sbin/az-usbload /tmp/az-usbload.rule
sed -e 's/^elif \[ -n "\$HAVE" \] && /elif false \&\& /' /tmp/az-usbload.rule > /tmp/az-usbload.norule
chk "rc2 the mutation changed exactly the rule line" '[ "$(diff /tmp/az-usbload.rule /tmp/az-usbload.norule | grep -c "^+[^+]")" = 1 ] && sh -n /tmp/az-usbload.norule'
cp /tmp/az-usbload.norule /usr/sbin/az-usbload; chmod +x /usr/sbin/az-usbload
reset; ep147; echo EEEE > /tmp/mark.state; marker_loop
mkdir -p $P7; echo old > $P7/azmod-old.so; echo "LD_PRELOAD=$P7/azmod-old.so" > $P7/az-preload.env
AZ_BOOT_WINDOW=1000000000 boot
chk "rc2 r13's input: restarts a deck already running mods" restarted
cp /tmp/az-usbload.rule /usr/sbin/az-usbload; chmod +x /usr/sbin/az-usbload

echo "== rc3 CONTROL: the loader with the no-stick rule cut out holds r16's input (staged, no restart)"
sed -e 's/^\[ -z "\$MNT" \] && \[ -z "\${AZ_RERUN:-}" \] && NOSTICK=1$/: no-stick rule cut/' /tmp/az-usbload.rule > /tmp/az-usbload.nostick
chk "rc3 the mutation changed exactly the rule line" '[ "$(diff /tmp/az-usbload.rule /tmp/az-usbload.nostick | grep -c "^+[^+]")" = 1 ] && sh -n /tmp/az-usbload.nostick'
cp /tmp/az-usbload.nostick /usr/sbin/az-usbload; chmod +x /usr/sbin/az-usbload
reset; ep147; echo EEEE > /tmp/mark.state; marker_loop
mkdir -p $P7; echo old > $P7/azmod-old.so; echo "LD_PRELOAD=$P7/azmod-old.so" > $P7/az-preload.env
umount /media/usb/sda1; mv /dev/sda1 /dev.sda1.off
AZ_BOOT_WINDOW=1000000000 boot
chk "rc3 r16's input: not restarted (the old hold)" '! restarted && lg "EP147 is running mods"'
mv /dev.sda1.off /dev/sda1; mount -o ro /dev/sda1 /media/usb/sda1
cp /tmp/az-usbload.rule /usr/sbin/az-usbload; chmod +x /usr/sbin/az-usbload

echo "== rc CONTROL: the same inputs against the loader with the policy cut out DO restart, so r2/r3/r10 can fail"
cp /usr/sbin/az-usbload /tmp/az-usbload.policy
sed -e 's/^elif may_restart boot; then$/elif true; then/' \
    -e 's/^if \[ -n "\${AZ_RERUN:-}" \] && \[ -z "\${AZ_FORCE_RELOAD:-}" \]; then$/if false; then/' \
    -e 's/\[ -z "\${AZ_FORCE_RELOAD:-}" \] && \[ "\$(decks_state)" = loaded \]/false/' /tmp/az-usbload.policy > /tmp/az-usbload.nopolicy
chk "rc the mutation changed exactly the three policy lines" '[ "$(diff /tmp/az-usbload.policy /tmp/az-usbload.nopolicy | grep -c "^+[^+]")" = 3 ] && sh -n /tmp/az-usbload.nopolicy'
cp /tmp/az-usbload.nopolicy /usr/sbin/az-usbload; chmod +x /usr/sbin/az-usbload
reset; ep147; echo LEEE > /tmp/mark.state; marker_loop
AZ_BOOT_WINDOW=1000000000 AZ_DEFER_MAX=3 boot
chk "rc r2's input: restarts over a loaded track" restarted
reset; ep147
AZ_BOOT_WINDOW=0 AZ_DEFER_MAX=3 boot
chk "rc r3's input: restarts long after boot with no proof" restarted
reset; ep147; echo LEEE > /tmp/mark.state; marker_loop
AZ_RERUN=1 AZ_BOOT_WINDOW=1000000000 boot
chk "rc r10's input: a mods update restarts by itself" restarted
cp /tmp/az-usbload.policy /usr/sbin/az-usbload; chmod +x /usr/sbin/az-usbload
reset
losetup -d "$lo" 2>/dev/null
