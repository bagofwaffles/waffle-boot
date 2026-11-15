# Fuzzing

libFuzzer targets for every parser of untrusted input among the vendored
libraries (`src/third_party/README.md`). Each is built with clang, AddressSanitizer
and UndefinedBehaviorSanitizer, and starts from seeds that `seeds.py` makes
from the test data and builders under `tests/oracle/`.

| Fuzzer | Input | What it exercises |
| --- | --- | --- |
| `fuzz_fatfs` | the first sectors of a RAM disk of S-12.001's size (the rest reads as zero) | FatFs mounting a volume a host left (S-12.020), walking and reading it, and writing to it as the bootloader does |
| `fuzz_yaffs2` | records of chunk number, data area and tags for an 8-block yaffs2 volume (see the file) | the yaffs2 scan, lookups, reads and writes |
| `fuzz_inflate` | a mode byte (raw deflate, zlib or gzip) and a stream | zlib's inflate |
| `fuzz_unzip` | a zip archive | minizip reading every member from memory |
| `fuzz_p224` | public key, signature and digest | micro-ecc's P-224 key check and signature verification |

    make fuzz                    # build build/fuzz/fuzz_*
    make fuzz-run FUZZ_TIME=60   # run each for 60 s; fails on the first finding

Below, `<target>` is a fuzzer's name without `fuzz_` (`fatfs`, `yaffs2`, ...).
New inputs are kept in `build/fuzz/corpus/<target>/`; a finding is written to
`build/fuzz/<target>-crash-*` (or `-leak-*`, `-timeout-*`). Reproduce one with
`build/fuzz/fuzz_<target> <file>`. Once it is fixed, keep the input in
`tests/fuzz/regressions/<target>/`: `seeds.py` adds those to the seeds, so
every run starts from them. CI's `fuzz` job (`tools/ci-local.sh fuzz` runs it
locally) runs `make fuzz-run` for a bounded time and keeps any finding.

The yaffs2 sources are built without UBSan's `null` and `object-size` checks:
yaffs2's `list_entry()` is the `&((type *)NULL)->member` offsetof idiom, which
both report on every list walk. AddressSanitizer still catches real null
dereferences and out-of-bounds accesses there.

The fixes for fuzzing findings are among the local changes listed in
`src/third_party/README.md`.
