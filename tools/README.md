# Tools

Run them from the repository root. The Python tools need only the standard library: run them as
`python3 -I -B tools/<name>`. Each tool's help is its docstring or header comment; their tests are in
`tests/tools/`.

| Tool | What it does | Used by |
| --- | --- | --- |
| **Build and image** | | |
| `mkldr.py` | Writes the LDR boot stream (front and main image) from the linked ELF files | `make image`, `make cross` |
| `elf32.py` | Reads ELF32 files: sections, symbols, relocations, load addresses | the other image tools |
| `layout.py` | Checks the image's size, placement and layout | `make image`, `make cross` |
| `front_image.py` | The frozen front's checks: pin, include boundary, init, stack | `make cross`, [The Front](https://github.com/bagofwaffles/waffle-boot/wiki/The-Front) |
| `bdf2c.py` | Writes the display font, and the front's copy, from the Spleen BDF file | `check/generated` |
| `png2logo.py` | Turns a PNG into the hand-over frame's image | `make image LOGO=…` |
| `regen.py` | Runs the test data generators under `tests/oracle/`; `--check` fails on a stale file | `make regen`, `conformance.sh` |
| `bump_version.py` | Raises the bootloader's patch version | `make bump-version` |
| `testos_layout.py` | Checks the test OS's placement and stream | `make test-os` |
| `testos_regs.py` | Writes S-13a's OS-entry registers as a C table for the test OS | `make test-os` |
| `installer_image.py` | Writes the image the installer carries, as C, after checking its front against `src/front/front.pin` | `make installer` |
| `toolchain/` | Builds the bfin-elf cross toolchain ([`README.md`](toolchain/README.md)) | `make cross` |
| **Checks** | | |
| `ci-local.sh`, `ci/` | Runs the CI jobs in local Linux containers | [Running the checks](#running-the-checks) |
| `conformance.sh` | The conformance packages' self-tests, the conformance harness and its coverage check | the `conformance` job |
| `repro-check.sh` | Builds twice in different places and compares the outputs | the `repro` job |
| `layering.py` | Checks the includes of the core, board and driver layers and the test OS | `check/layering` |
| `leak-check.py` | Keeps personal or foreign data out of commits, pushes and posts | `check/leak`, the git hooks |
| `leak-check-install.sh` | Installs `leak-check.py`'s git hooks, for the maintainer | by hand |
| **On a unit** | | |
| `wbrecover.py` | Recovery's host tool, for a unit started with Power + M4 | [Recovery Reference](https://github.com/bagofwaffles/waffle-boot/wiki/Recovery-Reference) |
| `testos_mon.py` | The host end of the test OS's monitor and of the developer requests | [Test OS](https://github.com/bagofwaffles/waffle-boot/wiki/Test-OS), [Developer Host Link](https://github.com/bagofwaffles/waffle-boot/wiki/Developer-Host-Link) |
| `mtpcp.py` | Copies a file to the unit's `/yaffs2` volume over MTP and reads it back | [Test OS](https://github.com/bagofwaffles/waffle-boot/wiki/Test-OS) |
| `crc_probe.py` | Probes the CRC peripheral and memory DMA through the developer requests | by hand |
| `pkte_probe.py` | Probes the security packet engine through the developer requests | by hand |
| **Screenshots** | | |
| `ui-shots.sh` | Renders the boot menu's screens into `docs/ui/` | `make ui-update` |
| `ui_shots.c` | Draws each screen with the bootloader's own code into raw frames | `ui-shots.sh` |
| `maps.py` | Draws the README's and the wiki's maps of the SPI flash and the eMMC into `docs/maps/`, each for a light and a dark page | by hand, when the layout changes |
| `ui_shots.py` | Turns the raw frames into PNG files, and into the README's `tour.gif` | `ui-shots.sh` |
| `wiki-publish.sh` | Copies `wiki/` into the GitHub wiki's repository, its screenshots and maps pointed at `docs/ui/` and `docs/maps/`; pushes only with `--push` | by hand |

`docs/web/` is the browser's counterpart of `wbrecover.py`; `tests/web/run.sh` checks it against the
tool (Node 20 or later), and `tests/web/sim_server.py` serves it with a simulated unit.

## Running the checks

`tools/ci-local.sh` runs the checks in Ubuntu 24.04 containers, on a copy of your working tree. It needs
Docker.

    tools/ci-local.sh                  # the light set: host tests, cross build, conformance
    tools/ci-local.sh all              # every job, with gcc and clang
    tools/ci-local.sh host analysis    # chosen jobs

The jobs are `host` (gcc and clang), `analysis` (clang analyzer, cppcheck), `cross` (toolchain, cross
build, image, size and test OS checks), `repro` (every reproducible-build check), `fuzz` (`FUZZ_TIME`
seconds each, default 60) and `conformance`. The toolchain, `cross` and `repro` jobs run on linux/amd64, the
others on the machine's own platform, so on Apple Silicon only those are emulated; `--platform linux/amd64`
runs everything there. The toolchain is built once into a Docker volume, so the first run is slow.
`tools/ci-local.sh --help` lists the options.

On Linux the same checks run directly:

    make check                    # host build and unit tests
    tools/conformance.sh          # generated data, package self-tests, conformance harness
    make analyze cppcheck         # static analysis
    tools/repro-check.sh host     # reproducible build; `cross` once the toolchain is built
    make cross                    # with the toolchain (tools/toolchain/README.md)
