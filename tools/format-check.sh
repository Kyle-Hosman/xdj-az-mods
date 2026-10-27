#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# format-check.sh: the mods sources are formatted as .clang-format says, by clang-format 21.1.8.
#
#   tools/format-check.sh              check every in-scope file; exit 1 naming each unformatted one
#   tools/format-check.sh --fix        reformat the in-scope files in place
#   tools/format-check.sh FILE...      check only these files (any path; the exclusions do not apply)
#   tools/format-check.sh --fix FILE...  reformat only these files
# In the published source layout (src/, no guest/) only the FILE modes run here; tests/run.sh checks
# the whole tree.
#
# The version is pinned because clang-format versions format differently, and the check must give
# the same answer on every machine. Where the binary comes from, first match wins:
#   1. $CLANG_FORMAT, which must be 21.1.8 (a wrong one is an error, never a fallback)
#   2. clang-format on PATH, if it is 21.1.8 (pip install clang-format==21.1.8 gives exactly that,
#      on macOS, Linux and Windows)
#   3. the azmod-test:18.04 docker image (docker/azmod-test.Dockerfile installs 21.1.8 from a
#      sha256-pinned wheel); the script re-runs itself inside it, with the repository mounted
# None of them: exit 2 with what to do. A missing or wrong clang-format never passes.
#
# Scope: the C sources under guest/azmod, guest/azstem, guest/common, guest/azhandoff,
# tools/unit-tests and tools/fuzz that git tracks (outside a git checkout: every .c and .h
# there), minus EXCLUDE below (generated, vendored, templated).
# Exit: 0 all formatted, 1 unformatted files (each named, with the first lines of its diff),
# 2 the check could not run.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WANT=21.1.8
IMG=azmod-test:18.04
DIRS=(guest/azmod guest/azstem guest/common guest/azhandoff tools/unit-tests tools/fuzz)

# Not formatted, each for its reason. Every pattern must match a file in scope: a stale entry is
# an error, so the list cannot silently go out of date.
EXCLUDE=(
    # GENERATED tables (a generator writes them; run.sh's addrgen check compares ep147addr.h byte for byte)
    '^guest/azmod/ep147addr\.h$'        # per-firmware address table
    '^guest/azstem/ep147addr\.h$'       # per-firmware address table
    '^guest/azmod/ep147png\.h$'         # tools/ep147-pngtable.py
    # GENERATED art: image bytes as C arrays, each header written by its own art tool
    '^guest/azmod/eyeicon\.h$'          # the Hide Title eye icon
    '^guest/azmod/fxart\.h$'            # Beat FX panel art
    '^guest/azmod/fxbars\.h$'           # Drum Roll Beat FX bar labels
    '^guest/azmod/fxhdr\.h$'            # the Drum Roll X-PAD header bar
    '^guest/azmod/holdtile\.h$'         # the Drum Roll HOLD pad tile
    '^guest/azmod/sfxtile\.h$'          # the Stem FX pad tiles
    '^guest/azmod/menu_icons\.h$'       # Mod Menu icons, from the SVGs in azmod/icons
    '^guest/azmod/menulabels\.h$'       # MENU panel labels
    '^guest/azmod/collabels\.h$'        # the Browse COLUMNS button's labels
    '^guest/azmod/stems4art\.h$'        # 4-deck source-cell stem strips
    '^guest/azmod/stemsmark\.h$'        # the browser's stems mark
    '^guest/azmod/vsbar\.h$'            # the vinyl slider's canvas, an RGBA PNG
    '^guest/azmod/srcref\.h$'           # a 1-bit screen mask
    '^guest/azmod/wavref\.h$'           # a 1-bit screen mask
    '^guest/azmod/caption_strips\.h$'   # PNG byte arrays; exported as a template
    # the OneLibrary key headers: the public export replaces them with templates
    '^guest/azmod/onelibkey\.h$'
    '^guest/azmod/onelibkey_sm\.h$'
    # vendored third-party code, kept as upstream wrote it
    '^guest/azstem/stb_vorbis\.c$'
    # 1.2.2's code, frozen verbatim as a test oracle
    '^tools/unit-tests/legacy-1\.2\.2/'
)

die() { echo "FORMAT CHECK COULD NOT RUN: $*" >&2; exit 2; }
version_ok() { "$1" --version 2>/dev/null | grep -Eq "clang-format version ${WANT//./\\.}( |$)"; }

fix=0
if [ "${1:-}" = "--fix" ]; then fix=1; shift; fi

