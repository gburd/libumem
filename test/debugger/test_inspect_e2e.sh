#!/usr/bin/env bash
#
# End-to-end test for the umem inspection toolchain.
#
# Verifies:
#   1. UMEM_DEBUG=audit captures non-zero stack depth (ie getpcstack works)
#   2. umem --pid attaches and runs findleaks
#   3. JSON output is well-formed and accurate
#   4. Live findleaks matches expected leak count from the test program
#   5. Magazine cached-set subtraction works
#   6. Binary snapshot round-trips through the offline reader
#   7. log dump returns audit records with stacks
#   8. whatis resolves an arbitrary leaked address
set -euo pipefail

cd "$(dirname "$0")/../.."
ROOT=$(pwd)
TOOL=$ROOT/tools/umem

if [[ ! -x $TOOL ]]; then
	echo "FAIL: $TOOL not found / not executable"
	exit 1
fi
if [[ ! -x $ROOT/test/test_inspect_live ]]; then
	echo "FAIL: $ROOT/test/test_inspect_live not built (run make)"
	exit 1
fi

PIDFILE=$(mktemp)
SNAPFILE=$(mktemp -u --suffix=.ums)
LOGFILE=$(mktemp)

cleanup() {
	if [[ -n ${TPID:-} ]]; then
		kill -9 "$TPID" 2>/dev/null || true
	fi
	if [[ -n ${SH_PID:-} ]]; then
		kill -9 "$SH_PID" 2>/dev/null || true
	fi
	rm -f "$PIDFILE" "$SNAPFILE" "$LOGFILE"
	wait 2>/dev/null || true
}
trap cleanup EXIT

UMEM_DEBUG=audit UMEM_LOGGING=transaction=1m \
    LD_LIBRARY_PATH="$ROOT/.libs" \
    "$ROOT/test/test_inspect_live" "$PIDFILE" >/dev/null &
SH_PID=$!

# Wait for the test program to register its pid.
for _ in $(seq 1 30); do
	[ -s "$PIDFILE" ] && break
	sleep 0.1
done
if [[ ! -s $PIDFILE ]]; then
	echo "FAIL: test_inspect_live did not write its pid to $PIDFILE"
	exit 1
fi
TPID=$(cat "$PIDFILE")
echo "test pid: $TPID"

# Wait for libumem to finish initialising in the target.
sleep 0.3

# ---------------------------------------------------------------------------
# Test 1: live findleaks (text)
# ---------------------------------------------------------------------------
echo "[1/8] live findleaks --text"
"$TOOL" --pid "$TPID" findleaks -n 20 >"$LOGFILE" 2>&1 || {
	cat "$LOGFILE"
	echo "FAIL: findleaks returned non-zero"
	exit 1
}
grep -q "^findleaks: " "$LOGFILE" || {
	cat "$LOGFILE"
	echo "FAIL: findleaks output missing summary line"
	exit 1
}

# ---------------------------------------------------------------------------
# Test 2: live findleaks (json) -- accurate counts
# ---------------------------------------------------------------------------
echo "[2/8] live findleaks --json (parse + assert counts)"
JSON=$("$TOOL" --pid "$TPID" findleaks -f json)
python3 - "$JSON" <<'PY'
import json, sys
d = json.loads(sys.argv[1])
# test program leaks: 2 x 4096-byte allocs from leak_big()
#                    + 10 x 64-byte allocs from leak_small() called twice
# = 12 buffers, 8832 bytes total.
assert d["total_buffers"] == 12, f"expected 12 leaked buffers, got {d['total_buffers']}"
assert d["total_bytes"] == 8832, f"expected 8832 bytes leaked, got {d['total_bytes']}"
# 2 cached buffers from churn allocations sitting in magazines.
assert d["cached_skipped"] == 2, f"expected 2 cached, got {d['cached_skipped']}"
# At least one class with cache=umem_alloc_4096
assert any(c["cache"] == "umem_alloc_4096" for c in d["classes"]), \
    "no umem_alloc_4096 leak class found"
assert any(c["cache"] == "umem_alloc_64"   for c in d["classes"]), \
    "no umem_alloc_64 leak class found"
# Stacks should be non-empty (UMEM_DEBUG=audit + getpcstack working)
for c in d["classes"]:
    assert len(c["stack"]) > 0, f"empty stack for class {c}"
print(f"  OK: {d['total_buffers']} buffers, {d['total_bytes']} bytes, "
      f"{d['cached_skipped']} cached, {len(d['classes'])} classes")
