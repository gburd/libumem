# umem_cpu_node[] OOB fix validation on >256-CPU hardware — 2026-07-23

Validation of commit **04060e7** (`fix(core): size umem_cpu_node[] to
umem_max_ncpus`) on real >256-CPU metal hardware, as required before trusting
the fix.

## Instance

- **intel-hi = c7i.metal-48xl, 192 vCPU, x86_64**, performance governor.
- Built on the instance via `scripts/ec2/clean-regen.sh --enable-asan`
  (`-fsanitize=address -fno-omit-frame-pointer`). Never built locally.

## The critical number: `umem_max_ncpus = 512`

`umem_max_ncpus` is rounded up to a power of two during init
(`umem.c`: `while ((umem_max_ncpus & (umem_max_ncpus - 1)) != 0)
umem_max_ncpus++;`). On this 192-vCPU box it lands at **512**, which is
**> UMEM_MAX_DEPOT_CPUS (256)**.

Because `cache_depot_ncpus == umem_max_ncpus == 512`, the depot's per-CPU
stealing loop in `umem_depot_alloc` indexes `umem_cpu_node[cpu]` over
`[0, 512)`. The pre-04060e7 code sized `umem_cpu_node[]` to the fixed
256-entry `umem_cpu_node_static[]`, so any CPU id in [256, 512) read **past
the end of the array** — an OOB the ASan build catches as a
`global-buffer-overflow` in `umem_depot_alloc` on essentially every
allocation. **This box therefore genuinely exercises the >256 path** (it is
not one of the "rounds to <=256, path not hit" cases the repro warns about).

## Result: repro exits 0 with the fix

```
=== vCPU ===
192
=== OOB repro (asan) ===
repro_cpu_node_oob: survived (umem_max_ncpus <= 256 here)
repro_exit=0
=== detect umem_max_ncpus ===
umem_max_ncpus=512
```

- `test/unit/repro_cpu_node_oob` **exits 0** under ASan
  (`ASAN_OPTIONS=abort_on_error=1:halt_on_error=1`). Before 04060e7 this
  aborted with `global-buffer-overflow in umem_depot_alloc` on the first
  allocation. The repro's own "survived" message text assumes <=256; the
  authoritative signal is the ASan **exit code 0** on a box where
  `umem_max_ncpus=512`, i.e. the >256 depot path is live and no longer OOB.
- The dynamic `umem_cpu_node[]` is now sized to `umem_max_ncpus` (512), so
  `UMEM_CPU_NODE(idx)` is in-bounds for every depot CPU slot.

## Non-regression: test_main under ASan

`test/.libs/test_main --no-fork` under ASan runs the full unit suite with no
ASan OOB report in any allocator/depot/magazine path. It aborts only inside
**`/umem_own/mt/move_transfer`** (the experimental *ownership-tracking*
multithreaded test — Workstream H), which is unrelated to the core allocator
or this OOB fix and pre-exists this validation (the added
`umem_dump_contention` hook is read-only and is not exercised by test_main).
The ownership-API abort is out of scope for Workstream D; flagged for H.

## Conclusion

The `umem_cpu_node[]` OOB fix (04060e7) is **validated on genuine >256-CPU
hardware**: with `umem_max_ncpus=512` the depot path that previously read past
the 256-entry static array now runs clean under ASan (exit 0). No 384-vCPU
box (r8i.metal-96xl) was required — this 192-vCPU c7i.metal-48xl already
rounds `umem_max_ncpus` to 512 and thus exercises indices in [256, 512).
