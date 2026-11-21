#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""S-13b decoders and trace checkers. Standard library only; synthetic inputs only.

Neutral trace formats:
- port trace:  [{"op": "w"|"r", "reg": "PORTA_MUX", "val": int, "t_ns": float (optional)} | {"op": "mark"}, ...]
               (BF70x register names; the SET/CLR registers are aliases of the one register; "mark" stands
               where the first SPI, UART or MSI register access is; the trace ends at the hand-off)
- eMMC trace:  [{"ev": "clock", "hz": int} | {"ev": "width", "bits": int} | {"ev": "fail"} |
                {"ev": "busy_end"} | {"ev": "cmd", "idx": int, "arg": int, "resp": int, "t_ms": float}, ...]
- bridge pin trace (slave): port events as above plus {"op": "w", "reg": "SPI2_CTL", "val": int} and
               {"op": "r", "reg": "PORTB_DATA", "val": int}, and {"op": "ev", "ev": "spi2_error" | "give_up"}
               where the slave gives a transfer up
- slave transmit trace (S-13b.045): see `check_slave_tx`
- master's reset between sessions (S-13b.040): see `check_master_sessions`
- the slave's reply set-up (S-13b.044): see `check_reply_setup`
- a stage's SPI register writes with its frames (S-13b.010, .019): see `check_spi_start`
- the flash at a stage's end (S-13b.015): see `check_flash_end`
- the UART at its disable (S-13b.072): see `check_uart_close`
- a bridge session moved by DMA (S-13b.083): see `check_bridge_dma`
- a part of the slave boot stream moved by DMA under flow control (S-13b.023): see `check_boot_dma`
Each checker returns a list of (rule, message) violations; an empty list passes.

The generic decoders, the port model and the flash/eMMC/UART checkers are the conformance package's
`hw.buses`; this file holds the S-13b tables (SPI rules, RCA, OCR argument, switch indexes, pin mux, GPIO
states, strap bits), binds them, and adds the S-13b-only checks (ports, strap timing, PA4, timing).
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'conformance'))
from hw import buses as _B  # noqa: E402
from hw.buses import (SCLK0, decode_spi, READ, RDSR, WREN, SE, PP, REMS, RDID, OPCODES, _addr,  # noqa: E402,F401
                      READY, DATA_CMDS, XFER_CMDS, POWER_OFF_LONG, msi_emmc_clock, switch_fields,
                      check_power_off, check_transfers, decode_uart, PORTS, _pin, _blank, replay_one, replay)

# --- SPI -----------------------------------------------------------------------------------------

# per S-13b.010/.011, .020/.021, .040-.042: property -> allowed values
SPI_RULES = {
    'flash':             {'master': {1}, 'hw_select': {0}, 'word_bits': {8}, 'lsb_first': {0}, 'io': {'single'},
                          'mode': {0, 3}, 'mmap': {0}, 'sck_max': 50e6},
    # S-13b.022: flow control is free on the boot link; with it, PA6 low is ready
    'slave_boot':        {'master': {1}, 'hw_select': {0}, 'word_bits': {8}, 'lsb_first': {0}, 'io': {'single'},
                          'mode': {1}, 'rdy_active_high': {0}, 'sck_max': SCLK0 / 3 + 1},
    'bridge_master_pre': {'master': {1}, 'word_bits': {32}, 'lsb_first': {0}, 'io': {'single'}, 'mode': {1},
                          'flow_control': {1}, 'rdy_active_high': {0}, 'sck_max': 25e6},
    'bridge_master_post': {'master': {1}, 'word_bits': {32}, 'lsb_first': {0}, 'io': {'single'}, 'mode': {1},
                           'flow_control': {1}, 'rdy_active_high': {0}, 'sck_max': 25e6},
    'bridge_slave_pre':  {'master': {0}, 'word_bits': {32}, 'lsb_first': {0}, 'io': {'single'}, 'mode': {1},
                          'flow_control': {1}},
    'bridge_slave_post': {'master': {0}, 'word_bits': {32}, 'lsb_first': {0}, 'io': {'single'}, 'mode': {1},
                          'flow_control': {1}},
    # S-13b.041, .042: the quad I/O option after the link, off by default
    'bridge_master_quad_opt': {'master': {1}, 'word_bits': {32}, 'lsb_first': {0}, 'io': {'quad'}, 'mode': {1},
                               'flow_control': {1}, 'rdy_active_high': {0}, 'fast_mode': {1}, 'sck_max': 50e6},
    'bridge_slave_quad_opt': {'master': {0}, 'word_bits': {32}, 'lsb_first': {0}, 'io': {'quad'}, 'mode': {1},
                              'flow_control': {1}},
}
# S-13b.041, S-09.075: the master's request sessions on rungs 1-4 of the link
_RUNG = {'master': {1}, 'word_bits': {32}, 'lsb_first': {0}, 'mode': {1}, 'flow_control': {1}, 'rdy_active_high': {0}}
SPI_RULES['bridge_master_rung1'] = dict(_RUNG, io={'single'}, sck_max=25e6)
SPI_RULES['bridge_master_rung2'] = dict(_RUNG, io={'single'}, fast_mode={1}, sck_max=50e6)
SPI_RULES['bridge_master_rung3'] = dict(_RUNG, io={'quad'}, fast_mode={1}, sck_max=25e6)
SPI_RULES['bridge_master_rung4'] = dict(_RUNG, io={'quad'}, fast_mode={1}, sck_max=50e6)
# per S-13b.044: while PA4 is handed to SPI2, flow control follows the session's direction
_REPLY = {'enabled': {1}, 'flow_on_tx': {1}, 'flow_watermark': {0}, 'rdy_active_high': {0}}     # transmit FIFO, empty
_REQUEST = {'enabled': {1}, 'flow_on_tx': {0}, 'flow_watermark': {2}, 'rdy_active_high': {0}}   # receive FIFO, half full
for _link in ('pre', 'post', 'quad_opt'):
    SPI_RULES['bridge_slave_reply_' + _link] = dict(SPI_RULES['bridge_slave_' + _link], **_REPLY)
    SPI_RULES['bridge_slave_request_' + _link] = dict(SPI_RULES['bridge_slave_' + _link], **_REQUEST)
# S-13b.085: a quad data session (S-09.081): the master's receive in quad I/O at 50 MHz with fast mode, the
# slave's transmit as a quad reply
SPI_RULES['bridge_master_qdata'] = dict(SPI_RULES['bridge_master_quad_opt'])
SPI_RULES['bridge_slave_reply_qdata'] = dict(SPI_RULES['bridge_slave_reply_quad_opt'])


def check_spi(kind, ctl, clk, sclk0=SCLK0):
    return _B.check_spi_config(SPI_RULES, kind, ctl, clk, sclk0)


# --- SPI flash frames (S-13b.013-.016) ----------------------------------------------------------

FLASH_SIZE = 0x100000


def check_flash(frames):
    return _B.check_flash(frames, FLASH_SIZE)


def check_flash_end(frames):
    """S-13b.015: `frames` is a stage's flash trace up to where it ends: its hard reset, its power-off
    request or the hand-off. After the last SE or PP frame there is an RDSR frame whose last
    status byte has WIP = 0 (`busy_at_end`)."""
    busy = False
    for n, f in enumerate(frames):
        mosi, miso = bytes.fromhex(f['mosi']), bytes.fromhex(f.get('miso', ''))
        if mosi[:1] and mosi[0] in (SE, PP):
            busy = n
        elif mosi[:1] and mosi[0] == RDSR and len(miso) >= 2 and not miso[-1] & 1:
            busy = False
    return [] if busy is False else [('busy_at_end', 'frame #%d: no RDSR with WIP = 0 after it' % busy)]


# --- SPI registers at a stage's start, and the select writes (S-13b.010, .019, .020) -----------

SPI_CTL_START_CLEAR = 1 | 1 << 18 | 3 << 20 | 1 << 31       # EN, FMODE, MIOM, MMSE


