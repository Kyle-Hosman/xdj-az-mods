# Stems format

Per-track stems live in `STEMS/` on a USB stick, one folder per track, four Ogg Vorbis
files per folder. rekordbox does not export stems and players do not read
them: this layout belongs to AZ-MODS, and other players ignore the folder.

| Side | Component | Source |
|---|---|---|
| Reader | `azstem.so` | `src/azstem/stem_playback.c` (folder search, identity, file roles, decode), `src/azstem/ofsload.c` (offset measured at load), `src/azstem/stem_envelopes.c` (envelope cache) |
| Writer | AZ-MODS desktop app | stem separation, and copying stems from one stick to another |
| Writer | Remote Stems helper | `deck/azrs/azrs.sh`: downloads a set separated on a computer and writes it onto the USB |

"Must" marks what the deck needs to play the stems. "Should" marks what the AZ-MODS
writers do, which keeps a set interchangeable with theirs.

## Layout

```
<stick>/
  Contents/...                  the tracks, wherever the library puts them
  STEMS/
    <folder>/                   one per track (see "Folder names")
      vocals.ogg
      other.ogg
      bass.ogg
      drums.ogg
      source.json               optional: which file the set was made from
      versions.txt              optional: what made the set
      azstem.env                written by the deck, never by a writer
```

| Rule | Detail |
|---|---|
| `STEMS` | a directory at the root of the USB, name matched ignoring ASCII case |
| USBs searched | on every track load, every entry one level below `/media/usb`, `/run/media`, `/mnt` and `/media`. A track's stems may be on a different USB from the track |
| Dot names | entries whose name starts with `.` are skipped everywhere: USB folders, stem folders, files |

**When stems are active.** azstem looks for a stems tree about every 5 seconds until it
finds one: a non-empty `STEMS/` (at least one entry not starting with `.`) on any mounted
USB. Until then no track loads stems. Once found, it stays found
for the rest of the session, and each track load searches every USB as above.

## The track key

A folder is matched to a track by the track's **key**: the last component of the
track's path as the deck sees it, with everything from the last `.` removed.

| Track path | Key |
|---|---|
| `Contents/Artist/Artist - Song.mp3` | `Artist - Song` |
| `Music/Song.v2.flac` | `Song.v2` |
| `Music/Intro .mp3` | `Intro ` (trailing space kept) |
| `Music/README` | `README` (no dot: the whole name) |

| Property | Rule |
|---|---|
| Source of the path | the path the deck's library holds for the track (the rekordbox export's). Take the name from the library, not from a directory listing: the two can differ in Unicode normalization (a precomposed letter in one, a letter plus combining accent in the other) |
| Comparison | bytes, ignoring ASCII case only. No Unicode normalization or Unicode case folding |
| Length limit | the absolute path (mount point included, for example `/media/usb/sda1/Contents/...`) is held in a 300-byte buffer and the file name in a 256-byte one. A longer path or name is truncated, the key comes out wrong, and the stems are not found |

The key comes from the file name alone, so two different songs with the same file
name in different folders share a key. `source.json` tells their sets apart.

## Folder names

A folder belongs to a track when it is a directory and its name is one of these,
compared ignoring ASCII case:

| Rank | Name | Used for |
|---|---|---|
| 0 | `<key>` | the normal case |
| 1 | `<key>_` | a key the USB cannot store as written |
| 2 | `<key>-XXXXXXXX` | `XXXXXXXX` is exactly 8 hex digits, either case: a second song with the same file name |

Nothing else matches. A bare prefix does not: the folder `Song (Extended)` never
belongs to the track `Song.mp3`, and neither does `Song-v2` or `Song_1`.

### When the AZ-MODS app appends `_`

FAT and exFAT cannot store some names as written, and a folder stored under a different
name would never match. The app appends `_` when:

- the key ends in `.` or a space, or
- the part of the key before its first `.`, with trailing spaces removed, is a Windows
  device name, ignoring case: `CON`, `PRN`, `AUX`, `NUL`, `COM1` to `COM9`, `LPT1` to
  `LPT9`.

