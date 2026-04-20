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
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

#ifndef _UMEM_PROFILE_H
#define _UMEM_PROFILE_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct umem_cache;

/*
 * Allocation profile binary format (.ump) magic and version.
 */
#define UMP_MAGIC       0x554D5031      /* "UMP1" */
#define UMP_VERSION     1

/*
 * Maximum caches and phases tracked per profile.
 */
#define UMP_MAX_CACHES  256
#define UMP_MAX_PHASES  1024
#define UMP_MAX_THREADS 256

/*
 * Phase detection threshold: a rate change exceeding this fraction
 * of the previous rate triggers a new phase boundary.
 */
#define UMP_PHASE_THRESHOLD     0.50

/*
 * Number of stable intervals required before marking steady state.
 */
#define UMP_STEADY_INTERVALS    10

/*
 * Cosine similarity threshold for phase matching during replay.
 */
#define UMP_SIMILARITY_THRESHOLD 0.80

/*
 * Profile mode: record allocations or replay from a saved profile.
 */
typedef enum umem_profile_mode {
	UMEM_PROFILE_OFF = 0,
	UMEM_PROFILE_RECORD,
	UMEM_PROFILE_USE
} umem_profile_mode_t;

/*
 * Recording: initialize profiling from the UMEM_PROFILE envvar.
 * Value must be "record:/path" or "use:/path".
 * Returns 0 on success, -1 on failure.
 */
int  umem_profile_init(const char *mode_and_path);

/*
 * Finalize and write the profile on exit (record mode) or
 * release loaded profile state (use mode).
 */
void umem_profile_fini(void);

/*
 * Called from the update thread every reap interval to sample
 * cache statistics and detect phase transitions.
 */
void umem_profile_sample(void);

/*
 * Replay: load a previously recorded profile from disk.
 * Returns 0 on success, -1 on failure.
 */
int  umem_profile_load(const char *path);

/*
 * Apply profile hints to a cache at creation time: select the
 * optimal magazine type and pre-allocate slabs.
 */
void umem_profile_apply_cache(struct umem_cache *cp);

/*
 * Called from the update thread during replay to compare the
 * current allocation rate vector against profiled phases and
 * pre-allocate for upcoming phases.
 */
void umem_profile_check_phase(void);

/*
 * Dump a human-readable summary of the in-memory profile to fp.
 */
void umem_profile_dump_text(FILE *fp);

#ifdef __cplusplus
}
#endif

#endif /* _UMEM_PROFILE_H */
