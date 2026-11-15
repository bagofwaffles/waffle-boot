# SPDX-License-Identifier: GPL-2.0-or-later
"""Self-test of the hw package. Run: python3 -I tests/oracle/conformance/hw/selftest.py
Prints one line per group and a summary; exits nonzero on any failure. Synthetic inputs only."""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from hw import buses, helpers, mmrtrace, order, panel, regdump, spiframes, tracediff  # noqa: E402

RESULTS = []


def group(name):
    def deco(fn):
        try:
            fn()
            RESULTS.append((name, None))
        except Exception as e:  # report and continue
            RESULTS.append((name, '%s: %s' % (type(e).__name__, e)))
        return fn
    return deco


@group('mmrtrace format')
def _():
    ev = mmrtrace.parse('w 20057018/4=202\n@100 r 0x20002008 4\nnoise\n@7 m 80000000\nret\nw 10=3\n')
    assert [e['k'] for e in ev] == ['w', 'r', 'm', 'ret', 'w'], ev
    assert ev[0] == {'cyc': None, 'k': 'w', 'a': 0x20057018, 'v': 0x202}
    assert ev[1]['cyc'] == 100 and ev[1]['v'] == 4 and ev[2]['a'] == 0x80000000
    assert ev[4]['a'] == 0x10 and ev[4]['v'] == 3
    for e in ev:
        assert mmrtrace.parse_line(mmrtrace.format_event(e)) == e, e
    assert mmrtrace.first(ev, mmrtrace.w(0x20057018, 0x200)) == 0
    assert mmrtrace.first(ev, mmrtrace.w(0x20057018, 0x200, zero=True)) is None
    assert mmrtrace.last(ev, lambda e: e['k'] == 'w') == 4
    out = {'fail': [], 'unchecked': []}
    mmrtrace.duration(ev, 0, 1, 10, 'x', out)
    assert out['unchecked'] == ['x']


TABLE = ('req\tpoints\tregister\taddress\tfield\tbits\trule\tvalue\n'
         'R.1\tA,B\tREG0\t0x1000\tLO\t3:0\teq\t5\n'
         'R.2\tA\tREG0\t0x1000\tHI\t15:8\tin\t0x10..0x20\n'
         'R.3\tB\tREG1\t0x1004\tBIT\t31\tne\t1\n'
         'R.4\tA\tREG2\t0x1008\tX\t7:0\tany\t\n'
         'R.5\tB\tREG1\t0x1004\tN\t7:0\tge\t3\n'
         'R.6\tB\tREG1\t0x1004\tM\t15:8\tle\t2\n')


@group('regdump rules')
def _():
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, 't.tsv')
        with open(p, 'w') as f:
            f.write(TABLE)
        rows = regdump.load_table(p)
    assert regdump.points_of(rows) == ['A', 'B']
    assert regdump.check('A', {'0x1000': 0x1505}, rows) == []          # REG2 is all-any: may be missing
    assert regdump.check('A', regdump.parse_dump('1000=0x2105'), rows)[0].startswith('R.2')
    assert regdump.check('A', {'REG0': 0x1504}, rows)[0].startswith('R.1')
    assert regdump.check('B', regdump.parse_dump('{"REG0": 5, "0x1004": "0x00000203"}'), rows) == []
    f = regdump.check('B', {'REG0': 5, 'REG1': 0x80000302}, rows)
    assert sorted(x[:3] for x in f) == ['R.3', 'R.5', 'R.6'], f
    assert 'missing' in regdump.check('B', {'REG0': 5}, rows)[0]
    try:
        regdump.check('C', {}, rows, regdump.points_of(rows))
        raise AssertionError('unknown point accepted')
    except ValueError:
        pass


def init_trace(cal_cycles=None):
    o = order
    t = ['w %x 800' % o.PHY_CTL0, 'r %x 5' % o.CGU_STAT, 'w %x 4042442' % o.CGU_DIV, 'w %x 2000' % o.CGU_CTL,
         'r %x 4' % o.CGU_STAT, 'w %x 0' % o.PHY_CTL0, 'w %x 1' % o.PHY_CTL4, 'w %x 1' % o.PHY_CTL3,
         'w %x 1' % o.PADCTL2, 'w %x 10000000' % o.PADCTL0]
    t += ['w %x 1' % a for a in o.DMC_PROG] + ['w %x 2404' % o.DMC_CTL, 'r %x 4' % o.DMC_STAT, 'ret']
    if cal_cycles is not None:
        t = ['@%d %s' % (i * cal_cycles, x) for i, x in enumerate(t)]
    return '\n'.join(t)


