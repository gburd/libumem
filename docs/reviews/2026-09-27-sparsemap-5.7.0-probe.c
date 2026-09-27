/*
 * sparsemap-repro.c -- public-API reproducer for sparsemap 5.7.0.
 *
 * Self-contained: needs only an unmodified sm.c/sm.h pair.
 *
 *   cc -std=c11 -D_GNU_SOURCE -O2 -I<sparsemap> -o repro sparsemap-repro.c <sparsemap>/sm.c
 *   ./repro [amplify_timeout_s=10] [fuzz_trials=300] [fuzz_seed_base=0]
 *
 *   sanitizers: cc -std=c11 -D_GNU_SOURCE -O1 -g -fsanitize=address,undefined \
 *               -fno-omit-frame-pointer -I<sparsemap> -o repro-san sparsemap-repro.c <sparsemap>/sm.c
 *
 * Builds unchanged against 5.5.0, 5.6.0 and 5.7.0.  Each check prints PASS,
 * FAIL or INFO; the exit status is the number of FAILs.
 *
 *   R1 split_gap    sm_split at an index in the gap between two chunks keeps
 *                   the lower chunk in `map`.
 *   R2 equals_enc   sm_equals/sm_hash are content-based: an RLE chunk and the
 *                   same bits as sparse chunks compare and hash equal.
 *   R3 amplify      sm_equals / sm_is_subset / sm_overlap on a valid 24-byte
 *                   map declaring a 2^31-bit run finish in < 1 s.
 *   R4 wire         the serialized form of {5,70}: is the body a v2 chunk
 *                   stream (readable by 5.6.x)?  Can this build decode the
 *                   bytes 5.7.0 writes, and the bytes 5.6.x writes?
 *   R5 small_alloc  sm_rank/sm_select/sm_span/sm_scan on a small set: how
 *                   many allocations per call, and are the answers still
 *                   correct when malloc fails?
 *   R6 rle_len      a valid RLE chunk of length 0 must read as empty in every
 *                   reader; removing the only bit of a length-1 RLE chunk
 *                   empties the map.
 *   R7 offset_big   sm_offset by ~SSIZE_MAX of a chunk above 0 (look for
 *                   "runtime error" under UBSan).
 *   R9 fixed_buf    sm_add into a fixed caller buffer (sm_wrap, no grow)
 *                   that the chunk form fits in must not fail with ENOSPC.
 *   R10 hash_vals   INFO: sm_hash of fixed sets, to compare across versions
 *                   (a persisted sm_hash, e.g. a hash index, must be rebuilt
 *                   if these change).
 *   R11 misaligned  sm_wrap/sm_open over a 4-byte-aligned buffer (e.g. a
 *                   PostgreSQL varlena's VARDATA).  INFO only: under UBSan
 *                   look for "misaligned address" runtime errors.
 *   R12 diff_align  sm_difference of two RLE runs sharing a start: the
 *                   result must pass sm_validate and survive sm_serialize ->
 *                   sm_deserialize.
 *   R8 fuzz         random mutations over [0, 5120) checked after every step
 *                   against a bool[] oracle: validate, content, rank/select,
 *                   equals/hash/compare vs a rebuilt map, serialize round
 *                   trip, set algebra, split, offset.
 */
#include <sm.h>

#include <signal.h>
#include <stdbool.h>
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
		fflush(stdout);                                           \
		if (!(ok))                                                \
			fails++;                                          \
	} while (0)
#define INFO(...)                                                         \
	do {                                                              \
		printf("INFO ");                                          \
		printf(__VA_ARGS__);                                      \
		printf("\n");                                             \
		fflush(stdout);                                           \
	} while (0)

#define RLE_DESC(cap, len)                                                \
	(0x4000000000000000ULL | ((uint64_t)(cap) << 31) | (uint64_t)(len))

static double
now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (t.tv_sec * 1e3 + t.tv_nsec / 1e6);
}

/* Bit-by-bit content equality. */
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

/* ---------------------------------------------------------------- R1 */
static void
r1_split_gap(void)
{
	sm_t *m = sm_create(1 << 16), *o = sm_create(1 << 16);
	if (sm_add(m, 1) != 1 || sm_add(m, 8193) != 8193)
		abort();
	uint64_t rc = sm_split(m, 5000, o);
	int lo_in_m = sm_contains(m, 1, NULL);
	int lo_in_o = sm_contains(o, 1, NULL);
	int hi_in_o = sm_contains(o, 8193, NULL);
	REPORT(rc == 5000 && lo_in_m && !lo_in_o && hi_in_o,
	    "R1 split_gap: {1,8193} split@5000 rc=%llu map{1}=%d other{1}=%d "
	    "other{8193}=%d |map|=%zu |other|=%zu",
	    (unsigned long long)rc, lo_in_m, lo_in_o, hi_in_o,
	    sm_cardinality(m), sm_cardinality(o));
	sm_free(m);
	sm_free(o);
}

/* ---------------------------------------------------------------- R2 */
static void
cmp_pair(const char *name, sm_t *a, sm_t *b)
{
	int ref = ref_equals(a, b);
	int eq = sm_equals(a, b);
	int heq = sm_hash(a) == sm_hash(b);
	REPORT(ref == eq && (!ref || heq),
	    "R2 equals_enc/%s: |a|=%zu |b|=%zu valid=%d/%d ref_equals=%d "
	    "sm_equals=%d hash_equal=%d",
	    name, sm_cardinality(a), sm_cardinality(b), sm_validate(a),
	    sm_validate(b), ref, eq, heq);
}