def check_spi_start(trace, spi='SPI2'):
    """S-13b.019: a stage's first write of SPI2_CTL has EN, FMODE, MIOM and MMSE clear, and no frame comes
    before it (`first_ctl`). S-13b.010, .020: every SPI_SLVSEL write is made twice, back to back
    (`slvsel_twice`).

    The trace is the stage's register writes from its entry, in order, with a mark where each chip-select
    frame starts: {"op": "w", "reg": "SPI2_CTL", "val": int} | {"op": "frame"}. Writes to other controllers'
    registers are skipped."""
    out, ctl_seen, n = [], False, 0
    while n < len(trace):
        e = trace[n]
        if e['op'] == 'frame':
            if not ctl_seen:
                out.append(('first_ctl', '#%d a frame before the first %s_CTL write' % (n, spi)))
                ctl_seen = True
        elif e['op'] == 'w' and e['reg'] == spi + '_CTL':
            if not ctl_seen and e['val'] & SPI_CTL_START_CLEAR:
                out.append(('first_ctl', '#%d first %s_CTL write is 0x%08x' % (n, spi, e['val'])))
            ctl_seen = True
        elif e['op'] == 'w' and e['reg'] == spi + '_SLVSEL':
            nxt = trace[n + 1] if n + 1 < len(trace) else None
            if nxt and nxt['op'] == 'w' and nxt['reg'] == e['reg'] and nxt['val'] == e['val']:
                n += 1
            else:
                out.append(('slvsel_twice', '#%d %s_SLVSEL 0x%04x written once' % (n, spi, e['val'])))
        n += 1
    return out


# --- eMMC commands (S-13b.051-.055) ------------------------------------------------------------

RCA = 0x5A7E
OCR_ARG = 0x40000080
SWITCH_INDEXES = {32, 33, 34, 163, 179, 183, 185}


def check_switches(events):
    """S-13b.055: every CMD6 is write-byte to an allowed index and is followed by CMD13 until ready."""
    return _B.check_switches(events, SWITCH_INDEXES)


R1_SWITCH_ERROR = 1 << 7
R1_STATE_TRAN = 4


def check_busy_end(events):
    """S-13b.055: a CMD13 reply ends a busy period only with READY_FOR_DATA set and CURRENT_STATE = transfer.
    After a CMD6, a write (CMD24, CMD25) or an erase (CMD38) the next command that is not CMD13 follows such
    a reply or a `busy_end` event (DAT0 released), else `emmc_busy_end`; CMD12 after CMD25 belongs to the
    write. A CMD13 reply with SWITCH_ERROR set while a CMD6's busy end is awaited fails the switch: the next
    event is `fail`, else `emmc_switch_error`."""
    out, wait = [], None
    for n, e in enumerate(events):
        if e['ev'] in ('busy_end', 'fail'):
            wait = None
        elif e['ev'] == 'cmd':
            idx, resp = e['idx'], e.get('resp', 0)
            if idx == 13:
                if wait == 6 and resp & R1_SWITCH_ERROR:
                    if n + 1 >= len(events) or events[n + 1]['ev'] != 'fail':
                        out.append(('emmc_switch_error', '#%d SWITCH_ERROR and the switch did not fail' % n))
                    wait = None
                elif resp & READY and (resp >> 9) & 15 == R1_STATE_TRAN:
                    wait = None
            elif not (idx == 12 and wait == 25):
                if wait is not None:
                    out.append(('emmc_busy_end', '#%d CMD%d before the busy end of CMD%d' % (n, idx, wait)))
                wait = idx if idx in (6, 24, 25, 38) else None
    return out


def check_init(events, cmd1_bound_ms=1000, width=8):
    """S-13b.051/.052: the bring-up order, the identification clock, the OCR loop, the clock before the
    timing switch, the bus width on data transfers and at the end; S-13b.061 with width 4 (the OP-1F's
    `device.emmc_width`): Switch(183, 1), host width 1 then 4, never 8."""
    return _B.check_init(events, RCA, OCR_ARG, SWITCH_INDEXES, cmd1_bound_ms, width)


def check_timing(device_type, driver_strength, hs_timing, clock_hz):
    """S-13b.053: the eMMC clock after the switch against the HS_TIMING written. An eMMC that reports HS200
    (DEVICE_TYPE bit 4) counts as supporting high speed at 52 MHz as well, and HS200 timing (0x32) is
    accepted without tuning at no more than 50 MHz for an eMMC with DEVICE_TYPE bit 4 and DRIVER_STRENGTH
    bit 3."""
    if clock_hz > 50e6:
        return [('timing_vs_clock', 'clock above 50 MHz')]
    if hs_timing is None:
        ok = clock_hz <= 26e6
        return [] if ok else [('timing_vs_clock', 'no switch but clock %.0f' % clock_hz)]
    if hs_timing == 0x01:
        if not device_type & 0x13:
            return [('timing_vs_clock', 'high speed written for an eMMC without it')]
        limit = 52e6 if device_type & 0x12 else 26e6
        return [] if clock_hz <= limit else [('timing_vs_clock', 'clock %.0f above %.0f' % (clock_hz, limit))]
    if hs_timing == 0x32:
        if not device_type & 0x10:
            return [('timing_vs_clock', 'HS200 timing written for an eMMC without it')]
        return [] if driver_strength & 0x08 else [('timing_vs_clock', 'HS200 without driver type 3')]
    return [('timing_vs_clock', 'HS_TIMING 0x%02x' % hs_timing)]


# --- UART (S-13b.070/.071) ----------------------------------------------------------------------------

def check_uart(link, ctl, clk, sclk0=SCLK0):
    return _B.check_uart_config(link, ctl, clk, 1_000_000, flow=(1, 1) if link == 'uart1' else (0, 0),
                                fcpol=0 if link == 'uart1' else None, sclk0=sclk0)


UART_STAT_TEMT = 1 << 7


def check_uart_close(trace):
    """S-13b.072: a UART that is enabled is disabled (`UARTn_CTL` written with EN clear) only after a read of
    `UARTn_STAT` with TEMT set that no byte written to `UARTn_THR` follows, or after the bounded wait for it
    has run out (`uart_cut`).

    The trace: {"op": "w"|"r", "reg": "UART0_STAT"|"UART0_THR"|"UART0_CTL", "val": int} |
    {"op": "ev", "ev": "tx_wait_expired", "uart": "UART0"}; a byte the transmit DMA hands over counts as a
    THR write."""
    out, on, empty = [], {}, {}
    for n, e in enumerate(trace):
        if e['op'] == 'ev':
            if e['ev'] == 'tx_wait_expired':
                empty[e['uart']] = True
            continue
        u, _, reg = e['reg'].partition('_')
        if e['op'] == 'r' and reg == 'STAT':
            empty[u] = bool(e['val'] & UART_STAT_TEMT)
        elif e['op'] == 'w' and reg == 'THR':
            empty[u] = False
        elif e['op'] == 'w' and reg == 'CTL':
            if e['val'] & 1:
                if not on.get(u):
                    empty[u] = False        # just enabled: nothing is known of the transmitter yet
                on[u] = True
            else:
                if on.get(u) and not empty.get(u, False):
                    out.append(('uart_cut', '#%d %s disabled without its transmitter read empty' % (n, u)))
                on[u] = False
    return out


# --- Ports (S-13b.001-.008, .043) ----------------------------------------------------------------------

PINMUX = {  # S-13b.003
    'master': {'A': (0x30002000, 0x4040), 'B': (0x1400FFFF, 0xFFFF), 'C': (0x00855100, 0x0BD3)},
    'slave':  {'A': (0x00000200, 0x0000), 'B': (0x00000000, 0xFF00), 'C': (0x055AAA00, 0x3FF0)},
}
GPIO = {  # S-13b.006 at the hand-off: (outputs low, outputs high, outputs whose level the owner sets, inputs)
    'master': (['PA3'], ['PC11'], [], ['PA6', 'PA7', 'PC2', 'PC5']),
    'slave': (['PA3', 'PA6', 'PA9', 'PA10', 'PA11', 'PB6'], ['PC2'], ['PA4', 'PB2'],
              ['PA1', 'PA15', 'PB4', 'PB7', 'PB15', 'PC14']),
}
STRAP_BITS = 0x105
PA2_BIT = 0x004
STRAP_SETTLE_NS = 10_000     # S-13b.002: drivers released to sample
STRAP_INEN_NS = 1_000        # S-13b.008: inputs enabled to sample
PA4 = 0x10


