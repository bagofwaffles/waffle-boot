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
KEY_HELP, KEY_WHITE, KEY_SHIFT, KEY_USB = 0x00, 0x0D, 0x37, 0x18
NKEYS = 72


def digit(key):
    return KEY_DIGIT.get(key, 0)


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


# ---------- S-15.116 the confirmation of a protected component ----------
CONFIRM_HOLD_MS = 5000
CONFIRM_TIMEOUT_MS = 60000
KNOB_CLICKS = (0x0A, 0x0B, 0x0C, 0x0D)
KEY_SKIP = 0x15
KEY_PRECOMP = 0x17                              # S-07.032, S-07.033: ignored


def confirm_choice(events, down_at_open=(), readable=True, end_ms=None):
    """S-15.116: events after the confirmation is shown at time 0, as (ms, key, down) in time order. Returns
    (choice, ms): 'install' or 'skip' and when it was made, or (None, None) while it goes on waiting at end_ms
    (the last event's time when None)."""
    if not readable:
        return 'skip', 0
    stale, hold = set(down_at_open), None        # hold: (key, the time of its key down)
    held = set(down_at_open)                      # every key down now (S-07.031)

    def due(t):
        """The choice the time makes by t, if any."""
        if hold is not None and hold[1] + CONFIRM_HOLD_MS <= CONFIRM_TIMEOUT_MS and hold[1] + CONFIRM_HOLD_MS <= t:
            return 'install', hold[1] + CONFIRM_HOLD_MS
        if t >= CONFIRM_TIMEOUT_MS:
            return 'skip', CONFIRM_TIMEOUT_MS
        return None
    for t, key, down in events:
        d = due(t)
        if d:
            return d
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
        if digit(key) == 8:
            return 'skip', t
        if hold is not None:                      # any other key down ends the hold (S-07.033)
            if key != KEY_PRECOMP:
                hold = None
            continue
        if digit(key) == 1 and key not in KNOB_CLICKS and held - {key, KEY_PRECOMP} == set():
            hold = (key, t)
    end = end_ms if end_ms is not None else (events[-1][0] if events else 0)
    return due(end) or (None, None)


# ---------- S-15.172 the settings ----------
# Kept in the running slot's settings sector (S-02.096); no file on the eMMC is read or written for them.
# (key, default), in the order of the record's flag bits 0 to 3
SETTINGS_KEYS = (('developer_mode', False), ('color', True), ('mtp_target', True), ('start_in_boot_menu', False))
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
FORMAT_RUN = ['Formatting the drive. Please wait...', 'ok:Done.']
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
def keytest(events):
    """events: list of (key, down). Returns the tested set after each event, and the end flag."""
    tested, down, lone = set(), set(), None
    out = []
    for key, is_down in events:
        if is_down:
            lone = key if not down else None
            down.add(key)
        else:
            down.discard(key)
            if key == lone:
                tested.add(key)
        end = len(tested) == NKEYS or (KEY_SHIFT in down and 0x15 in down)
        out.append((sorted(tested), end))
    return out


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
