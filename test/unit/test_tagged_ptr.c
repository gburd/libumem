/*
 * Test suite for tagged pointer implementation
 * Verifies atomicity, encoding, and ABA prevention
 */

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>

/* Import implementation from umem_impl.h */
#define UMEM_PTR_MASK   0x0000FFFFFFFFFFFFULL
#define UMEM_VER_SHIFT  48

typedef union umem_tagged_ptr {
	uint64_t raw;
	struct {
		uint64_t _bits;
	} _packed;
} umem_tagged_ptr_t;

static inline void *
umem_tagged_ptr_get(umem_tagged_ptr_t tp)
{
	return (void *)(uintptr_t)(tp.raw & UMEM_PTR_MASK);
}

static inline uint16_t
umem_tagged_ver_get(umem_tagged_ptr_t tp)
{
	return (uint16_t)(tp.raw >> UMEM_VER_SHIFT);
}

static inline umem_tagged_ptr_t
umem_tagged_ptr_make(void *ptr, uint16_t ver)
{
	umem_tagged_ptr_t tp;
	tp.raw = ((uint64_t)(uintptr_t)ptr & UMEM_PTR_MASK) |
	    ((uint64_t)ver << UMEM_VER_SHIFT);
	return tp;
}

static inline umem_tagged_ptr_t
atomic_load_tagged_ptr(volatile umem_tagged_ptr_t *ptr)
{
	umem_tagged_ptr_t val;
	val.raw = __sync_fetch_and_add((volatile uint64_t *)&ptr->raw, 0);
	return val;
}

static inline int
atomic_cas_tagged_ptr(volatile umem_tagged_ptr_t *ptr,
                     umem_tagged_ptr_t *expected,
                     umem_tagged_ptr_t desired)
{
	uint64_t old = __sync_val_compare_and_swap((volatile uint64_t *)&ptr->raw,
	    expected->raw, desired.raw);
	if (old == expected->raw) {
		return 1;
	} else {
		expected->raw = old;
		return 0;
	}
}

/* Test structure size and alignment */
static void
test_structure_properties(void)
{
	printf("Test 1: Structure properties\n");
	assert(sizeof(umem_tagged_ptr_t) == 8);
	assert(_Alignof(umem_tagged_ptr_t) == 8);
	printf("  sizeof(umem_tagged_ptr_t) = 8 bytes ✓\n");
	printf("  _Alignof(umem_tagged_ptr_t) = 8 bytes ✓\n");
}

/* Test pointer encoding and decoding */
static void
test_encoding_decoding(void)
{
	printf("\nTest 2: Encoding/decoding\n");

	void *test_ptr = (void *)0x00007fffdeadbeef;
	uint16_t test_ver = 0x1234;

	umem_tagged_ptr_t tp = umem_tagged_ptr_make(test_ptr, test_ver);
	void *decoded_ptr = umem_tagged_ptr_get(tp);
	uint16_t decoded_ver = umem_tagged_ver_get(tp);

	printf("  Original: ptr=%p ver=0x%04x\n", test_ptr, test_ver);
	printf("  Encoded:  raw=0x%016lx\n", tp.raw);
	printf("  Decoded:  ptr=%p ver=0x%04x\n", decoded_ptr, decoded_ver);

	assert(decoded_ptr == test_ptr);
	assert(decoded_ver == test_ver);
	printf("  Encoding/decoding correct ✓\n");
}

/* Test NULL pointer handling */
static void
test_null_pointer(void)
{
	printf("\nTest 3: NULL pointer handling\n");

	umem_tagged_ptr_t tp = umem_tagged_ptr_make(NULL, 42);
	void *decoded_ptr = umem_tagged_ptr_get(tp);
	uint16_t decoded_ver = umem_tagged_ver_get(tp);

	printf("  NULL with version 42: raw=0x%016lx\n", tp.raw);
	assert(decoded_ptr == NULL);
	assert(decoded_ver == 42);
	printf("  NULL handling correct ✓\n");
}

