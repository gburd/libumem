/*
 * Long-running test: allocate some leaks, write our pid, wait.
 */
#include "umem.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void __attribute__((noinline))
leak_small(void)
{
	for (int i = 0; i < 5; i++)
		(void) umem_alloc(64, UMEM_DEFAULT);
}

static void __attribute__((noinline))
leak_big(void)
{
	(void) umem_alloc(4096, UMEM_DEFAULT);
	(void) umem_alloc(4096, UMEM_DEFAULT);
}

int
main(int argc, char **argv)
{
	for (int i = 0; i < 100; i++) {
		void *p = umem_alloc(32, UMEM_DEFAULT);
		umem_free(p, 32);
	}

	leak_small();
	leak_big();
	leak_small();

	if (argc > 1) {
		FILE *fp = fopen(argv[1], "w");
		if (fp != NULL) {
			fprintf(fp, "%d\n", (int)getpid());
			fclose(fp);
		}
	}
	printf("pid=%d sleeping forever; attach or use umem\n",
	    (int)getpid());
	fflush(stdout);
	while (1)
		sleep(3600);
	return (0);
}
