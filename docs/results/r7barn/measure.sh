# usage: bash measure.sh <threads> <outdir>   (run from the isolated default tree)
set -u
T=$1; OUT=$2; top=$(pwd); P="$top-probe"
mkdir -p "$OUT"
export GLIBC_TUNABLES=glibc.rtld.optional_static_tls=65536
{ echo "sha=$(cat ISOLATED_PROVENANCE | sed -n 's/^sha=//p')"; uname -a; nproc
  curl -s -m 2 http://169.254.169.254/latest/meta-data/instance-type 2>/dev/null || true; echo
  . /etc/os-release; echo "$PRETTY_NAME"; gcc --version | head -1; ldd --version | head -1
  sha256sum "$P/.libs/libumem.so" "$top/.libs/libumem.so"; } > "$OUT/provenance.txt" 2>&1
if [ "$T" -ge 64 ]; then NP=$((T*400000)); FR=64:256; else NP=40000000; FR=16:4096; fi
FN=$((T*4*25000))   # frag -n sizes the pool as n/threads/4 objects: 25k/thread
POOL=$((T*4*25000))
run() { # label, then bench args; probe build
  local l=$1; shift
  ( cd "$P" && LD_LIBRARY_PATH=.libs timeout 900 ./test/bench/.libs/bench_main -a umem -c "$@" \
      > "$OUT/$l.csv" 2> "$OUT/$l.err"; echo "rc=$?" >> "$OUT/$l.err" )
  echo "== $l"; grep -E "^BARN (src|stale|allocs|umem_alloc_)" "$OUT/$l.err"; tail -1 "$OUT/$l.err"
}
run prodcons-n  -w prodcons -t $T -n $NP -s 64:256
run frag-n      -w frag     -t $T -n $FN -s $FR
run prodcons-D  -w prodcons -t $T -n $POOL -s 64:256 -D 10:4 -W 1
run frag-D      -w frag     -t $T -n $POOL -s $FR    -D 10:4 -W 1
# sustained_load.sh continuous prodcons (+frag), probe build
( cd "$P" && SUSTAINED_MODE=continuous SUSTAINED_WINDOWS=3 SUSTAINED_WARMUPS=1 \
    SUSTAINED_FRAG_SIZES=$FR timeout 1200 ./scripts/ec2/sustained_load.sh umem 20 $T \
    > "$OUT/sustained.out" 2>&1; echo "rc=$?" >> "$OUT/sustained.out" )
cp "$P"/docs/results/*/sustained.toml.log "$OUT/sustained.log" 2>/dev/null || \
  find "$P/docs/results" -name 'sustained.toml.log' -newer "$OUT/provenance.txt" -exec cp {} "$OUT/sustained.log" \;
echo "== sustained"; grep -E "^BARN (src|stale|allocs|umem_alloc_)" "$OUT/sustained.log"; tail -2 "$OUT/sustained.out"
# perf on the DEFAULT build
sudo sysctl -w kernel.perf_event_paranoid=-1 >/dev/null 2>&1
for w in prodcons frag; do
  s=64:256; [ $w = frag ] && s=$FR
  LD_LIBRARY_PATH=.libs perf record -F 499 -g -o "$OUT/perf-$w.data" -- \
    ./test/bench/.libs/bench_main -a umem -c -w $w -t $T -n $POOL -s $s -D 10:2 -W 1 \
    > "$OUT/perf-$w.csv" 2> "$OUT/perf-$w.err"
  perf report -i "$OUT/perf-$w.data" --stdio --no-children -g none --percent-limit 0.3 \
    --sort symbol 2>/dev/null | grep -v "^#" | grep -v "^$" | head -40 > "$OUT/perf-$w.top"
  perf report -i "$OUT/perf-$w.data" --stdio --children -g none --percent-limit 0.3 \
    --sort symbol 2>/dev/null | grep -v "^#" | grep -v "^$" | head -60 > "$OUT/perf-$w.children"
  rm -f "$OUT/perf-$w.data"
  echo "== perf $w (self)"; head -15 "$OUT/perf-$w.top"
done
echo MEASURE_DONE
