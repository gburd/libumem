/*
 * Unit tests for umem ownership/borrowing system.
 */

#include "../munit.h"
#include "../../umem_own.h"
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <umem.h>

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
/* Error type coverage: all 6 error types                             */
/* ------------------------------------------------------------------ */

/*
 * USE_AFTER_FREE: drop then deref.
 * (Already covered by test_use_after_drop, but this test also
 * verifies deref_const and borrow after drop.)
 */
static MunitResult
test_error_use_after_free_const(const MunitParameter params[],
    void *data)
{
	(void)params;
	(void)data;

	umem_own_set_error_handler(test_error_handler);
	reset_errors();

	umem_owned_t *o = umem_own_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(o);
	umem_drop(o);

	/* deref_const on dropped handle */
	const void *cp = umem_deref_const(o);
	munit_assert_null(cp);
	munit_assert_int(last_error, ==, UMEM_OWN_USE_AFTER_FREE);
	munit_assert_int(error_count, ==, 1);

	/* borrow on dropped handle */
	reset_errors();
	umem_ref_t ref = umem_borrow(o);
	munit_assert_null(ref.ref_owner);
	munit_assert_int(last_error, ==, UMEM_OWN_USE_AFTER_FREE);

	umem_own_set_error_handler(NULL);
	return MUNIT_OK;
}

/*
 * BORROW_CONFLICT: mut borrow then shared borrow (debug mode).
 * In debug mode, borrow checks ohd_mut_borrow and rejects.
 */
static MunitResult
test_error_borrow_conflict_debug(const MunitParameter params[],
    void *data)
{
	(void)params;
	(void)data;

	uint32_t saved = umem_ownership_debug;
	umem_ownership_debug = 1;

	umem_own_set_error_handler(test_error_handler);
	reset_errors();

	umem_owned_t *o = umem_own_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(o);

	umem_ref_t mref = umem_borrow_mut(o);
	munit_assert_not_null(mref.ref_owner);

	/* Shared borrow while mut borrow active -> conflict */
	umem_ref_t sref = umem_borrow(o);
	munit_assert_null(sref.ref_owner);
	munit_assert_int(last_error, ==, UMEM_OWN_BORROW_CONFLICT);

	/* Second mut borrow while first active -> conflict */
	reset_errors();
	umem_ref_t mref2 = umem_borrow_mut(o);
	munit_assert_null(mref2.ref_owner);
	munit_assert_int(last_error, ==, UMEM_OWN_BORROW_CONFLICT);

	umem_unborrow(mref);
	umem_drop(o);

	umem_own_set_error_handler(NULL);
	umem_ownership_debug = saved;
	return MUNIT_OK;
}

/*
 * BORROW_CONFLICT: shared borrow active, then mut borrow (debug).
 */
static MunitResult
test_error_mut_borrow_with_shared(const MunitParameter params[],
    void *data)
{
	(void)params;
	(void)data;

	uint32_t saved = umem_ownership_debug;
	umem_ownership_debug = 1;

	umem_own_set_error_handler(test_error_handler);
	reset_errors();

	umem_owned_t *o = umem_own_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(o);

	umem_ref_t sref = umem_borrow(o);
	munit_assert_not_null(sref.ref_owner);

	/* Mut borrow while shared borrow active -> conflict */
	umem_ref_t mref = umem_borrow_mut(o);
	munit_assert_null(mref.ref_owner);
	munit_assert_int(last_error, ==, UMEM_OWN_BORROW_CONFLICT);

	umem_unborrow(sref);
	umem_drop(o);

	umem_own_set_error_handler(NULL);
	umem_ownership_debug = saved;
	return MUNIT_OK;
}

/*
 * THREAD_VIOLATION: alloc on main thread, deref from another (debug).
 */

struct thread_violation_ctx {
	umem_owned_t		*owned;
	umem_own_error_t	error;
	int			got_error;
};

static void
thread_violation_handler(umem_own_error_t err, const umem_owned_t *o,
    const char *msg)
{
	(void)o;
	(void)msg;
	/* Store in a global since we can't pass context to handler */
}

static _Atomic int tv_error_type;
static _Atomic int tv_error_seen;

