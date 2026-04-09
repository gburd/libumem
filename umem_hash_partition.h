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
 * Hash-based partitioning for consistent mapping of keys to claimants.
 * Used for NUMA node selection without syscalls.
 */

#ifndef _UMEM_HASH_PARTITION_H
#define _UMEM_HASH_PARTITION_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_CLAIMANTS 1024
#define MAX_NAME_LEN 256

typedef struct {
	char name[MAX_NAME_LEN];
	double weight;
} claimant_weight_t;

typedef struct {
	char name[MAX_NAME_LEN];
	uint64_t size;
} claimant_size_t;

typedef struct hash_partitions hash_partitions_t;

/*
 * Create hash partitions with equal distribution
 */
hash_partitions_t *hash_partitions_create(const char **claimants,
    size_t num_claimants);

/*
 * Create hash partitions with weighted distribution
 * Weights are normalized and mapped to hash space proportionally
 */
hash_partitions_t *hash_partitions_create_with_weights(
    const claimant_weight_t *weights, size_t num_weights,
    size_t decimal_digits);

/*
 * Create hash partitions with explicit sizes
 */
hash_partitions_t *hash_partitions_create_with_sizes(
    const claimant_size_t *sizes, size_t num_sizes);

/*
 * Get claimant for a given hash value
 * Uses binary search: O(log n)
 */
const char *hash_partitions_get_claimant(const hash_partitions_t *hp,
    uint64_t hash);

/*
 * Hash a key and get its claimant
 * Uses FNV-1a hash algorithm
 */
const char *hash_partitions_get_claimant_by_key(const hash_partitions_t *hp,
    const void *key, size_t key_len);

/*
 * Free hash partition structure
 */
void hash_partitions_free(hash_partitions_t *hp);

#ifdef __cplusplus
}
#endif

#endif /* _UMEM_HASH_PARTITION_H */
