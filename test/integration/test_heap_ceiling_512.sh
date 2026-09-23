#!/usr/bin/env bash
# Second arm of the heap-ceiling regression: 512 B objects.
#
# 512 B objects take a different allocation path from 4 KiB ones (one-page
# slabs served through umem_va's quantum cache) and were still hitting
# vm.max_map_count at 8.2 GB after the 4 KiB path was fixed.  The plain
# test_heap_ceiling arm is blind to this; this arm is not.
here=$(cd "$(dirname "$0")" && pwd)
# Prefer the LIBTOOL WRAPPER (test/integration/test_heap_ceiling), not the raw
# .libs/ binary: make check runs this script without LD_LIBRARY_PATH, and the
# raw binary then fails to load libumem.so.1 (exit 127, "cannot open shared
# object file").  The wrapper sets up the library path itself.  Fall back to
# the raw binary only for hand-runs where LD_LIBRARY_PATH is already set.
bin="$here/test_heap_ceiling"
[[ -x $bin ]] || bin="$here/.libs/test_heap_ceiling"
[[ -x $bin ]] || { echo "SKIP: test_heap_ceiling not built"; exit 77; }
exec "$bin" 512