static void
r2_equals_encoding(void)
{
	{ /* RLE [0,4096) vs the same bits as two full sparse chunks */
		sm_t *a = from_range(0, 4096);
		uint64_t w[5] = { 2, 0, ~0ULL, 2048, ~0ULL };
		sm_t *b = sm_open_copy((uint8_t *)w, sizeof w, 256);
		if (b == NULL) {
			REPORT(0, "R2 equals_enc/open: sm_open_copy rejected "
			    "two full sparse chunks");
		} else {
			cmp_pair("rle_vs_2sparse", a, b);
			sm_free(b);
		}
		sm_free(a);
	}
	{
		sm_t *a = from_range(0, 8192), *b = from_range(0, 8192);
		sm_remove(b, 3000);
		if (sm_add_grow(&b, 3000) != 3000)
			abort();
		cmp_pair("remove_readd", a, b);
		sm_free(a);
		sm_free(b);
	}
	{
		sm_t *a = from_range(0, 5000), *lo = from_range(0, 100);
		sm_t *x = sm_xor(a, lo), *c = from_range(100, 5000);
		cmp_pair("xor_vs_add", x, c);
		sm_free(a);
		sm_free(lo);
		sm_free(x);
		sm_free(c);
	}
	{
		sm_t *a = from_range(0, 6000), *b = sm_create(1024);
		for (uint64_t i = 6000; i-- > 0;)
			if (sm_add_grow(&b, i) != i)
				abort();
		cmp_pair("asc_vs_desc", a, b);
		sm_free(a);
		sm_free(b);
	}
}

/* ---------------------------------------------------------------- R3 */
static sm_t *
big_rle(uint64_t start)
{
	uint64_t w[3] = { 1, start, RLE_DESC(0x7FFFFFFFULL, 0x7FFFFFFFULL) };
	return (sm_open_copy((uint8_t *)w, sizeof w, 64));
}

enum { OP_EQUALS, OP_SUBSET, OP_OVERLAP_DISJOINT };
static const char *op_name[] = { "sm_equals(big,big)",
	"sm_is_subset(big,big)", "sm_overlap(big,{2^31+5})" };

