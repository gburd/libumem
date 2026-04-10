/*
 * Unit tests for umem_audit.c
 *
 * Tests debugger integration and audit functions
 */

#include "../../config.h"
#include "../../umem.h"
#include "../../umem_impl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../munit.h"

/* External audit functions */
extern void umem_dump_audit_buffer(umem_bufctl_audit_t *bcp);
extern void umem_dump_all_audits(void);
extern umem_bufctl_audit_t *umem_get_audit_info(void *addr);
extern void umem_dump_cache_stats(void);
extern int umem_verify_all_caches(void);
extern void umem_find_leaks(void);

/* Test: umem_dump_audit_buffer with NULL pointer */
static MunitResult
test_dump_audit_buffer_null(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	/* Redirect stderr to /dev/null to avoid test output noise */
	int saved_stderr = dup(STDERR_FILENO);
	FILE *devnull = fopen("/dev/null", "w");
	dup2(fileno(devnull), STDERR_FILENO);

	/* Should handle NULL gracefully */
	umem_dump_audit_buffer(NULL);

	/* Restore stderr */
	dup2(saved_stderr, STDERR_FILENO);
	close(saved_stderr);
	fclose(devnull);

	return MUNIT_OK;
}

/* Test: umem_dump_audit_buffer with mock audit structure */
static MunitResult
test_dump_audit_buffer_valid(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	/* Create a mock audit structure */
	umem_bufctl_audit_t audit;
	memset(&audit, 0, sizeof(audit));

	audit.bc_addr = (void *)0x12345678;
	audit.bc_cache = NULL;  /* Will show "<unknown>" */
	audit.bc_timestamp = 123456789;
	audit.bc_thread = (thread_t)(uintptr_t)0x999;
	audit.bc_depth = 0;  /* No stack trace */

	/* Redirect stderr to /dev/null */
	int saved_stderr = dup(STDERR_FILENO);
	FILE *devnull = fopen("/dev/null", "w");
	dup2(fileno(devnull), STDERR_FILENO);

	umem_dump_audit_buffer(&audit);

	/* Restore stderr */
	dup2(saved_stderr, STDERR_FILENO);
	close(saved_stderr);
	fclose(devnull);

	return MUNIT_OK;
}

/* Test: umem_dump_audit_buffer with stack trace */
static MunitResult
test_dump_audit_buffer_with_stack(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	/* Create audit-enabled cache */
	umem_cache_t *cache = umem_cache_create(
		"test_audit_cache",
		64,
		0,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		0x00000001  /* UMF_AUDIT */
	);
	munit_assert_not_null(cache);

	/* Allocate from cache (creates real audit record) */
	void *ptr = umem_cache_alloc(cache, UMEM_DEFAULT);
	munit_assert_not_null(ptr);

	/* Get audit info with real stack trace */
	umem_bufctl_audit_t *audit = umem_get_audit_info(ptr);

	/* Redirect stderr to /dev/null */
	int saved_stderr = dup(STDERR_FILENO);
	FILE *devnull = fopen("/dev/null", "w");
	dup2(fileno(devnull), STDERR_FILENO);

	/* Dump audit buffer - will have real addresses from actual allocation */
	umem_dump_audit_buffer(audit);

	/* Restore stderr */
	dup2(saved_stderr, STDERR_FILENO);
	close(saved_stderr);
	fclose(devnull);

	/* Cleanup */
	umem_cache_free(cache, ptr);
	umem_cache_destroy(cache);

	return MUNIT_OK;
}

/* Test: umem_dump_audit_buffer with many stack frames */
static MunitResult
test_dump_audit_buffer_many_frames(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	/* Create audit-enabled cache */
	umem_cache_t *cache = umem_cache_create(
		"test_audit_many",
		128,
		0,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		0x00000001  /* UMF_AUDIT */
	);
	munit_assert_not_null(cache);

	/* Allocate from cache (creates real audit record) */
	void *ptr = umem_cache_alloc(cache, UMEM_DEFAULT);
	munit_assert_not_null(ptr);

	/* Get audit info - real stack trace will have actual frames */
	umem_bufctl_audit_t *audit = umem_get_audit_info(ptr);

	/* Redirect stderr to /dev/null */
	int saved_stderr = dup(STDERR_FILENO);
	FILE *devnull = fopen("/dev/null", "w");
	dup2(fileno(devnull), STDERR_FILENO);

	/* Dump audit buffer - tests max frame display limit */
	umem_dump_audit_buffer(audit);

	/* Restore stderr */
	dup2(saved_stderr, STDERR_FILENO);
	close(saved_stderr);
	fclose(devnull);

	/* Cleanup */
	umem_cache_free(cache, ptr);
	umem_cache_destroy(cache);

	return MUNIT_OK;
}

/* Test: umem_dump_all_audits */
static MunitResult
test_dump_all_audits(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	/* Initialize umem if not already done */
	void *ptr = umem_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(ptr);

	/* Redirect stderr to /dev/null */
	int saved_stderr = dup(STDERR_FILENO);
	FILE *devnull = fopen("/dev/null", "w");
	dup2(fileno(devnull), STDERR_FILENO);

	/* Dump all audits (may or may not find any depending on UMEM_DEBUG) */
	umem_dump_all_audits();

	/* Restore stderr */
	dup2(saved_stderr, STDERR_FILENO);
	close(saved_stderr);
	fclose(devnull);

	umem_free(ptr, 64);

	return MUNIT_OK;
}

