# r7samp: 1-in-N latency sampling (bench_main -T N, default 16)

Commits: c27a0a6 (test, fails), 5f212ed (fix), e8d526c/9095ca0 (logs and script).
Boxes: c7i.2xlarge (x86_64) and the arm-lo type (aarch64), Debian 13, gcc 14.2.0,
glibc 2.41, libjemalloc2 5.3.0-3. Isolated `git archive` builds (verify-isolated.sh).

## Test (test_bench_accounting, test_lat_sampling)
- c27a0a6, x86 (`prefix-c27a0a6-intel/`): **FAIL, 21 failures**. The clock_gettime
  count is the same at -T 16 and -T 1, e.g. frag 762160 = 762160, frag -D
  3558498 reads for 1779240 ops.
- e8d526c, both arches (`pass1-e8d526c/`): **PASS, 0 failures**. At -T 16 the
  clock reads are 1/16 of -T 1 (frag 47640 vs 762160) and ops are unchanged
  (381079 = 381079). Covers single, multi, prodcons, frag, -D prodcons, -D frag.

## perf, frag t=8, -s 16:4096 -D 10:2 -W 1, perf -F 499 -g (`pass2-9095ca0/`)
Self %, summed over the clock_gettime/vdso symbols and td_* symbols. ops/s is the
kept interval, a single run under perf. It is NOT an allocator comparison.

| arch | alloc | -T | clock_gettime | td_* | ops/s |
|---|---|---|---|---|---|
| x86 | jemalloc | 1 | 18.46 | 17.88 | 18.40 M |
| x86 | jemalloc | 16 | 4.45 | 2.14 | 23.85 M |
| x86 | umem | 1 | 21.20 | 15.61 | 15.42 M |
| x86 | umem | 16 | 3.55 | 1.97 | 20.73 M |
| arm | jemalloc | 1 | 26.02 | 31.93 | 31.66 M |
| arm | jemalloc | 16 | 3.69 | 3.85 | 61.52 M |
| arm | umem | 1 | 19.37 | 19.45 | 22.71 M |
| arm | umem | 16 | 2.45 | 1.97 | 36.21 M |

## Finding (not fixed, out of scope): `-a jemalloc` does not load at this HEAD
Pass 1: `-a jemalloc` printed no row (6 perf samples, all in ld.so), and
LD_DEBUG shows the dlopen of libjemalloc.so.2 failing. Control in pass 2, on
both arches: the default run prints nothing, and
`GLIBC_TUNABLES=glibc.rtld.optional_static_tls=65536` loads it. So the cause is
static TLS space. libumem.so has a 0x10b4-byte TLS segment and jemalloc needs
0xa48 initial-exec. The jemalloc cells above use that tunable. r7harness got
jemalloc rows at 2e88c21, so something since then grew the static TLS used. I
have not bisected it. Silently dropping an arm ("not available") is an
accounting defect for any comparison run.
