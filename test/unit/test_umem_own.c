/*
 * Unit tests for umem ownership/borrowing system.
 */

#define UMEM_ENABLE_EXPERIMENTAL
#include "../munit.h"
#include "../../umem_own.h"
#include <string.h>
#include <pthread.h>

/* ------------------------------------------------------------------ */
/* Error capture infrastructure                                       */
/* ------------------------------------------------------------------ */

static umem_own_error_t last_error = 0;
static int error_count = 0;

static void
test_error_handler(umem_own_error_t err, const umem_owned_t *o,
    const char *msg)
{
	(void)o;
	(void)msg;
	last_error = err;
	error_count++;
}

static void
reset_errors(void)
{
	last_error = 0;
	error_count = 0;
}

/* ------------------------------------------------------------------ */
/* Lightweight mode tests (always on)                                 */
/* ------------------------------------------------------------------ */

static MunitResult
test_own_alloc_drop(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_owned_t *o = umem_own_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(o);
	munit_assert_int(umem_own_is_valid(o), !=, 0);
	munit_assert_uint32(umem_own_state(o), ==,
	    UMEM_OWN_STATE_OWNED);

	umem_drop(o);
	return MUNIT_OK;
}

static MunitResult
test_deref_write_read(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_owned_t *o = umem_own_alloc(128, UMEM_DEFAULT);
	munit_assert_not_null(o);

	void *p = umem_deref(o);
	munit_assert_not_null(p);

	memset(p, 0xAB, 128);
	unsigned char *bytes = (unsigned char *)p;
	for (int i = 0; i < 128; i++) {
		munit_assert_uint8(bytes[i], ==, 0xAB);
	}

	umem_drop(o);
	return MUNIT_OK;
}

static MunitResult
test_deref_const(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_owned_t *o = umem_own_alloc(32, UMEM_DEFAULT);
	munit_assert_not_null(o);

	void *p = umem_deref(o);
	memset(p, 0x42, 32);

	const void *cp = umem_deref_const(o);
	munit_assert_not_null(cp);
	munit_assert_uint8(((const unsigned char *)cp)[0], ==, 0x42);

	umem_drop(o);
	return MUNIT_OK;
}

static MunitResult
test_use_after_drop(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_own_set_error_handler(test_error_handler);
	reset_errors();

	umem_owned_t *o = umem_own_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(o);
	umem_drop(o);

	/*
	 * After drop, deref should fail.  The memory is freed, so we
	 * cannot safely dereference the handle.  However, our error
	 * handler captures the error type.  We pass the handle knowing
	 * the state field may still be readable (the slab hasn't been
	 * recycled yet in a single-threaded test).
	 */
	void *p = umem_deref(o);
	munit_assert_null(p);
	munit_assert_int(last_error, ==, UMEM_OWN_USE_AFTER_FREE);

	umem_own_set_error_handler(NULL);
	return MUNIT_OK;
}

static MunitResult
test_double_drop(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_own_set_error_handler(test_error_handler);
	reset_errors();

	umem_owned_t *o = umem_own_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(o);
	umem_drop(o);

	/* Second drop should report DOUBLE_FREE */
	umem_drop(o);
	munit_assert_int(last_error, ==, UMEM_OWN_DOUBLE_FREE);
	munit_assert_int(error_count, ==, 1);

	umem_own_set_error_handler(NULL);
	return MUNIT_OK;
}

static MunitResult
test_move_transfer(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_owned_t *o = umem_own_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(o);

	void *p1 = umem_deref(o);
	memset(p1, 0xCC, 64);

	umem_owned_t *moved = umem_move(o);
	munit_assert_not_null(moved);

	/* moved handle should be valid */
	munit_assert_int(umem_own_is_valid(moved), !=, 0);

	/* Data should be preserved */
	void *p2 = umem_deref(moved);
	munit_assert_not_null(p2);
	munit_assert_uint8(((unsigned char *)p2)[0], ==, 0xCC);

	umem_drop(moved);
	return MUNIT_OK;
}

