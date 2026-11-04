# Deck-side scripts

The shell scripts, units and tests that put the mods onto a deck and keep them running: they
find the USB stick, check what it carries, copy the mods into the deck, start the deck
software with them loaded, and run the boot hooks. The libraries they load are built from
`src/` ([BUILDING.md](../BUILDING.md)). The scripts are POSIX `sh` for the deck's busybox. The
USB layout, flag files and boot sequence are specified in
[docs/USB-STICK.md](../docs/USB-STICK.md).

| Path | What it is |
|---|---|
| `payload/` | what the AZ-MODS firmware installs: `usr/sbin/az-usbload` (the loader), `usr/sbin/az-usbpick` (picks the USB by physical port, USB1 first), the systemd units that run them, the `EP147.service` drop-in that sets the preload (`EP147.service.d/10-azmod.conf`), and `home/root/scripts/usb-renotify.sh` (re-announces mounted USB media to EP147 after a restart). `az-bootlog` (an opt-in boot snapshot, written to `MODS/az-boot.log` only when the USB holds `MODS/diag.on`) is in the tree but left out of the release firmware payload |
| `stick/deck/boot.d/` | the boot hooks, run in name order: `05-keep-testmode.sh`, `10-sshgate.sh`, `20-bootlogo.sh` (restores the stock boot logo on request), `30-uninstall.sh` |
| `stick/deck/lib/` | the helpers: the on-deck updater (`az-modupdate`), the Community Mods fetcher (`az-community`), the USB gadget re-announcement (`gadget-renotify.sh`), the shared log cap (`az-logcap.sh`), and dropbear's scheduling while EP147 starts (`az-sshprio`, run by the unit `10-sshgate.sh` writes) |
| `azrs/azrs.sh` | the Remote Stems helper azmod starts when tracks are queued for stemming; staged as `lib/azrs.sh` |
| `stage_stick.py` | lays all of the above out as the USB's `MODS/deck/` folder, comment-stripped and `sh -n` checked |
| `make_pins.py` | writes `deck/trust.pins` for a mod set and optionally signs it with minisign |
| `strip_payload.py` | the comment and blank-line strip that the firmware payload and the USB copy go through |
| `dropbear/` | the build script for the SSH server firmware 2.00 lacks ([dropbear](#dropbear)) |
| `test/` | tests for the loader, the hooks and the helpers ([Tests](#tests)) |

## What `stage_stick.py` writes

```sh
python3 deck/stage_stick.py <out_dir> [--version <v>] [--manifest <json>] [--no-guard]
```

`<out_dir>` becomes exactly the `MODS/deck/` folder:

| File | Source |
|---|---|
| `version` | `--version` (default `0.0.0`); logged by the loader and written to the USB's `MODS/.deck-info` as `deck=` |
| `az-usbload` | `payload/usr/sbin/az-usbload`: the firmware copy hands over to this one |
| `lib/az-usbpick`, `lib/usb-renotify.sh` | `payload/`, preferred over the firmware copies |
| `lib/azrs.sh` | `azrs/azrs.sh`, verbatim (it has a heredoc, which the strip refuses), still `sh -n` checked |
| `lib/az-handoff-guard` | `src/out/az-handoff-guard`, built first with `make CC=gcc azhandoff` in the build container; `--no-guard` leaves it out |
| `lib/*` | `stick/deck/lib/`, except dot files and `*.md`; ELF files and `*.LICENSE` copied verbatim |
| `boot.d/NN-*.sh` | `stick/deck/boot.d/` |

`--manifest <json>` also writes the file list with SHA-256s in the shape of a mods
`manifest.json` `files` array.

## How the scripts reach a deck

1. The AZ-MODS desktop app writes the mods to the USB stick: `MODS/azmod.so`,
   `MODS/azstem.so`, and `MODS/deck/` as `stage_stick.py` lays it out, with the signed
   `deck/trust.pins` from `make_pins.py`.
2. The deck software starts with the mod set it loaded last time, so a normal boot does not
   touch the USB stick early. The loader the firmware installed (`payload/`) runs after it
   and picks a USB (USB1 first) whose `MODS/` folder holds a core (`azmod.so` or
   `azmod-core.so`) or one of the files `azmods.cfg`, `uninstall`, `logo.restore`. When the USB carries `MODS/deck/az-usbload`, pinned and passing
   `sh -n`, the installed loader hands over to that copy, so a loader fix ships on the USB
   without reflashing. `MODS/deck/` is copied to RAM (`/tmp/az-deck`) first: nothing runs off
   the USB.
3. The loader runs or stages only files whose SHA-256 is in `deck/trust.pins`, and only when
   that file carries a genuine signature by the AZ-MODS release key; anything unpinned is
   removed from the RAM copy and never reaches the cache. The trust model and the developer
   override are in [docs/UPDATES-AND-SIGNING.md](../docs/UPDATES-AND-SIGNING.md).
4. The loader copies the USB's pinned `.so` files into its cache on the settings partition
   (`/home/root/settings/az-usbload`), writes the preload setting the deck software reads at
   start, and runs the `boot.d` hooks.

   | On the USB | Effect |
   |---|---|
   | `mods = off` in `MODS/azmods.cfg`, or `MODS/uninstall` | the deck goes back to stock, with a restart under the restart policy |
   | no mods USB at boot | the deck goes back to stock right away, with the early-boot restart (never over a loaded track) |
   | a mods USB with no usable set | stock is staged; it applies at the user's Reload or the next boot |
   | `deck = off` in `MODS/azmods.cfg` | the USB's loader and the hooks are skipped; the mods still load |
   | `MODS/community-mods/` (without each mod's `data/`) | mirrored into the same cache |
   | `community-mods = off` in `MODS/azmods.cfg` | no community mod loads |
   | `MODS/azmods.cfg` | every setting; the deck takes it over its own copy when it changed, and writes Mod Menu changes back ([USB-STICK.md](../docs/USB-STICK.md#azmodscfg-settings-and-switches)) |

5. A changed set takes effect at an EP147 restart. A deck already running mods is not
   restarted by the loader: the set is staged and applies at the user's Reload in the Mod
   Menu or the next boot. The full restart policy is in
   [docs/USB-STICK.md](../docs/USB-STICK.md).
6. While the deck runs, the loader watches for requests from the Mod Menu. The Updates
   screen and the Community Mods screens write a one-line request file, and the loader runs
   `lib/az-modupdate` or `lib/az-community` for it. A mods update is only staged: the screen
   says "Update installed. Reload to apply." A community "apply" is the user's own Reload
   (the Mod Menu asks first when a track is loaded) and restarts without waiting for the
   decks to empty. A community mod installed from the deck is approved by that action; a mod
   put on the USB any other way is shown as "New mod found: allow?" until the user allows it.

Every log the deck scripts append to is capped by `lib/az-logcap.sh`: past 64 KiB a log moves
to `<log>.1`, replacing the previous one. The loader sources that helper (it runs as root)
only from a copy that passes the trust check; a script that runs before any set was staged
has no helper and no cap.

To try a change by hand:

```sh
docker run --rm --platform=linux/arm64 -v "$PWD/src":/g azmod-build:18.04 make -C /g CC=gcc azhandoff
python3 deck/stage_stick.py <stick>/MODS/deck        # or --no-guard, without the guard
python3 deck/make_pins.py <stick>/MODS --sign <minisign secret key>
```

then reboot the deck with that USB in. `make_pins.py` pins every file under `deck/` and
every top-level `*.so`; without `--sign` only `trust.pins` is written and the set does not run.
A signed set is accepted only by a loader built with the matching public key; the alternative
is the developer override ([docs/UPDATES-AND-SIGNING.md](../docs/UPDATES-AND-SIGNING.md)).

## dropbear

SSH on the deck is off unless the USB holds `MODS/ssh.enable` and a key that has been
allowed at the deck ([docs/USB-STICK.md](../docs/USB-STICK.md#allowing-a-key)). Firmware 1.30 ships its own dropbear SSH server;
firmware 2.00 does not, so the USB brings one as `MODS/deck/lib/dropbear`, which
`10-sshgate.sh` uses only when `/usr/sbin/dropbear` is absent. It is built from the pinned
upstream release and is not in this repository. Building it needs Docker and the
`azmod-build:18.04` image ([BUILDING.md](../BUILDING.md)):

```sh
deck/dropbear/build-dropbear.sh /path/to/workdir
cp /path/to/workdir/dropbear deck/stick/deck/lib/dropbear
cp /path/to/workdir/LICENSE  deck/stick/deck/lib/dropbear.LICENSE
```

`stage_stick.py` then copies both into `MODS/deck/lib/`. `dropbear/README.md` has the upstream
version, the tarball and binary checksums, and the build options. The script refuses a binary
whose highest `GLIBC_` symbol is above the deck's 2.29.

## Tests

```sh
deck/test/run.sh                 # every suite
deck/test/run.sh hooks trust     # some
sh deck/test/clguard.sh          # the loader's crash-loop guard, on the host
```

`run.sh` first strips and stages the payload and the USB folder (with `--no-guard`) and runs
`sh -n` on every script, so the suites exercise what ships. `lint` (shellcheck, in its own
pinned container) and `clguard` run on the host; every other suite runs in a privileged
`alpine:3.20` container against real FAT loop images.

| Needs | Detail |
|---|---|
| Docker with `--privileged` containers and loop devices | Docker Desktop on macOS and Windows works; on Windows, run from Git Bash |
| Python 3.9+ on the host | `python3`, `python` or `py -3` |
| the repository on a path Docker can mount | on macOS, under the home folder: an unshared path shows up in the container as an empty folder and no test runs |
| a copy of a deck's root file system, named by `AZ_DECK_ROOTFS` | for `community`, `remotestems`, `sshprio` and the first part of `trust`, which run under the deck's own busybox, curl and shell; without it they fail rather than pass on a different userland |

| Suites |
|---|
| `lint`, `clguard`, `ports`, `mirror`, `hooks`, `poststart`, `gate`, `restart`, `bootlogo`, `uninstall`, `renotify`, `watch`, `trust`, `modupdate`, `community`, `remotestems`, `logcap`, `sshprio`, `boothooks` |

To skip the suites that need a deck root file system, name the others. Each suite pairs its
refusal cases with a control that must succeed in the same run.

| Result | Meaning |
|---|---|
| exit 0 | every suite printed `PASS` and none printed `FAIL` or `NO_DOSFSTOOLS` |
| `NO RESULTS from <suite> (instrument broken)` | the suite printed no `PASS` line; counted as a failure |
| exit 2 | another run holds the lock past `AZ_TEST_LOCK_WAIT`, or a suite container is already running |

Run one `run.sh` at a time per host: the suites share the host's loop devices, and a host-wide
lock (`/tmp/az-release-payload-tests.lock`) makes a second run wait for the first, up to
`AZ_TEST_LOCK_WAIT` seconds (default 3600; 0 refuses at once). A full run takes about 45
minutes.
