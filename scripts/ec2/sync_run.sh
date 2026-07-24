#!/usr/bin/env bash
# Local helper: sync source (no docs/results, no --delete) to an EC2 host and
# run a command via ssh, bypassing run-remote.sh's results-pull (which nests
# infinitely when another workstream has synced docs/results up).
# Usage: sync_run.sh <public-dns> "<remote command>"
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$DIR/common.sh"
DNS="${1:?usage: sync_run.sh <dns> <cmd>}"
CMD="${2:?usage: sync_run.sh <dns> <cmd>}"
REPO_ROOT="$(cd "$DIR/../.." && pwd)"
rsync -az --exclude '.git' --exclude '*.o' --exclude '*.lo' --exclude '.libs' \
	--exclude 'docs/results' --exclude '*.gcno' --exclude '*.gcda' \
	-e "ssh $SSH_OPTS -i $KEY_FILE" \
	"$REPO_ROOT/" "${SSH_USER}@${DNS}:libumem/"
ssh $SSH_OPTS -i "$KEY_FILE" "${SSH_USER}@${DNS}" "cd libumem && { $CMD ; }"
