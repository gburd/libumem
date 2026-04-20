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
 * Three-tier symbol resolution:
 *   1. libdw (best): DWARF debug info for file:line
 *   2. addr2line (fallback): fork addr2line for file:line
 *   3. dladdr only (minimum): function+offset, no file:line
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
#include <sys/wait.h>
#endif

#ifdef HAVE_LIBDW
#include <elfutils/libdwfl.h>
#endif

/* Resolver tier enumeration */
enum {
	RESOLVE_DLADDR = 0,
	RESOLVE_ADDR2LINE = 1,
	RESOLVE_LIBDW = 2
};

static int resolver_tier = RESOLVE_DLADDR;

/* addr2line result cache: simple direct-mapped hash table */
#define A2L_CACHE_SIZE 256
#define A2L_CACHE_MASK (A2L_CACHE_SIZE - 1)

struct a2l_entry {
	uintptr_t pc;
	char func[128];
	char file[256];
	int line;
	int valid;
};

static struct a2l_entry a2l_cache[A2L_CACHE_SIZE];

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

#ifndef _WIN32
static int
check_addr2line(void)
{
	/*
	 * Verify addr2line is available and /proc/self/exe is readable.
	 * We only check once during init.
	 */
	if (access("/proc/self/exe", R_OK) != 0)
		return (0);

	int pipefd[2];
	if (pipe(pipefd) != 0)
		return (0);

	pid_t pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return (0);
	}

	if (pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		execlp("addr2line", "addr2line", "--version",
		    (char *)NULL);
		_exit(127);
	}

	close(pipefd[1]);
	char tmp[64];
	ssize_t nr = read(pipefd[0], tmp, sizeof(tmp));
	(void) nr;
	close(pipefd[0]);

	int status;
	waitpid(pid, &status, 0);

	return (WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void
resolve_addr2line(uintptr_t pc, const char **func, const char **file,
    int *line)
{
	unsigned int idx = ((unsigned int)(pc >> 2)) & A2L_CACHE_MASK;
	struct a2l_entry *ent = &a2l_cache[idx];

	if (ent->valid && ent->pc == pc) {
		if (ent->func[0] != '\0')
			*func = ent->func;
		if (ent->file[0] != '\0')
			*file = ent->file;
		*line = ent->line;
		return;
	}

	char addr_str[32];
	(void) snprintf(addr_str, sizeof(addr_str), "0x%lx",
	    (unsigned long)pc);

	int pipefd[2];
	if (pipe(pipefd) != 0)
		return;

	pid_t pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}

	if (pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[1]);
		execlp("addr2line", "addr2line",
		    "-e", "/proc/self/exe", "-f", "-p",
		    addr_str, (char *)NULL);
		_exit(127);
	}

	close(pipefd[1]);

	char output[512];
	ssize_t total = 0;
	ssize_t n;
	while (total < (ssize_t)(sizeof(output) - 1)) {
		n = read(pipefd[0], output + total,
		    sizeof(output) - 1 - (size_t)total);
		if (n <= 0)
			break;
		total += n;
	}
	output[total] = '\0';
	close(pipefd[0]);

	int status;
	waitpid(pid, &status, 0);

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return;

	/*
	 * addr2line -f -p output format:
	 *   "function at file:line"
	 * or "?? at ??:0" for unknown
	 */
	ent->pc = pc;
	ent->valid = 1;
	ent->func[0] = '\0';
	ent->file[0] = '\0';
	ent->line = 0;

	char *at = strstr(output, " at ");
	if (at != NULL) {
		size_t flen = (size_t)(at - output);
		if (flen >= sizeof(ent->func))
			flen = sizeof(ent->func) - 1;
		(void) memcpy(ent->func, output, flen);
		ent->func[flen] = '\0';

		char *loc = at + 4;
		char *colon = strrchr(loc, ':');
		if (colon != NULL) {
			size_t plen = (size_t)(colon - loc);
			if (plen >= sizeof(ent->file))
				plen = sizeof(ent->file) - 1;
			(void) memcpy(ent->file, loc, plen);
			ent->file[plen] = '\0';

			/* Strip trailing newline from line number */
			ent->line = (int)strtol(colon + 1, NULL, 10);
		}
	}

	/* Skip "??" results */
	if (strcmp(ent->func, "??") == 0)
		ent->func[0] = '\0';
	if (strcmp(ent->file, "??") == 0)
		ent->file[0] = '\0';

	if (ent->func[0] != '\0')
		*func = ent->func;
	if (ent->file[0] != '\0')
		*file = ent->file;
	*line = ent->line;
}
#endif /* !_WIN32 */

int
umem_stacktrace_init(void)
{
	resolver_tier = RESOLVE_DLADDR;

#ifdef _WIN32
	return (resolver_tier);
#else

#ifdef HAVE_LIBDW
	if (init_libdw() == 0) {
		resolver_tier = RESOLVE_LIBDW;
		return (resolver_tier);
	}
#endif

	if (check_addr2line()) {
		resolver_tier = RESOLVE_ADDR2LINE;
	}

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
	case RESOLVE_ADDR2LINE:
		resolve_addr2line(pc, &func, &file, &line);
		break;
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
