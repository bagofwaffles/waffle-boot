waffle-boot's boot menu, built as an OS image. The bootloader already on a unit loads it in place of the
OS, so waffle-boot's drivers, menus, bridge, USB and display run on real hardware with nothing written to
the SPI flash. The [installer](Install-waffle-boot) is built on it.

> **Warning.** This can leave a unit unable to start. Try nothing here on a unit you are not prepared to
> lose. Loading a test OS replaces the unit's OS; keep TE's firmware file at
> hand to write it back.

## Variants

| Command | Builds | For |
| --- | --- | --- |
| `make test-os` | `build/bf703-testos/waffle-test-smc.ldr`, `waffle-test-gpio.ldr` | The boot menu on the unit; the two differ in how the panel is driven |
| `make test-os-mon` | `waffle-test-mon.ldr` | The same, with a USB monitor for `tools/testos_mon.py` |
| `make installer` | `build/waffle-install.ldr` | The hardware checks, then this build's `waffle-boot.ldr` written to the unit: see [Install waffle-boot](Install-waffle-boot) |

## Loading it

The bootloader on the unit must offer an OS write from its internal drive, as waffle-boot's developer menu
does (**5 Write /os.ldr to boot partition**).

1. Turn MTP on (developer menu, 2) and copy the image as `os.ldr`:

       python3 tools/mtpcp.py put build/bf703-testos/waffle-test-gpio.ldr
       python3 tools/mtpcp.py verify build/bf703-testos/waffle-test-gpio.ldr

   Go on only on `identical`. `mtpcp.py` needs libusb; quit other MTP clients first.
2. Run the OS write, then switch off and on. Judge it by what starts.

The test OS writes its log to `testos.txt` on its USB disk, and paints the panel a new color at each
start-up step, so a unit that shows nothing else still shows how far it got.

## Read more

[`docs/test-os.md`](../blob/main/docs/test-os.md): boot timing builds and the monitor's commands.
