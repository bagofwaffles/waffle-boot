#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The CI jobs, run inside the tools/ci-local.sh container the way
# .github/workflows/ci.yml runs them. Not meant to be run on the host.
#
# Usage: tools/ci/jobs.sh <tree> <job>...
#
# <tree> is the extracted copy of the working tree. A <job> is one of
# host:gcc, host:clang, analysis, repro:gcc, repro:clang, repro:cross,
# toolchain, cross, fuzz, conformance:gcc, conformance:clang. They run
# in order, each in a fresh copy of the tree, as each CI job runs in a fresh
# checkout (make does not rebuild when only CC changes), and stop at the
# first failure.
#
# Environment: CI_LOCAL_MAKE_JOBS (make -j, default all CPUs), CI_LOCAL_CCACHE
# (0 turns ccache off), FUZZ_TIME.

set -euo pipefail

src=$1
shift
work=$HOME/work/waffle-boot/waffle-boot

export MAKEFLAGS=-j${CI_LOCAL_MAKE_JOBS:-$(nproc)}

# ccache, in its own volume, for every compile except the reproducibility
# checks and the toolchain build, which run as in CI.
ccache=${CI_LOCAL_CCACHE:-1}
if [ "$ccache" = 1 ]; then
	export PATH=/usr/lib/ccache:$PATH
fi
nocache() { CCACHE_DISABLE=1 "$@"; }

# repro-check.sh lists the tree with git.
git -C "$src" init -q
git -C "$src" add -A

job_host() {
	"$1" --version
	make CC="$1" check
}

job_analysis() {
	clang --version
	cppcheck --version
	make analyze
	make cppcheck
}

# repro:cross is the reproducibility step of CI's cross job; it needs the
# toolchain.
job_repro() {
	if [ "$1" = cross ]; then
		nocache tools/repro-check.sh cross
	else
		MAKEFLAGS="$MAKEFLAGS CC=$1" nocache tools/repro-check.sh host
	fi
}

# Built once into the volume; a no-op when it is already there.
job_toolchain() {
	nocache flock /cache/.lock tools/toolchain/build.sh
}

# The disassembly CI prints goes to a file here; it is only the check that
# objdump reads the library. The reproducibility check is repro:cross.
# Besides the release image and the test OS's panel variants, the build
# variants no other job compiles: the test OS's monitor and installer, and a
# development image with breadcrumbs, timing and debug logging, carried by
# its own installer.
job_cross() {
	local bin cc=()
	bin=$(tools/toolchain/build.sh --print-prefix)/bin
	if [ "$ccache" = 1 ]; then
		cc=(CROSS_CC="ccache $bin/bfin-elf-gcc")
	fi
	make cross "${cc[@]}"
	"$bin/bfin-elf-objdump" -d build/bf703/libwaffle.a > build/bf703/libwaffle.objdump
	make size
	make test-os test-os-mon test-os-install "${cc[@]}"
	make image test-os-install BOOT_CRUMBS=1 BOOT_TIMING=1 LOG_DEBUG=1 \
		BUILD=build/dev "${cc[@]}"
}

# CI keeps the findings as an artifact; the container is discarded, so
# print them instead.
job_fuzz() {
	clang --version
	make fuzz
	make fuzz-run FUZZ_TIME="${FUZZ_TIME:-60}" || {
		for f in build/fuzz/*-crash-* build/fuzz/*-leak-* \
			build/fuzz/*-timeout-* build/fuzz/*-oom-*; do
			[ -e "$f" ] || continue
			echo "--- finding $f (base64):"
			base64 "$f"
		done
		return 1
	}
}

# Only the harness depends on the compiler: the clang job runs only that.
job_conformance() {
	python3 --version
	"$1" --version
	CC=$1 tools/conformance.sh "$([ "$1" = gcc ] && echo all || echo harness)"
	# CI publishes the whole of COVERAGE.md; here the totals, as the
	# container is discarded (coverage.py runs on the host as well).
	sed -n '/^| Scenarios/,/^$/p' tests/conformance/COVERAGE.md
}

for j in "$@"; do
	job=${j%%:*}
	arg=
	if [[ $j == *:* ]]; then
		arg=${j#*:}
	fi
	label=$job${arg:+ ($arg)}
	if ! declare -F "job_$job" > /dev/null; then
		echo "jobs.sh: unknown job: $j" >&2
		exit 2
	fi
	echo "==> ci-local: $label ($MAKEFLAGS, ccache $([ "$ccache" = 1 ] && echo on || echo off))"
	start=$SECONDS
	rm -rf "$work"
	mkdir -p "$(dirname "$work")"
	cp -a "$src" "$work"
	set +e
	(set -e; cd "$work"; "job_$job" ${arg:+"$arg"})
	status=$?
	set -e
	t=$((SECONDS - start))
	if [ "$status" -ne 0 ]; then
		printf 'ci-local: FAILED: %s after %dm%02ds\n' "$label" $((t / 60)) $((t % 60)) >&2
		exit 1
	fi
	printf 'ci-local: passed: %s in %dm%02ds\n' "$label" $((t / 60)) $((t % 60))
done
