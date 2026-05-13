#!/usr/bin/env bash
#
# End-to-end test for the lldb integration.  Mirrors test_inspect_e2e.sh
# but exercises tools/lldb/umem_lldb.py via `lldb --batch`.
set -eu
# Don't use pipefail: lldb --batch exits non-zero after `process detach`
# even on success, which would cancel the awk pipeline.

cd "$(dirname "$0")/../.."
ROOT=$(pwd)

if ! command -v lldb >/dev/null 2>&1; then
	echo "SKIP: lldb not installed"
	exit 0
fi
if [[ ! -x $ROOT/test/test_inspect_live ]]; then
	echo "FAIL: test/test_inspect_live not built"
	exit 1
fi

PIDFILE=$(mktemp)
LOGFILE=$(mktemp)
cleanup() {
	kill -9 "${TPID:-0}" "${SH_PID:-0}" 2>/dev/null || true
	rm -f "$PIDFILE" "$LOGFILE"
	wait 2>/dev/null || true
}
trap cleanup EXIT

UMEM_DEBUG=audit UMEM_LOGGING=transaction=1m \
    LD_LIBRARY_PATH="$ROOT/.libs" \
    "$ROOT/test/test_inspect_live" "$PIDFILE" >/dev/null &
SH_PID=$!

for _ in $(seq 1 30); do
	[ -s "$PIDFILE" ] && break
	sleep 0.1
done
[[ -s $PIDFILE ]] || { echo "FAIL: no pidfile"; exit 1; }
TPID=$(cat "$PIDFILE")
echo "test pid: $TPID"
sleep 0.3

echo "[1/3] lldb umem findleaks"
attempt=0
while (( attempt < 3 )); do
	attempt=$(( attempt + 1 ))
	out=$(lldb --batch \
		-o "command script import $ROOT/tools/lldb/umem_lldb.py" \
		-o "process attach -p $TPID" \
		-o "umem findleaks -f json -n 20" \
		-o "process detach" 2>"$LOGFILE" |
		awk '/^\{"version":/ { print; exit }')
	if [[ -n $out ]]; then break; fi
	sleep 0.5
done
if [[ -z $out ]]; then
	echo "FAIL: lldb returned no JSON after $attempt attempts"
	cat "$LOGFILE" >&2
	exit 1
fi
printf '%s' "$out" | python3 -c '
import json, sys
d = json.loads(sys.stdin.read())
assert d["total_buffers"] == 12, "buffer count wrong"
assert d["total_bytes"] == 8832, "byte count wrong"
# cached count is normally 2 from the churn loop, but PTC interaction
# under heavy CI load can vary slightly; allow 0..4.
assert 0 <= d["cached_skipped"] <= 4, "cached count out of range"
print("  OK:", d["total_buffers"], "buffers,", d["cached_skipped"], "cached")
'

echo "[2/3] lldb umem status"
lldb --batch \
	-o "command script import $ROOT/tools/lldb/umem_lldb.py" \
	-o "process attach -p $TPID" \
	-o "umem status -f json" \
	-o "process detach" 2>"$LOGFILE" |
	awk '/^\{"caches":/ { print; exit }' |
	python3 -c '
import json, sys
data = sys.stdin.read().strip()
d = json.loads(data)
assert len(d["caches"]) > 30, "too few caches"
print("  OK:", len(d["caches"]), "caches")
'

echo "[3/3] lldb umem walk"
lldb --batch \
	-o "command script import $ROOT/tools/lldb/umem_lldb.py" \
	-o "process attach -p $TPID" \
	-o "umem walk allocated -f json -n 50" \
	-o "process detach" 2>"$LOGFILE" |
	awk '/^\{"entries":/ { print; exit }' |
	python3 -c '
import json, sys
data = sys.stdin.read().strip()
d = json.loads(data)
assert len(d["entries"]) > 0, "walk returned no entries"
print("  OK:", len(d["entries"]), "buffers walked")
'

echo ""
echo "ALL 3 LLDB TESTS PASSED"
