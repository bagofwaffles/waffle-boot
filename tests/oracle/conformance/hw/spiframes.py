# SPDX-License-Identifier: GPL-2.0-or-later
"""SPI frame decoder over an MMR trace (format: mmrtrace.py).

A BF70x SPI controller (ADSP-BF70x HRM, SPI chapter) driven by software chip select is seen in an MMR trace
as writes to SPI_SLVSEL (chip select), writes to SPI_TFIFO (one transmitted word each) and reads of
SPI_RFIFO (one received word each). The decoder turns that into one frame per chip-select assertion, the
flash-trace format of buses.py: {"mosi": "hex", "miso": "hex", "ssel": n}.

Register offsets from the controller base (HRM register map): CTL 0x04, SLVSEL 0x18, RFIFO 0x50,
TFIFO 0x58. SPI_SLVSEL bit n (n = 1..7) is SSEn, the enable of select n; bit n+8 is SSELn, its level. Select n
is asserted while SSEn = 1 and SSELn = 0. SPI_CTL.SIZE (bits 10:9: 0, 1, 2 for 8, 16, 32 bits) sets the bytes
per FIFO word; a word goes into the frame most significant byte first, unless SPI_CTL.LSBF (bit 12) is set.

Not decoded: hardware slave select (SPI_CTL.ASSEL), DMA-fed transfers (no FIFO accesses in the trace) and
memory-mapped mode.

FIFO semantics: words written to TFIFO while no select is asserted wait in the FIFO and go out at the start
of the next frame. An RFIFO read after the select is released belongs to the frame that just ended.
`stray` counts TX words still waiting at the end of the trace and RX reads before any frame.
"""

CTL, SLVSEL, RFIFO, TFIFO = 0x04, 0x18, 0x50, 0x58


class SpiFrameDecoder:
    def __init__(self, base, ssel=None):
        """base: the controller's MMR base address. ssel: decode only select n (1..7); None: any select."""
        self.base = base
        self.ssel = ssel
        self.frames = []
        self.stray = 0
        self._ctl = 0
        self._slvsel = 0
        self._cur = None          # (ssel, mosi bytearray, miso bytearray)
        self._pending = []        # TX words written with no select asserted

    def _active(self):
        sels = [self.ssel] if self.ssel else range(1, 8)
        for n in sels:
            if self._slvsel >> n & 1 and not self._slvsel >> (n + 8) & 1:
                return n
        return None

    def _word(self, v):
        size = {0: 1, 1: 2, 2: 4}.get((self._ctl >> 9) & 3, 4)
        b = (v & ((1 << 8 * size) - 1)).to_bytes(size, 'big')
        return b[::-1] if self._ctl >> 12 & 1 else b

    def feed(self, e):
        """One mmrtrace event. Events outside the controller's registers are ignored."""
        a = e.get('a')
        if a is None or not self.base <= a < self.base + 0x100:
            return
        off, k = a - self.base, e['k']
        if k == 'w' and off == CTL:
            self._ctl = e['v']
        elif k == 'w' and off == SLVSEL:
            self._slvsel = e['v']
            n = self._active()
            if self._cur is not None and n != self._cur[0]:
                self._close()
            if n is not None and self._cur is None:
                self._cur = (n, bytearray(b''.join(self._pending)), bytearray())
                self._pending = []
        elif k == 'w' and off == TFIFO:
            if self._cur is None:
                self._pending.append(self._word(e['v']))
            else:
                self._cur[1].extend(self._word(e['v']))
        elif k == 'r' and off == RFIFO:
            if self._cur is None:
                if self.frames:
                    self.frames[-1]['miso'] += self._word(e['v']).hex()
                else:
                    self.stray += 1
            else:
                self._cur[2].extend(self._word(e['v']))

    def _close(self):
        n, mosi, miso = self._cur
        self.frames.append({'mosi': mosi.hex(), 'miso': miso.hex(), 'ssel': n})
        self._cur = None

    def finish(self):
        """Close a frame still open at the end of the trace (it is kept, marked 'open': True)."""
        if self._cur is not None:
            self._close()
            self.frames[-1]['open'] = True
        self.stray += len(self._pending)
        self._pending = []
        return self.frames


def decode(events, base, ssel=None):
    """Frames of one controller from an iterable of mmrtrace events."""
    d = SpiFrameDecoder(base, ssel)
    for e in events:
        d.feed(e)
    return d.finish()
