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
 * Ownership/borrowing system for libumem.
 *
 * Lightweight mode (always on):
 *   16-byte header before user data: {refcount, state, alloc_size}.
 *   Checks state on deref/drop/move; ~2% overhead.
 *
 * Full debug mode (UMEM_OPTIONS=ownership):
 *   Extended header with thread owner, borrow counts, alloc stack.
 *   Enforces single-owner, borrow-vs-mut-borrow exclusion, thread
 *   affinity.  ~15% overhead.
 */

#include "config.h"
#include "umem_own.h"
#include "umem_base.h"
#include "misc.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal header layouts                                            */
/* ------------------------------------------------------------------ */

/*
 * Lightweight header: 16 bytes prepended to every owned allocation.
 * {refcount, state} for runtime checks, {alloc_size} so drop can
 * free the correct total.  16-byte alignment preserves max_align_t.
 */
typedef struct own_hdr_lite {
	_Atomic uint32_t	ohl_refcount;
	_Atomic uint32_t	ohl_state;
	size_t			ohl_alloc_size;
} own_hdr_lite_t;

_Static_assert(sizeof(own_hdr_lite_t) <= 16,
    "lightweight header must fit in 16 bytes");

/*
 * Debug header: extends the lightweight header with ownership
 * metadata.  Only used when umem_ownership_debug is set.
 */
#define	OWN_STACK_DEPTH	8

typedef struct own_hdr_debug {
	own_hdr_lite_t		ohd_lite;
	thread_t		ohd_owner_thread;
	_Atomic int32_t		ohd_borrow_count;
	_Atomic int32_t		ohd_mut_borrow;
	uintptr_t		ohd_alloc_stack[OWN_STACK_DEPTH];
	int			ohd_stack_depth;
} own_hdr_debug_t;

/* ------------------------------------------------------------------ */
/* Globals                                                            */
/* ------------------------------------------------------------------ */

uint32_t umem_ownership_debug = 0;

static umem_own_error_fn own_error_handler = NULL;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static inline int
is_debug_mode(void)
{
	return (umem_ownership_debug != 0);
}

static inline size_t
header_size(void)
{
	return (is_debug_mode() ?
	    sizeof (own_hdr_debug_t) : sizeof (own_hdr_lite_t));
}

/* Round up to 16-byte alignment for user data */
static inline size_t
aligned_header_size(void)
{
	size_t hs = header_size();
	return ((hs + 15) & ~(size_t)15);
}

static inline own_hdr_lite_t *
to_lite(umem_owned_t *o)
{
	return ((own_hdr_lite_t *)o);
}

static inline const own_hdr_lite_t *
to_lite_c(const umem_owned_t *o)
{
	return ((const own_hdr_lite_t *)o);
}

static inline own_hdr_debug_t *
to_debug(umem_owned_t *o)
{
	return ((own_hdr_debug_t *)o);
}

static inline void *
to_data(umem_owned_t *o)
{
	return ((char *)o + aligned_header_size());
}

static inline const void *
to_data_c(const umem_owned_t *o)
{
	return ((const char *)o + aligned_header_size());
}

static void
report_error(umem_own_error_t err, const umem_owned_t *o,
    const char *msg)
{
	if (own_error_handler != NULL) {
		own_error_handler(err, o, msg);
		return;
	}

	log_message("umem ownership: %s (err=%d handle=%p)\n",
	    msg, (int)err, (const void *)o);

	if (is_debug_mode() && o != NULL) {
		const own_hdr_debug_t *d = (const own_hdr_debug_t *)o;
		log_message("  alloc_size=%zu owner_thread=%lu\n",
		    d->ohd_lite.ohl_alloc_size,
		    (unsigned long)d->ohd_owner_thread);
	}

	if (umem_abort != 0)
		umem_panic("umem ownership abort: %s", msg);
}

static void
capture_stack(own_hdr_debug_t *d)
{
	d->ohd_stack_depth = getpcstack(d->ohd_alloc_stack,
	    OWN_STACK_DEPTH, 0);
}

