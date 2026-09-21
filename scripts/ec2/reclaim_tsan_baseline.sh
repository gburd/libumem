#!/usr/bin/env bash
# TSAN pre-fix baseline for P1.5c.  Runs against the PRE-FIX umem.c with the
# POST-FIX test driver, which is the only combination that can show the
# slab_state race: the pre-fix driver deadlocks (P1.5c reap re-entry), and the
# pre-fix umem_slab_reclaim is what publishes slab_state unlocked.
set -u
./scripts/ec2/clean-regen.sh >/dev/null 2>&1
python3 - <<'PY'
import re
s=open('umem.c').read()
o=s
# 1. Restore the unlocked publication in umem_slab_reclaim (both paths).
s=s.replace("""	if (umem_slab_keeps_metadata(cp))
		return;
""","",1)
s=s.replace("""	if (end <= start)
		return;
""","""	if (end <= start) {
		sp->slab_state = SLAB_CLEAN;
		return;
	}
""",1)
s=s.replace("""	(void) madvise(reclaim_base, reclaim_size, MADV_DONTNEED);
#endif
}""","""	(void) madvise(reclaim_base, reclaim_size, MADV_DONTNEED);
#endif
	sp->slab_state = SLAB_CLEAN;
}""",1)
# 2. Remove the locked publication loop added by the fix.
s=re.sub(r"\n\twhile \(reclaim_list != NULL\) \{\n\t\tsp = reclaim_list;\n\t\treclaim_list = sp->slab_reclaim_next;\n\t\tASSERT\(sp->slab_state == SLAB_RECLAIMING\);\n\t\tASSERT\(sp->slab_refcnt == 0\);\n\t\tsp->slab_state = SLAB_CLEAN;\n\t\}\n", "\n", s, count=1)
assert s!=o, "no pre-fix reversal applied -- baseline would be meaningless"
assert "sp->slab_state = SLAB_CLEAN;" in s
open('umem.c','w').write(s)
PY
echo "=== reverted P1.5a/c in umem.c (P1.5c TSAN baseline); keeping the umem_reap guard so it does not deadlock ==="
grep -n "slab_state = SLAB_CLEAN" umem.c
./configure --enable-tsan >/dev/null 2>&1 && make -j"$(nproc)" >/dev/null 2>&1 && echo "TSAN BUILD OK (pre-fix reclaim)"
TSAN_LIB="$(ls /usr/lib/gcc/*/*/libtsan.so.0.0.0 2>/dev/null | head -1)"
export LD_PRELOAD="$TSAN_LIB" LD_LIBRARY_PATH=.libs
export TSAN_OPTIONS=halt_on_error=0:exitcode=0:history_size=4
echo "--- tsan race, PRE-FIX slab_state publication ---"
UMEM_OPTIONS=reclaim=1,reclaim_delay=0,reap_interval=1 timeout 600 ./test/unit/.libs/repro_reclaim_reuse race > /tmp/base.log 2>&1
echo "rc=$?"
echo "tsan_warnings=$(grep -c 'WARNING: ThreadSanitizer' /tmp/base.log)"
echo "slab_state race reports: $(grep -c 'umem_slab_reclaim' /tmp/base.log)"
echo "--- reports naming umem_slab_reclaim / slab_alloc / slab_free ---"
grep -nE 'umem_slab_reclaim|umem_slab_alloc|umem_slab_free|umem_cache_reclaim_pages' /tmp/base.log | head -20
echo "--- first report mentioning umem_slab_reclaim ---"
awk '/WARNING: ThreadSanitizer/{buf=""} {buf=buf"\n"$0} /^$/{if (buf ~ /umem_slab_reclaim/) {print buf; exit}}' /tmp/base.log | head -30
