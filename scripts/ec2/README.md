# libumem EC2 build/test/bench harness

All heavy libumem work — full builds, `make check`, sanitizers, concurrency
stress, and high-core benchmarks — runs on **AWS EC2**, never on the local
host (`floki`). Local use is limited to editing, `git`, and reading results.
This avoids starving/OOMing the workstation. See the `aws-benchmark` skill for
the underlying launch→tune→measure→terminate methodology.

## Account

- Profile: `numa` (`AWS_PROFILE=numa`), region `us-east-2`, account `713545428937`.
- Burner account, cost is not a constraint — **but always terminate idle
  instances.** A forgotten `.metal` (~$14/hr) is the only real cost mistake.

## Roles (the test matrix: Intel + aarch64, low + very-high core)

| Role       | Arch    | Instance          | vCPU |
|------------|---------|-------------------|------|
| `intel-lo` | x86_64  | `c7i.2xlarge`     | 8    |
| `intel-hi` | x86_64  | `c7i.metal-48xl`  | 192  |
| `arm-lo`   | arm64   | `c7g.2xlarge`     | 8    |
| `arm-hi`   | arm64   | `c8g.metal-48xl`  | 192  |

RISC-V has no first-class EC2 host: cross-compile on EC2, run correctness
under `qemu-user`; RISC-V perf numbers are non-authoritative.

## Workflow

```bash
# 1. Launch (idempotent; creates key pair + SSH-from-your-IP SG on first run)
./scripts/ec2/launch.sh intel-lo

# 2. Bootstrap (toolchain + OS tuning + provenance meta.toml). Once per instance.
./scripts/ec2/bootstrap.sh intel-lo

# 3. Run anything in the repo dir; results auto-pull to docs/results/<date>-<role>/
./scripts/ec2/run-remote.sh intel-lo \
  './autogen.sh && ./configure && make -j$(nproc) && \
   LD_LIBRARY_PATH=.libs test/.libs/test_main --no-fork | tail -5'

# 4. TERMINATE when done (or reap anything idle >2h)
./scripts/ec2/terminate.sh intel-lo
./scripts/ec2/terminate.sh --reap-idle
./scripts/ec2/terminate.sh --all
```

## Safety rules

- Instances are tagged `Project=libumem, Role=<role>`. The scripts only ever
  touch instances with that tag — other projects' instances are never affected.
- `launch.sh` is idempotent: a running instance for a role is reused, not
  duplicated.
- Every workstream agent owns its role's instance for its runs and **must**
  `terminate.sh <role>` (or hand it to the shared pool) when idle.
- OS tuning (governor=performance, THP=never, numa_balancing=0) is applied by
  `bootstrap.sh`; a perf number taken before bootstrap is invalid.

## Authoritative performance lives here, not in CI

GitHub Actions runs correctness only (unit/ASan/UBSan on amd64 **and** arm64;
see `.github/workflows/test.yml`). The `benchmark` CI job is a smoke check, not
a gate: shared CI runners are too noisy and too small (few vCPU, no governor
control) for scaling or tail-latency numbers.

**All authoritative perf is measured here on tuned EC2 metal:**

- x86_64: `intel-hi` = `c7i.metal-48xl` (192 vCPU, performance governor).
- aarch64: `arm-hi` = `c8g.metal-48xl` (192 vCPU, Graviton4).

The shared 8-vCPU `intel-lo`/`arm-lo` boxes are for correctness and quick
sanity only; their numbers carry run-to-run noise and are **not** used for
scaling claims. Any perf figure in `README.md`/`CHANGELOG.md` cites a committed
result under `docs/results/` produced by a `run-remote.sh <hi-role>` run.

### Multi-agent coordination hazards (learned the hard way)

- **`terminate.sh --reap-idle` never reaps `intel-lo`/`arm-lo`** (the shared
  low-core dev boxes) — it only reclaims idle metal. Terminate a shared box
  explicitly by role (`terminate.sh intel-lo`). Do NOT run `--reap-idle` to
  clean up your own high-core box; use `terminate.sh intel-hi`.
- **The worktree is shared.** `run-remote.sh` rsyncs the local worktree up, so
  UNCOMMITTED local edits from one agent can be clobbered by another's sync.
  Commit small and often; never leave edits uncommitted across a remote run.
- **The exec-test helper lives at `test/unit/.libs/umem_env_helper`** (the real
  binary), not the libtool wrapper — `clean-regen.sh` builds don't regenerate
  the wrapper reliably.

## Results provenance

`run-remote.sh` pulls `docs/results/` and the instance `meta.toml` back into
`docs/results/<date>-<role>/`. A result without its `meta.toml` (instance type,
kernel, glibc, compiler, tuning state) does not count.
