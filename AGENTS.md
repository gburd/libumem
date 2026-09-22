# AGENTS.md — working rules for libumem

Read this before touching anything. These rules come from real failures on
this project, not style preference.

## 1. Never build, test, or benchmark on the local host

The local machine (`floki`) is for editing and git only. Every compile, test,
stress run, and benchmark runs on EC2 through `scripts/ec2/`.

```sh
export AWS_PROFILE=hotdog          # verify first; see §2
./scripts/ec2/launch.sh    intel-lo@<worker>
./scripts/ec2/bootstrap.sh intel-lo@<worker>
./scripts/ec2/job.sh       intel-lo@<worker> start <job> <timeout_s> '<cmd>'
./scripts/ec2/terminate.sh intel-lo@<worker>
```

Roles: `intel-lo` (8 vCPU x86_64), `intel-hi` (192 vCPU metal x86_64),
`arm-lo` (8 vCPU aarch64), `arm-hi` (192 vCPU metal aarch64).

## 2. The AWS profile rotates. Verify, never assume

This burner account has rotated repeatedly (`numa` → `beef` → `bene` →
`hotdog`). At session start:

```sh
aws sts get-caller-identity          # if this fails, probe for a live profile
aws configure list-profiles
```

The account may contain instances belonging to other people. **Only ever
touch instances tagged `Project=libumem`.** Never use `--all` blindly, and
never `--reap-idle` without reading what it would kill.

Always terminate what you launch, and confirm with an empty:

```sh
aws ec2 describe-instances \
  --filters Name=tag:Project,Values=libumem \
            Name=instance-state-name,Values=running,pending \
  --query 'Reservations[].Instances[].[InstanceId]' --output text
```

## 3. Parallel agents must use worker-scoped roles

`run-remote.sh` rsyncs the worktree with `--delete`. Two agents sharing one
role's instance silently clobber each other's sources and logs. This has
already corrupted in-flight work on this project.

Use a distinct suffix per agent: `intel-lo@ptc`, `arm-hi@frag`. Each gets its
own instance and its own remote directory (`~/libumem-<worker>`).

## 4. Use `job.sh` for anything slow, not bare `run-remote.sh`

`run-remote.sh` keeps an SSH session open for the command's whole life. A
local timeout then orphans the remote work: it keeps running (on a possibly
192-vCPU box) while you believe the step ended. Re-running stacks concurrent
copies that fight over cores and produce fictitious results.

`job.sh` puts the deadline and the cleanup on the remote side:

- `timeout` runs remotely, so the limit survives SSH loss
- process-group cleanup, so `make -j` workers and stress binaries die with
  the job instead of being orphaned (verified: 0 survivors)
- logs in `~/libumem-jobs/<job>/`, outside the rsynced tree, so they survive
  the next sync and are readable after failure
- refuses to start a job that is already running
- every poll (`status`, `tail`) returns immediately

```sh
./scripts/ec2/job.sh intel-lo@w1 start build 1800 './scripts/ec2/clean-regen.sh && make -j$(nproc) && make check'
./scripts/ec2/job.sh intel-lo@w1 status build
./scripts/ec2/job.sh intel-lo@w1 tail   build 40
./scripts/ec2/job.sh intel-lo@w1 fetch  build     # logs -> docs/results/jobs/
./scripts/ec2/job.sh intel-lo@w1 kill   build
```

Always run `clean-regen.sh` in the **same** job as `make`. A separate
invocation re-syncs and leaves stale generated autotools files that fail
with local `aclocal-1.18`/`missing` paths.

## 5. Preserve original evidence before re-running

When something fails, `fetch` the logs before rebuilding, re-syncing, or
retrying. A later pass does not explain an earlier failure, and re-running
destroys the only record. An undiagnosed 7/8 test failure was lost this way.

Never attribute a failure to a cause you have not shown. "Probably variance"
is not a diagnosis.

## 6. Evidence rules

- A passing test proves only what it actually executes. `make check` runs 8
  entries; it is not the comprehensive suite.
- Distinguish PASS, SKIP, FAIL, and never-executed. Returning 0 for an
  unavailable prerequisite is a bug, not a skip.
- Record what produced a number: commit, configure flags, instance type,
  library versions, binary identity.
- Do not claim a fix works without a check that fails before it and passes
  after.
- Entering a code path is not the same as that path doing its job. The rseq
  assembly is called constantly and still serves zero magazine hits.

## 7. Code and comment standards

- Comments state invariants: which lock protects what, who owns an object,
  what lifecycle transitions are legal. Not narration, not confidence.
- If a comment claims something the code does not do, that is a bug. Several
  exist today (fork lock order, "flush all bins").
- Fix root causes at the shared function, not per caller. **And then grep for
  siblings.** Fixing one function and claiming the class is closed is the most
  common failure here: the v3.0.0 `errno` fix landed in
  `vmem_mmap_top_alloc()` while `vmem_mmap_alloc()` kept the identical defect,
  and the release notes said "failure paths now leave errno alone." Before
  claiming a defect class is fixed, search for the same pattern and state what
  you found.
