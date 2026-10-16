---
title: The rseq per-CPU layer
parent: Design
nav_order: 3
permalink: /design/rseq-percpu/
lede: >-
  The restartable-sequence lock-free reload wired in behind the per-thread
  cache -- armed and serving in v3.3.2 (P8.5b), after a long period inert.
---

**rseq** (restartable sequences) is a Linux kernel feature that lets a
thread run a short critical section against *the CPU it is currently on*
with no lock: if the thread is preempted or migrated mid-sequence, the
kernel restarts it at an abort label. libumem uses it to reload a per-CPU
magazine without taking the depot mutex.

## The long inert period

For most of this port's history the rseq assembly ran on every qualifying
alloc and free and served **zero** allocations. The reason was structural,
not a bug in the assembly: the per-thread cache (PTC) fronts the rseq layer
and drains the depot's `cache_full` list before the reload ever looks, so
the per-CPU magazines it reads were never populated (`armed=0`,
`rseq_alloc=0`, `no_full=100%` under every workload). The README said so
plainly rather than claiming a feature that did nothing.

{: .warning }
> Entering a code path is not the same as that path doing its job. The rseq
> assembly was called constantly and still served zero magazine hits. That
> gap is exactly the kind the AGENTS.md evidence rules exist to catch.

## P8.5b: arming the layer

v3.3.2 routes the per-thread cache's magazine refill *through* the rseq
layer. On a PTC-magazine miss, the buffer is pulled from (or pushed to) a
per-CPU magazine via the rseq alloc/free fast path, arming `cache_rseq[cpu]`
from the depot through the already-safe reload. A per-thread-cache miss is
now served lock-free from a per-CPU magazine instead of from the
mutex-protected depot.

It is on by default (`--enable-rseq=auto`, enabled at runtime on any
rseq-capable Linux, glibc 2.35+ managing registration or manual). Counter
evidence (Debian 13, GCC 14.2, glibc-managed rseq so the restartable-
sequence commit is memory-safe on both x86_64 and aarch64): at 192 threads
on metal, `rseq_alloc` reached 57.8 M (x86_64) / 241 M (aarch64). The layer
is finally doing work.

`UMEM_OPTIONS=norseq` turns it off with no rebuild:
[`umem_rseq_init`] is skipped, `umem_rseq_enabled` stays 0, and every
allocation falls back to the depot path.

## What it buys, and what it costs

Measured same-binary, rseq on vs `UMEM_OPTIONS=norseq`, 192 threads on
Debian metal. These are a tail-latency and memory win, **not** a throughput
win:

| | x86_64 on / off | aarch64 on / off |
|---|---|---|
| sustained prodcons p999 | 29.7 / 41.7 us | 5.0 / 16.9 us |
| sustained prodcons p99 | 4.1 / 6.5 us | **1.7 / 1.0 us** (worse) |
| sustained prodcons throughput | 3 % lower with rseq | 7 % lower with rseq |
| RSS at peak | 130 / 246 MB | 122 / 219 MB |

aarch64 p99 is *higher* with it on -- the layer is not free. RSS roughly
halves. Throughput is slightly lower. Whether to keep it on is a per-
workload decision, which is why the off switch exists.

{: .warning }
> **The aliasing bug.** The first version of this routing (on master, never
> released) handed the same buffer to two owners under an rseq abort, at
> ~2.5 % of oracle runs. It passed every gate it was put through; it was
> caught only by a 100-run oracle *rate* test, and fixed with a regression
> that failed on every pre-fix run. The mechanism and why the earlier gates
> missed it are documented in
> `docs/results/2026-09-26-p85b-aliasing-mechanism-v2.md`. The lesson is in
> the ADR on rseq default-on.

## Validation owed

The measured windows above were labelled 180 s but actually ran 17-43 s (a
harness calibration bug, fixed in `38ec141`), under a glibc TLS setting that
left each thread only a few KB of stack. Read the *direction* of each row,
not the absolute values; a full 15-minute re-measurement against glibc and
jemalloc superseded the throughput tables (see the README's heavy-load
section). Any further rseq tuning (e.g. magazine rounds) must re-run the
A/B with a null control on real metal.
