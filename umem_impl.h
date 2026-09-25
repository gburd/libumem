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
 * Copyright 2004 Sun Microsystems, Inc.  All rights reserved.
 *
 * Portions Copyright 2012 Joyent, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 * Portions Copyright 2006-2008 Message Systems, Inc. All rights reserved.
 */

/*
 * Copyright (c) 2012 Joyent, Inc.  All rights reserved.
 * Copyright (c) 2015 by Delphix. All rights reserved.
 */

#ifndef _UMEM_IMPL_H
#define	_UMEM_IMPL_H

/* #pragma ident	"@(#)umem_impl.h	1.6	05/06/08 SMI" */

#include "config.h"
#include <umem.h>
#include <stdatomic.h>
#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#elif defined(__GNUC__)
/* GCC always provides __builtin_alloca */
# ifndef alloca
#  define alloca __builtin_alloca
# endif
#endif

#ifdef HAVE_SYS_SYSMACROS_H
#include <sys/sysmacros.h>
#endif

#if HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include <sys/vmem.h>
#ifdef HAVE_THREAD_H
#include <thread.h>
/* Solaris thread.h provides mutex_t, cond_t, hrtime_t, thr_create, etc.
 * but not our compat macros. Define them before including sol_compat.h
 * and tell sol_compat.h to skip the type/function redefinitions. */
# ifndef THR_RETURN
#  define THR_RETURN void *
#  define THR_API
# endif
# ifndef INLINE
#  define INLINE inline
# endif
# define _EC_UMEM_SOL_COMPAT_TYPES_DEFINED
# include "sol_compat.h"
#else
# include "sol_compat.h"
#endif

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * umem memory allocator: implementation-private data structures
 */

/*
 * Internal flags for umem_cache_create
 */
#define	UMC_QCACHE	0x00100000
#define	UMC_INTERNAL	0x80000000

/*
 * Cache flags
 */
#define	UMF_AUDIT	0x00000001	/* transaction auditing */
#define	UMF_DEADBEEF	0x00000002	/* deadbeef checking */
#define	UMF_REDZONE	0x00000004	/* redzone checking */
#define	UMF_CONTENTS	0x00000008	/* freed-buffer content logging */
#define	UMF_CHECKSIGNAL	0x00000010	/* abort when in signal context */
#define	UMF_NOMAGAZINE	0x00000020	/* disable per-cpu magazines */
#define	UMF_FIREWALL	0x00000040	/* put all bufs before unmapped pages */
#define	UMF_LITE	0x00000100	/* lightweight debugging */

#define	UMF_HASH	0x00000200	/* cache has hash table */
#define	UMF_RANDOMIZE	0x00000400	/* randomize other umem_flags */
#define	UMF_PTC		0x00000800	/* cache has per-thread caching */

#define	UMF_CHECKNULL	0x00001000	/* heap exhaustion checking */

#define	UMF_BUFTAG	(UMF_DEADBEEF | UMF_REDZONE)
#define	UMF_TOUCH	(UMF_BUFTAG | UMF_LITE | UMF_CONTENTS)
#define	UMF_RANDOM	(UMF_TOUCH | UMF_AUDIT | UMF_NOMAGAZINE)
#define	UMF_DEBUG	(UMF_RANDOM | UMF_FIREWALL)

#define	UMEM_STACK_DEPTH	umem_stack_depth

#define	UMEM_FREE_PATTERN		0xdeadbeefdeadbeefULL
#define	UMEM_UNINITIALIZED_PATTERN	0xbaddcafebaddcafeULL
#define	UMEM_REDZONE_PATTERN		0xfeedfacefeedfaceULL
#define	UMEM_REDZONE_BYTE		0xbb

/*
 * Cache line size and alignment macros.
 * Use the configure-detected value if available, otherwise default to 64.
 * Override at configure time with --with-cache-line-size=N for platforms
 * that use larger coherence lines (e.g., SPARC T-series: 128B).
 */
#ifdef UMEM_CACHE_LINE_SIZE_CONFIG
#define	UMEM_CACHE_LINE_SIZE	UMEM_CACHE_LINE_SIZE_CONFIG
#else
#define	UMEM_CACHE_LINE_SIZE	64
#endif
#define	UMEM_CACHE_ALIGNED	__attribute__((aligned(UMEM_CACHE_LINE_SIZE)))

/*
 * Prefetch macros for performance optimization
 */
#define	UMEM_PREFETCH_READ(addr)	__builtin_prefetch((addr), 0, 3)
#define	UMEM_PREFETCH_WRITE(addr)	__builtin_prefetch((addr), 1, 3)
#define	UMEM_PREFETCH_BATCH(base, stride, count) \
	do { \
		for (int _i = 0; _i < (count); _i++) { \
			UMEM_PREFETCH_READ((char *)(base) + (_i) * (stride)); \
		} \
	} while (0)

