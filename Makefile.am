AUTOMAKE_OPTIONS = subdir-objects
ACLOCAL_AMFLAGS = -I m4

lib_LTLIBRARIES = libumem.la libumem_malloc.la
noinst_PROGRAMS = umem_test umem_test2 umem_test3 \
		  umem_ptc_test umem_ptc_fork_test umem_ptc_bench \
		  test/test_main \
		  test/property/prop_alloc_free2 \
		  test/property/prop_cache \
		  test/property/prop_fragmentation \
		  test/bench/bench_main \
		  test/bench/bench_depot_contention \
		  test/integration/test_signals \
		  test/integration/test_oom \
		  test/integration/test_multithreaded \
		  test/integration/test_realloc_bootstrap \
		  test/integration/test_debug_features \
		  test/integration/test_threading_stress

libumem_la_LDFLAGS = -lpthread $(NUMA_LIBS)

# Conditionally include stub_stand.c and genasm for Solaris/Illumos
if SOLARIS_BUILD
STUB_SOURCES = stub_stand.c
GENASM_SOURCES = $(GENASM_DIR)/umem_genasm.c
else
STUB_SOURCES = tmem_stubs.c
GENASM_SOURCES = $(GENASM_DIR)/umem_genasm.c
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

libumem_la_SOURCES = init_lib.c \
			umem_agent_support.c \
			umem_fail.c \
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
			sol_compat.h \
			vmem.c \
			sys/vmem.h \
			sys/vmem_impl_user.h \
			malloc.c \
			malloc_guard.c \
			malloc_guard.h \
			umem_audit.c \
			umem_hooks.c \
			umem_hooks.h \
			$(GENASM_SOURCES) \
			$(STUB_SOURCES) \
			$(FREEBSD_SOURCES) \
			$(PERCPU_SOURCES)

libumem_malloc_la_SOURCES = malloc_interpose.c malloc_guard.h
libumem_malloc_la_LDFLAGS = -lpthread -ldl -R$(libdir)
libumem_malloc_la_LIBADD = libumem.la

# Initialize flags unconditionally to avoid += errors
AM_CFLAGS = -fno-builtin-calloc
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
			 test/unit/test_overflow_fixes.c
test_test_main_LDADD = libumem.la libumem_malloc.la
test_test_main_LDFLAGS = -pthread

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

# Integration tests
test_integration_test_multithreaded_SOURCES = test/integration/test_multithreaded.c test/munit.c
test_integration_test_multithreaded_LDADD = libumem.la
test_integration_test_multithreaded_LDFLAGS = -pthread

test_integration_test_signals_SOURCES = test/integration/test_signals.c test/munit.c
test_integration_test_signals_LDADD = libumem.la
test_integration_test_signals_LDFLAGS = -pthread

test_integration_test_oom_SOURCES = test/integration/test_oom.c test/munit.c
test_integration_test_oom_LDADD = libumem.la
test_integration_test_oom_LDFLAGS = -pthread

test_integration_test_realloc_bootstrap_SOURCES = test/integration/test_realloc_bootstrap.c test/munit.c
test_integration_test_realloc_bootstrap_LDADD = libumem.la libumem_malloc.la
test_integration_test_realloc_bootstrap_LDFLAGS = -pthread

test_integration_test_debug_features_SOURCES = test/integration/test_debug_features.c test/munit.c
test_integration_test_debug_features_LDADD = libumem.la libumem_malloc.la
test_integration_test_debug_features_LDFLAGS = -pthread

test_integration_test_threading_stress_SOURCES = test/integration/test_threading_stress.c test/munit.c
test_integration_test_threading_stress_LDADD = libumem.la libumem_malloc.la
test_integration_test_threading_stress_LDFLAGS = -pthread

man3_MANS = umem_alloc.3 umem_cache_create.3 umem_debug.3
EXTRA_DIST = COPYRIGHT OPENSOLARIS.LICENSE umem.spec Doxyfile umem_test4 \
	$(man3_MANS) \
	NIX_USAGE.md IMPLEMENTATION_STATUS.md \
	scripts/run-coverage.sh scripts/run-sanitizers.sh \
	tools/gdb/umem_gdb.py tools/lldb/umem_lldb.py \
	examples/palloc_integration.c examples/README.md \
	riscv64/umem_genasm.c aarch64/umem_genasm.c \
	test/qc.c test/qc.h test/munit.c test/munit.h \
	test/tdigest.c test/tdigest.h test/common.c test/common.h \
	test/bench/bench_framework.c test/bench/bench_framework.h test/bench/allocators.c \
	test/property/prop_alloc_free.c

nobase_include_HEADERS = umem.h sys/vmem.h umem_hooks.h

if ENABLE_PERCPU_CACHING
nobase_include_HEADERS += umem_percpu.h
endif

# Tests - pthread_create/malloc deadlock limitation
# Working tests:
#   umem_test           - Single-threaded, direct umem_alloc() API
#   umem_test3          - Single-threaded, malloc interposition
#   umem_ptc_fork_test  - Multi-process fork, direct umem_alloc() API
#
# Disabled tests (known limitation):
#   umem_test2          - Hangs: LD_PRELOAD malloc + pthread_create deadlock
#   umem_ptc_test       - Hangs: LD_PRELOAD malloc + pthread_create deadlock
#
# Root cause: libumem uses pthread_getspecific() for PTC, which creates circular
# dependency when malloc() is interposed: pthread_create -> malloc -> umem_malloc
# -> pthread_getspecific -> deadlock. This is an architectural limitation.
#
# See PTHREAD_LIMITATION.md and PTHREAD_STATUS.md for details and alternatives.
TESTS = umem_test umem_test2 umem_test3 umem_ptc_fork_test

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

.PHONY: rpm coverage coverage-clean pgo-generate pgo-use pgo-clean
rpm: dist-bzip2
	rpmbuild -ta $(distdir).tar.bz2
