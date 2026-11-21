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
| `make bump-version` | Raise the patch version. Do it once per build that goes onto a unit, then redraw the screenshots (`tools/ui-shots.sh docs/ui`): `make check` compares their version with the tree's |
| `make test-os` | The [Test OS](Test-OS) |
| `make installer` | The installer ([Installer Reference](Installer-Reference#building-it)) |

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
| A screen's wording | Edit `src/s15_menu.c` or `src/s15_lines.c` and `tests/vectors/s15/ui/`, then `make ui-update` ([Screen Style Guide](Screen-Style-Guide)) |

## The frozen front

`src/front/` is pinned: every cross build fails when its bytes change. A front change needs the
maintainer's sign-off, `make front-pin`, the `host`, `cross`, `repro` and `conformance` jobs, and a recovery
round on a unit before it is committed to a slot ([The Front](The-Front#changing-it)).

## Source map

Where the code for each [requirement label](Specification) (`S-NN.NNN`) lives. A `src/sNN_*.c` file holds the logic of one
label number; files without a number are shared (the boot flow, drawing, library adapters).

| Label | Topic | Sources |
| --- | --- | --- |
| S-01 | Boot chain and memory map | `src/s01_memmap.c`, `src/boot_flow.c`, `src/front/` |
| S-02 | SPI flash slots | `src/s02_*.c` |
| S-03 | eMMC layout | `src/s03_emmc.c` |
| S-04 | Persistent records | `src/s04_records.c` |
| S-05 | The OS hand-over | `src/s05_handoff.c` |
| S-06 | Serial framing | `src/s06_*.c` |
| S-07 | The PSoC link | `src/s07_*.c` |
| S-08 | The Nordic link | `src/s08_*.c` |
| S-09 | The SPI bridge between the two processors | `src/s09_*.c`, `src/wire_s09.c` |
| S-10a to S-10c | USB device and mass storage, MTP, host commands | `src/s10a_*.c`, `src/s10b_*.c`, `src/s10c_*.c` |
| S-11 | Firmware packages and installs | `src/s11_*.c`, `src/wire_package.c`, `src/wire_zip.c` |
| S-12 | Filesystems | `src/wire_fat.c`, `src/wire_yaffs.c`, `src/glue/` |
| S-13a, S-13b | Bring-up: clocks and DDR, buses and pins | `src/drivers/bf70x/` |
| S-13c | The display | `src/s13c_panel.c`, `src/display_*.c`, `src/board/opxy_display.c`, `src/drivers/bf70x/display.c` |
| S-14 | Logging | `src/s14_*.c` |
| S-15 | The menus' keys and actions | `src/s15_*.c` |
| S-16 | Compatibility and design register | across the above |
| S-17 | Board revisions | `src/s17_revisions.c` |

### Layers

| Path | What it holds |
| --- | --- |
| `src/*.c`, `src/include/waffle/*.h` | Hardware-independent logic behind operation structs. It includes no board or driver header, and no driver includes its headers; `tools/layering.py` checks this in `make check` and lists the crossings that remain |
| `src/wire_*.c` | Adapters from those structs to the vendored libraries (crypto, zip, filesystems, CRCs) |
| `src/board/`, `src/include/waffle/board/` | The board, for the OP-XY and the OP-1 Field: wiring the logic to the drivers, held mode, the test OS's hooks |
| `src/drivers/bf70x/`, `src/include/waffle/drivers/` | ADSP-BF70x drivers: start-up, CPLBs, interrupts, timers, clocks and DDR, pins, DMA, SPI and serial flash, UART, eMMC, OTP; the BF703 linker script |
| `src/front/` | The frozen front and recovery ([The Front](The-Front)) |
| `src/testos/` | The test OS's board files, linker script, and the SPI driver's test-OS parts, which `spi.c` includes in that build only, and the installer ([Test OS](Test-OS), [Installer Reference](Installer-Reference)) |
| `src/glue/`, `src/third_party/` | Our glue, and the vendored picolibc, FatFs, yaffs2, zlib and minizip, BearSSL, micro-ecc, Spleen and TinyUSB (listed in `src/third_party/README.md`) |
| `tests/unit/` | Host unit tests, driver tests against a register-access fake, the libraries tested end to end |
| `tests/conformance/` | The conformance tests: drive the code with the reference models of `tests/oracle/`; `coverage.py` writes an untracked `COVERAGE.md` listing every scenario in `scenarios.tsv` and how it is covered |
| `tests/fuzz/` | libFuzzer targets for the parsers |
| `tests/oracle/` | Test data and reference models, by label number, the generators that write the data, and the conformance packages |
| `tests/vectors/` | The unit tests' vectors, exported from `tests/oracle/`, and the screens' own (`s15/ui/`) |
| `tests/tools/` | The tools' own tests |
| `tools/` | Build, image, check and device tools ([`tools/README.md`](../blob/main/tools/README.md)) |
