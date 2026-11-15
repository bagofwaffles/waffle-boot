#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""S-10a mass-storage model: an extension of tests/oracle/conformance/usb/scsi.py's Target with the further
commands, the stopped state and the host's acquire and release of the disk (S-10a.050). Synthetic content.
"""
import hashlib
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'conformance'))
from usb import descriptors as D  # noqa: E402
from usb import scsi as S  # noqa: E402

BLOCKS = 454656                       # S-12.001
BLOCK = 512
VENDOR, PRODUCT, REVISION = 'teenage ', 'OP-XY Disk', ''
SERIAL = 'XY0A12BC'                   # synthetic

READ12, WRITE12, MODE_SELECT10 = 0xA8, 0xAA, 0x55
SENSE_INVALID_FIELD_IN_PARAM = (S.ILLEGAL_REQUEST, 0x26, 0x00)

# commands that access the medium (S-10a.045): absent or stopped -> NOT READY 3Ah
MEDIA_OPS = {S.TEST_UNIT_READY, S.READ6, S.WRITE6, S.READ10, S.WRITE10, READ12, WRITE12, S.READ_CAPACITY10,
             S.READ_FORMAT_CAPACITIES, S.VERIFY10, S.SYNCHRONIZE_CACHE10, S.MODE_SENSE6, S.MODE_SENSE10}


def vpd_pages(serial):
    """S-10a.042: page 80h is the serial; page 83h one T10 vendor-ID designator."""
    ident = ('teenage engineering OP-XY Disk ' + serial).encode('ascii')
    return {0x80: serial.encode('ascii'), 0x83: S.designation_descriptor(1, ident, code_set=2)}


class Unit(S.Target):
    def __init__(self, serial=SERIAL, present=True):
        dev = S.BlockDevice(BLOCKS, BLOCK, present=False, removable=True)
        super().__init__(dev, vendor=VENDOR, product=PRODUCT, revision=REVISION, peripheral=0x00, rmb=True,
                         version=0x02, response_format=0x02, vpd=vpd_pages(serial), mode_pages={},
                         block_descriptor=True, sense_valid='auto', unit_attention=True, max_lun=0)
        self.stopped = False
        self.acquired = False
        self.releases = 0             # count of releases the host made (S-10a.050)
        if present:
            self.offer()

    # -- device side (S-10a.049)
    def offer(self):
        self.stopped = False
        self.acquired = False
        self.set_present(True)

    def withdraw(self, own_write=False):
        """S-10a.049/.050: withdraw for a scan or host-link transfer (releases a held disk) or for the
        bootloader's own write (S-12.018: ends the hold, no release)."""
        if not own_write:
            self._release()
        self.pending_ua = False
        self.acquired = False
        self.set_present(False)

    # -- bus side (S-10a.050)
    def bus_reset(self):
        self._release()
        self.stopped = False

    def deconfigure(self):
        """SET_CONFIGURATION(0): a held disk is released (S-10a.050)."""
        self._release()

    def select_configuration(self):
        """SET_CONFIGURATION, nonzero, current value included (S-10a.032): abandons a command in progress,
        releases a held disk, makes a stopped unit ready; keeps offer state, sense and unit attention."""
        self._release()
        self.stopped = False

    def _release(self):
        if self.acquired:
            self.acquired = False
            self.releases += 1

    def execute(self, cdb, data_out=b'', lun=0):
        cdb = bytes(cdb)
        if not cdb or len(cdb) < S.cdb_length(cdb[0]):
            return super().execute(cdb, data_out, lun)
        op = cdb[0]
        if op in MEDIA_OPS and self.dev.present and self.stopped and not self.pending_ua:
            return self._fail(S.SENSE_MEDIUM_NOT_PRESENT)
        if op in MEDIA_OPS and not self.dev.present:
            return self._fail(S.SENSE_MEDIUM_NOT_PRESENT)
        r = self._execute(cdb, data_out, lun)
        if op in MEDIA_OPS and r.status == S.GOOD:
            self.acquired = True
        return r

    def _execute(self, cdb, data_out, lun):
        op = cdb[0]
        if op in (S.REQUEST_SENSE, S.INQUIRY):
            return super().execute(cdb, data_out, lun)
        if self.pending_ua:
            self.pending_ua = False
            return self._fail(S.SENSE_MEDIUM_CHANGED)
        if op == S.START_STOP_UNIT:
            power, loej, start = cdb[4] >> 4, bool(cdb[4] & 2), bool(cdb[4] & 1)
            if power:
                return self._ok()
            if start:
                if not self.dev.present:
                    return self._fail(S.SENSE_MEDIUM_NOT_PRESENT)
                self.stopped = False
                return self._ok()
            self.stopped = True
            if loej:
                self._release()
            return self._ok()
        if op == S.PREVENT_ALLOW:
            return self._ok()
        if op in (S.MODE_SELECT6, MODE_SELECT10):
            n = cdb[4] if op == S.MODE_SELECT6 else struct.unpack_from('>H', cdb, 7)[0]
            return self._ok(b'', n)
        if op == S.MODE_SENSE10:
            if cdb[3]:
                return self._fail(S.SENSE_INVALID_FIELD_IN_CDB)
            pc, page, dbd = cdb[2] >> 6, cdb[2] & 0x3F, bool(cdb[1] & 8)
            d6, err = self.mode_sense6_data(dbd, pc, page)
            if err:
                return self._fail(err)
            bd = d6[4:]
            body = bytes([0, d6[2], 0, 0]) + struct.pack('>H', len(bd)) + bd
            data = struct.pack('>H', len(body)) + body
            return self._ok(data[:struct.unpack_from('>H', cdb, 7)[0]])
        if op == S.READ_FORMAT_CAPACITIES:
            data = bytes([0, 0, 0, 8]) + struct.pack('>I', BLOCKS) + bytes([2]) + BLOCK.to_bytes(3, 'big')
            return self._ok(data[:struct.unpack_from('>H', cdb, 7)[0]])
        if op == S.SYNCHRONIZE_CACHE10:
            return self._ok()
        if op == S.VERIFY10:
            if cdb[1] & 0x02:                               # BYTCHK: no compare offered
                return self._fail(S.SENSE_INVALID_FIELD_IN_CDB)
            lba, n = struct.unpack_from('>I', cdb, 2)[0], struct.unpack_from('>H', cdb, 7)[0]
            if lba + n > BLOCKS:
                return self._fail(S.SENSE_LBA_OUT_OF_RANGE, info=lba)
            return self._ok()
        if op in (READ12, WRITE12):
            lba, n = struct.unpack_from('>I', cdb, 2)[0], struct.unpack_from('>I', cdb, 6)[0]
            ten = bytes([S.READ10 if op == READ12 else S.WRITE10, cdb[1]]) + struct.pack('>I', lba) + b'\0' + \
                struct.pack('>H', min(n, 0xFFFF)) + b'\0'
            if n > 0xFFFF:                                  # beyond the 10-byte form: same rules, longer length
                if cdb[1] & 0xE0:
                    return self._fail(S.SENSE_INVALID_FIELD_IN_CDB)
                if lba + n > BLOCKS:
                    return self._fail(S.SENSE_LBA_OUT_OF_RANGE, info=lba)
                if op == READ12:
                    return self._ok(self.dev.read(lba, n))
                need = n * BLOCK
                if len(data_out) < need:
                    return S.Reply(S.GOOD, b'', need, self.sense_bytes())
                self.dev.write(lba, data_out[:need])
                return self._ok(b'', need)
            return super().execute(ten, data_out, lun)
        return super().execute(cdb, data_out, lun)


