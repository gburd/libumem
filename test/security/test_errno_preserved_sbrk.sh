#!/usr/bin/env bash
# The sbrk-backend arm of test_errno_preserved (P5.5).  vmem_sbrk_alloc()
# restored the caller's errno on its failure path, as vmem_mmap_alloc() did
# before P5.5, so a failed allocation from the sbrk heap returned NULL with
# errno unchanged.  Under RLIMIT_AS, brk() fails with ENOMEM just as mmap()
# does, so the same program checks it.
here=$(cd "$(dirname "$0")" && pwd)
# The libtool wrapper sets the library path; the raw binary is for hand-runs.
bin="$here/test_errno_preserved"
[[ -x $bin ]] || bin="$here/.libs/test_errno_preserved"
[[ -x $bin ]] || { echo "SKIP: test_errno_preserved not built"; exit 77; }
UMEM_OPTIONS="backend=sbrk${UMEM_OPTIONS:+,$UMEM_OPTIONS}" exec "$bin"
