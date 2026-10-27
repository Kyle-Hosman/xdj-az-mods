#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# build.sh: build src/out/azmod.so and src/out/azstem.so the way the official releases are built.
# Run tools/extract-assets.py first (see BUILDING.md).
set -eu
cd "$(dirname "$0")"
for h in caption_strips collabels fxart fxbars fxhdr holdtile menulabels sfxtile onelibkey onelibkey_sm srcref stems4art wavref; do
  [ -f "src/azmod/$h.h" ] || { echo "src/azmod/$h.h is missing: run python3 tools/extract-assets.py <official azmod.so> first" >&2; exit 1; }
done
docker image inspect azmod-build:18.04 >/dev/null 2>&1 ||
  docker build --platform=linux/arm64 -f docker/azmod-build.Dockerfile -t azmod-build:18.04 .
rm -rf src/out
docker run --rm --platform=linux/arm64 -v "$PWD/src":/g azmod-build:18.04 make -C /g CC=gcc azmod azstem
echo
if command -v shasum >/dev/null 2>&1; then shasum -a 256 src/out/azmod.so src/out/azstem.so
else sha256sum src/out/azmod.so src/out/azstem.so; fi
