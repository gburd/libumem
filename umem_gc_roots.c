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
 * Implements Boehm-style conservative scanning: every word-aligned value
 * in thread stacks, registers, and writable data segments is checked as
 * a potential pointer. The mark_fn callback (provided by the GC core)
 * determines whether a value actually points to a GC-managed allocation.
 *
 * Platform support:
 *   - Linux:   pthread_getattr_np, dl_iterate_phdr, /proc/self/maps
 *   - FreeBSD: pthread_attr_get_np, dl_iterate_phdr
 *   - Illumos: thr_stksegment, dl_iterate_phdr
 *   - Fallback: conservative stack estimate, no data segment scanning
 */

#include "config.h"
#include "umem_gc_roots.h"
#include <umem_impl.h>

#include <setjmp.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>

#ifdef HAVE_DLFCN_H
#include <dlfcn.h>
#endif

/*
 * Conservative scanning deliberately reads every word-aligned slot across
 * a stack/register/data region, which necessarily reads past the bounds of
 * individual objects living in that region.  Under AddressSanitizer that
 * is reported as a stack-buffer-overflow / global-buffer-overflow even
 * though it is intentional and safe (the region itself is mapped).  Mark
 * the single function that performs these raw reads as not-instrumented,
 * exactly as Boehm GC does for its scanners, so the collector is usable
 * under ASan.  All conservative reads route through umem_gc_scan_stack
 * (registers and data segments call it, as does the GC core's object
 * scanner), so one annotation covers every case.
 */
#if defined(__has_attribute)
#if __has_attribute(no_sanitize_address)
#define	UMEM_GC_NO_ASAN	__attribute__((no_sanitize_address))
#endif
#endif
#ifndef UMEM_GC_NO_ASAN
#define	UMEM_GC_NO_ASAN
#endif

#ifdef __linux__
#include <link.h>
#include <elf.h>
#endif

#ifdef __FreeBSD__
#include <link.h>
#include <elf.h>
#endif

#ifdef __sun
#include <thread.h>
#include <link.h>
#include <elf.h>
#endif

#if defined(__linux__) || defined(__FreeBSD__) || defined(__sun)
#define	HAVE_DL_ITERATE_PHDR	1
#endif

/* Forward declaration */
static int umem_gc_get_stack_bounds_for(void **base, size_t *size,
    pthread_t thread);

/* ------------------------------------------------------------------ */
/* Thread registry                                                     */
/* ------------------------------------------------------------------ */

/* Non-static: accessed by umem_gc.c for STW signal delivery */
umem_gc_thread_info_t umem_gc_threads[UMEM_GC_MAX_THREADS];
int umem_gc_nthreads;
mutex_t umem_gc_threads_lock = DEFAULTMUTEX;

int
umem_gc_thread_register(void)
{
	int i;
	int slot = -1;
	pthread_t self = pthread_self();

	mutex_lock(&umem_gc_threads_lock);

	for (i = 0; i < UMEM_GC_MAX_THREADS; i++) {
		if (umem_gc_threads[i].gcti_registered &&
		    pthread_equal(umem_gc_threads[i].gcti_thread, self)) {
			mutex_unlock(&umem_gc_threads_lock);
			return (0);
		}
		if (!umem_gc_threads[i].gcti_registered && slot == -1)
			slot = i;
	}

	if (slot == -1) {
		mutex_unlock(&umem_gc_threads_lock);
		return (-1);
	}

	umem_gc_threads[slot].gcti_thread = self;
	umem_gc_threads[slot].gcti_stack_base = NULL;
	umem_gc_threads[slot].gcti_stack_size = 0;
	umem_gc_threads[slot].gcti_suspended = 0;
	umem_gc_threads[slot].gcti_registered = 1;

	umem_gc_get_stack_bounds_for(
	    &umem_gc_threads[slot].gcti_stack_base,
	    &umem_gc_threads[slot].gcti_stack_size,
	    self);

	umem_gc_nthreads++;
	mutex_unlock(&umem_gc_threads_lock);
	return (0);
}

int
umem_gc_thread_unregister(void)
{
	int i;
	pthread_t self = pthread_self();

	mutex_lock(&umem_gc_threads_lock);

	for (i = 0; i < UMEM_GC_MAX_THREADS; i++) {
		if (umem_gc_threads[i].gcti_registered &&
		    pthread_equal(umem_gc_threads[i].gcti_thread, self)) {
			umem_gc_threads[i].gcti_registered = 0;
			umem_gc_threads[i].gcti_stack_base = NULL;
			umem_gc_threads[i].gcti_stack_size = 0;
			umem_gc_nthreads--;
			mutex_unlock(&umem_gc_threads_lock);
			return (0);
		}
	}

	mutex_unlock(&umem_gc_threads_lock);
	return (-1);
}

int
umem_gc_thread_count(void)
{
	int n;

	mutex_lock(&umem_gc_threads_lock);
	n = umem_gc_nthreads;
	mutex_unlock(&umem_gc_threads_lock);
	return (n);
}

/* ------------------------------------------------------------------ */
/* Platform-specific stack bounds                                      */
/* ------------------------------------------------------------------ */

/*
 * Internal helper: get stack bounds for a specific thread.
 * Sets *base to the stack base address, *size to the stack size.
 * On stacks that grow downward (all supported platforms), the
 * scannable range is [base, base + size).
 */
static int
umem_gc_get_stack_bounds_for(void **base, size_t *size, pthread_t thread)
{
#if defined(__linux__)
	pthread_attr_t attr;
	void *stack_addr;
	size_t stack_size;

	if (pthread_getattr_np(thread, &attr) != 0)
		return (-1);

	if (pthread_attr_getstack(&attr, &stack_addr, &stack_size) != 0) {
		pthread_attr_destroy(&attr);
		return (-1);
	}

	pthread_attr_destroy(&attr);
	*base = stack_addr;
	*size = stack_size;
	return (0);

#elif defined(__FreeBSD__)
	pthread_attr_t attr;
	void *stack_addr;
	size_t stack_size;

	pthread_attr_init(&attr);
	if (pthread_attr_get_np(thread, &attr) != 0) {
		pthread_attr_destroy(&attr);
		return (-1);
	}

	if (pthread_attr_getstack(&attr, &stack_addr, &stack_size) != 0) {
		pthread_attr_destroy(&attr);
		return (-1);
	}

	pthread_attr_destroy(&attr);
	*base = stack_addr;
	*size = stack_size;
	return (0);

#elif defined(__sun)
	stack_t st;

	/*
	 * thr_stksegment returns the top of the stack in ss_sp
	 * and the size in ss_size. The base is (ss_sp - ss_size).
	 */
	if (thr_stksegment(&st) != 0)
		return (-1);

	*base = (void *)((uintptr_t)st.ss_sp - st.ss_size);
	*size = st.ss_size;
	return (0);

#else
	(void)thread;
	*base = NULL;
	*size = 0;
	return (-1);
#endif
}

int
umem_gc_get_stack_bounds(void **low, void **high)
{
	void *base;
	size_t size;

#if defined(__linux__) || defined(__FreeBSD__)
	if (umem_gc_get_stack_bounds_for(&base, &size,
	    pthread_self()) == 0) {
		*low = base;
		*high = (void *)((uintptr_t)base + size);
		return (0);
	}
#elif defined(__sun)
	if (umem_gc_get_stack_bounds_for(&base, &size,
	    pthread_self()) == 0) {
		*low = base;
		*high = (void *)((uintptr_t)base + size);
		return (0);
	}
#endif

	/*
	 * Fallback: use local variable address and assume 8MB stack.
	 * The local variable is near the current stack pointer.
	 * Stacks grow downward, so the low end is addr - 8MB
	 * and the high end is addr + a small margin.
	 */
	{
		volatile int stack_marker;
		uintptr_t sp = (uintptr_t)&stack_marker;
		size_t default_stack = 8 * 1024 * 1024;

		if (sp > default_stack)
			*low = (void *)(sp - default_stack);
		else
			*low = (void *)0;

		*high = (void *)(sp + 4096);
	}
	return (0);
}

/* ------------------------------------------------------------------ */
/* Stack scanning                                                      */
/* ------------------------------------------------------------------ */

UMEM_GC_NO_ASAN void
umem_gc_scan_stack(void *stack_low, void *stack_high,
    umem_gc_mark_fn mark_fn)
{
	uintptr_t *p;
	uintptr_t low;
	uintptr_t high;

	if (stack_low == NULL || stack_high == NULL || mark_fn == NULL)
		return;

	if (stack_low >= stack_high)
		return;

	/* Align to word boundary */
	low = ((uintptr_t)stack_low + sizeof(uintptr_t) - 1) &
	    ~(sizeof(uintptr_t) - 1);
	high = (uintptr_t)stack_high & ~(sizeof(uintptr_t) - 1);

	for (p = (uintptr_t *)low; p < (uintptr_t *)high; p++) {
		uintptr_t val = *p;

		/*
		 * Quick filter: skip values that can't be valid pointers.
		 * On most platforms, user-space addresses are above 4KB
		 * (below that is unmapped null-guard) and below some
		 * architecture-specific ceiling.
		 */
		if (val < 4096)
			continue;

#if defined(__amd64__) || defined(__x86_64__)
		/* x86-64 canonical addresses: high 17 bits must be same */
		if ((val >> 47) != 0 && (val >> 47) != 0x1FFFF)
			continue;
#endif

#if defined(__aarch64__)
		/* AArch64 userspace is typically below 0x0001000000000000 */
		if (val >= (uintptr_t)1 << 48)
			continue;
#endif

		mark_fn((void *)val);
	}
}

/* ------------------------------------------------------------------ */
/* Register scanning                                                   */
/* ------------------------------------------------------------------ */

void
umem_gc_scan_registers(umem_gc_mark_fn mark_fn)
{
	jmp_buf regs;

	if (mark_fn == NULL)
		return;

	/*
	 * setjmp saves the current register state (callee-saved registers,
	 * stack pointer, etc.) into jmp_buf. We then scan that buffer for
	 * potential pointers. This is the standard Boehm GC technique for
	 * flushing registers onto scannable memory.
	 *
	 * We use volatile to prevent the compiler from optimizing away
	 * the setjmp or the scan of its buffer.
	 */
	(void) memset(regs, 0, sizeof(regs));

	if (setjmp(regs) == 0) {
		volatile jmp_buf *vregs = (volatile jmp_buf *)&regs;
		umem_gc_scan_stack((void *)vregs,
		    (void *)((uintptr_t)vregs + sizeof(jmp_buf)),
		    mark_fn);
	}
}

/* ------------------------------------------------------------------ */
/* Data segment scanning                                               */
/* ------------------------------------------------------------------ */

#ifdef HAVE_DL_ITERATE_PHDR

struct gc_phdr_ctx {
	umem_gc_mark_fn	mark_fn;
};

static int
gc_phdr_callback(struct dl_phdr_info *info, size_t size, void *data)
{
	struct gc_phdr_ctx *ctx = (struct gc_phdr_ctx *)data;
	int i;

	(void)size;

	for (i = 0; i < (int)info->dlpi_phnum; i++) {
		const ElfW(Phdr) *phdr = &info->dlpi_phdr[i];
		uintptr_t seg_start;
		uintptr_t seg_end;

		/* Only scan writable, loadable segments (.data, .bss) */
		if (phdr->p_type != PT_LOAD)
			continue;
		if (!(phdr->p_flags & PF_W))
			continue;

		seg_start = info->dlpi_addr + phdr->p_vaddr;
		seg_end = seg_start + phdr->p_memsz;

		/*
		 * p_memsz can be larger than p_filesz for .bss.
		 * We scan the full in-memory size since .bss is
		 * zero-initialized but may contain pointers at runtime.
		 */
		umem_gc_scan_stack((void *)seg_start, (void *)seg_end,
		    ctx->mark_fn);
	}

	return (0);
}

void
umem_gc_scan_data_segments(umem_gc_mark_fn mark_fn)
{
	struct gc_phdr_ctx ctx;

	if (mark_fn == NULL)
		return;

	ctx.mark_fn = mark_fn;
	dl_iterate_phdr(gc_phdr_callback, &ctx);
}

#else /* !HAVE_DL_ITERATE_PHDR */

void
umem_gc_scan_data_segments(umem_gc_mark_fn mark_fn)
{
	/*
	 * No dl_iterate_phdr available.  Data segment scanning is
	 * skipped. Conservative GC will still work if all roots are
	 * reachable from thread stacks and registers.
	 */
	(void)mark_fn;
}

#endif /* HAVE_DL_ITERATE_PHDR */

/* ------------------------------------------------------------------ */
/* Scan a single registered thread                                     */
/* ------------------------------------------------------------------ */

