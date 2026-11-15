# SPDX-License-Identifier: GPL-2.0-or-later
"""The Cypress (Infineon) PSoC bootloader host protocol (application note AN68272), and a scripted
model of a PSoC in that bootloader.

Packets (both directions)::

    SOP 0x01 | cmd or status (1) | data length (2, LE) | data | checksum (2, LE) | EOP 0x17

The checksum covers SOP .. the last data byte. ``'sum'``: the two's complement of the 16-bit byte sum.
``'crc'``: CRC-16/X-25 (reflected 0x8408, init 0xFFFF, complemented) with its bytes swapped. A
``.cyacd`` (format v0) header line names which one is used.

The model answers on a raw byte stream (``feed(bytes) -> bytes``) and can be a S06Responder's
``raw_handler``. ``hook(cmd, data)`` may return None (answer normally), ``NO_REPLY``, a status code, or
bytes sent as they are.
"""
SOP, EOP = 0x01, 0x17

CMD = {'verify_checksum': 0x31, 'get_flash_size': 0x32, 'get_app_status': 0x33, 'erase_row': 0x34,
       'sync': 0x35, 'set_active_app': 0x36, 'send_data': 0x37, 'enter': 0x38, 'program_row': 0x39,
       'verify_row': 0x3A, 'exit': 0x3B}
CMD_NAME = {v: k for k, v in CMD.items()}
NO_RESPONSE = {CMD['sync'], CMD['exit']}

SUCCESS, ERR_LENGTH, ERR_DATA, ERR_CMD = 0x00, 0x03, 0x04, 0x05
ERR_DEVICE, ERR_VERSION, ERR_CHECKSUM, ERR_ARRAY, ERR_ROW = 0x06, 0x07, 0x08, 0x09, 0x0A
ERR_PROTECT, ERR_APP, ERR_ACTIVE, ERR_UNK = 0x0B, 0x0C, 0x0D, 0x0F

NO_REPLY = object()


def checksum(data, kind='sum'):
    data = bytes(data)
    if kind == 'sum':
        return (-sum(data)) & 0xFFFF
    if kind == 'crc':
        crc = 0xFFFF
        for b in data:
            for _ in range(8):
                crc = (crc >> 1) ^ 0x8408 if (crc ^ b) & 1 else crc >> 1
                b >>= 1
        crc = ~crc & 0xFFFF
        return ((crc << 8) | (crc >> 8)) & 0xFFFF
    raise ValueError(f'checksum kind {kind!r}')


def packet(code, data=b'', kind='sum'):
    """One packet: a command (host to bootloader) or a response (status first)."""
    data = bytes(data)
    body = bytes([SOP, code]) + len(data).to_bytes(2, 'little') + data
    return body + checksum(body, kind).to_bytes(2, 'little') + bytes([EOP])


command = response = packet


class PacketError(ValueError):
    def __init__(self, reason):
        super().__init__(reason)
        self.reason = reason        # 'sop', 'eop', 'checksum', 'short'


def parse(buf, kind='sum'):
    """(code, data) of the packet at the start of ``buf``. Raises PacketError."""
    buf = bytes(buf)
    if len(buf) < 7:
        raise PacketError('short')
    if buf[0] != SOP:
        raise PacketError('sop')
    n = int.from_bytes(buf[2:4], 'little')
    if len(buf) < n + 7:
        raise PacketError('short')
    if buf[n + 6] != EOP:
        raise PacketError('eop')
    if int.from_bytes(buf[n + 4:n + 6], 'little') != checksum(buf[:n + 4], kind):
        raise PacketError('checksum')
    return buf[1], buf[4:4 + n]


def split_packets(stream):
    """([packet bytes], rest): whole packets at SOP positions (bytes before an SOP are skipped)."""
    stream = bytes(stream)
    out = []
    while True:
        i = stream.find(bytes([SOP]))
        if i < 0:
            return out, b''
        stream = stream[i:]
        if len(stream) < 4:
            return out, stream
        n = int.from_bytes(stream[2:4], 'little')
        if len(stream) < n + 7:
            return out, stream
        out.append(stream[:n + 7])
        stream = stream[n + 7:]


def row_checksum(row):
    """The 8-bit row check a verify-row reply carries: the two's complement of the row's byte sum."""
    return (-sum(bytes(row))) & 0xFF