# ---------------------------------------------------------------- CDB helpers for the vectors
def cdb_start_stop(loej, start, power=0):
    return bytes([S.START_STOP_UNIT, 0, 0, 0, (power << 4) | (2 if loej else 0) | (1 if start else 0), 0])


def cdb_prevent(prevent):
    return bytes([S.PREVENT_ALLOW, 0, 0, 0, 1 if prevent else 0, 0])


def cdb_read_capacity10(lba, pmi):
    return struct.pack('>BBIBBBB', S.READ_CAPACITY10, 0, lba, 0, 0, 1 if pmi else 0, 0)


def cdb_rfc(alloc=0xFC):
    return bytes([S.READ_FORMAT_CAPACITIES, 0, 0, 0, 0, 0, 0]) + struct.pack('>H', alloc) + b'\0'


def cdb_mode_sense10(page=0x3F, pc=0, dbd=False, alloc=0xFF):
    return bytes([S.MODE_SENSE10, 8 if dbd else 0, (pc << 6) | page, 0, 0, 0, 0]) + struct.pack('>H', alloc) + b'\0'


def cdb_mode_select6(n):
    return bytes([S.MODE_SELECT6, 0x10, 0, 0, n, 0])


def cdb_sync():
    return bytes([S.SYNCHRONIZE_CACHE10]) + bytes(9)


