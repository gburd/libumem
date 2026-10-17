
## c1-t192-64-256.toml

c8g.metal-48xl aarch64, threads 192, prodcons 64:256, frag 64:256, sha 2d8645f

### frag-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 1,134 / 1,134 / 1,135 | 979.5 / 989.0 / 995.0 | 3,518 / 3,533 / 3,568 | 3,812 / 3,812 / 3,812 |
| libc | 473.9 / 474.1 / 474.1 | 2,286 / 2,290 / 2,290 | 3,076 / 3,080 / 3,084 | 3,906 / 3,906 / 3,906 |
| umem | 731.5 / 734.2 / 737.5 | 979.0 / 980.5 / 985.5 | 2,339 / 2,464 / 2,642 | 4,040 / 4,219 / 4,247 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc better than libc (2.39x); jemalloc better than umem (1.54x); umem better than libc (1.55x)
- p99 ns: jemalloc better than libc (2.31x); jemalloc vs umem: no resolvable difference; umem better than libc (2.34x)
- p999 ns: libc better than jemalloc (1.15x); umem better than jemalloc (1.43x); umem better than libc (1.25x)
- peak RSS MiB: jemalloc better than libc (1.02x); jemalloc better than umem (1.11x); libc better than umem (1.08x)

### prodcons-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 1,179 / 1,256 / 1,283 | 551.5 / 551.5 / 568.0 | 1,360 / 1,378 / 1,460 | 162.8 / 163.3 / 165.6 |
| libc | 290.4 / 293.9 / 310.8 | 6,466 / 6,620 / 6,735 | 11,466 / 12,199 / 12,208 | 65.1 / 65.1 / 65.1 |
| umem | 537.6 / 568.7 / 578.9 | 342.5 / 352.0 / 383.5 | 2,142 / 2,291 / 2,325 | 259.2 / 279.6 / 312.2 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc better than libc (4.27x); jemalloc better than umem (2.21x); umem better than libc (1.93x)
- p99 ns: jemalloc better than libc (12.00x); umem better than jemalloc (1.57x); umem better than libc (18.81x)
- p999 ns: jemalloc better than libc (8.85x); jemalloc better than umem (1.66x); umem better than libc (5.32x)
- peak RSS MiB: libc better than jemalloc (2.51x); jemalloc better than umem (1.71x); libc better than umem (4.29x)


RESULT PASS (0 invariant failures)
