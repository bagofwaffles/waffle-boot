#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Recovery's host tool: talks to a unit started with Power + M4 (wiki/Recovery-Reference.md).

    wbrecover.py [--port /dev/cu.usbmodemNNN] info
    wbrecover.py [--port PORT] write [--running | --new-front] [--force] IMAGE.ldr
    wbrecover.py [--port PORT] select [--force] IMAGE.ldr
    wbrecover.py [--port PORT] try | switch | keep [--force]
    wbrecover.py [--port PORT] put-back
    wbrecover.py [--port PORT] restart [--force]
    wbrecover.py check IMAGE.ldr

info     what the unit reports: why recovery runs, both slots, which one runs and which one the
         selector names, where each kind of write would go or why it is refused, the battery,
         the write's state; from front version 11 also what the other slot holds, which slot
         actions the unit takes now (and with --force), and a countdown under way
write    checks IMAGE first (size, S-02.091's stream rules, a waffle-boot front in it, the same
         front as the unit runs; for a front before version 14 also that it is not the image
         of the slot that is kept), opens the transfer,
         states the risk of the write the unit announces and asks for yes (a typed phrase for
         an in-place write), sends it, follows the unit's write, then compares the unit's
         read-back and its selector with the file. It writes the slot the selector does not
         name; --running (front version 6 or later) writes the running slot instead. A
         version 5 front asks only when a good image is replaced. It warns when a slot holds
         another image with the file's version. --new-front (front version 7 or later) writes
         an image with another front to the other slot for one trial start, after a typed
         phrase
select   keeps a slot written with --new-front: from that slot's own recovery in its trial
         start, or from the old front's recovery once the trial start has run
try      (front version 11 or later) has the next start run the other slot's image once, as it
         lies; no image is sent. The starts after it boot the running slot again, until a keep
switch   (front 11) makes the other slot the boot slot, after a warning by what it
         holds and a typed phrase; no image is sent. After a trial start of it that came up,
         keep is the action
keep     (front 11) keeps a tried slot with no image sent: the other slot once its trial start has
         run, or the running slot in its own trial start
