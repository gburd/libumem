
## c3-t192-1024-4096.toml

c7i.metal-48xl x86_64, threads 192, prodcons 1024:4096, frag 1024:4096, sha 2a5b850

### frag-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 112.7 / 112.8 / 112.8 | 5,458 / 5,483 / 5,486 | 10,712 / 10,719 / 10,736 | 3,721 / 3,721 / 3,721 |
| libc | 104.9 / 104.9 / 104.9 | 4,856 / 4,861 / 4,862 | 13,124 / 13,136 / 13,432 | 3,254 / 3,254 / 3,254 |
| umem | 106.9 / 107.1 / 107.3 | 3,916 / 3,931 / 3,948 | 7,618 / 7,688 / 7,698 | 3,446 / 3,450 / 3,457 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc better than libc (1.07x); jemalloc better than umem (1.05x); umem better than libc (1.02x)
- p99 ns: libc better than jemalloc (1.13x); umem better than jemalloc (1.39x); umem better than libc (1.24x)
- p999 ns: jemalloc better than libc (1.23x); umem better than jemalloc (1.39x); umem better than libc (1.71x)
- peak RSS MiB: libc better than jemalloc (1.14x); umem better than jemalloc (1.08x); libc better than umem (1.06x)

### prodcons-sustained  (latency sampled 1/16)

| allocator | Mops/s min / med / max | p99 ns min / med / max | p999 ns min / med / max | peak RSS MiB min / med / max |
|---|---|---|---|---|
| jemalloc | 296.7 / 304.2 / 369.6 | 5,389 / 6,968 / 7,093 | 17,851 / 25,210 / 29,174 | 334.9 / 363.9 / 409.8 |
| libc | 172.7 / 178.8 / 184.9 | 15,536 / 15,660 / 15,935 | 42,830 / 46,730 / 48,102 | 288.2 / 288.5 / 289.2 |
| umem | 126.8 / 133.0 / 137.8 | 1,360 / 1,414 / 1,482 | 4,374 / 4,430 / 4,456 | 921.8 / 929.5 / 931.5 |

Verdicts (non-overlap of 3-run ranges):

- Mops/s: jemalloc better than libc (1.70x); jemalloc better than umem (2.29x); libc better than umem (1.34x)
- p99 ns: jemalloc better than libc (2.25x); umem better than jemalloc (4.93x); umem better than libc (11.08x)
- p999 ns: jemalloc better than libc (1.85x); umem better than jemalloc (5.69x); umem better than libc (10.55x)
- peak RSS MiB: libc better than jemalloc (1.26x); jemalloc better than umem (2.55x); libc better than umem (3.22x)


RESULT PASS (0 invariant failures)
