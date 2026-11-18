With **developer mode** on, the boot menu answers requests from a computer on its USB serial port (S-10c.110,
id `0x7D`), sent with `tools/testos_mon.py`. They are part of the standard image. With developer mode off,
every request is refused and changes nothing. `ping`, `rd` and `wr` work as with the test OS's monitor
([Test OS](Test-OS#the-usb-monitor)).

> **Warning.** A hand-over with options can leave a unit that only the other slot brings back. Try
> hand-overs only with a working bootloader in the other slot, and start that slot from the recovery menu
> (Power + M4, then 1, Choose the slot to start; [How to Recover](How-to-Recover#start-the-other-slot-once-and-keep-it)).

## Turning it on

1. Put waffle-boot on the unit ([How to Install](How-to-Install)).
2. Boot menu → **2 Settings → 1 Developer mode: On** (S-15.172).

The serial port is in every USB set while developer mode is on, MTP included (S-10a.010). Turning MTP on or
off re-enumerates the device: reopen the port.

## Files over MTP

The developer menu's **2 MTP file transfer** shares the internal drive read-write (S-10b, level W). A
computer's file manager, or `tools/mtpcp.py` (`ls`, `put`, `verify`, `rm`, `mkdir`, `mv`, `tree`), can copy,
delete, move and rename files and folders and set their dates. Each change is flushed to the eMMC before the
computer is answered. `lost+found` cannot be deleted, moved or renamed; a name that exists is never
replaced, so a copy over an existing file is refused (delete it first, as `mtpcp.py put` does). Nothing
outside the drive can be reached, and the bootloader's own files on it (`os.ldr`, `upgrade/`) are not protected.

## Requests

| Request | What it does |
| --- | --- |
| `ping`, `rd ADDR`, `wr ADDR VALUE` | Is it there; read or write the master's registers |
| `dev-srd ADDR`, `dev-swr ADDR VALUE` | Read or write a slave register over the bridge |
| `dev-l1rd ADDR [WORDS]` | Read L1 instruction memory (`0x11A00000`…) through ENX |
| `dev-flags [MASK]`, `dev-handover [MASK]` | Read or set the hand-over options, then hand over the OS with them ([below](#trying-a-hand-over)) |
| `dev-put FILE [--as NAME]` | Write FILE to `/yaffs2/os.ldr` (the only name allowed) and verify its SHA-256; safe from a power cut once it answers. The developer menu's OS write (5) then uses it |
| `dev-get [NAME]` | The file's SHA-256 and size |
| `dev-yscan OUT.json [--obj ID]...` | Read the yaffs2 volume's tags and the root's headers; read-only, no file contents |
| `yshow OUT.json [--obj ID]...` | A saved scan's summary, offline |
| `dev-cal-start` | Restart into one OS start with the calibration screen where the OS draws its first screen (S-04.012, S-05.016) |
| `dev-screen [--json]` | The screen's name, title, rows and styles, MTP state, USB set and flags such as `armed` |
| `dev-key KEY...` | Press keys: `1`–`8`, `white`, `back`, `help`, `shift`, a key number (`0x12`), or `KEY:down`/`KEY:up`. Keys queued behind a long operation act after it |
| `dev-shot OUT.png` | The frame buffer as a PNG |
| `dev-start` | Why this start's OS start failed ([below](#why-the-os-did-not-start)) |
| `dev-stream` | How this start sent the slave its boot stream ([below](#the-slaves-boot-stream)) |
| `log [S]` | The log for S seconds (default 10), including an install's failed step and cause |
| `nordic-info` | The Nordic's `mode`, `sw_version`, `bl_version`, `boot_slot`; changes nothing |
| `continue-os` | Restart the unit so that its next start boots the OS (S-10c.076), even with Start in boot menu on. If the slave does not answer, the restart is a plain one and Start in boot menu applies. Needs no developer mode; `continue-os --sysex` prints it as a MIDI SysEx message for a unit without a serial port |
| `dev-bench`, `dev-check`, `dev-extract` | Time the firmware package check and extraction ([below](#timing-the-package-check)) |
| `dev-link-record`, `dev-link-test [N]` | The bridge link's record and its bench ([below](#the-bridge-link)) |
| `dev-quad-data` | How this start's OS copy used quad data replies ([below](#quad-data-replies)) |
| `dev-quad-reply [N] [options]` | What ends a reply sent in quad I/O, tried on the unit ([below](#the-quad-reply-test)) |

- A file request while the slave is not answering fails with status 3 (`the slave is not answering`), not
  1 (refused volume): restart the unit.
- `continue-os` refuses during an install or OS write, and before the boot menu is up.
- `dev-key` cannot answer an install's `Confirm Install`, which reads the unit's own keys. Nothing else is
  filtered: `dev-key 5 7:down`, then `dev-key 7:up` after 5 s, opens System and runs a factory reset; what
  erases still waits for its key to be held 5 s.

## Hand-over options

A bitmask. **Zero is the normal hand-over**, as a boot does it (S-05.023). Each bit changes one step of it,
to find which piece an OS needs.

| Bit | When set |
| --- | --- |
| `0x001` | Leave the slave's MSI controller as found |
| `0x002` | Leave every SEC source as found |
| `0x004` | Leave SP and FP as the kernel left them |
| `0x008` | Leave EVT2–EVT15 as they are |
| `0x010` | Leave DMA20/DMA21 running |
| `0x020` | Set `CRC1_CTL` to S-05.031's value |
| `0x040` | Leave the USB controller running |
| `0x080` | Leave the timers running |
| `0x100` | Leave the CPLBs and caches off at the jump |
| `0x200` | The slave's teardown is interrupts off alone |
| `0x400` | Do not release the slave |
| `0x800` | Trace: report each milestone over USB; the last report before the port dies is the hang. Also prints PA6 (slave ready) and PC5 (bridge attention) for 3 s after the slave's OS started |
| `0x1000` | Skip the PMIC, `0x51` and logo redraw before the load |
| `0x2000` | Do not wait for the slave's OS before the teardown; with `0x800`, the line window starts at the release |
| `0x4000` | Unused, ignored |
| `0x8000` | The slave hands PA4 to its disabled SPI2 at its server stop (S-05.031); add `0x2000` |
| `0x10000` | Watch the lines 10 s from the release, then stay in held mode (no teardown, no jump); the bridge is gone, `rd` still works |
| `0x20000` | The slave pulses PA4 at each step; with `0x10000` (`0x30000`) printed as `slave steps: 1 teardown, 2 MSI done, 3 SEC done, 4 entry set, 5 delay over, 6 first load call, 7 kernel returned, 8 before the jump` |
| `0x40000` | SPI0 off with the `OPXY_HO_SPI0_*` values; `SPI2_CTL` is `0x2` in every hand-over (S-05.016) |

## Trying a hand-over

Needs developer mode, a valid plaintext OS in the active partition, and the boot menu (hold com or the
white knob while switching on, or a start whose load failed).

1. `tools/testos_mon.py dev-flags` shows the options (`ping` first confirms the port).
2. `tools/testos_mon.py dev-flags 0x010` sets them.
3. `tools/testos_mon.py dev-handover` hands over with them; `dev-handover FLAGS [MD] [SD]` passes a mask and
   ms delays before the master's and the slave's jump (e.g. `dev-handover 0x30000 0 1000`). The port goes
   away if the OS was entered or the unit hung, and stays if it returned to held mode.
4. If the OS did not come up, start a slot that reaches the boot menu from the recovery menu, set another
   mask and retry. No reinstall is needed.

If the slave's OS does not start within `T_OSSTART`, the hand-over returns to held mode with code 5, the
bridge gone (S-05.032). Nothing survives the recovery reset, so read a trace live.

## Why the OS did not start

`dev-start` (request 37) prints the same account as the main screen and Device Info's `OS start` row
(S-15.163), at any time later in that start:

```
last plain start: The OS could not be started.
  reason:   DSP2 stopped answering during the image read
  OS start: Sector 1281 read: 0x12 reply broke
  load:     a select or read failed; cause: a failed select or read (S-05.007)
  step:     an image read of partition 2, 128 sector(s) from 1281
  header:   format 3, length 742764 bytes, CRC deadbeef; the partition 1024 sectors
  bridge:   the slave was lost on request 0x12 (arg 0x501): the reply broke off [the reply ran out its bound; rc -5, status 0x0, 3 words left]
```

A refused image adds `refusal` (S-05.024), a failed release adds `release`. With no failed start it prints
`the last plain start did not fail, or none was tried`. Layout: `opxy_start_report()` in
`src/include/waffle/board/opxy_services.h`.

## The slave's boot stream

`dev-stream` (request 40) prints how this start sent the slave its boot stream, at any time in that start,
in every build but a byte-by-byte one, `BOOT_LINK_FC=0`, which answers `bad request`
([Test OS](Test-OS#the-slaves-stream-by-the-controllers-flow-control)). It reads only. For example:

```
this start's stream: by the controller under flow control, fed by DMA
  outcome:  streamed, ended at the FINAL block; 120.500 ms from the wait for the ready line
  bridge:   linked
  sent:     412 sessions, 437 parts by DMA, 421376 bytes by the controller, 0 one at a time
  pacing:   guard 200 us, 85.547 ms in the guards; 3 parts began with the line high, 0.030 ms waited, the longest 12 us
  held:     SPI_STAT.FCS set in 2400 of 91000 samples, in 380 parts, the first at byte 16
  fell back: no
```

After a fall-back it names the cause, the session and the byte, and the driver's account of the part that
failed. Layout: `opxy_stream_report()` in `src/include/waffle/board/opxy_boot.h`.

## Timing the package check

| Request | What it times |
| --- | --- |
| `dev-bench [KIB]` | Each check stage over KIB KiB (default 1024), and whether the packet engine and CRC peripheral are used |
| `dev-check` | The whole check (S-11.020) of the package on the USB disk; `checks 1-14 passed` means it agreed |
| `dev-extract` | The check, then extraction (checks 16 to 18) with nothing staged, and the bridge's cost per request; then the link's record |

For `dev-check` and `dev-extract`, copy the package, run `sync`, and **do not eject** (an eject starts the
install). Nothing is written, so the package still installs. All three are refused during a long
operation, a scan or install, or a host-link transfer.

`dev-extract` raises the bridge first as an install does (S-09.075): to single I/O at 50 MHz (rung 2), or
the fastest rung below it that works. It lowers it to plain mode at the end (S-09.078). USB is not serviced during the link tests, usually under a second. If the tool says the
slave is absent, installs are refused until a restart; restart too if it says the slave could not be told
the link is plain.

## The bridge link

| Request | What it does |
| --- | --- |
| `dev-link-record` | The link's record for this start (S-09.070 to S-09.079): the last negotiation per rung, whether the slave is settled (if not, restart), the last 16 fall-backs, the last link test's faults |
| `dev-link-test [N]` | N tests (default 16, 1 to 255) of each length from 12 bytes to 64 KiB on rungs 1 to 4 and plain mode; a table of passes, faults and speed against plain (under 1 is faster) |

An install raises the link no higher than rung 2; the quad I/O rungs 3 and 4 are run only by
`dev-link-test` and `dev-quad-reply`. `dev-link-test` needs no package, writes nothing to the eMMC and sends only echoes, `0x19`, `0x1A` and
`0x1B`. The default run sends about 17 MB in about 6 s, plus about 1 s per fault, bounded at 90 s. It is
refused as the timing requests are, and without the slave. If the slave could not be told the link is
plain at the end, restart. Reply formats: `src/include/waffle/devhost.h`.

## The quad reply test

Replies over the bridge travel in single I/O, but for the sector data of a start's OS copy
([below](#quad-data-replies)). `dev-quad-reply` tries the candidate ends on the unit itself (S-09.080,
S-13b.084): for each, the slave sends test replies in quad I/O, ends each one on that
candidate's flag, and keeps a record of what its SPI controller showed; the master checks every
word of each reply and then fetches the record in single I/O.

    tools/testos_mon.py dev-quad-reply [N] [--cand tf-dma,tf-core,spif] [--ren 0,1]
                                           [--clock 25,50,25f,50f] [--len 12,64,...] [--json]

| Option | Default | Meaning |
| --- | --- | --- |
| `N` | 16 | Test replies per cell, 1 to 4096 |
| `--cand` | all three | `tf-dma`: the transmit-finished flag, fed by DMA; `tf-core`: the same, fed by the core; `spif`: the SPI-finished flag, read by a loop that reads nothing else |
| `--ren` | `0,1` | The receive channel off and on, for the `tf-` candidates |
| `--clock` | all four | The master at 25 or 50 MHz, `f` with its fast mode on |
| `--len` | 12 to 65536 | The reply lengths, in bytes, whole 32-bit words |

Each cell (a candidate, the receive channel, a clock and a length) is one request to the unit, bounded at
120 s, so the default run is 140 cells. The table, per candidate and clock, one line per length:

| Column | Meaning |
| --- | --- |
| `trials` | Test replies sent |
| `whole` | Replies whose every word was right, the last included |
| `flag` | Replies the candidate's flag ended |
| `missing` | Replies whose flag did not come within the bound (30 ms); the reply was given up |
| `last word` | Replies whose last word alone was wrong: a flag that came before the last word had left |
| `a word` | Replies with another word wrong |
| `broke` | Sessions that did not complete; the bridge was brought back and the run went on |
| `delay cycles` | The flag's delay after the last word was fed, least and most, in the slave's cycles (400 a microsecond) |
| `lines` | The data lines (D0 to D3) of every wrong bit: one line alone points at a stuck or open line |
| `master RF` | Whether the master's receive-finished flag was set at the end of every reply |

The first failure in a cell is printed under it with the slave's record: how the reply ended,
`SPI_STAT`, the transmit word count and the DMA status at the end, and the times from the hand-over of
its ready line. A verdict per candidate follows: `ended every reply and every reply was whole`, or where
it failed first. `--json` prints every cell and the verdicts as JSON.

The test writes nothing to the eMMC or the flash and sends nothing but its own requests; a session that
breaks is followed by the bridge's usual way back (echoes and polls). Both processors are back in single
I/O after each reply, and the run ends with the link plain and the slave answering. If the
tool says the slave stopped answering, or did not answer after a cell, restart the unit. It is
refused as `dev-link-test` is. Reply format: `OPXY_DEV_QUAD_REPLY` in `src/include/waffle/devhost.h`.

## Quad data replies

A start copies the OS image with its sector data in quad I/O at 50 MHz when the quad reply test passes
first in that start (S-09.081): eight test replies of 1 KiB and 64 KiB under the transmit-finished flag
fed by DMA, the master at 50 MHz with fast mode, each checked word by word. Then the link is raised to
its usual rung and both processors agree on the mode with a `0x19` exchange; each sector read's reply
is a short head in single I/O followed by its data in quad I/O, checked against the head. Any failure
drops the rest of the start to single I/O replies and reads that sector range again; a copy whose CRC
fails after quad data is read once more in single I/O. After the copy the link is lowered: the
installer, MTP, the disk and the file commands never see quad data.

`dev-quad-data` (request 41) prints how this start's copy went, at any time; it reads only:

```
this start's quad data replies: taken up, ended with the link
  test:     8 of 8 test replies whole with their flag, 41.250 ms
  data:     37 sessions in quad I/O at 50 MHz, 2359296 bytes; 0 failed
  link:     rung 0 now
```

Reply format: `OPXY_DEV_QUAD_DATA` in `src/include/waffle/devhost.h`.

## Wire format

A request is one frame on the USB serial port: route `0x21` (host to master), a sequence byte, the id
`0x7D`, the payload's length, the payload and a CRC-8 over the four header bytes and the payload (reflected
polynomial `0x4D`, each input byte XORed with `0xA5` first), the whole COBS-encoded and ended by a zero
byte. The payload is an opcode byte and its little-endian arguments; the reply's payload starts with a
status byte: 0 ok, 1 refused (developer mode off, or the access refused), 2 malformed, 3 the slave is not
answering. The log shares the port, so skip frames of other ids. `frame()` and `crc8()` in
`tools/testos_mon.py` are the reference; opcodes and reply layouts are in `src/include/waffle/devhost.h`.
