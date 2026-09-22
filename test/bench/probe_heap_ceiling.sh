#!/usr/bin/env bash
# probe_heap_ceiling.sh -- why does umem's mmap heap stop growing at ~5.3GB?
#
# probe_alloc_failure established: uniform failure across every size class,
# heap arena free=0 and unable to grow, errno=0, reap+retry does not help, and
# libc reaching 96GB VmHWM on the same box.  So it is a global arena limit, and
# the arena cannot obtain more memory from the OS.
#
# errno=0 is NOT evidence that mmap succeeded: vmem_mmap_top_alloc() saves
# errno on entry and restores it on every exit path (vmem_mmap.c:143,183,190),
# so a genuine mmap ENOMEM is erased before any caller can see it.  The failure
# is therefore unobservable through errno by construction.
#
# Leading hypothesis: vm.max_map_count.  umem's mmap backend grows the heap by
# mmap()ing chunks and extending the arena, each of which can create a separate
# VMA.  Linux caps VMAs per process at vm.max_map_count (default 65530) and
# returns ENOMEM from mmap once that is hit -- while still having plenty of
# free RAM, which is exactly the shape observed (failing at 5GB on a box where
# libc reached 96GB).
#
# This script measures the VMA count against the heap size as it grows, which
# either confirms or refutes that directly.
set -uo pipefail
cd "$(dirname "$0")/../.."
export LD_LIBRARY_PATH="$PWD/.libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

BIN=test/bench/.libs/probe_alloc_failure
[[ -x $BIN ]] || BIN=test/bench/probe_alloc_failure
if [[ ! -x $BIN ]]; then
	echo "SKIP: probe_alloc_failure not built"
	exit 77
fi

echo "=== kernel limits ==="
echo "vm.max_map_count   = $(cat /proc/sys/vm/max_map_count 2>/dev/null || echo unknown)"
echo "vm.overcommit_memory = $(cat /proc/sys/vm/overcommit_memory 2>/dev/null || echo unknown)"
echo "MemTotal           = $(awk '/MemTotal/{print $2" kB"}' /proc/meminfo)"
echo "ulimit -v          = $(ulimit -v)"
echo "ulimit -d          = $(ulimit -d)"
echo ""

# Run the umem arm in the background and sample its VMA count against RSS.
echo "=== sampling VMA count while umem grows its heap ==="
"$BIN" 16 400000 > /tmp/ceiling_probe.log 2>&1 &
pid=$!
echo "pid=$pid"
printf '%8s %12s %12s %14s\n' "sec" "VMAs" "VmRSS_kB" "VmSize_kB"
max_vma=0
for i in $(seq 1 120); do
	[[ -d /proc/$pid ]] || break
	vma=$(wc -l < /proc/$pid/maps 2>/dev/null || echo 0)
	rss=$(awk '/VmRSS/{print $2}' /proc/$pid/status 2>/dev/null || echo 0)
	vsz=$(awk '/VmSize/{print $2}' /proc/$pid/status 2>/dev/null || echo 0)
	(( vma > max_vma )) && max_vma=$vma
	if (( i % 5 == 0 )); then
		printf '%8s %12s %12s %14s\n' "$i" "$vma" "$rss" "$vsz"
	fi
	sleep 1
done
wait $pid 2>/dev/null
echo ""
echo "peak VMA count observed = $max_vma"
echo "vm.max_map_count        = $(cat /proc/sys/vm/max_map_count 2>/dev/null || echo unknown)"
echo ""
echo "=== probe output ==="
grep -E "arm |ok=|FIRST FAILURE|vmem\[" /tmp/ceiling_probe.log || cat /tmp/ceiling_probe.log

echo ""
echo "=== interpretation ==="
limit=$(cat /proc/sys/vm/max_map_count 2>/dev/null || echo 0)
if (( max_vma > 0 && limit > 0 )); then
	pct=$(( max_vma * 100 / limit ))
	echo "peak VMAs = $max_vma of $limit ($pct%)"
	if (( pct >= 90 )); then
		echo "=> CONFIRMS vm.max_map_count as the ceiling: the process ran out"
		echo "   of VMA slots, so mmap returned ENOMEM with RAM still free."
		echo "   vmem_mmap_top_alloc() then restored errno, erasing the cause."
	else
		echo "=> DOES NOT confirm vm.max_map_count: the process was well below"
		echo "   the VMA limit when allocations began failing.  The ceiling is"
		echo "   something else; do not report max_map_count as the cause."
	fi
fi