def cdb_verify(lba, n, bytchk=False):
    return struct.pack('>BBIBHB', S.VERIFY10, 2 if bytchk else 0, lba, 0, n, 0)


def cdb_read12(lba, n):
    return struct.pack('>BBIIBB', READ12, 0, lba, n, 0, 0)


def cdb_write12(lba, n):
    return struct.pack('>BBIIBB', WRITE12, 0, lba, n, 0, 0)


def pattern(n_blocks, seed):
    return bytes((seed + i * 7) & 0xFF for i in range(n_blocks * BLOCK))


def _step(unit, name, cdb, data_out=b''):
    r = unit.execute(cdb, data_out)
    sense = S.sense_triple(r.sense) if r.status == S.CHECK_CONDITION else None
    d = dict(name=name, cdb=cdb.hex(), status=r.status, data_in_len=len(r.data_in), data_out_len=r.data_out_len,
             sense=list(sense) if sense else None)
    if len(data_out) <= 1024:
        d['data_out'] = data_out.hex()
    else:
        d['data_out_sha256'] = hashlib.sha256(data_out).hexdigest()
    if len(r.data_in) <= 1024:
        d['data_in'] = r.data_in.hex()
    else:
        d['data_in_sha256'] = hashlib.sha256(r.data_in).hexdigest()
    return d


