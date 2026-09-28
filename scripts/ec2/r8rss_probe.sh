#!/usr/bin/env bash
# scripts/ec2/r8rss_probe.sh -- round-8 @r8rss diagnosis of O2-O5 (prodcons).
# Run inside an isolated tree (verify-isolated.sh).  Diagnosis only:
#  1. reproduce prodcons RSS/throughput for umem/libc/jemalloc at t=8/32/64
#     (t>nproc oversubscribes, mimicking t=384 on 192 vCPU);
#  2. umem attribution controls (tcache=0, norseq) at t=32;
#  3. gdb retention snapshot of a live umem prodcons run (r8rss_retention.py);
#  4. perf record/annotate at t=32 for the three allocators, perf stat at t=64.
set -u
OUT=${OUT:-$HOME/r8rss-out}
[ -d "$OUT" ] && mv "$OUT" "$OUT.old.$$"
mkdir -p "$OUT"
./scripts/ec2/clean-regen.sh >"$OUT/regen.log" 2>&1 || { cat "$OUT/regen.log"; exit 1; }
make -j"$(nproc)" libumem.la test/bench/bench_main >"$OUT/make.log" 2>&1 || { tail -30 "$OUT/make.log"; exit 1; }
export LD_LIBRARY_PATH=$PWD/.libs
export GLIBC_TUNABLES=glibc.rtld.optional_static_tls=65536
B=test/bench/.libs/bench_main
{ uname -a; nproc; cat ISOLATED_PROVENANCE; gcc --version | head -1; ldd --version | head -1
  ls -l /usr/lib/*/libjemalloc.so* 2>/dev/null; ldd $B; } >"$OUT/provenance.txt" 2>&1
$B -H >"$OUT/header.csv" 2>&1
CSV="$OUT/rows.csv"
bench() { # tag env-string -- bench args
	local tag=$1 envs=$2; shift 2
	env $envs $B "$@" -c 2>>"$OUT/stderr.log" | sed "s/^/$tag,/" >>"$CSV"
}
for s in 64:256 1024:4096; do for t in 8 32 64; do for a in umem libc jemalloc; do
	bench "repro-$s-t$t" "" -a $a -w prodcons -t $t -n $((t*100000)) -s $s -D 5:4 -W 1 -T 16
done; done; done
for o in tcache=0 norseq tcache=0,norseq; do
	bench "ctl-$o-t32" "UMEM_OPTIONS=$o" -a umem -w prodcons -t 32 -n 3200000 -s 64:256 -D 5:4 -W 1 -T 16
done
snap() { # tag t sizes
	$B -a umem -w prodcons -t $2 -n $(($2*100000)) -s $3 -D 5:8 -W 1 -T 16 -c >"$OUT/snap-$1.csv" 2>&1 &
	local pid=$!; sleep 22
	grep -E 'VmRSS|VmHWM|Threads' /proc/$pid/status >"$OUT/snap-$1.txt"
	sudo gdb -p $pid -batch -x scripts/ec2/r8rss_retention.py >>"$OUT/snap-$1.txt" 2>&1
	wait $pid
}
snap t32-64-256 32 64:256
snap t64-64-256 64 64:256
snap t64-1k-4k 64 1024:4096
sudo sysctl -w kernel.perf_event_paranoid=-1 >/dev/null 2>&1
for a in umem libc jemalloc; do
	d="$OUT/perf-$a-t32"
	perf record -F 999 -g -o $d.data -- $B -a $a -w prodcons -t 32 -n 3200000 -s 64:256 -D 5:3 -W 1 -T 16 -c >$d.csv 2>$d.err
	perf report -i $d.data --no-children --sort dso,sym --stdio -g none 2>/dev/null | grep -v '^$' | head -45 >$d.top
	perf report -i $d.data --children --sort sym --stdio -g none 2>/dev/null | grep -v '^$' | head -45 >$d.children
	perf report -i $d.data --no-children --sort sym --stdio -g caller,0.5,callee --percent-limit 3 2>/dev/null | head -150 >$d.callers
	for f in umem_free_wrapper _umem_free umem_alloc_wrapper _umem_alloc consumer_thread producer_thread; do
		perf annotate -i $d.data --stdio -s $f 2>/dev/null | grep -v '^\s*0.00 :' | awk '$1+0>=0.8' | head -30 >$d.ann-$f
	done
	unlink $d.data
done
for a in umem libc jemalloc; do
	perf stat -e task-clock,context-switches,cpu-migrations -o "$OUT/stat-$a-t64.txt" -- \
		$B -a $a -w prodcons -t 64 -n 6400000 -s 64:256 -D 5:4 -W 1 -T 16 -c >"$OUT/stat-$a-t64.csv" 2>&1
done
echo R8RSS_DONE
