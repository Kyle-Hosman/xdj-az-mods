# Building AZ-MODS from source

The source of the AZ-MODS mods for the XDJ-AZ:

| Output | Built from | What it is |
|---|---|---|
| `azmod.so` | `src/azmod/` | the Mod Menu and every on-screen mod (Pad Colors, Overlays, Themes, Playlist Editing, the stem and drum roll pads, 60 FPS Waveforms and more) |
| `azstem.so` | `src/azstem/` | stems and drum rolls, played through the deck's own audio engine |
| `az-handoff-guard` | `src/azhandoff/` | the way back to the deck while a mod's program runs in EP147's place; rides the USB in `MODS/deck/lib/` |
| `MODS/deck/` | `deck/` | the loader and helper scripts that start the mods from the USB stick |

The AZ-MODS desktop app installs official builds of these files. How the libraries work
inside the deck is in [ARCHITECTURE.md](ARCHITECTURE.md); the USB formats and the systems
around the mods are in [docs/](docs/README.md).


## Not in this tree

| What | Why | Filled in by |
|---|---|---|
| UI art made from the firmware's own images: pad caption strips (`caption_strips.h`), Beat FX panel art (`fxart.h`, `fxhdr.h`, `fxbars.h`), the drum roll HOLD pad tile (`holdtile.h`), the Stem FX pad tiles (`sfxtile.h`), MENU panel button faces (`menulabels.h`), the Browse COLUMNS button (`collabels.h`), 4-deck stem strips (`stems4art.h`), the SOURCE and title screen masks (`srcref.h`, `wavref.h`) | firmware-derived bytes | `tools/extract-assets.py`, from the `assets/*.h.in` templates |
| The `exportLibrary.db` key (`onelibkey.h`, `onelibkey_sm.h`), used to mark tracks that have stems | not publishable | the same script and templates |
| Firmware and update files | not needed to build | |
| Development-only code | compiled out of the release build | |
| The generators of the project's own images (`eyeicon.h`, `stemsmark.h`, `menu_icons.h` and others) | project tools, not included | nothing: the headers are plain, editable data and are the source for those images; each names its generator |