- Mark deliberate shortcuts with `ponytail:` naming the ceiling and upgrade
  path.
- Do not ship unsound code to hit a number. Document honestly instead.

## 7a. Security rules

libumem ships `libumem_malloc.so` as an `LD_PRELOAD` drop-in, so it can be
loaded into setuid binaries, root daemons, and network-facing servers. Assume
all three.

- **State the attacker position.** A security finding means nothing without
  one. Use: (A) setuid/setgid target, (B) root daemon, (C) attacker controls the
  environment but not the code, (D) attacker controls allocation patterns and
  buffer contents but not the environment. "Could be bad" is not a finding.
- **Never add a file, socket, or exec side effect that an environment variable
  can trigger**, without gating it on privilege. `execlp` and any other
  `PATH`-resolving exec are forbidden in library code: use an absolute path or
  do not do it.
- **File writers get `O_EXCL`/`O_NOFOLLOW`** or an explanation of why the path
  cannot be attacker-influenced. No library writer in this tree had either as
  of v3.0.0.
- **Sockets do not go in shared directories under predictable names**, and
  `stat`-then-act on a filesystem path is a TOCTOU unless you can say why not.
- **Peer authorization uses `geteuid()`**, not `getuid()`: for a setuid target
  the real uid is the unprivileged invoker.
- **Compare against glibc.** "Same as glibc" is acceptable; "worse than glibc"
  is a finding that needs a decision. libumem's unmangled in-buffer freelist
  links are currently worse (P5.4).
- **A hardening change needs a regression that demonstrates the exposure it
  closes.** Security fixes are exactly where an unverified claim is most
  dangerous, so the pre-fix demonstration rule applies with no exceptions.
- Correctness work and hardening work are different disciplines. Passing the
  correctness gate says nothing about hostile input.

## 8. `docs/` is durable

`docs/` holds plans, results, and runbooks under version control. Generated
Doxygen output goes to `doxygen-out/`. `make clean` must never delete
`docs/` — it used to, which is how the documentation tree was lost.

## 9. Git

Codeberg remote `origin`, branch `master`, fast-forward only, never
force-push. Conventional commits. Commit promptly so parallel agents do not
lose work; a mid-session reset by another agent has already destroyed
uncommitted edits.

### Shared-worktree rules (all learned from real damage here)

This is ONE working tree with several agents editing it at once. Therefore:

- **Never `git add -A`, `git add .`, or `git commit -a`.** Add your own paths
  explicitly. An `git add -A` swept another agent's half-finished header into
  an unrelated commit and produced a broken-build sha on `master`.
- **On a file another agent is also editing, `git add <path>` is NOT enough
  either** — it stages the whole file, including their uncommitted hunks. This
  happened on `umem.c`: an agent staged only its own path and still swept a
  second agent's in-flight `umem_secure_mode()` gate into its commit, briefly
  breaking the build. On a contended file use `git add -p`, or stage an explicit
  blob, and check `git diff --cached` before committing. If you discover you
  swept someone else's work, commit a correction on top — do not reset or amend.
- **Never `git reset`** (soft or hard), and never `git checkout -- .`. Another
  agent's commit can be HEAD at any moment; a `git reset --soft HEAD~1`
  removed someone else's commit from the branch (recovered from reflog). To
  undo your own commit, `git revert` it or commit a correction on top.
- **Never amend or rewrite a commit that is not the tip you just made**, and
  never rewrite once anyone has committed on top. A bad sha in the log is much
  cheaper than rewritten shared history.
- **Re-read a file before editing it** if another agent may own part of it; a
  commit may have landed since you last looked.
- **Verify against committed refs, not the working tree.** Use
  `scripts/ec2/verify-isolated.sh <role> <ref> ...`, which ships
  `git archive` (committed content only). A plain `run-remote.sh` sync
  measures everyone's in-flight edits at once, and results obtained that way
  cannot be attributed to any commit.
- **Do not verify against a tree with another agent's files reverted.** That is
  its own contamination. If their commit blocks you, say so and wait or fix
  the specific breakage.
- If you break the shared build, fix it or report it immediately. Everyone
  else is blocked until you do.

## 10. Current state: v3.0.0, with a security phase open

The correctness work is done: the ten reachable defects the 2026-09-21 review
found in default paths are fixed, each with a demonstrated pre-fix failure on
both architectures, and the measurement machinery that hid them is repaired.

**What is NOT done is security hardening.** The 2026-09-22 audit of `v3.0.0`
found one Critical and three High issues, including a `PATH`-resolved
`execlp("addr2line")` on the unconditional init path with no privilege gate.
See Phase 5 in `docs/plans/2026-09-21-production-readiness.md`.

Until Phase 5's exit criteria are met:

- libumem is **not** for privileged (setuid/root) or network-facing use.
- Do not soften the README's status banner.
- Do not add performance features.

The known ~5 GB Linux heap ceiling also remains open, with an honest record of
a failed fix attempt.
