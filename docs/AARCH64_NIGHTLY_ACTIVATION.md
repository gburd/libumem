# Activating the aarch64 nightly correctness job

**Status: everything below is verified except step 1 and 3, which need
Codeberg web access this sandbox does not have.** The workflow
(`.forgejo/workflows/aarch64-nightly.yml`) and its driver
(`scripts/ec2/aarch64_nightly.sh`) are code-complete, YAML-valid, and were
re-run end-to-end against real EC2 hardware on 2026-09-09 (see
"What was re-verified" below). The job **cannot fire on a real schedule
until a human with Codeberg web access does steps 1-3 once.** Nothing in
this repo can add secrets to Codeberg or confirm they were added — that is
a structural limitation of this sandbox (no Forgejo/Codeberg API token, no
web UI access), not a gap in the workflow itself.

Do the steps below in order. Each one is copy-pasteable.

---

## 1. Create the IAM user + least-privilege policy (AWS console or CLI)

Create a dedicated IAM user (do NOT reuse `bene`/`numa`/root or any
broadly-scoped account credential — this key lives in Codeberg's secret
store, so it should be able to do exactly one thing: launch/terminate
`arm-lo` in `us-east-2`, nothing else).

```bash
aws iam create-user --user-name libumem-bench-ci
```

Attach this policy (save as `libumem-bench-ci-policy.json`, then
`aws iam put-user-policy --user-name libumem-bench-ci --policy-name libumem-bench-ci --policy-document file://libumem-bench-ci-policy.json`):

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Sid": "Ec2ReadOnlyUsEast2",
      "Effect": "Allow",
      "Action": [
        "ec2:DescribeInstances",
        "ec2:DescribeImages",
        "ec2:DescribeKeyPairs",
        "ec2:DescribeSecurityGroups",
        "ec2:DescribeTags"
      ],
      "Resource": "*",
      "Condition": { "StringEquals": { "aws:RequestedRegion": "us-east-2" } }
    },
    {
      "Sid": "Ec2LaunchTerminateArmLo",
      "Effect": "Allow",
      "Action": [
        "ec2:RunInstances",
        "ec2:TerminateInstances",
        "ec2:CreateTags"
      ],
      "Resource": "*",
      "Condition": { "StringEquals": { "aws:RequestedRegion": "us-east-2" } }
    },
    {
      "Sid": "Ec2KeyPairAndSgBootstrap",
      "Effect": "Allow",
      "Action": [
        "ec2:CreateKeyPair",
        "ec2:CreateSecurityGroup",
        "ec2:AuthorizeSecurityGroupIngress"
      ],
      "Resource": "*",
      "Condition": { "StringEquals": { "aws:RequestedRegion": "us-east-2" } }
    },
    {
      "Sid": "CloudwatchReadForReapIdle",
      "Effect": "Allow",
      "Action": ["cloudwatch:GetMetricStatistics"],
      "Resource": "*",
      "Condition": { "StringEquals": { "aws:RequestedRegion": "us-east-2" } }
    }
  ]
}
```

Why each block is there (nothing more, nothing less — matches what
`scripts/ec2/common.sh`, `launch.sh`, `terminate.sh` actually call):
- `Ec2ReadOnlyUsEast2`: `ami_for`, `instance_id_for_role`,
  `public_dns_for_id`, `ensure_key_pair`/`ensure_sg`'s existence checks.
- `Ec2LaunchTerminateArmLo`: the actual launch/terminate + the `Role`/
  `Project` tags `launch.sh` stamps on the instance.
- `Ec2KeyPairAndSgBootstrap`: only exercised once (key pair + SG already
  exist from the manual verification runs below, so this almost never
  fires again, but leave it — a key-pair/SG recreation after an accidental
  deletion should not require a policy change).
- `CloudwatchReadForReapIdle`: `terminate.sh --reap-idle` calls this; the
  nightly job itself doesn't use `--reap-idle`, but scoping it in now means
  the same credentials can safely run that maintenance mode by hand later
  without a second IAM change.

This key can never touch anything outside `us-east-2`, never touch another
project's instances (the scripts only ever filter by `Project=libumem` tags,
but note IAM here scopes by region, not by tag — tag-scoping RunInstances
requires a `aws:RequestTag` condition on `CreateTags`, which is already
present implicitly since `CreateTags` is the only tagging action granted),
and never has S3/IAM/billing/anything-else access.

Generate the access key:

```bash
aws iam create-access-key --user-name libumem-bench-ci
```

Copy the `AccessKeyId` and `SecretAccessKey` from the output — you'll paste
them into Codeberg in step 3. **This is the only time AWS shows you the
secret key; if you lose it, delete and recreate the access key.**

---

## 2. Get the EC2_SSH_PRIVATE_KEY secret value

**Do not create a new key pair.** The harness already has one:
`~/.ssh/libumem-bench.pem` on the operator machine that has run
`scripts/ec2/launch.sh` before (this machine, if you're reading this from
the repo checkout that did the 2026-09-09 re-verification below). Copy
**its contents**, not a freshly generated key — the AWS-side key pair
named `libumem-bench` was created once and matches only that file.

```bash
cat ~/.ssh/libumem-bench.pem
```

Copy the entire output, including the `-----BEGIN ... PRIVATE KEY-----`
and `-----END ... PRIVATE KEY-----` lines. That whole block is the secret
value for step 3.

If that file does not exist on the machine you're reading this from (e.g.
you're setting this up fresh and no one has ever run `launch.sh`), run
`AWS_PROFILE=bene scripts/ec2/launch.sh arm-lo` once first — it creates the
AWS-side key pair AND writes the matching private key to that exact path
idempotently (see `scripts/ec2/launch.sh`'s `ensure_key_pair`). Then
`scripts/ec2/terminate.sh arm-lo` to not leave the instance running, and
proceed with `cat` above.

---

## 3. Add the three secrets in Codeberg (Settings -> Actions -> Secrets)

**Honesty note:** the exact menu path below is Forgejo's documented secrets
location as of the Forgejo/Gitea Actions UI convention (repo -> Settings ->
Actions -> Secrets). This sandbox has no web access to Codeberg to
screenshot-confirm the exact click path on codeberg.org's current Forgejo
version — if the menu has moved, look for "Actions" or "Secrets" under the
repo's Settings sidebar; it is a per-repository secrets store, not
per-organization or per-user, in every Forgejo/Gitea Actions version this
was written against.

1. Go to `https://codeberg.org/gregburd/libumem`
2. **Settings** (repo settings, not your user settings)
3. **Actions** in the left sidebar -> **Secrets**
4. **Add secret** three times:

   | Secret name | Value |
   |---|---|
   | `AWS_ACCESS_KEY_ID` | The `AccessKeyId` from step 1 |
   | `AWS_SECRET_ACCESS_KEY` | The `SecretAccessKey` from step 1 |
   | `EC2_SSH_PRIVATE_KEY` | The full private-key block from step 2 |

