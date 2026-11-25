# SPDX-License-Identifier: GPL-2.0-or-later
"""S-10a USB device logic: src/core/usb/desc.c, usb/ch9.c, usb/msc_scsi.c and usb/msc_bot.c (through h_s10a)
against conformance/usb and tests/oracle/S-10a-usb-msc (vectors, s10a_model)."""

import functools
import hashlib
import json
import random
import struct
import sys
import unittest
import zlib

from harness import hx, requirement, run, scenario, spec_module, spec_path, unhex
from conformance.usb import descriptors as D
from conformance.usb import expected as E
from conformance.usb import scsi as S


def vectors(name):
    with open(spec_path('S-10a-usb-msc', 'vectors', name + '.json'), encoding='utf-8') as f:
        return json.load(f)


DESC = vectors('descriptors')
SCSI = vectors('scsi')
MEDIUM = vectors('medium')
SERIALS = vectors('serial')


@functools.lru_cache(maxsize=None)
def spec_test():
    """S-10a's model (s10a_model: the unit, Ep0, serial_rule). It imports usb by its bare name from its
    own path; the path and those names are put back once it is loaded."""
    saved_path = list(sys.path)
    bare = ('gen_vectors', 's10a_model', 'usb', 'usb.descriptors', 'usb.scsi', 'usb.expected', 'usb.mtp')
    saved = {k: sys.modules.pop(k) for k in bare if k in sys.modules}
    try:
        return spec_module('S-10a-usb-msc', 's10a_model')
    finally:
        sys.path[:] = saved_path
        for k in bare:
            sys.modules.pop(k, None)
        sys.modules.update(saved)


def model():
    return spec_test()


SETS = ['midi', 'midi_cdc', 'msc', 'msc_cdc', 'mtp', 'mtp_vendor', 'cdc', 'mtp_cdc',     # enum s10a_set
        'mtp_vendor_cdc']
SET_NO = {n: i for i, n in enumerate(SETS)}
FN_MIDI, FN_CDC, FN_MSC, FN_MTP, FN_MTP_VENDOR = range(5)                       # enum s10a_function
SERIAL = 'XY0A12BC'
BLOCKS, BLOCK = 454656, 512
STALL = 'STALL'


def ser(text):
    return text.encode('ascii').hex() if text else '-'


def pattern(n, seed):
    """h_s10a's pat:SEED: n bytes, SEED + 7 * i each (s10a_model.pattern's bytes)."""
    return bytes((seed + 7 * i) & 0xFF for i in range(n))


def take_data(t, i):
    """A DATA field of h_s10a at t[i]: (bytes or ('crc', length, crc32), the next index)."""
    if t[i] == 'crc':
        return ('crc', int(t[i + 1]), int(t[i + 2], 16)), i + 3
    return unhex(t[i]), i + 1


def same_data(got, want):
    want = bytes(want)
    if isinstance(got, tuple):
        return got[1] == len(want) and got[2] == zlib.crc32(want)
    return got == want


def show(d):
    return d if isinstance(d, tuple) else d.hex()


# ================================================================== descriptors

def variants():
    for key in sorted(DESC):
        name, speed, variant = key.split('/')
        yield key, name, speed, variant == 'provisioned', DESC[key]


@functools.lru_cache(maxsize=None)
def device_descriptors():
    """{key: {'device', 'qualifier', 'configs', 'other', 'past', 'strings', 'unknown'}} as h_s10a gives
    them for every set, speed and serial variant of descriptors.json."""
    cmds, slots = [], []
    for key, name, speed, has, v in variants():
        sn, hi, text = SET_NO[name], int(speed == 'high'), SERIAL if has else ''
        n = len(v['configurations'])
        top = max(int(i) for i in v['string_bytes'])

        def add(c, what, k=key):
            cmds.append(c)
            slots.append((k, what))
        add('device %d %d' % (sn, has), 'device')
        add('qualifier %d' % sn, 'qualifier')
        for i in range(n):
            add('config %d %d %d %d 0' % (sn, has, i, hi), ('configs', i))
            add('config %d %d %d %d 1' % (sn, has, i, hi), ('other', i))
        add('config %d %d %d %d 0' % (sn, has, n, hi), 'past')
        for i in range(top + 1):
            add('string %d %s %d' % (sn, ser(text), i), ('strings', i))
        for i in (top + 1, 0xEE, 0xFF):
            add('string %d %s %d' % (sn, ser(text), i), ('unknown', i))
    out = run('s10a', cmds)
    got = {}
    for (key, what), o in zip(slots, out):
        g = got.setdefault(key, {'configs': {}, 'other': {}, 'strings': {}, 'unknown': {}})
        if isinstance(what, tuple):
            g[what[0]][what[1]] = unhex(o)
        else:
            g[what] = unhex(o)
    return got


def function_of(cls):
    if cls[0] == 1:
        return FN_MIDI
    if cls[:2] in ((2, 2), (0x0A, 0)):
        return FN_CDC
    return {(8, 6, 0x50): FN_MSC, (6, 1, 1): FN_MTP, (0xFF, 1, 1): FN_MTP_VENDOR}[cls]


