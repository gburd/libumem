#!/bin/bash
# v4.0.0 verification, run inside a verify-isolated tree.  Needs ~/v333.tar
# (git archive v3.3.3) on the box for the export diff against the last .so.1.
cat /etc/os-release | grep -E '^PRETTY_NAME'; gcc --version | head -1
r() { printf '\n=== %s ===\n' "$*"; }
r "0. build"
./scripts/ec2/clean-regen.sh >/tmp/v4rg.log 2>&1 && make -j$(nproc) >/tmp/v4b.log 2>&1; echo "build_rc=$?"; tail -3 /tmp/v4b.log
r "1. sonames"
for f in .libs/libumem.so .libs/libumem_malloc.so; do echo "$f -> $(readlink -f $f | xargs basename) SONAME=$(objdump -p $f | awk '/SONAME/{print $2}')"; done
echo "libumem_malloc NEEDED: $(objdump -p .libs/libumem_malloc.so | awk '/NEEDED/{print $2}' | tr '\n' ' ')"
r "2. no umem_sm_ / sm_ exports"
echo "umem_sm_ exports: $(nm -D --defined-only .libs/libumem.so | grep -c ' umem_sm_')"
echo "any sm_ symbol at all: $(nm .libs/libumem.so | grep -cE ' (umem_)?sm_[a-z]')"
echo "libumem.so defined_exports=$(nm -D --defined-only .libs/libumem.so | wc -l)"
echo "libumem_malloc.so defined_exports=$(nm -D --defined-only .libs/libumem_malloc.so | wc -l)"
nm -D --defined-only .libs/libumem.so | awk '{print $NF}' | sort > /tmp/v4-exports.txt
r "3. baseline v3.3.3 exports"
B=/tmp/v333; find $B -delete 2>/dev/null; mkdir -p $B && tar xf "$HOME/v333.tar" -C $B 2>/dev/null
if [ -f $B/configure.ac ]; then ( cd $B && ./scripts/ec2/clean-regen.sh >/dev/null 2>&1 && make -j$(nproc) >/tmp/v333b.log 2>&1 ); echo "v333_build_rc=$?"
  nm -D --defined-only $B/.libs/libumem.so | awk '{print $NF}' | sort > /tmp/v333-exports.txt
  echo "v3.3.3 libumem.so defined_exports=$(wc -l </tmp/v333-exports.txt) umem_sm_=$(grep -c '^umem_sm_' /tmp/v333-exports.txt)"
  echo "removed (v3.3.3 -> v4): $(comm -23 /tmp/v333-exports.txt /tmp/v4-exports.txt | wc -l), of which umem_sm_: $(comm -23 /tmp/v333-exports.txt /tmp/v4-exports.txt | grep -c '^umem_sm_')"
  echo "removed non-sm: $(comm -23 /tmp/v333-exports.txt /tmp/v4-exports.txt | grep -v '^umem_sm_' | tr '\n' ' ')"
  echo "added: $(comm -13 /tmp/v333-exports.txt /tmp/v4-exports.txt | tr '\n' ' ')"
  nm -D --defined-only $B/.libs/libumem_malloc.so | awk '{print $NF}' | sort > /tmp/v333m.txt
  nm -D --defined-only .libs/libumem_malloc.so | awk '{print $NF}' | sort > /tmp/v4m.txt
  cmp -s /tmp/v333m.txt /tmp/v4m.txt && echo "libumem_malloc exports: identical to v3.3.3" || { echo "libumem_malloc exports DIFFER"; diff /tmp/v333m.txt /tmp/v4m.txt; }
else echo "v3.3.3 baseline unavailable (~/v333.tar missing)"; fi
r "4. LD_PRELOAD via the .so.1 drop-in still works"
LD_LIBRARY_PATH=.libs LD_PRELOAD=.libs/libumem_malloc.so.1 /bin/ls / >/dev/null; echo "preload_ls_rc=$?"
LD_LIBRARY_PATH=.libs LD_PRELOAD=.libs/libumem_malloc.so.1 /bin/sh -c 'grep -c umem /proc/self/maps'
r "5. exit_criteria_gate"
bash scripts/ec2/exit_criteria_gate.sh >/tmp/v4gate.log 2>&1; echo "gate_rc=$?"
grep -E '^=== |rc=|^# (TOTAL|PASS|SKIP|XFAIL|FAIL|XPASS|ERROR)|GATE|tarball=|^(FAIL|ERROR):|RESULT' /tmp/v4gate.log
r "6. --enable-introspect make check"
./scripts/ec2/clean-regen.sh --enable-introspect >/dev/null 2>&1 && make -j$(nproc) >/dev/null 2>&1 && make check -j$(nproc) >/tmp/v4ck-i.log 2>&1; echo "introspect_rc=$?"
grep -E '^# (TOTAL|PASS|SKIP|XFAIL|FAIL|XPASS|ERROR)|^(FAIL|ERROR|XPASS):' /tmp/v4ck-i.log
r "7. P8.5b regressions"
./scripts/ec2/clean-regen.sh >/dev/null 2>&1 && make -j$(nproc) >/dev/null 2>&1
bash scripts/ec2/p85b_fix_lo_gate.sh 30 >/tmp/v4p85.log 2>&1; echo "p85b_rc=$?"; grep -E '^GATE' /tmp/v4p85.log
mkdir -p ~/v4-out && cp /tmp/v4*.log /tmp/v4-exports.txt /tmp/v333-exports.txt ~/v4-out/ 2>/dev/null
echo "=== v4gate done"
