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
 *
 * Portions Copyright 2006-2008 Message Systems, Inc. All rights reserved.
 */

/* #pragma ident	"@(#)misc.c	1.6	05/06/08 SMI" */

#define _BUILDING_UMEM_MISC_C
#include "config.h"
/* #include "mtlib.h" */
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#if HAVE_DLFCN_H
#include <dlfcn.h>
#endif
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#if HAVE_SYS_AUXV_H
#include <sys/auxv.h>
#endif

#if HAVE_SYS_MACHELF_H
#include <sys/machelf.h>
#endif

#include <umem_impl.h>
#include "misc.h"

#define	UMEM_ERRFD	2	/* goes to standard error */
#define	UMEM_MAX_ERROR_SIZE 4096 /* error messages are truncated to this */

/*
 * This is a circular buffer for holding error messages.
 * umem_error_enter appends to the buffer, adding "..." to the beginning
 * if data has been lost.
 */

#define	ERR_SIZE 8192		/* must be a power of 2 */

static mutex_t umem_error_lock = DEFAULTMUTEX;
/* Log lines dropped because the ring was busy (see umem_log_enter). */
volatile unsigned long umem_error_dropped;

static char umem_error_buffer[ERR_SIZE] = "";
static uint_t umem_error_begin = 0;
static uint_t umem_error_end = 0;

#define WRITE_IGNORE(fd, buf, len) \
	do { \
		ssize_t __attribute__((unused)) _ret; \
		_ret = write((fd), (buf), (len)); \
	} while (0)

#define	WRITE_AND_INC(var, value) { \
	umem_error_buffer[(var)++] = (value); \
	var = P2PHASE((var), ERR_SIZE); \
}

static void
umem_log_enter(const char *error_str)
{
	int looped;
	char c;

	looped = 0;

	/*
	 * TRYLOCK, NOT LOCK.  This ring is diagnostic: it records error text
	 * for a debugger or umemctl to read later.  It is reached from
	 * umem_err_recoverable() on every refused free(), and under LD_PRELOAD
	 * with umem_abort = 0 that is a steady-state path.  A signal handler
	 * that frees a bad pointer while the interrupted thread is inside this
	 * function used to deadlock on umem_error_lock -- glibc's
	 * malloc_printerr writes and aborts without a lock and cannot
	 * (production-readiness review 2026-09-24, 4.6).  Losing one log line
	 * under exactly that race is the right trade; the line that IS lost is
	 * counted so a reader knows the ring is incomplete.
	 */
	if (mutex_trylock(&umem_error_lock) != 0) {
		(void) __atomic_add_fetch(&umem_error_dropped, 1,
		    __ATOMIC_RELAXED);
		return;
	}

	while ((c = *error_str++) != '\0') {
		WRITE_AND_INC(umem_error_end, c);
		if (umem_error_end == umem_error_begin)
			looped = 1;
	}

	umem_error_buffer[umem_error_end] = 0;

	if (looped) {
		uint_t idx;
		umem_error_begin = P2PHASE(umem_error_end + 1, ERR_SIZE);

		idx = umem_error_begin;
		WRITE_AND_INC(idx, '.');
		WRITE_AND_INC(idx, '.');
		WRITE_AND_INC(idx, '.');
	}

	(void) mutex_unlock(&umem_error_lock);
}

void
umem_error_enter(const char *error_str)
{
#ifndef UMEM_STANDALONE
	if (umem_output && !issetugid())
		WRITE_IGNORE(UMEM_ERRFD, error_str, strlen(error_str));
#endif

	umem_log_enter(error_str);
}

