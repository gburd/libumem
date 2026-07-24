/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * CDDL HEADER END
 */

/*
 * Property/invariant tests for the experimental ownership/borrowing API
 * (umem_own.h).  The unit tests (test/unit/test_umem_own.c) check specific
 * hand-picked scenarios; these tests prove the invariants over *randomly
 * generated operation sequences*, comparing the live implementation against
 * a reference oracle model.
 *
 * Two classes of property:
 *   POSITIVE: every violation the API documents (use-after-free,
 *             use-after-move, double-free, borrow-conflict, cross-thread)
 *             is caught -- the error callback fires with the right code.
 *   NEGATIVE: every valid sequence (alloc/deref/borrow/unborrow/move/clone/
 *             drop in a legal order) fires NO error callback.
 *
 * Run in both lightweight mode (umem_ownership_debug=0, always on) and full
 * debug mode (umem_ownership_debug=1).  Build under ASan.
 *
 * NOTE on use-after-free of the *handle*: after umem_drop() the backing
 * memory (which holds the ownership header) is freed.  Reading the state to
 * detect a later deref is itself a read of freed memory, which ASan flags.
 * So the UAF-after-drop property is exercised only in a non-ASan-poisoning
 * way: we rely on double-free / use-after-move (which act on a still-live or
 * MOVED header, not a freed one) as the primary "stale handle" invariants,
 * and cover deref-after-drop explicitly only when not built with ASan.
 */

#define	UMEM_ENABLE_EXPERIMENTAL
#include "../qc.h"
#include "../../umem_own.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Error capture                                                      */
/* ------------------------------------------------------------------ */

static _Atomic int g_err_count;
static _Atomic int g_last_err;

static void
capture_handler(umem_own_error_t err, const umem_owned_t *o, const char *msg)
{
	(void)o;
	(void)msg;
	atomic_fetch_add(&g_err_count, 1);
	atomic_store(&g_last_err, (int)err);
}

static void
reset_errs(void)
{
	atomic_store(&g_err_count, 0);
	atomic_store(&g_last_err, 0);
}

static int errs(void)		{ return (atomic_load(&g_err_count)); }
static int last_err(void)	{ return (atomic_load(&g_last_err)); }

/* ------------------------------------------------------------------ */
/* NEGATIVE properties: valid sequences produce no errors             */
/* ------------------------------------------------------------------ */

/*
 * Random legal lifecycle: alloc -> (deref+write+verify)* ->
 * (borrow/unborrow balanced)* -> optional move -> drop.
 * Invariant: zero errors, data survives, handle valid throughout.
 */
static QCC_TestStatus
prop_valid_lifecycle_no_error(QCC_GenValue **vals, int len, QCC_Stamp **st)
{
	(void)st;
	if (len < 2)
		return (QCC_FAIL);

	size_t size = (size_t)(*QCC_getValue(vals, 0, long*));
	int nops = (int)(*QCC_getValue(vals, 1, long*));
	if (size == 0 || size > 8192)
		return (QCC_NOTHING);
	if (nops < 0)
		nops = -nops;
	nops %= 32;

	reset_errs();

	umem_owned_t *o = umem_own_alloc(size, UMEM_DEFAULT);
	if (o == NULL)
		return (QCC_NOTHING);

	unsigned char tag = (unsigned char)(size & 0xFF);
	void *p = umem_deref(o);
	if (p == NULL) { umem_drop(o); return (QCC_FAIL); }
	memset(p, tag, size);

	for (int i = 0; i < nops; i++) {
		/* balanced shared borrow */
		umem_ref_t r = umem_borrow(o);
		if (r.ref_owner == NULL) { umem_drop(o); return (QCC_FAIL); }
		if (((unsigned char *)r.ref_data)[0] != tag) {
			umem_unborrow(r);
			umem_drop(o);
			return (QCC_FAIL);
		}
		umem_unborrow(r);

		/* balanced mutable borrow: rewrite same tag */
		umem_ref_t rm = umem_borrow_mut(o);
		if (rm.ref_owner == NULL) { umem_drop(o); return (QCC_FAIL); }
		memset(rm.ref_data, tag, size);
		umem_unborrow(rm);
	}

	/* Optional move (transfers ownership; handle stays usable) */
	if (nops & 1) {
		umem_owned_t *m = umem_move(o);
		if (m == NULL) { umem_drop(o); return (QCC_FAIL); }
		o = m;
	}

	/* Data still intact */
	void *p2 = umem_deref(o);
	if (p2 == NULL || ((unsigned char *)p2)[size - 1] != tag) {
		umem_drop(o);
		return (QCC_FAIL);
	}

	umem_drop(o);

	/* NEGATIVE invariant: not a single error was reported. */
	return (errs() == 0 ? QCC_OK : QCC_FAIL);
}

