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
 * Two modes:
 *   Lightweight (always on, ~2% overhead): state + refcount tracking.
 *   Full debug (UMEM_OPTIONS=ownership, ~15% overhead): thread checks,
 *   borrow tracking, stack capture on alloc.
 *
 * Inspired by Rust's ownership model, adapted for C.
 */

#ifndef _UMEM_OWN_H
#define	_UMEM_OWN_H

#include <sys/types.h>
#include <umem.h>
#include <stdint.h>

#ifdef	__cplusplus
extern "C" {
#endif

/* Ownership states */
#define	UMEM_OWN_STATE_OWNED	1
#define	UMEM_OWN_STATE_MOVED	2
#define	UMEM_OWN_STATE_DROPPED	3

/* Error types for violation callbacks */
typedef enum umem_own_error {
	UMEM_OWN_USE_AFTER_FREE = 1,
	UMEM_OWN_USE_AFTER_MOVE,
	UMEM_OWN_DOUBLE_FREE,
	UMEM_OWN_BORROW_CONFLICT,
	UMEM_OWN_THREAD_VIOLATION,
	UMEM_OWN_LEAK
} umem_own_error_t;

/* Opaque handle to an owned allocation */
typedef struct umem_owned umem_owned_t;

/* Borrow reference (value type, returned by copy) */
typedef struct umem_ref {
	umem_owned_t	*ref_owner;
	void		*ref_data;
	int		ref_mutable;
} umem_ref_t;

/* Core ownership API */
umem_owned_t	*umem_own_alloc(size_t size, int flags);
void		 umem_drop(umem_owned_t *owned);
void		*umem_deref(umem_owned_t *owned);
const void	*umem_deref_const(const umem_owned_t *owned);

/* Borrow API (full debug mode; lightweight mode returns valid refs
 * but skips conflict checks) */
umem_ref_t	 umem_borrow(umem_owned_t *owned);
umem_ref_t	 umem_borrow_mut(umem_owned_t *owned);
void		 umem_unborrow(umem_ref_t ref);

/* Transfer */
umem_owned_t	*umem_move(umem_owned_t *from);
umem_owned_t	*umem_clone(const umem_owned_t *src, size_t size);

/* Cache-integrated */
umem_owned_t	*umem_own_cache_alloc(umem_cache_t *cp, int flags);

/* Query */
int		 umem_own_is_valid(const umem_owned_t *owned);
uint32_t	 umem_own_state(const umem_owned_t *owned);

/* Configuration (called during umem init from envvar.c) */
extern uint32_t	 umem_ownership_debug;

/* Error callback (optional; default prints to stderr) */
typedef void (*umem_own_error_fn)(umem_own_error_t err,
    const umem_owned_t *owned, const char *msg);
void		 umem_own_set_error_handler(umem_own_error_fn fn);

#ifdef	__cplusplus
}
#endif

#endif	/* _UMEM_OWN_H */
