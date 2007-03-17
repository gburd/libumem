AC_INIT([umem], [1.0], [], [umem])
AM_INIT_AUTOMAKE([dist-bzip2])

dnl AC_ARG_ENABLE([malloc-replacement],
dnl               AS_HELP_STRING([--enable-malloc-replacement],
dnl                              [Include implementations of malloc/free/etc. in libumem (default is no)]),
dnl               [case "${enableval}" in
dnl                 yes) malloc_replacement=true ;;
dnl                 no)  malloc_replacement=false ;;
dnl                 *) AC_MSG_ERROR(bad value ${enableval} for --enable-malloc-replacement) ;;
dnl               esac],[malloc_replacement=false])
dnl AM_CONDITIONAL(MALLOC_REPLACEMENT, test x$malloc_replacement = xtrue)

AC_PROG_CC
AM_PROG_AS
AC_PROG_LIBTOOL

AC_C_INLINE

AC_CHECK_HEADERS([sys/mman.h sys/sysmacros.h sys/time.h])

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile Doxyfile umem.spec])

AC_OUTPUT