/* ------------------------------------------------------------------ */
/* State validation                                                   */
/* ------------------------------------------------------------------ */

static int
require_owned(umem_owned_t *o, const char *op)
{
	if (o == NULL) {
		report_error(UMEM_OWN_USE_AFTER_FREE, NULL, op);
		return (-1);
	}

	uint32_t st = atomic_load_explicit(&to_lite(o)->ohl_state,
	    memory_order_acquire);

	if (st == UMEM_OWN_STATE_OWNED)
		return (0);
	if (st == UMEM_OWN_STATE_MOVED)
		report_error(UMEM_OWN_USE_AFTER_MOVE, o, op);
	else
		report_error(UMEM_OWN_USE_AFTER_FREE, o, op);
	return (-1);
}

static int
require_thread(umem_owned_t *o, const char *op)
{
	if (!is_debug_mode())
		return (0);

	own_hdr_debug_t *d = to_debug(o);
	if (d->ohd_owner_thread != thr_self()) {
		report_error(UMEM_OWN_THREAD_VIOLATION, o, op);
		return (-1);
	}
	return (0);
}

/* ------------------------------------------------------------------ */
/* Header initialization                                              */
/* ------------------------------------------------------------------ */

static void
init_header(umem_owned_t *o, size_t user_size)
{
	own_hdr_lite_t *l = to_lite(o);

	atomic_store_explicit(&l->ohl_refcount, 0,
	    memory_order_release);
	atomic_store_explicit(&l->ohl_state, UMEM_OWN_STATE_OWNED,
	    memory_order_release);
	l->ohl_alloc_size = user_size;

	if (is_debug_mode()) {
		own_hdr_debug_t *d = to_debug(o);
		d->ohd_owner_thread = thr_self();
		atomic_store_explicit(&d->ohd_borrow_count, 0,
		    memory_order_release);
		atomic_store_explicit(&d->ohd_mut_borrow, 0,
		    memory_order_release);
		capture_stack(d);
	}
}

/* ------------------------------------------------------------------ */
/* Public API: Core ownership                                         */
/* ------------------------------------------------------------------ */

umem_owned_t *
umem_own_alloc(size_t size, int flags)
{
	size_t total = aligned_header_size() + size;
	void *raw = _umem_alloc(total, flags);

	if (raw == NULL)
		return (NULL);

	umem_owned_t *o = (umem_owned_t *)raw;
	init_header(o, size);
	return (o);
}

void *
umem_deref(umem_owned_t *o)
{
	if (require_owned(o, "deref") != 0)
		return (NULL);
	if (require_thread(o, "deref") != 0)
		return (NULL);
	return (to_data(o));
}

const void *
umem_deref_const(const umem_owned_t *o)
{
	if (o == NULL) {
		report_error(UMEM_OWN_USE_AFTER_FREE, NULL,
		    "deref_const NULL");
		return (NULL);
	}

	uint32_t st = atomic_load_explicit(&to_lite_c(o)->ohl_state,
	    memory_order_acquire);
	if (st != UMEM_OWN_STATE_OWNED) {
		report_error(st == UMEM_OWN_STATE_MOVED ?
		    UMEM_OWN_USE_AFTER_MOVE : UMEM_OWN_USE_AFTER_FREE,
		    o, "deref_const");
		return (NULL);
	}
	return (to_data_c(o));
}

