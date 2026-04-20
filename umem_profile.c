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

/*
 * Allocation pattern profiling and predictive replay for libumem.
 *
 * In "record" mode, the update thread periodically samples cache statistics,
 * detects phase transitions (burst vs steady state), and writes a binary
 * profile (.ump) on exit.
 *
 * In "use" mode, a previously recorded profile is loaded at startup and
 * applied to caches as they are created: magazine sizes are tuned and
 * slabs are pre-allocated based on observed peaks.  The update thread
 * also performs runtime phase matching via cosine similarity to predict
 * and pre-allocate for upcoming allocation bursts.
 */

#include "config.h"
#include "umem_base.h"
#include "umem_profile.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

/* ================================================================
 * Internal data structures
 * ================================================================ */

/*
 * Per-cache snapshot taken each sample interval.
 */
typedef struct profile_cache_snapshot {
	uint64_t buftotal;
	double   alloc_rate;    /* allocs/sec */
	double   free_rate;     /* frees/sec  */
} profile_cache_snapshot_t;

/*
 * Per-cache summary computed at profile finalization.
 */
typedef struct profile_cache_record {
	char     name[UMEM_CACHE_NAMELEN + 1];
	size_t   bufsize;
	uint64_t steady_state_buftotal;
	uint64_t peak_buftotal;
	double   alloc_rate;
	double   free_rate;
	uint32_t optimal_magazine_size;
	uint32_t slab_count_needed;
} profile_cache_record_t;

/*
 * A phase is a contiguous time interval with a stable allocation
 * rate vector across all caches.
 */
typedef struct profile_phase {
	uint32_t start_ms;
	uint32_t end_ms;
	uint32_t num_caches;
	profile_cache_snapshot_t snapshots[UMP_MAX_CACHES];
} profile_phase_t;

/*
 * Per-thread summary (aggregated from PTC stats).
 */
typedef struct profile_thread_record {
	uint32_t primary_cache_idx;
	uint64_t total_allocs;
	uint64_t total_frees;
} profile_thread_record_t;

/*
 * On-disk binary header.
 */
typedef struct ump_file_header {
	uint32_t magic;
	uint32_t version;
	uint64_t timestamp;     /* epoch seconds when profile was written */
	uint32_t duration_ms;
	uint32_t num_caches;
	uint32_t num_phases;
	uint32_t num_threads;
} ump_file_header_t;

/*
 * Live recording state: built incrementally by umem_profile_sample().
 */
typedef struct profile_state {
	umem_profile_mode_t mode;
	char                path[1024];

	/* Recording state */
	struct timeval      start_time;
	uint32_t            sample_count;
	uint32_t            num_caches;

	/* Per-cache tracking arrays (indexed by discovery order) */
	char     cache_names[UMP_MAX_CACHES][UMEM_CACHE_NAMELEN + 1];
	size_t   cache_bufsizes[UMP_MAX_CACHES];
	uint64_t prev_alloc_ops[UMP_MAX_CACHES];
	uint64_t prev_free_ops[UMP_MAX_CACHES];
	uint64_t prev_buftotal[UMP_MAX_CACHES];
	double   cur_alloc_rates[UMP_MAX_CACHES];
	double   cur_free_rates[UMP_MAX_CACHES];
	uint64_t peak_buftotal[UMP_MAX_CACHES];
	uint64_t steady_buftotal[UMP_MAX_CACHES];
	uint32_t stable_intervals[UMP_MAX_CACHES];
	uint64_t mag_reloads_prev[UMP_MAX_CACHES];
	uint64_t mag_reloads_cur[UMP_MAX_CACHES];

	/* Phase tracking */
	uint32_t            num_phases;
	profile_phase_t     phases[UMP_MAX_PHASES];
	int                 in_phase;
	double              prev_rates[UMP_MAX_CACHES];

	/* Entropy-based random workload detection */
	int                 random_workload;

	/* Pre-built index: cache walk order -> profile cache index */
	int                 cache_to_profile_idx[UMP_MAX_CACHES];

	/* Replay state (loaded profile) */
	ump_file_header_t          loaded_header;
	profile_cache_record_t     loaded_caches[UMP_MAX_CACHES];
	profile_phase_t            loaded_phases[UMP_MAX_PHASES];
	int                        current_phase_idx;

} profile_state_t;

