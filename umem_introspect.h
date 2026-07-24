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
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

#ifndef _UMEM_INTROSPECT_H
#define	_UMEM_INTROSPECT_H

/*
 * In-process introspection control channel (the umemctl server side).
 *
 * OPT-IN and ZERO-COST WHEN DISABLED: nothing here touches the alloc/free
 * hot path unless UMEM_OPTIONS=introspect=1 is set at init AND a break
 * predicate is armed via the control channel. When the whole feature is
 * compiled out (no --enable-introspect / no UMEM_INTROSPECT define) every
 * symbol below is an empty inline and the hot-path break hook vanishes
 * entirely -- the disassembly of _umem_alloc/_umem_free is byte-identical to
 * a build without this file.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct umem_cache;

#ifdef UMEM_INTROSPECT

/*
 * Global break arm flag. This is the ONLY hot-path touch point. It is 0
 * unless a break predicate is armed via the control channel, so the check
 * the allocator inlines is a single load + predict-not-taken branch that
 * never fires in production, and it is only ever emitted when
 * --enable-introspect is set.
 */
extern volatile int umem_introspect_break_armed;

/* Set by envvar.c option parsing (introspect=1). */
extern int umem_introspect_enabled;

/* Started lazily from umem_init() only when introspect=1 was parsed. */
void umem_introspect_start(void);

/*
 * Hot-path hook: called just before _umem_alloc returns a buffer, ONLY when
 * umem_introspect_break_armed != 0. Evaluates armed predicates and, on match,
 * spin-waits on a condvar until a client sends "continue".
 */
void umem_introspect_break_check(void *buf, size_t size,
    struct umem_cache *cp);

#else /* !UMEM_INTROSPECT */

#define	umem_introspect_break_armed 0
static inline void umem_introspect_start(void) {}

#endif /* UMEM_INTROSPECT */

#ifdef __cplusplus
}
#endif

#endif /* _UMEM_INTROSPECT_H */
