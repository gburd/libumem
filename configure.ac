AC_INIT([umem], [1.0.2], [], [umem])
AM_INIT_AUTOMAKE([dist-bzip2])
AC_CONFIG_MACRO_DIRS([m4])

AC_PROG_CC

# Require C17 standard (ISO/IEC 9899:2018)
# Matches PostgreSQL v16+ requirements
# Try -std=c17 first, fall back to -std=c11
AC_MSG_CHECKING([for C17 support])
OLD_CFLAGS="$CFLAGS"
# Add feature test macros for POSIX and GNU extensions
CFLAGS="$CFLAGS -std=c17 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE"
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
    #if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 201710L
    #error C17 not supported
    #endif
]], [[]])],
    [AC_MSG_RESULT([yes])],
    [AC_MSG_RESULT([no, trying C11])
     CFLAGS="$OLD_CFLAGS -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE"
     AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
         #if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 201112L
         #error C11 not supported
         #endif
     ]], [[]])],
         [AC_MSG_WARN([Using C11 instead of C17])],
         [AC_MSG_ERROR([C11 or C17 compiler required (GCC 9.0+, Clang 10.0+)])])])

# Verify C11/C17 atomics support
AC_CHECK_HEADER([stdatomic.h], [],
    [AC_MSG_ERROR([C11/C17 atomics required (<stdatomic.h>)])])

AM_PROG_AS
LT_INIT

AC_C_INLINE

# Detect OS and set conditionals
AC_MSG_CHECKING([for Solaris/Illumos build])
case "$host_os" in
  solaris*|*illumos*)
    AC_MSG_RESULT(yes)
    AM_CONDITIONAL(SOLARIS_BUILD, true)
    AC_DEFINE(SOLARIS_BUILD, [1], [Building on Solaris/Illumos])
    ;;
  *)
    AC_MSG_RESULT(no)
    AM_CONDITIONAL(SOLARIS_BUILD, false)
    ;;
esac

AC_MSG_CHECKING([for FreeBSD build])
case "$host_os" in
  freebsd*)
    AC_MSG_RESULT(yes)
    AM_CONDITIONAL(FREEBSD_BUILD, true)
    AC_DEFINE(FREEBSD_BUILD, [1], [Building on FreeBSD])
    ;;
  *)
    AC_MSG_RESULT(no)
    AM_CONDITIONAL(FREEBSD_BUILD, false)
    ;;
esac

AC_MSG_CHECKING([system architecture])
# Note: Cross-compilation support can be enhanced in future by:
#   - Checking for cross-compilation tools (AS_IF([test "$cross_compiling" = yes]))
#   - Using host_cpu, build_cpu, and target_cpu appropriately
#   - Validating TLS model compatibility across architectures
case "$host_cpu" in
  x86_64|amd64)
    AC_MSG_RESULT(amd64)
    GENASM_DIR="amd64"
    AC_DEFINE(ARCH_AMD64, [1], [amd64 architecture])
    ;;
  i?86)
    AC_MSG_RESULT(i386)
    GENASM_DIR="i386"
    AC_DEFINE(ARCH_I386, [1], [i386 architecture])
    ;;
  sparc*)
    AC_MSG_RESULT(sparc)
    GENASM_DIR="sparc"
    AC_DEFINE(ARCH_SPARC, [1], [sparc architecture])
    ;;
  riscv64)
    AC_MSG_RESULT(riscv64)
    GENASM_DIR="riscv64"
    AC_DEFINE(ARCH_RISCV64, [1], [RISC-V 64-bit architecture])
    ;;
  aarch64|arm64)
    AC_MSG_RESULT(aarch64)
    GENASM_DIR="aarch64"
    AC_DEFINE(ARCH_AARCH64, [1], [ARM64 64-bit architecture])
    AC_MSG_NOTICE([Configuring for ARM64 (aarch64) architecture])
    ;;
  *)
    AC_MSG_RESULT($host_cpu)
    AC_MSG_WARN([Unsupported architecture: $host_cpu, defaulting to generic])
    GENASM_DIR="sparc"
    ;;
esac

AC_SUBST(GENASM_DIR)

