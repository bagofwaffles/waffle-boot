## What you need

- Linux, with a C compiler, Python 3, git and GNU binutils (`objcopy`), for the host build and tests.
- The bfin-elf cross toolchain (GCC 8) for the unit's image. `tools/toolchain/build.sh` builds it from a
  public Blackfin+ GNU fork ([`tools/toolchain/README.md`](../blob/main/tools/toolchain/README.md)).
- Docker, for `tools/ci-local.sh`.

On macOS the toolchain and `make cross` work natively, but `make check` does not link (picolibc's symbol
aliases). Use `tools/ci-local.sh` there.

## Build

| Command | Result |
| --- | --- |
| `make cross` | The cross build with every gate: `build/bf703/waffle-boot.ldr` (the boot stream), `.elf`, `.layout.json` |
| `make image LOGO=my-logo.png` | Your own 480 × 222 picture at the OS hand-over |
| `make image OPXY_BL_BUILD=n` | A build number, so two builds differ |
| `make bump-version` | Raise the patch version. Do it once per build that goes onto a unit |
| `make test-os` | The [Test OS](Test-OS) |

## Test

| Command | What it runs |
| --- | --- |
| `make check` | Host build, unit tests, tool tests, layering and leak checks |
| `tools/conformance.sh` | The conformance packages' self-tests, then the conformance harness and its coverage check |
| `python3 -I -B tools/regen.py --check` | Checks that the generated test data is current |
| `make analyze cppcheck` | Static analysis |
| `make fuzz` | libFuzzer targets for the parsers (clang) |
| `tools/repro-check.sh host`, `tools/repro-check.sh cross` | Builds twice in different places and compares |

## Local CI

`tools/ci-local.sh` runs the checks in an Ubuntu 24.04 container on a copy of your tree:

    tools/ci-local.sh                  # host tests, cross build, conformance
    tools/ci-local.sh all              # everything, with gcc and clang
    tools/ci-local.sh host analysis    # chosen jobs

Jobs: `host`, `analysis`, `repro`, `cross`, `fuzz`, `conformance`. The first toolchain build is slow; it is
kept in a Docker volume.

| A change to | Run |
| --- | --- |
| docs or comments only | `python3 -I -B tools/leak-check.py tree` |
| `src/` | `host analysis`, plus `cross` when the image changes, `repro` when the build changes |
| `tests/oracle/`, vectors, the conformance harness | `conformance`, plus `host` when vectors change |
| A screen's wording | Edit `src/s15_menu.c` or `src/s15_lines.c` and `tests/vectors/s15/ui/`, then `make ui-update` |

## The frozen front

`src/front/` is pinned: every cross build fails when its bytes change. A front change needs the
maintainer's sign-off, `make front-pin`, the `host`, `cross`, `repro` and `conformance` jobs, and a recovery
round on a unit before it is committed to a slot.

## Layout

| Path | What it holds |
| --- | --- |
| `src/sNN_*.c` | The logic of one requirement label number each ([Requirement Labels](Specification)) |
| `src/front/` | The frozen front and recovery |
| `src/board/`, `src/drivers/bf70x/` | The OP-XY board, and the chip's drivers |
| `src/glue/`, `src/third_party/` | Vendored libraries (picolibc, FatFs, yaffs2, zlib, minizip, BearSSL, micro-ecc, TinyUSB), the Spleen font, and their adapters |
| `tests/unit/`, `tests/conformance/`, `tests/fuzz/` | Unit tests, the conformance harness, fuzz targets |
| `tests/oracle/` | Test data and reference models |
| `tools/` | Build, image, check and device tools ([`tools/README.md`](../blob/main/tools/README.md)) |