FLASH_BUS_REGS = ('PORTB_MUX', 'PORTB_FER', 'PORTB_FER_SET', 'PORTB_FER_CLR')


def check_ports(trace, role):
    """S-13b.001, .003, .004 on the way; .003's end state at the first SPI/UART/MSI access (the mark, or the
    end of the trace without one); .006 at the end of the trace (the hand-off). Two steps of S-18 are not the
    strap sample or the first MUX write: S-18.002's fallback sample (a PORTA_DATA read with PA2's input
    enabled alone, DIR untouched), and S-18.001's flash-bus write (port B's MUX and FER before the event
    `first_stage_read`), which S-13b.004 exempts."""
    out, st = [], _blank()
    sampled, muxed, at_mark, fell_back, read = False, False, None, False, False
    pre_inputs = {'B': 0x400} if role == 'slave' else {'B': 0x400, 'C': 0x10}
    for n, e in enumerate(trace):
        if e['op'] == 'mark':
            if at_mark is None:
                at_mark = st
            continue
        if e['op'] == 'ev':
            read = read or e.get('ev') == 'first_stage_read'
            continue
        reg = e['reg']
        if e['op'] == 'r' and reg == 'PORTA_DATA' and not sampled and not fell_back and \
                (st['A']['INEN'] & STRAP_BITS) == PA2_BIT and not st['A']['DIR'] & STRAP_BITS:
            fell_back = True
            continue
        if e['op'] == 'r' and reg == 'PORTA_DATA' and not sampled:
            a = st['A']
            if (a['DIR'] & STRAP_BITS) or (a['INEN'] & STRAP_BITS) != STRAP_BITS:
                out.append(('strap_before_mux', '#%d PORTA_DATA read with the straps not inputs' % n))
            if muxed:
                out.append(('strap_before_mux', '#%d strap read after a MUX/FER write' % n))
            sampled = True
            continue
        if e['op'] != 'w' or not reg.startswith('PORT'):
            continue
        port, base = reg[4], reg[6:].partition('_')[0]
        if base in ('MUX', 'FER') and role == 'master' and not read and reg in FLASH_BUS_REGS:
            st = replay_one(st, e)
            continue
        if base in ('MUX', 'FER'):
            if not sampled:
                out.append(('strap_before_mux', '#%d %s before the strap sample' % (n, reg)))
            if sampled and not muxed and st['A']['INEN'] & STRAP_BITS:
                out.append(('strap_before_mux', '#%d strap input enables not cleared' % n))
            if not muxed:
                for p, bits in pre_inputs.items():
                    if st[p]['DIR'] & bits or (st[p]['INEN'] & bits) != bits:
                        out.append(('mux_before_fer', '#%d %s before P%s 0x%x is an input' % (n, reg, p, bits)))
            muxed = True
        before = st[port]['FER']
        new = replay_one(st, e)
        if base == 'FER' and new[port]['FER'] & ~before and new[port]['MUX'] != PINMUX[role][port][0]:
            out.append(('mux_before_fer', '#%d FER bits set on port %s with MUX 0x%08x' % (n, port, new[port]['MUX'])))
        st = new
    mk = at_mark if at_mark is not None else st
    for p in PORTS:
        mux, fer = PINMUX[role][p]
        if (mk[p]['MUX'], mk[p]['FER']) != (mux, fer):
            out.append(('pinmux_end_state', 'port %s MUX 0x%08x FER 0x%04x' % (p, mk[p]['MUX'], mk[p]['FER'])))
    low, high, owned, ins = GPIO[role]
    for name in low + high + owned:
        p, b = _pin(name)
        if not st[p]['DIR'] & b or st[p]['INEN'] & b:
            out.append(('gpio_directions', '%s not an output' % name))
    for name in ins:
        p, b = _pin(name)
        if st[p]['DIR'] & b or not st[p]['INEN'] & b:
            out.append(('gpio_directions', '%s not an input' % name))
    for name in high:
        p, b = _pin(name)
        if not st[p]['DATA'] & b:
            out.append(('gpio_directions', '%s not driven high' % name))
    for name in low:
        p, b = _pin(name)
        if st[p]['DATA'] & b:
            out.append(('gpio_directions', '%s not driven low' % name))
    return out


# --- The OP-1F's main CPU (S-13b.003, .004, .006 per device; S-18.001, S-18.020) ------------------------

PINMUX_1F = {'A': (0x30280000, 0x4620), 'B': (0x1400FFFF, 0xFFFF), 'C': (0x000AAA00, 0x17F3)}
GPIO_1F = (['PA0', 'PA3', 'PA4', 'PA6', 'PC13'], ['PA1', 'PA2', 'PA7', 'PC2', 'PC11', 'PC14'])  # kept, inputs
PA8 = ('A', 1 << 8)


def check_ports_1f(trace):
    """S-13b.003's six values of the OP-1F at the mark (the first SPI, UART or MSI access, or the trace's
    end): `pinmux_end_state`; FER bits set over another MUX value: `mux_before_fer`; S-13b.004 there: no DIR
    or INEN write of PB10 or PC4 before the first MUX write (`inputs_first_1f`); S-13b.006: PA0, PA3, PA4,
    PA6 and PC13 outputs whose DATA bit is never written (`level_written`), PA1, PA2, PA7, PC2, PC11 and
    PC14 inputs at the hand-off (`gpio_directions`), PA8 never an input (`pa8_input`)."""
    out, st, muxed, at_mark = [], _blank(), False, None
    kept = {_pin(n) for n in GPIO_1F[0]}
    for n, e in enumerate(trace):
        if e['op'] == 'mark':
            if at_mark is None:
                at_mark = st
            continue
        reg = e.get('reg', '')
        if e['op'] != 'w' or not reg.startswith('PORT'):
            continue
        port, base = reg[4], reg[6:].partition('_')[0]
        if base in ('MUX', 'FER'):
            muxed = True
        elif base in ('DIR', 'INEN') and not muxed and \
                ((port == 'B' and e['val'] & 0x400) or (port == 'C' and e['val'] & 0x10)):
            out.append(('inputs_first_1f', '#%d %s of PB10 or PC4 before the first MUX write' % (n, reg)))
        if base == 'DATA':
            for p, b in kept:
                if p == port and e['val'] & b:
                    out.append(('level_written', '#%d %s writes a kept output\'s level' % (n, reg)))
        before = st[port]['FER']
        new = replay_one(st, e)
        if base == 'FER' and new[port]['FER'] & ~before and new[port]['MUX'] != PINMUX_1F[port][0]:
            out.append(('mux_before_fer', '#%d FER bits set on port %s with MUX 0x%08x' % (n, port, new[port]['MUX'])))
        if new[PA8[0]]['INEN'] & PA8[1] and not new[PA8[0]]['DIR'] & PA8[1]:
            out.append(('pa8_input', '#%d PA8 made an input' % n))
        st = new
    mk = at_mark if at_mark is not None else st
    for p in PORTS:
        mux, fer = PINMUX_1F[p]
        if (mk[p]['MUX'], mk[p]['FER']) != (mux, fer):
            out.append(('pinmux_end_state', 'port %s MUX 0x%08x FER 0x%04x' % (p, mk[p]['MUX'], mk[p]['FER'])))
    for p, b in kept:
        if not st[p]['DIR'] & b or st[p]['INEN'] & b:
            out.append(('gpio_directions', 'P%s%d not an output' % (p, b.bit_length() - 1)))
    for name in GPIO_1F[1]:
        p, b = _pin(name)
        if st[p]['DIR'] & b or not st[p]['INEN'] & b:
            out.append(('gpio_directions', '%s not an input' % name))
    return out


FLASH_BUS = {'PORTB_MUX': 0x1400FFFF, 'PORTB_FER': 0x0000FFFF}   # the same on every device (S-18.001)


