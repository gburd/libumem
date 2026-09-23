# test/limits -- Phase 6 hard-limit probes

Measurement instruments for the "Hard limits" section of
`docs/plans/2026-09-21-production-readiness.md`. Each probe pushes one
dimension (object count, object size, threads, caches, heap size at fork,
kernel caps, reclaim) until libumem fails or degrades, and reports the number
and the caller-visible behaviour. **Never in `make check`**: they take minutes
and gigabytes.

Every probe except `probe_caches` is built twice from one source:
`<probe>` (libumem API) and `<probe>_glibc` (`-DLIM_GLIBC`, plain libc), so the
"does glibc share this limit" answer comes from the same program.
`run_all.sh <probe> args...` runs the pair.

| probe | dimension | args |
|---|---|---|
| `probe_objcount` | live small objects | `[count] [size]` |
| `probe_objsize`  | oversize arena, vmem segments | `<size> <count> [rounds]` |
| `probe_threads`  | PTC footprint, exit drain | `<nthreads> [allocs/thread]` |
| `probe_caches`   | `umem_cache_create` count, applyall stall, fork | `<n> [--fork]` |
| `probe_fork`     | `fork()` with a large heap | `[heap_gb] [forks]` |
| `probe_rlimit`   | RLIMIT_AS/DATA, overcommit=2, max_map_count | `as\|data\|none <mb>` |
| `probe_reclaim`  | RSS return after freeing a heap | `[heap_gb] [window_s] [small\|big\|both]` |

Results and the diagnosis for each live in the plan, not here. Run them on
EC2 through `scripts/ec2/verify-isolated.sh` so the sha is pinned.
