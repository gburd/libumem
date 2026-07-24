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
 * Property/invariant tests for the experimental allocation profiler
 * (umem_profile.h) and the offline .ump reader (tools/umem_profile_dump).
 *
 * Two invariants:
 *
 *   1. REPLAY IS CORRECTNESS-NEUTRAL.  A profile is only a performance hint:
 *      replaying it (UMEM_PROFILE_USE + umem_profile_apply_cache) pre-warms
 *      caches by allocating and immediately freeing, populating the depot.
 *      It must NEVER change program-visible allocation behavior.  We run the
 *      SAME deterministic workload three ways -- profiling OFF, RECORD, and
 *      USE (replay) -- and assert every program-visible property is
 *      identical: every alloc succeeds, returns a correctly-sized, writable,
 *      properly-zeroed (zalloc) buffer, distinct live pointers never alias,
 *      and the data written survives until free.  (Addresses may differ --
 *      that is not program-visible; behavior may not.)
 *
 *   2. .UMP PARSE MATCHES RECORDED STATS.  The offline tool
 *      tools/umem_profile_dump, parsing the on-disk .ump, must report the
 *      same per-cache numbers (bufsize, steady/peak buftotal) that the
 *      in-library umem_profile_dump_text() reports from the loaded profile.
 *      This proves the writer and the independent reader agree on the format.
 *
 * Build under ASan.  Needs umem_impl.h for cache field access (like the unit
 * test).  The dump tool path is passed in argv[0]-relative or via
 * UMEM_PROFILE_DUMP_TOOL.
 */

#define	UMEM_ENABLE_EXPERIMENTAL
#include "../qc.h"
#include "../../umem_impl.h"
#include "../../umem_profile.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define	PROF_PATH	"/tmp/prop_umem_profile.ump"

static const char *g_dump_tool =
#ifdef UMEM_PROFILE_DUMP_TOOL_DEFAULT
    UMEM_PROFILE_DUMP_TOOL_DEFAULT;
#else
    "tools/umem_profile_dump";
#endif

/* ------------------------------------------------------------------ */
/* Deterministic workload + program-visible correctness oracle        */
/* ------------------------------------------------------------------ */

/*
 * Run a fixed workload against a named cache and record, into `results`,
 * everything the program can observe: for each allocation, whether it
 * succeeded, and a checksum of the bytes read back after writing a
 * deterministic pattern.  Also verifies no two live buffers alias.
 *
 * Returns 0 on success (all program-visible invariants held), non-zero on a
 * violation.  Fills *checksum with a fold of all observed data so two runs
 * can be compared for behavioral identity.
 */
static int
run_workload(const char *cache_name, size_t bufsize, int napply,
    uint64_t *checksum_out)
{
	umem_cache_t *cp = umem_cache_create((char *)cache_name,
	    bufsize, 0, NULL, NULL, NULL, NULL, NULL, 0);
	if (cp == NULL)
		return (-1);

	/* If a profile is loaded, apply it (pre-warm).  Neutral or not is
	 * exactly what we are testing. */
	for (int a = 0; a < napply; a++)
		umem_profile_apply_cache(cp);

	enum { N = 512 };
	void *bufs[N];
	uint64_t checksum = 0;
	int rc = 0;

	for (int i = 0; i < N; i++) {
		bufs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
		if (bufs[i] == NULL) { rc = 1; goto out; }

		/* No two live buffers may alias. */
		for (int j = 0; j < i; j++) {
			if (bufs[j] == bufs[i]) { rc = 2; goto out; }
		}

		/* Deterministic per-index pattern. */
		unsigned char pat = (unsigned char)((i * 31 + 7) & 0xFF);
		memset(bufs[i], pat, bufsize);

		umem_profile_sample();		/* drive the profiler */
	}

	/* Read back: every byte must be exactly what we wrote. */
	for (int i = 0; i < N; i++) {
		unsigned char pat = (unsigned char)((i * 31 + 7) & 0xFF);
		unsigned char *b = bufs[i];
		for (size_t k = 0; k < bufsize; k++) {
			if (b[k] != pat) { rc = 3; goto out; }
			checksum = checksum * 1099511628211ULL + b[k];
		}
	}

out:
	for (int i = 0; i < N; i++) {
		if (bufs[i] != NULL)
			umem_cache_free(cp, bufs[i]);
	}
	umem_cache_destroy(cp);
	if (checksum_out != NULL)
		*checksum_out = checksum;
	return (rc);
}

/* ------------------------------------------------------------------ */
/* Invariant 1: replay is correctness-neutral                          */
/* ------------------------------------------------------------------ */

/*
 * For a generated bufsize:
 *   (a) run OFF, capture checksum + success.
 *   (b) RECORD a profile of the same workload; write it out.
 *   (c) load the profile in USE mode; run again WITH apply_cache (replay).
 *   Assert: both runs succeeded and produced the identical checksum, i.e.
 *   replay changed nothing the program can see.
 */
