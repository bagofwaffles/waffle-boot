#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Run the jobs of .github/workflows/ci.yml in local Linux containers
# (Ubuntu 24.04, tools/ci/Dockerfile), so that problems show up before a push.
#
# Usage: tools/ci-local.sh [options] [job...]
#
#   light        (default) a draft pull request's jobs: host build and tests
#                (gcc), the cross build with the image and size checks, the
#                conformance suite (gcc)
#   all          a full run: every job below
#   host         host build and tests, gcc and clang
#   analysis     clang static analyzer and cppcheck
#   repro        the reproducible builds: host (gcc and clang) and cross
#   cross        the toolchain and the cross build, with the image, size and
#                test OS checks
#   fuzz         the fuzzers, FUZZ_TIME seconds each (default 60)
#   conformance  the conformance suite: all of it with gcc, the harness with
#                clang
#
# Independent jobs run side by side, one container each, with their output
# in a log file each; the summary names the failures and shows the end of
# their logs. With --serial they run one after another, in one container
# per platform, with the output on the terminal, and stop at the first
# failure.
#
# They run on a copy of the working tree: the committed files with their
# uncommitted changes, and untracked files that are not ignored (so not
# build/). Nothing from the host is mounted; the copy goes in on stdin. The
# bfin-elf toolchain is built once into a Docker volume named, like CI's
# cache key, after the pinned commit, the platform and tools/toolchain/build.sh.
# Compiles go through ccache, in a volume of its own per platform, except in
# the reproducibility checks and the toolchain build.
#
# The toolchain, cross and reproducibility jobs run on linux/amd64, as on CI;
# the others run on the machine's own platform (linux/arm64 on an arm64
# machine), where they need no emulation.
#
#   -j N, --jobs N     at most N containers at once (default 4)
#   --serial           one job at a time, in one container per platform
#   --no-ccache        compile without ccache
#   --platform P       every job on platform P (also CI_LOCAL_PLATFORM);
#                      --platform linux/amd64 is CI's platform throughout
#   DOCKER             container command (default docker)
#   CI_LOCAL_MAKE_JOBS make -j in each container (default: all the
#                      container's CPUs, so a long job left alone uses them all)
#   FUZZ_TIME          seconds per fuzzer

set -euo pipefail

usage() {
	awk 'NR >= 7 && /^#/ { sub(/^# ?/, ""); print; next } NR >= 7 { exit }' "$0" >&2
	exit "${1:-2}"
}

platform=${CI_LOCAL_PLATFORM:-}
docker=${DOCKER:-docker}
limit=4
serial=0
ccache=1
names=()
while [ $# -gt 0 ]; do
	case $1 in
	-j|--jobs)    [ $# -ge 2 ] || usage; limit=$2; shift 2 ;;
	-j*)          limit=${1#-j}; shift ;;
	--jobs=*)     limit=${1#*=}; shift ;;
	--serial)     serial=1; shift ;;
	--no-ccache)  ccache=0; shift ;;
	--platform)   [ $# -ge 2 ] || usage; platform=$2; shift 2 ;;
	--platform=*) platform=${1#*=}; shift ;;
	-h|--help)    usage 0 ;;
	light|all|host|analysis|repro|cross|fuzz|conformance) names+=("$1"); shift ;;
	*) echo "ci-local: unknown job or option: $1" >&2; usage ;;
	esac
done
case $limit in
''|*[!0-9]*|0) echo "ci-local: -j needs a positive number" >&2; usage ;;
esac
if [ ${#names[@]} -eq 0 ]; then
	names=(light)
fi

# The jobs of each name (tools/ci/jobs.sh), in the workflow's order. A job's
# own name runs it as a full run does.
expand() {
	case $1 in
	light)       echo host:gcc toolchain cross conformance:gcc ;;
	all)         echo host:gcc host:clang analysis repro:gcc repro:clang \
	                  toolchain cross repro:cross fuzz conformance:gcc conformance:clang ;;
	host)        echo host:gcc host:clang ;;
	analysis)    echo analysis ;;
	repro)       echo repro:gcc repro:clang toolchain repro:cross ;;
	cross)       echo toolchain cross ;;
	fuzz)        echo fuzz ;;
	conformance) echo conformance:gcc conformance:clang ;;
	esac
}
want=" "
for n in "${names[@]}"; do
	for j in $(expand "$n"); do
		case $want in *" $j "*) ;; *) want="$want$j " ;; esac
	done
