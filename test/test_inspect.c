/*
 * Smoke test: deliberately leak some buffers, then run findleaks and
 * log_dump in-process.  Expected to report the leaks with the right
 * counts/sizes.
 *
 * Run with:
 *   UMEM_DEBUG=audit UMEM_LOGGING=transaction=1m \
 *     LD_LIBRARY_PATH=.libs ./test_inspect
 */
#include "umem.h"
#include "umem_inspect.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void __attribute__((noinline))
leaker_A(void)
{
	for (int i = 0; i < 7; i++)
		(void) umem_alloc(64, UMEM_DEFAULT);
}

static void __attribute__((noinline))
leaker_B(void)
{
	for (int i = 0; i < 3; i++)
		(void) umem_alloc(4096, UMEM_DEFAULT);
}

int
main(void)
{
	/* Churn so the transaction log has some non-leak entries. */
	for (int i = 0; i < 50; i++) {
		void *p = umem_alloc(128, UMEM_DEFAULT);
		umem_free(p, 128);
	}

	leaker_A();
	leaker_B();
	leaker_A();

	fprintf(stderr, "\n=== umem_status_dump(text) ===\n");
	umem_status_dump(stderr, UMEM_FMT_TEXT);

	fprintf(stderr, "\n=== umem_findleaks(text) ===\n");
	size_t n = umem_findleaks(stderr, UMEM_FMT_TEXT, 20);
	fprintf(stderr, "\n[visited %zu live buffers]\n", n);

	fprintf(stderr, "\n=== umem_findleaks(json) ===\n");
	umem_findleaks(stderr, UMEM_FMT_JSON, 5);

	fprintf(stderr, "\n=== umem_log_dump(text, 10) ===\n");
	umem_log_dump(stderr, UMEM_FMT_TEXT, 10);

	fprintf(stderr, "\n=== umem_whatis ===\n");
	void *p = umem_alloc(512, UMEM_DEFAULT);
	umem_buffer_info_t info;
	if (umem_whatis(p, &info) == 0) {
		fprintf(stderr, "  whatis(%p) => cache=%s size=%zu state=%d\n",
		    p, info.cache_name, info.size, info.state);
	} else {
		fprintf(stderr, "  whatis(%p) failed\n", p);
	}
	umem_bufctl_audit_dump(stderr, p);
	umem_free(p, 512);

	return (0);
}
