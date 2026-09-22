AUTOMAKE_OPTIONS = subdir-objects
ACLOCAL_AMFLAGS = -I m4

lib_LTLIBRARIES = libumem.la libumem_malloc.la
noinst_PROGRAMS = umem_test umem_test2 umem_test3 \
		  umem_ptc_test umem_ptc_fork_test umem_ptc_bench \
		  test/test_main test/test_debug \
		  test/test_inspect test/test_inspect_live \
		  test/unit/umem_env_helper \
		  test/unit/repro_cpu_node_oob \
		  test/unit/test_rseq_fastpath \
		  test/property/prop_ownership \
		  test/property/prop_profile \
		  test/property/prop_palloc \
		  test/bench/bench_contention \
		  test/bench/bench_affinity_vs_mutex \
		  test/integration/introspect_churn \
		  test/property/prop_alloc_free2 \
		  test/property/prop_cache \
		  test/property/prop_fragmentation \
		  test/bench/bench_main \
		  test/bench/test_bench_accounting \
		  test/bench/bench_depot_contention \
		  test/bench/bench_simd \
		  test/bench/bench_simd_threshold \
		  test/bench/bench_numa_hash \
		  test/bench/bench_profile_test \
		  test/integration/test_signals \
		  test/integration/test_oom \
		  test/integration/test_multithreaded \
		  test/integration/test_realloc_bootstrap \
		  test/integration/test_debug_features \
		  test/integration/test_threading_stress \
		  test/integration/test_fork_mt_load \
		  test/integration/test_update_thread_startup \
		  test/stress/stress_main \
		  test/stress/stress_concurrency_oracle \
		  test/stress/repro_calloc_interpose_race \
		  test/unit/test_overflow_contracts \
		  test/unit/test_aligned_contracts \
		  test/unit/test_hash_partition \
		  test/unit/test_lifecycle_churn \
		  test/unit/test_hook_contracts \
		  test/unit/test_inspect_contracts \
		  test/unit/repro_reclaim_destroy \
		  test/unit/repro_reclaim_reuse \
		  test/bench/bench_debug_overhead \
		  test/bench/bench_cache_rseq \
		  test/bench/probe_reclaim_rss \
		  tools/umem_profile_dump

libumem_la_LDFLAGS = -lpthread -lm $(NUMA_LIBS) $(SOLARIS_SOCKET_LIBS) -version-info 1:0:0

# Conditionally include stub_stand.c for Solaris/Illumos
if SOLARIS_BUILD
STUB_SOURCES = stub_stand.c
else
STUB_SOURCES =
endif

# Conditionally include FreeBSD pthread hooks
if FREEBSD_BUILD
FREEBSD_SOURCES = freebsd_pthread_hooks.c
else
FREEBSD_SOURCES =
endif

# Conditionally include rseq support
if ENABLE_RSEQ
RSEQ_SOURCES = umem_rseq.c umem_rseq.h umem_rseq_x86_64.S umem_rseq_aarch64.S
else
RSEQ_SOURCES =
endif

# Conditionally include NUMA topology queries (--enable-numa).  This is
# topology information only; it does not make the allocator NUMA-aware.
# See umem_numa.h for what this does and does not provide.
if ENABLE_NUMA
NUMA_SOURCES = umem_numa.c umem_numa.h umem_hash_partition.c umem_hash_partition.h
else
NUMA_SOURCES =
endif

# Per-CPU magazine caching (umem_percpu.[ch]) and HTM (umem_htm.[ch]) used
# to be wired in here behind --enable-percpu-caching / --enable-htm.  Both
# options were removed from configure.ac on 2026-09-21 and both sources are
# quarantined in attic/ (not built, not installed, not distributed).  The
# percpu flag turned a working build into a compile error; the HTM source
# list was unconditionally empty while the header advertised a speedup.
# See attic/README.md.

# Conditionally include SPARC assembly on Solaris
# The .S extension ensures the C preprocessor runs on the assembly
# (needed for #ifdef directives in the file).
if SOLARIS_BUILD
if ARCH_SPARC
SPARC_ASM_SOURCES = sparc_subr_sol.S
X86_ASM_SOURCES =
else
SPARC_ASM_SOURCES =
# x86 illumos/Solaris: provide getfp()/_breakpoint() via a portable C compat
# unit (header-independent; see x86_subr_compat.c).  Without these the
# LD_PRELOAD malloc interposer aborts on its first stack walk with an
# undefined 'getfp' symbol.
X86_ASM_SOURCES = x86_subr_compat.c
endif
else
SPARC_ASM_SOURCES =
X86_ASM_SOURCES =
endif

# Assembly files need minimal preprocessing flags - avoid C-specific
# flags like -std=c17 that confuse the assembler.
AM_CCASFLAGS =

