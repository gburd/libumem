/*
 * Copyright (c) 2006-2008 Message Systems, Inc. All rights reserved
 * This header file distributed under the terms of the CDDL.
 * Portions Copyright 2004 Sun Microsystems, Inc. All Rights reserved.
 */
#ifndef _EC_UMEM_SOL_COMPAT_H_
#define _EC_UMEM_SOL_COMPAT_H_

#include "config.h"

#include <stdint.h>
#include <time.h>
#include <pthread.h>

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#ifdef _WIN32
typedef char *caddr_t;
#endif

/*
 * Thread return type and calling convention.
 * MinGW provides pthreads, so we use the same path as Linux/macOS.
 * Only native MSVC Win32 builds need DWORD/WINAPI.
 */
#if defined(_WIN32) && !defined(__MINGW32__)
# define THR_RETURN DWORD
# define THR_API WINAPI
# define INLINE __inline
#else
# define THR_RETURN void *
# define THR_API
# define INLINE inline
#endif

#if defined(__MACH__) || defined(_WIN32)
#define NO_WEAK_SYMBOLS
#define _umem_cache_alloc(a,b) umem_cache_alloc(a,b)
#define _umem_cache_free(a,b) umem_cache_free(a,b)
#define _umem_zalloc(a,b) umem_zalloc(a,b)
#define _umem_alloc(a,b) umem_alloc(a,b)
#define _umem_alloc_align(a,b,c) umem_alloc_align(a,b,c)
#define _umem_free(a,b) umem_free(a,b)
#define _umem_free_align(a,b) umem_free_align(a,b)
#endif

#ifdef _WIN32
#define bcopy(s, d, n)  	memcpy(d, s, n)
#define bzero(m, s)			memset(m, 0, s)
/*
 * MinGW may not declare gettimeofday even with sys/time.h.
 * Provide a compat wrapper using clock_gettime(CLOCK_REALTIME).
 */
static INLINE int umem_gettimeofday(struct timeval *tv, void *tz)
{
  struct timespec ts;
  (void)tz;
  clock_gettime(CLOCK_REALTIME, &ts);
  tv->tv_sec = ts.tv_sec;
  tv->tv_usec = (long)(ts.tv_nsec / 1000);
  return 0;
}
#define gettimeofday(tv, tz) umem_gettimeofday(tv, tz)
#endif

/*
 * On Solaris/Illumos, <thread.h> provides these types and functions.
 * _EC_UMEM_SOL_COMPAT_TYPES_DEFINED is set by umem_impl.h when
 * HAVE_THREAD_H is available, to skip the redefinitions.
 */
#ifndef _EC_UMEM_SOL_COMPAT_TYPES_DEFINED
typedef pthread_t thread_t;
typedef pthread_mutex_t mutex_t;
typedef pthread_cond_t cond_t;
typedef uint64_t hrtime_t;
typedef uint32_t uint_t;
typedef unsigned long ulong_t;
typedef struct timespec timestruc_t;
typedef long long longlong_t;
typedef struct timespec timespec_t;
static INLINE hrtime_t gethrtime(void) {
#if defined(_WIN32) && !defined(__MINGW32__)
  LARGE_INTEGER freq, count;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&count);
  return (hrtime_t)((count.QuadPart * 1000000000LL) / freq.QuadPart);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (((uint64_t)ts.tv_sec) << 32) | (uint64_t)ts.tv_nsec;
#endif
}
# define thr_self()                pthread_self()
static INLINE thread_t _thr_self(void) {
  return thr_self();
}
#if defined(__MACH__)
#define CPUHINT() (pthread_mach_thread_np(pthread_self()))
#endif
# define thr_sigsetmask            pthread_sigmask

#define THR_BOUND     1
#define THR_DETACHED  2
#define THR_DAEMON    4

static INLINE int thr_create(void *stack_base __attribute__((unused)),
  size_t stack_size __attribute__((unused)),
  THR_RETURN (THR_API *start_func)(void*),
  void *arg, long flags, thread_t *new_thread_ID)
{
  int ret;
  pthread_attr_t attr;

  pthread_attr_init(&attr);

  if (flags & THR_DETACHED) {
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  }
  ret = pthread_create(new_thread_ID, &attr, start_func, arg);
  pthread_attr_destroy(&attr);
  return ret;
}
#endif /* _EC_UMEM_SOL_COMPAT_TYPES_DEFINED */

/*
 * On Solaris/Illumos, <synch.h> (via <thread.h>) provides mutex_init,
 * cond_wait, etc. natively.  Only define pthread-based shims on other
 * platforms.
 */
