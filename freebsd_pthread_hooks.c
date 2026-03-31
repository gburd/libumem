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
 * FreeBSD-specific pthread hooks for optimal thread creation compatibility.
 *
 * FreeBSD provides two allocator-specific hooks that eliminate pthread_create
 * deadlock without requiring dlsym(RTLD_NEXT) or TLS guards:
 *
 * 1. _pthread_mutex_init_calloc_cb() - Custom allocator for mutex initialization
 * 2. _malloc_thread_cleanup() - Thread exit cleanup without pthread_key_t
 *
 * These hooks are used by jemalloc on FreeBSD and provide zero-deadlock
 * pthread_create compatibility.
 *
 * See docs/PTHREAD_RESEARCH.md section 1 for detailed analysis.
 */

#ifdef __FreeBSD__

#include "config.h"
#include "umem_impl.h"
#include <stdlib.h>

/* External reference to existing bootstrap allocator */
extern void *bootstrap_malloc(size_t size);
extern int is_bootstrap_pointer(void *ptr);
extern void bootstrap_free(void *ptr);
extern int umem_ready;

/*
 * FreeBSD pthread hooks (declared in FreeBSD's libthr)
 * These are internal FreeBSD APIs used by custom allocators.
 */
extern void _pthread_mutex_init_calloc_cb(void *(*alloc_cb)(size_t));

/*
 * freebsd_pthread_alloc - Bootstrap allocator for pthread mutex initialization
 *
 * Called by FreeBSD's pthread_mutex_init() during thread creation.
 * Must not access TLS or call any pthread functions to avoid recursion.
 * Uses the existing bootstrap_malloc which does direct mmap.
 */
static void *
freebsd_pthread_alloc(size_t size)
{
	return bootstrap_malloc(size);
}

/*
 * _malloc_thread_cleanup - Called by pthread during thread exit
 *
 * FreeBSD's pthread implementation calls this function during pthread_exit(),
 * giving the allocator a chance to clean up per-thread data without using
 * pthread_key_t (which can cause deadlock).
 *
 * This implementation is intentionally simple - we just let the OS reclaim
 * thread-local resources. Future versions could add explicit PTC flushing
 * if needed.
 */
void
_malloc_thread_cleanup(void)
{
	/*
	 * Note: We don't perform explicit cleanup here because:
	 * 1. The kernel will reclaim thread-local memory
	 * 2. Magazine caches are flushed during normal operation
	 * 3. PTC data structures will be cleaned up by the kernel
	 *
	 * If explicit PTC cleanup becomes necessary, add it here:
	 *   if (umem_ready == UMEM_READY) {
	 *       umem_ptc_cleanup();
	 *   }
	 */
}

/*
 * register_freebsd_pthread_hooks - Register hooks during library initialization
 *
 * Constructor priority 101 ensures this runs early, before any pthread
 * operations, but after basic libc initialization. This must run before
 * umem_init() to ensure pthread_create works during umem initialization.
 */
__attribute__((constructor(101)))
static void
register_freebsd_pthread_hooks(void)
{
	_pthread_mutex_init_calloc_cb(freebsd_pthread_alloc);
}

#endif /* __FreeBSD__ */