/* Test version overflow */
static void
test_version_overflow(void)
{
	printf("\nTest 4: Version overflow\n");

	void *ptr = (void *)0x12345678;
	uint16_t ver = 0xFFFF;

	umem_tagged_ptr_t tp = umem_tagged_ptr_make(ptr, ver);
	uint16_t decoded_ver = umem_tagged_ver_get(tp);

	printf("  Version 0xFFFF: raw=0x%016lx\n", tp.raw);
	assert(decoded_ver == 0xFFFF);

	/* Test wraparound */
	uint16_t next_ver = decoded_ver + 1;  /* Should wrap to 0 */
	umem_tagged_ptr_t tp2 = umem_tagged_ptr_make(ptr, next_ver);
	uint16_t decoded_ver2 = umem_tagged_ver_get(tp2);

	printf("  Version after wraparound: 0x%04x\n", decoded_ver2);
	assert(decoded_ver2 == 0);
	printf("  Version overflow correct ✓\n");
}

/* Test atomic load */
static void
test_atomic_load(void)
{
	printf("\nTest 5: Atomic load\n");

	void *ptr = (void *)0x00007fff12345678;
	umem_tagged_ptr_t tp = umem_tagged_ptr_make(ptr, 100);

	volatile umem_tagged_ptr_t shared = tp;
	umem_tagged_ptr_t loaded = atomic_load_tagged_ptr(&shared);

	printf("  Original: raw=0x%016lx\n", tp.raw);
	printf("  Loaded:   raw=0x%016lx\n", loaded.raw);

	assert(loaded.raw == tp.raw);
	assert(umem_tagged_ptr_get(loaded) == ptr);
	assert(umem_tagged_ver_get(loaded) == 100);
	printf("  Atomic load correct ✓\n");
}

/* Test atomic CAS success */
static void
test_atomic_cas_success(void)
{
	printf("\nTest 6: Atomic CAS (success)\n");

	void *old_ptr = (void *)0x1000;
	void *new_ptr = (void *)0x2000;

	volatile umem_tagged_ptr_t shared = umem_tagged_ptr_make(old_ptr, 5);
	umem_tagged_ptr_t expected = umem_tagged_ptr_make(old_ptr, 5);
	umem_tagged_ptr_t desired = umem_tagged_ptr_make(new_ptr, 6);

	int success = atomic_cas_tagged_ptr(&shared, &expected, desired);

	printf("  CAS returned: %d (expected 1)\n", success);
	printf("  After CAS: raw=0x%016lx\n", shared.raw);

	assert(success == 1);
	assert(shared.raw == desired.raw);
	assert(umem_tagged_ptr_get(shared) == new_ptr);
	assert(umem_tagged_ver_get(shared) == 6);
	printf("  CAS success correct ✓\n");
}

/* Test atomic CAS failure */
static void
test_atomic_cas_failure(void)
{
	printf("\nTest 7: Atomic CAS (failure)\n");

	void *current_ptr = (void *)0x1000;
	void *wrong_ptr = (void *)0x9999;
	void *new_ptr = (void *)0x2000;

	volatile umem_tagged_ptr_t shared = umem_tagged_ptr_make(current_ptr, 5);
	umem_tagged_ptr_t expected = umem_tagged_ptr_make(wrong_ptr, 5);
	umem_tagged_ptr_t desired = umem_tagged_ptr_make(new_ptr, 6);

	int success = atomic_cas_tagged_ptr(&shared, &expected, desired);

	printf("  CAS returned: %d (expected 0)\n", success);
	printf("  Shared unchanged: raw=0x%016lx\n", shared.raw);
	printf("  Expected updated: raw=0x%016lx\n", expected.raw);

	assert(success == 0);
	assert(shared.raw != desired.raw);
	assert(expected.raw == shared.raw);  /* Expected updated with current */
	printf("  CAS failure correct ✓\n");
}