def scsi_vector():
    """Command sequences on a unit that is offered, with the first unit attention consumed first."""
    seqs = {}
    u = Unit()
    steps = [_step(u, 'first command after the offer: unit attention', S.cdb_tur()),
             _step(u, 'then ready', S.cdb_tur()),
             _step(u, 'standard INQUIRY 36', S.cdb_inquiry(36)),
             _step(u, 'standard INQUIRY cut to 5', S.cdb_inquiry(5)),
             _step(u, 'standard INQUIRY 96 (36 owed)', S.cdb_inquiry(96)),
             _step(u, 'standard INQUIRY 261: allocation length in bytes 3-4', S.cdb_inquiry(0x0105)),
             _step(u, 'standard INQUIRY with CMDDT set: ignored', bytes([S.INQUIRY, 0x02, 0, 0, 36, 0])),
             _step(u, 'VPD 00h', S.cdb_inquiry(255, True, 0x00)),
             _step(u, 'VPD 80h', S.cdb_inquiry(255, True, 0x80)),
             _step(u, 'VPD 83h', S.cdb_inquiry(255, True, 0x83)),
             _step(u, 'VPD B0h not offered', S.cdb_inquiry(255, True, 0xB0)),
             _step(u, 'REQUEST SENSE after it', S.cdb_request_sense(18)),
             _step(u, 'INQUIRY page code without EVPD', S.cdb_inquiry(36, False, 0x80)),
             _step(u, 'READ CAPACITY(10)', S.cdb_read_capacity10()),
             _step(u, 'READ CAPACITY(10) LBA 1, PMI 0: refused', cdb_read_capacity10(1, False)),
             _step(u, 'REQUEST SENSE: READ CAPACITY(10) LBA without PMI', S.cdb_request_sense(18)),
             _step(u, 'READ CAPACITY(10) LBA 1, PMI 1', cdb_read_capacity10(1, True)),
             _step(u, 'READ FORMAT CAPACITIES', cdb_rfc()),
             _step(u, 'MODE SENSE(6) all pages', S.cdb_mode_sense6(0x3F)),
             _step(u, 'MODE SENSE(6) all pages, DBD', S.cdb_mode_sense6(0x3F, dbd=True)),
             _step(u, 'MODE SENSE(6) caching page not offered', S.cdb_mode_sense6(0x08)),
             _step(u, 'MODE SENSE(6) informational exceptions not offered', S.cdb_mode_sense6(0x1C)),
             _step(u, 'MODE SENSE(6) saved values', S.cdb_mode_sense6(0x3F, pc=3)),
             _step(u, 'MODE SENSE(10) all pages', cdb_mode_sense10(0x3F)),
             _step(u, 'MODE SENSE(10) all pages, DBD', cdb_mode_sense10(0x3F, dbd=True)),
             _step(u, 'MODE SELECT(6) accepted and ignored', cdb_mode_select6(12), bytes(12)),
             _step(u, 'PREVENT', cdb_prevent(True)),
             _step(u, 'ALLOW', cdb_prevent(False)),
             _step(u, 'SYNCHRONIZE CACHE(10)', cdb_sync()),
             _step(u, 'VERIFY(10)', cdb_verify(0, 8)),
             _step(u, 'VERIFY(10) BYTCHK', cdb_verify(0, 8, True)),
             _step(u, 'WRITE(10) 2 blocks at 100', S.cdb_write10(100, 2), pattern(2, 1)),
             _step(u, 'READ(10) them back', S.cdb_read10(100, 2)),
             _step(u, 'WRITE(6) with a 21-bit LBA', S.cdb_write6(0x10100, 1), pattern(1, 9)),
             _step(u, 'READ(10) at 0x10100', S.cdb_read10(0x10100, 1)),
             _step(u, 'READ(6) length 0 = 256 blocks', bytes([S.READ6, 0, 0, 0, 0, 0])),
             _step(u, 'READ(12)', cdb_read12(100, 2)),
             _step(u, 'WRITE(12)', cdb_write12(454654, 2), pattern(2, 3)),
             _step(u, 'READ(10) of the last two blocks', S.cdb_read10(454654, 2)),
             _step(u, 'READ(10) past the end', S.cdb_read10(454655, 2)),
             _step(u, 'REQUEST SENSE: LBA out of range', S.cdb_request_sense(18)),
             _step(u, 'READ(10) length 0', S.cdb_read10(0, 0)),
             _step(u, 'READ(10) RDPROTECT', bytes([S.READ10, 0x20]) + bytes(8)),
             _step(u, 'FORMAT UNIT unsupported', bytes([0x04, 0, 0, 0, 0, 0])),
             _step(u, 'RESERVE(6) unsupported', bytes([0x16, 0, 0, 0, 0, 0])),
             _step(u, 'WRITE BUFFER unsupported', bytes([0x3B]) + bytes(9)),
             _step(u, 'REPORT LUNS unsupported', bytes([0xA0]) + bytes(11)),
             _step(u, 'REQUEST SENSE: invalid opcode', S.cdb_request_sense(18)),
             _step(u, 'REQUEST SENSE when there is none', S.cdb_request_sense(18)),
             _step(u, 'REQUEST SENSE cut to 8', S.cdb_request_sense(8))]
    seqs['offered'] = steps
    # an unprovisioned unit (S-10a.004: no serial): page 80h is empty, page 83h ends with the separator
    u = Unit(serial='')
    seqs['no_serial'] = [_step(u, 'VPD 00h', S.cdb_inquiry(255, True, 0x00)),
                         _step(u, 'VPD 80h, no serial', S.cdb_inquiry(255, True, 0x80)),
                         _step(u, 'VPD 83h, no serial', S.cdb_inquiry(255, True, 0x83)),
                         _step(u, 'VPD 83h cut to 8', S.cdb_inquiry(8, True, 0x83))]
    return seqs


