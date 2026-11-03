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
 * pthread_create/malloc circular dependency deadlock, the pattern
 * profiling tools (valgrind, ASan) use.  (An earlier version cited
 * docs/PTHREAD_RESEARCH.md; that file is not in the tree.)
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

/*
 * ===========================================================================
 * FAULT INJECTION (allocation failure testing)
 * ===========================================================================
 *
 * A deterministic, thread-safe allocation-failure injector that lives in the
 * LD_PRELOAD drop-in only (libumem_malloc.so), never in the core allocator.
 * It is the allocation layer of the single-host fault-injection test suite
 * (Noxu DB; owner directive 2026-11-03): it does for malloc/calloc/realloc/
 * memalign/posix_memalign/aligned_alloc what the FAINT tool
 * (github.com/misc0110/faint) does, but as a thread-safe allocator-resident
 * mode rather than a per-allocation backtrace walk, so it works on a heavily
 * multi-threaded program with many thousands of call sites (FAINT loses
 * call-site identity and is not thread-safe there; measured).
 *
 * One environment variable selects the mode, read once at load (so a child
 * spawned with it set is faulted; the parent is not):
 *
 *   UMEM_FAULT=nth:<n>[,every:<m>]
 *                          Fail the Nth qualifying allocation of the process
 *                          (1-based, deterministic, no backtrace). With
 *                          every:<m>, also fail every m-th allocation after
 *                          the Nth. The foundation of the "fail the Nth
 *                          allocation" sweep.
 *
 *   UMEM_FAULT=site:<hex>[,count:<c>]
 *                          Fail allocations whose caller return address is
 *                          <hex> (a value from a profile run). count:1 fails
 *                          only the first such allocation; omitted/0 fails
 *                          every one. The return address is
 *                          __builtin_return_address(0) of the entry point;
 *                          it is resolved to file:line OFFLINE afterwards
 *                          with addr2line against the preloaded program (no
 *                          per-allocation symbolization on the hot path, the
 *                          re-entrant trap FAINT fell into).
 *
 *   UMEM_FAULT=profile:<file>
 *                          Record, per allocation, the caller's return
 *                          address and a hit count; write "ADDR COUNT" lines
 *                          to <file> at exit. Feeds the site list for the
 *                          inject phase. Never fails an allocation.
 *
 * Optional modifiers (any mode):
 *   UMEM_FAULT_SIZE_MIN / UMEM_FAULT_SIZE_MAX  Restrict injection to requests
 *                          in [min,max] bytes (0 = unbounded). Lets a sweep
 *                          aim at the large allocations JE treats as
 *                          recoverable (cache / log buffers / rep buffers).
 *   UMEM_FAULT_LOG=<file>  Append "FAIL seq=.. caller=.. size=.." per injected
 *                          failure so the harness can assert a fault ACTUALLY
 *                          fired (a test that passes with no fault is vacuous).
 *
 * THREAD SAFETY & REENTRANCY: the allocation counter and the site-fired flag
 * are atomics; the profile table is lock-free open addressing; and a
 * thread-local reentrancy guard (fi_in_fault) means libumem's own internal
 * allocations, and the injector's own bookkeeping, never fault. The injector
 * adds nothing to the hot path when UMEM_FAULT is unset (one relaxed load of
 * a flag that is 0).
 */

/* Armed? A single relaxed load gates the whole injector off the hot path. */
static int fi_armed = 0;

/*
 * Thread-local reentrancy guard. Set while the injector (or libumem's own
 * internal machinery, reached through a faulting entry point) is running, so
 * a nested allocation never faults and the injector never recurses into
 * itself. initial-exec TLS for the same reason calloc_depth uses it below:
 * a single register-relative load, no __tls_get_addr allocation.
 */
static __thread int fi_in_fault __attribute__((tls_model("initial-exec")));

/* Nth-allocation mode. */
static unsigned long long fi_nth = 0;      /* 1-based; 0 = disabled */
static unsigned long long fi_every = 0;    /* 0 = once; else every Nth after */

