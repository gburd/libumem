/*
 * Unit tests for umem_hooks.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "munit.h"
#include "umem_hooks.h"

/* Test allocator state */
typedef struct {
	int alloc_calls;
	int free_calls;
	int realloc_calls;
	size_t last_size;
	void *last_ptr;
} test_alloc_state_t;

/* Test allocator functions */
static void *
test_alloc(size_t size, void *arg)
{
	test_alloc_state_t *state = (test_alloc_state_t *)arg;
	state->alloc_calls++;
	state->last_size = size;
	void *ptr = malloc(size);
	state->last_ptr = ptr;
	return ptr;
}

static void
test_free(void *ptr, void *arg)
{
	test_alloc_state_t *state = (test_alloc_state_t *)arg;
	state->free_calls++;
	state->last_ptr = ptr;
	free(ptr);
}

static void *
test_realloc(void *ptr, size_t size, void *arg)
{
	test_alloc_state_t *state = (test_alloc_state_t *)arg;
	state->realloc_calls++;
	state->last_size = size;
	void *new_ptr = realloc(ptr, size);
	state->last_ptr = new_ptr;
	return new_ptr;
}

/* Test: Register and unregister hook */
static MunitResult
test_hook_register_unregister(const MunitParameter params[], void* data)
{
	test_alloc_state_t state = {0};
	umem_hook_t hook = {
		.hook_name = "test_hook",
		.hook_alloc = test_alloc,
		.hook_free = test_free,
		.hook_arg = &state
	};

	/* Register hook */
	int ret = umem_hook_register(&hook);
	munit_assert_int(ret, ==, 0);
	munit_assert_int(hook.hook_active, ==, 1);
	munit_assert_uint64(hook.alloc_count, ==, 0);
	munit_assert_uint64(hook.bytes_allocated, ==, 0);

	/* Try to register same hook again - should fail */
	ret = umem_hook_register(&hook);
	munit_assert_int(ret, ==, -1);

	/* Unregister hook */
	umem_hook_unregister(&hook);
	munit_assert_int(hook.hook_active, ==, 0);

	/* Unregister again - should be safe */
	umem_hook_unregister(&hook);

	return MUNIT_OK;
}

/* Test: Register with NULL hook */
static MunitResult
test_hook_register_null(const MunitParameter params[], void* data)
{
	int ret = umem_hook_register(NULL);
	munit_assert_int(ret, ==, -1);

	return MUNIT_OK;
}

/* Test: Register with NULL name */
static MunitResult
test_hook_register_null_name(const MunitParameter params[], void* data)
{
	test_alloc_state_t state = {0};
	umem_hook_t hook = {
		.hook_name = NULL,
		.hook_alloc = test_alloc,
		.hook_free = test_free,
		.hook_arg = &state
	};

	int ret = umem_hook_register(&hook);
	munit_assert_int(ret, ==, -1);

	return MUNIT_OK;
}

/* Test: Register with no functions */
static MunitResult
test_hook_register_no_functions(const MunitParameter params[], void* data)
{
	test_alloc_state_t state = {0};
	umem_hook_t hook = {
		.hook_name = "no_funcs",
		.hook_alloc = NULL,
		.hook_free = NULL,
		.hook_arg = &state
	};

	int ret = umem_hook_register(&hook);
	munit_assert_int(ret, ==, -1);

	return MUNIT_OK;
}

/* Test: Track allocation */
static MunitResult
test_hook_track_alloc(const MunitParameter params[], void* data)
{
	test_alloc_state_t state = {0};
	umem_hook_t hook = {
		.hook_name = "alloc_test",
		.hook_alloc = test_alloc,
		.hook_free = test_free,
		.hook_arg = &state
	};

	umem_hook_register(&hook);

	/* Track allocation */
	void *ptr = umem_hook_track_alloc(&hook, 1024);
	munit_assert_not_null(ptr);
	munit_assert_int(state.alloc_calls, ==, 1);
	munit_assert_size(state.last_size, ==, 1024);
	munit_assert_uint64(hook.alloc_count, ==, 1);
	munit_assert_uint64(hook.bytes_allocated, ==, 1024);
	munit_assert_uint64(hook.bytes_current, ==, 1024);
	munit_assert_uint64(hook.peak_bytes, ==, 1024);

	/* Track another allocation */
	void *ptr2 = umem_hook_track_alloc(&hook, 512);
	munit_assert_not_null(ptr2);
	munit_assert_int(state.alloc_calls, ==, 2);
	munit_assert_uint64(hook.alloc_count, ==, 2);
	munit_assert_uint64(hook.bytes_allocated, ==, 1536);
	munit_assert_uint64(hook.bytes_current, ==, 1536);
	munit_assert_uint64(hook.peak_bytes, ==, 1536);

	free(ptr);
	free(ptr2);
	umem_hook_unregister(&hook);

	return MUNIT_OK;
}

