# AGENTS.md — working rules for libumem

Read this before touching anything. These rules come from real failures on
this project, not style preference.

## See also

This file holds the **project-specific** direction: the EC2/burner story, the
AWS-profile rotation, the shared-worktree git rules, the security threat model,
the libumem-specific evidence rules, and the current release state. The rest is
split out so it can be shared and kept current:

- **[`.github/copilot-instructions.md`](.github/copilot-instructions.md)** --
  the generic working-style and evidence rules (verify-before-workaround,
  establish-a-baseline, don't-call-failures-transient, grep-for-sibling-bugs,
  trace-via-`write()`-not-`printf`, keep-changes-focused, challenge-wrong-
  assumptions, report-incidental-findings-separately, the plans convention).
- **[`.github/skills/building_and_testing.md`](.github/skills/building_and_testing.md)**
  -- the build/test/benchmark runbook (`clean-regen.sh` + `make` in one job,
  `make check` is a smoke suite, `exit_criteria_gate.sh`, `hotpath_ab.sh`
  null-first A/B, the P8.3 quiet-rig gate, the `job.sh` pattern, which test
  targets mean what, `XFAIL_TESTS` discipline).

Kiro, Pi, Claude, and Maki all read this `AGENTS.md`; the shared
`copilot-instructions.md` is the single generic-rules file (no per-agent
mirror is maintained in this repo).

## 1. Never build, test, or benchmark on the local host

The local machine (`floki`) is for editing and git only. Every compile, test,
stress run, and benchmark runs on EC2 through `scripts/ec2/`. The build/test
runbook is in `.github/skills/building_and_testing.md`.

```sh
export AWS_PROFILE=kiwi             # verify first; see §2
./scripts/ec2/launch.sh    intel-lo@<worker>
./scripts/ec2/bootstrap.sh intel-lo@<worker>
./scripts/ec2/job.sh       intel-lo@<worker> start <job> <timeout_s> '<cmd>'
./scripts/ec2/terminate.sh intel-lo@<worker>
```

Roles: `intel-lo` (8 vCPU x86_64), `intel-hi` (192 vCPU metal x86_64),
`arm-lo` (8 vCPU aarch64), `arm-hi` (192 vCPU metal aarch64). Debian instances
only (`EC2_OS=debian`).

## 2. The AWS profile rotates. Verify, never assume

This burner account has rotated repeatedly (`numa` → `beef` → `bene` →
`hotdog` → `kiwi`); the profile name itself has rotated across accounts
(373102893032 -> 170848442262 -> kiwi/830118047484 -> kiwi/505435643998 (the burner is replaced wholesale, same profile name; old account + its boxes are torn down on rotation)
2026-10-10). The account also holds the maintainer's OTHER projects' instances
(libdb, libxtc, undo, dbsql, noxu-bench m8id/m8gd metal, an unnamed c7i,
freebsd audits), tagged `Owner=gburd` but NOT `Project=libumem`. That is
expected, not a stranger's: still, only ever touch `Project=libumem` **and**
the worker role you launched. At session start:

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

## 4. Build and test on EC2 → see the building_and_testing skill

The full build/test/benchmark runbook lives in
[`.github/skills/building_and_testing.md`](.github/skills/building_and_testing.md).
The one rule that must never be broken, repeated here because it is
project-critical:

**Use `job.sh` for anything slow, not bare `run-remote.sh`.** `run-remote.sh`
keeps an SSH session open for the command's whole life; a local timeout then
orphans the remote work (it keeps running on a possibly 192-vCPU box while you
believe the step ended), and re-running stacks concurrent copies that fight
over cores and produce fictitious results. `job.sh` runs `timeout(1)` and the
process-group cleanup on the remote side, logs outside the rsynced tree, and
refuses to start a job already running.

Always run `clean-regen.sh` in the **same** job as `make`; a separate
invocation re-syncs and leaves stale generated autotools files that fail with
local `aclocal-1.18`/`missing` paths.

## 5. Preserve original evidence before re-running

When something fails, `fetch` the logs before rebuilding, re-syncing, or
retrying. A later pass does not explain an earlier failure, and re-running
destroys the only record. An undiagnosed 7/8 test failure was lost this way.

Never attribute a failure to a cause you have not shown. "Probably variance"
is not a diagnosis.

## 6. Evidence rules

- A passing test proves only what it actually executes. `make check` runs ~8
  entries; it is not the comprehensive suite (see the building_and_testing
  skill and `test/README.md`).
- Distinguish PASS, SKIP, FAIL, and never-executed. Returning 0 for an
  unavailable prerequisite is a bug, not a skip.
- Record what produced a number: commit, configure flags, instance type,
  library versions, binary identity.
- Do not claim a fix works without a check that fails before it and passes
  after.
- Entering a code path is not the same as that path doing its job. The rseq
  assembly is called constantly and still serves zero magazine hits.

## 7. Code and comment standards → generic rules in copilot-instructions.md

The generic code-quality rules (keep changes focused, fix root causes at the
shared function **then grep for siblings**, comments state invariants not
narration, trace via `write()` not `printf`) are in
[`.github/copilot-instructions.md`](.github/copilot-instructions.md). The
project-specific standing points:

