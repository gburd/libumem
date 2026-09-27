#!/usr/bin/env bash
# scripts/ec2/heavy_load_compare.sh - the specific comparison the user asked
# for: libumem vs glibc vs jemalloc under heavy load on a 192-core NUMA metal
# box, hundreds of active threads, "run it for 15 minutes each, ignore the
# first 5 minutes".
#
# Runs ON the instance (Debian metal), inside an isolated build tree.  Emits
# a TOML result under docs/results/, via sustained_load.sh in CONTINUOUS
# mode:
#   * ONE bench_main process per (allocator, workload), running 15 x 60 s
#     intervals (bench_main -D 60:15).  The first 5 intervals (= the first 5
#     minutes of THAT process) are dropped; the next 10 are printed, one row
#     each.  Separate short processes would warm the OS but never the
#     allocator instance being measured.
#   * Whole 15-minute runs in the order given, workload-major:
#     prodcons: umem, libc, jemalloc, umem@null; then frag in the same order.
#     umem@null is a second, identical umem run placed LAST, so it brackets
#     the others in time: umem vs umem@null is this run's own noise and drift.
#   * No op budgets: every run is time-bounded, so throughput is ops done in
#     the same 10 measured minutes.
#   * frag sizes 64:256 by default (16:4096 at 192 threads holds ~10 GB live,
#     which measures umem's heap ceiling rather than fragmentation; see
#     sustained_load.sh).  frag live pool: SUSTAINED_FRAG_POOL objects/thread.
#
# Usage (on the instance, in the build tree):
#   THREADS=192 ./scripts/ec2/heavy_load_compare.sh
#   THREADS=384 SUSTAINED_OUT=heavy-384.toml ./scripts/ec2/heavy_load_compare.sh
set -euo pipefail

THREADS="${THREADS:-$(nproc)}"
ALLOCS="${ALLOCS:-umem,libc,jemalloc,umem@null}"

export SUSTAINED_MODE=continuous
export SUSTAINED_SEC="${SUSTAINED_SEC:-60}"
export SUSTAINED_WARMUPS="${SUSTAINED_WARMUPS:-5}"    # 5 x 60s = first 5 min dropped
export SUSTAINED_WINDOWS="${SUSTAINED_WINDOWS:-10}"   # 10 x 60s = 10 min measured
export SUSTAINED_FRAG_SIZES="${SUSTAINED_FRAG_SIZES:-64:256}"
# 100,000 live-pool objects per thread: at 64:256 (+16 B umem header) that is
# ~3 GB live at t=192 and ~6 GB at t=384 when pools are full -- heavy, and
# far below these boxes' 384 GB.
export SUSTAINED_FRAG_POOL="${SUSTAINED_FRAG_POOL:-100000}"
export SUSTAINED_OUT="${SUSTAINED_OUT:-heavy-load-compare-t${THREADS}.toml}"

nallocs=$(echo "$ALLOCS" | tr ',' '\n' | grep -c .)
per_run=$(( (SUSTAINED_WARMUPS + SUSTAINED_WINDOWS) * SUSTAINED_SEC ))
echo "=== heavy-load comparison ==="
echo "allocators   : $ALLOCS"
echo "workloads    : prodcons 64:256, frag $SUSTAINED_FRAG_SIZES"
echo "threads      : $THREADS  (box has $(nproc) online)"
echo "per run      : $SUSTAINED_WARMUPS + $SUSTAINED_WINDOWS intervals x ${SUSTAINED_SEC}s in ONE process"
echo "             = $(( per_run / 60 )) min, first $(( SUSTAINED_WARMUPS * SUSTAINED_SEC / 60 )) min dropped"
echo "total        : $(( 2 * nallocs )) runs = ~$(( 2 * nallocs * per_run / 60 )) min"
echo "numa         :"
numactl -H 2>/dev/null | sed 's/^/  /' || echo "  (numactl unavailable)"

# jemalloc must be resolvable or the run is a lie about a three-way comparison.
if echo "$ALLOCS" | tr ',' '\n' | grep -qx jemalloc; then
	jem=$(ldconfig -p 2>/dev/null | grep -oE '/[^ ]*libjemalloc\.so[^ ]*' | head -1 || true)
	if [ -z "$jem" ]; then
		echo "FATAL: jemalloc requested but libjemalloc.so not found (apt install libjemalloc2)" >&2
		exit 1
	fi
	echo "jemalloc     : $jem"
fi

# Every arm must actually load and run under the exported environment before
# 2 x 15 minutes per arm are spent: an allocator that bench_main cannot load
# prints no row and would silently drop out of the comparison.
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}${LD_LIBRARY_PATH:+:}$PWD/.libs"
export GLIBC_TUNABLES="${GLIBC_TUNABLES:-glibc.rtld.optional_static_tls=65536}"
for a in $(echo "$ALLOCS" | tr ',' ' '); do
	b="${a%%@*}"
	for w in prodcons frag; do
		if ! test/bench/.libs/bench_main -a "$b" -w "$w" -t "$THREADS" -n $((THREADS * 100000)) \
			-s 64:256 -c 2>/dev/null | grep -q "^$b,"; then
			echo "FATAL: preflight: $b $w at t=$THREADS produced no row (not loadable or crashed)" >&2
			exit 1
		fi
	done
done
echo "preflight    : every arm loads and runs prodcons + frag at t=$THREADS"

exec ./scripts/ec2/sustained_load.sh "$ALLOCS" "$SUSTAINED_SEC" "$THREADS"
