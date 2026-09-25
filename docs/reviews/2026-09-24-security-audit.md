# Security audit of v3.2.0 → next (2026-09-24/25)

Attacker positions as in `AGENTS.md` §7a: **(A)** setuid/setgid target,
**(B)** root daemon, **(C)** attacker controls the environment but not the
code, **(D)** attacker controls allocation patterns and buffer contents but
not the environment. Severity per Phase 5's scale. Every finding has a
file:line and a position, or it is a NOTE.

Written by the coordinator: the agent assigned this audit was stopped by the
model provider before producing output (the second such stop this round; the
first was the P8.3 agent, restarted with plainer wording). Phase 5 and Phase 7
of `docs/plans/2026-09-21-production-readiness.md` are the prior record and
are not repeated.

## Findings in the v3.2.0 delta

### P5.10 -- `free()` unmapped a range named by the caller's own bytes — HIGH, FIXED
`malloc.c` bootstrap allocator. Position **D**. Found independently by the
P8.3 agent (recorded in its STATUS as "open as a P5 item") and by the
production-readiness review. Step 1 of `process_free()` read `buf[-1]` for
every freed pointer and, on `BOOTSTRAP_MAGIC`, called `bootstrap_free()` →
`munmap(hdr, hdr->size)` with both from that memory. Three fixes, the first
two shown insufficient by the same test (`test_forged_bootstrap`):
live-count gate (28 bootstrap mappings survive `umem_init` in every process,
count never zero); registry (reintroduced the P8.1 lock-per-free collapse,
0.005 preload/API); registry behind a hull (`3767b4c`: 0 of 200,000 heap
pointers fall in the hull; ratio 0.598). glibc has no pre-ownership read;
libumem now matches. Full table in the plan.

### P5.11 -- `reap_interval=0` spun a core, and was honoured under AT_SECURE — HIGH, FIXED
`umem_update_thread.c:103,125`; `envvar.c` "reap_interval". Position **C**
against **A**. The update thread's deadline is `now + reap_interval`; at 0 it
is always past and `umem_cache_applyall()` runs back to back forever.
Measured: **2.003 s of CPU in a 2 s sleep**. `reap_interval` is a pure tuning
option and P5.2's gate (file/socket/exec/disclosure side effects only)
honours it under `AT_SECURE`, so a hostile environment burns a core of a
setuid target for its lifetime. glibc ignores every `MALLOC_*` tunable under
`AT_SECURE`: **worse than glibc**. Fixed at the parser (`cbb1a2e`): 0 is
refused and logged; the option stays available in secure mode. Regression
`test_reap_interval_zero.sh`: 2.003 → 0.004 s.

### P5.12 -- per-thread cache structs shared slabs with user buffers — HIGH, FIXED
`umem_ptc.c` `umem_ptc_get()` (was `umem_alloc(sizeof (umem_ptc_t))`).
Position **D**. `umem_ptc_t` is 24,064 bytes: `bins[0].slots` (a pointer) at
offset 0, then 36 bin records, magazine pointers, and a `pool[]` of the
addresses the next `umem_alloc()` returns unchecked. It was allocated from
`umem_alloc_24576`, the user size class for 20,481–24,576-byte requests, one
object per slab, spans contiguous. Measured: **8 of 8 live PTCs exactly one
object-size from a user buffer**. An overrun of one byte past a user buffer's
end rewrites the allocator's slot pointer. glibc's `tcache_perthread_struct`
has the same adjacency and safe-links its entries (2.32); libumem's slots are
raw: **worse than glibc**. P5.4 mangled the freelist links in *freed*
buffers for the same attack class and did not reach this structure. Fixed
(`444b062`, `ec5c10f`): `umem_ptc_cache`, `UMC_INTERNAL` in
`umem_internal_arena` alongside the magazine caches -- its slabs hold only
PTCs. Post-fix 0 adjacent. Slot mangling itself is not done (P5.13, open, a
per-op XOR on the hot path; glibc pays it).

### P5.14 -- `mmap_guard` was honoured under AT_SECURE — MEDIUM, FIXED
`envvar.c` "mmap_guard" (`ab8a73d`). Position **C** against **A**. The entry
said "not secure-unsafe in either direction ... neither is a file, socket,
exec or disclosure side effect". Wrong test: `mmap_guard=0` disables the
`PROT_NONE` remap on every freed span, so a use-after-free into a freed
≥16 MiB buffer reads instead of faulting -- an environment-controlled
downgrade of a hardening property. Not worse than glibc (no guard there at
all), but §7a is about not letting the environment weaken a privileged
target. Gated (`a1912e2`); a setuid target gets the 16 MiB default.

