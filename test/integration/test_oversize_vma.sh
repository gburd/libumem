#!/usr/bin/env bash
# P6.2: both arms.  Default must not cost a VMA per freed oversize object;
# the guard arm (every span guarded) must, proving the PROT_NONE path exists
# and the default arm's pass is not vacuous.
here=$(cd "$(dirname "$0")" && pwd)
bin="$here/test_oversize_vma"
[[ -x $bin ]] || bin="$here/.libs/test_oversize_vma"
[[ -x $bin ]] || { echo "SKIP: test_oversize_vma not built"; exit 77; }
rc=0
"$bin" || rc=$?
[[ $rc -eq 77 ]] && exit 77
UMEM_OPTIONS="mmap_guard=4096${UMEM_OPTIONS:+,$UMEM_OPTIONS}" "$bin" guard || rc=1
exit $rc
