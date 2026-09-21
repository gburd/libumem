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
 * malloc interposition via dlsym(RTLD_NEXT)
 *
 * This file implements lazy initialization of libumem to avoid the
 * pthread_create/malloc circular dependency deadlock. The approach is
 * based on patterns used by profiling tools (valgrind, ASan) and
 * recommended in docs/PTHREAD_RESEARCH.md section 3.
 *
 * Key design:
 * 1. Use dlsym(RTLD_NEXT) to get real libc malloc
 * 2. State machine: UNINIT → BOOTSTRAP → READY
 * 3. Route early calls to bootstrap allocator (mmap-based)
 * 4. Track pointer ownership to know which free path to use
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "config.h"
#include <dlfcn.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "umem_impl.h"
#include "malloc_guard.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* External: umem readiness state and functions */
#define	UMEM_READY_STARTUP	1
#define	UMEM_READY		3
extern int umem_ready;
extern int umem_init(void);
extern void *umem_malloc(size_t);
extern void umem_malloc_free(void *);

/* Bootstrap allocator functions from malloc.c */
extern void *bootstrap_malloc(size_t);
extern void bootstrap_free(void *);
extern int is_bootstrap_pointer(void *);
extern int process_free(void *, int, size_t *);

/* Bootstrap header structure (from malloc.c) */
#define BOOTSTRAP_MAGIC 0xB007B007B007B007ULL
typedef struct bootstrap_header {
	uint64_t magic;
	size_t size;
} bootstrap_header_t;

/* State machine for interposition */
typedef enum {
	INTERPOSE_UNINIT,	/* Before first malloc call */
	INTERPOSE_BOOTSTRAP,	/* Resolving libc or initializing umem */
	INTERPOSE_READY		/* Fully initialized, use umem */
} interpose_state_t;

/* Direct initialization - ATOMIC_VAR_INIT deprecated in C17 */
static atomic_int interpose_state = INTERPOSE_UNINIT;

/* Pointers to real libc functions */
static void *(*libc_malloc)(size_t) = NULL;
static void (*libc_free)(void *) = NULL;
static void *(*libc_calloc)(size_t, size_t) = NULL;
static void *(*libc_realloc)(void *, size_t) = NULL;
static void *(*libc_memalign)(size_t, size_t) = NULL;

/*
 * ===========================================================================
 * POINTER OWNERSHIP
 * ===========================================================================
 *
 * Under interposition a pointer reaching free()/realloc()/
 * malloc_usable_size() can come from four different owners.  Every one of
 * those entry points MUST classify it the same way: a pointer one of them
 * recognizes and another does not is how a static-buffer or libc pointer
 * ends up in umem's metadata decoder.  That is what interpose_owner_of()
 * below is for -- it is the single classifier all three use.
 *
 *   OWN_STATIC    Storage carved out of a static bump buffer during
 *                 dlsym(3) resolution, before any real allocator exists.
 *                 LIFETIME: permanent.  Never freed, never reused -- see
 *                 static_alloc().
 *   OWN_BOOTSTRAP mmap'd by bootstrap_malloc() (malloc.c), carrying a
 *                 bootstrap_header_t.  Freed with bootstrap_free().
 *   OWN_LIBC      Obtained from the real libc allocator during the bootstrap
 *                 window (only libc memalign(3) does this).  Recorded in
 *                 libc_ptrs[] together with its size; freed with libc_free.
 *   OWN_UMEM      Everything else once interposition is READY: umem's own
 *                 malloc_data_t-tagged storage.
 */
typedef enum {
	OWN_UNKNOWN,
	OWN_STATIC,
	OWN_BOOTSTRAP,
	OWN_LIBC,
	OWN_UMEM
} interpose_owner_t;

/*
 * Static bump buffer for allocations made while resolving libc symbols.
 *
 * dlsym(3) may call calloc() internally, which would recurse back into us
 * before libc_malloc is known.  Storage handed out here is PERMANENT: the
 * bump offset never moves backward and the buffer is never reset.  That is
 * deliberate and is the core of the P1.1 fix -- any scheme that recycles this
 * storage can hand the same bytes to two live callers.  The total is a few
 * hundred bytes for the process lifetime.
 *
 * Each chunk carries a header so malloc_usable_size()/realloc() know the
 * exact size, rather than guessing and over-reading.
 *
 * THREAD SAFETY: static_buffer_used is advanced under static_buffer_lock.
 * in_dlsym is only ever set by resolve_libc_functions(), which runs from the
 * library constructor before the process has created a second thread.
 */
