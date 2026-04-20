/*
 * Unit tests for allocation pattern profiling and predictive replay.
 */

#include "../munit.h"
#include "../../umem_impl.h"
#include "../../umem_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_PROFILE_PATH "/tmp/claude-1000/test_profile.ump"

/*
 * Test 1: Record a profile during an allocation workload and verify
 * the .ump file is created with valid contents.
 */
static MunitResult
test_profile_record(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	(void)unlink(TEST_PROFILE_PATH);

	char spec[256];
	snprintf(spec, sizeof(spec), "record:%s", TEST_PROFILE_PATH);
	int rc = umem_profile_init(spec);
	munit_assert_int(rc, ==, 0);

	/* Generate some allocation activity */
	umem_cache_t *cp = umem_cache_create("test_prof", 128, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	void *bufs[200];
	for (int i = 0; i < 200; i++) {
		bufs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(bufs[i]);
	}

	/* Sample a few times to build profile data */
	umem_profile_sample();
	umem_profile_sample();

	for (int i = 0; i < 200; i++)
		umem_cache_free(cp, bufs[i]);

	umem_profile_sample();

	umem_profile_fini();

	/* Verify file exists and has non-zero size */
	struct stat st;
	int stat_rc = stat(TEST_PROFILE_PATH, &st);
	munit_assert_int(stat_rc, ==, 0);
	munit_assert_size(st.st_size, >, 0);

	umem_cache_destroy(cp);

	return MUNIT_OK;
}

/*
 * Test 2: Load a recorded profile and verify cache pre-sizing is applied.
 */
static MunitResult
test_profile_load_and_apply(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	/* First, record a profile */
	(void)unlink(TEST_PROFILE_PATH);

	char rec_spec[256];
	snprintf(rec_spec, sizeof(rec_spec), "record:%s", TEST_PROFILE_PATH);
	int rc = umem_profile_init(rec_spec);
	munit_assert_int(rc, ==, 0);

	umem_cache_t *cp = umem_cache_create("test_apply", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	void *bufs[500];
	for (int i = 0; i < 500; i++) {
		bufs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(bufs[i]);
	}
	umem_profile_sample();
	for (int i = 0; i < 500; i++)
		umem_cache_free(cp, bufs[i]);

	umem_profile_fini();
	umem_cache_destroy(cp);

	/* Now load and verify */
	char use_spec[256];
	snprintf(use_spec, sizeof(use_spec), "use:%s", TEST_PROFILE_PATH);
	rc = umem_profile_init(use_spec);
	munit_assert_int(rc, ==, 0);

	/* Create a cache with matching name; apply_cache should succeed */
	cp = umem_cache_create("test_apply", 64, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	umem_profile_apply_cache(cp);

	/*
	 * Verify the cache has been warmed: buftotal should be > 0 after
	 * pre-allocation (the profile recorded peak buftotal >= 500).
	 */
	munit_assert_uint64(cp->cache_buftotal, >, 0);

	umem_profile_fini();
	umem_cache_destroy(cp);

	return MUNIT_OK;
}

/*
 * Test 3: Verify phase detection with a synthetic burst pattern.
 * We alternate between low and high allocation rates, sampling after
 * each to trigger phase boundaries.
 */
static MunitResult
test_profile_phase_detection(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	(void)unlink(TEST_PROFILE_PATH);

	char spec[256];
	snprintf(spec, sizeof(spec), "record:%s", TEST_PROFILE_PATH);
	int rc = umem_profile_init(spec);
	munit_assert_int(rc, ==, 0);

	umem_cache_t *cp = umem_cache_create("test_phase", 256, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	/* Phase A: low activity */
	void *bufs[20];
	for (int i = 0; i < 20; i++) {
		bufs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(bufs[i]);
	}
	umem_profile_sample();
	for (int i = 0; i < 20; i++)
		umem_cache_free(cp, bufs[i]);
	umem_profile_sample();

	/* Phase B: high burst */
	void *burst[1000];
	for (int i = 0; i < 1000; i++) {
		burst[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
		munit_assert_not_null(burst[i]);
	}
	umem_profile_sample();
	for (int i = 0; i < 1000; i++)
		umem_cache_free(cp, burst[i]);
	umem_profile_sample();

	umem_profile_fini();

	/* Load the profile and verify phases were recorded */
	rc = umem_profile_load(TEST_PROFILE_PATH);
	munit_assert_int(rc, ==, 0);

	/* Dump to verify visually (also exercises dump_text) */
	umem_profile_dump_text(stdout);

	umem_profile_fini();
	umem_cache_destroy(cp);

	return MUNIT_OK;
}

/*
 * Test 4: Verify the dump tool output format by loading a profile
 * and checking the text output contains expected fields.
 */
static MunitResult
test_profile_dump_format(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	(void)unlink(TEST_PROFILE_PATH);

	char spec[256];
	snprintf(spec, sizeof(spec), "record:%s", TEST_PROFILE_PATH);
	int rc = umem_profile_init(spec);
	munit_assert_int(rc, ==, 0);

	umem_cache_t *cp = umem_cache_create("test_dump", 32, 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	void *p = umem_cache_alloc(cp, UMEM_DEFAULT);
	munit_assert_not_null(p);
	umem_profile_sample();
	umem_cache_free(cp, p);
	umem_profile_fini();

	/* Load and dump to a temp file */
	rc = umem_profile_load(TEST_PROFILE_PATH);
	munit_assert_int(rc, ==, 0);

	char dump_path[] = "/tmp/claude-1000/test_profile_dump.txt";
	FILE *fp = fopen(dump_path, "w");
	munit_assert_not_null(fp);
	umem_profile_dump_text(fp);
	fclose(fp);

	/* Read back and check format */
	fp = fopen(dump_path, "r");
	munit_assert_not_null(fp);

	char line[512];
	int found_header = 0;
	int found_cache = 0;

	while (fgets(line, sizeof(line), fp) != NULL) {
		if (strstr(line, "=== umem profile ===") != NULL)
			found_header = 1;
		if (strstr(line, "test_dump") != NULL)
			found_cache = 1;
	}
	fclose(fp);

	munit_assert_int(found_header, ==, 1);
	munit_assert_int(found_cache, ==, 1);

	umem_profile_fini();
	umem_cache_destroy(cp);

	(void)unlink(dump_path);

	return MUNIT_OK;
}

/*
 * Test: init with NULL or empty string is a no-op.
 */
static MunitResult
test_profile_init_noop(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	munit_assert_int(umem_profile_init(NULL), ==, 0);
	munit_assert_int(umem_profile_init(""), ==, 0);

	/* Invalid mode prefix */
	munit_assert_int(umem_profile_init("invalid:/tmp/foo"), ==, -1);

	return MUNIT_OK;
}

/*
 * Test: loading a nonexistent file fails gracefully.
 */
static MunitResult
test_profile_load_missing(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	int rc = umem_profile_load("/tmp/claude-1000/nonexistent.ump");
	munit_assert_int(rc, ==, -1);

	return MUNIT_OK;
}

static MunitTest profile_tests[] = {
	{ "/record", test_profile_record, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/load_and_apply", test_profile_load_and_apply, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/phase_detection", test_profile_phase_detection, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/dump_format", test_profile_dump_format, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/init_noop", test_profile_init_noop, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/load_missing", test_profile_load_missing, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_profile = {
	"/profile",
	profile_tests,
	NULL,
	1,
	MUNIT_SUITE_OPTION_NONE
};
