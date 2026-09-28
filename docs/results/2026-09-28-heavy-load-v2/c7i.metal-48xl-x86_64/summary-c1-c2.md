
## c1-t192-64-256.toml

c7i.metal-48xl x86_64, threads 192, prodcons 64:256, frag 64:256, sha 2a5b850

### frag-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 1,120 / 1,120 / 1,120 | 896.0 / 909.0 / 912.5 | 1,934 / 1,940 / 1,950 | 3,811 / 3,811 / 3,811 |
| libc | 429.1 / 429.5 / 429.7 | 2,090 / 2,091 / 2,093 | 3,532 / 3,588 / 3,599 | 3,907 / 3,907 / 3,907 |
| umem | 644.4 / 646.6 / 648.7 | 739.0 / 740.5 / 742.5 | 1,910 / 1,976 / 2,032 | 4,279 / 4,291 / 4,293 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc better than libc (2.61x); jemalloc better than umem (1.73x); umem better than libc (1.51x)
- p99 ns: jemalloc better than libc (2.30x); umem better than jemalloc (1.23x); umem better than libc (2.82x)
- p999 ns: jemalloc better than libc (1.85x); jemalloc vs umem: no resolvable difference; umem better than libc (1.82x)
- peak RSS MiB: jemalloc better than libc (1.03x); jemalloc better than umem (1.13x); libc better than umem (1.10x)

### prodcons-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 780.8 / 865.0 / 981.8 | 1,005 / 1,217 / 1,467 | 2,893 / 3,242 / 3,470 | 94.7 / 94.8 / 95.9 |
| libc | 372.8 / 866.9 / 1,096 | 984.0 / 1,666 / 7,211 | 3,869 / 7,252 / 18,352 | 66.4 / 66.5 / 66.5 |
| umem | 566.7 / 581.4 / 597.0 | 521.5 / 530.5 / 559.5 | 2,856 / 2,932 / 3,089 | 236.8 / 237.1 / 245.8 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc vs libc: no resolvable difference; jemalloc better than umem (1.49x); libc vs umem: no resolvable difference
- p99 ns: jemalloc vs libc: no resolvable difference; umem better than jemalloc (2.29x); umem better than libc (3.14x)
- p999 ns: jemalloc better than libc (2.24x); jemalloc vs umem: no resolvable difference; umem better than libc (2.47x)
- peak RSS MiB: libc better than jemalloc (1.43x); jemalloc better than umem (2.50x); libc better than umem (3.57x)


## c2-t384-64-256.toml

c7i.metal-48xl x86_64, threads 384, prodcons 64:256, frag 64:256, sha 2a5b850

### frag-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 1,134 / 1,136 / 1,136 | 785.5 / 803.5 / 807.0 | 1,964 / 1,974 / 1,975 | 3,903 / 3,903 / 3,903 |
| libc | 455.4 / 456.4 / 457.7 | 2,030 / 2,034 / 2,040 | 3,566 / 3,596 / 3,613 | 3,955 / 3,955 / 3,955 |
| umem | 662.4 / 662.7 / 663.4 | 760.5 / 760.5 / 765.0 | 2,256 / 2,284 / 2,322 | 4,088 / 4,106 / 4,165 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc better than libc (2.49x); jemalloc better than umem (1.71x); umem better than libc (1.45x)
- p99 ns: jemalloc better than libc (2.53x); umem better than jemalloc (1.06x); umem better than libc (2.68x)
- p999 ns: jemalloc better than libc (1.82x); jemalloc better than umem (1.16x); umem better than libc (1.57x)
- peak RSS MiB: jemalloc better than libc (1.01x); jemalloc better than umem (1.05x); libc better than umem (1.04x)

### prodcons-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 2,423 / 2,429 / 2,433 | 459.5 / 462.0 / 480.0 | 1,398 / 1,465 / 1,494 | 186.2 / 186.2 / 186.9 |
| libc | 2,896 / 2,960 / 2,966 | 224.0 / 229.5 / 263.0 | 911.0 / 939.0 / 1,006 | 126.4 / 126.4 / 126.4 |
| umem | 621.7 / 623.9 / 630.5 | 463.0 / 470.5 / 482.5 | 2,374 / 2,514 / 2,547 | 422.4 / 435.4 / 443.5 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: libc better than jemalloc (1.22x); jemalloc better than umem (3.89x); libc better than umem (4.74x)
- p99 ns: libc better than jemalloc (2.01x); jemalloc vs umem: no resolvable difference; libc better than umem (2.05x)
- p999 ns: libc better than jemalloc (1.56x); jemalloc better than umem (1.72x); libc better than umem (2.68x)
- peak RSS MiB: libc better than jemalloc (1.47x); jemalloc better than umem (2.34x); libc better than umem (3.45x)


RESULT PASS (0 invariant failures)
