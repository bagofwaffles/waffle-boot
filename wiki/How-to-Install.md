**Install with the guided page: <https://bagofwaffles.github.io/waffle-boot/?mode=install>**

It walks through every step in the browser: getting the installer onto the unit, the checks, and the
install itself.

> **Back up your files first.** The OS has to be installed again afterward.

> **Status:** tested on real hardware, but not widely: use it with caution. A broken bootloader can leave a
> unit that only an external flash programmer brings back.

## You need

- The unit, charged, and a USB cable. Keep it on USB while it writes.
- A computer with Chrome or Edge.
- A developer menu on the unit that writes an OS file. A unit that has never had developer mode on needs it
  turned on first, with the [ferret tool](https://bagofwaffles.github.io/ferret/op-xy/). This project does
  nothing for that step.
- The OS file, to install again when waffle-boot is on the unit ([Updating the OS](Updating-the-OS)).

Every write is started on the unit: you hold the key it names for 5 s. To leave at any point without
installing, hold **8** for 5 s.

A unit that already runs waffle-boot does not need the installer: choose **Upgrade / Recover** on the page
([How to Recover](How-to-Recover)).

## When something does not pass

| The unit shows | It means | What to do |
| --- | --- | --- |
| The install is not offered | A check failed or could not run, or the carried image did not pass its check. Nothing was written to the SPI flash | Save `check.txt` (on the page, or from the unit's USB disk). Hold 8 to leave. A wrong check is fixed in the installer; nothing overrides it |
| The trial did not reach the boot menu | The trial start ran, or did not run, the new bootloader, but its boot menu did not show | Nothing was kept: the unit starts the bootloader it had. Hold 1 to write the trial again, or 8 to leave |
| The write did not complete | A write or its read-back failed | The unit keeps a bootloader that starts. Hold 1 to try again, or 8 to leave |

<img src="../docs/ui/installer-locked.png" width="480" alt="The install is not offered">

`check.txt` lists every check. It holds no serial number or key.

## More

[Installer Reference](Installer-Reference): what each step writes, the checks, building the installer, and
its USB protocol.
