---
title: Debugging a live heap
parent: Guide
nav_order: 4
permalink: /guide/debugging/
lede: >-
  Find leaks, walk caches, identify a pointer, and break before the
  allocation that eventually leaks -- against a running process, with
  umem, umemctl, gdb, and lldb.
---

libumem brings Solaris `mdb`-class heap debugging to Linux, FreeBSD, and
macOS. The mechanism and the arming cost model are on the
[Introspection design page]({{ '/design/introspection/' | relative_url }}); this
is the how-to.

## umem(1): point-in-time, non-invasive

`umem(1)` drives gdb against a live pid (or reads an offline `.ums`
snapshot). It needs no in-process thread and no special build, which makes
it the tool for CI and post-mortem.

```bash
umem --pid $(pgrep mydaemon) findleaks     # outstanding allocations
umem --pid $(pgrep mydaemon) status        # process-wide counters
umem --pid $(pgrep mydaemon) walk          # walk the caches
umem --pid $(pgrep mydaemon) whatis 0x...  # which cache/slab/buffer owns an addr
umem --pid $(pgrep mydaemon) snapshot out.ums   # capture state for offline analysis
```

A **zero exit means a report was produced.** A debugger failure (cannot
attach, nonzero exit, killed, no output) exits 2 and passes gdb's stderr
through -- it is not reported as a clean empty run.

{: .warning }
> **`umem --core` is refused** (exit 2), by design: every command runs
> *inside* the target process, and a core has no process to run them. Use
> the snapshot workflow -- `umem --pid ... snapshot out.ums`, then analyze
> `out.ums` offline, anywhere, with no target process required.

Leak reports require per-buffer tracking: run the target under
`UMEM_DEBUG=audit` (see [debug modes]({{ '/reference/build-options/' | relative_url }})).
Note that audit stacks are ~2 frames deep in a default-built libumem; build
libumem with frame pointers for depth.

## umemctl: live, interactive, opt-in

`umemctl` uses an in-process control socket for the things a ptrace snapshot
cannot do: streaming logs, a live TUI, and breaking *before* a leaked
allocation. It is opt-in at both compile and run time, with **zero hot-path
cost when nothing is armed**.

```bash
./configure --enable-introspect && make -j"$(nproc)"
UMEM_OPTIONS=introspect=1 LD_LIBRARY_PATH=.libs ./myapp &
umemctl $! stats
```

```
umemctl <pid> stats                    # process-wide counters
umemctl <pid> caches                   # one line per cache
umemctl <pid> cache <name>             # detail for one cache
umemctl <pid> whatis <addr>            # which cache/slab/buffer owns an addr
umemctl <pid> leaks                    # outstanding allocations (audit mode)
umemctl <pid> logtail                  # stream live log-like events
umemctl <pid> record <out.log>         # capture the event stream
umemctl <pid> break <predicate>        # arm a break (size=/cache=/seq=)
umemctl <pid> break leaked --set <set> # stop before a leaked alloc
umemctl <pid> continue                 # resume a stopped thread
umemctl <pid> monitor [--once]         # full-screen TUI
```

The socket lives at `$XDG_RUNTIME_DIR/umem.<pid>.sock` (when that is a real
0700 euid-owned directory) or `/tmp/umem-<euid>/umem.<pid>.sock` otherwise;
`$UMEM_INTROSPECT_SOCK` overrides both. **Only a peer with the target's euid
is served.** The server handles one client at a time -- stop a `logtail`,
`record`, or `monitor` stream before you `continue` a parked target.

### Break before the leak

The signature workflow no other allocator offers: learn the signatures of
leaked allocations, then stop the allocating thread *before* the allocation
that will later leak, so a debugger catches the exact stack.

```bash
umemctl $! record --learn-leaks leakset   # phase 1: learn signatures
umemctl $! break leaked --set leakset      # phase 2: stop before a leaked alloc
# ... attach gdb/lldb to the parked thread, inspect the stack ...
umemctl $! continue
```

{: .note }
> While a break is armed, every alloc/free takes the slow path, so
> allocation is slower -- that is the cost of observing every event.
> Disarmed, there is no break test on the fast path at all. Buffers already
> in a thread's per-thread cache when the break is armed are served
> unobserved until the cache drains or the thread exits.

## gdb / lldb integrations

The install ships gdb and lldb scripts exposing the same command set under a
`umem` prefix, plus conditional breakpoints on allocator events:

```
umem findleaks
umem whatis 0x...
umem break alloc size=1024    # stop on a 1024-byte allocation
umem events                   # tap alloc/free/corruption events
```

All fourteen tool-review findings (T1-T14) were fixed in v3.3.3, each with a
regression; see the [Introspection]({{ '/design/introspection/' | relative_url }})
page for the residual caveats.
