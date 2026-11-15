# SPDX-License-Identifier: GPL-2.0-or-later
"""USB Mass Storage Bulk-Only Transport (BOT 1.0) and SCSI (SPC-3, SBC-2) reply oracles: CBW/CSW
codecs, a block-device Target model, bot_expect (BOT 6.7 cases) and check_* functions on wire bytes."""
import struct
from collections import namedtuple

Finding = namedtuple('Finding', 'rule where msg')


class MalformedError(ValueError):
    pass


CBW_SIGNATURE, CSW_SIGNATURE = 0x43425355, 0x53425355
CBW_LEN, CSW_LEN = 31, 13
CSW_PASSED, CSW_FAILED, CSW_PHASE_ERROR = 0, 1, 2

TEST_UNIT_READY, REQUEST_SENSE, INQUIRY, MODE_SELECT6, MODE_SENSE6 = 0x00, 0x03, 0x12, 0x15, 0x1A
START_STOP_UNIT, PREVENT_ALLOW, READ_FORMAT_CAPACITIES, READ_CAPACITY10 = 0x1B, 0x1E, 0x23, 0x25
READ10, WRITE10, VERIFY10, SYNCHRONIZE_CACHE10, MODE_SENSE10 = 0x28, 0x2A, 0x2F, 0x35, 0x5A
READ6, WRITE6 = 0x08, 0x0A
VPD_SUPPORTED_PAGES, VPD_UNIT_SERIAL, VPD_DEVICE_ID = 0x00, 0x80, 0x83

NO_SENSE, RECOVERED_ERROR, NOT_READY, MEDIUM_ERROR, HARDWARE_ERROR = 0x0, 0x1, 0x2, 0x3, 0x4
ILLEGAL_REQUEST, UNIT_ATTENTION, DATA_PROTECT = 0x5, 0x6, 0x7

# (key, asc, ascq) of the conditions the model raises (SPC-3 annex D)
SENSE_NONE = (NO_SENSE, 0x00, 0x00)
SENSE_INVALID_OPCODE = (ILLEGAL_REQUEST, 0x20, 0x00)
SENSE_LBA_OUT_OF_RANGE = (ILLEGAL_REQUEST, 0x21, 0x00)
SENSE_INVALID_FIELD_IN_CDB = (ILLEGAL_REQUEST, 0x24, 0x00)
SENSE_LUN_NOT_SUPPORTED = (ILLEGAL_REQUEST, 0x25, 0x00)
SENSE_SAVING_NOT_SUPPORTED = (ILLEGAL_REQUEST, 0x39, 0x00)
SENSE_WRITE_PROTECTED = (DATA_PROTECT, 0x27, 0x00)
SENSE_MEDIUM_NOT_PRESENT = (NOT_READY, 0x3A, 0x00)
SENSE_MEDIUM_CHANGED = (UNIT_ATTENTION, 0x28, 0x00)

GOOD, CHECK_CONDITION = 0x00, 0x02


# ------------------------------------------------------------------------------ BOT wrappers
CBW = namedtuple('CBW', 'signature tag data_length flags lun cb_length cb')
CSW = namedtuple('CSW', 'signature tag residue status')


def encode_cbw(tag, data_length, data_in, lun, cdb):
    cdb = bytes(cdb)
    if not 1 <= len(cdb) <= 16:
        raise ValueError('CDB length 1..16')
    return struct.pack('<IIIBBB', CBW_SIGNATURE, tag, data_length, 0x80 if data_in else 0, lun, len(cdb)) + \
        cdb + bytes(16 - len(cdb))


def decode_cbw(b, max_lun=0):
    """BOT 6.2.1 (valid: 31 bytes and the signature; otherwise MalformedError) and 6.2.2 (meaningful:
    reserved bits zero, LUN supported, CB length 1..16; violations are findings)."""
    b = bytes(b)
    if len(b) != CBW_LEN:
        raise MalformedError(f'CBW: {len(b)} bytes, a valid CBW is {CBW_LEN} (BOT 6.2.1)')
    c = CBW(*struct.unpack_from('<IIIBBB', b), cb=b[15:31])
    if c.signature != CBW_SIGNATURE:
        raise MalformedError(f'CBW: signature {c.signature:#010x} (BOT 6.2.1)')
    f = []
    if c.flags & 0x7F:
        f.append(Finding('cbw_reserved', 'CBW', f'bmCBWFlags {c.flags:#04x}'))
    if c.lun & 0xF0:
        f.append(Finding('cbw_reserved', 'CBW', f'bCBWLUN {c.lun:#04x}'))
    if c.lun & 0x0F > max_lun:
        f.append(Finding('cbw_lun', 'CBW', f'LUN {c.lun & 0xF} > max {max_lun}'))
    if c.cb_length & 0xE0 or not 1 <= c.cb_length & 0x1F <= 16:
        f.append(Finding('cbw_cb_length', 'CBW', f'bCBWCBLength {c.cb_length}'))
    return c, f


def encode_csw(tag, residue, status):
    return struct.pack('<IIIB', CSW_SIGNATURE, tag, residue, status)


