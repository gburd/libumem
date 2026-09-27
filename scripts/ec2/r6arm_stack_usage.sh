#!/usr/bin/env bash
# scripts/ec2/r6arm_stack_usage.sh -- per-function frame sizes of the library
# (gcc -fstack-usage), for the sibling sweep in
# docs/results/2026-09-27-arm-dlopen-stack-crash.md.  Isolated tree.
set -u
./scripts/ec2/clean-regen.sh --enable-rseq CFLAGS="-O2 -g -fstack-usage" >regen.log 2>&1 \
	|| { echo "SKIP: regen"; tail -20 regen.log; exit 77; }
make -j"$(nproc)" libumem.la libumem_malloc.la >make.log 2>&1 || { echo "SKIP: build"; tail -30 make.log; exit 77; }
cat ISOLATED_PROVENANCE; uname -m
echo "== library frames >= 512 B (file:line:col:function  bytes  qualifier) =="
find .libs -name '*.su' -print0 | xargs -0 cat | awk -F'\t' '$2 >= 512' | sort -t$'\t' -k2,2nr
L="$PWD/.libs"; export LD_LIBRARY_PATH="$L"
gcc -O2 -g -o probe test/integration/probe_first_alloc_stack.c -I. -L"$L" -lumem -ldl -pthread || exit 77
echo "== per-step depth (clean probe, no neighbour TLS) =="
for m in none ncpus stinit numa dlopen umem; do ./probe $m 0 m 2>&1 | tail -1; done
for k in 16 32 64 128; do printf 'umem %sK: ' $k; ./probe umem $k >/dev/null 2>&1; echo "rc=$?"; done
exit 0
