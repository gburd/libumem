#!/usr/bin/env bash
# scripts/ec2/terminate.sh <role> | --all | --reap-idle
#
#   <role>       terminate the instance for one role (intel-lo, arm-hi, ...)
#   --all        terminate every Project=libumem instance
#   --reap-idle  terminate Project=libumem instances idle >2h (avg CPU <2%)
#
# ALWAYS run this when done. A forgotten .metal is the only real cost mistake.

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$DIR/common.sh"
ROLE="term"

all_libumem_ids() {
	aws ec2 describe-instances \
		--filters "Name=tag:Project,Values=${PROJECT_TAG}" \
			"Name=instance-state-name,Values=running,pending,stopping,stopped" \
		--query 'Reservations[].Instances[].InstanceId' --output text
}

terminate_ids() {
	local ids="$*"
	[ -z "$ids" ] && { log "nothing to terminate"; return 0; }
	log "terminating: $ids"
	aws ec2 terminate-instances --instance-ids $ids \
		--query 'TerminatingInstances[].[InstanceId,CurrentState.Name]' --output text
}

# avg CPUUtilization over the last 2h for an instance (empty if no datapoints).
avg_cpu_2h() {
	local id="$1" end start
	end="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	start="$(date -u -d '2 hours ago' +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || date -u -v-2H +%Y-%m-%dT%H:%M:%SZ)"
	aws cloudwatch get-metric-statistics --namespace AWS/EC2 \
		--metric-name CPUUtilization --dimensions "Name=InstanceId,Value=$id" \
		--start-time "$start" --end-time "$end" --period 7200 --statistics Average \
		--query 'Datapoints[0].Average' --output text 2>/dev/null
}

case "${1:-}" in
	--all)
		terminate_ids "$(all_libumem_ids)"
		;;
	--reap-idle)
		# SAFETY: never auto-reap the shared low-core dev boxes (intel-lo /
		# arm-lo).  In a multi-agent setup those are shared build/test hosts;
		# only reap idle HIGH-CORE (metal) instances, which are the expensive
		# ones worth reclaiming.  Terminate shared boxes explicitly by role.
		reap=""
		for id in $(all_libumem_ids); do
			role="$(aws ec2 describe-instances --instance-ids "$id" \
				--query 'Reservations[].Instances[].Tags[?Key==`Role`].Value|[0]' \
				--output text 2>/dev/null)"
			case "$role" in
				intel-lo|arm-lo)
					log "skip shared box $id ($role) — reap explicitly if needed"
					continue ;;
			esac
			cpu="$(avg_cpu_2h "$id")"
			if [ -n "$cpu" ] && [ "$cpu" != "None" ] && \
			   awk "BEGIN{exit !($cpu < 2.0)}"; then
				log "idle candidate $id ($role, avg CPU ${cpu}%)"
				reap="$reap $id"
			fi
		done
		terminate_ids $reap
		;;
	intel-lo|intel-hi|arm-lo|arm-hi)
		terminate_ids "$(instance_id_for_role "$1")"
		;;
	*)
		echo "usage: terminate.sh <role> | --all | --reap-idle" >&2
		echo "running Project=libumem instances:" >&2
		aws ec2 describe-instances --filters "Name=tag:Project,Values=${PROJECT_TAG}" \
			"Name=instance-state-name,Values=running,pending" \
			--query 'Reservations[].Instances[].[InstanceId,InstanceType,Tags[?Key==`Role`].Value|[0]]' \
			--output text >&2 || true
		exit 2
		;;
esac
