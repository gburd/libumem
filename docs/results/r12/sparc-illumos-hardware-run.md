# UltraSPARC / illumos HARDWARE run (closes the v4.2.0 owed item)

Host "sun" over ssh: real UltraSPARC (sun4u, SUNW,A70), OpenIndiana (illumos).
This is the first ACTUAL SPARCv9 + illumos run; prior rounds were
cross-compile-only. 2026-10-16, libumem master 2d8645f.

Environment:
```
SunOS sun 5.11 illumos-31d3d510d0 sun4u sparc SUNW,A70
gcc (OpenIndiana 13.4.0-oi-2) 13.4.0
64-bit sparcv9 applications
```

## Build
- clean-regen + ./configure + gmake: the CORE LIBRARY builds clean.
  libumem.so.2.0.0 = "ELF 64-bit MSB SPARCV9, UltraSPARC1 Extensions Required".
  libumem_malloc.so.1.0.0 links (pulls -lnsl -lsocket, the illumos socket libs
  the PAL names). The SPARC getpcstack walk (aal/sparc_subr_sol.S) compiles.
- One NON-allocator build failure: test/bench/bench_affinity_vs_mutex.c uses
  Linux-only CPU_ZERO/CPU_SET/sched_setaffinity -> does not compile on illumos.
  It is a BENCHMARK, not the library or a functional test. FOLLOW-UP: guard
  that bench behind a Linux #ifdef (or a PAL cpu-affinity shim) so `gmake`
  (the full default target) completes on illumos. Does not affect the lib or
  make check's functional tests.

## Functional tests (real SPARCv9 silicon)
- umem_test: 3/3 PASS (basic alloc, PTC enabled, post-init alloc).
- umem_test2: 3/3 PASS incl. a 4-thread stress, 4000 allocs/thread.
- umem_test3: 4/4 PASS (11 size classes, 1000 rapid alloc/free cycles).
- LD_PRELOAD drop-in (libumem_malloc.so.1) into a 64-bit program:
  malloc/calloc/realloc/free all work, rc 0.

## The "Killed" red herring (diagnosed, not a bug)
Preloading the 64-bit libumem_malloc.so into the system /bin/true,/usr/bin/ls
etc. failed with "ld.so.1: wrong ELF class: ELFCLASS64" -> those system
binaries are 32-bit; ld.so.1 correctly refuses a 64-bit lib in a 32-bit
process. This is the documented illumos dual-ABI case (README: build both
ABIs into lib and lib/64). Into a 64-bit target the interposer works. Not a
libumem defect.

## Verdict
libumem's SPARCv9/illumos support is now HARDWARE-VALIDATED (build + functional
+ multithreaded + interposer), not merely cross-compiling. One Linux-ism in a
bench file is the only portability gap found, and it is non-blocking.