/* Test: Track free */
static MunitResult
test_hook_track_free(const MunitParameter params[], void* data)
{
	test_alloc_state_t state = {0};
	umem_hook_t hook = {
		.hook_name = "free_test",
		.hook_alloc = test_alloc,
		.hook_free = test_free,
		.hook_arg = &state
	};

	umem_hook_register(&hook);

	/* Allocate and free */
	void *ptr = umem_hook_track_alloc(&hook, 1024);
	munit_assert_not_null(ptr);

	umem_hook_track_free(&hook, ptr, 1024);
	munit_assert_int(state.free_calls, ==, 1);
	munit_assert_uint64(hook.free_count, ==, 1);
	munit_assert_uint64(hook.bytes_freed, ==, 1024);
	munit_assert_uint64(hook.bytes_current, ==, 0);

	umem_hook_unregister(&hook);

	return MUNIT_OK;
}

/* Test: Track free with NULL pointer */
static MunitResult
test_hook_track_free_null(const MunitParameter params[], void* data)
{
	test_alloc_state_t state = {0};
	umem_hook_t hook = {
		.hook_name = "free_null_test",
		.hook_alloc = test_alloc,
		.hook_free = test_free,
		.hook_arg = &state
	};

	umem_hook_register(&hook);

	/* Free NULL - should be safe */
	umem_hook_track_free(&hook, NULL, 0);
	munit_assert_int(state.free_calls, ==, 0);
	munit_assert_uint64(hook.free_count, ==, 0);

	umem_hook_unregister(&hook);

	return MUNIT_OK;
}

/* Test: Track realloc with custom realloc function */
static MunitResult
test_hook_track_realloc(const MunitParameter params[], void* data)
{
	test_alloc_state_t state = {0};
	umem_hook_t hook = {
		.hook_name = "realloc_test",
		.hook_alloc = test_alloc,
		.hook_free = test_free,
		.hook_realloc = test_realloc,
		.hook_arg = &state
	};

	umem_hook_register(&hook);

	/* Allocate initial memory */
	void *ptr = umem_hook_track_alloc(&hook, 1024);
	munit_assert_not_null(ptr);

	/* Realloc to larger size */
	void *new_ptr = umem_hook_track_realloc(&hook, ptr, 1024, 2048);
	munit_assert_not_null(new_ptr);
	munit_assert_int(state.realloc_calls, ==, 1);
	munit_assert_uint64(hook.realloc_count, ==, 1);
	munit_assert_uint64(hook.bytes_allocated, ==, 3072); /* 1024 + 2048 */
	munit_assert_uint64(hook.bytes_freed, ==, 1024);
	munit_assert_uint64(hook.bytes_current, ==, 2048);

	free(new_ptr);
	umem_hook_unregister(&hook);

	return MUNIT_OK;
}

/* Test: Track realloc without custom realloc function */
static MunitResult
test_hook_track_realloc_emulated(const MunitParameter params[], void* data)
{
	test_alloc_state_t state = {0};
	umem_hook_t hook = {
		.hook_name = "realloc_emulated_test",
		.hook_alloc = test_alloc,
		.hook_free = test_free,
		.hook_realloc = NULL, /* No realloc - will be emulated */
		.hook_arg = &state
	};

	umem_hook_register(&hook);

	/* Allocate initial memory */
	void *ptr = umem_hook_track_alloc(&hook, 1024);
	munit_assert_not_null(ptr);
	memset(ptr, 0xAB, 1024);

	/* Realloc to larger size - should use alloc+free emulation */
	void *new_ptr = umem_hook_track_realloc(&hook, ptr, 1024, 2048);
	munit_assert_not_null(new_ptr);
	munit_assert_int(state.alloc_calls, ==, 2); /* Initial + realloc */
	munit_assert_int(state.free_calls, ==, 1);  /* Old ptr freed */
	munit_assert_uint64(hook.realloc_count, ==, 1);

	/* Verify data was copied */
	munit_assert_uchar(((unsigned char *)new_ptr)[0], ==, 0xAB);

	free(new_ptr);
	umem_hook_unregister(&hook);

	return MUNIT_OK;
}

