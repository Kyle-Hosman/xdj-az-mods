#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Host tests: run in the azmod build container (aarch64), from the source root (the directory
# that holds azmod/):  bash azmod/test/community/run.sh
# Every case is a real mod .so, a real stage dir and the real community.c.
set -u
T=$(cd "$(dirname "$0")" && pwd); A=$T/../..; W=/tmp/cmtest; rm -rf $W; mkdir -p $W
CC=${CC:-gcc}
# the harness (the host and community.c) may be built with a sanitizer: HARNESS_CC; the mods never are,
# since the host refuses a mod that links a sanitizer runtime as it would on a deck
HCC=${HARNESS_CC:-$CC}
PASS=0; FAIL=0
# cv: each mod's values in the settings file ($W/root/azmod.conf), one file per mod in the old
# key=number / key$=text form ($W/cv/<id>.cfg), rebuilt before every check
cv(){ rm -rf $W/cv; mkdir -p $W/cv; [ -f $W/root/azmod.conf ] || return 0
    CVD=$W/cv perl -ne 'next unless /^([a-z0-9-]+):(cfg|str)\.(\S+) = (.*)$/; my ($i,$n,$k,$v)=($1,$2,$3,$4); $k=~s/:([0-9a-f]{2})/chr hex $1/ge; if ($n eq "str") { $v=~s/^"(.*)"$/$1/; $k.="\$" } open my $f, ">>", "$ENV{CVD}/$i.cfg" or die; print $f "$k=$v\n"' $W/root/azmod.conf; }
ok(){ cv; if eval "$2"; then PASS=$((PASS+1)); echo "PASS $1"; else FAIL=$((FAIL+1)); echo "FAIL $1"; echo "---- output:"; cat $W/out; fi; }
$HCC -O1 -g -Wall -I$A -DCM_ROOT="\"$W/root\"" -DCM_LOGDIR="\"$W/log\"" -DCM_USB_ROOT="\"$W/usb\"" -no-pie -o $W/harness $T/harness.c -ldl -lpthread -rdynamic 2>$W/cc.log || { cat $W/cc.log; exit 1; }
TAG=$($W/harness tag); read A0 A1 < <($W/harness addrs)
echo "tag $TAG"
$W/harness tagmp > $W/out
ok "tag stable when code pages are re-protected (hooks)" 'grep -q STABLE $W/out'
# the community Reload confirm asks whenever a track is loaded, also on a stick without stems
ok "Reload confirm: a loaded deck asks, with no STEMS tree on the stick" '[ "$($W/harness anyloaded 1)" = 1 ]'
ok "Reload confirm: decks not known asks" '[ "$($W/harness anyloaded -1)" = 1 ]'
ok "Reload confirm: every deck empty does not ask" '[ "$($W/harness anyloaded 0)" = 0 ]'
ok "CONTROL: the stems export alone answers \"nothing loaded\" for that deck (the old question)" '[ "$($W/harness anyloaded-old)" = 0 ]'
cp $W/harness $T/.harness.bin 2>/dev/null   # for tagoracle.py, run on the host (no python in the build container)
mod(){  # mod <id> <extra cflags...>
    local id=$1; shift
    mkdir -p $W/root/az-usbload/community/$id
    $CC -O1 -fPIC -shared -fvisibility=hidden -I$A -DMOD_ID="\"$id\"" "$@" -o $W/root/az-usbload/community/$id/$id.so $T/mods.c ${MODLIBS:-} 2>>$W/cc.log || { echo "mod $id build failed"; cat $W/cc.log; exit 1; }
}
manifest(){  # manifest <id> <firmware-json> <hooks> [extra]
    printf '{"azmod":1,"id":"%s","name":"Test %s","version":"1.0.0","author":"t","summary":"s","source":"https://example.com/x","api":1,"firmware":%s,"hooks":%s%s}\n' \
        "$1" "$1" "$2" "$3" "${4:-}" > $W/root/az-usbload/community/$1/azmod.json
}
# enable = turned on on the stick and approved on the deck (the deck's own list, section 8), so the
# cases before section 8 do not depend on that list; enable_only leaves the deck's list alone
enable_only(){ printf '%s\n' "$@" > $W/root/az-usbload/community/enabled; }
approve(){ mkdir -p $W/root/azmod-community; : > $W/root/azmod-community/.approved-init
    for i in "$@"; do f=$W/root/az-usbload/community/$i/$i.so
        [ -f "$f" ] && echo "$i $(sha256sum < "$f" | cut -d' ' -f1)" >> $W/root/azmod-community/approved; done; return 0; }
enable(){ enable_only "$@"; approve "$@"; }
reset(){ rm -rf $W/root $W/log; mkdir -p $W/root/az-usbload/community; }
st(){ grep "^MOD $1 " $W/out | sed 's/.*state=\([0-9]*\).*/\1/'; }
rs(){ grep "^MOD $1 " $W/out | sed 's/.*reason=//'; }

# 1. good "any" mod loads, registers a screen, ticks, edits the LED frame, clears quarantine
reset; mod good-any; manifest good-any '["any"]' false; enable good-any
$W/harness init run > $W/out
ok "good any-mod loads"            '[ "$(st good-any)" = 1 ] && grep -q "screen=1" $W/out'
ok "quarantine counter cleared at 90s" 'grep -q "STARTING_EXISTS 0" $W/out'
ok "on_frame edits the LED frame"  'grep -q "FRAME 0 byte0=42" $W/out'
ok "cfg flushed to the settings file"           'grep -q "^ticked=1" $W/cv/good-any.cfg'
ok "load line logged without logs on" 'grep -q "loaded 1.0.0" $W/log/good-any.log'
# the per-mod log cap holds across EP147 restarts: a log left by earlier starts past half the
# cap moves to <id>.log.1 at this start's first line (it would otherwise grow 256 KB per start)
reset; mod good-any; manifest good-any '["any"]' false; enable good-any
mkdir -p $W/log; head -c 200000 /dev/zero | tr '\0' x > $W/log/good-any.log
$W/harness init run > $W/out
ok "per-mod log: an old log past half the cap moves to .1" '[ "$(wc -c < $W/log/good-any.log.1)" -eq 200000 ] && grep -q "loaded 1.0.0" $W/log/good-any.log && [ "$(wc -c < $W/log/good-any.log)" -lt 200000 ]'

# 0. "Update available" only for a newer version: catalog 0.9.0 with 1.0.1 installed is not an
# update. Control: a plain "versions differ" test would offer it.
ok "update offered for a newer version (1.0.3 over 1.0.1)" '[ "$($W/harness vernewer 1.0.3 1.0.1)" = 1 ]'
ok "update offered numerically (1.0.10 over 1.0.9)"         '[ "$($W/harness vernewer 1.0.10 1.0.9)" = 1 ]'
ok "no update for an older version (0.9.0 under 1.0.1)"      '[ "$($W/harness vernewer 0.9.0 1.0.1)" = 0 ]'
ok "no update for the same version"                          '[ "$($W/harness vernewer 1.0.1 1.0.1)" = 0 ]'
ok "no update for a malformed version"                       '[ "$($W/harness vernewer 1.1 1.0.1)" = 0 ]'
ok "CONTROL: the old test (versions differ) offers 0.9.0 under 1.0.1" '[ "$($W/harness verdiffer 0.9.0 1.0.1)" = 1 ]'

# 1a. min_core against the core's AZ_CORE_VERSION (1.2.3): equal / older / absent load; newer and
# malformed are refused with the reason on the Mod Menu
reset
for m in mc-eq mc-old mc-none mc-new mc-bad; do mod $m; done
manifest mc-eq '["any"]' false ',"min_core":"1.2.3"'
manifest mc-old '["any"]' false ',"min_core":"1.0.0"'
manifest mc-none '["any"]' false
manifest mc-new '["any"]' false ',"min_core":"1.2.10"'
manifest mc-bad '["any"]' false ',"min_core":"1.3"'
enable mc-eq mc-old mc-none mc-new mc-bad
$W/harness init > $W/out
ok "min_core equal loads"          '[ "$(st mc-eq)" = 1 ]'
ok "min_core older loads"          '[ "$(st mc-old)" = 1 ]'
ok "no min_core loads"             '[ "$(st mc-none)" = 1 ]'
ok "min_core newer refused (1.2.10 > 1.2.3, numeric not text)" '[ "$(st mc-new)" = 3 ] && rs mc-new | grep -q "Needs AZ-MODS 1.2.10 or newer"'
ok "min_core malformed refused"    '[ "$(st mc-bad)" = 3 ] && rs mc-bad | grep -q "MAJOR.MINOR.PATCH"'

