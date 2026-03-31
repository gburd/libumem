/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2026 Gregory Burd. All rights reserved.
 * Use is subject to license terms.
 */

#ifndef MALLOC_GUARD_H
#define MALLOC_GUARD_H

/*
 * Per-thread recursion guard using initial-exec TLS model.
 *
 * This prevents deadlock when pthread_create calls malloc, which calls
 * pthread_getspecific, which calls malloc again.
 *
 * The initial-exec TLS model is used because:
 * 1. Single instruction access (mov %fs:offset, %eax on x86-64)
 * 2. No __tls_get_addr() call that might allocate memory
 * 3. Available immediately during thread creation
 *
 * Limitation: Only works with LD_PRELOAD, not dlopen().
 * See: docs/PTHREAD_RESEARCH.md section 4 for details.
 */

extern __thread int umem_malloc_recursion_depth
    __attribute__((tls_model("initial-exec")));

/*
 * Enter malloc context. Returns previous recursion depth.
 * If return value > 0, caller is in recursive malloc and should
 * use bootstrap allocator.
 */
static inline int
umem_enter_malloc(void)
{
	return umem_malloc_recursion_depth++;
}

/*
 * Exit malloc context. Must be called after every umem_enter_malloc().
 */
static inline void
umem_exit_malloc(void)
{
	umem_malloc_recursion_depth--;
}

/*
 * Check if currently in malloc context (recursion detected).
 * Returns non-zero if recursive malloc call is in progress.
 */
static inline int
umem_in_malloc(void)
{
	return umem_malloc_recursion_depth > 0;
}

#endif /* MALLOC_GUARD_H */
