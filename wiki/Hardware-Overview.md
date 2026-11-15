The parts of the OP-XY the bootloader drives.

| Part | What it does | [Labels](Specification) |
| --- | --- | --- |
| Two ADSP-BF703 (Blackfin+) processors | The **master** and the **slave**. A strap pin (PA2) tells them apart. Both run the same bootloader image | S-01, S-17 |
| Boot ROM | On each chip. Starts the master from the SPI flash, and the slave from the master over SPI. Cannot be changed | S-01 |
| SPI flash, 1 MiB | On the master. Holds the factory first stage, the metadata, the slot selector, two bootloader slots and the boot store | S-02 |
| DDR2 memory | 2 Gbit, 16-bit, 200 MHz. Set up by the bootloader's first steps; the core runs at 400 MHz | S-13a |
| The eMMC | Soldered on the board, on the slave. Two boot partitions hold the OS (A/B); the user area holds the internal drive (yaffs2) and the OS's FAT volume | S-03, S-12 |
| SPI bridge | Links the two processors: the master streams the slave its boot image, then sends it requests (eMMC, files) | S-09, S-13b |
| PSoC | A coprocessor on the master's UART0 (1 Mbaud). Scans the keys and knobs, drives the LEDs, switches power, reports the battery and USB supply. Shown as "Backplane" on screens | S-06, S-07 |
| Nordic | The Bluetooth coprocessor, on the master's UART1 (1 Mbaud). Shown as "BLE" | S-08 |
| Display | 480 × 222 visible pixels, RGB565, on an 8-bit parallel bus with an ST7796S-class controller | S-13c |
| USB device | On the master. Offers a disk, MTP, MIDI and a serial port, by screen | S-10a to S-10c |

## Keys the bootloader reads

The PSoC reports 72 keys and knob clicks by number. The bootloader uses few:

| Key | Where it matters |
| --- | --- |
| M1 to M4 | Digits 1 to 4 in the boot menu; keys 1 to 4 in the recovery menu. M4 at power-on is Power + M4 |
| Knob 1 to 4 clicks | Digits 1 to 4 in the boot menu. Knob 4 is the **white knob** |
| Sound keys 1 to 8 | Digits 1 to 8 in the boot menu; sound 8 is **Back** everywhere, recovery included |
| com (`usb`, key `0x18`) and the white knob | Held at power-on: open the boot menu |
| Shift | Held while opening Install firmware: writes the system information to the USB disk |
| Help | Opens Device Info from the main screen |

## Power

The power switch is handled by the PSoC. Every wait in the start-up serves the PSoC, so switching off works
at any point, recovery included. The one exception: recovery holds the power-off back while a write leaves
no slot that starts ([Flash Layout and Write Safety](Flash-Layout-and-Write-Safety)). On USB power with
the switch off, the unit only charges; the screen stays dark.

## Read more

- [Boot Chain](Boot-Chain), [Flash Layout and Write Safety](Flash-Layout-and-Write-Safety)
- [Requirement Labels](Specification)
