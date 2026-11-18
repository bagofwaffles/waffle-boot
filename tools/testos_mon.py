#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The host's end of the test OS's USB debug monitor (wiki/Test-OS.md).

    testos_mon.py [--port /dev/cu.usbmodemNNN] COMMAND...

    ping                 is the monitor there
    report               the text of testos.txt so far
    rd ADDR [WORDS]      read words of the master's registers or image
    wr ADDR VALUE        write a word
    load ADDR FILE       upload bytes to the top of the master's heap
    call ADDR [ARG]      run an uploaded routine, uint32_t f(uint32_t)
    run ADDR [ARG] [MS]  the same under the watchdog: a routine that has not returned in
                         MS (default 2000) has the PSoC reboot the unit
    last [clear]         a routine that never returned, from the note it left on /yaffs2
    stall [MS]           a test of the watchdog: run a routine that never returns
    send                 what the slave's last reply send saw, with TWC's timing
    q11 WHICH [N] [--elf PATH] [--json FILE]
                         S-13b Q-11's measurements, WHICH 1a to 1h or all, N exchanges each (default
                         16): armed replies and listens on the slave, read back over the bridge. Needs
                         the `mon` image on both CPUs and its ELF (default
                         build/bf703-testos/waffle-test-mon.elf) for the slave's addresses
    beacon [US]          the slave's attention line: burst lengths, 0 for a long gap
    init                 the bridge's init session, single I/O
    echo [ARG]           the echo request
    pull [LEN]           the pull request, reading its first word as held mode does
    req CMD ARG0 ARG1    any immediate request
    srd ADDR [WORDS]     read words of the slave, over the bridge
    swr ADDR VALUE       write a word of the slave, over the bridge
    dev-l1rd ADDR [WORDS] read words of L1 instruction memory (0x11A00000..) through ENX (developer mode)
    dev-put FILE [--as NAME]
                         write FILE to /yaffs2/NAME (default os.ldr) over USB, then verify its sha256
    dev-get [NAME]       the sha256 and size of /yaffs2/NAME (default os.ldr)
    dev-yscan OUT.json [--obj ID]...
                         read the yaffs2 region's tags raw, and the headers of the root's entries
                         (and of objects ID): block classes, sequence numbers, summaries, blocks
                         holding chunks of another sequence number, each root name's header
                         history. Read only; no file data. Writes OUT.json, prints a summary
    yshow FILE.json [--obj ID]...
                         the summary of a saved dev-yscan, and every chunk of objects ID
    dev-srd ADDR         a slave register, the master forwarding over the bridge (developer mode)
    dev-swr ADDR VALUE   write a slave register (developer mode)
    dev-flags [VALUE]    show the hand-over options, or set them (0 is the normal hand-over)
    dev-screen [--json]  the screen held mode shows: its name, title, rows, the MTP state and USB set
    dev-key KEY...       press keys, as the PSoC's key events do, and wait for the menus to take them.
                         KEY: a digit 1-8, white, back (digit 8), help, shift, or a key number (0x12);
                         KEY:down or KEY:up for half a press
    dev-shot OUT.png     the display's frame buffer as a PNG
    dev-bench [KIB]      time each stage of the package check over KIB KiB (default 1024): the
                         sector copy from the RAM disk, CRC-16, SHA-256, AES, one screen frame, and
                         SHA-256 and AES in software alone; and whether the packet engine and the
                         CRC peripheral are in use
    dev-check            time the whole check of the package on the RAM disk, stage by stage,
                         without writing the zip back: the package stays installable. Copy the
                         package and sync first, and do not eject (an eject starts the install)
    dev-extract          the check, then the extraction of the package on the RAM disk timed stage by
                         stage with nothing staged, and the bridge's cost per request from requests
                         that write nothing; prints an estimate of the extraction without the slave's
                         storage time, then the link's record. Writes nothing: as dev-check, copy,
                         sync and do not eject
    dev-link-record      the bridge link's record for the boot: the last negotiation rung by rung, the
                         last 16 fall-backs of requests and the last link test's faults (its first 12
                         and last 4), each with where it broke, the driver's words and the slave's
                         own account
    dev-start            why this boot's plain start failed, as the main screen, Device Info and the
                         load's log line give it: the step, the partition and sector, the header and
                         the image's CRC, the refusal, the release's reply, and for a lost bridge the
                         request, where it broke and the driver's four words
    dev-stream           how this start sent the slave its boot stream (a BOOT_LINK_FC build): the
                         way, the outcome, whether the bridge linked, the sessions, parts and
                         bytes, the time, the guards and waits, and a fall-back's cause and place
    dev-quad-data        how this start's OS copy used quad data replies (S-09.081): taken up or not
                         and why, the quad reply test before it, the data sessions taken whole in
                         quad I/O and their bytes, and a drop to single I/O with where it broke
    dev-ckpt             the read-only checkpoint test of the slave's volume: a second yaffs2 mount
                         in the master's memory restores the checkpoint read only, over the bridge's
                         sector reads, under a device that refuses every write; whether yaffs2 took
                         it and why not, its reads and time, its tree against the scanned mount's,
                         every block's chunk-0 tags against the checkpoint's block states and
                         sequence number, and the writes refused (0). Writes nothing
    dev-cal-start        restart into one plain start of the OS with the calibration screen in the
                         memory the OS draws its first screen over (S-04.012, S-05.016): photograph
                         the OS's first screen. The boot after it is black there again
    dev-link-test [N]    the link bench from held mode, no package needed: N link tests (default 16)
                         of each length from 12 bytes to 64 KiB on each rung, then plain, in one pass,
                         every session by DMA as requests go; a table of passes, faults by where
                         they broke, and microseconds per request and per 4 KiB; dev-link-record has
                         each fault. Writes nothing to the eMMC
    dev-quad-reply [N] [--cand tf-dma,tf-core,spif] [--ren 0,1] [--clock 25,50,25f,50f]
                   [--len 12,64,...] [--json]
                         the quad reply test from held mode (S-09.080): N test replies (default 16, up
                         to 4096) a cell, each sent by the slave in quad I/O and ended by one candidate
                         (TF fed by the DMA, TF fed by the core, or SPIF read alone), REN clear and set
                         for the TF candidates, the master at 25 and 50 MHz with fast mode on and off
                         (f), at each length from 12 bytes to 64 KiB; a table per candidate and clock
                         and a verdict per candidate. Writes nothing to storage
    dev-handover [FLAGS] [MD] [SD]
                         hand over the active-partition OS with FLAGS (default: the set options);
                         MD/SD are ms delays before the master/slave jump. With the trace option
                         (0x800) it prints each milestone; the last before the port dies is the hang.
                         0x800 prints PA6 and PC5 for 3 s after the slave's OS started, or from
                         the release with 0x2000 (no wait for the slave's OS); 0x10000 watches them
                         10 s from the release and stays in held mode; 0x8000 has the slave hand
                         PA4 to SPI2 instead of driving it low; 0x20000 has the slave pulse PA4 at
                         each step (use 0x30000: printed as steps)
    table                the slave's bring-up table
    emmc                 start the slave's eMMC bring-up, then the table
    crumbs HEX...        decode the breadcrumbs line of an installer's check.txt (a breadcrumb build,
                         BOOT_CRUMBS=1): its hex bytes, from byte 1 of the tried slot's last sector;
                         needs no unit
    encode OP HEX        print a frame's wire bytes, for a bench script
    continue-os          restart the unit from the boot menu so that the next start boots the OS
                         (S-10c.076, firmware sub-command 0D 19): the reply 54, then the unit shuts
                         down and restarts, so the port goes away; 84 and why when it cannot now
                         (an install or OS write under way, not in the boot menu)
    continue-os --sysex  print the same request as one MIDI carrier SysEx message (S-10c.020), in hex,
                         for any MIDI sender: a unit with developer mode off has no serial port
    log [S]              the bootloader's or the test OS's log on this port for S seconds (default
                         10): the log-configuration request turns its sink on (S-14.013). A
                         BOOT_TIMING=1 build repeats its timing report there in held mode
    nordic-info          the Nordic's own info text (mode, versions and, where it gives them, the
                         boot slot), asked through the bootloader's forwarding in held mode
                         (S-06.050). Sends one info request; changes nothing

Python 3, standard library only. Registers and our own image only: the monitor
refuses the rest, and refuses every write to the flash controller and the OTP.
"""
import glob
import os
import select
import struct
import sys
import termios
import time

ROUTE = 0x21            # S-06.010: the host to the master
NORDIC_ROUTE = 0x23     # the host to the Nordic: the master forwards it (S-06.050)
INFO, INFO_REPLY = 0x09, 0x0A   # S-17.021: the coprocessor info exchange
ID = 0x7D               # OPXY_TESTOS_MON_ID
PING, READ, WRITE, LOAD, CALL, REPORT, BUS, BEACON, RUN, LAST = range(10)
RUN_MAGIC = 0x52554E31
LOAD_TOP = 0x8A000000 - 0x10000  # a place for the stall routine
STATUS = {0: 'ok', 1: 'refused', 2: 'bad request', 3: 'the slave is not answering'}
ROW, PEEK, POKE = 0x0F, 0x0E, 0x0D
DEV_SREAD, DEV_SWRITE, DEV_HANDOVER, DEV_FLAGS, DEV_L1READ = 20, 21, 22, 23, 24  # developer requests (S-10c.110)
DEV_PUT, DEV_SHA = 25, 26
DEV_YTAGS, DEV_YMETA = 27, 28
DEV_SCREEN, DEV_KEY, DEV_FRAME = 29, 30, 31
DEV_BENCH, DEV_CHECK, DEV_EXTRACT = 32, 33, 34
DEV_LINK_RECORD, DEV_LINK_TEST = 35, 36
DEV_BOOT_START, START_REPORT = 37, 61   # OPXY_DEV_BOOT_START and OPXY_START_REPORT's bytes
DEV_CAL_START = 38      # OPXY_DEV_CAL_START
DEV_QUAD_REPLY = 39     # OPXY_DEV_QUAD_REPLY (S-09.080)
DEV_STREAM, STREAM_REPORT = 40, 88     # OPXY_DEV_STREAM and OPXY_STREAM_REPORT's bytes (S-09.009)
DEV_QUAD_DATA, QDATA_REPLY = 41, 46    # OPXY_DEV_QUAD_DATA and OPXY_QDATA_REPLY, its status byte included
DEV_CKPT, CKPT_SUMMARY = 42, 62        # OPXY_DEV_CKPT and OPXY_CKPT_SUMMARY, after the status byte
CKPT_TIMEOUT = 600.0                   # the test: a mount, a tree walk and every block's tags
QDATA_LAYOUT = '<BBBHHBIIIIIB4I'      # the reply after its status byte (S-09.081)
QDATA_STATES = ['not tried', 'not taken up', 'taken up', 'taken up, then dropped', 'taken up, ended with the link']
QDATA_WHY = ['', 'the bus has no quad receive', 'the quad reply test failed', 'the link is on no checked rung',
             'the slave did not take mode 3', "the mode's exchange did not get through",
             'a data session broke', "a data session's check word was wrong", 'another request fell back',
             'dropped for the read again of the copy', 'dropped earlier']
QDATA_FIRST = ['none', 'a session did not complete', 'only the last word was wrong', 'a word was wrong',
               "the candidate's flag missing", "the slave's record did not come"]
STREAM_REPORT_V1 = 72                  # version 1's, without the FCS counts
STREAM_WAYS = ['byte by byte', 'by the controller under flow control, fed by the core',
               'by the controller under flow control, fed by DMA']
STREAM_OUTCOMES = ['streamed', "the slot's CRC failed, nothing sent", 'the slave never showed ready',
                   'the slave did not open', 'the stream stopped part-way']
STREAM_ENDS = ['the FINAL block', 'a bad signature', "the slot's end"]
STREAM_CAUSES = ['none', "the slave's hold", 'a DMA error', 'the controller stopped with the line low',
                 'no end of the transmit with the line low', 'the driver refused the part']
QUAD_N, QUAD_N_MAX, QUAD_LIMIT_S = 16, 4096, 120
QUAD_LAYOUT = '<BBBIHBB8H8HIIBIIIQBHI4I18I'     # the reply after its status byte
QUAD_REPLY = 167
QUAD_LENS = [12, 64, 256, 0x1000, 0x4000, 0x8000, 0x10000]
# the candidates (S09_QT_...), the master's clocks (S09_QT_FAST 1, S09_QT_FMODE 2)
QUAD_CANDS = {'tf-dma': 1, 'tf-core': 2, 'spif': 3}
QUAD_CAND_TEXT = {1: 'TF, DMA feed', 2: 'TF, core feed', 3: 'SPIF, SPI_STAT alone'}
QUAD_CLOCKS = {'25': 0, '50': 1, '25f': 2, '50f': 3}
QUAD_FLAGS = [(0x01, 'THE SLAVE STOPPED ANSWERING: restart the unit'),
              (0x02, 'cut short by its bound (%d s)' % QUAD_LIMIT_S),
              (0x04, 'the slave did not answer after it: restart the unit'),
              (0x08, 'refused')]
# how a test reply ended on the slave (S09_QE_...) and what it saw (S09_QF_...)
QUAD_ENDS = ['no reply', 'the flag', 'the bound', 'the master ended it', 'underrun or collision',
             'a DMA error', 'the master never began', 'the driver refused it']
QUAD_SEEN = [(0x100, 'TF in SPI_STAT'), (0x200, 'TF in SPI_ILAT'), (0x400, 'fed whole'),
             (0x800, 'the master began'), (0x1000, 'the master ended'), (0x2000, 'single I/O after')]
QUAD_FIRST = ['-', 'the session broke', 'the last word was wrong', 'a word was wrong',
              'the flag did not come', "the slave's record did not come"]
QUAD_REFUSED = ['armed', 'an argument out of range', 'the link not plain', 'not armed',
                "the slave's build has no test"]
QUAD_REC = ['hdr', 'no', 'arm', 'len', 'end', 'stat', 'dma', 'twc', 'stat_or', 'stat_bound', 'hand',
            't_start', 't_feed', 't_flag', 't_end', 'spif', 'rx', 'stage']
QUAD_NONE = 0xFFFFFFFF
SPI_STAT_RF, SPI_STAT_TF = 1 << 10, 1 << 11
RECORD_PARTS, RECORD_RUNG_PARTS, RECORD_FALL_PARTS = 19, 4, 6
LINK_TEST_N, LINK_TEST_PARTS = 16, 5
LINK_TEST_LIMIT_S = 90  # OPXY_LINK_TEST_LIMIT_S: the unit's bound on a run; the tool waits a minute more
RESTART = 'the slave may still be in a fall-back\'s state: restart the unit before an install or a hand-over'
# where an exchange broke (S09_AT_...), why the slave fell back (S-09.079), a rung's result
AT_TEXT = ['no fault', 'the send failed', 'the ready line never came', 'the reply never started',
           'the reply broke off', "the reply's first word was wrong", "the reply's check word was wrong",
           'no reply within T_EXEC', "the slave's line said it fell back", "the reply's value was wrong"]
AT_SHORT = ['-', 'send', 'ready', 'no reply', 'broke', 'first word', 'check', 'T_EXEC', 'slave fell',
            'value']
WHY_TEXT = ['no fall-back yet', 'a check word wrong', 'a session shorter than its request',
            'a header it ignores', 'its receive failed', 'its trial ran out', 'a number or check in conflict',
            'a request other than a test on trial', 'its check failed its known answers',
            'a reply it saw no end of; its replies end by their word count from then on']
RR_TEXT = ['kept', '0x19 on trial failed', 'a test failed', '0x19 keep failed', 'left, no faster than plain',
           'the check failed its known answers']
RR_NONE = 0xFF          # a link test's rung not tried; an entry's fault: lost before its trial
LINK_TEST_REPLY = 37    # a run's answer after its status byte (OPXY_LINK_TEST_REPLY less one)
LINK_TEST_ROW = 33      # a row of its table
BENCH_FAULT = 114       # a link test's fault as the record keeps it
# the SPI driver's stage of a failure (WB_SPI_STAGE_..., bf70x_spi.h); with dma_err, ERRC in bits 8-10
SPI_STAGES = ['none', 'arguments refused', 'waiting for transmit room', 'waiting for the transmit to end',
              'waiting for a received word', 'the DMA refused the transfer', "the DMA's count stopped",
              'the DMA reported an error', 'receive overrun or mode fault', 'the master-ready pin never high',
              'the first word never came', 'a later word never came', 'PB15 rose with words to go',
              'transmit underrun or collision', 'the reply ran out its bound', 'the header refused']
PATTERNS = ['0x00', '0xFF', '0xAA', '0x55', '0x0F', '0xF0', 'walking one', 'walking zero', 'count', 'random']
BENCH_STAGES = ['sector copy', 'crc-16', 'sha-256', 'aes-256-cbc', 'screen frame', 'sha-256 software',
                'aes-256 software']
CHECK_STAGES = ['reads', 'crc-16', 'sha-256', 'aes-256-cbc', 'signature', 'usb service', 'whole check']
CHECK_REASONS = ['', 'short', 'preamble', 'type', 'magic', 'product', 'device', 'crc', 'format', 'sku',
                 'length', 'hash', 'key', 'nokey', 'name', 'ziplen', 'zip', 'member', 'space', 'spacequery',
                 'size', 'stream']
EXTRACT_COUNTS = ['files', 'directories', 'staging writes', 'write requests', 'mkdir requests', 'stat requests']
EXTRACT_STAGES = ['zip reads', 'decryption', 'inflate', 'crc-32', 'member listing', 'usb service',
                  'whole extraction', 'checks 1-15']
EXTRACT_FLAGS = [(0x01, 'the slave is absent: no bridge request was timed'),
                 (0x02, 'A WRITE REQUEST SUCCEEDED: the batches stopped; check the volume'),
                 (0x04, 'the second processor stopped answering; restart the unit before installing'),
                 (0x08, 'too many directories to tell apart: the stat count is high'),
                 (0x10, RESTART)]
PROBES = ['write', 'stat']
RUNGS = ['single I/O at 25 MHz, plain', 'single I/O at 25 MHz, checked',
         'single I/O at 50 MHz, checked', 'quad I/O at 25 MHz, checked', 'quad I/O at 50 MHz, checked']
PLAIN_WORD_US = 1.28    # S-13b.041: 32 bits in single I/O at 25 MHz, the plain wire's word
CCLK_HZ = 400e6         # S-13a.001: the cycle counter's rate
KEY_UP, KEY_DOWN, KEY_PRESS = 0, 1, 2
KEY_PAIRS, FRAME_CHUNK, FRAME_BYTES = 32, 240, 2 * 224 * 480
# The digits of S-15.011 (M1-M4, then sounds 5-8) and the keys the menus name.
KEYS = {'1': 0x06, '2': 0x07, '3': 0x08, '4': 0x09, '5': 0x12, '6': 0x13, '7': 0x14, '8': 0x15,
        'white': 0x0D, 'back': 0x15, 'help': 0x00, 'shift': 0x37}
USB_SETS = ['midi', 'midi+cdc', 'disk', 'disk+cdc', 'mtp', 'mtp vendor', 'serial', 'mtp+cdc',
            'mtp vendor+cdc']
MTP_STATES = ['off', 'vendor (macOS)', 'standard (Windows)']
SCREEN_FLAGS = [(0x01, 'usb up'), (0x02, 'key test'), (0x04, 'armed'), (0x08, 'busy'),
                (0x10, 'keys held'), (0x20, 'shift')]
PUT_OPEN, PUT_DATA, PUT_CLOSE = 0, 1, 2
SEND_ADDR = 0   # wb_spi_testos_send in the running image's ELF
STEPS = ['server', 'msi init', 'eMMC init', 'volume', 'eMMC features', 'mount',
         'last request', 'last send', 'queue']


FW, FW_REPLY, FW_CONTINUE = 0x0D, 0x0E, 0x19   # S-10c.070, S-10c.076
CARRIER = bytes([0xF0, 0x00, 0x20, 0x76, 0x21, 0x66])   # S-10c.020


def pack7(data):
    """S-10c.001: groups of up to seven bytes, each after a header of their bit 7s."""
    out = bytearray()
    for i in range(0, len(data), 7):
        group = data[i:i + 7]
        out.append(sum(((b >> 7) & 1) << k for k, b in enumerate(group)))
        out += bytes(b & 0x7F for b in group)
    return bytes(out)


def continue_sysex():
    """The firmware command 0D 19 as one carrier message: the frame's wire bytes
    with its delimiter, packed (S-10c.020, S-10c.022)."""
    return CARRIER + pack7(frame(bytes([FW_CONTINUE]), 0, ident=FW)) + b'\xf7'


def continue_os(mon, timeout=5.0):
    """0D 19 on the serial port, then its 0E reply: 54, or 84 and a text."""
    mon.seq += 1
    os.write(mon.fd, frame(bytes([FW_CONTINUE]), mon.seq, ident=FW))
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        while b'\0' in mon.rx:
            wire, _, rest = bytes(mon.rx).partition(b'\0')
            mon.rx = bytearray(rest)
            got = unframe(wire)
            if not got or got[0] != FW_REPLY or not got[1] or got[1][0] == 0x92:
                continue
            if got[1][0] == 0x54:
                print('54: the unit restarts, and its next start boots the OS')
                return 0
            print('%02x: %s' % (got[1][0], got[1][1:].decode('ascii', 'replace')))
            return 1
        ready, _, _ = select.select([mon.fd], [], [], 0.05)
        if ready:
            try:
                mon.rx += os.read(mon.fd, 4096)
            except OSError:
                break
    raise SystemExit('no reply to continue-os: is the unit in the boot menu?')


def crc8(data):
    """S-06.003: reflected 0x4D, every input byte XORed with 0xA5."""
    crc = 0
    for b in data:
        crc ^= b ^ 0xA5
        for _ in range(8):
            crc = (crc >> 1) ^ 0xB2 if crc & 1 else crc >> 1
    return crc


def cobs_encode(raw):
    out, block = bytearray(), bytearray()
    for b in raw:
        if b == 0:
            out += bytes([len(block) + 1]) + block
            block = bytearray()
        else:
            block.append(b)
            if len(block) == 254:
                out += b'\xff' + block
                block = bytearray()
    return bytes(out + bytes([len(block) + 1]) + block)


def cobs_decode(enc):
    out, i = bytearray(), 0
    while i < len(enc):
        code = enc[i]
        if code == 0 or i + code > len(enc):
            raise ValueError('bad COBS')
        out += enc[i + 1:i + code]
        i += code
        if code != 0xFF and i < len(enc):
            out.append(0)
    return bytes(out)


def frame(payload, seq=0, ident=ID, route=ROUTE):
    raw = bytes([route, seq & 0xFF, ident, len(payload)]) + payload
    return cobs_encode(raw + bytes([crc8(raw)])) + b'\0'


def unframe(wire):
    """(id, payload) of a frame's bytes without its delimiter, or None if bad."""
    try:
        raw = cobs_decode(wire)
    except ValueError:
        return None
    if len(raw) < 5 or raw[3] != len(raw) - 5 or crc8(raw[:-1]) != raw[-1]:
        return None
    return raw[2], raw[4:-1]