@group('order rules (BF70x bring-up)')
def _():
    out = order.check_init(mmrtrace.parse(init_trace()))
    assert out['fail'] == [] and len(out['unchecked']) == 3, out          # no cycles, no memory events
    out = order.check_init(mmrtrace.parse(init_trace(cal_cycles=1)))
    assert any('CALSTRT to DMC programming' in x for x in out['fail']), out
    out = order.check_init(mmrtrace.parse(init_trace(cal_cycles=10000)))
    assert out['fail'] == [], out
    bad = init_trace().replace('w %x 2404\n' % order.DMC_CTL, 'w %x 404\n' % order.DMC_CTL)     # INIT without DLLCAL
    assert any('without DLLCAL' in x for x in order.check_init(mmrtrace.parse(bad))['fail'])
    bad = init_trace().replace('w %x 1\n' % order.PHY_CTL3, '')
    assert any('DMC0_PHY_CTL3' in x for x in order.check_init(mmrtrace.parse(bad))['fail'])
    run = 'w %x 4042442\n' % order.CGU_DIV
    assert order.check_run(mmrtrace.parse(run))['fail'][0].startswith('S-13a.009')
    run = 'r %x 0\nw %x 4042442\n' % (order.CGU_STAT, order.CGU_DIV)
    assert order.check_run(mmrtrace.parse(run))['fail'] == []
    run = 'r %x 0\nw %x 44042442\n' % (order.CGU_STAT, order.CGU_DIV)     # same fields, UPDT set
    assert [x[:9] for x in order.check_run(mmrtrace.parse(run))['fail']] == ['S-13a.007'], run
    rtc = 'r %x 5\nw %x 5\n' % (order.RTC_INIT, order.RTC_INIT)          # CAL kept
    assert order.check_rtc(mmrtrace.parse(rtc))['fail'] == []
    rtc = 'r %x 5\nw %x 0\n' % (order.RTC_INIT, order.RTC_INIT)          # CAL cleared
    assert order.check_rtc(mmrtrace.parse(rtc))['fail'][0].startswith('S-13a.071')
    rtc = 'w %x 1\n' % 0x200D1280
    assert order.check_rtc(mmrtrace.parse(rtc))['fail'] != []


@group('spi decode and config rules')
def _():
    d = buses.decode_spi(0x2593, 3)
    assert (d['enabled'], d['master'], d['mode'], d['word_bits'], d['flow_control']) == (1, 1, 1, 32, 1), d
    assert d['sck_hz'] == 25e6
    d = buses.decode_spi(0x222510, 0)
    assert (d['io'], d['flow_on_tx'], d['flow_watermark']) == ('quad', 0, 2), d
    rules = {'x': {'master': {1}, 'mode': {1}, 'sck_max': 20e6}}
    assert [r for r, _ in buses.check_spi_config(rules, 'x', 0x2593, 3)] == ['sck']
    assert [r for r, _ in buses.check_spi_config(rules, 'x', 0x2591, 9)] == ['master']


def fr(mosi, miso=''):
    return {'mosi': mosi, 'miso': miso}


@group('spi-nor frames (JEDEC)')
def _():
    good = [fr('9f000000'), fr('06'), fr('20001000'), fr('0500', '0003'), fr('0500', '0000'),
            fr('06'), fr('020010f0' + '00' * 16), fr('0500', '0000'), fr('03000000' + '00' * 4)]
    assert buses.check_flash(good, 0x100000) == []
    assert [r for r, _ in buses.check_flash([fr('20001000')], 0x100000)] == ['wren_before_write']
    assert [r for r, _ in buses.check_flash([fr('06'), fr('20001000'), fr('03000000ff')], 0x100000)] == ['busy_wait']
    assert [r for r, _ in buses.check_flash([fr('03100000ff')], 0x100000)] == ['address_range']
    assert [r for r, _ in buses.check_flash([fr('06'), fr('20000800')], 0x100000)] == ['alignment']
    assert [r for r, _ in buses.check_flash([fr('ab')], 0x100000)] == ['opcode_set']


RCA, OCR = 0x1234, 0x40000080
IDX = {183, 185}


