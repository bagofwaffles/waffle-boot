# SPDX-License-Identifier: GPL-2.0-or-later
"""BF70x bring-up order rules over an MMR trace (format: mmrtrace.py).

Checks the ADSP-BF70x HRM programming sequences (DMC start-up, CGU clock change and alignment, SEC reset
bits, watchdog hard-reset set-up, RTC 1 Hz writes). Each failure names the requirement it checks.

    python3 -I order.py init TRACE        (S-13a.008, S-13a.018, S-13a.019)
    python3 -I order.py init --entry-locked TRACE   (as init, entered with the PLL locked: S-13a.007)
    python3 -I order.py run TRACE         (S-13a.006, S-13a.007, S-13a.009, S-13a.020, S-13a.050)
    python3 -I order.py hardreset TRACE   (S-13a.061)
    python3 -I order.py rtc TRACE         (S-13a.071, S-13a.072)

Trace spans: `init` from the INIT stage's entry to its return; `run` from the main image's start to the
kernel call or reset; `hardreset` up to the reset; `rtc` the main image's run. Duration rules need @CYCLE
and are reported as not checked without it.

Exit status 0 when every checked rule holds. Python 3, standard library only.
"""
import sys

CGU_CTL, CGU_STAT, CGU_DIV = 0x20002000, 0x20002008, 0x2000200C
PHY_CTL0, PHY_CTL3, PHY_CTL4 = 0x200C1000, 0x200C100C, 0x200C1010
PADCTL0, PADCTL2 = 0x200C1034, 0x200C103C
DMC_CTL, DMC_STAT = 0x200C0004, 0x200C0008
DMC_PROG = {0x200C0040: 'CFG', 0x200C0044: 'TR0', 0x200C0048: 'TR1', 0x200C004C: 'TR2',
            0x200C0060: 'MR', 0x200C0064: 'EMR1', 0x200C0068: 'EMR2'}
RCU_CTL, RCU_MSG, RCU_MSG_CLR, RCU_BCODE = 0x20000000, 0x20000060, 0x20000068, 0x2000001C
SEC_GCTL, SEC_FCTL, SEC_SCTL3 = 0x20004000, 0x20004010, 0x20004818
SEC_CCTL0, SEC_CPMSK0, SEC_CPLVL0 = 0x20004400, 0x20004410, 0x20004418
SEC_SCTL_LO, SEC_SCTL_HI = 0x20004800, 0x20004B50  # SEC0_SCTLn at 0x20004800 + 8n; the odd words are SSTATn
SEC_RESET = 0x2                                    # bit 1 (RESET) of SEC0_GCTL, SEC0_FCTL and SEC0_CCTLn
WDOG_CTL, WDOG_CNT = 0x20046000, 0x20046004
CGU_CLKOUTSEL, DPM_CTL = 0x20002010, 0x20003000
CGU_DIV_UPDT = 1 << 30      # CGU0_DIV.UPDT: starts a clock alignment
RTC_STAT, RTC_INIT = 0x200D128C, 0x200D1298
DDR_LO = 0x80000000

CCLK_PER_DCLK = 2            # S-13a.001: CCLK 400 MHz, DCLK 200 MHz
PAD_CAL_DCLK = 300           # HRM: pad calibration completes in 300 DCLK cycles
DLL_LOCK_DCLK = 4500         # HRM: wait 4500 DCLK cycles for the DLL to lock

if __package__:
    from .mmrtrace import parse, first, last, w, duration  # noqa: F401
else:                                # run as a script
    import os
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from hw.mmrtrace import parse, first, last, w, duration  # noqa: F401


