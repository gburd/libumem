
## c2-t384-64-256.toml

c8g.metal-48xl aarch64, threads 384, prodcons 64:256, frag 64:256, sha 2d8645f

### prodcons-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 1,677 / 1,959 / 1,969 | 417.5 / 436.5 / 483.5 | 1,169 / 1,190 / 1,290 | 339.9 / 341.5 / 342.4 |
| libc | 3,188 / 3,276 / 3,626 | 336.0 / 409.5 / 450.5 | 1,467 / 2,590 / 2,794 | 124.5 / 124.5 / 124.5 |
| umem | 503.1 / 505.6 / 528.8 | 354.5 / 360.0 / 361.0 | 2,178 / 2,302 / 2,306 | 427.4 / 429.1 / 429.2 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: libc better than jemalloc (1.67x); jemalloc better than umem (3.87x); libc better than umem (6.48x)
- p99 ns: jemalloc vs libc: no resolvable difference; umem better than jemalloc (1.21x); libc vs umem: no resolvable difference
- p999 ns: jemalloc better than libc (2.18x); jemalloc better than umem (1.94x); libc vs umem: no resolvable difference
- peak RSS MiB: libc better than jemalloc (2.74x); jemalloc better than umem (1.26x); libc better than umem (3.45x)


RESULT PASS (0 invariant failures)
