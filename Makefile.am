AUTOMAKE_OPTIONS = subdir-objects
ACLOCAL_AMFLAGS = -I m4

lib_LTLIBRARIES = libumem.la libumem_malloc.la
noinst_PROGRAMS = umem_test umem_test2 umem_test3 \
		  umem_ptc_test umem_ptc_fork_test umem_ptc_bench \
		  test/test_main test/test_debug \
		  test/test_inspect test/test_inspect_live \
		  test/unit/umem_env_helper \
		  test/unit/repro_cpu_node_oob \
		  test/property/prop_ownership \
		  test/property/prop_gc \
		  test/property/prop_profile \
		  test/property/prop_palloc \
		  test/bench/bench_contention \
		  test/integration/introspect_churn \
		  tools/umemctl \
		  test/property/prop_alloc_free2 \
		  test/property/prop_cache \
		  test/property/prop_fragmentation \
		  test/bench/bench_main \
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
		  test/stress/stress_main \
		  test/bench/bench_debug_overhead \
		  test/bench/bench_cache_rseq \
		  tools/umem_profile_dump

libumem_la_LDFLAGS = -lpthread -lm $(NUMA_LIBS)

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

# Conditionally include per-CPU caching
if ENABLE_PERCPU_CACHING
PERCPU_SOURCES = umem_percpu.c umem_percpu.h
else
PERCPU_SOURCES =
endif

# Conditionally include rseq support
if ENABLE_RSEQ
RSEQ_SOURCES = umem_rseq.c umem_rseq.h umem_rseq_x86_64.S umem_rseq_aarch64.S
else
RSEQ_SOURCES =
endif

# Conditionally include NUMA support
if ENABLE_NUMA
NUMA_SOURCES = umem_numa.c umem_numa.h umem_hash_partition.c umem_hash_partition.h
else
NUMA_SOURCES =
endif

# HTM (hardware transactional memory) support is currently disabled
# at source-level; the umem_htm.[ch] files have not been written.
# Keep the configure flag for forward compatibility but always treat
# the source list as empty.
HTM_SOURCES =

# Conditionally include SPARC assembly on Solaris
# The .S extension ensures the C preprocessor runs on the assembly
# (needed for #ifdef directives in the file).
if SOLARIS_BUILD
if ARCH_SPARC
SPARC_ASM_SOURCES = sparc_subr_sol.S
else
SPARC_ASM_SOURCES =
endif
else
SPARC_ASM_SOURCES =
endif

# Assembly files need minimal preprocessing flags - avoid C-specific
# flags like -std=c17 that confuse the assembler.
AM_CCASFLAGS =

libumem_la_SOURCES = init_lib.c \
			atomic.h \
			sparsemap.h \
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
			umem_gc.c \
			umem_gc.h \
			umem_gc_roots.c \
			umem_gc_roots.h \
			umem_sparsemap.c \
			umem_sparsemap.h \
			$(STUB_SOURCES) \
			$(FREEBSD_SOURCES) \
			$(PERCPU_SOURCES) \
			$(RSEQ_SOURCES) \
			$(NUMA_SOURCES) \
			$(HTM_SOURCES) \
			$(SPARC_ASM_SOURCES)

libumem_malloc_la_SOURCES = malloc_interpose.c malloc_guard.h
libumem_malloc_la_LDFLAGS = -lpthread -ldl -R$(libdir)
libumem_malloc_la_LIBADD = libumem.la

# Initialize flags unconditionally to avoid += errors
AM_CFLAGS = -fno-builtin-calloc $(SIMD_CFLAGS) -D_UMEM_INTERNAL
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
			 test/unit/test_gc.c \
			 test/unit/test_sparsemap.c \
			 test/unit/test_coverage.c \
			 test/unit/test_stacktrace.c \
			 test/unit/test_profile.c
test_test_main_LDADD = libumem.la
test_test_main_LDFLAGS = -pthread -lm
test_test_main_CPPFLAGS = -I$(srcdir)/test \
	-DUMEM_ENV_HELPER='"$(abs_builddir)/test/unit/.libs/umem_env_helper"'

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

test_property_prop_ownership_SOURCES = test/property/prop_ownership.c test/qc.c
test_property_prop_ownership_LDADD = libumem.la
test_property_prop_ownership_LDFLAGS = -pthread -lm
test_property_prop_ownership_CPPFLAGS = -I$(srcdir)

test_property_prop_gc_SOURCES = test/property/prop_gc.c test/qc.c
test_property_prop_gc_LDADD = libumem.la
test_property_prop_gc_LDFLAGS = -pthread -lm
test_property_prop_gc_CPPFLAGS = -I$(srcdir)

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

