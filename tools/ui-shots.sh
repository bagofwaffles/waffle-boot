#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Renders the held-mode screens (S-15) with the bootloader's own drawing
# code and writes them as PNG files, for docs/ui/ and for checking a change
# to the screens by eye (wiki/Screen-Style-Guide.md).
#
# Usage: tools/ui-shots.sh [output directory]   (default docs/ui)
#
# Builds the host library (make BUILD=$BUILD, default build), so it runs
# where `make check` does. Needs python3 (standard library only).
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-docs/ui}
build=${BUILD:-build}
make -s BUILD="$build" "$build/host/libwaffle.a" "$build/host/libcrc.a" "$build/host/libzlib.a"
raw="$build/ui-shots"
rm -rf "$raw"
mkdir -p "$raw" "$out"
# The pictures show this tree's version (tools/bump_version.py), and each PNG
# carries it, so that `tools/ui_shots.py --check` finds pictures left behind.
ver=$(python3 -I -B tools/bump_version.py --show)
major=${ver%%.*}
rest=${ver#*.}
minor=${rest%%.*}
patch=${rest#*.}
${CC:-cc} -std=c11 -O2 -DSHOT_MAJOR="$major" -DSHOT_MINOR="$minor" -DSHOT_PATCH="$patch" \
	-Isrc -Isrc/include tools/ui_shots.c src/front/display/draw.c src/front/display/font.c \
	src/front/recovery/screen.c src/front/recovery/ui.c src/front/recovery/proto.c src/front/slots/slots.c \
	src/front/slots/image.c src/front/start/util.c src/front/start/device.c \
	"$build/host/libwaffle.a" "$build/host/libcrc.a" "$build/host/libzlib.a" -o "$raw/ui-shots"
"$raw/ui-shots" "$raw"
python3 -I -B tools/ui_shots.py "$raw" "$out" "$ver"