/*
 * Per-thread CPU hint: which per-CPU cache (cache_cpu[]) this thread uses.
 *
 * THE PORT BUG THIS REPLACES (P8.2's real mechanism).  On Solaris CPUHINT()
 * is thr_self(), a small integer thread id, so `hint & cache_cpu_mask` spreads
 * threads over the per-CPU caches.  This port defined CPUHINT() as
 * pthread_self() cast to int -- a stack ADDRESS, page-aligned, whose low bits
 * are always zero.  `hint & mask` was therefore 0 for EVERY thread, and the
 * value was cached in TLS once, forever.  Measured on an 8-vCPU box with 8
 * threads allocating 2560-byte objects: 7 threads on cache_cpu[0], one
 * elsewhere (the one that happened to register rseq before its first
 * allocation).  Every operation that reached the magazine layer -- every size
 * above umem_ptc_maxsize, and every PTC miss below it -- serialised on ONE
 * cc_lock for the whole process.  That is the 1k:4k collapse: 0.06x glibc at
 * 64 threads, and 1.5 Mops/s at t=8 where 1536-byte objects (PTC-served) do
 * 32.  Not the depot, not magazine size, not a missing size class.
 *
 * The old comment here claimed the hint was "reset on magazine reload to
 * detect CPU migration".  The 2026-09-23 rewrite of this comment said
 * "Nothing reset it."  THAT WAS FALSE (comment review 2026-09-24, CB-1):
 * umem_cpu_reload() has always called reset_cpu_hint_cache() below, so the
 * hint IS re-derived on every CPU-layer magazine exchange.  Before the fix
 * the reset was invisible because the re-derived value was again
 * pthread_self() & mask == 0.  After it, the reset is live and the hint
 * tracks migration at reload granularity -- one sched_getcpu()/rseq read
 * per magazine reload, not per call.  That is the cost model this comment
 * should have described; the 5 % t=1 figure below is for re-reading on
 * EVERY call, which is a different thing.  Kept: a reload is where a
 * migrated thread most plausibly is, and the cost is amortised over
 * magsize operations.
 *
 * WHAT THIS DOES NOW.  On the one miss, prefer the kernel-maintained rseq
 * cpu_id, registering the thread first if the caller has not (registration
 * is idempotent and cheap; before, the first allocation computed its hint
 * BEFORE the rseq block in _umem_cache_alloc registered, so the rseq path
 * was never consulted for the value that got cached).  Without rseq, use
 * sched_getcpu() -- a real CPU number.  Only as a last resort fall back to a
 * thread-id hash, and then hash the address bits that vary (>> 12) rather
 * than the ones that never do.
 *
 * The value is cached in TLS after the first successful read, as before.
 * A first attempt re-read the rseq cpu_id on every call to track migration
 * and cost 5 % at t=1 on the magazine path (4.16 -> 3.94 Mops, 2560 B,
 * median of 7, alternating builds): three TLS loads and a dereference where
 * there had been one load.  Spread is the property that matters here, not
 * exactness -- a stale-after-migration hint costs cache-line traffic, not
 * serialisation, and is exactly what the Solaris thr_self() hint always
 * did.  So: read once, correctly, and cache.
 */
extern __thread int cached_cpu_hint;

/*
 * rseq integration for CPU hint caching.
 * When rseq is available and registered, we read the kernel-maintained
 * cpu_id instead of calling sched_getcpu() or using thread ID hashing.
 */
#if defined(__linux__) && defined(HAVE_LINUX_RSEQ_H)
#include "umem_rseq.h"
#endif

#if defined(__linux__)
#include <sched.h>
#endif

static inline int __attribute__((always_inline))
get_cached_cpu_hint(void)
{
	int hint = cached_cpu_hint;

	if (unlikely(hint == -1)) {
#ifdef UMEM_RSEQ_AVAILABLE
		if (umem_rseq_enabled) {
			if (!umem_rseq_registered)
				(void) umem_rseq_register_thread();
			if (umem_rseq_cpu_idp != NULL) {
				hint = (int)*umem_rseq_cpu_idp;
				if (hint >= 0) {
					cached_cpu_hint = hint;
					return hint;
				}
			}
		}
#endif
#if defined(__linux__)
		hint = sched_getcpu();
		if (hint < 0)
#endif
		{
#ifdef CPUHINT
			hint = CPUHINT();
#else
			extern thread_t _thr_self(void);
			hint = (int)((uintptr_t)(_thr_self()) >> 12);
#endif
			if (hint < 0)
				hint = -hint;
		}
		cached_cpu_hint = hint;
	}
	return hint;
}

/*
 * Reset the CPU hint cache to force refresh on next access.
 * Called during magazine reload to detect CPU migration.
 */
static inline void __attribute__((always_inline))
reset_cpu_hint_cache(void)
{
	cached_cpu_hint = -1;
}

#define	UMEM_FATAL_FLAGS	(UMEM_NOFAIL)
#define	UMEM_SLEEP_FLAGS	(0)

/*
 * Redzone size encodings for umem_alloc() / umem_free().  We encode the
 * allocation size, rather than storing it directly, so that umem_free()
 * can distinguish frees of the wrong size from redzone violations.
 */
#define	UMEM_SIZE_ENCODE(x)	(251 * (x) + 1)
#define	UMEM_SIZE_DECODE(x)	((x) / 251)
#define	UMEM_SIZE_VALID(x)	((x) % 251 == 1)

