#!/usr/bin/env bash
#
# UMEM_DEBUG=audit must not re-enter the allocator from getpcstack().
#
# THE DEFECT (Codeberg bug report 2026-11-04, found by libdb's dst-rt suite)
#   With audit on, vmem_hash_insert() records a stack for every vmem segment,
#   holding vmem locks while the arena is half-populated.  getpcstack()'s
#   first call on each thread asked glibc pthread_getattr_np() for the stack
#   bounds, and pthread_getattr_np() mallocs (the CPU-affinity buffer).  Under
#   LD_PRELOAD that malloc is libumem's, so the allocator re-entered itself
#   from inside vmem_populate():
#     - rseq on (default): ASSERT(vmp->vm_cflags & VMC_POPULATOR) at
#       vmem.c:848 -> SIGABRT (rc 134), every run;
#     - UMEM_OPTIONS=norseq, or other timing: the update thread and the main
#       thread deadlock on vmem0.vm_lock / vmem_nosleep_lock (rc 124 here).
#   An in_lookup flag stopped recursion back into umem_stack_bounds() but not
#   the re-entrant malloc itself.
#
# THE FIX: umem_stack_bounds() reads the stack mapping from /proc/self/maps
#   with open/read into a stack buffer -- nothing on that path allocates.
#
# WHAT THIS RUNS: a one-malloc program, and one whose first malloc is on a
#   fresh thread (each thread takes the uncached lookup once), under
#   LD_PRELOAD=libumem_malloc.so with UMEM_DEBUG=audit and UMEM_DEBUG=default,
#   each with and without UMEM_OPTIONS=norseq, UMEM_AUDIT_ITERS times (default
#   20; the report used 100), every run killed with SIGKILL after 10 s.  Each
#   must exit 0.
#
# CONTROL: the same programs under UMEM_DEBUG=guards (no audit, so getpcstack
#   is never reached on the vmem path) must exit 0 too, so a failure here is
#   about audit and not about the preload or the harness.
#
# Exit: 0 pass, 1 the defect reproduces, 77 prerequisite missing, 99 harness.
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PRELOAD=""
for c in "$ROOT"/.libs/libumem_malloc.so "$ROOT"/.libs/libumem_malloc.so.*; do
	[[ -f $c ]] && { PRELOAD=$c; break; }
done
[[ -n $PRELOAD ]] || { echo "SKIP: libumem_malloc.so not built"; exit 77; }
CC=${CC:-cc}
command -v "$CC" >/dev/null 2>&1 || CC=gcc
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: no C compiler"; exit 77; }

ITERS=${UMEM_AUDIT_ITERS:-20}
T=$(mktemp -d) || exit 99
trap 'rm -rf "$T"' EXIT

cat >"$T/one.c" <<'EOF'
#include <stdlib.h>
int main(void) { void *p = malloc(16); free(p); return p == NULL; }
EOF
cat >"$T/thr.c" <<'EOF'
#include <pthread.h>
#include <stdlib.h>
static void *w(void *a) { void *p = malloc(16); free(p); return a; }
int main(void)
{
	pthread_t t;
	if (pthread_create(&t, NULL, w, NULL) != 0) return 2;
	pthread_join(t, NULL);
	void *p = malloc(16); free(p);
	return 0;
}
EOF
# -O0 so the compiler cannot fold the malloc/free pair away.
"$CC" -O0 -o "$T/one" "$T/one.c" >"$T/cc.log" 2>&1 &&
"$CC" -O0 -pthread -o "$T/thr" "$T/thr.c" >>"$T/cc.log" 2>&1 ||
	{ echo "HARNESS: could not build probes"; cat "$T/cc.log"; exit 99; }

# run_bounded <UMEM_DEBUG> <UMEM_OPTIONS> <prog>: run once, 10 s limit,
# SIGKILL on expiry.  timeout(1) is not enough: a hung probe was observed
# to outlive `timeout 10` for 17 h on lixr (the uutils timeout there stayed
# in futex_wait).  So watch the pid ourselves and kill -9 it.  Returns the
# probe's exit status, or 124 when it had to be killed.
run_bounded() {
	local dbg=$1 opt=$2 prog=$3 pid t
	env LD_PRELOAD="$PRELOAD" UMEM_DEBUG="$dbg" UMEM_OPTIONS="$opt" \
	    "$prog" >/dev/null 2>&1 &
	pid=$!
	for ((t = 0; t < 100; t++)); do
		kill -0 "$pid" 2>/dev/null || { wait "$pid"; return $?; }
		sleep 0.1
	done
	kill -9 "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	return 124
}

# run_mode <label> <UMEM_DEBUG> <UMEM_OPTIONS>: prints a summary; returns 1 on
# any non-zero exit.
run_mode() {
	local label=$1 dbg=$2 opt=$3 prog rc i bad=0 seen
	for prog in one thr; do
		seen=""
		for ((i = 0; i < ITERS; i++)); do
			run_bounded "$dbg" "$opt" "$T/$prog"
			rc=$?
			if [[ $rc -ne 0 ]]; then
				bad=1
				[[ " $seen " == *" $rc "* ]] || seen="$seen $rc"
			fi
		done
		if [[ -z $seen ]]; then
			printf '  ok   %-28s %-3s %d/%d exit 0\n' "$label" "$prog" "$ITERS" "$ITERS"
		else
			printf '  FAIL %-28s %-3s exit codes:%s (134=SIGABRT, 124=hung)\n' \
			    "$label" "$prog" "$seen"
		fi
	done
	return $bad
}

echo "[control] UMEM_DEBUG=guards (no audit on the vmem path)"
run_mode "guards" "guards" "" ||
	{ echo "HARNESS: control arm failed; cannot attribute the audit arms"; exit 99; }

fail=0
echo "[test] audit / default (default includes audit)"
run_mode "audit"                "audit"   ""       || fail=1
run_mode "audit+norseq"         "audit"   "norseq" || fail=1
run_mode "default"              "default" ""       || fail=1
run_mode "default+norseq"       "default" "norseq" || fail=1

if (( fail )); then
	echo "RESULT: FAIL (UMEM_DEBUG=audit re-enters the allocator from getpcstack)"
	exit 1
fi
echo "RESULT: PASS (audit survives the first allocation on the main and a new thread)"
exit 0