class Monitor:
    def __init__(self, port):
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = attrs[1] = attrs[3] = 0
        attrs[2] = termios.CS8 | termios.CLOCAL | termios.CREAD
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        termios.tcflush(self.fd, termios.TCIOFLUSH)
        self.seq = 0
        self.rx = bytearray()

    def call(self, payload, timeout=5.0):
        self.seq += 1
        os.write(self.fd, frame(payload, self.seq))
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            while b'\0' in self.rx:
                wire, _, rest = bytes(self.rx).partition(b'\0')
                self.rx = bytearray(rest)
                got = unframe(wire)
                if got and got[0] == ID:    # the S-14 log shares this port
                    return got[1]
            ready, _, _ = select.select([self.fd], [], [], 0.05)
            if ready:
                self.rx += os.read(self.fd, 4096)
        raise SystemExit('no answer from the monitor: is the unit hung? power-cycle it')

    def send(self, payload):
        """Send one frame without waiting for a reply."""
        self.seq += 1
        os.write(self.fd, frame(payload, self.seq))

    def next_frame(self, timeout):
        """The payload of the next frame of this id, or None if none arrives in
        time (or the port vanished)."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            while b'\0' in self.rx:
                wire, _, rest = bytes(self.rx).partition(b'\0')
                self.rx = bytearray(rest)
                got = unframe(wire)
                if got and got[0] == ID:
                    return got[1]
            ready, _, _ = select.select([self.fd], [], [], 0.05)
            if ready:
                try:
                    self.rx += os.read(self.fd, 4096)
                except OSError:
                    return None
        return None

    def checked(self, payload, timeout=5.0):
        r = self.call(payload, timeout)
        if r[0] != 0:
            raise SystemExit('monitor: %s' % STATUS.get(r[0], r[0]))
        return r[1:]

    def bus(self, program, timeout=5.0):
        return self.checked(bytes([BUS]) + program, timeout)

    def session(self, cmd, slot=0, length=0, arg0=0, arg1=0):
        """An immediate request and its reply: (ok, reply word 0, reply word 1, [notes])."""
        hdr = struct.pack('<BBHII', cmd, slot, length, arg0, arg1)
        prog = (b's\x01' + b't' + bytes([len(hdr)]) + hdr + b'w\x00' + struct.pack('<I', 500000) + b's\x00'
                + b'w\x01' + struct.pack('<I', 500000) + b's\x01' + b'r\x08' + b'w\x00' + struct.pack('<I', 500000)
                + b's\x00' + b'c\x00')    # c: SPI0 reset, after a receive (wiki/Test-OS.md)
        r = self.bus(prog)
        # select, send, wait(5), select, wait(5), select, recv(9), wait(5), select
        notes = []
        if r[1] != 1:
            notes.append('send failed')
        if r[2] != 1:
            notes.append('ready line did not rise after the request')
        wait = struct.unpack('<I', r[3:7])[0]
        if r[8] != 1:
            notes.append('ready line did not fall for the reply (%d us)' % struct.unpack('<I', r[9:13])[0])
        if r[14] != 1:
            notes.append('receive failed')
        w0, w1 = struct.unpack('<II', r[15:23])
        if r[23] != 1:
            notes.append('ready line did not rise after the reply')
        return not notes, w0, w1, notes + ['request end %d us' % wait]


LOG_CONFIG = 0xB0      # S-14.012, S-14.013: the log-configuration request
LOG_IDS = (0x03, 0x04)  # S-14.021: a log line, an error line


def show_log(mon, seconds):
    """Threshold 3 and both link sinks on, then each log line as it comes: the
    timestamp (S-14.001, u32 big-endian, microseconds) and the body."""
    mon.seq += 1
    os.write(mon.fd, frame(bytes([0xA2, 0x03]), mon.seq, LOG_CONFIG))
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        while b'\0' in mon.rx:
            wire, _, rest = bytes(mon.rx).partition(b'\0')
            mon.rx = bytearray(rest)
            got = unframe(wire)
            if got and got[0] in LOG_IDS and len(got[1]) >= 4:
                ts = struct.unpack('>I', got[1][:4])[0]
                print('%10.3f ms  %s' % (ts / 1000.0, got[1][4:].decode('latin-1')))
        ready, _, _ = select.select([mon.fd], [], [], 0.05)
        if ready:
            mon.rx += os.read(mon.fd, 4096)


def nordic_info(mon, timeout=1.0):
    """The payload of the Nordic's reply to one info request sent through the master
    (S-06.050, S-06.052: a host that wants the answer sends with source 2), or None."""
    mon.seq += 1
    os.write(mon.fd, frame(b'', mon.seq, INFO, NORDIC_ROUTE))
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        while b'\0' in mon.rx:
            wire, _, rest = bytes(mon.rx).partition(b'\0')
            mon.rx = bytearray(rest)
            got = unframe(wire)
            if got and got[0] == INFO_REPLY:
                return got[1]
        ready, _, _ = select.select([mon.fd], [], [], 0.05)
        if ready:
            mon.rx += os.read(mon.fd, 4096)
    return None


def show_nordic_info(mon):
    reply = nordic_info(mon)
    if reply is None:
        raise SystemExit('no answer from the Nordic within 1 s: is the unit in held mode, past the '
                         'link greeting (about 6 s after the screen appears)?')
    text = reply.decode('latin-1')
    print(text)
    entries = dict(e.split(':', 1) for e in text.split(';') if ':' in e)
    for key in ('mode', 'sw_version', 'bl_version', 'boot_slot'):
        print('  %-11s %s' % (key, entries.get(key, '(no such entry)')))


def num(s):
    return int(s, 0)


CRUMBS = {1: 'opxy_master_main entered: stream, INIT and start code all done',
          2: 'flash set up', 3: 'bring-up done', 4: 'DDR check done', 5: 'metadata read',
          6: 'PSoC link open, first log line', 7: 'S-07 start-up done', 8: 'early logo done',
          9: 'slave start entered', 10: 'held mode entered', 11: 'running-slot inference entered',
          12: 'running-slot inference returned', 15: 'EXCEPTION', 17: 'OS staged and accepted: the release',
          19: 'release answered'}
SPACES = {1: 'L1 data A', 2: 'L1 data B', 3: 'L1 scratch', 4: 'L2', 5: 'other', 6: 'DDR'}


def show_crumbs(c):
    """A breadcrumb build's marks, from the tried slot's last sector (src/board/opxy_crumbs.c); c[0] is
    that sector's byte 0, the start log's (S-02.095)."""
    marks = [i for i in range(1, 32) if c[i] == i]
    if all(b == 0xFF for b in c[1:]):           # byte 0 is a trial's entry (S-02.095)
        print('breadcrumbs: none')
        return
    for i in marks:
        print('  crumb %2d: %s' % (i, CRUMBS.get(i, '?')))
    print('  init sp %08x dctl %08x dflt %08x' % struct.unpack('<3I', bytes(c[32:44])))
    if c[15] == 15:
        seqstat, retx = struct.unpack('<2I', bytes(c[44:52]))
        print('  SEQSTAT %08x (EXCAUSE %02x, HWERRCAUSE %02x) RETX %08x'
              % (seqstat, seqstat & 0x3F, (seqstat >> 14) & 0x1F, retx))
    spaces = [SPACES.get(b, '%02x' % b) for b in c[52:64] if b != 0xFF]
    if spaces:
        print('  memory compared, in order: ' + ', '.join(spaces))


def show_last(mon, clear=False):
    r = mon.checked(bytes([LAST, 1 if clear else 0]))
    if len(r) < 12:
        print('no note: no routine has failed to return')
        return
    magic, addr, arg = struct.unpack('<3I', r[:12])
    print('NEVER RETURNED: %08x(%08x)%s' % (addr, arg, '' if magic == RUN_MAGIC else ' (bad note)'))


def wait_for_port(t0, gone):
    while time.monotonic() - t0 < 120:
        ports = glob.glob('/dev/cu.usbmodem*')
        if gone and not ports:
            return None
        if not gone and ports:
            return ports[0]
        time.sleep(0.1)
    raise SystemExit('the port did not %s within 120 s' % ('go' if gone else 'come back'))


def send_stats(mon):
    # The symbol's address differs between builds: testos_send below is set by --send-addr.
    addr = SEND_ADDR
    names = ['or stat', 'last stat', 'rc', 'out', 'in', 'twc', 'twc before', 'twc at low',
             'low at', 'twc 0 at', 'empty at', 'last word at']
    for i, name in enumerate(names):
        print('%-13s %08x' % (name, expect(mon, PEEK, 0, addr + 4 * i)))


def print_gap(b):
    """The longest time USB went unserviced in the request; absent from older builds."""
    if len(b) == 4:
        print('usb: longest gap between services %.1f ms' % (struct.unpack('<I', b)[0] / 1000))


def show_session(r):
    ok, w0, w1, notes = r
    print('%s reply %08x %08x; %s' % ('ok' if ok else 'FAILED', w0, w1, '; '.join(notes)))


def expect(mon, cmd, slot, arg0, arg1=0):
    ok, w0, w1, notes = mon.session(cmd, slot, 0, arg0, arg1)
    want = cmd | slot << 8
    if not ok or w0 != want:
        raise SystemExit('request %02x: reply %08x %08x, wanted first word %08x; %s'
                         % (cmd, w0, w1, want, '; '.join(notes)))
    return w1


# ---- Q-11's measurements (S-13b Q-11; src/include/waffle/testos.h) ----

REPLY_FCCH, REPLY_TXCTL, REPLY_NO_REN, REPLY_QUEUE, REPLY_DMA = 0x01, 0x02, 0x04, 0x08, 0x10
REPLY_EN_LAST, REPLY_QUAD, REPLY_TF_LATE = 0x020, 0x040, 0x080
REPLY_NO_TWCEN, REPLY_NO_TTI, REPLY_TIGHT = 0x100, 0x200, 0x400
ARM_SKIP = 0x80000000
REQ_QUAD, REQ_EN_LAST = 0x1, 0x2
WHOLE = REPLY_FCCH | REPLY_TXCTL | REPLY_QUEUE          # S-13b.044's reply set-up
QUAD_LAST = WHOLE | REPLY_QUAD | REPLY_EN_LAST
M_SINGLE, M_QUAD = (0x00002592, 3), (0x00242592, 1)     # S-13b.040, .041: SPI0_CTL, SPI0_CLK
FMODE = 1 << 18
NONE = 0x80000000
Q11_SLOTS = ['mode', 'rc', 'ended', 'word0', 'dma done', 'tf unmask', 'tf stat', 'tf ilat', 'twc 0',
             'pb15 low', 'pb15 high', 'spif count', 'spif first', 'spif last', 'stat', 'twc']
Q11_TIMES = ['dma done', 'tf unmask', 'tf stat', 'tf ilat', 'twc 0', 'pb15 low', 'pb15 high',
             'spif first', 'spif last']
ENDED = ['-', 'tf', 'count', 'master', 'bound', 'error', 'no hand-over']
REQSTAT = ['bits', 'rc', 'hdr0', 'hdr1', 'hdr2', 'stat', 'fallbacks', 'whole']
Q11_SYMBOLS = ['wb_spi_testos_mode', 'wb_spi_testos_arm', 'wb_spi_testos_q11', 'wb_spi_testos_tally',
               'wb_spi_testos_req', 'wb_spi_testos_reqstat', 'wb_spi_testos_send']
Q11_ELF = 'build/bf703-testos/waffle-test-mon.elf'
BUS_MAX = 248           # mon_bus(): an S-06 payload less the opcode, or less the status byte
RISE_US = 500000
STAT_ROR, STAT_TUR, STAT_TC, STAT_TF = 1 << 4, 1 << 5, 1 << 6, 1 << 11
ECHO = 0x0A


def symbol_addrs(elf, names=Q11_SYMBOLS):
    """{name: address} of C symbols in an elf32.Elf; the toolchain prefixes them with '_'."""
    out = {}
    for n in names:
        v = elf.symbol('_' + n)
        if v is None:
            v = elf.symbol(n)
        if v is None:
            raise SystemExit('no symbol %s in the ELF: build the monitor image (make test-os-mon)' % n)
        out[n] = v
    return out


def read_symbols(path):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import elf32
    try:
        return symbol_addrs(elf32.read(path))
    except (OSError, elf32.ElfError) as e:
        raise SystemExit('%s: %s' % (path, e))


def signed(v):
    return v - (1 << 32) if v & 0x80000000 else v


def cycles(v):
    """A slot of cycles as an int, or None for not seen."""
    return None if v == NONE else signed(v)


class Prog:
    """A MON_BUS program (src/testos/opxy_testos.c, mon_bus()) and the layout of its output."""

    def __init__(self):
        self.code = bytearray()
        self.steps = []         # (op, bytes received)

    def _add(self, op, code, n=0):
        self.code += code
        self.steps.append((op, n))
        return len(self.steps) - 1

    def sel(self, on):
        return self._add('s', b's' + bytes([1 if on else 0]))

    def send(self, data):
        return self._add('t', b't' + bytes([len(data)]) + data)

    def recv(self, n):
        return self._add('r', b'r' + bytes([n]), n)

    def wait(self, ready, us):
        """Wait for the ready line: ready (PA6 low) or not (PA6 high)."""
        return self._add('w', b'w' + bytes([1 if ready else 0]) + struct.pack('<I', us))

    def delay(self, us):
        return self._add('d', b'd' + struct.pack('<I', us))

    def conf(self, phase=0):
        return self._add('c', b'c' + bytes([phase]))

    def send_last(self, ctl, clk, data):
        return self._add('T', b'T' + struct.pack('<IIB', ctl, clk, len(data)) + data)

    def recv_last(self, ctl, clk, n):
        return self._add('R', b'R' + struct.pack('<IIB', ctl, clk, n), n)

    @staticmethod
    def room(op, n):
        """The output bytes a step writes."""
        return {'r': 1 + n, 'w': 5, 'T': 17, 'R': 17 + n}.get(op, 1)

    def fits(self):
        """Whether mon_bus() runs every step: the program fits a payload, and each step finds the
        room it needs, and at least 6 bytes, in the output."""
        o = 0
        for op, n in self.steps:
            if o + max(6, self.room(op, n)) > BUS_MAX:
                return False
            o += self.room(op, n)
        return len(self.code) <= BUS_MAX

    def parse(self, r):
        """A dict per step: op, ok, and data, us, status or seen as the step gives them. A step
        the output does not reach has ok False and missing True."""
        out, o = [], 0
        for op, n in self.steps:
            need = self.room(op, n)
            if o + need > len(r) or (r[o] == 0xFF and op not in 'TR'):
                out.append({'op': op, 'ok': False, 'missing': True})
                continue
            d = {'op': op, 'ok': r[o] == 1}
            if op in 'rR':
                d['data'] = bytes(r[o + 1:o + 1 + n])
            if op == 'w':
                d['us'] = struct.unpack_from('<I', r, o + 1)[0]
            if op in 'TR':
                d['status'] = r[o]
                d['seen'] = list(struct.unpack_from('<4I', r, o + 1 + (n if op == 'R' else 0)))
            out.append(d)
            o += need
        return out


def hdr(cmd, arg0=0, arg1=0):
    return struct.pack('<BBHII', cmd, 0, 0, arg0, arg1)


def immediate(p, req, request=None, reply=None, gap_us=None, rise_us=RISE_US):
    """One immediate session's steps; returns their indices. The request goes in single I/O, or under
    request = (ctl, clk) by T; the 8-byte reply likewise by R. With gap_us, PC11 rises that long after
    the reply's words and the ready line is waited for after it: the slave then sees the master's end
    whatever ended its reply."""
    first = len(p.steps)
    p.sel(True)
    if request:
        p.send_last(request[0], request[1], req)
    else:
        p.send(req)
    p.wait(False, rise_us)
    p.sel(False)
    p.wait(True, rise_us)
    p.sel(True)
    if reply:
        p.recv_last(reply[0], reply[1], 8)
    else:
        p.recv(8)
    if gap_us is None:
        p.wait(False, rise_us)
        p.sel(False)
    else:
        p.delay(gap_us)
        p.sel(False)
        p.wait(False, rise_us)
    return list(range(first, len(p.steps)))


def session_of(parsed, idx, cmd, want1=None):
    """One session's result from parsed steps: ok (every step, and the reply's header word is cmd's
    and, given want1, its value), the two words, the T and R steps' status and seen, and the last
    wait for the ready line to rise."""
    steps = [parsed[i] for i in idx]
    rx = next(s for s in steps if s['op'] in 'rR')
    w0 = w1 = None
    if len(rx.get('data', b'')) == 8:
        w0, w1 = struct.unpack('<II', rx['data'])
    res = {'steps_ok': all(s['ok'] for s in steps), 'words': [w0, w1],
           'right': w0 == cmd and (want1 is None or w1 == want1)}
    res['ok'] = res['steps_ok'] and res['right']
    for s in steps:
        if s['op'] in 'TR':
            res['request' if s['op'] == 'T' else 'reply'] = {'status': s.get('status'), 'seen': s.get('seen')}
    waits = [s for s in steps if s['op'] == 'w']
    if waits and 'us' in waits[-1]:
        res['rise_us'] = waits[-1]['us']
    return res


def run_prog(mon, p, timeout=15.0):
    if not p.fits():
        raise SystemExit('internal: a bus program does not fit the monitor')
    return p.parse(mon.bus(bytes(p.code), timeout))


def poke_prog(p, addr, value):
    return immediate(p, hdr(POKE, addr, value))


def peek_words(mon, addrs):
    """Words of the slave, five sessions to a bus program; None for a peek that failed."""
    out = []
    for i in range(0, len(addrs), 5):
        p = Prog()
        idx = [immediate(p, hdr(PEEK, a)) for a in addrs[i:i + 5]]
        p.conf(0)
        parsed = run_prog(mon, p)
        for k in idx:
            s = session_of(parsed, k, PEEK)
            out.append(s['words'][1] if s['ok'] else None)
    return out


def echo_arg(i):
    return (0x5A3C0000 ^ (i * 0x9E3779B1)) & 0xFFFFFFFF


def bridge_check(mon, when):
    """A plain echo in single I/O, again after 2.5 s (a quad listen gives up after 2 s); exit if lost."""
    for attempt in (0, 1):
        ok, w0, w1, _ = mon.session(ECHO, arg0=0x13572468)
        if ok and w0 == ECHO and w1 == 0x13572468 ^ 0xFFFFFFFF:
            return
        if attempt == 0:
            time.sleep(2.5)
    print('the bridge does not answer a single-I/O echo %s (reply %s %s)' % (
        when, '%08x' % w0 if w0 is not None else '-', '%08x' % w1 if w1 is not None else '-'))
    print('the bridge is lost: restart the unit through the PSoC\'s request (wiki/Test-OS.md), not by hand')
    raise SystemExit(1)


def slave_clean(mon, sym):
    for name in ('wb_spi_testos_arm', 'wb_spi_testos_req', 'wb_spi_testos_mode'):
        expect(mon, POKE, 0, sym[name], 0)


def reply_exchange(mon, sym, mode, i, master=M_QUAD, gap_us=50, rise_us=RISE_US):
    """One armed reply (mode None: the default one): the mode, the arm count of one with its skip,
    the slot of the mode cleared, the echo, the arm count cleared; then the slots."""
    arg = echo_arg(i)
    p = Prog()
    if mode is not None:
        poke_prog(p, sym['wb_spi_testos_mode'], mode)
        poke_prog(p, sym['wb_spi_testos_q11'], 0)
        poke_prog(p, sym['wb_spi_testos_arm'], 1 | ARM_SKIP)
    idx = immediate(p, hdr(ECHO, arg), reply=master if mode is not None else None,
                    gap_us=gap_us if mode is not None else None, rise_us=rise_us)
    if mode is not None:
        poke_prog(p, sym['wb_spi_testos_arm'], 0)
    p.conf(0)
    ex = session_of(run_prog(mon, p), idx, ECHO, arg ^ 0xFFFFFFFF)
    ex.update({'i': i, 'arg': arg, 'mode': mode, 'master': list(master) if mode is not None else None})
    if mode is not None:
        q = peek_words(mon, [sym['wb_spi_testos_q11'] + 4 * k for k in range(len(Q11_SLOTS))])
        ex['q11'] = q
    return ex


def request_batch(mon, sym, bits, args, master=M_QUAD):
    """Requests in quad I/O, the listens armed for as many: each its echo, every reply single."""
    p = Prog()
    poke_prog(p, sym['wb_spi_testos_req'], bits | len(args) << 8)
    idx = [immediate(p, hdr(ECHO, a), request=master) for a in args]
    p.conf(0)
    parsed = run_prog(mon, p)
    return [session_of(parsed, k, ECHO, a ^ 0xFFFFFFFF) for k, a in zip(idx, args)]


def q11_named(q):
    return dict(zip(Q11_SLOTS, q)) if q else {}


def fmt_cycles(v):
    c = cycles(v) if v is not None else None
    return '-' if c is None else str(c)


def exchange_line(ex):
    words = ' '.join('%08x' % w if w is not None else '--------' for w in ex['words'])
    s = '%3d %-5s %s' % (ex['i'], 'ok' if ex['ok'] else 'WRONG' if ex['steps_ok'] else 'FAIL', words)
    q = q11_named(ex.get('q11'))
    if q:
        if None in q.values():
            return s + '  slots: a peek failed'
        s += '  ended %s rc %d mode %03x' % (ENDED[q['ended']] if q['ended'] < len(ENDED) else q['ended'],
                                              signed(q['rc']), q['mode'])
        s += ''.join(' %s %s' % (k.replace(' ', '-'), fmt_cycles(q[k])) for k in Q11_TIMES)
        s += ' spif %d/%d stat %08x twc %d' % (q['spif count'] & 0xFFFF, q['spif count'] >> 16, q['stat'],
                                               q['twc'])
    for side in ('request', 'reply'):
        if side in ex:
            seen = ex[side]['seen'] or [0, 0, NONE, NONE]
            s += '  master %s status %02x or %08x last %08x words %s fin %s' % (
                'T' if side == 'request' else 'R', ex[side]['status'] or 0, seen[0], seen[1],
                fmt_cycles(seen[2]), fmt_cycles(seen[3]))
    if 'rise_us' in ex:
        s += '  rise %d us' % ex['rise_us']
    return s


def span(vals):
    vals = [v for v in vals if v is not None]
    return '%d..%d' % (min(vals), max(vals)) if vals else '-'


def reply_summary(exs, armed=True):
    """The numbers Q-11 (1a) to (1f) ask for, from a variant's exchanges."""
    n = len(exs)
    lines = ['replies right %d of %d' % (sum(1 for e in exs if e['ok']), n)]
    if not armed:
        return lines
    qs = [q11_named(e.get('q11')) for e in exs]
    qs = [q for q in qs if q and None not in q.values()]
    used = [q for q in qs if q['mode'] != 0]
    lines.append('armed replies recorded %d of %d' % (len(used), n))
    ends = {}
    for q in used:
        k = ENDED[q['ended']] if q['ended'] < len(ENDED) else str(q['ended'])
        ends[k] = ends.get(k, 0) + 1
    lines.append('ended by: %s' % (', '.join('%s %d' % kv for kv in sorted(ends.items())) or '-'))
    tf = [q for q in used if cycles(q['tf stat']) is not None]
    lines.append('TF read set in SPI_STAT on %d of %d, in SPI_ILAT on %d' % (
        len(tf), len(used), sum(1 for q in used if cycles(q['tf ilat']) is not None)))
    lines.append('cycles from TF to the master\'s end (PB15 high): %s' % span(
        [cycles(q['pb15 high']) - cycles(q['tf stat']) for q in tf if cycles(q['pb15 high']) is not None]))
    lines.append('cycles from TWC 0 to TF: %s; from the DMA done to TF: %s' % (
        span([cycles(q['tf stat']) - cycles(q['twc 0']) for q in tf if cycles(q['twc 0']) is not None]),
        span([cycles(q['tf stat']) - cycles(q['dma done']) for q in tf if cycles(q['dma done']) is not None])))
    lines.append('DMA done seen on %d; TF unmasked at %s cycles' % (
        sum(1 for q in used if cycles(q['dma done']) is not None), span([cycles(q['tf unmask']) for q in used])))
    errs = sum(1 for q in used if q['stat'] & (STAT_TUR | STAT_TC))
    lines.append('TUR or TC in the last SPI_STAT on %d' % errs)
    if any(q['mode'] & REPLY_TIGHT for q in used):
        tight = [q for q in used if q['mode'] & REPLY_TIGHT]
        moved = [q for q in tight if q['spif count'] != 0]
        lines.append('tight loop: a SPIF change on %d of %d; falls %s, rises %s; first %s, last %s cycles' % (
            len(moved), len(tight), span([q['spif count'] & 0xFFFF for q in tight]),
            span([q['spif count'] >> 16 for q in tight]), span([cycles(q['spif first']) for q in moved]),
            span([cycles(q['spif last']) for q in moved])))
    rx = [e['reply'] for e in exs if 'reply' in e]
    if rx:
        lines.append('master R: status ok on %d of %d, RF seen on %d, cycles to the last word %s' % (
            sum(1 for r in rx if r['status'] == 1), len(rx),
            sum(1 for r in rx if r['seen'] and cycles(r['seen'][3]) is not None),
            span([cycles(r['seen'][2]) for r in rx if r['seen']])))
    return lines


