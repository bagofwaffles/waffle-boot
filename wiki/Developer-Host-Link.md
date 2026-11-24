The boot menu's **host link**: a small, versioned protocol that lets a computer or a web page read and write
the unit's files, install a package and restart the unit, after one hold on the unit approves it. Any site
or tool may use it; `docs/web/hostlink.js` (Web Serial or Web MIDI) and `tools/hostlink.py` are clients.
It also carries the PSoC's link as raw bytes and the PSoC's reset, for a computer's own PSoC bootloader
tool, after a warning instead of the hold ([below](#the-psoc-bridge-and-reset)). The developer requests
(memory, the second processor, hand-overs) are [below](#developer-requests).

- **Host link** is a setting, on by default: Boot menu → **2 Settings → 7 Host link**. Off, the unit
  answers only HELLO, saying it is off, and refuses every change asked for over USB.
- **One hold approves a computer for the session.** The session ends at the next restart, when Host link
  is turned off, or when the computer ends it. It does not end when USB is unplugged: the code cannot
  tell that apart from the unit's own USB changes.
- **Safe reads need no approval**; changes and the reads of files' contents need it.
- **Never from a computer**, approved or not: the factory reset and Protected writes. Protected writes
  applies to what a computer asks for exactly as at the unit.
- **The PSoC requests need no hold.** The keys come from the PSoC, so a PSoC that does not run could never
  approve anything. The bridge and the PSoC reset need Host link on and nothing more: the unit always shows
  a warning and counts down 15 s first, approved or not, and switching it off (or 8 on it, while the keys
  work) stops them.

## The approval

A computer asks with APPROVE. The unit shows, over the screen it was on (Main Menu, Settings, System,
Device Info or the developer menu, with nothing under way):

```
Host Link
  Allow this computer?
  Files, installs, restarts. Until restart.

  1 Allow (hold to select)
  8 Cancel
```

Only the unit's own keys answer it: **1 held** allows, **8** refuses, and 60 s with no key is
a timeout. A knob click starts no hold. The screen it covered comes back after the answer. The computer
reads the answer with SESSION. Until then every call that needs approval answers status 2, so a page can
say "hold 1 on the unit".

**The exception: the PSoC requests.** PSOC_OPEN and PSOC_RESET need no hold and no session. The unit warns
and counts down 15 s instead ([below](#the-psoc-bridge-and-reset)), whether or not the session is approved.
Every other call keeps the rule above.

## Framing

A request is one S-06 frame: route `0x21` (host to master), a sequence byte, the id **`0x7C`**, the
payload's length, the payload and a CRC-8 over the four header bytes and the payload (reflected
polynomial `0x4D`, each input byte XORed with `0xA5` first), the whole COBS-encoded and ended by a zero
byte. Replies come on route `0x12`. Frames of other ids share the port (the log): skip them. The most
payload is 249 bytes.

On the USB serial port the frames are the port's bytes. On the USB MIDI port (with no serial port, as
with developer mode off) each run of up to 245 frame bytes, the zero byte included, is one SysEx message:
`F0 00 20 76 <device> 66`, the bytes packed seven to eight (a byte of bit 7s, then the seven bytes'
low bits), `F7`. The device byte is `21` on the OP-XY and `02` on the OP-1 Field.

The payload is **call:1 tag:1** and the call's arguments; the reply is **call:1 tag:1 status:1** and its
data. The tag is the host's own: the reply carries it back. Numbers are little-endian. Paths are UTF-8,
with no NUL, to the end of the request.

## Versions

HELLO always answers, with the protocol's version and the oldest client version the unit serves. A
client whose version is outside that range stops there. A newer client finds out what this unit has from
HELLO's lists of calls, actions and roots; a call it does not have answers status 5.

## Calls

| Call | Id | Request after call and tag | Reply data | Class |
| --- | --- | --- | --- | --- |
| HELLO | `00` | `version:1` (the client's) | below | safe |
| SESSION | `01` | | `state:1` | safe |
| APPROVE | `02` | | `state:1`: pending, or approved | safe |
| END | `03` | | | safe |
| INFO | `04` | | the 24 bytes below | safe |
| SET | `05` | `setting:1 value:1` | | needs approval; setting 2 never |
| JOB | `06` | `job:1` | a job record | safe |
| STAT | `10` | `path` | `type:1 size:4 mtime:4` | safe on `/yaffs2`, else needs approval |
| LIST | `11` | `index:2 path` | `next:2`, then entries `type:1 size:4 len:1 name` | safe on `/yaffs2`, else needs approval |
| GET | `12` | `offset:4 length:1 path` | `offset:4`, up to 224 bytes; fewer: the end | needs approval |
| HASH | `13` | `path` | `size:4 sha256:32` | needs approval |
| PUT_OPEN | `14` | `size:4 sha256:32 path` | `have:4` | needs approval |
| PUT_DATA | `15` | `offset:4`, up to 224 bytes | `have:4` | needs approval |
| PUT_END | `16` | | | needs approval |
| REMOVE | `17` | `path` (a file, or an empty directory) | | needs approval |
| MKDIR | `18` | `path` | | needs approval |
| RENAME | `19` | `from`, `00`, `to` (one root; no name in the way) | | needs approval |
| RUN | `20` | `action:1`, its arguments | a job record | by its action |
| PSOC_OPEN | `30` | `entry:1`: 1 the bootloader-entry message first, 0 none | the PSoC record | warned: no approval |
| PSOC_DATA | `31` | the bytes for the PSoC, up to 247 | the PSoC record, then the PSoC's bytes, up to 236 | warned: no approval |
| PSOC_CLOSE | `32` | | the PSoC record, then the PSoC's last bytes | warned: no approval |
| PSOC_STATE | `33` | | the PSoC record | safe |
| PSOC_RESET | `34` | | the PSoC record | warned: no approval |
| PSOC_EVENT | `7E` | (the unit's own) | `status 0`, the PSoC record | |
| EVENT | `7F` | (the unit's own) | `status 0`, a job record | |

Never from a host: the actions `10` (factory reset) and `11` (Protected writes), and SET's setting 2
(Protected writes), approved or not. The developer requests (id `0x7D`) need developer mode and the
approval. One table in the code holds this (`hl_calls`, `hl_actions` and `hl_settings` in
`src/hostlink.c`); a unit test holds every id there once.

`type` is 1 a file, 2 a directory. LIST's `next` is the index to ask from next, `FFFF` once the
directory has no more. SET's setting 7 value 0 turns Host link off for this start, as a computer's key
on Settings would; it is not saved.

**HELLO**'s reply: `57 42 48 4C` ("WBHL"), `version:1 oldest:1 on:1`; with Host link off, nothing more.
On: `session:1 max_payload:1 chunk:1`, INFO's 24 bytes, `n:1` and the calls, `n:1` and the actions
(not the never ones), `n:1` and the roots, each `id:1 flags:1 name 00` (flags: 1 read, 2 write, 4 list
and stat open).

**INFO**: the bootloader's version (4 × u16), the front's version (u16), the device (0 OP-XY, 1 OP-1 Field),
the running slot (0 Slot 1, 1 Slot 2, `FF` not established), this start's reason (u16), flags (u16: 1
Protected writes, 2 developer mode, 4 color, 8 MTP target macOS, 16 Start in boot menu, 32 Host link, 64
the second processor answering, 128 the drive mounted), and the OS version in the metadata (4 × u16).

**Session** states: 0 none, 1 pending, 2 approved, 3 refused, 4 timed out.

## Status codes

| Status | Meaning |
| --- | --- |
| 0 | ok |
| 1 | Host link is off |
| 2 | needs the session's approval: hold 1 on the unit |
| 3 | never from a host |
| 4 | malformed |
| 5 | no such call, action or setting in this build |
| 6 | the path is refused |
| 7 | not found |
| 8 | exists |
| 9 | I/O error |
| 10 | busy, or the approval screen cannot show now |
| 11 | the whole file's SHA-256 is not the one declared |
| 12 | not the offset the unit holds; the data is `have:4` |
| 13 | the volume does not answer (the second processor is lost: restart) |
| 14 | the root takes no write |
| 15 | the unit's own checks refused it |
| 16 | the volume is not one this build reads (exFAT, or no FAT volume): "not readable by this build" |
| 17 | the PSoC bridge is not open |

## Files

A path starts with a root. One function checks every path (`hl_path_parse()`): a root's name, then
components of 1 to 255 bytes, none `.` or `..`, none empty, no trailing `/`, no byte below `0x20`, `0x7F`,
`\` or `:`, at most 192 bytes in all. A rename stays in one root. No file call reaches the SPI flash,
memory or a raw sector of the drive.

| Root | What it is | Read | Write | List and stat | OP-XY | OP-1 Field |
| --- | --- | --- | --- | --- | --- | --- |
| `/yaffs2` | the drive, as MTP offers it | approval | approval | open | through the second processor | on the one processor |
| `/os` | the installed OS: `boot1` and `boot2`, each boot partition's header sector and the image it states (the header alone when it states none) | approval | never: the OS write action | approval | through the second processor | on the one processor |
| `/user` | the OS's FAT volume on the eMMC's user area, read and written with FatFs | approval | approval | approval | yes | not offered: the OP-1 Field has no FAT region |

GET reads by offset, so a broken transfer goes on where it stopped. A put writes to `<path>.wbpart`,
from PUT_OPEN's offset 0: each PUT_DATA's offset must be the unit's `have`, which every reply gives. A
PUT_OPEN of the same path, size and SHA-256 while that put is open goes on from where it stands. PUT_END
puts the file on the eMMC (not its cache), reads it back whole, and renames it into place only when its
size and SHA-256 are those declared; otherwise the temporary file is removed and the old one stays.
`lost+found` cannot be removed or renamed. Every change is on the eMMC before the reply.

`/user` is the OS's FAT volume as FatFs reads it: FAT12, 16 or 32. This build has no exFAT, and a volume
it does not read answers status 16. A put there, a removal, a directory and a rename each reach the
eMMC, its cache flushed, before the reply; a put cut short leaves at most its `.wbpart` file, which a
later session lists and removes. Nothing formats the volume or writes its partition table, and its
sectors are the only ones reached: one bound in the block device under FatFs refuses any range outside
it. While the host link is up the bootloader is the only thing running, so the volume is not shared with
the OS and needs no rule for that. Protected writes covers the bootloader slots and the unit's identity,
not files on a drive (MTP's writes to the drive do not look at it either), so it does not apply to
`/user`.

## Actions

| Action | Id | Arguments | What runs |
| --- | --- | --- | --- |
| install | `01` | `parts:1 path` | the package on the drive, as the upgrade screen installs a file: its checks, the parts chosen, Protected writes as at the unit |
| OS write | `02` | `path` | the developer menu's OS write, from that file: its checks, into the boot partition the OS is not in |
| start once | `03` | | the developer menu's 3: restart once into `/yaffs2/os.ldr`, writing nothing |
| restart | `04` | | the plain restart |
| continue to OS | `05` | | Continue to the OS (as 7 on the main screen) |
| restart into a mode | `06` | `mode:1` | as the firmware command's `EE`: 1 the upgrade screen's disk, 2 the serial port, 4 reason 50, else the upgrade screen over MIDI |
| factory reset | `10` | | never |
| Protected writes | `11` | | never |

`parts` is one bit per part of the install's list: 1 content, 2 OS, 4 bootloader, 8 BLE, 16 BLE
bootloader, 32 keyboard; `FF` every part the unit would offer. A part the list would not let the unit's
keys pick (the bootloader with Protected writes off, a part that is up to date or switched off) is not
run. A package whose signature does not match installs without the unit's warning: the approved session
is the consent. The job's text then ends in `(signature invalid)`, and the unit's log says so too.

One job runs at a time. RUN answers a job record at once (state queued); the job starts when the unit's
loop comes round, and while it runs the unit sends an EVENT each time its record changes. Requests sent
during a job are answered after it. JOB asks for the record at any time.

**Job record**: `id:1 action:1 state:1 step:1 percent:1 error:1`, then a short text and `00`. States: 1
queued, 2 running, 3 done, 4 failed. Percent `FF`: not known. Error: a status code. A restart's record is
done with the text `restarting` just before the unit goes.

## The older paths

The firmware command's transfers (`0D 02`, `03`, `05`) on CDC and over MIDI, the MIDI firmware transfer,
the restarts `0D 18`, `0D 19` and `0D EE`, factory unlock and set metadata all need Host link on and the
approved session. Without it they are refused with `84` and a text (`host link off`, or `not allowed: hold
1 on the unit`), or status `01` and the text in the TE form, and they change nothing; a refused one also
puts the approval screen up, so a tool that tries again after the hold goes on. An install that comes
that way still asks for its parts on the unit, after the warning for a package whose signature does
not match. The same holds for these asked for through the Nordic's
link; the PSoC's link, the unit's own, is not asked. While a PSoC request holds the unit every one of them
is refused with the text `busy: a computer has the backplane`, approved or not.

The log (S-14) goes to the serial port only after the log-configuration request `B0`, which needs Host
link on and the approved session like the changes above (without it the request is not taken); the log
stops when the session ends. Frames a computer sends past the master, to the PSoC, the Nordic or the
second processor (S-06.050's forwarding), are dropped without the approved session, but for a ping
(`01`) or an info request (`09`), which only read; what those send back to the computer is not held.

## The PSoC bridge and reset

The PSoC (the backplane: keys, knobs, LEDs, the power switch and the battery) runs its own application and
has its own serial bootloader. Two requests let a computer reach it through the master, to load a new PSoC
application or recover one whose application is bad, with the computer's own tool for that bootloader's
protocol. `tools/hostlink.py` carries the bytes; it holds no uploader.

**The warning.** PSOC_OPEN and PSOC_RESET need Host link on, but no hold and no approved session: the
keys come from the PSoC. The unit puts a warning over the screen it was on (Main Menu, Settings, Display,
System, Device Info or the developer menu, with nothing under way; otherwise the request answers status 10)
and counts down 15 s:

```
Backplane Link
  A computer asks for the backplane link.
  Keys stop working until the computer ends it.
  ! Switch the unit off now to stop it.

  Starts in 15 s.
  8 Cancel
```

The reset's says `Backplane Reset`, `A computer asks to reset the backplane.`, `The whole unit switches off
and starts again.` and `Resets in 15 s.`. **Switching the unit off stops either one.** The unit's own 8
cancels it too, while the keys work; a key a computer sends does not. PSOC_CLOSE, END, Host link turned off
and USB unplugged cancel it as well. Nothing in the countdown waits for a key or the PSoC: a unit whose PSoC
did not answer at power-on (it opens the boot menu) counts down and opens the bridge all the same.

**The bridge.** Once the countdown is over the master sends the PSoC's bootloader-entry message `0xEE`
(with `entry` 1; send 0 for a PSoC already in its bootloader) and puts the link in raw mode. From then the
computer does the whole bootloader protocol: each PSOC_DATA's bytes go to the PSoC as they are, whole or
not at all (status 10 and nothing written when there is no room), and each reply carries what the PSoC has
sent since the last one, in order. The unit holds up to 2 KiB between requests; a reply that cannot carry
all of it says how much is still held. A byte the unit could not hold is counted, never dropped silently.
Nothing of the master's own goes onto the link while it is raw, and nothing else reaches the PSoC. The
screen then says:

```
Backplane Link
  A computer has the backplane link.
  Keys do not work until the computer ends it.
  To end it here, switch the unit off.
```

No key acts on it. While the link is raw the bootloader reads no PSoC message, so it does not see the
switch; what the PSoC does when it is switched off then is up to the PSoC.

**The end.** PSOC_CLOSE (also END, Host link off, or USB unplugged) frames the link again, greets the PSoC
and says in the record whether it greeted; PSOC_CLOSE's reply carries the PSoC's last bytes. **The unit
sends no exit packet of its own**: to start the PSoC's application, send the bootloader's exit packet
through the bridge before PSOC_CLOSE. A PSoC that greets later is answered as at any time.

**The reset.** PSOC_RESET's countdown ends in the plain restart, the restart action's, whose first step is
the PSoC reset request (`97 DE AD BE EF`). The restart goes through the PSoC's power, **the master's
included**: the whole unit restarts and the session ends. The record says `resetting` just before. A PSoC that has not greeted in this start
may not take that request, and the unit would then wait with its screen dark, so the reset is refused then
(status 15): use the bridge.

**While either is under way** the host link serves only SESSION, END, INFO, JOB and the PSoC calls; the
rest answer status 10 (busy), a never one still 3. The older paths are refused, no frame from a computer is
forwarded to the PSoC, and of the developer requests only `ping`, `rd`, `dev-screen` and `dev-shot` answer.

**The PSoC record**: `state:1 what:1 seconds:1 flags:1 overruns:4 held:2`. States: 0 none, 1 counting
down, 2 open, 3 canceled, 4 ended, 5 resetting. What: 1 the bridge with the entry message, 2 the bridge
without it, 3 the reset. Seconds: left of the countdown. Flags: 1 the PSoC greeted when the bridge ended, 2
it had greeted when the computer asked. Overruns: the PSoC's bytes lost since the bridge opened. Held: its
bytes the unit holds after this reply. The unit sends PSOC_EVENT with the record at each change of state and
of the seconds, on the link the request came on.

From a terminal:

    tools/hostlink.py psoc-open               # the warning and countdown, then the bridge, 0xEE first
    tools/hostlink.py psoc-open --no-entry    # the same, nothing sent first
    tools/hostlink.py psoc-raw-tx '01 38 06 00 ...' 0.5   # send, print what comes back in 0.5 s
    tools/hostlink.py psoc-close              # framed again; whether the PSoC greeted
    tools/hostlink.py psoc-state
    tools/hostlink.py psoc-reset              # the whole unit restarts

From Python, for an uploader of your own:

```python
from hostlink import Client, SerialPort, PsocBridge, find_port

b = PsocBridge(Client(SerialPort(find_port())))
b.open(entry=True)              # PsocError if the countdown was stopped
reply = b.exchange(packet, timeout=1.0)
greeted = b.close()
```

## A site installs a package

The page has the package as bytes, and the unit shows the boot menu.

1. HELLO: `05 21 01 7c 03 04 01 01 d1 00` (call `00`, tag `01`, version 1). The reply says Host link is on,
   version 1, the device, and lists the calls, the install action and the root `/yaffs2`.
2. APPROVE: `08 21 02 7c 02 02 02 28 00`. The reply's state is 1, pending: the page shows "hold 1 on the
   unit". The page asks SESSION (`08 21 03 7c 02 01 03 63 00`) every quarter second until the state is
   2, approved (3 refused, 4 timed out).
3. PUT_OPEN `/yaffs2/pkg.bin` with its size and SHA-256, then PUT_DATA in 224-byte pieces from offset 0,
   each at the offset the last reply gave, then PUT_END. Status 11: the file did not arrive whole; put it
   again.
4. RUN install, parts `FF`: `19 21 09 7c 13 20 09 01 ff 2f 79 61 66 66 73 32 2f 70 6b 67 2e 62 69 6e 63
   00` (call `20`, tag `09`, action `01`, parts `FF`, the path). The reply is the job's record, queued.
5. EVENTs follow: `copying` with its percent, `installing`, the package's phases, then `done` with
   `[done]` or `[nothing to do]`, or `failed` with the reason. The unit's screen shows the install as
   for a package from the computer.
6. END, or leave the session to end at the restart.

`docs/web/hostlink.js` does each step: `hello()`, `requestApproval()`, `put()`, `runAction(A.INSTALL, {
path, parts, onProgress })`. From a terminal: `tools/hostlink.py approve`, `put pkg.bin /yaffs2/pkg.bin`,
`install /yaffs2/pkg.bin`.

## Developer requests

With **developer mode** on and the session approved, the boot menu also answers the developer requests
(S-10c.110, id `0x7D`), sent with `tools/testos_mon.py`. Without developer mode every one is refused; with
it but without the approval every one but `ping` answers status 4 (`not approved`) and changes nothing.
`ping`, `rd` and `wr` work as with the test OS's monitor ([Test OS](Test-OS#the-usb-monitor)).

> **Warning.** A hand-over with options can leave a unit that only the other slot brings back. Try
> hand-overs only with a working bootloader in the other slot, and start that slot from the recovery menu
> (Power + M4, then 1, Change the boot slot; [How to Recover](How-to-Recover#start-the-other-slot-once-and-keep-it)).

Developer mode: Boot menu → **2 Settings → 1 Developer mode: On** (S-15.172). The serial port is then in
every USB set, MTP included (S-10a.010). Turning MTP on or off re-enumerates the device: reopen the port.

### Files over MTP

The developer menu's **2 MTP file transfer** shares the internal drive read-write (S-10b, level W). A
computer's file manager, or `tools/mtpcp.py` (`ls`, `put`, `verify`, `rm`, `mkdir`, `mv`, `tree`), can copy,
delete, move and rename files and folders and set their dates. Each change is flushed to the eMMC before the
computer is answered. `lost+found` cannot be deleted, moved or renamed; a name that exists is never
replaced, so a copy over an existing file is refused (delete it first, as `mtpcp.py put` does). Nothing
outside the drive can be reached, and the bootloader's own files on it (`os.ldr`, `upgrade/`) are not protected.

### Requests

| Request | What it does |
| --- | --- |
| `ping`, `rd ADDR`, `wr ADDR VALUE` | Is it there; read or write the master's registers |
| `dev-srd ADDR`, `dev-swr ADDR VALUE` | Read or write a slave register over the bridge |
| `dev-l1rd ADDR [WORDS]` | Read L1 instruction memory (`0x11A00000`…) through ENX |
| `dev-flags [MASK]`, `dev-handover [MASK]` | Read or set the hand-over options, then hand over the OS with them ([below](#trying-a-hand-over)) |
| `dev-put FILE [--as NAME]` | Write FILE to `/yaffs2/os.ldr` (or NAME) by the host link's put, checked whole before it takes the name; asks for the approval first. The developer menu's OS write (5) then uses it |
| `dev-get [NAME]` | The file's SHA-256 and size, by the host link |
| `dev-yscan OUT.json [--obj ID]...` | Read the yaffs2 volume's tags and the root's headers; read-only, no file contents |
| `yshow OUT.json [--obj ID]...` | A saved scan's summary, offline |
| `dev-cal-start` | Restart into one OS start with the calibration screen where the OS draws its first screen (S-04.012, S-05.016) |
| `dev-screen [--json]` | The screen's name, title, rows and styles, MTP state, USB set and flags such as `armed` |
| `dev-key KEY...` | Press keys: `1`–`8`, `white`, `back`, `help`, `shift`, a key number (`0x12`), or `KEY:down`/`KEY:up`. Keys queued behind a long operation act after it |
| `dev-shot OUT.png` | The frame buffer as a PNG |
| `dev-start` | Why this start's OS start failed ([below](#why-the-os-did-not-start)) |
| `dev-stream` | How this start sent the slave its boot stream ([below](#the-slaves-boot-stream)) |
| `log [S]` | The log for S seconds (default 10), including an install's failed step and cause; asks for the approval first |
| `nordic-info` | The Nordic's `mode`, `sw_version`, `bl_version`, `boot_slot`; changes nothing, needs no approval |
| `continue-os` | Restart the unit so that its next start boots the OS (S-10c.076), even with Start in boot menu on. Needs Host link and the approval; `continue-os --sysex` prints it as a MIDI SysEx message for a unit without a serial port |
| `dev-bench`, `dev-check`, `dev-extract` | Time the firmware package check and extraction ([below](#timing-the-package-check)) |
| `dev-link-record`, `dev-link-test [N]` | The bridge link's record and its bench ([below](#the-bridge-link)) |
| `dev-quad-data` | How this start's OS copy used quad data replies ([below](#quad-data-replies)) |
| `dev-quad-reply [N] [options]` | What ends a reply sent in quad I/O, tried on the unit ([below](#the-quad-reply-test)) |

- A file request while the slave is not answering fails with status 3 (`the slave is not answering`), not
  1 (refused volume): restart the unit.
- `continue-os` refuses during an install or OS write, and before the boot menu is up.
- `dev-key` cannot answer an install's list of parts or the approval screen, which read the unit's own
  keys. A key a computer sends never starts the factory reset and never changes Protected writes; what else
  erases still waits for its key to be held on the unit.

The requests' payload is an opcode byte and its little-endian arguments in frames of id `0x7D`, framed as
[above](#framing); the reply's payload starts with a status byte: 0 ok, 1 refused (developer mode off, or
the access refused), 2 malformed, 3 the slave is not answering, 4 not approved. `frame()` and `crc8()` in
`tools/testos_mon.py` are the reference; opcodes and reply layouts are in `src/include/waffle/devhost.h`.

### Hand-over options

A bitmask. **Zero is the normal hand-over**, as a boot does it (S-05.023). Each bit changes one step of it,
to find which piece an OS needs.

| Bit | When set |
| --- | --- |
| `0x001` | Leave the slave's MSI controller as found |
| `0x002` | Leave every SEC source as found |
| `0x004` | Leave SP and FP as the kernel left them |
| `0x008` | Leave EVT2–EVT15 as they are |
| `0x010` | Leave DMA20/DMA21 running |
| `0x020` | Set `CRC1_CTL` to S-05.031's value |
| `0x040` | Leave the USB controller running |
| `0x080` | Leave the timers running |
| `0x100` | Leave the CPLBs and caches off at the jump |
| `0x200` | The slave's teardown is interrupts off alone |
| `0x400` | Do not release the slave |
| `0x800` | Trace: report each milestone over USB; the last report before the port dies is the hang. Also prints PA6 (slave ready) and PC5 (bridge attention) for 3 s after the slave's OS started |
| `0x1000` | Skip the PMIC, `0x51` and logo redraw before the load |
| `0x2000` | Do not wait for the slave's OS before the teardown; with `0x800`, the line window starts at the release |
| `0x4000` | Unused, ignored |
| `0x8000` | The slave hands PA4 to its disabled SPI2 at its server stop (S-05.031); add `0x2000` |
| `0x10000` | Watch the lines 10 s from the release, then stay in held mode (no teardown, no jump); the bridge is gone, `rd` still works |
| `0x20000` | The slave pulses PA4 at each step; with `0x10000` (`0x30000`) printed as `slave steps: 1 teardown, 2 MSI done, 3 SEC done, 4 entry set, 5 delay over, 6 first load call, 7 kernel returned, 8 before the jump` |
| `0x40000` | SPI0 off with the `OPXY_HO_SPI0_*` values; `SPI2_CTL` is `0x2` in every hand-over (S-05.016) |

### Trying a hand-over

Needs developer mode, the approved session (`tools/hostlink.py approve`), a valid plaintext OS in the active partition, and the boot menu (hold com or the
white knob while switching on, or a start whose load failed).

1. `tools/testos_mon.py dev-flags` shows the options (`ping` first confirms the port).
2. `tools/testos_mon.py dev-flags 0x010` sets them.
3. `tools/testos_mon.py dev-handover` hands over with them; `dev-handover FLAGS [MD] [SD]` passes a mask and
   ms delays before the master's and the slave's jump (e.g. `dev-handover 0x30000 0 1000`). The port goes
   away if the OS was entered or the unit hung, and stays if it returned to held mode.
4. If the OS did not come up, start a slot that reaches the boot menu from the recovery menu, set another
   mask and retry. No reinstall is needed.

If the slave's OS does not start within `T_OSSTART`, the hand-over returns to held mode with code 5, the
bridge gone (S-05.032). Nothing survives the recovery reset, so read a trace live.

### Why the OS did not start

`dev-start` (request 37) prints the same account as the main screen and Device Info's `OS start` row
(S-15.163), at any time later in that start:

```
last plain start: The OS could not be started.
  reason:   DSP2 stopped answering during the image read
  OS start: Sector 1281 read: 0x12 reply broke
  load:     a select or read failed; cause: a failed select or read (S-05.007)
  step:     an image read of partition 2, 128 sector(s) from 1281
  header:   format 3, length 742764 bytes, CRC deadbeef; the partition 1024 sectors
  bridge:   the slave was lost on request 0x12 (arg 0x501): the reply broke off [the reply ran out its bound; rc -5, status 0x0, 3 words left]
```

A refused image adds `refusal` (S-05.024), a failed release adds `release`. A start from `/os.ldr`
(S-05.035) begins `/os.ldr could not be started.` and adds `file`, the file's length and its copy's CRC. With no failed start it prints
`the last plain start did not fail, or none was tried`. Layout: `opxy_start_report()` in
`src/include/waffle/board/opxy_services.h`.

### The slave's boot stream

`dev-stream` (request 40) prints how this start sent the slave its boot stream, at any time in that start,
in every build but a byte-by-byte one, `BOOT_LINK_FC=0`, which answers `bad request`
([Test OS](Test-OS#the-slaves-stream-by-the-controllers-flow-control)). It reads only. For example:

```
this start's stream: by the controller under flow control, fed by DMA
  outcome:  streamed, ended at the FINAL block; 120.500 ms from the wait for the ready line
  bridge:   linked
  sent:     412 sessions, 437 parts by DMA, 421376 bytes by the controller, 0 one at a time
  pacing:   guard 200 us, 85.547 ms in the guards; 3 parts began with the line high, 0.030 ms waited, the longest 12 us
  held:     SPI_STAT.FCS set in 2400 of 91000 samples, in 380 parts, the first at byte 16
  fell back: no
```

After a fall-back it names the cause, the session and the byte, and the driver's account of the part that
failed. Layout: `opxy_stream_report()` in `src/include/waffle/board/opxy_boot.h`.

### Timing the package check

| Request | What it times |
| --- | --- |
| `dev-bench [KIB]` | Each check stage over KIB KiB (default 1024), and whether the packet engine and CRC peripheral are used |
| `dev-check` | The whole check (S-11.020) of the package on the USB disk; `checks 1-14 passed` means it agreed |
| `dev-extract` | The check, then extraction (checks 16 to 18) with nothing staged, and the bridge's cost per request; then the link's record |

For `dev-check` and `dev-extract`, copy the package, run `sync`, and **do not eject** (an eject starts the
install). Nothing is written, so the package still installs. All three are refused during a long
operation, a scan or install, or a host-link transfer.

`dev-extract` raises the bridge first as an install does (S-09.075): to single I/O at 50 MHz (rung 2), or
the fastest rung below it that works. It lowers it to plain mode at the end (S-09.078). USB is not serviced during the link tests, usually under a second. If the tool says the
slave is absent, installs are refused until a restart; restart too if it says the slave could not be told
the link is plain.

### The bridge link

| Request | What it does |
| --- | --- |
| `dev-link-record` | The link's record for this start (S-09.070 to S-09.079): the last negotiation per rung, whether the slave is settled (if not, restart), the last 16 fall-backs, the last link test's faults |
| `dev-link-test [N]` | N tests (default 16, 1 to 255) of each length from 12 bytes to 64 KiB on rungs 1 to 4 and plain mode; a table of passes, faults and speed against plain (under 1 is faster) |

An install raises the link no higher than rung 2; the quad I/O rungs 3 and 4 are run only by
`dev-link-test` and `dev-quad-reply`. `dev-link-test` needs no package, writes nothing to the eMMC and sends only echoes, `0x19`, `0x1A` and
`0x1B`. The default run sends about 17 MB in about 6 s, plus about 1 s per fault, bounded at 90 s. It is
refused as the timing requests are, and without the slave. If the slave could not be told the link is
plain at the end, restart. Reply formats: `src/include/waffle/devhost.h`.

### The quad reply test

Replies over the bridge travel in single I/O, but for the sector data of a start's OS copy
([below](#quad-data-replies)). `dev-quad-reply` tries the candidate ends on the unit itself (S-09.080,
S-13b.084): for each, the slave sends test replies in quad I/O, ends each one on that
candidate's flag, and keeps a record of what its SPI controller showed; the master checks every
word of each reply and then fetches the record in single I/O.

    tools/testos_mon.py dev-quad-reply [N] [--cand tf-dma,tf-core,spif] [--ren 0,1]
                                           [--clock 25,50,25f,50f] [--len 12,64,...] [--json]

| Option | Default | Meaning |
| --- | --- | --- |
| `N` | 16 | Test replies per cell, 1 to 4096 |
| `--cand` | all three | `tf-dma`: the transmit-finished flag, fed by DMA; `tf-core`: the same, fed by the core; `spif`: the SPI-finished flag, read by a loop that reads nothing else |
| `--ren` | `0,1` | The receive channel off and on, for the `tf-` candidates |
| `--clock` | all four | The master at 25 or 50 MHz, `f` with its fast mode on |
| `--len` | 12 to 65536 | The reply lengths, in bytes, whole 32-bit words |

Each cell (a candidate, the receive channel, a clock and a length) is one request to the unit, bounded at
120 s, so the default run is 140 cells. The table, per candidate and clock, one line per length:

| Column | Meaning |
| --- | --- |
| `trials` | Test replies sent |
| `whole` | Replies whose every word was right, the last included |
| `flag` | Replies the candidate's flag ended |
| `missing` | Replies whose flag did not come within the bound (30 ms); the reply was given up |
| `last word` | Replies whose last word alone was wrong: a flag that came before the last word had left |
| `a word` | Replies with another word wrong |
| `broke` | Sessions that did not complete; the bridge was brought back and the run went on |
| `delay cycles` | The flag's delay after the last word was fed, least and most, in the slave's cycles (400 a microsecond) |
| `lines` | The data lines (D0 to D3) of every wrong bit: one line alone points at a stuck or open line |
| `master RF` | Whether the master's receive-finished flag was set at the end of every reply |

The first failure in a cell is printed under it with the slave's record: how the reply ended,
`SPI_STAT`, the transmit word count and the DMA status at the end, and the times from the hand-over of
its ready line. A verdict per candidate follows: `ended every reply and every reply was whole`, or where
it failed first. `--json` prints every cell and the verdicts as JSON.

The test writes nothing to the eMMC or the flash and sends nothing but its own requests; a session that
breaks is followed by the bridge's usual way back (echoes and polls). Both processors are back in single
I/O after each reply, and the run ends with the link plain and the slave answering. If the
tool says the slave stopped answering, or did not answer after a cell, restart the unit. It is
refused as `dev-link-test` is. Reply format: `OPXY_DEV_QUAD_REPLY` in `src/include/waffle/devhost.h`.

### Quad data replies

A start copies the OS image with its sector data in quad I/O at 50 MHz when the quad reply test passes
first in that start (S-09.081): eight test replies of 1 KiB and 64 KiB under the transmit-finished flag
fed by DMA, the master at 50 MHz with fast mode, each checked word by word. Then the link is raised to
its usual rung and both processors agree on the mode with a `0x19` exchange; each sector read's reply
is a short head in single I/O followed by its data in quad I/O, checked against the head. Any failure
drops the rest of the start to single I/O replies and reads that sector range again; a copy whose CRC
fails after quad data is read once more in single I/O. After the copy the link is lowered: the
installer, MTP, the disk and the file commands never see quad data.

`dev-quad-data` (request 41) prints how this start's copy went, at any time; it reads only:

```
this start's quad data replies: taken up, ended with the link
  test:     8 of 8 test replies whole with their flag, 41.250 ms
  data:     37 sessions in quad I/O at 50 MHz, 2359296 bytes; 0 failed
  link:     rung 0 now
```

Reply format: `OPXY_DEV_QUAD_DATA` in `src/include/waffle/devhost.h`.