/*
 * The bufctl (buffer control) structure keeps some minimal information
 * about each buffer: its address, its slab, and its current linkage,
 * which is either on the slab's freelist (if the buffer is free), or
 * on the cache's buf-to-bufctl hash table (if the buffer is allocated).
 * In the case of non-hashed, or "raw", caches (the common case), only
 * the freelist linkage is necessary: the buffer address is at a fixed
 * offset from the bufctl address, and the slab is at the end of the page.
 *
 * NOTE: bc_next must be the first field; raw buffers have linkage only.
 */
typedef struct umem_bufctl {
	struct umem_bufctl	*bc_next;	/* next bufctl struct */
	void			*bc_addr;	/* address of buffer */
	struct umem_slab	*bc_slab;	/* controlling slab */
} umem_bufctl_t;

/*
 * The UMF_AUDIT version of the bufctl structure.  The beginning of this
 * structure must be identical to the normal bufctl structure so that
 * pointers are interchangeable.
 */

#define	UMEM_BUFCTL_AUDIT_SIZE_DEPTH(frames) \
	((size_t)(&((umem_bufctl_audit_t *)0)->bc_stack[frames]))

/*
 * umem_bufctl_audits must be allocated from a UMC_NOHASH cache, so we
 * require that 2 of them, plus 2 buftags, plus a umem_slab_t, all fit on
 * a single page.
 *
 * For ILP32, this is about 1000 frames.
 * For LP64, this is about 490 frames.
 */

#define	UMEM_BUFCTL_AUDIT_ALIGN	32

#define	UMEM_BUFCTL_AUDIT_MAX_SIZE					\
	(P2ALIGN((PAGESIZE - sizeof (umem_slab_t))/2 -			\
	    sizeof (umem_buftag_t), UMEM_BUFCTL_AUDIT_ALIGN))

#define	UMEM_MAX_STACK_DEPTH						\
	((UMEM_BUFCTL_AUDIT_MAX_SIZE -					\
	    UMEM_BUFCTL_AUDIT_SIZE_DEPTH(0)) / sizeof (uintptr_t))

typedef struct umem_bufctl_audit {
	struct umem_bufctl	*bc_next;	/* next bufctl struct */
	void			*bc_addr;	/* address of buffer */
	struct umem_slab	*bc_slab;	/* controlling slab */
	umem_cache_t		*bc_cache;	/* controlling cache */
	hrtime_t		bc_timestamp;	/* transaction time */
	thread_t		bc_thread;	/* thread doing transaction */
	struct umem_bufctl	*bc_lastlog;	/* last log entry */
	void			*bc_contents;	/* contents at last free */
	int			bc_depth;	/* stack depth */
	uintptr_t		bc_stack[1];	/* pc stack */
} umem_bufctl_audit_t;

#define	UMEM_LOCAL_BUFCTL_AUDIT(bcpp)					\
		*(bcpp) = (umem_bufctl_audit_t *)			\
		    alloca(UMEM_BUFCTL_AUDIT_SIZE)

#define	UMEM_BUFCTL_AUDIT_SIZE						\
	UMEM_BUFCTL_AUDIT_SIZE_DEPTH(UMEM_STACK_DEPTH)

/*
 * A umem_buftag structure is appended to each buffer whenever any of the
 * UMF_BUFTAG flags (UMF_DEADBEEF, UMF_REDZONE, UMF_VERIFY) are set.
 */
typedef struct umem_buftag {
	uint64_t		bt_redzone;	/* 64-bit redzone pattern */
	umem_bufctl_t		*bt_bufctl;	/* bufctl */
	intptr_t		bt_bxstat;	/* bufctl ^ (alloc/free) */
} umem_buftag_t;

#define	UMEM_BUFTAG(cp, buf)		\
	((umem_buftag_t *)((char *)(buf) + (cp)->cache_buftag))

#define	UMEM_BUFCTL(cp, buf)		\
	((umem_bufctl_t *)((char *)(buf) + (cp)->cache_bufctl))

#define	UMEM_BUF(cp, bcp)		\
	((void *)((char *)(bcp) - (cp)->cache_bufctl))

/*
 * Slab freelist link mangling (P5.4).
 *
 * For every cache without UMF_HASH -- the default for small objects -- the
 * bufctl, and therefore bc_next, sits at cache_bufctl INSIDE THE USER BUFFER
 * (see UMEM_BUFCTL above and umem_cache_create()'s
 * cache_bufctl = chunksize - UMEM_ALIGN).  bc_next is the slab freelist link:
 * umem_slab_free() stores sp->slab_head there and umem_slab_alloc() pops it
 * back off and hands the result out as the next allocation.  An ordinary
 * overflow out of a live buffer into the tail of the adjacent FREED one
 * therefore reaches it, and unmangled that is an attacker-chosen-address
 * primitive -- worse than glibc, which has mangled these links since 2.32
 * ("safe-linking").
 *
 * The stored form is  ptr ^ umem_link_cookie ^ (&slot >> 12), so the value in
 * memory depends on WHERE it is stored: overwriting a link with a chosen
 * address no longer yields that address, and aiming anywhere deliberately
 * requires knowing both the per-process cookie and the link's own address.
 * XOR is an involution, so one macro both encodes and decodes, and NULL (the
 * end of the freelist) round-trips with no special case.
 *
 * This covers the SLAB FREELIST use of bc_next only.  The UMF_HASH
 * allocated-address chain -- built in umem_slab_alloc(), unlinked in
 * umem_slab_free(), rebuilt by umem_hash_rescale(), walked by umem_error()
 * and by the inspectors' hash-table walks -- stores plain pointers, because
 * those bufctls are allocated from cache_bufctl_cache, outside any user
 * buffer, where a buffer overflow cannot reach them.
 *
 * EVERY reader of a slab freelist link must decode.  In-tree readers:
 * umem.c umem_slab_create() (both the build loop and its failure unwind),
 * umem_slab_alloc(), umem_slab_free(), umem_slab_destroy();
 * umem_inspect.c slab_buf_is_free() and collect_freed_cache();
 * umem_introspect.c is_allocated() (built only under --enable-introspect).
 *
 * Building the library with -DUMEM_NO_LINK_MANGLE restores the pre-fix
 * behaviour in an otherwise identical binary.  That is the control arm for
 * test/unit/test_freelist_mangle (which must fail against it) and for the
 * throughput A/B, and it is not a supported configuration.
 */
