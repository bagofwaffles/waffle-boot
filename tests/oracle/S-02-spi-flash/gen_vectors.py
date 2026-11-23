#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""S-02 vector generator (standard library only): writes the SPI-flash data files under vectors/.

Run:  python3 -I gen_vectors.py [--check]   (--check: regenerate in memory and compare with the files)
"""
import csv
import io
import json
import os
import random
import re
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
VEC = os.path.join(HERE, "vectors")
sys.path.insert(0, HERE)
import slot_kind  # noqa: E402  S-02.106

FLASH_SIZE = 0x100000
SECTOR = 0x1000
PAGE = 0x100
SELECTOR = 0xD000
RECORD = (0xE000, 0xF000)
SLOT = (0x10000, 0x88000)
SLOT_MAX = 0x78000
LOG_SECTOR = SLOT_MAX - SECTOR   # S-02.045: offset of a slot's last sector
LOGO_SECTOR = SLOT_MAX - 3 * SECTOR   # the boot logo's header sector (src/front/fr_handoff.h)
TAG = 0x0000B004
META = 0xC000
META_LEN = 0x34
META_MAGIC = 0xDEADBABE
PROTECTED_END = 0xD000
MIRROR = 0xB000          # S-02.063
MIRROR_LEN = 0x68

STATES = ["good", "badcrc", "badtag", "erased", "badlen", "hugelen", "empty"]
TABLE_INDICES = [0x00, 0x01, 0x02, 0x7F, 0x80, 0xFE, 0xFF]
TABLE_VERIFIED = [0x00, 0x01, 0x02, 0xFE, 0xFF]
RESETS = 3


def hx(sel):
    return "%02x %02x" % sel


def boot_rule(index):
    """S-02.011."""
    return 0 if index == 0 else 1


# ---------------------------------------------------------------- the two first-stage profiles

def factory_check(state):
    """S-02.021, S-02.026: (result, what was read)."""
    if state in ("badtag", "erased"):
        return "fail", "record"
    if state == "hugelen":
        return "unspecified", "record+slot"
    if state in ("good", "empty"):
        return "pass", "record+slot"
    return "fail", "record+slot"          # badcrc, badlen (CRC differs by construction)


def conforming_check(state):
    """S-02.016, S-02.031: 'pass' for a good record, else 'fail'."""
    if state == "good":
        return "pass"
    return "fail"


def factory_reset(sel, slots):
    """One reset of the first stage. Returns dict(checks, result, boot, sel_after, writes)."""
    index, verified = sel
    c = boot_rule(index)
    if verified == 1:
        return dict(checks="none", result="-", boot=c, sel_after=[sel], writes=0)
    result, _ = factory_check(slots[c])
    if result == "unspecified":
        return dict(checks="slot%d" % c, result=result, boot="unspecified", sel_after=["unspecified"],
                    writes="unspecified")
    new = (index, 1) if result == "pass" else ((1 if index == 0 else 0), 1)
    return dict(checks="slot%d" % c, result=result, boot=c, sel_after=[new], writes=1)


def conforming_reset(sel, slots):
    """One reset of a conforming first stage (S-02.030-.035). Returns dict(checks, result, boot, sel_after,
    writes)."""
    index, verified = sel
    c = boot_rule(index)
    if conforming_check(slots[c]) == "pass":
        if verified == 1:
            return dict(checks="slot%d" % c, result="pass", boot=c, sel_after=[sel], writes=0)
        return dict(checks="slot%d" % c, result="pass", boot=c, sel_after=[(index, 1)], writes=1)
    o = 1 - c
    if conforming_check(slots[o]) == "pass":
        return dict(checks="slot%d+slot%d" % (c, o), result="fail", boot=o, sel_after=[(o, 1)], writes=1)
    if verified == 1:
        return dict(checks="slot%d+slot%d" % (c, o), result="fail", boot=c, sel_after=[sel], writes=0)
    return dict(checks="slot%d+slot%d" % (c, o), result="fail", boot=c, sel_after=[(index, 1)], writes=1)


PROFILES = {"factory": factory_reset, "conforming": conforming_reset}


def run_resets(profile, sel, slots, n=RESETS):
    """Boots on n consecutive resets with the slot contents fixed. Each entry: slot number, or a marker."""
    f = PROFILES[profile]
    boots = []
    for _ in range(n):
        r = f(sel, slots)
        boots.append(r["boot"])
        nxt = r["sel_after"][0]
        if not isinstance(nxt, tuple):
            boots.extend([nxt] * (n - len(boots)))
            break
        sel = nxt
    return boots


def boots_good(boots, slots):
    return [isinstance(b, int) and slots[b] == "good" for b in boots]


# ---------------------------------------------------------------- the table

def fmt_after(lst):
    return "|".join(hx(s) if isinstance(s, tuple) else s for s in lst)


def first_stage_rows():
    rows = []
    for index in TABLE_INDICES:
        for verified in TABLE_VERIFIED:
            for s0 in STATES:
                for s1 in STATES:
                    sel = (index, verified)
                    slots = (s0, s1)
                    row = {"selector": hx(sel), "slot0": s0, "slot1": s1}
                    for prof, f in PROFILES.items():
                        r = f(sel, slots)
                        row[prof + "_checks"] = r["checks"]
                        row[prof + "_result"] = r["result"]
                        row[prof + "_boot"] = r["boot"]
                        row[prof + "_sel_after"] = fmt_after(r["sel_after"])
                        row[prof + "_writes"] = r["writes"]
                        nxt = r["sel_after"][0]
                        if isinstance(nxt, tuple):
                            r2 = f(nxt, slots)
                            row[prof + "_reset2_boot"] = r2["boot"]
                            row[prof + "_reset2_sel_after"] = fmt_after(r2["sel_after"])
                        else:
                            row[prof + "_reset2_boot"] = nxt
                            row[prof + "_reset2_sel_after"] = nxt
                    rows.append(row)
    return rows


TABLE_FIELDS = ["selector", "slot0", "slot1"] + [
    p + "_" + k for p in ("factory", "conforming")
    for k in ("checks", "result", "boot", "sel_after", "writes", "reset2_boot", "reset2_sel_after")]


# ---------------------------------------------------------------- read faults (S-02.036)

FAULT_SETS = ["selector", "slot0", "slot1", "selector+slot0", "selector+slot1", "slot0+slot1",
              "selector+slot0+slot1"]
FAULT_STATES = ["good", "badcrc"]


def conforming_reset_faults(sel, slots, faults):
    """One reset of a conforming first stage with the reads named in `faults` failing (S-02.036)."""
    names = faults.split("+")
    seen = False
    judged = sel
    if "selector" in names:
        judged, seen = (0xFF, 0xFF), True
    index, verified = judged
    c = boot_rule(index)
    o = 1 - c

    def check(s):
        nonlocal seen
        if "slot%d" % s in names:
            seen = True
            return "fail"
        return conforming_check(slots[s])

    if check(c) == "pass":
        boot, checks = c, "slot%d" % c
    elif check(o) == "pass":
        boot, checks = o, "slot%d+slot%d" % (c, o)
    else:
        boot, checks = c, "slot%d+slot%d" % (c, o)
    if not seen:
        r = conforming_reset(sel, slots)
        return dict(fault_seen=0, checks=r["checks"], boot=r["boot"], sel_after=r["sel_after"][0],
                    writes=r["writes"])
    return dict(fault_seen=1, checks=checks, boot=boot, sel_after=sel, writes=0)


def read_fault_rows():
    rows = []
    for index in TABLE_INDICES:
        for verified in TABLE_VERIFIED:
            for s0 in FAULT_STATES:
                for s1 in FAULT_STATES:
                    for faults in FAULT_SETS:
                        r = conforming_reset_faults((index, verified), (s0, s1), faults)
                        rows.append({"selector": hx((index, verified)), "slot0": s0, "slot1": s1,
                                     "faults": faults, "fault_seen": r["fault_seen"], "checks": r["checks"],
                                     "boot": r["boot"], "sel_after": hx(r["sel_after"]),
                                     "writes": r["writes"]})
    return rows


FAULT_FIELDS = ["selector", "slot0", "slot1", "faults", "fault_seen", "checks", "boot", "sel_after", "writes"]


# ---------------------------------------------------------------- writers and power cuts

IMAGE_LEN = 0x1345   # two sectors, 20 page programs: enough to show the extent and the order


def writer_target(rule, index):
    if rule == "conforming":           # S-02.043
        return 1 if index == 0 else 0
    return 0 if index == 1 else 1      # stock_rule: the negative control


def upgrade_ops(rule, sel, n=IMAGE_LEN, package=False, recovery=False):
    """The writes of a slot upgrade; package: the package's bootloader step, which reads the selector back;
    recovery: recovery's image write, which erases the settings sector too."""
    t = writer_target(rule, sel[0])
    base = SLOT[t]
    ops = []
    nsec = (n + SECTOR - 1) // SECTOR
    for k in range(nsec):
        sec = base + k * SECTOR
        ops.append(dict(cls="slot_erase", op="erase", offset=sec, length=SECTOR))
        end = min(base + n, sec + SECTOR)
        a = sec
        while a < end:
            ln = min(PAGE - (a % PAGE), end - a)
            ops.append(dict(cls="slot_program", op="program", offset=a, length=ln))
            a += ln
    if rule == "conforming" and n <= LOG_SECTOR:                 # S-02.045
        ops.append(dict(cls="slot_erase", op="erase", offset=base + LOG_SECTOR, length=SECTOR))
    assert not recovery or n <= SLOT_MAX - 2 * SECTOR            # S-02.084
    if rule == "conforming" and (package or recovery) and n <= LOGO_SECTOR:    # the boot logo's header
        ops.append(dict(cls="slot_erase", op="erase", offset=base + LOGO_SECTOR, length=SECTOR))
    if rule == "conforming" and (package or recovery) and n <= SLOT_MAX - 2 * SECTOR:   # S-02.103
        ops.append(dict(cls="slot_erase", op="erase", offset=base + SLOT_MAX - 2 * SECTOR, length=SECTOR))
    ops.append(dict(cls="record_erase", op="erase", offset=RECORD[t], length=SECTOR))
    ops.append(dict(cls="record_program", op="program", offset=RECORD[t], length=12))
    if rule == "conforming":                                     # S-02.047
        ops.append(dict(cls="readback", op="read", offset=RECORD[t], length=12))
        ops.append(dict(cls="readback", op="read", offset=base, length=n))
    ops.append(dict(cls="selector_erase", op="erase", offset=SELECTOR, length=SECTOR))
    ops.append(dict(cls="selector_program", op="program", offset=SELECTOR, length=2))
    if rule == "conforming" and package:                         # S-02.048: the selector read back
        ops.append(dict(cls="selector_readback", op="read", offset=SELECTOR, length=2))
    for i, o in enumerate(ops, 1):
        o["n"] = i
    return t, ops


def after_slot_change(old):
    """Target slot state once its contents differ from what the old record describes."""
    return "badcrc" if old == "good" else old


def cut_state(start_sel, start_slots, t, ops, i, torn):
    """Flash state for a cut after op i (boundary) or inside op i (torn). i = 0: before any op.
    Returns (selector, slots) where selector is a tuple or ('superset_of', tuple)."""
    sel = start_sel
    slots = list(start_slots)
    seen = ops[:i] if not torn else ops[:i - 1]
    for o in seen:
        c = o["cls"]
        if c in ("slot_erase", "slot_program"):
            slots[t] = after_slot_change(start_slots[t])
        elif c == "record_erase":
            slots[t] = "erased"
        elif c == "record_program":
            slots[t] = "good"
        elif c == "selector_erase":
            sel = (0xFF, 0xFF)
        elif c == "selector_program":
            sel = (t, 0)
    if torn:
        c = ops[i - 1]["cls"]
        if c in ("slot_erase", "slot_program"):
            slots[t] = after_slot_change(start_slots[t])
        elif c in ("record_erase", "record_program"):
            slots[t] = "badtag"           # a torn record is modeled as badtag (S-02.016)
        elif c == "selector_erase":
            sel = ("superset_of", sel)      # each bit old or 1
        elif c == "selector_program":
            sel = ("superset_of", (t, 0))   # each bit cleared or not
        # readback: no flash change
    return sel, tuple(slots)


def superset_values(sel):
    v = (sel[0] << 8) | sel[1]
    free = [b for b in range(16) if not (v >> b) & 1]
    out = []
    for m in range(1 << len(free)):
        x = v
        for j, b in enumerate(free):
            if (m >> j) & 1:
                x |= 1 << b
        out.append(((x >> 8) & 0xFF, x & 0xFF))
    return out


def outcome(profile, sel, slots, baseline_good, precondition):
    """(safe, boots) for one concrete selector."""
    boots = run_resets(profile, sel, slots)
    good = boots_good(boots, slots)
    if any(b == "open" for b in boots):
        return "open", boots
    if any(b == "unspecified" for b in boots):
        return "unspecified", boots
    if precondition:
        return all(good), boots
    return all(g or not bg for g, bg in zip(good, baseline_good)), boots


def eval_cut(profile, sel, slots, baseline_good, precondition):
    if isinstance(sel[0], str):
        vals = superset_values(sel[1])
        res = [outcome(profile, v, slots, baseline_good, precondition)[0] for v in vals]
        if "open" in res:
            safe = "open"
        else:
            safe = all(r is True for r in res)
        return safe, "varies(%d values)" % len(vals)
    safe, boots = outcome(profile, sel, slots, baseline_good, precondition)
    return safe, " ".join(str(b) for b in boots)


_CLOSURE = {}
_REACH = {}
_MASKS = {}
_N = 1 << 16


def _clr_masks():
    """CLR[b]: the set (as a 65,536-bit integer) of 16-bit values with bit b clear."""
    out = []
    for b in range(16):
        w = 1 << b
        unit = (1 << w) - 1                      # w ones, then w zeros: period 2w
        m, span = unit, 2 * w
        while span < _N:
            m |= m << span
            span *= 2
        out.append(m)
    return out


CLR = _clr_masks()


def up(m):
    """Up-closure of a set of 16-bit values: every value with at least the same 1 bits."""
    for b in range(16):
        m |= (m & CLR[b]) << (1 << b)
    return m


def _masks(profile, slots):
    key = (profile, slots)
    if key not in _MASKS:
        f = PROFILES[profile]
        bad = opn = 0
        for x in range(_N):
            r = f((x >> 8, x & 0xFF), slots)
            if not isinstance(r["boot"], int):
                opn |= 1 << x
            elif slots[r["boot"]] != "good":
                bad |= 1 << x
        _MASKS[key] = (bad, opn)
    return _MASKS[key]


def _writers(profile, slots):
    """Per index byte i: (mask of the selectors with index i on which one reset of `profile` writes the
    selector, the set of values those resets write)."""
    key = ("w", profile, slots)
    if key not in _MASKS:
        f = PROFILES[profile]
        out = []
        for i in range(256):
            m = 0
            vals = set()
            for v in range(256):
                r = f((i, v), slots)
                if r["writes"] != 0:
                    m |= 1 << ((i << 8) | v)
                    vals.update(x for x in r["sel_after"] if isinstance(x, tuple))
            out.append((m, sorted(vals)))
        _MASKS[key] = out
    return _MASKS[key]


def multicut_safe(profile, sel, slots):
    """Second-order cuts: the first stage's own selector write (erase 0xD000, program 2 bytes) may itself be
    torn, any number of times. R = the selectors reachable at the start of some reset; safe iff every value
    in R boots a good slot. `sel` is a tuple or ('superset_of', tuple). Which resets write, and what, comes
    from the profile (S-02.032-.034)."""
    key = (profile, sel, slots)
    if key in _CLOSURE:
        return _CLOSURE[key]
    writers = _writers(profile, slots)
    s0 = up(1 << ((sel[1][0] << 8) | sel[1][1])) if isinstance(sel[0], str) else 1 << ((sel[0] << 8) | sel[1])
    r = s0
    while True:
        gen = 0
        for i in range(256):
            m, vals = writers[i]
            w = r & m
            if not w:
                continue
            gen |= w                                           # torn erase of a writing reset's selector
            for vv in vals:
                gen |= 1 << ((vv[0] << 8) | vv[1])             # torn program: up() of the value written
        nr = s0 | up(gen)
        if nr == r:
            break
        r = nr
    _REACH[key] = r
    bad, opn = _masks(profile, slots)
    safe = False if r & bad else ("open" if r & opn else True)
    _CLOSURE[key] = safe
    return safe