static profile_state_t profile_state;

/* ================================================================
 * Utility helpers
 * ================================================================ */

static uint32_t
elapsed_ms(const struct timeval *start)
{
	struct timeval now;
	(void) gettimeofday(&now, NULL);
	uint32_t secs = (uint32_t)(now.tv_sec - start->tv_sec);
	int32_t usecs = (int32_t)(now.tv_usec - start->tv_usec);
	return secs * 1000 + usecs / 1000;
}

/*
 * Find or add a cache index by name.
 */
static int
find_or_add_cache(const char *name, size_t bufsize)
{
	uint32_t i;
	profile_state_t *ps = &profile_state;

	for (i = 0; i < ps->num_caches; i++) {
		if (strcmp(ps->cache_names[i], name) == 0)
			return (int)i;
	}

	if (ps->num_caches >= UMP_MAX_CACHES)
		return -1;

	i = ps->num_caches++;
	(void) strncpy(ps->cache_names[i], name, UMEM_CACHE_NAMELEN);
	ps->cache_names[i][UMEM_CACHE_NAMELEN] = '\0';
	ps->cache_bufsizes[i] = bufsize;
	ps->prev_alloc_ops[i] = 0;
	ps->prev_free_ops[i] = 0;
	ps->prev_buftotal[i] = 0;
	ps->cur_alloc_rates[i] = 0.0;
	ps->cur_free_rates[i] = 0.0;
	ps->peak_buftotal[i] = 0;
	ps->steady_buftotal[i] = 0;
	ps->stable_intervals[i] = 0;
	ps->mag_reloads_prev[i] = 0;
	ps->mag_reloads_cur[i] = 0;
	ps->prev_rates[i] = 0.0;

	return (int)i;
}

/*
 * L1 distance phase matcher with early exit.
 * Returns 1 if current and profiled rate vectors are within 20% L1 distance,
 * exits early if distance exceeds 30% of the running total.
 */
static int
phase_matches(double *current, double *profiled, int n)
{
	double sum_diff = 0, sum_total = 0;
	int i;

	for (i = 0; i < n; i++) {
		sum_diff += fabs(current[i] - profiled[i]);
		sum_total += current[i] + profiled[i];
		if (sum_diff > sum_total * 0.3)
			return 0;
	}
	return (sum_total > 0 && sum_diff / sum_total < 0.2);
}

/*
 * Determine optimal magazine size from observed reload frequency.
 * Higher reload rates mean the magazine is too small.
 */
static uint32_t
compute_optimal_magsize(uint64_t total_reloads, uint64_t total_allocs)
{
	if (total_allocs == 0 || total_reloads == 0)
		return 15;

	double ratio = (double)total_allocs / (double)total_reloads;

	if (ratio < 4)
		return 3;
	if (ratio < 16)
		return 7;
	if (ratio < 32)
		return 15;
	if (ratio < 64)
		return 31;
	if (ratio < 128)
		return 63;
	if (ratio < 256)
		return 127;
	return 255;
}

/* ================================================================
 * Recording
 * ================================================================ */

