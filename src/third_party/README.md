# Vendored components

The upstream libraries the bootloader uses, with their sources and versions.
Every local change is listed under [Local changes](#local-changes), and each
file it touches says so near its top. `third_party.mk` builds each component
for the host (unit tests, fuzzers) and the target (`make cross`); `make size`
prints their target sizes. The glue between them and the bootloader is ours,
under `src/glue/`.

| Component | Upstream | Version | License | Local changes |
| --- | --- | --- | --- | --- |
| picolibc (subset) | <https://github.com/picolibc/picolibc> | 1.8.12, `2ae376c6cdf4fef90ca2388ecf7a07457fa63cff` | BSD-3-Clause and other permissive notices of newlib (`COPYING.picolibc`) | none; configured by `src/glue/libc/picolibc.h` |
| FatFs | <http://elm-chan.org/fsw/ff/>, from the release mirror <https://github.com/abbrev/fatfs> | R0.16 with patches 1–3 (2026-07-10), `ab992fcf2926b8a1ee3f0a8f9cefb6584174100d` | FatFs license (BSD-style, 1 clause) | `ffconf.h` configured (S-12 §3) |
| yaffs2 | <https://github.com/Aleph-One-Ltd/yaffs2> | master of 2026-06-29, `474b3acb927d27b2305618aaf24456b9d33fe91b` | GPL-2.0 (its `direct/` headers LGPL-2.1) | zero-erased media; lazy checkpoint erase; checkpoint erased first; fuzzing fixes (below) |
| zlib (inflate) | <https://github.com/madler/zlib> | 1.3.2, `da607da739fa6047df13e66a2af6b8bec7c2a498` | Zlib | none; built with `BUILDFIXED` and an empty `Z_TESTW` (below) |
| minizip (unzip) | <https://github.com/madler/zlib>, `contrib/minizip` | 1.3.2, as zlib | Zlib (`crypt.h`: Info-ZIP, not built) | `ioapi.c` builds without stdio; built with `NOUNCRYPT`, `MINIZIP_NO_STDIO` and `IOAPI_NO_64` |
| micro-ecc | <https://github.com/kmackay/micro-ecc> | 1.2, `541b3a78026420a3e369c4c9281c396b5e531113` | BSD-2-Clause | none; P-224 only, by `-D` options |
| BearSSL (subset) | <https://www.bearssl.org/git/BearSSL>, from FreeBSD's vendor import | `79c060e` (2023-02-20), freebsd-src `d84e570b54961e8874bbd8de25635eb96be0977e` | MIT | none |
| Spleen (8x16 font) | <https://github.com/fcambus/spleen> | 2.2.0, `0493c34e22791824767c618fed42c434b477662c` | BSD-2-Clause | none; `spleen-8x16.bdf` only, turned into `src/display_font.c` by `tools/bdf2c.py` |
| TinyUSB (device stack) | <https://github.com/hathach/tinyusb> | 0.21.0, `dae3f9a366bfcddbf9dcf1b48d7500286a849539` | MIT | none; configured by `src/glue/tinyusb/tusb_config.h`. `src/drivers/bf70x/usb_dcd.c` is ours, after its `portable/mentor/musb/dcd_musb.c` |

CRC-16/XMODEM, the S-06 CRC-8 and the PSoC CRC-16 are ours (`src/glue/crc`), not
vendored; CRC-32 is zlib's `crc32()`.

Every license above is compatible with GPL-2.0-or-later. yaffs2 is GPL-2.0
only, so a bootloader image that contains it is distributed under GPL-2.0,
which GPL-2.0-or-later code allows; Aleph One also states that
GPL-3.0-compatible use is fine.

## Where the sources came from

- **FatFs** comes from abbrev/fatfs, which mirrors each official release
  and patch as one commit. Its `master` is R0.16 with the three patches
  elm-chan published up to 2026-07-10, the current release. Patch 3 fixes
  CVE-2026-6682, CVE-2026-6683 and CVE-2026-6687 (BPB validation; the last
  two concern exFAT, which is not built). The files can be checked against
  the official `ff16.zip` and patches.
- **BearSSL** comes from FreeBSD's verbatim vendor import of upstream commit
  `79c060e`; `contrib/bearssl/src` and `inc` in freebsd-src `main` are
  identical to it. Upstream has later commits; none changes a file carried
  here.

Some components carry more upstream files than the build uses (headers and
build files of picolibc, TinyUSB's other classes); `third_party.mk` lists
what is built.

## Why these

- **C library: a picolibc subset.** The bootloader needs `mem*`/`str*`,
  `snprintf` and `strtol`/`strtoul`; the other components need `qsort` and
  `malloc`. picolibc is newlib's maintained, permissively licensed successor
  for embedded targets (it keeps newlib's Blackfin code), and its small
  stdio gives an integer-only `snprintf` of 1.4 KB. Its sources build
  unchanged with GCC 8 from our Makefile, with `src/glue/libc/picolibc.h` in
  place of the header its meson build would generate, so no new build
  system is needed, and no whole libc is built for a few functions. Integer
  division is libgcc's (`__divsi3`, `__udivdi3`, ...). `malloc` takes
  memory from `sbrk()`, which the board code provides
  (`src/board/opxy_main.c`).
- **AES: BearSSL `aes_big`**, its table AES, with CBC decryption. The
  constant-time `aes_ct` takes about three times as many instructions a
  byte on the BF703, where it dominated the package check. The tables
  (1.5 KB) are linked into uncached L1 data (`bf703.ld`), so a lookup takes
  the same time whatever its index.
- **SHA-256: BearSSL** `sha2small`, from the same import.
- **P-224: micro-ecc.** BearSSL implements P-256, P-384 and P-521 only.

## Local changes

- **FatFs** `ffconf.h`: code page 850, long names with UTF-8 at the API,
  `f_mkfs`, labels, and `f_utime` with `f_chmod` (one option,
  `FF_USE_CHMOD`) on, and fast seek on (the
  package's in-place recovery, `src/wire_fat.c`); everything else at its
  defaults.
- **yaffs2**, zero-erased media (`CONFIG_YAFFS_ERASED_ZERO`, S-12.043): the
  four places that test or set the erased value: the erased check in
  `yaffs_guts.c`, two comparisons in `yaffs_packedtags2.c`, and the
  summary's sequence number in `yaffs_summary.c`. The last is set with or
  without the option, where upstream left the field unset; otherwise the
  code without the option is upstream's.
- **yaffs2**, lazy checkpoint erase (`CONFIG_YAFFS_LAZY_CHECKPT_ERASE`,
  S-12.054, S-12.055): unlike upstream, a mount does not erase a checkpoint
  it did not restore, and an unmount that writes no checkpoint does not
  erase the old one. Every chunk write and block erase still invalidates the
  checkpoint first.
- **yaffs2**, checkpoint erased first (`CONFIG_YAFFS_CHECKPT_ERASE_FIRST`,
  S-12.055): a checkpoint block whose erase fails stays a checkpoint
  instead of being marked bad, and while one is left no chunk is written
  and no other block is erased. Upstream carried on writing, so a reader
  that restores that checkpoint lost the session's changes.
- **yaffs2**, lazy mount clean-up (`CONFIG_YAFFS_LAZY_MOUNT_CLEANUP`,
  S-12.054): unlike upstream, a mount does not erase the blocks its scan
  found with nothing in use, nor delete the objects in the unlinked and
  deleted directories (after a rename over a file, that wrote the replaced file's
  header into the deleted directory). `yaffs_check_gc()`, which every
  chunk write and every collection passes through first, and a checkpoint
  save do both before anything else, as the mount would have.
- **yaffs2**, fuzzing fixes (S-12.046, S-12.047): a crash when a header
  retyped the unlinked or deleted directory; a NULL dereference listing a
  hard link to a missing object; a hard link to a hard link or to itself,
  which left the scan's list head on the stack in the link's chain;
  block order for sequence numbers of 0x80000000 and above; signed shifts
  (two in `yaffs_packedtags2.c`, one in `FSIZE_COMBINE`, `yaffs_guts.h`);
  symbolic-link strings leaked at unmount.
- **minizip** `ioapi.c`: with `MINIZIP_NO_STDIO`, opening an archive by name
  fails instead of calling `fopen()`; archives are read from memory
  (`src/glue/minizip`).

## Notes for the code that uses them

- **FatFs R0.16's `f_mkfs` is not R0.13c's.** Asked for S-12.004's
  parameters it writes every byte of S-12.003 to S-12.006 except the
  partition's end CHS (it picks 8 heads, not 255) and the volume ID (it adds
  the volume size to the time). `wb_fatfs_format()` (`src/glue/fatfs/format.c`)
  sets both afterwards and writes the label; `test_fatfs` checks the whole
  format against `ramdisk_format.json`. FatFs still strips trailing spaces
  and dots from names: S-12.012's refusal is the caller's.
- **What S-12 asks beyond upstream yaffs2** is not in this import and
  belongs to the bootloader's filesystem layer: the trusted-header rule
  (S-12.051), refusing a big-endian volume before the mount (S-12.052),
  the space reserve as S-12.056 states it, and mounting read-write for
  the lost+found fix-up (S-12 §3). The test images have 8 blocks, so the
  tests lower the reserve from 5 to 2 (yaffs2 needs more blocks than the
  reserve plus 2); the reserve only limits writing.
- **minizip does not enforce S-11.030.** It opens a member of another
  method (bzip2, without bzip2 support, comes back as raw bytes), reads an
  encrypted member, and checks a member's CRC-32 only when it was read to
  the end; it compares no length. The bootloader checks the method and the
  encryption flag from the file information, reads every member to its
  end, and compares the length read with the stated size.
- **zlib**: the inflate subset is `adler32.c`, `crc32.c`, `inffast.c`,
  `inflate.c`, `inftrees.c` and `zutil.c`. `src/wire_zip.c` reads each
  member raw from minizip and inflates it itself, step for step as
  minizip's read did, so minizip computes no CRC-32 of it. Built with
  `BUILDFIXED`, inflate builds its fixed-code tables in RAM on first use;
  with `Z_TESTW` empty, `crc32()` works byte by byte from one 1 KiB table,
  without the braid tables. The bulk CRC-32s run on the CRC peripheral
  (`src/board/opxy_crc.c`).
- **yaffs2** is built without yaffs1 (`CONFIG_YAFFS_NO_YAFFS1`,
  `src/glue/yaffs2/yaffs_port.h`): every device is a yaffs2 one.
- **micro-ecc** verifies against an unchecked public key; check Q0 with
  `uECC_valid_public_key()` once when it is read. It truncates a SHA-256
  digest to the leftmost 224 bits, as S-11.012 requires.

## Target sizes

`make size`, archive totals under `-Os` with the Makefile's `TARGET_SIZE`
(`-O2` for crc, zlib, and picolibc's `memcpy` and `memset`, which copy by
words; `-mcpu=bf703-none`, `-ffunction-sections`); a linked image keeps only
what it uses.

| Component | text | data | bss |
| --- | ---: | ---: | ---: |
| picolibc | 5,000 | 0 | 16 |
| crc (ours) | 940 | 0 | 0 |
| blockdev (ours) | 316 | 0 | 0 |
| fatfs (with glue) | 15,867 | 0 | 522 |
| yaffs2 (with glue) | 50,731 | 9 | 80,180 |
| zlib | 15,086 | 0 | 2,196 |
| minizip (with glue) | 7,716 | 0 | 0 |
| micro-ecc | 8,260 | 0 | 4 |
| BearSSL | 5,568 | 0 | 0 |
| TinyUSB | 7,812 | 29 | 366 |

## Tests

`make check` runs a known-answer test per component against the
specification's vectors (`tests/unit/test_*.c`, vectors exported by
`tests/vectors/export.py`); `make fuzz-run` runs the libFuzzer targets
(`tests/fuzz/README.md`).
