/*
 * test/debugger/test_event_cb.c -- T1 of docs/reviews/2026-09-26-tool-review.md.
 *
 * umem_inspect.h and umem_inspect(3) advertise a C event API: register a
 * callback with umem_inspect_set_event_cb(), turn events on with
 * umem_inspect_enable_events(1), and "every alloc, free, and
 * detected-corruption event" reaches it.  The gdb/lldb `umem break alloc|free`
 * commands break on umem_event_alloc/umem_event_free, which are reached only
 * from the same fan-out, umem_inspect_notify().
 *
 * PRE-FIX BEHAVIOUR: umem_inspect_notify() has no caller anywhere in the
 * allocator, so the callback never runs.  This test allocates and frees
 * through every public entry point (umem_alloc small / >64 KiB / oversize,
 * umem_zalloc, umem_cache_alloc, malloc is out of scope: it is a separate
 * library) and fails if the callback saw no ALLOC or no FREE event.
 *
 * It passes once the hooks are wired, or once the API is withdrawn and this
 * test with it -- the coordinator's call (review T1).
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "umem.h"
#include "umem_inspect.h"

static atomic_uint hits[UMEM_EV__COUNT];

static void
cb(umem_event_t ev, const umem_buffer_info_t *info, void *arg)
{
	(void) info;
	(void) arg;
	if ((unsigned)ev < UMEM_EV__COUNT)
		atomic_fetch_add(&hits[ev], 1);
}

int
main(void)
{
	static const size_t sizes[] = { 16, 64, 512, 4096, 70000, 98304,
	    262144 };
	umem_cache_t *cp;
	int i;
	size_t k;

	umem_inspect_set_event_cb(cb, NULL, ~0u);
	umem_inspect_enable_events(1);

	cp = umem_cache_create("evcb", 100, 0, NULL, NULL, NULL, NULL, NULL,
	    0);
	for (i = 0; i < 200; i++) {
		for (k = 0; k < sizeof (sizes) / sizeof (sizes[0]); k++) {
			void *p = umem_alloc(sizes[k], UMEM_NOFAIL);
			umem_free(p, sizes[k]);
		}
		void *z = umem_zalloc(777, UMEM_NOFAIL);
		umem_free(z, 777);
		void *c = umem_cache_alloc(cp, UMEM_NOFAIL);
		umem_cache_free(cp, c);
	}

	printf("event callback hits: alloc=%u free=%u (%d allocations made)\n",
	    atomic_load(&hits[UMEM_EV_ALLOC]),
	    atomic_load(&hits[UMEM_EV_FREE]),
	    200 * (int)(sizeof (sizes) / sizeof (sizes[0]) + 2));

	if (atomic_load(&hits[UMEM_EV_ALLOC]) == 0 ||
	    atomic_load(&hits[UMEM_EV_FREE]) == 0) {
		printf("FAIL: T1 umem_inspect_set_event_cb() callback never ran "
		    "for alloc/free with events enabled\n");
		return (1);
	}
	printf("PASS\n");
	return (0);
}
