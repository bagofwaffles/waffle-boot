# waffle-boot

An open-source, clean-room bootloader for the Teenage Engineering OP-XY. It replaces the code that starts
the device's two processors, loads the OS, and offers menus for installs and repairs.

waffle-boot is an independent project. It is not affiliated with or endorsed by Teenage Engineering, and it
contains no code or data from the device's firmware. License: GPL-2.0-or-later.

> **Do not flash anything from this project onto an OP-XY.** There is no supported install yet, and a
> broken bootloader can leave the device unable to start. See [Status and Roadmap](Status-and-Roadmap).

<img src="images/main.png" width="480" alt="The boot menu's main screen">

## Use it

| Page | What it covers |
| --- | --- |
| [Boot Menu](Boot-Menu) | Opening the boot menu, and what each item does |
| [Recovery](Recovery) | What recovery is, how it opens, and which guide to follow |
| [Recovery Menu](Recovery-Menu) | Power + M4: start the other slot once, switch, keep, put back |
| [Recover from a Computer](Recover-from-a-Computer) | `tools/wbrecover.py`: write an image over USB |
| [My Unit Will Not Start](My-Unit-Will-Not-Start) | What you see, and which guide to follow |
| [Factory Reset](Factory-Reset) | Remove user data from the boot menu |
| [Installing Firmware](Installing-Firmware) | What can be installed today, and what cannot yet |
| [Getting Logs Off the Unit](Getting-Logs-Off-the-Unit) | System information, logs and recovery reports |

## Understand it

| Page | What it covers |
| --- | --- |
| [Status and Roadmap](Status-and-Roadmap) | What works on a unit, and where open work is tracked |
| [Hardware Overview](Hardware-Overview) | The two processors, the PSoC, the Nordic, the flash, the eMMC, the display |
| [Boot Chain](Boot-Chain) | From power-on to the OS |
| [Flash Layout and Write Safety](Flash-Layout-and-Write-Safety) | The SPI flash's areas, and the rules that keep a unit bootable |
| [Glossary](Glossary) | Slot, selector, front, trial start and other terms |
| [FAQ](FAQ) | Short answers |

## Work on it

| Page | What it covers |
| --- | --- |
| [Building and Testing](Building-and-Testing) | Toolchain, builds, unit tests, conformance, local CI |
| [Test OS](Test-OS) | The bootloader packaged as an OS image, for testing on a unit |
| [Developer Host Link](Developer-Host-Link) | USB requests the boot menu answers with developer mode on |
| [Requirement Labels](Specification) | The `S-NN.NNN` labels that code comments and tests cite |
| [Clean-Room Rules and Contributing](Clean-Room-Rules-and-Contributing) | What clean-room means here, and where contributions stand |
