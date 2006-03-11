lib_LTLIBRARIES = libumem.la
libumem_la_SOURCES =	init_lib.c \
			umem_agent_support.c \
			umem_fail.c \
			umem_fork.c \
			umem_update_thread.c \
			vmem_mmap.c \
			vmem_sbrk.c \
			envvar.o \
			getpcstack.o \
			malloc.o \
			misc.o \
			vmem_base.o \
			umem.o \
			vmem.o

# XXX: Standalone version?
# See <http://cvs.opensolaris.org/source/xref/on/usr/src/lib/libumem/Makefile.com>

# XXX: Non-i386: SPARC asm. x86_64?
# Convert this to GNU as format: i386_subr_sol.s
