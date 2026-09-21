#!/usr/bin/env bash
# scripts/ec2/job.sh - run a long remote command as a supervised background JOB.
#
#   job.sh <role> start   <job> <timeout_sec> "<command>"
#   job.sh <role> status  <job>
#   job.sh <role> tail    <job> [lines]
#   job.sh <role> wait    <job> [poll_sec] [max_wait_sec]
#   job.sh <role> fetch   <job>          # copy the job's logs into docs/results/
#   job.sh <role> kill    <job>
#   job.sh <role> list
#
# WHY THIS EXISTS (read before using run-remote.sh for anything slow):
#
#   run-remote.sh holds an SSH session open for the whole command.  Three
#   things went wrong repeatedly with that model on this project:
#
#     1. A local timeout/disconnect orphaned the remote work, which kept
#        running (a 192-vCPU box burning money, or a stress binary spinning
#        for HOURS) while the agent believed the step had ended.
#     2. Re-running the "same" command stacked multiple concurrent copies,
#        each fighting for the same cores and the same log filenames, which
#        produced nonsense results that looked like real regressions.
#     3. Every run-remote.sh call re-rsyncs the worktree with --delete, so
#        logs written into the repo directory were destroyed before anyone
#        read them.
#
#   This runner fixes all three: the deadline and cleanup live on the REMOTE
#   side, output goes to a persistent location OUTSIDE the synced repo, and
#   every poll is a short command that returns immediately.
#
# GUARANTEES
#   * timeout(1) runs remotely, so the deadline survives SSH loss.
#   * setsid + process-group kill, so children (make -j, stress binaries,
#     pinned benchmarks) die with the job instead of being orphaned.
#   * logs live in ~/libumem-jobs/<job>/ (NOT the rsynced repo), so they
#     survive the next sync and are still readable after a failure.
#   * "start" refuses to launch if that job is already running, instead of
#        silently doubling the load.
#   * exit status, start/end time, and the command are all recorded.

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$DIR/common.sh"

ROLE="${1:?usage: job.sh <role> <start|status|tail|wait|fetch|kill|list> ...}"
ACTION="${2:?usage: job.sh <role> <start|status|tail|wait|fetch|kill|list> ...}"
IID="$(require_running_role "$ROLE")"
DNS="$(public_dns_for_id "$IID")"
REMOTE_DIR="$(role_remote_dir "$ROLE")"
JOBROOT="libumem-jobs"

rsh() { ssh $SSH_OPTS -i "$KEY_FILE" "${SSH_USER}@${DNS}" "$@"; }

case "$ACTION" in
start)
	JOB="${3:?job name}"; TMO="${4:?timeout seconds}"; CMD="${5:?command}"
	# Build the remote supervisor script locally, ship it base64-encoded, so
	# no quoting/escaping games and no self-matching pkill patterns.
	sup="$(cat <<'SUPEOF'
#!/usr/bin/env bash
# Remote job supervisor. Args: <jobroot> <job> <repodir> <timeout> <cmd>
set -u
JOBROOT="$1"; JOB="$2"; REPODIR="$3"; TMO="$4"; CMD="$5"
D="$HOME/$JOBROOT/$JOB"
if [ -f "$D/pgid" ] && kill -0 "-$(cat "$D/pgid")" 2>/dev/null; then
	echo "ALREADY_RUNNING pgid=$(cat "$D/pgid")"; exit 3
fi
rm -rf "$D"; mkdir -p "$D"
printf '%s\n' "$CMD" > "$D/cmd"
date -u +%Y-%m-%dT%H:%M:%SZ > "$D/started"
echo "$TMO" > "$D/timeout"
# setsid: own process group => we can kill the whole tree later.
# timeout --kill-after: the deadline is enforced HERE, not by the SSH client.
setsid bash -c '
	D="$1"; TMO="$2"; REPODIR="$3"; CMD="$4"
	echo $$ > "$D/pgid"
	cd "$HOME/$REPODIR" 2>/dev/null || cd "$HOME"
	timeout --signal=TERM --kill-after=30s "$TMO" bash -o pipefail -c "$CMD" \
		> "$D/out.log" 2>&1
	rc=$?
	# Record the result BEFORE any cleanup: a cleanup bug must never be able
	# to hide the job outcome (an earlier version pkill-ed its own group and
	# left status permanently UNKNOWN).
	echo "$rc" > "$D/rc"
	date -u +%Y-%m-%dT%H:%M:%SZ > "$D/ended"
	[ "$rc" = 124 ] && echo "TIMEOUT after ${TMO}s" >> "$D/out.log"
	# timeout(1) only signals its DIRECT child.  Backgrounded grandchildren
	# (make -j workers, detached stress binaries) otherwise survive and keep
	# consuming a possibly 192-vCPU box -- verified happening before this.
	# Kill every process in our group EXCEPT this supervisor and its own
	# pgrep/sleep helpers, so recording above always completes.
	me=$$
	victims=$(ps -eo pid=,pgid= | awk -v g="$me" -v s="$me" '"'"'$2==g && $1!=s {print $1}'"'"')
	[ -n "$victims" ] && kill -TERM $victims 2>/dev/null
	sleep 2
	victims=$(ps -eo pid=,pgid= | awk -v g="$me" -v s="$me" '"'"'$2==g && $1!=s {print $1}'"'"')
	[ -n "$victims" ] && kill -KILL $victims 2>/dev/null
	rm -f "$D/pgid"
