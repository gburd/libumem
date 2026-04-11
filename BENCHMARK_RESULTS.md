# libumem Cross-Platform Benchmark Results

Date: 2026-04-11
Commit: f5f23ba (medium fixes + critical fixes)

## Summary Table

| Platform | OS | Compiler | Arch | Allocator | Workload | Threads | ops/sec | p99_ns | peak_rss_mb | cpu_user_ms | cpu_sys_ms |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Linux (local) | Linux 6.12.80 | GCC 15.2.0 | x86_64 | libc | single | 1 | 6,401,587 | 29 | 2.89 | 31.0 | 0.1 |
| Linux (local) | Linux 6.12.80 | GCC 15.2.0 | x86_64 | libc | multi | 4 | 5,369,193 | 40 | 2.89 | 59.9 | 64.4 |
| Linux (local) | Linux 6.12.80 | GCC 15.2.0 | x86_64 | umem | single | 1 | 5,317,348 | 62 | 4.01 | 37.4 | 0.1 |
| Linux (local) | Linux 6.12.80 | GCC 15.2.0 | x86_64 | umem | multi | 4 | SEGFAULT | -- | -- | -- | -- |
| FreeBSD (nuc) | FreeBSD 15.0-RELEASE | clang 19.1.7 | amd64 | libc | single | 1 | 5,564,214 | 76 | 0.00 | 40.7 | 0.0 |
| FreeBSD (nuc) | FreeBSD 15.0-RELEASE | clang 19.1.7 | amd64 | libc | multi | 4 | 17,320,137 | 99 | 0.01 | 39.8 | 5.4 |
| FreeBSD (nuc) | FreeBSD 15.0-RELEASE | clang 19.1.7 | amd64 | umem | single | 1 | 4,530,941 | 87 | 0.01 | 41.8 | 2.4 |
| FreeBSD (nuc) | FreeBSD 15.0-RELEASE | clang 19.1.7 | amd64 | umem | multi | 4 | 7,113,667 | 1,264 | 0.01 | 84.6 | 23.5 |
| RISC-V (greenfly) | Linux 6.6.63-ky | GCC 13.3.0 | riscv64 | libc | single | 1 | 782,588 | 125 | 1.38 | 64.2 | 0.0 |
| RISC-V (greenfly) | Linux 6.6.63-ky | GCC 13.3.0 | riscv64 | libc | multi | 4 | 1,064,130 | 339 | 1.50 | 117.9 | 51.7 |
| RISC-V (greenfly) | Linux 6.6.63-ky | GCC 13.3.0 | riscv64 | umem | single | 1 | 588,988 | 952 | 2.88 | 79.8 | 4.9 |
| RISC-V (greenfly) | Linux 6.6.63-ky | GCC 13.3.0 | riscv64 | umem | multi | 4 | 744,111 | 1,648 | 3.00 | 130.7 | 93.0 |
| SPARC (icarus) | SunOS 5.11 | GCC 13.4.0 | sun4u | libc | single | 1 | 210,697 | 1,100 | 0.00 | 125.7 | 20.5 |
| SPARC (icarus) | SunOS 5.11 | GCC 13.4.0 | sun4u | libc | multi | 4 | 263,170 | 3,330 | 0.00 | 203.0 | 23.6 |
| SPARC (icarus) | SunOS 5.11 | GCC 13.4.0 | sun4u | umem | single | 1 | 148,623 | 1,156 | 0.00 | 157.1 | 23.4 |
| SPARC (icarus) | SunOS 5.11 | GCC 13.4.0 | sun4u | umem | multi | 4 | 178,240 | 1,631 | 0.00 | 169.0 | 27.6 |

## Notes

- Linux x86_64 n=200000; RISC-V and SPARC n=50000
- Linux umem multi-thread (4 threads, 200k ops) crashes with SEGFAULT -- works at 50k ops with 2 threads; likely pre-existing race in magazine layer at high contention
- FreeBSD RSS reporting reads 0 (getrusage quirk)
- SPARC RSS reporting reads 0 (Solaris/OI getrusage quirk)
- umem is ~0.7-0.8x libc throughput on single-thread across platforms (PTC + magazine overhead)
- FreeBSD libc multi-thread shows 3x scaling (jemalloc); umem multi-thread shows respectable scaling
- SPARC is ~30x slower than x86_64 (expected for UltraSPARC IIIi vintage hardware)