extern uintptr_t umem_link_cookie;

#ifdef	UMEM_NO_LINK_MANGLE
#define	UMEM_LINK_MANGLE(slotp, val)	((umem_bufctl_t *)(val))
#else
#define	UMEM_LINK_MANGLE(slotp, val)					\
	((umem_bufctl_t *)((uintptr_t)(val) ^ umem_link_cookie ^		\
	    ((uintptr_t)(slotp) >> 12)))
#endif

#define	UMEM_LINK_DEMANGLE(slotp, val)	UMEM_LINK_MANGLE(slotp, val)

#define	UMEM_SLAB(cp, buf)		\
	((umem_slab_t *)P2END((uintptr_t)(buf), (cp)->cache_slabsize) - 1)

#define	UMEM_CPU_CACHE(cp, cpu)		\
	(umem_cpu_cache_t *)((char *)cp + cpu->cpu_cache_offset)

#define	UMEM_MAGAZINE_VALID(cp, mp)	\
	(((umem_slab_t *)P2END((uintptr_t)(mp), PAGESIZE) - 1)->slab_cache == \
	    (cp)->cache_magtype->mt_cache)

/*
 * A magazine's OWN capacity in rounds, from the slab header of the
 * umem_magazine_<N> cache it was allocated from (bufsize is
 * (N + 1) * sizeof (void *): one slot for mag_next plus N rounds).  This is
 * the header form of umem.c's umem_mag_capacity(), for readers outside
 * umem.c.  Use it, never cp->cache_magtype->mt_magsize, to bound indexing
 * into a magazine already in hand (P1.3b): after a resize, shells of the
 * OLD capacity stay on the depot lists until popped and destroyed, so the
 * cache's current magtype over-reads them.  umem_inspect.c did exactly that
 * (comment review 2026-09-24, CB-2).
 */
#define	UMEM_MAGAZINE_CAPACITY(mp)	\
	((int)(((umem_slab_t *)P2END((uintptr_t)(mp), PAGESIZE) - 1)-> \
	    slab_cache->cache_bufsize / sizeof (void *)) - 1)

#define	UMEM_SLAB_MEMBER(sp, buf)	\
	((size_t)(buf) - (size_t)(sp)->slab_base < \
	    (sp)->slab_cache->cache_slabsize)

#define	UMEM_BUFTAG_ALLOC	0xa110c8edUL
#define	UMEM_BUFTAG_FREE	0xf4eef4eeUL

/*
 * Slab page states for background reclamation.
 */
#define	SLAB_ACTIVE	0	/* has allocated buffers (refcnt > 0) */
#define	SLAB_DIRTY	1	/* empty, pages still resident */
#define	SLAB_CLEAN	2	/* empty, pages advised away (MADV_DONTNEED) */
#define	SLAB_RECLAIMING	3	/* madvise in progress, do not allocate */

typedef struct umem_slab {
	struct umem_cache	*slab_cache;	/* controlling cache */
	void			*slab_base;	/* base of allocated memory */
	struct umem_slab	*slab_next;	/* next slab on freelist */
	struct umem_slab	*slab_prev;	/* prev slab on freelist */
	struct umem_bufctl	*slab_head;	/* first free buffer */
	long			slab_refcnt;	/* outstanding allocations */
	long			slab_chunks;	/* chunks (bufs) in this slab */
	uint32_t		slab_state;	/* SLAB_ACTIVE/DIRTY/CLEAN */
	uint32_t		slab_idle_time;	/* seconds empty (approx) */
	struct umem_slab	*slab_reclaim_next; /* update-thread reclaim list */
} umem_slab_t;

#define	UMEM_HASH_INITIAL	64

#define	UMEM_HASH(cp, buf)	\
	((cp)->cache_hash_table +	\
	(((uintptr_t)(buf) >> (cp)->cache_hash_shift) & (cp)->cache_hash_mask))

typedef struct umem_magazine {
	void	*mag_next;
	void	*mag_round[1];		/* one or more rounds */
} umem_magazine_t;

/*
 * The magazine types for fast per-cpu allocation
 */
