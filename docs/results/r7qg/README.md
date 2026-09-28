# r7qg: does 3816199's quiet-rig gate hide the P8.3 defect?

**Answer: no.** The pre-fix library (6705310) with the gated test from 3816199
FAILs 3/3 on both architectures. The gate never retried (0 noisy attempts) and
never SKIPped. Post-fix (3816199), `interpose_regress.sh` exits 0 3/3 on both.

Rig: Debian 13, gcc 14.2.0, c7i.2xlarge (intel-lo@r7qg, i-0d042fd8839bb87da) and
c7g.2xlarge (arm-lo@r7qg, i-0097429435bcd1c5c), idle and running nothing else.
Both instances were terminated after the runs.

## (a) PRE-FIX: 6705310 library + 3816199 test, LD_PRELOAD=.libs/libumem_malloc.so

Tree: `git archive 6705310`, then an overlay of `test/stress/repro_interpose_free_ratio.c`
from 3816199 (blob cd93d73, sha256 7430c16b...). See `ship_prefix.sh` and `PROVENANCE-prefix`.

| box | run | null median | null min | preload/API median | min | rc |
|-----|-----|------|------|------|------|----|
| c7i | 1 | 1.016 | 0.981 | 0.401 | 0.391 | 1 FAIL |
| c7i | 2 | 1.005 | 0.984 | 0.415 | 0.395 | 1 FAIL |
| c7i | 3 | 0.986 | 0.965 | 0.407 | 0.394 | 1 FAIL |
| c7g | 1 | 1.002 | 0.982 | 0.390 | 0.383 | 1 FAIL |
| c7g | 2 | 0.993 | 0.934 | 0.392 | 0.388 | 1 FAIL |
| c7g | 3 | 1.010 | 0.973 | 0.391 | 0.386 | 1 FAIL |

Log: `*/prefix-attempt5-PASS-build-3xFAIL/out.log`.

**Build caveat.** 6705310 does not build as-is on Debian 13 / gcc 14:
- With libnuma-dev present, `umem.c` calls `numa_available()` etc. without
  `#include <numa.h>`, which is an implicit declaration and a hard error in gcc 14.
  Later fixed by 4b64153. See attempt1.
- `umem_null_cache`'s positional initializer in `umem.c` is out of step with
  `umem_impl.h` at that sha (the `cache_percpu_map`/`cache_percpu_len` fields were
  added to the struct but not to the initializer). That is `-Wint-conversion`,
  also a hard error in gcc 14. See attempts 2-4.

The build that ran used `./configure --disable-numa CFLAGS='-g -O2 -Wno-error=int-conversion'`
and made only `libumem.la libumem_malloc.la test/stress/repro_interpose_free_ratio`.
No source was changed. The mis-ordered initializer only affects the static
`umem_null_cache` sentinel, whose fields the hot malloc/free path does not
read. The ratios (0.39-0.42) match the documented pre-fix values of 0.409 / 0.395.

## (b) POST: 3816199 (git archive), default configure, full `make -j8`

`interpose_regress.sh` run standalone 3 times (log: `*/prefix-attempt3-*/out.log`, PART B):

| box | run | null median | null min | preload/API median | min | rc |
|-----|-----|------|------|------|------|----|
| c7i | 1 | 1.009 | 0.920 | 0.543 | 0.521 | 0 |
| c7i | 2 | 1.028 | 0.967 | 0.595 | 0.575 | 0 |
| c7i | 3 | 0.991 | 0.925 | 0.564 | 0.532 | 0 |
| c7g | 1 | 0.992 | 0.977 | 0.503 | 0.499 | 0 |
| c7g | 2 | 0.998 | 0.914 | 0.505 | 0.499 | 0 |
| c7g | 3 | 0.995 | 0.976 | 0.506 | 0.491 | 0 |

`make check -j8`, once per box: `PASS: test/stress/interpose_regress.sh`.
Totals: TOTAL 68, PASS 54, SKIP 12, XFAIL 2, FAIL 0, XPASS 0, ERROR 0 on both boxes.

**Inside that PASS, the ratio sub-test SKIPPED on both boxes**
(`*/makecheck/interpose_regress.log`). The null min stayed below 0.88 on all
4 attempts: c7i 0.817/0.618/0.701/0.632, c7g 0.579/0.497/0.601/0.687. The
script counts 77 as not-a-failure. Under `make check -j8` on an 8-vCPU box,
the gate returns "no verdict" instead of judging. This is the behaviour it was
designed to have, but `make check` does not currently exercise P8.3; only the
standalone run does.
