# bfin-elf cross toolchain

`build.sh` fetches and builds the ADSP-BF703 cross toolchain from the public Blackfin+ GNU toolchain fork:

- Repository: <https://github.com/deanm1278/blackfin-plus-gnu>
- Pinned commit: `e4408a15b9be6cd911283f103a1a3f69e9a6b8c0` (tag `0.0.1`), recorded in `build.sh` as
  `TOOLCHAIN_COMMIT`
- Built: binutils 2.29.51 and GCC 8.0.0 (20171013 snapshot), C only, with libgcc, for target `bfin-elf`.
  No C library, no C++, no gdb.

The built toolchain is never committed.

## Usage

    tools/toolchain/build.sh                 # build (does nothing if already built)
    tools/toolchain/build.sh --print-prefix  # where it is installed
    tools/toolchain/build.sh --print-commit  # the pinned commit

It installs into
`${WAFFLE_TOOLCHAIN_CACHE:-${XDG_CACHE_HOME:-~/.cache}/waffle-boot/toolchain}/bfin-elf-<commit>`, where
`make cross` finds it. To use another `bfin-elf` toolchain, pass `CROSS_COMPILE=/path/to/bfin-elf-`.
`JOBS` sets the number of parallel jobs (default: all CPUs).

It fetches only the `binutils-gdb/` and `gcc/` trees of the pinned commit (shallow, sparse), and deletes
its working directory after a successful build. A full build takes about 6 minutes on a 4-CPU x86-64 Linux
machine.

`tools/ci-local.sh` builds it once into a Docker volume and reuses it until the pinned commit or `build.sh`
changes.

## Prerequisites

On Linux (tested on Ubuntu 24.04, host GCC 13):

    sudo apt-get install build-essential git libgmp-dev libmpfr-dev libmpc-dev flex bison

On macOS (tested on arm64, Xcode 16.4's clang 17), the Command Line Tools and:

    brew install gmp mpfr libmpc

Both need about 1.5 GB of free disk during the build (the source fetch is about 1.4 GB; the installed
toolchain about 55 MB) and network access to github.com.

The 2017-era sources need a few host settings, all in `build.sh`: `-std=gnu11` and `-std=gnu++14 -fcommon`
for the host, no manuals (`MAKEINFO=true`), and on macOS Homebrew's paths, the bundled zlib's `fdopen`
declaration kept, and on non-x86 Macs GCC 8's default host hooks.

One source patch, `gcc-host-qsort.patch`, keeps the compiler's output the same on every host. GCC 8 sorts
with the host C library's `qsort`, and some of its comparators leave elements equal (induction variable
candidates in `add_iv_candidate_derived_from_uses`, `tree-ssa-loop-ivopts.c`) or are not consistent (the
scheduler's `rank_for_schedule`). The GNU C library keeps equal elements in order and macOS does not, so
the two hosts emitted some instructions in a different order. The patch routes `qsort_chk` (`vec.c`, which
every `qsort` call in `gcc/` reaches in this checking build) and the scheduler's direct call through a merge
sort that makes the same comparisons as the GNU C library's, so the output on Linux is unchanged. The stamp
in the install directory records the patches; a toolchain built without them is rebuilt.

## Notes

- The Makefile passes `-mcpu=bf703-none`. The compiler maps every BF70x part to the `bf706-none` multilib
  (`gcc/config/bfin/t-bfin-elf`).
- The toolchain brings no C library. The linker script (`src/drivers/bf70x/bf703.ld`), the start-up code
  (`src/drivers/bf70x/start.S`) and the picolibc subset (`src/third_party/`) are this repository's.
