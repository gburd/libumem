#!/usr/bin/env bash
# r7v_run.sh <job> <name>=<configure flags> ...
# Harness for the @r7verify check of aa36fdd.  Run from the verify-isolated.sh
# directory (a pristine `git archive` of the ref).  Each config builds in its
# own copy of that pristine tree, sequentially (one make check at a time on the
# box), and its evidence lands in ~/libumem-jobs/<job>/<name>/ so job.sh fetch
# picks it up.  Does not stop at a failing config.
set -u
SRC="$PWD"; J="$HOME/libumem-jobs/$1"; shift
cat "$SRC/ISOLATED_PROVENANCE"; uname -a; nproc; gcc --version | head -n 1
for spec in "$@"; do
	name="${spec%%=*}"; flags="${spec#*=}"
	W="$HOME/r7v-w-$name"; O="$J/$name"
	rm -rf "$W"; mkdir -p "$W" "$O"; cp -a "$SRC/." "$W/"
	cp "$SRC/ISOLATED_PROVENANCE" "$O/"
	echo "=== $name: ./configure $flags"
	cd "$W" || continue
	if ! ./scripts/ec2/clean-regen.sh $flags; then
		cp /tmp/cfg.log "$O/configure.log" 2>/dev/null; echo "$name CONFIGURE_FAIL"; continue
	fi
	cp config.log "$O/"
	if ! make -j"$(nproc)" > "$O/build.log" 2>&1; then
		echo "$name BUILD_FAIL"; tail -n 30 "$O/build.log"; continue
	fi
	make check -j"$(nproc)" > "$O/check.log" 2>&1; echo "$name make_check_rc=$?"
	cp test-suite.log "$O/" 2>/dev/null
	grep -E '^# (TOTAL|PASS|SKIP|XFAIL|FAIL|XPASS|ERROR):' "$O/check.log" | tee "$O/totals.txt"
	# Every non-passing test: its own .log and .trs.
	for t in $(grep -lE ':test-result: (FAIL|XPASS|ERROR)' $(find . -name '*.trs')); do
		cp --parents "${t%.trs}.log" "$t" "$O/"; echo "$name NONPASS $t"
	done
	# automake's test-driver appends "<RES> <test> (exit status: N)" to each .log.
	for b in test_inspect_e2e test_tool_T9_leak_stack; do
		for trs in $(find test -name "$b*.trs"); do
			cp --parents "${trs%.trs}.log" "$trs" "$O/"
			echo "$name $b: $(grep ':test-result:' "$trs") | $(grep 'exit status' "${trs%.trs}.log")"
		done
	done
	cd "$SRC"
done
echo "=== r7v_run done"