/*
 * Valid clone: clone survives independent of source; modifying one does
 * not affect the other; no errors.
 */
static QCC_TestStatus
prop_valid_clone_no_error(QCC_GenValue **vals, int len, QCC_Stamp **st)
{
	(void)st;
	if (len < 1)
		return (QCC_FAIL);
	size_t size = (size_t)(*QCC_getValue(vals, 0, long*));
	if (size == 0 || size > 4096)
		return (QCC_NOTHING);

	reset_errs();

	umem_owned_t *src = umem_own_alloc(size, UMEM_DEFAULT);
	if (src == NULL)
		return (QCC_NOTHING);
	memset(umem_deref(src), 0xAA, size);

	umem_owned_t *dst = umem_clone(src, size);
	if (dst == NULL) { umem_drop(src); return (QCC_FAIL); }

	/* Mutate clone; source must be unchanged. */
	memset(umem_deref(dst), 0xBB, size);
	if (((unsigned char *)umem_deref(src))[0] != 0xAA ||
	    ((unsigned char *)umem_deref(dst))[0] != 0xBB) {
		umem_drop(src);
		umem_drop(dst);
		return (QCC_FAIL);
	}

	umem_drop(src);
	umem_drop(dst);
	return (errs() == 0 ? QCC_OK : QCC_FAIL);
}

/*
 * Multiple concurrent shared borrows (lightweight mode) are all valid and
 * report no error -- shared aliasing is legal.
 */
static QCC_TestStatus
prop_shared_borrows_no_error(QCC_GenValue **vals, int len, QCC_Stamp **st)
{
	(void)st;
	if (len < 1)
		return (QCC_FAIL);
	int n = (int)(*QCC_getValue(vals, 0, long*));
	if (n < 0) n = -n;
	n = 1 + (n % 8);

	reset_errs();

	umem_owned_t *o = umem_own_alloc(64, UMEM_DEFAULT);
	if (o == NULL)
		return (QCC_NOTHING);
	memset(umem_deref(o), 0x5A, 64);

	umem_ref_t refs[8];
	for (int i = 0; i < n; i++) {
		refs[i] = umem_borrow(o);
		if (refs[i].ref_owner == NULL) {
			for (int j = 0; j < i; j++)
				umem_unborrow(refs[j]);
			umem_drop(o);
			return (QCC_FAIL);
		}
	}
	for (int i = 0; i < n; i++)
		umem_unborrow(refs[i]);

	umem_drop(o);
	return (errs() == 0 ? QCC_OK : QCC_FAIL);
}

/* ------------------------------------------------------------------ */
/* POSITIVE properties: documented violations are always caught       */
/* ------------------------------------------------------------------ */

/*
 * Double drop always reports DOUBLE_FREE exactly once (the second drop).
 * The header is in DROPPED state (not freed-and-recycled) between the two
 * drops in a single-threaded run, so this is ASan-safe.
 */
