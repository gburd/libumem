#!/bin/sh
# One-shot: regenerate autotools on the EC2 instance with its own toolchain,
# then configure+build under ASan.  The worktree is rsynced from a host with
# newer autotools (1.18) than this instance (1.16); we delete the stale
# generated files and regenerate with the local toolchain so make never tries
# to rebuild them mid-build.
set -e
cd ~/libumem || exit 1
RM="rm"
$RM -rf autom4te.cache aclocal.m4 configure Makefile Makefile.in \
    config.status config.log conftest*
autoreconf -fi >/tmp/a.log 2>&1 || { echo RECONF_FAIL; tail -25 /tmp/a.log; exit 1; }
# autoreconf's embedded automake sometimes skips regenerating Makefile.in
# when a leftover Makefile from a prior maintainer-mode run has skewed
# timestamps; force it explicitly.
if [ ! -f Makefile.in ]; then
    automake --add-missing --copy >>/tmp/a.log 2>&1 || true
fi
[ -f Makefile.in ] || { echo NO_MAKEFILE_IN; tail -25 /tmp/a.log; exit 1; }
echo RECONF_OK
./configure --enable-asan >/tmp/c.log 2>&1 || { echo CONFIGURE_FAIL; tail -25 /tmp/c.log; exit 1; }
echo CONFIGURE_OK
# rsync preserves the host's mtimes, which are skewed relative to this
# instance's clock and toolchain; normalize so make never decides a
# generated autotools file is stale and tries to regenerate it (which would
# invoke the host's automake version baked into the synced Makefile.in).
touch aclocal.m4
sleep 1
touch configure config.h.in 2>/dev/null || true
sleep 1
find . -name 'Makefile.in' -exec touch {} +
sleep 1
touch config.status Makefile
make -j"$(nproc)" >/tmp/m.log 2>&1 || { echo BUILD_FAIL; tail -30 /tmp/m.log; exit 1; }
echo BUILD_OK
