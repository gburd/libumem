#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "umem.h"

/*
 * These symbols are defined in libumem but not exported via umem.h.
 * We access them to verify PTC (per-thread cache) state.
 */
extern int umem_ptc_enabled;
extern const int umem_genasm_supported;

static const char *TESTSTRINGS[] = {
  "fred",
  "fredfredfred",
  "thisisabitlongerthantheotherstrings",
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ01234567890",
};

#define N_TESTSTRINGS (sizeof(TESTSTRINGS) / sizeof(TESTSTRINGS[0]))
#define N_TESTS 1000
#define N_THREADS 4

struct thread_result {
  int success;
  int alloc_count;
};

/*
 * Thread worker: performs the same alloc/verify/free cycle as the
 * original single-threaded test.  Each thread exercises the PTC
 * path independently.
 */
static void *
thread_worker(void *arg)
{
  struct thread_result *res = (struct thread_result *)arg;
  char *testcases[N_TESTSTRINGS][N_TESTS + 1];
  size_t len[N_TESTSTRINGS];
  int i, j;

  memset(testcases, 0, sizeof(testcases));
  res->success = 1;
  res->alloc_count = 0;

  for (i = 0; i < (int)N_TESTSTRINGS; ++i)
    len[i] = strlen(TESTSTRINGS[i]) + 1;

  for (j = 0; j < N_TESTS; ++j)
  {
    for (i = 0; i < (int)N_TESTSTRINGS; ++i)
    {
      testcases[i][j] = umem_alloc(len[i], UMEM_DEFAULT);
      if (testcases[i][j] == NULL) {
        res->success = 0;
        return NULL;
      }
      strcpy(testcases[i][j], TESTSTRINGS[i]);
      res->alloc_count++;
    }
  }

  /* Verify contents before freeing. */
  for (j = 0; j < N_TESTS; ++j)
  {
    for (i = 0; i < (int)N_TESTSTRINGS; ++i)
    {
      if (strcmp(testcases[i][j], TESTSTRINGS[i]) != 0) {
        fprintf(stderr, "thread %p: data corruption at [%d][%d]\n",
            (void *)pthread_self(), i, j);
        res->success = 0;
        return NULL;
      }
    }
  }

  for (j = 0; j < N_TESTS; ++j)
  {
    for (i = (int)N_TESTSTRINGS - 1; i >= 0; --i)
    {
      umem_free(testcases[i][j], len[i]);
    }

    if ((j % 25) == 0)
      umem_reap();
  }

  return NULL;
}

int
main (void)
{
  char *testcases[N_TESTSTRINGS][N_TESTS + 1];
  size_t len[N_TESTSTRINGS];
  int i, j;

  memset(testcases, 0, sizeof(testcases));

  for (i = 0; i < (int)N_TESTSTRINGS; ++i)
  {
    len[i] = strlen(TESTSTRINGS[i]) + 1;
  }

  /* --- Original single-threaded test --- */

  puts("Allocating...");

  for (j = 0; j < N_TESTS; ++j)
  {
    for (i = 0; i < (int)N_TESTSTRINGS; ++i)
    {
      testcases[i][j] = umem_alloc(len[i], UMEM_DEFAULT);
      if (testcases[i][j] == NULL) {
        fprintf(stderr, "umem_alloc failed at [%d][%d]\n", i, j);
        return EXIT_FAILURE;
      }
      strcpy(testcases[i][j], TESTSTRINGS[i]);
    }
  }

  puts("Deallocating...");

  for (j = 0; j < N_TESTS; ++j)
  {
    for (i = (int)N_TESTSTRINGS - 1; i >= 0; --i)
    {
      umem_free(testcases[i][j], len[i]);
    }

    if ((j % 25) == 0)
    {
      puts("Reaping...");
      umem_reap();
    }
  }

  /* --- PTC status check --- */

  printf("PTC: genasm_supported=%d, ptc_enabled=%d\n",
      umem_genasm_supported, umem_ptc_enabled);

  /* --- Multi-threaded PTC exercise --- */

  printf("Starting %d threads for multi-threaded PTC test...\n", N_THREADS);
  {
    pthread_t threads[N_THREADS];
    struct thread_result results[N_THREADS];
    int failed = 0;

    memset(results, 0, sizeof(results));

    for (i = 0; i < N_THREADS; i++) {
      if (pthread_create(&threads[i], NULL, thread_worker, &results[i]) != 0) {
        fprintf(stderr, "pthread_create failed for thread %d\n", i);
        return EXIT_FAILURE;
      }
    }

    for (i = 0; i < N_THREADS; i++) {
      pthread_join(threads[i], NULL);
      printf("  thread %d: %s (%d allocations)\n", i,
          results[i].success ? "PASS" : "FAIL",
          results[i].alloc_count);
      if (!results[i].success)
        failed = 1;
    }

    if (failed) {
      fprintf(stderr, "multi-threaded PTC test FAILED\n");
      return EXIT_FAILURE;
    }
  }

  puts("Done");

  return 0;
}