libumem_la_SOURCES = init_lib.c \
			atomic.h \
			sm.c \
			sm.h \
			umem_agent_support.c \
			umem_fail.c \
			umem_stacktrace.c \
			umem_stacktrace.h \
			umem_fork.c \
			umem_update_thread.c \
			vmem_mmap.c \
			vmem_sbrk.c \
			envvar.c \
			getpcstack.c \
			misc.c \
			misc.h \
			vmem_base.c \
			vmem_base.h \
			vmem_stand.h \
			umem.c \
			umem.h \
			umem_base.h \
			umem_impl.h \
			umem_simd.h \
			sol_compat.h \
			vmem.c \
			sys/vmem.h \
			sys/vmem_impl_user.h \
			malloc.c \
			malloc_guard.c \
			malloc_guard.h \
			umem_audit.c \
			umem_inspect.c \
			umem_inspect.h \
			umem_introspect.c \
			umem_introspect.h \
			umem_hooks.c \
			umem_hooks.h \
			umem_ptc.c \
			umem_ptc.h \
			umem_profile.c \
			umem_profile.h \
			umem_arena.c \
			umem_arena.h \
			umem_own.c \
			umem_own.h \
			$(STUB_SOURCES) \
			$(FREEBSD_SOURCES) \
			$(RSEQ_SOURCES) \
			$(NUMA_SOURCES) \
			$(SPARC_ASM_SOURCES) \
			$(X86_ASM_SOURCES)

libumem_malloc_la_SOURCES = malloc_interpose.c malloc_guard.h
libumem_malloc_la_LDFLAGS = -lpthread -ldl -R$(libdir) -version-info 1:0:0
libumem_malloc_la_LIBADD = libumem.la

# Initialize flags unconditionally to avoid += errors
#
# -fno-builtin-calloc/-malloc/-realloc/-free: these files DEFINE the
# allocator entry points, so the compiler must not rewrite calls between
# them.  Specifically, GCC's strlen pass turns an adjacent
# "p = malloc(n); memset(p, 0, n);" into "p = calloc(n, 1);", which inside
# calloc() itself is unbounded self-recursion -- observed as a 100%-CPU spin
# with zero syscalls in malloc_interpose.c's calloc().  The source also
# defends against this structurally (see calloc_zero_fill there); this flag
# is the second line of defense, not the only one.
AM_CFLAGS = -fno-builtin-calloc -fno-builtin-malloc -fno-builtin-realloc \
	-fno-builtin-free $(SIMD_CFLAGS) -D_UMEM_INTERNAL
# Namespace the vendored sparsemap (sm.c/sm.h, upstream v5.4.0) so its public
# symbols become umem_sm_* and cannot collide with an application that also
# links its own copy of sparsemap.  Only sm.c/sm.h consult this macro.
AM_CPPFLAGS = -DSPARSEMAP_PREFIX=umem_
AM_LDFLAGS =

# Add coverage flags if enabled
if ENABLE_COVERAGE
AM_CFLAGS += $(COVERAGE_CFLAGS)
AM_LDFLAGS += $(COVERAGE_LDFLAGS)
endif

# Add sanitizer flags if enabled
if ENABLE_ASAN
AM_CFLAGS += $(ASAN_CFLAGS)
AM_LDFLAGS += $(ASAN_LDFLAGS)
endif

if ENABLE_UBSAN
AM_CFLAGS += $(UBSAN_CFLAGS)
AM_LDFLAGS += $(UBSAN_LDFLAGS)
endif

if ENABLE_TSAN
AM_CFLAGS += $(TSAN_CFLAGS)
AM_LDFLAGS += $(TSAN_LDFLAGS)
endif

# Add PGO flags if enabled
if ENABLE_PGO_GENERATE
AM_CFLAGS += $(PGO_CFLAGS)
AM_LDFLAGS += $(PGO_LDFLAGS)
endif

if ENABLE_PGO_USE
AM_CFLAGS += $(PGO_CFLAGS)
AM_LDFLAGS += $(PGO_LDFLAGS)
endif

umem_test_SOURCES = umem_test.c
umem_test_LDADD = libumem.la

umem_test2_SOURCES = umem_test2.c
umem_test2_LDADD = libumem.la libumem_malloc.la
umem_test2_LDFLAGS = -lpthread

umem_test3_SOURCES = umem_test3.c
umem_test3_LDADD = libumem.la libumem_malloc.la

umem_ptc_test_SOURCES = umem_ptc_test.c
umem_ptc_test_LDADD = libumem.la libumem_malloc.la
umem_ptc_test_LDFLAGS = -pthread

umem_ptc_fork_test_SOURCES = umem_ptc_fork_test.c
umem_ptc_fork_test_LDADD = libumem.la libumem_malloc.la
umem_ptc_fork_test_LDFLAGS = -pthread

umem_ptc_bench_SOURCES = umem_ptc_bench.c
umem_ptc_bench_LDADD = libumem.la
umem_ptc_bench_LDFLAGS = -pthread

