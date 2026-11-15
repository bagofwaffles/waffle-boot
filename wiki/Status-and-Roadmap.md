**Under development. Boots on one unit. No supported install.**

> Do not flash anything from this project onto an OP-XY. A broken bootloader can leave a unit that only an
> external SPI flash programmer brings back.

## Shown on a unit

- A start from a committed slot through to the OP-XY's own OS: the boot ROM, the factory first stage,
  waffle-boot, the second processor, and the OS loaded from the eMMC.
- Writing a slot over USB from recovery.
- The [Test OS](Test-OS) installer's trial and commit.

## Open work

Open work, including what is not yet proven on a unit, is tracked in the repository's issues.

## Will not be done here

- Repairing the factory first stage. It is not part of this project; a failed write of it needs a flash
  programmer.
- Decrypting an OS stored encrypted on the eMMC. The boot menu then says
  `OS needs reinstalling: install a TE firmware update`.

## Security

Report a security problem through the repository's private vulnerability reporting, not a public issue.
There is no release yet; fixes go to the current branch.