class Descriptors(unittest.TestCase):

    @scenario('S-10a/SC-10A-ENUM')
    @requirement('S-10a.001', 'S-10a.002', 'S-10a.003', 'S-10a.005', 'S-10a.012', 'S-10a.013',
                 'S-10a.014', 'S-10a.015', 'S-10a.016', 'S-10a.017', 'S-10a.018', 'S-10a.019',
                 'S-10a.020', 'S-10a.021')
    def test_SC_10A_ENUM_vectors(self):
        """Every descriptor of every set, speed and serial variant byte for byte; an index past the
        last configuration and an unknown string index (0xEE included) are refused (length 0)."""
        got = device_descriptors()
        for key, name, speed, has, v in variants():
            g = got[key]
            with self.subTest(key=key):
                self.assertEqual(g['device'].hex(), v['device'])
                self.assertEqual(g['qualifier'].hex(), v['qualifier'])
                self.assertEqual([g['configs'][i].hex() for i in range(len(v['configurations']))],
                                 v['configurations'])
                self.assertEqual([g['other'][i].hex() for i in range(len(v['other_speed']))],
                                 v['other_speed'])
                self.assertEqual(g['past'], b'')
                self.assertEqual({i: b.hex() for i, b in g['strings'].items() if b},
                                 {int(i): b for i, b in v['string_bytes'].items()})
                self.assertEqual(set(g['unknown'].values()), {b''})

    @scenario('S-10a/SC-10A-ENUM')
    @requirement('S-10a.003', 'S-10a.005', 'S-10a.012', 'S-10a.021')
    def test_SC_10A_ENUM_oracles(self):
        """conformance/usb's chapter-9 and class rules find no problem in what the device builds: the
        device and qualifier, each configuration at its speed and in its other-speed form at the other
        speed; every string index named resolves to a string (S-10a.005), each with the text the
        vector gives, under LANGID 0x0409."""
        got = device_descriptors()
        for key, name, speed, has, v in variants():
            g, other = got[key], 'full' if speed == 'high' else 'high'
            with self.subTest(key=key):
                self.assertEqual(D.check_device(g['device'], speed), [])
                self.assertEqual(D.check_qualifier(g['qualifier'], g['device']), [])
                named = {D.parse_device(g['device'])[f] for f in ('iManufacturer', 'iProduct', 'iSerialNumber')}
                for i, raw in sorted(g['configs'].items()):
                    cfg = D.parse_configuration(raw)
                    self.assertEqual(D.check_configuration(cfg, speed), [], (i, D.tree_summary(cfg)))
                    self.assertEqual(E.match_configuration(raw, E.template_from_configuration(
                        bytes.fromhex(v['configurations'][i]))), [])
                    ocfg = D.parse_configuration(g['other'][i])
                    self.assertEqual(g['other'][i][1], 7)
                    self.assertEqual(D.check_configuration(ocfg, other), [], i)
                    named.add(cfg.fields['iConfiguration'])
                    named.update(a.fields['iInterface'] for a in cfg.alternates)
                    named.update(f['iFunction'] for _, f in cfg.iads)
                named.discard(0)
                strings = {i: b for i, b in g['strings'].items() if b}
                self.assertEqual(D.parse_string(strings[0], langids=True), [0x0409])
                self.assertEqual(set(strings) - {0}, named, 'each string named, none unnamed')
                for i, raw in strings.items():
                    if i:
                        self.assertEqual(E.match_string(raw, v['strings'][str(i)]), [], i)

    @requirement('S-10a.002', 'S-10a.011', 'S-10a.013', 'S-10a.020')
    def test_set_shape(self):
        """s10a_desc's accessors (what usb/ch9.c and usb/device.c route by) against the configurations
        they build: bNumConfigurations, bNumInterfaces, each interface's function and that function's
        first interface, bMaxPower of each configuration value (0 past the last), and the endpoints."""
        names = SETS
        cmds = ['shape %d' % SET_NO[n] for n in names]
        addrs = [a for a in range(1, 16)] + [0x80 | a for a in range(1, 16)]
        cmds += ['hasep %d %d' % (SET_NO[n], a) for n in names for a in addrs]
        out = run('s10a', cmds)
        has = dict(zip([(n, a) for n in names for a in addrs], out[len(names):]))
        for n, o in zip(names, out):
            v = DESC[n + '/high/provisioned']
            cfgs = [D.parse_configuration(bytes.fromhex(c)) for c in v['configurations']]
            t = o.split()
            nif = cfgs[0].fields['bNumInterfaces']
            want_fn = []
            first = {}
            for a in cfgs[0].alternates:
                fn = function_of(a.cls)
                iad = [f for _, f in cfgs[0].iads
                       if f['bFirstInterface'] <= a.number < f['bFirstInterface'] + f['bInterfaceCount']]
                first.setdefault(fn, iad[0]['bFirstInterface'] if iad else a.number)
                want_fn.append('%d:%d' % (fn, first[fn]))
            powers = [c.fields['bMaxPower'] for c in cfgs] + [0] * (4 - len(cfgs))
            with self.subTest(set=n):
                self.assertEqual(int(t[0]), D.parse_device(bytes.fromhex(v['device']))['bNumConfigurations'])
                self.assertEqual(int(t[1]), nif)
                self.assertEqual(t[2:2 + nif], want_fn)
                self.assertEqual([int(x) for x in t[2 + nif:]], powers)
                self.assertEqual(FN_CDC in [int(x.split(':')[0]) for x in want_fn],
                                 n in ('midi_cdc', 'msc_cdc', 'cdc', 'mtp_cdc', 'mtp_vendor_cdc'),
                                 'S-10a.011: the CDC port')
                eps = {e.fields['bEndpointAddress'] for c in cfgs for e in c.endpoints()}
                self.assertEqual({a for a in addrs if has[(n, a)] == '1'}, eps)

    @requirement('S-10a.004')
    def test_S10a_004_serial(self):
        """s10a_serial_from_meta: serial.json, and seeded metadata serials through s10a_model.serial_rule."""
        rule = spec_test().serial_rule
        rnd = random.Random(10004)
        metas = [bytes.fromhex(c['metadata_serial']) for c in SERIALS]
        metas += [b'', b'\0', b'AB\0CD', b'ABCDEFGHIJ', b'\x7f\x1f ~', bytes(range(0x7A, 0x84))]
        metas += [bytes(rnd.choice((0, 0x20, 0x41, 0x7E, 0x7F, 0x80, 0xFF, rnd.randrange(256)))
                        for _ in range(rnd.randrange(12))) for _ in range(200)]
        out = run('s10a', ['serial ' + hx(m) for m in metas])
        for c in SERIALS:
            self.assertEqual(unhex(out[metas.index(bytes.fromhex(c['metadata_serial']))]).decode('ascii'),
                             c['usb_serial'] or '')
        for m, o in zip(metas, out):
            self.assertEqual(unhex(o).decode('ascii'), rule(m) or '', m.hex())

    @requirement('S-10a.010', 'S-10a.011')
    def test_S10a_010_state_table(self):
        """s10a_set_for_state against S-10a.010's table: each state with the serial flag clear, set,
        and developer mode on (which sets the flag); the upgrade screen a host transfer opens starts no set."""
        # enum s10a_state: main, upgrade 40, upgrade, upgrade kept, battery, the factory menu's
        # vendor MTP item, its standard MTP item, both off again. FLAG: bit 0 the serial flag, bit 1
        # developer mode.
        table = [('midi', 'midi_cdc', 'midi_cdc'), ('midi', 'midi_cdc', 'midi_cdc'),
                 ('msc', 'msc_cdc', 'msc_cdc'), (None, None, None), ('msc', 'msc', 'msc'),
                 ('mtp_vendor', 'mtp_vendor', 'mtp_vendor_cdc'), ('mtp', 'mtp', 'mtp_cdc'),
                 ('cdc', 'cdc', 'cdc')]
        flags = (0, 1, 3)
        cmds = ['state %d %d' % (s, f) for s in range(len(table)) for f in flags]
        out = run('s10a', cmds)
        want = [table[s][i] for s in range(len(table)) for i in range(len(flags))]
        self.assertEqual(out, ['-' if w is None else str(SET_NO[w]) for w in want])


# ================================================================== chapter 9