# New test suite - unified runner with all unit tests
test_test_main_SOURCES = test/test_main.c test/munit.c \
			 test/unit/test_umem_alloc.c \
			 test/unit/test_umem_advanced.c \
			 test/unit/test_umem_cache.c \
			 test/unit/test_umem_align.c \
			 test/unit/test_umem_debug.c \
			 test/unit/test_vmem.c \
			 test/unit/test_vmem_sbrk.c \
			 test/unit/test_umem_hooks.c \
			 test/unit/test_error_paths.c \
			 test/unit/test_rare_flags.c \
			 test/unit/test_boundary_conditions.c \
			 test/unit/test_umem_audit.c \
			 test/unit/test_malloc.c \
			 test/unit/test_envvar.c \
			 test/unit/test_umem_fail.c \
			 test/unit/test_overflow_fixes.c \
			 test/unit/test_magazine_tune.c \
			 test/unit/test_cache_consistency.c \
			 test/unit/test_umem_stats.c \
			 test/unit/test_depot_consistency.c \
			 test/unit/test_sbo.c \
			 test/unit/test_arena.c \
			 test/unit/test_umem_own.c \
			 test/unit/test_coverage.c \
			 test/unit/test_reclaim.c \
			 test/unit/test_stacktrace.c \
			 test/unit/test_profile.c
test_test_main_LDADD = libumem.la
test_test_main_LDFLAGS = -pthread -lm
test_test_main_CPPFLAGS = -I$(srcdir)/test \
	-DUMEM_ENV_HELPER='"$(abs_builddir)/test/unit/.libs/umem_env_helper"' \
	-DUMEM_RECLAIM_DESTROY_HELPER='"$(abs_builddir)/test/unit/.libs/repro_reclaim_destroy"' \
	-DUMEM_RECLAIM_REUSE_HELPER='"$(abs_builddir)/test/unit/.libs/repro_reclaim_reuse"'

# P1.4 / P1.5 regressions: cache destroy with retained empty slabs, and
# reclaimed-slab reuse (hash+guards buftags, multi-page non-hash freelists,
# unlocked slab_state publication).  Exec helpers because the interesting
# cases need UMEM_DEBUG / UMEM_OPTIONS, which umem reads once at init.  They
# reach into allocator internals (slab_state, cache_slabsize), so they
# include umem_base.h and need -I$(srcdir) plus the sys/ headers.
test_unit_repro_reclaim_destroy_SOURCES = test/unit/repro_reclaim_destroy.c
test_unit_repro_reclaim_destroy_LDADD = libumem.la
test_unit_repro_reclaim_destroy_LDFLAGS = -pthread
test_unit_repro_reclaim_destroy_CPPFLAGS = -I$(srcdir)

test_unit_repro_reclaim_reuse_SOURCES = test/unit/repro_reclaim_reuse.c
test_unit_repro_reclaim_reuse_LDADD = libumem.la
test_unit_repro_reclaim_reuse_LDFLAGS = -pthread
test_unit_repro_reclaim_reuse_CPPFLAGS = -I$(srcdir)

# --- v2.1.0 additive test/tool targets (exec-helper harness, property
# --- tests, benchmark contention driver, umemctl live-inspection CLI) ---
test_unit_umem_env_helper_SOURCES = test/unit/umem_env_helper.c
test_unit_umem_env_helper_LDADD = libumem.la
test_unit_umem_env_helper_LDFLAGS = -pthread -lm
test_unit_umem_env_helper_CPPFLAGS = -I$(srcdir)

test_unit_repro_cpu_node_oob_SOURCES = test/unit/repro_cpu_node_oob.c
test_unit_repro_cpu_node_oob_LDADD = libumem.la
test_unit_repro_cpu_node_oob_LDFLAGS = -pthread
test_unit_repro_cpu_node_oob_CPPFLAGS = -I$(srcdir)

test_unit_test_rseq_fastpath_SOURCES = test/unit/test_rseq_fastpath.c
test_unit_test_rseq_fastpath_LDADD = libumem.la
test_unit_test_rseq_fastpath_LDFLAGS = -pthread
test_unit_test_rseq_fastpath_CPPFLAGS = -I$(srcdir)

test_property_prop_ownership_SOURCES = test/property/prop_ownership.c test/qc.c
test_property_prop_ownership_LDADD = libumem.la
test_property_prop_ownership_LDFLAGS = -pthread -lm
test_property_prop_ownership_CPPFLAGS = -I$(srcdir)


test_property_prop_profile_SOURCES = test/property/prop_profile.c test/qc.c
test_property_prop_profile_LDADD = libumem.la
test_property_prop_profile_LDFLAGS = -pthread -lm
test_property_prop_profile_CPPFLAGS = -I$(srcdir)

test_property_prop_palloc_SOURCES = test/property/prop_palloc.c test/qc.c
test_property_prop_palloc_LDADD = libumem.la
test_property_prop_palloc_LDFLAGS = -pthread -lm -lrt
test_property_prop_palloc_CPPFLAGS = -I$(srcdir)

test_bench_bench_contention_SOURCES = test/bench/bench_contention.c \
				test/bench/bench_framework.c \
				test/bench/allocators.c test/tdigest.c
test_bench_bench_contention_LDADD = libumem.la
test_bench_bench_contention_LDFLAGS = -pthread -lm
test_bench_bench_contention_CPPFLAGS = -I$(srcdir)

