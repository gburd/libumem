---
title: The hardening stance
parent: Philosophy
nav_order: 3
permalink: /philosophy/hardening-stance/
lede: >-
  What "hardened" claims and what it refuses to claim -- and the evidence
  discipline that makes the difference meaningful.
---

libumem ships `libumem_malloc.so` as an `LD_PRELOAD` drop-in, which means it
can end up inside a setuid binary, a root daemon, or a network-facing
server. The hardening work assumes all three. But the *framing* of that work
is as important as the work itself, because a security claim is exactly where
an unverified statement does the most damage.

## What "hardened" means here

It means a specific, bounded thing: **the exposures three adversarial audits
found are closed, and each stays closed under a regression that fails before
the fix and passes after.** The mechanism pages
([mangling]({{ '/design/freelist-mangling/' | relative_url }}),
[span table]({{ '/design/span-table/' | relative_url }}),
[hardening]({{ '/design/hardening/' | relative_url }})) list them.

## What it does not mean

{: .warning }
> This was a hardening pass, **not a security proof.** It does not establish
> that no exposures remain. libumem is a `malloc` replacement, so the attack
> surface is as broad as "every allocation and free in the process." A
> privileged or network-facing deployment is *defensible on the specific
> exposures the audits found* -- which is not the same as *proven safe
> against an unaudited class*.

Correctness work and hardening work are different disciplines. Passing the
correctness gate -- `make check`, the property and stress suites -- says
nothing about hostile input. A buffer overflow that corrupts a free link is
not a crash the correctness suite would ever produce; it needs an adversarial
test that constructs the exact forged state.

## The evidence discipline

Every hardening change in this tree carries a regression that demonstrates
the exposure it closes, with no exceptions. The rules, learned from real
failures on this project:

- **State the attacker position.** "Could be bad" is not a finding. Every
  audit finding names one of: (A) setuid/setgid target, (B) root daemon,
  (C) attacker controls the environment, (D) attacker controls allocation
  patterns and buffer contents.
- **Demonstrate the exposure before fixing it.** The P5.4 freelist fix had
  an evidence gap -- its original test's target was outside the victim slab,
  so the *containment check* alone blocked it and the *mangling* was never
  exercised. A new `inslab` case isolated the mangling and failed with it
  disabled. Without that, "the mangling blocks it" would have been an
  unverified claim.
- **Grep for siblings.** The v3.0.0 `errno` fix landed in one function while
  an identical defect sat in its sibling; the release notes claimed the
  class was closed. P5.13 safe-linked the PTC bins and P5.13b had to come
  back for the magazine rounds behind them. Before claiming a defect class
  is fixed, search for the same pattern and state what you found.
- **Compare against glibc.** "Same as glibc" is acceptable; "worse than
  glibc" is a finding that needs a decision. libumem's forward-only cookie
  mangling is *stronger* than glibc's cookieless safe-linking; the span
  table matches jemalloc's exact rtree and is stronger than glibc's absent
  check.

## The library-code rules

Because the library can be loaded into privileged processes, library code
follows hard rules: no `PATH`-resolving exec (`execlp` is forbidden); file
writers get `O_EXCL`/`O_NOFOLLOW` or a reason they cannot be
attacker-influenced; sockets do not go in shared directories under
predictable names; peer authorization uses `geteuid()`, not `getuid()` (for
a setuid target the real uid is the unprivileged invoker); and any file,
socket, or exec side effect an environment variable could trigger is gated
on privilege via secure mode.

The coordinated disclosure policy is in
[`SECURITY.md`](https://codeberg.org/gregburd/libumem/src/branch/master/SECURITY.md).