def decode_csw(b, cbw=None):
    b = bytes(b)
    if len(b) != CSW_LEN:
        raise MalformedError(f'CSW: {len(b)} bytes, a CSW is {CSW_LEN}')
    c = CSW(*struct.unpack('<IIIB', b))
    if c.signature != CSW_SIGNATURE:
        raise MalformedError(f'CSW: signature {c.signature:#010x}')
    f = []
    if c.status > CSW_PHASE_ERROR:
        f.append(Finding('csw_status', 'CSW', f'bCSWStatus {c.status}'))
    if cbw is not None:
        if c.tag != cbw.tag:
            f.append(Finding('csw_tag', 'CSW', f'tag {c.tag:#x} != CBW tag {cbw.tag:#x}'))
        if c.residue > cbw.data_length:
            f.append(Finding('csw_residue', 'CSW', f'residue {c.residue} > dCBWDataTransferLength {cbw.data_length}'))
    return c, f


BotOutcome = namedtuple('BotOutcome', 'case transfer status residue stall')


def bot_expect(host_len, host_in, dev_len, dev_in, command_ok=True):
    """BOT 1.0 6.7: host expects host_len bytes (direction host_in when host_len > 0), device intends
    dev_len bytes (direction dev_in). Returns the case number, bytes moved, CSW status, residue and
    which pipe the device may stall ('in', 'out' or None; for cases 4/5/9/11 the device may instead
    end the transfer short, so a stall there is permitted, not required)."""
    ok = CSW_PASSED if command_ok else CSW_FAILED
    if host_len == 0:
        if dev_len == 0:
            return BotOutcome(1, 0, ok, 0, None)
        return BotOutcome(2 if dev_in else 3, 0, CSW_PHASE_ERROR, 0, None)
    if host_in:
        if dev_len == 0:
            return BotOutcome(4, 0, ok, host_len, 'in')
        if not dev_in:
            return BotOutcome(8, 0, CSW_PHASE_ERROR, host_len, 'in')
        if host_len > dev_len:
            return BotOutcome(5, dev_len, ok, host_len - dev_len, 'in')
        if host_len == dev_len:
            return BotOutcome(6, dev_len, ok, 0, None)
        return BotOutcome(7, host_len, CSW_PHASE_ERROR, 0, None)
    if dev_len == 0:
        return BotOutcome(9, 0, ok, host_len, 'out')
    if dev_in:
        return BotOutcome(10, 0, CSW_PHASE_ERROR, host_len, 'out')
    if host_len > dev_len:
        return BotOutcome(11, dev_len, ok, host_len - dev_len, 'out')
    if host_len == dev_len:
        return BotOutcome(12, dev_len, ok, 0, None)
    return BotOutcome(13, host_len, CSW_PHASE_ERROR, 0, None)


