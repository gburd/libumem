# libumem Debugging on Linux/FreeBSD

This document covers post-Solaris debugging of libumem-using programs.
The Solaris workflow was `mdb -p <pid>; ::findleaks; ::umem_log`; on
non-Solaris platforms the equivalents are provided by:

- `umem_inspect.h` — in-process C API (call from app, gdb, or lldb).
- `tools/umem` — standalone CLI wrapping gdb in batch mode.
- `tools/gdb/umem_gdb.py` — gdb integration (`umem` command prefix).
- `tools/lldb/umem_lldb.py` — lldb integration (same commands).
- `tools/umem_dump_reader` — offline reader for binary snapshots.

## Environment

For richest output, run the target with:

```bash
UMEM_DEBUG=audit                 # record stack trace per buffer
UMEM_LOGGING=transaction=1m      # enable 1 MB transaction log
```

Both have measurable overhead (~30% for audit, ~5% for logging) and are
intended for debugging or staging, not production.  The tools degrade
gracefully when these are off:

| flag set | findleaks | log_dump | whatis stack | bufctl_audit |
|---|---|---|---|---|
| nothing | counts only | empty | no | no |
| `audit` | full | empty | yes | yes |
| `transaction=1m` | counts only | full | no | no |
| both | full | full | yes | yes |

Stack capture relies on `getpcstack()`.  On Linux this uses
`backtrace(3)`, which works with frame pointers OR DWARF unwind info;
distro-shipped binaries normally have one or the other.  ARM64 walks
frame pointers directly.

## Quick start with `umem`

```bash
# Live process
umem --pid 12345 findleaks
umem --pid 12345 findleaks -f json | jq .
umem --pid 12345 status
umem --pid 12345 log -n 100
umem --pid 12345 whatis 0x7f4f4a032000
umem --pid 12345 walk allocated -n 50
umem --pid 12345 snapshot /tmp/state.ums

# Core dump
umem --core core.12345 --exe ./myapp findleaks

# Offline (no live process needed)
umem --dump /tmp/state.ums findleaks
umem --dump /tmp/state.ums status
umem --dump /tmp/state.ums log
```

`findleaks` reports allocated buffers grouped by allocation stack.
Allocator-internal caches (`UMC_NOHASH`, `UMC_QCACHE`) and buffers
sitting in per-CPU magazines are filtered out so the report shows
*actual* leaks.

## Workflow examples

### Find leaks in a long-running daemon

```bash
$ pgrep mydaemon
4711

$ umem --pid 4711 findleaks -n 5
findleaks: 142 allocated buffers (98304 bytes) in 8 distinct leak classes
(skipped 13 buffers currently sitting in magazines / per-CPU caches)

   COUNT        BYTES         SIZE CACHE                    STACK (top frame)
-------- ------------ ------------ ------------------------ -------------------
      96        49152          512 umem_alloc_512             #0  0x... in alloc_session+0x42 ()
      32        16384          512 umem_alloc_512             #0  0x... in alloc_session+0x42 ()
      14         7168          512 umem_alloc_512             #0  0x... in cache_entry_new+0x18 ()
       ...

== class 0: 96 allocs, 49152 bytes, cache=umem_alloc_512 (sample buffer 0x...) ==
    #0  0x7fa... in alloc_session+0x42 () at session.c:120
    #1  0x7fa... in handle_request+0x...  at server.c:284
    ...
```

The top class is your hottest leak site.  Open `session.c:120`, find
the `umem_alloc(512, ...)` and trace why its `umem_free` is missing.

### Watch leak growth over time

```bash
while true; do
    umem --pid 4711 findleaks -f json |
        jq -r '"\(now|todate) total=\(.total_buffers) bytes=\(.total_bytes)"'
    sleep 60
done
```

When `total_bytes` grows monotonically, you have a leak.

### Capture state at crash time

Add to your application's signal handler:

```c
#include <umem_inspect.h>

static void
crash_handler(int sig)
{
    umem_inspect_snapshot("/tmp/crash.ums");
    raise(sig);  /* re-raise to dump core */
}
```

Post-mortem:

```bash
umem --dump /tmp/crash.ums findleaks
umem --dump /tmp/crash.ums log -n 100
```

### Investigate a corruption error

When libumem detects corruption (UMERR_BADADDR, UMERR_REDZONE,
UMERR_DUPFREE, etc.) it calls `umem_event_error()` before aborting.
Set a breakpoint on it:

```bash
$ gdb -p 4711
(gdb) source /path/to/tools/gdb/umem_gdb.py
(gdb) umem break error
(gdb) continue
# ... wait for corruption ...
(gdb) bt              # who called free with the bad pointer
(gdb) p code          # UMERR_* code
(gdb) umem whatis buf # which cache, allocated or freed?
(gdb) umem bufctl buf # full audit record with alloc stack
```

### Break on huge allocations

```bash
(gdb) umem events on
(gdb) umem break alloc -s 1048576
Breakpoint set on umem_event_alloc if size >= 1048576
(gdb) continue
# ... breaks the next time someone does umem_alloc(>=1MB, ...) ...
(gdb) bt
```

