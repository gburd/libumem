/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * See the License file distributed with this work for details.
 *
 * CDDL HEADER END
 */

/*
 * Hash-based partitioning implementation
 * Extracted from hash_demo.c for production use
 */

#include "umem_hash_partition.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define UINT64_MAX_VAL UINT64_MAX

struct hash_partitions {
	uint64_t *lower_bounds;
	uint64_t *interval_sizes;
	char (*lb_to_c)[MAX_NAME_LEN];
	size_t count;
	size_t capacity;
};

static double
round_f64(double f, size_t decimal_digits)
{
	double factor = pow(10.0, (double)decimal_digits);
	return trunc((f * factor) + 0.5) / factor;
}

/*
 * FNV-1a hash algorithm
 * Fast, good distribution, simple implementation
 */
static uint64_t
simple_hash(const void *data, size_t len)
{
	const uint8_t *bytes = (const uint8_t *)data;
	uint64_t hash = 14695981039346656037ULL; /* FNV offset basis */

	for (size_t i = 0; i < len; i++) {
		hash ^= bytes[i];
		hash *= 1099511628211ULL; /* FNV prime */
	}

	return hash;
}

static hash_partitions_t *
hash_partitions_alloc(size_t capacity)
{
	hash_partitions_t *hp = malloc(sizeof (hash_partitions_t));
	if (!hp)
		return (NULL);

	hp->lower_bounds = malloc(capacity * sizeof (uint64_t));
	hp->interval_sizes = malloc(capacity * sizeof (uint64_t));
	hp->lb_to_c = malloc(capacity * sizeof (char[MAX_NAME_LEN]));

	if (!hp->lower_bounds || !hp->interval_sizes || !hp->lb_to_c) {
		free(hp->lower_bounds);
		free(hp->interval_sizes);
		free(hp->lb_to_c);
		free(hp);
		return (NULL);
	}

	hp->count = 0;
	hp->capacity = capacity;
	return (hp);
}

void
hash_partitions_free(hash_partitions_t *hp)
{
	if (hp) {
		free(hp->lower_bounds);
		free(hp->interval_sizes);
		free(hp->lb_to_c);
		free(hp);
	}
}

hash_partitions_t *
hash_partitions_create(const char **claimants, size_t num_claimants)
{
	if (num_claimants == 0)
		return (NULL);

	hash_partitions_t *hp = hash_partitions_alloc(num_claimants);
	if (!hp)
		return (NULL);

	uint64_t interval = UINT64_MAX_VAL / num_claimants;
	uint64_t next_lower_bound = 0;

	for (size_t i = 0; i < num_claimants; i++) {
		if (strlen(claimants[i]) == 0)
			continue;

		hp->lower_bounds[hp->count] = next_lower_bound;
		hp->interval_sizes[hp->count] = interval;
		strncpy(hp->lb_to_c[hp->count], claimants[i],
		    MAX_NAME_LEN - 1);
		hp->lb_to_c[hp->count][MAX_NAME_LEN - 1] = '\0';

		next_lower_bound += interval;
		hp->count++;
	}

	/* Adjust last interval to cover remaining space */
	if (hp->count > 0) {
		uint64_t sum = 0;
		for (size_t i = 0; i < hp->count; i++) {
			sum += hp->interval_sizes[i];
		}
		hp->interval_sizes[hp->count - 1] += (UINT64_MAX_VAL - sum);
	}

	return (hp);
}

hash_partitions_t *
hash_partitions_create_with_sizes(const claimant_size_t *sizes,
    size_t num_sizes)
{
	if (num_sizes == 0)
		return (NULL);

	hash_partitions_t *hp = hash_partitions_alloc(num_sizes);
	if (!hp)
		return (NULL);

	uint64_t next_lower_bound = 0;

	for (size_t i = 0; i < num_sizes; i++) {
		hp->lower_bounds[i] = next_lower_bound;
		hp->interval_sizes[i] = sizes[i].size;
		strncpy(hp->lb_to_c[i], sizes[i].name, MAX_NAME_LEN - 1);
		hp->lb_to_c[i][MAX_NAME_LEN - 1] = '\0';
		next_lower_bound += sizes[i].size;
		hp->count++;
	}

	/* Adjust last interval */
	uint64_t sum = 0;
	for (size_t i = 0; i < hp->count; i++) {
		sum += hp->interval_sizes[i];
	}
	if (hp->count > 0) {
		hp->interval_sizes[hp->count - 1] += (UINT64_MAX_VAL - sum);
	}

	return (hp);
}

hash_partitions_t *
hash_partitions_create_with_weights(const claimant_weight_t *weights,
    size_t num_weights, size_t decimal_digits)
{
	claimant_size_t *sizes = malloc(num_weights * sizeof (claimant_size_t));
	if (!sizes)
		return (NULL);

	double sum = 0.0;
	size_t valid_count = 0;

	/* Calculate sum and filter valid entries */
	for (size_t i = 0; i < num_weights; i++) {
		double w = round_f64(weights[i].weight, decimal_digits);
		if (strlen(weights[i].name) > 0 && w > 0.0) {
			strncpy(sizes[valid_count].name, weights[i].name,
			    MAX_NAME_LEN - 1);
			sizes[valid_count].name[MAX_NAME_LEN - 1] = '\0';
			sum += w;
			valid_count++;
		}
	}

	/* Convert weights to sizes */
	for (size_t i = 0; i < valid_count; i++) {
		double w = round_f64(weights[i].weight, decimal_digits);
		if (w == sum) {
			sizes[i].size = UINT64_MAX_VAL;
		} else {
			double fraction = w / sum;
			sizes[i].size = (uint64_t)((double)UINT64_MAX_VAL *
			    fraction);
		}
	}

	hash_partitions_t *result = hash_partitions_create_with_sizes(sizes,
	    valid_count);
	free(sizes);
	return (result);
}

const char *
hash_partitions_get_claimant(const hash_partitions_t *hp, uint64_t hash)
{
	if (!hp || hp->count == 0)
		return (NULL);

	if (hash >= hp->lower_bounds[hp->count - 1]) {
		return (hp->lb_to_c[hp->count - 1]);
	}

	/* Binary search: O(log n) */
	size_t left = 0;
	size_t right = hp->count - 1;

	while (left <= right) {
		size_t mid = left + (right - left) / 2;

		if (hash >= hp->lower_bounds[mid]) {
			if (mid == hp->count - 1 ||
			    hash < hp->lower_bounds[mid + 1]) {
				return (hp->lb_to_c[mid]);
			}
			left = mid + 1;
		} else {
			if (mid == 0)
				break;
			right = mid - 1;
		}
	}

	return (NULL);
}

const char *
hash_partitions_get_claimant_by_key(const hash_partitions_t *hp,
    const void *key, size_t key_len)
{
	uint64_t hash = simple_hash(key, key_len);
	return (hash_partitions_get_claimant(hp, hash));
}
