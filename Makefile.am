EXTRA_DIST = COPYRIGHT OPENSOLARIS.LICENSE umem.spec Doxyfile

lib_LTLIBRARIES = libumem.la
noinst_PROGRAMS = umem_test

umem_test_SOURCES = umem_test.c
umem_test_LDADD = -lumem -lpthread -ldl

libumem_la_SOURCES =	init_lib.c \
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
			sys/vmem_impl_user.h

nobase_include_HEADERS = umem.h sys/vmem.h

TESTS = umem_test

html-local:
	mkdir -p docs
	doxygen Doxyfile

clean-local:
	rm -rf docs

.PHONY:	rpm
rpm:	$(distdir).tar.bz2
	rpmbuild -ta $<

# malloc.c

# XXX: Standalone version?
# See <http://cvs.opensolaris.org/source/xref/on/usr/src/lib/libumem/Makefile.com>

# XXX: Non-i386: SPARC asm. x86_64?
# Convert this to GNU as format: i386_subr_sol.s
