#!/usr/bin/env bash
# Phase-4 release/packaging verification, run on EC2 via job.sh.
#
# Covers the plan's P2.6 items that belong to this workstream:
#   * installed prefix is usable by an external program (install-check)
#   * make dist produces a tarball that configures, builds, and passes check
#   * the tarball can build test/property/prop_palloc, which #includes
#     examples/umem_palloc.c -- the file that was in no source list and no
#     EXTRA_DIST, so a clean tarball could not build it
#   * a pre-fix control for the hash-partition regression
set -x

echo "=== 0. PRE-FIX CONTROL: hash-partition test against the pre-fix source ==="
# The remote tree is an rsync copy with no .git, so the pre-fix source is
# staged locally (scripts/ec2/stage_prefix_control.sh) as
# /tmp/prefix-control/old_hash_partition.c inside the synced tree.
mkdir -p /tmp/ctl
if [ -f tmp/prefix-control/old_hash_partition.c ]; then
	cp tmp/prefix-control/old_hash_partition.c /tmp/ctl/old_hash.c
elif git rev-parse --git-dir >/dev/null 2>&1; then
	git show 413a795^:umem_hash_partition.c > /tmp/ctl/old_hash.c
else
	echo "PRE_FIX_CONTROL_SKIPPED: no staged pre-fix source and no git"
fi
if [ -f /tmp/ctl/old_hash.c ]; then
	cp umem_hash_partition.h test/unit/test_hash_partition.c /tmp/ctl/
	gcc -I/tmp/ctl -o /tmp/ctl/ctl /tmp/ctl/test_hash_partition.c \
	    /tmp/ctl/old_hash.c -lm
	set +e
	/tmp/ctl/ctl
	echo "PRE_FIX_EXIT=$? (nonzero == the regression reproduces pre-fix)"
	set -e
fi

echo "=== 1. make install-check (installed prefix + external consumer) ==="
make install-check 2>&1 | tail -25

echo "=== 2. make dist ==="
make dist 2>&1 | tail -8
ls -la umem-*.tar.gz

echo "=== 3. configure + make + make check from the extracted tarball ==="
D=/tmp/distcheck
[ -d "$D" ] && mv "$D" "$D.old.$$"
mkdir -p "$D"
tar xzf umem-2.7.0.tar.gz -C "$D"
cd "$D/umem-2.7.0"
if ./configure > /tmp/dist-cfg.log 2>&1; then
	echo TARBALL_CONFIGURE_OK
else
	echo TARBALL_CONFIGURE_FAIL
	tail -25 /tmp/dist-cfg.log
	exit 1
fi
if make -j"$(nproc)" > /tmp/dist-make.log 2>&1; then
	echo TARBALL_MAKE_OK
else
	echo TARBALL_MAKE_FAIL
	tail -40 /tmp/dist-make.log
	exit 1
fi
make check 2>&1 | tail -18

echo "=== 4. tarball can build prop_palloc (examples/umem_palloc.c in EXTRA_DIST) ==="
if make test/property/prop_palloc 2>&1 | tail -6; then
	echo PROP_PALLOC_OK
else
	echo PROP_PALLOC_FAIL
fi

echo "=== 5. tarball install-check ==="
make install-check 2>&1 | tail -12
