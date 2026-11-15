#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Reproducible-build check: copy the tracked tree into two fresh directories
# at different paths, build each from clean with a different umask, locale
# and time zone, and require the outputs to be byte-identical.
#
# Usage: tools/repro-check.sh [host|cross|all]   (default: host)
#
# Extra make arguments may be passed in MAKEFLAGS (e.g. CC=clang).

set -euo pipefail

what=${1:-host}
case "$what" in
	host)  targets=(host) ;;
	cross) targets=(cross) ;;
	all)   targets=(host cross) ;;
	*) echo "usage: $0 [host|cross|all]" >&2; exit 2 ;;
esac

top=$(git -C "$(dirname "$0")/.." rev-parse --show-toplevel)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/waffle-repro.XXXXXX")
trap 'rm -rf "$scratch"' EXIT

# Copy the source files (tracked, plus untracked ones not ignored) as they
# are in the working tree; build outputs are ignored and so never copied.
copy_tree() {
	mkdir -p "$1"
	git -C "$top" ls-files -z --cached --others --exclude-standard |
		(cd "$top" && tar --null -T - -cf -) | tar -xf - -C "$1"
}

build() {
	local dir=$1 mask=$2 lang=$3 tz=$4
	(cd "$dir" && umask "$mask" && LC_ALL=$lang TZ=$tz \
		make -s --no-print-directory "${targets[@]}")
}

a=$scratch/a
b=$scratch/second-tree-at-a-longer-path/b
copy_tree "$a"
copy_tree "$b"

echo "==> build 1: $a"
build "$a" 022 C UTC
sleep 1   # make any embedded timestamp differ
echo "==> build 2: $b"
build "$b" 077 C.UTF-8 Pacific/Auckland

# Compare every output file (dependency files are build metadata, not output).
(cd "$a/build" && find . -type f ! -name '*.d' | LC_ALL=C sort) > "$scratch/list-a"
(cd "$b/build" && find . -type f ! -name '*.d' | LC_ALL=C sort) > "$scratch/list-b"
if ! cmp -s "$scratch/list-a" "$scratch/list-b"; then
	echo "FAIL: the two builds produced different sets of files" >&2
	diff "$scratch/list-a" "$scratch/list-b" >&2 || true
	exit 1
fi
if [ ! -s "$scratch/list-a" ]; then
	echo "FAIL: the build produced no output" >&2
	exit 1
fi

status=0
while read -r f; do
	if cmp -s "$a/build/$f" "$b/build/$f"; then
		printf 'same  %s  %s\n' "$(sha256sum < "$a/build/$f" | cut -d' ' -f1)" "${f#./}"
	else
		printf 'DIFF  %s\n' "${f#./}"
		status=1
	fi
done < "$scratch/list-a"

if [ "$status" -ne 0 ]; then
	echo "FAIL: build is not reproducible" >&2
	exit 1
fi
echo "OK: ${targets[*]} build is reproducible"
