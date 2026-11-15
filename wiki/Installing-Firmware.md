What can be installed on a unit that runs waffle-boot, and what cannot yet.

| Task | Status |
| --- | --- |
| Install waffle-boot on a unit with the factory bootloader | **Not supported yet.** Developers: [Test OS](Test-OS) |
| Replace waffle-boot in one slot with another waffle-boot build | [Recover from a Computer](Recover-from-a-Computer) |
| Install a TE firmware package (content and OS) from the boot menu | Built; **not yet confirmed** on a unit |
| Update the Bluetooth (BLE) or backplane (PSoC) chips | **Not supported yet.** Skipped: `Skipped: not in this version` |
| Return to the factory bootloader | **Not supported yet** |
| Write an OS image file directly | Developers: Boot menu → Developer menu → 5 |

## A TE firmware package, from the boot menu

**You need:** a USB cable, a computer, and the firmware package file (its name contains `tfw`).

1. Open the [Boot Menu](Boot-Menu) and press **1** (**Install firmware**).
2. A disk appears on the computer. Copy the package file to the disk's top folder.
3. Eject the disk. After a short wait the unit checks the package and installs it.
4. Do not switch off while it runs. Steps show as `Installed`, `Not included` or `Skipped`.
5. At `Done!`, press **6** to restart into the OS, or switch off and on.

<img src="images/upgrade-wait.png" width="480" alt="Waiting for a package">

With **Protected writes** off (the default), a package installs content and the OS only, and never touches
the bootloader or the first stage.

> **Warning.** With Protected writes on, an install writes the package's bootloader into a slot in place of
> waffle-boot, and can write the package's first stage. The unit asks for a 5 s hold of 1 before each.
> Skip both (8). A power cut while the first stage is written leaves a unit that only a flash programmer
> brings back.

## If it does not work

| Message | Meaning |
| --- | --- |
| `No valid package found` | No file with `tfw` in its name in the disk's top folder, or the package was refused. The reason line says why |
| `Upgrade failed.` or `Installation failed.` | A step failed; its row says which. Switch off and on, then install again |
| `The last install did not finish.` | The unit stopped mid-install. Install the same package again |
| `OS needs reinstalling: install a TE firmware update` | The OS on the eMMC is stored encrypted, which waffle-boot cannot load. An install of the OS writes it in a form it can |
