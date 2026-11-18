From power-on to the OS, on the master. Labels: S-01, S-02, S-05.

```
Power switch on
  -> Boot ROM            loads the first stage from flash offset 0
  -> First stage         reads the selector, picks Slot 1 or Slot 2      (not part of waffle-boot)
  -> Front               waffle-boot's first part: clocks, memory, logo, PSoC, keys
       Power + M4  ------> Recovery menu
       image damaged ---> Recovery menu
       3 unfinished ----> Recovery menu
  -> Main image          the rest of waffle-boot
       boots the slave, decides: OS or boot menu
       keys held, failed start, setting on -> Boot menu
  -> OS                  loaded from the eMMC's active boot partition, on both processors
```

## The steps

1. **Boot ROM.** Loads the first stage from offset 0 of the SPI flash.
2. **First stage.** Not part of waffle-boot; waffle-boot relies on what it does on every start (S-02.020
   to S-02.023). It starts the slot the **selector** names. A trusted slot (`verified` = `0x01`) starts
   without a check. Otherwise the first stage checks the slot's record and CRC and writes the selector: a
   pass trusts the slot, and a failure trusts the other slot while the failed one starts this once. Keys
   play no part in this step.
3. **Front.** The first part of every waffle-boot image, frozen and pinned. It sets up the clocks and DDR,
   draws the logo, greets the PSoC, waits while the unit only charges, and reads the keys once. Then:
   - M4 alone held: **recovery** (Power + M4);
   - the rest of its image fails its check: recovery;
   - three starts in a row did not finish: recovery;
   - otherwise it marks this start in the slot's **start log** and starts the main image.
4. **Main image.** Streams the running slot's image to the slave (by DMA under the SPI
   controller's flow control; a build option sends it byte by byte, [Test OS](Test-OS)), reads that slot's
   settings, and takes the boot decision. It enters the **boot menu** when the boot keys are held, when the OS asked for the bootloader,
   after an interrupted install, when **Start in boot menu** is on, or when the OS cannot be loaded.
   Otherwise it loads the OS.
5. **OS.** The master reads the OS image from the eMMC's active boot partition through the slave, shows
   the hand-over frame, and hands both processors to the OS.

The main image clears the start log's mark once it is up: at its menus, its battery wait, or an OS image
staged and checked. A start that never gets there counts as unfinished. On a normal start the main image
also erases the log, once 256 or more entries are used or when it reads full or not valid
([Flash Layout and Write Safety](Flash-Layout-and-Write-Safety#the-rules)).

## Slots and the trial start

There are two bootloader slots, shown as **Slot 1** and **Slot 2**. The selector names one. The running
slot is the one that actually started; it differs from the selected one only after a failed check.

A **trial start** runs a new image once without trusting it:

1. The new slot's record is marked so that the first stage's check fails on purpose, and the selector names
   it, not trusted.
2. At the next start the check fails: the first stage trusts the old slot again, but starts the new one
   this once.
3. If the new front comes up, it marks the start in its own log. That slot can then be **kept**.
4. If it does not come up, switch off and on: the old slot starts.

The recovery menu's **Try** ([How to Recover](How-to-Recover#start-the-other-slot-once-and-keep-it)) and the host tool's `try` and `write --new-front` use it.

## Read more

- [Flash Layout and Write Safety](Flash-Layout-and-Write-Safety)
- [The Front](The-Front): the front and its failsafes
- [Recovery Reference](Recovery-Reference)