### Break when a specific address is freed

```bash
(gdb) umem events on
(gdb) umem break free -a 0x7fa9b0c00000
(gdb) continue
# ... breaks when that buffer is freed ...
(gdb) bt
```

### Offline analysis pipeline

```bash
# In production, snapshot every hour:
*/60 * * * * umem --pid $(pgrep mydaemon) snapshot /var/log/umem/$(date +\%H).ums

# Locally, post-process:
for f in /var/log/umem/*.ums; do
    h=${f##*/}; h=${h%.ums}
    bytes=$(umem --dump $f findleaks -f json | jq .total_bytes)
    echo "$h: $bytes bytes"
done
```

## GDB / LLDB direct

If you'd rather drive the debugger interactively:

```bash
# GDB
echo 'set auto-load safe-path /' >> ~/.gdbinit
echo 'source /path/to/libumem/tools/gdb/umem_gdb.py' >> ~/.gdbinit

# LLDB
echo 'command script import /path/to/libumem/tools/lldb/umem_lldb.py' \
    >> ~/.lldbinit
```

Commands (identical in both):

| command | purpose |
|---|---|
| `umem findleaks [-f text\|json] [-n N]` | leak summary |
| `umem log [-f text\|json] [-n N]` | transaction log |
| `umem status [-f text\|json]` | per-cache stats |
| `umem walk [allocated\|freed\|log] [-f text\|json] [-n N]` | enumerate buffers |
| `umem whatis <addr>` | identify a pointer |
| `umem bufctl <addr>` | full audit record |
| `umem snapshot <path>` | binary or text dump |
| `umem events on\|off` | arm hook points |
| `umem break alloc [-s SIZE] [-c CACHE]` | break on big allocations |
| `umem break free [-a ADDR] [-c CACHE]` | break when ADDR is freed |
| `umem break error [-c CODE]` | break on detected corruption |
| `umem help` | this list |

## In-process C API

All inspection works from inside the application too.  Useful in test
harnesses, exit handlers, or admin endpoints:

```c
#include <umem_inspect.h>

void admin_dump_leaks(int fd) {
    FILE *fp = fdopen(dup(fd), "w");
    umem_findleaks(fp, UMEM_FMT_JSON, 50);
    fclose(fp);
}

void on_shutdown(void) {
    umem_inspect_snapshot("/tmp/shutdown.ums");
}
```

The walkers are also exposed for custom tooling:

```c
static int my_cb(const umem_buffer_info_t *info, void *arg) {
    /* called once per allocated buffer */
    return 0;  /* non-zero stops the walk */
}
size_t n = umem_walk_allocated(my_cb, NULL);
```

## Known caveats

1. **Stack traces require frame pointers OR DWARF unwind info.**  A
   `-O2` binary built without `-fno-omit-frame-pointer` and stripped of
   debug info may produce truncated stacks.

2. **Buffers in PTC (per-thread cache) appear allocated** because the
   PTC keeps them outside the magazine layer.  However, `UMEM_DEBUG=audit`
   automatically disables PTC for affected size classes, so under audit
   mode this is not an issue in practice.

3. **`umem_findleaks` filters allocator-internal caches by default.**
   Use `umem_walk_allocated()` directly (without the wrapper) to see
   them.  Internal caches almost never contain user leaks; they're
   slab metadata, magazine shells, and bufctl records.

4. **The transaction log is a fixed-size ring buffer.**  Records older
   than the log can hold are lost.  Increase `UMEM_LOGGING=transaction=N`
   if you need more history.

5. **`gdb` requires `set auto-load safe-path /` (or specific allowlists)**
   to load the Python extension.  `umem` sets this internally.

## Files

| file | purpose |
|---|---|
| `umem_inspect.h` | public C API |
| `umem_inspect.c` | implementation |
| `tools/umem` | CLI wrapper for gdb batch mode |
| `tools/umem_dump_reader` | offline snapshot reader |
| `tools/gdb/umem_gdb.py` | gdb integration |
| `tools/lldb/umem_lldb.py` | lldb integration |
| `test/debugger/test_inspect_e2e.sh` | end-to-end test |
| `test/test_inspect_live.c` | reference test program |

## Comparison to the Solaris mdb workflow

| Solaris mdb | Linux/FreeBSD equivalent |
|---|---|
| `mdb -p PID` | `umem --pid PID <cmd>` or attach with gdb |
| `mdb core` | `umem --core core --exe bin <cmd>` |
| `::findleaks` | `umem findleaks` |
| `::umem_log` / `::umem_logs -v` | `umem log` |
| `::umastat` | `umem status` |
| `::whatis ADDR` | `umem whatis ADDR` |
| `::bufctl_audit BCP` | `umem bufctl ADDR` |
| `::umem_cache` walk | `umem walk allocated` |
| (none) | `umem snapshot /tmp/x.ums` + offline `--dump` |
| (none) | `umem break alloc/free/error` (mdb has bp via dcmds) |

The breakpoint conveniences are the only thing the Linux workflow
*adds* over Solaris mdb — the rest is restoring parity.