def ch9_script(v):
    """SC-10A-CH9's requests for one set: (bmRequestType, bRequest, wValue, wIndex, wLength), or
    'busreset'."""
    cfgs = [D.parse_configuration(bytes.fromhex(c)) for c in v['configurations']]
    n = len(cfgs)
    ifaces = sorted({a.number for a in cfgs[0].alternates})
    eps = sorted({e.fields['bEndpointAddress'] for c in cfgs for e in c.endpoints()})
    absent = next(a for a in (0x85, 0x05, 0x8F, 0x0F) if a not in eps)
    top = max(int(i) for i in v['string_bytes'])
    no_if = max(ifaces) + 1
    r = []

    def s(bm, req, value, index, length):
        r.append((bm, req, value, index, length))

    def descriptors():
        for wl in (1, 2, 8, 18, 64, 255):
            s(0x80, 6, 0x0100, 0, wl)
        s(0x80, 6, 0x0600, 0, 10)
        for i in range(n + 1):
            total = len(bytes.fromhex(v['configurations'][i])) if i < n else 9
            for t in (2, 7):
                for wl in sorted({1, 8, 9, 64, 255, total, 4096}):
                    s(0x80, 6, t << 8 | i, 0, wl)
        for i in list(range(top + 1)) + [top + 1, 0xEE]:
            for lang in (0x0409, 0x0000):
                s(0x80, 6, 0x0300 | i, lang, 255)
            s(0x80, 6, 0x0300 | i, 0x0409, 2)
        for t in (4, 5, 9, 15, 0x21, 0x22):
            s(0x80, 6, t << 8, 0, 255)

    # the Default state
    s(0x80, 0, 0, 0, 2)
    s(0x80, 8, 0, 0, 1)
    s(0x81, 0, 0, ifaces[0], 2)
    s(0x81, 10, 0, ifaces[0], 1)
    s(0x01, 11, 0, ifaces[0], 0)
    for e in (0x00, 0x80, eps[0]):
        s(0x82, 0, 0, e, 2)
    s(0x02, 3, 0, eps[0], 0)
    descriptors()
    # SET_CONFIGURATION 0-4, GET_CONFIGURATION after each; the current value again
    for value in range(5):
        s(0x00, 9, value, 0, 0)
        s(0x80, 8, 0, 0, 1)
    s(0x00, 9, 1, 0, 0)
    s(0x00, 9, 1, 0, 0)
    s(0x80, 8, 0, 0, 1)
    s(0x80, 0, 0, 0, 2)
    descriptors()
    # interfaces
    for i in ifaces + [no_if]:
        s(0x81, 0, 0, i, 2)
        s(0x81, 10, 0, i, 1)
        s(0x01, 11, 0, i, 0)
        s(0x01, 11, 1, i, 0)
        s(0x81, 10, 0, i, 1)
        s(0x01, 3, 0, i, 0)                    # interface features
        s(0x01, 1, 0, i, 0)
    # endpoint halts, on each endpoint, endpoint 0 and one not in the set
    for e in eps + [0x00, 0x80, absent]:
        s(0x82, 0, 0, e, 2)
        s(0x02, 3, 0, e, 0)
        s(0x82, 0, 0, e, 2)
        s(0x02, 1, 0, e, 0)
        s(0x82, 0, 0, e, 2)
        s(0x02, 1, 0, e, 0)                    # clear when not halted
    # DEVICE_REMOTE_WAKEUP, each followed by GET_STATUS; TEST_MODE (selector 4, Test_Packet)
    s(0x00, 3, 1, 0, 0)
    s(0x80, 0, 0, 0, 2)
    s(0x00, 1, 1, 0, 0)
    s(0x80, 0, 0, 0, 2)
    s(0x00, 3, 2, 0x0400, 0)
    # SET_DESCRIPTOR, SYNCH_FRAME, unknown standard requests
    s(0x00, 7, 0x0100, 0, 18)
    s(0x82, 12, 0, eps[0], 2)
    s(0x80, 2, 0, 0, 2)
    s(0x00, 4, 0, 0, 0)
    # vendor requests of both directions and every recipient, wLength 0 and 64
    for bm in (0x40, 0xC0, 0x41, 0xC1, 0x42, 0xC2):
        for wl in (0, 64):
            s(bm, 1, 0, 0, wl)
    # class requests to each interface and to one not in the configuration; to the device and to
    # an endpoint
    for i in ifaces + [no_if]:
        s(0x21, 0x22, 0, i, 0)
        s(0xA1, 0xFE, 0, i, 1)
        s(0xA1, 0x21, 0, i, 7)
    s(0x20, 0x22, 0, 0, 0)
    s(0xA0, 0xFE, 0, 0, 1)
    s(0x22, 0x01, 0, eps[0], 0)
    # SET_CONFIGURATION(0) back to the Address state, then a bus reset
    s(0x00, 9, 0, 0, 0)
    s(0x81, 10, 0, ifaces[0], 1)
    s(0x00, 9, n, 0, 0)
    r.append('busreset')
    s(0x80, 8, 0, 0, 1)
    s(0x80, 0, 0, 0, 2)
    s(0x81, 0, 0, ifaces[0], 2)
    s(0x00, 9, 1, 0, 0)
    s(0x80, 8, 0, 0, 1)
    return r


def setup_hex(bm, req, value, index, length):
    return struct.pack('<BBHHH', bm, req, value, index, length).hex()


def device_answer(line):
    t = line.split()
    if t[0] == 'stall':
        return STALL
    if t[0] in ('accept', 'reselect'):
        return b''
    if t[0] == 'reply':
        return unhex(t[1])
    return ('forward', t[1])


def ch9_mismatches(ep0, script, out):
    """The requests where the device's answer (s10a_ch9_setup and the stack's part) is not the model's.
    A forwarded request is the stack's: GET_DESCRIPTOR is compared by its bytes, GET_STATUS of an
    endpoint by its length (the stack holds the halt), a class request counts as routed to its function,
    and anything else must complete with no data."""
    bad = []
    for req, line in zip(script, out):
        if req == 'busreset':
            ep0.bus_reset()
            continue
        bm, rq, value, index, length = req
        before = ep0.config
        want = ep0.request(bm, rq, value, index, length)
        got = device_answer(line)
        ok = True
        if want == STALL:
            ok = got == STALL or (rq == 6 and got == ('forward', 'stall'))
        elif want == 'class':
            ok = isinstance(got, tuple)
        elif isinstance(got, tuple):
            w = got[1]
            if rq == 6:
                ok = w == hx(want)
            elif rq == 0:
                ok = len(want) == 2 and w != 'stall' and len(unhex(w)) == 2
            else:
                ok = w == '-' and want == b''
        else:
            ok = got == want
        if line == 'reselect' and not (rq == 9 and value and value == before):
            ok = False
        if rq == 9 and bm == 0 and value and value == before and line != 'reselect':
            ok = False
        if not ok:
            bad.append('%s: device %s, model %r' % (setup_hex(*req), line, want))
    return bad


class Chapter9(unittest.TestCase):

    def run_script(self, key):
        v = DESC[key]
        name, speed, variant = key.split('/')
        script = ch9_script(v)
        cmds = ['ch9 %d %s %d' % (SET_NO[name], ser(SERIAL if variant == 'provisioned' else ''), speed == 'high')]
        cmds += ['busreset' if r == 'busreset' else 'setup ' + setup_hex(*r) for r in script]
        return v, script, run('s10a', cmds)[1:]

    @scenario('S-10a/SC-10A-CH9')
    @requirement('S-10a.031', 'S-10a.032', 'S-10a.033', 'S-10a.034')
    def test_SC_10A_CH9_request_table(self):
        """Every set at both speeds and both serial variants: SC-10A-CH9's requests against the request
        model Ep0. S-10a.031/.032 leave three answers free; the device must match one combination of
        them for the whole run (EP0_CHOICES)."""
        T = spec_test()
        for key in sorted(DESC):
            v, script, out = self.run_script(key)
            with self.subTest(key=key):
                results = [(ch9_mismatches(T.Ep0(v, c), script, out), c) for c in T.ep0_choice_sets()]
                best = min(results, key=lambda x: len(x[0]))
                self.assertEqual(best[0], [], 'closest choices %s' % best[1])

    @requirement('S-10a.031')
    def test_S10a_031_descriptor_cuts(self):
        """A forwarded GET_DESCRIPTOR of a configuration, cut at each wLength, still walks under
        parse_configuration (USB 2.0 9.4.3)."""
        for key in ('midi_cdc/high/provisioned', 'msc_cdc/full/no_serial', 'mtp/high/provisioned'):
            v = DESC[key]
            name, speed, variant = key.split('/')
            wls = (1, 2, 8, 9, 31, 64, 100, 255)
            reqs, totals = [], []
            for i in range(len(v['configurations'])):
                for t, src in ((2, 'configurations'), (7, 'other_speed')):
                    for wl in wls:
                        reqs.append((0x80, 6, t << 8 | i, 0, wl))
                        totals.append(len(bytes.fromhex(v[src][i])))
            out = run('s10a', ['ch9 %d - %d' % (SET_NO[name], speed == 'high')] +
                      ['setup ' + setup_hex(*r) for r in reqs])[1:]
            for r, total, o in zip(reqs, totals, out):
                t = o.split()
                self.assertEqual(t[0], 'forward', (key, r))
                data = unhex(t[1])
                self.assertEqual(len(data), min(r[4], total), (key, r))
                if len(data) >= 2:
                    D.parse_configuration(data, wlength=r[4])

    @requirement('S-10a.031')
    def test_S10a_031_set_address(self):
        """SET_ADDRESS is the stack's (USB 2.0 9.4.6) in the Default and Address states."""
        out = run('s10a', ['ch9 0 - 1', 'setup ' + setup_hex(0x00, 5, 7, 0, 0),
                           'setup ' + setup_hex(0x80, 5, 7, 0, 0)])
        self.assertEqual(out[1:], ['forward -', 'stall'])

    @requirement('S-10a.032', 'S-10a.020')
    def test_S10a_032_input_current(self):
        """The target usb/device.c sets on a selection is 2 x s10a_desc_max_power of the value selected:
        the model's 500, 100 and 2 mA (S-10a.032)."""
        T = spec_test()
        out = run('s10a', ['shape %d' % SET_NO[n] for n in SETS])
        for n, o in zip(SETS, out):
            v = DESC[n + '/high/provisioned']
            powers = [int(x) for x in o.split()[-4:]]
            ep0 = T.Ep0(v)
            for value in range(1, len(v['configurations']) + 1):
                ep0.request(0x00, 9, value, 0, 0)
                self.assertEqual(2 * powers[value - 1], ep0.target_ma, (n, value))

    @requirement('S-10a.033')
    def test_S10a_033_halt_while_reset_owed(self):
        """While the mass-storage function waits for a reset recovery, CLEAR_FEATURE(ENDPOINT_HALT) of
        either bulk endpoint is accepted and leaves it halted; GET_STATUS shows the halt; the other
        endpoints of a disk+CDC set are not held."""
        bad_cbw = S.encode_cbw(1, 0, False, 1, S.cdb_tur())          # LUN 1
        cmds = ['lu %s 1' % ser(SERIAL), 'ch9 %d %s 1' % (SET_NO['msc_cdc'], ser(SERIAL)),
                'setup ' + setup_hex(0x00, 9, 1, 0, 0), 'send ' + bad_cbw.hex(), 'eps']
        for e in (0x87, 0x08):
            cmds += ['setup ' + setup_hex(0x02, 1, 0, e, 0), 'setup ' + setup_hex(0x82, 0, 0, e, 2)]
        cmds += ['setup ' + setup_hex(0x02, 3, 0, 0x89, 0), 'setup ' + setup_hex(0x02, 1, 0, 0x89, 0),
                 'setup ' + setup_hex(0x82, 0, 0, 0x89, 2), 'eps']
        out = run('s10a', cmds)
        self.assertEqual(out[2], 'forward -')
        self.assertEqual(out[3], '1')
        self.assertEqual(out[4].split()[:5], ['1', '1', '0', '0', '1'])
        self.assertEqual(out[5:9], ['accept', 'forward 0100', 'accept', 'forward 0100'])
        self.assertEqual(out[9:12], ['forward -', 'forward -', 'forward 0000'])
        self.assertEqual(out[12].split()[:2], ['1', '1'])


