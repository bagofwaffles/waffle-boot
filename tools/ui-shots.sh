#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Renders the held-mode screens (S-15) with the bootloader's own drawing
# code and writes them as PNG files, for docs/ui/ and for checking a change
# to the screens by eye (docs/ui-style-guide.md).
#
# Usage: tools/ui-shots.sh [output directory]   (default docs/ui)
#
# Builds the host library (make BUILD=$BUILD, default build), so it runs
# where `make check` does. Needs python3 (standard library only).
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-docs/ui}
build=${BUILD:-build}
make -s BUILD="$build" "$build/host/libwaffle.a"
raw="$build/ui-shots"
rm -rf "$raw"
mkdir -p "$raw" "$out"
${CC:-cc} -std=c11 -O2 -Isrc -Isrc/include tools/ui_shots.c src/front/fr_draw.c src/front/fr_font.c \
	src/front/fr_recscreen.c src/front/fr_recui.c src/front/fr_proto.c src/front/fr_slots.c \
	src/front/fr_image.c src/front/fr_util.c \
	"$build/host/libwaffle.a" -o "$raw/ui-shots"
"$raw/ui-shots" "$raw"
python3 -I -B tools/ui_shots.py "$raw" "$out"
