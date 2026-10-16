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

### Item 0 gap check

Status: DONE (2026-10-16). The split landed as three files whose sum is a
superset of the pre-split `AGENTS.md`: project-specific direction stays in
`AGENTS.md`; generic working-style/evidence rules moved to
`.github/copilot-instructions.md`; the build/test/benchmark runbook moved to
`.github/skills/building_and_testing.md`. No rule was deleted. Source files
that cite `AGENTS.md §4/§6/§7/§7a/§8/§10` still resolve: those anchors remain
in `AGENTS.md`, with §4 and §7 keeping the libumem-specific substance plus a
pointer to the new file. The AWS account line was updated `hotdog` -> `kiwi`
(830118047484). All seven cross-references resolve (verified mechanically).

Every rule in snmalloc's `.github/copilot-instructions.md` and
`.github/skills/building_and_testing.md`, and where libumem now carries an
equivalent:

| # | snmalloc rule | libumem equivalent | File |
|---|---------------|--------------------|------|
| 1 | Read code first; keep changes focused; no unrelated renames | "Read the relevant code before... Keep changes focused" | copilot-instructions |
| 2 | Written plan for multi-stage work; `PLAN.md`; approval | "Use a written plan... stores plans in `docs/plans/`" | copilot-instructions (Plans) |
| 3 | Match validation to risk; baseline before broad work; use CI/origin-main when attribution unclear | "Establish a relevant baseline before broad... measure against a committed ref" | copilot-instructions |
| 4 | Complete through validation + bounded self-review; independent review when warranted | "Finish approved work through its validation and a bounded self-review" | copilot-instructions |
| 5 | Report incidental findings separately; challenge assumptions | "Report incidental findings separately... Challenge an instruction or an assumption" | copilot-instructions |
| 6 | Allocator tracing via `write()` not `printf`/`message` | "Allocator tracing must use `write()`... learned in `umem_fail.c` (P5.16)" | copilot-instructions (Tracing) |
| 7 | Verify hypotheses before a workaround | "Verify a hypothesis before implementing a workaround" | copilot-instructions |
| 8 | Don't claim `main` broken from a local failure; check CI | "do not conclude the shared build (`master`) is broken from a local failure alone" | copilot-instructions |
| 9 | Search the whole change for other instances of a bug pattern | "Grep for sibling bugs... the v3.0.0 `errno` fix" (sharper example) | copilot-instructions + AGENTS §7 |
| 10 | Prefer evidently-correct designs; don't mistake no-obvious-bug for correct | "Prefer designs simple enough that their correctness is evident" | copilot-instructions |
| 11 | Use cross-platform macros, not raw compiler attributes | DEFERRED to item 1 (PAL/AAL). No PAL exists yet; the macro-vs-raw-attribute rule has no home until the layer lands. Noted here as owed. | (item 1) |
| 12 | No platform assumptions (fixed VA width, max alloc from one platform) | DEFERRED to item 1 (PAL/AAL); same reason. The heap-ceiling fix (v3.2.0) is the live instance of this hazard. | (item 1) |
| 13 | Trust API-boundary bounds checks; no redundant internal checks | Covered in spirit by "avoid over-engineering" / "designs simple enough"; libumem has no stated internal-redundant-check rule and no evidence of the failure, so not transcribed verbatim. | copilot-instructions (partial) |
| 14 | New caches must bypass out-of-range inputs | Not applicable yet; no new-cache rule in libumem. Fold into item 1/future caches work if it arises. | n/a |
| 15 | Keep headers minimal, include only direct deps | Not transcribed (C project, no evidenced failure). Candidate for item 1 header hygiene. | n/a |
| 16 | No direct C++ stdlib dependency; use C headers / stl wrappers | N/A -- libumem is C, not C++. | n/a |
| 17 | Explicit wiring over hidden/convention-based behaviour | "Prefer explicit wiring" captured as "preserve the existing structure... no hidden deps" under Change discipline | copilot-instructions |
| 18 | Document behavioural coupling at the component that could break it | "Comments state current invariants -- which lock protects what, who owns an object" | copilot-instructions + AGENTS §7 |
| 19 | Easy to change, but no speculative extension points | "Do not add abstractions, options, or extension points for hypothetical requirements" | copilot-instructions |
| 20 | Avoid over-engineering/churn; preserve names and boundaries | "Avoid over-engineering and review churn. Preserve names and function boundaries" | copilot-instructions |
| 21 | Evaluate copied patterns in their new context | Not transcribed verbatim; subsumed by "keep changes focused / correctness evident." Minor; add if a copy-paste bug recurs. | copilot-instructions (partial) |
| 22 | Update stale comments/tests/docs | "Update comments, tests, and documentation the change makes stale" | copilot-instructions |
| 23 | Comments describe current behaviour, not change history | "Comments state current invariants... not the history of the change" | copilot-instructions + AGENTS §7 |
| 24 | Follow the build/test skill; select validation by behaviour; report factually | Entire `building_and_testing.md`; AGENTS §4 points to it | skill + AGENTS §4 |

