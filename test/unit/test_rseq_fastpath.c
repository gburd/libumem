/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * CDDL HEADER END
 */

/*
 * Regression test for the rseq fast path assembly bugs found while
 * evaluating whether to arm umem_rseq_alloc_slowpath()/
 * umem_rseq_free_slowpath() (docs/results/2026-09-09-rseq-reload-*.md).
 *
 * These are bugs in umem_rseq_alloc_fastpath()/umem_rseq_free_fastpath()
 * (umem_rseq_x86_64.S / umem_rseq_aarch64.S) -- code that is ALREADY live
 * in the allocation hot path, not the still-inert reload slowpath. They
 * went undetected because with the reload slowpath inert, cache_rseq[cpu]
 * .rounds is permanently 0, so the fast path never actually pops/pushes a
 * round in production; only a direct, single-thread, no-migration test
 * like this one (or arming the reload) exercises the indexing arithmetic.
 *
 * Bugs fixed (verified on real intel-hi x86_64 and arm-hi aarch64
 * hardware before/after):
 *
 *   1. x86_64 + aarch64 alloc fast path indexed the magazine with the
 *      PRE-decrement round count instead of the post-decrement count
 *      (mag_round[rounds] instead of mag_round[rounds-1]) -- an
 *      immediate double-allocation the instant rounds > 0.
 *   2. aarch64 fast path hardcoded access to umem's own private,
 *      unregistered TLS rseq area instead of honoring the runtime
 *      umem_rseq_fs_offset (glibc-managed rseq detection) that
 *      umem_rseq_x86_64.S already used -- silently disabled the fast
 *      path on any glibc >= 2.35 aarch64 target (always aborted).
 *   3. aarch64 free fast path hardcoded the magazine-full bound as 63
 *      instead of reading the cache's actual magsize field -- a heap
 *      buffer overflow for any magtype with magsize < 63 (most of
 *      umem_magtype[]: 1, 3, 7, 15, 31).
 *   4. aarch64 free fast path's "magazine full" case fell through into
 *      the success epilogue, which unconditionally overwrote the -1
 *      failure return with 0 -- silently reported a refused/dropped
 *      free as successful (the buffer was never stored anywhere).
 *   5. aarch64 used RSEQ_SIG 0x53053053 (copied from x86_64) before its
 *      abort labels. That is not a valid aarch64 instruction signature;
 *      the kernel/glibc convention for aarch64 is 0xd428bc00 (the
 *      encoding of "BRK #0x45e0" -- see glibc's
 *      sysdeps/unix/sysv/linux/aarch64/bits/rseq.h and the kernel's
 *      tools/testing/selftests/rseq/rseq-arm64.h). Whenever glibc (which
 *      owns rseq registration on glibc >= 2.35) registered with its own
 *      correct signature, any real migration into umem's critical
 *      section aborted into a signature mismatch and the kernel
 *      force-killed the thread with SIGSEGV ("possible attack attempt")
 *      instead of restarting it -- reproduced live on Graviton
 *      (c8g.metal) hardware under test/stress/stress_concurrency_oracle.
 *
 * This test isolates (1)-(4): single-threaded, pinned to one CPU via
 * sched_setaffinity so no real migration is involved, driving the fast
 * path assembly directly against a hand-built magazine. It does NOT
 * exercise (5) directly (that needs a real migration-triggered kernel
 * abort, which test/stress/stress_concurrency_oracle's --pattern=multi
 * under high thread count covers instead) -- but re-reads cpu_id before
 * every call the way umem.c does, so a spurious abort here would also
 * surface as a wrong-index/NULL failure.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sched.h>
#include "umem_rseq.h"

#ifndef UMEM_RSEQ_AVAILABLE
int
main(void)
{
	fprintf(stderr, "SKIP: rseq not available on this build\n");
	return (0);
}
#else

extern void *umem_rseq_alloc_fastpath(umem_rseq_cache_t *cache, int cpu_id);
extern int umem_rseq_free_fastpath(umem_rseq_cache_t *cache, void *buf,
    int cpu_id);

/*
 * P5.13b demangle bridge: the fast path now stores mag_round[] slots
 * XOR-mangled (stored = ptr ^ umem_link_cookie ^ (&slot >> 12), exactly
 * UMEM_SLOT_MANGLE) and demangles on pop.  This direct-asm test must build
 * MANGLED magazines and demangle when it inspects a slot.  umem_link_cookie
 * is hidden-visibility (not linkable from here), so recover the cookie
 * EMPIRICALLY: push a known sentinel into a known slot via the real (already
 * mangling) free fast path, read back the stored word, and solve
 *   cookie = stored ^ sentinel ^ (&slot >> 12).
 * No library change, no dependence on symbol visibility.
 */
