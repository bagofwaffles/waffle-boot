# SPDX-License-Identifier: GPL-2.0-or-later
"""AES (FIPS-197) and CBC mode (SP 800-38A), pure Python, for test packages only. Slow but
dependency-free; self-tested against the FIPS-197 Appendix C.3 vector."""


def _xt(a):
    return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else a << 1


def _mul(a, b):
    r = 0
    while b:
        if b & 1:
            r ^= a
        a = _xt(a)
        b >>= 1
    return r


def _sbox():
    s = [0] * 256
    for i in range(256):
        inv = 0 if i == 0 else next(x for x in range(1, 256) if _mul(i, x) == 1)
        v = inv
        for k in range(1, 5):
            v ^= ((inv << k) | (inv >> (8 - k))) & 0xFF
        s[i] = v ^ 0x63
    return s


SBOX = _sbox()
INV = [0] * 256
for _i, _v in enumerate(SBOX):
    INV[_v] = _i
M2 = [_mul(i, 2) for i in range(256)]
M3 = [_mul(i, 3) for i in range(256)]
M9 = [_mul(i, 9) for i in range(256)]
M11 = [_mul(i, 11) for i in range(256)]
M13 = [_mul(i, 13) for i in range(256)]
M14 = [_mul(i, 14) for i in range(256)]


def expand(key):
    nk = len(key) // 4
    if nk not in (4, 6, 8):
        raise ValueError('AES key must be 16, 24 or 32 bytes')
    nr = nk + 6
    w = [list(key[4 * i:4 * i + 4]) for i in range(nk)]
    rcon = 1
    for i in range(nk, 4 * (nr + 1)):
        t = list(w[i - 1])
        if i % nk == 0:
            t = [SBOX[b] for b in t[1:] + t[:1]]
            t[0] ^= rcon
            rcon = _xt(rcon)
        elif nk > 6 and i % nk == 4:
            t = [SBOX[b] for b in t]
        w.append([a ^ b for a, b in zip(w[i - nk], t)])
    return [sum(w[4 * r:4 * r + 4], []) for r in range(nr + 1)]


def _shift(s, inv=False):
    o = [0] * 16
    for c in range(4):
        for r in range(4):
            src = (c + r) % 4 if not inv else (c - r) % 4
            o[4 * c + r] = s[4 * src + r]
    return o


def encrypt_block(rk, b):
    s = [x ^ k for x, k in zip(b, rk[0])]
    for rnd in range(1, len(rk)):
        s = _shift([SBOX[x] for x in s])
        if rnd != len(rk) - 1:
            m = []
            for c in range(4):
                a = s[4 * c:4 * c + 4]
                m += [M2[a[0]] ^ M3[a[1]] ^ a[2] ^ a[3], a[0] ^ M2[a[1]] ^ M3[a[2]] ^ a[3],
                      a[0] ^ a[1] ^ M2[a[2]] ^ M3[a[3]], M3[a[0]] ^ a[1] ^ a[2] ^ M2[a[3]]]
            s = m
        s = [x ^ k for x, k in zip(s, rk[rnd])]
    return bytes(s)


def decrypt_block(rk, b):
    n = len(rk) - 1
    s = [x ^ k for x, k in zip(b, rk[n])]
    for rnd in range(n - 1, -1, -1):
        s = [INV[x] for x in _shift(s, inv=True)]
        s = [x ^ k for x, k in zip(s, rk[rnd])]
        if rnd:
            m = []
            for c in range(4):
                a = s[4 * c:4 * c + 4]
                m += [M14[a[0]] ^ M11[a[1]] ^ M13[a[2]] ^ M9[a[3]], M9[a[0]] ^ M14[a[1]] ^ M11[a[2]] ^ M13[a[3]],
                      M13[a[0]] ^ M9[a[1]] ^ M14[a[2]] ^ M11[a[3]], M11[a[0]] ^ M13[a[1]] ^ M9[a[2]] ^ M14[a[3]]]
            s = m
    return bytes(s)


def cbc_encrypt(key, iv, data):
    if len(data) % 16:
        raise ValueError('CBC data must be a multiple of 16 bytes')
    rk, prev, out = expand(key), bytes(iv), bytearray()
    for i in range(0, len(data), 16):
        prev = encrypt_block(rk, bytes(a ^ b for a, b in zip(data[i:i + 16], prev)))
        out += prev
    return bytes(out)


def cbc_decrypt(key, iv, data):
    if len(data) % 16:
        raise ValueError('CBC data must be a multiple of 16 bytes')
    rk, prev, out = expand(key), bytes(iv), bytearray()
    for i in range(0, len(data), 16):
        c = bytes(data[i:i + 16])
        out += bytes(a ^ b for a, b in zip(decrypt_block(rk, c), prev))
        prev = c
    return bytes(out)


def selftest():
    key = bytes(range(32))
    pt = bytes.fromhex('00112233445566778899aabbccddeeff')
    ct = encrypt_block(expand(key), pt)
    assert ct.hex() == '8ea2b7ca516745bfeafc49904b496089', ct.hex()
    assert decrypt_block(expand(key), ct) == pt
    return True
