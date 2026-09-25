/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * CDDL HEADER END
 */

/* Helper for test_reap_interval_zero.sh: allocate once, sleep 2 s, print
 * this process's total CPU seconds (user+sys, all threads). */
#include "umem.h"
#include <stdio.h>
#include <unistd.h>
#include <sys/resource.h>

int
main(void)
{
	struct rusage ru;
	void *p = umem_alloc(64, UMEM_DEFAULT);

	if (p == NULL)
		return (1);
	umem_free(p, 64);
	sleep(2);
	if (getrusage(RUSAGE_SELF, &ru) != 0)
		return (1);
	printf("%.3f\n",
	    ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
	    ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6);
	return (0);
}
