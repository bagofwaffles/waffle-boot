**Can I install waffle-boot on my OP-XY?**
Yes, with the installer: follow [How to Install](How-to-Install). waffle-boot is tested on real hardware,
but not widely: use it with caution. A failed install can leave a unit that needs a flash programmer. See
[Status and Roadmap](Status-and-Roadmap).

**Does this contain Teenage Engineering's firmware?**
No. waffle-boot is a clean-room implementation, not a decompilation. See
[Clean-Room Rules and Contributing](Clean-Room-Rules-and-Contributing).

**What is the difference between the boot menu and the recovery menu?**
The [Boot Menu](Boot-Menu) is part of the main image: installs, settings, the factory reset. The recovery
menu (Power + M4, [How to Recover](How-to-Recover)) is part of the front, which runs even when the rest of
the image is damaged. It only writes bootloader images and chooses the slot to start.

**Why two slots?**
So that a new bootloader is written beside the running one. If the new one fails, the old one still starts.

**Recovery opened by itself. Why?**
Either the image failed its check (`This image failed its check`), or three starts in a row did not finish
(`Three starts in a row did not finish`). Quickly switching off three times during start-up does it too.
Switching off and on with no key held starts normally again.

**Does opening the recovery menu change anything?**
No. Nothing is written until you confirm an action, or a computer's write passes its countdown.

**I confirmed a write from the computer by mistake. Can I stop it?**
Yes, during the 15 s countdown: switch the unit off or unplug USB. Stopping the tool or the page does not
stop it.

**Why does the boot menu say `OS needs reinstalling: install a TE firmware update`?**
The OS on the eMMC was stored encrypted, and waffle-boot loads only unencrypted images. An OS install
writes it in a form waffle-boot can load. See [Updating the OS](Updating-the-OS).

**Will a factory reset remove waffle-boot?**
No. It removes user data, logs and content packs only. See [Boot Menu](Boot-Menu#factory-reset).

**Does waffle-boot update the Bluetooth or backplane chips?**
It can: choose BLE, BLE bootloader or Backplane on the install's list. These steps are not tested on a unit
yet. The backplane step needs the PSoC unlock, and a build without it fails that step before anything is
sent.

**Which computer can I use for recovery?**
A computer with Chrome or Edge, with [the page](https://bagofwaffles.github.io/waffle-boot/). Or macOS
or Linux, with Python 3 and a clone of this repository, for `tools/wbrecover.py`. See
[How to Recover](How-to-Recover#write-a-slot-from-a-computer).

**How do I tell two builds apart on screen?**
The status bar shows the version, and in recovery the front's too (`v0.9.13 F16`). Run `make bump-version`
before each build you put on a unit.

**Where do I report a problem?**
Bugs: an issue, with the files from [Getting Logs Off the Unit](Getting-Logs-Off-the-Unit). Security
problems: the repository's private vulnerability reporting.