static MunitResult
test_clone(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_owned_t *src = umem_own_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(src);

	void *ps = umem_deref(src);
	memset(ps, 0xDD, 64);

	umem_owned_t *dst = umem_clone(src, 64);
	munit_assert_not_null(dst);

	/* Both should be valid independently */
	munit_assert_int(umem_own_is_valid(src), !=, 0);
	munit_assert_int(umem_own_is_valid(dst), !=, 0);

	/* Data in clone should match */
	const void *pd = umem_deref_const(dst);
	munit_assert_uint8(((const unsigned char *)pd)[0], ==, 0xDD);

	/* Modifying clone shouldn't affect source */
	void *pd_mut = umem_deref(dst);
	memset(pd_mut, 0xEE, 64);
	munit_assert_uint8(((unsigned char *)ps)[0], ==, 0xDD);

	umem_drop(src);
	umem_drop(dst);
	return MUNIT_OK;
}

static MunitResult
test_borrow_unborrow(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_owned_t *o = umem_own_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(o);

	void *p = umem_deref(o);
	memset(p, 0x11, 64);

	umem_ref_t ref = umem_borrow(o);
	munit_assert_not_null(ref.ref_owner);
	munit_assert_not_null(ref.ref_data);
	munit_assert_int(ref.ref_mutable, ==, 0);

	/* Can read through borrow */
	munit_assert_uint8(((unsigned char *)ref.ref_data)[0], ==,
	    0x11);

	umem_unborrow(ref);

	/* After unborrow, can drop */
	umem_drop(o);
	return MUNIT_OK;
}

static MunitResult
test_borrow_mut(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_owned_t *o = umem_own_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(o);

	umem_ref_t ref = umem_borrow_mut(o);
	munit_assert_not_null(ref.ref_owner);
	munit_assert_int(ref.ref_mutable, ==, 1);

	/* Can write through mutable borrow */
	memset(ref.ref_data, 0x22, 64);

	umem_unborrow(ref);

	/* Verify write persisted */
	void *p = umem_deref(o);
	munit_assert_uint8(((unsigned char *)p)[0], ==, 0x22);

	umem_drop(o);
	return MUNIT_OK;
}

static MunitResult
test_drop_with_borrow(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_own_set_error_handler(test_error_handler);
	reset_errors();

	umem_owned_t *o = umem_own_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(o);

	umem_ref_t ref = umem_borrow(o);
	munit_assert_not_null(ref.ref_owner);

	/* Drop with outstanding borrow should error */
	umem_drop(o);
	munit_assert_int(last_error, ==, UMEM_OWN_BORROW_CONFLICT);
	munit_assert_int(error_count, ==, 1);

	/* Clean up: unborrow and drop for real */
	umem_unborrow(ref);
	umem_drop(o);

	umem_own_set_error_handler(NULL);
	return MUNIT_OK;
}

static MunitResult
test_multiple_shared_borrows(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_owned_t *o = umem_own_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(o);

	void *p = umem_deref(o);
	memset(p, 0x33, 64);

	/* Multiple shared borrows should work */
	umem_ref_t r1 = umem_borrow(o);
	umem_ref_t r2 = umem_borrow(o);
	umem_ref_t r3 = umem_borrow(o);

	munit_assert_not_null(r1.ref_owner);
	munit_assert_not_null(r2.ref_owner);
	munit_assert_not_null(r3.ref_owner);

	/* All should read same data */
	munit_assert_uint8(((unsigned char *)r1.ref_data)[0], ==,
	    0x33);
	munit_assert_uint8(((unsigned char *)r2.ref_data)[0], ==,
	    0x33);

	umem_unborrow(r3);
	umem_unborrow(r2);
	umem_unborrow(r1);
	umem_drop(o);

	return MUNIT_OK;
}

static MunitResult
test_move_with_borrow(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_own_set_error_handler(test_error_handler);
	reset_errors();

	umem_owned_t *o = umem_own_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(o);

	umem_ref_t ref = umem_borrow(o);
	munit_assert_not_null(ref.ref_owner);

	/* Move with outstanding borrow should fail */
	umem_owned_t *moved = umem_move(o);
	munit_assert_null(moved);
	munit_assert_int(last_error, ==, UMEM_OWN_BORROW_CONFLICT);

	umem_unborrow(ref);
	umem_drop(o);

	umem_own_set_error_handler(NULL);
	return MUNIT_OK;
}