def check_device_before_pins(trace):
    """S-18.001 (rule `device_before_pins`) on a main CPU's trace with {"op": "ev", "ev": "first_stage_read"}
    after the last flash read of S-18.003: before it, no port register write but PORTB_MUX and PORTB_FER
    with their values and S-18.002's fallback sample (one PORTA_INEN_SET of PA2 alone), and no UART, SPI0
    or MSI register access; a trace without the event read no first stage."""
    out = []
    end = next((n for n, e in enumerate(trace) if e['op'] == 'ev' and e.get('ev') == 'first_stage_read'), None)
    if end is None:
        return [('device_before_pins', 'no first-stage read')]
    fallback = [n for n, e in enumerate(trace[:end])
                if e['op'] == 'w' and e.get('reg') == 'PORTA_INEN_SET' and e['val'] == PA2_BIT][:1]
    for n, e in enumerate(trace[:end]):
        reg = e.get('reg', '')
        if n in fallback:
            continue
        if e['op'] == 'w' and reg.startswith('PORT') and FLASH_BUS.get(reg) != e['val']:
            out.append(('device_before_pins', '#%d %s before the device is decided' % (n, reg)))
        if e['op'] in ('w', 'r') and (reg.startswith('UART') or reg.startswith('SPI0_') or
                                     reg.startswith('MSI')):
            out.append(('device_before_pins', '#%d %s before the device is decided' % (n, reg)))
    return out


def check_no_spi0(trace):
    """S-18.020, S-13b.083 without second-cpu (rule `no_spi0`): no SPI0 register access at all."""
    return [('no_spi0', '#%d %s' % (n, e['reg'])) for n, e in enumerate(trace)
            if e['op'] in ('w', 'r') and e.get('reg', '').startswith('SPI0_')]


def check_no_strap_sample(trace):
    """S-18.020 without straps (rule `no_strap_sample`): no PORTA_DATA read with PA0, PA2 and PA8 released
    as inputs, and PA8's input never enabled."""
    out, st = [], _blank()
    for n, e in enumerate(trace):
        reg = e.get('reg', '')
        if e['op'] == 'r' and reg == 'PORTA_DATA':
            a = st['A']
            if not a['DIR'] & STRAP_BITS and (a['INEN'] & STRAP_BITS) == STRAP_BITS:
                out.append(('no_strap_sample', '#%d PORTA_DATA read as a strap sample' % n))
            continue
        if e['op'] == 'w' and reg.startswith('PORT'):
            st = replay_one(st, e)
            if st['A']['INEN'] & PA8[1]:
                out.append(('no_strap_sample', '#%d PA8 input enabled' % n))
    return out


def check_strap_timing(trace, settle_ns=STRAP_SETTLE_NS, inen_ns=STRAP_INEN_NS):
    """S-13b.002, .008 on a port trace with t_ns: from the release of the last strap driver, and from the
    last strap input enable, to the PORTA_DATA sample."""
    st, released, enabled = _blank(), None, None
    for e in trace:
        if e['op'] == 'r' and e['reg'] == 'PORTA_DATA':
            out = []
            if released is None or e['t_ns'] - released < settle_ns:
                out.append(('strap_settle', 'sample %s ns after the drivers were released' %
                            (None if released is None else e['t_ns'] - released)))
            if enabled is None or e['t_ns'] - enabled < inen_ns:
                out.append(('strap_inen_floor', 'sample %s ns after the inputs were enabled' %
                            (None if enabled is None else e['t_ns'] - enabled)))
            return out
        if e['op'] != 'w':
            continue
        new = replay_one(st, e)
        if released is None and e['reg'].startswith('PORTA_DIR') and not new['A']['DIR'] & STRAP_BITS:
            released = e['t_ns']
        if (st['A']['INEN'] & STRAP_BITS) != STRAP_BITS and (new['A']['INEN'] & STRAP_BITS) == STRAP_BITS:
            enabled = e['t_ns']
        st = new
    return [('strap_settle', 'no strap sample')]


def check_pa4(trace):
    """S-13b.043 (slave): PA4 is handed to SPI2 (PORTA_FER bit 4 set) only while SPI2 is enabled and the
    last PB15 read was high; whenever it is a GPIO it is an output driven high. Once PA4 has been taken back
    after an SPI2 error or a given-up transfer (trace events {"op": "ev", "ev": "spi2_error" | "give_up"}),
    it is not handed to SPI2 again until PB15 has read high after the take-back.
    From the server stop after a release reply of 0 ({"ev": "server_stop"}) to the OS's first instruction
    ({"ev": "os_start"}, or the trace's end) PA4 is not handed to SPI2, and once driven low as a GPIO output it
    stays so; at the OS's first instruction it is a GPIO output driven low (rule pa4_stop, S-05.020)."""
    out, st, ctl, pb15, armed = [], _blank(), 0, None, False
    failed, await_high = False, False
    stopped, low_seen = False, False

    def low(s):
        a = s['A']
        return not a['FER'] & PA4 and a['DIR'] & PA4 and not a['INEN'] & PA4 and not a['DATA'] & PA4

    for n, e in enumerate(trace):
        if e['op'] == 'ev':
            failed = failed or e['ev'] in ('spi2_error', 'give_up')
            stopped = stopped or e['ev'] == 'server_stop'
            if e['ev'] == 'os_start':
                if stopped and not low(st):
                    out.append(('pa4_stop', '#%d PA4 not a GPIO output driven low at the OS start' % n))
                return out
            continue
        if e['op'] == 'r' and e['reg'] == 'PORTB_DATA':
            pb15 = bool(e['val'] & 0x8000)
            if pb15:
                await_high = False
            continue
        if e['op'] != 'w':
            continue
        if e['reg'] == 'SPI2_CTL':
            ctl = e['val']
            continue
        new = replay_one(st, e)
        if stopped:
            if new['A']['FER'] & PA4 and not st['A']['FER'] & PA4:
                out.append(('pa4_stop', '#%d PA4 handed to SPI2 after the server stop' % n))
            elif low_seen and not low(new):
                out.append(('pa4_stop', '#%d PA4 no longer driven low before the OS start' % n))
            low_seen = low_seen or low(new)
            st = new
            continue
        if new['A']['FER'] & PA4 and not st['A']['FER'] & PA4:
            if not ctl & 1:
                out.append(('pa4_handover', '#%d PA4 handed to SPI2 with SPI2 disabled' % n))
            if not pb15:
                out.append(('pa4_handover', '#%d PA4 handed to SPI2 without PB15 read high' % n))
            if await_high:
                out.append(('pa4_rehand', '#%d PA4 handed to SPI2 again after a failed transfer, before PB15 '
                                          'read high' % n))
        if st['A']['FER'] & PA4 and not new['A']['FER'] & PA4 and failed:
            failed, await_high = False, True
        st = new
        a = st['A']
        if a['DIR'] & PA4:
            armed = True
        if armed and not a['FER'] & PA4 and (not a['DIR'] & PA4 or a['INEN'] & PA4 or not a['DATA'] & PA4):
            out.append(('pa4_handover', '#%d PA4 as GPIO but not an output driven high' % n))
    if stopped and not low(st):
        out.append(('pa4_stop', 'PA4 not a GPIO output driven low at the end of the trace'))
    return out


TX_END_BOUND_US = 1000       # S-13b.045: from the last word's last edge to the take-back
REPLY_QUIET_US = 500         # S-13b.045: after an overrun, no word received for this long


