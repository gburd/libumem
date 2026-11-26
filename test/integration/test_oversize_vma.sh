#!/usr/bin/env bash
# P6.2: both arms.  Default must not cost a VMA per freed oversize object;
# the guard arm (every span guarded) must, proving the PROT_NONE path exists
# and the default arm's pass is not vacuous.
here=$(cd "$(dirname "$0")" && pwd)
bin="$here/test_oversize_vma"
[[ -x $bin ]] || bin="$here/.libs/test_oversize_vma"
[[ -x $bin ]] || { echo "SKIP: test_oversize_vma not built"; exit 77; }
# Both arms test the mmap heap's span return; the sbrk heap never returns a
# span to the kernel, so the backend is pinned whatever the default is.
rc=0
UMEM_OPTIONS="backend=mmap${UMEM_OPTIONS:+,$UMEM_OPTIONS}" "$bin" || rc=$?
[[ $rc -eq 77 ]] && exit 77
UMEM_OPTIONS="backend=mmap,mmap_guard=4096${UMEM_OPTIONS:+,$UMEM_OPTIONS}" \
	"$bin" guard || rc=1
exit $rc