typedef struct umem_magtype {
	int		mt_magsize;	/* magazine size (number of rounds) */
	int		mt_align;	/* magazine alignment */
	size_t		mt_minbuf;	/* all smaller buffers qualify */
	size_t		mt_maxbuf;	/* no larger buffers qualify */
	umem_cache_t	*mt_cache;	/* magazine cache */
} umem_magtype_t;

#if (defined(__PTHREAD_MUTEX_SIZE__) && __PTHREAD_MUTEX_SIZE__ >= 24) || defined(UMEM_PTHREAD_MUTEX_TOO_BIG)
#define	UMEM_CPU_CACHE_SIZE	128	/* must be power of 2 */
#else
#define	UMEM_CPU_CACHE_SIZE	64	/* must be power of 2 */
#endif
#define	UMEM_CPU_PAD		(UMEM_CPU_CACHE_SIZE - sizeof (mutex_t) - \
	2 * sizeof (uint_t) - 2 * sizeof (void *) - 4 * sizeof (int))
#define	UMEM_CACHE_SIZE(ncpus)	\
	((size_t)(&((umem_cache_t *)0)->cache_cpu[ncpus]))

typedef struct umem_cpu_cache {
	mutex_t		cc_lock;	/* protects slow path (magazine reload) */
	int		cc_rounds;	/* number of objects in loaded mag */
	int		cc_prounds;	/* number of objects in previous mag */
	umem_magazine_t	*cc_loaded;	/* the currently loaded magazine */
	umem_magazine_t	*cc_ploaded;	/* the previously loaded magazine */
	int		cc_magsize;	/* number of rounds in a full mag */
	int		cc_flags;	/* CPU-local copy of cache_flags */
	uint_t		cc_alloc;	/* allocations from this cpu */
	uint_t		cc_free;	/* frees to this cpu */
#if (!defined(_LP64) || defined(UMEM_PTHREAD_MUTEX_TOO_BIG)) && !defined(_WIN32)
	/* on win32, UMEM_CPU_PAD evaluates to zero, and the MS compiler
	 * won't allow static initialization of arrays containing structures
	 * that contain zero size arrays */
	char		cc_pad[UMEM_CPU_PAD]; /* for nice alignment (32-bit) */
#endif
} __attribute__((aligned(UMEM_CACHE_LINE_SIZE))) umem_cpu_cache_t;

/*
 * Tagged pointer for lock-free stack operations.
 * Packs a pointer and a 16-bit version counter into a single 64-bit word
 * so the pair can be atomically loaded/CAS'd with standard 64-bit atomics.
 *
 * Requires that userspace pointers fit in 48 bits. This is true for:
 *   - x86_64: canonical form enforces 48-bit user VA
 *   - aarch64: 48-bit VA is the default (ARMv8.2 52-bit VA is NOT supported)
 *   - RISC-V Sv39/Sv48: 39/48-bit VA
 *   - SPARC: 44-bit VA
 *
 * Use umem_tagged_ptr_check() at init time to verify at runtime.
 */
#if defined(__aarch64__) && defined(__ARM_FEATURE_MEMORY_TAGGING)
#error "ARM MTE (Memory Tagging Extension) conflicts with tagged pointer scheme"
#endif

#define	UMEM_PTR_MASK	0x0000FFFFFFFFFFFFULL
#define	UMEM_VER_SHIFT	48

typedef union umem_tagged_ptr {
	uint64_t	raw;		/* Atomic-friendly 64-bit representation */
	struct {
		/*
		 * On little-endian (x86_64, aarch64-le) the low 48 bits
		 * are the pointer and the high 16 are the version.
		 * We access them through the helper macros below rather
		 * than relying on bitfield layout.
		 */
		uint64_t	_bits;
	} _packed;
} umem_tagged_ptr_t;

static inline void *
umem_tagged_ptr_get(umem_tagged_ptr_t tp)
{
	return (void *)(uintptr_t)(tp.raw & UMEM_PTR_MASK);
}

static inline uint16_t
umem_tagged_ver_get(umem_tagged_ptr_t tp)
{
	return (uint16_t)(tp.raw >> UMEM_VER_SHIFT);
}

static inline umem_tagged_ptr_t
umem_tagged_ptr_make(void *ptr, uint16_t ver)
{
	umem_tagged_ptr_t tp;
	tp.raw = ((uint64_t)(uintptr_t)ptr & UMEM_PTR_MASK) |
	    ((uint64_t)ver << UMEM_VER_SHIFT);
	return tp;
}

/*
 * Verify at runtime that the tagged pointer scheme works on this platform.
 * Checks that a stack address fits within UMEM_PTR_MASK.
 *
 * On ARMv8.2+ with 52-bit VA (LVA) or RISC-V Sv57 (57-bit VA), the
 * 48-bit mask is insufficient. We detect these at runtime and panic
 * with a clear message rather than silently corrupting data.
 *
 * Returns 0 on success, -1 if tagged pointers cannot be used safely.
 */
