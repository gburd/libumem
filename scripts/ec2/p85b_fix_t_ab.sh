#!/usr/bin/env bash
# scripts/ec2/p85b_fix_t_ab.sh -- rseq on vs UMEM_OPTIONS=norseq, same binary,
# bench_main multi 16:256 at t=1 and t=$(nproc), 5 alternating runs each.
# Run from the root of an already-built tree.  Prints the CSV rows.
set -u
export LD_LIBRARY_PATH="$PWD/.libs"
B=test/bench/.libs/bench_main
for t in 1 "$(nproc)"; do
	n=$(( t == 1 ? 20000000 : 96000000 ))
	for i in 1 2 3 4 5; do
		for arm in on off; do
			if [ $arm = off ]; then o=norseq; else o=; fi
			printf 't=%s arm=%s run=%s ' "$t" "$arm" "$i"
			UMEM_OPTIONS=$o "$B" -a umem -w multi -t "$t" -n "$n" -s 16:256 -A -c 2>/dev/null | grep "^umem," | tail -1
		done
	done
done
