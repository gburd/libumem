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
 * umem_env_helper.c -- exec-helper probe binary.
 *
 * Each invocation runs ONE probe in a fresh process so that UMEM_* env
 * vars (read once at umem init) actually take effect.  Used by
 * test_envvar.c and test_umem_debug.c via posix_spawn/execve.
 *
 * Usage: umem_env_helper <check> [arg]
 *   flag <hexmask>      exit 0 if (umem_flags & mask) == mask, else 1
 *   opt  <var> <expect> exit 0 if the named global == expected value
 *   alive               alloc/free once, exit 0 (init smoke)
 *   audit_sizes         alloc/free a spread of sizes under audit (exit 0 if all succeed)
 *   overflow            heap-overflow then free (should abort under guards)
 *   firewall_overflow   overflow a firewall-guarded buffer (should SIGSEGV)
 *   double_free         free the same buffer twice (should abort)
 *   uaf                 touch freed memory then churn (should abort under deadbeef)
 * A probe that *survives* corruption returns 0 => detection FAILED.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "umem.h"
#include "umem_impl.h"   /* UMF_* masks, umem_flags */
#ifndef UMEM_STANDALONE
#include "vmem_base.h"    /* VMEM_BACKEND_*, vmem_backend/allocator */
#endif

/* Config globals parsed from env at init (see envvar.c). */
extern uint32_t umem_flags;
extern uint32_t umem_stack_depth;
extern uint32_t umem_output;
extern size_t umem_content_maxsave;
extern size_t umem_minfirewall;
extern size_t umem_maxverify;
extern uint32_t umem_abort;
extern uint32_t umem_mtbf;
extern uint32_t umem_max_ncpus;
extern uint32_t umem_depot_contention;
extern uint32_t umem_reap_interval;
extern size_t umem_ptc_size;
extern uint32_t umem_logging;
extern size_t umem_transaction_log_size;
extern size_t umem_content_log_size;
extern size_t umem_failure_log_size;
extern size_t umem_slab_log_size;
#ifndef UMEM_STANDALONE
extern uint_t vmem_backend;
extern uint_t vmem_allocator;
#endif

/*
 * "opt" probe: compare a named config global against an expected value.
 * Exact-equal for scalars; for the vmem bitmask "backend" the expected
 * value is treated as a mask that must be set.
 * Returns 0 on match, 1 on mismatch, 2 if the name is unknown.
 */
static int
check_opt(const char *name, const char *expect)
{
	unsigned long long want = strtoull(expect, NULL, 0);

#ifndef UMEM_STANDALONE
	if (!strcmp(name, "backend"))
		return ((vmem_backend & want) == want) ? 0 : 1;
	if (!strcmp(name, "allocator"))
		return ((unsigned long long)vmem_allocator == want) ? 0 : 1;
#endif
#define MATCH_U(n, var) if (!strcmp(name, n)) return ((unsigned long long)(var) == want) ? 0 : 1;
	MATCH_U("stack_depth", umem_stack_depth)
	MATCH_U("output", umem_output)
	MATCH_U("content_maxsave", umem_content_maxsave)
	MATCH_U("minfirewall", umem_minfirewall)
	MATCH_U("maxverify", umem_maxverify)
	MATCH_U("abort", umem_abort)
	MATCH_U("mtbf", umem_mtbf)
	MATCH_U("max_ncpus", umem_max_ncpus)
	MATCH_U("depot_contention", umem_depot_contention)
	MATCH_U("reap_interval", umem_reap_interval)
	MATCH_U("ptc_size", umem_ptc_size)
	MATCH_U("logging", umem_logging)
	MATCH_U("transaction_log_size", umem_transaction_log_size)
	MATCH_U("content_log_size", umem_content_log_size)
	MATCH_U("failure_log_size", umem_failure_log_size)
	MATCH_U("slab_log_size", umem_slab_log_size)
#undef MATCH_U
	return 2;
}

int
main(int argc, char **argv)
{
	if (argc < 2)
		return 2;

	/* Force init so env-derived flags are applied. */
	void *warm = umem_alloc(64, UMEM_DEFAULT);
	umem_free(warm, 64);

	const char *cmd = argv[1];

	if (!strcmp(cmd, "flag")) {
		if (argc < 3)
			return 2;
		uint32_t mask = (uint32_t)strtoul(argv[2], NULL, 0);
		return (umem_flags & mask) == mask ? 0 : 1;
	}
	if (!strcmp(cmd, "opt")) {
		if (argc < 4)
			return 2;
		return check_opt(argv[2], argv[3]);
	}
	if (!strcmp(cmd, "alive"))
		return 0;
	if (!strcmp(cmd, "audit_sizes")) {
		/*
		 * Under UMEM_DEBUG=audit (set by the parent), every alloc is
		 * backed by an audit bufctl.  Allocate a spread of sizes that
		 * hit several size-class caches, verify none return NULL, and
		 * free them.  Exits non-zero if any allocation fails.
		 */
		void *ptrs[10];
		for (int i = 0; i < 10; i++) {
			ptrs[i] = umem_alloc(64 + i * 8, UMEM_DEFAULT);
			if (ptrs[i] == NULL)
				return 1;
		}
		for (int i = 0; i < 10; i++)
			umem_free(ptrs[i], 64 + i * 8);
		return 0;
	}
	if (!strcmp(cmd, "overflow")) {
		char *p = umem_alloc(64, UMEM_DEFAULT);
		memset(p, 0xAA, 96);          /* 32B past end */
		umem_free(p, 64);             /* should abort under guards */
		return 0;                     /* survived => detection FAILED */
	}
	if (!strcmp(cmd, "firewall_overflow")) {
		/*
		 * Firewall places each buffer flush against an unmapped guard
		 * page.  Requires UMEM_DEBUG=firewall=<N> with this size >= N,
		 * and a size that is a multiple of the cache alignment so
		 * buf+size lands exactly on the guard page.  Writing past the
		 * end must fault immediately (SIGSEGV), before any free.
		 */
		size_t sz = 4096;
		volatile char *p = umem_alloc(sz, UMEM_DEFAULT);
		if (p == NULL)
			return 3;
		p[sz] = 0x7f;                 /* one byte past end -> guard page */
		p[sz + 64] = 0x7f;            /* belt and suspenders */
		umem_free((void *)p, sz);
		return 0;                     /* survived => firewall FAILED */
	}
	if (!strcmp(cmd, "double_free")) {
		char *p = umem_alloc(64, UMEM_DEFAULT);
		umem_free(p, 64);
		umem_free(p, 64);
		return 0;                     /* survived => detection FAILED */
	}
	if (!strcmp(cmd, "uaf")) {
		char *p = umem_alloc(64, UMEM_DEFAULT);
		umem_free(p, 64);
		memset(p, 0x55, 64);          /* touch freed (DEADBEEF verify) */
		void *q = umem_alloc(64, UMEM_DEFAULT);
		umem_free(q, 64);
		return 0;
	}
	return 2;
}