# aarch64-specific checks
if test "$GENASM_DIR" = "aarch64"; then
    # Check for ARM-specific features
    AC_MSG_CHECKING([for ARM64 NEON support])
    AC_COMPILE_IFELSE(
        [AC_LANG_PROGRAM([[#include <arm_neon.h>]], [[]])],
        [AC_MSG_RESULT([yes])
         AC_DEFINE([HAVE_NEON], [1], [ARM64 NEON SIMD support available])],
        [AC_MSG_RESULT([no])]
    )

    # Check TLS support
    AC_MSG_CHECKING([for ARM64 TLS support])
    AC_COMPILE_IFELSE(
        [AC_LANG_PROGRAM([[
            __thread int test_var = 0;
            int test_tls(void) {
                unsigned long tp;
                __asm__ __volatile__("mrs %0, tpidr_el0" : "=r" (tp));
                return (int)tp;
            }
        ]], [[test_tls();]])],
        [AC_MSG_RESULT([yes])],
        [AC_MSG_RESULT([no])
         AC_MSG_ERROR([ARM64 TLS support required but not available])]
    )
fi

# SIMD vectorization support
AC_MSG_CHECKING([for x86_64 SSE2 support])
if test "$GENASM_DIR" = "amd64"; then
    AC_COMPILE_IFELSE(
        [AC_LANG_PROGRAM([[#include <emmintrin.h>]], [[__m128i v = _mm_setzero_si128();]])],
        [AC_MSG_RESULT([yes])
         AC_DEFINE([HAVE_SSE2], [1], [x86_64 SSE2 SIMD support available])],
        [AC_MSG_RESULT([no])]
    )

    AC_MSG_CHECKING([for x86_64 AVX2 support])
    OLD_CFLAGS_SIMD="$CFLAGS"
    CFLAGS="$CFLAGS -mavx2"
    AC_COMPILE_IFELSE(
        [AC_LANG_PROGRAM([[#include <immintrin.h>]], [[__m256i v = _mm256_setzero_si256();]])],
        [AC_MSG_RESULT([yes])
         AC_DEFINE([HAVE_AVX2], [1], [x86_64 AVX2 SIMD support available])
         SIMD_CFLAGS="-mavx2"],
        [AC_MSG_RESULT([no])
         SIMD_CFLAGS=""]
    )
    CFLAGS="$OLD_CFLAGS_SIMD"
    AC_SUBST(SIMD_CFLAGS)
else
    AC_MSG_RESULT([skipped (not x86_64)])
fi

AC_MSG_CHECKING([whether pthread_mutex_t is larger than 24 bytes])
AC_RUN_IFELSE(
  [AC_LANG_SOURCE([
#include <pthread.h>
int main(void){return (sizeof(pthread_mutex_t) > 24);}
  ])],
  [AC_MSG_RESULT(yes)],
  [
    AC_MSG_RESULT(no)
    AC_DEFINE(UMEM_PTHREAD_MUTEX_TOO_BIG, [1], [need bigger cache])
    AC_MSG_WARN([*** increasing umem cpu cache size to compensate.])
  ]
)

AC_CHECK_LIB(dl,dlopen)

AC_CHECK_HEADERS([atomic.h sys/mman.h sys/sysmacros.h sys/time.h malloc.h])
AC_CHECK_FUNCS([issetugid mallinfo mallinfo2 malloc_stats])

# Check for FreeBSD pthread hooks
AC_MSG_CHECKING([for FreeBSD pthread hooks])
AC_LINK_IFELSE([
    AC_LANG_PROGRAM([[
        extern void _pthread_mutex_init_calloc_cb(void *(*)(size_t));
        extern void _malloc_thread_cleanup(void);
    ]], [[
        _pthread_mutex_init_calloc_cb(0);
    ]])
], [
    AC_MSG_RESULT([yes])
    AC_DEFINE([HAVE_FREEBSD_PTHREAD_HOOKS], [1],
        [Define if FreeBSD pthread hooks are available])
], [
    AC_MSG_RESULT([no])
])

# Coverage support
AC_ARG_ENABLE([coverage],
    [AS_HELP_STRING([--enable-coverage], [Enable coverage reporting with gcov/lcov])],
    [coverage=$enableval], [coverage=no])

AM_CONDITIONAL([ENABLE_COVERAGE], [test "x$coverage" = "xyes"])

if test "x$coverage" = "xyes"; then
    AC_MSG_NOTICE([Coverage reporting enabled])
    COVERAGE_CFLAGS="--coverage -fprofile-arcs -ftest-coverage"
    COVERAGE_LDFLAGS="--coverage -lgcov"
    AC_SUBST(COVERAGE_CFLAGS)
    AC_SUBST(COVERAGE_LDFLAGS)
fi

# AddressSanitizer support
AC_ARG_ENABLE([asan],
    [AS_HELP_STRING([--enable-asan], [Enable AddressSanitizer for memory error detection])],
    [asan=$enableval], [asan=no])

AM_CONDITIONAL([ENABLE_ASAN], [test "x$asan" = "xyes"])

if test "x$asan" = "xyes"; then
    AC_MSG_NOTICE([AddressSanitizer enabled])
    ASAN_CFLAGS="-fsanitize=address -fno-omit-frame-pointer -g"
    ASAN_LDFLAGS="-fsanitize=address"
    AC_SUBST(ASAN_CFLAGS)
    AC_SUBST(ASAN_LDFLAGS)
fi

# UndefinedBehaviorSanitizer support
AC_ARG_ENABLE([ubsan],
    [AS_HELP_STRING([--enable-ubsan], [Enable UndefinedBehaviorSanitizer])],
    [ubsan=$enableval], [ubsan=no])

AM_CONDITIONAL([ENABLE_UBSAN], [test "x$ubsan" = "xyes"])

if test "x$ubsan" = "xyes"; then
    AC_MSG_NOTICE([UndefinedBehaviorSanitizer enabled])
    UBSAN_CFLAGS="-fsanitize=undefined -fno-omit-frame-pointer -g"
    UBSAN_LDFLAGS="-fsanitize=undefined"
    AC_SUBST(UBSAN_CFLAGS)
    AC_SUBST(UBSAN_LDFLAGS)
fi

# ThreadSanitizer support
AC_ARG_ENABLE([tsan],
    [AS_HELP_STRING([--enable-tsan], [Enable ThreadSanitizer for race detection])],
    [tsan=$enableval], [tsan=no])

AM_CONDITIONAL([ENABLE_TSAN], [test "x$tsan" = "xyes"])

if test "x$tsan" = "xyes"; then
    AC_MSG_NOTICE([ThreadSanitizer enabled])
    TSAN_CFLAGS="-fsanitize=thread -fno-omit-frame-pointer -g"
    TSAN_LDFLAGS="-fsanitize=thread"
    AC_SUBST(TSAN_CFLAGS)
    AC_SUBST(TSAN_LDFLAGS)
fi

# Recursion guard support
AC_ARG_ENABLE([recursion-guard],
    [AS_HELP_STRING([--enable-recursion-guard],
        [Enable malloc recursion guard (adds 30-40% overhead, needed only if pthread operations call malloc during thread creation)])],
    [recursion_guard=$enableval], [recursion_guard=yes])

AM_CONDITIONAL([ENABLE_RECURSION_GUARD], [test "x$recursion_guard" = "xyes"])

if test "x$recursion_guard" = "xyes"; then
    AC_MSG_NOTICE([Recursion guard enabled (default)])
    AC_DEFINE([UMEM_ENABLE_RECURSION_GUARD], [1], [Enable malloc recursion guard])
else
    AC_MSG_NOTICE([Recursion guard disabled for maximum performance])
fi

# Per-CPU caching support (experimental)
AC_ARG_ENABLE([percpu-caching],
    [AS_HELP_STRING([--enable-percpu-caching],
        [Enable experimental per-CPU magazine caching (provides 100-200% throughput improvement at high thread counts, disabled by default)])],
    [percpu_caching=$enableval], [percpu_caching=no])

AM_CONDITIONAL([ENABLE_PERCPU_CACHING], [test "x$percpu_caching" = "xyes"])

if test "x$percpu_caching" = "xyes"; then
    AC_MSG_NOTICE([Per-CPU caching enabled (experimental)])
    AC_DEFINE([UMEM_PER_CPU_CACHE], [1], [Enable per-CPU magazine caching])

    # Check for sched_getcpu on Linux
    AC_CHECK_FUNCS([sched_getcpu], [
        AC_DEFINE([HAVE_SCHED_GETCPU], [1], [Define if sched_getcpu is available])
    ])

    # Check for libnuma (optional NUMA support)
    AC_CHECK_LIB([numa], [numa_available], [
        AC_DEFINE([HAVE_LIBNUMA], [1], [Define if libnuma is available])
        NUMA_LIBS="-lnuma"
        AC_SUBST(NUMA_LIBS)
    ])
else
    AC_MSG_NOTICE([Per-CPU caching disabled (default)])
fi

# Experimental rseq support (Linux 4.18+)
AC_ARG_ENABLE([rseq],
    [AS_HELP_STRING([--enable-rseq],
        [Enable restartable sequences for true per-CPU caching (Linux 4.18+, 50-200% improvement)])],
    [rseq=$enableval], [rseq=no])

AM_CONDITIONAL([ENABLE_RSEQ], [test "x$rseq" = "xyes"])

if test "x$rseq" = "xyes"; then
    AC_MSG_NOTICE([RSEQ support enabled (experimental)])
    AC_CHECK_HEADERS([linux/rseq.h], [
        AC_DEFINE([HAVE_LINUX_RSEQ_H], [1], [Define if linux/rseq.h is available])
    ])
else
    AC_MSG_NOTICE([RSEQ support disabled (default)])
fi

# NUMA-aware allocation
AC_ARG_ENABLE([numa],
    [AS_HELP_STRING([--enable-numa],
        [Enable NUMA-aware memory allocation (10-30% improvement on multi-socket systems)])],
    [numa=$enableval], [numa=no])

AM_CONDITIONAL([ENABLE_NUMA], [test "x$numa" = "xyes"])

if test "x$numa" = "xyes"; then
    AC_MSG_NOTICE([NUMA-aware allocation enabled])
    AC_CHECK_LIB([numa], [numa_available], [
        AC_DEFINE([HAVE_LIBNUMA], [1], [Define if libnuma is available])
        NUMA_LIBS="-lnuma"
        AC_SUBST(NUMA_LIBS)
    ], [
        AC_MSG_ERROR([libnuma required for NUMA support but not found])
    ])
else
    AC_MSG_NOTICE([NUMA-aware allocation disabled (default)])
fi

# Hardware Transactional Memory (Intel TSX)
AC_ARG_ENABLE([htm],
    [AS_HELP_STRING([--enable-htm],
        [Enable Hardware Transactional Memory support (5-15% improvement, Intel Haswell+)])],
    [htm=$enableval], [htm=no])

AM_CONDITIONAL([ENABLE_HTM], [test "x$htm" = "xyes"])

if test "x$htm" = "xyes"; then
    AC_MSG_NOTICE([Hardware Transactional Memory enabled])
    AC_CHECK_HEADERS([immintrin.h], [
        AC_DEFINE([HAVE_IMMINTRIN_H], [1], [Define if immintrin.h is available])

        # Check for TSX support
        AC_MSG_CHECKING([for Intel TSX intrinsics])
        AC_COMPILE_IFELSE([
            AC_LANG_PROGRAM([[#include <immintrin.h>]],
                [[unsigned int status = _xbegin();
                  if (status == _XBEGIN_STARTED) _xend();]])
        ], [
            AC_MSG_RESULT([yes])
            AC_DEFINE([HAVE_TSX], [1], [Define if Intel TSX is available])
        ], [
            AC_MSG_RESULT([no])
            AC_MSG_WARN([immintrin.h found but TSX intrinsics not available])
        ])
    ], [
        AC_MSG_WARN([immintrin.h not found, HTM support limited])
    ])
else
    AC_MSG_NOTICE([Hardware Transactional Memory disabled (default)])
fi

# Profile-Guided Optimization support
AC_ARG_ENABLE([pgo],
    [AS_HELP_STRING([--enable-pgo=generate|use],
        [Enable Profile-Guided Optimization: generate creates instrumented build, use applies profile data])],
    [pgo=$enableval], [pgo=no])

AM_CONDITIONAL([ENABLE_PGO_GENERATE], [test "x$pgo" = "xgenerate"])
AM_CONDITIONAL([ENABLE_PGO_USE], [test "x$pgo" = "xuse"])

if test "x$pgo" = "xgenerate"; then
    AC_MSG_NOTICE([PGO: Generating profile data (instrumented build)])
    PGO_CFLAGS="-fprofile-generate"
    PGO_LDFLAGS="-fprofile-generate"
    AC_SUBST(PGO_CFLAGS)
    AC_SUBST(PGO_LDFLAGS)
elif test "x$pgo" = "xuse"; then
    AC_MSG_NOTICE([PGO: Using profile data (optimized build)])
    PGO_CFLAGS="-fprofile-use -fprofile-correction"
    PGO_LDFLAGS="-fprofile-use"
    AC_SUBST(PGO_CFLAGS)
    AC_SUBST(PGO_LDFLAGS)
fi

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile Doxyfile umem.spec])

AC_OUTPUT

# Summary
echo "
Configuration summary:
  Architecture:     $host_cpu ($GENASM_DIR)
  OS:               $host_os
  Recursion guard:  $recursion_guard
  Per-CPU caching:  $percpu_caching
  RSEQ support:     $rseq
  NUMA support:     $numa
  HTM support:      $htm
  PGO:              $pgo
  Coverage:         $coverage
  AddressSanitizer: $asan
  UBSanitizer:      $ubsan
  ThreadSanitizer:  $tsan
"
