# SPDX-License-Identifier: GPL-2.0-or-later
"""Bus decoders and trace checkers for BF70x boards: SPI, SPI-NOR flash frames, eMMC commands, UART,
GPIO ports. Standard library only; synthetic inputs only.

Trace formats:
- port trace:  [{"op": "w"|"r", "reg": "PORTA_MUX", "val": int, "t_ns": float (optional)} | {"op": "mark"}, ...]
               (BF70x register names; the SET/CLR registers are views of the one register, as the HRM
               defines them)
- flash trace: [{"mosi": "hex", "miso": "hex"}, ...]                    one entry per chip-select frame
               (spiframes.py builds it from an MMR trace)
- eMMC trace:  [{"ev": "clock", "hz": int} | {"ev": "width", "bits": int} | {"ev": "fail"} |
                {"ev": "busy_end"} | {"ev": "cmd", "idx": int, "arg": int, "resp": int, "t_ms": float}, ...]
Each checker returns a list of (rule, message) violations; an empty list passes. Register fields are from
the ADSP-BF70x HRM (SPI ch. 29, UART, PORT ch. 20, MSI); flash opcodes are the JEDEC SPI-NOR set; eMMC
commands are JEDEC JESD84 (eMMC). Board values come in as arguments (rule tables, RCA, OCR argument,
allowed EXT_CSD indexes).
"""

SCLK0 = 100_000_000

# --- SPI -----------------------------------------------------------------------------------------
def decode_spi(ctl, clk, sclk0=SCLK0):
    """SPI_CTL / SPI_CLK fields (ADSP-BF70x HRM ch. 29) as bus properties."""
    cpha, cpol = (ctl >> 4) & 1, (ctl >> 5) & 1
    return {
        'enabled': ctl & 1, 'master': (ctl >> 1) & 1, 'cpha': cpha, 'cpol': cpol,
        'mode': {(0, 0): 0, (0, 1): 1, (1, 0): 2, (1, 1): 3}[(cpol, cpha)],
        'hw_select': (ctl >> 6) & 1, 'word_bits': {0: 8, 1: 16, 2: 32}.get((ctl >> 9) & 3),
        'lsb_first': (ctl >> 12) & 1, 'flow_control': (ctl >> 13) & 1, 'flow_on_tx': (ctl >> 14) & 1,
        'rdy_active_high': (ctl >> 15) & 1, 'flow_watermark': (ctl >> 16) & 3, 'fast_mode': (ctl >> 18) & 1,
        'io': {0: 'single', 1: 'dual', 2: 'quad'}.get((ctl >> 20) & 3), 'mmap': (ctl >> 31) & 1,
        'sck_hz': sclk0 / ((clk & 0xFFFF) + 1),
    }



def check_spi_config(rules, kind, ctl, clk, sclk0=SCLK0):
    """Check SPI_CTL / SPI_CLK against rules[kind]: {property: allowed set, 'sck_max': Hz while enabled}."""
    d, out = decode_spi(ctl, clk, sclk0), []
    for prop, allowed in rules[kind].items():
        if prop == 'sck_max':
            if d['enabled'] and d['master'] and d['sck_hz'] > allowed:
                out.append(('sck', '%s: SCK %.0f Hz above %.0f' % (kind, d['sck_hz'], allowed)))
        elif d[prop] not in allowed:
            out.append((prop, '%s: %s = %r, allowed %r' % (kind, prop, d[prop], sorted(allowed, key=str))))
    return out


# --- SPI-NOR flash frames (JEDEC opcodes) -------------------------------------------------------

READ, RDSR, WREN, SE, PP, REMS, RDID = 0x03, 0x05, 0x06, 0x20, 0x02, 0x90, 0x9F
OPCODES = {READ, RDSR, WREN, SE, PP, REMS, RDID}


def _addr(b):
    return b[1] << 16 | b[2] << 8 | b[3]