# ================================================================== mass storage

INQ = dict(peripheral=0x00, rmb=True, version=0x02, response_format=0x02, vendor='teenage ',
           product='OP-XY Disk', revision='')
MEDIUM_DEV = S.BlockDevice(BLOCKS, BLOCK)


def alloc_of(cdb):
    op = cdb[0]
    if op == 0x12:
        return struct.unpack_from('>H', cdb, 3)[0]
    if op in (0x03, 0x1A):
        return cdb[4]
    if op in (0x23, 0x5A):
        return struct.unpack_from('>H', cdb, 7)[0]
    return None


def oracle_findings(cdb, data):
    """The conformance/usb reply checks that apply to a GOOD reply of `cdb`."""
    op, alloc = cdb[0], alloc_of(cdb)
    if op == 0x12 and cdb[1] & 1:
        if cdb[2] == 0x00:
            return S.decode_vpd00(data, alloc)[1]
        if cdb[2] == 0x83:
            return S.decode_vpd83(data, alloc)[1]
        return []
    if op == 0x12:
        return S.check_inquiry(data, alloc, **INQ)
    if op == 0x03:
        return S.check_request_sense(data, alloc)
    if op == 0x1A:
        return S.check_mode_sense6(data, alloc, MEDIUM_DEV)
    if op == 0x25:
        return S.check_read_capacity10(data, MEDIUM_DEV)
    return []


def sense_choices(want):
    """REQUEST SENSE data after ILLEGAL REQUEST 21h/00h: the information field is 0 or the first LBA
    (S-10a.043), so both forms pass."""
    want = bytes(want)
    out = [want]
    if len(want) >= 14 and want[2] & 0xF == 5 and want[12] == 0x21:
        alt = bytearray(want)
        alt[0] = 0x70
        alt[3:7] = bytes(4)
        out.append(bytes(alt))
    return out


def find_seed(n, sha):
    for seed in range(256):
        if hashlib.sha256(pattern(n, seed)).hexdigest() == sha:
            return seed
    raise AssertionError('no pattern seed gives ' + sha)


# The sequences of medium.json that s10a_model.medium_vector starts on a unit never offered.
NOT_OFFERED = {'no_medium', 'stop_eject_states'}


def unit_for(name):
    """The unit a vector sequence starts on: serial, offered."""
    if name in NOT_OFFERED:
        return SERIAL, False
    if name == 'no_serial':
        return '', True
    return SERIAL, True