def medium_vector():
    """S-10a.047-.051: the medium's states as a host sees them."""
    out = {}
    # never offered (the battery-wait set, S-10a.051)
    u = Unit(present=False)
    out['no_medium'] = [_step(u, 'INQUIRY', S.cdb_inquiry(36)),
                        _step(u, 'TEST UNIT READY', S.cdb_tur()),
                        _step(u, 'READ CAPACITY(10)', S.cdb_read_capacity10()),
                        _step(u, 'READ(10)', S.cdb_read10(0, 1)),
                        _step(u, 'MODE SENSE(6)', S.cdb_mode_sense6(0x3F)),
                        _step(u, 'START STOP UNIT start', cdb_start_stop(False, True)),
                        _step(u, 'REQUEST SENSE', S.cdb_request_sense(18)),
                        _step(u, 'TEST UNIT READY again', S.cdb_tur())]
    # withdrawal and re-offer (S-10a.049, S-12.018)
    u = Unit()
    seq = [_step(u, 'first TUR: unit attention', S.cdb_tur()),
           _step(u, 'TUR ready', S.cdb_tur()),
           _step(u, 'READ(10) acquires', S.cdb_read10(0, 1))]
    u.withdraw()
    seq += [dict(name='event: withdraw', event='withdraw'), dict(name='released', releases=u.releases),
            _step(u, 'TUR while withdrawn', S.cdb_tur()),
            _step(u, 'READ(10) while withdrawn', S.cdb_read10(0, 1)),
            _step(u, 'INQUIRY while withdrawn', S.cdb_inquiry(36)),
            _step(u, 'TUR while withdrawn', S.cdb_tur())]
    u.offer()
    seq += [dict(name='event: offer', event='offer'),
            _step(u, 'TUR after the re-offer: unit attention', S.cdb_tur()),
            _step(u, 'TUR ready', S.cdb_tur())]
    out['withdraw_reoffer'] = seq
    # a withdrawal for the bootloader's own write (S-12.018) while held: no release
    u = Unit()
    seq = [_step(u, 'TUR: unit attention', S.cdb_tur()), _step(u, 'READ(10) acquires', S.cdb_read10(0, 1))]
    u.withdraw(own_write=True)
    seq += [dict(name='event: withdraw for an own write', event='withdraw_own_write'),
            dict(name='released', releases=u.releases), _step(u, 'TUR while withdrawn', S.cdb_tur())]
    u.offer()
    seq += [dict(name='event: offer', event='offer'),
            _step(u, 'TUR after the re-offer: unit attention', S.cdb_tur()),
            _step(u, 'READ(10) acquires again', S.cdb_read10(0, 1)), dict(name='released', releases=u.releases)]
    out['own_write_withdraw'] = seq
    # unit attention consumed by REQUEST SENSE
    u = Unit()
    out['ua_by_request_sense'] = [_step(u, 'REQUEST SENSE reports the unit attention', S.cdb_request_sense(18)),
                                  _step(u, 'TUR ready', S.cdb_tur())]
    # host eject and start (S-10a.050)
    u = Unit()
    seq = [_step(u, 'TUR: unit attention', S.cdb_tur()), _step(u, 'TUR ready (acquired)', S.cdb_tur()),
           _step(u, 'START STOP UNIT eject', cdb_start_stop(True, False))]
    seq.append(dict(name='released', releases=u.releases))
    seq += [_step(u, 'TUR after eject', S.cdb_tur()), _step(u, 'INQUIRY after eject', S.cdb_inquiry(36)),
            _step(u, 'READ(10) after eject', S.cdb_read10(0, 1)),
            _step(u, 'START STOP UNIT start', cdb_start_stop(False, True)),
            _step(u, 'TUR ready again', S.cdb_tur())]
    seq.append(dict(name='released', releases=u.releases))
    out['eject_start'] = seq
    # stop without eject: stopped but not released
    u = Unit()
    seq = [_step(u, 'TUR: unit attention', S.cdb_tur()), _step(u, 'TUR ready (acquired)', S.cdb_tur()),
           _step(u, 'START STOP UNIT stop', cdb_start_stop(False, False)),
           _step(u, 'TUR while stopped', S.cdb_tur())]
    seq.append(dict(name='released', releases=u.releases))
    seq += [_step(u, 'START STOP UNIT power condition (ignored)', cdb_start_stop(False, False, power=1)),
            _step(u, 'START STOP UNIT start', cdb_start_stop(False, True)), _step(u, 'TUR', S.cdb_tur())]
    out['stop_start'] = seq
    # bus reset after acquisition releases; before acquisition does not
    u = Unit()
    u.bus_reset()
    seq = [dict(name='event: bus reset before any access', event='bus_reset'),
           dict(name='released', releases=u.releases),
           _step(u, 'TUR: unit attention', S.cdb_tur()), _step(u, 'TUR ready (acquired)', S.cdb_tur())]
    u.bus_reset()
    seq += [dict(name='event: bus reset', event='bus_reset'), dict(name='released', releases=u.releases),
            _step(u, 'TUR after the reset', S.cdb_tur())]
    out['bus_reset'] = seq
    # stop and eject in the states No medium and Unit attention owed (S-10a.047, S-10a.048)
    u = Unit(present=False)
    seq = [_step(u, 'no medium: START STOP UNIT stop', cdb_start_stop(False, False)),
           _step(u, 'no medium: START STOP UNIT eject', cdb_start_stop(True, False)),
           dict(name='released', releases=u.releases)]
    u.offer()
    seq += [dict(name='event: offer', event='offer'),
            _step(u, 'unit attention owed: START STOP UNIT stop', cdb_start_stop(False, False)),
            _step(u, 'TUR ready: the stop was not done', S.cdb_tur())]
    u.withdraw()
    u.offer()
    seq += [dict(name='event: withdraw', event='withdraw'), dict(name='event: offer', event='offer'),
            _step(u, 'unit attention owed: START STOP UNIT eject', cdb_start_stop(True, False)),
            _step(u, 'TUR ready: the eject was not done', S.cdb_tur()),
            dict(name='released', releases=u.releases),
            _step(u, 'ready: START STOP UNIT stop', cdb_start_stop(False, False)),
            _step(u, 'stopped: START STOP UNIT eject', cdb_start_stop(True, False)),
            dict(name='released', releases=u.releases),
            _step(u, 'TUR still stopped', S.cdb_tur())]
    out['stop_eject_states'] = seq
    # SET_CONFIGURATION with the current value while the disk is held and the unit stopped (S-10a.032)
    u = Unit()
    seq = [_step(u, 'TUR: unit attention', S.cdb_tur()), _step(u, 'TUR ready (acquired)', S.cdb_tur()),
           _step(u, 'START STOP UNIT stop', cdb_start_stop(False, False))]
    u.select_configuration()
    seq += [dict(name='event: SET_CONFIGURATION, current value', event='set_configuration_same'),
            dict(name='released', releases=u.releases),
            _step(u, 'TUR ready again, no unit attention', S.cdb_tur())]
    u.deconfigure()
    seq += [dict(name='event: SET_CONFIGURATION(0)', event='set_configuration_0'),
            dict(name='released', releases=u.releases)]
    out['reselect'] = seq
    return out


