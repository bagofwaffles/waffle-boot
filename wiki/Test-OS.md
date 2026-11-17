The test OS is waffle-boot's boot menu (held mode) built as an OS image, so a bootloader already on the
unit loads it in place of the OS (S-05). It runs waffle-boot's drivers, menus, bridge, USB and display on
both processors with nothing written to the SPI flash: it never hands over to an OS, and refuses flash
erase, flash program and OS writes. The installer ([Installer Reference](Installer-Reference)) is built on
it.

> **Warning.** This can leave a unit unable to start. Try nothing here on a unit you are not prepared to
> lose. Loading a test OS replaces the unit's OS; keep TE's firmware file at hand to write it back.

## Variants

| Build | Output in `build/bf703-testos/` | What it is |
| --- | --- | --- |
| `make test-os` | `waffle-test-smc.ldr`, `waffle-test-gpio.ldr` | The test OS, with the normal or a hand-driven panel driver (`TESTOS_PANEL=smc` or `gpio` builds one) |
| `make test-os-mon` | `waffle-test-mon.ldr` | The `smc` image with the USB monitor, for `tools/testos_mon.py` |
| `make installer` | `waffle-test-installer.ldr`, copied to `build/waffle-install.ldr` | The hardware checks, then this build's `waffle-boot.ldr` written to the unit ([How to Install](How-to-Install)); no monitor |

Every build fails unless the stream passes the boot-stream checks and `tools/testos_layout.py`, which caps
it at `0x800000` bytes, the most a unit loads. The test OS's code is in `src/testos/` and behind
`OPXY_TEST_OS` (`src/include/waffle/testos.h`); the normal image is untouched.

## Loading it

The bootloader on the unit must offer a write of `os.ldr` from its yaffs2 volume (the internal drive) to the
inactive boot partition, as waffle-boot's developer menu does (**5 Write /os.ldr to boot partition**;
S-15.092 to S-15.094; S-15.170 says how to reach it).

1. Turn MTP on in that menu (developer menu, 2), quit any other MTP client, and copy the stream as
   `os.ldr` (needs libusb):

       python3 tools/mtpcp.py put build/bf703-testos/waffle-test-gpio.ldr
       python3 tools/mtpcp.py verify build/bf703-testos/waffle-test-gpio.ldr

   Go on only on status 0 and `identical`; on `FAILED`, stop.
2. Run the menu's OS write, then switch off and on. Judge it by what starts: the test OS's first log line
   names its variant.
3. The OS write removes the unit's OS from the other partition. To go back, write TE's firmware file the
   same way. The installer is the exception: it makes the unit's OS active again when it ends.

## What it reports

- Log lines tagged `testos`, also written to `testos.txt` on the USB disk when it is first formatted.
- `flash.bin` beside it: a copy of the whole SPI flash, the unit's metadata included. Never share it.
- Register fields that differ from S-13a's values at OS entry.
- `te` and `frames on te` lines: the panel's TE period and four frame timings (15.05 ms expected;
  S-13c.018, HW-FRAME).

### The color trail

The panel changes color at each start-up milestone, so a unit that shows nothing else shows how far it got.
The monitor and the installer leave the trail out and paint only a fatal stop (magenta).

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

## Timing a boot

    make installer test-os-mon BOOT_TIMING=1 BUILD=build/timing

Install the timing build with its installer, then write `waffle-test-mon.ldr` as the OS from waffle-boot's
developer menu. The bootloader stamps the cycle counter at each milestone
(`src/include/waffle/boot_time.h`), at 400 MHz from INIT (earlier counts raw), in a record at `0x8FFFFC00`.

- After a hand-over to the monitor's test OS, `testos_mon.py report` prints a `time` line per milestone,
  then the PSoC greeting, battery and charge waits, the OS copy's rate and the slave stream's pacing. The
  record is cleared once read.
- After a charge wait (on USB with the switch off), lines also show `since on`, from the
  `panel light sent` line.
- The slave stream's own steps sit among the milestones, numbered 30 to 34: `stream ready wait` and
  `stream ready` (S-09.003's wait), `stream opened`, `stream first header`, `stream last byte`.
- `stream split` (two lines) divides the stream's time from that wait to its end: the wait, the byte
  transfers, the ready line's windows (with `BOOT_LINK_FC`, the sends), the ready samples, the waits on a
  high line (or the sends' time beyond the wire), the chip select, and the rest, the loop's own.
  `stream blocks` counts blocks, fill or header-only blocks, sessions and bytes; `stream bytes` gives a
  transfer's average, shortest and longest, less a counter read, and the bytes' time on the wire alone
  (`stream sends` with `BOOT_LINK_FC`); `stream longest waits` lists the eight longest, each with the
  bytes sent before it and its block; `stream measuring` gives the counter reads the split adds and their
  cost.
- In the boot menu, `testos_mon.py log` prints the same report. A trial start has no serial port.

## The slave's ready line after a byte

Each byte of the slave's boot stream waits for PA6 low (S-13b.022), after a settle time
`BOOT_LINK_SETTLE_US` (default 2 µs, 0 for none; each µs costs about 0.4 s of a start). To measure PA6 over
a window after each byte:

    make installer test-os-mon BOOT_TIMING=1 BOOT_TIMING_WINDOW_US=5 BUILD=build/timing

The report adds when PA6 rose after each byte, in eight steps of the window. Rises only in the first step,
or none, mean the settle time can go. It also counts the slot reads at 50 MHz that were retried; one that
then read good means 50 MHz is marginal on that unit.

## The slave's stream by the controller's flow control

`BOOT_LINK_FC=1` sends the slave's boot stream through SPI0's FIFO, paced by the controller's flow control
on PA6. It is off by default and in every release; try it only as a trial, and leave the installer with 8
at the commit, so that the next start is the old bootloader's:

    make installer test-os-mon BOOT_LINK_FC=1 BOOT_TIMING=1 BUILD=build/linkfc

`BOOT_LINK_FC=2` does the same with DMA4 feeding SPI0, so the core is off the stream (S-09.009,
S-13b.023):

    make installer test-os-mon BOOT_LINK_FC=2 BOOT_TIMING=1 BUILD=build/linkdma

- Each session goes in parts, a block's header alone and then its payload, each part once PA6 reads low
  and at least `BOOT_LINK_GUARD_US` (default 20 µs) after the last part ended. Before a session, a piece of
  up to 1,024 bytes or a new block, that time passes with the chip select high.
- A part the controller gives up with PA6 low (a DMA error, a stall, no end of the transmit) sends the rest
  of the stream byte by byte, the default way, from the first byte not sent, in the same start, once. A part
  the slave holds for T_STREAM ends the stream, as a held byte does.
- The slave checks its L1 code before it answers the bridge or touches the eMMC (S-09.019): a damaged stream
  ends with the slave not answering, not running. `BOOT_L1_CHECK=1` adds the same check to a build that sends
  the stream byte by byte, and `BOOT_L1_CHECK=0` leaves it out of a `BOOT_LINK_FC` build.
- The master cannot restart the slave, so a stream it sent whole but the slave did not take whole still ends
  with the slave absent for that start; the next start tries again.

The boot log has a line with the way, the sessions, bytes and time, any fall-back and whether the bridge
linked; `testos_mon.py dev-stream` prints the same at any time in that start
([Developer Host Link](Developer-Host-Link#the-slaves-boot-stream)). With `BOOT_TIMING=1`, `stream by DMA`
gives the parts, the guards' time, the parts that began with PA6 high and how often `SPI0_STAT.FCS` read set
while the parts ran (the slave holding the controller), and `stream fall-back` whether and where it fell back.