/* Test: umem_get_audit_info */
static MunitResult
test_get_audit_info(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	void *ptr = umem_alloc(128, UMEM_DEFAULT);
	munit_assert_not_null(ptr);

	/* Try to get audit info (will likely return NULL unless UMEM_DEBUG=audit) */
	umem_bufctl_audit_t *audit = umem_get_audit_info(ptr);

	/* We don't assert anything specific since it depends on debug settings */
	/* Just ensure the function doesn't crash */
	(void)audit;

	umem_free(ptr, 128);

	return MUNIT_OK;
}

/* Test: umem_get_audit_info with NULL */
static MunitResult
test_get_audit_info_null(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	/* Should handle NULL gracefully */
	umem_bufctl_audit_t *audit = umem_get_audit_info(NULL);
	munit_assert_null(audit);

	return MUNIT_OK;
}

/* Test: umem_dump_cache_stats */
static MunitResult
test_dump_cache_stats(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	/* Make some allocations to ensure caches exist */
	void *ptrs[10];
	for (int i = 0; i < 10; i++) {
		ptrs[i] = umem_alloc(64 * (i + 1), UMEM_DEFAULT);
		munit_assert_not_null(ptrs[i]);
	}

	/* Redirect stderr to /dev/null */
	int saved_stderr = dup(STDERR_FILENO);
	FILE *devnull = fopen("/dev/null", "w");
	dup2(fileno(devnull), STDERR_FILENO);

	/* Dump cache stats */
	umem_dump_cache_stats();

	/* Restore stderr */
	dup2(saved_stderr, STDERR_FILENO);
	close(saved_stderr);
	fclose(devnull);

	/* Free allocations */
	for (int i = 0; i < 10; i++) {
		umem_free(ptrs[i], 64 * (i + 1));
	}

	return MUNIT_OK;
}

/* Test: umem_verify_all_caches */
static MunitResult
test_verify_all_caches(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	/* Make some allocations */
	void *ptr1 = umem_alloc(256, UMEM_DEFAULT);
	void *ptr2 = umem_alloc(512, UMEM_DEFAULT);
	munit_assert_not_null(ptr1);
	munit_assert_not_null(ptr2);

	/* Redirect stderr to /dev/null */
	int saved_stderr = dup(STDERR_FILENO);
	FILE *devnull = fopen("/dev/null", "w");
	dup2(fileno(devnull), STDERR_FILENO);

	/* Verify caches - should pass with no errors */
	int errors = umem_verify_all_caches();

	/* Restore stderr */
	dup2(saved_stderr, STDERR_FILENO);
	close(saved_stderr);
	fclose(devnull);

	/* Should find no errors in healthy caches */
	munit_assert_int(errors, ==, 0);

	umem_free(ptr1, 256);
	umem_free(ptr2, 512);

	return MUNIT_OK;
}

/* Test: umem_find_leaks */
static MunitResult
test_find_leaks(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	/* Allocate many buffers to trigger leak detection output */
	void *ptrs[150];
	for (int i = 0; i < 150; i++) {
		ptrs[i] = umem_alloc(1024, UMEM_DEFAULT);
		munit_assert_not_null(ptrs[i]);
	}

	/* Redirect stderr to /dev/null */
	int saved_stderr = dup(STDERR_FILENO);
	FILE *devnull = fopen("/dev/null", "w");
	dup2(fileno(devnull), STDERR_FILENO);

	/* Find leaks - should report the buffers we just allocated */
	umem_find_leaks();

	/* Restore stderr */
	dup2(saved_stderr, STDERR_FILENO);
	close(saved_stderr);
	fclose(devnull);

	/* Free allocations */
	for (int i = 0; i < 150; i++) {
		umem_free(ptrs[i], 1024);
	}

	return MUNIT_OK;
}

/* Test: umem_find_leaks with no leaks */
static MunitResult
test_find_leaks_none(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	/* Don't allocate much, so leak detector reports nothing */
	void *ptr = umem_alloc(32, UMEM_DEFAULT);
	munit_assert_not_null(ptr);
	umem_free(ptr, 32);

	/* Redirect stderr to /dev/null */
	int saved_stderr = dup(STDERR_FILENO);
	FILE *devnull = fopen("/dev/null", "w");
	dup2(fileno(devnull), STDERR_FILENO);

	/* Find leaks - should report minimal/no buffers */
	umem_find_leaks();

	/* Restore stderr */
	dup2(saved_stderr, STDERR_FILENO);
	close(saved_stderr);
	fclose(devnull);

	return MUNIT_OK;
}

static MunitTest audit_tests[] = {
	{
		"/dump_audit_buffer/null",
		test_dump_audit_buffer_null,
		NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL
	},
	{
		"/dump_audit_buffer/valid",
		test_dump_audit_buffer_valid,
		NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL
	},
	{
		"/dump_audit_buffer/with_stack",
		test_dump_audit_buffer_with_stack,
		NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL
	},
	{
		"/dump_audit_buffer/many_frames",
		test_dump_audit_buffer_many_frames,
		NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL
	},
	{
		"/dump_all_audits",
		test_dump_all_audits,
		NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL
	},
	{
		"/get_audit_info",
		test_get_audit_info,
		NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL
	},
	{
		"/get_audit_info/null",
		test_get_audit_info_null,
		NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL
	},
	{
		"/dump_cache_stats",
		test_dump_cache_stats,
		NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL
	},
	{
		"/verify_all_caches",
		test_verify_all_caches,
		NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL
	},
	{
		"/find_leaks",
		test_find_leaks,
		NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL
	},
	{
		"/find_leaks/none",
		test_find_leaks_none,
		NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL
	},
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_umem_audit = {
	"/umem_audit",
	audit_tests,
	NULL,
	1,
	MUNIT_SUITE_OPTION_NONE
};