put-back (front 11) makes the tried other slot's record good again, as before its trial
restart  has the unit restart (the PSoC's reset), unless the unit says a restart is not safe yet
check    only the file checks of write; no unit needed

--force (front 11) has the unit lift the refusals that protect it; the tool warns first, says what
could go wrong and asks for a typed phrase. Without it the unit refuses as before. After a write of
Slot 1 in the same recovery the tool refuses --force and asks for a restart first. From front 11
the unit counts down 15 s after a write or an action is confirmed, before its first erase:
switching it off or unplugging USB in that time stops it with nothing written. These commands also
let a unit whose keys do not work be recovered from the host alone; on the unit the same actions are
in the recovery menu (key 1, "Change the boot slot").

The port is found by its USB product string, "OP-XY Recovery" (vendor 0x2367, product 0x0021) or,
on an OP-1F, "OP-1 Recovery" (product 0x0102), through ioreg on macOS and sysfs on Linux; elsewhere
give --port. Exit status 0 only when the
command did what it says (write: the slot holds the file's bytes, read back, and the selector
names it). Python 3, standard library only; needs this repository's tests/oracle/ and tools/ for
the image checks, and reads src/front/front.pin and its git history to tell a signed front pin.
"""
import argparse
import glob
import hashlib
import os
import re
import select
import struct
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'tests', 'oracle', 'S-01-boot-chain'))
import mkldr  # noqa: E402
import check_stream  # noqa: E402

VID, PID = 0x2367, 0x0021      # src/front/fr_usb.h
PRODUCT = 'OP-XY Recovery'
PID_1F, PRODUCT_1F = 0x0102, 'OP-1 Recovery'    # S-02.093 on the OP-1F
# Recovery's port on each device: (product id, product string).
PORTS = ((PID, PRODUCT), (PID_1F, PRODUCT_1F))

# src/front/fr_proto.h
PROTO = 2                      # from front version 11
OLD_PROTO = 1                  # fronts 5 to 10
HELLO, INFO, BEGIN, DATA, END, RESTART, ACT, REPLY = 1, 2, 3, 4, 5, 6, 7, 0x80
MAX_DATA = 1024
MAX_PAYLOAD = MAX_DATA + 8     # FR_PROTO_MAX
HEAD = 6
FRAME_MAX = HEAD + MAX_PAYLOAD + 4
NO_SLOT = 0xFF
SLOT_CAP = 0x78000             # S-02.002, S-02.044: what the unit takes
IMAGE_MAX = 0x77000            # S-02.095: recovery's own limit, the slot's last sector holds the start log
WB_IMAGE_MAX = 0x76000         # S-02.104: this tool's, the sector below it holds the slot's settings
BEGIN_REPLY_LEN, DATA_REPLY_LEN, END_REPLY_LEN, ACT_REPLY_LEN = 3, 5, 2, 4
HELLO_LEN, HELLO_O_VERSION, HELLO_O_PIN = 47, 7, 15
INFO_V5, INFO_V6, INFO_V11 = 199, 219, 232      # INFO's length from a version 5, 6 and 11 front
INFO_V20 = 242                                  # and from a version 20 front: the device (S-18)
# version 20: the device (0 OP-XY, 1 OP-1F), how it was decided (0 the first stage's table, 2 not
# identified: the default, the OP-XY), and the first stage's n and c (S-18.003, S-18.004)
INFO_O_DEVICE, INFO_O_DEV_HOW, INFO_O_FS_N, INFO_O_FS_C = 232, 233, 234, 238
DEVICES = ('OP-XY', 'OP-1F')
HOW_TABLE, HOW_UNKNOWN = 0, 2
INFO_O_VERSION, INFO_O_PIN, INFO_O_READBACK = 21, 29, 61
INFO_O_SLOT, INFO_SLOT_LEN = 93, 53                     # slot k at INFO_O_SLOT + INFO_SLOT_LEN * k
INFO_S_VERSION, INFO_S_FRONT_CRC, INFO_S_PIN = 9, 17, 21
INFO_O_RUN_WAY, INFO_O_RUN_REFUSAL, INFO_O_WAY, INFO_O_BATT_LEN, INFO_O_BATT = 199, 200, 201, 202, 203
BATT_MAX = 16
# version 11: the countdown's seconds left, the action under way or ended, the other slot's tier,
# the status each action would get now unforced and forced, the slot a keep would select, and
# the status of the last request refused, or countdown stopped, since the write made last
INFO_O_COUNT, INFO_O_ACT, INFO_O_TIER, INFO_O_ACTS, INFO_O_FORCED, INFO_O_KEEP, INFO_O_REFUSED = \
    219, 220, 221, 222, 226, 230, 231
INFO_HEAD = '<BBBBBBBBIIHBBB'  # why, flags, running, the selector's two bytes, target, refusal,
                               # phase, len, got, progress, result, image_rc, written
F_ESTABLISHED, F_SELECTOR, F_OWN_DESC, F_MOVED, F_SPOILED, F_HELD, F_READ_ERROR, F_UNSETTLED = \
    1, 2, 4, 8, 16, 32, 64, 128
S_GOOD, S_HAS_DESC, S_HAS_FRONT = 1, 2, 4
S_TRIED = 8                    # the record carries the trial mark
S_STARTED = 16                 # tried, and its front marked its trial start in its log
S_TE_PREFIX = 32               # good or tried, and S-02.106 rule 3 holds (front 17 and later)
# S-02.106: the TE bootloader builds by (length, CRC-32 of the slot's bytes).
TE_BUILDS = {
    (0x5ADB0, 0x7AF4665C): (1000, 0, 12, 0),
    (0x5B7AC, 0x0789B2D9): (1000, 0, 13, 0),
    (0x60000, 0x5DEE4B51): (1000, 0, 20, 0),
    (0x60CA4, 0xA930BEF3): (1000, 0, 23, 0),
}
BEGIN_F_RUNNING, BEGIN_F_TRIAL, BEGIN_F_SELECT = 1, 2, 4
BEGIN_F_FORCE = 0x80           # in BEGIN's, ACT's and RESTART's flags
WAY_NONE, WAY_PLAIN, WAY_SELECT_FIRST, WAY_IN_PLACE, WAY_TRIAL, WAY_SELECT, \
    WAY_TRY, WAY_SWITCH, WAY_KEEP, WAY_PUT_BACK = range(10)
ACT_TRY, ACT_SWITCH, ACT_KEEP, ACT_PUT_BACK = 1, 2, 3, 4
TIER_NONE, TIER_SAME, TIER_FRONT, TIER_FOREIGN, TIER_BAD = range(5)
TRIAL_FRONT = 7                # the first front that takes another front, by trial
KEEPS_WRITE = 8                # the first whose refusals leave INFO showing an earlier write
ACTS_FRONT = 11                 # the first with the slot actions, the force flag and the countdown
SAME_FRONT = 14                 # the first that writes an image the kept slot already holds
COUNTDOWN = 15                 # FR_COUNTDOWN_US: seconds from an accepted END or ACT to its first erase
ACT_NAMES = {ACT_TRY: 'try', ACT_SWITCH: 'switch', ACT_KEEP: 'keep', ACT_PUT_BACK: 'put-back'}
ACT_WAYS = {ACT_TRY: WAY_TRY, ACT_SWITCH: WAY_SWITCH, ACT_KEEP: WAY_KEEP, ACT_PUT_BACK: WAY_PUT_BACK}
WHY = {1: 'Power + M4 was held', 2: "the image check failed: the running slot's image is not intact",
       3: 'three starts in a row did not finish (S-02.095); the power switch leaves recovery, and '
          'the next start is a normal one'}
PHRASE = 'erase the running slot in place'
TRIAL_PHRASE = 'try the new front'
SWITCH_PHRASE = 'make it the boot slot'
FORCE_PHRASE = 'force the %s'  # the command's name: force the write, force the switch, ...
# While a write of this recovery has left the running slot bad, the unit reads the selector and
# both slots again before it answers INFO, BEGIN and RESTART, plans from the flash at END, and
# after a read that failed reads them by itself every 5 s (S-02.084): two CRC passes and two
# stream scans of up to 0x77000 bytes each, some seconds. A request resent while it reads queues
# another such read behind the first, so these requests wait one long time for the reply to the
# request sent, and only then send it again. HELLO waits as long: it can arrive during a read.
LONG_TIMEOUT = 30.0
LONG_RETRIES = 3
PHASES = ['idle', 'receiving', 'writing', 'done', 'failed', 'counting down']
IDLE, RECEIVING, WRITING, DONE, FAILED, COUNTING = range(6)

RESET_HINT = 'Restart once without holding a key, then hold Power + M4 again.'
SAFE_HINT = ('Run info first: restart only with a checked boot slot; the unit refuses a restart '
             'while a write of this recovery has left no checked boot slot.')
FRONT_HINT = ('Another front goes in only as a trial: write --new-front IMAGE, on a front of '
              'version 7 or later; from an older front, the test OS installer (wiki/Test-OS.md).')
OTHER_HINT = ('Start the other slot if it is checked and holds a stream the boot ROM can start '
              '(S-02.091): wbrecover.py try starts it once, wbrecover.py switch makes it the boot slot; or '
              'key 1, "Change the boot slot", in the recovery menu.')
SLOT_MENU = 'or key 1, "Change the boot slot", in the recovery menu'
NOT_STARTED_HINT = ('Write it again with --new-front and restart with no key held, so that it starts '
                    'once; then switch off and on and select. Or select from its own recovery in the '
                    'trial start (try holding key 4 through the restart). A front older than version '
                    '7 never leaves that mark: put it on with the test OS installer (wiki/Test-OS.md).')
OLD_FRONT_RISK = ('A front without the trial mark (any front older than version 7) can never be '
                  'selected: %s\'s image is then lost for nothing.')
# enum fr_write_rc, then the protocol's own codes: what happened, and what to do.
STATUS = {
    0: ('ok', ''),
    1: ('a write or an action is already under way, or counting down to its first erase',
        'Wait for it to end: run info.'),
    2: ('the unit could not establish which slot it is running from (S-01.002)', RESET_HINT),
    3: ('the running slot is not the slot the selector names', RESET_HINT),
    4: ("the selector is not settled: its verified byte is not 01 (S-02.090)", RESET_HINT),
    5: ('the boot slot failed its check, and is not the running slot with its front intact (S-02.084)',
        OTHER_HINT),
    6: ('the image is empty or too large: recovery takes at most 0x76000 bytes, 0x77000 before front 17 (S-02.084)',
        'Check the file: wbrecover.py check IMAGE.'),
    7: ('the image is not a bootable stream with a waffle-boot front (S-02.091)',
        'Check the file (wbrecover.py check IMAGE) and build it with this tree\'s tools.'),
    8: ('the image arrived damaged: its SHA-256 is not the one announced', 'Try the write again.'),
    9: ('a flash erase, program or read of the write failed',
        'The selector was not changed: the unit still starts as before. Try the write again.'),
    10: ('the slot did not read back as written, or not as the SHA-256 announced',
         'The selector was not changed: the unit still starts as before. Try the write again.'),
    11: ('the slot was written and read back, but the selector write failed or did not read back',
         SAFE_HINT),
    12: ("the image's front is not the one this unit runs and no trial was asked for, or the "
         'running slot is checked and holds no waffle-boot front (S-02.084)', FRONT_HINT),
    13: ('the slot that is kept already holds this image, so the unit could not tell which slot '
         'runs; or the target is the checked running slot, which already holds it (a front before '
         'version 14 refuses this; from version 14 both slots may hold the same image)',
         'Nothing to do; or write another build.'),
    14: ('the selector write before the write of the running slot failed; no slot or record was '
         'written', 'Run info to see what the selector names now.'),
    15: ('an in-place write of the running slot becomes possible only after a write of the other '
         'slot has failed in this recovery (S-02.084)',
         'Try a plain write first (wbrecover.py write IMAGE): it is safe and loses nothing.'),
    16: ("the unit's state changed since the write was confirmed: it would not go as BEGIN's "
         'answer said, and nothing was written', 'Run info, then write again.'),
    17: ('the selector or a slot could not be read for the start condition; nothing was written',
         'Run info and try again.'),
    18: ('the target is the only checked slot, so the unit accepts only the image it holds, to settle '
         'the selector on it (S-02.084)', 'Take one of the ways out that wbrecover.py info lists.'),
    19: ('a trial needs the running slot checked, and neither a trial nor a select is taken once '
         'this recovery has begun a write of the running slot, written the selector to the other '
         'slot, or begun a trial write (S-02.084)',
         'Run info to see the state; after a trial write, restart first.'),
    20: ('no slot holds this image as tried where a select can take it: from the old front the '
         'other slot, in a trial start the running slot (S-02.084)',
         'Send the file that was written with --new-front, after its trial start has run.'),
    21: ('the tried slot has not started: its front has left no mark of a trial start (S-02.084)',
         NOT_STARTED_HINT),
    22: ('that slot is already the boot slot (verified 01), so nothing was written',
         'Nothing to do: restart the unit (wbrecover.py restart) to start it.'),
    23: ('the other slot failed its check and is not tried: nothing checks it at a start (S-02.084)',
         'Write an image to it first (wbrecover.py write IMAGE).'),
    24: ('the write was stopped before its first erase: the unit was switched off or USB was '
         'unplugged, and nothing was written', 'Run info, then start the command again if it is '
         'still wanted.'),
    0x40: ('the unit did not understand the request',
           'Run info: the unit\'s front may be older or newer than this tool knows.'),
    0x41: ('not now: no transfer is open, or a write is under way', 'Run info, then start the command '
           'again.'),
    0x42: ('a chunk came out of order', 'Start the command again.'),
    0x43: ('the transfer is short, or a chunk runs past its end', 'Start the command again.'),
    0x44: ('the unit refuses a restart: a write of this recovery left the running slot damaged, or '
           'did not settle the selector on it, and a restart is not safe yet',
           'Write a slot first: each way out ends by settling the selector.'),
}
IMAGE_RC = {1: 'empty or larger than a slot (S-02.044)',
            2: 'its blocks do not parse up to a FINAL block (S-02.091 rule 1)',
            3: 'its INIT block is missing, repeated, or not in L1 (rule 2)',
            4: 'a block loads outside L1, L2 and DDR or into the boot ROM\'s reserve (rule 3)',
            5: 'a block loads into the first stage\'s stack, 0x11807000-0x11808000 (rule 4)',
            6: 'a block code\'s BCODE is not 4 (rule 5)',
            7: 'it carries no waffle-boot front'}


class Failure(Exception):
    pass


# Hints that send the user to a restart or to the other slot, or say the unit starts as before: never
# while a write of this recovery has left the running slot bad (retry_lines() says what to do).
RESTART_TIPS = (2, 3, 4, 5, 9, 10, 11, 22)
# statuses 3 and 4 once this recovery has made a write: that write's selector, which only a start
# settles
AFTER_WRITE = ('restart before another write: this recovery has made a write, and the unit takes '
               'the next one only after a start', 'Restart the unit (wbrecover.py restart) with no '
               'key held, then hold Power + M4 again.')


def v11(i):
    """INFO came from a front of version 11 or later: the slot actions."""
    return i is not None and i.get('front', 0) >= ACTS_FRONT


def other_hint(i):
    return OTHER_HINT


def after_write(i):
    """This recovery has made a write or an action, as INFO shows it: one done, one that ran and
    failed, or the selector written before a write of the running slot."""
    return i is not None and (i.get('phase') == DONE or bool(i.get('moved')) or
                              (i.get('phase') == FAILED and i.get('result') in (9, 10, 11, 14)))


def slot1_window(i):
    """S-02.090: this recovery has written Slot 1 (the selector reads index 00, not settled), so a forced
    second write could, with a cut and a torn first-stage selector write, leave 01 01 trusting a part
    written Slot 2. The tool refuses --force there: a restart settles the selector first."""
    return after_write(i) and i.get('sel_index') == 0x00 and i.get('sel_verified') != 0x01


FORCE_AFTER_SLOT1 = ('Refused before sending, even with --force: this recovery has written Slot 1 and '
                     'the selector is not settled, so a second write now can leave the unit starting a '
                     'slot that is not whole if the power is cut. Restart the unit first (wbrecover.py '
                     'restart) with no key held, then hold Power + M4 again. Nothing was sent.')


def status_text(st, image_rc=0, hint=True, i=None):
    what, tip = STATUS.get(st, ('status 0x%02x' % st, ''))
    if st == 5:
        tip = other_hint(i)
    if st in (3, 4) and after_write(i) and not (i.get('spoiled') or i.get('unsettled')):
        what, tip = AFTER_WRITE
    if i is not None and (i['spoiled'] or i.get('unsettled')) and st in RESTART_TIPS:
        tip = ''
    if st == 7 and image_rc in IMAGE_RC:
        what += ': ' + IMAGE_RC[image_rc]
    return what + ('. ' + tip if tip and hint else '')


def refusal_text(st, i=None):
    """A refusal of the start condition, before any image: INFO's refusals."""
    if st == 12:        # the running slot good, its front not intact in the flash
        return "the running slot's waffle-boot front is not intact in the flash (S-02.084)" + \
            ('' if i is not None and (i['spoiled'] or i.get('unsettled')) else '. ' + other_hint(i))
    return status_text(st, i=i)


def slot_name(k):
    """S-15's shown slot number: slot 0 is Slot 1."""
    return 'Slot %d' % (k + 1) if k in (0, 1) else 'no slot'


def version(v):
    """As the unit's screens show it: three places, and the build number
    after a '+' only when it is not 0."""
    return '%d.%d.%d' % tuple(v[:3]) + ('+%d' % v[3] if v[3] else '')


# --- frames ---

def frame(typ, seq, payload=b''):
    body = struct.pack('<BBH', typ, seq & 0xFF, len(payload)) + payload
    return b'WR' + body + struct.pack('<I', zlib.crc32(body) & 0xFFFFFFFF)


def take_frame(buf):
    """(type, seq, payload) of the first whole frame in buf, which it removes,
    or None. Bytes that do not start a good frame are dropped up to the next "WR"."""
    while True:
        i = buf.find(b'WR')
        if i < 0:
            del buf[:max(0, len(buf) - 1)]
            return None
        del buf[:i]
        if len(buf) < HEAD:
            return None
        n = struct.unpack_from('<H', buf, 4)[0]
        if n > MAX_PAYLOAD:
            del buf[:1]
            continue
        if len(buf) < HEAD + n + 4:
            return None
        if zlib.crc32(bytes(buf[2:HEAD + n])) & 0xFFFFFFFF != struct.unpack_from('<I', buf, HEAD + n)[0]:
            del buf[:1]
            continue
        got = (buf[2], buf[3], bytes(buf[HEAD:HEAD + n]))
        del buf[:HEAD + n + 4]
        return got


# --- the port ---

def find_port():
    if sys.platform == 'darwin':
        for pid, product in PORTS:
            try:
                out = subprocess.run(['ioreg', '-r', '-l', '-n', product], capture_output=True,
                                     text=True, timeout=10).stdout
            except (OSError, subprocess.SubprocessError):
                out = ''
            if re.search(r'"idVendor" = %d\b' % VID, out) and re.search(r'"idProduct" = %d\b' % pid, out):
                ports = re.findall(r'"IOCalloutDevice" = "([^"]+)"', out)
                if ports:
                    return ports[0]
    elif sys.platform.startswith('linux'):
        for tty in sorted(glob.glob('/sys/class/tty/ttyACM*')):
            usb = os.path.dirname(os.path.realpath(os.path.join(tty, 'device')))

            def field(name):
                try:
                    with open(os.path.join(usb, name)) as f:
                        return f.read().strip()
                except OSError:
                    return ''
            if field('idVendor') == '%04x' % VID and \
                    any(field('idProduct') == '%04x' % pid and field('product') == product
                        for pid, product in PORTS):
                return '/dev/' + os.path.basename(tty)
    raise Failure('no serial port named "%s" or "%s" found: is the unit in recovery (Power + M4) and '
                  'on USB? Or give --port' % (PRODUCT, PRODUCT_1F))


class Link:
    """The recovery port: one request at a time, each with a timeout and retries."""

    def __init__(self, port, timeout=5.0, retries=4, long_timeout=LONG_TIMEOUT,
                 long_retries=LONG_RETRIES):
        import termios          # here, so that check runs where there is no termios
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = attrs[1] = attrs[3] = 0
        attrs[2] = termios.CS8 | termios.CLOCAL | termios.CREAD
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        termios.tcflush(self.fd, termios.TCIOFLUSH)
        self.seq = 0
        self.rx = bytearray()
        self.timeout, self.retries = timeout, retries
        self.long_timeout, self.long_retries = long_timeout, long_retries
        self.max_data = MAX_DATA
        self.front = 0                  # the front's version, from HELLO
        self.synced = False
        self.sent = 0                   # the attempts of the last request that went out whole

    def close(self):
        os.close(self.fd)

    def send(self, data):
        """All of data to the port: a write may take fewer bytes than it is given."""
        view = memoryview(data)
        try:
            while view:
                view = view[os.write(self.fd, view):]
        except OSError:
            raise Failure('the port went away: was the unit restarted or unplugged?')

    def sync(self):
        """Puts the front's parser back in step before the first request. The parser keeps a
        partial frame until its stated length has come (fr_proto.c, next_frame()), so a frame an
        earlier host left cut off would take the start of this host's first request as its tail.
        From version 8 the front drops it when USB's configured state changes, which closing the
        port need not do.
        FRAME_MAX zero bytes complete any such frame and every frame that could start inside its
        bytes; their CRCs fail and the parser drops them byte by byte. Zeros hold no "WR", so it
        then drops the zeros too, as it drops them into an empty buffer."""
        if not self.synced:
            self.send(bytes(FRAME_MAX))
            self.synced = True

    def _reply(self, typ, seq, end):
        while True:
            got = take_frame(self.rx)
            if got is not None:
                if got[0] == typ | REPLY and got[1] == seq:
                    return got[2]
                continue                # a late reply to an earlier attempt
            left = end - time.monotonic()
            if left <= 0:
                return None
            ready, _, _ = select.select([self.fd], [], [], min(left, 0.05))
            if ready:
                try:
                    data = os.read(self.fd, 4096)
                except OSError:
                    data = b''
                if not data:            # ready with nothing to read: the device is gone
                    raise Failure('the port went away: was the unit restarted or unplugged?')
                self.rx += data

    def request(self, typ, payload=b'', timeout=None, retries=None):
        """The reply's payload and the number of the attempt that got it (1 = the first).
        Every attempt has a sequence number of its own, so a late reply to an earlier attempt
        is not taken. The requests the unit may answer only after reading the flash wait the
        long timeout before they are sent again."""
        slow = typ in (HELLO, INFO, BEGIN, END, RESTART, ACT)
        if timeout is None:
            timeout = self.long_timeout if slow else self.timeout
        if retries is None:
            retries = self.long_retries if slow else self.retries
        self.sync()
        self.sent = 0
        for attempt in range(1, retries + 1):
            self.seq = (self.seq + 1) & 0xFF
            self.send(frame(typ, self.seq, payload))
            self.sent = attempt
            got = self._reply(typ, self.seq, time.monotonic() + timeout)
            if got is not None:
                return got, attempt
        raise Failure('no answer from the unit after %d tries: is it still in recovery? '
                      'Unplug and replug USB. Unless a write of the running slot failed in this recovery, '
                      'you may also restart it with Power + M4' % retries)


# --- the replies ---

def sized(r, n, what):
    """r, a reply's payload, once it holds the n bytes its type carries."""
    if len(r) < n:
        raise Failure("%s's reply is %d bytes, short of the %d it carries: the unit did not "
                      'answer as its protocol says; run info' % (what, len(r), n))
    return r


def parse_hello(p):
    if len(p) < HELLO_LEN:
        raise Failure('HELLO reply too short (%d bytes)' % len(p))
    proto, front, max_data, why, flags = struct.unpack_from('<BHHBB', p, 0)
    return {'proto': proto, 'front_version': front, 'max_data': max_data, 'why': why,
            'own_desc': bool(flags & F_OWN_DESC),
            'version': struct.unpack_from('<4H', p, HELLO_O_VERSION),
            'pin': p[HELLO_O_PIN:HELLO_O_PIN + 32]}


def parse_info(p):
    """INFO's fields; v6 is true when the version 6 fields came (INFO_V6 bytes)."""
    if len(p) < INFO_V5:
        raise Failure('INFO reply too short (%d bytes)' % len(p))
    i = {'v6': len(p) >= INFO_V6}
    (i['why'], flags, i['running'], i['sel_index'], i['sel_verified'], i['target'], i['refusal'],
     i['phase'], i['len'], i['got'], i['progress'], i['result'], i['image_rc'],
     i['written']) = struct.unpack_from(INFO_HEAD, p, 0)
    i['established'] = bool(flags & F_ESTABLISHED)
    i['selector_ok'] = bool(flags & F_SELECTOR)
    i['own_desc'] = bool(flags & F_OWN_DESC)
    i['moved'] = i['v6'] and bool(flags & F_MOVED)
    i['spoiled'] = i['v6'] and bool(flags & F_SPOILED)
    i['held'] = i['v6'] and bool(flags & F_HELD)
    i['read_error'] = i['v6'] and bool(flags & F_READ_ERROR)
    i['unsettled'] = i['v6'] and bool(flags & F_UNSETTLED)
    i['run_way'], i['run_refusal'], i['way'] = \
        (p[INFO_O_RUN_WAY], p[INFO_O_RUN_REFUSAL], p[INFO_O_WAY]) if i['v6'] else (0, 0, 0)
    i['batt'] = bytes(p[INFO_O_BATT:INFO_O_BATT + min(p[INFO_O_BATT_LEN], BATT_MAX)]) \
        if i['v6'] else b''
    i['v11'] = len(p) >= INFO_V11
    if i['v11']:
        i['count'], i['act'], i['tier'] = p[INFO_O_COUNT], p[INFO_O_ACT], p[INFO_O_TIER]
        i['acts'] = list(p[INFO_O_ACTS:INFO_O_ACTS + 4])        # by enum fr_act, from 1
        i['forced'] = list(p[INFO_O_FORCED:INFO_O_FORCED + 4])
        i['keep'] = p[INFO_O_KEEP]
        i['refused'] = p[INFO_O_REFUSED]
    else:
        i['refused'] = 0
        i['count'] = i['act'] = i['tier'] = 0
        i['acts'] = i['forced'] = None
        i['keep'] = NO_SLOT
    i['v20'] = len(p) >= INFO_V20
    if i['v20']:
        i['device'], i['dev_how'] = p[INFO_O_DEVICE], p[INFO_O_DEV_HOW]
        i['fs_n'], i['fs_c'] = struct.unpack_from('<II', p, INFO_O_FS_N)
    else:
        i['device'] = i['dev_how'] = i['fs_n'] = i['fs_c'] = None
    i['version'] = struct.unpack_from('<4H', p, INFO_O_VERSION)
    i['pin'] = p[INFO_O_PIN:INFO_O_PIN + 32]
    i['readback'] = p[INFO_O_READBACK:INFO_O_READBACK + 32]
    i['slots'] = []
    for k in range(2):
        o = INFO_O_SLOT + INFO_SLOT_LEN * k
        sf, length, crc = struct.unpack_from('<BII', p, o)
        i['slots'].append({'good': bool(sf & S_GOOD), 'has_desc': bool(sf & S_HAS_DESC),
                           'has_front': bool(sf & S_HAS_FRONT), 'tried': bool(sf & S_TRIED),
                           'started': bool(sf & S_STARTED),
                           'te_prefix': bool(sf & S_TE_PREFIX),
                           'length': length, 'crc': crc,
                           'version': struct.unpack_from('<4H', p, o + INFO_S_VERSION),
                           'front_crc': struct.unpack_from('<I', p, o + INFO_S_FRONT_CRC)[0],
                           'pin': p[o + INFO_S_PIN:o + INFO_S_PIN + 32]})
    return i


def selected(index):
    """S-02.011: 0x00 selects slot 0, anything else slot 1."""
    return 0 if index == 0 else 1


def waffle(s):
    return s['has_desc'] and s['has_front']


def slot_kind(s):
    """S-02.106: ('WB', version), ('TE', version), ('TE', None) or ('??', None) for a good or tried
    slot, from INFO's slot fields and flag bit 5; None for any other slot."""
    if not (s['good'] or s['tried']):
        return None
    if s['has_desc']:
        return ('WB', tuple(s['version']))
    v = TE_BUILDS.get((s['length'], s['crc'] ^ 0xFFFFFFFF if s['tried'] else s['crc']))
    if v is not None:
        return ('TE', v)
    return ('TE', None) if s['te_prefix'] else ('??', None)


def kind_text(k):
    """A slot kind as the slot lines show it: 'WB v0.9.36', 'TE v1000.0.20' (with '+build' when the
    build is not 0), 'TE' or '??'."""
    return k[0] if k[1] is None else '%s v%s' % (k[0], version(k[1]))


def slot_state(s):
    """S-15.151's state word: checked, tried, tried, started, or check failed."""
    if s['tried']:
        return 'tried, started' if s['started'] else 'tried'
    return 'checked' if s['good'] else 'check failed'


def image_words(s):
    """A slot's image, for a warning."""
    return 'version %s' % version(s['version']) if waffle(s) else 'not a waffle-boot image'


def battery(b):
    """(state, words) of the PSoC's battery report (S-07.053). state: 'charged' (present and
    usable by S-07.053: above 5 % and 3500 mV), 'low', 'absent' or 'unknown'."""
    if not b:
        return 'unknown', 'unknown: no report came from the unit'
    if b[0] == 0:
        return 'absent', 'absent: the cable alone powers the unit'
    words = 'charging' if b[0] == 2 else 'not charging'
    if len(b) < 7:
        return 'unknown', 'present, %s%s, its voltage not reported' % (
            words, ', %d %%' % b[1] if len(b) > 1 else '')
    mv = struct.unpack_from('>H', b, 5)[0]
    words += ', %d %%, %d mV' % (b[1], mv)
    if b[1] > 5 and mv > 3500:
        return 'charged', 'present and charged (%s)' % words
    return 'low', 'present but low (%s)' % words


def damaged(i):
    """S-02.084's damaged state: this recovery began a write of the running slot, and that slot
    is bad (flag bit 4), or read back good with the selector not settled on it while the other
    slot is bad (bit 7)."""
    return (i['spoiled'] or i['unsettled']) and i['phase'] != WRITING


def blocked(i):
    """Flag bit 5: in the damaged state, a restart is not safe yet (S-02.084)."""
    return damaged(i) and i['held']


def plain_hint(i):
    """A failure's hint is a plain write's, from a unit that starts as before."""
    return i['way'] not in (WAY_SELECT_FIRST, WAY_IN_PLACE) and not damaged(i)


READ_ERROR = ('A read of the selector or a slot failed, so what the unit reports of them may not be '
              'what the flash holds. Ask again (wbrecover.py info): the unit reads them again, and '
              'by itself every 5 s while it holds.')


def uncut_reach(i):
    """The good slot a first stage reaches when its own selector write is not cut short, from
    INFO's selector and slots, or None (S-02.021-S-02.023)."""
    if not i['selector_ok'] or i['read_error']:
        return None
    n = selected(i['sel_index'])
    if i['slots'][n]['good']:
        return n
    if i['sel_verified'] != 1 and i['slots'][1 - n]['good']:
        return 1 - n
    return None


def state_line(i):
    """The first sentence of the damaged state."""
    a, b = slot_name(i['running']), slot_name(1 - i['running'])
    if i['spoiled']:
        return 'The failed write left %s, the running slot, damaged.' % a
    return ('A write of this recovery read %s, the running slot, back checked but did not settle the '
            'selector on it, and %s failed its check.' % (a, b))


def held_lines(i):
    """Bit 5: why the unit holds, said as INFO lets it be said (S-02.084)."""
    a = i['running']
    n = uncut_reach(i)
    if i['read_error']:
        why = ('What the flash holds is not known after a read that failed, so the unit claims no '
               'start.')
    elif n is None:
        if not i['slots'][0]['good'] and not i['slots'][1]['good']:
            reason = 'no slot is checked'
        elif i['selector_ok'] and i['sel_verified'] == 1:
            reason = ('the selector trusts %s, which is damaged'
                      % slot_name(selected(i['sel_index'])))
        else:
            reason = 'no start falls back from the slot the selector names to a checked one'
        why = ('The unit does not start at all if it is restarted or loses power now: %s.' % reason)
    else:
        why = ('A start that is not cut short would reach %s, but a cut of the first stage\'s own '
               'selector write could leave a selector that leads to no checked slot, so a restart is '
               'not safe yet.' % slot_name(n))
    return [why, "The unit holds the power switch's power-off and refuses a restart. Keep the cable "
                 'in until a write has settled the selector (the ways out below); with no host, '
                 'keep the unit powered.']


def reach_words(i):
    """In the damaged state with bit 5 clear: which good slot a restart starts (S-02.084)."""
    n = selected(i['sel_index'])
    if i['slots'][n]['good']:
        return 'a restart starts %s, which the selector names' % slot_name(n)
    if not i['slots'][1 - n]['good']:       # INFO disagrees with itself
        return 'the unit reports that a start reaches a checked slot'
    return ('the first restart starts %s, which the selector names and which is damaged: it may '
            'hang or open recovery again. The restart after it starts %s'
            % (slot_name(n), slot_name(1 - n)))


def leads_to_other(i):
    """In the damaged state: the selector leads to the other slot, which is good, so the unit
    writes the running slot again for either request (S-02.084)."""
    return damaged(i) and i['target'] == i['running']


def only_select(i, target):
    """In the damaged state, the target already good: the only checked slot, which the unit takes
    only with the image it holds, to settle the selector on it (S-02.084, status 18)."""
    return damaged(i) and target in (0, 1) and i['slots'][target]['good']


def record_words(s):
    return '%s, %d bytes, CRC-32 %08x' % (image_words(s), s['length'], s['crc'])


def ways_out(i):
    """The writes that end the damaged state, each ending by settling the selector on the slot
    it wrote (S-02.084), as INFO's target and offset 199 announce them."""
    a, b = i['running'], 1 - i['running']
    A, B = slot_name(a), slot_name(b)
    sb = i['slots'][b]
    if leads_to_other(i):
        words = ('write and write --running both write %s again (way 1), with no selector write '
                 'before it, and end by settling the selector on %s. %s is not touched' % (A, A, B))
        if i.get('front', 0) >= SAME_FRONT:
            return words + '.'
        return words + ', and its own image is refused: there is nothing to do with it.'

    if only_select(i, b):
        # S-02.084: the unit takes only a waffle-boot image with the running front
        if not (waffle(sb) and i['own_desc'] and sb['pin'] == i['pin']):
            first = ('%s holds %s, which this recovery does not take as a file.'
                     % (B, 'an image with another front' if waffle(sb) else 'no waffle-boot image'))
        else:
            first = ('write with the image %s holds (%s) settles the selector on %s: the slot is not '
                     'erased, only the selector is written.' % (B, image_words(sb), B))
    else:
        first = 'write writes %s.' % B
    way = i['run_way']
    if way == WAY_PLAIN and only_select(i, a):
        second = ('write --running with the image %s holds (%s) settles the selector on %s: the '
                  'slot is not erased, only the selector is written; another image is refused.'
                  % (A, record_words(i['slots'][a]), A))
    elif way == WAY_PLAIN:
        second = ('write --running writes %s again (way 1), with no selector write before it.' % A)
    elif way == WAY_SELECT_FIRST:
        second = 'write --running settles the selector on %s first and rewrites %s.' % (B, A)
    elif way == WAY_IN_PLACE:
        second = 'write --running writes %s again, in place.' % A
    else:
        second = 'write --running is refused: %s' % refusal_text(i['run_refusal'], i)
    return '%s %s Each write ends by settling the selector on the slot it wrote.' % (first, second)


def retry_lines(i):
    """The state after an earlier write of the running slot in this recovery, and what to do."""
    out = [READ_ERROR] if i['read_error'] else []
    if not i['established'] or i['running'] not in (0, 1):
        return out
    a, b = slot_name(i['running']), slot_name(1 - i['running'])
    if damaged(i):
        out.append(state_line(i))
        if i['held']:
            out += held_lines(i)
        else:
            out.append('But %s. restart is accepted.' % reach_words(i))
        return out + ['Ways out: ' + ways_out(i)]
    if i['moved'] and i['selector_ok'] and selected(i['sel_index']) != i['running']:
        line = 'This recovery already selected %s before a write of %s, the running slot' % (b, a)
        if i['target'] == i['running']:
            line += ': a write goes to %s again, with no selector write before it' % a
        out.append(line + '. A restart now starts %s.' % b)
    return out


def run_words(i):
    """What a write with --running would do."""
    a, b = slot_name(i['running']), slot_name(1 - i['running'])
    way = i['run_way']
    if way == WAY_PLAIN and only_select(i, i['running']):
        return ('would go to %s, which takes only the image it holds (%s), to settle the selector '
                'on it: the slot is not erased, only the selector is written'
                % (a, record_words(i['slots'][i['running']])))
    if way == WAY_PLAIN and damaged(i):
        return 'would go to %s, the running slot, again (way 1), with no selector write before it' % a
    if way == WAY_PLAIN:
        return 'would go to %s, which the selector does not name now' % a
    if way == WAY_SELECT_FIRST:
        return 'would go to %s, the running slot, after the selector names %s' % (a, b)
    if way == WAY_IN_PLACE:
        return ('would go to %s, the running slot, in place, since %s failed its check: an interruption '
                'leaves a unit that does not start' % (a, b))
    return 'refused: %s' % refusal_text(i['run_refusal'], i)


def settle_lines(i, way, target):
    """In the damaged state every write ends by settling the selector on its target (S-02.084);
    for an in-place write, the window runs to that settling unless the selector reads
    {target, 01}, which only INFO's selector, read without error, can say."""
    if not damaged(i):
        return []
    lines = ['The write ends by settling the selector on %s.' % slot_name(target)]
    if way == WAY_IN_PLACE and i['selector_ok'] and not i['read_error'] and \
            not (selected(i['sel_index']) == target and i['sel_verified'] == 1):
        lines.append('The risk lasts until then, not only to the slot\'s last program: the selector '
                     'reads %02x %02x now.' % (i['sel_index'], i['sel_verified']))
    return lines


# --- the actions on the image a slot holds (front version 11) ---

ACT_LABELS = {ACT_TRY: 'Try:', ACT_SWITCH: 'Switch:', ACT_KEEP: 'Keep:', ACT_PUT_BACK: 'Put back:'}
DONE_WORDS = {WAY_TRY: '%s marked for one trial start', WAY_SWITCH: '%s is the boot slot',
              WAY_KEEP: '%s kept as the boot slot', WAY_PUT_BACK: "%s's record made good again"}
TIER_WORDS = {
    TIER_SAME: 'holds an image with the front this unit runs',
    TIER_FRONT: 'holds another waffle-boot front: its recovery may not be this one, or may not open',
    TIER_FOREIGN: 'holds no waffle-boot front: there is no recovery there',
    TIER_BAD: 'failed its check: its record does not match its bytes, and nothing checks it at a start'}


def other_of(i):
    return 1 - i['running'] if i['established'] and i['running'] in (0, 1) else NO_SLOT


def act_slot(i, act):
    """The slot an action acts on: the other slot, or for a keep the slot INFO names."""
    return i['keep'] if act == ACT_KEEP else other_of(i)


def under_way(i):
    """The write or the action INFO shows under way, for the countdown."""
    if i['act'] in ACT_NAMES:
        return 'the %s of %s' % (ACT_NAMES[i['act']], slot_name(act_slot(i, i['act'])))
    return 'the write'


def act_text(act, st, i, hint=True):
    """An action's refusal (S-02.084), said for that action, with what to do."""
    B = slot_name(other_of(i))
    words = {
        (ACT_TRY, 19): ('a try needs the running slot checked with its front intact, and none comes once '
                        'this recovery has written the running slot or the selector',
                        'Restart first (wbrecover.py restart), with no key held.'),
        (ACT_SWITCH, 7): ("%s's stream does not pass S-02.091's rules: the boot ROM would not start it"
                          % B, 'Write a bootable image to it first (wbrecover.py write IMAGE).'),
        (ACT_SWITCH, 12): ('%s does not hold the front this unit runs: a switch to it would make it '
                           'the boot slot with no trial' % B, 'Try it once first (wbrecover.py try).'),
        (ACT_SWITCH, 19): ('this recovery has written the running slot or the selector',
                           'Restart first (wbrecover.py restart), with no key held.'),
        (ACT_SWITCH, 21): ('%s is tried, but its front has left no mark of a trial start: nothing '
                           'shows that it comes up' % B, 'Restart with no key held so that it starts '
                           'once; then switch off and on, hold Power + M4 and switch or keep.'),
        (ACT_KEEP, 19): ('this recovery has written the running slot or the selector, or begun a try '
                         'or a trial write', 'Restart first (wbrecover.py restart), with no key held.'),
        (ACT_KEEP, 20): ('no slot is tried where a keep can take it: from this front the other slot, '
                         'tried, with the selector on the running slot with verified 01; in a trial '
                         'start the running slot', 'Run info; a try comes first (wbrecover.py try).'),
        (ACT_KEEP, 21): ('%s is tried, but its front has left no mark of a trial start: nothing shows '
                         'that it comes up' % B, 'Restart with no key held so that it starts once; '
                         'then switch off and on, hold Power + M4 and keep.'),
        (ACT_PUT_BACK, 20): ('%s is not tried: there is no trial mark to take off' % B,
                             'Nothing to do.'),
    }
    for act2 in (ACT_TRY, ACT_SWITCH):
        words[(act2, 22)] = ('%s is already the boot slot (verified 01)' % B,
                             'Nothing to do: restart the unit (wbrecover.py restart) to start it.')
        words[(act2, 23)] = ('%s failed its check and is not tried: nothing checks it at a start' % B,
                             'Write an image to it first (wbrecover.py write IMAGE).')
    if (act, st) not in words:
        return status_text(st, hint=hint, i=i)
    what, tip = words[(act, st)]
    if (i['spoiled'] or i.get('unsettled')) and 'estart' in tip:
        tip = ''
    return what + ('. ' + tip if tip and hint else '')


def act_goal(i, act):
    k = slot_name(act_slot(i, act))
    return {ACT_TRY: 'would start %s once, at the next start' % k,
            ACT_SWITCH: 'would make %s the boot slot' % k,
            ACT_KEEP: 'would keep %s, with no image sent' % k,
            ACT_PUT_BACK: "would make %s's record good again, with no selector write" % k}[act]


def keep_after_trial(i):
    """A keep would start now, of the slot a switch names: the slot came up from its trial, and the
    recovery menu offers its keep in place of the switch."""
    return i['acts'][ACT_KEEP - 1] == 0 and act_slot(i, ACT_KEEP) == act_slot(i, ACT_SWITCH)


def action_lines(i):
    """Front 11: the other slot's tier, and what each action would do now, unforced and forced."""
    if not i['v11'] or other_of(i) == NO_SLOT:
        return []
    out = []
    if i['tier'] in TIER_WORDS:
        out.append('Other:    %s %s' % (slot_name(other_of(i)), TIER_WORDS[i['tier']]))
    for act in (ACT_TRY, ACT_SWITCH, ACT_KEEP, ACT_PUT_BACK):
        un, fo = i['acts'][act - 1], i['forced'][act - 1]
        if un == 0:
            words = act_goal(i, act)
        elif fo == 0:
            words = 'with --force only (%s): %s' % (act_text(act, un, i, False), act_goal(i, act))
        else:
            words = 'refused: %s' % act_text(act, fo, i, False)
        if act == ACT_SWITCH and keep_after_trial(i):
            words += '; after its trial, keep is the action'
        out.append('%-10s%s' % (ACT_LABELS[act], words))
    return out


def trial_hint(i):
    """A try first, where one is possible now."""
    if i['acts'][ACT_TRY - 1] == 0:
        return ' A try starts it once first: wbrecover.py try.'
    if i['forced'][ACT_TRY - 1] == 0:
        return ' A try starts it once first: wbrecover.py try --force.'
    return ''


def slot_words(i, k):
    """What slot k holds, from INFO."""
    s = i['slots'][k]
    return 'version %s' % version(s['version']) if s['has_desc'] else 'no waffle-boot image'


def switch_lines(i, b):
    """The warning before a switch for good, by the tier of the slot switched to."""
    B, A = slot_name(b), slot_name(1 - b)
    tier = i['tier']
    if tier == TIER_BAD:
        return ['DANGER: %s failed its check: its record does not match its bytes. Switched to, the selector trusts it, '
                'and nothing will ever check this slot again: if its image does not start, there may '
                'be no way back without a flash programmer.%s' % (B, trial_hint(i))]
    if tier == TIER_FOREIGN:
        return ['WARNING: %s holds no waffle-boot front. Once switched, there is no recovery from '
                'there: Power + M4 does nothing, and nothing on the unit brings waffle-boot back.%s'
                % (B, trial_hint(i))]
    if tier == TIER_FRONT:
        return ['WARNING: %s holds another waffle-boot front (%s). Once switched, recovery may not be '
                'reachable from it: if that front does not start, or its recovery is not this one, '
                'there may be no way back to %s.%s' % (B, slot_words(i, b), A, trial_hint(i))]
    return ['%s holds an image with the front this unit runs (%s). A switch makes it the boot slot: every start '
            'boots %s from now on.%s' % (B, slot_words(i, b), B, trial_hint(i))]


def try_lines(i, b):
    """What a try of slot b does, by its tier."""
    B, A = slot_name(b), slot_name(1 - b)
    lines = ['The next start runs %s once, as it lies (%s); the starts after it boot %s again, '
             'until a keep (wbrecover.py keep) after that start.' % (B, slot_words(i, b), A)]
    tier = i['tier']
    if tier == TIER_FRONT:
        lines.append('%s holds another waffle-boot front: its recovery may not open in that start. '
                     'If it does not come up, switch the unit off and on: %s boots again.' % (B, A))
    elif tier == TIER_FOREIGN:
        lines.append('%s holds no waffle-boot front: there is no recovery in that start. Switch the '
                     'unit off and on to come back to %s.' % (B, A))
    else:
        lines.append('If it does not come up, switch the unit off and on: %s boots again.' % A)
    if b == 1:
        lines.append('Slot 2 has two short windows: a power cut during the selector erase of this '
                     'try, or in the first instant of the trial start, can leave Slot 2 trusted '
                     'before its image has run; if it then does not start, the unit is lost.')
    return lines


def keep_lines(i, k):
    K, other = slot_name(k), slot_name(1 - k)
    if k == i['running']:
        lines = ['This is the trial start of %s. The keep makes %s the boot slot: its record is '
                 'made good and the selector names it; its image is not written again, and %s is kept '
                 'as it is.' % (K, K, other)]
        if i['why'] == 2:
            lines.append('WARNING: recovery runs because the image check failed: this image runs, but '
                         'does not pass its own check. Keeping it leaves a unit that starts only into '
                         'recovery.')
        return lines
    lines = ['%s is tried; the keep makes it the boot slot: its record is made good and the '
             'selector names it, to be checked at the next start. Its image is not written again, '
             'and %s is kept as it is.' % (K, other)]
    if i['tier'] in (TIER_FRONT, TIER_FOREIGN):
        lines.append('%s %s.' % (K, TIER_WORDS[i['tier']]))
    return lines


def put_back_lines(i, b):
    B, A = slot_name(b), slot_name(1 - b)
    sel = sel_known(i)
    line = ("%s's trial mark is taken off: its record is made good again, as before the trial, and "
            'its image is not written.' % B)
    if sel is not None and sel[0] == b and sel[1] != 1 and not damaged(i):
        line += (' The selector names %s for a try that has not started; it is first settled back on '
                 '%s.' % (B, A))
    else:
        line += ' The selector is not written.'
    return [line]


# what --force lifts for an action, and what could go wrong (S-02.084)
def force_lines(act, st, i):
    """The warning before a forced action: the refusal lifted, and its risk."""
    A, B = slot_name(i['running']), slot_name(other_of(i))
    lines = ['Without --force the unit refuses this: %s.' % act_text(act, st, i, False).rstrip('.')]
    if st in (3, 4):
        lines.append('Forced, the %s goes on over the selector as it reads now (%02x %02x), which a '
                     'start has not settled yet.' % (ACT_NAMES[act], i['sel_index'], i['sel_verified']))
    elif act == ACT_TRY and st == 19:
        lines.append('Forced, the trial start comes back to %s all the same: if %s does not come up, '
                     'switching off and on starts %s, which may not start either.' % (A, B, A))
    elif st == 23:
        lines.append('Forced, the unit acts on %s as it lies: nothing has checked its image.' % B)
    elif st == 21:
        lines.append('Forced, %s is kept as the boot slot although its front never showed that it starts: '
                     'if it does not, recovery may not be reachable.' % B)
    elif st in (19, 20):
        lines.append('Forced, the %s goes on from the state this recovery left.' % ACT_NAMES[act])
    return lines


def countdown_note(i):
    """Front 11: what follows the confirmation, before the first erase."""
    if not v11(i):
        return []
    if i['held']:
        return ['The unit begins at once, with no countdown: it holds the power-off while a write of '
                'this recovery has left no checked slot to start.']
    return ['Once confirmed, the unit counts down %d s before its first erase: switching it off or '
            'unplugging USB in that time stops it, with the flash untouched.' % COUNTDOWN]


def describe(i):
    """INFO as lines a person reads."""
    out = ['Recovery runs because %s.' % WHY.get(i['why'], 'of reason %d' % i['why'])]
    if i['v20']:
        out.append(device_line(i))
    sel = selected(i['sel_index']) if i['selector_ok'] else None
    if i['established']:
        out.append('Running:  %s' % slot_name(i['running']))
    else:
        out.append('Running:  not established (the loaded image matches neither slot whole, or '
                   'both alike)')
    if i['selector_ok']:
        out.append('Selector: names %s, verified %02x%s%s' % (
            slot_name(sel), i['sel_verified'],
            '' if i['sel_verified'] == 1 else ' (not settled: the next start checks it)',
            ' (this recovery selected it, before a write of the running slot)' if i['moved'] else ''))
    else:
        out.append('Selector: could not be read')
    if i['own_desc']:
        out.append("Front:    the running image's version %s, front pin %s"
                   % (version(i['version']), i['pin'].hex()))
    else:
        out.append("Front:    the running image's descriptor was not found")
    for k, s in enumerate(i['slots']):
        tags = [t for t, on in (('running', i['established'] and i['running'] == k),
                                ('boot slot', sel == k)) if on]
        kind = slot_kind(s)
        line = '%s:   %s' % (slot_name(k), slot_state(s))
        if kind is not None:
            line += ', %s, %d bytes, CRC-32 %08x' % (kind_text(kind), s['length'], s['crc'])
        elif s['has_desc']:
            line += ', version %s' % version(s['version'])
        if s['has_desc']:
            line += ', front %s' % ('present' if s['has_front'] else 'damaged')
            line += ' (pin %s...)' % s['pin'][:8].hex()
        if tags:
            line += ' [%s]' % ', '.join(tags)
        out.append(line)
    if i['target'] in (0, 1):
        line = 'Write:    would go to %s' % slot_name(i['target'])
        t = i['slots'][i['target']]
        if only_select(i, i['target']):
            line += (', which takes only the image it holds (%s, %d bytes, CRC-32 %08x) and is then '
                     'selected: the slot is not erased, only the selector is written'
                     % (image_words(t), t['length'], t['crc']))
        elif leads_to_other(i):
            line += ', the running slot, again (way 1), with no selector write before it'
        elif t['good']:
            line += ', replacing a checked image (%s)' % image_words(t)
        out.append(line)
    else:
        out.append('Write:    refused: %s' % refusal_text(i['refusal'], i))
    if i['v6']:
        out.append('          with --running: %s' % run_words(i))
    out += action_lines(i)
    if i['v6']:
        out.append('Battery:  %s' % battery(i['batt'])[1])
    ph = i['phase']
    state = PHASES[ph] if ph < len(PHASES) else 'phase %d' % ph
    if ph == RECEIVING:
        state += ', %d of %d bytes' % (i['got'], i['len'])
    elif ph == COUNTING:
        state += ': %d s left before the first erase of %s. Switching the unit off or unplugging ' \
            'USB now stops it, with nothing written' % (i['count'], under_way(i))
    elif ph == WRITING:
        state += '%s, %.1f%%' % ({WAY_IN_PLACE: ' in place', WAY_TRIAL: ' for one trial start',
                                  WAY_SELECT: ' a select', WAY_TRY: ': a try',
                                  WAY_SWITCH: ': a switch', WAY_KEEP: ': a keep',
                                  WAY_PUT_BACK: ': a put-back'}.get(i['way'], ''),
                                 i['progress'] / 10.0)
    elif ph == DONE and i['way'] in DONE_WORDS:
        state += ': %s' % (DONE_WORDS[i['way']] % slot_name(i['written']))
    elif ph == DONE:
        state += ': %s %s and read back, SHA-256 %s' % (
            slot_name(i['written']), {WAY_TRIAL: 'written for one trial start',
                                      WAY_SELECT: 'selected'}.get(i['way'], 'written'),
            i['readback'].hex())
    elif ph == FAILED:
        state += ': %s' % status_text(i['result'], i['image_rc'],     # its hint is a plain write's
                                      hint=plain_hint(i), i=i)
    out.append('State:    ' + state)
    if i['refused']:        # front 11: the state above is the write made last
        out.append('Since:    %s: %s' % ('the next write or action' if i['refused'] == 24
                                         else 'the last request after it was refused',
                                         status_text(i['refused'], i=i)))
    # a trial write that ran and failed (not a refusal before it)
    failed = ph == FAILED and i['way'] == WAY_TRIAL and i['result'] in (9, 10, 11)
    return out + tried_lines(i, failed) + retry_lines(i)


def sel_known(i):
    """(the slot the selector names, its verified byte), or None when it was not read."""
    if not i['selector_ok'] or i['read_error']:
        return None
    return selected(i['sel_index']), i['sel_verified']


def trial_start(i):
    """This recovery runs in a trial start (S-02.022): the running slot is tried and the selector
    names the other slot with verified 01, as the first stage leaves it."""
    a = i['running']
    return i['established'] and a in (0, 1) and i['slots'][a]['tried'] and \
        sel_known(i) == (1 - a, 1)


def keeps(i):
    """The commands that keep a tried slot."""
    return 'wbrecover.py keep (or select IMAGE)' if v11(i) else 'select IMAGE'


def trial_begun(i):
    """This recovery began a trial write (S-02.084): one is running, done, or failed after its
    first erase. A select is then refused with status 19 until a restart."""
    return i['way'] == WAY_TRIAL and (i['phase'] in (WRITING, DONE) or
                                      (i['phase'] == FAILED and i['result'] in (9, 10, 11)))


def select_blocked(i):
    """Why the old front would not take a select now, or '' (S-02.084's start condition)."""
    a = i['running']
    if trial_begun(i):
        return ('this recovery began a trial write, so a select needs a restart first (status 19)')
    if damaged(i) or i['moved']:
        return ('this recovery wrote the selector or began a write of %s (status 19)' % slot_name(a))
    if sel_known(i) != (a, 1):
        return 'the selector does not read %s with verified 01' % slot_name(a)
    if not i['slots'][a]['has_front']:
        return "%s's front is not intact" % slot_name(a)
    return ''


def tried_lines(i, failed_trial=False):
    """What a tried slot means in the state INFO reports: whether a trial start comes, has come,
    or cannot come, and whether a select would be taken, judged from the selector, the started
    bit and this recovery's own writes."""
    a = i['running']
    if not i['established'] or a not in (0, 1):
        return []
    sel = sel_known(i)
    A, B = slot_name(a), slot_name(1 - a)
    if i['slots'][a]['tried']:
        if trial_start(i):
            return ['This is the trial start of %s: %s keeps it; switching off and on starts %s '
                    'again.' % (A, keeps(i), B)]
        if sel is None:
            return ['%s is tried and running, but the selector could not be read, so the tool cannot '
                    'tell whether this is a trial start: run info again before a select.' % A]
        if sel == (a, 1) and v11(i):
            return ['%s is tried, but the selector trusts it although it never passed a trial: '
                    'every start boots it, and a select is refused. To leave this state, switch to '
                    '%s from here (wbrecover.py switch, %s), if it is checked and holds a stream the '
                    'boot ROM can start; from its recovery, wbrecover.py try starts %s once, or a '
                    'plain write replaces it.' % (A, B, SLOT_MENU, A)]
        if sel == (a, 1):
            return ['%s is tried, but the selector trusts it although it never passed a trial: '
                    'every start boots it, and a select is refused. From its recovery, write '
                    '--new-front IMAGE again for a trial, or a plain write replaces %s.' % (A, A)]
        if sel[0] == a:
            return ['%s is tried and running, and the selector still names it unverified (verified '
                    '%02x): this trial start did not finish its selector step. The next start is '
                    'another trial start of %s, and the select works there.' % (A, sel[1], A)]
        return ['%s is tried and running, but the selector names %s with verified %02x, not 01, so '
                'this is not a trial start: a select is refused here.' % (A, B, sel[1])]
    t = 1 - a
    if not i['slots'][t]['tried']:
        return []
    again = ('Try it again with wbrecover.py try (the image it holds) or write --new-front IMAGE, '
             'then restart with no key held.' if v11(i) else
             'Write it again with --new-front, then restart with no key held.')
    if sel is not None and sel[0] == t and sel[1] != 1:
        if failed_trial:
            return ['The failed trial write left %s tried, with the selector naming it unverified: '
                    'the next start runs it once, a trial start, and a select comes after that start '
                    '(in this recovery it is refused).' % B]
        return ['%s is tried: the next start runs it once, its trial start.' % B]
    if i['slots'][t]['started']:
        if failed_trial:
            return ["The failed trial write did not reach %s's record: the earlier trial's mark "
                    'still stands for the image %s held before. A select of that image needs a '
                    'restart first (in this recovery it is refused, status 19).' % (B, B)]
        why = select_blocked(i)
        if why:
            return ['%s is tried and its front marked its trial start, but a select is not taken '
                    'now: %s.' % (B, why)]
        return ['%s is tried and its front marked its trial start: %s keeps it.' % (B, keeps(i))]
    out = []
    if failed_trial:
        out.append('The failed trial write left %s carrying the trial mark: it cannot be selected.' % B)
    if sel is None:
        return out + ['%s is tried; the selector could not be read.' % B]
    n, v = sel
    if n == t and v11(i):
        return out + ['%s is tried, but the selector trusts it although it never passed a trial: '
                      'the next start, and every one after, boots it. To end this, switch to %s from '
                      "%s's recovery (wbrecover.py switch, %s); from here, wbrecover.py put-back "
                      'makes its record good again, as it was before the trial.' % (B, A, B, SLOT_MENU)]
    if n == t:
        return out + ['%s is tried, but the selector trusts it although it never passed a trial: '
                      'the next start, and every one after, boots it.' % B]
    if v == 1:
        line = ('%s is tried, its front has left no mark of a trial start, and the selector trusts '
                '%s, so no trial start will come: a select is refused. %s' % (B, A, again))
        if not failed_trial:
            line += (' If the unit did not come up in that trial start, do not write the same image '
                     'again: its front does not start; rebuild it. A newer front in %s also marks such '
                     'a trial as spent on its next start, which the tool sees only as not started: '
                     'either way the trial write must be repeated to try again.' % A)
        return out + [line]
    return out + ['%s is tried, but the selector names %s (verified %02x): the next start checks %s '
                  'and boots it, so no trial start will come. %s' % (B, A, v, A, again)]


# --- the image ---

def blocks_of(data):
    return check_stream.ldr.parse(data)


def check_image(data):
    """(problems, front) for an image as a slot image: S-02.044's size, S-02.091's rules,
    and a waffle-boot front as the unit's scan() judges one (front_scan()). front holds the
    descriptor's fields, pin_calc the front's own pin and pin_ok whether the two agree."""
    probs = []
    if not 1 <= len(data) <= SLOT_CAP:
        return ['the file is %d bytes; a slot image is 1 to %d bytes (S-02.044)' % (len(data), SLOT_CAP)], None
    if len(data) > WB_IMAGE_MAX:
        return ['the file is %d bytes; a waffle-boot image is at most %d (0x%x) bytes, since the '
                "slot's last two sectors hold its settings and its start log (S-02.104, S-02.095)"
                % (len(data), WB_IMAGE_MAX, WB_IMAGE_MAX)], None
    res = check_stream.check('bootloader', data, first_stage_stack=(0x11807000, 0x11808000))
    probs += ['stream rule %s failed%s' % (r, ': ' + d if d else '') for r, ok, d in res if not ok]
    if any(r == 'parse' for r, ok, _ in res if not ok):
        return probs, None
    blocks = blocks_of(data)
    scan = front_scan(data, blocks)
    if len(scan['descs']) != 1:
        probs.append('no waffle-boot front: %d descriptor blocks at %08x'
                     % (len(scan['descs']), mkldr.FR_DESC_ADDR))
        return probs, None
    b = scan['descs'][0]
    front = parse_desc(data[b['payload_offset']:b['payload_offset'] + min(b['count'], DESC_MAX)])
    if front is None:
        probs.append('no waffle-boot front: the descriptor does not parse')
        return probs, None
    if scan['bad']:
        probs.append('no waffle-boot front: %s' % scan['bad'])
    l2, l1c = mkldr.FR_WINDOWS[2], mkldr.FR_WINDOWS[0]
    if scan['first'] is None or not l2[0] <= scan['first'] < l2[1] or \
            scan['final'] is None or not l2[0] <= scan['final'] < l2[1]:
        probs.append("no waffle-boot front: the entry is not in the front's L2 window")
    if scan['init'] is None or not l1c[0] <= scan['init'] < l1c[1]:
        probs.append("no waffle-boot front: the INIT target is not in the front's L1 code window")
    if scan['crc'] != front['front_crc']:
        probs.append("no waffle-boot front: the front's bytes do not match the descriptor's CRC")
    pin = scan['pin']
    front['pin_calc'] = pin.digest()
    front['pin_ok'] = front['pin_calc'] == front['pin']
    return probs, front


DESC_MAX = mkldr.FR_DESC_HEAD + 8 * mkldr.FR_DESC_REGIONS + 4


def _meets(a, b, span):
    return a < span[1] and span[0] < b


def front_scan(data, blocks):
    """The front's part of the device's scan() (src/front/fr_slots.c); the two must agree.
    Blocks in stream order up to FINAL; a block with a destination (a count, not IGNORE) that
    meets the shared block must be the descriptor (data, at its address, inside the block);
    one that meets a front window must be data inside that window, and then counts, in stream
    order, toward the front's CRC and pin. Anything else there: no waffle-boot front."""
    out = {'descs': [], 'bad': '', 'first': None, 'final': None, 'init': None,
           'crc': 0, 'pin': hashlib.sha256()}
    for b in blocks:
        t, n, fl = b['target'], b['count'], b['flags']
        end = t + n
        data_blk = not fl & check_stream.ldr.FILL
        if fl & check_stream.ldr.FIRST and out['first'] is None:
            out['first'] = t
        if fl & check_stream.ldr.INIT and out['init'] is None:
            out['init'] = t
        if n and not fl & check_stream.ldr.IGNORE:
            if _meets(t, end, mkldr.FR_SHARED):
                if data_blk and t == mkldr.FR_DESC_ADDR and end <= mkldr.FR_SHARED[1]:
                    out['descs'].append(b)
                elif not out['bad']:
                    out['bad'] = 'a block at %08x meets the shared block and is not the descriptor' % t
            else:
                for lo, hi in mkldr.FR_WINDOWS:
                    if not _meets(t, end, (lo, hi)):
                        continue
                    if data_blk and lo <= t and end <= hi:
                        payload = data[b['payload_offset']:b['payload_offset'] + n]
                        out['crc'] = zlib.crc32(payload, out['crc'])
                        out['pin'].update(struct.pack('<II', t, n) + payload)
                    elif not out['bad']:
                        out['bad'] = ('a %s block at %08x+%x meets the front\'s window %08x-%08x '
                                      'and is not data inside it'
                                      % ('FILL' if not data_blk else 'data', t, n, lo, hi))
                    break
        if fl & check_stream.ldr.FINAL:
            out['final'] = t
            break
    out['crc'] &= 0xFFFFFFFF
    return out


PIN_FILE, PROTO_H = 'src/front/front.pin', 'src/front/fr_proto.h'


def _pin_of(text):
    try:
        return bytes.fromhex(text.split()[0])
    except (IndexError, ValueError):
        return None


def _front_version_of(text):
    m = re.search(r'#define\s+FR_FRONT_VERSION\s+(\d+)', text or '')
    return int(m.group(1)) if m else None


def _git(root, *args):
    """git's output, or None on any failure: no shell, a time limit, and no repository named by
    the environment."""
    env = {k: v for k, v in os.environ.items() if not k.startswith('GIT_')}
    try:
        r = subprocess.run(['git', '-C', root] + list(args), capture_output=True, text=True,
                           timeout=10, env=env)
    except (OSError, subprocess.SubprocessError, ValueError):
        return None
    return r.stdout if r.returncode == 0 else None


def signed_pins(root=os.path.join(HERE, '..')):
    """What this checkout says of front pins, or None when it says nothing:
    {'git': whether HEAD's history of src/front/front.pin could be read,
     'pins': {pin: the front's version from fr_proto.h at the same commit, or None} for every
             pin a commit of that history set (signed),
     'now': the working tree's pin, or None}."""
    try:
        with open(os.path.join(root, PIN_FILE)) as f:
            now = _pin_of(f.read())
    except OSError:
        now = None
    revs = _git(root, 'log', '--format=%H', 'HEAD', '--', PIN_FILE)
    pins = {}
    for rev in (revs or '').split():
        text = _git(root, 'show', '%s:%s' % (rev, PIN_FILE))
        pin = _pin_of(text) if text is not None else None
        if pin is None:
            continue            # a commit that removed it
        if pins.get(pin) is None:
            pins[pin] = _front_version_of(_git(root, 'show', '%s:%s' % (rev, PROTO_H)))
    ok = revs is not None and bool(pins)
    if not ok and now is None:
        return None
    return {'git': ok, 'pins': pins if ok else {}, 'now': now}


def signed_words(pin, lookup):
    """(state, version, words) for a front pin: state 'signed' (a commit of this checkout set it
    in src/front/front.pin), 'unsigned' or 'unknown'. version only from that commit."""
    try:
        got = lookup()
    except Exception:       # noqa: BLE001 - any failure means it cannot tell
        got = None
    if got is None:
        return 'unknown', None, ('cannot tell whether this checkout has signed it: neither '
                                 '%s nor its git history could be read' % PIN_FILE)
    if not got['git']:
        return 'unknown', None, (
            'cannot tell whether this checkout has signed it: no git history of %s; %s' % (
                PIN_FILE, 'it is the pin this checkout has now' if pin == got['now'] else
                'it is not the pin this checkout has now'))
    if pin in got['pins']:
        v = got['pins'][pin]
        return 'signed', v, 'a pin this checkout has signed: a commit set it in %s (%s)' % (
            PIN_FILE, 'front v%d' % v if v is not None else 'its front version not known')
    if pin == got['now']:
        return 'unsigned', None, ("NOT a pin this checkout has signed: the working tree's pin, "
                                  'not in a commit')
    return 'unsigned', None, 'NOT a pin this checkout has signed'


def parse_desc(raw):
    if len(raw) < mkldr.FR_DESC_HEAD + 4:
        return None
    magic, ver, size, entry, n, main_crc, front_crc = struct.unpack_from('<IHHIIII', raw, 0)
    if magic != mkldr.FR_DESC_MAGIC or ver != mkldr.FR_DESC_VERSION or n > mkldr.FR_DESC_REGIONS \
            or size != mkldr.FR_DESC_HEAD + 8 * n + 4 or size > len(raw) \
            or zlib.crc32(raw[:size - 4]) & 0xFFFFFFFF != struct.unpack_from('<I', raw, size - 4)[0]:
        return None
    return {'entry': entry, 'front_crc': front_crc, 'version': struct.unpack_from('<4H', raw, 0x18),
            'pin': bytes(raw[0x20:0x40])}


# --- the commands ---

def hello(link):
    """HELLO, and what the unit understands, by its front's version: protocol 1 from fronts 5 to
    10, protocol 2 (ACT, the force flag, the countdown) from front 11."""
    h = parse_hello(link.request(HELLO)[0])
    want = PROTO if h['front_version'] >= ACTS_FRONT else OLD_PROTO
    if h['proto'] != want:
        raise Failure('the unit speaks recovery protocol %d with front version %d; this tool speaks '
                      'protocol %d to fronts 5 to %d and protocol %d from front %d'
                      % (h['proto'], h['front_version'], OLD_PROTO, ACTS_FRONT - 1, PROTO, ACTS_FRONT))
    link.max_data = max(1, min(h['max_data'], MAX_DATA))
    link.front = h['front_version']
    return h


def get_info(link, timeout=None):
    i = parse_info(link.request(INFO, timeout=timeout)[0])
    if link.front >= 6 and not i['v6'] or link.front >= ACTS_FRONT and not i['v11']:
        raise Failure('INFO reply too short for a version %d front' % link.front)
    i['front'] = link.front
    return i


def too_old(out, link, what):
    """A request a front before version 11 does not know: never sent."""
    out('This unit runs front version %d: %s needs a version %d front. Nothing was sent.'
        % (link.front, what, ACTS_FRONT))
    return 1


def cmd_info(link, out=print):
    h = hello(link)
    i = get_info(link)
    out('Front version %d, protocol %d.' % (h['front_version'], h['proto']))
    for line in describe(i):
        out(line)
    return 0


def device_line(i):
    """INFO's device (front 20, S-18.004): the device and how, with n and c when not identified."""
    name = DEVICES[i['device']] if i['device'] < len(DEVICES) else 'device %d' % i['device']
    if i['dev_how'] == HOW_TABLE:
        return 'Device:   %s, identified by its first stage' % name
    return 'Device:   not identified (first stage n %d, c %08x): recovery runs as on the %s' % (
        i['fs_n'], i['fs_c'], name)


def progress(out, what, done, total):
    out('\r%s: %5.1f%%' % (what, 100.0 * done / total if total else 100.0), end='')


def ask_tty(prompt):
    try:
        return input(prompt)
    except EOFError:
        return ''


def confirm_lines(i, way, target):
    """The warning for the write BEGIN announced (this way, to this target), from INFO."""
    if way == WAY_IN_PLACE:
        return in_place_lines(i, target)
    return base_lines(i, way, target) + settle_lines(i, way, target)


def base_lines(i, way, target):
    a, b = i['running'], 1 - i['running']
    A, B = slot_name(a), slot_name(b)
    sa, sb = i['slots'][a], i['slots'][b]
    if way == WAY_SELECT_FIRST:
        lines = ['This erases and rewrites %s, the running slot. The unit first selects %s: if the '
                 "write is interrupted, the unit starts %s's image (%s)." % (A, B, B, image_words(sb))]
        if not waffle(sb):
            lines.append('The unit is then on that bootloader, without this recovery.')
        return lines
    if only_select(i, target):
        T = slot_name(target)
        return ['This settles the selector on %s, which already holds this image (%s). The slot is not '
                'erased: the unit reads it back and then writes only the selector, to name %s. %s '
                'keeps its image if this is interrupted.'
                % (T, image_words(i['slots'][target]), T, T)] + \
            (held_lines(i) if i['held'] else [])
    if target == a and damaged(i):      # S-02.084's retry rule, way 1
        lines = ['The unit announced a write of %s, the running slot, with no selector write before '
                 'it (way 1): the selector leads to %s, which is checked and is not touched.' % (A, B)]
        return lines + (held_lines(i) if i['held'] else [])
    if target == a:     # a retry after the other slot was selected
        lines = ['This write goes to %s, the running slot, with no selector write before it: %s is '
                 "selected, and an interruption starts %s's image (%s)." % (A, B, B, image_words(sb))]
        if sa['good']:
            lines.append('%s holds a checked image (%s), which this write replaces.' % (A, image_words(sa)))
        return lines
    if damaged(i):
        lines = [state_line(i) + ' An interruption of this write leaves no settled selector either: '
                 'until a write has ended well, a restart is not safe.']
        return lines + (held_lines(i) if i['held'] else [])
    if sb['good']:
        lines = ['%s holds a checked image (%s), which this write replaces.' % (B, image_words(sb))]
        if i['v6'] and not sa['good']:
            lines.append('%s, the running slot, failed its check, so %s holds the only checked image. write '
                         '--running is the safer choice in this state: it selects %s first and '
                         'rewrites %s, so an interruption starts %s.' % (A, B, B, A, B))
        return lines
    return ['%s holds no checked image, so nothing checked is lost. %s, the running slot, is left as it '
            'is: if the write is interrupted, the unit starts as before.' % (B, A)]


def in_place_lines(i, target=None):
    """The warning before an in-place write, with the battery report INFO carries."""
    a, b = slot_name(i['running']), slot_name(1 - i['running'])
    state, words = battery(i['batt'])
    lines = ['DANGER: %s holds no checked image, so this write erases %s, the running slot, and '
             'writes it in place.' % (b, a),
             'If the write is interrupted (a power cut, a pulled cable with no charged battery, the '
             'power switch), the unit cannot be repaired without a flash programmer.']
    if damaged(i):
        lines.append('%s is already damaged by a failed write of this recovery.' % a)
        lines += held_lines(i) if i['held'] else []
        lines += settle_lines(i, WAY_IN_PLACE, i['running'] if target is None else target)
    else:
        lines.append('The unit offers this only because a write of %s failed in this recovery.' % b)
    lines.append('Battery: %s.' % words)
    if state == 'charged':
        lines.append('The battery carries the unit if the cable comes out. Do not touch the power '
                     'switch until the write has ended.')
    else:
        lines.append('Keep the cable in and do not touch the power switch until the write has ended.')
    return lines


def refused(out, st, i, before):
    if st == 15:
        out('Refused: the other slot failed its check, and %s. %s %s' % (
            STATUS[15][0], STATUS[15][1],
            'Nothing was sent.' if before else 'Nothing was sent beyond BEGIN.'))
        return 1
    text = refusal_text(st, i) if before else status_text(st, i=i)
    out('Refused%s: %s' % (' before sending' if before else '', text))
    if st == 18:
        out('Ways out: ' + ways_out(i))
    return 1


def only_good_refusal(out, i, target):
    """Status 18's case, judged from INFO's record of the target before BEGIN."""
    T = slot_name(target)
    out('Refused before sending: %s is the only checked slot, so the unit accepts only the image it '
        'holds (%s), to settle the selector on it, and this file is not it (status 18).'
        % (T, record_words(i['slots'][target])))
    out('Ways out: ' + ways_out(i))
    return 1


def short(pin):
    return pin[:8].hex() + '...'


def pin_refusal(out, front):
    """The descriptor's pin is a claim: a file whose front hashes to another pin is refused."""
    out("Refused before sending: the file's descriptor claims front pin %s, but its front's own "
        "bytes give %s: the file is not a build of this tree's tools. Nothing was sent."
        % (short(front['pin']), short(front['pin_calc'])))
    return 1


def other_front_refusal(out, link, i, front, new_front):
    """Another front, refused before BEGIN: without --new-front, or on a front before version 7."""
    if link.front < TRIAL_FRONT and new_front:
        out('Refused before sending: the file carries another front, and this unit runs front '
            'version %d: a front before version %d cannot take another front in recovery. The way '
            'is the test OS installer (wiki/Test-OS.md). Nothing was sent.'
            % (link.front, TRIAL_FRONT))
        return 1
    line = ('Refused before sending: the file carries another front (pin %s; the unit runs pin %s). '
            'Recovery writes another front only for one trial start, with write --new-front IMAGE.'
            % (short(front['pin']), short(i['pin'])))
    if link.front < TRIAL_FRONT:
        line += (' This unit runs front version %d, which cannot: the way is the test OS installer '
                 '(wiki/Test-OS.md).' % link.front)
    out(line + ' Nothing was sent.')
    return 1


def trial_lines(h, i, front, target, signed):
    """The warning before a trial write (way 4) to target; signed is signed_words()'s answer."""
    a = i['running']
    state, ver, words = signed
    lines = ["The file's front is not the one this unit runs.",
             '  running: front %d, pin %s   file: pin %s, %s'
             % (h['front_version'], short(i['pin']), short(front['pin']), words)]
    if state == 'unsigned':
        lines.append('WARNING: this checkout never pinned that front, so nothing here says it was '
                     'built from this tree or has ever started on a unit. Go on only if you know '
                     'where the file comes from.')
    elif state == 'unknown':
        lines.append('Make sure of where the file comes from before going on.')
    if ver is None:
        lines.append(OLD_FRONT_RISK % slot_name(target))
    t = i['slots'][target]
    if t['good'] or t['tried']:
        lines.append('%s holds an image (%s), which this write erases: it is lost, whether or not '
                     'the new front starts.' % (slot_name(target), image_words(t)))
    else:
        lines.append('%s holds no checked image, so nothing checked is lost.' % slot_name(target))
    lines.append('It will be written to %s and started ONCE. If the unit does not come up, switch '
                 'it off and on: it boots %s again. Nothing is selected until a select '
                 '(wbrecover.py select IMAGE).' % (slot_name(target), slot_name(a)))
    if target == 1:
        lines.append('Slot 2 has two short windows: a power cut during the selector erase at the end '
                     'of this write, or in the first instant of the trial start, can leave Slot 2 '
                     'trusted before its front has run; if that front then does not start, the unit '
                     'is lost.')
    return lines


def end_lost(link, before, i, way, sha):
    """END's reply was lost and END sent again found no transfer open: whether INFO now reports
    this transfer ('info': its write, or its refusal), a refusal while INFO shows a write made
    earlier in this recovery ('refused'), or cannot tell ('unknown'). From version 8 a refused END
    leaves that earlier write in INFO (S-02.093); a write that ran shows the way BEGIN announced
    and, done, this file's read-back. A failed write with that way is this one or the earlier one,
    unless INFO before BEGIN, idle or ended, showed something else. before: INFO before BEGIN."""
    if link.front < KEEPS_WRITE:
        return 'info'
    if link.front >= ACTS_FRONT:       # INFO keeps the refusal that follows a write
        return 'refused' if i['refused'] or (i['phase'] == FAILED and i['way'] == WAY_NONE) \
            else 'info'
    if i['way'] != way or (i['phase'] == DONE and i['readback'] != sha):
        return 'refused'
    if i['phase'] == DONE:
        return 'info'
    state = (i['phase'], i['result'], i['way'])
    if before is not None and before['phase'] in (IDLE, DONE, FAILED) and \
            (before['phase'], before['result'], before['way']) != state:
        return 'info'
    return 'unknown'


def made_words(i):
    """The earlier write a refusal leaves in INFO."""
    if i['phase'] == DONE and i['way'] in DONE_WORDS:
        return DONE_WORDS[i['way']] % slot_name(i['written'])
    if i['phase'] == DONE:
        return '%s written and read back' % slot_name(i['written'])
    return '%s that failed: %s' % ('an action' if i['act'] else 'a write',
                                   status_text(i['result'], hint=False))


COUNTDOWN_LINE = ('The unit counts down %d s before its first erase. Switching the unit off or '
                  'unplugging USB in that time stops it, with the flash untouched; stopping this tool '
                  'does not.' % COUNTDOWN)


def follow(link, way, out, poll, deadline, before=None):
    """After an accepted END or ACT: from front 11 the countdown, then the write or the action,
    until it ends; in the damaged state the unit begins at once. (None, INFO at the end, whether a
    countdown was seen) or (exit status, None, that). before: INFO from before the request."""
    end = time.monotonic() + deadline + COUNTDOWN
    seen, left = None, None
    try:
        while True:
            i = get_info(link)
            ph = i['phase']
            if seen is None and ph != COUNTING and v11(i) and before is not None and before['held']:
                out('The unit began at once, with no countdown: it holds the power-off while a write '
                    'of this recovery has left no checked slot to start.')
            if ph == COUNTING:
                if seen != COUNTING:
                    out(COUNTDOWN_LINE)
                if i['count'] != left:
                    left = i['count']
                    out('\rCountdown: %2d s left' % left, end='')
            elif ph not in (WRITING, DONE, FAILED):
                out('')
                out('The unit is not writing (phase %d); run info.' % ph)
                return 1, None, left is not None
            elif seen == COUNTING:
                out('')                 # the countdown's line ends
            if ph == WRITING:
                progress(out, 'Writing', i['progress'], 1000)
            seen = ph
            if ph in (DONE, FAILED):
                break
            if time.monotonic() > end:
                out('')
                out('The write did not finish in %d s; run info.' % (deadline + COUNTDOWN))
                return 1, None, left is not None
            time.sleep(poll)
    except Failure as e:
        if seen == COUNTING:
            raise Failure('%s. The unit was counting down: if it was switched off or USB was '
                          'unplugged, it stopped, and nothing was written (status 24); else it goes '
                          'on when the countdown ends%s. Run info once the unit answers again'
                          % (e, ', in place: then do not switch the unit off or pull the cable'
                             if way == WAY_IN_PLACE else ''))
        if way == WAY_IN_PLACE:
            raise Failure('%s. The in-place write may still be running: do not switch the unit off '
                          'or pull the cable; wait, then run info' % e)
        raise
    counted = left is not None
    if counted and ph == FAILED and i['result'] == 24:
        out('Stopped: %s' % status_text(24, i=i))
        return 1, None, counted
    if counted and i['refused'] == 24:
        # S-02.093: a stop leaves the write this recovery made before in INFO, and says so
        out('Stopped: %s The unit still reports the write it made earlier in this recovery (%s); '
            'nothing of this one was written.' % (status_text(24, i=i), made_words(i)))
        return 1, None, counted
    return None, i, counted


def transfer(link, data, i, way, out, poll, deadline, before=None):
    """DATA, END and the write's progress: (None, INFO at the end) or (exit status, None).
    way is the write BEGIN announced, before INFO from before BEGIN."""
    off = 0
    while off < len(data):
        chunk = data[off:off + link.max_data]
        try:
            r = link.request(DATA, struct.pack('<I', off) + chunk)[0]
        except Failure as e:
            # S-02.084: a transfer the host left ends with nothing written (status 24)
            said = str(e) if str(e).endswith(('.', '?')) else str(e) + '.'
            raise Failure('%s The transfer stopped at byte %d of %d, before END: nothing was '
                          'written. Once the unit is back, run the command again: it begins anew.'
                          % (said, off, len(data)))
        r = sized(r, DATA_REPLY_LEN, 'DATA')
        st, nxt = r[0], struct.unpack_from('<I', r, 1)[0]
        if st == 0x42 and nxt <= len(data):    # out of step: go on where the unit is
            off = nxt
            continue
        if st:
            out('')
            out('Refused at byte %d: %s' % (off, status_text(st)))
            return 1, None
        off = nxt
        progress(out, 'Sending', off, len(data))
    out('')
    r, attempt = link.request(END)
    st, image_rc = sized(r, END_REPLY_LEN, 'END')[:2]
    lost = st == 0x41 and attempt > 1           # taken and ended, or refused: end_lost()
    if st and attempt > 1 and st in (1, 0x41):
        st = None                               # an earlier END got through
    if st:
        out('Refused: %s' % status_text(st, image_rc, i=i))
        if st == 18:
            out('Ways out: ' + ways_out(i))
        return 1, None
    rc, i, counted = follow(link, way, out, poll, deadline, before)
    if rc is not None:
        return rc, None
    # a countdown seen: END was taken, whatever its lost reply said
    how = end_lost(link, before, i, way, hashlib.sha256(data).digest()) \
        if lost and not counted else 'info'
    if how == 'refused':
        out('')
        if i['refused']:            # front 11: INFO keeps the reason
            out('Refused: %s' % status_text(i['refused'], i=i))
            out("END's reply was lost; the unit still reports the write made earlier in this "
                'recovery (%s). Nothing of this file was written.' % made_words(i))
            if i['refused'] == 18:
                out('Ways out: ' + ways_out(i))
        elif i['phase'] == FAILED and i['way'] == WAY_NONE:     # no earlier write: INFO has it
            out('Refused: %s' % status_text(i['result'], i['image_rc'], i=i))
            if i['result'] == 18:
                out('Ways out: ' + ways_out(i))
        else:
            out("Refused: the unit did not take this write. END's reply was lost, and the unit "
                'still reports the write made earlier in this recovery (%s), as a refused END '
                'leaves it (S-02.093); the reason was only in the lost reply. Nothing of this file '
                'was written. Run info, then write again.' % made_words(i))
        return 1, None
    if how == 'unknown':
        out('')
        out("END's reply was lost, and the unit reports a failed write with the way this one was "
            'announced: %s. That is either a write made earlier in this recovery, which a refused '
            'END leaves in place (S-02.093), or this write, failed: what the unit reports cannot '
            'tell the two apart. Run info to see the slots and the selector before writing again.'
            % status_text(i['result'], hint=False))
        for line in retry_lines(i):
            out(line)
        return 1, None
    progress(out, 'Writing', 1000, 1000)
    out('')
    if i['phase'] == FAILED:
        out('FAILED: %s' % status_text(i['result'], i['image_rc'], hint=plain_hint(i), i=i))
        for line in (tried_lines(i, True) if way == WAY_TRIAL else []) + retry_lines(i):
            out(line)
        return 1, None
    return None, i


def unverified(out, k, checks, tip):
    bad = [n for n, ok in checks if not ok]
    if bad:
        out('NOT VERIFIED: what the unit reports of %s does not match: %s. %s'
            % (slot_name(k), ', '.join(bad), tip))
    return bool(bad)


def trial_done(out, i, data, sha, crc):
    """After a trial write: the slot tried, its record marked, the selector as S-02.084 leaves it."""
    k = i['written']
    s = i['slots'][k] if k in (0, 1) else None
    want = (0x00, 0xFE) if k == 0 else (0xFF, 0xFF)
    checks = [('the read-back SHA-256', i['readback'] == sha),
              ("the slot's record length", s is not None and s['length'] == len(data)),
              ("the slot's record CRC-32", s is not None and s['crc'] in (crc, crc ^ 0xFFFFFFFF)),
              ('the slot is tried', s is not None and s['tried']),
              ('the selector, which should read %02x %02x' % want,
               i['selector_ok'] and (i['sel_index'], i['sel_verified']) == want)]
    if unverified(out, k, checks, 'Run info first: it says whether a trial start of %s would come. '
                  'Do not select it; write --new-front again if info does not show it tried.'
                  % slot_name(k)):
        return 1
    a, m = slot_name(1 - k), slot_name(k)
    out('Verified: %s holds this file for one trial start (%d bytes, SHA-256 %s), read back by the '
        'unit; the selector reads %02x %02x.' % (m, len(data), sha.hex(), want[0], want[1]))
    out('Next: restart the unit (wbrecover.py restart) with no key held and let it come up: that '
        'start tries %s once and records itself in %s. If it does not come up, switch it off and '
        'on: %s boots again.' % (m, m, a))
    sel = 'keep (or select IMAGE)' if v11(i) else 'select IMAGE'
    out("To keep the new front, select it: after that start, switch the unit off and on and run "
        "wbrecover.py %s from %s's recovery (Power + M4). Or try holding key 4 (M4) "
        "through the restart (not yet proven on a unit): if the new front's recovery opens, run "
        'wbrecover.py %s there. A trial start into recovery that is not selected there '
        'records nothing: the trial write must then be repeated.' % (sel, a, sel))
    return 0


# What --force lifts for a write (S-02.084), and what could go wrong.
WRITE_RISK = {
    3: ('The selector does not name the running slot. Forced, the write goes to the running slot, '
        'the one the selector does not name; the unit takes it only while the boot slot is '
        'checked, which a cut of the write then starts.'),
    4: ('The selector is not settled: its verified byte is not 01, so the next start was to check '
        'it. Forced, the write goes on over it: a cut can leave a selector that leads to no good '
        'slot.'),
    5: ("The boot slot failed its check, and the running slot's front is not intact in the flash. Forced, "
        'no front is asked of the image: the unit can end on an image whose recovery is not this one, '
        'or none.'),
    12: ("The running slot's front is not intact in the flash. Forced, no front is asked of the "
         'image: the unit can end on an image whose recovery is not this one, or none.'),
    15: ('The other slot failed its check and no write of it has failed here. Forced, the running slot is '
         'erased and written in place at once: a cut leaves a unit that cannot be repaired without a '
         'flash programmer. A plain write of the other slot first is safe (wbrecover.py write IMAGE).'),
    19: ('The running slot is not checked. Forced, the trial goes on: if the new front does not come up, '
         'switching off and on starts the running slot, which may not start either.'),
    'front': ("The file's front is not the one this unit runs. Forced, it is written and selected for "
              'good, with no trial: if that front does not start, recovery may not be reachable. A '
              'trial is safer: write --new-front IMAGE starts it once first.'),
    'none': ('The file carries no waffle-boot front. Forced, it is written and selected: from then on '
             'the unit has no recovery (Power + M4 does nothing), and nothing on the unit brings '
             'waffle-boot back.'),
}


def ask_force(out, ask, name, lines):
    """The warning before a forced request, and its typed phrase: whether to go on."""
    out('With --force the unit lifts the refusals that protect it:')
    for line in lines:
        out(line)
    phrase = FORCE_PHRASE % name
    return ask('Type "%s" to go on: ' % phrase).strip() == phrase


def not_needed(out, name):
    out('--force is not needed: the unit takes this %s without it, so it is sent without the flag.'
        % name)


def cmd_write(link, data, out=print, poll=0.2, deadline=900.0, running=False, ask=ask_tty,
              new_front=False, pins=signed_pins, force=False):
    """ask(prompt) returns what the user typed. On a version 6 front the confirmation is on
    BEGIN's answer: the way and the slot the unit will write. pins() gives the pins this
    checkout has signed (signed_pins()). force (front 11): the refusals that protect the unit are
    lifted, after a warning and a typed phrase before BEGIN."""
    sha = hashlib.sha256(data).digest()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    probs, front = check_image(data)
    if probs:
        # only --force takes a bootable stream with no waffle-boot front (S-02.084)
        if not force or not all(p.startswith('no waffle-boot front') for p in probs):
            for line in probs:
                out('  ' + line)
            out('The file is not a slot image recovery writes; nothing was sent.')
            return 1
        front = None
    if front is not None and not front['pin_ok']:
        return pin_refusal(out, front)
    h = hello(link)
    if force and link.front < ACTS_FRONT:
        return too_old(out, link, '--force')
    i = get_info(link)
    if running and not i['v6']:
        out('This unit runs front version %d, which writes only the slot the selector does not '
            'name: --running needs a version 6 front. Nothing was sent.' % h['front_version'])
        return 1
    for line in retry_lines(i):
        out(line)
    if new_front and front is None:
        out('Refused before sending: --new-front writes another waffle-boot front for one trial '
            'start, and this file carries none. Nothing was sent.')
        return 1
    # S-02.084: the front the unit runs, by the pin it reports; the same front needs no flag
    if new_front and not i['own_desc']:
        out("Refused before sending: the unit does not report the front it runs (the running "
            "image's descriptor is not valid), so the tool cannot tell whether the file's front "
            'differs; and the unit takes a trial only with the running slot checked. Run info. '
            'Nothing was sent.')
        return 1
    other = front is not None and i['own_desc'] and front['pin'] != i['pin']
    trial = other and not running
    # INFO's prediction, to refuse early; the unit decides at BEGIN and again at END.
    if running:
        way, refusal = i['run_way'], i['run_refusal']
        target = i['running'] if way != WAY_NONE else NO_SLOT
    else:
        way, target, refusal = WAY_TRIAL if trial else WAY_PLAIN, i['target'], i['refusal']
    risks = []
    if force:
        if front is None:
            risks.append(WRITE_RISK['none'])
        elif other and not new_front:
            risks.append(WRITE_RISK['front'])
            trial = False
            way = WAY_PLAIN if not running else way
        if refusal == 3 and not i['slots'][1 - i['running']]['good']:
            # S-02.084: no write of the running slot under a selector that names a bad slot
            out('Refused before sending, even with --force: %s, and that slot failed its check (status 5). '
                'Nothing was sent.' % STATUS[3][0])
            return 1
        if target not in (0, 1) and refusal in (3, 4, 5, 12, 15):
            risks.append(WRITE_RISK[refusal])
            target = None               # BEGIN's answer says where it goes
        if trial and not i['slots'][i['running']]['good']:
            risks.append(WRITE_RISK[19])
        if not risks:
            not_needed(out, 'write')
            force = False
    if other and not force and (not new_front or link.front < TRIAL_FRONT):
        return other_front_refusal(out, link, i, front, new_front)
    if target is not None and target not in (0, 1):
        return refused(out, refusal, i, True)
    signed = signed_words(front['pin'], pins) if trial else None
    if trial and signed[0] == 'signed' and signed[1] is not None and signed[1] < TRIAL_FRONT:
        out('Refused before sending: the file\'s front is version %d (a commit of this checkout set '
            'its pin at that version). A front older than version %d leaves no mark in its trial '
            'start and has no select of its own, so its trial could never be kept, and %s\'s image '
            'would be lost for nothing. Put an older front on with the test OS installer '
            '(wiki/Test-OS.md). Nothing was sent.'
            % (signed[1], TRIAL_FRONT, slot_name(1 - i['running'])))
        return 1
    if trial and (damaged(i) or i['moved'] or
                  (not i['slots'][i['running']]['good'] and not force)):
        out('Refused before sending: %s' % status_text(19, i=i))
        return 1
    if target is not None and only_select(i, target) and \
            (i['slots'][target]['length'], i['slots'][target]['crc']) != (len(data), crc):
        return only_good_refusal(out, i, target)
    why = refusal_before(i, data, target, way, force) if target is not None else 0
    if why == 13 and leads_to_other(i):
        out('Refused before sending: %s already holds this image and the selector leads to it, so '
            'there is nothing to do with it (status 13). Write %s again.'
            % (slot_name(1 - target), slot_name(target)))
        return 1
    if why:
        out('Refused before sending: %s' % status_text(why, i=i))
        return 1
    for k, v in same_version(i, data):
        out('Warning: %s holds a different image with the same version %s; '
            'make bump-version gives the next one.' % (slot_name(k), version(v)))
    flags = (BEGIN_F_TRIAL if trial else BEGIN_F_RUNNING if running else 0) | \
        (BEGIN_F_FORCE if force else 0)
    begin = struct.pack('<I', len(data)) + sha + (bytes([flags]) if flags else b'')
    if force and slot1_window(i):
        out(FORCE_AFTER_SLOT1)
        return 1
    if force and not ask_force(out, ask, 'write', risks):
        out('Stopped: nothing was sent, and nothing was written.')
        return 1
    before = i
    if i['v6']:
        r = sized(link.request(BEGIN, begin)[0], BEGIN_REPLY_LEN, 'BEGIN')
        if r[0]:
            return refused(out, r[0], i, False)
        way, target = r[1], r[2]
        ways = (WAY_TRIAL,) if trial else (WAY_PLAIN, WAY_SELECT_FIRST, WAY_IN_PLACE)
        if way not in ways or target not in (0, 1) or (trial and target == i['running']):
            raise Failure("BEGIN's reply announces way %d to slot index %d, not the %s the tool "
                          'asked for. Nothing was written: no END was sent. Likely the unit\'s '
                          'state changed, for instance this recovery already wrote the selector or '
                          'began a write; run info'
                          % (way, target, 'trial write of the other slot' if trial else 'write'))
        i = get_info(link)      # the state now; for an in-place write, the battery it asked
        why = refusal_before(i, data, target, way, force)
        if why:
            out('Refused: %s. Nothing was sent beyond BEGIN.' % status_text(why, i=i))
            return 1
    settle = damaged(i)         # S-02.084: the write settles the selector on its target
    out('Writing %d bytes to %s%s.' % (len(data), slot_name(target), {
        WAY_SELECT_FIRST: ', the running slot, after the selector names %s' % slot_name(1 - target),
        WAY_IN_PLACE: ', the running slot, in place',
        WAY_TRIAL: ', for one trial start'}.get(way, '')))
    out(SETTINGS_LINE)
    if way == WAY_TRIAL:
        for line in trial_lines(h, i, front, target, signed) + countdown_note(i):
            out(line)
        if ask('Type "%s" to go on: ' % TRIAL_PHRASE).strip() != TRIAL_PHRASE:
            out('Stopped: no image byte was sent, no END, and nothing was written.')
            return 1
    else:
        # a version 5 front: a warning only when a good image is replaced, as before
        lines = confirm_lines(i, way, target) if i['v6'] or i['slots'][target]['good'] else []
        for line in lines + (countdown_note(i) if lines else []):
            out(line)
        if way == WAY_IN_PLACE:
            if ask('Type "%s" to go on: ' % PHRASE).strip() != PHRASE:
                out('Stopped: no image byte was sent, and nothing was written.')
                return 1
        elif lines and ask('Type yes to go on: ').strip() != 'yes':
            out('Stopped: no image byte was sent, and nothing was written.')
            return 1
    if not i['v6']:
        st = sized(link.request(BEGIN, begin)[0], 1, 'BEGIN')[0]
        if st:
            out('Refused: %s' % status_text(st, i=i))
            return 1
    rc, i = transfer(link, data, i, way, out, poll, deadline, before)
    if rc is not None:
        return rc
    if way == WAY_TRIAL:
        return trial_done(out, i, data, sha, crc)
    k = i['written']
    s = i['slots'][k] if k in (0, 1) else None
    # S-02.084: an in-place write, and every write in the damaged state, ends with the selector
    # settled on its target, {k, 01}; where it names slot 1 already, it is left as it is
    settled = settle or i['way'] == WAY_IN_PLACE
    if settled:
        sel = ('the selector, which should name %s%s' % (slot_name(k), '' if k == 1 else
                                                          ' with verified 01'),
               i['selector_ok'] and selected(i['sel_index']) == k and
               (i['sel_verified'] == 1 or k == 1))
    else:           # S-02.048
        sel = ('the selector, which should read %02x 00' % k,
               i['selector_ok'] and (i['sel_index'], i['sel_verified']) == (k, 0))
    checks = [('the read-back SHA-256', i['readback'] == sha),
              ("the slot's record length", s is not None and s['length'] == len(data)),
              ("the slot's record CRC-32", s is not None and s['crc'] == crc),
              ('the slot is checked', s is not None and s['good']), sel]
    if unverified(out, k, checks,
                  'Run info first, and write again as it says.' if settle else SAFE_HINT):
        return 1
    out('Verified: %s holds this file (%d bytes, SHA-256 %s), read back by the unit.'
        % (slot_name(k), len(data), sha.hex()))
    if settled and i['sel_verified'] == 1:
        out('The selector names %s with verified 01: the next start takes it as it is. Restart '
            'the unit (wbrecover.py restart) without holding a key.' % slot_name(k))
    elif settled:
        out('The selector names %s (verified %02x): the next start checks it and settles it. '
            'Restart the unit (wbrecover.py restart) without holding a key.'
            % (slot_name(k), i['sel_verified']))
    else:
        out('The selector now names %s, to be checked at the next start. Restart the unit '
            '(wbrecover.py restart) without holding a key.' % slot_name(k))
    return 0


def select_lines(i, k):
    """What a select of slot k does, before its yes."""
    m, other = slot_name(k), slot_name(1 - k)
    if trial_start(i):
        lines = ['This is the trial start of %s, the new front, and it came up. The select makes %s '
                 'the boot slot: its record is made good and the selector names it; its image '
                 'is not written again, and %s is kept as it is.' % (m, m, other)]
        if i['why'] == 2:
            lines.append('WARNING: recovery runs because the image check failed: the new front runs, '
                         'but its image does not pass its own check. Selecting it leaves a unit that '
                         'starts only into recovery.')
        return lines
    return ['%s holds the new front, tried, and the unit recorded the new front\'s start in its '
            'trial. The select makes %s the boot slot: its record is made good and the selector '
            'names it; its image is not written again, and %s is kept as it is.' % (m, m, other)]


SELECT_RISK = {
    19: ('This recovery began a trial write. Forced, the select comes before that trial start has '
         'run, so nothing shows that the new front comes up.'),
    21: ("The tried slot's front has left no mark of a trial start: nothing shows that it comes up. "
         'Forced, it is selected all the same: if it does not start, recovery may not be reachable.'),
}


def cmd_select(link, data, out=print, poll=0.2, deadline=900.0, ask=ask_tty, force=False):
    """select: the image sent again with BEGIN flag bit 2, to select the slot tried with it."""
    sha = hashlib.sha256(data).digest()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    front = check_image(data)[1]
    if front is not None and not front['pin_ok']:
        return pin_refusal(out, front)
    h = hello(link)
    if force and link.front < ACTS_FRONT:
        return too_old(out, link, '--force')
    i = get_info(link)
    if link.front < TRIAL_FRONT:
        out('This unit runs front version %d: select needs a version %d front. Nothing was sent.'
            % (h['front_version'], TRIAL_FRONT))
        return 1
    for line in retry_lines(i):
        out(line)
    a = i['running']
    if not i['established'] or a not in (0, 1):
        return refused(out, i['refusal'], i, True)
    here = trial_start(i)
    if i['slots'][a]['tried'] and not here:
        out('Refused before sending: %s Nothing was sent.' % ' '.join(tried_lines(i)))
        return 1
    k = a if here else 1 - a
    s = i['slots'][k]
    if not here and not s['tried']:
        out('Refused before sending: no slot is tried, so there is nothing to select. A trial write '
            'comes first: write --new-front IMAGE, then a restart for its trial start. Nothing was '
            'sent.')
        return 1
    if not here and i['target'] not in (0, 1):
        lines = [refusal_text(i['refusal'], i).rstrip('.') + '.'] + tried_lines(i)
        sel = sel_known(i)
        if sel is not None and sel[0] == k and sel[1] != 1:
            lines.append('Restart the unit first, with no key held, for that trial start; then switch '
                         'off and on and select here. Or try holding key 4 through the restart and '
                         'select in its recovery.')
        out('Refused before sending: %s Nothing was sent.' % ' '.join(lines))
        return 1
    risks = []
    if force:
        if not here and not s['started']:
            risks.append(SELECT_RISK[21])
        if trial_begun(i):
            risks.append(SELECT_RISK[19])
        if not risks:
            not_needed(out, 'select')
            force = False
    if not here and not s['started'] and not force:   # S-02.084: the new front's mark of its trial start
        out('Refused before sending: %s is tried, but its front has left no mark of a trial start, '
            'so a select is refused. %s Nothing was sent.' % (slot_name(k), NOT_STARTED_HINT))
        return 1
    if s['length'] != len(data) or s['crc'] not in (crc, crc ^ 0xFFFFFFFF):
        out('Refused before sending: %s. %s holds %d bytes as tried, with another CRC-32 than this '
            'file: send the file written with --new-front. Nothing was sent.'
            % (STATUS[20][0], slot_name(k), s['length']))
        return 1
    begin = struct.pack('<I', len(data)) + sha + bytes([BEGIN_F_SELECT | (BEGIN_F_FORCE if force else 0)])
    if force and slot1_window(i):
        out(FORCE_AFTER_SLOT1)
        return 1
    if force and not ask_force(out, ask, 'select', risks):
        out('Stopped: nothing was sent, and nothing was written.')
        return 1
    before = i
    r = sized(link.request(BEGIN, begin)[0], BEGIN_REPLY_LEN, 'BEGIN')
    if r[0]:
        return refused(out, r[0], i, False)
    if r[1] != WAY_SELECT or r[2] != k:
        raise Failure("BEGIN's reply announces way %d to slot index %d, not a select of %s. Nothing "
                      "was written: no END was sent. Likely the unit's state changed since info; "
                      'run info' % (r[1], r[2], slot_name(k)))
    i = get_info(link)
    out('Selecting %s: sending its image again (%d bytes).' % (slot_name(k), len(data)))
    for line in select_lines(i, k) + countdown_note(i):
        out(line)
    if ask('Type yes to go on: ').strip() != 'yes':
        out('Stopped: no image byte was sent, no END, and nothing was written.')
        return 1
    rc, i = transfer(link, data, i, WAY_SELECT, out, poll, deadline, before)
    if rc is not None:
        return rc
    k = i['written']
    s = i['slots'][k] if k in (0, 1) else None
    checks = [('the read-back SHA-256', i['readback'] == sha),
              ("the slot's record length", s is not None and s['length'] == len(data)),
              ("the slot's record CRC-32", s is not None and s['crc'] == crc),
              ('the slot is checked', s is not None and s['good'] and not s['tried']),
              ('the selector, which should read %02x 00' % k,
               i['selector_ok'] and (i['sel_index'], i['sel_verified']) == (k, 0))]
    if unverified(out, k, checks, SAFE_HINT):
        return 1
    out('Selected: %s holds this file (%d bytes, SHA-256 %s), read back by the unit, and the '
        'selector names it, to be checked at the next start. Restart the unit (wbrecover.py '
        'restart) without holding a key.' % (slot_name(k), len(data), sha.hex()))
    return 0


def refusal_before(i, data, target, way=WAY_PLAIN, force=False):
    """What the unit would refuse in data at END, judged from INFO (S-02.084): 12 when its
    front is not the running slot's (not for a trial, and not forced); for a front before
    version 14, 13 when the slot that is kept already holds it, or the target is the good
    running slot and holds it. The unit checks again on the slots' bytes."""
    if not i['selector_ok'] or not i['established'] or i['running'] not in (0, 1):
        return 0
    a = i['slots'][i['running']]
    front = check_image(data)[1]
    if way == WAY_TRIAL or force:
        pass                    # another front, by trial or forced: no front rule
    elif a['has_front']:
        if front is None or (front['pin'], front['front_crc']) != (a['pin'], a['front_crc']):
            return 12
    elif i['own_desc']:       # a retry: the front the running slot had
        if front is None or front['pin'] != i['pin']:
            return 12
    if i.get('front', 0) >= SAME_FRONT:
        return 0                # both slots may hold the same image
    mine = (len(data), zlib.crc32(data) & 0xFFFFFFFF)
    kept = i['slots'][1 - target]
    if (kept['length'], kept['crc']) == mine:
        return 13
    # not in the damaged state, where the good running slot's own image settles the selector
    if target == i['running'] and a['good'] and (a['length'], a['crc']) == mine and not damaged(i):
        return 13
    return 0


def same_version(i, data):
    """(slot, version) for each slot that carries the file's descriptor version but holds
    other bytes."""
    front = check_image(data)[1]
    if front is None:
        return []
    mine = (len(data), zlib.crc32(data) & 0xFFFFFFFF)
    return [(k, s['version']) for k, s in enumerate(i['slots'])
            if s['has_desc'] and tuple(s['version']) == tuple(front['version'])
            and (s['length'], s['crc']) != mine]


RESTART_RISK = ('Forced, the unit restarts all the same: it may not start at all, and then recovery '
                'is not reachable either; there may be no way back without a flash programmer. A '
                'write first settles the selector (the ways out above).')


def cmd_restart(link, out=print, ask=ask_tty, force=False):
    hello(link)
    if force and link.front < ACTS_FRONT:
        return too_old(out, link, '--force')
    i = get_info(link)
    if i['read_error']:
        out(READ_ERROR)
    flags = b''
    if blocked(i) and force:
        lines = ['Without --force the unit refuses this: %s.' % STATUS[0x44][0], state_line(i)] + \
            held_lines(i) + ['Ways out: ' + ways_out(i), RESTART_RISK]
        if not ask_force(out, ask, 'restart', lines):
            out('Stopped: no restart was sent.')
            return 1
        flags = bytes([BEGIN_F_FORCE])
    elif blocked(i):
        out('Not restarted: %s.' % STATUS[0x44][0])
        for line in [state_line(i)] + held_lines(i):
            out(line)
        out('Ways out: ' + ways_out(i))
        return 1
    elif force:
        not_needed(out, 'restart')
    if damaged(i) and not flags:
        out('%s But %s.' % (state_line(i), reach_words(i)))
    try:
        r = link.request(RESTART, flags)[0]
    except Failure:
        if not link.sent:
            raise
        # the unit resets once its reply is out: a reply lost after the request went out most
        # likely means it restarted
        out('The restart request went out, but no reply came: the unit most likely restarted. '
            'Check that it starts; if it is still in recovery, run restart again.')
        return 0
    r = sized(r, 1, 'RESTART')
    if r[0]:
        out('Refused: %s' % status_text(r[0], i=i))
        if r[0] == 0x44:
            out('Ways out: ' + ways_out(i))
        return 1
    out('The unit restarts.')
    return 0


def act_confirm_lines(i, act, k):
    """What the action does to slot k, before its confirmation."""
    if act == ACT_TRY:
        return try_lines(i, k)
    if act == ACT_SWITCH:
        return switch_lines(i, k)
    if act == ACT_KEEP:
        return keep_lines(i, k)
    return put_back_lines(i, k)


def act_checks(act, before, i, k):
    """What INFO must show after an action on slot k ended well (S-02.084)."""
    s = i['slots'][k] if k in (0, 1) else None
    was = before['slots'][k] if k in (0, 1) else None
    sel = (i['sel_index'], i['sel_verified']) if i['selector_ok'] else None
    settled = damaged(before)
    if act == ACT_TRY:
        want = (0x00, 0xFE) if k == 0 else (0xFF, 0xFF)
        checks = [('the selector, which should read %02x %02x' % want, sel == want)]
        if was is not None and (was['good'] or was['tried']):
            checks.append(('the slot is tried', s is not None and s['tried']))
        return checks
    if act == ACT_PUT_BACK:
        checks = [('the slot is checked and not tried', s is not None and s['good'] and not s['tried'])]
        old = sel_known(before)
        if old is not None and old[0] == k and old[1] != 1 and not settled:
            checks.append(('the selector, which should name %s again' % slot_name(1 - k),
                           sel is not None and selected(sel[0]) == 1 - k))
        return checks
    checks = []
    if act == ACT_KEEP or was['good'] or was['tried']:
        checks.append(('the slot is checked and not tried', s is not None and s['good'] and not s['tried']))
    if settled or (act == ACT_SWITCH and not was['good'] and not was['tried']):
        # the selector settled on k, or a bad slot named with verified 01
        checks.append(('the selector, which should name %s with verified 01' % slot_name(k),
                       sel is not None and selected(sel[0]) == k and (sel[1] == 1 or k == 1 and settled)))
    else:
        checks.append(('the selector, which should read %02x 00' % k, sel == (k, 0)))
    return checks


def act_done(out, act, before, i, k):
    """After an action: verified from INFO, and what comes next."""
    K, other = slot_name(k), slot_name(1 - k)
    if i['phase'] == FAILED:
        out('FAILED: %s' % status_text(i['result'], hint=plain_hint(i), i=i))
        for line in tried_lines(i) + retry_lines(i):
            out(line)
        return 1
    if unverified(out, k, act_checks(act, before, i, k), 'Run info first.'):
        return 1
    if act == ACT_TRY:
        out('Done: %s is marked for one trial start; the selector reads %02x %02x.'
            % (K, i['sel_index'], i['sel_verified']))
        out('Next: restart the unit (wbrecover.py restart) with no key held: that start runs %s '
            'once. If it does not come up, switch it off and on: %s boots again. To keep %s once it '
            'has come up: wbrecover.py keep, from its own recovery in that start or from %s\'s '
            'after it, %s.' % (K, other, K, other, SLOT_MENU))
    elif act == ACT_SWITCH:
        out('Done: %s is the boot slot (the selector reads %02x %02x). Restart the unit '
            '(wbrecover.py restart) without holding a key: it starts %s.'
            % (K, i['sel_index'], i['sel_verified'], K))
    elif act == ACT_KEEP:
        out('Kept: %s is the boot slot, to be checked again at the next start. Restart the unit '
            '(wbrecover.py restart) without holding a key.' % K)
    else:
        out("Put back: %s's record is good again and it is no longer tried; the selector reads "
            '%02x %02x.' % (K, i['sel_index'], i['sel_verified']))
    return 0


def cmd_act(link, act, out=print, poll=0.2, deadline=900.0, ask=ask_tty, force=False):
    """try, switch, keep, put-back (front 11): ACT on the image a slot holds, with no image sent.
    The unit's own answer from INFO decides whether --force is needed, and whether it helps."""
    name = ACT_NAMES[act]
    hello(link)
    if link.front < ACTS_FRONT:
        return too_old(out, link, name)
    i = get_info(link)
    for line in retry_lines(i):
        out(line)
    if other_of(i) == NO_SLOT:
        out('Refused before sending: %s Nothing was sent.' % (status_text(2, i=i)))
        return 1
    un, fo = i['acts'][act - 1], i['forced'][act - 1]
    k = act_slot(i, act)
    flag = 0
    if un == 0 and force:
        not_needed(out, name)
    elif un and not force:
        line = 'Refused before sending: %s.' % act_text(act, un, i).rstrip('.')
        if fo == 0:
            line += ' --force lifts this refusal, after a warning.'
        out(line + ' Nothing was sent.')
        return 1
    elif un and fo:
        out('Refused before sending, even with --force: %s. Nothing was sent.'
            % act_text(act, fo, i).rstrip('.'))
        return 1
    elif un:
        flag = BEGIN_F_FORCE
    if k not in (0, 1):
        raise Failure('INFO names no slot for the %s; run info' % name)
    out({ACT_TRY: 'Trying %s once.', ACT_SWITCH: 'Making %s the boot slot.',
         ACT_KEEP: 'Keeping %s.', ACT_PUT_BACK: 'Putting %s back.'}[act] % slot_name(k))
    lines = act_confirm_lines(i, act, k) + countdown_note(i)
    if flag and slot1_window(i):
        out(FORCE_AFTER_SLOT1)
        return 1
    if flag:
        if not ask_force(out, ask, name, force_lines(act, un, i) + lines):
            out('Stopped: nothing was sent, and nothing was written.')
            return 1
    else:
        for line in lines:
            out(line)
        # a switch is for good: a typed phrase, as for every dangerous request
        phrase = SWITCH_PHRASE if act == ACT_SWITCH else 'yes'
        if ask('Type "%s" to go on: ' % phrase if act == ACT_SWITCH else
               'Type yes to go on: ').strip() != phrase:
            out('Stopped: nothing was sent, and nothing was written.')
            return 1
    r, attempt = link.request(ACT, bytes([act, flag]))
    r = sized(r, ACT_REPLY_LEN, 'ACT')
    st, way, slot = r[0], r[1], r[2]
    if st == 1 and attempt > 1:
        way, slot = ACT_WAYS[act], k        # an earlier ACT got through
    elif st:
        out('Refused: %s' % act_text(act, st, i))
        return 1
    if way != ACT_WAYS[act] or slot != k:
        out('The unit took the %s as way %d to slot index %d, not as the tool asked. %s'
            % (name, way, slot, 'Run info.' if i['held'] else
               'It counts down before it writes: switch the unit off or unplug USB now to stop it '
               'with the flash untouched; then run info.'))
        return 1
    rc, after, _ = follow(link, way, out, poll, deadline, i)
    if rc is not None:
        return rc
    progress(out, 'Writing', 1000, 1000)
    out('')
    return act_done(out, act, i, after, k)


# S-02.097: a recovery write leaves the slot's settings sector as it is; the settings there
# were saved for the image they name, so another image starts on the defaults.
SETTINGS_LINE = "The slot's settings start at their defaults unless it held this same image."


def cmd_check(data, out=print):
    probs, front = check_image(data)
    for p in probs:
        out('  ' + p)
    if probs:
        out('The file is not a slot image recovery writes.')
        return 1
    if not front['pin_ok']:
        pin_refusal(lambda line: out(line.replace('Refused before sending: ', '  ')
                                     .replace(' Nothing was sent.', '')), front)
        out('The file is not a slot image recovery writes.')
        return 1
    out('OK: %d bytes, a bootable stream with a waffle-boot front; version %s, front pin %s.'
        % (len(data), version(front['version']), front['pin'].hex()))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--port')
    sub = ap.add_subparsers(dest='cmd', required=True)
    force = dict(action='store_true', help='have the unit lift the refusals that protect it (front '
                 'version 11 or later), after a warning and a typed phrase')
    sub.add_parser('info')
    w = sub.add_parser('write')
    how = w.add_mutually_exclusive_group()
    how.add_argument('--running', action='store_true',
                     help='write the running slot instead of the one the selector does not name')
    how.add_argument('--new-front', action='store_true',
                     help='take an image with another front, written for one trial start')
    w.add_argument('--force', **force)
    w.add_argument('image')
    s = sub.add_parser('select')
    s.add_argument('--force', **force)
    s.add_argument('image')
    for name, words in (('try', 'start the other slot once, as it lies (front version 11 or later)'),
                        ('switch', 'make the other slot the boot slot; after its trial, keep (front version 11 or '
                         'later)'),
                        ('keep', 'keep a tried slot, with no image sent (front version 11 or later)')):
        sub.add_parser(name, help=words).add_argument('--force', **force)
    sub.add_parser('put-back', help="make the tried other slot's record good again (front version "
                   '11 or later)')
    sub.add_parser('restart').add_argument('--force', **force)
    c = sub.add_parser('check')
    c.add_argument('image')
    a = ap.parse_args(argv)
    acts = {v: k for k, v in ACT_NAMES.items()}
    try:
        if a.cmd in ('write', 'select', 'check'):
            with open(a.image, 'rb') as f:
                data = f.read()
            if a.cmd == 'check':
                return cmd_check(data)
            probs, front = check_image(data)
            # write --force takes a bootable stream with no waffle-boot front (cmd_write())
            nofront = probs and all(p.startswith('no waffle-boot front') for p in probs)
            if probs and not (a.cmd == 'write' and a.force and nofront):
                for line in probs:
                    print('  ' + line)
                print('The file is not a slot image recovery writes; nothing was sent.')
                return 1
            if not probs and not front['pin_ok']:
                return pin_refusal(print, front)
        link = Link(a.port or find_port())
        try:
            if a.cmd == 'info':
                return cmd_info(link)
            if a.cmd == 'write':
                return cmd_write(link, data, running=a.running, new_front=a.new_front,
                                 force=a.force)
            if a.cmd == 'select':
                return cmd_select(link, data, force=a.force)
            if a.cmd in acts:
                return cmd_act(link, acts[a.cmd], force=getattr(a, 'force', False))
            return cmd_restart(link, force=a.force)
        finally:
            link.close()
    except Failure as e:
        print('FAILED: %s' % e)
        return 1


if __name__ == '__main__':
    sys.exit(main())
