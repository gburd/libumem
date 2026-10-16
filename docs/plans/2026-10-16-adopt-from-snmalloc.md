# Round 10+: Adopt from snmalloc (master plan)

User directive (2026-10-16): adopt seven things from snmalloc into libumem,
studying ~/src/snmalloc, ~/ws/libdb (abstraction layer), ~/ws/libxtc (docs
infra). Source study report: /tmp/snmalloc-lessons-for-libumem.md.

## HARD CONSTRAINT: no EC2 right now
The `hotdog` burner expired 2026-10-10 14:43Z. As of 2026-10-16,
`aws sts get-caller-identity` returns NoCredentials. AGENTS.md §1 forbids
building/testing/benchmarking on floki. Therefore every item that needs a
compile, a test run, or a benchmark is BLOCKED on a fresh account and must be
designed/staged now, validated later -- never claimed done until tested on
EC2. Items that are pure editing/design/docs proceed now.

Classification:
- NO-EC2 (do now): item 0 (agent-rules reorg), item 3 docs scaffolding +
  content, item 6 (evaluation/roadmap doc), the DESIGN half of items 1/4/5.
- EC2-BLOCKED (design now, validate later): item 1 PAL/AAL must compile on
  every target; item 2 CI must run; item 4 F3 needs the metal A/B; item 5
  involution encoding needs the hot-path A/B + gates.

Each EC2-blocked item carries a "validation owed" list so a future session
with credentials can finish it without re-deriving the plan.

---

## Item 0 [NO-EC2, do now] -- Reorganize agent rules (snmalloc's split model)
snmalloc: `.github/copilot-instructions.md` (generic working style) delegates
to `.github/skills/building_and_testing.md` (repo build/test runbook) and a
`PLAN.md` convention for multi-stage work. libxtc: `AGENTS.md` +
`.agent-steering-domains.md` + a rich `docs/`.
TARGET for libumem:
- `AGENTS.md` keeps ONLY project-specific direction (the EC2 story, the AWS
  burner rotation, the shared-worktree git rules, the current-state section)
  and REFERENCES OUT to the rest.
- `.github/copilot-instructions.md` + mirror for the agents actually used
  (Pi/Kiro/Claude/Maki per the modules/home-manager/ai setup): the GENERIC
  working-style/evidence rules (verify-before-workaround, baseline, don't-
  call-transient, grep-for-siblings, trace-via-write-not-printf).
- `.github/skills/building_and_testing.md`: the libumem build/test/benchmark
  runbook (clean-regen, make check caveats, exit_criteria_gate, hotpath_ab,
  the EC2 job.sh story, the quiet-rig P8.3 gate, which targets mean what).
- Ensure libumem has an equivalent of every snmalloc rule. Gap check below.
Validation owed: none (docs only). Done when the split is committed and
AGENTS.md cross-references resolve.

