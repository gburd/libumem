---
title: The LD_PRELOAD drop-in
parent: Guide
nav_order: 2
permalink: /guide/ld-preload/
lede: >-
  Replace an existing binary's malloc with libumem -- no recompile, no code
  change.
---

`libumem_malloc.so` interposes `malloc`, `free`, `calloc`, `realloc`, and
friends. Point `LD_PRELOAD` at it and the process allocates through libumem
with no rebuild:

```bash
LD_PRELOAD=/usr/local/lib/libumem_malloc.so ./myapp
```

That is the whole interface. There is no API surface in this mode; it is the
system `malloc`, underneath an unmodified binary.

## When the drop-in is worth it

The drop-in gives you libumem's [caching
hierarchy]({{ '/design/magazine-depot-ptc/' | relative_url }}) and its
[live-heap debugging]({{ '/guide/debugging/' | relative_url }}) -- in
particular, `umem --pid $(pgrep myapp) findleaks` against an unmodified
production process, a workflow no other allocator supports. If your workload
is a long-running service with episodic leaks, that alone can justify it.

If your workload is "billions of anonymous small mallocs in a tight loop,"
jemalloc or mimalloc will usually be faster; the drop-in path carries a
per-call 8/16-byte size header that the native `umem_alloc` API does not.

## Performance notes you need to know

{: .warning }
> **Use `a74065e` or later for the drop-in.** Before that commit the
> interposer took a **process-global mutex on every `free()`** (a lock plus
> a 512-slot table scan that stays empty for a process's steady-state life),
> so the drop-in scaled *negatively* with threads -- 500x slower than glibc
> at 192 threads. The fix made the drop-in path scale; after it, at 192
> vCPU the drop-in reaches 393 Mops where it was 0.82 before.

The residual overhead after that fix is the per-call header design: the
drop-in writes, reads, and validates an 8- or 16-byte header that the
`umem_alloc`/`umem_free` API does not, and passes through a second shared
object. P8.3 removed a third of it; the rest is intrinsic to the header
design. Expect the drop-in to run at roughly 0.5-0.9x of the native API
depending on the benchmark.

## Privileged processes

{: .warning }
> **v3.0.0 and earlier must not be preloaded into setuid, setgid, or root
> processes.** v3.1.0 fixed the four issues behind that warning -- a
> `PATH`-resolved exec on the startup path, ungated `UMEM_OPTIONS`,
> symlink-following file writers, and a control socket that trusted the real
> uid. A privileged deployment is now defensible on those specific
> exposures; read [Security hardening]({{ '/design/hardening/' | relative_url }})
> and [`SECURITY.md`](https://codeberg.org/gregburd/libumem/src/branch/master/SECURITY.md)
> for what that does and does not mean.

In a setuid/setgid/`AT_SECURE` process, libumem enters **secure mode**:
`UMEM_OPTIONS` entries that have file, socket, or exec side effects are
ignored, while pure tuning options still apply.

## illumos / Solaris: dual-ABI preload

On illumos the base tools are often 32-bit. To interpose on both 32- and
64-bit processes with a single `LD_PRELOAD`, build both ABIs into the
runtime linker's `/64` path layout:

```sh
# 64-bit -> lib/64 (amd64)
./configure --libdir=/usr/lib/64 CC="gcc -m64"
make && make install && make distclean
# 32-bit -> lib
./configure --libdir=/usr/lib CC="gcc -m32"
make && make install
```

Then `LD_PRELOAD=/usr/lib/libumem_malloc.so.1` resolves to the 32-bit lib
for 32-bit targets and, via the `/64` token, to the 64-bit lib for 64-bit
targets.
