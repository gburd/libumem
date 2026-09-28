#!/usr/bin/env bash
# ship_prefix.sh <role>: git archive 6705310 + overlay 3816199's
# test/stress/repro_interpose_free_ratio.c into a fresh ~/libumem-r7qg-prefix (committed content only).
set -euo pipefail
R=/home/gburd/ws/libumem
source "$R/scripts/ec2/common.sh"
ROLE="$1"; IID="$(require_running_role "$ROLE")"; DNS="$(public_dns_for_id "$IID")"
S="ssh $SSH_OPTS -i $KEY_FILE ${SSH_USER}@${DNS}"
RD=libumem-r7qg-prefix
$S "mkdir ~/$RD"   # fails if it already exists: fresh box, fresh dir
git -C "$R" archive --format=tar 6705310 | $S "tar -xf - -C ~/$RD"
git -C "$R" show 3816199:test/stress/repro_interpose_free_ratio.c | $S "cat > ~/$RD/test/stress/repro_interpose_free_ratio.c"
$S "cd ~/$RD && printf 'base=6705310 (%s)\noverlay=test/stress/repro_interpose_free_ratio.c from 3816199 blob %s\nrole=%s\n' \
  '$(git -C "$R" rev-parse 6705310)' '$(git -C "$R" rev-parse 3816199:test/stress/repro_interpose_free_ratio.c)' '$ROLE' > PROVENANCE"
echo "shipped -> $DNS:~/$RD"
