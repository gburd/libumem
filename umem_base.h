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
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */
/*
 * Copyright (c) 2012, Joyent, Inc.  All rights reserved.
 */

#ifndef	_UMEM_BASE_H
#define	_UMEM_BASE_H

/* #pragma ident	"@(#)umem_base.h	1.4	05/06/08 SMI" */

#include <umem_impl.h>

#ifdef	__cplusplus
extern "C" {
#endif

#include "misc.h"

extern size_t pagesize;
#undef PAGESIZE
#define	PAGESIZE	pagesize

/*
 * umem.c: non-tunables
 */
extern vmem_t		*umem_memalign_arena;
extern vmem_t		*umem_oversize_arena;

extern int umem_ready;

/*
 * Debug hooks (review T1/T8).  umem_hooks is 0 unless a debugger or tool
 * asked for events (UMEM_HOOK_EVENTS, umem_inspect_enable_events) or armed
 * a umemctl break (UMEM_HOOK_BREAK, umem_introspect.c).  Whoever changes it
 * then calls umem_hooks_sync() (umem.c), which diverts traffic off the fast
 * paths so the fast paths never test or call anything new:
 *  - umem_ptc_bin_table becomes all -1: _umem_alloc/_umem_free skip the PTC
 *    (and the rseq layer it feeds); their bin < 0 arm alone tests
 *    umem_hooks and diverts to a cold path that reports the REQUEST size.
 *    bin >= 0 implies hooks off, so the PTC and magazine paths test nothing.
 *  - UMF_HOOKED goes into cc_flags of every non-internal cache, next to
 *    UMF_BUFTAG in the mask the magazine/rseq pop already tests, so
 *    umem_cache_alloc/free report from every layer.
 * Internal (UMC_INTERNAL, UMC_QCACHE) caches never report, so umem_alloc()
 * through umem_alloc_<n> reports once.  Objects already in a thread's PTC
 * bins or PTC magazines stay there, untouched, while hooks are on: not
 * issued, not leaked (thread exit or hooks-off returns them to use).
 */
#define	UMEM_HOOK_EVENTS	0x1
#define	UMEM_HOOK_BREAK		0x2
extern int umem_hooks __attribute__((visibility("hidden")));
#define	UMEM_HOOKS_ON()	\
	__builtin_expect(__atomic_load_n(&umem_hooks, __ATOMIC_RELAXED) != 0, 0)
struct umem_cache;
/* Returns buf, so a caller can tail-call it. */
__attribute__((visibility("hidden"))) void *umem_hook_alloc(void *buf,
    size_t size, struct umem_cache *cp);
__attribute__((visibility("hidden"))) void umem_hook_free(void *buf,
    size_t size, struct umem_cache *cp);
__attribute__((visibility("hidden"))) void umem_hooks_sync(void);
extern thread_t umem_init_thr;		/* the thread doing the init */

extern int umem_init(void);		/* do umem's initialization */
/* #pragma rarely_called(umem_init) */

extern umem_log_header_t *umem_transaction_log;
extern umem_log_header_t *umem_content_log;
extern umem_log_header_t *umem_failure_log;
extern umem_log_header_t *umem_slab_log;

extern mutex_t umem_init_lock;

extern mutex_t umem_cache_lock;
extern umem_cache_t umem_null_cache;

extern mutex_t umem_flags_lock;

extern mutex_t umem_update_lock;
extern cond_t umem_update_cv;
extern volatile thread_t umem_st_update_thr;
extern thread_t umem_update_thr;
extern struct timeval	umem_update_next;

extern volatile hrtime_t umem_reap_next;
extern volatile uint32_t umem_reaping;
#define	UMEM_REAP_DONE		0x00000000	/* inactive */
#define	UMEM_REAP_ADDING	0x00000001	/* umem_reap() is active */
#define	UMEM_REAP_ACTIVE	0x00000002	/* update thread is reaping */

/*
 * umem.c: tunables
 */
extern uint32_t umem_max_ncpus;
extern umem_cpu_t *umem_cpus;

extern uint32_t umem_stack_depth;
extern uint32_t umem_reap_interval;
extern uint32_t umem_update_interval;
extern uint32_t umem_depot_contention;
extern uint32_t umem_magazine_tuning;
extern uint32_t umem_abort;
extern uint32_t umem_output;
extern uint32_t umem_logging;
extern uint32_t umem_mtbf;
extern size_t umem_transaction_log_size;
extern size_t umem_content_log_size;
extern size_t umem_failure_log_size;
extern size_t umem_slab_log_size;
extern size_t umem_content_maxsave;
extern size_t umem_lite_minsize;
extern size_t umem_lite_maxalign;
extern size_t umem_maxverify;
extern size_t umem_minfirewall;
extern size_t umem_ptc_size;

extern uint32_t umem_reclaim_enabled;
extern uint32_t umem_reclaim_delay;


extern uint32_t umem_flags;

/*
 * P8.5b: UMEM_OPTIONS=norseq sets this to keep the restartable-sequence
 * per-CPU fast path off (umem_rseq_init() then leaves umem_rseq_enabled = 0
 * and every allocation falls back to the cc_lock/depot path).  It is a
 * uint_t because it is wired as an ITEM_FLAG target in envvar.c.  Disabling
 * is the safe direction, so the option is honoured even under AT_SECURE.
 */
extern uint_t umem_rseq_disabled;

/*
 * umem.c: Internal aliases (to avoid PLTs)
 */
extern void *_umem_alloc(size_t size, int umflags);
extern void *_umem_zalloc(size_t size, int umflags);
extern void _umem_free(void *buf, size_t size);

extern void *_umem_cache_alloc(umem_cache_t *cache, int flags);
extern void _umem_cache_free(umem_cache_t *cache, void *buffer);
extern int umem_cache_alloc_batch(umem_cache_t *cp, void **bufs, int count,
    int umflag);
extern int umem_cache_free_batch(umem_cache_t *cp, void **bufs, int count);

/*
 * umem.c: private interfaces
 */
extern void umem_type_init(caddr_t, size_t, size_t);
extern int umem_get_max_ncpus(void);
extern void umem_process_updates(void);
extern void umem_cache_applyall(void (*)(umem_cache_t *));
extern void umem_cache_update(umem_cache_t *);

extern void umem_alloc_sizes_add(size_t);
extern void umem_alloc_sizes_clear(void);
extern void umem_alloc_sizes_remove(size_t);

/*
 * umem_fork.c: private interfaces
 */
extern void umem_forkhandler_init(void);

/*
 * umem_update_thread.c
 */
extern int umem_create_update_thread(void);

/*
 * envvar.c:
 */
void umem_setup_envvars(int);
void umem_process_envvars(void);
extern char umem_profile_spec[512];

/*
 * malloc.c: traditional malloc/free interface
 */
extern void *umem_malloc(size_t);
extern void umem_malloc_free(void *);
extern void *_malloc(size_t);
extern void _free(void *);

#ifdef	__cplusplus
}
#endif

#endif	/* _UMEM_BASE_H */
