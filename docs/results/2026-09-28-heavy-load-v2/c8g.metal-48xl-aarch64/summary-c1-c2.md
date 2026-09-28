
## c1-t192-64-256.toml

c8g.metal-48xl aarch64, threads 192, prodcons 64:256, frag 64:256, sha 2a5b850

### frag-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 1,146 / 1,146 / 1,146 | 954.5 / 970.5 / 972.5 | 3,356 / 3,377 / 3,386 | 3,812 / 3,812 / 3,812 |
| libc | 471.6 / 472.3 / 472.4 | 2,290 / 2,293 / 2,302 | 3,072 / 3,078 / 3,084 | 3,906 / 3,906 / 3,906 |
| umem | 734.0 / 734.1 / 741.8 | 964.0 / 976.0 / 976.5 | 2,190 / 2,316 / 2,351 | 4,213 / 4,245 / 4,264 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc better than libc (2.43x); jemalloc better than umem (1.56x); umem better than libc (1.55x)
- p99 ns: jemalloc better than libc (2.36x); jemalloc vs umem: no resolvable difference; umem better than libc (2.35x)
- p999 ns: libc better than jemalloc (1.10x); umem better than jemalloc (1.46x); umem better than libc (1.33x)
- peak RSS MiB: jemalloc better than libc (1.02x); jemalloc better than umem (1.11x); libc better than umem (1.09x)

### prodcons-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 1,235 / 1,330 / 1,377 | 512.0 / 534.5 / 558.5 | 1,261 / 1,311 / 1,406 | 159.5 / 160.1 / 162.3 |
| libc | 279.0 / 289.1 / 313.3 | 5,694 / 6,266 / 6,685 | 10,404 / 10,896 / 11,076 | 65.2 / 65.2 / 65.2 |
| umem | 518.7 / 531.7 / 563.5 | 505.0 / 511.0 / 518.0 | 2,268 / 2,378 / 2,631 | 269.4 / 276.3 / 279.3 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc better than libc (4.60x); jemalloc better than umem (2.50x); umem better than libc (1.84x)
- p99 ns: jemalloc better than libc (11.72x); jemalloc vs umem: no resolvable difference; umem better than libc (12.26x)
- p999 ns: jemalloc better than libc (8.31x); jemalloc better than umem (1.81x); umem better than libc (4.58x)
- peak RSS MiB: libc better than jemalloc (2.46x); jemalloc better than umem (1.73x); libc better than umem (4.24x)


## c2-t384-64-256.toml

c8g.metal-48xl aarch64, threads 384, prodcons 64:256, frag 64:256, sha 2a5b850

### frag-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 1,169 / 1,171 / 1,172 | 841.0 / 847.0 / 847.5 | 3,184 / 3,192 / 3,195 | 3,915 / 3,915 / 3,915 |
| libc | 483.7 / 484.5 / 485.6 | 2,202 / 2,207 / 2,207 | 3,070 / 3,072 / 3,082 | 3,953 / 3,953 / 3,953 |
| umem | 768.8 / 775.1 / 776.9 | 941.5 / 943.5 / 948.0 | 1,954 / 1,958 / 2,024 | 4,017 / 4,104 / 4,111 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc better than libc (2.42x); jemalloc better than umem (1.51x); umem better than libc (1.60x)
- p99 ns: jemalloc better than libc (2.61x); jemalloc better than umem (1.11x); umem better than libc (2.34x)
- p999 ns: libc better than jemalloc (1.04x); umem better than jemalloc (1.63x); umem better than libc (1.57x)
- peak RSS MiB: jemalloc better than libc (1.01x); jemalloc better than umem (1.05x); libc better than umem (1.04x)

### prodcons-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 1,703 / 1,704 / 1,747 | 464.5 / 471.0 / 476.5 | 1,275 / 1,286 / 1,307 | 341.7 / 342.2 / 342.9 |
| libc | 3,341 / 3,415 / 3,501 | 342.0 / 345.5 / 376.0 | 2,061 / 2,341 / 2,506 | 124.5 / 124.5 / 124.5 |
| umem | 469.7 / 489.2 / 489.7 | 548.0 / 562.5 / 566.0 | 2,499 / 2,523 / 2,590 | 413.0 / 425.8 / 432.9 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: libc better than jemalloc (2.00x); jemalloc better than umem (3.48x); libc better than umem (6.98x)
- p99 ns: libc better than jemalloc (1.36x); jemalloc better than umem (1.19x); libc better than umem (1.63x)
- p999 ns: jemalloc better than libc (1.82x); jemalloc better than umem (1.96x); libc vs umem: no resolvable difference
- peak RSS MiB: libc better than jemalloc (2.75x); jemalloc better than umem (1.24x); libc better than umem (3.42x)


RESULT PASS (0 invariant failures)