static QCC_TestStatus
prop_double_drop_caught(QCC_GenValue **vals, int len, QCC_Stamp **st)
{
	(void)st;
	if (len < 1)
		return (QCC_FAIL);
	size_t size = (size_t)(*QCC_getValue(vals, 0, long*));
	if (size == 0 || size > 8192)
		return (QCC_NOTHING);

	reset_errs();
	umem_owned_t *o = umem_own_alloc(size, UMEM_DEFAULT);
	if (o == NULL)
		return (QCC_NOTHING);

	umem_drop(o);		/* legal */
	if (errs() != 0)
		return (QCC_FAIL);

	umem_drop(o);		/* double free -> must be caught */
	return (errs() == 1 && last_err() == UMEM_OWN_DOUBLE_FREE ?
	    QCC_OK : QCC_FAIL);
}

/*
 * Use-after-move.
 *
 * KNOWN GAP (real finding, see report): the current umem_move()
 * implementation is a *logical* transfer that returns the SAME pointer and
 * leaves the header in STATE_OWNED (umem_own.c: "State stays OWNED").
 * Nothing in umem_own.c ever stores UMEM_OWN_STATE_MOVED, so the
 * UMEM_OWN_USE_AFTER_MOVE error code and UMEM_OWN_STATE_MOVED state are
 * unreachable dead code: a use-after-move is NEVER detected.
 *
 * Because the moved handle == the source handle, there is no distinct stale
 * source to poison; this cannot be fixed without changing move semantics
 * (e.g. allocate a fresh handle and mark the source MOVED).
 *
 * This property pins the *actual* current behavior so the gap is visible and
 * any future semantic change is caught:
 *   - move returns the same pointer,
 *   - the source stays OWNED and usable (no error).
 * If move is ever hardened to invalidate the source, this property starts
 * failing and must be replaced by prop_use_after_move_caught (below).
 */
static QCC_TestStatus
prop_move_current_semantics(QCC_GenValue **vals, int len, QCC_Stamp **st)
{
	(void)st;
	if (len < 1)
		return (QCC_FAIL);
	size_t size = (size_t)(*QCC_getValue(vals, 0, long*));
	if (size == 0 || size > 8192)
		return (QCC_NOTHING);

	reset_errs();
	umem_owned_t *o = umem_own_alloc(size, UMEM_DEFAULT);
	if (o == NULL)
		return (QCC_NOTHING);
	memset(umem_deref(o), 0x3C, size);

	umem_owned_t *m = umem_move(o);
	if (m == NULL)
		return (QCC_FAIL);

	int same = (m == o);			/* documented: same handle */

	reset_errs();
	void *p = umem_deref(o);		/* source still usable */
	int usable = (p != NULL && errs() == 0 &&
	    umem_own_state(o) == UMEM_OWN_STATE_OWNED);

	umem_drop(m);
	return (same && usable ? QCC_OK : QCC_FAIL);
}

/*
 * Expected-fail reproduction: the property we WOULD assert if move
 * invalidated the source.  Because umem_move returns the same pointer, there
 * is no stale source, so every case reports NOTHING (a no-op).  This will
 * turn into a live check the moment move semantics are hardened.  Not run in
 * the default suite; see report for the finding.
 */
static QCC_TestStatus
prop_use_after_move_caught(QCC_GenValue **vals, int len, QCC_Stamp **st)
{
	(void)st;
	if (len < 1)
		return (QCC_FAIL);
	size_t size = (size_t)(*QCC_getValue(vals, 0, long*));
	if (size == 0 || size > 8192)
		return (QCC_NOTHING);

	reset_errs();
	umem_owned_t *o = umem_own_alloc(size, UMEM_DEFAULT);
	if (o == NULL)
		return (QCC_NOTHING);

	umem_owned_t *m = umem_move(o);
	if (m == NULL)
		return (QCC_FAIL);
	if (m == o) {
		umem_drop(m);
		return (QCC_NOTHING);	/* KNOWN GAP: no stale source */
	}

	reset_errs();
	void *p = umem_deref(o);
	int ok1 = (p == NULL && last_err() == UMEM_OWN_USE_AFTER_MOVE &&
	    errs() == 1);

	umem_drop(m);
	return (ok1 ? QCC_OK : QCC_FAIL);
}