/* Test: Find hook by name */
static MunitResult
test_hook_find(const MunitParameter params[], void* data)
{
	test_alloc_state_t state = {0};
	umem_hook_t hook = {
		.hook_name = "find_test",
		.hook_alloc = test_alloc,
		.hook_free = test_free,
		.hook_arg = &state
	};

	umem_hook_register(&hook);

	/* Find existing hook */
	umem_hook_t *found = umem_hook_find("find_test");
	munit_assert_not_null(found);
	munit_assert_string_equal(found->hook_name, "find_test");

	/* Find non-existent hook */
	umem_hook_t *not_found = umem_hook_find("nonexistent");
	munit_assert_null(not_found);

	/* Find with NULL name */
	umem_hook_t *null_found = umem_hook_find(NULL);
	munit_assert_null(null_found);

	umem_hook_unregister(&hook);

	return MUNIT_OK;
}

/* Test: Walk hooks */
static int
walk_callback(umem_hook_t *hook, void *arg)
{
	int *count = (int *)arg;
	(*count)++;
	return 0;
}

static int
walk_callback_fail(umem_hook_t *hook, void *arg)
{
	int *count = (int *)arg;
	(*count)++;
	return -1; /* Fail on purpose */
}

static MunitResult
test_hook_walk(const MunitParameter params[], void* data)
{
	test_alloc_state_t state1 = {0};
	test_alloc_state_t state2 = {0};
	umem_hook_t hook1 = {
		.hook_name = "walk_test1",
		.hook_alloc = test_alloc,
		.hook_free = test_free,
		.hook_arg = &state1
	};
	umem_hook_t hook2 = {
		.hook_name = "walk_test2",
		.hook_alloc = test_alloc,
		.hook_free = test_free,
		.hook_arg = &state2
	};

	umem_hook_register(&hook1);
	umem_hook_register(&hook2);

	/* Walk all hooks */
	int count = 0;
	int ret = umem_hook_walk(walk_callback, &count);
	munit_assert_int(ret, ==, 0);
	munit_assert_int(count, ==, 2);

	/* Walk with failing callback */
	count = 0;
	ret = umem_hook_walk(walk_callback_fail, &count);
	munit_assert_int(ret, ==, -1);
	munit_assert_int(count, ==, 2); /* All hooks still visited */

	/* Walk with NULL func */
	ret = umem_hook_walk(NULL, NULL);
	munit_assert_int(ret, ==, -1);

	umem_hook_unregister(&hook1);
	umem_hook_unregister(&hook2);

	return MUNIT_OK;
}

/* Test: Dump hooks */
static MunitResult
test_hook_dump(const MunitParameter params[], void* data)
{
	test_alloc_state_t state = {0};
	umem_hook_t hook = {
		.hook_name = "dump_test",
		.hook_alloc = test_alloc,
		.hook_free = test_free,
		.hook_arg = &state
	};

	umem_hook_register(&hook);

	/* Perform some allocations to get stats */
	void *ptr1 = umem_hook_track_alloc(&hook, 1024);
	void *ptr2 = umem_hook_track_alloc(&hook, 512);
	umem_hook_track_free(&hook, ptr1, 1024);

	/* Dump to file - use a named temp file since tmpfile() may fail
	 * in sandboxed environments where /tmp is not writable */
	const char *tmpdir = getenv("TMPDIR");
	if (tmpdir == NULL)
		tmpdir = "/tmp";
	char tmppath[256];
	snprintf(tmppath, sizeof(tmppath), "%s/umem_hook_dump_XXXXXX", tmpdir);
	int fd = mkstemp(tmppath);
	if (fd < 0) {
		/* Cannot create temp file - skip */
		free(ptr2);
		umem_hook_unregister(&hook);
		return MUNIT_SKIP;
	}
	FILE *fp = fdopen(fd, "w+");
	if (fp == NULL) {
		close(fd);
		unlink(tmppath);
		free(ptr2);
		umem_hook_unregister(&hook);
		return MUNIT_SKIP;
	}

	umem_hook_dump_one(fp, &hook);
	munit_assert_long(ftell(fp), >, 0);

	fseek(fp, 0, SEEK_SET);
	umem_hook_dump(fp);
	munit_assert_long(ftell(fp), >, 0);

	fclose(fp);
	unlink(tmppath);

	/* Test dump with NULL fp - should use stderr */
	umem_hook_dump(NULL);

	free(ptr2);
	umem_hook_unregister(&hook);

	/* Test dump with no hooks */
	snprintf(tmppath, sizeof(tmppath), "%s/umem_hook_dump2_XXXXXX", tmpdir);
	fd = mkstemp(tmppath);
	if (fd >= 0) {
		fp = fdopen(fd, "w+");
		if (fp != NULL) {
			umem_hook_dump(fp);
			fclose(fp);
		} else {
			close(fd);
		}
		unlink(tmppath);
	}

	return MUNIT_OK;
}

