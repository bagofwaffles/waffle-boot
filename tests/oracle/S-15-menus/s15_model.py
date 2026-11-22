# SPDX-License-Identifier: GPL-2.0-or-later
"""S-15 model: the digit keys, the boot chord, the settings and the gray formula, the pack names, the
key-test rule, the format and its remount, the hand-over frame, and the required-information match.
Python 3, standard library only.

Required information is matched by content, not by text: `info_match` checks rows against a token of
vectors/info_tokens.json."""
import hashlib
import re

# ---------- S-15.011 digits ----------
DIGITS = {1: (0x06, 0x0A, 0x0E), 2: (0x07, 0x0B, 0x0F), 3: (0x08, 0x0C, 0x10), 4: (0x09, 0x0D, 0x11),
          5: (0x12,), 6: (0x13,), 7: (0x14,), 8: (0x15,)}
KEY_DIGIT = {k: d for d, ks in DIGITS.items() for k in ks}
KEY_HELP, KEY_WHITE, KEY_SHIFT, KEY_COM = 0x00, 0x0D, 0x37, 0x18
NKEYS = 72


def digit(key, dev='xy'):
    return DEV[dev]['key_digit'].get(key, 0)


# ---------- S-15.016 the keys by device ----------
# The OP-XY's key of each role and the OP-1F's (S-18.030's device.key.*); precomp has no OP-1F key.
ROLE_1F = {0x06: 0x06, 0x07: 0x07, 0x08: 0x08, 0x09: 0x09,          # the first key of digits 1-4
           0x0A: 0x36, 0x0B: 0x37, 0x0C: 0x38, 0x0D: 0x39,          # the knob clicks
           0x0E: 0x2D, 0x0F: 0x2E, 0x10: 0x2F, 0x11: 0x10,          # the other key of digits 1-4
           0x12: 0x11, 0x13: 0x12, 0x14: 0x13, 0x15: 0x14,          # digits 5-8
           0x37: 0x26, 0x18: 0x2C}                                  # shift, the com key
# Keys of no role in the OP-XY's vectors, given keys of no role on the OP-1F (none above 0x39 exists there).
OTHER_1F = {0x00: 0x00, 0x40: 0x15, 0x47: 0x16, 0x17: 0x17}


def to_1f(key):
    """S-15.016: the OP-1F's key in the OP-XY key's role."""
    if key in ROLE_1F:
        return ROLE_1F[key]
    return OTHER_1F[key]


DEV = {
    'xy': dict(nkeys=NKEYS, shift=KEY_SHIFT, back=0x15, precomp=0x17, knobs=(0x0A, 0x0B, 0x0C, 0x0D),
               key_digit=KEY_DIGIT),
    '1f': dict(nkeys=58, shift=0x26, back=0x14, precomp=None, knobs=(0x36, 0x37, 0x38, 0x39),
               key_digit={to_1f(k): d for k, d in KEY_DIGIT.items()}),
}


# ---------- S-15.020 first screen ----------
def first_screen(reason):
    return {30: 'upgrade_disk', 40: 'upgrade_midi'}.get(reason, 'main')


def boot_prompt_choice(events, down_at_open=()):
    """S-15.180: events after the screen is shown, as (key, down). Returns 'start', 'held' or None."""
    down = set(down_at_open)
    for key, is_down in events:
        if not is_down:
            down.discard(key)
        elif key in down:
            continue
        else:
            down.add(key)
            d = digit(key)
            if d == 1:
                return 'start'
            if d == 2:
                return 'held'
    return None


# ---------- S-15.116 the selection of the steps before a run ----------
HOLD_MS = 5000
SELECT_TIMEOUT_MS = 60000
KNOB_CLICKS = (0x0A, 0x0B, 0x0C, 0x0D)
KEY_CANCEL = 0x15
KEY_PRECOMP = 0x17                              # S-07.032, S-07.033: ignored
# The rows' digits and their steps (S-11.057): 2 Main DSP, 3 Content, 4 BLE, 5 BLE bootloader, 6 Backplane,
# 7 Bootloader
ROW_STEPS = {2: 1, 3: 0, 4: 3, 5: 4, 6: 5, 7: 2}


