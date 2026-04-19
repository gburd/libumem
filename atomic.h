#ifndef _UMEM_ATOMIC_H
#define _UMEM_ATOMIC_H

#ifdef __sun
/* Solaris/Illumos - use native atomic.h */
#include <atomic.h>
#else

/*
 * C11 atomic operations for libumem.
 *
 * All operations use explicit memory ordering:
 *   - Counters/statistics: memory_order_relaxed
 *   - Synchronization (CAS): memory_order_acq_rel / memory_order_acquire
 *   - Barriers: memory_order_release / memory_order_acquire
 */

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <limits.h>

/* Define uint_t to match sol_compat.h */
#ifndef uint_t
typedef uint32_t uint_t;
#endif

typedef unsigned char uchar_t;

/*
 * Atomic compare-and-swap.
 * Returns the OLD value. Caller compares with expected to detect success.
 * Uses acq_rel ordering: acquire on read, release on write.
 */
static inline void *
_umem_atomic_cas_ptr(void *volatile *target, void *cmp, void *newval)
{
	void *expected = cmp;
	atomic_compare_exchange_strong_explicit(
	    (_Atomic(void *) *)target, &expected, newval,
	    memory_order_acq_rel, memory_order_acquire);
	return expected;
}
#define atomic_cas_ptr(target, cmp, new) \
	_umem_atomic_cas_ptr((void *volatile *)(target), (void *)(cmp), \
	    (void *)(new))

static inline uint_t
_umem_atomic_cas_uint(volatile uint_t *target, uint_t cmp, uint_t newval)
{
	uint_t expected = cmp;
	atomic_compare_exchange_strong_explicit(
	    (_Atomic(uint_t) *)target, &expected, newval,
	    memory_order_acq_rel, memory_order_acquire);
	return expected;
}
#define atomic_cas_uint(target, cmp, new) \
	_umem_atomic_cas_uint((volatile uint_t *)(target), (cmp), (new))

#define atomic_cas_int(target, cmp, new) \
	((int)_umem_atomic_cas_uint( \
	    (volatile uint_t *)(target), (uint_t)(cmp), (uint_t)(new)))

static inline unsigned long
_umem_atomic_cas_ulong(volatile unsigned long *target, unsigned long cmp,
    unsigned long newval)
{
	unsigned long expected = cmp;
	atomic_compare_exchange_strong_explicit(
	    (_Atomic(unsigned long) *)target, &expected, newval,
	    memory_order_acq_rel, memory_order_acquire);
	return expected;
}
#define atomic_cas_ulong(target, cmp, new) \
	_umem_atomic_cas_ulong((volatile unsigned long *)(target), (cmp), (new))

/*
 * Atomic increment/decrement.
 * Used for counters — relaxed ordering is sufficient.
 */
#define atomic_inc_32(p) \
	atomic_fetch_add_explicit((_Atomic(uint32_t) *)(p), 1, \
	    memory_order_relaxed)
#define atomic_dec_32(p) \
	atomic_fetch_sub_explicit((_Atomic(uint32_t) *)(p), 1, \
	    memory_order_relaxed)
#define atomic_inc_64(p) \
	atomic_fetch_add_explicit((_Atomic(uint64_t) *)(p), 1, \
	    memory_order_relaxed)
#define atomic_dec_64(p) \
	atomic_fetch_sub_explicit((_Atomic(uint64_t) *)(p), 1, \
	    memory_order_relaxed)

/*
 * Atomic add — returns the NEW value (old + delta).
 * Used for counters (relaxed) and synchronization variables.
 */
#undef atomic_add_64
#define atomic_add_64(p, v) \
	(atomic_fetch_add_explicit((_Atomic(uint64_t) *)(p), \
	    (uint64_t)(v), memory_order_relaxed) + (uint64_t)(v))

/*
 * Atomic add with acquire-release ordering for synchronization variables.
 * Returns the NEW value (old + delta).
 */
#undef atomic_add_64_acq_rel
#define atomic_add_64_acq_rel(p, v) \
	(atomic_fetch_add_explicit((_Atomic(uint64_t) *)(p), \
	    (uint64_t)(v), memory_order_acq_rel) + (uint64_t)(v))

/* Atomic swap */
#define atomic_swap_uint(target, new) \
	atomic_exchange_explicit((_Atomic(uint_t) *)(target), (new), \
	    memory_order_acq_rel)

#define atomic_swap_64(target, new) \
	atomic_exchange_explicit((_Atomic(uint64_t) *)(target), (new), \
	    memory_order_acq_rel)

#define atomic_swap_ptr(target, new) \
	atomic_exchange_explicit((_Atomic(void *) *)(target), (void *)(new), \
	    memory_order_acq_rel)

/*
 * Memory barriers — use appropriate C11 fence ordering.
 */
#define membar_producer() atomic_thread_fence(memory_order_release)
#define membar_consumer() atomic_thread_fence(memory_order_acquire)
#define membar_enter()    atomic_thread_fence(memory_order_seq_cst)

/*
 * Spin hint for CAS retry loops.
 * Reduces power consumption and pipeline stalls during spinning.
 */
#if defined(__x86_64__) || defined(__i386__)
#define UMEM_SPIN_HINT() __asm__ volatile("pause")
#elif defined(__aarch64__)
#define UMEM_SPIN_HINT() __asm__ volatile("yield")
#elif defined(__riscv)
/* RISC-V: pause hint (encoded as fence instruction) */
#define UMEM_SPIN_HINT() __asm__ volatile(".insn i 0x0f, 0, x0, x0, 0x010")
#elif defined(__sparc)
/* SPARC: membar for spin-wait */
#define UMEM_SPIN_HINT() __asm__ volatile("rd %%ccr, %%g0" ::: "memory")
#else
#define UMEM_SPIN_HINT() ((void)0)
#endif

#endif /* __sun */

#endif /* _UMEM_ATOMIC_H */
