#!/usr/bin/env bash
# scripts/ec2/r6arm_stack_probe.sh -- evidence run for
# docs/results/2026-09-27-arm-dlopen-stack-crash.md.  Run from the root of an
# isolated tree (verify-isolated.sh).  Builds the library (--enable-rseq -O2 -g,
# the shipped gate config), then:
#   1. usable stack below sp on a fresh default thread, with and without
#      GLIBC_TUNABLES=glibc.rtld.optional_static_tls=8388608;
#   2. peak stack depth of the FIRST allocation on a fresh thread for umem,
#      glibc malloc, jemalloc (LD_PRELOAD), libumem_malloc.so (LD_PRELOAD),
#      bare dlopen(NULL) and umem_get_max_ncpus();
#   3. survival of the first allocation on 16/32/48/64/128/256 KiB threads;
#   4. bench_main prodcons 16t under the tunable, umem vs jemalloc.
set -u
./scripts/ec2/clean-regen.sh --enable-rseq CFLAGS="-O2 -g" >regen.log 2>&1 \
	|| { echo "SKIP: regen"; tail -20 regen.log; exit 77; }
make -j"$(nproc)" >make.log 2>&1 || { echo "SKIP: build"; tail -30 make.log; exit 77; }
cat ISOLATED_PROVENANCE; uname -rm; nproc; getconf PAGESIZE
gcc --version | head -1; ldd --version | head -1
L="$PWD/.libs"
export LD_LIBRARY_PATH="$L"
gcc -O2 -g -o probe test/integration/probe_first_alloc_stack.c -I. -L"$L" -lumem -ldl -pthread \
	|| { echo "SKIP: probe build"; exit 77; }
JE=$(ldconfig -p | grep -oE '/[^ ]*libjemalloc\.so\.2' | head -1)
echo "jemalloc=$JE"
T8=glibc.rtld.optional_static_tls=8388608
run() { # label, env..., then probe args
	local lab="$1"; shift
	env "$@" >out.txt 2>&1; local rc=$?
	echo "$lab rc=$rc $(tail -1 out.txt)"
}
echo "== 1. usable stack on a default thread (mode none) =="
run "no-tunable"  ./probe none 0 m
run "tunable=8M"  GLIBC_TUNABLES=$T8 ./probe none 0 m
run "tunable=64K" GLIBC_TUNABLES=glibc.rtld.optional_static_tls=65536 ./probe none 0 m
echo "== 2. first-call peak stack depth (default 8 MiB thread, painted) =="
for m in umem dlopen ncpus; do run "depth $m" ./probe $m 0 m; done
run "depth glibc-malloc" ./probe malloc 0 m
[ -n "$JE" ] && run "depth jemalloc-malloc" LD_PRELOAD="$JE" ./probe malloc 0 m
run "depth umem_malloc.so" LD_PRELOAD="$L/libumem_malloc.so" ./probe malloc 0 m
run "depth umem norseq" UMEM_OPTIONS=norseq ./probe umem 0 m
echo "== 3. first allocation on a small explicit stack =="
for k in 16 20 24 32 40 48 64 128 256; do
	run "umem   ${k}K" ./probe umem $k
	run "glibc  ${k}K" ./probe malloc $k
	[ -n "$JE" ] && run "jemal  ${k}K" LD_PRELOAD="$JE" ./probe malloc $k
	run "umem_malloc.so ${k}K" LD_PRELOAD="$L/libumem_malloc.so" ./probe malloc $k
done
echo "== 4. bench_main prodcons 16t under the tunable =="
B=test/bench/.libs/bench_main
for a in umem jemalloc; do
	for tv in "$T8" "glibc.rtld.optional_static_tls=65536"; do
		if [ "$a" = jemalloc ] && [ -n "$JE" ]; then P="LD_PRELOAD=$JE"; else P="X_=1"; fi
		env GLIBC_TUNABLES="$tv" $P timeout 120 $B -a $a -w prodcons -t 16 -n 1600000 \
			-s 64:256 -r 1 -W 0 -A -c >bench.txt 2>&1
		echo "bench a=$a tunable=$tv rc=$? $(grep -ciE 'segm|error' bench.txt) err-lines"
	done
done
exit 0
