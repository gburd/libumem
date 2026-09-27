/*
 * sm_560_probe.c -- upstream report for sparsemap v5.6.0.
 * See docs/reviews/2026-09-27-sparsemap-5.6.0-review.md (libumem).
 *
 * Public API only; hand this file to upstream as-is.  Build it against an
 * unmodified sm.c/sm.h pair:
 *
 *   cc -std=c11 -D_GNU_SOURCE -O2 -I<sparsemap> -o probe sm_560_probe.c <sparsemap>/sm.c
 *   ./probe [P3_timeout_seconds, default 10]
 *
 * Also run under -O1 -fsanitize=address,undefined; the result is the same.
 * v5.5.0: P1 and P2 PASS, P3 FAIL (the pre-existing S4 class).
 * v5.6.0: P1 and P2 FAIL (regressions), sm_equals in P3 PASS (the S4 fix
 * works for it), sm_is_subset and sm_overlap in P3 still FAIL.  Exit status
 * is the number of FAILs.
 *
 *   P1 split_gap      sm_split at an index that lies in the gap between two
 *                     chunks must keep the lower chunk in `map`.
 *   P2 equals_enc     sm_equals must be content-based (sm.h: "an RLE chunk and
 *                     an equivalent sparse chunk encoding the same bits compare
 *                     equal"); sm_hash must agree with it.
 *   P3 amplify        sm_equals / sm_is_subset / sm_overlap on a valid 24-byte
 *                     map that declares a 2^31-bit run must terminate quickly
 *                     (the S4 class that 5.6.0 says it closed).
 */
#include <sm.h>

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define REPORT(ok, ...)                                                   \
	do {                                                              \
		printf("%s ", (ok) ? "PASS" : "FAIL");                    \
		printf(__VA_ARGS__);                                      \
		printf("\n");                                             \
		if (!(ok))                                                \
			fails++;                                          \
	} while (0)

/* Bit-by-bit content equality: the pre-5.6.0 sm_equals body. */
static int
ref_equals(const sm_t *a, const sm_t *b)
{
	uint64_t ia = sm_next_member(a, SM_IDX_MAX, NULL);
	uint64_t ib = sm_next_member(b, SM_IDX_MAX, NULL);
	while (ia != SM_IDX_MAX && ib != SM_IDX_MAX) {
		if (ia != ib)
			return (0);
		ia = sm_next_member(a, ia, NULL);
		ib = sm_next_member(b, ib, NULL);
	}
	return (ia == ib);
}

static sm_t *
from_range(uint64_t lo, uint64_t hi)
{
	sm_t *m = sm_create(1024);
	for (uint64_t i = lo; i < hi; i++)
		if (sm_add_grow(&m, i) != i)
			abort();
	return (m);
}

static void
p1_split_gap(void)
{
	sm_t *m = sm_create(1 << 16);
	sm_t *o = sm_create(1 << 16);
	if (sm_add(m, 1) != 1 || sm_add(m, 8193) != 8193)
		abort();
	uint64_t rc = sm_split(m, 5000, o);
	int lo_in_m = sm_contains(m, 1, NULL);
	int lo_in_o = sm_contains(o, 1, NULL);
	int hi_in_o = sm_contains(o, 8193, NULL);
	REPORT(rc == 5000 && lo_in_m && !lo_in_o && hi_in_o,
	    "P1 split_gap: {1,8193} split@5000 rc=%llu  map{1}=%d other{1}=%d "
	    "other{8193}=%d  |map|=%zu |other|=%zu",
	    (unsigned long long)rc, lo_in_m, lo_in_o, hi_in_o,
	    sm_cardinality(m), sm_cardinality(o));
	sm_free(m);
	sm_free(o);
}

static void
cmp_pair(const char *name, sm_t *a, sm_t *b)
{
	int ref = ref_equals(a, b);
	int eq = sm_equals(a, b);
	int heq = sm_hash(a) == sm_hash(b);
	REPORT(ref == eq && (!ref || heq),
	    "P2 equals_enc/%s: |a|=%zu |b|=%zu valid=%d/%d ref_equals=%d "
	    "sm_equals=%d hash_equal=%d",
	    name, sm_cardinality(a), sm_cardinality(b), sm_validate(a),
	    sm_validate(b), ref, eq, heq);
}

