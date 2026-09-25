/*
 * P5.8 regression: free() must refuse a pointer with a forged libumem header
 * WITHOUT mutating anything first.
 *
 * THE DEFECT (pre-fix, malloc.c:387 process_free())
 *   For any non-bootstrap pointer, process_free() did
 *
 *	buf = (malloc_data_t *)buf_arg; buf--;
 *	size = buf->malloc_size;
 *	switch (UMEM_MALLOC_DECODE(buf->malloc_stat, size)) { ... }
 *
 *   Three problems, in order of severity:
 *
 *   1. AN 8-BYTE READ BEFORE AN ARBITRARY ADDRESS.  Nothing established that
 *      buf_arg was ours before its header was read.  Under LD_PRELOAD that
 *      happens for real: a library with its own allocator, or an application
 *      bug, hands free() a pointer we never issued.
 *
 *   2. THE MAGIC IS FORGEABLE.  UMEM_MALLOC_DECODE (umem_impl.h) is
 *      `stat + size` and MALLOC_MAGIC is the fixed constant 0x3a10c000, so
 *      anyone who can write 8 bytes in front of an address can produce a
 *      header that passes.  There is no secret and no per-process salt.
 *
 *   3. THE ERROR PATH MUTATED STATE BEFORE VALIDATING.  Every
 *      successful-magic branch stored UMEM_FREE_PATTERN_32 into
 *      buf->malloc_stat before the size was ever sanity-checked, and the
 *      MALLOC_OVERSIZE/MEMALIGN branches wrote one tag's stat word *before*
 *      validating the other tag.  And because the interposer sets
 *      umem_abort = 0 (malloc_interpose.c, umem_interpose_init(), "log and
 *      continue"), a forged header that passed then proceeded into
 *      _umem_free()/vmem_xfree() on an address libumem does not own.  glibc
 *      aborts on a bad free; this continued into silent corruption.
 *
 * THE FIX (malloc.c)
 *   process_free() now (a) refuses to read the header at all unless it lies
 *   inside umem-owned address space, (b) re-checks ownership before reading a
 *   second tag, (c) validates the decoded size against the layout the magic
 *   named and checks [base, base+size) is umem-owned, and only then (d)
 *   writes anything.
 *
 * WHAT THIS ASSERTS
 *   A. A forged MALLOC_MAGIC header in a STACK buffer is refused, and the
 *      forged bytes are UNCHANGED afterwards -- proving no write happened
 *      (item 3).  Pre-fix, malloc_stat came back as UMEM_FREE_PATTERN_32.
 *   B. Same for a forged header in a .data buffer, and for MEMALIGN_MAGIC.
 *   C. A forged header with an absurd size is refused (item 2's other half:
 *      even at a plausible address a nonsense size must not be acted on).
 *   D. The process is still alive and its allocator still works afterwards --
 *      the refusal must not be a crash, and must not have corrupted state.
 *   E. CONTROL: ordinary malloc/free round-trips still work, so A-D are not
 *      passing because free() rejects everything.
 *
 * This links libumem directly and calls process_free() through the public
 * free() of the interposer, i.e. exactly the path an LD_PRELOAD'd process
 * takes.
 *
 * PRE-FIX DEMONSTRATION: against v3.0.0, arm A reports
 * "MUTATED: malloc_stat 0x3a10c000 -> 0xfeedface" (UMEM_FREE_PATTERN_32) and
 * the test fails.  Whether it then also crashes in _umem_free() depends on the
 * forged size; the mutation is the deterministic part, which is why it is what
 * is asserted.
 *
 * Exit: 0 pass, 1 fail.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <malloc.h>

#include "umem_impl.h"

static int failures;

/*
 * Under ASan, free() is intercepted by ASan before it reaches the interposer,
 * and a forged stack/static pointer is rejected there -- so the arms that hand
 * one to free() cannot reach process_free() at all.  Skip them rather than
 * claim a result the run did not produce.
 */
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

static void
pass(const char *what)
{
	printf("  PASS: %s\n", what);
}

static void
fail(const char *fmt, ...)
{
	va_list va;

	printf("  FAIL: ");
	va_start(va, fmt);
	vprintf(fmt, va);
	va_end(va);
	printf("\n");
	failures++;
}

/*
 * The header libumem puts in front of every malloc() result.  Duplicated from
 * malloc.c on purpose: this test forges one, so it must not depend on a
 * definition malloc.c could change without the forgery noticing.
 */
