#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Host-native unit tests for safety-critical pure logic in azmod/azstem.
#
#   ep147build   build identification (guest/common/ep147build.h)
#   addr_init    ep147_addr_init "inert unless complete" (azmod + azstem headers)
#   crc16_x25    panel bus frame CRC (guest/azmod/bus_interpose.c)
#   clock native the Clock in the place of EP147's status bar readout, only while it is not
#                recording (guest/azmod/clock_native.h)
#   browse edit  Column Edit: the width file, the divider hit test, a drag's limits, saved widths
#                fitted into the header, what a header tap does (guest/azmod/browse_edit.h)
#   remember wave zoom
#                Remember Waveform Zoom: the saved text, the six levels, a restore only over the
#                stock 1/4, a save only once a new level has settled (guest/azmod/wave_zoom.h)
#   clock_tz     the Clock readout: zone file parsing, offset at a time, 12/24-hour text; unknown,
#                never guessed, past a zone file's last transition (guest/azmod/clock_tz.h)
#   leadin jump  a beat jump back into the Extended Lead-In: a target before the start within the
#                lead-in's floor at the track's sample rate, refused past it (guest/azmod/leadin_jump.h)
#   clock set    Set Time Manually: fields wrap without carrying, local time to UTC through a zone
#                (the skipped hour), shown only when synchronized or set (guest/azmod/clock_set.h)
#   fd tracking  a bus fd that is closed or dup2'd over is forgotten before its number is
#                reused, and nothing is tracked outside EP147 (guest/azmod/bus_interpose.c)
#   azmod.cfg    load_config / save_config / settings_write (guest/azmod/azmod.c)
#   azmod.conf   the settings file: parse, rewrite, atomic save, and the migration from azmod.cfg
#                proven equivalent to 1.2.2's load_config + drum-bank radio (guest/azmod/azconf.c)
#   stem_folder_rank  accepted STEMS/ folder names (guest/azstem/stem_playback.c)
#   env cache    the stems' envelope cache on the stick: whole or not at all, a cut or impossible file
#                rejected (guest/azstem/stem_envelopes.c)
#   ofs cache    the stem offset cache on the stick: a lag reused only for the same source and stem
#                set, anything else a miss (guest/azstem/ofsload.c, stem_playback.c stems_key)
#   rs request   a Remote Stems request: the library's title and path cannot add a key line
#                (guest/azmod/remote_stems.c rs_write_req)
#   rs_status_banner  which Remote Stems status lines reach the banner (guest/azmod/remote_stems.c)
#   rs scan      the Remote Stems queue scan runs on the stem-marks worker and hands one atomic flag to the
#                message thread, also under ThreadSanitizer (guest/azmod/remote_stems.c rs_queue_has_req)
#   decks        what azstem publishes for the loader's restart policy: empty only on proof
#                (guest/azstem/deck_track_map.c)
#   strip neutral  Day overview strokes: neutral ones drawn dark on the light ground (day_mode.c)
#   drum feed    an empty deck rolls at the master deck's BPM, else 120 (guest/azmod/stems_display.c)
#   deckid       a tile deckstate or pad container maps to a deck only through the per-deck
#                table; before it is filled, or for any other pointer, -1 (deckid_test.c)
#   deck screen  the pad-mode getter marks the deck screen as up only for the deck screen's own
#                callers, never for azmod's own reads at startup (deckscreen_test.c)
#   wave att     the stem waveform curve follows the pad toggles and the Stem Volume gains,
#                capped at unity, rate limited per deck, overview tiles per deck on their own
#                cadence (wave_att.h, waveform.c build_atten)
#   folder copy  the FOLDER-view copy plan: names from the stick checked before they become paths,
#                litter never copied, nothing overwritten, folders before their contents, caps
#                (folder_copy.h)
#   stem volume link  CH3/CH4 EQ sets stem volumes only while both channels are free: not while
#                deck 3/4 has a track or a PRO DJ LINK player holds number 3/4, and not when unknown
#                (link_members.h, link_members.c, mod_menu_overlay.c ov_sync_stem_volume)
#   feed lock    60 FPS Waveforms: each deck's position feed runs once per displayed frame, validated
#                at use, dropped when its own timer stops, its own timer the fallback (waveform.c)
#   wave identity  waveform stem scaling and the stem-lane zoom model keyed by the rendering deck
#                (update() or the Content table): unknown = drawn stock and no model, never another
#                deck's; two decks' providers at one address keep separate models (waveform.c)
#   drumtile8    the DRUM ROLL tile-8 index is written only through a chain re-proven at use, and
#                never for an unknown deck (drum_roll_display.c drum8_index_apply)
#   sfxtile      an FX tile's image array is grown once by the four cyan Stem FX images and the
#                asked effect's index returned; a rebuilt or foreign array is grown again, never
#                taken; a bad array or effect gives -1 (guest/azmod/stems_display.c sfx_tile_index)
#   drumtile7    while a deck's drum roll is armed its tile 7 shows the appended HOLD art (HOLD on
#                lit), grown only into the tile's own ImagesComponent array, per deck, never for an
#                unknown deck (drum_roll_display.c drum_tiles_enforce, hold_tile_ready)
#   drumhold     pad 7 toggles the drum roll HOLD on an armed deck (pads 5 and 6 inert); the rate
#                is latched from the last touched X-PAD frame; HOLD sustains only a roll the strip
#                played, per deck, cleared by a deck select or a disarm; pad 7 LED dim / flashing
#                (bus_interpose.c drum_hold_pads, drum_hold_touch, drum_hold_led, mixer_xpad_drum)
#   bjjog        BEAT JUMP 2 First: the jog display's pad page is swapped BEAT JUMP <-> BEAT JUMP 2
#                on a copy handed to PlayerInfoData's store, never in EP147's staging area, and only
#                for the object's own staging area with the expected vtable (beat_jump.c bj_joginfo_hook)
#   jog stems    the stem state on the jog display: the HOT CUE pad page and its pad table per deck by
#                identity, only with Pad Info Display On, EP147's table restored (jog_stems_logic.h,
#                jog_display.c)
#   lifetime     LoadState / DjPlayerUnit: published after the ctor, unpublished by a dtor hook that
#                waits for ls_enter/ls_leave readers; the FGPR read inside the unit's ctor
#                (stem_playback.c, deck_track_map.c hooks)
#   sdk          community mod deck/UI services: beat phase never from two grids, playing, deck
#                events, confirm answered exactly once, the UI queue, thread_start's refusals
#   ltisnap      the deck's track copied inside EP147's LoadedTrackInfo update: no torn read, no lost
#                update between snapshot writers, never served after the LoadState's teardown
#   meowgrid     the drum roll's beat grid read under EP147's own references (meow protocol): one
#                whole live grid under concurrent swaps, every retired grid destroyed once
#   readers      TaglistEditor / mixer Channel / VinylSpeedAdjustor: a destructor unpublishes, then
#                waits for a message-thread reader inside the count (never for its own caller);
#                nothing cached without the destructor hooks (playlist_actions.c, bluetooth.c,
#                vinyl_brake.c, vinyl_slider.c vs_apply)
#   f4 identity  cached EP147 pointers revalidated at use: the per-deck blocks of a pinned
#                gui::PlayerInfoState (deck census, stems_display.c) and the WWB widgets (waveform.c
#                wwb_widget_live): unknown, never a default deck
#   bpmstate     a BpmState is pinned by a weak reference and read only while its use count is
#                above zero and its +0x08 word unchanged (stems_display.c bpmstate_capture/_read)
#   ls_tick      a spool left by a copy that stopped waiting is serviced until applied, at 2 s;
#                the spool's source-stick hint (guest/azmod/library_edits.c)
#   core dialog  the core's own questions (SSH key approval, a mod's removal) are shown with no
#                community mod loaded or running (community.c community_tick, sdk_ui.c)
#   approved list  the community mods the user allowed: whole or not at all, under the fetcher's
#                lock, one line per id, capped; a library's sha256 (guest/azmod/community.c)
#   azmod.json   a community mod's manifest: every refusal by name, the values, the firmware list
#                (guest/azmod/community.c cm_json_*)
#   spool write  the cross-stick copy's spool lands whole or not at all, also on a full stick
#                (library_edits.c cp_spool_write)
#   spool apply  a stick's library spool: applied whole or not at all, only the allowed statements,
#                and pass 2 runs exactly what pass 1 checked (library_edits.c ls_apply_buf)
#   cpspool      a playlist pasted onto another stick keeps every entry, repeats included;
#                a track pasted onto an existing list is not doubled; a pasted track the
#                target already has shows the copied art when its own file is gone, and keeps
#                its own when it is there (library_edits.c cp_track_sql, ls_art_fix); a folder
#                pasted onto another stick arrives with the same tree, ids from 16385, no marks
#                left, and a gone destination writes nothing (cp_folder_sql); PASTE onto the
#                other stick's row with the copied row's id is not "onto itself"
#                (playlist_actions.c pl_paste_is_self)
#   pdbart       export.pdb after a paste onto a stick that lists the tracks: a track whose own
#                art file is gone points at the paste's copy, one artwork row per copy; own art
#                on the stick and OneLibrary's rekordbox art left alone (pdb_sync.c ps_paste_art)
#   pdbsync2     export.pdb sync on generated sticks: a copied row names the analysis folder
#                the paste wrote; ill-formed / over-long names written whole characters only and
#                never stop the sync; a stick pulled at any write leaves the old or the new file
#                whole (pdb_sync.c)
#   pdbthreads   two sticks synced at once (ThreadSanitizer): one sync at a time (pdb_sync.c)
#   utf8cut      names copied into fixed buffers are cut at a character boundary (utf8_cut.h,
#                library_db.c sq_text, ep_mem.h own_str)
#   lspdbsync    a name or path copy that fails stops the export.pdb sync: rc -1, nothing
#                handed to pdbs_sync, every copy freed (library_edits.c ls_pdb_sync_root)
#   pdb_stock    export.pdb after EP147's own playlist writes: one sync per burst, whether or not
#                Playlist Editing is on, none for an unknown stick (guest/azmod/pdb_stock.c)
#   catalog      the Mod Menu's community catalog: caps, "Showing the first", the unlisted note; the
#                download count, Browse's Home (shelves of the tiles that fit, See all, the knob
#                scrolling the page) and the mod page's header
#   cmnotloaded  Community Mods not loaded this start: the notice on the Installed tab only, its wording,
#                Load next time, and Reload after it sends the apply request like Reload now; Keep off
#                keeps the box (cmnotloaded_test.c)
#   cmmod        an installed mod's page: a mod refused at load is enabled (Disable, its reason)
#   screenrow    a mod's SCREEN row opens its page by Community's path; BACK returns to the row (screenrow_test.c)
#   clocktzlist  the Clock's time zone list: zones by region, knob and touch pick, drag scrolls, BACK; the
#                Set Time Manually picker: knob editing, plus and minus, Set Time, BACK (clocktzlist_test.c)
#   ovclip       an overlay window's part of the buffer, cut as XPutImage cuts it (ovclip_test.c)
#   iconcache    the Mod Menu's icon cache: icons inside their box, a kept icon identical to its
#                drawing, ground / ink / theme / size in the key, what is never kept (iconcache_test.c)
#   menuscroll   rows registered past the screen: fixed row height, the lists scroll (knob, drag, tap) with
#                EP147's own scroll bar; both lists glide (a fling after a moving lift, caught by a finger,
#                knob steps glide); icons clipped to the list; a mod's page keeps the offset (menuscroll_test.c)
#   cmorderly    the community quarantine's orderly stop: a SIGTERM while EP147 is alive clears the start
#                count, a frozen deck's does not, the process still ends by the signal (cmorderly_test.c)
#   cmtouch      Community Mods home page by touch: a drag scrolls and never fires, a tap fires on release at the
#                scrolled position, the knob scrolls with no focus, EP147's scroll bar while it overflows (cmtouch_test.c)
#   drumroll     WAV formats + length cap, stick re-discovery, bank swap vs the audio thread,
#                Beat Sync on a paused / empty / playing deck, the grid's sample rate measured
#                from its tempo field and converted to the playhead base, its grid never read
#                torn (guest/azstem/drumroll.c)
#   ogg decode   decode_ogg_stereo_44100: the resample fallback and the length cap
#                (guest/azstem/stem_playback.c)
#   hookreloc    adrp re-encoded only within +-4 GB, else materialized (MOVZ/MOVK) to the same page;
#                the way back's jump; short functions refused (inline_hooks.c, azstem.c)
#   buscfg       the bus threads see whole Mod Menu settings combinations and whole LED blocks
#                (guest/azmod/bus_cfg.h, bus_cfg_publish); busread-lint: read()/ioctl() read
#                nothing else of the overlay's (busread-lint.sh)
#   art targets  stick art lands only at the start of one of this build's PNGs, and fits it
#                (azmod.c az_art_target_len, guest/azmod/ep147png.h from tools/ep147-pngtable.py)
#   azprot       a patch restores the protection /proc/self/maps reports (guest/common/azprot.h)
#   hooks        (aarch64) installs under load; the saved original is stored before every redirect
#                (azm_install_hook both paths, azm_patch_vtable, azstem install_hook_at); after the
#                constructor a 16-byte patch is refused (both libraries) and every install is in the
#                table with its path (inline_hooks.c azm_hooks_go_late, azm_hooks_dump); the crash
#                reporter's chain ends the process when EP147's handler calls back into it, also
#                while another thread is inside (crash_guards.c, hooks/crashchain.c)
#   community host (aarch64) the real community.c hosting real mod libraries: loading, refusals,
#                approval, settings and folder services, hook claims, quarantine, crash blame
#                (guest/azmod/test/community/run.sh), under each pass's sanitizer
#   altstack     (aarch64) the mods' threads and the main thread run an SA_ONSTACK handler after a stack
#                overflow; their alternate stacks are freed at exit (guest/common/azaltstack.h)
#   thread-lint  every thread starts through az_thread_create (thread-lint.sh)
#   redirect-lint  code/vtable redirects only through those ordered installers (redirect-lint.sh)
#   hookorder    azmod's installers run from its constructor, before azm_hooks_go_late, and the
#                overlay thread installs nothing (hookorder-lint.sh)
#   logcap       /tmp appends rotate past a cap; crash log byte cap; azrs log (azlogcap.h, crash_guards.c, azrs.sh)
#   logcap-lint  every append-mode open in azmod/azstem is capped (logcap-lint.sh)
#   azspawn      helpers start via posix_spawn, argv only, LD_PRELOAD cleared, quiet (guest/common/azspawn.h)
#   nofork       the release .so files import no system/popen/fork/exec (nofork-check.sh)
#   procmem      the release .so files read EP147's memory with plain loads only: no /proc/self/mem,
#                no safe_q/safe_qv/safe_str, process_vm_readv only in the crash reporter (procmem-lint.sh)
#   draghandoff  a drag's end, recorded on the bus read thread and performed on the message
#                thread: exactly once, first end wins, no stale record (guest/azmod/drag_handoff.h)
#   busthread    the bus read() interposer makes no UI/banner/drag-step call (busthread-lint.sh)
#   sdk deck_info  exactly the caller's .size written for every size; unknown fields -1 / "", never a
#                guessed value or another deck's (guest/azmod/sdk_deck.c sd_deck_info)
#   sdkinput     community mods' frame and touch filters (sdkinput_test.c)
#   brightgrid   Bright Grid brightens a moving ruler's ticks, never its themed background
#                (brightgrid_test.c, brightgrid.h)
#   brightcfg    Bright Grid's tuning file: applied when it appears, changes or goes away, and polled
#                by the overlay's 1 Hz poll, not by the rates log (brightcfg_test.c, waveform.c)
#   sdkdraw      the SDK drawing services' logic: views, surface limits, presses, blending, lines,
#                the pointer queue (sdkdraw_test.c, sdk_surface_logic.h)
#   bustrace     the bus read path replays a seeded frame stream to the recorded trace (busread-trace.sh)
#   cmlayout     every Community Mods screen draws as recorded, over the catalog and installed-mod
#                fixtures in every view, tab, page and scroll (cmlayout-golden.sh, cmlayout_digest.h)
#   deck slot    an audio unit is a deck by identity only; no number or a held slot is unknown, never
#                the next free slot (guest/azstem/stem_playback.c deck_slot)
#   trackgen     a deck's generation moves on load and eject, before the publish (stem_playback.c)
#   aztap        the audio tap ring: never waits, drops whole blocks and counts them, in order
#                (guest/common/aztap.h)
#   htgest       Hide Track Title toggles only on a whole one-finger tap (guest/azmod/ht_tap.h)
#   clock timer  the Clock's countdown Timer: the countdown rounded up, the last minute flashing red,
#                0:00 for 3 s, the popup's buttons, layout and hit test (guest/azmod/clock_timer.h)
#   fontsize     Remember Font Size: the Browse size is saved when set and applied to the
#                registered FontManager only (guest/azmod/font_size.c)
#   skey         Camelot Keys: row off = EP147's flag as asked; row on = every valid key id in
#                Camelot, into the caller's own string; kill switch = no hook (streaming_key.c)
#   hcanlz       a track's hot cues from its analysis file, and where they sit on a preview
#                (guest/azmod/hc_anlz.h)
#   smorder      the stem marks worker's order: the rows on screen first (stem_marks.c,
#                preview_hotcues.c)
#   stem fx      Stem FX (pads 5-8): echo out and brake rendered from the stem and the press alone,
#                the press state between the panel-bus writer and the audio readers (TSan),
#                tap latches / hold is momentary (guest/azstem/stem_fx.h, guest/azmod/stem_fx_pads.h)
#   blflip       the BEAT LOOP page flip is azmod's only on a deck with stems engaged; elsewhere
#                the press reaches EP147, as stock (guest/azmod/bl_flip.h, bus_interpose.c)
#   spinback     Extended Lead-In: the PlayerCore pre-roll is the cap (row on) or stock (off)
#                before EP147's jog clamp runs; other values and other classes untouched
#                (guest/azmod/spin_back.c)
#   bcols        Column Edit's columns: only those the DJ has set, on full-width lists, only while its
#                row is on, and EP147's own back when it goes off; PREVIEW and
#                the sub column remembered; the stem marks' column from the header
#                (guest/azmod/browse_columns.c, stem_marks.c)
#   browse fields  Column Edit, any field: the field texts, the saved file, the columns that can show
#                one, a pick from a column's button, the buttons placed clear of the dividers and of
#                each other, a press near a divider the header's, the field list moved under the
#                column, a cell drawn by EP147's sub column painter and put back, a touch on Preview
#                the row's, the worker over fake sticks
#                (guest/azmod/browse_fields.c, browse_fields.h)
#   htcover      Hide Track Title takes a press only when JUCE's hit test gives it to that deck's
#                bar, never one on a popup over it (guest/azmod/ht_cover.h)
#   key in title Key in Track Title: the key's box between the title and the eye or the row's end,
#                the tray key's copy (hidden = none, stale = none), each feature behind its own
#                switch in the shared paint hook; tap the key: its tap target, the tap's owner (the
#                key or the title, never both), the key shift panel's place (guest/azmod/kt_layout.h)
#   cmbrowse     Community Mods > Browse: the category of a manifest value, the updated date, the
#                four orders; the download count and its text; Home's
#                shelves; a mod page's "updated N days ago" (guest/azmod/cm_browse.h)
#   cmupd        Community Mods > Updates, Update all: the next install only after the last is done and
#                the catalog read again, never while busy; an error stops it; a mod that stopped
#                qualifying is skipped; the button's text (guest/azmod/cm_updall.h)
#   cmupdtab     Community Mods > Updates: the rail's tab and count, the mods listed (those the Installed
#                list marks "Update available"), a row's Update, Update all driven from the overlay's
#                tick, an error stopping it, a reload ending it (guest/azmod/screen_community.c)
#   mm resume    the Mod Menu shows again when SOURCE comes back, as it was when SOURCE was left;
#                SOURCE with the menu up or a real source picked forgets it (guest/azmod/mm_resume.h)
#   glide        a scrolled list stays under the finger (no lag), flings on a moving lift, stops at
#                the ends and on a finger, glides a knob step (guest/azmod/scroll_glide.h)
#   inputmem     Channel Input Select Memory at start: the SETTINGS.DAT reader, and a channel left
#                at DECK set to its saved input once, never the DJ's own choice (input_memory.h)
#   sdk*         community mod services: audio_tap's deck channels and closed-stream guard; launch
#                only the mod's own files, without LD_PRELOAD, its
#                own pids only; handoff's guards; reload refused with a track loaded; the library
#                read-only, USB ports by the deck's bus rule; the launch log cap; claim_vslot's slot checks (guest/azmod/sdk_{proc,lib,vslot}.c)
#   cloexec      every fd azmod/azstem open is close-on-exec (cloexec-lint.sh)
#   dupdefine    no header defines one macro twice at the top level (dupdefine-lint.sh)
#   vinyl save   the Vinyl Speed Adjust value is posted by EP147's message thread and written
#                by the overlay thread: one slot, newest wins (guest/azmod/vinyl_slider.c)
#   ep147-thread no blocking call in azstem's audio hooks or the vinyl touch handler, and
#                every g_sm_mu lock goes through sm_lock() (ep147-thread-lint.sh)
#   exports      the release .so files export exactly the listed symbols (exports.sh)
#   formatting   the sources are formatted by the pinned clang-format 21.1.8 (tools/format-check.sh)
#   static       clang-tidy and cppcheck report nothing unsuppressed over the release units, and a
#                planted use-after-free and buffer overrun are reported (tools/static-check.sh)
#   addrgen      guest/{azmod,azstem}/ep147addr.h equal a fresh tools/ep147-addrgen.py run
#   werror       azmod/azstem/guard compile lines (both Makefiles) fail on a new -Wall or
#                -Wextra warning (werror-check.sh)
#   fuzz         the untrusted-input parsers' libFuzzer harnesses (tools/fuzz): corpora and crash
#                regressions replayed everywhere, a short fuzz where clang-10/libFuzzer exists
#
# Production functions that live inside a .c translation unit are extracted by
# text at run time, never copied, so a suite cannot drift into testing a stale
# duplicate. Each suite is then rebuilt against one or more deliberately broken
# variants (a control) and the control must fail: a test that cannot produce the
# other answer proves nothing. A passing control fails the whole run.
#
# Usage: tools/unit-tests/run.sh      (HOST_CC overrides the compiler)
#   COVERAGE=1 tools/unit-tests/run.sh: line, branch and function coverage (see "coverage pass")
#   AZ_COMPILE_ONLY=1 tools/unit-tests/run.sh: build every test program, run none (see "compile-only pass")
#   In the published source tree: tests/run.sh, which runs this script there.
# Linux (the full suite, arm64): tools/unit-tests/run-linux.sh <a new directory in your home
#   folder> builds docker/azmod-test.Dockerfile as azmod-test:18.04 if missing (azmod-build:18.04
#   + sqlite3-dev, python3, git, docker.io, rsync), clones HEAD there and runs this script in it
#   with the docker socket mounted (some suites start their own containers).
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
T=tools/unit-tests
CC=${HOST_CC:-cc}
CFLAGS=(-std=gnu11 -O1 -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable)
OUT=$(mktemp -d "${TMPDIR:-/tmp}/az-unit-tests.XXXXXX")
FD_SCRATCH=
trap 'rm -rf "$OUT" ${FD_SCRATCH:+"$FD_SCRATCH"}' EXIT

# AZ_PUBLIC_TREE=1 (set by the published tree's tests/run.sh): the release sources only, as
# published. Checks that need development-build code, the git history or the development
# Makefile are reported as SKIP with the reason, never as a pass.
PUBLIC=${AZ_PUBLIC_TREE:-0}
failures=0
broken_controls=0
skipped=0   # checks and controls that did not run here: printed as SKIP, counted in the summary
co_notrun=0 # suites a compile-only pass built and did not run

# ---------------------------------------------------------------- sanitizer passes
# SAN=asan (AddressSanitizer + UndefinedBehaviorSanitizer) or SAN=tsan (ThreadSanitizer) rebuilds
# every host suite with the sanitizer and runs it; run-linux.sh runs both after the plain pass.
#  - Every $CC call goes through a wrapper that appends the flags, so the suites, their objects
#    and the harnesses are instrumented alike. -fwrapv is dropped there: the release build does not
#    use it, so signed overflow in the real code is reported as it would occur on the deck.
#  - Controls (the mutated copies) are built but not run: whether a planted logic bug makes a suite
#    fail is the plain pass's question, and a sanitizer abort would answer it for the wrong reason.
#    The lints, goldens of non-C tools, docker-built aarch64 checks and the release-library checks
#    test no host code and run in the plain pass only.
#  - The pass has its own controls (sanplant_test.c): a planted heap overflow and signed overflow
#    (asan) or data race (tsan) must each be reported by the sanitizer itself, and a clean build of
#    the same file must pass, in the same round.
#  - Every report from any process (a suite's children too) lands in $OUT/san/report.*: a suite
#    that leaves one fails even when its exit code says 0.
#  - Suppressions: tools/unit-tests/sanitizer-<asan|ubsan|lsan|tsan>.supp, read when present; each
#    entry carries its written reason.
SAN=${SAN:-}
SANFLAGS=()
case $SAN in
    "") ;;
    asan) SANFLAGS=(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g) ;;
    tsan) SANFLAGS=(-fsanitize=thread -g) ;;
    *) echo "SAN=$SAN: use asan or tsan"; exit 2 ;;
esac
if [ -n "$SAN" ]; then
    [ "$(uname -s)" = Linux ] || { echo "SAN=$SAN runs on Linux (tools/unit-tests/run-linux.sh)"; exit 2; }
    mkdir -p "$OUT/san"
    SANREAL=$(command -v "$CC") || { echo "SAN=$SAN: compiler $CC not found"; exit 2; }
    { echo '#!/usr/bin/env bash'
      echo 'args=(); for a in "$@"; do [ "$a" = -fwrapv ] || args+=("$a"); done'
      printf 'exec %q "${args[@]}"' "$SANREAL"; printf ' %q' "${SANFLAGS[@]}"; echo; } > "$OUT/san/cc"
    chmod +x "$OUT/san/cc"
    CC=$OUT/san/cc
    SANLOG=$OUT/san/report
    # supp <tool> : the suppressions option for tools/unit-tests/sanitizer-<tool>.supp when it exists
    supp() { [ ! -f "$ROOT/$T/sanitizer-$1.supp" ] || echo ":suppressions=$ROOT/$T/sanitizer-$1.supp"; }
    # halt at the first report: a suite that raced or overflowed has failed, whatever it printed next
    # (SAN_HALT=0 lets each suite run on and print every report, to survey findings; it still fails)
    H=${SAN_HALT:-1}
    export ASAN_OPTIONS="log_path=$SANLOG:halt_on_error=$H:detect_leaks=1:exitcode=86$(supp asan)"
    export UBSAN_OPTIONS="log_path=$SANLOG:print_stacktrace=1:halt_on_error=$H$(supp ubsan)"
    export LSAN_OPTIONS="print_suppressions=0$(supp lsan)"
    export TSAN_OPTIONS="log_path=$SANLOG:halt_on_error=$H:exitcode=86:second_deadlock_stack=1$(supp tsan)"
    echo "SANITIZER PASS: $SAN ($SANREAL ${SANFLAGS[*]})"
    echo
fi
# ---------------------------------------------------------------- coverage pass
# COVERAGE=1 measures the line, branch and function coverage of the host suites over the guest
# sources (clang source-based coverage; tools/unit-tests/coverage-report.py documents the method).
# As in a sanitizer pass, every $CC call goes through a wrapper that adds the flags, and the
# controls are built but not run: a mutated copy's coverage is not the suites'. The report lands in
# COVERAGE_DIR (default guest/out/coverage). Needs clang and llvm-profdata/llvm-cov (LLVM 12 or
# later, for branches): found through xcrun on macOS, else on PATH, or named by LLVM_PROFDATA and
# LLVM_COV. The denominator (every line of the release build, tested or not) is compiled from the
# release translation units with the headers of azmod-build:18.04; without docker the report
# covers only the code the suites compile. The suites clang cannot build (the Linux-only ones,
# the aarch64 hook tests, the Community Mods host) are measured by gcc --coverage in the
# containers (tools/unit-tests/coverage-gcc.sh, with its own two controls) and merged by line.
COVERAGE=${COVERAGE:-}
[ "$COVERAGE" != 0 ] || COVERAGE=
if [ -n "$COVERAGE" ]; then
    [ -z "$SAN" ] || { echo "COVERAGE=1 is its own pass: unset SAN"; exit 2; }
    "$CC" --version 2>/dev/null | grep -q clang || { echo "COVERAGE=1 needs clang (HOST_CC=$CC is not)"; exit 2; }
    covtool() { # covtool <name> : the LLVM tool's path, or nothing
        if command -v xcrun >/dev/null 2>&1 && xcrun --find "$1" >/dev/null 2>&1; then xcrun --find "$1"; return; fi
        local c
        for c in "$1" $(compgen -c "$1-" | sort -t- -k3 -n -r); do
            command -v "$c" >/dev/null 2>&1 && { command -v "$c"; return; }
        done
    }
    LLVM_PROFDATA=${LLVM_PROFDATA:-$(covtool llvm-profdata)}
    LLVM_COV=${LLVM_COV:-$(covtool llvm-cov)}
    [ -n "$LLVM_PROFDATA" ] && [ -n "$LLVM_COV" ] || { echo "COVERAGE=1: llvm-profdata and llvm-cov not found"; exit 2; }
    COVDIR=${COVERAGE_DIR:-$ROOT/guest/out/coverage}
    # the profiles stay in the scratch directory: suites rebuild guest/out (exports.sh) mid-run
    mkdir -p "$OUT/cov/prof"
    COVREAL=$(command -v "$CC")
    printf '#!/usr/bin/env bash\nexec %q "$@" -fprofile-instr-generate -fcoverage-mapping\n' "$COVREAL" > "$OUT/cov/cc"
    chmod +x "$OUT/cov/cc"
    CC=$OUT/cov/cc
    export FZ_CC=$CC # the fuzz harnesses' corpus replays (tools/fuzz/lib.sh) count too
    export LLVM_PROFILE_FILE="$OUT/cov/prof/%p-%m.profraw"
    echo "COVERAGE PASS ($COVREAL; report in $COVDIR)"
    echo
fi
# ---------------------------------------------------------------- compile-only pass
# AZ_COMPILE_ONLY=1 builds every test program and control variant with the host compiler and runs
# nothing: suite() and control() run no binary, and the checks that are not host compiles (lints,
# goldens, the static analysers, the formatter, the release-library checks, everything that starts
# a container) are not run. Builds that need a container or aarch64 Linux are reported as SKIP with
# the reason. Every $CC call goes through a wrapper that records a failed compile (its sources, its
# output and the compiler's messages) and lets the script go on, so one round names every program
# that does not compile; the summary then fails. A compile that only probes for a compiler feature
# (AZ_CC_PROBE=1) keeps its own exit status and is not recorded. The wrapper builds at -O0: no test
# build uses -Werror, so the optimizer adds only warnings, never an error, and the pass takes half
# the time.
COMPILE_ONLY=${AZ_COMPILE_ONLY:-0}
[ "$COMPILE_ONLY" != 0 ] || COMPILE_ONLY=
if [ -n "$COMPILE_ONLY" ]; then
    [ -z "$SAN" ] && [ -z "$COVERAGE" ] || { echo "AZ_COMPILE_ONLY=1 is its own pass: unset SAN and COVERAGE"; exit 2; }
    export AZ_COMPILE_ONLY=1   # the golden scripts build their harness and stop
    mkdir -p "$OUT/co"
    : > "$OUT/co/built"; : > "$OUT/co/failed"; : > "$OUT/co/errors"
    COREAL=$(command -v "$CC") || { echo "AZ_COMPILE_ONLY=1: compiler $CC not found"; exit 2; }
    cat > "$OUT/co/cc" <<EOF
#!/usr/bin/env bash
real=$(printf %q "$COREAL"); co=$(printf %q "$OUT/co"); top=$(printf %q "$OUT")
EOF
    cat >> "$OUT/co/cc" <<'EOF'
err=$(mktemp "$co/err.XXXXXX"); rc=0
args=(); for a in "$@"; do case $a in -O[1-3s]) a=-O0 ;; esac; args+=("$a"); done
"$real" "${args[@]}" 2> "$err" || rc=$?
cat "$err" >&2
[ -n "${AZ_CC_PROBE:-}" ] && { rm -f "$err"; exit "$rc"; }
srcs=; out=; prev=
for a in "$@"; do
    case $a in *.c) srcs="$srcs ${a##*/}" ;; esac
    [ "$prev" = -o ] && out=${a#"$top"/}
    prev=$a
done
if [ "$rc" -eq 0 ]; then
    echo "${srcs# } -> $out" >> "$co/built"
else
    echo "COMPILE FAILED  ${srcs# } (-o $out, exit $rc)" | tee -a "$co/failed" >&2
    { echo "COMPILE FAILED  ${srcs# } (-o $out, exit $rc)"; grep -iE 'error|undefined' "$err" | head -12; } >> "$co/errors"
fi
rm -f "$err"
exit 0
EOF
    chmod +x "$OUT/co/cc"
    CC=$OUT/co/cc
    echo "COMPILE-ONLY PASS ($COREAL): every test program built, none run"
    # the instrument's own control: a program using an undeclared name must be recorded as failed
    printf 'int main(void) { return planted_undeclared; }\n' > "$OUT/co/planted_test.c"
    "$CC" -o "$OUT/co/planted" "$OUT/co/planted_test.c" 2>/dev/null
    if grep -q '^COMPILE FAILED  planted_test\.c ' "$OUT/co/failed" && grep -q 'planted_undeclared' "$OUT/co/errors"; then
        echo "CONTROL FAILED AS REQUIRED  compile-only: a program using an undeclared name is recorded with its error"
    else
        echo "BROKEN TEST: control PASSED  compile-only: a program using an undeclared name was not recorded"
        broken_controls=$((broken_controls + 1))
    fi
    : > "$OUT/co/built"; : > "$OUT/co/failed"; : > "$OUT/co/errors"
    echo
fi
# co_skip <what> : 0 (and a SKIP line naming it) in the compile-only pass, else 1
co_skip() {
    [ -n "$COMPILE_ONLY" ] || return 1
    skip "compile-only: $1"
}
# san_reports : moves the reports left since the last call to the log, 0 when there were none
san_reports() {
    [ -n "$SAN" ] || return 0
    local f found=0
    for f in "$SANLOG".*; do
        [ -e "$f" ] || continue
        found=1
        echo "---- sanitizer report ($(basename "$f"))"
        cat "$f"
        rm -f "$f"
    done
    [ "$found" = 0 ]
}
# plain_only <what> : 0 in the plain pass; in a sanitizer pass prints that <what> runs in the plain
# pass (not a skip: it ran there) and returns 1
plain_only() {
    if [ -n "$COMPILE_ONLY" ]; then
        echo "(compile-only pass: $1 not run)"
        return 1
    fi
    [ -z "$SAN" ] && return 0
    echo "(sanitizer pass: $1 runs in the plain pass)"
    return 1
}

# skip <what> : a check or control that cannot run on this host. Never a pass, never "caught".
skip() {
    echo "SKIP  $*"
    skipped=$((skipped + 1))
}

md5_of() {
    if command -v md5sum >/dev/null 2>&1; then md5sum "$1" | cut -c1-32; else md5 -q "$1"; fi
}

# extract <file> <regex for the first line of the definition> -> stdout
# Prints from the matching line to the first line that is exactly "}".
extract() {
    RE="$2" awk '$0 ~ ENVIRON["RE"] {p=1} p {print} p && /^}$/ {exit}' "$1"
}

# mutate <src> <dst> <perl substitution> : fails if the substitution changed nothing,
# so a control can never silently become a copy of the real code.
mutate() {
    perl -0pe "$3" "$1" > "$2"
    if cmp -s "$1" "$2"; then
        echo "MUTATION DID NOT APPLY to $1: $3 (did the code change?)"
        exit 2
    fi
}

# menu_variant <dir> [part...] : a copy of guest/azmod/menu.c in <dir>, for a variant whose mutated
# parts are written beside it. menu.c is one translation unit of #included parts and a quoted
# #include searches the including file's directory first, so the copy compiles the parts in <dir>
# and takes the rest from -Iguest/azmod. Fails if menu.c no longer #includes a named part, so a
# mutated copy can never sit unused while the real part is what gets compiled.
menu_variant() {
    local dir=$1; shift
    mkdir -p "$dir"; cp guest/azmod/menu.c "$dir/menu.c"
    for p in "$@"; do
        grep -q "^#include \"$p\"" guest/azmod/menu.c ||
            { echo "menu.c does not #include $p: $dir would test the real one"; exit 2; }
    done
}

# suite <label> <binary> [args...] : must exit 0
suite() {
    local label=$1 bin=$2; shift 2
    local log="$OUT/suite.log" rc=0
    if [ -n "$COMPILE_ONLY" ]; then
        co_notrun=$((co_notrun + 1))
        return 0
    fi
    echo "=== $label ==="
    "$bin" "$@" > "$log" 2>&1 || rc=$?
    cat "$log"
    if ! san_reports || { [ -n "$SAN" ] && grep -qE '^[^ ]*: runtime error: |^WARNING: ThreadSanitizer|^==[0-9]+==ERROR: ' "$log"; }; then
        echo "FAIL  $label: the sanitizer reported (above)"
        rc=1
    fi
    [ "$rc" -eq 0 ] || failures=$((failures + 1))
    # a case the binary itself skipped (its own SKIP line) is counted like any other skip
    skipped=$((skipped + $(grep -c '^SKIP' "$log" || true)))
    echo
}

# control <label> <binary> [args...] : must exit nonzero
# <binary> is a program (path or PATH name) or a shell function of this script (hooktest,
# alttest, azrs_log_test, ...): a function is called, never exec'd (exec of a function name is
# "command not found", exit 127, which would read as a control that failed). A control that
# cannot run (its binary not built on this host, or exit 126/127) is a SKIP, not caught.
control() {
    [ -z "$SAN" ] || return 0 # the plain pass runs the controls (see "sanitizer passes")
    [ -z "$COVERAGE" ] || return 0 # and only the plain pass (see "coverage pass")
    [ -z "$COMPILE_ONLY" ] || return 0 # built above, never run (see "compile-only pass")
    local label=$1 bin=$2; shift 2
    local log="$OUT/control.log" rc=0 fn=0
    if declare -F "$bin" >/dev/null; then
        fn=1
    elif { [[ $bin == */* ]] && [ ! -x "$bin" ]; } || { [[ $bin != */* ]] && ! command -v "$bin" >/dev/null 2>&1; }; then
        skip "CONTROL NOT RUN (not counted as caught)  $label: $bin is not built on this host"
        return
    fi
    # a control is expected to crash: no core file (it would land in the working directory)
    if [ "$fn" = 1 ]; then
        ( ulimit -c 0; "$bin" "$@" ) > "$log" 2>&1 || rc=$?
    else
        ( ulimit -c 0; exec "$bin" "$@" ) > "$log" 2>&1 || rc=$?
    fi
    if [ "$rc" -eq 126 ] || [ "$rc" -eq 127 ]; then
        skip "CONTROL NOT RUN (not counted as caught)  $label: exit $rc: a command it needs was not found or not executable"
        tail -3 "$log" | sed 's/^/      /'
    elif [ "$rc" -ne 0 ]; then
        echo "CONTROL FAILED AS REQUIRED  $label ($(grep -c '^FAIL' "$log" || true) cases failed; exit $rc)"
        grep '^FAIL' "$log" | head -4 | sed 's/^/      /' || true   # a control that crashed has no FAIL lines
    else
        echo "BROKEN TEST: control PASSED  $label: the suite cannot detect this bug"
        broken_controls=$((broken_controls + 1))
    fi
}

# ---------------------------------------------------------------- the sanitizer's own controls
# (sanitizer passes only, see "sanitizer passes" above) san_control <label> <report regex> <bin>
# <args>: the planted bug must make <bin> exit nonzero and leave a report matching the regex.
san_control() {
    local label=$1 want=$2 bin=$3; shift 3
    local rc=0 rep
    mkdir -p "$OUT/sanplant"
    "$bin" "$@" > "$OUT/sanplant/out.log" 2>&1 || rc=$?
    # gcc's UBSan writes to stderr even with log_path set when it runs beside ASan: read both
    rep=$(cat "$SANLOG".* "$OUT/sanplant/out.log" 2>/dev/null || true); rm -f "$SANLOG".*
    if [ "$rc" -ne 0 ] && grep -qE "$want" <<<"$rep"; then
        echo "CONTROL FAILED AS REQUIRED  $label (exit $rc; $(grep -m1 -oE "$want" <<<"$rep"))"
    else
        echo "BROKEN TEST: control PASSED  $label: exit $rc, no report matching '$want'"
        { sed 's/^/      /' "$OUT/sanplant/out.log" | head -5; } || true # head may stop sed (pipefail)
        broken_controls=$((broken_controls + 1))
    fi
}
if [ -n "$SAN" ]; then
    mkdir -p "$OUT/sanplant"
    $CC "${CFLAGS[@]}" -pthread -o "$OUT/sanplant/t" $T/sanplant_test.c
    suite "sanitizer instrument ($SAN): clean run" "$OUT/sanplant/t" clean
    echo "=== sanitizer instrument ($SAN): planted bugs (each must be reported) ==="
    if [ "$SAN" = asan ]; then
        san_control "ASan: heap overflow one byte past a 24-byte block" 'AddressSanitizer: heap-buffer-overflow' "$OUT/sanplant/t" heap 24
        san_control "UBSan: INT_MAX + 3" 'runtime error: signed integer overflow' "$OUT/sanplant/t" signed 3
    else
        san_control "TSan: two threads increment one plain int" 'ThreadSanitizer: data race' "$OUT/sanplant/t" race 100000
    fi
    echo
fi

# ---------------------------------------------------------------- ep147build
mkdir -p "$OUT/epb/real" "$OUT/epb/ctl-md5" "$OUT/epb/ctl-nocompare"
cp guest/common/ep147build.h "$OUT/epb/real/"
# POSIX awk only (mawk has no {n} intervals) and any spacing ('{ "md5"' or '{"md5"'):
# an EPB_KNOWN row is an opening brace, a quoted 32-hex md5, then the size.
sizes=$(awk -F, '/^[ \t]*\{[ \t]*"[0-9a-f]+"[ \t]*,/ {
    md5 = $1; gsub(/[^0-9a-f]/, "", md5); if (length(md5) != 32) next
    s = $2; gsub(/[ \tL]/, "", s); print s }' guest/common/ep147build.h)
set -- $sizes
[ $# -ge 2 ] || { echo "could not read the EPB_KNOWN sizes from ep147build.h"; exit 2; }
# Sparse files with a known build's size but not its content (all zeros).
dd if=/dev/null of="$OUT/epb/size1" bs=1 seek="$1" 2>/dev/null
dd if=/dev/null of="$OUT/epb/size2" bs=1 seek="$2" 2>/dev/null
printf 'not an EP147\n' > "$OUT/epb/small"
EPB_ARGS=("$OUT/epb/size1" "$(md5_of "$OUT/epb/size1")" "$OUT/epb/size2" "$(md5_of "$OUT/epb/size2")" "$OUT/epb/small")

# control 1: one md5 round constant wrong -> RFC vectors must fail
mutate guest/common/ep147build.h "$OUT/epb/ctl-md5/ep147build.h" 's/0xd76aa478/0xd76aa479/'
# control 2: identify by size alone (md5 comparison dropped) -> the size-matched
# zero file must then be accepted, which the suite must catch
mutate guest/common/ep147build.h "$OUT/epb/ctl-nocompare/ep147build.h" 's/ && !strcmp\(EPB_KNOWN\[k\]\.md5, hex\)//'
for v in real ctl-md5 ctl-nocompare; do
    $CC "${CFLAGS[@]}" -I"$OUT/epb/$v" -I$T -o "$OUT/epb/$v/t" $T/ep147build_test.c
done
suite "ep147build" "$OUT/epb/real/t" "${EPB_ARGS[@]}"

# ---------------------------------------------------------------- addr_init
for lib in azmod azstem; do
    d="$OUT/addr-$lib"; mkdir -p "$d"
    hdr=guest/$lib/ep147addr.h
    extract "$hdr" '^static int ep147_addr_init\(' |
        perl -pe 's/\bep147_addr_init\b/synth_init/g; s/\bep147_addr_v130\b/synth_v130/g;
                  s/\bep147_addr_v200\b/synth_v200/g; s/\bep147_addr\b/synth_addr/g; s/\bEPI_N\b/SYNTH_N/g' \
        > "$d/real.inc"
    grep -q 'synth_init' "$d/real.inc" || { echo "extraction of ep147_addr_init from $hdr failed"; exit 2; }
    # control 1: the completeness check removed
    mutate "$d/real.inc" "$d/ctl-nocheck.inc" 's/\n[^\n]*if\(!t\[i\]\) return 0;[^\n]*//'
    # control 2: an unknown build falls back to the 1.30 table
    mutate "$d/real.inc" "$d/ctl-fallback.inc" 's/: 0;/: synth_v130;/'
    # control 3: check and copy merged, so a hole leaves a partial table behind
    mutate "$d/real.inc" "$d/ctl-partial.inc" \
        's/for\(int i = 0; i < SYNTH_N; i\+\+\)\{ if\(!t\[i\]\) return 0; \}\n(\s*)for\(int i = 0; i < SYNTH_N; i\+\+\) synth_addr\[i\] = t\[i\];/for(int i = 0; i < SYNTH_N; i++){ if(!t[i]) return 0; synth_addr[i] = t[i]; }/'
    for v in real ctl-nocheck ctl-fallback ctl-partial; do
        $CC "${CFLAGS[@]}" -I"$ROOT" -I$T -DADDR_HEADER="\"$hdr\"" -DSYNTH_INC="\"$d/$v.inc\"" \
            -DSUITE_NAME="\"addr_init/$lib\"" -o "$d/$v" $T/addr_init_test.c
    done
    suite "addr_init ($lib)" "$d/real"
done

# ---------------------------------------------------------------- crc16_x25
d="$OUT/crc"; mkdir -p "$d"
extract guest/azmod/bus_interpose.c '^static uint16_t crc16_x25\(' > "$d/real.inc"
grep -q '0x8408' "$d/real.inc" || { echo "extraction of crc16_x25 failed"; exit 2; }
mutate "$d/real.inc" "$d/ctl-poly.inc"    's/0x8408/0x1021/'           # unreflected polynomial
mutate "$d/real.inc" "$d/ctl-xorout.inc"  's/return crc \^ 0xFFFF;/return crc;/'  # final xor dropped
mutate "$d/real.inc" "$d/ctl-init.inc"    's/uint16_t crc = 0xFFFF;/uint16_t crc = 0;/'  # init 0
for v in real ctl-poly ctl-xorout ctl-init; do
    $CC "${CFLAGS[@]}" -I$T -DCRC_INC="\"$d/$v.inc\"" -o "$d/$v" $T/crc_test.c
done
suite "crc16_x25" "$d/real"

# ---------------------------------------------------------------- clock_tz (the Clock readout's zone)
# guest/azmod/clock_tz.h natively against the deck's zone files, and copies with one rule broken.
d="$OUT/clocktz"
for v in real ctl-edge ctl-rule ctl-order ctl-12h ctl-half ctl-sec24 ctl-sec12; do mkdir -p "$d/$v"; done
cp guest/azmod/clock_tz.h "$d/real/"
mutate "$d/real/clock_tz.h" "$d/ctl-edge/clock_tz.h"  's/if \(z->at\[mid\] <= t\)/if (z->at[mid] < t)/'
mutate "$d/real/clock_tz.h" "$d/ctl-rule/clock_tz.h"  's/    return i != n;\n/    return 0;\n/'
mutate "$d/real/clock_tz.h" "$d/ctl-order/clock_tz.h" 's/ \|\| \(i > 0 && t <= z->at\[i - 1\]\)//'
mutate "$d/real/clock_tz.h" "$d/ctl-12h/clock_tz.h"   's/tm.tm_hour % 12 \? tm.tm_hour % 12 : 12/tm.tm_hour % 12/'
mutate "$d/real/clock_tz.h" "$d/ctl-half/clock_tz.h"  's/    if \(m\)\n        snprintf/    if (0)\n        snprintf/'
mutate "$d/real/clock_tz.h" "$d/ctl-sec24/clock_tz.h" 's/if \(h24 && secs\)/if (h24 \&\& 0)/'
mutate "$d/real/clock_tz.h" "$d/ctl-sec12/clock_tz.h" 's/else if \(secs\)/else if (0)/'
for v in real ctl-edge ctl-rule ctl-order ctl-12h ctl-half ctl-sec24 ctl-sec12; do
    $CC "${CFLAGS[@]}" -I"$d/$v" -I$T -o "$d/$v/t" $T/clocktz_test.c
done
suite "clock_tz" "$d/real/t" $T/fixtures/zoneinfo

# the Clock in the place of EP147's status bar readout: guest/azmod/clock_native.h, and copies
# with one rule broken
d="$OUT/clocknative"
for v in real ctl-rec ctl-unknown ctl-label ctl-origin; do mkdir -p "$d/$v"; done
cp guest/azmod/clock_native.h "$d/real/"
mutate "$d/real/clock_native.h" "$d/ctl-rec/clock_native.h"     's/return clock_on && label_ok && icon == 0;/return clock_on \&\& label_ok \&\& icon != -1;/'
mutate "$d/real/clock_native.h" "$d/ctl-unknown/clock_native.h" 's/return clock_on && label_ok && icon == 0;/return clock_on \&\& label_ok \&\& icon < 1;/'
mutate "$d/real/clock_native.h" "$d/ctl-label/clock_native.h"   's/return clock_on && label_ok && icon == 0;/return clock_on \&\& icon == 0;/'
mutate "$d/real/clock_native.h" "$d/ctl-origin/clock_native.h"  's/    if \(x < 0 \|\| y < 0 \|\| w <= 0/    if (x <= 0 || y <= 0 || w <= 0/'
for v in real ctl-rec ctl-unknown ctl-label ctl-origin; do
    $CC "${CFLAGS[@]}" -I"$d/$v" -I$T -o "$d/$v/t" $T/clocknative_test.c
done
suite "clock native" "$d/real/t"

# Set Time Manually, the pure part: guest/azmod/clock_set.h (with clock_tz.h), and copies with one
# rule broken
d="$OUT/clockset"
for v in real ctl-carry ctl-leap ctl-gap ctl-valid; do mkdir -p "$d/$v"; cp guest/azmod/clock_tz.h "$d/$v/"; done
cp guest/azmod/clock_set.h "$d/real/"
mutate "$d/real/clock_set.h" "$d/ctl-carry/clock_set.h" 's/int n = hi - lo \+ 1, r = \(f->v\[k\] - lo \+ d % n \+ n\) % n;/int n = hi - lo + 1, r = f->v[k] - lo + d; (void) n;/'
mutate "$d/real/clock_set.h" "$d/ctl-leap/clock_set.h"  's/ \|\| y % 400 == 0;/;/'
mutate "$d/real/clock_set.h" "$d/ctl-gap/clock_set.h"   's/\*t = l - \(o3 != o2 \? o3 : o2\);/*t = l - o2;/'
mutate "$d/real/clock_set.h" "$d/ctl-valid/clock_set.h" 's/now >= marker - CS_SLACK\)/1)/'
for v in real ctl-carry ctl-leap ctl-gap ctl-valid; do
    $CC "${CFLAGS[@]}" -I"$d/$v" -I$T -o "$d/$v/t" $T/clockset_test.c
done
suite "clock set" "$d/real/t" $T/fixtures/zoneinfo

# ---------------------------------------------------------------- browse edit (Column Edit)
# guest/azmod/browse_edit.h natively, and against copies with one rule broken each.
d="$OUT/bedit"
BE_V="real ctl-dup ctl-zone ctl-title ctl-minw ctl-giveway ctl-tap ctl-handles ctl-partner ctl-trademax ctl-sclear ctl-sstep"
for v in $BE_V; do mkdir -p "$d/$v"; done
cp guest/azmod/browse_edit.h "$d/real/"
mutate "$d/real/browse_edit.h" "$d/ctl-dup/browse_edit.h"     's/ \|\| o->w\[id\]\)/)/'
mutate "$d/real/browse_edit.h" "$d/ctl-zone/browse_edit.h"    's/bestd = BE_ZONE \+ 1,/bestd = BE_ZONE + 2,/'
mutate "$d/real/browse_edit.h" "$d/ctl-title/browse_edit.h"   's/int lok = l != title_id && /int lok = /'
mutate "$d/real/browse_edit.h" "$d/ctl-minw/browse_edit.h"    's/    if \(w < BE_MINW\)\n        w = BE_MINW;\n//'
mutate "$d/real/browse_edit.h" "$d/ctl-giveway/browse_edit.h" 's/            want\[i\] -= give;/            (void) 0;/'
mutate "$d/real/browse_edit.h" "$d/ctl-tap/browse_edit.h"     's/    if \(!editing\)\n        return BE_TAP_SORT;\n//'
mutate "$d/real/browse_edit.h" "$d/ctl-handles/browse_edit.h" 's/            if \(\(l != title_id && l >= 1/            if (1 || (l != title_id \&\& l >= 1/'
mutate "$d/real/browse_edit.h" "$d/ctl-partner/browse_edit.h" 's/\*partner = lok && rok \? r : 0;/*partner = 0;/'
mutate "$d/real/browse_edit.h" "$d/ctl-trademax/browse_edit.h" 's/    if \(w > hi\)\n        w = hi;\n//'
mutate "$d/real/browse_edit.h" "$d/ctl-sclear/browse_edit.h"   's/out\[0\] = BE_SEARCH_KIDS\[i\].x - \(mv \? sh : 0\);/out[0] = BE_SEARCH_KIDS[i].x;/'
mutate "$d/real/browse_edit.h" "$d/ctl-sstep/browse_edit.h"    's/k <= BE_SEARCH_STEPS; k\+\+/k < 1; k++/'
for v in $BE_V; do
    $CC "${CFLAGS[@]}" -I"$d/$v" -I$T -o "$d/$v/t" $T/bedit_test.c
done
suite "browse edit" "$d/real/t"

# Beat jump into the Extended Lead-In: which targets before the track's start may be jumped to
# (guest/azmod/leadin_jump.h), and copies with one rule broken
d="$OUT/leadin"; mkdir -p "$d"
cp guest/azmod/leadin_jump.h "$d/real.h"
mutate "$d/real.h" "$d/ctl-edge.h"  's/return target >= lj_floor\(cap_ms, rate\) \? LJ_ALLOW : LJ_REFUSE;/return target > lj_floor(cap_ms, rate) ? LJ_ALLOW : LJ_REFUSE;/'
mutate "$d/real.h" "$d/ctl-rate.h"  's/    if \(rate < LJ_RATE_MIN \|\| rate > LJ_RATE_MAX \|\| cap_ms >= 0\)\n        return LJ_REFUSE;\n//'
mutate "$d/real.h" "$d/ctl-start.h" 's/    if \(target >= 0\)\n        return LJ_EP147;/    if (target > 0)\n        return LJ_EP147;/'
mutate "$d/real.h" "$d/ctl-ovf.h"   's/long long f = \(long long\) cap_ms \* rate;/long long f = (long long) (int) cap_ms * (int) (rate * 1000);/'
mutate "$d/real.h" "$d/ctl-unit.h"  's/return \(long\) \(f \/ 1000\);/return (long) (f \/ 1024);/'
for v in real ctl-edge ctl-rate ctl-start ctl-ovf ctl-unit; do
    $CC "${CFLAGS[@]}" -I$T -DLJ_HDR="\"$d/$v.h\"" -o "$d/$v" $T/leadin_jump_test.c
done
suite "leadin jump" "$d/real"

# Remember Waveform Zoom: the saved text, the six levels, when a zoom is restored and when a new one
# is saved (guest/azmod/wave_zoom.h), and copies with one rule broken
d="$OUT/wzsave"; mkdir -p "$d"
cp guest/azmod/wave_zoom.h "$d/real.h"
mutate "$d/real.h" "$d/ctl-level.h"  's/d == 8 \|\| d == 16\);/d == 8 || d == 16 || d == 32);/'
mutate "$d/real.h" "$d/ctl-stock.h"  's/return live == WZ_STOCK && wz_level_ok\(saved\);/return wz_level_ok(saved);/'
mutate "$d/real.h" "$d/ctl-settle.h" 's/now_ms - w->since_ms >= WZ_SETTLE_MS;/now_ms - w->since_ms >= 0;/'
mutate "$d/real.h" "$d/ctl-zero.h"   's/if \(\*s < .1. \|\| \*s > .9.\) \/\* no sign/if (*s < 0x30 || *s > 0x39) \/* no sign/'
mutate "$d/real.h" "$d/ctl-tail.h"   's/    if \(\*s\)\n        return WZ_NONE;\n//'
mutate "$d/real.h" "$d/ctl-same.h"   's/live != saved && now_ms/now_ms/'
for v in real ctl-level ctl-stock ctl-settle ctl-zero ctl-tail ctl-same; do
    $CC "${CFLAGS[@]}" -I$T -DWZ_HDR="\"$d/$v.h\"" -o "$d/$v" $T/wzsave_test.c
done
suite "remember wave zoom" "$d/real"

# ---------------------------------------------------------------- sdk drawing logic
# guest/azmod/sdk_surface_logic.h natively, and against copies with one rule broken each.
d="$OUT/sdkdraw"
for v in real ctl-limit ctl-blend ctl-view ctl-long ctl-drop ctl-xwinsize ctl-blendwrap ctl-xwinowned ctl-draglong; do mkdir -p "$d/$v"; done
cp guest/azmod/sdk_surface_logic.h "$d/real/"
mutate "$d/real/sdk_surface_logic.h" "$d/ctl-limit/sdk_surface_logic.h" 's/if \(mine >= SDK_SURF_PER_MOD\)/if (mine > SDK_SURF_PER_MOD)/'
mutate "$d/real/sdk_surface_logic.h" "$d/ctl-blend/sdk_surface_logic.h" 's/unsigned ia = 255u - a;/unsigned ia = 255u;/'
mutate "$d/real/sdk_surface_logic.h" "$d/ctl-view/sdk_surface_logic.h" 's/if \(deck_view\)\n        return AZ_VIEW_DECK;\n    if \(search\)\n        return AZ_VIEW_SEARCH;/if (search)\n        return AZ_VIEW_SEARCH;\n    if (deck_view)\n        return AZ_VIEW_DECK;/'
mutate "$d/real/sdk_surface_logic.h" "$d/ctl-long/sdk_surface_logic.h" 's/if \(p->down && !p->long_sent && /if (p->down \&\& /'
mutate "$d/real/sdk_surface_logic.h" "$d/ctl-drop/sdk_surface_logic.h" 's/__atomic_add_fetch\(&q->dropped, 1,/__atomic_add_fetch(\&q->dropped, 2,/'
mutate "$d/real/sdk_surface_logic.h" "$d/ctl-xwinsize/sdk_surface_logic.h" 's/\*keep = r\.w <= 0 \|\| r\.h <= 0;/*keep = 0;/'
mutate "$d/real/sdk_surface_logic.h" "$d/ctl-blendwrap/sdk_surface_logic.h" 's/long long x1 = X \+ w > fw \? fw - X : w, y1 = Y \+ h > fh \? fh - Y : h;/long long x1 = (int) (x + w) > fw ? fw - X : w, y1 = (int) (y + h) > fh ? fh - Y : h;/'
mutate "$d/real/sdk_surface_logic.h" "$d/ctl-xwinowned/sdk_surface_logic.h" 's/    if \(!owned\)\n        return SDK_XW_NOTOWNED;[^\n]*\n//'
mutate "$d/real/sdk_surface_logic.h" "$d/ctl-draglong/sdk_surface_logic.h" 's/        p->long_sent = 1;\n\}/        (void) 0;\n}/'
for v in real ctl-limit ctl-blend ctl-view ctl-long ctl-drop ctl-xwinsize ctl-blendwrap ctl-xwinowned ctl-draglong; do
    $CC "${CFLAGS[@]}" -fwrapv -pthread -I"$d/$v" -Iguest/azmod -I$T -o "$d/$v/t" $T/sdkdraw_test.c
done
suite "sdk drawing" "$d/real/t"

# ---------------------------------------------------------------- Bright Grid
# guest/azmod/brightgrid.h natively, and against copies with one rule broken each.
d="$OUT/brightgrid"
for v in real ctl-oldrule ctl-ticks; do mkdir -p "$d/$v"; done
cp guest/azmod/brightgrid.h "$d/real/"
mutate "$d/real/brightgrid.h" "$d/ctl-oldrule/brightgrid.h" 's/return !\(px & 0x00ffffffu\) \|\| px == bg;/return !(px \& 0x00ffffffu);/'
mutate "$d/real/brightgrid.h" "$d/ctl-ticks/brightgrid.h" 's/int v = \(int\) \(\(float\) i \* gain \+ 0\.5f\);/int v = (int) ((float) i * gain);/'
for v in real ctl-oldrule ctl-ticks; do
    $CC "${CFLAGS[@]}" -I"$d/$v" -I$T -o "$d/$v/t" $T/brightgrid_test.c
done
suite "bright grid" "$d/real/t"

# ---------------------------------------------------------------- Bright Grid's tuning file
# brightgrid_poll_cfg (waveform.c) natively against a file in a scratch dir; brightcfg_wiring:
# the poll runs from the overlay's 1 Hz poll, and nothing in the rates log (written only with
# azmod-logs.on) reads the file.
d="$OUT/brightcfg"; mkdir -p "$d/scratch"
{ grep -E '^#define BG_(GAIN|OD)_DEF ' guest/azmod/waveform.c
  grep -E '^static _Atomic int g_bg_(move_ms|gain_k)' guest/azmod/waveform.c | sed 's/_Atomic //' # clang: __atomic_* on plain ints
  extract guest/azmod/waveform.c '^static void brightgrid_read_cfg\('
  extract guest/azmod/waveform.c '^static void brightgrid_poll_cfg\('
} > "$d/real.inc"
grep -q 'stat(BG_CFG_PATH' "$d/real.inc" || { echo "extraction of brightgrid_poll_cfg failed"; exit 2; }
mutate "$d/real.inc" "$d/ctl-once.inc" 's/if \(seen\.read && present == seen\.present &&/if (seen.read || (present == seen.present \&\&/; s/seen\.nsec == \(long long\) st\.st_mtim\.tv_nsec\)\)\)/seen.nsec == (long long) st.st_mtim.tv_nsec))))/'
for v in real ctl-once; do
    $CC "${CFLAGS[@]}" -I$T -DBRIGHTCFG_INC="\"$d/$v.inc\"" -o "$d/$v" $T/brightcfg_test.c
done
suite "bright grid cfg" "$d/real" "$d/scratch"
brightcfg_wiring() { # $1 = mod_menu_overlay.c -> 0 when only ov_poll_1hz polls the file
    local f fn body
    extract "$1" '^static void ov_poll_1hz\(' | grep -q 'brightgrid_poll_cfg();' ||
        { echo "FAIL  bright grid cfg: ov_poll_1hz does not poll the tuning file"; return 1; }
    for fn in ov_measure_rates ov_rates_display ov_rates_pads ov_rates_wscale ov_rates_disp; do
        body=$(extract "$1" "^static void $fn\\(")
        [ -n "$body" ] || { echo "FAIL  bright grid cfg: $fn not found in $1"; return 1; }
        if grep -q 'brightgrid_' <<<"$body"; then
            echo "FAIL  bright grid cfg: $fn (the rates log, azmod-logs.on only) reads the tuning file"; return 1
        fi
    done
}
echo "=== bright grid cfg wiring ==="
if ! plain_only "bright grid cfg wiring"; then :
elif brightcfg_wiring guest/azmod/mod_menu_overlay.c; then
    echo "PASS  bright grid cfg: polled by ov_poll_1hz, not by the rates log"
else failures=$((failures + 1)); fi
mutate guest/azmod/mod_menu_overlay.c "$d/ctl-logs.c" 's/\n        brightgrid_poll_cfg\(\);//; s/(long gn = __atomic_load_n\(&g_gridaa_ns, __ATOMIC_RELAXED\);\n)/$1    brightgrid_read_cfg();\n/'
mutate guest/azmod/mod_menu_overlay.c "$d/ctl-rates.c" 's/(long gn = __atomic_load_n\(&g_gridaa_ns, __ATOMIC_RELAXED\);\n)/$1    brightgrid_read_cfg();\n/'
echo

# ---------------------------------------------------------------- fd tracking
# The tracking helpers and the open/close/dup2/dup3 interposers, extracted, with "/dev/..."
# rewritten to FDT_DEV "/..." so the test's "devices" are files in a scratch directory.
d="$OUT/fd"; mkdir -p "$d"
# off Linux this suite runs in Docker, which sees the home folder but not the macOS temp folder
[ "$(uname -s)" = Linux ] || { d=$(mktemp -d "$HOME/.az-unit-fd.XXXXXX"); FD_SCRATCH=$d; }
FD_DIR=$d
{ extract guest/azmod/bus_interpose.c '^static int gpiodrv_slot\('
  extract guest/azmod/bus_interpose.c '^static void bus_fd_drop\('
  extract guest/azmod/bus_interpose.c '^static void bus_fd_forget\('
  extract guest/azmod/bus_interpose.c '^static void bus_fd_note\('
} | perl -pe 's{"/dev/}{FDT_DEV "/}g' > "$d/track-real.inc"
extract guest/azmod/bus_interpose.c '^int open\(const char \*path' | perl -pe 's{"/dev/}{FDT_DEV "/}g' > "$d/open-real.inc"
{ extract guest/azmod/bus_interpose.c '^static int \(\*next_close\)'
  extract guest/azmod/bus_interpose.c '^static int \(\*next_dup2\)'
  extract guest/azmod/bus_interpose.c '^static int \(\*next_dup3\)'
} > "$d/hooks-real.inc"
grep -q 'bus_fd_forget(fd);' "$d/track-real.inc" && grep -q 'bus_fd_note(fd, path)' "$d/open-real.inc" &&
    grep -q 'next_dup3(oldfd, newfd, flags)' "$d/hooks-real.inc" || { echo "extraction of the fd tracking code failed"; exit 2; }
# control 1: the old code (open-only tracking: no close/dup2/dup3 hooks, open never forgets)
mutate "$d/track-real.inc" "$d/track-ctl-old.inc" 's/    bus_fd_forget\(fd\);\n    if \(!path\)/    if (!path)/'
: > "$d/hooks-ctl-old.inc"
# control 2..4: one hook does not forget
mutate "$d/hooks-real.inc" "$d/hooks-ctl-close.inc" 's/    if \(!g_azm_inert\)\n        bus_fd_forget\(fd\);\n    return next_close/    return next_close/'
mutate "$d/hooks-real.inc" "$d/hooks-ctl-dup2.inc" 's/bus_fd_forget\(newfd\);\n    return next_dup2/;\n    return next_dup2/'
mutate "$d/hooks-real.inc" "$d/hooks-ctl-dup3.inc" 's/bus_fd_forget\(newfd\);\n    return next_dup3/;\n    return next_dup3/'
# control 5: open() does not forget a number it hands out (an unhooked close goes unnoticed)
cp "$d/track-ctl-old.inc" "$d/track-ctl-note.inc"
# control 6: open() tracks outside EP147 (no inert pass-through)
mutate "$d/open-real.inc" "$d/open-ctl-inert.inc" 's/if \(g_azm_inert\) \{/if (0) {/'
# control 7: the second deck bus recorded in the first's slot
mutate "$d/track-real.inc" "$d/track-ctl-deck2.inc" 's/g_deckbus_fd\[1\] = fd;/g_deckbus_fd[0] = fd;/'
# control 8: a creating open's mode dropped
mutate "$d/open-real.inc" "$d/open-ctl-mode.inc" 's/int fd = next_open\(path, flags, m\);/int fd = next_open(path, flags, 0);/'
FD_VARIANTS="real ctl-old ctl-close ctl-dup2 ctl-dup3 ctl-note ctl-inert ctl-deck2 ctl-mode"
[ "$(uname -s)" = Linux ] || ! co_skip "fd tracking (9 builds): off Linux they build in azmod-build:18.04 (a container)" ||
    FD_VARIANTS=
for v in $FD_VARIANTS; do
    tr="$d/track-real.inc"; op="$d/open-real.inc"; hk="$d/hooks-real.inc"
    case $v in
        ctl-old) tr="$d/track-ctl-old.inc"; hk="$d/hooks-ctl-old.inc";;
        ctl-close|ctl-dup2|ctl-dup3) hk="$d/hooks-$v.inc";;
        ctl-note) tr="$d/track-ctl-note.inc";;
        ctl-inert) op="$d/open-ctl-inert.inc";;
        ctl-deck2) tr="$d/track-ctl-deck2.inc";;
        ctl-mode) op="$d/open-ctl-mode.inc";;
    esac
    mkdir -p "$d/dev-$v"
    if [ "$(uname -s)" = Linux ]; then
        $CC "${CFLAGS[@]}" -Wno-nonnull-compare -I$T -DFDT_DEV="\"$d/dev-$v\"" -DFD_TRACK_INC="\"$tr\"" \
            -DFD_OPEN_INC="\"$op\"" -DFD_HOOKS_INC="\"$hk\"" -o "$d/$v" $T/fd_test.c -ldl
    else
        # dup3 and RTLD_NEXT interposition of libc are Linux behavior: build and run this suite
        # in the release build container (same paths mounted, so the scratch "devices" match)
        docker run --rm --platform=linux/arm64 -v "$ROOT:$ROOT" -v "$d:$d" -w "$ROOT" azmod-build:18.04 \
            gcc "${CFLAGS[@]}" -Wno-nonnull-compare -I$T -DFDT_DEV="\"$d/dev-$v\"" -DFD_TRACK_INC="\"$tr\"" \
            -DFD_OPEN_INC="\"$op\"" -DFD_HOOKS_INC="\"$hk\"" -o "$d/$v.bin" $T/fd_test.c -ldl
        printf '#!/bin/sh\nexec docker run --rm --platform=linux/arm64 -v "%s:%s" azmod-build:18.04 "%s" "$@"\n' \
            "$d" "$d" "$d/$v.bin" > "$d/$v"
        chmod +x "$d/$v"
    fi
done
suite "fd tracking" "$d/real"

# ---------------------------------------------------------------- stem_folder_rank
d="$OUT/stemrank"; mkdir -p "$d"
extract guest/azstem/stem_playback.c '^static int stem_folder_rank\(' > "$d/real.inc"
grep -q 'isxdigit' "$d/real.inc" || { echo "extraction of stem_folder_rank failed"; exit 2; }
mutate "$d/real.inc" "$d/ctl-anyhex.inc"   's/if \(!isxdigit\(\(unsigned char\) t\[i\]\)\)/if (0)/'   # any suffix char accepted
mutate "$d/real.inc" "$d/ctl-prefix.inc"   's/    return -1;\n}\s*$/    return 3;\n}\n/s'                  # prefix match accepted
mutate "$d/real.inc" "$d/ctl-case.inc"     's/strncasecmp/strncmp/'                                     # case-sensitive
for v in real ctl-anyhex ctl-prefix ctl-case; do
    $CC "${CFLAGS[@]}" -I$T -DSTEMRANK_INC="\"$d/$v.inc\"" -o "$d/$v" $T/stemrank_test.c
done
suite "stem_folder_rank" "$d/real"

# ---------------------------------------------------------------- source.json fp
d="$OUT/srcfp"; mkdir -p "$d"
extract guest/azstem/stem_playback.c '^static int source_fp\(' > "$d/real.inc"
grep -q 'isxdigit' "$d/real.inc" || { echo "extraction of source_fp failed"; exit 2; }
# the old rule: whatever strtoull reads, if it ends 16 characters on at a quote
mutate "$d/real.inc" "$d/ctl-strtoull.inc" 's/    for \(int i = 9; i < 25; i\+\+\)\n.*?    return 1;\n/    char *e;\n    *fp = strtoull(q + 9, &e, 16);\n    return e == q + 25 \&\& *e == \x27"\x27;\n/s'
for v in real ctl-strtoull; do
    $CC "${CFLAGS[@]}" -I$T -DSRCFP_INC="\"$d/$v.inc\"" -o "$d/$v" $T/srcfp_test.c
done
suite "source.json fp" "$d/real"

# ---------------------------------------------------------------- the stems' envelope cache
# azstem.env on the stick (stem_envelopes.c env_cache_save / env_cache_load): whole or not at all,
# a cut or impossible file rejected (envcache_test.c)
d="$OUT/envcache"; mkdir -p "$d"
{ grep -E '^#define ENV_(MAGIC|MAX_FRAMES) ' guest/azstem/stem_envelopes.c
  grep -E '^static char g_env_dir\[NDECK\]' guest/azstem/stem_envelopes.c
  extract guest/azstem/stem_envelopes.c '^static void env_cache_path\('
  extract guest/azstem/stem_envelopes.c '^static void env_cache_save\('
  extract guest/azstem/stem_envelopes.c '^static int env_cache_load\('; } > "$d/real.inc"
grep -q 'ENV_MAX_FRAMES' "$d/real.inc" && grep -q 'g_env_dir' "$d/real.inc" && grep -q 'env_cache_load' "$d/real.inc" ||
    { echo "extraction of the envelope cache failed"; exit 2; }
# control: saved in place (a short save destroys the previous cache)
mutate "$d/real.inc" "$d/ctl-inplace.inc" 's/FILE \*f = fopen\(tmp, "wbe"\);/FILE *f = fopen(path, "wbe");/; s/if \(!ok \|\| rename\(tmp, path\) != 0\) \{/if (0) {/'
# control: any frame count taken
mutate "$d/real.inc" "$d/ctl-anyframes.inc" 's/ && fr <= ENV_MAX_FRAMES//'
for v in real ctl-inplace ctl-anyframes; do
    $CC "${CFLAGS[@]}" -I$T -DENVCACHE_INC="\"$d/$v.inc\"" -o "$d/$v" $T/envcache_test.c
done
suite "envelope cache" "$d/real" "$d/scratch-real"

# ---------------------------------------------------------------- the stem offset cache
# azstem.ofs on the stick (ofsload.c ofs_cache_parse / _load / _save, stem_playback.c stems_key):
# a lag is reused only for the same source and stem set, anything else is a miss (ofscache_test.c)
d="$OUT/ofscache"; mkdir -p "$d"
{ awk '/^#define OFSC_MAGIC / {p=1} p {print} p && /^static void ofs_cache_save\(/ {q=1} q && /^}$/ {exit}' guest/azstem/ofsload.c
  extract guest/azstem/stem_playback.c '^static unsigned long long stems_key\('; } > "$d/real.inc"
grep -q 'ofs_cache_parse' "$d/real.inc" && grep -q 'rename(tmp, path)' "$d/real.inc" && grep -q 'FNV-1a' guest/azstem/stem_playback.c &&
    grep -q '1099511628211ULL' "$d/real.inc" || { echo "extraction of the stem offset cache failed"; exit 2; }
# control: the source size is not compared
mutate "$d/real.inc" "$d/ctl-nosize.inc" 's/if \(sz != size \|\| /if ((void) size, /'
# control: the stem set is not compared
mutate "$d/real.inc" "$d/ctl-nokey.inc" 's/ \|\| k != skey/ || ((void) skey, (void) k, 0)/'
# control: any lag taken
mutate "$d/real.inc" "$d/ctl-anylag.inc" 's/ \|\| l < -OFS_LAGMAX \|\| l > OFS_LAGMAX//'
# control: text after the line taken
mutate "$d/real.inc" "$d/ctl-trailing.inc" 's/ \|\| \*p != .\\0.\)/)/'
# control: saved in place (a save that stops short destroys the previous file)
mutate "$d/real.inc" "$d/ctl-inplace.inc" 's/FILE \*f = fopen\(tmp, "we"\);/FILE *f = fopen(path, "we");/; s/if \(!ok \|\| rename\(tmp, path\) != 0\)/if (!ok)/'
for v in real ctl-nosize ctl-nokey ctl-anylag ctl-trailing ctl-inplace; do
    $CC "${CFLAGS[@]}" -I$T -DOFSCACHE_INC="\"$d/$v.inc\"" -o "$d/$v" $T/ofscache_test.c
done
suite "stem offset cache" "$d/real" "$d/scratch-real"

# ---------------------------------------------------------------- Remote Stems request files
# rs_write_req (remote_stems.c): key=value lines the helper reads, the library's text unable to add
# a line of its own (rsreq_test.c)
d="$OUT/rsreq"; mkdir -p "$d"
extract guest/azmod/remote_stems.c '^static int rs_write_req\(' > "$d/real.inc"
grep -q 'file= first' "$d/real.inc" || { echo "extraction of rs_write_req failed"; exit 2; }
# control: the title written as given (a line break in it adds a key line)
mutate "$d/real.inc" "$d/ctl-title.inc" 's/fputc\(\(unsigned char\) \*t < 0x20 \? . . : \*t, f\)/fputc(*t, f)/'
# control: any path written (a line break splits file=, a long one is cut)
mutate "$d/real.inc" "$d/ctl-path.inc" 's/if \(!path \|\| strpbrk\(path, "\\r\\n"\) \|\|\n\s*snprintf\(want, sizeof want, "file=%s\\n", path\) >= \(int\) sizeof want\)\n\s*return -74;/snprintf(want, sizeof want, "file=%s\\n", path);/'
for v in real ctl-title ctl-path; do
    $CC "${CFLAGS[@]}" -I$T -DRSREQ_INC="\"$d/$v.inc\"" -o "$d/$v" $T/rsreq_test.c
done
suite "remote stems request" "$d/real" "$d/scratch-real"

# ---------------------------------------------------------------- Remote Stems queue scan
# rs_queue_has_req / rs_scan_publish / rs_work_take (remote_stems.c): the directory scan runs on
# the stem-marks worker and hands one atomic flag to the message thread (rsscan_test.c). The
# threaded case also runs under ThreadSanitizer, where a plain flag is a reported race.
d="$OUT/rsscan"; mkdir -p "$d"
{ extract guest/azmod/remote_stems.c '^static int rs_queue_has_req\('
  grep -E '^static int g_rs_work;' guest/azmod/remote_stems.c
  extract guest/azmod/remote_stems.c '^static void rs_scan_publish\('
  extract guest/azmod/remote_stems.c '^static int rs_work_take\('
  extract guest/azmod/remote_stems.c '^static void rs_scan_loop\('; } > "$d/real.inc"
grep -q 'rs_scan_loop' "$d/real.inc" && grep -q 'azstems-queue' "$d/real.inc" && grep -q 'g_rs_work' "$d/real.inc" && grep -q 'rs_work_take' "$d/real.inc" ||
    { echo "extraction of the Remote Stems scan failed"; exit 2; }
# control: any file in the queue counts (not only *.req)
mutate "$d/real.inc" "$d/ctl-anyfile.inc" 's/if \(l > 4 && !strcmp\(e->d_name \+ l - 4, ".req"\)\) \{/if (e->d_name[0] != 0x2e) {/'
# control: taking the flag does not clear it (the helper start would repeat every tick)
mutate "$d/real.inc" "$d/ctl-noclear.inc" 's/return __atomic_exchange_n\(&g_rs_work, 0, __ATOMIC_ACQ_REL\);/return __atomic_load_n(&g_rs_work, __ATOMIC_ACQUIRE);/'
# control: a plain flag between the two threads (a data race)
mutate "$d/real.inc" "$d/ctl-plain.inc" 's/__atomic_store_n\(&g_rs_work, rs_queue_has_req\(root\), __ATOMIC_RELEASE\);/g_rs_work = rs_queue_has_req(root);/; s/return __atomic_exchange_n\(&g_rs_work, 0, __ATOMIC_ACQ_REL\);/int v = g_rs_work; g_rs_work = 0; return v;/'
# control: the loop scans whether or not the feature is on
mutate "$d/real.inc" "$d/ctl-ignoreon.inc" 's/if \(now >= next && __atomic_load_n\(on, __ATOMIC_RELAXED\)\) \{/if (now >= next) {/'
# control: the loop never scans (the scan left to another mod's worker)
mutate "$d/real.inc" "$d/ctl-noscan.inc" 's/\brs_scan_publish\(root\);/(void) root;/'
for v in real ctl-anyfile ctl-noclear ctl-plain ctl-ignoreon ctl-noscan; do
    $CC "${CFLAGS[@]}" -I$T -DRSSCAN_INC="\"$d/$v.inc\"" -o "$d/$v" $T/rsscan_test.c -lpthread
done
suite "remote stems queue scan" "$d/real" "$d/scratch-real"
RSS_TSAN=0
if [ -n "$SAN" ]; then
    echo "(sanitizer pass: the ThreadSanitizer case runs in the plain pass; this pass built the suite itself with $SAN)"
elif AZ_CC_PROBE=1 $CC -std=gnu11 -O1 -g -fsanitize=thread -I$T -DRSSCAN_INC="\"$d/real.inc\"" -o "$d/tsan-real" $T/rsscan_test.c -lpthread 2>/dev/null &&
   AZ_CC_PROBE=1 $CC -std=gnu11 -O1 -g -fsanitize=thread -I$T -DRSSCAN_INC="\"$d/ctl-plain.inc\"" -o "$d/tsan-ctl-plain" $T/rsscan_test.c -lpthread 2>/dev/null; then
    RSS_TSAN=1
    suite "remote stems queue scan (ThreadSanitizer)" env TSAN_OPTIONS=halt_on_error=1 "$d/tsan-real" "$d/scratch-tsan"
else
    skip "remote stems queue scan (ThreadSanitizer): $CC has no -fsanitize=thread"; echo
fi
# rsscan_home <guest/azmod dir>: the queue scan has its own thread, started by the Remote Stems
# install path; no other file calls the scan, so it never depends on another mod's hooks or worker.
rsscan_home() {
    local G=$1 calls inst
    calls=$(grep -nE '\brs_scan_publish\s*\(' "$G"/*.c | grep -vE '^[^:]*remote_stems\.c:[0-9]+:static ')
    inst=$(extract "$G/remote_stems.c" '^static void install_remotestems\(')
    [ "$(printf '%s\n' "$calls" | grep -c .)" = 1 ] &&
        ! printf '%s\n' "$calls" | grep -qv 'remote_stems\.c' &&
        printf '%s\n' "$inst" | grep -qE '^ +rs_scan_start\(\);' &&
        { echo "PASS  remote stems scan: its own thread, started by its install path"; return 0; }
    echo "FAIL  remote stems scan: called from outside remote_stems.c, or not started by its install:"
    printf '%s\n' "$calls" | sed 's/^/      /'
    return 1
}
mkdir -p "$d/ctl-guest"; cp guest/azmod/*.c "$d/ctl-guest/"
printf '\nstatic void rs_ctl_worker(void) { rs_scan_publish("/media/usb"); }\n' >> "$d/ctl-guest/stem_marks.c"
suite "remote stems scan home" rsscan_home guest/azmod

# ---------------------------------------------------------------- touch reader guard
# touch_read_guard.h: a failed read on EP147's touch device is reported as 0 bytes, so the
# reader's parser never gets a -1 to widen into a huge event count (touchguard_test.c). The
# threaded case also runs under ThreadSanitizer.
d="$OUT/touchguard"; mkdir -p "$d"
cp guest/azmod/touch_read_guard.h "$d/real.h"
grep -q 'tg_result' "$d/real.h" || { echo "touch_read_guard.h not found"; exit 2; }
# control: the guard passes a failed read through
mutate "$d/real.h" "$d/ctl-noguard.h" 's/return \(r < 0 && tg_tracked\(fd\)\) \? 0 : r;/return r;/'
# control: every descriptor's failed read is rewritten
mutate "$d/real.h" "$d/ctl-everyfd.h" 's/return \(r < 0 && tg_tracked\(fd\)\) \? 0 : r;/return r < 0 ? 0 : r;/'
# control: a closed descriptor stays tracked
mutate "$d/real.h" "$d/ctl-noforget.h" 's/(static void tg_forget\(int fd\) \{)\n/$1\n    return;\n/'
# control: any path that starts with the device's path counts
mutate "$d/real.h" "$d/ctl-prefix.h" 's/strcmp\(path, TG_PATH\) != 0/strncmp(path, TG_PATH, sizeof TG_PATH - 1) != 0/'
# control: a plain table between the threads (a data race)
mutate "$d/real.h" "$d/ctl-plain.h" 's/__atomic_load_n\(&g_tg_fd\[i\], __ATOMIC_ACQUIRE\)/g_tg_fd[i]/; s/__atomic_compare_exchange_n\(&g_tg_fd\[i\], &cur, -1, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED\);/if (g_tg_fd[i] == fd) g_tg_fd[i] = -1;/'
for v in real ctl-noguard ctl-everyfd ctl-noforget ctl-prefix ctl-plain; do
    $CC "${CFLAGS[@]}" -I$T -DTG_INC="\"$d/$v.h\"" -o "$d/$v" $T/touchguard_test.c -lpthread
done
suite "touch read guard" "$d/real"
TG_TSAN=0
if [ -n "$SAN" ]; then
    echo "(sanitizer pass: the ThreadSanitizer case runs in the plain pass; this pass built the suite itself with $SAN)"
elif AZ_CC_PROBE=1 $CC -std=gnu11 -O1 -g -fsanitize=thread -I$T -DTG_INC="\"$d/real.h\"" -o "$d/tsan-real" $T/touchguard_test.c -lpthread 2>/dev/null &&
   AZ_CC_PROBE=1 $CC -std=gnu11 -O1 -g -fsanitize=thread -I$T -DTG_INC="\"$d/ctl-plain.h\"" -o "$d/tsan-ctl-plain" $T/touchguard_test.c -lpthread 2>/dev/null; then
    TG_TSAN=1
    suite "touch read guard (ThreadSanitizer)" env TSAN_OPTIONS=halt_on_error=1 "$d/tsan-real"
else
    skip "touch read guard (ThreadSanitizer): $CC has no -fsanitize=thread"; echo
fi
# ---------------------------------------------------------------- Save Logs to USB (overlay side)
# savelogs_write_req / savelogs_parse (savelogs.c): one request through a temp file + rename, and a
# status line that answers this press only (savelogs_test.c). The request is sent only from
# handle_action, on the overlay thread (savelogs_caller below).
d="$OUT/savelogs"; mkdir -p "$d"
{ grep -E '^#define SAVELOGS_WAIT_MS ' guest/azmod/savelogs.c
  extract guest/azmod/savelogs.c '^static int savelogs_write_req\('
  extract guest/azmod/savelogs.c '^static int savelogs_parse\('; } > "$d/real.inc"
grep -q 'SAVELOGS_WAIT_MS' "$d/real.inc" && grep -q 'rename(tmp, req)' "$d/real.inc" && grep -q 'rq != id' "$d/real.inc" ||
    { echo "extraction of savelogs.c failed"; exit 2; }
# control: written in place (no temp file)
mutate "$d/real.inc" "$d/ctl-inplace.inc" 's/FILE \*f = fopen\(tmp, "we"\);/FILE *f = fopen(req, "we");/'
# control: the request id not compared (an earlier press's result shown as this one's)
mutate "$d/real.inc" "$d/ctl-anyid.inc" 's/if \(rq != id\) \{/if (0) {/'
# control: no wait limit (an unanswered request keeps the button busy forever)
mutate "$d/real.inc" "$d/ctl-nowait.inc" 's/if \(age_ms >= SAVELOGS_WAIT_MS\) \{/if (0) {/'
for v in real ctl-inplace ctl-anyid ctl-nowait; do
    $CC "${CFLAGS[@]}" -I$T -DSAVELOGS_INC="\"$d/$v.inc\"" -o "$d/$v" $T/savelogs_test.c
done
suite "save logs request" "$d/real" "$d/scratch-real"
# savelogs_caller <guest/azmod dir>: savelogs_request (the writer's only caller) is called from
# handle_action in mod_menu_overlay.c and nowhere else, so no request is ever written from
# EP147's UI, render, audio or bus threads.
savelogs_caller() {
    local G=$1 calls body
    calls=$(grep -nE '\bsavelogs_(request|write_req)\s*\(' "$G"/*.c | grep -vE '^[^:]*savelogs\.c:[0-9]+:static ')
    body=$(extract "$G/mod_menu_overlay.c" '^static void handle_action\(')
    [ "$(printf '%s\n' "$calls" | grep -c .)" = 2 ] &&
        printf '%s\n' "$calls" | grep -qE '^[^:]*/savelogs\.c:[0-9]+: +g_savelogs_sendfail = savelogs_write_req\(' &&
        printf '%s\n' "$calls" | grep -qE '^[^:]*/mod_menu_overlay\.c:' &&
        printf '%s\n' "$body" | grep -qE '^ +savelogs_request\(&g_state\);' &&
        { echo "PASS  save logs: the request is sent only from handle_action (overlay thread)"; return 0; }
    echo "FAIL  save logs: savelogs_request / savelogs_write_req called outside handle_action:"
    printf '%s\n' "$calls" | sed 's/^/      /'
    return 1
}
mkdir -p "$d/ctl-guest"; cp guest/azmod/*.c "$d/ctl-guest/"
printf '\nstatic void savelogs_ctl_bus(void) { savelogs_request(&g_state); }\n' >> "$d/ctl-guest/bus_interpose.c"
suite "save logs caller" savelogs_caller guest/azmod

# ---------------------------------------------------------------- rs_status_banner
d="$OUT/rsbanner"; mkdir -p "$d"
extract guest/azmod/remote_stems.c '^static int rs_status_banner\(' > "$d/real.inc"
grep -q 'banner=' "$d/real.inc" || { echo "extraction of rs_status_banner failed"; exit 2; }
mutate "$d/real.inc" "$d/ctl-nomark.inc" 's/if \(mark && !strncmp\(mark, "banner=", 7\)\)/if (0)/'   # the marker ignored
mutate "$d/real.inc" "$d/ctl-nocut.inc"  's/\*nl = 0;//'                                              # line 2 shown too
for v in real ctl-nomark ctl-nocut; do
    $CC "${CFLAGS[@]}" -I$T -DSTATUS_INC="\"$d/$v.inc\"" -o "$d/$v" $T/rsbanner_test.c
done
suite "rs_status_banner" "$d/real"
# ---------------------------------------------------------------- decks (may the loader restart EP147)
d="$OUT/decks"; mkdir -p "$d"
{ extract guest/azstem/deck_track_map.c '^static char decks_classify\('
  extract guest/azstem/deck_track_map.c '^static int decks_verdict\('; } > "$d/real.inc"
grep -q "prev_zero ? 'E'" "$d/real.inc" && grep -q 'unknown ? -1' "$d/real.inc" || { echo "extraction of decks_classify/decks_verdict failed"; exit 2; }
mutate "$d/real.inc" "$d/ctl-nodebounce.inc" "s/return prev_zero \? 'E' : 'U';/return 'E';/"   # one zero = empty
mutate "$d/real.inc" "$d/ctl-unknown.inc"    's/return unknown \? -1 : 0;/return 0;/'         # unknown read as empty
for v in real ctl-nodebounce ctl-unknown; do
    $CC "${CFLAGS[@]}" -I$T -DDECKS_INC="\"$d/$v.inc\"" -o "$d/$v" $T/decks_test.c
done
suite "decks" "$d/real"
# ---------------------------------------------------------------- strip neutral (Day overview strokes)
d="$OUT/stripneutral"; mkdir -p "$d"
{ grep -E '^#define DM_NEUTRAL_(SAT|INK) ' guest/azmod/day_mode.c; extract guest/azmod/day_mode.c '^static uint32_t dm_strip_neutral\('; } > "$d/real.inc"
grep -q 'DM_NEUTRAL_INK' "$d/real.inc" || { echo "extraction of dm_strip_neutral failed"; exit 2; }
mutate "$d/real.inc" "$d/ctl-off.inc" 's/if \(sat >= DM_NEUTRAL_SAT\)/if (1)/'   # neutral strokes left white
for v in real ctl-off; do
    $CC "${CFLAGS[@]}" -I$T -DSTRIP_INC="\"$d/$v.inc\"" -o "$d/$v" $T/stripneutral_test.c
done
suite "strip neutral" "$d/real"
# ---------------------------------------------------------------- drum feed (master BPM)
d="$OUT/drumfeed"; mkdir -p "$d"
extract guest/azmod/stems_display.c '^static float drum_feed_bpm\(' > "$d/real.inc"
grep -q 'DBL_MAX' "$d/real.inc" || { echo "extraction of drum_feed_bpm failed"; exit 2; }
mutate "$d/real.inc" "$d/ctl-nomaster.inc" 's/if \(live != DBL_MAX \|\|/if (1 ||/'                 # empty deck never takes the master
mutate "$d/real.inc" "$d/ctl-anyunknown.inc" 's/live != DBL_MAX \|\| //'                         # any unknown BPM takes the master's
mutate "$d/real.inc" "$d/ctl-trust.inc" 's/master < 0 \|\| (.*?) \|\| !master_ok/$1/'          # unknown / unreadable master trusted
for v in real ctl-nomaster ctl-anyunknown ctl-trust; do
    $CC "${CFLAGS[@]}" -I$T -DFEED_INC="\"$d/$v.inc\"" -o "$d/$v" $T/drumfeed_test.c
done
suite "drum feed" "$d/real"
# ---------------------------------------------------------------- BpmState (shared_ptr pin)
d="$OUT/bpmstate"; mkdir -p "$d"
{ grep -E '^#define (BPMSTATE_(DECK|TRACK|PIN_MAX)|BPMCTRL_(OFF|USE|WEAK)) ' guest/azmod/stems_display.c
  grep -E '^static (unsigned long|int) g_bpmstate' guest/azmod/stems_display.c
  extract guest/azmod/stems_display.c '^static int bpmstate_capture\('
  extract guest/azmod/stems_display.c '^static int bpmstate_read\('; } > "$d/real.inc"
grep -q 'BPMCTRL_WEAK), 1' "$d/real.inc" && grep -q 'g_bpmstate_id\[d\]' "$d/real.inc" ||
    { echo "extraction of bpmstate_capture/bpmstate_read failed"; exit 2; }
mutate "$d/real.inc" "$d/ctl-nouse.inc" 's/__atomic_load_n\(use, (__ATOMIC_ACQUIRE|__ATOMIC_RELAXED)\) <= 0/0/g'   # use count never read
mutate "$d/real.inc" "$d/ctl-noweak.inc" 's/    __atomic_add_fetch\(\(int \*\) \(ctrl \+ BPMCTRL_WEAK\), 1, __ATOMIC_ACQ_REL\);[^\n]*\n//'  # no weak reference
mutate "$d/real.inc" "$d/ctl-novt.inc" 's/if \(\*\(volatile uintptr_t \*\) ctrl != want_vt\)/if (0)/'    # block vtable not checked
mutate "$d/real.inc" "$d/ctl-noid.inc" 's/if \(id != __atomic_load_n\(&g_bpmstate_id\[d\], __ATOMIC_RELAXED\)\)/if (0)/'  # object identity not re-checked
mutate "$d/real.inc" "$d/ctl-nocap.inc" 's/> BPMSTATE_PIN_MAX\)/> 1000000)/'                     # pins unbounded
for v in real ctl-nouse ctl-noweak ctl-novt ctl-noid ctl-nocap; do
    $CC "${CFLAGS[@]}" -I$T -DBPM_INC="\"$d/$v.inc\"" -o "$d/$v" $T/bpmstate_test.c
done
suite "bpmstate" "$d/real"
# ---------------------------------------------------------------- the audio unit's deck (azstem)
# deck_slot (stem_playback.c): a unit is a deck only by identity; no number or a held slot is
# unknown, never the next free slot (deckslot_test.c)
d="$OUT/deckslot"; mkdir -p "$d"
awk '/^static const int STEM_SLOT\[NDECK\] = \{/ {p=1} p {print} p && /\};/ {exit}' guest/azstem/stem_playback.c > "$d/slots.inc"
extract guest/azstem/stem_playback.c '^static int deck_slot\(' > "$d/real.inc"
grep -q 'STEM_SLOT' "$d/slots.inc" && grep -q 'g_ap_conflict' "$d/real.inc" || { echo "extraction of deck_slot failed"; exit 2; }
# control: a unit with no number takes the first free slot (arrival order, the old claim)
mutate "$d/real.inc" "$d/ctl-arrival.inc" 's|(    /\* Unknown: never mixed)|    for (int i = 0; i < NDECK; i++) {\n        void *e0 = NULL;\n        if (atomic_compare_exchange_strong(\&g_deck_self[STEM_SLOT[i]], \&e0, self))\n            return STEM_SLOT[i];\n    }\n$1|'
# control: a held slot taken over by the second unit
mutate "$d/real.inc" "$d/ctl-takeover.inc" 's/atomic_fetch_or_explicit\(&g_ap_conflict, 1u << d, memory_order_relaxed\);/atomic_store(\&g_deck_self[s], self);\n            return s;/'
for v in real ctl-arrival ctl-takeover; do
    $CC "${CFLAGS[@]}" -pthread -I$T -DSTEMSLOT_INC="\"$d/slots.inc\"" -DDECKSLOT_INC="\"$d/$v.inc\"" -o "$d/$v" $T/deckslot_test.c
done
suite "deck slot" "$d/real"

# ---------------------------------------------------------------- EP147 object lifetime (azstem)
# region <file> <start regex> <stop regex>: from the start line up to (not including) the stop line
region() {
    START="$2" STOP="$3" awk '$0 ~ ENVIRON["START"] {p=1} p && $0 ~ ENVIRON["STOP"] {exit} p {print}' "$1"
}
d="$OUT/lifetime"; mkdir -p "$d"
SP=guest/azstem/stem_playback.c; DT=guest/azstem/deck_track_map.c; DR=guest/azstem/drumroll.c
region $SP '^static _Atomic\(void \*\) g_ls_self\[NDECK\];' '^#ifndef AZMOD_RELEASE /\* the path scan|^static int is_dir\(const char \*p\) \{' > "$d/ls.inc"  # the published tree has no dev block
region $DT '^/\* ---- DjPlayerUnit ctor hook' '^static int lti_id_zero\(' > "$d/dt.inc"
cat "$d/ls.inc" "$d/dt.inc" > "$d/real.inc"
grep -q 'ls_wait_readers' "$d/real.inc" && grep -q 'lti_update_hook' "$d/real.inc" && grep -q 'djpudtor_hook' "$d/real.inc" &&
    grep -q '^static int lti_read' "$d/real.inc" || { echo "extraction of the LoadState / LoadedTrackInfo / DjPlayerUnit code failed"; exit 2; }
mutate "$d/real.inc" "$d/ctl-nolsdtor.inc" 's/(static void lsdtor_hook\(void \*self\) \{\n)    for \(int d = 0; d < NDECK; d\+\+\) \{.*?\n    \}\n/$1/s'  # dtor unpublishes nothing
mutate "$d/real.inc" "$d/ctl-nomagic.inc" 's/ \|\| magic != LS_MAGIC \|\| d != /|| d != /'   # magic not re-checked at use
mutate "$d/real.inc" "$d/ctl-nowait.inc" 's/(static void ls_wait_readers\(int deck\) \{\n)/$1    return;\n/'   # dtor does not wait
mutate "$d/real.inc" "$d/ctl-lsvt.inc" 's/if \(vt != \(uintptr_t\) LS_VT \|\| magic != LS_MAGIC\)\n        return;\n    if \(d >= /if (magic != LS_MAGIC)\n        return;\n    if (d >= /'  # ctor hook ignores the vtable
mutate "$d/real.inc" "$d/ctl-djearly.inc" 's/    g_orig_djpuctor\(self, id, ctx, cap\);\n(.*?)\n\}\n/$1\n    g_orig_djpuctor(self, id, ctx, cap);\n}\n/s'  # published before the ctor ran
mutate "$d/real.inc" "$d/ctl-djdtor.inc" 's/(static void djpudtor_hook\(void \*self\) \{\n).*?\n    atomic_fetch_add_explicit\(&g_djpu_dtor_calls/$1    atomic_fetch_add_explicit(&g_djpu_dtor_calls/s'  # unit dtor clears nothing
mutate "$d/real.inc" "$d/ctl-noowner.inc" 's/if \(!valid \|\| !owner \|\|\n\s*atomic_load_explicit\(&g_lti_tok\[deck\], memory_order_acquire\) != owner\)/if (!valid)/'  # a copy served whoever owns it
mutate "$d/real.inc" "$d/ctl-fgprvt.inc" 's/return \*\(const volatile uintptr_t \*\) f == \(uintptr_t\) FGPR_VTABLE \? f : 0;/return f;/'  # FGPR not type-checked
mutate "$d/real.inc" "$d/ctl-nostale.inc" 's/    if \(atomic_load_explicit\(&g_lti_tok\[d\], memory_order_relaxed\) != lti\) \{\n.*?\n    \}\n//s; s/if \(!valid \|\| !owner \|\|\n\s*atomic_load_explicit\(&g_lti_tok\[deck\], memory_order_acquire\) != owner\)/if (!valid)/'  # capture after teardown, served regardless
mutate "$d/real.inc" "$d/ctl-norecheck.inc" 's/if \(atomic_load_explicit\(&sn->seq, memory_order_relaxed\) != s1\)\n\s*continue;\n//'  # reader without the second look
mutate "$d/real.inc" "$d/ctl-nolock.inc" 's/(static unsigned lti_wlock\(struct lti_snap \*sn\) \{\n    unsigned s = atomic_load_explicit\(&sn->seq, memory_order_relaxed\);\n).*?\n    atomic_thread_fence\(memory_order_release\);/$1    atomic_store_explicit(&sn->seq, s + 1u, memory_order_relaxed);\n    atomic_thread_fence(memory_order_release);/s'  # writers not serialized
for v in real ctl-nolsdtor ctl-nomagic ctl-nowait ctl-lsvt ctl-djearly ctl-djdtor ctl-noowner ctl-fgprvt; do
    $CC "${CFLAGS[@]}" -pthread -I$T -DLIFE_INC="\"$d/$v.inc\"" -o "$d/$v" $T/lifetime_test.c
done
suite "lifetime" "$d/real"
for v in real ctl-nostale ctl-norecheck ctl-nolock; do
    $CC "${CFLAGS[@]}" -pthread -I$T -DLIFE_INC="\"$d/$v.inc\"" -o "$d/snap-$v" $T/ltisnap_test.c
done
suite "ltisnap" "$d/snap-real" 1.0
# ---------------------------------------------------------------- community mod services: deck and UI
d="$OUT/sdk"; mkdir -p "$d"
L=guest/azmod/sdk_logic.h
region $DR '^/\* Correction for unmeasured cases' '^/\* ---- EP147.s own beat grid' > "$d/grid.inc"
region guest/azstem/sdk_export.c '^int azstem_deck_beat_phase\(int deck, double \*phase\);' '^NEVER_MATCHES$' >> "$d/grid.inc"
grep -q 'drum_grid_publish' "$d/grid.inc" && grep -q '^int azstem_deck_beat_phase(int deck, double \*phase) {' "$d/grid.inc" ||
    { echo "extraction of the beat grid seqlock / azstem_deck_beat_phase failed"; exit 2; }
cp $L "$d/real.h"; cp "$d/grid.inc" "$d/real.inc"
mutate "$d/grid.inc" "$d/ctl-norecheck.inc" 's/atomic_load_explicit\(&g->seq, memory_order_relaxed\) != s1 \|\| //'   # reader without the second look
mutate "$d/grid.inc" "$d/ctl-nodemand.inc" 's/(        atomic_store_explicit\(&g_grid_demand, 1, memory_order_relaxed\);)/        ;/'  # never asks for grids
mutate $L "$d/ctl-anymove.h"   's/if \(pos > p->last_pos\) \{/if (pos != p->last_pos) {/'          # any movement is playing
mutate $L "$d/ctl-nobase.h"    's/if \(pv->loaded != -2\) \{/if (1) {/'                             # the first state is an event
mutate $L "$d/ctl-closed.h"    's/    if \(c->state == SC_SHOWN && !can_show\)\n.*?\n//s'           # dialog gone, no answer
mutate $L "$d/ctl-clickyes.h"  's/        return \*sel \? 1 : 0;/        return 1;/'                     # a click on Cancel says yes
mutate $L "$d/ctl-backyes.h"   's/    case SC_IN_BACK:\n        return 0;/    case SC_IN_BACK:\n        return 1;/'  # BACK says yes
mutate $L "$d/ctl-second.h"    's/if \(c->state != SC_IDLE \|\| !fn\)/if (!fn)/'                     # a second question overwrites
mutate $L "$d/ctl-nocas.h"     's/if \(atomic_compare_exchange_weak_explicit\(&q->head, &pos, pos \+ 1, memory_order_relaxed,\n\s*memory_order_relaxed\)\) \{/if ((atomic_store_explicit(&q->head, pos + 1, memory_order_relaxed), 1)) {/'  # producers not serialized
mutate $L "$d/ctl-anyfn.h"     's/    if \(!hi \|\| fn < lo \|\| fn >= hi\)\n.*?\n//s'               # any entry point
mutate $L "$d/ctl-guess.h"     's/(        return SD_PAD_SLIPLOOP;\n)    return -1;/$1    return SD_PAD_HOTCUE;/'  # unknown modes guessed
for v in real ctl-norecheck ctl-nodemand; do
    $CC "${CFLAGS[@]}" -pthread -I$T -DSDK_LOGIC_INC="\"$d/real.h\"" -DGRID_INC="\"$d/$v.inc\"" -o "$d/$v" $T/sdk_test.c -lm
done
for v in ctl-anymove ctl-nobase ctl-closed ctl-clickyes ctl-backyes ctl-second ctl-nocas ctl-anyfn ctl-guess; do
    $CC "${CFLAGS[@]}" -pthread -I$T -DSDK_LOGIC_INC="\"$d/$v.h\"" -o "$d/$v" $T/sdk_test.c -lm
done
suite "sdk" "$d/real" 1.0
# ---------------------------------------------------------------- drum grid through EP147's references
d="$OUT/meowgrid"; mkdir -p "$d"
{ cat "$OUT/lifetime/ls.inc"; region $DR '^#define BG_PROV_OFF ' '^/\* Flusher: reload each deck'; } > "$d/real.inc"
grep -q 'meow_acquire' "$d/real.inc" && grep -q 'static int drum_grid_native' "$d/real.inc" || { echo "extraction of drum_grid_native failed"; exit 2; }
mutate "$d/real.inc" "$d/ctl-noref.inc" 's/return meow_acquire\(o\) \? o : 0;/return o;/'   # the slot's object used without a reference
mutate "$d/real.inc" "$d/ctl-resurrect.inc" 's/if \(c <= 0\)\n\s*return 0; \/\* already on its way out \*\//if (0)\n            return 0;/'   # a dying object taken back
mutate "$d/real.inc" "$d/ctl-nodestroy.inc" 's/if \(destroy\)\n\s*destroy\(/if (0)\n            destroy(/'   # last release destroys nothing
mutate "$d/real.inc" "$d/ctl-nowait.inc" 's/(static void ls_wait_readers\(int deck\) \{\n)/$1    return;\n/'   # teardown does not wait for readers
for v in real ctl-noref ctl-resurrect ctl-nodestroy ctl-nowait; do
    $CC "${CFLAGS[@]}" -pthread -I$T -DGRID_INC="\"$d/$v.inc\"" -o "$d/$v" $T/meowgrid_test.c
done
suite "meowgrid" "$d/real" 1.0
# ---------------------------------------------------------------- reader count + destructor hook
# The three EP147 objects azmod caches across calls (TaglistEditor, mixer Channel,
# VinylSpeedAdjustor): a destructor waits for a message-thread reader inside the count, never
# for its own caller, and unpublishes first; nothing is cached without the destructor hooks.
d="$OUT/readers"; mkdir -p "$d"
PA=guest/azmod/playlist_actions.c; BT=guest/azmod/bluetooth.c; VB=guest/azmod/vinyl_brake.c
{ region $PA '^static _Atomic uintptr_t g_tc_self;' '^static void tag_readd\('
  region $PA '^static void \*\(\*g_tc_orig_add\)' '^static int tag_capture_install\('; } > "$d/tc.inc"
{ grep -E '^static _Atomic uintptr_t g_mixch\[4\];' $BT
  region $BT '^static int g_mixch_dtor_ok;' '^static int mixch_input\('
  extract $BT '^static int mixch_input\('; } > "$d/mixch.inc"
{ grep -E '^static void \(\*g_orig_vb_set\)|^static _Atomic int g_vb_milli|^static _Atomic long g_vb_adj\[VB_NADJ\];' $VB
  region $VB '^static int g_vb_dtor_ok;' '^/\* Constructor phase'
  extract $VB '^static void vb_set_hook\('
  extract guest/azmod/vinyl_slider.c '^static void vs_apply\('; } > "$d/vb.inc"
grep -q 'ep_wait_readers(&g_tc_readers)' "$d/tc.inc" && grep -q 'static uintptr_t tc_hold' "$d/tc.inc" && grep -q 'static void tc_note' "$d/tc.inc" &&
grep -q 'ep_wait_readers(&g_mixch_readers)' "$d/mixch.inc" && grep -q 'static uintptr_t mixch_get' "$d/mixch.inc" && grep -q 'static void mixch_ctor_hook' "$d/mixch.inc" &&
grep -q 'ep_wait_readers(&g_vb_readers)' "$d/vb.inc" && grep -q 'static void vb_set_hook' "$d/vb.inc" && grep -q 'static void vs_apply' "$d/vb.inc" ||
    { echo "extraction of the reader-count / destructor code failed"; exit 2; }
for m in tc mixch vb; do
    R="$d/$m.inc"
    mutate "$R" "$d/$m-ctl-nowait.inc" 's/\n( *)if \(!on_msg_thread\(\)\)( \/\*[^\n]*)?\n\s*ep_wait_readers\([^)]*\);//'   # destructor does not wait for readers
    mutate "$R" "$d/$m-ctl-msgwait.inc" 's/\n( *)if \(!on_msg_thread\(\)\)( \/\*[^\n]*)?\n(\s*ep_wait_readers)/\n$3/'          # waits on the message thread too
    mutate "$R" "$d/$m-ctl-nounpub.inc" 's/\n\s*__atomic_compare_exchange_n\(&(g_tc_self|g_mixch\[i\]|g_vb_adj\[i\]), &e, 0,[^\n]*//'   # destructor leaves the object published
done
mutate "$d/tc.inc"    "$d/tc-ctl-nodtorok.inc"    's/if \(g_tc_del_ok\)/if (1)/'                      # cached without the destructor hooks
mutate "$d/mixch.inc" "$d/mixch-ctl-nodtorok.inc" 's/if \(g_mixch_dtor_ok && i >= 0/if (i >= 0/'      # cached without the destructor hooks
mutate "$d/vb.inc"    "$d/vb-ctl-nodtorok.inc"    's/for \(int i = 0; g_vb_dtor_ok && i < VB_NADJ/for (int i = 0; i < VB_NADJ/'   # recorded without the destructor hooks
mech() { case $1 in tc) echo 1 ;; mixch) echo 2 ;; vb) echo 3 ;; esac; }
# Tests of code that calls process_vm_readv or applies GCC's __atomic builtins to _Atomic
# objects (clang rejects that) build and run on Linux only; their extraction checks run everywhere.
skip_off_linux() {
    [ "$(uname -s)" = Linux ] && return 1
    skip "$1: Linux only: run this script under Linux or the build container (not a pass)"
}
if ! skip_off_linux "readers (3 suites)"; then
for m in tc mixch vb; do
    for v in "" -ctl-nowait -ctl-msgwait -ctl-nounpub -ctl-nodtorok; do
        $CC "${CFLAGS[@]}" -pthread -I$T -Iguest/azmod -DMECH=$(mech $m) -DRC_INC="\"$d/$m$v.inc\"" -o "$d/$m$v" $T/readers_test.c
    done
done
suite "readers: TaglistEditor"      "$d/tc" 1.0
suite "readers: mixer Channel"      "$d/mixch" 1.0
suite "readers: VinylSpeedAdjustor" "$d/vb" 1.0
fi
# ---------------------------------------------------------------- cached pointers revalidated at use (F4)
d="$OUT/f4ident"; mkdir -p "$d"
{ region guest/azmod/stems_display.c '^static _Atomic long g_perdeck\[AZMOD_STEM_DECKS\];' '^/\* Diagnostic: is this deckstate pointer held'
  grep -E '^static _Atomic uintptr_t g_wwb_comp\[8\]\[3\];' guest/azmod/waveform.c
  extract guest/azmod/waveform.c '^static uintptr_t wwb_widget_live\('; } > "$d/real.inc"
grep -q 'static uintptr_t perdeck_block' "$d/real.inc" && grep -q 'static int deck_for_perdeck_field' "$d/real.inc" &&
    grep -q 'static uintptr_t wwb_widget_live' "$d/real.inc" || { echo "extraction of perdeck_block / wwb_widget_live failed"; exit 2; }
mutate "$d/real.inc" "$d/ctl-noword0.inc"  's/ \|\|\n\s*\*\(volatile uintptr_t \*\) b != \(uintptr_t\) d\)/)/'                      # block's deck not re-checked at use
mutate "$d/real.inc" "$d/ctl-nouse.inc"    's/__atomic_load_n\(\(const int \*\) \(ctrl \+ PISCTRL_USE\), __ATOMIC_ACQUIRE\) <= 0 \|\|/0 ||/'  # disposal not seen
mutate "$d/real.inc" "$d/ctl-novtuse.inc"  's/\*\(volatile uintptr_t \*\) ctrl != \(uintptr_t\) PISCTRL_VT \|\|/0 ||/'                # control block vtable not re-checked
mutate "$d/real.inc" "$d/ctl-novtcap.inc"  's/if \(!PISCTRL_VT \|\| !own_q\(ctrl, &vt\) \|\| vt != \(uintptr_t\) PISCTRL_VT\)/if (!own_q(ctrl, \&vt))/'  # any make_shared block filed
mutate "$d/real.inc" "$d/ctl-wwbnovt.inc"  's/if \(!own_q\(w, &vt\) \|\| vt != WAVEW_MODES\[m\]\.vt\)/if (!own_q(w, \&vt))/'      # widget vtable not checked at use
mutate "$d/real.inc" "$d/ctl-wwbanythread.inc" 's/ \|\| !on_msg_thread\(\)\)/)/'                                                   # any thread
if ! skip_off_linux "f4 identity"; then
for v in real ctl-noword0 ctl-nouse ctl-novtuse ctl-novtcap ctl-wwbnovt ctl-wwbanythread; do
    $CC "${CFLAGS[@]}" -pthread -I$T -Iguest/azmod -DF4_INC="\"$d/$v.inc\"" -o "$d/$v" $T/f4ident_test.c
done
suite "f4 identity" "$d/real"
fi
# ---------------------------------------------------------------- deck identity (tiles, pad containers)
d="$OUT/deckid"; mkdir -p "$d"
{ extract guest/azmod/stems_display.c '^static int deck_for_ds\(long ds\) \{'
  extract guest/azmod/stems_display.c '^static int deck_for_modeobj\(long p\) \{'; } > "$d/real.inc"
grep -q 'PERDECK_TILEDS' "$d/real.inc" && grep -q 'PERDECK_MODEOBJ' "$d/real.inc" || { echo "extraction of deck_for_ds/deck_for_modeobj failed"; exit 2; }
# control 1: tile deckstates claimed in order of arrival until the table is up
mutate "$d/real.inc" "$d/ctl-arrival.inc" 's/    return deck_for_perdeck_field\(ds, PERDECK_TILEDS\);/    int r = deck_for_perdeck_field(ds, PERDECK_TILEDS);\n    static long seen[AZMOD_STEM_DECKS];\n    static int n;\n    if (r >= 0)\n        return r;\n    for (int i = 0; i < n; i++)\n        if (seen[i] == ds)\n            return i;\n    if (ds > 0x10000 && n < AZMOD_STEM_DECKS) {\n        seen[n] = ds;\n        return n++;\n    }\n    return -1;/'
# control 2: an unknown container falls back to deck 0
mutate "$d/real.inc" "$d/ctl-default.inc" 's/    return deck_for_perdeck_field\(p, PERDECK_MODEOBJ\);/    int r = deck_for_perdeck_field(p, PERDECK_MODEOBJ);\n    return r >= 0 ? r : 0;/'
for v in real ctl-arrival ctl-default; do
    $CC "${CFLAGS[@]}" -I$T -DDECKID_INC="\"$d/$v.inc\"" -o "$d/$v" $T/deckid_test.c
done
suite "deck identity" "$d/real"
# ---------------------------------------------------------------- deck screen evidence from the pad-mode getter
d="$OUT/deckscreen"; mkdir -p "$d"
extract guest/azmod/stems_display.c '^static int modeget_from_deck_screen\(uintptr_t lr\) \{' > "$d/real.inc"
grep -q 'TITLE_UPDATE_END' "$d/real.inc" || { echo "extraction of modeget_from_deck_screen failed"; exit 2; }
# control 1: every caller counts (azmod's own startup reads map the overlays over a black screen)
mutate "$d/real.inc" "$d/ctl-anycaller.inc" 's/    return \(lr >= WAVE_SWITCH.*?;\n/    return 1;\n/s'
# control 2: the pad title's window dropped
mutate "$d/real.inc" "$d/ctl-notitle.inc" 's/ \|\|\n\s*\(lr >= TITLE_UPDATE && lr < TITLE_UPDATE_END\)//'
for v in real ctl-anycaller ctl-notitle; do
    $CC "${CFLAGS[@]}" -I$T -DDECKSCREEN_INC="\"$d/$v.inc\"" -o "$d/$v" $T/deckscreen_test.c
done
suite "deck screen evidence" "$d/real"
# ---------------------------------------------------------------- waveform scaling + zoom model by deck identity
d="$OUT/wavezoom"; mkdir -p "$d"
{ region guest/azmod/waveform.c '^/\* The provider classes. color modes' '^/\* ---- Deck-tile overview bar'
  extract guest/azmod/waveform.c '^static int wwb_deck_by_content\('
  region guest/azmod/waveform.c '^/\* The deck of the FUN_01b93a28 running right now' '^/\* The accessor returns its result through x8'; } |
    sed 's/_Atomic //' > "$d/real.inc" # clang: __atomic_* on plain ints
grep -q 'static void wave_zoom_tick' "$d/real.inc" && grep -q '^int azmod_wave_zoom' "$d/real.inc" &&
    grep -q 'static int wwb_deck_by_content' "$d/real.inc" && grep -q '^void azm_wp_scale(long self' "$d/real.inc" ||
    { echo "extraction of the waveform zoom model / azm_wp_scale failed"; exit 2; }
# control 1: an unknown provider bound by its point count (the guess this code replaced)
mutate "$d/real.inc" "$d/ctl-guess.inc" 's/    else \{\n        g_wp_nodeck\+\+;\n        return;\n    \}/    else if ((deck = ctl_guess_deck(self)) < 0) {\n        g_wp_nodeck++;\n        return;\n    }/'
# control 2: an unknown deck taken as deck 0
mutate "$d/real.inc" "$d/ctl-deck0.inc" 's/    else \{\n        g_wp_nodeck\+\+;\n        return;\n    \}/    else\n        deck = 0;/'
# control 3: one zoom model per color mode, shared by every deck
mutate "$d/real.inc" "$d/ctl-onekey.inc" 's/wzoom \*z = &g_wz\[deck\]\[kind\];/wzoom *z = &g_wz[0][kind];/'
for v in real ctl-guess ctl-deck0 ctl-onekey; do
    $CC "${CFLAGS[@]}" -I$T -DWZ_INC="\"$d/$v.inc\"" -o "$d/$v" $T/wavezoom_test.c
done
suite "wave identity" "$d/real"
# ---------------------------------------------------------------- 60 FPS waveform feed lock
# lptick_hook / feed_run_frame (waveform.c): with 60 FPS Waveforms live, each deck's position feed
# runs once per displayed frame from the refresh path, its own timer only records it; entries are
# validated at use (feedlock_test.c). The threaded case also runs under ThreadSanitizer.
d="$OUT/feedlock"; mkdir -p "$d"
region guest/azmod/waveform.c '^/\* ---- Position feed lock' '^/\* ---- The waveform limiter' |
    sed 's/_Atomic //' > "$d/real.inc" # clang: __atomic_* on plain ints
grep -q '^static void lptick_hook(long self)' "$d/real.inc" && grep -q '^static void feed_run_frame(void)' "$d/real.inc" ||
    { echo "extraction of the feed lock failed"; exit 2; }
# control: the frame calls a recorded entry without validating it
mutate "$d/real.inc" "$d/ctl-novalid.inc" 's/if \(!feed_valid\(p\) \|\| /if (/'
# control: the own timer still runs the original while locked (two feeds per frame, the beat)
mutate "$d/real.inc" "$d/ctl-owntick.inc" 's/(    __atomic_store_n\(&g_feed\[i\]\.idle, 0, __ATOMIC_RELAXED\);\n)/$1    g_orig_lptick(self);\n/'
# control: an entry whose own timer stopped is never dropped
mutate "$d/real.inc" "$d/ctl-noidle.inc" 's/ \|\| __atomic_add_fetch\(&g_feed\[i\]\.idle, 1, __ATOMIC_RELAXED\) > FEED_IDLE_MAX//'
# control: no own-timer fallback when frames stop running the feed
mutate "$d/real.inc" "$d/ctl-nofallback.inc" 's/>= FEED_OWN_MAX\)/>= 1000000)/'
# control: a cross-thread switch read plainly (a data race, ThreadSanitizer only)
mutate "$d/real.inc" "$d/ctl-plain.inc" 's/__atomic_load_n\(&g_wave60_on, __ATOMIC_RELAXED\)/g_wave60_on/'
for v in real ctl-novalid ctl-owntick ctl-noidle ctl-nofallback; do
    $CC "${CFLAGS[@]}" -I$T -DFEEDLOCK_INC="\"$d/$v.inc\"" -o "$d/$v" $T/feedlock_test.c -lpthread
done
suite "feed lock" "$d/real"
if [ -n "$SAN" ]; then
    echo "(sanitizer pass: the ThreadSanitizer control runs in the plain pass; this pass built the suite itself with $SAN)"
elif AZ_CC_PROBE=1 $CC -std=gnu11 -O1 -g -fsanitize=thread -I$T -DFEEDLOCK_INC="\"$d/real.inc\"" -o "$d/tsan-real" $T/feedlock_test.c -lpthread 2>/dev/null &&
   AZ_CC_PROBE=1 $CC -std=gnu11 -O1 -g -fsanitize=thread -I$T -DFEEDLOCK_INC="\"$d/ctl-plain.inc\"" -o "$d/tsan-ctl-plain" $T/feedlock_test.c -lpthread 2>/dev/null; then
    FL_TSAN=1
    suite "feed lock (ThreadSanitizer)" env TSAN_OPTIONS=halt_on_error=1 "$d/tsan-real"
else
    FL_TSAN=0
    skip "feed lock (ThreadSanitizer): $CC has no -fsanitize=thread"; echo
fi
# ---------------------------------------------------------------- stem waveform curve: toggles and Stem Volume
# guest/azmod/wave_att.h natively, with build_atten (waveform.c) and ov_rebuild_attenuation
# (mod_menu_overlay.c) extracted; each control breaks one rule (waveatt_test.c)
d="$OUT/waveatt"
for v in real ctl-nogain ctl-nocap ctl-norate ctl-deck0 ctl-coarse ctl-ovevery ctl-ovall ctl-retry ctl-progress ctl-framekey; do mkdir -p "$d/$v"; done
{ region guest/azmod/waveform.c '^/\* Per track position, the fraction of the four stems' '^/\* Re-run EP147.s own "overview changed" path'
  extract guest/azmod/mod_menu_overlay.c '^static void ov_rebuild_attenuation\('; } |
    sed 's/_Atomic //' > "$d/real/watt.inc" # clang: __atomic_* on plain ints
grep -q '^static int build_atten(int deck, const int gq' "$d/real/watt.inc" &&
    grep -q '^static void ov_rebuild_attenuation(void)' "$d/real/watt.inc" ||
    { echo "extraction of build_atten / ov_rebuild_attenuation failed"; exit 2; }
cp guest/azmod/wave_att.h "$d/real/"
for v in ctl-nogain ctl-nocap ctl-norate ctl-deck0 ctl-coarse ctl-ovevery ctl-ovall ctl-retry ctl-progress ctl-framekey; do
    cp "$d/real/watt.inc" "$d/real/wave_att.h" "$d/$v/"
done
# control 1: gains ignored, the curve follows the pad toggles only (the behavior before)
mutate "$d/real/watt.inc" "$d/ctl-nogain/watt.inc" 's/gq\[k\] = g_stem_gain_fn \? watt_gain_q\(g_stem_gain_fn\(d, k\)\) : WATT_GQ_ONE;/gq[k] = WATT_GQ_ONE;/'
# control 2: a boosted stem pushes the factor above unity
mutate "$d/real/wave_att.h" "$d/ctl-nocap/wave_att.h" 's/return \(unsigned short\) \(a > WATT_UNITY \? WATT_UNITY : a\);/return (unsigned short) a;/'
# control 3: every gain change rebuilds at once
mutate "$d/real/wave_att.h" "$d/ctl-norate/wave_att.h" 's/return now_ms - last_ms >= WATT_GAIN_MIN_MS;/return 1;/'
# control 4: deck 1's gains used for every deck
mutate "$d/real/watt.inc" "$d/ctl-deck0/watt.inc" 's/g_stem_gain_fn\(d, k\)/g_stem_gain_fn(0, k)/'
# control 5: the coarse 1/64 gain step
mutate "$d/real/wave_att.h" "$d/ctl-coarse/wave_att.h" 's/#define WATT_GQ_ONE 256 /#define WATT_GQ_ONE 64 /; s/#define WATT_GQ_MAX 512 /#define WATT_GQ_MAX 128 /'
# control 6: the overview tiles refreshed on every gain rebuild (EP147 re-renders each one)
mutate "$d/real/wave_att.h" "$d/ctl-ovevery/wave_att.h" 's/if \(built && keyed\) \{/if (built) {/'
# control 7: one deck's change refreshes every deck's tiles
mutate "$d/real/watt.inc" "$d/ctl-ovall/watt.inc" 's/__atomic_fetch_or\(&g_ov_refresh, 1 << d, /__atomic_fetch_or(&g_ov_refresh, (1 << AZMOD_STEM_DECKS) - 1, /'
# control 8: the every-tick retry while the envelope is built publishes a curve each time
mutate "$d/real/watt.inc" "$d/ctl-retry/watt.inc" 's/        if \(!had\)\n            return 0;[^\n]*\n//'
# control 9: the decode's progress rebuilds on every tick
mutate "$d/real/wave_att.h" "$d/ctl-progress/wave_att.h" 's/return fr != fr_prev && now_ms - last_ms >= WATT_PROG_MIN_MS;/return fr != fr_prev;/'
# control 10: the frame count back in the key (every tick of a decode a keyed rebuild and refresh)
mutate "$d/real/watt.inc" "$d/ctl-framekey/watt.inc" 's/unsigned cur = \(fr > 0 \? 1u << 4 : 0\) \| \(\(eg & 0x07ffffffu\) << 5\);/unsigned cur = ((unsigned) fr << 5) | (eg \& 0u);/'
for v in real ctl-nogain ctl-nocap ctl-norate ctl-deck0 ctl-coarse ctl-ovevery ctl-ovall ctl-retry ctl-progress ctl-framekey; do
    $CC "${CFLAGS[@]}" -pthread -I"$d/$v" -I$T -DWATT_INC="\"$d/$v/watt.inc\"" -o "$d/$v/t" $T/waveatt_test.c
done
suite "wave att" "$d/real/t"
# ---------------------------------------------------------------- folder copy: the FOLDER-view copy plan
# guest/azmod/folder_copy.h natively on a fake file system (foldercopy_test.c)
d="$OUT/foldercopy"
for v in real ctl-dotdot ctl-junk ctl-overwrite ctl-merge ctl-depth ctl-room; do
    mkdir -p "$d/$v"; cp guest/azmod/folder_copy.h "$d/$v/"
done
mutate guest/azmod/folder_copy.h "$d/ctl-dotdot/folder_copy.h" 's/ \|\| !strcmp\(nm, "\.\."\)\)/)/'
mutate guest/azmod/folder_copy.h "$d/ctl-junk/folder_copy.h" 's/    if \(!strncmp\(nm, "\._", 2\)\)\n        return 1;\n//'
mutate guest/azmod/folder_copy.h "$d/ctl-overwrite/folder_copy.h" 's/        if \(st == 0\) \{/        if (st == 0 || (!is_dir \&\& st == 1 \&\& have != size)) {/'
mutate guest/azmod/folder_copy.h "$d/ctl-merge/folder_copy.h" 's/        if \(st == 0\) \{/        if (st == 0 || is_dir) {/'
mutate guest/azmod/folder_copy.h "$d/ctl-depth/folder_copy.h" 's/if \(depth > FC_MAX_DEPTH\) \{/if (0) {/'
mutate guest/azmod/folder_copy.h "$d/ctl-room/folder_copy.h" 's/    if \(p->n >= p->cap\) \{\n        p->err = FC_E_FULL;\n        return 1;\n    \}\n//'
for v in real ctl-dotdot ctl-junk ctl-overwrite ctl-merge ctl-depth ctl-room; do
    $CC "${CFLAGS[@]}" -I"$d/$v" -I$T -o "$d/$v/t" $T/foldercopy_test.c
done
suite "folder copy" "$d/real/t"
# fcopy_wiring: EP147's folder re-read is called only on an object whose vtable is checked, then
# the cached lists are dropped; PASTE of copied files goes to fc_request; the FOLDER menu has no
# UNDO; the library thread services the refresh and every mount cleans an interrupted copy.
fcopy_wiring() { # $1 folder_copy.c $2 playlist_actions.c $3 menu_popup.c $4 playlist_editor.c
    local body
    body=$(extract "$1" '^static void fc_refresh_service\(')
    grep -q 'fc_vt_is(dig, FC_DIGGER_VT))$' <<<"$body" && grep -q 'FC_DIGGER_REREAD' <<<"$body" &&
        grep -q 'FC_DROP_FOLDER' <<<"$body" ||
        { echo "FAIL  folder copy: the re-read is not guarded by the digger's vtable, or the list is not dropped"; return 1; }
    extract "$2" '^static int pl_request_paste\(' | grep -q 'fc_request(g_clip.media' ||
        { echo "FAIL  folder copy: PASTE does not route copied files to fc_request"; return 1; }
    grep -q 'if (w && type != MP_TYPE_FOLDER) {' "$3" ||
        { echo "FAIL  folder copy: the FOLDER menu shows UNDO"; return 1; }
    extract "$4" '^static void mlserver_onmsg_hook\(' | grep -q 'fc_refresh_service(server);' &&
        extract "$4" '^static void mm_note\(' | grep -q 'fc_mount_cleanup(mnt);' ||
        { echo "FAIL  folder copy: the library thread or the mount does not call folder copy"; return 1; }
}
A="guest/azmod"
echo "=== folder copy wiring ==="
if ! plain_only "folder copy wiring"; then :
elif fcopy_wiring $A/folder_copy.c $A/playlist_actions.c $A/menu_popup.c $A/playlist_editor.c; then
    echo "PASS  folder copy: guarded re-read, PASTE route, no UNDO, refresh and cleanup wired"
else failures=$((failures + 1)); fi
mutate $A/folder_copy.c "$d/ctl-noguard.c" 's/ && dig && fc_vt_is\(dig, FC_DIGGER_VT\)\)\n/ \&\& dig)\n/'
mutate $A/playlist_actions.c "$d/ctl-noroute.c" 's/        fc_request\(g_clip\.media, g_clip\.path,[^;]*;\n//'
mutate $A/menu_popup.c "$d/ctl-undo.c" 's/if \(w && type != MP_TYPE_FOLDER\) \{/if (w) {/'
mutate $A/playlist_editor.c "$d/ctl-norefresh.c" 's/\n    fc_refresh_service\(server\);[^\n]*//'
# ---------------------------------------------------------------- Stem Volume vs PRO DJ LINK players 3/4
# guest/azmod/link_members.h natively: the stand-down rule and the member set (two threads);
# stemvol_wiring: ov_sync_stem_volume decides through stemvol_stand_down with the member set,
# and install_static_hooks installs the member watch.
d="$OUT/stemvol"
for v in real ctl-nolink ctl-nowatch ctl-off1; do mkdir -p "$d/$v"; cp guest/azmod/link_members.h "$d/$v/"; done
# control 1: link players ignored (the behavior before)
mutate guest/azmod/link_members.h "$d/ctl-nolink/link_members.h" 's/return \(players & \(\(1ull << 3\) \| \(1ull << 4\)\)\) != 0;/return 0;/'
# control 2: an uninstalled member watch read as "no players"
mutate guest/azmod/link_members.h "$d/ctl-nowatch/link_members.h" 's/if \(!have_track_fn \|\| !watch_ok\)/if (!have_track_fn)/'
# control 3: player numbers off by one (2 and 3)
mutate guest/azmod/link_members.h "$d/ctl-off1/link_members.h" 's/\(1ull << 3\) \| \(1ull << 4\)/(1ull << 2) | (1ull << 3)/'
for v in real ctl-nolink ctl-nowatch ctl-off1; do
    $CC "${CFLAGS[@]}" -pthread -I"$d/$v" -I$T -o "$d/$v/t" $T/stemvol_test.c
done
suite "stem volume link" "$d/real/t"
stemvol_wiring() { # $1 = mod_menu_overlay.c -> 0 when Stem Volume decides through the rule
    local body
    body=$(extract "$1" '^static void ov_sync_stem_volume\(')
    grep -q 'stemvol_stand_down(' <<<"$body" && grep -q 'lm_players()' <<<"$body" && grep -q 'lm_watch_ok()' <<<"$body" ||
        { echo "FAIL  stem volume link: ov_sync_stem_volume does not use stemvol_stand_down with the member set"; return 1; }
    extract "$1" '^static void install_static_hooks\(' | grep -q 'install_link_members();' ||
        { echo "FAIL  stem volume link: install_static_hooks does not install the member watch"; return 1; }
}
echo "=== stem volume link wiring ==="
if ! plain_only "stem volume link wiring"; then :
elif stemvol_wiring guest/azmod/mod_menu_overlay.c; then
    echo "PASS  stem volume link: ov_sync_stem_volume uses the rule; the member watch is installed"
else failures=$((failures + 1)); fi
mutate guest/azmod/mod_menu_overlay.c "$d/ctl-oldrule.c" 's/int busy = stemvol_stand_down\(.*?lm_players\(\)\);/int busy = !g_deck_has_track_fn || g_deck_has_track_fn(2) || g_deck_has_track_fn(3);/s'
mutate guest/azmod/mod_menu_overlay.c "$d/ctl-noinstall.c" 's/\n    install_link_members\(\);[^\n]*//'
# ---------------------------------------------------------------- MENU panel: which first button is refused
# guest/azmod/menu_popup_gate.h natively: only the PLAYLIST list refuses stock ALL DELETE; HISTORY keeps it.
# mpgate_wiring: mp_click_hook refuses the first button only through mp_b1_refused(type).
d="$OUT/mpgate"
for v in real ctl-history; do mkdir -p "$d/$v"; cp guest/azmod/menu_popup_gate.h "$d/$v/"; done
# control 1: HISTORY refused too (the behavior before)
mutate guest/azmod/menu_popup_gate.h "$d/ctl-history/menu_popup_gate.h" 's/return type == 1;/return type == 0 || type == 1;/'
for v in real ctl-history; do
    $CC "${CFLAGS[@]}" -I"$d/$v" -I$T -o "$d/$v/t" $T/mpgate_test.c
done
suite "menu first button" "$d/real/t"
mpgate_wiring() { # $1 = menu_popup.c -> 0 when the first-button refusal goes through the gate
    local body
    body=$(extract "$1" '^static void mp_click_hook\(')
    grep -q 'clicked == k1 && mp_b1_refused(type)' <<<"$body" ||
        { echo "FAIL  menu first button: mp_click_hook refuses the first button without mp_b1_refused"; return 1; }
    grep -q '^#include "menu_popup_gate.h"' "$1" ||
        { echo "FAIL  menu first button: menu_popup.c does not include menu_popup_gate.h"; return 1; }
}
echo "=== menu first button wiring ==="
if ! plain_only "menu first button wiring"; then :
elif mpgate_wiring guest/azmod/menu_popup.c; then
    echo "PASS  menu first button: mp_click_hook refuses through mp_b1_refused"
else failures=$((failures + 1)); fi
mutate guest/azmod/menu_popup.c "$d/ctl-nogate.c" 's/clicked == k1 && mp_b1_refused\(type\)/clicked == k1/'
# ---------------------------------------------------------------- drum roll tile 8 (validated chain)
d="$OUT/drumtile8"; mkdir -p "$d"
extract guest/azmod/drum_roll_display.c '^static void drum8_index_apply\(void\) \{' > "$d/real.inc"
grep -q 'IMAGESCOMP_VT' "$d/real.inc" && grep -q 'PADTILE_VT' "$d/real.inc" || { echo "extraction of drum8_index_apply failed"; exit 2; }
mutate "$d/real.inc" "$d/ctl-notilevt.inc" 's/\(long\) v1\[0\] != \(long\) PADTILE_VT/0/'           # the tile's vtable not checked
mutate "$d/real.inc" "$d/ctl-nocap.inc" 's/\(long\) v1\[1\] != cap/0/'                             # stem child not compared with the capture
mutate "$d/real.inc" "$d/ctl-nocompvt.inc" 's/\(long\) v2\[0\] != \(long\) IMAGESCOMP_VT/0/'     # the component's vtable not checked
mutate "$d/real.inc" "$d/ctl-alwaysours.inc" 's/        if \(want == 0x19 && !shown\[g\]\)\n            continue;[^\n]*\n//'  # the unlatched tile written anyway
mutate "$d/real.inc" "$d/ctl-anydeck.inc" 's/(if \(gdeck < 0 \|\| gdeck >= AZMOD_STEM_DECKS\)\n\s*)continue;/${1}gdeck = 0;/'  # unknown deck -> a default deck
for v in real ctl-notilevt ctl-nocap ctl-nocompvt ctl-anydeck ctl-alwaysours; do
    $CC "${CFLAGS[@]}" -I$T -DTILE_INC="\"$d/$v.inc\"" -o "$d/$v" $T/drumtile8_test.c
done
suite "drum tile 8" "$d/real"
# ---------------------------------------------------------------- drum roll tile 7 (HOLD art)
d="$OUT/drumtile7"; mkdir -p "$d"
{
    grep -E '^#define (SLIP_CHILD_OFF|TILE_MODEL_OFF|MODEL_IDX_OFF|SLIP_ART_CNT) |^static (struct fx_img g_hold_art\[2\]|int g_hold_art_ok);' guest/azmod/drum_roll_display.c
    awk '/^static const int DRUM_IDX_PLAIN/ {p=1} p {print} p && /^#define DRUM_IDX_HOLD / {exit}' guest/azmod/drum_roll_display.c
    for fn in 'static int hold_tile_ready\(long model\)' 'static void drum_tiles_enforce\(void\)'; do
        extract guest/azmod/drum_roll_display.c "^$fn \{"
    done
} > "$d/real.inc"
[ "$(grep -c '^#define \(SLIP_CHILD_OFF\|TILE_MODEL_OFF\|MODEL_IDX_OFF\|SLIP_ART_CNT\|DRUM_IDX_HOLD\) ' "$d/real.inc")" = 5 ] &&
    grep -q 'g_hold_art_ok = 1' "$d/real.inc" && grep -q 'job\[j\].hold' "$d/real.inc" ||
    { echo "extraction of drum_tiles_enforce / hold_tile_ready failed"; exit 2; }
mutate "$d/real.inc" "$d/ctl-blank.inc" 's/\} else if \(slot == 6\) \{/} else if (0) {/'                     # tile 7 left blank
mutate "$d/real.inc" "$d/ctl-nohold.inc" 's/!!\(__atomic_load_n\(&g_drum_hold\[gdeck\], __ATOMIC_RELAXED\) & DRUM_HOLD_ON\)/0/'  # HOLD on not shown
mutate "$d/real.inc" "$d/ctl-onedeck.inc" 's/&g_drum_hold\[gdeck\]/&g_drum_hold[0]/'                        # one HOLD for every deck
mutate "$d/real.inc" "$d/ctl-novt.inc" 's/ \|\| \*\(long \*\) model != \(long\) IMAGESCOMP_VT//'               # component vtable not checked
mutate "$d/real.inc" "$d/ctl-notours.inc" 's/return a\[DRUM_IDX_HOLD\] == [^;]*;/return 1;/s'               # any 20-entry array taken as ours
mutate "$d/real.inc" "$d/ctl-anydeck.inc" 's/(if \(gdeck < 0 \|\| gdeck >= AZMOD_STEM_DECKS\)\n\s*)continue;/${1}gdeck = 0;/'  # unknown deck -> a default deck
for v in real ctl-blank ctl-nohold ctl-onedeck ctl-novt ctl-notours ctl-anydeck; do
    $CC "${CFLAGS[@]}" -I$T -DTILE_INC="\"$d/$v.inc\"" -o "$d/$v" $T/drumtile7_test.c
done
suite "drum tile 7" "$d/real"
# ---------------------------------------------------------------- Stem FX tiles (cyan art)
d="$OUT/sfxtile"; mkdir -p "$d"
{
    grep -E '^static (struct fx_img g_sfx_art\[4\]|int g_sfx_art_ok);' guest/azmod/stems_display.c
    extract guest/azmod/stems_display.c '^static int sfx_tile_index\(long comp, int fx\) \{'
} > "$d/real.inc"
[ "$(grep -c '^static \(struct fx_img g_sfx_art\|int g_sfx_art_ok\)' "$d/real.inc")" = 2 ] &&
    grep -q 'return (int) cnt - 4 + fx;' "$d/real.inc" || { echo "extraction of sfx_tile_index failed"; exit 2; }
mutate "$d/real.inc" "$d/ctl-regrow.inc" 's/if \(!ours\) \{/if (1) {/'                       # grown on every call
mutate "$d/real.inc" "$d/ctl-taken.inc" 's/if \(!ours\) \{/if (0) {/'                        # any array taken as grown
mutate "$d/real.inc" "$d/ctl-nofx.inc" 's/return \(int\) cnt - 4 \+ fx;/return (int) cnt - 4;/'  # one image for every effect
mutate "$d/real.inc" "$d/ctl-norange.inc" 's/ \|\| cnt < 4 \|\| cnt > 256//'                # any count accepted
for v in real ctl-regrow ctl-taken ctl-nofx ctl-norange; do
    $CC "${CFLAGS[@]}" -I$T -DSFX_INC="\"$d/$v.inc\"" -o "$d/$v" $T/sfxtile_test.c
done
suite "Stem FX tiles" "$d/real"
# ---------------------------------------------------------------- drum roll HOLD (pad 7)
d="$OUT/drumhold"; mkdir -p "$d"
for fn in drum_hold_led drum_hold_pads drum_hold_touch mixer_xpad_drum erp_drum_hold; do
    extract guest/azmod/bus_interpose.c "^static (void|unsigned|int) $fn\\("
done > "$d/real.inc"
for fn in drum_hold_led drum_hold_pads drum_hold_touch mixer_xpad_drum erp_drum_hold; do
    grep -qE "^static (void|unsigned|int) $fn\(" "$d/real.inc" || { echo "extraction of $fn failed"; exit 2; }
done
mutate "$d/real.inc" "$d/ctl-pad56.inc" 's/rise8 & 0x40u/rise8 & 0x70u/'                         # pads 5 and 6 toggle too
mutate "$d/real.inc" "$d/ctl-noarm.inc" 's/if \(!armed\) \{/if (0) {/'                            # an unarmed deck holds
mutate "$d/real.inc" "$d/ctl-nostrip.inc" 's/return h == \(DRUM_HOLD_ON \| DRUM_HOLD_SUSTAIN\);/return h != 0;/'  # HOLD plays with no strip touch
mutate "$d/real.inc" "$d/ctl-nolatch.inc" 's/if \(rate_fn && touched\)/if (rate_fn)/'             # untouched frames set the rate
mutate "$d/real.inc" "$d/ctl-global.inc" 's/int h = __atomic_load_n\(&g_drum_hold\[ld\]/int h = __atomic_load_n(&g_drum_hold[0]/'  # one hold for every deck
mutate "$d/real.inc" "$d/ctl-pad56lit.inc" 's/pb\[i \* 4 \+ 0\] = 0x00;/pb[i * 4 + 0] = 0x22;/'        # pads 5 and 6 lit amber
mutate "$d/real.inc" "$d/ctl-noflash.inc" 's/\(blink_on \? 0xFF : 0x00\)/0xFF/'                   # HOLD on lit steady
mutate "$d/real.inc" "$d/ctl-curdeck.inc" 's/prev >= 0 && prev != ld && /prev >= 0 && 0 && /'  # a deck select keeps the hold
mutate "$d/real.inc" "$d/ctl-revive.inc" 's/if \(h == DRUM_HOLD_ON\)\n\s*__atomic_compare_exchange_n\(&g_drum_hold\[ld\], &h, DRUM_HOLD_ON \| DRUM_HOLD_SUSTAIN, 0,\n\s*__ATOMIC_RELAXED, __ATOMIC_RELAXED\);/__atomic_store_n(&g_drum_hold[ld], DRUM_HOLD_ON | DRUM_HOLD_SUSTAIN, __ATOMIC_RELAXED);/'  # the mixer sets SUSTAIN without the CAS
mutate "$d/real.inc" "$d/ctl-mode.inc" 's/drum_hold_pads\(d, ld, held,/drum_hold_pads(d, ld, held \&\& drum,/'  # another pad mode clears the hold
mutate "$d/real.inc" "$d/ctl-noframe.inc" 's/if \(touch_fn && !__atomic_load_n\(&g_xpad_finger, __ATOMIC_RELAXED\)\) \{/if (0) {/'  # HOLD off waits for a mixer frame
for v in real ctl-pad56 ctl-noarm ctl-nostrip ctl-nolatch ctl-global ctl-pad56lit ctl-noflash ctl-curdeck ctl-revive ctl-mode ctl-noframe; do
    $CC "${CFLAGS[@]}" -pthread -I$T -Iguest/azmod -DHOLD_INC="\"$d/$v.inc\"" -o "$d/$v" $T/drumhold_test.c
done
suite "drum roll HOLD" "$d/real"
# ---------------------------------------------------------------- BEAT JUMP 2 First jog display
d="$OUT/bjjog"; mkdir -p "$d"
region guest/azmod/beat_jump.c '^/\* ---- Jog display ---- \*/' '^#### never' > "$d/real.inc"
grep -q 'static void bj_joginfo_hook(long data' "$d/real.inc" && grep -q 'static uint8_t bj_jog_case(' "$d/real.inc" ||
    { echo "extraction of bj_joginfo_hook failed"; exit 2; }
mutate "$d/real.inc" "$d/ctl-noswap.inc" 's/if \(sw != c\) \{/if (0) {/'                         # the store sees EP147's page (the bug)
mutate "$d/real.inc" "$d/ctl-casewrite.inc" 's/memcpy\(info, copy, BJ_JOG_CASE\)/memcpy(info, copy, sizeof copy)/'  # staging left swapped
mutate "$d/real.inc" "$d/ctl-nocopyback.inc" 's/\n\s*memcpy\(info, copy, BJ_JOG_CASE\);[^\n]*//'  # what the store filled in is lost
mutate "$d/real.inc" "$d/ctl-novt.inc" 's/ &&\s*v == BJ_PLAYERINFODATA_VT\)/)/'                   # the vtable not checked
mutate "$d/real.inc" "$d/ctl-nostaging.inc" 's/\(uintptr_t\) info == \(uintptr_t\) data \+ BJ_JOG_STAGING &&//'  # any pointer taken as the staging area
mutate "$d/real.inc" "$d/ctl-novariant.inc" 's/if \(variant != 1\)\n\s*return jcase;\n//'      # every layout variant swapped
for v in real ctl-noswap ctl-casewrite ctl-nocopyback ctl-novt ctl-nostaging ctl-novariant; do
    $CC "${CFLAGS[@]}" -pthread -I$T -DBJ_INC="\"$d/$v.inc\"" -o "$d/$v" $T/bjjog_test.c
done
suite "beat jump jog" "$d/real"
# ---------------------------------------------------------------- ls_tick (library spool)
# ---------------------------------------------------------------- Mod Menu Remove asks first (cm_ask)
d="$OUT/cmask"; mkdir -p "$d"
region guest/azmod/screen_community.c '^/\* A destructive button \(Remove\) asks first' '^/\* mods removed this start' > "$d/real.inc"
grep -q 'static void cm_ask(' "$d/real.inc" || { echo "extraction of cm_ask failed"; exit 2; }
mutate "$d/real.inc" "$d/ctl-retarget.inc" 's/    if \(g_cm_ask.open\)\n        return;\n//'          # a second press retargets the open question
mutate "$d/real.inc" "$d/ctl-noyes.inc" 's/if \(yes && !cm_busy\(\)\)/if (!cm_busy())/'           # sent whatever the answer
mutate "$d/real.inc" "$d/ctl-openany.inc" 's/ == 0;\n\}/ != 2;\n}/'                              # a refused dialog still counts as open
for v in real ctl-retarget ctl-noyes ctl-openany; do
    $CC "${CFLAGS[@]}" -I$T -DASK_INC="\"$d/$v.inc\"" -o "$d/$v" $T/cmask_test.c
done
suite "cm_ask" "$d/real"
d="$OUT/lsspool"; mkdir -p "$d"
extract guest/azmod/library_edits.c '^static void ls_tick\(' > "$d/tick.inc"
extract guest/azmod/library_edits.c '^static void ls_source_hint\(' > "$d/hint.inc"
grep -q 'g_ls_leftover' "$d/tick.inc" || { echo "extraction of ls_tick failed"; exit 2; }
grep -q 'cross-stick copy from' "$d/hint.inc" || { echo "extraction of ls_source_hint failed"; exit 2; }
# control 1: the old gate (a leftover spool waits for the next copy)
mutate "$d/tick.inc" "$d/tick-ctl-gate.inc" 's/if \(!copying && !leftover\)/if (!copying)/'
# control 2: the leftover never cleared (scans every 2 s forever)
mutate "$d/tick.inc" "$d/tick-ctl-noclear.inc" 's/__atomic_store_n\(&g_ls_leftover, 0, __ATOMIC_RELEASE\);/;/'
# control 3: the service armed with nothing waiting for it (a scan every 2 s, any spool applied)
mutate "$d/tick.inc" "$d/tick-ctl-idle.inc" 's/if \(!copying && !leftover\)/if (0)/'
# control 4: the source hint takes any path
mutate "$d/hint.inc" "$d/hint-ctl-any.inc" 's/ \|\| memchr\(name, .\/., k\)//'
for v in real ctl-gate ctl-noclear ctl-idle ctl-any; do
    ti="$d/tick.inc"; hi="$d/hint.inc"
    case $v in ctl-gate) ti="$d/tick-ctl-gate.inc";; ctl-noclear) ti="$d/tick-ctl-noclear.inc";; ctl-idle) ti="$d/tick-ctl-idle.inc";; ctl-any) hi="$d/hint-ctl-any.inc";; esac
    $CC "${CFLAGS[@]}" -I$T -DTICK_INC="\"$ti\"" -DHINT_INC="\"$hi\"" -o "$d/$v" $T/lsspool_test.c
    mkdir -p "$d/root-$v"
done
suite "ls_tick" "$d/real" "$d/root-real"
# ---------------------------------------------------------------- pdb_stock
d="$OUT/pdbstock"; mkdir -p "$d"
cp guest/azmod/pdb_stock.c "$d/real.c"
# control 1: a taken entry is not cleared (every tick after the debounce syncs again)
mutate "$d/real.c" "$d/ctl-noclear.c" 's/p->e\[i\]\.media = 0;\n(\s*)got = 1;/got = 1;/'
# control 2: no debounce (due at the write itself, so a burst syncs mid-burst)
mutate "$d/real.c" "$d/ctl-nodebounce.c" 's/p->e\[k\]\.due = now \+ PST_DEBOUNCE_MS;/p->e[k].due = now;/'
# control 3: a stock write never scheduled (as a Playlist Editing gate would do when off): the
# suite must then see no mirror, so this is what proves stock writes do reach export.pdb
mutate "$d/real.c" "$d/ctl-nomirror.c" 's/(static int pst_note\(struct pst \*p, unsigned media, long long now\) \{\n)/$1    return 0; \/* gated off: no stock sync *\/\n/'
# control 4: an unresolved mount still syncs
mutate "$d/real.c" "$d/ctl-nomount.c" 's/if \(!mount\(ctx, root, sizeof root\) \|\| root\[0\] != .\/.\)\n\s*return -1;/mount(ctx, root, sizeof root);/'
# control 5: media 0 (an unnamed library) noted
mutate "$d/real.c" "$d/ctl-media0.c" 's/    if \(!media\)\n        return 0;/    if (0)\n        return 0;/'
for v in real ctl-noclear ctl-nodebounce ctl-nomirror ctl-nomount ctl-media0; do
    $CC "${CFLAGS[@]}" -I$T -DPST_INC="\"$d/$v.c\"" -o "$d/$v" $T/pdbstock_test.c -lpthread
done
suite "pdb_stock" "$d/real"
# ---------------------------------------------------------------- the spool's write on the target stick
# cp_spool_write (library_edits.c): whole or not at all, also on a full stick (cpwrite_test.c)
d="$OUT/cpwrite"; mkdir -p "$d"
extract guest/azmod/library_edits.c '^static int cp_spool_write\(' > "$d/real.inc"
grep -q 'rename(tmp, path)' "$d/real.inc" || { echo "extraction of cp_spool_write failed"; exit 2; }
# control: the write's results ignored (the old code: a short spool renamed into place)
mutate "$d/real.inc" "$d/ctl-unchecked.inc" 's/if \(!ok \|\| rename\(tmp, path\) != 0\)/if (rename(tmp, path) != 0)/'
for v in real ctl-unchecked; do
    $CC "${CFLAGS[@]}" -I$T -DCPWRITE_INC="\"$d/$v.inc\"" -o "$d/$v" $T/cpwrite_test.c
done
suite "spool write" "$d/real" "$d/scratch-real"

# ---------------------------------------------------------------- azmod.json
# the community manifest's scanner (community.c cm_json_*): every refusal by name, the values,
# the firmware list (cmjson_test.c; tools/fuzz cmjson fuzzes the same code)
d="$OUT/cmjson"; mkdir -p "$d"
awk '/^static const char \*cm_json_ws\(/ {p=1} p {print} p && /^static int cm_json_fw\(/ {q=1} q && /^}$/ {exit}' guest/azmod/community.c > "$d/real.inc"
grep -q 'cm_json_walk' "$d/real.inc" && grep -q 'static int cm_json_fw' "$d/real.inc" || { echo "extraction of cm_json_* failed"; exit 2; }
# control: the repeated-key check dropped
mutate "$d/real.inc" "$d/ctl-repeat.inc" 's/                if \(why\) \*why = "azmod.json repeats a key";\n                return NULL;/                (void) 0;/'
# control: a nested object taken as a bare value
mutate "$d/real.inc" "$d/ctl-nested.inc" 's/\} else if \(\*p == .\{.\) \{\n            if \(why\) \*why = "azmod.json holds a nested object";\n            return NULL;\n        \}/}/'
# control: keys past the 48 remembered taken unchecked (a repeat after them goes unseen)
mutate "$d/real.inc" "$d/ctl-many.inc" 's/        if \(nseen == 48\) \{ [^\n]*\n            if \(why\) \*why = "azmod.json has too many keys";\n            return NULL;\n        \}\n        seen\[nseen\+\+\] = k0;/        if (nseen < 48)\n            seen[nseen++] = k0;/'
for v in real ctl-repeat ctl-nested ctl-many; do
    $CC "${CFLAGS[@]}" -I$T -DCMJSON_INC="\"$d/$v.inc\"" -o "$d/$v" $T/cmjson_test.c
done
suite "azmod.json" "$d/real"

# ---------------------------------------------------------------- the core's own dialog
# community_tick (community.c) services the confirm dialog (sdk_ui.c) for the core's questions
# with no community mod loaded or running (coretick_test.c)
d="$OUT/coretick"; mkdir -p "$d"
extract_coretick() { # $1 = community.c variant
    grep -E '^static pthread_mutex_t g_sd_cmu|^static struct sd_confirm g_sd_confirm' guest/azmod/sdk_ui.c
    awk '/^\/\* The dialog on screen: UI thread only. \*\/$/ {p=1; next} p {print} p && /^\} g_sd_dlg;$/ {exit}' guest/azmod/sdk_ui.c
    extract guest/azmod/sdk_ui.c '^static void sd_deliver\('
    extract guest/azmod/sdk_ui.c '^static void sd_dlg_close\(int yes\) \{'
    extract guest/azmod/sdk_ui.c '^static int sd_confirm_take\('
    extract guest/azmod/sdk_ui.c '^int azcm_confirm\('
    extract guest/azmod/sdk_ui.c '^static void sdk_confirm_tick\('
    extract guest/azmod/sdk_ui.c '^static void sdk_ui_tick\('
    extract "$1" '^static void community_tick\(void\) \{'
}
cp guest/azmod/community.c "$d/real.c"
# control: the dialog serviced only after the early returns (a deck with no running mod then
# never shows the core's questions)
mutate "$d/real.c" "$d/ctl-late.c" 's/    sdk_confirm_tick\(\); \/\* the dialog: [^\n]*\n//; s/(    sdk_ui_tick\(\);  )/    sdk_confirm_tick();\n$1/'
for v in real ctl-late; do
    extract_coretick "$d/$v.c" > "$d/$v.inc"
    grep -q 'g_sd_dlg;' "$d/$v.inc" && grep -q 'static void community_tick' "$d/$v.inc" && grep -q 'int azcm_confirm' "$d/$v.inc" ||
        { echo "extraction of the core dialog path failed ($v)"; exit 2; }
    $CC "${CFLAGS[@]}" -pthread -Iguest/azmod -I$T -DCORETICK_INC="\"$d/$v.inc\"" -o "$d/$v" $T/coretick_test.c
done
suite "core dialog" "$d/real"

# ---------------------------------------------------------------- the library gate's bounds
# community.c cm_elf_span / cm_elf_strtab: inside the file, no 64-bit wrap, aligned, NUL-ended
# (elfspan_test.c; tools/fuzz elfcheck fuzzes the whole gate)
d="$OUT/elfspan"; mkdir -p "$d"
{ extract guest/azmod/community.c '^static int cm_elf_span\('
  extract guest/azmod/community.c '^static int cm_elf_strtab\('; } > "$d/real.inc"
grep -q 'cm_elf_strtab' "$d/real.inc" || { echo "extraction of cm_elf_span failed"; exit 2; }
# control: the span tested as off + len <= sz (wraps in 64 bits)
mutate "$d/real.inc" "$d/ctl-wrap.inc" 's/return off <= sz && len <= sz - off && off % align == 0;/return off + len <= sz \&\& off % align == 0;/'
# control: a string table taken without its final NUL
mutate "$d/real.inc" "$d/ctl-nul.inc" 's/ && !b\[ss->sh_offset \+ ss->sh_size - 1\];/;/'
for v in real ctl-wrap ctl-nul; do
    $CC "${CFLAGS[@]}" -I$T -Itools/fuzz/compat -DELFSPAN_INC="\"$d/$v.inc\"" -o "$d/$v" $T/elfspan_test.c
done
suite "elf gate spans" "$d/real"

# ---------------------------------------------------------------- the approved list
# community.c cm_approve and the files beside it: whole or not at all, under the fetcher's lock
# (cmapprove_test.c)
d="$OUT/cmapprove"; mkdir -p "$d"
{ extract guest/azmod/community.c '^static int cm_id_ok\(const char \*id\) \{'
  extract guest/azmod/community.c '^static int cm_write_line\('
  perl -0ne 'print $1 if /(static struct \{\n    char id\[33\], sha\[65\];\n\} g_cm_ok.*?\nstatic int cm_sha256_file\(.*?\n\}\n)/s' guest/azmod/community.c
} > "$d/real.inc"
grep -q 'static int cm_approve' "$d/real.inc" && grep -q 'cm_sha256_file' "$d/real.inc" && grep -q 'cm_write_line' "$d/real.inc" ||
    { echo "extraction of the approved list failed"; exit 2; }
# control: written without the lock (the fetcher's write can be lost between read and rename)
mutate "$d/real.inc" "$d/ctl-nolock.inc" 's/    if \(!locked\) \{\n        close\(lk\);\n        return -1;\n    \}/    (void) locked;/'
# control: a failed write leaves memory ahead of the disk
mutate "$d/real.inc" "$d/ctl-memory.inc" 's/    if \(rc\) cm_approved_read\(\);[^\n]*//'
for v in real ctl-nolock ctl-memory; do
    $CC "${CFLAGS[@]}" -Iguest/common -I$T -DCMAPPROVE_INC="\"$d/$v.inc\"" -DCM_TEST_ROOT="\"$d/scratch-$v\"" \
        -o "$d/$v" $T/cmapprove_test.c
done
suite "approved list" "$d/real"

# ---------------------------------------------------------------- the library spool's applier
# ls_apply_buf + ls_allowed (library_edits.c) and lp_exec (library_db.c) on a real SQLite library:
# a stick's spool file is applied whole or not at all, only the allowed statements, and pass 2
# runs exactly the lines pass 1 checked (lsapply_test.c)
d="$OUT/lsapply"; mkdir -p "$d"
extract_lsapply() { # $1 = library_edits.c variant
    extract guest/azmod/library_db.c '^static int lp_exec\(void \*conn, const char \*sql, FILE \*lg\) \{'
    extract "$1" '^static const char \*const LS_TABLES\[\]'
    extract "$1" '^static void ls_source_hint\('
    grep -E '^#define LS_SPOOL_CAP ' "$1"
    extract "$1" '^static int ls_apply_buf\('
}
extract_lsapply guest/azmod/library_edits.c > "$d/real.inc"
grep -q 'static int ls_allowed' "$d/real.inc" && grep -q 'ls_pdb_sync(conn, path, hint, lg);' "$d/real.inc" &&
    grep -q 'static int lp_exec' "$d/real.inc" || { echo "extraction of ls_apply_buf failed"; exit 2; }
# control: any statement allowed
mutate "$d/real.inc" "$d/ctl-anystmt.inc" 's/if \(!ls_allowed\(l\) \|\| /if (/'
# control: a table name taken by its prefix (contentx passes as content)
mutate "$d/real.inc" "$d/ctl-prefix.inc" 's/ && \(t\[n\] == . . \|\| t\[n\] == .\(.\)//'
# control: a second statement after ';' on the line allowed
mutate "$d/real.inc" "$d/ctl-inner.inc" 's/ \|\| memchr\(l, .;., k - 1\)//'
# control: a failed statement committed what ran before it
mutate "$d/real.inc" "$d/ctl-commit.inc" 's/lp_exec\(conn, rc \? "ROLLBACK" : "COMMIT", 0\);/lp_exec(conn, "COMMIT", 0);/'
# control: applied without a backup of the library
mutate "$d/real.inc" "$d/ctl-nobackup.inc" 's/if \(lib_backup\(conn, lg\)\) \{/if (0) {/'
# control: the trailing-blank trim written into the buffer (pass 2 then stops at that line)
mutate "$d/real.inc" "$d/ctl-trimnul.inc" 's/            k--; \/\* measured only[^\n]*/            l[--k] = 0;/'
# control: a 0x01 inside a line allowed (pass 2 splits there and runs the rest unchecked)
mutate "$d/real.inc" "$d/ctl-mark.inc" 's/ \|\|\n\s*memchr\(l, 0x01, k\)\)/)/'
# control: a NUL byte accepted (the statements after it are dropped, the spool reported applied)
mutate "$d/real.inc" "$d/ctl-nul.inc" 's/    if \(memchr\(buf, 0, n\)\) \{/    if (0) {/'
for v in real ctl-anystmt ctl-prefix ctl-inner ctl-commit ctl-nobackup ctl-trimnul ctl-mark ctl-nul; do
    $CC "${CFLAGS[@]}" -I$T -DLSAPPLY_INC="\"$d/$v.inc\"" -o "$d/$v" $T/lsapply_test.c -lsqlite3
done
suite "spool apply" "$d/real" "$d/scratch-real"

# ---------------------------------------------------------------- cross-stick copy spool
d="$OUT/cpspool"; mkdir -p "$d"
extract_cp() {   # $1 = library_edits.c variant -> the spool builder and what it needs
    awk '/^enum \{$/ {e=1; buf=$0; next}
         e==1 { buf=buf"\n"$0; if ($0 ~ /^    (CPF_TITLE|CPK_TRACK),$/) e=2; else {e=0; buf=""}; if (e==2) {print buf; e=3}; next }
         e==3 { print; if ($0 ~ /^};$/) e=0; next }
         /^struct cp_trk \{|^struct cp_node \{/ {p=1}
         /^static struct \{$/ {g=1; gb=""}
         g { gb=gb $0 "\n"; if ($0 ~ /^\} g_cp;$/) {printf "%s", gb; g=0} else if ($0 ~ /^\}/) g=0; next }
         /^#define LS_ART_TAG |^#define CP_NEW_PLID / {print; next}
         /^static void cp_lit\(|^static void cp_put\(|^static char \*cp_track_sql\(|^static char \*cp_folder_sql\(|^static int cp_build_spool\(/ {p=1}
         p { print; if ($0 ~ /^\};?$/) p=0 }' "$1"
}
cp guest/azmod/library_edits.c "$d/real.c"
# control: every track deduplicated against the destination (the old spool), new list or not
mutate "$d/real.c" "$d/ctl-dedupe.c" 's/plguard, !mark\);/plguard, 1);/'
# control: only line breaks blanked in a literal (a 0x01 in a title reaches the spool)
mutate "$d/real.c" "$d/ctl-ctlbyte.c" "s/\\(unsigned char\\) \\*p < 0x20 && \\*p != '\\\\t'/*p == '\\\\n' || *p == '\\\\r'/"
for v in real ctl-dedupe ctl-ctlbyte; do
    extract_cp "$d/$v.c" > "$d/$v.inc"
    grep -q 'cp_build_spool' "$d/$v.inc" && grep -q 'struct cp_trk' "$d/$v.inc" && grep -q 'CPF_N' "$d/$v.inc" \
        || { echo "extraction of the cross-stick spool failed ($v)"; exit 2; }
    $CC "${CFLAGS[@]}" -I$T -DCP_INC="\"$d/$v.inc\"" -o "$d/$v" $T/cpspool_test.c -lsqlite3
done
suite "cross-stick spool" "$d/real"
# a FOLDER onto the other stick (cpfolder_test.c), with the PASTE router's "onto itself" test
cp guest/azmod/playlist_actions.c "$d/pa-real.c"
# control: every row of the tree hung directly under the new folder (nesting lost)
mutate "$d/real.c" "$d/ctl-flat.c" 's/, mark - nd->parent\);/, mark);/'
# control: new ids from max + 1 alone (below the 16385 EP147's own playlists start at)
mutate "$d/real.c" "$d/ctl-nofloor.c" 's/MAX\(16384,COALESCE\(MAX\(playlist_id\),0\)\)\+1/COALESCE(MAX(playlist_id),0)+1/'
# control: only the new folder's own mark cleared (the rows inside keep negative image_ids)
mutate "$d/real.c" "$d/ctl-onemark.c" 's/mark - \(g_cp\.nnode - 1\), mark\);/mark, mark);/'
# control: the same id on the other stick taken as the copied row itself
mutate "$d/pa-real.c" "$d/pa-ctl-selfany.c" 's/return same_stick && dest_row == clip_id;/return (void) same_stick, dest_row == clip_id;/'
for v in real ctl-flat ctl-nofloor ctl-onemark selfany; do
    src=$v; pa=real
    [ "$v" = selfany ] && { src=real; pa=ctl-selfany; }
    extract_cp "$d/$src.c" > "$d/fd-$v.inc"
    extract "$d/pa-$pa.c" '^static int pl_paste_is_self\(' > "$d/fd-pa-$v.inc"
    grep -q 'cp_folder_sql' "$d/fd-$v.inc" && grep -q 'struct cp_node' "$d/fd-$v.inc" && grep -q 'CPK_FOLDER' "$d/fd-$v.inc" \
        && grep -q 'pl_paste_is_self' "$d/fd-pa-$v.inc" \
        || { echo "extraction of the cross-stick folder copy failed ($v)"; exit 2; }
    $CC "${CFLAGS[@]}" -I$T -DCP_INC="\"$d/fd-$v.inc\"" -DPASTE_INC="\"$d/fd-pa-$v.inc\"" -o "$d/fd-$v" $T/cpfolder_test.c -lsqlite3
done
suite "cross-stick folder copy" "$d/fd-real"
# the pasted art for a track the target already has: the spool's art lines + ls_art_fix
# control 1: the spool without its art lines (a reused row keeps the target's broken art)
mutate "$d/real.c" "$d/ctl-noartline.c" 's/if \(img_dst\[0\] && !strpbrk\(t->f\[CPF_PATH\]/if (0 \&\& img_dst[0] \&\& !strpbrk(t->f[CPF_PATH]/'
# control 2: art that is on the stick replaced too
mutate "$d/real.c" "$d/ctl-artover.c" 's/if \(!ps_file_ok\(root, cur \? \(const char \*\) cur : 0\)\)/if (1)/'
for v in real ctl-noartline ctl-artover; do
    { grep -m1 '^#define PS_AZCP_ART ' guest/azmod/pdb_sync.c
      extract guest/azmod/pdb_sync.c '^static int ps_file_ok\('
      extract_cp "$d/$v.c"
      extract "$d/$v.c" '^static int ls_art_fix\('; } > "$d/art-$v.inc"
    grep -q 'ls_art_fix' "$d/art-$v.inc" && grep -q 'ps_file_ok' "$d/art-$v.inc" && grep -q 'LS_ART_TAG "' "$d/art-$v.inc" \
        || { echo "extraction of the spool's art fix failed ($v)"; exit 2; }
    $CC "${CFLAGS[@]}" -I$T -DCP_INC="\"$d/art-$v.inc\"" -o "$d/art-$v" $T/cpart_test.c -lsqlite3 -ldl
done
suite "cross-stick spool: artwork" "$d/art-real"
# ---------------------------------------------------------------- export.pdb sync: a paste's art
d="$OUT/pdbart"; mkdir -p "$d"
cp guest/azmod/pdb_sync.c "$d/real.c"
# control 1: the paste's-art step removed (the old sync: a listed track keeps its missing art)
mutate "$d/real.c" "$d/ctl-noart.c" 's/int arts = ps_paste_art\(&d, root, &dloc, trk, ntrk\);/int arts = 0;/'
# control 2: a row's own art replaced although its file is on the stick
mutate "$d/real.c" "$d/ctl-artover.c" 's/ \|\| \(cp && ps_file_ok\(root, cp\)\)//'
# control 3: a copied track row gets no art when the source stick's row has none
mutate "$d/real.c" "$d/ctl-srcnoart.c" 's/if \(!sid && typ != PT_ART\)/if (!sid)/'
for v in real ctl-noart ctl-artover ctl-srcnoart; do
    $CC "${CFLAGS[@]}" -I$T -DPDBS_INC="\"$d/$v.c\"" -o "$d/$v" $T/pdbart_test.c
done
suite "export.pdb: a paste's artwork" "$d/real"
# ---------------------------------------------------------------- export.pdb sync: paths, names, pulls
d="$OUT/pdbsync2"; mkdir -p "$d"
cp guest/azmod/pdb_sync.c "$d/real.c"
# control 1: a copied row keeps the source's analysis path (another track's files on the target)
mutate "$d/real.c" "$d/ctl-anlz.c" 's/if \(trk\[i\]\.anlz && trk\[i\]\.anlz\[0\]\) \{/if (0) {/'
# control 2: ill-formed UTF-8 encoded byte by byte (the old encoder: half a character, garbage)
mutate "$d/real.c" "$d/ctl-encbad.c" 's/            c = 0xfffd;/            c = u[0];/; s/if \(n && i \+ j == L && j < n\) \{/if (0) {/'
# control 3: a name too long for its row stops the whole sync (the old abort)
mutate "$d/real.c" "$d/ctl-longabort.c" 's/(        if \(fl && lg\) \/\* written as it fits)/        if (fl & PS_ENC_CUT) {\n            free(ord);\n            goto out;\n        }\n$1/'
# control 4: export.pdb rewritten in place (no temporary file, no rename): a pull tears it
mutate "$d/real.c" "$d/ctl-inplace.c" 's/int fd = open\(tmp, O_RDWR \| O_CREAT \| O_EXCL \| O_CLOEXEC, 0644\)/int fd = open(path, O_RDWR | O_CLOEXEC, 0644)/; s/if \(rename\(tmp, path\)\) \/\* \(3\) \*\//if (0) \/* (3) *\//'
# control 5: a stale temporary file removed even when it may share export.pdb's clusters
mutate "$d/real.c" "$d/ctl-stale.c" 's/\(ps_same_bytes\(tmp, path\) \|\| unlink\(tmp\)\)/unlink(tmp)/'
# control 6: a missing export.pdb not put back from the complete temporary file
mutate "$d/real.c" "$d/ctl-norecover.c" 's/if \(ps_recover\(path, lg\) < 0 && lg\)/if (0 \&\& ps_recover(path, lg) < 0 \&\& lg)/'
# control 7: the free list left as it was after a table shrinks (a page listed or missing)
mutate "$d/real.c" "$d/ctl-freelist.c" 's/(static int ps_free_list\(ps_buf \*d, unsigned typ\) \{\n)/$1    if (d)\n        return 0;\n/'
# control 8: a surplus page left with 0 slots (rekordbox 7's runaway allocation)
mutate "$d/real.c" "$d/ctl-tomb0.c" 's/(memset\(pg \+ PS_HDR, 0, d->pl - PS_HDR\);[^\n]*\n)    pg\[0x18\] = 1;/$1    pg[0x18] = 0;/'
for v in real ctl-anlz ctl-encbad ctl-longabort ctl-inplace ctl-stale ctl-norecover ctl-freelist ctl-tomb0; do
    $CC "${CFLAGS[@]}" -I$T -DPDBS_INC="\"$d/$v.c\"" -o "$d/$v" $T/pdbsync2_test.c
done
suite "export.pdb sync" "$d/real"
# two sticks at once: the threaded case runs under ThreadSanitizer when the host compiler has
# it (a shared work buffer used by both syncs is a data race there); the control drops the lock
d="$OUT/pdbthreads"; mkdir -p "$d"
cp guest/azmod/pdb_sync.c "$d/real.c"
mutate "$d/real.c" "$d/ctl-nolock.c" 's/    pthread_mutex_lock\(&g_ps_lock\);\n//; s/    pthread_mutex_unlock\(&g_ps_lock\);\n//'
$CC "${CFLAGS[@]}" -I$T -DPDBS_INC="\"$d/real.c\"" -o "$d/real" $T/pdbthreads_test.c -lpthread
suite "export.pdb sync: two sticks at once" "$d/real" 10
PT_TSAN=0
if [ -n "$SAN" ]; then
    echo "(sanitizer pass: the two-stick ThreadSanitizer case runs in the plain pass; this pass built the suite itself with $SAN)"
elif AZ_CC_PROBE=1 $CC -std=gnu11 -O1 -g -fsanitize=thread -I$T -DPDBS_INC="\"$d/real.c\"" -o "$d/tsan-real" $T/pdbthreads_test.c -lpthread 2>/dev/null &&
   AZ_CC_PROBE=1 $CC -std=gnu11 -O1 -g -fsanitize=thread -I$T -DPDBS_INC="\"$d/ctl-nolock.c\"" -o "$d/tsan-ctl-nolock" $T/pdbthreads_test.c -lpthread 2>/dev/null; then
    PT_TSAN=1
    suite "export.pdb sync: two sticks at once (ThreadSanitizer)" env TSAN_OPTIONS=halt_on_error=1 "$d/tsan-real" 10
else
    skip "export.pdb sync: two sticks at once (ThreadSanitizer): $CC has no -fsanitize=thread"; echo
fi
# ---------------------------------------------------------------- names cut at a character boundary
d="$OUT/utf8cut"; mkdir -p "$d"
cp guest/azmod/utf8_cut.h "$d/real.h"
# control: utf8_cut a no-op (the byte-wise cut of the old code)
mutate "$d/real.h" "$d/ctl-bytewise.h" 's/(static inline void utf8_cut\(char \*s\) \{\n)/$1    return;\n/'
extract guest/azmod/library_db.c '^static int sq_text\(' > "$d/sqtext.inc"
extract guest/azmod/ep_mem.h '^static inline int own_str\(' > "$d/ownstr.inc"
grep -q 'utf8_cut' "$d/sqtext.inc" && grep -q 'utf8_cut' "$d/ownstr.inc" || { echo "extraction of sq_text / own_str failed"; exit 2; }
for v in real ctl-bytewise; do
    $CC "${CFLAGS[@]}" -I$T -DUTF8_INC="\"$d/$v.h\"" -DSQTEXT_INC="\"$d/sqtext.inc\"" -DOWNSTR_INC="\"$d/ownstr.inc\"" \
        -o "$d/$v" $T/utf8cut_test.c
done
suite "utf8 cut" "$d/real"
# ---------------------------------------------------------------- export.pdb sync: library read
d="$OUT/lspdbsync"; mkdir -p "$d"
awk '/^struct pdbs_pl \{/ {p=1} p {print} p && /^\};/ && ++n == 3 {exit}' guest/azmod/pdb_sync.c > "$d/structs.inc"
extract guest/azmod/library_edits.c '^static char \*ls_dup\(' > "$d/dup.inc"
extract guest/azmod/library_edits.c '^static int ls_pdb_sync_root\(' > "$d/real.inc"
grep -q 'struct pdbs_track' "$d/structs.inc" || { echo "extraction of the pdbs structs failed"; exit 2; }
grep -q 'strdup' "$d/dup.inc" || { echo "extraction of ls_dup failed"; exit 2; }
grep -q 'pdbs_sync(root' "$d/real.inc" || { echo "extraction of ls_pdb_sync_root failed"; exit 2; }
# control: a failed copy not checked (its NULL row handed to pdbs_sync)
mutate "$d/real.inc" "$d/ctl-nullrow.inc" 's/\n *if \(!(?:pl|it|tr)\[[^\n]*\n[^\n]*bad = 1;[^\n]*//g'
for v in real ctl-nullrow; do
    $CC "${CFLAGS[@]}" -I$T -DPDBS_STRUCTS_INC="\"$d/structs.inc\"" -DLSDUP_INC="\"$d/dup.inc\"" \
        -DLSSYNC_INC="\"$d/$v.inc\"" -o "$d/$v" $T/lspdbsync_test.c
done
LSP_BIN="$d/real"
if [ -z "$SAN" ] && AZ_CC_PROBE=1 $CC "${CFLAGS[@]}" -fsanitize=address,undefined -fno-sanitize-recover=undefined -g -I$T \
        -DPDBS_STRUCTS_INC="\"$d/structs.inc\"" -DLSDUP_INC="\"$d/dup.inc\"" \
        -DLSSYNC_INC="\"$d/real.inc\"" -o "$d/real-san" $T/lspdbsync_test.c 2>"$d/san.log"; then
    LSP_BIN="$d/real-san"; echo "(export.pdb sync read: running under -fsanitize=address,undefined)"
elif [ -n "$SAN" ]; then
    echo "(export.pdb sync read: instrumented by the $SAN pass)"
else
    echo "(export.pdb sync read: sanitizers unavailable with $CC, plain build; $(head -1 "$d/san.log"))"
fi
suite "export.pdb sync: library read" "$LSP_BIN"
# ---------------------------------------------------------------- drumroll
# The whole drumroll.c is #included by the test (DRUM_INC), stubs for the rest of azstem
# in drumroll_test.c. Controls are copies of drumroll.c with one protection removed.
d="$OUT/drum"; mkdir -p "$d/scratch"
cp guest/azstem/drumroll.c "$d/real.c"
# control 1: the format code ignored (any tag but float decodes as PCM: A-law, mu-law)
mutate "$d/real.c" "$d/ctl-tag.c" 's/    if \(tag == 1\) \{/    if (tag != 3) {/'
# control 2: any PCM bit depth accepted (12-bit, 20-bit)
mutate "$d/real.c" "$d/ctl-depth.c" 's/return bits == 8 \|\| bits == 16 \|\| bits == 24 \|\| bits == 32;/return bits >= 8;/'
# control 3: no length cap
mutate "$d/real.c" "$d/ctl-cap.c" 's/#define DRUM_MAX_SECONDS 8/#define DRUM_MAX_SECONDS 100000/'
# control 4: the base is never re-checked (the old resolve-once behavior)
mutate "$d/real.c" "$d/ctl-resolve.c" 's/if \(!drum_base_same\(\)\) \{/if (0) {/'
# control 5: the swap does not wait for the audio thread
mutate "$d/real.c" "$d/ctl-quiesce.c" 's/if \(!drum_rt_quiesce\(\)\) \{/if (0) {/'
# control 6: Beat Sync trusted on a stopped deck (the old behavior: a paused or ejected deck
# waits forever for a grid boundary and the roll is silent)
mutate "$d/real.c" "$d/ctl-stall.c" 's/#define DRUM_SYNC_STALL 4410L/#define DRUM_SYNC_STALL (1L << 41)/'
# control 7: Beat Sync never used (a playing deck fires at the press, off the grid)
mutate "$d/real.c" "$d/ctl-nosync.c" 's/return \(s1 == s2\) && valid && beat >= 1\.0;/return 0;/'
# control 8: the grid's rate assumed (the old behavior: a 48 kHz grid published as 44.1 kHz)
mutate "$d/real.c" "$d/ctl-norate.c" 's/double to_play = \(double\) AZ_EP147_RATE \/ \(double\) rate;/double to_play = 1.0;/'
# control 9: the measured rate never checked (a grid whose tempo and spacing disagree is used)
mutate "$d/real.c" "$d/ctl-anyrate.c" 's/rate = bg_grid_rate\(head, k\);/rate = AZ_EP147_RATE;/'
# control 10: the grid seqlock without its fences: no release
# fence after the odd store, no acquire fence before the audio reader's second look at seq.
# Checked in the source by seqlock-lint.sh, on every host: a threaded run of this copy tears
# only when the CPU happens to reorder inside the window, so as a run it passed by chance.
mutate "$d/real.c" "$d/ctl-gridfence.c" 's/(\/\* -> odd \*\/\n)    atomic_thread_fence\(memory_order_release\);\n/$1/; s/(int valid = atomic_load_explicit\(&g->valid, memory_order_relaxed\);\n)    atomic_thread_fence\(memory_order_acquire\); [^\n]*\n    unsigned s2 = atomic_load_explicit\(&g->seq, memory_order_relaxed\);\n    if \(s1 != s2/$1    unsigned s2 = atomic_load_explicit(&g->seq, memory_order_acquire);\n    if (s1 != s2/'
# control 11: the hit age ignored (a hit fired early against one reading is fired again at the
# next: a flam)
mutate "$d/real.c" "$d/ctl-hitage.c" 's/            if \(age \+ tonext < period \/ 2\)\n                tonext \+= period;\n            else if/            (void) age;\n            if (0)\n                tonext += period;\n            else if/'
# control 12: decks 3 and 4's tone glide starting at zero (their first hit fades in from silence)
mutate "$d/real.c" "$d/ctl-glide.c" 's/sm_gain\[DRUM_SIDES\] = \{1\.f, 1\.f, 1\.f, 1\.f\}/sm_gain[DRUM_SIDES] = {1.f, 1.f}/'
# control 13: the loaded bank reloaded at every stick discovery, Drum Roll on or off
mutate "$d/real.c" "$d/ctl-bank0.c" 's/int req = atomic_load_explicit\(&g_drum_req_bank, memory_order_relaxed\);/int req = atomic_load_explicit(\&g_drum_bank, memory_order_relaxed);/'
for v in real ctl-tag ctl-depth ctl-cap ctl-resolve ctl-quiesce ctl-stall ctl-nosync ctl-norate ctl-anyrate ctl-hitage ctl-glide ctl-bank0; do
    $CC "${CFLAGS[@]}" -Wno-unused-parameter -I$T -Iguest/azstem -DDRUM_INC="\"$d/$v.c\"" -o "$d/$v" $T/drumroll_test.c -lpthread -lm
done
suite "drumroll" "$d/real" "$d/scratch"
plain_only "seqlock-lint" && suite "drum grid seqlock ordering (seqlock-lint)" bash $T/seqlock-lint.sh "$d/real.c"

# ---------------------------------------------------------------- ogg decode
d="$OUT/ogg"; mkdir -p "$d"
extract guest/azstem/stem_playback.c '^static float \*decode_ogg_stereo_44100\(' > "$d/real.inc"
grep -q 'stb_vorbis_open_filename' "$d/real.inc" || { echo "extraction of decode_ogg_stereo_44100 failed"; exit 2; }
# control: the old fallback (a failed resample allocation hands back the native-rate buffer)
mutate "$d/real.inc" "$d/ctl-fallback.inc" 's/if \(!rs\) \{ \/\* never hand back the native-rate buffer as 44100 \*\/\n\s*free\(st\);\n\s*return NULL;/if (!rs) { *out_frames = nfr; return st;/'
# control: max_frames ignored
mutate "$d/real.inc" "$d/ctl-nocap.inc" 's/if \(max_frames > 0\) \{/if (0) {/'
for v in real ctl-fallback ctl-nocap; do
    $CC "${CFLAGS[@]}" -Wno-unused-parameter -I$T -DOGG_INC="\"$d/$v.inc\"" -o "$d/$v" $T/ogg_test.c
done
suite "ogg decode" "$d/real"

# ---------------------------------------------------------------- vinyl save handoff
d="$OUT/vsave"; mkdir -p "$d"
awk '/^static int g_vs_save_req/{p=1} p{print} p&&/^static void vs_save_flush/{q=1} q&&/^}$/{exit}' \
    guest/azmod/vinyl_slider.c > "$d/real.inc"
grep -q '__atomic_exchange_n(&g_vs_save_req' "$d/real.inc" || { echo "extraction of vs_save_post/vs_save_flush failed"; exit 2; }
# control: the flush reads the slot without consuming it (every tick rewrites p7)
mutate "$d/real.inc" "$d/ctl-noclear.inc" 's/__atomic_exchange_n\(&g_vs_save_req, -1, __ATOMIC_ACQ_REL\)/__atomic_load_n(&g_vs_save_req, __ATOMIC_ACQUIRE)/'
# control: the first value posted wins over a newer one
mutate "$d/real.inc" "$d/ctl-oldest.inc" 's/(\s+)__atomic_store_n\(&g_vs_save_req, m, __ATOMIC_RELEASE\);/$1if (__atomic_load_n(&g_vs_save_req, __ATOMIC_ACQUIRE) < 0) __atomic_store_n(&g_vs_save_req, m, __ATOMIC_RELEASE);/'
for v in real ctl-noclear ctl-oldest; do
    $CC "${CFLAGS[@]}" -I$T -DVS_INC="\"$d/$v.inc\"" -o "$d/$v" $T/vsave_test.c -lpthread
done
suite "vinyl save handoff" "$d/real"

# ---------------------------------------------------------------- settings (azmod.conf + azmod.cfg)
# azmod.c's settings block (load_settings / save_config, extracted) over the real menu.c and
# azconf.c: migration from 1.2.2's files, the byte-exact dual write, the conf's authority,
# newer / garbage confs, registered rows' keys (cfg_test.c).
d="$OUT/cfg"; mkdir -p "$d"
{ extract guest/azmod/azmod.c '^static int settings_write\('
  awk '/^\/\* ==== settings: azmod.conf/ {p=1} p {print} p && /^static void save_config\(const azmod_state \*s\) \{/ {q=1} q && /^}$/ {exit}' guest/azmod/azmod.c
} > "$d/real.inc"
grep -q 'static void load_settings(azmod_state \*s)' "$d/real.inc" && grep -q 'az_cfg_char(i)' "$d/real.inc" &&
    grep -q 'rename(tmp, path)' "$d/real.inc" && grep -q 'az_fire_changes' "$d/real.inc" ||
    { echo "extraction of the settings block failed"; exit 2; }
# control 1: azmod.cfg line 1 written by item id, not by legacy slot (the 1.2.2 file scrambled)
mutate "$d/real.inc" "$d/ctl-byid.inc" 's/buf\[n\+\+\] = \(char\) az_cfg_char\(i\);/buf[n++] = az_on(i < AZF_COUNT ? i : 0) ? 0x31 : 0x30;/'
# control 2: the old in-place save of azmod.cfg (truncate, then write)
mutate "$d/real.inc" "$d/ctl-inplace.inc" 's/settings_write\(CONFIG_PATH, buf, n, 1\) == 0;/({ FILE *f = fopen(CONFIG_PATH, "w"); if (f) { fwrite(buf, 1, n, f); fclose(f); } 1; });/'
# control 3: a newer build's azmod.conf overwritten
mutate "$d/real.inc" "$d/ctl-newer.inc" 's/g_conf_readonly = r == -3;/g_conf_readonly = 0;/'
# control 4: azmod.conf not the authority (every boot migrates azmod.cfg again)
mutate "$d/real.inc" "$d/ctl-noauth.inc" 's/    if \(r != 0\) \{\n        \/\* -1 absent/    if (1) {\n        \/* -1 absent/'
$CC -w -Iguest/azmod -c -o "$d/menu.o" guest/azmod/menu.c
$CC -w -Iguest/azmod -c -o "$d/azconf.o" guest/azmod/azconf.c
$CC -w -Iguest/azmod -Dmain=lanetest_main -c -o "$d/stubs.o" guest/azmod/lanetest.c
# control 5: no rollback check (azmod.cfg rewritten by 1.2.2 is ignored: its change is lost)
mutate "$d/real.inc" "$d/ctl-norollback.inc" 's/if \(was && strcmp\(was, cur\)\)/if (0)/'
# control 6: the small files' copies not refreshed at load (stale)
mutate "$d/real.inc" "$d/ctl-stale.inc" 's/        conf_refresh_files\(g_conf\); \/\* the small files/        (void) 0; \/* the small files/'
# control 7: the migration writes a 2-way CHOICE as on/off (a save then rewrites it as its index)
mutate "$d/real.inc" "$d/ctl-choiceonoff.inc" 's/\.int_slots = g_int_slots,\s*\.n_int_slots = ni\}/.int_slots = g_int_slots, .n_int_slots = 0}/'
# control 8: the stick's azmods.cfg ignored (the deck copy wins)
mutate "$d/real.inc" "$d/ctl-noimport.inc" 's/if \(settings_import_pending\(\)\) \{ \/\* the stick/if (0) { \/* the stick/'
# control 9: the theme and the vinyl value not saved into azmod.conf (the old per-deck files)
mutate "$d/real.inc" "$d/ctl-nosmallsave.inc" 's/    conf_capture_small\(c\);/    (void) 0;/'
# control 10: no one-time import of azmod-theme / azmod-vinyl.v into an older conf
mutate "$d/real.inc" "$d/ctl-nosmallimport.inc" 's/        conf_import_small\(c\); \/\* once/        (void) 0; \/* once/'
# control 11: the waveform zoom's level not saved into azmod.conf
mutate "$d/real.inc" "$d/ctl-nowzsave.inc" 's/    wz_conf_capture\(c\);/    (void) 0;/'
# control 12: the waveform zoom's level not loaded from azmod.conf
mutate "$d/real.inc" "$d/ctl-nowzload.inc" 's/    wz_conf_apply\(c\);/    (void) 0;/'
for v in real ctl-byid ctl-inplace ctl-newer ctl-noauth ctl-norollback ctl-stale ctl-choiceonoff ctl-noimport ctl-nosmallsave ctl-nosmallimport ctl-nowzsave ctl-nowzload; do
    $CC "${CFLAGS[@]}" -Wno-unused-parameter -Iguest/azmod -I$T -DCFG_INC="\"$d/$v.inc\"" \
        -o "$d/$v" $T/cfg_test.c "$d/menu.o" "$d/azconf.o" "$d/stubs.o"
done
suite "settings" "$d/real" "$d/scratch-real"

# ---------------------------------------------------------------- Mod Menu dump (golden)
# Every Mod Menu screen a knob walk reaches, the settings each azmod.cfg loads to, and what a
# press / a tap does, for a set of real and edge-case files (menudump_test.c), byte-identical
# to the golden that 1.2.2's code (before the feature-table refactor) produced with the
# refactor's intended changes applied to it (menudump-golden.sh, menudump-intended.patch).
d="$OUT/menudump"; mkdir -p "$d"
cp "$OUT/cfg/real.inc" "$d/cfg.inc"
mkdir -p "$d/guest"; cp -R guest/azmod guest/common "$d/guest/"
# control 1: one feature's default flipped in features.def (Remote Stems default ON -> OFF)
mutate guest/azmod/features.def "$d/features-ctl.def" 's/("remote-stems",\s+"Remote Stems",\s+STEMS,\s+AZ_ROW_TOGGLE,\s+)AZ_ROW_DEFAULT_ON/${1}0/'
mkdir -p "$d/ctl/guest"; cp -R guest/azmod guest/common "$d/ctl/guest/"; cp "$d/features-ctl.def" "$d/ctl/guest/azmod/features.def"
$CC -w -Iguest/azmod -Dmain=lanetest_main -Dazmod_ttf_menu=lanetest_ttf_menu -Dazmod_ttf_menu2=lanetest_ttf_menu2 \
    -c -o "$d/stubs.o" guest/azmod/lanetest.c
for v in real ctl; do
    G="$d/guest/azmod"; [ $v = ctl ] && G="$d/ctl/guest/azmod"
    BUILDS="rel dev"; [ "$PUBLIC" = 1 ] && BUILDS=rel   # the published tree has no development code
    for b in $BUILDS; do
        F=(); [ $b = rel ] && F=(-DAZMOD_RELEASE)
        $CC -w -I"$G" -c -o "$d/azconf-$v-$b.o" "$G/azconf.c"
        $CC -std=gnu11 -O1 -w "${F[@]}" -I"$G" -I$T -DMENU_C="\"$G/menu.c\"" -DCM_CATALOG="\"$d/nocat\"" \
            -DCFG_INC="\"$d/cfg.inc\"" -DMENUDUMP_NEW='"menudump_new.h"' -o "$d/$v-$b" $T/menudump_test.c \
            "$d/stubs.o" "$d/azconf-$v-$b.o"
    done
done
mkdir -p "$d/s1" "$d/s2" "$d/s3" "$d/s4"
suite "menu dump (release)" $T/menudump-check.sh "$d/real-rel" $T/menudump.golden-rel.gz "$d/s1/azmod.cfg"
if [ "$PUBLIC" = 1 ]; then
    skip "menu dump (development): the published tree has no development-build code"
else
    suite "menu dump (development)" $T/menudump-check.sh "$d/real-dev" $T/menudump.golden-dev.gz "$d/s2/azmod.cfg"
fi

# ---------------------------------------------------------------- build warnings are errors
# guest/Makefile (release and development) and the public source's Makefile.public: the compile
# lines of azmod.so, azstem.so and az-handoff-guard reject a new -Wall or -Wextra warning
# (werror-check.sh). Runs in the release build container; the control Makefile is written under
# guest/out, which the container shares.
WERRCTL="$ROOT/guest/out/unit-werrorctl"; mkdir -p "$WERRCTL"
mutate guest/Makefile "$WERRCTL/Makefile" 's/^AZ_WARN := -Wall -Wextra -Werror$/AZ_WARN := -Wall -Wextra/m'
if [ "$PUBLIC" = 1 ]; then
    werrortest() { docker run --rm --platform=linux/arm64 -v "$ROOT/guest":/g:ro -v "$ROOT/$T":/t:ro \
        azmod-build:18.04 bash /t/werror-check.sh /g "$@"; }
    suite "build warnings are errors (release)" werrortest
    skip "build warnings are errors (development): the published tree has no development build"
    skip "build warnings are errors (Makefile.public): the published tree's Makefile is the one above"
elif plain_only "werror-check.sh (release compile lines)"; then
    werrortest() { docker run --rm --platform=linux/arm64 -v "$ROOT/guest":/g:ro -v "$ROOT/$T":/t:ro \
        -v "$ROOT/tools/public-source":/p:ro azmod-build:18.04 bash /t/werror-check.sh /g "$@"; }
    suite "build warnings are errors (release)" werrortest
    suite "build warnings are errors (development)" werrortest AZMOD_RELEASE=0
    suite "build warnings are errors (Makefile.public)" werrortest -f /p/Makefile.public
fi

# ---------------------------------------------------------------- azmod.conf
# guest/azmod/azconf.c. The migration's equivalence oracle is 1.2.2's own code, frozen in
# legacy-1.2.2/ (load_config, azmod_state_init and the drum-bank radio
# block of mod_menu_overlay.c), since the feature table replaced it in the tree.
d="$OUT/azconf"; mkdir -p "$d"
L=$ROOT/$T/legacy-1.2.2
grep -q 'g_state.mod_enabled\[AZMOD_ACTION_DRUMB1 + b\]' $L/bank.inc && grep -q 'bank = 1;' $L/bank.inc ||
    { echo "the frozen drum-bank radio block is not 1.2.2's"; exit 2; }
cp guest/azmod/azconf.c "$d/azconf-real.c"
# control: duplicate keys, the first one wins
mutate guest/azmod/azconf.c "$d/azconf-ctl-firstwins.c" 's/if \(dup >= 0\) \/\* the last one wins, in its own place \*\//if (0)/'
# control: the version check dropped (version = 99 and no version line load)
mutate guest/azmod/azconf.c "$d/azconf-ctl-noversion.c" 's/if \(v != 1\) \{/if (0) {/'
# control: migration's line-1 minimum width dropped (a lone "1" migrates as data)
mutate guest/azmod/azconf.c "$d/azconf-ctl-minwidth.c" 's/#define AZCONF_CFG_MIN_WIDTH 13/#define AZCONF_CFG_MIN_WIDTH 1/'
# control: the drum-bank radio resolves to the last bank on, not the first
mutate guest/azmod/azconf.c "$d/azconf-ctl-lastbank.c" 's/pos = k;\n\s*break;/pos = k;/'
# control: FPS -> OVERLAYS for an old file dropped
mutate guest/azmod/azconf.c "$d/azconf-ctl-nofps.c" 's/L->on\[d->slot_overlays\] = 1;/;/'
# control: saved in place (no .new, no rename)
mutate guest/azmod/azconf.c "$d/azconf-ctl-inplace.c" 's/int fd = open\(tmp,/int fd = open(path,/; s/if \(!ok \|\| rename\(tmp, path\) != 0\)/if (!ok)/'
# control: save's cap without the final newline's byte (a file load took cannot be saved back)
mutate guest/azmod/azconf.c "$d/azconf-ctl-savecap.c" 's/if \(over_cap\(buf, len\)\) \{ \/\* load would refuse it \*\//if (len > AZCONF_FILE_MAX) {/'
$CC -w -DAZMOD_RELEASE -I$L -Iguest/azmod -c -o "$d/menu-rel.o" $L/state_init.c
AZC_ARGS=(-Wno-unused-parameter -DAZMOD_RELEASE -I$L -Iguest/azmod -I$T -DCFG_INC="\"$L/cfg.inc\""
          -DBANK_INC="\"$L/bank.inc\"")
# control: a write that stopped short still renamed into place (a full p7)
mutate guest/azmod/azconf.c "$d/azconf-ctl-short.c" 's/int ok = off == len && fsync\(fd\) == 0;/int ok = fsync(fd) == 0;/'
for v in real ctl-firstwins ctl-noversion ctl-minwidth ctl-lastbank ctl-nofps ctl-inplace ctl-savecap ctl-short; do
    $CC "${CFLAGS[@]}" "${AZC_ARGS[@]}" -DAZCONF_C="\"$d/azconf-$v.c\"" \
        -o "$d/$v" $T/azconf_test.c "$d/menu-rel.o"
done
# the real suite under ASan + UBSan when this compiler has them (menu.o stays uninstrumented)
AZC_BIN="$d/real"
if [ -z "$SAN" ] && AZ_CC_PROBE=1 $CC "${CFLAGS[@]}" "${AZC_ARGS[@]}" -fsanitize=address,undefined -fno-sanitize-recover=undefined -g \
        -DAZCONF_C="\"$d/azconf-real.c\"" -o "$d/real-san" $T/azconf_test.c "$d/menu-rel.o" \
        2>"$d/san.log"; then
    AZC_BIN="$d/real-san"; echo "(azmod.conf: running under -fsanitize=address,undefined)"
else
    echo "(azmod.conf: sanitizers unavailable with $CC, plain build; $(head -1 "$d/san.log"))"
fi
suite "azmod.conf" env ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0" "$AZC_BIN" "$d/scratch"

# ---------------------------------------------------------------- the 60 FPS group
# features.def FPS60 (a GROUP whose left checkbox is a bypass with memory): byte-exact round
# trips from every reachable state, the members after every tap equal to 1.2.2's logic (the
# oracle: legacy-1.2.2/fps60.inc), a hand toggle clears the memory (fps60_test.c).
d="$OUT/fps60"; mkdir -p "$d"
$CC -w -I$T/legacy-1.2.2 -Iguest/azmod -c -o "$d/oracle.o" $T/fps60_oracle.c
for v in real old; do
    F=(); [ $v = old ] && F=(-DOLD_AS_IMPL)   # control: 1.2.2's logic as the implementation
    $CC "${CFLAGS[@]}" "${F[@]}" -Iguest/azmod -I$T -o "$d/$v" $T/fps60_test.c "$d/oracle.o" "$OUT/cfg/menu.o" "$OUT/cfg/stubs.o"
done
suite "60 FPS group" "$d/real"

# ---------------------------------------------------------------- registered rows
# azfeatures.c azf_register / row_value / row_set / row_lookup and the menu drawing and
# handling registered rows through the official rows' code (rows_test.c).
d="$OUT/rows"; mkdir -p "$d"
# the variants put a mutated azfeatures.c beside a copy of menu.c: it must be menu.c that includes it
menu_variant "$d/probe" azfeatures.c
cp guest/azmod/menu.c "$d/menu-real.c"; cp guest/azmod/azfeatures.c "$d/azfeatures-real.c"
# control 1: the registered rows listed after the utilities (not right under Community Mods)
mutate guest/azmod/azfeatures.c "$d/azfeatures-ctl-order.c" 's/(#define AZ_REGISTERED_ROWS\(\) \{AZ_ORD_R, 0\},)/#define AZ_REGISTERED_ROWS()/; s/(    AZ_ORDER\[\] = \{\n)/$1/; s/(#undef AZ_RETIRED\n\};\nstatic const signed char AZ_RETIRED_SLOTS)/#undef AZ_RETIRED\n    {AZ_ORD_R, 0},\n};\nstatic const signed char AZ_RETIRED_SLOTS/'
# control 2: row_set accepts an official row
mutate guest/azmod/azfeatures.c "$d/azfeatures-ctl-set.c" 's/if \(handle < AZI_REG0 \|\| handle >= AZI_REG0 \+ g_nreg \|\| !az_has_value\(handle\)\)/if (handle < 0 || handle >= AZI_REG0 + g_nreg || !az_has_value(handle))/'
# control 3: a child row may hang under an official row
mutate guest/azmod/azfeatures.c "$d/azfeatures-ctl-parent.c" 's/    if \(f.parent && parent == AZF_NONE\) \{/    if (f.parent \&\& parent == AZF_NONE \&\& 0) {/'
# control 4: the row keeps the mod's own icon buffer instead of a copy
mutate guest/azmod/azfeatures.c "$d/azfeatures-ctl-iconref.c" 's/g->r\.d\.icon = g->icon;/g->r.d.icon = f.icon;/'
# control 5: the menu never draws a registered row's icon
cp guest/azmod/azfeatures.c "$d/azfeatures-ctl-icondraw.c"
mutate guest/azmod/ui_draw.c "$d/ui_draw-ctl-icondraw.c" 's/            blit_icon\(fb, x, y, sz, c, r->d\.icon\);\n            break;\n/            break;\n/'
$CC -w -Iguest/azmod -Dmain=lanetest_main -Dazmod_ttf_menu=lanetest_ttf_menu -Dazmod_ttf_menu2=lanetest_ttf_menu2 \
    -c -o "$d/stubs.o" guest/azmod/lanetest.c
# control 6: retiring a stopped mod's rows does nothing
mutate guest/azmod/azfeatures.c "$d/azfeatures-ctl-retire.c" 's/    for \(int k = 0; mod_id && k < g_nreg; k\+\+\) \{\n        struct az_reg \*g = &g_reg\[k\];/    for (int k = 0; mod_id \&\& k < 0; k++) {\n        struct az_reg *g = \&g_reg[k];/'
for v in real ctl-order ctl-set ctl-parent ctl-iconref ctl-icondraw ctl-retire; do
    mkdir -p "$d/$v"; cp guest/azmod/menu.c "$d/$v/menu.c"; cp "$d/azfeatures-$v.c" "$d/$v/azfeatures.c"
    [ -f "$d/menu-$v.c" ] && cp "$d/menu-$v.c" "$d/$v/menu.c"
    [ -f "$d/ui_draw-$v.c" ] && cp "$d/ui_draw-$v.c" "$d/$v/ui_draw.c" # menu.c includes it from its own folder first
    $CC -std=gnu11 -O1 -w -I"$d/$v" -Iguest/azmod -I$T -DMENU_C="\"$d/$v/menu.c\"" -DCM_CATALOG="\"$d/nocat\"" \
        -o "$d/$v/t" $T/rows_test.c "$d/stubs.o"
done
suite "registered rows" "$d/real/t"

# ---------------------------------------------------------------- Mod Menu keys (a public interface)
# The official keys in features.def == the published table (docs/MOD-MENU-KEYS.md, generated by
# keys_test.c --print), plus the table's integrity: unique keys, parents first, every legacy
# slot one row's or retired (keys_test.c).
d="$OUT/keys"; mkdir -p "$d/real" "$d/ctl"
cp guest/azmod/menu.c guest/azmod/azfeatures.c guest/azmod/features.def "$d/real/"
cp guest/azmod/menu.c guest/azmod/azfeatures.c "$d/ctl/"
# control: a key renamed in features.def without the table
mutate guest/azmod/features.def "$d/ctl/features.def" 's/"stem-mark-style"/"stem-mark-look"/'
for v in real ctl; do
    $CC -std=gnu11 -O1 -w -DAZMOD_RELEASE -I"$d/$v" -Iguest/azmod -I$T -DMENU_C="\"$d/$v/menu.c\"" \
        -DCM_CATALOG="\"$d/nocat\"" -o "$d/$v/t" $T/keys_test.c "$OUT/rows/stubs.o"
done
KEYS_MD=tools/public-source/skel/docs/MOD-MENU-KEYS.md; [ "$PUBLIC" = 1 ] && KEYS_MD=docs/MOD-MENU-KEYS.md
suite "Mod Menu keys" "$d/real/t" "$KEYS_MD"

# ---------------------------------------------------------------- community draw services
d="$OUT/draw"; mkdir -p "$d"
menu_variant "$d/src-real"; menu_variant "$d/src-ctl" ui_draw.c screen_community.c
# control: the old 32-bit clip and no clamping of a mod's values (INT_MAX wraps, writes past the buffer)
mutate guest/azmod/ui_draw.c "$d/src-ctl/ui_draw.c" 's{    /\* clip in 64-bit.*?x = \(int\) x0;\s*y = \(int\) y0;\s*w = \(int\) \(x1 - x0\);\s*h = \(int\) \(y1 - y0\);\n}{    if (x < 0) { w += x; x = 0; }\n    if (y < 0) { h += y; y = 0; }\n    if (x + w > fb->w) w = fb->w - x;\n    if (y + h > fb->h) h = fb->h - y;\n    if (w <= 0 || h <= 0) return;\n}s'
mutate guest/azmod/screen_community.c "$d/src-ctl/screen_community.c" 's/#define CM_WH\(v\) cm_clampi\(\(v\), 0, 32767\)/#define CM_WH(v) (v)/; s/#define CM_XY\(v\) cm_clampi\(\(v\), -32768, 32767\)/#define CM_XY(v) (v)/'
# control: text_width passes the mod's px to the face unclamped
menu_variant "$d/src-ctl-tw" screen_community.c
mutate guest/azmod/screen_community.c "$d/src-ctl-tw/screen_community.c" 's/    if \(px < 8\) px = 8; \/\* as draw_text draws it[^\n]*\n    else if \(px > 96\) px = 96;\n//'
for v in real ctl ctl-tw; do
    $CC -w -fwrapv -Iguest/azmod -Dazmod_ttf_menu_width=draw_ttf_width -c -o "$d/menu-$v.o" "$d/src-$v/menu.c"
    $CC "${CFLAGS[@]}" -Wno-unused-parameter -Iguest/azmod -I$T -o "$d/$v" $T/draw_test.c "$d/menu-$v.o" "$OUT/cfg/stubs.o"
done
suite "draw" "$d/real"

# ---------------------------------------------------------------- community catalog (Mod Menu)
d="$OUT/cmcat"; mkdir -p "$d"
for v in real ctl-oldcaps ctl-nocount ctl-silent ctl-listed ctl-noheld ctl-anyname ctl-noupd ctl-noteall ctl-notab ctl-nodl ctl-dlzero ctl-nohome ctl-seeall ctl-rowall ctl-homeknob ctl-dotutf8; do menu_variant "$d/src-$v" screen_community.c; done
# control 1: the old caps (16 repositories, 64 mods, 96 controls)
mutate guest/azmod/screen_community.c "$d/src-ctl-oldcaps/screen_community.c" 's/#define CM_NREPO 96/#define CM_NREPO 16/; s/#define CM_NCAT  128/#define CM_NCAT  64/; s/#define CM_MAXCTL 256/#define CM_MAXCTL 96/'
# control 2: mod lines past the array dropped without counting them (the old behavior)
mutate guest/azmod/screen_community.c "$d/src-ctl-nocount/screen_community.c" 's/(\(n >= 13 && !strcmp\(fl\[0\], "mod"\) && g_ncc >= CM_NCAT\)) g_cc_more\+\+;/$1 {}/'
# control 3: the "Showing the first" note never drawn
mutate guest/azmod/screen_community.c "$d/src-ctl-silent/screen_community.c" 's/if \(g_cc_more > 0\) \{   \/\* the catalog holds/if (0) {   \/* the catalog holds/'
# control 4: every repository taken as listed (no mark, no note)
mutate guest/azmod/screen_community.c "$d/src-ctl-listed/screen_community.c" 's/return g_cr\[i\]\.listed;\n    return 0;/return 1;\n    return 1;/'
# control 5: the same-id fields (17-19) never read: no held note, no also note, Install offered
mutate guest/azmod/screen_community.c "$d/src-ctl-noheld/screen_community.c" 's/cm_cpy\(m->held, sizeof m->held, n >= 17 \? fl\[16\] : ""\);/cm_cpy(m->held, sizeof m->held, "");/; s/m->nalso = n >= 19 \? atoi\(fl\[18\]\) : 0;/m->nalso = 0;/'
# control 6: an installed id named from any line with that id (the other author's too)
mutate guest/azmod/screen_community.c "$d/src-ctl-anyname/screen_community.c" 's/ && g_cc\[i\]\.name\[0\] && !g_cc\[i\]\.held\[0\]\) return g_cc\[i\]\.name;/ \&\& g_cc[i].name[0]) return g_cc[i].name;/'
# control 8: the trust note drawn on every tab, not the Installed tab alone
mutate guest/azmod/screen_community.c "$d/src-ctl-noteall/screen_community.c" 's/if \(g_cm_view == CMV_HOME && g_cm_tab == 0 && draw\)/if (g_cm_view == CMV_HOME \&\& draw)/'
# control 9: the breadcrumb never names the tab a page was opened from
mutate guest/azmod/screen_community.c "$d/src-ctl-notab/screen_community.c" 's/for \(int with_tab = 1; with_tab >= 0; with_tab--\)/for (int with_tab = 0; with_tab >= 0; with_tab--)/'
# control 7: field 20 (updated) never read: the Updated order has no dates
mutate guest/azmod/screen_community.c "$d/src-ctl-noupd/screen_community.c" 's/cm_cpy\(m->upd, sizeof m->upd, n >= 20 && cmb_date_ok\(fl\[19\]\) \? fl\[19\] : ""\);/cm_cpy(m->upd, sizeof m->upd, "");/'
# control 10: field 21 (downloads) never read: no counts, no Most downloaded
mutate guest/azmod/screen_community.c "$d/src-ctl-nodl/screen_community.c" 's/m->counted = n >= 21 && cmb_dl_parse\(fl\[20\], &m->dl\);/m->counted = 0;/'
# control 11: a tile with no count shows 0
mutate guest/azmod/screen_community.c "$d/src-ctl-dlzero/screen_community.c" 's/if \(m->counted\) \{   \/\* no count: nothing, never 0 \*\//if (1) {/'
# control 12: Browse opens on All mods, not Home
mutate guest/azmod/screen_community.c "$d/src-ctl-nohome/screen_community.c" 's/static int g_cm_fcat = CMB_HOME,/static int g_cm_fcat = CMB_ALL,/'
# control 13: See all keeps the order it had (Most downloaded opens All mods A-Z)
mutate guest/azmod/screen_community.c "$d/src-ctl-seeall/screen_community.c" 's/g_cm_sort = cmb_shelf_sort\(c->idx\); //'
# control 14: a shelf draws every mod it holds, past the grid's right edge
mutate guest/azmod/screen_community.c "$d/src-ctl-rowall/screen_community.c" 's/nf = h->n < cols \? h->n : cols;/nf = h->n;/'
# control 15: the knob on the home page does not scroll it
mutate guest/azmod/screen_community.c "$d/src-ctl-homeknob/screen_community.c" 's/        \} else if \(d\) \{   \/\* a row per detent, gliding/        } else if (0) {   \/* a row per detent, gliding/'
# control 17: the mod page's separator as a UTF-8 middle dot (two glyphs in the menu font)
mutate guest/azmod/screen_community.c "$d/src-ctl-dotutf8/screen_community.c" 's/#define CM_DOT "  \\xb7  "/#define CM_DOT "  \\xc2\\xb7  "/'
$CC -w -Iguest/azmod -Dmain=lanetest_main -Dazmod_ttf_menu=lanetest_ttf_menu -Dazmod_ttf_menu2=lanetest_ttf_menu2 \
    -c -o "$d/stubs.o" guest/azmod/lanetest.c
for v in real ctl-oldcaps ctl-nocount ctl-silent ctl-listed ctl-noheld ctl-anyname ctl-noupd ctl-noteall ctl-notab ctl-nodl ctl-dlzero ctl-nohome ctl-seeall ctl-rowall ctl-homeknob ctl-dotutf8; do
    $CC -std=gnu11 -O1 -w -Iguest/azmod -I$T -DMENU_C="\"$d/src-$v/menu.c\"" -DCM_CATALOG="\"$d/catalog-$v\"" \
        -o "$d/$v" $T/cmcat_test.c "$d/stubs.o"
done
suite "catalog" "$d/real"

# ---------------------------------------------------------------- an installed mod's page (Mod Menu)
d="$OUT/cmmod"; mkdir -p "$d"
for v in real ctl-loaded; do menu_variant "$d/src-$v" screen_community.c; done
# control: the toggle and the reload check read "on" as "loaded" (a refused mod offered Enable)
mutate guest/azmod/screen_community.c "$d/src-ctl-loaded/screen_community.c" 's/return m->state == AZCM_LOADED \|\| m->state == AZCM_REFUSED \|\| m->state == AZCM_FAILED;/return m->state == AZCM_LOADED;/'
$CC -w -Iguest/azmod -Dmain=lanetest_main -Dazmod_ttf_menu=lanetest_ttf_menu -Dazmod_ttf_menu2=lanetest_ttf_menu2 \
    -Dazcm_count=lanetest_azcm_count -Dazcm_info=lanetest_azcm_info -c -o "$d/stubs.o" guest/azmod/lanetest.c
for v in real ctl-loaded; do
    $CC -std=gnu11 -O1 -w -Iguest/azmod -I$T -DMENU_C="\"$d/src-$v/menu.c\"" -DCM_CATALOG="\"$d/catalog-$v\"" \
        -o "$d/$v" $T/cmmod_test.c "$d/stubs.o"
done
suite "installed mod page" "$d/real"

# ---------------------------------------------------------------- Community Mods > Updates (Mod Menu)
d="$OUT/cmupdtab"; mkdir -p "$d"
for v in real ctl-removed ctl-railcount ctl-noblock ctl-noreload ctl-nobg ctl-reloadmid ctl-chkopens ctl-knobchk ctl-countunder ctl-splitopens ctl-noflight ctl-autoeach ctl-autonoretry; do menu_variant "$d/src-$v" screen_community.c; done
mutate guest/azmod/screen_community.c "$d/src-ctl-removed/screen_community.c"   's/return k >= 0 && !cm_removed\(id\) \? k : -1;/return k;/'
mutate guest/azmod/screen_community.c "$d/src-ctl-railcount/screen_community.c" 's/snprintf\(ul, sizeof ul, nu \? "Updates %d" : "Updates", nu\);/snprintf(ul, sizeof ul, "Updates");/'
mutate guest/azmod/screen_community.c "$d/src-ctl-noblock/screen_community.c"   's/    return g_cm_ua\.on \|\| \(g_cs/    return 0 || (g_cs/'
mutate guest/azmod/screen_community.c "$d/src-ctl-noreload/screen_community.c"  's/    cmu_cancel\(&g_cm_ua\);   \/\* a reload ends Update all \*\/\n//'
mutate guest/azmod/screen_community.c "$d/src-ctl-nobg/screen_community.c"      's/void azmod_cm_bg_tick\(void\) \{ cm_updall_tick\(\); \}/void azmod_cm_bg_tick(void) {}/'
mutate guest/azmod/screen_community.c "$d/src-ctl-reloadmid/screen_community.c" 's/\(g_cm_reloading \|\| \(cm_need_reload\(\) && !cm_busy\(\)\)\)/(g_cm_reloading || cm_need_reload())/'
# the checkbox opens the page: no control of its own, so a touch on it is the row's
mutate guest/azmod/screen_community.c "$d/src-ctl-chkopens/screen_community.c"  's/        if \(box\) cm_chk_ctl\(lay, &n, CMC_CHK, i, y, h\);\n//'
# the knob's press toggles: the knob's focus lands on a checkbox (the split's list, cm_split_knob)
mutate guest/azmod/screen_community.c "$d/src-ctl-knobchk/screen_community.c"   's/        if \(ctl\[k\]\.x >= rx \|\| ctl\[k\]\.kind == CMC_CHK\) continue;/        if (ctl[k].x >= rx) continue;/'
# a checkbox toggles while the last toggle is unanswered (the fetcher would drop it)
mutate guest/azmod/screen_community.c "$d/src-ctl-noflight/screen_community.c" 's/if \(!cm_busy\(\) && !g_cm_flip\.at && cm_chk/if (!cm_busy() \&\& cm_chk/'
# every opening refreshes (no 10-minute limit)
mutate guest/azmod/screen_community.c "$d/src-ctl-autoeach/screen_community.c" 's/    if \(cm_busy\(\) \|\| \(!retry && g_cm_auto_at && cm_ms\(\) - g_cm_auto_at < CM_AUTO_MS\)\) return;/    if (cm_busy()) return;/'
# a failed refresh waits the full 10 minutes before the next opening tries again
mutate guest/azmod/screen_community.c "$d/src-ctl-autonoretry/screen_community.c" 's/\(!retry && g_cm_auto_at/(g_cm_auto_at/'
# the split's row opens the full page instead of selecting it
mutate guest/azmod/screen_community.c "$d/src-ctl-splitopens/screen_community.c" 's/    case CMC_MOD:     if \(cm_isplit\(\) && !g_cm_row_opening\) \{ cm_split_select\(CMC_MOD << 16 \| c->idx\); break; \}[^\n]*\n/    case CMC_MOD:\n/'
# the header's catalog count drawn before the band that covers it
mutate guest/azmod/screen_community.c "$d/src-ctl-countunder/screen_community.c" 's/(        fill_rect\(fb, 0, top, fb->w, hh, C_HEADER_BG\);\n)(        if \(cnt\[0\]\) c_text\(fb, cntx, top, 0, hh, cnt, dpx, C_TEXT_DIM\);[^\n]*\n)/$2$1/'
$CC -w -Iguest/azmod -Dmain=lanetest_main -Dazmod_ttf_menu=lanetest_ttf_menu -Dazmod_ttf_menu2=lanetest_ttf_menu2 \
    -Dazcm_count=lanetest_azcm_count -Dazcm_info=lanetest_azcm_info -Dazcm_ver_newer=lanetest_azcm_ver_newer \
    -c -o "$d/stubs.o" guest/azmod/lanetest.c
for v in real ctl-removed ctl-railcount ctl-noblock ctl-noreload ctl-nobg ctl-reloadmid ctl-chkopens ctl-knobchk ctl-countunder ctl-splitopens ctl-noflight ctl-autoeach ctl-autonoretry; do
    $CC -std=gnu11 -O1 -w -Iguest/azmod -I$T -DMENU_C="\"$d/src-$v/menu.c\"" -DCM_CATALOG="\"$d/catalog-$v\"" \
        -DCM_STATUS="\"$d/status-$v\"" -DCM_REQ="\"$d/req-$v\"" -o "$d/$v" $T/cmupdtab_test.c "$d/stubs.o"
done
suite "community updates tab" "$d/real"

# ---------------------------------------------------------------- Community Mods not loaded this start
# cmnotloaded_test.c: the notice (Installed tab only), Load next time, and the Reload after it
d="$OUT/cmnotloaded"; mkdir -p "$d"
NLV="real ctl-reset ctl-alltabs ctl-nobutton ctl-resize ctl-oldrow ctl-leftline ctl-crash ctl-noreq ctl-keepoff ctl-reload400 ctl-togglereload ctl-nowait"
for v in $NLV; do menu_variant "$d/src-$v" screen_community.c; done
# control 1: Reload sets the page back to the question Load next time already answered
mutate guest/azmod/screen_community.c "$d/src-ctl-reset/screen_community.c" 's/(    cm_req\("apply", 0, 0\);\n    g_cm_reload = 0;)/$1 g_cm_resumed = 0;/'
# control 2: the notice is laid out on the Browse tab too
mutate guest/azmod/screen_community.c "$d/src-ctl-alltabs/screen_community.c" 's/(    if \(cm_status_shown\(\) && cm_progress_line\(\)\) \{   \/\* Refresh is on the rail)/    if (azcm_suspended() && !g_cm_keptoff) { lay->y = y; lay->n = n; cm_home_notloaded(lay); y = lay->y; n = lay->n; }\n$1/'
# control 3: no Reload in the header (the one Reload)
mutate guest/azmod/screen_community.c "$d/src-ctl-nobutton/screen_community.c" 's/        if \(!g_cm_reloading\) CM_CTL\(CMC_APPLY, 0, rx, ry, rsz, rsz\);\n//'
# control 4: the box shrinks after Load next time (the rows move up)
mutate guest/azmod/screen_community.c "$d/src-ctl-resize/screen_community.c" 's/int bh2 = 3 \* lh \+ bh3 \+ 2 \* pad;/int bh2 = (g_cm_resumed ? lh : 3 * lh + bh3) + 2 * pad;/'
# control 5: a row still says "Held back (see above)"
mutate guest/azmod/screen_community.c "$d/src-ctl-oldrow/screen_community.c" 's/snprintf\(buf, n, "%s", "Not loaded"\);/snprintf(buf, n, "%s", "Held back (see above)");/'
# control 6: the left-pane card carries a line about it
mutate guest/azmod/screen_community.c "$d/src-ctl-leftline/screen_community.c" 's/(    \/\* the master state: the row.s own checkbox is the control; this just names it \*\/)/    if (azcm_suspended() \&\& !g_cm_resumed) { c_text(fb, x0 + pad, y, 0, lh, "Not loaded this time: open Community Mods.", dpx, CM_BAD); y += lh; }\n$1/'
# control 7: the reason is always the restarts one
mutate guest/azmod/screen_community.c "$d/src-ctl-crash/screen_community.c" 's/m0\.reason, "Suspended: the deck crashed %d times in %d minutes", &cn, &cm\) == 2/m0.reason, "Suspended: the deck crashed %d times in %d minutes", \&cn, \&cm) == 99/'
# control 8: Reload sends nothing
mutate guest/azmod/screen_community.c "$d/src-ctl-noreq/screen_community.c" 's/    cm_req\("apply", 0, 0\);\n//'
# control 9: Keep off takes the notice away (the rows move up)
mutate guest/azmod/screen_community.c "$d/src-ctl-keepoff/screen_community.c" 's/    if \(azcm_suspended\(\)\) \{\n        lay->y = y;/    if (azcm_suspended() \&\& !g_cm_keptoff) {\n        lay->y = y;/'
# control 11: the master switch sends the reload itself
mutate guest/azmod/screen_community.c "$d/src-ctl-togglereload/screen_community.c" 's/(    azcm_set_all_off\(off\);\n    g_cm_pending_off = off;\n)/$1    cm_req("apply", 0, 0);\n/'
# control 12: a pressed Reload does not wait (it can be pressed again, nothing says it is coming)
mutate guest/azmod/screen_community.c "$d/src-ctl-nowait/screen_community.c" 's/    g_cm_reloading = 1;   \/\* g_cm_resumed stays/    g_cm_reloading = 0;   \/* g_cm_resumed stays/'
# control 10: Community's Reload held to the 400 ms of every other control
mutate guest/azmod/screen_community.c "$d/src-ctl-reload400/screen_community.c" 's/static bool cm_home_reload\(int ctl\) \{ return \(ctl >> 16\) == CMC_APPLY; \}/static bool cm_home_reload(int ctl) { (void) ctl; return 0; }/'
$CC -w -Iguest/azmod -Dmain=lanetest_main -Dazmod_ttf_menu=lanetest_ttf_menu -Dazmod_ttf_menu2=lanetest_ttf_menu2 \
    -Dazcm_count=lanetest_azcm_count -Dazcm_info=lanetest_azcm_info -Dazcm_suspended=lanetest_azcm_suspended \
    -Dazcm_resume=lanetest_azcm_resume -Dazcm_any_loaded=lanetest_azcm_any_loaded \
    -Dazcm_reload_requested=lanetest_azcm_reload_requested -c -o "$d/stubs.o" guest/azmod/lanetest.c
for v in $NLV; do
    $CC -std=gnu11 -O1 -w -Iguest/azmod -I$T -DMENU_C="\"$d/src-$v/menu.c\"" -DCM_CATALOG="\"$d/catalog-$v\"" -DCM_REQ="\"$d/req-$v\"" \
        -o "$d/$v" $T/cmnotloaded_test.c "$d/stubs.o"
done
suite "community not loaded" "$d/real"

# ---------------------------------------------------------------- a mod's SCREEN row (Mod Menu)
# screenrow_test.c: a registered SCREEN row opens its mod's page by Community's own path
# (screen_community.c cm_open_row -> cm_fire CMC_MOD) and BACK returns to the row.
d="$OUT/screenrow"; mkdir -p "$d"
menu_variant "$d/src-real"; menu_variant "$d/src-ctl-flash" screen_community.c
# control: the row keeps the old behavior (a press flashes or does nothing; no page)
mutate guest/azmod/screen_community.c "$d/src-ctl-flash/screen_community.c" 's/(static bool cm_open_row\(azmod_state \*s, int it\) \{\n)/$1    if (1) return false; \/\* control: a SCREEN row keeps its old behavior \*\/\n/'
$CC -w -Iguest/azmod -Dmain=lanetest_main -Dazmod_ttf_menu=lanetest_ttf_menu -Dazmod_ttf_menu2=lanetest_ttf_menu2 \
    -Dazcm_count=lanetest_azcm_count -Dazcm_info=lanetest_azcm_info -Dazcm_screen_draw=lanetest_azcm_screen_draw \
    -Dazcm_screen_input=lanetest_azcm_screen_input -Dazcm_screen_enter=lanetest_azcm_screen_enter \
    -Dazcm_page_screen=lanetest_azcm_page_screen -Dazcm_row_target=lanetest_azcm_row_target \
    -c -o "$d/stubs.o" guest/azmod/lanetest.c
# controls: the row's own screen is ignored (the mod's page always); the preview band stays empty
menu_variant "$d/src-ctl-ownscr" screen_community.c
mutate guest/azmod/screen_community.c "$d/src-ctl-ownscr/screen_community.c" 's/    azcm_page_screen\(idx, own\); \/\* the row.s own screen; NULL = the mod.s page \*\//    azcm_page_screen(idx, NULL);/'
menu_variant "$d/src-ctl-band" menu_full.c
mutate guest/azmod/menu_full.c "$d/src-ctl-band/menu_full.c" 's/        if \(it0 >= AZI_REG0 && it0 < AZI_ACT0 && az_kind\(it0\) == AZ_ROW_SCREEN\) \{/        if (0) {/'
for v in real ctl-flash ctl-ownscr ctl-band; do
    $CC -std=gnu11 -O1 -w -Iguest/azmod -I$T -DMENU_C="\"$d/src-$v/menu.c\"" -DCM_CATALOG="\"$d/catalog-$v\"" \
        -o "$d/$v" $T/screenrow_test.c "$d/stubs.o"
done
suite "screen row" "$d/real"

# ---------------------------------------------------------------- the Mod Menu's lists scroll
# menuscroll_test.c: 20 registered rows overflow the lists; the rows keep their height and the lists
# scroll (knob, drag, tap), with EP147's own scroll bar (ui_draw.c draw_vsb).
d="$OUT/menuscroll"; mkdir -p "$d"
menu_variant "$d/src-real"
menu_variant "$d/src-ctl-shrink" menu_full.c
menu_variant "$d/src-ctl-shrinkc" menu_list.c
menu_variant "$d/src-ctl-footer" menu_list.c
menu_variant "$d/src-ctl-offscreen" menu_full.c
menu_variant "$d/src-ctl-dragtoggle" menu_full.c
menu_variant "$d/src-ctl-reload400" menu_full.c
menu_variant "$d/src-ctl-reloadkeep" menu_full.c
menu_variant "$d/src-ctl-nofling" menu_full.c
menu_variant "$d/src-ctl-nocatch" menu_full.c
menu_variant "$d/src-ctl-noput" menu_full.c
# control 1: the old scale-down path (the left list divides its space by every entry, mods' included)
mutate guest/azmod/menu_full.c "$d/src-ctl-shrink/menu_full.c" 's/        n \+= !item_is_registered\(t\[k\]\);/        n += 1;/'
# control 1b: the same for the compact list's budget (it moved the Updates screen too)
# control 1c: the compact list's bottom margin is half a row again, so its last row ends under the footer
mutate guest/azmod/menu_list.c "$d/src-ctl-footer/menu_list.c" 's/return foot > half \? foot : half;/return half;/'
mutate guest/azmod/menu_list.c "$d/src-ctl-shrinkc/menu_list.c" 's/if \(!item_hidden\(i\) && !item_is_registered\(i\)\)\n            rows\+\+;/if (!item_hidden(i))\n            rows++;/'
# control 2: the knob scrolls nothing, so the selection walks off the screen
mutate guest/azmod/menu_full.c "$d/src-ctl-offscreen/menu_full.c" 's/            c_lglide_show\(fb, k\); \/\* the list scrolls to keep the selection in view \*\//            (void) k;/'
# control 3: a drag still fires the control under it (the release ignores the movement)
mutate guest/azmod/menu_full.c "$d/src-ctl-dragtoggle/menu_full.c" 's/    if \(g_rdrag\.moved\) \{\n        g_mhit = "release:drag";/    if (0) {\n        g_mhit = "release:drag";/'
# control 4: a lift never flings (the release is one more drag step)
mutate guest/azmod/menu_full.c "$d/src-ctl-nofling/menu_full.c" 's/        glide_release\(g, \(float\) to, ms, GLIDE_STOP_V\);/        glide_drag(g, (float) to, ms);/'
# control 5: a finger on a flinging list fires the control under it
mutate guest/azmod/menu_full.c "$d/src-ctl-nocatch/menu_full.c" 's/c_press\(tx, ty, caught \? -1 : ctl, false, g_rscroll\);/c_press(tx, ty, ctl, false, g_rscroll);/'
# control 6: choosing another pane keeps the right pane's offset and glide
mutate guest/azmod/menu_full.c "$d/src-ctl-noput/menu_full.c" 's/    if \(it != s->left_sel\)\n        c_glide_put\(&g_rgl, &g_rglpx, &g_rscroll, 0\);\n//'
# control 7: Reload held to the 400 ms of every other control
mutate guest/azmod/menu_full.c "$d/src-ctl-reload400/menu_full.c" 's/bool reload = g_rdrag\.ctl >= 0 && /bool reload = 0 \&\& g_rdrag.ctl >= 0 \&\& /'
# control 8: a Reload press held a full second still restarts
mutate guest/azmod/menu_full.c "$d/src-ctl-reloadkeep/menu_full.c" 's/held >= C_RELOAD_HOLD_MS/held >= 100000/'
$CC -w -Iguest/azmod -Dmain=lanetest_main -Dazmod_ttf_menu=lanetest_ttf_menu -Dazmod_ttf_menu2=lanetest_ttf_menu2 \
    -Dazcm_count=lanetest_azcm_count -Dazcm_info=lanetest_azcm_info -Dazcm_screen_draw=lanetest_azcm_screen_draw \
    -Dazcm_screen_input=lanetest_azcm_screen_input -Dazcm_screen_enter=lanetest_azcm_screen_enter \
    -Dazmod_dm_on=lanetest_dm_on -Dazmod_dm_map=lanetest_dm_map -Dazmod_dm_key=lanetest_dm_key \
    -c -o "$d/stubs.o" guest/azmod/lanetest.c
for v in real ctl-shrink ctl-shrinkc ctl-footer ctl-offscreen ctl-dragtoggle ctl-nofling ctl-nocatch ctl-noput ctl-reload400 ctl-reloadkeep; do
    $CC -std=gnu11 -O1 -w -Iguest/azmod -I$T -DMENU_C="\"$d/src-$v/menu.c\"" -DCM_CATALOG="\"$d/catalog-$v\"" \
        -o "$d/$v" $T/menuscroll_test.c "$d/stubs.o"
done
suite "menu scroll" "$d/real"

# iconcache_test.c: the Mod Menu's icon cache (ui_draw.c draw_item_icon)
d="$OUT/iconcache"; mkdir -p "$d"
for v in real ctl-noground ctl-notheme ctl-anyground ctl-nohit ctl-spill; do menu_variant "$d/src-$v" ui_draw.c; done
# control 1: the ground color left out of the key (an icon kept on one band shown on another)
mutate guest/azmod/ui_draw.c "$d/src-ctl-noground/ui_draw.c" 's/g_icache\[k\]\.ink == c &&\n            g_icache\[k\]\.ground == ground && /g_icache[k].ink == c \&\&\n            /'
# control 2: the theme left out of the key (a kept icon survives a palette change)
mutate guest/azmod/ui_draw.c "$d/src-ctl-notheme/ui_draw.c" 's/g_icache\[k\]\.ground == ground && g_icache\[k\]\.theme == theme\) \{/g_icache[k].ground == ground) {/'
# control 3: any ground counts as one color (an icon over a pattern is kept)
mutate guest/azmod/ui_draw.c "$d/src-ctl-anyground/ui_draw.c" 's/            if \(r\[i\] != g\) \{/            if (0) {/'
# control 4: the cache never answers (every draw is a fresh drawing)
mutate guest/azmod/ui_draw.c "$d/src-ctl-nohit/ui_draw.c" 's/        if \(g_icache\[k\]\.used && g_icache\[k\]\.item == item/        if (0 \&\& g_icache[k].used \&\& g_icache[k].item == item/'
# control 5: an icon that draws past its box (Pad Colors' last square, too wide)
mutate guest/azmod/ui_draw.c "$d/src-ctl-spill/ui_draw.c" 's/fill_rect\(fb, x \+ sz - q, y \+ sz - q, q, q, g\[3\]\);/fill_rect(fb, x + sz - q, y + sz - q, q + sz, q, g[3]);/'
for v in real ctl-noground ctl-notheme ctl-anyground ctl-nohit ctl-spill; do
    $CC -std=gnu11 -O1 -w -Iguest/azmod -I$T -DMENU_C="\"$d/src-$v/menu.c\"" -DCM_CATALOG="\"$d/catalog-$v\"" \
        -o "$d/$v" $T/iconcache_test.c "$OUT/menuscroll/stubs.o"
done
suite "icon cache" "$d/real"

# ovclip_test.c: guest/azmod/ov_clip.h (each control: a changed copy of the header, found first)
d="$OUT/ovclip"; mkdir -p "$d"
for v in real ctl-nodx ctl-noright; do mkdir -p "$d/inc-$v"; done
cp guest/azmod/ov_clip.h "$d/inc-real/ov_clip.h"
# control 1: a part cut on the left is drawn at the window's left edge, not where it belongs
mutate guest/azmod/ov_clip.h "$d/inc-ctl-nodx/ov_clip.h" 's/p->dx = \(int\) \(cx0 - x0\);/p->dx = 0;/'
# control 2: no cut at the buffer's right edge (the request the server refuses)
mutate guest/azmod/ov_clip.h "$d/inc-ctl-noright/ov_clip.h" 's/cx1 = x1 > W \? W : x1/cx1 = x1/'
for v in real ctl-nodx ctl-noright; do
    $CC -std=gnu11 -O1 -w -I"$d/inc-$v" -Iguest/azmod -I$T -o "$d/$v" $T/ovclip_test.c
done
suite "overlay put clipping" "$d/real"

# clocktzlist_test.c: the Clock's time zone list (screen_overlays.c AZ_CLOCK_TZ)
d="$OUT/clocktzlist"; mkdir -p "$d"
for v in real ctl-cycle ctl-noskip ctl-noshow ctl-dragpick ctl-headpick ctl-noscreen ctl-setnoreq ctl-noedit; do menu_variant "$d/src-$v" screen_overlays.c menu_input.c menu_full.c; done
# control 1: the row does not open the list (the zone stays as it was)
mutate guest/azmod/menu_input.c "$d/src-ctl-cycle/menu_input.c" 's/            clock_tz_open\(s, fb\);\n/            (void) fb;\n/'
# control 2: the knob lands on headings too
mutate guest/azmod/screen_overlays.c "$d/src-ctl-noskip/screen_overlays.c" 's/            if \(row\[r\] >= 0\)\n                break;/            break;/'
# control 3: the knob does not scroll the list, so the selection walks off the screen
mutate guest/azmod/screen_overlays.c "$d/src-ctl-noshow/screen_overlays.c" 's/        s->menu_sel = r;\n        tzl_show\(fb, r\);/        s->menu_sel = r;/'
# control 4: a release after a drag still picks the zone under it
mutate guest/azmod/menu_full.c "$d/src-ctl-dragpick/menu_full.c" 's/    if \(g_rdrag\.moved\) \{\n        g_mhit = "release:drag";/    if (0) {\n        g_mhit = "release:drag";/'
# control 5: a heading picks (a region's index stored as a zone)
mutate guest/azmod/screen_overlays.c "$d/src-ctl-headpick/screen_overlays.c" 's/    if \(row < 0 \|\| row >= n \|\| rows\[row\] < 0\)\n        return; \/\* a heading or nothing: no zone \*\//    if (row < 0 || row >= n)\n        return;/'
# control 6: the Clock row opens no screen of its own
mutate guest/azmod/screen_overlays.c "$d/src-ctl-noscreen/screen_overlays.c" 's/    return it == AZF_CLOCK \? AZ_CLOCK : it/    return it/'
# control 7: Set Time hands nothing over
mutate guest/azmod/screen_overlays.c "$d/src-ctl-setnoreq/screen_overlays.c" 's/    __atomic_store_n\(&g_azmod_clock_set_req, t, __ATOMIC_RELEASE\);\n//'
# control 8: the knob always moves between rows (no editing)
mutate guest/azmod/screen_overlays.c "$d/src-ctl-noedit/screen_overlays.c" 's/        if \(g_cset_edit && s->menu_sel < CS_SET_ROWN\)/        if (0)/'
for v in real ctl-cycle ctl-noskip ctl-noshow ctl-dragpick ctl-headpick ctl-noscreen ctl-setnoreq ctl-noedit; do
    $CC -std=gnu11 -O1 -w -Iguest/azmod -I$T -DMENU_C="\"$d/src-$v/menu.c\"" -DCM_CATALOG="\"$d/catalog-$v\"" \
        -o "$d/$v" $T/clocktzlist_test.c "$OUT/menuscroll/stubs.o"
done
suite "clock zone list" "$d/real"

# ---------------------------------------------------------------- Community Mods home page by touch
# cmtouch_test.c: the home page's press is decided on its release (menu_full.c azmod_pane_drag), so a
# drag scrolls it; the knob scrolls it with no focus; EP147's own scroll bar while it overflows.
d="$OUT/cmtouch"; mkdir -p "$d"
for v in real ctl-knobfocus ctl-noscroll ctl-nobar ctl-header ctl-noclamp ctl-slow ctl-staleglide ctl-lag ctl-drawleft ctl-railedge; do menu_variant "$d/src-$v" screen_community.c menu_full.c menu_composite.c; done
# control 1: the home page is handled like the other pages again (knob focus, a press fires at once)
mutate guest/azmod/screen_community.c "$d/src-ctl-knobfocus/screen_community.c" 's/    if \(g_cm_view == CMV_HOME\) \{\n        \/\* The home page works by touch/    if (0) {\n        \/* The home page works by touch/'
# control 2: a drag on the page moves nothing
mutate guest/azmod/menu_full.c "$d/src-ctl-noscroll/menu_full.c" 's/        cmoved = cm_home_scroll_to\(fb, g_rdrag\.s0 - dy, phase\);/        cmoved = false;/'
# control 3: no scroll bar
mutate guest/azmod/screen_community.c "$d/src-ctl-nobar/screen_community.c" 's/        if \(draw\) draw_vsb\(fb, cm_isplit\(\) \? bx \+ bw \/ 2 - pad \/ 2 : bx \+ bw \+ pad, by, bh, content, bh, g_cm_scroll\);\n//'
# control 4: a press on the header hits the page's control scrolled up under it
mutate guest/azmod/screen_community.c "$d/src-ctl-header/screen_community.c" 's/    if \(ty < dock_y\(fb\) \+ menu_hdrh\(fb\) \|\| ty >= dock_y\(fb\) \+ dock_h\(fb\)\)\n        return -1;\n//'
# control 5: the knob scrolls past the ends
mutate guest/azmod/screen_community.c "$d/src-ctl-noclamp/screen_community.c" 's/\(float\) cm_scroll_clamp\(\(int\) \(from \+ 0\.5f\) \+ d \* c_rrowh\(fb\), g_cm_smax\)/(float) ((int) (from + 0.5f) + d * c_rrowh(fb))/; s/glide_step\(&g_cm_gl, cm_ms\(\), \(float\) g_cm_smax\);/glide_step(\&g_cm_gl, cm_ms(), 1e9f);/'
# control 6: every home control counts as navigation (a slow press on a view button switches it)
mutate guest/azmod/screen_community.c "$d/src-ctl-slow/screen_community.c" 's/    return k == CMC_TAB \|\| k == CMC_TILE \|\| k == CMC_STILE \|\| k == CMC_SEEALL \|\| k == CMC_MOD \|\| k == CMC_INSTCAT \|\| k == CMC_BACK \|\|\n           k == CMC_SORTBTN \|\| k == CMC_SORT \|\| k == CMC_SORTOFF;/    return k >= 0;/'
# control 8: the glide forgets that it took over an offset set from outside, so it cancels every later drag
mutate guest/azmod/screen_community.c "$d/src-ctl-staleglide/screen_community.c" 's/    g_cm_glpx = g_cm_scroll;\n}/}/'
# control 11: the finger margin is tried before an exact hit again (the top of a rail tab picks the tab above)
mutate guest/azmod/screen_community.c "$d/src-ctl-railedge/screen_community.c" 's/    for \(int pass = 0; pass < 2; pass\+\+\) \{\n        int pad = pass \? fb->h \* 8 \/ 800 : 0;\n        for \(int k = nb;/    for (int pass = 1; pass < 2; pass++) {\n        int pad = pass ? fb->h * 8 \/ 800 : 0;\n        for (int k = nb;/'
# control 10: the full screen draws the Mod Menu's left list under it again (unseen, every frame)
mutate guest/azmod/menu_composite.c "$d/src-ctl-drawleft/menu_composite.c" 's/if \(s->mode == AZ_COMMUNITY && g_cm_full && !cm_animating\(\)\) \{/if (0) {/'
# control 9: a drag eases toward the finger instead of being at it (the page trails the finger)
mutate guest/azmod/screen_community.c "$d/src-ctl-lag/screen_community.c" 's/    else glide_drag\(&g_cm_gl, \(float\) v, cm_ms\(\)\);/    else glide_to(\&g_cm_gl, (float) v, cm_ms());/'
$CC -w -Iguest/azmod -Dmain=lanetest_main -Dazmod_ttf_menu=lanetest_ttf_menu -Dazmod_ttf_menu2=lanetest_ttf_menu2 \
    -c -o "$d/stubs.o" guest/azmod/lanetest.c
for v in real ctl-knobfocus ctl-noscroll ctl-nobar ctl-header ctl-noclamp ctl-slow ctl-staleglide ctl-lag ctl-drawleft ctl-railedge; do
    $CC -std=gnu11 -O1 -w -Iguest/azmod -I$T -DMENU_C="\"$d/src-$v/menu.c\"" -DCM_CATALOG="\"$d/catalog-$v\"" \
        -o "$d/$v" $T/cmtouch_test.c "$d/stubs.o"
done
suite "community touch" "$d/real"

# cmorderly_test.c: guest/azmod/cm_orderly.h (each control: a changed copy of the header, found first)
d="$OUT/cmorderly"; mkdir -p "$d"
for v in real ctl-always ctl-noraise ctl-firstseen ctl-norestore; do mkdir -p "$d/inc-$v"; done
cp guest/azmod/cm_orderly.h "$d/inc-real/cm_orderly.h"
# control 1: every SIGTERM clears the count (a frozen deck's too)
mutate guest/azmod/cm_orderly.h "$d/inc-ctl-always/cm_orderly.h" 's/if \(g_cmo_path\[0\] && cmo_alive\([^\n]*\)\)\n/if (g_cmo_path[0])\n/'
# control 2: the handler does not raise the signal again (EP147 would keep running)
mutate guest/azmod/cm_orderly.h "$d/inc-ctl-noraise/cm_orderly.h" 's/    raise\(sig\);\n/    (void) 0;\n/'
# control 3: the first count seen counts as a heartbeat
mutate guest/azmod/cm_orderly.h "$d/inc-ctl-firstseen/cm_orderly.h" 's/        if \(g_cmo_seen >= 0\)\n            __atomic_store_n/        __atomic_store_n/'
# control 4: the default disposition instead of the one before the handler
mutate guest/azmod/cm_orderly.h "$d/inc-ctl-norestore/cm_orderly.h" 's/sigaction\(sig, &g_cmo_old, NULL\);/signal(sig, SIG_DFL);/'
for v in real ctl-always ctl-noraise ctl-firstseen ctl-norestore; do
    $CC -std=gnu11 -O1 -w -I"$d/inc-$v" -Iguest/azmod -I$T -o "$d/$v" $T/cmorderly_test.c -lpthread
done
suite "community orderly stop" "$d/real"

# ---------------------------------------------------------------- community icons (PNG gate)
d="$OUT/png"; mkdir -p "$d"
awk '/^static uint32_t cmi_crc/ {p=1} p {print} p && /no IEND/ {q=1} q && /^}$/ {exit}' guest/azmod/community_icons.c > "$d/real.inc"
grep -q 'cmi_png_ok' "$d/real.inc" || { echo "extraction of cmi_png_ok failed"; exit 2; }
# control: the CRC check removed (a corrupted chunk would reach the decoder)
mutate "$d/real.inc" "$d/ctl-nocrc.inc" 's/if ?\(cmi_crc\(type, len \+ 4\) != cmi_be32\(type \+ 4 \+ len\)\) return 0;//'
python3 - "$d" <<'PY'
import struct, zlib, sys, os
d = sys.argv[1]
def chunk(t, data): return struct.pack(">I", len(data)) + t + data + struct.pack(">I", zlib.crc32(t + data) & 0xffffffff)
def png(w, h, ct=6, depth=8, interlace=0, extra=b"", rows=None, plte=None):
    bpp = {6: 4, 2: 3, 3: 1}[ct] * (2 if depth == 16 else 1)
    raw = b"".join(b"\0" + b"\x80" * (w * bpp) for _ in range(h))
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ct, 0, 0, interlace))
    if plte: out += chunk(b"PLTE", plte)
    return out + extra + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
def w(name, b): open(os.path.join(d, name), "wb").write(b)
good = png(256, 256)
w("rgba256.png", good); w("rgb64.png", png(64, 64, ct=2)); w("pal32.png", png(32, 32, ct=3, plte=b"\x10\x20\x30" * 4))
bad = bytearray(good); bad[-20] ^= 1; w("badcrc.png", bytes(bad))
w("trailing.png", good + b"junk"); w("interlaced.png", png(32, 32, interlace=1)); w("depth16.png", png(32, 32, depth=16))
meta = (chunk(b"iCCP", b"x\0\0" + zlib.compress(b"y" * 100)) + chunk(b"tEXt", b"Software\0ImageMagick")
        + chunk(b"tIME", b"\x07\xea\x0a\x09\x0c\x00\x00") + chunk(b"bKGD", b"\0\0\0\0\0\0") + chunk(b"zTXt", b"c\0\0" + zlib.compress(b"z" * 50)))
w("editor.png", png(32, 32, extra=meta)); w("editor-plain.png", png(32, 32))
w("critical.png", png(32, 32, extra=chunk(b"ABCD", b"1234")))
w("shot1280.png", png(1280, 800, ct=2)); w("shot1281.png", png(1281, 800, ct=2)); w("shot801.png", png(1280, 801, ct=2))
w("big1024.png", png(1024, 1024)); w("noiend.png", good[:-12])
lo = bytearray(good); lo[33:37] = struct.pack(">I", 1 << 30); w("lenover.png", bytes(lo))
w("twoihdr.png", good[:33] + chunk(b"IHDR", struct.pack(">IIBBBBB", 8, 8, 8, 6, 0, 0, 0)) + good[33:])
w("notpng.png", b"GIF89a" + b"\0" * 64)
PY
for v in real ctl-nocrc; do
    $CC "${CFLAGS[@]}" -I$T -DPNG_INC="\"$d/$v.inc\"" -o "$d/$v" $T/png_test.c
done
suite "png" "$d/real" "$d"

# ---------------------------------------------------------------- jog stems (the jog display)
# jog_stems_logic.h whole and the hook bodies of jog_display.c
d="$OUT/jogstems"; mkdir -p "$d"
cp guest/azmod/jog_stems_logic.h "$d/logic-real.h"
region guest/azmod/jog_display.c '^#define JOG_PADINFO_FILL ' '^/\* Constructor phase' > "$d/hooks-real.inc"
region guest/azmod/jog_display.c '^/\* Message thread \(rtick_hook\), diagnostics on' '^@@never@@' >> "$d/hooks-real.inc"
grep -q 'jogstems_tick' "$d/hooks-real.inc" || { echo "extraction of the jog log tick failed"; exit 2; }
grep -q 'jog_cue_slot_hook' "$d/hooks-real.inc" && grep -q 'jog_padinfo_hook' "$d/hooks-real.inc" &&
    grep -q 'jog_cues_build' "$d/logic-real.h" || { echo "extraction of the jog display code failed"; exit 2; }
# controls in the logic: the user's Pad Info Display setting ignored; Pad Info turned off (the
# artwork page, which has no waveform preview); a stem that is off shown as set
mutate "$d/logic-real.h" "$d/logic-ctl-nosetting.h" 's/if \(!\(word & JOG_ON\) \|\| \*d4 != 1\)/if (!(word \& JOG_ON))/'
mutate "$d/logic-real.h" "$d/logic-ctl-artpage.h" 's/    \*d5 = 0;\n    return word;/    *(unsigned char *) d4 = 0;\n    *d5 = 0;\n    return word;/'
mutate "$d/logic-real.h" "$d/logic-ctl-offset.h" 's/p < 4 \? \(int\) \(\(mask >> s\) & 1u\) : s == solo/p < 4 ? 1 : s == solo/'
# controls in the hooks: EP147's own table not replaced inside the update; EP147's table not
# sent back when stems stop; the queue's vtable not checked; a default deck in the record; the
# deck left set after the collector's update
mutate "$d/hooks-real.inc" "$d/hooks-ctl-nosubst.inc" 's/        if \(active & JOG_ON\) \{\n            struct jog_cue cues/        if (0) {\n            struct jog_cue cues/'
mutate "$d/hooks-real.inc" "$d/hooks-ctl-norestore.inc" 's/        g_orig_jog_pads\(view, \(const void \*\) \(self \+ JOG_CUE_TABLE\), hdr\);\n//'
mutate "$d/hooks-real.inc" "$d/hooks-ctl-noviewvt.inc" 's/ \|\| \*\(const uintptr_t \*\) view != \(uintptr_t\) JOG_TIQ_VT\)/)/'
mutate "$d/hooks-real.inc" "$d/hooks-ctl-deck0.inc" 's/unsigned d = \*\(const unsigned char \*\) \(pid \+ JOG_PID_DECK\);/unsigned d = 0;/'
mutate "$d/hooks-real.inc" "$d/hooks-ctl-tlsleak.inc" 's/    g_orig_jog_cue_update\(self, changed, reset\);\n    t_jog_deck = -1;/    g_orig_jog_cue_update(self, changed, reset);/'
# preview-strip controls: off stems drawn tall; EP147's strip not kept (restores a stale one);
# no restore when stems stop; the deck of any queue taken as deck 2
mutate "$d/logic-real.h" "$d/logic-ctl-stripoff.h" 's/c\.h = \(uint8_t\) \(on \? JOG_PREV_ON_H : JOG_PREV_OFF_H\);/c.h = (uint8_t) JOG_PREV_ON_H;/'
mutate "$d/hooks-real.inc" "$d/hooks-ctl-stripkeep.inc" 's/    memcpy\(g_jp\[d\]\.ep, v->begin, sizeof\(struct jog_prev_col\) \* \(size_t\) n\);\n//'
mutate "$d/hooks-real.inc" "$d/hooks-ctl-striprestore.inc" 's/            g_orig_jog_prev\(view, &v, g_jp\[d\]\.a, g_jp\[d\]\.b, g_jp\[d\]\.c\);\n//'
mutate "$d/logic-real.h" "$d/logic-ctl-stripfull.h" 's/    return n > JOG_PREV_SENT \? JOG_PREV_SENT : n;/    return n;/'
mutate "$d/hooks-real.inc" "$d/hooks-ctl-stripdeck.inc" 's/            d = i;\n/            d = i;\n    if (d < 0)\n        d = 1;\n/'
mutate "$d/logic-real.h" "$d/logic-ctl-xferseq.h" 's/    if \(x->have_seq && m->seq == x->seq\)\n        return 0;\n//'
for v in real ctl-nosetting ctl-artpage ctl-offset ctl-nosubst ctl-norestore ctl-noviewvt ctl-deck0 ctl-tlsleak ctl-xferseq ctl-stripoff ctl-stripkeep ctl-striprestore ctl-stripdeck ctl-stripfull; do
    lg="$d/logic-real.h"; hk="$d/hooks-real.inc"
    [ -f "$d/logic-$v.h" ] && lg="$d/logic-$v.h"
    [ -f "$d/hooks-$v.inc" ] && hk="$d/hooks-$v.inc"
    $CC "${CFLAGS[@]}" -pthread -I$T -DJOG_LOGIC_INC="\"$lg\"" -DJOG_HOOK_INC="\"$hk\"" \
        -o "$d/$v" $T/jogstems_test.c
done
suite "jog stems" "$d/real"

# ---------------------------------------------------------------- community icons (while decoding)
# azcm_icon: a queued decode draws nothing, a refused or overdue one the placeholder, a decoded
# one its pixels; a finished icon moves the counter the Community screen redraws on.
d="$OUT/iconwait"; mkdir -p "$d"
awk '/^#define CMI_N / {p=1} p {print} /^const uint32_t \*azcm_icon\(/ {f=1} f && /^}$/ {exit}' guest/azmod/community_icons.c > "$d/real.inc"
grep -q 'azcm_icons_done' "$d/real.inc" && grep -q 'CMI_WAIT_MS' "$d/real.inc" || { echo "extraction of azcm_icon failed"; exit 2; }
# controls: a queued decode not reported as pending (the placeholder shows, then the picture
# replaces it), and the wait never running out (a stuck decode leaves the tile empty for good)
mutate "$d/real.inc" "$d/ctl-nowait.inc" 's/    \*pending = 1;\n    return NULL;/    return NULL;/'
mutate "$d/real.inc" "$d/ctl-forever.inc" 's/ && now_ms\(\) - a\[i\]\.asked < CMI_WAIT_MS//'
for v in real ctl-nowait ctl-forever; do
    $CC "${CFLAGS[@]}" -I$T -DICON_INC="\"$d/$v.inc\"" -o "$d/$v" $T/iconwait_test.c
done
suite "icon wait" "$d/real"
suite "icon wait (no decode hook)" "$d/real" nohook

# ---------------------------------------------------------------- bus settings, no torn reads
# guest/azmod/bus_cfg.h as it ships + bus_cfg_publish (mod_menu_overlay.c): the bus threads see
# whole published settings combinations and whole LED blocks. The suite carries its own
# in-round control (the old direct reads of the globals must show mixes).
d="$OUT/buscfg"; mkdir -p "$d/ctl-norecheck"
extract guest/azmod/mod_menu_overlay.c '^static void bus_cfg_publish\(void\) \{' > "$d/publish.inc"
grep -q 'bus_cfg_pack' "$d/publish.inc" || { echo "extraction of bus_cfg_publish failed"; exit 2; }
# control: seqblk_read without the second look at the counter (a torn block accepted)
mutate guest/azmod/bus_cfg.h "$d/ctl-norecheck/bus_cfg.h" 's/    if \(__atomic_load_n\(&b->seq, __ATOMIC_RELAXED\) != s1\)\n        return 0;\n//'
$CC "${CFLAGS[@]}" -pthread -Iguest/azmod -DPUBLISH_INC="\"$d/publish.inc\"" -o "$d/real" $T/buscfg_test.c
$CC "${CFLAGS[@]}" -pthread -I"$d/ctl-norecheck" -Iguest/azmod -DPUBLISH_INC="\"$d/publish.inc\"" -o "$d/ctl-norecheck/t" $T/buscfg_test.c
suite "bus settings (no torn reads)" "$d/real"
echo "=== busread lint ==="
if ! plain_only "busread lint"; then :; elif "$T/busread-lint.sh" guest; then :; else failures=$((failures + 1)); fi
BL="$OUT/busread-lint"; mkdir -p "$BL/azmod"
# control: read() takes the Playlist Editing toggle from g_state again (the old line)
mutate guest/azmod/bus_interpose.c "$BL/azmod/bus_interpose.c" \
    's/__atomic_store_n\(&g_reorder_enabled, bus_cfg_reorder\(cfg\), __ATOMIC_RELAXED\);/__atomic_store_n(\&g_reorder_enabled, g_state.mod_enabled[AZMOD_ACTION_PLEDIT], __ATOMIC_RELAXED);/'
echo

echo "=== Community Mods layouts ==="
if CC="$CC" "$T/cmlayout-golden.sh" guest "$OUT/cmlayout/real"; then :; else failures=$((failures + 1)); fi
CG="$OUT/cmlayout/ctl-guest"; mkdir -p "$CG"; cp -R guest/azmod "$CG/"
# control: the full-screen header's divider one pixel taller
mutate guest/azmod/screen_community.c "$CG/azmod/screen_community.c.new" \
    's/fill_rect\(fb, 0, top \+ hh - 2, fb->w, 2, C_DIVIDER\);/fill_rect(fb, 0, top + hh - 3, fb->w, 3, C_DIVIDER);/'
mv "$CG/azmod/screen_community.c.new" "$CG/azmod/screen_community.c"
echo

echo "=== busread trace ==="
if CC=$CC "$T/busread-trace.sh" guest "$OUT/bustrace/real" && san_reports; then :; else failures=$((failures + 1)); fi
BT="$OUT/bustrace/ctl"; mkdir -p "$BT/azmod"; cp guest/azmod/*.h "$BT/azmod/"
# control: the overlay watchdog trips one frame later
mutate guest/azmod/bus_interpose.c "$BT/azmod/bus_interpose.c" \
    's/if \(wd_stall > 200u\)\n        menu_open = 0;/if (wd_stall > 201u)\n        menu_open = 0;/'
# control: community filters see mixer frames that carry no CRC (the gate removed)
BU="$OUT/bustrace/ctlcrc"; mkdir -p "$BU/azmod"; cp guest/azmod/*.h "$BU/azmod/"
mutate guest/azmod/bus_interpose.c "$BU/azmod/bus_interpose.c" \
    's/    if \(crc_in\) \/\* community mods see only checked input[^\n]*\n        sdk_filter_raw/    sdk_filter_raw/'
# control: a community mod's surface holds the knob for the whole replay
mutate "$T/busread_trace_test.c" "$OUT/bustrace/focus_test.c" \
    's/static _Atomic int g_sdk_focus, g_sdk_back_pending;/static _Atomic int g_sdk_focus = 1, g_sdk_back_pending;/'
echo

echo "=== ssh key approval (the Mod Menu's question) ==="
SA="$OUT/sshapprove"; mkdir -p "$SA"
sa_build() { $CC -O1 -Wall -I guest/azmod -I "$T" -DSSHA_C="\"$1\"" -o "$2" "$T/sshapprove_test.c"; }
if sa_build "$PWD/guest/azmod/ssh_approve.c" "$SA/real"; then suite "ssh key approval" "$SA/real"
else echo "FAIL  ssh key approval: the suite did not build"; failures=$((failures + 1)); fi
# controls: Cancel approves too; the question pops up while a track is loaded; a canceled key is
# asked about again on every tick
mutate guest/azmod/ssh_approve.c "$SA/ctl1.c" 's/    if \(!yes\)\n        return;\n//'
mutate guest/azmod/ssh_approve.c "$SA/ctl2.c" 's/    if \(azcm_any_loaded\(\)\)\n        return; \/\* never in the middle of a set \*\/\n//'
mutate guest/azmod/ssh_approve.c "$SA/ctl3.c" 's/    ssha_mark\(&g_ssha, fps\[i\]\);\n//'
for k in 1 2 3; do sa_build "$SA/ctl$k.c" "$SA/ctl$k" || failures=$((failures + 1)); done
echo

echo "=== sdk input filters ==="
SI="$OUT/sdkinput"; mkdir -p "$SI"
si_build() { $CC -O1 -Wall -I guest/azmod -DSDK_INPUT_C="\"$1\"" -o "$2" "$T/sdkinput_test.c"; }
if si_build "$PWD/guest/azmod/sdk_input.c" "$SI/real"; then suite "sdk input filters" "$SI/real"
else echo "FAIL  sdk input filters: the suite did not build"; failures=$((failures + 1)); fi
# controls: a passed touch is offered again mid-press; a slow filter is never switched off;
# the raw stage hands out the frame itself instead of a copy
mutate guest/azmod/sdk_input.c "$SI/ctl1.c" 's/                g_sdk_touch_decided = 1;\n//'
mutate guest/azmod/sdk_input.c "$SI/ctl2.c" 's/    __atomic_store_n\(off, 1, __ATOMIC_RELEASE\);\n//'
mutate guest/azmod/sdk_input.c "$SI/ctl3.c" 's/ff->fn\(bus, copy, m, ff->user\);/ff->fn(bus, (unsigned char *) f, m, ff->user);/'
# control 4: the owner of a touch is called whatever its state, and never timed (the old path)
mutate guest/azmod/sdk_input.c "$SI/ctl4.c" 's/                if \(!__atomic_load_n\(&tf->off, __ATOMIC_ACQUIRE\) && tf->c->state == AZCM_LOADED\) \{\n                    g_cm_in_frame = 1;\n                    uint64_t t0 = sdk_ticks\(\);\n                    tf->fn\(&t, tf->user\);\n                    if \(sdk_ticks\(\) - t0 > g_sdk_budget_ticks\)\n                        sdk_strike\(tf->c, &tf->strikes, &tf->off, "touch filter"\);\n                    g_cm_in_frame = 0;\n                \}/                g_cm_in_frame = 1;\n                tf->fn(\&t, tf->user);\n                g_cm_in_frame = 0;/'
for k in 1 2 3 4; do si_build "$SI/ctl$k.c" "$SI/ctl$k" || failures=$((failures + 1)); done
echo

# ---------------------------------------------------------------- art targets from a stick
# az_art_target_len (azmod.c) + the generated image table (ep147png.h): a stick's N.addr may only
# name the first byte of one of this build's PNG images, with a PNG that fits. The shipped art
# sets (assets/azmod-pngpatch; the 2.00 set from the desktop app's source when found) must
# all still be accepted.
d="$OUT/art"; mkdir -p "$d"
{ grep -E '^static const unsigned char AZ_PNG_SIG' guest/azmod/azmod.c
  extract guest/azmod/azmod.c '^static size_t az_art_target_len\(' ; } > "$d/real.inc"
grep -q 'ep147png_len' "$d/real.inc" || { echo "extraction of az_art_target_len failed"; exit 2; }
# control 1: any address accepted, its length taken from elsewhere (what a live-memory check allowed)
mutate "$d/real.inc" "$d/ctl-anyaddr.inc" 's/(uint32_t len = ep147png_len\(g_ep147_build, va\);)/$1\n    if (!len)\n        len = 65536;/'
# control 2: the fit check dropped
mutate "$d/real.inc" "$d/ctl-nofit.inc" 's/ \|\| n > len//'
# control 3: one build's table for every build
mutate "$d/real.inc" "$d/ctl-anybuild.inc" 's/ep147png_len\(g_ep147_build, va\)/ep147png_len(EPB_V130, va)/'
for v in real ctl-anyaddr ctl-nofit ctl-anybuild; do
    $CC "${CFLAGS[@]}" -Wno-unused-function -I$T -Iguest/azmod -DART_INC="\"$d/$v.inc\"" -o "$d/$v" $T/arttarget_test.c
done
ART_ARGS=()
if [ "$PUBLIC" = 1 ]; then
    # the stick art blobs are images taken from the firmware, which the published tree does not carry
    suite "art targets" "$d/real"
    skip "the shipped art sets: firmware-derived images the published tree does not carry"
else
    ART130=assets/azmod-pngpatch
    ART200=
    for m in "${AZ_MODS:-}" "$ROOT/../AZ-MODS"; do
        [ -n "$m" ] && [ -f "$m/backend/azmods/mods/azmod-pngpatch/0.addr.2.00" ] && { ART200=$m/backend/azmods/mods/azmod-pngpatch; break; }
    done
    ART_ARGS=("$ART130" ${ART200:+"$ART200"})
    suite "art targets" "$d/real" "${ART_ARGS[@]}"
fi
echo "=== art table vs the binaries ==="
# AZ_EP147_V130 / AZ_EP147_V200: the EP147 binaries of firmware 1.30 and 2.00, when available
EPV130=${AZ_EP147_V130:-}; EPV200=${AZ_EP147_V200:-}
if ! plain_only "the art table check"; then :; elif [ -f "$EPV130" ] && [ -f "$EPV200" ]; then
    python3 tools/ep147-pngtable.py --check "$EPV130" "$EPV200" || failures=$((failures + 1))
else
    skip "EP147 binaries not given (AZ_EP147_V130, AZ_EP147_V200): ep147png.h not regenerated here (not a pass)"
fi
echo

# ---------------------------------------------------------------- hook relocation + page protection
# azmod's and azstem's adrp re-encoding (only when it fits +-4 GB), the function-shape refusal
# (a ret/br inside the instructions a patch covers) and azprot.h's restore-what-was-there rule.
d="$OUT/hook"; mkdir -p "$d"
for f in azm_pcrel_kind azm_adrp_target azm_adrp_reloc_ok azm_reloc_adrp azm_ends_fn azm_patch_ok azm_emit_abs_addr azm_abs_jump; do
    extract guest/azmod/inline_hooks.c "^static [a-z0-9_]+ $f\\("
done > "$d/azm.inc"
for f in pcrel_kind ends_fn reloc_adrp adrp_reloc_ok adrp_target emit_abs_addr emit_abs_jump; do
    extract guest/azstem/azstem.c "^static [a-z0-9_]+ $f\\("
done > "$d/stem.inc"
grep -q 'azm_patch_ok' "$d/azm.inc" && grep -q 'azm_abs_jump' "$d/azm.inc" && grep -q 'ends_fn' "$d/stem.inc" &&
    grep -q 'emit_abs_jump' "$d/stem.inc" || { echo "extraction of the hook relocation helpers failed"; exit 2; }
# control 1: an adrp always taken as re-encodable (the truncating re-encode)
mutate "$d/azm.inc" "$d/azm-ctl-anyreach.inc" 's/return nimm >= -\(1LL << 20\) && nimm < \(1LL << 20\);/return 1;/'
mutate "$d/stem.inc" "$d/stem-ctl-anyreach.inc" 's/return nimm >= -\(1LL << 20\) && nimm < \(1LL << 20\);/return 1;/'
# control 2: the reach bound off by one page
mutate "$d/azm.inc" "$d/azm-ctl-edge.inc" 's/nimm < \(1LL << 20\);/nimm <= (1LL << 20);/'
# control 3: the function-shape check dropped (a short function's neighbor overwritten)
mutate "$d/azm.inc" "$d/azm-ctl-shape.inc" 's/if \(azm_ends_fn\(f\[i\]\)\)/if (0)/'
# control 4: a MOVK's shift one place off (azstem; bits 32..47 land in 16..31)
mutate "$d/stem.inc" "$d/stem-ctl-movk.inc" 's/out\[2\] = 0xF2800000u \| \(2u << 21\)/out[2] = 0xF2800000u | (1u << 21)/'
# control 5: the top 16 bits never loaded (azmod)
mutate "$d/azm.inc" "$d/azm-ctl-movk.inc" 's/\n    out\[3\] = 0xF2800000u[^\n]*/\n    out[3] = 0xD503201Fu; \/* nop *\//'
for v in real ctl-anyreach ctl-edge ctl-shape ctl-movk ctl-azmmovk; do
    ai="$d/azm.inc"; si="$d/stem.inc"
    case $v in ctl-anyreach) ai="$d/azm-ctl-anyreach.inc";; ctl-edge) ai="$d/azm-ctl-edge.inc";; ctl-shape) ai="$d/azm-ctl-shape.inc";;
               ctl-movk) si="$d/stem-ctl-movk.inc";; ctl-azmmovk) ai="$d/azm-ctl-movk.inc";; esac
    $CC "${CFLAGS[@]}" -I$T -DAZM_INC="\"$ai\"" -DSTEM_INC="\"$si\"" -o "$d/$v" $T/hookreloc_test.c
done
$CC "${CFLAGS[@]}" -I$T -DAZM_INC="\"$d/azm.inc\"" -DSTEM_INC="\"$d/stem-ctl-anyreach.inc\"" -o "$d/ctl-stem-anyreach" $T/hookreloc_test.c
suite "hook relocation" "$d/real"

d="$OUT/azprot"; mkdir -p "$d"
cp guest/common/azprot.h "$d/real.h"
# control 1: the old rule, a fixed protection whatever the page had
mutate "$d/real.h" "$d/ctl-fixed.h" 's/            if \(cur >= b\)\n                return prot;/            if (cur >= b)\n                return PROT_READ | PROT_EXEC;/'
# control 2: a range spanning two protections accepted (the first one wins)
mutate "$d/real.h" "$d/ctl-span.h" 's/if \(prot >= 0 && p != prot\)\n\s*return -1;[^\n]*//'
# control 3: a hole inside the range ignored
mutate "$d/real.h" "$d/ctl-hole.h" 's/if \(lo > cur\)\n\s*return -1;[^\n]*//'
for v in real ctl-fixed ctl-span ctl-hole; do
    $CC "${CFLAGS[@]}" -I$T -DAZPROT_INC="\"$d/$v.h\"" -o "$d/$v" $T/azprot_test.c
done
suite "azprot" "$d/real" "$d"

# the same rules on aarch64 Linux with real /proc/self/maps: installs leave code, trampolines and
# islands r-x, a vtable patch restores r-- or r-x as the page had (guest/azmod/test/hooks)
# (the control header is copied under guest/out: docker shares the source tree, not $TMPDIR)
HOOKCTL="$ROOT/guest/out/unit-hookctl"; mkdir -p "$HOOKCTL"; cp "$OUT/azprot/ctl-fixed.h" "$HOOKCTL/"
hooktest() { docker run --rm --platform=linux/arm64 -v "$ROOT/guest":/g:ro -v "$HOOKCTL":/ctl:ro \
    -e HOOKTEST_CFLAGS="$1" azmod-build:18.04 bash -c 'cd /g && bash azmod/test/hooks/run.sh'; }
if [ "$PUBLIC" = 1 ]; then
    # the hook host test drives inline_hooks.c through a test-only switch the release sources
    # do not carry, and its crash-chain control is an earlier revision from the git history
    skip "hooks (aarch64): needs the development sources (test-only switches)"
else
    echo "=== hooks (aarch64, real page tables) ==="
    if ! plain_only "hooks (aarch64, built in azmod-build:18.04)"; then :; elif hooktest ""; then :; else failures=$((failures + 1)); fi
    echo
    # Redirect order controls (hooktest section 5): the saved original stored after the redirect,
    # the 1.1.2 overlay_thread shape. Each must fail its order probe.
    mutate guest/azmod/inline_hooks.c "$HOOKCTL/inline_hooks-late-island.c" \
        's/        __atomic_store_n\(orig, \(void \*\) tramp, __ATOMIC_RELAXED\);[^\n]*\n(        __atomic_store_n\(func, b, __ATOMIC_RELEASE\);[^\n]*\n)/$1        __atomic_store_n(orig, (void *) tramp, __ATOMIC_RELAXED);\n/'
    mutate guest/azmod/inline_hooks.c "$HOOKCTL/inline_hooks-late-slot.c" \
        's/(    __atomic_store_n\(orig, \*slot, __ATOMIC_RELAXED\);\n)(    __atomic_store_n\(slot, hook, __ATOMIC_RELEASE\);\n)/    void *was = *slot;\n$2    __atomic_store_n(orig, was, __ATOMIC_RELAXED);\n/'
    mutate guest/azstem/azstem.c "$HOOKCTL/azstem-late-orig.c" \
        's/    __atomic_store_n\(orig, \(void \*\) tramp, __ATOMIC_RELAXED\);\n    __atomic_thread_fence\(__ATOMIC_RELEASE\);\n//; s/(    logln\("install: hooked @)/    __atomic_store_n(orig, (void *) tramp, __ATOMIC_RELAXED);\n$1/'
    # Late-phase controls (hooktest section 6): the 16-byte fallback taken after the constructor
    # (no phase check), and azstem's installer without its after-the-constructor refusal.
    mutate guest/azmod/inline_hooks.c "$HOOKCTL/inline_hooks-late-fallback.c" \
        's/    if \(!isl && __atomic_load_n\(&g_azm_late, __ATOMIC_ACQUIRE\)\) \{/    if (0) {/'
    mutate guest/azstem/azstem.c "$HOOKCTL/azstem-late-any.c" \
        's/    if \(atomic_load_explicit\(&g_hooks_late, memory_order_acquire\)\) \{/    if (0) {/'
    # crash-chain controls (hooks/crashchain.c): the reporter's earlier revision, verbatim (one owner:
    # a thread faulting while another is inside cannot see its own re-entry), and the per-thread
    # re-entry check removed
    # (adapted to cm_blame_fault's current signature, so the control fails for its own reason)
    git show 3a6b90d:guest/azmod/crash_guards.c | perl -0pe 's/\(uintptr_t\) uc->uc_mcontext.regs\[29\], g_cr_mem\);/(uintptr_t) uc->uc_mcontext.regs[29]);/' > "$HOOKCTL/crash_guards-owner.c"
    grep -q 'regs\[29\]);' "$HOOKCTL/crash_guards-owner.c" || { echo "crash-chain control: adaptation did not apply"; exit 2; }
    mutate guest/azmod/crash_guards.c "$HOOKCTL/crash_guards-noreentry.c" \
        's/    if \(cr_inside_find\(tid\) >= 0\) \{/    if (0) {/'
    # a raised or sent signal never re-raised after the re-entry (the process carries on)
    mutate guest/azmod/crash_guards.c "$HOOKCTL/crash_guards-noraise.c" \
        's/(    if \(cr_inside_find\(tid\) >= 0\) \{\n(?:.*\n){5})        if \(sig == SIGABRT \|\| si->si_code <= 0\)\n            raise\(sig\);\n/$1/'
    # the blamed community mod left out of the report
    mutate guest/azmod/crash_guards.c "$HOOKCTL/crash_guards-noblame.c" 's/    if \(blamed\) \{ \/\*/    if (0 \&\& blamed) { \/*/'
    hooktest_crash() { docker run --rm --platform=linux/arm64 -v "$ROOT/guest":/g:ro -v "$HOOKCTL":/ctl:ro \
        -e HOOKTEST_CRASH="$1" azmod-build:18.04 bash -c 'cd /g && bash azmod/test/hooks/run.sh'; }
    hooktest_stem() { docker run --rm --platform=linux/arm64 -v "$ROOT/guest":/g:ro -v "$HOOKCTL":/ctl:ro \
        -e HOOKTEST_STEM="$1" azmod-build:18.04 bash -c 'cd /g && bash azmod/test/hooks/run.sh'; }
fi

# Alternate signal stacks (guest/common/azaltstack.h), on aarch64 Linux: an overflow on a
# thread from az_thread_create, and on the main thread after azalt_this_thread(0), still runs
# an SA_ONSTACK handler; a plain pthread_create thread does not (the in-round control); 2000
# started threads leave no stacks mapped.
mkdir -p "$HOOKCTL/alt-none" "$HOOKCTL/alt-leak"
mutate guest/common/azaltstack.h "$HOOKCTL/alt-none/azaltstack.h" \
    's/(static int azalt_this_thread\(int free_at_exit\) \{\n)/$1    return 0;\n/'
mutate guest/common/azaltstack.h "$HOOKCTL/alt-leak/azaltstack.h" \
    's/    if \(free_at_exit\)\n        pthread_setspecific\(azalt_key, m \+ pg\);\n//'
alttest() { docker run --rm --platform=linux/arm64 -v "$ROOT/guest":/g:ro -v "$HOOKCTL":/ctl:ro azmod-build:18.04 \
    bash -c "gcc -O0 -pthread $1 -I/g/common -o /tmp/alt /g/common/azaltstack_test.c && /tmp/alt"; }
echo "=== alternate signal stacks (aarch64) ==="
if ! plain_only "altstack (aarch64, built in azmod-build:18.04)"; then :; elif alttest ""; then :; else failures=$((failures + 1)); fi
echo

# ---------------------------------------------------------------- the Community Mods host (aarch64)
# guest/azmod/test/community/run.sh: the real community.c hosting real mod libraries built from
# mods.c (loading, every refusal, approval, settings and folder services, hook claims, the start
# quarantine, crash blame), in azmod-build:18.04. A sanitizer pass builds the host with its
# sanitizer (HARNESS_CC) and the mods without: the host refuses a mod linked with a sanitizer
# runtime, as it would on a deck. ASan leaves fatal signals to the host's own crash handler, which
# the crash-blame cases exercise. A report from any harness process fails the suite. The copies the
# controls build from live under guest/out (docker shares the source tree, not $TMPDIR).
CMH="$ROOT/guest/out/unit-cmhost"; rm -rf "$CMH"; mkdir -p "$CMH"
cmhost() { # cmhost <guest tree> : 0 when every case passed and no sanitizer reported
    local g=$1 r rc=0 hcc=gcc f
    case $SAN in
        asan) hcc="gcc -fsanitize=address,undefined -fno-sanitize-recover=undefined" ;;
        tsan) hcc="gcc -fsanitize=thread" ;;
    esac
    r=$(mktemp -d "$CMH/san.XXXXXX")
    docker run --rm --platform=linux/arm64 --security-opt seccomp=unconfined -v "$g":/g:ro -v "$r":/san -w /g \
        -e HARNESS_CC="$hcc" \
        -e ASAN_OPTIONS=log_path=/san/report:detect_leaks=1:handle_segv=0:handle_sigbus=0:handle_abort=0:handle_sigfpe=0:handle_sigill=0 \
        -e UBSAN_OPTIONS=log_path=/san/report:print_stacktrace=1 -e TSAN_OPTIONS=log_path=/san/report \
        azmod-build:18.04 bash azmod/test/community/run.sh || rc=$?
    for f in "$r"/report.*; do
        [ -e "$f" ] || continue
        echo "---- sanitizer report ($(basename "$f"))"; cat "$f"; rc=1
    done
    rm -rf "$r"
    return $rc
}
# cmhost_variant <name> <perl substitution on community.c> : a copy of the tree the harness builds
cmhost_variant() {
    mkdir -p "$CMH/$1"
    rsync -a --exclude out guest/azmod guest/common "$CMH/$1/"
    mutate guest/azmod/community.c "$CMH/$1/azmod/community.c" "$2"
}
if [ ! -f guest/azmod/test/community/run.sh ]; then
    skip "community host (aarch64): guest/azmod/test/community is not in this tree"
else
    # control: an enabled mod that is not on the deck's approved list loads anyway
    cmhost_variant ctl-noapprove 's/    if \(!cm_is_approved\(c->id, c->sha\)\) \{/    if (0) {/'
    # control: a claim_hook within 16 bytes of an existing hook is accepted
    cmhost_variant ctl-overlap 's/if \(addr < g_hooked\[i\].addr \+ 16 && g_hooked\[i\].addr < addr \+ 16\)/if (0)/'
    # control: claim_hook taken after init() (from a tick)
    cmhost_variant ctl-lateclaim 's/    if \(!g_sdk_in_init \|\| \(c - g_cm\) != g_cm_loading\) \{ \/\* a hook goes in before the mod runs \*\//    if (0) {/'
    # control: register_screen taken after init()
    cmhost_variant ctl-latescreen 's/(cm_self\(id, CM_RA\(\), "register_screen"\);\n    if \(!c\) return -1;\n)    if \(!g_sdk_in_init \|\| \(c - g_cm\) != g_cm_loading\) \{/$1    if (0) {/'
    # control: a text setting copied byte-wise into a short buffer (half a character)
    cmhost_variant ctl-utf8 's/    utf8_copy\(buf, \(size_t\) n, v\); \/\* a short buffer[^\n]*/    snprintf(buf, (size_t) n, "%s", v);/'
    # control: a settings key with a control byte taken
    cmhost_variant ctl-crkey 's/        if \(\*q < 0x20 \|\| \*q == 0x7f\) return 0; \/\* one line per key in the file \*\//        if (*q == 0x0a) return 0;/'
    # TSan regression: the logs switch, written by the tick and read by mod threads, as a plain int
    cmhost_variant ctl-logsrace 's/static _Atomic int g_cm_logs_on = 0;/static int g_cm_logs_on = 0;/'
    suite "community host (aarch64, real mods in azmod-build:18.04)" cmhost "$ROOT/guest"
fi
echo "=== thread lint ==="
if ! plain_only "thread lint"; then :; elif "$T/thread-lint.sh" guest; then :; else failures=$((failures + 1)); fi
TL="$OUT/thread-lint"; mkdir -p "$TL/azmod" "$TL/azstem" "$TL/common"
cp guest/azmod/*.c "$TL/azmod/"; cp guest/azstem/*.c "$TL/azstem/"; cp guest/common/*.h "$TL/common/"
printf '%s\n' 'static void planted(void) { pthread_t t; pthread_create(&t, NULL, flusher, NULL); }' >> "$TL/azstem/azstem.c"
echo

echo "=== redirect lint ==="
if ! plain_only "redirect lint"; then :; elif "$T/redirect-lint.sh" guest; then :; else failures=$((failures + 1)); fi
RL="$OUT/redirect-lint"; mkdir -p "$RL/azmod" "$RL/azstem"
cp guest/azmod/*.c "$RL/azmod/"; cp guest/azstem/*.c "$RL/azstem/"
# control: a hand-written slot swap planted in day_mode.c (the shape the lint exists for)
printf '%s\n' 'static int planted_swap(void **slot, void *hook, void **orig) {' \
    '    azprot_t pt;' '    if (azprot_unlock((uintptr_t) slot, 8, &pt) != 0)' '        return -1;' \
    '    *slot = hook;' '    *orig = *slot;' '    return azprot_relock(&pt);' '}' >> "$RL/azmod/day_mode.c"
echo

echo "=== hook order lint ==="
if ! plain_only "hook order lint"; then :; elif "$T/hookorder-lint.sh" guest; then :; else failures=$((failures + 1)); fi
HO="$OUT/hookorder"; mkdir -p "$HO/c1/azmod" "$HO/c2/azmod"
cp guest/azmod/*.c "$HO/c1/azmod/"; cp guest/azmod/*.c "$HO/c2/azmod/"
# control 1: an installer back on the overlay thread (the 1.1.2 race)
mutate guest/azmod/mod_menu_overlay.c "$HO/c1/azmod/mod_menu_overlay.c" \
    's/(    install_wavestems_hook\(\);[^\n]*\n)(.*?)(    azm_hooks_dump\(AZM_HOOKTABLE_PATH\);\n)/$2$3$1/s'
# control 2: the late phase marked before the constructor's installs
mutate guest/azmod/azmod.c "$HO/c2/azmod/azmod.c" \
    's/(    install_static_hooks\(\);\n)(.*?)(    azm_hooks_go_late\(\);\n)/$3$2$1/s'
echo

# ---------------------------------------------------------------- /tmp size caps
# azlogcap.h rotation, the crash reporter's byte cap (crash_guards.c cr_put), and the remote
# stems helper's log (guest/azrs/azrs.sh). /tmp on the deck is RAM.
d="$OUT/logcap"; mkdir -p "$d/scratch"
cp guest/common/azlogcap.h "$d/real.h"
{ grep -E '^#define CR_LOG_MAX ' guest/azmod/crash_guards.c
  grep -E '^static long g_cr_bytes;' guest/azmod/crash_guards.c
  extract guest/azmod/crash_guards.c '^static void cr_put\(' ; } > "$d/crput.inc"
grep -q 'CR_LOG_MAX' "$d/crput.inc" && grep -q 'g_cr_bytes' "$d/crput.inc" && grep -q 'crash log full' "$d/crput.inc" ||
    { echo "extraction of cr_put failed"; exit 2; }
# control 1: rotation never happens (the old append-forever open)
mutate "$d/real.h" "$d/ctl-norotate.h" 's/st\.st_size <= max\)/st.st_size <= max || 1)/'
# control 2: the crash reporter writes without counting (the old cr_put)
mutate "$d/crput.inc" "$d/crput-ctl-nocap.inc" 's/if \(at \+ \(long\) n <= CR_LOG_MAX - \(long\) sizeof full\) \{/if (1) {/'
for v in real ctl-norotate ctl-nocap; do
    hi="$d/real.h"; ci="$d/crput.inc"
    case $v in ctl-norotate) hi="$d/ctl-norotate.h";; ctl-nocap) ci="$d/crput-ctl-nocap.inc";; esac
    mkdir -p "$d/scratch-$v"
    $CC "${CFLAGS[@]}" -I$T -DLOGCAP_INC="\"$hi\"" -DCRPUT_INC="\"$ci\"" -o "$d/$v" $T/logcap_test.c
done
suite "logcap" "$d/real" "$d/scratch-real"

# azrs.sh: the log() function, run on its own with a small cap
azrs_log_test() { # $1 = azrs.sh -> 0 when 2000 lines leave log <= cap + 32 lines and a log.1
    local w; w=$(mktemp -d "$OUT/azrs.XXXXXX")
    { echo "D=$w; LOG=\$D/log; AZRS_LOG_MAX=4096"
      awk '/^AZRS_LOG_MAX=/ {p=1} /^log\(\)/ {p=1} p {print} p && /; }$/ {exit}' "$1" | sed 's/^AZRS_LOG_MAX=.*AZRS_LOG_N=0$/AZRS_LOG_N=0/'
      echo 'i=0; while [ $i -lt 2000 ]; do log "line $i of a helper that keeps working"; i=$((i+1)); done'
      echo 'n=$(wc -c < $LOG); [ "$n" -le $((4096 + 32 * 60)) ] && [ -f $LOG.1 ] && tail -1 $LOG | grep -q "line 1999"'
    } > "$w/t.sh"
    sh "$w/t.sh" || { echo "FAIL  azrs: $(wc -c < "$w/log") bytes in the log after 2000 lines (cap 4096)"; return 1; }
}
echo "=== azrs log cap ==="
if ! plain_only "the azrs log cap (shell)"; then :; elif azrs_log_test guest/azrs/azrs.sh; then echo "PASS  azrs: 2000 log lines stay under the cap, newest kept, log.1 written"
else echo "FAIL  azrs: the helper log grew past its cap"; failures=$((failures + 1)); fi
mutate guest/azrs/azrs.sh "$d/azrs-ctl.sh" 's/^(\s*)if \[ \$\(\(AZRS_LOG_N % 32\)\) = 1 \].*fi\n//m'
echo

echo "=== logcap lint ==="
if ! plain_only "logcap lint"; then :; elif "$T/logcap-lint.sh" guest; then :; else failures=$((failures + 1)); fi
echo

echo "=== bufsize lint ==="
if ! plain_only "bufsize lint"; then :; elif "$T/bufsize-lint.sh" guest; then :; else failures=$((failures + 1)); fi
echo

# ---------------------------------------------------------------- verify.sh and build-check.sh
# The published tree's two entry scripts, run on bad input: each answers with its own exit code
# and a message, not a traceback or a cd error.
ENTRY="$OUT/entry" # its own name: the controls call these at the end, when d is another suite's
mkdir -p "$ENTRY/v/src/out" "$ENTRY/b/tools"
verify_test() { # $1 = verify.sh -> 0 when a malformed manifest exits 3 with "malformed manifest"
    local m out rc
    cp "$1" "$ENTRY/v/verify.sh" || return 2
    echo a > "$ENTRY/v/src/out/azmod.so"; echo b > "$ENTRY/v/src/out/azstem.so"
    for m in '{"entries": [{"files": [' '{"version": "1.2.3"}' '[1, 2]' \
             '{"entries": [{"files": [{"name": "azmod.so", "sha256": "00"}]}]}'; do
        printf '%s' "$m" > "$ENTRY/v/m.json"
        rc=0; out=$(sh "$ENTRY/v/verify.sh" "$ENTRY/v/m.json" 2>&1) || rc=$?
        if [ "$rc" != 3 ] || ! grep -q '^malformed manifest ' <<<"$out" || grep -q Traceback <<<"$out"; then
            echo "FAIL  verify.sh: manifest $m gave exit $rc:"; sed 's/^/      /' <<<"$out"; return 1
        fi
    done
    printf '{"entries": [{"files": [{"name": "azmod.so", "sha256": "00"}, {"name": "azstem.so", "sha256": "00"}]}]}' > "$ENTRY/v/m.json"
    rc=0; sh "$ENTRY/v/verify.sh" "$ENTRY/v/m.json" >/dev/null 2>&1 || rc=$?
    [ "$rc" = 1 ] || { echo "FAIL  verify.sh: a valid manifest with other hashes gave exit $rc, not 1"; return 1; }
}
buildcheck_test() { # $1 = build-check.sh -> 0 when no argument (no ../../guest here) prints a usage line, exit 2
    local out rc=0
    cp "$1" "$ENTRY/b/tools/build-check.sh" || return 2
    out=$(cd "$ENTRY/b" && bash tools/build-check.sh 2>&1) || rc=$?
    if [ "$rc" != 2 ] || ! grep -q '^usage: ' <<<"$out"; then
        echo "FAIL  build-check.sh with no argument gave exit $rc:"; sed 's/^/      /' <<<"$out"; return 1
    fi
}
# ctl_detects <test function> <args>: for a control, 1 only when the test itself reported the
# planted bug (it returned 1); a setup failure (2) reads as a control that caught nothing
ctl_detects() {
    local r=0
    "$@" || r=$?
    [ "$r" = 1 ] && return 1
    echo "the test did not run its check (exit $r)"
    return 0
}
echo "=== verify.sh / build-check.sh ==="
if ! plain_only "verify.sh / build-check.sh"; then :
elif verify_test tools/public-source/skel/verify.sh && buildcheck_test tools/refactor/build-check.sh; then
    echo "PASS  verify.sh: a malformed manifest exits 3 with a message; build-check.sh: no argument prints its usage"
else failures=$((failures + 1)); fi
mutate tools/public-source/skel/verify.sh "$ENTRY/verify-ctl.sh" 's/except KeyError as e:\n.*?sys\.exit\(3\)\n.*?sys\.exit\(3\)\n/except ZeroDivisionError:\n    pass\n/s'
mutate tools/refactor/build-check.sh "$ENTRY/buildcheck-ctl.sh" 's/\[ \$# -le 1 \] && \[ -f "\$g\/Makefile" \] \|\|/true ||/'
echo

# ---------------------------------------------------------------- helper processes: posix_spawn, no shell
d="$OUT/azspawn"; mkdir -p "$d"
cp guest/common/azspawn.h "$d/real.h"; cp guest/common/azaltstack.h "$d/"   # azspawn.h includes it
# control 1: LD_PRELOAD inherited (the child would load azmod/azstem itself)
mutate "$d/real.h" "$d/ctl-preload.h" 's/if \(strncmp\(environ\[i\], "LD_PRELOAD=", 11\) != 0\)/if (1)/; s/own\[k\+\+\] = \(char \*\) "LD_PRELOAD=";//'
# control 2: QUIET ignored (output reaches EP147's stdout/stderr)
mutate "$d/real.h" "$d/ctl-loud.h" 's/ok = posix_spawn_file_actions_addopen\(&fa, 1, "\/dev\/null", O_WRONLY, 0\) == 0 &&\n\s*posix_spawn_file_actions_adddup2\(&fa, 1, 2\) == 0;/ok = 1;/'
for v in real ctl-preload ctl-loud; do
    mkdir -p "$d/s-$v"
    $CC "${CFLAGS[@]}" -I$T -DAZSPAWN_INC="\"$d/$v.h\"" -o "$d/$v" $T/azspawn_test.c -lpthread
done
suite "azspawn" "$d/real" "$d/s-real"

# ---------------------------------------------------------------- drag handoff
# The real header, and copies with one protection removed. The threaded case also runs
# under ThreadSanitizer when the host compiler supports it: the publication-order control
# (the record written after `full` is released) is caught there as a data race.
d="$OUT/draghandoff"; mkdir -p "$d"
cp guest/azmod/drag_handoff.h "$d/real.h"
mutate "$d/real.h" "$d/ctl-nolatch.h" 's/if \(!dh_is_open\(seen\) \|\| !__atomic_compare_exchange_n\(&s->state, &seen, seen \| 1u, 0,\s*__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE\)\)/if (!dh_is_open(seen))/'
mutate "$d/real.h" "$d/ctl-noclear.h" 's/    __atomic_store_n\(&s->full, 0, __ATOMIC_RELEASE\);\n//'
mutate "$d/real.h" "$d/ctl-openfull.h" 's/    if \(__atomic_load_n\(&s->full, __ATOMIC_ACQUIRE\)\)\n        return 0;\n    unsigned st/    unsigned st/'
mutate "$d/real.h" "$d/ctl-nogen.h" 's/\(st \| 1u\) \+ 1u/0u/'
mutate "$d/real.h" "$d/ctl-localany.h" 's/return dh_is_open\(st\) && __atomic_compare_exchange_n\(&s->state, &st, st \| 1u, 0,\s*__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE\);/return 1;/'
mutate "$d/real.h" "$d/ctl-early.h" 's/    s->it = \*it;\n(    __atomic_add_fetch\(&s->posted, 1, __ATOMIC_RELAXED\);\n)(    __atomic_store_n\(&s->full, 1, __ATOMIC_RELEASE\);\n)/$1$2    s->it = *it;\n/'
for v in real ctl-nolatch ctl-noclear ctl-openfull ctl-nogen ctl-localany; do
    $CC "${CFLAGS[@]}" -I$T -DDH_HDR="\"$d/$v.h\"" -o "$d/$v" $T/draghandoff_test.c -lpthread
done
suite "drag handoff" "$d/real"
DH_TSAN=0
if [ -n "$SAN" ]; then
    echo "(sanitizer pass: the drag handoff ThreadSanitizer case runs in the plain pass; this pass built the suite itself with $SAN)"
elif AZ_CC_PROBE=1 $CC -std=gnu11 -O1 -g -fsanitize=thread -I$T -DDH_HDR="\"$d/real.h\"" -o "$d/tsan-real" $T/draghandoff_test.c -lpthread 2>/dev/null &&
   AZ_CC_PROBE=1 $CC -std=gnu11 -O1 -g -fsanitize=thread -I$T -DDH_HDR="\"$d/ctl-early.h\"" -o "$d/tsan-ctl-early" $T/draghandoff_test.c -lpthread 2>/dev/null; then
    DH_TSAN=1
    suite "drag handoff (ThreadSanitizer)" env TSAN_OPTIONS=halt_on_error=1 "$d/tsan-real" 50000
else
    skip "drag handoff (ThreadSanitizer): $CC has no -fsanitize=thread"; echo
fi
# busthread-lint: the real read(), and a copy with the old direct drag_cancel() call put back
cp guest/azmod/bus_interpose.c "$d/bi-real.c"
mutate "$d/bi-real.c" "$d/bi-ctl.c" 's/(back_swallow = 1; \/\* keep swallowing until RELEASE \*\/\n)/$1                    drag_cancel();\n/'
plain_only "busthread-lint" && suite "busthread-lint" bash $T/busthread-lint.sh "$d/bi-real.c"

# ---- community mod services: other programs, the library, audio taps, vtable slots ----
# aztap and the library build anywhere; sdk_proc (posix_spawn scheduling) and sdk_vslot (ELF
# program headers, mprotect of RELRO) are Linux behavior, so off Linux they build and run in the
# release build container, as fd does.
d="$OUT/aztap"; mkdir -p "$d"
cp guest/common/aztap.h "$d/real.h"
mutate "$d/real.h" "$d/ctl-overwrite.h" 's/    if \(n <= 0 \|\| \(unsigned\) n > AZTAP_FRAMES - \(w - r\)\) \{/    (void) r;\n    if (n <= 0) {/'
mutate "$d/real.h" "$d/ctl-nowant.h"    's/if \(!aztap_wanted\(q\) \|\| !L \|\| !R\)/if (!L || !R)/'
for v in real ctl-overwrite ctl-nowant; do
    $CC "${CFLAGS[@]}" -pthread -I$T -DTAP_HDR="\"$d/$v.h\"" -o "$d/$v" $T/aztap_test.c
done
suite "audio tap ring" "$d/real"

# Hide Track Title's tap rule: only a whole one-finger tap toggles a title
d="$OUT/htgest"; mkdir -p "$d"
cp guest/azmod/ht_tap.h "$d/real.h"; cp guest/azmod/touch_seq.h "$d/"
mutate "$d/real.h" "$d/ctl-press.h" 's/(        g->t = ms;\n)        return 0;/$1        *tx = x;\n        *ty = y;\n        return 1;/'
mutate "$d/real.h" "$d/ctl-slop.h"  's/    if \(dx < -HT_TAP_SLOP \|\| dx > HT_TAP_SLOP \|\| dy < -HT_TAP_SLOP \|\| dy > HT_TAP_SLOP\)\n        return 0;\n//'
mutate "$d/real.h" "$d/ctl-hold.h"  's/ \|\| ms - g->t > HT_TAP_MAX_MS//'
mutate "$d/real.h" "$d/ctl-multi.h" 's/if \(g->moved \|\| g->multi \|\| /if (g->moved || /'
mutate "$d/real.h" "$d/ctl-held.h"  's/g->multi = g->held != 0;/g->multi = 0;/'
mutate "$d/real.h" "$d/ctl-repeat.h" 's/== TOUCH_MOVE\) \{/== -1) {/'
for v in real ctl-press ctl-slop ctl-hold ctl-multi ctl-held ctl-repeat; do
    $CC "${CFLAGS[@]}" -I$T -DHTTAP_HDR="\"$d/$v.h\"" -o "$d/$v" $T/htgest_test.c
done
suite "hide title tap" "$d/real"

# The Clock's countdown Timer: what the Clock's place shows, the popup's buttons and layout
d="$OUT/clocktimer"; mkdir -p "$d"
cp guest/azmod/clock_timer.h "$d/real.h"
mutate "$d/real.h" "$d/ctl-rounddown.h" 's/v\.secs = \(int\) \(\(rem \+ 999\) \/ 1000\);/v.secs = (int) (rem \/ 1000);/'
mutate "$d/real.h" "$d/ctl-steady.h"    's/v\.red = rem <= CT_RED_MS && \(rem - 1\) % 1000 >= 1000 - CT_FLASH_MS;/v.red = rem <= CT_RED_MS;/'
mutate "$d/real.h" "$d/ctl-nodone.h"    's/    if \(past < CT_DONE_MS\) \{/    if (0) {/'
mutate "$d/real.h" "$d/ctl-closeruns.h" 's/    case CT_B_CLOSE:\n        p->open = 0;/    case CT_B_CLOSE:\n        if (!p->running)\n            ct_start(t, now_ms, p->draft_s);\n        p->open = 0;/'
mutate "$d/real.h" "$d/ctl-adddraft.h"  's/        if \(p->running\)\n            return ct_extend\(t, now_ms, add\);\n//'
mutate "$d/real.h" "$d/ctl-nocap.h"     's/p->draft_s = p->draft_s \+ add > CT_MAX_S \? CT_MAX_S : p->draft_s \+ add;/p->draft_s += add;/'
mutate "$d/real.h" "$d/ctl-edge.h"      's/x < r->x \+ r->w && y >= r->y && y < r->y \+ r->h/x <= r->x + r->w \&\& y >= r->y \&\& y <= r->y + r->h/'
mutate "$d/real.h" "$d/ctl-plain.h"     's/    __atomic_store_n\(&t->end_ms, now_ms \+ \(int64_t\) secs \* 1000, __ATOMIC_RELEASE\);/    t->end_ms = now_ms + (int64_t) secs * 1000;/'
for v in real ctl-rounddown ctl-steady ctl-nodone ctl-closeruns ctl-adddraft ctl-nocap ctl-edge; do
    $CC "${CFLAGS[@]}" -I$T -DCT_HDR="\"$d/$v.h\"" -o "$d/$v" $T/clocktimer_test.c -lpthread
done
suite "clock timer" "$d/real" 20000
CT_TSAN=0
if [ -n "$SAN" ]; then
    echo "(sanitizer pass: the clock timer ThreadSanitizer control runs in the plain pass; this pass built the suite itself with $SAN)"
elif AZ_CC_PROBE=1 $CC -std=gnu11 -O1 -g -fsanitize=thread -I$T -DCT_HDR="\"$d/real.h\"" -o "$d/tsan-real" $T/clocktimer_test.c -lpthread 2>/dev/null &&
   AZ_CC_PROBE=1 $CC -std=gnu11 -O1 -g -fsanitize=thread -I$T -DCT_HDR="\"$d/ctl-plain.h\"" -o "$d/tsan-ctl-plain" $T/clocktimer_test.c -lpthread 2>/dev/null; then
    CT_TSAN=1
    suite "clock timer (ThreadSanitizer)" env TSAN_OPTIONS=halt_on_error=1 "$d/tsan-real" 200000
else
    skip "clock timer (ThreadSanitizer): $CC has no -fsanitize=thread"; echo
fi

# Remember Font Size: what is saved, and which FontManager gets the saved size
d="$OUT/fontsize"; mkdir -p "$d"
cp guest/azmod/font_size.c "$d/real.c"
mutate "$d/real.c" "$d/ctl-anyctor.c"   's/if \(!FS_REG_RET \|\| ret != FS_REG_RET\)/if (!FS_REG_RET)/'
mutate "$d/real.c" "$d/ctl-badsize.c"   's/if \(size < 0 \|\| size >= FS_N \|\| !fs_on\(\)\)/if (!fs_on())/'
mutate "$d/real.c" "$d/ctl-range.c"     's/return n >= 0 && n < FS_N \? n : -1;/return n;/'
mutate "$d/real.c" "$d/ctl-rewrite.c"   's/if \(__atomic_exchange_n\(&g_fs_saved, size, __ATOMIC_ACQ_REL\) == size\)\n\s*return;/__atomic_store_n(\&g_fs_saved, size, __ATOMIC_RELEASE);/'
mutate "$d/real.c" "$d/ctl-rowoff.c"    's/int v = fs_on\(\) \? fs_load_file\(\) : -1;/int v = fs_load_file();/'
mutate "$d/real.c" "$d/ctl-anyfm.c"     's/own_q\(fm, &vt\) && vt == FS_VT \? fm : 0;/own_q(fm, \&vt) ? fm : 0;/'
mutate "$d/real.c" "$d/ctl-anybar.c"    's/ \|\| !own_q\(\(uintptr_t\) bar, &vt\) \|\| vt != FS_BAR_VT\)/)/'
for v in real ctl-anyctor ctl-badsize ctl-range ctl-rewrite ctl-rowoff ctl-anyfm ctl-anybar; do
    $CC "${CFLAGS[@]}" -pthread -I$T -DFS_SRC="\"$d/$v.c\"" -o "$d/$v" $T/fontsize_test.c
done
suite "remember font size" "$d/real"

# Streaming keys in Camelot: the notation EP147's key text function is asked for
d="$OUT/skey"; mkdir -p "$d"
cp guest/azmod/streaming_key.c "$d/real.c"
mutate "$d/real.c" "$d/ctl-noforce.c" 's/        return 1;\n    return flag & 0xff;/        return flag & 0xff;\n    return flag & 0xff;/'
mutate "$d/real.c" "$d/ctl-range.c"   's/ && key && \*key >= 1 && \*key <= SK_IDS\)/ \&\& key)/'
mutate "$d/real.c" "$d/ctl-x8.c"      's/ldr x8, \[sp, #80\]/add x8, sp, #16/'
mutate "$d/real.c" "$d/ctl-nokill.c"  's/if \(access\(SK_OFF_PATH, F_OK\) == 0\)/if (0)/'
mutate "$d/real.c" "$d/ctl-norow.c"   's/if \(az_value\(AZF_CAMELOT_KEYS\) && key && /if (key \&\& /'
mutate "$d/real.c" "$d/ctl-retest.c"  's/__ATOMIC_ACQ_REL\) == row\)/__ATOMIC_ACQ_REL) != -1)/'
for v in real ctl-noforce ctl-range ctl-x8 ctl-nokill ctl-norow ctl-retest; do
    $CC "${CFLAGS[@]}" -pthread -I$T -DSK_SRC="\"$d/$v.c\"" -o "$d/$v" $T/skey_test.c
done
suite "streaming key" "$d/real"

# Camelot Keys on the deck screen: EP147's Camelot rule for a pitch class and a major/minor flag
d="$OUT/camelot"; mkdir -p "$d"
cp guest/azmod/camelot_text.h "$d/real.h"
mutate "$d/real.h" "$d/ctl-default.h" 's/int v = i < 11 \? num\[i\] : 8, n = 0;/int v = i < 11 ? num[i] : 1, n = 0;/'
mutate "$d/real.h" "$d/ctl-suffix.h"  's/out\[n\+\+\] = major \? .B. : .A.;/out[n++] = major ? '"'"'A'"'"' : '"'"'B'"'"';/'
mutate "$d/real.h" "$d/ctl-index.h"   's/unsigned i = \(unsigned\) \(pitch - 1\) & 0xffu;/unsigned i = (unsigned) pitch \& 0xffu;/'
for v in real ctl-default ctl-suffix ctl-index; do
    $CC "${CFLAGS[@]}" -I$T -DCAMELOT_HDR="\"$d/$v.h\"" -o "$d/$v" $T/camelot_test.c
done
suite "camelot text" "$d/real"

# Preview Hot Cues: a track's hot cues from its analysis file
d="$OUT/hcanlz"; mkdir -p "$d"
cp guest/azmod/hc_anlz.h "$d/real.h"
mutate "$d/real.h" "$d/ctl-nopco2.h"   's/    if \(have_co2\)\n        return nco2;\n//'
mutate "$d/real.h" "$d/ctl-seclen.h"   's/if \(len < 12 \|\| len > n - o \|\| /if (len < 12 || /'
mutate "$d/real.h" "$d/ctl-entrylen.h" 's/if \(en > end - e \|\| /if (/'
mutate "$d/real.h" "$d/ctl-comment.h"  's/const uint8_t \*c = b \+ e \+ 0x2c \+ lc;/const uint8_t *c = b + e + 0x2c;/'
mutate "$d/real.h" "$d/ctl-pastend.h"  's/if \(w <= 0 \|\| !len_ms \|\| ms >= len_ms\)/if (w <= 0 || !len_ms)/'
mutate "$d/real.h" "$d/ctl-pickco2.h"  's/\(t\[3\] == .2. \|\| t\[3\] == .B.\)/t[3] == '"'"'B'"'"'/'
for v in real ctl-nopco2 ctl-seclen ctl-entrylen ctl-comment ctl-pastend ctl-pickco2; do
    $CC "${CFLAGS[@]}" -I$T -DHCANLZ_HDR="\"$d/$v.h\"" -o "$d/$v" $T/hcanlz_test.c
done
suite "hot cues from analysis files" "$d/real"

# The stem marks worker's order: track rows' stems, then the hot cues of the rows on screen,
# then playlist rows (stem_marks.c sm_pop / sm_serve_tracks / sm_work_one, preview_hotcues.c)
d="$OUT/smorder"; mkdir -p "$d"
{ extract guest/azmod/stem_marks.c '^static int sm_pop\(unsigned'
  extract guest/azmod/stem_marks.c '^static void sm_serve_tracks\(.*\{$'
  extract guest/azmod/stem_marks.c '^static int sm_work_one\(.*\{$'; } > "$d/sm.inc"
{ extract guest/azmod/preview_hotcues.c '^static struct ph_ent \*ph_ent\(.*\{'
  extract guest/azmod/preview_hotcues.c '^static int ph_get\(.*\{$'
  extract guest/azmod/preview_hotcues.c '^static int ph_queued\(.*\{'
  extract guest/azmod/preview_hotcues.c '^static int ph_serve_one\(.*\{$'; } > "$d/ph.inc"
# each function's definition, once (a forward declaration must not start an extract)
grep -q '^static int sm_pop(unsigned' "$d/sm.inc" || { echo "extraction of sm_pop failed"; exit 2; }
for f in 'void sm_serve_tracks' 'int sm_work_one'; do
    [ "$(grep -c "^static $f(.*{" "$d/sm.inc")" = 1 ] || { echo "extraction of $f failed"; exit 2; }
done
for f in 'struct ph_ent \*ph_ent' 'int ph_get' 'int ph_queued' 'int ph_serve_one'; do
    [ "$(grep -c "^static $f(.*{" "$d/ph.inc")" = 1 ] || { echo "extraction of $f failed"; exit 2; }
done
mutate "$d/sm.inc" "$d/ctl-phlast.inc" 's/int got = g_smq_n && sm_pop\(&cid, &h, &media, title, sizeof title, 1\);/int got = g_smq_n \&\& sm_pop(\&cid, \&h, \&media, title, sizeof title, 0);/'
mutate "$d/sm.inc" "$d/ctl-stepnohc.inc" 's/        if \(!ph_serve_one\(lg\)\)\n            return;/        return;/'
mutate "$d/ph.inc" "$d/ctl-fifo.inc" 's/int k = g_phq_n - 1;\n(.*\n)*?        g_phq_n--;/int k = 0;\n        cid = g_phq[k].cid;\n        h = g_phq[k].h;\n        at = g_phq[k].at;\n        qn = g_phq_n;\n        sqn = g_smq_n;\n        snprintf(title, sizeof title, "%s", g_phq[k].title);\n        memmove(\&g_phq[0], \&g_phq[1], (size_t) (g_phq_n - 1) * sizeof g_phq[0]);\n        g_phq_n--;/'
mutate "$d/ph.inc" "$d/ctl-noevict.inc" 's/if \(ask && g_phq_n == PH_Q\) \{/if (0) {/; s/    if \(ask\) \{/    if (ask \&\& g_phq_n < PH_Q) {/'
$CC "${CFLAGS[@]}" -I$T -DSM_SRC="\"$d/sm.inc\"" -DPH_SRC="\"$d/ph.inc\"" -o "$d/real" $T/smorder_test.c -lpthread
$CC "${CFLAGS[@]}" -I$T -DSM_SRC="\"$d/ctl-phlast.inc\"" -DPH_SRC="\"$d/ph.inc\"" -o "$d/ctl-phlast" $T/smorder_test.c -lpthread
$CC "${CFLAGS[@]}" -I$T -DSM_SRC="\"$d/ctl-stepnohc.inc\"" -DPH_SRC="\"$d/ph.inc\"" -o "$d/ctl-stepnohc" $T/smorder_test.c -lpthread
$CC "${CFLAGS[@]}" -I$T -DSM_SRC="\"$d/sm.inc\"" -DPH_SRC="\"$d/ctl-fifo.inc\"" -o "$d/ctl-fifo" $T/smorder_test.c -lpthread
$CC "${CFLAGS[@]}" -I$T -DSM_SRC="\"$d/sm.inc\"" -DPH_SRC="\"$d/ctl-noevict.inc\"" -o "$d/ctl-noevict" $T/smorder_test.c -lpthread
suite "stem marks worker order" "$d/real"

# Extended Lead-In: which pre-roll the jog clamp sees, and on what
d="$OUT/spinback"; mkdir -p "$d"
cp guest/azmod/spin_back.c "$d/real.c"
mutate "$d/real.c" "$d/ctl-anyvalue.c" 's/    if \(cur != SB_STOCK_MS && cur != SB_CAP_MS\)\n        return cur;\n//'
mutate "$d/real.c" "$d/ctl-anycore.c"  's/ \|\| vt != SB_CORE_VT\)/)/'
mutate "$d/real.c" "$d/ctl-after.c"    's/    sb_apply\(strategy\);\n(    g_o_sb_clamp\(strategy, a1, pos, readable, flag\);\n)/$1    sb_apply(strategy);\n/'
mutate "$d/real.c" "$d/ctl-nooff.c"    's/return on \? SB_CAP_MS : SB_STOCK_MS;/return on ? SB_CAP_MS : cur;/'
mutate "$d/real.c" "$d/ctl-noalign.c"  's/\(core & 7\) \|\| //'
for v in real ctl-anyvalue ctl-anycore ctl-after ctl-nooff ctl-noalign; do
    $CC "${CFLAGS[@]}" -pthread -Iguest/azmod -I$T -DSB_SRC="\"$d/$v.c\"" -o "$d/$v" $T/spinback_test.c
done
suite "extended lead-in" "$d/real"

# The BEAT LOOP page flip: azmod's flip only on a deck with stems engaged (bl_flip.h)
d="$OUT/blflip"; mkdir -p "$d"
extract guest/azmod/bus_interpose.c '^static void erp_beat_loop_page\(' > "$d/real.inc"
cp guest/azmod/bl_flip.h "$d/bl_flip.h"
mutate "$d/real.inc" "$d/ctl-anydeck.inc" 's/__atomic_load_n\(&g_erp_active\[ld\], __ATOMIC_RELAXED\)/1/'
mutate "$d/real.inc" "$d/ctl-sidedeck.inc" 's/&g_erp_active\[ld\]/&g_erp_active[d]/'
mkdir -p "$d/anymode"; cp "$d/real.inc" "$d/anymode/real.inc"
mutate guest/azmod/bl_flip.h "$d/anymode/bl_flip.h" 's/ && fresh_mode == 3 \+ 1;/ \&\& fresh_mode != 0;/'
for v in real ctl-anydeck ctl-sidedeck; do
    $CC "${CFLAGS[@]}" -I$T -I"$d" -DBLF_SRC="\"$d/$v.inc\"" -o "$d/$v" $T/blflip_test.c
done
$CC "${CFLAGS[@]}" -I$T -I"$d/anymode" -DBLF_SRC="\"$d/anymode/real.inc\"" -o "$d/ctl-anymode" $T/blflip_test.c
suite "beat loop flip" "$d/real"

# Stem FX on pads 5-8: the sound and the press state (stem_fx.h), and the pads' tap / hold logic
# (stem_fx_pads.h). The ThreadSanitizer build runs one writer pressing while two readers render.
d="$OUT/stemfx"; mkdir -p "$d"
cp guest/azstem/stem_fx.h "$d/real.h"
mutate "$d/real.h" "$d/ctl-posttaps.h" 's/long long m = \(i - o->on\) \/ D \+ 1;/long long m = 1;/'
mutate "$d/real.h" "$d/ctl-noseq.h" 's/        if \(a & 1u\)\n            continue;\n        o->kind/        o->kind/'
mutate "$d/real.h" "$d/ctl-notag.h" 's/return \(\(uint64_t\) idx << 16\) \| \(tag & 0xffffu\);/return (uint64_t) idx << 16;/'
mutate "$d/real.h" "$d/ctl-nooff.h" 's/        w \*= 1.f - \(float\) v \/ \(float\) SFX_XF;/        w *= 1.f;/'
mutate "$d/real.h" "$d/ctl-plain.h" 's/o->kind = atomic_load_explicit\(&s->kind, memory_order_relaxed\);/o->kind = *(int *) &s->kind;/; s/atomic_store_explicit\(&s->kind, kind, memory_order_relaxed\);/*(int *) &s->kind = kind;/'
for v in real ctl-posttaps ctl-noseq ctl-notag ctl-nooff; do
    $CC "${CFLAGS[@]}" -I$T -DSFX_INC="\"$d/$v.h\"" -o "$d/$v" $T/stemfx_test.c -lpthread -lm
done
suite "stem fx" "$d/real"
SFX_TSAN=0
if [ -n "$SAN" ]; then
    echo "(sanitizer pass: the ThreadSanitizer case runs in the plain pass; this pass built the suite itself with $SAN)"
elif AZ_CC_PROBE=1 $CC -std=gnu11 -O1 -g -fsanitize=thread -I$T -DSFX_INC="\"$d/real.h\"" -o "$d/tsan-real" $T/stemfx_test.c -lpthread -lm 2>/dev/null &&
   AZ_CC_PROBE=1 $CC -std=gnu11 -O1 -g -fsanitize=thread -I$T -DSFX_INC="\"$d/ctl-plain.h\"" -o "$d/tsan-ctl-plain" $T/stemfx_test.c -lpthread -lm 2>/dev/null; then
    SFX_TSAN=1
    suite "stem fx (ThreadSanitizer)" env TSAN_OPTIONS=halt_on_error=1 "$d/tsan-real"
else
    skip "stem fx (ThreadSanitizer): $CC has no -fsanitize=thread"; echo
fi
d="$OUT/sfxpads"; mkdir -p "$d"
cp guest/azmod/stem_fx_pads.h "$d/real.h"
mutate "$d/real.h" "$d/ctl-holdflip.h" 's/now - h\[f\].t0 >= SFXP_HOLD_MS/now - h[f].t0 < SFXP_HOLD_MS/'
mutate "$d/real.h" "$d/ctl-curdeck.h" 's/fx\(h\[f\].ld, f, 0\); \/\* held/fx(ld, f, 0); \/* held/'
mutate "$d/real.h" "$d/ctl-offrelease.h" 's/            fx\(ld, f, 0\); \/\* a press while on turns it off; its release does nothing \*\/\n            continue;/            fx(ld, f, 0);\n            h[f].ld = ld;\n            h[f].t0 = now;\n            continue;/'
mutate "$d/real.h" "$d/ctl-refused.h" 's/        if \(fx\(ld, f, 1\)\) \{/        fx(ld, f, 1);\n        {/'
for v in real ctl-holdflip ctl-curdeck ctl-offrelease ctl-refused; do
    $CC "${CFLAGS[@]}" -I$T -DSFXP_INC="\"$d/$v.h\"" -o "$d/$v" $T/sfxpads_test.c
done
suite "stem fx pads" "$d/real"

# Column Edit's columns, and the Browse PREVIEW button and sub column remembered (browse_columns.c);
# the stem marks' column from the list's header (stem_marks.c sm_hdr_lastcol)
d="$OUT/bcols"; mkdir -p "$d"
cp guest/azmod/browse_columns.c "$d/real.c"
extract guest/azmod/stem_marks.c '^static int sm_hdr_lastcol\(' > "$d/sm.inc"
grep -q 'ep_off(7, 8)' "$d/sm.inc" || { echo "extraction of sm_hdr_lastcol failed"; exit 2; }
mutate "$d/real.c" "$d/ctl-compact.c" 's/return \(int\) \(m >> \(8 \* \(BC_HDR_MODE & 7\)\)\) == 0 \? h : 0;/return h;/'
mutate "$d/real.c" "$d/ctl-all.c"     's/bc_set_cols\(h, g_bc_cols, AZMOD_BCOL_SET_OF\(g_bc_cols\)\);/bc_set_cols(h, g_bc_cols, AZMOD_BCOL_ALL);/'
mutate "$d/real.c" "$d/ctl-unset.c"   's/bc_set_cols\(h, g_bc_cols, AZMOD_BCOL_SET_OF\(g_bc_cols\)\);/bc_set_cols(h, g_bc_cols, (g_bc_cols ^ AZMOD_BCOL_DEFAULT) & AZMOD_BCOL_ALL);/'
mutate "$d/real.c" "$d/ctl-ungated.c" 's/        if \(bc_ce_on\(\)\)\n            bc_set_cols/        if (1)\n            bc_set_cols/'
mutate "$d/real.c" "$d/ctl-noback.c"  's/    if \(!want\)\n        for/    if (0)\n        for/'
mutate "$d/real.c" "$d/ctl-recself.c" 's/        g_bc_setvis = g_o_bc_setvis;/        g_bc_setvis = bc_setvis_hook;/'
mutate "$d/real.c" "$d/ctl-gone.c"    's/    if \(i >= 0\)\n        memset\(&g_bc_ep\[i\], 0, sizeof g_bc_ep\[i\]\);/    (void) i;/'
mutate "$d/real.c" "$d/ctl-early.c"   's/\} else if \(__atomic_load_n\(&g_sc_tries, __ATOMIC_ACQUIRE\) > 0\) \{/} else {/'
mutate "$d/real.c" "$d/ctl-pvany.c"   's/if \(bc_killed\(BC_PV_OFF, &g_pv_off\) \|\| !bc_obj\(\(uintptr_t\) fb, BC_FB_VT\)\)\n        return;\n    on =/if (bc_killed(BC_PV_OFF, &g_pv_off))\n        return;\n    on =/'
mutate "$d/real.c" "$d/ctl-afon.c"    's/    return bc_allfields\(\) \? 1 : v;/    return 1 ? 1 : v;/'
mutate "$d/real.c" "$d/ctl-afseq.c"   's/    if \(!bc_allfields\(\) \|\| g_o_sort_vis\(row\)\)\n        return seq;/    return seq;/'
mutate "$d/real.c" "$d/ctl-ids.c"     's/case AZMOD_BCOL_BPM: return ep_off\(6, 7\);/case AZMOD_BCOL_BPM: return 6;/'
mutate "$d/sm.inc" "$d/ctl-smasc.inc" 's/for \(int id = ep_off\(7, 8\); id >= 1; id--\)/for (int id = 1; id <= ep_off(7, 8); id++)/'
for v in real ctl-compact ctl-all ctl-unset ctl-ungated ctl-noback ctl-recself ctl-gone ctl-early ctl-pvany ctl-afon ctl-afseq ctl-ids; do
    $CC "${CFLAGS[@]}" -I$T -DBC_SRC="\"$d/$v.c\"" -DSM_INC="\"$d/sm.inc\"" -o "$d/$v" $T/bcols_test.c
done
$CC "${CFLAGS[@]}" -I$T -DBC_SRC="\"$d/real.c\"" -DSM_INC="\"$d/ctl-smasc.inc\"" -o "$d/ctl-smasc" $T/bcols_test.c
suite "browse columns" "$d/real"

# Column Edit, any field (browse_fields.c, browse_fields.h): the fields and their text, the saved
# file, the columns that can show one, a pick, the buttons, a cell, the Preview component, and the
# worker on its own thread over fake sticks. Each variant is a copy of browse_fields.c with
# browse_fields.h beside it (the .c includes the header from its own directory).
d="$OUT/bfields"; mkdir -p "$d/real"
cp guest/azmod/browse_fields.c guest/azmod/browse_fields.h "$d/real/"
extract guest/azmod/browse_columns.c '^static int bc_col_id\(' > "$d/bcid.inc"
grep -q 'AZMOD_BCOL_KEY' "$d/bcid.inc" || { echo "extraction of bc_col_id failed"; exit 2; }
awk '/^#define AZMOD_BCOL_/ { on = 1 } on { print } on && !/\\\r?$/ { on = 0 }' guest/azmod/browse_columns.c | tr -d '\r' > "$d/bcol.inc"
grep -q 'AZMOD_BCOL_KEY' "$d/bcol.inc" || { echo "extraction of the AZMOD_BCOL definitions failed"; exit 2; }
BF_C="ctl-track ctl-stale ctl-ownclick ctl-restore ctl-compact ctl-sel1 ctl-load ctl-wave ctl-disagree ctl-inset
      ctl-narrow ctl-cover ctl-left0 ctl-ownfwd ctl-still ctl-stack ctl-norestore ctl-popleft ctl-poprow ctl-preview
      ctl-evcomp ctl-evcol ctl-tapmove ctl-dragtap ctl-pause ctl-owngate ctl-tlfull ctl-tlparent ctl-tlclamp ctl-look ctl-negx
      ctl-ungated ctl-hidekeep ctl-hideany ctl-hidepass ctl-unhide ctl-hidestale ctl-hideott ctl-hidelate"
BF_H="ctl-dup ctl-overlong"
for v in $BF_C $BF_H; do mkdir -p "$d/$v"; cp "$d/real/browse_fields.c" "$d/real/browse_fields.h" "$d/$v/"; done
mutate "$d/real/browse_fields.h" "$d/ctl-dup/browse_fields.h"      's/ \|\| field\[id\] != B2_NONE\)/)/'
mutate "$d/real/browse_fields.h" "$d/ctl-overlong/browse_fields.h" 's/if \(cp < min \|\| /if (/'
mutate "$d/real/browse_fields.c" "$d/ctl-track/browse_fields.c"    's/\(unsigned char\) \(f == B2_TRACK \? B2_NONE : f\)/(unsigned char) f/'
mutate "$d/real/browse_fields.c" "$d/ctl-stale/browse_fields.c"    's/ \|\| drag_now_ms\(\) - g_bf_ask_ms > B2_ASK_MS\)/)/'
mutate "$d/real/browse_fields.c" "$d/ctl-ownclick/browse_fields.c" 's/        __atomic_store_n\(&g_bf_ask, 0, __ATOMIC_RELEASE\);\n        b2_hide_row\(wref_get/        b2_hide_row(wref_get/'
mutate "$d/real/browse_fields.c" "$d/ctl-restore/browse_fields.c"  's/ __attribute__\(\(cleanup\(b2_restore\)\)\)//; s/    b2_restore\(&sw\);\n    return 1;/    return 1;/'
mutate "$d/real/browse_fields.c" "$d/ctl-compact/browse_fields.c"  's/ \|\| !b2_full_list\(self\)\)/)/'
mutate "$d/real/browse_fields.c" "$d/ctl-sel1/browse_fields.c"     's/    if \(col != 1\) \{\n        g_b2_sel_model/    if (1) {\n        g_b2_sel_model/'
mutate "$d/real/browse_fields.c" "$d/ctl-load/browse_fields.c"     's/if \(rc > 0 && bf_swappable\(id\)\)/if (rc > 0)/'
mutate "$d/real/browse_fields.c" "$d/ctl-wave/browse_fields.c"     's/if \(bf_field_of\(1\) == B2_NONE \|\| !on_msg_thread\(\)/if (!on_msg_thread()/'
mutate "$d/real/browse_fields.c" "$d/ctl-disagree/browse_fields.c" 's/\} else if \(strcmp\(got, v\) \|\| gk != k\) \{/} else if (0) {/'
mutate "$d/real/browse_fields.c" "$d/ctl-inset/browse_fields.c"    's/\? x\[r\] \+ w\[r\] - zone - B2_BTN_W :/? x[r] + w[r] - B2_BTN_W :/'
mutate "$d/real/browse_fields.c" "$d/ctl-narrow/browse_fields.c"   's/: x\[r\] \+ w\[r\] - B2_BTN_W;/: -1;\n        if (b < 0)\n            continue;/'
mutate "$d/real/browse_fields.c" "$d/ctl-cover/browse_fields.c"    's/        if \(b \+ B2_BTN_W > limit\)\n            b = limit - B2_BTN_W;\n//'
mutate "$d/real/browse_fields.c" "$d/ctl-left0/browse_fields.c"    's/        if \(b < 0\)\n            b = 0;\n        bx\[r\] = b;/        bx[r] = b;/'
mutate "$d/real/browse_fields.c" "$d/ctl-ownfwd/browse_fields.c"   's/    if \(!h \|\| !b2_hdr_xy\(\(uintptr_t\) btn, e, &x, &y\)\) \{\n        g_o_b2_btn_mouse\[0\]/    if (!b2_hdr_xy((uintptr_t) btn, e, \&x, \&y)) {\n        g_o_b2_btn_mouse[0]/'
mutate "$d/real/browse_fields.c" "$d/ctl-tapmove/browse_fields.c"  's/    if \(!tap \|\| dragged\)\n        return;/    if (dragged)\n        return;/'
mutate "$d/real/browse_fields.c" "$d/ctl-dragtap/browse_fields.c"  's/    if \(!tap \|\| dragged\)\n        return;/    if (!tap)\n        return;/'
mutate "$d/real/browse_fields.c" "$d/ctl-owngate/browse_fields.c"  's/if \(be_editing\(\) && own_q/if (own_q/'
mutate "$d/real/browse_fields.c" "$d/ctl-tlfull/browse_fields.c"   's/                drag_is_widget\(w\) \|\| !b2_full_list\(w\)\)\n                return 0;\n            \*row/                drag_is_widget(w))\n                return 0;\n            *row/'
mutate "$d/real/browse_fields.c" "$d/ctl-tlparent/browse_fields.c" 's/        par != g_b2_tl_sub\.label \|\| /        0 || /'
mutate "$d/real/browse_fields.c" "$d/ctl-tlclamp/browse_fields.c"  's/            w\[i\] = ob\[0\] - x\[i\];/            (void) 0;/'
mutate "$d/real/browse_fields.c" "$d/ctl-look/browse_fields.c"     's/vt == g_b2_btn_vt && img_q\(vt \+ 4 \* 8, &fn\) && fn\)/vt == g_b2_btn_vt \&\& 0)/'
mutate "$d/real/browse_fields.c" "$d/ctl-pause/browse_fields.c"    's/long ms = ev == 1 && g_b2_gest\.down \? g_b2_gest\.seq\.last : now_ms\(\);/long ms = now_ms();/'
mutate "$d/real/browse_fields.c" "$d/ctl-still/browse_fields.c"    's/    b2_lis_click\(lis, \(void \*\) B2_BTN_NAME\);/    (void) lis;/'
mutate "$d/real/browse_fields.c" "$d/ctl-stack/browse_fields.c"    's/memcpy\(pb, g_b2_pop_orig, sizeof pb\); \/\* moved before/b2_bounds(pop, pb); \/* moved before/'
mutate "$d/real/browse_fields.c" "$d/ctl-norestore/browse_fields.c" 's/ \/\* and its rows as EP147 made them \*\/\n        b2_pop_restore\(\);/ \/* and its rows as EP147 made them *\//'
mutate "$d/real/browse_fields.c" "$d/ctl-popleft/browse_fields.c"  's/    if \(x \+ dx < left\)\n        dx = left - x;\n//'
mutate "$d/real/browse_fields.c" "$d/ctl-poprow/browse_fields.c"   's/\(lb, g_b2_pop\.row, 1, 1\)/(lb, 0, 1, 1)/'
mutate "$d/real/browse_fields.c" "$d/ctl-preview/browse_fields.c"  's/    if \(!row\) \{\n        g_o_b2_wave_mouse\[k\]\(self, e\);\n        return;\n    \}/    g_o_b2_wave_mouse[k](self, e);\n    if (!row)\n        return;/'
mutate "$d/real/browse_fields.c" "$d/ctl-evcomp/browse_fields.c"   's/    memcpy\(b \+ 0x28, &row, sizeof row\);\n//'
mutate "$d/real/browse_fields.c" "$d/ctl-evcol/browse_fields.c"    's/cb\[0\] \+ cb\[2\] - b2_me_x\(e\) \+ 1/cb[0] - b2_me_x(e) + 1/'
mutate "$d/real/browse_fields.c" "$d/ctl-ungated/browse_fields.c"  's/    if \(!g_b2_on \|\| !bc_ce_on\(\) \|\| id < 1/    if (!g_b2_on || id < 1/'
mutate "$d/real/browse_fields.c" "$d/ctl-hidekeep/browse_fields.c" 's/    if \(have\) \{ \/\* the last label goes/    if (0) { \/* the last label goes/'
mutate "$d/real/browse_fields.c" "$d/ctl-hideany/browse_fields.c"  's/want = g_b2_hide_ok && col && b2_col_bit\(col\) != 0;/want = g_b2_hide_ok \&\& col;/'
mutate "$d/real/browse_fields.c" "$d/ctl-hidepass/browse_fields.c" 's/        g_o_b2_pop_click\(model, -1\);/        g_o_b2_pop_click(model, row);/'
mutate "$d/real/browse_fields.c" "$d/ctl-unhide/browse_fields.c"   's/    if \(b2_col_hidden\(col\)\) \/\* a field picked for a hidden column: shown again \*\/\n        bc_set_mask[^\n]*\n//'
mutate "$d/real/browse_fields.c" "$d/ctl-hidestale/browse_fields.c" 's/ && own_str\(t, tok, sizeof tok\) && !strcmp\(tok, B2_HIDE_LABEL\);/;/'
mutate "$d/real/browse_fields.c" "$d/ctl-hideott/browse_fields.c"  's/    uintptr_t obj = c \+ \(uintptr_t\) off;/    uintptr_t obj = c;/'
mutate "$d/real/browse_fields.c" "$d/ctl-hidelate/browse_fields.c" 's/        b2_hide_row\(pc, self->col\);\n//'
mutate "$d/real/browse_fields.c" "$d/ctl-negx/browse_fields.c"     's/if \(!own_q\(\(uintptr_t\) e \+ 8, &q\) \|\| !b2_bounds/if (!own_q((uintptr_t) e + 8, \&q) || (int32_t) q < 0 || !b2_bounds/'
for v in real $BF_C $BF_H; do
    $CC "${CFLAGS[@]}" -pthread -I$T -Iguest/azmod -DBF_SRC="\"$d/$v/browse_fields.c\"" -DBCID_INC="\"$d/bcid.inc\"" \
        -DBCOL_INC="\"$d/bcol.inc\"" -o "$d/$v/t" $T/bfields_test.c -ldl
done
suite "browse fields" "$d/real/t"

# Hide Track Title: a press counts for a deck's bar only when JUCE's hit test gives it to that
# deck's widget, not to a popup drawn over it
d="$OUT/htcover"; mkdir -p "$d"
cp guest/azmod/ht_cover.h "$d/real.h"
mutate "$d/real.h" "$d/ctl-any.h"    's/        if \(hit == owner\)\n            return 1;/        if (hit)\n            return 1;/'
mutate "$d/real.h" "$d/ctl-direct.h" 's/i\+\+, hit = parent\(hit\)\)/i++, hit = 0)/'
mutate "$d/real.h" "$d/ctl-every.h"  's/            m \|= 1u << d;/            m = (1u << HT_COVER_DECKS) - 1;/'
for v in real ctl-any ctl-direct ctl-every; do
    $CC "${CFLAGS[@]}" -I$T -DHTCOVER_HDR="\"$d/$v.h\"" -o "$d/$v" $T/htcover_test.c
done
suite "hide title cover" "$d/real"

# Key in Track Title: the key's box in the title row, the copy of the tray's key, its freshness,
# and the shared paint hook's gate split (guest/azmod/kt_layout.h)
d="$OUT/ktlayout"; mkdir -p "$d"
cp guest/azmod/kt_layout.h "$d/real.h"
mutate "$d/real.h" "$d/ctl-noeye.h"   's/int right = eye_left >= 0 \? eye_left - KT_GAP : row_right - pad_right;/int right = row_right - pad_right;/'
mutate "$d/real.h" "$d/ctl-under.h"   's/    r->x = title_right;/    r->x = title_right - 40;/'
mutate "$d/real.h" "$d/ctl-hidden.h"  's/const char \*t = visible && text &&/const char *t = text \&\&/'
mutate "$d/real.h" "$d/ctl-stale.h"   's/return on && age < KT_STALE_FRAMES && text \? text : "";/return on \&\& text ? text : "";/'
mutate "$d/real.h" "$d/ctl-inherit.h" 's/    p.key = kt_on && want && \*want;/    p.key = ht_on \&\& kt_on \&\& want \&\& *want;/'
mutate "$d/real.h" "$d/ctl-tapinherit.h" 's/    if \(kt_on && key_shown && in_key\)/    if (ht_on \&\& kt_on \&\& key_shown \&\& in_key)/'
mutate "$d/real.h" "$d/ctl-wholebar.h" 's/    if \(kt_on && key_shown && in_key\)/    if (kt_on \&\& key_shown \&\& (in_key || in_bar))/'
mutate "$d/real.h" "$d/ctl-offscreen.h" 's/    if \(px \+ w > area\[0\] \+ area\[2\]\)/    if (0)/'
for v in real ctl-noeye ctl-under ctl-hidden ctl-stale ctl-inherit ctl-tapinherit ctl-wholebar ctl-offscreen; do
    $CC "${CFLAGS[@]}" -I$T -DKTLAYOUT_HDR="\"$d/$v.h\"" -o "$d/$v" $T/ktlayout_test.c
done
suite "key in title" "$d/real"

# Community Mods > Browse: which catalog mods show, and in what order (guest/azmod/cm_browse.h)
d="$OUT/cmbrowse"; mkdir -p "$d"
cp guest/azmod/cm_browse.h "$d/real.h"
mutate "$d/real.h" "$d/ctl-unknown.h"   's/    return CMB_OTHER;\n\}/    return 0;\n}/'
mutate "$d/real.h" "$d/ctl-date.h"      's/return mo >= 1 && mo <= 12 && d >= 1 && d <= 31;/return 1;/'
mutate "$d/real.h" "$d/ctl-unstable.h"  's/\(c == 0 && out\[j - 1\] < i\)/(c == 0 \&\& 0)/'
mutate "$d/real.h" "$d/ctl-undated.h"   's/            return da \? -1 : 1;/            return da ? 1 : -1;/'
mutate "$d/real.h" "$d/ctl-usable.h"    's/if \(!a->usable != !b->usable\)/if (0)/'
mutate "$d/real.h" "$d/ctl-installed.h" 's/if \(sort == CMB_SORT_INSTALLED && !a->installed != !b->installed\)/if (0)/'
mutate "$d/real.h" "$d/ctl-digits.h"    's/if \(cmb_digit\(\*a\) && cmb_digit\(\*b\)\) \{/if (0) {/'
mutate "$d/real.h" "$d/ctl-dl14.h"      's/if \(i >= 13 \|\| v\[i\] < /if (v[i] < /'
mutate "$d/real.h" "$d/ctl-dlorder.h"   's/return a->downloads > b->downloads \? -1 : 1;/return a->downloads < b->downloads ? -1 : 1;/'
mutate "$d/real.h" "$d/ctl-uncounted.h" 's/return a->counted \? -1 : 1;/return a->counted ? 1 : -1;/'
mutate "$d/real.h" "$d/ctl-shelfmin.h"  's/#define CMB_SHELF_MIN 2 /#define CMB_SHELF_MIN 1 /'
mutate "$d/real.h" "$d/ctl-topzero.h"   's/return m->counted && m->downloads > 0;/return m->counted;/'
mutate "$d/real.h" "$d/ctl-comma.h"     's/if \(d && d % 3 == 0\)/if (0)/'
mutate "$d/real.h" "$d/ctl-clock.h"     's/long age = now >= CMB_CLOCK_SET \?/long age = 1 ?/'
for v in real ctl-unknown ctl-date ctl-unstable ctl-undated ctl-usable ctl-installed ctl-digits ctl-dl14 ctl-dlorder ctl-uncounted ctl-shelfmin ctl-topzero ctl-comma ctl-clock; do
    $CC "${CFLAGS[@]}" -I$T -DCMBROWSE_HDR="\"$d/$v.h\"" -o "$d/$v" $T/cmbrowse_test.c
done
suite "community browse" "$d/real"

# Community Mods > Updates: Update all, one install after another (guest/azmod/cm_updall.h)
d="$OUT/cmupd"; mkdir -p "$d"
cp guest/azmod/cm_updall.h "$d/real.h"
mutate "$d/real.h" "$d/ctl-nowait.h"  's/    if \(r->sent\) \{/    if (r->sent \&\& 0) {/'
mutate "$d/real.h" "$d/ctl-nocat.h"   's/if \(in->cgen == r->cgen0 && ok\(/if (0 \&\& ok(/'
mutate "$d/real.h" "$d/ctl-noerr.h"   's/        if \(!strcmp\(st, "error"\)\) return cmu_end\(r, CMU_FAILED\);\n//'
mutate "$d/real.h" "$d/ctl-noskip.h"  's/    while \(r->pos < r->n && !ok\(r->ids\[r->pos\], user\)\) r->pos\+\+;\n//'
mutate "$d/real.h" "$d/ctl-busy.h"    's/    if \(cmu_busy_state\(st\)\) return CMU_WAIT;/    if (0) return CMU_WAIT;/'
mutate "$d/real.h" "$d/ctl-stale.h"   's/        if \(in->sgen == r->sgen0\) \/\*/        if (0) \/*/'
mutate "$d/real.h" "$d/ctl-count.h"   's/\(r->pos < r->n \? r->pos : r->n - 1\) \+ 1, r->n\)/r->pos, r->n)/'
for v in real ctl-nowait ctl-nocat ctl-noerr ctl-noskip ctl-busy ctl-stale ctl-count; do
    $CC "${CFLAGS[@]}" -I$T -DCMUPD_HDR="\"$d/$v.h\"" -o "$d/$v" $T/cmupd_test.c
done
suite "community updates" "$d/real"

# a scrolled list's motion: follow, fling, ends, catch, knob
d="$OUT/glide"; mkdir -p "$d"
cp guest/azmod/scroll_glide.h "$d/real.h"
mutate "$d/real.h" "$d/ctl-nofollow.h"  's/    g->pos \+= \(g->goal - g->pos\) \* \(t \/ \(GLIDE_FOLLOW_MS \+ t\)\);/    g->pos = g->goal;/'
mutate "$d/real.h" "$d/ctl-nofling.h"   's/        g->v = glide_clampf\(v, -GLIDE_MAX_V, GLIDE_MAX_V\);/        (void) v;/'
mutate "$d/real.h" "$d/ctl-noend.h"     's/    if \(g->goal <= 0 \|\| g->goal >= max\) \{/    if (0) {/'
mutate "$d/real.h" "$d/ctl-stillfling.h" 's/    float v = ms - g->sms <= GLIDE_WINDOW_MS \? g->sv : 0;/    float v = g->sv;/'
mutate "$d/real.h" "$d/ctl-decay.h"     's/#define GLIDE_FLING_MS 200\.f/#define GLIDE_FLING_MS 325.f/'
mutate "$d/real.h" "$d/ctl-floor.h"     's/\(dt > GLIDE_STEP_MIN_MS \? dt : GLIDE_STEP_MIN_MS\)/(dt > 0 ? dt : 1)/'
mutate "$d/real.h" "$d/ctl-stop.h"      's/#define GLIDE_STOP_V 0\.06f/#define GLIDE_STOP_V 0.02f/'
mutate "$d/real.h" "$d/ctl-nocatch.h"   's/(    g->goal = g->pos;\n)    g->v = 0;\n/$1/'
mutate "$d/real.h" "$d/ctl-ease.h"      's/    g->pos = g->goal = y;\n/    g->goal = y;\n/'
for v in real ctl-nofollow ctl-nofling ctl-noend ctl-stillfling ctl-nocatch ctl-ease ctl-decay ctl-floor ctl-stop; do
    $CC "${CFLAGS[@]}" -I$T -DGLIDE_HDR="\"$d/$v.h\"" -o "$d/$v" $T/glide_test.c
done
suite "scroll glide" "$d/real"

# Channel Input Select Memory at start: the SETTINGS.DAT reader and the per-channel decision
d="$OUT/inputmem"; mkdir -p "$d"
cp guest/azmod/input_memory.h "$d/real.h"
mutate "$d/real.h" "$d/ctl-boundary.h" 's/        if \(i > 0 && b\[i - 1\] != 0\)\n            continue;\n//'
mutate "$d/real.h" "$d/ctl-type.h"     's/e\[0\] != 0x01 \|\| e\[1\] != 5 \|\| e\[2\] != 0x01/e[0] != 0x01/'
mutate "$d/real.h" "$d/ctl-settle.h"   's/return ms >= IM_SETTLE_MS \? IM_SELECT : IM_WAIT;/return IM_SELECT;/'
mutate "$d/real.h" "$d/ctl-override.h" 's/ \|\| cur != IM_DECK \|\|/ ||/'
mutate "$d/real.h" "$d/ctl-memory.h"   's/ \|\| s->memory != 1\)/)/'
for v in real ctl-boundary ctl-type ctl-settle ctl-override ctl-memory; do
    $CC "${CFLAGS[@]}" -I$T -DINPUTMEM_HDR="\"$d/$v.h\"" -o "$d/$v" $T/inputmem_test.c
done
suite "input memory" "$d/real"

# the Mod Menu reopens when SOURCE comes back: remembered on a leave, kept on the way back,
# forgotten by SOURCE with the menu up or a real source, taken once
d="$OUT/mmresume"; mkdir -p "$d"
cp guest/azmod/mm_resume.h "$d/real.h"
mutate "$d/real.h" "$d/ctl-srcclears.h" 's/return mmr_level\(open, want, focused\) != MMR_NONE \? MMR_NONE : cur;/return MMR_NONE;/'
mutate "$d/real.h" "$d/ctl-lower.h"     's/    return lvl > cur \? lvl : cur;/    return lvl;/'
mutate "$d/real.h" "$d/ctl-nowant.h"    's/    if \(!open \|\| !want\)/    if (!open)/'
mutate "$d/real.h" "$d/ctl-notake.h"    's/return __atomic_exchange_n\(lvl, MMR_NONE, __ATOMIC_RELAXED\);/return __atomic_load_n(lvl, __ATOMIC_RELAXED);/'
# the page: an open that brings it back without a resume, and a close that forgets it
mutate "$d/real.h" "$d/ctl-pagealways.h" 's/return __atomic_exchange_n\(armed, 0, __ATOMIC_ACQ_REL\) \? kept : MMR_PAGE_NONE;/return kept;/'
mutate "$d/real.h" "$d/ctl-pagekeep.h"   's/return mode != MMR_PAGE_NONE \? mode : kept;/return mode;/'
for v in real ctl-srcclears ctl-lower ctl-nowant ctl-notake ctl-pagealways ctl-pagekeep; do
    $CC "${CFLAGS[@]}" -pthread -I$T -DMMR_HDR="\"$d/$v.h\"" -o "$d/$v" $T/mmresume_test.c
done
suite "mm resume" "$d/real"

d="$OUT/sdkaudio"; mkdir -p "$d"
cp guest/azmod/sdk_audio.c "$d/real.c"
mutate "$d/real.c" "$d/ctl-pair.c"    's/\(const int32_t \*\) buf \+ 2 \* d,/(const int32_t *) buf,/'
mutate "$d/real.c" "$d/ctl-noclose.c" 's/    atomic_compare_exchange_strong\(&g_sdk_mix_pcm, &want, NULL\);\n/    (void) want;\n/'
mutate "$d/real.c" "$d/ctl-permod.c"  's/if \(acc && mine < SDK_TAPS_MOD && !full &&/if (acc \&\& !full \&\&/'
mutate "$d/real.c" "$d/ctl-reconf.c"  's/    atomic_compare_exchange_strong\(&g_sdk_mix_pcm, &was, NULL\);\n/    (void) was;\n/'
mutate "$d/real.c" "$d/ctl-cblock.c"  's/            int subs = g_sdk_tap_n;\n            pthread_mutex_unlock\(&g_sdk_tap_mu\);\n/            int subs = g_sdk_tap_n;\n/; s/            lost_seen\[s\] = lost;\n/            lost_seen[s] = lost;\n            pthread_mutex_unlock(\&g_sdk_tap_mu);\n/'
for v in real ctl-pair ctl-noclose ctl-permod ctl-reconf ctl-cblock; do
    $CC "${CFLAGS[@]}" -pthread -I$T -Iguest/azmod -DAUDIO_INC="\"$d/$v.c\"" -o "$d/$v" $T/sdkaudio_test.c -lm -ldl
done
suite "sdk: audio_tap" "$d/real"

d="$OUT/sdklib"; mkdir -p "$d"
cp guest/azmod/sdk_lib.c "$d/real.c"
mutate "$d/real.c" "$d/ctl-rw.c" 's/"file:%s\?immutable=1"/"file:%s"/; s/0x01 \| 0x40 \/\* READONLY \| URI \*\//0x02 | 0x40/'
mutate "$d/real.c" "$d/ctl-ui.c"      's/    if \(g_cm_on_ui\)\n/    if (0)\n/'
mutate "$d/real.c" "$d/ctl-anyport.c" 's/ \|\| sdk_port_of_sysfs\(real\) != stick \+ 1\)/)/'
mutate "$d/real.c" "$d/ctl-guess.c"   's/    if \(hits != 1\)\n/    if (hits < 1)\n/'
mutate "$d/real.c" "$d/ctl-kickstick.c" 's/if \(!src\[0\] \|\| \(strncmp\(src, mount, ml\) == 0 && src\[ml\] == .\/.\) \|\| sdk_spool_waits\(src\)\) \{/if (!src[0]) {/'
mutate "$d/real.c" "$d/ctl-kickother.c" 's/ \|\| sdk_spool_waits\(src\)\) \{/) {/'
mutate "$d/real.c" "$d/ctl-kickunknown.c" 's/if \(!src\[0\] \|\| \(strncmp/if ((strncmp/; s/return 1; \/\* not on a stick it knows/return 0; \/* not on a stick it knows/; s/    if \(!end\)\n        return 1;/    if (!end)\n        return 0;/'
mutate "$d/real.c" "$d/ctl-trunc.c"   's/            rc = 1; \/\* the first k rows; the rest did not fit \*\//            rc = 0;/g'
if [ "$(uname -s)" = Linux ]; then SDK_EXPORT=-rdynamic; else SDK_EXPORT=-Wl,-export_dynamic; fi
for v in real ctl-rw ctl-ui ctl-anyport ctl-guess ctl-kickstick ctl-kickunknown ctl-kickother ctl-trunc; do
    $CC "${CFLAGS[@]}" $SDK_EXPORT -I$T -Iguest/azmod -DLIB_INC="\"$d/$v.c\"" -o "$d/$v" $T/sdklib_test.c -lsqlite3 -ldl
done
suite "sdk: library (read-only)" "$d/real"

# The handoff guard (guest/azhandoff): BACK held 3 s stops a hung program, and the restore that
# starts EP147, checks it, retries once and reboots as the last resort. Built for this host
# against a fake systemctl; a FIFO stands in for the panel bus (handoffguard_test.c).
d="$OUT/hguard"; mkdir -p "$d"
hg_build() { # hg_build <variant> <guard_logic.h> <az-handoff-guard.c>
    mkdir -p "$d/$1"
    cp "$2" "$d/$1/guard_logic.h"; cp "$3" "$d/$1/az-handoff-guard.c"
    $CC -O1 -Wall -I"$d/$1" -DHG_SYSTEMCTL="\"$d/$1/systemctl\"" -DHG_SETTLE_S=0 -DHG_BUS="\"$d/$1/bus\"" \
        -o "$d/$1/guard" "$d/$1/az-handoff-guard.c"
    $CC -O1 -Wall -I$T -DGUARD_LOGIC="\"$d/$1/guard_logic.h\"" -DGUARD_BIN="\"$d/$1/guard\"" \
        -DFAKE_SYSTEMCTL="\"$d/$1/systemctl\"" -DGUARD_FIFO="\"$d/$1/bus\"" -o "$d/$1/t" $T/handoffguard_test.c
}
HGL=guest/azhandoff/guard_logic.h; HGC=guest/azhandoff/az-handoff-guard.c
hg_build real $HGL $HGC
# controls: no CRC check (a bus error reads as BACK); the hold survives a gap in the stream;
# the restore neither retries nor reboots
mutate $HGL "$d/nocrc.h" 's/    if \(n < HG_MIXER_LEN\)\n        return 0;\n    uint16_t c = hg_crc16_x25\(f, 96\);\n    return f\[96\] == \(uint8_t\) \(c & 0xff\) && f\[97\] == \(uint8_t\) \(c >> 8\);/    return n >= HG_MIXER_LEN;/'
mutate $HGL "$d/nogap.h" 's/        s->since_ms = -1; \/\* the stream stopped/        (void) 0; \/* the stream stopped/'
mutate $HGC "$d/noretry.c" 's/    fprintf\(stderr, "az-handoff-guard: EP147 did not start; trying once more\\n"\);\n    sleep\(2\);\n    if \(ep147_started\(\)\)\n        return 0;\n    fprintf\(stderr, "az-handoff-guard: EP147 still not running; rebooting the deck\\n"\);\n    systemctl\("reboot", NULL, NULL, NULL\);\n/    (void) 0;\n/'
hg_build ctl-nocrc "$d/nocrc.h" $HGC
hg_build ctl-nogap "$d/nogap.h" $HGC
hg_build ctl-noretry $HGL "$d/noretry.c"
suite "handoff guard" "$d/real/t"

# row_set (sdk_rows.c): a running mod's own rows only, from its own code (sdkrows_test.c)
d="$OUT/sdkrows"; mkdir -p "$d"
{ extract guest/azmod/community.c '^static int cm_called_by\('
  extract guest/azmod/sdk_rows.c '^static int cm_row_set\('; } > "$d/real.inc"
grep -q 'cm_called_by(c, CM_RA())' "$d/real.inc" || { echo "extraction of cm_row_set failed"; exit 2; }
# control: any mod's row set (the owner not checked)
mutate "$d/real.inc" "$d/ctl-anyowner.inc" 's/    if \(!c \|\| !cm_called_by\(c, CM_RA\(\)\) \|\|\n\s*\(c->state != AZCM_LOADED && \(int\) \(c - g_cm\) != g_cm_loading\)\) \{/    if (0) {/'
for v in real ctl-anyowner; do
    $CC "${CFLAGS[@]}" -I$T -DSDKROWS_INC="\"$d/$v.inc\"" -o "$d/$v" $T/sdkrows_test.c
done
suite "sdk: row_set" "$d/real"

# deck_info (sdk_deck.c): exactly the caller's .size written, unknown never mistaken for a value
d="$OUT/sdkdeck"; mkdir -p "$d"
{ grep -E '^static _Atomic\(azstem_deck_snapshot_fn\) g_sd_snap;|^static _Atomic int g_sd_playing\[' guest/azmod/sdk_deck.c
  extract guest/azmod/sdk_deck.c '^static int sd_snapshot\('
  extract guest/azmod/sdk_deck.c '^static int sd_deck_info\('; } > "$d/real.inc"
grep -q 'g_sd_snap;' "$d/real.inc" && grep -q 'g_sd_playing' "$d/real.inc" && grep -q 'static int sd_deck_info' "$d/real.inc" ||
    { echo "extraction of sd_deck_info failed"; exit 2; }
# control: the core's whole struct copied whatever the caller's size
mutate "$d/real.inc" "$d/ctl-size.inc" 's/size_t n = \(size_t\) size < sizeof v \? \(size_t\) size : sizeof v;/size_t n = sizeof v;/'
# control: a caller's struct too small taken anyway
mutate "$d/real.inc" "$d/ctl-min.inc" 's/    if \(size < \(int\) \(offsetof\(az_deck_info, loaded\) \+ sizeof out->loaded\)\)\n        return -1;\n//'
for v in real ctl-size ctl-min; do
    $CC "${CFLAGS[@]}" -I$T -Iguest/azmod -DSDKDECK_INC="\"$d/$v.inc\"" -o "$d/$v" $T/sdkdeck_test.c
done
suite "sdk: deck_info" "$d/real"

# sdk_linux <dir> <test.c> <macro> <variant...> : build each variant for Linux
sdk_linux() {
    local dd=$1 src=$2 mac=$3; shift 3
    [ "$(uname -s)" = Linux ] || ! co_skip "$(basename "$src") ($# builds): off Linux they build in azmod-build:18.04 (a container)" ||
        return 0
    local flags=(-std=gnu11 -O1 -Wall -Wno-unused-function -Wno-unused-variable -fPIE -pie
                 -Wl,-z,relro,-z,now -rdynamic -pthread -I$T -Iguest/azmod -Iguest/common)
    for v in "$@"; do
        if [ "$(uname -s)" = Linux ]; then
            $CC "${flags[@]}" -D$mac="\"$dd/$v.c\"" -o "$dd/$v" $src -ldl
        else
            docker run --rm --platform=linux/arm64 -v "$ROOT:$ROOT" -v "$dd:$dd" -w "$ROOT" azmod-build:18.04 \
                gcc "${flags[@]}" -D$mac="\"$dd/$v.c\"" -o "$dd/$v.bin" $src -ldl
            printf '#!/bin/sh\nexec docker run --rm --platform=linux/arm64 -v "%s:%s" -e TMPDIR=%s azmod-build:18.04 "%s" "$@"\n' \
                "$dd" "$dd" "$dd" "$dd/$v.bin" > "$dd/$v"
            chmod +x "$dd/$v"
        fi
    done
}
SDKD=${FD_SCRATCH:-$OUT}
d="$SDKD/sdkproc"; mkdir -p "$d"
cp guest/azmod/sdk_proc.c "$d/real.c"
mutate "$d/real.c" "$d/ctl-anypath.c"  's/    if \(strncmp\(rp, rd, k\) != 0 \|\| rp\[k\] != .\/.\)\n        return 0;\n/    (void) k;\n/'
mutate "$d/real.c" "$d/ctl-preload.c"  's/            if \(strncmp\(environ\[i\], "LD_PRELOAD=", 11\) != 0\)\n//'
mutate "$d/real.c" "$d/ctl-anyowner.c" 's/g_sdk_proc\[i\].pid == pid && pid > 0 && !strcmp\(g_sdk_proc\[i\].id, id\)/g_sdk_proc[i].pid == pid \&\& pid > 0/'
mutate "$d/real.c" "$d/ctl-playing.c"  's/    if \(sdk_any_deck_playing\(\)\) \{\n        cm_logf\(c, 1, "handoff refused: a deck is playing"\);\n        goto out;\n    \}\n/    if (0)\n        goto out;\n/'
mutate "$d/real.c" "$d/ctl-nocap.c"    's/        if \(out >= 0 && size \+ n > SDK_LOG_CAP && size > 0\) \{/        if (0) {/'
mutate "$d/real.c" "$d/ctl-fdleak.c"   's/        sdk_close_inherited\(&fa\); \/\* after the dup2s[^\n]*\n/        (void) 0;\n/'
mutate "$d/real.c" "$d/ctl-noguard.c"  's/    return access\(SDK_GUARD_STAGE, R_OK\) == 0 \? SDK_GUARD_STAGE : NULL;/    return NULL;/'
# control: the log's own name taken as given (a link or a second name of a file elsewhere)
mutate "$d/real.c" "$d/ctl-logname.c"  's/    return lstat\(out, &st\) != 0 \? errno == ENOENT : S_ISREG\(st.st_mode\) && st.st_nlink == 1;/    (void) st;\n    return 1;/'
# control: the log opened through a link
mutate "$d/real.c" "$d/ctl-logfollow.c" 's/ \| O_NOFOLLOW, 0644\);/, 0644);/; s/if \(fd >= 0 && \(fstat\(fd, &st\) != 0 \|\| !S_ISREG\(st.st_mode\) \|\| st.st_nlink != 1\)\)/if (fd >= 0 \&\& fstat(fd, \&st) != 0)/'
# control: launch takes any number of arguments
mutate "$d/real.c" "$d/ctl-argmax.c" 's/        if \(i >= SDK_ARGV_MAX\) \{\n            cm_logf\(c, 1, "launch refused: more than %d arguments", SDK_ARGV_MAX\);/        if (i >= SDK_ARGV_MAX + 8) {\n            cm_logf(c, 1, "launch refused: more than %d arguments", SDK_ARGV_MAX);/'
# control: a second handoff while one is in progress
mutate "$d/real.c" "$d/ctl-busy.c" 's/    if \(atomic_exchange\(&g_sdk_handoff_busy, 1\)\) \{/    if (atomic_exchange(\&g_sdk_handoff_busy, 1) \&\& 0) {/'
# control: a failed spawn keeps the log pipe's write end (its copier never ends)
mutate "$d/real.c" "$d/ctl-pipeleak.c" 's/    if \(log\)\n        close\(pfd\[1\]\); \/\* the child holds the only write end now[^\n]*\n    return pid;/    if (log \&\& pid > 0)\n        close(pfd[1]);\n    return pid;/'
sdk_linux "$d" $T/sdkproc_test.c PROC_INC real ctl-anypath ctl-preload ctl-anyowner ctl-playing ctl-nocap ctl-fdleak ctl-noguard ctl-logname ctl-logfollow ctl-argmax ctl-busy ctl-pipeleak
suite "sdk: other programs" "$d/real"

d="$SDKD/sdkvslot"; mkdir -p "$d"
cp guest/azmod/sdk_vslot.c "$d/real.c"
mutate "$d/real.c" "$d/ctl-anyfn.c"   's/        if \(cur < g_cm_text_lo \|\| cur >= g_cm_text_hi\)\n            why = [^\n]*\n/        (void) cur;\n/'
mutate "$d/real.c" "$d/ctl-noowner.c" 's/    for \(int i = 0; i < g_sdk_vslot_n; i\+\+\)\n        if \(g_sdk_vslot\[i\].slot == slot\) \{/    for (int i = 0; i < 0; i++)\n        if (g_sdk_vslot[i].slot == slot) {/'
sdk_linux "$d" $T/sdkvslot_test.c VSLOT_INC real ctl-anyfn ctl-noowner
suite "sdk: claim_vslot" "$d/real"

d="$OUT/trackgen"; mkdir -p "$d"
{ extract guest/azstem/stem_playback.c '^static void deck_track_changed\('
  extract guest/azstem/stem_playback.c '^static void stems_tick\(' ; } > "$d/real.inc"
grep -q 'deck_track_changed(deck); /\* generation + identity: ejected \*/' "$d/real.inc" || { echo "extraction of stems_tick failed"; exit 2; }
# control 1: the eject path does not bump the generation (the old code)
mutate "$d/real.inc" "$d/ctl-eject.inc" 's/\n\s*deck_track_changed\(deck\); \/\* generation \+ identity: ejected \*\///'
# control 2: the bump after the curtrack publish (azmod would see the name before the generation)
mutate "$d/real.inc" "$d/ctl-order.inc" 's/\n\s*deck_track_changed\(deck\);\n(\s*\n\s*\/\* Drop the old stems)/\n$1/; s/(publish_deck_file\(deck, "curtrack", tr\);\n\s*publish_src\(deck\);)/$1\n        deck_track_changed(deck);/'
# control 3: no stems base skips the deck before its identity (the old code)
mutate "$d/real.inc" "$d/ctl-nobase.inc" 's/(int have_base = g_stems_base\[0\] != 0;)/$1\n        if (!have_base) continue;/'
# control 4: the track identified without a base never loads once a base turns up
mutate "$d/real.inc" "$d/ctl-deferred.inc" 's/g_load_deferred\[deck\] = 1;/g_load_deferred[deck] = 0;/'
for v in real ctl-eject ctl-order ctl-nobase ctl-deferred; do
    $CC "${CFLAGS[@]}" -Wno-unused-parameter -I$T -DTICK_INC="\"$d/$v.inc\"" -o "$d/$v" $T/trackgen_test.c
done
suite "trackgen" "$d/real"


# ---------------------------------------------------------------- close-on-exec
# (runs its own control: a planted fopen "rb" / open / socket must be reported)
echo "=== cloexec lint ==="
if ! plain_only "cloexec lint"; then :; elif "$T/cloexec-lint.sh" guest; then :; else failures=$((failures + 1)); fi
echo

# ---------------------------------------------------------------- one definition per macro
echo "=== duplicate #define lint ==="
if [ -n "$COMPILE_ONLY" ]; then echo "(compile-only pass: duplicate #define lint not run)"
elif bash "$T/dupdefine-lint.sh"; then :; else failures=$((failures + 1)); fi
# control: a header that repeats a definition must be reported
dupdefine_ctl() {
    local dd="$OUT/dupdefine"; mkdir -p "$dd"
    printf '#ifndef X_H\n#define X_H\n#define PLANTED 1\n#define OTHER 2\n#define PLANTED 1\n#endif\n' > "$dd/planted.h"
    bash "$T/dupdefine-lint.sh" "$dd"
}
echo

# ---------------------------------------------------------------- EP147-thread blocking lint
# (runs its own control: one planted violation per rule must be reported)
echo "=== ep147-thread lint ==="
if ! plain_only "ep147-thread lint"; then :; elif "$T/ep147-thread-lint.sh" guest; then :; else failures=$((failures + 1)); fi
echo

# ---------------------------------------------------------------- formatting
# The in-scope sources are formatted as .clang-format says, by clang-format 21.1.8 exactly
# (tools/format-check.sh: a host clang-format of that version, else the one in azmod-test:18.04).
# A missing or wrong clang-format is a failure here, never a skip.
# Controls, run with the same instrument in the same round: a copy of a real file passes, and the
# same copy with one brace's spacing broken is reported by name. The copies live under guest/out
# (docker shares the source tree, not $TMPDIR).
echo "=== formatting (format-check.sh) ==="
if ! plain_only "format-check"; then :; elif bash tools/format-check.sh; then :; else failures=$((failures + 1)); fi
FC="$ROOT/guest/out/unit-fmtctl"; rm -rf "$FC"; mkdir -p "$FC/real" "$FC/ctl"
cp guest/azmod/beat_jump.c "$FC/real/beat_jump.c"
mutate guest/azmod/beat_jump.c "$FC/ctl/beat_jump.c" 's/\) \{$/){/m'
for v in real ctl; do
    [ -z "$SAN" ] && [ -z "$COMPILE_ONLY" ] || break
    rc=0; out=$(bash tools/format-check.sh "$FC/$v/beat_jump.c" 2>&1) || rc=$?
    sed 's/^/      /' <<<"$out"
    if [ $rc -ne 0 ] && [ $rc -ne 1 ]; then
        echo "FAIL  format-check could not run on the $v copy (exit $rc): a failure, not a skip"
        failures=$((failures + 1))
    elif [ $v = real ]; then
        if [ $rc -eq 0 ]; then echo "PASS  format-check: an unchanged copy of beat_jump.c is formatted"
        else echo "FAIL  format-check: an unchanged copy of beat_jump.c reported"; failures=$((failures + 1)); fi
    elif [ $rc -eq 1 ] && grep -q "^UNFORMATTED  .*/ctl/beat_jump.c$" <<<"$out"; then
        echo "CONTROL FAILED AS REQUIRED  format-check: beat_jump.c with ') {' made '){'"
    else
        echo "BROKEN TEST: control PASSED  format-check: beat_jump.c with ') {' made '){' (the check cannot see it)"
        broken_controls=$((broken_controls + 1))
    fi
done
rm -rf "$FC"
echo

# ---------------------------------------------------------------- static analysis
# clang-tidy (guest/.clang-tidy) and cppcheck (tools/static-check/cppcheck.supp) over the release
# translation units: any finding not suppressed with its reason fails (tools/static-check.sh: the
# pinned tools on PATH, else the ones in azmod-test:18.04). A check that cannot run is a failure
# here, never a skip.
# Controls, same instrument, same round: unchanged copies of azconf.c and az-handoff-guard.c are
# clean under their units' flags; the copy of azconf.c with a write after free() is reported by
# both analyzers (each one proven to run), and the guard copy whose read() overruns its 512-byte
# frame buffer by one is reported by cppcheck. The copies live under guest/out (docker shares the
# source tree, not $TMPDIR).
echo "=== static analysis (static-check.sh) ==="
if [ -n "$COMPILE_ONLY" ]; then echo "(compile-only pass: static-check.sh not run)"
elif bash tools/static-check.sh; then :; else failures=$((failures + 1)); fi
SCC="$ROOT/guest/out/unit-staticctl"; rm -rf "$SCC"; mkdir -p "$SCC/real" "$SCC/uaf" "$SCC/oob"
cp guest/azmod/azconf.c guest/azhandoff/az-handoff-guard.c "$SCC/real/"
mutate guest/azmod/azconf.c "$SCC/uaf/azconf.c" 's/free\(l->raw\);/free(l->raw);\n    l->raw[0] = 0;/'
mutate guest/azhandoff/az-handoff-guard.c "$SCC/oob/az-handoff-guard.c" 's/read\(fd, f, sizeof f\)/read(fd, f, sizeof f + 1)/'
# static_case <label> <unit> <copy> [expected finding line (ERE)...]: none given = must be clean
static_case() {
    local label=$1 tu=$2 f=$3 rc=0 out miss=; shift 3
    [ -z "$COMPILE_ONLY" ] || return 0
    out=$(bash tools/static-check.sh --as "$tu" "$f" 2>&1) || rc=$?
    sed 's/^/      /' <<<"$out"
    if [ $rc -ne 0 ] && [ $rc -ne 1 ]; then
        echo "FAIL  static-check could not run on $label (exit $rc): a failure, not a skip"
        failures=$((failures + 1)); return
    fi
    if [ $# -eq 0 ]; then
        if [ $rc -eq 0 ]; then echo "PASS  static-check: $label is clean"
        else echo "FAIL  static-check: $label reported"; failures=$((failures + 1)); fi
        return
    fi
    for re in "$@"; do grep -Eq "$re" <<<"$out" || miss="$miss [$re]"; done
    if [ $rc -eq 1 ] && [ -z "$miss" ]; then
        echo "CONTROL FAILED AS REQUIRED  static-check: $label"
    else
        echo "BROKEN TEST: control PASSED  static-check: $label, not reported:${miss:- (exit 0)}"
        broken_controls=$((broken_controls + 1))
    fi
}
static_case "an unchanged copy of azconf.c" azmod/azconf.c "$SCC/real/azconf.c"
static_case "an unchanged copy of az-handoff-guard.c" azhandoff/az-handoff-guard.c "$SCC/real/az-handoff-guard.c"
static_case "azconf.c line_free writing l->raw after free()" azmod/azconf.c "$SCC/uaf/azconf.c" \
    '^TIDY  .*/uaf/azconf\.c:[0-9]+:[0-9]+: warning: Use of memory after it is freed \[clang-analyzer-unix\.Malloc\]$' \
    '^CPPCHECK  .*/uaf/azconf\.c:[0-9]+: cppcheck-deallocuse: '
static_case "az-handoff-guard.c read() one byte past its 512-byte buffer" azhandoff/az-handoff-guard.c "$SCC/oob/az-handoff-guard.c" \
    '^CPPCHECK  .*/oob/az-handoff-guard\.c:[0-9]+: cppcheck-bufferAccessOutOfBounds: '
rm -rf "$SCC"
echo

# ---------------------------------------------------------------- exports
# (builds the release .so files fresh; its control drops one name from the list)
echo "=== exported symbols (exports.sh) ==="
if ! plain_only "exports.sh (release libraries)"; then :; elif bash "$T/exports.sh"; then :; else failures=$((failures + 1)); fi
echo

# ---------------------------------------------------------------- no fork of EP147
# (uses the .so files exports.sh just built; its control plants a system() call)
echo "=== no system/popen/fork in the release libraries (nofork-check.sh) ==="
if ! plain_only "nofork-check.sh (release libraries)"; then :; elif bash "$T/nofork-check.sh"; then :; else failures=$((failures + 1)); fi
echo

# ---------------------------------------------------------------- no fault-free reads of EP147
# (uses the same .so files; its control plants a library with the string, safe_q and a reader)
echo "=== no /proc/self/mem or fault-free reader in the release libraries (procmem-lint.sh) ==="
if ! plain_only "procmem-lint.sh (release libraries)"; then :; elif bash "$T/procmem-lint.sh"; then :; else failures=$((failures + 1)); fi
echo

# ---------------------------------------------------------------- addrgen
# The committed address tables must be exactly what the generator writes from the EP_ names
# in use, so a stale or hand-edited table cannot ship. Controls run the same --check on temp
# copies of the tree: one with an EP_ use removed, one with a v2.00 value changed.
if [ "$PUBLIC" = 1 ]; then
    # the tables are generated from address notes the published tree does not carry; it ships
    # the generated headers (addrgen-setup.sh)
    skip "ep147 address tables: generated from notes the published tree does not carry"
else
    echo "=== ep147 address tables are current (ep147-addrgen.py --check) ==="
    if ! plain_only "ep147-addrgen.py --check"; then :; elif python3 tools/ep147-addrgen.py --check; then :; else failures=$((failures + 1)); fi
    echo
    AG="$OUT/addrgen"
    "$T/addrgen-setup.sh" "$AG"
fi

# ---------------------------------------------------------------- fuzz harnesses (tools/fuzz)
# Plain pass only: the harnesses are built by clang-10 with their own sanitizers, which a
# gcc sanitizer pass neither instruments nor reads the reports of.
if [ -z "$SAN" ]; then
# The libFuzzer harnesses for the parsers that read untrusted input (listed in tools/fuzz/lib.sh), built
# against the real source the same way (tools/fuzz/lib.sh). On any host: each seed corpus and its
# crash regressions (regress-*) replayed once under ASan+UBSan. With libFuzzer (clang-10, in
# azmod-test:18.04): each harness also fuzzes for FZ_SECS (default 3) seconds from its corpus.
# Control: a planted off-by-one in cm_json_strat must fail the replay, and (with libFuzzer) be
# found by fuzzing from a one-input corpus that does not itself trigger it.
echo "=== fuzz harnesses (tools/fuzz) ==="
. tools/fuzz/lib.sh
FZ="$OUT/fuzz"
FZ_SECS=${FZ_SECS:-3}
# fz_short <bin> <secs> <dir> <corpus...>: one bounded libFuzzer run, quiet unless it fails
fz_short() {
    local bin=$1 secs=$2 dir=$3 rc=0; shift 3
    "$bin" -max_total_time="$secs" -timeout=10 -rss_limit_mb=1536 -artifact_prefix="$dir/" \
        "$@" > "$dir/fuzz.log" 2>&1 || rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "PASS  $(basename "$dir"): $(grep -E '^Done [0-9]+ runs' "$dir/fuzz.log" | tail -1)"
    else
        grep -E 'ERROR|runtime error|INVARIANT|Test unit written' "$dir/fuzz.log" | head -6
        echo "FAIL  $(basename "$dir"): exit $rc"
    fi
    return "$rc"
}
fz_planted_found() { # the planted bug fuzzed from one harmless seed for up to 120 s
    mkdir -p "$FZ/ctl-strat/seed" "$FZ/ctl-strat/corpus"
    printf '\000{"id":"ab"}' > "$FZ/ctl-strat/seed/one"
    fz_short "$FZ/ctl-strat/fuzz" 120 "$FZ/ctl-strat" "$FZ/ctl-strat/corpus" "$FZ/ctl-strat/seed"
}
for n in $FZ_NAMES; do
    fz_prepare "$n" "$FZ/$n/src" || exit 2
    if fz_compile "$n" "$FZ/$n/src" "$FZ/$n/replay" replay; then
        suite "fuzz corpus replay: $n" "$FZ/$n/replay" "tools/fuzz/corpus/$n"
    else
        echo "FAIL  fuzz build (replay): $n"; failures=$((failures + 1))
    fi
done
# the bugs the harnesses found, put back one at a time: each crash regression in the corpus must
# fail the replay of its harness again (so the corpus can tell the fixed code from the old)
fz_revert() { # <label> <harness> <file in the prepared source> <perl substitution>
    local dir="$FZ/rev-$1"
    fz_prepare "$2" "$dir/src" || exit 2
    mutate "$dir/src/$3" "$dir/mut" "$4"
    mv "$dir/mut" "$dir/src/$3"
    fz_compile "$2" "$dir/src" "$dir/replay" replay || exit 2
}
fz_revert pdbpage pdbsync pdb_sync.c 's/return \(\(size_t\) p \+ 1\) \* d->pl <= d->n/return (size_t) (p + 1) * d->pl <= d->n/'
fz_revert pdbrows pdbsync pdb_sync.c 's/    if \(\(unsigned\) \(n \+ 15\) \/ 16 \* 0x24 > d->pl - PS_HDR\)\n        return -1;[^\n]*\n//'
fz_revert pdbsrcstr pdbsync pdb_sync.c 's/            if \(\(loc & 0xffff\) \+ f \+ 3 > s\.pl\)\n                goto out;[^\n]*\n//'
fz_revert pdbstr pdbsync pdb_sync.c 's/unsigned n = s\[0\] >> 1 \? \(s\[0\] >> 1\) - 1 : 0;\n        if \(n > avail - 1\)\n            n = avail - 1;/unsigned n = (s[0] >> 1) - 1;/'
fz_revert elfalign elfcheck elfcheck.inc 's/ && off % align == 0;/;/'
fz_revert datelen cmbrowse cm_browse.h 's/    if \(v\[10\]\)\n        return 0;\n//'
fz_revert jsonu stemname stemname.inc 's/                if \(strnlen\(s \+ 1, 4\) < 4\)[^\n]*\n                    return 0;\n//'
# a planted bug the colfields corpus must catch: a duplicate column id accepted (browse_fields.h)
fz_revert colfdup colfields browse_fields.h 's/ \|\| field\[id\] != B2_NONE\)/)/'
fz_prepare cmjson "$FZ/ctl-strat/src" || exit 2
mutate "$FZ/ctl-strat/src/cmjson.inc" "$FZ/ctl-strat/cmjson.mut" 's/if \(k < n - 1\) out\[k\+\+\] = c;/if (k < n) out[k++] = c;/'
mv "$FZ/ctl-strat/cmjson.mut" "$FZ/ctl-strat/src/cmjson.inc"
fz_compile cmjson "$FZ/ctl-strat/src" "$FZ/ctl-strat/replay" replay || exit 2
# tools/fuzz/run.sh must fail closed: a harness that does not extract or build is a FAIL line and a
# non-zero exit, in every mode, and the other harnesses are still reported. The scratch trees share
# guest/ and the unit tests and carry their own copy of tools/fuzz with one thing broken.
fz_closed_tree() { # <dir>
    mkdir -p "$1/tools"
    ln -s "$ROOT/guest" "$1/guest"
    ln -s "$ROOT/tools/unit-tests" "$1/tools/unit-tests"
    cp -R tools/fuzz "$1/tools/fuzz"
}
fz_closed_tree "$FZ/ctl-closed-build"
echo 'this line is not C;' >> "$FZ/ctl-closed-build/tools/fuzz/png_fuzz.c"
fz_closed_tree "$FZ/ctl-closed-extract"
mutate "$FZ/ctl-closed-extract/tools/fuzz/lib.sh" "$FZ/ctl-closed-extract/lib.mut" 's/static int cmi_png_fit\(unsigned char/static int cmi_png_gone(unsigned char/'
mv "$FZ/ctl-closed-extract/lib.mut" "$FZ/ctl-closed-extract/tools/fuzz/lib.sh"
# fz_closed <tree> <FAIL prefix> <other harnesses that must still PASS> <run.sh args...>: exits
# nonzero only when run.sh did the same AND named the broken harness; zero (a BROKEN TEST) otherwise
fz_closed() {
    local d=$1 want=$2 others=$3 rc=0 got; shift 3
    ( cd "$d" && bash tools/fuzz/run.sh "$@" ) > "$d/run.out" 2>&1 || rc=$?
    got=$(grep -c '^PASS' "$d/run.out" || true)
    if [ "$rc" -ne 0 ] && grep -q "^FAIL  $want" "$d/run.out" && [ "$got" -eq "$others" ]; then
        grep "^FAIL  $want" "$d/run.out" | head -2
        return 1
    fi
    echo "run.sh $*: exit $rc, $got PASS lines (wanted $others), no FAIL line for the broken harness"
    return 0
}
# the same round passes: an intact harness replays through run.sh with exit 0 and a PASS line
fz_runsh_passes() { # <name>
    local out
    out=$(bash tools/fuzz/run.sh --replay "$1") || { echo "$out"; return 1; }
    echo "$out"
    grep -q "^PASS  fuzz replay $1: " <<< "$out"
}
suite "fuzz run.sh: replay png through the script passes" fz_runsh_passes png
FZ_ALL_OTHERS=$(( $(wc -w <<< "$FZ_NAMES") - 1 ))
FZ_LIB=0
if fz_has_libfuzzer; then
    FZ_LIB=1
    for n in $FZ_NAMES; do
        mkdir -p "$FZ/$n/corpus"
        if fz_compile "$n" "$FZ/$n/src" "$FZ/$n/fuzz" fuzz; then
            suite "fuzz $n (${FZ_SECS} s)" fz_short "$FZ/$n/fuzz" "$FZ_SECS" "$FZ/$n" "$FZ/$n/corpus" "tools/fuzz/corpus/$n"
        else
            echo "FAIL  fuzz build (libFuzzer): $n"; failures=$((failures + 1))
        fi
    done
    fz_compile cmjson "$FZ/ctl-strat/src" "$FZ/ctl-strat/fuzz" fuzz || exit 2
elif [ "$(uname -s)" = Linux ]; then
    skip "fuzz: no clang-10 with libFuzzer here (rebuild azmod-test:18.04 from docker/azmod-test.Dockerfile)"
else
    echo "(fuzz: no libFuzzer with this host's compiler: the corpora were replayed, not fuzzed)"
fi
else
    FZ="$OUT/fuzz" FZ_LIB=0 FZ_ALL_OTHERS=0
    echo "(fuzz: plain pass only; the harnesses carry their own clang-10 ASan + UBSan)"
fi
echo

# ---------------------------------------------------------------- controls
# ThreadSanitizer regressions: each race the TSan pass found and fixed, put back in a copy of the
# fixed code, must be reported again (the fixed code itself runs clean in its suite above).
if [ "$SAN" = tsan ]; then
    echo "=== ThreadSanitizer regressions (each must be reported) ==="
    d="$OUT/sanregress"; mkdir -p "$d"
    # the Beat Sync grid's fields written with plain stores (drumroll.c drum_grid_publish)
    mutate "$OUT/sdk/real.inc" "$d/grid-plain.inc" 's/atomic_store_explicit\(&g->beat_src, beat, memory_order_relaxed\);/*(double *) \&g->beat_src = beat;/'
    $CC "${CFLAGS[@]}" -pthread -I$T -DSDK_LOGIC_INC="\"$OUT/sdk/real.h\"" -DGRID_INC="\"$d/grid-plain.inc\"" -o "$d/grid-plain" $T/sdk_test.c -lm
    san_control "TSan: the beat grid's fields written with plain stores" 'ThreadSanitizer: data race' "$d/grid-plain" 1.0
    # the track snapshot's words written with plain stores (deck_track_map.c lti_snap_put)
    mutate "$OUT/lifetime/real.inc" "$d/snap-plain.inc" 's/__atomic_store_n\(&w\[i\], v, __ATOMIC_RELAXED\);/w[i] = v;/'
    $CC "${CFLAGS[@]}" -pthread -I$T -DLIFE_INC="\"$d/snap-plain.inc\"" -o "$d/snap-plain" $T/ltisnap_test.c
    san_control "TSan: the track snapshot written with plain stores" 'ThreadSanitizer: data race' "$d/snap-plain" 1.0
    # a VinylSpeedAdjustor published with a relaxed compare-and-swap (vinyl_brake.c vb_set_hook)
    mutate "$OUT/readers/vb.inc" "$d/vb-relaxed.inc" 's/__atomic_compare_exchange_n\(&g_vb_adj\[i\], &z, self, 0, __ATOMIC_SEQ_CST,/__atomic_compare_exchange_n(\&g_vb_adj[i], \&z, self, 0, __ATOMIC_RELAXED,/'
    $CC "${CFLAGS[@]}" -pthread -I$T -Iguest/azmod -DMECH=3 -DRC_INC="\"$d/vb-relaxed.inc\"" -o "$d/vb-relaxed" $T/readers_test.c
    san_control "TSan: a VinylSpeedAdjustor published relaxed" 'ThreadSanitizer: data race' "$d/vb-relaxed" 1.0
    [ ! -f guest/azmod/test/community/run.sh ] ||
        san_control "TSan: the community logs switch read as a plain int (community.c)" 'ThreadSanitizer: data race' cmhost "$CMH/ctl-logsrace"
    echo
fi
if [ -n "$SAN" ]; then
    echo "=== controls: run in the plain pass (a sanitizer abort would fail them for the wrong reason) ==="
else
    echo "=== controls (each must fail) ==="
fi
control "ep147build: md5 round constant off by one"       "$OUT/epb/ctl-md5/t" "${EPB_ARGS[@]}"
control "ep147build: identify by size only"               "$OUT/epb/ctl-nocompare/t" "${EPB_ARGS[@]}"
for lib in azmod azstem; do
    control "addr_init/$lib: completeness check removed"   "$OUT/addr-$lib/ctl-nocheck"
    control "addr_init/$lib: unknown build -> 1.30 table"  "$OUT/addr-$lib/ctl-fallback"
    control "addr_init/$lib: check+copy merged (partial)"  "$OUT/addr-$lib/ctl-partial"
done
control "crc16_x25: unreflected polynomial 0x1021"        "$OUT/crc/ctl-poly"
control "crc16_x25: final xor dropped"                    "$OUT/crc/ctl-xorout"
control "crc16_x25: init 0 instead of 0xFFFF"             "$OUT/crc/ctl-init"
control "clock_tz: a transition applies one second late"    "$OUT/clocktz/ctl-edge/t" $T/fixtures/zoneinfo
control "clock_tz: a DST footer rule read as no DST"       "$OUT/clocktz/ctl-rule/t" $T/fixtures/zoneinfo
control "clock_tz: transition times not checked for order" "$OUT/clocktz/ctl-order/t" $T/fixtures/zoneinfo
control "clock_tz: 12-hour noon and midnight shown as 0"   "$OUT/clocktz/ctl-12h/t" $T/fixtures/zoneinfo
control "leadin jump: a target exactly at the floor refused"        "$OUT/leadin/ctl-edge"
control "leadin jump: an unknown sample rate accepted"              "$OUT/leadin/ctl-rate"
control "leadin jump: a target at the start taken as before it"     "$OUT/leadin/ctl-start"
control "leadin jump: the floor computed in 32 bits"                "$OUT/leadin/ctl-ovf"
control "leadin jump: the floor's milliseconds taken as 1024ths"    "$OUT/leadin/ctl-unit"
control "remember wave zoom: 1/32 taken as a level"                          "$OUT/wzsave/ctl-level"
control "remember wave zoom: restored over a zoom that is not the stock 1/4" "$OUT/wzsave/ctl-stock"
control "remember wave zoom: a new zoom saved before it settles"             "$OUT/wzsave/ctl-settle"
control "remember wave zoom: a leading zero accepted"                        "$OUT/wzsave/ctl-zero"
control "remember wave zoom: text after the level accepted"                  "$OUT/wzsave/ctl-tail"
control "remember wave zoom: the saved level saved again"                    "$OUT/wzsave/ctl-same"
control "clock set: a field carries instead of wrapping"   "$OUT/clockset/ctl-carry/t" $T/fixtures/zoneinfo
control "clock set: 2000 taken as a common year"           "$OUT/clockset/ctl-leap/t" $T/fixtures/zoneinfo
control "clock set: a skipped local time lands an hour early" "$OUT/clockset/ctl-gap/t" $T/fixtures/zoneinfo
control "clock set: a set time shown after a power-off reset" "$OUT/clockset/ctl-valid/t" $T/fixtures/zoneinfo
control "clock_tz: half-hour offsets shown as whole hours"  "$OUT/clocktz/ctl-half/t" $T/fixtures/zoneinfo
control "clock_tz: 24-hour seconds not shown"               "$OUT/clocktz/ctl-sec24/t" $T/fixtures/zoneinfo
control "clock_tz: 12-hour seconds not shown"               "$OUT/clocktz/ctl-sec12/t" $T/fixtures/zoneinfo
control "clock native: the Clock drawn while EP147 records"      "$OUT/clocknative/ctl-rec/t"
control "clock native: an unreadable icon taken as not recording" "$OUT/clocknative/ctl-unknown/t"
control "clock native: the Clock drawn without a checked label"  "$OUT/clocknative/ctl-label/t"
control "clock native: a rect at the screen's edge dropped"      "$OUT/clocknative/ctl-origin/t"
control "browse edit: a duplicate column id in the width file accepted" "$OUT/bedit/ctl-dup/t"
control "browse edit: a divider grabbed one pixel outside the zone"    "$OUT/bedit/ctl-zone/t"
control "browse edit: the title column resized by a drag"              "$OUT/bedit/ctl-title/t"
control "browse edit: a drag below the narrowest width"                "$OUT/bedit/ctl-minw/t"
control "browse edit: saved widths never give way to the title"        "$OUT/bedit/ctl-giveway/t"
control "browse edit: a tap outside edit mode toggles instead of sorting" "$OUT/bedit/ctl-tap/t"
control "browse edit: a divider no touch can drag marked as a handle"   "$OUT/bedit/ctl-handles/t"
control "browse edit: two resizable neighbors do not trade width"     "$OUT/bedit/ctl-partner/t"
control "browse edit: a trade past the neighbor's narrowest width"   "$OUT/bedit/ctl-trademax/t"
control "fd tracking: the old code (open-only tracking)"   "$FD_DIR/ctl-old"
control "fd tracking: close does not forget"             "$FD_DIR/ctl-close"
control "fd tracking: dup2 does not forget"              "$FD_DIR/ctl-dup2"
control "fd tracking: the second deck bus in the first slot" "$FD_DIR/ctl-deck2"
control "fd tracking: a creating open loses its mode"      "$FD_DIR/ctl-mode"
control "fd tracking: dup3 does not forget"              "$FD_DIR/ctl-dup3"
control "fd tracking: open() does not forget a reused number" "$FD_DIR/ctl-note"
control "fd tracking: open() tracks outside EP147"       "$FD_DIR/ctl-inert"
control "png: CRC check removed"                          "$OUT/png/ctl-nocrc" "$OUT/png"
control "jog stems: Pad Info Display Off overridden"        "$OUT/jogstems/ctl-nosetting"
control "jog stems: Pad Info turned off (no waveform preview)" "$OUT/jogstems/ctl-artpage"
control "jog stems: a stem that is off shown as set"          "$OUT/jogstems/ctl-offset"
control "jog stems: EP147's own table not replaced"           "$OUT/jogstems/ctl-nosubst"
control "jog stems: EP147's table not sent back"              "$OUT/jogstems/ctl-norestore"
control "jog stems: the queue's vtable not checked"            "$OUT/jogstems/ctl-noviewvt"
control "jog stems: a default deck instead of the record's"    "$OUT/jogstems/ctl-deck0"
control "jog stems: the deck left set after the update"       "$OUT/jogstems/ctl-tlsleak"
control "jog stems: timing counts a repeated frame twice"     "$OUT/jogstems/ctl-xferseq"
control "jog stems: strip, an off stem drawn tall"           "$OUT/jogstems/ctl-stripoff"
control "jog stems: strip, EP147's strip not kept"           "$OUT/jogstems/ctl-stripkeep"
control "jog stems: strip, EP147's strip not put back"       "$OUT/jogstems/ctl-striprestore"
control "jog stems: strip, an unknown queue taken as deck 2" "$OUT/jogstems/ctl-stripdeck"
control "jog stems: strip, EP147's 1200 columns sent (145 pieces)" "$OUT/jogstems/ctl-stripfull"
control "icon wait: a queued decode not pending (placeholder flash)" "$OUT/iconwait/ctl-nowait"
control "icon wait: a decode that never finishes leaves the tile empty" "$OUT/iconwait/ctl-forever"
control "draw: 32-bit clip, unclamped mod values"            "$OUT/draw/ctl"
control "draw: text_width asks the face for any px"            "$OUT/draw/ctl-tw"
control "catalog: the old caps (16 repositories, 64 mods, 96 controls)" "$OUT/cmcat/ctl-oldcaps"
control "catalog: mods past the array dropped uncounted"   "$OUT/cmcat/ctl-nocount"
control "catalog: the \"Showing the first\" note never drawn" "$OUT/cmcat/ctl-silent"
control "catalog: every repository taken as listed"        "$OUT/cmcat/ctl-listed"
control "catalog: the same-id fields never read"           "$OUT/cmcat/ctl-noheld"
control "catalog: an installed id named from the other author's line" "$OUT/cmcat/ctl-anyname"
control "catalog: field 20 (updated) never read"            "$OUT/cmcat/ctl-noupd"
control "catalog: the trust note on every tab"              "$OUT/cmcat/ctl-noteall"
control "catalog: the breadcrumb without the tab"           "$OUT/cmcat/ctl-notab"
control "catalog: field 21 (downloads) never read"          "$OUT/cmcat/ctl-nodl"
control "catalog: a tile with no count shows 0"             "$OUT/cmcat/ctl-dlzero"
control "catalog: Browse opens on All mods"                 "$OUT/cmcat/ctl-nohome"
control "catalog: See all keeps the old order"              "$OUT/cmcat/ctl-seeall"
control "catalog: a shelf draws past the grid's edge"        "$OUT/cmcat/ctl-rowall"
control "catalog: the knob on Home scrolls nothing"         "$OUT/cmcat/ctl-homeknob"
control "catalog: the mod page's dot as UTF-8"              "$OUT/cmcat/ctl-dotutf8"
control "installed mod page: \"on\" read as \"loaded\" (a refused mod offered Enable)" "$OUT/cmmod/ctl-loaded"
control "not loaded: Reload resets the page to the question Load next time answered" "$OUT/cmnotloaded/ctl-reset"
control "not loaded: the notice on the Browse tab too"              "$OUT/cmnotloaded/ctl-alltabs"
control "not loaded: no Reload in the header after Load next time"  "$OUT/cmnotloaded/ctl-nobutton"
control "not loaded: the notice shrinks and the rows move"          "$OUT/cmnotloaded/ctl-resize"
control "not loaded: a row says Held back"                          "$OUT/cmnotloaded/ctl-oldrow"
control "not loaded: a line in the left-pane card"                  "$OUT/cmnotloaded/ctl-leftline"
control "not loaded: a crash reason shown as restarts"              "$OUT/cmnotloaded/ctl-crash"
control "not loaded: Keep off takes the notice away and the rows move" "$OUT/cmnotloaded/ctl-keepoff"
control "not loaded: Reload over 400 ms is taken back" "$OUT/cmnotloaded/ctl-reload400"
control "not loaded: Reload after Load next time sends nothing"     "$OUT/cmnotloaded/ctl-noreq"
control "card: the master switch sends a reload by itself"         "$OUT/cmnotloaded/ctl-togglereload"
control "card: a pressed Reload does not wait"                      "$OUT/cmnotloaded/ctl-nowait"
control "screen row: the row keeps its old behavior (no page)" "$OUT/screenrow/ctl-flash"
control "screen row: the row's own screen is ignored"          "$OUT/screenrow/ctl-ownscr"
control "screen row: the preview band stays empty"             "$OUT/screenrow/ctl-band"
control "menu scroll: the old scale-down (the left list divides by every entry)" "$OUT/menuscroll/ctl-shrink"
control "menu scroll: the old scale-down (the compact list's budget counts the mods' rows)" "$OUT/menuscroll/ctl-shrinkc"
control "menu scroll: the compact list's last row ends under the version footer" "$OUT/menuscroll/ctl-footer"
control "menu scroll: the knob does not scroll (the selection goes off-screen)" "$OUT/menuscroll/ctl-offscreen"
control "menu scroll: a drag still fires the control under it" "$OUT/menuscroll/ctl-dragtoggle"
control "menu scroll: a lift never flings" "$OUT/menuscroll/ctl-nofling"
control "menu scroll: a finger on a flinging list fires the control under it" "$OUT/menuscroll/ctl-nocatch"
control "menu scroll: another pane keeps the right pane's offset and glide" "$OUT/menuscroll/ctl-noput"
control "menu Reload: a press over 400 ms is taken back" "$OUT/menuscroll/ctl-reload400"
control "menu Reload: a press held a full second still restarts" "$OUT/menuscroll/ctl-reloadkeep"
control "icon cache: the ground color left out of the key" "$OUT/iconcache/ctl-noground"
control "icon cache: the theme left out of the key" "$OUT/iconcache/ctl-notheme"
control "icon cache: an icon over a pattern is kept" "$OUT/iconcache/ctl-anyground"
control "icon cache: the cache never answers" "$OUT/iconcache/ctl-nohit"
control "icon cache: an icon draws past its box" "$OUT/iconcache/ctl-spill"
control "overlay put clipping: a part cut on the left drawn at the window's edge" "$OUT/ovclip/ctl-nodx"
control "overlay put clipping: no cut at the right edge" "$OUT/ovclip/ctl-noright"
control "community touch: the home page keeps knob focus and fires on the press" "$OUT/cmtouch/ctl-knobfocus"
control "community touch: a drag does not scroll the page"   "$OUT/cmtouch/ctl-noscroll"
control "community touch: no scroll bar"                     "$OUT/cmtouch/ctl-nobar"
control "community orderly stop: every SIGTERM clears the count" "$OUT/cmorderly/ctl-always"
control "community orderly stop: the signal is not raised again" "$OUT/cmorderly/ctl-noraise"
control "community orderly stop: the first count seen is a heartbeat" "$OUT/cmorderly/ctl-firstseen"
control "community orderly stop: the default disposition, not the one before" "$OUT/cmorderly/ctl-norestore"
control "community touch: a drag eases behind the finger"   "$OUT/cmtouch/ctl-lag"
control "clock zone list: the row does not open the list"    "$OUT/clocktzlist/ctl-cycle"
control "clock zone list: the knob lands on headings"        "$OUT/clocktzlist/ctl-noskip"
control "clock zone list: the knob does not scroll the list" "$OUT/clocktzlist/ctl-noshow"
control "clock zone list: a drag picks the zone under it"    "$OUT/clocktzlist/ctl-dragpick"
control "clock zone list: a heading is picked as a zone"     "$OUT/clocktzlist/ctl-headpick"
control "clock zone list: the Clock row opens no screen"     "$OUT/clocktzlist/ctl-noscreen"
control "clock zone list: Set Time hands nothing over"       "$OUT/clocktzlist/ctl-setnoreq"
control "clock zone list: the time picker never edits"       "$OUT/clocktzlist/ctl-noedit"
control "community touch: a press on the header hits a control scrolled under it" "$OUT/cmtouch/ctl-header"
control "community touch: the knob scrolls past the ends"    "$OUT/cmtouch/ctl-noclamp"
control "community touch: a slow press on a view button switches it" "$OUT/cmtouch/ctl-slow"
control "community touch: a tap on a rail tab's top edge picks the tab above" "$OUT/cmtouch/ctl-railedge"
control "community touch: a set offset leaves the glide stuck"      "$OUT/cmtouch/ctl-staleglide"
control "community touch: the full screen draws the left list under it" "$OUT/cmtouch/ctl-drawleft"
control "stem_folder_rank: any 8-char suffix accepted"     "$OUT/stemrank/ctl-anyhex"
control "stem_folder_rank: prefix match accepted"          "$OUT/stemrank/ctl-prefix"
control "stem_folder_rank: case-sensitive"                 "$OUT/stemrank/ctl-case"
control "source.json fp: strtoull's own syntax accepted (0x, sign, space)" "$OUT/srcfp/ctl-strtoull"
control "verify.sh: a malformed manifest raises (the old traceback, exit 1)" ctl_detects verify_test "$ENTRY/verify-ctl.sh"
control "build-check.sh: no argument goes straight to cd (the old error)"  ctl_detects buildcheck_test "$ENTRY/buildcheck-ctl.sh"
control "rs_status_banner: the helper's marker ignored"   "$OUT/rsbanner/ctl-nomark"
control "rs_status_banner: line 1 not cut at the newline" "$OUT/rsbanner/ctl-nocut"
control "decks: one zero TrackID read as an empty deck"   "$OUT/decks/ctl-nodebounce"
control "decks: an unknown deck read as empty"           "$OUT/decks/ctl-unknown"
control "strip neutral: white strokes left white"     "$OUT/stripneutral/ctl-off"
control "drum feed: an empty deck never takes the master's BPM" "$OUT/drumfeed/ctl-nomaster"
control "drum feed: any unknown BPM takes the master's"     "$OUT/drumfeed/ctl-anyunknown"
control "drum feed: an unknown or unreadable master trusted" "$OUT/drumfeed/ctl-trust"
control "bpmstate: the use count never read (a disposed BpmState trusted)" "$OUT/bpmstate/ctl-nouse"
control "bpmstate: no weak reference taken (the block can be freed)"     "$OUT/bpmstate/ctl-noweak"
control "bpmstate: the control block's vtable not checked"               "$OUT/bpmstate/ctl-novt"
control "bpmstate: the object's +0x08 word not re-checked at read"       "$OUT/bpmstate/ctl-noid"
control "bpmstate: pins unbounded"                                       "$OUT/bpmstate/ctl-nocap"
control "lifetime: LoadState dtor hook unpublishes nothing"         "$OUT/lifetime/ctl-nolsdtor"
control "lifetime: LoadState magic not re-checked at use"           "$OUT/lifetime/ctl-nomagic"
control "lifetime: LoadState dtor does not wait for readers"        "$OUT/lifetime/ctl-nowait"
control "lifetime: LoadState ctor hook ignores the vtable"          "$OUT/lifetime/ctl-lsvt"
control "lifetime: DjPlayerUnit published before its ctor ran"      "$OUT/lifetime/ctl-djearly"
control "lifetime: DjPlayerUnit dtor hook clears nothing"           "$OUT/lifetime/ctl-djdtor"
control "lifetime: a track copy served whoever owns it"             "$OUT/lifetime/ctl-noowner"
control "lifetime: the FGPR from the ctor not type-checked"         "$OUT/lifetime/ctl-fgprvt"
control "ltisnap: stale generation (copy after teardown, served)"   "$OUT/lifetime/snap-ctl-nostale" 1.0
control "ltisnap: torn read (reader without the second look)"       "$OUT/lifetime/snap-ctl-norecheck" 1.0
control "ltisnap: lost update (snapshot writers not serialized)"    "$OUT/lifetime/snap-ctl-nolock" 1.0
control "sdk: beat phase read without the second look (torn grid)"  "$OUT/sdk/ctl-norecheck" 20.0
control "sdk: beat phase never asks the flusher for grids"          "$OUT/sdk/ctl-nodemand" 1.0
control "sdk: any playhead movement counts as playing"              "$OUT/sdk/ctl-anymove"
control "sdk: a deck's first known state fires an event"            "$OUT/sdk/ctl-nobase"
control "sdk: confirm left unanswered when its dialog goes away"   "$OUT/sdk/ctl-closed"
control "sdk: confirm: a click on Cancel answers yes"               "$OUT/sdk/ctl-clickyes"
control "sdk: confirm: BACK answers yes"                            "$OUT/sdk/ctl-backyes"
control "sdk: a second confirm overwrites the open one"             "$OUT/sdk/ctl-second"
control "sdk: UI queue producers not serialized"                    "$OUT/sdk/ctl-nocas"
control "sdk: thread_start takes any entry point"                   "$OUT/sdk/ctl-anyfn"
control "sdk: unknown pad modes guessed as HOT CUE"                 "$OUT/sdk/ctl-guess"
control "meowgrid: grid used without a reference"                  "$OUT/meowgrid/ctl-noref" 1.0
control "meowgrid: a dying grid taken back (count 0 acquired)"      "$OUT/meowgrid/ctl-resurrect" 1.0
control "meowgrid: the last release destroys nothing"               "$OUT/meowgrid/ctl-nodestroy" 1.0
control "meowgrid: LoadState teardown does not wait for readers"   "$OUT/meowgrid/ctl-nowait" 1.0
# readers is built on Linux only: off Linux its binaries do not exist, and a missing binary
# (exit 127) would count as a control that failed
if [ "$(uname -s)" = Linux ]; then
    control "readers: TaglistEditor destructor does not wait for readers"    "$OUT/readers/tc-ctl-nowait" 1.0
    control "readers: TaglistEditor destructor waits on the message thread" "$OUT/readers/tc-ctl-msgwait" 1.0
    control "readers: TaglistEditor destructor leaves it published"        "$OUT/readers/tc-ctl-nounpub" 1.0
    control "readers: TaglistEditor cached without the destructor hooks"   "$OUT/readers/tc-ctl-nodtorok" 1.0
    control "readers: mixer Channel destructor does not wait for readers"    "$OUT/readers/mixch-ctl-nowait" 1.0
    control "readers: mixer Channel destructor waits on the message thread" "$OUT/readers/mixch-ctl-msgwait" 1.0
    control "readers: mixer Channel destructor leaves it published"        "$OUT/readers/mixch-ctl-nounpub" 1.0
    control "readers: mixer Channel cached without the destructor hooks"   "$OUT/readers/mixch-ctl-nodtorok" 1.0
    control "readers: VinylSpeedAdjustor destructor does not wait for readers"    "$OUT/readers/vb-ctl-nowait" 1.0
    control "readers: VinylSpeedAdjustor destructor waits on the message thread" "$OUT/readers/vb-ctl-msgwait" 1.0
    control "readers: VinylSpeedAdjustor destructor leaves it published"        "$OUT/readers/vb-ctl-nounpub" 1.0
    control "readers: VinylSpeedAdjustor cached without the destructor hooks"   "$OUT/readers/vb-ctl-nodtorok" 1.0
fi
control "f4 identity: per-deck block's player index not re-checked"  "$OUT/f4ident/ctl-noword0"
control "f4 identity: PlayerInfoState disposal not seen"          "$OUT/f4ident/ctl-nouse"
control "f4 identity: control block vtable not re-checked at use"  "$OUT/f4ident/ctl-novtuse"
control "f4 identity: any make_shared block filed at capture"      "$OUT/f4ident/ctl-novtcap"
control "f4 identity: WWB widget vtable not checked at use"        "$OUT/f4ident/ctl-wwbnovt"
control "f4 identity: WWB widget used off the message thread"      "$OUT/f4ident/ctl-wwbanythread"
control "drum tile 8: the tile's vtable not checked"               "$OUT/drumtile8/ctl-notilevt"
control "drum tile 8: stem child not compared with the capture"    "$OUT/drumtile8/ctl-nocap"
control "drum tile 8: the unlatched (Stem FX) tile written anyway"  "$OUT/drumtile8/ctl-alwaysours"
control "drum tile 8: the component's vtable not checked"          "$OUT/drumtile8/ctl-nocompvt"
control "drum tile 8: an unknown deck falls back to deck 1"        "$OUT/drumtile8/ctl-anydeck"
control "drum tile 7: tile 7 left blank while armed"              "$OUT/drumtile7/ctl-blank"
control "Stem FX tiles: grown on every call"                     "$OUT/sfxtile/ctl-regrow"
control "Stem FX tiles: any array taken as grown"               "$OUT/sfxtile/ctl-taken"
control "Stem FX tiles: one image for every effect"             "$OUT/sfxtile/ctl-nofx"
control "Stem FX tiles: any count accepted"                     "$OUT/sfxtile/ctl-norange"
control "drum tile 7: HOLD on not shown"                          "$OUT/drumtile7/ctl-nohold"
control "drum tile 7: one deck's HOLD shown on every grid"        "$OUT/drumtile7/ctl-onedeck"
control "drum tile 7: the component's vtable not checked"         "$OUT/drumtile7/ctl-novt"
control "drum tile 7: any 20-entry array taken as ours"           "$OUT/drumtile7/ctl-notours"
control "drum tile 7: an unknown deck falls back to deck 1"       "$OUT/drumtile7/ctl-anydeck"
control "drum roll HOLD: pads 5 and 6 toggle it too"               "$OUT/drumhold/ctl-pad56"
control "drum roll HOLD: an unarmed deck holds"                     "$OUT/drumhold/ctl-noarm"
control "drum roll HOLD: pads 5 and 6 lit"                         "$OUT/drumhold/ctl-pad56lit"
control "drum roll HOLD: plays without a strip touch"               "$OUT/drumhold/ctl-nostrip"
control "drum roll HOLD: untouched frames set the rate"             "$OUT/drumhold/ctl-nolatch"
control "drum roll HOLD: one hold for every deck"                   "$OUT/drumhold/ctl-global"
control "drum roll HOLD: pad 7 lit steady, not flashing"            "$OUT/drumhold/ctl-noflash"
control "drum roll HOLD: another pad mode clears the hold"          "$OUT/drumhold/ctl-mode"
control "drum roll HOLD: HOLD off waits for a mixer frame"           "$OUT/drumhold/ctl-noframe"
control "drum roll HOLD: a deck select keeps the hold"              "$OUT/drumhold/ctl-curdeck"
control "drum roll HOLD: the mixer revives a cleared hold"          "$OUT/drumhold/ctl-revive"
control "beat jump jog: the jog shows EP147's page (no swap)"     "$OUT/bjjog/ctl-noswap"
control "beat jump jog: staging left swapped after the store"   "$OUT/bjjog/ctl-casewrite"
control "beat jump jog: what the store filled in is lost"       "$OUT/bjjog/ctl-nocopyback"
control "beat jump jog: PlayerInfoData vtable not checked"      "$OUT/bjjog/ctl-novt"
control "beat jump jog: any pointer taken as the staging area"  "$OUT/bjjog/ctl-nostaging"
control "beat jump jog: every layout variant swapped"           "$OUT/bjjog/ctl-novariant"
control "deck identity: tiles claimed in order of arrival"        "$OUT/deckid/ctl-arrival"
control "deck identity: an unknown container is deck 0"            "$OUT/deckid/ctl-default"
control "deck screen evidence: every getter caller counts"       "$OUT/deckscreen/ctl-anycaller"
control "deck screen evidence: the pad title's calls ignored"     "$OUT/deckscreen/ctl-notitle"
control "wave identity: an unknown provider bound by its point count" "$OUT/wavezoom/ctl-guess"
control "feed lock: a recorded entry called without validation" "$OUT/feedlock/ctl-novalid"
control "feed lock: the own timer runs the original while locked" "$OUT/feedlock/ctl-owntick"
control "feed lock: a stopped feed never dropped" "$OUT/feedlock/ctl-noidle"
control "feed lock: no own-timer fallback without frames" "$OUT/feedlock/ctl-nofallback"
if [ "${FL_TSAN:-0}" = 1 ]; then
    control "feed lock: a cross-thread switch read plainly (ThreadSanitizer)" env TSAN_OPTIONS=halt_on_error=1 "$OUT/feedlock/tsan-ctl-plain"
fi
control "wave identity: an unknown deck taken as deck 0"           "$OUT/wavezoom/ctl-deck0"
control "wave identity: one zoom model per color mode for all decks" "$OUT/wavezoom/ctl-onekey"
control "wave att: Stem Volume gains ignored (toggles only)"       "$OUT/waveatt/ctl-nogain/t"
control "wave att: a boosted stem above unity"                    "$OUT/waveatt/ctl-nocap/t"
control "wave att: every gain change rebuilds at once"            "$OUT/waveatt/ctl-norate/t"
control "wave att: deck 1's gains used for every deck"            "$OUT/waveatt/ctl-deck0/t"
control "wave att: the coarse 1/64 gain step"                     "$OUT/waveatt/ctl-coarse/t"
control "wave att: overview tiles refreshed on every gain rebuild" "$OUT/waveatt/ctl-ovevery/t"
control "wave att: one deck's change refreshes every deck's tiles" "$OUT/waveatt/ctl-ovall/t"
control "wave att: the envelope retry publishes a curve each tick" "$OUT/waveatt/ctl-retry/t"
control "wave att: the decode's progress rebuilds on every tick" "$OUT/waveatt/ctl-progress/t"
control "wave att: the frame count in the key" "$OUT/waveatt/ctl-framekey/t"
control "folder copy: \"..\" accepted as a name"                 "$OUT/foldercopy/ctl-dotdot/t"
control "folder copy: AppleDouble ._ files copied"                "$OUT/foldercopy/ctl-junk/t"
control "folder copy: a file of another size overwritten"         "$OUT/foldercopy/ctl-overwrite/t"
control "folder copy: a taken folder merged into"                 "$OUT/foldercopy/ctl-merge/t"
control "folder copy: no depth cap"                               "$OUT/foldercopy/ctl-depth/t"
control "folder copy: no room check in the walk"                  "$OUT/foldercopy/ctl-room/t"
F="guest/azmod"
control "folder copy: the re-read without the vtable check" fcopy_wiring "$OUT/foldercopy/ctl-noguard.c" $F/playlist_actions.c $F/menu_popup.c $F/playlist_editor.c
control "folder copy: PASTE not routed to the copy"         fcopy_wiring $F/folder_copy.c "$OUT/foldercopy/ctl-noroute.c" $F/menu_popup.c $F/playlist_editor.c
control "folder copy: UNDO on the FOLDER menu"              fcopy_wiring $F/folder_copy.c $F/playlist_actions.c "$OUT/foldercopy/ctl-undo.c" $F/playlist_editor.c
control "folder copy: the refresh never serviced"           fcopy_wiring $F/folder_copy.c $F/playlist_actions.c $F/menu_popup.c "$OUT/foldercopy/ctl-norefresh.c"
control "stem volume link: link players 3/4 ignored (the old rule)"  "$OUT/stemvol/ctl-nolink/t"
control "stem volume link: no member watch read as no players"       "$OUT/stemvol/ctl-nowatch/t"
control "stem volume link: player numbers off by one"                "$OUT/stemvol/ctl-off1/t"
control "stem volume link: ov_sync_stem_volume keeps the old rule"   stemvol_wiring "$OUT/stemvol/ctl-oldrule.c"
control "stem volume link: the member watch not installed"           stemvol_wiring "$OUT/stemvol/ctl-noinstall.c"
control "menu first button: HISTORY's ALL DELETE refused (the old gate)" "$OUT/mpgate/ctl-history/t"
control "menu first button: mp_click_hook refuses without the gate"      mpgate_wiring "$OUT/mpgate/ctl-nogate.c"
control "duplicate #define: a repeated definition is reported"    dupdefine_ctl
control "cm_ask: a second press retargets the open question" "$OUT/cmask/ctl-retarget"
control "cm_ask: the request is sent whatever the answer"   "$OUT/cmask/ctl-noyes"
control "cm_ask: a refused dialog still counts as open"     "$OUT/cmask/ctl-openany"
control "ls_tick: a leftover spool waits for the next copy" "$OUT/lsspool/ctl-gate" "$OUT/lsspool/root-ctl-gate"
control "ls_tick: the leftover never cleared"           "$OUT/lsspool/ctl-noclear" "$OUT/lsspool/root-ctl-noclear"
control "ls_tick: armed with nothing waiting"            "$OUT/lsspool/ctl-idle" "$OUT/lsspool/root-ctl-idle"
control "ls_source_hint: any path taken as the source"  "$OUT/lsspool/ctl-any" "$OUT/lsspool/root-ctl-any"
control "pdb_stock: a taken sync not cleared"            "$OUT/pdbstock/ctl-noclear"
control "pdb_stock: no debounce"                         "$OUT/pdbstock/ctl-nodebounce"
control "pdb_stock: stock writes never mirror (must mirror unconditionally)" "$OUT/pdbstock/ctl-nomirror"
control "pdb_stock: unresolved mount still synced"       "$OUT/pdbstock/ctl-nomount"
control "pdb_stock: media 0 noted"                       "$OUT/pdbstock/ctl-media0"
control "envelope cache: saved in place" "$OUT/envcache/ctl-inplace" "$OUT/envcache/scratch-ctl-inplace"
control "envelope cache: any frame count taken" "$OUT/envcache/ctl-anyframes" "$OUT/envcache/scratch-ctl-anyframes"
control "stem offset cache: another source size taken" "$OUT/ofscache/ctl-nosize" "$OUT/ofscache/scratch-ctl-nosize"
control "stem offset cache: another stem set taken" "$OUT/ofscache/ctl-nokey" "$OUT/ofscache/scratch-ctl-nokey"
control "stem offset cache: a lag past the window taken" "$OUT/ofscache/ctl-anylag" "$OUT/ofscache/scratch-ctl-anylag"
control "stem offset cache: text after the line taken" "$OUT/ofscache/ctl-trailing" "$OUT/ofscache/scratch-ctl-trailing"
control "stem offset cache: saved in place" "$OUT/ofscache/ctl-inplace" "$OUT/ofscache/scratch-ctl-inplace"
control "remote stems request: a title's line break adds a key line" "$OUT/rsreq/ctl-title" "$OUT/rsreq/scratch-ctl-title"
control "remote stems request: a split or cut path queued" "$OUT/rsreq/ctl-path" "$OUT/rsreq/scratch-ctl-path"
control "remote stems scan: any file in the queue counted as a request" "$OUT/rsscan/ctl-anyfile" "$OUT/rsscan/scratch-ctl-anyfile"
control "remote stems scan: taking the answer does not clear it" "$OUT/rsscan/ctl-noclear" "$OUT/rsscan/scratch-ctl-noclear"
control "remote stems scan: the loop scans while the feature is off" "$OUT/rsscan/ctl-ignoreon" "$OUT/rsscan/scratch-ctl-ignoreon"
control "remote stems scan: the loop never scans" "$OUT/rsscan/ctl-noscan" "$OUT/rsscan/scratch-ctl-noscan"
control "remote stems scan: run from the stem-marks worker" rsscan_home "$OUT/rsscan/ctl-guest"
if [ "$RSS_TSAN" = 1 ]; then
control "remote stems scan: a plain flag between the threads (TSan race)" env TSAN_OPTIONS=halt_on_error=1 "$OUT/rsscan/tsan-ctl-plain" "$OUT/rsscan/scratch-ctl-plain"
fi
control "touch guard: a failed read passed through" "$OUT/touchguard/ctl-noguard"
control "touch guard: every descriptor's failed read rewritten" "$OUT/touchguard/ctl-everyfd"
control "touch guard: a closed descriptor stays tracked" "$OUT/touchguard/ctl-noforget"
control "touch guard: a longer path counted as the reader" "$OUT/touchguard/ctl-prefix"
if [ "$TG_TSAN" = 1 ]; then
control "touch guard: a plain table between the threads (TSan race)" env TSAN_OPTIONS=halt_on_error=1 "$OUT/touchguard/tsan-ctl-plain"
fi
control "save logs request: written in place" "$OUT/savelogs/ctl-inplace" "$OUT/savelogs/scratch-ctl-inplace"
control "save logs request: an earlier press's result taken as this one's" "$OUT/savelogs/ctl-anyid" "$OUT/savelogs/scratch-ctl-anyid"
control "save logs request: no wait limit" "$OUT/savelogs/ctl-nowait" "$OUT/savelogs/scratch-ctl-nowait"
control "save logs caller: a request sent from the bus code" savelogs_caller "$OUT/savelogs/ctl-guest"
control "core dialog: serviced only while a mod runs" "$OUT/coretick/ctl-late"
control "elf gate spans: off + len wraps in 64 bits" "$OUT/elfspan/ctl-wrap"
control "elf gate spans: a string table without its NUL" "$OUT/elfspan/ctl-nul"
control "approved list: written without the lock" "$OUT/cmapprove/ctl-nolock"
control "approved list: memory ahead of the disk after a failed write" "$OUT/cmapprove/ctl-memory"
control "azmod.json: a repeated key accepted"   "$OUT/cmjson/ctl-repeat"
control "azmod.json: a nested object accepted"  "$OUT/cmjson/ctl-nested"
control "azmod.json: a repeat past the 48th key unseen" "$OUT/cmjson/ctl-many"
control "spool write: a short spool renamed into place" "$OUT/cpwrite/ctl-unchecked" "$OUT/cpwrite/scratch-ctl"
control "spool apply: any statement allowed" "$OUT/lsapply/ctl-anystmt" "$OUT/lsapply/scratch-ctl-anystmt"
control "spool apply: a table taken by its name's prefix" "$OUT/lsapply/ctl-prefix" "$OUT/lsapply/scratch-ctl-prefix"
control "spool apply: a second statement after ';' allowed" "$OUT/lsapply/ctl-inner" "$OUT/lsapply/scratch-ctl-inner"
control "spool apply: a failed spool commits what ran" "$OUT/lsapply/ctl-commit" "$OUT/lsapply/scratch-ctl-commit"
control "spool apply: applied without a backup" "$OUT/lsapply/ctl-nobackup" "$OUT/lsapply/scratch-ctl-nobackup"
control "spool apply: the trailing-blank trim ends pass 2 early" "$OUT/lsapply/ctl-trimnul" "$OUT/lsapply/scratch-ctl-trimnul"
control "spool apply: a 0x01 inside a line runs a statement pass 1 never saw" "$OUT/lsapply/ctl-mark" "$OUT/lsapply/scratch-ctl-mark"
control "spool apply: a NUL byte accepted, the rest of the file dropped" "$OUT/lsapply/ctl-nul" "$OUT/lsapply/scratch-ctl-nul"
control "cross-stick spool: a new list deduplicated (a repeated track dropped)" "$OUT/cpspool/ctl-dedupe"
control "cross-stick spool: a control byte in a title copied into the spool" "$OUT/cpspool/ctl-ctlbyte"
control "cross-stick folder copy: every row hung under the new folder" "$OUT/cpspool/fd-ctl-flat"
control "cross-stick folder copy: new playlist ids below 16385" "$OUT/cpspool/fd-ctl-nofloor"
control "cross-stick folder copy: the marks inside the copy left behind" "$OUT/cpspool/fd-ctl-onemark"
control "paste router: the other stick's row with the same id taken as itself" "$OUT/cpspool/fd-selfany"
control "cross-stick spool: no art lines (a reused track keeps the target's missing art)" "$OUT/cpspool/art-ctl-noartline"
control "cross-stick spool: art that is on the target replaced" "$OUT/cpspool/art-ctl-artover"
control "export.pdb sync: a failed copy handed to pdbs_sync as NULL" "$OUT/lspdbsync/ctl-nullrow"
control "export.pdb: no paste's-art step (a listed track keeps its missing art)" "$OUT/pdbart/ctl-noart"
control "export.pdb: a track's own art on the stick replaced by a paste's" "$OUT/pdbart/ctl-artover"
control "export.pdb: a copied track whose source row has no art gets none" "$OUT/pdbart/ctl-srcnoart"
control "export.pdb sync: a copied row keeps the source's analysis path" "$OUT/pdbsync2/ctl-anlz"
control "export.pdb sync: ill-formed UTF-8 encoded byte by byte"   "$OUT/pdbsync2/ctl-encbad"
control "export.pdb sync: an over-long name stops the sync"        "$OUT/pdbsync2/ctl-longabort"
control "export.pdb sync: rewritten in place (a pull tears it)"     "$OUT/pdbsync2/ctl-inplace"
control "export.pdb sync: a stale identical temporary file removed" "$OUT/pdbsync2/ctl-stale"
control "export.pdb sync: a missing export.pdb not put back"       "$OUT/pdbsync2/ctl-norecover"
control "export.pdb sync: the free list not rebuilt after a shrink" "$OUT/pdbsync2/ctl-freelist"
control "export.pdb sync: a surplus page left with 0 slots"         "$OUT/pdbsync2/ctl-tomb0"
if [ "$PT_TSAN" = 1 ]; then
control "export.pdb sync: two sticks at once without the lock (TSan race)" env TSAN_OPTIONS=halt_on_error=1 "$OUT/pdbthreads/tsan-ctl-nolock" 10
fi
control "utf8 cut: names cut byte-wise"                           "$OUT/utf8cut/ctl-bytewise"
control "drumroll: format code ignored"                 "$OUT/drum/ctl-tag" "$OUT/drum/scratch"
control "drumroll: any PCM bit depth accepted"           "$OUT/drum/ctl-depth" "$OUT/drum/scratch"
if [ "$PUBLIC" = 1 ]; then   # caught by the steady-roll case, which reads the development-only hit record
    skip "CONTROL NOT RUN (not counted as caught)  drumroll: the hit age ignored (a flam): the published tree has no hit record"
else
    control "drumroll: the hit age ignored (a flam)"     "$OUT/drum/ctl-hitage" "$OUT/drum/scratch"
fi
control "drumroll: decks 3/4 tone glide from zero"       "$OUT/drum/ctl-glide" "$OUT/drum/scratch"
control "drumroll: the bank reloaded with Drum Roll off" "$OUT/drum/ctl-bank0" "$OUT/drum/scratch"
control "drumroll: Beat Sync grid seqlock without its fences" bash $T/seqlock-lint.sh "$OUT/drum/ctl-gridfence.c"
control "drumroll: no length cap"                        "$OUT/drum/ctl-cap" "$OUT/drum/scratch"
control "drumroll: DRUM ROLLS resolved once per run"     "$OUT/drum/ctl-resolve" "$OUT/drum/scratch"
control "drumroll: bank swap does not wait for RT"       "$OUT/drum/ctl-quiesce" "$OUT/drum/scratch"
control "drumroll: Beat Sync trusted on a stopped deck"  "$OUT/drum/ctl-stall" "$OUT/drum/scratch"
control "drumroll: Beat Sync never used"                 "$OUT/drum/ctl-nosync" "$OUT/drum/scratch"
control "drumroll: grid rate assumed to be 44.1 kHz"     "$OUT/drum/ctl-norate" "$OUT/drum/scratch"
control "drumroll: grid rate never checked"              "$OUT/drum/ctl-anyrate" "$OUT/drum/scratch"
control "ogg decode: resample failure -> native buffer"  "$OUT/ogg/ctl-fallback"
control "ogg decode: max_frames ignored"                 "$OUT/ogg/ctl-nocap"
control "vinyl save: flush does not consume the slot"     "$OUT/vsave/ctl-noclear"
control "vinyl save: the oldest posted value wins"         "$OUT/vsave/ctl-oldest"
control "settings: azmod.cfg line 1 by item id, not legacy slot" "$OUT/cfg/ctl-byid" "$OUT/cfg/scratch-byid"
control "settings: azmod.cfg written in place (truncate + write)" "$OUT/cfg/ctl-inplace" "$OUT/cfg/scratch-inplace"
control "settings: a newer azmod.conf overwritten"         "$OUT/cfg/ctl-newer" "$OUT/cfg/scratch-newer"
control "settings: azmod.conf not the authority"            "$OUT/cfg/ctl-noauth" "$OUT/cfg/scratch-noauth"
control "settings: no rollback check (1.2.2's change lost)"  "$OUT/cfg/ctl-norollback" "$OUT/cfg/scratch-norollback"
control "settings: the small files' copies not refreshed"  "$OUT/cfg/ctl-stale" "$OUT/cfg/scratch-stale"
control "settings: the stick's azmods.cfg ignored"  "$OUT/cfg/ctl-noimport" "$OUT/cfg/scratch-noimport"
control "settings: a 2-way CHOICE migrated as on/off"    "$OUT/cfg/ctl-choiceonoff" "$OUT/cfg/scratch-choiceonoff"
control "settings: theme / vinyl.brake not saved in azmod.conf" "$OUT/cfg/ctl-nosmallsave" "$OUT/cfg/scratch-nosmallsave"
control "settings: theme / vinyl.brake not imported from the old files" "$OUT/cfg/ctl-nosmallimport" "$OUT/cfg/scratch-nosmallimport"
control "settings: the waveform zoom not saved in azmod.conf" "$OUT/cfg/ctl-nowzsave" "$OUT/cfg/scratch-nowzsave"
control "settings: the waveform zoom not loaded from azmod.conf" "$OUT/cfg/ctl-nowzload" "$OUT/cfg/scratch-nowzload"
control "bus settings: seqblk_read without the re-check"   "$OUT/buscfg/ctl-norecheck/t"
control "azmod.conf: duplicate keys, the first one wins"     "$OUT/azconf/ctl-firstwins" "$OUT/azconf/s1"
control "azmod.conf: version check dropped"                 "$OUT/azconf/ctl-noversion" "$OUT/azconf/s2"
control "azmod.conf: migration min width dropped"           "$OUT/azconf/ctl-minwidth" "$OUT/azconf/s3"
control "azmod.conf: drum bank = the last one on"           "$OUT/azconf/ctl-lastbank" "$OUT/azconf/s4"
control "azmod.conf: FPS -> OVERLAYS rule dropped"          "$OUT/azconf/ctl-nofps" "$OUT/azconf/s5"
control "azmod.conf: saved in place (no .new + rename)"     "$OUT/azconf/ctl-inplace" "$OUT/azconf/s6"
control "azmod.conf: save cap without the final newline"    "$OUT/azconf/ctl-savecap" "$OUT/azconf/s7"
control "azmod.conf: a short write renamed into place"        "$OUT/azconf/ctl-short" "$OUT/azconf/s8"
control "busread lint: read() reads g_state again"         "$T/busread-lint.sh" "$BL"
control "art targets: any .rodata address accepted"      "$OUT/art/ctl-anyaddr" ${ART_ARGS[@]+"${ART_ARGS[@]}"}
control "art targets: a blob longer than its image accepted" "$OUT/art/ctl-nofit" ${ART_ARGS[@]+"${ART_ARGS[@]}"}
control "art targets: the 1.30 table used for every build" "$OUT/art/ctl-anybuild" ${ART_ARGS[@]+"${ART_ARGS[@]}"}
if [ -f guest/azmod/test/community/run.sh ]; then
    control "community host: an enabled mod that is not approved loads"  cmhost "$CMH/ctl-noapprove"
    control "community host: a hook claim overlapping another is accepted" cmhost "$CMH/ctl-overlap"
    control "community host: claim_hook taken after init()"           cmhost "$CMH/ctl-lateclaim"
    control "community host: register_screen taken after init()"     cmhost "$CMH/ctl-latescreen"
    control "community host: a text setting cut inside a character" cmhost "$CMH/ctl-utf8"
    control "community host: a settings key with a control byte"   cmhost "$CMH/ctl-crkey"
fi
control "hook relocation: adrp always re-encoded (azmod)"   "$OUT/hook/ctl-anyreach"
control "hook relocation: adrp always re-encoded (azstem)"  "$OUT/hook/ctl-stem-anyreach"
control "hook relocation: reach bound off by one page"      "$OUT/hook/ctl-edge"
control "hook relocation: function-shape check dropped"     "$OUT/hook/ctl-shape"
control "hook relocation: a MOVK's shift off by one place (azstem)" "$OUT/hook/ctl-movk"
control "hook relocation: the top 16 bits never loaded (azmod)" "$OUT/hook/ctl-azmmovk"
control "azprot: fixed restore protection"                  "$OUT/azprot/ctl-fixed" "$OUT/azprot"
control "azprot: range spanning two protections accepted"   "$OUT/azprot/ctl-span" "$OUT/azprot"
control "azprot: hole inside the range ignored"             "$OUT/azprot/ctl-hole" "$OUT/azprot"
if [ "$PUBLIC" = 1 ]; then
    skip "hooks and crash chain controls (aarch64): need the development sources"
else
    control "hooks (aarch64): fixed restore protection"        hooktest "-include /ctl/ctl-fixed.h"
    control "hooks (aarch64): island B stored before the original" hooktest "-I/g/azmod -DHOOKTEST_INLINE_HOOKS=\"/ctl/inline_hooks-late-island.c\""
    control "hooks (aarch64): vtable slot stored before the original" hooktest "-I/g/azmod -DHOOKTEST_INLINE_HOOKS=\"/ctl/inline_hooks-late-slot.c\""
    control "hooks (aarch64): azstem stores the original after the jump" hooktest_stem /ctl/azstem-late-orig.c
    control "hooks (aarch64): 16-byte fallback written after the constructor" hooktest "-I/g/azmod -DHOOKTEST_INLINE_HOOKS=\"/ctl/inline_hooks-late-fallback.c\""
    control "hooks (aarch64): azstem installs after its constructor"      hooktest_stem /ctl/azstem-late-any.c
    control "crash chain (aarch64): the single-owner reporter (3a6b90d)" hooktest_crash /ctl/crash_guards-owner.c
    control "crash chain (aarch64): re-entry check removed"     hooktest_crash /ctl/crash_guards-noreentry.c
    control "crash chain (aarch64): a raised or sent signal not re-raised" hooktest_crash /ctl/crash_guards-noraise.c
    control "crash chain (aarch64): the blamed mod not named"   hooktest_crash /ctl/crash_guards-noblame.c
fi
control "redirect lint: a hand-written slot swap in day_mode.c" "$T/redirect-lint.sh" "$RL"
control "hook order lint: install_wavestems_hook back on the overlay thread" "$T/hookorder-lint.sh" "$HO/c1"
control "hook order lint: azm_hooks_go_late before install_static_hooks"     "$T/hookorder-lint.sh" "$HO/c2"
control "altstack (aarch64): azalt_this_thread installs nothing" alttest "-I/ctl/alt-none"
control "altstack (aarch64): a thread's stack never freed"      alttest "-I/ctl/alt-leak"
control "thread lint: a raw pthread_create in azstem.c"         "$T/thread-lint.sh" "$TL"
control "logcap: rotation never happens"                    "$OUT/logcap/ctl-norotate" "$OUT/logcap/scratch-ctl-norotate"
control "logcap: crash reporter writes without a byte cap"  "$OUT/logcap/ctl-nocap" "$OUT/logcap/scratch-ctl-nocap"
control "azrs: log() without the size check"               azrs_log_test "$OUT/logcap/azrs-ctl.sh"
control "azspawn: LD_PRELOAD inherited"                     "$OUT/azspawn/ctl-preload" "$OUT/azspawn/s-ctl-preload"
control "azspawn: QUIET ignored"                            "$OUT/azspawn/ctl-loud" "$OUT/azspawn/s-ctl-loud"
control "drag handoff: bus record not latched (drop then BACK both act)" "$OUT/draghandoff/ctl-nolatch" 20000
control "drag handoff: take does not clear (double execution)"   "$OUT/draghandoff/ctl-noclear" 20000
control "drag handoff: GRAB opens over an untaken end (lost)"    "$OUT/draghandoff/ctl-openfull" 20000
control "drag handoff: no generation (stale frame ends new drag)" "$OUT/draghandoff/ctl-nogen" 20000
control "drag handoff: local end always wins (both act)"         "$OUT/draghandoff/ctl-localany" 20000
if [ "$DH_TSAN" = 1 ]; then
control "drag handoff: record written after full (TSan race)"   env TSAN_OPTIONS=halt_on_error=1 "$OUT/draghandoff/tsan-ctl-early" 50000
fi
control "busthread-lint: drag_cancel() back in read()"           bash $T/busthread-lint.sh "$OUT/draghandoff/bi-ctl.c"
control "cmlayout-golden: the header divider one pixel taller"  env CC="$CC" bash $T/cmlayout-golden.sh "$CG" "$OUT/cmlayout/ctlout"
control "busread-trace: the watchdog threshold moved"          bash $T/busread-trace.sh "$BT" "$OUT/bustrace/ctlout"
control "busread-trace: mixer frames without a CRC reach filters" bash $T/busread-trace.sh "$BU" "$OUT/bustrace/ctlcrcout"
control "ssh approve: Cancel approves the key too"                "$OUT/sshapprove/ctl1"
control "ssh approve: the question interrupts a loaded deck"        "$OUT/sshapprove/ctl2"
control "ssh approve: a canceled key is asked about on every tick" "$OUT/sshapprove/ctl3"
control "sdk input: a passed touch is offered again mid-press"   "$OUT/sdkinput/ctl1"
control "sdk input: a slow filter is never switched off"         "$OUT/sdkinput/ctl2"
control "sdk input: the raw stage hands out the frame itself"    "$OUT/sdkinput/ctl3"
control "sdk input: a touch owner is called whatever its state"   "$OUT/sdkinput/ctl4"
control "busread-trace: a surface holds the knob throughout"     env BUSTRACE_HARNESS="$OUT/bustrace/focus_test.c" bash $T/busread-trace.sh guest "$OUT/bustrace/focusout"
control "sdk drawing: a 5th surface per mod allowed"              "$OUT/sdkdraw/ctl-limit/t"
control "sdk drawing: blend ignores the destination"            "$OUT/sdkdraw/ctl-blend/t"
control "sdk drawing: search outranks the deck view"            "$OUT/sdkdraw/ctl-view/t"
control "sdk drawing: LONG repeats for one press"               "$OUT/sdkdraw/ctl-long/t"
control "sdk drawing: a drag still sends LONG"                  "$OUT/sdkdraw/ctl-draglong/t"
control "sdk drawing: drops miscounted"                         "$OUT/sdkdraw/ctl-drop/t"
control "sdk drawing: xwin_show forces a size it was not given"  "$OUT/sdkdraw/ctl-xwinsize/t"
control "sdk drawing: blend clips in 32 bits (wraps near INT_MAX)" "$OUT/sdkdraw/ctl-blendwrap/t"
control "sdk drawing: xwin_show admits a window the mod did not launch" "$OUT/sdkdraw/ctl-xwinowned/t"
control "bright grid: the old rule brightens the ruler background" "$OUT/brightgrid/ctl-oldrule/t"
control "bright grid: the ticks rounded differently from before"  "$OUT/brightgrid/ctl-ticks/t"
control "bright grid cfg: an unchanged-file check that never re-reads" "$OUT/brightcfg/ctl-once" "$OUT/brightcfg/scratch"
control "bright grid cfg: read only by the rates log (the old wiring)" brightcfg_wiring "$OUT/brightcfg/ctl-logs.c"
control "bright grid cfg: the rates log reads the file as well" brightcfg_wiring "$OUT/brightcfg/ctl-rates.c"
control "audio tap: the producer overwrites unread frames"    "$OUT/aztap/ctl-overwrite"
control "audio tap: stores while nobody listens"             "$OUT/aztap/ctl-nowant"
control "hide title: toggles on the press"                     "$OUT/htgest/ctl-press"
control "hide title: a drag counts as a tap"                   "$OUT/htgest/ctl-slop"
control "hide title: a resting finger counts as a tap"         "$OUT/htgest/ctl-hold"
control "hide title: a second finger is ignored"               "$OUT/htgest/ctl-multi"
control "hide title: a finger already down is ignored"         "$OUT/htgest/ctl-held"
control "hide title: a press at each step of a drag re-anchors it" "$OUT/htgest/ctl-repeat"
control "clock timer: the seconds round down"                 "$OUT/clocktimer/ctl-rounddown" 2000
control "clock timer: the last minute is steady red"           "$OUT/clocktimer/ctl-steady" 2000
control "clock timer: no 0:00 at the end"                      "$OUT/clocktimer/ctl-nodone" 2000
control "clock timer: Close starts the timer"                  "$OUT/clocktimer/ctl-closeruns" 2000
control "clock timer: an add while running only sets"          "$OUT/clocktimer/ctl-adddraft" 2000
control "clock timer: the duration is not capped"              "$OUT/clocktimer/ctl-nocap" 2000
control "clock timer: a button's hit rect is one px too big"   "$OUT/clocktimer/ctl-edge" 2000
if [ "$CT_TSAN" = 1 ]; then
control "clock timer: a plain store of the deadline (TSan race)" env TSAN_OPTIONS=halt_on_error=1 "$OUT/clocktimer/tsan-ctl-plain" 200000
fi
control "font size: any FontManager gets the saved size"        "$OUT/fontsize/ctl-anyctor"
control "font size: a refused size is saved"                     "$OUT/fontsize/ctl-badsize"
control "font size: an out-of-range saved size is applied"        "$OUT/fontsize/ctl-range"
control "font size: the same size is written again"               "$OUT/fontsize/ctl-rewrite"
control "font size: applied with the kill switch"                "$OUT/fontsize/ctl-rowoff"
control "font size: saved from a view whose FontManager is not one" "$OUT/fontsize/ctl-anyfm"
control "font size: a selector moved on an object of another class" "$OUT/fontsize/ctl-anybar"
control "streaming key: row on, a classic request is passed on as it is" "$OUT/skey/ctl-noforce"
control "streaming key: an id past the table is asked for in Camelot" "$OUT/skey/ctl-range"
control "streaming key: the string is not written in the caller's place" "$OUT/skey/ctl-x8"
control "streaming key: installed with the kill switch"           "$OUT/skey/ctl-nokill"
control "streaming key: Camelot with the row off"                 "$OUT/skey/ctl-norow"
control "camelot text: an out-of-table pitch class gives 1"  "$OUT/camelot/ctl-default"
control "camelot text: major and minor letters swapped"     "$OUT/camelot/ctl-suffix"
control "camelot text: the table indexed by the pitch class" "$OUT/camelot/ctl-index"
control "streaming key: no self-test after the row changes"       "$OUT/skey/ctl-retest"
control "hot cues: the PCOB lists win over the colored PCO2 list"  "$OUT/hcanlz/ctl-nopco2"
control "hot cues: a section longer than the file is read"        "$OUT/hcanlz/ctl-seclen"
control "hot cues: an entry longer than its section is read"      "$OUT/hcanlz/ctl-entrylen"
control "hot cues: the color is read before the comment"          "$OUT/hcanlz/ctl-comment"
control "hot cues: a cue past the track's end is drawn"           "$OUT/hcanlz/ctl-pastend"
control "hot cues: the cue-section reader skips the colored PCO2 list" "$OUT/hcanlz/ctl-pickco2"
control "worker order: hot cues wait for every playlist row"       "$OUT/smorder/ctl-phlast"
control "worker order: a playlist scan's step skips the hot cues"     "$OUT/smorder/ctl-stepnohc"
control "worker order: the oldest hot cue request first"             "$OUT/smorder/ctl-fifo"
control "worker order: a full hot cue queue drops the new row"        "$OUT/smorder/ctl-noevict"
control "spin back: a pre-roll of any value is replaced"          "$OUT/spinback/ctl-anyvalue"
control "spin back: a core of any class gets the cap"             "$OUT/spinback/ctl-anycore"
control "spin back: the cap is set after the clamp ran"           "$OUT/spinback/ctl-after"
control "spin back: row off never restores the stock 2 s"         "$OUT/spinback/ctl-nooff"
control "spin back: a misaligned core is used"                    "$OUT/spinback/ctl-noalign"
control "beat loop flip: a deck without stems flips azmod's page" "$OUT/blflip/ctl-anydeck"
control "beat loop flip: the side's deck index read for the logical deck" "$OUT/blflip/ctl-sidedeck"
control "beat loop flip: flips whatever EP147's mode"                 "$OUT/blflip/ctl-anymode"
control "stem fx: post-press audio repeats"                   "$OUT/stemfx/ctl-posttaps"
control "stem fx: a half-written press read"                  "$OUT/stemfx/ctl-noseq"
control "stem fx: a latch without its press's tag"            "$OUT/stemfx/ctl-notag"
control "stem fx: no fade back in after the release"          "$OUT/stemfx/ctl-nooff"
if [ "$SFX_TSAN" = 1 ]; then
control "stem fx: a plain field between the threads (TSan race)" env TSAN_OPTIONS=halt_on_error=1 "$OUT/stemfx/tsan-ctl-plain"
fi
control "stem fx pads: tap and hold swapped"                  "$OUT/sfxpads/ctl-holdflip"
control "stem fx pads: release sent to the deck shown now"    "$OUT/sfxpads/ctl-curdeck"
control "stem fx pads: the release of an off press acts"      "$OUT/sfxpads/ctl-offrelease"
control "stem fx pads: a refused press is remembered"         "$OUT/sfxpads/ctl-refused"
control "browse columns: a compact pane's columns changed too"           "$OUT/bcols/ctl-compact"
control "browse columns: every column set at each layout, defaults too"   "$OUT/bcols/ctl-all"
control "browse columns: only columns off EP147's TRACK default applied" "$OUT/bcols/ctl-unset"
control "browse columns: the columns applied with Column Edit off"       "$OUT/bcols/ctl-ungated"
control "browse columns: Column Edit off keeps the DJ's columns"         "$OUT/bcols/ctl-noback"
control "browse columns: our own column changes taken as EP147's"        "$OUT/bcols/ctl-recself"
control "browse columns: a destroyed header's record kept"               "$OUT/bcols/ctl-gone"
control "browse columns: the sub column taken as restored before an ask"  "$OUT/bcols/ctl-early"
control "browse columns: PREVIEW saved from any object"                   "$OUT/bcols/ctl-pvany"
control "browse columns: every sort row offered, Column Edit off too"  "$OUT/bcols/ctl-afon"
control "browse columns: an offered hidden row keeps EP147's order"    "$OUT/bcols/ctl-afseq"
control "browse columns: BPM is column 6 on every build"                  "$OUT/bcols/ctl-ids"
control "browse columns: the stem marks' column is the leftmost visible"  "$OUT/bcols/ctl-smasc"
control "browse fields: an id given twice in the saved file accepted"   "$OUT/bfields/ctl-dup/t"
control "browse fields: an overlong UTF-8 form kept as valid"           "$OUT/bfields/ctl-overlong/t"
control "browse fields: Track set as a column's field"                  "$OUT/bfields/ctl-track/t"
control "browse fields: a stale pick taken for a column"                "$OUT/bfields/ctl-stale/t"
control "browse fields: EP147's own button leaves the column's ask"     "$OUT/bfields/ctl-ownclick/t"
control "browse fields: the row's sub item not restored after a paint"  "$OUT/bfields/ctl-restore/t"
control "browse fields: a compact list drawn with a field"              "$OUT/bfields/ctl-compact/t"
control "browse fields: column 1 records the selected row"              "$OUT/bfields/ctl-sel1/t"
control "browse fields: a saved id that cannot show a field loaded"     "$OUT/bfields/ctl-load/t"
control "browse fields: Preview's own paint replaced with no field"     "$OUT/bfields/ctl-wave/t"
control "browse fields: two sticks that disagree read as known"         "$OUT/bfields/ctl-disagree/t"
control "browse fields: a field button over a divider's zone"           "$OUT/bfields/ctl-inset/t"
control "browse fields: a narrow column gets no button"                 "$OUT/bfields/ctl-narrow/t"
control "browse fields: a button covers its right neighbour"            "$OUT/bfields/ctl-cover/t"
control "browse fields: a button past the header's left edge"           "$OUT/bfields/ctl-left0/t"
control "browse fields: a press on EP147's own button taken"            "$OUT/bfields/ctl-ownfwd/t"
control "browse fields: a moved touch on a button opens its list"       "$OUT/bfields/ctl-tapmove/t"
control "browse fields: a divider's drag ending on a button opens it"   "$OUT/bfields/ctl-dragtap/t"
control "browse fields: a drag after a pause read as a new touch"       "$OUT/bfields/ctl-pause/t"
control "browse fields: EP147's own button taken outside edit mode"     "$OUT/bfields/ctl-owngate/t"
control "browse fields: a compact list's title drawn with a field"      "$OUT/bfields/ctl-tlfull/t"
control "browse fields: another label's text drawn as the field"        "$OUT/bfields/ctl-tlparent/t"
control "browse fields: the title's button over EP147's own"            "$OUT/bfields/ctl-tlclamp/t"
control "browse fields: a button left looking pressed after a lift"     "$OUT/bfields/ctl-look/t"
control "browse fields: a still release does not open the list"         "$OUT/bfields/ctl-still/t"
control "browse fields: a second list move stacks on the first"         "$OUT/bfields/ctl-stack/t"
control "browse fields: EP147's own click leaves the list moved"        "$OUT/bfields/ctl-norestore/t"
control "browse fields: the list moved past the header's left edge"     "$OUT/bfields/ctl-popleft/t"
control "browse fields: a drag off a button's left edge dropped"       "$OUT/bfields/ctl-negx/t"
control "browse fields: the moved list selects the wrong row"           "$OUT/bfields/ctl-poprow/t"
control "browse fields: the preview still plays with a field"           "$OUT/bfields/ctl-preview/t"
control "browse fields: the event keeps the waveform as its component"  "$OUT/bfields/ctl-evcomp/t"
control "browse fields: the tap lands on Preview's own column"          "$OUT/bfields/ctl-evcol/t"
control "browse fields: fields shown with Column Edit off"              "$OUT/bfields/ctl-ungated/t"
control "browse fields: Hide Column left in EP147's own list"           "$OUT/bfields/ctl-hidekeep/t"
control "browse fields: Hide Column offered on Preview and the title"  "$OUT/bfields/ctl-hideany/t"
control "browse fields: the Hide Column row's index passed to EP147"   "$OUT/bfields/ctl-hidepass/t"
control "browse fields: a field picked for a hidden column, still hidden" "$OUT/bfields/ctl-unhide/t"
control "browse fields: a row EP147 rebuilt taken as Hide Column"       "$OUT/bfields/ctl-hidestale/t"
control "browse fields: the popup's component taken as the popup object" "$OUT/bfields/ctl-hideott/t"
control "browse fields: Hide Column added only after EP147 shows the list" "$OUT/bfields/ctl-hidelate/t"
control "hide title: a press on a popup over the bar counts"    "$OUT/htcover/ctl-any"
control "hide title: a press on the title's label does not count" "$OUT/htcover/ctl-direct"
control "hide title: a press on one bar counts for every deck"  "$OUT/htcover/ctl-every"
control "key in title: the key ignores the eye and runs under it" "$OUT/ktlayout/ctl-noeye"
control "key in title: the key's box starts under the title"     "$OUT/ktlayout/ctl-under"
control "key in title: a hidden Label's placeholder is shown"    "$OUT/ktlayout/ctl-hidden"
control "key in title: a copy no update refreshes is shown forever" "$OUT/ktlayout/ctl-stale"
control "key in title: the key inherits Hide Track Title's switch" "$OUT/ktlayout/ctl-inherit"
control "key in title: a key tap needs Hide Track Title's switch" "$OUT/ktlayout/ctl-tapinherit"
control "key in title: a tap anywhere on the bar opens the panel" "$OUT/ktlayout/ctl-wholebar"
control "key in title: the panel may run off the screen's right edge" "$OUT/ktlayout/ctl-offscreen"
control "community browse: an unknown category taken as Visual"    "$OUT/cmbrowse/ctl-unknown"
control "community browse: any digits taken as a date"             "$OUT/cmbrowse/ctl-date"
control "community browse: ties not kept in catalog order"         "$OUT/cmbrowse/ctl-unstable"
control "community browse: undated mods first"                     "$OUT/cmbrowse/ctl-undated"
control "community browse: mods that cannot install here mixed in" "$OUT/cmbrowse/ctl-usable"
control "community browse: Installed ignores installs"             "$OUT/cmbrowse/ctl-installed"
control "community browse: numbers in names compared as text"      "$OUT/cmbrowse/ctl-digits"
control "community browse: a count of 14 digits taken"             "$OUT/cmbrowse/ctl-dl14"
control "community browse: Downloads puts the fewest first"       "$OUT/cmbrowse/ctl-dlorder"
control "community browse: mods with no count before counted ones" "$OUT/cmbrowse/ctl-uncounted"
control "community browse: a shelf of one mod shown"              "$OUT/cmbrowse/ctl-shelfmin"
control "community browse: 0 downloads on Most downloaded"        "$OUT/cmbrowse/ctl-topzero"
control "community browse: a count with no commas"                "$OUT/cmbrowse/ctl-comma"
control "community updates: the next install sent before the last is done" "$OUT/cmupd/ctl-nowait"
control "community updates: the next sent before the catalog is read again" "$OUT/cmupd/ctl-nocat"
control "community updates: an error does not stop the run"          "$OUT/cmupd/ctl-noerr"
control "community updates: a mod that stopped qualifying is sent"   "$OUT/cmupd/ctl-noskip"
control "community updates: an install sent while the fetcher is busy" "$OUT/cmupd/ctl-busy"
control "community updates: a done from before the send taken for it" "$OUT/cmupd/ctl-stale"
control "community updates: the button counts from 0"                "$OUT/cmupd/ctl-count"
control "updates tab: a mod removed this start listed"            "$OUT/cmupdtab/ctl-removed"
control "updates tab: the rail's tab without its count"          "$OUT/cmupdtab/ctl-railcount"
control "updates tab: other requests sent while Update all runs" "$OUT/cmupdtab/ctl-noblock"
control "updates tab: a reload leaves the run going"             "$OUT/cmupdtab/ctl-noreload"
control "updates tab: the run moves only with the menu drawn"    "$OUT/cmupdtab/ctl-nobg"
control "updates tab: the Reload note between two installs"     "$OUT/cmupdtab/ctl-reloadmid"
control "installed rows: a touch on the checkbox opens the page"   "$OUT/cmupdtab/ctl-chkopens"
control "installed rows: the knob's press toggles the checkbox"   "$OUT/cmupdtab/ctl-knobchk"
control "updates tab: the header's count drawn under its band"  "$OUT/cmupdtab/ctl-countunder"
control "installed split: a row opens the full page"            "$OUT/cmupdtab/ctl-splitopens"
control "installed rows: a second toggle sent while the first is unanswered" "$OUT/cmupdtab/ctl-noflight"
control "community: every opening refreshes"                        "$OUT/cmupdtab/ctl-autoeach"
control "community: a failed refresh is not retried on the next opening" "$OUT/cmupdtab/ctl-autonoretry"
control "community browse: a clock that is not set counts days"   "$OUT/cmbrowse/ctl-clock"
control "input memory: a name inside a longer name is matched" "$OUT/inputmem/ctl-boundary"
control "input memory: a string entry read as an integer"     "$OUT/inputmem/ctl-type"
control "input memory: applied before EP147's own apply"      "$OUT/inputmem/ctl-settle"
control "input memory: the DJ's own choice is overridden"     "$OUT/inputmem/ctl-override"
control "input memory: applied with Memory off"               "$OUT/inputmem/ctl-memory"
control "scroll glide: a knob step jumps to the row"             "$OUT/glide/ctl-nofollow"
control "scroll glide: a drag eases toward the finger (lags it)"   "$OUT/glide/ctl-ease"
control "scroll glide: a fling slows slower than EP147's"         "$OUT/glide/ctl-decay"
control "scroll glide: a step's speed without EP147's 5 ms floor" "$OUT/glide/ctl-floor"
control "scroll glide: a fling runs on below EP147's stop speed"  "$OUT/glide/ctl-stop"
control "scroll glide: a moving lift does not fling"               "$OUT/glide/ctl-nofling"
control "scroll glide: a fling runs past the end"                  "$OUT/glide/ctl-noend"
control "scroll glide: a finger that stopped still flings"         "$OUT/glide/ctl-stillfling"
control "scroll glide: a finger does not catch a fling"            "$OUT/glide/ctl-nocatch"
control "mm resume: SOURCE from another view forgets the menu"  "$OUT/mmresume/ctl-srcclears"
control "mm resume: a weaker sample lowers the level"          "$OUT/mmresume/ctl-lower"
control "mm resume: a real source picked still reopens"        "$OUT/mmresume/ctl-nowant"
control "mm resume: SOURCE shown leaves the level for later"   "$OUT/mmresume/ctl-notake"
control "mm resume: every open brings the old page back"      "$OUT/mmresume/ctl-pagealways"
control "mm resume: a close of the closed menu forgets the page" "$OUT/mmresume/ctl-pagekeep"
control "sdk audio_tap: every deck reads deck 1's channels"  "$OUT/sdkaudio/ctl-pair"
control "sdk audio_tap: a closed stream's address still read" "$OUT/sdkaudio/ctl-noclose"
control "sdk library: the connection opens read-write"       "$OUT/sdklib/ctl-rw"
control "sdk library: reads on the UI thread"                 "$OUT/sdklib/ctl-ui"
control "sdk library: any port's library taken for a stick"   "$OUT/sdklib/ctl-anyport"
control "sdk library: two libraries in one port, one guessed" "$OUT/sdklib/ctl-guess"
control "sdk library: spool kick while a deck plays from that USB" "$OUT/sdklib/ctl-kickstick"
control "sdk library: spool kick with a playing deck's USB unknown" "$OUT/sdklib/ctl-kickunknown"
control "sdk library: spool kick while another played USB has a spool" "$OUT/sdklib/ctl-kickother"
control "sdk library: a list cut at the cap reported as complete" "$OUT/sdklib/ctl-trunc"
control "handoff guard: a frame with a bad CRC counts as BACK"   "$OUT/hguard/ctl-nocrc/t"
control "handoff guard: a BACK hold survives a gap in the stream" "$OUT/hguard/ctl-nogap/t"
control "handoff guard: the restore neither retries nor reboots" "$OUT/hguard/ctl-noretry/t"
control "sdk audio_tap: no per-mod limit"                     "$OUT/sdkaudio/ctl-permod"
control "sdk audio_tap: a re-configured stream still read"    "$OUT/sdkaudio/ctl-reconf"
control "sdk audio_tap: callbacks run under the subscribers' lock" "$OUT/sdkaudio/ctl-cblock"
control "deck slot: a unit with no number takes the first free slot" "$OUT/deckslot/ctl-arrival"
control "deck slot: a held slot taken over by a second unit"         "$OUT/deckslot/ctl-takeover"
control "sdk row_set: another mod's row set" "$OUT/sdkrows/ctl-anyowner"
control "sdk deck_info: the whole struct written whatever the caller's size" "$OUT/sdkdeck/ctl-size"
control "sdk deck_info: a struct without room for loaded taken"        "$OUT/sdkdeck/ctl-min"
control "sdk launch: the log is not capped"                   "$SDKD/sdkproc/ctl-nocap"
control "sdk launch: a log named by a link out of the data folder accepted" "$SDKD/sdkproc/ctl-logname"
control "sdk launch: the log opened through a link"           "$SDKD/sdkproc/ctl-logfollow"
control "sdk launch: any number of arguments"                "$SDKD/sdkproc/ctl-argmax"
control "sdk handoff: a second one while one is in progress"  "$SDKD/sdkproc/ctl-busy"
control "sdk launch: a failed spawn leaves its log pipe open" "$SDKD/sdkproc/ctl-pipeleak"
control "sdk launch: any path accepted (no folder check)"    "$SDKD/sdkproc/ctl-anypath"
control "sdk launch: LD_PRELOAD passed to the child"         "$SDKD/sdkproc/ctl-preload"
control "sdk launch: EP147's descriptors passed to the child"   "$SDKD/sdkproc/ctl-fdleak"
control "sdk launch: any mod may ask about or stop any pid"  "$SDKD/sdkproc/ctl-anyowner"
control "sdk handoff: stops EP147 while a deck plays"      "$SDKD/sdkproc/ctl-playing"
control "sdk handoff: the guard on the deck is not used"     "$SDKD/sdkproc/ctl-noguard"
control "sdk claim_vslot: a slot not holding EP147 code"     "$SDKD/sdkvslot/ctl-anyfn"
control "sdk claim_vslot: a slot claimed twice"              "$SDKD/sdkvslot/ctl-noowner"
control "menu dump: one default flipped in features.def (Remote Stems off)" $T/menudump-check.sh "$OUT/menudump/ctl-rel" $T/menudump.golden-rel.gz "$OUT/menudump/s3/azmod.cfg"
control "build warnings: AZ_WARN without -Werror"           werrortest -f out/unit-werrorctl/Makefile
control "menu dump: the old code's own golden (without the intended changes)" $T/menudump-check.sh "$OUT/menudump/real-rel" $T/menudump.golden-rel.orig.gz "$OUT/menudump/s4/azmod.cfg"
control "60 FPS group: 1.2.2's logic (the dark-empty round trip)" "$OUT/fps60/old"
control "registered rows: listed after the utilities"      "$OUT/rows/ctl-order/t"
control "registered rows: row_set accepts an official row" "$OUT/rows/ctl-set/t"
control "registered rows: a child under an official row"   "$OUT/rows/ctl-parent/t"
control "registered rows: the icon not copied"            "$OUT/rows/ctl-iconref/t"
control "registered rows: the icon never drawn"           "$OUT/rows/ctl-icondraw/t"
control "registered rows: a stopped mod's rows are not retired" "$OUT/rows/ctl-retire/t"
control "Mod Menu keys: a key renamed in features.def only"  "$OUT/keys/ctl/t" "$KEYS_MD"
control "trackgen: eject does not bump the generation"      "$OUT/trackgen/ctl-eject"
control "trackgen: bump after the curtrack publish"         "$OUT/trackgen/ctl-order"
control "trackgen: no stems base skips the identity"       "$OUT/trackgen/ctl-nobase"
control "trackgen: no load once a base turns up"           "$OUT/trackgen/ctl-deferred"
if [ "$PUBLIC" = 1 ]; then
    skip "addrgen controls: generated from notes the published tree does not carry"
else
    control "addrgen: an EP_ use removed, table not regenerated"  python3 "$AG/ctl-use/tools/ep147-addrgen.py" --check
    control "addrgen: a v2.00 value changed, table not regenerated" python3 "$AG/ctl-value/tools/ep147-addrgen.py" --check
fi
control "fuzz replay: a planted off-by-one in cm_json_strat" "$FZ/ctl-strat/replay" tools/fuzz/corpus/cmjson
control "fuzz replay: ps_page's page index wraps in 32 bits (pdb_sync.c)" "$FZ/rev-pdbpage/replay" tools/fuzz/corpus/pdbsync
control "fuzz replay: ps_rows takes any slot count (pdb_sync.c)" "$FZ/rev-pdbrows/replay" tools/fuzz/corpus/pdbsync
control "fuzz replay: a source string offset past its page (pdb_sync.c)" "$FZ/rev-pdbsrcstr/replay" tools/fuzz/corpus/pdbsync
control "fuzz replay: ps_str trusts a short string's length (pdb_sync.c)" "$FZ/rev-pdbstr/replay" tools/fuzz/corpus/pdbsync
control "fuzz replay: cm_elf_check reads misaligned section headers (community.c)" "$FZ/rev-elfalign/replay" tools/fuzz/corpus/elfcheck
control "fuzz replay: cmb_date_ok takes a date with more after it (cm_browse.h)" "$FZ/rev-datelen/replay" tools/fuzz/corpus/cmbrowse
control "fuzz replay: json_str copies a \\u escape past the end (stem_playback.c)" "$FZ/rev-jsonu/replay" tools/fuzz/corpus/stemname
control "fuzz replay: b2_fields_parse takes a column id twice (browse_fields.h)" "$FZ/rev-colfdup/replay" tools/fuzz/corpus/colfields
control "fuzz run.sh: a harness that does not build, --replay one" fz_closed "$FZ/ctl-closed-build" "fuzz replay png: build failed" 0 --replay png
control "fuzz run.sh: a harness that does not build, --replay all still reports the rest" fz_closed "$FZ/ctl-closed-build" "fuzz replay png: build failed" "$FZ_ALL_OTHERS" --replay all
control "fuzz run.sh: a harness whose source cannot be extracted, --replay one" fz_closed "$FZ/ctl-closed-extract" "fuzz replay png: could not prepare" 0 --replay png
control "fuzz run.sh: a harness whose source cannot be extracted, --replay all" fz_closed "$FZ/ctl-closed-extract" "fuzz replay png: could not prepare" "$FZ_ALL_OTHERS" --replay all
[ "$FZ_LIB" = 0 ] || control "fuzz run.sh: a harness that does not build, fuzz mode" fz_closed "$FZ/ctl-closed-build" "fuzz png: build failed" 0 png 1
[ "$FZ_LIB" = 0 ] || control "fuzz: the planted off-by-one found from a harmless seed (120 s cap)" fz_planted_found
echo

# a sanitizer pass proves something only about binaries it instrumented: every ELF executable the
# pass built must carry the sanitizer's runtime
if [ -n "$SAN" ]; then
    echo "=== sanitizer coverage: every executable this pass built is instrumented ==="
    want=__${SAN}_init; nbin=0; nplain=0
    while IFS= read -r f; do
        [ "$(head -c 4 "$f" | od -An -c | tr -d ' ')" = 177ELF ] || continue
        nbin=$((nbin + 1))
        grep -q "$want" "$f" || { nplain=$((nplain + 1)); echo "      not instrumented: $f"; }
    done < <(find "$OUT" -path "$OUT/san" -prune -o -type f -perm -u+x -print)
    if [ "$nbin" -gt 0 ] && [ "$nplain" -eq 0 ]; then
        echo "PASS  $nbin executables, each linked with $want"
    else
        echo "FAIL  $nplain of $nbin executables lack $want"; failures=$((failures + 1))
    fi
    # control: the same file built by the compiler itself, without the wrapper, must lack it
    "$SANREAL" -O1 -pthread -o "$OUT/san/plain" $T/sanplant_test.c
    if grep -q "$want" "$OUT/san/plain"; then
        echo "BROKEN TEST: control PASSED  sanitizer coverage: an uninstrumented build carries $want"
        broken_controls=$((broken_controls + 1))
    else
        echo "CONTROL FAILED AS REQUIRED  sanitizer coverage: sanplant built without the wrapper lacks $want"
    fi
    echo
fi
if [ -n "$COVERAGE" ]; then
    echo "=== coverage: the gcc-built suites (coverage-gcc.sh, in the containers) ==="
    GCOV_ARGS=()
    if ! command -v docker >/dev/null 2>&1; then
        skip "coverage of the gcc-built suites: no docker (the report covers the clang suites only)"
    elif bash "$T/coverage-gcc.sh" "$ROOT/guest/out/unit-gcccov"; then
        GCOV_ARGS=(--gcc-lcov "$ROOT/guest/out/unit-gcccov/report/coverage.info")
    else
        echo "FAIL  coverage of the gcc-built suites (a suite or a control above)"; failures=$((failures + 1))
    fi
    echo
    echo "=== coverage report ==="
    rm -rf "$COVDIR"; mkdir -p "$COVDIR"
    if python3 "$T/coverage-report.py" --root "$ROOT" --out "$OUT" --prof "$OUT/cov/prof" --covdir "$COVDIR" \
            --profdata "$LLVM_PROFDATA" --cov "$LLVM_COV" --cc "$COVREAL" ${GCOV_ARGS[@]+"${GCOV_ARGS[@]}"}; then
        [ ${#GCOV_ARGS[@]} = 0 ] || cp "$ROOT/guest/out/unit-gcccov/report/summary.txt" "$COVDIR/gcc-summary.txt"
    else
        echo "FAIL  coverage report"; failures=$((failures + 1))
    fi
    echo
fi
if [ -n "$COMPILE_ONLY" ]; then
    echo "=== compile-only pass ==="
    nb=$(wc -l < "$OUT/co/built" | tr -d ' '); nf=$(wc -l < "$OUT/co/failed" | tr -d ' ')
    ns=$(grep -oE '[A-Za-z0-9_]+_test\.c' "$OUT/co/built" | sort -u | wc -l | tr -d ' ')
    [ "$nf" -eq 0 ] || awk '/^COMPILE FAILED/ || !seen[$0]++' "$OUT/co/errors" # each error line once
    if [ "$nf" -ne 0 ] || [ "$failures" -ne 0 ] || [ "$broken_controls" -ne 0 ]; then
        echo "UNIT TESTS (compile only): FAILED ($nf compile(s) failed, $failures other failure(s), $broken_controls broken control(s); $nb compiled, $skipped skipped)"
        exit 1
    fi
    echo "UNIT TESTS (compile only): OK ($nb compiler runs over $ns test programs, $co_notrun suites built and not run, $skipped skipped: the SKIP lines above)"
    exit 0
fi
TAG=${SAN:+ ($SAN)}   # a sanitizer pass's summary names it, so it is never read as the plain pass's
TAG=${TAG:-${COVERAGE:+ (coverage)}}
if [ "$failures" -ne 0 ] || [ "$broken_controls" -ne 0 ]; then
    echo "UNIT TESTS$TAG: FAILED ($failures suite(s) failed, $broken_controls control(s) passed that must fail, $skipped skipped)"
    exit 1
fi
if [ "$skipped" -ne 0 ]; then
    echo "UNIT TESTS$TAG: OK WITH $skipped SKIPPED (everything that ran passed or failed as required; the SKIP lines above did not run here)"
    exit 0
fi
echo "UNIT TESTS$TAG: OK (all suites passed, all controls failed as required)"
