
## c1-t192-64-256.toml

c7i.metal-48xl x86_64, threads 192, prodcons 64:256, frag 64:256, sha 2d8645f

### frag-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 1,116 / 1,116 / 1,117 | 892.0 / 894.5 / 936.5 | 1,922 / 1,928 / 1,970 | 3,810 / 3,810 / 3,811 |
| libc | 423.8 / 423.8 / 424.0 | 2,112 / 2,118 / 2,119 | 3,748 / 3,788 / 3,874 | 3,907 / 3,907 / 3,907 |
| umem | 632.7 / 634.0 / 634.3 | 748.5 / 754.0 / 757.5 | 1,930 / 2,048 / 2,090 | 4,257 / 4,278 / 4,290 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc better than libc (2.63x); jemalloc better than umem (1.76x); umem better than libc (1.50x)
- p99 ns: jemalloc better than libc (2.37x); umem better than jemalloc (1.19x); umem better than libc (2.81x)
- p999 ns: jemalloc better than libc (1.96x); jemalloc vs umem: no resolvable difference; umem better than libc (1.85x)
- peak RSS MiB: jemalloc better than libc (1.03x); jemalloc better than umem (1.12x); libc better than umem (1.09x)

### prodcons-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 806.6 / 820.0 / 913.1 | 1,124 / 1,286 / 1,288 | 3,108 / 3,338 / 3,341 | 94.6 / 95.5 / 95.6 |
| libc | 669.4 / 981.4 / 1,031 | 1,110 / 1,192 / 3,208 | 4,342 / 5,142 / 8,764 | 66.4 / 66.4 / 66.5 |
| umem | 562.1 / 567.8 / 577.0 | 385.5 / 394.0 / 415.5 | 2,722 / 2,782 / 2,834 | 235.3 / 237.4 / 242.1 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc vs libc: no resolvable difference; jemalloc better than umem (1.44x); libc better than umem (1.73x)
- p99 ns: jemalloc vs libc: no resolvable difference; umem better than jemalloc (3.26x); umem better than libc (3.03x)
- p999 ns: jemalloc better than libc (1.54x); umem better than jemalloc (1.20x); umem better than libc (1.85x)
- peak RSS MiB: libc better than jemalloc (1.44x); jemalloc better than umem (2.49x); libc better than umem (3.58x)


RESULT PASS (0 invariant failures)
