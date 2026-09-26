# Tool review: umemctl, umem(1), gdb and lldb helpers (2026-09-26)

Question: do `umemctl` and the GDB/LLDB add-ons do what they advertise? This
first pass reads the claims (`docs/UMEMCTL.md`, `tools/DEBUGGING.md`, each
command's own help text) against the code that implements them. Findings marked
**read** are from source only; the runtime pass on EC2 follows, and each
finding gets a regression before it is called fixed.

## What `make check` actually exercises

| tool | commands tested | test |
|---|---|---|
| `tools/umem` (gdb wrapper) | findleaks (text, json), status, log, walk allocated, whatis, snapshot + `--dump` round trip | `test/debugger/test_inspect_e2e.sh` |
| lldb helper | findleaks, status, walk allocated | `test/debugger/test_lldb_e2e.sh` (SKIPs without lldb) |
| `umemctl` | `stats`, `break` (contract checks only) | `test/debugger/test_introspect_contracts.sh` (SKIPs without `--enable-introspect`) |

Untested: gdb `umem break`, `umem events`, `umem bufctl`, `umem walk freed|log`;
lldb `log`, `whatis`, `bufctl`, `snapshot`, `break`, `events`; umemctl
`caches`, `cache`, `whatis`, `leaks`, `logtail`, `record`, `record
--learn-leaks`, `break leaked --set`, `continue`, `monitor`.

## Findings

### T1 (HIGH, read): `umem break` / `umem events` can never fire, in both gdb and lldb

Both helpers implement `umem break alloc|free|error` as a debugger breakpoint on
`umem_event_alloc` / `umem_event_free` / `umem_event_error`, after calling
`umem_inspect_enable_events(1)`. Those three functions exist
(`umem_inspect.c:84-104`, `noinline, used`) and are reached only from
`umem_inspect_notify()`. **Nothing in the allocator calls
`umem_inspect_notify()`**: zero call sites in `umem.c`, `malloc.c`,
`umem_ptc.c`, and none in git history (`git log -S`). So the breakpoint is set,
events are "enabled", and it never triggers. The command appears to work and
silently catches nothing. The same applies to `umem_inspect_set_event_cb()`,
the public C callback API that `umem_inspect.h` and `tools/DEBUGGING.md`
advertise, since it fans out from the same dead function.
`umem_inspect.h:213-215` documents `break umem_event_alloc if size > ...` as
the way to use it. No test covers it, which is how it survived.
Fix options: wire `umem_inspect_notify()` into the alloc/free/error paths behind
the existing `umem_inspect_events_enabled` flag (a load and a
predicted-not-taken branch on the fast path, so it needs the hot-path A/B), or
withdraw the commands and the API and say so. The PTC and rseq fast paths are
where most allocations are served now, so a hook only on `_umem_cache_alloc`
would miss them as well.

### T2 (MEDIUM, read): `umemctl monitor` always shows "bufs in-use 0"

The server's `stats` reply emits `bufs_held` (renamed in `cb76707`, the
Phase 3 "outstanding vs leak" contract). The `umemctl` monitor parser still
matches `bufs_inuse` (`tools/umemctl.c` stats parser), so that field is never
filled and the dashboard's in-use line reads 0 against every target.
`docs/UMEMCTL.md` also still lists `bufs_inuse`. The `caches` table header
became `held` on the server as well. `monitor --once` is not in any test.

### T3 (LOW, read): `docs/UMEMCTL.md` has drifted from the code

- Socket path: the doc says `/tmp/umem.<pid>.sock`. Since P5.6 the server
  uses `$XDG_RUNTIME_DIR/umem.<pid>.sock` or `/tmp/umem-<euid>/umem.<pid>.sock`
  (a 0700 private directory). The client already follows the server; the doc
  does not.
- `monitor` is documented as showing "magazine reloads". That counter was
  deleted (`921b502`), and the monitor does not print it.
- `whatis` claims to identify "any live pointer". It reports slab ownership
  only: a buffer the application has already freed into a magazine, rseq
  magazine or PTC bin reads `held`, and oversize allocations (routed past the
  caches into vmem) are not found at all. The server code says this in a
  comment; the user doc does not.

### T4 (LOW, read): `tools/DEBUGGING.md` still advertises `--core`

`umem --core core.12345 --exe ./myapp findleaks` appears in the quick start, and
the mdb translation table maps `mdb core` to `umem --core`. Phase 3 item 6
withdrew `--core`: `tools/umem.c` now refuses it and exits 2 with an
explanation. The doc promises a mode the tool deliberately rejects.

### T5 (LOW, read): `findleaks` accounting gaps are not in the user doc

Phase 3 left these open and recorded them: PTC bins are not enumerable (their
objects are reported as `ptc_unaccounted`), oversize allocations are not
walked, and rseq cached-set subtraction is untested. Once P8.5b routes most
allocations through PTC and rseq, a larger share of "held" buffers are in
fact cached and free. `tools/DEBUGGING.md` mentions none of this.

## Next

1. Runtime pass on a Debian EC2 box with `--enable-introspect`: run every
   advertised command of all four tools against a live target and record
   PASS / wrong output / crash / silent no-op. That turns the **read** findings
   above into observed ones, or refutes them.
2. Each fix gets a regression that fails first: T1 a gdb/lldb test that arms
   `umem break alloc` and asserts it stops; T2 `monitor --once` asserting
   non-zero held against a target that holds buffers.
3. T1 needs a decision (wire the hook in, with a hot-path A/B, or withdraw the
   feature). The rest are straightforward.
