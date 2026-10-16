---
title: Security hardening
parent: Design
nav_order: 8
permalink: /design/hardening/
lede: >-
  The Phase 5 security posture: what three adversarial audits found, what
  was fixed, and -- honestly -- what that does and does not establish.
---

libumem ships `libumem_malloc.so` as an `LD_PRELOAD` drop-in, so it can be
loaded into **setuid binaries, root daemons, and network-facing servers**.
Phase 5 hardening assumes all three. This page is the honest framing; the
mechanism-level pages are
[Freelist mangling]({{ '/design/freelist-mangling/' | relative_url }}) and
[The span table]({{ '/design/span-table/' | relative_url }}).

## Attacker positions

A security finding means nothing without a stated attacker position. The
audits used these:

- **(A)** a setuid/setgid target;
- **(B)** a root daemon;
- **(C)** attacker controls the environment but not the code;
- **(D)** attacker controls allocation patterns and buffer contents but not
  the environment.

## The 2026-09-22 audit (against v3.0.0), fixed in v3.1.0

One critical and three high findings, each now closed with a regression
demonstrating the pre-fix exposure:

| Was | Now |
|---|---|
| `execlp("addr2line")` on the unconditional init path, `PATH`-resolved, gated only by an ungated `getenv` -- code execution as the elevated user **before `main()`** (position A) | **Deleted.** It never worked anyway (`-e /proc/self/exe` names addr2line itself post-exec). `dladdr`/`libdw` remain. |
| No `issetugid`/`AT_SECURE` gating on option parsing (position C) | `umem_secure_mode()` consulted first; options with file/socket/exec side effects ignored in secure mode, tuning options unaffected. |
| No `O_EXCL`/`O_NOFOLLOW` in any library writer -- snapshot/profile paths followed symlinks | One `umem_open_write()`: `O_NOFOLLOW`, `S_ISREG`, single-link, euid-owned, 0600, truncate only after the checks. |
| Freelist links stored unmangled inside freed buffers -- a one-buffer overflow yielded an arbitrary-address allocation (position D) | Links mangled with an `AT_RANDOM` cookie + slot address, plus containment validation that reports rather than dereferences. |
| Control socket at `/tmp/umem.<pid>.sock`, reclaimed via symlink-following `stat` | euid-private directory, `lstat`, bind-then-`rename()` reclaim that removes nothing it did not create. |
| `SO_PEERCRED` trusted the **real** uid -- a setuid target's unprivileged invoker got full control, including a thread-parking DoS | `geteuid()` or root only. |
| Interposed `free()` wrote a foreign pointer's header *before* validating it | Ownership range-checked before the header read and after decode; all mutation after acceptance. |
| `getpcstack()` walked frames with no stack bounds -- arbitrary reads under audit mode | Real stack bounds consulted; the 16 MiB heuristic is a documented fallback only. |

## The 2026-09-24 audit (of the v3.2.0 delta), fixed in v3.3.0

Seven more findings, each with a regression that fails on the parent commit
-- notably that `free()` could `munmap` a range named by the caller's own
bytes (P5.10, which took three fixes, the first two shown insufficient by
the same test), a `reap_interval=0` core-spin honoured for setuid targets
(P5.11), a per-thread cache struct sharing slabs with user buffers (P5.12),
`mmap_guard` disable-able from the environment (P5.14), a deadlock when a
signal handler ran during a foreign `free()`'s under-lock heap walk (P5.15),
full-stack `libdw` symbolisation on every refused `free()` (P5.16), and fork
children leaking every non-forking thread's per-thread cache (P1.3d). Two of
these were introduced by v3.2.0's own fixes and had passed correctness
review. The v3.3.1 follow-ups added the span table (P7.4) and the
magazine-round mangling (P5.13b).

## Honest framing

{: .warning }
> This was a hardening pass, **not a security proof.** What it establishes:
> the specific exposures three adversarial audits found are closed and stay
> closed under regression. What it does **not** establish: that none remain.
> libumem is a `malloc` replacement, so the surface is broad.

A privileged (setuid/root) or network-facing deployment is now *defensible
on the specific exposures the audits found* -- which is not the same as
proven safe against an unaudited class. Correctness work and hardening work
are different disciplines: passing the correctness gate says nothing about
hostile input, and every hardening change in this tree carries a regression
that demonstrates the exposure it closes, because a security fix is exactly
where an unverified claim is most dangerous.

For the threat positions actually considered and the exit criteria, see
Phase 5 of `docs/plans/2026-09-21-production-readiness.md`, the coordinated
disclosure policy in
[`SECURITY.md`](https://codeberg.org/gregburd/libumem/src/branch/master/SECURITY.md),
and the [hardening-stance essay]({{ '/philosophy/hardening-stance/' | relative_url }}).
