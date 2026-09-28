#!/usr/bin/env bash
# docs/results/r8exit/exit_storm_ab.sh -- run in ~/libumem-ab-r8exit after
# hotpath_ab.sh has built {pre,pre2,post}.  One exit_storm binary (built
# against post's umem.h), library swapped by LD_LIBRARY_PATH.  Null
# (pre vs pre2) first, then pre vs post; REPLICATES alternating pairs each.
set +e
T="${T:-8}"; ROUNDS="${ROUNDS:-300}"; REPLICATES="${REPLICATES:-9}"
OUT="${OUTDIR:-$PWD/results}/exit-storm-t$T.txt"; mkdir -p "$(dirname "$OUT")"
for a in pre pre2 post; do
	rm -rf /tmp/es_$a; mkdir -p /tmp/es_$a
	cp -L $a/.libs/libumem.so.2 /tmp/es_$a/ || exit 1
	sha256sum /tmp/es_$a/libumem.so.2
done | tee "$OUT"
gcc -O2 -I post -o /tmp/exit_storm exit_storm.c /tmp/es_post/libumem.so.2 -pthread || exit 1
sha256sum /tmp/exit_storm | tee -a "$OUT"
run() { LD_LIBRARY_PATH=/tmp/es_$1 /tmp/exit_storm "$T" "$ROUNDS"; }
pair() {  # $1 A $2 B $3 label
	for i in $(seq "$REPLICATES"); do
		if [ $((i % 2)) = 1 ]; then x=$(run $1); y=$(run $2); else y=$(run $2); x=$(run $1); fi
		echo "$3 $i A[$1] $x | B[$2] $y"
	done | tee -a "$OUT"
}
echo "# exit_storm T=$T ROUNDS=$ROUNDS vcpu=$(nproc) $(date -u +%FT%TZ)" | tee -a "$OUT"
pair pre pre2 null
pair pre post ab
