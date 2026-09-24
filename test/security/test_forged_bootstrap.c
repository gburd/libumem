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

/*
 * P5.10: free() must not unmap a range named by the caller's own bytes.
 *
 * THE DEFECT.  Step 1 of process_free()'s validation order classifies the
 * pointer as a bootstrap allocation by reading the 8 bytes BEFORE it and
 * comparing them with BOOTSTRAP_MAGIC -- before step 2 has established that
 * the pointer is inside umem's address space at all.  If they match, the
 * pointer goes to bootstrap_free(), which does munmap(hdr, hdr->size) with
 * both the address and the length taken from that same memory.  So a caller
 * who controls the 16 bytes in front of a pointer -- attacker position D:
 * controls buffer contents, not the environment -- can make free() unmap a
 * range of their choosing.  Not a read; an unmap.
 *
 * P5.8 moved every other pre-ownership read behind the hull check and left
 * this one in front, on the argument that a bootstrap mmap can land inside the
 * hull's gaps and would then be misdecoded as a malloc_data_t.  That argument
 * is right for the case where a bootstrap pointer CAN exist.  In the steady
 * state of nearly every process none does (bootstrap allocations are made
 * only before umem is READY or inside a recursive malloc), and the fix gates
 * the read on a live count of them: zero live means the read is skipped.
 *
 * WHAT THIS TESTS.  Map a page; write BOOTSTRAP_MAGIC and a size equal to the
 * mapping into its first 16 bytes; call free() on the address 16 bytes in.
 * Then touch the page.
 *
 *   PASS: the page is still mapped (free() refused a pointer it does not own).
 *   FAIL: SIGSEGV -- free() unmapped the caller's page on the caller's say-so.
 *
 * The touch runs in a child so a FAIL is a reported verdict, not a crash of
 * the test.  The control arm proves the plumbing: a REAL bootstrap-shaped
 * allocation is not available from outside the library, so the control is
 * that a real malloc()/free() pair still works after the forgery attempt
 * (the allocator was not left in a bad state by refusing).
 *
 * ASan intercepts free() ahead of the interposer and rejects the pointer
 * itself, so under --enable-asan this test cannot reach process_free: SKIP.
 *
 * Pre-fix (bootstrap magic read unconditionally): the child dies with
 * SIGSEGV.  Post-fix (802c6ac reverted / 6842a35): the page survives.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define	BOOTSTRAP_MAGIC	0xB007B007B007B007ULL

static int
asan_active(void)
{
#if defined(__SANITIZE_ADDRESS__)
	return (1);
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
	return (1);
#else
	return (0);
#endif
#else
	return (0);
#endif
}

int
main(void)
{
	long pg = sysconf(_SC_PAGESIZE);
	pid_t pid;
	int status;

	if (asan_active()) {
		printf("SKIP: ASan intercepts free() ahead of the interposer\n");
		return (77);
	}

	/* Bring the allocator up so bootstrap_live has reached its steady state. */
	{
		void *w = malloc(64);
		if (w == NULL)
			return (77);
		free(w);
	}

	pid = fork();
	if (pid < 0)
		return (77);
	if (pid == 0) {
		char *page = mmap(NULL, (size_t)pg, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		volatile char sink;

		if (page == MAP_FAILED)
			_exit(77);

		/* The forgery: a bootstrap header naming this very mapping. */
		((uint64_t *)page)[0] = BOOTSTRAP_MAGIC;
		((size_t *)page)[1] = (size_t)pg;
		memset(page + 16, 0x5a, 64);

		free(page + 16);

		/*
		 * If free() believed the header, the page is gone and this
		 * faults.  If it refused, this reads back 0x5a.
		 */
		sink = page[16];
		(void) sink;

		/* Control: the allocator still works after refusing. */
		{
			void *p = malloc(128);
			if (p == NULL)
				_exit(2);
			free(p);
		}
		_exit(page[16] == 0x5a ? 0 : 3);
	}

	if (waitpid(pid, &status, 0) != pid)
		return (77);

	if (WIFSIGNALED(status)) {
		printf("RESULT: FAIL (child died with signal %d: free() "
		    "unmapped a page because the caller wrote BOOTSTRAP_MAGIC "
		    "in front of it -- the bootstrap classification read runs "
		    "before the ownership check and acts on what it finds)\n",
		    WTERMSIG(status));
		return (1);
	}
	switch (WEXITSTATUS(status)) {
	case 0:
		printf("RESULT: PASS (forged bootstrap header refused; the page "
		    "is still mapped and the allocator still works)\n");
		return (0);
	case 77:
		printf("SKIP: mmap failed in child\n");
		return (77);
	case 2:
		printf("RESULT: FAIL (allocator broken after the refusal: "
		    "malloc returned NULL)\n");
		return (1);
	default:
		printf("RESULT: FAIL (page contents changed: %d)\n",
		    WEXITSTATUS(status));
		return (1);
	}
}
