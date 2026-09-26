/*
 * tool_target -- live target for the debugger / umemctl runtime pass
 * (docs/reviews/2026-09-26-tool-review.md) and the regressions built on it.
 *
 * Neither existing target covers what the tools advertise:
 * test_inspect_live.c is single-threaded and idle, introspect_churn.c is
 * single-threaded with sizes <= 496, and test_gdb_extension.c blocks on
 * getchar().  This one:
 *
 *   - HOLDS a known set across size classes, including a 96 KiB buffer
 *     (cache-served, > 64 KiB) and two 256 KiB buffers (oversize: above
 *     UMEM_MAXBUF, served from vmem, not from any cache);
 *   - FREES a 48-byte buffer on the main thread, which lands in the main
 *     thread's PTC bin (unless tcache=0 or a debug mode disables PTC), a
 *     96 KiB buffer (magazine / rseq layer), and a 256 KiB buffer (vmem);
 *   - keeps NTHREADS churn threads allocating and freeing continuously, via
 *     umem_alloc, umem_zalloc (size 777 only) and umem_cache_alloc on a user
 *     cache "tt_user" (size 100), so an alloc breakpoint has live traffic of
 *     every entry-point kind;
 *   - leaks one 200-byte buffer every 4096 churn iterations (capped), from a
 *     distinct noinline site, so leak tools have a real leak whose size is NOT
 *     a cache size (200 is served by the 224-byte class);
 *   - with argv[4] == "dupfree:<secs>", double-frees a buffer after <secs>
 *     seconds so umem_event_error() is reachable (run under
 *     UMEM_DEBUG=default; the delay leaves time for a debugger to attach).
 *
 * argv[1] = info file.  "key=value" lines are written to <file>.tmp and
 *           renamed into place once the held set exists, so a reader that
 *           sees the file sees all of it.
 * argv[2] = churn threads (default 3)
 * argv[3] = seconds to live, 0 = forever (default 0)
 * argv[4] = "dupfree:<secs>" (optional)
 *
 * SIGUSR2 is ignored so a harness can use it to knock a debugger out of
 * `continue` without the detach then killing the target.
 */

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "umem.h"

#define	NHELD_64	64
#define	NHELD_512	16
#define	NHELD_4K	8
#define	NHELD_96K	4
#define	NHELD_BIG	2
#define	SZ_96K		(96 * 1024)
#define	SZ_BIG		(256 * 1024)
#define	LEAK_CAP	256

static umem_cache_t *user_cache;
static volatile unsigned long leaked;

void *held_64[NHELD_64], *held_512[NHELD_512], *held_4k[NHELD_4K];
void *held_96k[NHELD_96K], *held_big[NHELD_BIG];
void *freed_ptc, *freed_96k, *freed_big;

static void __attribute__((noinline))
hold_all(void)
{
	int i;

	for (i = 0; i < NHELD_64; i++)
		held_64[i] = umem_alloc(64, UMEM_NOFAIL);
	for (i = 0; i < NHELD_512; i++)
		held_512[i] = umem_alloc(512, UMEM_NOFAIL);
	for (i = 0; i < NHELD_4K; i++)
		held_4k[i] = umem_alloc(4096, UMEM_NOFAIL);
	for (i = 0; i < NHELD_96K; i++)
		held_96k[i] = umem_alloc(SZ_96K, UMEM_NOFAIL);
	for (i = 0; i < NHELD_BIG; i++)
		held_big[i] = umem_alloc(SZ_BIG, UMEM_NOFAIL);
}

static void __attribute__((noinline))
free_some(void)
{
	freed_ptc = umem_alloc(48, UMEM_NOFAIL);
	umem_free(freed_ptc, 48);
	freed_96k = umem_alloc(SZ_96K, UMEM_NOFAIL);
	umem_free(freed_96k, SZ_96K);
	freed_big = umem_alloc(SZ_BIG, UMEM_NOFAIL);
	umem_free(freed_big, SZ_BIG);
}

