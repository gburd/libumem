/*
 * fuzz_malloc.c -- libFuzzer/AFL harness over the public malloc API.
 *
 * Drives the SHIPPED interposer surface -- malloc/free/realloc/calloc/
 * posix_memalign -- the way a real program under
 * LD_PRELOAD=libumem_malloc.so would. We deliberately link against nothing
 * internal: the harness calls the standard libc names, so the symbols it
 * exercises are whichever allocator is loaded (libumem_malloc.so when
 * preloaded, the system allocator otherwise). That keeps the fuzzer honest
 * about the actual interposed boundary and portable across libFuzzer and AFL.
 *
 * The fuzzer input is read as a little opcode stream: each step picks an
 * operation, a live-slot index, a size, and (for aligned allocation) an
 * alignment, so the corpus explores random sizes, alignments, and
 * alloc/free/realloc interleavings against a bounded live set.
 *
 * Entry points:
 *   LLVMFuzzerTestOneInput  -- libFuzzer and AFL (afl-clang-fast) persistent.
 *   main                    -- standalone replay of files named on argv, so
 *                              CI can run it without a fuzzing driver linked.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Bounded live set: large enough to force reuse and churn, small enough to
 * keep each input cheap. */
#define	NSLOTS		64
/* Clamp sizes so a hostile input cannot ask for a terabyte and OOM the box;
 * the allocator paths we care about are all exercised well under this. */
#define	MAX_SIZE	(1u << 16)

struct slot {
	void *p;
	size_t sz;	/* usable size we requested; for the memset touch */
};

/*
 * Run one allocation program described by the byte stream. Shared by the
 * libFuzzer entry point and the standalone replay main().
 */
static void
run(const uint8_t *data, size_t size)
{
	struct slot slots[NSLOTS];
	size_t i;

	memset(slots, 0, sizeof(slots));

	/* Each operation consumes 4 bytes: op, slot, size-lo, size-hi. */
	for (i = 0; i + 4 <= size; i += 4) {
		unsigned op = data[i] % 5u;
		unsigned idx = data[i + 1] % NSLOTS;
		size_t req = ((size_t)data[i + 2] |
		    ((size_t)data[i + 3] << 8)) % MAX_SIZE;
		struct slot *s = &slots[idx];

		switch (op) {
		case 0:	/* malloc */
			if (s->p == NULL) {
				s->p = malloc(req);
				s->sz = (s->p != NULL) ? req : 0;
				if (s->p != NULL && req != 0)
					memset(s->p, 0xA5, req);
			}
			break;
		case 1:	/* free */
			free(s->p);
			s->p = NULL;
			s->sz = 0;
			break;
		case 2:	/* realloc (grow/shrink/move) */
			s->p = realloc(s->p, req);
			if (s->p != NULL) {
				s->sz = req;
				if (req != 0)
					memset(s->p, 0x5A, req);
			} else {
				s->sz = 0;
			}
			break;
		case 3:	/* calloc */
			if (s->p == NULL) {
				/* Split req into count*elt so overflow and
				 * zero-fill paths get exercised. */
				size_t elt = (data[i + 2] & 0x1f) + 1;
				size_t cnt = (req / elt) + 1;
				s->p = calloc(cnt, elt);
				s->sz = (s->p != NULL) ? cnt * elt : 0;
			}
			break;
		case 4:	/* posix_memalign */
			if (s->p == NULL) {
				/* Alignment a power of two in [sizeof(void*),
				 * 4096]; req as the size. */
				size_t align = (size_t)1
				    << (3 + (data[i + 1] % 10));
				void *q = NULL;
				if (posix_memalign(&q, align, req) == 0) {
					s->p = q;
					s->sz = req;
					if (req != 0)
						memset(s->p, 0x33, req);
				}
			}
			break;
		}
	}

	/* Drain the live set so a leak-detecting build (ASan/LSan) sees a
	 * clean exit for a well-formed program. */
	for (i = 0; i < NSLOTS; i++)
		free(slots[i].p);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	run(data, size);
	return (0);
}

#ifndef	FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
/*
 * Standalone replay driver. Built when no libFuzzer runtime is linked
 * (the default autotools build): reads each file named on argv and runs it
 * once, so `make check` can exercise the harness over a seed corpus without
 * a fuzzing engine. With no args it reads stdin, so AFL's
 * `afl-fuzz -- ./fuzz_malloc` persistent/stdin modes still work.
 */
int
main(int argc, char **argv)
{
	int i;

	if (argc < 2) {
		static uint8_t buf[1 << 20];
		size_t n = fread(buf, 1, sizeof(buf), stdin);
		run(buf, n);
		return (0);
	}

	for (i = 1; i < argc; i++) {
		FILE *f = fopen(argv[i], "rb");
		static uint8_t buf[1 << 20];
		size_t n;

		if (f == NULL) {
			perror(argv[i]);
			continue;
		}
		n = fread(buf, 1, sizeof(buf), f);
		fclose(f);
		run(buf, n);
	}
	return (0);
}
#endif	/* !FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION */
