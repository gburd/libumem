#!/usr/bin/env bash
# Clean-and-build recipe for EC2 remote runs.
# Local (NixOS) build artifacts (libtool with /run/current-system paths,
# asan-tainted Makefile/config.status, automake-1.18 aclocal.m4) get rsynced
# over and break the EC2 build. Purge all generated files, regenerate on the
# instance, and touch timestamps so make doesn't re-invoke automake-1.18.
find . -name "*.o" -delete 2>/dev/null || true
find . -name "*.lo" -delete 2>/dev/null || true
find . -name "*.so*" -delete 2>/dev/null || true
find . -name "*.la" -delete 2>/dev/null || true
find . -name "*.a" -delete 2>/dev/null || true
find . -maxdepth 1 \( -name aclocal.m4 -o -name Makefile.in -o -name configure \
    -o -name Makefile -o -name config.status -o -name libtool \
    -o -name config.h -o -name config.h.in \) -delete 2>/dev/null || true
./autogen.sh >/tmp/ag.log 2>&1 || { echo "AUTOGEN_FAIL"; tail -20 /tmp/ag.log; exit 1; }
./configure "$@" >/tmp/cfg.log 2>&1 || { echo "CONFIGURE_FAIL"; tail -20 /tmp/cfg.log; exit 1; }
touch aclocal.m4 configure Makefile.in Makefile config.status
echo "REGEN_OK"
