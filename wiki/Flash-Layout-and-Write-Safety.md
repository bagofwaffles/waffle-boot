The SPI flash holds 1 MiB in 256 sectors of 4 KiB. Labels: S-02. The full write-by-write reference is
[`docs/flash-writes.md`](../blob/main/docs/flash-writes.md).

## Areas

| Area | Offset | Holds |
| --- | --- | --- |
| First stage | `0x00000` | The factory first stage, which picks a slot on every start. Not ours; waffle-boot writes it only in a protected install |
| Mirror | `0x0B000` | A copy of the metadata record |
| Metadata | `0x0C000` | Serial, versions and install state |
| Selector | `0x0D000` | The slot to start, and `verified`: `0x01` means "start it without a check" |
| Record 0, record 1 | `0x0E000`, `0x0F000` | One per slot: the image's CRC-32, its length and a tag |
| Slot 1 | `0x10000` | A bootloader image, at most `0x77000` bytes; its last sector (`0x87000`) is the start log |
| Slot 2 | `0x88000` | The same; start log at `0xFF000`. The sector at `0xFE000` holds the boot store while the image does not reach it |

The code numbers the slots 0 and 1; screens show them as Slot 1 and Slot 2. The boot store holds
**Start in boot menu**. An image over `0x76000` bytes in Slot 2 covers it, and the setting reads as off;
`make cross` never builds one that large.

## What keeps a unit bootable

- **Two slots.** A write goes to the slot the selector does not name. The running slot is untouched.
- **A fixed order.** Image, start-log sector, record, a read-back of the whole slot, and only then the
  selector. A power cut before the selector write leaves the unit as it was.
- **The first stage's check.** A new slot is selected untrusted. If its check fails, the other slot is
  trusted again.
- **The front's own check.** Before it starts the main image, the front checks it. A damaged one opens
  recovery.
- **The start log.** Three starts in a row that do not finish open recovery with no key.
- **Trial starts.** A new front runs once before anything trusts it ([Boot Chain](Boot-Chain)).
- **Checks before erasing.** Recovery checks an image's size, its boot-stream rules, its SHA-256 and its
  front before it erases anything, and reads it back before it touches the selector.
- **The countdown.** A write or action from a computer waits 15 s first. Switching off in that time stops it
  with nothing written.

## What is not covered

These leave a unit that only an **external SPI flash programmer** brings back:

- A power cut during an **in-place write** of the running slot while the other slot is bad. Recovery
  refuses it until a plain write has failed, asks for a typed phrase, and holds the power-off, but the hold
  may not close the window.
- A **front that fails before it reads the keys**, in a slot that is trusted. Recovery lives in the front.
- A power cut while the **first stage** is written (protected installs only).
- Two short windows in a **trial to Slot 2**: a cut during the selector erase at the end of the trial write,
  or in the first instant of the trial start. They matter only if that new front also fails.

A forced switch to a slot with **no waffle-boot front** leaves the unit on that other bootloader, without
waffle-boot's recovery.

## Read more

- [`docs/front.md`](../blob/main/docs/front.md): the failsafes in full
- [`docs/recovery.md`](../blob/main/docs/recovery.md): every recovery write and refusal