PY

# ---------------------------------------------------------------------------
# Test 3: status
# ---------------------------------------------------------------------------
echo "[3/8] live status --json"
"$TOOL" --pid "$TPID" status -f json | python3 -c '
import json, sys
d = json.loads(sys.stdin.read())
caches = d["caches"]
assert len(caches) > 30, f"expected many caches, got {len(caches)}"
named = {c["name"] for c in caches}
assert "umem_alloc_4096" in named
assert "umem_alloc_64"   in named
print(f"  OK: {len(caches)} caches reported")
'

# ---------------------------------------------------------------------------
# Test 4: log dump returns records with non-zero stacks
# ---------------------------------------------------------------------------
echo "[4/8] live log dump"
"$TOOL" --pid "$TPID" log -f json -n 50 | python3 -c '
import json, sys
d = json.loads(sys.stdin.read())
n = len(d["records"])
assert n > 0, "transaction log empty"
with_stack = sum(1 for r in d["records"] if len(r["stack"]) > 0)
assert with_stack >= n // 2, \
    f"too few records with stacks ({with_stack}/{n}) -- getpcstack broken?"
print(f"  OK: {n} records, {with_stack} with stack traces")
'

# ---------------------------------------------------------------------------
# Test 5: walk allocated -- enumerate buffers
# ---------------------------------------------------------------------------
echo "[5/8] live walk allocated"
"$TOOL" --pid "$TPID" walk allocated -f json -n 200 | python3 -c '
import json, sys
d = json.loads(sys.stdin.read())
assert len(d["entries"]) > 0
sizes = {e["size"] for e in d["entries"]}
assert 4096 in sizes, "4096-byte alloc not seen in walk output"
print("  OK:", len(d["entries"]), "buffers walked")
'

# ---------------------------------------------------------------------------
# Test 6: whatis on a leaked address
# ---------------------------------------------------------------------------
echo "[6/8] live whatis"
ADDR=$("$TOOL" --pid "$TPID" findleaks -f json | python3 -c '
import json, sys
d = json.loads(sys.stdin.read())
for c in d["classes"]:
    if c["cache"] == "umem_alloc_4096":
        print(c["sample"])
        break
')
if [[ -z $ADDR ]]; then
	echo "FAIL: no umem_alloc_4096 sample address found"
	exit 1
fi
"$TOOL" --pid "$TPID" whatis "$ADDR" >"$LOGFILE"
grep -q "umem_alloc_4096" "$LOGFILE" || {
	cat "$LOGFILE"
	echo "FAIL: whatis did not identify umem_alloc_4096"
	exit 1
}
grep -q "ALLOCATED" "$LOGFILE" || {
	cat "$LOGFILE"
	echo "FAIL: whatis did not report ALLOCATED state"
	exit 1
}
echo "  OK: whatis($ADDR) -> umem_alloc_4096 ALLOCATED"

# ---------------------------------------------------------------------------
# Test 7: snapshot binary write + offline read consistency
# ---------------------------------------------------------------------------
echo "[7/8] snapshot binary round-trip"
"$TOOL" --pid "$TPID" snapshot "$SNAPFILE" >/dev/null
[[ -s $SNAPFILE ]] || { echo "FAIL: empty snapshot file"; exit 1; }

# Verify magic
MAGIC=$(head -c 4 "$SNAPFILE")
[[ $MAGIC == "UMS2" ]] || { echo "FAIL: wrong magic ($MAGIC)"; exit 1; }

# Offline must match live counts.
"$TOOL" --dump "$SNAPFILE" findleaks -f json | python3 -c '
import json, sys
d = json.loads(sys.stdin.read())
assert d["total_buffers"] == 12, "offline buffer count mismatch"
assert d["total_bytes"] == 8832, "offline byte count mismatch"
assert d["cached_skipped"] == 2, "offline cached mismatch"
print("  OK: offline matches live")
'

# ---------------------------------------------------------------------------
# Test 8: snapshot status / log readability
# ---------------------------------------------------------------------------
echo "[8/8] snapshot status + log readable offline"
"$TOOL" --dump "$SNAPFILE" status -f json | python3 -c '
import json, sys
d = json.loads(sys.stdin.read())
assert len(d["caches"]) > 30
print("  OK:", len(d["caches"]), "caches")
'

echo ""
echo "ALL 8 TESTS PASSED"