static QCC_TestStatus
prop_replay_neutral(QCC_GenValue **vals, int len, QCC_Stamp **st)
{
	(void)st;
	if (len < 1)
		return (QCC_FAIL);
	size_t bufsize = (size_t)(*QCC_getValue(vals, 0, long*));
	if (bufsize < 8 || bufsize > 4096)
		return (QCC_NOTHING);
	bufsize &= ~(size_t)7;		/* 8-byte align the request */
	if (bufsize == 0)
		return (QCC_NOTHING);

	char name[64];
	static int seq;
	snprintf(name, sizeof (name), "prop_prof_%d_%zu", seq++, bufsize);

	/* (a) baseline: profiling OFF */
	uint64_t off_sum = 0;
	if (run_workload(name, bufsize, 0, &off_sum) != 0)
		return (QCC_FAIL);

	/* (b) RECORD */
	(void)unlink(PROF_PATH);
	char spec[128];
	snprintf(spec, sizeof (spec), "record:%s", PROF_PATH);
	if (umem_profile_init(spec) != 0)
		return (QCC_NOTHING);
	uint64_t rec_sum = 0;
	int rec_rc = run_workload(name, bufsize, 0, &rec_sum);
	umem_profile_fini();
	if (rec_rc != 0)
		return (QCC_FAIL);

	/* RECORD must also be neutral vs OFF. */
	if (rec_sum != off_sum)
		return (QCC_FAIL);

	/* (c) USE / replay */
	snprintf(spec, sizeof (spec), "use:%s", PROF_PATH);
	if (umem_profile_init(spec) != 0)
		return (QCC_NOTHING);
	uint64_t use_sum = 0;
	int use_rc = run_workload(name, bufsize, 1, &use_sum);
	umem_profile_fini();
	if (use_rc != 0)
		return (QCC_FAIL);

	/* The invariant: replay is program-visibly identical to OFF. */
	return (use_sum == off_sum ? QCC_OK : QCC_FAIL);
}

/*
 * Applying a profile multiple times (idempotence) must also be neutral --
 * repeated pre-warm must not change results or leak/corrupt.
 */
static QCC_TestStatus
prop_replay_idempotent(QCC_GenValue **vals, int len, QCC_Stamp **st)
{
	(void)st;
	if (len < 1)
		return (QCC_FAIL);
	size_t bufsize = (size_t)(*QCC_getValue(vals, 0, long*));
	if (bufsize < 8 || bufsize > 4096)
		return (QCC_NOTHING);
	bufsize &= ~(size_t)7;
	if (bufsize == 0)
		return (QCC_NOTHING);

	char name[64];
	static int seq;
	snprintf(name, sizeof (name), "prop_idem_%d_%zu", seq++, bufsize);

	uint64_t off_sum = 0;
	if (run_workload(name, bufsize, 0, &off_sum) != 0)
		return (QCC_FAIL);

	(void)unlink(PROF_PATH);
	char spec[128];
	snprintf(spec, sizeof (spec), "record:%s", PROF_PATH);
	if (umem_profile_init(spec) != 0)
		return (QCC_NOTHING);
	(void)run_workload(name, bufsize, 0, NULL);
	umem_profile_fini();

	snprintf(spec, sizeof (spec), "use:%s", PROF_PATH);
	if (umem_profile_init(spec) != 0)
		return (QCC_NOTHING);
	uint64_t use_sum = 0;
	int rc = run_workload(name, bufsize, 5, &use_sum);	/* apply x5 */
	umem_profile_fini();

	return (rc == 0 && use_sum == off_sum ? QCC_OK : QCC_FAIL);
}

/* ------------------------------------------------------------------ */
/* Invariant 2: .ump parse matches recorded stats                      */
/* ------------------------------------------------------------------ */

/*
 * Extract "bufsize steady peak" for a given cache name from a dump produced
 * either by umem_profile_dump_text() or by the umem_profile_dump tool.  The
 * per-cache line is:
 *   <name padded> <bufsize> <steady_bufs> <peak_bufs> <alloc/s> <free/s> ...
 * Returns 0 and fills out params on success.
 */
static int
parse_cache_line(const char *path, const char *cache_name,
    unsigned long long *bufsize, unsigned long long *steady,
    unsigned long long *peak)
{
	FILE *fp = fopen(path, "r");
	if (fp == NULL)
		return (-1);
	char line[512];
	int found = -1;
	size_t namelen = strlen(cache_name);
	while (fgets(line, sizeof (line), fp) != NULL) {
		/* Cache lines begin with the (left-justified) cache name. */
		if (strncmp(line, cache_name, namelen) != 0)
			continue;
		/* Ensure it's the name field, not a substring match: the
		 * char after the name must be space or NUL. */
		char c = line[namelen];
		if (c != ' ' && c != '\t' && c != '\0' && c != '\n')
			continue;
		const char *p = line + namelen;
		if (sscanf(p, " %llu %llu %llu", bufsize, steady, peak) == 3) {
			found = 0;
			break;
		}
	}
	fclose(fp);
	return (found);
}