def check_slave_tx(words, trace, io='single', end=None):
    """S-13b.045 (slave, reply session): when the slave ends its transmit and when it gives it up.

    `words` is the session's word count (S-09.012). `io` is 'single', or 'quad' for S-13b.041's quad I/O
    option: there the finish is `tf`, not the received-word count, and a status read that carries rx_words
    means the receive channel is on, which quad I/O does not allow (`tx_quad_rx_channel`). `end` is 'tf' for a
    single-I/O reply fed by DMA and ended on `TF` (S-13b.083): its finish is `tf` too. The trace is a
    neutral event list:
      {"ev": "hand"}                 PA4 handed to SPI2 (S-13b.043)
      {"ev": "take"}                 PA4 taken back (GPIO, high)
      {"ev": "stop"}                 the slave stops SPI2's transmit (SPI2 or its transmit channel disabled,
                                     or the transmit FIFO flushed)
      {"ev": "pb15_high"}            PB15 read high: the master ended the session (S-09.013)
      {"ev": "start", "word": k}     the master's first SCK edge of word k (0-based)
      {"ev": "end", "word": k}       the master's last SCK edge of word k
      {"ev": "status", ...}          SPI2 status the slave reads: flags among tfifo_empty, spif, tf,
                                     twc_zero, tur, tc, ror (a receive overrun); rx_words is the count of
                                     words received so far, quiet_us the time since the last of them
      {"ev": "dma_done"}             the transmit DMA's done status is set (recorded; the unmask order is
                                     `check_reply_setup`'s)
    An "end" and a "take" may carry t_us. The trace ends where the master's T_REPLY would run out.
    Rules: the transmit ends (take, stop) only after the last word's end, never at an empty transmit FIFO
    within the reply, nor at `tf`, `twc_zero` or `spif` before it, unless PB15 has read high
    (`tx_end_early`); it does end once the last word has ended (`tx_end_missing`); the take-back that ends a
    good reply follows a read of the finish: in single I/O the received-word count at the session's word
    count, not `tf` or anything else, or, once `ror` has been read, `twc_zero` with `quiet_us` at least
    REPLY_QUIET_US; and in quad I/O `tf` (`tx_end_no_count`), comes within 1 ms of the last word's last edge
    (`tx_end_late`), and goes with stopping the transmit (`tx_no_stop`); a transmit error (TUR or TC) ends
    the transmit without waiting for its finish (`tx_error_ignored`): after the error, while PA4 is still
    handed to SPI2, a later word's start or end, a read of the finish (the count reached, or `tf`), or a take
    once the last word has ended means the error was not acted on."""
    out, handed, failed, last_done, taken, err_n, cur = [], False, False, False, False, None, None
    counted, last_t, stopped, master_gone, ror = False, None, False, False, False
    for n, e in enumerate(trace):
        ev = e['ev']
        if ev == 'hand':
            handed, taken, counted, stopped, ror = True, False, False, False, False
        elif ev == 'pb15_high':
            master_gone = True
        elif ev in ('start', 'end'):
            if failed and handed and not taken and err_n is not None and (ev == 'start' or e['word'] != cur):
                out.append(('tx_error_ignored', '#%d word %d %s after the transmit error at #%d, PA4 still handed'
                            % (n, e['word'], ev, err_n)))
            if ev == 'start':
                cur = e['word']
            elif e['word'] == words - 1:
                last_done, last_t = True, e.get('t_us')
        elif ev == 'status' and handed and not taken:
            ror = ror or bool(e.get('ror'))
            # after an overrun the count falls short: the end from the transmit side
            tf_end = io == 'quad' or end == 'tf'
            quiet_end = not tf_end and ror and e.get('twc_zero') and e.get('quiet_us', 0) >= REPLY_QUIET_US
            finish = e.get('tf') or e.get('rx_words') == words or quiet_end
            if io == 'quad' and 'rx_words' in e:
                out.append(('tx_quad_rx_channel', '#%d received-word count read in quad I/O: the receive channel '
                            'is on' % n))
            if (e.get('tf') if tf_end else e.get('rx_words') == words or quiet_end):
                counted = True
            if (e.get('tur') or e.get('tc')) and not last_done and not failed:
                failed, err_n = True, n
            elif finish and failed and err_n is not None:
                out.append(('tx_error_ignored', '#%d transmit error at #%d not acted on before the finish'
                            % (n, err_n)))
        elif ev in ('take', 'stop'):
            stopped = stopped or ev == 'stop'
            if not failed and not last_done and not master_gone:
                out.append(('tx_end_early', '#%d %s before the last word (%d) has ended' % (n, ev, words - 1)))
            if ev == 'take':
                if not failed and last_done and not taken:
                    if not counted:
                        out.append(('tx_end_no_count', '#%d PA4 taken back with no read of %s'
                                    % (n, 'TF' if io == 'quad' or end == 'tf' else '%d received words' % words)))
                    if last_t is not None and e.get('t_us') is not None and e['t_us'] - last_t > TX_END_BOUND_US:
                        out.append(('tx_end_late', '#%d PA4 taken back %d us after the last word'
                                    % (n, e['t_us'] - last_t)))
                if failed and handed and not taken and last_done and err_n is not None:
                    out.append(('tx_error_ignored', '#%d PA4 taken back only after the last word, transmit '
                                'error at #%d' % (n, err_n)))
                taken, err_n = True, None
    if taken and last_done and not failed and not stopped:
        out.append(('tx_no_stop', 'PA4 taken back, transmit not stopped'))
    if handed and not taken:
        if failed:
            out.append(('tx_error_ignored', 'transmit error at #%d, PA4 never taken back' % err_n))
        elif last_done:
            out.append(('tx_end_missing', 'last word ended, PA4 never taken back'))
    return out


REPLY_TXCTL = 0x1011D
REPLY_ORDER = ('stat', 'feed', 'twc', 'txctl', 'rxctl', 'en')    # S-13b.044's write order, EN last


def check_reply_setup(link, s):
    """S-13b.044 (slave, reply session): SPI2's whole set-up at the hand-over of PA4.

    `link` is 'pre' or 'post', both single I/O, or 'quad' for S-13b.041's quad I/O option after the link.
    `s` has: ctl, txctl, rxctl (the three registers), twc, words (the session's word count), queued (words
    queued for the transmit FIFO past the session's last word), and optionally `order`, the set-up's writes
    as they were made:
      'stat'       SPI_STAT cleared              'twc'    SPI_TWC loaded
      'feed'       the transmit DMA started, or   'txctl'  SPI2_TXCTL written
                   the core feed made ready       'rxctl'  SPI2_RXCTL written
      'en'         SPI2_CTL written with EN       'dma'    as 'feed', and the feed is the transmit DMA
      'dma_done'   the transmit DMA's done status set
      'tf_unmask'  SPI_IMSK.TF unmasked
    Rules: `reply_ctl` SPI2_CTL, `reply_txctl` SPI2_TXCTL, `reply_rx_off` REN clear in single I/O, so no
    received-word count for S-13b.045's end, `reply_rx_quad` SPI2_RXCTL not 0 in quad I/O, where only one
    channel may be enabled, `reply_twc` SPI_TWC is not the session's word count, `reply_no_pad` not two words
    queued past the last. In quad I/O the order is checked: `reply_enable_last` EN is not the last of the
    set-up's writes, `reply_order` the other writes out of S-13b.044's order, `reply_tf_unmask` with the
    transmit DMA, TF unmasked before the DMA's done status. In single I/O the order is not checked.
    With `end` = 'tf' in `s`, a single-I/O reply fed by DMA and ended on `TF` (S-13b.083), REN must be
    clear (`reply_rx_on`) and the order is checked as in quad I/O."""
    out = []
    quad = link == 'quad'
    for rule, msg in check_spi('bridge_slave_reply_' + ('quad_opt' if quad else link), s['ctl'], 0):
        out.append(('reply_ctl', '%s: %s' % (rule, msg)))
    if s['txctl'] != REPLY_TXCTL:
        out.append(('reply_txctl', 'SPI2_TXCTL 0x%x, not 0x%x' % (s['txctl'], REPLY_TXCTL)))
    if quad and s['rxctl']:
        out.append(('reply_rx_quad', 'SPI2_RXCTL 0x%x in quad I/O: only the transmit channel may be on' % s['rxctl']))
    tf_end = not quad and s.get('end') == 'tf'
    if tf_end and s['rxctl'] & 1:
        out.append(('reply_rx_on', 'REN set in a reply ended on TF'))
    if not quad and not tf_end and not s['rxctl'] & 1:
        out.append(('reply_rx_off', 'REN clear in a reply: no received-word count'))
    if s['queued'] != 2:
        out.append(('reply_no_pad', '%d words queued past the last word, not 2' % s['queued']))
    if s['twc'] != s['words']:
        out.append(('reply_twc', 'SPI_TWC %d for a session of %d words' % (s['twc'], s['words'])))
    order = s.get('order')
    if (quad or tf_end) and order is not None:
        steps = ['feed' if x == 'dma' else x for x in order]
        setup = [x for x in steps if x in REPLY_ORDER]
        if 'en' not in setup or setup.index('en') != len(setup) - 1:
            out.append(('reply_enable_last', 'EN set before the set-up was written: %s' % ', '.join(order)))
        rest = [x for x in setup if x != 'en']
        if rest != [x for x in REPLY_ORDER if x in rest]:
            out.append(('reply_order', 'set-up written as %s' % ', '.join(order)))
        if 'dma' in order and 'tf_unmask' in order:
            if 'dma_done' not in order or order.index('tf_unmask') < order.index('dma_done'):
                out.append(('reply_tf_unmask', 'TF unmasked before the transmit DMA was done'))
    return out