# ---- the files (on the host: a git worktree's metadata is outside the tree docker sees)
cd "$ROOT"
[ -f .clang-format ] || die "no .clang-format at $ROOT"
files=()
if [ $# -gt 0 ]; then
    files=("$@")
else
    # the whole-tree scope is the development layout; the published layout (src/, tests/unit)
    # is checked by tests/run.sh, which assembles that layout in tests/.work/tree
    [ -d guest ] || die "no guest/ here (the published source layout): run tests/run.sh, which assembles
    the tree and runs this check on it, or name the files: $0 [--fix] src/azmod/<file>.c ..."
    if command -v git >/dev/null 2>&1 && git rev-parse --git-dir >/dev/null 2>&1; then
        all=$(git ls-files -- "${DIRS[@]}" | grep -E '\.[ch]$' || true)
    else # a tree without git metadata (a source archive): every C file under those folders
        all=$(for d in "${DIRS[@]}"; do [ ! -d "$d" ] || find "$d" -type f \( -name '*.c' -o -name '*.h' \); done | LC_ALL=C sort)
    fi
    [ -n "$all" ] || die "no tracked C files under ${DIRS[*]}"
    for re in "${EXCLUDE[@]}"; do
        grep -Eq "$re" <<<"$all" || die "exclusion '$re' matches no tracked file (remove it from tools/format-check.sh)"
    done
    while IFS= read -r f; do
        skip=0
        for re in "${EXCLUDE[@]}"; do [[ $f =~ $re ]] && { skip=1; break; }; done
        [ $skip = 1 ] || files+=("$f")
    done <<<"$all"
fi
[ ${#files[@]} -gt 0 ] || die "nothing to check"

# ---- the binary
CF=
if [ -n "${CLANG_FORMAT:-}" ]; then
    version_ok "$CLANG_FORMAT" || die "CLANG_FORMAT=$CLANG_FORMAT is not clang-format $WANT ($("$CLANG_FORMAT" --version 2>&1 | head -1))"
    CF=$CLANG_FORMAT
elif command -v clang-format >/dev/null 2>&1 && version_ok clang-format; then
    CF=clang-format
elif [ -z "${FORMAT_CHECK_IN_DOCKER:-}" ] && command -v docker >/dev/null 2>&1 &&
        docker image inspect "$IMG" >/dev/null 2>&1; then
    # the file list goes in (made above, on the host); docker shares the home folder, not $TMPDIR,
    # so every file must be under $ROOT
    abs=()
    for f in "${files[@]}"; do
        a="$(cd "$(dirname "$f")" && pwd)/$(basename "$f")"
        case "$a" in "$ROOT"/*) ;; *) die "$f is outside $ROOT (docker cannot see it)";; esac
        abs+=("$a")
    done
    exec docker run --rm --platform=linux/arm64 -v "$ROOT:$ROOT" -w "$ROOT" \
        -e FORMAT_CHECK_IN_DOCKER=1 -e CLANG_FORMAT=/usr/local/bin/clang-format "$IMG" \
        bash "$ROOT/tools/format-check.sh" $([ $fix = 1 ] && echo --fix) "${abs[@]}"
else
    have=$(command -v clang-format >/dev/null 2>&1 && clang-format --version 2>&1 | head -1 || echo "none on PATH")
    die "clang-format $WANT not found (PATH: $have). Either
    pip install clang-format==$WANT     (then clang-format on PATH, or set CLANG_FORMAT)
  or build the docker image that carries it:
    docker build --platform=linux/arm64 -t $IMG -f docker/azmod-test.Dockerfile docker"
fi
echo "format-check: $("$CF" --version | head -1)"

if [ $fix = 1 ]; then
    "$CF" --style=file:"$ROOT/.clang-format" -i "${files[@]}"
    echo "format-check: reformatted ${#files[@]} files"
    exit 0
fi

# ---- the check: clang-format's output must be the file itself
bad=0
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
for f in "${files[@]}"; do
    [ -f "$f" ] || die "$f: no such file"
    "$CF" --style=file:"$ROOT/.clang-format" --assume-filename="$f" < "$f" > "$tmp" ||
        die "clang-format failed on $f"
    if ! cmp -s "$f" "$tmp"; then
        echo "UNFORMATTED  $f"
        diff -u "$f" "$tmp" | sed -n '3,14p' | sed 's/^/      /' || true
        bad=$((bad + 1))
    fi
done
if [ $bad -ne 0 ]; then
    echo "FORMAT CHECK: FAILED ($bad of ${#files[@]} files unformatted; tools/format-check.sh --fix reformats them)"
    exit 1
fi
echo "FORMAT CHECK: OK (${#files[@]} files)"