static MunitResult
test_null_operations(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_own_set_error_handler(test_error_handler);
	reset_errors();

	/* NULL deref should report error */
	void *p = umem_deref(NULL);
	munit_assert_null(p);
	munit_assert_int(error_count, ==, 1);

	/* NULL drop is safe (no-op) */
	int prev_count = error_count;
	umem_drop(NULL);
	munit_assert_int(error_count, ==, prev_count);

	/* NULL unborrow is safe */
	umem_ref_t null_ref = { NULL, NULL, 0 };
	umem_unborrow(null_ref);
	munit_assert_int(error_count, ==, prev_count);

	/* NULL is_valid returns 0 */
	munit_assert_int(umem_own_is_valid(NULL), ==, 0);

	/* NULL state returns 0 */
	munit_assert_uint32(umem_own_state(NULL), ==, 0);

	umem_own_set_error_handler(NULL);
	return MUNIT_OK;
}

static MunitResult
test_various_sizes(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	size_t sizes[] = {
	    1, 7, 8, 15, 16, 31, 32, 63, 64,
	    128, 256, 512, 1024, 4096, 8192
	};
	int nsizes = sizeof(sizes) / sizeof(sizes[0]);

	for (int i = 0; i < nsizes; i++) {
		umem_owned_t *o = umem_own_alloc(sizes[i],
		    UMEM_DEFAULT);
		munit_assert_not_null(o);

		void *p = umem_deref(o);
		munit_assert_not_null(p);

		/* Verify alignment */
		uintptr_t addr = (uintptr_t)p;
		munit_assert_uint64(addr % 16, ==, 0);

		/* Write pattern and read back */
		memset(p, (int)(i & 0xFF), sizes[i]);
		unsigned char *bytes = (unsigned char *)p;
		munit_assert_uint8(bytes[sizes[i] - 1], ==,
		    (unsigned char)(i & 0xFF));

		umem_drop(o);
	}

	return MUNIT_OK;
}

static MunitResult
test_clone_null(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_own_set_error_handler(test_error_handler);
	reset_errors();

	umem_owned_t *dst = umem_clone(NULL, 64);
	munit_assert_null(dst);
	munit_assert_int(last_error, ==, UMEM_OWN_USE_AFTER_FREE);

	umem_own_set_error_handler(NULL);
	return MUNIT_OK;
}

static MunitResult
test_rapid_alloc_drop(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	for (int i = 0; i < 5000; i++) {
		umem_owned_t *o = umem_own_alloc(64, UMEM_DEFAULT);
		munit_assert_not_null(o);
		void *p = umem_deref(o);
		munit_assert_not_null(p);
		((char *)p)[0] = (char)i;
		umem_drop(o);
	}

	return MUNIT_OK;
}

static MunitResult
test_borrow_read_only(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_owned_t *o = umem_own_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(o);

	void *p = umem_deref(o);
	memset(p, 0x55, 64);

	/* Shared borrow returns non-mutable ref */
	umem_ref_t ref = umem_borrow(o);
	munit_assert_int(ref.ref_mutable, ==, 0);

	/* Verify data accessible */
	munit_assert_uint8(((unsigned char *)ref.ref_data)[63], ==,
	    0x55);

	umem_unborrow(ref);
	umem_drop(o);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Test array                                                         */
/* ------------------------------------------------------------------ */

static MunitTest own_tests[] = {
	{ "/alloc_drop", test_own_alloc_drop,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/deref_write_read", test_deref_write_read,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/deref_const", test_deref_const,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/use_after_drop", test_use_after_drop,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/double_drop", test_double_drop,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/move_transfer", test_move_transfer,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/clone", test_clone,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/borrow_unborrow", test_borrow_unborrow,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/borrow_mut", test_borrow_mut,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/drop_with_borrow", test_drop_with_borrow,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/multiple_shared_borrows", test_multiple_shared_borrows,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/move_with_borrow", test_move_with_borrow,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/null_operations", test_null_operations,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/various_sizes", test_various_sizes,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/clone_null", test_clone_null,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/rapid_alloc_drop", test_rapid_alloc_drop,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/borrow_read_only", test_borrow_read_only,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_umem_own = {
	"/umem_own",
	own_tests,
	NULL,
	1,
	MUNIT_SUITE_OPTION_NONE
};