Build/test skill rules (snmalloc `building_and_testing.md`) -> libumem:

| snmalloc rule | libumem equivalent |
|---------------|--------------------|
| Debug build for functional validation; `-check` flavour | `./configure` (ASSERT live) vs `-O3 -DNDEBUG` release; both tested |
| Rebuild before tests; never test a stale binary | "Never test a stale binary. Rebuild in the same directory" |
| Format changed code before commit | libumem has `.clang-format` + `.forgejo/workflows/lint.yml`; format gate noted in CI (item 2) |
| Prefer focused tests while developing | `make check TESTS='...'`, standalone `test_main`, per-property binaries |
| Use a timeout for tests that could hang | `job.sh` runs `timeout(1)` remote-side; standalone runs use `timeout 180` |
| Full suite when breadth/risk warrants | `exit_criteria_gate.sh` (the release gate) |
| Treat failure as actionable; don't call it transient; preserve failing output | copilot-instructions "don't call a failure transient" + AGENTS §5 "preserve evidence" |
| Compare with CI/origin-main when attribution unclear | `verify-isolated.sh <ref>` against committed content |
| Benchmark only Release; retain raw results; report config | `hotpath_ab.sh` null-first A/B; AGENTS §6 "record what produced a number"; results in `docs/results/` |
| Quick disposable experiment recipe | libumem has no disposable-experiment carve-out; everything slow goes through `job.sh`. Not transcribed -- EC2 cost model differs; `job.sh` already records revision/host. |
| Test-library (`snmalloc_testlib`) classification | N/A -- libumem has no two-flavour testlib split. |

No snmalloc rule that libumem *should* have is missing from the three files.
The DEFERRED rows (11, 12) are platform-abstraction rules with no landing
spot until item 1's PAL/AAL exists; they are recorded here as owed so item 1
adds them to `docs/PORTING.md`/the PAL contract rather than being lost. The
N/A rows are C++-specific or testlib-specific and do not apply to a C
allocator.

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

### Item 3 STATUS (2026-10-16, @r10docs) -- AUTHORED, Pages deploy owed

The docs site is authored and committed to master (explicit-path commits,
not amended): infra from libxtc, content snmalloc-structured.