void
umem_profile_sample(void)
{
	profile_state_t *ps = &profile_state;
	umem_cache_t *cp;
	int phase_changed = 0;

	if (ps->mode != UMEM_PROFILE_RECORD && ps->mode != UMEM_PROFILE_USE)
		return;

	if (ps->mode == UMEM_PROFILE_USE) {
		if (!ps->random_workload)
			umem_profile_check_phase();
		return;
	}

	ps->sample_count++;

	/*
	 * Walk all caches, compute rates.
	 */
	(void) mutex_lock(&umem_cache_lock);
	for (cp = umem_null_cache.cache_next; cp != &umem_null_cache;
	    cp = cp->cache_next) {
		int idx = find_or_add_cache(cp->cache_name, cp->cache_bufsize);
		if (idx < 0)
			continue;

		/*
		 * Compute alloc_ops from per-CPU cc_alloc counters
		 * instead of using the removed hot-path atomic.
		 */
		uint64_t alloc_ops = 0;
		uint32_t ci;
		for (ci = 0; ci <= cp->cache_cpu_mask; ci++) {
			umem_cpu_cache_t *tc =
			    (umem_cpu_cache_t *)((char *)cp +
			    umem_cpus[ci].cpu_cache_offset);
			alloc_ops += tc->cc_alloc;
		}
		uint64_t slab_free = cp->cache_slab_free;
		uint64_t buftotal = cp->cache_buftotal;
		uint64_t mag_reloads = cp->cache_mag_reloads;

		double alloc_rate = (double)(alloc_ops - ps->prev_alloc_ops[idx]);
		double free_rate = (double)(slab_free - ps->prev_free_ops[idx]);

		ps->prev_alloc_ops[idx] = alloc_ops;
		ps->prev_free_ops[idx] = slab_free;
		ps->prev_buftotal[idx] = buftotal;
		ps->cur_alloc_rates[idx] = alloc_rate;
		ps->cur_free_rates[idx] = free_rate;
		ps->mag_reloads_cur[idx] = mag_reloads;

		/* Track peak */
		if (buftotal > ps->peak_buftotal[idx])
			ps->peak_buftotal[idx] = buftotal;

		/* Stability detection */
		double prev_rate = ps->prev_rates[idx];
		double delta = (prev_rate > 1e-6) ?
		    fabs(alloc_rate - prev_rate) / prev_rate : 0.0;

		if (delta > UMP_PHASE_THRESHOLD) {
			ps->stable_intervals[idx] = 0;
			phase_changed = 1;
		} else {
			ps->stable_intervals[idx]++;
			if (ps->stable_intervals[idx] >= UMP_STEADY_INTERVALS)
				ps->steady_buftotal[idx] = buftotal;
		}

		ps->prev_rates[idx] = alloc_rate;
	}
	(void) mutex_unlock(&umem_cache_lock);

	/*
	 * Entropy check: if the allocation rate distribution is
	 * near-uniform random, skip recording this sample and
	 * mark the workload as random. Shannon entropy > 90% of
	 * log2(num_caches) indicates near-uniform distribution.
	 */
	if (ps->num_caches > 1) {
		double total_rate = 0.0;
		uint32_t ei;

		for (ei = 0; ei < ps->num_caches; ei++)
			total_rate += ps->cur_alloc_rates[ei];

		if (total_rate > 0.0) {
			double entropy = 0.0;
			for (ei = 0; ei < ps->num_caches; ei++) {
				double p = ps->cur_alloc_rates[ei] / total_rate;
				if (p > 1e-12)
					entropy -= p * log2(p);
			}
			double max_entropy = log2((double)ps->num_caches);
			if (entropy > max_entropy * 0.9) {
				ps->random_workload = 1;
				return;
			}
		}
	}
	ps->random_workload = 0;

	/*
	 * Phase boundary detection: if any cache had a significant
	 * rate change, end the current phase and start a new one.
	 */
	if (phase_changed && ps->num_phases < UMP_MAX_PHASES) {
		uint32_t now_ms = elapsed_ms(&ps->start_time);
		uint32_t i;

		if (ps->in_phase && ps->num_phases > 0)
			ps->phases[ps->num_phases - 1].end_ms = now_ms;

		profile_phase_t *ph = &ps->phases[ps->num_phases];
		ph->start_ms = now_ms;
		ph->end_ms = 0;
		ph->num_caches = ps->num_caches;

		for (i = 0; i < ps->num_caches; i++) {
			ph->snapshots[i].buftotal = ps->prev_buftotal[i];
			ph->snapshots[i].alloc_rate = ps->cur_alloc_rates[i];
			ph->snapshots[i].free_rate = ps->cur_free_rates[i];
		}

		ps->num_phases++;
		ps->in_phase = 1;
	}
}

/* ================================================================
 * Profile writing (record mode finalization)
 * ================================================================ */

