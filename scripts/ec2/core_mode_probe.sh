#!/usr/bin/env bash
# Does `umem --core` actually produce a report, or silently nothing?
#
# umem(1) and README.md advertise core-dump forensics.  tools/gdb/umem_gdb.py
# implements every command by calling fopen()/umem_findleaks() IN THE INFERIOR
# (see _call_library there), and a core file has no process to run them.  This
# probe runs the same command against (a) a live pid and (b) that process's
# core, and compares the output.  A silent success on the core is worse than a
# failure: it looks like "no leaks".
set -x
which gdb || { echo "NO_GDB"; exit 0; }

cat > /tmp/holder.c <<'EOF'
#include <umem.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv) {
	for (int i = 0; i < 200; i++) (void) umem_alloc(128, UMEM_DEFAULT);
	if (argc > 1) { abort(); }		/* core mode */
	printf("ready %d\n", (int)getpid());
	fflush(stdout);
	sleep(300);				/* live mode */
	return 0;
}
EOF
gcc -I. -o /tmp/holder /tmp/holder.c -L.libs -lumem -lpthread -lm || exit 1

UMEM_BIN=tools/umem
[ -x tools/.libs/umem ] && UMEM_BIN=tools/.libs/umem
[ -x "$UMEM_BIN" ] || { echo NO_UMEM_BIN; exit 0; }

# --- (a) live pid control ---------------------------------------------------
LD_LIBRARY_PATH=.libs UMEM_DEBUG=audit /tmp/holder &
HPID=$!
sleep 3
set +e
LD_LIBRARY_PATH=.libs timeout 180 "$UMEM_BIN" --pid "$HPID" findleaks \
    > /tmp/live.out 2>/tmp/live.err
echo "LIVE_EXIT=$?"
set -e
kill "$HPID" 2>/dev/null || true
echo "--- LIVE stdout ($(wc -c < /tmp/live.out) bytes) ---"
head -25 /tmp/live.out
echo "--- LIVE stderr ---"
head -10 /tmp/live.err

# --- (b) same command against a core ---------------------------------------
cd /tmp
ulimit -c unlimited
echo core.%p | sudo tee /proc/sys/kernel/core_pattern >/dev/null
set +e
LD_LIBRARY_PATH="$OLDPWD/.libs" UMEM_DEBUG=audit /tmp/holder crash
set -e
CORE=$(ls -t /tmp/core.* 2>/dev/null | head -1)
cd "$OLDPWD"
[ -n "$CORE" ] || { echo NO_CORE_PRODUCED; exit 0; }

set +e
LD_LIBRARY_PATH=.libs timeout 180 "$UMEM_BIN" --core "$CORE" --exe /tmp/holder \
    findleaks > /tmp/core.out 2>/tmp/core.err
echo "CORE_EXIT=$?"
set -e
echo "--- CORE stdout ($(wc -c < /tmp/core.out) bytes) ---"
head -25 /tmp/core.out
echo "--- CORE stderr ---"
head -25 /tmp/core.err

echo "=== VERDICT ==="
if [ ! -s /tmp/core.out ]; then
	echo "CORE MODE PRODUCES NO REPORT (and check CORE_EXIT above: exiting 0"
	echo "with empty output is a silent false 'no leaks')"
else
	echo "core mode produced output; compare with live above"
fi
