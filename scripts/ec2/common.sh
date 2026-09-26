#!/usr/bin/env bash
# scripts/ec2/common.sh - shared config + helpers for the libumem EC2 harness.
#
# Sourced by launch.sh / bootstrap.sh / run-remote.sh / terminate.sh.
# All heavy libumem build/test/bench work runs on EC2 so the local host
# (floki) is never starved or OOM'd. Burner account; ALWAYS terminate idle
# instances (terminate.sh --reap-idle).

set -euo pipefail

# --- account / region -------------------------------------------------------
# NOTE: this burner account/profile has rotated repeatedly (numa -> beef ->
# bene -> hotdog).  Always verify with `aws sts get-caller-identity` before a
# session; if it fails, probe `aws configure list-profiles` for a live one and
# update this default rather than assuming the previous one still works.
export AWS_PROFILE="${AWS_PROFILE:-hotdog}"
export AWS_DEFAULT_REGION="${AWS_DEFAULT_REGION:-us-east-2}"
export AWS_PAGER=""

PROJECT_TAG="libumem"
KEY_NAME="${KEY_NAME:-libumem-bench}"
KEY_FILE="${KEY_FILE:-$HOME/.ssh/${KEY_NAME}.pem}"
SG_NAME="${SG_NAME:-libumem-ssh}"
# OS selection: al2023 (default) or debian.  Debian uses a different login
# user and apt instead of dnf; bootstrap.sh honours EC2_OS too.  The user asked
# for the comparison + P8.5b work to run on the latest Debian.
EC2_OS="${EC2_OS:-al2023}"
case "$EC2_OS" in
	debian) SSH_USER="admin" ;;   # Debian official AMIs log in as 'admin'
	al2023) SSH_USER="ec2-user" ;;
	*) echo "unknown EC2_OS: $EC2_OS (use al2023|debian)" >&2; exit 1 ;;
esac
SSH_OPTS="-o StrictHostKeyChecking=accept-new -o ConnectTimeout=15 -o ServerAliveInterval=30 -o IdentitiesOnly=yes -o IdentityAgent=none"

# Amazon Linux 2023 AMIs are resolved dynamically (see ami_for).

# --- role -> (arch, instance-type) -----------------------------------------
# Low = 8 vCPU; High = very high core count (metal). Intel + Graviton(aarch64).
#
# WORKER-SCOPED ROLES: a role may carry a "@worker" suffix, e.g.
# "intel-lo@ptc" or "arm-hi@w2".  The suffix selects the same arch and
# instance type as the base role but gets its OWN tagged instance and its own
# remote working directory (see run-remote.sh).  This exists because
# run-remote.sh rsyncs the local worktree with --delete: two agents sharing one
# role's instance silently clobber each other's in-flight sources and logs.
# Parallel agents MUST each use a distinct @worker suffix.
role_base() { echo "${1%%@*}"; }
role_worker() { case "$1" in *@*) echo "${1#*@}" ;; *) echo "" ;; esac; }

role_arch() {
	case "$(role_base "$1")" in
		intel-lo|intel-hi) echo x86_64 ;;
		arm-lo|arm-hi)     echo arm64 ;;
		*) echo "unknown role: $1" >&2; return 1 ;;
	esac
}

role_instance_type() {
	case "$(role_base "$1")" in
		intel-lo) echo c7i.2xlarge ;;       # 8 vCPU
		intel-hi) echo c7i.metal-48xl ;;    # 192 vCPU
		arm-lo)   echo c7g.2xlarge ;;        # 8 vCPU
		arm-hi)   echo c8g.metal-48xl ;;     # 192 vCPU (Graviton4); c7g.metal=64 fallback
		*) echo "unknown role: $1" >&2; return 1 ;;
	esac
}

# Remote working directory for a role.  Worker-scoped roles get their own
# directory so a shared instance (if ever reused) still cannot cross-clobber.
role_remote_dir() {
	local w; w="$(role_worker "$1")"
	if [ -n "$w" ]; then echo "libumem-$w"; else echo "libumem"; fi
}

# --- helpers ----------------------------------------------------------------
log() { printf '[ec2:%s] %s\n' "${ROLE:-?}" "$*" >&2; }

ami_for() {
	local arch="$1" pat
	if [ "$EC2_OS" = "debian" ]; then
		# Debian official AMIs, owner 136693071363.  Try the newest stable
		# major first (13 trixie), then 12 bookworm.  Values globs support
		# only * and ? (no [0-9] ranges), so enumerate majors explicitly.
		local amiarch v img
		case "$arch" in
			x86_64) amiarch=amd64 ;;
			arm64)  amiarch=arm64 ;;
			*) echo "unknown arch: $arch" >&2; return 1 ;;
		esac
		for v in 14 13 12; do
			img=$(aws ec2 describe-images --owners 136693071363 \
				--filters "Name=name,Values=debian-${v}-${amiarch}-*" \
					'Name=state,Values=available' \
				--query 'reverse(sort_by(Images,&CreationDate))[:1].ImageId' \
				--output text 2>/dev/null)
			if [ -n "$img" ] && [ "$img" != "None" ]; then
				echo "$img"; return
			fi
		done
		echo "no debian AMI found for $arch" >&2; return 1
	fi
	case "$arch" in
		x86_64) pat='al2023-ami-2023.*-x86_64' ;;
		arm64)  pat='al2023-ami-2023.*-arm64' ;;
		*) echo "unknown arch: $arch" >&2; return 1 ;;
	esac
	aws ec2 describe-images --owners amazon \
		--filters "Name=name,Values=${pat}" 'Name=state,Values=available' \
		--query 'reverse(sort_by(Images,&CreationDate))[:1].ImageId' \
		--output text
}

# Instance ID for a role's *running or pending* instance (empty if none).
instance_id_for_role() {
	aws ec2 describe-instances \
		--filters "Name=tag:Project,Values=${PROJECT_TAG}" \
			"Name=tag:Role,Values=$1" \
			"Name=instance-state-name,Values=running,pending" \
		--query 'Reservations[].Instances[].InstanceId' --output text
}

public_dns_for_id() {
	aws ec2 describe-instances --instance-ids "$1" \
		--query 'Reservations[].Instances[].PublicDnsName' --output text
}

require_running_role() {
	local id; id="$(instance_id_for_role "$1")"
	if [ -z "$id" ]; then
		echo "no running instance for role '$1' (launch.sh $1 first)" >&2
		return 1
	fi
	echo "$id"
}