typedef struct {
	uint32_t malloc_size;
	uint32_t malloc_stat;
} forged_header_t;

/*
 * Lay a forged header in front of `payload` and hand the payload to free().
 * Returns 1 if the header's stat word was MUTATED by the call.
 *
 * ASAN NOTE.  Under --enable-asan, ASan interposes free() ahead of the
 * interposer and rejects a non-malloc()-ed pointer itself
 * ("attempting free on address which was not malloc()-ed"), aborting before
 * libumem's process_free() is ever entered.  That is ASan doing its job, but it
 * means the forgery never reaches the code under test, so these arms are
 * skipped in an ASan build rather than reported.  The non-ASan run is the one
 * that exercises process_free(); see asan_active() and main().
 */
static int
forge_and_free(void *region, size_t region_size, uint32_t size_field,
    uint32_t magic, const char *what)
{
	forged_header_t *hdr;
	void *payload;
	uint32_t stat_before, stat_after;
	uint32_t size_before, size_after;

	/* 16-byte-align the payload, as a real malloc result would be. */
	hdr = (forged_header_t *)(((uintptr_t)region + 15) & ~(uintptr_t)15);
	payload = (void *)(hdr + 1);
	if ((uintptr_t)payload + 64 >
	    (uintptr_t)region + region_size) {
		fail("%s: test bug, region too small", what);
		return (0);
	}

	hdr->malloc_size = size_field;
	/* UMEM_MALLOC_ENCODE: the whole point is that this is computable. */
	hdr->malloc_stat = UMEM_MALLOC_ENCODE(magic, size_field);
	memset(payload, 0x5A, 32);

	stat_before = hdr->malloc_stat;
	size_before = hdr->malloc_size;

	free(payload);

	stat_after = hdr->malloc_stat;
	size_after = hdr->malloc_size;

	if (stat_after != stat_before || size_after != size_before) {
		fail("%s: MUTATED the forged header before validating it "
		    "(stat 0x%08x -> 0x%08x, size %u -> %u)", what,
		    stat_before, stat_after, size_before, size_after);
		return (1);
	}
	pass(what);
	return (0);
}

/* A forged header in writable static storage (not the stack, not the heap). */
static char data_region[512] __attribute__((aligned(16)));

