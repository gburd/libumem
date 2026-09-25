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
 * Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/* #pragma ident	"@(#)malloc.c	1.5	05/06/08 SMI" */

#include "config.h"
#include <unistd.h>

#include <errno.h>
#include <pthread.h>

#include <string.h>

#ifdef HAVE_SYS_SYSMACROS_H
#include <sys/sysmacros.h>
#endif

#include "umem_base.h"
#include "umem_impl.h"
#include "vmem_base.h"

#include "misc.h"
#include "malloc_guard.h"

/* External: umem readiness state */
extern int umem_ready;

#if HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

/*
 * Bootstrap allocator: Used during early initialization to avoid deadlock
 * when pthread_create/pthread_setspecific call malloc before umem is ready.
 *
 * Based on jemalloc's arena 0 and tcmalloc's Arena pattern:
 * - Uses direct mmap (no umem dependency)
 * - No TLS access required
 * - Marked allocations for detection during free
 *
 * This breaks the circular dependency:
 *   pthread_create -> malloc -> umem_init -> pthread_once -> malloc
 */
#define BOOTSTRAP_MAGIC 0xB007B007B007B007ULL
#define BOOTSTRAP_MAX_DEPTH 16

typedef struct bootstrap_header {
	uint64_t magic;
	size_t size;
} bootstrap_header_t;

static __thread int bootstrap_depth;

/*
 * Registry of live bootstrap mappings.
 *
 * WHY A REGISTRY AND NOT A HEADER (P5.10).  A bootstrap allocation is its
 * own mmap() with a bootstrap_header_t at the front.  free() has to
 * recognise one, and until this registry it did so by reading the 8 bytes
 * BEFORE the caller's pointer and comparing them with BOOTSTRAP_MAGIC -- for
 * every pointer, before anything had established the pointer was ours.  If
 * they matched, bootstrap_free() did munmap(hdr, hdr->size) with both the
 * address and the length taken from that same caller-controlled memory:
 * attacker position D (controls buffer contents) gets an unmap of a chosen
 * range.  test/security/test_forged_bootstrap.c demonstrates it.
 *
 * A live-count gate (382c529) skips the read when no bootstrap allocation is
 * live, and was measured to close this for the "steady state of nearly every
 * process".  It does not: 28 bootstrap allocations survive umem_init() in an
 * ordinary process (libdw's proc_maps_report and init_libdw strdup/calloc,
 * getpcstack's stack-bounds lookup, three dlsym) and are never freed, so the
 * count never reaches zero and the header read runs on every free() for the
 * life of the process.  The gate is kept as the fast-path short-circuit for
 * processes where the count IS zero; what closes the exposure is below.
 *
 * The registry makes recognition a LOOKUP in memory we own, not a read of
 * memory the caller owns: is_bootstrap_pointer(p) is "is p in the table",
 * and bootstrap_free() unmaps the size the TABLE recorded.  A forged header
 * is then just bytes.  Same construction as libc_ptrs[] in
 * malloc_interpose.c: fixed table, one mutex, a live-count gate read
 * lock-free on the fast path.  MAX_BOOTSTRAP_PTRS is sized from the
 * measured steady state (28) with headroom for a deeper init or a
 * dlopen()-heavy program; when it is full, bootstrap_malloc() FAILS rather
 * than hand out an unregistered pointer that free() could then not
 * recognise -- an unregistered bootstrap pointer would be treated as
 * foreign and refused, leaking the mapping, which is the safe direction but
 * still a leak, so the table is sized not to fill.
 *
 * THREAD SAFETY: table entries under bootstrap_ptr_lock; bootstrap_live is
 * the lock-free gate (count up BEFORE the pointer is returned, down AFTER
 * the entry is cleared and the mapping unmapped).
 */
#define	MAX_BOOTSTRAP_PTRS	256
struct bootstrap_ent {
	void *ptr;		/* the pointer handed to the caller (hdr + 1) */
	size_t size;		/* total mapping size, for munmap */
};
static struct bootstrap_ent bootstrap_ptrs[MAX_BOOTSTRAP_PTRS];
static pthread_mutex_t bootstrap_ptr_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic long bootstrap_live;

/*
 * Convex hull of every registered bootstrap mapping, [lo, hi).  The live
 * count alone is not a usable fast-path gate: 28 bootstrap allocations
 * survive umem_init() in an ordinary process and are never freed, so the
 * count is nonzero for the life of nearly every process, and a locked
 * 256-slot scan on every free() reproduced P8.1's collapse exactly
 * (preload/API 0.005 at t=8 with the count-only gate).
 *
 * The hull is exact enough: a pointer outside [lo, hi) cannot be
 * registered, and that is two relaxed loads and two compares.  A pointer
 * inside takes the locked scan, which is the exact answer; bootstrap
 * mappings are their own mmaps in a region the heap does not share, so the
 * hull is tight and ordinary heap pointers miss it.  lo only falls and hi
 * only rises (a freed mapping does not shrink them), so a stale read is a
 * superset -- the same argument as umem_may_own()'s hull.
 */
