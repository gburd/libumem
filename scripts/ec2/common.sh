#!/usr/bin/env bash
# scripts/ec2/common.sh - shared config + helpers for the libumem EC2 harness.
#
# Sourced by launch.sh / bootstrap.sh / run-remote.sh / terminate.sh.
# All heavy libumem build/test/bench work runs on EC2 so the local host
# (floki) is never starved or OOM'd. Burner account; ALWAYS terminate idle
# instances (terminate.sh --reap-idle).

set -euo pipefail

# --- account / region -------------------------------------------------------
export AWS_PROFILE="${AWS_PROFILE:-numa}"
export AWS_DEFAULT_REGION="${AWS_DEFAULT_REGION:-us-east-2}"
export AWS_PAGER=""

PROJECT_TAG="libumem"
KEY_NAME="${KEY_NAME:-libumem-bench}"
KEY_FILE="${KEY_FILE:-$HOME/.ssh/${KEY_NAME}.pem}"
SG_NAME="${SG_NAME:-libumem-ssh}"
SSH_USER="ec2-user"           # Amazon Linux 2023 default user
SSH_OPTS="-o StrictHostKeyChecking=accept-new -o ConnectTimeout=15 -o ServerAliveInterval=30 -o IdentitiesOnly=yes -o IdentityAgent=none"

# Amazon Linux 2023 AMIs are resolved dynamically (see ami_for).

# --- role -> (arch, instance-type) -----------------------------------------
# Low = 8 vCPU; High = very high core count (metal). Intel + Graviton(aarch64).
role_arch() {
	case "$1" in
		intel-lo|intel-hi) echo x86_64 ;;
		arm-lo|arm-hi)     echo arm64 ;;
		*) echo "unknown role: $1" >&2; return 1 ;;
	esac
}

role_instance_type() {
	case "$1" in
		intel-lo) echo c7i.2xlarge ;;       # 8 vCPU
		intel-hi) echo c7i.metal-48xl ;;    # 192 vCPU
		arm-lo)   echo c7g.2xlarge ;;        # 8 vCPU
		arm-hi)   echo c8g.metal-48xl ;;     # 192 vCPU (Graviton4); c7g.metal=64 fallback
		*) echo "unknown role: $1" >&2; return 1 ;;
	esac
}

# --- helpers ----------------------------------------------------------------
log() { printf '[ec2:%s] %s\n' "${ROLE:-?}" "$*" >&2; }

ami_for() {
	local arch="$1" pat
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
