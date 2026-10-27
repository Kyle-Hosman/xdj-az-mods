#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# tests/run.sh: the host-native unit tests of the mods (tests/unit), on this source tree.
#
# The suite is written against the development layout (guest/azmod, guest/azstem,
# tools/unit-tests). This script assembles that layout from this tree in tests/.work/tree
# (copies, not links: suites that run in the build container mount it, and a container
# cannot follow a link that points outside its mount), then runs tests/unit/run.sh there with
# AZ_PUBLIC_TREE=1. In that mode the checks that need code or data this tree does not carry
# (development builds, the git history, the address notes, firmware-derived images) are
# reported as SKIP with the reason; nothing is counted as a pass that did not run.
#
# Needs: a C compiler, perl, python3, and Docker with azmod-build:18.04 for the suites that
# run on aarch64 Linux (build.sh creates the image). Like build.sh, it needs the asset headers
# that tools/extract-assets.py writes from an official azmod.so. On Linux, the full suite and
# the sanitizer passes (SAN=asan, SAN=tsan) run in azmod-test:18.04, the release toolchain plus
# the test tools:
#   docker build --platform=linux/arm64 -t azmod-test:18.04 -f docker/azmod-test.Dockerfile docker
# tests/run-linux.sh builds both images when missing and runs all three passes in azmod-test.
#
# Usage: tests/run.sh            HOST_CC overrides the compiler; CLANG_FORMAT names a
#                                clang-format 21.1.8 for the formatting check; SAN=asan or
#                                SAN=tsan runs a sanitizer pass (Linux only);
#                                AZ_COMPILE_ONLY=1 builds every test program and runs none
set -euo pipefail
cd "$(dirname "$0")/.."
PUB=$PWD
for h in caption_strips collabels fxart fxbars fxhdr holdtile menulabels sfxtile onelibkey onelibkey_sm srcref stems4art wavref; do
    [ -f "src/azmod/$h.h" ] || { echo "src/azmod/$h.h is missing: run python3 tools/extract-assets.py <official azmod.so> first" >&2; exit 2; }
done
V=$PUB/tests/.work/tree
rm -rf "$PUB/tests/.work"
mkdir -p "$V/guest/out" "$V/tools/refactor" "$V/docs"
for d in azmod azstem common azhandoff; do
    mkdir -p "$V/guest/$d"
    cp -R "src/$d/." "$V/guest/$d/"
done
cp src/azmod/test/lanetest.c "$V/guest/azmod/lanetest.c"
cp tests/unit/common/*.c "$V/guest/common/"
mkdir -p "$V/guest/azrs" && cp deck/azrs/azrs.sh "$V/guest/azrs/azrs.sh"
cp src/Makefile "$V/guest/Makefile"
cp -R tests/unit "$V/tools/unit-tests"
cp tools/format-check.sh tools/ep147-pngtable.py tools/static-check.sh "$V/tools/"
cp -R tools/static-check "$V/tools/static-check"
cp -R tools/fuzz "$V/tools/fuzz"
cp src/.clang-tidy "$V/guest/.clang-tidy"
cp tools/build-check.sh "$V/tools/refactor/build-check.sh"
mkdir -p "$V/tools/public-source/skel" && cp verify.sh "$V/tools/public-source/skel/verify.sh"
cp -R docker "$V/docker"
cp .clang-format "$V/.clang-format"
cp docs/MOD-MENU-KEYS.md "$V/docs/MOD-MENU-KEYS.md"
cd "$V"
AZ_PUBLIC_TREE=1 bash tools/unit-tests/run.sh