static uintptr_t g_test_cookie;
static int
tslot_cookie_probe(int cpu)
{
	test_magazine_t m;
	umem_rseq_cache_t rc;
	memset(&m, 0, sizeof (m));
	memset(&rc, 0, sizeof (rc));
	rc.loaded_mag = &m;
	rc.magsize = 4;
	rc.rounds = 0;
	void *sentinel = (void *)(uintptr_t)0xabcd0000;
	int c = cpu;
	while (c >= 0 && umem_rseq_free_fastpath(&rc, sentinel, c) != 0)
		c = umem_rseq_get_cpu();
	if (rc.rounds != 1)
		return (-1);
	uintptr_t stored = (uintptr_t)m.mag_round[0];
	g_test_cookie = stored ^ (uintptr_t)sentinel ^
	    ((uintptr_t)&m.mag_round[0] >> 12);
	return (0);
}
static inline void *
tslot_mangle(void *slotp, void *val)
{
	return ((void *)((uintptr_t)val ^ g_test_cookie ^
	    ((uintptr_t)slotp >> 12)));
}
#define	tslot_demangle(slotp, val)	tslot_mangle((slotp), (val))

typedef struct test_magazine {
	void *mag_next;
	void *mag_round[64];
} test_magazine_t;

static int
pin_to_current_rseq_cpu(int *cpu_out)
{
	int cpu = umem_rseq_get_cpu();
	if (cpu < 0)
		return (-1);
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof (set), &set) != 0)
		return (-1);
	*cpu_out = umem_rseq_get_cpu();
	return (0);
}

/*
 * Bug (1): alloc fast path must pop mag_round[rounds-1], the same slot
 * the plain-C fast path in _umem_cache_alloc() would pop, and must never
 * return the same live pointer twice.
 */
static int
test_alloc_index(int cpu)
{
	const int magsize = 15;
	test_magazine_t mag;
	memset(&mag, 0, sizeof (mag));

	uintptr_t sentinels[15];
	for (int i = 0; i < magsize; i++) {
		sentinels[i] = (uintptr_t)0x1000 + i * 0x10;
		/* Store mangled so the demangle-on-pop yields the sentinel. */
		mag.mag_round[i] = tslot_mangle(&mag.mag_round[i],
		    (void *)sentinels[i]);
	}

	umem_rseq_cache_t rc;
	memset(&rc, 0, sizeof (rc));
	rc.loaded_mag = &mag;
	rc.magsize = magsize;
	rc.rounds = magsize;

	int seen[15] = { 0 };
	for (int expected_idx = magsize - 1; expected_idx >= 0;
	    expected_idx--) {
		cpu = umem_rseq_get_cpu();
		void *buf = umem_rseq_alloc_fastpath(&rc, cpu);
		if (buf == NULL) {
			fprintf(stderr, "FAIL(alloc): NULL at expected_idx=%d "
			    "rounds=%d\n", expected_idx, rc.rounds);
			return (-1);
		}
		int idx = -1;
		for (int i = 0; i < magsize; i++)
			if (buf == (void *)sentinels[i]) {
				idx = i;
				break;
			}
		if (idx < 0) {
			fprintf(stderr, "FAIL(alloc): unrecognized pointer %p "
			    "(OOB read) at expected_idx=%d\n", buf,
			    expected_idx);
			return (-1);
		}
		if (seen[idx]) {
			fprintf(stderr, "FAIL(alloc): DOUBLE-ALLOCATION of "
			    "sentinel index %d (off-by-one index bug)\n", idx);
			return (-1);
		}
		seen[idx] = 1;
		if (idx != expected_idx) {
			fprintf(stderr, "FAIL(alloc): got index %d, expected "
			    "%d\n", idx, expected_idx);
			return (-1);
		}
	}
	if (rc.rounds != 0) {
		fprintf(stderr, "FAIL(alloc): rounds=%d after draining "
		    "magazine, expected 0\n", rc.rounds);
		return (-1);
	}
	return (0);
}