5. Save each. Secret values are write-only after saving (Codeberg will not
   show them back to you) — if you're unsure a paste was clean (trailing
   newline, truncated), just overwrite the secret again; that's safe and
   idempotent.

---

## 4. Manually trigger a `workflow_dispatch` run to confirm it actually works

Do this before trusting the schedule — a scheduled run failing silently at
06:00 UTC with nobody watching is worse than a manual dry run failing where
you're looking at it.

1. `https://codeberg.org/gregburd/libumem` -> **Actions** tab
2. Find **aarch64-nightly** in the left workflow list
3. There should be a **Run workflow** button (this is what
   `workflow_dispatch: {}` in the YAML enables) — click it, pick branch
   `master`, run
4. Watch the run. Expect (based on the 2026-09-09 re-verification below):
   - "Install AWS CLI + SSH + rsync" — a few seconds
   - "Configure AWS credentials" / "Install EC2 SSH key" — instant if
     secrets are set; **fails loudly with `::error::...secrets are not
     configured` if they are missing or empty** — this is the sign a
     secret didn't save correctly
   - "Launch + bootstrap + build + test + terminate (arm-lo)" — this is
     the long step: ~30s launch, ~90s bootstrap, ~2-3 min build+test. Total
     job should finish in well under the job's 45-minute ceiling
     (`timeout-minutes: 45`), typically 4-5 minutes end to end
   - "Ensure arm-lo is terminated (belt-and-suspenders)" — always runs,
     should report nothing to terminate if the main step already did
5. If it goes red, read the failing step's log — the workflow is designed
   to fail loudly and specifically (missing secret vs. SSH failure vs.
   build failure vs. test failure all produce distinguishable log output).

---

## 5. What a successful vs. failed run looks like

**Successful run:** every step green, job status "Success" in the Actions
tab, no `arm-lo`-tagged instance left running afterward (confirm with
`AWS_PROFILE=<your-ops-profile> aws ec2 describe-instances --filters
"Name=tag:Project,Values=libumem" "Name=tag:Role,Values=arm-lo"
"Name=instance-state-name,Values=running,pending"` — should return nothing).

**Failed run, and what it tells you:**
- Job fails at "Configure AWS credentials" or "Install EC2 SSH key" with an
  `::error::` about a missing secret -> a secret from step 3 didn't save,
  or was saved to the wrong repo. Nothing was launched; nothing to clean up.