def multicut_reach(profile, sel, slots):
    """The set R of multicut_safe, as a list of selector tuples."""
    key = (profile, sel, slots)
    if key not in _REACH:
        _CLOSURE.pop(key, None)
        multicut_safe(profile, sel, slots)
    r = _REACH[key]
    return [(x >> 8, x & 0xFF) for x in range(_N) if (r >> x) & 1]


def reads_record(profile, sel, slots, s):
    """True if one reset of `profile` on selector `sel` reads slot s's record (S-02.020, .021, .030, .031,
    .033)."""
    index, verified = sel
    c = boot_rule(index)
    if profile == "factory":
        return verified != 1 and c == s
    if c == s:
        return True
    return conforming_check(slots[c]) != "pass"


TORN_MODEL = {
    "erase": "each bit old value or 1",
    "program": "each bit to be cleared: old or 0",
    "slot": "a torn slot erase or program leaves the target slot bad (badcrc if it was good)",
    "record": "a torn record erase or program is modeled as badtag",
    "selector": "a torn selector erase or program gives every value with at least the final value's 1 bits "
                "(superset_of)",
}


START_STATES = [
    # selector, slot0, slot1, note
    ((0x00, 0x01), "good", "good", "slot 0 in use, trusted"),
    ((0x01, 0x01), "good", "good", "slot 1 in use, trusted (the factory state)"),
    ((0x02, 0x01), "good", "good", "odd index, trusted"),
    ((0xFF, 0x01), "good", "good", "erased selector after the first stage verified slot 1"),
    ((0xFF, 0xFF), "good", "good", "erased selector"),
    ((0x00, 0x00), "good", "good", "selected slot 0 not yet checked (second upgrade in one session)"),
    ((0x01, 0x00), "good", "good", "selected slot 1 not yet checked (second upgrade in one session)"),
    ((0x00, 0x01), "good", "erased", "slot 1 never written"),
    ((0x01, 0x01), "badcrc", "good",
     "reached from {00 00} with slot 0 bad: the first stage wrote {01 01} and runs slot 0 (S-02.023)"),
    ((0x01, 0x01), "good", "badcrc", "precondition violated: selected slot corrupted after it was trusted"),
    ((0x00, 0x01), "badcrc", "good", "precondition violated: selected slot corrupted after it was trusted"),
    ((0xFF, 0xFF), "good", "badcrc", "precondition violated: erased selector, slot 1 bad"),
    ((0x01, 0x01), "badcrc", "badcrc",
     "precondition violated, Q7 state: reached from {00 00} with slot 0 bad; the first stage wrote {01 01} "
     "without reading slot 1, which is bad too, and runs slot 0 (S-02.023)"),
]


def powercut_data():
    seqs = []
    cuts = []
    for rule in ("conforming", "stock_rule"):
        for k, (sel, s0, s1, note) in enumerate(START_STATES, 1):
            slots = (s0, s1)
            precondition = slots[boot_rule(sel[0])] == "good"
            t, ops = upgrade_ops(rule, sel, package=True)
            selected = boot_rule(sel[0])
            fp_ok = all(not (SLOT[selected] <= o["offset"] < SLOT[selected] + SLOT_MAX)
                        and o["offset"] != RECORD[selected]
                        for o in ops if o["op"] != "read")
            sid = "%s-%02d" % ("C" if rule == "conforming" else "R", k)
            seq_safe = {}
            seq_results = {p: [] for p in PROFILES}
            mc_results = {p: [] for p in PROFILES}
            base_good = {p: boots_good(run_resets(p, sel, slots), slots) for p in PROFILES}
            points = [(0, False)] + [(i, tor) for i in range(1, len(ops) + 1) for tor in (True, False)]
            for i, torn in points:
                csel, cslots = cut_state(sel, slots, t, ops, i, torn)
                row = {"sequence": sid, "op_n": i, "kind": "none" if i == 0 else ("torn" if torn else "after"),
                       "op_class": ops[i - 1]["cls"] if i else "-",
                       "selector": ("superset_of " + hx(csel[1])) if isinstance(csel[0], str) else hx(csel),
                       "slot0": cslots[0], "slot1": cslots[1]}
                for p in PROFILES:
                    safe, boots = eval_cut(p, csel, cslots, base_good[p], precondition)
                    row[p + "_boots"] = boots
                    row[p + "_safe"] = safe
                    seq_results[p].append(safe)
                    mc = multicut_safe(p, csel, cslots) if precondition else "n/a"
                    row[p + "_safe_multicut"] = mc
                    mc_results[p].append(mc)
                cuts.append(row)
            seq_mc = {}
            for p in PROFILES:
                rs = seq_results[p]
                seq_safe[p] = False if False in rs else next((r for r in rs if r is not True), True)
                rs = mc_results[p]
                seq_mc[p] = False if False in rs else next((r for r in rs if r is not True), True)
            seqs.append({
                "id": sid, "writer_rule": rule,
                "start": {"selector": hx(sel), "slot0": s0, "slot1": s1, "note": note},
                "precondition": precondition, "selected": selected, "target": t,
                "selector_written": hx((t, 0)), "image_length": IMAGE_LEN,
                "ops": [{"n": o["n"], "class": o["cls"], "op": o["op"], "offset": "0x%05x" % o["offset"],
                         "length": o["length"]} for o in ops],
                "footprint_ok": fp_ok,
                "start_allowed": (sel[1] == 1 and precondition) if rule == "conforming" else "n/a",
                "safe": seq_safe,
                "safe_multicut": seq_mc,
            })
    return {"image_length": IMAGE_LEN, "resets_per_cut": RESETS,
            "sequences": seqs}, cuts


CUT_FIELDS = ["sequence", "op_n", "kind", "op_class", "selector", "slot0", "slot1",
              "factory_boots", "factory_safe", "factory_safe_multicut",
              "conforming_boots", "conforming_safe", "conforming_safe_multicut"]


# ---------------------------------------------------------------- layout and formats

def layout():
    return {
        "flash_size": "0x100000", "sector": "0x1000", "page": "0x100", "erased_byte": "0xff",
        "protected_end": "0xd000", "slot_capacity": "0x78000",
        "areas": [
            {"name": "first_stage", "start": "0x00000", "end": "0x0c000", "first_stage_max": "0xb000",
             "mirror_sector": {"start": "0x0b000", "end": "0x0c000", "content": "0x0b000+0x68"}},
            {"name": "metadata", "start": "0x0c000", "end": "0x0d000", "content": "0x0c000+0x34"},
            {"name": "selector", "start": "0x0d000", "end": "0x0e000", "content": "0x0d000+2"},
            {"name": "record0", "start": "0x0e000", "end": "0x0f000", "content": "0x0e000+12"},
            {"name": "record1", "start": "0x0f000", "end": "0x10000", "content": "0x0f000+12"},
            {"name": "slot0", "start": "0x10000", "end": "0x88000",
             "settings_sector": {"start": "0x86000", "end": "0x87000", "usable_while_length_at_most": "0x76000"}},
            {"name": "slot1", "start": "0x88000", "end": "0x100000",
             "settings_sector": {"start": "0xfe000", "end": "0xff000", "usable_while_length_at_most": "0x76000"}},
        ],
    }


def pattern(n, seed):
    return bytes(((i * 7 + seed) ^ (i >> 8)) & 0xFF for i in range(n))


def records():
    sels = []
    for b in ["00 00", "00 01", "01 00", "01 01", "02 01", "02 00", "ff ff", "ff 01", "7f 02", "80 fe",
              "00 ff", "01 ff", "fe 01"]:
        i, v = (int(x, 16) for x in b.split())
        sels.append({"bytes": b, "index": i, "verified": v, "selected_slot": boot_rule(i),
                     "check_on_next_boot": v != 1})
    recs = []
    for name, crc, ln, tag in [("typical", 0x1D0C2B3A, 0x1345, TAG), ("max_length", 0xCAFEF00D, SLOT_MAX, TAG),
                               ("badtag_low", 0x12345678, 0x1000, 0x0000B005),
                               ("badtag_high", 0x12345678, 0x1000, 0x0001B004),
                               ("erased", 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF), ("empty", 0, 0, TAG)]:
        raw = crc.to_bytes(4, "little") + ln.to_bytes(4, "little") + tag.to_bytes(4, "little")
        recs.append({"name": name, "crc32": "0x%08x" % crc, "length": "0x%x" % ln, "tag": "0x%08x" % tag,
                     "bytes": raw.hex(" "), "tag_ok": tag == TAG,
                     "length_in_range": 1 <= ln <= SLOT_MAX})
    crcs = [{"name": "check", "data_ascii": "123456789", "crc32": "0x%08x" % zlib.crc32(b"123456789")},
            {"name": "zero_length", "length": 0, "crc32": "0x00000000"},
            {"name": "one_zero_byte", "data_hex": "00", "crc32": "0x%08x" % zlib.crc32(b"\x00")},
            {"name": "erased_sector", "fill": "0xff", "length": SECTOR,
             "crc32": "0x%08x" % zlib.crc32(b"\xff" * SECTOR)}]
    for n, seed in [(IMAGE_LEN, 3), (0x2345, 5), (SLOT_MAX, 11)]:
        crcs.append({"name": "pattern_%x_%d" % (n, seed), "pattern": "((i*7+seed) ^ (i>>8)) & 0xff",
                     "seed": seed, "length": n, "crc32": "0x%08x" % zlib.crc32(pattern(n, seed))})
    trial = []                                       # S-02.016: trial mark records (complemented CRC)
    for n, seed in [(IMAGE_LEN, 3), (0x2345, 5), (LOG_SECTOR, 11), (SLOT_MAX, 13)]:
        c = zlib.crc32(pattern(n, seed))
        raw = (c ^ 0xFFFFFFFF).to_bytes(4, "little") + n.to_bytes(4, "little") + TAG.to_bytes(4, "little")
        trial.append({"pattern": "((i*7+seed) ^ (i>>8)) & 0xff", "seed": seed, "length": n,
                      "crc32_of_bytes": "0x%08x" % c, "record_bytes": raw.hex(" ")})
    return {"selector": sels, "record": recs, "crc": crcs, "trial_mark": trial,
            "metadata_magic": {"offset": "0xc000", "value": "0xdeadbabe",
                               "bytes": META_MAGIC.to_bytes(4, "little").hex(" "), "record_length": "0x34"}}


# ------------------------------------------------ the session option (S-02.055-.059)

COMPONENTS = [
    # name, protected, SPI-flash areas the step writes (S-02.042)
    ("bootloader", True, ["target_slot", "target_record", "selector"]),
    ("os", False, []),
    ("content", False, []),
    ("nordic_application", False, []),
    ("nordic_bootloader", False, []),
    ("psoc_backplane", False, []),
]
UPGRADE_STATE_AREAS = ["metadata", "metadata_mirror"]   # S-02.049, .057, .064
# S-11.073: the steps a build may switch off; this version switches none off
SWITCHABLE = ["nordic_application", "nordic_bootloader", "psoc_backplane"]


def install_case(case, package, option, selected=None, switched_off=()):
    """One package case's steps. selected: the components selected on the unit (S-11.096); None: every
    one. A protected component is selected only with the option on. switched_off: of SWITCHABLE, the
    steps a build switched off (S-11.073)."""
    selected = [n for n, _, _ in COMPONENTS] if selected is None else list(selected)
    if not option:
        selected = [n for n in selected if n != "bootloader"]
    steps = []
    areas = set(UPGRADE_STATE_AREAS)
    for name, protected, comp_areas in COMPONENTS:
        if protected and not option:
            steps.append({"component": name, "action": "skip", "report": "skipped", "reason": "option_off",
                          "flash_ops": 0})
        elif name in switched_off:
            steps.append({"component": name, "action": "skip", "report": "skipped", "reason": "switched_off",
                          "flash_ops": 0})
        elif name not in selected:
            steps.append({"component": name, "action": "skip", "report": "skipped", "reason": "not_selected",
                          "flash_ops": 0})
        else:
            steps.append({"component": name, "action": "install",
                          "report": ("written_with_option" if protected else "step_result"),
                          "flash_ops": "per S-02.042-.049" if name == "bootloader" else 0})
            areas.update(comp_areas)
    return {"case": case, "package": package, "option": "on" if option else "off", "selected": selected,
            "steps": steps, "spi_flash_areas": sorted(areas)}


def package_steps():
    return {
        "components": [{"name": n, "protected": p, "spi_flash_areas": a} for n, p, a in COMPONENTS],
        "upgrade_state_areas": UPGRADE_STATE_AREAS,
        "cases": [
            install_case("option_off", "waffle-boot", False),
            install_case("option_on", "waffle-boot", True),
            install_case("option_on_bootloader_not_selected", "waffle-boot", True, ("os", "content")),
            install_case("option_on_only_bootloader", "waffle-boot", True, ("bootloader",)),
            install_case("option_off_te", "factory", False),
            install_case("option_on_te", "factory", True),
            install_case("option_on_te_bootloader_not_selected", "factory", True, ("os", "content")),
            install_case("option_on_switched_off", "waffle-boot", True, switched_off=SWITCHABLE),
        ],
    }


def cut_phase(op_class, kind):
    """S-02.094: the phase a cut falls in."""
    if kind == "none":
        return "not_started"
    if op_class in ("slot_erase", "slot_program", "record_erase") or (op_class == "record_program"
                                                                       and kind == "torn"):
        return "partial"
    if op_class in ("record_program", "readback"):
        return "complete"
    if (op_class == "selector_program" and kind == "after") or op_class == "selector_readback":
        return "selected"
    return "selector"


def target_booted(profile, sel, slots, t):
    vals = superset_values(sel[1]) if isinstance(sel[0], str) else [sel]
    return any(b == t for v in vals for b in run_resets(profile, v, slots))


NEXT_BOOT_FIELDS = ["sequence", "op_n", "kind", "op_class", "phase", "selected_at_start", "target",
                 "target_state", "selector",
                 "factory_boots", "factory_target_booted", "factory_safe", "factory_safe_multicut",
                 "conforming_boots", "conforming_target_booted", "conforming_safe", "conforming_safe_multicut"]


def next_boot_rows():
    """S-02.094: one row per cut point of every conforming sequence that meets the precondition, the
    resets judged on the flash as the cut left it."""
    rows = []
    for k, (sel, s0, s1, note) in enumerate(START_STATES, 1):
        slots = (s0, s1)
        selected = boot_rule(sel[0])
        if slots[selected] != "good":
            continue
        t, ops = upgrade_ops("conforming", sel, package=True)
        base_good = {p: boots_good(run_resets(p, sel, slots), slots) for p in PROFILES}
        points = [(0, False)] + [(i, tor) for i in range(1, len(ops) + 1) for tor in (True, False)]
        for i, torn in points:
            csel, cslots = cut_state(sel, slots, t, ops, i, torn)
            kind = "none" if i == 0 else ("torn" if torn else "after")
            cls = ops[i - 1]["cls"] if i else "-"
            row = {"sequence": "C-%02d" % k, "op_n": i, "kind": kind, "op_class": cls,
                   "phase": cut_phase(cls, kind), "selected_at_start": selected, "target": t,
                   "target_state": cslots[t],
                   "selector": ("superset_of " + hx(csel[1])) if isinstance(csel[0], str) else hx(csel)}
            for p in PROFILES:
                safe, boots = eval_cut(p, csel, cslots, base_good[p], True)
                row[p + "_boots"] = boots
                row[p + "_target_booted"] = target_booted(p, csel, cslots, t)
                row[p + "_safe"] = safe
                row[p + "_safe_multicut"] = multicut_safe(p, csel, cslots)
            rows.append(row)
    return rows


# ---------------------------------------------------------------- the boot key chord (S-02.080-.084)