/*
 * umem_secure_mode() -- "this process must not be steered by its environment".
 *
 * True when the process is running with elevated or otherwise
 * caller-unverifiable privilege:
 *
 *   issetugid()          set-uid/set-gid, or the ids changed since exec.
 *   getauxval(AT_SECURE)  the loader's own "secure execution required" bit,
 *                         which also covers file capabilities, MAC transitions,
 *                         and AT_SECURE binaries that are not setuid.
 *
 * Neither subsumes the other, so both are consulted.  On platforms without
 * getauxval() only issetugid() is used (sol_compat.h supplies an
 * issetugid() for platforms lacking that too).
 *
 * The result is cached: it cannot change for the life of the process (a later
 * setuid() does not un-taint an exec), and umem_init() consults it before any
 * allocator machinery is up, so it must not allocate or lock.
 *
 * WHAT THIS GATES: every UMEM_* option with a file, socket, or exec side
 * effect (see envvar.c:umem_env_secure_filter).  Pure tuning options keep
 * working -- the goal is no side effects, not a crippled allocator.
 *
 * umem_secure_mode_force is TEST-ONLY.  It is deliberately NOT settable from
 * the environment: making it so would hand an attacker the ability to turn
 * the gate OFF, which is the whole exposure this closes.  Only in-process
 * code (the regression in test/security/) can set it.
 */
int umem_secure_mode_force = -1;	/* test-only: <0 = not forced */

int
umem_secure_mode(void)
{
	static int cached = -1;

	if (umem_secure_mode_force >= 0)
		return (umem_secure_mode_force != 0);

	if (cached >= 0)
		return (cached);

	cached = 0;
#ifndef UMEM_STANDALONE
	if (issetugid())
		cached = 1;
#if HAVE_GETAUXVAL && defined(AT_SECURE)
	else if (getauxval(AT_SECURE) != 0)
		cached = 1;
#endif
#endif
	return (cached);
}

/*
 * umem_open_write() -- the ONLY way library code creates a file whose path
 * came from outside (UMEM_OPTIONS=profile=record:/path, a snapshot path from
 * a debugger or the control channel).  P5.3.
 *
 * Before this existed, the writers were
 *   open(path, O_WRONLY|O_CREAT|O_TRUNC, 0644)   (umem_profile.c)
 *   fopen(path, "wb") / fopen(path, "w")         (umem_inspect.c)
 * with no O_EXCL and no O_NOFOLLOW anywhere in the library, so a symlink at
 * `path` truncated whatever it pointed at, as the target's uid.
 *
 * WHAT IS CHECKED, and why in this order:
 *
 *   O_NOFOLLOW     the final component must not be a symlink.  This is the
 *                  symlink-truncation hole itself.
 *   no O_TRUNC     truncation happens AFTER the checks below, via
 *                  ftruncate() on the fd.  Passing O_TRUNC here would
 *                  destroy a hardlinked victim before we looked at it.
 *   S_ISREG        no fifos (open() would have blocked), no devices.
 *   st_nlink == 1  a second link means someone else also names this inode,
 *                  which is the hardlink version of the same attack.
 *   st_uid == euid we must own it.  geteuid(), NOT getuid(): for a setuid
 *                  target the real uid is the unprivileged invoker
 *                  (AGENTS.md 7a).
 *
 * Every check is on the returned fd, so there is no window between checking
 * and acting -- unlike stat()-then-open().
 *
 * NOT closed by this: a symlink in a LEADING directory component of an
 * attacker-writable path.  Refusing that needs openat() walking each
 * component, and the residual risk is bounded by the caller choosing the
 * path.  In secure mode (setugid/AT_SECURE) these writers are unreachable
 * anyway: the options that carry a path are filtered in envvar.c before
 * parsing.
 *
 * Mode is 0600, not the old 0644: a profile or snapshot contains heap
 * addresses and cache names.
 *
 * Returns an fd, or -1 with errno set.
 */
