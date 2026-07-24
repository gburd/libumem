#!/usr/bin/env bash
# Prove the DEFAULT build (UMEM_INTROSPECT undefined) is byte-identical whether
# or not the introspect source edits are present in umem.c -- i.e. the feature
# adds ZERO cost to the production hot path unless --enable-introspect.
set -e
grab() { objdump -d --no-show-raw-insn .libs/umem.o | sed -n "/<$1>:/,/^\$/p" | sed 's/^[0-9a-f]* //'; }

# Build A: current umem.c (introspect source present) in the DEFAULT config.
./scripts/ec2/clean-regen.sh >/dev/null 2>&1
make -j"$(nproc)" libumem.la >/dev/null 2>&1
grab _umem_alloc > /tmp/mine_alloc.txt
grab _umem_free  > /tmp/mine_free.txt

# Build B: umem.c with all introspect edits removed, DEFAULT config.
cp umem.c /tmp/umem.c.bak
python3 - <<'PY'
import re
s=open('umem.c').read()
s=s.replace('#include "umem_introspect.h"\n','')
s=s.replace('if (likely(bin >= 0) &&\n\t\t\t    likely(!umem_introspect_break_armed)) {','if (likely(bin >= 0)) {')
s=re.sub(r'#ifdef UMEM_INTROSPECT\n\t\tif \(unlikely\(umem_introspect_break_armed\) && buf != NULL\)\n\t\t\tumem_introspect_break_check\(buf, size, cp\);\n#endif\n','',s)
s=re.sub(r'\n\t/\*\n\t \* Start the in-process introspection channel.*?umem_introspect_start\(\);\n','\n',s,flags=re.S)
open('umem.c','w').write(s)
PY
echo "introspect refs remaining in stripped umem.c: $(grep -c introspect umem.c || true)"
make -j"$(nproc)" libumem.la >/dev/null 2>&1
grab _umem_alloc > /tmp/orig_alloc.txt
grab _umem_free  > /tmp/orig_free.txt
cp /tmp/umem.c.bak umem.c

rc=0
if diff -q /tmp/mine_alloc.txt /tmp/orig_alloc.txt >/dev/null; then echo ALLOC_DEFAULT_IDENTICAL; else echo ALLOC_DEFAULT_DIFFERS; diff /tmp/mine_alloc.txt /tmp/orig_alloc.txt | head; rc=1; fi
if diff -q /tmp/mine_free.txt /tmp/orig_free.txt >/dev/null; then echo FREE_DEFAULT_IDENTICAL; else echo FREE_DEFAULT_DIFFERS; diff /tmp/mine_free.txt /tmp/orig_free.txt | head; rc=1; fi
echo "alloc lines: mine=$(wc -l </tmp/mine_alloc.txt) orig=$(wc -l </tmp/orig_alloc.txt)"
exit $rc