/* Site mode. */
static void *fi_site = NULL;               /* caller return addr to fail */
static int fi_site_once = 0;
static atomic_int fi_site_fired = 0;

/* Size window. */
static size_t fi_size_min = 0;
static size_t fi_size_max = 0;             /* 0 = unbounded */

/* Profile mode. */
static int fi_profile = 0;
static char fi_profile_path[256];
static char fi_log_path[256];
static int fi_log_fd = -1;

/* Per-process allocation sequence (every qualifying alloc increments it). */
static atomic_ullong fi_seq = 0;

/*
 * Profile table: open-addressed caller-address -> count. Fixed capacity so it
 * never itself allocates (which would recurse into us). A full table stops
 * recording new sites (reported at dump). 64 Ki slots covers far more than
 * the ~thousands of monomorphized Rust call sites.
 */
#define FI_PROFILE_SLOTS (1u << 16)
struct fi_prof_ent {
	_Atomic(void *) addr;
	atomic_ullong count;
};
static struct fi_prof_ent fi_prof[FI_PROFILE_SLOTS];
static atomic_size_t fi_prof_used = 0;

/* The one block()/unblock() reentrancy guard is inherited from below; the
 * profile writer runs only at exit via atexit, single-threaded enough that a
 * plain file write is safe. */

static unsigned long long
fi_getenv_ull(const char *name, unsigned long long dflt)
{
	const char *v = getenv(name);
	if (v == NULL || *v == '\0')
		return (dflt);
	return (strtoull(v, NULL, 0));
}

/*
 * Dump the profile at exit: one "ADDR COUNT\n" line per recorded call site,
 * ADDR in hex. Offline, addr2line resolves ADDR against the program.
 */
static void
fi_dump_profile(void)
{
	if (!fi_profile || fi_profile_path[0] == '\0')
		return;
	FILE *f = fopen(fi_profile_path, "w");
	if (f == NULL)
		return;
	unsigned long long total = 0;
	for (size_t i = 0; i < FI_PROFILE_SLOTS; i++) {
		void *a = atomic_load_explicit(&fi_prof[i].addr,
		    memory_order_relaxed);
		if (a == NULL)
			continue;
		unsigned long long c = atomic_load_explicit(&fi_prof[i].count,
		    memory_order_relaxed);
		total += c;
		(void) fprintf(f, "%p %llu\n", a, c);
	}
	(void) fprintf(f, "# sites=%zu total_allocs=%llu\n",
	    atomic_load_explicit(&fi_prof_used, memory_order_relaxed), total);
	(void) fclose(f);
}

/* Record one allocation's caller in the profile table (lock-free). */
static void
fi_profile_record(void *caller)
{
	if (caller == NULL)
		return;
	size_t h = ((uintptr_t)caller >> 4) & (FI_PROFILE_SLOTS - 1);
	for (size_t probe = 0; probe < FI_PROFILE_SLOTS; probe++) {
		size_t i = (h + probe) & (FI_PROFILE_SLOTS - 1);
		void *cur = atomic_load_explicit(&fi_prof[i].addr,
		    memory_order_acquire);
		if (cur == caller) {
			atomic_fetch_add_explicit(&fi_prof[i].count, 1,
			    memory_order_relaxed);
			return;
		}
		if (cur == NULL) {
			void *expected = NULL;
			if (atomic_compare_exchange_strong_explicit(
			    &fi_prof[i].addr, &expected, caller,
			    memory_order_acq_rel, memory_order_acquire)) {
				atomic_fetch_add_explicit(&fi_prof_used, 1,
				    memory_order_relaxed);
				atomic_fetch_add_explicit(&fi_prof[i].count, 1,
				    memory_order_relaxed);
				return;
			}
			if (expected == caller) {
				atomic_fetch_add_explicit(&fi_prof[i].count, 1,
				    memory_order_relaxed);
				return;
			}
			/* lost the race to another addr; keep probing */
		}
	}
	/* table full: drop (reported via fi_prof_used < sites at dump) */
}