KEY_SETS = ["", "1", "2", "4", "1+2", "1+4", "2+4", "1+2+4", "3", "1+3", "unreadable"]
CHORDS = {"4": "recovery"}
CHORD_SELECTORS = [(0x00, 0x00), (0x00, 0x01), (0x01, 0x00), (0x01, 0x01), (0x02, 0x01), (0xFF, 0xFF)]
CHORD_FIELDS = ["keys", "chord", "selector", "slot0", "slot1", "boot", "slave_slot", "announce",
                "recovery_target", "recovery_refusal", "running", "running_front", "recovery_running_way",
                "recovery_trial"]


def chord_rows():
    """chord_table.csv's rows (S-02.080-.084): per key set, and for the recovery chord per selector, slot
    states and running slot, the boot, the recovery target or refusal, the running-slot write's way and
    the trial write's result at recovery's start."""
    rows = []
    for keys in KEY_SETS:
        chord = CHORDS.get(keys, "none")       # S-02.080
        if chord == "none":
            rows.append({"keys": keys or "-", "chord": "none", "selector": "-", "slot0": "-", "slot1": "-",
                         "boot": "by_selector", "slave_slot": "running", "announce": "none",
                         "recovery_target": "-", "recovery_refusal": "-", "running": "-",
                         "running_front": "-", "recovery_running_way": "-", "recovery_trial": "-"})
            continue
        for sel in CHORD_SELECTORS:
            for s0 in ("good", "badcrc"):
                for s1 in ("good", "badcrc"):
                    slots = (s0, s1)
                    selected = boot_rule(sel[0])
                    ways = [("selected", f) for f in (("intact",) if slots[selected] == "good"
                                                      else ("intact", "damaged"))]
                    for running, front in ways + [("other", "-"), ("unknown", "-")]:
                        if running == "unknown":
                            refusal = "not_running"                      # S-02.084, S-01.002
                        elif running == "other":
                            refusal = "not_selected"
                        elif sel[1] != 0x01:
                            refusal = "unverified"                       # S-02.090
                        elif front != "intact":
                            refusal = "selected_bad"                     # S-02.084, status 5
                        else:
                            refusal = "none"
                        target = 1 - selected if refusal == "none" else "none"   # S-02.043
                        if refusal != "none":
                            way = "refused"
                        else:                                            # S-02.084
                            way = "select_first" if slots[1 - selected] == "good" else "refused"
                        trial = "refused" if refusal != "none" else (
                            "trial" if slots[selected] == "good" else "trial_state")
                        rows.append({"keys": keys, "chord": chord, "selector": hx(sel), "slot0": s0,
                                     "slot1": s1, "boot": "recovery", "slave_slot": "-",
                                     "announce": "recovery", "recovery_target": target,
                                     "recovery_refusal": refusal, "running": running,
                                     "running_front": front, "recovery_running_way": way,
                                     "recovery_trial": trial})
    return rows


# ------------------------------------------------ recovery's write of the running slot (S-02.084)

RUNNING_STARTS = [
    # selector, the running slot A's state (its front intact), the other slot B's state
    ((0x00, 0x01), "good", "good"), ((0x00, 0x01), "badcrc", "good"),
    ((0x00, 0x01), "good", "badcrc"), ((0x00, 0x01), "badcrc", "badcrc"),
    ((0x01, 0x01), "good", "good"), ((0x01, 0x01), "badcrc", "good"),
    ((0x01, 0x01), "good", "badcrc"), ((0x01, 0x01), "badcrc", "badcrc"),
]


def sel_of_hx(s):
    x, y = s.split()
    return (int(x, 16), int(y, 16))


def running_ops(a, way, n=IMAGE_LEN, close=False):
    """The ops of recovery's write of the running slot A (S-02.084, S-02.045-.050), by way: select_first
    or in_place (close: in_place with the final selector write kept)."""
    assert n <= SLOT_MAX - 2 * SECTOR  # S-02.084
    b = 1 - a
    ops = []
    if way == "select_first":
        ops.append(dict(cls="select_erase", op="erase", offset=SELECTOR, length=SECTOR))
        ops.append(dict(cls="select_program", op="program", offset=SELECTOR, length=2, value=hx((b, 1))))
        ops.append(dict(cls="select_read", op="read", offset=SELECTOR, length=2))
    _, up_ops = upgrade_ops("conforming", (b, 1), n, recovery=True)   # a selector naming B targets A
    for o in up_ops:
        if way == "in_place" and not close and o["cls"] in ("selector_erase", "selector_program"):
            continue
        o = dict(o)
        o.pop("n")
        if o["cls"] == "selector_program":
            o["value"] = hx((a, 0))
        ops.append(o)
    for i, o in enumerate(ops, 1):
        o["n"] = i
    return ops


def running_cut_state(sel, slots, a, ops, i, torn, stream="start"):
    """The flash after a cut after op i (boundary) or inside op i (torn): (selector, slots, A's stream).
    A's stream is `start` while no byte of A has changed (or `stream`, the state a failed write of this
    recovery left), `partial` from its first erase until its last program has completed, `whole` after it
    (every byte of the image in place; the record may still be old or erased). A torn selector is
    ('superset_of', value), as in cut_state()."""
    slots = list(slots)
    state = {"sel": sel, "stream": stream}
    last_program = max(o["n"] for o in ops if o["cls"] == "slot_program")

    def apply(o, torn_op):
        c = o["cls"]
        if c in ("select_erase", "selector_erase"):
            state["sel"] = ("superset_of", state["sel"]) if torn_op else (0xFF, 0xFF)
        elif c in ("select_program", "selector_program"):
            v = sel_of_hx(o["value"])
            state["sel"] = ("superset_of", v) if torn_op else v
        elif c in ("slot_erase", "slot_program"):
            slots[a] = after_slot_change(slots[a])
            if o["n"] > last_program or (o["n"] == last_program and not torn_op):
                state["stream"] = "whole"          # the last sector's erase comes after the last program
            else:
                state["stream"] = "partial"
        elif c in ("record_erase", "record_program"):
            slots[a] = "badtag" if torn_op else ("erased" if c == "record_erase" else "good")

    for o in (ops[:i] if not torn else ops[:i - 1]):
        apply(o, False)
    if torn:
        apply(ops[i - 1], True)
    return state["sel"], tuple(slots), state["stream"]


def running_outcome(profile, sel, slots, a, stream):
    """(safe, boots) for one concrete selector: every reset boots a good slot, or A while its stream is
    `start` or `whole`."""
    boots = run_resets(profile, sel, slots)
    ok = all(isinstance(x, int) and (slots[x] == "good" or (x == a and stream in ("start", "whole")))
             for x in boots)
    return ok, boots


def plain_ops(a, n=IMAGE_LEN):
    """The default write of the other slot B while the selector names A (S-02.043-.049), {B, 0x00} last."""
    t, ops = upgrade_ops("conforming", (a, 1), n, recovery=True)
    ops = [dict(o) for o in ops]
    for o in ops:
        if o["cls"] == "selector_program":
            o["value"] = hx((t, 0))
    return ops


def running_late(profile, sel, slots, a, stream):
    """How many of the RESETS starts come before every later one boots a good slot (or A as it may
    start): 0, 1, ... or 'never'."""
    _, boots = running_outcome(profile, sel, slots, a, stream)
    ok = [isinstance(x, int) and (slots[x] == "good" or (x == a and stream in ("start", "whole"))) for x in boots]
    for k in range(len(ok)):
        if all(ok[k:]):
            return k
    return "never"


def sel_values(csel):
    """The concrete selectors a cut state stands for: a value; ('superset_of', v), every value with at
    least v's 1 bits; ('superset_index', v) or ('superset_verified', v), the same in that byte alone, the
    other byte v's."""
    if not isinstance(csel[0], str):
        return [csel]
    v = csel[1]
    if csel[0] == "superset_of":
        return superset_values(v)
    if csel[0] == "superset_index":
        return [(x[0], v[1]) for x in superset_values((v[0], 0xFF))]
    if csel[0] == "superset_verified":
        return [(v[0], x[1]) for x in superset_values((0xFF, v[1]))]
    if csel[0] == "subset_index":                   # a torn program from the old index toward 0x00
        return [(x, v[1]) for x in range(256) if x & ~v[0] == 0]
    return [(v[0], y) for y in range(256) if y & ~v[1] == 0 and y & 1]   # subset_verified, toward 0x01


def sel_text(csel):
    return ("%s %s" % (csel[0], hx(csel[1]))) if isinstance(csel[0], str) else hx(csel)


def _cut_row(i, torn, ops, csel, cslots, a, stream):
    row = {"op_n": i, "kind": "none" if i == 0 else ("torn" if torn else "after"),
           "op_class": ops[i - 1]["cls"] if i else "-",
           "selector": sel_text(csel),
           "slot0": cslots[0], "slot1": cslots[1], "a_stream": stream}
    for p in PROFILES:
        vals = sel_values(csel)
        late = [running_late(p, v, cslots, a, stream) for v in vals]
        row[p + "_late"] = "never" if "never" in late else max(late)
        if len(vals) > 1:
            row[p + "_safe"] = all(running_outcome(p, v, cslots, a, stream)[0] for v in vals)
            row[p + "_boots"] = "varies(%d values)" % len(vals)
        else:
            safe, boots = running_outcome(p, vals[0], cslots, a, stream)
            row[p + "_safe"] = safe
            row[p + "_boots"] = " ".join(str(x) for x in boots)
    return row


def split_selector(ops):
    """S-02.084, S-02.048: each 2-byte selector program split into the index byte, its 1-byte read, the
    verified byte, then the 2-byte read."""
    out = []
    for k, o in enumerate(ops):
        if o["cls"] not in ("select_program", "selector_program"):
            out.append(dict(o))
            continue
        base = o["cls"]
        idx, ver = o["value"].split()
        out.append(dict(cls=base + "_index", op="program", offset=SELECTOR, length=1, value=idx))
        out.append(dict(cls=base + "_index_read", op="read", offset=SELECTOR, length=1))
        out.append(dict(cls=base + "_verified", op="program", offset=SELECTOR + 1, length=1, value=ver))
        out.append(dict(cls=base.replace("_program", "_read"), op="read", offset=SELECTOR, length=2))
    for i, o in enumerate(out, 1):
        o["n"] = i
    return out


def damaged_cut_state(sel, slots, a, t, ops, i, torn, stream):
    """running_cut_state() for a write of target t (A or B) while A is damaged, with the selector
    programmed in one 2-byte program or a byte at a time: a torn byte program leaves that byte with at
    least its final value's 1 bits and the other byte as it was."""
    slots = list(slots)
    state = {"sel": sel, "stream": stream}
    progs = [o["n"] for o in ops if o["cls"] == "slot_program"]
    last_program = max(progs) if progs else 0

    def apply(o, torn_op):
        c = o["cls"]
        if c in ("select_erase", "selector_erase"):
            state["sel"] = ("superset_of", state["sel"]) if torn_op else (0xFF, 0xFF)
        elif c in ("select_program", "selector_program"):
            v = sel_of_hx(o["value"])
            state["sel"] = ("superset_of", v) if torn_op else v
        elif c.endswith("_zero_index"):             # a program alone: bits cleared, toward 0x00
            v = state["sel"]
            state["sel"] = ("subset_index", v) if torn_op else (0x00, v[1])
        elif c.endswith("_one_verified"):           # toward 0x01, bit 0 kept
            v = state["sel"]
            state["sel"] = ("subset_verified", v) if torn_op else (v[0], 0x01)
        elif c.endswith("_index"):
            v = (int(o["value"], 16), state["sel"][1])
            state["sel"] = ("superset_index", v) if torn_op else v
        elif c.endswith("_verified"):
            v = (state["sel"][0], int(o["value"], 16))
            state["sel"] = ("superset_verified", v) if torn_op else v
        elif c in ("slot_erase", "slot_program"):
            slots[t] = after_slot_change(slots[t])
            if t == a:
                state["stream"] = "whole" if (o["n"] > last_program or (o["n"] == last_program and not torn_op)) \
                    else "partial"
        elif c in ("record_erase", "record_program"):
            slots[t] = "badtag" if torn_op else ("erased" if c == "record_erase" else "good")

    for o in (ops[:i] if not torn else ops[:i - 1]):
        apply(o, False)
    if torn:
        apply(ops[i - 1], True)
    return state["sel"], tuple(slots), state["stream"]


def damaged_cuts(sel, slots, a, t, ops, stream):
    """The cut rows, each with factory_cut: the first stage with its own selector write cut
    short in any way (factory_cut())."""
    cuts = []
    for i, torn in _points(ops):
        csel, cslots, cstream = damaged_cut_state(sel, slots, a, t, ops, i, torn, stream)
        row = _cut_row(i, torn, ops, csel, cslots, a, cstream)
        row["factory_cut"] = factory_cut(csel, cslots, a, cstream)
        if t == a and torn and ops[i - 1]["cls"] in ("record_erase", "record_program"):
            # S-02.052, S-02.026: a torn record of A judged both ways, as empty and as badcrc
            tear = {}
            for outcome, state in (("passes", "empty"), ("fails", "badcrc")):
                s2 = list(cslots)
                s2[a] = state
                tear[outcome] = factory_cut(csel, tuple(s2), a, cstream)
            row["record_tear"] = tear
        cuts.append(row)
    return cuts


def factory_unsafe(cuts):
    """The cuts after which some path of the first stage, its own write cut, ends on no slot
    that starts."""
    return [{"op_n": c["op_n"], "kind": c["kind"], "op_class": c["op_class"]} for c in cuts
            if c["factory_cut"] is not True]


def class14(t, n=IMAGE_LEN):
    """S-02.045-.047, S-02.103 for target t: its sectors, the last sector's erase, the settings sector's
    erase, the record, the read-back."""
    _, ops = upgrade_ops("conforming", (1 - t, 1), n, recovery=True)
    return [dict(o) for o in ops if o["cls"] not in ("selector_erase", "selector_program")]


def settled(sel, t):
    """S-02.084: True when the selector needs no settling on target t."""
    return boot_rule(sel[0]) == t and (sel[1] == 0x01 or t == 1)


def settle_ops(cur, t, prefix, force_erase=False):
    """S-02.084: the ops that settle the selector on slot t from the value cur, and the value after.
    force_erase: always take the erase path (the control)."""
    ops = [dict(cls=prefix + "_settle_read", op="read", offset=SELECTOR, length=2)]
    if settled(cur, t):
        return ops, cur
    if t == 0 and cur[1] & 0x01 and not force_erase:
        if cur[0] != 0x00:
            ops += [dict(cls=prefix + "_zero_index", op="program", offset=SELECTOR, length=1, value="00"),
                    dict(cls=prefix + "_zero_index_read", op="read", offset=SELECTOR, length=1)]
        if cur[1] != 0x01:
            ops.append(dict(cls=prefix + "_one_verified", op="program", offset=SELECTOR + 1, length=1,
                            value="01"))
        ops.append(dict(cls=prefix + "_read", op="read", offset=SELECTOR, length=2))
        return ops, (0x00, 0x01)
    ops += [dict(cls=prefix + "_erase", op="erase", offset=SELECTOR, length=SECTOR),
            dict(cls=prefix + "_program", op="program", offset=SELECTOR, length=2, value=hx((t, 1)))]
    return ops, (t, 0x01)


def rescue_body(sel, a, t, way, force_erase=False):
    """The ops of a write of target t while A is damaged, before splitting, and the selector after it: for
    select_first the settling on B first; the slot's classes 1-4 (for selector_only the read-back
    alone); then the settling on t."""
    body, cur = [], sel
    if way == "select_first":
        ops, cur = settle_ops(cur, 1 - a, "select", force_erase)
        body += ops
    body += [dict(o) for o in class14(t) if way != "selector_only" or o["cls"] == "readback"]
    ops, cur = settle_ops(cur, t, "selector", force_erase)
    body += ops
    for i, o in enumerate(body, 1):
        o["n"] = i
    return body, cur


def regressions(cuts, key):
    """The cuts that are not safe after an earlier cut of the same write was (key: factory_cut is True,
    or factory_late is not 'never')."""
    ok = [c["factory_cut"] is True if key == "cut" else c["factory_late"] != "never" for c in cuts]
    return [{"op_n": c["op_n"], "kind": c["kind"], "op_class": c["op_class"]}
            for k, c in enumerate(cuts) if not ok[k] and any(ok[:k])]


