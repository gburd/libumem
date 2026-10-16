# Pre-PAL-refactor baseline (behaviour-preserving before-picture)

ref ee69eaa, intel-lo@palci (kiwi, c7i.8xlarge, Debian 13 trixie, gcc 14),
isolated build via verify-isolated.sh, 2026-10-16.

make check (default build): TOTAL 72, PASS 59, SKIP 13, XFAIL 0, FAIL 0,
XPASS 0, ERROR 0. libumem.so.2.0.0 + libumem_malloc.so.1 built clean.

The PAL/AAL refactor (item 1) MUST reproduce this exact pass set on x86_64 and
aarch64, and hotpath_ab.sh must be inside the null (the refactor relocates
platform code; it may not change behaviour or cost anything on the fast path).
