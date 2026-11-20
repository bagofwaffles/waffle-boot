#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate S-11's vectors and tables. Each case's `expect` is stated independently, and the generator
checks that s11_model agrees before it writes anything.

    python3 -I -B tests/oracle/S-11-packages/gen_vectors.py

Only conformance test keys are used (images.tfw.test_keys). Python 3, standard library only.
"""
import csv
import hashlib
import io
import itertools
import json
import os
import struct
import sys
import types
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'conformance'))
import s11_model as m  # noqa: E402
from images import aes, ldr, p224, tfw, fwzip  # noqa: E402
from images.crc import crc16_xmodem  # noqa: E402

# Every zip member gets one fixed timestamp, so that the vectors regenerate byte for byte.
zipfile.time = types.SimpleNamespace(time=lambda: 0, localtime=lambda t=None: (2026, 1, 1, 0, 0, 0, 3, 1, 0))

V = os.path.join(HERE, 'vectors')
PKG = os.path.join(V, 'pkg')

# Every file write_packages() and disk_cases() write under vectors/pkg/, by name; main() checks the set.
PKG_FILES = (
    'cdc_forced.bin', 'crc_field_wrong.bin', 'crc_header_reserved_flip.bin', 'crc_last_byte_flip.bin',
    'crc_manifest_flip.bin', 'crc_payload_flip.bin', 'decoy_firmware.zip', 'enc_chain.bin',
    'enc_split_chain.bin', 'enc_trailing.bin', 'forced_by_name.bin', 'form_body_ok_midi.bin',
    'form_body_on_disk.bin', 'form_file_ok.bin', 'form_file_on_host.bin', 'header_address_garbage.bin',
    'header_bad_magic.bin', 'header_device_0.bin', 'header_device_1.bin', 'header_device_2.bin',
    'header_device_255.bin', 'header_device_3.bin', 'header_device_6_forced.bin', 'header_device_7.bin',
    'header_length_garbage.bin', 'header_product_bad.bin', 'header_product_sku_bits.bin',
    'header_product_variant.bin', 'header_reserved_flip.bin', 'header_version_forced.bin',
    'header_version_tampered_te_signed.bin', 'key_00.bin', 'key_00_nokey.bin', 'key_01.bin', 'key_01_dev.bin',
    'key_01_option.bin', 'key_01_option_nokey.bin', 'key_02.bin', 'key_fe.bin', 'key_ff.bin',
    'key_ff_option_dev.bin', 'manifest_block_version_2.bin', 'manifest_product_bad.bin',
    'manifest_product_bad_option.bin', 'manifest_tag_TESH.bin', 'midi_not_forced.bin',
    'multi_fault_crc_and_tag.bin', 'multi_fault_sku_and_hash.bin', 'name_encrypted.bin',
    'name_firmware_zip.bin', 'name_no_zip.bin', 'name_upper_ZIP.bin', 'name_zip_after_nul.bin',
    'name_zip_at_78.bin', 'name_zip_at_7a.bin', 'not_forced_upper.bin', 'payload_appended_byte.bin',
    'payload_hash_flip.bin', 'payload_hash_flip_option.bin', 'payload_length_minus1.bin',
    'payload_length_plus1.bin', 'payload_length_plus1_option.bin', 'preamble_bad_magic.bin',
    'preamble_crc_garbage.bin', 'preamble_forced.bin', 'preamble_type_1.bin',
    'recorded_version_file_absent.bin', 'short_body.bin', 'short_file.bin', 'sig_high_bytes_nonzero.bin',
    'sig_index_01.bin', 'sig_index_01_option.bin', 'sig_index_ff_dev.bin', 'sig_no_q0.bin',
    'sig_no_q0_option.bin', 'sig_other_key.bin', 'sig_other_key_option.bin', 'sig_r_zero.bin',
    'sig_reserved_flip_after_signing.bin', 'sig_s_n.bin', 'sig_unsigned_option.bin', 'sig_valid.bin',
    'version_header_vs_file.bin', 'zip_garbage.bin', 'ziplen_enc_needs_block.bin', 'ziplen_exact.bin',
    'ziplen_over_by_1.bin', 'ziplen_zero.bin',
)
AES0, D, PUB = tfw.test_keys()
AES1 = tfw.test_keys(b'opxy-conformance-k1')[0]
D_OTHER, PUB_OTHER = p224.test_key(b'not-the-cloud-key')


# ---- a package writer with a knob for every field

def zipb(files, method=zipfile.ZIP_DEFLATED):
    b = io.BytesIO()
    with zipfile.ZipFile(b, 'w', method) as z:
        for n, d in files.items():
            z.writestr(n, d)
    return b.getvalue()


def os_image(n, entry=0x11A00000, regions=(), fill=b'o'):
    """An OS image of n bytes that S-05.024 accepts: a stream of no destination (or `regions`) and filler
    after its FINAL block."""
    st = ldr.simple_os(entry, list(regions))
    assert len(st) <= n
    return st + fill * (n - len(st))


BASE_ZIP = fwzip.build({'os': os_image(256 * 48)}, version=(1, 2, 0))


def assemble(zip_bytes=BASE_ZIP, name=b'firmware.zip', key_index=0xFE, key=None, sig_index=0x00, sign_d=D,
             iv=bytes(range(16)), pre=True, pre_magic=0xBABECAFE, pre_type=0, pre_crc=None, pre_version=(1, 2, 0, 0),
             magic=0xBEEFCAFE, device=1, length=None, version=(1, 2, 0, 0), address=0, product=0x84000,
             hdr_reserved=b'', tag=b'TESG', block_version=1, mprod=0x84000, zip_length=None, payload_extra=b'',
             split_chain=False, man_patch=None, post=None, fix_crc=True, final=None):
    # The like-for-like case is images.tfw.build; the knobs below write what that builder never writes
    # (bad magics, lengths, CRCs, split chains, patches).
    if (pre_magic == 0xBABECAFE and pre_type == 0 and pre_crc is None and magic == 0xBEEFCAFE and length is None
            and not hdr_reserved and tag == b'TESG' and block_version == 1 and mprod == product
            and zip_length is None and not payload_extra and not split_chain and man_patch is None
            and post is None and fix_crc and final is None and len(name) <= 0x100
            and (key_index in (0x00, 0x01, 0xFE) or key is not None)):
        return tfw.build(zip_bytes, name=name, version=version, device_id=device, product=product,
                         preamble=pre_version if pre else None, aes_key_index=key_index, sig_key_index=sig_index,
                         iv=iv, address=address, aes_key=key, sign_d=sign_d,
                         signature='valid' if sign_d is not None else 'unsigned')
    return _assemble_raw(zip_bytes, name, key_index, key, sig_index, sign_d, iv, pre, pre_magic, pre_type,
                         pre_crc, pre_version, magic, device, length, version, address, product, hdr_reserved,
                         tag, block_version, mprod, zip_length, payload_extra, split_chain, man_patch, post,
                         fix_crc, final)


def _assemble_raw(zip_bytes, name, key_index, key, sig_index, sign_d, iv, pre, pre_magic, pre_type, pre_crc,
                  pre_version, magic, device, length, version, address, product, hdr_reserved, tag,
                  block_version, mprod, zip_length, payload_extra, split_chain, man_patch, post, fix_crc, final):
    nb = name.ljust(0x100, b'\0')[:0x100]
    zp = zip_bytes + bytes(-len(zip_bytes) % 0x80)
    if key_index in (0x00, 0x01) or key is not None:
        k = key or (AES0 if key_index == 0x00 else AES1)
        if split_chain:
            payload = aes.cbc_encrypt(k, iv, nb) + aes.cbc_encrypt(k, iv, zp)
        else:
            payload = aes.cbc_encrypt(k, iv, nb + zp)
    else:
        payload = nb + zp
    payload += payload_extra
    man = bytearray(0x300)
    man[0x04] = key_index
    man[0x70:0x80] = iv
    struct.pack_into('<I', man, 0x80, len(zip_bytes) if zip_length is None else zip_length)
    man[0x84:0x88] = tag
    man[0x88] = block_version
    man[0x89] = sig_index
    struct.pack_into('<I', man, 0x8C, mprod)
    struct.pack_into('<I', man, 0x98, len(payload))
    man[0x9C:0xBC] = hashlib.sha256(payload).digest()
    if man_patch:
        man_patch(man, payload)
    if sign_d is not None:
        r, s = p224.sign(sign_d, hashlib.sha256(bytes(man[:0x10]) + bytes(0x40) + bytes(man[0x50:])).digest())
        man[0x10:0x30] = r.to_bytes(32, 'big')
        man[0x30:0x50] = s.to_bytes(32, 'big')
    body_rest = bytes(man) + payload
    if post:
        body_rest = post(bytearray(body_rest))
    hdr = struct.pack('>IBIH4HII', magic, device, len(body_rest) if length is None else length,
                      crc16_xmodem(body_rest) if fix_crc else 0x1234, *version, address, product)
    hdr = (hdr + hdr_reserved).ljust(0x40, b'\0')[:0x40]
    body = hdr + bytes(body_rest)
    if pre:
        p = struct.pack('>IBH4H', pre_magic, pre_type, crc16_xmodem(body) if pre_crc is None else pre_crc,
                        *pre_version)
        body = p.ljust(0x40, b'\0') + body
    return final(bytearray(body)) if final else body


def unit(q0=True, k0=True, k1=True, option=False, dev=False):
    return m.Unit(PUB if q0 else None, AES0 if k0 else None, AES1 if k1 else None, option, dev)


def ujson(u):
    return dict(q0='test' if u.q0 else None, k0='test' if u.k0 else None, k1='test_k1' if u.k1 else None,
                option=u.option, dev=u.dev)


def flip(i, x=0x01):
    def f(b):
        b[i] ^= x
        return bytes(b)
    return f


def man_flip(off):
    return lambda b: flip(off)(b)


# (case, kwargs for assemble, form, link, file_name, unit kwargs, expected reason or None, extra expectations)
def package_cases():
    C = []

    def add(case, kw=None, form='file', link=None, fname='op-xy.tfw', u=None, reason=None, req='', **exp):
        C.append(dict(case=case, kw=kw or {}, form=form, link=link, file_name=fname if form == 'file' else None,
                      unit=u or {}, reason=reason, req=req, exp=exp))
    add('form_file_ok', req='S-11.001', te_signed=True)
    add('form_body_ok_midi', dict(pre=False), form='body', link='midi', req='S-11.001')
    add('form_file_on_host', dict(pre=True), form='body', link='midi', reason='MAGIC', req='S-11.001')
    add('form_body_on_disk', dict(pre=False), reason='PREAMBLE', req='S-11.001')
    add('preamble_bad_magic', dict(pre_magic=0xBABECAFF), reason='PREAMBLE', req='S-11.003')
    add('preamble_type_1', dict(pre_type=1), reason='TYPE', req='S-11.003')
    add('preamble_crc_garbage', dict(pre_crc=0xDEAD), req='S-11.003')
    add('preamble_forced', dict(pre_version=(0, 1, 0, 7)), req='S-11.003', forced=True)
    add('short_file', None, reason='SHORT', req='S-11.014')
    add('short_body', None, form='body', link='midi', reason='SHORT', req='S-11.014')
    add('header_bad_magic', dict(magic=0xBEEFCAFF), reason='MAGIC', req='S-11.005')
    for d in (0, 2, 3, 7, 0xFF):
        add('header_device_%d' % d, dict(device=d), reason='DEVICE', req='S-11.006')
    add('header_device_1', dict(device=1), req='S-11.006', forced=False)
    add('header_device_6_forced', dict(device=6), req='S-11.005', forced=True)
    add('header_version_forced', dict(version=(0, 1, 0, 5)), req='S-11.005', forced=True)
    add('header_product_bad', dict(product=0x88000), reason='PRODUCT', req='S-11.007')
    add('header_product_variant', dict(product=0x84400), reason='PRODUCT', req='S-11.007')
    add('header_product_sku_bits', dict(product=0xFF0843FF), req='S-11.007')
    add('header_length_garbage', dict(length=0xFFFFFFFF), req='S-11.009')
    add('header_address_garbage', dict(address=0x1F000), req='S-11.009')
    add('header_reserved_flip', dict(hdr_reserved=bytes(range(1, 0x26))), req='S-11.009')
    add('crc_manifest_flip', dict(final=flip(0x80 + 0x200)), reason='CRC', req='S-11.008')
    add('crc_payload_flip', dict(final=flip(0x80 + 0x300 + 0x150)), reason='CRC', req='S-11.008')
    add('crc_last_byte_flip', dict(final=flip(-1)), reason='CRC', req='S-11.008')
    add('crc_header_reserved_flip', dict(final=flip(0x40 + 0x30)), req='S-11.008')
    add('crc_field_wrong', dict(fix_crc=False), reason='CRC', req='S-11.008')
    add('manifest_tag_TESH', dict(tag=b'TESH'), reason='FORMAT', req='S-11.011')
    add('manifest_block_version_2', dict(block_version=2), reason='FORMAT', req='S-11.011')
    add('sig_valid', req='S-11.012', te_signed=True)
    add('sig_reserved_flip_after_signing', dict(post=flip(0x200)), req='S-11.012', te_signed=False)
    add('sig_high_bytes_nonzero', dict(post=lambda b: bytes(b[:0x10] + b'\xAA\xBB\xCC\xDD' + b[0x14:0x30] +
                                                            b'\x11\x22\x33\x44' + b[0x34:])),
        req='S-11.012', te_signed=True)
    add('sig_r_zero', dict(post=lambda b: bytes(b[:0x10] + bytes(32) + b[0x30:])), te_signed=False,
        req='S-11.012')
    add('sig_s_n', dict(post=lambda b: bytes(b[:0x30] + p224.N.to_bytes(32, 'big') + b[0x50:])),
        te_signed=False, req='S-11.012')
    add('sig_other_key', dict(sign_d=D_OTHER), req='S-11.022', te_signed=False)
    add('sig_other_key_option', dict(sign_d=D_OTHER), u=dict(option=True), req='S-11.022', te_signed=False)
    add('sig_index_01', dict(sig_index=0x01), req='S-11.022', te_signed=False)
    add('sig_index_01_option', dict(sig_index=0x01), u=dict(option=True), req='S-11.022', te_signed=False)
    add('sig_index_ff_dev', dict(sig_index=0xFF), u=dict(dev=True), req='S-11.022', te_signed=False)
    add('sig_unsigned_option', dict(sign_d=None, sig_index=0xFF), u=dict(option=True), req='S-11.022')
    add('sig_no_q0', None, u=dict(q0=False), req='S-11.021', te_signed=False)
    add('sig_no_q0_option', None, u=dict(q0=False, option=True), req='S-11.021', te_signed=False)
    add('manifest_product_bad', dict(mprod=0x88000), reason='SKU', req='S-11.013')
    add('payload_length_plus1', dict(man_patch=lambda mm, p: struct.pack_into('<I', mm, 0x98, len(p) + 1)),
        reason='LENGTH', req='S-11.014')
    add('payload_length_minus1', dict(man_patch=lambda mm, p: struct.pack_into('<I', mm, 0x98, len(p) - 1)),
        reason='LENGTH', req='S-11.014')
    add('payload_appended_byte', dict(post=lambda b: bytes(b) + b'\0'), reason='LENGTH', req='S-11.014')
    add('payload_hash_flip', dict(man_patch=lambda mm, p: mm.__setitem__(0x9C, mm[0x9C] ^ 1)), reason='HASH',
        req='S-11.014')
    # checks 9-11 apply with the option on too (S-11.022)
    add('manifest_product_bad_option', dict(mprod=0x88000), u=dict(option=True), reason='SKU', req='S-11.022')
    add('payload_length_plus1_option', dict(man_patch=lambda mm, p: struct.pack_into('<I', mm, 0x98, len(p) + 1)),
        u=dict(option=True), reason='LENGTH', req='S-11.022')
    add('payload_hash_flip_option', dict(man_patch=lambda mm, p: mm.__setitem__(0x9C, mm[0x9C] ^ 1)),
        u=dict(option=True), reason='HASH', req='S-11.022')
    add('key_00', dict(key_index=0x00), req='S-11.015')
    add('key_00_nokey', dict(key_index=0x00), u=dict(k0=False), reason='NOKEY', req='S-11.023')
    add('key_01', dict(key_index=0x01), reason='KEY', req='S-11.023')
    add('key_01_dev', dict(key_index=0x01), u=dict(dev=True), reason='KEY', req='S-11.023')
    add('key_01_option', dict(key_index=0x01), u=dict(option=True), req='S-11.023')
    add('key_01_option_nokey', dict(key_index=0x01), u=dict(option=True, k1=False), reason='NOKEY', req='S-11.023')
    add('key_fe', dict(key_index=0xFE), req='S-11.015')
    add('key_ff', dict(key_index=0xFF, key=AES0), reason='KEY', req='S-11.023')
    add('key_ff_option_dev', dict(key_index=0xFF, key=AES0), u=dict(option=True, dev=True), reason='KEY',
        req='S-11.023')
    add('key_02', dict(key_index=0x02, key=AES0), reason='KEY', req='S-11.015')
    add('name_firmware_zip', dict(name=b'op-xy_1.2.0.zip'), req='S-11.017')
    add('name_no_zip', dict(name=b'firmware.bin'), reason='NAME', req='S-11.017')
    add('name_zip_after_nul', dict(name=b'a\0b.zip'), reason='NAME', req='S-11.017')
    add('name_zip_at_7a', dict(name=b'x' * 0x7A + b'.zip'), reason='NAME', req='S-11.017')
    add('name_zip_at_78', dict(name=b'x' * 0x78 + b'.zip'), req='S-11.017')
    add('name_upper_ZIP', dict(name=b'FIRMWARE.ZIP'), reason='NAME', req='S-11.017')
    add('name_encrypted', dict(key_index=0x00, name=b'no-extension'), reason='NAME', req='S-11.017')
    add('ziplen_exact', dict(zip_bytes=BASE_ZIP + bytes(-len(BASE_ZIP) % 0x80)), req='S-11.018')
    add('ziplen_over_by_1', dict(zip_bytes=BASE_ZIP + bytes(-len(BASE_ZIP) % 0x80),
                                 zip_length=len(BASE_ZIP) + (-len(BASE_ZIP) % 0x80) + 1), reason='ZIPLEN',
        req='S-11.018')
    add('ziplen_enc_needs_block', dict(key_index=0x00, zip_bytes=BASE_ZIP + bytes(-len(BASE_ZIP) % 0x80),
                                       zip_length=len(BASE_ZIP) + (-len(BASE_ZIP) % 0x80) + 1),
        reason='ZIPLEN', req='S-11.018')
    add('ziplen_zero', dict(zip_length=0), reason='ZIP', req='S-11.018')
    add('zip_garbage', dict(zip_bytes=bytes(range(200))), reason='ZIP', req='S-11.018')
    add('enc_chain', dict(key_index=0x00), req='S-11.016')
    add('enc_split_chain', dict(key_index=0x00, split_chain=True), req='S-11.016', zip_reason='MEMBER')
    add('enc_trailing', dict(key_index=0x00, payload_extra=bytes(0x123)), req='S-11.016')
    add('multi_fault_crc_and_tag', dict(tag=b'XXXX', fix_crc=False), reason='CRC', req='S-11.020')
    add('multi_fault_sku_and_hash', dict(mprod=0, man_patch=lambda mm, p: mm.__setitem__(0x9C, mm[0x9C] ^ 1)),
        reason='SKU', req='S-11.020')
    add('version_header_vs_file', dict(zip_bytes=fwzip.build({'os': os_image(20000)}, version=(9, 9, 9), build=9)),
        req='S-11.038', version=[1, 2, 0, 0], recorded_version=[9, 9, 9, 9])
    # A TE-signed package with its TE header version rewritten: accepted, TE-signed and forced; the recorded
    # version is the zip's.
    add('header_version_tampered_te_signed',
        dict(final=lambda b: bytes(b[:0x4B] + struct.pack('>4H', 0, 1, 0, 0) + b[0x53:])),
        req='S-11.038', te_signed=True, forced=True, version=[0, 1, 0, 0], recorded_version=[1, 2, 0, 0])
    add('recorded_version_file_absent', dict(zip_bytes=zipb({'firmware/blackfin/bin/os.ldr': os_image(5000)})),
        req='S-11.038', version=[1, 2, 0, 0], recorded_version=None)
    add('forced_by_name', None, fname='op-xy_force.tfw', req='S-11.047', forced=True)
    add('not_forced_upper', None, fname='op-xy_FORCE.tfw', req='S-11.047', forced=False)
    add('cdc_forced', dict(pre=False), form='body', link='cdc', req='S-11.050', forced=True)
    add('midi_not_forced', dict(pre=False), form='body', link='midi', req='S-11.050', forced=False)
    return C


def build_case(c):
    if c['case'] == 'short_file':
        return assemble()[:0x30]
    if c['case'] == 'short_body':
        return assemble(pre=False)[:0x20]
    return assemble(**c['kw'])


def write_packages():
    os.makedirs(PKG, exist_ok=True)
    for f in os.listdir(PKG):
        os.remove(os.path.join(PKG, f))
    out = []
    for c in package_cases():
        data = build_case(c)
        u = unit(**c['unit'])
        got = m.check_package(data, c['form'], u, file_name=c['file_name'], link=c['link'])
        assert got['reason'] == c['reason'], (c['case'], got['reason'], c['reason'])
        zr, staged = m.check_zip(got['zip'], 1 << 30) if got['accepted'] else (None, None)
        assert zr == c['exp'].get('zip_reason'), (c['case'], zr)
        if got['accepted'] and zr is None:
            # from construction: every package not listed otherwise carries BASE_ZIP's firmware.version 1.2.0+0
            c['exp'].setdefault('recorded_version', [1, 2, 0, 0])
            pv = m.package_version(staged)
            assert (None if pv is None else list(pv)) == c['exp']['recorded_version'], c['case']
        for k, v in c['exp'].items():
            if k in ('zip_reason', 'recorded_version'):
                continue
            gv = list(got[k]) if k == 'version' else got[k]
            assert gv == v, (c['case'], k, gv, v)
        fn = c['case'] + '.bin'
        with open(os.path.join(PKG, fn), 'wb') as f:
            f.write(data)
        exp = dict(accepted=c['reason'] is None, reason=c['reason'])
        exp.update({k: v for k, v in c['exp'].items()})
        exp['zip_reason'] = zr
        if c['reason'] is None:
            exp['zip_sha256'] = hashlib.sha256(got['zip']).hexdigest()
        out.append(dict(case=c['case'], file='pkg/' + fn, form=c['form'], link=c['link'],
                        file_name=c['file_name'], unit=ujson(u), expect=exp, requirement=c['req']))
    return dict(keys='images.tfw.test_keys(): cloud key Q0 = its P-224 public key, K0 = its AES key; '
                     "K1 = images.tfw.test_keys(b'opxy-conformance-k1') AES key",
                cases=out, disk_cases=disk_cases())


# ---- the disk path end to end (S-11.046-S-11.048): which file is read, and which zip is staged

OS_A = os_image(256 * 48)                       # BASE_ZIP's OS, in every package built here
OS_B = os_image(0x3000, fill=b'\xB0')                        # the decoy's OS
DECOY_ZIP = fwzip.build({'os': OS_B}, version=(9, 0, 0))
DISK_CASES = [
    # case, root entries in directory order (name, file under vectors/), package taken, its reason
    ('decoy_zip_before', [('firmware_a.zip', 'pkg/decoy_firmware.zip'), ('op-xy.tfw', 'pkg/form_file_ok.bin')],
     'op-xy.tfw', None),
    ('decoy_firmware_zip_present', [('firmware.zip', 'pkg/decoy_firmware.zip'),
                                    ('op-xy.tfw', 'pkg/form_file_ok.bin')], 'op-xy.tfw', None),
    ('decoy_zip_alone', [('firmware.zip', 'pkg/decoy_firmware.zip')], None, None),
    ('decoy_zip_named_tfw', [('firmware_tfw.zip', 'pkg/decoy_firmware.zip'),
                             ('op-xy.tfw', 'pkg/form_file_ok.bin')], 'firmware_tfw.zip', 'PREAMBLE'),
]


def disk_cases():
    with open(os.path.join(PKG, 'decoy_firmware.zip'), 'wb') as f:
        f.write(DECOY_ZIP)
    out = []
    for case, ents, want, reason in DISK_CASES:
        files = []
        for n, ref in ents:
            with open(os.path.join(V, ref), 'rb') as f:
                files.append((n, False, f.read()))
        name, got = m.disk_install(files, unit())
        assert name == want, (case, name)
        installs = want is not None and reason is None
        if want is not None:
            assert got['reason'] == reason, (case, got['reason'])
        exp = dict(package=want, reason=reason,
                   zip_sha256=hashlib.sha256(BASE_ZIP).hexdigest() if installs else None,
                   os_sha256=hashlib.sha256(OS_A).hexdigest() if installs else None,
                   decoy_os_sha256=hashlib.sha256(OS_B).hexdigest())
        if installs:
            assert got['zip'] == BASE_ZIP, case
            assert m.components(m.check_zip(got['zip'], 1 << 30)[1])['os'][0] == OS_A, case
        out.append(dict(case=case, entries=[[n, ref] for n, ref in ents], expect=exp, requirement='S-11.048'))
    return out


# ---- zips (S-11.030-S-11.035)

def patch_flag(z, name, bit):
    b = bytearray(z)
    nb = name.encode()
    for sig, flag_off, name_off in ((b'PK\x03\x04', 6, 30), (b'PK\x01\x02', 8, 46)):
        i = 0
        while True:
            i = b.find(sig, i)
            if i < 0:
                break
            if b[i + name_off:i + name_off + len(nb)] == nb:
                b[i + flag_off] |= bit
            i += 4
    return bytes(b)


def patch_crc(z, name):
    b = bytearray(z)
    nb = name.encode()
    for sig, crc_off, name_off in ((b'PK\x03\x04', 14, 30), (b'PK\x01\x02', 16, 46)):
        i = 0
        while True:
            i = b.find(sig, i)
            if i < 0:
                break
            if b[i + name_off:i + name_off + len(nb)] == nb:
                b[i + crc_off] ^= 0xFF
            i += 4
    return bytes(b)


def zip64_zip():
    b = io.BytesIO()
    with zipfile.ZipFile(b, 'w') as z:
        with z.open('firmware/firmware.version', 'w', force_zip64=True) as f:
            f.write(b'1.0.0+0\n')
    return b.getvalue()


def corrupt_deflate():
    z = zipb({'firmware/blackfin/bin/os.ldr': bytes(range(256)) * 64})
    b = bytearray(z)
    i = b.find(b'PK\x03\x04') + 30 + len('firmware/blackfin/bin/os.ldr') + 5
    for k in range(i, i + 40):
        b[k] ^= 0x5A
    return bytes(b)


def zip_cases():
    V1 = b'1.0.0+0\n'
    big = 1 << 30
    C = []

    def add(case, z, reason=None, free=big, req='', **exp):
        C.append(dict(case=case, z=z, reason=reason, free=free, req=req, exp=exp))
    add('stored', zipb({'firmware/firmware.version': V1, 'firmware/blackfin/bin/os.ldr': os_image(12000)},
                       zipfile.ZIP_STORED), req='S-11.030', components=['os'])
    add('deflated', zipb({'firmware/firmware.version': V1, 'firmware/blackfin/bin/os.ldr': os_image(12000)}),
        req='S-11.030', components=['os'])
    zm = io.BytesIO()
    with zipfile.ZipFile(zm, 'w') as z:
        z.writestr('firmware/blackfin/bin/os.ldr', os_image(12000), zipfile.ZIP_STORED)
        z.writestr('firmware/ble/bin/ble.bin', b'b' * 3000, zipfile.ZIP_DEFLATED)
    add('mixed', zm.getvalue(), req='S-11.030', components=['ble', 'os'])
    add('bzip2_member', zipb({'firmware/firmware.version': V1}, zipfile.ZIP_BZIP2), 'MEMBER', req='S-11.030')
    add('lzma_member', zipb({'firmware/firmware.version': V1}, zipfile.ZIP_LZMA), 'MEMBER', req='S-11.030')
    add('zip64', zip64_zip(), 'MEMBER', req='S-11.030')
    add('encrypted_member', patch_flag(zipb({'firmware/firmware.version': V1}), 'firmware/firmware.version', 1),
        'MEMBER', req='S-11.030')
    add('bad_member_crc', patch_crc(zipb({'firmware/firmware.version': V1}), 'firmware/firmware.version'), 'MEMBER',
        req='S-11.030')
    add('truncated_deflate', corrupt_deflate(), 'MEMBER', req='S-11.030')
    add('members_reordered', zipb({'firmware/blackfin/bin/os.ldr': os_image(12000), 'firmware/firmware.version': V1}),
        req='S-11.030', components=['os'])
    add('name_dotdot', zipb({'firmware/../user/x': b'x'}), 'MEMBER', req='S-11.031')
    add('name_absolute', zipb({'/firmware/x': b'x'}), 'MEMBER', req='S-11.031')
    add('name_backslash', zipb({'firmware\\x': b'x'}), 'MEMBER', req='S-11.031')
    add('name_dot', zipb({'./firmware/blackfin/bin/os.ldr': b'o' * 12000}), 'MEMBER', req='S-11.031')
    add('name_empty_component', zipb({'firmware//blackfin/bin/os.ldr': b'o' * 12000}), 'MEMBER',
        req='S-11.031')
    add('outside_firmware', zipb({'readme.txt': b'hi', 'firmware/blackfin/bin/os.ldr': os_image(12000)}),
        req='S-11.031', components=['os'], staged=['firmware/blackfin/bin/os.ldr', 'readme.txt'])
    zd = io.BytesIO()
    with zipfile.ZipFile(zd, 'w') as z:
        z.writestr('firmware/', b'')
        z.writestr('firmware/blackfin/', b'')
        z.writestr('firmware/blackfin/bin/', b'')
        z.writestr('firmware/blackfin/bin/os.ldr', os_image(12000))
    add('with_dir_entries', zd.getvalue(), req='S-11.032', components=['os'])
    sz = zipb({'firmware/blackfin/bin/os.ldr': os_image(12000)})
    add('space_equal', sz, free=12000 + 0x400000, req='S-11.033', components=['os'])
    add('space_one_less', sz, 'SPACE', free=12000 + 0x400000 - 1, req='S-11.033')
    add('space_query_fails', sz, 'SPACEQUERY', free=None, req='S-11.033')
    allc = {n: b'\x5a' * 100 for n in m.COMPONENTS}
    allc['bootloader'] = BOOT_STREAM
    allc['os'] = os_image(100)
    files = {img: allc[n] for n, (img, ver, _) in m.COMPONENTS.items()}
    files.update({ver: V1 for n, (img, ver, _) in m.COMPONENTS.items()})
    files['firmware/blackfin/content/op1.json'] = b'{}'
    add('all_components', zipb(files), req='S-11.034', components=sorted(m.COMPONENTS))
    add('empty_image', zipb({'firmware/blackfin/bin/os.ldr': b'', 'firmware/ble/bin/ble.bin': b'b'}),
        req='S-11.034', components=['ble'])
    head = {'bootloader': BOOT_STREAM, 'os': os_image(32)}
    for n, (img, ver, lim) in m.COMPONENTS.items():
        h = head.get(n, b'')   # a bootable stream padded after FINAL (S-02.091)
        add('limit_%s_at' % n, zipb({img: h + bytes(lim - len(h))}), req='S-11.035', components=[n])
        add('limit_%s_over' % n, zipb({img: h + bytes(lim + 1 - len(h))}), 'SIZE', req='S-11.035')
    # S-11.034: a first-stage image is no component: staged as a plain member, never checked or limited
    fs = 'firmware/blackfin/bin/spiboot.ldr'
    add('first_stage_not_a_component', zipb({fs: b'\x5a' * 0xC001, 'firmware/blackfin/bin/spiboot.version': V1,
                                            'firmware/blackfin/bin/os.ldr': os_image(32)}),
        req='S-11.034', components=['os'])
    # check 18 (S-11.035, S-02.091): the bootloader must be a bootable stream
    V = {'firmware/firmware.version': V1, 'firmware/blackfin/bin/os.ldr': os_image(12000)}
    bl = m.COMPONENTS['bootloader'][0]
    add('stream_bootloader_ok', zipb(dict(V, **{bl: BOOT_STREAM})), req='S-11.035', components=['bootloader', 'os'])
    add('stream_bootloader_not_ldr', zipb(dict(V, **{bl: b'\x5a' * 100})), 'STREAM', req='S-11.035')
    add('stream_bootloader_no_init', zipb(dict(V, **{bl: boot_stream(inits=0)})), 'STREAM', req='S-11.035')
    add('stream_bootloader_ddr_before_init', zipb(dict(V, **{bl: boot_stream(pre_ddr=True)})), 'STREAM',
        req='S-11.035')
    add('stream_bootloader_rom_boot_reserve', zipb(dict(V, **{bl: boot_stream(reserve=True)})), 'STREAM',
        req='S-11.035')
    add('stream_bootloader_bcode', zipb(dict(V, **{bl: boot_stream(code=3)})), 'STREAM', req='S-11.035')
    add('stream_bootloader_no_final', zipb(dict(V, **{bl: BOOT_STREAM[:-16]})), 'STREAM', req='S-11.035')
    add('stream_size_first', zipb(dict(V, **{bl: b'\x5a' * 0x78001})), 'SIZE', req='S-11.035')
    # check 18 for the OS image (S-05.024), refused before the run
    os_img = m.COMPONENTS['os'][0]
    L = ldr_mod()
    add('stream_os_ok', zipb({os_img: os_image(4096, regions=[(0x80100000, bytes(64))])}), req='S-11.035',
        components=['os'])
    add('stream_os_not_ldr', zipb({os_img: b'o' * 12000}), 'STREAM', req='S-11.035')
    add('stream_os_no_final', zipb({os_img: os_image(32)[:16] + bytes(16)}), 'STREAM', req='S-11.035')
    add('stream_os_staging', zipb({os_img: os_image(4096, regions=[(0x8A000000, bytes(64))])}), 'STREAM',
        req='S-11.035')
    add('stream_os_init', zipb({os_img: L.build_ldr([{'flags': L.INIT, 'target': 0x11A00000, 'count': 0},
                                                      {'flags': L.FINAL, 'target': 0x11A00000, 'count': 0}])}),
        'STREAM', req='S-11.035')
    return C


def boot_stream(inits=1, pre_ddr=False, reserve=False, code=4):
    L = ldr_mod()
    b = [{'flags': L.FIRST | L.IGNORE, 'target': 0x11A00000, 'count': 0, 'dmacode': code},
         {'target': 0x11800000, 'payload': bytes(range(32))},
         {'target': 0x11A00000, 'payload': bytes(range(64))}]
    if pre_ddr:
        b.append({'target': 0x8803D000, 'payload': bytes(8)})
    b += [{'flags': L.INIT, 'target': 0x11A00000, 'count': 0}] * inits
    b += [{'flags': L.FIRST | L.IGNORE, 'target': 0x11A00000, 'count': 0},
          {'target': 0x8803D000, 'payload': bytes(range(48))}]
    if reserve:
        b.append({'target': 0x08000800, 'payload': bytes(8)})
    b += [{'flags': L.FINAL, 'target': 0x11A00000, 'count': 0}]
    return L.build_ldr(b)


def ldr_mod():
    from images import ldr
    return ldr


BOOT_STREAM = boot_stream()


def write_zips():
    out = []
    for c in zip_cases():
        reason, staged = m.check_members(c['z'], c['free'])
        assert reason == c['reason'], (c['case'], reason, c['reason'])
        exp = dict(reason=c['reason'])
        if reason is None:
            comps = sorted(m.components(staged))
            if 'components' in c['exp']:
                assert comps == c['exp']['components'], (c['case'], comps)
            exp['components'] = comps
            if 'staged' in c['exp']:
                got = sorted(p for p, b in staged.items() if b is not None)
                assert got == c['exp']['staged'], (c['case'], got)
                exp['staged'] = got
        out.append(dict(case=c['case'], zip_hex=c['z'].hex(), free_space=c['free'], expect=exp,
                        requirement=c['req']))
    return dict(cases=out, space_arithmetic=[
        dict(sizes=[0x7FFFFFFF, 0x7FFFFFFF], free=0xFFFFFFFF, reason='SPACE'),
        dict(sizes=[0x80000000], free=0x80000000 + 0x400000, reason=None),
        dict(sizes=[0x80000000], free=0x80000000 + 0x3FFFFF, reason='SPACE'),
        dict(sizes=[], free=0x400000, reason=None),
        dict(sizes=[1], free=None, reason='SPACEQUERY')])


# ---- versions, product words, CRC, progress (S-11.007, .008, .036, .037, .062)

def versions():
    V = [
        ('312e322e302b304061326539326333370a', [1, 2, 0, 0]),
        (b'1000.0.20+0@a27e4f05a893ec8bd4a473c79f809aca5e34d38d\n'.hex(), [1000, 0, 20, 0]),
        (b'1.2.3'.hex(), [1, 2, 3, 0]), (b'1.2.3+'.hex(), [1, 2, 3, 0]), (b'1.2'.hex(), [1234, 0, 0, 0]),
        (b'abc'.hex(), [1234, 0, 0, 0]), ('', [1234, 0, 0, 0]), (None, [1234, 0, 0, 0]),
        (b'70000.1.2+3'.hex(), [70000 % 65536, 1, 2, 3]), (b' 1. 2.\t3+ 4\n'.hex(), [1, 2, 3, 4]),
        (b'a.b.c+d'.hex(), [0, 0, 0, 0]),
        (b'1.2.3@abcdefabcdefabcdefabcdefabcdef+9'.hex(), [1, 2, 3, 0]),
        (b'1.2.3@abcdefabcdefabcdefabcdef+9'.hex(), [1, 2, 3, 9]),
        (b'1.2.3+4@x'.hex(), [1, 2, 3, 4]),
        ((b'1' * 33 + b'.2.3').hex(), [1234, 0, 0, 0]),
    ]
    out = []
    for h, want in V:
        got = list(m.version_from_text(None if h is None else bytes.fromhex(h)))
        assert got == want, (h, got, want)
        out.append(dict(file_hex=h, version=want))
    cmp = [([1, 2, 0, 0], [1, 2, 0, 0], 0), ([1, 2, 0, 1], [1, 2, 0, 0], 1), ([1, 2, 0, 0], [1, 10, 0, 0], -1),
           ([1000, 0, 20, 0], [1000, 0, 21, 0], -1), ([2, 0, 0, 0], [1, 65535, 65535, 65535], 1),
           ([1234, 0, 0, 0], [1000, 0, 20, 0], 1)]
    for a, b, r in cmp:
        assert m.vcmp(a, b) == r
    return dict(parse=out, compare=[dict(a=a, b=b, result=r) for a, b, r in cmp])


def product_words():
    W = [(0x00084000, True), (0x000843FF, True), (0xFF084000, True), (0x00084400, False), (0x00088000, False),
         (0, False), (0x00FF8000, False), (0x00080000 | (0x21 << 14) | 0x10000000, True)]
    for w, ok in W:
        assert m.product_ok(w) == ok, hex(w)
    return [dict(word='%08X' % w, passes=ok) for w, ok in W]


def crc_vectors():
    V = [(b'123456789', 0x31C3), (b'', 0x0000), (b'\x00', 0x0000), (b'\xff', 0x1EF0), (b'A', 0x58E5)]
    for d, c in V:
        assert m.crc16(d) == c, (d, hex(m.crc16(d)))
    return [dict(data_hex=d.hex(), crc16='%04X' % c) for d, c in V]


def progress_vectors():
    # S-11.062: the second byte is always 0
    return [dict(step=s, count=0, file_hex=bytes([s, 0]).hex()) for s in range(8)]


# ---- content (S-11.039, .040, .056, .075-.079)

def content_cases():
    base = {'content/op1.json': b'old', 'content/wavetables/a.mip': b'w', 'content/dlc/A/manifest.json':
            b'{"name":"Pack A"}', 'content/dlc/A/a1': b'1', 'content/dlc/A/a2': b'2', 'content/dlc/B/b1': b'b',
            'firmware.version': b'1.0.0+0\n', 'user/song.xy': b'u'}
    CD = m.CONTENT_DIR
    C = []

    def add(case, staged, co, result, after, req, tree=None):
        C.append(dict(case=case, staged=staged, content_only=co, result=result,
                      tree_before=tree or base, tree_after=after, req=req))
    full = {CD + 'op1.json': b'new', 'firmware/firmware.version': b'1.2.0+0\n',
            'firmware/blackfin/bin/os.ldr': os_image(32)}
    add('full_keeps_dlc', full, False, 'OK',
        {'content/op1.json': b'new', 'content/dlc/A/manifest.json': base['content/dlc/A/manifest.json'],
         'content/dlc/A/a1': b'1', 'content/dlc/A/a2': b'2', 'content/dlc/B/b1': b'b',
         'firmware.version': b'1.2.0+0\n', 'user/song.xy': b'u'}, 'S-11.075')
    nodlc = {k: v for k, v in base.items() if not k.startswith('content/dlc/')}
    add('full_without_dlc', full, False, 'OK',
        {'content/op1.json': b'new', 'firmware.version': b'1.2.0+0\n', 'user/song.xy': b'u'}, 'S-11.075', nodlc)
    part = {CD + 'op1.json': b'new', CD + 'partial_upgrade': b'', 'firmware/firmware.version': b'1.2.0+0\n',
            'firmware/blackfin/bin/os.ldr': os_image(32)}
    add('partial_removes_nothing', part, False, 'OK',
        dict(base, **{'content/op1.json': b'new', 'firmware.version': b'1.2.0+0\n'}), 'S-11.076')
    pack = {CD + 'dlc/A/manifest.json': b'{"name":"Pack A v2"}', CD + 'dlc/A/a1': b'1new',
            CD + 'dlc/partial_upgrade': b''}
    add('pack_replace_partial', pack, True, 'OK',
        {k: v for k, v in base.items() if not k.startswith('content/dlc/A/')} |
        {'content/dlc/A/manifest.json': b'{"name":"Pack A v2"}', 'content/dlc/A/a1': b'1new'}, 'S-11.077')
    packnm = {CD + 'dlc/A/manifest.json': b'{"name":"Pack A v2"}', CD + 'dlc/A/a1': b'1new'}
    add('pack_without_marker_wipes_content', packnm, True, 'OK',
        {'content/dlc/A/manifest.json': b'{"name":"Pack A v2"}', 'content/dlc/A/a1': b'1new',
         'content/dlc/A/a2': b'2', 'content/dlc/B/b1': b'b', 'firmware.version': b'1.0.0+0\n',
         'user/song.xy': b'u'}, 'S-11.075')
    add('partial_marker_dir', {CD + 'x/partial_upgrade/readme': b'', CD + 'op1.json': b'new',
                               'firmware/blackfin/bin/os.ldr': os_image(32)}, False, 'OK',
        dict(base, **{'content/op1.json': b'new'}), 'S-11.040')
    add('marker_outside_content', {'firmware/partial_upgrade': b'', CD + 'op1.json': b'new',
                                   'firmware/blackfin/bin/os.ldr': os_image(32)}, False, 'OK',
        {'content/op1.json': b'new', 'content/dlc/A/manifest.json': base['content/dlc/A/manifest.json'],
         'content/dlc/A/a1': b'1', 'content/dlc/A/a2': b'2', 'content/dlc/B/b1': b'b',
         'firmware.version': b'1.0.0+0\n', 'user/song.xy': b'u'}, 'S-11.040')
    dlc_kept = {k: v for k, v in base.items() if not k.startswith('content/') or k.startswith('content/dlc/')}
    add('dotfile', {CD + '.a': b'dot', CD + 'op1.json': b'n', 'firmware/blackfin/bin/os.ldr': os_image(32)}, False,
        'OK', dict(dlc_kept, **{'content/.a': b'dot', 'content/op1.json': b'n'}), 'S-11.079')
    add('nested_dirs', {CD + 'x/y/z.bin': b'z', 'firmware/blackfin/bin/os.ldr': os_image(32)}, False, 'OK',
        dict(dlc_kept, **{'content/x/y/z.bin': b'z'}), 'S-11.079')
    add('only_marker_not_found', {CD + 'partial_upgrade': b'', 'firmware/blackfin/bin/os.ldr': os_image(32)}, False,
        'Not found', base, 'S-11.066')
    add('no_content_dir', {'firmware/blackfin/bin/os.ldr': os_image(32), 'firmware/firmware.version': b'9.9.9+9\n'},
        False, 'Not found', base, 'S-11.078')
    out = []
    for c in C:
        r, after = m.install_content(c['tree_before'], c['staged'], c['content_only'])
        assert m.RESULT_NAMES[r] == c['result'], (c['case'], r)
        assert after == c['tree_after'], (c['case'], sorted(set(after.items()) ^ set(c['tree_after'].items())))
        hx = lambda d: {k: (v.hex() if v is not None else None) for k, v in sorted(d.items())}  # noqa: E731
        out.append(dict(case=c['case'], staged=hx(c['staged']), content_only=c['content_only'],
                        tree_before=hx(c['tree_before']), result=c['result'],
                        tree_after=hx(c['tree_after']), requirement=c['req']))
    names = [
        ('pack_name_one', {m.DLC_DIR + 'A/manifest.json': b'{"name": "Drums"}', m.DLC_DIR + 'A/k': b'1'}, 'Drums'),
        ('pack_name_two', {m.DLC_DIR + 'B/manifest.json': b'{"name": "Bass"}',
                           m.DLC_DIR + 'A/manifest.json': b'{"name": "Arp"}'}, 'Arp'),
        ('pack_name_no_manifest_first', {m.DLC_DIR + 'A/k': b'1', m.DLC_DIR + 'B/manifest.json':
                                         b'{"name": "Bass"}'}, None),
        ('pack_name_missing_name', {m.DLC_DIR + 'A/manifest.json': b'{"title": "x"}'}, ''),
        ('pack_name_long', {m.DLC_DIR + 'A/manifest.json': b'{"name": "' + b'L' * 100 + b'"}'}, 'L' * 100),
        ('pack_name_late', {m.DLC_DIR + 'A/manifest.json': b'{"pad": "' + b'p' * 600 + b'", "name": "Late"}'},
         'Late'),
        ('pack_name_over_4096', {m.DLC_DIR + 'A/manifest.json': b'{"pad": "' + b'p' * 5000 + b'", "name": "Late"}'},
         'Late'),
    ]
    also = {'pack_name_over_4096': ['']}
    nout = []
    for case, st, want in names:
        got = m.pack_name(st)
        if want is None:
            want = 'Bass'
        assert got == want, (case, got)
        row = dict(case=case, staged={k: v.hex() for k, v in st.items()}, name=want,
                   note='listing order taken as name order in this vector')
        if case in also:
            row['also_conforming'] = also[case]
            row['note'] += '; a manifest over 4096 bytes may be read in its first 4096 bytes only (S-11.039)'
        nout.append(row)
    return dict(installs=out, pack_names=nout)


# ---- tables

def write_tsv(name, header, rows, comment):
    with open(os.path.join(HERE, name), 'w', newline='') as f:
        f.write('# ' + comment + '\n')
        w = csv.writer(f, delimiter='\t', lineterminator='\n')
        w.writerow(header)
        w.writerows(rows)


def check_acceptance(rows):
    """Build a package per distinct (sig, valid, kidx) and run the model under each unit."""
    cache = {}
    for sig, valid, q0, kidx, k0, k1, opt, dev, te, verdict in rows:
        key = (sig, valid, kidx)
        if key not in cache:
            ki = int(kidx, 16)
            cache[key] = assemble(sig_index=int(sig, 16), sign_d=D if valid else D_OTHER, key_index=ki,
                                  key=AES0 if ki in (0xFF, 0x02) else None)
        u = m.Unit(PUB if q0 else None, AES0 if k0 else None, AES1 if k1 else None, bool(opt), bool(dev))
        got = m.check_package(cache[key], 'file', u, file_name='p.tfw')
        assert (got['reason'] or 'accept') == verdict, (sig, valid, q0, kidx, k0, k1, opt, dev, got['reason'])
        if got['reason'] in (None, 'KEY', 'NOKEY'):
            assert int(got['te_signed']) == te


def step_rows():
    R = m.RESULT_NAMES
    rows = []
    # content; S-11.096: a step not selected does nothing (here and below)
    for cd, files, fail, sel in itertools.product([0, 1], [0, 3], [0, 1], [1, 0]):
        if fail and not files:
            continue
        c = dict(content_dir=cd, files=files, move_fails=fail, selected=sel)
        exp = ('Not found' if not cd else 'Skipped' if not sel else
               'Failure' if fail else ('OK' if files else 'Not found'))
        rows.append(['content', json.dumps(c, sort_keys=True), exp])
    for p, ok, opt, sel in itertools.product([0, 1], [1, 0], [0, 1], [1, 0]):
        c = dict(present=p, write_ok=ok, option=opt, selected=sel)
        exp = 'Not found' if not p else 'Skipped' if not sel else ('OK' if ok else 'Internal error')
        rows.append(['os', json.dumps(c, sort_keys=True), exp])
    # S-11.090, S-11.096: the bootloader is written only with the option on and the step selected
    opt_sel = [(0, 0), (0, 1), (1, 0), (1, 1)]
    for p, (opt, sel), rel, forced, dev, ok in itertools.product([0, 1], opt_sel, ['older', 'equal', 'newer'],
                                                                 [0, 1], [0, 1], [1, 0]):
        c = dict(present=p, option=opt, selected=sel, rel=rel, forced=forced, dev=dev, write_ok=ok)
        if not p:
            exp = 'Not found'
        elif not opt or not sel:
            exp = 'Skipped'
        else:
            # S-11.068: no version test under the option
            exp = 'OK' if ok else 'Failure'
        rows.append(['bootloader', json.dumps(c, sort_keys=True), exp])
    # S-11.068 items 4 and 5: a start S-02.090 refuses, for `verified` alone or for a bad selected slot,
    # whether this power-on's own bootloader step wrote the selector read, and whether the selected slot's
    # record has the checked image's length and CRC-32
    for (opt, sel), start, here, match in itertools.product(opt_sel, ['refused_verified', 'refused_bad'], [0, 1],
                                                            [0, 1]):
        c = dict(present=1, option=opt, selected=sel, rel='newer', forced=0, dev=0, write_ok=1, start=start,
                 written_here=here, slot_matches=match)
        if not opt or not sel:
            exp = 'Skipped'
        elif start == 'refused_verified' and here and match:
            exp = 'Skipped'
        else:
            exp = 'Failure'
        rows.append(['bootloader', json.dumps(c, sort_keys=True), exp])
    # S-11.057: the reserved step 6 reports Not found, whatever the option and the selection
    for opt, sel in opt_sel:
        rows.append(['reserved', json.dumps(dict(option=opt, selected=sel), sort_keys=True), 'Not found'])
    # S-11.069-S-11.071 with the step switched on; S-11.073 with it off: Not found or Skipped, nothing else;
    # Already installed comes before the selection
    for on, p, rel, forced, dfu, ok, sel in itertools.product([1, 0], [0, 1], ['equal', 'newer'], [0, 1], [1, 0],
                                                              [1, 0], [1, 0]):
        c = dict(switched_on=on, present=p, rel=rel, forced=forced, in_dfu=dfu, write_ok=ok, selected=sel)
        exp = ('Not found' if not p else 'Skipped' if not on else
               'Already installed' if rel == 'equal' and not forced else 'Skipped' if not sel else
               'Failure' if not dfu else 'OK' if ok else 'Failure')
        rows.append(['ble', json.dumps(c, sort_keys=True), exp])
    for on, p0, p1, rel, forced, slot, ok, sel in itertools.product([1, 0], [0, 1], [0, 1], ['equal', 'newer'],
                                                                   [0, 1], [0, 1, 255], [1, 0], [1, 0]):
        c = dict(switched_on=on, present0=p0, present1=p1, rel=rel, forced=forced, slot=slot, write_ok=ok,
                 selected=sel)
        if not p0 and not p1:
            exp = 'Not found'
        elif not on:
            exp = 'Skipped'
        elif rel == 'equal' and not forced:
            exp = 'Already installed'
        elif not sel:
            exp = 'Skipped'                     # no slot query
        elif slot not in (0, 1):
            exp = 'Failure'
        elif not (p0, p1)[slot]:
            exp = 'Not found'
        else:
            exp = 'OK' if ok else 'Failure'
        rows.append(['ble bootloader', json.dumps(c, sort_keys=True), exp])
    # S-11.070 item 7: the step's own DFU-mode queries, after the slot and the check
    for slot, ok in itertools.product([0, 1, 255], [1, 0]):
        c = dict(switched_on=1, present0=1, present1=1, rel='newer', forced=0, slot=slot, in_dfu=0, write_ok=ok,
                 selected=1)
        rows.append(['ble bootloader', json.dumps(c, sort_keys=True), 'Failure'])
    for on, p, rel, forced, ok, sel in itertools.product([1, 0], [0, 1], ['equal', 'newer'], [0, 1], [1, 0],
                                                         [1, 0]):
        c = dict(switched_on=on, present=p, rel=rel, forced=forced, write_ok=ok, selected=sel)
        exp = ('Not found' if not p else 'Skipped' if not on else
               'Already installed' if rel == 'equal' and not forced else 'Skipped' if not sel else
               'OK' if ok else 'Failure')
        rows.append(['keyboard', json.dumps(c, sort_keys=True), exp])
    # S-11.095: the check of a staged image, where the step would write, and not where it ends earlier
    for staged, bad in (('bad', 'Bad file'), ('read_error', 'Internal error')):
        for step, c, exp in [
                ('os', dict(present=1, option=0, write_ok=1, selected=1), bad),
                ('os', dict(present=1, option=0, write_ok=0, selected=1), bad),
                ('os', dict(present=0, option=0, write_ok=1, selected=1), 'Not found'),
                ('os', dict(present=1, option=0, write_ok=1, selected=0), 'Skipped'),
                ('bootloader', dict(present=1, option=1, selected=1, rel='newer', forced=0, dev=0, write_ok=1),
                 bad),
                ('bootloader', dict(present=1, option=0, selected=0, rel='newer', forced=0, dev=0, write_ok=1),
                 'Skipped'),
                ('bootloader', dict(present=1, option=1, selected=0, rel='newer', forced=0, dev=0, write_ok=1),
                 'Skipped'),
                # S-11.095, S-11.068: the check comes before a refused start, item 5's case included
                ('bootloader', dict(present=1, option=1, selected=1, rel='newer', forced=0, dev=0, write_ok=1,
                                    start='refused_verified', written_here=1, slot_matches=1), bad),
                ('bootloader', dict(present=1, option=1, selected=1, rel='newer', forced=0, dev=0, write_ok=1,
                                    start='refused_bad', written_here=0, slot_matches=0), bad),
                # S-11.057: the reserved step makes no check
                ('reserved', dict(option=1, selected=1), 'Not found'),
                ('ble', dict(switched_on=1, present=1, rel='newer', forced=0, in_dfu=1, write_ok=1, selected=1),
                 bad),
                # S-11.069: the check before the step's own DFU-mode queries
                ('ble', dict(switched_on=1, present=1, rel='newer', forced=0, in_dfu=0, write_ok=1, selected=1),
                 bad),
                ('ble', dict(switched_on=1, present=1, rel='newer', forced=0, in_dfu=1, write_ok=1, selected=0),
                 'Skipped'),
                ('ble bootloader', dict(switched_on=1, present0=1, present1=0, rel='newer', forced=0, slot=0,
                                        in_dfu=0, write_ok=1, selected=1), bad),
                ('ble', dict(switched_on=1, present=1, rel='equal', forced=0, in_dfu=1, write_ok=1, selected=1),
                 'Already installed'),
                ('ble', dict(switched_on=0, present=1, rel='newer', forced=0, in_dfu=1, write_ok=1, selected=1),
                 'Skipped'),
                ('ble bootloader', dict(switched_on=1, present0=1, present1=0, rel='newer', forced=0, slot=0,
                                        write_ok=1, selected=1), bad),
                ('ble bootloader', dict(switched_on=1, present0=1, present1=0, rel='newer', forced=0, slot=1,
                                        write_ok=1, selected=1), 'Not found'),
                ('ble bootloader', dict(switched_on=1, present0=1, present1=0, rel='newer', forced=0, slot=255,
                                        write_ok=1, selected=1), 'Failure'),
                ('ble bootloader', dict(switched_on=0, present0=1, present1=0, rel='newer', forced=0, slot=0,
                                        write_ok=1, selected=1), 'Skipped'),
                ('ble bootloader', dict(switched_on=1, present0=1, present1=0, rel='newer', forced=0, slot=0,
                                        write_ok=1, selected=0), 'Skipped'),
                ('keyboard', dict(switched_on=1, present=1, rel='newer', forced=0, write_ok=1, selected=1), bad),
                ('keyboard', dict(switched_on=1, present=1, rel='equal', forced=0, write_ok=1, selected=1),
                 'Already installed'),
                ('keyboard', dict(switched_on=0, present=1, rel='newer', forced=0, write_ok=1, selected=1),
                 'Skipped'),
                ('keyboard', dict(switched_on=1, present=1, rel='newer', forced=0, write_ok=1, selected=0),
                 'Skipped')]:
            c['staged'] = staged
            rows.append([step, json.dumps(c, sort_keys=True), exp])
    for step, c, exp in rows:
        got = R[m.step_result(m.STEPS.index(step), json.loads(c))]
        assert got == exp, (step, c, got, exp)
    # S-11.064 item 3 after the result, S-08.048: a Failure of the application step whose queries answered
    # "in DFU" came from its transfer, which sends the prepare first, so the run's end sends no exit
    out = []
    for step, c, exp in rows:
        d = json.loads(c)
        end = 'no_exit' if step == 'ble' and exp == 'Failure' and d.get('in_dfu', 1) else 'exit'
        assert m.end_nordic(m.STEPS.index(step), d) == end, (step, c, end)
        out.append([step, c, exp, end])
    return out


def pick_rows():
    """S-11.096: what the selection offers each step: a row for each step carried; dim when it is switched
    off, protected with the option off, or would report Already installed with its installed version known."""
    rows = []
    for cd in (0, 1):
        rows.append(['content', dict(content_dir=cd), 'open' if cd else 'absent'])
    for p in (0, 1):
        rows.append(['os', dict(present=p), 'open' if p else 'absent'])
    for p, opt in itertools.product((0, 1), (0, 1)):
        rows.append(['bootloader', dict(present=p, option=opt),
                     'absent' if not p else 'open' if opt else 'option off'])
    for step in ('ble', 'keyboard', 'ble bootloader'):
        carried = ([dict(present0=a, present1=b) for a, b in ((0, 0), (1, 0), (0, 1), (1, 1))]
                   if step == 'ble bootloader' else [dict(present=0), dict(present=1)])
        for car, on, rel, forced, known in itertools.product(carried, (1, 0), ('equal', 'newer'), (0, 1), (1, 0)):
            c = dict(car, switched_on=on, rel=rel, forced=forced, known=known)
            present = any(car.values())
            exp = ('absent' if not present else 'switched off' if not on else
                   'up to date' if rel == 'equal' and not forced and known else 'open')
            rows.append([step, c, exp])
    rows.append(['reserved', dict(option=1), 'absent'])
    out = []
    for step, c, exp in rows:
        got = m.pick(m.STEPS.index(step), c)
        assert got == exp, (step, c, got, exp)
        out.append([step, json.dumps(c, sort_keys=True), exp])
    return out


def staged_check_vector():
    """S-11.095: synthetic image bytes, each read as it would arrive."""
    img = bytes((i * 7 + 3) & 0xFF for i in range(64))
    flip = img[:20] + bytes([img[20] ^ 0x10]) + img[21:]
    stated = [len(img), zlib.crc32(img) & 0xFFFFFFFF]
    cases = [
        ('staging_ok', stated, [img], 'ok'),
        ('staging_flip', stated, [flip], 'bad'),
        ('staging_short', stated, [img[:-1]], 'bad'),
        ('staging_read_fails', stated, [None], 'read_error'),
        ('staging_long', stated, [img + b'\0'], 'bad'),
    ]
    out = []
    for name, st, reads, exp in cases:
        assert m.staged_check(st, reads) == exp, name
        out.append(dict(case=name, stated=st, reads=[None if r is None else r.hex() for r in reads], expect=exp,
                        written=reads[0].hex() if exp == 'ok' else None))
    return dict(cases=out)


def run_rows():
    R = {n: i for i, n in enumerate(m.RESULT_NAMES)}
    # case, content_only, nordic_images, option, results, outcome, nordic_dfu_ok, version_file[, steps_off,
    # selected]; without the last two: every step switched on and selected (S-11.073, S-11.096)
    on, off, every = (), m.STEPS_OFF, m.SELECTABLE
    cases = [
        ('all_ok', 0, 1, 1, ['OK'] * 6 + ['Not found'], 'done', 1, 1),
        ('normal_install_skips', 0, 1, 0, ['OK', 'OK', 'Skipped', 'OK', 'OK', 'OK', 'Not found'], 'done', 1, 1),
        ('only_protected_skipped', 0, 0, 0, ['Not found', 'Not found', 'Skipped', 'Not found', 'Not found',
                                             'Not found', 'Not found'], 'nothing to do', 1, 1),
        ('all_already', 0, 1, 1, ['Not found', 'OK', 'OK', 'Already installed', 'Already installed',
                                  'Already installed', 'Not found'], 'done', 1, 1),
        ('nothing_to_do', 0, 1, 0, ['Not found', 'Not found', 'Skipped', 'Already installed', 'Already installed',
                                    'Already installed', 'Not found'], 'nothing to do', 1, 1),
        ('older_bootloader_written', 0, 0, 1, ['OK', 'OK', 'OK', 'Not found', 'Not found', 'Not found',
                                               'Not found'], 'done', 1, 1),
        ('no_version_file', 0, 0, 0, ['Not found', 'OK', 'Not found', 'Not found', 'Not found', 'Not found',
                                      'Not found'], 'done', 1, 0),
        ('os_internal_error_stops', 0, 0, 0, ['OK', 'Internal error'], 'failed', 1, 1),
        ('ble_failure_stops', 0, 1, 1, ['OK', 'OK', 'OK', 'Failure'], 'failed', 1, 1),
        ('bad_file_stops', 0, 0, 0, ['Bad file'], 'failed', 1, 1),
        ('content_only', 1, 0, 0, ['OK', 'Not found', 'Not found', 'Not found', 'Not found', 'Not found',
                                   'Not found'], 'done', 1, 1),
        ('content_only_fails', 1, 0, 0, ['Failure'], 'failed', 1, 1),
        ('nordic_no_dfu', 0, 1, 0, [], 'failed', 0, 1),
        # S-11.073: this version's steps 3-5 switched off. Nordic images present: no DFU entry, so a Nordic
        # refusal changes nothing; step 6 is reserved and reports Not found
        ('this_version', 0, 1, 0, ['OK', 'OK', 'Skipped', 'Skipped', 'Skipped', 'Skipped', 'Not found'], 'done', 1,
         1, off, (0, 1)),
        ('this_version_nordic_refuses_dfu', 0, 1, 0, ['OK', 'OK', 'Skipped', 'Skipped', 'Skipped', 'Skipped',
                                                       'Not found'], 'done', 0, 1, off, (0, 1)),
        ('this_version_selected', 0, 1, 1, ['OK', 'OK', 'OK', 'Skipped', 'Skipped', 'Skipped', 'Not found'],
         'done', 1, 1, off, (0, 1, 2)),
        ('this_version_bootloader_not_selected', 0, 1, 1, ['OK', 'OK', 'Skipped', 'Skipped', 'Skipped', 'Skipped',
                                                            'Not found'], 'done', 1, 1, off, (0, 1)),
        ('this_version_only_switched_off', 0, 1, 0, ['Not found', 'Not found', 'Not found', 'Skipped', 'Skipped',
                                                      'Skipped', 'Not found'], 'nothing to do', 1, 1, off, ()),
        ('this_version_os_fails', 0, 1, 0, ['OK', 'Internal error'], 'failed', 1, 1, off, (0, 1)),
        # S-11.096: a step not selected reports Skipped; S-11.064: the package version only with the OS
        # installed in this run; S-11.061: the DFU entry only for a selected Nordic step
        ('os_not_selected', 0, 0, 1, ['OK', 'Skipped', 'OK', 'Not found', 'Not found', 'Not found', 'Not found'],
         'done', 1, 1, on, (0, 2)),
        ('only_content_selected', 0, 1, 0, ['OK', 'Skipped', 'Skipped', 'Skipped', 'Skipped', 'Skipped',
                                            'Not found'], 'done', 0, 1, on, (0,)),
        ('ble_selected', 0, 1, 0, ['Not found', 'Skipped', 'Skipped', 'OK', 'Skipped', 'Skipped', 'Not found'],
         'done', 1, 1, on, (3,)),
        ('ble_selected_no_dfu', 0, 1, 0, [], 'failed', 0, 1, on, (3,)),
        ('ble_bootloader_selected', 0, 1, 0, ['Not found', 'Skipped', 'Skipped', 'Skipped', 'OK', 'Skipped',
                                              'Not found'], 'done', 1, 1, on, (4,)),
        ('nothing_selected', 0, 1, 1, ['Not found', 'Skipped', 'Skipped', 'Skipped', 'Skipped', 'Skipped',
                                       'Not found'], 'nothing to do', 1, 1, on, ()),
        ('content_only_selected', 1, 0, 0, ['OK', 'Not found', 'Not found', 'Not found', 'Not found', 'Not found',
                                            'Not found'], 'done', 1, 1, on, (0,)),
    ]
    rows = []
    for row in cases:
        case, co, nordic, opt, results, outcome, dfu, vf = row[:8]
        steps_off, selected = row[8:] if len(row) > 8 else (on, every)
        res = {i: R[r] for i, r in enumerate(results)}
        got = m.run(res, bool(co), bool(nordic), bool(dfu), bool(vf), steps_off=steps_off, selected=set(selected))
        # S-11.061 item 2, S-11.073, S-11.096: a DFU entry only for a Nordic image whose step is on and selected
        dfu_entry = bool(nordic) and any(s not in steps_off and s in selected for s in m.NORDIC_STEPS)
        assert (['nordic', 'enter_dfu'] in [list(x) for x in got['writes']]) == dfu_entry, case
        for i, r in enumerate(results):       # S-11.058, S-11.073, S-11.090, S-11.096: a step's Skipped
            if r == 'Skipped':
                assert (i == 2 and not opt) or i in steps_off or i not in selected, (case, i)
            if i == m.RESERVED:
                assert r == 'Not found', (case, i)
            if i in steps_off:
                assert r in ('Skipped', 'Not found'), (case, i)
        assert got['outcome'] == outcome, (case, got['outcome'])
        assert [m.RESULT_NAMES[r] for _, r in got['ran']] == results, case
        prog = [list(x[1]) for x in got['writes'] if x[0] == 'progress']
        meta = [x[1] for x in got['writes'] if x[0] == 'meta']
        # S-11.061, S-11.064: state 0xAB0D first unless content-only, and state 0 at the end of every run
        # that is not content-only, completed or failed; the package version only with the OS installed
        os_ok = len(results) > 1 and results[1] == 'OK'
        want_meta = [] if co else ['state=AB0D', ('state=0' if outcome == 'failed' or not vf or not os_ok
                                                  else 'os_version=package,state=0')]
        assert meta == want_meta, (case, meta)
        rows.append([case, co, nordic, ','.join(map(str, steps_off)), int(dfu_entry), opt,
                     ','.join(map(str, sorted(selected))), dfu, vf, ','.join(results), outcome,
                     json.dumps(prog), json.dumps(meta),
                     json.dumps([list(x) if isinstance(x, tuple) else x for x in got['writes']])])
    return rows


def interrupted_rows():
    """S-11.080: the boot's handling of the upgrade state: with 0xAB0D, whatever the keys, the slave and the
    write's result, held mode, one metadata write of state 0, the notice, and nothing else."""
    rows = []
    for state, keys, slave, wok in itertools.product((0, m.UPGRADING), ('not_held', 'held'),
                                                     ('present', 'absent'), (1, 0)):
        if state == 0 and (not wok or slave == 'absent'):
            continue                      # no write is made
        got = m.interrupted_boot(state, keys == 'held', slave == 'present', bool(wok))
        if state == m.UPGRADING:
            want = ('held', ['state=0'], True, 0 if wok else m.UPGRADING)
        else:
            want = ('held' if keys == 'held' else 'os', [], False, 0)
        assert (got['decision'], got['writes'], got['notice'], got['state_after']) == want, (state, keys, slave, wok)
        assert (got['steps'], got['nordic_dfu'], got['record_access']) == (0, 0, 0)
        rows.append(['0x%04x' % state, keys, slave, wok if state else '', got['decision'],
                     json.dumps(got['writes']), int(got['notice']), '0x%04x' % got['state_after'],
                     got['steps'], got['nordic_dfu'], got['record_access']])
    return rows