def select_choice(events, pickable, down_at_open=(), end_ms=None, dev='xy', prompt=False):
    """S-15.116: events after the selection is shown at time 0 with every row at No, as (ms, key, down) in time
    order; pickable: the steps whose rows can be picked. Returns (choice, ms, selected): 'install' or 'cancel'
    and when it was made, with the steps at Yes then, or (None, None, selected) while it goes on waiting at
    end_ms (the last event's time when None). Every key event restarts the 60 s, which never end a hold.
    prompt: S-15.197's prompt, the same keys with no row, so the hold needs no row at Yes (sig_prompt_choice)."""
    P = DEV[dev]
    stale, hold = set(down_at_open), None        # hold: (key, the time of its key down)
    held = set(down_at_open)                      # every key down now (S-07.031)
    selected, last = set(), 0

    def due(t):
        """The choice the time makes by t, if any."""
        if hold is not None:
            return ('install', hold[1] + HOLD_MS) if hold[1] + HOLD_MS <= t else None
        if t >= last + SELECT_TIMEOUT_MS:
            return 'cancel', last + SELECT_TIMEOUT_MS
        return None
    for t, key, down in events:
        if key >= P['nkeys']:                     # S-15.014: no such key; nothing acts
            continue
        d = due(t)
        if d:
            return d + (sorted(selected),)
        last = t
        if down:
            held.add(key)
        else:
            held.discard(key)
        if key in stale:
            if not down:
                stale.discard(key)
            continue
        if not down:
            if hold is not None and hold[0] == key:
                hold = None
            continue
        if hold is not None:                      # any other key down only ends the hold (S-07.033)
            if key != P['precomp']:
                hold = None
            continue
        if digit(key, dev) == 8:
            return 'cancel', t, sorted(selected)
        step = ROW_STEPS.get(digit(key, dev))
        if step is not None:
            if step in pickable:
                selected ^= {step}
        elif (digit(key, dev) == 1 and key not in P['knobs'] and (selected or prompt)
              and held - {key, P['precomp']} == set()):
            hold = (key, t)
    end = end_ms if end_ms is not None else (events[-1][0] if events else 0)
    d = due(end)
    return (d + (sorted(selected),)) if d else (None, None, sorted(selected))


# ---------- S-15.197 the signature prompt before an install ----------
SIG_STATES = ('valid', 'invalid', 'none')         # S-11.021


def sig_prompt_shown(state):
    """S-15.197: only an invalid package shows the prompt; a valid or none one goes straight to the
    selection."""
    return state == 'invalid'


def sig_valid_shown(state):
    """S-15.197: a valid package is shown as TE's (`sig:valid`) on the selection and in the outcome; a none
    package says nothing of a signature."""
    return state == 'valid'


def sig_prompt_choice(events, down_at_open=(), end_ms=None, dev='xy'):
    """S-15.197: the prompt shown at time 0. Key 1 held 5 s (the hold, S-15.015, with S-15.116's no-other-key
    rule) goes on to the selection ('install'); digit 8 cancels, and so do 60 s without a key event, as on the
    selection. Returns (choice, ms): choice None while it waits at end_ms."""
    choice, at, _ = select_choice(events, (), down_at_open, end_ms, dev, prompt=True)
    return choice, at


# ---------- S-15.172 the settings ----------
# Kept in the running slot's settings sector (S-02.096); no file on the eMMC is read or written for them.
# (key, default), in the order of the record's flag bits 0 to 3
SETTINGS_KEYS = (('developer_mode', False), ('color', False), ('mtp_target', True), ('start_in_boot_menu', False))
SETTINGS_DEFAULTS = {k: d for k, d in SETTINGS_KEYS}


def settings_flags(settings):
    """S-15.172, S-02.096: the record's flag byte for these settings (mtp_target True: macOS)."""
    return sum(1 << i for i, (k, _) in enumerate(SETTINGS_KEYS) if settings[k])


def settings_of_flags(f):
    """The settings a record's flag byte gives; bits 4 to 7 ignored."""
    return {k: bool(f >> i & 1) for i, (k, _) in enumerate(SETTINGS_KEYS)}