def request_summary(exs, before, after, args):
    """The numbers Q-11 (1g) and (1h) ask for. before and after: the slave's reqstat words."""
    n = len(exs)
    tx = [e['request'] for e in exs if 'request' in e]
    lines = ['replies (single I/O) right %d of %d' % (sum(1 for e in exs if e['ok']), n)]
    lines.append('master T: status ok on %d of %d, TF read set on %d, cycles to the last word %s, to TF %s' % (
        sum(1 for t in tx if t['status'] == 1), len(tx),
        sum(1 for t in tx if t['seen'] and cycles(t['seen'][3]) is not None),
        span([cycles(t['seen'][2]) for t in tx if t['seen']]),
        span([cycles(t['seen'][3]) for t in tx if t['seen']])))
    if before and after and None not in before + after:
        b, a = dict(zip(REQSTAT, before)), dict(zip(REQSTAT, after))
        lines.append('slave: quad requests whose header came whole %d of %d; 2 s fallbacks %d' % (
            a['whole'] - b['whole'], n, a['fallbacks'] - b['fallbacks']))
        lines.append('slave: SPI_STAT or-ed over the requests %08x (ROR %s); last listen %04x rc %d' % (
            a['stat'], 'set' if a['stat'] & STAT_ROR else 'clear', a['bits'] & 0xFFFF, signed(a['rc'])))
        if args:
            want = list(struct.unpack('<3I', hdr(ECHO, args[-1])))
            got = [a['hdr0'], a['hdr1'], a['hdr2']]
            lines.append('slave: the last header as received %s: %s' % (
                ' '.join('%08x' % w for w in got), 'right' if got == want else 'WRONG'))
    else:
        lines.append('slave: its record could not be read')
    return lines


