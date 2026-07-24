# Firewall detection RED — diagnosis (2026-07-23, intel-lo)

## Symptom
`test_main --no-fork --single /umem_debug/firewall_detection` FAILS:
the `overflow` probe run with `UMEM_DEBUG=firewall` exits 0 (survives) instead
of faulting on a guard page.

## Root cause: TEST bug (not a library bug)

`firewall` is declared `ITEM_SIZE` in `envvar.c:268` — a **required** size
argument (`=minbytes`). `process_item()` (envvar.c:676) sets
`arg_required = 1` for `ITEM_SIZE`, and when the argument is missing it does
`goto invalid` and returns **before** OR-ing `UMF_FIREWALL` into `umem_flags`.

So `UMEM_DEBUG=firewall` (no `=N`) sets **neither** the flag **nor**
`umem_minfirewall`. `umem_minfirewall` stays at its `ULONG_MAX` init default,
so `umem_cache_create` never arms `UMF_FIREWALL` on any cache
(umem.c:4067: `bufsize >= umem_minfirewall`). No guard page is ever placed.

Measured on EC2 intel-lo:

| env | `UMF_FIREWALL` set? | `umem_minfirewall` |
|-----|--------------------|--------------------|
| `UMEM_DEBUG=firewall`    | NO  (flag probe rc=1) | ULONG_MAX (opt rc=1 vs 0) |
| `UMEM_DEBUG=firewall=64` | YES (flag probe rc=0) | 64 (opt rc=0) |

Second, even with the flag armed, firewall guards only objects
`>= umem_minfirewall`, and places the buffer **flush against the guard page**
(umem.c:4129 firewall coloring: `cache_mincolor = cache_maxcolor =
slabsize - chunksize`, pushing each buffer to the end of its page-quantum
slab). The overflow must therefore:
  1. use `UMEM_DEBUG=firewall=<N>`, and
  2. allocate a buffer `>= N` whose size is a multiple of the cache alignment
     (so `buf + bufsize` == the guard page), then
  3. write **past** `bufsize`.

The old probe allocated 64 bytes and wrote 32 bytes past the end — a redzone
overrun, not a firewall overrun. Under firewall there is no redzone; a 64B
alloc under `firewall` (no size) is not even guarded.

## Library behavior is CORRECT
- `firewall` legitimately requires a byte threshold (documented: "=minbytes.
  Every object >= minbytes in size will have its end against an unmapped page").
- With `firewall=<N>` and an allocation `>= N`, the buffer sits flush against a
  `PROT_NONE` page and an overrun faults with SIGSEGV — verified below.

## Fix
Add a dedicated `firewall_overflow` probe to `umem_env_helper` that allocates a
page-sized buffer and writes past it, and repoint `test_firewall_detection` to
run it under `UMEM_DEBUG=firewall=<N>`. This is a test/helper fix; no library
change.
