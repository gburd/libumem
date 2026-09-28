#!/usr/bin/env bash
# scripts/ec2/r8rel_o5_perf.sh -- O5 perf redo, x86 lo, quiet box.
# Builds pre (8c70e66 = 79ae76f^, before size-aware free) and post (744f639)
# prodcons benches, perf-records the DIRECT .libs/bench_main (no shell wrapper),
# restricts perf report to the bench PID, reports the free-wrapper self% and the
# real "Throughput:" lines. Confirms libc/jemalloc prodcons unchanged.
#
# Ships two git archives (pre.tar, post.tar) placed in ~ by the caller.
set -u
J=$(nproc)
OPS=20000000            # 20M ops, t=8 => 2.5M/thread; enough samples, ~few s
OUT=~/o5-perf; rm -rf "$OUT"; mkdir -p "$OUT"

build() { # dir tarball
  local d="$1" tb="$2"
  rm -rf "$d"; mkdir -p "$d"; tar -xf "$tb" -C "$d"
  ( cd "$d" && ./scripts/ec2/clean-regen.sh CFLAGS="-O2 -g -fno-omit-frame-pointer" >regen.log 2>&1 \
      && make -j"$J" test/bench/bench_main >make.log 2>&1 ) \
    || { echo "BUILD_FAIL $d"; tail -20 "$d/make.log" "$d/regen.log"; return 1; }
}

# perf a single (alloc,label) directly on the .libs binary; restrict report to bench PID.
runperf() { # dir label alloc freewrapper
  local d="$1" label="$2" alloc="$3" fw="$4"
  local bin="$d/test/bench/.libs/bench_main"
  [ -x "$bin" ] || bin="$d/test/bench/bench_main"   # fall back to wrapper only if libtool didn't split
  echo "=== $label ($alloc) ==="
  # Run once WITHOUT perf to capture the clean Throughput line (no perf overhead).
  ( cd "$d" && LD_LIBRARY_PATH=.libs ./test/bench/.libs/bench_main -a "$alloc" -w prodcons -t 8 -n "$OPS" ) \
     > "$OUT/${label}.thr.log" 2>&1
  grep -E "Throughput:" "$OUT/${label}.thr.log" | tail -3
  # perf record on the direct binary. --call-graph=fp; output per label.
  ( cd "$d" && LD_LIBRARY_PATH=.libs perf record --call-graph=fp -o "$OUT/${label}.data" -- \
      ./test/bench/.libs/bench_main -a "$alloc" -w prodcons -t 8 -n "$OPS" ) \
      > "$OUT/${label}.rec.log" 2>&1
  # Identify the bench PID from the perf.data (the recorded workload is the only child).
  perf report -i "$OUT/${label}.data" --stdio --no-children --sort=pid 2>/dev/null \
      | grep -E "bench_main" | head > "$OUT/${label}.pids.log"
  BPID=$(perf report -i "$OUT/${label}.data" --stdio --no-children --sort=pid 2>/dev/null \
      | grep -oE ":[0-9]+" | tr -d ':' | sort -u | head -1)
  echo "bench pid(s): $(cat "$OUT/${label}.pids.log")"
  # Self% by symbol, restricted to the bench PID.
  perf report -i "$OUT/${label}.data" --stdio --no-children --sort=symbol \
      ${BPID:+--pid=$BPID} 2>/dev/null > "$OUT/${label}.self.log"
  echo "-- top self symbols ($label) --"
  grep -vE "^#|^$" "$OUT/${label}.self.log" | head -15
  echo "-- free-wrapper self% ($fw) --"
  grep -E "$fw" "$OUT/${label}.self.log" | head -3
}

echo "########## BUILD PRE (8c70e66) ##########"
build ~/o5pre ~/pre.tar || exit 1
echo "########## BUILD POST (744f639) ##########"
build ~/o5post ~/post.tar || exit 1

echo "########## PRE perf (umem, umem_free_wrapper) ##########"
runperf ~/o5pre pre_umem umem "umem_free_wrapper"
echo "########## POST perf (umem, umem_free_sized_wrapper) ##########"
runperf ~/o5post post_umem umem "umem_free_sized_wrapper"

echo "########## libc control (pre vs post) ##########"
runperf ~/o5pre  pre_libc  libc "libc_free"
runperf ~/o5post post_libc libc "libc_free"
echo "########## jemalloc control (pre vs post) ##########"
runperf ~/o5pre  pre_jemalloc  jemalloc "je_"
runperf ~/o5post post_jemalloc jemalloc "je_"

echo "########## SUMMARY: Throughput lines ##########"
for f in pre_umem post_umem pre_libc post_libc pre_jemalloc post_jemalloc; do
  echo "[$f] $(grep -E 'Throughput:' "$OUT/${f}.thr.log" | tail -1)"
done
echo "########## O5 PERF DONE ##########"
