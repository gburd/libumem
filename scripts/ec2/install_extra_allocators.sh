#!/usr/bin/env bash
# scripts/ec2/install_extra_allocators.sh - install/build the 6 non-package
# allocators (mimalloc, snmalloc, rpmalloc from source; jemalloc, tcmalloc,
# scudo from distro packages) system-wide so allocators.c's dlopen() finds
# them by soname with no per-invocation env vars. Run once per instance
# (idempotent). x86_64 and aarch64.
set -euo pipefail

ARCH=$(uname -m)
echo "== installing packaged allocators (jemalloc, tcmalloc/gperftools, clang+compiler-rt for scudo) =="
sudo dnf install -y jemalloc-devel gperftools-devel gperftools-libs cmake ninja-build \
    clang-tools-extra compiler-rt libstdc++-static libstdc++-devel git python3 >/dev/null

CLANG_VER=$(clang --version | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
SCUDO_SO="/usr/lib64/clang/${CLANG_VER}/lib/linux/libclang_rt.scudo_standalone-$( [[ $ARCH == aarch64 ]] && echo aarch64 || echo x86_64 ).so"
if [[ -f "$SCUDO_SO" ]]; then
    sudo cp "$SCUDO_SO" /usr/local/lib/libscudo_standalone.so
    echo "scudo: $SCUDO_SO -> /usr/local/lib/libscudo_standalone.so"
else
    echo "scudo: NOT FOUND at $SCUDO_SO (clang version mismatch?) -- allocator_scudo will report unavailable" >&2
fi

mkdir -p /tmp/build
cd /tmp/build

echo "== mimalloc (from source) =="
[[ -d mimalloc-src ]] || git clone --depth 1 https://github.com/microsoft/mimalloc.git mimalloc-src
mkdir -p mimalloc-src/out/release
( cd mimalloc-src/out/release && cmake -DCMAKE_BUILD_TYPE=Release ../.. >cfg.log 2>&1 && make -j"$(nproc)" >build.log 2>&1 )
MI_SO=$(find mimalloc-src/out/release -maxdepth 1 -name 'libmimalloc.so.*' -type f | head -1)
if [[ -n "$MI_SO" ]]; then
    sudo cp "$MI_SO" /usr/local/lib/libmimalloc.so
    echo "mimalloc: $MI_SO -> /usr/local/lib/libmimalloc.so"
else
    echo "mimalloc: BUILD FAILED, see /tmp/build/mimalloc-src/out/release/build.log" >&2
fi

echo "== snmalloc (from source) =="
[[ -d snmalloc-src ]] || git clone --depth 1 https://github.com/microsoft/snmalloc.git snmalloc-src
mkdir -p snmalloc-src/build
( cd snmalloc-src/build && cmake -GNinja -DCMAKE_BUILD_TYPE=Release \
    -DSNMALLOC_STATIC_LIBRARY=ON -DSNMALLOC_BUILD_TESTING=OFF ../ >cfg.log 2>&1 && ninja >build.log 2>&1 )
if [[ -f snmalloc-src/build/libsnmallocshim.so ]]; then
    sudo cp snmalloc-src/build/libsnmallocshim.so /usr/local/lib/libsnmallocshim.so
    echo "snmalloc: -> /usr/local/lib/libsnmallocshim.so"
else
    echo "snmalloc: BUILD FAILED, see /tmp/build/snmalloc-src/build/build.log" >&2
fi

echo "== rpmalloc (from source) =="
[[ -d rpmalloc-src ]] || git clone --depth 1 https://github.com/mjansson/rpmalloc.git rpmalloc-src
( cd rpmalloc-src && python3 configure.py >cfg.log 2>&1 && ninja >build.log 2>&1 )
RP_SO=$(find rpmalloc-src/bin -name 'librpmalloc.so' -path '*release*' | head -1)
if [[ -n "$RP_SO" ]]; then
    sudo cp "$RP_SO" /usr/local/lib/librpmalloc.so
    echo "rpmalloc: $RP_SO -> /usr/local/lib/librpmalloc.so"
else
    echo "rpmalloc: BUILD FAILED, see /tmp/build/rpmalloc-src/build.log" >&2
fi

sudo ldconfig -n /usr/local/lib
echo '/usr/local/lib' | sudo tee /etc/ld.so.conf.d/usrlocal.conf >/dev/null
sudo ldconfig
echo "== done; ldconfig cache: =="
ldconfig -p | grep -iE 'mimalloc|snmalloc|rpmalloc|scudo|jemalloc|tcmalloc' || true