# ---------- S-15.173 the color setting ----------
def gray(c):
    """S-15.173: the RGB565 gray a color is drawn in while the color is off."""
    r, g, b = c >> 11, (c >> 5) & 0x3F, c & 0x1F
    if r == b == g >> 1:
        return c
    y = (77 * (8 * r + r // 4) + 150 * (4 * g + g // 16) + 29 * (8 * b + b // 4) + 128) // 256
    return (y // 8) * 2048 + (y // 4) * 32 + y // 8


def is_gray(c):
    return c >> 11 == c & 0x1F == ((c >> 5) & 0x3F) >> 1


# ---------- S-15.071 - S-15.073 the format ----------
def volume_refused(mount_result):
    """A refused volume: its last mount failed (on the master, a negative 0x08 result, S-09.041), whatever
    the cause."""
    r = mount_result & 0xFFFFFFFF
    return r >= 0x80000000


# The lines of format_runs.json: waffle-boot's wording, and the required information of a failure.
FORMAT_RUN = ['Formatting...', 'ok:Done.']
FORMAT_FAILED_LINE = 'error:{INFO:the drive could not be formatted}'
VOLUME_BLOCKS = 0x1200                           # S-12.042: blocks 0 to 0x11FF, one erase group each


def format_run(select_fails=False, failing=(), n_blocks=VOLUME_BLOCKS):
    """S-12.057, S-15.072: (blocks erased in order, lines appended, next screen). The erasing goes on past a
    failed block; a failed select erases nothing; a failed format keeps the format menu current."""
    if select_fails:
        return [], [FORMAT_RUN[0], FORMAT_FAILED_LINE], 'format'
    blocks = list(range(n_blocks))
    failing = set(range(n_blocks)) if failing == 'all' else set(failing)
    if failing & set(blocks):
        return blocks, [FORMAT_RUN[0], FORMAT_FAILED_LINE], 'format'
    return blocks, list(FORMAT_RUN), 'upgrade'


def format_remount(refused_before, mount_result):
    """S-15.072: after every format, failed or not, the volume is mounted again (one 0x08 on the master);
    (mounts made, refused afterwards). The refused state before the format plays no part."""
    return 1, volume_refused(mount_result)




# ---------- S-15.041 manifest names ----------
def manifest_name(data):
    """data: the file's bytes, or None when missing."""
    if not data:
        return ''
    text = data[:511].decode('latin-1')
    i = text.find('"name"')
    if i < 0:
        return ''
    i += 6
    while i < len(text) and text[i] in ' \t\r\n:':
        i += 1
    if i >= len(text) or text[i] != '"':
        return ''
    out = []
    i += 1
    while i < len(text) and text[i] != '"' and len(out) < 63:
        out.append(text[i])
        i += 1
    return ''.join(out)


def pack_label(name, dirname):
    """S-15.041: the name, else the directory name; 63 characters, '?' for one the screen does not show."""
    return ''.join(c if 0x20 <= ord(c) <= 0x7E else '?' for c in (name if name else dirname)[:63])


# ---------- required information (vectors/info_tokens.json) ----------
def _word(alt, values):
    for k, v in values.items():
        alt = alt.replace('$' + k, str(v))
    prefix = alt.endswith('*')
    alt = alt.rstrip('*').lower()
    return re.compile(r'(?<![a-z0-9])' + re.escape(alt) + ('' if prefix else r'(?![a-z0-9])'))


def info_match(spec, rows, values=None):
    """True when some run of at most spec['rows'] consecutive rows holds, for every group, one of its
    alternatives as a word (an alternative ending in '*' is a word prefix), and none of spec['not'].
    Matching ignores case; '$name' in an alternative is replaced by values[name]. With spec['follows'], the
    run must start on the row right after a row equal to that exact text."""
    values = values or {}
    groups = [[_word(a, values) for a in g] for g in spec['groups']]
    nots = [_word(a, values) for a in spec.get('not', [])]
    follows = spec.get('follows')
    exact = list(rows)
    rows = [r.lower() for r in rows]
    n = spec.get('rows', 1)
    for i in range(len(rows)):
        if follows is not None and (i == 0 or exact[i - 1] != follows):
            continue
        for j in range(i + 1, min(len(rows), i + n) + 1):
            run = rows[i:j]
            if any(w.search(r) for w in nots for r in run):
                continue
            if all(any(w.search(r) for w in g for r in run) for g in groups):
                return True
    return False


# ---------- S-15.064, S-15.065 key test ----------
def keytest(events, dev='xy'):
    """events: list of (key, down). Returns the tested set after each event, and the end flag. On the OP-1F
    (S-15.016) 58 keys, and shift 0x26 with digit 8's 0x14 end it."""
    P = DEV[dev]
    tested, down, lone = set(), set(), None
    out = []
    for key, is_down in events:
        if key >= P['nkeys']:
            pass
        elif is_down:
            lone = key if not down else None
            down.add(key)
        else:
            down.discard(key)
            if key == lone:
                tested.add(key)
        end = len(tested) == P['nkeys'] or (P['shift'] in down and P['back'] in down)
        out.append((sorted(tested), end))
    return out


# ---------- S-15.052 the factory reset by device ----------
RESET_REMOVES = {'xy': ('user', 'logs', 'content/dlc'), '1f': ('user', 'logs')}
FAT_ERASE = {'xy': True, '1f': False}


def factory_reset(tree, dev='xy', refused=False):
    """S-15.052 with device.reset_removes (S-18.030): tree {path under /yaffs2: bytes}. Returns (tree after,
    the operations in order: ('remove', path), ('erase_os_fat',), ('clear_settings',))."""
    ops, t = [], dict(tree)
    if not refused:
        for d in RESET_REMOVES[dev]:
            ops.append(('remove', '/yaffs2/' + d))
            t = {p: b for p, b in t.items() if p != d and not p.startswith(d + '/')}
    if FAT_ERASE[dev]:
        ops.append(('erase_os_fat',))
    ops.append(('clear_settings',))
    return t, ops


# ---------- S-15.196 one MTP item, chosen by shift ----------
def mtp_item(events, dev='1f'):
    """events: (key, down) up to and including digit 2's key down on the developer menu, then any later
    events. Returns the interface the item starts: 'MTP vendor' when shift was held at that key down
    (S-15.012's state), else 'MTP'; later events change nothing."""
    P = DEV[dev]
    shift, chosen = False, None
    for key, down in events:
        if key == P['shift']:
            shift = down
        elif down and chosen is None and digit(key, dev) == 2:
            chosen = 'MTP vendor' if shift else 'MTP'
    return chosen


# ---------- S-15.132 hand-over frame ----------
LOGO_WHITE, LOGO_FAINT = 0xFFFF, 0x5ACB


def logo_pixel(x, y):
    """Screen position (x, y) of the step row."""
    if not 104 <= y <= 117:
        return 0x0000
    for i in range(16):
        left = 59 + 22 * i + 6 * (i // 4)
        if left <= x < left + 14:
            if i % 4 == 0:
                return LOGO_WHITE
            dx, dy = x - left, y - 104
            return LOGO_FAINT if min(dx, dy, 13 - dx, 13 - dy) < 2 else 0x0000
    return 0x0000


def handover_pixel(c, r):
    """Window column c (0..223), row r (0..479), the logo alone (S-13c.023: x = 479 - r, y = c)."""
    x, y = 479 - r, c
    if y > 221:
        return 0x0000
    return logo_pixel(x, y)


def handover_frame():
    b = bytearray()
    for r in range(480):
        for c in range(224):
            p = handover_pixel(c, r)
            b += bytes((p >> 8, p & 0xFF))
    return bytes(b)


def handover_vector():
    f = handover_frame()
    px = [f[i] << 8 | f[i + 1] for i in range(0, len(f), 2)]
    return {'bytes': len(f), 'sha256': hashlib.sha256(f).hexdigest(),
            'white_pixels': px.count(LOGO_WHITE), 'faint_pixels': px.count(LOGO_FAINT)}


# ---------- S-15.150 the chord key ----------
CHORD_KEYS = {0x09: 'recovery'}


def chord(held):
    """held: set of key numbers reported held at start-up, or None when unreadable."""
    if held is None or len(held) != 1:
        return 'none'
    return CHORD_KEYS.get(next(iter(held)), 'none')


# ---------- S-15.003 declared font ----------
BODY_ROWS = 9        # the body rows check_font lays out (waffle-boot's console)


def check_font(font, extent=None):
    """The S-15.003 declared-font check: every character 0x20..0x7E present, one glyph height, glyphs pairwise
    distinct (the space is the only blank glyph); the 9 body rows (from `top`, `pitch` apart), the title
    bar's text row (`title_row`) and the status bar's text row (`status_row`) do not overlap and, given the
    visible extent across the rows, lie within it (S-15.002)."""
    chars = [chr(c) for c in range(0x20, 0x7F)]
    if any(c not in font['glyphs'] for c in chars):
        return 'incomplete'
    h = font['height']
    seen = {}
    for c in chars:
        g = font['glyphs'][c]
        if len(g) != h:
            return 'height'
        key = tuple(g)
        if key in seen:
            return 'duplicate %r %r' % (seen[key], c)
        seen[key] = c
    if any(font['glyphs'][' ']):
        return 'space not blank'
    rows = [(font['top'] + i * font['pitch'], font['top'] + i * font['pitch'] + h) for i in range(BODY_ROWS)]
    bars = [(font[k], font[k] + h) for k in ('title_row', 'status_row')]
    if font['pitch'] < h:
        return 'rows overlap'
    if any(a < y1 and y0 < b for a, b in rows for y0, y1 in bars) or bars[0][1] > bars[1][0]:
        return 'bar overlaps'
    if extent is not None and (min(rows[0][0], bars[0][0]) < 0 or max(rows[-1][1], bars[1][1]) > extent):
        return 'does not fit'
    return 'ok'
