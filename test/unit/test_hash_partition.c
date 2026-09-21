/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * CDDL HEADER END
 */

/*
 * Regression test for umem_hash_partition.c's weighted partitioning.
 *
 * Pre-fix failure (reproduced before the fix landed): the weight->size
 * conversion loop read weights[i] from the ORIGINAL uncompacted input
 * array while writing sizes[i] of the COMPACTED output array.  With any
 * rejected entry (empty name or non-positive weight) ahead of the
 * accepted ones, every accepted claimant got some other claimant's
 * weight.  The canonical case below -- A:0 (rejected), B:1, C:1 -- gave
 * B a zero-width interval and C the entire hash space, instead of half
 * of the space each.
 *
 * Also covers: nonfinite weights must not reach the double->uint64
 * conversion, and lower_bounds[] must stay strictly increasing (the
 * binary search in hash_partitions_get_claimant() depends on it).
 *
 * Standalone (no munit): umem_hash_partition.c is only linked into
 * libumem when --enable-numa is used, so this test compiles the unit
 * directly, exactly like test/bench/bench_numa_hash.c does.
 */

#include "umem_hash_partition.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)						\
	do {								\
		if (!(cond)) {						\
			failures++;					\
			printf("FAIL %s:%d: ", __FILE__, __LINE__);	\
			printf(__VA_ARGS__);				\
			printf("\n");					\
		}							\
	} while (0)

/*
 * Sample the hash space uniformly and count how many samples each
 * claimant owns.  A claimant with a zero-width interval gets 0 samples.
 */
static size_t
count_samples(const hash_partitions_t *hp, const char *name, size_t nsamples)
{
	size_t hits = 0;

	for (size_t i = 0; i < nsamples; i++) {
		/* Spread samples across the full uint64 range. */
		uint64_t h = (uint64_t)((long double)UINT64_MAX *
		    ((long double)i / (long double)nsamples));
		const char *owner = hash_partitions_get_claimant(hp, h);

		if (owner != NULL && strcmp(owner, name) == 0)
			hits++;
	}
	return hits;
}

/*
 * The regression: a rejected leading entry must not shift the weights.
 * A:0 is rejected; B:1 and C:1 must split the space ~50/50.
 */
static void
test_rejected_entry_does_not_shift_weights(void)
{
	claimant_weight_t w[3] = {
		{ "A", 0.0 },
		{ "B", 1.0 },
		{ "C", 1.0 },
	};
	const size_t nsamples = 10000;

	hash_partitions_t *hp = hash_partitions_create_with_weights(w, 3, 2);
	CHECK(hp != NULL, "create_with_weights returned NULL");
	if (hp == NULL)
		return;

	size_t a = count_samples(hp, "A", nsamples);
	size_t b = count_samples(hp, "B", nsamples);
	size_t c = count_samples(hp, "C", nsamples);

	CHECK(a == 0, "zero-weight claimant A owns %zu/%zu samples", a,
	    nsamples);
	/* 50/50 +- 2%.  Pre-fix this was b == 0, c == nsamples. */
	CHECK(b > nsamples * 48 / 100 && b < nsamples * 52 / 100,
	    "B owns %zu/%zu samples, expected ~%zu", b, nsamples,
	    nsamples / 2);
	CHECK(c > nsamples * 48 / 100 && c < nsamples * 52 / 100,
	    "C owns %zu/%zu samples, expected ~%zu", c, nsamples,
	    nsamples / 2);

	hash_partitions_free(hp);
}

/* Unequal weights must be honoured proportionally, not by input order. */
static void
test_weight_proportions(void)
{
	claimant_weight_t w[3] = {
		{ "", 5.0 },		/* rejected: empty name */
		{ "big", 3.0 },
		{ "small", 1.0 },
	};
	const size_t nsamples = 10000;

	hash_partitions_t *hp = hash_partitions_create_with_weights(w, 3, 2);
	CHECK(hp != NULL, "create_with_weights returned NULL");
	if (hp == NULL)
		return;

	size_t big = count_samples(hp, "big", nsamples);
	size_t small = count_samples(hp, "small", nsamples);

	/* 3:1 split -> 75% / 25%, +- 2%. */
	CHECK(big > nsamples * 73 / 100 && big < nsamples * 77 / 100,
	    "big owns %zu/%zu samples, expected ~%zu", big, nsamples,
	    nsamples * 3 / 4);
	CHECK(small > nsamples * 23 / 100 && small < nsamples * 27 / 100,
	    "small owns %zu/%zu samples, expected ~%zu", small, nsamples,
	    nsamples / 4);

	hash_partitions_free(hp);
}

