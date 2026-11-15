# Developer requests

With developer mode on (Settings, `1 Developer mode`; `developer_mode=on`, S-15.172), the boot menu answers
requests from a computer on the USB serial port (S-10c.110, id `0x7D`), sent with `tools/testos_mon.py`.
With it off, every request is refused and changes nothing. `ping`, `rd` and `wr` work as with the test OS's
monitor ([`test-os.md`](test-os.md#the-usb-monitor)).

The serial port is in every USB set while developer mode is on, MTP included (S-10a.010). Turning MTP on or
off re-enumerates the device: reopen the port.

> A hand-over can leave a unit that only the other slot brings back. Try hand-overs only with a working
> bootloader in the other slot; start it from the recovery menu (Power + M4, then 1, Choose the slot to
> start; [`recovery.md`](recovery.md)).

## Hand-over options

A bitmask. **Zero is the normal hand-over**, as a boot does it (S-05.023). Each bit changes one step of it.

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

## Requests

| Request | What it does |
| --- | --- |
| `dev-srd ADDR`, `dev-swr ADDR VALUE` | Read or write a slave register over the bridge |
| `dev-l1rd ADDR [WORDS]` | Read L1 instruction memory (`0x11A00000`…) through ENX |
| `dev-put FILE [--as NAME]` | Write FILE to `/yaffs2/os.ldr` (the only name allowed) and verify its SHA-256; safe from a power cut once it answers. The developer menu's OS write (5) then uses it |
| `dev-get [NAME]` | The file's SHA-256 and size |
| `dev-yscan OUT.json [--obj ID]...` | Read the yaffs2 volume's tags and the root's headers; read-only, no file contents |
| `yshow OUT.json [--obj ID]...` | A saved scan's summary, offline |
| `dev-cal-start` | Restart into one OS start with the calibration screen where the OS draws its first screen (S-04.012, S-05.016) |
| `dev-screen [--json]` | The screen's name, title, rows and styles, MTP state, USB set and flags such as `armed` |
| `dev-key KEY...` | Press keys: `1`–`8`, `white`, `back`, `help`, `shift`, a key number (`0x12`), or `KEY:down`/`KEY:up`. Keys queued behind a long operation act after it |
| `dev-shot OUT.png` | The frame buffer as a PNG |
| `dev-start` | Why this start's OS start failed (below) |
| `log [S]` | The log for S seconds (default 10), including an install's failed step and cause |
| `nordic-info` | The Nordic's `mode`, `sw_version`, `bl_version`, `boot_slot`; changes nothing |
| `continue-os` | Restart the unit so that its next start boots the OS (S-10c.076), even with Start in boot menu on. If the second processor does not answer, the restart is a plain one and Start in boot menu applies. Needs no developer mode; `continue-os --sysex` prints it as a MIDI SysEx message for a unit without a serial port |

- A file request while the slave is not answering fails with status 3 (`the slave is not answering`), not
  1 (refused volume): restart the unit.
- `continue-os` refuses during an install or OS write, and before the boot menu is up.
- `dev-key` cannot answer an install's `Confirm Install`, which reads the unit's own keys. Nothing else is
  filtered: `dev-key 5 7:down`, then `dev-key 7:up` after 5 s, holds and runs a factory reset.

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

## Timing the package check

| Request | What it times |
| --- | --- |
| `dev-bench [KIB]` | Each check stage over KIB KiB (default 1024), and whether the packet engine and CRC peripheral are used |
| `dev-check` | The whole check (S-11.020) of the package on the USB disk; `checks 1-14 passed` means it agreed |
| `dev-extract` | The check, then extraction (checks 16 to 18) with nothing staged, and the bridge's cost per request; then the link's record |

For `dev-check` and `dev-extract`, copy the package, run `sync`, and **do not eject** (an eject starts the
install). Nothing is written, so the package still installs. All three are refused during a long
operation, a scan or install, or a host-link transfer.

`dev-extract` raises the bridge to its fastest working rung first (S-09.075) and lowers it to plain mode at
the end (S-09.078). USB is not serviced during the link tests, usually under a second. If the tool says the
slave is absent, installs are refused until a restart; restart too if it says the slave could not be told
the link is plain.

## The bridge link

| Request | What it does |
| --- | --- |
| `dev-link-record` | The link's record for this start (S-09.070 to S-09.079): the last negotiation per rung, whether the slave is settled (if not, restart), the last 16 fall-backs, the last link test's faults |
| `dev-link-test [N]` | N tests (default 16, 1 to 255) of each length from 12 bytes to 64 KiB on rungs 1 to 4 and plain mode; a table of passes, faults and speed against plain (under 1 is faster) |

`dev-link-test` needs no package, writes nothing to the eMMC and sends only echoes, `0x19`, `0x1A` and
`0x1B`. The default run sends about 17 MB in about 6 s, plus about 1 s per fault, bounded at 90 s. It is
refused as the timing requests are, and without the slave. If the slave could not be told the link is
plain at the end, restart. Reply formats: `src/include/waffle/devhost.h`.

## Wire format

A request is one frame on the USB serial port: route `0x21` (host to master), a sequence byte, the id
`0x7D`, the payload's length, the payload and a CRC-8 over the four header bytes and the payload (reflected
polynomial `0x4D`, each input byte XORed with `0xA5` first), the whole COBS-encoded and ended by a zero
byte. The payload is an opcode byte and its little-endian arguments; the reply's payload starts with a
status byte: 0 ok, 1 refused (developer mode off, or the access refused), 2 malformed, 3 the slave is not
answering. The log shares the port, so skip frames of other ids. `frame()` and `crc8()` in
`tools/testos_mon.py` are the reference; opcodes and reply layouts are in `src/include/waffle/devhost.h`.

## Getting waffle-boot on for this

Install it as any OS ([`test-os.md`](test-os.md#the-installer)): build the installer, write it as the OS,
`install trial yes`, let it return, `install commit yes`. With developer mode on, the requests answer in
the boot menu.