int
umem_open_write(const char *path)
{
	struct stat st;
	int fd, oerrno;

	fd = open(path, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return (-1);

	if (fstat(fd, &st) != 0)
		goto reject;

	if (!S_ISREG(st.st_mode) || st.st_nlink != 1 ||
	    st.st_uid != geteuid()) {
		errno = EPERM;
		goto reject;
	}

	if (ftruncate(fd, 0) != 0)
		goto reject;

	return (fd);

reject:
	oerrno = errno;
	(void) close(fd);
	errno = oerrno;
	return (-1);
}

int
highbit(ulong_t i)
{
	register int h = 1;

	if (i == 0)
		return (0);
#ifdef _LP64
	if (i & 0xffffffff00000000ul) {
		h += 32; i >>= 32;
	}
#endif
	if (i & 0xffff0000) {
		h += 16; i >>= 16;
	}
	if (i & 0xff00) {
		h += 8; i >>= 8;
	}
	if (i & 0xf0) {
		h += 4; i >>= 4;
	}
	if (i & 0xc) {
		h += 2; i >>= 2;
	}
	if (i & 0x2) {
		h += 1;
	}
	return (h);
}

int
lowbit(ulong_t i)
{
	register int h = 1;

	if (i == 0)
		return (0);
#ifdef _LP64
	if (!(i & 0xffffffff)) {
		h += 32; i >>= 32;
	}
#endif
	if (!(i & 0xffff)) {
		h += 16; i >>= 16;
	}
	if (!(i & 0xff)) {
		h += 8; i >>= 8;
	}
	if (!(i & 0xf)) {
		h += 4; i >>= 4;
	}
	if (!(i & 0x3)) {
		h += 2; i >>= 2;
	}
	if (!(i & 0x1)) {
		h += 1;
	}
	return (h);
}

void
hrt2ts(hrtime_t hrt, timestruc_t *tsp)
{
	tsp->tv_sec = hrt / NANOSEC;
	tsp->tv_nsec = hrt % NANOSEC;
}

void
log_message(const char *format, ...)
{
	char buf[UMEM_MAX_ERROR_SIZE] = "";

	va_list va;

	va_start(va, format);
	(void) vsnprintf(buf, UMEM_MAX_ERROR_SIZE-1, format, va);
	va_end(va);

#ifndef UMEM_STANDALONE
	if (umem_output > 1)
		WRITE_IGNORE(UMEM_ERRFD, buf, strlen(buf));
#endif

	umem_log_enter(buf);
}

#ifndef UMEM_STANDALONE
void
debug_printf(const char *format, ...)
{
	char buf[UMEM_MAX_ERROR_SIZE] = "";

	va_list va;

	va_start(va, format);
	(void) vsnprintf(buf, UMEM_MAX_ERROR_SIZE-1, format, va);
	va_end(va);

	WRITE_IGNORE(UMEM_ERRFD, buf, strlen(buf));
}
#endif

void
umem_vprintf(const char *format, va_list va)
{
	char buf[UMEM_MAX_ERROR_SIZE] = "";

	(void) vsnprintf(buf, UMEM_MAX_ERROR_SIZE-1, format, va);

	umem_error_enter(buf);
}

void
umem_printf(const char *format, ...)
{
	va_list va;

	va_start(va, format);
	umem_vprintf(format, va);
	va_end(va);
}

/*ARGSUSED*/
void
umem_printf_warn(void *ignored __attribute__((unused)), const char *format, ...)
{
	va_list va;

	va_start(va, format);
	umem_vprintf(format, va);
	va_end(va);
}

/*
 * print_sym tries to print out the symbol and offset of a pointer
 */
int
print_sym(void *pointer)
{
#if HAVE_SYS_MACHELF_H
	int result;
	Dl_info sym_info;

	uintptr_t end = NULL;

	Sym *ext_info = NULL;

	result = dladdr1(pointer, &sym_info, (void **)&ext_info,
	    RTLD_DL_SYMENT);

	if (result != 0) {
		const char *endpath;

		end = (uintptr_t)sym_info.dli_saddr + ext_info->st_size;

		endpath = strrchr(sym_info.dli_fname, '/');
		if (endpath)
			endpath++;
		else
			endpath = sym_info.dli_fname;
		umem_printf("%s'", endpath);
	}

	if (result == 0 || (uintptr_t)pointer > end) {
		umem_printf("?? (0x%p)", pointer);
		return (0);
	} else {
		umem_printf("%s+0x%p", sym_info.dli_sname,
		    (char *)pointer - (char *)sym_info.dli_saddr);
		return (1);
	}
#else
	umem_printf("?? (0x%p)", pointer);
	return 0;
#endif
}
