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
 * GDB-style stack trace formatting for libumem error reporting.
 *
 * Two-tier symbol resolution:
 *   1. libdw (best): DWARF debug info for file:line
 *   2. dladdr only (minimum): function+offset, no file:line
 *
 * There used to be a middle tier that forked addr2line(1) via
 * execlp("addr2line", ...).  It is gone, for two independent reasons
 * (P5.1, docs/plans/2026-09-21-production-readiness.md):
 *
 *   1. It could not work.  It passed `-e /proc/self/exe`, which after the
 *      exec names *addr2line itself*, not the traced binary.  Measured on
 *      x86_64 with the identical fork/exec sequence: `-e /proc/self/exe`
 *      yields "?? ??:0" where the real executable path yields
 *      "main at demo.c:38".  The tier existed and resolved nothing.
 *   2. execlp() resolves through PATH, and umem_stacktrace_init() sits on
 *      the unconditional umem_init() path, so a process with an
 *      attacker-influenced PATH -- including a setuid binary *linked*
 *      against libumem, which AT_SECURE does not protect -- ran an
 *      attacker-chosen "addr2line" as the elevated user before main().
 *
 * Attack surface with no working benefit, so it is deleted rather than
 * gated: AGENTS.md 7a forbids PATH-resolving exec in library code outright.
 * test/security/test_no_path_exec.sh asserts no such exec comes back.
 */

#include "config.h"
#include "umem_stacktrace.h"
#include "misc.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#ifndef _WIN32
#include <dlfcn.h>
#include <unistd.h>
#endif

#ifdef HAVE_LIBDW
#include <elfutils/libdwfl.h>
#endif

/*
 * Resolver tier enumeration.  Value 1 is retired: it was the addr2line
 * fallback.  The numbering is left alone so a tier value recorded anywhere
 * still means what it always meant.
 */
enum {
	RESOLVE_DLADDR = 0,
	RESOLVE_LIBDW = 2
};

static int resolver_tier = RESOLVE_DLADDR;

#ifdef HAVE_LIBDW
static Dwfl *dwfl_handle;
static const Dwfl_Callbacks dwfl_callbacks = {
	.find_elf = dwfl_linux_proc_find_elf,
	.find_debuginfo = dwfl_standard_find_debuginfo,
	.section_address = dwfl_offline_section_address,
};

static int
init_libdw(void)
{
	dwfl_handle = dwfl_begin(&dwfl_callbacks);
	if (dwfl_handle == NULL)
		return (-1);

	if (dwfl_linux_proc_report(dwfl_handle, getpid()) != 0) {
		dwfl_end(dwfl_handle);
		dwfl_handle = NULL;
		return (-1);
	}

	if (dwfl_report_end(dwfl_handle, NULL, NULL) != 0) {
		dwfl_end(dwfl_handle);
		dwfl_handle = NULL;
		return (-1);
	}

	return (0);
}

static void
resolve_libdw(uintptr_t pc, const char **func, const char **file,
    int *line)
{
	if (dwfl_handle == NULL)
		return;

	Dwfl_Module *mod = dwfl_addrmodule(dwfl_handle, (Dwarf_Addr)pc);
	if (mod == NULL)
		return;

	const char *name = dwfl_module_addrname(mod, (Dwarf_Addr)pc);
	if (name != NULL)
		*func = name;

	Dwfl_Line *dwline = dwfl_module_getsrc(mod, (Dwarf_Addr)pc);
	if (dwline != NULL) {
		int lineno = 0;
		const char *src = dwfl_lineinfo(dwline, NULL, &lineno,
		    NULL, NULL, NULL);
		if (src != NULL) {
			*file = src;
			*line = lineno;
		}
	}
}
#endif /* HAVE_LIBDW */

/*
 * Pre-warmer for backtrace(3) -- see getpcstack.c.  We expose this
 * flag globally so getpcstack can refuse to call backtrace() until
 * we've safely loaded libgcc_s outside any allocator-intercept path.
 */
int umem_backtrace_warmed = 0;

/*
 * Set to 1 by libumem_malloc.so's constructor when it installs malloc
 * interposers.  In that mode backtrace() in the slab path recurses
 * through libgcc_s's dlopen, so we refuse to warm.  Definition is in
 * libumem.so so libumem_malloc.so can override it from its earlier
 * (priority 101) constructor.
 */
int umem_malloc_is_interposing = 0;

static void
backtrace_warm(void)
{
#ifndef _WIN32
#if defined(HAVE_BACKTRACE)
	extern int backtrace(void **, int);
	void *frames[2];
	(void) backtrace(frames, 2);
	umem_backtrace_warmed = 1;
#endif
#endif
}

/*
 * No constructor.  Pre-warming backtrace(3) at load time is unsafe
 * because libumem.so's constructor may run before libumem_malloc.so
 * has had a chance to register itself, and the libgcc_s dlopen path
 * inside backtrace() may then recurse through umem_alloc.  Instead,
 * the warming is done lazily from umem_stacktrace_init(), which
 * callers (umem_findleaks etc.) invoke on the request path.
 */

int
umem_stacktrace_init(void)
{
	resolver_tier = RESOLVE_DLADDR;

#ifdef _WIN32
	return (resolver_tier);
#else

	/*
	 * Warm backtrace() on the first call.  Skip if libumem_malloc
	 * is interposing -- recursing through libgcc_s's dlopen would
	 * land back in umem_alloc.
	 */
	if (!umem_backtrace_warmed && !umem_malloc_is_interposing)
		backtrace_warm();

#ifdef HAVE_LIBDW
	if (init_libdw() == 0) {
		resolver_tier = RESOLVE_LIBDW;
		return (resolver_tier);
	}
#endif

	return (resolver_tier);
#endif /* _WIN32 */
}

void
umem_stacktrace_format(uintptr_t pc, int frame_num, char *buf,
    size_t bufsz)
{
	const char *func = "??";
	const char *file = "??";
	int line = 0;
	uintptr_t offset = 0;

#ifndef _WIN32
	Dl_info info;
	if (dladdr((void *)pc, &info) && info.dli_sname != NULL) {
		func = info.dli_sname;
		offset = pc - (uintptr_t)info.dli_saddr;
	}

	switch (resolver_tier) {
#ifdef HAVE_LIBDW
	case RESOLVE_LIBDW:
		resolve_libdw(pc, &func, &file, &line);
		break;
#endif
	default:
		break;
	}
#endif /* !_WIN32 */

	if (line > 0) {
		(void) snprintf(buf, bufsz,
		    "  #%d  0x%lx in %s+0x%lx () at %s:%d",
		    frame_num, (unsigned long)pc,
		    func, (unsigned long)offset, file, line);
	} else if (strcmp(file, "??") != 0) {
		(void) snprintf(buf, bufsz,
		    "  #%d  0x%lx in %s+0x%lx () at %s",
		    frame_num, (unsigned long)pc,
		    func, (unsigned long)offset, file);
	} else {
		(void) snprintf(buf, bufsz,
		    "  #%d  0x%lx in %s+0x%lx ()",
		    frame_num, (unsigned long)pc,
		    func, (unsigned long)offset);
	}
}

void
umem_stacktrace_print(uintptr_t *pcs, int depth, const char *header)
{
	char buf[512];

	if (depth <= 0)
		return;

	if (header != NULL)
		umem_printf("%s\n", header);

	for (int i = 0; i < depth; i++) {
		umem_stacktrace_format(pcs[i], i, buf, sizeof(buf));
		umem_printf("%s\n", buf);
	}
}
