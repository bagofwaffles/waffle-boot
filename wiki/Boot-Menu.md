The boot menu is waffle-boot's own set of screens, shown instead of starting the OS. (The code calls it
**held mode**.) For the red recovery screens, see [Recovery Menu](Recovery-Menu).

## Opening it

- **Hold com, or press and hold the white knob (knob 4), while you switch the unit on.** Keep holding until
  the menu appears, then let go. A key still held when the menu opens does nothing until pressed again.
- **Settings → 6 Start in boot menu: On.** Every start then opens the menu instead of the OS.
- **By itself**, when the OS cannot be started, when the last install did not finish, or when the OS
  restarts the unit into the bootloader for an update.

## Keys

Items are chosen by digit. Digits 1 to 4 are M1 to M4, the knob 1 to 4 clicks, or sound keys 1 to 4.
Digits 5 to 8 are sound keys 5 to 8 only. **8 goes back** on every screen below the main one.

Whatever erases asks first: press and hold the item's own key. Its row turns red with `Hold 7 for 5 s` (its
digit), and a bar fills along the bottom row. Held for 5 s, it runs once; letting go earlier, or any other
key, cancels. A knob click never starts it.

## Main Menu

<img src="images/main.png" width="480" alt="Main Menu">

| Item | What it does |
| --- | --- |
| 1 Install firmware | Offers a USB disk for a firmware package ([Installing Firmware](Installing-Firmware)). Hold **shift** while pressing 1 to also copy system information and logs to the disk ([Getting Logs Off the Unit](Getting-Logs-Off-the-Unit)) |
| 2 Settings | The settings below |
| 3 Developer menu | Shown only while developer mode is on |
| 4 Device info | Serial numbers, versions, the second processor's state, and why the OS did not start. The help key opens it too |
| 5 System | Key test, volume calibration, content packs, format, factory reset |

The status bar shows the running slot (`SLOT1` or `SLOT2`) and `USB`, lit while a disk is offered. Under
the items, the screen says when the OS could not be started, and why.

## Settings

<img src="images/settings.png" width="480" alt="Settings">

| Item | What it does |
| --- | --- |
| 1 Developer mode | Off by default. On: shows the developer menu and adds a USB serial port |
| 2 Protected writes | Lets installs write the bootloader slots and the first stage, and lets a computer change the unit's serial and identity. Hold 2 for 5 s to turn it on; its warning shows while you hold. One press turns it off. Lasts until the next restart. **A power cut while the first stage is written needs a flash programmer** |
| 3 Color | Off draws every screen in gray |
| 5 MTP target | macOS or Windows: which MTP interface the developer menu offers |
| 6 Start in boot menu | Every start opens this menu. Kept in the SPI flash, not on the drive |

Developer mode, Color and the MTP target are kept on the internal drive; a format returns them to their
defaults.

### The settings file

They are kept in `/yaffs2/bootloader.settings`, a text file of at most 512 bytes. Its first line is
`waffle-boot settings 1`; each other line is `key=value`:

```
waffle-boot settings 1
developer_mode=off
color=on
mtp_target=macos
```

| Key | Values | Default |
| --- | --- | --- |
| `developer_mode` | `on`, `off` | `off` |
| `color` | `on`, `off` | `on` |
| `mtp_target` | `macos`, `windows` | `macos` |

Lines end in LF or CR LF. An unknown key or value is ignored. A missing file, one over 512 bytes, one with
a byte that is not printable ASCII, or one without the first line gives every setting its default. The
boot menu writes the whole file to `bootloader.settings.new`, then renames it over the old one.

## System

<img src="images/system-armed.png" width="480" alt="System, factory reset held">

| Item | What it does |
| --- | --- |
| 1 Key test | Press every key and knob. **Shift + 8** leaves it |
| 2 Reset volume calibration | Resets the volume knob's calibration, then returns to the main menu |
| 3 Remove content packs | Shown when packs are installed. Hold a pack's digit for 5 s to remove it |
| 5 Format drive | Erases the whole internal drive. Hold M1 or sound key 1 for 5 s |
| 7 Factory reset | Removes user data ([Factory Reset](Factory-Reset)) |

## Developer menu

| Item | What it does |
| --- | --- |
| 2 MTP file transfer | Shares the internal drive with the computer. 8 turns it off |
| 5 Write /os.ldr to boot partition | Writes the file `os.ldr` from the drive to the eMMC's inactive boot partition. After a success, 6 restarts into the OS |
| 6 Restart | Restarts the unit |