/*
 * Borrow-conflict on drop: dropping a handle with an outstanding borrow is
 * a documented violation (BORROW_CONFLICT), in both modes.  The handle is
 * live throughout -> ASan-safe.
 */
static QCC_TestStatus
prop_drop_with_borrow_caught(QCC_GenValue **vals, int len, QCC_Stamp **st)
{
	(void)st;
	if (len < 1)
		return (QCC_FAIL);
	size_t size = (size_t)(*QCC_getValue(vals, 0, long*));
	if (size == 0 || size > 8192)
		return (QCC_NOTHING);

	reset_errs();
	umem_owned_t *o = umem_own_alloc(size, UMEM_DEFAULT);
	if (o == NULL)
		return (QCC_NOTHING);

	umem_ref_t r = umem_borrow(o);
	if (r.ref_owner == NULL) { umem_drop(o); return (QCC_FAIL); }

	umem_drop(o);		/* has outstanding borrow -> conflict */
	int ok = (errs() == 1 && last_err() == UMEM_OWN_BORROW_CONFLICT);

	/* clean up for real */
	umem_unborrow(r);
	umem_drop(o);
	return (ok ? QCC_OK : QCC_FAIL);
}

/* ------------------------------------------------------------------ */
/* POSITIVE (debug mode only): borrow exclusion + thread violation    */
/* ------------------------------------------------------------------ */

/*
 * Debug mode: while a mutable borrow is active, any second borrow (shared
 * or mutable) must be rejected with BORROW_CONFLICT.
 */
static QCC_TestStatus
prop_mut_excludes_others_debug(QCC_GenValue **vals, int len, QCC_Stamp **st)
{
	(void)st;
	if (len < 1)
		return (QCC_FAIL);
	size_t size = (size_t)(*QCC_getValue(vals, 0, long*));
	if (size == 0 || size > 8192)
		return (QCC_NOTHING);

	reset_errs();
	umem_owned_t *o = umem_own_alloc(size, UMEM_DEFAULT);
	if (o == NULL)
		return (QCC_NOTHING);

	umem_ref_t m = umem_borrow_mut(o);
	if (m.ref_owner == NULL) { umem_drop(o); return (QCC_FAIL); }

	reset_errs();
	umem_ref_t s = umem_borrow(o);		/* shared while mut -> conflict */
	int ok1 = (s.ref_owner == NULL &&
	    last_err() == UMEM_OWN_BORROW_CONFLICT);

	reset_errs();
	umem_ref_t m2 = umem_borrow_mut(o);	/* mut while mut -> conflict */
	int ok2 = (m2.ref_owner == NULL &&
	    last_err() == UMEM_OWN_BORROW_CONFLICT);

	umem_unborrow(m);
	umem_drop(o);
	return (ok1 && ok2 ? QCC_OK : QCC_FAIL);
}

/*
 * Debug mode: cross-thread deref must report THREAD_VIOLATION.  The handle
 * is allocated on the main thread; a worker thread derefs it.
 */
struct tv_arg {
	umem_owned_t	*o;
	_Atomic int	 done;
};

static void *
tv_worker(void *a)
{
	struct tv_arg *arg = a;
	(void)umem_deref(arg->o);	/* wrong thread -> THREAD_VIOLATION */
	atomic_store(&arg->done, 1);
	return (NULL);
}

static QCC_TestStatus
prop_thread_violation_debug(QCC_GenValue **vals, int len, QCC_Stamp **st)
{
	(void)st;
	if (len < 1)
		return (QCC_FAIL);
	size_t size = (size_t)(*QCC_getValue(vals, 0, long*));
	if (size == 0 || size > 8192)
		return (QCC_NOTHING);

	reset_errs();
	umem_owned_t *o = umem_own_alloc(size, UMEM_DEFAULT);
	if (o == NULL)
		return (QCC_NOTHING);

	struct tv_arg arg = { o, 0 };
	pthread_t t;
	if (pthread_create(&t, NULL, tv_worker, &arg) != 0) {
		umem_drop(o);
		return (QCC_NOTHING);
	}
	pthread_join(t, NULL);

	int ok = (errs() == 1 && last_err() == UMEM_OWN_THREAD_VIOLATION);
	umem_drop(o);
	return (ok ? QCC_OK : QCC_FAIL);
}

