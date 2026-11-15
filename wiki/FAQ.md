**Can I install waffle-boot on my OP-XY?**
Not yet. The [installer](Install-waffle-boot) has not yet been run on a unit, and a failed install can leave a unit that needs a flash programmer.
See [Status and Roadmap](Status-and-Roadmap).

**Does this contain Teenage Engineering's firmware?**
No. waffle-boot is a clean-room implementation, not a decompilation. Its code is written from scratch, from
public documentation and the project's own functional specification, and the repository holds no code or
data from the device's firmware. See
[Clean-Room Rules and Contributing](Clean-Room-Rules-and-Contributing).

**What is the difference between the boot menu and the recovery menu?**
The [Boot Menu](Boot-Menu) is part of the main image: installs, settings, the factory reset. The
[Recovery Menu](Recovery-Menu) is part of the front, which runs even when the rest of the image is damaged.
It only writes bootloader images and chooses the slot to start.

**Why two slots?**
So that a new bootloader is written beside the running one. If the new one fails, the old one still starts.

**Recovery opened by itself. Why?**
Either the image failed its check (`This image failed its check`), or three starts in a row did not finish
(`Three starts in a row did not finish`). Quickly switching off three times during start-up does it too.
Switching off and on with no key held starts normally again.

**Does opening the recovery menu change anything?**
No. Nothing is written until you confirm an action, or a computer's write passes its countdown.

**I confirmed a write from the computer by mistake. Can I stop it?**
Yes, during the 15 s countdown: switch the unit off or unplug USB. Stopping the tool does not stop it.

**Why does the boot menu say `OS needs reinstalling: install a TE firmware update`?**
The OS on the eMMC was stored encrypted, and waffle-boot loads only unencrypted images. An OS install
writes it in a form waffle-boot can load. See [Installing Firmware](Installing-Firmware).

**Will a factory reset remove waffle-boot?**
No. It removes user data, logs and content packs only. See [Factory Reset](Factory-Reset).

**Does waffle-boot update the Bluetooth or backplane chips?**
Not yet. Those install steps are switched off and show `Skipped: not in this version`.

**Which computer can I use for recovery?**
macOS or Linux, with Python 3 and a clone of this repository. See
[Recover from a Computer](Recover-from-a-Computer).

**How do I tell two builds apart on screen?**
The status bar shows the version, and in recovery the front's too (`v0.9.13 F15`). Run `make bump-version`
before each build you put on a unit.

**Where do I report a problem?**
Bugs: an issue, with the files from [Getting Logs Off the Unit](Getting-Logs-Off-the-Unit). Security
problems: the repository's private vulnerability reporting.
