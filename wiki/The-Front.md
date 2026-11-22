The front is the small first part of every waffle-boot image. On every start it brings up the clocks and
memory, draws the logo (a stored [Boot Logo](Boot-Logo), or its own), opens the PSoC link (power and keys) and
reads the keys once. Then it enters recovery ([Recovery Reference](Recovery-Reference)), or checks the rest of
its image and starts it (S-01.023): the stub, which unpacks the main image ([Boot Chain](Boot-Chain#the-steps)).

It runs in memory of its own, is linked alone, and shares only the hand-off record's header with the main
image, so it still runs when the rest of the image is damaged. The rule behind every failsafe: no write
removes the last image the unit can start, except an in-place write the user asks for in so many words.

## Changing it

A front that fails before its key read takes recovery with it, and only an external flash programmer
brings the unit back. So the SHA-256 of its loaded bytes is pinned in `src/front/front.pin`, and every cross
build fails when they change. A change needs the maintainer's sign-off. Then `make front-pin`
records the new pin, the `host`, `cross`, `repro` and `conformance` jobs must pass, and the new front goes
through a recovery round on a unit ([Recovery Reference](Recovery-Reference)) before that build is committed to a slot.

Every cross build also checks (`tools/front_image.py` and the stream tools):

- the front is compiled only from allowed files, nothing else includes its headers except the hand-off
  record's, and its link fails on any outside symbol or unplaced section;
- the INIT stage touches only on-chip L1 memory (S-01.010);
- every stack frame is fixed and at most 1 KiB, and twice their sum fits the 16 KiB stack;
- the image and the front alone pass the boot ROM's rules (S-02.091), no main-image or stub byte lands
  in the front's memory, and the image is at most `0x76000` bytes;
- the packed main image inflates back to the linked image, in Python and with the stub's own code.

## The flash

The full map, with every writer, is in [Flash Layout and Write Safety](Flash-Layout-and-Write-Safety).


| Offset | Holds |
| --- | --- |
| `0x00000` | The first stage, which picks a slot on every start. It reads no keys; the front never writes it |
| `0xD000` | The selector: the slot to start, and `verified` (`0x01`: start it without a check) |
| `0xE000`, `0xF000` | One record per slot: the image's CRC-32, length and a tag |
| `0x10000` | Slot 1: an image of at most `0x76000` bytes from waffle-boot's writers, recovery included; then its settings sector and, last, its start log. A stored boot logo's header is at `0x85000`, its data below it, in the room the image leaves |
| `0x88000` | Slot 2: the same; its settings sector is `0xFE000` |

A slot whose image is over `0x76000` bytes has no settings (S-02.096 to S-02.099): its settings sector holds
image bytes, every setting reads at its default and a save is refused. Over `0x77000` it has no start log either.

## The failsafes

- **Writes spare the running slot.** A write goes to the slot the selector does not name, in order: image,
  last sector, record, read-back of the whole slot, and only then the selector, naming the new slot
  unverified. A cut before the selector changes nothing. Every selector written names a slot read as checked
  in that start, except a trial's (S-02.047 to S-02.052).
- **The first stage checks a new slot.** On a start with `verified` not `0x01`, the first stage checks the
  boot slot's record and CRC and writes the selector. A pass trusts the slot; a failure names the other
  slot as trusted: the failed slot starts this once, the other from then on (S-02.021 to S-02.023).
- **The front checks its own image** (descriptor and CRC-32) and enters recovery if either fails
  (S-01.023).
- **The start log.** Each start marks a bit in the boot slot's last sector; the main image clears
  another once it is up (menus, battery wait, boot prompt, or an OS staged and checked). Three unfinished
  starts in a row open recovery. The front never erases the log and times nothing, so a slow install
  cannot trigger it (S-02.095). The main image erases the log on a normal start once 256 or more entries are
  used, and when it reads full or not valid, so the count never stops (S-02.101).
- **One chord.** M4 alone at power-on opens recovery and writes nothing. Every other key set is no
  chord, and an unreadable key counts as not pressed (S-02.080, S-15.150).
- **Recovery writes only from a known place**: its slot established, the boot slot, trusted, with an intact
  front. It starts no second processor, does not touch the eMMC and reads no settings (S-02.084).
- **The logo, read before anything else.** While the panel comes out of its reset, the front reads the boot
  logo of the slot it runs from (the one slot whose stream holds the loaded descriptor, else the slot the
  selector names): the header, then its data into recovery's staging buffer, a page a read, and draws it whole
  into the frame buffer before the first frame goes out. It draws its own logo instead when anything is not
  right: the header's magic, version, size or CRC, a size out of range, the image CRC not the slot record's,
  the data not lying wholly between the image's end and the header, the data's CRC, or a run of the lines
  that leaves its line or the data. Every loop is bounded by the header's sizes, checked first. The hand-off
  record says which slot's logo it drew, and the main image reads that one again for its hand-over frame, so
  both show the same picture. Recovery's screens never show it; `Loading recovery menu...` goes under
  whichever logo the start drew.
- **Caches for its own work, none at its exit.** On the main processor the front turns the L1 caches on once
  its INIT stage has the clocks and DDR up: the instruction cache for its code in L2, the data cache
  (write-through, in the upper half of L1 data A) for L2 and the DDR range that holds the main image's DDR part
  and recovery's buffer. Every other address keeps CPLB defaults that raise no exception and cache nothing,
  the panel's frame buffer, the one buffer a DMA reads, among them. Before its one exit, the jump to the main
  image, it turns both off again and puts back every register it changed, so the main image starts as it
  always has. The second processor's front leaves the caches off.
- **The running slot, quickly.** Once the main image's check has passed, the front first reads only each
  slot's image descriptor. When one slot's is the loaded one and the other's differs, as with two builds,
  or the other slot's whole stream carries no descriptor, as with another bootloader, that slot is the
  running one and no stream is read. Otherwise, as when both slots hold the same file, the
  front compares memory with each slot's whole stream, as below, which adds about 2.7 s to that start.
  Recovery always compares the whole streams.
- **Both slots may hold the same image.** The front tells its slot by comparing memory with each slot's
  stream. When both match whole, either one is this image, and the front counts as its own the slot the unit
  goes on starting: the one the selector names when it is trusted or checked, otherwise the other one. A write
  then keeps exactly the slot a start after a power cut runs. The main image names the same slot in its
  status bar and streams it to the second processor. A trial of an image the kept slot also holds,
  byte for byte in the flash, is marked started at the next start, whichever slot the boot ROM read
  (S-02.054, S-02.095).
- **Recovery checks before it erases**: size, the boot ROM's rules, the announced SHA-256, and a front
  matching its descriptor and the one the unit runs. A state change during
  the transfer refuses with nothing written (status 16). The read-back must match before the selector is
  touched (S-02.084, S-02.093). Every image it writes erases the slot's settings sector too, so the new
  image starts with its settings at their defaults (S-02.103), and, where the image leaves it free, the boot
  logo's header sector, so the new image starts on the built-in logo.
- **The running slot** is written only on request. With the other slot checked, it is made the boot slot
  first. With its check failed, the write goes in place with no fallback: only after a plain write of the other slot
  failed, after the typed phrase, and with the power switch held off. After a failed write of the running
  slot, recovery re-reads the flash before every answer, never erases a selector that leads to a checked slot,
  writes the selector a byte at a time, and refuses to restart while no start would reach a checked slot.
- **A new front only by trial**, with the running slot checked. The tried slot gets a record that fails on
  purpose and a pending start-log entry; the first stage starts it once and goes back. Nothing selects it
  until its front has marked that start, or its own recovery selects it during the trial start. The
  installer follows the same rules ([Installer Reference](Installer-Reference)).
- **The power switch** works at every point of a start and in recovery. The power-off is held only during an
  in-place write, and after a failed write of the running slot while no start would reach a checked slot
  (S-02.084).

## What is not covered

Any of these can leave a unit that only an external programmer on the SPI flash brings back:

- a cut during an in-place write, or after a failed write of the running slot before the selector is
  settled (whether the PSoC honors the held power-off is not established);
- a cut during the selector erase at the end of a trial write to Slot 2, or in the first stage's selector
  write during its trial start, if that front then also fails before its key read;
- a front that fails before its key read, or one that passes its trial start and fails later;
- an image that passes the stream check (block headers only) but hangs, since a trusted slot starts on
  every reset;
- the first stage, which nothing here repairs. The bootloader does not write it: no install has a
  first-stage step.