def check_init(ev, entry_locked=False):
    """entry_locked: the stage was entered with CGU0_CTL at CGU_FIELDS and PLOCK = 1. CGU0_CTL and CGU0_STAT
    reads with those values before the first CGU0_CTL write show the same."""
    out = {'fail': [], 'unchecked': []}
    F = out['fail']
    # S-13a.007
    plock = ctl_ok = entry_locked
    for e in ev:
        if e['k'] == 'r' and e['v'] is not None and e['a'] == CGU_STAT:
            plock = entry_locked or bool(e['v'] & 0x4)
        elif e['k'] == 'r' and e['v'] is not None and e['a'] == CGU_CTL:
            mask, val = CGU_FIELDS[CGU_CTL]
            ctl_ok = entry_locked or (e['v'] & mask) == val
        elif e['k'] == 'w' and e['a'] == CGU_CTL:
            if plock and ctl_ok:
                F.append('S-13a.007: CGU0_CTL written while it holds S-13a.002\'s fields and PLOCK = 1 (a relock)')
            break
    cgu_w = lambda e: e['k'] == 'w' and e['a'] in (CGU_CTL, CGU_DIV)
    prog_w = lambda e: e['k'] == 'w' and e['a'] in DMC_PROG
    ret = first(ev, lambda e: e['k'] == 'ret')
    end = ret if ret is not None else len(ev)
    c0, c1 = first(ev, cgu_w), last(ev, cgu_w)
    p0 = first(ev, prog_w)
    if c0 is None:
        F.append('S-13a.018: no CGU0_CTL or CGU0_DIV write')
    if p0 is None:
        F.append('S-13a.018: no DMC configuration write')
        return out
    # S-13a.008: CGU0_DIV before CGU0_CTL
    ctl0 = first(ev, lambda e: e['k'] == 'w' and e['a'] == CGU_CTL)
    div0 = first(ev, lambda e: e['k'] == 'w' and e['a'] == CGU_DIV)
    if ctl0 is not None and (div0 is None or div0 > ctl0):
        F.append('S-13a.008: CGU0_CTL written before CGU0_DIV: the PLL relocks on the dividers it was entered with')
    # S-13a.008: CLKSALGN read clear before each CGU0_DIV write
    for i in range(end):
        if ev[i]['k'] == 'w' and ev[i]['a'] == CGU_DIV:
            ok, j = False, i - 1
            while j >= 0 and not cgu_w(ev[j]):
                if ev[j]['k'] == 'r' and ev[j]['a'] == CGU_STAT and ev[j]['v'] is not None:
                    ok = not ev[j]['v'] & 0x8          # the last read before the write decides
                    break
                j -= 1
            if not ok:
                F.append('S-13a.008: CGU0_DIV written without a CGU0_STAT read showing CLKSALGN clear before it')
    # S-13a.008: PHY_CTL0 bit 11 around the clock change
    s = first(ev, w(PHY_CTL0, 0x800))
    clr = None
    if c0 is not None:
        if s is None or s > c0:
            F.append('S-13a.008: DMC0_PHY_CTL0 bit 11 not set before the first CGU write')
        clr = first(ev, w(PHY_CTL0, 0x800, zero=True), c1)
        if clr is None or clr > p0:
            F.append('S-13a.008: DMC0_PHY_CTL0 bit 11 not cleared after the clock change and before DMC programming')
        lock = first(ev, lambda e: e['k'] == 'r' and e['a'] == CGU_STAT and e['v'] & 0x4 and not e['v'] & 0xA, c1)
        if lock is None or lock > p0:
            F.append('S-13a.018: no CGU0_STAT read showing PLOCK set, PLLBP clear and CLKSALGN clear between the '
                     'last CGU write and DMC programming')
        if clr is not None and lock is not None and clr < lock:
            F.append('S-13a.008: DMC0_PHY_CTL0 bit 11 cleared before the PLL was seen locked')
    # S-13a.018: pad calibration
    m4 = first(ev, w(PHY_CTL4))
    m3 = first(ev, w(PHY_CTL3))
    m2 = first(ev, w(PADCTL2))
    cal = first(ev, w(PADCTL0, 1 << 28))
    if cal is None or cal > p0:
        F.append('S-13a.018: CAL_PADCTL0.CALSTRT not set before DMC programming')
    else:
        for i, name in ((m4, 'DMC0_PHY_CTL4'), (m3, 'DMC0_PHY_CTL3'), (m2, 'DMC0_CAL_PADCTL2')):
            if i is None or i > cal:
                F.append('S-13a.018: %s not written before CALSTRT' % name)
        duration(ev, cal, p0, PAD_CAL_DCLK * CCLK_PER_DCLK, 'S-13a.018 CALSTRT to DMC programming', out)
    if clr is not None and clr < p0:
        # the DLL lock time counts from the hold's release
        duration(ev, clr, p0, DLL_LOCK_DCLK * CCLK_PER_DCLK, 'S-13a.018 PHY hold release to DMC programming', out)
    elif c1 is not None:
        duration(ev, c1, p0, DLL_LOCK_DCLK * CCLK_PER_DCLK, 'S-13a.018 last clock change to DMC programming', out)
    # S-13a.018: DMC registers before CTL.INIT
    start = first(ev, w(DMC_CTL, 0x4))
    if start is None:
        F.append('S-13a.018: DMC0_CTL.INIT never set')
        return out
    if not ev[start]['v'] & 0x2000:
        F.append('S-13a.018: DMC0_CTL.INIT set without DLLCAL')
    for a, name in DMC_PROG.items():
        i = first(ev, w(a))
        if i is None or i > start:
            F.append('S-13a.018: DMC0_%s not written before DMC0_CTL.INIT' % name)
    # S-13a.019: INITDONE
    done = first(ev, lambda e: e['k'] == 'r' and e['a'] == DMC_STAT and e['v'] & 0x4, start)
    if done is None or done > end:
        F.append('S-13a.019: DMC0_STAT.INITDONE not seen before the INIT stage returns')
    if first(ev, lambda e: e['k'] == 'm') is None:
        out['unchecked'].append('S-13a.019 no DDR access before INITDONE (trace has no memory events)')
    else:
        ddr = first(ev, lambda e: e['k'] == 'm' and e['a'] is not None and e['a'] >= DDR_LO)
        if ddr is not None and (done is None or ddr < done):
            F.append('S-13a.019: DDR accessed at %#x before INITDONE' % ev[ddr]['a'])
    return out


