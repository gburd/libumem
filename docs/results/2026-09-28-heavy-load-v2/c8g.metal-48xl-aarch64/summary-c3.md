
## c3-t192-1024-4096.toml

c8g.metal-48xl aarch64, threads 192, prodcons 1024:4096, frag 1024:4096, sha 2a5b850

### frag-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 122.5 / 122.5 / 122.5 | 12,812 / 12,859 / 12,929 | 25,162 / 25,230 / 25,284 | 3,832 / 3,833 / 3,835 |
| libc | 119.1 / 119.1 / 119.1 | 6,338 / 6,367 / 6,372 | 13,449 / 13,542 / 13,728 | 3,253 / 3,253 / 3,253 |
| umem | 124.5 / 124.6 / 124.7 | 5,460 / 5,480 / 5,549 | 14,376 / 14,668 / 15,018 | 3,555 / 3,558 / 3,568 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc better than libc (1.03x); umem better than jemalloc (1.02x); umem better than libc (1.05x)
- p99 ns: libc better than jemalloc (2.02x); umem better than jemalloc (2.35x); umem better than libc (1.16x)
- p999 ns: libc better than jemalloc (1.86x); umem better than jemalloc (1.72x); libc better than umem (1.08x)
- peak RSS MiB: libc better than jemalloc (1.18x); umem better than jemalloc (1.08x); libc better than umem (1.09x)

### prodcons-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 1,270 / 1,274 / 1,275 | 685.0 / 698.0 / 713.5 | 1,648 / 1,660 / 1,664 | 500.6 / 501.3 / 502.5 |
| libc | 80.3 / 82.5 / 118.3 | 11,996 / 15,579 / 15,659 | 15,864 / 21,095 / 21,120 | 286.6 / 290.0 / 291.6 |
| umem | 137.6 / 140.0 / 141.3 | 1,466 / 1,503 / 1,506 | 4,552 / 4,713 / 5,182 | 935.1 / 1,020 / 1,023 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc better than libc (15.45x); jemalloc better than umem (9.10x); umem better than libc (1.70x)
- p99 ns: jemalloc better than libc (22.32x); jemalloc better than umem (2.15x); umem better than libc (10.37x)
- p999 ns: jemalloc better than libc (12.71x); jemalloc better than umem (2.84x); umem better than libc (4.48x)
- peak RSS MiB: libc better than jemalloc (1.73x); jemalloc better than umem (2.04x); libc better than umem (3.52x)


RESULT PASS (0 invariant failures)