A template carries the code, the array names, their sizes and a SHA-256 per array, but not
the bytes. Firmware-derived content stays out of contributions too; see the
[Guidelines](https://github.com/Kyle-Hosman/az-mods-sdk/blob/main/docs/GUIDELINES.md).

## Requirements

| | For |
|---|---|
| [Docker](https://www.docker.com/) | every build runs in the Ubuntu 18.04 arm64 image official releases are built in (`docker/azmod-build.Dockerfile`) |
| arm64 emulation on an Intel or AMD host | built into Docker Desktop (macOS, Windows); on Linux: `docker run --privileged --rm tonistiigi/binfmt --install arm64` |
| Python 3.6+ | `tools/extract-assets.py` |
| An official `azmod.so` of the same release as this source | filling in the templates |

## Build

**1. Toolchain image (once).**

```sh
docker build --platform=linux/arm64 -f docker/azmod-build.Dockerfile -t azmod-build:18.04 .
```

**2. Fill in the templates.** Take `azmod.so` from the official mods release that matches
this source (`mods-v<version>.tar.gz` on
[az-mods-updates releases](https://github.com/Kyle-Hosman/az-mods-updates/releases)). The
`MODS/azmod.so` on a deck's USB works when it is that same release.

```sh
python3 tools/extract-assets.py path/to/azmod.so
```

The script copies each array out of the `.so` by symbol name, checks it against the
template's SHA-256 and writes the header into `src/azmod/`. Exit 0: every header written and
verified. Exit 1: an array is missing or differs, so the `.so` is from another release. The
check is per array, so an `azmod.so` of another release whose arrays are unchanged also
passes and gives the same headers.

**3. Build.**

macOS and Linux:

```sh
docker run --rm --platform=linux/arm64 -v "$PWD/src":/g azmod-build:18.04 make -C /g CC=gcc azmod azstem
```

Windows (PowerShell):

```powershell
docker run --rm --platform=linux/arm64 -v "${PWD}\src:/g" azmod-build:18.04 make -C /g CC=gcc azmod azstem
```

| `make` target | Output |
|---|---|
| `azmod azstem` | `src/out/azmod.so`, `src/out/azstem.so` |
| `azhandoff` | `src/out/az-handoff-guard` (static) |
| `all` (the default) | all three |

`./build.sh` runs steps 1 and 3 for `azmod azstem` (building the image only when it is
missing) and prints both SHA-256s. It stops when step 2 has not been run.

Each target links to `<name>.full`, and the shipped file is that link stripped
(`--strip-unneeded`). The dynamic exports stay, and `azmod.so` also keeps the symbols of the
template arrays, which is how `extract-assets.py` finds them. The `.full` file keeps the whole
symbol table, for reading a crash address.

## Check a build against an official release

Each official mods release has a `manifest.json` with the SHA-256 of every file. Its `source`
block names the public repository, the commit the release was built from, and the build line.

1. Check out the commit named in `source.commit`.
2. Fill in the templates and build, as above.
3. Compare:

```sh
./verify.sh path/to/manifest.json
```

or by hand:

```sh
shasum -a 256 src/out/azmod.so src/out/azstem.so
```

`verify.sh` prints the release version and its source commit, then `MATCH` or `DIFFERENT`
per file.

| Exit | Meaning |
|---|---|
| 0 | `azmod.so` and `azstem.so` both equal the manifest's entries: the official files were built from this code |
| 1 | a file differs |
| 2 | usage error, the manifest cannot be read, or no `src/out/*.so` yet |
| 3 | the manifest is not a valid release manifest (not JSON, or no `azmod.so` / `azstem.so` entry) |

`verify.sh` covers `azmod.so` and `azstem.so` only. The rest of a release:

| File | Reproduced by this tree |
|---|---|
| `deck/lib/az-handoff-guard` | yes, by `make CC=gcc azhandoff`; compare `src/out/az-handoff-guard` with the manifest's `deck/lib` entry by hand |
| `azmod-pngpatch/` | no: image patches made from the firmware's own artwork, not published, like the templates |
| `deck/lib/dropbear` | built separately, by `deck/dropbear/build-dropbear.sh` ([deck/README.md](deck/README.md#dropbear)) |
| the other files in `MODS/deck/` | laid out from `deck/` by `deck/stage_stick.py`, with the release's version and changelog |

The image starts from the `ubuntu:18.04` tag and installs unversioned packages, so it is not
pinned: reproducibility is measured by this comparison, not guaranteed by the image.

## Run your own build on your deck

A deck loads only `.so` files and deck scripts that the release key signed, so a build of your
own is refused (the loader logs why). To test one on your own deck:

1. Install AZ-MODS on a USB stick as usual (the app or the release set), so `MODS/` holds the
   deck scripts the loader runs.
2. Copy your `src/out/azmod.so` and `src/out/azstem.so` over the ones in `<stick>/MODS/`.
3. Create the developer override on the deck over SSH:

   ```sh
   touch /home/root/settings/azmods-unsigned.on     # this deck loads unsigned builds
   rm /home/root/settings/azmods-unsigned.on        # back to signed builds only
   ```

4. Power-cycle the deck with the stick in, or run `systemctl restart az-usbload.service` over
   SSH: the loader copies the stick's libraries to the deck and restarts the deck program.
   Restarting only the deck program does not copy them.

The override affects only that deck, it also lets deck scripts that are not pinned run, and the loader logs `trust: DEVELOPER OVERRIDE` on every boot while
it is there. Details: [docs/UPDATES-AND-SIGNING.md](docs/UPDATES-AND-SIGNING.md#developer-override).
Community mods do not need it: see [Community Mods](docs/COMMUNITY-MODS-INTERNALS.md).

## Tests

| Suite | Command | Details |
|---|---|---|
| Unit tests (host-native, plus aarch64 suites in the build container) | `tests/run.sh` | [docs/TESTING.md](docs/TESTING.md) |
| Unit tests, all three passes (plain, ASan+UBSan, TSan) in `azmod-test:18.04` | `tests/run-linux.sh` | [docs/TESTING.md](docs/TESTING.md) |
| Stem lane self-check and Community Mods host tests, on their own | see [ARCHITECTURE.md](ARCHITECTURE.md), section 8 | |
| Deck scripts | `deck/test/run.sh` | [deck/README.md](deck/README.md#tests) |

`tests/run.sh` and `tests/run-linux.sh` need the headers step 2 writes.

## Layout

| Path | What it is |
|---|---|
| `src/azmod/` | `azmod.so`: the Mod Menu and the on-screen mods |
| `src/azstem/` | `azstem.so`: stems and drum roll audio (with stb_vorbis, public domain / MIT) |
| `src/common/` | header-only code shared by both libraries: firmware identification, page protection, threads, process spawning, log caps |
| `src/azhandoff/` | `az-handoff-guard` and its decision logic |
| `src/azmod/test/` | host tests for azmod: `lanetest.c`, `community/` |
| `src/Makefile` | the release compile lines |
| `src/.clang-tidy`, `.clang-format` | the static check's and the formatter's configuration |
| `assets/` | the templates for the firmware-derived images and the database key |
| `docker/` | `azmod-build.Dockerfile` (the release toolchain) and `azmod-test.Dockerfile` (the unit tests' Linux image) |
| `deck/` | the deck-side loader, scripts, units and their tests ([deck/README.md](deck/README.md)) |
| `tests/` | the unit tests (`tests/unit/`) and their runners |
| `tools/` | `extract-assets.py`, `ep147-pngtable.py`, `format-check.sh`, `static-check.sh` with `static-check/`, `build-check.sh`, the fuzz harnesses in `fuzz/`, `public-hygiene`, `install-hooks.sh` |
| `docs/` | the USB's file formats and the systems around the mods ([docs/README.md](docs/README.md)) |

The mods hook the deck's own software at fixed addresses, one table per supported firmware
(`src/azmod/ep147addr.h`, `src/azstem/ep147addr.h`, selected by `src/common/ep147build.h`).
On a firmware without a complete table they stay inert; see
[docs/FIRMWARE-SUPPORT.md](docs/FIRMWARE-SUPPORT.md).

## Contributing

Bug reports, pull requests and the checks a change passes first: [CONTRIBUTING.md](CONTRIBUTING.md).
The conventions specific to this tree are in [ARCHITECTURE.md](ARCHITECTURE.md), section 9.

## License

GPL-3.0-only. See `LICENSE`, and `NOTICE` for the third-party parts.
