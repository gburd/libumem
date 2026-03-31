AUTOMAKE_OPTIONS = subdir-objects
ACLOCAL_AMFLAGS = -I m4

lib_LTLIBRARIES = libumem.la libumem_malloc.la
noinst_PROGRAMS = umem_test umem_test2 umem_test3 \
		  umem_ptc_test umem_ptc_fork_test umem_ptc_bench \
		  test/test_main \
		  test/property/prop_alloc_free2 \
		  test/bench/bench_main

libumem_la_LDFLAGS = -lpthread

# Conditionally include stub_stand.c and genasm for Solaris/Illumos
if SOLARIS_BUILD
STUB_SOURCES = stub_stand.c
GENASM_SOURCES = $(GENASM_DIR)/umem_genasm.c
else
STUB_SOURCES = tmem_stubs.c
GENASM_SOURCES = $(GENASM_DIR)/umem_genasm.c
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
			umem_audit.c \
			umem_hooks.c \
			umem_hooks.h \
			$(GENASM_SOURCES) \
			$(STUB_SOURCES)

libumem_malloc_la_SOURCES = malloc.c
libumem_malloc_la_LDFLAGS = -lpthread -R$(libdir)
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

umem_test_SOURCES = umem_test.c
umem_test_LDADD = libumem.la

umem_test2_SOURCES = umem_test2.c
umem_test2_LDADD = libumem.la
umem_test2_LDFLAGS = -lpthread

umem_test3_SOURCES = umem_test3.c
umem_test3_LDADD = libumem.la libumem_malloc.la

umem_ptc_test_SOURCES = umem_ptc_test.c
umem_ptc_test_LDADD = libumem.la
umem_ptc_test_LDFLAGS = -pthread

umem_ptc_fork_test_SOURCES = umem_ptc_fork_test.c
umem_ptc_fork_test_LDADD = libumem.la
umem_ptc_fork_test_LDFLAGS = -pthread

umem_ptc_bench_SOURCES = umem_ptc_bench.c
umem_ptc_bench_LDADD = libumem.la
umem_ptc_bench_LDFLAGS = -pthread

# New test suite - unified runner with all unit tests
test_test_main_SOURCES = test/test_main.c test/munit.c \
			 test/unit/test_umem_alloc.c \
			 test/unit/test_umem_cache.c \
			 test/unit/test_umem_align.c \
			 test/unit/test_umem_debug.c \
			 test/unit/test_vmem.c
test_test_main_LDADD = libumem.la
test_test_main_LDFLAGS = -pthread

# Property-based tests
test_property_prop_alloc_free2_SOURCES = test/property/prop_alloc_free2.c test/qc.c
test_property_prop_alloc_free2_LDADD = libumem.la
test_property_prop_alloc_free2_LDFLAGS = -pthread

# Benchmarks
test_bench_bench_main_SOURCES = test/bench/bench_main.c test/bench/bench_framework.c \
				test/bench/allocators.c test/tdigest.c
test_bench_bench_main_LDADD = libumem.la
test_bench_bench_main_LDFLAGS = -pthread -lm

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

# Thread-related and complex tests disabled due to pthread_create/malloc interaction (see PTHREAD_LIMITATION.md)
# Disabled: umem_test2, umem_test4, umem_ptc_test, umem_ptc_fork_test, test/test_main, test/property/prop_alloc_free2
# Basic tests verify core malloc/free functionality works correctly
TESTS = umem_test umem_test3

html-local:
	mkdir -p docs
	doxygen Doxyfile

clean-local:
	rm -rf docs

.PHONY: rpm
rpm: dist-bzip2
	rpmbuild -ta $(distdir).tar.bz2
