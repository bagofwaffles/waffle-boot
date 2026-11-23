| Term | Meaning |
| --- | --- |
| **Boot menu** | waffle-boot's own screens, shown instead of starting the OS. The code calls it **held mode** |
| **Recovery menu** | The red screens of the front, opened by Power + M4. Writes images and chooses the slot to start |
| **Power + M4** | M4 held, alone, while the unit is switched on. The only key chord |
| **Master, slave** | The two BF703 processors. The master runs the flash, PSoC, Nordic, display and USB; the slave the eMMC. Screens call the slave DSP2 |
| **First stage** | The program at flash offset 0 that the boot ROM loads; it picks a slot on every start. Not part of waffle-boot |
| **Slot** | One of two places in the flash for a bootloader image: **Slot 1** and **Slot 2** on screen (0 and 1 in the code) |
| **Selector** | Two bytes in the flash: which slot to start, and `verified` |
| **Verified** | The selector byte that says "start without a check" (`0x01`): the first stage then starts the boot slot as it is. Any other value makes the first stage check the slot and write the selector (S-02.020 to S-02.022) |
| **Record** | One per slot: the image's length, CRC-32 and a tag |
| **Checked, check failed** | A slot whose record matches its image is checked; otherwise its check failed |
| **Boot slot** | The slot the selector names: the one the next start runs |
| **Bootloader type** | `WB` (waffle-boot), `TE` (a TE bootloader) or `??`, shown for each checked or tried slot |
| **Running slot** | The slot this start came from. Usually the boot slot |
| **Front** | The small first part of every waffle-boot image: start-up to the key read, and recovery. Frozen and pinned |
| **Front pin** | The SHA-256 of the front's bytes, kept in `src/front/front.pin` |
| **Main image** | The rest of a waffle-boot image: the boot menu, the OS load, installs. Stored deflated in the slot |
| **Stub** | The small part after the front that inflates the main image into memory and starts it |
| **Try, trial start** | A slot started once without being trusted. If it does not come up, the next start boots the old slot |
| **Tried, started** | A slot under trial; `started` once its front has come up in that trial |
| **Keep** | Make a tried slot that started the boot slot |
| **Switch** | Make the other slot the boot slot, with no trial |
| **Put back** | Make a tried slot's record pass its check again, undoing a try |
| **Start log** | The last sector of a slot. Counts starts that did not finish; three in a row open recovery |
| **Settings sector** | The SPI flash sector below each slot's start log (`0x86000`, `0xFE000`) that holds the settings of that slot's image |
| **Countdown** | The 15 s before recovery writes for a computer. Switching off in it stops the write |
| **In-place write** | A write of the running slot while the other slot's check failed. The one write with nothing to fall back on |
| **Protected writes** | A boot menu setting, until the next restart, that lets installs write the slots. No install writes the first stage |
| **The eMMC** | The flash memory chip soldered on the board. Holds the OS and the internal drive |
| **Internal drive** | The yaffs2 volume on the eMMC: user data, content packs, logs |
| **Boot partitions** | Two eMMC areas holding the OS image; one is active |
| **Hand-over** | The bootloader passing control to the OS |
| **Test OS** | The boot menu built as an OS image, for testing on a unit |
| **White knob** | Knob 4. Held at power-on, it opens the boot menu |
| **com** | The key the code calls `usb` (`0x18`). Held at power-on, it opens the boot menu |
| **Backplane** | Screens' name for the PSoC, the key and power coprocessor |
| **BLE** | Screens' name for the Nordic Bluetooth chip |
| **LDR stream** | The boot ROM's image format, used for the first stage, the slots and the OS |
| **Package** | A firmware file (`.tfw`) installed from the boot menu's USB disk |
| **Signature state** | What the check of a package's signature found: valid, does not match, or none. It never refuses a package or a part; a valid one is shown as signed by TE, and one that does not match is installed only after a 5 s hold of 1, or from an approved host link session |
| **Developer mode** | A setting that adds the developer menu and a USB serial port |
| **Flash programmer** | An external tool that writes the SPI flash chip directly. The only fix for a unit with nothing that starts |
