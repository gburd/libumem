AC_INIT([umem], [2.4.0], [], [umem])
AM_INIT_AUTOMAKE([dist-bzip2])
AC_CONFIG_MACRO_DIRS([m4])

AC_PROG_CC

# Require C17 standard (ISO/IEC 9899:2018)
# Matches PostgreSQL v16+ requirements
# Try -std=c17 first, fall back to -std=c11
AC_MSG_CHECKING([for C17 support])
OLD_CFLAGS="$CFLAGS"
# Add feature test macros for POSIX and GNU/BSD extensions.
# On BSDs, _POSIX_C_SOURCE hides BSD APIs (gettimeofday, sbrk, etc.)
# so we skip it there; BSD headers provide POSIX APIs by default.
UNAME_S=$(uname -s 2>/dev/null || echo unknown)
case "$UNAME_S" in
  FreeBSD*|DragonFly*|NetBSD*|OpenBSD*)
    PLATFORM_FEATURE_FLAGS=""
    ;;
  SunOS*)
    PLATFORM_FEATURE_FLAGS="-D_POSIX_C_SOURCE=200809L -D__EXTENSIONS__"
    ;;
  *)
    PLATFORM_FEATURE_FLAGS="-D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE"
    ;;
esac
CFLAGS="$CFLAGS -std=c17 $PLATFORM_FEATURE_FLAGS"
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
    #if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 201710L
    #error C17 not supported
    #endif
]], [[]])],
    [AC_MSG_RESULT([yes])],
    [AC_MSG_RESULT([no, trying C11])
     CFLAGS="$OLD_CFLAGS -std=c11 $PLATFORM_FEATURE_FLAGS"
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

# Check for Python (for generating compile_commands.json)
AM_PATH_PYTHON([3.6], [], [AC_MSG_WARN([Python >= 3.6 not found; compile_commands.json generation will not work])])

AC_C_INLINE

# Detect OS and set conditionals
AC_MSG_CHECKING([for Solaris/Illumos build])
case "$host_os" in
  solaris*|*illumos*)
    AC_MSG_RESULT(yes)
    AM_CONDITIONAL(SOLARIS_BUILD, true)
    AC_DEFINE(SOLARIS_BUILD, [1], [Building on Solaris/Illumos])
    # illumos/Solaris split the sockets + name-service APIs out of libc into
    # libsocket / libnsl; components using socket()/connect() (the umemctl
    # introspection control channel, both server and client) must link them.
    # AC_SEARCH_LIBS is a no-op where the symbol is already in libc (Linux).
    saved_LIBS=$LIBS
    LIBS=
    AC_SEARCH_LIBS([socket], [socket], [], [], [])
    AC_SEARCH_LIBS([gethostbyname], [nsl], [], [], [])
    SOLARIS_SOCKET_LIBS=$LIBS
    LIBS=$saved_LIBS
    AC_SUBST(SOLARIS_SOCKET_LIBS)
    ;;
  *)
    AC_MSG_RESULT(no)
    AM_CONDITIONAL(SOLARIS_BUILD, false)
    SOLARIS_SOCKET_LIBS=
    AC_SUBST(SOLARIS_SOCKET_LIBS)
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
    arch_sparc=yes
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
AM_CONDITIONAL(ARCH_SPARC, [test "x$arch_sparc" = "xyes"])

# Detect hardware cache line size for false-sharing avoidance.
# Auto-detection tries getconf, sysfs, then falls back to architecture defaults.
# Override with --with-cache-line-size=N if auto-detection is wrong.
AC_ARG_WITH([cache-line-size],
    [AS_HELP_STRING([--with-cache-line-size=N],
        [Override hardware cache line size in bytes (default: auto-detect)])],
    [cache_line_size=$withval],
    [cache_line_size=auto])

if test "x$cache_line_size" = "xauto"; then
    AC_MSG_CHECKING([hardware cache line size])
    cache_line_size=
    # Try getconf (POSIX, works on Linux/FreeBSD/Illumos)
    if test -z "$cache_line_size"; then
        cache_line_size=`getconf LEVEL1_DCACHE_LINESIZE 2>/dev/null`
        if test "x$cache_line_size" = "x0" || test "x$cache_line_size" = "x"; then
            cache_line_size=
        fi
    fi
    # Try sysfs (Linux)
    if test -z "$cache_line_size" && test -f /sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size; then
        cache_line_size=`cat /sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size 2>/dev/null`
    fi
    # Try sysctl (FreeBSD/macOS)
    if test -z "$cache_line_size"; then
        cache_line_size=`sysctl -n hw.cachelinesize 2>/dev/null`
    fi
    # Architecture-based defaults if detection failed
    if test -z "$cache_line_size" || test "$cache_line_size" -lt 16 2>/dev/null; then
        case "$host_cpu" in
            aarch64*|arm64*)
                # Apple M-series uses 128B, most ARM64 uses 64B
                cache_line_size=64 ;;
            sparc*|sparcv9)
                # UltraSPARC-III/IV: 64B, T-series: 128B coherence
                cache_line_size=64 ;;
            *)
                cache_line_size=64 ;;
        esac
    fi
    AC_MSG_RESULT([$cache_line_size bytes])
