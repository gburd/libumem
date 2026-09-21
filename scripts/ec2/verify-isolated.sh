#!/usr/bin/env bash
# scripts/ec2/verify-isolated.sh <role> <git-ref> <job> <timeout_s> "<command>"
#
# Run a verification against an ISOLATED snapshot of a git ref, so the result
# cannot be contaminated by other agents' uncommitted edits.
#
# WHY THIS EXISTS
#   run-remote.sh rsyncs the local WORKING TREE.  With several agents editing
#   one checkout, every "make check is green" from a plain run-remote.sh sync
#   is measuring a MIXTURE of everyone's in-flight edits, not the ref under
#   test.  This was observed on this project: one agent's verification silently
#   included another's half-finished umem.c, and a test failure could not be
#   attributed to any commit.
#
#   Worker-scoped roles isolate the INSTANCE.  This isolates the SOURCE.
#
# It ships `git archive <ref>` (committed content only -- no working-tree
# state, no other agent's edits) and builds that in its own remote directory,
# then runs the command under job.sh so it still gets a remote deadline,
# process-group cleanup, and persistent logs.
#
# Use this for any result you intend to report or commit as evidence.
#
#   ./scripts/ec2/verify-isolated.sh intel-lo@w1 HEAD v1 1800 \
#       './scripts/ec2/clean-regen.sh && make -j$(nproc) && make check'
#   ./scripts/ec2/verify-isolated.sh intel-lo@w1 abc1234 base 1800 '...'

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$DIR/common.sh"

ROLE="${1:?usage: verify-isolated.sh <role> <git-ref> <job> <timeout_s> \"<cmd>\"}"
REF="${2:?git ref (e.g. HEAD, a sha, a tag)}"
JOB="${3:?job name}"
TMO="${4:?timeout seconds}"
CMD="${5:?command}"

IID="$(require_running_role "$ROLE")"
DNS="$(public_dns_for_id "$IID")"
REPO_ROOT="$(cd "$DIR/../.." && pwd)"

SHA="$(git -C "$REPO_ROOT" rev-parse --verify "$REF")" || {
	echo "verify-isolated: '$REF' is not a valid git ref" >&2; exit 1; }
SHORT="${SHA:0:12}"
# Isolated per (worker, ref, job) so concurrent verifications never share a dir.
RDIR="libumem-iso-${JOB}-${SHORT}"

log "isolating $REF ($SHORT) -> $DNS:$RDIR"
TAR="$(mktemp -t libumem-iso-XXXXXX.tar)"
# git archive = committed content ONLY.  Deliberately not `rsync` and not
# `git stash`: the point is that no uncommitted state can leak in.
git -C "$REPO_ROOT" archive --format=tar "$SHA" > "$TAR"

ssh $SSH_OPTS -i "$KEY_FILE" "${SSH_USER}@${DNS}" \
	"rm -rf ~/$RDIR && mkdir -p ~/$RDIR"
# shellcheck disable=SC2002
cat "$TAR" | ssh $SSH_OPTS -i "$KEY_FILE" "${SSH_USER}@${DNS}" \
	"tar -xf - -C ~/$RDIR"
rm -f "$TAR"

# Record exactly what was tested, next to the logs, so a result can never be
# reported without its provenance.
ssh $SSH_OPTS -i "$KEY_FILE" "${SSH_USER}@${DNS}" \
	"printf 'ref=%s\nsha=%s\nrole=%s\njob=%s\nisolated=yes\n' \
		'$REF' '$SHA' '$ROLE' '$JOB' > ~/$RDIR/ISOLATED_PROVENANCE"

log "running '$JOB' against $SHORT (isolated; timeout ${TMO}s)"
# Run via job.sh's supervisor semantics but rooted in the isolated directory.
"$DIR/job.sh" "$ROLE" start "$JOB" "$TMO" "cd ~/$RDIR && { $CMD ; }"
echo "sha=$SHA dir=$RDIR"
