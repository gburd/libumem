#!/usr/bin/env bash
# scripts/ec2/introspect_zerocost.sh — verify the introspection break hook is
# zero-cost when the feature is not compiled in: disassemble _umem_alloc/
# _umem_free from a default build vs an --enable-introspect build (runtime-off)
# and diff. They must be byte-identical.
set -e
OBJ=".libs/umem.o"
grab() { objdump -d --no-show-raw-insn "$OBJ" | sed -n "/<$1>:/,/^\$/p" | sed 's/^[0-9a-f]* //'; }

./scripts/ec2/clean-regen.sh >/dev/null 2>&1
make -j"$(nproc)" libumem.la >/dev/null 2>&1
grab _umem_alloc > /tmp/alloc_off.txt
grab _umem_free  > /tmp/free_off.txt

./scripts/ec2/clean-regen.sh --enable-introspect >/dev/null 2>&1
make -j"$(nproc)" libumem.la >/dev/null 2>&1
grab _umem_alloc > /tmp/alloc_on.txt
grab _umem_free  > /tmp/free_on.txt

rc=0
if diff -q /tmp/alloc_off.txt /tmp/alloc_on.txt >/dev/null; then
  echo "ALLOC_IDENTICAL"
else
  echo "ALLOC_DIFFERS"; diff /tmp/alloc_off.txt /tmp/alloc_on.txt | head -30; rc=1
fi
if diff -q /tmp/free_off.txt /tmp/free_on.txt >/dev/null; then
  echo "FREE_IDENTICAL"
else
  echo "FREE_DIFFERS"; diff /tmp/free_off.txt /tmp/free_on.txt | head -30; rc=1
fi
echo "alloc insn lines: $(wc -l < /tmp/alloc_off.txt) (off) / $(wc -l < /tmp/alloc_on.txt) (compiled-in)"
exit $rc
