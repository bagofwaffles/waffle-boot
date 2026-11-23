How the installer works. To install, follow [How to Install](How-to-Install).

> **Status:** tested on real hardware, but not widely: use it with caution. A broken bootloader can leave a
> unit that only an external flash programmer brings back.

The installer, `waffle-install.ldr`, runs as the unit's OS. It is built on the [Test OS](Test-OS). It checks the hardware, and only when every check passes does it write the
waffle-boot image it carries. Every write is started on the unit, by holding the step's key (`Hold to select`) until its bar is full.

One image serves both devices (S-05.043): it is laid out like the OP-1F's OS (see
[The image's layout](#the-images-layout)), which the OP-XY's own bootloader takes as well, and it decides
the device at its entry. **That single image has run on neither unit yet**; the runs described below were
made with the earlier images built for one device each.

Back up your files before running it: the OS has to be installed again afterward
([Updating the OS](Updating-the-OS)).

## What it needs on the unit

- A developer menu that writes an OS file to the boot partition the OS is not in.
- An OP-XY, or an OP-1F: see [On an OP-1F](#on-an-op-1f).

Before it touches any pin it decides the device: the boot mode, then the first stage's fingerprint
(S-18.003), read from the SPI flash and never written. It installs only on a first stage the table names.
On a first stage it does not know it goes on as the OP-XY (S-18.004) and writes nothing.

## The checks

It tests the clocks and DDR, the SPI flash, the PSoC, the second processor and the bridge, the eMMC, the
display, the clock and the Nordic chip. Then it asks for one guided step, the key check: press com, then M4 (4 under the display), to continue.
Where it found an OS to go back to, every screen from the key check on offers `8 Return to the old OS (hold to select)`:
it makes that OS active again and installs nothing (`Not installed`, then switch off and on).
A test of a part the device does not have reads `n/a` ("not on this device") and counts as passed.
Reading the screen shows the panel works. USB is shown by the page connecting at all.

The results are on the screen, on the page, and in `check.txt` on the unit's USB disk (no serial number or
key).

## What it writes

- **Before the trial:** nothing. It reads the selector, both slots and the boot partitions' headers, and
  checks the image it carries: its CRC-32, that it is a stream the unit can start, its descriptor, and its
  front against the pinned front (`src/front/front.pin`), and that it is at most `0x76000` bytes (S-02.104).
- **The trial:** the slot the selector does not name, its settings sector and its boot logo's header sector
  erased with its start log (the new image starts on default settings, S-02.103, and the built-in logo), that slot's record with the trial mark (the CRC
  inverted, so the first stage starts it once), byte 0 of its start log `0xFD` (a trial pending), and the
  selector naming that slot unverified. The way back is the old slot, untouched: a power cut at any point
  leaves a selected slot that starts. If the selector step fails, the installer puts the selector back on
  the old slot with programs alone, never an erase. When it cannot do that safely, it leaves the selector as
  it is and refuses every further write until the unit restarts ("Restart before another write").
- **The trial start:** the new slot's front marks the start (`0xFD` to `0xFC`), and its main image marks
  the boot menu reached (`0xFC` to `0xF0`, entry 1 of the log finished, S-02.095). Nothing else.
- **The commit:** only after both marks: the record with the right CRC, read back with the slot's bytes,
  then the selector `{slot, 0x00}`.
  Then the active byte of the two boot partitions' headers, your OS's first.
- **The trial again:** the commit screen offers it too, on 2 held. It is the trial above, written
  the same way with the same checks over the same slot, never the slot the selector names (on the OP-1F
  never the last slot of the unit's own bootloader). So one trial start can go to the recovery menu (Power
  + M4; on the OP-1F Power and the recovery key) and another to the boot menu before you keep it. The
  commit still needs a trial start that reached the boot menu.
- **Never:** the first stage, the metadata, or the slot the selector names.

A breadcrumb build (`BOOT_CRUMBS=1`) runs only as a trial: the commit is refused, and `check.txt` lists its
marks, which `tools/testos_mon.py crumbs` decodes.

## On an OP-1F

> **Status:** initial support, from 0.9.48 (S-18.050). On one OP-1F the survey and the installer have run,
> each put on as below, with the trial and the commit, then recovery and a start to the OS. Save the
> survey's `check.txt` if you run it.

There it is written as the OS by the unit's own means (the page does not send it), and so is the survey:

    make installer-survey         # build/waffle-install-survey.ldr
    make installer                # build/waffle-install.ldr

(`make installer-1f` and `make installer-1f-survey` build the same two files.) Both carry the same
waffle-boot image.

