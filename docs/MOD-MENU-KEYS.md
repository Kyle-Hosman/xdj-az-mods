# Mod Menu keys

Every official Mod Menu setting has a **key**: its name in `/home/root/settings/azmod.conf`
(`key = value` lines) and the name a community mod passes to `row_lookup()` to read it
(`src/azmod/azmod-mod.h`). The keys are a stable public interface: a key is never renamed or
reused, and a removed setting keeps its key reserved.

| Where | Form |
|---|---|
| `azmod.conf` | a toggle is `on` or `off`; a choice is its number |
| `row_lookup(key)` | a read-only handle; `-1` for an unknown key, a group or a hidden row |
| `row_value(handle)` | a toggle reads `1` only while it and every toggle above it are on; a choice reads its index |
| `row_set(handle, value)` | refused (`-1`) for an official row: only the DJ changes those |
| a community mod's own rows | saved in `azmod.conf` as `<mod-id>:<key>` |
| groups (`fps60`, `browse-view`, `extras`) | hold other rows only; no value |

The table is generated from `src/azmod/features.def` (release build) by
`tests/unit/keys_test.c`, run with `--print`. The unit tests (`tests/unit/run.sh`) fail when
the table and `features.def` disagree.

<!-- keys:begin (generated from features.def by keys_test.c --print, in the unit tests) -->
| key | label | kind | parent | values |
|---|---|---|---|---|
| `stems` | Stems | toggle | - | 0 = off, 1 = on |
| `stem-pads-page` | Stems on BEAT LOOP 2 | choice | stems | 0 = "BEAT LOOP", 1 = "BEAT LOOP 2" |
| `stem-pads-fx` | Bottom Stem Pads | choice | stems | 0 = "Solo", 1 = "FX" |
| `stem-fx-echo-length` | Echo Length | choice | stem-pads-fx | 0 = "-1", 1 = "-3/4", 2 = "-1/2", 3 = "-1/4", 4 = "1/4", 5 = "1/2", 6 = "3/4", 7 = "1" |
| `stem-fx-brake-length` | Brake Length | choice | stem-pads-fx | 0 = "1/2", 1 = "1", 2 = "2", 3 = "4" |
| `remote-stems` | Remote Stems | toggle | stems | 0 = off, 1 = on |
| `stem-marks` | Stem Marks | toggle | stems | 0 = off, 1 = on |
| `stem-mark-style` | Stem Mark Style | choice | stem-marks | 0 = "Color", 1 = "Mono" |
| `stem-volume-eq` | Stem Volume on CH 3/4 EQ | toggle | stems | 0 = off, 1 = on |
| `drum-roll` | Drum Roll | toggle | - | 0 = off, 1 = on |
| `drum-roll-bank` | Bank | choice | drum-roll | 0 = "1", 1 = "2", 2 = "3", 3 = "4" |
| `fps60-waveforms` | 60 FPS Waveforms | toggle | fps60 | 0 = off, 1 = on |
| `fps60-ui` | 60 FPS UI | toggle | fps60 | 0 = off, 1 = on |
| `playlist-editing` | Playlist Editing | toggle | - | 0 = off, 1 = on |
| `playlist-grab` | SHIFT + Hold to Move | choice | playlist-editing | 0 = "PRESS", 1 = "SHIFT + PRESS" |
| `themes` | Themes | toggle | - | 0 = off, 1 = on |
| `overlays` | Overlays | toggle | extras | 0 = off, 1 = on |
| `overlay-fps` | FPS | toggle | overlays | 0 = off, 1 = on |
| `overlay-cpu-temp` | CPU Temp | toggle | overlays | 0 = off, 1 = on |
| `overlay-cpu-usage` | CPU Usage | toggle | overlays | 0 = off, 1 = on |
| `overlay-cpu-clock` | CPU Clock | toggle | overlays | 0 = off, 1 = on |
| `overlay-ram` | RAM Usage | toggle | overlays | 0 = off, 1 = on |
| `overlay-clock` | Clock | toggle | extras | 0 = off, 1 = on |
| `overlay-clock-24h` | Time Format | choice | overlay-clock | 0 = "12-hour", 1 = "24-hour" |
| `overlay-clock-seconds` | Show Seconds | toggle | overlay-clock | 0 = off, 1 = on |
| `column-edit` | Column Edit | toggle | browse-view | 0 = off, 1 = on |
| `preview-hot-cues` | Preview Hot Cues | toggle | browse-view | 0 = off, 1 = on |
| `beat-jump-2-first` | BEAT JUMP 2 First | toggle | extras | 0 = off, 1 = on |
| `fine-vinyl-speed` | Fine Vinyl Speed Adjust | toggle | extras | 0 = off, 1 = on |
| `bt-auto-connect` | Bluetooth Auto-Connect | toggle | extras | 0 = off, 1 = on |
| `bt-remember-channel` | Remember Selected Channel | toggle | extras | 0 = off, 1 = on |
| `hide-track-title` | Hide Track Title | toggle | extras | 0 = off, 1 = on |
| `deck-key-title` | Key in Track Title | toggle | extras | 0 = off, 1 = on |
| `phase-meter` | Phase Meter | toggle | extras | 0 = off, 1 = on |
| `extended-lead-in` | Extended Lead-In | toggle | extras | 0 = off, 1 = on |
| `camelot-keys` | Camelot Keys | toggle | extras | 0 = off, 1 = on |
| `remember-waveform-zoom` | Remember Waveform Zoom | toggle | extras | 0 = off, 1 = on |
<!-- keys:end -->
