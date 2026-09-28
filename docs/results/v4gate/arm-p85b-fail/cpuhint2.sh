#!/bin/bash
V4=~/libumem-iso-v4gate-b8e0e02438df; V3=/tmp/v333
one() { LD_LIBRARY_PATH=$1/.libs $1/test/integration/.libs/test_cpu_hint_spread 2>&1 | grep -oE 'distinct per-cpu caches used: [0-9]+' | awk '{print $NF}'; }
run() { local tag=$1 dir=$2 n=$3 out=""; for i in $(seq $n); do out="$out $(one $dir)"; done; fails=$(echo $out | tr ' ' '\n' | awk '$1<4' | wc -l); echo "$tag: distinct=[$out ] fails(<4)=$fails/$n"; }
for L in 24 64; do
  for i in $(seq $L); do ( while :; do :; done ) & done; sleep 1
  run "v4     +$L busy" $V4 20
  run "v3.3.3 +$L busy" $V3 20
  kill $(jobs -p) 2>/dev/null; wait 2>/dev/null
done
echo done