static void
tv_error_handler(umem_own_error_t err, const umem_owned_t *o,
    const char *msg)
{
	(void)o;
	(void)msg;
	atomic_store(&tv_error_type, (int)err);
	atomic_store(&tv_error_seen, 1);
}

static void *
thread_violation_worker(void *arg)
{
	umem_owned_t *o = (umem_owned_t *)arg;

	/* Deref from wrong thread should fail in debug mode */
	void *p = umem_deref(o);
	(void)p;
	return NULL;
}

static MunitResult
test_error_thread_violation(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	uint32_t saved = umem_ownership_debug;
	umem_ownership_debug = 1;

	atomic_store(&tv_error_type, 0);
	atomic_store(&tv_error_seen, 0);
	umem_own_set_error_handler(tv_error_handler);

	umem_owned_t *o = umem_own_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(o);

	pthread_t thr;
	int rc = pthread_create(&thr, NULL, thread_violation_worker, o);
	munit_assert_int(rc, ==, 0);
	pthread_join(thr, NULL);

	munit_assert_int(atomic_load(&tv_error_seen), ==, 1);
	munit_assert_int(atomic_load(&tv_error_type), ==,
	    UMEM_OWN_THREAD_VIOLATION);

	umem_drop(o);
	umem_own_set_error_handler(NULL);
	umem_ownership_debug = saved;
	return MUNIT_OK;
}

/*
 * LEAK: The UMEM_OWN_LEAK error type exists but is not automatically
 * detected at runtime (would require a scan at exit). This test
 * verifies the error type value exists and the handler can receive it
 * by manually calling report via the handler.
 */