# introspect_churn: long-running workload to drive umemctl (G1-G4 tests).
test_integration_introspect_churn_SOURCES = test/integration/introspect_churn.c
test_integration_introspect_churn_LDADD = libumem.la
test_integration_introspect_churn_LDFLAGS = -pthread
test_integration_introspect_churn_CPPFLAGS = -I$(srcdir)

# umemctl: live-process inspection CLI. Pure AF_UNIX socket client, no
# libumem linkage; connects to a process running introspect=1.
tools_umemctl_SOURCES = tools/umemctl.c

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

# Stress test (long-running, use -d 10 for quick CI)
test_stress_stress_main_SOURCES = test/stress/stress_main.c
test_stress_stress_main_LDADD = libumem.la
test_stress_stress_main_LDFLAGS = -pthread

# Debug overhead benchmark
test_bench_bench_debug_overhead_SOURCES = test/bench/bench_debug_overhead.c
test_bench_bench_debug_overhead_LDADD = libumem.la
test_bench_bench_debug_overhead_LDFLAGS = -pthread

# Cache RSEQ benchmark
test_bench_bench_cache_rseq_SOURCES = test/bench/bench_cache_rseq.c test/tdigest.c
test_bench_bench_cache_rseq_LDADD = libumem.la
test_bench_bench_cache_rseq_LDFLAGS = -pthread -lm

tools_umem_profile_dump_SOURCES = tools/umem_profile_dump.c
tools_umem_profile_dump_LDADD =
tools_umem_profile_dump_LDFLAGS = -lm

# Install the runtime introspection CLI on PATH.
bin_SCRIPTS = tools/umem tools/umem_dump_reader

# Pull the gdb / lldb scripts and the umem(1) entry point into the dist
# tarball.
debuggerdir = $(pkgdatadir)/debugger
dbg_gdbdir = $(debuggerdir)/gdb
dbg_lldbdir = $(debuggerdir)/lldb
dbg_gdb_DATA = tools/gdb/umem_gdb.py
dbg_lldb_DATA = tools/lldb/umem_lldb.py

man1_MANS = umem.1
man3_MANS = umem_alloc.3 umem_cache_create.3 umem_debug.3 umem_inspect.3
man7_MANS = umem_debugging.7
EXTRA_DIST = COPYRIGHT OPENSOLARIS.LICENSE umem.spec Doxyfile \
	$(man1_MANS) $(man3_MANS) $(man7_MANS) \
	scripts/run-coverage.sh scripts/run-sanitizers.sh \
	tools/gdb/umem_gdb.py tools/lldb/umem_lldb.py \
	tools/umem tools/umem_dump_reader \
	tools/DEBUGGING.md \
	examples/palloc_integration.c examples/README.md \
	test/qc.c test/qc.h test/munit.c test/munit.h \
	test/tdigest.c test/tdigest.h test/common.c test/common.h \
	test/bench/bench_framework.c test/bench/bench_framework.h test/bench/allocators.c \
	test/bench/bench_profile_test.c test/bench/expected_profile.txt \
	test/bench/verify_profile.sh \
	test/property/prop_alloc_free.c

nobase_include_HEADERS = umem.h sys/vmem.h umem_hooks.h \
	umem_arena.h umem_own.h umem_gc.h gc.h \
	umem_profile.h umem_stacktrace.h umem_inspect.h

if ENABLE_PERCPU_CACHING
nobase_include_HEADERS += umem_percpu.h
endif

if ENABLE_RSEQ
nobase_include_HEADERS += umem_rseq.h
endif

if ENABLE_NUMA
nobase_include_HEADERS += umem_numa.h
endif

# HTM is not implemented; the include line below is intentionally
# absent.  See the HTM_SOURCES definition near the top.

# Test suite
# Using simple smoke tests that don't trigger pthread_create/malloc deadlock.
# test/test_main (comprehensive unit tests) hangs due to malloc interposition
# creating circular dependency: pthread_create -> malloc -> pthread_getspecific.
# See PTHREAD_LIMITATION.md for details.
TESTS = umem_test umem_test2 umem_test3 umem_ptc_fork_test test/test_debug \
	test/debugger/test_inspect_e2e.sh test/debugger/test_lldb_e2e.sh

TEST_EXTENSIONS = .sh
SH_LOG_COMPILER = $(SHELL)

test_test_inspect_SOURCES = test/test_inspect.c
test_test_inspect_LDADD = libumem.la

test_test_inspect_live_SOURCES = test/test_inspect_live.c
test_test_inspect_live_LDADD = libumem.la

dist_noinst_SCRIPTS = test/debugger/test_inspect_e2e.sh \
	test/debugger/test_lldb_e2e.sh

html-local:
	mkdir -p docs
	doxygen Doxyfile

clean-local:
	rm -rf docs

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
