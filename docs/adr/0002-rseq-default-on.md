---
title: "ADR-0002: rseq default-on"
parent: Decisions (ADRs)
nav_order: 2
permalink: /adr/0002-rseq-default-on/
---

# ADR-0002: The rseq per-CPU layer is on by default

- **Status:** Accepted (v3.3.2).
- **Decision:** build the rseq per-CPU fast path where the kernel supports
  it (`--enable-rseq=auto`) and enable it at runtime by default, with
  `UMEM_OPTIONS=norseq` as a no-rebuild off switch.

## Context

Restartable sequences let a thread reload a per-CPU magazine lock-free. For
most of the port's life the rseq assembly ran but served zero allocations
(the per-thread cache drained the depot before the reload ever looked). v3.3.2
routed the PTC refill through it, so it finally does work. The question was
whether to default it on.

## Decision

On by default. Measured at 192 threads on Debian metal, it roughly halves
RSS and lowers prodcons p999; it is a tail-latency and memory win. The off
switch exists because it is **not** a pure win: throughput is ~3-7 % lower
with it on, and aarch64 p99 is slightly *worse*. A per-workload operator can
turn it off with `UMEM_OPTIONS=norseq`, no rebuild.

## Consequences

- The default binary gets the memory and tail-latency benefit.
- The decision is reversible per-process at runtime.

{: .warning }
> **The cautionary tale.** The first version of this routing handed the same
> buffer to two owners under an rseq abort, at ~2.5 % of oracle runs. It
> **passed every gate it was put through** and was caught only by a 100-run
> oracle *rate* test -- not by any single-run gate. The fix has a regression
> that failed on every pre-fix run. The lesson that shaped this ADR: a
> lock-free fast path on by default must be gated by a rate/oracle test, not
> only a pass/fail run, because its failure mode is probabilistic. Entering a
> code path is not the same as that path being correct.

## Alternatives considered

- **Default off, opt-in.** Rejected: the memory win is large enough to be
  the right default, and the off switch covers the workloads it hurts.
- **No off switch.** Rejected: aarch64 p99 regresses with it on, so an
  operator needs a lever.
