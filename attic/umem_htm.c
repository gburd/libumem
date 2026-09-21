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

#include "config.h"
#include "umem_htm.h"

#ifdef UMEM_HTM_AVAILABLE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "umem_impl.h"

/* Global state */
int umem_htm_enabled = 0;
int umem_htm_available_flags = 0;

/* Configuration */
static double umem_htm_abort_threshold = 0.20; /* 20% abort rate */

#ifdef UMEM_TSX_AVAILABLE

/*
 * Detect Intel TSX support via CPUID
 */
static int
detect_tsx_support(void)
{
	unsigned int eax, ebx, ecx, edx;
	int flags = 0;

#if defined(__x86_64__) || defined(__i386__)
	/* Check for RTM (bit 11 of EBX in leaf 7) */
	eax = 7;
	ecx = 0;
	__asm__ __volatile__(
		"cpuid"
		: "=a" (eax), "=b" (ebx), "=c" (ecx), "=d" (edx)
		: "0" (eax), "2" (ecx)
	);

	if (ebx & (1 << 11)) {
		flags |= UMEM_HTM_TSX_RTM;
	}

	/* Check for HLE (bit 4 of EBX in leaf 7) */
	if (ebx & (1 << 4)) {
		flags |= UMEM_HTM_TSX_HLE;
	}
#endif

	return flags;
}

int
umem_tsx_classify_abort(unsigned int status)
{
	if (status & _XABORT_EXPLICIT) {
		return UMEM_HTM_ABORT_EXPLICIT;
	}
	if (status & _XABORT_RETRY) {
		return UMEM_HTM_ABORT_RETRY;
	}
	if (status & _XABORT_CONFLICT) {
		return UMEM_HTM_ABORT_CONFLICT;
	}
	if (status & _XABORT_CAPACITY) {
		return UMEM_HTM_ABORT_CAPACITY;
	}
	if (status & _XABORT_DEBUG) {
		return UMEM_HTM_ABORT_DEBUG;
	}
	return UMEM_HTM_ABORT_OTHER;
}

#endif /* UMEM_TSX_AVAILABLE */

int
umem_htm_available(void)
{
	int flags = 0;

#ifdef UMEM_TSX_AVAILABLE
	flags |= detect_tsx_support();
#endif

#ifdef UMEM_ARM_TME_AVAILABLE
	/* ARM TME detection would go here */
#endif

#ifdef UMEM_POWER_TM_AVAILABLE
	/* Power TM detection would go here */
#endif

	return flags;
}

int
umem_htm_init(void)
{
	umem_htm_available_flags = umem_htm_available();

	if (umem_htm_available_flags == 0) {
		return -1;
	}

	umem_htm_enabled = 1;
	return 0;
}

void
umem_htm_fini(void)
{
	umem_htm_enabled = 0;
	umem_htm_available_flags = 0;
}

int
umem_htm_begin(void)
{
#ifdef UMEM_TSX_AVAILABLE
	if (umem_htm_available_flags & UMEM_HTM_TSX_RTM) {
		unsigned int status = _xbegin();
		if (status == _XBEGIN_STARTED) {
			return UMEM_HTM_SUCCESS;
		}
		return umem_tsx_classify_abort(status);
	}
#endif

	return UMEM_HTM_ABORT_OTHER;
}

void
umem_htm_end(void)
{
#ifdef UMEM_TSX_AVAILABLE
	if (umem_htm_available_flags & UMEM_HTM_TSX_RTM) {
		_xend();
		return;
	}
#endif
}

void
umem_htm_abort(int reason)
{
#ifdef UMEM_TSX_AVAILABLE
	if (umem_htm_available_flags & UMEM_HTM_TSX_RTM) {
		_xabort((unsigned int)reason);
		return;
	}
#endif
}

int
umem_htm_test(void)
{
#ifdef UMEM_TSX_AVAILABLE
	if (umem_htm_available_flags & UMEM_HTM_TSX_RTM) {
		return _xtest();
	}
#endif
	return 0;
}

void
umem_htm_update_stats(umem_htm_cache_state_t *state, int status)
{
	if (state == NULL) {
		return;
	}

	if (status == UMEM_HTM_SUCCESS) {
		state->stats.commits++;
		state->sample_window++;
	} else {
		state->stats.aborts++;
		state->sample_aborts++;

		switch (status) {
		case UMEM_HTM_ABORT_EXPLICIT:
			state->stats.aborts_explicit++;
			break;
		case UMEM_HTM_ABORT_RETRY:
			state->stats.aborts_retry++;
			break;
		case UMEM_HTM_ABORT_CONFLICT:
			state->stats.aborts_conflict++;
			break;
		case UMEM_HTM_ABORT_CAPACITY:
			state->stats.aborts_capacity++;
			break;
		default:
			state->stats.aborts_other++;
			break;
		}
	}

	/* Update abort rate every 1000 operations */
	if (state->sample_window + state->sample_aborts >= 1000) {
		double total = (double)(state->sample_window + state->sample_aborts);
		state->stats.abort_rate = (double)state->sample_aborts / total;

		/* Check if we should disable HTM */
		if (state->stats.abort_rate > umem_htm_abort_threshold &&
		    !state->dynamic_disable) {
			state->dynamic_disable = 1;
			state->stats.disabled_count++;
		}

		/* Reset sample window */
		state->sample_window = 0;
		state->sample_aborts = 0;
	}
}

int
umem_htm_should_use(umem_htm_cache_state_t *state)
{
	if (!umem_htm_enabled) {
		return 0;
	}

	if (state == NULL) {
		return 1; /* No state, use HTM */
	}

	if (state->dynamic_disable) {
		return 0;
	}

	return state->stats.enabled;
}