static inline int
umem_tagged_ptr_check(void)
{
#if defined(_LP64) && !defined(ARCH_SPARC)
	volatile char probe;
	uintptr_t addr = (uintptr_t)&probe;

	if (addr & ~(uintptr_t)UMEM_PTR_MASK) {
#if defined(__aarch64__)
		/*
		 * ARMv8.2+ with 52-bit VA (LVA): the 48-bit mask
		 * would need to be widened to 0x000FFFFFFFFFFFFFULL.
		 * This is not yet supported.
		 */
		return (-1);
#elif defined(__riscv) && (__riscv_xlen == 64)
		/*
		 * RISC-V Sv57 uses 57-bit VA which exceeds even a
		 * 52-bit mask. Tagged pointers cannot work here
		 * without 128-bit atomics.
		 */
		return (-1);
#else
		return (-1);
#endif
	}
#endif
	return (0);
}

/*
 * The magazine lists used in the depot.
 * Each list is protected by its own mutex for simple, correct locking.
 * The depot is a cold path — the hot path is the per-CPU magazine layer.
 */
typedef struct umem_maglist {
	mutex_t		ml_lock;	/* protects this list */
	umem_magazine_t	*ml_list;	/* head of magazine linked list */
	long		ml_total;	/* number of magazines */
	long		ml_min;		/* min since last update */
	long		ml_reaplimit;	/* max reapable magazines */
	uint64_t	ml_alloc;	/* allocations from this list */
} __attribute__((aligned(UMEM_CACHE_LINE_SIZE))) umem_maglist_t;

#define	UMEM_CACHE_NAMELEN	31

struct umem_cache {
	/*
	 * Statistics
	 */
	uint64_t	cache_slab_create;	/* slab creates */
	uint64_t	cache_slab_destroy;	/* slab destroys */
	uint64_t	cache_slab_alloc;	/* slab layer allocations */
	uint64_t	cache_slab_free;	/* slab layer frees */
	uint64_t	cache_alloc_fail;	/* total failed allocations */
	uint64_t	cache_buftotal;		/* total buffers */
	uint64_t	cache_bufmax;		/* max buffers ever */
	uint64_t	cache_rescale;		/* # of hash table rescales */
	uint64_t	cache_lookup_depth;	/* hash lookup depth */
	uint64_t	cache_depot_contention;	/* mutex contention count */
	uint64_t	cache_depot_contention_prev; /* previous snapshot */
	uint64_t	cache_alloc_ops;	/* total allocation operations */

	/*
	 * Cache properties
	 */
	char		cache_name[UMEM_CACHE_NAMELEN + 1];
	size_t		cache_bufsize;		/* object size */
	size_t		cache_align;		/* object alignment */
	umem_constructor_t *cache_constructor;
	umem_destructor_t *cache_destructor;
	umem_reclaim_t	*cache_reclaim;
	void		*cache_private;		/* opaque arg to callbacks */
	vmem_t		*cache_arena;		/* vmem source for slabs */
	int		cache_cflags;		/* cache creation flags */
	int		cache_flags;		/* various cache state info */
	int		cache_uflags;		/* UMU_* flags */
	uint32_t	cache_mtbf;		/* induced alloc failure rate */
	umem_cache_t	*cache_next;		/* forward cache linkage */
	umem_cache_t	*cache_prev;		/* backward cache linkage */
	umem_cache_t	*cache_unext;		/* next in update list */
	umem_cache_t	*cache_uprev;		/* prev in update list */
	uint32_t	cache_cpu_mask;		/* mask for cpu offset */

	/*
	 * Slab layer
	 */
	mutex_t		cache_lock;		/* protects slab layer */
	size_t		cache_chunksize;	/* buf + alignment [+ debug] */
	size_t		cache_slabsize;		/* size of a slab */
	size_t		cache_bufctl;		/* buf-to-bufctl distance */
	size_t		cache_buftag;		/* buf-to-buftag distance */
	size_t		cache_verify;		/* bytes to verify */
	size_t		cache_contents;		/* bytes of saved content */
	size_t		cache_color;		/* next slab color */
	size_t		cache_mincolor;		/* maximum slab color */
	size_t		cache_maxcolor;		/* maximum slab color */
	size_t		cache_hash_shift;	/* get to interesting bits */
	size_t		cache_hash_mask;	/* hash table mask */
	umem_slab_t	*cache_freelist;	/* slab free list */
	umem_slab_t	cache_nullslab;		/* end of freelist marker */
	umem_cache_t	*cache_bufctl_cache;	/* source of bufctls */
	umem_bufctl_t	**cache_hash_table;	/* hash table base */
	/*
	 * Depot layer — mutex-protected magazine lists.
	 * Each list has its own lock; the depot is a cold path.
	 */
	umem_magtype_t	*cache_magtype;		/* magazine type */
	umem_maglist_t	cache_full;		/* full magazines */
	umem_maglist_t	cache_empty;		/* empty magazines */

	/*
	 * Per-CPU depot layer — eliminates cross-CPU contention.
	 *
	 * cache_depot_full, cache_depot_empty and (with rseq) cache_rseq are
	 * three arrays carved from ONE mapping, cache_percpu_map of
	 * cache_percpu_len bytes, allocated from umem_cache_arena (P6.4).
	 * They used to be three separate page-rounded mmap()s holding 512 B
	 * each on an 8-CPU box: 12 KB of an 18.6 KB per-cache footprint was
	 * page rounding, and destroying a cache munmap()ed three holes into
	 * whatever the kernel had merged the mappings into -- 29,159 VMAs
	 * left behind by 50k destroyed caches.  vmem memory is never
	 * unmapped, so destroy leaves no hole at all.
	 */
	void		*cache_percpu_map;	/* the one block, or NULL */
	size_t		cache_percpu_len;	/* its length */
	int		cache_depot_ncpus;	/* number of per-CPU depot slots */
	umem_maglist_t	*cache_depot_full;	/* array[ncpus] of full mag lists */
	umem_maglist_t	*cache_depot_empty;	/* array[ncpus] of empty mag lists */

