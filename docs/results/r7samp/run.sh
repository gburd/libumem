#!/usr/bin/env bash
# r7samp proof: test_bench_accounting + perf of frag at t=8, -T 1 vs -T 16.
# Run from an isolated `git archive` tree (scripts/ec2/verify-isolated.sh).
set -u
./scripts/ec2/clean-regen.sh || exit 1
make -j"$(nproc)" >/tmp/r7samp-make.log 2>&1 || { tail -30 /tmp/r7samp-make.log; exit 1; }
make -j"$(nproc)" test/bench/bench_main test/bench/test_bench_accounting >>/tmp/r7samp-make.log 2>&1 \
	|| { tail -30 /tmp/r7samp-make.log; exit 1; }
echo "=== provenance ==="
cat ISOLATED_PROVENANCE 2>/dev/null; uname -m; nproc; gcc --version | head -1
ldd --version | head -1; dpkg -l libjemalloc2 2>/dev/null | tail -1
command -v perf >/dev/null || sudo apt-get install -y -q linux-perf >/dev/null 2>&1
perf --version
sudo sysctl -q kernel.perf_event_paranoid=-1 kernel.kptr_restrict=0

echo "=== test_bench_accounting ==="
./test/bench/test_bench_accounting; echo "TEST_RC=$?"

B=test/bench/bench_main
[ -x test/bench/.libs/bench_main ] && B=test/bench/.libs/bench_main
export LD_LIBRARY_PATH="$PWD/.libs:${LD_LIBRARY_PATH:-}"
for a in jemalloc umem; do
	for T in 1 16; do
		tag="$a-frag-T$T"
		echo "=== perf $tag: bench_main -a $a -c -w frag -t 8 -n 800000 -s 16:4096 -D 10:2 -W 1 -T $T ==="
		perf record -F 499 -g -o "/tmp/$tag.data" -- \
			"$B" -a "$a" -c -w frag -t 8 -n 800000 -s 16:4096 -D 10:2 -W 1 -T "$T" \
			2>"/tmp/$tag.err"
		echo "rc=$?"; cat "/tmp/$tag.err" | tail -3
		perf report -i "/tmp/$tag.data" --no-children --sort sym --stdio 2>/dev/null \
			| grep -v '^#' | grep -v '^$' | grep '%' >"/tmp/$tag.top"
		echo "--- summary (self %) ---"
		awk '/clock_gettime|vdso/ {c+=$1} / td_[a-z_]+/ {t+=$1}
		     END {printf "SUMMARY %s clock_gettime+vdso=%.2f td_*=%.2f\n", "'"$tag"'", c, t}' "/tmp/$tag.top"
		echo "--- top 20 symbols ---"; head -20 "/tmp/$tag.top"
		echo "--- dso ---"
		perf report -i "/tmp/$tag.data" --no-children --sort dso --stdio 2>/dev/null \
			| grep '%' | grep -v '^#' | head -8
	done
done