/* Log one injected failure (best effort). */
static void
fi_log(unsigned long long seq, void *caller, size_t size)
{
	if (fi_log_fd < 0)
		return;
	char buf[128];
	int n = snprintf(buf, sizeof(buf), "FAIL seq=%llu caller=%p size=%zu\n",
	    seq, caller, size);
	if (n > 0)
		(void) write(fi_log_fd, buf, (size_t)n);
}

/*
 * The injector decision, called at the top of each allocating entry point.
 * `caller` is the application return address (__builtin_return_address(0) of
 * the entry point). Returns 1 to FAIL the allocation (return NULL / set
 * errno), 0 to proceed. Thread-safe; cheap when disarmed.
 */
static int
fi_should_fail(void *caller, size_t size)
{
	if (__builtin_expect(fi_armed == 0, 1))
		return (0);

	/*
	 * Reentrancy guard: if we are already inside the injector on this
	 * thread (e.g. fi_profile_record / fi_log touched the allocator, or
	 * libumem's own internal machinery allocated while servicing a faulted
	 * entry point), do not fault the nested allocation and do not recurse.
	 */
	if (fi_in_fault)
		return (0);

	/* Size window: a request outside [min,max] never fails. */
	if (fi_size_min != 0 && size < fi_size_min)
		return (0);
	if (fi_size_max != 0 && size > fi_size_max)
		return (0);

	fi_in_fault = 1;

	/* PROFILE phase records, never fails. */
	if (fi_profile) {
		fi_profile_record(caller);
		fi_in_fault = 0;
		return (0);
	}

	unsigned long long seq = atomic_fetch_add_explicit(&fi_seq, 1,
	    memory_order_relaxed) + 1;
	int fail = 0;

	/* Site mode. */
	if (fi_site != NULL) {
		if (caller == fi_site) {
			if (fi_site_once) {
				int expected = 0;
				if (atomic_compare_exchange_strong(
				    &fi_site_fired, &expected, 1))
					fail = 1;
			} else {
				fail = 1;
			}
		}
	} else if (fi_nth != 0) {
		/* Nth mode (and every-m-th after the Nth). */
		if (seq == fi_nth) {
			fail = 1;
		} else if (seq > fi_nth && fi_every != 0 &&
		    ((seq - fi_nth) % fi_every) == 0) {
			fail = 1;
		}
	}

	if (fail)
		fi_log(seq, caller, size);

	fi_in_fault = 0;
	return (fail);
}

/*
 * Parse one `key:value` (or bare `key`) token of UMEM_FAULT into the injector
 * state. Tokens are comma-separated; the first selects the mode.
 */
static void
fi_parse_token(const char *key, const char *val)
{
	if (strcmp(key, "nth") == 0) {
		fi_nth = (val != NULL) ? strtoull(val, NULL, 0) : 0;
	} else if (strcmp(key, "every") == 0) {
		fi_every = (val != NULL) ? strtoull(val, NULL, 0) : 0;
	} else if (strcmp(key, "site") == 0) {
		if (val != NULL)
			fi_site = (void *)(uintptr_t)strtoull(val, NULL, 0);
	} else if (strcmp(key, "count") == 0) {
		/* count:1 => fail the site once; anything else => every time */
		fi_site_once = (val != NULL &&
		    strtoull(val, NULL, 0) == 1) ? 1 : 0;
	} else if (strcmp(key, "profile") == 0) {
		if (val != NULL && *val != '\0') {
			fi_profile = 1;
			(void) strncpy(fi_profile_path, val,
			    sizeof(fi_profile_path) - 1);
		}
	}
}

/*
 * Parse UMEM_FAULT (+ the UMEM_FAULT_* modifiers) from the environment.
 * Called once from the constructor. Sets fi_armed only if a mode is selected.
 *
 * Grammar: UMEM_FAULT=mode[:value][,key[:value]]...
 *   nth:<n>[,every:<m>]
 *   site:<hex>[,count:<c>]
 *   profile:<file>
 */
