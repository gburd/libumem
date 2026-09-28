#!/usr/bin/env bash
# scripts/ec2/r8rss_hi.sh -- @r8rss metal check (c8g.metal-48xl): prodcons
# 1024:4096 t=192 rows for umem/jemalloc/libc, one gdb retention snapshot
# (r8rss_retention.py) and perf top for umem/jemalloc.  Diagnosis only.
set -u
O=$HOME/r8hi; mkdir -p "$O"
./scripts/ec2/clean-regen.sh >"$O/regen.log" 2>&1 || exit 1
make -j"$(nproc)" libumem.la test/bench/bench_main >"$O/make.log" 2>&1 || exit 1
export LD_LIBRARY_PATH=$PWD/.libs GLIBC_TUNABLES=glibc.rtld.optional_static_tls=65536
B=test/bench/.libs/bench_main
A="-w prodcons -t 192 -n 19200000 -s 1024:4096 -T 16 -c"
{ cat ISOLATED_PROVENANCE; nproc; uname -r; } >"$O/prov.txt"
for a in umem jemalloc libc; do $B -a $a $A -D 10:3 -W 1 2>/dev/null >>"$O/rows.csv"; done
$B -a umem $A -D 10:5 -W 1 >"$O/snap.csv" 2>&1 &
pid=$!; sleep 30
grep -E 'VmRSS|VmHWM' /proc/$pid/status >"$O/snap.txt"
sudo gdb -p $pid -batch -x scripts/ec2/r8rss_retention.py >>"$O/snap.txt" 2>&1
wait $pid
sudo sysctl -w kernel.perf_event_paranoid=-1 >/dev/null
for a in umem jemalloc; do
	perf record -F 499 -g -o "$O/p.data" -- $B -a $a $A -D 10:2 -W 1 >"$O/perf-$a.csv" 2>&1
	perf report -i "$O/p.data" --no-children --sort dso,sym --stdio -g none 2>/dev/null | grep -v '^$' | head -40 >"$O/perf-$a.top"
	perf report -i "$O/p.data" --children --sort sym --stdio -g none 2>/dev/null | grep -v '^$' | head -60 >"$O/perf-$a.children"
	unlink "$O/p.data"
done
echo HI_DONE