static void
umem_gc_scan_thread(umem_gc_thread_info_t *ti, umem_gc_mark_fn mark_fn)
{
	void *stack_low;
	void *stack_high;

	if (ti == NULL || !ti->gcti_registered || mark_fn == NULL)
		return;

	if (ti->gcti_stack_base == NULL || ti->gcti_stack_size == 0)
		return;

	stack_high = (void *)((uintptr_t)ti->gcti_stack_base +
	    ti->gcti_stack_size);

	if (ti->gcti_suspended) {
		/*
		 * The thread is parked in the STW signal handler and has
		 * spilled its registers into gcti_regs and recorded its live
		 * stack pointer in gcti_sp.  Scan the spilled register block
		 * (a callee-saved register may hold the only pointer to a
		 * live object) and only the live portion of the stack, from
		 * the suspended SP up to the stack base -- not the stale
		 * theoretical extent below SP.
		 */
		umem_gc_scan_stack((void *)ti->gcti_regs,
		    (void *)((uintptr_t)ti->gcti_regs + sizeof (ti->gcti_regs)),
		    mark_fn);

		stack_low = ti->gcti_sp;
		if (stack_low == NULL ||
		    (uintptr_t)stack_low < (uintptr_t)ti->gcti_stack_base ||
		    (uintptr_t)stack_low >= (uintptr_t)stack_high)
			stack_low = ti->gcti_stack_base;
	} else {
		/*
		 * Thread was not suspended (STW inactive or timed out):
		 * best-effort scan of its full stack extent.
		 */
		stack_low = ti->gcti_stack_base;
	}

	umem_gc_scan_stack(stack_low, stack_high, mark_fn);
}

/* ------------------------------------------------------------------ */
/* Full root scan                                                      */
/* ------------------------------------------------------------------ */

void
umem_gc_scan_all_roots(umem_gc_mark_fn mark_fn, int threads_locked)
{
	int i;
	pthread_t self;
	void *stack_low;
	void *stack_high;

	if (mark_fn == NULL)
		return;

	/*
	 * Phase 1: Flush registers onto the stack and scan them.
	 * Must happen before stack scanning to capture register roots.
	 */
	umem_gc_scan_registers(mark_fn);

	/*
	 * Phase 2: Scan current thread's stack.
	 * We get fresh bounds rather than using registered info
	 * because the current SP is where we actually are now.
	 */
	if (umem_gc_get_stack_bounds(&stack_low, &stack_high) == 0) {
		volatile int stack_marker;
		uintptr_t sp = (uintptr_t)&stack_marker;

		/*
		 * Scan from current SP to stack high.
		 * On downward-growing stacks, the live portion is
		 * from the current SP up to the stack top.
		 */
		if (sp >= (uintptr_t)stack_low &&
		    sp < (uintptr_t)stack_high) {
			umem_gc_scan_stack((void *)sp, stack_high, mark_fn);
		} else {
			umem_gc_scan_stack(stack_low, stack_high, mark_fn);
		}
	}

	/*
	 * Phase 3: Scan other registered threads' stacks.
	 * During a real STW pause these threads are suspended in the
	 * signal handler; each has spilled its registers and recorded its
	 * live SP (see umem_gc_scan_thread).  The caller is responsible for
	 * stopping the world before calling this function.
	 *
	 * If the caller already holds umem_gc_threads_lock across the STW
	 * window (threads_locked != 0) we must NOT re-acquire it: the lock
	 * is non-recursive and re-locking would self-deadlock.  Holding it
	 * across the whole pause is what keeps the target thread set stable
	 * so the suspend handshake cannot strand the collector.
	 */
	self = pthread_self();

	if (!threads_locked)
		mutex_lock(&umem_gc_threads_lock);
	for (i = 0; i < UMEM_GC_MAX_THREADS; i++) {
		if (!umem_gc_threads[i].gcti_registered)
			continue;
		if (pthread_equal(umem_gc_threads[i].gcti_thread, self))
			continue;
		umem_gc_scan_thread(&umem_gc_threads[i], mark_fn);
	}
	if (!threads_locked)
		mutex_unlock(&umem_gc_threads_lock);

	/*
	 * Phase 4: Scan writable data segments (.data, .bss).
	 * Global/static variables may hold GC roots.
	 */
	umem_gc_scan_data_segments(mark_fn);
}
