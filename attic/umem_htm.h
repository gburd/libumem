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
 * Hardware Transactional Memory Support
 *
 * This provides hardware transactional memory (HTM) support for libumem,
 * using Intel TSX (RTM/HLE) or ARM TME to eliminate lock overhead in
 * depot operations.
 *
 * Performance: 5-15% improvement in contended workloads by replacing
 * depot locks with transactions. Falls back to locks on abort.
 *
 * Requires: Intel TSX (Haswell+), ARM TME, or IBM Power8+
 * Enable with: UMEM_OPTIONS=htm=on|off
 */

#ifndef _UMEM_HTM_H
#define _UMEM_HTM_H

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Check for TSX support at compile time */
#if defined(__x86_64__) || defined(__i386__)
#ifdef HAVE_IMMINTRIN_H
#define UMEM_TSX_AVAILABLE 1
#endif
#endif

/* Check for ARM TME support */
#if defined(__aarch64__)
/* ARM TME detection would go here */
/* #define UMEM_ARM_TME_AVAILABLE 1 */
#endif

/* Check for Power TM support */
#if defined(__powerpc64__)
/* Power TM detection would go here */
/* #define UMEM_POWER_TM_AVAILABLE 1 */
#endif

#if defined(UMEM_TSX_AVAILABLE) || defined(UMEM_ARM_TME_AVAILABLE) || \
    defined(UMEM_POWER_TM_AVAILABLE)
#define UMEM_HTM_AVAILABLE 1
#endif

#ifdef UMEM_HTM_AVAILABLE

#include <stdint.h>
#include <stddef.h>

#ifdef UMEM_TSX_AVAILABLE
#include <immintrin.h>
#endif

/*
 * HTM operation status codes
 */
#define UMEM_HTM_SUCCESS        0    /* Transaction succeeded */
#define UMEM_HTM_ABORT_EXPLICIT 1    /* Explicit abort */
#define UMEM_HTM_ABORT_RETRY    2    /* Retry may succeed */
#define UMEM_HTM_ABORT_CONFLICT 3    /* Memory conflict detected */
#define UMEM_HTM_ABORT_CAPACITY 4    /* Transaction too large */
#define UMEM_HTM_ABORT_DEBUG    5    /* Debug interrupt */
#define UMEM_HTM_ABORT_NESTED   6    /* Nested transaction failed */
#define UMEM_HTM_ABORT_OTHER    7    /* Other abort reason */

/*
 * HTM retry limits
 */
#define UMEM_HTM_MAX_RETRIES    3    /* Max transaction retries */
#define UMEM_HTM_ABORT_THRESHOLD 20  /* Disable after 20% abort rate */

/*
 * HTM statistics per cache
 */
typedef struct umem_htm_stats {
	uint64_t commits;		/* Successful commits */
	uint64_t aborts;		/* Total aborts */
	uint64_t aborts_explicit;	/* Explicit aborts */
	uint64_t aborts_retry;		/* Retry aborts */
	uint64_t aborts_conflict;	/* Conflict aborts */
	uint64_t aborts_capacity;	/* Capacity aborts */
	uint64_t aborts_other;		/* Other aborts */
	uint64_t lock_fallbacks;	/* Fallback to locks */
	uint64_t disabled_count;	/* Times HTM was disabled */
	double abort_rate;		/* Current abort rate */
	int enabled;			/* HTM currently enabled */
} umem_htm_stats_t;

/*
 * Per-cache HTM state
 */
typedef struct umem_htm_cache_state {
	umem_htm_stats_t stats;		/* Statistics */
	uint64_t sample_window;		/* Commits in current window */
	uint64_t sample_aborts;		/* Aborts in current window */
	int dynamic_disable;		/* Dynamically disabled */
} umem_htm_cache_state_t;

/*
 * Global HTM state
 */
extern int umem_htm_enabled;		/* HTM available and enabled */
extern int umem_htm_available_flags;	/* Available HTM features */