def check_master_sessions(trace):
    """S-13b.040 (master): SPI0 is reset between sessions, and not within one.

    The trace is a neutral event list, in order:
      {"ev": "select", "on": true|false}   PC11 driven low (true) or high (false)
      {"ev": "word", "dir": "tx"|"rx"}     one word of the session clocked
      {"ev": "en", "on": true|false}       SPI_CTL written, with EN set (true) or clear (false); "miom": m when
                                           the write gives SPI_CTL.MIOM the value m (0 single I/O, 2 quad I/O),
                                           absent when it leaves MIOM as it was. MIOM is 0 at the trace's start
      {"ev": "setup", "reg": name}         a transfer set-up write: SPI_STAT, the DMA channel, SPI_TWC,
                                           SPI_RWC, SPI_TXCTL or SPI_RXCTL
    Rules: between a session's last word and the next session's first, EN is cleared and then set
    (`spi0_not_reset`); EN is not cleared between two words of one session (`spi0_reset_in_session`); no word
    is clocked with EN clear (`spi0_word_disabled`). The first session of a trace has no earlier session.
    A write that changes MIOM finds EN clear and leaves it clear (`miom_while_enabled`). No session, in single
    or in quad I/O, has a set-up write between the last setting of EN and its first word (`enable_last`,
    S-13b.083). Set-up writes after a session's first word, between two parts of it, are not covered."""
    out, en, sessions, cleared, set_after, in_session, words_here, cleared_here = [], True, 0, False, False, False, 0, False
    miom, setup_after_en = 0, False
    for n, e in enumerate(trace):
        ev = e['ev']
        if ev == 'setup':
            if en and not (in_session and words_here):
                setup_after_en = True
            continue
        if ev == 'en':
            if 'miom' in e and e['miom'] != miom:
                if en or e['on']:
                    out.append(('miom_while_enabled', '#%d MIOM %d -> %d with EN set' % (n, miom, e['miom'])))
                miom = e['miom']
            if e['on'] and not en:
                setup_after_en = False
            en = e['on']
            if not e['on']:
                cleared, set_after = True, False
                if in_session and words_here:
                    cleared_here = True
            elif cleared:
                set_after = True
        elif ev == 'select':
            in_session = e['on']
            if e['on']:
                words_here, cleared_here = 0, False
        elif ev == 'word':
            if not en:
                out.append(('spi0_word_disabled', '#%d word clocked with EN clear' % n))
            if cleared_here:
                out.append(('spi0_reset_in_session', '#%d word after EN was cleared within the session' % n))
                cleared_here = False
            if words_here == 0:
                if setup_after_en:
                    out.append(('enable_last', '#%d session %d starts with set-up written after EN'
                                % (n, sessions)))
                if sessions and not (cleared and set_after):
                    out.append(('spi0_not_reset', '#%d first word of session %d with no reset since the last '
                                'session' % (n, sessions)))
                sessions += 1
            cleared, set_after = False, False
            words_here += 1
    return out


# --- DMA (S-13b.080, S-13b.083) --------------------------------------------------------------------------

DMA_CHANNELS = {4: 'SPI0 TX', 5: 'SPI0 RX', 8: 'SPI2 TX', 9: 'SPI2 RX', 10: 'UART0 TX', 11: 'UART0 RX',
                12: 'UART1 TX', 13: 'UART1 RX'}


# S-13b.083: the bridge's sessions by DMA. Channel per CPU and direction, the channel's configuration
# (enabled, 32-bit peripheral words, descriptor-array flow with four words fetched, SYNC clear; bit 1 set for
# a receive) and the SPI channel's control value.
BRIDGE_DMA = {'master_send': ('SPI0', 4), 'master_receive': ('SPI0', 5),
              'slave_receive': ('SPI2', 9), 'slave_reply': ('SPI2', 8)}
DMA_CFG_SEND, DMA_CFG_RECEIVE = 0x00035021, 0x00035023
DMA_CFG_MSIZE = 0x00000700               # the memory word size: chosen per buffer
MASTER_TXCTL, MASTER_RXCTL, SLAVE_RXCTL = 0x0001001D, 0x0001001D, 0x00010015
SPI_TF, SPI_RF, PA4_BIT = 0x800, 0x400, 0x10
REQUEST_HEADER = 12                      # S-09.020


