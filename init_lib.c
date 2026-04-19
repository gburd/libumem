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

/* #pragma ident	"@(#)init_lib.c	1.2	05/06/08 SMI" */

/*
 * Initialization routines for the library version of libumem.
 */

#include "config.h"
#include "umem_base.h"
#include "vmem_base.h"

#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#if HAVE_DLFCN_H
#include <dlfcn.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif
#include <fcntl.h>
#include <string.h>

#ifdef __FreeBSD__
#include <machine/param.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

#if defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

void
vmem_heap_init(void)
{
#ifdef _WIN32
	vmem_backend = VMEM_BACKEND_MMAP;
	(void) vmem_sbrk_arena(NULL, NULL);
#else
# if defined(sun)
	void *handle = dlopen("libmapmalloc.so.1", RTLD_NOLOAD);

	if (handle != NULL) {
		log_message("sbrk backend disabled\n");
		vmem_backend = VMEM_BACKEND_MMAP;
	}
# else
	if (vmem_backend == 0) {
		/* prefer mmap, as sbrk() seems to have problems wither working
		 * with other allocators or has some Solaris specific assumptions. */
		vmem_backend = VMEM_BACKEND_MMAP;
	}
# endif

	if ((vmem_backend & VMEM_BACKEND_MMAP) != 0) {
		vmem_backend = VMEM_BACKEND_MMAP;
		(void) vmem_mmap_arena(NULL, NULL);
	} else {
		vmem_backend = VMEM_BACKEND_SBRK;
		(void) vmem_sbrk_arena(NULL, NULL);
	}
#endif
}

/*ARGSUSED*/
void
umem_type_init(caddr_t start __attribute__((unused)),
    size_t len __attribute__((unused)),
    size_t pgsize __attribute__((unused)))
{
#ifdef _WIN32
	SYSTEM_INFO info;
	GetSystemInfo(&info);
	pagesize = info.dwPageSize;
#elif !defined(__FreeBSD__)
	pagesize = _sysconf(_SC_PAGESIZE);
#else
	pagesize = PAGE_SIZE;
#endif
}

int
umem_get_max_ncpus(void)
{
#ifdef linux
  /*
   * On Linux, sysconf(_SC_NPROCESSORS_ONLN) reads /proc/stat via
   * glibc, which calls malloc internally. Since umem_get_max_ncpus
   * is called during umem_init before the allocator is fully ready,
   * we read /proc/stat directly into a static buffer to avoid
   * recursive malloc.
   */
  static int ncpus = 0;

  if (ncpus == 0) {
    char proc_stat[8192];
    int fd;

    ncpus = 1;
    fd = open("/proc/stat", O_RDONLY);
    if (fd >= 0) {
      const ssize_t n = read(fd, proc_stat, sizeof(proc_stat) - 1);
      if (n >= 0) {
        const char *cur;
        const char *next;

        proc_stat[n] = '\0';
        cur = proc_stat;
        while (*cur && (next = strstr(cur + 3, "cpu"))) {
          cur = next;
        }

        if (*cur)
          ncpus = atoi(cur + 3) + 1;
      }

      close(fd);
    }
  }

  return ncpus;

#elif defined(_WIN32)

  SYSTEM_INFO info;
  GetSystemInfo(&info);
  return info.dwNumberOfProcessors;

#else /* !linux, !_WIN32 */

  /*
   * Non-Linux POSIX: sysconf is safe here because vmem is already
   * initialized by the time umem_get_max_ncpus is called, so any
   * malloc from sysconf's internals can be satisfied.
   */
#ifdef _SC_NPROCESSORS_ONLN
  {
    long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpus > 0)
      return ((int)(2 * ncpus));
  }
#endif

#ifdef _SC_NPROCESSORS_CONF
  {
    long ncpus = sysconf(_SC_NPROCESSORS_CONF);
    if (ncpus > 0)
      return ((int)(2 * ncpus));
  }
#endif

#if defined(__FreeBSD__) || defined(__APPLE__) || defined(__NetBSD__) || \
    defined(__OpenBSD__)
  {
    int mib[2] = { CTL_HW, HW_NCPU };
    int ncpus = 0;
    size_t len = sizeof (ncpus);
    if (sysctl(mib, 2, &ncpus, &len, NULL, 0) == 0 && ncpus > 0)
      return (2 * ncpus);
  }
#endif

  return (1);

#endif /* linux */
}