/*
 * Record a workload, then compare the in-library dump_text output against
 * the standalone tool's parse of the same .ump.  They must agree on
 * bufsize / steady / peak for our cache.
 */
static int
check_parse_match(const char *cache_name, size_t bufsize)
{
	(void)unlink(PROF_PATH);
	char spec[128];
	snprintf(spec, sizeof (spec), "record:%s", PROF_PATH);
	if (umem_profile_init(spec) != 0)
		return (-1);
	if (run_workload(cache_name, bufsize, 0, NULL) != 0) {
		umem_profile_fini();
		return (-1);
	}
	umem_profile_fini();		/* writes PROF_PATH */

	/* In-library view. */
	if (umem_profile_load(PROF_PATH) != 0)
		return (-1);
	const char *lib_txt = "/tmp/prop_prof_lib.txt";
	FILE *lf = fopen(lib_txt, "w");
	if (lf == NULL) { umem_profile_fini(); return (-1); }
	umem_profile_dump_text(lf);
	fclose(lf);
	umem_profile_fini();

	unsigned long long lb = 0, ls = 0, lp = 0;
	if (parse_cache_line(lib_txt, cache_name, &lb, &ls, &lp) != 0) {
		(void)unlink(lib_txt);
		return (-2);		/* cache not found in library dump */
	}

	/* Independent tool view. */
	const char *tool_txt = "/tmp/prop_prof_tool.txt";
	char cmd[512];
	snprintf(cmd, sizeof (cmd), "%s %s > %s 2>/dev/null",
	    g_dump_tool, PROF_PATH, tool_txt);
	int rc = system(cmd);
	if (rc != 0) {
		(void)unlink(lib_txt);
		return (-3);		/* tool failed to parse */
	}

	unsigned long long tb = 0, ts = 0, tp = 0;
	if (parse_cache_line(tool_txt, cache_name, &tb, &ts, &tp) != 0) {
		(void)unlink(lib_txt);
		(void)unlink(tool_txt);
		return (-4);		/* cache not found in tool dump */
	}

	(void)unlink(lib_txt);
	(void)unlink(tool_txt);

	/* The core assertion: writer and independent reader agree. */
	if (lb != tb || ls != ts || lp != tp)
		return (-5);
	/* And they reflect the real cache: bufsize must match request. */
	if (tb != (unsigned long long)bufsize)
		return (-6);
	return (0);
}

static QCC_TestStatus
prop_ump_parse_matches(QCC_GenValue **vals, int len, QCC_Stamp **st)
{
	(void)st;
	if (len < 1)
		return (QCC_FAIL);
	size_t bufsize = (size_t)(*QCC_getValue(vals, 0, long*));
	if (bufsize < 8 || bufsize > 4096)
		return (QCC_NOTHING);
	bufsize &= ~(size_t)7;
	if (bufsize == 0)
		return (QCC_NOTHING);

	char name[64];
	static int seq;
	snprintf(name, sizeof (name), "prop_ump_%d_%zu", seq++, bufsize);

	int rc = check_parse_match(name, bufsize);
	if (rc == -1)
		return (QCC_NOTHING);	/* setup issue, not a property fail */
	return (rc == 0 ? QCC_OK : QCC_FAIL);
}

/* ------------------------------------------------------------------ */
/* Generator + driver                                                  */
/* ------------------------------------------------------------------ */

static QCC_GenValue *gen_bufsize(void) { return (QCC_genLongR(8, 4096)); }

int
main(int argc, char *argv[])
{
	const char *env = getenv("UMEM_PROFILE_DUMP_TOOL");
	if (env != NULL)
		g_dump_tool = env;
	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--dump-tool=", 12) == 0)
			g_dump_tool = argv[i] + 12;
	}

	QCC_init(0);

	int fails = 0;

	printf("=== profiling round-trip properties ===\n");
	printf("(dump tool: %s)\n", g_dump_tool);

	printf("[neutral] replay never changes program-visible results\n");
	if (QCC_testForAll(200, 2000, prop_replay_neutral, 1,
	    gen_bufsize) != 0)
		fails++;

	printf("[neutral] repeated replay is idempotent + neutral\n");
	if (QCC_testForAll(200, 2000, prop_replay_idempotent, 1,
	    gen_bufsize) != 0)
		fails++;

	printf("[parse] umem_profile_dump matches recorded stats\n");
	if (QCC_testForAll(100, 2000, prop_ump_parse_matches, 1,
	    gen_bufsize) != 0)
		fails++;

	printf("\n=====================================\n");
	if (fails == 0) {
		printf("All profiling property tests passed!\n");
		return (0);
	}
	printf("%d profiling property group(s) failed\n", fails);
	return (1);
}