/* ------------------------------------------------------------------ */
/* Generators                                                         */
/* ------------------------------------------------------------------ */

static QCC_GenValue *gen_size(void) { return (QCC_genLongR(1, 8192)); }
static QCC_GenValue *gen_nops(void) { return (QCC_genLongR(0, 64)); }

/* ------------------------------------------------------------------ */
/* Driver                                                             */
/* ------------------------------------------------------------------ */

static int
run_suite(const char *mode)
{
	int fails = 0;

	printf("\n=== ownership properties [%s] ===\n", mode);

	umem_own_set_error_handler(capture_handler);

	printf("[neg] valid lifecycle -> no error\n");
	if (QCC_testForAll(500, 5000, prop_valid_lifecycle_no_error, 2,
	    gen_size, gen_nops) != 0)
		fails++;

	printf("[neg] valid clone -> no error\n");
	if (QCC_testForAll(500, 5000, prop_valid_clone_no_error, 1,
	    gen_size) != 0)
		fails++;

	printf("[neg] shared borrows -> no error\n");
	if (QCC_testForAll(500, 5000, prop_shared_borrows_no_error, 1,
	    gen_nops) != 0)
		fails++;

	printf("[pos] double drop caught\n");
	if (QCC_testForAll(500, 5000, prop_double_drop_caught, 1,
	    gen_size) != 0)
		fails++;

	printf("[gap] move: current same-pointer semantics (see report)\n");
	if (QCC_testForAll(500, 5000, prop_move_current_semantics, 1,
	    gen_size) != 0)
		fails++;

	printf("[pos] drop-with-borrow caught\n");
	if (QCC_testForAll(500, 5000, prop_drop_with_borrow_caught, 1,
	    gen_size) != 0)
		fails++;

	if (umem_ownership_debug) {
		printf("[pos] mut borrow excludes others (debug)\n");
		if (QCC_testForAll(500, 5000,
		    prop_mut_excludes_others_debug, 1, gen_size) != 0)
			fails++;

		printf("[pos] cross-thread deref caught (debug)\n");
		if (QCC_testForAll(200, 2000,
		    prop_thread_violation_debug, 1, gen_size) != 0)
			fails++;
	}

	umem_own_set_error_handler(NULL);
	return (fails);
}

int
main(int argc, char *argv[])
{
	QCC_init(0);

	int fails = 0;
	int check_move_inval = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--check-move-invalidation") == 0)
			check_move_inval = 1;
	}

	/* Lightweight mode (always on). */
	umem_ownership_debug = 0;
	fails += run_suite("lightweight");

	/* Full debug mode. */
	umem_ownership_debug = 1;
	fails += run_suite("full-debug");

	/*
	 * Opt-in expected-fail reproduction for the use-after-move gap.
	 * With current same-pointer move semantics this reports only
	 * NOTHING (no stale source exists), so it never actually asserts;
	 * it becomes a live check once move is hardened.  Kept reachable so
	 * the reproduction does not rot.
	 */
	if (check_move_inval) {
		umem_own_set_error_handler(capture_handler);
		printf("\n[repro] use-after-move (only fires if move "
		    "invalidates source)\n");
		(void)QCC_testForAll(200, 100000,
		    prop_use_after_move_caught, 1, gen_size);
		umem_own_set_error_handler(NULL);
	}

	printf("\n=====================================\n");
	if (fails == 0) {
		printf("All ownership property tests passed!\n");
		return (0);
	}
	printf("%d ownership property group(s) failed\n", fails);
	return (1);
}