- INFRA (done): `docs/_config.yml` (title libumem, just-the-docs, baseurl
  `/libumem`, aux_links to Codeberg+GitHub, note/rationale/not_chosen/warning
  callouts), `docs/Gemfile`(+`.lock`), `_layouts/content.html`,
  `_includes/nav_footer_custom.html`, `_sass/custom/custom.scss`,
  `docs/Doxyfile` (public headers), `docs/gen_api_reference.sh` (adapted to
  libumem's man(7)-macro pages in the repo root), `docs/lychee.toml`,
  `docs/check_mermaid.sh`. `.gitignore` gains `!docs/Doxyfile` and ignores
  the generated `docs/reference/api/`.
- WORKFLOWS (done): `.github/workflows/pages.yml` (GitHub Pages) and
  `.forgejo/workflows/pages.yml` (Codeberg Pages): Jekyll + gen_api_reference
  + Doxygen into /api + lychee + mermaid-check + deploy; branch `master`,
  path-trigger on docs/**, README.md, SECURITY.md, headers, man pages.
- CONTENT (done, real prose lifted from README/CHANGELOG/plan/results/reviews,
  not stubs): landing `docs/index.md`; `docs/design/` (slab-and-vmem,
  magazine-depot-ptc, rseq-percpu, freelist-mangling, span-table,
  introspection, heap-ceiling, hardening) + links to the existing
  `docs/design/PAL.md` (bed0e99) as the porting contract; `docs/guide/`
  (getting-started, ld-preload, c-api, debugging); `docs/reference/`
  (build-options, limits, man-pages, api index); `docs/philosophy/` (lineage,
  what-changed, hardening-stance); `docs/adr/` 0001-0005 (GC removed, rseq
  default-on, remote-free declined, involution-encoding declined, sparsemap
  removed); `SECURITY.md`.
- NOT a separate `docs/porting/` dir: the porting contract lives in the
  already-committed `docs/design/PAL.md`; the design index and nav link to it.

VALIDATION OWED (needs the CI runner / repo settings -- authored now, deploy
proven later):
1. The Pages workflows actually run green. Neither has executed; a runner is
   required. Local `jekyll build` was NOT run (no ruby/jekyll on floki);
   structural validation only: all 30 permalinks unique, every internal
   `relative_url` link resolves to a declared permalink, all front-matter
   well-formed, `gen_api_reference.sh`/`check_mermaid.sh` pass shellcheck -S
   error. mandoc/mermaid gates and the Doxygen build run only in CI.
2. GitHub Pages must be enabled (Settings -> Pages -> Source: GitHub Actions).
3. Codeberg Pages needs the forgejo deploy `token` secret available to the
   `.forgejo/workflows/pages.yml` job and the `git-pages` branch/setup.
4. The two mermaid diagrams (magazine-depot-ptc, span-table) are validated
   only by the CI mermaid-check; confirm they render.
5. The lychee external-link pass may flag the USENIX/Codeberg URLs; the
   config excludes the known rate-limiters, re-check on first green run.


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

## SCOPE REVISION (2026-10-16, user guardrails) + kiwi account live

Two user guardrails narrow the program:
1. "If remote-free fundamentally changes libumem, don't do it."
   => ITEM 4 (F3 remote-free) is DROPPED. snmalloc's message-passing remote-free
   requires a per-allocator identity and an MPSC return queue that libumem does
   not have; grafting it on is a fundamental re-architecture of the cross-thread
   path, not an adoption. libumem stays a Solaris-slab port (magazine/depot/PTC).
   Round 9 already proved the cheaper stripe-rotation did not help; the honest
   state is: cross-thread locality gap is CHARACTERIZED and DEFERRED, not fixed.
2. "Skip involution encoding if it conflicts with libumem LIFO magazines."
   => ITEM 5 (involution encoding) is DROPPED. Confirmed from code: libumem
   magazines are LIFO stacks -- alloc pops mag_round[--rounds], free pushes
   mag_round[rounds++] (umem.c:3873, 4031, 4160; comment umem.c:2653 "the alloc
   paths pop mag_round[--rounds]"). snmalloc's FreelistProtection.md states its
   backward-link encoding "can only use queues, and not stacks" because
   construction and consumption order must match. libumem's LIFO magazines and
   PTC bins violate that precondition. So it conflicts; skipped as instructed.
   libumem keeps its P5.4/P5.13 forward-link cookie mangling (already stronger
   than glibc's cookieless safe-linking per AGENTS.md).

REMAINING program (now achievable without re-architecting libumem):
- Item 0: agent-rules split. NO-EC2.
- Item 1: PAL/AAL abstraction layer (illumos/SPARCv9, RISC-V, arm32, aarch64,
  + existing). Compile/cross-test on EC2 (kiwi). This is a REFACTOR that
  preserves behaviour -- it moves platform code behind one interface, it does
  not change the allocator. Fits the "don't fundamentally change libumem" rule.
- Item 2: CI merge (QEMU cross, TSan/UBSan, fuzzing, format/tidy gate,
  self-host, BSD VM, all-checks). Validate on EC2.
- Item 3: docs site (libxtc Jekyll+just-the-docs+Pages infra, snmalloc content
  structure). NO-EC2 authoring; Pages deploy is CI.
- Item 6: headline-feature evaluation doc, now recording items 4 and 5 as
  evaluated-and-declined with the reasons above.

EC2: account is now `kiwi` -> 830118047484 (hotdog expired 2026-10-10). The
account holds other gburd projects (noxu-bench m8id/m8gd metal, unnamed c7i) --
NEVER touch anything not tagged Project=libumem AND the worker role. Debian
instances only (EC2_OS=debian).

## EXECUTION MODE (2026-10-16, user: implement all phases, don't check in unless critical)
Goal restated by user: a more well-organized, maintainable, EFFICIENT slab
allocator with all of Solaris umem's decades-proven features, modernized for
today's high-core-count NUMA hardware -- WITHOUT re-architecting what makes it
umem (hence items 4 and 5 dropped).
Coordinator runs autonomously; surfaces only for a critical decision or a
materially-wrong assumption with dramatic impact.

Phase A launched (NO-EC2): @r10rules (item 0 agent-rules split), @r10docs
(item 3 docs site + Pages), @r10eval (item 6 headline evaluation). PAL/AAL
DESIGN (item 1) authored directly: docs/design/PAL.md (bed0e99).
Phase B started: a PRE-refactor baseline gate (job 'basegate') is running on
intel-lo@palci (kiwi, Debian 13) at bed0e99, so the PAL/AAL refactor's
"behaviour-preserving" claim has a before-picture (identical gate + null A/B
required after).
Account: kiwi 830118047484. Only Project=libumem+role instances touched.

## PHASE A COMPLETE + PHASE B LAUNCHED (2026-10-16)
Phase A (NO-EC2) DONE, on master:
- Item 0 (e470c5f): agent rules split -- AGENTS.md (project-specific + See-also)
  + .github/copilot-instructions.md (generic) + .github/skills/building_and_testing.md
  (runbook); gap-check table in this plan; nothing lost (diffed).
- Item 3 (125949b..f6147da): full docs site -- libxtc Jekyll+just-the-docs infra,
  snmalloc content structure, real prose. docs/{design,guide,reference,philosophy,
  adr}/, SECURITY.md, .github+.forgejo pages.yml. 5 ADRs incl. remote-free-declined,
  involution-declined. Validation owed: Pages deploy green (CI runner).
- Item 6 (ee69eaa): docs/design/snmalloc-feature-evaluation.md. Verdicts:
  same-thread-free lock-free = ALREADY-HAVE (fence-free PTC); remote-free = DECLINE
  (re-architecture, gap real: 15.7x LLC misses); large-page = PARTIAL (density half
  DONE via heap-ceiling fix; separate-pagemap/sizeless-free = future candidate,
  removes O5 miss); two-branch fastpath = ADOPT-as-goal (libumem ~5 branches/3
  essential); PAL = ADOPT (item 1).
- PAL/AAL design (bed0e99): docs/design/PAL.md.
- Pre-PAL baseline (01a34c5): ee69eaa x86 Debian 72/59/13/0.
Phase B RUNNING (kiwi, Debian): @r10pal (item 1 refactor, branch r10pal, strict
behaviour-preserving gate: identical make check + null fast-path A/B both arches,
then QEMU riscv64/armhf/aarch64 + illumos cross) and @r10ci (item 2 CI merge:
matrix + QEMU cross + TSan/UBSan + fuzz + format/tidy + self-host + all-checks).

## ITEM 1 (PAL/AAL) MERGED (7e5ed07, 2026-10-16)
Behaviour-preserving refactor step 1-3 merged to master after both gates PASS
on x86_64 AND aarch64:
- Gate A (make check): 73/60/13/0 both arches = baseline 72/59/13/0 + the one
  new pal_aal_smoke test. No pre-existing test changed state.
- Gate B (fast-path A/B): insn/pair +0.00% at all 13 points both arches;
  throughput all inside the null (x86 -0.27..+0.39%, arm -0.11..+0.01%).
- Gate E (#error cascade + link contract): PASS.
What merged: pal/{pal.h,pal_concept.h,pal_posix.c}, aal/{aal.h,aal_concept.h}
+ relocated arch .S/.c, docs/porting/README.md, test/port/pal_aal_smoke.c,
Makefile.am wiring. This is the CONTRACT + POSIX base + arch-file relocation;
further carve-out of umem.c/vmem_mmap.c platform code into the PAL is a
follow-up (the layer exists; migration continues incrementally).
Gate C (QEMU cross riscv64/armhf/aarch64) RUNNING (@r10xc): aarch64 cross-
compile already confirmed (ELF aarch64 lib built). riscv64/arm32 are NEW AALs
stubbed by the refactor; honest per-arch coverage status being recorded.
Gate D (illumos/SPARCv9 run) owed; cross-compile only unless a host appears.
