#!/bin/bash
# @r7h5 driver, run by verify-isolated.sh in the isolated tree of C.
#   $1=A (master) $2=B (A + UMEM_HOT_ALIGN) $3=C (B + T1/T8)
# ~/ab1 = ship_ab_arms A B, ~/ab2 = ship_ab_arms B C.  Everything is
# sequential on one box so nothing else competes with the A/B.
set +e
A=$1 B=$2 C=$3; ISO=$PWD; O=~/r7h5
rm -r -f "$O"; mkdir -p "$O"
sudo sysctl -w kernel.yama.ptrace_scope=0 >/dev/null 2>&1
echo "== box: $(grep PRETTY /etc/os-release) $(uname -m) gcc=$(gcc -dumpversion) $(cat /sys/devices/virtual/dmi/id/product_name 2>/dev/null)"
echo "== A->B"; (cd ~/ab1 && OUTDIR=$O/ab1 PRE=$A POST=$B ./post/scripts/ec2/hotpath_ab.sh >/dev/null 2>&1); cat $O/ab1/hotpath-ab.txt
echo "== B->C"; (cd ~/ab2 && OUTDIR=$O/ab2 PRE=$B POST=$C ./post/scripts/ec2/hotpath_ab.sh >/dev/null 2>&1); cat $O/ab2/hotpath-ab.txt
echo "== alignment: nm address mod 64, objdump symbol line"
for t in ab1/pre:A ab1/post:B ab2/post:C; do
	so=$(ls ~/${t%%:*}/.libs/libumem.so.*.*.* | head -1)
	for f in _umem_alloc _umem_free _umem_cache_alloc _umem_cache_free; do
		a=$(nm "$so" | awk -v f=$f '$3==f{print $1;exit}')
		echo "${t#*:} $f 0x$a mod64=$((16#$a % 64)) | $(objdump -d "$so" | grep -m1 "<$f>:")"
	done
done
cd "$ISO" || exit 1
rm -r -f ../r7h5-def ../r7h5-p85; mkdir ../r7h5-def ../r7h5-p85
cp -a . ../r7h5-def/; cp -a . ../r7h5-p85/
chk() { # $1 label, rest = configure flags
	local l=$1; shift
	echo "== gate: $l make check"
	{ ./scripts/ec2/clean-regen.sh "$@" >regen.log 2>&1 && make -j$(nproc) >mk.log 2>&1; } \
		|| { echo "$l BUILD FAIL"; tail -20 mk.log; return; }
	make -j$(nproc) check >chk.log 2>&1; echo "$l CHECK rc=$?"
	grep -E "^# (TOTAL|PASS|SKIP|XFAIL|FAIL|XPASS|ERROR)" test-suite.log
	grep -E "^[A-Z]+: test/debugger/test_tool_T(1|8)_" chk.log
	grep -E "^(FAIL|XPASS|ERROR|XFAIL):" chk.log
	grep -E "^SKIP:" chk.log
}
chk introspect --enable-introspect
cd ../r7h5-def && chk default
echo "== gate: rseq repros + 8t oracle (p85b_fix_lo_gate.sh 30)"
cd ../r7h5-p85 && ./scripts/ec2/p85b_fix_lo_gate.sh 30
echo "ALL_DONE"
