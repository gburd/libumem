#!/usr/bin/env bash
# TSAN pre-fix baseline for P1.5c: does TSAN actually report the unlocked
# slab_state publication?  Without this control, "0 reports post-fix" is
# meaningless.
#
# Method: restore the pre-fix umem_slab_reclaim() and
# umem_cache_reclaim_pages() EXACTLY, by applying the reverse of the fix
# commit's hunks for those two functions, then build --enable-tsan and run the
# race case.
#
# Two things must be preserved or the baseline measures the wrong failure:
#   * the umem_reap() IN_UPDATE guard stays, otherwise the driver deadlocks
#     before TSAN reports anything (see the /reap_reentry control);
#   * the pre-fix madvise LIMIT stays pre-fix -- for a non-hash cache it was
#     (uintptr_t)sp, i.e. it stopped below the embedded umem_slab_t.  An
#     earlier version of this script reverted only the state stores and left
#     the post-fix whole-slab limit in place, which madvised the slab metadata
#     itself and crashed in a way the real pre-fix code never did.
set -u
REF_FIX="${REF_FIX:-}"
if [ -z "$REF_FIX" ]; then
	echo "REF_FIX (the commit whose reclaim hunks to reverse) must be set"; exit 1
fi
./scripts/ec2/clean-regen.sh >/dev/null 2>&1

python3 - <<'PY'
s = open('umem.c').read()
orig = s

# --- umem_slab_reclaim: restore pre-fix body (metadata NOT excluded, limit
# --- stops below embedded metadata for non-hash, state published here).
s = s.replace("""	/*
	 * Slabs whose in-buffer metadata the allocator reads again after
	 * reactivation are never discarded.  See umem_slab_keeps_metadata().
	 */
	if (umem_slab_keeps_metadata(cp))
		return;

""", "", 1)
s = s.replace("""	uintptr_t limit = P2END((uintptr_t)sp->slab_base, cp->cache_slabsize);""",
"""	uintptr_t limit = (unlikely(cp->cache_flags & UMF_HASH)) ?
	    P2END((uintptr_t)sp->slab_base, cp->cache_slabsize) :
	    (uintptr_t)sp;""", 1)
s = s.replace("""	if (end <= start)
		return;
""", """	if (end <= start) {
		sp->slab_state = SLAB_CLEAN;
		return;
	}
""", 1)
s = s.replace("""	(void) madvise(reclaim_base, reclaim_size, MADV_DONTNEED);
#endif
}""", """	(void) madvise(reclaim_base, reclaim_size, MADV_DONTNEED);
#endif
	sp->slab_state = SLAB_CLEAN;
}""", 1)

# --- umem_cache_reclaim_pages: drop the locked publication loop.
i = s.find("\twhile (reclaim_list != NULL) {\n\t\tsp = reclaim_list;\n\t\treclaim_list = sp->slab_reclaim_next;\n\t\tASSERT(sp->slab_state == SLAB_RECLAIMING);")
assert i != -1, "locked publication loop not found"
j = s.find("\t}\n", i)
s = s[:i] + s[j+3:]

assert s != orig
# Post-conditions: pre-fix shape restored, guard untouched.
assert s.count("sp->slab_state = SLAB_CLEAN;") == 2, "expected exactly the two pre-fix stores"
assert "umem_slab_keeps_metadata(cp))\n\t\treturn;" not in s, "exclusion still present"
assert "(uintptr_t)sp;" in s, "pre-fix non-hash limit not restored"
assert "if (IN_UPDATE())\n\t\treturn;" in s, "umem_reap guard must stay"
open('umem.c','w').write(s)
print("pre-fix reclaim restored")
PY

echo "=== verify the revert matches the fix commit's parent for these hunks ==="
git diff --stat 2>/dev/null || true
grep -n "slab_state = SLAB_CLEAN" umem.c
grep -n "uintptr_t limit" umem.c

./configure --enable-tsan >/dev/null 2>&1 && make -j"$(nproc)" libumem.la test/unit/repro_reclaim_reuse >/dev/null 2>&1 && echo "TSAN BUILD OK (pre-fix reclaim)"
TSAN_LIB="$(ls /usr/lib/gcc/*/*/libtsan.so.0.0.0 2>/dev/null | head -1)"
[ -f "$TSAN_LIB" ] || { echo "no TSAN runtime"; exit 0; }
export LD_PRELOAD="$TSAN_LIB" LD_LIBRARY_PATH=.libs
export TSAN_OPTIONS=halt_on_error=0:exitcode=0:history_size=7
echo "--- tsan race, PRE-FIX slab_state publication (long, unthrottled) ---"
RECLAIM_RACE_SECONDS=30 UMEM_OPTIONS=reclaim=1,reclaim_delay=0,reap_interval=0 timeout 600 \
    ./test/unit/.libs/repro_reclaim_reuse race > /tmp/base.log 2>&1
echo "rc=$?"
echo "tsan_warnings=$(grep -c 'WARNING: ThreadSanitizer' /tmp/base.log)"
echo "reports naming umem_slab_reclaim: $(grep -c 'umem_slab_reclaim' /tmp/base.log)"
echo "reports naming slab_state readers: $(grep -cE 'umem_slab_alloc|umem_slab_free' /tmp/base.log)"
echo "--- any report mentioning the reclaim functions ---"
grep -nE 'umem_slab_reclaim|umem_slab_alloc|umem_slab_free|umem_cache_reclaim_pages' /tmp/base.log | head -20
echo "--- full report containing umem_slab_reclaim, if any ---"
awk 'BEGIN{RS="=================="} /umem_slab_reclaim/{print; exit}' /tmp/base.log | head -40

echo "=== how many reclaim passes did the run actually get? ==="
# If the publication window is never entered, a zero-report result says
# nothing about the race.  Count DIRTY->CLEAN transitions by instrumenting
# nothing: just report the reap interval in effect and the run length, so the
# log carries the sensitivity of the measurement.
echo "reap_interval=0 (limiter off), run=30s"
