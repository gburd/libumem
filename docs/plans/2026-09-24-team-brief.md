# Team brief: v3.2.0 → next (2026-09-24)

Read `AGENTS.md` first; everything there is binding. This file adds the rules
specific to this round. Every agent reads it in full.

## Non-negotiable process

- **EC2 only.** `export AWS_PROFILE=hotdog`; verify with `aws sts
  get-caller-identity` (account 585335547908). Your worker suffix is in your
  brief; use it for every role (`intel-lo@<suffix>`, `arm-lo@<suffix>`, and
  `intel-hi@<suffix>` ONLY where your brief says metal is needed, terminated
  the moment that run ends). Metal is $14/hour.
- **`job.sh start/status/tail` with short polls** for every build, test and
  run. Never a foreground `run-remote.sh` longer than a few seconds. Three
  agents on this project have died holding a long call.
- **Keep your model turns short.** Two agents this week died on provider
  timeouts after a sequence of large file reads. Prefer several `sed -n
  A,Bp` reads of 40-60 lines over one big one. Commit each verified piece
  immediately so a timeout loses nothing.
- **Reported numbers come from `verify-isolated.sh <role> <ref> <job> <timeout>
  "<cmd>"`**, which ships `git archive <ref>` (committed content only). Pass
  multi-line commands as `echo <base64> | base64 -d > /tmp/x.sh && bash
  /tmp/x.sh`; a `$(cat file)` argument silently becomes empty. Run
  `clean-regen.sh` in the SAME job as `make`.
- **Shared worktree.** Never `git add -A`, never `git reset`, never amend a
  commit that is not the tip you just made. `umem.c` is contended: `git add
  -p` or explicit paths, then `git diff --cached` before every commit.
  Re-read a file before editing it.
- **Terminate what you launch** and confirm with `aws ec2 describe-instances
  --filters Name=tag:Role,Values=*<suffix>*` showing nothing. If you die
  before that, the coordinator sweeps; do not make that necessary.

## Evidence standard (AGENTS.md §6, restated because it keeps being missed)

- A fix needs a regression that **FAILS at the parent commit and PASSES
  after**, both actually run. Commit the test alone first so the pre-fix run
  is against committed content. A test that passes on pre-fix code is
  vacuous; several agents have caught their own tests this way.
- **Isolate before you claim.** When a change has two parts, build and
  measure them separately. This round's P6.3 agent shipped two changes in one
  commit; one was a footprint win and the other was a 28x cliff, and it took
  five builds to tell them apart.
- **A/B on the fast path** means alternating PRE/POST builds on the same box
  in the same minute, median of 7 or 9, at t=1 AND t=8, and the result must be
  inside the null control's spread. Read `ae86536`'s commit message for the
  method and for what a 5 % regression looks like when caught.
- **`bench_main -w single` at t=1 is NOT an allocator microbench.** 70 % of
  its cycles are its own t-digest histogram and the vDSO clock; it moved 7 %
  on a change that made the allocator faster. Use a bare alloc/free loop
  (`test/bench/` has none yet; write one, ten lines) or `perf stat`
  instructions-per-op for t=1 questions.
- **Gate before done:** `scripts/ec2/exit_criteria_gate.sh` via
  `verify-isolated.sh` on intel-lo AND arm-lo, default AND
  `--enable-introspect`. Plus the exact PTC oracles if you touched
  `umem.c`'s PTC or depot paths: `test_ptc_thread_exit_drain` (P1.3a
  stranded = 0), `test_ptc_resize_no_loss_probe` x12 (P1.3c ledger 0),
  `test_cpu_hint_spread`, `test_ptc_footprint`, and
  `scripts/ec2/oracle_matrix.sh` default + asan.

## Writing standard (AGENTS.md §7)

Comments state invariants: which lock protects what, who owns an object, what
transitions are legal. Not narration, not confidence, not "this optimizes".
Where a comment records a measurement, it names the box, the sha and the
number. Where a first diagnosis was wrong, the record says so; nothing is
softened after the fact. Plan entries get `**STATUS:**` with an evidence
table, in the voice of the existing entries. CHANGELOG entries go under
`## [Unreleased]` (create it above `## [3.2.0]`) in the same voice.

## Coordination

- Hot-path work (`umem.c` 2400-4100: depot, PTC magazines, `_umem_alloc`,
  `_umem_free`) belongs to ONE agent this round (`@hot`). Nobody else edits
  those lines. If your task needs a change there, write it up in your report
  and stop.
- `malloc_interpose.c` / `malloc.c` belong to `@interp`.
- `umem_fork.c`, `umem_cache_create/destroy/applyall` and
  `umem_cache_lock` users belong to `@cache`.
- Review agents (`@review-*`) are READ-ONLY on source. They write findings to
  `docs/reviews/2026-09-24-<topic>.md` and nothing else. A finding without a
  file:line and (for security) an attacker position is not a finding.