#define STATIC_BUFFER_SIZE 4096
#define STATIC_CHUNK_MAGIC 0x5A5A0BEEFULL

typedef struct static_chunk {
	uint64_t magic;
	size_t size;		/* usable bytes following this header */
} static_chunk_t;

static char static_buffer[STATIC_BUFFER_SIZE]
    __attribute__((aligned(16)));
static size_t static_buffer_used = 0;
static pthread_mutex_t static_buffer_lock = PTHREAD_MUTEX_INITIALIZER;
static int in_dlsym = 0;

/*
 * Carve `size` permanent bytes out of the static buffer.  Returns NULL when
 * exhausted -- never wraps, never reuses.
 */
static void *
static_alloc(size_t size)
{
	size_t need;
	static_chunk_t *chunk = NULL;

	/* header + 16-byte-aligned payload, with overflow checks */
	if (size > SIZE_MAX - 15)
		return (NULL);
	need = (size + 15) & ~(size_t)15;
	if (need > SIZE_MAX - sizeof (static_chunk_t))
		return (NULL);
	need += sizeof (static_chunk_t);

	(void) pthread_mutex_lock(&static_buffer_lock);
	if (need <= STATIC_BUFFER_SIZE - static_buffer_used) {
		chunk = (static_chunk_t *)&static_buffer[static_buffer_used];
		static_buffer_used += need;
	}
	(void) pthread_mutex_unlock(&static_buffer_lock);

	if (chunk == NULL)
		return (NULL);

	chunk->magic = STATIC_CHUNK_MAGIC;
	chunk->size = need - sizeof (static_chunk_t);
	return ((void *)(chunk + 1));
}

static int
is_static_pointer(const void *ptr)
{
	return (ptr >= (const void *)static_buffer &&
	    ptr < (const void *)(static_buffer + STATIC_BUFFER_SIZE));
}

static size_t
get_static_size(void *ptr)
{
	static_chunk_t *chunk = (static_chunk_t *)ptr - 1;

	if (!is_static_pointer(chunk) || chunk->magic != STATIC_CHUNK_MAGIC)
		return (0);
	return (chunk->size);
}

/*
 * libc pointer tracking.
 *
 * Records pointer AND size: without the size, realloc() of one of these had
 * to guess the old length (it copied the NEW size from the OLD pointer, an
 * over-read on growth) on any platform lacking malloc_usable_size(3).
 *
 * Slots are reused once cleared.  The table used to be append-only, so a
 * program doing sustained memalign/free churn during the bootstrap window
 * exhausted all 512 entries and every later libc pointer became untracked --
 * and an untracked libc pointer reaching free() is handed to umem's decoder.
 *
 * INVARIANT: a libc allocation is either recorded here or has been handed to
 * libc_free().  It is never live-and-unrecorded, which is why tracking
 * failure fails the allocation (see track_libc_ptr callers) and why the
 * record is released only AFTER the operation that replaces it has succeeded.
 *
 * THREAD SAFETY: every field is accessed under libc_ptr_lock.
 */
#define MAX_LIBC_PTRS 512
struct libc_ptr_ent {
	void *ptr;
	size_t size;
};
static struct libc_ptr_ent libc_ptrs[MAX_LIBC_PTRS];
static pthread_mutex_t libc_ptr_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Fork participation.
 *
 * static_buffer_lock and libc_ptr_lock are taken by ORDINARY free() and
 * realloc() on every call (see interpose_owner_of()).  Without fork handling a
 * child could inherit either one held by a thread that does not exist in the
 * child, and the child's very next free() would block forever.
 *
 * libumem.so's fork handler calls these weak hooks (see umem_fork.c): they run
 * BEFORE umem_init_lock and every allocator lock on the prepare side, and
 * after them on the release side, because free() acquires these locks before
 * it enters the allocator.
 *
 * In the child the locks are RE-INITIALIZED rather than unlocked: the owning
 * thread may be gone, and unlocking a mutex this thread does not own is
 * undefined.  Re-initializing is safe because a post-fork child has exactly
 * one thread, so no other thread can be mid-critical-section.  The protected
 * data itself (the static buffer offset and the tracking table) is plain
 * memory copied by fork and remains consistent: entries describe allocations
 * the child has inherited and may legitimately free.
 */
