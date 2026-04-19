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

#ifndef _UMEM_GC_H
#define	_UMEM_GC_H

#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * GC allocation flags (stored in gc_flags)
 */
#define	UMEM_GC_ATOMIC		0x00000001	/* no pointers to scan */
#define	UMEM_GC_FINALIZE	0x00000002	/* has finalizer */
#define	UMEM_GC_MARKED		0x00000004	/* reachable this cycle */
#define	UMEM_GC_PINNED		0x00000008	/* exempt from collection */

/*
 * GC phase state machine
 */
typedef enum umem_gc_phase {
	GC_PHASE_IDLE = 0,
	GC_PHASE_MARK,		/* concurrent marking */
	GC_PHASE_REMARK,	/* STW: fix up missed references */
	GC_PHASE_SWEEP,		/* concurrent sweep */
	GC_PHASE_FINALIZE	/* STW: run finalizers */
} umem_gc_phase_t;

/*
 * Per-GC-allocation header, prepended to every GC-managed object.
 * The user pointer returned by umem_gc_alloc() points just past this header.
 */
typedef struct umem_gc_header {
	_Atomic uint32_t gc_mark;	/* mark bit for current cycle */
	uint32_t	gc_flags;	/* UMEM_GC_ATOMIC, etc. */
	size_t		gc_size;	/* user-visible allocation size */
	void		(*gc_finalizer)(void *, void *);
	void		*gc_finalizer_data;
	struct umem_gc_header *gc_next;	/* global GC object list */
	struct umem_gc_header *gc_prev;	/* doubly-linked for O(1) removal */
} umem_gc_header_t;

/*
 * Convert between user pointer and GC header
 */
#define	UMEM_GC_HEADER(ptr) \
	((umem_gc_header_t *)((char *)(ptr) - sizeof (umem_gc_header_t)))

#define	UMEM_GC_USER_PTR(hdr) \
	((void *)((char *)(hdr) + sizeof (umem_gc_header_t)))

/*
 * GC statistics
 */
typedef struct umem_gc_stats {
	size_t		gcs_heap_size;		/* current heap size */
	size_t		gcs_free_bytes;		/* bytes on free lists */
	size_t		gcs_total_allocated;	/* lifetime bytes allocated */
	size_t		gcs_bytes_allocated;	/* bytes since last GC */
	size_t		gcs_bytes_survived;	/* bytes surviving last GC */
	uint64_t	gcs_collections;	/* total GC cycles */
	uint64_t	gcs_objects_freed;	/* total objects freed */
	uint64_t	gcs_finalizers_run;	/* total finalizers executed */
} umem_gc_stats_t;

/*
 * Thread registration record (for root scanning)
 */
typedef struct umem_gc_thread {
	pthread_t		gct_tid;
	void			*gct_stack_base;
	size_t			gct_stack_size;
	struct umem_gc_thread	*gct_next;
} umem_gc_thread_t;

/* ----------------------------------------------------------------
 * umem-native GC API
 * ---------------------------------------------------------------- */

int	umem_gc_init(void);
void	*umem_gc_alloc(size_t size, int flags);
void	*umem_gc_alloc_atomic(size_t size, int flags);
void	*umem_gc_realloc(void *ptr, size_t new_size);
void	umem_gc_free(void *ptr);
void	umem_gc_collect(void);
int	umem_gc_register_thread(void);
void	umem_gc_unregister_thread(void);
void	umem_gc_register_finalizer(void *ptr,
	    void (*fn)(void *, void *), void *cd,
	    void (**old_fn)(void *, void *), void **old_cd);

/* Statistics */
void		umem_gc_get_stats(umem_gc_stats_t *stats);
size_t		umem_gc_get_heap_size(void);
size_t		umem_gc_get_free_bytes(void);
size_t		umem_gc_get_total_bytes(void);

/* GC phase query */
umem_gc_phase_t	umem_gc_get_phase(void);

/* ----------------------------------------------------------------
 * Hooks for mark/sweep phases (used by root scanner, Task #2)
 * ---------------------------------------------------------------- */

umem_gc_header_t *umem_gc_find_header(void *ptr);
void		umem_gc_mark_object(umem_gc_header_t *hdr);
void		umem_gc_walk_objects(
		    void (*fn)(umem_gc_header_t *hdr, void *arg), void *arg);

/* Thread list access (for root scanning) */
umem_gc_thread_t *umem_gc_get_thread_list(void);
void		umem_gc_lock_threads(void);
void		umem_gc_unlock_threads(void);

/* GC object list lock (for sweep phase coordination) */
void		umem_gc_lock_objects(void);
void		umem_gc_unlock_objects(void);

/* Advance GC phase (called by orchestrator) */
void		umem_gc_set_phase(umem_gc_phase_t phase);

/* Current mark value (incremented each cycle) */
uint32_t	umem_gc_get_mark_value(void);

/* ----------------------------------------------------------------
 * Boehm-compatible macros (convenience wrappers)
 * The full Boehm-compatible gc.h header is provided separately.
 * These are the core macros used internally.
 * ---------------------------------------------------------------- */

#define	GC_INIT()		umem_gc_init()
#define	GC_MALLOC(sz)		umem_gc_alloc((sz), 0)
#define	GC_MALLOC_ATOMIC(sz)	umem_gc_alloc_atomic((sz), 0)
#define	GC_REALLOC(p, sz)	umem_gc_realloc((p), (sz))
#define	GC_FREE(p)		umem_gc_free(p)
#define	GC_gcollect()		umem_gc_collect()

#define	GC_register_my_thread()		umem_gc_register_thread()
#define	GC_unregister_my_thread()	umem_gc_unregister_thread()

#define	GC_REGISTER_FINALIZER(p, fn, cd, ofn, ocd) \
	umem_gc_register_finalizer((p), (fn), (cd), (ofn), (ocd))

#define	GC_get_heap_size()	umem_gc_get_heap_size()
#define	GC_get_free_bytes()	umem_gc_get_free_bytes()
#define	GC_get_total_bytes()	umem_gc_get_total_bytes()

#ifdef __cplusplus
}
#endif

#endif /* _UMEM_GC_H */