# S-13a.002, S-13a.003, S-13a.005: (mask, value) of the clock fields, for CLKIN = 25 MHz
CGU_FIELDS = {CGU_CTL: (0x7F01, 0x2000),                  # MSEL 14:8 = 32, DF 0 = 0
              CGU_DIV: (0x1FDFFFFF, 0x04042442),          # OSEL, DSEL, S1SEL, SYSSEL, S0SEL, CSEL
              CGU_CLKOUTSEL: (0x1F, 0x1)}                 # CLKOUTSEL 4:0 = 1
DMC_LO, DMC_HI = 0x200C0000, 0x200C2000


def is_sctl(a):
    return SEC_SCTL_LO <= a < SEC_SCTL_HI and (a - SEC_SCTL_LO) % 8 == 0


def check_run(ev):
    out = {'fail': [], 'unchecked': []}
    F = out['fail']
    aligned = False         # S-13a.009: a CGU0_STAT read with CLKSALGN clear since the last CGU write
    sctl_seen = False       # S-13a.050: some SEC0_SCTLn has been written
    cprio_seen = False      # S-13a.050: SEC0_CPMSK0 or SEC0_CPLVL0 has been written
    for e in ev:
        if e['k'] == 'r' and e['a'] == CGU_STAT and e['v'] is not None:
            if not e['v'] & 0x8:                      # CGU_STAT.CLKSALGN (bit 3) clear
                aligned = True
            continue
        if e['k'] != 'w':
            continue
        a, v = e['a'], e['v']
        if a == CGU_DIV and not aligned:
            F.append('S-13a.009: CGU0_DIV = %#x written without a CGU0_STAT read showing CLKSALGN clear '
                     'since the previous CGU write' % v)
        if a in (CGU_CTL, CGU_DIV):
            aligned = False
        if is_sctl(a):
            sctl_seen = True
        elif a in (SEC_CPMSK0, SEC_CPLVL0):
            cprio_seen = True
        elif a in (SEC_GCTL, SEC_FCTL) and v & SEC_RESET and sctl_seen:
            F.append('S-13a.050: %s = %#x sets RESET after a SEC0_SCTLn write'
                     % ('SEC0_GCTL' if a == SEC_GCTL else 'SEC0_FCTL', v))
        elif a == SEC_CCTL0 and v & SEC_RESET and cprio_seen:
            F.append('S-13a.050: SEC0_CCTL0 = %#x sets RESET after a SEC0_CPMSK0 or SEC0_CPLVL0 write' % v)
        if a in CGU_FIELDS:
            mask, want = CGU_FIELDS[a]
            if v & mask != want:
                F.append('S-13a.007: CGU write %#x = %#x changes a clock field' % (a, v))
            if a == CGU_DIV and v & CGU_DIV_UPDT:
                F.append('S-13a.007: CGU0_DIV = %#x sets UPDT (a clock alignment) while running from DDR' % v)
        elif a == DPM_CTL and v & 0x18:
            F.append('S-13a.006: DPM0_CTL = %#x requests deep sleep or hibernate' % v)
        elif DMC_LO <= a < DMC_HI:
            F.append('S-13a.020: DMC register %#x written after the INIT stage' % a)
    return out


RTC_1HZ = {0x200D1280: 'CLK', 0x200D1284: 'ALM', 0x200D1290: 'STPWTCH', RTC_INIT: 'INIT'}


