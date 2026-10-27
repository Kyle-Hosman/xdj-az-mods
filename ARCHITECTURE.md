# Architecture

How the AZ-MODS source is organized and how the two libraries work inside the XDJ-AZ.
Building and verifying: [BUILDING.md](BUILDING.md). USB formats and the systems around the
libraries (updates, Community Mods, library editing, Remote Stems, firmware support,
testing): [docs/](docs/README.md).

## 1. Overview

The XDJ-AZ runs one Linux process, `EP147`, that owns the user interface (an X11/JUCE
application), the panel buses and the audio engine. AZ-MODS is two shared objects loaded
into it with `LD_PRELOAD`:

| Library | Source | Role |
|---|---|---|
| `azmod.so` | `src/azmod/` | everything visible and the input side: the Mod Menu, pad and button LED colors, the stem and drum roll pad displays, waveform and frame-rate changes, Themes, playlist editing and library edits, Bluetooth helpers, the Update Mods screen, the Community Mods host, and the panel-bus filtering that turns pad presses into stem and drum roll requests |
| `azstem.so` | `src/azstem/` | the audio side: finds and decodes the stems of each loaded track, injects them into EP147's own per-deck source read, runs the drum roll sampler |

### Deck side

| Piece | Path | Does |
|---|---|---|
| Loader | `deck/payload/usr/sbin/az-usbload` | mirrors the chosen USB's `MODS/` set into a cache on the settings partition and writes the environment file EP147 reads at start |
| Preload drop-in | `deck/payload/etc/systemd/system/EP147.service.d/10-azmod.conf` | points EP147 at that environment file, so the first start of each boot preloads the last applied set without touching the USB |
| USB folder `MODS/deck/` | assembled by `deck/stage_stick.py` | the `boot.d` hooks from `deck/stick/deck/boot.d/` (test-mode keep-alive, SSH gating, boot logo restore, uninstall); the helpers from `deck/stick/deck/lib/` (`az-modupdate`, `az-community`, `gadget-renotify.sh`, `az-logcap.sh`, `az-sshprio`); newer copies of the loader and its helpers from `deck/payload/` (`az-usbload`, `az-usbpick`, `usb-renotify.sh`); the Remote Stems helper from `deck/azrs/`; the handoff guard built from `src/azhandoff/` |
| Trust | `deck/make_pins.py` | the loader runs a script from `MODS/deck/`, or stages a `.so`, only when its SHA-256 is in a signed `trust.pins` ([docs/UPDATES-AND-SIGNING.md](docs/UPDATES-AND-SIGNING.md)) |
| Boot snapshot | `deck/payload/usr/sbin/az-bootlog` | opt-in, does nothing unless the USB carries `MODS/diag.on`; not in the release firmware payload |

Network work runs outside EP147. azmod writes one-line request files and the loader's request
watcher runs the helper; both helpers report back through status files under `/tmp` that
azmod reads.

| Request file | Helper | Screen |
|---|---|---|
| `/run/az-modupdate.req` | `lib/az-modupdate` (the on-deck updater) | Update Mods |
| `/run/az-community.req` | `lib/az-community` (the Community Mods fetcher) | Community Mods |

The Remote Stems helper (`deck/azrs/azrs.sh`, staged as `lib/azrs.sh`) runs as its own
systemd unit, which azmod starts when there is queued work. The boot sequence and the USB
layout are in [docs/USB-STICK.md](docs/USB-STICK.md); the scripts in
[deck/README.md](deck/README.md).

## 2. Design principles

Stated in the file headers and visible throughout the code.

| Principle | Where it shows |
|---|---|
| **Drive EP147's native layer instead of drawing over it.** | Stem pads are EP147's own `PadStemWidget` tiles with their `ImagesComponent` image index rewritten (`stems_display.c`). Stem mutes reshape the native waveform by feeding its data providers a scaled point array (`waveform.c`). Messages use EP147's own caution strip and box (`banner.c`). Themes remap colors at the renderer's `setFill` (`day_mode.c`). Stems are injected below EP147's DSP, so its own tempo, key shift, key lock and scratch apply to them (`key_shift.c`). X11 overlay windows (`mod_menu_overlay.c`) carry the Mod Menu and a few readouts. |
| **Identity by construction.** | "Which deck does this object belong to" is answered by hooking the constructor that receives the deck as an argument: `DjPlayerUnit(PlayerId)` and `gui::LoadState` in `deck_track_map.c`, `gui::BpmState` in `stems_display.c`, `usecase::mixer::Channel` in `bluetooth.c`, the waveform widgets in `waveform.c`. A lookup that cannot answer returns unknown and the caller does nothing; there is no default deck. One documented exception: `deck_slot` (`stem_playback.c`) gives an audio unit it cannot identify a free slot so its levels and mute keep working, and records the case as a conflict. |
| **Validate at use.** | Cached object pointers are checked for alignment and the expected vtable every time they are used, and dropped when the check fails (`beat_fx_panel.c`, `drum_roll_display.c`, `stems_display.c`, `menu_popup.c`). |
| **Exact firmware identity, inert otherwise.** | Every EP147 address is a per-build table entry. The running build is identified by the md5 of `/proc/self/exe` (`src/common/ep147build.h`). An unknown build, or a table with any missing entry, leaves the library inert: no hooks, and the interposers forward to the next definition unchanged. There is no fallback to another build's table ([docs/FIRMWARE-SUPPORT.md](docs/FIRMWARE-SUPPORT.md)). |
| **Fail-safe patching.** | The inline-hook installer declines a function whose prologue it cannot relocate, and callers treat a decline as "hook not installed" (`inline_hooks.c`, the installer in `azstem.c`). |
| **Nothing heavy on real-time or paint paths.** | Hooks on EP147's audio thread do no file I/O, allocation or locking; they read and publish atomics, and a background thread does the rest (`azstem.c`, `drumroll.c`). Flags read by hot hooks are refreshed off the hot path (`key_shift.c`, `vinyl_brake.c`). |
| **Features are gated.** | Each mod follows a Mod Menu row (declared once in `features.def`, read through `azfeatures.h`), an opt-in flag file (`*.on`) or a kill switch (`*.off`) under `/home/root/settings`. Hooks that must be installed at start stay installed and pass through while their feature is off. |