static int
write_profile(const char *path)
{
	profile_state_t *ps = &profile_state;
	int fd;
	uint32_t i;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;

	/* Close final phase */
	if (ps->in_phase && ps->num_phases > 0) {
		uint32_t now_ms = elapsed_ms(&ps->start_time);
		ps->phases[ps->num_phases - 1].end_ms = now_ms;
	}

	/* Build header */
	ump_file_header_t hdr;
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = UMP_MAGIC;
	hdr.version = UMP_VERSION;
	hdr.timestamp = (uint64_t)ps->start_time.tv_sec;
	hdr.duration_ms = elapsed_ms(&ps->start_time);
	hdr.num_caches = ps->num_caches;
	hdr.num_phases = ps->num_phases;
	hdr.num_threads = 0;

	if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr))
		goto fail;

	/* Write per-cache records */
	for (i = 0; i < ps->num_caches; i++) {
		profile_cache_record_t rec;
		memset(&rec, 0, sizeof(rec));

		(void) strncpy(rec.name, ps->cache_names[i],
		    UMEM_CACHE_NAMELEN);
		rec.name[UMEM_CACHE_NAMELEN] = '\0';
		rec.bufsize = ps->cache_bufsizes[i];
		rec.steady_state_buftotal = ps->steady_buftotal[i];
		rec.peak_buftotal = ps->peak_buftotal[i];
		rec.alloc_rate = ps->cur_alloc_rates[i];
		rec.free_rate = ps->cur_free_rates[i];
		rec.optimal_magazine_size = compute_optimal_magsize(
		    ps->mag_reloads_cur[i] - ps->mag_reloads_prev[i],
		    ps->prev_alloc_ops[i]);

		/* Estimate slab count: peak_buftotal / estimated bufs_per_slab
		 * A rough heuristic: slabsize is typically 64K, so bufs_per_slab
		 * ~ 65536 / bufsize.  Minimum 1. */
		uint32_t bufs_per_slab = 1;
		if (rec.bufsize > 0 && rec.bufsize <= 65536)
			bufs_per_slab = (uint32_t)(65536 / rec.bufsize);
		if (bufs_per_slab == 0)
			bufs_per_slab = 1;
		rec.slab_count_needed =
		    (uint32_t)((rec.peak_buftotal + bufs_per_slab - 1) /
		    bufs_per_slab);

		if (write(fd, &rec, sizeof(rec)) != (ssize_t)sizeof(rec))
			goto fail;
	}

	/* Write phase records */
	for (i = 0; i < ps->num_phases; i++) {
		profile_phase_t *ph = &ps->phases[i];
		uint32_t phase_hdr[3];
		phase_hdr[0] = ph->start_ms;
		phase_hdr[1] = ph->end_ms;
		phase_hdr[2] = ph->num_caches;

		if (write(fd, phase_hdr, sizeof(phase_hdr)) !=
		    (ssize_t)sizeof(phase_hdr))
			goto fail;

		size_t snap_size = ph->num_caches *
		    sizeof(profile_cache_snapshot_t);
		if (write(fd, ph->snapshots, snap_size) !=
		    (ssize_t)snap_size)
			goto fail;
	}

	(void) close(fd);
	return 0;

fail:
	(void) close(fd);
	return -1;
}

/* ================================================================
 * Profile reading (use mode)
 * ================================================================ */