class Unit(unittest.TestCase):

    def replay(self, name, seq):
        """A vector sequence on a new unit: each command, then the unit's state."""
        serial, offered = unit_for(name)
        cmds, steps = ['lu %s %d' % (ser(serial), offered)], []
        for s in seq:
            if 'event' in s:
                cmds.append('event ' + s['event'])
                steps.append((s, 1))
            elif 'releases' in s:
                cmds.append('lustate')
                steps.append((s, 1))
            else:
                if 'data_out_sha256' in s:
                    data = 'pat:%d' % find_seed(s['data_out_len'], s['data_out_sha256'])
                else:
                    data = s['data_out'] or '-'
                cmds += ['cmd %s %s' % (s['cdb'], data), 'lustate']
                steps.append((s, 2))
        out = run('s10a', cmds)[1:]
        i, after_21h = 0, False
        for s, k in steps:
            lines, i = out[i:i + k], i + k
            with self.subTest(seq=name, step=s['name']):
                if 'releases' in s:
                    self.assertEqual(int(lines[0].split()[4]), s['releases'])
                if 'cdb' not in s:
                    continue
                t = lines[0].split()
                status, direction, length = int(t[0]), int(t[1]), int(t[2])
                data, _ = take_data(t, 3)
                st = [int(x) for x in lines[1].split()]
                cdb = bytes.fromhex(s['cdb'])
                self.assertEqual(status, s['status'])
                if s['data_in_len']:
                    self.assertEqual((direction, length), (1, s['data_in_len']))
                    if 'data_in' in s:
                        allowed = sense_choices(bytes.fromhex(s['data_in'])) if after_21h and cdb[0] == 3 \
                            else [bytes.fromhex(s['data_in'])]
                        self.assertIn(data, allowed)
                    else:
                        self.assertIsInstance(data, tuple)
                        self.assertEqual(data[1], s['data_in_len'])
                    if status == S.GOOD and not isinstance(data, tuple):
                        self.assertEqual(oracle_findings(cdb, data), [])
                elif s['data_out_len']:
                    self.assertEqual((direction, length), (2, s['data_out_len']))
                else:
                    self.assertEqual(data, b'')
                if s['sense']:
                    self.assertEqual(st[5:8], s['sense'])
                elif status == S.GOOD:
                    self.assertEqual(st[5:8], [0, 0, 0], 'GOOD clears the sense (S-10a.043)')
                after_21h = cdb[0] != 3 and bool(s['sense']) and s['sense'][1] == 0x21

    @requirement('S-10a.042', 'S-10a.043', 'S-10a.044', 'S-10a.045', 'S-10a.046', 'S-10a.052')
    def test_scsi_vectors(self):
        """scsi.json (offered, no_serial) command by command: status, data, sense; each GOOD reply
        through conformance/usb's reply checks (check_inquiry, check_request_sense, check_mode_sense6,
        check_read_capacity10, decode_vpd00, decode_vpd83)."""
        for name, seq in sorted(SCSI.items()):
            self.replay(name, seq)

    @requirement('S-10a.043', 'S-10a.047', 'S-10a.048', 'S-10a.049', 'S-10a.050', 'S-10a.051')
    def test_medium_vectors(self):
        """medium.json: every state of S-10a.047, START STOP UNIT in each, the offer and withdrawal,
        the releases (S-10a.050) counted by the unit."""
        for name, seq in sorted(MEDIUM.items()):
            self.replay(name, seq)

    @requirement('S-10a.052')
    def test_S10a_052_large_reads(self):
        """READ(6) of 256 blocks and READ(12) of 0xFF blocks, against the model's bytes (crc) and the
        vector's sha256 of them."""
        M = model()
        step = next(s for s in SCSI['offered'] if s['name'].startswith('READ(6) length 0'))
        u = M.Unit()
        u.execute(S.cdb_tur())
        u.execute(S.cdb_write10(100, 2), M.pattern(2, 1))
        r = u.execute(bytes.fromhex(step['cdb']))
        self.assertEqual(hashlib.sha256(r.data_in).hexdigest(), step['data_in_sha256'])
        out = run('s10a', ['lu %s 1' % ser(SERIAL), 'cmd 000000000000 -',
                           'cmd %s pat:1' % S.cdb_write10(100, 2).hex(), 'cmd %s -' % step['cdb'],
                           'cmd %s -' % M.cdb_read12(0x10, 0xFF).hex()])
        data, _ = take_data(out[3].split(), 3)
        self.assertTrue(same_data(data, r.data_in))
        data, _ = take_data(out[4].split(), 3)
        self.assertTrue(same_data(data, u.execute(M.cdb_read12(0x10, 0xFF)).data_in))

    @requirement('S-10a.042')
    def test_S10a_042_vpd_offer(self):
        """INQUIRY with EVPD for every page code: the pages listed in page 00h answer with their own
        code, every other gets ILLEGAL REQUEST 24h/00h (check_vpd_offer); with and without a serial."""
        for serial in (SERIAL, ''):
            cmds = ['lu %s 1' % ser(serial)]
            for p in range(256):
                cmds += ['cmd %s -' % S.cdb_inquiry(255, True, p).hex(), 'lustate']
            out = run('s10a', cmds)[1:]
            replies = {}
            for p in range(256):
                t, st = out[2 * p].split(), [int(x) for x in out[2 * p + 1].split()]
                replies[p] = (int(t[0]), unhex(t[3]), tuple(st[5:8]))
            self.assertEqual(S.check_vpd_offer(replies[0][1], replies), [], serial)
            self.assertEqual(S.decode_vpd00(replies[0][1])[0], [0x00, 0x80, 0x83])
            descs, f = S.decode_vpd83(replies[0x83][1])
            self.assertEqual(f, [])
            self.assertEqual(descs[0].ident, ('teenage engineering OP-XY Disk ' + serial).encode('ascii'))

    @requirement('S-10a.046')
    def test_S10a_046_every_opcode(self):
        """Every operation code with a 16-byte zero CDB on a ready unit against s10a_model.Unit:
        status, data and sense (compare_reply)."""
        M = model()
        cmds = []
        for op in range(256):
            cmds += ['lu %s 1' % ser(SERIAL), 'cmd 000000000000 -',
                     'cmd %s -' % (bytes([op]) + bytes(15)).hex(), 'lustate']
        out = run('s10a', cmds)
        for op in range(256):
            u = M.Unit()
            u.execute(S.cdb_tur())
            t = out[4 * op + 2].split()
            data, _ = take_data(t, 3)
            st = [int(x) for x in out[4 * op + 3].split()]
            cdb = bytes([op]) + bytes(15)
            want = u.execute(cdb)
            with self.subTest(op='%02x' % op):
                self.assertEqual(int(t[0]), want.status)
                self.assertTrue(same_data(data, want.data_in), show(data))
                if want.status == S.CHECK_CONDITION:
                    self.assertEqual(tuple(st[5:8]), S.sense_triple(want.sense))

    def differential(self, plan, offered=True):
        """A plan of ('event', NAME) and ('cmd', CDB, DATA) on a new unit (serial, offered or not) and on
        s10a_model.Unit: each reply (status, data, sense) and the release count after each step. Returns
        the TEST UNIT READY replies (status, sense)."""
        M = model()
        cmds = ['lu %s %d' % (ser(SERIAL), offered)]
        for p in plan:
            if p[0] == 'event':
                cmds += ['event ' + p[1], 'lustate']
            else:
                cmds += ['cmd %s %s' % (p[1].hex(), p[2]), 'lustate']
        out = run('s10a', cmds)[1:]
        u = M.Unit(present=offered)
        after_21h = False
        tur = []
        for i, p in enumerate(plan):
            line, st = out[2 * i], [int(x) for x in out[2 * i + 1].split()]
            if p[0] == 'event':
                {'withdraw': u.withdraw, 'withdraw_own_write': lambda: u.withdraw(own_write=True),
                 'offer': u.offer, 'bus_reset': u.bus_reset,
                 'set_configuration_same': u.select_configuration, 'set_configuration_0': u.deconfigure}[p[1]]()
                self.assertEqual(st[4], u.releases, (i, p))
                continue
            _, cdb, arg = p
            want = u.execute(cdb, b'' if arg == '-' else pattern(rw_blocks(cdb) * BLOCK, int(arg[4:])))
            t = line.split()
            got, _ = take_data(t, 3)
            where = (i, cdb.hex())
            self.assertEqual(int(t[0]), want.status, where)
            if cdb[0] == 3 and after_21h:
                self.assertIn(got, sense_choices(want.data_in), where)
            else:
                self.assertTrue(same_data(got, want.data_in), (where, show(got)))
            if want.status == S.CHECK_CONDITION:
                self.assertEqual(tuple(st[5:8]), S.sense_triple(want.sense), where)
            self.assertEqual(st[4], u.releases, where)
            after_21h = cdb[0] != 3 and want.status == S.CHECK_CONDITION and \
                S.sense_triple(want.sense)[1] == 0x21
            if cdb[0] == 0:
                tur.append((int(t[0]), tuple(st[5:8])))
        return tur

    @scenario('S-10a/SC-10A-MEDIUM', 'S-10a/SC-10A-RELEASE')
    @requirement('S-10a.043', 'S-10a.045', 'S-10a.047', 'S-10a.048', 'S-10a.049', 'S-10a.050',
                 'S-10a.052')
    def test_media_walk(self):
        """A seeded walk through the disk's states: the bootloader's offers and withdrawals (for its own
        writes), bus resets and selections, and the host's media, sense and START STOP commands, each
        reply and the release count against s10a_model.Unit."""
        tur = self.differential(self.walk(('withdraw_own_write',)))
        self.assertEqual([f for f in S.check_medium_change(tur) if f.rule == 'tur_sense'], [])

    @staticmethod
    def walk(withdrawals):
        """test_media_walk's plan, each withdrawal one of `withdrawals`' events."""
        M = model()
        rnd = random.Random(10047)
        present = True
        plan = []
        for _ in range(1500):
            k = rnd.random()
            if k < 0.06:
                plan.append(('event', rnd.choice(withdrawals) if present else 'offer'))
                present = not present
                continue
            if k < 0.10:
                plan.append(('event', rnd.choice(('bus_reset', 'set_configuration_same', 'set_configuration_0'))))
                continue
            lba = rnd.choice((0, 1, 5, 100, 0x10100, BLOCKS - 2, BLOCKS - 1, BLOCKS))
            n = rnd.choice((1, 2, 3))
            pat = 'pat:%d' % rnd.randrange(256)
            plan.append(('cmd',) + rnd.choice((
                (S.cdb_tur(), '-'), (S.cdb_tur(), '-'), (S.cdb_inquiry(36), '-'),
                (S.cdb_request_sense(18), '-'), (S.cdb_request_sense(rnd.choice((0, 8, 252))), '-'),
                (S.cdb_read10(lba, n), '-'), (S.cdb_write10(lba, n), pat),
                (S.cdb_read6(lba & 0x1FFFFF, n), '-'), (S.cdb_write6(lba & 0x1FFFFF, n), pat),
                (M.cdb_read12(lba, n), '-'), (M.cdb_write12(lba, n), pat),
                (S.cdb_read_capacity10(), '-'), (S.cdb_mode_sense6(), '-'), (M.cdb_mode_sense10(), '-'),
                (M.cdb_rfc(), '-'), (M.cdb_verify(lba, n), '-'), (M.cdb_sync(), '-'),
                (M.cdb_start_stop(False, False), '-'), (M.cdb_start_stop(True, False), '-'),
                (M.cdb_start_stop(False, True), '-'), (M.cdb_start_stop(True, True), '-'),
                (M.cdb_start_stop(False, False, power=2), '-'), (M.cdb_prevent(True), '-'),
                (M.cdb_prevent(False), '-'))))
        return plan

    @requirement('S-10a.043', 'S-10a.047', 'S-10a.048', 'S-10a.049', 'S-10a.050')
    def test_state_edges(self):
        """The edges a walk seldom takes, against s10a_model.Unit: a withdrawal (for an own write) while
        the unit attention is owed drops it (REQUEST SENSE while withdrawn does not report it); a stop
        then a bus reset, a re-selection or an offer each make the unit ready; an eject before the host
        holds the disk releases nothing; REQUEST SENSE reports and consumes the unit attention."""
        plans = self.edges('withdraw_own_write')
        for i, plan in enumerate(plans):
            with self.subTest(plan=i):
                self.differential(plan)
        with self.subTest(plan='no medium'):
            self.differential(self.edges_no_medium(), offered=False)

    @staticmethod
    def edges(withdraw):
        """test_state_edges' plans, each withdrawal the event `withdraw`."""
        M = model()
        tur, rs, inq = ('cmd', S.cdb_tur(), '-'), ('cmd', S.cdb_request_sense(18), '-'), \
            ('cmd', S.cdb_inquiry(36), '-')
        stop, eject = ('cmd', M.cdb_start_stop(False, False), '-'), ('cmd', M.cdb_start_stop(True, False), '-')
        read = ('cmd', S.cdb_read10(7, 1), '-')
        ev = lambda name: ('event', withdraw if name == 'withdraw' else name)   # noqa: E731
        return [
            [ev('withdraw'), rs, tur, rs, inq, ev('offer'), rs, tur, rs],
            [ev('withdraw'), ev('offer'), ev('withdraw'), rs, ev('offer'), tur, tur],
            [rs, tur, stop, tur, ev('bus_reset'), tur, stop, ev('set_configuration_same'), tur,
             stop, ev('set_configuration_0'), tur, stop, ev('withdraw'), tur, ev('offer'), tur, tur],
            [tur, eject, tur, ('cmd', M.cdb_start_stop(False, True), '-'), read, eject, rs, read],
            [tur, read, stop, eject, ev('bus_reset'), read, ev('set_configuration_0'), read,
             ev('set_configuration_same'), read, ev('withdraw'), ev('bus_reset'), ev('offer'), read, read],
        ]

    @staticmethod
    def edges_no_medium():
        M = model()
        tur, rs = ('cmd', S.cdb_tur(), '-'), ('cmd', S.cdb_request_sense(18), '-')
        stop, eject = ('cmd', M.cdb_start_stop(False, False), '-'), ('cmd', M.cdb_start_stop(True, False), '-')
        return [stop, eject, rs, tur, ('event', 'offer'), stop, tur, tur, eject, tur]

    @scenario('S-10a/SC-10A-RELEASE')
    @requirement('S-10a.050')
    def test_S10a_050_scan_withdrawal_releases(self):
        """A scan's withdrawal while the host holds the disk is a release, an own write's is not:
        test_state_edges' plans and test_media_walk's walk with scan withdrawals (the walk's mixed with
        own writes), against s10a_model.Unit."""
        for i, plan in enumerate(self.edges('withdraw')):
            with self.subTest(plan=i):
                self.differential(plan)
        with self.subTest(plan='walk'):
            self.differential(self.walk(('withdraw', 'withdraw_own_write')))

    @scenario('S-10a/SC-10A-MEDIUM')
    @requirement('S-10a.049', 'S-10a.051')
    def test_SC_10A_MEDIUM_poll(self):
        """A host polling TEST UNIT READY: (0) the set starting with no medium, then the first offer;
        (a), (b) a withdrawal and re-offer while the host holds the disk; a withdrawal while the unit
        attention is still owed drops it. NOT READY 3Ah/00h throughout each withdrawal, then exactly one
        UNIT ATTENTION 28h/00h, then GOOD (check_medium_change with require_absent); INQUIRY answered
        throughout."""
        tur, inq = 'cmd 000000000000 -', 'cmd %s -' % S.cdb_inquiry(36).hex()
        script = ['lu %s 0' % ser(SERIAL), tur, inq, tur, 'event offer', tur, tur,
                  'cmd %s -' % S.cdb_read10(0, 1).hex(), tur,
                  'event withdraw', tur, inq, tur, tur, 'event offer', tur, inq, tur, tur,
                  'event withdraw', 'event offer', 'event withdraw', tur, 'event offer', tur, tur]
        cmds = []
        for c in script:
            cmds += [c, 'lustate']
        out = run('s10a', cmds)
        polls, inqs, phases, phase = [], [], [], 0
        for c, o, st in zip(script, out[0::2], out[1::2]):
            if c == 'event offer':
                phases.append(polls[phase:])
                phase = len(polls)
            if c == tur:
                polls.append((int(o.split()[0]), tuple(int(x) for x in st.split()[5:8])))
            elif c == inq:
                inqs.append(o.split()[0])
        phases.append(polls[phase:])
        self.assertEqual(set(inqs), {'0'})
        self.assertEqual(S.check_medium_change(polls, require_absent=True), [])
        ua = (S.CHECK_CONDITION, (6, 0x28, 0))
        absent = (S.CHECK_CONDITION, (2, 0x3A, 0))
        good = (S.GOOD, (0, 0, 0))
        self.assertEqual(phases[0], [absent, absent])
        self.assertEqual(phases[1], [ua, good, good, absent, absent, absent])
        self.assertEqual(phases[2], [ua, good, good])
        self.assertEqual(phases[-1], [ua, good])
        self.assertEqual(phases[-2], [absent])