Q11_ALL = ['1a', '1b', '1c', '1d', '1e', '1f', '1g', '1h']


def reply_variants(which):
    """[(label, slave mode or None, master (ctl, clk), gap us, rise us)] of a reply measurement."""
    dma = QUAD_LAST | REPLY_DMA | REPLY_TF_LATE
    core = QUAD_LAST
    if which == '1a':
        return [('dma', dma, M_QUAD, 50, RISE_US)]
    if which == '1b':
        return [('core', core, M_QUAD, 50, RISE_US)]
    if which == '1c':
        out = []
        for label, mode in (('dma', dma), ('core', core)):
            for fm, clk in ((False, 1), (False, 3), (True, 3)):
                ctl = M_QUAD[0] if fm else M_QUAD[0] & ~FMODE
                out.append(('%s fmode %s clk %d' % (label, 'on' if fm else 'off', clk), mode, (ctl, clk), 50,
                            RISE_US))
        return out
    if which == '1d':
        return [('dma tight', dma | REPLY_TIGHT, M_QUAD, 5000, 1000000)]
    if which == '1e':
        return [('txctl whole', dma, M_QUAD, 50, RISE_US),
                ('twcen clear', dma | REPLY_NO_TWCEN, M_QUAD, 50, RISE_US),
                ('tti clear', dma | REPLY_NO_TTI, M_QUAD, 50, RISE_US)]
    if which == '1f':
        return [('quad, ren clear', dma, M_QUAD, 50, RISE_US), ('single, default', None, M_SINGLE, None, RISE_US)]
    return []


def q11_measure(mon, sym, which, n, log):
    print('== Q-11 (%s), %d exchanges' % (which, n))
    slave_clean(mon, sym)
    bridge_check(mon, 'before (%s)' % which)
    if which in ('1g', '1h'):
        expect(mon, POKE, 0, sym['wb_spi_testos_reqstat'] + 4 * REQSTAT.index('stat'), 0)
        rs = [sym['wb_spi_testos_reqstat'] + 4 * k for k in range(len(REQSTAT))]
        before = peek_words(mon, rs)
        exs, args = [], []
        bits = REQ_QUAD | REQ_EN_LAST
        if which == '1g':
            for i in range(n):
                got = request_batch(mon, sym, bits, [echo_arg(i)])
                got[0].update({'i': i, 'arg': echo_arg(i), 'reqstat': peek_words(mon, rs)})
                exs += got
                args.append(echo_arg(i))
                print('  ' + exchange_line(got[0]) + '  slave ' + ' '.join(
                    '%s %s' % (k, '%x' % v if v is not None else '-') for k, v in zip(REQSTAT, got[0]['reqstat'])))
        else:
            for start in range(0, n, 4):
                batch = [echo_arg(i) for i in range(start, min(n, start + 4))]
                got = request_batch(mon, sym, bits, batch)
                for k, (e, a) in enumerate(zip(got, batch)):
                    e.update({'i': start + k, 'arg': a})
                    print('  ' + exchange_line(e))
                exs += got
                args += batch
        after = peek_words(mon, rs)
        for line in request_summary(exs, before, after, args):
            print('  ' + line)
        log.append({'measure': which, 'variant': 'quad requests', 'reqstat_before': before,
                    'reqstat_after': after, 'exchanges': exs})
    else:
        for label, mode, master, gap, rise in reply_variants(which):
            print('-- %s: slave mode %s, master %08x clk %d' % (
                label, '%03x' % mode if mode is not None else 'default', master[0], master[1]))
            t0 = peek_words(mon, [sym['wb_spi_testos_tally'], sym['wb_spi_testos_tally'] + 4])
            exs = []
            for i in range(n):
                ex = reply_exchange(mon, sym, mode, i, master, gap, rise)
                print('  ' + exchange_line(ex))
                exs.append(ex)
            t1 = peek_words(mon, [sym['wb_spi_testos_tally'], sym['wb_spi_testos_tally'] + 4])
            for line in reply_summary(exs, mode is not None):
                print('  ' + line)
            if mode is not None and None not in t0 + t1:
                print('  slave tally: armed replies %d, their code OK %d' % (t1[0] - t0[0], t1[1] - t0[1]))
            log.append({'measure': which, 'variant': label, 'mode': mode, 'master': list(master),
                        'tally': [t0, t1], 'exchanges': exs})
    slave_clean(mon, sym)
    bridge_check(mon, 'after (%s)' % which)


def q11(mon, a):
    opts = {'--elf': Q11_ELF, '--json': None}
    rest = []
    it = iter(a)
    for w in it:
        if w in opts:
            opts[w] = next(it, None)
        else:
            rest.append(w)
    if not rest or rest[0] not in Q11_ALL + ['all']:
        raise SystemExit('q11 %s [N] [--elf PATH] [--json FILE]' % '|'.join(Q11_ALL + ['all']))
    n = num(rest[1]) if len(rest) > 1 else 16
    sym = read_symbols(opts['--elf'])
    log = []
    try:
        for which in (Q11_ALL if rest[0] == 'all' else [rest[0]]):
            q11_measure(mon, sym, which, n, log)
    finally:
        if opts['--json']:
            import json
            with open(opts['--json'], 'w') as fh:
                json.dump({'symbols': sym, 'runs': log}, fh, indent=1)
    return 0