static _Atomic uintptr_t bootstrap_lo = UINTPTR_MAX;
static _Atomic uintptr_t bootstrap_hi = 0;

static void
bootstrap_hull_add(const void *buf, size_t total_size)
{
	uintptr_t b = (uintptr_t)buf - sizeof (bootstrap_header_t);
	uintptr_t e = b + total_size;
	uintptr_t cur;

	cur = atomic_load_explicit(&bootstrap_lo, memory_order_relaxed);
	while (b < cur && !atomic_compare_exchange_weak_explicit(&bootstrap_lo,
	    &cur, b, memory_order_release, memory_order_relaxed))
		;
	cur = atomic_load_explicit(&bootstrap_hi, memory_order_relaxed);
	while (e > cur && !atomic_compare_exchange_weak_explicit(&bootstrap_hi,
	    &cur, e, memory_order_release, memory_order_relaxed))
		;
}

static inline int
bootstrap_hull_may_contain(const void *buf)
{
	uintptr_t a = (uintptr_t)buf;

	return (a >= atomic_load_explicit(&bootstrap_lo, memory_order_acquire) &&
	    a < atomic_load_explicit(&bootstrap_hi, memory_order_acquire));
}

/* Returns 1 and records the mapping, or 0 if the table is full. */
static int
bootstrap_register(void *buf, size_t total_size)
{
	size_t i;
	int ok = 0;

	(void) pthread_mutex_lock(&bootstrap_ptr_lock);
	for (i = 0; i < MAX_BOOTSTRAP_PTRS; i++) {
		if (bootstrap_ptrs[i].ptr == NULL) {
			/* Count up BEFORE the pointer becomes findable. */
			atomic_fetch_add_explicit(&bootstrap_live, 1,
			    memory_order_release);
			bootstrap_ptrs[i].ptr = buf;
			bootstrap_ptrs[i].size = total_size;
			bootstrap_hull_add(buf, total_size);
			ok = 1;
			break;
		}
	}
	(void) pthread_mutex_unlock(&bootstrap_ptr_lock);
	return (ok);
}

/*
 * If buf is a registered bootstrap pointer, clear its entry and return the
 * mapping size (nonzero); else return 0.  Clearing and unmapping are the
 * caller's two steps; the live count goes down after both.
 */
static size_t
bootstrap_unregister(const void *buf)
{
	size_t i, sz = 0;

	(void) pthread_mutex_lock(&bootstrap_ptr_lock);
	for (i = 0; i < MAX_BOOTSTRAP_PTRS; i++) {
		if (bootstrap_ptrs[i].ptr == buf) {
			sz = bootstrap_ptrs[i].size;
			bootstrap_ptrs[i].ptr = NULL;
			bootstrap_ptrs[i].size = 0;
			break;
		}
	}
	(void) pthread_mutex_unlock(&bootstrap_ptr_lock);
	return (sz);
}

static int
bootstrap_registered(const void *buf)
{
	size_t i;
	int found = 0;

	(void) pthread_mutex_lock(&bootstrap_ptr_lock);
	for (i = 0; i < MAX_BOOTSTRAP_PTRS; i++) {
		if (bootstrap_ptrs[i].ptr == buf) {
			found = 1;
			break;
		}
	}
	(void) pthread_mutex_unlock(&bootstrap_ptr_lock);
	return (found);
}

/*
 * Exposed for malloc_interpose.c
 * These functions are used during bootstrap phase when umem is not yet ready.
 */