# ================================================================== Bulk-Only Transport

def cbw(tag, length, data_in, cdb, lun=0):
    return S.encode_cbw(tag, length, data_in, lun, cdb).hex()


def host_result(line):
    """MOVED DATA HALTS CSW of h_s10a's "host"."""
    t = line.split()
    data, i = take_data(t, 1)
    return int(t[0]), data, t[i], unhex(t[i + 1])


BOT_STATE_CBW = 1


class Bot(unittest.TestCase):

    @staticmethod
    def commands():
        """The commands of scsi.json (each on a fresh, ready unit), with their write data."""
        out = []
        for name, seq in sorted(SCSI.items()):
            for s in seq:
                if 'cdb' in s and not s['name'].startswith('first command'):
                    out.append((unit_for(name)[0], bytes.fromhex(s['cdb'])))
        return out

    @scenario('S-10a/SC-10A-BOT')
    @requirement('S-10a.040', 'S-10a.052')
    def test_SC_10A_BOT_thirteen_cases(self):
        """Every command of scsi.json in a CBW with dCBWDataTransferLength 0, the intent, less and more,
        in both directions: the data moved, the halts and the CSW against bot_expect for the case the
        device's intent (s10a_model.Unit) makes. In cases 4, 5, 9 and 11 a halt or a short phase both
        pass; the other cases' halts are required as bot_expect gives them. Cases 11 and 12 store the
        blocks the CDB names, cases 8, 9 and 10 store nothing; after each the device expects a CBW."""
        M = model()
        cmds, cases = [], []
        tag = 0x1000
        for serial, cdb in self.commands():
            u = M.Unit(serial)
            u.execute(S.cdb_tur())
            need = u.execute(cdb).data_out_len
            r = M.Unit(serial)
            r.execute(S.cdb_tur())
            r = r.execute(cdb, pattern(need, 3))
            dev_in = len(r.data_in) > 0
            dev_len = len(r.data_in) or r.data_out_len
            ok = r.status == S.GOOD
            lens = {0, dev_len, dev_len + BLOCK, dev_len - BLOCK if dev_len > BLOCK else max(dev_len - 1, 0)}
            for host_len in sorted(lens):
                for host_in in (True, False):
                    if host_len == 0 and not host_in:
                        continue
                    tag += 1
                    o = S.bot_expect(host_len, host_in, dev_len, dev_in, ok)
                    blocks = None
                    if cdb[0] in (0x0A, 0x2A, 0xAA) and ok and dev_len:
                        lba, n = M_lba(cdb)
                        blocks = (lba, n)
                    cmds += ['lu %s 1' % ser(serial), 'bot', 'host %s -' % cbw(1, 0, False, S.cdb_tur()),
                             'host %s %s' % (cbw(tag, host_len, host_in, cdb), 'pat:3' if not host_in else '-'),
                             'eps']
                    if blocks:
                        cmds.append('blocks %d %d' % blocks)
                    cases.append((cdb, host_len, host_in, tag, o, r, blocks))
        out = run('s10a', cmds)
        i = 0
        for cdb, host_len, host_in, tag, o, r, blocks in cases:
            moved, data, halts, csw_b = host_result(out[i + 3])
            eps = [int(x) for x in out[i + 4].split()]
            stored = None
            if blocks:
                stored, _ = take_data(out[i + 5].split(), 0)
            i += 6 if blocks else 5
            with self.subTest(cdb=cdb.hex(), host_len=host_len, host_in=host_in, case=o.case):
                csw, f = S.decode_csw(csw_b, S.decode_cbw(bytes.fromhex(cbw(tag, host_len, host_in, cdb)))[0])
                self.assertEqual(f, [])
                self.assertEqual((csw.tag, csw.status, csw.residue), (tag, o.status, o.residue))
                self.assertEqual(moved, o.transfer)
                if host_in:
                    self.assertTrue(same_data(data, r.data_in[:o.transfer]), show(data))
                if o.case in (4, 5, 9, 11):
                    self.assertIn(halts, ('-', o.stall))
                else:
                    self.assertEqual(halts, o.stall or '-')
                self.assertEqual((eps[0], eps[1], eps[3], eps[4], eps[5], eps[6]),
                                 (0, 0, 1, 0, 0, BOT_STATE_CBW), 'a CBW expected')
                if blocks:
                    n = blocks[1] * BLOCK
                    if o.case in (11, 12):
                        self.assertTrue(same_data(stored, pattern(n, 3)), 'the blocks stored')
                    elif o.case in (8, 9, 10):
                        self.assertTrue(same_data(stored, bytes(n)), 'nothing stored')

    @scenario('S-10a/SC-10A-BOT')
    @requirement('S-10a.040', 'S-10a.033', 'S-10a.041')
    def test_SC_10A_BOT_refused_cbw(self):
        """CBWs of 30 and 32 bytes, a bad signature and LUN 1 (decode_cbw refuses each): both bulk
        endpoints halted; CLEAR_FEATURE on both accepted and still halted; a Bulk-Only Mass Storage
        Reset; CLEAR_FEATURE on both clears them; TEST UNIT READY answered."""
        good = S.encode_cbw(7, 0, False, 0, S.cdb_tur())
        bads = [good[:30], good + b'\0', b'USBX' + good[4:], S.encode_cbw(7, 0, False, 1, S.cdb_tur())]
        for b in bads[:3]:
            with self.assertRaises(S.MalformedError):
                S.decode_cbw(b)
        self.assertIn('cbw_lun', {x.rule for x in S.decode_cbw(bads[3])[1]})
        clear = ['setup ' + setup_hex(0x02, 1, 0, e, 0) for e in (0x87, 0x08)]
        for b in bads:
            cmds = ['lu %s 1' % ser(SERIAL), 'bot', 'host %s -' % cbw(1, 0, False, S.cdb_tur()),
                    'send ' + b.hex(), 'eps'] + clear + ['eps', 'setup ' + setup_hex(0x21, 0xFF, 0, 0, 0),
                                                          'eps'] + clear + \
                ['eps', 'host %s -' % cbw(8, 0, False, S.cdb_tur())]
            out = run('s10a', cmds)
            with self.subTest(cbw=b.hex()):
                self.assertEqual(out[3], '1')
                self.assertEqual(out[4].split()[:5], ['1', '1', '0', '0', '1'])
                self.assertEqual(out[5:7], ['accept', 'accept'])
                self.assertEqual(out[7].split()[:5], ['1', '1', '0', '0', '1'])
                self.assertEqual(out[8], 'forward -')
                self.assertEqual(out[9].split()[4], '0', 'the reset ends the wait')
                self.assertEqual(out[10:12], ['forward -', 'forward -'])
                self.assertEqual(out[12].split()[:5], ['0', '0', '0', '1', '0'])
                moved, _, halts, csw = host_result(out[13])
                self.assertEqual(csw, S.encode_csw(8, 0, S.CSW_PASSED))

    @scenario('S-10a/SC-10A-BOT')
    @requirement('S-10a.040', 'S-10a.041')
    def test_SC_10A_BOT_phase_error_recovery(self):
        """A phase error (case 2: INQUIRY with no data phase), then the reset recovery, then a command;
        the reset changes nothing of the unit's state (a unit attention owed is still reported)."""
        out = run('s10a', ['lu %s 1' % ser(SERIAL), 'bot',
                           'host %s -' % cbw(2, 0, False, S.cdb_inquiry(36)),
                           'setup ' + setup_hex(0x21, 0xFF, 0, 0, 0),
                           'setup ' + setup_hex(0x02, 1, 0, 0x87, 0), 'setup ' + setup_hex(0x02, 1, 0, 0x08, 0),
                           'host %s -' % cbw(3, 0, False, S.cdb_tur()), 'host %s -' % cbw(4, 0, False, S.cdb_tur())])
        self.assertEqual(host_result(out[2])[3], S.encode_csw(2, 0, S.CSW_PHASE_ERROR))
        self.assertEqual(out[3:6], ['forward -'] * 3)
        self.assertEqual(host_result(out[6])[3], S.encode_csw(3, 0, S.CSW_FAILED), 'the unit attention')
        self.assertEqual(host_result(out[7])[3], S.encode_csw(4, 0, S.CSW_PASSED))

    @scenario('S-10a/SC-10A-BOT')
    @requirement('S-10a.041')
    def test_S10a_041_class_requests(self):
        """Get Max LUN: one byte 00; with wLength 2, wValue 1 or the wrong direction a STALL; a reset
        with wValue 1 or wLength 1 a STALL; other class requests a STALL. On the disk set (interface 0)
        and the disk+CDC set (interface 2)."""
        reqs = [((0xA1, 0xFE, 0, 1), 'forward 00'), ((0xA1, 0xFE, 0, 2), 'forward stall'),
                ((0xA1, 0xFE, 1, 1), 'forward stall'), ((0x21, 0xFE, 0, 1), 'forward stall'),
                ((0x21, 0xFF, 0, 0), 'forward -'), ((0x21, 0xFF, 1, 0), 'forward stall'),
                ((0x21, 0xFF, 0, 1), 'forward stall'), ((0xA1, 0xFF, 0, 0), 'forward stall'),
                ((0x21, 0x00, 0, 0), 'forward stall'), ((0xA1, 0xFC, 0, 1), 'forward stall')]
        for name, itf in (('msc', 0), ('msc_cdc', 2)):
            cmds = ['lu %s 1' % ser(SERIAL), 'ch9 %d - 1' % SET_NO[name], 'setup ' + setup_hex(0, 9, 1, 0, 0)]
            cmds += ['setup ' + setup_hex(bm, rq, value, itf, length) for (bm, rq, value, length), _ in reqs]
            out = run('s10a', cmds)[3:]
            self.assertEqual(out, [w for _, w in reqs], name)

    @scenario('S-10a/SC-10A-MEDIUM')
    @requirement('S-10a.049')
    def test_SC_10A_MEDIUM_withdraw_in_flight(self):
        """A withdrawal requested during a READ(10)'s data phase: the command completes as the state
        before (all its data, CSW GOOD); the next TEST UNIT READY fails NOT READY 3Ah/00h."""
        n = 24
        tail = ['take 4096'] * 2
        out = run('s10a', ['lu %s 1' % ser(SERIAL), 'bot', 'host %s -' % cbw(1, 0, False, S.cdb_tur()),
                           'send ' + cbw(2, n * BLOCK, True, S.cdb_read10(0, n)), 'take 4096', 'event withdraw']
                  + tail + ['take 13', 'host %s -' % cbw(3, 0, False, S.cdb_tur()),
                            'host %s -' % cbw(4, 18, True, S.cdb_request_sense(18))])
        got = b''.join(unhex(o) for o in [out[4]] + out[6:6 + len(tail)] if o != 'halt')
        self.assertEqual(got, bytes(n * BLOCK))
        self.assertEqual(unhex(out[6 + len(tail)]), S.encode_csw(2, 0, S.CSW_PASSED))
        self.assertEqual(host_result(out[7 + len(tail)])[3], S.encode_csw(3, 0, S.CSW_FAILED))
        sense = host_result(out[8 + len(tail)])[1]
        self.assertEqual(S.sense_triple(sense), (2, 0x3A, 0))

    @scenario('S-10a/SC-10A-MEDIUM')
    @requirement('S-10a.041', 'S-10a.049', 'S-10a.080')
    def test_SC_10A_MEDIUM_withdraw_held_back(self):
        """Variant (c): a READ(10) of 64 blocks none of whose data the host reads, then a withdrawal: no
        CSW and the command still in its data phase. A Bulk-Only Mass Storage Reset and the reset
        recovery's CLEAR_FEATUREs (variant bus_reset: a bus reset and SET_CONFIGURATION(1)) abandon it:
        the READ's data dropped, a CBW expected, and the next TEST UNIT READY fails, REQUEST SENSE
        3Ah/00h."""
        clear = ['setup ' + setup_hex(0x02, 1, 0, e, 0) for e in (0x87, 0x08)]
        resets = {'bomsr': ['setup ' + setup_hex(0x21, 0xFF, 0, 0, 0)] + clear,
                  'bus_reset': ['busreset', 'setup ' + setup_hex(0, 9, 1, 0, 0)]}
        for variant, reset in sorted(resets.items()):
            out = run('s10a', ['lu %s 1' % ser(SERIAL), 'bot', 'host %s -' % cbw(1, 0, False, S.cdb_tur()),
                               'send ' + cbw(2, 64 * BLOCK, True, S.cdb_read10(0, 64)), 'event withdraw', 'eps']
                      + reset + ['eps', 'host %s -' % cbw(3, 0, False, S.cdb_tur()),
                                 'host %s -' % cbw(4, 18, True, S.cdb_request_sense(18))])
            with self.subTest(variant=variant):
                held = [int(x) for x in out[5].split()]
                self.assertEqual((held[0], held[1], held[2]), (0, 0, 1), 'the READ\'s data armed, no halt')
                self.assertNotEqual(held[6], BOT_STATE_CBW, 'the READ not ended by the withdrawal')
                k = 6 + len(reset)
                after = [int(x) for x in out[k].split()]
                self.assertEqual((after[0], after[1], after[2], after[3], after[6]), (0, 0, 0, 1, BOT_STATE_CBW),
                                 'the READ\'s data dropped, a CBW expected')
                self.assertEqual(host_result(out[k + 1])[3], S.encode_csw(3, 0, S.CSW_FAILED))
                self.assertEqual(S.sense_triple(host_result(out[k + 2])[1]), (2, 0x3A, 0))

    @scenario('S-10a/SC-10A-RELEASE')
    @requirement('S-10a.032', 'S-10a.048', 'S-10a.050', 'S-10a.080')
    def test_SC_10A_RELEASE_bus_side(self):
        """Through the control endpoint: (h) SET_CONFIGURATION with the current value while the disk is
        held and the unit stopped is a re-selection: a release, the unit ready with no unit attention,
        and a READ(10) in its data phase abandoned with no CSW, the next CBW answered; (c)
        SET_CONFIGURATION(0) and (b) a bus reset each release a held disk; (d) a stop and (e) PREVENT
        then ALLOW do not."""
        M = model()
        tur = lambda tag: 'host %s -' % cbw(tag, 0, False, S.cdb_tur())   # noqa: E731
        sel = 'setup ' + setup_hex(0, 9, 1, 0, 0)
        cmds = ['lu %s 1' % ser(SERIAL), 'bot', tur(1), tur(2),
                'host %s -' % cbw(3, 0, False, M.cdb_start_stop(False, False)), 'lustate',
                sel, 'lustate', tur(4),
                'send ' + cbw(5, 16 * BLOCK, True, S.cdb_read10(0, 16)), 'take 4096', 'eps', sel, 'eps', tur(6),
                'lustate', 'setup ' + setup_hex(0, 9, 0, 0, 0), 'lustate', sel, tur(7), 'busreset', 'lustate',
                sel, tur(8), 'host %s -' % cbw(9, 0, False, M.cdb_prevent(True)),
                'host %s -' % cbw(10, 0, False, M.cdb_prevent(False)), 'lustate']
        out = run('s10a', cmds)
        rel = lambda k: int(out[k].split()[4])                       # noqa: E731
        self.assertEqual(rel(5), 0, '(d) a stop does not release')
        self.assertEqual(out[6], 'reselect')
        self.assertEqual(rel(7), 1, '(h) the re-selection releases')
        self.assertEqual(host_result(out[8])[3], S.encode_csw(4, 0, S.CSW_PASSED), 'ready, no unit attention')
        self.assertEqual(out[11].split()[5], '1', 'the data phase in progress')
        self.assertEqual(out[12], 'reselect')
        self.assertEqual(out[13].split(), ['0', '0', '0', '1', '0', '0', str(BOT_STATE_CBW)],
                         'abandoned: no CSW, a CBW expected')
        self.assertEqual(host_result(out[14])[3], S.encode_csw(6, 0, S.CSW_PASSED))
        self.assertEqual(rel(15), 2, 'held again by TEST UNIT READY, released by the second re-selection')
        self.assertEqual(out[16], 'forward -')
        self.assertEqual(rel(17), 3, '(c) SET_CONFIGURATION(0) releases')
        self.assertEqual(out[18], 'forward -')
        self.assertEqual(host_result(out[19])[3], S.encode_csw(7, 0, S.CSW_PASSED))
        self.assertEqual(rel(21), 4, '(b) the bus reset releases')
        self.assertEqual(host_result(out[23])[3], S.encode_csw(8, 0, S.CSW_PASSED))
        self.assertEqual(rel(26), 4, '(e) PREVENT and ALLOW do not release')


def M_lba(cdb):
    """(LBA, blocks) of a READ or WRITE(6), (10) or (12)."""
    if cdb[0] in (0xA8, 0xAA):
        return struct.unpack_from('>I', cdb, 2)[0], struct.unpack_from('>I', cdb, 6)[0]
    return S.rw_lba_length(cdb)


def rw_blocks(cdb):
    return M_lba(cdb)[1]


if __name__ == '__main__':
    unittest.main()