test_bench_bench_affinity_vs_mutex_SOURCES = test/bench/bench_affinity_vs_mutex.c
test_bench_bench_affinity_vs_mutex_LDFLAGS = -pthread

# introspect_churn: long-running workload to drive umemctl (G1-G4 tests).
test_integration_introspect_churn_SOURCES = test/integration/introspect_churn.c
test_integration_introspect_churn_LDADD = libumem.la
test_integration_introspect_churn_LDFLAGS = -pthread
test_integration_introspect_churn_CPPFLAGS = -I$(srcdir)

# umemctl: live-process inspection CLI. Pure AF_UNIX socket client, no
# libumem linkage; connects to a process running introspect=1.
tools_umemctl_SOURCES = tools/umemctl.c
# illumos: the AF_UNIX control-channel client needs libsocket/libnsl
# (empty on Linux/glibc, where socket() is in libc).
tools_umemctl_LDADD = $(SOLARIS_SOCKET_LIBS)

test_test_debug_SOURCES = test/test_debug.c
test_test_debug_LDADD = libumem.la
test_test_debug_LDFLAGS = -pthread

# Property-based tests
test_property_prop_alloc_free2_SOURCES = test/property/prop_alloc_free2.c test/qc.c
test_property_prop_alloc_free2_LDADD = libumem.la libumem_malloc.la
test_property_prop_alloc_free2_LDFLAGS = -pthread

test_property_prop_cache_SOURCES = test/property/prop_cache.c test/qc.c
test_property_prop_cache_LDADD = libumem.la libumem_malloc.la
test_property_prop_cache_LDFLAGS = -pthread

test_property_prop_fragmentation_SOURCES = test/property/prop_fragmentation.c test/qc.c
test_property_prop_fragmentation_LDADD = libumem.la libumem_malloc.la
test_property_prop_fragmentation_LDFLAGS = -pthread

# Benchmarks
test_bench_bench_main_SOURCES = test/bench/bench_main.c test/bench/bench_framework.c \
				test/bench/allocators.c test/tdigest.c
test_bench_bench_main_LDADD = libumem.la
test_bench_bench_main_LDFLAGS = -pthread -lm

# P2.1/P2.2 regressions: the benchmark harness's own accounting.  These are
# measurement defects, so the check is on the harness, not the allocator -- it
# uses a mock malloc/free allocator and asserts the arithmetic (total-vs-
# per-thread budget, the work floor, live-bytes-means-live-bytes, peak RSS
# paired with the live bytes it was sampled with, no ratio for workloads that
# hold no live set, honest thread counts).  Fast and deterministic, so it is
# in TESTS.
test_bench_test_bench_accounting_SOURCES = test/bench/test_bench_accounting.c \
				test/bench/bench_framework.c test/tdigest.c
test_bench_test_bench_accounting_LDADD =
test_bench_test_bench_accounting_LDFLAGS = -pthread -lm

test_bench_bench_depot_contention_SOURCES = test/bench/bench_depot_contention.c test/tdigest.c
test_bench_bench_depot_contention_LDADD = libumem.la
test_bench_bench_depot_contention_LDFLAGS = -pthread -lm

test_bench_bench_simd_SOURCES = test/bench/bench_simd.c
test_bench_bench_simd_LDADD =
test_bench_bench_simd_LDFLAGS = -lm

test_bench_bench_simd_threshold_SOURCES = test/bench/bench_simd_threshold.c
test_bench_bench_simd_threshold_LDADD =
test_bench_bench_simd_threshold_LDFLAGS = -lm

test_bench_bench_numa_hash_SOURCES = test/bench/bench_numa_hash.c umem_hash_partition.c
test_bench_bench_numa_hash_CPPFLAGS = -I$(srcdir)
test_bench_bench_numa_hash_LDADD =
test_bench_bench_numa_hash_LDFLAGS = -pthread -lm $(NUMA_LIBS)

test_bench_bench_profile_test_SOURCES = test/bench/bench_profile_test.c
test_bench_bench_profile_test_LDADD = libumem.la
test_bench_bench_profile_test_LDFLAGS = -pthread -lm

# Integration tests
test_integration_test_multithreaded_SOURCES = test/integration/test_multithreaded.c test/munit.c
test_integration_test_multithreaded_LDADD = libumem.la
test_integration_test_multithreaded_LDFLAGS = -pthread
test_integration_test_multithreaded_CPPFLAGS = -I$(srcdir)/test

test_integration_test_signals_SOURCES = test/integration/test_signals.c test/munit.c
test_integration_test_signals_LDADD = libumem.la
test_integration_test_signals_LDFLAGS = -pthread
test_integration_test_signals_CPPFLAGS = -I$(srcdir)/test

test_integration_test_oom_SOURCES = test/integration/test_oom.c test/munit.c
test_integration_test_oom_LDADD = libumem.la
test_integration_test_oom_LDFLAGS = -pthread
test_integration_test_oom_CPPFLAGS = -I$(srcdir)/test

