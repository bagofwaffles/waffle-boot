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
| A screen's wording | Edit `src/core/ui/menu.c` or `src/core/ui/lines.c` and `tests/vectors/s15/ui/`, then `make ui-update` ([Screen Style Guide](Screen-Style-Guide)) |

## The frozen front

`src/front/` is pinned: every cross build fails when its bytes change. A front change needs the
maintainer's sign-off, `make front-pin`, the `host`, `cross`, `repro` and `conformance` jobs, and a recovery
round on a unit before it is committed to a slot ([The Front](The-Front#changing-it)).

## Source map

| Path | What it holds |
| --- | --- |
| `src/core/<topic>/`, `src/include/waffle/<topic>/` | Hardware-independent logic, one folder per topic: `boot`, `flash`, `emmc`, `link`, `psoc`, `nordic`, `bridge`, `usb`, `mtp`, `host`, `install`, `fs`, `display`, `ui`, `log`, `util` |
| `src/board/`, `src/include/waffle/board/` | The board, for the OP-XY and the OP-1 Field, wiring the logic to the drivers: `boot/`, `storage/`, `usb/`, `install/`, `ui/` (the boot menu and its services), `dev/` (developer mode tools) |
| `src/drivers/bf70x/`, `src/include/waffle/drivers/` | ADSP-BF70x drivers: start-up, CPLBs, interrupts, timers, clocks and DDR, pins, DMA, SPI and serial flash, UART, the eMMC, OTP; the BF703 linker script |
| `src/front/` | The frozen front and recovery, by topic: `start/`, `slots/`, `psoc/`, `display/`, `recovery/`, `hw/` ([The Front](The-Front)) |
| `src/stub/` | The stub that unpacks the main image, linked alone |
| `src/testos/`, `src/include/waffle/testos/` | The test OS and the installer, their linker scripts, and the SPI driver's test-OS parts, which `spi.c` includes in that build only ([Test OS](Test-OS), [Installer Reference](Installer-Reference)) |
| `src/glue/`, `src/third_party/` | Our glue, and the vendored picolibc, FatFs, yaffs2, zlib and minizip, BearSSL, micro-ecc, Spleen and TinyUSB (listed in `src/third_party/README.md`) |

File names say what the code does. The [requirement labels](Specification) (`S-NN.NNN`) in comments tie
it to the requirements, so `git grep S-02.084` finds the code for one. Where each label number's code lives
for the most part:

| Label | Sources |
| --- | --- |
| S-01 | `src/core/boot/memmap.c`, `src/core/boot/flow.c`, `src/board/boot/`, `src/front/`, `src/stub/`, `src/drivers/bf70x/bf703.ld` |
| S-02 | `src/core/flash/`, `src/front/slots/`, `src/front/recovery/`, `src/board/install/install.c` |
| S-03 | `src/core/emmc/emmc.c`, `src/board/storage/storage.c` |
| S-04 | `src/core/boot/records.c`, `src/board/ui/services.c` |
| S-05 | `src/core/boot/handoff.c`, `src/board/boot/handover.c` |
| S-06 | `src/core/link/`, `src/front/psoc/` |
| S-07 | `src/core/psoc/`, `src/front/psoc/` |
| S-08 | `src/core/nordic/` |
| S-09 | `src/core/bridge/`, `src/board/boot/master.c`, `src/board/boot/slave.c` |
| S-10a | `src/core/usb/`, `src/board/usb/usb.c` |
| S-10b | `src/core/mtp/`, `src/board/usb/mtp.c` |
| S-10c | `src/core/host/`, `src/board/usb/hostlink.c`, `src/board/dev/devhost.c` |
| S-11 | `src/core/install/`, `src/board/install/` |
| S-12 | `src/core/fs/`, `src/glue/` |
| S-13a, S-13b | `src/drivers/bf70x/`, `src/board/boot/values.c`, `src/front/start/init.c`, `src/front/hw/` |
| S-13c | `src/core/display/panel.c`, `src/core/display/draw.c`, `src/board/ui/display.c`, `src/drivers/bf70x/display.c`, `src/front/display/` |
| S-14 | `src/core/log/` |
| S-15 | `src/core/ui/`, `src/board/ui/` |
| S-16 | across the above |
| S-17 | `src/core/boot/revisions.c` |
| S-18 | `src/core/boot/device.c`, `src/core/boot/device_identify.c`, `src/front/start/device.c` |

### Layers

`tools/layering.py` checks the includes in `make check`, by where a file lives:

| Layer | Paths | May include |
| --- | --- | --- |
| core | `src/core/`, `src/include/waffle/<topic>/` | no board, driver or test OS file; it works behind operation structs |
| board | `src/board/`, `src/include/waffle/board/`, `src/include/waffle/testos.h`, `src/include/waffle/testos/` | anything |
| driver | `src/drivers/`, `src/include/waffle/drivers/` | no core or board header |
| test OS | `src/testos/` | anything |

Every layer may include `waffle/util/bytes.h` and `waffle/util/div64.h`, which include none of the layers'
headers. The crossings that remain are listed, each with its reason, in `tools/layering.py`.

The front is checked apart by `tools/front_image.py` in every cross build: it is compiled only from
`src/front/` and `src/include/waffle/drivers/`, and nothing outside it includes a front header except
`src/front/handoff.h`. `src/front/sources.mk` lists the front's sources in link order, which decides its
loaded bytes; a new front source goes there, and the build fails on one it does not list. `src/stub/`,
`src/glue/` and `src/third_party/` are not checked.

### Tests and tools

| Path | What it holds |
| --- | --- |
| `tests/unit/` | Host unit tests, driver tests against a register-access fake, the libraries tested end to end |
| `tests/conformance/` | The conformance tests: drive the code with the reference models of `tests/oracle/`; `coverage.py` writes an untracked `COVERAGE.md` listing every scenario in `scenarios.tsv` and how it is covered |
| `tests/fuzz/` | libFuzzer targets for the parsers |
| `tests/oracle/` | Test data and reference models, by label number, the generators that write the data, and the conformance packages |
| `tests/vectors/` | The unit tests' vectors, exported from `tests/oracle/`, and the screens' own (`s15/ui/`) |
| `tests/tools/` | The tools' own tests |
| `tools/` | Build, image, check and device tools ([`tools/README.md`](../blob/main/tools/README.md)) |
