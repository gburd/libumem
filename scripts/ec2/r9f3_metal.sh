#!/usr/bin/env bash
# r9f3 step 3: metal A/B of owner_recycle OFF vs ON, SAME post library (r9f3
# tip). prodcons 1024:4096 at t=192 and t=384, -D 60:10 -W 5 (10 min run, first
# 5 min dropped). 3 runs per (config,threads). Reports ops/s, p99, p999, peak
# RSS from the per-interval CSV (steady-state intervals only). Then one perf
# pass per config: perf c2c (x86) / perf stat remote_access (arm), ON vs OFF,
# to show the remote-HITM/LLC-miss volume the feature removes. perf data -> /tmp
# (tmpfs, 189 GB) NEVER the root disk.
set -u
POST=3c5a247
BASE=~/libumem-metal-r9f3b
rm -rf "$BASE"; mkdir -p "$BASE"; cd "$BASE"
git clone --quiet https://codeberg.org/gregburd/libumem.git repo 2>/dev/null || \
  git clone --quiet ssh://codeberg.org/gregburd/libumem.git repo
cd repo && git fetch --quiet origin && git checkout --quiet "$POST"
./scripts/ec2/clean-regen.sh >regen.log 2>&1 || { echo REGEN_FAIL; tail -20 regen.log; exit 1; }
make -j"$(nproc)" libumem.la test/bench/bench_main >make.log 2>&1 || { echo MAKE_FAIL; tail -30 make.log; exit 1; }
export LD_LIBRARY_PATH=$PWD/.libs
export GLIBC_TUNABLES=glibc.rtld.optional_static_tls=65536
B=test/bench/.libs/bench_main
ARCH=$(uname -m)
O="$BASE/out"; mkdir -p "$O"
# steady 5-min window after dropping first 5 of ten 60s intervals
ARGS_T="-w prodcons -n 19200000 -s 1024:4096 -T 16 -c -D 60:10 -W 5"
{ echo "arch=$ARCH nproc=$(nproc) kernel=$(uname -r)"; . /etc/os-release; echo "os=$PRETTY_NAME"; echo "post=$POST"; } > "$O/prov.txt"

run_cfg() {  # $1 cfg(off|on) $2 threads $3 run#
  local cfg="$1" t="$2" r="$3" opt=""
  [ "$cfg" = on ] && opt="owner_recycle=1"
  UMEM_OPTIONS="$opt" numactl --interleave=all -- \
    $B -a umem $ARGS_T -t "$t" 2>/dev/null > "$O/csv-$cfg-t$t-r$r.csv"
}

echo "### THROUGHPUT A/B (3 runs each) ###"
for t in 192 384; do
  for cfg in off on; do
    for r in 1 2 3; do
      echo "run cfg=$cfg t=$t r=$r $(date -u +%T)"
      run_cfg "$cfg" "$t" "$r"
    done
  done
done

echo "### SUMMARY (steady intervals only: workload has /interval=K, K>=5 kept) ###"
# col7=ops_per_sec col11=lat_p99 col12=lat_p999 col15=rss_at_live_peak
for t in 192 384; do
  for cfg in off on; do
    echo "--- cfg=$cfg t=$t ---"
    cat "$O"/csv-$cfg-t$t-r*.csv 2>/dev/null | awk -F, '
      /interval=/ { ops[NR]=$7; p99[NR]=$11; p999[NR]=$12; rss[NR]=$15;
        n++; sops+=$7; if($15>maxrss)maxrss=$15 }
      END{ if(n>0) printf "  intervals=%d  mean_ops/s=%.3fM  peak_rss=%.1fMiB\n", n, sops/n/1e6, maxrss/1048576 }'
    # min/max ops across all steady intervals of all 3 runs = the range for the non-overlap rule
    cat "$O"/csv-$cfg-t$t-r*.csv 2>/dev/null | awk -F, '/interval=/{print $7}' | sort -n | awk '
      {v[NR]=$1} END{ if(NR>0) printf "  ops/s range: min=%.3fM max=%.3fM (n=%d)\n", v[1]/1e6, v[NR]/1e6, NR }'
    cat "$O"/csv-$cfg-t$t-r*.csv 2>/dev/null | awk -F, '/interval=/{print $11}' | sort -n | awk '{v[NR]=$1} END{ if(NR>0) printf "  p99 ns: med=%.0f max=%.0f\n", v[int(NR/2)+1], v[NR] }'
    cat "$O"/csv-$cfg-t$t-r*.csv 2>/dev/null | awk -F, '/interval=/{print $12}' | sort -n | awk '{v[NR]=$1} END{ if(NR>0) printf "  p999 ns: med=%.0f max=%.0f\n", v[int(NR/2)+1], v[NR] }'
  done
done

echo "### PERF mechanism A/B (feature OFF vs ON) -> /tmp ###"
sudo sysctl -w kernel.perf_event_paranoid=-1 >/dev/null 2>&1 || true
PA="-w prodcons -n 19200000 -s 1024:4096 -T 16 -c -D 60:7 -W 2 -t 192"
for cfg in off on; do
  opt=""; [ "$cfg" = on ] && opt="owner_recycle=1"
  if [ "$ARCH" = x86_64 ]; then
    echo "=== c2c $cfg ==="
    if UMEM_OPTIONS="$opt" perf c2c record -o /tmp/c2c-$cfg.data -- \
        numactl --interleave=all -- $B -a umem $PA >"$O/c2c-$cfg.run.log" 2>&1; then
      perf c2c report -i /tmp/c2c-$cfg.data --stdio 2>/dev/null | head -60 > "$O/c2c-$cfg.report.txt"
    else echo "C2C_UNAVAIL $cfg"; fi
    rm -f /tmp/c2c-$cfg.data
  else
    echo "=== perf stat remote $cfg ==="
    EV="instructions,cache-misses,ll_cache_rd,ll_cache_miss_rd,remote_access,remote_access_rd,l2d_cache_refill"
    UMEM_OPTIONS="$opt" perf stat -e "$EV" -o "$O/stat-$cfg.txt" -- \
      numactl --interleave=all -- $B -a umem $PA >"$O/stat-$cfg.run.log" 2>&1 || \
      UMEM_OPTIONS="$opt" perf stat -o "$O/stat-$cfg.txt" -- \
        numactl --interleave=all -- $B -a umem $PA >"$O/stat-$cfg.run.log" 2>&1 || echo "STAT_FAIL $cfg"
  fi
done
echo "METAL_DONE arch=$ARCH"
