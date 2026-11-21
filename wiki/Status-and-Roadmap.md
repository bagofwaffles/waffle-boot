**Status:** tested on real hardware, but not widely: use it with caution.

> A broken bootloader can leave a unit that only an external SPI flash programmer brings back.

## Shown on an OP-XY

- A start from a committed slot through to the unit's own OS: the boot ROM, the factory first stage,
  waffle-boot, the second processor, and the OS loaded from the eMMC.
- Writing a slot over USB from recovery ([How to Recover](How-to-Recover)).
- The installer: its checks, the trial, and keeping the new bootloader ([How to Install](How-to-Install)).

## Shown on an OP-1 Field (initial support, from 0.9.48)

On one unit:

- The survey, then the installer, each put on with `tools/mtpcp.py prepare-1f` and the unit's own OS
  write; the prepare step takes the unit's bootloader 1000.2.0 or 1000.1.1 only
  ([Installer Reference](Installer-Reference#on-an-op-1f)). The installer's trial and keeping the new
  bootloader.
- Writing a slot from recovery, a trial start, and keeping it.
- A start through to the unit's own OS.

Not yet run on an OP-1 Field: the Bluetooth (BLE) chip's updates, the backplane update, and installing a
firmware package. Recovery's key there is provisional. The installer knows one OP-1 Field first stage; on a
unit with another it writes nothing.

## Open work

Open work is tracked in the repository's issues.

## Will not be done here

- Repairing the factory first stage. It is not part of this project; a failed write of it needs a flash
  programmer.
- Decrypting an OS stored encrypted on the eMMC. The boot menu then says
  `OS needs reinstalling: install a TE firmware update`.

## Security

Report a security problem through the repository's private vulnerability reporting, not a public issue.
Fixes go to the current branch and into the next release.