void *
bootstrap_malloc(size_t size)
{
	bootstrap_header_t *hdr;
	size_t total_size;

	/*
	 * Checked header addition.  Unchecked, bootstrap_malloc(SIZE_MAX)
	 * wrapped to a ~15-byte total, mapped that, and returned a non-NULL
	 * pointer claiming SIZE_MAX usable bytes.
	 */
	if (size > SIZE_MAX - sizeof (bootstrap_header_t)) {
		errno = ENOMEM;
		return (NULL);
	}
	total_size = size + sizeof (bootstrap_header_t);

	if (++bootstrap_depth > BOOTSTRAP_MAX_DEPTH) {
		const char msg[] = "libumem: fatal bootstrap malloc "
		    "recursion (depth > 16)\n";
		(void) write(STDERR_FILENO, msg, sizeof (msg) - 1);
		abort();
	}

#ifdef _WIN32
	hdr = (bootstrap_header_t *)VirtualAlloc(NULL, total_size,
	    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (hdr == NULL) {
		bootstrap_depth--;
		return (NULL);
	}
#else
	hdr = mmap(NULL, total_size, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	if (hdr == MAP_FAILED) {
		bootstrap_depth--;
		return (NULL);
	}
#endif

	hdr->magic = BOOTSTRAP_MAGIC;
	hdr->size = total_size;
	if (!bootstrap_register(hdr + 1, total_size)) {
		/*
		 * Table full.  Fail the allocation rather than hand out a
		 * pointer free() cannot recognise (see the registry comment).
		 */
#ifdef _WIN32
		(void) VirtualFree(hdr, 0, MEM_RELEASE);
#else
		(void) munmap(hdr, total_size);
#endif
		bootstrap_depth--;
		errno = ENOMEM;
		return (NULL);
	}
	bootstrap_depth--;
	return (void *)(hdr + 1);
}

/*
 * Step 1 of process_free()'s validation order: is buf a bootstrap pointer?
 *
 * Answered from the registry, never from buf[-1].  The live-count gate makes
 * the common case (no bootstrap allocation live) one relaxed load; a nonzero
 * count means a locked table scan, which is the exact answer.  Nothing here
 * reads memory the caller controls, so a forged BOOTSTRAP_MAGIC in front of
 * a pointer is inert (P5.10).  The header's magic field is retained for the
 * bootstrap_free() consistency check below and for debugger inspection; it
 * is no longer consulted to decide anything.
 */
static inline int
bootstrap_pointer_p(const void *buf)
{
	if (atomic_load_explicit(&bootstrap_live, memory_order_acquire) == 0)
		return (0);
	if (!bootstrap_hull_may_contain(buf))
		return (0);
	return (bootstrap_registered(buf));
}

/* Exported for malloc_interpose.c's classifier. */
int
is_bootstrap_pointer(void *buf)
{
	if (buf == NULL)
		return (0);
	return (bootstrap_pointer_p(buf));
}

void
bootstrap_free(void *buf)
{
	bootstrap_header_t *hdr;
	size_t sz;

	if (buf == NULL)
		return;

	/*
	 * The registry decides.  An unregistered pointer is refused here
	 * without reading it; the size unmapped is the one the table recorded
	 * at bootstrap_malloc() time, not whatever is in front of buf now.
	 */
	sz = bootstrap_unregister(buf);
	if (sz == 0)
		return;

	hdr = (bootstrap_header_t *)buf - 1;
	/*
	 * Consistency check on OUR header, now that buf is known to be ours.
	 * A mismatch is a corruption of a bootstrap mapping, reported like any
	 * other; the mapping is unmapped either way using the recorded size.
	 */
	if (hdr->magic != BOOTSTRAP_MAGIC || hdr->size != sz)
		umem_err_recoverable("bootstrap_free(%p): header corrupted "
		    "(magic %#llx size %zu, expected size %zu)\n", buf,
		    (unsigned long long)hdr->magic, hdr->size, sz);

#ifdef _WIN32
	(void) VirtualFree(hdr, 0, MEM_RELEASE);
#else
	(void) munmap(hdr, sz);
#endif
	/* Count down AFTER the mapping is gone. */
	atomic_fetch_sub_explicit(&bootstrap_live, 1, memory_order_release);
}

/*
 * malloc_data_t is an 8-byte structure which is located "before" the pointer
 * returned from {m,c,re}alloc and memalign.  The first four bytes give
 * information about the buffer, and the second four bytes are a status byte.
 *
 * See umem_impl.h for the various magic numbers used, and the size
 * encode/decode macros.
 *
 * The 'size' of the buffer includes the tags.  That is, we encode the
 * argument to umem_alloc(), not the argument to malloc().
 */

typedef struct malloc_data {
	uint32_t malloc_size;
	uint32_t malloc_stat; /* = UMEM_MALLOC_ENCODE(state, malloc_size) */
} malloc_data_t;

/*
 * NOTE: malloc/free interposition is now handled by malloc_interpose.c
 * which uses dlsym(RTLD_NEXT) to avoid pthread_create deadlocks.
 * The weak symbol pragmas have been removed.
 */

/*
 * On Linux, the PTC genasm code sets these function pointers after
 * generating the per-thread-cache assembly.  malloc() and free() check
 * these and dispatch through them when set.
 */

/*
 * umem_malloc: the real malloc implementation.
 *
 * This is the function that PTC falls back to for allocations it
 * cannot handle (overflow, oversized, etc.).
 * On non-x86, malloc is a weak alias to this function.
 */
void *
umem_malloc(size_t size_arg)
{
#ifdef _LP64
	uint32_t high_size = 0;
#endif
	size_t size;
	malloc_data_t *ret;

	/*
	 * Use bootstrap allocator if umem is not fully initialized.
	 * Based on jemalloc's arena 0 pattern - provides emergency allocation
	 * during initialization without requiring TLS or umem infrastructure.
	 */
	if (umem_ready != UMEM_READY)
		return (bootstrap_malloc(size_arg));

	/*
	 * Check for recursive malloc call (e.g., pthread_create -> malloc ->
	 * pthread_getspecific -> malloc). Use bootstrap allocator to break
	 * the cycle. This uses initial-exec TLS for single-instruction access.
	 */
	if (umem_enter_malloc() > 0) {
		umem_exit_malloc();
		return (bootstrap_malloc(size_arg));
	}

	size = size_arg + sizeof (malloc_data_t);

#ifdef _LP64
	if (size > UMEM_SECOND_ALIGN) {
		size += sizeof (malloc_data_t);
		high_size = (size >> 32);
	}
#endif
	if (size < size_arg) {
		umem_exit_malloc();
		errno = ENOMEM;			/* overflow */
		return (NULL);
	}
	ret = (malloc_data_t *)_umem_alloc(size, UMEM_DEFAULT);
	if (ret == NULL) {
		umem_exit_malloc();
		if (size <= UMEM_MAXBUF)
			errno = EAGAIN;
		else
			errno = ENOMEM;
		return (NULL);
#ifdef _LP64
	} else if (high_size > 0) {
		uint32_t low_size = (uint32_t)size;

		/*
		 * uses different magic numbers to make it harder to
		 * undetectably corrupt
		 */
		ret->malloc_size = high_size;
		ret->malloc_stat = UMEM_MALLOC_ENCODE(MALLOC_MAGIC, high_size);
		ret++;

		ret->malloc_size = low_size;
		ret->malloc_stat = UMEM_MALLOC_ENCODE(MALLOC_OVERSIZE_MAGIC,
		    low_size);
		ret++;
	} else if (size > UMEM_SECOND_ALIGN) {
		uint32_t low_size = (uint32_t)size;

		ret++; /* leave the first 8 bytes alone */

		ret->malloc_size = low_size;
		ret->malloc_stat = UMEM_MALLOC_ENCODE(MALLOC_SECOND_MAGIC,
		    low_size);
		ret++;
#endif
	} else {
		ret->malloc_size = size;
		ret->malloc_stat = UMEM_MALLOC_ENCODE(MALLOC_MAGIC, size);
		ret++;
	}

	umem_exit_malloc();
	return ((void *)ret);
}

/*
 * NOTE: calloc() is now in malloc_interpose.c
 */

/*
 * umem_memalign: internal memalign implementation
 * Used by malloc_interpose.c when umem is fully initialized
 */
void *
umem_memalign(size_t align, size_t size_arg)
{
	size_t size;
	uintptr_t phase;

	void *buf;
	malloc_data_t *ret;

	size_t overhead;

	if (size_arg == 0 || align == 0 || (align & (align - 1)) != 0) {
		errno = EINVAL;
		return (NULL);
	}

	/*
	 * if malloc provides the required alignment, use it.
	 */
	if (align <= UMEM_ALIGN ||
	    (align <= UMEM_SECOND_ALIGN && size_arg >= UMEM_SECOND_ALIGN))
		return (umem_malloc(size_arg));

#ifdef _LP64
	overhead = 2 * sizeof (malloc_data_t);
#else
	overhead = sizeof (malloc_data_t);
#endif

	ASSERT(overhead <= align);

	size = size_arg + overhead;
	phase = align - overhead;

	if (umem_memalign_arena == NULL && umem_init() == 0) {
		errno = ENOMEM;
		return (NULL);
	}

	if (size < size_arg) {
		errno = ENOMEM;			/* overflow */
		return (NULL);
	}

	buf = vmem_xalloc(umem_memalign_arena, size, align, phase,
	    0, NULL, NULL, VM_NOSLEEP);

	if (buf == NULL) {
		if ((size_arg + align) <= UMEM_MAXBUF)
			errno = EAGAIN;
		else
			errno = ENOMEM;

		return (NULL);
	}

	ret = (malloc_data_t *)buf;
	{
		uint32_t low_size = (uint32_t)size;

#ifdef _LP64
		uint32_t high_size = (uint32_t)(size >> 32);

		ret->malloc_size = high_size;
		ret->malloc_stat = UMEM_MALLOC_ENCODE(MEMALIGN_MAGIC,
		    high_size);
		ret++;
#endif

		ret->malloc_size = low_size;
		ret->malloc_stat = UMEM_MALLOC_ENCODE(MEMALIGN_MAGIC, low_size);
		ret++;
	}

	ASSERT(P2PHASE((uintptr_t)ret, align) == 0);
	ASSERT((void *)((uintptr_t)ret - overhead) == buf);

	return ((void *)ret);
}

/*
 * NOTE: memalign, posix_memalign, and valloc are now in malloc_interpose.c
 */

/*
 * ---- "could this address possibly be ours?" (P5.8) ----
 *
 * process_free() below is handed pointers by the LD_PRELOAD interposer, and
 * under LD_PRELOAD not every pointer reaching free() came from us: a library
 * with its own allocator, or a plain bug in the application, delivers a
 * foreign or wild pointer.  The malloc_data_t magic is a FIXED CONSTANT
 * (umem_impl.h: UMEM_MALLOC_DECODE is stat + size), so it is forgeable, and
 * the interposer sets umem_abort = 0 -- so a header that passes used to be
 * enough to make libumem free memory it does not own.
 *
 * This is the range check the bootstrap allocator already has in
 * is_bootstrap_pointer(), for umem's own memory: every byte umem hands out
 * lies inside a span of vmem_heap (the "mmap_heap"/sbrk heap arena), because
 * umem_internal/va/default/oversize/memalign/firewall all import from it.
 *
 * WHAT IT IS: the convex hull of vmem_heap's spans -- a SUPERSET of owned
 * memory, since spans are not contiguous.  So it is a necessary, not a
 * sufficient, condition: exactly the same character as is_bootstrap_pointer's
 * magic check.  It cannot be fooled by a header forged in a foreign heap or
 * on the stack, which is the exposure it closes.
 *
 * WHY A CACHED HULL and not vmem_contains(): vmem_contains() walks the span
 * list under the arena lock, and this runs on every free().  The hull is two
 * compares.  It only ever WIDENS -- the updates below are CAS loops, so a
 * concurrent pair cannot narrow it by losing an update -- which means a stale
 * read can produce a false MISS but never a false hit.  A miss then refreshes
 * and re-tests, so a valid pointer is never rejected.
 *
 * NO LOCK OF ITS OWN, deliberately: this is on the free() path, and a new
 * lock there would have to be placed in the fork/vmem lock order.  vmem_walk()
 * takes only the arena's own lock, which free() does not already hold.
 *
 * ponytail: convex hull, refreshed on miss.  If a between-spans foreign
 * pointer with a forged header ever matters, the upgrade is a sorted span
 * table with a binary search, or vmem_contains() gated on the hull hit.
 */
static _Atomic uintptr_t umem_heap_lo = (uintptr_t)UINTPTR_MAX;
static _Atomic uintptr_t umem_heap_hi;

static void
hull_span_cb(void *arg, void *addr, size_t size)
{
	uintptr_t *hull = arg;	/* [0] = lo, [1] = hi */
	uintptr_t start = (uintptr_t)addr;

	if (start < hull[0])
		hull[0] = start;
	if (start + size > hull[1])
		hull[1] = start + size;
}

/* Widen lo downward / hi upward, never the other way (see above). */
static void
hull_lower(_Atomic uintptr_t *slot, uintptr_t v)
{
	uintptr_t cur = atomic_load(slot);

	while (v < cur) {
		if (atomic_compare_exchange_weak(slot, &cur, v))
			return;
	}
}

static void
hull_raise(_Atomic uintptr_t *slot, uintptr_t v)
{
	uintptr_t cur = atomic_load(slot);

	while (v > cur) {
		if (atomic_compare_exchange_weak(slot, &cur, v))
			return;
	}
}

/*
 * Re-read vmem_heap's spans and widen the cached hull.  Called only when a
 * candidate missed the hull, i.e. never on the common path.  vmem_walk()
 * calls the callback with the arena lock held, so the callback must not
 * allocate -- it does not.
 */
static void
hull_refresh(void)
{
	uintptr_t hull[2];

	if (vmem_heap == NULL)
		return;
	hull[0] = (uintptr_t)UINTPTR_MAX;
	hull[1] = 0;
	vmem_walk(vmem_heap, VMEM_SPAN, hull_span_cb, hull);
	if (hull[1] == 0)
		return;			/* no spans yet */
	hull_lower(&umem_heap_lo, hull[0]);
	hull_raise(&umem_heap_hi, hull[1]);
}

/*
 * Is [addr, addr + len) inside the hull [lo, hi)?  lo/hi are bounds the
 * caller loaded from umem_heap_lo/hi.  Because the hull only widens, ANY
 * pair ever read is a subset of the current hull, so a hit against a
 * stale pair is a hit against the current one; a miss may be stale and
 * must go through umem_may_own() before it is treated as a refusal.
 */
static inline int
hull_contains(uintptr_t lo, uintptr_t hi, const void *addr, size_t len)
{
	uintptr_t a = (uintptr_t)addr;
	uintptr_t end = a + len;

	if (a == 0 || len == 0 || end < a)	/* wrapped: not a real object */
		return (0);
	return (a >= lo && end <= hi);
}

/*
 * Could [addr, addr + len) be memory umem handed out?  Conservative: a false
 * "yes" is possible (the hull is a superset), a false "no" is not.  This is
 * the MISS path, reached only when hull_contains() against the caller's
 * loaded pair said no: the heap may simply have grown since the last
 * refresh, so re-read the spans and re-test before rejecting anything.
 * The hit path is MAY_OWN in process_free_umem().
 */
static int
umem_may_own(const void *addr, size_t len)
{
	hull_refresh();
	return (hull_contains(atomic_load(&umem_heap_lo),
	    atomic_load(&umem_heap_hi), addr, len));
}

/*
 * process_free:
 *
 * Pulls information out of a buffer pointer, and optionally free it.
 * This is used by free() and realloc() to process buffers.
 *
 * On failure, calls umem_err_recoverable() with an appropriate message
 * On success, returns the data size through *data_size_arg, if (!is_free).
 *
 * Preserves errno, since free()'s semantics require it.
 *
 * VALIDATION ORDER (P5.8).  Under LD_PRELOAD this is handed pointers that are
 * not necessarily ours, so it is the trust boundary and the ordering matters:
 *
 *   1. bootstrap pointer?  (its own range + magic check)
 *   2. is the HEADER inside umem-owned address space?  If not, return without
 *      reading it -- reading buf[-1] for an arbitrary pointer was an 8-byte
 *      read before an arbitrary address.
 *   3. decode the header; the magic tells us the layout
 *   4. is the decoded size self-consistent, and does [base, base+size) lie
 *      inside umem-owned address space?
 *   5. ONLY NOW write anything.
 *
 * Pre-fix, step 2 did not exist, step 4 did not exist, and step 5 happened
 * inside the switch: every successful-magic branch stored UMEM_FREE_PATTERN_32
 * into buf->malloc_stat BEFORE the size was sanity-checked, and the
 * MALLOC_OVERSIZE/MEMALIGN branches wrote one header's stat word before
 * validating the other one.  With a forged header those writes landed in
 * memory libumem does not own, and (since the interposer sets umem_abort = 0)
 * execution continued into _umem_free()/vmem_xfree() on a foreign address.
 * Nothing is written now until the pointer has been accepted.
 *
 * Exposed for malloc_interpose.c
 */

static int process_free_umem(void *, int, size_t *);

/*
 * free() must leave errno as it found it, so process_free_umem() saves and
 * restores it on every path.  errno is (*__errno_location()), a PLT call
 * per free from this DSO.  The address it returns is fixed for the life of
 * the thread (it is the thread's own TLS slot), so it is cached here once
 * per thread; initial-exec TLS is one %fs/tpidr-relative load, the model
 * malloc_guard.h already requires of libumem.so.  A forked child has one
 * thread and inherits that thread's own slot, so nothing is reset on fork.
 */
static __thread int *errno_slot __attribute__((tls_model("initial-exec")));

static inline int *
errno_addr(void)
{
	int *p = errno_slot;

	if (__builtin_expect(p == NULL, 0))
		p = errno_slot = &errno;
	return (p);
}

int
process_free(void *buf_arg,
    int do_free,		/* free the buffer, or just get its size? */
    size_t *data_size_arg)	/* output: bytes of data in buf_arg */
{
	/*
	 * Step 1: bootstrap pointers carry a bootstrap_header_t, not a
	 * malloc_data_t, and must not reach the decoder below.
	 */
	if (buf_arg != NULL && bootstrap_pointer_p(buf_arg)) {
		if (data_size_arg != NULL) {
			bootstrap_header_t *hdr = (bootstrap_header_t *)buf_arg - 1;
			*data_size_arg = hdr->size - sizeof(bootstrap_header_t);
		}
		/* For do_free=1, bootstrap_free should be called instead */
		return (1);
	}
	return (process_free_umem(buf_arg, do_free, data_size_arg));
}

/*
 * Steps 2-5 of process_free().  PRECONDITION: the caller has already
 * established that buf_arg is not a bootstrap pointer (step 1), so that
 * check is not repeated here.  umem_malloc_free() and process_free() are
 * the only callers and both do step 1 first.
 */
static int
process_free_umem(void *buf_arg, int do_free, size_t *data_size_arg)
{
	malloc_data_t *buf;
	void *base;
	size_t size;
	size_t data_size;
	/* Set by the switch, acted on only after validation (step 5). */
	malloc_data_t *poison_lo = NULL;	/* lowest stat word to clear */
	int npoison = 0;			/* 1 or 2 consecutive words */
	int memalign = 0;
	size_t overhead_min;			/* smallest legal `size` */

	const char *message;
	int *ep = errno_addr();
	int old_errno = *ep;
	/*
	 * One read of the hull for every ownership test in this call.  A hit
	 * against these bounds is a hit against the current hull (they only
	 * widen); a miss goes to umem_may_own(), which refreshes and
	 * re-tests, so a valid pointer is never refused on a stale pair.
	 */
	uintptr_t hlo = atomic_load_explicit(&umem_heap_lo,
	    memory_order_relaxed);
	uintptr_t hhi = atomic_load_explicit(&umem_heap_hi,
	    memory_order_relaxed);
#define	MAY_OWN(p, n) \
	(hull_contains(hlo, hhi, (p), (n)) || umem_may_own((p), (n)))

	buf = (malloc_data_t *)buf_arg;

	buf--;

	/*
	 * Step 2: do not read a header that cannot be ours.  This is the check
	 * that turns "8-byte read before an arbitrary address" into a refusal.
	 * Only the FIRST header is covered here; the two-tag layouts re-check
	 * before reading their second one.
	 */
	if (!MAY_OWN(buf, sizeof (malloc_data_t))) {
		umem_err_recoverable("%s(%p): not a libumem allocation "
		    "(outside umem's heap)\n",
		    do_free ? "free" : "realloc", buf_arg);
		*ep = old_errno;
		return (0);
	}

	size = buf->malloc_size;

	switch (UMEM_MALLOC_DECODE(buf->malloc_stat, size)) {

	case MALLOC_MAGIC:
		base = (void *)buf;
		overhead_min = sizeof (malloc_data_t);
		data_size = size - sizeof (malloc_data_t);
		poison_lo = buf;
		npoison = 1;
		goto validate;

#ifdef _LP64
	case MALLOC_SECOND_MAGIC:
		base = (void *)(buf - 1);
		overhead_min = 2 * sizeof (malloc_data_t);
		data_size = size - 2 * sizeof (malloc_data_t);
		/* Only the second tag carries state; the first is padding. */
		poison_lo = buf;
		npoison = 1;
		goto validate;

	case MALLOC_OVERSIZE_MAGIC: {
		size_t high_size;

		buf--;
		if (!MAY_OWN(buf, sizeof (malloc_data_t))) {
			message = "invalid or corrupted buffer";
			break;
		}
		high_size = buf->malloc_size;

		if (UMEM_MALLOC_DECODE(buf->malloc_stat, high_size) !=
		    MALLOC_MAGIC) {
			message = "invalid or corrupted buffer";
			break;
		}

		size += high_size << 32;

		base = (void *)buf;
		overhead_min = 2 * sizeof (malloc_data_t);
		data_size = size - 2 * sizeof (malloc_data_t);
		poison_lo = buf;
		npoison = 2;
		goto validate;
	}
#endif

	case MEMALIGN_MAGIC: {
		size_t overhead = sizeof (malloc_data_t);

#ifdef _LP64
		size_t high_size;

		overhead += sizeof (malloc_data_t);

		buf--;
		if (!MAY_OWN(buf, sizeof (malloc_data_t))) {
			message = "invalid or corrupted buffer";
			break;
		}
		high_size = buf->malloc_size;

		if (UMEM_MALLOC_DECODE(buf->malloc_stat, high_size) !=
		    MEMALIGN_MAGIC) {
			message = "invalid or corrupted buffer";
			break;
		}
		size += high_size << 32;
		/* Both tags are cleared, after validation. */
		npoison = 2;
#else
		npoison = 1;
#endif

		base = (void *)buf;
		overhead_min = overhead;
		data_size = size - overhead;
		poison_lo = buf;
		memalign = 1;
		goto validate;
	}
	default:
		if (buf->malloc_stat == UMEM_FREE_PATTERN_32)
			message = "double-free or invalid buffer";
		else
			message = "invalid or corrupted buffer";
		break;
	}

	umem_err_recoverable("%s(%p): %s\n",
	    do_free? "free" : "realloc", buf_arg, message);

	*ep = old_errno;
	return (0);

validate:
	/*
	 * Step 4.  The magic is a fixed constant, so passing the switch above
	 * proves nothing on its own -- it is forgeable, and an attacker who can
	 * write into a buffer can write a header in front of an address of
	 * their choosing.  Two independent things must also hold:
	 *
	 *   - the size is self-consistent with the layout the magic named, so
	 *     data_size cannot have underflowed to a huge value;
	 *   - the whole object lies inside umem's heap, so a header forged in a
	 *     foreign heap, on the stack, or in a data section is rejected.
	 *
	 * Failing either of these is treated exactly like a bad magic: report,
	 * mutate nothing, and return 0 so the caller does not use the size.
	 */
	if (size < overhead_min || !MAY_OWN(base, size)) {
		umem_err_recoverable("%s(%p): header claims %zu bytes at %p, "
		    "which is not a libumem allocation; refusing\n",
		    do_free ? "free" : "realloc", buf_arg, size, base);
		*ep = old_errno;
		return (0);
	}

	/* Step 5: accepted.  Now, and only now, mutate. */
	if (do_free) {
		int i;
		for (i = 0; i < npoison; i++)
			poison_lo[i].malloc_stat = UMEM_FREE_PATTERN_32;
	}

	if (memalign) {
		if (do_free)
			vmem_xfree(umem_memalign_arena, base, size);
		else
			*data_size_arg = data_size;
	} else {
		if (do_free)
			_umem_free(base, size);
		else
			*data_size_arg = data_size;
	}

	*ep = old_errno;
	return (1);
#undef MAY_OWN
}

/*
 * umem_malloc_free: the real free implementation.
 *
 * This is the function that PTC generated code falls back to for
 * free operations it cannot handle.
 * On non-x86, free is a weak alias to this function.
 */
void
umem_malloc_free(void *buf)
{
	if (buf == NULL)
		return;

	/*
	 * Step 1 of process_free()'s order, done here once: a bootstrap
	 * allocation (from before umem was ready) is a direct mmap and is
	 * released with munmap.  Everything else goes to steps 2-5.
	 */
	if (bootstrap_pointer_p(buf)) {
		bootstrap_free(buf);
		return;
	}

	/*
	 * Steps 2-5 for the two single-state-word layouts umem_malloc()
	 * produces for every request up to UMEM_MAXBUF: MALLOC_MAGIC (8-byte
	 * tag) and, on _LP64, MALLOC_SECOND_MAGIC (16-byte tag, state in the
	 * second word).  This is process_free_umem() with do_free fixed to 1
	 * and only these two cases; the checks and their order are the same:
	 *
	 *   2. the header word lies inside the hull -- before it is read
	 *   3. decode; the magic names the layout
	 *   4. size >= the layout's overhead, and [base, base+size) is inside
	 *      the hull -- before anything is written
	 *   5. poison the state word, then _umem_free(); errno is saved and
	 *      restored around it, as process_free_umem() does
	 *
	 * Anything else -- the two-tag OVERSIZE and MEMALIGN layouts, a bad
	 * magic, a double free, a hull miss on either test -- falls through
	 * to process_free_umem(), which repeats these steps from the top (a
	 * hull miss there also refreshes the hull) and reports.  So a pointer
	 * this path refuses is decided by exactly the code that decided it
	 * before, and one this path accepts passed the same tests it would
	 * have passed there.
	 */
	{
		malloc_data_t *hdr = (malloc_data_t *)buf - 1;
		uintptr_t hlo = atomic_load_explicit(&umem_heap_lo,
		    memory_order_relaxed);
		uintptr_t hhi = atomic_load_explicit(&umem_heap_hi,
		    memory_order_relaxed);
		void *base;
		size_t size, overhead;
		uint32_t magic;
		int *ep, old_errno;

		if (!hull_contains(hlo, hhi, hdr, sizeof (*hdr)))
			goto slow;
		size = hdr->malloc_size;
		magic = UMEM_MALLOC_DECODE(hdr->malloc_stat, size);
		if (magic == MALLOC_MAGIC) {
			base = hdr;
			overhead = sizeof (malloc_data_t);
#ifdef _LP64
		} else if (magic == MALLOC_SECOND_MAGIC) {
			base = hdr - 1;
			overhead = 2 * sizeof (malloc_data_t);
#endif
		} else {
			goto slow;
		}
		if (size < overhead || !hull_contains(hlo, hhi, base, size))
			goto slow;
		hdr->malloc_stat = UMEM_FREE_PATTERN_32;
		ep = errno_addr();
		old_errno = *ep;
		_umem_free(base, size);
		*ep = old_errno;
		return;
	}
slow:
	(void) process_free_umem(buf, 1, NULL);
}

/*
 * NOTE: malloc(), free(), calloc(), realloc(), memalign(), posix_memalign(),
 * and valloc() are now in malloc_interpose.c which handles both the bootstrap
 * phase and dispatching to umem once initialized.
 */

/*
 * _malloc and _free are the PTC (per-thread cache) trampoline entry points.
 * On Solaris/Illumos, libc provides these in writable+executable text segments
 * so that umem_genasm() can overwrite them with generated assembly.
 * On Linux, these are simple trampolines -- after PTC genasm activates,
 * malloc()/free() call through the generated code directly via function
 * pointers, so these functions are only used for symbol resolution by the
 * genasm initialization code.
 */
#ifndef __sun
void *
_malloc(size_t size)
{
	return (umem_malloc(size));
}

void
_free(void *buf)
{
	umem_malloc_free(buf);
}
#endif
