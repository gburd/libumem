
## c4-t192-frag-16384-65536.toml

c7i.metal-48xl x86_64, threads 192, prodcons 64:256, frag 16384:65536, sha 2a5b850

### frag-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 8.0 / 8.0 / 8.0 | 15,336 / 15,477 / 15,500 | 25,806 / 25,978 / 26,012 | 4,138 / 4,139 / 4,141 |
| libc | 8.1 / 8.1 / 8.1 | 11,909 / 11,954 / 12,038 | 141,752 / 143,802 / 144,870 | 3,285 / 3,286 / 3,287 |
| umem | 8.3 / 8.3 / 8.3 | 12,342 / 12,475 / 13,174 | 22,204 / 22,479 / 23,282 | 3,197 / 3,199 / 3,204 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: libc better than jemalloc (1.01x); umem better than jemalloc (1.04x); umem better than libc (1.03x)
- p99 ns: libc better than jemalloc (1.29x); umem better than jemalloc (1.24x); libc better than umem (1.04x)
- p999 ns: jemalloc better than libc (5.54x); umem better than jemalloc (1.16x); umem better than libc (6.40x)
- peak RSS MiB: libc better than jemalloc (1.26x); umem better than jemalloc (1.29x); umem better than libc (1.03x)


RESULT PASS (0 invariant failures)