## Item 1 [EC2-BLOCKED design-now] -- PAL + AAL abstraction layer
Combine snmalloc's `pal/`+`aal/` (C++ Concept-checked, ~21 OSes) with libdb's
`src/os/os_<operation>.c` + per-OS override dirs (os_qnx, os_windows) pattern,
rendered in libumem's C idiom. Collect today's scattered platform code
(getpcstack.c, x86_subr_compat.c, i386_subr_sol.s, vmem_mmap.c, init_lib.c,
the __linux__/__FreeBSD__ #ifdefs in umem.c) behind one `pal/` + `aal/`.
REQUIRED TARGETS (user): illumos/UltraSPARCv9, RISC-V (illumos|bsd|linux),
arm32 (armhf), aarch64, plus existing x86_64; keep Linux/FreeBSD/macOS.
Include any abstraction libumem lacks that libdb/snmalloc have (atomics, csprng,
yield, cpu-count, clock, page-state notify, aligned reserve).
Design doc: docs/design/PAL.md + docs/PORTING.md (the required-method contract,
libdb's porting guide as the model). A C "concept" = one header enumerating the
required symbols + a compile-time #error cascade / PAL smoke test.
Validation owed: cross-compile+test on each target (QEMU-user for riscv64,
armhf, aarch64; a real illumos/SPARC run if hardware/VM available). BLOCKED.

## Item 2 [EC2-BLOCKED design-now] -- Merge CI (libumem + snmalloc best-of)
Combine libumem's current .forgejo gate with snmalloc's main.yml breadth:
QEMU cross-build+test (riscv64/armhf/aarch64), TSan+UBSan (--repeat-until-fail),
fuzzing, clang-format+clang-tidy-as-error gate, a SELF-HOST job (make check
under LD_PRELOAD=libumem_malloc.so), BSD VM jobs, an all-checks final gate.
Keep libumem's EC2-gated heavy jobs where runners can't reach (metal benches).
Author the workflows now; they can only be proven green once a runner/account
exists. Validation owed: the workflows actually run. BLOCKED.

## Item 3 [NO-EC2 scaffolding + content] -- Docs via libxtc infra, snmalloc structure, GitHub Pages
Adopt libxtc's docs/ infrastructure VERBATIM where possible: Jekyll +
just-the-docs theme, Doxygen API merged into /api, `.github/workflows/pages.yml`
+ `.forgejo/workflows/pages.yml` deploying to GitHub AND Codeberg Pages, with
link-check (lychee) and mermaid-validation gates. Content STRUCTURE modeled on
snmalloc (per-mechanism design docs) with libxtc's section taxonomy
(guide/, reference/, philosophy/, adr/):
- docs/design/ one page per libumem mechanism (slab/magazine/depot/PTC, rseq
  per-CPU, P5.4/P5.13 mangling, P7.4 span table, T1-T14 introspection, the
  heap-ceiling fix) -- lift from existing commit/results prose.
- docs/guide/ (getting-started, LD_PRELOAD, the C API, debugging with
  umem/umemctl/gdb/lldb).
- docs/reference/ (man pages -> API, build options, limits).
- docs/porting/ (item 1's PORTING contract).
- docs/philosophy/ (slab lineage, what this port changed, the hardening stance).
- docs/adr/ (the decisions: GC removed, rseq default-on, F3 deferred, etc.).
- SECURITY.md disclosure policy.
Validation owed: the Pages build is CI (needs a runner), but authoring +
local `jekyll build` check is NO-EC2. Mostly do-now.

## Item 4 [EC2-BLOCKED design-now] -- Finish F3 via snmalloc message-passing remote-free
Round 9 proved (c2c) the cross-thread cost is real (15.7x LLC misses) but the
stripe-rotation prototype did NOT help (remote traffic unchanged). The RIGHT
mechanism is snmalloc's: a free by a non-owning thread is batched into a
per-allocator MPSC message queue and recycled by the OWNER, warm. Study
~/src/snmalloc src/test/func/freelist_mpscq/, the remote-dealloc path, and
docs/AddressSpace.md. Design how this maps onto libumem's magazine/depot/PTC
(libumem has no per-allocator identity today -- this is the hard part). This is
the biggest, riskiest change; same merge bar as round 9 (fast path call-free
when off/unaffected, clear metal win, c2c shows remote traffic dropped, no RSS
regression). Design-now on branch; validate on metal later. BLOCKED on EC2.

## Item 5 [EC2-BLOCKED design-now] -- Involution freelist encoding replaces P5.4/P5.13
snmalloc docs/security/FreelistProtection.md: encode BOTH directions of a
doubly-linked free queue -- forward f(a)=a^k0, backward g(a,b)=(a^k1)*(b^k2),
invariant x.next.prev==x -- catching corruption libumem's forward-only cookie
mangling cannot. TRADE-OFF snmalloc states: it forces QUEUES not stacks (same
construction/consumption order). libumem magazines are LIFO stacks, so this is
NOT a drop-in; the design must reconcile the ordering constraint with the
magazine/PTC-bin model or scope the encoding to the paths that are queues.
Design doc first (docs/design/freelist-encoding.md), then implement behind the
existing mangle path, with a regression that FAILs on a forged backward link
(which forward-only mangling misses) and the hot-path A/B. BLOCKED on EC2 for
the A/B + gates.

## Item 6 [NO-EC2, do now] -- Evaluate snmalloc's headline features for libumem's roadmap
Write docs/design/snmalloc-feature-evaluation.md assessing each snmalloc
headline against libumem's architecture, with a verdict (adopt / partial /
reject) and reasoning:
1. Same-thread free lock-free -- libumem's PTC already does this; compare.
2. Remote-free via message passing -- = item 4 (F3). Adopt (hard).
3. Large page ranges to cut meta-data -- libumem uses per-slab mmap spans;
   evaluate vs snmalloc's large-range + separate pagemap (relates to the
   P6.x VMA-density and heap-ceiling work).
4. Two-branch malloc fast path -- measure libumem's current fast-path branch
   count; is it reachable in C without snmalloc's C++ metaprogramming?
5. Platform deps abstracted -- = item 1 (PAL/AAL). Adopt.
   Plus the two hard scenarios (cross-thread free, batch dealloc) = item 4.
Validation owed: none (analysis). Do-now.

---

## Sequencing (what to actually do, in order)
Phase A (NO-EC2, now): item 0, item 6, item 3 scaffolding+content, and the
DESIGN docs for items 1/4/5. All committable without a build... EXCEPT
anything that must compile. A docs-only `jekyll build` can be checked locally
(it is doc tooling, not libumem's build -- allowed), but do NOT run libumem's
own build/tests on floki.
Phase B (needs a fresh burner): item 1 PAL/AAL compile+cross-test, item 2 CI
green, item 4 F3 metal A/B, item 5 encoding A/B+gates, item 3 Pages CI deploy.

## Note on scope and honesty
This is months of work, several items architecturally deep (PAL carve-out, F3
remote-free, involution encoding under LIFO magazines). The coordinator will
land the NO-EC2 phase incrementally with explicit-path commits and report what
is DONE vs DESIGNED-BUT-UNVALIDATED. Nothing EC2-dependent is called done until
a fresh account exists and the validation-owed list is cleared. No shortcuts to
hit "adopted": a staged design with an owed-validation list is the honest state
for the blocked items.
