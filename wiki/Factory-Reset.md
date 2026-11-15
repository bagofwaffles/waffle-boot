Remove your user data from the unit, from the boot menu.

> **Warning.** This cannot be undone. Copy anything you want to keep first.

**You need:** the unit, charged or on USB.

## What it removes

- the folders `user`, `logs` and `content/dlc` (installed content packs) on the internal drive;
- the start of the OS's own FAT volume on the eMMC.

It keeps the bootloader, the OS, waffle-boot's settings and **Start in boot menu**. It does not restart the
unit.

## Steps

1. Open the [Boot Menu](Boot-Menu): hold **com** or the white knob while you switch on.
2. Press **5** (sound key 5): **System**.
3. Press and hold **7** (sound key 7). The **Factory reset** row turns red, shows `Hold 7 for 5 s`, and a
   bar fills along the bottom row.
4. Keep holding for 5 s; the reset starts then. Letting go earlier, or pressing any other key, cancels and
   nothing is removed.
5. Wait for `User data removed. Please restart your OP-XY.`
6. Switch the unit off and on.

<img src="images/system-armed.png" width="480" alt="Factory reset, 7 held">

## To erase everything instead

**System → 5 Format drive** erases the whole internal drive, settings included. Hold M1 or sound key 1 for
5 s to confirm it. After the format the unit opens **Install Firmware**.

## If it does not work

| Message | Meaning |
| --- | --- |
| `User data not removed: the drive can't be read` | The drive cannot be read. Nothing was removed. A format erases it |
| `User data not removed: DSP2 is not answering` | The second processor stopped. Switch off and on, then try again |
| `Some user data could not be removed` | Part of it stayed. Switch off and on, then try again |
