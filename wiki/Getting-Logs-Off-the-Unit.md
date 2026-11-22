Four ways to collect what you need for a bug report. The bootloader itself writes no log files; it copies
the OS's.

> **Privacy.** `systeminfo/serial.txt` holds the unit's serial numbers. Remove it before you share files.

## 1. System information and logs to the USB disk

**You need:** a USB cable and a computer.

1. Open the [Boot Menu](Boot-Menu).
2. Hold **shift**, and press **1** (**Install firmware**). Let go.
3. A disk appears on the computer. Copy the `systeminfo` and `logs` folders.
4. Eject the disk. The unit looks for a package and finds none (`No valid package`).
5. Press **8** to go back.

| File | Holds |
| --- | --- |
| `systeminfo/version.txt` | Firmware, Blackfin, bootloader, backplane and Nordic versions |
| `systeminfo/serial.txt` | Unit, DSP and backplane serial numbers |
| `systeminfo/licenses.txt` | Third-party notices |
| `logs/` | The newest log files from the internal drive, up to 5 MiB |

## 2. The screen

**4 Device info** shows versions, serials, the second processor's state (`DSP2`) and, after a failed
start, the step that failed (`OS start`). A photo of it is often enough.

## 3. The recovery report

In the [recovery menu](How-to-Recover#open-the-recovery-menu-power--m4) (Power + M4), with USB connected, from the
repository's root:

    python3 tools/wbrecover.py info

It prints both slots, the selector, what each command would do, and the battery. The page's Upgrade / Recover tab shows what the
unit reports once connected.

## 4. The live bootloader log (developers)

1. In the boot menu, **2 Settings → 1 Developer mode: On**. The unit adds a USB serial port.
2. From the repository's root:

       python3 tools/testos_mon.py log 60

   It prints the log for 60 s. Give `--port PORT` before `log` if the port is not found.

## If it does not work

- **No disk appears.** Use another cable or port. The status bar's `USB` lights while the disk is offered.
- **No `logs` folder.** The OS has written no logs yet.
- **No serial port.** Developer mode must be on, and the unit in the boot menu.