void
umem_interpose_lockup(void)
{
	(void) pthread_mutex_lock(&static_buffer_lock);
	(void) pthread_mutex_lock(&libc_ptr_lock);
}

void
umem_interpose_release(void)
{
	(void) pthread_mutex_unlock(&libc_ptr_lock);
	(void) pthread_mutex_unlock(&static_buffer_lock);
}

void
umem_interpose_release_child(void)
{
	(void) pthread_mutex_init(&libc_ptr_lock, NULL);
	(void) pthread_mutex_init(&static_buffer_lock, NULL);
	/*
	 * calloc_depth is per-thread (initial-exec TLS).  The single surviving
	 * child thread inherits ITS OWN value, which is 0 unless fork() was
	 * called from inside calloc() -- impossible, since calloc() does not
	 * fork.  Threads that vanished took their own copies with them, so
	 * there is nothing to reset here.
	 */
}

/*
 * Record a libc allocation.  Returns 0 if the table is full, in which case
 * the caller MUST NOT return the pointer to the application.
 */
static int
track_libc_ptr(void *ptr, size_t size)
{
	size_t i;
	int ok = 0;

	if (ptr == NULL)
		return (0);

	(void) pthread_mutex_lock(&libc_ptr_lock);
	for (i = 0; i < MAX_LIBC_PTRS; i++) {
		if (libc_ptrs[i].ptr == NULL) {
			libc_ptrs[i].ptr = ptr;
			libc_ptrs[i].size = size;
			ok = 1;
			break;
		}
	}
	(void) pthread_mutex_unlock(&libc_ptr_lock);

	return (ok);
}

/*
 * Look up a libc allocation WITHOUT releasing the record; optionally reports
 * its size.  Ownership stays with the table so a caller that then fails can
 * leave the allocation recognizable.
 */
static int
is_libc_pointer(void *ptr, size_t *sizep)
{
	size_t i;
	int found = 0;

	if (ptr == NULL)
		return (0);

	(void) pthread_mutex_lock(&libc_ptr_lock);
	for (i = 0; i < MAX_LIBC_PTRS; i++) {
		if (libc_ptrs[i].ptr == ptr) {
			if (sizep != NULL)
				*sizep = libc_ptrs[i].size;
			found = 1;
			break;
		}
	}
	(void) pthread_mutex_unlock(&libc_ptr_lock);

	return (found);
}

/*
 * Release the ownership record for a libc allocation.  Call this only once
 * the allocation is actually about to be handed to libc_free().
 */
static void
untrack_libc_ptr(void *ptr)
{
	size_t i;

	if (ptr == NULL)
		return;

	(void) pthread_mutex_lock(&libc_ptr_lock);
	for (i = 0; i < MAX_LIBC_PTRS; i++) {
		if (libc_ptrs[i].ptr == ptr) {
			libc_ptrs[i].ptr = NULL;
			libc_ptrs[i].size = 0;
			break;
		}
	}
	(void) pthread_mutex_unlock(&libc_ptr_lock);
}

/*
 * Get the usable size of a bootstrap allocation.
 * Bootstrap allocations have a header before the returned pointer.
 */
static size_t
get_bootstrap_size(void *ptr)
{
	bootstrap_header_t *hdr;

	if (ptr == NULL)
		return (0);

	hdr = (bootstrap_header_t *)ptr - 1;
	if (hdr->magic != BOOTSTRAP_MAGIC)
		return (0);

	/* Return data size (total size minus header) */
	return (hdr->size - sizeof(bootstrap_header_t));
}

/*
 * The single ownership classifier.  free(), realloc() and
 * malloc_usable_size() all route through this so they cannot disagree.
 * *sizep, when non-NULL, receives the usable size for every owner except
 * OWN_UNKNOWN.
 */