def check_rtc(ev):
    out = {'fail': [], 'unchecked': []}
    F = out['fail']
    init_seen = False
    pending = None          # the last 1 Hz register written, until STAT shows the write finished
    cal = None              # S-13a.071: RTC0_INIT.CAL (bits 3:0) of the last RTC0_INIT read
    for e in ev:
        if e['k'] == 'r' and e['a'] == RTC_INIT and e['v'] is not None:
            cal = e['v'] & 0xF
        if e['k'] == 'w' and e['a'] == RTC_INIT:
            if cal is None:
                F.append('S-13a.071: RTC0_INIT = %#x written without a preceding RTC0_INIT read' % e['v'])
            elif e['v'] & 0xF != cal:
                F.append('S-13a.071: RTC0_INIT = %#x changes CAL from %#x' % (e['v'], cal))
        if e['k'] == 'r' and e['a'] == RTC_STAT and e['v'] is not None:
            if not e['v'] & 0x1:                      # RTC_STAT.WRPEND (bit 0) clear
                pending = None
        elif e['k'] == 'w' and e['a'] in RTC_1HZ:
            name = RTC_1HZ[e['a']]
            if name != 'INIT' and not init_seen:
                F.append('S-13a.072: RTC0_%s written before RTC0_INIT' % name)
            if pending is not None:
                F.append('S-13a.072: RTC0_%s written while the RTC0_%s write may be pending' % (name, pending))
            init_seen = init_seen or name == 'INIT'
            pending = name
    return out


def check_hardreset(ev):
    out = {'fail': [], 'unchecked': []}
    F = out['fail']
    arm = first(ev, lambda e: e['k'] == 'w' and e['a'] == WDOG_CTL and ((e['v'] >> 4) & 0xFF) != 0xAD)
    if arm is None:
        F.append('S-13a.061: WDOG0 never enabled')
        return out
    cnt = last(ev[:arm], w(WDOG_CNT))
    if cnt is None:
        F.append('S-13a.061: WDOG0_CNT not written before the watchdog is enabled')
    elif ev[cnt]['v'] == 0:
        F.append('S-13a.061: WDOG0_CNT is 0')
    pre = ev[:arm]
    for pred, msg in ((w(RCU_CTL, 1 << 8), 'RCU0_CTL.SRSTREQEN'),
                      (w(SEC_GCTL, 1), 'SEC0_GCTL.EN'),
                      (w(SEC_SCTL3, 0x6), 'SEC0_SCTL3.SEN and FEN'),
                      (w(SEC_FCTL, 0x41), 'SEC0_FCTL.EN and SREN'),
                      (w(RCU_BCODE, 0x10), 'RCU0_BCODE.NOMEMINIT'),
                      (lambda e: e['k'] == 'w' and ((e['a'] == RCU_MSG_CLR and e['v'] & (1 << 16)) or
                                                    (e['a'] == RCU_MSG and not e['v'] & (1 << 16))),
                       'RCU0_MSG.C0L1INIT clear')):
        if last(pre, pred) is None:
            F.append('S-13a.061: %s not written before the watchdog is enabled' % msg)
    # the fault route starts at the first write that sets SEC0_SCTL3.FEN or SEC0_FCTL.SREN
    route = first(pre, lambda e: w(SEC_SCTL3, 0x2)(e) or w(SEC_FCTL, 0x40)(e))
    if route is not None:
        for e in pre[route + 1:]:
            if e['k'] == 'w' and e['a'] in (SEC_GCTL, SEC_FCTL, SEC_CCTL0) and e['v'] & SEC_RESET:
                F.append('S-13a.061: %#x = %#x sets RESET after the watchdog fault route is set up'
                         % (e['a'], e['v']))
    after = first(ev, lambda e: e['k'] == 'w', arm + 1)
    if after is not None:
        F.append('S-13a.061: MMR write to %#x after the watchdog is enabled' % ev[after]['a'])
    return out


def main(argv):
    modes = {'init': check_init, 'run': check_run, 'hardreset': check_hardreset, 'rtc': check_rtc}
    locked = len(argv) == 4 and argv[1] == 'init' and argv[2] == '--entry-locked'
    if locked:
        argv = argv[:2] + argv[3:]
    if len(argv) != 3 or argv[1] not in modes:
        print(__doc__)
        return 2
    with open(argv[2]) as f:
        ev = parse(f.read())
    out = check_init(ev, entry_locked=True) if locked else modes[argv[1]](ev)
    for x in out['fail']:
        print('FAIL', x)
    for x in out['unchecked']:
        print('NOT CHECKED', x)
    print('%s: %d failed, %d not checked' % (argv[1], len(out['fail']), len(out['unchecked'])))
    return 1 if out['fail'] else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