	/* NUMA-aware depot statistics */
	uint64_t	cache_depot_local;	/* hits from local CPU */
	uint64_t	cache_depot_remote;	/* steals from same NUMA node */
	uint64_t	cache_depot_cross_node;	/* steals from remote NUMA node */
	uint64_t	cache_mag_total;	/* total magazine shells outstanding */

#ifdef UMEM_RSEQ_AVAILABLE
	/*
	 * Per-CPU rseq layer
	 * Each CPU has its own magazine cache accessed via rseq critical
	 * sections. Array of umem_rseq_get_ncpus() entries, carved from
	 * cache_percpu_map (64-byte aligned; it never needed a page).
	 */
	umem_rseq_cache_t *cache_rseq;		/* per-CPU rseq caches */
#endif

#ifdef UMEM_NUMA_AVAILABLE
	/*
	 * NUMA layer
	 */
	void *cache_numa_info;			/* NUMA-aware depot info */
#endif

	/*
	 * Per-CPU layer
	 * Each CPU cache is cache-line aligned to prevent false sharing.
	 * The alignment attribute on umem_cpu_cache_t ensures proper spacing.
	 */
	umem_cpu_cache_t cache_cpu[1];		/* cache_cpu_mask + 1 entries */
};

typedef struct umem_cpu_log_header {
	mutex_t		clh_lock;
	char		*clh_current;
	size_t		clh_avail;
	int		clh_chunk;
	int		clh_hits;
	char		clh_pad[UMEM_CPU_CACHE_SIZE -
				sizeof (mutex_t) - sizeof (char *) -
				sizeof (size_t) - 2 * sizeof (int)];
} umem_cpu_log_header_t;

typedef struct umem_log_header {
	mutex_t		lh_lock;
	char		*lh_base;
	int		*lh_free;
	size_t		lh_chunksize;
	int		lh_nchunks;
	int		lh_head;
	int		lh_tail;
	int		lh_hits;
	umem_cpu_log_header_t lh_cpu[1];	/* actually umem_max_ncpus */
} umem_log_header_t;

typedef struct umem_cpu {
	uint32_t cpu_cache_offset;
	uint32_t cpu_number;
} umem_cpu_t;

#define	UMEM_MAXBUF	131072

#define	UMEM_ALIGN		8	/* min guaranteed alignment */
#define	UMEM_ALIGN_SHIFT	3	/* log2(UMEM_ALIGN) */
#define	UMEM_VOID_FRACTION	8	/* never waste more than 1/8 of slab */

/*
 * Minimum objects per slab for hashed caches, and the slab size ceiling that
 * floor may push up to.
 *
 * The best-fit loop in umem_cache_create() picks the slab size with the least
 * per-object waste, trying 1..UMEM_VOID_FRACTION objects.  On Solaris, whose
 * heap quantum is 64 KiB, that yields 16 objects per slab for a 4 KiB chunk.
 * On Linux the quantum is the 4 KiB page, so the same loop yields ONE object
 * per slab: every 4 KiB allocation is its own span, its own mprotect(), and
 * its own kernel VMA.  vm.max_map_count (default 65530) is then exhausted at
 * roughly 5-8 GB of heap and umem_alloc() returns NULL -- measured 65,532 VMAs
 * at failure, and 64,270 of 64,275 mprotect calls being exactly 4096 bytes.
 * See docs/results/2026-09-22-umem-heap-ceiling-vma.md.
 *
 * This floor restores Solaris-like span density: a slab holds at least
 * UMEM_MIN_SLAB_OBJECTS objects, provided that does not push the slab above
 * UMEM_MIN_SLAB_CEILING (so large objects, which were already one-per-slab on
 * Solaris too, stay one-per-slab and do not balloon).  16 and 64 KiB are
 * exactly the Solaris figures for a 4 KiB chunk.
 *
 * PORTABILITY: this is deliberately NOT gated on the platform, and it does not
 * need to be.  It is arithmetically a no-op wherever the heap quantum is
 * already 64 KiB (illumos/Solaris via MAP_ALIGN): there, best-fit yields
 * >= 16 objects for every chunk small enough for the floor to matter, and for
 * larger chunks the ceiling equals the slab size best-fit already chose --
 * verified for chunks 64 B through 64 KiB, every one unchanged.  The floor
 * only changes behaviour where the quantum is smaller than 64 KiB, which is
 * exactly the case it exists to correct.  A third platform with some other
 * quantum gets the same rule applied to its own numbers, which is the intent:
 * the invariant is "at least 16 objects per slab up to 64 KiB", not "do
 * something Linux-specific".
 *
 * Cost: a hashed cache whose objects are 1-4 KiB now reserves a 16-64 KiB slab
 * on first use instead of one page.  That is more address space held per
 * lightly-used cache; it is NOT more RSS until the pages are touched, since
 * spans are MAP_NORESERVE.  Measured on a small workload before/after in the
 * commit that introduced this.
 */