# 1c. core 1.2.3 services: text settings round-trip (control bytes dropped, default when unset),
# data_dir = MODS/community-mods/<id>/data on the stick the mod was installed from (created), assets_dir
# = the package's assets/ there, both NULL with no such stick (control: the same mod, stick absent)
reset; rm -rf $W/usb; mod files-mod -DMOD_FILES; manifest files-mod '["any"]' false; enable files-mod
mkdir -p $W/usb/sdz1/MODS/community-mods/files-mod/assets; cp $W/root/az-usbload/community/files-mod/azmod.json $W/usb/sdz1/MODS/community-mods/files-mod/
$W/harness init run > $W/out
CF=$W/cv/files-mod.cfg
ok "text setting round-trips, control bytes dropped" 'grep -qx "str_ok=1" $CF && grep -qx "def_ok=1" $CF && grep -qx "name\$=Helloworld!" $CF'
ok "data_dir on the stick, created"                 'grep -qx "data\$=$W/usb/sdz1/MODS/community-mods/files-mod/data" $CF && [ -d $W/usb/sdz1/MODS/community-mods/files-mod/data ]'
ok "assets_dir = the package assets/ on the stick"  'grep -qx "assets\$=$W/usb/sdz1/MODS/community-mods/files-mod/assets" $CF'
ok "numbers and text share the file, old parser safe" 'grep -qx "has_api=1" $CF && [ "$(grep -c "^loaded=" $CF)" = 1 ]'
ok "stick present all along: the later answer is the path (CONTROL for the eject case)" 'grep -qx "after\$=$W/usb/sdz1/MODS/community-mods/files-mod/data" $CF'
reset; rm -rf $W/usb $W/usb.away; mod files-mod -DMOD_FILES; manifest files-mod '["any"]' false; enable files-mod
mkdir -p $W/usb/sdz1/MODS/community-mods/files-mod; cp $W/root/az-usbload/community/files-mod/azmod.json $W/usb/sdz1/MODS/community-mods/files-mod/
CM_TEST_EJECT=$W/usb $W/harness init run > $W/out
ok "after an eject: data_dir NULL, and a pointer kept from before still holds the full path (never \"\")" 'grep -qx "after\$=NULL" $CF && grep -qx "kept\$=$W/usb/sdz1/MODS/community-mods/files-mod/data" $CF'
ok "asked again right after the eject (inside the re-check window): NULL, never the old path" 'grep -qx "soon\$=NULL" $CF'
reset; rm -rf $W/usb $W/usb.away; mod files-mod -DMOD_FILES; manifest files-mod '["any"]' false; enable files-mod
$W/harness init run > $W/out
ok "no stick: data_dir and assets_dir NULL (CONTROL)" 'grep -qx "data\$=NULL" $CF && grep -qx "assets\$=NULL" $CF'

# 1c1. text settings at their limits (a value cut at 127 bytes, a buffer shorter than the value,
# buffers of 1 and 0 bytes, keys with a control byte or over 31 bytes, the 17th text key)
reset; rm -rf $W/usb; mod stredge-mod -DMOD_STREDGE; manifest stredge-mod '["any"]' false; enable stredge-mod
$W/harness init run > $W/out
SF=$W/cv/stredge-mod.cfg
ok "a text value over 127 bytes is cut whole characters only" 'grep -qx "long_len=126" $SF && grep -qx "long_whole=1" $SF'
ok "a buffer shorter than the value: cut whole characters only, the length returned" 'grep -qx "cut_len=4" $SF && grep -qx "cut_whole=1" $SF'
ok "a 1-byte buffer gets \"\" (0), a 0-byte one is not touched" 'grep -qx "n1=1" $SF && grep -qx "n0=1" $SF'
ok "a key with a control byte, or over 31 bytes: refused" 'grep -qx "crkey_refused=1" $SF && grep -qx "longkey_refused=1" $SF'
ok "the 17th text key is dropped" 'grep -qx "over_dropped=1" $SF'
ok "the settings file stays one line per key" '[ "$(grep -c . $SF)" = "$(sort -u $SF | grep -c .)" ] && ! grep -q "$(printf "\r")" $SF'

# 1c2. deck_loaded / deck_title follow the deck's load state, with no STEMS tree on the stick:
# deck 1 loaded, deck 2 empty; the title is the track's when the stems path has no file name
reset; mod decks-mod -DMOD_DECKS; manifest decks-mod '["any"]' false; enable decks-mod
CM_TEST_DECKS=LE $W/harness init run > $W/out
DF=$W/cv/decks-mod.cfg
ok "deck_loaded: a loaded deck is 1 without a STEMS tree, an empty deck 0" 'grep -qx "loaded0=1" $DF && grep -qx "loaded1=0" $DF'
ok "deck_title: the loaded deck's title, nothing for the empty one" 'grep -qx "title0\$=Title 1" $DF && grep -qx "title0_len=7" $DF && grep -qx "title1_len=0" $DF'
ok "deck_title: buffers of 1 byte and decks -1 / 4 give nothing" 'grep -qx "title_n1=1" $DF && grep -qx "title_bad=1" $DF'
reset; mod decks-mod -DMOD_DECKS; manifest decks-mod '["any"]' false; enable decks-mod
CM_TEST_DECKS=LE CM_TEST_TITLE="$(printf '\xc3\xa9\xc3\xa9\xc3\xa9')" $W/harness init run > $W/out
ok "deck_title: a title cut to the buffer keeps whole characters" 'grep -qx "title6_whole=1" $DF && grep -qx "title6_len=4" $DF'
reset; mod decks-mod -DMOD_DECKS; manifest decks-mod '["any"]' false; enable decks-mod
CM_TEST_DECKS=UU $W/harness init run > $W/out
ok "deck_loaded: an unknown deck is not reported as loaded (CONTROL for the case above)" 'grep -qx "loaded0=0" $DF && grep -qx "title0_len=0" $DF'

# 1d. a mod's own thread uses settings, folders and the log while the tick thread writes and the
# core flushes: no crash, the final values intact, the file well-formed (one value per key)
# a mod's own thread logging while the core ticks (under ThreadSanitizer: the state the two
# share, such as the logs switch, is read and written without a race)
reset; MODLIBS=-lpthread mod lograce -DMOD_LOGRACE; manifest lograce '["any"]' false; enable lograce
$W/harness init run > $W/out 2>&1
ok "a mod's thread logging while the core ticks: loaded and running" 'grep -q "^MOD lograce state=1 " $W/out'
reset; MODLIBS=-lpthread mod thr-mod -DMOD_THREADS; manifest thr-mod '["any"]' false; enable thr-mod
$W/harness init run > $W/out
TF=$W/cv/thr-mod.cfg
ok "own thread + tick + flush: each value intact, the two keys at most one step apart" 'n=$(sed -n "s/^wn=//p" $TF); m=$(sed -n "s/^ws[$]=w\([0-9]*\)$/\1/p" $TF); [ -n "$n" ] && [ -n "$m" ] && [ "$n" -gt 0 ] && [ $((n - m)) -ge -1 ] && [ $((n - m)) -le 1 ] && grep -q "^tk=" $TF && grep -q "^MOD thr-mod state=1 " $W/out'
ok "settings file well-formed (each key once)"     '[ -z "$(sed "s/[$]*=.*//" $TF | sort | uniq -d)" ]'

# 1e. azmod.json must be one flat object: a nested object (e.g. {"x":{"firmware":..,"hooks":true}})
# and a repeated key are refused, whatever the first or last value says
reset; mod nested; manifest nested '["any"]' false ',"x":{"firmware":["ep147-0000000000000000"],"hooks":true}'
mod twice; manifest twice '["any"]' false ',"hooks":true'; mod plain; manifest plain '["any"]' false; enable nested twice plain
$W/harness init > $W/out
ok "a nested object in azmod.json refused"   '[ "$(st nested)" = 3 ] && rs nested | grep -q "nested object"'
ok "a repeated key in azmod.json refused"    '[ "$(st twice)" = 3 ] && rs twice | grep -q "repeats a key"'
ok "a plain manifest beside them loads (CONTROL)" '[ "$(st plain)" = 1 ]'

# 1f. a mod changing a setting every tick: written at most every 5 s (p7 is small flash), and a
# one-off change still reaches the file at once (control: the good-any run above wrote ticked=1)
reset; mod hammer -DMOD_HAMMER; manifest hammer '["any"]' false; enable hammer
$W/harness init run > $W/out
ok "a setting changed every tick: at most ~20 writes in 95 s" 'n=$(sed -n "s/^CFGWRITES //p" $W/out); [ -n "$n" ] && [ "$n" -le 22 ] && [ "$n" -ge 2 ]'
ok "the latest value still lands"                           'grep -q "^every_tick=" $W/cv/hammer.cfg'

# 1g. on_frame runs on the control bus (real-time): its log line is queued and written by the next
# tick, a settings call there is refused (never blocks the bus); tick-time settings still work (control)
reset; mod framer -DMOD_FRAMELOG; manifest framer '["any"]' false; enable framer; touch $W/root/azmod-logs.on
$W/harness init run > $W/out
ok "a log line from on_frame reaches the log (queued)" 'grep -q "logged from on_frame" $W/log/framer.log'
ok "settings from on_frame refused, and said so"       '! grep -q "from_frame" $W/cv/framer.cfg && grep -q "from on_frame were refused" $W/log/framer.log'
ok "settings from tick still work (CONTROL)"           'grep -q "^ticked=1" $W/cv/framer.cfg'