## Checked and found fine (the delta)

- **Update thread privileges and reach.** Runs as the process, touches only
  allocator state, no file/socket/exec. `reclaim_delay=0` reclaims ASAP
  (performance, not safety); `reap_interval` huge means reclaim never runs
  (a leak, not corruption). `pthread_create` inside the first `malloc()`
  under a seccomp filter that denies `clone3`: `umem_create_update_thread`
  returns 0, `umem_init` continues without the thread (the pre-`9bbe58b`
  state), no failure. Fork: the thread is not inherited; `umem_do_release`
  zeroes `umem_update_thr` and `cceae1d` recreates it after every lock is
  released. Nothing held across fork that the child does not release.
- **`abort` under AT_SECURE.** Arming abort makes detected corruption
  fatal. Position C cannot *cause* a recoverable error (that needs position
  D); enabling stricter detection (`UMEM_DEBUG=guards`, also honoured)
  finds more real bugs, which is glibc's default posture (glibc aborts on
  double free). Same as glibc.
- **CPU hint (`ae86536`).** Every use is `& mask` or bounds-checked
  (`umem.c:3383,3635`: `cpu >= 0 && cpu < umem_rseq_get_ncpus()`). CPUs
  online after init wrap into the mask. Position D cannot influence which
  CPU a thread runs on. Same as glibc arenas.
- **Per-cache block from `umem_cache_arena` (`ede1849`).** Descriptor and
  depot arrays now share an arena but its spans are separate from user
  spans (`umem_cache_arena` ← `umem_internal_arena` ← heap); no user object
  is in those slabs.
- **Integer overflow on alloc paths.** `umem_malloc`: `size_arg + 8 (+8)`
  checked by `size < size_arg` after both adds (`malloc.c`). `calloc`:
  `SIZE_MAX / elsize < nelem` (`malloc_interpose.c:799`). `memalign`:
  `size < size_arg` (`malloc.c`). `umem_cache_create`: `chunksize <
  bufsize` after `P2ROUNDUP` (`umem.c:5070`). `bootstrap_malloc`: `size >
  SIZE_MAX - sizeof (hdr)` (P1.7). All guarded.
- **Introspection protocol (`umem_introspect.c`).** Lines read with
  `fgets(line, 256)`; commands are fixed strings; `whatis <addr>` resolves
  the address against every cache's slab list under `umem_cache_lock` and
  reports only if it lands in a real slab -- no dereference of an arbitrary
  peer-supplied address; `break`/`sig` take addresses as predicates, not
  reads. Peer is euid-authorized (P5.7); socket path is euid-private
  (P5.6). The channel is off by default and secure-gated.
- **New tests and scripts.** The new `.sh` tests under `test/security/` and
  `test/integration/` read `/proc` and run binaries; none writes a file or
  opens a socket. `scripts/ec2/` runs as the operator on throwaway boxes.

## Open

- **P5.13** — PTC slot pointers and magazine round arrays are unmangled.
  P5.12 removes the *adjacency* that made them reachable by a heap overrun;
  a write primitive that can reach `umem_internal_arena` still finds raw
  pointers. glibc's safe-linking covers its equivalent. Cost is a per-op
  XOR on the hot path; needs the brief's A/B. Position D.
- **P7.4** — `umem_may_own()` convex hull (unchanged; characterised in
  Phase 7).
- **P7.5** — leading-component symlinks (unchanged; by design).

## Verdict

v3.2.0 as tagged had three HIGH exposures that v3.1.0 did not (P5.10 was
inherited but unfound; P5.11 and P5.12 were introduced by `9bbe58b` and
`b8c39e6` respectively -- both this week's fixes, both reviewed, neither
caught until this audit). All three plus P5.14 are fixed on master with
regressions that fail at the parent commit. The "not for setuid/root/
network-facing" answer **stands** until P5.13 and P7.4 are decided: a
position-D attacker with a heap write primitive still has an unmangled
pointer table to aim at (P5.13), and a forged header between spans is still
accepted (P7.4). Those are the two remaining items where libumem is weaker
than jemalloc or scudo; against glibc it is now at parity or better on every
axis this audit checked.

The process finding: two of the three HIGHs were in changes made this week by
agents whose work was gated, A/B'd, and reviewed for correctness -- and
*correctness review did not look for them*. §7a's last rule ("correctness
work and hardening work are different disciplines") is not a slogan; every
hot-path or lifecycle change should get the four-position question asked of
it before it ships, and the review that asks it should not be the same one
that checked the lock order.
