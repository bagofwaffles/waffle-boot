# SPDX-License-Identifier: GPL-2.0-or-later
"""S-03 model: the eMMC header parser, load rule and write rule, used to generate and check the vectors.
It models boot partition contents and the eMMC operations a bootloader issues; the user area is not modeled.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'conformance'))
from images import emmc  # noqa: E402
from images.crc import crc32  # noqa: E402

SECTOR = 512
GROUP = 0x400                  # sectors per erase group (S-03.001)
OS_MAX = 0x800000              # S-03.042
WRITE_MIN = 10000              # S-03.051: length + 512 >= 10000
USER, BP1, BP2 = 0, 1, 2


# ---- the header and the eMMC (S-03.021-S-03.027), from images.emmc ----

n_sectors = emmc.n_sectors
pack_header = emmc.pack_header


def parse_header(b):
    """The reader's view of a header (emmc.parse_header): mode 'plain', 'encrypted' (format 2 or 3 with a
    nonzero encrypted byte: not loaded, S-03.048) or 'refused' (`format` above 3: fails the load, S-03.023,
    S-03.042)."""
    h = emmc.parse_header(b)
    if h['mode'] == 'open':
        h['mode'] = 'refused'
    return h


Emmc = emmc.Emmc                 # Emmc(B): BP1/BP2 bytearrays of B bytes; an erase zeroes one erase group


# ---- the load rule (S-03.040-S-03.048) ----

LOAD_FAILS = ('select1', 'read_header1', 'select2', 'read_header2', 'read_image', 'select0')


def load(device, fallback=False, fail=()):
    """Returns {'outcome': 'BP1'|'BP2'|'no_os', 'reason', 'selects', 'header_reads', 'extent', 'image'}.
    A header marked encrypted fails the load (S-03.048). `fail` names injected operation failures, each
    after its retries (S-03.010): see LOAD_FAILS. fallback=True tries BP2 after a failed active BP1."""
    selects, header_reads = [], []

    def finish(outcome, reason, extent=None, image=None):
        selects.append(USER)
        if 'select0' in fail:                       # S-03.046
            outcome, reason, image = 'no_os', 'select0_error', None
        return {'outcome': outcome, 'reason': reason, 'selects': selects, 'header_reads': header_reads,
                'extent': extent, 'image': image}

    def try_part(part):
        h = parse_header(device.read(part, 0, 1))
        n = n_sectors(h['length'])
        if h['length'] == 0:
            return None, 'zero_length', None
        if h['length'] > OS_MAX:
            return None, 'too_long', None
        if SECTOR * (n + 1) > device.B:
            return None, 'past_partition', None
        if h['mode'] == 'refused':                  # S-03.042: format above 3
            return None, 'format', None
        if h['mode'] == 'encrypted':                # S-03.042, S-03.048: no image read
            return None, 'encrypted', None
        if 'read_image' in fail:
            return None, 'read_error', None
        img = device.read(part, 1, n)[:h['length']]
        if crc32(img) != h['crc']:
            return None, 'crc_mismatch', (1, n)
        return img, '', (1, n)

    selects.append(BP1)
    if 'select1' in fail:
        return finish('no_os', 'select_error')
    header_reads.append(BP1)
    if 'read_header1' in fail:
        return finish('no_os', 'read_error')
    if parse_header(device.read(BP1, 0, 1))['active']:
        order = [BP1, BP2] if fallback else [BP1]
    else:
        order = [BP2]
    reason = 'no_active'
    for part in order:
        if part == BP2:
            selects.append(BP2)
            if 'select2' in fail:
                return finish('no_os', 'select_error')
            header_reads.append(BP2)
            if 'read_header2' in fail:
                return finish('no_os', 'read_error')
            if not parse_header(device.read(BP2, 0, 1))['active']:
                continue
        img, why, extent = try_part(part)
        if img is not None:
            return finish('BP%d' % part, '', extent, img)
        reason = why
    return finish('no_os', reason)


# ---- the write rule (S-03.050-S-03.067), from images.emmc.write_os ----
# order 'two_step' (S-03.055): the target's header is written with the active byte 0, read back, rewritten
# with the active byte 1 and read back; the other partition's active byte is then rewritten, and the
# partition erased, if at all, after that. order 'previous' selects profile 'factory' (one header write,
# the other partition's group 0 erased), the control case of the tear check (S-03.067).

acceptable = emmc.acceptable
last_writable_sector = emmc.last_writable_sector
WRITE_FAILS = emmc.WRITE_FAILS
DEACTIVATIONS = emmc.DEACTIVATIONS    # S-03.058: the rewrite alone
ORDERS = {'two_step': 'two_step', 'previous': 'factory'}


def write(device, image, corrupt_sector=None, corrupt_header=None, deactivate='rewrite', cut_after=None,
          fail=None, order='two_step', tear=None):
    """The OS writer (S-03.050-S-03.067): emmc.write_os with order 'two_step' or 'previous', storing
    plaintext only (S-03.033). Returns emmc.write_os's (result, ops, target)."""
    return emmc.write_os(device, image, corrupt_sector=corrupt_sector,
                         corrupt_header=corrupt_header, deactivate=deactivate, cut_after=cut_after,
                         fail=fail, profile=ORDERS[order], tear=tear)


