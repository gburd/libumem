#!/usr/bin/env bash
# scripts/ec2/launch.sh <role> - launch a tagged libumem EC2 instance.
#
#   roles: intel-lo | intel-hi | arm-lo | arm-hi
#
# One-time setup (key pair + security group) is created lazily on first run.
# Prints the instance id + public DNS. Idempotent: if a running instance for
# the role already exists, it is reused (not duplicated).
#
# Cost note (burner account): high roles are .metal (~$14/hr). Run
# terminate.sh <role> when done, or terminate.sh --reap-idle.

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$DIR/common.sh"

ROLE="${1:?usage: launch.sh <intel-lo|intel-hi|arm-lo|arm-hi>}"
ARCH="$(role_arch "$ROLE")"
ITYPE="$(role_instance_type "$ROLE")"

# --- one-time: key pair -----------------------------------------------------
ensure_key_pair() {
	if aws ec2 describe-key-pairs --key-names "$KEY_NAME" >/dev/null 2>&1; then
		[ -f "$KEY_FILE" ] || { echo "key '$KEY_NAME' exists in AWS but $KEY_FILE is missing" >&2; exit 1; }
		return
	fi
	log "creating key pair $KEY_NAME -> $KEY_FILE"
	mkdir -p "$(dirname "$KEY_FILE")"
	aws ec2 create-key-pair --key-name "$KEY_NAME" \
		--query 'KeyMaterial' --output text > "$KEY_FILE"
	chmod 600 "$KEY_FILE"
}

# --- one-time: security group (SSH from our IP only) ------------------------
ensure_sg() {
	local sgid myip
	sgid="$(aws ec2 describe-security-groups --filters "Name=group-name,Values=$SG_NAME" \
		--query 'SecurityGroups[0].GroupId' --output text 2>/dev/null)"
	if [ "$sgid" = "None" ] || [ -z "$sgid" ]; then
		log "creating security group $SG_NAME"
		sgid="$(aws ec2 create-security-group --group-name "$SG_NAME" \
			--description "libumem bench SSH" --query 'GroupId' --output text)"
	fi
	myip="$(curl -s --max-time 10 https://checkip.amazonaws.com | tr -d '[:space:]')"
	# Authorize (ignore "already exists").
	aws ec2 authorize-security-group-ingress --group-id "$sgid" \
		--protocol tcp --port 22 --cidr "${myip}/32" >/dev/null 2>&1 || true
	echo "$sgid"
}

existing="$(instance_id_for_role "$ROLE")"
if [ -n "$existing" ]; then
	log "role $ROLE already running: $existing ($(public_dns_for_id "$existing"))"
	echo "$existing"
	exit 0
fi

ensure_key_pair
SGID="$(ensure_sg)"
AMI="$(ami_for "$ARCH")"
log "launching $ROLE: $ITYPE ($ARCH) ami=$AMI sg=$SGID"

IID="$(aws ec2 run-instances \
	--image-id "$AMI" --instance-type "$ITYPE" \
	--key-name "$KEY_NAME" --security-group-ids "$SGID" \
	--block-device-mappings 'DeviceName=/dev/xvda,Ebs={VolumeSize=60,VolumeType=gp3,Iops=6000,Throughput=400}' \
	--metadata-options 'HttpTokens=required,HttpEndpoint=enabled' \
	--tag-specifications "ResourceType=instance,Tags=[{Key=Project,Value=${PROJECT_TAG}},{Key=Role,Value=${ROLE}},{Key=Name,Value=libumem-${ROLE}}]" \
	--query 'Instances[0].InstanceId' --output text)"

log "waiting for $IID to run..."
aws ec2 wait instance-running --instance-ids "$IID"
DNS="$(public_dns_for_id "$IID")"
log "waiting for SSH on $DNS..."
for _ in $(seq 1 40); do
	if ssh $SSH_OPTS -i "$KEY_FILE" "${SSH_USER}@${DNS}" true 2>/dev/null; then break; fi
	sleep 5
done
log "ready: $IID $DNS  (next: bootstrap.sh $ROLE)"
echo "$IID"