def check_flash(frames, size):
    """JEDEC SPI-NOR frame rules: the opcode set, frame shapes, no command but RDSR while WIP may be 1 (the
    last status byte of an RDSR frame clears it), addresses below `size`, WREN directly before SE/PP, 4 KB
    sector and 256-byte page alignment."""
    out, busy, armed = [], False, False
    for n, f in enumerate(frames):
        mosi, miso = bytes.fromhex(f['mosi']), bytes.fromhex(f.get('miso', ''))
        if not mosi:
            out.append(('frame_shape', '#%d empty frame' % n))
            continue
        op = mosi[0]
        if op not in OPCODES:
            out.append(('opcode_set', '#%d opcode 0x%02x' % (n, op)))
            continue
        shape = {READ: len(mosi) >= 5, RDSR: len(mosi) >= 2, WREN: len(mosi) == 1, SE: len(mosi) == 4,
                 PP: 5 <= len(mosi) <= 4 + 256, REMS: len(mosi) >= 6 and mosi[1:4] == b'\0\0\0',
                 RDID: len(mosi) >= 4}[op]
        if not shape:
            out.append(('frame_shape', '#%d opcode 0x%02x length %d' % (n, op, len(mosi))))
            continue
        if op == RDSR:
            if len(miso) >= 2 and not (miso[-1] & 1):     # the last status byte of the frame
                busy = False
            continue
        if busy:
            out.append(('busy_wait', '#%d opcode 0x%02x while WIP may be 1' % (n, op)))
        if op in (READ, SE, PP) and _addr(mosi) >= size:
            out.append(('address_range', '#%d address 0x%06x past the flash' % (n, _addr(mosi))))
        if op in (SE, PP):
            if not armed:
                out.append(('wren_before_write', '#%d opcode 0x%02x without WREN before it' % (n, op)))
            if op == SE and _addr(mosi) & 0xFFF:
                out.append(('alignment', '#%d SE at 0x%06x' % (n, _addr(mosi))))
            if op == PP and (_addr(mosi) & 0xFF) + len(mosi) - 4 > 256:
                out.append(('alignment', '#%d PP crosses a page' % n))
            busy = True
        armed = op == WREN
    return out




# --- eMMC commands (JESD84) ----------------------------------------------------------------

READY = 1 << 8
DATA_CMDS = {6, 8, 17, 18, 23, 24, 25, 35, 36, 38}