def rescue_sequence(k, sel, slots, a, t, way, prior, control_way=None, stream="partial"):
    """A write of target t while the running slot A is damaged (S-02.084), selector writes a byte at a
    time, with its controls: one 2-byte program (control_), the erase path (erase_control_) and
    control_way."""
    body, end = rescue_body(sel, a, t, way)
    ops = split_selector(body)
    cuts = damaged_cuts(sel, slots, a, t, ops, stream)
    control = damaged_cuts(sel, slots, a, t, body, stream)
    ebody, _ = rescue_body(sel, a, t, way, force_erase=True)
    erase_cuts = damaged_cuts(sel, slots, a, t, split_selector(ebody), stream)
    out = {
        "id": "W-%02d" % k,
        "start": {"selector": hx(sel), "slot0": slots[0], "slot1": slots[1], "a_stream": stream},
        "prior": prior,
        "running": a, "way": way, "target": t,
        "selector_end": hx(end),
        "ops": _ops_out(ops),
        "safe": {p: all(c[p + "_safe"] for c in cuts) for p in PROFILES},
        "factory_never": factory_never(cuts),
        "control_factory_never": factory_never(control),
        "factory_cut_unsafe": factory_unsafe(cuts),
        "control_factory_cut_unsafe": factory_unsafe(control),
        "regressions": {"late": regressions(cuts, "late"), "cut": regressions(cuts, "cut")},
        "erase_control_regressions": {"late": regressions(erase_cuts, "late"),
                                      "cut": regressions(erase_cuts, "cut")},
        "cuts": cuts,
    }
    if control_way:
        cbody, _ = rescue_body(sel, a, a, control_way, force_erase=True)
        ccuts = damaged_cuts(sel, slots, a, a, split_selector(cbody), stream)
        out["way_control"] = {"way": control_way, "settling": "erase",
                              "regressions": {"late": regressions(ccuts, "late"),
                                              "cut": regressions(ccuts, "cut")}}
    return out


def factory_never(cuts):
    """The selector-operation cuts after which the first stage, uncut from then on, never reaches
    a good slot."""
    return [{"op_n": c["op_n"], "kind": c["kind"], "op_class": c["op_class"]} for c in cuts
            if c["factory_late"] == "never" and c["op_class"].startswith(("select", "selector"))]


def _points(ops):
    return [(0, False)] + [(i, t) for i in range(1, len(ops) + 1) for t in (True, False)]


def _ops_out(ops):
    return [dict({"n": o["n"], "class": o["cls"], "op": o["op"], "offset": "0x%05x" % o["offset"],
                  "length": o["length"]}, **({"value": o["value"]} if "value" in o else {})) for o in ops]


def _failed(write, target, ops):
    """A write of this recovery that ended in a flash failure at its first erase, which took effect."""
    return {"write": write, "target": target, "failed_op": 1, "result": "flash", "effect": "taken",
            "ops": _ops_out(ops[:1])}


def running_data():
    seqs = []
    for k, (sel, sa, sb) in enumerate(RUNNING_STARTS, 1):
        a = boot_rule(sel[0])
        b = 1 - a
        slots = [None, None]
        slots[a], slots[b] = sa, sb
        way = "select_first" if sb == "good" else "in_place"
        prior = []
        if way == "in_place":
            # S-02.084: preceded by a failed write of B, which leaves B bad
            prior = [_failed("plain", b, plain_ops(a))]
            slots[b] = after_slot_change(sb)
        slots = tuple(slots)
        ops = running_ops(a, way)
        cuts = []
        for i, torn in _points(ops):
            csel, cslots, stream = running_cut_state(sel, slots, a, ops, i, torn)
            cuts.append(_cut_row(i, torn, ops, csel, cslots, a, stream))
        seqs.append({
            "id": "W-%02d" % k,
            "start": {"selector": hx(sel), "slot0": slots[0], "slot1": slots[1], "a_stream": "start"},
            "prior": prior,
            "running": a, "way": way, "target": a,
            "selector_end": hx(sel) if way == "in_place" else hx((a, 0)),
            "ops": _ops_out(ops),
            "safe": {p: all(c[p + "_safe"] for c in cuts) for p in PROFILES},
            "cuts": cuts,
        })
    a1, b1 = 1, 0
    in_place_failed = {a: _failed("in_place", a, running_ops(a, "in_place")) for a in (0, 1)}
    plain_failed = {a: _failed("plain", 1 - a, plain_ops(a)) for a in (0, 1)}
    # A damaged by a failed in-place write; the default write of B follows.
    for sel in ((0x00, 0x01), (0x01, 0x01)):
        a = boot_rule(sel[0])
        seqs.append(rescue_sequence(len(seqs) + 1, sel, ("badcrc", "badcrc"), a, 1 - a, "plain",
                                    [plain_failed[a], in_place_failed[a]]))
    # A (slot 1) damaged, the selector FF FF or {A, 3F}: with B good selector_only and select_first,
    # with B bad in_place.
    b20, _ = rescue_body((a1, 0x01), a1, b1, "plain")
    for sel in ((0xFF, 0xFF), (a1, 0x3F)):
        for way, sb in (("selector_only", "good"), ("select_first", "good"), ("in_place", "badcrc")):
            slots = ("badcrc", "badcrc") if sb != "good" else ("good", "badcrc")
            prior = [plain_failed[a1], in_place_failed[a1]]
            if sb == "good":
                prior.append({"write": "plain", "target": b1, "result": "selector",
                              "note": "run 20: the whole write of B from {A, 01}, its index program not "
                                      "stored; the selector then set as the start gives it",
                              "ops": _ops_out(split_selector(b20))})
            seqs.append(rescue_sequence(len(seqs) + 1, sel, slots, a1, b1 if way == "selector_only" else a1,
                                        way, prior))
    # A damaged, B good and named with verified 00: the write of A. Both mirrors.
    for sel, a in (((0x00, 0x00), 1), ((0x01, 0x00), 0)):
        slots = [None, None]
        slots[a], slots[1 - a] = "badcrc", "good"
        seqs.append(rescue_sequence(len(seqs) + 1, sel, tuple(slots), a, a, "plain", [in_place_failed[a]]))
    # The selector names A with verified 00, A bad, B good: the write of A. Both mirrors, with the
    # select_first control.
    for sel, a in (((0x01, 0x00), 1), ((0x00, 0x00), 0)):
        slots = [None, None]
        slots[a], slots[1 - a] = "badcrc", "good"
        seqs.append(rescue_sequence(len(seqs) + 1, sel, tuple(slots), a, a, "plain", [in_place_failed[a]],
                                    control_way="select_first"))
    # A written and read back good, the selector not settled, B bad: selector_only on A. From FF FF and
    # 00 00, both mirrors.
    for sel in ((0xFF, 0xFF), (0x00, 0x00)):
        for a in (0, 1):
            slots = [None, None]
            slots[a], slots[1 - a] = "good", "badcrc"
            unsettled = {"write": "in_place", "target": a, "result": "selector",
                         "note": "A written and read back good, the settling not stored; the selector then "
                                 "set as the start gives it"}
            seqs.append(rescue_sequence(len(seqs) + 1, sel, tuple(slots), a, a, "selector_only",
                                        [plain_failed[a], in_place_failed[a], unsettled], stream="whole"))
    # A (slot 1) damaged, B = slot 0 good, the selector 00 03: selector_only on B, and plain on A.
    for way, t in (("selector_only", 0), ("plain", 1)):
        seqs.append(rescue_sequence(len(seqs) + 1, (0x00, 0x03), ("good", "badcrc"), 1, t, way,
                                    [in_place_failed[1]]))
    # A (slot 0) damaged, B = slot 1 good, the selector 00 FE: select_first on A, and selector_only on B.
    for way, t in (("select_first", 0), ("selector_only", 1)):
        seqs.append(rescue_sequence(len(seqs) + 1, (0x00, 0xFE), ("badcrc", "good"), 0, t, way,
                                    [in_place_failed[0]]))
    return {
        "image_length": IMAGE_LEN,
        "sequences": seqs,
        "trial_sequences": trial_data(),
    }


# ---------------------------------------------------------------- another front, by trial (S-02.084)

def _trial_class14(b, n=IMAGE_LEN):
    ops = class14(b, n)
    for o in ops:
        if o["cls"] == "record_program":
            o["value"] = "trial_mark"
    return ops


def trial_ops(b, n=IMAGE_LEN):
    """S-02.084: the ops of the trial write of B."""
    rd = dict(cls="trial_read", op="read", offset=SELECTOR, length=2)
    log = SLOT[b] + LOG_SECTOR
    ops = []
    for o in _trial_class14(b, n):
        ops.append(o)
        if o["cls"] == "slot_erase" and o["offset"] == log - SECTOR:   # S-02.095, after S-02.103's erase
            ops += [dict(cls="trial_log", op="program", offset=log, length=1, value="fd"),
                    dict(cls="trial_log_read", op="read", offset=log, length=1)]
    ops.append(dict(cls="readback", op="read", offset=log, length=1))
    ops += [dict(cls="trial_erase", op="erase", offset=SELECTOR, length=SECTOR), dict(rd)]
    if b == 0:
        ops += [dict(cls="trial_verified", op="program", offset=SELECTOR + 1, length=1, value="fe"), dict(rd),
                dict(cls="trial_index", op="program", offset=SELECTOR, length=1, value="00"), dict(rd)]
    for i, o in enumerate(ops, 1):
        o["n"] = i
    return ops


def trial_start_ops(sel, b, n=IMAGE_LEN):
    """The ops of a start on a selector naming the tried slot B (S-02.021, .022, .095)."""
    after = (0x01, 0x01) if sel[0] == 0x00 else (0x00, 0x01)
    ops = [dict(cls="fs_read", op="read", offset=SELECTOR, length=2),
           dict(cls="fs_read", op="read", offset=RECORD[b], length=12),
           dict(cls="fs_read", op="read", offset=SLOT[b], length=n),
           dict(cls="fs_erase", op="erase", offset=SELECTOR, length=SECTOR),
           dict(cls="fs_program", op="program", offset=SELECTOR, length=2, value=hx(after)),
           # S-02.095
           dict(cls="mark_read", op="read", offset=SLOT[b] + LOG_SECTOR, length=SECTOR),
           dict(cls="mark", op="program", offset=SLOT[b] + LOG_SECTOR, length=1, value="fc")]
    for i, o in enumerate(ops, 1):
        o["n"] = i
    return ops


def select_ops(b, n=IMAGE_LEN, pending=False):
    """S-02.084, S-02.095: the ops of the select of the tried slot B (pending: entry 0 pending)."""
    log = SLOT[b] + LOG_SECTOR
    ops = [dict(cls="select_log_read", op="read", offset=log, length=1)]
    if pending:
        ops += [dict(cls="select_log", op="program", offset=log, length=1, value="fc"),
                dict(cls="select_log_read", op="read", offset=log, length=1)]
    ops += [dict(cls="record_erase", op="erase", offset=RECORD[b], length=SECTOR),
           dict(cls="record_program", op="program", offset=RECORD[b], length=12, value="record"),
           dict(cls="readback", op="read", offset=RECORD[b], length=12),
           dict(cls="readback", op="read", offset=SLOT[b], length=n),
           dict(cls="selector_erase", op="erase", offset=SELECTOR, length=SECTOR),
           dict(cls="selector_program", op="program", offset=SELECTOR, length=2, value=hx((b, 0))),
           dict(cls="selector_read", op="read", offset=SELECTOR, length=2)]
    for i, o in enumerate(ops, 1):
        o["n"] = i
    return ops


def trial_cut_state(sel, slots, b, ops, i, torn, stream, log="ff"):
    """The flash after a cut after op i (boundary) or inside op i (torn): (selector, slots, B's stream, byte 0
    of B's last sector: 'ff', 'fd', 'fc', or 'old|new' for a torn one-bit program)."""
    slots = list(slots)
    state = {"sel": sel, "stream": stream, "log": log}
    progs = [o["n"] for o in ops if o["cls"] == "slot_program"]
    last_program = max(progs) if progs else 0

    def apply(o, t):
        c = o["cls"]
        cur = state["sel"]
        if c in ("slot_erase", "slot_program"):
            slots[b] = after_slot_change(slots[b])
            state["stream"] = "whole" if (o["n"] > last_program or (o["n"] == last_program and not t)) \
                else "partial"
            if c == "slot_erase" and o["offset"] == SLOT[b] + LOG_SECTOR:
                state["log"] = ("%s|ff" % state["log"]) if t and state["log"] != "ff" else "ff"
        elif c in ("trial_log", "mark", "select_log"):
            state["log"] = ("%s|%s" % (state["log"], o["value"])) if t else o["value"]
        elif c == "record_erase":
            slots[b] = "badtag" if t else "erased"
        elif c == "record_program":
            slots[b] = "badtag" if t else ("tried" if o["value"] == "trial_mark" else "good")
        elif c in ("trial_erase", "selector_erase", "fs_erase"):
            state["sel"] = ("superset_of", cur) if t else (0xFF, 0xFF)
        elif c in ("selector_program", "fs_program"):
            v = sel_of_hx(o["value"])
            state["sel"] = ("superset_of", v) if t else v
        elif c == "trial_verified":
            v = (cur[0], int(o["value"], 16))
            state["sel"] = ("superset_verified", v) if t else v
        elif c == "trial_index":
            v = (int(o["value"], 16), cur[1])
            state["sel"] = ("superset_index", v) if t else v

    for o in (ops[:i] if not torn else ops[:i - 1]):
        apply(o, False)
    if torn:
        apply(ops[i - 1], True)
    return state["sel"], tuple(slots), state["stream"], state["log"]


def started(slots, b, log):
    """S-02.095: B tried and entry 0 finished: True, False, or 'either' (a torn program)."""
    if slots[b] != "tried":
        return False
    vals = {v == "fc" for v in log.split("|")}
    return "either" if len(vals) > 1 else vals.pop()


def trusts(sel, b):
    """The selector names B with verified 0x01: the first stage boots B on every reset unchecked."""
    return boot_rule(sel[0]) == b and sel[1] == 0x01


_TRUST = {}


def trust_mask(b):
    if b not in _TRUST:
        m = 0
        for x in range(1 << 16):
            if trusts((x >> 8, x & 0xFF), b):
                m |= 1 << x
        _TRUST[b] = m
    return _TRUST[b]


def trial_walk(profile, sel, slots):
    """The selectors met at the start of each of RESETS uncut resets and after the last, and the boots."""
    f = PROFILES[profile]
    seen, boots = [], []
    for _ in range(RESETS):
        seen.append(sel)
        r = f(sel, slots)
        boots.append(r["boot"])
        sel = r["sel_after"][0]
    seen.append(sel)
    return seen, boots


def trial_cut_row(i, torn, ops, csel, cslots, b, stream, log):
    row = {"op_n": i, "kind": "none" if i == 0 else ("torn" if torn else "after"),
           "op_class": ops[i - 1]["cls"] if i else "-", "selector": sel_text(csel),
           "slot0": cslots[0], "slot1": cslots[1], "b_stream": stream, "b_log": log,
           "b_started": started(cslots, b, log)}
    vals = sel_values(csel)
    # S-02.084: some value the cut leaves names A with verified 0x01, B tried and started
    row["select_from_old_front"] = row["b_started"] is not False and any(
        boot_rule(v[0]) == 1 - b and v[1] == 0x01 for v in vals)
    for p in PROFILES:
        walks = [trial_walk(p, v, cslots) for v in vals]
        row[p + "_boots"] = (" ".join(str(x) for x in walks[0][1]) if len(vals) == 1
                             else "varies(%d values)" % len(vals))
        row[p + "_boots_ok"] = all(isinstance(x, int) and (cslots[x] == "good" or (x == b and stream == "whole"))
                                   for _, bt in walks for x in bt)
        if p == "factory":
            row["factory_trusts_tried"] = cslots[b] == "tried" and any(trusts(s, b) for sn, _ in walks
                                                                         for s in sn)
    reach = factory_closure(start_mask(csel), cslots)
    row["factory_cut_trusts_tried"] = cslots[b] == "tried" and (reach & trust_mask(b)) != 0
    return row


