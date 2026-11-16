#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A probe of the ADSP-BF70x Security Packet Engine (PKTE) from the host.

    pkte_probe.py [--port /dev/cu.usbmodemNNN] [--force] [dhm] [arm] [drv] [speed]

It drives the engine with the developer host's register reads and writes
(wiki/Developer-Host-Link.md: held mode, developer mode on), so it needs no new build.
It checks what the driver relies on: whether the
engine decrypts AES-256-CBC and hashes SHA-256 as the hardware reference
says (chapter 15), how bytes map to words, whether it reads and writes DDR
by itself (autonomous ring mode), whether a hash and a CBC chain continue
across operations, whether it decrypts in place, and how fast it is.

dhm    direct host mode: every byte through the registers (three tests)
arm    autonomous ring mode on a little memory at the top of the heap
drv    the operations exactly as src/drivers/bf70x/pkte.c starts them
speed  autonomous ring mode over 8 MiB, in 1 MiB and in 32 KiB operations
With no name, all four.

It writes the engine's registers and, for all but dhm, 0x89C00000-0x8A000000:
the top 4 MiB of the bootloader's heap, which grows up from 0x8833D000 and
in held mode stays some 24 MiB short of it. Before it writes there it
samples the range and stops unless every word is still one of the two fill
values DDR holds where nothing has been written since power-on (0x55555555,
0xAAAAAAAA). --force skips that check: use it only for a second run since
power-on (the first run's own writes fail the check), or on a unit whose
untouched DDR reads otherwise, and then only in held mode entered straight
from power-on, with no install, transfer or MTP session since. It writes no
flash and no eMMC. Run it with no install in progress, and power the unit
off and on afterwards.

Python 3, standard library only.
"""
import hashlib
import struct
import sys
import time

import testos_mon as tm

BASE = 0x200E0000
CTL_STAT, SRC, DEST, SA, STATE, USERID, LEN = 0x000, 0x004, 0x008, 0x00C, 0x010, 0x018, 0x01C
CDRBASE, RDRBASE, RING_CFG, CDSC, RDSC = 0x080, 0x084, 0x088, 0x090, 0x094
CFG, STAT, INBUF, OUTBUF, CLK_CTL, IUMSK = 0x100, 0x104, 0x110, 0x114, 0x1E8, 0x200
SA_CMD0, SA_CMD1, SA_KEY, SA_RDY = 0x400, 0x404, 0x408, 0x47C
ST_IV, ST_CNT, ST_DIGEST, DATAIO = 0x500, 0x510, 0x518, 0x800

# PKTE_SA_CMD0 and PKTE_SA_CMD1 (tables 15-61, 15-62)
OP_DECRYPT, OP_HASH, DIR_IN = 0, 3, 1 << 3
CIPHER_AES, CIPHER_NULL, HASH_SHA256, HASH_NULL = 3 << 8, 15 << 8, 3 << 12, 15 << 12
DIGEST_8 = 8 << 20
IV_STATE, HASH_STATE, HASH_INIT, SAVE_IV, SAVE_HASH = 2 << 24, 2 << 26, 3 << 26, 1 << 28, 1 << 29
KEY_256, CBC, COPY_PAYLOAD, COPY_DIGEST, COPY_PAD = 4 << 24, 1 << 8, 1 << 2, 1 << 0, 1 << 3
HOSTRDY, PERDY, HASHFINAL, LEN_HSTRDY = 1 << 0, 1 << 1, 1 << 4, 1 << 22
OPDN, STAT_ERR = 1 << 9, 0x1E0

HEAP = 0x89C00000
M_SRC, M_DST, M_REC, M_EVICT = HEAP, HEAP + 0x100000, HEAP + 0x200000, HEAP + 0x300000
M_SA, M_STATE, M_CDR, M_RDR = M_REC, M_REC + 0x100, M_REC + 0x1000, M_REC + 0x4000
FILL = {0x55555555, 0xAAAAAAAA}     # DDR where nothing has been written since power-on
KEEP = 0xC3C3C3C3                   # a hash's destination, which must stay as it was

# NIST SP 800-38A F.2.6, CBC-AES256.Decrypt
KEY = bytes.fromhex('603deb1015ca71be2b73aef0857d7781' '1f352c073b6108d72d9810a30914dff4')
IV = bytes.fromhex('000102030405060708090a0b0c0d0e0f')
CT = bytes.fromhex('f58c4c04d6e5f1ba779eabfb5f7bfbd6' '9cfc4e967edb808d679f777bc6702c7d'
                   '39f23369a9d9bacfa530e26304231461' 'b2eb05e2c39be9fcda6c19078c6a9d1b')
PT = bytes.fromhex('6bc1bee22e409f96e93d7e117393172a' 'ae2d8a571e03ac9c9eb76fac45af8e51'
                   '30c81c46a35ce411e5fbc1191a0a52ef' 'f69f2445df4f9b17ad2b417be66c3710')

K256 = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2]


def sha256_state(data):
    """The eight state words after the whole 64-byte blocks of data, no padding (FIPS 180-4)."""
    h = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]
    rotr = lambda x, n: (x >> n | x << (32 - n)) & 0xFFFFFFFF
    for off in range(0, len(data), 64):
        w = list(struct.unpack('>16I', data[off:off + 64]))
        for i in range(16, 64):
            s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ w[i - 15] >> 3
            s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ w[i - 2] >> 10
            w.append((w[i - 16] + s0 + w[i - 7] + s1) & 0xFFFFFFFF)
        a, b, c, d, e, f, g, hh = h
        for i in range(64):
            t1 = (hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + (e & f ^ ~e & g) + K256[i] + w[i]) & 0xFFFFFFFF
            t2 = ((rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + (a & b ^ a & c ^ b & c)) & 0xFFFFFFFF
            a, b, c, d, e, f, g, hh = (t1 + t2) & 0xFFFFFFFF, a, b, c, (d + t1) & 0xFFFFFFFF, e, f, g
        h = [(x + y) & 0xFFFFFFFF for x, y in zip(h, (a, b, c, d, e, f, g, hh))]
    return h


class Unit:
    def __init__(self, mon):
        self.mon = mon

    def rd(self, addr, words=1):
        out = []
        while words:
            n = min(words, 60)
            r = self.mon.checked(bytes([tm.READ]) + struct.pack('<IB', addr, n))
            out += struct.unpack('<%dI' % n, r[:4 * n])
            addr, words = addr + 4 * n, words - n
        return out

    def wr(self, addr, *values):
        for i, v in enumerate(values):
            self.mon.checked(bytes([tm.WRITE]) + struct.pack('<II', addr + 4 * i, v & 0xFFFFFFFF))

    def reg(self, off):
        return self.rd(BASE + off)[0]

    def put(self, off, *values):
        self.wr(BASE + off, *values)

    def mem(self, addr, data):
        data += bytes(-len(data) % 4)
        self.wr(addr, *struct.unpack('<%dI' % (len(data) // 4), data))

    def evict(self):
        """Reads enough other memory that no line read before is still in the data cache."""
        self.rd(M_EVICT, 0x20000 // 4)

    def bytes_at(self, addr, n):
        return struct.pack('<%dI' % ((n + 3) // 4), *self.rd(addr, (n + 3) // 4))[:n]

    def reset(self, mode):
        self.put(CFG, 3)
        self.put(CFG, mode << 8)


def heap_free(u):
    """Whether the probe's memory looks unwritten since power-on, from 16 words every 64 KiB."""
    seen = set()
    for addr in range(HEAP, HEAP + 0x400000, 0x10000):
        seen.update(u.rd(addr, 16))
    return seen <= FILL


def kept(u, addr, n):
    """Whether n bytes at addr, filled with KEEP before an operation, still hold it (after result())."""
    return set(u.rd(addr, n // 4)) == {KEEP}


def words(data):
    return struct.unpack('<%dI' % (len(data) // 4), data)


def verdict(name, ok, detail=''):
    print('%-44s %s%s' % (name, 'ok' if ok else 'FAILED', '  ' + detail if detail else ''))
    return ok


def digest_order(got, want):
    """How eight words read from the engine relate to SHA-256's H0..H7."""
    if list(got) == list(want):
        return 'the words are H0..H7'
    if list(got) == [struct.unpack('<I', struct.pack('>I', w))[0] for w in want]:
        return 'each word is H byte-swapped (the digest bytes in memory order)'
    return None


# ---- direct host mode ----

def dhm_op(u, cmd0, cmd1, data, final=True, iv=None, out_words=None):
    """One operation, its input through PKTE_DATAIO_BUF; the output words, PKTE_STAT, PKTE_CTL_STAT."""
    u.reset(0)
    u.put(CTL_STAT, HOSTRDY | (HASHFINAL if final else 0))
    u.put(LEN, LEN_HSTRDY | len(data))
    u.put(CDSC, 1)
    if iv is not None:
        u.put(ST_IV, *words(iv))
    u.put(ST_CNT, 0, 0)
    u.put(SA_CMD0, cmd0)
    u.put(SA_CMD1, cmd1)
    if cmd0 & 0xF00 == CIPHER_AES:
        u.put(SA_KEY, *words(KEY))
    u.put(SA_RDY, 1)
    u.put(DATAIO, *words(data))
    u.put(INBUF, len(data))
    stat = 0
    for _ in range(200):
        stat = u.reg(STAT)
        if stat & (OPDN | STAT_ERR):
            break
    n = out_words if out_words is not None else stat >> 22
    out = u.rd(BASE + DATAIO, n) if n else []
    u.put(OUTBUF, 4 * n)
    ctl = u.reg(CTL_STAT)
    return out, stat, ctl


def dhm(u):
    print('direct host mode')
    out, stat, ctl = dhm_op(u, OP_DECRYPT | DIR_IN | CIPHER_AES | HASH_NULL | IV_STATE,
                            KEY_256 | CBC | COPY_PAYLOAD, CT, iv=IV)
    got = struct.pack('<%dI' % len(out), *out)
    verdict('AES-256-CBC decrypt (SP 800-38A F.2.6)', got == PT,
            'stat %08x ctl %08x out %s' % (stat, ctl, got.hex()))
    u.put(RDSC, 1)

    out, stat, ctl = dhm_op(u, OP_HASH | CIPHER_NULL | HASH_SHA256 | DIGEST_8 | HASH_INIT,
                            COPY_DIGEST | COPY_PAD, CT)
    want = struct.unpack('>8I', hashlib.sha256(CT).digest())
    order = digest_order(out[-8:], want) if len(out) >= 8 else None
    verdict('SHA-256 of 64 bytes, final', order is not None,
            (order or '') + ' stat %08x ctl %08x out %s' % (stat, ctl, ' '.join('%08x' % w for w in out)))
    u.put(RDSC, 1)

    out, stat, ctl = dhm_op(u, OP_HASH | CIPHER_NULL | HASH_SHA256 | DIGEST_8 | HASH_INIT | SAVE_HASH,
                            0, CT, final=False, out_words=0)
    state, count = u.rd(BASE + ST_DIGEST, 8), u.rd(BASE + ST_CNT, 2)
    order = digest_order(state, sha256_state(CT))
    verdict('SHA-256 of one block, not final: the state', order is not None and count[0] == 64,
            (order or '') + ' count %d stat %08x ctl %08x state %s'
            % (count[0], stat, ctl, ' '.join('%08x' % w for w in state)))
    u.put(RDSC, 1)


# ---- autonomous ring mode ----

def ring(u, descs, timeout=30.0):
    """Runs the descriptors (src, dst, length, final) on the SA and state records in memory.
    Returns the seconds from the trigger to the last result, or None when they did not all come."""
    u.reset(3)
    u.put(RING_CFG, len(descs))            # one entry more than is used
    u.put(CDRBASE, M_CDR)
    u.put(RDRBASE, M_RDR)
    for i, (src, dst, length, final) in enumerate(descs):
        u.wr(M_CDR + 32 * i, HOSTRDY | (HASHFINAL if final else 0), src, dst, M_SA, M_STATE, 0, i,
             LEN_HSTRDY | length)
    t0 = time.monotonic()
    u.put(CDSC, len(descs))
    while time.monotonic() - t0 < timeout:
        done = u.reg(RDSC) & 0x7FF
        t = time.monotonic() - t0
        if done >= len(descs):
            return t
    print('  only %d of %d results; PKTE_STAT %08x, interrupt status %08x'
          % (u.reg(RDSC) & 0x7FF, len(descs), u.reg(STAT), u.reg(IUMSK)))
    return None


def sa_record(u, cmd0, cmd1, iv=None, state=None, count=0):
    u.wr(M_SA, cmd0, cmd1, *(words(KEY) + (0,) * 22))
    u.wr(M_STATE, *(words(iv or bytes(16)) + (count, 0) + tuple(state or (0,) * 8)))


def result(u, n):
    """The status bytes of the first n result descriptors."""
    u.evict()
    return ['%02x' % (u.rd(M_RDR + 32 * i)[0] >> 16 & 0xFF) for i in range(n)]


def arm(u):
    print('autonomous ring mode')
    dec0 = OP_DECRYPT | DIR_IN | CIPHER_AES | HASH_NULL | IV_STATE | SAVE_IV
    dec1 = KEY_256 | CBC | COPY_PAYLOAD
    u.mem(M_SRC, CT)
    sa_record(u, dec0, dec1, iv=IV)
    done = ring(u, [(M_SRC, M_DST, 64, True)])
    status = result(u, 1)
    got = u.bytes_at(M_DST, 64)
    verdict('AES-256-CBC decrypt, DDR to DDR', done is not None and got == PT,
            'status %s out %s' % (status, got.hex()))
    iv = u.bytes_at(M_STATE, 16)
    verdict('  the state record holds the next IV', iv == CT[48:], iv.hex())

    # the chain continued from the state the engine saved, in two operations, in place
    u.mem(M_SRC + 0x1000, CT)
    sa_record(u, dec0, dec1, iv=IV)
    done = ring(u, [(M_SRC + 0x1000, M_SRC + 0x1000, 32, True), (M_SRC + 0x1020, M_SRC + 0x1020, 32, True)])
    status = result(u, 2)
    got = u.bytes_at(M_SRC + 0x1000, 64)
    verdict('AES-256-CBC decrypt in place, two operations', done is not None and got == PT,
            'status %s out %s' % (status, got.hex()))

    # a hash continued across two operations, neither final
    data = CT + PT + CT
    u.mem(M_SRC + 0x2000, data)
    hash0 = OP_HASH | CIPHER_NULL | HASH_SHA256 | DIGEST_8 | SAVE_HASH
    sa_record(u, hash0 | HASH_INIT, 0)
    u.wr(M_DST + 0x2000, *((KEEP,) * 32))
    done = ring(u, [(M_SRC + 0x2000, M_DST + 0x2000, 128, False)])
    status = result(u, 1)
    untouched = kept(u, M_DST + 0x2000, 128)
    state, count = u.rd(M_STATE + 24, 8), u.rd(M_STATE + 16, 2)
    order = digest_order(state, sha256_state(data[:128]))
    verdict('SHA-256 of two blocks, not final: the state', done is not None and order is not None,
            (order or '') + ' status %s count %d state %s' % (status, count[0], ' '.join('%08x' % w for w in state)))
    verdict('  it writes nothing to its destination', untouched)
    u.wr(M_SA, hash0 | HASH_STATE)
    done = ring(u, [(M_SRC + 0x2080, M_DST + 0x2000, 64, False)])
    status = result(u, 1)
    state, count = u.rd(M_STATE + 24, 8), u.rd(M_STATE + 16, 2)
    order = digest_order(state, sha256_state(data))
    verdict('  continued from the saved state', done is not None and order is not None,
            (order or '') + ' status %s count %d state %s' % (status, count[0], ' '.join('%08x' % w for w in state)))


def drv(u):
    """The driver's own sequence (src/drivers/bf70x/pkte.c): one descriptor, no final flag, the hash
    always from the state record, its destination a buffer as long as its input that must stay as
    it was."""
    print("the driver's sequence")
    data = (CT + PT) * 8
    u.mem(M_SRC + 0x3000, data)
    u.wr(M_SA, OP_HASH | CIPHER_NULL | HASH_SHA256 | DIGEST_8 | HASH_STATE | SAVE_HASH, *((0,) * 31))
    u.wr(M_STATE, *((0,) * 6 + tuple(sha256_state(b''))))
    u.wr(M_DST + 0x4000, *((KEEP,) * (len(data) // 4)))
    done = ring(u, [(M_SRC + 0x3000, M_DST + 0x4000, len(data), False)])
    status = result(u, 1)
    untouched = kept(u, M_DST + 0x4000, len(data))
    state, count = u.rd(M_STATE + 24, 8), u.rd(M_STATE + 16, 2)
    verdict('SHA-256 of 16 blocks from the given state', done is not None and status == ['00']
            and list(state) == sha256_state(data) and count[0] == len(data), 'status %s count %d' % (status, count[0]))
    verdict('  it writes nothing to its destination', untouched)
    sa_record(u, OP_DECRYPT | DIR_IN | CIPHER_AES | HASH_NULL | IV_STATE | SAVE_IV,
              KEY_256 | CBC | COPY_PAYLOAD, iv=IV)
    done = ring(u, [(M_SRC, M_DST + 0x3000, 64, False)])
    status = result(u, 1)
    got = u.bytes_at(M_DST + 0x3000, 64)
    verdict('AES-256-CBC decrypt, no final flag', done is not None and status == ['02'] and got == PT,
            'status %s' % status)
    # wb_pkte_end(): a block under a zero key, then the registers as reset leaves them
    u.wr(M_SA, OP_DECRYPT | DIR_IN | CIPHER_AES | HASH_NULL | IV_STATE | SAVE_IV,
         KEY_256 | CBC | COPY_PAYLOAD, *((0,) * 30))
    u.wr(M_STATE, *((0,) * 14))
    u.wr(M_SRC + 0x5000, 0, 0, 0, 0)
    done = ring(u, [(M_SRC + 0x5000, M_DST + 0x5000, 16, False)])
    status = result(u, 1)
    u.reset(0)
    u.put(RING_CFG, 0)
    u.put(CDRBASE, 0)
    u.put(RDRBASE, 0)
    regs = [u.reg(r) for r in (CFG, RING_CFG, CDRBASE, RDRBASE)]
    verdict('the end of use: a zero key, registers as after reset', done is not None and status == ['02']
            and regs == [0, 0, 0, 0], 'status %s registers %s' % (status, ' '.join('%08x' % r for r in regs)))


def speed(u):
    print('speed (host clock: a few ms of error on each)')
    dec = (OP_DECRYPT | DIR_IN | CIPHER_AES | HASH_NULL | IV_STATE | SAVE_IV, KEY_256 | CBC | COPY_PAYLOAD)
    sha = (OP_HASH | CIPHER_NULL | HASH_SHA256 | DIGEST_8 | HASH_STATE | SAVE_HASH, 0)
    for name, (cmd0, cmd1) in (('AES-256-CBC decrypt', dec), ('SHA-256', sha)):
        for size, count in ((0x100000 - 0x40, 8), (0x8000, 255)):
            sa_record(u, cmd0, cmd1, iv=IV, state=sha256_state(b''))
            t = ring(u, [(M_SRC, M_DST, size, False)] * count, 120.0)
            if t is None:
                continue
            status = set(result(u, count))
            print('%-20s %3d x %7d bytes %7.3f s %7.1f MiB/s  status %s'
                  % (name, count, size, t, size * count / 1048576 / t, ' '.join(sorted(status))))


def main(argv):
    args = argv[1:]
    port = None
    if args[:1] == ['--port']:
        port, args = args[1], args[2:]
    force = args[:1] == ['--force']
    if force:
        args = args[1:]
    if args and (set(args) - {'dhm', 'arm', 'drv', 'speed'}):
        print(__doc__)
        return 2
    u = Unit(tm.Monitor(port or tm.find_port()))
    if args != ['dhm'] and not force and not heap_free(u):
        print('0x%08X-0x%08X does not hold the fill of unwritten DDR: something has written there '
              '(this probe, if it ran since power-on). Nothing was written. See --force in the help.'
              % (HEAP, HEAP + 0x400000))
        return 1
    print('PKTE_CFG %08x PKTE_STAT %08x PKTE_CLK_CTL %08x' % (u.reg(CFG), u.reg(STAT), u.reg(CLK_CTL)))
    for name, test in (('dhm', dhm), ('arm', arm), ('drv', drv), ('speed', speed)):
        if not args or name in args:
            test(u)
    u.reset(0)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