int
main(void)
{
	char stack_region[512] __attribute__((aligned(16)));
	void *p;
	int i;

	printf("P5.8: free() must refuse a forged header without mutating it\n");
	printf("  (UMEM_MALLOC_DECODE is stat+size and MALLOC_MAGIC is the "
	    "fixed constant 0x%x -- forging one is arithmetic)\n",
	    (unsigned)MALLOC_MAGIC);

	/*
	 * Make sure the allocator is up and has spans, so the ownership check
	 * is answering from a populated heap rather than an empty one.  An
	 * empty heap would refuse everything and make every arm vacuous -- arm
	 * E is the guard against that.
	 */
	p = malloc(64);
	if (p == NULL) {
		printf("  FAIL: malloc(64) returned NULL; cannot test\n");
		return (1);
	}
	free(p);

	if (asan_active()) {
		/*
		 * Arms A-C hand a forged stack/static pointer to free().  ASan
		 * intercepts that and aborts before libumem sees it, so those
		 * arms cannot test process_free() here.  Arms E and F use only
		 * real allocations and still mean something, so run those and
		 * report the rest honestly as not covered by this build.
		 */
		printf("[A-C] SKIPPED under ASan: ASan interposes free() ahead "
		    "of libumem and rejects a non-malloc()-ed pointer itself, "
		    "so the forgery never reaches process_free().  The non-ASan "
		    "run covers these.\n");
		goto controls;
	}

	/* ---------------------------------------------- A: stack forgery */
	printf("[A] forged MALLOC_MAGIC header on the stack\n");
	(void) forge_and_free(stack_region, sizeof (stack_region),
	    (uint32_t)(32 + sizeof (forged_header_t)), MALLOC_MAGIC,
	    "stack MALLOC_MAGIC forgery refused, header untouched");

	/* ---------------------------------------------- B: .data forgery */
	printf("[B] forged headers in static storage\n");
	(void) forge_and_free(data_region, sizeof (data_region),
	    (uint32_t)(32 + sizeof (forged_header_t)), MALLOC_MAGIC,
	    "static MALLOC_MAGIC forgery refused, header untouched");
	(void) forge_and_free(data_region, sizeof (data_region),
	    (uint32_t)(32 + 2 * sizeof (forged_header_t)), MEMALIGN_MAGIC,
	    "static MEMALIGN_MAGIC forgery refused, header untouched");
#ifdef _LP64
	(void) forge_and_free(data_region, sizeof (data_region),
	    (uint32_t)(32 + 2 * sizeof (forged_header_t)),
	    MALLOC_SECOND_MAGIC,
	    "static MALLOC_SECOND_MAGIC forgery refused, header untouched");
#endif

	/* ---------------------------------------------- C: absurd sizes */
	printf("[C] forged header with a nonsense size\n");
	(void) forge_and_free(stack_region, sizeof (stack_region),
	    0u, MALLOC_MAGIC,
	    "size 0 refused (data_size would underflow), header untouched");
	(void) forge_and_free(stack_region, sizeof (stack_region),
	    0xFFFFFFF0u, MALLOC_MAGIC,
	    "size ~4GB refused, header untouched");

controls:
	/* ---------------------------------------------- D: still healthy */
	printf("[D] the allocator survived and still works\n");
	for (i = 0; i < 64; i++) {
		void *q = malloc(64 + i * 8);
		if (q == NULL) {
			fail("malloc failed after the refusals (i=%d)", i);
			break;
		}
		memset(q, 0xC3, 64 + i * 8);
		free(q);
	}
	if (i == 64)
		pass("64 malloc/free round-trips after the refusals");

	/* ---------------------------------------------- E: vacuity guard */
	printf("[E] control: real allocations are still recognized as ours\n");
	{
		void *ptrs[32];
		int n = 0, recognized = 0;

		for (n = 0; n < 32; n++) {
			ptrs[n] = malloc(16 << (n % 8));
			if (ptrs[n] == NULL)
				break;
		}
		/*
		 * malloc_usable_size() runs the SAME classifier free() does
		 * (interpose_owner_of -> process_free(ptr, 0, &size)) and
		 * returns 0 when the pointer is not recognized -- without
		 * mutating or freeing anything.  So it reports directly whether
		 * the new ownership check accepts umem's own memory.  If it did
		 * not, every arm above would be passing for the wrong reason.
		 */
		for (i = 0; i < n; i++) {
			if (malloc_usable_size(ptrs[i]) >= (size_t)(16 << (i % 8)))
				recognized++;
		}
		for (i = 0; i < n; i++)
			free(ptrs[i]);

		if (n == 0) {
			fail("could not allocate anything; arms A-D are vacuous");
		} else if (recognized == n) {
			printf("  PASS: all %d real allocations report their "
			    "usable size (accepted by the ownership check)\n", n);
		} else {
			fail("only %d of %d real allocations were recognized -- "
			    "the ownership check rejects umem's own memory",
			    recognized, n);
		}
	}

	/*
	 * And a foreign-but-plausible pointer must still be refused rather than
	 * accepted just because it sits near the heap: a pointer one page into a
	 * real allocation, with no header of its own.
	 *
	 * Skipped under ASan: ASan intercepts malloc_usable_size() and aborts on
	 * an interior pointer ("bad-malloc_usable_size") before libumem's
	 * classifier runs, so the arm cannot reach the code it is about.
	 */
	printf("[F] a pointer with no header is not accepted\n");
	if (asan_active()) {
		printf("  (skipped under ASan: ASan rejects an interior pointer "
		    "in malloc_usable_size() itself)\n");
	} else {
		void *big = malloc(64 * 1024);

		if (big == NULL) {
			printf("  (skipped: 64K allocation failed)\n");
		} else {
			/* Deep inside a live allocation: owned memory, but not a
			 * header libumem ever wrote. */
			void *interior = (char *)big + 4096;
			if (malloc_usable_size(interior) == 0)
				pass("an interior pointer is not accepted as an "
				    "allocation");
			else
				fail("an interior pointer was accepted "
				    "(usable_size %zu)",
				    malloc_usable_size(interior));
			free(big);
		}
	}

	printf("\n");
	if (failures != 0) {
		printf("test_forged_free: FAIL (%d)\n", failures);
		return (1);
	}
	printf("test_forged_free: PASS\n");
	return (0);
}
