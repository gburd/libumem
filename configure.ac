AC_INIT([umem], [1.0.2], [], [umem])
AM_INIT_AUTOMAKE([dist-bzip2])

AC_CONFIG_MACRO_DIRS([m4])

AC_PROG_CC
AM_PROG_AS
LT_INIT

AC_C_INLINE

# Detect OS and set conditionals
AC_MSG_CHECKING([for Solaris/Illumos build])
case "$host_os" in
  solaris*|*illumos*)
    AC_MSG_RESULT(yes)
    AM_CONDITIONAL(SOLARIS_BUILD, true)
    AC_DEFINE(SOLARIS_BUILD, [1], [Building on Solaris/Illumos])
    ;;
  *)
    AC_MSG_RESULT(no)
    AM_CONDITIONAL(SOLARIS_BUILD, false)
    ;;
esac

AC_MSG_CHECKING([system architecture])
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
    ;;
  *)
    AC_MSG_RESULT($host_cpu)
    AC_MSG_WARN([Unsupported architecture: $host_cpu, defaulting to generic])
    GENASM_DIR="sparc"
    ;;
esac

AC_SUBST(GENASM_DIR)

AC_MSG_CHECKING([whether pthread_mutex_t is larger than 24 bytes])
AC_RUN_IFELSE(
  [AC_LANG_SOURCE([
#include <pthread.h>
int main(void){return (sizeof(pthread_mutex_t) > 24);}
  ])],
  [AC_MSG_RESULT(yes)],
  [
    AC_MSG_RESULT(no)
    AC_DEFINE(UMEM_PTHREAD_MUTEX_TOO_BIG, [1], [need bigger cache])
    AC_MSG_WARN([*** increasing umem cpu cache size to compensate.])
  ]
)

AC_CHECK_LIB(dl,dlopen)

AC_CHECK_HEADERS([atomic.h sys/mman.h sys/sysmacros.h sys/time.h malloc.h])
AC_CHECK_FUNCS([issetugid mallinfo malloc_stats])

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile Doxyfile umem.spec])

AC_OUTPUT