void
umem_drop(umem_owned_t *o)
{
	if (o == NULL)
		return;

	own_hdr_lite_t *l = to_lite(o);
	uint32_t st = atomic_load_explicit(&l->ohl_state,
	    memory_order_acquire);

	if (st == UMEM_OWN_STATE_DROPPED) {
		report_error(UMEM_OWN_DOUBLE_FREE, o, "double drop");
		return;
	}
	if (st == UMEM_OWN_STATE_MOVED) {
		report_error(UMEM_OWN_USE_AFTER_MOVE, o,
		    "drop after move");
		return;
	}
	if (st != UMEM_OWN_STATE_OWNED) {
		report_error(UMEM_OWN_USE_AFTER_FREE, o,
		    "drop corrupted handle");
		return;
	}

	if (require_thread(o, "drop") != 0)
		return;

	uint32_t refs = atomic_load_explicit(&l->ohl_refcount,
	    memory_order_acquire);
	if (refs != 0) {
		report_error(UMEM_OWN_BORROW_CONFLICT, o,
		    "drop with outstanding borrows");
		return;
	}

	if (is_debug_mode()) {
		own_hdr_debug_t *d = to_debug(o);
		int32_t bc = atomic_load_explicit(&d->ohd_borrow_count,
		    memory_order_acquire);
		int32_t mb = atomic_load_explicit(&d->ohd_mut_borrow,
		    memory_order_acquire);
		if (bc != 0 || mb != 0) {
			report_error(UMEM_OWN_BORROW_CONFLICT, o,
			    "drop with active debug borrows");
			return;
		}
	}

	atomic_store_explicit(&l->ohl_state,
	    UMEM_OWN_STATE_DROPPED, memory_order_release);

	size_t total = aligned_header_size() + l->ohl_alloc_size;
	_umem_free(o, total);
}

/* ------------------------------------------------------------------ */
/* Public API: Borrow                                                 */
/* ------------------------------------------------------------------ */

umem_ref_t
umem_borrow(umem_owned_t *o)
{
	umem_ref_t ref = { NULL, NULL, 0 };

	if (require_owned(o, "borrow") != 0)
		return (ref);
	if (require_thread(o, "borrow") != 0)
		return (ref);

	if (is_debug_mode()) {
		own_hdr_debug_t *d = to_debug(o);
		int32_t mb = atomic_load_explicit(&d->ohd_mut_borrow,
		    memory_order_acquire);
		if (mb != 0) {
			report_error(UMEM_OWN_BORROW_CONFLICT, o,
			    "borrow while mut borrow active");
			return (ref);
		}
		atomic_fetch_add_explicit(&d->ohd_borrow_count, 1,
		    memory_order_acq_rel);
	}

	atomic_fetch_add_explicit(&to_lite(o)->ohl_refcount, 1,
	    memory_order_acq_rel);

	ref.ref_owner = o;
	ref.ref_data = to_data(o);
	ref.ref_mutable = 0;
	return (ref);
}

umem_ref_t
umem_borrow_mut(umem_owned_t *o)
{
	umem_ref_t ref = { NULL, NULL, 0 };

	if (require_owned(o, "borrow_mut") != 0)
		return (ref);
	if (require_thread(o, "borrow_mut") != 0)
		return (ref);

	if (is_debug_mode()) {
		own_hdr_debug_t *d = to_debug(o);

		int32_t bc = atomic_load_explicit(&d->ohd_borrow_count,
		    memory_order_acquire);
		if (bc != 0) {
			report_error(UMEM_OWN_BORROW_CONFLICT, o,
			    "mut borrow while shared borrows active");
			return (ref);
		}

		int32_t mb = atomic_load_explicit(&d->ohd_mut_borrow,
		    memory_order_acquire);
		if (mb != 0) {
			report_error(UMEM_OWN_BORROW_CONFLICT, o,
			    "mut borrow while another mut active");
			return (ref);
		}

		atomic_store_explicit(&d->ohd_mut_borrow, 1,
		    memory_order_release);
	}

	atomic_fetch_add_explicit(&to_lite(o)->ohl_refcount, 1,
	    memory_order_acq_rel);

	ref.ref_owner = o;
	ref.ref_data = to_data(o);
	ref.ref_mutable = 1;
	return (ref);
}