# ------------------------------------------------------------------------------ block device
class BlockDevice:
    def __init__(self, num_blocks, block_size=512, present=True, write_protect=False, removable=True, fill=0):
        self.num_blocks, self.block_size = num_blocks, block_size
        self.present, self.write_protect, self.removable = present, write_protect, removable
        self.fill = fill
        self.blocks = {}                       # lba -> bytes (sparse)

    def read(self, lba, n):
        return b''.join(self.blocks.get(lba + i, bytes([self.fill]) * self.block_size) for i in range(n))

    def write(self, lba, data):
        bs = self.block_size
        for i in range(len(data) // bs):
            self.blocks[lba + i] = bytes(data[i * bs:(i + 1) * bs])


Reply = namedtuple('Reply', 'status data_in data_out_len sense')


def _ascii_field(s, n, name):
    b = s.encode('ascii') if isinstance(s, str) else bytes(s)
    if len(b) > n:
        raise ValueError(f'{name} longer than {n} bytes')
    return b + b' ' * (n - len(b))


class Target:
    """One logical unit. INQUIRY parameters (SPC-3 6.4.2): peripheral (qualifier<<5 | type), rmb,
    version, response_format byte (byte 3 as a whole), flags5/6/7, vendor (8), product (16), revision
    (4). vpd: {page: payload bytes} beyond page 0 (generated). mode_pages: {page: current bytes}.
    sense_valid: 'auto' (VALID when the information field is nonzero), True or False."""

    def __init__(self, dev, vendor='', product='', revision='', peripheral=0x00, rmb=None, version=0x05,
                 response_format=0x02, flags5=0, flags6=0, flags7=0, vpd=None, mode_pages=None,
                 block_descriptor=True, sense_valid='auto', unit_attention=True, max_lun=0):
        self.dev = dev
        self.inq = dict(vendor=_ascii_field(vendor, 8, 'vendor'), product=_ascii_field(product, 16, 'product'),
                        revision=_ascii_field(revision, 4, 'revision'), peripheral=peripheral,
                        rmb=dev.removable if rmb is None else rmb, version=version,
                        response_format=response_format, flags=(flags5, flags6, flags7))
        self.vpd = dict(vpd or {})
        self.mode_pages = dict(mode_pages or {})
        self.block_descriptor = block_descriptor
        self.sense_valid = sense_valid
        self.unit_attention = unit_attention
        self.max_lun = max_lun
        self.sense = SENSE_NONE
        self.info = 0
        self.cinfo = 0
        self.pending_ua = False

    # -- medium events
    def set_present(self, present):
        """Medium removed (False) or inserted (True). Insertion raises the unit attention MEDIUM MAY
        HAVE CHANGED (ASC 28h, SPC-3 annex D); while absent, media commands get NOT READY / MEDIUM NOT
        PRESENT."""
        if present and not self.dev.present and self.unit_attention:
            self.pending_ua = True
        self.dev.present = present

    def medium_change(self):
        """A withdrawal and re-offer of the medium as the host sees it: absent, then present."""
        self.set_present(False)
        self.set_present(True)

    # -- replies
    def standard_inquiry(self):
        i = self.inq
        b = bytes([i['peripheral'], 0x80 if i['rmb'] else 0, i['version'], i['response_format'], 31]) + \
            bytes(i['flags']) + i['vendor'] + i['product'] + i['revision']
        assert len(b) == 36
        return b

    def vpd_page(self, page):
        if page == 0x00:
            pages = sorted(set([0] + list(self.vpd)))
            return bytes([self.inq['peripheral'], 0, 0, len(pages)]) + bytes(pages)
        if page in self.vpd:
            p = self.vpd[page]
            return bytes([self.inq['peripheral'], page]) + struct.pack('>H', len(p)) + p
        return None

    def sense_bytes(self):
        key, asc, ascq = self.sense
        valid = self.sense_valid if self.sense_valid != 'auto' else self.info != 0
        return bytes([0x70 | (0x80 if valid else 0), 0, key]) + struct.pack('>I', self.info) + bytes([10]) + \
            struct.pack('>I', self.cinfo) + bytes([asc, ascq, 0, 0, 0, 0])

    def mode_sense6_data(self, dbd, pc, page):
        if pc == 3:
            return None, SENSE_SAVING_NOT_SUPPORTED
        if page == 0x3F:
            pages = b''.join(self._page(p, pc) for p in sorted(self.mode_pages))
        elif page in self.mode_pages:
            pages = self._page(page, pc)
        else:
            return None, SENSE_INVALID_FIELD_IN_CDB
        bd = b''
        if self.block_descriptor and not dbd:
            # SBC-2 6.4.2 short LBA mode parameter block descriptor: NUMBER OF LOGICAL BLOCKS (4 bytes,
            # FFFFFFFFh when the medium has more), reserved, LOGICAL BLOCK LENGTH (3 bytes)
            n = min(self.dev.num_blocks, 0xFFFFFFFF)
            bd = n.to_bytes(4, 'big') + bytes([0]) + self.dev.block_size.to_bytes(3, 'big')
        dsp = 0x80 if self.dev.write_protect else 0
        body = bytes([0, dsp, len(bd)]) + bd + pages
        return bytes([len(body)]) + body, None

    def _page(self, p, pc):
        cur = self.mode_pages[p]
        if pc == 1:                               # changeable values: none changeable
            cur = cur[:2] + bytes(len(cur) - 2)
        return cur

    def _fail(self, sense, info=0):
        self.sense, self.info, self.cinfo = sense, info, 0
        return Reply(CHECK_CONDITION, b'', 0, self.sense_bytes())

    def _ok(self, data=b'', out_len=0):
        self.sense, self.info, self.cinfo = SENSE_NONE, 0, 0
        return Reply(GOOD, bytes(data), out_len, self.sense_bytes())

    def execute(self, cdb, data_out=b'', lun=0):
        """A CDB shorter than its operation code's group length (SPC-3 4.3.4.1) is answered with
        INVALID FIELD IN CDB; the model never raises on CDB contents."""
        cdb = bytes(cdb)
        if not cdb:
            return self._fail(SENSE_INVALID_OPCODE)
        op = cdb[0]
        if len(cdb) < cdb_length(op):
            return self._fail(SENSE_INVALID_FIELD_IN_CDB)
        if op == REQUEST_SENSE:
            alloc = cdb[4]
            if self.pending_ua:
                self.pending_ua = False
                self.sense, self.info = SENSE_MEDIUM_CHANGED, 0
            data = self.sense_bytes()[:alloc]
            self.sense, self.info, self.cinfo = SENSE_NONE, 0, 0
            return Reply(GOOD, data, 0, self.sense_bytes())
        if lun > self.max_lun:
            if op == INQUIRY:
                return self._ok(bytes([0x7F]) + self.standard_inquiry()[1:][:max(0, _alloc16(cdb) - 1)])
            return self._fail(SENSE_LUN_NOT_SUPPORTED)
        if op == INQUIRY:
            alloc = _alloc16(cdb)
            if cdb[1] & 1:
                page = self.vpd_page(cdb[2])
                if page is None:
                    return self._fail(SENSE_INVALID_FIELD_IN_CDB)
                return self._ok(page[:alloc])
            if cdb[2]:
                return self._fail(SENSE_INVALID_FIELD_IN_CDB)
            return self._ok(self.standard_inquiry()[:alloc])
        if self.pending_ua:
            self.pending_ua = False
            return self._fail(SENSE_MEDIUM_CHANGED)
        if op == TEST_UNIT_READY:
            return self._ok() if self.dev.present else self._fail(SENSE_MEDIUM_NOT_PRESENT)
        if op == MODE_SENSE6:
            if cdb[3]:                               # subpage code: no subpages modeled
                return self._fail(SENSE_INVALID_FIELD_IN_CDB)
            data, err = self.mode_sense6_data(bool(cdb[1] & 8), cdb[2] >> 6, cdb[2] & 0x3F)
            if err:
                return self._fail(err)
            return self._ok(data[:cdb[4]])
        if op == READ_CAPACITY10:
            if not self.dev.present:
                return self._fail(SENSE_MEDIUM_NOT_PRESENT)
            if cdb[8] & 1 == 0 and struct.unpack_from('>I', cdb, 2)[0]:
                return self._fail(SENSE_INVALID_FIELD_IN_CDB)   # LBA nonzero without PMI (SBC-2 5.10)
            last = min(self.dev.num_blocks - 1, 0xFFFFFFFF)
            return self._ok(struct.pack('>II', last, self.dev.block_size))
        if op in (READ6, WRITE6, READ10, WRITE10):
            lba, n = rw_lba_length(cdb)
            if not self.dev.present:
                return self._fail(SENSE_MEDIUM_NOT_PRESENT)
            if op in (READ10, WRITE10) and cdb[1] & 0xE0:
                # RDPROTECT / WRPROTECT nonzero without protection information (SBC-2 READ(10), WRITE(10))
                return self._fail(SENSE_INVALID_FIELD_IN_CDB)
            if lba + n > self.dev.num_blocks:
                return self._fail(SENSE_LBA_OUT_OF_RANGE, info=lba)
            if op in (READ6, READ10):
                return self._ok(self.dev.read(lba, n))
            if self.dev.write_protect:
                return self._fail(SENSE_WRITE_PROTECTED)
            need = n * self.dev.block_size
            if len(data_out) < need:
                # the host moved fewer bytes than the CDB asks for: the unit still intends Do = need
                # bytes and writes nothing; the transport (bot_expect) decides the outcome
                return Reply(GOOD, b'', need, self.sense_bytes())
            self.dev.write(lba, data_out[:need])
            return self._ok(b'', need)
        return self._fail(SENSE_INVALID_OPCODE)


def _alloc16(cdb):
    return struct.unpack_from('>H', cdb, 3)[0]


def cdb_length(op):
    """SPC-3 4.3.4.1 operation code groups: 0 = 6 bytes, 1 and 2 = 10, 4 = 16, 5 = 12; others
    (reserved / vendor specific) are given 1 so that only the opcode is required."""
    return {0: 6, 1: 10, 2: 10, 4: 16, 5: 12}.get(op >> 5, 1)


def rw_lba_length(cdb):
    """(LBA, number of blocks) of a READ/WRITE(6) or (10) CDB as SBC-2 defines them. READ(6) and
    WRITE(6): a 21-bit LBA (byte 1 bits 4:0, bytes 2 and 3) and a TRANSFER LENGTH of 0 meaning 256
    blocks (SBC-2 5.5, 5.25). READ(10)/WRITE(10): a 32-bit LBA and 16-bit length; 0 means none."""
    cdb = bytes(cdb)
    if len(cdb) < 1 or len(cdb) < cdb_length(cdb[0]):
        raise MalformedError('CDB shorter than its group length')
    if cdb[0] in (READ6, WRITE6):
        return ((cdb[1] & 0x1F) << 16) | (cdb[2] << 8) | cdb[3], cdb[4] or 256
    if cdb[0] in (READ10, WRITE10):
        return struct.unpack_from('>I', cdb, 2)[0], struct.unpack_from('>H', cdb, 7)[0]
    raise MalformedError(f'opcode {cdb[0]:#04x} is not READ/WRITE(6)/(10)')


def cdb_read6(lba, n):
    """n in 1..256 (256 is sent as 0)."""
    if not 0 <= lba < 1 << 21 or not 1 <= n <= 256:
        raise ValueError('READ(6): LBA 21 bits, 1..256 blocks')
    return bytes([READ6, lba >> 16, (lba >> 8) & 0xFF, lba & 0xFF, n & 0xFF, 0])


def cdb_write6(lba, n):
    return bytes([WRITE6]) + cdb_read6(lba, n)[1:]


def cdb_inquiry(alloc, evpd=False, page=0):
    return bytes([INQUIRY, 1 if evpd else 0, page]) + struct.pack('>H', alloc) + b'\0'


def cdb_read10(lba, n):
    return struct.pack('>BBIBHB', READ10, 0, lba, 0, n, 0)


def cdb_write10(lba, n):
    return struct.pack('>BBIBHB', WRITE10, 0, lba, 0, n, 0)


def cdb_mode_sense6(page=0x3F, pc=0, dbd=False, alloc=0xFF, subpage=0):
    return bytes([MODE_SENSE6, 8 if dbd else 0, (pc << 6) | page, subpage, alloc, 0])


def cdb_request_sense(alloc=18):
    return bytes([REQUEST_SENSE, 0, 0, 0, alloc, 0])


def cdb_read_capacity10():
    return bytes([READ_CAPACITY10]) + bytes(9)


def cdb_tur():
    return bytes(6)


def run_bot(target, cbw_bytes, data_out=b''):
    """One BOT command end to end: (data-in bytes on the wire, CSW bytes, BotOutcome). The device
    intention is the reply's data length; residue and status follow bot_expect."""
    cbw, f = decode_cbw(cbw_bytes, target.max_lun)
    if f:
        # a valid but not meaningful CBW: BOT 6.4 leaves the response to the device; this model
        # answers with a phase error and no data
        return b'', encode_csw(cbw.tag, cbw.data_length, CSW_PHASE_ERROR), None
    host_in = bool(cbw.flags & 0x80)
    cdb = cbw.cb[:cbw.cb_length]
    r = target.execute(cdb, data_out if not host_in else b'', cbw.lun & 0xF)
    dev_len, dev_in = (len(r.data_in), True) if r.data_in else (r.data_out_len, False)
    o = bot_expect(cbw.data_length, host_in, dev_len, dev_in, r.status == GOOD)
    data = r.data_in[:o.transfer] if dev_in else b''
    return data, encode_csw(cbw.tag, o.residue, o.status), o


# ------------------------------------------------------------------------------ reply checks
def check_inquiry(data, alloc, **want):
    """Standard INQUIRY data as received for allocation length alloc. Structural: no more than
    min(alloc, 5 + additional length) bytes; additional length >= 31; vendor/product/revision
    printable ASCII (SPC-3 4.4.1). want: exact values for any of peripheral, rmb, version,
    response_format, vendor, product, revision (padded as on the wire)."""
    data = bytes(data)
    out = []
    if len(data) > alloc:
        out.append(Finding('alloc_length', 'INQUIRY', f'{len(data)} bytes for allocation length {alloc}'))
    if len(data) >= 5:
        total = 5 + data[4]
        if data[4] < 31:
            out.append(Finding('inquiry_length', 'INQUIRY', f'additional length {data[4]} < 31'))
        if len(data) < min(alloc, total):
            out.append(Finding('alloc_length', 'INQUIRY', f'{len(data)} bytes, {min(alloc, total)} owed'))
        if len(data) > total:
            out.append(Finding('inquiry_length', 'INQUIRY', 'more bytes than 5 + additional length'))
    if len(data) >= 4 and data[3] & 0x0F != 2:
        out.append(Finding('response_format', 'INQUIRY', f'response data format {data[3] & 0xF}'))
    for lo, hi, name in ((8, 16, 'vendor'), (16, 32, 'product'), (32, 36, 'revision')):
        seg = data[lo:hi]
        if any(c < 0x20 or c > 0x7E for c in seg):
            out.append(Finding('inquiry_ascii', 'INQUIRY', f'{name} not printable ASCII'))
        if name in want and seg != _ascii_field(want[name], hi - lo, name)[:len(seg)]:
            out.append(Finding('inquiry_value', 'INQUIRY', f'{name} {seg!r}'))
    for k, pos, fn in (('peripheral', 0, lambda b: b), ('rmb', 1, lambda b: bool(b & 0x80)),
                       ('version', 2, lambda b: b), ('response_format', 3, lambda b: b)):
        if k in want and len(data) > pos and fn(data[pos]) != want[k]:
            out.append(Finding('inquiry_value', 'INQUIRY', f'{k} {data[pos]:#04x}'))
    return out


def check_read_capacity10(data, dev=None):
    data = bytes(data)
    if len(data) != 8:
        return [Finding('capacity_length', 'READ CAPACITY(10)', f'{len(data)} bytes, 8 owed')]
    last, bs = struct.unpack('>II', data)
    out = []
    if bs == 0:
        out.append(Finding('block_length', 'READ CAPACITY(10)', 'block length 0'))
    if dev is not None:
        if last != min(dev.num_blocks - 1, 0xFFFFFFFF):
            out.append(Finding('capacity_value', 'READ CAPACITY(10)', f'last LBA {last}, medium has {dev.num_blocks} blocks'))
        if bs != dev.block_size:
            out.append(Finding('capacity_value', 'READ CAPACITY(10)', f'block length {bs}, medium {dev.block_size}'))
    return out


def check_request_sense(data, alloc, sense=None, info=None):
    """Fixed-format sense data (SPC-3 4.5.3) received for allocation length alloc. sense: expected
    (key, asc, ascq); info: expected INFORMATION field."""
    data = bytes(data)
    out = []
    if len(data) > alloc:
        out.append(Finding('alloc_length', 'REQUEST SENSE', f'{len(data)} bytes for allocation length {alloc}'))
    if not data:
        return out
    if data[0] & 0x7F not in (0x70, 0x71):
        out.append(Finding('sense_format', 'REQUEST SENSE', f'response code {data[0] & 0x7F:#04x}'))
        return out
    if len(data) > 7:
        total = 8 + data[7]
        if data[7] < 10:
            out.append(Finding('sense_length', 'REQUEST SENSE', f'additional sense length {data[7]} < 10'))
        if len(data) < min(alloc, total):
            out.append(Finding('alloc_length', 'REQUEST SENSE', f'{len(data)} bytes, {min(alloc, total)} owed'))
    if len(data) > 2 and data[2] & 0x10:
        out.append(Finding('sense_reserved', 'REQUEST SENSE', 'reserved bit 4 of byte 2'))
    if sense is not None:
        got = (data[2] & 0xF if len(data) > 2 else None, data[12] if len(data) > 12 else None,
               data[13] if len(data) > 13 else None)
        want = (sense[0], sense[1] if len(data) > 12 else None, sense[2] if len(data) > 13 else None)
        if got != want:
            out.append(Finding('sense_value', 'REQUEST SENSE', f'{got} != {want}'))
    if info is not None and len(data) >= 7 and struct.unpack_from('>I', data, 3)[0] != info:
        out.append(Finding('sense_value', 'REQUEST SENSE', 'INFORMATION field'))
    return out


def check_mode_sense6(data, alloc, dev=None):
    data = bytes(data)
    out = []
    if len(data) > alloc:
        out.append(Finding('alloc_length', 'MODE SENSE(6)', f'{len(data)} bytes for {alloc}'))
    if len(data) < 4:
        return out if len(data) == min(alloc, 4) or len(data) == alloc else out + [
            Finding('mode_length', 'MODE SENSE(6)', 'header cut short')]
    total = data[0] + 1
    if len(data) < min(alloc, total):
        out.append(Finding('alloc_length', 'MODE SENSE(6)', f'{len(data)} bytes, {min(alloc, total)} owed'))
    if data[3] not in (0, 8):
        out.append(Finding('block_descriptor', 'MODE SENSE(6)', f'block descriptor length {data[3]}'))
    if dev is not None and bool(data[2] & 0x80) != dev.write_protect:
        out.append(Finding('write_protect', 'MODE SENSE(6)', 'WP bit does not match the medium'))
    if data[3] == 8 and len(data) >= 12 and dev is not None:
        n = int.from_bytes(data[4:8], 'big')            # SBC-2 6.4.2 short LBA block descriptor
        bl = int.from_bytes(data[9:12], 'big')
        if n != min(dev.num_blocks, 0xFFFFFFFF) or bl != dev.block_size:
            out.append(Finding('block_descriptor', 'MODE SENSE(6)', f'{n} blocks x {bl}'))
    off = 4 + data[3]
    while off + 2 <= min(len(data), total):
        pl = data[off + 1]
        if off + 2 + pl > total:
            out.append(Finding('mode_page', 'MODE SENSE(6)', f'page {data[off] & 0x3F:#04x} runs past the mode data'))
            break
        off += 2 + pl
    return out


# ------------------------------------------------------------------------------ VPD pages (SPC-3 7.6)
# Device Identification page (0x83, SPC-3 7.6.3): designation descriptors
CODE_SET_BINARY, CODE_SET_ASCII, CODE_SET_UTF8 = 1, 2, 3
ASSOC_LU, ASSOC_PORT, ASSOC_TARGET = 0, 1, 2
DESIG_VENDOR, DESIG_T10, DESIG_EUI64, DESIG_NAA, DESIG_REL_PORT, DESIG_PORT_GROUP, DESIG_LU_GROUP, \
    DESIG_MD5, DESIG_NAME_STRING = range(9)
Designator = namedtuple('Designator', 'protocol code_set piv association type ident')


def designation_descriptor(dtype, ident, code_set=None, association=ASSOC_LU, piv=False, protocol=0):
    """One designation descriptor (SPC-3 7.6.3.1): header of 4 bytes, then the designator."""
    ident = bytes(ident)
    if code_set is None:
        code_set = CODE_SET_ASCII if dtype in (DESIG_T10,) else \
            CODE_SET_UTF8 if dtype == DESIG_NAME_STRING else CODE_SET_BINARY
    if len(ident) > 255:
        raise ValueError('designator longer than 255 bytes')
    return bytes([(protocol & 0xF) << 4 | code_set & 0xF, (0x80 if piv else 0) | (association & 3) << 4 | dtype & 0xF,
                  0, len(ident)]) + ident


def build_vpd83(descriptors, peripheral=0x00):
    """The whole page: header with PAGE LENGTH equal to the bytes that follow it, then descriptors."""
    body = b''.join(descriptors)
    return bytes([peripheral, VPD_DEVICE_ID]) + struct.pack('>H', len(body)) + body


def _vpd_header(data, page, name):
    data = bytes(data)
    if len(data) < 4:
        raise MalformedError(f'{name}: {len(data)} bytes, header is 4')
    out = []
    if data[1] != page:
        out.append(Finding('vpd_page_code', name, f'page code {data[1]:#04x}'))
    return data, struct.unpack_from('>H', data, 2)[0], out


def _vpd_lengths(data, alloc, plen, name):
    out = []
    total = 4 + plen
    if alloc is not None:
        if len(data) > alloc:
            out.append(Finding('alloc_length', name, f'{len(data)} bytes for allocation length {alloc}'))
        if len(data) < min(alloc, total):
            out.append(Finding('alloc_length', name, f'{len(data)} bytes, {min(alloc, total)} owed'))
    if len(data) > total:
        out.append(Finding('vpd_page_length', name, f'{len(data) - total} bytes after PAGE LENGTH {plen}'))
    return out


def decode_vpd83(data, alloc=None):
    """Device Identification VPD page as received. Returns (designators, findings). Structural:
    fewer than 4 bytes, or a descriptor that runs past PAGE LENGTH, raises MalformedError; a
    descriptor cut by the end of the received bytes (allocation length) is dropped, not an error.
    Findings: vpd_page_code, alloc_length, vpd_page_length (descriptors do not fill PAGE LENGTH
    exactly, or bytes beyond it), desig_reserved, desig_code_set, desig_length, desig_naa,
    desig_ascii, desig_association, lu_name (no logical-unit name designator: types 1, 2, 3 or 8
    with association 0), designator_duplicate."""
    name = 'VPD 0x83'
    data, plen, out = _vpd_header(data, VPD_DEVICE_ID, name)
    out += _vpd_lengths(data, alloc, plen, name)
    end = min(len(data), 4 + plen)
    full = len(data) >= 4 + plen
    descs, off = [], 4
    pend = 4 + plen
    while off < end:
        if off + 4 > pend:
            out.append(Finding('vpd_page_length', name, f'{pend - off} bytes left over at the end'))
            break
        if off + 4 > len(data):
            break                                          # header cut by the allocation length
        if off + 4 + data[off + 3] > pend:
            raise MalformedError(f'{name}: descriptor at {off} runs past PAGE LENGTH')
        if off + 4 + data[off + 3] > len(data):
            break                                          # designator cut by the allocation length
        h0, h1, h2, n = data[off:off + 4]
        d = Designator(h0 >> 4, h0 & 0xF, bool(h1 & 0x80), (h1 >> 4) & 3, h1 & 0xF, data[off + 4:off + 4 + n])
        out += _check_designator(d, h1, h2, f'{name}[{len(descs)}]')
        descs.append(d)
        off += 4 + n
    if full:
        if not any(d.association == ASSOC_LU and d.type in (DESIG_T10, DESIG_EUI64, DESIG_NAA, DESIG_NAME_STRING)
                   for d in descs):
            out.append(Finding('lu_name', name, 'no logical-unit designator of type 1, 2, 3 or 8'))
        keys = [(d.association, d.type, d.ident) for d in descs]
        if len(set(keys)) != len(keys):
            out.append(Finding('designator_duplicate', name, 'the same designator listed twice'))
    return descs, out


def _check_designator(d, h1, h2, where):
    out = []
    if h2 or h1 & 0x40 or d.association == 3:
        out.append(Finding('desig_reserved', where, 'reserved bits or association 3'))
    if d.piv and d.association == ASSOC_LU:
        out.append(Finding('desig_reserved', where, 'PIV set with association logical unit'))
    if d.code_set not in (CODE_SET_BINARY, CODE_SET_ASCII, CODE_SET_UTF8):
        out.append(Finding('desig_code_set', where, f'code set {d.code_set}'))
    n, t = len(d.ident), d.type
    if d.code_set == CODE_SET_ASCII and any(c < 0x20 or c > 0x7E for c in d.ident):
        out.append(Finding('desig_ascii', where, 'ASCII designator not printable'))
    if t == DESIG_T10 and n < 8:
        out.append(Finding('desig_length', where, f'T10 vendor ID designator of {n} bytes (< 8)'))
    elif t == DESIG_EUI64:
        if n not in (8, 12, 16) or d.code_set != CODE_SET_BINARY:
            out.append(Finding('desig_length', where, f'EUI-64 designator: {n} bytes, code set {d.code_set}'))
    elif t == DESIG_NAA:
        naa = d.ident[0] >> 4 if n else None
        if d.code_set != CODE_SET_BINARY or {2: 8, 3: 8, 5: 8, 6: 16}.get(naa) != n:
            out.append(Finding('desig_naa', where, f'NAA {naa} with {n} bytes, code set {d.code_set}'))
    elif t in (DESIG_REL_PORT, DESIG_PORT_GROUP, DESIG_LU_GROUP):
        if n != 4 or d.code_set != CODE_SET_BINARY:
            out.append(Finding('desig_length', where, f'type {t}: {n} bytes (4 owed)'))
        if (t == DESIG_LU_GROUP) != (d.association == ASSOC_LU):
            out.append(Finding('desig_association', where, f'type {t} with association {d.association}'))
    elif t == DESIG_MD5:
        if n != 16 or d.association != ASSOC_LU:
            out.append(Finding('desig_length', where, f'MD5 LU designator of {n} bytes'))
    elif t == DESIG_NAME_STRING:
        if d.code_set != CODE_SET_UTF8 or n % 4 or not n or d.ident[-1] != 0:
            out.append(Finding('desig_length', where, 'SCSI name string: UTF-8, NUL-terminated, multiple of 4'))
    elif t > DESIG_NAME_STRING:
        out.append(Finding('desig_reserved', where, f'designator type {t} reserved'))
    return out


def decode_vpd00(data, alloc=None):
    """Supported VPD Pages page (SPC-3 7.6.10) as received: (page list, findings). Findings:
    vpd_page_code, alloc_length, vpd_page_length, vpd_list_order (not ascending, or a page twice),
    vpd_list_zero (page 0x00 itself not listed)."""
    name = 'VPD 0x00'
    data, plen, out = _vpd_header(data, VPD_SUPPORTED_PAGES, name)
    out += _vpd_lengths(data, alloc, plen, name)
    pages = list(data[4:4 + plen])
    if any(a >= b for a, b in zip(pages, pages[1:])):
        out.append(Finding('vpd_list_order', name, 'pages not in strictly ascending order'))
    if len(data) >= 4 + plen and VPD_SUPPORTED_PAGES not in pages:
        out.append(Finding('vpd_list_zero', name, 'page 0x00 not listed'))
    return pages, out


def sense_triple(sense):
    """(key, asc, ascq) from fixed-format sense bytes, a triple, or None."""
    if sense is None:
        return None
    if isinstance(sense, tuple) and len(sense) == 3:
        return sense
    s = bytes(sense)
    if not s or s[0] & 0x7F not in (0x70, 0x71):
        return None
    return (s[2] & 0xF if len(s) > 2 else None, s[12] if len(s) > 12 else None, s[13] if len(s) > 13 else None)


def check_vpd_offer(supported, replies):
    """Consistency of the pages a unit offers (SPC-3 6.4.1, 7.6.10). supported: bytes of the page
    0x00 reply (complete). replies: {page: (status, data_in, sense)} for INQUIRY with EVPD=1 and that
    page (sense as bytes or triple). A listed page must answer GOOD with its own page code
    (vpd_listed_fails); an unlisted page must get CHECK CONDITION, ILLEGAL REQUEST / INVALID FIELD
    IN CDB (vpd_unlisted_answers, vpd_unlisted_sense)."""
    pages, out = decode_vpd00(supported)
    for page, r in sorted(replies.items()):
        status, data, sense = r[0], bytes(r[1] or b''), sense_triple(r[2] if len(r) > 2 else None)
        where = f'VPD {page:#04x}'
        if page in pages:
            if status != GOOD or len(data) < 2 or data[1] != page:
                out.append(Finding('vpd_listed_fails', where, f'listed in page 0x00, status {status:#04x}'))
        elif status == GOOD:
            out.append(Finding('vpd_unlisted_answers', where, 'not listed in page 0x00 but answered'))
        elif sense is not None and sense != SENSE_INVALID_FIELD_IN_CDB:
            out.append(Finding('vpd_unlisted_sense', where, f'sense {sense}'))
    return out


# ------------------------------------------------------------------------------ sequences and replies
_TUR_ALLOWED = {(NOT_READY, 0x3A), (UNIT_ATTENTION, 0x28), (UNIT_ATTENTION, 0x29), (NOT_READY, 0x04)}


def check_medium_change(replies, require_absent=False):
    """A host's TEST UNIT READY poll across a withdrawal and re-offer of the medium. replies: the
    (status, sense) of each TUR in order (sense bytes or (key, asc, ascq); ignored when GOOD).
    Findings: tur_sense (a CHECK CONDITION other than NOT READY 3Ah/04h or UNIT ATTENTION 28h/29h),
    medium_change_no_ua (after MEDIUM NOT PRESENT the unit becomes ready with no UNIT ATTENTION
    28h/00h in between), medium_change_not_seen (the poll never saw the change: no not-present and
    no unit attention), medium_absent_not_seen (require_absent and no MEDIUM NOT PRESENT)."""
    out = []
    absent_seen = ua_seen = waiting_ua = False
    for i, r in enumerate(replies):
        status, sense = r[0], sense_triple(r[1] if len(r) > 1 else None)
        if status == GOOD:
            if waiting_ua:
                out.append(Finding('medium_change_no_ua', f'TUR[{i}]', 'ready again without unit attention 28h'))
                waiting_ua = False
            continue
        if sense is None or (sense[0], sense[1]) not in _TUR_ALLOWED:
            out.append(Finding('tur_sense', f'TUR[{i}]', f'status {status:#04x} sense {sense}'))
            continue
        if sense[:2] == (NOT_READY, 0x3A):
            absent_seen = waiting_ua = True
        elif sense[:2] == (UNIT_ATTENTION, 0x28):
            ua_seen, waiting_ua = True, False
    if not absent_seen and not ua_seen:
        out.append(Finding('medium_change_not_seen', 'TUR', 'neither MEDIUM NOT PRESENT nor unit attention 28h'))
    if require_absent and not absent_seen:
        out.append(Finding('medium_absent_not_seen', 'TUR', 'no MEDIUM NOT PRESENT reply'))
    return out


def compare_reply(target, cdb, observed, data_out=b'', lun=0):
    """Differential oracle: run cdb on the model target (which advances its state) and compare with
    the observed (status, data_in[, sense]). Findings: reply_status, reply_data, reply_sense (key,
    ASC, ASCQ of a CHECK CONDITION, when the observed sense is given)."""
    want = target.execute(cdb, data_out, lun)
    status, data = observed[0], bytes(observed[1] or b'')
    sense = sense_triple(observed[2]) if len(observed) > 2 and observed[2] is not None else None
    name = f'opcode {bytes(cdb)[0]:#04x}' if cdb else 'empty CDB'
    out = []
    if status != want.status:
        out.append(Finding('reply_status', name, f'status {status:#04x}, model {want.status:#04x}'))
    if data != want.data_in:
        out.append(Finding('reply_data', name, f'{len(data)} bytes, model {len(want.data_in)}'))
    if want.status == CHECK_CONDITION and sense is not None and sense != sense_triple(want.sense):
        out.append(Finding('reply_sense', name, f'{sense} != {sense_triple(want.sense)}'))
    return out
