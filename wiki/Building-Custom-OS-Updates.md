How to build a firmware package (`.tfw`) that carries your own OS image, in a form waffle-boot installs:
unsigned, with a plaintext payload. To install one, see [Updating the OS](Updating-the-OS).

waffle-boot needs no signature and no encryption. A package is accepted on its format, its CRC-16, and its
payload length and SHA-256, which catch a damaged file but prove nothing about who made it. Its signature is
checked but never required ([Signatures](Updating-the-OS#signatures)): a package without one installs every
part you choose, the bootloader too with Protected writes on, with no warning.

**Status:** the script's packages pass the reference model under `tests/oracle/S-11-packages/`. An install of
one is **not yet confirmed** on a unit.

> **Warning.** An install replaces the OS on the unit. Keep TE's firmware file at hand to write it back, and
> know [How to Recover](How-to-Recover) first.

## The package file

Five parts, one after another. Offsets below are from the start of each part; bytes not listed are zero.

| Part | Length | What it holds |
| --- | --- | --- |
| Preamble | `0x40` | A magic, a type and a version |
| TE header | `0x40` | A magic, the device id, a CRC-16, a version and the product word |
| Manifest | `0x300` | The payload's length and SHA-256, the zip's length |
| Name block | `0x100` | The zip's name |
| Zip | any | The files to install |

The **payload** is the name block and everything after it, to the end of the file.

### Preamble (big-endian)

| Offset | Size | Value |
| --- | --- | --- |
| `0x00` | 4 | `0xBABECAFE` |
| `0x04` | 1 | `0x00` |
| `0x05` | 2 | CRC-16 of everything after the preamble (written, not checked) |
| `0x07` | 4 × 2 | Version: major, minor, patch, build |

### TE header (big-endian)

| Offset | Size | Value |
| --- | --- | --- |
| `0x00` | 4 | `0xBEEFCAFE` |
| `0x04` | 1 | Device id: `0x01` |
| `0x05` | 4 | Length of everything after the header (written, not checked) |
| `0x09` | 2 | CRC-16 of everything after the header: the manifest and the payload |
| `0x0B` | 4 × 2 | Version: major, minor, patch, build |
| `0x13` | 4 | `0` |
| `0x17` | 4 | Product word: `0x00084000` |

The CRC-16 is CRC-16/XMODEM: polynomial `0x1021`, start value 0, no reflection. Do not use the version
0.1.0 in either place: it marks a forced package.

### Manifest (little-endian)

| Offset | Size | Value |
| --- | --- | --- |
| `0x04` | 1 | `0xFE`: the payload is plaintext |
| `0x10` | `0x40` | Zero: no signature. Nonzero bytes that do not verify make the unit warn before the list |
| `0x80` | 4 | The zip's length in bytes |
| `0x84` | 5 | `TESG`, then `0x01` |
| `0x89` | 1 | `0xFF`: no signing key |
| `0x8C` | 4 | Product word: `0x00084000` (not checked; the TE header's is) |
| `0x98` | 4 | The payload's length in bytes |
| `0x9C` | 32 | SHA-256 of the payload |

### Name block

The zip's name, ending in a zero byte, then zeros to `0x100` bytes. The name must contain `.zip` and be under
`0x7C` bytes: `firmware.zip` does.

### After the zip

Zero bytes may follow the zip; the script pads the payload to a multiple of `0x80`. The payload's length and
SHA-256 cover them, the zip's length does not.

## The zip

| Member | What it is |
| --- | --- |
| `firmware/blackfin/bin/os.ldr` | Your OS image |
| `firmware/blackfin/bin/os.version` | Its version as text: `1.2.3+0`. The list of parts shows it on the Main DSP row |
| `firmware/firmware.version` | The package's version, in the same form. The install screen shows it, and it is recorded as the installed version when an install that wrote the OS ends |

Rules for the zip:

- Members are stored or deflated. No encryption, no zip64.
- Names use `/`, with no leading `/`, no empty part, and no `.` or `..` part.
- The internal drive needs free space for the members' unpacked sizes plus 4 MiB.
- A version file needs its two `.` characters in its first 32 bytes; otherwise it reads as 1234.0.0.

Leave these out of an OS package:

- `firmware/blackfin/bin/bootloader.ldr`. With **Protected writes** on, a bootloader in the package that is
  chosen on the unit replaces waffle-boot in a slot. A `spiboot.ldr` is unpacked with the rest and never
  used.
- `firmware/blackfin/content/`. A package with that folder removes the unit's content (all but `content/dlc/`)
  and installs the package's in its place, unless a file name under it contains `partial_upgrade`.

## The OS image

`os.ldr` is a boot stream for the ADSP-BF70x. waffle-boot checks it when the package is unpacked, and again
before each start:

- Every block header is 16 bytes, with the signature `0xAD` and an XOR of zero over the 16 bytes.
- No block has the INIT, CALLBACK, INDIRECT, QUICKBOOT, AUX or SAVE flag.
- The stream ends in a FINAL block whose payload is all there. Bytes after it are ignored.
- Every block's destination lies in L1, L2 or DDR, and clear of the ranges below.
- The image is at least 9,488 bytes and at most `0x800000`, and fits the eMMC's boot partition with one
  512-byte sector to spare.

| Range | Kept for |
| --- | --- |
| `0x08000000`-`0x08002000` | The boot ROM |
| `0x0803C000`-`0x0803E000` | waffle-boot's hand-over, in L2 |
| `0x11B01F00`-`0x11B02000` | The reboot record |
| `0x8803D000`-`0x88A65000` | waffle-boot's hand-over, in DDR |
| `0x8A000000`-`0x8A800000` | The staging area the image is loaded from |

The OS step has no version test: an image in the package is written whatever version the unit holds.

## A script

Python 3, standard library only. It writes a package with one OS image.

```python
#!/usr/bin/env python3
"""mktfw.py OS_LDR VERSION OUT.tfw: an unsigned, plaintext package holding one OS image."""
import hashlib, io, struct, sys, zipfile

def crc16(data, crc=0):                                 # CRC-16/XMODEM
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = (crc << 1) ^ 0x1021 if crc & 0x8000 else crc << 1
        crc &= 0xFFFF
    return crc

os_ldr, version, out = open(sys.argv[1], 'rb').read(), sys.argv[2], sys.argv[3]
v = [int(x) for x in version.split('.')] + [0]          # "1.2.3" -> 1, 2, 3, build 0
text = '%d.%d.%d+%d' % tuple(v)
product = 0x21 << 14

buf = io.BytesIO()
with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('firmware/firmware.version', text)
    z.writestr('firmware/blackfin/bin/os.ldr', os_ldr)
    z.writestr('firmware/blackfin/bin/os.version', text)
zip_bytes = buf.getvalue()

payload = b'firmware.zip'.ljust(0x100, b'\0') + zip_bytes + bytes(-len(zip_bytes) % 0x80)
man = bytearray(0x300)
man[0x04] = 0xFE                                        # plaintext payload
struct.pack_into('<I', man, 0x80, len(zip_bytes))
man[0x84:0x89] = b'TESG\x01'
man[0x89] = 0xFF                                        # no signing key
struct.pack_into('<I', man, 0x8C, product)
struct.pack_into('<I', man, 0x98, len(payload))
man[0x9C:0xBC] = hashlib.sha256(payload).digest()

rest = bytes(man) + payload
header = struct.pack('>IBIH4HII', 0xBEEFCAFE, 0x01, len(rest), crc16(rest), *v, 0, product)
body = header.ljust(0x40, b'\0') + rest
preamble = struct.pack('>IBH4H', 0xBABECAFE, 0x00, crc16(body), *v).ljust(0x40, b'\0')
open(out, 'wb').write(preamble + body)
```

```
python3 mktfw.py os.ldr 1.2.3 custom.tfw
```

## On an OP-1F

> **Status:** not yet run on a unit.

Use the OP-1F's product word, `0x00008000`, in the TE header: set `product = 0x02 << 14` in the script (it
writes the same word in the manifest, where it is not checked). Its package is then checked as on the OP-XY.

The OP-1F also takes a second form: a manifest without the tag `TESG` at `0x84`. Of it, waffle-boot reads
only the payload key at `0x04`, the IV at `0x70` and the zip's length at `0x80`; there is no signature,
product word, payload length or SHA-256 to check. The CRC-16 and every check from the payload key on still
apply, so the payload key must be `0xFE` here too. A manifest with `TESG` is the first form, and needs `0x01`
after it. Prefer the first form: its SHA-256 catches a damaged file.

In `checktfw.py`, pass `device='OP-1F'` to `m.check_package`.

## Checking a package

The reference model makes the bootloader's checks on a computer. From the repository's top folder:

```python
#!/usr/bin/env python3
"""checktfw.py PACKAGE: the package and zip checks of the reference model."""
import sys
sys.path[:0] = ['tests/oracle/conformance', 'tests/oracle/S-11-packages']
import s11_model as m

r = m.check_package(open(sys.argv[1], 'rb').read(), 'file', m.Unit(), file_name=sys.argv[1])
print('package:', r['reason'] or 'accepted')
if r['accepted']:
    reason, staged = m.check_members(r['zip'], 1 << 30)
    print('zip:', reason or 'accepted')
```

```
python3 -I -B checktfw.py custom.tfw
```

It does not make the eMMC size check; that one is made on the unit.

## Installing it

The file's name must contain `tfw`, in lower case, and must not start with `.`. The unit takes the first such
file in the disk's top folder. Then follow [Updating the OS](Updating-the-OS#steps): the unit lists the
package's parts, and writes the OS only when **2 Main DSP** is set to `Yes`.

## Refusal reasons

A refused package shows `No valid package` and a reason line.

| Reason | Meaning |
| --- | --- |
| `SHORT` | The file ends before a part is complete |
| `PREAMBLE`, `TYPE` | The preamble's magic or type byte is wrong |
| `MAGIC`, `PRODUCT`, `DEVICE` | The TE header's magic, product word or device id is wrong |
| `CRC` | The header's CRC-16 does not match the manifest and payload |
| `FORMAT` | The manifest lacks `TESG` and `0x01` at `0x84` (on an OP-1F, a tag other than `TESG` is the second form, not a refusal) |
| `LENGTH` | The manifest's payload length is not the payload's |
| `HASH` | The manifest's SHA-256 is not the payload's |
| `KEY`, `NOKEY` | The byte at `0x04` of the manifest is not `0xFE` |
| `NAME` | The name block's name has no `.zip` |
| `ZIPLEN` | The zip's stated length is longer than the payload holds |
| `ZIP` | The zip cannot be read |
| `MEMBER` | A member breaks a rule of the zip, or cannot be unpacked to its stated length and CRC-32 |
| `SPACE` | The internal drive is too full |
| `SIZE` | An image is over its limit (`0x800000` for the OS) |
| `STREAM` | The OS image fails a boot-stream check above |
