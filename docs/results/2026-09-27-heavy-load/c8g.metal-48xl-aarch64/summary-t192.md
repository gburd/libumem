## docs/results/2026-09-27-heavy-load/c8g.metal-48xl-aarch64/heavy-t192.toml
c8g.metal-48xl aarch64 threads=192 sha=e7b7ea7b49cd mode=continuous numa=numactl --interleave=all tunables=glibc.rtld.optional_static_tls=65536 stack=8317599

Per run (each row = one 15-min process, stats over its 10 kept intervals):

| workload | run | Mops/s med (min-max) | p50 | p99 med (min-max) | p999 med (min-max) | peak RSS MB |
|---|---|---|---:|---|---|---:|
| frag-sustained | jemalloc | 24.487 (24.486-24.487) | 33 | 395 (366-411) | 864 (860-866) | 3621 |
| frag-sustained | jemalloc@2 | 24.525 (24.524-24.525) | 33 | 386 (376-395) | 877 (872-878) | 3622 |
| frag-sustained | libc | 22.898 (22.895-22.900) | 286 | 1802 (1796-1805) | 16590 (10696-21588) | 3715 |
| frag-sustained | libc@2 | 24.458 (24.455-24.460) | 290 | 1774 (1770-1778) | 18906 (16126-20558) | 3715 |
| frag-sustained | umem | 24.306 (24.304-24.307) | 122 | 685 (648-751) | 4676 (2840-9269) | 3958 |
| frag-sustained | umem@2 | 23.094 (23.092-23.096) | 120 | 698 (646-733) | 4314 (3322-8420) | 4012 |
| prodcons-sustained | jemalloc | 1.980 (1.977-2.007) | 136 | 486 (403-518) | 1278 (1273-1284) | 95 |
| prodcons-sustained | jemalloc@2 | 1.986 (1.984-1.989) | 145 | 792 (749-811) | 1387 (1366-1400) | 97 |
| prodcons-sustained | libc | 1.442 (1.439-1.454) | 212 | 1570 (1480-1802) | 6933 (6735-7113) | 47 |
| prodcons-sustained | libc@2 | 1.645 (1.625-1.666) | 356 | 7008 (6665-7544) | 21548 (21258-22220) | 46 |
| prodcons-sustained | umem | 1.423 (1.422-1.423) | 68 | 289 (261-315) | 1582 (1503-1688) | 169 |
| prodcons-sustained | umem@2 | 1.615 (1.611-1.620) | 77 | 444 (420-520) | 5356 (5143-5436) | 168 |

Per allocator (mean of its two runs; gap = |run1 - run2| / mean):

| workload | allocator | Mops/s (gap) | p50 ns (gap) | p99 ns (gap) | p999 ns (gap) | peak RSS MB (gap) |
|---|---|---:|---:|---:|---:|---:|
| frag-sustained | jemalloc | 24.506 (0.2 %) | 33 (0.0 %) | 390 (2.4 %) | 870 (1.6 %) | 3622 (0.0 %) |
| frag-sustained | libc | 23.678 (6.6 %) | 288 (1.4 %) | 1788 (1.6 %) | 17748 (13.1 %) | 3715 (0.0 %) |
| frag-sustained | umem | 23.700 (5.1 %) | 121 (1.7 %) | 692 (2.0 %) | 4495 (8.1 %) | 3985 (1.4 %) |
| prodcons-sustained | jemalloc | 1.983 (0.3 %) | 141 (6.0 %) | 639 (48.0 %) | 1333 (8.1 %) | 96 (1.6 %) |
| prodcons-sustained | libc | 1.544 (13.1 %) | 284 (50.7 %) | 4289 (126.8 %) | 14240 (102.6 %) | 46 (2.4 %) |
| prodcons-sustained | umem | 1.519 (12.6 %) | 72 (12.4 %) | 367 (42.4 %) | 3468 (108.8 %) | 169 (0.6 %) |

Pairwise (ratio A/B of the means; 'noise' = the larger of A's and B's own gap):

| workload | A vs B | Mops/s | p50 ns | p99 ns | p999 ns | peak RSS MB |
|---|---|---|---|---|---|---|
| frag-sustained | umem vs libc | 1.00x, inside noise (noise 6.6 %) | 0.42x, umem better (noise 1.7 %) | 0.39x, umem better (noise 2.0 %) | 0.25x, umem better (noise 13.1 %) | 1.07x, libc better (noise 1.4 %) |
| frag-sustained | umem vs jemalloc | 0.97x, inside noise (noise 5.1 %) | 3.67x, jemalloc better (noise 1.7 %) | 1.77x, jemalloc better (noise 2.4 %) | 5.17x, jemalloc better (noise 8.1 %) | 1.10x, jemalloc better (noise 1.4 %) |
| frag-sustained | jemalloc vs libc | 1.03x, inside noise (noise 6.6 %) | 0.11x, jemalloc better (noise 1.4 %) | 0.22x, jemalloc better (noise 2.4 %) | 0.05x, jemalloc better (noise 13.1 %) | 0.97x, jemalloc better (noise 0.0 %) |
| prodcons-sustained | umem vs libc | 0.98x, inside noise (noise 13.1 %) | 0.26x, umem better (noise 50.7 %) | 0.09x, umem better (noise 126.8 %) | 0.24x, umem better (noise 108.8 %) | 3.65x, libc better (noise 2.4 %) |
| prodcons-sustained | umem vs jemalloc | 0.77x, jemalloc better (noise 12.6 %) | 0.52x, umem better (noise 12.4 %) | 0.57x, umem better (noise 48.0 %) | 2.60x, jemalloc better (noise 108.8 %) | 1.76x, jemalloc better (noise 1.6 %) |
| prodcons-sustained | jemalloc vs libc | 1.28x, jemalloc better (noise 13.1 %) | 0.50x, jemalloc better (noise 50.7 %) | 0.15x, jemalloc better (noise 126.8 %) | 0.09x, jemalloc better (noise 102.6 %) | 2.08x, libc better (noise 2.4 %) |

RESULT: PASS (0 invariant failures)
