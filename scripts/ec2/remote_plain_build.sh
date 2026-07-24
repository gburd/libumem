#!/bin/sh
# Build libumem WITHOUT sanitizers on the EC2 instance (for high-core stress
# where the pre-existing umem_cpu_node OOB would trip ASan on >256-CPU boxes).
# Same clean-regen + timestamp-normalize approach as remote_asan_build.sh.
set -e
cd ~/libumem || exit 1
RM="rm"
$RM -rf autom4te.cache aclocal.m4 configure Makefile Makefile.in \
    config.status config.log conftest*
autoreconf -fi >/tmp/a.log 2>&1 || { echo RECONF_FAIL; tail -25 /tmp/a.log; exit 1; }
if [ ! -f Makefile.in ]; then
    automake --add-missing --copy >>/tmp/a.log 2>&1 || true
fi
[ -f Makefile.in ] || { echo NO_MAKEFILE_IN; tail -25 /tmp/a.log; exit 1; }
echo RECONF_OK
./configure >/tmp/c.log 2>&1 || { echo CONFIGURE_FAIL; tail -25 /tmp/c.log; exit 1; }
echo CONFIGURE_OK
touch aclocal.m4; sleep 1
touch configure config.h.in 2>/dev/null || true; sleep 1
find . -name 'Makefile.in' -exec touch {} +; sleep 1
touch config.status Makefile
make clean >/dev/null 2>&1 || true
# make clean can miss subdir-objects .o; scrub stale objects from any prior
# (e.g. ASan) build so the plain link does not pull in sanitizer stubs.
find . \( -name '*.o' -o -name '*.lo' \) -delete
find . -name '.libs' -type d -exec $RM -rf {} + 2>/dev/null || true
make -j"$(nproc)" >/tmp/m.log 2>&1 || { echo BUILD_FAIL; tail -30 /tmp/m.log; exit 1; }
echo BUILD_OK