test_integration_test_realloc_bootstrap_SOURCES = test/integration/test_realloc_bootstrap.c test/munit.c
test_integration_test_realloc_bootstrap_LDADD = libumem.la libumem_malloc.la
test_integration_test_realloc_bootstrap_LDFLAGS = -pthread
test_integration_test_realloc_bootstrap_CPPFLAGS = -I$(srcdir)/test

test_integration_test_debug_features_SOURCES = test/integration/test_debug_features.c test/munit.c
test_integration_test_debug_features_LDADD = libumem.la libumem_malloc.la
test_integration_test_debug_features_LDFLAGS = -pthread
test_integration_test_debug_features_CPPFLAGS = -I$(srcdir)/test

test_integration_test_threading_stress_SOURCES = test/integration/test_threading_stress.c test/munit.c
test_integration_test_threading_stress_LDADD = libumem.la libumem_malloc.la
test_integration_test_threading_stress_LDFLAGS = -pthread
test_integration_test_threading_stress_CPPFLAGS = -I$(srcdir)/test

# P1.2 regression: multithreaded fork under allocation load (fork lock order).
test_integration_test_fork_mt_load_SOURCES = test/integration/test_fork_mt_load.c
test_integration_test_fork_mt_load_LDADD = libumem.la
test_integration_test_fork_mt_load_LDFLAGS = -pthread

# P1.3a regression: a thread must return ALL its PTC-cached objects on exit.
noinst_PROGRAMS += test/integration/test_ptc_thread_exit_drain
test_integration_test_ptc_thread_exit_drain_SOURCES = \
	test/integration/test_ptc_thread_exit_drain.c
test_integration_test_ptc_thread_exit_drain_LDADD = libumem.la
test_integration_test_ptc_thread_exit_drain_LDFLAGS = -pthread
test_integration_test_ptc_thread_exit_drain_CPPFLAGS = -I$(srcdir)
test_integration_test_fork_mt_load_CPPFLAGS = -I$(srcdir)

# P1.6 regression: update-thread startup handshake.  -rdynamic so the
# executable's pthread_cond_wait interposer preempts libc's for calls made
# from libumem.so (see the comment at the top of the test).
test_integration_test_update_thread_startup_SOURCES = \
	test/integration/test_update_thread_startup.c
test_integration_test_update_thread_startup_LDADD = libumem.la
test_integration_test_update_thread_startup_LDFLAGS = -pthread -rdynamic
test_integration_test_update_thread_startup_CPPFLAGS = -I$(srcdir)

# Stress test (long-running, use -d 10 for quick CI)
test_stress_stress_main_SOURCES = test/stress/stress_main.c
test_stress_stress_main_LDADD = libumem.la
test_stress_stress_main_LDFLAGS = -pthread

# rseq repros.  BUILT ONLY UNDER --enable-rseq: unlike
# test/unit/test_rseq_fastpath.c and test/bench/bench_cache_rseq.c, neither
# of these two has a UMEM_RSEQ_AVAILABLE guard, and both use
# umem_rseq_cache_t and the umem_rseq_* entry points directly.  Listed
# unconditionally, they made `./configure --disable-rseq && make` fail to
# build -- invisible on any host with linux/rseq.h, since rseq is
# auto-detected on there.  Confirmed pre-existing:
# docs/results/2026-09-21-make-check-flaky-inspect-e2e.log.
if ENABLE_RSEQ
noinst_PROGRAMS += test/stress/repro_rseq_trailing_store \
		   test/stress/repro_naive_reload_race
endif

test_stress_repro_rseq_trailing_store_SOURCES = test/stress/repro_rseq_trailing_store.c
test_stress_repro_rseq_trailing_store_LDADD = libumem.la
test_stress_repro_rseq_trailing_store_LDFLAGS = -pthread
test_stress_repro_rseq_trailing_store_CPPFLAGS = -I$(srcdir)

test_stress_repro_naive_reload_race_SOURCES = test/stress/repro_naive_reload_race.c
test_stress_repro_naive_reload_race_LDADD = libumem.la
test_stress_repro_naive_reload_race_LDFLAGS = -pthread
test_stress_repro_naive_reload_race_CPPFLAGS = -I$(srcdir)

# Interposer fork participation (P1.2 cross-file follow-up).  Links only
# libumem.la; it is meant to be run under LD_PRELOAD=libumem_malloc.so and
# reports automake SKIP (77) when it is not, since without the interposer
# there is nothing of ours in front of malloc to test.
noinst_PROGRAMS += test/stress/repro_interpose_fork
test_stress_repro_interpose_fork_SOURCES = test/stress/repro_interpose_fork.c
test_stress_repro_interpose_fork_LDADD = libumem.la
test_stress_repro_interpose_fork_LDFLAGS = -pthread
test_stress_repro_interpose_fork_CPPFLAGS = -I$(srcdir)

test_stress_stress_concurrency_oracle_SOURCES = test/stress/stress_concurrency_oracle.c
test_stress_stress_concurrency_oracle_LDADD = libumem.la
test_stress_stress_concurrency_oracle_LDFLAGS = -pthread