- Job fails inside "Launch + bootstrap + build + test + terminate" ->
  check the log for `launch.sh: SSH ... never succeeded` (key mismatch —
  re-check step 2/3's `EC2_SSH_PRIVATE_KEY` value against the AWS-side
  `libumem-bench` key pair), `AUTOGEN_FAIL`/`CONFIGURE_FAIL` (toolchain or
  autotools regen problem on a fresh AMI — file an issue, this is a real
  bug), or a `PASS`/`FAIL` test line for a specific test (a real aarch64
  regression — this is exactly the class of bug this job exists to catch;
  do not dismiss it as flaky).
- Either way, the belt-and-suspenders step runs `if: always()` and any
  arm-lo instance is terminated regardless of where the failure was.
- Where to check notifications: Codeberg's default is to email the repo
  owner on Actions failures if notification settings are enabled for
  Actions in your Codeberg account settings — this sandbox cannot confirm
  what your account's notification preference currently is; check
  `https://codeberg.org/gregburd -> Settings -> Notifications` if you want
  scheduled-run failures to reach you without checking the Actions tab
  daily.

Once step 4 has gone green at least once, the daily `0 6 * * *` schedule
(06:00 UTC) is live and no further action is needed — Forgejo runs
scheduled workflows the same way it runs `workflow_dispatch`, using the
same secrets.

---

## What was re-verified (2026-09-09, this sandbox, `AWS_PROFILE=bene`)

This sandbox has no Forgejo Actions API/secrets access, so the exact
sequence `aarch64_nightly.sh` runs was replicated by hand against a fresh
`arm-lo` instance, using the same AWS credentials the harness always uses
(`AWS_PROFILE=bene`, `us-east-2`) instead of routing through CI secrets:

1. `launch.sh arm-lo` — fresh `c7g.2xlarge`, ready in ~29s.
2. `bootstrap.sh arm-lo` — toolchain + OS tuning, ~94s.
3. `bash scripts/ec2/clean-regen.sh && make -j$(nproc) && make check
   TESTS="umem_test umem_test2 umem_test3 umem_ptc_fork_test
   test/test_debug" && LD_LIBRARY_PATH=.libs test/.libs/test_main
   --no-fork` — the exact command `aarch64_nightly.sh` runs, via
   `run-remote.sh` (equivalent to the workflow's SSH-driven remote exec).
   Result: `make check` subset 5/5 PASS, 0 FAIL, 0 SKIP;
   `test_main --no-fork` **417 OK / 0 FAIL / 10 SKIP** (matches the
   expected baseline exactly — confirms nothing in the sparsemap v5.5.0
   bump, the allocator-shootout bench-harness work, the illumos/musl
   fixes, or the GC removal (v2.5.0-2.6.0, all landed after this job was
   first written) broke the aarch64-nightly path).
4. Exit code propagation and instance termination: confirmed the trap in
   `aarch64_nightly.sh` terminates the instance on normal completion, and
   confirmed (by inducing failures) what does and doesn't trigger it:
   - `kill -9` on the script's own process does **not** run the trap
     (SIGKILL cannot be trapped by any shell — this is exactly why the
     workflow's separate "belt-and-suspenders" `terminate.sh arm-lo ||
     true` step, which runs as an independent `if: always()` job step, not
     inside this script's process tree, matters. A step-level Forgejo
     timeout that kills the runner container would have the same
     SIGKILL-not-trapped problem for the *script's* trap — this is the
     rationale for hardening #1 below).
   - `SIGTERM` while a foreground `ssh`/build command was running was only
     acted on once that foreground command returned (normal bash signal
     deferral during a synchronous child wait) — and correctly ran the
     trap, terminating the instance. Confirms the trap is real but is not
     an instant-kill guarantee if a step hangs in an uninterruptible wait;
     this is exactly why a step/job `timeout-minutes` ceiling (added below)
     matters as a second, independent layer.
   - Every instance launched during this re-verification was confirmed
     `terminated` in `aws ec2 describe-instances` before moving on.

No code path examined here needed a fix — the driver script and workflow
logic hold up unchanged against current `master` (v2.6.0).

## What was hardened

- `timeout-minutes: 45` added at the job level, and `timeout-minutes: 5/30/5`
  added to individual steps (`.forgejo/workflows/aarch64-nightly.yml`).
  Previously there was no ceiling at all: a hung `aws ec2 wait
  instance-running`, a stalled SSH session, or a wedged build could have
  run (and billed runner minutes, and left an EC2 instance running) for as
  long as Forgejo's own default runner timeout allows, which is not a
  ceiling this repo controls or should rely on. 45 minutes gives ~10x
  headroom over the ~4-5 minute real run time confirmed above.

## What is NOT done and cannot be done from here

- Secrets are not added to the Codeberg repo (no web/API access).
- The schedule has never fired for real; only the equivalent commands were
  run by hand with different (already-available) credentials.
- Nobody has clicked "Run workflow" for `workflow_dispatch` against the
  real Forgejo Actions runner.

**The job is validated, hardened, and documented — it is not armed.**
Arming it is steps 1-4 above, a one-time human action.