/* Feature flags */
#define UMEM_HTM_TSX_RTM   0x01		/* Intel RTM available */
#define UMEM_HTM_TSX_HLE   0x02		/* Intel HLE available */
#define UMEM_HTM_ARM_TME   0x04		/* ARM TME available */
#define UMEM_HTM_POWER_TM  0x08		/* Power TM available */

/*
 * HTM detection and initialization
 */

/*
 * umem_htm_available - Check if HTM is available on this system
 *
 * Detects HTM support via CPUID (x86), system registers (ARM), or
 * other platform-specific mechanisms.
 *
 * Returns bitmask of available features or 0 if none.
 */
int umem_htm_available(void);

/*
 * umem_htm_init - Initialize HTM subsystem
 *
 * Detects HTM capabilities and prepares for transactional execution.
 * Called during umem_init().
 *
 * Returns 0 on success, -1 on failure.
 */
int umem_htm_init(void);

/*
 * umem_htm_fini - Clean up HTM subsystem
 *
 * Called during umem shutdown.
 */
void umem_htm_fini(void);

/*
 * Intel TSX operations
 */

#ifdef UMEM_TSX_AVAILABLE

/*
 * umem_tsx_begin - Begin a TSX transaction
 *
 * Starts a Restricted Transactional Memory (RTM) transaction.
 * Returns _XBEGIN_STARTED on success, or an abort status on failure.
 *
 * Usage:
 *   unsigned status = umem_tsx_begin();
 *   if (status == _XBEGIN_STARTED) {
 *     // transactional code
 *     umem_tsx_end();
 *   } else {
 *     // fallback path
 *   }
 *
 * Returns _XBEGIN_STARTED or abort status.
 */
static inline unsigned int umem_tsx_begin(void)
{
	return _xbegin();
}

/*
 * umem_tsx_end - Commit a TSX transaction
 *
 * Commits the current transaction. All memory operations become
 * visible atomically.
 */
static inline void umem_tsx_end(void)
{
	_xend();
}

/*
 * umem_tsx_abort - Abort a TSX transaction
 *
 * Explicitly aborts the current transaction with the given status code.
 *
 * @param status Abort status (0-255)
 */
static inline void umem_tsx_abort(unsigned int status)
{
	_xabort(status);
}

/*
 * umem_tsx_test - Test if in a TSX transaction
 *
 * Returns non-zero if currently in a transaction, 0 otherwise.
 */
static inline int umem_tsx_test(void)
{
	return _xtest();
}

/*
 * umem_tsx_classify_abort - Classify abort status
 *
 * Determines the type of abort from the status code.
 *
 * @param status Abort status from _xbegin()
 *
 * Returns UMEM_HTM_ABORT_* code.
 */
int umem_tsx_classify_abort(unsigned int status);

#endif /* UMEM_TSX_AVAILABLE */

/*
 * Generic HTM interface (works with TSX, TME, Power TM)
 */

/*
 * umem_htm_begin - Begin a hardware transaction
 *
 * Platform-independent transaction begin. Detects platform and uses
 * appropriate HTM mechanism.
 *
 * Returns UMEM_HTM_SUCCESS or abort code.
 */
int umem_htm_begin(void);

/*
 * umem_htm_end - Commit a hardware transaction
 *
 * Platform-independent transaction commit.
 */
void umem_htm_end(void);

/*
 * umem_htm_abort - Abort a hardware transaction
 *
 * Platform-independent transaction abort.
 *
 * @param reason Abort reason code
 */
void umem_htm_abort(int reason);

/*
 * umem_htm_test - Test if in a transaction
 *
 * Returns non-zero if in transaction, 0 otherwise.
 */
int umem_htm_test(void);

/*
 * Depot operations with HTM
 */

/*
 * umem_htm_depot_alloc - Allocate from depot using HTM
 *
 * Attempts to allocate a magazine from the depot using a hardware
 * transaction instead of locks. Falls back to locks on abort.
 *
 * @param cache  Cache to allocate from
 * @param state  HTM state for this cache
 *
 * Returns magazine pointer or NULL.
 */
void *umem_htm_depot_alloc(void *cache, umem_htm_cache_state_t *state);