# P1.1 regression: interposer calloc storage ownership under concurrency.
# Deliberately NOT linked against libumem_malloc.la -- it must be run under
# LD_PRELOAD (see test/stress/interpose_regress.sh), and it doubles as its own
# control when run against the platform allocator.
test_stress_repro_calloc_interpose_race_SOURCES = \
	test/stress/repro_calloc_interpose_race.c
test_stress_repro_calloc_interpose_race_LDFLAGS = -pthread

# P1.7(a,b) regression: checked arithmetic in the arena and bootstrap paths.
test_unit_test_overflow_contracts_SOURCES = test/unit/test_overflow_contracts.c
test_unit_test_overflow_contracts_LDADD = libumem.la
test_unit_test_overflow_contracts_CPPFLAGS = -I$(srcdir)

# P1.7(c,e) regression: aligned-allocation and realloc-ownership contracts.
# Like the calloc race, run both with and without LD_PRELOAD.
test_unit_test_aligned_contracts_SOURCES = test/unit/test_aligned_contracts.c
test_unit_test_aligned_contracts_CPPFLAGS = -I$(srcdir)

# P2.7 lifecycle coverage: thread churn, cache create/destroy churn, and
# cross-thread free, at a small deterministic scale for `make check`.  The
# stress-scale runs and the cases that need a controlled environment
# (LD_PRELOAD, UMEM_OPTIONS) are in test/stress/lifecycle_stress.sh, which
# reuses test_fork_mt_load and repro_reclaim_reuse rather than duplicating
# them.
test_unit_test_lifecycle_churn_SOURCES = test/unit/test_lifecycle_churn.c
test_unit_test_lifecycle_churn_LDADD = libumem.la
test_unit_test_lifecycle_churn_LDFLAGS = -pthread
test_unit_test_lifecycle_churn_CPPFLAGS = -I$(srcdir)

# Phase 3 item 3: umem_hooks.h concurrency contract (unregister drains
# in-flight callbacks; callbacks may re-enter the API; nonzero walk callback
# stops the walk).
test_unit_test_hook_contracts_SOURCES = test/unit/test_hook_contracts.c
test_unit_test_hook_contracts_LDADD = libumem.la
test_unit_test_hook_contracts_LDFLAGS = -pthread
test_unit_test_hook_contracts_CPPFLAGS = -I$(srcdir)

# Phase 3 items 1/2/7: umem_inspect cache-lifetime and snapshot contracts
# (inspection concurrent with cache destroy churn; walker callbacks may
# allocate; whatis distinguishes CACHED from held).
test_unit_test_inspect_contracts_SOURCES = test/unit/test_inspect_contracts.c
test_unit_test_inspect_contracts_LDADD = libumem.la
test_unit_test_inspect_contracts_LDFLAGS = -pthread
test_unit_test_inspect_contracts_CPPFLAGS = -I$(srcdir)

# P4 regression: weighted hash partitioning (umem_hash_partition.c).
# Compiles the unit directly rather than linking libumem, because
# umem_hash_partition.c is only part of the library under --enable-numa;
# this test must still run in a default build.  Not in TESTS for the same
# reason as the P1.2/P1.6 regressions -- see the comment above TESTS.
test_unit_test_hash_partition_SOURCES = test/unit/test_hash_partition.c \
	umem_hash_partition.c umem_hash_partition.h
test_unit_test_hash_partition_CPPFLAGS = -I$(srcdir)
test_unit_test_hash_partition_LDADD =
test_unit_test_hash_partition_LDFLAGS = -lm

# Debug overhead benchmark
test_bench_bench_debug_overhead_SOURCES = test/bench/bench_debug_overhead.c
test_bench_bench_debug_overhead_LDADD = libumem.la
test_bench_bench_debug_overhead_LDFLAGS = -pthread

# Slab-reclamation retention/RSS observation (P1.4/P1.5).  Reports RSS and
# per-arena retained bytes at fixed points in one sequence; it is an
# observation, not a benchmark, and makes no throughput claim.
test_bench_probe_reclaim_rss_SOURCES = test/bench/probe_reclaim_rss.c
test_bench_probe_reclaim_rss_LDADD = libumem.la
test_bench_probe_reclaim_rss_LDFLAGS = -pthread
test_bench_probe_reclaim_rss_CPPFLAGS = -I$(srcdir)

# Cache RSEQ benchmark
test_bench_bench_cache_rseq_SOURCES = test/bench/bench_cache_rseq.c test/tdigest.c
test_bench_bench_cache_rseq_LDADD = libumem.la
test_bench_bench_cache_rseq_LDFLAGS = -pthread -lm

tools_umem_profile_dump_SOURCES = tools/umem_profile_dump.c
tools_umem_profile_dump_LDADD =
tools_umem_profile_dump_LDFLAGS = -lm

# Install the runtime introspection CLIs on PATH.  tools/umem is a compiled
# C program (was a bash wrapper); umem_dump_reader remains a python script.
#
# umemctl is installed rather than noinst: README.md and docs document it as
# a command users run against their own processes, and a documented command
# that only exists in the build tree is not a command.
bin_PROGRAMS = tools/umem tools/umemctl
tools_umem_SOURCES = tools/umem.c
bin_SCRIPTS = tools/umem_dump_reader