int
umem_profile_load(const char *path)
{
	profile_state_t *ps = &profile_state;
	int fd;
	uint32_t i;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	if (read(fd, &ps->loaded_header, sizeof(ps->loaded_header)) !=
	    (ssize_t)sizeof(ps->loaded_header))
		goto fail;

	if (ps->loaded_header.magic != UMP_MAGIC ||
	    ps->loaded_header.version != UMP_VERSION)
		goto fail;

	if (ps->loaded_header.num_caches > UMP_MAX_CACHES ||
	    ps->loaded_header.num_phases > UMP_MAX_PHASES)
		goto fail;

	/* Read per-cache records */
	for (i = 0; i < ps->loaded_header.num_caches; i++) {
		if (read(fd, &ps->loaded_caches[i],
		    sizeof(profile_cache_record_t)) !=
		    (ssize_t)sizeof(profile_cache_record_t))
			goto fail;
	}

	/* Read phase records */
	for (i = 0; i < ps->loaded_header.num_phases; i++) {
		uint32_t phase_hdr[3];
		if (read(fd, phase_hdr, sizeof(phase_hdr)) !=
		    (ssize_t)sizeof(phase_hdr))
			goto fail;

		ps->loaded_phases[i].start_ms = phase_hdr[0];
		ps->loaded_phases[i].end_ms = phase_hdr[1];
		ps->loaded_phases[i].num_caches = phase_hdr[2];

		if (ps->loaded_phases[i].num_caches > UMP_MAX_CACHES)
			goto fail;

		size_t snap_size = ps->loaded_phases[i].num_caches *
		    sizeof(profile_cache_snapshot_t);
		if (read(fd, ps->loaded_phases[i].snapshots, snap_size) !=
		    (ssize_t)snap_size)
			goto fail;
	}

	(void) close(fd);
	ps->current_phase_idx = -1;

	/*
	 * Build cache walk order -> profile cache index mapping.
	 * Walk live caches once and match by name against the loaded
	 * profile to eliminate O(n^2) strcmp in check_phase.
	 */
	for (i = 0; i < UMP_MAX_CACHES; i++)
		ps->cache_to_profile_idx[i] = -1;

	{
		umem_cache_t *cp;
		uint32_t walk_idx = 0;

		(void) mutex_lock(&umem_cache_lock);
		for (cp = umem_null_cache.cache_next;
		    cp != &umem_null_cache;
		    cp = cp->cache_next) {
			if (walk_idx >= UMP_MAX_CACHES)
				break;
			for (i = 0; i < ps->loaded_header.num_caches; i++) {
				if (strcmp(ps->loaded_caches[i].name,
				    cp->cache_name) == 0) {
					ps->cache_to_profile_idx[walk_idx] =
					    (int)i;
					break;
				}
			}
			walk_idx++;
		}
		(void) mutex_unlock(&umem_cache_lock);
	}

	return 0;

fail:
	(void) close(fd);
	return -1;
}

/* ================================================================
 * Replay: apply profile to caches
 * ================================================================ */

void
umem_profile_apply_cache(struct umem_cache *cp)
{
	profile_state_t *ps = &profile_state;
	uint32_t i;

	if (ps->mode != UMEM_PROFILE_USE)
		return;

	/* Find matching cache by name, falling back to bufsize */
	profile_cache_record_t *match = NULL;
	for (i = 0; i < ps->loaded_header.num_caches; i++) {
		if (strcmp(ps->loaded_caches[i].name, cp->cache_name) == 0) {
			match = &ps->loaded_caches[i];
			break;
		}
	}
	if (match == NULL) {
		for (i = 0; i < ps->loaded_header.num_caches; i++) {
			if (ps->loaded_caches[i].bufsize ==
			    cp->cache_bufsize) {
				match = &ps->loaded_caches[i];
				break;
			}
		}
	}

	if (match == NULL)
		return;

	/*
	 * Pre-allocate slabs by forcing allocations up to the profiled
	 * peak.  We allocate and immediately free to populate the depot.
	 */
	if (match->slab_count_needed > 0 && cp->cache_bufsize > 0) {
		uint32_t bufs_per_slab = 1;
		if (cp->cache_bufsize <= 65536)
			bufs_per_slab =
			    (uint32_t)(65536 / cp->cache_bufsize);
		if (bufs_per_slab == 0)
			bufs_per_slab = 1;

		uint32_t target = match->slab_count_needed * bufs_per_slab;
		if (target > 4096)
			target = 4096;

		void **bufs = (void **)_umem_alloc(
		    target * sizeof(void *), UMEM_DEFAULT);
		if (bufs != NULL) {
			uint32_t allocated = 0;
			uint32_t j;
			for (j = 0; j < target; j++) {
				bufs[j] = _umem_cache_alloc(cp, UMEM_DEFAULT);
				if (bufs[j] == NULL)
					break;
				allocated++;
			}
			for (j = 0; j < allocated; j++)
				_umem_cache_free(cp, bufs[j]);
			_umem_free(bufs, target * sizeof(void *));
		}
	}
}

/* ================================================================
 * Runtime phase matching
 * ================================================================ */

