#!/bin/bash
# @r7h5 A->C: A=master, C=B+T1/T8, in ~/ab3 (ship_ab_arms A C), then the
# hot/.cold partition of the four entry points in A and C.
#   $1=A $2=C
set +e
A=$1 C=$2; O=~/r7h5
echo "== box: $(grep PRETTY /etc/os-release) $(uname -m) gcc=$(gcc -dumpversion) $(cat /sys/devices/virtual/dmi/id/product_name 2>/dev/null)"
echo "== A->C"; (cd ~/ab3 && OUTDIR=$O/ab3 PRE=$A POST=$C ./post/scripts/ec2/hotpath_ab.sh >/dev/null 2>&1); cat $O/ab3/hotpath-ab.txt
dis() { objdump -d --no-show-raw-insn "$1" | awk -v f="<$2>:" '$2==f{p=1;next} p&&/^$/{exit} p'; }
echo "== partition: insns per symbol, calls/jumps to hook symbols, which part holds them"
for t in A:ab3/pre C:ab3/post; do
	so=$(ls ~/${t#*:}/.libs/libumem.so.*.*.* | head -1)
	for f in _umem_alloc _umem_free _umem_cache_alloc _umem_cache_free; do
		for s in $f $f.cold; do
			n=$(dis "$so" $s | wc -l); [ "$n" -eq 0 ] && { echo "${t%%:*} $s absent"; continue; }
			h=$(dis "$so" $s | grep -oE "<(umem_hook_[a-z_]+|umem_[a-z_]*hooked[a-z_]*|umem_cache_alloc_debug|umem_cache_free_debug|umem_alloc_retry|umem_slab_alloc)[^>]*>" | sort | uniq -c | tr '\n' ' ')
			echo "${t%%:*} $s insns=$n refs: $h"
		done
	done
done
echo "ALL_DONE"
