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
 * FreeBSD-specific pthread hooks for thread cleanup compatibility.
 *
 * FreeBSD's libthr calls _malloc_thread_cleanup() during pthread_exit(),
 * giving custom allocators a chance to release per-thread resources
 * without using pthread_key_t (which can deadlock during thread exit).
 *
 * Note: _pthread_mutex_init_calloc_cb() is an internal FreeBSD API for
 * initializing mutexes used by the allocator itself. Its real signature is:
 *   int _pthread_mutex_init_calloc_cb(pthread_mutex_t *mutex,
 *       void *(calloc_cb)(size_t, size_t));
 * It is called by the allocator when it needs a new mutex, NOT registered
 * as a global constructor. libumem does not currently need this hook
 * because its mutex initialization does not call malloc.
 */

#ifdef __FreeBSD__

#include "config.h"
#include "umem_impl.h"

extern int umem_ready;

/*
 * _malloc_thread_cleanup - Called by libthr during thread exit
 *
 * FreeBSD's pthread implementation calls this exported symbol during
 * pthread_exit(), giving the allocator a chance to clean up per-thread
 * data without using pthread_key_t.
 */
void
_malloc_thread_cleanup(void)
{
	/*
	 * No explicit cleanup needed: the kernel reclaims thread-local
	 * memory, and magazine caches are flushed during normal operation.
	 */
}

#endif /* __FreeBSD__ */
