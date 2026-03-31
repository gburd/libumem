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

#include "config.h"
#include "malloc_guard.h"

/*
 * Per-thread recursion depth counter.
 *
 * Uses initial-exec TLS model for fastest access:
 * - Single mov instruction on x86-64 (mov %fs:offset, %eax)
 * - No function calls (avoids __tls_get_addr)
 * - No memory allocation during access
 * - Available during pthread_create
 *
 * This TLS variable is initialized to 0 for each new thread.
 *
 * Only compiled when UMEM_ENABLE_RECURSION_GUARD is defined.
 */
#ifdef UMEM_ENABLE_RECURSION_GUARD
__thread int umem_malloc_recursion_depth
    __attribute__((tls_model("initial-exec"))) = 0;
#endif