done
specs=()
for j in host:gcc host:clang analysis repro:gcc repro:clang toolchain \
	cross repro:cross fuzz conformance:gcc conformance:clang; do
	case $want in *" $j "*) specs+=("$j") ;; esac
done

label() {
	case $1 in
	*:*) echo "${1%%:*} (${1#*:})" ;;
	*)   echo "$1" ;;
	esac
}
duration() { printf '%dm%02ds' $(($1 / 60)) $(($1 % 60)); }

cd "$(git -C "$(dirname "$0")/.." rev-parse --show-toplevel)"

if ! command -v "$docker" > /dev/null; then
	echo "ci-local: $docker not found (set DOCKER for another container command)" >&2
	exit 2
fi

# The machine's own container platform.
case $("$docker" info --format '{{.Architecture}}' 2> /dev/null || uname -m) in
aarch64|arm64) native=linux/arm64 ;;
*)             native=linux/amd64 ;;
esac
# The platform a job runs on.
platform_of() {
	if [ -n "$platform" ]; then
		echo "$platform"
		return
	fi
	case $1 in
	toolchain|cross|repro:*) echo linux/amd64 ;;
	*)                       echo "$native" ;;
	esac
}
plat() { echo "$1" | tr / -; }

commit=$(tools/toolchain/build.sh --print-commit)
script=$(git hash-object tools/toolchain/build.sh)
toolchain_volume() { echo "waffle-boot-bfin-elf-${commit:0:12}-$(plat "$1")-${script:0:12}"; }

platforms=
emulated=
for j in "${specs[@]}"; do
	p=$(platform_of "$j")
	case " $platforms " in *" $p "*) ;; *) platforms="$platforms${platforms:+ }$p" ;; esac
	if [ "$p" != "$native" ]; then
		emulated="$emulated${emulated:+, }$(label "$j")"
	fi
done
if [ -n "$emulated" ]; then
	echo "ci-local: emulated on this machine ($native), so slower: $emulated" >&2
fi

started=$SECONDS
for p in $platforms; do
	echo "==> ci-local: container image waffle-boot-ci:$(plat "$p")"
	if ! "$docker" build --platform "$p" -t "waffle-boot-ci:$(plat "$p")" \
		-f tools/ci/Dockerfile tools/ci >&2; then
		echo "ci-local: FAILED: building the container image for $p" >&2
		exit 1
	fi
done
cpus=$("$docker" info --format '{{.NCPU}}' 2> /dev/null || echo 2)
case $cpus in ''|*[!0-9]*|0) cpus=2 ;; esac

# The tree to copy, written through a scratch index so the real one is left
# alone.
scratch=$(mktemp -d "${TMPDIR:-/tmp}/ci-local.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
index=$(git rev-parse --git-path index)
if [ -f "$index" ]; then
	cp "$index" "$scratch/index"
fi
GIT_INDEX_FILE=$scratch/index git add -A
tree=$(GIT_INDEX_FILE=$scratch/index git write-tree)
git archive --format=tar -o "$scratch/tree.tar" "$tree"

# run <make jobs> <job>...: the jobs, all of one platform, in one container
# named so that an interrupted run can remove it. Only the jobs that use the
# toolchain mount its volume.
run() {
	local make_jobs=${CI_LOCAL_MAKE_JOBS:-$1} p j mounts=()
	shift
	p=$(platform_of "$1")
	for j in "$@"; do
		case $j in
		toolchain|cross|repro:cross)
			mounts=(--mount "type=volume,src=$(toolchain_volume "$p"),dst=/cache")
			break ;;
		esac
	done
	"$docker" run --rm -i --init --platform "$p" \
		--name "ci-local-$$-$(echo "$*" | tr ' :' '--')" \
		${mounts[@]+"${mounts[@]}"} \
		--mount "type=volume,src=waffle-boot-ccache-$(plat "$p"),dst=/ccache" \
		-e FUZZ_TIME -e CI_LOCAL_CCACHE=$ccache -e CI_LOCAL_MAKE_JOBS="$make_jobs" \
		"waffle-boot-ci:$(plat "$p")" \
		bash -c 'mkdir src && tar -xf - -C src && exec src/tools/ci/jobs.sh "$HOME/src" "$@"' \
		jobs.sh "$@" < "$scratch/tree.tar"
}

