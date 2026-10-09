#!/usr/bin/env bash
# scripts/ec2/r9f3_step0.sh -- Round 9 F3 step 0: CONFIRM OR REFUTE the
# locality mechanism before any allocator change.
#
# Claim under test (docs/plans/2026-09-28-round9.md "Gate order step 0",
# docs/results/r8rss/README.md "F3 / O4"): on metal, libumem's producer
# memset in the t=192 1-4 KB prodcons workload is ~10x costlier per op than
# jemalloc's because umem recycles freed objects anonymously (a freed object
# goes to a different producer than the one that reallocates it), so the
# producer memset writes COLD / REMOTELY-OWNED cache lines; jemalloc returns
# a freed object to its allocating thread, so reuse stays cache-warm. The
# r8rss 10x was derived from perf SHARE, not from cache-miss / HITM counters.
# This script measures the HITM counters.
#
# No library change. Runs the SAME bench invocation r8rss_hi.sh used, but
# swaps `perf record` for `perf c2c record` (and `perf mem` / `perf stat`
# fallbacks per arch). Raw perf c2c report is saved for both allocators.
set -u
O=$HOME/r9f3_step0; mkdir -p "$O"
./scripts/ec2/clean-regen.sh >"$O/regen.log" 2>&1 || { echo REGEN_FAIL; exit 1; }
make -j"$(nproc)" libumem.la test/bench/bench_main >"$O/make.log" 2>&1 || { echo MAKE_FAIL; exit 1; }
export LD_LIBRARY_PATH=$PWD/.libs
export GLIBC_TUNABLES=glibc.rtld.optional_static_tls=65536
B=test/bench/.libs/bench_main
# Same shape as r8rss_hi.sh: prodcons, t=192, 1024:4096, -c required by -D.
# -D 60:10 -W 5  = ten 60s intervals, drop first FIVE (= first 5 min dropped),
# measure the last 5 min in steady state. perf spans the whole run; the
# producer memset is identical across intervals so symbol attribution holds.
A="-w prodcons -t 192 -n 19200000 -s 1024:4096 -T 16 -c -D 60:10 -W 5"

{ cat ISOLATED_PROVENANCE 2>/dev/null; echo "nproc=$(nproc)"; echo "kernel=$(uname -r)"; echo "arch=$(uname -m)"; echo "os=$(. /etc/os-release; echo "$PRETTY_NAME")"; } >"$O/prov.txt"

sudo sysctl -w kernel.perf_event_paranoid=-1 >/dev/null 2>&1 || true
sudo sysctl -w kernel.perf_event_paranoid=-1 2>&1 | tee -a "$O/perf-setup.txt"
perf list 2>/dev/null | grep -iE 'hitm|remote|l3_miss|mem_|cache_miss|snoop|offcore|cross' > "$O/perf-list-relevant.txt" 2>&1 || true

for a in umem jemalloc; do
	echo "=== $a : throughput row ==="
	# One clean throughput run (no perf) for the row.
	$B -a "$a" $A 2>/dev/null >"$O/rows-$a.csv" || echo "ROW_FAIL $a"

	echo "=== $a : perf c2c record ==="
	if perf c2c record -o "$O/c2c-$a.data" -- $B -a "$a" $A >"$O/c2c-$a.run.log" 2>&1; then
		perf c2c report -i "$O/c2c-$a.data" --stdio 2>/dev/null >"$O/c2c-$a.report.txt" || echo "C2C_REPORT_FAIL $a"
		# Full detail (per cacheline, with symbols) for the producer store.
		perf c2c report -i "$O/c2c-$a.data" -NN --stdio -c tid,iaddr 2>/dev/null >"$O/c2c-$a.report.full.txt" || true
	else
		echo "C2C_RECORD_UNAVAILABLE $a -- falling back to perf mem / perf stat"
		# Fallback 1: perf mem (load/store sampling with data source).
		if perf mem record -o "$O/mem-$a.data" -- $B -a "$a" $A >"$O/mem-$a.run.log" 2>&1; then
			perf mem report -i "$O/mem-$a.data" --stdio 2>/dev/null >"$O/mem-$a.report.txt" || true
		else
			echo "MEM_RECORD_UNAVAILABLE $a"
		fi
	fi

	echo "=== $a : perf stat remote/HITM counters ==="
	# Fallback 2 / corroboration: per-arch counters. Try a broad set; perf
	# drops the ones this PMU lacks and keeps the rest.
	ARCH=$(uname -m)
	if [ "$ARCH" = "aarch64" ]; then
		# Graviton4 (Neoverse V2): remote/LLC refill events.
		EVENTS="instructions,cache-misses,ll_cache_rd,ll_cache_miss_rd,remote_access,remote_access_rd,l2d_cache_refill,llc_miss_rd"
	else
		# Sapphire Rapids (c7i): offcore / HITM / remote DRAM.
		EVENTS="instructions,cache-misses,mem_load_l3_miss_retired.remote_dram,mem_load_l3_miss_retired.remote_hitm,mem_load_l3_miss_retired.remote_fwd,mem_load_retired.l3_miss,offcore_response.demand_rfo.l3_miss.remote_hitm"
	fi
	perf stat -e "$EVENTS" -o "$O/stat-$a.txt" -- $B -a "$a" $A >"$O/stat-$a.run.log" 2>&1 || \
		perf stat -o "$O/stat-$a.txt" -- $B -a "$a" $A >"$O/stat-$a.run.log" 2>&1 || echo "STAT_FAIL $a"

	rm -f "$O/c2c-$a.data" "$O/mem-$a.data" 2>/dev/null || true
done
echo STEP0_DONE