def emmc_init_trace():
    c = lambda i, a=0, r=0: {'ev': 'cmd', 'idx': i, 'arg': a, 'resp': r}
    return [{'ev': 'clock', 'hz': 400_000}, c(0), c(1, OCR, 0x00FF8080), c(1, OCR, 0x80FF8080), c(2),
            c(3, RCA << 16), c(9, RCA << 16), c(7, RCA << 16), c(8), c(6, 0x03B70200), c(13, RCA << 16, 0x900),
            {'ev': 'width', 'bits': 8}, c(6, 0x03B90100), {'ev': 'busy_end'},
            {'ev': 'clock', 'hz': 50_000_000}, c(17, 0)]


@group('emmc commands (JESD84)')
def _():
    assert buses.check_init(emmc_init_trace(), RCA, OCR, IDX) == []
    assert [r for r, _ in buses.check_init(emmc_init_trace(), 0x5555, OCR, IDX)] == ['emmc_init']
    assert [r for r, _ in buses.check_init(emmc_init_trace(), RCA, OCR, {183})] == ['emmc_switch']
    ev = emmc_init_trace()
    ev.insert(11, {'ev': 'cmd', 'idx': 17, 'arg': 0, 'resp': 0})       # before the host width follows the eMMC
    assert [r for r, _ in buses.check_init(ev, RCA, OCR, IDX)] == ['bus_width_match']
    assert buses.switch_fields(0x03B70200) == (3, 183, 2)
    assert buses.msi_emmc_clock(0, 50_000_000) == 50_000_000 and buses.msi_emmc_clock(2, 50_000_000) == 12.5e6
    off = [{'ev': 'cmd', 'idx': 6, 'arg': buses.POWER_OFF_LONG}, {'ev': 'cmd', 'idx': 13}]
    assert [r for r, _ in buses.check_power_off(off)] == ['power_off_quiet']
    xfer = [{'ev': 'cmd', 'idx': 18, 'arg': 0}, {'ev': 'cmd', 'idx': 13, 'arg': 0}]
    assert [r for r, _ in buses.check_transfers(xfer)] == ['emmc_transfer']


@group('uart decode and line setting')
def _():
    d = buses.decode_uart(0x301, 0x80000064)
    assert (d['enabled'], d['data_bits'], d['stop_bits'], d['parity'], d['baud']) == (1, 8, 1, False, 1e6), d
    assert buses.check_uart_config('u', 0x301, 0x80000064, 1_000_000) == []
    assert [r for r, _ in buses.check_uart_config('u', 0x301, 0x64, 1_000_000)] == ['baud']
    assert [r for r, _ in buses.check_uart_config('u', 0x18000301, 0x80000064, 1_000_000)] == ['flow']


@group('port model (SET/CLR views)')
def _():
    st = buses.replay([{'op': 'w', 'reg': 'PORTA_INEN', 'val': 0x40},
                       {'op': 'w', 'reg': 'PORTA_INEN_SET', 'val': 0x80},
                       {'op': 'w', 'reg': 'PORTA_DATA_CLR', 'val': 0x1},
                       {'op': 'r', 'reg': 'PORTA_INEN', 'val': 0},
                       {'op': 'w', 'reg': 'PORTB_DIR_SET', 'val': 0x3}, {'op': 'w', 'reg': 'PORTB_DIR_CLR', 'val': 0x1}])
    assert st['A']['INEN'] == 0xC0 and st['B']['DIR'] == 0x2 and st['A']['DATA'] == 0
    assert buses._pin('PC11') == ('C', 1 << 11)


@group('spi frames from an MMR trace')
def _():
    B = 0x20057000
    t = ['w %x 13' % (B + 4), 'w %x 202' % (B + 0x18),
         'w %x 5' % (B + 0x58), 'w %x 0' % (B + 0x58),                 # pre-filled before the select
         'w %x 2' % (B + 0x18), 'r %x ff' % (B + 0x50), 'r %x 1' % (B + 0x50), 'w %x 202' % (B + 0x18),
         'r %x 7' % (B + 0x50),                                         # late read: the frame just ended
         'w %x 2' % (B + 0x18), 'w %x 3' % (B + 0x58), 'w %x 4' % (0x20055058), 'w %x 202' % (B + 0x18),
         'w %x 213' % (B + 4), 'w %x 2' % (B + 0x18), 'w %x 1234' % (B + 0x58), 'w %x 202' % (B + 0x18),
         'w %x 9' % (B + 0x58)]
    d = spiframes.SpiFrameDecoder(B)
    for e in mmrtrace.parse('\n'.join(t)):
        d.feed(e)
    f = d.finish()
    assert [(x['mosi'], x['miso']) for x in f] == [('0500', 'ff0107'), ('03', ''), ('1234', '')], f
    assert d.stray == 1 and all(x['ssel'] == 1 for x in f)
    assert spiframes.decode(mmrtrace.parse('\n'.join(t)), B, ssel=2) == []