def serial_rule(meta):
    """S-10a.004: up to 8 bytes, cut at the first NUL, bytes outside 20h..7Eh become '0'."""
    meta = meta[:8].split(b'\0')[0]
    if not meta:
        return None
    return ''.join(chr(c) if 0x20 <= c <= 0x7E else '0' for c in meta)


# The chapter 9 request model (S-10a.031-.034)
STALL = 'STALL'
UNIT_LOAD_MA = 100                                               # S-10a.032: USB 2.0 7.2.1

# The answers S-10a.031/.032 allow two of. A run uses one combination throughout.
EP0_CHOICES = {'status_unconfigured': (b'\x01\x00', b'\x00\x00'),  # GET_STATUS(device) before a configuration
               'remote_wakeup': ('stall', 'accept'),             # CLEAR/SET_FEATURE(DEVICE_REMOTE_WAKEUP)
               'alt_other': ('stall', 'accept')}                 # SET_INTERFACE with an alternate other than 0


def ep0_choice_sets():
    import itertools
    keys = sorted(EP0_CHOICES)
    for combo in itertools.product(*(EP0_CHOICES[k] for k in keys)):
        yield dict(zip(keys, combo))


class Ep0:
    """S-10a.031-.034 request model for one presented set: the control answers a host may expect.
    `choices` picks one allowed answer per EP0_CHOICES key."""

    def __init__(self, vec, choices=None):
        self.c = dict(status_unconfigured=b'\x01\x00', remote_wakeup='stall', alt_other='stall')
        self.c.update(choices or {})
        self.wakeup = False
        self.v = vec
        self.dev = bytes.fromhex(vec['device'])
        self.cfgs = [bytes.fromhex(c) for c in vec['configurations']]
        self.other = [bytes.fromhex(c) for c in vec['other_speed']]
        self.strings = {int(k): bytes.fromhex(x) for k, x in vec['string_bytes'].items()}
        self.config = 0
        self.halted = {}
        self.toggle = {}
        self.target_ma = None

    def ifaces(self):
        if not self.config:
            return set()
        return {a.number for a in D.parse_configuration(self.cfgs[self.config - 1]).alternates}

    def eps(self):
        if not self.config:
            return set()
        return {e.fields['bEndpointAddress'] for e in D.parse_configuration(self.cfgs[self.config - 1]).endpoints()}

    def _enable(self, eps):
        for e in eps:
            self.halted[e], self.toggle[e] = False, 0

    def bus_reset(self):                                         # S-10a.080
        self.config, self.halted, self.toggle, self.wakeup = 0, {}, {}, False
        self.target_ma = UNIT_LOAD_MA

    def vbus_lost(self):                                         # S-10a.081
        self.bus_reset()

    def request(self, bm, req, value, index, length):
        rtype, recip = (bm >> 5) & 3, bm & 0x1F
        if rtype == 2:
            return STALL                                         # S-10a.034: every vendor request
        if rtype == 1:
            return STALL if recip != 1 or index & 0xFF not in self.ifaces() else 'class'
        if req == 6 and bm == 0x80:                              # GET_DESCRIPTOR
            t, i = value >> 8, value & 0xFF
            if t == 1:
                data = self.dev
            elif t == 6:
                data = bytes.fromhex(self.v['qualifier'])
            elif t in (2, 7):
                src = self.cfgs if t == 2 else self.other
                if i >= len(src):
                    return STALL
                data = src[i]
            elif t == 3:
                if i not in self.strings:
                    return STALL
                data = self.strings[i]
            else:
                return STALL
            return data[:length]
        if req == 0 and bm & 0x80:                               # GET_STATUS
            if recip == 0:
                st = b'\x01\x00' if self.config else self.c['status_unconfigured']
                return bytes([st[0] | (2 if self.wakeup else 0), 0])
            if recip == 1:
                return b'\0\0' if index in self.ifaces() else STALL
            if recip == 2:
                if index & 0x7F == 0:
                    return b'\0\0'
                return bytes([int(self.halted[index]), 0]) if index in self.eps() else STALL
        if req in (1, 3) and not bm & 0x80:                      # CLEAR / SET_FEATURE
            if recip == 2 and value == 0:
                if index & 0x7F == 0:
                    return b''
                if index not in self.eps():
                    return STALL
                self.halted[index] = req == 3
                if req == 1:
                    self.toggle[index] = 0
                return b''
            if recip == 0 and req == 3 and value == 2:
                return b''                                      # TEST_MODE accepted
            if recip == 0 and value == 1:                        # DEVICE_REMOTE_WAKEUP
                if self.c['remote_wakeup'] == 'stall':
                    return STALL
                self.wakeup = req == 3
                return b''
            return STALL
        if req == 9 and bm == 0:                                 # SET_CONFIGURATION
            if value == 0:
                self.config = 0
                self.target_ma = UNIT_LOAD_MA
                return b''
            if not 1 <= value <= len(self.cfgs):
                return STALL
            self.config = value                                  # the current value too: re-selected
            self._enable(self.eps())
            self.target_ma = 2 * D.parse_configuration(self.cfgs[value - 1]).fields['bMaxPower']
            return b''
        if req == 8 and bm == 0x80:                              # GET_CONFIGURATION
            return bytes([self.config])
        if req == 10 and bm == 0x81:                             # GET_INTERFACE
            return b'\0' if index in self.ifaces() else STALL
        if req == 11 and bm == 0x01:                             # SET_INTERFACE (the target is not touched)
            if index not in self.ifaces():
                return STALL
            if value != 0:
                return STALL if self.c['alt_other'] == 'stall' else b''
            cfg = D.parse_configuration(self.cfgs[self.config - 1])
            self._enable(e.fields['bEndpointAddress'] for a in cfg.alternates if a.number == index for e in a.endpoints)
            return b''
        return STALL                                             # SET_DESCRIPTOR, SYNCH_FRAME, the rest
