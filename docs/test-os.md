# The test OS

The test OS is waffle-boot's boot menu (held mode) built as an OS image, so a bootloader already on the
unit loads it in place of the OS (S-05). It runs waffle-boot's drivers, menus, bridge, USB and display on
both processors, never hands over to an OS, and refuses flash erase, flash program and OS writes. Its
installer variant is how waffle-boot gets onto a unit today.

> **This can leave a unit unable to start.** There is no supported install. Try nothing here on a unit you
> are not prepared to lose.

| Build | Output in `build/bf703-testos/` | What it is |
| --- | --- | --- |
| `make test-os` | `waffle-test-smc.ldr`, `waffle-test-gpio.ldr` | The test OS, with the normal or a hand-driven panel driver (`TESTOS_PANEL=smc` or `gpio` builds one) |
| `make test-os-mon` | `waffle-test-mon.ldr` | The `smc` image with the USB monitor |
| `make test-os-install` | `waffle-test-install.ldr` | The monitor with this build's `waffle-boot.ldr` inside |

Every build fails unless the stream passes the boot-stream checks and `tools/testos_layout.py`, which caps
it at `0x800000` bytes, the most a unit loads. The test OS's code is in `src/testos/` and behind `OPXY_TEST_OS`
(`src/include/waffle/testos.h`); the normal image is untouched.

## Onto a device

The bootloader on the unit must offer a write of `os.ldr` from its yaffs2 volume to the inactive boot
partition, as waffle-boot's developer menu does (S-15.092 to S-15.094; S-15.170 says how to reach it).

1. Turn MTP on in that menu, quit any other MTP client, and copy the stream (needs libusb):

       python3 tools/mtpcp.py put build/bf703-testos/waffle-test-gpio.ldr
       python3 tools/mtpcp.py verify build/bf703-testos/waffle-test-gpio.ldr

   Go on only on status 0 and `identical`; on `FAILED`, stop.
2. Run the menu's OS write, then switch off and on. Judge it by what starts: the test OS's first log line
   names its variant.
3. The OS write removes the unit's OS from the other partition. To go back, write TE's firmware file the
   same way. Installing waffle-boot needs an OS write afterwards too.

## What it reports

- Log lines tagged `testos`, also written to `testos.txt` on the USB disk when it is first formatted.
- `flash.bin` beside it: a copy of the whole SPI flash, the unit's metadata included. Never share it.
- Register fields that differ from S-13a's values at OS entry.
- `te` and `frames on te` lines: the panel's TE period and four frame timings (15.05 ms expected;
  S-13c.018, HW-FRAME).

### The color trail

The panel changes color at each start-up milestone, so a unit that shows nothing else shows how far it got.

## The USB monitor

`tools/testos_mon.py` (Python 3, standard library) drives `waffle-test-mon.ldr`; with no command it lists
them all.

| Command | What it does |
| --- | --- |
| `ping`, `report` | Is it there; `testos.txt` so far |
| `rd ADDR [WORDS]`, `wr ADDR VALUE` | The master's registers |
| `srd ADDR [WORDS]`, `swr ADDR VALUE` | The slave's, over the bridge |
| `init`, `echo [ARG]`, `pull [LEN]`, `req CMD ARG0 ARG1`, `send` | One raw bridge request; what the slave's last reply send saw |
| `table`, `emmc`, `beacon [US]` | The slave's bring-up table, its eMMC bring-up, its attention line |
| `load ADDR FILE`, `call ADDR [ARG]` | Upload a routine to `0x89F00000` and run it, `uint32_t f(uint32_t)` |
| `run ADDR [ARG] [MS]`, `last [clear]`, `stall [MS]` | Run under the watchdog (default 2000 ms), how the last one ended, a watchdog test |
| `q11 WHICH [N] [--elf PATH] [--json FILE]` | S-13b's Q-11 measurements, `1a` to `1h` or `all`; needs `mon` on both processors |
| `encode OP HEX` | A frame's wire bytes |

