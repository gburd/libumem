#!/usr/bin/env bash
# scripts/ec2/bootstrap.sh <role> - install toolchain + apply OS tuning on the
# role's instance, and record provenance (meta.toml). Idempotent.
#
# Toolchain: gcc, clang, autoconf/automake/libtool, lcov, valgrind, perf,
# numactl, gdb, lldb, python3, git, rsync.
# Tuning: CPU governor=performance, THP=never, numa_balancing=0.

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$DIR/common.sh"

ROLE="${1:?usage: bootstrap.sh <role>}"
IID="$(require_running_role "$ROLE")"
DNS="$(public_dns_for_id "$IID")"
log "bootstrapping $IID ($DNS)"

# Heredoc runs ON the instance.
ssh $SSH_OPTS -i "$KEY_FILE" "${SSH_USER}@${DNS}" 'bash -s' <<'REMOTE'
set -euo pipefail
echo "== installing toolchain =="
sudo dnf -y groupinstall "Development Tools" >/dev/null 2>&1 || true
sudo dnf -y install \
	gcc clang autoconf automake libtool make \
	lcov valgrind numactl gdb lldb python3 git rsync \
	perf kernel-devel patchutils >/dev/null 2>&1 || \
	sudo dnf -y install gcc clang autoconf automake libtool make numactl gdb python3 git rsync

echo "== OS tuning (best-effort; metal honors more of these) =="
# CPU governor -> performance
if command -v cpupower >/dev/null 2>&1; then
	sudo cpupower frequency-set -g performance >/dev/null 2>&1 || true
else
	for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
		[ -w "$g" ] && echo performance | sudo tee "$g" >/dev/null 2>&1 || true
	done
fi
# Transparent hugepages off (interferes with explicit tuning + adds jitter)
echo never | sudo tee /sys/kernel/mm/transparent_hugepage/enabled >/dev/null 2>&1 || true
# Deterministic NUMA placement
sudo sysctl -w kernel.numa_balancing=0 >/dev/null 2>&1 || true
# Allow perf for non-root
sudo sysctl -w kernel.perf_event_paranoid=1 >/dev/null 2>&1 || true
# Core dumps for stress crashes
ulimit -c unlimited 2>/dev/null || true

echo "== provenance -> ~/meta.toml =="
{
	echo "# libumem EC2 instance provenance"
	echo "captured = \"$(date -u +%Y-%m-%dT%H:%M:%SZ)\""
	echo "uname = \"$(uname -a)\""
	echo "gcc = \"$(gcc --version | head -1)\""
	echo "clang = \"$(clang --version 2>/dev/null | head -1 || echo n/a)\""
	echo "glibc = \"$(ldd --version | head -1)\""
	echo "governor = \"$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)\""
	echo "thp = \"$(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo unknown)\""
	echo "numa_balancing = \"$(cat /proc/sys/kernel/numa_balancing 2>/dev/null || echo unknown)\""
	echo ""
	echo "[cpu]"
	lscpu | sed 's/^/# /'
	echo ""
	echo "[numa]"
	(numactl -H 2>/dev/null || echo "numactl unavailable") | sed 's/^/# /'
} > ~/meta.toml
echo "== bootstrap done =="
REMOTE

log "bootstrap complete for $ROLE (meta.toml on instance; pulled with results)"
