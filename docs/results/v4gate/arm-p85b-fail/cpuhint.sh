#!/bin/bash
V4=~/libumem-iso-v4gate-b8e0e02438df; V3=/tmp/v333
for t in "$V4" "$V3"; do [ -x $t/test/integration/.libs/test_cpu_hint_spread ] || { cd $t && make -j$(nproc) test/integration/test_cpu_hint_spread >/dev/null 2>&1; }; done
one() { LD_LIBRARY_PATH=$1/.libs $1/test/integration/.libs/test_cpu_hint_spread 2>&1 | grep -oE 'distinct per-cpu caches used: [0-9]+' | awk '{print $NF}'; }
run() { local tag=$1 dir=$2 n=$3 out=""; for i in $(seq $n); do out="$out $(one $dir)"; done; fails=$(echo $out | tr ' ' '\n' | awk '$1<4' | wc -l); echo "$tag: distinct=[$out ] fails(<4)=$fails/$n"; }
echo "nproc=$(nproc) $(uname -r)"
run "v4    idle" $V4 20
run "v3.3.3 idle" $V3 20
for i in $(seq $(nproc)); do ( while :; do :; done ) & done; LOADPIDS=$(jobs -p); sleep 1
run "v4    +8 busy loops" $V4 20
run "v3.3.3 +8 busy loops" $V3 20
kill $LOADPIDS 2>/dev/null; wait 2>/dev/null
echo done
