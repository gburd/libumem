/*
 * Standalone tool to read and display a .ump allocation profile.
 *
 * Usage: umem_profile_dump <file.ump>
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UMEM_CACHE_NAMELEN 31
#define UMP_MAGIC          0x554D5031
#define UMP_VERSION        1
#define UMP_MAX_CACHES     256
#define UMP_MAX_PHASES     1024

typedef struct {
	uint32_t magic;
	uint32_t version;
	uint64_t timestamp;
	uint32_t duration_ms;
	uint32_t num_caches;
	uint32_t num_phases;
	uint32_t num_threads;
} ump_file_header_t;

typedef struct {
	char     name[UMEM_CACHE_NAMELEN + 1];
	size_t   bufsize;
	uint64_t steady_state_buftotal;
	uint64_t peak_buftotal;
	double   alloc_rate;
	double   free_rate;
	uint32_t optimal_magazine_size;
	uint32_t slab_count_needed;
} profile_cache_record_t;

typedef struct {
	uint64_t buftotal;
	double   alloc_rate;
	double   free_rate;
} profile_cache_snapshot_t;

static int
read_exact(int fd, void *buf, size_t count)
{
	ssize_t n = read(fd, buf, count);
	return (n == (ssize_t)count) ? 0 : -1;
}

int
main(int argc, char *argv[])
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <file.ump>\n", argv[0]);
		return 1;
	}

	int fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "%s: %s: %s\n", argv[0], argv[1],
		    strerror(errno));
		return 1;
	}

	ump_file_header_t hdr;
	if (read_exact(fd, &hdr, sizeof(hdr)) != 0) {
		fprintf(stderr, "%s: failed to read header\n", argv[0]);
		close(fd);
		return 1;
	}

	if (hdr.magic != UMP_MAGIC) {
		fprintf(stderr, "%s: bad magic 0x%08x (expected 0x%08x)\n",
		    argv[0], hdr.magic, UMP_MAGIC);
		close(fd);
		return 1;
	}

	if (hdr.version != UMP_VERSION) {
		fprintf(stderr, "%s: unsupported version %u\n",
		    argv[0], hdr.version);
		close(fd);
		return 1;
	}

	printf("=== umem allocation profile ===\n");
	printf("version:    %u\n", hdr.version);
	printf("timestamp:  %llu\n", (unsigned long long)hdr.timestamp);
	printf("duration:   %u ms (%.1f s)\n", hdr.duration_ms,
	    hdr.duration_ms / 1000.0);
	printf("caches:     %u\n", hdr.num_caches);
	printf("phases:     %u\n", hdr.num_phases);
	printf("threads:    %u\n", hdr.num_threads);
	printf("\n");

	if (hdr.num_caches > UMP_MAX_CACHES) {
		fprintf(stderr, "%s: too many caches (%u)\n",
		    argv[0], hdr.num_caches);
		close(fd);
		return 1;
	}

	profile_cache_record_t caches[UMP_MAX_CACHES];

	printf("--- Per-cache summary ---\n");
	printf("%-32s %8s %12s %12s %10s %10s %6s %6s\n",
	    "name", "bufsize", "steady_bufs", "peak_bufs",
	    "alloc/s", "free/s", "magopt", "slabs");

	uint32_t i;
	for (i = 0; i < hdr.num_caches; i++) {
		if (read_exact(fd, &caches[i], sizeof(caches[i])) != 0) {
			fprintf(stderr, "%s: truncated at cache %u\n",
			    argv[0], i);
			close(fd);
			return 1;
		}

		profile_cache_record_t *r = &caches[i];
		printf("%-32s %8zu %12llu %12llu %10.1f %10.1f %6u %6u\n",
		    r->name, r->bufsize,
		    (unsigned long long)r->steady_state_buftotal,
		    (unsigned long long)r->peak_buftotal,
		    r->alloc_rate, r->free_rate,
		    r->optimal_magazine_size, r->slab_count_needed);
	}
	printf("\n");

	if (hdr.num_phases > UMP_MAX_PHASES) {
		fprintf(stderr, "%s: too many phases (%u)\n",
		    argv[0], hdr.num_phases);
		close(fd);
		return 1;
	}

	printf("--- Phases ---\n");
	for (i = 0; i < hdr.num_phases; i++) {
		uint32_t phase_hdr[3];
		if (read_exact(fd, phase_hdr, sizeof(phase_hdr)) != 0) {
			fprintf(stderr, "%s: truncated at phase %u\n",
			    argv[0], i);
			close(fd);
			return 1;
		}

		uint32_t num_snap_caches = phase_hdr[2];
		printf("Phase %u: %u ms - %u ms (%u caches)\n",
		    i, phase_hdr[0], phase_hdr[1], num_snap_caches);

		if (num_snap_caches > UMP_MAX_CACHES)
			num_snap_caches = UMP_MAX_CACHES;

		uint32_t j;
		for (j = 0; j < num_snap_caches; j++) {
			profile_cache_snapshot_t snap;
			if (read_exact(fd, &snap, sizeof(snap)) != 0) {
				fprintf(stderr,
				    "%s: truncated at phase %u snap %u\n",
				    argv[0], i, j);
				close(fd);
				return 1;
			}

			const char *name = (j < hdr.num_caches) ?
			    caches[j].name : "(unknown)";
			printf("  %-32s buftotal=%llu alloc_rate=%.1f "
			    "free_rate=%.1f\n",
			    name,
			    (unsigned long long)snap.buftotal,
			    snap.alloc_rate, snap.free_rate);
		}
	}

	close(fd);
	return 0;
}