/* Test: Dump with NULL hook */
static MunitResult
test_hook_dump_null(const MunitParameter params[], void* data)
{
	/* Use a named temp file since tmpfile() may fail in sandboxed environments */
	const char *tmpdir = getenv("TMPDIR");
	if (tmpdir == NULL)
		tmpdir = "/tmp";
	char tmppath[256];
	snprintf(tmppath, sizeof(tmppath), "%s/umem_hook_null_XXXXXX", tmpdir);
	int fd = mkstemp(tmppath);
	if (fd < 0)
		return MUNIT_SKIP;
	FILE *fp = fdopen(fd, "w+");
	if (fp == NULL) {
		close(fd);
		unlink(tmppath);
		return MUNIT_SKIP;
	}

	/* Should be safe */
	umem_hook_dump_one(fp, NULL);
	umem_hook_dump_one(NULL, NULL);

	fclose(fp);
	unlink(tmppath);

	return MUNIT_OK;
}

/* Test: Inactive hook operations */
static MunitResult
test_hook_inactive_operations(const MunitParameter params[], void* data)
{
	test_alloc_state_t state = {0};
	umem_hook_t hook = {
		.hook_name = "inactive_test",
		.hook_alloc = test_alloc,
		.hook_free = test_free,
		.hook_arg = &state
	};

	umem_hook_register(&hook);
	umem_hook_unregister(&hook);

	/* Operations on inactive hook should be safe */
	void *ptr = umem_hook_track_alloc(&hook, 1024);
	munit_assert_null(ptr);

	umem_hook_track_free(&hook, NULL, 0);

	ptr = umem_hook_track_realloc(&hook, NULL, 0, 1024);
	munit_assert_null(ptr);

	return MUNIT_OK;
}

/* Test: Peak bytes tracking */
static MunitResult
test_hook_peak_bytes(const MunitParameter params[], void* data)
{
	test_alloc_state_t state = {0};
	umem_hook_t hook = {
		.hook_name = "peak_test",
		.hook_alloc = test_alloc,
		.hook_free = test_free,
		.hook_arg = &state
	};

	umem_hook_register(&hook);

	/* Allocate increasing amounts */
	void *ptr1 = umem_hook_track_alloc(&hook, 1024);
	munit_assert_uint64(hook.peak_bytes, ==, 1024);

	void *ptr2 = umem_hook_track_alloc(&hook, 2048);
	munit_assert_uint64(hook.peak_bytes, ==, 3072);

	/* Free some memory - peak should remain */
	umem_hook_track_free(&hook, ptr1, 1024);
	munit_assert_uint64(hook.peak_bytes, ==, 3072);
	munit_assert_uint64(hook.bytes_current, ==, 2048);

	free(ptr2);
	umem_hook_unregister(&hook);

	return MUNIT_OK;
}

/* Test suite */
static MunitTest hook_tests[] = {
	{ "/register_unregister", test_hook_register_unregister, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/register_null", test_hook_register_null, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/register_null_name", test_hook_register_null_name, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/register_no_functions", test_hook_register_no_functions, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/track_alloc", test_hook_track_alloc, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/track_free", test_hook_track_free, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/track_free_null", test_hook_track_free_null, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/track_realloc", test_hook_track_realloc, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/track_realloc_emulated", test_hook_track_realloc_emulated, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/find", test_hook_find, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/walk", test_hook_walk, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/dump", test_hook_dump, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/dump_null", test_hook_dump_null, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/inactive_operations", test_hook_inactive_operations, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/peak_bytes", test_hook_peak_bytes, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_umem_hooks = {
	"/hooks", hook_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};