static interpose_owner_t
interpose_owner_of(void *ptr, size_t *sizep)
{
	size_t size;

	if (ptr == NULL)
		return (OWN_UNKNOWN);

	if (is_static_pointer(ptr)) {
		if (sizep != NULL)
			*sizep = get_static_size(ptr);
		return (OWN_STATIC);
	}

	if (is_bootstrap_pointer(ptr)) {
		if (sizep != NULL)
			*sizep = get_bootstrap_size(ptr);
		return (OWN_BOOTSTRAP);
	}

	if (is_libc_pointer(ptr, &size)) {
		if (sizep != NULL)
			*sizep = size;
		return (OWN_LIBC);
	}

	if (atomic_load(&interpose_state) == INTERPOSE_READY) {
		if (process_free(ptr, 0, &size) != 0) {
			if (sizep != NULL)
				*sizep = size;
			return (OWN_UMEM);
		}
	}

	return (OWN_UNKNOWN);
}

/*
 * Per-thread calloc recursion depth.
 *
 * WHY THIS IS PER-THREAD (it used to be a process-global int):
 *   A global flag set on every ordinary calloc() makes every OTHER thread
 *   that enters calloc() concurrently believe it is recursing.  With the old
 *   shared static bump buffer as the recursion path that produced
 *   overlapping live allocations; with any recursion path it is simply
 *   wrong -- recursion is a property of one call stack, i.e. of one thread.
 *
 * WHY initial-exec TLS IS SAFE HERE:
 *   The old comment claimed __thread could not be used because "we'd need
 *   TLS to be initialized to access it".  That is true of the general-dynamic
 *   model, whose access goes through __tls_get_addr() and can allocate.  It
 *   is not true of initial-exec: the variable lives in the static TLS block,
 *   which pthread_create() allocates and zeroes BEFORE the new thread runs
 *   any code, and access is a single register-relative load with no call and
 *   no allocation.  The calloc() that TLS setup performs happens on the
 *   CREATING thread, whose static TLS block has long existed.  malloc_guard.c
 *   already depends on exactly this for umem_malloc()'s own guard.
 *   Constraint (shared with malloc_guard.h): LD_PRELOAD only, not dlopen --
 *   which is already this library's only supported mode.
 */
static __thread int calloc_depth __attribute__((tls_model("initial-exec")));

/*
 * Zero-fill an allocation calloc() is about to return.
 *
 * This exists as a separate noinline function, and not as a plain memset()
 * call inside calloc(), because GCC's strlen/memset pass rewrites an
 * adjacent
 *
 *	p = malloc(n); memset(p, 0, n);
 *
 * pair into
 *
 *	p = calloc(n, 1);
 *
 * Inside calloc() itself that is unbounded self-recursion.  This was not
 * theoretical: with the memset written inline the generated calloc() ended in
 * "jmp calloc@plt" and the process spun at 100% CPU issuing zero syscalls
 * with flat RSS.  (The pre-fix code escaped the transform only by accident,
 * because its `volatile int in_calloc` stores sat between the two calls.)
 *
 * An opaque asm barrier is kept as well so the property does not depend on
 * the pass's willingness to look through a static function, nor on
 * -fno-builtin-malloc surviving in AM_CFLAGS.  Cost is nil next to the
 * memset itself.
 */
static void __attribute__((noinline))
calloc_zero_fill(void *p, size_t n)
{
	__asm__ __volatile__("" : : "r"(p) : "memory");
	(void) memset(p, 0, n);
}

/*
 * Resolve libc malloc functions using dlsym(RTLD_NEXT)
 */
static void
resolve_libc_functions(void)
{
	in_dlsym = 1;
	libc_malloc = dlsym(RTLD_NEXT, "malloc");
	libc_free = dlsym(RTLD_NEXT, "free");
	libc_calloc = dlsym(RTLD_NEXT, "calloc");
	libc_realloc = dlsym(RTLD_NEXT, "realloc");
	libc_memalign = dlsym(RTLD_NEXT, "memalign");
	in_dlsym = 0;
}

/*
 * Constructor: Resolve libc functions before any malloc calls
 * This is called automatically when the library is loaded via LD_PRELOAD.
 *
 * IMPORTANT: This constructor MUST run before libumem's __umem_init constructor,
 * because __umem_init may call pthread functions which call malloc. We use
 * priority 101 to ensure this runs first (lower priority numbers run first).
 *
 * Cross-.so note: numbered constructor priorities only order constructors
 * WITHIN a single shared object; they do not order across the .so boundary.
 * malloc_interpose lives in libumem_malloc.so and __umem_init in libumem.so,
 * so ordering is actually governed by load order (the LD_PRELOAD'd
 * libumem_malloc.so loads before libumem.so is pulled in as its NEEDED
 * dependency).  The numeric priority is therefore a no-op across the
 * boundary -- and the Solaris/illumos ld has no numbered .init_array, so we
 * use the plain constructor form there.
 *
 * NOTE: We do NOT call umem_init() here because that would trigger
 * pthread_create which calls malloc, creating a deadlock. Instead,
 * we let malloc calls during the bootstrap phase use the bootstrap
 * allocator, and umem will be initialized lazily on first use.
 */
