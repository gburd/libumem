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
 * Copyright 2014 Garrett D'Amore <garrett@damore.org>
 * Copyright 2010 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */
/*
 * Copyright (c) 2012, Joyent, Inc.  All rights reserved.
 */

/*
 * Stubs for the standalone to reduce the dependence on external libraries
 */

#include "config.h"
#include <string.h>
#ifdef __sun
#include <thread.h>
#else
#include <pthread.h>
#endif

#include "misc.h"

/*ARGSUSED*/
int
_cond_init(cond_t *cvp, int type, void *arg)
{
	return (0);
}

/*ARGSUSED*/
int
_cond_destroy(cond_t *cvp)
{
	return (0);
}

/*ARGSUSED*/
int
_cond_wait(cond_t *cv, mutex_t *mutex)
{
	umem_panic("attempt to wait on standumem cv %p", cv);

	/*NOTREACHED*/
	return (0);
}

/*ARGSUSED*/
int
_cond_broadcast(cond_t *cvp)
{
	return (0);
}

/*ARGSUSED*/
int
pthread_setcancelstate(int state, int *oldstate)
{
	return (0);
}

thread_t
_thr_self(void)
{
	return ((thread_t)1);
}

static mutex_t _mp = DEFAULTMUTEX;

/*ARGSUSED*/
int
__mutex_init(mutex_t *mp, int type, void *arg)
{
	(void) memcpy(mp, &_mp, sizeof (mutex_t));
	return (0);
}

/*ARGSUSED*/
int
__mutex_destroy(mutex_t *mp)
{
	return (0);
}

/*ARGSUSED*/
int
__mutex_held(mutex_t *mp)
{
	return (1);
}

/*ARGSUSED*/
int
__mutex_lock(mutex_t *mp)
{
	return (0);
}

/*ARGSUSED*/
int
__mutex_trylock(mutex_t *mp)
{
	return (0);
}

/*ARGSUSED*/
int
__mutex_unlock(mutex_t *mp)
{
	return (0);
}

/*
 * issetugid: only provide the stub where libc does NOT (the standalone/kernel
 * build).  When HAVE_ISSETUGID is set (illumos and modern Linux both have the
 * real one), this hardcoded return 1 would shadow libc's and make libumem
 * treat every process as tainted -- which, among other things, silently
 * suppresses umem_output error text to stderr (umem_error_enter gates the
 * fd-2 write on !issetugid()).  Found via test_freelist_mangle on illumos:
 * the corruption WAS detected (alloc returned NULL) but nothing was reported.
 */
#if !defined(HAVE_ISSETUGID) || !HAVE_ISSETUGID
int
issetugid(void)
{
	return (1);
}
#endif

int
isspace(int c)
{
	switch (c) {
	case ' ':
	case '\t':
	case '\n':
	case '\r':
	case '\f':
	case '\v':
		return (1);
	}
	return (0);
}