void
umem_profile_check_phase(void)
{
	profile_state_t *ps = &profile_state;
	umem_cache_t *cp;
	uint32_t i;

	if (ps->mode != UMEM_PROFILE_USE)
		return;

	if (ps->loaded_header.num_phases == 0)
		return;

	/*
	 * Build current rate vector using the pre-built index array
	 * to avoid O(n^2) string comparisons.
	 */
	double current_rates[UMP_MAX_CACHES];
	uint32_t ncaches = 0;
	uint32_t walk_idx = 0;

	memset(current_rates, 0, sizeof(current_rates));

	(void) mutex_lock(&umem_cache_lock);
	for (cp = umem_null_cache.cache_next; cp != &umem_null_cache;
	    cp = cp->cache_next) {
		if (walk_idx >= UMP_MAX_CACHES)
			break;

		int idx = ps->cache_to_profile_idx[walk_idx];
		walk_idx++;

		if (idx < 0)
			continue;

		/*
		 * Compute alloc_ops by summing per-CPU cc_alloc counters
		 * instead of relying on the atomic hot-path counter.
		 */
		uint64_t alloc_ops = 0;
		uint32_t cpu;
		for (cpu = 0; cpu <= cp->cache_cpu_mask; cpu++) {
			umem_cpu_cache_t *ccp =
			    (umem_cpu_cache_t *)((char *)cp +
			    umem_cpus[cpu].cpu_cache_offset);
			alloc_ops += ccp->cc_alloc;
		}

		double rate = (double)(alloc_ops -
		    ps->prev_alloc_ops[idx]);
		ps->prev_alloc_ops[idx] = alloc_ops;
		current_rates[idx] = rate;
		if ((uint32_t)idx >= ncaches)
			ncaches = (uint32_t)idx + 1;
	}
	(void) mutex_unlock(&umem_cache_lock);

	if (ncaches == 0)
		return;

	/*
	 * Compare current rates against each profiled phase
	 * using L1 distance with early exit.
	 */
	int best_phase = -1;

	for (i = 0; i < ps->loaded_header.num_phases; i++) {
		profile_phase_t *ph = &ps->loaded_phases[i];
		double phase_rates[UMP_MAX_CACHES];
		uint32_t j;

		uint32_t n = (ncaches < ph->num_caches) ?
		    ncaches : ph->num_caches;

		for (j = 0; j < n; j++)
			phase_rates[j] = ph->snapshots[j].alloc_rate;
		for (j = n; j < ncaches; j++)
			phase_rates[j] = 0.0;

		if (phase_matches(current_rates, phase_rates, (int)ncaches)) {
			best_phase = (int)i;
			break;
		}
	}

	if (best_phase < 0)
		return;

	if (best_phase == ps->current_phase_idx)
		return;

	ps->current_phase_idx = best_phase;

	/*
	 * Look ahead to the next phase and pre-allocate for it.
	 */
	uint32_t next_idx = (uint32_t)best_phase + 1;
	if (next_idx >= ps->loaded_header.num_phases)
		return;

	profile_phase_t *next_ph = &ps->loaded_phases[next_idx];

	(void) mutex_lock(&umem_cache_lock);
	walk_idx = 0;
	for (cp = umem_null_cache.cache_next; cp != &umem_null_cache;
	    cp = cp->cache_next) {
		if (walk_idx >= UMP_MAX_CACHES)
			break;

		int idx = ps->cache_to_profile_idx[walk_idx];
		walk_idx++;

		if (idx < 0 || (uint32_t)idx >= next_ph->num_caches)
			continue;

		uint64_t target_bufs = next_ph->snapshots[idx].buftotal;
		if (target_bufs > cp->cache_buftotal) {
			uint64_t gap = target_bufs - cp->cache_buftotal;
			if (gap > 64)
				gap = 64;
			uint32_t j;
			void *tmp[64];
			uint32_t got = 0;
			(void) mutex_unlock(&umem_cache_lock);
			for (j = 0; j < (uint32_t)gap; j++) {
				tmp[j] = _umem_cache_alloc(cp,
				    UMEM_DEFAULT);
				if (tmp[j] == NULL)
					break;
				got++;
			}
			for (j = 0; j < got; j++)
				_umem_cache_free(cp, tmp[j]);
			(void) mutex_lock(&umem_cache_lock);
		}
	}
	(void) mutex_unlock(&umem_cache_lock);
}

/* ================================================================
 * Initialization and shutdown
 * ================================================================ */

