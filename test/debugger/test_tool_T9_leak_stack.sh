#!/usr/bin/env bash
#
# T9 of docs/reviews/2026-09-26-tool-review.md: under UMEM_DEBUG=audit on
# x86_64 every audit stack is one frame deep and that frame is inside
# libumem, so `umemctl leaks` cannot name a caller; and `learn` records the
# cache bufsize (224) where `break leaked` compares the request size (200),
# so a learned set never matches tool_target's umem_alloc(200) leak.
#
# Fixed iff (a) at least one leak record's FIRST pc lies in the target
# executable (the caller, not the allocator), and (b) the learned set has a
# `sig size=200 ` line.  Control: >= 1 leak record and a libumem mapping.
# Exit: 0 fixed, 1 finding reproduces, 77 no introspect or x86_64 without
# --enable-frame-pointers, 99 control failed.
set -u
. "$(dirname "$0")/tool_common.sh"
skip_unless_introspect
# x86_64 walks %rbp only: a default build records one frame by design (opt-in
# --enable-frame-pointers, configure.ac).  aarch64 keeps frame records at -O2.
if [[ $(uname -m) == x86_64 ]] &&
    ! grep -q '^#define UMEM_HAVE_FRAME_POINTERS 1' "$ROOT/config.h"; then
	echo "SKIP: x86_64 libumem built without --enable-frame-pointers"
	exit 77
fi

CTL=$ROOT/tools/umemctl
export UMEM_INTROSPECT_SOCK=$TMPD/umem.sock
UMEM_OPTIONS=introspect=1 UMEM_DEBUG=audit UMEM_LOGGING=transaction=1m \
    start_target "$TMPD/info" 3 120
sleep 2
leaks=
for _ in $(seq 1 50); do
	leaks=$("$CTL" "$TPID" leaks 2>&1) && [[ $leaks == *'leak addr='* ]] && break
	sleep 0.1
done
nrec=$(grep -c '^leak addr=' <<<"$leaks")
(( nrec > 0 )) || { echo "ERROR: control: no leak records"; echo "$leaks"; exit 99; }

exe=$(readlink "/proc/$TPID/exe")
ranges() { awk -v p="$1" '$6 ~ p { split($1, r, "-"); print r[1], r[2] }' "/proc/$TPID/maps"; }
mapfile -t exe_r < <(ranges "^${exe}\$")
mapfile -t lib_r < <(ranges 'libumem\\.so')
(( ${#lib_r[@]} > 0 && ${#exe_r[@]} > 0 )) ||
	{ echo "ERROR: control: no libumem/exe mapping for $exe"; exit 99; }
inside() {  # inside <pc> <range>...
	local pc=$((16#${1#0x})) r lo hi; shift
	for r in "$@"; do
		read -r lo hi <<<"$r"
		(( pc >= 16#$lo && pc < 16#$hi )) && return 0
	done
	return 1
}
firsts=$(awk '/^leak addr=/ { want = 1; next } want && /^  0x/ { print $1; want = 0 }' <<<"$leaks")
n_exe=0 n_lib=0
for pc in $firsts; do
	inside "$pc" "${exe_r[@]}" && n_exe=$((n_exe + 1))
	inside "$pc" "${lib_r[@]}" && n_lib=$((n_lib + 1))
done
echo "leak records $nrec: first pc in exe $n_exe, in libumem $n_lib"
grep '^leak addr=' <<<"$leaks" | awk '{print $NF}' | sort | uniq -c | head -5

"$CTL" "$TPID" record --learn-leaks "$TMPD/set" >/dev/null 2>&1
n200=$(grep -c '^sig size=200 ' "$TMPD/set" 2>/dev/null)
echo "learned: $(grep -c '^sig ' "$TMPD/set" 2>/dev/null) sigs, size=200: ${n200:-0}"

fail=0
(( n_exe > 0 )) || { echo "T9 REPRODUCES: no leak record names a caller outside libumem"; fail=1; }
(( ${n200:-0} > 0 )) || { echo "T9 REPRODUCES: learned set has no request-size 200 signature"; fail=1; }
(( fail )) && exit 1
echo "T9 FIXED"
exit 0
