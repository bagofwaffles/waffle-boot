# SPDX-License-Identifier: GPL-2.0-or-later
"""A sparse, sector-addressed disk image (a map of written units over a fill value) and helpers to compare
two images. Readers in this package accept a `SparseDisk`, `bytes`/`bytearray`, or a path to an image file.
"""
import os


class SparseDisk:
    """`size` bytes, all `fill` except the units that were written. Unit = `unit` bytes (a sector)."""

    def __init__(self, size, unit=512, fill=0):
        if size % unit:
            raise ValueError('size must be a multiple of the unit')
        self.size, self.unit, self.fill = size, unit, fill
        self._blank = bytes([fill]) * unit
        self.units = {}

    def write(self, offset, data):
        data = bytes(data)
        if offset < 0 or offset + len(data) > self.size:
            raise ValueError(f'write {offset:#x}+{len(data):#x} outside the disk ({self.size:#x})')
        u = self.unit
        pos = 0
        while pos < len(data):
            n, o = divmod(offset + pos, u)
            take = min(u - o, len(data) - pos)
            cur = self.units.get(n)
            if cur is None:
                if o == 0 and take == u:
                    self.units[n] = data[pos:pos + u]
                    pos += take
                    continue
                cur = self._blank
            b = bytearray(cur)
            b[o:o + take] = data[pos:pos + take]
            self.units[n] = bytes(b)
            pos += take

    def read(self, offset, length):
        u = self.unit
        out = bytearray()
        pos = offset
        end = offset + length
        if offset < 0 or end > self.size:
            raise ValueError(f'read {offset:#x}+{length:#x} outside the disk ({self.size:#x})')
        while pos < end:
            n, o = divmod(pos, u)
            take = min(u - o, end - pos)
            out += self.units.get(n, self._blank)[o:o + take]
            pos += take
        return bytes(out)

    def to_bytes(self):
        return self.read(0, self.size)

    def save(self, path):
        """Writes the image; unwritten units become holes where the file system supports them
        (only valid when fill is 0)."""
        with open(path, 'wb') as f:
            if self.fill == 0:
                f.truncate(self.size)
                for n in sorted(self.units):
                    f.seek(n * self.unit)
                    f.write(self.units[n])
            else:
                f.write(self.to_bytes())

    def written_units(self):
        return sorted(self.units)


class _BytesSource:
    def __init__(self, data):
        self.data = data
        self.size = len(data)

    def read(self, offset, length):
        if offset < 0 or offset + length > self.size:
            raise ValueError(f'read {offset:#x}+{length:#x} outside the image ({self.size:#x})')
        return bytes(self.data[offset:offset + length])


class _FileSource:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.size = os.path.getsize(path)

    def read(self, offset, length):
        if offset < 0 or offset + length > self.size:
            raise ValueError(f'read {offset:#x}+{length:#x} outside the image ({self.size:#x})')
        self.f.seek(offset)
        return self.f.read(length)


def source(img):
    """Anything with .read(offset, length) and .size: a SparseDisk, bytes, or a file path."""
    if isinstance(img, (bytes, bytearray, memoryview)):
        return _BytesSource(bytes(img))
    if isinstance(img, (str, os.PathLike)):
        return _FileSource(img)
    if hasattr(img, 'read') and hasattr(img, 'size'):
        return img
    raise TypeError(f'not an image: {type(img).__name__}')


def diff_units(a, b, unit=512, limit=10):
    """Compares two images unit by unit. Returns (number of differing units, [first `limit` unit
    numbers]). Images of different sizes differ in every unit past the shorter one."""
    sa, sb = source(a), source(b)
    n = max(sa.size, sb.size)
    count, first = 0, []
    sparse = [s for s in (sa, sb) if isinstance(s, SparseDisk)]
    if len(sparse) == 2 and sa.size == sb.size and sa.unit == sb.unit == unit and sa.fill == sb.fill:
        cand = sorted(set(sa.units) | set(sb.units))       # only written units can differ
    else:
        cand = range((n + unit - 1) // unit)
    for k in cand:
        o = k * unit
        ra = sa.read(o, min(unit, sa.size - o)) if o < sa.size else None
        rb = sb.read(o, min(unit, sb.size - o)) if o < sb.size else None
        if ra != rb:
            count += 1
            if len(first) < limit:
                first.append(k)
    return count, first


def assert_unmodified(before, after, unit=512):
    """Raises AssertionError naming the first differing units if `after` differs from `before`."""
    count, first = diff_units(before, after, unit)
    if count:
        raise AssertionError(f'volume modified: {count} units of {unit} bytes differ, first {first}')
