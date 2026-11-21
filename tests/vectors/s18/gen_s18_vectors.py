#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate s18_vectors.h from tests/oracle/S-18-devices/vectors/.

Run from the repository root:  python3 -I -B tests/vectors/s18/gen_s18_vectors.py --out DIR
"""
import json
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import genout  # noqa: E402  (before anything the generator reads)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
VEC = os.path.join(ROOT, 'tests', 'oracle', 'S-18-devices', 'vectors')
OUT = 's18_vectors.h'
DEVICES = ('OP-XY', 'OP-1F')
GPIO_KIND = {'in': 0, 'low': 1, 'high': 2, 'keep': 3}


def load(name):
    with open(os.path.join(VEC, name)) as f:
        return json.load(f)


def c_str(s):
    return 'NULL' if s is None else '"%s"' % s


def dev_code(d):
    return -1 if d is None else DEVICES.index(d)


def pin(name):
    """PA5 -> port 0 * 16 + 5."""
    return 'ABC'.index(name[1]) * 16 + int(name[2:])


def hexes(xs, width=0):
    return ', '.join('0x%0*Xu' % (width, x) for x in xs)


def first_stages(lines):
    fs = load('first_stage.json')
    lines.append('/* first_stage.json: S-18.003, S-18.004. seed -1: an erased flash; '
                 'has_arg 0: no argument set. */')
    lines.append('struct s18v_stage { const char *name; uint32_t length; int seed; int has_arg; '
                 'uint32_t argument; unsigned npatch; struct { uint32_t off; uint8_t b[4]; } patch[2]; '
                 'uint32_t n; int has_c; uint32_t c; int device; /* -1 none */ };')
    lines.append('static const struct s18v_stage s18v_stages[] = {')
    for c in fs['cases']:
        assert len(c['patches']) <= 2
        patches = ', '.join('{ 0x%Xu, { %s } }' % (p['offset'], hexes(bytes.fromhex(p['bytes']), 2))
                            for p in c['patches']) or '{ 0u, { 0 } }'
        lines.append('\t{ %s, 0x%Xu, %d, %d, 0x%08Xu, %d, { %s }, 0x%08Xu, %d, %s, %d },' % (
            c_str(c['name']), c['length'], -1 if c['seed'] is None else c['seed'],
            c['argument'] is not None, c['argument'] or 0, len(c['patches']), patches, c['n'],
            c['c'] is not None, c['c'] + 'u' if c['c'] else '0u', dev_code(c['device'])))
    lines += ['};', '']
    lines.append('struct s18v_row { uint32_t n, c; int device; };')
    lines.append('static const struct s18v_row s18v_table[] = {')
    for r in fs['table']:
        lines.append('\t{ 0x%Xu, %su, %d },' % (r['n'], r['c'], dev_code(r['device'])))
    lines += ['};', '']
    names = [c['name'] for c in fs['cases']]
    faults = ('whole', 'head', 'later', 'busy_head', 'busy_later', 'flip')
    lines.append('/* first_stage.json tries: S-18.003. fault[i]: try i\'s, S18V_F_*; past nfault, whole. */')
    for i, f in enumerate(faults):
        lines.append('#define S18V_F_%s %d' % (f.upper(), i))
    lines.append('struct s18v_try { const char *name; int stage; unsigned nfault; int fault[3]; '
                 'unsigned tries; int device; unsigned how; uint32_t n, c; };')
    lines.append('static const struct s18v_try s18v_tries[] = {')
    for t in fs['tries']:
        assert len(t['faults']) <= 3
        fl = ', '.join('S18V_F_%s' % f.upper() for f in t['faults']) or '0'
        lines.append('\t{ %s, %d, %d, { %s }, %d, %d, %d, 0x%08Xu, %s },' % (
            c_str(t['name']), names.index(t['first_stage']), len(t['faults']), fl, t['tries'],
            dev_code(t['device']), ('table', 'second_cpu', 'unknown').index(t['how']), t['n'],
            (t['c'] + 'u') if t['c'] else '0u'))
    lines += ['};', '']
    lines.append('#define S18V_TRIES %du' % fs['most_tries'])
    lines.append('#define S18V_ARG_OFF 0x%Xu' % fs['arg_offset'])
    lines.append('#define S18V_HEAD %du' % fs['head'])
    lines.append('#define S18V_N_MAX 0x%Xu' % fs['n_max'])
    lines.append('')


def identify(lines):
    hows = ('table', 'second_cpu', 'unknown')
    lines.append('/* identify.json: S-18.002, S-18.004, S-18.005. stage: an index into s18v_stages, '
                 '-1 none. */')
    lines.append('struct s18v_ident { const char *name; unsigned boot_mode, pa2; int stage; int main_cpu; '
                 'unsigned device, how; int reads_flash, strap_sample, disagreement; '
                 'const char *device_id; uint32_t n, c; int fallback_sample, role_flag; };')
    names = [c['name'] for c in load('first_stage.json')['cases']]
    lines.append('static const struct s18v_ident s18v_idents[] = {')
    for r in load('identify.json')['rows']:
        assert r['record'] == {'device': dev_code(r['device']), 'how': hows.index(r['how'])}
        lines.append('\t{ %s, %d, %d, %d, %d, %d, %d, %d, %d, %d, %s, 0x%08Xu, %s, %d, %d },' % (
            c_str(r['name']), r['boot_mode'], r['pa2'],
            -1 if r['first_stage'] is None else names.index(r['first_stage']),
            r['cpu'] == 'main', dev_code(r['device']), hows.index(r['how']), r['reads_flash'],
            r['strap_sample'], r['role_disagreement_logged'], c_str(r['device_id']),
            r['n'] or 0, (r['c'] + 'u') if r['c'] else '0u', r['fallback_sample'], r['role_flag']))
    lines += ['};', '']


def features(lines):
    f = load('features.json')
    lines.append('/* features.json: S-18.020, in the table\'s order. */')
    lines.append('struct s18v_feature { const char *name; int has[2]; };')
    lines.append('static const struct s18v_feature s18v_features[] = {')
    for r in f['features']:
        lines.append('\t{ %s, { %d, %d } },' % (c_str(r['name']), r['OP-XY'], r['OP-1F']))
    lines += ['};', '']


def profile(lines):
    p = load('profile.json')
    rows = {}
    for table in p['tables'].values():
        for r in table:
            rows[r['name']] = r
    seen = set()

    def v(name, d):
        seen.add(name)
        return rows[name][d]

    lines.append('/* profile.json: S-18.030 and the per-device texts. gpio kinds: 0 in, 1 low, 2 high, '
                 '3 keep; pins: port * 16 + bit; WB_KEY_NONE 0xFF. */')
    lines.append('struct s18v_profile {')
    lines.append('\tunsigned product_number; uint32_t product_base; const char *product_text, *sku_prefix;')
    lines.append('\tunsigned usb_pid, midi_pid, midi_byte; uint8_t midi_family[2]; int member_from_sku; '
                 'uint8_t midi_member[2];')
    lines.append('\tunsigned metadata_size; int ramdisk_one; uint32_t ramdisk_base, ramdisk_len, '
                 'ramdisk_blocks;')
    lines.append('\tunsigned emmc_width; uint32_t emmc_sectors; int emmc_at_least; int fat_erase; '
                 'unsigned reset_only_n; const char *reset_only[2];')
    lines.append('\tuint32_t mux[3], fer[3]; unsigned gpio_n; struct { uint8_t pin, kind; } gpio[16]; '
                 'unsigned panel_dc;')
    lines.append('\tunsigned key_count, bitmap_bytes; uint8_t hold[2], debug[2]; int debug_both; '
                 'unsigned precomp;')
    lines.append('\tuint8_t digit[8][3]; unsigned digit_knob, shift, confirm, com, recovery;')
    lines.append('\tconst char *device_id, *contract;')
    lines.append('};')
    lines.append('static const struct s18v_profile s18v_profiles[2] = {')
    for d in DEVICES:
        member = v('device.midi_member', d)
        if isinstance(member, dict):
            assert member['from_sku'] == {'bytes': 2, 'bits': 7, 'low_first': True}
            assert member['sku_unset'] == 0xFFFF
            from_sku, mbytes = 1, [member['when_unset'] & 0xFF, member['when_unset'] >> 8]
        else:
            from_sku, mbytes = 0, member
        rd = v('device.ramdisk_regions', d)
        regions = rd.get('regions', [[0, 0]])
        assert len(regions) == 1
        es = v('device.emmc_sectors', d)
        rr = v('device.reset_removes', d)
        only = rr.get('only', [])
        assert len(only) <= 2
        gpio = [(pin(n), GPIO_KIND[k]) for n, k in v('device.gpio_outputs', d)] + \
               [(pin(n), 0) for n in v('device.gpio_inputs', d)]
        assert len(gpio) <= 16
        hold = v('device.key.hold', d)
        debug = v('device.key.debug', d)
        digits = [v('device.key.digit%d' % i, d) for i in range(1, 5)] + \
                 [[k, 0xFF, 0xFF] for k in v('device.key.digit5to8', d)]
        pc = v('device.key.precomp', d)
        lines.append('\t{ /* %s */' % d)
        lines.append('\t\t0x%02Xu, 0x%08Xu, %s, %s,' % (
            v('device.product_number', d), v('device.product_base', d),
            c_str(v('device.product_text', d)), c_str(v('device.sku_prefix', d))))
        lines.append('\t\t0x%04Xu, 0x%04Xu, 0x%02Xu, { %s }, %d, { %s },' % (
            v('device.usb_pid', d), v('device.midi_pid', d), v('device.midi_byte', d),
            hexes(v('device.midi_family', d), 2), from_sku, hexes(mbytes, 2)))
        lines.append('\t\t0x%02Xu, %d, 0x%08Xu, 0x%08Xu, %du,' % (
            v('device.metadata_size', d), 'regions' in rd, regions[0][0], regions[0][1],
            v('device.ramdisk_blocks', d)))
        lines.append('\t\t%du, 0x%Xu, %d, %d, %d, { %s },' % (
            v('device.emmc_width', d), es.get('exactly', es.get('at_least')), 'at_least' in es,
            rr['fat_erase'], len(only), ', '.join(c_str(x) for x in only) or 'NULL'))
        lines.append('\t\t{ %s }, { %s }, %d, { %s }, 0x%02Xu,' % (
            hexes([v('device.port%s_mux' % x, d) for x in 'abc'], 8),
            hexes([v('device.port%s_fer' % x, d) for x in 'abc'], 8), len(gpio),
            ', '.join('{ 0x%02Xu, %d }' % g for g in gpio), pin(v('device.panel_dc', d))))
        lines.append('\t\t%du, %du, { %s }, { %s }, %d, 0x%02Xu,' % (
            v('device.key.count', d), v('device.key.bitmap_bytes', d), hexes(hold['either'], 2),
            hexes(debug.get('both', debug.get('either')), 2), 'both' in debug, 0xFF if pc is None else pc))
        lines.append('\t\t{ %s }, %du, 0x%02Xu, 0x%02Xu, 0x%02Xu, 0x%02Xu,' % (
            ', '.join('{ %s }' % hexes(x, 2) for x in digits), p['digit_knob'][d],
            v('device.key.shift', d), v('device.key.confirm', d), v('device.key.com', d),
            v('device.key.recovery', d)))
        lines.append('\t\t%s, %s,' % (c_str(p['device_id'][d]), c_str(p['contract'][d])))
        lines.append('\t},')
        # rows the C struct has no field for
        seen.update(('device.logo',))
    lines += ['};', '']
    assert seen == set(rows), set(rows) - seen
    lines.append('#define S18V_DEVICE_ID_UNKNOWN %s' % c_str(p['device_id']['unknown']))
    lines.append('')


def main():
    lines = ['/* SPDX-License-Identifier: GPL-2.0-or-later */',
             '/* Generated by tests/vectors/s18/gen_s18_vectors.py from',
             ' * tests/oracle/S-18-devices/vectors/. Do not edit. */',
             '#ifndef S18_VECTORS_H', '#define S18_VECTORS_H', '',
             '#include <stddef.h>', '#include <stdint.h>', '']
    first_stages(lines)
    identify(lines)
    features(lines)
    profile(lines)
    lines += ['#endif /* S18_VECTORS_H */', '']
    genout.write(OUT, '\n'.join(lines))
    genout.finish()


if __name__ == '__main__':
    sys.exit(main())
