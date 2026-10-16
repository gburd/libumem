# AI working-style rules for libumem

Generic working-style and evidence rules for any agent (Copilot, Pi, Kiro,
Claude, Maki) touching this repository. These are the discipline rules that are
not specific to libumem's infrastructure. The project-specific direction --
the EC2/burner story, the AWS-profile rotation, the shared-worktree git rules,
the security threat model, and the current release state -- lives in
[`../AGENTS.md`](../AGENTS.md). The build/test/benchmark runbook lives in
[`skills/building_and_testing.md`](skills/building_and_testing.md).

These rules come from real failures on this project, not style preference.

## Working style

- Read the relevant code before proposing or making a change. Keep changes
  focused on the task; preserve the existing structure, layers, and
  conventions; avoid unrelated renames or refactoring.
- Match validation effort to the risk and scope of the change. Establish a
  relevant baseline before broad or high-risk work; a small change may use
  targeted validation. When failure attribution is unclear, measure against a
  committed ref, not the dirty worktree (`scripts/ec2/verify-isolated.sh`,
  AGENTS.md §9).
- Finish approved work through its validation and a bounded self-review before
  reporting it done. Use independent review when the breadth, risk, or
  complexity warrants it.
- Report incidental findings separately, unless fixing one is necessary to make
  the requested change correct. Do not fold an unrelated fix into the diff.
- Challenge an instruction or an assumption when the evidence contradicts it.
  "You asked for X, but the code shows Y" is more useful than silently doing X.

## Evidence and debugging

- **Verify a hypothesis before implementing a workaround.** Inspect the actual
  data, write a minimal reproducer, or compare against a known-good revision
  first. A workaround built on a guessed cause hides the real defect.
- **Establish a baseline before a broad change.** You cannot claim a change
  helped, or did no harm, without the before number. For performance that is
  the null-first A/B (`hotpath_ab.sh`); for correctness it is a demonstrated
  pre-fix failure.
- **Do not call a failure transient.** Treat every failure as actionable until
  there is evidence otherwise; a rerun that passes is not that evidence.
  "Probably variance" is not a diagnosis, and never attribute a failure to a
  cause you have not shown. An undiagnosed 7/8 test failure was lost on this
  project by re-running before preserving the evidence (AGENTS.md §5).
- **Grep for sibling bugs.** When a bug pattern can occur more than once, fix
  the root cause once in the shared function, then search the whole tree for
  the same pattern and fix every instance together. Fixing one site and
  declaring the class closed is the most common failure here: the v3.0.0
  `errno` fix landed in `vmem_mmap_top_alloc()` while `vmem_mmap_alloc()` kept
  the identical defect, and the release notes claimed "failure paths now leave
  errno alone." Before claiming a defect class is fixed, state what the search
  found.
- **Entering a code path is not the same as that path doing its job.** The rseq
  assembly is called constantly and still served zero magazine hits. A test
  that reaches a line proves only that the line was reached.
- Start by checking new or changed code, but do not conclude the shared build
  (`master`) is broken from a local failure alone. Check CI and, when useful,
  reproduce against a committed ref.

## Tracing inside the allocator

- **Allocator tracing must use `write()` directly** to stderr or a file, never
  `printf`, `fprintf`, or any libc call that may itself call `malloc` and
  recurse through the allocator under diagnosis. libumem learned this in
  `umem_fail.c` (P5.16): the failure-reporting path cannot allocate. A trace
  that recurses either deadlocks or corrupts the state you are trying to read.

## Change discipline

- Avoid over-engineering and review churn. Preserve names and function
  boundaries unless changing them improves correctness or clarity. Do not add
  abstractions, options, or extension points for hypothetical requirements.
- Prefer designs simple enough that their correctness is evident. The absence
  of an obvious bug in a complicated design is not evidence that the design is
  correct.
- Update comments, tests, and documentation the change makes stale. Comments
  state current invariants -- which lock protects what, who owns an object,
  what lifecycle transitions are legal -- not the history of the change, not
  narration, not confidence. A comment that claims something the code does not
  do is a bug (AGENTS.md §7).
- Mark deliberate shortcuts with a `ponytail:` comment naming the ceiling and
  the upgrade path. Do not ship unsound code to hit a number; document the
  limit honestly instead.
- Code you move or merge keeps its error handling and validation. Never cut
  validation at a trust boundary, error handling that prevents data loss, or
  security checks to make a diff smaller.

## Plans

- Use a written plan for genuinely multi-stage work, or when the user asks for
  one. This project stores plans in `docs/plans/<date>-<name>.md` under version
  control (AGENTS.md §8), one per round of work; snmalloc's `PLAN.md`
  convention is the single-file equivalent. Obtain approval before implementing
  a plan that introduces significant design choices.
