# Security policy

libumem ships `libumem_malloc.so` as an `LD_PRELOAD` drop-in, so it can be
loaded into **setuid/setgid binaries, root daemons, and network-facing
servers**. We treat memory-safety and privilege-boundary bugs in library
code as security issues and ask that they be reported privately first.

## Reporting a vulnerability

**Do not open a public issue for a suspected vulnerability.** Instead:

- Email the maintainer at **greg@burd.me** with `[libumem security]` in the
  subject.
- If you use Codeberg, you may also open a confidential issue on
  <https://codeberg.org/gregburd/libumem> if that option is available to
  you.

Please include, as far as you can:

1. The affected version or commit (`git rev-parse HEAD`, or the release tag).
2. The build configuration (`configure` flags) and platform (OS, arch,
   libc).
3. Whether the target is linked (`-lumem`) or using the `LD_PRELOAD`
   drop-in.
4. **The attacker position** this exposes (this is what makes a report
   actionable):
   - **(A)** a setuid/setgid target;
   - **(B)** a root daemon;
   - **(C)** the attacker controls the environment but not the code;
   - **(D)** the attacker controls allocation patterns and buffer contents
     but not the environment.
5. A reproducer, or a description precise enough to build one -- ideally a
   test that fails on an unfixed build.

A report that names a concrete attacker position and a reproducer is handled
far faster than "this could be bad."

## What is in scope

- Memory-safety defects reachable from untrusted allocation patterns or
  buffer contents (position D): heap overflow consequences, freelist/link
  corruption that yields an arbitrary-address allocation, use-after-free
  acceptance, double-free acceptance.
- Privilege-boundary defects in library code (positions A/B/C): anything an
  environment variable can make a privileged process do to the filesystem,
  the network, or via exec; peer-authorization errors on the introspection
  socket; symlink/TOCTOU issues in file writers.
- Introspection-channel issues: unauthorized access to the `umemctl`
  control socket, or a denial of service through it.

## What is out of scope

- Crashes or leaks that require a non-default debug build or an
  attacker-chosen `configure` flag.
- The experimental headers (`umem_own.h`, `umem_profile.h`, the budget
  example) as *safety mechanisms* -- they are documented diagnostic aids,
  **not** safety or enforcement guarantees, and are not claimed to defend
  against anything. A bug in them is a correctness bug, not a security
  bypass of a guarantee that was never made.
- Expected memory-overhead characteristics (libumem holds ~1.5x its live set
  at small sizes; this is the slab/magazine trade, documented).

## Supported versions

Security fixes land on `master` and in the next release. Older release tags
are not back-patched; upgrade to the latest release. In particular:

- **v3.0.0 and earlier must not be preloaded into setuid/setgid/root
  processes** -- a `PATH`-resolved exec on startup, ungated `UMEM_OPTIONS`,
  symlink-following file writers, and a control socket that trusted the real
  uid. **v3.1.0 fixes all four.**

## Our disclosure practice

- We will acknowledge a report within a reasonable time and work with you on
  a fix and a coordinated disclosure date.
- Every security fix lands with a **regression that fails before the fix and
  passes after**, on x86_64 and aarch64 -- a security fix is exactly where an
  unverified claim is most dangerous, so we do not ship one without a
  demonstration of the exposure it closes.
- We will credit you in the changelog unless you prefer otherwise.

## Honest framing

The hardening work to date closed the specific exposures three adversarial
audits found (2026-09-22, 2026-09-24, and the v3.3.1 follow-ups), each with a
regression. **This was a hardening pass, not a security proof.** It does not
establish that no exposures remain; libumem is a `malloc` replacement, so the
surface is broad. A privileged or network-facing deployment is defensible on
the audited exposures, which is not the same as proven safe against an
unaudited class. See the design pages on
[hardening](https://codeberg.org/gregburd/libumem/src/branch/master/docs/design/hardening.md)
and the Phase 5 section of
`docs/plans/2026-09-21-production-readiness.md`.
