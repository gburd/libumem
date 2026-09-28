#!/bin/bash
# r7h4 step 2: perf stat at 512 N=128 t=1, alternating arms, 5 runs each, one bench binary.
set +e
O=~/r7h4; B=/tmp/hab_bench_pairs; RUNS=${RUNS:-5}
perf list 2>/dev/null > $O/perf-list.txt
EV=""
for e in cycles instructions branch-misses L1-icache-load-misses frontend_retired.dsb_miss idq_uops_not_delivered.core \
         idq.dsb_uops idq.mite_uops frontend_retired.l1i_miss icache_data.stalls baclears.any; do
  r=$(perf stat -x, -e $e:u true 2>&1 | grep -v "^#" | head -1)
  case "$r" in *"not supported"*|*"not counted"*|*"event syntax"*|"") echo "event $e: UNAVAILABLE ($r)";; *) EV="$EV,$e:u";; esac
done | tee $O/perf-events.txt
for e in cycles instructions branch-misses L1-icache-load-misses frontend_retired.dsb_miss idq_uops_not_delivered.core idq.dsb_uops idq.mite_uops frontend_retired.l1i_miss icache_data.stalls baclears.any; do
  r=$(perf stat -x, -e $e:u true 2>&1 | grep -v "^#" | head -1)
  case "$r" in *"not supported"*|*"not counted"*|*"event syntax"*|"") ;; *) EV="$EV,$e:u";; esac
done; EV=${EV#,}; echo "events: $EV" | tee -a $O/perf-events.txt
: > $O/perf-raw.csv
for i in $(seq 1 $RUNS); do for t in ${ARMS:-pre post}; do
  d=/tmp/hab_$t   # the exact libs hotpath_ab.sh (ab-main) just measured
  out=$(LD_LIBRARY_PATH=$d perf stat -x, -e "$EV" numactl --physcpubind=0 --localalloc -- $B -s 512 -n 128 -t 1 -d 1 2>&1)
  pairs=$(echo "$out" | awk '/Mpairs/{print $(NF-1)*1e6}')
  echo "$out" | awk -F, -v t=$t -v i=$i -v p=$pairs '$3 ~ /:u$/ || $3 ~ /[a-z]/ {if ($1 ~ /^[0-9.]+$/) printf "%s,%d,%s,%s,%s,%s\n", t,i,$3,$1,p,$5}' >> $O/perf-raw.csv
done; done
# per-pair medians per arm; delta post vs pre
awk -F, '{v[$1","$3]=v[$1","$3]" "($4/$5); ev[$3]=1; mp[$1]=mp[$1]" "$5}
 function med(s,  a,n,i,j,t){n=split(s,a," ");for(i=1;i<=n;i++)for(j=i+1;j<=n;j++)if(a[j]+0<a[i]+0){t=a[i];a[i]=a[j];a[j]=t}; return (n%2)?a[(n+1)/2]:(a[n/2]+a[n/2+1])/2}
 END{ printf "%-36s %14s %14s %9s\n","event (per pair, median of runs)","f40a2da","post","delta";
  for (e in ev){a=med(v["pre,"e]); b=med(v["post,"e]); printf "%-36s %14.4f %14.4f %+8.2f%%\n", e, a, b, (a>0)?(b-a)/a*100:0}
  a=med(mp["pre"]); b=med(mp["post"]); printf "%-36s %14.0f %14.0f %+8.2f%%\n","pairs/s (-d 1)",a,b,(b-a)/a*100 }' $O/perf-raw.csv | tee $O/perf-summary.txt
