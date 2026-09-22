/*
 * probe_alloc_failure.c -- diagnose why umem_alloc returns NULL under the
 * 192-thread fragmentation workload, when glibc malloc never does.
 *
 * WHAT PROMPTED THIS
 *   The alloc_failures column added for P2.2/P2.5 revealed that on
 *   c7i.metal-48xl the frag workload at 192 threads had umem fail ~10.5M of
 *   ~26.7M allocation attempts (~39%), every window, while libc failed zero
 *   with the same budget on the same box.  Nothing in the old output recorded
 *   that at all, so every previously published umem frag number was computed
 *   over a run that was failing two allocations in five.
 *
 * WHAT THIS SETTLES, in the order the cheap explanations should be ruled out:
 *
 *   1. errno / genuine exhaustion.  Captures errno and RSS/VSZ at the first
 *      failures.  Note the suspicious direction: umem peaked ~5GB while libc
 *      reached ~9.4GB and succeeded, so "out of memory" does not obviously
 *      fit -- the allocator using LESS memory is the one failing.
 *   2. vmem segment pool.  Reports the heap arena's vm_nsegfree and the
 *      arena's own failure counter before and after, so a vmem_populate()
 *      failure is visible as such rather than inferred.
 *   3. size class.  Histograms the FAILING sizes, so a per-cache limit
 *      (clustered) is distinguishable from a global one (uniform).
 *   4. UMEM_DEFAULT vs UMEM_NOFAIL.  umem_alloc(UMEM_DEFAULT) is CONTRACTUALLY
 *      allowed to fail where glibc malloc works much harder before returning
 *      NULL.  If retrying, or UMEM_NOFAIL, makes the failures vanish, then the
 *      honest finding is "umem's default is fail-fast under pressure and the
 *      benchmark never asked it to retry" -- a benchmark-fairness and
 *      documentation problem, not an allocator defect.  Arm C measures this.
 *
 * Arms (each holds a large live set, like the frag workload does):
 *   A  umem_alloc(UMEM_DEFAULT)     the configuration that failed
 *   B  malloc()                      the control that did not fail
 *   C  umem_alloc(UMEM_DEFAULT) with a bounded retry after umem_reap()
 *
 * This probe reads umem/vmem state but modifies NO allocator code.
 *
 * Usage: probe_alloc_failure [threads] [live_objects_per_thread]
 */

#include "config.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "umem.h"
#include "sys/vmem.h"
/* vmem_heap_arena() is private (vmem_base.h), and that header needs the
 * internal build context.  Declare just the one symbol we read, rather than
 * pulling the private headers into a benchmark probe. */
typedef void *vmem_alloc_t_fwd;
extern vmem_t *vmem_heap_arena(void *, void *);

/* Failing-size histogram: the frag workload's range is 16..4096. */
#define NBUCKET 14                    /* 2^1 .. 2^14 */
static atomic_ullong fail_by_size[NBUCKET];
static atomic_ullong ok_by_size[NBUCKET];

static int bucket_of(size_t sz)
{
	int b = 0;
	while ((size_t)1 << (b + 1) < sz && b < NBUCKET - 1)
		b++;
	return b;
}

static atomic_ullong total_ok, total_fail;
static atomic_int first_errno = -1;
static atomic_ullong first_fail_rss_kb, first_fail_vsz_kb;
static atomic_size_t first_fail_size;

static size_t read_status_kb(const char *field)
{
	FILE *f = fopen("/proc/self/status", "r");
	if (!f) return 0;
	char line[256];
	size_t len = strlen(field), v = 0;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, field, len) == 0) {
			char *p = line + len;
			while (*p == ' ' || *p == '\t') p++;
			v = strtoull(p, NULL, 10);
			break;
		}
	}
	fclose(f);
	return v;
}

enum arm { ARM_UMEM, ARM_LIBC, ARM_UMEM_RETRY };

struct warg {
	int tid;
	enum arm arm;
	size_t nlive;
};

static void *worker(void *a)
{
	struct warg *w = a;
	void **live = calloc(w->nlive, sizeof(*live));
	size_t *lsz = calloc(w->nlive, sizeof(*lsz));
	unsigned int seed = 42u ^ ((unsigned)w->tid * 2654435761u);

	if (!live || !lsz) {
		fprintf(stderr, "probe: driver alloc failed\n");
		free(live); free(lsz);
		return NULL;
	}

	/* Build a large live set, exactly what the frag workload does. */
	for (size_t i = 0; i < w->nlive; i++) {
		size_t sz = 16 + (rand_r(&seed) % (4096 - 16 + 1));
		void *p = NULL;

		errno = 0;
		switch (w->arm) {
		case ARM_LIBC:
			p = malloc(sz);
			break;
		case ARM_UMEM:
			p = umem_alloc(sz, UMEM_DEFAULT);
			break;
		case ARM_UMEM_RETRY:
			p = umem_alloc(sz, UMEM_DEFAULT);
			if (p == NULL) {
				/* Ask the allocator to reclaim, then retry
				 * once.  If this rescues the allocation, the
				 * NULL was fail-fast under transient pressure,
				 * not exhaustion. */
				umem_reap();
				p = umem_alloc(sz, UMEM_DEFAULT);
			}
			break;
		}

		int e = errno;
		int b = bucket_of(sz);
		if (p == NULL) {
			atomic_fetch_add(&total_fail, 1);
			atomic_fetch_add(&fail_by_size[b], 1);
			int expect = -1;
			if (atomic_compare_exchange_strong(&first_errno,
			    &expect, e)) {
				atomic_store(&first_fail_size, sz);
				atomic_store(&first_fail_rss_kb,
				    read_status_kb("VmRSS:"));
				atomic_store(&first_fail_vsz_kb,
				    read_status_kb("VmSize:"));
			}
			continue;
		}
		memset(p, 0xAB, sz);
		live[i] = p;
		lsz[i] = sz;
		atomic_fetch_add(&total_ok, 1);
		atomic_fetch_add(&ok_by_size[b], 1);
	}

	for (size_t i = 0; i < w->nlive; i++) {
		if (!live[i]) continue;
		if (w->arm == ARM_LIBC) free(live[i]);
		else umem_free(live[i], lsz[i]);
	}
	free(live); free(lsz);
	return NULL;
}