#ifndef _EC_UMEM_SOL_COMPAT_TYPES_DEFINED
# define mutex_init(mp, type, arg) pthread_mutex_init(mp, NULL)
# define mutex_lock(mp)            pthread_mutex_lock(mp)
# define mutex_unlock(mp)          pthread_mutex_unlock(mp)
# define mutex_destroy(mp)         pthread_mutex_destroy(mp)
# define mutex_trylock(mp)         pthread_mutex_trylock(mp)
# define DEFAULTMUTEX              PTHREAD_MUTEX_INITIALIZER
# define DEFAULTCV                 PTHREAD_COND_INITIALIZER
# define MUTEX_HELD(mp)            1 /* not really, but only used in an assert */

# define cond_init(c, type, arg)   pthread_cond_init(c, NULL)
# define cond_wait(c, m)           pthread_cond_wait(c, m)
# define _cond_wait(c, m)          pthread_cond_wait(c, m)
# define cond_signal(c)            pthread_cond_signal(c)
# define cond_broadcast(c)         pthread_cond_broadcast(c)
# define cond_destroy(c)           pthread_cond_destroy(c)
# define cond_timedwait            pthread_cond_timedwait
# define _cond_timedwait           pthread_cond_timedwait
#endif /* _EC_UMEM_SOL_COMPAT_TYPES_DEFINED */

#ifndef RTLD_FIRST
# define RTLD_FIRST 0
#endif

/*
 * Atomic increment operations.
 * Uses C11 atomics (stdatomic.h) on all platforms.
 * The inline asm CAS and PPC mutex fallback have been removed.
 */
#ifdef ECELERITY
# include "umem_atomic.h"
#else
# include <stdatomic.h>

static INLINE uint_t umem_atomic_inc(volatile uint_t *mem)
{
  return atomic_fetch_add_explicit((_Atomic(uint_t) *)mem, 1,
      memory_order_relaxed) + 1;
}

static INLINE uint64_t umem_atomic_inc64(volatile uint64_t *mem)
{
  return atomic_fetch_add_explicit((_Atomic(uint64_t) *)mem, 1,
      memory_order_relaxed) + 1;
}

#endif

#define P2PHASE(x, align)    ((x) & ((align) - 1))
#define P2ALIGN(x, align)    ((x) & -(align))
#define P2NPHASE(x, align)    (-(x) & ((align) - 1))
#define P2ROUNDUP(x, align)   (-(-(x) & -(align)))
#define P2END(x, align)     (-(~(x) & -(align)))
#define P2PHASEUP(x, align, phase)  ((phase) - (((phase) - (x)) & -(align)))
#define P2CROSS(x, y, align)    (((x) ^ (y)) > (align) - 1)
#define P2SAMEHIGHBIT(x, y)    (((x) ^ (y)) < ((x) & (y)))
#define IS_P2ALIGNED(v, a) ((((uintptr_t)(v)) & ((uintptr_t)(a) - 1)) == 0)
#define ISP2(x)    (((x) & ((x) - 1)) == 0)
/*
 * return TRUE if adding len to off would cause it to cross an align
 * boundary.
 * eg, P2BOUNDARY(0x1234, 0xe0, 0x100) == TRUE (0x1234 + 0xe0 == 0x1314)
 * eg, P2BOUNDARY(0x1234, 0x50, 0x100) == FALSE (0x1234 + 0x50 == 0x1284)
 */
#define P2BOUNDARY(off, len, align) \
    (((off) ^ ((off) + (len) - 1)) > (align) - 1)

#ifndef atomic_add_64
#define atomic_add_64(lvalptr, delta) \
	__sync_fetch_and_add((volatile uint64_t *)(lvalptr), (uint64_t)(delta))
#endif
#define atomic_add_32_nv(a, b)  	  umem_atomic_inc(a)

#ifndef NANOSEC
#define NANOSEC 1000000000
#endif

#ifdef _WIN32
#define issetugid()		  0
#elif !HAVE_ISSETUGID
#define issetugid()       (geteuid() == 0)
#endif

#define _sysconf(a) sysconf(a)
#ifndef __NORETURN
#define __NORETURN  __attribute__ ((noreturn))
#endif

/*
 * On Solaris/Illumos, getpcstack.c uses real stack walking via
 * getfp()/flush_windows() from assembly and stack_getbounds()/
 * thr_stksegment() from the system.  On other platforms, use
 * a dummy implementation since those APIs are not available.
 * ARM64 has its own implementation in getpcstack.c.
 */
#if !defined(HAVE_THREAD_H) && \
    !defined(__aarch64__) && !defined(__arm64__)
#define EC_UMEM_DUMMY_PCSTACK 1
#endif
static INLINE int __nthreads(void)
{
  /* or more; just to force multi-threaded mode */
  return 2;
}

#if (SIZEOF_VOID_P == 8)
# define _LP64 1
#endif

#ifndef MIN
# define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
# define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif

/*
 * Branch prediction hints for optimization
 */
#ifndef likely
# define likely(x)      __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
# define unlikely(x)    __builtin_expect(!!(x), 0)
#endif


#endif
