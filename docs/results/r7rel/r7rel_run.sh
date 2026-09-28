#!/usr/bin/env bash
# r7rel_run.sh <job> <v332-src-dir> [fp]
# The @r7rel qualification of a release candidate.  Run from the
# verify-isolated.sh directory (a pristine `git archive` of the ref).  Each step
# works in its own copy of that pristine tree, one at a time on the box, and its
# evidence lands in ~/libumem-jobs/<job>/<step>/ so `job.sh fetch` picks it up.
# Does not stop at a failing step.  Pass "fp" to add the x86-only
# --enable-introspect --enable-frame-pointers config.
# Order matters: p85b_fix_lo_gate.sh sets vm.max_map_count=65530, so it runs last.
set -u
SRC="$PWD"; J="$HOME/libumem-jobs/$1"; OLD="$2"; FP="${3:-}"
cat "$SRC/ISOLATED_PROVENANCE"; uname -a; nproc; gcc --version | head -n 1
grep PRETTY_NAME /etc/os-release
TMO='timeout 900'   # per-test deadline for the make checks this script drives

fresh() { # step -> cd into a fresh copy of the pristine tree; O=evidence dir
	W="$HOME/r7rel-w-$1"; O="$J/$1"
	rm -rf "$W"; mkdir -p "$W" "$O"; cp -a "$SRC/." "$W/"
	cp "$SRC/ISOLATED_PROVENANCE" "$O/"; cd "$W" || exit 1
}
tools() { # name: T1/T8/T9 tool-test results (test-driver's "exit status")
	for trs in $(find test -name 'test_tool_T[189]_*.trs'); do
		cp --parents "${trs%.trs}.log" "$trs" "$O/"
		echo "$1 $(basename "${trs%.trs}"): $(grep ':test-result:' "$trs") | $(grep 'exit status' "${trs%.trs}.log")"
	done
}
nonpass() { # name
	for t in $(grep -lE ':test-result: (FAIL|XPASS|ERROR)' $(find . -name '*.trs')); do
		cp --parents "${t%.trs}.log" "$t" "$O/"; echo "$1 NONPASS $t"
	done
}
check() { # name flags...: configure, build, make check -j with a per-test timeout
	local n="$1"; shift; fresh "$n"
	echo "=== $n: ./configure $*"
	./scripts/ec2/clean-regen.sh "$@" >"$O/regen.log" 2>&1 || { echo "$n CONFIGURE_FAIL"; cd "$SRC"; return; }
	cp config.log "$O/"
	make -j"$(nproc)" >"$O/build.log" 2>&1 || { echo "$n BUILD_FAIL"; tail -n 30 "$O/build.log"; cd "$SRC"; return; }
	make check -j"$(nproc)" LOG_COMPILER="$TMO" SH_LOG_COMPILER="$TMO" >"$O/check.log" 2>&1
	echo "$n make_check_rc=$?"; cp test-suite.log "$O/" 2>/dev/null
	grep -E '^# (TOTAL|PASS|SKIP|XFAIL|FAIL|XPASS|ERROR):' "$O/check.log" | sed "s/^/$n /"
	nonpass "$n"; tools "$n"; cd "$SRC"
}

echo "=== 1. exit_criteria_gate"
fresh gate
bash scripts/ec2/exit_criteria_gate.sh >"$O/gate.log" 2>&1; echo "gate_rc=$?"
grep -E '^=== |rc=|^# |GATE|tarball=|^(FAIL|ERROR):|RESULT' "$O/gate.log"
cp /tmp/ck.log /tmp/ck2.log /tmp/tbk.log /tmp/tm.log /tmp/tm2.log /tmp/or.log /tmp/or2.log /tmp/ir.log "$O/" 2>/dev/null
cp test-suite.log "$O/test-suite-ndebug.log" 2>/dev/null
nonpass gate; cd "$SRC"

check introspect --enable-introspect
[ "$FP" = fp ] && check introspect-fp --enable-introspect --enable-frame-pointers
check norseq --disable-rseq

echo "=== 4. ABI (default config, installed headers) vs v3.3.2"
sudo apt-get install -y abigail-tools >/dev/null 2>&1 || echo "abigail-tools not installed"
abi() { # name srcdir
	local n="$1" P="/tmp/r7rel-abi-$1"; rm -rf "$HOME/r7rel-w-abi-$n"; cp -a "$2" "$HOME/r7rel-w-abi-$n"
	cd "$HOME/r7rel-w-abi-$n" || return
	./scripts/ec2/clean-regen.sh >/dev/null 2>&1 && make -j"$(nproc)" >/dev/null 2>&1 \
		&& rm -rf "$P" && make install DESTDIR="$P" >/dev/null 2>&1 || { echo "$n ABI_BUILD_FAIL"; cd "$SRC"; return; }
	INC=$(find "$P" -name umem_hooks.h -printf '%h\n' | head -n 1)
	printf '#include <stdio.h>\n#include <umem.h>\n#include <umem_hooks.h>\nint main(void){printf("%%zu\\n",sizeof(umem_hook_t));return 0;}\n' >/tmp/sz.c
	gcc -I"$INC" /tmp/sz.c -o /tmp/sz && echo "$n sizeof(umem_hook_t)=$(/tmp/sz)"
	for so in libumem.so libumem_malloc.so; do
		L=$(readlink -f "$(find "$P" -name "$so" | head -n 1)")
		echo "$n $so -> $(basename "$L") $(readelf -d "$L" | grep SONAME | sed 's/.*\[/[/')"
		nm -D --defined-only "$L" | awk '{print $NF}' | sort >"$O/exports-$n-$so.txt"
		echo "$n $so defined_exports=$(wc -l <"$O/exports-$n-$so.txt")"
	done
	cd "$SRC"
}
fresh abi; cd "$SRC"
abi new "$SRC"; abi v332 "$OLD"
for so in libumem.so libumem_malloc.so; do
	echo "exports $so diff (v332 -> new):"; diff "$O/exports-v332-$so.txt" "$O/exports-new-$so.txt" && echo "  identical"
	if command -v abidiff >/dev/null; then
		abidiff "$(readlink -f "$(find /tmp/r7rel-abi-v332 -name "$so" | head -n 1)")" \
		        "$(readlink -f "$(find /tmp/r7rel-abi-new -name "$so" | head -n 1)")" >"$O/abidiff-$so.txt" 2>&1
		echo "abidiff $so rc=$?"; head -n 20 "$O/abidiff-$so.txt"
	fi
done

echo "=== 5. P8.5b regressions (p85b_fix_lo_gate.sh 30)"
fresh p85b
bash scripts/ec2/p85b_fix_lo_gate.sh 30 >"$O/p85b.log" 2>&1; echo "p85b_rc=$?"
grep -E '^GATE|^# |^SKIP' "$O/p85b.log"
cp test-suite.log gate-*.log "$O/" 2>/dev/null; cd "$SRC"
echo "=== r7rel_run done"
