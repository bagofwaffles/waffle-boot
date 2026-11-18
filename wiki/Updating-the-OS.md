Updating the OS from the boot menu: install a TE firmware package (content and OS) on a unit that runs
waffle-boot. To install waffle-boot itself, see [How to Install](How-to-Install).

## What can be installed

| Task | Status |
| --- | --- |
| Install a TE firmware package (content and OS) from the boot menu | Built; **not yet confirmed** on a unit |
| Install waffle-boot | [How to Install](How-to-Install) |
| Replace waffle-boot in one slot with another waffle-boot build | [How to Recover](How-to-Recover#write-a-slot-from-a-computer) |
| Update the Bluetooth (BLE) or backplane (PSoC) chips | **Not supported yet.** Those steps show `Skipped: not in this version` |
| Return to the factory bootloader | **Not supported yet** |
| Write an OS image file directly | Developers: Boot menu → Developer menu → 5 ([Test OS](Test-OS#loading-it)) |

## Steps

You need a USB cable, a computer, and the firmware package file (its name contains `tfw`).

1. Open the [Boot Menu](Boot-Menu) and press **1** (**Install firmware**).
2. A disk appears on the computer. Copy the package file to the disk's top folder.
3. Eject the disk. After a short wait the unit checks the package and installs it.
4. Do not switch off while it runs. Steps show as `Installed`, `Not included` or `Skipped`.
5. At `Done!`, press **6** to restart into the OS, or switch off and on.

<img src="../docs/ui/upgrade-wait.png" width="480" alt="Waiting for a package">

With **Protected writes** off (the default), a package installs content and the OS only, and never touches
the bootloader or the first stage.

> **Warning.** With Protected writes on, an install writes the package's bootloader into a slot in place of
> waffle-boot. The unit asks for a 5 s hold of 1 first; skip it with 8. A package's first stage is never
> written: its step shows `Skipped: not in this version`.

## If it does not work

| Message | Meaning |
| --- | --- |
| `No valid package found` | No file with `tfw` in its name in the disk's top folder, or the package was refused. The reason line says why |
| `Upgrade failed.` or `Installation failed.` | A step failed; its row says which. Switch off and on, then install again |
| `The last install did not finish.` | The unit stopped mid-install. Install the same package again |
| `Bootloader  Skipped: written, runs on restart` | The same package was installed again before a restart: its bootloader is already in the other slot and starts after the next restart |
| `OS needs reinstalling: install a TE firmware update` | The OS on the eMMC is stored encrypted, which waffle-boot cannot load. An install of the OS writes it in a form it can |