#if defined(__sun) || defined(__SVR4)
__attribute__((constructor))
#else
__attribute__((constructor(101)))
#endif
static void
umem_interpose_init(void)
{
	/* Tell libumem we're acting as the process malloc.  This
	 * disables backtrace(3)-based stack capture in the slab
	 * paths, which would otherwise dlopen libgcc_s through us
	 * and recurse fatally. */
	extern int umem_malloc_is_interposing;
	umem_malloc_is_interposing = 1;

	/* Resolve libc functions */
	atomic_store(&interpose_state, INTERPOSE_BOOTSTRAP);
	resolve_libc_functions();

	/*
	 * Disable abort on recoverable errors in interpose mode.
	 * When LD_PRELOAD is used, we may encounter pointers from:
	 * - libc malloc (allocated before libumem loaded)
	 * - Shared libraries with their own allocators
	 * - System libraries (zlib, etc.)
	 *
	 * Rather than crashing on invalid frees, log errors and continue.
	 * This matches glibc malloc's behavior and is safer for LD_PRELOAD.
	 */
	extern uint_t umem_abort;
	umem_abort = 0;

	/*
	 * Don't call umem_init() here - let it initialize naturally
	 * through the first umem API call. The bootstrap allocator will
	 * handle any malloc calls that happen during initialization.
	 */
}

/*
 * malloc - main interposition point
 */
void *
malloc(size_t size)
{
	/*
	 * Handle dlsym's malloc calls with the permanent static buffer.
	 * dlsym may call malloc/calloc internally, so we provide storage
	 * that does not depend on any allocator existing yet.
	 */
	if (in_dlsym)
		return (static_alloc(size));

	/*
	 * Trigger umem initialization if startup is complete but
	 * umem_init() hasn't been called yet. This breaks the
	 * chicken-and-egg problem: umem_init() is normally called
	 * by umem_alloc(), but malloc interposition never calls
	 * umem_alloc() while in BOOTSTRAP state, so umem never
	 * initializes.
	 *
	 * During umem_init(), recursive malloc() calls will still
	 * use bootstrap_malloc() because interpose_state remains
	 * BOOTSTRAP until init completes.
	 */
	if (atomic_load(&interpose_state) == INTERPOSE_BOOTSTRAP) {
		static volatile int umem_init_attempted = 0;

		if (umem_ready == UMEM_READY_STARTUP &&
		    !umem_init_attempted) {
			umem_init_attempted = 1;
			(void) umem_init();
		}

		if (umem_ready == UMEM_READY) {
			int expected = INTERPOSE_BOOTSTRAP;
			atomic_compare_exchange_strong(&interpose_state,
			    &expected, INTERPOSE_READY);
		}
	}

	/* Fast path: fully initialized */
	if (__builtin_expect(atomic_load(&interpose_state) == INTERPOSE_READY, 1)) {
		return (umem_malloc(size));
	}

	/*
	 * Pre-constructor or during umem_init(): use bootstrap allocator.
	 * This handles recursive malloc calls during initialization.
	 */
	return (bootstrap_malloc(size));
}

/*
 * free - must handle mixed allocation sources
 */
void
free(void *ptr)
{
	if (ptr == NULL)
		return;

	switch (interpose_owner_of(ptr, NULL)) {
	case OWN_STATIC:
		/*
		 * Permanent storage from the dlsym-resolution buffer.  It is
		 * never reused, so "freeing" it is a no-op by design: reuse
		 * is exactly what would let two callers hold the same bytes.
		 */
		return;
	case OWN_BOOTSTRAP:
		bootstrap_free(ptr);
		return;
	case OWN_LIBC:
		/* Release the record only as we hand it back to libc. */
		untrack_libc_ptr(ptr);
		if (libc_free != NULL)
			libc_free(ptr);
		return;
	case OWN_UMEM:
		umem_malloc_free(ptr);
		return;
	case OWN_UNKNOWN:
		break;
	}

	/*
	 * Unrecognized.  Before interposition is READY the pointer most
	 * likely predates us, so libc owns it.  Once READY, hand it to umem,
	 * which logs a recoverable error (umem_abort is 0 in interpose mode)
	 * rather than crashing on a foreign pointer -- matching glibc's
	 * tolerance and keeping LD_PRELOAD usable with libraries that carry
	 * their own allocators.
	 */
	if (atomic_load(&interpose_state) != INTERPOSE_READY) {
		if (libc_free != NULL)
			libc_free(ptr);
		return;
	}

	umem_malloc_free(ptr);
}