On the OP-1F the checks have no second processor and no bridge (`n/a`), reach the eMMC on the main CPU,
ask the PSoC for its keys and battery without the ping, and the key check asks for the com key, then the
recovery key, by their roles; the screen shows the number of each key pressed.

**The survey** runs on either device; it writes nothing to the SPI flash: its write engine refuses every step, its write gate refuses
every erase and program, and its link sends the flash driver's erase and program to functions that refuse, and its build fails when the
linked image still holds the driver's own erase or program.
It makes the OS in the other boot partition active again as soon as it has looked. It shows, and saves in
`check.txt` and over USB (type `0x44`), what an install would depend on: the first stage's `n` and `c`, the
boot mode, the selector's two bytes, each slot's record and whether it verifies and is a waffle-boot stream,
the first bytes of each slot's start log, EXT_CSD[179] (PART_CONFIG), SEC_COUNT and the boot partition's
size, and PA2's level where the OP-1F's own pins make it an input. Its last line says whether an install
would be offered.

**The installer** writes as on the OP-XY (above), with one rule more: it never writes over the last slot
whose image is not a waffle-boot stream, so the bootloader the unit came with stays in one slot. A trial
that would is not offered ("Would overwrite the unit's own bootloader").

### Putting the image on an OP-1F

Either file goes on as `/yaffs2/os.ldr`, and the unit's own OS write in its developer menu writes it.
Before that write, `tools/mtpcp.py prepare-1f` arranges the volume so that the write takes the file
(S-05.042). Start the unit in drive mode: hold **com** while powering on, then press 3, then 2. Keep the
computer on the cable from then until the write.

    python3 tools/mtpcp.py prepare-1f build/waffle-install-survey.ldr --dry-run
    python3 tools/mtpcp.py prepare-1f build/waffle-install-survey.ldr

The tool:

- reads the bootloader build from the unit and stops, changing nothing, on any build but 1000.1.1 and
  1000.2.0;
- lists the whole volume and works out the plan: the files directly in `logs/`, any earlier
  `waffle-pad*.bin` at the top and the old `os.ldr` go, the image goes on as `os.ldr`, and one pad file,
  `waffle-pad.bin`, goes at the top when the volume needs it. The pad's size keeps the counts far from
  their next step. `--dry-run` prints the plan and stops there;
- copies every file it will remove to `~/waffle-op1f-backup/<time>/` (or `--backup DIR`), and checks each
  copy by SHA-256 before anything is removed;
- makes the changes, lists the volume again, and goes on only if every row matches the plan. Then it reads
  `os.ldr` and the pad back;
- prints **READY TO WRITE** with the margins. Then, in the same start, copy nothing more to the disk,
  leave drive mode for the developer menu and run its OS write. Any mismatch stops the tool with a line
  that says so; do not run the write then.

The arrangement is checked against two writes that landed and one that was refused, all with bootloader
1000.2.0. Treat it as provisional: the unit's log grows while it runs, so go from READY TO WRITE to the
write without a pause. A refused write leaves the OS that was active, and that OS still starts.

## The image's layout

The stream is shaped like the OP-1F's OS 1.7.3 (S-05.040, S-05.041): one stream, the flags FIRST, IGNORE
and FINAL, entry `0x11A00000` (FIRST and FINAL), FINAL argument 0, one L1 code block at `0x11A00000` that
jumps to the rest, and the DDR image from `0x8D000000` in blocks of at most `0xACAB8` bytes. Its image and
heap fill `0x8D000000` to `0x8E000000`, inside that OS's second DDR range, clear of the frame rows, the
staging area, the handover reserve and the OP-1F's RAM disk. On the OP-XY the installer's RAM disk keeps
clear of that range: it is in three parts, `0x80000000` to the handover reserve, `0x89000000` to
`0x8D000000`, and `0x8E000000` to `0x8FDC3000`, together the usual disk's size. waffle-boot's own OS
start and the developer menu's OS write accept this stream (S-05.024), so it also loads on a unit that
already runs waffle-boot. `tools/testos_layout.py --device both` checks the layout for both devices at
every build.

## Sending it from the page

On a unit already on waffle-boot, the file can go onto the drive as `/yaffs2/os.ldr` by the boot menu's host
link once the hold on the unit approves the computer (`tools/hostlink.py put`, or `tools/testos_mon.py
dev-put`; [Developer Host Link](Developer-Host-Link)), which checks its size and SHA-256 before it takes the
name. Nothing is written to a boot partition until 5 is pressed in the developer menu.

## Building it

    make bump-version
    make installer

`build/waffle-install.ldr` carries this build's `build/bf703/waffle-boot.ldr`. The build fails unless that
image is a bootloader stream whose descriptor carries the pinned front.

## The installer's USB protocol