# Pull the gdb / lldb scripts and the umem(1) entry point into the dist
# tarball.
debuggerdir = $(pkgdatadir)/debugger
dbg_gdbdir = $(debuggerdir)/gdb
dbg_lldbdir = $(debuggerdir)/lldb
dbg_gdb_DATA = tools/gdb/umem_gdb.py
dbg_lldb_DATA = tools/lldb/umem_lldb.py

man1_MANS = umem.1
man3_MANS = umem_alloc.3 umem_cache_create.3 umem_debug.3 umem_inspect.3 \
	umem_hooks.3
man7_MANS = umem_debugging.7
EXTRA_DIST = COPYRIGHT OPENSOLARIS.LICENSE umem.spec Doxyfile \
	$(man1_MANS) $(man3_MANS) $(man7_MANS) \
	scripts/run-coverage.sh scripts/run-sanitizers.sh \
	test/stress/lifecycle_stress.sh \
	tools/gdb/umem_gdb.py tools/lldb/umem_lldb.py \
	tools/umem_dump_reader \
	tools/DEBUGGING.md \
	examples/palloc_integration.c examples/README.md \
	examples/umem_palloc.c examples/umem_palloc.h \
	attic/README.md \
	test/install/external_consumer.c \
	test/qc.c test/qc.h test/munit.c test/munit.h \
	test/tdigest.c test/tdigest.h test/common.c test/common.h \
	test/bench/bench_framework.c test/bench/bench_framework.h test/bench/allocators.c \
	test/bench/test_bench_accounting.c \
	test/bench/bench_profile_test.c test/bench/expected_profile.txt \
	test/bench/verify_profile.sh \
	test/property/prop_alloc_free.c

# Installed headers.  Every header listed here must be compilable by an
# external consumer with nothing but the install prefix on its include
# path -- no "config.h", no umem_impl.h, no other uninstalled header.
# test/install/external_consumer.c is the check; see the install-check
# target at the bottom of this file.
#
# Deliberately NOT installed, because they include "config.h" (which is a
# build artifact and is not installed) and/or umem_impl.h (private):
#   umem_rseq.h, umem_numa.h, umem_impl.h, umem_base.h, vmem_base.h
# They used to be installed under --enable-rseq / --enable-numa, which
# produced an install whose own headers did not compile.
nobase_include_HEADERS = umem.h sys/vmem.h umem_hooks.h \
	umem_arena.h umem_own.h \
	umem_profile.h umem_stacktrace.h umem_inspect.h

# Test suite -- DELIBERATELY SMALL.  `make check` is a smoke suite, not the
# comprehensive one, and anything that quotes it as evidence should say so.
#
# test/test_main (the several-hundred-assertion unit suite) is excluded: it
# is run explicitly as `test/test_main --no-fork`.  Historically this was
# attributed to a pthread_create -> malloc -> pthread_getspecific deadlock
# under malloc interposition, documented in a PTHREAD_LIMITATION.md that no
# longer exists in the tree; the interposer has since been reworked (it now
# hands off to umem proper once initialization completes), so that
# attribution is stale.  Whether test_main can safely join TESTS has not
# been re-tested -- do not assume either way without running it.
TESTS = umem_test umem_test2 umem_test3 umem_ptc_fork_test test/test_debug \
	test/debugger/test_inspect_e2e.sh test/debugger/test_lldb_e2e.sh \
	test/stress/oracle_fast.sh

# Phase 1 regressions (docs/plans/2026-09-21-production-readiness.md).
#
# NOTE: this block previously ended with a trailing backslash.  GNU make
# continues a COMMENT across backslash-newline, so it silently swallowed the
# next two lines and dropped test_overflow_contracts and interpose_regress.sh
# out of TESTS entirely -- `make check` then reported a green 8/8 while two
# committed regressions never ran.  Never end a comment line here with a
# backslash.
#
# Run explicitly rather than from TESTS (long-running, or need a controlled
# environment):
#   test/integration/test_fork_mt_load            (P1.2 fork lock order)
#   test/integration/test_update_thread_startup   (P1.6 startup handshake)
#   test/stress/repro_interpose_fork              (interposer fork handling)
TESTS += test/unit/test_overflow_contracts \
	test/unit/test_aligned_contracts \
	test/unit/test_hash_partition \
	test/unit/test_hook_contracts \
	test/unit/test_inspect_contracts \
	test/stress/interpose_regress.sh \
	test/stress/oracle_control.sh \
	test/unit/test_lifecycle_churn \
	test/bench/test_bench_accounting \
	test/bench/check_budget.sh

TEST_EXTENSIONS = .sh
# The .sh tests are '#!/usr/bin/env bash' scripts using bash-only syntax, and
# they are shipped executable.  Let the kernel honour their shebang: with an
# empty compiler automake invokes the script itself.  This used to be
# $(SHELL), which forced them through /bin/sh -- dash on Debian/Ubuntu --
# where bash-only constructs fail for reasons that have nothing to do with
# the allocator.
SH_LOG_COMPILER =