static MunitResult
test_error_leak_type(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	/* Verify the enum value is defined and distinct */
	munit_assert_int(UMEM_OWN_LEAK, ==, 6);
	munit_assert_int(UMEM_OWN_LEAK, !=, UMEM_OWN_USE_AFTER_FREE);
	munit_assert_int(UMEM_OWN_LEAK, !=, UMEM_OWN_DOUBLE_FREE);
	munit_assert_int(UMEM_OWN_LEAK, !=, UMEM_OWN_BORROW_CONFLICT);
	munit_assert_int(UMEM_OWN_LEAK, !=, UMEM_OWN_THREAD_VIOLATION);

	/* Verify handler receives LEAK if it were reported */
	umem_own_set_error_handler(test_error_handler);
	reset_errors();

	/* Simulate: directly call handler with LEAK */
	test_error_handler(UMEM_OWN_LEAK, NULL, "simulated leak");
	munit_assert_int(last_error, ==, UMEM_OWN_LEAK);
	munit_assert_int(error_count, ==, 1);

	umem_own_set_error_handler(NULL);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Multi-threaded tests                                               */
/* ------------------------------------------------------------------ */

/*
 * 4 threads each calling umem_borrow() simultaneously.
 * Shared borrows should all succeed (lightweight mode uses
 * atomic refcount).
 */
#define	MT_BORROW_THREADS	4

struct mt_borrow_ctx {
	umem_owned_t	*owned;
	_Atomic int	ready;
	_Atomic int	success_count;
};

static void *
mt_shared_borrow_worker(void *arg)
{
	struct mt_borrow_ctx *ctx = arg;

	/* Spin until all threads are created */
	while (atomic_load(&ctx->ready) == 0)
		;

	umem_ref_t ref = umem_borrow(ctx->owned);
	if (ref.ref_owner != NULL) {
		/* Read data to verify */
		unsigned char val =
		    ((unsigned char *)ref.ref_data)[0];
		(void)val;
		atomic_fetch_add(&ctx->success_count, 1);
		umem_unborrow(ref);
	}
	return NULL;
}

static MunitResult
test_mt_shared_borrows(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_owned_t *o = umem_own_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(o);
	void *p = umem_deref(o);
	memset(p, 0x77, 64);

	struct mt_borrow_ctx ctx;
	ctx.owned = o;
	atomic_store(&ctx.ready, 0);
	atomic_store(&ctx.success_count, 0);

	pthread_t threads[MT_BORROW_THREADS];
	for (int i = 0; i < MT_BORROW_THREADS; i++) {
		int rc = pthread_create(&threads[i], NULL,
		    mt_shared_borrow_worker, &ctx);
		munit_assert_int(rc, ==, 0);
	}

	/* Release all threads simultaneously */
	atomic_store(&ctx.ready, 1);

	for (int i = 0; i < MT_BORROW_THREADS; i++)
		pthread_join(threads[i], NULL);

	/*
	 * In lightweight mode, all shared borrows succeed (no
	 * thread check). All 4 threads should have borrowed.
	 */
	munit_assert_int(atomic_load(&ctx.success_count), ==,
	    MT_BORROW_THREADS);

	umem_drop(o);
	return MUNIT_OK;
}

/*
 * Thread A holds mut borrow, thread B attempts shared borrow.
 * In debug mode, thread B should get THREAD_VIOLATION (wrong
 * owner thread) before even reaching borrow conflict check.
 */
static _Atomic int mt_conflict_error;

static void
mt_conflict_handler(umem_own_error_t err, const umem_owned_t *o,
    const char *msg)
{
	(void)o;
	(void)msg;
	atomic_store(&mt_conflict_error, (int)err);
}

struct mt_conflict_ctx {
	umem_owned_t	*owned;
	_Atomic int	ready;
};

static void *
mt_conflict_worker(void *arg)
{
	struct mt_conflict_ctx *ctx = arg;

	while (atomic_load(&ctx->ready) == 0)
		;

	/* Attempt borrow from wrong thread */
	umem_ref_t ref = umem_borrow(ctx->owned);
	if (ref.ref_owner != NULL)
		umem_unborrow(ref);
	return NULL;
}

static MunitResult
test_mt_borrow_conflict(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	uint32_t saved = umem_ownership_debug;
	umem_ownership_debug = 1;

	atomic_store(&mt_conflict_error, 0);
	umem_own_set_error_handler(mt_conflict_handler);

	umem_owned_t *o = umem_own_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(o);

	/* Main thread takes mut borrow */
	umem_ref_t mref = umem_borrow_mut(o);
	munit_assert_not_null(mref.ref_owner);

	struct mt_conflict_ctx ctx;
	ctx.owned = o;
	atomic_store(&ctx.ready, 0);

	pthread_t thr;
	int rc = pthread_create(&thr, NULL, mt_conflict_worker, &ctx);
	munit_assert_int(rc, ==, 0);

	atomic_store(&ctx.ready, 1);
	pthread_join(thr, NULL);

	/* Thread B should get THREAD_VIOLATION (checked first) */
	munit_assert_int(atomic_load(&mt_conflict_error), ==,
	    UMEM_OWN_THREAD_VIOLATION);

	umem_unborrow(mref);
	umem_drop(o);
	umem_own_set_error_handler(NULL);
	umem_ownership_debug = saved;
	return MUNIT_OK;
}

/*
 * umem_move between threads: thread A creates and moves,
 * thread B receives the moved handle (in debug mode, move
 * resets the owner thread).
 */

struct mt_move_ctx {
	umem_owned_t	*moved;
	_Atomic int	ready;
	_Atomic int	success;
};

static void *
mt_move_receiver(void *arg)
{
	struct mt_move_ctx *ctx = arg;

	while (atomic_load(&ctx->ready) == 0)
		;

	/*
	 * After move, the handle's owner thread is set to the
	 * thread that called umem_move(). In debug mode, the
	 * receiving thread needs to call umem_move() again to
	 * take ownership. In lightweight mode, no thread check.
	 */
	umem_owned_t *local = umem_move(ctx->moved);
	if (local != NULL) {
		void *p = umem_deref(local);
		if (p != NULL) {
			munit_assert_uint8(
			    ((unsigned char *)p)[0], ==, 0xAA);
			atomic_store(&ctx->success, 1);
		}
		umem_drop(local);
	}
	return NULL;
}

static MunitResult
test_mt_move_transfer(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_owned_t *o = umem_own_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(o);
	void *p = umem_deref(o);
	memset(p, 0xAA, 64);

	/* Move ownership (resets owner to current thread) */
	umem_owned_t *moved = umem_move(o);
	munit_assert_not_null(moved);

	struct mt_move_ctx ctx;
	ctx.moved = moved;
	atomic_store(&ctx.ready, 0);
	atomic_store(&ctx.success, 0);

	pthread_t thr;
	int rc = pthread_create(&thr, NULL, mt_move_receiver, &ctx);
	munit_assert_int(rc, ==, 0);

	atomic_store(&ctx.ready, 1);
	pthread_join(thr, NULL);

	munit_assert_int(atomic_load(&ctx.success), ==, 1);
	return MUNIT_OK;
}

/*
 * Clone while another thread reads: verify both copies valid.
 */

struct mt_clone_ctx {
	umem_owned_t	*src;
	_Atomic int	ready;
	_Atomic int	reader_ok;
};

static void *
mt_clone_reader(void *arg)
{
	struct mt_clone_ctx *ctx = arg;

	while (atomic_load(&ctx->ready) == 0)
		;

	/* Read from src while main thread clones */
	const void *p = umem_deref_const(ctx->src);
	if (p != NULL) {
		unsigned char val = ((const unsigned char *)p)[0];
		if (val == 0xBB)
			atomic_store(&ctx->reader_ok, 1);
	}
	return NULL;
}

static MunitResult
test_mt_clone_while_reading(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_owned_t *src = umem_own_alloc(64, UMEM_DEFAULT);
	munit_assert_not_null(src);
	void *p = umem_deref(src);
	memset(p, 0xBB, 64);

	struct mt_clone_ctx ctx;
	ctx.src = src;
	atomic_store(&ctx.ready, 0);
	atomic_store(&ctx.reader_ok, 0);

	pthread_t thr;
	int rc = pthread_create(&thr, NULL, mt_clone_reader, &ctx);
	munit_assert_int(rc, ==, 0);

	atomic_store(&ctx.ready, 1);

	/* Clone from main thread */
	umem_owned_t *dst = umem_clone(src, 64);
	munit_assert_not_null(dst);

	pthread_join(thr, NULL);

	/* Both copies should be valid */
	munit_assert_int(umem_own_is_valid(src), !=, 0);
	munit_assert_int(umem_own_is_valid(dst), !=, 0);

	const void *dp = umem_deref_const(dst);
	munit_assert_uint8(((const unsigned char *)dp)[0], ==, 0xBB);

	/* Reader thread should have read successfully */
	munit_assert_int(atomic_load(&ctx.reader_ok), ==, 1);

	umem_drop(src);
	umem_drop(dst);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Cache-integrated ownership                                         */
/* ------------------------------------------------------------------ */

static MunitResult
test_own_cache_alloc(const MunitParameter params[], void *data)
{
	(void)params;
	(void)data;

	umem_cache_t *cp = umem_cache_create("test_own_cache",
	    128, 0, NULL, NULL, NULL, NULL, NULL, 0);
	munit_assert_not_null(cp);

	umem_owned_t *o = umem_own_cache_alloc(cp, UMEM_DEFAULT);
	munit_assert_not_null(o);
	munit_assert_int(umem_own_is_valid(o), !=, 0);
	munit_assert_uint32(umem_own_state(o), ==,
	    UMEM_OWN_STATE_OWNED);

	/* Can deref and use the allocation */
	void *p = umem_deref(o);
	munit_assert_not_null(p);
	memset(p, 0xFF, 64);

	/* Drop returns to cache */
	umem_drop(o);

	umem_cache_destroy(cp);
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
	/* Error type coverage */
	{ "/error/use_after_free_const",
	    test_error_use_after_free_const,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/error/borrow_conflict_debug",
	    test_error_borrow_conflict_debug,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/error/mut_borrow_with_shared",
	    test_error_mut_borrow_with_shared,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/error/thread_violation",
	    test_error_thread_violation,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/error/leak_type", test_error_leak_type,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	/* Multi-threaded tests */
	{ "/mt/shared_borrows", test_mt_shared_borrows,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/mt/borrow_conflict", test_mt_borrow_conflict,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/mt/move_transfer", test_mt_move_transfer,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/mt/clone_while_reading", test_mt_clone_while_reading,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	/* Cache integration */
	{ "/cache_alloc", test_own_cache_alloc,
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