/*
 * malloc_usable_size - report the usable size of an interposer-owned pointer.
 *
 * Must be interposed for the same reason realloc() must: without it the
 * application's malloc_usable_size() reaches libc, which reads libc malloc
 * metadata that does not exist in front of umem/bootstrap/static storage.
 * Uses the same classifier as free()/realloc().
 */
size_t
malloc_usable_size(void *ptr)
{
	size_t size = 0;

	if (interpose_owner_of(ptr, &size) == OWN_UNKNOWN)
		return (0);
	return (size);
}

/*
 * calloc - allocate and zero
 *
 * The recursion guard is per-thread (calloc_depth) and the recursion path is
 * bootstrap_malloc(), not a shared bump buffer.  Two consequences, both
 * required by P1.1:
 *   - One thread's ordinary calloc() cannot push another thread onto the
 *     recursion path.
 *   - Storage handed out on the recursion path is individually owned
 *     (mmap + header) and freeable, so nothing has to be "recycled" while it
 *     might still be live.
 */
void *
calloc(size_t nelem, size_t elsize)
{
	size_t size;
	void *ret;

	/* Check for overflow BEFORE any size calculations */
	if (nelem > 0 && elsize > 0) {
		if (SIZE_MAX / elsize < nelem) {
			errno = ENOMEM;
			return (NULL);
		}
	}

	size = nelem * elsize;

	/*
	 * Handle dlsym's calloc calls with the permanent static buffer.
	 * dlsym may call calloc internally on some systems.
	 */
	if (in_dlsym) {
		ret = static_alloc(size);
		if (ret == NULL) {
			errno = ENOMEM;
			return (NULL);
		}
		calloc_zero_fill(ret, size);
		return (ret);
	}

	/*
	 * Recursive calloc on THIS thread (pthread_create -> allocate_dtv ->
	 * calloc -> malloc -> ... -> calloc).  Break the cycle with the
	 * bootstrap allocator, which needs no TLS and no umem.
	 */
	if (calloc_depth > 0) {
		ret = bootstrap_malloc(size);
		if (ret == NULL) {
			errno = ENOMEM;
			return (NULL);
		}
		calloc_zero_fill(ret, size);
		return (ret);
	}

	calloc_depth++;
	ret = malloc(size);
	if (ret != NULL)
		calloc_zero_fill(ret, size);
	calloc_depth--;

	return (ret);
}

/*
 * realloc - resize allocation
 *
 * Ownership rule (P1.7c): the old allocation's ownership record is released
 * only after the replacement has been allocated AND the contents copied.  A
 * realloc that fails must leave the original live, intact, and still
 * recognized by free() and by a later realloc().
 */
void *
realloc(void *ptr, size_t size)
{
	void *new_ptr;
	size_t old_size = 0;
	interpose_owner_t owner;

	if (ptr == NULL)
		return (malloc(size));

	if (size == 0) {
		free(ptr);
		return (NULL);
	}

	owner = interpose_owner_of(ptr, &old_size);

	switch (owner) {
	case OWN_UMEM:
		if (size == old_size)
			return (ptr);
		break;
	case OWN_STATIC:
	case OWN_BOOTSTRAP:
	case OWN_LIBC:
		if (old_size == 0) {
			/* Recognized owner but unusable size: corrupt header. */
			errno = EINVAL;
			return (NULL);
		}
		break;
	case OWN_UNKNOWN:
		/*
		 * Not ours.  Before READY this is a pre-interposition libc
		 * pointer and libc_realloc can handle it correctly (it knows
		 * its own metadata).  After READY an unrecognized pointer is
		 * invalid: we must not guess a length by reading foreign
		 * metadata, which is what the old malloc_usable_size()
		 * fallback did.
		 */
		if (atomic_load(&interpose_state) != INTERPOSE_READY &&
		    libc_realloc != NULL)
			return (libc_realloc(ptr, size));
		errno = EINVAL;
		return (NULL);
	}

	new_ptr = malloc(size);
	if (new_ptr == NULL) {
		/*
		 * Failure: ptr is untouched and still recorded under its
		 * original owner.  errno is set by malloc().
		 */
		return (NULL);
	}

	(void) memcpy(new_ptr, ptr, MIN(old_size, size));

	/* Only now does the old allocation stop being the live one. */
	free(ptr);
	return (new_ptr);
}

