#!/usr/bin/env bash
# scripts/ec2/r6arm_fix_experiment.sh <patchset> -- EXPERIMENT, not evidence of
# a fix: applies candidate edits with sed to an isolated tree, builds, and
# measures first-allocation stack depth and small-stack survival, to choose a
# regression budget the fix can meet.  patchset: none | static | static+err
set -u
PS="${1:-none}"
case "$PS" in
static|static+err)
	sed -i 's/^    char proc_stat\[65536\];/    static char proc_stat[65536];/' init_lib.c
	grep -n "static char proc_stat" init_lib.c || { echo "SKIP: sed static"; exit 77; } ;;
esac
case "$PS" in
static+err)
	sed -i 's/^#define\tUMEM_MAX_ERROR_SIZE 4096/#define\tUMEM_MAX_ERROR_SIZE 1024/' misc.c
	grep -n "UMEM_MAX_ERROR_SIZE 1024" misc.c || { echo "SKIP: sed err"; exit 77; } ;;
esac
./scripts/ec2/clean-regen.sh --enable-rseq CFLAGS="-O2 -g -fstack-usage" >regen.log 2>&1 \
	|| { echo "SKIP: regen"; tail -20 regen.log; exit 77; }
make -j"$(nproc)" >make.log 2>&1 || { echo "SKIP: build"; tail -30 make.log; exit 77; }
cat ISOLATED_PROVENANCE; echo "patchset=$PS"; uname -m
L="$PWD/.libs"; export LD_LIBRARY_PATH="$L"
gcc -O2 -g -o probe test/integration/probe_first_alloc_stack.c -I. -L"$L" -lumem -ldl -pthread || exit 77
echo "== frames >= 1024 B =="; find .libs -name '*.su' -print0 | xargs -0 cat | awk -F'\t' '$2 >= 1024' | sort -t$'\t' -k2,2nr
echo "== depth, default thread (lazy binding / LD_BIND_NOW) =="
./probe umem 0 m | tail -1
LD_BIND_NOW=1 ./probe umem 0 m | tail -1
./probe malloc 0 m | tail -1
UMEM_OPTIONS=bogus_option ./probe umem 0 m 2>&1 | tail -1
echo "== survival: explicit small stacks =="
for k in 16 20 24 32 64 128; do printf 'umem %sK=' $k; ./probe umem $k >/dev/null 2>&1; echo -n "$? "; done; echo
printf 'umem 16K bogus-opt='; UMEM_OPTIONS=bogus_option ./probe umem 16 >/dev/null 2>&1; echo $?
echo "== survival: first umem_alloc with N bytes of stack left (PROBE_LEAVE) =="
for n in 4096 5120 6144 7168 7680 8192 10240 12288 16384; do
	printf '%s=' $n; PROBE_LEAVE=$n ./probe umem 0 >/dev/null 2>&1; r1=$?
	PROBE_LEAVE=$n ./probe malloc 0 >/dev/null 2>&1; printf 'umem:%s,glibc:%s ' $r1 $?
done; echo
echo "== x86 no-crash under the tunable: where does the 64 KiB frame land? =="
GLIBC_TUNABLES=glibc.rtld.optional_static_tls=8388608 PROBE_NEIGHBOUR=1 PROBE_WHERE=66000 ./probe umem 0 2>&1; echo "rc=$?"
exit 0
