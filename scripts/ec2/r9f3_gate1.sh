#!/usr/bin/env bash
# r9f3 gate 1 re-check: objdump of fast-path symbols, normalizing RIP-relative
# displacements and call/jump targets (which shift when unrelated globals are
# added) so a TRUE opcode/operation change still shows but link drift does not.
set -e
BASE=~/libumem-ab-r9f3b
PRESO=$(ls "$BASE"/pre/.libs/libumem.so.*.*.* | head -1)
POSTSO=$(ls "$BASE"/post/.libs/libumem.so.*.*.* | head -1)
SYMS="_umem_alloc _umem_free _umem_cache_alloc _umem_cache_free"
# Normalize: drop address col; mask RIP-rel disp hex (0x....(%rip)); mask the
# "# <addr> <sym>" comment; mask absolute call/jmp targets; mask the raw byte
# column (bytes encode the shifted displacement). Keep mnemonic + register ops.
norm() {
  objdump -d --disassemble="$1" "$2" 2>/dev/null \
    | sed '1,/>:/d' \
    | sed -E 's/^[[:space:]]*[0-9a-f]+:\t//' \
    | sed -E 's/^([0-9a-f]{2} )+\t//' \
    | sed -E 's/0x[0-9a-f]+\(%rip\)/0xDISP(%rip)/g' \
    | sed -E 's/#.*$//' \
    | sed -E 's/\b(call|jmp|je|jne|jg|jl|jle|jge|ja|jb|jae|jbe|js|jns)[[:space:]]+[0-9a-f]+/\1 TARGET/g' \
    | sed -E 's/\$0x[0-9a-f]+/\$0xIMM/g' \
    | sed -E 's/[[:space:]]+/ /g; s/[[:space:]]+$//'
}
rc=0
for s in $SYMS; do
  norm "$s" "$PRESO"  > /tmp/n_pre_$s.txt
  norm "$s" "$POSTSO" > /tmp/n_post_$s.txt
  if diff -q /tmp/n_pre_$s.txt /tmp/n_post_$s.txt >/dev/null; then
    echo "  $s: IDENTICAL after reloc-normalization ($(wc -l < /tmp/n_pre_$s.txt) insns)"
  else
    echo "  $s: DIFFERS after reloc-normalization"
    diff /tmp/n_pre_$s.txt /tmp/n_post_$s.txt | head -60
    rc=1
  fi
done
echo "GATE1_NORM_RC=$rc"
# Also: count that instruction COUNT is identical per symbol (a new branch
# would add lines even after normalization).
echo "=== instruction counts (pre vs post) ==="
for s in $SYMS; do
  echo "  $s: pre=$(objdump -d --disassemble="$s" "$PRESO" 2>/dev/null | grep -cE '^[[:space:]]*[0-9a-f]+:') post=$(objdump -d --disassemble="$s" "$POSTSO" 2>/dev/null | grep -cE '^[[:space:]]*[0-9a-f]+:')"
done