# 1h. reserved ids (the folder's own file names, AZ-MODS' own names) are never loaded
reset; mod azmod; manifest azmod '["any"]' false; mod good-any; manifest good-any '["any"]' false; enable azmod good-any
$W/harness init > $W/out
ok "a reserved id is not loaded, a normal one beside it is (CONTROL)" '! grep -q "^MOD azmod " $W/out && [ "$(st good-any)" = 1 ]'

# 1b. a mod linking libX11, and one exporting an extra symbol: refused (azmod-pack agrees)
reset; mod good-any; manifest good-any '["any"]' false
mod links-x11 -Wl,--no-as-needed -lX11; manifest links-x11 '["any"]' false
mod extra-export -DMOD_EXTRA; manifest extra-export '["any"]' false
enable good-any links-x11 extra-export
$W/harness init > $W/out
ok "links libX11 refused"          '[ "$(st links-x11)" = 3 ] && rs links-x11 | grep -q "libX11"'
ok "extra export refused"          '[ "$(st extra-export)" = 3 ] && rs extra-export | grep -q "exports only"'
ok "control beside them loads"     '[ "$(st good-any)" = 1 ]'

# 2. refusals, one mod each, all in one start next to a good mod (the control in the same round)
reset
mod good-any; manifest good-any '["any"]' false
mod wrong-fw -DFWTAG='"ep147-0000000000000000"'; manifest wrong-fw '["ep147-0000000000000000"]' true
mod no-entry -DMOD_NOENTRY; manifest no-entry '["any"]' false
mod exports-read -DMOD_EXPORTREAD; manifest exports-read '["any"]' false
mod any-hooks; manifest any-hooks '["any"]' true
mod init-fail -DMOD_INITFAIL; manifest init-fail '["any"]' false
mod id-mismatch; manifest id-mismatch '["any"]' false; sed -i 's/"id":"id-mismatch"/"id":"other"/' $W/root/az-usbload/community/id-mismatch/azmod.json
mod lib-fw-differs -DFWTAG='"ep147-1111111111111111"'; manifest lib-fw-differs "[\"$TAG\"]" false
mod no-source; manifest no-source '["any"]' false; sed -i 's/"source":"[^"]*",//' $W/root/az-usbload/community/no-source/azmod.json
mod api-2; manifest api-2 '["any"]' false; sed -i 's/"api":1/"api":2/' $W/root/az-usbload/community/api-2/azmod.json
mod turned-off; manifest turned-off '["any"]' false; touch $W/root/az-usbload/community/turned-off/turned-off.off
mkdir -p $W/root/az-usbload/community/x86-lib; printf '\177ELF\002\001\001\000\000\000\000\000\000\000\000\000\003\000\076\000' > $W/root/az-usbload/community/x86-lib/x86-lib.so; head -c 4000 /dev/zero >> $W/root/az-usbload/community/x86-lib/x86-lib.so; manifest x86-lib '["any"]' false
mod missing-json; rm -f $W/root/az-usbload/community/missing-json/azmod.json
enable good-any wrong-fw no-entry exports-read any-hooks init-fail id-mismatch lib-fw-differs no-source api-2 turned-off x86-lib missing-json "BAD/ID" not-installed
$W/harness init > $W/out
ok "control mod still loads beside 13 bad ones" '[ "$(st good-any)" = 1 ]'
ok "wrong firmware refused"        '[ "$(st wrong-fw)" = 3 ] && rs wrong-fw | grep -q "Not made for this firmware"'
ok "no entry refused (ELF)"        '[ "$(st no-entry)" = 3 ] && rs no-entry | grep -q az_mod_v1_entry'
ok "exporting read() refused"      '[ "$(st exports-read)" = 3 ] && rs exports-read | grep -q "Exports read()"'
ok "any + hooks refused"           '[ "$(st any-hooks)" = 3 ] && rs any-hooks | grep -q "patches code"'
ok "init failure = FAILED, closed" '[ "$(st init-fail)" = 4 ] && rs init-fail | grep -q "returned 3"'
ok "manifest id mismatch refused"  '[ "$(st id-mismatch)" = 3 ]'
ok "library fw list must match"    '[ "$(st lib-fw-differs)" = 3 ] && rs lib-fw-differs | grep -q "library is not built"'
ok "no source link refused"        '[ "$(st no-source)" = 3 ] && rs no-source | grep -q "source"'
ok "api 2 refused"                 '[ "$(st api-2)" = 3 ] && rs api-2 | grep -q "API 2"'
ok "per-mod off switch"            '[ "$(st turned-off)" = 2 ]'
ok "x86 library refused (ELF)"     '[ "$(st x86-lib)" = 3 ] && rs x86-lib | grep -q "wrong CPU"'
ok "missing azmod.json refused"    '[ "$(st missing-json)" = 3 ]'
ok "bad id in enabled ignored"     '! grep -q "BAD/ID" $W/out'
ok "not-installed mod refused, not crashed" '[ "$(st not-installed)" = 3 ]'
ok "exactly one live mod"          'grep -q "^LIVE 1" $W/out'

# 2b. the library itself: missing, empty, too large, unloadable, no descriptor, another API's
# descriptor, no init(); and a manifest without a readable firmware list. One start, beside a
# good mod (the control in the same round).
reset
mod good-any; manifest good-any '["any"]' false
mkdir -p $W/root/az-usbload/community/no-lib; manifest no-lib '["any"]' false
mkdir -p $W/root/az-usbload/community/empty-lib; : > $W/root/az-usbload/community/empty-lib/empty-lib.so; manifest empty-lib '["any"]' false
mod huge-lib; truncate -s 17M $W/root/az-usbload/community/huge-lib/huge-lib.so; manifest huge-lib '["any"]' false
mod undef-lib -DMOD_UNDEF; manifest undef-lib '["any"]' false
mod null-entry -DMOD_NULLENTRY; manifest null-entry '["any"]' false
mod abi-2 -DMOD_ABI=2; manifest abi-2 '["any"]' false
mod small-desc -DMOD_SIZE=8; manifest small-desc '["any"]' false
mod no-init -DMOD_NOINIT; manifest no-init '["any"]' false
mod fw-text; manifest fw-text '"any"' false
enable good-any no-lib empty-lib huge-lib undef-lib null-entry abi-2 small-desc no-init fw-text
$W/harness init > $W/out
ok "a good mod loads beside 9 broken libraries (CONTROL)" '[ "$(st good-any)" = 1 ] && grep -q "^LIVE 1" $W/out'
ok "no library file: refused, named"         '[ "$(st no-lib)" = 3 ] && rs no-lib | grep -q "Missing no-lib.so"'
ok "an empty library: refused"               '[ "$(st empty-lib)" = 3 ] && rs empty-lib | grep -q "empty or too large"'
ok "a library over 16 MB: refused"           '[ "$(st huge-lib)" = 3 ] && rs huge-lib | grep -q "empty or too large"'
ok "a library dlopen cannot load: refused, with its reason (not the path)" '[ "$(st undef-lib)" = 3 ] && rs undef-lib | grep -q "Could not load: undefined symbol: az_test_symbol_nobody_defines"'
ok "a descriptor that is NULL: refused"      '[ "$(st null-entry)" = 3 ] && rs null-entry | grep -q "returned nothing"'
ok "another API's descriptor: refused"       '[ "$(st abi-2)" = 3 ] && rs abi-2 | grep -q "different mod API"'
ok "a descriptor too small: refused"         '[ "$(st small-desc)" = 3 ] && rs small-desc | grep -q "different mod API"'
ok "no init(): refused"                      '[ "$(st no-init)" = 3 ] && rs no-init | grep -q "has no init()"'
ok "a firmware entry that is not a list: refused" '[ "$(st fw-text)" = 3 ] && rs fw-text | grep -q "no readable firmware list"'

