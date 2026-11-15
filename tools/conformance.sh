#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The conformance suite, as CI runs it:
#   0. every generated vector file under tests/oracle/ current, and no
#      generated test header tracked (tools/regen.py --check);
#   1. the self-tests of the five tests/oracle/conformance packages;
#   2. spec/S-*/test_*.py, only when a private spec/ checkout is present;
#   3. the host harness of tests/conformance/ (tests/conformance/README.md),
#      then coverage.py --check, which fails when a test's mark names a
#      scenario not in tests/conformance/scenarios.tsv, and COVERAGE.md,
#      generated (not tracked).
#
#   tools/conformance.sh            run all four
#   tools/conformance.sh harness    run only the harness (all that depends on CC)
#
# The four run at once, and so do the checks of step 2; each step's
# output is printed in one piece, in the order above, and any failure fails
# the whole.
#
# Variables: PYTHON (default python3), MAKE (default make), CC, BUILD
# (default build), passed through to the harness build.
# Python 3 standard library only.

set -eu

cd "$(dirname "$0")/.."

PYTHON=${PYTHON:-python3}
MAKE=${MAKE:-make}
BUILD=${BUILD:-build}

regen() {
	echo "== generated vector files"
	"$PYTHON" -I -B tools/regen.py --check
}

selftests() {
	echo "== conformance package self-tests"
	"$PYTHON" -I -B tests/oracle/conformance/images/selftest.py
	"$PYTHON" -I -B tests/oracle/conformance/fs/selftest.py
	"$PYTHON" -I -B tests/oracle/conformance/usb/selftest.py
	"$PYTHON" -I -B tests/oracle/conformance/hw/selftest.py
	"$PYTHON" -I -B -m unittest discover -s tests/oracle/conformance/serial -t tests/oracle/conformance
}

spectests() {
	echo "== spec/ checks"
	if [ ! -d spec ]; then
		echo "skipped: no spec/"
		return 0
	fi
	n=0
	for t in spec/S-*/test_*.py; do
		n=$((n + 1))
		"$PYTHON" -I -B "$t" > "$tmp/spec.$n" 2>&1 &
		eval "spec_$n=\$!"
	done
	failed=0
	n=0
	for t in spec/S-*/test_*.py; do
		n=$((n + 1))
		echo "-- $t"
		s=0
		eval "wait \$spec_$n" || s=$?
		cat "$tmp/spec.$n"
		if [ "$s" -ne 0 ]; then
			echo "conformance: $t failed (exit status $s)"
			failed=1
		fi
	done
	return $failed
}

harness() {
	echo "== host harness"
	if [ -n "${CC:-}" ]; then
		"$MAKE" -f tests/conformance/Makefile BUILD="$BUILD" CC="$CC"
	else
		"$MAKE" -f tests/conformance/Makefile BUILD="$BUILD"
	fi
	WB_CONFORMANCE_BUILD="$BUILD/conformance" "$PYTHON" -I -B tests/conformance/run.py
	"$PYTHON" -I -B tests/conformance/coverage.py --check
	"$PYTHON" -I -B tests/conformance/coverage.py
}

case "${1:-all}" in
all)
	tmp=$(mktemp -d "${TMPDIR:-/tmp}/wb-conformance.XXXXXX")
	trap 'rm -rf "$tmp"' EXIT
	trap 'exit 130' INT TERM
	for step in regen selftests spectests harness; do
		(set -e; "$step") > "$tmp/$step" 2>&1 &
		eval "pid_$step=\$!"
	done
	failed=
	for step in regen selftests spectests harness; do
		s=0
		eval "wait \$pid_$step" || s=$?
		cat "$tmp/$step"
		if [ "$s" -ne 0 ]; then
			failed="$failed $step"
		fi
	done
	if [ -n "$failed" ]; then
		echo "conformance: FAILED:$failed" >&2
		exit 1
	fi
	;;
harness)
	harness
	;;
*)
	echo "usage: $0 [all|harness]" >&2
	exit 2
	;;
esac
