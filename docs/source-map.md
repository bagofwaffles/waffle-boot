# Source map

Where the code for each requirement label (`S-NN.NNN`) lives. A `src/sNN_*.c` file holds the logic of one
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

## Layers

| Path | What it holds |
| --- | --- |
| `src/*.c`, `src/include/waffle/*.h` | Hardware-independent logic behind operation structs. It includes no board or driver header, and no driver includes its headers; `tools/layering.py` checks this in `make check` and lists the crossings that remain |
| `src/wire_*.c` | Adapters from those structs to the vendored libraries (crypto, zip, filesystems, CRCs) |
| `src/board/`, `src/include/waffle/board/` | The OP-XY: wiring the logic to the drivers, held mode, the test OS's hooks |
| `src/drivers/bf70x/`, `src/include/waffle/drivers/` | ADSP-BF70x drivers: start-up, CPLBs, interrupts, timers, clocks and DDR, pins, DMA, SPI and serial flash, UART, eMMC, OTP; the BF703 linker script |
| `src/front/` | The frozen front ([`front.md`](front.md)) |
| `src/testos/` | The test OS's board files, linker script, and the SPI driver's test-OS parts, which `spi.c` includes in that build only, and the installer ([`test-os.md`](test-os.md), [`install.md`](install.md)) |
| `src/glue/`, `src/third_party/` | Our glue, and the vendored picolibc, FatFs, yaffs2, zlib and minizip, BearSSL, micro-ecc, Spleen and TinyUSB ([`README.md`](../src/third_party/README.md)) |
| `tests/unit/` | Host unit tests, driver tests against a register-access fake, the libraries tested end to end |
| `tests/conformance/` | The conformance tests: drive the code with the reference models of `tests/oracle/`; `coverage.py` writes an untracked `COVERAGE.md` listing every scenario in `scenarios.tsv` and how it is covered |
| `tests/fuzz/` | libFuzzer targets for the parsers |
| `tests/oracle/` | Test data and reference models, by label number, the generators that write the data, and the conformance packages |
| `tests/vectors/` | The unit tests' vectors, exported from `tests/oracle/`, and the screens' own (`s15/ui/`) |
| `tests/tools/` | The tools' own tests |