def check_bridge_dma(kind, trace, words=None, parts=None):
    """S-13b.083: one bridge session moved by DMA, from its register trace.

    `kind` is 'master_send' (a request session, DMA4), 'master_receive' (a reply session, DMA5),
    'slave_receive' (a request session, DMA9) or 'slave_reply' (the feed of a reply session, DMA8; the rest
    of a reply's set-up is `check_reply_setup`'s and its end `check_slave_tx`'s). `words` is the session's word
    count (the sends); `parts` the byte counts of the parts a receive is taken in, in order. The trace, in
    order:
      {"op": "w"|"r", "reg": name, "val": int}       a register write, or a read and its value (SPI0_*, SPI2_*,
                                                     DMAn_*, PORTA_FER_SET, PORTA_FER_CLR)
      {"op": "ev", "ev": "desc", "ch": n, "bytes": [..]}   the descriptor array given to channel n: its
                                                     buffers' byte counts
      {"op": "ev", "ev": "select", "on": bool}       the master's PC11 driven low (true) or high
    A read of DMAn_STAT with bit 0 set is the DMA's done status seen; a read of SPI0_ILAT with TF set is the
    transmit-finished interrupt taken. Writes this session does not name (SPI0_CLK, a control write that
    leaves EN as it was) are passed over.
    Rules, all kinds: `dma_core` a word written to the transmit FIFO or read from the receive FIFO by the
    core (a reply's received words, which the slave counts, are not the session's bytes), or the channel never
    started; `dma_cfg` the channel's configuration; `dma_desc` the descriptors do not cover the part;
    `dma_order` the set-up's writes out of order or one missing; `dma_enable_last` a set-up write after EN.
    master_send: `send_twc`, `send_txctl`; `tf_unmask` TF unmasked before DMA4's done status was seen;
    `send_end` the transmit stopped, EN cleared or PC11 raised before TF was taken, or not TEN, then EN, then
    PC11. master_receive: `recv_rwc`, `recv_rxctl`; `recv_part` between two parts REN not cleared at the done
    status, or SPI0_CTL written to another value; `recv_end` not REN, then EN, then PC11 after the last part.
    slave_receive: `recv_rxctl` (a word count is `recv_rxctl` too); `recv_header` the first part is not the
    12-byte header; `recv_part` between two parts anything but DMA9's status, descriptors and configuration
    written; `recv_end` not PA4 taken back, then EN cleared, then REN cleared after the last part."""
    spi, ch = BRIDGE_DMA[kind]
    R = lambda name: '%s_%s' % (spi, name)                                         # noqa: E731
    D = lambda name: 'DMA%d_%s' % (ch, name)                                       # noqa: E731
    out = []

    def bad(rule, msg, n):
        out.append((rule, '#%d %s' % (n, msg)))

    send = kind in ('master_send', 'slave_reply')
    cfg_want = DMA_CFG_SEND if send else DMA_CFG_RECEIVE
    core = (R('TFIFO'), 'w') if send else (R('RFIFO'), 'r')
    started = False
    for n, e in enumerate(trace):
        if (e.get('reg'), e['op']) == core:
            bad('dma_core', '%s by the core' % core[0], n)
        if e['op'] == 'w' and e['reg'] == D('CFG') and e['val'] & 1:
            started = True
            if e['val'] & ~DMA_CFG_MSIZE != cfg_want:
                bad('dma_cfg', '%s 0x%x, not 0x%x' % (D('CFG'), e['val'], cfg_want), n)
    if not started:
        out.append(('dma_core', 'DMA%d never started' % ch))
    desc = [e['bytes'] for e in trace if e['op'] == 'ev' and e['ev'] == 'desc' and e['ch'] == ch]

    if kind == 'slave_reply':
        if [sum(d) for d in desc] != [4 * words + 8]:
            out.append(('dma_desc', 'descriptors of %s bytes for a reply of %d words and the two queued'
                        % (desc, words)))
        return out

    if kind == 'master_send':
        if [sum(d) for d in desc] != [4 * words]:
            out.append(('dma_desc', 'descriptors of %s bytes for a session of %d words' % (desc, words)))
        keys = {(R('STAT'), 'stat'), (D('STAT'), 'ack'), (D('DSCPTR_CUR'), 'dscptr'), (D('CFG'), 'cfg'),
                (R('TWC'), 'twc'), (R('TXCTL'), 'txctl')}
        names = dict(keys)
        setup, en, done, unmasked, taken, stage = [], False, False, False, False, 0
        for n, e in enumerate(trace):
            if e['op'] == 'ev':
                if e['ev'] == 'select' and not e['on']:
                    if stage != 2:
                        bad('send_end', 'PC11 raised before the transmit was stopped and EN cleared', n)
                    stage = 3
                continue
            reg, val = e['reg'], e['val']
            if e['op'] == 'r':
                if reg == D('STAT') and val & 1 and en:
                    done = True
                elif reg == R('ILAT') and val & SPI_TF and unmasked:
                    taken = True
                continue
            if reg == R('CTL'):
                if val & 1 and not en:
                    en = True
                    if setup != ['stat', 'ack', 'dscptr', 'cfg', 'twc', 'txctl']:
                        bad('dma_order', 'set-up written as %s before EN' % ', '.join(setup), n)
                elif not val & 1 and en:
                    en = False
                    if not taken or stage != 1:
                        bad('send_end', 'EN cleared before TF was taken and the transmit stopped', n)
                    stage = 2
                continue
            if reg == R('IMSK_SET') and val & SPI_TF:
                unmasked = True
                if not done:
                    bad('tf_unmask', 'TF unmasked before DMA%d\'s done status was seen' % ch, n)
                continue
            if reg == R('TXCTL') and (en or stage) and not val & 1:
                if not taken:
                    bad('send_end', 'the transmit stopped before TF was taken', n)
                stage = max(stage, 1)
                continue
            if reg in names and not (reg in (D('STAT'), R('STAT')) and en):       # with EN set: acknowledgements
                if en:
                    bad('dma_enable_last', '%s written with EN set' % reg, n)
                    continue
                if reg == R('STAT') and val != 0xFFFFFFFF:
                    continue
                setup.append(names[reg])
                if reg == R('TWC') and val != words:
                    bad('send_twc', 'SPI_TWC %d for a session of %d words' % (val, words), n)
                if reg == R('TXCTL') and val != MASTER_TXCTL:
                    bad('send_txctl', '%s 0x%x, not 0x%x' % (reg, val, MASTER_TXCTL), n)
        if stage != 3:
            out.append(('send_end', 'the session did not end: TF taken, TEN and EN cleared, PC11 raised'))
        return out

    if [sum(d) for d in desc] != list(parts):
        out.append(('dma_desc', 'descriptors of %s bytes for parts of %s' % (desc, list(parts))))

    if kind == 'master_receive':
        names = {D('DSCPTR_CUR'): 'dscptr', D('CFG'): 'cfg', R('RWC'): 'rwc', R('RXCTL'): 'rxctl'}
        p, setup, en, ctl, running, done, stage = 0, [], False, None, False, False, 0
        for n, e in enumerate(trace):
            if e['op'] == 'ev':
                if e['ev'] == 'select' and not e['on']:
                    if stage != 2:
                        bad('recv_end', 'PC11 raised before REN and EN were cleared after the last part', n)
                    stage = 3
                continue
            reg, val = e['reg'], e['val']
            if e['op'] == 'r':
                if reg == D('STAT') and val & 1 and running:
                    done = True
                continue
            if reg == R('CTL'):
                if val & 1 and not en:
                    en, ctl = True, val
                    if p == 0 and not running:
                        running = True
                        if setup != ['dscptr', 'cfg', 'rwc', 'rxctl']:
                            bad('dma_order', 'set-up written as %s before EN' % ', '.join(setup), n)
                        setup = []
                    else:
                        bad('recv_part', 'EN set again within the session', n)
                elif not val & 1 and en:
                    en = False
                    if p < len(parts):
                        bad('recv_part', 'EN cleared before the last part', n)
                    elif stage != 1:
                        bad('recv_end', 'EN cleared before REN', n)
                    stage = 2
                elif en and val != ctl:
                    bad('recv_part', 'SPI0_CTL written 0x%x within the session' % val, n)
                continue
            if reg == R('RXCTL') and not val & 1:
                if running and not done:
                    bad('recv_part', 'REN cleared before DMA%d\'s done status' % ch, n)
                if running:
                    running, done, p = False, False, p + 1
                    if p == len(parts):
                        stage = 1
                continue
            if reg not in names:
                continue
            if running and done:                             # the next part's set-up, REN still set
                bad('recv_part', '%s written with REN not cleared after the part' % reg, n)
                running, done, p = False, False, p + 1
            elif running:
                bad('dma_enable_last' if p == 0 else 'recv_part', '%s written while the part runs' % reg, n)
                continue
            if p >= len(parts):
                continue
            setup.append(names[reg])
            if reg == R('RWC') and val != parts[p] // 4:
                bad('recv_rwc', 'SPI_RWC %d for a part of %d words' % (val, parts[p] // 4), n)
            if reg == R('RXCTL'):
                if val != MASTER_RXCTL:
                    bad('recv_rxctl', '%s 0x%x, not 0x%x' % (reg, val, MASTER_RXCTL), n)
                if p:                                        # a later part runs from here: EN is set already
                    running = True
                    if setup != ['dscptr', 'cfg', 'rwc', 'rxctl']:
                        bad('dma_order', 'part %d set up as %s' % (p, ', '.join(setup)), n)
                    setup = []
        if stage != 3:
            out.append(('recv_end', 'the session did not end: REN and EN cleared, PC11 raised'))
        return out

    # slave_receive
    if desc and sum(desc[0]) != REQUEST_HEADER:
        out.append(('recv_header', 'the first part is %d bytes, not the %d-byte header' % (sum(desc[0]),
                                                                                      REQUEST_HEADER)))
    p, setup, en, handed, running, stage = 0, [], False, False, False, 0
    for n, e in enumerate(trace):
        if e['op'] != 'w':
            if e['op'] == 'r' and e['reg'] == D('STAT') and e['val'] & 1 and running:
                running, p = False, p + 1
                setup = []
            continue
        reg, val = e['reg'], e['val']
        last = p == len(parts)
        if handed and not running and not last:               # between two parts
            if reg in (D('STAT'), D('DSCPTR_CUR')):
                setup.append('dscptr' if reg == D('DSCPTR_CUR') else 'ack')
            elif reg == D('CFG'):
                running = True
                if setup[-1:] != ['dscptr']:
                    bad('dma_order', 'DMA%d enabled before its descriptors' % ch, n)
            else:
                bad('recv_part', '%s written between two parts' % reg, n)
            continue
        if handed and last:                                   # the end: PA4, EN, REN
            if reg == 'PORTA_FER_CLR' and val & PA4_BIT:
                stage = 1 if stage == 0 else stage
            elif reg == R('CTL') and not val & 1:
                if stage != 1:
                    bad('recv_end', 'EN cleared before PA4 was taken back', n)
                stage = 2
            elif reg == R('RXCTL') and not val & 1:
                if stage != 2:
                    bad('recv_end', 'REN cleared before EN', n)
                stage = 3
            continue
        if handed:                                            # a part runs
            if reg in (R('CTL'), R('RXCTL'), 'PORTA_FER_CLR', D('CFG'), D('DSCPTR_CUR')):
                bad('recv_part', '%s written while a part runs' % reg, n)
            continue
        if reg == R('CTL'):
            if val & 1 and not en:
                en = True
                if setup:                                     # set within the set-up; else it was set already
                    setup.append('en')
            elif not val & 1:
                en = False
        elif reg == D('CFG'):
            setup.append('cfg' if val & 1 else 'off')
        elif reg == R('IMSK_CLR') and val & SPI_RF:
            setup.append('mask')
        elif reg == D('DSCPTR_CUR'):
            setup.append('dscptr')
        elif reg == R('RWC'):
            bad('recv_rxctl', 'a word count loaded for a request', n)
        elif reg == R('RXCTL'):
            setup.append('rxctl')
            if val != SLAVE_RXCTL:
                bad('recv_rxctl', '%s 0x%x, not 0x%x' % (reg, val, SLAVE_RXCTL), n)
        elif reg == 'PORTA_FER_SET' and val & PA4_BIT:
            handed, running = True, True
            if not en:
                bad('dma_order', 'PA4 handed to SPI2 with EN clear', n)
            if [x for x in setup if x != 'en'] != ['off', 'mask', 'dscptr', 'cfg', 'rxctl'] or \
                    ('en' in setup and setup[-1] != 'en'):
                bad('dma_order', 'set-up written as %s before the hand-over' % ', '.join(setup), n)
    if stage != 3:
        out.append(('recv_end', 'the session did not end: PA4 taken back, EN and REN cleared'))
    return out


# --- S-13b.023: the slave boot stream's parts by DMA4 under flow control (S-09.009's option) -----

BOOT_DMA_CFG = 0x00035001       # DMA_CFG_SEND with 8-bit peripheral words
BOOT_TXCTL = 0x0001001C         # TTI, TWCEN, TDR 1, TUWM 1; TEN after it
BOOT_CTL_FC = 0x00002013        # S-13b.020's control value with FCEN (ready active low), and EN


def check_boot_dma(trace, n):
    """S-13b.023: one part of n bytes of the slave boot stream moved by DMA4 under SPI0's flow control, from
    its register trace in check_bridge_dma's format ("select" being SPI0_SEL3 low, true, or high): the
    control value's write, the select, then the part. Rules:
    `boot_ctl` the last SPI0_CTL write before the select is not 0x2013 (S-13b.020's set-up with flow control,
    ready active low, and EN), or SPI0_CTL is written after it: EN stays set, as SEL3 is SPI0's;
    `boot_core` a byte written to SPI0_TFIFO by the core during the part, or DMA4 never started;
    `boot_cfg` DMA4_CFG started with another value than 0x35001 (its memory word size is free);
    `boot_desc` the descriptors given to DMA4 do not cover n bytes;
    `boot_order` the set-up's writes not, in this order: SPI0_TXCTL 0, SPI0_RXCTL 0, SPI0_STAT cleared,
    DMA4_STAT acknowledged, DMA4_DSCPTR_CUR, DMA4_CFG, SPI0_TWC, SPI0_TXCTL 0x1001C, SPI0_TXCTL 0x1001D;
    `boot_twc` SPI0_TWC not n;
    `boot_end` not, after the set-up: DMA4's done status read, then SPI0_STAT read with TF, then
    SPI0_TXCTL 0 and DMA4 stopped, with the select still low."""
    out = []

    def bad(rule, msg, k):
        out.append((rule, '#%d %s' % (k, msg)))

    sel = [k for k, e in enumerate(trace) if e['op'] == 'ev' and e['ev'] == 'select' and e['on']]
    begin = sel[0] + 1 if sel else 0
    ctl = [(k, e['val']) for k, e in enumerate(trace) if e['op'] == 'w' and e['reg'] == 'SPI0_CTL']
    before = [v for k, v in ctl if k < begin]
    if not before or before[-1] != BOOT_CTL_FC:
        bad('boot_ctl', 'SPI0_CTL before the part %s, not 0x%x' % (
            '0x%x' % before[-1] if before else 'unwritten', BOOT_CTL_FC), 0)
    for k, v in ctl:
        if k >= begin:
            bad('boot_ctl', 'SPI0_CTL 0x%x written during the part' % v, k)
            break
    part = trace[begin:]
    want = [('SPI0_TXCTL', 0), ('SPI0_RXCTL', 0), ('SPI0_STAT', None), ('DMA4_STAT', None),
            ('DMA4_DSCPTR_CUR', None), ('DMA4_CFG', None), ('SPI0_TWC', None), ('SPI0_TXCTL', BOOT_TXCTL),
            ('SPI0_TXCTL', BOOT_TXCTL | 1)]
    names = {x[0] for x in want}
    writes = [(begin + k, e) for k, e in enumerate(part) if e['op'] == 'w' and e['reg'] in names]
    started = False
    for k, e in writes:
        if e['reg'] == 'DMA4_CFG' and e['val'] & 1:
            started = True
            if e['val'] & ~0x700 != BOOT_DMA_CFG:
                bad('boot_cfg', 'DMA4_CFG 0x%x, not 0x%x' % (e['val'], BOOT_DMA_CFG), k)
        if e['reg'] == 'SPI0_TWC' and e['val'] != n:
            bad('boot_twc', 'SPI0_TWC %d, not %d' % (e['val'], n), k)
    if not started:
        out.append(('boot_core', 'DMA4 never started'))
    for k, e in enumerate(part):
        if e['op'] == 'w' and e['reg'] == 'SPI0_TFIFO':
            bad('boot_core', 'a byte by the core', begin + k)
    desc = [e['bytes'] for e in part if e['op'] == 'ev' and e['ev'] == 'desc' and e['ch'] == 4]
    if [sum(d) for d in desc] != [n]:
        out.append(('boot_desc', 'descriptors of %s bytes for a part of %d' % (desc, n)))
    setup = writes[:len(want)]
    for (k, e), (reg, val) in zip(setup, want):
        if e['reg'] != reg or (val is not None and e['val'] != val):
            bad('boot_order', '%s 0x%x where %s%s is due' % (e['reg'], e['val'], reg,
                                                             '' if val is None else ' 0x%x' % val), k)
            break
    else:
        if len(setup) < len(want):
            out.append(('boot_order', 'the set-up stops after %d of its %d writes' % (len(setup), len(want))))
    ten = [k for k, e in writes if e['reg'] == 'SPI0_TXCTL' and e['val'] & 1]
    after = ten[0] + 1 if ten else len(trace)
    step = 0                    # 0: DMA4's done, 1: TF, 2: TXCTL 0, 3: DMA4 stopped
    for k in range(after, len(trace)):
        e = trace[k]
        if step == 0 and e['op'] == 'r' and e['reg'] == 'DMA4_STAT' and e['val'] & 1:
            step = 1
        elif step == 1 and e['op'] == 'r' and e['reg'] == 'SPI0_STAT' and e['val'] & SPI_TF:
            step = 2
        elif e['op'] == 'w' and e['reg'] == 'SPI0_TXCTL' and not e['val'] & 1:
            if step != 2:
                bad('boot_end', 'TEN cleared before the done status and TF', k)
                return out
            step = 3
        elif e['op'] == 'w' and e['reg'] == 'DMA4_CFG' and not e['val'] & 1:
            if step != 3:
                bad('boot_end', 'DMA4 stopped before TEN was cleared after TF', k)
                return out
            step = 4
        elif e['op'] == 'ev' and e['ev'] == 'select' and not e['on'] and step < 4:
            bad('boot_end', 'the select raised before the part ended', k)
            return out
    if step < 4:
        out.append(('boot_end', 'the part never ended'))
    return out