static void
r3_one(int op, int limit_s)
{
	double t0 = now_ms();
	pid_t pid = fork();
	if (pid == 0) {
		alarm((unsigned)limit_s);
		sm_t *a = big_rle(0), *b = big_rle(0), *s = sm_create(1024);
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
	int st = 0;
	waitpid(pid, &st, 0);
	double ms = now_ms() - t0;
	int timed_out = WIFSIGNALED(st) && WTERMSIG(st) == SIGALRM;
	REPORT(WIFEXITED(st) && WEXITSTATUS(st) == 0 && ms < 1000.0,
	    "R3 amplify: %-26s %s %.1f ms", op_name[op],
	    timed_out ? "TIMEOUT(killed by alarm) after" : "finished in", ms);
}

/* ---------------------------------------------------------------- R4 */
/* The 40 bytes 5.7.0's sm_serialize writes for {5,70} on a little-endian
 * host: 16-byte wire header (magic "sm10", version 2, LE flag, byte 6 = 1,
 * cardinality 2) + small-set body (header word 2^63|2, two bitmap words). */
static const uint8_t wire570_5_70[40] = {
	0x73, 0x6d, 0x31, 0x30, 0x02, 0x01, 0x01, 0x00,
	0x02, 0, 0, 0, 0, 0, 0, 0,
	0x02, 0, 0, 0, 0, 0, 0, 0x80,
	0x20, 0, 0, 0, 0, 0, 0, 0,
	0x40, 0, 0, 0, 0, 0, 0, 0,
};

static int
is_5_70(sm_t *m)
{
	return (m != NULL && sm_cardinality(m) == 2 &&
	    sm_contains(m, 5, NULL) && sm_contains(m, 70, NULL));
}

static void
r4b_read570(void)
{
	sm_t *d = sm_deserialize(wire570_5_70, sizeof wire570_5_70);
	sm_t *oc = sm_open_copy(wire570_5_70 + 16, 24, 0);
	_Alignas(uint64_t) uint8_t raw[24];
	memcpy(raw, wire570_5_70 + 16, 24);
	sm_t *wr = sm_wrap(raw, sizeof raw);
	sm_open(wr, raw, sizeof raw);
	REPORT(is_5_70(d) && is_5_70(oc) && is_5_70(wr),
	    "R4b wire/read-5.7.0-bytes {5,70}: sm_deserialize=%s "
	    "sm_open_copy=%s sm_open(|m|=%zu)",
	    d ? (is_5_70(d) ? "{5,70}" : "WRONG") : "NULL",
	    oc ? (is_5_70(oc) ? "{5,70}" : "WRONG") : "NULL",
	    sm_cardinality(wr));
	if (d)
		sm_free(d);
	if (oc)
		sm_free(oc);
	sm_free(wr);
}

static void
r4_wire(void)
{
	/* (a) what this build writes */
	sm_t *m = sm_create(1024);
	sm_add(m, 5);
	sm_add(m, 70);
	size_t n = sm_serialized_size(m);
	uint8_t *buf = calloc(1, n);
	size_t w = sm_serialize(m, buf, n);
	uint64_t hdr = 0;
	memcpy(&hdr, buf + 16, 8);
	printf("INFO R4 sm_serialize({5,70}) = %zu bytes:", w);
	for (size_t i = 0; i < w; i++)
		printf(" %02x", buf[i]);
	printf("\n");
	REPORT((hdr >> 63) == 0,
	    "R4a wire/write: body header word = 0x%016llx (%s); a 5.6.x "
	    "reader needs a chunk count here",
	    (unsigned long long)hdr,
	    (hdr >> 63) ? "small-set flag set" : "v2 chunk count");

	/* (b) read the bytes 5.7.0 writes.  Forked: a reader that does not
	 * validate may walk a 2^63 chunk count. */
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		fails = 0;
		r4b_read570();
		fflush(stdout);
		_exit(fails);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	if (WIFEXITED(st))
		fails += WEXITSTATUS(st);
	else
		REPORT(0, "R4b wire/read-5.7.0-bytes: reader crashed, signal %d",
		    WIFSIGNALED(st) ? WTERMSIG(st) : -1);

	/* (c) read the v2 chunk-stream body 5.6.x writes for {5,70} */
	uint64_t c[5] = { 1, 0, 0x0A, 1ULL << 5, 1ULL << 6 };
	sm_t *cc = sm_open_copy((uint8_t *)c, sizeof c, 0);
	REPORT(is_5_70(cc) && sm_equals(cc, m) && sm_hash(cc) == sm_hash(m),
	    "R4c wire/read-5.6-bytes {5,70}: decoded=%d equals(built)=%d "
	    "hash_equal=%d",
	    is_5_70(cc), cc ? sm_equals(cc, m) : 0,
	    cc ? sm_hash(cc) == sm_hash(m) : 0);
	if (cc)
		sm_free(cc);
	free(buf);
	sm_free(m);
}

/* ---------------------------------------------------------------- R5 */
static int fail_alloc;
static unsigned long n_alloc;
static void *
t_malloc(size_t n)
{
	n_alloc++;
	return (fail_alloc ? NULL : malloc(n));
}
static void *
t_realloc(void *p, size_t n)
{
	n_alloc++;
	return (fail_alloc ? NULL : realloc(p, n));
}
static void
t_free(void *p)
{
	free(p);
}

static size_t scan_n;
static void
count_scan(uint64_t v[], size_t n, void *aux)
{
	(void)v;
	(void)aux;
	scan_n += n;
}

static void
r5_small_alloc(void)
{
	sm_allocator_t hooks = { t_malloc, t_realloc, t_free };
	sm_allocator_t libc = { 0 };
	/* {5,70,71}: 5.7.0 keeps it in small-set form (24 B vs 40 B as a
	 * chunk); earlier versions store one sparse chunk. */
	sm_t *m = sm_create(1024);
	sm_add(m, 5);
	sm_add(m, 70);
	sm_add(m, 71);
	sm_set_allocator(hooks);

	n_alloc = 0;
	(void)sm_rank(m, 0, 1023, true);
	unsigned long a_rank = n_alloc;
	n_alloc = 0;
	(void)sm_select(m, 1, true);
	unsigned long a_sel = n_alloc;

	fail_alloc = 1;
	size_t rank = sm_rank(m, 0, 1023, true);
	uint64_t sel = sm_select(m, 1, true);
	uint64_t span = sm_span(m, 0, 1, true);
	scan_n = 0;
	sm_scan(m, count_scan, 0, NULL);
	fail_alloc = 0;
	sm_set_allocator(libc);

	REPORT(rank == 3 && sel == 70 && span == 5 && scan_n == 3,
	    "R5 small_alloc {5,70,71} (size %zu B): allocations per call rank=%lu "
	    "select=%lu; with malloc failing: rank=%zu(3) select=%llu(70) "
	    "span=%llu(5) scan=%zu(3)",
	    sm_get_size(m), a_rank, a_sel, rank, (unsigned long long)sel,
	    (unsigned long long)span, scan_n);

	/* cost of a read-only query on a small set */
	sm_t *s = sm_create(1024);
	uint64_t x = 0x9E3779B97F4A7C15ULL;
	for (int i = 0; i < 300; i++) {
		x ^= x >> 12, x ^= x << 25, x ^= x >> 27;
		sm_add(s, (x * 0x2545F4914F6CDD1DULL) % 1024);
	}
	size_t card = sm_cardinality(s);
	const int iters = 20000;
	volatile size_t sink = 0;
	double t0 = now_ms();
	for (int i = 0; i < iters; i++)
		sink += sm_rank(s, 0, (uint64_t)(i % 1024), true);
	double t_rank = (now_ms() - t0) * 1e6 / iters;
	t0 = now_ms();
	for (int i = 0; i < iters; i++)
		sink += sm_select(s, (uint64_t)i % card, true);
	double t_sel = (now_ms() - t0) * 1e6 / iters;
	t0 = now_ms();
	for (int i = 0; i < iters; i++)
		sink += sm_contains(s, (uint64_t)(i % 1024), NULL);
	double t_con = (now_ms() - t0) * 1e6 / iters;
	sm_t *s2 = sm_create(1024);
	for (int i = 0; i < 300; i++) {
		x ^= x >> 12, x ^= x << 25, x ^= x >> 27;
		sm_add(s2, (x * 0x2545F4914F6CDD1DULL) % 1024);
	}
	t0 = now_ms();
	for (int i = 0; i < iters / 10; i++) {
		sm_t *u = sm_union(s, s2);
		sink += sm_get_size(u);
		sm_free(u);
	}
	double t_uni = (now_ms() - t0) * 1e7 / iters;
	(void)sink;
	INFO("R5 cost, %zu random bits below 1024, size=%zu B: sm_rank %.0f ns, "
	    "sm_select %.0f ns, sm_union %.0f ns, sm_contains %.0f ns per call",
	    card, sm_get_size(s), t_rank, t_sel, t_uni, t_con);
	sm_free(s2);
	sm_free(s);
	sm_free(m);
}

/* ---------------------------------------------------------------- R6 */
static uint64_t
walk_count(sm_t *m, uint64_t limit)
{
	uint64_t n = 0, i = sm_next_member(m, SM_IDX_MAX, NULL);
	while (i != SM_IDX_MAX && n < limit) {
		n++;
		i = sm_next_member(m, i, NULL);
	}
	return (n);
}

static void
r6_rle_len(void)
{
	/* (a) RLE chunk, capacity 2048, length 0 */
	uint64_t w0[3] = { 1, 0, RLE_DESC(2048, 0) };
	sm_t *z = sm_open_copy((uint8_t *)w0, sizeof w0, 64);
	if (z == NULL) {
		REPORT(1, "R6a rle_len0: rejected by sm_open_copy");
	} else {
		size_t card = sm_cardinality(z);
		uint64_t walk = walk_count(z, 1u << 20);
		size_t rank = sm_rank(z, 0, 2047, true);
		sm_t *e = sm_create(64);
		REPORT(card == walk && rank == walk,
		    "R6a rle_len0 {start 0, cap 2048, len 0}: accepted, "
		    "valid=%d sm_cardinality=%zu sm_rank(0,2047)=%zu "
		    "next_member walk=%llu sm_contains(0)=%d sm_is_empty=%d "
		    "sm_equals(empty)=%d",
		    sm_validate(z), card, rank, (unsigned long long)walk,
		    sm_contains(z, 0, NULL), sm_is_empty(z), sm_equals(z, e));
		sm_free(e);
		sm_free(z);
	}
	/* (b) RLE chunk, capacity 2048, length 1; remove its only bit */
	uint64_t w1[3] = { 1, 0, RLE_DESC(2048, 1) };
	sm_t *o = sm_open_copy((uint8_t *)w1, sizeof w1, 64);
	if (o == NULL) {
		REPORT(0, "R6b rle_len1: rejected by sm_open_copy");
	} else {
		size_t before = sm_cardinality(o);
		sm_remove(o, 0);
		size_t card = sm_cardinality(o);
		uint64_t walk = walk_count(o, 1u << 20);
		REPORT(before == 1 && card == 0 && walk == 0 && sm_validate(o),
		    "R6b rle_len1 remove(0): |before|=%zu sm_cardinality=%zu "
		    "walk=%llu valid=%d",
		    before, card, (unsigned long long)walk, sm_validate(o));
		sm_free(o);
	}
}

/* ---------------------------------------------------------------- R7 */
static void
r7_offset_big(void)
{
	sm_t *m = sm_create(1024);
	sm_add(m, 5000);
	ssize_t off = (ssize_t)(~(size_t)0 >> 1); /* SSIZE_MAX */
	sm_t *r = sm_offset(m, off);
	uint64_t want = 5000 + (uint64_t)off;
	REPORT(r != NULL && sm_cardinality(r) == 1 &&
	        sm_next_member(r, SM_IDX_MAX, NULL) == want,
	    "R7 offset_big: sm_offset({5000}, SSIZE_MAX) -> %s first=%llu "
	    "(want %llu)",
	    r ? "map" : "NULL",
	    r ? (unsigned long long)sm_next_member(r, SM_IDX_MAX, NULL) : 0ULL,
	    (unsigned long long)want);
	if (r)
		sm_free(r);
	sm_free(m);
}

/* ---------------------------------------------------------------- R9 */
static void
r9_one(const char *name, size_t cap, const uint64_t *bits, int n)
{
	_Alignas(uint64_t) uint8_t buf[256];
	memset(buf, 0, sizeof buf);
	sm_t *m = sm_wrap(buf, cap);
	sm_clear(m);
	int ok = 1;
	char rcs[128] = "";
	for (int i = 0; i < n; i++) {
		uint64_t rc = sm_add(m, bits[i]);
		size_t l = strlen(rcs);
		snprintf(rcs + l, sizeof rcs - l, "%s%s", i ? "," : "",
		    rc == bits[i] ? "ok" : "ENOSPC");
		ok &= rc == bits[i] && sm_contains(m, bits[i], NULL);
	}
	REPORT(ok, "R9 fixed_buf/%s: sm_wrap(%zu B) sm_add -> %s  size=%zu B",
	    name, cap, rcs, sm_get_size(m));
	sm_free(m);
}

static void
r9_fixed_buffer(void)
{
	/* 5.6.0 stores {1000} as one 32-byte sparse chunk and {0,1000} as
	 * 40 bytes; both fit with room to spare. */
	const uint64_t a[] = { 1000 }, b[] = { 0, 1000 };
	r9_one("{1000}", 96, a, 1);
	r9_one("{0,1000}", 128, b, 2);
}

/* --------------------------------------------------------------- R10 */
static void
r10_hash_values(void)
{
	sm_t *a = sm_create(1024), *b = from_range(2000, 2100),
	     *c = from_range(0, 4096);
	sm_add(a, 5);
	sm_add(a, 70);
	INFO("R10 sm_hash {5,70}=%016llx [2000,2100)=%016llx [0,4096)=%016llx",
	    (unsigned long long)sm_hash(a), (unsigned long long)sm_hash(b),
	    (unsigned long long)sm_hash(c));
	sm_free(a);
	sm_free(b);
	sm_free(c);
}

/* --------------------------------------------------------------- R11 */
static void
r11_misaligned(void)
{
	_Alignas(uint64_t) uint8_t raw[8 + 256];
	memset(raw, 0, sizeof raw);
	uint8_t *buf = raw + 4; /* 4-byte aligned, like VARDATA() */
	sm_t *m = sm_wrap(buf, 256);
	sm_clear(m);
	sm_add(m, 5);
	sm_add(m, 70);
	size_t card = sm_cardinality(m);
	int has = sm_contains(m, 70, NULL);
	size_t used = sm_get_size(m);
	sm_t *v = sm_wrap(buf, used);
	sm_open(v, buf, used);
	INFO("R11 misaligned: sm_wrap(buf+4) {5,70}: |m|=%zu contains(70)=%d "
	    "size=%zu, sm_open -> |v|=%zu",
	    card, has, used, sm_cardinality(v));
	sm_free(v);
	sm_free(m);
}

/* --------------------------------------------------------------- R12 */
static void
r12_one(const char *name, sm_t *a, sm_t *b, size_t want, uint64_t wlo,
    uint64_t whi)
{
	sm_t *d = sm_difference(a, b);
	size_t n = d ? sm_serialized_size(d) : 0;
	uint8_t *buf = n ? malloc(n) : NULL;
	sm_t *rt = d ? sm_deserialize(buf, sm_serialize(d, buf, n)) : NULL;
	uint64_t lo = d ? sm_minimum(d) : 0, hi = d ? sm_maximum(d) : 0;
	/* sm_open of the result's own bytes (what a caller that persisted
	 * sm_get_data()/sm_get_size() reads back) */
	size_t on = 0, rt_card = rt ? sm_cardinality(rt) : 0;
	if (d) {
		size_t sz = sm_get_size(d);
		uint8_t *copy = malloc(sz);
		memcpy(copy, sm_get_data(d), sz);
		sm_t *v = sm_wrap(copy, sz);
		sm_open(v, copy, sz);
		on = sm_cardinality(v);
		sm_free(v);
		free(copy);
	}
	printf("INFO R12 %s: sm_difference bytes:", name);
	for (size_t i = 0; d && i < sm_get_size(d); i++)
		printf("%s%02x", i % 8 ? "" : " ",
		    ((const uint8_t *)sm_get_data(d))[i]);
	printf("\n");
	REPORT(d != NULL && sm_cardinality(d) == want && lo == wlo &&
	        hi == whi && sm_validate(d) && rt_card == want && on == want,
	    "R12 diff_align/%s -> |d|=%zu (want %zu) "
	    "min=%llu max=%llu sm_validate=%d; sm_deserialize(sm_serialize(d))"
	    "=%s |%zu|; sm_open(bytes of d) |%zu|",
	    name, d ? sm_cardinality(d) : 0, want, (unsigned long long)lo,
	    (unsigned long long)hi, d ? sm_validate(d) : 0,
	    rt ? "map" : "NULL", rt_card, on);
	if (rt)
		sm_free(rt);
	free(buf);
	if (d)
		sm_free(d);
}

static void
r12_diff_align(void)
{
	/* (a) library-built */
	sm_t *a = sm_create(1024), *b = sm_create(1024);
	sm_add_range(a, 2048, 5092);
	sm_add_range(b, 2048, 4698);
	r12_one("[2048,5092)-[2048,4698)", a, b, 394, 4698, 5091);
	sm_free(a);
	sm_free(b);
	/* (c) a hole in the middle of a run */
	a = sm_create(1024);
	b = sm_create(1024);
	sm_add_range(a, 2048, 5092);
	sm_add_range(b, 3000, 3100);
	r12_one("[2048,5092)-[3000,3100)", a, b, 2944, 2048, 5091);
	sm_free(a);
	sm_free(b);
	/* (b) the same sets as the valid bytes the fuzzer produced: one RLE
	 * chunk at 2048, capacity 4096, lengths 3044 and 2650 */
	uint64_t wa[3] = { 1, 2048, RLE_DESC(4096, 3044) };
	uint64_t wb[3] = { 1, 2048, RLE_DESC(4096, 2650) };
	a = sm_open_copy((uint8_t *)wa, sizeof wa, 0);
	b = sm_open_copy((uint8_t *)wb, sizeof wb, 0);
	if (a == NULL || b == NULL)
		REPORT(0, "R12 diff_align/bytes: sm_open_copy rejected input");
	else
		r12_one("bytes:[2048,5092)-[2048,4698)", a, b, 394, 4698, 5091);
	if (a)
		sm_free(a);
	if (b)
		sm_free(b);
}

/* Siblings: every set op over RLE-run pairs whose boundaries are not
 * chunk-aligned; each result must pass sm_validate. */
static void
r12_siblings(void)
{
	static const uint64_t pr[][4] = {
		{ 2048, 5092, 2048, 4698 }, { 2048, 5092, 3000, 3100 },
		{ 2048, 5092, 4698, 8000 }, { 0, 6000, 2500, 9000 },
		{ 4096, 10000, 0, 5000 }, { 2048, 4200, 4100, 9000 },
	};
	sm_t *(*fn[4])(const sm_t *, const sm_t *) = { sm_union,
		sm_intersection, sm_difference, sm_xor };
	const char *fname[4] = { "union", "intersection", "difference", "xor" };
	int bad[4] = { 0 }, total = 0;
	for (size_t k = 0; k < sizeof pr / sizeof pr[0]; k++) {
		sm_t *a = sm_create(1024), *b = sm_create(1024);
		sm_add_range(a, pr[k][0], pr[k][1]);
		sm_add_range(b, pr[k][2], pr[k][3]);
		for (int f = 0; f < 4; f++) {
			sm_t *r = fn[f](a, b);
			total++;
			if (r != NULL && !sm_validate(r)) {
				if (bad[f]++ == 0)
					printf("  R12 first invalid %s: "
					    "[%llu,%llu) op [%llu,%llu)\n",
					    fname[f],
					    (unsigned long long)pr[k][0],
					    (unsigned long long)pr[k][1],
					    (unsigned long long)pr[k][2],
					    (unsigned long long)pr[k][3]);
			}
			if (r)
				sm_free(r);
		}
		sm_free(a);
		sm_free(b);
	}
	REPORT(bad[0] + bad[1] + bad[2] + bad[3] == 0,
	    "R12 siblings: results failing sm_validate: union=%d "
	    "intersection=%d difference=%d xor=%d (of %d each)",
	    bad[0], bad[1], bad[2], bad[3], total / 4);
}

/* ---------------------------------------------------------------- R8 */
#define U 5120
static uint64_t rs = 0x243F6A8885A308D3ULL;
static uint64_t
rnd(void)
{
	rs ^= rs >> 12;
	rs ^= rs << 25;
	rs ^= rs >> 27;
	return (rs * 0x2545F4914F6CDD1DULL);
}
static uint64_t
rndn(uint64_t n)
{
	return (rnd() % n);
}
static uint64_t
rpos(void)
{
	return (rndn(10) < 3 ? rndn(1100) : rndn(U)); /* bias to small cap */
}

enum { C_MUT, C_QRY, C_EQ, C_SER, C_SET, C_SPLIT, C_OFF, NCAT };
static const char *cname[NCAT] = { "mutate+validate", "rank/select/prev",
	"equals/hash/compare", "serialize", "set-algebra", "split", "offset" };
static unsigned long c_fail[NCAT], c_run[NCAT];
static char why[256];

static void
fz(int cat, bool ok, uint64_t seed, int step, const char *what)
{
	c_run[cat]++;
	if (ok)
		return;
	if (c_fail[cat]++ == 0)
		printf("  R8 first %s failure (trial seed %#llx step %d): %s: %s\n",
		    cname[cat], (unsigned long long)seed, step, what, why);
}

/* m == {i + shift : o[i], lo <= i < hi, i + shift >= 0}, cardinality, valid */
static bool
content(sm_t *m, const bool *o, int64_t shift, uint64_t lo, uint64_t hi)
{
	uint64_t cnt = 0;
	uint64_t got = m ? sm_next_member(m, SM_IDX_MAX, NULL) : SM_IDX_MAX;
	why[0] = 0;
	for (uint64_t i = lo; i < hi; i++) {
		if (!o[i])
			continue;
		int64_t t = (int64_t)i + shift;
		if (t < 0)
			continue;
		if (got != (uint64_t)t) {
			snprintf(why, sizeof why, "expected member %lld, got %llu",
			    (long long)t, (unsigned long long)got);
			return (false);
		}
		cnt++;
		got = sm_next_member(m, got, NULL);
	}
	if (got != SM_IDX_MAX) {
		snprintf(why, sizeof why, "unexpected member %llu",
		    (unsigned long long)got);
		return (false);
	}
	if (m != NULL && sm_cardinality(m) != cnt) {
		snprintf(why, sizeof why, "sm_cardinality=%zu, members=%llu",
		    sm_cardinality(m), (unsigned long long)cnt);
		return (false);
	}
	if (m != NULL && !sm_validate(m)) {
		snprintf(why, sizeof why, "sm_validate()==false (%llu members)",
		    (unsigned long long)cnt);
		return (false);
	}
	return (true);
}

static sm_t *
build(const bool *o, int how)
{
	sm_t *m = sm_create(1 << 16);
	if (how == 0) {
		for (uint64_t i = 0; i < U; i++)
			if (o[i])
				sm_add(m, i);
	} else if (how == 1) {
		for (uint64_t i = U; i-- > 0;)
			if (o[i])
				sm_add(m, i);
	} else { /* maximal runs via sm_add_range */
		for (uint64_t i = 0; i < U;) {
			if (!o[i]) {
				i++;
				continue;
			}
			uint64_t j = i;
			while (j < U && o[j])
				j++;
			sm_add_range(m, i, j);
			i = j;
		}
	}
	return (m);
}

static void
random_mutate(sm_t *m, bool *o)
{
	unsigned op = (unsigned)rndn(10);
	uint64_t lo = rpos();
	uint64_t r = rndn(10);
	uint64_t len = 1 + (r < 5 ? rndn(64) : r < 8 ? rndn(600) : rndn(4096));
	uint64_t hi = lo + len > U ? U : lo + len;
	if (op < 4) {
		sm_add(m, lo);
		o[lo] = true;
	} else if (op < 6) {
		sm_remove(m, lo);
		o[lo] = false;
	} else if (op < 8) {
		sm_add_range(m, lo, hi);
		for (uint64_t i = lo; i < hi; i++)
			o[i] = true;
	} else if (op < 9) {
		sm_remove_range(m, lo, hi);
		for (uint64_t i = lo; i < hi; i++)
			o[i] = false;
	} else {
		sm_flip_range(m, lo, hi);
		for (uint64_t i = lo; i < hi; i++)
			o[i] = !o[i];
	}
}

static int
sgn(int x)
{
	return ((x > 0) - (x < 0));
}

static int
o_compare(const bool *a, const bool *b)
{
	uint64_t i = 0, j = 0;
	for (;;) {
		while (i < U && !a[i])
			i++;
		while (j < U && !b[j])
			j++;
		if (i == U || j == U)
			return (i == U && j == U) ? 0 : (i == U ? -1 : 1);
		if (i != j)
			return (i < j ? -1 : 1);
		i++;
		j++;
	}
}

static void
heavy(sm_t *m, const bool *o, uint64_t seed, int step)
{
	static bool ob[U], tmp[U];
	uint64_t card = 0;
	for (uint64_t i = 0; i < U; i++)
		card += o[i];

	/* rank / select / prev_member / contains / min / max */
	{
		bool ok = true;
		for (int k = 0; k < 8 && ok; k++) {
			uint64_t x = rndn(U), y = x + rndn(U - x), n = 0;
			for (uint64_t i = x; i <= y; i++)
				n += o[i];
			size_t r = sm_rank(m, x, y, true);
			if (r != n) {
				snprintf(why, sizeof why,
				    "sm_rank(%llu,%llu)=%zu want %llu",
				    (unsigned long long)x, (unsigned long long)y,
				    r, (unsigned long long)n);
				ok = false;
			}
			uint64_t p = rndn(U + 1), want = SM_IDX_MAX;
			for (uint64_t i = p; i-- > 0;)
				if (o[i]) {
					want = i;
					break;
				}
			uint64_t got = sm_prev_member(m, p, NULL);
			if (ok && got != want) {
				snprintf(why, sizeof why,
				    "sm_prev_member(%llu)=%llu want %llu",
				    (unsigned long long)p,
				    (unsigned long long)got,
				    (unsigned long long)want);
				ok = false;
			}
			if (ok && sm_contains(m, p % U, NULL) != o[p % U]) {
				snprintf(why, sizeof why, "sm_contains(%llu)",
				    (unsigned long long)(p % U));
				ok = false;
			}
			if (ok && card > 0) {
				uint64_t nth = rndn(card), c = 0, at = 0;
				for (at = 0; at < U; at++)
					if (o[at] && c++ == nth)
						break;
				uint64_t s = sm_select(m, nth, true);
				if (s != at) {
					snprintf(why, sizeof why,
					    "sm_select(%llu)=%llu want %llu",
					    (unsigned long long)nth,
					    (unsigned long long)s,
					    (unsigned long long)at);
					ok = false;
				}
			}
		}
		if (ok && card > 0) {
			uint64_t mn = 0, mx = 0;
			while (!o[mn])
				mn++;
			for (mx = U - 1; !o[mx]; mx--)
				;
			if (sm_minimum(m) != mn || sm_maximum(m) != mx) {
				snprintf(why, sizeof why,
				    "min/max %llu/%llu want %llu/%llu",
				    (unsigned long long)sm_minimum(m),
				    (unsigned long long)sm_maximum(m),
				    (unsigned long long)mn,
				    (unsigned long long)mx);
				ok = false;
			}
		}
		fz(C_QRY, ok, seed, step, "query");
	}

	/* equals / hash / compare / subset_compare vs rebuilt maps */
	for (int how = 0; how < 3; how++) {
		sm_t *b = build(o, how);
		bool ok = sm_equals(m, b) && sm_equals(b, m) &&
		    sm_hash(m) == sm_hash(b) && sm_compare(m, b) == 0 &&
		    sm_subset_compare(m, b) == SM_REL_EQUAL;
		snprintf(why, sizeof why,
		    "vs rebuilt(%s): equals=%d/%d hash_eq=%d compare=%d "
		    "subset_compare=%d (|m|=%llu)",
		    how == 0 ? "asc" : how == 1 ? "desc" : "ranges",
		    sm_equals(m, b), sm_equals(b, m), sm_hash(m) == sm_hash(b),
		    sm_compare(m, b), (int)sm_subset_compare(m, b),
		    (unsigned long long)card);
		fz(C_EQ, ok, seed, step, "rebuild");
		sm_free(b);
	}

	/* serialize round trip; open_copy of the raw buffer */
	{
		size_t n = sm_serialized_size(m);
		uint8_t *buf = malloc(n);
		size_t w = sm_serialize(m, buf, n);
		sm_t *d = sm_deserialize(buf, w);
		bool ok = w == n && d != NULL && content(d, o, 0, 0, U);
		if (d == NULL)
			snprintf(why, sizeof why, "sm_deserialize NULL (w=%zu)",
			    w);
		fz(C_SER, ok, seed, step, "serialize");
		if (d)
			sm_free(d);
		free(buf);
		sm_t *oc = sm_open_copy(sm_get_data(m), sm_get_size(m), 0);
		ok = oc != NULL && content(oc, o, 0, 0, U);
		fz(C_SER, ok, seed, step, "open_copy");
		if (oc)
			sm_free(oc);
	}

	/* set algebra against a second random map */
	{
		memset(ob, 0, sizeof ob);
		sm_t *b = sm_create(1 << 16);
		for (int k = 0, nk = 1 + (int)rndn(12); k < nk; k++)
			random_mutate(b, ob);
		struct {
			const char *name;
			sm_t *(*fn)(const sm_t *, const sm_t *);
			int kind;
		} ops[] = { { "union", sm_union, 0 },
			{ "intersection", sm_intersection, 1 },
			{ "difference", sm_difference, 2 },
			{ "xor", sm_xor, 3 } };
		uint64_t cu = 0, ci = 0, cd = 0, cx = 0;
		bool a_sub_b = true, b_sub_a = true, ovl = false;
		for (uint64_t i = 0; i < U; i++) {
			cu += o[i] || ob[i];
			ci += o[i] && ob[i];
			cd += o[i] && !ob[i];
			cx += o[i] != ob[i];
			if (o[i] && !ob[i])
				a_sub_b = false;
			if (ob[i] && !o[i])
				b_sub_a = false;
			if (o[i] && ob[i])
				ovl = true;
		}
		for (int k = 0; k < 4; k++) {
			for (uint64_t i = 0; i < U; i++) {
				bool x = o[i], y = ob[i];
				tmp[i] = k == 0 ? (x || y) : k == 1 ? (x && y) :
				    k == 2 ? (x && !y) : (x != y);
			}
			sm_t *r = ops[k].fn(m, b);
			bool ok = content(r, tmp, 0, 0, U);
			char w2[256];
			snprintf(w2, sizeof w2, "sm_%s", ops[k].name);
			fz(C_SET, ok, seed, step, w2);
			if (r)
				sm_free(r);
		}
		int rel = a_sub_b && b_sub_a ? SM_REL_EQUAL :
		    a_sub_b ? SM_REL_SUBSET_A : b_sub_a ? SM_REL_SUBSET_B :
		    SM_REL_DIFFERENT;
		bool ok = sm_union_cardinality(m, b) == cu &&
		    sm_intersection_cardinality(m, b) == ci &&
		    sm_difference_cardinality(m, b) == cd &&
		    sm_xor_cardinality(m, b) == cx &&
		    sm_is_subset(m, b) == a_sub_b && sm_overlap(m, b) == ovl &&
		    sm_nonempty_difference(m, b) == (cd > 0) &&
		    (int)sm_subset_compare(m, b) == rel &&
		    sgn(sm_compare(m, b)) == o_compare(o, ob);
		snprintf(why, sizeof why,
		    "card u/i/d/x %zu/%zu/%zu/%zu want %llu/%llu/%llu/%llu "
		    "subset=%d/%d overlap=%d/%d subset_compare=%d/%d "
		    "compare=%d/%d",
		    sm_union_cardinality(m, b), sm_intersection_cardinality(m, b),
		    sm_difference_cardinality(m, b), sm_xor_cardinality(m, b),
		    (unsigned long long)cu, (unsigned long long)ci,
		    (unsigned long long)cd, (unsigned long long)cx,
		    sm_is_subset(m, b), a_sub_b, sm_overlap(m, b), ovl,
		    (int)sm_subset_compare(m, b), rel, sgn(sm_compare(m, b)),
		    o_compare(o, ob));
		fz(C_SET, ok, seed, step, "predicates");
		sm_free(b);
	}

	/* split at a random index; union of the halves == original */
	{
		sm_t *c = sm_open_copy(sm_get_data(m), sm_get_size(m), 1 << 16);
		sm_t *other = sm_create(1 << 16);
		uint64_t idx = rndn(U + 64);
		uint64_t rc = c ? sm_split(c, idx, other) : SM_IDX_MAX;
		char w2[64];
		snprintf(w2, sizeof w2, "sm_split@%llu lower",
		    (unsigned long long)idx);
		bool ok = rc == idx && content(c, o, 0, 0, idx < U ? idx : U);
		if (rc != idx)
			snprintf(why, sizeof why, "rc=%llu",
			    (unsigned long long)rc);
		fz(C_SPLIT, ok, seed, step, w2);
		if (ok) {
			snprintf(w2, sizeof w2, "sm_split@%llu upper",
			    (unsigned long long)idx);
			ok = idx >= U || content(other, o, 0, idx, U);
			fz(C_SPLIT, ok, seed, step, w2);
		}
		if (ok) {
			sm_t *u = sm_union(c, other);
			ok = content(u, o, 0, 0, U) &&
			    (u == NULL || (sm_equals(u, m) &&
			                      sm_hash(u) == sm_hash(m)));
			if (why[0] == 0)
				snprintf(why, sizeof why,
				    "union(halves) equals=%d hash_eq=%d",
				    u ? sm_equals(u, m) : -1,
				    u ? sm_hash(u) == sm_hash(m) : -1);
			fz(C_SPLIT, ok, seed, step, "union of halves");
			if (u)
				sm_free(u);
		}
		if (c)
			sm_free(c);
		sm_free(other);
	}

	/* offset */
	{
		int64_t d = (int64_t)rndn(6001) - 3000;
		sm_t *r = sm_offset(m, (ssize_t)d);
		char w2[64];
		snprintf(w2, sizeof w2, "sm_offset(%lld)", (long long)d);
		fz(C_OFF, content(r, o, d, 0, U), seed, step, w2);
		if (r)
			sm_free(r);
	}
}

static int seed_base;

static int
r8_fuzz_child(int trials)
{
	static bool o[U];
	for (int t = 0; t < trials; t++) {
		uint64_t seed = 0x9E3779B97F4A7C15ULL * (uint64_t)(t + 1 + seed_base);
		rs = seed;
		memset(o, 0, sizeof o);
		sm_t *m = sm_create(1 << 16);
		for (int step = 0; step < 120; step++) {
			random_mutate(m, o);
			bool ok = content(m, o, 0, 0, U);
			fz(C_MUT, ok, seed, step, "after mutation");
			if (!ok)
				break; /* map diverged; next trial */
			if (step % 8 == 7)
				heavy(m, o, seed, step);
		}
		sm_free(m);
	}
	int bad = 0;
	for (int c = 0; c < NCAT; c++) {
		printf("%s R8 fuzz/%s: %lu failures in %lu checks\n",
		    c_fail[c] ? "FAIL" : "PASS", cname[c], c_fail[c], c_run[c]);
		bad += c_fail[c] != 0;
	}
	fflush(stdout);
	return (bad);
}

static void
r8_fuzz(int trials)
{
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0)
		_exit(r8_fuzz_child(trials));
	int st = 0;
	waitpid(pid, &st, 0);
	if (WIFEXITED(st)) {
		fails += WEXITSTATUS(st);
	} else {
		REPORT(0, "R8 fuzz: child killed by signal %d",
		    WIFSIGNALED(st) ? WTERMSIG(st) : -1);
	}
}

int
main(int argc, char **argv)
{
	int limit = argc > 1 ? atoi(argv[1]) : 10;
	int trials = argc > 2 ? atoi(argv[2]) : 300;
	seed_base = argc > 3 ? atoi(argv[3]) : 0;
	printf("sparsemap %s\n", SM_VERSION_STRING);
	r1_split_gap();
	r2_equals_encoding();
	r3_one(OP_EQUALS, limit);
	r3_one(OP_SUBSET, limit);
	r3_one(OP_OVERLAP_DISJOINT, limit);
	r4_wire();
	r5_small_alloc();
	r6_rle_len();
	r7_offset_big();
	r9_fixed_buffer();
	r10_hash_values();
	r11_misaligned();
	r12_diff_align();
	r12_siblings();
	r8_fuzz(trials);
	printf("FAILS=%d\n", fails);
	return (fails);
}