echo "==> ci-local: jobs: ${specs[*]}"
for p in $platforms; do
	group=
	for j in "${specs[@]}"; do
		[ "$(platform_of "$j")" = "$p" ] && group="$group${group:+ }$j"
	done
	echo "    $p: $group"
done
echo "    ccache $([ $ccache = 1 ] && echo "volume per platform" || echo off)"

# One after another, in order; consecutive jobs of one platform share a
# container.
if [ $serial = 1 ]; then
	status=0
	group=()
	for j in "${specs[@]}" ''; do
		if [ ${#group[@]} -gt 0 ] &&
			{ [ -z "$j" ] || [ "$(platform_of "$j")" != "$(platform_of "${group[0]}")" ]; }; then
			run "$cpus" "${group[@]}" || { status=$?; break; }
			group=()
		fi
		[ -z "$j" ] || group+=("$j")
	done
	echo "ci-local: $([ $status = 0 ] && echo "all passed" || echo "stopped at the failure above") in $(duration $((SECONDS - started)))"
	exit $status
fi

# The toolchain first, on its own: the cross build and its reproducibility
# check need it.
rest=()
for j in "${specs[@]}"; do
	if [ "$j" = toolchain ]; then
		if ! run "$cpus" toolchain; then
			echo "ci-local: FAILED: toolchain" >&2
			exit 1
		fi
	else
		rest+=("$j")
	fi
done
if [ ${#rest[@]} -eq 0 ]; then
	exit 0
fi

logs=$(mktemp -d "${TMPDIR:-/tmp}/ci-local-logs.XXXXXX")
n=${#rest[@]}
at_once=$((limit < n ? limit : n))
make_jobs=$cpus
echo "==> ci-local: $n jobs, $at_once at a time, make -j${CI_LOCAL_MAKE_JOBS:-$make_jobs} each; logs in $logs"
# On an interrupt, stop the containers still running.
cleanup() {
	local j
	for j in "${rest[@]:0:$launched}"; do
		[ -f "$(log "$j").status" ] ||
			"$docker" rm -f "ci-local-$$-$(echo "$j" | tr : -)" > /dev/null 2>&1 || true
	done
	rm -rf "$scratch"
}
launched=0
trap cleanup EXIT
trap 'exit 130' INT TERM

log() { echo "$logs/$(echo "$1" | tr : -)"; }
finished=0
reported=" "
failed=()
while [ $finished -lt $n ]; do
	while [ $launched -lt $n ] && [ $((launched - finished)) -lt "$limit" ]; do
		j=${rest[$launched]}
		f=$(log "$j")
		(
			t0=$SECONDS
			s=0
			run "$make_jobs" "$j" > "$f.log" 2>&1 || s=$?
			echo $((SECONDS - t0)) > "$f.time"
			echo $s > "$f.status"
		) &
		echo "    started $(label "$j")"
		launched=$((launched + 1))
	done
	sleep 1
	for j in "${rest[@]:0:$launched}"; do
		f=$(log "$j")
		case $reported in *" $j "*) continue ;; esac
		[ -f "$f.status" ] || continue
		reported="$reported$j "
		finished=$((finished + 1))
		if [ "$(cat "$f.status")" = 0 ]; then
			echo "    passed  $(label "$j") in $(duration "$(cat "$f.time")")"
		else
			echo "    FAILED  $(label "$j") after $(duration "$(cat "$f.time")")"
			failed+=("$j")
		fi
	done
done
wait

total=$(duration $((SECONDS - started)))
if [ ${#failed[@]} -eq 0 ]; then
	echo "ci-local: all passed in $total"
	exit 0
fi
for j in "${failed[@]}"; do
	echo
	echo "--- $(label "$j"): the end of $(log "$j").log"
	tail -n 40 "$(log "$j").log"
done
echo
names=
for j in "${failed[@]}"; do
	names="$names${names:+, }$(label "$j")"
done
echo "ci-local: FAILED: $names ($total in all; logs in $logs)"
exit 1