void *
umem_htm_depot_alloc(void *cache, umem_htm_cache_state_t *state)
{
	umem_cache_t *cp = (umem_cache_t *)cache;
	void *mag = NULL;
	int retries = 0;
	int status;

	if (!umem_htm_should_use(state)) {
		/* Fall back to regular lock-based allocation */
		state->stats.lock_fallbacks++;
		return NULL; /* Caller uses normal path */
	}

	/* Try with HTM */
	while (retries < UMEM_HTM_MAX_RETRIES) {
		status = umem_htm_begin();
		if (status == UMEM_HTM_SUCCESS) {
			/*
			 * Transactional code - access depot without lock
			 * This is a simplified example; real implementation
			 * would access depot structures directly.
			 */

			/* Verify no lock is held (would abort) */
			if (umem_htm_test()) {
				/* Get magazine from depot */
				/* mag = depot->full_list; */
				/* depot->full_list = mag->next; */

				umem_htm_end();
				umem_htm_update_stats(state, UMEM_HTM_SUCCESS);
				return mag;
			}
		}

		/* Transaction aborted */
		umem_htm_update_stats(state, status);

		/* Don't retry on capacity or nested aborts */
		if (status == UMEM_HTM_ABORT_CAPACITY ||
		    status == UMEM_HTM_ABORT_NESTED) {
			break;
		}

		retries++;
	}

	/* Fall back to locks */
	state->stats.lock_fallbacks++;
	return NULL; /* Caller uses normal lock path */
}

void
umem_htm_depot_free(void *cache, void *mag, umem_htm_cache_state_t *state)
{
	umem_cache_t *cp = (umem_cache_t *)cache;
	int retries = 0;
	int status;

	if (mag == NULL) {
		return;
	}

	if (!umem_htm_should_use(state)) {
		/* Fall back to regular lock-based free */
		state->stats.lock_fallbacks++;
		return; /* Caller uses normal path */
	}

	/* Try with HTM */
	while (retries < UMEM_HTM_MAX_RETRIES) {
		status = umem_htm_begin();
		if (status == UMEM_HTM_SUCCESS) {
			/*
			 * Transactional code - access depot without lock
			 */

			if (umem_htm_test()) {
				/* Return magazine to depot */
				/* mag->next = depot->empty_list; */
				/* depot->empty_list = mag; */

				umem_htm_end();
				umem_htm_update_stats(state, UMEM_HTM_SUCCESS);
				return;
			}
		}

		/* Transaction aborted */
		umem_htm_update_stats(state, status);

		if (status == UMEM_HTM_ABORT_CAPACITY ||
		    status == UMEM_HTM_ABORT_NESTED) {
			break;
		}

		retries++;
	}

	/* Fall back to locks */
	state->stats.lock_fallbacks++;
	/* Caller uses normal lock path */
}

void
umem_htm_get_stats(umem_htm_cache_state_t *state, umem_htm_stats_t *stats)
{
	if (state == NULL || stats == NULL) {
		return;
	}

	memcpy(stats, &state->stats, sizeof(umem_htm_stats_t));
}

void
umem_htm_reset_stats(umem_htm_cache_state_t *state)
{
	if (state == NULL) {
		return;
	}

	memset(&state->stats, 0, sizeof(umem_htm_stats_t));
	state->sample_window = 0;
	state->sample_aborts = 0;
	state->dynamic_disable = 0;
	state->stats.enabled = 1;
}

void
umem_htm_enable(void)
{
	if (umem_htm_available_flags != 0) {
		umem_htm_enabled = 1;
	}
}

void
umem_htm_disable(void)
{
	umem_htm_enabled = 0;
}

void
umem_htm_set_abort_threshold(double threshold)
{
	if (threshold >= 0.0 && threshold <= 1.0) {
		umem_htm_abort_threshold = threshold;
	}
}

void
umem_htm_dump_stats(umem_htm_stats_t *stats)
{
	if (stats == NULL) {
		return;
	}

	fprintf(stderr, "HTM Statistics:\n");
	fprintf(stderr, "  Commits: %lu\n", stats->commits);
	fprintf(stderr, "  Aborts: %lu\n", stats->aborts);
	fprintf(stderr, "    Explicit: %lu\n", stats->aborts_explicit);
	fprintf(stderr, "    Retry: %lu\n", stats->aborts_retry);
	fprintf(stderr, "    Conflict: %lu\n", stats->aborts_conflict);
	fprintf(stderr, "    Capacity: %lu\n", stats->aborts_capacity);
	fprintf(stderr, "    Other: %lu\n", stats->aborts_other);
	fprintf(stderr, "  Lock fallbacks: %lu\n", stats->lock_fallbacks);
	fprintf(stderr, "  Disabled count: %lu\n", stats->disabled_count);
	fprintf(stderr, "  Abort rate: %.2f%%\n", stats->abort_rate * 100.0);
	fprintf(stderr, "  Enabled: %s\n", stats->enabled ? "yes" : "no");
}

void
umem_htm_dump(void)
{
	fprintf(stderr, "HTM State:\n");
	fprintf(stderr, "  Enabled: %d\n", umem_htm_enabled);
	fprintf(stderr, "  Available flags: 0x%x\n", umem_htm_available_flags);

#ifdef UMEM_TSX_AVAILABLE
	if (umem_htm_available_flags & UMEM_HTM_TSX_RTM) {
		fprintf(stderr, "  Intel TSX RTM: available\n");
	}
	if (umem_htm_available_flags & UMEM_HTM_TSX_HLE) {
		fprintf(stderr, "  Intel TSX HLE: available\n");
	}
#endif

	fprintf(stderr, "  Abort threshold: %.2f%%\n",
	    umem_htm_abort_threshold * 100.0);
}

#endif /* UMEM_HTM_AVAILABLE */
