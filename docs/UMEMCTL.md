# umemctl — live-process inspection CLI

`umemctl` is libumem's live-process observability tool — the mdb-style
inspection surface for a *running* process, complementing the post-mortem
GDB/LLDB extensions (`tools/DEBUGGER_QUICKREF.md`). It attaches to a process's
introspection socket and drives a line protocol to read stats, walk caches,
identify a pointer, list leaks, stream a live event log, monitor in a TUI,
record the stream, and stop a thread just before an allocation of interest —
including *before the allocation that is eventually leaked*.

## Requirements (opt-in, zero-cost when off)

`umemctl` only works against a process that opted in at both compile time and
runtime:

1. **Compile time:** build libumem with `./configure --enable-introspect`.
   Without it, `umem_introspect_break_check` and the socket server are compiled
   out entirely — the disassembly of `_umem_alloc`/`_umem_free` is
   byte-identical to a build without the feature.
2. **Runtime:** run the target process with `UMEM_OPTIONS=introspect=1`. This
   lazily starts a background thread that opens a per-process Unix domain
   socket.

The break hot-path hook fires *only* when a break predicate is armed
(`umem_introspect_break_armed != 0`); with introspection enabled but no break
set, the steady-state alloc/free path is unaffected.

```bash
./configure --enable-introspect && make -j$(nproc)
UMEM_OPTIONS=introspect=1 LD_LIBRARY_PATH=.libs ./myapp &
umemctl $! stats
```

The socket is `/tmp/umem.<pid>.sock`, or `$UMEM_INTROSPECT_SOCK` if set (the
same variable must be visible to both the target and `umemctl`).

## Commands

```
umemctl <pid> stats                    # process-wide counters
umemctl <pid> caches                   # one line per cache
umemctl <pid> cache <name>             # detail for one cache
umemctl <pid> whatis <addr>            # which cache/slab/buffer owns an address
umemctl <pid> leaks                    # outstanding allocations (audit mode)
umemctl <pid> logtail                  # stream live log-like events
umemctl <pid> record <out.log>         # capture the event stream to a file
umemctl <pid> record --learn-leaks <set>   # phase 1: learn leaked signatures
umemctl <pid> break <predicate>        # arm a break (see below)
umemctl <pid> break leaked --set <set> # phase 2: stop before a leaked alloc
umemctl <pid> continue                 # resume a stopped thread
umemctl <pid> monitor [--once]         # full-screen ANSI TUI
```

### stats

Process-wide roll-up: `pid`, `caches`, `bufs_inuse`, `bufs_total`,
`slab_create`, `slab_destroy`, `depot_contention`, `mag_reloads`, `rss_kb`.

### caches / cache

`caches` prints `name  bufsize  inuse  total  flags` (one row per cache, walked
off the live circular `cache_next` list). `cache <name>` adds per-cache detail:
`align`, `chunksize`, `slabsize`, `slab_alloc`/`slab_free`, `buftotal`, and the
per-cache `depot_contention` / `mag_reloads` counters.

### whatis

`whatis <addr>` maps an arbitrary address to its owning cache → slab → buffer
using the real slab arithmetic, so you can identify any live pointer without a
debugger attached.

### leaks

`leaks` lists outstanding (allocated-but-not-freed) allocations from the audit
records. Requires the target to run under `UMEM_DEBUG=audit` for the records to
exist; each entry carries the size and allocating call-site PC.

### logtail

`logtail` streams log-like events as they happen — transaction-log entries plus
slab create/destroy deltas — the `tail -f` for the allocator. It runs until you
Ctrl-C or the target exits.

### monitor

`monitor` is a dependency-free (plain-ANSI, no ncurses) full-screen dashboard
refreshing at ~2 Hz: caches, bufs in-use/total, slab create/destroy, depot
contention, magazine reloads, RSS, and the top caches by in-use count.
`--once` renders a single frame and exits (used in tests / scripts).

### record

`record <out.log>` captures the same stream `logtail` shows to a file while
also echoing it, so you keep a durable trace of a process's allocation
behavior.

## break predicates

`break` arms the alloc-path stop. On a match the *allocating thread* blocks on
a condvar (spin/park) before the buffer is returned, so a debugger attached to
that thread sees the exact allocating stack. `continue` broadcasts the condvar
and disarms.

| predicate        | stops when …                                                     |
|------------------|------------------------------------------------------------------|
| `size=<n>`       | an allocation of exactly `n` bytes is about to return            |
| `cache=<name>`   | an allocation from cache `<name>` is about to return             |
| `seq=<n>`        | the `n`-th matching allocation is about to return                |
| `token=BREAK`    | the next allocation (one-shot) — used by the recording-token flow |
| `leaked`         | an allocation matching a known-leaked signature (see below)      |

Example — stop before the 500th 128-byte allocation, inspect under GDB, resume:

```bash
umemctl $PID break seq=500        # (with a size/cache filter armed as needed)
# thread parks in the allocator; attach gdb to $PID, `bt` shows the caller
umemctl $PID continue             # resume
```

### Two-phase "break before the leaked allocation"

The headline workflow: stop *just before* an allocation whose memory is never
freed, so you can see the leaking call stack live.

**Phase 1 — learn the leak signatures.** Run the target under audit, exercise
the leak, then harvest the not-yet-freed allocation signatures (size + PC) into
a set file:

```bash
UMEM_OPTIONS=introspect=1 UMEM_DEBUG=audit LD_LIBRARY_PATH=.libs ./myapp &
PID=$!
# ... drive the workload that leaks ...
umemctl $PID record --learn-leaks leaks.set
```

**Phase 2 — break on the leaked signature.** Re-run (or continue) the target,
push the learned signatures, and arm `break leaked`. On the next allocation
matching a leaked signature the allocating thread stops before returning the
buffer:

```bash
UMEM_OPTIONS=introspect=1 UMEM_DEBUG=audit LD_LIBRARY_PATH=.libs ./myapp &
PID=$!
umemctl $PID break leaked --set leaks.set
# thread parks in the allocator at the leaking site; attach gdb, `bt`
umemctl $PID continue
```

`break leaked --set` pushes each `sig size=<n> pc=<addr>` line from the set
file to the server (`sig` protocol command) before arming, so the predicate
matches by size and allocation PC.

## Notes

- `logtail`/`monitor`/`record` are read-only; they never arm the break hook, so
  they add no hot-path cost beyond the periodic poll the target's introspect
  thread already does.
- `break` is the only feature that touches the alloc hot path, and only while a
  predicate is armed. `continue` disarms.
- Everything requires `--enable-introspect` at build time. A stock production
  build has no socket thread and no break hook.

## See also

- `tools/DEBUGGER_QUICKREF.md` — post-mortem GDB/LLDB inspection.
- `umem_introspect.h` / `umem_introspect.c` — the server-side protocol.
- `tools/umemctl.c` — the client.