def run(mon, argv):
    c, a = argv[0], argv[1:]
    if c == 'crumbs':
        # check.txt lists bytes 1 to 63
        show_crumbs([0xFF] + list(bytes.fromhex(''.join(a))) + [0xFF] * 64)
    elif c == 'log':
        show_log(mon, float(a[0]) if a else 10.0)
    elif c == 'nordic-info':
        show_nordic_info(mon)
    elif c == 'ping':
        print(mon.checked(bytes([PING])))
    elif c == 'report':
        off, total = 0, None
        while total is None or off < total:
            r = mon.checked(bytes([REPORT]) + struct.pack('<I', off))
            total = struct.unpack('<I', r[:4])[0]
            if len(r) == 4:
                break
            sys.stdout.write(r[4:].decode('utf-8', 'replace'))
            off += len(r) - 4
    elif c == 'rd':
        addr, words = num(a[0]), num(a[1]) if len(a) > 1 else 1
        r = mon.checked(bytes([READ]) + struct.pack('<IB', addr, words))
        for i in range(words):
            print('%08x: %08x' % (addr + 4 * i, struct.unpack_from('<I', r, 4 * i)[0]))
    elif c == 'wr':
        mon.checked(bytes([WRITE]) + struct.pack('<II', num(a[0]), num(a[1])))
    elif c == 'load':
        addr = num(a[0])
        with open(a[1], 'rb') as f:
            data = f.read()
        for off in range(0, len(data), 240):
            mon.checked(bytes([LOAD]) + struct.pack('<I', addr + off) + data[off:off + 240])
        print('%d bytes at %08x' % (len(data), addr))
    elif c == 'call':
        r = mon.checked(bytes([CALL]) + struct.pack('<II', num(a[0]), num(a[1]) if len(a) > 1 else 0), 180.0)
        print('%08x' % struct.unpack('<I', r)[0])
    elif c == 'run':
        ms = num(a[2]) if len(a) > 2 else 2000
        r = mon.checked(bytes([RUN]) + struct.pack('<III', num(a[0]), num(a[1]) if len(a) > 1 else 0, ms),
                        ms / 1000 + 3)
        print('returned %08x' % struct.unpack('<I', r)[0])
    elif c == 'last':
        show_last(mon, a[:1] == ['clear'])
    elif c == 'stall':
        ms = num(a[0]) if a else 2000
        mon.checked(bytes([LOAD]) + struct.pack('<I', LOAD_TOP) + bytes.fromhex('00200020'))
        t0 = time.monotonic()
        os.write(mon.fd, frame(bytes([RUN]) + struct.pack('<III', LOAD_TOP, 0, ms), 200))
        os.close(mon.fd)
        print('running a routine that never returns, %d ms watchdog' % ms)
        port = wait_for_port(t0, gone=True)
        print('port gone after %.1f s' % (time.monotonic() - t0))
        port = wait_for_port(t0, gone=False)
        print('port back after %.1f s' % (time.monotonic() - t0))
        time.sleep(4)
        show_last(Monitor(port), True)
    elif c == 'send':
        send_stats(mon)
    elif c == 'q11':
        return q11(mon, a)
    elif c == 'beacon':
        r = mon.checked(bytes([BEACON]) + struct.pack('<I', num(a[0]) if a else 200000), 10.0)
        print('pc5 %s, bursts: %s' % ('high' if r[0] else 'low', ' '.join(str(b) for b in r[1:])))
    elif c == 'init':
        show_session(mon.session(0x01))
    elif c == 'echo':
        show_session(mon.session(0x0A, arg0=num(a[0]) if a else 0x12345678))
    elif c == 'pull':
        length = num(a[0]) if a else 0x1000
        hdr = struct.pack('<BBHII', 0x20, 0, length, 0, 0)
        prog = (b's\x01t' + bytes([len(hdr)]) + hdr + b'w\x00' + struct.pack('<I', 500000) + b's\x00'
                + b'w\x01' + struct.pack('<I', 500000) + b's\x01r\x04r\x04w\x00' + struct.pack('<I', 500000) + b's\x00c\x00')
        r = mon.bus(prog)
        print('request: send %d, rise %d; reply: fall %d' % (r[1], r[2], r[8]))
        print('first recv %d: %s; second recv %d: %s; rise %d' % (
            r[14], r[15:19].hex(), r[19], r[20:24].hex(), r[24]))
    elif c == 'req':
        show_session(mon.session(num(a[0]), arg0=num(a[1]) if len(a) > 1 else 0,
                                 arg1=num(a[2]) if len(a) > 2 else 0))
    elif c == 'srd':
        addr, words = num(a[0]), num(a[1]) if len(a) > 1 else 1
        for i in range(words):
            print('%08x: %08x' % (addr + 4 * i, expect(mon, PEEK, 0, addr + 4 * i)))
    elif c == 'swr':
        print('%08x' % expect(mon, POKE, 0, num(a[0]), num(a[1])))
    elif c in ('table', 'emmc'):
        expect(mon, ROW, 0, 0, 1 if c == 'emmc' else 0)
        if c == 'emmc':
            r = mon.bus(b'w\x01' + struct.pack('<I', 8000000), 12.0)
            print('eMMC bring-up: %s after %d ms' % ('ended' if r[0] else 'NO ANSWER',
                                                     struct.unpack('<I', r[1:5])[0] / 1000))
        for i, name in enumerate(STEPS):
            v = expect(mon, ROW, 0, i)
            code = v >> 16 & 0xFF
            print('%-14s %s' % (name, 'not run' if code == 0xFF else 'code %d, %d' % (code, v & 0xFFFF)))
    elif c == 'dev-put':
        import hashlib
        name = 'os.ldr'
        if '--as' in a:
            name = a[a.index('--as') + 1]
            a = a[:a.index('--as')]
        with open(a[0], 'rb') as fh:
            data = fh.read()
        mon.checked(bytes([DEV_PUT, PUT_OPEN]) + name.encode())
        for off in range(0, len(data), 240):
            mon.checked(bytes([DEV_PUT, PUT_DATA]) + data[off:off + 240])
        mon.checked(bytes([DEV_PUT, PUT_CLOSE]))
        r = mon.checked(bytes([DEV_SHA]) + name.encode(), 30.0)
        digest, size = bytes(r[:32]), struct.unpack_from('<I', r, 32)[0]
        ok = digest == hashlib.sha256(data).digest() and size == len(data)
        print('%d bytes to /yaffs2/%s, sha256 %s: %s'
              % (len(data), name, digest.hex(), 'verified' if ok else 'MISMATCH'))
        return 0 if ok else 1
    elif c == 'dev-get':
        name = a[0] if a else 'os.ldr'
        r = mon.checked(bytes([DEV_SHA]) + name.encode(), 30.0)
        print('/yaffs2/%s: sha256 %s, %d bytes'
              % (name, bytes(r[:32]).hex(), struct.unpack_from('<I', r, 32)[0]))
    elif c in ('dev-yscan', 'yshow'):
        import json
        objs = [num(a[i + 1]) for i, v in enumerate(a) if v == '--obj']
        if c == 'yshow':
            with open(a[0]) as fh:
                scan = json.load(fh)
        else:
            def tags(first, stride, count):
                return mon.checked(ytags_req(first, stride, count), 10.0)

            def meta(chunk, off, n):
                return mon.checked(ymeta_req(chunk, off, n), 10.0)

            scan = yscan(tags, meta, objs, progress=lambda b: print(
                '\rblock %d' % b, end='', file=sys.stderr, flush=True))
            print(file=sys.stderr)
            with open(a[0], 'w') as fh:
                json.dump(scan, fh)
        print(yreport(scan, objs))
    elif c == 'dev-l1rd':
        addr, words = num(a[0]), num(a[1]) if len(a) > 1 else 1
        r = mon.checked(bytes([DEV_L1READ]) + struct.pack('<IB', addr, words))
        for i in range(words):
            print('%08x: %08x' % (addr + 4 * i, struct.unpack_from('<I', r, 4 * i)[0]))
    elif c == 'dev-srd':
        print('%08x' % struct.unpack('<I', mon.checked(bytes([DEV_SREAD]) + struct.pack('<I', num(a[0])))[:4])[0])
    elif c == 'dev-swr':
        print('%08x' % struct.unpack('<I', mon.checked(bytes([DEV_SWRITE]) + struct.pack('<II', num(a[0]), num(a[1])))[:4])[0])
    elif c == 'dev-flags':
        if a:
            r = mon.checked(bytes([DEV_FLAGS]) + struct.pack('<BI', 1, num(a[0])))
        else:
            r = mon.checked(bytes([DEV_FLAGS]) + struct.pack('<BI', 0, 0))
        print('hand-over options = %08x' % struct.unpack('<I', r[:4])[0])
    elif c == 'dev-handover':
        flags = num(a[0]) if a else None
        md = num(a[1]) if len(a) > 1 else 0
        sd = num(a[2]) if len(a) > 2 else 0
        body = bytes([DEV_HANDOVER])
        if flags is not None:
            body += struct.pack('<IHH', flags, md, sd)
        reasons = {1: 'no OS in the active partition', 2: 'the image was refused',
                   3: 'the slave is absent or declined',
                   4: 'stayed after the release (0x10000); the slave is released, the bridge gone',
                   5: "the slave's OS did not start (S-05.032); the slave is released, the bridge gone"}
        mon.send(body)
        print('hand-over triggered%s' % ('' if flags is None else ' with options %08x' % flags))
        ack = mon.next_frame(5.0)			# the immediate OK ack
        if ack is None:
            print('no ack: developer mode may be off, or the port is gone')
            return 0
        t0 = time.monotonic()
        second = None
        while time.monotonic() - t0 < 45.0:
            fr = mon.next_frame(0.5)
            if fr is not None and len(fr) >= 3 and fr[0] == 0 and fr[1] == 0x50:
                print('  lines: ' + line_log(fr))
                if flags is not None and flags & 0x20000:
                    steps, tail = mark_steps(fr)
                    print('  slave steps: ' + (', '.join('%d %s' % (k, MARK_NAMES.get(k, '?')) for k in steps)
                                               or 'none'))
                    if tail is not None:
                        print('  PA6 high from %d ms on and still high: PA4 freed (the OS), or the slave reset' % tail)
                continue
            if fr is not None:
                second = fr
                break
            if not glob.glob('/dev/cu.usbmodem*'):
                print('port gone after %.1f s: the OS was entered, or the unit hung with USB torn '
                      'down (Power + M4 to recover)' % (time.monotonic() - t0))
                return 0
        if second is not None and len(second) >= 2 and second[0] == 1:
            print('hand-over returned to held mode: %s (code %d)'
                  % (reasons.get(second[1], 'unknown'), second[1]))
            return 0
        mon.rx = bytearray()
        mon.send(bytes([DEV_FLAGS]) + struct.pack('<BI', 0, 0))
        r = mon.next_frame(3.0)
        if r is not None and len(r) >= 5 and r[0] == 0:
            print('the unit is in held mode (options %08x): the hand-over returned without a failure '
                  'frame' % struct.unpack('<I', r[1:5])[0])
        else:
            print('the port is present but the unit does not answer: it is LOCKED (hung with USB left '
                  'running). Power + M4 to recover.')
        return 0
    elif c == 'dev-screen':
        scr = read_screen(mon.checked)
        if a[:1] == ['--json']:
            import json
            print(json.dumps(scr, indent=1))
        else:
            print(screen_text(scr))
    elif c == 'dev-key':
        for body in key_requests(parse_keys(a)):
            wait_keys(mon)
            mon.checked(body)
        wait_keys(mon)
    elif c == 'dev-shot':
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import ui_shots
        for _ in range(3):
            before = read_screen(mon.checked)['changes']
            fb = read_frame(mon.checked)
            if read_screen(mon.checked)['changes'] == before:
                break
            print('the screen changed while it was read: again', file=sys.stderr)
        ui_shots.frame_png(fb, a[0])
        print('%s: the %d-byte frame buffer' % (a[0], len(fb)))
    elif c == 'dev-bench':
        kib = num(a[0]) if a else 1024
        served, crc_served = {}, b''
        for stage, name in enumerate(BENCH_STAGES):
            r = mon.checked(bytes([DEV_BENCH]) + struct.pack('<BH', stage, kib), 300.0)
            _, size, cycles, engine = struct.unpack('<BIQB', r[:14])
            served[name] = engine
            if name == 'crc-16':
                crc_served = r[14:16]
            s = cycles / CCLK_HZ
            print('%-16s %9d bytes %9.1f ms %8.2f MiB/s %7.1f cycles/byte'
                  % (name, size, s * 1e3, size / 1048576 / s, cycles / size))
        # each stage answers for itself; the software stages never use the engine
        for name in ('sha-256', 'aes-256-cbc'):
            print('packet engine, %s: %s'
                  % (name, 'in use' if served[name] else 'not in use (software)'))
        print_crc(crc_served)
    elif c == 'dev-check':
        r = mon.checked(bytes([DEV_CHECK]), 1800.0)
        reason, size = struct.unpack('<BI', r[:5])
        cycles = struct.unpack('<%dQ' % len(CHECK_STAGES), r[5:5 + 8 * len(CHECK_STAGES)])
        name = CHECK_REASONS[reason] if reason < len(CHECK_REASONS) else str(reason)
        print('%d bytes: %s' % (size, 'checks 1-14 passed' if reason == 0 else 'refused ' + name.upper()))
        for stage, t in zip(CHECK_STAGES, cycles):
            print('%-13s %9.2f s %8.2f MiB/s' % (stage, t / CCLK_HZ, size / 1048576 / (t / CCLK_HZ) if t else 0.0))
        rest = cycles[-1] - sum(cycles[:-1])
        print('%-13s %9.2f s' % ('the rest', rest / CCLK_HZ))
        print('packet engine: %s' % ('in use' if r[5 + 8 * len(CHECK_STAGES)] else 'not in use (software)'))
        print_crc(r[6 + 8 * len(CHECK_STAGES):8 + 8 * len(CHECK_STAGES)])
        print_gap(r[8 + 8 * len(CHECK_STAGES):12 + 8 * len(CHECK_STAGES)])
    elif c == 'dev-extract':
        r = mon.checked(bytes([DEV_EXTRACT]), 3600.0)
        if len(r) == 1:
            raise SystemExit('dev-extract: refused')
        n_c, n_s = len(EXTRACT_COUNTS), len(EXTRACT_STAGES)
        reason, flags = r[0], r[1]
        counts = dict(zip(EXTRACT_COUNTS, struct.unpack('<%dI' % n_c, r[2:2 + 4 * n_c])))
        o = 2 + 4 * n_c
        size, = struct.unpack('<Q', r[o:o + 8])
        o += 8
        cycles = dict(zip(EXTRACT_STAGES, struct.unpack('<%dQ' % n_s, r[o:o + 8 * n_s])))
        o += 8 * n_s
        probes = [struct.unpack('<IQI', r[o + 16 * i:o + 16 * i + 16]) for i in range(len(PROBES))]
        crc_served = r[o + 16 * len(PROBES):o + 16 * len(PROBES) + 2]
        # the link the probes ran on (S-09.075): rung, fall-backs of requests, the probes' write KiB;
        # then the gap and the rungs tried; absent from older builds
        link = r[o + 16 * len(PROBES) + 2:o + 16 * len(PROBES) + 5]
        more = r[o + 16 * len(PROBES) + 9:o + 16 * len(PROBES) + 10]
        kib = link[2] if len(link) == 3 and link[2] else 4
        name = CHECK_REASONS[reason] if reason < len(CHECK_REASONS) else str(reason)
        print('%d bytes in %d files, %d directories: %s'
              % (size, counts['files'], counts['directories'],
                 'checks 1-18 passed' if reason == 0 else 'refused ' + name.upper()))
        # not an install's time: this request decrypts the zip again, in software
        own = {'decryption': '  this request\'s own: not part of an install',
               'whole extraction': '  with this request\'s own decryption',
               'checks 1-15': '  before the extraction; an install decrypts here'}
        for stage in EXTRACT_STAGES:
            t = cycles[stage]
            print('%-17s %9.2f s %8.2f MiB/s %7.1f cycles/byte%s'
                  % (stage, t / CCLK_HZ, size / 1048576 / (t / CCLK_HZ) if t else 0.0, t / size if size else 0.0,
                     own.get(stage, '')))
        print('%-17s %9.2f s   the master\'s time in an install\'s extraction'
              % ('as an install', (cycles['whole extraction'] - cycles['decryption']) / CCLK_HZ))
        rest = cycles['whole extraction'] - sum(cycles[s] for s in EXTRACT_STAGES[:6])
        print('%-17s %9.2f s   (the sink but its CRC-32, the zip64 check, the space rule)'
              % ('the rest', rest / CCLK_HZ))
        print('inflate computes no CRC-32: the extraction\'s is the only one')
        print(', '.join('%s %d' % (k, counts[k]) for k in EXTRACT_COUNTS[2:]))
        for bit, text in EXTRACT_FLAGS:
            if flags & bit:
                print(text)
        if len(link) == 3:
            # S-09.078: where the extraction's requests and the probes ran; the link is plain after
            where = RUNGS[link[0]] if link[0] < len(RUNGS) else 'rung %d' % link[0]
            print('bridge: requests ran in %s (rung %d)' % (where, link[0]))
            if link[1]:
                print('bridge: %d fall-back(s) of requests in this boot: the record below says where and why'
                      % link[1])
            print('bridge: the link was lowered to plain when it ended')
        each = {}
        for label, (sent, t, err) in zip(PROBES, probes):
            if not sent:
                continue
            us = t / CCLK_HZ * 1e6 / sent
            each[label] = us
        if each and len(link) == 3:
            # what the probes were: say it so that it cannot be read as plain or as a full install
            print('probes, on rung %d (%s): %d file writes of %d KiB each, refused EBADF, and %d stats'
                  % (link[0], where, probes[0][0], kib, probes[1][0]))
        for label in PROBES:
            if label in each:
                sent, _, err = probes[PROBES.index(label)]
                print('bridge %-13s %3d requests %9.1f us each (errno %d)'
                      % ('write %d KiB' % kib if label == 'write' else label, sent, each[label], err))
        if 'write' in each and 'stat' in each:
            # a stat is a request with next to no payload: what every request costs
            per_byte = (each['write'] - each['stat']) / (kib * 1024)
            print('of a %d KiB write: %.1f us is the request itself (a stat\'s time), %.1f us its bytes'
                  ' (%.1f us per 4 KiB)' % (kib, each['stat'], each['write'] - each['stat'], per_byte * 4096))
            if len(link) == 3 and link[0]:
                plain_4k = (12 + 4096) / 4 * PLAIN_WORD_US
                print('  the plain wire would take %.1f us per 4 KiB of bytes (32 bits at 25 MHz a word)'
                      % plain_4k)
            # the real extraction reads a zip the check decrypted already
            master = (cycles['whole extraction'] - cycles['decryption']) / CCLK_HZ
            wire, reqs, small = bridge_estimate(size, counts, each['write'], each['stat'], kib)
            print('bridge %.2f s: %d bytes x %.4f us + %d requests x %.1f us'
                  % (wire, size, per_byte, reqs, each['stat']))
            print('  (requests: %d writes and %d small: mkdir, stat, open and close, the free-space query; '
                  'not the staging area\'s reset)' % (counts['write requests'], small))
            print('estimate %.2f s: master %.2f s + bridge %.2f s' % (master + wire, master, wire))
            print('  excludes the slave\'s storage time, which this request cannot measure without writing')
        print_crc(crc_served)
        print_gap(r[o + 16 * len(PROBES) + 5:o + 16 * len(PROBES) + 9])
        if len(more) == 1:
            show_link_record(mon)
    elif c == 'dev-link-record':
        show_link_record(mon)
    elif c == 'dev-start':
        show_start(mon)
    elif c == 'dev-cal-start':
        cal_start(mon)
    elif c == 'dev-stream':
        show_stream(mon)
    elif c == 'dev-quad-data':
        show_quad_data(mon)
    elif c == 'dev-ckpt':
        show_ckpt(mon)
    elif c == 'dev-link-test':
        n = num(a[0]) if a else LINK_TEST_N
        if not 1 <= n <= 255:
            raise SystemExit('dev-link-test: N is 1 to 255')
        show_link_test(mon, n)
    elif c == 'dev-quad-reply':
        quad_reply(mon, a)
    elif c == 'encode':
        print(frame(bytes([num(a[0])]) + bytes.fromhex(a[1] if len(a) > 1 else ''), 1).hex())
    elif c == 'continue-os' and a == ['--sysex']:
        print(continue_sysex().hex(' '))
    elif c == 'continue-os' and not a:
        return continue_os(mon)
    else:
        print(__doc__)
        return 2
    return 0


def bridge_estimate(size, counts, write_us, stat_us, kib):
    """The bridge's share of an extraction, in seconds, from dev-extract's counts and probes: the bytes the
    staging writes carried at the probe write's cost per byte (its time less a stat's, over its bytes), and
    every request's own cost at a stat's time. Returns (seconds, requests, small requests)."""
    per_byte = (write_us - stat_us) / (kib * 1024)
    small = counts['mkdir requests'] + counts['stat requests'] + 2 * counts['files'] + 1
    reqs = counts['write requests'] + small
    return (size * per_byte + reqs * stat_us) / 1e6, reqs, small


def rung_name(r):
    if r == 0xFF:
        return 'nowhere (the slave is absent)'
    return 'rung %d (%s)' % (r, RUNGS[r] if r < len(RUNGS) else '?')


def driver_text(words, master=True):
    """The SPI driver's own account of a failure: its rc, status, words left and stage, by name."""
    rc, stat, left, stage = words
    name = SPI_STAGES[stage & 0xFF] if (stage & 0xFF) < len(SPI_STAGES) else 'stage %d' % (stage & 0xFF)
    if stage & 0xFF == 7:
        name += ' (ERRC %d)' % (stage >> 8 & 7)
    hold = '; the slave\'s ready line was holding the master' if master and stat & 1 << 20 else ''
    return '[%s; rc %d, status 0x%x, %d words left%s]' % (name, rc - (1 << 32) if rc >> 31 else rc, stat,
                                                        left, hold)


def fault_text(at, words, more=None):
    t = AT_TEXT[at] if at < len(AT_TEXT) else 'stage %d' % at
    if at in (1, 4):        # a send or a receive: the driver's own account
        t += ' ' + driver_text(words)
        if more and any(more):
            t += ' ' + dma_text(more, at == 1)
    return t


DMA_RUN = ['idle', 'fetching a descriptor', 'moving data', 'waiting for a trigger',
           'waiting for its writes to drain']


def dma_text(more, send=True):
    """The driver's second four words: the DMA channel's DMA_STAT and DMA_CFG, then SPI_TXCTL and SPI_TWC
    after a send or SPI_RXCTL and SPI_RWC after a receive, as read at the failure, by field."""
    stat, cfg, xctl, count = more
    run = stat >> 8 & 7
    return ('[DMA %s%s%s, error cause %d (status 0x%x); configuration 0x%x: %s, %s; %s control 0x%x, '
            '%d word(s) still to %s]' % (
                DMA_RUN[run] if run < len(DMA_RUN) else 'run state %d' % run,
                ', done' if stat & 1 else ', not done', ', ERROR' if stat & 2 else '', stat >> 4 & 7, stat,
                cfg, 'enabled' if cfg & 1 else 'disabled',
                'its interrupt asked for' if cfg & (1 << 20) else 'no interrupt asked for',
                'transmit' if send else 'receive', xctl, count, 'send' if send else 'receive'))


def fault_more(b):
    """A fault with the driver's second four words, as the record keeps a link test's: at:1 then 8 words."""
    return b[0], struct.unpack('<4I', b[1:17]), struct.unpack('<4I', b[17:33])


def rr_text(r):
    return RR_TEXT[r] if r < len(RR_TEXT) else 'result %d' % r


def bench_fault_text(e):
    """One of the last link test's faults, as the record keeps it (BENCH_FAULT bytes)."""
    no, kind, rung, res, length, req, to, fall_us = struct.unpack('<HBBBIHBI', e[:16])
    at, words, more = fault_more(e[16:49])
    eat, ewords, emore = fault_more(e[49:82])
    if kind == 1:
        line = '  fault %d: entering %s: ' % (no + 1, rung_name(rung))
        if res == RR_NONE:
            line += 'the slave was lost while the link was lowered: %s' % fault_text(at, words, more)
        elif res == 2:
            line += 'the test of %d bytes failed: %s' % (length, fault_text(at, words, more))
        elif res in (1, 3):
            line += '%s: %s' % (rr_text(res), fault_text(at, words, more))
        else:
            line += rr_text(res)
    else:
        line = '  fault %d: %s, request %d of %d bytes: %s' % (
            no + 1, rung_name(rung), req + 1, length, fault_text(at, words, more))
    lines = [line]
    if to == 0xFF:
        lines.append('    THE SLAVE WAS LOST: no recovery echo answered in %.1f ms; the last: %s' % (
            fall_us / 1000.0, 'none could be sent (no free slot)' if eat == 0 else fault_text(eat, ewords, emore)))
    elif fall_us:
        lines.append('    the link came back to %s after %.1f ms of echoes' % (rung_name(to), fall_us / 1000.0))
    if e[82]:
        lines.append('    ' + status_text(e[82:114]))
    return lines


def status_text(b):
    """The slave's 0x1B answer as the record keeps it: told fell why cmd mode len got and 4 words."""
    told, fell, why, cmd, mode, length, got = struct.unpack('<BIBBBII', b[:16])
    words = struct.unpack('<4I', b[16:32])
    if not told:
        return 'the slave did not say'
    t = 'the slave: %s' % (WHY_TEXT[why] if why < len(WHY_TEXT) else 'cause %d' % why)
    if why:
        t += ' on 0x%02x of %d bytes, %d in, in mode %d' % (cmd, length, got, mode)
    if why == 4:
        t += ' ' + driver_text(words, False)
    return t + '; its fall-backs %d' % fell


