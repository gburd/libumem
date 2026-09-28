#!/bin/bash
# r7h4 step 1: build f40a2da / dc4f29c / ae9af7e and count insns per hot function
set +e
O=~/r7h4; mkdir -p $O
bld() { ( cd "$1" && ./scripts/ec2/clean-regen.sh >/dev/null && make -j$(nproc) >/dev/null 2>&1 ) || echo "BUILD FAIL $1"; cp "$(ls $1/.libs/libumem.so.*.*.* | head -1)" $O/lib_$2.so; }
bld ~/ab-main/pre pre; bld ~/ab-dc/post dc; bld ~/ab-main/post post
FN="_umem_cache_alloc _umem_cache_alloc.cold _umem_cache_free _umem_cache_free.cold _umem_alloc _umem_alloc.cold _umem_free _umem_free.cold umem_cache_alloc_tail umem_alloc_hooked umem_free_hooked"
dis() { objdump -d --no-show-raw-insn "$1" | awk -v f="<$2>:" '$2==f{p=1;next} p&&/^$/{exit} p'; }
{
printf "%-24s %8s %8s %8s   (insns; start addr mod 64)\n" fn f40a2da dc4f29c ae9af7e
for f in $FN; do
  printf "%-24s" $f
  for t in pre dc post; do n=$(dis $O/lib_$t.so $f | wc -l); a=$(nm $O/lib_$t.so | awk -v f=$f '$3==f{print $1; exit}'); [ -n "$a" ] && m=$((16#$a % 64)) || m=-; printf " %5s@%-3s" $n $m; done; echo
done
} | tee $O/insn-counts.txt
for t in pre dc post; do for f in _umem_cache_alloc _umem_cache_free _umem_alloc _umem_free; do
  dis $O/lib_$t.so $f | sed -E 's/^ *[0-9a-f]+:\s*//; s/[0-9a-f]{4,} <([^>+]+)(\+0x[0-9a-f]+)?>/<\1>/; s/0x[0-9a-f]+\(%rip\)/X(%rip)/' > $O/n_${t}_$f.s; done; done
for f in _umem_cache_alloc _umem_cache_free _umem_alloc _umem_free; do
  echo "=== $f pre->dc"; diff $O/n_pre_$f.s $O/n_dc_$f.s | head -60
  echo "=== $f pre->post"; diff $O/n_pre_$f.s $O/n_post_$f.s | head -80
done > $O/disasm-diff.txt
nm -S --size-sort $O/lib_pre.so | tail -3 >/dev/null
echo DONE
