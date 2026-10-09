#!/usr/bin/env bash
# r9f3 gate 1 (objdump byte-identical) + gate 2 (hotpath A/B), feature OFF.
set -e
PRE=4bbfe44; POST=3c5a247
BASE=~/libumem-ab-r9f3b
rm -rf "$BASE"; mkdir -p "$BASE"; cd "$BASE"
git clone --quiet https://codeberg.org/gregburd/libumem.git repo 2>/dev/null || \
  git clone --quiet ssh://codeberg.org/gregburd/libumem.git repo
cd repo && git fetch --quiet origin '+refs/heads/*:refs/remotes/origin/*' && cd ..
for tag in pre:$PRE post:$POST pre2:$PRE; do
  d="${tag%%:*}"; r="${tag##*:}"; rm -rf "$d"; mkdir -p "$d"
  ( cd repo && git archive "$r" ) | tar -x -C "$d"
done
NCPU=$(nproc)
echo "=== BUILD pre($PRE) and post($POST) ==="
for d in pre post; do
  ( cd "$d" && ./scripts/ec2/clean-regen.sh >/dev/null 2>&1 && make -j"$NCPU" >/dev/null 2>&1 ) \
    && echo "built $d ok" || { echo "BUILD $d FAILED"; exit 1; }
done
PRESO=$(ls pre/.libs/libumem.so.*.*.* | head -1)
POSTSO=$(ls post/.libs/libumem.so.*.*.* | head -1)
echo
echo "=== GATE 1: objdump of fast-path symbols (feature OFF, byte-identical?) ==="
SYMS="_umem_alloc _umem_free _umem_cache_alloc _umem_cache_free"
rc=0
for s in $SYMS; do
  objdump -d --disassemble="$s" "$PRESO"  2>/dev/null | sed '1,/>:/d' | sed 's/^ *[0-9a-f]*://' | awk '{$1=$1};1' > /tmp/pre_$s.txt
  objdump -d --disassemble="$s" "$POSTSO" 2>/dev/null | sed '1,/>:/d' | sed 's/^ *[0-9a-f]*://' | awk '{$1=$1};1' > /tmp/post_$s.txt
  if diff -q /tmp/pre_$s.txt /tmp/post_$s.txt >/dev/null; then
    echo "  $s: IDENTICAL ($(wc -l < /tmp/pre_$s.txt) insns)"
  else
    echo "  $s: DIFFERS"
    diff /tmp/pre_$s.txt /tmp/post_$s.txt | head -40
    rc=1
  fi
done
echo "GATE1_RC=$rc"
echo
echo "=== GATE 2: hotpath A/B (feature OFF) ==="
PRE=$PRE POST=$POST OUTDIR="$BASE/results" ./post/scripts/ec2/hotpath_ab.sh || true
echo "=== hotpath-ab.txt ==="
cat "$BASE/results/hotpath-ab.txt"