def msi_emmc_clock(clkdiv, source_hz=SCLK0 // 2):
    return source_hz if clkdiv == 0 else source_hz / (2 * clkdiv)


def switch_fields(arg):
    return (arg >> 24) & 3, (arg >> 16) & 0xFF, (arg >> 8) & 0xFF


def check_switches(events, indexes):
    """Every CMD6 is a write-byte access to an EXT_CSD index in `indexes` and is followed only by CMD13 until
    the eMMC is ready (R1 READY_FOR_DATA, or a busy_end event)."""
    out, waiting = [], None
    for e in events:
        if e['ev'] == 'busy_end':
            waiting = None
            continue
        if e['ev'] != 'cmd':
            continue
        if waiting is not None:
            if e['idx'] == 13:
                if e.get('resp', 0) & READY:
                    waiting = None
                continue
            out.append(('emmc_switch', 'CMD%d before the busy wait of switch %d ended' % (e['idx'], waiting)))
            waiting = None
        if e['idx'] == 6:
            access, idx, _ = switch_fields(e['arg'])
            if access != 3 or idx not in indexes:
                out.append(('emmc_switch', 'CMD6 arg 0x%08x' % e['arg']))
            waiting = idx
    return out


XFER_CMDS = {8, 17, 18, 24, 25}       # commands that move data on DAT[7:0]
POWER_OFF_LONG = 0x03220300          # Switch(34, 3)


def check_init(events, rca, ocr_arg, indexes, cmd1_bound_ms=1000):
    """eMMC bring-up: the identification order (CMD0, CMD1 with `ocr_arg` until OCR bit 31, CMD2, CMD3 with
    `rca`, optional CMD9, CMD7), the identification clock, the OCR loop, the clock before the
    timing switch, the bus width on data transfers and at the end."""
    out, cmds = [], [e for e in events if e['ev'] == 'cmd']
    # clock and width, event by event
    clock, host_w, emmc_w = None, 1, 1
    seen7 = hs_done = ext_csd_read = width_switched = False
    pending = None                                   # the switch whose busy period has not ended
    for e in events:
        ev = e['ev']
        if ev == 'clock':
            clock = e['hz']
            if seen7 and not hs_done and clock > 26_000_000:
                out.append(('compat_clock', 'eMMC clock %d Hz before the HS_TIMING switch' % clock))
            continue
        if ev == 'width':
            host_w = e['bits']
            continue
        if ev == 'cmd' and e['idx'] == 13 and pending is None:
            continue
        if ev == 'busy_end' or (ev == 'cmd' and e['idx'] == 13 and e.get('resp', 0) & READY):
            if pending is not None:
                _, idx, val = switch_fields(pending['arg'])
                if idx == 183:
                    emmc_w = {0: 1, 1: 4, 2: 8}.get(val, emmc_w)
                if idx == 185 and val:
                    hs_done = True
                pending = None
            continue
        if ev != 'cmd':
            continue
        idx = e['idx']
        if not seen7:
            if clock is None or clock > 400_000:
                out.append(('ident_clock', 'CMD%d at %s Hz' % (idx, clock)))
            if host_w != 1:
                out.append(('ident_width', 'CMD%d on a %d-bit bus' % (idx, host_w)))
        if idx in XFER_CMDS and host_w != emmc_w:
            out.append(('bus_width_match', 'CMD%d with the host at %d bits, the eMMC at %d' % (idx, host_w, emmc_w)))
        if seen7 and idx == 8:
            ext_csd_read = True
        if idx == 6:
            _, sidx, _ = switch_fields(e['arg'])
            if sidx == 185 and not ext_csd_read:
                out.append(('emmc_init', 'HS_TIMING switch before EXT_CSD was read'))
            if sidx == 183 and seen7:
                width_switched = True
            pending = e
        seen7 = seen7 or idx == 7
    # the identification order
    i = 0
    if not cmds or cmds[0]['idx'] != 0:
        return out + [('emmc_init', 'does not start with CMD0')]
    while i < len(cmds) and cmds[i]['idx'] == 0:
        i += 1
    t0 = cmds[i]['t_ms'] if i < len(cmds) and 't_ms' in cmds[i] else 0
    ready, last_t = False, t0
    while i < len(cmds) and cmds[i]['idx'] == 1:
        if cmds[i]['arg'] != ocr_arg:
            out.append(('emmc_init', 'CMD1 arg 0x%08x' % cmds[i]['arg']))
        ready = bool(cmds[i].get('resp', 0) >> 31)
        last_t = cmds[i].get('t_ms', 0)
        i += 1
        if ready:
            break
    if not ready:
        failed = any(e['ev'] == 'fail' for e in events)
        if failed and last_t - t0 < cmd1_bound_ms:
            out.append(('cmd1_bound', 'gave up after %.0f ms, before %d ms' % (last_t - t0, cmd1_bound_ms)))
        return out if failed else out + [('emmc_init', 'no CMD1 with OCR bit 31')]
    for idx, arg, optional in ((2, 0, False), (3, rca << 16, False), (9, rca << 16, False), (7, rca << 16, False)):
        got = (cmds[i]['idx'], cmds[i]['arg']) if i < len(cmds) else None
        if got == (idx, arg):
            i += 1
        elif not optional:
            return out + [('emmc_init', 'expected CMD%d 0x%08x, got %r' % (idx, arg, got))]
    # after CMD7 the order is free; the end state is not
    if not width_switched:
        out.append(('emmc_init', 'no Switch(183, 2) after CMD7'))
    if (host_w, emmc_w) != (8, 8):
        out.append(('emmc_init', 'bus width at the end: host %d, eMMC %d' % (host_w, emmc_w)))
    return out + check_switches(events, indexes)


def check_power_off(events):
    """No command after Switch(34, 3), the long power-off notification."""
    out, notified = [], False
    for e in events:
        if e['ev'] != 'cmd':
            continue
        if notified:
            out.append(('power_off_quiet', 'CMD%d after the power-off notification' % e['idx']))
        if e['idx'] == 6 and e['arg'] == POWER_OFF_LONG:
            notified = True
    return out


def check_transfers(events):
    """Transfer command forms (CMD18 ended by CMD12, CMD25 with CMD23 or CMD12, CMD36 matching CMD35) and
    the ready wait after writes and erases."""
    out, cmds = [], [e for e in events if e['ev'] == 'cmd']
    need_ready, erase_start = False, None
    for n, c in enumerate(cmds):
        idx = c['idx']
        if idx == 13:
            if c.get('resp', 0) & READY:
                need_ready = False
            continue
        if need_ready and idx in DATA_CMDS:
            out.append(('emmc_transfer', 'CMD%d before READY_FOR_DATA' % idx))
            need_ready = False
        nxt = cmds[n + 1]['idx'] if n + 1 < len(cmds) else None
        prev = cmds[n - 1]['idx'] if n else None
        if idx == 18 and nxt != 12:
            out.append(('emmc_transfer', 'CMD18 not ended by CMD12'))
        if idx == 25 and prev != 23 and nxt != 12:
            out.append(('emmc_transfer', 'CMD25 without CMD23 or CMD12'))
        if idx == 35:
            erase_start = c['arg']
        if idx == 36 and c['arg'] != erase_start:
            out.append(('emmc_transfer', 'CMD36 0x%x differs from CMD35 0x%x' % (c['arg'], erase_start)))
        if idx in (24, 38) or (idx == 12 and prev == 25) or (idx == 25 and prev == 23):
            need_ready = True
    return out



# --- UART ---------------------------------------------------------------------------------------

def decode_uart(ctl, clk, sclk0=SCLK0):
    div = clk & 0xFFFF
    edbo = (clk >> 31) & 1
    return {
        'enabled': ctl & 1, 'loop': (ctl >> 1) & 1, 'mode': (ctl >> 4) & 3, 'data_bits': 5 + ((ctl >> 8) & 3),
        'stop_bits': 2 if (ctl >> 12) & 1 else 1, 'parity': bool((ctl >> 14) & 1), 'fcpol': (ctl >> 22) & 1,
        'arts': (ctl >> 27) & 1, 'acts': (ctl >> 28) & 1,
        'baud': sclk0 / (div * (1 if edbo else 16)) if div else 0,
    }



def check_uart_config(link, ctl, clk, baud, data_bits=8, parity=False, stop_bits=1, flow=(0, 0), fcpol=None,
                      tolerance=0.01, sclk0=SCLK0):
    """UART_CTL / UART_CLK against a line setting: baud within tolerance, framing, normal mode, no loop-back,
    ARTS/ACTS = flow, and FCPOL when given."""
    d, out = decode_uart(ctl, clk, sclk0), []
    if abs(d['baud'] - baud) > baud * tolerance:
        out.append(('baud', '%s: %.0f baud' % (link, d['baud'])))
    for prop, want in (('data_bits', data_bits), ('parity', parity), ('stop_bits', stop_bits), ('mode', 0),
                       ('loop', 0)):
        if d[prop] != want:
            out.append((prop, '%s: %s = %r' % (link, prop, d[prop])))
    if (d['arts'], d['acts']) != tuple(flow):
        out.append(('flow', '%s: ARTS/ACTS = %r, want %r' % (link, (d['arts'], d['acts']), tuple(flow))))
    if fcpol is not None and d['fcpol'] != fcpol:
        out.append(('fcpol', '%s: FCPOL %d' % (link, d['fcpol'])))
    return out


# --- GPIO ports (HRM ch. 20) ---------------------------------------------------------------------

PORTS = 'ABC'


def _pin(name):
    return name[1], 1 << int(name[2:])


def _blank():
    return {p: {'FER': 0, 'MUX': 0, 'DIR': 0, 'INEN': 0, 'DATA': 0} for p in PORTS}


def replay_one(st, e):
    """Port state after one write; a SET/CLR register sets/clears bits of the one register (HRM)."""
    st = {p: dict(v) for p, v in st.items()}
    if e.get('op') != 'w' or not e['reg'].startswith('PORT'):
        return st
    port, reg = e['reg'][4], e['reg'][6:]
    base, _, kind = reg.partition('_')
    if base in st[port]:
        if kind == 'SET':
            st[port][base] |= e['val']
        elif kind == 'CLR':
            st[port][base] &= ~e['val']
        else:
            st[port][base] = e['val']
    return st


def replay(trace):
    st = _blank()
    for e in trace:
        st = replay_one(st, e)
    return st
