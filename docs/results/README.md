# libumem benchmark results

Every performance result lives here under a provenance-stamped directory so it
can be reproduced and compared. A number without provenance does not count.

## Layout

```
docs/results/<date>-<instance>-<arch>/
    matrix.toml     # per-point scaling matrix (see below)
    meta.toml       # instance type, kernel, glibc, gcc, governor, THP, git SHA
    matrix.log      # stderr from the run (build/allocator warnings)
```

`<date>` is `YYYY-MM-DD`, `<instance>` is the EC2 instance type
(e.g. `c7i.2xlarge`, `c7i.metal-48xl`), `<arch>` is `uname -m`
(`x86_64` / `aarch64`).

## How the matrix is produced

`test/bench/matrix.sh` (run on EC2 via `scripts/ec2/run-remote.sh <role>`)
sweeps:

- **workloads**: `single`, `multi`, `prodcons`, `frag`
  (`single`/`frag` are single-threaded; `multi`/`prodcons` sweep threads)
- **threads**: `1,2,4,8,16,32,64,128,192` capped at the instance vCPU count
- **sizes**: `16:64`, `64:256`, `256:1024`, `1024:4096`
- **allocators**: `libc`, `umem`, plus `jemalloc`/`tcmalloc` if installed

Each point is measured with the stabilized harness (Task C1): warm-up runs
discarded, median-of-N reported, with the coefficient of variation (`ops_cov`)
across the N runs recorded. `--pin` binds threads with `numactl`/`taskset` and
requires the `performance` governor on hosts that expose it (bare-metal).

## `matrix.toml` format

A top-level header (`date`, `instance_type`, `arch`, `vcpu`) followed by one
`[[point]]` table per (allocator, workload, size, threads):

```toml
[[point]]
allocator = "umem"
workload = "multi"
threads_requested = 8   # requested; threads = actual worker count used
threads = 8
size = "64:256"
ops_per_sec = 4021553.0 # median over `runs` measured runs
lat_p50 = 41            # ns; p90/p99/p999/max/mean also present
lat_p99 = 210
lat_p999 = 3210
peak_rss_bytes = 12582912
frag = 1.25
ops_cov = 0.0300        # stddev/mean of throughput across runs
runs = 5
unstable = 0            # 1 if ops_cov > 0.10 -> do not gate on this point
```

## Comparing baselines

- A dated cross-arch summary (`<date>-baseline.md`) plots throughput-vs-threads
  and p50/p99/p999-vs-threads for umem vs glibc per arch, and names the thread
  count where umem stops scaling on each arch.
- Soft CI regression annotation (Task C3) compares a short EC2-generated
  reference under `test/bench/baseline/` via `bench_compare_history`.
- Authoritative gating is done on EC2 with the pinned matrix, never on shared
  CI runners.