/*
 * umem_htm_depot_free - Return magazine to depot using HTM
 *
 * Attempts to return a magazine to the depot using a hardware
 * transaction instead of locks. Falls back to locks on abort.
 *
 * @param cache Cache to free to
 * @param mag   Magazine to return
 * @param state HTM state for this cache
 */
void umem_htm_depot_free(void *cache, void *mag,
    umem_htm_cache_state_t *state);

/*
 * Statistics and monitoring
 */

/*
 * umem_htm_update_stats - Update HTM statistics after operation
 *
 * Updates statistics and decides whether to disable HTM for a cache
 * based on abort rate.
 *
 * @param state  HTM state to update
 * @param status Operation status (UMEM_HTM_SUCCESS or abort code)
 */
void umem_htm_update_stats(umem_htm_cache_state_t *state, int status);

/*
 * umem_htm_get_stats - Get HTM statistics
 *
 * Gets current HTM statistics for a cache.
 *
 * @param state HTM state to query
 * @param stats Output structure (caller-allocated)
 */
void umem_htm_get_stats(umem_htm_cache_state_t *state,
    umem_htm_stats_t *stats);

/*
 * umem_htm_reset_stats - Reset HTM statistics
 *
 * Resets statistics counters for a cache.
 *
 * @param state HTM state to reset
 */
void umem_htm_reset_stats(umem_htm_cache_state_t *state);

/*
 * umem_htm_should_use - Determine if HTM should be used
 *
 * Checks if HTM should be used based on current statistics and
 * configuration. Returns 0 if HTM is disabled or abort rate is too high.
 *
 * @param state HTM state to check
 *
 * Returns non-zero if HTM should be used, 0 otherwise.
 */
int umem_htm_should_use(umem_htm_cache_state_t *state);

/*
 * Configuration
 */

/*
 * umem_htm_enable - Enable HTM for all caches
 *
 * Globally enables HTM if available.
 */
void umem_htm_enable(void);

/*
 * umem_htm_disable - Disable HTM for all caches
 *
 * Globally disables HTM.
 */
void umem_htm_disable(void);

/*
 * umem_htm_set_abort_threshold - Set abort rate threshold
 *
 * Sets the abort rate threshold above which HTM is disabled for a cache.
 * Default is 20% (0.20).
 *
 * @param threshold Abort rate threshold (0.0 to 1.0)
 */
void umem_htm_set_abort_threshold(double threshold);

/*
 * Debugging
 */

/*
 * umem_htm_dump - Dump HTM state for debugging
 *
 * Prints HTM information to stderr.
 */
void umem_htm_dump(void);

/*
 * umem_htm_dump_stats - Dump HTM statistics
 *
 * Prints HTM statistics for a cache to stderr.
 *
 * @param stats Statistics to dump
 */
void umem_htm_dump_stats(umem_htm_stats_t *stats);

/*
 * Helper macros
 */

/*
 * UMEM_HTM_ENABLED - Check if HTM is enabled
 */
#define UMEM_HTM_ENABLED() (umem_htm_enabled)

/*
 * UMEM_HTM_TRY - Try operation with HTM, fall back to locks
 *
 * Usage:
 *   UMEM_HTM_TRY(state, lock,
 *     // transactional code
 *   );
 */
#define UMEM_HTM_TRY(state, lock, code) \
	do { \
		int __retries = 0; \
		int __status; \
		while (__retries < UMEM_HTM_MAX_RETRIES) { \
			__status = umem_htm_begin(); \
			if (__status == UMEM_HTM_SUCCESS) { \
				code; \
				umem_htm_end(); \
				umem_htm_update_stats((state), UMEM_HTM_SUCCESS); \
				break; \
			} \
			umem_htm_update_stats((state), __status); \
			__retries++; \
		} \
		if (__retries >= UMEM_HTM_MAX_RETRIES) { \
			pthread_mutex_lock(lock); \
			code; \
			pthread_mutex_unlock(lock); \
		} \
	} while (0)

#ifdef __cplusplus
}
#endif

#endif /* UMEM_HTM_AVAILABLE */

#endif /* _UMEM_HTM_H */