# ---- torn operations (S-03.067): the byte-granular tear model ----

HEADER_FIELDS = 0x0E          # bytes 0x00-0x0D decide a load (S-03.026)


def tear_variants(before, after):
    """Representative byte-granular tears of one operation's range (each byte old or new): the active
    byte (offset 0x0C of a header sector at range offset 0) and the other header fields land
    independently, each all old, all new, or (fields) half and half; the rest of the range all old, all
    new, or its first half new. Returns a list of byte strings, each a possible stored result."""
    if before == after:
        return [after]
    hb, ha = before[:SECTOR], after[:SECTOR]
    diff = [i for i in range(HEADER_FIELDS) if hb[i] != ha[i] and i != 0x0C]

    def fields(choice):
        h = bytearray(hb)
        for i in range(SECTOR):
            if i == 0x0C or i in diff:
                continue
            h[i] = ha[i] if choice != 'old' else hb[i]   # reserved bytes: follow the fields' choice
        for k, i in enumerate(diff):
            new = choice == 'new' or (choice == 'half' and k < (len(diff) + 1) // 2)
            h[i] = ha[i] if new else hb[i]
        return h

    field_choices = ['old', 'new'] + (['half'] if len(diff) >= 2 else [])
    active_choices = [hb[0x0C], ha[0x0C]] if hb[0x0C] != ha[0x0C] else [ha[0x0C]]
    rest_b, rest_a = before[SECTOR:], after[SECTOR:]
    half = (len(rest_a) // SECTOR // 2) * SECTOR
    rests = [rest_b, rest_a] if rest_b != rest_a else [rest_a]
    if rest_b != rest_a and half:
        rests.append(rest_a[:half] + rest_b[half:])
    out = []
    for fc in field_choices:
        for av in active_choices:
            h = fields(fc)
            h[0x0C] = av
            for r in rests:
                v = bytes(h) + r
                if v not in out:
                    out.append(v)
    return out


def torn_outcomes(start_device, image, old, k, **kw):
    """The boot outcomes ('old', 'new', 'none', 'other') over the tear variants of the k-th erase/write of
    the write of `image` on a copy of `start_device`, and the op itself."""
    probe = start_device.copy()
    seen = {}

    def capture(device, part, a, before, after):
        seen['args'] = (part, a, before, after)

    res, ops, _ = write(probe, image, tear=(k, capture), **kw)
    assert res == 'cut', res
    part, a, before, after = seen['args']
    outcomes, outcomes_fb = set(), set()
    for v in tear_variants(before, after):
        c = probe.copy()
        c.boot[part][a:a + len(v)] = v
        outcomes.add(boots(c, old, image))
        outcomes_fb.add(boots(c, old, image, fallback=True))
    return sorted(outcomes), sorted(outcomes_fb), ops[-1]


def boots(device, old, new, fallback=False):
    """'old', 'new' or 'none': which image the eMMC boots as it stands."""
    r = load(device, fallback)
    if r['outcome'] == 'no_os':
        return 'none'
    if new is not None and r['image'] == new:
        return 'new'
    if old is not None and r['image'] == old:
        return 'old'
    return 'other'
