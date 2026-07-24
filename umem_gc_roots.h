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
 * Conservative root scanner for libumem GC.
 *
 * Scans thread stacks, registers, and data segments for potential
 * pointers into GC-managed heap. Uses the same conservative approach
 * as Boehm GC: any word-aligned value that looks like a valid heap
 * pointer is treated as a root.
 */

#ifndef _UMEM_GC_ROOTS_H
#define	_UMEM_GC_ROOTS_H

#include <sys/types.h>
#include <stdint.h>
#include <signal.h>
#include <setjmp.h>
#include <pthread.h>

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * Per-thread info for GC root scanning.
 * Tracks stack bounds so the collector can scan each thread's stack.
 *
 * During stop-the-world the suspend signal handler records the thread's
 * live stack pointer (gcti_sp) and spills all callee-saved registers into
 * gcti_regs, then increments the collector's suspended count as its ACK.
 * The collector waits for every target thread's ACK before marking, then
 * scans each parked thread from gcti_sp up to (gcti_stack_base +
 * gcti_stack_size) plus gcti_regs -- so a live pointer held only in a
 * callee-saved register or on the current stack frame is always a root.
 */
typedef struct umem_gc_thread_info {
	pthread_t	gcti_thread;
	void		*gcti_stack_base;
	size_t		gcti_stack_size;
	int		gcti_registered;
	volatile sig_atomic_t gcti_suspended;	/* set by STW handler */
	void		*volatile gcti_sp;	/* suspended stack pointer */
	sigjmp_buf	gcti_regs;		/* spilled registers (STW) */
} umem_gc_thread_info_t;

/* Maximum threads the GC can track */
#define	UMEM_GC_MAX_THREADS	1024

/* Callback type: called for each potential pointer found during scanning */
typedef void (*umem_gc_mark_fn)(void *potential_ptr);

/*
 * Register scanning.
 * Uses setjmp() to flush registers onto the stack, then scans the
 * jmp_buf for potential pointers.
 */
void umem_gc_scan_registers(umem_gc_mark_fn mark_fn);

/*
 * Stack scanning.
 * Walks every word-aligned position between stack_low and stack_high,
 * calling mark_fn for each value found.
 */
void umem_gc_scan_stack(void *stack_low, void *stack_high,
    umem_gc_mark_fn mark_fn);

/*
 * Get the current thread's stack bounds.
 * Returns 0 on success, -1 on failure.
 * Platform-specific:
 *   Linux:   pthread_getattr_np() + pthread_attr_getstack()
 *   FreeBSD: pthread_attr_get_np() + pthread_attr_getstack()
 *   Illumos: thr_stksegment()
 *   Other:   local variable address +/- 8MB estimate
 */
int umem_gc_get_stack_bounds(void **low, void **high);

/*
 * Data segment scanning.
 * Uses dl_iterate_phdr() to find writable PT_LOAD segments,
 * then scans each for potential pointers.
 */
void umem_gc_scan_data_segments(umem_gc_mark_fn mark_fn);

/*
 * Full root scan.
 * Scans registers, current thread's stack, all registered threads'
 * stacks, and data segments.  Pass threads_locked != 0 when the caller
 * already holds the thread-registry lock for the whole stop-the-world
 * window (the collector does); it then must not be re-acquired here.
 */
void umem_gc_scan_all_roots(umem_gc_mark_fn mark_fn, int threads_locked);

/*
 * Thread list management.
 * Threads register/unregister themselves so the GC knows which
 * stacks to scan.
 */
int umem_gc_thread_register(void);
int umem_gc_thread_unregister(void);

/*
 * Get the current number of registered threads.
 */
int umem_gc_thread_count(void);

#ifdef	__cplusplus
}
#endif

#endif	/* _UMEM_GC_ROOTS_H */