static void __attribute__((noinline))
leak_site(void)
{
	void *p = umem_alloc(200, UMEM_DEFAULT);

	if (p != NULL) {
		memset(p, 0xab, 200);
		leaked++;
	}
}

static void *
churn(void *arg)
{
	static const size_t sizes[] = {
		24, 64, 200, 512, 1500, 4096, 9000, 40000, 70000, SZ_96K,
		200000
	};
	const unsigned nsz = sizeof (sizes) / sizeof (sizes[0]);
	void *ring[16] = { 0 };
	size_t rsz[16];
	unsigned long i = (uintptr_t)arg;

	for (;;) {
		unsigned r = (unsigned)(i % 16);
		size_t sz = sizes[i % nsz];
		void *u, *z;

		if (ring[r] != NULL)
			umem_free(ring[r], rsz[r]);
		ring[r] = umem_alloc(sz, UMEM_DEFAULT);
		rsz[r] = sz;
		if (ring[r] != NULL)
			memset(ring[r], 0x5a, sz < 64 ? sz : 64);

		if ((u = umem_cache_alloc(user_cache, UMEM_DEFAULT)) != NULL)
			umem_cache_free(user_cache, u);
		if ((z = umem_zalloc(777, UMEM_DEFAULT)) != NULL)
			umem_free(z, 777);

		if ((i % 4096) == 0 && leaked < LEAK_CAP)
			leak_site();
		if ((++i % 1024) == 0)
			usleep(500);
	}
	return (NULL);
}

int
main(int argc, char **argv)
{
	const char *info = (argc > 1) ? argv[1] : NULL;
	int nthr = (argc > 2) ? atoi(argv[2]) : 3;
	long secs = (argc > 3) ? strtol(argv[3], NULL, 10) : 0;
	int dupfree = (argc > 4 && strncmp(argv[4], "dupfree:", 8) == 0);
	long dupfree_at = dupfree ? strtol(argv[4] + 8, NULL, 10) : 0;
	time_t start = time(NULL);
	pthread_t tid;
	int i;

	(void) signal(SIGUSR2, SIG_IGN);

	user_cache = umem_cache_create("tt_user", 100, 0, NULL, NULL, NULL,
	    NULL, NULL, 0);
	hold_all();
	free_some();

	if (info != NULL) {
		char tmp[4096];
		FILE *fp;

		(void) snprintf(tmp, sizeof (tmp), "%s.tmp", info);
		if ((fp = fopen(tmp, "w")) == NULL) {
			perror(tmp);
			return (1);
		}
		fprintf(fp, "pid=%ld\n", (long)getpid());
		fprintf(fp, "held_64=%p\nheld_512=%p\nheld_4k=%p\n",
		    held_64[0], held_512[0], held_4k[0]);
		fprintf(fp, "held_96k=%p\nheld_big=%p\n", held_96k[0],
		    held_big[0]);
		fprintf(fp, "freed_ptc=%p\nfreed_96k=%p\nfreed_big=%p\n",
		    freed_ptc, freed_96k, freed_big);
		fprintf(fp, "nheld=%d\n", NHELD_64 + NHELD_512 + NHELD_4K +
		    NHELD_96K + NHELD_BIG);
		(void) fclose(fp);
		if (rename(tmp, info) != 0) {
			perror("rename");
			return (1);
		}
	}

	for (i = 0; i < nthr; i++)
		(void) pthread_create(&tid, NULL, churn,
		    (void *)(uintptr_t)(i * 7));

	for (;;) {
		(void) sleep(1);
		if (dupfree && time(NULL) - start >= dupfree_at) {
			void *p = umem_alloc(64, UMEM_NOFAIL);

			umem_free(p, 64);
			umem_free(p, 64);	/* UMERR_DUPFREE under debug */
			dupfree = 0;
		}
		if (secs > 0 && time(NULL) - start >= secs)
			break;
	}
	return (0);
}