def show_link_record(mon):
    """OPXY_DEV_LINK_RECORD's parts, in plain words."""
    parts = []
    for p in range(RECORD_PARTS):
        try:
            parts.append(mon.checked(bytes([DEV_LINK_RECORD, p])))
        except SystemExit:
            print('the link\'s record: not in this bootloader')
            return
    s = parts[0]
    falls, negs, tried, kept, rung, fell, settled, bench = struct.unpack('<IIBBBIBI', s[3:23])
    print('link record: %d negotiation(s); the last tried %d rung(s) and ended on %s; requests now on %s'
          % (negs, tried, rung_name(kept), rung_name(rung)))
    print('link record: %d fall-back(s) of requests in this boot; %d fall-back(s) of the link in all'
          % (falls, fell))
    if not settled:
        print('link record: ' + RESTART)
    for p in parts[1:1 + RECORD_RUNG_PARTS]:
        for i in range(p[2]):
            e = p[3 + 72 * i:3 + 72 * (i + 1)]
            r, res, pat, length, at = struct.unpack('<BBBIB', e[:8])
            words = struct.unpack('<4I', e[8:24])
            big = struct.unpack('<3I', e[24:36])
            plain, = struct.unpack('<I', e[36:40])
            line = '  tried %s: ' % rung_name(r)
            if res == 2:
                line += 'the test of %d bytes (%s) failed: %s' % (
                    length, PATTERNS[pat] if pat < len(PATTERNS) else pat, fault_text(at, words))
            elif res in (1, 3):
                line += '%s: %s' % (RR_TEXT[res], fault_text(at, words))
            elif res == 5:
                line += RR_TEXT[res]
            else:
                line += 'tests passed'
            if plain:
                line += '; 64 KiB sessions %s us, the least against the plain wire\'s %d us (%.3f)' % (
                    '/'.join(str(b) for b in big), plain, min(big) / plain)
            elif r > 1 and res == 0:
                line += '; its gate passed earlier in this boot'
            if res in (0, 4):
                line += ': ' + RR_TEXT[res]
            print(line)
            if e[40]:
                print('    ' + status_text(e[40:72]))
    for p in parts[1 + RECORD_RUNG_PARTS:1 + RECORD_RUNG_PARTS + RECORD_FALL_PARTS]:
        for i in range(p[2]):
            f = p[3 + 73 * i:3 + 73 * (i + 1)]
            frm, to, cmd, tries, length = struct.unpack('<BBBBI', f[:8])
            at, words, more = fault_more(f[8:41])
            print('  request 0x%02x of %d bytes (%s) fell back from %s to %s: %s'
                  % (cmd, length, 'sent again' if tries else 'its first send', rung_name(frm), rung_name(to),
                     fault_text(at, words, more)))
            print('    ' + status_text(f[41:73]))
    if bench:
        print('link record: the last link test had %d fault(s)%s' % (
            bench, '; its first 12 and its last 4 are kept' if bench > 16 else ''))
    for p in parts[1 + RECORD_RUNG_PARTS + RECORD_FALL_PARTS:]:
        for i in range(p[2]):
            for line in bench_fault_text(p[3 + BENCH_FAULT * i:3 + BENCH_FAULT * (i + 1)]):
                print(line)


# OPXY_DEV_BOOT_START's report (waffle/board/opxy_services.h), and the words S-15.163's main screen and
# Device Info use for it (src/s15_lines.c), so the host reads what the unit showed.
START_LAYOUT = '<7B7I6BI4I'
START_FIELDS = ('attempted', 'cause', 'result', 'step', 'step_part', 'part', 'select0_failed', 'sector',
                'count', 'format', 'length', 'crc', 'image_crc', 'boot_size', 'refusal', 'replied', 'reply',
                'lost', 'cmd', 'at', 'arg0')
START_CAUSES = ['none', 'no OS (S-05.006)', 'the image marked encrypted', 'a failed select or read (S-05.007)',
                "S-05.024's refusal", "the slave's release (S-05.021)", "the slave's OS did not start (S-05.032)"]
START_RESULTS = ['loaded', 'no active partition', 'length 0', 'too long', 'past the partition', 'format refused',
                 'encrypted', 'CRC mismatch', 'a select or read failed']
START_STEPS = ['none', 'a select', 'a header read', 'an image read', "the user area's select"]
START_WHYS = ['OK', 'NO_ACTIVE', 'SELECT', 'HEADER_READ', 'IMAGE_READ', 'USER_SELECT', 'FORMAT', 'ZERO_LENGTH',
              'TOO_LONG', 'PAST_PARTITION', 'CRC', 'ENCRYPTED', 'REFUSED', 'SLAVE_NO_OS', 'NO_REPLY',
              'SLAVE_START']
# S-05.024's rules, by S05_REFUSE_... less one (S15_CHECK_...): the main screen's words, then Device Info's
CHECKS = ['LENGTH', 'PARSE', 'FLAGS', 'SPACE', 'STAGING', 'RESERVE', 'RECORD', 'BOOT_RESERVE']
CHECK_REASON = ['the image is too long to stage', "the image's block headers are not valid",
                'the image uses a block flag not allowed', 'the image loads outside memory',
                'the image loads over the staging area', 'the image loads over the hand-over reserve',
                'the image loads over the reboot record', "the image loads over the ROM's boot reserve"]
CHECK_INFO = ['too long', 'block headers', 'block flags', 'outside memory', 'over staging', 'over reserve',
              'over reboot record', 'over boot reserve']
START_AT = ['lost', 'send failed', 'no ready', 'no reply', 'reply broke', 'bad reply', 'bad check', 'timed out',
            'slave fell', 'bad value']
START_NONE = 'the last plain start did not fail, or none was tried'
S03_OS_MAX = 0x800000


def start_fields(r):
    """The report's fields by name, 'fault' the driver's four words."""
    v = struct.unpack(START_LAYOUT, bytes(r[:START_REPORT]))
    d = dict(zip(START_FIELDS, v))
    d['fault'] = v[len(START_FIELDS):]
    return d