class Bootloader:
    """A PSoC in its bootloader, at packet level.

    ``arrays`` maps a flash array id to (first row, last row) as get-flash-size reports them; ``row_size``
    is the row length program-row must deliver (None: any). State a test reads: ``rows`` {(array, row):
    bytes}, ``commands`` [(cmd, data)] in order, ``entered``, ``exited``, ``active_app``."""

    def __init__(self, silicon_id=0x2E123069, silicon_rev=0x00, version=(1, 30, 1), arrays=None,
                 row_size=128, kind='sum', app_valid=True, require_enter=False, hook=None):
        self.silicon_id = silicon_id
        self.silicon_rev = silicon_rev
        self.version = bytes(version)
        self.arrays = dict(arrays if arrays is not None else {0: (0x0000, 0x00FF)})
        self.row_size = row_size
        self.kind = kind
        self.app_valid = app_valid
        self.require_enter = require_enter
        self.hook = hook
        self.rows = {}
        self.commands = []
        self.entered = False
        self.exited = False
        self.active_app = 0
        self._data = bytearray()
        self._buf = bytearray()

    def feed(self, data):
        self._buf += data
        out = bytearray()
        pkts, rest = split_packets(bytes(self._buf))
        self._buf = bytearray(rest)
        for p in pkts:
            out += self._packet(p)
        return bytes(out)

    __call__ = feed

    def _reply(self, status, data=b''):
        return packet(status, data, self.kind)

    def _packet(self, p):
        try:
            cmd, data = parse(p, self.kind)
        except PacketError as e:
            return self._reply(ERR_CHECKSUM if e.reason == 'checksum' else ERR_DATA)
        self.commands.append((cmd, data))
        if self.hook is not None:
            h = self.hook(cmd, data)
            if h is NO_REPLY:
                return b''
            if isinstance(h, (bytes, bytearray)):
                return bytes(h)
            if isinstance(h, int):
                return self._reply(h)
        if self.require_enter and not self.entered and cmd != CMD['enter']:
            return self._reply(ERR_ACTIVE)
        return self._command(cmd, data)

    def _row(self, data):
        if len(data) < 3:
            return None, ERR_LENGTH
        arr, row = data[0], int.from_bytes(data[1:3], 'little')
        if arr not in self.arrays:
            return None, ERR_ARRAY
        lo, hi = self.arrays[arr]
        if not lo <= row <= hi:
            return None, ERR_ROW
        return (arr, row), SUCCESS

    def _command(self, cmd, data):
        r = self._reply
        if cmd == CMD['enter']:
            self.entered, self.exited = True, False
            self._data.clear()
            return r(SUCCESS, self.silicon_id.to_bytes(4, 'little') + bytes([self.silicon_rev]) + self.version)
        if cmd == CMD['exit']:
            self.exited, self.entered = True, False
            return b''
        if cmd == CMD['sync']:
            self._data.clear()
            return b''
        if cmd == CMD['get_flash_size']:
            if len(data) != 1:
                return r(ERR_LENGTH)
            if data[0] not in self.arrays:
                return r(ERR_ARRAY)
            lo, hi = self.arrays[data[0]]
            return r(SUCCESS, lo.to_bytes(2, 'little') + hi.to_bytes(2, 'little'))
        if cmd == CMD['send_data']:
            self._data += data
            return r(SUCCESS)
        if cmd in (CMD['program_row'], CMD['erase_row'], CMD['verify_row']):
            key, st = self._row(data)
            if key is None:
                return r(st)
            if cmd == CMD['program_row']:
                row = bytes(self._data) + data[3:]
                self._data.clear()
                if self.row_size is not None and len(row) != self.row_size:
                    return r(ERR_LENGTH)
                self.rows[key] = row
                return r(SUCCESS)
            if cmd == CMD['erase_row']:
                self.rows[key] = bytes(self.row_size or 0)
                return r(SUCCESS)
            return r(SUCCESS, bytes([row_checksum(self.rows.get(key, bytes(self.row_size or 0)))]))
        if cmd == CMD['verify_checksum']:
            return r(SUCCESS, bytes([1 if self.app_valid else 0]))
        if cmd == CMD['get_app_status']:
            if len(data) != 1:
                return r(ERR_LENGTH)
            return r(SUCCESS, bytes([1 if self.app_valid else 0, 1 if data[0] == self.active_app else 0]))
        if cmd == CMD['set_active_app']:
            if len(data) != 1:
                return r(ERR_LENGTH)
            self.active_app = data[0]
            return r(SUCCESS)
        return r(ERR_CMD)


# ---- .cyacd (format v0) ----

def build_cyacd(silicon_id, silicon_rev, rows, checksum_type=0):
    """The text of a .cyacd file: header ``IIIIIIIIRRCC`` (silicon id, revision, checksum type, hex), then
    one ``:AARRRRLLLLDD..CC`` line per (array, row, data): big-endian fields, line check = two's
    complement of the byte sum of the line."""
    lines = [f'{silicon_id:08X}{silicon_rev:02X}{checksum_type:02X}']
    for arr, row, data in rows:
        b = bytes([arr]) + row.to_bytes(2, 'big') + len(data).to_bytes(2, 'big') + bytes(data)
        lines.append(':' + (b + bytes([(-sum(b)) & 0xFF])).hex().upper())
    return '\r\n'.join(lines) + '\r\n'


def parse_cyacd(text):
    """{'silicon_id', 'silicon_rev', 'checksum_type', 'rows': [(array, row, data, line_check)]}.
    Raises ValueError on a malformed line or a wrong line check."""
    lines = [l.strip() for l in text.splitlines() if l.strip()]
    h = bytes.fromhex(lines[0])
    if len(h) != 6:
        raise ValueError('header')
    rows = []
    for l in lines[1:]:
        if not l.startswith(':'):
            raise ValueError(f'line {l[:16]!r}')
        b = bytes.fromhex(l[1:])
        n = int.from_bytes(b[3:5], 'big')
        if len(b) != n + 6 or sum(b) & 0xFF:
            raise ValueError(f'line {l[:16]!r}: length or check')
        rows.append((b[0], int.from_bytes(b[1:3], 'big'), b[5:5 + n], b[-1]))
    return {'silicon_id': int.from_bytes(h[:4], 'big'), 'silicon_rev': h[4], 'checksum_type': h[5],
            'rows': rows}


def verify_row_expected(array, row, data, line_check):
    """What a host compares a verify-row reply with, from a .cyacd line: the line check plus the line's
    header bytes, which leaves the row data's own check (``row_checksum``)."""
    n = len(data)
    return (line_check + array + row + (row >> 8) + n + (n >> 8)) & 0xFF