void
umem_unborrow(umem_ref_t ref)
{
	if (ref.ref_owner == NULL)
		return;

	umem_owned_t *o = ref.ref_owner;
	own_hdr_lite_t *l = to_lite(o);

	uint32_t prev = atomic_fetch_sub_explicit(&l->ohl_refcount,
	    1, memory_order_acq_rel);
	if (prev == 0) {
		report_error(UMEM_OWN_BORROW_CONFLICT, o,
		    "unborrow underflow");
		atomic_store_explicit(&l->ohl_refcount, 0,
		    memory_order_release);
		return;
	}

	if (is_debug_mode()) {
		own_hdr_debug_t *d = to_debug(o);
		if (ref.ref_mutable) {
			atomic_store_explicit(&d->ohd_mut_borrow, 0,
			    memory_order_release);
		} else {
			atomic_fetch_sub_explicit(&d->ohd_borrow_count,
			    1, memory_order_acq_rel);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Public API: Transfer                                               */
/* ------------------------------------------------------------------ */

umem_owned_t *
umem_move(umem_owned_t *from)
{
	if (require_owned(from, "move") != 0)
		return (NULL);
	if (require_thread(from, "move") != 0)
		return (NULL);

	own_hdr_lite_t *l = to_lite(from);
	uint32_t refs = atomic_load_explicit(&l->ohl_refcount,
	    memory_order_acquire);
	if (refs != 0) {
		report_error(UMEM_OWN_BORROW_CONFLICT, from,
		    "move with outstanding borrows");
		return (NULL);
	}

	/*
	 * Move is a logical transfer: the source handle is returned
	 * with the new owner. State stays OWNED — the caller becomes
	 * the owner. No intermediate state is observable.
	 */

	if (is_debug_mode()) {
		own_hdr_debug_t *d = to_debug(from);
		d->ohd_owner_thread = thr_self();
		atomic_store_explicit(&d->ohd_borrow_count, 0,
		    memory_order_release);
		atomic_store_explicit(&d->ohd_mut_borrow, 0,
		    memory_order_release);
	}

	return (from);
}

umem_owned_t *
umem_clone(const umem_owned_t *src, size_t size)
{
	if (src == NULL) {
		report_error(UMEM_OWN_USE_AFTER_FREE, NULL,
		    "clone from NULL");
		return (NULL);
	}

	uint32_t st = atomic_load_explicit(&to_lite_c(src)->ohl_state,
	    memory_order_acquire);
	if (st != UMEM_OWN_STATE_OWNED) {
		report_error(st == UMEM_OWN_STATE_MOVED ?
		    UMEM_OWN_USE_AFTER_MOVE : UMEM_OWN_USE_AFTER_FREE,
		    src, "clone");
		return (NULL);
	}

	/* Clamp to source allocation size to prevent overread */
	size_t src_size = to_lite_c(src)->ohl_alloc_size;
	size_t copy_size = (size < src_size) ? size : src_size;

	umem_owned_t *dst = umem_own_alloc(size, UMEM_DEFAULT);
	if (dst == NULL)
		return (NULL);

	memcpy(to_data(dst), to_data_c(src), copy_size);
	return (dst);
}

/* ------------------------------------------------------------------ */
/* Public API: Cache-integrated                                       */
/* ------------------------------------------------------------------ */

umem_owned_t *
umem_own_cache_alloc(umem_cache_t *cp, int flags)
{
	void *raw = _umem_cache_alloc(cp, flags);
	if (raw == NULL)
		return (NULL);

	umem_owned_t *o = (umem_owned_t *)raw;
	init_header(o, cp->cache_bufsize);
	return (o);
}

/* ------------------------------------------------------------------ */
/* Public API: Query                                                  */
/* ------------------------------------------------------------------ */

int
umem_own_is_valid(const umem_owned_t *o)
{
	if (o == NULL)
		return (0);

	uint32_t st = atomic_load_explicit(&to_lite_c(o)->ohl_state,
	    memory_order_acquire);
	return (st == UMEM_OWN_STATE_OWNED);
}

uint32_t
umem_own_state(const umem_owned_t *o)
{
	if (o == NULL)
		return (0);

	return (atomic_load_explicit(&to_lite_c(o)->ohl_state,
	    memory_order_acquire));
}

/* ------------------------------------------------------------------ */
/* Public API: Error handler                                          */
/* ------------------------------------------------------------------ */

void
umem_own_set_error_handler(umem_own_error_fn fn)
{
	own_error_handler = fn;
}