The installer offers its RAM disk and one USB serial port (vendor `0x2367`, product `0x0021`). On the port it
answers requests and changes nothing: there is no request that writes.

Every frame is recovery's (S-02.093): `57 52` ("WR"), a type (u8), a sequence number (u8), the payload's
length (u16), the payload, and the CRC-32 of S-02.015 over the type to the payload's end (u32), all
little-endian; a payload is at most 1032 bytes. A reply has the request's type with bit 7 set and its
sequence number. Bytes that make no whole frame with a good CRC are dropped up to the next `57 52`. No
request is read while a reply waits. The page already has this codec, and the types below never meet
recovery's (`0x01` to `0x07`), which the installer answers with the payload `FF`.

While it writes, and in some long checks, the installer does not service the port for up to about 30 s.

| Type | Request | Reply |
| --- | --- | --- |
| `0x40` HELLO | none | 64 bytes: `WBIN`, protocol version (1), phase, flags, the number of tests, the carried image's version (4 × u16), length (u32), CRC-32 (u32), front pin (32 bytes), then 56 the unit's device (0 OP-XY, 1 OP-1F), 57 how it was decided (0 the first stage's table, 1 the second CPU, 2 not identified), 58 the device the image installs for (one image serves both: the unit's device, or 0 when it is not identified), 59 1 for a survey build, and 4 zeros |
| `0x41` STATE | none | 32 bytes, then each test's state (u8): 0 phase, 1 flags, 2 verdict (0 pass, 1 pass with gaps, 2 fail, `FF` not yet), 3 the test under way (`FF` none), 4 the lock, 5 and 6 the selector, 7 and 8 each slot's state, 9 the target slot (`FF` none), 10 the last write's code (`FF` none), 11 the OS partition, 12 the hold's progress (u16, 0 to 1000), 14 the key held (digit), 15 the trial, 16 the key of the next step (1, or 0), 17 bit k set when slot k holds a waffle-boot stream, the rest zeros |
| `0x42` TEST | the test's index (u8) | status (0, or 1 for no such test), index, state, kind (0 OS, 1 automatic, 2 guided), then four NUL-ended strings: its id, its name, the detail line (at most 56 characters) and the report's note (at most 160) |
| `0x43` REPORT | an offset (u32) | status (0), `check.txt`'s length (u32), then up to 1024 of its bytes from the offset. The text is rewritten as the checks run, so a reader starts again when the length changes |
| `0x44` SURVEY | none | a survey build, once its checks end: 76 bytes, `src/include/waffle/board/installer.h` lists them; any other build answers `FF` |

- **Phase:** 0 starting, 1 checking, 2 the key check (8 held returns to the old OS), 3 locked, 4 ready (the trial offered), 5 writing the
  trial, 6 the trial written, 7 the commit offered (hold 1, or 2 for the trial again), 8 committing, 9 done, 10 a write failed, 11 the trial
  failed, 12 returned to the old OS without installing, 13 the survey's end.
- **Flags:** bit 0 the checks done, bit 1 the install offered, bit 2 the carried image passed its check, bit 3
  a breadcrumb build, bit 4 a survey build.
- **Lock:** 0 none, 1 a check failed, 2 a check could not run, 3 the carried image, 4 the selected slot not
  verified or not good, 5 and 6 not used, 7 the eMMC not reachable, 8 the device not
  identified by its first stage, 9 an image built for the other device (not set by the one image), 10 the trial would write over the
  last slot that is not waffle-boot's (OP-1F), 11 a survey build.
- **Test state:** 0 not run, 1 pass, 2 fail, 3 not tested, 5 not on this device.
- **Slot state:** bit 0 good, 1 this image under the trial mark, 2 this image committed, 3 trial pending, 4
  trial started, 5 trial spent, 6 trial entry odd, 7 the trial reached the boot menu.
- **OS partition:** 0 not looked at, 1 found and checked, 2 found, stored encrypted, 3 not found, 4 fails its
  check, 5 not read, 6 active again, 7 could not be made active again. Hold 8 (return to the old OS) is offered while it is 1 or 2.
- **Trial:** 0 none, 1 written, 2 reached the boot menu, 3 started without reaching it, 4 used up without a
  start of the new bootloader.
- **Write codes:** S-02's (2 I/O, 3 size, 4 verify, 5 start, 6 not good, 7 not a bootable stream), `40` a
  breadcrumb build's commit, `41` the trial not started, `43` the carried image, `44` the boot menu not
  reached, `45` a failed selector step left as it was, `46` the last slot that is not waffle-boot's (OP-1F),
  `47` a survey build.

`src/testos/installer_logic.c` is the reference, `tests/unit/test_board_installer.c` holds frames byte for
byte, and `docs/web/install.js` reads them.