### Diagnostics

| Gate | Effect |
|---|---|
| `AZ_PROBE_ON(path)` (`menu.h`) | the constant 0 unless the build defines `AZMOD_PROBES` (a release build does not); with it, `access(path)` |
| `azlog_open()` (`azmod.c`) | writes nothing unless `/home/root/settings/azmod-logs.on` existed when it was first called; appends are capped at 256 KB |
| the same flag file | azstem's log (`/tmp/azstem.log`), the touch-simulation inputs (`/tmp/azmod-touchsim-*`), and a few azmod writers that check it at the time of writing |

Files a release build reads or writes outside both gates:

| Kind | Files |
|---|---|
| azmod test inputs, read with plain `access()` / `open()` | `/tmp/azmod-fake-temp`, `/tmp/azmod-fake-cpufreq` (Sys Info, used only when the deck exposes no sensor); the Themes test triggers and counters `/tmp/azmod-dm-*` and `/tmp/azmod-daymode.off` (`day_mode.c`) |
| azmod test harnesses behind their own flag under `/home/root/settings` | for example `azmod-banner.on`, which reads `/tmp/azmod-banner.txt` (`banner.c`) |
| azmod state | `/tmp/azmod-disp60` (exists while the 60 FPS UI clock is armed); the empty `/tmp/azmod-shots` directory (created at overlay start); `/tmp/azmod-hooktable` (every hook install and the path it took, written at overlay start and again after the community mods load, `inline_hooks.c`) |
| crash reports (`crash_guards.c`) | `/tmp/azmod-crash.log`: opened at every start, written only when EP147 takes a fatal signal, capped at 256 KB and moved to `.1` at start when nearly full. `/tmp/azmod-crash.txt`: the fallback reporter's, moved to `.1` past 256 KB |
| Community Mods host (`community.c`) | `/tmp/azmod-community/loaded` (status) and each mod's load result in its log `/tmp/azmod-community/<id>.log` |
| read-only overrides | `/tmp/azmod-pngpatch` (art patch directory, after `/home/root/settings/az-usbload/azmod-pngpatch` and `/home/root/settings/azmod-pngpatch`); `/tmp/azstem.stemsdir` (stems base directory, after the `AZSTEM_STEMS_DIR` environment variable and `/home/root/settings/azstem.stemsdir`; used until a base is found); `/tmp/azstem.track` (deck 1 track name fallback, after `/home/root/settings/azstem.track`) |
| azstem files in `/tmp` | `/tmp/azstem-build.log` (one line at start, moved to `.1` past 16 KB); `/tmp/azstem.probestat` (status, about once a second); `/tmp/azstem.phprof`, `/tmp/azstem.stall` (flusher timing); `/tmp/azstem.cursrc1..4` (the file loaded on each deck, written on change); `/tmp/az-decks` (which decks have a track loaded, for the loader's restart policy; written on change and at least every 3 seconds) |

Apart from the crash reports and the hook table, each is written at most about once a second
or once per track or stem change. All are under `/tmp` (cleared on reboot); none is written
to the settings partition.

## 3. Source layout

### `src/azmod/`

| File | Purpose |
|---|---|
| `azmod.c` | Library entry: constructor, process filter, firmware identification, shared state, settings load and save, the JUCE message-thread tick; `#include`s the feature modules below (except `menu.c`, `text.c` and `azconf.c`, which are separate translation units, and `day_mode.c`, `pdb_sync.c` and `pdb_stock.c`, which are included by `pad_leds.c`, `library_edits.c` and `playlist_editor.c`) |
| `azmod_internal.h` | Declarations shared across the `azmod.c` translation unit |
| `ep147addr.h` | Per-firmware EP147 address tables for azmod (generated; see section 9) |
| `ep147png.h` | The running build's PNG images (start and length) that staged art may replace |
| `menu.h`, `menu.c` | Mod Menu model and software compositor; `menu.h` is also the interface between the azmod translation units. `menu.c` is one translation unit that `#include`s `azfeatures.c` and the parts below, in a fixed order (each uses only what the ones above it define) |
| `pad_palette.h`, `stem_overlay.h`, `azcm.h`, `menu_theme.h` | Included by `menu.h`: the pad-mode color palette (index-stable), the stem UI's geometry and the azstem getters, the community core's interface to the Mod Menu, the Themes color map for azmod's own drawing |
| `ui_draw.c` | Part of `menu.c`: drawing primitives (pixels, rects, text, checkboxes, icons, rounded rects, the 5x7 font) |
| `menu_layout.c` | Part of `menu.c`: the menu's geometry (compact strip, docked pane), its header and backdrop, the closed-state bars' rects |
| `menu_list.c` | Part of `menu.c`: the main list built from the feature table (visible rows, order, row geometry, pins, the compact list's rows) |
| `menu_state.c` | Part of `menu.c`: state init, the tap flash, BACK |
| `menu_full.c` | Part of `menu.c`: the full-screen Mod Menu (left list, right pane, touch and knob, the preview card) |
| `screen_colors.c`, `screen_update.c`, `changelog.c`, `screen_themes.c`, `screen_overlays.c`, `screen_sysinfo.c`, `screen_community.c` | Parts of `menu.c`: Pad Colors, Update Mods and its changelog box, the Themes picker, Overlays and the deck-screen readouts, System Info and its Save Logs to USB button, Community Mods |
| `stem_overlay.c` | Part of `menu.c`: the deck-screen stem UI (strip, mini lanes, dots, pill grid) |
| `menu_input.c`, `menu_composite.c` | Parts of `menu.c`: `azmod_input` / `azmod_tick` and `azmod_composite`, the dispatchers over every screen |
| `text.c` | FreeType text rendering in EP147's own typefaces, for the overlay |
| `inline_hooks.c` | AArch64 inline-hook and vtable-patch primitives used by every azmod hook, and the hook install table (section 5) |
| `bus_interpose.c` | libc interposers on the panel buses (`open`, `read`, `ioctl` and the descriptor bookkeeping around them): LED frame rewrite, input filtering |
| `bus_cfg.h` | What the overlay thread publishes for the bus threads, lock-free (section 5) |
| `drag_handoff.h` | Hands the end of a playlist drag from the bus read thread to the message tick (section 5) |
| `mod_menu_overlay.c` | Overlay thread: X11 windows, painting, input, Sys Info, screen detection; `install_static_hooks`, the constructor's hook list |
| `pad_leds.c` | Pad and pad-mode LED color publishing; includes `day_mode.c` |
| `day_mode.c` | Themes: native color remapping for the whole UI |
| `stems_display.c` | Native stem pad grid, tile art and per-deck source-cell strips |
| `waveform.c` | Stem-scaled native waveforms and overviews; the 60 FPS options |
| `brightgrid.h` | Bright Grid's pixel rule for the beat ruler: brighten the ticks, never the background (used by `waveform.c`) |
| `wave_att.h` | The stem waveform height rule: each stem's share at its pad toggle and Stem Volume gain, capped at unity, and when a deck's curve is rebuilt (used by `waveform.c` and `mod_menu_overlay.c`) |
| `drum_roll_display.c` | Drum roll pad and x-pad state, pad tiles, caption, Beat FX rate row |
| `beat_fx_panel.c` | Beat FX panel hooks; the X-PAD parameter row that follows the drum roll finger zone |
| `beat_jump.c` | "Beat Jump 2 First": swaps the jump sizes of the two BEAT JUMP pad pages |
| `extras.c` | Hide Track Title, Phase Meter and Phase Layout |
| `vinyl_brake.c`, `vinyl_slider.c` | Stepless vinyl brake: setter hook and the SHORTCUT slider |
| `source_list_entry.c` | The native MOD MENU row in EP147's SOURCE list |
| `banner.c` | Text in EP147's own caution strip and box |
| `menu_popup.c` | Playlist editing actions in EP147's own MENU panel |
| `playlist_editor.c` | Reorder, create, rename and delete playlists and folders in Browse; includes `pdb_stock.c` |
| `playlist_actions.c` | Remove, add, paste, undo, folder delete and Tag List copy requests |
| `library_db.c` | SQLite helpers over EP147's own connection; per-stick library server checks |
| `library_edits.c` | On-stick library writes on the library thread: journal and undo, backups, cross-stick copy, SQL spool; includes `pdb_sync.c` |
| `pdb_sync.c` | Keeps `export.pdb` consistent with `exportLibrary.db` after a cross-stick paste (libc only) |
| `pdb_stock.c` | Schedules the same `export.pdb` re-sync after EP147's own playlist writes while Playlist Editing is on (libc only) |
| `stem_marks.c` | Browse-list indicators for tracks that have stems |
| `remote_stems.c` | MENU > MAKE STEMS: queues tracks for separation on a computer |
| `track_list_rows.c` | TrackID-to-path table, touch-scroll row cache, shared paint slots |
| `touch_accum.c` | Lets slow touch drags scroll (opt-in, `azmod-touchaccum.on`) |
| `bluetooth.c` | Bluetooth auto-connect, remembered CH3/CH4 input, UART speed recovery |
| `browse_columns.c` | The Browse PREVIEW button and the track list header's sub column choice remembered across restarts, and Column Edit's hidden and shown columns (through the deck's own list header) |
| `browse_edit.c`, `browse_edit.h` | Column Edit: the COLUMN EDIT and RESET buttons, its edit mode, resizing the Browse track list columns and the saved widths |
| `browse_fields.c`, `browse_fields.h` | Column Edit: a library field shown on a Browse track list column, drawn by the deck's own cell painter; Hide Column in the deck's field list |
| `crash_guards.c` | Guard for a stock QR-login fault; async-signal-safe crash reporter, run on an alternate signal stack |
| `updates.c` | Update Mods screen: version, status and changelog readers, request writer |
| `savelogs.c` | System Info's Save Logs to USB: request writer and status reader |
| `probe_menu.c` | An empty stub in this source: the browse MENU observer exists in development builds only |
| `folder_preview.c` | Fills the Browse preview pane for a selected playlist folder, which firmware 2.00 leaves empty; patches one slot in each of six browse event vtables, all or none (kill switch `azmod-folderpreview.off`) |
| `community.c` | Community mod host: loads the enabled mods from the stage (`dlopen`, `RTLD_LOCAL`) after manifest, firmware-tag, ELF, `min_core` and approved-list checks; one owner per hooked address; per-mod settings and logs; start quarantine and per-mod crash blame (the crash reporter names the mod that faulted) |
| `community_icons.c` | Community tile icons, decoded by EP147's own PNG decoder on the message thread |
| `sdk_input.c` | Community mod services, included by `azmod.c`: frame filters on the panel buses and the touch filter, each call timed against a budget; a filter that keeps overrunning is switched off |
| `sdk_deck.c`, `sdk_ui.c`, `sdk_logic.h` | Community mod services, included by `azmod.c`: deck state and events (from azstem's `sdk_export.c`), banners, the confirm dialog (also the core's own questions), `post_ui` and mod threads; `sdk_logic.h` holds their decisions, free of EP147 so the host tests run them |
| `sdk_surface.c`, `sdk_surface_logic.h` | Community mod services, included by `community.c`: which view EP147 shows, mod surfaces on the overlay, windows of programs a mod launched, pointer input, images and lines; the logic header is the part the host tests drive |
| `sdk_rows.c` | Community mod services, included by `community.c`: rows a mod registers in the Mod Menu and read-only lookups of the official rows |
| `sdk_proc.c`, `sdk_lib.c`, `sdk_audio.c`, `sdk_vslot.c` | Community mod services, included by `community.c`: other programs (launch, handoff, reload), the rekordbox library read-only, per-deck audio taps (the ALSA interposers) and vtable slot claims |
| `ssh_approve.c`, `ssh_approve_logic.h` | Asks on the deck before a USB stick's SSH key is allowed; the boot hook holds every unknown key until then |
| `ep_mem.h` | How azmod reads EP147's memory, and the fault-free read the crash reporter's frame walk uses |
| `utf8_cut.h` | Cuts a string at a UTF-8 character boundary (included by `ep_mem.h`) |
| `features.def` | The one declaration of every Mod Menu row: enum, settings key, label, parent, kind, flags, the legacy `azmod.cfg` slot and the help text (section 5, "Persistence") |
| `azfeatures.h`, `azfeatures.c` | The row table built from `features.def` plus the rows mods register: values, parents, pins and the `azmod.cfg` slot mapping; pure logic, included by `menu.c` |
| `azconf.h`, `azconf.c` | `azmod.conf`: reading and writing the settings file, the migration from 1.2.2's `azmod.cfg` and the copy kept for a rollback; its own translation unit |
| `azmod-mod.h` | The community mod ABI (v1) and `AZ_CORE_VERSION`: the same file as `include/azmod-mod.h` in the [AZ-MODS SDK](https://github.com/Kyle-Hosman/az-mods-sdk) |
| `onelibrary_key_emulator.c` | Emulator-only `sqlite3_*` interposers: plain pass-through unless `/home/root/settings/azmod-onelibrarykey.on` exists, a flag for the emulator image only (on a deck it would force a wrong key into EP147's database connections) |
| `eyeicon.h`, `menu_icons.h`, `stemsmark.h`, `vsbar.h` | Embedded image data drawn by the modules above |
| `icons/` | Source SVGs for the menu icons; licenses and credits in `SOURCES.txt`, the `LICENSE-*` files beside them and the top-level `NOTICE` |
| `test/lanetest.c` | Host-native self-check for the stem lane geometry in `stem_overlay.c`, built through `menu.c` (section 8) |
| `test/community/` | Host tests for `community.c` with real test mods (`run.sh`, its harness and test mods, in the build container; section 8) |

The firmware-derived headers (UI art and the database key) ship as templates in
`assets/*.h.in`, filled into `src/azmod/` by `tools/extract-assets.py`
([BUILDING.md](BUILDING.md)).

### `src/azstem/`

| File | Purpose |
|---|---|
| `azstem.c` | Library entry: constructor, inline-hook installer, flusher and mask-poll threads, stem mask reader; `#include`s the modules below (`ofsload.c` and `drumroll.c` through `key_shift.c` and `stem_playback.c`) |
| `azstem_internal.h` | Declarations shared across the azstem translation unit (per-deck `deck_stems` state) |
| `ep147addr.h` | Per-firmware EP147 address tables for azstem |
| `ep147rate.h` | `AZ_EP147_RATE`, the deck render rate (section 7) |
| `stem_playback.c` | Stem folder discovery, progressive parallel Ogg decode, per-deck slots, offset capture; includes `drumroll.c` |
| `stem_envelopes.c` | Per-stem peak envelopes that azmod draws under the stem buttons |
| `deck_track_map.c` | Deck identity from the `DjPlayerUnit` constructor; track identity from `gui::LoadState`; `/tmp/az-decks` |
| `key_shift.c` | Render tap, source-read injection, `KeyControlFGPR` hooks and per-deck tagging; includes `ofsload.c` |
| `ofsload.c` | Stem offset measured at load against EP147's own decode |
| `drumroll.c` | Drum roll sampler: bank discovery, decode, voice scheduler, real-time mix |
| `sdk_export.c` | Per-deck state for the community mod services (`../common/azdeckinfo.h`): the loaded track, the playhead, stems and the beat grid; atomic loads or one sequence-locked copy, never blocking |
| `stb_vorbis.c` | Vendored Ogg Vorbis decoder (public domain / MIT), its own translation unit |

### `src/common/`

Header-only code shared by the two libraries:

| File | Purpose |
|---|---|
| `ep147build.h` | Identifies the running EP147 build by md5 (section 5) |
| `azprot.h` | Opens a page range for writing and restores its exact protection (section 5) |
| `azaltstack.h` | `az_thread_create`: every thread the libraries start gets an alternate signal stack for the crash reporter |
| `azspawn.h` | Starts helper programs with `posix_spawn` and an argv array: EP147 is never forked and no shell runs |
| `azlogcap.h` | Size caps for every file the libraries append to under `/tmp` |
| `azsha256.h` | SHA-256 for the community approved list |
| `azdeckinfo.h` | What azstem publishes about each deck for the community mod services (`sdk_export.c` writes it, azmod's `sdk_deck.c` reads it) |
| `aztap.h` | The audio tap ring: filled on EP147's audio thread without waiting, drained by a core thread for the mods that asked |

### `src/azhandoff/`

| File | Purpose |
|---|---|
| `az-handoff-guard.c` | Runs beside a program a mod hands the deck to (`handoff`), reading the mixer bus read-only: holding BACK for 3 seconds stops that program (SIGTERM, then SIGKILL two seconds later). Once it ends, `--restore` starts EP147 again, once more if it does not stay up, and reboots the deck if it still is not running. Static, no preload |
| `guard_logic.h` | The guard's decisions (frame checks, the hold timer, the restart policy), apart from its I/O so the host tests drive them |

## 4. Translation units

`azmod.c` and `azstem.c` `#include` their feature modules (`.c` files) rather than compiling
them separately, and the modules share declarations through `azmod_internal.h` and
`azstem_internal.h`. The release link lines in `src/Makefile` follow this:

| Library | Translation units |
|---|---|
| `azmod.so` | `azmod.c`, `menu.c`, `text.c`, `azconf.c` |
| `azstem.so` | `azstem.c`, plus `stb_vorbis.o` |

A module included by `azmod.c` cannot see the statics of `menu.c`, `text.c` or `azconf.c`;
what crosses between them is the `azmod_*` interface declared in `menu.h` and the settings
interface in `azconf.h`, not `azmod_internal.h`. `menu.c` is itself assembled from the parts it
`#include`s (`azfeatures.c`, then `ui_draw.c` ... `menu_composite.c`), so they share its
statics; no part includes another.

The reason is symbol hygiene. A preloaded library sits in front of EP147 in symbol
resolution, so any external symbol it defines can interpose on a symbol of the same name in
EP147 or its libraries. Inside one translation unit every function and variable can be
`static`; any helper that is not becomes an export. The exports are kept to the groups below,
and the unit tests compare each release library's exported symbols against a fixed list
(`tests/unit/exports-azmod.txt`, `tests/unit/exports-azstem.txt`, checked by
`tests/unit/exports.sh`).

| Group | Symbols |
|---|---|
| libc interposers (`bus_interpose.c`) | `open`, `open64`, `close`, `dup2`, `dup3`, `read`, `ioctl`, `lseek`, `lseek64`, `recvmsg` |
| ALSA interposers (`sdk_audio.c`) | `snd_pcm_hw_params`, `snd_pcm_writei`, `snd_pcm_close`: the community audio taps; they copy samples only for a tap a mod asked for |
| sqlite3 interposers (`onelibrary_key_emulator.c`) | `sqlite3_key`, `sqlite3_open_v2`, `sqlite3_exec`, `sqlite3_prepare_v2` |
| Hook thunks | entry points EP147 jumps to, written in top-level assembly with `.globl`: `azm_*` in `waveform.c` (`azm_rw_shim`, `azm_upd_shim`, `azm_wp6_*`, `azm_fast6_*`, `azm_call_sret`) and `stems_display.c` (`azm_forge_*`); and the C functions those thunks call by name (`azm_wp_scale`, `azm_upd_enter` / `azm_upd_exit`, `azm_rw_enter` / `azm_rw_exit`, `azm_fast_*`). Symbols that assembly needs but must not export are hidden, with `.hidden` in the assembly (`menu_popup.c`) or `__attribute__((visibility("hidden")))` (`day_mode.c`, `stems_display.c`) |
| The `azmod_*` interface | declared in `menu.h`, between `azmod.c`, `menu.c` and `text.c`. `menu.c` reads and writes a few files (the theme, the Community Mods catalog, status and request files) but has no dependency on EP147, so it builds and runs on a host (`test/lanetest.c`); `text.c` is separate so FreeType stays out of it. One `azmod_*` symbol is not in `menu.h`: `azmod_trackid_path` (`track_list_rows.c`), which azstem resolves at run time |
| The `azstem_*` interface | resolved by azmod with `dlsym` at run time: drum roll control (`azstem_drum_*`), stem availability (`azstem_has_stems`, `azstem_present`, `azstem_deck_has_track`, `azstem_decks_loaded`), track generation (`azstem_trackgen`), the loaded track's name (`azstem_deck_track_name`), the decoded data behind the mini waveforms (`azstem_wave`, `azstem_frames`, `azstem_env_gen`, `azstem_env_final`), stem EQ (`azstem_stem_eq`, and `azstem_stem_gain`, which the stem waveforms scale by), and for the community mod services `azstem_deck_snapshot`, `azstem_deck_beat_phase` and `azstem_play_obs`. azmod treats each pointer as absent until azstem is loaded |

The vendored decoder is the one file compiled on its own (`stb_vorbis.c`, built with
`-fvisibility=hidden`), so its functions link into azstem without being exported.

## 5. Lifecycle

**Process filter.** `LD_PRELOAD` is set for the whole `EP147.service` unit, so the libraries
are mapped into every process in it. Both constructors (`azmod_init` in `azmod.c`,
`azstem_init` in `azstem.c`) return at once unless `/proc/self/comm` is `EP147`, so no hooks
or threads exist in other processes. The interposers are still bound there, but `g_azm_inert`
starts set and is cleared only once EP147 is identified, so they forward every call
unchanged.

**Firmware identification.** Each constructor calls `ep147_identify("/proc/self/exe")` and
`ep147_addr_init()`. Only a file whose size and md5 match an entry in `EPB_KNOWN` is
accepted, and `ep147_addr_init` fails if any address the library uses is zero for that build.
azstem appends the result to `/tmp/azstem-build.log`; azmod writes it to
`/tmp/azmod-build.log` when diagnostics are on. On failure azmod leaves `g_azm_inert` set,
which makes every interposer forward unchanged, and azstem returns before installing
anything. Code names an address by its 1.30 value (`EP_1b93a28`), which expands to that entry
of the running build's table.

**Hook installation.** `azm_install_hook` (`inline_hooks.c`):

| Case | Result |
|---|---|
| null or misaligned address; a PC-relative branch, literal load or ADR in the first four instructions | declined (returns -1): the caller treats the hook as not installed |
| ADRP in the first four instructions | re-encoded when the target is within range of the trampoline, otherwise materialized as an absolute address (MOVZ + MOVK) |
| an executable island can be mapped within branch range | the hook gets a 16-byte absolute jump in the island, and the function's first instruction becomes a B to it in one atomic 4-byte store, so a thread already in the function runs either the old or the new code |
| no island in range, during azmod's constructor | the first four instructions are overwritten with the absolute jump (`LDR X17` / `BR X17`); refused for a function that returns or branches away (`ret`, `br`) before its fourth instruction |
| no island in range, after the constructor | refused (`AZM_REFUSED_LATE`), nothing written |

The relocated prologue goes into a trampoline followed by a jump back, returned through
`*orig` so the hook can call the original. `azm_patch_vtable` swaps a single vtable slot
instead. Every install is recorded with the path it took, and `azm_hooks_dump` writes the
table to `/tmp/azmod-hooktable`. azstem carries its own installer (`install_hook_at`, in
`azstem.c`) with the same refusal and ADRP rules but always the four-instruction patch, since
its hooks go in from the constructor; after the constructor it refuses.

**Page protection.** Every write into EP147's image (a hook, a vtable slot, a patched
instruction, replacement art in `.rodata`) goes through `azprot_unlock` / `azprot_relock`
(`common/azprot.h`). Unlock reads the range's protection from `/proc/self/maps`, adds write
access for the store, and refuses (the caller skips its write) when part of the range is
unmapped or the range spans two protections. Relock puts back exactly what was read, so code
stays `r-x`, RELRO vtables stay `r--`, and `.rodata` that shares the code segment keeps its
execute bit. A process-wide lock is held from unlock to relock. A failed relock is logged.
Trampolines are written while private and writable, then sealed `r-x` before anything can
jump to them; islands are mapped `r-x` and each slot is written through the same
unlock/relock, so no page stays writable and executable.

**Install order.** EP147's threads start once the constructors return, so both libraries
install their hooks from the constructor. azmod's constructor, in order: Bluetooth UART
recovery and the Bluetooth hooks; the crash reporter and its `cr_watch` thread; the early
settings load (the row values, before any hook that reads one); the banner; early Themes;
touch accumulation; the MENU panel hooks; the staged art patches; then
`install_static_hooks` (`mod_menu_overlay.c`), every remaining hook. It then calls
`azm_hooks_go_late` and starts the overlay thread, which installs nothing itself; the only
later installs are community mods' hooks (island path only). Some hooks install only when
their flag or setting asks for them; many install unconditionally and check their Mod Menu
row or flag at run time.

The art patches (`pngpatch_apply`) are skipped as a set unless every blob has an address file
for the running build; each blob then applies or is refused on its own, and only at the start
of one of the build's PNG images listed in `ep147png.h`, with a PNG that fits it.

azstem installs its constructor hooks (`DjPlayerUnit`, `gui::LoadState`) before EP147
constructs its players, then the audio hooks; USB scanning is left to the flusher so startup
is not delayed.

**Threads created by azmod.** Every thread is created with `az_thread_create`, which gives it
an alternate signal stack.

| Thread | Does |
|---|---|
| `overlay_thread` | owns the Mod Menu state (`g_state`): X11 windows, input, screen detection, LED color publishing, stem toggles and settings saves; loads the community mods and runs their `init`, tick and paint |
| `cr_watch` | re-arms the crash handlers for the first minute |
| `dm_watch` | follows the Themes switch |
| `cp_worker` | cross-stick copies, at `SCHED_IDLE` |
| `sm_worker` | the stem-mark file checks, at `SCHED_IDLE` |
| `rs_playlist_worker` | queues playlists for Remote Stems, at `SCHED_IDLE` |
| `az_reload_run` | `systemctl daemon-reload`, then the EP147 restart, for the Mod Menu's Reload row |
| waiter (`azspawn_waiter`) | reaps a helper started with `azspawn` (`common/azspawn.h`: `systemctl`, `systemd-run` for the Remote Stems unit) that nobody waits on |
| community mod services | the audio tap thread (drains the tap rings every 10 ms), mod threads (`sdk_ui.c`), and the launch, log-copy, stop and handoff helpers of `sdk_proc.c` |

Much of azmod runs on EP147's own threads, inside the calls it hooks:

| EP147 thread | azmod code |
|---|---|
| bus thread | the `read` / `ioctl` interposers |
| JUCE message thread | `calltimers_hook` (installed with its own once-only installer) drives `msg_tick`; widget hooks; `rtick_hook` |
| each USB's library thread | `mlserver_onmsg_hook` |
| player thread | `vinyl_brake.c` |

Requests from the bus thread to the overlay thread travel as atomics. The bus thread runs at
real-time priority and never calls into EP147's UI: when a press drops a dragged playlist row
or BACK cancels the drag, it only records that end in `drag_handoff.h`, and the message tick
performs the commit or cancel. The overlay thread owns `g_state`, which is never locked, and
the bus thread never reads it. The overlay publishes what the bus acts on (`bus_cfg.h`) as one
64-bit settings word (Playlist Editing, Stems on BEAT LOOP 2, the reorder hooks' state and
the x-pad's five bytes), one 32-bit word per pad-mode color, and each deck's 32-byte pad LED
block behind a sequence counter, so a frame is edited by one whole published combination and
the bus thread never waits. Message-thread hooks and `dm_watch` still read single fields of
`g_state` (such as `mod_enabled`) without synchronization.

**Community Mods.** The overlay thread calls `community_init` (`community.c`) once at its
start. For each enabled mod in the loader's stage (`/home/root/settings/az-usbload/community`,
mirrored from the USB's `MODS/community-mods/`) it checks, in order:

1. the per-mod switch;
2. the `azmod.json` manifest (id, API version, firmware tag, hooks, `min_core`);
3. the ELF file: an aarch64 shared object that exports `az_mod_v1_entry` and nothing else
   apart from linker boilerplate (an export of something azmod defines, or any `sqlite3_*`,
   is named in the refusal);
4. the deck's approved list: the mod's id with the SHA-256 of exactly that `.so`; otherwise
   the Mod Menu asks "New mod found: allow?".

It then loads the mod with `dlopen(RTLD_NOW | RTLD_LOCAL)` and hands it the core API declared
in `azmod-mod.h`. Every refusal has a reason the Mod Menu shows. Mods hook EP147 through the
same installer as azmod, and an address already hooked by azmod or another mod is refused.

| Protection | Rule |
|---|---|
| start quarantine | a start that loaded mods and did not stay up 90 seconds is counted; the second such start in a row loads none |
| crash blame | a crash whose faulting PC, return address or walked frames lie in a mod's code suspends only that mod on the next start |
| unexplained crashes | three crashes within 30 minutes that blame no mod suspend every community mod until the DJ resumes them |

The deck-side half is the fetcher `deck/stick/deck/lib/az-community`, which browses
repositories, downloads, verifies and installs mods onto the USB; the loader applies the
result. Details: [docs/COMMUNITY-MODS-INTERNALS.md](docs/COMMUNITY-MODS-INTERNALS.md). The
mod-author documentation, a template mod and the publishing rules are in the
[AZ-MODS SDK](https://github.com/Kyle-Hosman/az-mods-sdk).

**Threads created by azstem.**

| Thread | Does |
|---|---|
| `flusher` | a 200 ms loop: track change detection, stem discovery, offset measurement, drum bank loading, identity maps, `/tmp/az-decks` |
| `maskpoll_thread` | reads the stem masks at 20 Hz and reports a stalled flusher |
| `loader_thread` | one per track load |
| `dth_prog` | one decoder per stem, in parallel |

**Persistence.** The Mod Menu's settings live in `/home/root/settings/azmod.conf`:

```
# AZ-MODS settings
version = 1
<key> = <value>
```

| Rule | Detail |
|---|---|
| Keys | one per row, from `features.def`; stable and listed in [docs/MOD-MENU-KEYS.md](docs/MOD-MENU-KEYS.md). A community mod's rows are `<mod-id>:<key>` |
| Values | toggles `on` / `off`, choices their number |
| Rewrite | comments, unknown keys and line order are kept; duplicate keys collapse to the last; lines over 511 bytes, with control bytes, or not UTF-8 are dropped |
| Invalid file | no `version = 1` line, or over 64 KB: treated as absent |
| Newer version | `version = 2` and up is never overwritten; this build reads its settings through `azmod.cfg` |
| Writes | atomic and durable: `<path>.new`, fsync, rename, fsync of the directory (`azconf_save`) |

`azmod.cfg` (1.2.2's file) is still written beside it, byte for byte in that format (line 1
one `0`/`1` per legacy slot, `features.def`'s `cfg` column; line 2 the pad-mode colors; line 3
the pin mask by slot), so a rollback to 1.2.2 keeps its settings. At start:

| Found | Result |
|---|---|
| valid `azmod.conf` | it is the authority |
| no valid `azmod.conf` (absent, invalid, or newer) | 1.2.2's files are migrated (`azmod.cfg` by the slot-to-key map, the flag files, `azmod-theme`, `azmod-vinyl.v`, `azmod-btchannel`), applied, and `azmod.conf` saved; the old files are not touched |
| `azmod.cfg` bytes differ from `legacy.cfg-hash` in `azmod.conf` | another build wrote it (a rollback): the keys 1.2.2 knows are migrated again over the conf, every other key is kept |
| neither file | defaults, nothing written |

The theme and the vinyl value are `azmod.conf` keys (`theme`, `vinyl.brake`); a conf without
one takes it once from `azmod-theme` / `azmod-vinyl.v`. `azmod-btchannel` is still written and
read by its own code; `azmod.conf` carries a copy. Other features keep their own small files in the same
directory and are switched by `*.on` / `*.off` flag files there.

## 6. Data flow

**Panel input and LEDs.** The panel controls reach EP147 over SPI bus devices that EP147
reads itself. The `open` interposer records their descriptors:

| Device | Bus | Carries |
|---|---|---|
| `/dev/subucom_spi1.0` | ErP bus | deck buttons, pads, pad modes; the outgoing 128-byte LED frame |
| `/dev/subucom_spi3.0` | mixer bus | browse knob, click, BACK, EQ knobs and the x-pad strip (position and zone bytes defined in `drum_roll_display.c`) |
| `/dev/subucom_spi2.0`, `/dev/subucom_spi5.0` | deck MCU buses | jog platters: tracked always, read only for a community mod's filter |

The `read` interposer, on EP147's bus thread, decodes each frame after the underlying read
returns and before EP147 sees it: it tracks pad mode and deck selection, raises stem and drum
roll requests, drives Mod Menu and playlist gestures from the knob, and clears the bits EP147
must not see. A cleared frame gets a recomputed CRC-16/X25 only if it arrived with a valid
one, so a corrupted frame is never made acceptable. The `ioctl` interposer rewrites the
outgoing LED frame in flight with the colors the overlay thread published (`pad_leds.c`), lets
community mods edit their cells, and recomputes its CRC once. A watchdog on the overlay
thread's heartbeat releases the Mod Menu's knob, click and BACK swallowing on the mixer bus if
that thread stalls; the stem pad, drum roll pad and x-pad paths do not consult it.

**Pad press to stem toggle.** With a deck's stems engaged and its stems page shown, a rising
edge on pads 1 to 4 sets bits in `g_pad_req[deck]` (logical deck 0..3), a rising edge on pads
5 to 8 sets solo requests in `g_pad_solo_req[deck]`, and all eight pads are swallowed. The
overlay thread exchanges the request words to zero, flips `stem_on`, and `write_stems` writes
one mask per deck, as a decimal line:

| File | Deck |
|---|---|
| `/home/root/settings/azstem.stems` | 1 |
| `/home/root/settings/azstem.stems2` .. `azstem.stems4` | 2 .. 4 |

Bit 7 means engaged; bits 0..3 select vocals, inst, bass and drums. azstem's mask-poll
thread reads them into each deck's `ctrl` atomic, which the audio hooks consume. In the other
direction azstem publishes per-deck availability files (`azstem.avail`,
`azstem.avail2..4`) that azmod reads to light the pads and arm the displays. Drum roll
requests take the direct path: azmod calls the `azstem_drum_*` functions it resolved with
`dlsym`.

**Screen and library changes.** Mods reach EP147's widgets on the JUCE message thread:

- hooks on constructors and refresh functions that capture widgets and rewrite their
  `ImagesComponent` image index, followed by a repaint;
- vtable slot patches on specific classes (`Label::setText`, `paintOverChildren`, mouse
  slots);
- EP147's own caution managers, handed a private copy of a stock message definition for one
  request (`banner.c`).

Library edits use EP147's own SQLite connection on the target USB's library thread, then
invalidate EP147's list caches so the open screen re-reads (`library_edits.c`;
[docs/LIBRARY-EDITING.md](docs/LIBRARY-EDITING.md)). The Mod Menu itself is drawn by `menu.c`
into an off-screen buffer and pushed to override-redirect X windows sized to exactly what is
drawn (`mod_menu_overlay.c`).

## 7. azstem: the audio path

EP147 renders each deck in a JUCE audio graph node. The object that reaches the hooked
`processBlockInternal` is a `dj_player::DjPlayerUnit`, which inherits it from
`DjPlayerAudioProcessor`; six units run it, the four decks and two
`meow::AudioRecorderUnit`s. azstem hooks three levels of that path:

| Level | Hook | Does |
|---|---|---|
| Render tap | `hook_pbi`, wrapping `processBlockInternal` | marks the deck whose render is running on this thread before calling the original, then reads levels and writes the output: silences an engaged deck with every stem off or while paused, and mixes the drum roll voice in. Runs on the real-time audio thread and touches only atomics and preallocated buffers, plus one cached `gettid` call per thread. (The `azstem.mute` deck mask is a development-build input a release build never reads.) |
| Source-read injection | `pbread_hook`, wrapping `PageBuffer::read` | when a deck's stems are engaged, replaces the track PCM with the sum of the enabled stems, each scaled by its stem gain, at the source sample index corrected by the measured offset. Mask changes are crossfaded; with all four stems on at unity gain EP147's own PCM is left untouched |
| Key shift | `key_shift.c` | pitch shifting runs in `KeyControlFGPR`, which reads its source ahead of the playhead on its own threads, where the render tag is not visible. The hooks on its source setup and reads tag each read with the deck that owns that `KeyControlFGPR`, from an identity map built by construction, so injection is routed to the right deck on those threads too |

Because injection is below EP147's time-domain processing, tempo, key shift, key lock and
scratch are applied to the stems by EP147 itself. azstem's only pitch and rate processing is
linear resampling of stems and drum samples that are not 44.1 kHz at decode, and the drum roll
voice, which plays at a variable rate with linear interpolation and is mixed after EP147's
processing, so EP147 does not key-shift it.

**Deck and track identity.** The `DjPlayerUnit` constructor receives the player number and
seeds `g_deck_self[]` before the first audio block; the number is accepted only when its two
copies in the argument agree. The `gui::LoadState` constructor receives the deck and leads to
EP147's `LoadedTrackInfo` for the track name. azmod's `azmod_trackid_path` supplies file paths
captured from `AnalyzeManager::requestAnalyze`. A track change invalidates that deck's cached
mappings so a reused allocation is never mistaken for the old object.

**Decode.** On a track change the flusher looks for the track's stem folder under
`<stick>/STEMS/` on every USB, and ranks every candidate:

1. proven identity;
2. the same USB;
3. the name match (`<track>`, `<track>_` or `<track>-<8 hex digits>`, ignoring case).

The on-stick format is specified in [docs/STEMS-FORMAT.md](docs/STEMS-FORMAT.md). The flusher
then starts a loader thread. Each stem is decoded with stb_vorbis to interleaved 44.1 kHz
float, in parallel: a 44.1 kHz stem in 0.5 s chunks, a stem at any other rate whole and then
resampled. The real-time gate opens once every present stem has its first audio and the offset
is known; until then EP147's own audio plays unchanged. `ofsload.c` measures the offset
between the stems and EP147's own decode of the source (MP3 with EP147's libmpg123
configuration, AAC in MP4 through a separate path, `m4a_decode_native`) and applies only rules
listed in its tables; anything else plays with offset 0. Envelopes for azmod's per-stem mini
waveforms are filled progressively during the decode (`stem_envelopes.c`).

**Sample rate.** EP147's audio device runs at 96 kHz, but each deck renders, buffers and counts
its playhead at 44.1 kHz whatever the file's rate. A 48 kHz file is resampled before the page
cache, so `PageBuffer::read` positions are 44.1 kHz source samples for it too (measured with
stems injected at 44.1 kHz on 48 kHz tracks); EP147 keeps a {44100, 1/44.1} rate pair in its
playback snapshot; and the drum roll's 44.1 kHz samples, mixed into the deck output one frame
per frame, play at their own pitch. EP147 has no setting that changes this, so azstem names
the rate once, as `AZ_EP147_RATE` in `ep147rate.h`. Data EP147 builds with a rate of its own is
not assumed to use it: one of EP147's beat grid producers converts each beat's time with a rate
it is handed at run time, so before Beat Sync uses a grid, `drum_grid_native` measures the
grid's rate from its own tempo field (spacing times BPM over 60), converts it to the
playhead's time base, and refuses a grid whose spacing and tempo disagree.

## 8. Building and verifying

Both libraries are built in an Ubuntu 18.04 arm64 container with the compile lines in
`src/Makefile`, the lines official releases use. The image is not pinned (it starts from the
`ubuntu:18.04` tag and installs unversioned packages): reproducibility is measured by comparing
a build with an official release's hashes. [BUILDING.md](BUILDING.md) covers the toolchain
image, filling the firmware-derived templates, the build, and `verify.sh`.

`src/Makefile` has no test target. The test levels and their commands are in
[docs/TESTING.md](docs/TESTING.md):

| Tests | Command |
|---|---|
| unit tests | `tests/run.sh`, from the repository root |
| unit tests, all three passes (plain, ASan+UBSan, TSan) | `tests/run-linux.sh` |
| deck scripts | `deck/test/run.sh` ([deck/README.md](deck/README.md#tests)) |

The two host tests under `src/azmod/test/` also run on their own:

- the stem lane self-check, on the host against the real `menu.c`, from `src/azmod/`:

  ```sh
  cc -w -I. -o lanetest menu.c test/lanetest.c && ./lanetest
  ```

- the Community Mods host tests, in the build container (they build aarch64 code), from the
  repository root:

  ```sh
  docker run --rm --platform=linux/arm64 -v "$PWD/src":/g -w /g azmod-build:18.04 bash azmod/test/community/run.sh
  ```

## 9. Contributing notes

The stability practices for mods (identity resolved rather than guessed,
cached pointers validated at use, no blocking work on real-time and paint paths) are in the
SDK's [Guidelines](https://github.com/Kyle-Hosman/az-mods-sdk/blob/main/docs/GUIDELINES.md),
and section 2 shows where this code applies them. Specific to this tree:

- **Keep new symbols `static`.** A function or variable that is not `static` becomes a public
  export of the library and can interpose on EP147. Add a feature as a module `#include`d by
  `azmod.c` or `azstem.c`, with shared declarations in the matching `_internal.h` (or in
  `menu.h` when `menu.c` or `text.c` needs them). Export something only when another library
  or EP147 must find it, hide symbols that only assembly references (`.hidden` or
  `visibility("hidden")`), and update the export lists in `tests/unit/`.
- **Never write a bare EP147 address.** Use the `EP_<1.30 address>` name from `ep147addr.h`.
  A missing entry makes the whole library inert on that build, which is the intended failure.
  The tables are generated by a tool that is not included in this source; to propose a new
  address, open an issue or pull request against the table with the evidence for its value on
  every supported build ([docs/FIRMWARE-SUPPORT.md](docs/FIRMWARE-SUPPORT.md)).
- **Install hooks from the constructor.** Add a hook to `install_static_hooks`
  (`mod_menu_overlay.c`) or the constructor's early list; after `azm_hooks_go_late` only the
  island path is accepted. Treat a declined hook as "feature off" and leave EP147's behavior
  untouched.
- **Per-deck state is an array indexed by deck**, never a single global.
- **Gate new features.** Put a feature behind a Mod Menu row or a flag file, make the flag
  guard the work itself, and make the off state behave exactly like stock. Put diagnostics
  behind `AZ_PROBE_ON()` or `azlog_open()` so release builds stay silent.
- **Add a Mod Menu row in `features.def`.** One `AZ_FEATURE` line declares it; its place in
  the file is its place in the menu. The key is a stable public name: never rename or reuse
  one. The key table in [docs/MOD-MENU-KEYS.md](docs/MOD-MENU-KEYS.md) is generated from
  `features.def` (`tests/unit/keys_test.c --print`), and the unit tests fail when the two
  disagree. A new row has no legacy `azmod.cfg` slot (`cfg` -1); a retired slot stays
  as `AZ_RETIRED` and is never reused.