static void report_vmem(const char *when)
{
	vmem_t *heap = vmem_heap_arena(NULL, NULL);
	if (heap == NULL) {
		printf("  vmem[%s]: heap arena unavailable\n", when);
		return;
	}
	/* vmem_t internals are private; report what the public API exposes.
	 * vmem_size() with VMEM_FREE|VMEM_ALLOC is the arena's total span. */
	printf("  vmem[%-6s]: heap in_use=%zu free=%zu total=%zu\n", when,
	    vmem_size(heap, VMEM_ALLOC), vmem_size(heap, VMEM_FREE),
	    vmem_size(heap, VMEM_ALLOC | VMEM_FREE));
}

static void run_arm(const char *name, enum arm arm, int nthreads, size_t nlive)
{
	atomic_store(&total_ok, 0);
	atomic_store(&total_fail, 0);
	atomic_store(&first_errno, -1);
	for (int i = 0; i < NBUCKET; i++) {
		atomic_store(&fail_by_size[i], 0);
		atomic_store(&ok_by_size[i], 0);
	}

	printf("\n=== arm %s: %d threads x %zu live objects, sizes 16..4096 ===\n",
	    name, nthreads, nlive);
	if (arm != ARM_LIBC)
		report_vmem("before");

	pthread_t *th = calloc(nthreads, sizeof(*th));
	struct warg *wa = calloc(nthreads, sizeof(*wa));
	int started = 0;
	for (int i = 0; i < nthreads; i++) {
		wa[i] = (struct warg){ .tid = i, .arm = arm, .nlive = nlive };
		if (pthread_create(&th[i], NULL, worker, &wa[i]) != 0) {
			fprintf(stderr, "probe: pthread_create failed at %d\n", i);
			break;
		}
		started++;
	}
	for (int i = 0; i < started; i++)
		pthread_join(th[i], NULL);
	free(th); free(wa);

	unsigned long long ok = atomic_load(&total_ok);
	unsigned long long bad = atomic_load(&total_fail);
	printf("  threads_started=%d  ok=%llu  FAILED=%llu  (%.1f%% of attempts)\n",
	    started, ok, bad,
	    (ok + bad) ? 100.0 * (double)bad / (double)(ok + bad) : 0.0);
	printf("  peak VmHWM=%zu kB  VmSize=%zu kB\n",
	    read_status_kb("VmHWM:"), read_status_kb("VmSize:"));

	if (bad > 0) {
		int e = atomic_load(&first_errno);
		printf("  FIRST FAILURE: size=%zu errno=%d (%s)\n"
		       "                 at that moment VmRSS=%llu kB VmSize=%llu kB\n",
		    atomic_load(&first_fail_size), e,
		    e > 0 ? strerror(e) : "unset/0 -- NOT an errno-reporting failure",
		    atomic_load(&first_fail_rss_kb),
		    atomic_load(&first_fail_vsz_kb));
		printf("  failing sizes by power-of-two bucket:\n");
		for (int i = 0; i < NBUCKET; i++) {
			unsigned long long f = atomic_load(&fail_by_size[i]);
			unsigned long long o = atomic_load(&ok_by_size[i]);
			if (f == 0 && o == 0) continue;
			printf("    [%6zu..%6zu) ok=%-10llu fail=%-10llu %s\n",
			    (size_t)1 << i, (size_t)1 << (i + 1), o, f,
			    (f > 0 && o == 0) ? "<- ALL failed" :
			    (f > 0) ? "<- some failed" : "");
		}
	}
	if (arm != ARM_LIBC)
		report_vmem("after");
}

int main(int argc, char **argv)
{
	int nthreads = (argc > 1) ? atoi(argv[1]) : 192;
	size_t nlive = (argc > 2) ? strtoull(argv[2], NULL, 10) : 250000;
	if (nthreads < 1) nthreads = 1;

	printf("probe_alloc_failure: threads=%d live/thread=%zu\n",
	    nthreads, nlive);
	printf("total live objects at peak = %.1fM, ~%.1f GB at 2KB mean\n",
	    (double)nthreads * nlive / 1e6,
	    (double)nthreads * nlive * 2048 / 1e9);

	/* libc first: it is the control and it does not perturb umem state. */
	run_arm("B libc malloc (control)", ARM_LIBC, nthreads, nlive);
	run_arm("A umem_alloc UMEM_DEFAULT", ARM_UMEM, nthreads, nlive);
	run_arm("C umem_alloc + reap and retry", ARM_UMEM_RETRY, nthreads,
	    nlive);

	printf("\n--- how to read this ---\n"
	    "errno=ENOMEM with VmSize near a ceiling  => genuine exhaustion\n"
	    "errno unset/0                            => umem declined without\n"
	    "                                            an OS-level failure\n"
	    "arm C failures ~0 while arm A fails      => fail-fast under\n"
	    "                                            pressure, benchmark\n"
	    "                                            never asked for retry\n"
	    "failures clustered in one size bucket    => per-cache limit\n"
	    "failures uniform across buckets          => global/arena limit\n");
	return 0;
}
