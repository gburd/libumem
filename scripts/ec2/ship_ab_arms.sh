#!/usr/bin/env bash
# scripts/ec2/ship_ab_arms.sh <role> <pre-ref> <post-ref> [remote-dir]
#
# Lay out an A/B directory on a worker: ~/<dir>/{pre,post,pre2} from
# `git archive` of the two refs (pre2 is an independent copy of pre for the
# null control).  Committed content only, same reason as verify-isolated.sh.
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$DIR/common.sh"
ROLE="${1:?role}"; PRE="${2:?pre ref}"; POST="${3:?post ref}"; RDIR="${4:-libumem-ab}"
IID="$(require_running_role "$ROLE")"; DNS="$(public_dns_for_id "$IID")"
REPO_ROOT="$(cd "$DIR/../.." && pwd)"
rsh() { ssh $SSH_OPTS -i "$KEY_FILE" "${SSH_USER}@${DNS}" "$@"; }
rsh "rm -rf ~/$RDIR && mkdir -p ~/$RDIR/pre ~/$RDIR/post ~/$RDIR/pre2"
for pair in "pre:$PRE" "post:$POST" "pre2:$PRE"; do
	sub="${pair%%:*}"; ref="${pair#*:}"
	sha="$(git -C "$REPO_ROOT" rev-parse --verify "$ref")" || exit 1
	git -C "$REPO_ROOT" archive --format=tar "$sha" | rsh "tar -xf - -C ~/$RDIR/$sub"
	rsh "echo $sha > ~/$RDIR/$sub/SHA"
	log "$sub <- $ref ($sha)"
done