def stock_rows():
    """S-11.091: each condition varied alone. The bootloader can be selected only when it is present and the
    option is on (S-11.096: its row cannot be picked otherwise)."""
    rows = []
    for opt, te, bl in itertools.product([0, 1], repeat=3):
        for sel in ([0, 1] if opt and bl else [0]):
            st = m.return_to_stock(opt, te, bl, sel)
            assert st == (opt == 1 and te == 1 and bl == 1 and sel == 1)
            rows.append([opt, te, bl, sel, int(st), m.os_form(st)])
    return rows


DISK = [
    ('first_of_two', [('a.tfw', 0), ('b.tfw', 0)], 'a.tfw', 0, 0),
    ('dot_skipped', [('.x.tfw', 0), ('b.tfw', 0)], 'b.tfw', 0, 0),
    ('upper_not_package', [('TFW.BIN', 0), ('cdc_please', 0)], None, 0, 1),
    ('dir_skipped', [('dir.tfw', 1), ('c.tfw', 0)], 'c.tfw', 0, 0),
    ('subdir_not_found', [('sub', 1), ('sub/x.tfw', 0)], None, 0, 0),
    ('substring_anywhere', [('notes_tfw.txt', 0)], 'notes_tfw.txt', 0, 0),
    ('forced_by_name', [('op-xy_force.tfw', 0)], 'op-xy_force.tfw', 1, 0),
    ('upper_force_not_forced', [('FORCE.tfw', 0)], 'FORCE.tfw', 0, 0),
    ('decoy_zip_before', [('firmware_a.zip', 0), ('op-xy.tfw', 0)], 'op-xy.tfw', 0, 0),
    ('decoy_firmware_zip_present', [('firmware.zip', 0), ('op-xy.tfw', 0)], 'op-xy.tfw', 0, 0),
    ('decoy_zip_alone', [('firmware.zip', 0)], None, 0, 0),
    ('cdc_beside_package', [('cdc_please', 0), ('junk.tfw', 0)], 'junk.tfw', 0, 0),
]