/* Test ABA detection via version */
static void
test_aba_detection(void)
{
	printf("\nTest 8: ABA detection\n");

	void *ptr_a = (void *)0xAAAA;

	/* Initial state: pointer A, version 1 */
	volatile umem_tagged_ptr_t shared = umem_tagged_ptr_make(ptr_a, 1);
	umem_tagged_ptr_t snapshot = atomic_load_tagged_ptr(&shared);

	printf("  Initial: ptr=%p ver=%u\n",
	    umem_tagged_ptr_get(snapshot), umem_tagged_ver_get(snapshot));

	/* Simulate ABA: another thread pops A, pops B, pushes A again
	 * but with incremented version */
	umem_tagged_ptr_t temp = umem_tagged_ptr_make(ptr_a, 5);
	shared = temp;  /* Direct assignment to simulate other thread */

	printf("  After ABA: ptr=%p ver=%u (same pointer, different version)\n",
	    umem_tagged_ptr_get(shared), umem_tagged_ver_get(shared));

	/* Now try CAS with old snapshot - should fail due to version mismatch */
	umem_tagged_ptr_t expected = snapshot;
	umem_tagged_ptr_t desired = umem_tagged_ptr_make((void *)0xBBBB, 2);
	int success = atomic_cas_tagged_ptr(&shared, &expected, desired);

	printf("  CAS with old version: %d (expected 0)\n", success);
	assert(success == 0);  /* Must fail - version changed */
	printf("  ABA prevention working ✓\n");
}

/* Multithreaded contention test */
#define NUM_THREADS 8
#define OPS_PER_THREAD 10000

static volatile umem_tagged_ptr_t shared_stack;
static volatile int success_count = 0;

static void *
contention_thread(void *arg)
{
	(void)arg;
	int local_success = 0;

	for (int i = 0; i < OPS_PER_THREAD; i++) {
		/* Try to increment the "pointer" value atomically */
		umem_tagged_ptr_t expected, desired;
		do {
			expected = atomic_load_tagged_ptr(&shared_stack);
			void *old_val = umem_tagged_ptr_get(expected);
			uint16_t old_ver = umem_tagged_ver_get(expected);

			void *new_val = (void *)((uintptr_t)old_val + 1);
			uint16_t new_ver = old_ver + 1;

			desired = umem_tagged_ptr_make(new_val, new_ver);
		} while (!atomic_cas_tagged_ptr(&shared_stack, &expected, desired));

		local_success++;
	}

	__sync_fetch_and_add(&success_count, local_success);
	return NULL;
}

static void
test_concurrent_operations(void)
{
	printf("\nTest 9: Concurrent operations\n");

	shared_stack = umem_tagged_ptr_make(NULL, 0);
	success_count = 0;

	pthread_t threads[NUM_THREADS];

	printf("  Spawning %d threads, %d ops each...\n",
	    NUM_THREADS, OPS_PER_THREAD);

	for (int i = 0; i < NUM_THREADS; i++) {
		pthread_create(&threads[i], NULL, contention_thread, NULL);
	}

	for (int i = 0; i < NUM_THREADS; i++) {
		pthread_join(threads[i], NULL);
	}

	umem_tagged_ptr_t final = atomic_load_tagged_ptr(&shared_stack);
	uintptr_t final_val = (uintptr_t)umem_tagged_ptr_get(final);
	uint16_t final_ver = umem_tagged_ver_get(final);

	printf("  Expected value: %d\n", NUM_THREADS * OPS_PER_THREAD);
	printf("  Final value:    %lu\n", (unsigned long)final_val);
	printf("  Final version:  %u\n", final_ver);
	printf("  Success count:  %d\n", success_count);

	assert(final_val == (uintptr_t)(NUM_THREADS * OPS_PER_THREAD));
	assert(success_count == NUM_THREADS * OPS_PER_THREAD);
	printf("  Concurrent operations correct ✓\n");
}

int
main(void)
{
	printf("=== Tagged Pointer Atomicity Test Suite ===\n\n");

	test_structure_properties();
	test_encoding_decoding();
	test_null_pointer();
	test_version_overflow();
	test_atomic_load();
	test_atomic_cas_success();
	test_atomic_cas_failure();
	test_aba_detection();
	test_concurrent_operations();

	printf("\n=== All Tests Passed ===\n");
	return 0;
}