static void
p2_equals_encoding(void)
{
	/* (a) library-built RLE [0,4096) vs two full sparse chunks with the
	 * same bits, as a valid serialized body would carry them. */
	{
		sm_t *a = from_range(0, 4096);
		uint8_t body[40];
		uint64_t w[5] = { 2, 0, ~0ULL, 2048, ~0ULL };
		memcpy(body, w, sizeof w);
		sm_t *b = sm_open_copy(body, sizeof body, 256);
		if (b == NULL) {
			REPORT(0, "P2 equals_enc/open: sm_open_copy rejected "
			    "two full sparse chunks");
		} else {
			cmp_pair("rle_vs_2sparse", a, b);
			sm_free(b);
		}
		sm_free(a);
	}
	/* (b) library-only: same bits, reached by different edit histories. */
	{
		sm_t *a = from_range(0, 8192);
		sm_t *b = from_range(0, 8192);
		sm_remove(b, 3000);
		if (sm_add_grow(&b, 3000) != 3000)
			abort();
		cmp_pair("remove_readd", a, b);
		sm_free(a);
		sm_free(b);
	}
	{
		sm_t *a = from_range(0, 5000);
		sm_t *lo = from_range(0, 100);
		sm_t *x = sm_xor(a, lo); /* [100, 5000) */
		sm_t *c = from_range(100, 5000);
		cmp_pair("xor_vs_add", x, c);
		sm_free(a);
		sm_free(lo);
		sm_free(x);
		sm_free(c);
	}
	{
		/* build ascending vs descending */
		sm_t *a = from_range(0, 6000);
		sm_t *b = sm_create(1024);
		for (uint64_t i = 6000; i-- > 0;)
			if (sm_add_grow(&b, i) != i)
				abort();
		cmp_pair("asc_vs_desc", a, b);
		sm_free(a);
		sm_free(b);
	}
}

/* test_amplify.c's make_big_rle: one valid RLE chunk, cap == len == 2^31-1. */
static sm_t *
big_rle(uint64_t start)
{
	uint8_t body[24];
	uint64_t w[3] = { 1, start,
		0x4000000000000000ULL | (0x7FFFFFFFULL << 31) | 0x7FFFFFFFULL };
	memcpy(body, w, sizeof w);
	return (sm_open_copy(body, sizeof body, 64));
}

enum { OP_EQUALS, OP_SUBSET, OP_OVERLAP_DISJOINT };
static const char *op_name[] = { "sm_equals(big,big)",
	"sm_is_subset(big,big)", "sm_overlap(big,{2^31+5})" };

static void
p3_one(int op, int limit_s)
{
	struct timespec t0, t1;
	pid_t pid = fork();
	if (pid == 0) {
		alarm((unsigned)limit_s);
		sm_t *a = big_rle(0), *b = big_rle(0);
		sm_t *s = sm_create(1024);
		if (a == NULL || b == NULL || s == NULL)
			_exit(3);
		sm_add(s, (1ULL << 31) + 5);
		volatile int r = 0;
		switch (op) {
		case OP_EQUALS: r = sm_equals(a, b); break;
		case OP_SUBSET: r = sm_is_subset(a, b); break;
		case OP_OVERLAP_DISJOINT: r = sm_overlap(a, s); break;
		}
		(void)r;
		_exit(0);
	}
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int st = 0;
	waitpid(pid, &st, 0);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double ms = (t1.tv_sec - t0.tv_sec) * 1e3 +
	    (t1.tv_nsec - t0.tv_nsec) / 1e6;
	int timed_out = WIFSIGNALED(st) && WTERMSIG(st) == SIGALRM;
	REPORT(WIFEXITED(st) && WEXITSTATUS(st) == 0 && ms < 1000.0,
	    "P3 amplify: %-26s %s %.1f ms", op_name[op],
	    timed_out ? "TIMEOUT(killed by alarm) after" : "finished in", ms);
}

int
main(int argc, char **argv)
{
	int limit = argc > 1 ? atoi(argv[1]) : 10;
	printf("sparsemap %s\n", SM_VERSION_STRING);
	p1_split_gap();
	p2_equals_encoding();
	p3_one(OP_EQUALS, limit);
	p3_one(OP_SUBSET, limit);
	p3_one(OP_OVERLAP_DISJOINT, limit);
	printf("FAILS=%d\n", fails);
	return (fails);
}