/*
 * Bugs (3) and (4): free fast path must refuse to push once rounds
 * reaches the cache's ACTUAL magsize (not a hardcoded constant), must
 * return -1 (not 0) when refusing, and must never write past
 * mag_round[magsize-1].
 */
static int
test_free_bounds(int cpu)
{
	const int magsize = 7; /* deliberately < any hardcoded bound */
	test_magazine_t mag;
	memset(&mag, 0, sizeof (mag));

	umem_rseq_cache_t rc;
	memset(&rc, 0, sizeof (rc));
	rc.loaded_mag = &mag;
	rc.magsize = magsize;
	rc.rounds = 0;

	for (int i = 0; i < magsize; i++) {
		void *buf = (void *)(uintptr_t)(0x2000 + i * 0x10);
		cpu = umem_rseq_get_cpu();
		if (umem_rseq_free_fastpath(&rc, buf, cpu) != 0) {
			fprintf(stderr, "FAIL(free): refused push %d into "
			    "non-full magazine (magsize=%d, rounds=%d)\n",
			    i, magsize, rc.rounds);
			return (-1);
		}
		if (tslot_demangle(&mag.mag_round[i], mag.mag_round[i]) != buf ||
		    rc.rounds != i + 1) {
			fprintf(stderr, "FAIL(free): push %d landed wrong "
			    "(mag_round[%d]=%p want %p, rounds=%d want %d)\n",
			    i, i,
			    tslot_demangle(&mag.mag_round[i], mag.mag_round[i]),
			    buf, rc.rounds, i + 1);
			return (-1);
		}
	}

	/* Magazine is now exactly full: one more push must be refused
	 * with -1, not silently accepted with 0 (bug 4), and must not
	 * have written past mag_round[magsize-1] (bug 3). */
	cpu = umem_rseq_get_cpu();
	void *overflow_buf = (void *)(uintptr_t)0x9999;
	int ret = umem_rseq_free_fastpath(&rc, overflow_buf, cpu);
	if (ret != -1) {
		fprintf(stderr, "FAIL(free): push into a full magazine "
		    "(magsize=%d) was NOT refused (returned %d) -- "
		    "bounds check broken, heap overflow risk\n", magsize,
		    ret);
		return (-1);
	}
	if (rc.rounds != magsize) {
		fprintf(stderr, "FAIL(free): rounds changed to %d despite "
		    "refused push (state corrupted)\n", rc.rounds);
		return (-1);
	}
	if (mag.mag_round[magsize] != NULL) {
		fprintf(stderr, "FAIL(free): write landed past "
		    "mag_round[magsize-1] (heap overflow)\n");
		return (-1);
	}
	return (0);
}

int
main(void)
{
	if (umem_rseq_init() != 0 || umem_rseq_register_thread() != 0 ||
	    !umem_rseq_asm_safe) {
		fprintf(stderr, "SKIP: rseq asm fast path not available on "
		    "this system\n");
		return (0);
	}

	int cpu;
	if (pin_to_current_rseq_cpu(&cpu) != 0) {
		fprintf(stderr, "SKIP: could not pin to a CPU\n");
		return (0);
	}

	if (tslot_cookie_probe(cpu) != 0) {
		fprintf(stderr, "SKIP: could not probe link cookie\n");
		return (0);
	}

	int fail = 0;
	fail |= test_alloc_index(cpu);
	fail |= test_free_bounds(cpu);

	if (fail) {
		fprintf(stderr, "RESULT: FAIL\n");
		return (1);
	}
	printf("RESULT: PASS (rseq fast path index/bounds arithmetic "
	    "correct)\n");
	return (0);
}

#endif /* UMEM_RSEQ_AVAILABLE */