- If a comment claims something the code does not do, that is a bug. Several
  have existed in this tree (fork lock order, "flush all bins").
- The grep-for-siblings rule has a scar here: the v3.0.0 `errno` fix landed in
  `vmem_mmap_top_alloc()` while `vmem_mmap_alloc()` kept the identical defect,
  and the release notes claimed the class was closed. Before claiming a defect
  class is fixed, search for the same pattern and state what you found.
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
- **The index is shared too: plain `git commit` commits everything anyone has
  staged.** `git add <your path>` followed by `git commit` swept four files
  another agent had already staged into an unrelated commit (`286cd75`). Always
  commit with explicit paths (`git commit <paths>`, which implies `--only`), and
  run `git diff --cached --stat` immediately before committing: anything listed
  that you did not stage is someone else's.
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

## 10. Current state: v4.3.0

The correctness work is done: the ten reachable defects the 2026-09-21 review
found in default paths are fixed, each with a demonstrated pre-fix failure on
both architectures, and the measurement machinery that hid them is repaired.
The Linux ~5 GB heap ceiling is fixed (v3.2.0, both halves: UMEM_MIN_SLAB_*
and UMEM_MIN_QCACHE_SLAB). The 2026-09-26 tool review (T1-T14) is closed:
the live-heap debugging tools -- gdb/lldb `umem break`/`umem events`,
`umem_inspect_set_event_cb()`, `umemctl break` -- fire on every allocation
path with no fast-path cost while disarmed (v3.3.3). The vendored sparsemap
is gone and the soname is `libumem.so.2` (v4.0.0); `libumem_malloc.so.1` is
unchanged.

**Phase 5 security hardening is complete.** The 2026-09-22 audit's Critical
and three High issues, the 2026-09-24 audit of the v3.2.0 delta, and the two
v3.3.1 follow-ups are all fixed, each with a regression that demonstrates the
pre-fix exposure and passes after, on x86_64 and aarch64. See the Phase 5
status table in `docs/plans/2026-09-21-production-readiness.md`; every row is
FIXED and the exit criteria are met.

**Portability layer and hardware validation (v4.0.0-v4.2.2).** A PAL/AAL
abstraction (`pal/`, `aal/`) was carved out as a behaviour-preserving refactor
(fast path +0.00 % both arches); snmalloc was studied, two items adopted (PAL,
getpcstack new-arch frame walks) and two declined with ADRs (remote-free,
involution encoding -- they re-architect libumem). The allocator is now
hardware-validated beyond x86_64/aarch64:
- **UltraSPARC/illumos (host `sun`, v4.2.2):** builds as a real SPARCV9 object,
  umem_test 1/2/3 PASS, interposer works, full `gmake` completes (test
  Linux-isms fixed). `gmake check` 43P/19S/8F/3E -- the 11 FAIL/ERROR are
  Linux-specific harnesses (gdb/Python debugger checks, Linux signal/VMA
  assumptions), not allocator bugs.
- **RISC-V (host `lixr`, Spacemit X100, v4.3.0):** the lock-free rseq asm is
  WRITTEN, hardware-validated, and SHIPPED. The signal-storm SEGV was a wrong
  RSEQ signature (0x53053053 instead of RISC-V's 0xf1401073 = `csrw mhartid,
  x0`); glibc owns the rseq area and the kernel SIGSEGVs on an abort-signature
  mismatch. Fixed in v4.3.0, validated under the signal storm
  (repro_rseq_trailing_store + repro_reload_commit_abort PASS, umem_test 1/2/3
  PASS, rseq engages). `docs/results/r12/riscv-rseq-hardware.md`.

- Performance work is allowed. Every change still needs its before/after A/B
  (`hotpath_ab.sh`) with the fast path inside the null, or the cost recorded
  and justified. Soundness is not traded for a number (AGENTS.md 7).
- The README's honest framing stays: this was a hardening pass, not a security
  proof. It is `malloc`, so the surface is broad. Keep saying what has been
  shown and what has not.
- A privileged (setuid/root) or network-facing deployment is now defensible on
  the specific exposures the audits found, but that is not the same as proven
  safe against an unaudited class. Do not overstate it.

Owed, recorded honestly: porting the 11 Linux-specific test harnesses to
illumos (a separate, larger effort); the green CI/Pages badge (needs Codeberg
Actions enabled + a registered Forgejo runner -- a repo-settings step; the
workflows build green locally, docs/results/r11/task1-ci-pages-local-validation.md).
The x86 metal benchmark half of comparison-v3 is unrun: the arm half + both-arch
c1 are done and show no PAL/AAL drift from v2, and the x86 metal launch
intermittently hits the shared burner account's vCPU quota (other projects'
instances consume it; headroom comes and goes). The lock-free RISC-V rseq asm
is now SHIPPED (v4.3.0) -- no longer owed.

Open work is tracked in the round plans (`docs/plans/2026-09-28-round8.md`,
`docs/plans/2026-10-16-adopt-from-snmalloc.md`, and earlier), not here.