def trial_sequence(k, way, place, sel, slots, running, b, stream, log="ff"):
    ops = (trial_ops(b) if way == "trial" else trial_start_ops(sel, b) if way == "trial_start"
           else select_ops(b, pending=log == "fd" and place == "tried_front"))
    cuts = []
    for i, torn in _points(ops):
        csel, cslots, cstream, clog = trial_cut_state(sel, slots, b, ops, i, torn, stream, log)
        cuts.append(trial_cut_row(i, torn, ops, csel, cslots, b, cstream, clog))
    end, _, _, _ = trial_cut_state(sel, slots, b, ops, len(ops), False, stream, log)

    def where(key):
        return [{"op_n": c["op_n"], "kind": c["kind"], "op_class": c["op_class"]} for c in cuts if c[key]]
    return {"id": "W-%02d" % k, "way": way, "place": place, "running": running, "target": b,
            "start": {"selector": hx(sel), "slot0": slots[0], "slot1": slots[1], "b_stream": stream,
                      "b_log": log},
            "selector_end": hx(end), "ops": _ops_out(ops),
            "boots_ok": {p: all(c[p + "_boots_ok"] for c in cuts) for p in PROFILES},
            "factory_trusts_tried": where("factory_trusts_tried"),
            "factory_cut_trusts_tried": where("factory_cut_trusts_tried"),
            "cuts": cuts}


def trial_data():
    """W-29 to W-36: the trial write, the trial start and the select, for B = slot 0 (A = slot 1,
    selector 01 01) and B = slot 1 (A = slot 0, selector 00 01)."""
    seqs = []
    starts = {0: (0x01, 0x01), 1: (0x00, 0x01)}

    def pair(b, sb, sa):
        s = [None, None]
        s[b], s[1 - b] = sb, sa
        return tuple(s)
    for b in (0, 1):
        seqs.append(trial_sequence(29 + b, "trial", "old_front", starts[b], pair(b, "good", "good"), 1 - b, b,
                                   "old"))
    for b in (0, 1):
        sel = (0x00, 0xFE) if b == 0 else (0xFF, 0xFF)
        seqs.append(trial_sequence(31 + b, "trial_start", "first_stage", sel, pair(b, "tried", "good"), 1 - b,
                                   b, "whole", "fd"))
    # old_front: B tried and started; tried_front: entry 0 pending
    for place, k, log in (("old_front", 33, "fc"), ("tried_front", 35, "fd")):
        for b in (0, 1):
            seqs.append(trial_sequence(k + b, "select", place, starts[b], pair(b, "tried", "good"),
                                       1 - b if place == "old_front" else b, b, "whole", log))
    return seqs


# ---------------------------------------------------------------- which slot a start reaches (S-02.084)

REACH_FIELDS = ["selector", "slot0", "slot1", "reach", "factory", "conforming", "factory_cut", "old_rule"]
SURE_TO_FAIL = ("badtag", "erased", "badcrc")     # S-02.021


def reach_rule(sel, slots):
    """S-02.084: 'named', 'fallback' or 'none', the slot a start reaches under the factory profile with its
    selector write cut short in any way."""
    n = boot_rule(sel[0])
    if sel[1] == 0x01:
        return "named" if slots[n] == "good" else "none"
    if slots[n] == "good":
        return "named" if n == 1 or slots[1] == "good" else "none"
    if n == 0 and slots[1] == "good" and sel[1] & 0xFE and slots[0] in SURE_TO_FAIL:
        return "fallback"
    return "none"


def old_reach_rule(sel, slots):
    """The negative control for reach_rule()."""
    n = boot_rule(sel[0])
    if slots[n] == "good":
        return "named"
    if sel[1] != 0x01 and slots[1 - n] == "good" and slots[n] in SURE_TO_FAIL:
        return "fallback"
    return "none"


_EQ = {}
_OK = {}


def _eq_mask(byte, val):
    """The 16-bit selector values whose index (byte 0) or verified (byte 1) byte is val."""
    key = (byte, val)
    if key not in _EQ:
        m = 0
        for x in range(256):
            m |= 1 << ((val << 8 | x) if byte == 0 else (x << 8 | val))
        _EQ[key] = m
    return _EQ[key]


def start_mask(csel):
    """The values a cut state stands for (sel_values()), as a set of 16-bit values."""
    if not isinstance(csel[0], str):
        return 1 << (csel[0] << 8 | csel[1])
    v = csel[1]
    if csel[0].startswith("subset_"):
        m = 0
        for x in sel_values(csel):
            m |= 1 << (x[0] << 8 | x[1])
        return m
    m = up(1 << (v[0] << 8 | v[1]))
    if csel[0] == "superset_index":
        return m & _eq_mask(1, v[1])
    if csel[0] == "superset_verified":
        return m & _eq_mask(0, v[0])
    return m


def _ok_masks(slots, a, stream):
    """(ok, unspecified): the selector values from which one uncut factory reset settles on a good slot
    (or A while its stream is `start` or `whole`), and those whose reset is unspecified (S-02.026)."""
    key = (slots, a, stream)
    if key not in _OK:
        ok = uns = 0
        for x in range(1 << 16):
            r = factory_reset((x >> 8, x & 0xFF), slots)
            if r["writes"] == "unspecified":
                uns |= 1 << x
                continue
            end = (x >> 8, x & 0xFF) if r["writes"] == 0 else r["sel_after"][0]
            n = boot_rule(end[0])
            if slots[n] == "good" or (n == a and stream in ("start", "whole")):
                ok |= 1 << x
        _OK[key] = (ok, uns)
    return _OK[key]


def factory_closure(start, slots):
    """Every selector value reachable from the values in start under the factory profile, its selector
    write (erase, then program) torn any number of times: up-closures of the old and the new values."""
    writers = _writers("factory", slots)
    r = start
    while True:
        gen = 0
        for i in range(256):
            m, vals = writers[i]
            w = r & m
            if not w:
                continue
            gen |= w
            for vv in vals:
                gen |= 1 << ((vv[0] << 8) | vv[1])
        nr = start | up(gen)
        if nr == r:
            return r
        r = nr


def factory_cut(csel, slots, a=None, stream=None):
    """True when every path of factory_closure() ends on a good slot (or A as it may start);
    'unspecified' where a path meets S-02.026; else False."""
    ok, uns = _ok_masks(tuple(slots), a, stream)
    reach = factory_closure(start_mask(csel), tuple(slots))
    if reach & uns:
        return "unspecified"
    return reach & ~ok == 0


def reach_class(slots, first, second):
    """From a profile's first two boots: 'first' (the first start boots a good slot), 'second' (the
    second does), 'never', or 'unspecified'."""
    if "unspecified" in (first, second):
        return "unspecified"
    if isinstance(first, int) and slots[first] == "good":
        return "first"
    if isinstance(second, int) and slots[second] == "good":
        return "second"
    return "never"


def reach_rows():
    """reach.csv's rows: reach_rule() against both profiles, over first_stage_table.csv's rows."""
    rows = []
    for r in first_stage_rows():
        sel, slots = sel_of_hx(r["selector"]), (r["slot0"], r["slot1"])
        row = {"selector": r["selector"], "slot0": r["slot0"], "slot1": r["slot1"],
               "reach": reach_rule(sel, slots)}
        for p in PROFILES:
            row[p] = reach_class(slots, r[p + "_boot"], r[p + "_reset2_boot"])
        fc = factory_cut(sel, slots)
        row["factory_cut"] = "unspecified" if fc == "unspecified" else ("reached" if fc else "not")
        row["old_rule"] = old_reach_rule(sel, slots)
        rows.append(row)
    return rows


# ---------------------------------------------------------------- the start log (S-02.095)

LOG_FIELDS = ["case", "selector", "record_tag", "record_length", "log", "patch", "fault",
              "state", "slot", "next", "unfinished", "program", "byte_after", "starts", "entry"]
LOG_LIMIT = 3            # unfinished starts in a row before recovery


def log_base(slot):
    return SLOT[slot] + LOG_SECTOR


def log_sector(log, patch):
    """The sector from the compact form: a hex prefix, then 0xFF, then an optional 'offset:hex' patch."""
    sec = bytearray(b"\xff" * SECTOR)
    pre = bytes.fromhex(log)
    sec[:len(pre)] = pre
    if patch != "-":
        off, data = patch.split(":")
        data = bytes.fromhex(data)
        sec[int(off, 16):int(off, 16) + len(data)] = data
    return bytes(sec)


