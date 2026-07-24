#!/usr/bin/env bash
# scripts/ec2/run-remote.sh <role> "<command>" - sync worktree to the role's
# instance, run <command> in the repo dir, stream output, then pull results
# (docs/results/, *.toml, *.csv) back locally.
#
# This is the workhorse: every heavy build/test/bench task in the plan runs
# through here so the local host is never loaded.
#
#   ./scripts/ec2/run-remote.sh intel-lo \
#       './autogen.sh && ./configure && make -j$(nproc) && \
#        LD_LIBRARY_PATH=.libs test/.libs/test_main --no-fork | tail -5'

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$DIR/common.sh"

ROLE="${1:?usage: run-remote.sh <role> \"<command>\"}"
CMD="${2:?usage: run-remote.sh <role> \"<command>\"}"
IID="$(require_running_role "$ROLE")"
DNS="$(public_dns_for_id "$IID")"

REPO_ROOT="$(cd "$DIR/../.." && pwd)"
REMOTE_DIR="libumem"

log "sync -> $DNS:$REMOTE_DIR"
rsync -az --delete \
	--exclude '.git' --exclude '*.o' --exclude '*.lo' --exclude '.libs' \
	--exclude '*.gcno' --exclude '*.gcda' --exclude 'test/core.*' \
	--exclude 'docs/results' \
	-e "ssh $SSH_OPTS -i $KEY_FILE" \
	"$REPO_ROOT/" "${SSH_USER}@${DNS}:${REMOTE_DIR}/"

log "run: $CMD"
set +e
ssh $SSH_OPTS -i "$KEY_FILE" "${SSH_USER}@${DNS}" \
	"cd ${REMOTE_DIR} && { $CMD ; }"
RC=$?
set -e
log "remote exit rc=$RC"

# Pull results back (created by bench/test tasks). Flatten directly into
# docs/results/ on the LOCAL side (the remote docs/results is excluded from
# the upload sync, so it only ever holds results this instance produced).
STAMP="$(date +%Y-%m-%d)-${ROLE}"
LOCAL_RES="$REPO_ROOT/docs/results"
mkdir -p "$LOCAL_RES"
log "pull results -> docs/results/"
rsync -az --exclude '.gitkeep' -e "ssh $SSH_OPTS -i $KEY_FILE" \
	"${SSH_USER}@${DNS}:${REMOTE_DIR}/docs/results/" "$LOCAL_RES/" 2>/dev/null || true
# Grab the instance meta.toml under a per-role name.
rsync -az -e "ssh $SSH_OPTS -i $KEY_FILE" \
	"${SSH_USER}@${DNS}:meta.toml" "$LOCAL_RES/${STAMP}-meta.toml" 2>/dev/null || true

exit $RC
