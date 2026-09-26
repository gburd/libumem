#!/usr/bin/env bash
# scripts/ec2/heavy_load_compare.sh - the specific comparison the user asked
# for: libumem vs glibc vs jemalloc under heavy load on a 192-core NUMA metal
# box, hundreds of active threads, 15 minutes per allocator with the first 5
# minutes discarded.
#
# Runs ON the instance (Debian metal), inside an isolated build tree.  Emits
# a TOML result under docs/results/.  Uses sustained_load.sh's proven
# window/warmup/alternating machinery:
#   * 60s windows.  5 warmup windows discarded (= first 5 min), 10 measured
#     windows kept (= next 10 min).  15 min wall per allocator, first 5 dropped,
#     exactly as asked -- and 10 measured windows give a real p999 distribution
#     rather than one coarse 10-min bucket.
#   * A,B,C,A,B,C,... interleave across umem/libc/jemalloc so thermal/neighbour
#     drift hits all three equally.
#   * Matched per-window operation budget per workload (calibrate once).
#
# Usage (on the instance, in the build tree):
#   THREADS=192 ./scripts/ec2/heavy_load_compare.sh
#   THREADS=384 WORKLOAD=prodcons ./scripts/ec2/heavy_load_compare.sh
set -euo pipefail

THREADS="${THREADS:-$(nproc)}"
ALLOCS="${ALLOCS:-umem,libc,jemalloc}"

# 15 min per allocator PER WORKLOAD, first 5 discarded, at 60s window
# granularity.  sustained_load.sh drives both prodcons (cross-thread
# alloc/free -- the NUMA all-to-all contention shape) and frag (fragmentation
# growth under a churning live set); both are what "heavy load" means here, so
# both are measured.
export SUSTAINED_SEC="${SUSTAINED_SEC:-60}"
export SUSTAINED_WARMUPS="${SUSTAINED_WARMUPS:-5}"    # 5 x 60s = 5 min discarded
export SUSTAINED_WINDOWS="${SUSTAINED_WINDOWS:-10}"   # 10 x 60s = 10 min measured
export SUSTAINED_OUT="${SUSTAINED_OUT:-heavy-load-compare.toml}"

echo "=== heavy-load comparison ==="
echo "allocators   : $ALLOCS"
echo "workloads    : prodcons + frag (both, per sustained_load.sh)"
echo "threads      : $THREADS  (box has $(nproc) online)"
echo "per alloc/wl : $SUSTAINED_WARMUPS warmup + $SUSTAINED_WINDOWS measured x ${SUSTAINED_SEC}s"
echo "             = $(( (SUSTAINED_WARMUPS + SUSTAINED_WINDOWS) * SUSTAINED_SEC / 60 )) min, first $(( SUSTAINED_WARMUPS * SUSTAINED_SEC / 60 )) min discarded"
echo "numa         :"
numactl -H 2>/dev/null | sed 's/^/  /' | head -6 || echo "  (numactl unavailable)"

# jemalloc must be resolvable or the run is a lie about a two-way comparison.
if echo "$ALLOCS" | tr ',' '\n' | grep -qx jemalloc; then
	jem=$(ldconfig -p 2>/dev/null | grep -oE '/[^ ]*libjemalloc\.so[^ ]*' | head -1 || true)
	if [ -z "$jem" ]; then
		echo "FATAL: jemalloc requested but libjemalloc.so not found (apt install libjemalloc-dev)" >&2
		exit 1
	fi
	echo "jemalloc     : $jem"
fi

# sustained_load.sh drives both prodcons and frag; the window/warmup/thread
# knobs above are exported into its environment.
exec ./scripts/ec2/sustained_load.sh "$ALLOCS" "$SUSTAINED_SEC" "$THREADS"