def start_log(sel, tag, length, sec, fault):
    """S-02.095: a start log row's expectations and the ops the step makes."""
    out = {"slot": "-", "next": "-", "unfinished": "-", "program": "-", "byte_after": "-", "starts": "main",
           "entry": "-"}
    ops = []
    if fault == "read_selector":
        return dict(out, state="read_failed"), ops
    slot = boot_rule(sel[0])
    out["slot"] = slot
    if fault == "read_record":
        return dict(out, state="read_failed"), ops
    if not (tag == TAG and 1 <= length <= LOG_SECTOR):
        return dict(out, state="none"), ops
    if fault == "read_log":
        return dict(out, state="read_failed"), ops
    # Entry k: two bits of byte k div 4, the started bit low. F free, U started and not finished,
    # D finished or closed, X not valid.
    names = {3: "F", 2: "U", 0: "D", 1: "X"}
    s = "".join(names[(byte >> (2 * q)) & 3] for byte in sec for q in range(4))
    if "F" not in s:
        return dict(out, state="odd" if "X" in s else "full"), ops
    m = re.fullmatch(r"([UD]*)(F+)", s)
    if not m:
        return dict(out, state="odd"), ops
    used = m.group(1)
    nxt = len(used)
    unfinished = len(used) - len(used.rstrip("U"))
    out.update(state="ok", next=nxt, unfinished=unfinished)
    if unfinished >= LOG_LIMIT:
        entry, bit, goes = nxt - 1, 1, "recovery"           # the finished bit of the newest entry
    else:
        entry, bit, goes = nxt, 0, "main_entry"            # the started bit of the first free entry
    off = log_base(slot) + entry // 4
    old = sec[entry // 4]
    value = 0xFF ^ (1 << (2 * (entry % 4) + bit))
    ops.append(("program", off, value))
    out["program"] = "0x%05x:%02x" % (off, value)
    if fault in ("program", "stuck"):                       # no entry named, the main image
        out["byte_after"] = "%02x" % old
        return out, ops
    out["byte_after"] = "%02x" % (old & value)
    out["starts"] = goes
    if goes == "main_entry":
        out["entry"] = entry
    return out, ops


def _log_case(case, log="", patch="-", sel=(0x00, 0x01), tag=TAG, length=IMAGE_LEN, fault="none"):
    return {"case": case, "selector": hx(sel), "record_tag": "0x%08x" % tag, "record_length": "0x%x" % length,
            "log": log, "patch": patch, "fault": fault}


def start_log_rows():
    """start_log.csv's rows. `log` is the sector's first bytes, the rest 0xFF; `patch` puts bytes at an
    offset in the sector. The record is the selected slot's. `fault`: a read of the selector, the record or
    the sector fails; the program fails (`program`) or leaves the byte as it was (`stuck`)."""
    cases = [
        _log_case("empty"),
        _log_case("position 1", "fc"), _log_case("position 2", "f0"), _log_case("position 3", "c0"),
        _log_case("byte boundary", "00"),
        _log_case("before page boundary", "00" * 255 + "c0"), _log_case("page boundary", "00" * 256),
        _log_case("last entry", "00" * 4095 + "c0"),
        _log_case("full, finished", "00" * 4096), _log_case("full, unfinished", "aa" * 4096),
        _log_case("full, an entry not valid", "00" * 4095 + "01"),
        _log_case("odd: entry not valid", "fd"), _log_case("odd: not valid after used", "f4"),
        _log_case("odd: used after free", "f3"), _log_case("odd: used after free, next byte", "ff00"),
        _log_case("odd: byte later in sector", "fc", "800:7f"),
        _log_case("odd: last byte", "", "fff:fe"),
        _log_case("count 1 after finished", "f8"),
        _log_case("count 2 after finished", "e8"), _log_case("count 3 after finished", "a8"),
        _log_case("count 4 after finished", "a8fe"),
        _log_case("count 1 after unfinished, finished", "88"),
        _log_case("three unfinished", "ea"), _log_case("after recovery", "ca"),
        _log_case("four unfinished", "aa"),
        _log_case("record: tag", tag=0x0000B005), _log_case("record: erased", tag=0xFFFFFFFF, length=0xFFFFFFFF),
        _log_case("record: length 0", length=0), _log_case("record: length 1", length=1),
        _log_case("record: length 0x77000", length=LOG_SECTOR),
        _log_case("record: length 0x77001", length=LOG_SECTOR + 1),
        _log_case("record: length 0x78000", length=SLOT_MAX),
        _log_case("read: selector", fault="read_selector"), _log_case("read: record", fault="read_record"),
        _log_case("read: log", "ea", fault="read_log"),
        _log_case("program fails", fault="program"), _log_case("program fails, three", "ea", fault="program"),
        _log_case("byte unchanged", fault="stuck"), _log_case("byte unchanged, three", "ea", fault="stuck"),
        _log_case("slot 1", sel=(0x01, 0x01)), _log_case("slot 1, three", "ea", sel=(0x01, 0x01)),
        _log_case("slot 1, index 02", "fc", sel=(0x02, 0x01)),
        _log_case("slot 1, erased selector", sel=(0xFF, 0xFF)),
        _log_case("slot 0, verified 00", "f8", sel=(0x00, 0x00)),
    ]
    rows = []
    for c in cases:
        exp, _ = start_log(sel_of_hx(c["selector"]), int(c["record_tag"], 16), int(c["record_length"], 16),
                           log_sector(c["log"], c["patch"]), c["fault"])
        rows.append(dict(c, **exp))
    return rows


# ---------------------------------------------------------------- the metadata mirror (S-02.060-.065)

def _meta_record(sw):
    """A metadata record with factory fields, calibration and both pad fields set; sw_version `sw`."""
    r = bytearray(META_LEN)
    r[0x00:0x04] = META_MAGIC.to_bytes(4, "little")
    r[0x04:0x0E] = b"SN000123\x00\x5a"
    r[0x0E:0x10] = (42).to_bytes(2, "little")
    r[0x10:0x12] = (1).to_bytes(2, "little")
    r[0x12:0x1A] = b"".join(v.to_bytes(2, "little") for v in (1, 0, 0, 0))
    r[0x1A:0x22] = b"".join(v.to_bytes(2, "little") for v in sw)
    r[0x22:0x24] = b"\x11\x22"
    r[0x24:0x28] = (0).to_bytes(4, "little")
    r[0x28:0x2C] = (0x12345678).to_bytes(4, "little")
    r[0x2C:0x2E] = (0x0800).to_bytes(2, "little")
    r[0x2E:0x32] = (0x0123).to_bytes(2, "little") + (0x0456).to_bytes(2, "little")
    r[0x32:0x34] = b"\x33\x44"
    return bytes(r)


OLD_REC = _meta_record((1, 2, 3, 4))
NEW_REC = _meta_record((1, 2, 4, 0))
OTHER_REC = (OLD_REC[:0x12] + (2).to_bytes(2, "little") + OLD_REC[0x14:0x24] + (0xAB0D).to_bytes(4, "little")
             + OLD_REC[0x28:])      # another writer's record: os_version 2.0.0.0, state 0xAB0D
TAIL = [(0x034, pattern(0xCC, 0x51)), (0xF00, pattern(0x100, 0x52))]      # non-FF sector bytes
META_FIELDS = [("magic", 0x00, 0x04), ("serial", 0x04, 0x0E), ("sku", 0x0E, 0x10), ("factory", 0x10, 0x12),
               ("os_version", 0x12, 0x1A), ("sw_version", 0x1A, 0x22), ("pad22", 0x22, 0x24),
               ("state", 0x24, 0x28), ("cv_gain", 0x28, 0x2C), ("cv_zero", 0x2C, 0x2E),
               ("cv_pair2", 0x2E, 0x32), ("pad32", 0x32, 0x34)]
FACTORY_BYTES = [(0x04, 0x12), (0x28, 0x34)]         # S-02.062
META_RULES = ["conforming", "factory", "neg_c_first", "neg_magic_only", "neg_c_wins"]
META_STARTS = ["mirror_old", "mirror_absent"]
RESTORE_STARTS = ["pending_old", "pending_erased"]
ERASE_PATTERNS = ["zeros:0-4", "bit:4.2", "seed:0", "seed:1", "seed:2"]
PROGRAM_PATTERNS = ["ones:0-4", "zeros:4-5", "seed:0", "seed:1", "seed:2"]


def mirror_bytes(rec):
    """S-02.063: the record, then its bitwise complement."""
    return bytes(rec) + bytes(~b & 0xFF for b in rec)


def mirror_valid(b, magic_only=False):
    r, c = bytes(b[:META_LEN]), bytes(b[META_LEN:MIRROR_LEN])
    if r[:4] != META_MAGIC.to_bytes(4, "little"):
        return None
    if not magic_only and any((x ^ y) != 0xFF for x, y in zip(r, c)):
        return None
    return r


def _sector_with(rec, tail=True):
    sec = bytearray(b"\xff" * SECTOR)
    sec[:META_LEN] = rec
    if tail:
        for off, b in TAIL:
            sec[off:off + len(b)] = b
    return sec


def _start_flash(start):
    mir = bytearray(b"\xff" * SECTOR)
    meta = _sector_with(OLD_REC)
    if start in ("mirror_old", "stale"):
        mir[:MIRROR_LEN] = mirror_bytes(OLD_REC)
    if start in ("stale", "written_then_other"):
        meta[:META_LEN] = OTHER_REC
    if start in RESTORE_STARTS:
        mir[:MIRROR_LEN] = mirror_bytes(NEW_REC)
        if start == "pending_erased":
            meta = bytearray(b"\xff" * SECTOR)
    return {MIRROR: mir, META: meta}


def _pages(base, content):
    return [("program", base + p, bytes(content[p:p + PAGE])) for p in range(0, SECTOR, PAGE)
            if bytes(content[p:p + PAGE]).count(0xFF) != PAGE]


def meta_write_ops(rule, fl):
    """S-02.064: the metadata write's ops under `rule` (conforming, factory or a negative control)."""
    if rule == "factory":
        return [("erase", META, None), ("program", META, NEW_REC)]
    target = bytearray(fl[META])
    target[:META_LEN] = NEW_REC
    mirror = [("erase", MIRROR, None), ("program", MIRROR, mirror_bytes(NEW_REC)), ("read", MIRROR, MIRROR_LEN)]
    meta = [("erase", META, None)] + _pages(META, target) + [("read", META, SECTOR)]
    first = [("read", META, SECTOR)]
    if rule == "neg_c_first":
        return first + meta + mirror
    # S-02.064 step 0: the sector read a second time, a page at a time, before the first erase; step 7:
    # the mirror erased once the record reads back.
    again = [("read", META + p, PAGE) for p in range(0, SECTOR, PAGE)]
    return first + again + mirror + meta + ([("erase", MIRROR, None)] if rule == "conforming" else [])


def _mask(pattern, data_before):
    kind, arg = pattern.split(":")
    if kind == "seed":
        rnd = random.Random(int(arg))
        return lambda i: rnd.getrandbits(8)
    if kind == "bit":
        i0, k = (int(x) for x in arg.split("."))
        return lambda i: (1 << k) if i == i0 else 0
    a, b = (int(x) for x in arg.split("-"))
    if kind == "ones":
        return lambda i: 0xFF if a <= i < b else 0
    return lambda i: 0 if a <= i < b else 0xFF                       # zeros


def _apply(fl, op, torn=None):
    kind, off, arg = op
    sec, rel = off - off % SECTOR, off % SECTOR
    if kind == "read":
        return
    if kind == "erase":
        if torn is None:
            fl[sec][:] = b"\xff" * SECTOR
        else:
            m = _mask(torn, bytes(fl[sec]))
            for i in range(SECTOR):
                fl[sec][i] |= m(i) & 0xFF                            # each bit old or 1
        return
    m = None if torn is None else _mask(torn, None)
    for i, v in enumerate(arg):
        cur = fl[sec][rel + i]
        if m is None:
            fl[sec][rel + i] = cur & v
        else:
            clear = cur & ~v & 0xFF                                  # the bits this program would clear
            fl[sec][rel + i] = cur & ~(clear & m(i)) & 0xFF


def restore_ops(rule, fl):
    """S-02.065: the restore ops a boot makes from this state, and whether it restores."""
    ops = [("read", MIRROR, MIRROR_LEN), ("read", META, SECTOR)]
    m = mirror_valid(fl[MIRROR], magic_only=(rule == "neg_magic_only"))
    if rule == "factory" or m is None or bytes(fl[META][:META_LEN]) == m:
        return ops if rule != "factory" else [], False
    if rule == "neg_c_wins" and bytes(fl[META][:4]) == META_MAGIC.to_bytes(4, "little"):
        return ops, False
    target = bytearray(fl[META])
    target[:META_LEN] = m
    return ops + [("erase", META, None)] + _pages(META, target) + [("read", META, SECTOR)], True


def _run(fl, ops, cut):
    """Apply ops under a cut: ("after", n) completes ops 1..n; ("torn", n, pattern) tears op n."""
    for k, op in enumerate(ops, 1):
        if cut is not None and cut[0] == "after" and k > cut[1]:
            return
        if cut is not None and cut[0] == "torn" and k == cut[1]:
            _apply(fl, op, cut[2])
            return
        _apply(fl, op)


def _classify(rec):
    rec = bytes(rec)
    return "old" if rec == OLD_REC else "new" if rec == NEW_REC else \
        "other_writer" if rec == OTHER_REC else "absent" if rec[:4] != META_MAGIC.to_bytes(4, "little") else "corrupt"


def _judge(rec):
    """S-02.062: (factory bytes kept, fields ok, safe) for the record the boot presents."""
    rec = bytes(rec)
    factory = all(rec[a:b] == OLD_REC[a:b] for a, b in FACTORY_BYTES)
    fields = all(rec[a:b] in (OLD_REC[a:b], NEW_REC[a:b]) for _n, a, b in META_FIELDS)
    return factory, fields, rec[:4] == META_MAGIC.to_bytes(4, "little") and factory and fields


META_CUT_FIELDS = ["rule", "start", "phase", "op_n", "kind", "pattern", "op", "offset", "mirror_valid", "c_magic",
                   "c_before_restore", "restored", "result", "factory_kept", "fields_ok", "tail_kept", "safe"]


def _cuts(ops):
    out = [("after", n, "") for n in range(len(ops) + 1)]
    for n, op in enumerate(ops, 1):
        for pat in {"erase": ERASE_PATTERNS, "program": PROGRAM_PATTERNS}.get(op[0], []):
            out.append(("torn", n, pat))
    return out


def _row(rule, start, phase, cut, ops, fl):
    """Boot after the cut (for the restore phase, the boot after the cut restore), then judge."""
    rrule = "conforming" if phase == "restore" else rule
    mv = mirror_valid(fl[MIRROR]) is not None
    cmagic = bytes(fl[META][:4]) == META_MAGIC.to_bytes(4, "little")
    before = _classify(fl[META][:META_LEN])
    rops, restored = restore_ops(rrule, fl)
    _run(fl, rops, None)
    factory, fields, safe = _judge(fl[META][:META_LEN])
    tail = all(bytes(fl[META][o:o + len(b)]) == b for o, b in TAIL)
    n = cut[1]
    op = ops[n - 1] if cut[0] == "torn" or (n and n <= len(ops)) else None
    return {"rule": rule, "start": start, "phase": phase, "op_n": n, "kind": cut[0], "pattern": cut[2],
            "op": op[0] if op else "", "offset": "0x%05x" % op[1] if op else "",
            "mirror_valid": "yes" if mv else "no", "c_magic": "yes" if cmagic else "no",
            "c_before_restore": before, "restored": "yes" if restored else "no",
            "result": _classify(fl[META][:META_LEN]), "factory_kept": "yes" if factory else "no",
            "fields_ok": "yes" if fields else "no", "tail_kept": "yes" if tail else "no",
            "safe": "yes" if safe else "no"}


def metadata_data():
    rows = []
    seqs = {}
    for rule in META_RULES:
        for start in META_STARTS:
            ops = meta_write_ops(rule, _start_flash(start))
            seqs.setdefault(rule, [{"n": k, "op": o[0], "offset": "0x%05x" % o[1],
                                    "length": len(o[2]) if o[0] == "program" else (SECTOR if o[0] == "erase" else o[2])}
                                   for k, o in enumerate(ops, 1)])
            for cut in _cuts(ops):
                fl = _start_flash(start)
                _run(fl, ops, cut)
                rows.append(_row(rule, start, "write", cut, ops, fl))
    restore_seqs = {}
    for start in RESTORE_STARTS:
        ops, _ = restore_ops("conforming", _start_flash(start))
        restore_seqs[start] = [{"n": k, "op": o[0], "offset": "0x%05x" % o[1],
                                "length": len(o[2]) if o[0] == "program" else (SECTOR if o[0] == "erase" else o[2])}
                               for k, o in enumerate(ops, 1)]
        for cut in _cuts(ops):
            fl = _start_flash(start)
            _run(fl, ops, cut)
            rows.append(_row("conforming", start, "restore", cut, ops, fl))
    # A stale mirror rolls back another writer's record; after a completed write the mirror sector is
    # erased (S-02.064 step 7), so that writer's record stands.
    for start in ("stale", "written_then_other"):
        fl = _start_flash(start)
        r = _row("conforming", start, "none", ("none", 0, ""), [], fl)
        r["safe"] = "n/a"
        rows.append(r)
    data = {
        "mirror_offset": "0x0b000", "mirror_length": "0x68", "first_stage_limit": "0xb000",
        "old_record": OLD_REC.hex(" "), "new_record": NEW_REC.hex(" "), "other_writer_record": OTHER_REC.hex(" "),
        "mirror_of_old": mirror_bytes(OLD_REC).hex(" "),
        "tail": [{"offset": "0x%03x" % o, "bytes": b.hex(" ")} for o, b in TAIL],
        "factory_bytes": [["0x%02x" % a, "0x%02x" % b] for a, b in FACTORY_BYTES],
        "fields": [{"name": n, "start": "0x%02x" % a, "end": "0x%02x" % b} for n, a, b in META_FIELDS],
        "write_sequences": seqs, "restore_sequences": restore_seqs,
        "patterns": {"seed:N": "random.Random(N).getrandbits(8) per byte of the operation",
                     "zeros:A-B": "mask 0 for bytes A..B-1 of the operation, 0xff elsewhere",
                     "ones:A-B": "mask 0xff for bytes A..B-1, 0 elsewhere",
                     "bit:I.K": "only bit K of byte I",
                     "meaning": "a torn erase raises the mask's bits; a torn program clears the mask's bits "
                                "among those it would clear"},
    }
    return data, rows


# ---------------------------------------------------------------- bootable images (S-02.091)

BOOT_CALL_STACK = (0x11807000, 0x11808000)    # S-02.091, S-01.009, S-01.022


def _ldr():
    sys.path.insert(0, os.path.join(HERE, "..", "conformance"))
    from images import ldr
    return ldr


def small_images():
    """A small slot image accepted by S-02.091 (52 bytes), in a tuple."""
    L = _ldr()
    code = {"target": 0x11A00000, "payload": pattern(4, 16)}
    fin = {"flags": L.FINAL, "target": 0x11A00000, "count": 0}
    slot = L.build_ldr([code, {"flags": L.INIT, "target": 0x11A00000, "count": 0}, fin])
    return (slot,)


def bootable_cases():
    """Each case: an image (hex), its kind (slot), whether S-02.091 accepts it and, when it does not, the
    S-01 rule (check_stream.py's rule name) that refuses it."""
    L = _ldr()
    I, F, X, FI, FL = L.FIRST | L.IGNORE, L.FINAL, L.INIT, L.FILL, L.FILL

    def p(n, seed):
        return pattern(n, seed)

    def slot(pre=(), post=(), inits=1, first_code=4, tail=b"", init_target=0x11A00000):
        b = [{"flags": I, "target": 0x11A00000, "count": 0, "dmacode": first_code},
             {"target": 0x11800000, "payload": p(0x20, 1)},
             {"target": 0x11A00000, "payload": p(0x40, 2)}] + list(pre)
        b += [{"flags": X, "target": init_target, "count": 0}] * inits
        b += [{"flags": I, "target": 0x11A00000, "count": 0},
              {"target": 0x0803C000, "payload": p(0x20, 3)},
              {"target": 0x8803D000, "payload": p(0x20, 4)},
              {"flags": FL, "target": 0x88200000, "count": 0x100, "fill": 0},
              {"target": 0x11A00000, "payload": p(0x40, 5)}] + list(post)
        b += [{"flags": F, "target": 0x11A00000, "count": 0}]
        return L.build_ldr(b) + tail

    good = slot()
    badsig = bytearray(good); badsig[3] = 0xAC
    badsum = bytearray(good); badsum[2] ^= 0x01
    cases = [
        ("good", "slot", good, True, None, "every rule met (a usual layout's shape)"),
        ("good_trailing_bytes", "slot", slot(tail=b"\xff" * 32), True, None,
         "bytes after the FINAL block's end, inside the image, are allowed"),
        ("bad_signature", "slot", bytes(badsig), False, "parse", "first header's signature 0xAC"),
        ("bad_checksum", "slot", bytes(badsum), False, "parse", "first header's checksum byte off by one bit"),
        ("no_final", "slot", good[:-16], False, "parse", "the image ends before a FINAL block"),
        ("final_past_end", "slot", good[:-0x50], False, "parse",
         "a payload runs past the image's end before FINAL"),
        ("no_init", "slot", slot(inits=0), False, "one_init_block", "no INIT block"),
        ("two_init", "slot", slot(inits=2), False, "one_init_block", "two INIT blocks"),
        ("init_target_not_code", "slot", slot(init_target=0x11800000), False, "init_in_l1",
         "the INIT block's target in L1 data, not L1 code"),
        ("l2_before_init", "slot", slot(pre=[{"target": 0x08010000, "payload": p(8, 9)}]), False,
         "init_in_l1", "an L2 destination before the INIT block"),
        ("ddr_before_init", "slot", slot(pre=[{"target": 0x8803D000, "payload": p(8, 10)}]), False,
         "init_before_ddr", "a DDR destination before the INIT block"),
        ("rom_boot_reserve", "slot", slot(post=[{"target": 0x08000800, "payload": p(8, 11)}]), False,
         "rom_boot_reserve", "a block in the ROM's L2 boot reserve after the INIT block"),
        ("record", "slot", slot(post=[{"target": 0x11B01F00, "payload": p(8, 12)}]), False,
         "record_and_rom_stack", "a block over the reboot record"),
        ("outside_spaces", "slot", slot(post=[{"target": 0x20000000, "payload": p(8, 13)}]), False,
         "inside_spaces", "a block aimed at the system MMR space"),
        ("bcode_3", "slot", slot(first_code=3), False, "bcode", "first block's BCODE 0x3 (S-01.020)"),
        ("over_boot_call_stack", "slot",
         slot(post=[{"target": 0x11807F00, "payload": p(0x40, 14)}]), False, "spares_first_stage_stack",
         "a block over 0x11807000-0x11808000, the boot-call stack rule 4 spares under any first stage"),
        ("boot_call_stack_low_edge", "slot",
         slot(post=[{"target": 0x11807000, "payload": p(8, 16)}]), False, "spares_first_stage_stack",
         "a block at the range's first word (the floor of a bootloader's boot-call SP is above it, S-01.022)"),
        ("below_boot_call_stack", "slot",
         slot(post=[{"target": 0x11806FF8, "payload": p(8, 17)}]), True, None,
         "a block ending at 0x11807000, just below the range: accepted"),
        ("small_slot", "slot", small_images()[0], True, None,
         "a small stream: a code block, the INIT block and FINAL (SC-UPG-SIZE pads it)"),
        ("one_byte", "slot", b"\x00", False, "parse", "a 1-byte image cannot hold a block header"),
    ]
    return {
        "boot_call_stack": ["0x%08x" % BOOT_CALL_STACK[0], "0x%08x" % BOOT_CALL_STACK[1]],
        "padding": {"byte": "0xff",
                    "slot_lengths": ["0x2345", "0x78000", "0x78001"]},
        "cases": [{"name": nm, "kind": k, "accepted": ok, "refused_by": rule, "note": note,
                   "length": len(img), "image": img.hex()} for nm, k, img, ok, rule, note in cases],
    }


# ---------------------------------------------------------------- the settings sector (S-02.096-.100, .103-.105)

SET_MAGIC = b"WBSS"             # 57 42 53 53; the old boot store's was WBST
SET_OLD_MAGIC = b"WBST"
SET_HEADER = 12
SET_MAX_PAYLOAD = PAGE - SET_HEADER - 4      # 240
SET_PAGES = SECTOR // PAGE                   # 16
SET_SLOT_MAX = SLOT_MAX - 2 * SECTOR         # 0x76000: S-02.099, S-02.104
SET_SECTOR = tuple(SLOT[k] + SET_SLOT_MAX for k in (0, 1))   # 0x86000, 0xfe000
SET_FLAGS = ("developer_mode", "color", "mtp_macos", "start_in_boot_menu", "host_link_off")
SET_DEFAULTS = 0x04                          # developer mode off, color off, MTP macOS, start in boot menu off,
                                             # host link on (bit 4, 0x10, is payload byte 1's bit 0: off)


def set_record(flags, stamp, payload=None, version=1, length=None, magic=SET_MAGIC, crc=None):
    """S-02.096: magic, version u16, length u16, stamp u32, payload, CRC-32 over all before it. The
    payload a writer writes: byte 0's flags, and with the host link off (0x10) byte 1, 0x01."""
    pl = (bytes([flags & 0x0F]) + (b"\x01" if flags & 0x10 else b"")) if payload is None else payload
    n = len(pl) if length is None else length
    head = magic + version.to_bytes(2, "little") + n.to_bytes(2, "little") + stamp.to_bytes(4, "little") + pl
    c = zlib.crc32(head) if crc is None else crc
    return head + c.to_bytes(4, "little")


def set_decode(page, stamp):
    """S-02.096, S-02.097: (flags, why) for one page; flags None when it gives no settings."""
    if page[:4] != SET_MAGIC:
        return None, "erased" if page == b"\xff" * len(page) else "magic"
    if int.from_bytes(page[4:6], "little") != 1:
        return None, "version"
    n = int.from_bytes(page[6:8], "little")
    if not 1 <= n <= SET_MAX_PAYLOAD:
        return None, "length"
    if zlib.crc32(page[:SET_HEADER + n]) != int.from_bytes(page[SET_HEADER + n:SET_HEADER + n + 4], "little"):
        return None, "crc"
    if int.from_bytes(page[8:12], "little") != stamp:
        return None, "stamp"
    host_off = 0x10 if n >= 2 and page[SET_HEADER + 1] & 0x01 else 0
    return (page[SET_HEADER] & 0x0F) | host_off, "valid"


def set_pages(sector):
    return [sector[i * PAGE:(i + 1) * PAGE] for i in range(SET_PAGES)]


def set_pick(sector, stamp):
    """S-02.097: (flags, page) of the highest page with a valid record holding the stamp; (defaults, None)."""
    if sector is None:
        return SET_DEFAULTS, None
    for i in reversed(range(SET_PAGES)):
        f, _ = set_decode(set_pages(sector)[i], stamp)
        if f is not None:
            return f, i
    return SET_DEFAULTS, None


def set_layout(sector):
    """(used, valid): the pages up to the last used one, and whether no erased page comes before a used one."""
    used, gap, ok = 0, False, True
    for i, pg in enumerate(set_pages(sector)):
        if pg == b"\xff" * PAGE:
            gap = True
        else:
            ok = ok and not gap
            used = i + 1
    return used, ok


def set_usable(run, now=None):
    """S-02.099: the running slot keeps settings. run: dict(established, good, rec) with rec 12 bytes (None:
    unreadable); now: the record read again (default: the same)."""
    rec = run["rec"]
    if not run["established"] or not run["good"] or rec is None:
        return False
    ln, tag = int.from_bytes(rec[4:8], "little"), int.from_bytes(rec[8:12], "little")
    now = rec if now is None else now
    return tag == TAG and 1 <= ln <= SET_SLOT_MAX and now == rec


def set_save_ops(slot, sector, flags, stamp):
    """S-02.098: the operations of a save from this sector and the sector after it, the guard passed."""
    base = SET_SECTOR[slot]
    ops = [["read", "0x%05x" % RECORD[slot], 12], ["read", "0x%05x" % base, SECTOR]]
    have, _ = set_pick(sector, stamp)
    if set_pick(sector, stamp)[1] is not None and have == flags:
        return ops, sector, None
    used, ok = set_layout(sector)
    out = bytearray(sector)
    if not ok or used == SET_PAGES:
        ops += [["erase", "0x%05x" % base], ["read", "0x%05x" % base, SECTOR]]
        out[:] = b"\xff" * SECTOR
        used = 0
    rec = set_record(flags, stamp)
    at = base + used * PAGE
    ops += [["program", "0x%05x" % at, rec.hex()], ["read", "0x%05x" % at, len(rec)]]
    out[used * PAGE:used * PAGE + len(rec)] = rec
    return ops, bytes(out), used


def set_sector_of(pages):
    """A sector from {page index: bytes}; the rest erased."""
    sec = bytearray(b"\xff" * SECTOR)
    for i, pg in pages.items():
        sec[i * PAGE:i * PAGE + len(pg)] = pg
    return bytes(sec)


def set_hex_pages(sector):
    """The sector as {page index: its bytes up to the last byte that is not ff}, erased pages left out."""
    return {str(i): pg.rstrip(b"\xff").hex() for i, pg in enumerate(set_pages(sector)) if pg != b"\xff" * PAGE}


def set_torn_check(stamps):
    """S-02.098: for each stamp, every pair of written records new and other (flag values 0 to 15) where a
    torn program of new on an erased page (bits of new cleared, others not yet) could read as other: other's
    bytes a bitwise superset of new's (1s) and other valid. Stamp and length bytes are the same, so only the
    flag byte and the CRC differ."""
    hits = []
    for st in stamps:
        for f in range(16):
            new = set_record(f, st)
            for g in range(16):
                if g == f:
                    continue
                other = set_record(g, st)
                if all((o & n) == n for o, n in zip(other, new)):
                    hits.append({"stamp": "0x%08x" % st, "new": f, "reads_as": g})
    return hits


def set_recovery_ops(ops, t):
    """S-02.103: a recovery image write's ops, the erase of target t's settings sector named settings_erase,
    checked to come right after the erase of its last sector (the start log), or of the boot logo's header
    sector that follows it, before the trial mark or the record erase."""
    log = SLOT[t] + LOG_SECTOR
    out = []
    for o in ops:
        o = dict(o)
        o.pop("n", None)
        if o["op"] == "erase" and o["offset"] == SET_SECTOR[t]:
            o["cls"] = "settings_erase"
        out.append(o)
    k = [i for i, o in enumerate(out) if o["cls"] == "settings_erase"]
    nxt = [i for i, o in enumerate(out) if o["cls"] in ("trial_log", "record_erase")]
    before = out[k[0] - 1]["offset"]
    assert len(k) == 1 and out[k[0]]["offset"] == SET_SECTOR[t] and \
        (before == log or (before == SLOT[t] + LOGO_SECTOR and out[k[0] - 2]["offset"] == log))
    assert nxt and k[0] < nxt[0]
    return out


def set_recovery():
    """S-02.084, S-02.103: recovery's image writes, each erasing the target's settings sector after its log
    sector and before the trial mark or the record erase; its actions and the selector's settling, which erase
    nothing of it; the image lengths BEGIN takes."""
    writes = []
    for a in (0, 1):
        b = 1 - a
        for way, t, ops in (("plain", b, plain_ops(a)), ("select_first", a, running_ops(a, "select_first")),
                            ("trial", b, trial_ops(b)), ("in_place", a, running_ops(a, "in_place"))):
            out = set_recovery_ops(ops, t)
            nxt = next(o["cls"] for o in out if o["cls"] in ("trial_log", "record_erase"))
            writes.append({"way": way, "running": a, "target": t, "length": "0x%x" % IMAGE_LEN,
                           "settings_sector": "0x%05x" % SET_SECTOR[t],
                           "settings_erase": {"after": "the log sector's and the logo header sector's erases",
                                              "before": "the trial mark" if nxt == "trial_log"
                                              else "the record erase"},
                           "ops": [{"op": o["op"], "offset": "0x%05x" % o["offset"], "length": o["length"],
                                    "cls": o["cls"]} for o in out]})
    sets = (SET_SECTOR[0], SET_SECTOR[1])
    acts = {}
    for c in act_data()["cases"]:
        hit = any(o["op"] == "erase" and int(o["offset"], 16) in sets for o in c["ops"])
        assert not hit, c
        acts.setdefault(c["act"], 0)
        acts[c["act"]] += 1
    actions = [{"action": n, "cases": acts[n], "erases_settings": False, "source": "recovery_actions.json"}
               for n in ("try", "switch", "keep", "put_back")]
    actions.append({"action": "select", "erases_settings": False,
                    "source": "the selector write alone (S-02.084)"})
    actions.append({"action": "settle", "erases_settings": False,
                    "source": "the slot's own image settles the selector and erases nothing of the slot"})
    lengths = []
    for n in (0, 1, IMAGE_LEN, SET_SLOT_MAX - 1, SET_SLOT_MAX, SET_SLOT_MAX + 1, LOG_SECTOR, LOG_SECTOR + 1,
              SLOT_MAX, SLOT_MAX + 1):
        ok = 1 <= n <= SET_SLOT_MAX
        lengths.append({"length": "0x%x" % n, "begin": "accepted" if ok else "refused", "status": 0 if ok else 6,
                        "erases_settings": ok, "written": "the image" if ok else "nothing"})
    return {"image_max": "0x%x" % SET_SLOT_MAX, "writes": writes, "actions": actions, "lengths": lengths}


def settings_sector():
    st = 0x1D0C2B3A                       # the running image's record CRC in the read cases
    other = 0x5A5AA5A5
    dflt, allon, menu = set_record(SET_DEFAULTS, st), set_record(0x0F, st), set_record(0x0E, st)
    dev = set_record(0x07, st)
    longer = set_record(0, st, payload=b"\x0e\x5a\xa5\x00\xff")
    longest = set_record(0, st, payload=bytes([0x0B]) + bytes((i * 7) & 0xFF for i in range(SET_MAX_PAYLOAD - 1)))
    records = [("defaults", dflt), ("all_on", allon), ("start_in_boot_menu", menu), ("developer_mode", dev),
               ("longer_unknown_bytes", longer), ("longest", longest)]
    # reads: (name, the sector, its stamp) -> flags and the page that gave them
    full = {i: set_record(i & 0x0F, st) for i in range(SET_PAGES)}
    reads = [
        ("erased", set_sector_of({})),
        ("one_record", set_sector_of({0: menu})),
        ("stamp_mismatch", set_sector_of({0: set_record(0x0E, other)})),
        ("newest_of_three", set_sector_of({0: dflt, 1: dev, 2: menu})),
        ("newest_wrong_stamp", set_sector_of({0: dflt, 1: dev, 2: set_record(0x0E, other)})),
        ("full_sector", set_sector_of(full)),
        ("gap_takes_highest", set_sector_of({0: dflt, 2: menu})),
        ("old_boot_store_record", set_sector_of({0: b"WBST\x01\x00\x01\x00\x01" +
                                                 zlib.crc32(b"WBST\x01\x00\x01\x00\x01").to_bytes(4, "little")})),
        ("old_record_then_new", set_sector_of({0: b"WBST\x01\x00\x01\x00\x01" +
                                               zlib.crc32(b"WBST\x01\x00\x01\x00\x01").to_bytes(4, "little"),
                                               1: menu})),
        ("flag_bits_4_to_7", set_sector_of({0: set_record(0xF8 & 0x0F, st, payload=b"\xf8")})),
        ("host_link_off", set_sector_of({0: set_record(0x14, st)})),
        ("host_link_byte_other_bits", set_sector_of({0: set_record(0, st, payload=b"\x0e\xfe")})),
        ("host_link_off_then_on", set_sector_of({0: set_record(0x14, st), 1: dflt})),
        ("longer_unknown_bytes", set_sector_of({0: longer})),
        ("longest", set_sector_of({0: longest})),
        ("wrong_magic", set_sector_of({0: set_record(0x0E, st, magic=b"WBSU")})),
        ("version_0", set_sector_of({0: set_record(0x0E, st, version=0)})),
        ("version_2", set_sector_of({0: set_record(0x0E, st, version=2)})),
        ("length_0", set_sector_of({0: set_record(0, st, payload=b"", length=0)})),
        ("length_241", set_sector_of({0: set_record(0, st, payload=b"\x0e" * 241)[:PAGE]})),
        ("wrong_crc", set_sector_of({0: set_record(0x0E, st, crc=zlib.crc32(menu[:13]) ^ 1)})),
        ("image_bytes", pattern(SECTOR, 9)),
        ("zeros", bytes(SECTOR)),
    ]
    for k in range(len(menu)):                      # a torn program: the first k bytes on page 1, page 0 older
        reads.append(("torn_%d" % k, set_sector_of({0: dflt, 1: menu[:k]})))
    out_reads = []
    for n, sec in reads:
        f, pg = set_pick(sec, st)
        out_reads.append({"name": n, "stamp": "0x%08x" % st, "pages": set_hex_pages(sec), "fill": "ff",
                          "flags": f, "page": pg, "gives": "a record" if pg is not None else "defaults"})
    out_reads.append({"name": "failed_read", "stamp": "0x%08x" % st, "pages": None, "flags": SET_DEFAULTS,
                      "page": None, "gives": "defaults"})
    # the guard: the running slot as this start found it
    def rec_of(ln, tag=TAG, crc=st):
        return crc.to_bytes(4, "little") + ln.to_bytes(4, "little") + tag.to_bytes(4, "little")
    guards = []
    for name, est, good, rec, now in (
            ("usable", True, True, rec_of(0x2345), None),
            ("length_1", True, True, rec_of(1), None),
            ("length_0x76000", True, True, rec_of(0x76000), None),
            ("length_0x76001", True, True, rec_of(0x76001), None),
            ("length_0x77000", True, True, rec_of(0x77000), None),
            ("length_0x78000", True, True, rec_of(0x78000), None),
            ("length_0", True, True, rec_of(0), None),
            ("bad_tag", True, True, rec_of(0x2345, tag=0x0000B005), None),
            ("not_established", False, True, rec_of(0x2345), None),
            ("not_good", True, False, rec_of(0x2345), None),
            ("trial_mark", True, False, rec_of(0x2345, crc=st ^ 0xFFFFFFFF), None),
            ("record_changed", True, True, rec_of(0x2345), rec_of(0x2345, crc=other)),
            ("record_unreadable", True, True, rec_of(0x2345), "unreadable")):
        u = set_usable(dict(established=est, good=good, rec=rec),
                       None if now in (None, "unreadable") else now) and now != "unreadable"
        guards.append({"name": name, "established": est, "good": good, "record": rec.hex(" "),
                       "record_now": None if now is None else ("unreadable" if now == "unreadable" else now.hex(" ")),
                       "usable": u, "read_gives": "the sector" if u else "defaults, sector not read",
                       "save": "written" if u else "refused, nothing written",
                       "clear": "made" if u else "refused, nothing written"})
    # saves, for both slots
    saves = []
    starts = [("from_erased", set_sector_of({})), ("append", set_sector_of({0: dflt})),
              ("append_after_old_stamp", set_sector_of({0: set_record(0x0E, other)})),
              ("append_after_torn", set_sector_of({0: dflt, 1: menu[:7]})),
              ("after_old_boot_store", set_sector_of({0: b"WBST\x01\x00\x01\x00\x01" +
                                                      zlib.crc32(b"WBST\x01\x00\x01\x00\x01").to_bytes(4, "little")})),
              ("same_value", set_sector_of({0: dflt, 1: dev})),
              ("full_wraps", set_sector_of(full)),
              ("not_valid_gap", set_sector_of({0: dflt, 2: menu})),
              ("image_bytes", pattern(SECTOR, 9))]
    for slot in (0, 1):
        ops, after, used = set_save_ops(slot, set_sector_of({0: dflt}), 0x17, st)
        saves.append({"name": "host_link_off", "slot": slot, "stamp": "0x%08x" % st, "flags": 0x17,
                      "before": set_hex_pages(set_sector_of({0: dflt})), "before_pattern": None,
                      "ops": ops, "page": used, "after": set_hex_pages(after),
                      "footprint": ["0x%05x" % SET_SECTOR[slot], "0x%05x" % (SET_SECTOR[slot] + SECTOR)]})
        for name, sec in starts:
            ops, after, used = set_save_ops(slot, sec, 0x07, st)
            saves.append({"name": name, "slot": slot, "stamp": "0x%08x" % st, "flags": 0x07,
                          "before": set_hex_pages(sec) if name != "image_bytes" else None,
                          "before_pattern": 9 if name == "image_bytes" else None,
                          "ops": ops, "page": used, "after": set_hex_pages(after),
                          "footprint": ["0x%05x" % SET_SECTOR[slot], "0x%05x" % (SET_SECTOR[slot] + SECTOR)]})
    clears = []
    for slot in (0, 1):
        for name, sec in (("erased", set_sector_of({})), ("used", set_sector_of({0: dflt, 1: dev}))):
            base = SET_SECTOR[slot]
            ops = [["read", "0x%05x" % RECORD[slot], 12], ["read", "0x%05x" % base, SECTOR]]
            if name == "used":
                ops += [["erase", "0x%05x" % base], ["read", "0x%05x" % base, SECTOR]]
            clears.append({"name": name, "slot": slot, "before": set_hex_pages(sec), "ops": ops,
                           "after": "erased"})
    cuts = [{"op": "program", "at": "before", "reads": "old"},
            {"op": "program", "at": "torn", "reads": "old"},
            {"op": "program", "at": "after", "reads": "new"},
            {"op": "erase", "at": "before", "reads": "old"},
            {"op": "erase", "at": "torn", "reads": "an earlier value or defaults"},
            {"op": "erase", "at": "after", "reads": "defaults"},
            {"op": "clear_erase", "at": "torn", "reads": "an earlier value or defaults"},
            {"op": "clear_erase", "at": "after", "reads": "defaults"}]
    upgrade = []
    for n in (0x1345, SET_SLOT_MAX - 1, SET_SLOT_MAX, SET_SLOT_MAX + 1, LOG_SECTOR, LOG_SECTOR + 1, SLOT_MAX):
        upgrade.append({"length": "0x%x" % n, "erases_start_log": n <= LOG_SECTOR,
                        "erases_settings": n <= SET_SLOT_MAX,
                        "slot_keeps_settings": n <= SET_SLOT_MAX})
    checks = [{"slot": k, "check": c, "range": r} for k in (0, 1) for c, r in (
        ("the slot's CRC and trial mark", ["0x%05x" % SLOT[k], "0x%05x + length" % SLOT[k]]),
        ("the start log", ["0x%05x" % (SLOT[k] + LOG_SECTOR), "0x%05x" % (SLOT[k] + SLOT_MAX)]),
        ("mirror and metadata", ["0xb000", "0xd000"]),
        ("selector and records", ["0xd000", "0x10000"]))]
    return {
        "magic": SET_MAGIC.hex(" "), "old_magic": SET_OLD_MAGIC.hex(" "), "page": PAGE, "pages": SET_PAGES,
        "max_payload": SET_MAX_PAYLOAD, "flag_bits": list(SET_FLAGS), "defaults": SET_DEFAULTS,
        "sectors": [{"slot": k, "start": "0x%05x" % SET_SECTOR[k], "end": "0x%05x" % (SET_SECTOR[k] + SECTOR)}
                    for k in (0, 1)],
        "slot_max_length": "0x%x" % SET_SLOT_MAX,
        "records": [{"name": n, "length": len(r), "bytes": r.hex()} for n, r in records],
        "reads": out_reads, "guards": guards, "saves": saves, "clears": clears, "cuts": cuts,
        "upgrade": upgrade, "recovery": set_recovery(), "checks": checks,
        "torn_check": set_torn_check([st, other, 0, 0xFFFFFFFF])}


def _csv(fields, rows):
    buf = io.StringIO()
    w = csv.DictWriter(buf, fieldnames=fields, lineterminator="\n")
    w.writeheader()
    for r in rows:
        w.writerow(r)
    return buf.getvalue()


# ---------------------------------------------------------------- output

# ---------------------------------------------------------------- recovery's actions (S-02.084)

ACT_NAMES = {1: "try", 2: "switch", 3: "keep", 4: "put_back"}
ACT_KINDS = ["good", "good_nolog", "good_pending", "tried", "started", "badcrc", "empty", "badlen", "erased"]
_FIRST_STAGE_STATE = {"good": "good", "good_nolog": "good", "good_pending": "good", "tried": "badcrc",
                      "started": "badcrc", "badcrc": "badcrc", "empty": "empty", "badlen": "badlen",
                      "erased": "erased", "badtag": "badtag"}


def _act_log0(kind):
    return {"good_pending": 0xFD, "tried": 0xFD, "started": 0xFC}.get(kind, 0xFF)


def _act_has_log(kind):
    """S-02.095: True when a record of this kind has a start log."""
    return kind in ("good", "good_pending", "tried", "started")


def _act_record_fails(kind):
    """S-02.021: True when the first stage's check fails a record of this kind."""
    return kind not in ("empty", "badlen")


def act_decide(act, a, kind, start, force):
    """S-02.084: (status, target, ops) for recovery action `act` with nothing done before; ops are
    ('e'|'p', off, len, value) with value a hex string."""
    b = 1 - a
    sel = {"at_home": (a, 1), "trial_start": (b, 1)}.get(start) or ((0xFF, 0xFF) if b == 1 else (0x00, 0xFE))
    s = boot_rule(sel[0])
    at_home = s == a and sel[1] == 0x01
    trusted = s == b and sel[1] == 0x01
    good = kind in ("good", "good_nolog", "good_pending")
    tried = kind in ("tried", "started")
    log = SLOT[b] + LOG_SECTOR
    ops = []

    def log_clean(k, t):
        if _act_has_log(k) and (_act_log0(k) & 3) == 1:
            ops.append(("e", SLOT[t] + LOG_SECTOR, SECTOR, ""))

    def record_right(t):
        ops.append(("e", RECORD[t], SECTOR, ""))
        ops.append(("p", RECORD[t], 12, "record"))

    def sel_put(t, v):
        ops.append(("e", SELECTOR, SECTOR, ""))
        if v == 0x01:
            ops.append(("p", SELECTOR, 1, "%02x" % t))
            ops.append(("p", SELECTOR + 1, 1, "01"))
        else:
            ops.append(("p", SELECTOR, 2, "%02x %02x" % (t, v)))

    def settle(t):
        if boot_rule(sel[0]) == t and (sel[1] == 0x01 or t == 1):
            return
        if t == 0 and sel[1] & 1:
            if sel[0] != 0:
                ops.append(("p", SELECTOR, 1, "00"))
            if sel[1] != 1:
                ops.append(("p", SELECTOR + 1, 1, "01"))
            return
        sel_put(t, 0x01)

    if act == 1:
        if trusted:
            return 22, b, []
        if not force and not at_home:
            return (3 if s != a else 4), b, []
        if not force and not good and not tried:
            return 23, b, []
        if good or tried:
            if _act_has_log(kind):
                ops += [("e", log, SECTOR, ""), ("p", log, 1, "fd")]
            if good:
                ops += [("e", RECORD[b], SECTOR, ""), ("p", RECORD[b], 12, "trial_mark")]
        elif not _act_record_fails(kind):
            ops.append(("e", RECORD[b], SECTOR, ""))
        ops.append(("e", SELECTOR, SECTOR, ""))
        if b == 0:
            ops += [("p", SELECTOR + 1, 1, "fe"), ("p", SELECTOR, 1, "00")]
        return 0, b, ops
    if act == 2:
        if trusted:
            return 22, b, []
        if not force:
            if not at_home:
                return (3 if s != a else 4), b, []
            if not good and not tried:
                return 23, b, []
            if tried and kind != "started":
                return 21, b, []
        if tried:
            log_clean(kind, b)
            record_right(b)
        elif good:
            log_clean(kind, b)
        sel_put(b, 0x00 if good or tried else 0x01)
        return 0, b, ops
    if act == 3:
        if tried:
            if not force and not at_home:
                return 20, b, []
            if not force and kind != "started":
                return 21, b, []
            log_clean(kind, b)
            record_right(b)
            sel_put(b, 0x00)
            return 0, b, ops
        if start == "trial_start":
            if not force and not (trusted and good):
                return 20, a, []
            ops.append(("p", SLOT[a] + LOG_SECTOR, 1, "fc"))     # A's pending entry marked finished
            record_right(a)
            sel_put(a, 0x00)
            return 0, a, ops
        return 20, b, []
    if not tried:
        return 20, b, []
    if s == b and sel[1] != 0x01:
        settle(a)
    log_clean(kind, b)
    record_right(b)
    return 0, b, ops


def _act_between(old, new):
    """Every 16-bit selector value a program from old to new, cut short, can leave: new <= v <= old bitwise."""
    o = (old[0] << 8) | old[1]
    n = (new[0] << 8) | new[1]
    free = [i for i in range(16) if (o >> i) & 1 and not (n >> i) & 1]
    out = set()
    for m in range(1 << len(free)):
        v = n
        for j, i in enumerate(free):
            if (m >> j) & 1:
                v |= 1 << i
        out.add((v >> 8, v & 0xFF))
    return out


def _act_superset(old):
    """Every value an erase of old, cut short, can leave: every bit at its old value or 1."""
    return _act_between((0xFF, 0xFF), old)


def _act_apply(op, sel, rec, log0, a, b, torn):
    """The selector values and the record states an operation leaves (torn or whole)."""
    kind, off, n, val = op
    sels = {sel}
    if off == SELECTOR or off == SELECTOR + 1:
        if kind == "e":
            sels = _act_superset(sel) if torn else {(0xFF, 0xFF)}
        else:
            new = list(sel)
            vals = [int(x, 16) for x in val.split()]
            for i, v in enumerate(vals):
                new[off - SELECTOR + i] &= v
            new = tuple(new)
            sels = _act_between(sel, new) if torn else {new}
    elif off in RECORD:
        k = RECORD.index(off)
        rec = list(rec)
        if torn:
            rec[k] = "badtag"
        elif kind == "e":
            rec[k] = "erased"
        else:
            rec[k] = "tried" if val == "trial_mark" else "good"
        rec = tuple(rec)
    return sels, rec, log0


def _act_judge(sels, rec, a):
    states = tuple(_FIRST_STAGE_STATE[r] for r in rec)
    fboots, cboots = set(), set()
    trusts_tried = trusts_bad = False
    for v in sels:
        n = boot_rule(v[0])
        if v[1] == 0x01 and rec[n] in ("tried", "started"):
            trusts_tried = True
        if v[1] == 0x01 and states[n] != "good" and rec[n] not in ("tried", "started"):
            trusts_bad = True
        fboots.add(tuple(run_resets("factory", v, states, 2)))
        cboots.add(tuple(run_resets("conforming", v, states, 2)))
    return sorted(fboots, key=str), sorted(cboots, key=str), trusts_tried, trusts_bad


def act_case(act, a, kind, start):
    b = 1 - a
    sel = {"at_home": (a, 1), "trial_start": (b, 1)}.get(start) or ((0xFF, 0xFF) if b == 1 else (0x00, 0xFE))
    rec = [None, None]
    rec[a] = "tried" if start == "trial_start" else "good"
    rec[b] = kind
    rec = tuple(rec)
    st_u, target, _ = act_decide(act, a, kind, start, False)
    st_f, target_f, ops = act_decide(act, a, kind, start, True)
    case = {"act": ACT_NAMES[act], "a": a, "b_kind": kind, "start": start, "selector": hx(sel),
            "status": st_u, "status_forced": st_f, "target": target_f if st_f == 0 else None,
            "ops": [{"op": {"e": "erase", "p": "program"}[o[0]], "offset": "0x%x" % o[1], "length": o[2],
                     "value": o[3]} for o in ops]}
    if st_f != 0:
        return case
    cuts = []
    for i in range(len(ops) + 1):
        for torn in ((False,) if i == len(ops) else (False, True)):
            sels, r, lg = {sel}, rec, None
            for o in ops[:i]:
                nxt = set()
                for v in sels:
                    ns, r2, _ = _act_apply(o, v, r, lg, a, b, False)
                    nxt |= ns
                sels = nxt
                r = r2
            if torn:
                nxt = set()
                for v in sels:
                    ns, r2, _ = _act_apply(ops[i], v, r, lg, a, b, True)
                    nxt |= ns
                sels = nxt
                r = r2
            fb, cb, tt, tb = _act_judge(sels, r, a)
            cuts.append({"after": i, "torn": torn, "records": list(r), "selectors": len(sels),
                         "selector": hx(next(iter(sels))) if len(sels) == 1 else "many",
                         "factory_boots": [list(x) for x in fb], "conforming_boots": [list(x) for x in cb],
                         "trusts_tried": tt, "trusts_bad": tb})
    case["cuts"] = cuts
    return case


def act_data():
    cases = []
    for a in (0, 1):
        for act in (1, 2, 3, 4):
            for kind in ACT_KINDS:
                cases.append(act_case(act, a, kind, "at_home"))
            if act == 3:
                cases.append(act_case(act, a, "good", "trial_start"))
            if act == 4:
                cases.append(act_case(act, a, "tried", "after_try"))
    return {"slot": [hex(x) for x in SLOT], "record": [hex(x) for x in RECORD],
            "selector": hex(SELECTOR), "cases": cases}


def render():
    out = {}
    out["layout.json"] = json.dumps(layout(), indent=1) + "\n"
    out["records.json"] = json.dumps(records(), indent=1) + "\n"
    buf = io.StringIO()
    w = csv.DictWriter(buf, fieldnames=TABLE_FIELDS, lineterminator="\n")
    w.writeheader()
    for r in first_stage_rows():
        w.writerow(r)
    out["first_stage_table.csv"] = buf.getvalue()
    out["first_stage_read_faults.csv"] = _csv(FAULT_FIELDS, read_fault_rows())
    seqs, cuts = powercut_data()
    out["powercut_sequences.json"] = json.dumps(seqs, indent=1) + "\n"
    buf = io.StringIO()
    w = csv.DictWriter(buf, fieldnames=CUT_FIELDS, lineterminator="\n")
    w.writeheader()
    for r in cuts:
        w.writerow(r)
    out["powercut_cuts.csv"] = buf.getvalue()
    out["package_steps.json"] = json.dumps(package_steps(), indent=1) + "\n"
    out["upgrade_cut_boots.csv"] = _csv(NEXT_BOOT_FIELDS, next_boot_rows())
    out["chord_table.csv"] = _csv(CHORD_FIELDS, chord_rows())
    out["recovery_running.json"] = json.dumps(running_data(), indent=1) + "\n"
    out["start_log.csv"] = _csv(LOG_FIELDS, start_log_rows())
    out["reach.csv"] = _csv(REACH_FIELDS, reach_rows())
    mdata, mrows = metadata_data()
    out["metadata_writes.json"] = json.dumps(mdata, indent=1) + "\n"
    out["metadata_cuts.csv"] = _csv(META_CUT_FIELDS, mrows)
    out["bootable_images.json"] = json.dumps(bootable_cases(), indent=1) + "\n"
    out["recovery_actions.json"] = json.dumps(act_data(), indent=1) + "\n"
    out["settings_sector.json"] = json.dumps(settings_sector(), indent=1) + "\n"
    out["slot_kinds.json"] = json.dumps({"cases": slot_kind.cases()}, indent=1) + "\n"
    return out


def main():
    out = render()
    if "--check" in sys.argv:
        bad = [n for n, t in out.items() if open(os.path.join(VEC, n)).read() != t]
        print("differ:" if bad else "all vectors match", " ".join(bad))
        return 1 if bad else 0
    os.makedirs(VEC, exist_ok=True)
    for n, t in out.items():
        with open(os.path.join(VEC, n), "w") as f:
            f.write(t)
        print("wrote", n, len(t))
    return 0


if __name__ == "__main__":
    sys.exit(main())