#define	UMEM_MIN_SLAB_OBJECTS	16
#define	UMEM_MIN_SLAB_CEILING	(64 * 1024)

/*
 * Minimum slab size for a vmem QUANTUM CACHE (UMC_QCACHE).
 *
 * This is the second half of the heap-ceiling fix, and the half the first one
 * missed.  UMEM_MIN_SLAB_OBJECTS above covers the hashed best-fit path, which
 * is what 1-4 KiB objects take.  Objects of <= 512 B take a different path:
 * their slabs are one page each, and a page-sized span request is <= the
 * umem_va arena's qcache_max (8 pages), so it is served FROM A QCACHE.  The
 * qcache slab rule at that time was
 *
 *     bestfit = MAX(1 << highbit(3 * vm_qcache_max), 64)
 *
 * which for qcache_max = 32 KiB gives 128 KiB -- and each qcache slab is its
 * own mmap(MAP_FIXED) that the kernel does not merge.  One VMA per 128 KiB of
 * small-object heap, so vm.max_map_count (65530) is reached at about 8 GB.
 * Measured on c7i.2xlarge: 512 B objects failed at 16.77M objects / 8.2 GB
 * with 63,323 VMAs; 64 B objects were at 48,218 VMAs by 100M objects.  glibc
 * on the same box: 54 VMAs.  The first fix's own regression passed at 9 GB of
 * 4 KiB objects and so did not see this, because it tested only the class
 * that fix repaired.
 *
 * Levers were measured, not argued (2 GB of 512 B objects, VMAs):
 *   base                          15,702
 *   qcache_max 16 pages            7,891
 *   qcache_max -> 1 MiB slabs      2,034   (+1 MB RSS on a 5 MB heap)
 *   qcache slab floor 1 MiB        2,026
 *   qcache slab floor 4 MiB          274   (zero small-heap cost: 5.1 MB either way)
 *   mprotect instead of mmap      15,789   (no contiguous reservation to merge into)
 *   + 64 MiB reservations         15,706   (hand-out not address-ordered)
 *
 * So: a 4 MiB floor on qcache slabs.  Cost is address space, not RSS -- a
 * qcache slab is MAP_NORESERVE and only touched as it fills -- which is why the
 * small-heap RSS did not move.
 *
 * PORTABILITY, checked rather than assumed: this is not gated on platform and
 * does not need to be.  On illumos the heap quantum is 64 KiB (MAP_ALIGN) but
 * umem_va's requested qcache_max is still 8 * pagesize = 32 KiB, which is
 * SMALLER than one quantum, so vmem_create() computes nqcache = 0 and umem_va
 * has no quantum caches at all there.  The floor is a no-op on illumos because
 * the path it floors does not exist on illumos.  (With 8 KiB pages nqcache is
 * 1 and the single qcache slab grows 256 KiB -> 4 MiB, which is the same
 * address-space-only cost as on Linux.)  The qcache arm of test_slab_floor
 * pins the Linux behaviour; a 64 KiB-quantum arena in that test has no qcache
 * to check, which is itself the illumos fact.
 */
#define	UMEM_MIN_QCACHE_SLAB	(4 * 1024 * 1024)

/*
 * For 64 bits, buffers >= 16 bytes must be 16-byte aligned
 */
#ifdef _LP64
#define	UMEM_SECOND_ALIGN 16
#else
#define	UMEM_SECOND_ALIGN UMEM_ALIGN
#endif

#define	MALLOC_MAGIC			0x3a10c000 /* 8-byte tag */
#define	MEMALIGN_MAGIC			0x3e3a1000

#ifdef _LP64
#define	MALLOC_SECOND_MAGIC		0x16ba7000 /* 8-byte tag, 16-aligned */
#define	MALLOC_OVERSIZE_MAGIC		0x06e47000 /* 16-byte tag, _LP64 */
#endif

#define	UMEM_MALLOC_ENCODE(type, sz)	(uint32_t)((type) - (sz))
#define	UMEM_MALLOC_DECODE(stat, sz)	(uint32_t)((stat) + (sz))
#define	UMEM_FREE_PATTERN_32		(uint32_t)(UMEM_FREE_PATTERN)

#define	UMU_MAGAZINE_RESIZE	0x00000001
#define	UMU_HASH_RESCALE	0x00000002
#define	UMU_REAP		0x00000004
#define	UMU_NOTIFY		0x08000000
#define	UMU_ACTIVE		0x80000000

#define	UMEM_READY_INIT_FAILED		-1
#define	UMEM_READY_STARTUP		1
#define	UMEM_READY_INITING		2
#define	UMEM_READY			3

#ifdef UMEM_STANDALONE
extern void umem_startup(caddr_t, size_t, size_t, caddr_t, caddr_t);
extern int umem_add(caddr_t, size_t);
#endif

/*
 * Global allocation table for size-based cache lookup
 */
extern umem_cache_t *umem_alloc_table[UMEM_MAXBUF >> UMEM_ALIGN_SHIFT];

#ifdef	__cplusplus
}
#endif

#endif	/* _UMEM_IMPL_H */