/*
 * memalign - allocate aligned memory
 */
void *
memalign(size_t align, size_t size)
{
	void *ret;

	/* Validate alignment: must be a nonzero power of two. */
	if (align == 0 || (align & (align - 1)) != 0) {
		errno = EINVAL;
		return (NULL);
	}

	/* Fast path: fully initialized */
	if (__builtin_expect(atomic_load(&interpose_state) == INTERPOSE_READY, 1)) {
		extern void *umem_memalign(size_t, size_t);
		return (umem_memalign(align, size));
	}

	/*
	 * Bootstrap phase: use libc_memalign if available.
	 *
	 * The resulting pointer MUST be recorded, because free() has no other
	 * way to tell it apart from a umem pointer -- an unrecorded libc
	 * pointer reaching free() after READY is handed to umem's metadata
	 * decoder.  If the table is full we therefore give the allocation
	 * straight back rather than return a pointer we cannot classify.
	 */
	if (atomic_load(&interpose_state) == INTERPOSE_BOOTSTRAP &&
	    libc_memalign != NULL) {
		ret = libc_memalign(align, size);
		if (ret == NULL)
			return (NULL);
		if (!track_libc_ptr(ret, size)) {
			if (libc_free != NULL)
				libc_free(ret);
			errno = ENOMEM;
			return (NULL);
		}
		return (ret);
	}

	/*
	 * Last resort (pre-constructor, no libc memalign): bootstrap storage.
	 * bootstrap_malloc() is page-granular from mmap, so it satisfies any
	 * alignment up to a page; beyond that we cannot honor the request and
	 * must fail rather than return misaligned storage.
	 */
	{
		extern size_t pagesize;
		size_t pgsz = pagesize != 0 ? pagesize : 4096;

		ret = bootstrap_malloc(size);
		if (ret == NULL)
			return (NULL);
		if (((uintptr_t)ret & (align - 1)) != 0 || align > pgsz) {
			bootstrap_free(ret);
			errno = ENOMEM;
			return (NULL);
		}
		return (ret);
	}
}

/*
 * posix_memalign - POSIX aligned allocation
 *
 * POSIX requires alignment to be a power of two AND a multiple of
 * sizeof(void *); anything else is EINVAL.  It also requires the error
 * number to be RETURNED -- reading errno after a failure is wrong because
 * errno may legitimately be 0, which would report success while *memptr
 * stayed unset.
 */
int
posix_memalign(void **memptr, size_t alignment, size_t size)
{
	void *ptr;

	if (memptr == NULL)
		return (EINVAL);

	if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
	    (alignment % sizeof (void *)) != 0)
		return (EINVAL);

	ptr = memalign(alignment, size);
	if (ptr == NULL) {
		/*
		 * size == 0 is permitted to return either NULL or a freeable
		 * pointer; report success with NULL rather than a spurious
		 * error.
		 */
		if (size == 0) {
			*memptr = NULL;
			return (0);
		}
		return (ENOMEM);
	}

	*memptr = ptr;
	return (0);
}

/*
 * aligned_alloc - C11 aligned allocation
 *
 * Must be interposed: left to libc it would return libc storage that this
 * library's free() then classifies as umem's.  C11 leaves behavior undefined
 * when size is not a multiple of alignment; we accept it (as glibc does)
 * rather than fail, since rejecting it breaks conforming-enough callers.
 */
void *
aligned_alloc(size_t alignment, size_t size)
{
	if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
		errno = EINVAL;
		return (NULL);
	}
	return (memalign(alignment, size));
}

/*
 * valloc - page-aligned allocation
 */
void *
valloc(size_t size)
{
	extern size_t pagesize;
	return (memalign(pagesize != 0 ? pagesize : 4096, size));
}
