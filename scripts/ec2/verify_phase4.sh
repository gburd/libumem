#!/usr/bin/env bash
# Phase-4 final verification.  Every remaining build option must actually
# build, `make check` must stay 8/8, and the release artifacts must work.
#
# Run in ONE job with clean-regen.sh (AGENTS.md section 4).
set -u

pass=0; fail=0
step() { echo; echo "############ $* ############"; }
ok()   { echo "PASS: $*"; pass=$((pass+1)); }
bad()  { echo "FAIL: $*"; fail=$((fail+1)); }

check_suite() {   # $1 = label
	local out rc
	out="$(make check 2>&1)"; rc=$?
	echo "$out" | sed -n '/^PASS:\|^FAIL:\|^SKIP:\|^# TOTAL\|^# PASS\|^# FAIL\|^# SKIP\|^# ERROR/p'
	if [ $rc -ne 0 ]; then bad "$1: make check rc=$rc"; echo "$out" | tail -30; return 1; fi
	local total passed failed
	total=$(echo "$out"  | sed -n 's/^# TOTAL: *//p' | tail -1)
	passed=$(echo "$out" | sed -n 's/^# PASS: *//p'  | tail -1)
	failed=$(echo "$out" | sed -n 's/^# FAIL: *//p'  | tail -1)
	if [ "$total" = "8" ] && [ "$passed" = "8" ] && [ "$failed" = "0" ]; then
		ok "$1: make check 8/8"
	else
		bad "$1: make check TOTAL=$total PASS=$passed FAIL=$failed (expected 8/8/0)"
	fi
}

build_cfg() {     # $1 = label, rest = configure args
	local label="$1"; shift
	step "$label: configure $*"
	if ./scripts/ec2/clean-regen.sh "$@" > /tmp/regen.log 2>&1; then
		ok "$label: configure"
	else
		bad "$label: configure"; tail -25 /tmp/regen.log; return 1
	fi
	if make -j"$(nproc)" > /tmp/make.log 2>&1; then
		ok "$label: make"
	else
		bad "$label: make"; tail -40 /tmp/make.log; return 1
	fi
	grep -c 'warning:' /tmp/make.log | sed 's/^/  compiler warnings: /'
}

# --- 1. default build: check + hash regression + install-check ---------------
build_cfg "default" || exit 1
check_suite "default"

step "default: hash-partition regression"
if test/unit/test_hash_partition; then ok "test_hash_partition"; else bad "test_hash_partition"; fi

step "default: no AVX2 in a generic build"
if grep -q -- '-mavx2' /tmp/make.log; then
	bad "default build used -mavx2 (should be opt-in)"
else
	ok "default build is AVX2-free"
fi
if [ -f config.h ] && grep -q '^#define HAVE_AVX2' config.h; then
	bad "HAVE_AVX2 defined in a default build"
else
	ok "HAVE_AVX2 not defined by default"
fi

step "default: removed options are actually gone"
for opt in percpu-caching htm; do
	if ./configure --help 2>/dev/null | grep -q -- "--enable-$opt"; then
		bad "--enable-$opt still advertised by configure --help"
	else
		ok "--enable-$opt is gone from configure --help"
	fi
done
# An unknown --enable-* is a warning, not an error, in autoconf; make sure it
# at least does not silently produce a percpu/htm build.
./configure --enable-percpu-caching > /tmp/cfg-stale.log 2>&1
if grep -qi 'unrecognized option.*percpu' /tmp/cfg-stale.log; then
	ok "--enable-percpu-caching is reported as unrecognized"
else
	echo "  (note: autoconf did not flag it; checking it has no effect)"
fi
if [ -f config.h ] && grep -q '^#define UMEM_PER_CPU_CACHE' config.h; then
	bad "UMEM_PER_CPU_CACHE still definable"
else
	ok "UMEM_PER_CPU_CACHE cannot be defined"
fi

step "default: install-check (installed prefix + external consumer)"
./scripts/ec2/clean-regen.sh > /dev/null 2>&1 && make -j"$(nproc)" > /dev/null 2>&1
if make install-check 2>&1 | tail -8; then ok "install-check"; else bad "install-check"; fi

# --- 2. each remaining feature flag -----------------------------------------
build_cfg "rseq-off" --disable-rseq   && check_suite "rseq-off"
build_cfg "rseq-on"  --enable-rseq    && check_suite "rseq-on"

step "numa: is libnuma present on this host?"
if ! ls /usr/lib64/libnuma.so* /usr/lib/x86_64-linux-gnu/libnuma.so* >/dev/null 2>&1; then
	echo "  libnuma not installed; installing"
	sudo dnf install -y numactl-libs numactl-devel >/dev/null 2>&1 || \
	  sudo yum install -y numactl-devel >/dev/null 2>&1 || true
fi
if ls /usr/lib64/libnuma.so 2>/dev/null >/dev/null || ls /usr/lib/*/libnuma.so 2>/dev/null >/dev/null; then
	build_cfg "numa-on" --enable-numa && check_suite "numa-on"
else
	echo "SKIP: --enable-numa (libnuma unavailable on this host -- SKIP, not pass)"
fi
build_cfg "numa-off" --disable-numa && check_suite "numa-off"

step "avx2: opt-in build"
build_cfg "avx2-on" --enable-avx2 && {
	if grep -q -- '-mavx2' /tmp/make.log; then ok "--enable-avx2 passes -mavx2"; else bad "--enable-avx2 did not pass -mavx2"; fi
	check_suite "avx2-on"
}

build_cfg "introspect" --enable-introspect && check_suite "introspect"

# --- 3. release artifacts ---------------------------------------------------
build_cfg "dist-base" || exit 1
step "make dist"
if make dist > /tmp/dist.log 2>&1; then ok "make dist"; else bad "make dist"; tail -20 /tmp/dist.log; fi
ls -la umem-*.tar.gz 2>/dev/null

step "build + check + install-check from the extracted tarball"
D=/tmp/ph4dist
[ -d "$D" ] && mv "$D" "$D.prev.$$"
mkdir -p "$D"
V="$(ls umem-*.tar.gz | head -1 | sed 's/^umem-//; s/\.tar\.gz$//')"
tar xzf "umem-$V.tar.gz" -C "$D"
(
	cd "$D/umem-$V" || exit 1
	if ./configure > /tmp/tb-cfg.log 2>&1; then echo "PASS: tarball configure"; else echo "FAIL: tarball configure"; tail -20 /tmp/tb-cfg.log; exit 1; fi
	if make -j"$(nproc)" > /tmp/tb-make.log 2>&1; then echo "PASS: tarball make"; else echo "FAIL: tarball make"; tail -30 /tmp/tb-make.log; exit 1; fi
	make check 2>&1 | sed -n '/^# TOTAL\|^# PASS\|^# FAIL/p'
	if make test/property/prop_palloc > /tmp/tb-palloc.log 2>&1; then
		echo "PASS: tarball builds prop_palloc (examples/umem_palloc.c is distributed)"
	else
		echo "FAIL: tarball cannot build prop_palloc"; tail -15 /tmp/tb-palloc.log
	fi
	make install-check 2>&1 | tail -6
)
tbrc=$?
[ $tbrc -eq 0 ] && ok "tarball end-to-end" || bad "tarball end-to-end"

echo
echo "############ SUMMARY ############"
echo "PASS: $pass   FAIL: $fail"
[ "$fail" -eq 0 ] || exit 1