> The monitor refuses the OTP and flash-controller writes, but an uploaded routine is not limited: run only
> code you have read. Do not reset the master by hand while the slave is up (S-04.032); restart through the
> PSoC's request (S-04.065).

## The installer

1. Run `make bump-version`, then `make test-os-install`, and load the installer as in
   [Onto a device](#onto-a-device).
2. `python3 -I -B tools/testos_mon.py install status`: the selector, each slot, and each tried slot's
   start-log entry. Writes nothing.
3. `python3 -I -B tools/testos_mon.py install trial yes`: writes the new image for one trial start.
4. Restart with no key held: the new image starts once and marks its start.
5. Back in the installer, `python3 -I -B tools/testos_mon.py install commit yes`.

| Form | Writes | Next start |
| --- | --- | --- |
| `trial` | The unselected slot, a record that fails on purpose, start-log byte 0 = `FD` (pending), and the selector unverified: `{00, FE}` for slot index 0, `{FF, FF}` for index 1 | That slot once (S-02.022, S-02.023); its front marks `FC` (started). The next reset starts the old slot |
| `commit` | The correct record and the selector `{k, 00}`; needs `FC` | The new slot is trusted. The way back is the recovery menu (Power + M4, then 1) or a package |

- The installer cannot reach the first stage, the metadata or the selected slot, and checks the image's
  CRC before the first erase.
- `commit` refuses an entry that is pending, spent (the old slot used it up) or odd; those need a new trial.
- `trial` takes an image the selected slot already holds: from front version 14 the two slots may hold the
  same image. An older front makes no mark.
- With slot index 1, a cut during the selector erase, or the selector write `{00, 01}` the first stage
  makes after its check fails (S-02.022), can trust the new slot early; index 0 has no such window.
- A trial proves start-up, the display and the boot menu, not the bridge or the OS hand-over.

**Breadcrumbs.** `make test-os-install BOOT_CRUMBS=1 BUILD=build/crumbs` makes the bootloader leave a byte per
milestone in the trial slot's last sector (from byte 1), which `install status` prints. `commit` refuses a
breadcrumb build. `install status` also sums up the selected slot's start log.

## Timing a boot

    make test-os-install BOOT_TIMING=1 BUILD=build/timing

The bootloader stamps the cycle counter at each milestone (`src/include/waffle/boot_time.h`), at 400 MHz
from INIT (earlier counts raw), in a record at `0x8FFFFC00`.

- After a hand-over to this installer, `testos_mon.py report` prints a `time` line per milestone, then the
  PSoC greeting, battery and charge waits, the OS copy's rate and the slave stream's pacing. The record is
  cleared once read.
- After a charge wait (on USB with the switch off), lines also show `since on`, from the `panel light sent`
  line.
- In the boot menu, `testos_mon.py log` prints the same report. A trial start has no serial port.

## The slave's ready line after a byte

Each byte of the slave's boot stream waits for PA6 low (S-13b.022), after a settle time
`BOOT_LINK_SETTLE_US` (default 2 µs, 0 for none; each µs costs about 0.4 s of a start). To measure PA6 over
a window after each byte:

    make test-os-install BOOT_TIMING=1 BOOT_TIMING_WINDOW_US=5 BUILD=build/timing

The report adds when PA6 rose after each byte, in eight steps of the window. Rises only in the first step,
or none, mean the settle time can go. It also counts the slot reads at 50 MHz that were retried; one that
then read good means 50 MHz is marginal on that unit.

## The slave's stream by the controller's flow control

`BOOT_LINK_FC=1` sends the slave's boot stream through SPI0's FIFO, paced by the controller's flow control
on PA6. It is off by default and in every release; try it only as a trial:

    make test-os-install BOOT_LINK_FC=1 BOOT_TIMING=1 BUILD=build/linkfc
