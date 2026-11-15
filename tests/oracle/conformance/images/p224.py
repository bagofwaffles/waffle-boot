# SPDX-License-Identifier: GPL-2.0-or-later
"""ECDSA over NIST P-224 (FIPS 186-4), pure Python, for test packages: keys come from `test_key(seed)`,
nonces are derived deterministically from the key and the digest."""

import hashlib
import hmac

P = 2 ** 224 - 2 ** 96 + 1
A = P - 3
B = 0xB4050A850C04B3ABF54132565044B0B7D7BFD8BA270B39432355FFB4
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFF16A2E0B8F03E13DD29455C5C2A3D
G = (0xB70E0CBD6BB4BF7F321390B94A03C1D356C21122343280D6115C1D21,
     0xBD376388B5F723FB4C22DFE6CD4375A05A07476444D5819985007E34)


def _add(p, q):
    if p is None:
        return q
    if q is None:
        return p
    if p[0] == q[0] and (p[1] + q[1]) % P == 0:
        return None
    if p == q:
        lam = (3 * p[0] * p[0] + A) * pow(2 * p[1], -1, P) % P
    else:
        lam = (q[1] - p[1]) * pow(q[0] - p[0], -1, P) % P
    x = (lam * lam - p[0] - q[0]) % P
    return x, (lam * (p[0] - x) - p[1]) % P


def mul(k, pt=G):
    r = None
    while k:
        if k & 1:
            r = _add(r, pt)
        pt = _add(pt, pt)
        k >>= 1
    return r


def on_curve(pt):
    x, y = pt
    return (y * y - (x * x * x + A * x + B)) % P == 0


def _z(digest):
    z = int.from_bytes(digest, 'big')
    excess = len(digest) * 8 - 224
    return z >> excess if excess > 0 else z


def test_key(seed=b'opxy-conformance-test-key'):
    """(private int, public (x, y)) derived from `seed`."""
    if isinstance(seed, str):
        seed = seed.encode()
    d = int.from_bytes(hashlib.sha256(b'p224 test key' + seed).digest(), 'big') % (N - 1) + 1
    return d, mul(d)


def sign(d, digest):
    z = _z(digest)
    ctr = 0
    while True:
        k = int.from_bytes(hmac.new(d.to_bytes(28, 'big'), digest + bytes([ctr]), hashlib.sha256).digest(), 'big') % N
        ctr += 1
        if k == 0:
            continue
        r = mul(k)[0] % N
        if r == 0:
            continue
        s = pow(k, -1, N) * (z + r * d) % N
        if s:
            return r, s


def verify(pub, digest, r, s):
    if not (1 <= r < N and 1 <= s < N) or pub is None or not on_curve(pub):
        return False
    w = pow(s, -1, N)
    pt = _add(mul(_z(digest) * w % N), mul(r * w % N, pub))
    return pt is not None and pt[0] % N == r


def pub_bytes(pub):
    """x then y, 28 bytes each, big-endian (0x38 bytes)."""
    return pub[0].to_bytes(28, 'big') + pub[1].to_bytes(28, 'big')


def pub_from_bytes(b):
    return int.from_bytes(b[:28], 'big'), int.from_bytes(b[28:56], 'big')
