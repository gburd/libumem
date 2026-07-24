#!/bin/sh
# Run an ASan-instrumented libumem test binary on the EC2 instance.
# libasan has no versioned soname symlink on AL2023, so preload the file
# directly.  Usage: remote_run_asan.sh <path-to-.libs-binary> [args...]
cd ~/libumem || exit 1
ASAN="$(gcc -print-file-name=libasan.so.6.0.0)"
[ -f "$ASAN" ] || ASAN="$(ls /usr/lib/gcc/*/*/libasan.so.6.0.0 2>/dev/null | head -1)"
exec env ASAN_OPTIONS=detect_leaks=0:abort_on_error=0 \
    LD_PRELOAD="$ASAN" LD_LIBRARY_PATH=.libs "$@"
