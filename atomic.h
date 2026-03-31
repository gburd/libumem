#ifndef _UMEM_ATOMIC_H
#define _UMEM_ATOMIC_H

#ifdef __sun
/* Solaris/Illumos - use native atomic.h */
#include <atomic.h>
#else
/* Linux - use GCC/Clang builtins */
#include <stddef.h>
#include <stdint.h>
#include <limits.h>

/* Define uint_t to match sol_compat.h */
#ifndef uint_t
typedef uint32_t uint_t;
#endif

typedef unsigned char uchar_t;

/* Atomic compare-and-swap */
#define atomic_cas_ptr(target, cmp, new) \
	__sync_val_compare_and_swap((target), (cmp), (new))

#define atomic_cas_uint(target, cmp, new) \
	__sync_val_compare_and_swap((target), (cmp), (new))

#define atomic_cas_ulong(target, cmp, new) \
	__sync_val_compare_and_swap((target), (cmp), (new))

/* Atomic increment/decrement */
#define atomic_inc_32(p) __sync_add_and_fetch((p), 1)
#define atomic_dec_32(p) __sync_sub_and_fetch((p), 1)
#define atomic_inc_64(p) __sync_add_and_fetch((p), 1)
#define atomic_dec_64(p) __sync_sub_and_fetch((p), 1)

/* Atomic add - undefine sol_compat.h version and override on Linux */
#undef atomic_add_64
#define atomic_add_64(p, v) __sync_add_and_fetch((p), (v))

/* Atomic swap */
#define atomic_swap_uint(target, new) \
	__sync_lock_test_and_set((target), (new))

#define atomic_swap_64(target, new) \
	__sync_lock_test_and_set((target), (new))

#define atomic_swap_ptr(target, new) \
	__sync_lock_test_and_set((target), (new))

/* Memory barrier */
#define membar_producer() __sync_synchronize()
#define membar_consumer() __sync_synchronize()
#define membar_enter() __sync_synchronize()

#endif /* __sun */

#endif /* _UMEM_ATOMIC_H */