int
umem_profile_init(const char *mode_and_path)
{
	profile_state_t *ps = &profile_state;

	if (mode_and_path == NULL || mode_and_path[0] == '\0')
		return 0;

	memset(ps, 0, sizeof(*ps));

	if (strncmp(mode_and_path, "record:", 7) == 0) {
		ps->mode = UMEM_PROFILE_RECORD;
		(void) strncpy(ps->path, mode_and_path + 7,
		    sizeof(ps->path) - 1);
		ps->path[sizeof(ps->path) - 1] = '\0';
		(void) gettimeofday(&ps->start_time, NULL);
		umem_magazine_tuning = 1;
		atexit(umem_profile_fini);
		return 0;
	}

	if (strncmp(mode_and_path, "use:", 4) == 0) {
		ps->mode = UMEM_PROFILE_USE;
		(void) strncpy(ps->path, mode_and_path + 4,
		    sizeof(ps->path) - 1);
		ps->path[sizeof(ps->path) - 1] = '\0';
		if (umem_profile_load(ps->path) != 0) {
			ps->mode = UMEM_PROFILE_OFF;
			return -1;
		}
		(void) gettimeofday(&ps->start_time, NULL);
		return 0;
	}

	return -1;
}

void
umem_profile_fini(void)
{
	profile_state_t *ps = &profile_state;

	if (ps->mode == UMEM_PROFILE_RECORD) {
		/* Take a final sample to capture end-state, even if
		 * the update thread never ran (short-lived processes) */
		umem_profile_sample();
		(void) write_profile(ps->path);
	}

	ps->mode = UMEM_PROFILE_OFF;
}

/* ================================================================
 * Human-readable dump
 * ================================================================ */

void
umem_profile_dump_text(FILE *fp)
{
	profile_state_t *ps = &profile_state;
	uint32_t i, j;

	if (ps->loaded_header.magic != UMP_MAGIC) {
		fprintf(fp, "(no profile loaded)\n");
		return;
	}

	fprintf(fp, "=== umem profile ===\n");
	fprintf(fp, "version:    %u\n", ps->loaded_header.version);
	fprintf(fp, "timestamp:  %llu\n",
	    (unsigned long long)ps->loaded_header.timestamp);
	fprintf(fp, "duration:   %u ms\n", ps->loaded_header.duration_ms);
	fprintf(fp, "caches:     %u\n", ps->loaded_header.num_caches);
	fprintf(fp, "phases:     %u\n", ps->loaded_header.num_phases);
	fprintf(fp, "threads:    %u\n", ps->loaded_header.num_threads);
	fprintf(fp, "\n");

	fprintf(fp, "--- Per-cache summary ---\n");
	fprintf(fp, "%-32s %8s %12s %12s %8s %8s %6s %6s\n",
	    "name", "bufsize", "steady_bufs", "peak_bufs",
	    "alloc/s", "free/s", "magopt", "slabs");
	for (i = 0; i < ps->loaded_header.num_caches; i++) {
		profile_cache_record_t *r = &ps->loaded_caches[i];
		fprintf(fp, "%-32s %8zu %12llu %12llu %8.1f %8.1f %6u %6u\n",
		    r->name, r->bufsize,
		    (unsigned long long)r->steady_state_buftotal,
		    (unsigned long long)r->peak_buftotal,
		    r->alloc_rate, r->free_rate,
		    r->optimal_magazine_size, r->slab_count_needed);
	}
	fprintf(fp, "\n");

	fprintf(fp, "--- Phases ---\n");
	for (i = 0; i < ps->loaded_header.num_phases; i++) {
		profile_phase_t *ph = &ps->loaded_phases[i];
		fprintf(fp, "Phase %u: %u ms - %u ms (%u caches)\n",
		    i, ph->start_ms, ph->end_ms, ph->num_caches);
		for (j = 0; j < ph->num_caches &&
		    j < ps->loaded_header.num_caches; j++) {
			fprintf(fp, "  %-32s buftotal=%llu alloc_rate=%.1f\n",
			    ps->loaded_caches[j].name,
			    (unsigned long long)ph->snapshots[j].buftotal,
			    ph->snapshots[j].alloc_rate);
		}
	}
}
