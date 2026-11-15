waffle-boot's boot menu, built as an OS image. The bootloader already on a unit loads it in place of the
OS, so waffle-boot's drivers, menus, bridge, USB and display run on real hardware with nothing written to
the SPI flash. Its installer variant is how waffle-boot gets onto a unit today.

> **Warning.** This can leave a unit unable to start. There is no supported install. Try nothing here on a
> unit you are not prepared to lose. Loading a test OS replaces the unit's OS; keep TE's firmware file at
> hand to write it back.

## Variants

| Command | Builds | For |
| --- | --- | --- |
| `make test-os` | `build/bf703-testos/waffle-test-smc.ldr`, `waffle-test-gpio.ldr` | The boot menu on the unit; the two differ in how the panel is driven |
| `make test-os-mon` | `waffle-test-mon.ldr` | The same, with a USB monitor for `tools/testos_mon.py` |
| `make test-os-install` | `waffle-test-install.ldr` | The monitor, with this build's `waffle-boot.ldr` inside, to write it to a slot |

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

## The installer

Run `make bump-version` before each build that goes onto a unit. With the installer running as the OS:

    python3 -I -B tools/testos_mon.py install status
    python3 -I -B tools/testos_mon.py install trial yes
    python3 -I -B tools/testos_mon.py install commit yes

| Form | Writes | Next start |
| --- | --- | --- |
| `status` | Nothing | |
| `trial` | The slot that is not selected, a record that fails its check on purpose, a pending trial entry, the selector | The new slot runs once and marks its start. The start after it boots the old slot |
| `commit` | The record, right this time, and the selector. Needs the trial start's mark | The new slot is trusted |

The installer never touches the first stage, the metadata or the selected slot, and checks the image's CRC
before the first erase. After `trial`, restart with no key held: the new slot starts once and loads the
installer again as the OS. Then run `commit`. The way back afterwards is the [Recovery Menu](Recovery-Menu).

A trial proves the start-up, the display and the boot menu. It does not prove the bridge or the OS hand-over.

## Read more

[`docs/test-os.md`](../blob/main/docs/test-os.md): boot timing builds and the monitor's commands.