fi
AC_DEFINE_UNQUOTED(UMEM_CACHE_LINE_SIZE_CONFIG, [$cache_line_size],
    [Hardware cache line size for false-sharing avoidance])

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
AC_COMPILE_IFELSE(
  [AC_LANG_PROGRAM([[
#include <pthread.h>
  ]], [[
    _Static_assert(sizeof(pthread_mutex_t) > 24,
        "pthread_mutex_t is not larger than 24 bytes");
  ]])],
  [
    AC_MSG_RESULT(yes)
    AC_DEFINE(UMEM_PTHREAD_MUTEX_TOO_BIG, [1], [need bigger cache])
    AC_MSG_WARN([*** increasing umem cpu cache size to compensate.])
  ],
  [AC_MSG_RESULT(no)]
)

AC_CHECK_LIB(dl,dlopen)

# Check for elfutils libdw (DWARF debug info for stack traces)
AC_CHECK_LIB(dw, dwfl_begin, [HAVE_LIBDW=yes], [HAVE_LIBDW=no])
if test "x$HAVE_LIBDW" = "xyes"; then
    AC_DEFINE(HAVE_LIBDW, 1, [Have elfutils libdw for DWARF stack traces])
    LIBS="$LIBS -ldw"
fi

AC_CHECK_HEADERS([atomic.h sys/mman.h sys/sysmacros.h sys/time.h malloc.h thread.h sys/frame.h sys/stack.h alloca.h])
AC_CHECK_FUNCS([issetugid mallinfo mallinfo2 malloc_stats malloc_usable_size])

# Check for FreeBSD pthread hooks
AC_MSG_CHECKING([for FreeBSD pthread hooks])
AC_LINK_IFELSE([
    AC_LANG_PROGRAM([[
        extern void _malloc_thread_cleanup(void);
    ]], [[
        _malloc_thread_cleanup();
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

# In-process introspection channel (umemctl server side).
# Opt-in at both compile time (this flag) and runtime (UMEM_OPTIONS=introspect=1).
# The break-before-return hook only exists in _umem_alloc when this is enabled,
# so the default production hot path is provably identical to a build without it.
AC_ARG_ENABLE([introspect],
    [AS_HELP_STRING([--enable-introspect],
        [Enable the umemctl live-process introspection channel (opt-in, off by default; zero hot-path cost unless a break predicate is armed at runtime)])],
    [introspect=$enableval], [introspect=no])

AM_CONDITIONAL([ENABLE_INTROSPECT], [test "x$introspect" = "xyes"])

if test "x$introspect" = "xyes"; then
    AC_MSG_NOTICE([Introspection channel enabled (umemctl)])
    AC_DEFINE([UMEM_INTROSPECT], [1], [Enable umemctl introspection channel])
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
    [AS_HELP_STRING([--enable-rseq@<:@=auto@:>@],
        [Enable restartable sequences for true per-CPU caching (Linux 4.18+, 50-200% improvement) @<:@default=auto@:>@])],
    [rseq=$enableval], [rseq=auto])

# Auto-detect rseq support if not explicitly disabled
if test "x$rseq" = "xauto"; then
    AC_CHECK_HEADERS([linux/rseq.h], [rseq=yes], [rseq=no])
fi

AM_CONDITIONAL([ENABLE_RSEQ], [test "x$rseq" = "xyes"])

if test "x$rseq" = "xyes"; then
    AC_MSG_NOTICE([RSEQ support enabled (auto-detected)])
    AC_DEFINE([HAVE_LINUX_RSEQ_H], [1], [Define if linux/rseq.h is available])
else
    AC_MSG_NOTICE([RSEQ support disabled (not available)])
fi

# NUMA-aware allocation
AC_ARG_ENABLE([numa],
    [AS_HELP_STRING([--enable-numa@<:@=auto@:>@],
        [Enable NUMA-aware memory allocation (10-30% improvement on multi-socket systems) @<:@default=auto@:>@])],
    [numa=$enableval], [numa=auto])

# Auto-detect NUMA support if not explicitly disabled
if test "x$numa" = "xauto"; then
    AC_CHECK_LIB([numa], [numa_available], [numa=yes], [numa=no])
fi

AM_CONDITIONAL([ENABLE_NUMA], [test "x$numa" = "xyes"])

if test "x$numa" = "xyes"; then
    AC_MSG_NOTICE([NUMA-aware allocation enabled (auto-detected)])
    AC_CHECK_LIB([numa], [numa_available], [
        AC_DEFINE([HAVE_LIBNUMA], [1], [Define if libnuma is available])
        NUMA_LIBS="-lnuma"
        AC_SUBST(NUMA_LIBS)
    ], [
        AC_MSG_ERROR([libnuma required for NUMA support but not found])
    ])
else
    AC_MSG_NOTICE([NUMA-aware allocation disabled (not available)])
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
  libdw (DWARF):    $HAVE_LIBDW
"