' _ "$D" "$TMO" "$REPODIR" "$CMD" < /dev/null > "$D/sup.log" 2>&1 &
disown
sleep 1
echo "STARTED $JOB"
SUPEOF
)"
	b64="$(printf '%s' "$sup" | base64 -w0)"
	log "start job '$JOB' (timeout ${TMO}s) on $ROLE"
	rsh "mkdir -p ~/$JOBROOT && echo $b64 | base64 -d > ~/$JOBROOT/sup.sh && chmod +x ~/$JOBROOT/sup.sh && ~/$JOBROOT/sup.sh '$JOBROOT' '$JOB' '$REMOTE_DIR' '$TMO' $(printf '%q' "$CMD")"
	;;
status)
	JOB="${3:?job name}"
	rsh "D=~/$JOBROOT/$JOB; \
		if [ ! -d \$D ]; then echo 'NO_SUCH_JOB'; exit 0; fi; \
		if [ -f \$D/pgid ] && kill -0 -\$(cat \$D/pgid) 2>/dev/null; then \
			echo \"RUNNING since \$(cat \$D/started) pgid=\$(cat \$D/pgid) lines=\$(wc -l < \$D/out.log 2>/dev/null || echo 0)\"; \
		elif [ -f \$D/rc ]; then \
			echo \"DONE rc=\$(cat \$D/rc) started=\$(cat \$D/started) ended=\$(cat \$D/ended 2>/dev/null) lines=\$(wc -l < \$D/out.log 2>/dev/null || echo 0)\"; \
		else echo 'UNKNOWN (no rc, no live pgid -- supervisor died?)'; cat \$D/sup.log 2>/dev/null | tail -5; fi"
	;;
tail)
	JOB="${3:?job name}"; N="${4:-40}"
	rsh "tail -n $N ~/$JOBROOT/$JOB/out.log 2>/dev/null || echo '(no output yet)'"
	;;
wait)
	JOB="${3:?job name}"; POLL="${4:-30}"; MAXW="${5:-7200}"
	waited=0
	while :; do
		st="$(rsh "D=~/$JOBROOT/$JOB; if [ -f \$D/pgid ] && kill -0 -\$(cat \$D/pgid) 2>/dev/null; then echo RUNNING; elif [ -f \$D/rc ]; then echo DONE:\$(cat \$D/rc); else echo UNKNOWN; fi" 2>/dev/null || echo SSH_FAIL)"
		case "$st" in
			DONE:*) log "job '$JOB' $st"; echo "$st"; break ;;
			UNKNOWN) log "job '$JOB' state UNKNOWN"; echo UNKNOWN; break ;;
		esac
		if [ "$waited" -ge "$MAXW" ]; then
			log "job '$JOB' still $st after ${waited}s (local wait cap; job keeps running remotely)"
			echo "STILL_RUNNING"; break
		fi
		sleep "$POLL"; waited=$((waited + POLL))
	done
	;;
fetch)
	JOB="${3:?job name}"
	dest="$(cd "$DIR/../.." && pwd)/docs/results/jobs/${ROLE//@/-}-$JOB"
	mkdir -p "$dest"
	rsync -az -e "ssh $SSH_OPTS -i $KEY_FILE" \
		"${SSH_USER}@${DNS}:$JOBROOT/$JOB/" "$dest/" 2>/dev/null || true
	log "fetched -> $dest"
	;;
kill)
	JOB="${3:?job name}"
	# Kill the whole process group, not just the supervisor: otherwise
	# make -j / stress binaries keep running (and keep costing money).
	rsh "D=~/$JOBROOT/$JOB; if [ -f \$D/pgid ]; then kill -TERM -\$(cat \$D/pgid) 2>/dev/null; sleep 2; kill -KILL -\$(cat \$D/pgid) 2>/dev/null; echo KILLED; else echo 'not running'; fi"
	;;
list)
	rsh "for d in ~/$JOBROOT/*/; do [ -d \$d ] || continue; n=\$(basename \$d); \
		if [ -f \$d/pgid ] && kill -0 -\$(cat \$d/pgid) 2>/dev/null; then s=RUNNING; \
		elif [ -f \$d/rc ]; then s=\"rc=\$(cat \$d/rc)\"; else s=UNKNOWN; fi; \
		echo \"\$n \$s\"; done"
	;;
*)
	echo "unknown action: $ACTION" >&2; exit 1 ;;
esac