@group('panel-bus trace reader')
def _():
    lines = ['0\tRESET\t1', '100\tCS\t0', '200\tCMD\t2A', '300\tDATA\t00 31 01 0E\tend=600',
             '700\tCMD\t2C', '800\tRUN\t3\tABCD\tend=1300', '1400\tCS\t1', '1500\tCMD\t11',
             '1600\tCS\t0', '1700\tCMD\tD3', '1800\tREAD\t00', '1900\tREAD\t77', '2000\tTE\t1',
             '2100\tMSG51\t3C FF 02', '2200\tMARK\theld', '2300\tCMD!\t29']
    tr = panel.Trace(lines)
    assert [(x.cmd, bytes(x.params), x.reads) for x in tr.txns] == [
        (0x2A, bytes.fromhex('0031010E'), []), (0x2C, bytes.fromhex('ABCD') * 3, []), (0x11, b'', []),
        (0xD3, b'', [0x00, 0x77]), (0x29, b'', [])]
    assert tr.txns[1].t_end == 1300 and tr.te == [2000] and tr.msg51 == [(2100, b'\x3c\xff\x02')]
    assert tr.marks == {'held': 2200} and len(tr.bus_errors) == 2, tr.bus_errors   # CMD 11 with CS high; CMD!
    assert panel.hexbytes('0a 0B') == b'\x0a\x0b'


@group('helpers (ID responder, TE, 0x51 tap)')
def _():
    r = helpers.IdResponder({0xD3: b'\x00\x00\x77\x96'})
    r.command(0xD3)
    assert [r.read() for _ in range(5)] == [0, 0, 0x77, 0x96, 0]
    r.cs(1)
    assert r.read() == 0
    r.command(0x04)
    assert r.read() == 0
    assert helpers.IdResponder().replies[0x04][1:3] == b'\x77\x96'
    t = helpers.te_times(0, 1_000_000_000, 60)
    assert len(t) == 60 and t[1] == 16_666_667
    assert helpers.te_lines(5, 20_000_000, 60, phase_ns=1) == ['6\tTE\t1', '16666673\tTE\t1']
    tap = helpers.Msg51Tap()
    tap.message(10, 0x51, b'\x3c\xff\x02')
    tap.message(11, 0x52, b'\x00')
    assert tap.lines == ['10\tMSG51\t3C FF 02']
    m = helpers.merge_lines(['5\tTE\t1', '20\tTE\t1'], tap.lines, ['5\tCS\t0'])
    assert m == ['5\tTE\t1', '5\tCS\t0', '10\tMSG51\t3C FF 02', '20\tTE\t1']
    tr = panel.Trace(m)
    assert tr.te == [5, 20] and tr.msg51 == [(10, b'\x3c\xff\x02')]


@group('tracediff masks')
def _():
    a = ['0\tRESET\t0', '10\tRESET\t1', '50\tCMD\t11', '60\tMSG51\t00 00 00', 'x hw_rev=4']
    b = ['0\tRESET\t0', '30\tRESET\t1', '70\tCMD\t11', '80\tMSG51\t00 00 00', 'x hw_rev=5']
    assert tracediff.diff(a, b) != []
    assert tracediff.diff(a, b, drop=[r'hw_rev='], windows={'a': [(0, 20)], 'b': [(0, 40)]}) == []
    assert tracediff.diff(a, b, subs=[(r'hw_rev=\d+', 'hw_rev=*')], windows=[(0, 40)]) == []
    assert tracediff.diff(a, b, subs=[(r'hw_rev=\d+', 'hw_rev=*')], windows=[(0, 40)], times=True) != []


def main():
    failed = 0
    for name, err in RESULTS:
        print('%-40s %s' % (name, 'ok' if err is None else 'FAIL ' + err))
        failed += err is not None
    print('%d groups, %d passed, %d failed' % (len(RESULTS), len(RESULTS) - failed, failed))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