# 3. hooks: declared + right firmware -> in-text claim ok; same address again, a core-owned one, and one outside EP147 refused
reset; mod hooker -DMOD_CLAIM -DFWTAG="\"$TAG\""; manifest hooker "[\"$TAG\"]" true; enable hooker
H=$((16#$A0 >> 32)); L=$((16#$A0 & 0xffffffff)); H2=$((16#$A1 >> 32)); L2=$((16#$A1 & 0xffffffff))
mkdir -p $W/root/azmod-community; printf 'a0h=%d\na0l=%d\na1h=%d\na1l=%d\na2h=%d\na2l=%d\na3h=0\na3l=4096\n' $H $L $H $L $H2 $L2 > $W/root/azmod-community/hooker.cfg
$W/harness init > $W/out
ok "hooking mod loads"             '[ "$(st hooker)" = 1 ] && grep -q "hooks=1" $W/out'
ok "in-text claim ok"              'grep -q "CLAIM hooker 0 0" $W/out'
ok "same address twice refused"    'grep -q "CLAIM hooker 1 -1" $W/out'
ok "core-owned address refused"    'grep -q "CLAIM hooker 2 -1" $W/out && grep -q "patched by AZ-MODS" $W/log/hooker.log'
ok "address outside EP147 refused" 'grep -q "CLAIM hooker 3 -1" $W/out && grep -q "not inside EP147" $W/log/hooker.log'
# 3b. overlapping a hook is refused either side (a hook rewrites 16 bytes); 16 bytes away is fine (control)
reset; mod hooker2 -DMOD_CLAIM -DFWTAG="\"$TAG\""; manifest hooker2 "[\"$TAG\"]" true; enable hooker2
B=$((16#$A1)); P1=$((B + 4)); P2=$((B - 12)); P3=$((B + 16))
mkdir -p $W/root/azmod-community; printf 'a0h=%d\na0l=%d\na1h=%d\na1l=%d\na2h=%d\na2l=%d\na3h=0\na3l=0\n' $((P1 >> 32)) $((P1 & 0xffffffff)) $((P2 >> 32)) $((P2 & 0xffffffff)) $((P3 >> 32)) $((P3 & 0xffffffff)) > $W/root/azmod-community/hooker2.cfg
$W/harness init > $W/out
ok "a claim 4 bytes into a hook refused"     'grep -q "CLAIM hooker2 0 -1" $W/out && grep -q "overlaps the hook" $W/log/hooker2.log'
ok "a claim 12 bytes before a hook refused"  'grep -q "CLAIM hooker2 1 -1" $W/out'
ok "16 bytes after a hook is allowed (CONTROL)" 'grep -q "CLAIM hooker2 2 0" $W/out'
# 3c. a mod passing another mod's id: the claim and the settings write are refused
reset; mod victim -DMOD_CLAIM -DFWTAG="\"$TAG\""; manifest victim "[\"$TAG\"]" true
mod imposter -DMOD_IMPERSONATE="\"victim\"" -DFWTAG="\"$TAG\""; manifest imposter "[\"$TAG\"]" true; enable victim imposter
mkdir -p $W/root/azmod-community; printf 'a0h=%d\na0l=%d\n' $H $L > $W/root/azmod-community/imposter.cfg
$W/harness init run > $W/out
ok "claim_hook with another mod's id refused" 'grep -qx "as_other_claim=-1" $W/cv/imposter.cfg && grep -q "outside its code" $W/log/victim.log'
ok "settings written with another mod's id refused" '! grep -q "written_by_other" $W/cv/victim.cfg 2>/dev/null'
ok "the victim's own calls still work (CONTROL)" 'grep -q "^loaded=1" $W/cv/victim.cfg'
# 3d. the same against a mod that is installed but never loaded (switched off): it has no code, so
# nothing may act as it (its settings, its log; data_dir goes through the same check, but a
# never-loaded mod has no stick folder in this harness, so asking for it could not show a leak)
reset; rm -rf $W/usb; mkdir -p $W/usb; mod sleeper; manifest sleeper '["any"]' false; touch $W/root/az-usbload/community/sleeper/sleeper.off
mod imposter2 -DMOD_IMPERSONATE="\"sleeper\""; manifest imposter2 '["any"]' false; enable imposter2
mkdir -p $W/root/azmod-community; echo planted=7 > $W/root/azmod-community/sleeper.cfg; touch $W/root/azmod-logs.on
$W/harness init run > $W/out
ok "a never-loaded mod: its settings cannot be read by another mod" 'grep -qx "other_cfg=-1" $W/cv/imposter2.cfg'
ok "a never-loaded mod: nothing is written to its log by another mod" '! grep -q IMPOSTER-LINE $W/log/sleeper.log 2>/dev/null'
ok "a never-loaded mod: nothing written into its settings by another mod" '! grep -q written_by_other $W/cv/sleeper.cfg'
ok "each kind of refused call is logged for that mod (cfg_set and cfg_get)" 'grep -q "^.*cfg_set refused" $W/log/sleeper.log && grep -q "^.*cfg_get refused" $W/log/sleeper.log'
ok "the imposter itself loaded and ran (CONTROL)" '[ "$(st imposter2)" = 1 ] && grep -q "^loaded=1" $W/cv/imposter2.cfg'

# 3e. claim_hook and register_screen after init(): refused and logged; the same address taken in
# init() by another mod (control: the address itself is claimable)
reset; mod latecomer -DMOD_LATE -DFWTAG="\"$TAG\""; manifest latecomer "[\"$TAG\"]" true
mod early -DMOD_CLAIM -DFWTAG="\"$TAG\""; manifest early "[\"$TAG\"]" true; enable latecomer early
P4=$(( 16#$A0 ))
mkdir -p $W/root/azmod-community
printf 'a0h=%d\na0l=%d\n' $((P4 >> 32)) $((P4 & 0xffffffff)) > $W/root/azmod-community/latecomer.cfg
printf 'a0h=%d\na0l=%d\n' $((P4 >> 32)) $((P4 & 0xffffffff)) > $W/root/azmod-community/early.cfg
$W/harness init run > $W/out
ok "claim_hook after init() refused, and said so" 'grep -qx "late_claim=-1" $W/cv/latecomer.cfg && grep -q "claim_hook .* refused: call it from init()" $W/log/latecomer.log'
ok "register_screen after init() refused, and said so" 'grep -qx "late_screen=-1" $W/cv/latecomer.cfg && grep -q "register_screen refused: call it from init()" $W/log/latecomer.log'
ok "the same claim from init() is taken (CONTROL)" 'grep -q "CLAIM early 0 0" $W/out'
#   hooks not declared -> claim refused, mod still loads
reset; mod quiet -DMOD_CLAIM -DFWTAG="\"$TAG\""; manifest quiet "[\"$TAG\"]" false; enable quiet
mkdir -p $W/root/azmod-community; printf 'a0h=%d\na0l=%d\n' $H $L > $W/root/azmod-community/quiet.cfg
$W/harness init > $W/out
ok "undeclared hooks refused"      '[ "$(st quiet)" = 1 ] && grep -q "CLAIM quiet 0 -1" $W/out && grep -q "does not declare" $W/log/quiet.log'

# 3f. a crash inside libc, from a bad pointer a mod handed it: blame finds the mod up the stack
reset; mod good-any; manifest good-any '["any"]' false; mod libcrash -DMOD_CRASHLIB -fno-omit-frame-pointer; manifest libcrash '["any"]' false
enable good-any libcrash
timeout 10 $W/harness init run > $W/out 2>&1
ok "a crash inside libc is blamed on the mod that called it" '[ "$(cat $W/root/azmod-community/.blame 2>/dev/null)" = libcrash ]'
# 3g. unexplained crashes with community mods on: 3 within 30 min hold them all back (with the reason);
# 2 recent, or 3 old ones, do not (controls); resume clears the record
reset; mod good-any; manifest good-any '["any"]' false; enable good-any; mkdir -p $W/root/azmod-community
N=$(date +%s); printf '%s\n%s\n%s\n' $((N-60)) $((N-600)) $((N-1200)) > $W/root/azmod-community/.crashes
$W/harness init > $W/out
ok "3 unexplained crashes in 30 min: suspended, reason says so" '[ "$(st good-any)" = 5 ] && rs good-any | grep -q "crashed 3 times in 30 minutes"'
printf '%s\n%s\n' $((N-60)) $((N-600)) > $W/root/azmod-community/.crashes; rm -f $W/root/azmod-community/.starting
$W/harness init > $W/out
ok "2 recent crashes: loads (CONTROL)" '[ "$(st good-any)" = 1 ]'
printf '%s\n%s\n%s\n' $((N-4000)) $((N-5000)) $((N-6000)) > $W/root/azmod-community/.crashes; rm -f $W/root/azmod-community/.starting
$W/harness init > $W/out
ok "3 old crashes: loads, and the old ones are dropped (CONTROL)" '[ "$(st good-any)" = 1 ] && ! [ -s $W/root/azmod-community/.crashes ]'

# 4. quarantine: two starts that never reached 90 s -> the third loads nothing, says so; resume clears it
reset; mod good-any; manifest good-any '["any"]' false; enable good-any
$W/harness init > $W/out; $W/harness init > $W/out; $W/harness init > $W/out
ok "suspended after two short starts" 'grep -q "^SUSPENDED 1" $W/out && [ "$(st good-any)" = 5 ]'
rm -f $W/root/azmod-community/.starting; $W/harness init > $W/out
ok "loads again after resume"      '[ "$(st good-any)" = 1 ]'
#   a reload the DJ asks for is not a failed start: two short starts that each ended in Reload
reset; mod good-any; manifest good-any '["any"]' false; enable good-any
$W/harness init reload > $W/out; $W/harness init reload > $W/out; $W/harness init > $W/out
ok "two Reloads in a row: still loads" '[ "$(st good-any)" = 1 ] && grep -q "^SUSPENDED 0" $W/out'
#   but a Reload while quarantined does not lift the quarantine (only Resume does)
$W/harness init > $W/out; $W/harness init > $W/out; $W/harness init reload > $W/out; $W/harness init > $W/out
ok "Reload while quarantined: stays suspended" '[ "$(st good-any)" = 5 ]'
#   all community mods off (stick switch, then the menu switch)
reset; mod good-any; manifest good-any '["any"]' false; enable good-any; touch $W/root/az-usbload/community-mods.off
$W/harness init > $W/out
ok "community-mods.off loads nothing"   '[ "$(st good-any)" = 2 ] && ! [ -e $W/root/azmod-community/.starting ]'
ok "all-off: the mod shows its azmod.json name" 'grep -q "^MOD good-any .*name=Test good-any|" $W/out'
reset; mod good-any; manifest good-any '["any"]' false; enable good-any; mkdir -p $W/root/azmod-community; touch $W/root/azmod-community/off
$W/harness init > $W/out
ok "menu off switch loads nothing" '[ "$(st good-any)" = 2 ]'
#   nothing enabled = nothing touched
reset; $W/harness init > $W/out
ok "no mods: no quarantine file, no p7 writes" '! [ -e $W/root/azmod-community ]'
# 5. crash blame: a mod faults in tick -> EP147 dies -> next start suspends only it
reset; mod good-any; manifest good-any '["any"]' false; mod crasher -DMOD_CRASH; manifest crasher '["any"]' false
enable good-any crasher
$W/harness init crash > $W/out 2>&1; RC=$?
ok "a crashing mod still kills the process (no swallowed fault)" '[ $RC -ge 128 ] && ! grep -q "NO CRASH" $W/out'
ok "blame names the crashing mod"  '[ "$(cat $W/root/azmod-community/.blame 2>/dev/null)" = crasher ]'
$W/harness init > $W/out
ok "next start: only the crasher is suspended" '[ "$(st crasher)" = 5 ] && [ "$(st good-any)" = 1 ] && rs crasher | grep -q "crashed the deck"'
ok "suspended mod keeps its real name" 'grep -q "^NAME crasher Test crasher" $W/out'
ok "blame consumed, stage untouched" '! [ -e $W/root/azmod-community/.blame ] && ! [ -e $W/root/az-usbload/community/crasher/crasher.off ]'
rm -f $W/root/azmod-community/crasher.crashed; $W/harness init > $W/out
ok "unsuspend loads it again"      '[ "$(st crasher)" = 1 ]'
#   a crash not in a mod blames nobody
reset; mod good-any; manifest good-any '["any"]' false; enable good-any
$W/harness init selfcrash > $W/out 2>&1
ok "core crash blames no mod"      '! [ -e $W/root/azmod-community/.blame ]'
# a disabled mod (out of `enabled`, <id>/<id>.off, what `az-community disable` leaves) is
# listed as off so the Mod Menu can offer Enable; it never loads and never counts toward the
# quarantine. A dir with neither is not listed.
reset; mod good-any; manifest good-any '["any"]' false; mod was-on; manifest was-on '["any"]' false
touch $W/root/az-usbload/community/was-on/was-on.off; mod stray; manifest stray '["any"]' false; enable good-any
$W/harness init > $W/out
ok "disabled mod listed as off"     '[ "$(st was-on)" = 2 ] && rs was-on | grep -q "Disabled"'
ok "disabled mod shows its azmod.json name, not its id (read at listing; same fact as below)" 'grep -q "^MOD was-on .*name=Test was-on|" $W/out'
ok "disabled mod keeps its real name" 'grep -q "^NAME was-on Test was-on" $W/out'
ok "control beside it loads"        '[ "$(st good-any)" = 1 ]'
ok "dir with no .off not listed"    '! grep -q "^MOD stray " $W/out'
reset; mod was-on; manifest was-on '["any"]' false; touch $W/root/az-usbload/community/was-on/was-on.off; enable
$W/harness init > $W/out
ok "only-disabled: listed, no quarantine count" '[ "$(st was-on)" = 2 ] && ! [ -e $W/root/azmod-community/.starting ]'
# 6. a mod that freezes or crashes inside its own init(): the "starting" marker names it, one start
for kind in INITHANG INITCRASH CTORCRASH; do
    reset; mod good-any; manifest good-any '["any"]' false
    mod stuck -DMOD_$kind; manifest stuck '["any"]' false; enable good-any stuck
    # SIGTERM, then SIGKILL as systemd stops EP147: under TSan a SIGTERM waits for a libc call the spin never makes
    timeout -k 2 5 $W/harness init > $W/out 2>&1; RC=$?
    ok "$kind: the start does not finish" '[ $RC -ne 0 ]'
    ok "$kind: the starting marker names the mod" '[ "$(cat $W/root/azmod-community/.initing 2>/dev/null)" = stuck ]'
    $W/harness init > $W/out
    ok "$kind: next start suspends only it" '[ "$(st stuck)" = 5 ] && [ "$(st good-any)" = 1 ] && rs stuck | grep -q "stopped while this mod was starting"'
    ok "$kind: marker consumed" '! [ -e $W/root/azmod-community/.initing ]'
done
# 7. one id, one owner: settings (p7), files (the mod folder's data/ on the stick) and a crash suspension
# belong to the repository the installed copy came from (.origin repo=). Another author's mod under
# the same id (remove one, install the other) starts fresh; the first one's state is set aside and
# comes back when that one is installed again. Controls: the same owner keeps everything.
RA=https://example.com/alice/azmods-repo.json; RB=https://example.com/bob/azmods-repo.json
STG=$W/root/az-usbload/community/files-mod; STK=$W/usb/sdz1/MODS/community-mods/files-mod
CF=$W/cv/files-mod.cfg
origin(){ printf 'repo=%s\nversion=1.0.0\n' "$1" > $STG/.origin; printf 'repo=%s\nversion=1.0.0\n' "$1" > $STK/.origin; }
reset; rm -rf $W/usb $W/usb.away; mod files-mod -DMOD_FILES; manifest files-mod '["any"]' false; enable files-mod
mkdir -p $STK; cp $STG/azmod.json $STK/; origin $RA
$W/harness init run > $W/out
ok "owner A: loads, settings and data folder stamped with A" '[ "$(st files-mod)" = 1 ] && grep -qx "loaded=1" $CF && [ "$(cat $W/root/azmod-community/files-mod.owner)" = $RA ] && [ "$(cat $STK/.data.owner)" = $RA ]'
echo alice > $STK/data/mine.txt
$W/harness init run > $W/out
ok "CONTROL: the same owner again (an update) keeps its settings and files" 'grep -qx "loaded=2" $CF && [ "$(cat $STK/data/mine.txt)" = alice ] && grep -qx "data\$=$STK/data" $CF'
origin $RB; : > $W/root/azmod-community/files-mod.crashed   # A crashed last; then B's mod replaced it
$W/harness init run > $W/out
ok "owner B: not suspended for A's crash" '[ "$(st files-mod)" = 1 ] && ! [ -e $W/root/azmod-community/files-mod.crashed ]'
ok "owner B: starts with fresh settings (loaded=1, not A's 3)" 'grep -qx "loaded=1" $CF && [ "$(cat $W/root/azmod-community/files-mod.owner)" = $RB ]'
ok "owner B: an empty data folder at the same path, A's file not in it" '[ -d $STK/data ] && ! [ -e $STK/data/mine.txt ] && grep -qx "data\$=$STK/data" $CF && [ "$(cat $STK/.data.owner)" = $RB ]'
ok "A's settings and files are set aside, not deleted" '[ "$(ls -a $W/root/azmod-community | grep -c "^\.files-mod@[0-9a-f]\{16\}\.cfg$")" = 1 ] && grep -qx "loaded=2" $W/root/azmod-community/.files-mod@*.cfg && [ "$(cat $STK/.data@*/mine.txt)" = alice ]'
echo bob > $STK/data/mine.txt
origin $RA
$W/harness init run > $W/out
ok "A again: its own settings and files come back (loaded=3, alice), B's set aside" 'grep -qx "loaded=3" $CF && [ "$(cat $STK/data/mine.txt)" = alice ] && [ "$(cat $STK/.data@*/mine.txt)" = bob ] && grep -qx "loaded=1" $W/root/azmod-community/.files-mod@*.cfg'
# GitHub ignores case in owner/name: the same repository spelled two ways is one owner
GA=https://raw.githubusercontent.com/Alice/Mods/HEAD/azmods-repo.json
origin $GA; $W/harness init run > $W/out
printf 'repo=%s\n' https://raw.githubusercontent.com/alice/mods/HEAD/azmods-repo.json > $STK/.origin
$W/harness init run > $W/out
ok "the same GitHub repository in another letter case keeps settings and files" 'grep -qx "loaded=2" $CF && grep -qx "data\$=$STK/data" $CF && [ "$(ls -a $STK | grep -c "^\.data@"|ls -a $STK | grep -c "^\.data@")" = 2 ]'
# the stick: only one that carries this owner's copy of the id
origin $RA; printf 'repo=%s\n' $RB > $STK/.origin
$W/harness init run > $W/out
ok "a stick whose copy of the id is another author's is not this mod's: data_dir and assets_dir NULL" 'grep -qx "data\$=NULL" $CF && grep -qx "assets\$=NULL" $CF'
mkdir -p $W/usb/sdy1/MODS/community-mods/files-mod/assets; cp $STG/azmod.json $W/usb/sdy1/MODS/community-mods/files-mod/
printf 'repo=%s\n' $RA > $W/usb/sdy1/MODS/community-mods/files-mod/.origin
$W/harness init run > $W/out
ok "two sticks with the id: the one carrying this owner's copy is used" 'grep -qx "data\$=$W/usb/sdy1/MODS/community-mods/files-mod/data" $CF && grep -qx "assets\$=$W/usb/sdy1/MODS/community-mods/files-mod/assets" $CF'
# a copy without .origin (copied by hand) is owner "local" on both sides: control, it works
reset; rm -rf $W/usb; mod files-mod -DMOD_FILES; manifest files-mod '["any"]' false; enable files-mod
mkdir -p $STK; cp $STG/azmod.json $STK/
$W/harness init run > $W/out
ok "CONTROL: no .origin anywhere: owner local, data_dir on the stick" 'grep -qx "data\$=$STK/data" $CF && [ "$(cat $STK/.data.owner)" = local ]'
# 7b. a mod's values live in the settings file (azmod.conf = MODS/azmods.cfg), so they follow the
# USB: numbers and text round-trip through it, values brought in on a USB are read, another owner's
# values never reach this one (set aside on p7), and a test build's p7 file is moved in once
C7=$W/root/azmod.conf
reset; rm -rf $W/usb; mod files-mod -DMOD_FILES; manifest files-mod '["any"]' false; enable files-mod
$W/harness init run > $W/out
ok "values written to the settings file, number and quoted text, none on p7" 'grep -qx "files-mod:cfg.loaded = 1" $C7 && grep -qx "files-mod:str.name = \"Helloworld!\"" $C7 && grep -qx "files-mod:owner.repo = local" $C7 && ! [ -e $W/root/azmod-community/files-mod.cfg ]'
$W/harness init run > $W/out
ok "and read back from it at the next start (loaded=2)" 'grep -qx "files-mod:cfg.loaded = 2" $C7'
printf 'version = 1\nfiles-mod:cfg.loaded = 41\nfiles-mod:owner.repo = local\n' > $C7
$W/harness init run > $W/out
ok "values brought in with the USB's settings file are the mod's (41 -> 42)" 'grep -qx "files-mod:cfg.loaded = 42" $C7'
printf 'version = 1\nfiles-mod:cfg.loaded = 41\nfiles-mod:owner.repo = https://example.com/bob/azmods-repo.json\n' > $C7
$W/harness init run > $W/out
ok "another owner's values: not handed over (loaded=1), set aside on p7, out of the file" 'grep -qx "files-mod:cfg.loaded = 1" $C7 && grep -qx "loaded=41" $W/root/azmod-community/.files-mod@*.cfg && ! grep -q "= 41" $C7 && grep -qx "files-mod:owner.repo = local" $C7'
reset; rm -rf $W/usb; mod files-mod -DMOD_FILES; manifest files-mod '["any"]' false; enable files-mod
mkdir -p $W/root/azmod-community; printf 'loaded=7\nBig Key=5\nnote$= two spaces  \n' > $W/root/azmod-community/files-mod.cfg
$W/harness init run > $W/out
ok "a test build's p7 file is moved into the settings file once (7 -> 8), then removed" 'grep -qx "files-mod:cfg.loaded = 8" $C7 && ! [ -e $W/root/azmod-community/files-mod.cfg ]'
ok "a key outside [a-z0-9._-] is escaped, text keeps its spaces, both read back" 'grep -qx "files-mod:cfg.:42ig:20:4bey = 5" $C7 && grep -qx "Big Key=5" $W/cv/files-mod.cfg && grep -qx "note\$= two spaces  " $W/cv/files-mod.cfg'
$W/harness init run > $W/out
ok "CONTROL: the next start reads the file only (8 -> 9), the p7 file not recreated" 'grep -qx "files-mod:cfg.loaded = 9" $C7 && ! [ -e $W/root/azmod-community/files-mod.cfg ]'

# 8. Approved mods: enabled on the stick is not enough. The deck keeps its own list (p7:
# azmod-community/approved, "<id> <sha256 of <id>.so>"), written by a deck install (az-community),
# by Allow, and once by the grandfathering. ran-<id> is written by the mod's constructor: it
# exists only if any of the mod's code ran (dlopen runs it before init()).
AP=$W/root/azmod-community/approved; AI=$W/root/azmod-community/.approved-init
CS=$W/root/az-usbload/community
sha(){ sha256sum < $CS/$1/$1.so | cut -d' ' -f1; }
ran(){ [ -e $W/ran-$1 ]; }
# one EP147 start; each start here follows a Reload (which clears the quarantine counter), so
# several in a row are not "restarted right after starting"
start(){ rm -f $W/root/azmod-community/.starting; $W/harness init "$@"; }
lines_ok(){ ! grep -qvE '^[a-z0-9-]{3,32} [0-9a-f]{64}$' $AP; }
orig(){ printf 'repo=%s\nversion=1.0.0\nsha256=%s\nsize=1\ninstalled=2000-01-01 00:00\n' "${2:-https://raw.githubusercontent.com/Owner/Repo/HEAD/azmods-repo.json}" \
        "$(printf 'a%.0s' $(seq 64))" > $CS/$1/.origin; }
# 8a. the digest: sha256sum's answer, the FIPS 180-4 vectors; control: one byte different
reset; mod good-any; manifest good-any '["any"]' false
ok "sha256 of a mod file equals sha256sum's"        '[ "$($W/harness sha $CS/good-any/good-any.so)" = "$(sha good-any)" ]'
ok "sha256 FIPS vector \"abc\""                       '[ "$($W/harness shastr abc)" = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad ]'
ok "sha256 FIPS two-block vector (56 bytes)"         '[ "$($W/harness shastr abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq)" = 248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1 ]'
ok "CONTROL: \"abd\" does not give abc's digest"      '[ "$($W/harness shastr abd)" != ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad ]'
# 8b. enabled but not on the list: refused before dlopen (its constructor never ran), shown as a new
# mod; control in the same start: an approved mod beside it loads and its constructor ran
reset; rm -f $W/ran-*
mod good-any -DMOD_CTORMARK; manifest good-any '["any"]' false; mod stranger -DMOD_CTORMARK; manifest stranger '["any"]' false
enable_only good-any stranger; approve good-any
start > $W/out
ok "an enabled mod that is not approved is not loaded"         '[ "$(st stranger)" = 6 ] && rs stranger | grep -qx "New mod found: allow?"'
ok "... and none of its code ran (no constructor)"             '! ran stranger'
ok "CONTROL: the approved mod beside it loads, its code ran"   '[ "$(st good-any)" = 1 ] && ran good-any'
ok "the menu counts one new mod; a hand-copied mod has no origin" 'grep -qx "NEW 1" $W/out && grep -qx "FROM stranger -" $W/out'
ok "the hash the core checked is the file's sha256"            'grep -qx "SHA stranger $(sha stranger)" $W/out'
ok "the start did not add it to the list"                      '! grep -q "^stranger " $AP'
# 8c. Allow: the line lands on p7 in the shared format, the mod loads at the next start
start allow stranger > $W/out
ok "Allow writes it to the list (exact line, shared format)"   'grep -q "^ALLOW 0 allowed=1 NEW 0$" $W/out && grep -qx "stranger $(sha stranger)" $AP && lines_ok'
ok "Allow keeps the other approvals"                           'grep -qx "good-any $(sha good-any)" $AP'
start > $W/out
ok "allowed: it loads after the reload, its code ran"          '[ "$(st stranger)" = 1 ] && ran stranger && grep -qx "NEW 0" $W/out'
ok "CONTROL: Allow on a mod that is not new is refused, the list unchanged" 'S0=$(md5sum < $AP); start allow stranger > $W/out2; grep -q "^ALLOW -1 " $W/out2 && [ "$(md5sum < $AP)" = "$S0" ]'
# 8d. the .so swapped under an approved id: a different hash is a new mod again
rm -f $W/ran-stranger; OLDSHA=$(sha stranger)
mod stranger -DMOD_CTORMARK -O2 -DSWAPPED=1; manifest stranger '["any"]' false
start > $W/out
ok "a swapped .so (different sha256) under an approved id is not loaded" '[ "$(sha stranger)" != "$OLDSHA" ] && [ "$(st stranger)" = 6 ] && ! ran stranger'
ok "CONTROL: the unchanged mod beside it still loads"          '[ "$(st good-any)" = 1 ]'
start allow stranger > $W/out
ok "allowing the new build replaces the id's line (one line per id)" '[ "$(grep -c "^stranger " $AP)" = 1 ] && grep -qx "stranger $(sha stranger)" $AP'
ok "and the old build would now be refused (its hash is gone)" '! grep -q " $OLDSHA$" $AP'
# 8e. Allow re-reads the list under the lock: a line another writer (az-community) added after this
# start read the list survives (control: the mutation without the re-read loses it)
reset; mod good-any; manifest good-any '["any"]' false; mod stranger; manifest stranger '["any"]' false
enable_only good-any stranger; approve good-any
start allowx stranger "outside-mod $(printf 'b%.0s' $(seq 64))" > $W/out
ok "Allow keeps a line written by another writer after this start read the list" 'grep -q "^outside-mod " $AP && grep -qx "stranger $(sha stranger)" $AP && grep -qx "good-any $(sha good-any)" $AP'
# 8f. what a deck install writes (az-community: the id with the new build's hash) loads; the
# previous build's hash for the same id does not
reset; mod upd -DMOD_CTORMARK; manifest upd '["any"]' false; enable_only upd; mkdir -p $W/root/azmod-community; : > $AI
echo "upd 0000000000000000000000000000000000000000000000000000000000000000" > $AP
start > $W/out
ok "CONTROL: a line for the id with another hash does not load it" '[ "$(st upd)" = 6 ]'
echo "upd $(sha upd)" > $AP
start > $W/out
ok "the line a deck update writes (id + the new .so's sha256) loads it" '[ "$(st upd)" = 1 ]'
# 8g. Grandfathering, once. A deck with community state from an earlier core (azmod-community/
# exists), no .approved-init yet: enabled mods with an install record (.origin) are approved
# without asking; a hand copy, a turned-off mod and a broken record are not.
reset; mkdir -p $W/root/azmod-community
for m in gf-rec gf-hand gf-off gf-bad; do mod $m; manifest $m '["any"]' false; done
orig gf-rec; orig gf-off; printf 'repo=https://example.com/x/azmods-repo.json\nversion=1.0.0\n' > $CS/gf-bad/.origin
touch $CS/gf-off/gf-off.off; enable_only gf-rec gf-hand gf-off gf-bad
start > $W/out
ok "grandfathered: enabled with an install record loads without asking" '[ "$(st gf-rec)" = 1 ] && grep -qx "gf-rec $(sha gf-rec)" $AP'
ok "not grandfathered: copied by hand (no .origin)"     '[ "$(st gf-hand)" = 6 ] && ! grep -q "^gf-hand " $AP'
ok "not grandfathered: an install record missing sha256=/installed=" '[ "$(st gf-bad)" = 6 ] && ! grep -q "^gf-bad " $AP'
ok "not grandfathered: turned off"                      '[ "$(st gf-off)" = 2 ] && ! grep -q "^gf-off " $AP'
ok "grandfathering logged, once marker written"         'grep -q "gf-rec grandfathered" $W/out && [ -e $AI ] && grep -q "approved once without asking" $W/log/gf-rec.log'
ok "where it came from: owner/repo of the record"       'grep -qx "FROM gf-rec owner/repo" $W/out'
mod gf-late; manifest gf-late '["any"]' false; orig gf-late; enable_only gf-rec gf-hand gf-bad gf-late
start > $W/out
ok "once: a mod with a record that appears later asks"  '[ "$(st gf-late)" = 6 ] && ! grep -q "^gf-late " $AP'
ok "CONTROL: the grandfathered one still loads"         '[ "$(st gf-rec)" = 1 ]'
rm -f $AI; start > $W/out
ok "CONTROL: without the once marker the same mod would have been grandfathered" '[ "$(st gf-late)" = 1 ] && grep -q "^gf-late " $AP'
#   a deck with no community state from before: nothing grandfathered, the marker written
reset; mod gf-rec; manifest gf-rec '["any"]' false; orig gf-rec; enable_only gf-rec
start > $W/out
ok "no earlier community state: a mod with a record asks, the marker is written" '[ "$(st gf-rec)" = 6 ] && [ -e $AI ] && ! grep -qs "^gf-rec " $AP'
#   it runs on the first start even when community mods are off (all off: nothing loads)
reset; mkdir -p $W/root/azmod-community; touch $W/root/azmod-community/off
mod gf-rec; manifest gf-rec '["any"]' false; orig gf-rec; enable_only gf-rec
start > $W/out
ok "grandfathering runs on the first start even with community mods off" '[ "$(st gf-rec)" = 2 ] && grep -qx "gf-rec $(sha gf-rec)" $AP && [ -e $AI ]'
rm -f $W/root/azmod-community/off; start > $W/out
ok "... and the mod loads once they are back on"        '[ "$(st gf-rec)" = 1 ]'
#   a list the grandfathering could not write: no marker, so the next start tries again
reset; mkdir -p $W/root/azmod-community; mod gf-rec; manifest gf-rec '["any"]' false; orig gf-rec; enable_only gf-rec
mkdir $AP.new   # rename target blocked: the write fails
start > $W/out
ok "a failed write leaves no marker (the next start grandfathers)" '! [ -e $AI ] && [ "$(st gf-rec)" = 6 ]'
rmdir $AP.new; start > $W/out
ok "CONTROL: the next start, writable again, grandfathers it" '[ -e $AI ] && [ "$(st gf-rec)" = 1 ]'
# 8h. a mod that is not approved changes nothing on p7: another author's copy put under an
# installed id (a stranger's stick) neither sets the installed one's settings aside nor is held
# back by its suspension. Control: once allowed, the owner switch happens as in section 7.
OA=https://example.com/alice/azmods-repo.json; OB=https://example.com/bob/azmods-repo.json; SD=$W/root/azmod-community
reset; mod own; manifest own '["any"]' false; orig own $OA; enable own
echo "mine=1" > $SD/own.cfg; echo $OA > $SD/own.owner; : > $SD/own.crashed
mod own -O2 -DOTHER_AUTHOR=1; manifest own '["any"]' false; orig own $OB
start > $W/out
ok "another author's unapproved copy: New mod, not held back by the other copy's suspension" '[ "$(st own)" = 6 ]'
ok "... and nothing on p7 moved: settings, owner stamp and suspension as they were" 'grep -qx "mine=1" $SD/own.cfg && [ "$(cat $SD/own.owner)" = $OA ] && [ -e $SD/own.crashed ] && ! ls -a $SD | grep -q "^\.own@"'
start allow own > $W/out; start > $W/out
ok "CONTROL: allowed, it loads and the owner switch sets the other's state aside" '[ "$(st own)" = 1 ] && [ "$(cat $SD/own.owner)" = $OB ] && ls -a $SD | grep -q "^\.own@" && ! [ -e $SD/own.crashed ]'
#   malformed lines are never matched, and do not break the others
reset; mod good-any; manifest good-any '["any"]' false; enable_only good-any; mkdir -p $W/root/azmod-community; : > $AI
printf 'good-any %s extra\nBAD/ID %s\ngood-any %s\n' "$(sha good-any | tr a-f A-F)" "$(sha good-any)" "$(sha good-any)" > $AP
start > $W/out
ok "a valid line beside malformed ones loads"           '[ "$(st good-any)" = 1 ]'
printf 'good-any %s\n' "$(sha good-any | tr a-f A-F)" > $AP; start > $W/out
ok "CONTROL: an upper-case hash alone is not a match"   '[ "$(st good-any)" = 6 ]'
#   p7 stays tiny: at most 64 lines, the oldest dropped
reset; mod good-any; manifest good-any '["any"]' false; enable_only good-any; mkdir -p $W/root/azmod-community; : > $AI
for i in $(seq 100 199); do echo "m$i $(printf 'c%.0s' $(seq 64))"; done > $AP
start allow good-any > $W/out
ok "the list is capped at 64 lines, the newest kept"    '[ "$(wc -l < $AP)" = 64 ] && tail -1 $AP | grep -q "^good-any " && ! grep -q "^m100 " $AP && grep -q "^m199 " $AP'
# 9. a paint context is live only inside the on_draw that received it: a mod that keeps it and
#    draws from tick draws nothing (the page itself still draws)
reset; mod keeper -DMOD_KEEPDRAW; manifest keeper '["any"]' false; mod good-any; manifest good-any '["any"]' false; enable keeper good-any
start paint > $W/out
ok "a kept paint context draws nothing after on_draw returns" 'grep -q "PAINT painted=2 in_paint=2 after_ticks=0" $W/out'
#    Control: the same mods on a core whose drawing services do not check the context draw
#    from every tick (so the case above can see a stale draw when there is one)
rm -rf $W/ctlsrc; mkdir -p $W/ctlsrc; cp -r $A $W/ctlsrc/azmod; cp -r $A/../common $W/ctlsrc/common 2>/dev/null
perl -0pi -e 's/    if \(d && d == g_cm_paint\)\n        return 1;/    if (d)\n        return 1;/' $W/ctlsrc/azmod/community.c
if cmp -s $A/community.c $W/ctlsrc/azmod/community.c; then echo "MUTATION DID NOT APPLY (paint control)"; FAIL=$((FAIL+1)); fi
$HCC -O1 -g -w -I$W/ctlsrc/azmod -DCM_ROOT="\"$W/root\"" -DCM_LOGDIR="\"$W/log\"" -DCM_USB_ROOT="\"$W/usb\"" -no-pie -o $W/harness-ctl $W/ctlsrc/azmod/test/community/harness.c -ldl -lpthread -rdynamic 2>>$W/cc.log
rm -f $W/root/azmod-community/.starting; $W/harness-ctl init paint > $W/out
ok "CONTROL: without the check, the kept context draws from tick" 'grep -q "PAINT painted=2 in_paint=2 after_ticks=[1-9]" $W/out'
# 10. Controls for the eject, refusal-log and deck-state cases: the same cases on cores with
#     each fix removed must show the unfixed behavior (so those cases can fail)
ctl_core() {   # ctl_core <name> <perl substitution>: $W/harness-<name> built from a mutated community.c
    rm -rf $W/ctlsrc; mkdir -p $W/ctlsrc; cp -r $A $W/ctlsrc/azmod; cp -r $A/../common $W/ctlsrc/common 2>/dev/null
    perl -0pi -e "$2" $W/ctlsrc/azmod/community.c
    if cmp -s $A/community.c $W/ctlsrc/azmod/community.c; then echo "MUTATION DID NOT APPLY ($1 control)"; FAIL=$((FAIL+1)); fi
    $HCC -O1 -g -w -I$W/ctlsrc/azmod -DCM_ROOT="\"$W/root\"" -DCM_LOGDIR="\"$W/log\"" -DCM_USB_ROOT="\"$W/usb\"" -no-pie -o $W/harness-$1 $W/ctlsrc/azmod/test/community/harness.c -ldl -lpthread -rdynamic 2>>$W/cc.log
}
ctl_core stale 's/ && \(!c->data_ok \|\| access\(c->data, F_OK\) == 0\)\)\n        return/)\n        return/'
reset; rm -rf $W/usb $W/usb.away; mod files-mod -DMOD_FILES; manifest files-mod '["any"]' false; enable files-mod
mkdir -p $W/usb/sdz1/MODS/community-mods/files-mod; cp $W/root/az-usbload/community/files-mod/azmod.json $W/usb/sdz1/MODS/community-mods/files-mod/
CM_TEST_EJECT=$W/usb $W/harness-stale init run > $W/out
ok "CONTROL: without the existence check, the stale path is handed out right after the eject" 'grep -qx "soon\$=$W/usb/sdz1/MODS/community-mods/files-mod/data" $W/cv/files-mod.cfg'
ctl_core said 's/int seen = 0, k = 0;/static int said; int seen = said++ != 0, k = 0;/'
reset; rm -rf $W/usb; mkdir -p $W/usb; mod sleeper; manifest sleeper '["any"]' false; touch $W/root/az-usbload/community/sleeper/sleeper.off
mod imposter2 -DMOD_IMPERSONATE="\"sleeper\""; manifest imposter2 '["any"]' false; enable imposter2
mkdir -p $W/root/azmod-community; echo planted=7 > $W/root/azmod-community/sleeper.cfg; touch $W/root/azmod-logs.on
$W/harness-said init run > $W/out
ok "CONTROL: one process-wide flag logs only the first refusal" 'grep -q "cfg_set refused" $W/log/sleeper.log && ! grep -q "cfg_get refused" $W/log/sleeper.log'
ctl_core stems 's/    struct azstem_deck_snap s;\n    return cm_snap\(d, &s\) == 0 && s.loaded == 1;/    return g_deck_has_track_fn ? g_deck_has_track_fn(d) != 0 : 0;/'
reset; mod decks-mod -DMOD_DECKS; manifest decks-mod '["any"]' false; enable decks-mod
CM_TEST_DECKS=LE $W/harness-stems init run > $W/out
ok "CONTROL: following the stems' track, a loaded deck without a STEMS tree reads 0" 'grep -qx "loaded0=0" $W/cv/decks-mod.cfg'
# 11. Turning a mod off and on while the deck runs (az_mod_v1.stop): only a mod with stop() and
#     nothing that lasts until the next start; nothing is called into it between stop() and init();
#     the stage says what was chosen, so a restart without a re-mirror agrees
lv(){ grep "^LIVESET" $W/out | sed -n "${1}p"; }
reset; mod live-mod -DMOD_LIVE; manifest live-mod '["any"]' false; enable live-mod
$W/harness init live live-mod off,on,off,on > $W/out
ok "live off: done now, stop() called, nothing runs, the stage says off" 'lv 1 | grep -q "^LIVESET off rc=1 state=2 byte0=0 ran=0 inits=1 stops=1 filtered=0 filters=1 off=1 enabled=\[\] dirty=0"'
ok "live on again: init() again, on_frame and the filter run, the filter slot reused" 'lv 2 | grep -q "^LIVESET on rc=1 state=1 byte0=42 ran=0 inits=2 stops=1 filtered=1 filters=1 off=0 enabled=\[live-mod \] dirty=0"'
ok "off and on twice: still one filter slot" 'lv 4 | grep -q "^LIVESET on rc=1 state=1 byte0=42 ran=0 inits=3 stops=2 filtered=2 filters=1 off=0"'
reset; mod plain-mod; manifest plain-mod '["any"]' false; enable plain-mod
$W/harness init live plain-mod off > $W/out
ok "no stop(): keeps running until a reload, the stage already says off" 'lv 1 | grep -q "^LIVESET off rc=0 state=1 byte0=42 .* off=1 enabled=\[\]"'
reset; mod live-hooks -DMOD_LIVE -DFWTAG="\"$TAG\""; manifest live-hooks "[\"$TAG\"]" true; enable live-hooks
$W/harness init live live-hooks off > $W/out
ok "stop() but \"hooks\": true: not stopped live" 'lv 1 | grep -q "^LIVESET off rc=0 state=1 byte0=42 .* stops=0"'
reset; mod live-late -DMOD_LIVE; manifest live-late '["any"]' false; enable live-late; enable_only; touch $W/root/az-usbload/community/live-late/live-late.off
$W/harness init live live-late on,off > $W/out
ok "turned on live, never loaded this start: loads now" 'grep -q "^MOD live-late state=2" $W/out && lv 1 | grep -q "^LIVESET on rc=1 state=1 byte0=42 .* inits=1 .* off=0 enabled=\[live-late \]"'
ok "and off again live" 'lv 2 | grep -q "^LIVESET off rc=1 state=2 byte0=0 .* stops=1 .* off=1 enabled=\[\]"'
reset; mod late-plain; manifest late-plain '["any"]' false; enable late-plain; enable_only; touch $W/root/az-usbload/community/late-plain/late-plain.off
$W/harness init live late-plain on,on > $W/out
ok "turned on live without stop(): waits for a reload (twice), the stage says on" 'lv 1 | grep -q "^LIVESET on rc=0 state=2 .* off=0 enabled=\[late-plain \]" && lv 2 | grep -q "^LIVESET on rc=0 state=2"'
reset; mod live-mod -DMOD_LIVE; manifest live-mod '["any"]' false; enable live-mod
$W/harness init livestress live-mod 40 > $W/out
ok "40 live offs and ons while the bus thread runs frames: no call outside init()..stop()" 'grep -q "^STRESS bad=0 overlap=0 filtered=[1-9][0-9]* frames=[1-9][0-9]* filters=1 state=1" $W/out'
ctl_core nodrain 's/    while \(__atomic_load_n\(&c->inflight, __ATOMIC_SEQ_CST\) && k\+\+ < CM_LIVE_DRAIN_MS\) cm_wait_ms\(1\);\n    if \(__atomic_load_n\(&c->inflight, __ATOMIC_SEQ_CST\)\) \{/    if (0) {/'
reset; mod live-mod -DMOD_LIVE; manifest live-mod '["any"]' false; enable live-mod
# the race is this control's point: its sanitizer report is not one of the suite's
TSAN_OPTIONS="${TSAN_OPTIONS:-}:report_bugs=0" $W/harness-nodrain init livestress live-mod 40 > $W/out
ok "CONTROL: without waiting for calls in flight, stop() runs during one" 'grep -q "^STRESS bad=0 overlap=[1-9]" $W/out'
# 12. A service as the last statement of a callback: a tail call at -O2, unless the mod is built
#     with the SDK template's -fno-optimize-sibling-calls; the refusal names that flag
reset; mod tail-flag -DMOD_LIVE -DMOD_TAILSTOP -O2 -fno-optimize-sibling-calls; manifest tail-flag '["any"]' false; enable tail-flag
$W/harness init live tail-flag off > $W/out
ok "last call in stop(), built with the flag: the service runs" 'grep -qx "stopped=1" $W/cv/tail-flag.cfg && ! grep -q "refused" $W/log/tail-flag.log'
reset; mod tail-noflag -DMOD_LIVE -DMOD_TAILSTOP -O2; manifest tail-noflag '["any"]' false; enable tail-noflag
$W/harness init live tail-noflag off > $W/out
ok "CONTROL: last call in stop(), built without the flag: refused, the log names the flag" '! grep -q "^stopped=" $W/cv/tail-noflag.cfg && grep -q "cfg_set refused.*-fno-optimize-sibling-calls" $W/log/tail-noflag.log'
echo "passed $PASS failed $FAIL"
[ $FAIL = 0 ]
