## docs/results/2026-09-27-heavy-load/c7i.metal-48xl-x86_64/heavy-t192.toml
c7i.metal-48xl x86_64 threads=192 sha=e7b7ea7b49cd mode=continuous numa=numactl --interleave=all tunables=glibc.rtld.optional_static_tls=65536 stack=8319103

Per run (each row = one 15-min process, stats over its 10 kept intervals):

| workload | run | Mops/s med (min-max) | p50 | p99 med (min-max) | p999 med (min-max) | peak RSS MB |
|---|---|---|---:|---|---|---:|
| frag-sustained | jemalloc | 15.161 (15.155-15.163) | 27 | 12778 (12611-12875) | 25111 (24973-25268) | 3612 |
| frag-sustained | jemalloc@2 | 13.948 (13.945-13.958) | 27 | 13736 (13557-13839) | 27678 (27505-27928) | 3612 |
| frag-sustained | libc | 14.928 (14.914-14.938) | 442 | 17516 (17319-17612) | 30980 (30823-31262) | 3717 |
| frag-sustained | libc@2 | 16.437 (16.433-16.448) | 470 | 16203 (15988-16290) | 29253 (29110-29391) | 3717 |
| frag-sustained | umem | 16.582 (16.567-16.598) | 210 | 11808 (11745-11925) | 24472 (24327-24573) | 4127 |
| frag-sustained | umem@2 | 15.890 (15.872-15.909) | 208 | 12581 (12440-12630) | 25630 (25338-25881) | 4126 |
| prodcons-sustained | jemalloc | 2.001 (1.968-2.013) | 282 | 13438 (12766-15158) | 27042 (25988-28679) | 64 |
| prodcons-sustained | jemalloc@2 | 1.888 (1.861-2.168) | 260 | 16064 (14394-17306) | 30934 (29653-34682) | 65 |
| prodcons-sustained | libc | 1.090 (1.066-1.373) | 880 | 18808 (17530-35356) | 46444 (41706-97757) | 49 |
| prodcons-sustained | libc@2 | 0.921 (0.918-1.202) | 890 | 19052 (18617-36896) | 42380 (41780-102109) | 50 |
| prodcons-sustained | umem | 2.018 (1.910-2.023) | 172 | 4196 (3827-4596) | 19124 (18774-20240) | 167 |
| prodcons-sustained | umem@2 | 2.051 (2.045-2.061) | 152 | 3175 (3016-3414) | 14738 (14490-15155) | 165 |

Per allocator (mean of its two runs; gap = |run1 - run2| / mean):

| workload | allocator | Mops/s (gap) | p50 ns (gap) | p99 ns (gap) | p999 ns (gap) | peak RSS MB (gap) |
|---|---|---:|---:|---:|---:|---:|
| frag-sustained | jemalloc | 14.555 (8.3 %) | 27 (0.0 %) | 13257 (7.2 %) | 26395 (9.7 %) | 3612 (0.0 %) |
| frag-sustained | libc | 15.682 (9.6 %) | 456 (5.9 %) | 16859 (7.8 %) | 30117 (5.7 %) | 3717 (0.0 %) |
| frag-sustained | umem | 16.236 (4.3 %) | 209 (0.5 %) | 12195 (6.3 %) | 25052 (4.6 %) | 4127 (0.0 %) |
| prodcons-sustained | jemalloc | 1.944 (5.8 %) | 271 (7.9 %) | 14751 (17.8 %) | 28988 (13.4 %) | 64 (2.3 %) |
| prodcons-sustained | libc | 1.006 (16.8 %) | 885 (1.1 %) | 18930 (1.3 %) | 44412 (9.2 %) | 49 (1.2 %) |
| prodcons-sustained | umem | 2.034 (1.6 %) | 162 (12.3 %) | 3686 (27.7 %) | 16932 (25.9 %) | 166 (1.3 %) |

Pairwise (ratio A/B of the means; 'noise' = the larger of A's and B's own gap):

| workload | A vs B | Mops/s | p50 ns | p99 ns | p999 ns | peak RSS MB |
|---|---|---|---|---|---|---|
| frag-sustained | umem vs libc | 1.04x, inside noise (noise 9.6 %) | 0.46x, umem better (noise 5.9 %) | 0.72x, umem better (noise 7.8 %) | 0.83x, umem better (noise 5.7 %) | 1.11x, libc better (noise 0.0 %) |
| frag-sustained | umem vs jemalloc | 1.12x, umem better (noise 8.3 %) | 7.74x, jemalloc better (noise 0.5 %) | 0.92x, umem better (noise 7.2 %) | 0.95x, inside noise (noise 9.7 %) | 1.14x, jemalloc better (noise 0.0 %) |
| frag-sustained | jemalloc vs libc | 0.93x, inside noise (noise 9.6 %) | 0.06x, jemalloc better (noise 5.9 %) | 0.79x, jemalloc better (noise 7.8 %) | 0.88x, jemalloc better (noise 9.7 %) | 0.97x, jemalloc better (noise 0.0 %) |
| prodcons-sustained | umem vs libc | 2.02x, umem better (noise 16.8 %) | 0.18x, umem better (noise 12.3 %) | 0.19x, umem better (noise 27.7 %) | 0.38x, umem better (noise 25.9 %) | 3.37x, libc better (noise 1.3 %) |
| prodcons-sustained | umem vs jemalloc | 1.05x, inside noise (noise 5.8 %) | 0.60x, umem better (noise 12.3 %) | 0.25x, umem better (noise 27.7 %) | 0.58x, umem better (noise 25.9 %) | 2.58x, jemalloc better (noise 2.3 %) |
| prodcons-sustained | jemalloc vs libc | 1.93x, jemalloc better (noise 16.8 %) | 0.31x, jemalloc better (noise 7.9 %) | 0.78x, jemalloc better (noise 17.8 %) | 0.65x, jemalloc better (noise 13.4 %) | 1.31x, libc better (noise 2.3 %) |

RESULT: PASS (0 invariant failures)
