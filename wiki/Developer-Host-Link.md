With **developer mode** on, the boot menu adds a USB serial port and answers requests from
`tools/testos_mon.py`. They are part of the standard image. With developer mode off every request is
refused and changes nothing. Label: S-10c.110.

> **Warning.** A hand-over with options can leave a unit that only the other slot brings back. Use it on a
> unit with a working bootloader in the other slot, and start that slot from the [Recovery Menu](Recovery-Menu).

## Turning it on

Boot menu → **2 Settings → 1 Developer mode: On**. The port is there on every boot menu screen, MTP
included. Turning MTP on or off re-enumerates USB: reopen the port.

## Requests

| Command | What it does |
| --- | --- |
| `ping`, `rd ADDR`, `wr ADDR VALUE` | Is it there; read or write the master's registers |
| `dev-srd`, `dev-swr` | Read or write a slave register over the bridge |
| `dev-flags [MASK]`, `dev-handover [MASK]` | Read or set the hand-over options, then hand over the OS with them |
| `dev-put FILE`, `dev-get` | Write `os.ldr` to the internal drive and verify it; print its SHA-256 and size |
| `dev-screen`, `dev-key KEY…`, `dev-shot OUT.png` | Read the screen, press keys, save the frame as a PNG |
| `dev-start` | Why the last plain start of the OS failed |
| `log [S]` | Print the bootloader's log for S seconds |
| `nordic-info` | The Nordic's own version text |
| `continue-os` | Restart the unit into the OS, even with Start in boot menu on. `continue-os --sysex` prints the same request as a MIDI message, for a unit with developer mode off |
| `dev-bench`, `dev-check`, `dev-extract` | Time the firmware package check and extraction |
| `dev-link-record`, `dev-link-test [N]` | The bridge link's record and its bench |
| `dev-yscan OUT.json`, `dev-l1rd` | Raw reads of the yaffs2 region and of L1 instruction memory |

`dev-key 5 7:down`, then `dev-key 7:up` after 5 s, opens System and runs the factory reset: nothing is
filtered, but what erases still waits for its key to be held 5 s.

## Hand-over options

A bitmask. **Zero is a normal hand-over.** Each bit moves one piece away from it (leave the caches off,
keep a DMA running, skip the slave's release, stay in the boot menu after the release, and so on), to find
which piece an OS needs. The table is in [`docs/devhost.md`](../blob/main/docs/devhost.md).

1. `tools/testos_mon.py dev-flags` reads the options.
2. `tools/testos_mon.py dev-flags 0x010` sets them.
3. `tools/testos_mon.py dev-handover` hands over. The port goes away if the OS was entered or the unit hung.
4. If the OS did not come up, start a slot that reaches the boot menu from the recovery menu, and retry
   with another mask.