def disk_file(name):
    """The bytes a disk_search.tsv file holds: a name containing tfw holds the base package, a .zip the
    decoy zip, anything else 10 bytes of junk."""
    base = name.rsplit('/', 1)[-1]
    if 'tfw' in base and not base.endswith('.zip'):
        return assemble()
    if base.endswith('.zip'):
        return DECOY_ZIP
    return b'x' * 10


def disk_rows():
    rows = []
    for case, ents, want, forced, cdc in DISK:
        top = [(n, d) for n, d in ents if '/' not in n]
        got = m.disk_search(top)
        assert got == want, (case, got)
        name, chk = m.disk_install([(n, d, None if d else disk_file(n)) for n, d in top], unit())
        inst = chk['zip'] if chk and chk['accepted'] else None
        # from construction: a taken package file holds the base package, whose zip is BASE_ZIP
        assert inst == (BASE_ZIP if want is not None else None), case
        f = int(got is not None and 'force' in got)
        assert f == forced
        c = int(got is None and m.cdc_please(top))
        assert c == cdc, case
        rows.append([case, json.dumps(ents), want or '', forced, cdc,
                     hashlib.sha256(BASE_ZIP).hexdigest() if want is not None else 'none'])
    return rows


def main():
    os.makedirs(V, exist_ok=True)

    def dump(name, obj):
        with open(os.path.join(V, name + '.json'), 'w') as f:
            json.dump(obj, f, indent=1, sort_keys=False)
            f.write('\n')
    dump('packages', write_packages())
    dump('zips', write_zips())
    dump('versions', versions())
    dump('product_words', product_words())
    dump('crc16', crc_vectors())
    dump('progress', progress_vectors())
    dump('content', content_cases())
    check_acceptance(m.acceptance_rows())
    write_tsv('steps.tsv', ['step', 'conditions', 'result', 'end_nordic'], step_rows(),
              'S-11.066-S-11.073, S-11.096; end_nordic: S-11.064 item 3 after the result (exit: the query and '
              'the exit when in DFU; no_exit: no exit command, S-08.048)')
    write_tsv('picks.tsv', ['step', 'conditions', 'pick'], pick_rows(), 'S-11.096, S-11.073, S-11.090')
    write_tsv('runs.tsv', ['case', 'content_only', 'nordic_images', 'steps_off', 'dfu_entry', 'option',
                           'selected', 'nordic_dfu_ok', 'version_file', 'results', 'outcome', 'progress_after',
                           'metadata_writes', 'writes'], run_rows(),
              'S-11.057-S-11.065, S-11.073, S-11.096, S-11.061')
    write_tsv('interrupted.tsv', ['state', 'keys', 'slave', 'meta_write_ok', 'decision', 'metadata_writes',
                                  'notice', 'state_after', 'steps', 'nordic_dfu', 'record_access'],
              interrupted_rows(),
              'S-11.080')
    write_tsv('stock.tsv', ['option', 'te_signed', 'bootloader_present', 'bootloader_selected', 'return_to_stock',
                            'os_form'],
              stock_rows(), 'S-11.091, S-11.092, S-11.096')
    write_tsv('disk_search.tsv', ['case', 'entries_in_directory_order', 'package', 'forced', 'cdc_reset',
                                  'installed_zip'], disk_rows(),
              'S-11.046-S-11.048, S-04.024')
    with open(os.path.join(HERE, 'vectors', 'staged_check.json'), 'w') as f:
        json.dump(staged_check_vector(), f, indent=1)
        f.write('\n')
    assert sorted(os.listdir(PKG)) == sorted(PKG_FILES), sorted(set(os.listdir(PKG)) ^ set(PKG_FILES))
    print('vectors written')


if __name__ == '__main__':
    main()
