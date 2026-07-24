#!/usr/bin/env bash
# STEP 0 OOB fix validation on >256-CPU hardware (intel-hi).
set +e
ASAN_DIR="$(dirname "$(gcc -print-file-name=libasan.so)")"
# The dir ships libasan.so.6.0.0 but not the libasan.so.6 SONAME the binaries
# were linked against; create it in a writable temp dir and prepend that.
ASAN_LINKDIR="$(mktemp -d)"
if [ -e "${ASAN_DIR}/libasan.so.6.0.0" ] && [ ! -e "${ASAN_DIR}/libasan.so.6" ]; then
    ln -sf "${ASAN_DIR}/libasan.so.6.0.0" "${ASAN_LINKDIR}/libasan.so.6"
fi
ASAN_DIR="${ASAN_LINKDIR}:${ASAN_DIR}"
export LD_LIBRARY_PATH=".libs:${ASAN_DIR}:${LD_LIBRARY_PATH}"
echo "=== vCPU ==="
nproc
echo "=== OOB repro (asan) ==="
LD_LIBRARY_PATH=".libs:${ASAN_DIR}" ASAN_OPTIONS=abort_on_error=1:halt_on_error=1 \
    test/unit/.libs/repro_cpu_node_oob
echo "repro_exit=$?"
echo "=== detect umem_max_ncpus ==="
gcc test/bench/probe_ncpus.c -o /tmp/probe_ncpus -I. -L.libs -lumem \
    -fsanitize=address 2>/tmp/pn.log
if [ $? -eq 0 ]; then
    LD_LIBRARY_PATH=".libs:${ASAN_DIR}" /tmp/probe_ncpus
else
    echo "BUILDFAIL"; cat /tmp/pn.log
fi
echo "=== quick test_main --no-fork under ASan (tail) ==="
LD_LIBRARY_PATH=".libs:${ASAN_DIR}" ASAN_OPTIONS=abort_on_error=1:halt_on_error=1 \
    test/.libs/test_main --no-fork 2>&1 | tail -20
echo "test_main_exit=${PIPESTATUS[0]}"
