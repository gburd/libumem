AUTOMAKE_OPTIONS = subdir-objects
ACLOCAL_AMFLAGS = -I m4

lib_LTLIBRARIES = libumem.la libumem_malloc.la
noinst_PROGRAMS = umem_test umem_test2 umem_test3 \
		  umem_ptc_test umem_ptc_fork_test umem_ptc_bench

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
			$(GENASM_SOURCES) \
			$(STUB_SOURCES)

libumem_malloc_la_SOURCES = malloc.c
libumem_malloc_la_LDFLAGS = -lpthread -R$(libdir)
libumem_malloc_la_LIBADD = libumem.la

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

man3_MANS = umem_alloc.3 umem_cache_create.3 umem_debug.3
EXTRA_DIST = COPYRIGHT OPENSOLARIS.LICENSE umem.spec Doxyfile umem_test4 \
	$(man3_MANS)

nobase_include_HEADERS = umem.h sys/vmem.h

TESTS = umem_test umem_test2 umem_test3 umem_test4 umem_ptc_test umem_ptc_fork_test

html-local:
	mkdir -p docs
	doxygen Doxyfile

clean-local:
	rm -rf docs

.PHONY: rpm
rpm: dist-bzip2
	rpmbuild -ta $(distdir).tar.bz2