| Track file | Key | Folder |
|---|---|---|
| `Song..mp3` | `Song.` | `Song._` |
| `Intro .mp3` | `Intro ` | `Intro _` |
| `CON.mp3` | `CON` | `CON_` |
| `con.remix.mp3` | `con.remix` | `con.remix_` |

The deck accepts `<key>_` for any key, so this rule is the writer's, not something the
deck checks.

### Keys that cannot have stems

The app refuses these, and other writers should too:

| Key | Why |
|---|---|
| empty or only spaces (a file named `.mp3`) | nothing to match |
| starts with `.` | the deck skips dot-named folders |
| contains `\ / : * ? " < > \|` or a control character below U+0020 | FAT and exFAT cannot store it, so no folder can carry the name |

### The 8-hex suffix

When a folder named `<key>` already holds a different song's stems, the app uses
`<key>-` plus the CRC-32 of the track's USB-relative path (UTF-8, forward slashes),
as 8 lowercase hex digits. The deck treats the digits as opaque: any 8 hex digits are
accepted, and `source.json` decides which song a set belongs to.

## How the deck chooses a folder

On each track load the deck collects every matching folder on every USB, checks each one's identity against the loaded file (see
[How identity is decided](#how-identity-is-decided)), discards any made from a
different file, and keeps the best of the rest by, in order:

1. identity proven before identity unknown;
2. on the same USB as the track's file before on another USB;
3. name rank, 0 before 1 before 2.

On a full tie, the first folder found in directory order wins.

The deck does not check that a folder is complete before choosing it. A matching folder
with no `.ogg` files in it wins over a complete set that ranks lower, and the track then
plays without stems. See [Writing a folder safely](#writing-a-folder-safely).

The browser's stems mark uses the same rules, but reads each stems directory listing
from a cache that is refreshed when the directory's modification time or
size changes, or after 60 seconds.

## Stem files

The deck reads the folder in directory order and keeps a file when its name does not
start with `.` and ends in `.ogg` (ignoring case). Everything else is ignored, including
`source.json`, `versions.txt` and `azstem.env`.

Each kept file gets a role from these substrings anywhere in its name, ignoring case,
tested in this order:

| Order | Role | Substrings | Pad |
|---|---|---|---|
| 1 | vocals | `vocal` | 1 |
| 2 | other (instruments) | `instrument`, `inst`, `other` | 2 |
| 3 | bass | `bass` | 3 |
| 4 | drums | `drum` | 4 |

- The first test that matches decides: `Bass Drum.ogg` is bass, `Instrumental
  Vocals.ogg` is vocals.
- A file that matches no role is ignored.
- Two files for one role: the first in directory order is used.

Writers should use exactly `vocals.ogg`, `other.ogg`, `bass.ogg` and `drums.ogg`, which
is what AZ-MODS writes and what its app looks for.

### Missing roles

| Situation | Deck behavior |
|---|---|
| A role has no file, or its file fails to decode | left out of the deck's "present" set; its stem pad stays dark |
| At least one file decodes | the deck plays stems |
| No file decodes | the track plays without stems |
| All four roles present, all four on, every stem at unity gain | the deck plays its own decode of the track, not the sum of the stems |
| A role missing | the deck plays the sum of the stems present, so that part of the music is absent even with every pad on |

The AZ-MODS app counts a folder as a finished set only when it holds a file named
`vocals`, `other`, `bass` and `drums` (any extension), and otherwise offers to make the
track's stems again. A third-party set should contain all four.

## Audio encoding

**What the deck accepts.** Ogg Vorbis, decoded with stb_vorbis to 32-bit float.

| Property | Deck behavior |
|---|---|
| Codec | Ogg Vorbis only; the `.ogg` extension is required |
| Sample rate | 44100 Hz is decoded progressively in 0.5 s chunks, one thread per stem, and stems start playing while the rest decodes. Any other rate is decoded whole and resampled to 44100 Hz with linear interpolation: slower to start, lower in quality |
| Channels | stereo as is; mono copied to both sides; more than two channels: the first two |
| Level | float samples beyond full scale are kept, not clipped |

**Memory.** Decoded stems are held in RAM, locked, at 8 bytes per frame per stem
(stereo float at 44100 Hz): four stems of a six-minute track take about 508 MB. Before
decoding, the deck adds up what the set needs. If that would leave less than 400 MB of
`MemAvailable` (the build-time constant `AZSTEM_MEM_FLOOR_MB`; the deck's current stems
count as free), or
`/proc/meminfo` cannot be read, the track plays without stems.

**What AZ-MODS writes.** Ogg Vorbis through libvorbis, 44100 Hz, stereo, at the
quality the app's Stem Quality setting selects:

| Stem Quality | Separation setting | libvorbis quality |
|---|---|---|
| Medium (default) | `fast` | 5 |
| High | `good` | 8 |
| Very High | `max` | 10 |

The track is decoded to 16-bit PCM at 44100 Hz stereo, separated, and each stem is kept
as 32-bit float until it is encoded, so peaks above full scale reach the Ogg file intact.
Third-party writers should match: Vorbis, 44100 Hz, stereo, any libvorbis quality.

## Timing and length

| Rule | Detail |
|---|---|
| Common start | frame 0 of every stem must be frame 0 of the track as decoded from the very start of the file. The deck plays stem frame *n* at the track's source sample *n* (at 44100 Hz), less the offset below. No trimming, start padding, time-stretching or fades |
| Same length | the deck uses the length of the shortest stem it decoded; past that point, while stems are engaged, it plays silence (except in the all-stems-on case under [Missing roles](#missing-roles), where the deck's own decode plays). All stems should be exactly as long as the decoded track; a longer stem's extra audio is never played |
| Offset | measured at load (below). Stems open about a second after the track loads while this runs, at most 15 s when the stems decode slowly; the track's own audio plays in the meantime |

**Offset.** Decoders disagree about an MP3's leading delay by a few samples to one
frame, so a set made with one decoder can sit slightly off the deck's own decode. The
deck cross-correlates the sum of the stems against its own decode of the first 20 s of
the file (`.mp3`, `.m4a` or `.mp4`, chosen by extension). It applies the correction only
when the correlation has a clear peak (at least 0.80, and 0.10 above the runner-up) and
the file type has a rule in `ofsload.c`; otherwise the offset is 0:

| File | Rates with a rule |
|---|---|
| CBR MP3, Info frame vendor `LAME` | 44.1 and 48 kHz |
| CBR MP3, Info frame vendor `Lavc` | 44.1 and 48 kHz |
| CBR MP3, Info frame vendor `Lame` | 44.1 kHz |
| AAC-LC in MP4 | 44.1 and 48 kHz |
| anything else (VBR, no Info frame, other encoders or rates, HE-AAC, other formats) | none: offset 0 |

The vendor match is case-sensitive. The deck decodes MP3 from the top of the file with
the encoder delay kept (no gapless trimming), so a writer that decodes the same way is
the closest match.

## source.json

Optional. It records which file a set was made from, so two different songs with the
same file name never play each other's stems, while copies of one song in different
folders can share a set. Without it, a set loads by name only (identity unknown).

```json
{"file": "Contents/Artist/Artist - Song.mp3", "size": 9123456, "mtime": 1767225600, "fp": "fnv1a64:0123456789abcdef"}
```

| Field | Type | Meaning |
|---|---|---|
| `file` | string | the track's path relative to the USB root, forward slashes, as the library holds it |
| `size` | integer | the track file's size in bytes |
| `mtime` | integer | the track file's modification time, seconds since the Unix epoch. Informational: neither the deck nor the app matches on it |
| `fp` | string | `fnv1a64:` followed by 16 lowercase hex digits: the [fingerprint](#fingerprint) |

What the deck's parser reads:

| Constraint | Detail |
|---|---|
| Shape | a single flat JSON object, UTF-8 |
| Size | the first 4095 bytes are read |
| Non-ASCII | escape as `\uXXXX`, with surrogate pairs above U+FFFF |
| Escapes in `file` | only `\"`, `\\`, `\/` and `\uXXXX`. Any other escape, `\u0000`, or a decoded `file` of 1024 bytes or more makes the record unknown |

The AZ-MODS app writes it as one line ending in LF, with Python's default `json.dumps`
escaping.

### Fingerprint

An identity for the audio in a file, with no decoding: 64-bit FNV-1a (offset basis
`0xcbf29ce484222325`, prime `0x100000001b3`) over, in order:

1. the file size in bytes, as ASCII decimal, followed by `\n`;
2. up to 65536 bytes from offset 0;
3. up to 65536 bytes from offset `max(0, size / 2 - 32768)` (integer division);
4. up to 65536 bytes from offset `max(0, size - 65536)`.

Each read stops at end of file; for small files the regions overlap and are hashed as
read. Reference implementation:

```python
def fingerprint(path):
    with open(path, "rb") as f:
        size = f.seek(0, 2)
        h = 0xcbf29ce484222325
        chunks = [f"{size}\n".encode()]
        for off in (0, max(0, size // 2 - 32768), max(0, size - 65536)):
            f.seek(off)
            chunks.append(f.read(65536))
        for data in chunks:
            for b in data:
                h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return f"fnv1a64:{h:016x}"
```

### How identity is decided

For each folder whose name matched, the deck compares `source.json` with the loaded
track file. The first step that applies gives the answer:

| Step | Condition | Answer |
|---|---|---|
| 1 | `file` matches and `size` equals the file's size | proven |
| 2 | `fp` present: the deck fingerprints the track file | equal: proven, whatever the path; different: different file; track file unreadable: unknown |
| 3 | `size` equals the file's size, or `file` matches | proven |
| 4 | `size` or `file` present | different file |
| 5 | none of the above, or no readable `source.json` | unknown |

When the deck cannot read the track file's size, every folder is unknown. A folder made
from a different file is never loaded; an unknown folder is loaded by name, ranked below
any proven one.

"`file` matches" means, on the deck, that the track's absolute path ends with `/`
followed by `file`, compared ignoring ASCII case. The AZ-MODS app compares the whole
USB-relative path, also ignoring ASCII case. For a correct USB-relative `file` the
two agree.

## versions.txt

Optional and informational; the deck ignores it. The AZ-MODS app reads it to show which
quality a set was made with, and copies it along when it copies a set to another USB.
One `<key> <value>` pair per line:

```
drums 1
bass 1
other 1
vocals 2
quality fast
ogg_q 5
level medium
vocals_model htdemucs
```

| Key | Meaning |
|---|---|
| `drums`, `bass`, `other`, `vocals` | the separation version of that stem, an integer; a later engine can use it to offer re-separating an outdated set |
| `quality` | the separation setting: `fast`, `good` or `max` |
| `ogg_q` | the libvorbis quality, 5 to 10 |
| `level` | the Stem Quality the set counts as: `medium`, `high` or `very_high` |
| `vocals_model` | the model that produced the vocals |

The app ignores keys it does not know. A third-party writer either omits the file or
writes only lines that are true for its output; without one, the app shows the set's
quality as unknown.

## azstem.env

After decoding a set, the deck writes `azstem.env` into the folder: a cache of the
per-stem waveform envelopes, read on the next load of the track so the stem waveforms
appear at once. It is rewritten after every decode.

| Offset | Size | Content |
|---|---|---|
| 0 | 7 | `AZENV1\n` |
| 7 | 12 | three little-endian u32: stem count (4), buckets per stem (65536), 0 |
| 19 | 8 | little-endian i64: the decoded length in frames, greater than 0 and at most 317,520,000 (two hours at 44100 Hz) |
| 27 | 262144 | 4 x 65536 bytes, stem-major (vocals, other, bass, drums): left-channel peak per bucket, 0 to 255, scaled so the loudest stem's peak is 255 |

A file that does not match this header is ignored. The cache is not tied to the audio
it was made from, so a writer that replaces the audio in an existing folder should
delete it; otherwise the old envelope shows until the new set finishes decoding. The deck writes it
as `azstem.env.tmp` and renames it into place; if the USB cannot be written, it skips
the cache.

## Writing a folder safely

The deck considers a folder as soon as it has a matching name and does not check that
it is complete, and a USB can be pulled at any moment. A writer therefore never lets a
matching name point at a partial set:

1. Work out the key and the folder name as above. Find an existing `STEMS` folder at the
   root ignoring case, or create `STEMS`.
2. Write everything into a staging folder inside `STEMS/` whose name starts with a dot,
   for example `STEMS/.Artist - Song.part/`. The deck and the AZ-MODS app skip dot names,
   and staging on the same volume makes the final step a rename. Write the four `.ogg`
   files, then `source.json` and `versions.txt`, and flush them to the USB.
3. If no folder of that name exists, rename the staging folder to it. That single rename
   is the moment the set appears.
4. If one exists (a re-made set), rename it out of the way to a dot name
   (`STEMS/.Artist - Song.old`), rename the staging folder into place, then delete the
   old one.
5. On macOS, delete the `._<name>` AppleDouble files the system creates beside each file
   on FAT and exFAT. The deck skips them; other tools may not.
6. On the next run, delete leftover dot-named staging folders from an interrupted write.

How the current writers do it:

| Writer | Procedure |
|---|---|
| Remote Stems helper (deck) | the procedure above: writes into `STEMS/.<folder>.part`, moves an existing folder to `.<folder>.old`, renames the new one into place, removes the old one and syncs. Leftover `.part` and `.old` folders are removed at its next start, on every USB paired for Remote Stems |
| AZ-MODS app, copying stems to another USB | copies into `STEMS/.azcopy-<folder>` and renames it into place. When the target folder exists, it moves each file over the existing one with one atomic replace per file, then writes `source.json` |
| AZ-MODS app, separating | works in `STEMS/.azmods-<random>/`. When every stem is encoded it publishes the set: a new folder appears with one rename; a re-made set swaps the whole folder (old one renamed to `.azold-<random>`, new one renamed in, old one deleted), so the old `azstem.env` goes with it. `source.json` is written into the published folder straight after, so for a moment a new set has none and the deck ranks it by name only. If the system refuses the folder rename (Windows, while a file in the folder is held open), it replaces the files one at a time |

## Requirements summary

| Item | Deck | AZ-MODS app |
|---|---|---|
| `STEMS/` at the USB root (any case) | required | required |
| Folder named `<key>`, `<key>_` or `<key>-XXXXXXXX` | required | required |
| At least one decodable role | required | all four required to count as made |
| File names `vocals.ogg`, `other.ogg`, `bass.ogg`, `drums.ogg` | substring match is enough | exact names (`vocals`, `other`, `bass`, `drums`) |
| Ogg Vorbis | required | written |
| 44100 Hz | recommended (other rates are resampled) | written |
| Stereo | recommended (mono is duplicated) | written |
| Common start, equal length | required for correct playback | written |
| `source.json` | optional; tells same-name songs apart | written; without it the set may be offered again when another song shares the name |
| `versions.txt` | ignored | optional; shown as the set's quality |
| `azstem.env` | written by the deck | not written |

## Worked example

A USB holds the track `Music/Artist - Song.mp3`, an MP3 of 9,123,456 bytes.

1. Key: the last component is `Artist - Song.mp3`; cutting at the last `.` gives
   `Artist - Song`. It does not end in `.` or a space, is not a device name and has no
   forbidden characters, so the folder is `Artist - Song`.
2. Separate the track into four stems from a 44100 Hz decode of the whole file and
   encode each as stereo Ogg Vorbis at 44100 Hz, all the same length.
3. Write the set into `STEMS/.Artist - Song.part/`:

   ```
   STEMS/.Artist - Song.part/
     vocals.ogg
     other.ogg
     bass.ogg
     drums.ogg
     source.json
   ```

   with `source.json`:

   ```json
   {"file": "Music/Artist - Song.mp3", "size": 9123456, "mtime": 1767225600, "fp": "fnv1a64:<16 hex digits from fingerprint()>"}
   ```

4. Rename `STEMS/.Artist - Song.part` to `STEMS/Artist - Song`.

When the track loads, the deck derives the key `Artist - Song`, finds
`STEMS/Artist - Song` at rank 0, finds that `source.json` names this file at this size
(proven), decodes the four stems and lights pads 1 to 4.

If the same USB also held a different song at `Other/Artist - Song.mp3`, its key would
also be `Artist - Song`. The deck would reject `STEMS/Artist - Song` for it (different
file), and that song's own set would go in `STEMS/Artist - Song-XXXXXXXX`, where the
AZ-MODS app uses the CRC-32 of `Other/Artist - Song.mp3` for the digits.
