---
title: Introspection
parent: Design
nav_order: 6
permalink: /design/introspection/
lede: >-
  The live-heap debugging surface -- umem, umemctl, and the gdb/lldb
  integrations. The feature that brings Solaris mdb-class debugging to
  Linux, FreeBSD, and macOS, and libumem's most distinctive capability.
---

On Solaris, `mdb`'s `::findleaks`, `::umem_log`, `::umastat`, `::whatis`,
and `::bufctl_audit` made userspace heap debugging a solved problem. No
mainstream Linux allocator has an equivalent. libumem restores the whole
workflow through two complementary tools and a C API, each firing on every
allocation path with no fast-path cost while disarmed (the 2026-09-26 tool
review, T1-T14, closed in v3.3.3).

## Two tools, different jobs

- **`umem(1)`** -- gdb/ptrace-driven, point-in-time. Works against a live
  pid *or* an offline `.ums` snapshot; emits text or JSON. Non-invasive (no
  in-process thread). Best for CI, post-mortem, and scripted leak-finding.
  Commands: `findleaks`, `log`, `status`, `walk`, `whatis`, `bufctl`,
  `snapshot`, `break`. See [`umem(1)`]({{ '/reference/api/umem/' | relative_url }}).

- **`umemctl`** -- an opt-in in-process channel
  (`--enable-introspect` + `UMEM_OPTIONS=introspect=1`) for the
  live/interactive things a ptrace snapshot cannot do: **streaming** event
  logs (`logtail`), a live full-screen TUI (`monitor`), `record`, and
  **break-before-a-leaked-allocation** -- stop the allocating thread so a
  debugger catches the exact stack of the allocation that is eventually
  leaked. Zero hot-path cost when off. See
  [the umemctl guide]({{ '/guide/debugging/' | relative_url }}).

- **gdb / lldb integrations** (`tools/`) -- the same command set under a
  `umem` prefix, plus conditional breakpoints on alloc/free/corruption
  events (`umem break`, `umem events`), and the live callback hook
  `umem_inspect_set_event_cb()`.

## How it works, and why core mode is refused

Every `umem(1)` command is executed by calling
[`umem_inspect(3)`]({{ '/reference/api/umem_inspect/' | relative_url }}) entry
points *inside the target process* -- for a live pid, through gdb driving
the target. This is the mechanism behind the one sharp edge:

{: .warning }
> **`umem --core` does not work, and now says so.** A core file has no
> process to call into, so there is nothing to execute the inspection
> entry points. It is refused with exit status 2 and an explanation. It
> previously exited 0 while printing nothing -- which reads as "no leaks."
> A control proved the regression: the same command on a *live* process
> reported 200 outstanding buffers; on that process's own core it produced
> zero bytes. A passive core reader is not implemented. Use the snapshot
> workflow instead: `umem_inspect_snapshot()` (or `umem --pid ... snapshot`)
> writes a `.ums` file you analyze offline, anywhere, with no target
> process required.

A zero exit from `umem` now *means a report was produced*: debugger
failures (cannot attach, nonzero exit, killed, no output) exit 2 and pass
gdb's stderr through, instead of being silently reported as a clean empty
run.

## The arming cost model

With no break armed, the alloc/free fast paths carry no break test and no
call -- this is the "zero hot-path cost when off" property, verified in the
tool review. Arming a break (or an event hook) sends every allocation and
free through the slow path, the same diversion the gdb/lldb event hooks
use, so `size=`, `cache=`, and `seq=` predicates see the real caller-
requested size. Allocation is slower while a break is armed; buffers already
in a thread's per-thread cache when the break is armed are served from there
unobserved until the break is disarmed or the thread exits.

## Audit mode and stack depth

Per-buffer alloc/free stack capture is `UMEM_DEBUG=audit`. In a default
build the recorded stacks are only ~2 frames deep, because libumem itself is
compiled `-O2` without frame pointers, so `getpcstack()` begins its walk
inside `umem_alloc()` and stops at the first frame-pointer-less allocator
frame. This is a property of how *libumem* is compiled, not of your
application: rebuilding *your* app with `-fno-omit-frame-pointer` does not
fix it. Build libumem with frame pointers for deeper capture; `libdw`-based
capture, where available, is unaffected.

## Known limits (2026-09-26 tool review)

All fourteen findings (T1-T14) were fixed in v3.3.3, each with a regression.
The residual honest caveats:

- `umem --core` is refused (above), by design.
- `umemctl whatis`/`findleaks` count a buffer freed into a per-thread cache
  as outstanding until it drains -- cached, not leaked.
- Audit-mode stack depth is shallow in a default build (above).
