
## c4-t192-frag-16384-65536.toml

c8g.metal-48xl aarch64, threads 192, prodcons 64:256, frag 16384:65536, sha 2a5b850

### frag-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 9.0 / 9.0 / 9.0 | 41,824 / 41,845 / 42,433 | 57,518 / 57,759 / 58,346 | 4,465 / 4,466 / 4,470 |
| libc | 7.8 / 7.8 / 7.8 | 23,748 / 23,764 / 23,803 | 1,367,214 / 1,374,545 / 1,382,238 | 3,279 / 3,282 / 3,282 |
| umem | 9.5 / 9.6 / 9.6 | 52,812 / 53,447 / 98,680 | 106,986 / 110,532 / 195,280 | 3,195 / 3,196 / 3,214 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc better than libc (1.15x); umem better than jemalloc (1.07x); umem better than libc (1.22x)
- p99 ns: libc better than jemalloc (1.76x); jemalloc better than umem (1.28x); libc better than umem (2.25x)
- p999 ns: jemalloc better than libc (23.80x); jemalloc better than umem (1.91x); umem better than libc (12.44x)
- peak RSS MiB: libc better than jemalloc (1.36x); umem better than jemalloc (1.40x); umem better than libc (1.03x)


RESULT PASS (0 invariant failures)