static void
fi_init(void)
{
	const char *spec = getenv("UMEM_FAULT");
	const char *log = getenv("UMEM_FAULT_LOG");

	fi_size_min = (size_t)fi_getenv_ull("UMEM_FAULT_SIZE_MIN", 0);
	fi_size_max = (size_t)fi_getenv_ull("UMEM_FAULT_SIZE_MAX", 0);

	if (spec != NULL && *spec != '\0') {
		/* Copy so we can tokenize in place without touching environ. */
		char buf[512];
		(void) strncpy(buf, spec, sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = '\0';
		char *save = NULL;
		for (char *tok = strtok_r(buf, ",", &save); tok != NULL;
		    tok = strtok_r(NULL, ",", &save)) {
			char *colon = strchr(tok, ':');
			if (colon != NULL) {
				*colon = '\0';
				fi_parse_token(tok, colon + 1);
			} else {
				fi_parse_token(tok, NULL);
			}
		}
	}

	if (fi_profile)
		(void) atexit(fi_dump_profile);

	if (log != NULL && *log != '\0') {
		(void) strncpy(fi_log_path, log, sizeof(fi_log_path) - 1);
		/* Open via fopen then dup the fd; append mode. */
		FILE *lf = fopen(fi_log_path, "a");
		if (lf != NULL) {
			fi_log_fd = dup(fileno(lf));
			(void) fclose(lf);
		}
	}

	fi_armed = (fi_profile || fi_site != NULL || fi_nth != 0) ? 1 : 0;
}

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
 * Number of live entries in libc_ptrs[].  This is the fast-path gate for
 * is_libc_pointer(), and it exists because of a measured ~500x collapse:
 *
 *   The table only ever gains entries from bootstrap-phase memalign() --
 *   i.e. before umem is READY -- and is EMPTY for the entire steady-state
 *   life of almost every process.  Yet free() consulted it unconditionally,
 *   taking the process-global libc_ptr_lock and scanning all 512 slots on
 *   every single call.  A global mutex on the hottest path in the library,
 *   acquired to search an empty table.
 *
 *   Measured on c7i.metal-48xl (192 vCPU), multi 16:64, null-controlled
 *   (preload-arm sd 3.75%): the LD_PRELOAD interposer ran 3.0 Mops/s at one
 *   thread and 0.8 Mops/s at 192 -- NEGATIVE scaling -- while the umem_alloc
 *   API path on the same build did 398 Mops/s at 192 (glibc: 421).  Every
 *   earlier comparison in this repo measured the API path, which is why a
 *   500x gap on the drop-in path went unseen.
 *
 * Reads of the count are relaxed and lock-free.  That is sound because the
 * count is only a GATE: a nonzero read falls through to the locked, exact
 * scan; a zero read means "no libc entry can match", and that is true at
 * the instant of the read.  Could an entry be added after a zero read?
 * Only by bootstrap memalign(), which by construction hands a pointer to
 * the caller before anyone can free() it -- so a pointer being freed while
 * the count reads zero was never in the table.  (Track increments BEFORE
 * publishing the pointer; untrack decrements AFTER the slot is cleared, so
 * a stale nonzero only costs a lock, never a miss.)
 */
static atomic_size_t libc_ptr_live = 0;

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
			/* Count up BEFORE the pointer becomes findable. */
			atomic_fetch_add_explicit(&libc_ptr_live, 1,
			    memory_order_release);
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

	/*
	 * Fast path: an empty table cannot contain ptr.  This single relaxed
	 * load replaces a mutex acquire + 512-slot scan on every free(), and
	 * is the whole fix for the negative thread scaling described above.
	 */
	if (atomic_load_explicit(&libc_ptr_live, memory_order_acquire) == 0)
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
			/* Count down AFTER the slot is cleared. */
			atomic_fetch_sub_explicit(&libc_ptr_live, 1,
			    memory_order_release);
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
 * The interposer is its own object, and its malloc()/free() frames sit
 * between the caller and libumem.so on every audited stack: register its text
 * with getpcstack()'s internal-frame skip too (T9).
 */
__attribute__((constructor))
static void
umem_interpose_pcstack_init(void)
{
	extern void umem_pcstack_internal(const void *addr);

	umem_pcstack_internal((const void *)(uintptr_t)&umem_interpose_pcstack_init);
}

/*
 * Constructor: Resolve libc functions before any malloc calls
 * This is called automatically when the library is loaded via LD_PRELOAD.
 *
 * This constructor MUST run before libumem's __umem_init constructor,
 * because __umem_init may call pthread functions which call malloc.  What
 * guarantees that is LOAD ORDER, not the constructor(101) below: numbered
 * constructor priorities only order constructors WITHIN a single shared
 * object, and malloc_interpose lives in libumem_malloc.so while __umem_init
 * is in libumem.so.  The LD_PRELOAD'd libumem_malloc.so loads before
 * libumem.so is pulled in as its NEEDED dependency, so its constructors
 * run first.  The 101 is a no-op across the boundary and is kept only as
 * documentation of intent; the Solaris/illumos ld has no numbered
 * .init_array, so the plain form is used there.  (This comment used to
 * say "We use priority 101 to ensure this runs first", which the
 * paragraph after it then contradicted.)
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
	 *
	 * WHY (the legitimate reason): under LD_PRELOAD we are handed pointers
	 * from allocators that are not us -- libc malloc for anything allocated
	 * before we loaded, shared libraries carrying their own allocator, and
	 * plain application bugs.  Aborting the process on the first such
	 * pointer makes libumem unusable as a drop-in.
	 *
	 * THIS IS NOT "THE SAME AS GLIBC", and the comment here used to say it
	 * was.  glibc ABORTS on an invalid free ("free(): invalid pointer").
	 * Continuing is strictly weaker than glibc for this case, and P5.8
	 * examined exactly that: with umem_abort = 0, a pointer whose header
	 * passed the magic check proceeded to free memory libumem does not own.
	 *
	 * WHAT MAKES CONTINUING DEFENSIBLE NOW, and it was not before: as of
	 * P5.8, process_free() validates BEFORE it mutates -- ownership of the
	 * header, ownership of [base, base+size), and the size against the
	 * layout the magic named -- and a pointer that fails is refused having
	 * changed nothing.  So "log and continue" now continues from a state
	 * the allocator never touched, rather than from a half-applied free.
	 * That is the difference between tolerating a foreign pointer and
	 * acting on a forged one.
	 *
	 * The remaining gap versus glibc is DETECTION LOUDNESS, not corruption:
	 * a program with a double-free bug gets a log line from us where glibc
	 * would kill it.  Set UMEM_OPTIONS=abort to get the abort back --
	 * the option exists as of this comment; the earlier text named
	 * "abort=1", which no option table entry ever implemented and which
	 * the flag parser would have rejected for taking a value.  (Its
	 * inverse has always been UMEM_DEBUG=noabort -- a different
	 * variable -- which is part of why nobody noticed.)
	 *
	 * ORDER: this constructor runs at load; umem_init() parses
	 * UMEM_OPTIONS on the first allocation, after it.  So the env
	 * setting wins, which is the intent.
	 */
	extern uint_t umem_abort;
	umem_abort = 0;

	/* Arm the allocation-failure injector from UMEM_FAIL_* (no-op unless
	 * one is set). Read once here so a child spawned with the variables
	 * set is faulted and the parent is not. */
	fi_init();
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
	 * Allocation-failure injection (UMEM_FAULT). Checked only after the
	 * dlsym-bootstrap guard above, so loader-internal allocations are
	 * never faulted. __builtin_return_address(0) is the application call
	 * site of this malloc.
	 */
	if (__builtin_expect(fi_armed, 0) &&
	    fi_should_fail(__builtin_return_address(0), size)) {
		errno = ENOMEM;
		return (NULL);
	}

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

	/*
	 * Steady-state fast path.  Once READY, the overwhelmingly common case
	 * is a umem-owned pointer, and interpose_owner_of() classifies it by
	 * running process_free(ptr, 0, ...) -- a full header decode -- only
	 * for umem_malloc_free() to run process_free(ptr, 1, ...) and decode
	 * the SAME header again.  Two decodes per free on the hot path.
	 *
	 * The static check is a range compare on this file's own buffer and
	 * must stay first (those pointers have no umem header).  The libc
	 * check is gated on the live count (see libc_ptr_live).  Everything
	 * else goes to umem_malloc_free(), which itself does step 1 of
	 * process_free()'s validation order (bootstrap pointer -> munmap) and
	 * then validates and either frees or reports -- exactly what the
	 * classify-then-free sequence did, in one pass.  Since P5.8 a rejected
	 * pointer leaves state untouched, so skipping the pre-classification
	 * loses no safety.  A bootstrap check here would be the same read of
	 * ptr[-1] that umem_malloc_free() is about to make.
	 */
	if (__builtin_expect(atomic_load(&interpose_state) == INTERPOSE_READY, 1) &&
	    !is_static_pointer(ptr) &&
	    /*
	     * is_libc_pointer()'s gate, inline.  A nonzero count falls to
	     * interpose_owner_of() below, which does the locked scan.
	     */
	    atomic_load_explicit(&libc_ptr_live, memory_order_acquire) == 0) {
		umem_malloc_free(ptr);
		return;
	}

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
	 * which logs a recoverable error (umem_abort is 0 in interpose mode --
	 * see umem_interpose_init() for why, and for why that is weaker than
	 * glibc rather than "the same as") rather than crashing on a foreign
	 * pointer.  Since P5.8, process_free() refuses such a pointer without
	 * mutating allocator state, so continuing here cannot leave a
	 * half-applied free behind.
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

	/*
	 * Allocation-failure injection for a top-level calloc. Done here, with
	 * calloc's own application return address, so the failure is attributed
	 * to the caller's calloc site (not the internal malloc below).
	 */
	if (__builtin_expect(fi_armed, 0) &&
	    fi_should_fail(__builtin_return_address(0), size)) {
		errno = ENOMEM;
		return (NULL);
	}

	calloc_depth++;
	/*
	 * The internal malloc() must not run a SECOND, independent injection
	 * decision (it would advance the sequence counter at the wrong site).
	 * The thread-local reentrancy guard suppresses it; the real decision
	 * was made just above at calloc's own site.
	 */
	fi_in_fault = 1;
	ret = malloc(size);
	fi_in_fault = 0;
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

	/*
	 * Allocation-failure injection for realloc's grow/move. Done with
	 * realloc's own application return address and BEFORE the old
	 * allocation is touched, so a faulted realloc leaves ptr live and
	 * intact (correct C realloc failure semantics).
	 */
	if (__builtin_expect(fi_armed, 0) &&
	    fi_should_fail(__builtin_return_address(0), size)) {
		errno = ENOMEM;
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

	/* The injection decision was already made above; suppress a second
	 * one inside the internal malloc via the reentrancy guard. */
	fi_in_fault = 1;
	new_ptr = malloc(size);
	fi_in_fault = 0;
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

	/*
	 * Allocation-failure injection. memalign is the single aligned-alloc
	 * chokepoint: posix_memalign, aligned_alloc and valloc all route
	 * through it, so one check here covers every aligned path (Rust's
	 * System allocator uses posix_memalign / aligned_alloc for
	 * over-aligned layouts). On failure, errno=ENOMEM + NULL; posix_memalign
	 * translates that to its ENOMEM return below.
	 */
	if (__builtin_expect(fi_armed, 0) &&
	    fi_should_fail(__builtin_return_address(0), size)) {
		errno = ENOMEM;
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
