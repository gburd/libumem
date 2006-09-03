AC_INIT([umem], [1.0], [], [umem])
AM_INIT_AUTOMAKE([dist-bzip2])

AC_ARG_ENABLE([malloc-replacement],
              AS_HELP_STRING([--enable-malloc-replacement],
                             [Include implementations of malloc/free/etc. in libumem (default is no)]),
              [case "${enableval}" in
                yes) malloc_replacement=true ;;
                no)  malloc_replacement=false ;;
                *) AC_MSG_ERROR(bad value ${enableval} for --enable-malloc-replacement) ;;
              esac],[malloc_replacement=false])
AM_CONDITIONAL(MALLOC_REPLACEMENT, test x$malloc_replacement = xtrue)

AC_PROG_CC
AM_PROG_AS
AC_PROG_LIBTOOL

AC_C_INLINE

AC_CHECK_HEADERS([sys/mman.h sys/sysmacros.h sys/time.h])

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile Doxyfile umem.spec])

AC_OUTPUT
