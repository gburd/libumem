#!/usr/bin/env bash
# gateC-driver.sh -- QEMU-user cross-build + run a test subset for the new
# AALs the PAL/AAL refactor buys: riscv64, armhf (arm32), aarch64.
#
# Runs on the kiwi intel-lo box.  Expects to be invoked from inside an
# isolated git-archive tree (verify-isolated.sh), with the cross toolchains,
# qemu-user-static, binfmt-support and the cross sysroots already installed.
#
# For each arch it records COMPILES (library builds), LINKS (test binaries
# build), RUNS (smoke + a core test pass under qemu-user).  A build-only or
# #error/link-blocked arch is a VALID, honest result -- an incomplete AAL
# (e.g. no per-arch rseq .S) is the expected outcome for the NEW stubs; the
# driver records the gap, it does not fake a pass.
#
# This is validation only; it does NOT modify any library source.
set -u

SRC="$(pwd)"
OUT="$HOME/libumem-gateC-out"
rm -rf "$OUT"; mkdir -p "$OUT"

# arch | triple | qemu | sysroot | configure extra
# riscv64 and arm32 have NO per-arch rseq .S, and umem.c calls the rseq
# fastpath asm whenever HAVE_LINUX_RSEQ_H is defined (any Linux arch has the
# header).  So we first try the DEFAULT configure (rseq auto -> yes) to show
# the gap honestly, then retry with --disable-rseq.
ARCHES="riscv64:riscv64-linux-gnu:qemu-riscv64-static
arm32:arm-linux-gnueabihf:qemu-arm-static
aarch64:aarch64-linux-gnu:qemu-aarch64-static"

say() { printf '\n========== %s ==========\n' "$*"; }

build_one() {
	local arch="$1" triple="$2" qemu="$3" rseqflag="$4" tag="$5"
	local wd="$HOME/gateC-${arch}-${tag}"
	local log="$OUT/${arch}-${tag}.log"
	local sysroot="/usr/${triple}"
	rm -rf "$wd"; cp -a "$SRC" "$wd"
	# Subshell: an `exit` from a failed stage ends THIS build only, not the
	# whole driver.
	(
		cd "$wd" || exit 90
		echo "### arch=$arch triple=$triple rseq=$rseqflag tag=$tag"
		echo "### $($triple-gcc --version | head -1)"
		echo "--- clean-regen ---"
		./scripts/ec2/clean-regen.sh 2>&1 | tail -5
		echo "--- configure ---"
		# Cross configure.  --host sets the triple so autotools picks the
		# cross tools; CC pinned explicitly.  rseqflag is "" or --disable-rseq.
		./configure --host="$triple" CC="$triple-gcc" $rseqflag 2>&1 | tail -25
		cfg_rc=${PIPESTATUS[0]}
		echo "### configure rc=$cfg_rc"
		[ "$cfg_rc" -ne 0 ] && { echo "### STAGE=configure-FAIL"; exit 2; }
		echo "--- make lib (libumem.la) ---"
		make -j"$(nproc)" libumem.la 2>&1 | tail -40
		lib_rc=${PIPESTATUS[0]}
		echo "### make libumem.la rc=$lib_rc"
		[ "$lib_rc" -ne 0 ] && { echo "### STAGE=COMPILE-FAIL"; exit 3; }
		echo "### STAGE=COMPILES-OK"
		echo "--- make smoke + core test binaries ---"
		# pal_aal_smoke is the port-identity test; umem_test/umem_test2 are
		# core unit tests.  Build just these (full make is slow under cross).
		make -j"$(nproc)" test/port/pal_aal_smoke umem_test umem_test2 2>&1 | tail -40
		lnk_rc=${PIPESTATUS[0]}
		echo "### make tests rc=$lnk_rc"
		[ "$lnk_rc" -ne 0 ] && { echo "### STAGE=LINK-FAIL"; exit 4; }
		echo "### STAGE=LINKS-OK"
		echo "--- run under qemu: pal_aal_smoke ---"
		# Test binaries are libtool wrappers; the real ELF is in .libs/.
		# Run under qemu-user with the cross sysroot and the built libs on
		# the loader path.
		local libpath=".libs:$wd/.libs"
		for t in test/port/pal_aal_smoke umem_test umem_test2; do
			local elf="$(dirname "$t")/.libs/$(basename "$t")"
			[ -x "$elf" ] || elf="$t"
			echo ">>> qemu $qemu $elf"
			QEMU_LD_PREFIX="$sysroot" LD_LIBRARY_PATH="$libpath" \
				timeout 120 "$qemu" -L "$sysroot" "$elf"; rc=$?
			echo "### run $t rc=$rc"
		done
		echo "### STAGE=RAN"
	) >"$log" 2>&1
	echo "logged: $log (exit $?)"
}

for row in $ARCHES; do
	IFS=: read -r arch triple qemu <<<"$row"
	say "$arch ($triple) -- DEFAULT configure (rseq auto)"
	build_one "$arch" "$triple" "$qemu" "" default
	say "$arch ($triple) -- --disable-rseq"
	build_one "$arch" "$triple" "$qemu" "--disable-rseq" norseq
done

say "SUMMARY"
for row in $ARCHES; do
	IFS=: read -r arch triple qemu <<<"$row"
	for tag in default norseq; do
		f="$OUT/${arch}-${tag}.log"
		[ -f "$f" ] || continue
		stage="$(grep -o '### STAGE=[A-Za-z-]*' "$f" | tail -1 | sed 's/### STAGE=//')"
		cfg="$(grep -o '### configure rc=[0-9]*' "$f" | tail -1)"
		printf '%-10s %-8s final-stage=%-14s %s\n' "$arch" "$tag" "${stage:-NONE}" "$cfg"
	done
done
echo "=== logs in $OUT ==="
ls -la "$OUT"
