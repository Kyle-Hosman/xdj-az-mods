# Community Mods internals

The deck-side machinery behind Community Mods: the fetcher that browses repositories and
installs packages onto the USB, the files it shares with the Mod Menu, how the loader
carries installed mods onto the deck, and how the core loads, checks, quarantines and blames
them. Writing a mod, its API, the package, `azmod.json` and publishing a repository are in the
[AZ-MODS SDK](https://github.com/Kyle-Hosman/az-mods-sdk)
([Packaging and publishing](https://github.com/Kyle-Hosman/az-mods-sdk/blob/main/docs/06-publishing.md));
the stability practices for a mod's code are in its
[Guidelines](https://github.com/Kyle-Hosman/az-mods-sdk/blob/main/docs/GUIDELINES.md).

| Part | File | Runs |
|---|---|---|
| Fetcher | `deck/stick/deck/lib/az-community` | shell script under the loader's request watcher, outside EP147 |
| Watcher and mirror | `deck/payload/usr/sbin/az-usbload` (`modupdate_watch`, `community_mirror`) | at boot, and while waiting for requests |
| Core host | `src/azmod/community.c` | inside EP147, on the overlay thread |
| Mod services | `src/azmod/sdk_*.c` (part of the azmod unit, beside `community.c`) | inside EP147 |
| Mod Menu screens | `src/azmod/screen_community.c` (part of `menu.c`'s translation unit) | inside EP147, on the overlay thread |
| Tile icons | `src/azmod/community_icons.c` | inside EP147: decoded on its message thread, drawn by the overlay thread |
| ABI | `src/azmod/azmod-mod.h` | the same file as the SDK's `include/azmod-mod.h` |

## Trust model

A community mod runs in EP147's address space with the same access as the core; a shared
library is not a sandbox. The design rests on four properties:

| Property | Where |
|---|---|
| What cannot be vouched for is refused before any of its code runs | [Install](#install), [The start checks](#the-start-checks) |
| A mod that crashes or freezes the deck is suspended at the next start | [Quarantine and crash blame](#quarantine-and-crash-blame) |
| The DJ can always get back to official-only mods from the deck | [The trust screen and the switches](#the-trust-screen-and-the-switches) |
| Authors get a validator (the SDK's `azmod-pack`) that runs the checks the deck runs | the SDK |

Three structural rules follow:

- Nothing downloaded executes outside EP147, and the fetcher executes nothing.
- The fetcher writes the USB, plus one file on the deck: the [approved list](#approved-mods).
  The deck's copy of the mods (the stage) is written by the loader alone, when the user applies
  a change or at the next boot, so an install never takes effect through an unplanned EP147
  restart.
- A USB is not the deck's: anyone can write one. Turning a mod on in `MODS/azmods.cfg` is
  not enough to run it. The core loads a mod only when the deck's own approved list holds its
  id with the SHA-256 of exactly its `<id>.so`.

## Overview

```
 Mod Menu (azmod, in EP147)          watcher + fetcher (outside EP147)          network
 --------------------------          ---------------------------------          -------
 writes /run/az-community.req  --->  az-community <request>             <--->  azmods-repos.json
 reads  /tmp/az-community/status <-- writes status, catalog, icons             azmods-repo.json
 reads  /tmp/az-community/catalog                                              icons, packages
                                     writes MODS/community-mods/ on the stick
 "apply" ------------------------->  loader re-run: MODS/community-mods/ -> stage
                                     restart EP147
 core loads from the stage  <------
```

## Files and folders

On the USB:

```
MODS/
  community-mods/
    repos.txt                repositories the user added, one per line, as typed
    <id>/
      azmod.json             the mod's manifest
      <id>.so                the mod
      icon.png               its icon
      shots/                 optional screenshots
      assets/                optional files the mod reads (kept on the stick only)
      LICENSE                optional
      .origin                written by the installer (see ".origin")
      data/                  the mod's own files (data_dir), created by the core; never in a
                             package, never copied to the deck, kept by an update
      .data.owner            the owner data/ belongs to (see "One id, one owner")
      .data@<hash>/          another owner's data/, set aside
  azmods.cfg                 community-mods.<id> = on|off per mod, community-mods = off for all
                             (USB-STICK.md, "azmods.cfg")
```

On the deck:

| Path | Written by | Contents |
|---|---|---|
| `/home/root/settings/az-usbload/community/` | loader | the stage: a copy of `MODS/community-mods/` without each mod's `assets/`, `data/`, `.data.owner` and `.data@*`, `._*` files and top-level dot entries, plus `enabled` (the ids `azmods.cfg` turns on) and `<id>/<id>.off` (each mod it turns off), written from `azmods.cfg` |
| `/home/root/settings/az-usbload/community-mods.off` | loader | present when `azmods.cfg` says `community-mods = off` |
| `/home/root/settings/azmod-community/` | core, Mod Menu | per-mod settings, the approved list and the quarantine state ([table](#quarantine-and-crash-blame)); the fetcher writes only `approved` |
| `/tmp/az-community/` | fetcher | status, catalog, icons, caches, log, lock |
| `/tmp/azmod-community/` | core | per-mod logs and the load result file `loaded` |
| `/run/az-community.req` | Mod Menu | one request line |

## The fetcher

`az-community` serves one request per run. The watcher passes the request line as a single
argument; the script splits it on spaces with globbing off.

| Request | Does |
|---|---|
| `refresh` | fetch the curated list, every repository file and the icons; rebuild the catalog |
| `install <repo> <id>` | download the mod's package from the cached repository file entry, check it, install it into `MODS/community-mods/<id>/`, set `community-mods.<id> = on` |
| `remove <id>` | delete `MODS/community-mods/<id>/`, its `community-mods.<id>` key and its approved-list line |
| `enable <id>` | set `community-mods.<id> = on` in `MODS/azmods.cfg` |
| `disable <id>` | set `community-mods.<id> = off` |
| `addrepo <repo>` | add a repository to `repos.txt`, then refresh |
| `delrepo <repo>` | remove a repository from `repos.txt`, rebuild the catalog |
| `apply` | ask the watcher to apply the USB's community folder (state `reloading`) |
| `shots <repo> <id>` | download the mod's screenshots (the mod's page asks when it opens; a refresh never does) |
| `catalog` | rebuild the catalog from the caches, without the network |
| `settle` | run by the watcher after an apply: turns `reloading` into `done` or `error` |

### Repository addresses

| Form | Means |
|---|---|
| `owner/name` | `https://raw.githubusercontent.com/owner/name/HEAD/azmods-repo.json` |
| `https://github.com/owner/name` (also `http://github.com/...` and `github.com/...`, with or without a trailing `/` or `.git`) | the same |
| `https://...azmods-repo.json` | used as is |

- At most 256 characters from `A-Za-z0-9._~/:%+-`.
- `owner` matches `[A-Za-z0-9][A-Za-z0-9-]{0,38}`, `name` matches `[A-Za-z0-9_][A-Za-z0-9._-]{0,99}`.
- All spellings of one repository are one repository: `install` and `delrepo` accept any of
  them, and `repos.txt` keeps the one the user typed.
- Two `raw.githubusercontent.com` URLs that differ only in letter case are one repository
  wherever repositories are compared (the repository set, `addrepo`, `delrepo`, `.origin`, the
  core's owner stamps), since GitHub ignores case in `owner/name`. Any other URL compares exactly.
- `addrepo` refuses an address starting with `http://` ("Repository addresses must start with
  https://.").

### Every run

- Takes a lock (`/tmp/az-community/lock`). A second run started while one is active logs
  "busy" and exits without doing anything.
- Finishes or undoes an install that a power loss interrupted ([Interrupted installs](#interrupted-installs)).
- Exits with status 0. The answer is always the [status file](#the-status-file).

### Network rules

Every download goes through one function (`get`):

| Rule | Value |
|---|---|
| Protocol | HTTPS only, for the first request and every redirect |
| Redirects | at most 5 |
| Connect timeout | 10 s |
| Stall | abandoned below 1 KB/s for 10 s |
| Total time | 10 s for lists, indexes and icons; 300 s for a package |
| Size | capped by curl (`--max-filesize`) and checked again on disk; a partial file never survives a failure |

Reachability is probed with `https://github.com` before a refresh.

JSON is never evaluated: a tokenizer (`grep -E`) and an `awk` state machine turn each
document into `path TAB type TAB value` lines, and values are only ever data. Strings are
decoded (`\uXXXX` included) and every control character becomes a space. A UTF-8 byte-order
mark at the start is skipped.

### Limits

| Item | Limit | Beyond it |
|---|---|---|
| Curated list `azmods-repos.json` | 64 KB | the list is not updated (the last good copy is kept) |
| Download counts `stats.json` | 256 KB, 16384 counts | the counts are not updated (the last good copy is kept) |
| Repositories taken from the curated list | the first 64 entries | ignored |
| Repositories in `repos.txt` | 32 | `addrepo` refuses ("at most 32 repositories"); extra lines are not read |
| Repository address | 256 characters | refused |
| Repository file `azmods-repo.json` | 256 KB | the repository shows "the repository file is larger than 256 KB" |
| Mods per repository | 256 | further entries skipped |
| JSON nesting / tokens | 16 levels / 100000 tokens | the document is refused |
| Repository file `name`, `maintainer`, mod `name`, `author` | cut to 64 bytes when parsed | |
| Repository file `summary` | cut to 200 bytes when parsed, then to 119 in the catalog | |
| Repository file `description` | cut to 800 bytes when parsed, then to 599 in the catalog | |
| URLs (`package`, `source`, `icon`, `homepage`) | `https://`, at most 512 characters, URL-safe characters | entry skipped (`package`, `source`) or field dropped |
| Icon | 256 KB, PNG signature | not shown |
| Icons kept in `/tmp` | 8 MB (`AZ_COMMUNITY_ICON_CAP`), oldest dropped | |
| Icon fetching per refresh | 60 s (`AZ_COMMUNITY_ICON_BUDGET`) | the rest next refresh |
| Package download | 16 MB, and never more than the repository file's `size` | refused |
| Package entries | 256 | refused |
| Unpacked package | 16 MB in total | refused |
| Member name | 128 bytes, at most 3 `/` | refused |
| `azmod.json` in a package | 16383 bytes (what the core reads) | refused at install |
| Mods in the catalog | 128 (`CAT_MODS`), those installed from their repository first | counted on a closing `more` line |
| Free space on the deck's settings partition after an install | 12 MB (`AZ_COMMUNITY_P7_RESERVE_KB`) | refused |
| Free space on the USB | the unpacked size plus 1 MB | refused |
| Fetcher log | 64 KB, then moved to `log.1` | |

The Mod Menu holds 96 repository lines (`CM_NREPO` in `screen_community.c`: the 64 curated
plus the 32 a user can add) and 128 mod lines (`CM_NCAT`, equal to `CAT_MODS`). Lines past
its arrays are reported, never dropped silently: mod lines past the array, plus the count on
the catalog's `more` line, as "Showing the first N of M mods." on the Browse tab, and
repository lines past the array as "Showing the first N of M repositories."

### Refresh

The Mod Menu asks for a refresh when the full Community Mods screen opens: the first time after
each start, then at most once every 10 minutes, at once again when the last refresh did not end
clean (an error, or a repository or the Mod Index not read), and never while the fetcher is busy.
There is no Refresh button; while a refresh runs, the screen's left rail shows it, turning.

1. Probe the network. Offline: rebuild the catalog from the caches and report "No internet:
   showing mods from the last refresh. Connect the deck to WiFi, then open Community Mods again."
2. Fetch the curated list (`LIST_URL`, overridden by `AZ_COMMUNITY_LIST`; default
   `https://raw.githubusercontent.com/Kyle-Hosman/az-mods-index/HEAD/azmods-repos.json`).
   HTTP 404 means "no curated repositories", an answer rather than a failure. Any other
   failure keeps the copy from the last good refresh.
3. Compose the repository set: the curated list first, then `repos.txt`, de-duplicated on
   the normalized address. A repository in both counts as curated.
4. Delete the caches of repositories no longer in the set.
5. Fetch and parse every repository file. A repository that fails keeps its last good
   parse; its state records why.
6. Fetch the icons the catalog can show, within the time and size budget.
7. Write the catalog. A clean refresh reports nothing (the screen counts the catalog itself,
   "N mods" in its header); otherwise the message names only the problems: "K of R
   repositories could not be read." and "The Mod Index did not load. Open Community Mods again
   to retry."

### The repository file rules

A repository file is accepted only when it is valid JSON with `"azmods_repo": 1`. Each `mods` entry
must pass every rule below; an entry that fails is skipped and logged, and the rest of the
repository is still listed.

| Field | Rule |
|---|---|
| `id` | string, `[a-z0-9-]`, 3 to 32 characters, not reserved (`enabled`, `azmod`, `azstem`, `az-mods`, `azmods`), unique in the repository file |
| `name` | non-empty string |
| `version` | `MAJOR.MINOR.PATCH`, numbers only, each 1 to 4 digits (the only form the core compares for updates) |
| `package` | `https://` URL, at most 512 characters, URL-safe characters only |
| `sha256` | 64 hex digits (either case; stored lower case) |
| `size` | integer, 1 to 16777216 |
| `firmware` | array of strings: one or more `ep147-` + 16 lowercase hex digits, or exactly `["any"]` |
| `source` | `https://` URL, as for `package` |
| `hooks` | absent, or a boolean; `true` is refused with `["any"]` |
| `min_core` | absent, or `MAJOR.MINOR.PATCH` as for `version` |

Optional fields are taken when well formed and dropped otherwise:

| Field | Form |
|---|---|
| `author`, `summary`, `description` | strings |
| `icon` | `https://` URL |
| `category` | `visual`, `controls`, `audio` or `tools` |
| `updated` | `YYYY-MM-DD` |
| `assets_kb` | a non-negative integer |
| top-level `name`, `maintainer`, `homepage` | kept for display (`homepage` must be an `https://` URL) |

The format from the author's side is in the SDK's
[Packaging and publishing](https://github.com/Kyle-Hosman/az-mods-sdk/blob/main/docs/06-publishing.md).

The curated list is `{"azmods_repos": 1, "repos": [...]}`, each entry a repository address
the fetcher accepts; an entry it refuses is logged and skipped. Its format and review rules
are in the [az-mods-index](https://github.com/Kyle-Hosman/az-mods-index) README.

### The cache

`/tmp/az-community/cache/`, in RAM. A repository's files are named by `<h>`, the first 16
hex digits of the SHA-256 of its normalized repository file URL.

| File | Contents |
|---|---|
| `community.txt` | the curated list from the last good fetch, one address per line |
| `repos.cur` | the composed set: `url TAB listed TAB user TAB <h> TAB text` |
| `installed.cur` | the installed mods: `id TAB version TAB origin-url TAB enabled`; a folder with an `azmod.json` and no `.origin` (copied by hand) is listed with origin `-` |
| `winners` | `id TAB url`: the repository the catalog shows for each id; `install` refuses any other |
| `<h>.parsed.new` | the parser's raw output (`META`, `MOD`, `SKIP`, `COUNT` lines); transient |
| `<h>.mods` | one tab-separated line per valid mod, 18 fields: id, name, version, author, summary, category, icon URL, package URL, sha256, size, firmware (space-separated), hooks (0/1), source, updated, icon hash, min_core, description, assets_kb |
| `<h>.meta` | `name TAB maintainer TAB homepage` of the repository |
| `<h>.count` | `valid TAB skipped` entry counts |
| `<h>.state` | `ok TAB <note>` or `error TAB <reason> [TAB <HTTP code>]` |
| `icons.want` | icon URL and hash per icon the catalog can show; rewritten at every refresh |
| `stats.tsv` | the download counts from the last good `stats.json`: `repository TAB id TAB count` |

The raw repository file and its flattened form are deleted as soon as they are parsed. Icons are
stored as `/tmp/az-community/icons/<hash>.png`, `<hash>` being the first 16 hex digits of the
SHA-256 of the icon URL.

## The files the Mod Menu reads

The Mod Menu is C and never parses JSON from the network: the fetcher flattens everything
into line files. Every value has `|`, CR and LF removed and is cut to its byte cap without
splitting a UTF-8 sequence.

### The catalog

`/tmp/az-community/catalog`, rewritten whole (temporary file, then rename) after every
change. The first line is `v1`; a reader that finds anything else shows nothing. Then one
line per repository and one per mod, fields separated by `|`:

```
v1
repo|<repo>|<name>|<ok>|<listed>|<error>
mod|<repo>|<id>|<name>|<version>|<author>|<summary>|<category>|<hooks>|<firmware>|<size>|<icon>|<installed>|<min_core>|<description>|<assets_kb>|<held>|<also>|<nalso>|<updated>|<downloads>|<beta>|<shots>
more|<n>
```

**One line per id.** An id is one folder on the USB, one line in `enabled`, one settings
file and one data folder, so two repositories offering the same id cannot both be
installed. The catalog shows the line from, in order of preference:

1. the repository the installed copy came from (its `.origin`), so its updates, Disable and
   Remove stay reachable;
2. a repository in the Mod Index, over one the user added;
3. the earlier repository in the set.

The other repositories offering the id are counted on that line (`also`, `nalso`), and
`install` refuses them.

**The cap.** At most 128 mod lines. When the repositories list more, the mods installed from
their repository are written first (so their updates stay visible), then the rest in
repository order, and a closing `more|<n>` line gives how many were left out. Without that
line nothing was left out.

Repository line:

| # | Field | Cap | Meaning |
|---|---|---|---|
| 1 | `repo` | | the literal tag |
| 2 | repo | 256 | the repository as typed or listed; `install` and `delrepo` accept it back |
| 3 | name | 47 | the repository file's `name`, else the short address (`owner/name`) |
| 4 | ok | | `1` when the last fetch parsed, else `0` |
| 5 | listed | | `1` when the repository is on the curated list |
| 6 | error | 111 | why it could not be read (`not read yet` before the first fetch); empty when ok |

Mod line (23 fields):

| # | Field | Cap | Meaning |
|---|---|---|---|
| 1 | `mod` | | the literal tag |
| 2 | repo | 256 | the repository, as in its `repo` line |
| 3 | id | 32 | the mod id |
| 4 | name | 47 | |
| 5 | version | 23 | |
| 6 | author | 47 | |
| 7 | summary | 119 | |
| 8 | category | | `visual`, `controls`, `audio`, `tools` or empty |
| 9 | hooks | | `1` when the repository file says the mod patches code |
| 10 | firmware | | the tags, comma-separated, or `any` |
| 11 | size | | the package size in bytes |
| 12 | icon | | the local PNG path under `/tmp/az-community/icons/`, or empty until fetched |
| 13 | installed | 23 | the installed version, only when it was installed from this repository; else empty |
| 14 | min_core | | `MAJOR.MINOR.PATCH` or empty |
| 15 | description | 599 | one line |
| 16 | assets_kb | | how much the mod keeps on the USB, in KB; `0` when none or not stated |
| 17 | held | 256 | the id is installed from another repository (its short address), or `-` for a folder copied by hand; empty otherwise. Install is refused until that mod is removed |
| 18 | also | 256 | the first other repository offering the same id, as in its `repo` line; empty when none |
| 19 | nalso | | how many other repositories offer the same id |
| 20 | updated | | the repository file's `updated` date, `YYYY-MM-DD`, or empty |
| 21 | downloads | | the mod's download count from the Mod Index (see below), digits only; empty when there is none |
| 22 | beta | | `1` when the repository file says `"beta": true` (the author's word that the mod is not proven stable yet), else `0` |
| 23 | shots | | the local paths its screenshots are downloaded to (`/tmp/az-community/shots/<hash>.png`, space-separated, at most 3), or empty. The repository file's `screenshots` are 1 to 3 `https://` URLs; a list that breaks a rule drops the screenshots, never the mod. `shots` downloads them (PNG, at most 1 MB each, a PNG signature check); the deck shows them at most 1280 x 800 |

Fields are only ever added at the end. A reader accepts a `mod` line with 13 or more fields
(a `repo` line with 6 or more) and treats missing trailing fields as empty; the Mod Menu
does, and reads lines of up to 4095 bytes.

The Mod Menu decides "works on this deck" itself: the firmware field is `any`, or it holds
the running firmware's tag. It offers Install only when the mod works on this deck and its
`min_core` is not above the core's `AZ_CORE_VERSION`. Browse lists those mods first.

**Browse controls.** A row of view buttons: Home, All mods, Visual, Controls, Audio and Tools,
then Other when it holds any mod; each but Home with the number of mods it holds. Other holds the
mods with no category. A Sort menu at the end of the row picks the order.

**Home** (the view Browse opens on) is a list of shelves, each a title, "See all" and a row of
at most 12 tiles, five on screen; a sideways drag scrolls a row, an up or down drag the page:

| Shelf | Mods | See all opens |
|---|---|---|
| Most downloaded | mods with a download count above 0, most first | All mods, by Downloads |
| New & updated | mods with a valid `updated` date, newest first | All mods, by Updated |
| Visual, Controls, Audio, Tools, Other | that category's mods, by Downloads | that category, by Downloads |

A shelf with fewer than 2 mods is not shown. Turning the
browse knob moves a focus through the tiles in reading order (shelf by shelf, left to right),
scrolling the row and the page to keep it in view; pressing it opens the focused mod.

**All mods** and each category show the mods as a grid, in one of four orders (the Sort menu):

| Order | Mods |
|---|---|
| A-Z | by name, ignoring the case of ASCII letters |
| Updated | by the `updated` date, newest first; mods without a valid date after the dated ones |
| Installed | installed mods first, then by name |
| Downloads | by download count, most first; mods without a count after the counted ones, by name |

In every order, and on every shelf, the mods that cannot be installed here follow the ones that
can, except that Installed keeps every installed mod first; mods that compare equal keep the
catalog's order. The view and order last until EP147 restarts.

A tile shows the mod's icon, name and author, its state (Installed, Update available, ...) and
its download count; a mod with no count shows none, never 0. Its page shows the icon, the name,
"author . category . version, updated N days ago" (the date itself while the deck's clock is not
set), the download count, the repository, Install / Update to X / Disable / Remove, the summary,
whether it works on this deck, then the description with Permissions beside it (whether it uses
hooks, its firmware tags, its download size and the files it keeps on the USB).

**Download counts.** The Mod Index publishes `stats.json` beside its list:
`{"azmods_stats": 1, "generated": "YYYY-MM-DDTHH:MMZ", "mods": {"https://github.com/OWNER/REPO":
{"<id>": <downloads>}}}`. Each count is GitHub's download count of the mod's `<id>-<version>.azmod`
release assets over all its versions (from decks, the desktop app or by hand, updates and
reinstalls included), taken every 6 hours by a scheduled workflow in the index's repository.
The fetcher downloads it with the list on every refresh and keeps the last good copy, in `/tmp`
and on the USB as `MODS/community-mods/.stats` (a dot file: the loader never copies it to the
deck, and no id starts with a dot), so a deck that starts offline still shows the last counts.
A count is used only for a repository on the curated list and only as 1 to 13 digits; anything
else is dropped, never guessed. Decks never call the GitHub API.

### The status file

`/tmp/az-community/status`, rewritten whole (temporary file, then rename) on every change:

```
state=done
op=install
id=beat-counter
done=0
total=0
stamp=812
serial=14
off=0
msg=Beat Counter 1.0.0 installed. Press Reload now to start it.
```

| Key | Meaning |
|---|---|
| `state` | `idle`, `refreshing`, `downloading`, `verifying`, `installing`, `done`, `error`, or `reloading` after `apply` |
| `op` | the request being served |
| `id` | the mod id for `install`, `remove`, `enable` and `disable`; else empty |
| `done`, `total` | bytes while a package downloads; otherwise progress counts (repositories, icons) |
| `stamp` | seconds since boot when written |
| `serial` | increases every time the catalog is rewritten |
| `off` | `1` when `MODS/azmods.cfg` says `community-mods = off` |
| `msg` | one line for the user, at most 111 bytes; always the last line |

A reader may ignore `serial` and `off`. No internet is `state=error` with a message that
starts "No internet" (refresh) or "Could not download <name>: no internet connection."
(install).

The Mod Menu treats `done` after `install` or `remove` as "what loads has changed" and offers
Reload; after `enable` or `disable` it first asks the core to make the change now
([Turning a mod on or off without a reload](#turning-a-mod-on-or-off-without-a-reload)). When
the core cannot, it shows the pending state ("Enabled after Reload.") until the next start.

### The request file

`/run/az-community.req`: one request line, written by the Mod Menu to
`/run/az-community.req.new` and renamed.

| Side | Rule |
|---|---|
| Mod Menu | does not send a request that does not fit in 511 bytes |
| Watcher | reads at most 512 bytes of the first line, keeps printable ASCII only, deletes the file, runs the fetcher |
| Watcher start | a request left over from before the watcher started is deleted unread |

A newer request replaces one not yet served.

## Install

`install <repo> <id>` works only from the cached repository file: run `refresh` first.

1. **Check the request.** Valid id; the repository is in the cache and still lists the id.
   - If `MODS/community-mods/<id>/` exists, it must have been installed from the same repository
     (its `.origin`); otherwise "<name> by <author> already uses this ID. Remove it first."
     (or "<name> from <repository> ..." when the manifest has no author).
   - A folder with no `.origin` was not made by an installer and is never replaced
     ("MODS/community-mods/<id> exists but was not installed here; remove it first").
   - If the id is not installed, the repository must be the one the catalog shows for it
     (`winners`).
2. **Download** to `/tmp/az-community/work/pkg.azmod`, capped at the repository file's `size`.
3. **Verify** the file's size and SHA-256 against the repository file before opening it.
4. **List** the zip with busybox `unzip -l` and refuse unless every member name is safe:
   - `[A-Za-z0-9._/-]` only (so no spaces), no leading `/`, no component starting with `.`
     (so no `..`), no empty component;
   - nothing under `data/` (any case): "the package has a data/ folder: a mod's data is made on
     the USB, never shipped";
   - at most 128 bytes and 3 `/`;
   - no name listed twice, no two names that differ only in case (FAT ignores case);
   - the listing's file count matches its total line;
   - `azmod.json` and `<id>.so` at the root; no `<id>.off` in the package.

   Any message on stderr from the listing refuses the package: busybox strips a leading
   `../` or `/` and reports it only there.
5. **Unpack** each listed file with `unzip -p <name>` into a path the fetcher builds from the
   validated name, capped at its listed size, and check the size written. Anything other
   than regular files and folders refuses the package. The archive never chooses a path.
6. **Check `azmod.json`** ([The manifest check](#the-manifest-check)).
7. **Check space.** The USB needs the unpacked size plus 1 MB. The deck's settings
   partition must keep its 12 MB reserve once the loader has copied the mod (the new copy's
   size counted, the stage's old copy of the id credited).
8. **Write** `MODS/community-mods/.<id>.new/`, verify the copy against the unpacked files, then
   write `.origin`. Rename an existing `<id>/` to `.<id>.old/`, rename `.<id>.new/` to
   `<id>/`, move the old copy's `data/`, `.data.owner` and `.data@*` into it, then delete
   `.<id>.old/` (kept when a move fails). A package with a `data/` entry was refused at step 4.
9. **Enable**: set `community-mods.<id> = on` in `MODS/azmods.cfg` (written whole and renamed), unless the
   previous version was turned off; an update keeps a turned-off mod turned off.
10. **Approve**: write the id with the SHA-256 of the new `<id>.so` to the deck's
    [approved list](#approved-mods), replacing the id's previous line. An install or update
    requested from the deck is the user's decision to trust that mod, made on the deck. If
    the list cannot be written, the mod is still installed and the message says so.

The USB is remounted read-write only when it was mounted read-only, and restored
afterwards. Nothing in the package is executed, sourced or made executable. ELF checks are
not done here (the deck has no `readelf`); the core does them before it loads the mod.

| Result | Message |
|---|---|
| installed, enabled | `<name> <version> installed. Press Reload now to start it.` |
| installed, was disabled | `<name> <version> installed. It stays disabled, as before.` |
| installed, approval not written | `<name> <version> installed. After Reload, open it under Installed and press Allow.` |

### Interrupted installs

Directories whose names start with a dot are never loaded and never copied by the loader.
At its next start the fetcher recovers each id whose `<id>/` is missing:

| Leftover | Action |
|---|---|
| `.<id>.new/` complete (its `.origin` has an `installed=` line, written last) | renamed to `<id>/`: the install is finished |
| otherwise `.<id>.old/` | renamed back to `<id>/`: the previous version returns |

Then it deletes the leftovers (`.<id>.new`, `.<id>.old`, `.<id>.del`, `.enabled.new`,
`.repos.new`), except a copy that a failed recovery still needs.

### `.origin`

Records where an installed mod came from:

```
repo=https://raw.githubusercontent.com/someone/az-mods/HEAD/azmods-repo.json
version=1.0.0
sha256=<64 hex>
size=123456
installed=<YYYY-MM-DD HH:MM>
```

`repo` is the normalized repository file URL; `sha256` and `size` are the package's. The catalog's
`installed` field, the one-repository-per-id rule, the core's owner stamps and the AZ-MODS
desktop app (which installs into the same layout and writes the same record) all key on it.
The app follows the same install rules: it refuses what the core would refuse (the manifest
rules below, the 16383-byte manifest), and an update keeps a turned-off mod turned off (its
`community-mods.<id> = off` stays).

### The manifest check

The fetcher checks the package's `azmod.json` against the repository file entry it came from, in this
order:

| Rule | Refusal message (after "Not installed: ") |
|---|---|
| the file is at most 16383 bytes | `azmod.json is larger than 16383 bytes (the most the deck reads)` |
| the file is valid JSON | `azmod.json is not valid JSON (...)` |
| `azmod` is the number 1 | `azmod.json: "azmod" must be 1` |
| `id` equals the requested id | `azmod.json: id is not <id>` |
| `name` non-empty, at most 64 bytes | `azmod.json: name missing or longer than 64` |
| `version` `MAJOR.MINOR.PATCH` | `azmod.json: version must be MAJOR.MINOR.PATCH (for example 1.0.0)` |
| `version` equal to the repository file's version | `azmod.json: version X but the repository file says Y` |
| `author` non-empty | `azmod.json: author missing` |
| `source` an `https://` URL (at most 512 characters, URL-safe) | `azmod.json: source must be a public https:// URL` |
| `api` is the number 1 | `azmod.json: api N is not supported (this deck: 1)` |
| `firmware` an array of `ep147-<16 hex>` tags or `"any"` | `azmod.json: firmware must list ep147-<16 hex> tags or ["any"]` |
| `"any"` alone in `firmware` | `azmod.json: firmware "any" must be alone` |
| `hooks` absent or boolean | `azmod.json: hooks must be true or false` |
| not `"hooks": true` with `any` | `azmod.json: a firmware "any" mod cannot claim hooks` |
| `min_core` absent or `MAJOR.MINOR.PATCH` | `azmod.json: min_core must be MAJOR.MINOR.PATCH (for example 1.2.3)` |

## Remove, enable, disable, repositories

| Request | Does |
|---|---|
| `remove` | renames `<id>/` to `.<id>.old/`, removes its `community-mods.<id>` key, renames the copy to `.<id>.del/` and deletes it, then drops the id from the approved list. Its `data/` goes with it. Message: "<name> removed from the USB." |
| `enable`, `disable` | set `community-mods.<id>` to `on` or `off`. Message: "<name> turns on after Reload." / "<name> turns off after Reload." (not shown when the core makes the change now) |
| `addrepo` | refuses `http://` and a 33rd repository, says so when the repository is already present, appends the typed text to `repos.txt`, and refreshes |
| `delrepo` | removes every line of `repos.txt` that normalizes to the same address ("<repository> is not in your repositories" when none does). Mods installed from that repository stay installed |

A mod loads only when `azmods.cfg` says `community-mods.<id> = on`. After a change the
loader hands the USB's `azmods.cfg` to azmod, so the deck's working copy follows.

After the refresh, `addrepo` reports on that repository alone:

| Outcome | Message |
|---|---|
| read | `Added owner/name: N mods.` |
| no internet | `Added owner/name. No internet to read it yet: open Community Mods again on WiFi.` |
| HTTP 404 | `Not found at owner/name. Check the spelling.`, and the line is removed from `repos.txt` again |
| any other failure | `Added owner/name, but it could not be read: <reason>` |

## Apply

The Mod Menu's Reload button sends `apply`. While a track is loaded, or when the core cannot
tell, it first asks: "Reload restarts the deck software. Music stops, and loaded tracks are
unloaded, for about 10 seconds." Restart rules: [USB-STICK.md, "The restart policy"](USB-STICK.md#the-restart-policy).

1. The fetcher checks that the USB carries a core (`MODS/azmod.so` or `MODS/azmod-core.so`),
   since the loader stages community mods only for a USB with one, and sets
   `state=reloading`. Without one: "this USB has no AZ-MODS (MODS/azmod.so): nothing to apply".
2. The watcher sees `reloading` after `apply`, sets `AZ_FORCE_RELOAD=1` and re-runs the
   loader. The flag makes the loader restart EP147 even when the stage is unchanged (a change
   that lives only in the deck's settings, such as Resume or the master switch), and without
   waiting for the decks to empty.
3. The loader mirrors the USB into the stage, deletes `.starting` (a deliberate restart is
   not a failed start) and restarts EP147.
4. After the restart the watcher runs `settle`: `done` when the stage now holds the community
   folder (or the USB asks for none), otherwise `error` with "not applied: the deck could
   not copy MODS/community-mods (see /tmp/az-usbload.log)".

Before it sends `apply`, the Mod Menu writes every pending mod setting and tells the core that
this restart was asked for ([The reload exemption](#the-reload-exemption)).

### The mirror

At every boot and every re-run that stages a set with a core, the loader's `community_mirror`:

| USB | Stage |
|---|---|
| `azmods.cfg` says `community-mods = off` | `community/` deleted; `community-mods.off` written into the stage |
| no `MODS/community-mods/` | `community/` deleted |
| otherwise | copied when a checksum of the files it would copy differs from the stage's; then the stage's `enabled` and `<id>/<id>.off` are written from the `community-mods.<id>` keys |

What is copied: every file except macOS `._*` files, top-level dot entries (the fetcher's
transient folders) and each mod's `assets/`, which stays on the USB. One file list drives
the checksum, the space check and the copy. The copy goes to `community.new` and replaces
`community` only when its checksum matches. A copy that would leave less than 12 MB
(`AZ_COMMUNITY_P7_RESERVE_KB`) free on the settings partition is not made; the previous stage
stays and the loader logs why.

The core loads only from the stage, never from the USB.

## The trust screen and the switches

**Trust.** The first Install on a deck opens a screen:

> Before you install <name>, your first community mod:
>
> Community mods are made by other developers, not by AZ-MODS. AZ-MODS does not check them. They
> run inside the deck software with full access to your deck, and a faulty one can stop the
> music.
>
> Only install mods whose source you trust. You can turn every community mod off at any time
> under Community Mods.

"I understand, install" writes `/home/root/settings/azmod-community/.trusted` and sends the
install. The screen is not shown again on that deck.

**The master switch.** Community mods are all off when either is set:

| Switch | Set by | Takes effect |
|---|---|---|
| `community-mods = off` in `MODS/azmods.cfg` (USB) | the desktop app, or by hand | at the next loader run; the loader also removes the stage's copy |
| `/home/root/settings/azmod-community/off` (deck) | the Mod Menu switch | at the next start |

The official mods are not affected by either.

**Per mod.** `community-mods.<id> = off` in `MODS/azmods.cfg` turns one mod off; the core lists it as "Turned
off" and never reads its code.

## Approved mods

The deck's own record of the mod files the user has trusted:
`/home/root/settings/azmod-community/approved`, one line per mod, the id and the SHA-256 of
its `<id>.so` in lowercase hex, separated by one space.

```
beat-counter 3f5a...e1 (64 hex digits)
booth-clock 9c02...7b
```

**Reader.** The core, once per start, before it loads anything. A mod that is turned on and
passes every other start check but is not on the list (no line for its id, or a line with
another hash) is not loaded and none of its code runs. The Mod Menu lists it as "New mod
found: allow?"; its page says where the copy came from (the repository in its `.origin`, or
"copied there by hand") and offers Allow and Remove from USB. Allow writes the hash the core
computed at this start; the mod loads after the reload. If the stage holds different bytes
after the reload, the mod asks again.

**Writers.**

| Writer | When |
|---|---|
| the fetcher | an install or update requested from the deck (the line for the new `.so`, replacing the id's old one); a remove (drops the id) |
| the core | Allow on a "New mod found" page; [grandfathering](#grandfathering) |

The desktop app writes the USB on a computer and cannot reach the deck's list, so a mod it
installs or updates asks once on the deck. The app says so after the install ("Approve it on
the deck once (Mod Menu > Community Mods)").

**Rules.**

| Rule | Detail |
|---|---|
| One line per id, the newest last | approving a new hash for an id replaces its line, so a different build under an approved id asks again |
| Malformed lines | anything but exactly `<id> <64 lowercase hex>` is ignored and dropped at the next write |
| Size | at most 64 lines kept; the oldest are dropped first |
| Locking | both writers take an exclusive `flock` on `azmod-community/.approved.lock`, re-read the list and replace it through a temporary file and a rename (the core also `fsync`s the file and its folder). The fetcher tries five times, a second apart, the core for up to 1 s; neither holds the lock for more than a few milliseconds |
| What is hashed | the same bytes the core reads for the ELF checks, read once, before `dlopen`. Nothing is hashed on a hot path |

### Grandfathering

The first start of a core with the approved list (no `azmod-community/.approved-init` yet)
approves, without asking, each mod that meets all of these:

- the deck already has community state (`azmod-community/` existed when the start began), so
  it was running community mods before the approved list existed;
- the mod is turned on (`community-mods.<id> = on`);
- its `.origin` is a complete install record: a `repo=https://...` line, a `sha256=` line of
  64 hex digits and an `installed=` line, as the fetcher and the desktop app write it. A
  folder copied by hand has none.

Each grandfathered mod is logged (the core's log and the mod's own log). `.approved-init` is
written once every approval reached flash; if one could not be written, the next start tries
again. Grandfathering runs on that first start even when community mods are switched off or
quarantined. A mod that does not qualify asks once, like any new mod. An install record is
not proof of origin (a USB can carry a forged one), which is why this runs exactly once:
every mod that appears after that start asks. A deck with no community state and nothing
turned on writes nothing; its first start with mods turned on grandfathers none.

## The core

`community_init` runs once per EP147 start, on the overlay thread (which exists only in
EP147).

### Which mods are considered

1. **The firmware tag.** `ep147-` and the 64-bit FNV-1a, as 16 hex digits, of the file bytes
   of EP147's executable `PT_LOAD` segments, read from `/proc/self/exe` through its own
   program headers (never from `/proc/self/maps`, which a hook installer's `mprotect`
   changes). A tag that cannot be derived is empty; then only `any` mods can load.
2. **The list.** The stage's `enabled`, one id per line, in file order, skipping invalid and
   repeated ids; then every staged mod that has an `<id>.off` (so the Mod Menu can offer to
   turn it on). At most 16 mods in all (`CM_MAX`).
3. **Grandfathering**, once ([above](#grandfathering)), then the approved list is read.
4. **All off.** With community mods switched off, the enabled ones are listed as "Community
   mods are turned off" and nothing loads.
5. **Quarantine** ([below](#quarantine-and-crash-blame)).

### The start checks

For each enabled mod, in order:

| Step | Check | Refusal reason (shown in the Mod Menu) |
|---|---|---|
| 1 | `<id>.off` absent | Turned off |
| 2 | `azmod.json` readable (the core reads at most 16383 bytes), one flat object, no nested objects, no repeated keys | Missing azmod.json, or the scanner's reason |
| 3 | its `id` equals the folder name | azmod.json is for a different mod id |
| 4 | no `<id>.crashed` in the settings folder (unless the owner stamp names another owner) | Suspended: it crashed the deck / Suspended: the deck stopped while this mod was starting |
| 5 | `api` equals `AZ_MOD_ABI` (1) | Made for mod API N; this AZ-MODS has 1 |
| 6 | `min_core` absent, or `MAJOR.MINOR.PATCH` not above `AZ_CORE_VERSION` | Needs AZ-MODS X or newer / azmod.json min_core is not MAJOR.MINOR.PATCH |
| 7 | `source` present | No source link in azmod.json |
| 8 | `firmware` lists the running tag, or `any` | azmod.json has no readable firmware list / Not made for this firmware / This deck's firmware could not be identified |
| 9 | not `any` together with `"hooks": true` | Says "any" firmware but also patches code |
| 10 | ELF: at most 16 MB, 64-bit little-endian, `EM_AARCH64`, `ET_DYN`, a section table inside the file | Missing <id>.so / The mod file is empty or too large / Not a 64-bit ELF library / Built for the wrong CPU (not aarch64) / Not a shared library / Damaged ELF section table |
| 11 | ELF: exports `az_mod_v1_entry`, and nothing else besides linker boilerplate (`_init`, `_fini`, `_edata`, `_end`, `__bss_start` and its variants, `__end__`) | Does not export az_mod_v1_entry / Exports X; a mod exports only az_mod_v1_entry |
| 12 | ELF: exports none of the functions the core interposes: `read`, `write`, `open`, `open64`, `openat`, `close`, `ioctl`, `recvmsg`, `recv`, `recvfrom`, `sendmsg`, `select`, `poll`, `ppoll`, `mmap`, `mprotect`, `dlopen`, `dlsym`, `fopen`, `sqlite3_*` | Exports X(), which only AZ-MODS may define |
| 13 | ELF: `DT_NEEDED` only `libc.so.6`, `libm.so.6`, `libdl.so.2`, `libpthread.so.0`, `librt.so.1`, `libgcc_s.so.1`, `libstdc++.so.6`, `ld-linux-aarch64.so.1` | Links X, which a mod may not use |
| 14 | the id with the SHA-256 of the `<id>.so` bytes read for steps 10 to 13 is on the approved list | New mod found: allow? |
| 15 | `dlopen(RTLD_NOW \| RTLD_LOCAL)` succeeds | Could not load: <dlerror> |
| 16 | the descriptor `az_mod_v1_entry()` returns ([below](#the-descriptor)) | the matching reason |
| 17 | `init(&core)` returns 0 | Its init() returned N (followed by " (after patching code; reload to clear)" when it had claimed hooks) |

Steps 1 to 14 read files only; nothing of the mod runs before step 15. The approval check
comes last, so Allow is offered only for a mod that would otherwise load. Between steps 14
and 15 the core settles the mod's owner ([One id, one owner](#one-id-one-owner)) and writes
`.initing`. `RTLD_LOCAL` keeps the mod's symbols out of the process's scope, so a dlopened mod
cannot interpose anything. A library, once opened, is never closed: a constructor or `init`
may have started a thread or handed EP147 a pointer into its code. A mod whose `init` fails
stays mapped and is never called again.

### The descriptor

| Check | Refusal reason |
|---|---|
| `az_mod_v1_entry()` returns non-NULL | az_mod_v1_entry returned nothing |
| `abi` equals `AZ_MOD_ABI`, and `size` covers the structure through `on_frame` | Built for a different mod API |
| `id` equals the folder name | The library's id does not match azmod.json |
| `fw_tags` (first 32 entries) lists the running tag, or `any` for a mod whose manifest says `any` | The library is not built for this firmware |
| `init` present | The library has no init() |

### Hook claims

`claim_hook` succeeds only when all of these hold; every refusal is written to the mod's log:

| Condition | Logged refusal |
|---|---|
| called from inside the mod's own code (the return address lies in the mod's mapping), so a mod cannot claim in another mod's name | called with this mod's id from outside its code (or as the last call of a callback: build with -fno-optimize-sibling-calls) |
| called while the core runs that mod's `init` | call it from init() |
| its manifest says `"hooks": true` and its firmware is not `any` | azmod.json does not declare "hooks": true / a mod for "any" firmware may not patch code |
| the address lies inside EP147's executable range | not inside EP147's code |
| no existing hook, the core's or another mod's, starts within 16 bytes of it (a hook rewrites 16 bytes) | overlaps the hook at X patched by Y |
| a jump island is within reach (a 16-byte patch is not written while EP147 runs) | no jump island within reach of it |
| the core's hook installer accepts it | the hook installer declined it |

Why a hooking mod is tied to one firmware build: the SDK's
[Hooks and firmware tags](https://github.com/Kyle-Hosman/az-mods-sdk/blob/main/docs/05-hooks-and-firmware.md).

### Services and the caller check

Every service that takes a mod id (settings, `data_dir`, `assets_dir`, screens, rows, banners
and dialogs, threads, frame and touch filters, surfaces, deck events, the library, audio taps,
launching programs, handoff, reload) applies the same own-code check as `claim_hook`. A mod
that was never loaded (refused, turned off, waiting for approval) has no code range, so
nothing may act as it. A refusal is logged once per service and mod. The same services are
refused when called from `on_frame`, which runs on the real-time control bus thread; those
refusals are counted and logged by the overlay tick.

### Settings, data and assets

| Item | Detail |
|---|---|
| Settings | in the settings file (`azmod.conf` on the deck, `MODS/azmods.cfg` on the USB): `<id>:cfg.<key> = <integer>` and `<id>:str.<key> = "<text>"` lines, and `<id>:owner.repo = <owner>`. A key byte outside `a-z 0-9 . _ -` is written `:` and two hex digits; a row key (`a-z 0-9 -`) has no `.`, so these never collide with `<id>:<row>`. A `<id>.cfg` left by a test build is moved in at the mod's next load, then deleted. Keys up to 31 bytes without `=`, `$` or a newline; text values up to 127 bytes, control bytes dropped; at most 32 integers and 16 texts |
| Settings writes | from the overlay tick, about 250 ms after a change, at most once every 5 s for a mod that changes them constantly, and immediately before a reload or when the user leaves the mod's page; the settings file is written whole (temporary file, flush, rename) |
| `data_dir` | `MODS/community-mods/<id>/data/` on the USB that carries `MODS/community-mods/<id>/azmod.json` from the same owner as the copy that loaded (USBs are looked up under `/media/usb`). Created by the core, not copied to the deck, kept by updates and reinstalls, deleted when the mod is removed. A package with a `data/` entry is refused |
| `assets_dir` | `MODS/community-mods/<id>/assets/` on that USB, read only. Replaced by an update, never copied to the deck |
| Re-check | both are re-checked every 2 s, and at once when a cached path no longer exists. With the USB gone they return NULL. A path once handed out is never blanked or shortened, so a mod holding an old pointer cannot build a path on the deck's own file system |

### One id, one owner

Settings, the data folder and a crash suspension are found by the id, and an id is not an
author: two authors can choose the same one, and a user can remove one author's mod and
install another's under its id. That state therefore belongs to the owner of the installed
copy: the `repo=` line of its `.origin` (lowercased for `raw.githubusercontent.com`), or
`local` for a copy without one.

| | On the deck | On the USB |
|---|---|---|
| Stamp | `azmod-community/<id>.owner` (crash suspension); `<id>:owner.repo` in the settings file (settings) | `community-mods/<id>/.data.owner` |
| Another owner's state, set aside | `azmod-community/.<id>@<hash>.cfg` | `community-mods/<id>/.data@<hash>/` |
| Compared | after the approval check, before `dlopen` (a mod that is not approved changes nothing on the deck) | whenever `data_dir` is resolved |

`<hash>` is the FNV-1a 64 hash of that owner, as 16 hex digits; a dot name is never an id and
never handed out. When the stamp and the owner differ, the old owner's state is set aside,
`<id>.crashed` is deleted, the new owner's own set-aside state comes back if there is one,
and the stamp is rewritten. State without a stamp is adopted. A suspension left by another
owner's copy is ignored at the start checks. If the settings cannot be set aside they are
deleted; if the folder cannot be set aside, `data_dir` returns NULL rather than hand it
over. This keeps mods from inheriting each other's state by accident; it is not a sandbox.

### Logs

| | |
|---|---|
| File | `/tmp/azmod-community/<id>.log`, one line per call (a line is cut at about 400 bytes) |
| Cap | 256 KB per mod, counted across EP147 restarts: at the first line of a start, a log larger than 128 KB moves to `<id>.log.1`; once the file reaches 256 KB, further lines of that start are not written |
| When written | only while `/home/root/settings/azmod-logs.on` exists (checked every second), except the load result, approvals and refusals (hook claims, services called from the wrong place), which are always written |
| From `on_frame` | queued (8 slots per mod) and written by the overlay tick; lines that do not fit are counted and reported as dropped |

### The load result

After loading, the core writes `/tmp/azmod-community/loaded` (temporary file, then rename):

```
fw=ep147-0123456789abcdef
suspended=0
off=0
mod=beat-counter|1|1.0.0|
mod=old-thing|3|0.9.0|Not made for this firmware
```

| Key | Meaning |
|---|---|
| `fw` | the running firmware tag |
| `suspended` | `1` when the start quarantine held every mod back |
| `off` | `1` when community mods are switched off |
| `mod` | `id\|state\|version\|reason` per listed mod |

| State | Meaning |
|---|---|
| 1 | loaded |
| 2 | turned off |
| 3 | refused |
| 4 | failed (`init` returned non-zero) |
| 5 | suspended |
| 6 | not approved on this deck ("New mod found") |

The loader copies the `fw` line onto the USB as `fwtag=` in `MODS/.deck-info`, so a desktop
tool can show which mods work on that deck.

## Quarantine and crash blame

All state lives in `/home/root/settings/azmod-community/`, never in the stage (the loader
rewrites the stage from the USB).

| File | Meaning |
|---|---|
| `.starting` | starts that loaded community mods and have not yet run 90 s |
| `.crashes` | the times (Unix seconds) of crashes no mod could be blamed for |
| `.blame` | the id of the mod whose code crashed EP147 |
| `.initing` | the id of the mod whose `dlopen` or `init` is running |
| `<id>.crashed` | this mod is suspended (empty: it crashed; `froze`: the deck stopped while it started) |
| `off` | the Mod Menu's master switch |
| `.trusted` | the trust screen was confirmed |
| `approved` | the [approved list](#approved-mods) |
| `.approved.lock` | the lock both writers of `approved` take |
| `.approved-init` | the one-time grandfathering has run |
| `<id>.owner` | the owner its crash suspension belongs to |
| `.<id>@<hash>.cfg` | another owner's settings, set aside |

At each start with enabled mods and community mods on, before loading, in this order:

1. **Frozen in `init`.** `.initing` left behind names a mod: it gets `<id>.crashed` with
   `froze`, and `.starting` is cleared (the culprit is known).
2. **Blamed.** `.blame` names a mod: it gets `<id>.crashed`, and `.starting` is cleared.
3. **The start quarantine.** `.starting` at 2 or more: no community mod loads this start
   ("Suspended: the deck restarted twice right after starting with community mods").
4. **Repeated unexplained crashes.** 3 or more entries in `.crashes` within the last 30
   minutes: no community mod loads ("Suspended: the deck crashed N times in 30 minutes with
   community mods on"). Older entries are dropped.
5. Otherwise `.starting` is incremented and `fsync`ed before any mod's code runs, then the
   mods load. After 90 s of running, `.starting` is deleted.

A crashing mod therefore costs at most two short restarts, and the start quarantine trips
before the loader's [crash-loop guard](USB-STICK.md#the-crash-loop-guard), which would turn
the official mods off as well.

### Turning a mod on or off without a reload

A mod whose descriptor sets `stop` (`az_mod_v1.stop`, checked with `AZ_MOD_HAS_STOP`) is
turned off and on by `enable` and `disable` while the deck runs. The core
(`azcm_live_set`, UI thread) first writes the stage as the loader would from `azmods.cfg`
(`<id>/<id>.off` and the sorted `enabled` list), so a restart without a re-mirror agrees.

Off, only for a running mod that has `stop` and used nothing that lasts until the next start
(no `"hooks": true`, no claimed hook or vtable slot, no surface, X window, audio tap or
handoff). The core sets the mod's state first; every call from another thread (on_frame, frame
and touch filters) goes through `cm_enter`, which counts it, so the core then waits up to
500 ms for the calls already inside the mod to return. Then it calls `stop()` (with the
`.initing` marker written, so a stop that freezes or crashes EP147 suspends the mod), hides its
rows, retires its filters and deck listeners, drops its page, stops the programs it launched and
waits up to 1 s for the threads it started through `thread_start` to end. A call that does not
return, or a thread that does not end, leaves the mod off with `stop()` skipped or its threads
logged, and it can be turned on again only after a reload. The library is never unloaded.

On, only for a mod shown as Disabled. One stopped live earlier this start runs `init()` again,
if its staged `.so` still has the checked and approved sha256. One that was off at the start
goes through the whole start check now; it waits for a reload instead when its manifest says
`"hooks": true` or its descriptor has no `stop`. Registrations made again reuse the mod's old
slots: the same row key revives its row (handle and value kept), a frame filter on the same
buses and a touch filter reuse theirs.

A mod installed or updated this start is never turned on live: the stage holds what loaded at
the start until the Reload mirrors the USB again.

### The reload exemption

A reload the DJ asks for from the Mod Menu deletes `.starting` first (the loader also deletes
it before its own restart): a start that was usable is not a failed one, and two quick
installs each followed by Reload must not read as two failed starts. While the quarantine
holds, a reload changes nothing; only Resume lifts it.

### Crash blame

While any community mod is loaded, a handler for SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT
looks for the mod that owns the crash, in this order:

1. the faulting PC;
2. the return address (x30);
3. up to 16 frames up the frame-pointer chain, read with `process_vm_readv` so a corrupt
   stack cannot fault again.

A frame inside a mod's mapping names that mod: its id is written to `.blame` with
async-signal-safe calls only. No mod in any of those frames: the time is appended to
`.crashes`. Blame is recorded once per process. The handler then restores the previous
disposition and returns (SIGABRT is raised again), so EP147 dies as it would have.

When the core's crash reporter is armed it owns these signals and calls the same blame
function, so there is one handler per signal and blame still lands. The handlers run on an
alternate signal stack where the thread has one: EP147's main thread, every thread the core
starts and every thread a mod starts with `thread_start` have one, so a stack overflow there
is still recorded and blamed. A thread a mod creates any other way, and EP147's other
threads, have none, and a stack overflow on them ends EP147 with no record.

### Freezes

A freeze sends no signal. `.initing` is written and `fsync`ed before a mod's `dlopen` and
removed when its `init` returns, so a deck that stops inside a mod's start suspends exactly
that mod at the next start. A freeze after `init` is not attributed.

### Getting back

| Control | Does |
|---|---|
| Resume (quarantine banner) | deletes `.starting` and `.crashes`; mods load after the next reload |
| Keep off (quarantine banner) | switches community mods off (writes the deck's `off` file) |
| Try again (a suspended mod's page) | deletes `<id>.crashed`; the mod loads after the next reload |
| Enable / Disable | sends `enable` / `disable` |
| the master switch | writes or deletes the deck's `off` file |

## What the Mod Menu shows

On an installed mod (`cm_state_text` in `screen_community.c`):

| Core state | Shown |
|---|---|
| loaded | Running |
| turned off | Disabled, or Community mods are disabled |
| refused | Not loaded: `<reason>` |
| failed | Failed: Its init() returned N |
| suspended, blamed | Turned off: it crashed the deck |
| suspended, froze while starting | Turned off: the deck stopped while it was starting |
| suspended by the quarantine | Held back (see above), under the banner "Community mods were held back this time." with Resume and Keep off; after Keep off, "Turned off (all community mods kept off)" |
| not approved on this deck | New mod found: allow? (after Allow: "Allowed. Loads after Reload.") |

Pending changes are shown until the reload that applies them: "Enabled after Reload.",
"Disabled after Reload.", "Loads again after Reload.", "Removed. Unloads after Reload."
("Removed." for a mod that was not running). When a mod was suspended on its own, the home
page says "<name> stopped the deck, so it was disabled. Your other mods were not affected."
("N mods stopped the deck, so they were disabled. ..." for several).

**Browse badges**, at most one per tile, the first that applies:

| Badge | When |
|---|---|
| Removed | installed from this repository and removed this start |
| Update available | a newer version that works on this deck and this core |
| Needs newer AZ-MODS | a newer version whose `min_core` is above this core |
| Installed, disabled | installed, and disabled or not running |
| Installed | installed from this repository |
| Its ID is in use | another mod holds the id (`held`) |
| Not for this deck | the firmware field does not match |
| Needs newer AZ-MODS | `min_core` is above this core |

"Update available" compares `MAJOR.MINOR.PATCH` versions only (each part at most 9999); the
fetcher and the app refuse any other version, so every installable mod can be offered an
update.

**Other marks and lines.**

| Where | Shown |
|---|---|
| a tile from a repository not in the Mod Index | an orange mark with the Repositories icon in its top-right corner; its page says "Not in the Mod Index" beside the repository |
| a tile for a mod marked beta | a blue mark with BETA in its top-left corner; its page says "BETA: This mod may not be stable yet." |
| a mod's page with screenshots | a row of them under the summary; a tap opens one large, the sides step through them, the middle or BACK closes it |
| the repository list | each repository marked Mod Index, Added by you, or the reason it could not be read |
| a mod's page, other repositories offer its id | "<repository> also offers a mod with this ID. Only one can be installed." |
| a mod's page, another mod holds its id | "A different mod with this ID is installed (from <repository>). Remove it first." or "A different mod with this ID is on the USB (copied there by hand). Remove it first.", and no Install |
| every tile and installed mod | its author, which tells apart two mods with the same name |

**The screen.** The left rail holds Installed, Browse and Repositories at the top and Updates at
the bottom ("Updates N" while there are updates). Installed is split as the Mod Menu is: the list on
the left (each row with its checkbox, which turns the mod on or off at once, and its `>`), the
selected mod's page on the right (its info, buttons and its own settings screen); the knob moves the
selection, a press goes into the page and BACK comes out. Updates lists every installed mod with an
update this deck can take, with Update all (one install after another, stopped by an error). The
header shows the number of mods listed, and Reload, one square at its top right whenever a reload
is due ("Changes take effect after a reload"); once pressed it shows "Please wait..." until the
deck restarts. The Mod Menu's own Community Mods page offers the same Reload when its switch is
toggled, and never reloads by itself.

## Failure modes

| What happens | What the user sees | Where the detail is |
|---|---|---|
| No WiFi during refresh | "No internet: showing mods from the last refresh. ..." and the cached catalog | `/tmp/az-community/log` |
| Curated list unreachable (not 404) | "... The Mod Index did not load. Open Community Mods again to retry." The last good list is used | fetcher log |
| A repository 404s or is unreachable | its row says "not found (check the address)" or "could not be reached (check the address, or try again later)" | `<h>.state` |
| A repository file is not JSON or not `azmods_repo` 1 | its row says why; its mods are not listed | `<h>.state` |
| Some repository file entries break the rules | "N entries skipped (invalid)" on the repository; the rest listed | fetcher log, one line per skipped entry |
| Package larger than the repository file says | "Not installed: the package is larger than the repository file says" | |
| Package hash or size differs from the repository file | "Not installed: sha256 mismatch (...)" or "Not installed: the package size does not match the repository file" | |
| Unsafe or malformed zip | "Not installed: <reason>" (unsafe name, nests too deep, two names differing only in case, streamed zip, ...) | |
| Manifest disagrees with the repository file or the rules | "Not installed: azmod.json: ..." | |
| USB or deck too full | "Not enough free space on the USB stick for <name>." / "Not enough free space on the deck for <name>." | |
| Same id installed from another repository | "<name> by <author> already uses this ID. Remove it first." | |
| Same id offered by a repository the catalog does not show | "Not installed: <repository> offers a mod with the same ID, and that one is listed here." | `winners` |
| USB read-only or pulled during install | "the USB is read-only" / "could not write the USB"; the previous version stays | |
| Power lost during install | nothing visible; the next fetcher run finishes or undoes it | fetcher log |
| Apply with no AZ-MODS on the USB | "this USB has no AZ-MODS (MODS/azmod.so): nothing to apply" | |
| The deck cannot copy the community folder | "not applied: the deck could not copy MODS/community-mods (see /tmp/az-usbload.log)" | `/tmp/az-usbload.log` |
| A mod is refused at start | "Not loaded: <reason>" on its page | `/tmp/azmod-community/<id>.log`, `loaded` |
| A mod's `init` fails | "Failed: Its init() returned N" | the mod's log |
| A mod crashes the deck | the deck restarts; the mod shows "Turned off: it crashed the deck"; the others load | the mod's log |
| A mod freezes the deck while starting | after the restart, "Turned off: the deck stopped while it was starting" | |
| Two short starts in a row with mods loaded, cause unknown | the quarantine banner; no community mod loads until Resume | `loaded` (`suspended=1`) |
| Three unexplained crashes in 30 minutes | the quarantine banner with that reason | `.crashes` |
| The loader's [crash-loop guard](USB-STICK.md#the-crash-loop-guard) trips anyway | the deck comes up stock, official mods included | `/tmp/az-usbload.log` |
| A mod's USB is ejected | `data_dir` and `assets_dir` return NULL; the mod carries on without its files | |
| Firmware updated under an installed mod | "Not made for this firmware" until the author ships a build for the new tag | |