def start_view(d):
    """The report in S-15's terms, as opxy_start_view() makes them (screens.json's `start` keys)."""
    v = {'why': 'OK', 'part': 0, 'value': 0, 'limit': 0, 'check': 'SPACE', 'lost': bool(d['lost']),
         'cmd': d['cmd'], 'at': d['at']}
    cause, result = d['cause'], d['result']
    if not d['attempted'] or cause == 0:
        v.update(lost=False, cmd=0, at=0)
    elif cause == 2:
        v.update(why='ENCRYPTED', part=d['part'])
    elif cause == 3:
        v['part'] = d['step_part']
        v['why'] = {2: 'HEADER_READ', 3: 'IMAGE_READ', 4: 'USER_SELECT'}.get(d['step'], 'SELECT')
        if d['step'] == 3:
            v.update(value=d['sector'], limit=d['count'])
    elif cause == 4:
        v['why'] = 'REFUSED'
        v['check'] = CHECKS[d['refusal'] - 1] if 1 <= d['refusal'] <= len(CHECKS) else 'SPACE'
    elif cause == 5:
        v.update(why='SLAVE_NO_OS' if d['replied'] else 'NO_REPLY', value=d['reply'])
    elif cause == 6:
        v['why'] = 'SLAVE_START'
    elif result == 2:
        v['why'] = 'ZERO_LENGTH'
    elif result == 3:
        v.update(why='TOO_LONG', value=d['length'], limit=S03_OS_MAX)
    elif result == 4:
        v.update(why='PAST_PARTITION', value=-(-d['length'] // 512) + 1, limit=d['boot_size'] // 512)
    elif result == 5:
        v.update(why='FORMAT', value=d['format'])
    elif result == 7:
        v.update(why='CRC', value=d['image_crc'], limit=d['crc'])
    else:
        v['why'] = 'NO_ACTIVE'
    return v


def start_lost(v):
    return v.get('lost', False) and v['why'] in ('SELECT', 'HEADER_READ', 'IMAGE_READ', 'USER_SELECT', 'NO_REPLY')


def start_reason(v):
    """The main screen's reason (s15_start_reason())."""
    w, lost = v['why'], start_lost(v)
    part, value = v.get('part', 0), v.get('value', 0)
    if w == 'NO_ACTIVE':
        t = 'no boot partition is active'
    elif w == 'SELECT':
        t = 'at the select' if lost else 'boot partition %d could not be selected' % part
    elif w == 'HEADER_READ':
        t = 'at the header read' if lost else 'the header of partition %d could not be read' % part
    elif w == 'IMAGE_READ':
        t = 'during the image read' if lost else 'the image could not be read'
    elif w == 'USER_SELECT':
        t = 'at the last select' if lost else 'the user area could not be selected'
    elif w == 'FORMAT':
        t = 'header format %d is not supported' % value
    elif w == 'ZERO_LENGTH':
        t = 'the header gives the image no length'
    elif w == 'TOO_LONG':
        t = 'the image is longer than 8 MiB'
    elif w == 'PAST_PARTITION':
        t = 'the image runs past the end of its partition'
    elif w == 'CRC':
        t = "the image's CRC does not match its header"
    elif w == 'ENCRYPTED':
        t = 'the image is encrypted'
    elif w == 'REFUSED':
        t = CHECK_REASON[CHECKS.index(v.get('check', 'LENGTH'))]
    elif w == 'SLAVE_NO_OS':
        t = 'DSP2 could not load the OS'
    elif w == 'NO_REPLY':
        t = 'at the release' if lost else 'DSP2 did not answer the release'
    elif w == 'SLAVE_START':
        t = "DSP2's OS did not start"
    else:
        return ''
    return ('DSP2 stopped answering ' if lost else '') + t


def start_info(v):
    """Device Info's OS start value (s15_start_info())."""
    w, lost = v['why'], start_lost(v)
    part, value, limit = v.get('part', 0), v.get('value', 0), v.get('limit', 0)
    if w == 'NO_ACTIVE':
        t = 'No active boot partition'
    elif w == 'SELECT':
        t = 'Select of partition %d' % part if lost else 'Partition %d select failed' % part
    elif w == 'HEADER_READ':
        t = 'Header of partition %d' % part if lost else 'Partition %d header read failed' % part
    elif w == 'IMAGE_READ':
        t = 'Sector %d read' % value if lost else 'Partition %d sector %d read failed' % (part, value)
    elif w == 'USER_SELECT':
        t = 'User area select' if lost else 'User area select failed'
    elif w == 'FORMAT':
        t = 'Header format %d not supported' % value
    elif w == 'ZERO_LENGTH':
        t = 'Header length 0'
    elif w == 'TOO_LONG':
        t = 'Length %d, at most %d' % (value, limit)
    elif w == 'PAST_PARTITION':
        t = 'Needs %d sectors, has %d' % (value, limit)
    elif w == 'CRC':
        t = 'CRC %08x, header says %08x' % (value, limit)
    elif w == 'ENCRYPTED':
        t = 'Partition %d image is encrypted' % part
    elif w == 'REFUSED':
        t = 'Hand-over check: ' + CHECK_INFO[CHECKS.index(v.get('check', 'LENGTH'))]
    elif w == 'SLAVE_NO_OS':
        t = 'DSP2 answered no OS, reply %d' % value
    elif w == 'NO_REPLY':
        t = 'Release' if lost else 'No reply to the release'
    elif w == 'SLAVE_START':
        t = "DSP2's OS did not start"
    else:
        return ''
    if lost:
        at = v.get('at', 0) & 0x7F
        t += ': 0x%02X %s' % (v.get('cmd', 0), START_AT[at] if at < len(START_AT) else START_AT[0])
    return t


def start_lines(r):
    """OPXY_DEV_BOOT_START's reply (after the status byte) in plain words."""
    if len(r) < START_REPORT or not r[0]:
        return [START_NONE]
    d = start_fields(r)
    v = start_view(d)
    out = ['last plain start: The OS could not be started.']
    if v['why'] == 'ENCRYPTED':
        out.append('  reason:   OS needs reinstalling: install a TE firmware update')
    else:
        out.append('  reason:   ' + start_reason(v))
    out.append('  OS start: ' + start_info(v))
    cause = START_CAUSES[d['cause']] if d['cause'] < len(START_CAUSES) else 'cause %d' % d['cause']
    result = START_RESULTS[d['result']] if d['result'] < len(START_RESULTS) else 'result %d' % d['result']
    out.append('  load:     %s; cause: %s' % (result, cause))
    if d['step']:
        step = '  step:     %s of partition %d' % (
            START_STEPS[d['step']] if d['step'] < len(START_STEPS) else 'step %d' % d['step'], d['step_part'])
        if d['step'] == 3:
            step += ', %d sector(s) from %d' % (d['count'], d['sector'])
        out.append(step)
    elif d['part']:
        out.append('  step:     none failed; partition %d chosen' % d['part'])
    if d['select0_failed']:
        out.append("  step:     the user area's select after it failed too")
    if d['format'] or d['length'] or d['crc']:
        line = "  header:   format %d, length %d bytes, CRC %08x" % (d['format'], d['length'], d['crc'])
        if d['result'] == 7:
            line += "; the image's CRC %08x" % d['image_crc']
        if d['boot_size']:
            line += '; the partition %d sectors' % (d['boot_size'] // 512)
        out.append(line)
    if d['cause'] == 4:
        out.append('  refusal:  %s (rule %d of S-05.024)' % (CHECK_REASON[CHECKS.index(v['check'])], d['refusal']))
    if d['cause'] == 5:
        out.append('  release:  ' + ('reply %d' % d['reply'] if d['replied'] else 'no reply'))
    if d['lost']:
        at = d['at'] & 0x7F
        out.append('  bridge:   the slave was lost on request 0x%02x (arg 0x%x)%s: %s %s' % (
            d['cmd'], d['arg0'], ' in a poll for it' if d['at'] & 0x80 else '',
            AT_TEXT[at] if at < len(AT_TEXT) else 'stage %d' % at, driver_text(d['fault'])))
    return out


def show_start(mon):
    """OPXY_DEV_BOOT_START, in plain words."""
    try:
        r = mon.checked(bytes([DEV_BOOT_START]))
    except SystemExit:
        print("the failed start's report: not in this bootloader")
        return
    for line in start_lines(r):
        print(line)


def stream_lines(r):
    """OPXY_DEV_STREAM's reply (after the status byte) in plain words."""
    if not ((r[:1] == b'\x01' and len(r) >= STREAM_REPORT_V1) or (r[:1] == b'\x02' and len(r) >= STREAM_REPORT)):
        return ["this start's stream: no report (an older bootloader, or not this build's)"]
    way, rc, end, fell, linked, held, ran = r[1], r[2], r[3], r[4], r[5], r[6], r[7]
    (sessions, parts, fast, slow, fell_session, fell_at, us, guard_us, wait_us, wait_max,
     high, guard) = struct.unpack_from('<12I', r, 8)
    fault = struct.unpack_from('<4I', r, 56)
    if not ran:
        return ["this start's stream: not sent in this start"]
    out = ["this start's stream: %s" % (STREAM_WAYS[way] if way < len(STREAM_WAYS) else 'way %d' % way)]
    line = '  outcome:  %s' % (STREAM_OUTCOMES[rc] if rc < len(STREAM_OUTCOMES) else 'outcome %d' % rc)
    if rc == 0:
        line += ', ended at %s' % (STREAM_ENDS[end] if end < len(STREAM_ENDS) else 'end %d' % end)
    out.append(line + '; %.3f ms from the wait for the ready line' % (us / 1000.0))
    out.append('  bridge:   %s' % ('linked' if linked else 'not linked: the slave did not answer'))
    if way == 2:
        out.append('  sent:     %d sessions, %d parts by DMA, %d bytes by the controller, %d one at a time'
                   % (sessions, parts, fast, slow))
        out.append('  pacing:   guard %d us, %.3f ms in the guards; %d parts began with the line high, '
                   '%.3f ms waited, the longest %d us' % (guard, guard_us / 1000.0, high, wait_us / 1000.0,
                                                           wait_max))
        if r[0] >= 2:
            samples, fcs, fcs_parts, first = struct.unpack_from('<4I', r, 72)
            out.append('  held:     SPI_STAT.FCS set in %d of %d samples, in %d parts%s' % (
                fcs, samples, fcs_parts, ', the first at byte %d' % (first - 1) if first else ''))
        if fell:
            out.append('  fell back at byte %d of session %d: %s; the rest one byte at a time' % (
                fell_at, fell_session, STREAM_CAUSES[fell] if fell < len(STREAM_CAUSES) else 'cause %d' % fell))
            out.append('  driver:   %s' % driver_text(list(fault)))
        else:
            out.append('  fell back: no')
        if held:
            out.append('  the slave held the ready line past T_STREAM')
    return out


def show_stream(mon):
    """OPXY_DEV_STREAM, in plain words."""
    try:
        r = mon.checked(bytes([DEV_STREAM]))
    except SystemExit:
        print("this start's stream: not in this bootloader (a byte-by-byte build, BOOT_LINK_FC=0, or older)")
        return
    for line in stream_lines(r):
        print(line)


def quad_data_lines(r):
    """OPXY_DEV_QUAD_DATA's reply (after the status byte) in plain words."""
    if len(r) < QDATA_REPLY - 1:
        return ["this start's quad data replies: no report (an older bootloader)"]
    (state, why, rung, sent, whole, first, test_len, test_us, replies, nbytes, failed, at,
     rc, stat, left, stage) = struct.unpack_from(QDATA_LAYOUT, r)
    word = lambda t, k: t[k] if k < len(t) else '%d' % k
    out = ["this start's quad data replies: %s%s" % (
        word(QDATA_STATES, state), ': ' + word(QDATA_WHY, why) if why else '')]
    if state == 0:
        return out
    line = '  test:     %d of %d test replies whole with their flag, %.3f ms' % (whole, sent, test_us / 1000.0)
    if why == 2:
        line += '; first failure %s at %d bytes' % (
            'refused, value %d' % (first & 0x7F) if first & 0x80 else word(QDATA_FIRST, first), test_len)
    out.append(line)
    out.append('  data:     %d sessions in quad I/O at 50 MHz, %d bytes; %d failed' % (replies, nbytes, failed))
    out.append('  link:     rung %d now' % rung)
    if state == 3 and why in (6, 7):
        out.append('  broke:    %s %s' % (word(AT_TEXT, at), driver_text([rc, stat, left, stage])))
    return out


CKPT_OUTCOME = {0: 'done', 1: "refused: the master's heap has no room for a second device",
                2: 'the test mount failed', 3: 'the volume is not on the device'}


def ckpt_lines(r, texts):
    """OPXY_DEV_CKPT's summary (after the status byte) and its texts, in plain words."""
    outcome, accepted, unmounted, cut, alloc_used = r[0], r[1], r[2], r[3], r[4]
    w = struct.unpack_from('<14I', r, 5)
    (blocks, reads, sectors, mount_us, refused, objs_scan, objs_ckpt, diffs, seq, alloc_block,
     alloc_page, newer, disagree, test_us) = w
    if alloc_block >= 1 << 31:
        alloc_block -= 1 << 32
    out = ['checkpoint test: %s, %.3f s in all' % (CKPT_OUTCOME.get(outcome, outcome), test_us / 1e6)]
    if outcome not in (0, 2):
        return out
    out.append('checkpoint: %s; %s by yaffs2%s' % (
        '%d blocks on the volume' % blocks if blocks else 'none on the volume',
        'taken' if accepted else 'not taken', '' if accepted else ' (%s)' % texts[0]))
    out.append('the test mount: %d reads (%d sectors) in %.3f ms' % (reads, sectors, mount_us / 1e3))
    if outcome == 2:
        out.append('writes refused: %d' % refused)
        return out
    out.append('trees: %d objects scanned, %d restored%s; %d differences' % (
        objs_scan, objs_ckpt, ' (cut short)' if cut else '', diffs))
    d = min(diffs, 4)
    out += ['  ' + t for t in texts[1:1 + d]]
    if accepted:
        out.append('stale guard: checkpoint sequence %#x, allocating block %d page %d (%s); %d blocks '
                   'newer than it, %d blocks disagree with its states%s' % (
                       seq, alloc_block, alloc_page, 'in use: written since' if alloc_used else 'unused',
                       newer, disagree, '' if (newer or disagree or alloc_used) else ': not stale'))
        out += ['  ' + t for t in texts[1 + d:]]
    out.append('writes refused: %d; test device %s' % (refused, 'unmounted' if unmounted else
                                                          'NOT unmounted'))
    return out


def show_ckpt(mon):
    """OPXY_DEV_CKPT: the test, then its texts, in plain words."""
    r = mon.checked(bytes([DEV_CKPT, 0]), CKPT_TIMEOUT)
    if len(r) < 1:
        raise SystemExit('dev-ckpt: a short reply')
    if len(r) < CKPT_SUMMARY:
        print('checkpoint test: %s' % CKPT_OUTCOME.get(r[0], r[0]))
        return
    texts = []
    for i in range(r[CKPT_SUMMARY - 1]):
        t = mon.checked(bytes([DEV_CKPT, 1 + i]))
        texts.append(t.split(b'\0', 1)[0].decode('utf-8', 'replace'))
    for line in ckpt_lines(r, texts or ['']):
        print(line)


def show_quad_data(mon):
    """OPXY_DEV_QUAD_DATA, in plain words."""
    try:
        r = mon.checked(bytes([DEV_QUAD_DATA]))
    except SystemExit:
        print("this start's quad data replies: not in this bootloader (an older one)")
        return
    for line in quad_data_lines(r):
        print(line)


def cal_start(mon):
    """OPXY_DEV_CAL_START: the answer comes before the restart."""
    mon.checked(bytes([DEV_CAL_START]))
    print('restarting: the next start shows the calibration screen around the OS\'s first screen;'
          ' photograph it')


def entry_text(entry, times):
    """A link test's rung as its run entered it: the last entry's result and the times entered."""
    if entry == RR_NONE and not times:
        return 'not tried'
    if entry == 0:
        return 'entered %d time(s)' % times
    if times:
        return 'entered %d time(s), then not again: %s' % (times, rr_text(entry))
    return 'could not be entered: %s' % rr_text(entry)


def show_link_test(mon, n):
    """OPXY_DEV_LINK_TEST: the run, one pass (S-09.075), then its table."""
    r = mon.checked(bytes([DEV_LINK_TEST, 0, n]), LINK_TEST_LIMIT_S + 60.0)
    if len(r) < LINK_TEST_REPLY:
        raise SystemExit('dev-link-test: refused (an install, a scan or a long operation, or no slave), '
                         'or a bootloader older than this tool')
    parts, count, flags = r[0], r[1], r[2]
    entry, entries = r[3:8], r[8:13]
    secs = struct.unpack('<Q', r[13:21])[0] / CCLK_HZ
    falls, limit = struct.unpack('<HH', r[21:25])
    stopped, end_rung, end_len, end_req, faults = struct.unpack('<BBIHI', r[25:37])
    print('link test: %d request(s) a length, %.1f s in all (the bound %d s), %d fall-back(s), %d fault(s)%s'
          % (count, secs, limit, falls, faults, ' (dev-link-record has each)' if faults else ''))
    if flags & 2:
        print('link test: CUT SHORT by its bound: the lengths not reached show no requests')
    if flags & 1:
        print('link test: THE SLAVE STOPPED ANSWERING: restart the unit')
    elif flags & 4:
        print('link test: ' + RESTART)
    if stopped:
        print('link test: it ended on %s, at request %d of %d bytes' % (
            rung_name(end_rung), end_req + 1, end_len))
    for k in range(1, 5):
        print('  %s: %s' % (rung_name(k), entry_text(entry[k], entries[k])))
    rows = []
    for p in range(parts):
        t = mon.checked(bytes([DEV_LINK_TEST, 1, p]))
        for i in range(t[2]):
            rows.append(t[3 + LINK_TEST_ROW * i:3 + LINK_TEST_ROW * (i + 1)])
    print('%-38s %8s %7s %10s %10s %8s  %s' % ('rung', 'bytes', 'passed', 'us/request', 'us/4 KiB',
                                               'vs plain', 'faults'))
    for row in rows:
        rung, length, sent, passed, us = struct.unpack('<BIHHI', row[:13])
        at = struct.unpack('<10H', row[13:33])
        if not sent:
            continue
        each = us / passed if passed else 0.0
        per4k = each * 4096 / length if length else 0.0
        plain = (12 + ((length + 3) & ~3)) / 4 * PLAIN_WORD_US
        faults = ', '.join('%s %d' % (AT_SHORT[k], at[k]) for k in range(10) if at[k]) or '-'
        if sent < count:
            faults += '; %d not sent' % (count - sent)
        print('%-38s %8d %3d/%-3d %10.1f %10.1f %8.2f  %s' % (
            rung_name(rung), length, passed, sent, each, per4k, each / plain if passed else 0.0, faults))
    print('vs plain: a request\'s time over the plain wire\'s for its words (32 bits at 25 MHz a word);'
          ' under 1 is faster than plain')


def quad_parse(r):
    """OPXY_DEV_QUAD_REPLY's answer after its status byte, as a dict."""
    if len(r) < QUAD_REPLY:
        raise SystemExit('dev-quad-reply: a short answer: a bootloader older than this tool')
    v = struct.unpack(QUAD_LAYOUT, r[:QUAD_REPLY])
    d = dict(zip(['cand', 'ren', 'clock', 'len', 'n', 'flags', 'why', 'sent', 'whole', 'flag', 'missing',
                  'last_wrong', 'word_wrong', 'broke', 'no_record'], v[:15]))
    d['ends'] = list(v[15:23])
    (d['dmin'], d['dmax'], d['lines'], d['m_or'], d['m_and'], d['s_or'], d['cycles'], d['first'],
     d['first_no'], d['first_word']) = v[23:33]
    d['first_m'] = list(v[33:37])
    d['first_rec'] = dict(zip(QUAD_REC, v[37:55]))
    return d


def clock_text(clock):
    return '%d MHz, fast mode %s' % (50 if clock & 1 else 25, 'on' if clock & 2 else 'off')


def cand_text(cand, ren):
    return '%s%s' % (QUAD_CAND_TEXT.get(cand, 'candidate %d' % cand), ', REN set' if ren else '')


def quad_delay(d):
    if d['dmin'] == QUAD_NONE:
        return '-'
    return '%d-%d' % (d['dmin'], d['dmax'])


def quad_lines(lines):
    return ','.join('D%d' % k for k in range(4) if lines >> k & 1) or '-'


def quad_rec_text(r):
    """The slave's record of a test reply (S09_QR_...), in words."""
    if not r['arm'] and not r['len']:
        return "the slave's record: none"

    def at(v):
        return '-' if v == QUAD_NONE else '%.1f us' % (v / CCLK_HZ * 1e6)
    end = r['end'] & 0xFF
    seen = ', '.join(t for b, t in QUAD_SEEN if r['end'] & b) or 'nothing'
    spif = r['spif']
    return ("the slave: ended by %s; saw %s; SPI_STAT %08x at the end (or-ed %08x, at the bound %08x), "
            "SPI_TWC %d, DMA_STAT %08x; from the hand-over: began %s, fed %s, flag %s, end %s; "
            "SPIF fell %d and rose %d time(s); %d word(s) received; stage %d" % (
                QUAD_ENDS[end] if end < len(QUAD_ENDS) else end, seen, r['stat'], r['stat_or'],
                r['stat_bound'], r['twc'], r['dma'], at(r['t_start']), at(r['t_feed']), at(r['t_flag']),
                at(r['t_end']), spif & 0xFFFF, spif >> 16, r['rx'], r['stage'] & 0xFF))


def quad_fail_text(d):
    """A cell's first failure, in words; '' when it had none."""
    k = d['first']
    if not k:
        return ''
    what = QUAD_FIRST[k] if k < len(QUAD_FIRST) else str(k)
    if d['first_word'] != QUAD_NONE:
        what += ' (word %d of %d first)' % (d['first_word'], d['len'] // 4)
    return 'trial %d: %s; the master: SPI_STAT %08x, %d word(s) left, %d cycles; %s' % (
        d['first_no'], what, d['first_m'][0], d['first_m'][1], d['first_m'][2],
        quad_rec_text(d['first_rec']))


def quad_verdict(cells):
    """A candidate's verdict over its cells, in the order run."""
    sent = sum(c['sent'] for c in cells)
    if all(c['flags'] & 8 for c in cells):
        return 'refused: %s' % QUAD_REFUSED[min(cells[0]['why'], len(QUAD_REFUSED) - 1)]
    if not sent:
        return 'nothing was sent'
    if all(c['whole'] == c['sent'] and c['flag'] == c['sent'] and not c['flags'] & 7 for c in cells):
        return 'ended every reply and every reply was whole (%d replies)' % sent
    for c in cells:
        bad = [(c['broke'], 'the session broke'), (c['last_wrong'], 'the last word was wrong'),
               (c['word_wrong'], 'a word was wrong'), (c['missing'], 'the flag did not come'),
               (c['no_record'], "the slave's record did not come")]
        bad = [(n, t) for n, t in bad if n]
        if bad or c['flags'] & 7:
            what = '; '.join('%s on %d of %d' % (t, n, c['sent']) for n, t in bad)
            flags = '; '.join(t for b, t in QUAD_FLAGS if c['flags'] & b and b != 8)
            return 'failed first at %s, %d bytes: %s' % (clock_text(c['clock']), c['len'],
                                                        '; '.join(x for x in (what, flags) if x))
    return 'failed'


def quad_args(a):
    """dev-quad-reply's arguments: N, the cells to run, --json."""
    opts = {'--cand': 'tf-dma,tf-core,spif', '--ren': '0,1', '--clock': '25,50,25f,50f',
            '--len': ','.join(str(n) for n in QUAD_LENS)}
    n, js, i = QUAD_N, False, 0
    while i < len(a):
        if a[i] == '--json':
            js = True
        elif a[i] in opts and i + 1 < len(a):
            opts[a[i]] = a[i + 1]
            i += 1
        elif not a[i].startswith('-'):
            n = num(a[i])
        else:
            raise SystemExit('dev-quad-reply: unknown option %s' % a[i])
        i += 1
    if not 1 <= n <= QUAD_N_MAX:
        raise SystemExit('dev-quad-reply: N is 1 to %d' % QUAD_N_MAX)
    try:
        cands = [QUAD_CANDS[x] for x in opts['--cand'].split(',')]
        rens = [int(x) for x in opts['--ren'].split(',')]
        clocks = [QUAD_CLOCKS[x] for x in opts['--clock'].split(',')]
        lens = [num(x) for x in opts['--len'].split(',')]
    except (KeyError, ValueError):
        raise SystemExit('dev-quad-reply: --cand tf-dma,tf-core,spif --ren 0,1 --clock 25,50,25f,50f '
                         '--len BYTES,...')
    if any(r not in (0, 1) for r in rens) or any(not 12 <= x <= 0x10000 or x % 4 for x in lens):
        raise SystemExit('dev-quad-reply: REN is 0 or 1, a length 12 to 65536 bytes in whole words')
    cells = []
    for cand in cands:
        for ren in rens:
            if ren and cand == 3:
                continue        # SPIF reads SPI_STAT alone: no receive channel to read
            for clock in clocks:
                for length in lens:
                    cells.append((cand, ren, clock, length))
    return n, cells, js


def quad_request(cand, ren, clock, length, n):
    return bytes([DEV_QUAD_REPLY, cand, ren, clock]) + struct.pack('<IH', length, n)


def quad_reply(mon, a):
    """OPXY_DEV_QUAD_REPLY, one request a cell, then the tables and the verdicts."""
    n, todo, js = quad_args(a)
    cells = []
    for cand, ren, clock, length in todo:
        d = quad_parse(mon.checked(quad_request(cand, ren, clock, length, n), QUAD_LIMIT_S + 60.0))
        cells.append(d)
        if d['flags'] & 1:
            break       # the slave is gone: nothing more can run
    if js:
        import json
        groups = {}
        for c in cells:
            groups.setdefault((c['cand'], c['ren']), []).append(c)
        print(json.dumps({'cells': cells, 'verdicts': [
            {'cand': k[0], 'ren': k[1], 'name': cand_text(*k), 'verdict': quad_verdict(v)}
            for k, v in groups.items()]}, indent=1))
        return
    print(quad_table(cells))


def quad_table(cells):
    """The text: a table per candidate and clock, a line per length, then a verdict per candidate."""
    out, groups, order = [], {}, []
    for c in cells:
        key = (c['cand'], c['ren'])
        if key not in groups:
            groups[key] = []
            order.append(key)
        groups[key].append(c)
    for key in order:
        clocks = []
        for c in groups[key]:
            if c['clock'] not in clocks:
                clocks.append(c['clock'])
        for clock in clocks:
            out.append('%s; the master at %s' % (cand_text(*key), clock_text(clock)))
            out.append('  %8s %6s %6s %6s %7s %9s %9s %6s %13s %6s  %s' % (
                'bytes', 'trials', 'whole', 'flag', 'missing', 'last word', 'a word', 'broke',
                'delay cycles', 'lines', 'master RF'))
            for c in groups[key]:
                if c['clock'] != clock:
                    continue
                came = c['whole'] + c['last_wrong'] + c['word_wrong']
                rf = 'every reply' if c['m_and'] & SPI_STAT_RF and came else \
                    'some' if c['m_or'] & SPI_STAT_RF else '-'
                note = '; '.join(t for b, t in QUAD_FLAGS if c['flags'] & b)
                out.append('  %8d %6d %6d %6d %7d %9d %9d %6d %13s %6s  %s%s' % (
                    c['len'], c['sent'], c['whole'], c['flag'], c['missing'], c['last_wrong'],
                    c['word_wrong'], c['broke'], quad_delay(c), quad_lines(c['lines']), rf,
                    ('  [' + note + ']') if note else ''))
                fail = quad_fail_text(c)
                if fail:
                    out.append('    ' + fail)
    out.append('verdicts:')
    for key in order:
        out.append('  %s: %s' % (cand_text(*key), quad_verdict(groups[key])))
    out.append('delay: the slave\'s cycles from its last feed to the flag (SPIF: from its last fall to its '
               'last rise); lines: the data lines of every wrong bit')
    return '\n'.join(out)


def print_crc(flags):
    """The timing replies' last two bytes: whether the CRC peripheral served CRC-16 and CRC-32.
    An older bootloader sends neither."""
    if len(flags) == 2:
        for name, on in zip(('crc-16', 'crc-32'), flags):
            print('CRC peripheral, %s: %s' % (name, 'in use' if on else 'not in use (software)'))


# ---- the screen and the keys (DEV_SCREEN, DEV_KEY, DEV_FRAME) ----

def key_code(name):
    """A key's S-07.030 number, from its name or number."""
    if name.lower() in KEYS:
        return KEYS[name.lower()]
    try:
        code = int(name, 0)
    except ValueError:
        raise SystemExit('unknown key %r: a digit 1-8, white, back, help, shift or a number' % name)
    if not 0 <= code < 72:
        raise SystemExit('key %r is not one of S-07.030\'s 72' % name)
    return code


def parse_keys(args):
    """[(key, action)] of KEY, KEY:down and KEY:up words."""
    out = []
    for w in args:
        name, _, half = w.partition(':')
        action = {'': KEY_PRESS, 'down': KEY_DOWN, 'up': KEY_UP}.get(half.lower())
        if action is None:
            raise SystemExit('%r: KEY, KEY:down or KEY:up' % w)
        out.append((key_code(name), action))
    return out


def key_requests(pairs):
    """DEV_KEY payloads of at most KEY_PAIRS pairs: each fits the empty queue."""
    return [bytes([DEV_KEY]) + b''.join(bytes(p) for p in pairs[i:i + KEY_PAIRS])
            for i in range(0, len(pairs), KEY_PAIRS)]


def wait_keys(mon, timeout=10.0):
    """Until the menus have taken every queued key event."""
    end = time.monotonic() + timeout
    while mon.checked(bytes([DEV_KEY]))[0]:
        if time.monotonic() > end:
            raise SystemExit('the menus did not take the keys: a long operation may be running')
        time.sleep(0.02)


def read_screen(checked, tries=3):
    """The screen as a dict, read again when it changed between its parts. checked(payload) returns a
    reply without its status byte."""
    for _ in range(tries):
        r = checked(bytes([DEV_SCREEN, 0]))
        parts, changes = r[0], struct.unpack_from('<I', r, 1)[0]
        name, rest = bytes(r[9:]).split(b'\0', 1)
        scr = {'screen': name.decode('latin-1'), 'title': rest.split(b'\0', 1)[0].decode('latin-1'),
               'changes': changes, 'flags': [n for bit, n in SCREEN_FLAGS if r[6] & bit],
               'mtp': MTP_STATES[r[7]] if r[7] < len(MTP_STATES) else str(r[7]),
               'usb': USB_SETS[r[8]] if r[8] < len(USB_SETS) else None, 'rows': []}
        for part in range(1, parts):
            p = checked(bytes([DEV_SCREEN, part]))
            if struct.unpack_from('<I', p, 1)[0] != changes:
                break
            style, text = bytes(p[7:]).split(b'\0', 1)
            scr['rows'].append({'style': style.decode('latin-1'), 'text': text.decode('latin-1')})
        else:
            return scr
    raise SystemExit('the screen kept changing while it was read')


def screen_text(scr):
    lines = ['%s: %s    usb %s, mtp %s%s' % (scr['screen'], scr['title'], scr['usb'] or 'none', scr['mtp'],
                                              ''.join(', ' + f for f in scr['flags']))]
    for row in scr['rows']:
        lines.append('  %-9s %s' % (row['style'], row['text'].replace('\t', '  |  ').replace('`', '')))
    return '\n'.join(lines)


def read_frame(checked):
    return b''.join(bytes(checked(bytes([DEV_FRAME]) + struct.pack('<IB', off, FRAME_CHUNK)))
                    for off in range(0, FRAME_BYTES, FRAME_CHUNK))


# ---- the yaffs2 region, raw (S-12 section 2; DEV_YTAGS and DEV_YMETA) ----

Y_BLOCKS, Y_CPB, Y_TAGS_MAX, Y_META_MAX = 0x1200, 128, 14, 232
Y_SEQ_CKPT, Y_SEQ_LO, Y_SEQ_HI = 0x21, 0x1000, 0xEFFFFEFF
Y_SUMMARY_ID, Y_SUMMARY_LEN, Y_HEADER = 0x10, 1540, 0x80000000


def ytags_req(first, stride, count):
    return bytes([DEV_YTAGS]) + struct.pack('<IIB', first, stride, count)


def ymeta_req(chunk, off, n):
    return bytes([DEV_YMETA]) + struct.pack('<IHB', chunk, off, n)


def yclass(seq):
    if seq == 0:
        return 'empty'
    if seq == Y_SEQ_CKPT:
        return 'checkpoint'
    return 'data' if Y_SEQ_LO <= seq <= Y_SEQ_HI else 'nothing'


def ytags_many(tags, first, stride, count):
    """[(seq, obj, cw, nbytes)] of count chunks first, first + stride, ..."""
    out = []
    while count:
        n = min(count, Y_TAGS_MAX)
        r = tags(first, stride, n)
        out += [struct.unpack_from('<IIII', r, 16 * i) for i in range(n)]
        first, count = first + stride * n, count - n
    return out


def ymeta_bytes(meta, chunk, off, n):
    out = b''
    while n:
        k = min(n, Y_META_MAX)
        out += meta(chunk, off, k)
        off, n = off + k, n - k
    return out


def ysummary(meta, chunk, seq):
    """S-12.049: the 127 entries of a valid summary, else None."""
    raw = ymeta_bytes(meta, chunk, 0, Y_SUMMARY_LEN)
    ver, _blk, sseq, total = struct.unpack_from('<IIII', raw, 0)
    if ver != 1 or sseq != seq or total != sum(raw[16:Y_SUMMARY_LEN]) & 0xFFFFFFFF:
        return None
    return [struct.unpack_from('<III', raw, 16 + 12 * i) for i in range(127)]


def yheader(meta, chunk):
    """The fields of an object header (S-12.045) that say where it sits."""
    raw = ymeta_bytes(meta, chunk, 0, 0x12C) + ymeta_bytes(meta, chunk, 0x1E8, 0x18)
    name = raw[0xA:0x10A].split(b'\0')[0]
    tail = raw[0x12C:]
    return {'type': struct.unpack_from('<I', raw, 0)[0], 'parent': struct.unpack_from('<I', raw, 4)[0],
            'name': name.decode('utf-8', 'replace'), 'mtime': struct.unpack_from('<I', raw, 0x11C)[0],
            'size': struct.unpack_from('<I', raw, 0x124)[0],
            'shadowed': struct.unpack_from('<I', tail, 0)[0], 'shrink_mark': struct.unpack_from('<I', tail, 4)[0],
            'size_hi': struct.unpack_from('<I', tail, 8)[0], 'shrink': struct.unpack_from('<I', tail, 20)[0]}


def yscan(tags, meta, objs=(), blocks=Y_BLOCKS, progress=None):
    """The region's tags, every non-empty block's own tags and summary, and the
    headers of the root's entries and of objs. Names only for the root's."""
    first = ytags_many(tags, 0, Y_CPB, blocks)
    last = ytags_many(tags, Y_CPB - 1, Y_CPB, blocks)
    out = {'blocks': [], 'empty': 0, 'headers': []}
    for b in range(blocks):
        if first[b][0] == 0 and last[b][0] == 0:
            out['empty'] += 1
            continue
        if progress:
            progress(b)
        own = ytags_many(tags, b * Y_CPB, 1, Y_CPB)
        seq = own[0][0]
        rec = {'b': b, 'seq': seq, 'class': yclass(seq), 'own': own, 'summary': 'none'}
        t = own[Y_CPB - 1]
        eff = list(own)
        if t[0] and t[1] == Y_SUMMARY_ID and t[2] == 1 and t[3] == Y_SUMMARY_LEN:
            entries = ysummary(meta, b * Y_CPB + Y_CPB - 1, seq)
            rec['summary'] = 'valid' if entries else 'invalid'
            if entries and rec['class'] == 'data':
                eff = [(seq,) + e if e[0] else own[i] for i, e in enumerate(entries)] + [own[-1]]
        rec['eff'] = eff
        out['blocks'].append(rec)
        if rec['class'] != 'data':
            continue
        for c, (cseq, obj, cw, nb) in enumerate(eff[:Y_CPB - 1]):
            if not cseq or not cw & Y_HEADER:
                continue
            h = {'b': b, 'c': c, 'seq': seq, 'own_seq': own[c][0], 'obj': obj & 0x0FFFFFFF,
                 'type': obj >> 28, 'parent': cw & 0x0FFFFFFF, 'shrink': cw >> 30 & 1,
                 'shadows': cw >> 29 & 1, 'nbytes': nb}
            if h['parent'] == 1 or h['obj'] in objs:
                try:
                    h['hdr'] = yheader(meta, b * Y_CPB + c)
                except SystemExit:
                    h['hdr'] = None         # the chunk's own tags are not a header's
            out['headers'].append(h)
    return out


def yreport(scan, objs=()):
    lines = []
    blocks = scan['blocks']
    classes = {}
    for r in blocks:
        classes[r['class']] = classes.get(r['class'], 0) + 1
    lines.append('blocks: %d empty, %s' % (scan['empty'], ', '.join(
        '%d %s' % (n, k) for k, n in sorted(classes.items()))))
    ck = [r for r in blocks if r['class'] == 'checkpoint']
    lines.append('checkpoint blocks: %s' % (' '.join(
        '%d(%d chunks)' % (r['b'], sum(1 for t in r['own'] if t[0])) for r in ck) or 'none'))
    other = [r for r in blocks if r['class'] == 'nothing']
    if other:
        lines.append('blocks holding nothing: %s' % ' '.join(
            '%d(seq %08x)' % (r['b'], r['seq']) for r in other[:20]))
    data = sorted((r for r in blocks if r['class'] == 'data'), key=lambda r: -r['seq'])
    if data:
        lines.append('data sequence numbers %08x..%08x; newest blocks:' % (data[-1]['seq'], data[0]['seq']))
        for r in data[:16]:
            used = sum(1 for t in r['own'] if t[0])
            lines.append('  block %4d seq %08x used %3d summary %s' % (r['b'], r['seq'], used, r['summary']))
    for r in data:
        seqs = {}
        for t in r['own'][:Y_CPB - 1]:
            if t[0] and t[0] != r['seq']:
                seqs[t[0]] = seqs.get(t[0], 0) + 1
        if seqs:
            lines.append('mixed block %d (seq %08x): %s' % (r['b'], r['seq'], ', '.join(
                '%d chunks of %08x' % (n, s) for s, n in sorted(seqs.items()))))
    seen = {}
    for h in scan['headers']:
        if h['parent'] == 1 and h.get('hdr'):
            seen.setdefault(h['hdr']['name'], []).append(h)
    lines.append('root entries (newest header first):')
    for name in sorted(seen):
        hs = sorted(seen[name], key=lambda h: (-h['seq'], -h['c']))
        lines.append('  %s' % name)
        for h in hs[:6]:
            lines.append('    obj %5d type %d seq %08x block %4d chunk %3d size %10d%s%s'
                         % (h['obj'], h['type'], h['seq'], h['b'], h['c'], h['hdr']['size'],
                            ' shrink' if h['shrink'] else '', ' shadows %d' % h['hdr']['shadowed']
                            if h['shadows'] else ''))
    for o in objs:
        lines.append('object %d:' % o)
        for h in scan['headers']:
            if h['obj'] == o:
                lines.append('  header seq %08x block %4d chunk %3d parent %d nbytes %d%s'
                             % (h['seq'], h['b'], h['c'], h['parent'], h['nbytes'],
                                ' shrink' if h['shrink'] else ''))
        pos = {}
        for r in data:
            for c, (cseq, obj, cw, nb) in enumerate(r['eff'][:Y_CPB - 1]):
                if cseq and obj == o and not cw & Y_HEADER:
                    pos.setdefault(cw, []).append((r['seq'], r['b'], c, nb, r['own'][c][0]))
        for k in sorted(pos):
            lines.append('  chunk %4d: %s' % (k, ' '.join(
                '%08x@%d.%d/%d%s' % (s, b, c, nb, '' if own == s else '(own %08x)' % own)
                for s, b, c, nb, own in sorted(pos[k], reverse=True))))
    return '\n'.join(lines)


def find_port():
    ports = sorted(glob.glob('/dev/cu.usbmodem*'))
    if not ports:
        raise SystemExit('no /dev/cu.usbmodem* port: is the unit on the main menu with USB connected?')
    return ports[0]


def line_log(fr):
    """A line report frame: [0, 0x50, count, (us:4, levels:1) * count], the levels
    bit 0 PA6 (slave ready) and bit 1 PC5 (bridge attention), each 1 = high."""
    out = []
    for i in range(fr[2]):
        us = struct.unpack_from('<I', fr, 3 + 5 * i)[0]
        lv = fr[3 + 5 * i + 4]
        out.append('%dms PA6 %s PC5 %s' % (us // 1000, 'high' if lv & 1 else 'low',
                                           'high' if lv & 2 else 'low'))
    return ', '.join(out)


MARK_NAMES = {1: 'teardown', 2: 'MSI done', 3: 'SEC done', 4: 'entry set', 5: 'delay over',
              6: 'first load call', 7: 'kernel returned', 8: 'before the jump'}


def mark_steps(fr):
    """The slave's OPXY_HO_SLAVE_MARKS pulses in a line report: each PA6 high run of k x 2 ms is step k.
    Returns the steps in order, and the start in ms of a final high that never fell (or None)."""
    steps, rise, tail = [], None, None
    for i in range(fr[2]):
        us = struct.unpack_from('<I', fr, 3 + 5 * i)[0]
        high = fr[3 + 5 * i + 4] & 1
        if high and rise is None and i > 0:
            rise = us
        elif not high and rise is not None:
            steps.append(int(round((us - rise) / 2000.0)))
            rise = None
    if rise is not None:
        tail = rise // 1000
    return steps, tail


def main(argv):
    args = argv[1:]
    port = None
    if args[:1] == ['--port']:
        port, args = args[1], args[2:]
    if args[:1] == ['--send-addr']:
        global SEND_ADDR
        SEND_ADDR, args = num(args[1]), args[2:]
    if not args:
        print(__doc__)
        return 2
    if args[0] in ('encode', 'crumbs') or args == ['continue-os', '--sysex']:
        return run(None, args)
    return run(Monitor(port or find_port()), args)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