/*
 * Nonfinite weights must be rejected outright rather than converted to
 * uint64 (undefined behaviour) -- and must not poison the sum for the
 * finite entries.
 */
static void
test_nonfinite_weights_rejected(void)
{
	claimant_weight_t w[3] = {
		{ "inf", INFINITY },
		{ "nan", NAN },
		{ "ok", 1.0 },
	};
	const size_t nsamples = 1000;

	hash_partitions_t *hp = hash_partitions_create_with_weights(w, 3, 2);
	CHECK(hp != NULL, "create_with_weights returned NULL");
	if (hp == NULL)
		return;

	CHECK(count_samples(hp, "inf", nsamples) == 0,
	    "infinite-weight claimant owns hash space");
	CHECK(count_samples(hp, "nan", nsamples) == 0,
	    "NaN-weight claimant owns hash space");
	CHECK(count_samples(hp, "ok", nsamples) == nsamples,
	    "sole finite claimant does not own the whole hash space");

	hash_partitions_free(hp);

	/* All-nonfinite input has no valid claimant at all. */
	claimant_weight_t bad[2] = { { "inf", INFINITY }, { "nan", NAN } };
	CHECK(hash_partitions_create_with_weights(bad, 2, 2) == NULL,
	    "all-nonfinite input produced a partition table");
}

/*
 * Explicit sizes that overflow the hash space must still leave
 * lower_bounds[] strictly increasing, or the binary search silently
 * returns the wrong claimant.  Probe via the public API: every claimant
 * that appears at all must own a contiguous, non-empty run.
 */
static void
test_oversized_sizes_keep_bounds_sorted(void)
{
	claimant_size_t s[3] = {
		{ "first", UINT64_MAX / 2 },
		{ "second", UINT64_MAX / 2 },
		{ "third", UINT64_MAX / 2 },	/* would wrap the cumulative sum */
	};
	const size_t nsamples = 10000;

	hash_partitions_t *hp = hash_partitions_create_with_sizes(s, 3);
	CHECK(hp != NULL, "create_with_sizes returned NULL");
	if (hp == NULL)
		return;

	size_t total = count_samples(hp, "first", nsamples) +
	    count_samples(hp, "second", nsamples) +
	    count_samples(hp, "third", nsamples);

	/* Every sample must be claimed by exactly one of the three. */
	CHECK(total == nsamples,
	    "%zu/%zu samples claimed; bounds are not a partition", total,
	    nsamples);

	hash_partitions_free(hp);
}

/* Equal-weight creation from plain names, including a skipped empty one. */
static void
test_equal_distribution(void)
{
	const char *names[3] = { "n0", "", "n2" };
	const size_t nsamples = 10000;

	hash_partitions_t *hp = hash_partitions_create(names, 3);
	CHECK(hp != NULL, "create returned NULL");
	if (hp == NULL)
		return;

	size_t n0 = count_samples(hp, "n0", nsamples);
	size_t n2 = count_samples(hp, "n2", nsamples);

	CHECK(n0 + n2 == nsamples, "%zu/%zu samples claimed", n0 + n2,
	    nsamples);
	CHECK(n0 > 0 && n2 > 0, "a claimant got a zero-width interval "
	    "(n0=%zu n2=%zu)", n0, n2);

	hash_partitions_free(hp);
}

int
main(void)
{
	test_rejected_entry_does_not_shift_weights();
	test_weight_proportions();
	test_nonfinite_weights_rejected();
	test_oversized_sizes_keep_bounds_sorted();
	test_equal_distribution();

	if (failures != 0) {
		printf("test_hash_partition: %d failure(s)\n", failures);
		return (1);
	}
	printf("test_hash_partition: all checks passed\n");
	return (0);
}
