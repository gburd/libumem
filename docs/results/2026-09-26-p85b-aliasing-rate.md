# P8.5b post-merge aliasing: three-arm failure rate (2026-09-26)

Recorded by the coordinator while tearing down the rate-test instances, because
the raw job logs live only under the gitignored `docs/results/jobs/` tree. The
P8.5b agent (5064938e) produced these; it had not written them up yet.

## Setup

`scripts/ec2/oracle_rate.sh <dir> 100 8 10`: 100 runs of
`stress_concurrency_oracle --threads=8 --duration=10 --size-class=mixed
--pattern=all` per arm, full stderr kept for every failing run. Each arm on its
own Debian 13 `c7i.2xlarge`, built via `verify-isolated.sh` from a committed ref.

| arm | ref | UMEM_OPTIONS | box | failures |
|---|---|---|---|---:|
| P8.5b, rseq on | `9fdc9d0` (master) | unset | intel-lo@r5rseq3a | **3 / 100** |
| P8.5b, rseq on (repeat) | `9fdc9d0` (master) | unset | intel-lo@r5rseq | **2 / 100** |
| P8.5b, rseq off | `9fdc9d0` (master) | `norseq` | intel-lo@r5rseq3b | 0 / 100 |
| pre-P8.5b | `bffeaaf` (`r5-pre-p85b-gcc14`: 5744436 + three Debian-GCC14 build fixes) | unset | intel-lo@r5rseq3c | 0 / 100 |

Pooled rseq-on rate 5/200 = 2.5 %. If either clean arm truly failed at 2.5 %,
the chance of 0/100 is (0.975)^100 = 8 %; of 0/100 in both, 0.6 %. The aliasing
is introduced by the P8.5b rseq routing. It is not a pre-existing flake.

## Captured failures (all five)

| stage | size (B) | byte offset | expected owner (tid seq) | found owner (tid seq) |
|---|---:|---:|---|---|
| free | 97,535 | 0 | 6 501100 | 6 501148 |
| hold | 77,915 | 0 | 0 598521 | 7 601052 |
| consume | 120,124 | 0 | 1 674325 | 0 673517 |
| consume | 79,230 | 0 | 0 294584 | 0 294594 |
| free | 65,633 | 0 | 0 547153 | 4 546885 |

Every failure: the whole buffer re-stamped from byte 0 by a different
allocation. Both self-aliasing (the same tid handed a buffer it still held, a
few dozen sequence numbers later) and cross-thread aliasing. **Every captured
size is 64-120 KB**, i.e. the large size classes, whose caches use the smallest
magazines -- the ones that can resize, and the only ones exposed to the
`rc->magsize` ceiling the previous agent documented as "not corruption". Lead,
not a finding.

## Consequence

Master (`24bffbd` onward) ships a default-on rseq path with a 2.5 % per-run
aliasing rate at 8 threads. v3.3.2 is blocked. Per the user constraint
(rseq stays default-on), the resolution is a root-cause fix with rseq on, or a
revert of the P8.5b range reported as a failure to ship the feature.

Raw logs (local, gitignored): `docs/results/jobs/{r5rseq3a,r5rseq3b,r5rseq3c,r5rseq}/`.