test_test_inspect_SOURCES = test/test_inspect.c
test_test_inspect_LDADD = libumem.la

test_test_inspect_live_SOURCES = test/test_inspect_live.c
test_test_inspect_live_LDADD = libumem.la

dist_noinst_SCRIPTS = test/debugger/test_inspect_e2e.sh \
	test/debugger/test_lldb_e2e.sh \
	test/stress/oracle_fast.sh \
	test/stress/oracle_control.sh \
	test/bench/check_budget.sh \
	test/stress/interpose_regress.sh

# Generated API documentation goes to doxygen-out/, NOT docs/.
# docs/ is durable, version-controlled project documentation (plans, results,
# runbooks).  This target used to write into docs/ and clean-local used to
# `rm -rf docs`, so `make clean` silently deleted committed documentation.
html-local:
	mkdir -p doxygen-out
	doxygen Doxyfile

clean-local:
	rm -rf doxygen-out
	rm -f test/install/external_consumer

# install-check: prove an INSTALLED prefix is usable by an outside program.
#
# Installs into a throwaway DESTDIR and compiles + runs
# test/install/external_consumer.c against nothing but that prefix's
# include/ and lib/.  This fails if any installed header needs an
# uninstalled one ("config.h", umem_impl.h, ...), which is exactly what
# installing umem_rseq.h / umem_numa.h used to do, and also fails if the
# installed umem(1) cannot run.
#
# Run AFTER a successful build:  make install-check
install-check:
	@set -e; \
	d=$$(mktemp -d); \
	echo "install-check: DESTDIR=$$d"; \
	$(MAKE) $(AM_MAKEFLAGS) install DESTDIR=$$d >$$d/install.log 2>&1 || \
	    { echo "install-check: make install FAILED"; tail -30 $$d/install.log; exit 1; }; \
	inc=$$d$(includedir); lib=$$d$(libdir); bin=$$d$(bindir); \
	echo "install-check: compiling external consumer against $$inc"; \
	$(CC) -I$$inc -o $$d/external_consumer \
	    $(srcdir)/test/install/external_consumer.c \
	    -L$$lib -lumem -lpthread -lm || \
	    { echo "install-check: external consumer FAILED to build"; exit 1; }; \
	LD_LIBRARY_PATH=$$lib $$d/external_consumer; \
	echo "install-check: running installed umem(1)"; \
	LD_LIBRARY_PATH=$$lib $$bin/umem --help >/dev/null 2>&1 || \
	    LD_LIBRARY_PATH=$$lib $$bin/umem --version >/dev/null 2>&1 || \
	    { echo "install-check: installed umem(1) is not runnable"; exit 1; }; \
	test -x $$bin/umemctl || \
	    { echo "install-check: umemctl not installed"; exit 1; }; \
	echo "install-check: PASS (prefix $$d)"
.PHONY: install-check

# Coverage targets
if ENABLE_COVERAGE
coverage: check
	@echo "Generating coverage report..."
	lcov --capture --directory . --output-file coverage.info
	lcov --remove coverage.info '/usr/*' '*/test/*' --output-file coverage.info.cleaned
	genhtml coverage.info.cleaned --output-directory test/coverage
	@echo "Coverage report generated in test/coverage/index.html"

coverage-clean:
	@echo "Cleaning coverage data..."
	find . -name "*.gcda" -delete
	find . -name "*.gcno" -delete
	find . -name "*.gcov" -delete
	rm -f coverage.info coverage.info.cleaned
	rm -rf test/coverage
	@echo "Coverage data cleaned"
endif

# PGO targets
# Usage:
#   1. make pgo-generate  - Build instrumented version and collect profile
#   2. make pgo-use       - Build optimized version using profile data
#   3. make pgo-clean     - Remove profile data
pgo-generate:
	@echo "PGO Step 1: Building instrumented version..."
	$(MAKE) clean
	./configure --enable-pgo=generate
	$(MAKE) all
	@echo "PGO Step 2: Running benchmarks to collect profile data..."
	$(MAKE) check
	./test/bench/bench_main || true
	@echo "Profile data generated. Run 'make pgo-use' to build optimized version."

pgo-use:
	@echo "PGO Step 3: Building optimized version with profile data..."
	./configure --enable-pgo=use
	$(MAKE) all
	@echo "PGO build complete. This version is optimized for your workload."

pgo-clean:
	@echo "Cleaning PGO profile data..."
	find . -name "*.gcda" -delete
	rm -rf pgo-data
	@echo "PGO data cleaned"

# Generate compile_commands.json for clangd/LSP support
compile_commands.json:
	@echo "Generating compile_commands.json..."
	@$(PYTHON) $(srcdir)/gen_compile_commands.py $(srcdir)
	@echo "Generated compile_commands.json"

.PHONY: rpm coverage coverage-clean pgo-generate pgo-use pgo-clean compile_commands.json
rpm: dist-bzip2
	rpmbuild -ta $(distdir).tar.bz2
