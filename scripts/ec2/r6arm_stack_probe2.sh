#!/usr/bin/env bash
# scripts/ec2/r6arm_stack_probe2.sh -- second evidence pass for
# docs/results/2026-09-27-arm-dlopen-stack-crash.md (isolated tree).
#   * which umem_init step uses the stack (ncpus, stacktrace_init, numa, dlopen)
#   * where the 64 KiB x86 first-allocation SEGV faults (gdb bt)
#   * whether the over-deep frame on a 128 KiB arm thread writes into another
#     thread's memory instead of faulting (PROBE_NEIGHBOUR canaries)
set -u
./scripts/ec2/clean-regen.sh --enable-rseq CFLAGS="-O2 -g" >regen.log 2>&1 \
	|| { echo "SKIP: regen"; tail -20 regen.log; exit 77; }
make -j"$(nproc)" >make.log 2>&1 || { echo "SKIP: build"; tail -30 make.log; exit 77; }
cat ISOLATED_PROVENANCE; uname -rm; grep -E "HAVE_LIBDW|HAVE_LIBNUMA|HAVE_BACKTRACE" config.h
L="$PWD/.libs"; export LD_LIBRARY_PATH="$L"
gcc -O2 -g -DPROBE_NB -o probe test/integration/probe_first_alloc_stack.c -I. -L"$L" -lumem -ldl -pthread \
	|| { echo "SKIP: probe build"; exit 77; }
run() { local lab="$1"; shift; env "$@" >out.txt 2>&1; echo "$lab rc=$? $(tr '\n' '|' <out.txt)"; }
echo "== per-step depth, default thread =="
for m in ncpus stinit numa dlopen umem; do run "depth $m" ./probe $m 0 m; done
echo "== which step: default thread, UMEM_OPTIONS / UMEM_DEBUG variants =="
run "depth umem UMEM_DEBUG=default" UMEM_DEBUG=default ./probe umem 0 m
echo "== neighbour canaries, probe thread 128 KiB (arm PTHREAD_STACK_MIN) / 64 KiB =="
for k in 128 64; do run "neighbour umem ${k}K" PROBE_NEIGHBOUR=1 ./probe umem $k; done
run "neighbour glibc 128K" PROBE_NEIGHBOUR=1 ./probe malloc 128
echo "== under the harness tunable, default thread (8 MiB minus static TLS) =="
T8=glibc.rtld.optional_static_tls=8388608
run "tunable ncpus"          GLIBC_TUNABLES=$T8 ./probe ncpus 0
run "tunable umem"           GLIBC_TUNABLES=$T8 ./probe umem 0
run "tunable umem neighbour" GLIBC_TUNABLES=$T8 PROBE_NEIGHBOUR=1 ./probe umem 0
run "tunable ncpus neighbour" GLIBC_TUNABLES=$T8 PROBE_NEIGHBOUR=1 ./probe ncpus 0
run "no-tunable ncpus"       ./probe ncpus 0
echo "== bench prodcons 16t, tunable, x5: umem; jemalloc dlopen (no preload); jemalloc LD_PRELOAD =="
JE=$(ldconfig -p | grep -oE '/[^ ]*libjemalloc\.so\.2' | head -1)
B=test/bench/.libs/bench_main
for i in 1 2 3 4 5; do
	for arm in umem je-dlopen je-preload; do
		case $arm in
		umem) a=umem; P=X_=1 ;; je-dlopen) a=jemalloc; P=X_=1 ;; je-preload) a=jemalloc; P="LD_PRELOAD=$JE" ;;
		esac
		env GLIBC_TUNABLES=$T8 $P timeout 120 $B -a $a -w prodcons -t 16 -n 1600000 -s 64:256 -r 1 -W 0 -A -c >bench.txt 2>&1
		printf '%s=%s ' $arm $?
		[ $i = 1 ] && grep -m1 -iE '^[a-z].*(jemalloc|umem)' bench.txt | cut -c1-60
	done; echo
done
echo "== bisect the smallest surviving stack (KiB) =="
for k in 64 68 72 76 80 84 88 96 112 128 136 144 160; do
	./probe umem $k >/dev/null 2>&1; printf '%sK=%s ' $k $?
done; echo
echo "== gdb: first umem_alloc on the smallest stack that faults =="
k=64; [ "$(uname -m)" = aarch64 ] && k=128
gdb -q -batch -ex run -ex bt -ex 'info registers sp' -ex 'x/3i $pc' \
	--args ./probe umem $k 2>&1 | grep -vE '^\[|^Using host' | head -40
exit 0
