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

## Runtime pass, partial (agent 09db6d84, stopped mid-pass)

Debian 13, `c7i.2xlarge`, `--enable-introspect` build, target
`test/debugger/tool_target` (committed `2530852`). The agent stopped after
umemctl's `logtail`; these are the rows it observed, recovered from its logs.

| tool / command | status | evidence |
|---|---|---|
| C API `umem_inspect_set_event_cb` + `enable_events(1)` | **SILENT NO-OP -- T1 confirmed** | `test_event_cb`: `alloc=0 free=0 (1800 allocations made)`, rc 1 |
| umemctl `stats` | PASS | `bufs_held 4461 bufs_total 7617 slab_create 136 ...` |
| umemctl `monitor --once` | **WRONG -- T2 confirmed** | `bufs in-use 0` in the same second that `stats` reports `bufs_held 4461` |
| socket path | T3 confirmed | `/tmp/umem.*.sock` absent; `/tmp/umem-1000/umem.<pid>.sock`, dir 0700, sock 0600 |
| umemctl `caches` | PASS (header says `inuse`, the server says `held`) | 68 caches listed |
| umemctl `cache <name>` / unknown name | PASS | full detail; `no such cache: nosuch` |
| umemctl `whatis` held 64 / 512 / 4 KiB / 96 KiB | PASS | correct cache, buffer, slab, `state held` |
| umemctl `whatis` held 256 KiB (oversize) | **WRONG -- T3 confirmed** | `not a umem buffer` for a live allocation |
| umemctl `whatis` freed into PTC bin (48 B) | **WRONG -- T3 confirmed** | `state held` for a freed buffer |
| umemctl `whatis` freed 96 KiB | **WRONG -- new, T6** | reported as a `umem_bufctl_cache` buffer (bufsize 24, buffer `addr-0x10`): mis-attributed to a different cache |
| umemctl `whatis 0x1` | PASS | `not a umem buffer` |
| umemctl `leaks` / `record --learn-leaks` without audit | PASS (refused as documented) | `requires UMEM_DEBUG=audit` |
| umemctl `record --learn-leaks` without audit | **WRONG -- new, T7** | prints the refusal AND `umemctl: wrote leak set to /tmp/ls0`, exit 0 |
| umemctl `logtail` / `record` (3 s) | inconclusive | only the `logtail start` banner in 3 s, `record` file 1 line; needs a longer run and a target that creates slabs |

New findings from this pass:

- **T6** `whatis` on a freed 96 KiB buffer names the wrong cache. Likely the
  buffer's slab was released back to vmem and the address was reused by a
  bufctl slab, so the answer is about whatever lives there now, not the
  freed buffer. Not diagnosed.
- **T7** `record --learn-leaks` reports success after the server refused:
  exit 0 and "wrote leak set" with an empty or refusal-only file.
- Debian bootstrap silently drops lldb: the apt package list names
  `jemalloc-bin`, which does not exist on Debian 13, so the whole install falls
  back to a list without lldb. `scripts/ec2/bootstrap.sh` bug.

Not yet run: umemctl `break` (all predicates), `break leaked --set`,
`continue`, `leaks`/`learn` under audit; every `tools/umem` command; every gdb
and lldb helper command, including `umem break` (T1 at the debugger level).

## Runtime pass, continued (agent @r5tools)

Build: `verify-isolated.sh intel-lo@r5tools 12b6336` =
`12b63367255ac41cd646b4faf15c7776de0fdeee`, `clean-regen.sh
--enable-introspect`, `-g -O2` (no `-fno-omit-frame-pointer`), Debian 13,
`c7i.2xlarge` i-0f24390ffd9e643c9, gdb 16.3, lldb 19.1.7, rseq fast path on,
PTC on. `kernel.yama.ptrace_scope=0`. Targets, all
`test/debugger/tool_target <info> 3 <secs>`:

- **A**: `UMEM_OPTIONS=introspect=1`
- **B**: A plus `UMEM_DEBUG=audit UMEM_LOGGING=transaction=1m`
- **E**: `UMEM_DEBUG=default`, 1 thread, `dupfree:8` (for the error event)

"Parked" means gdb `thread apply all bt` showed the thread in
`pthread_cond_wait <- umem_introspect_break_check <- _umem_alloc <- churn`.

### umemctl

| command | status | evidence |
|---|---|---|
| `break size=1500` (A) | PASS | `ok armed`; 3 of 3 churn threads parked, `#5 _umem_alloc (size=1500) at umem.c:4282`, `#6 churn () at tool_target.c:123`; `continue` -> `ok continue`, 0 parked 1 s later |
| `break size=24`, `size=70000`, `size=98304`, `cache=umem_alloc_1536`, `cache=umem_alloc_224` (A) | PASS | 3 threads parked within 5 s each; `continue` resumed |
| `break size=777` (A) | **SILENT NO-OP -- new, T8** | `ok armed`, 0 threads parked in 5 s; churn calls `umem_zalloc(777)` every iteration |
| `break cache=tt_user` (A) | **SILENT NO-OP -- T8** | `ok armed`, 0 parked in 5 s; churn calls `umem_cache_alloc(tt_user)` every iteration |
| `break size=200000` (A) | **SILENT NO-OP -- T8** | `ok armed`, 0 parked in 5 s; gdb shows a thread inside `vmem_alloc(size=200000) <- _umem_alloc:4294` at that moment |
| PTC-served sizes while armed (A, B) | PASS | the inlined PTC fast path is skipped while a break is armed (`likely(!umem_introspect_break_armed)`), so `size=24` parked 3 threads and `break leaked` parked on size 64 |
| `break seq=5000` (A) | PASS | one thread parked (`_umem_alloc size=4096`), others ran on |
| `break token=BREAK` (A) | PASS (as implemented: `seq=1`) | the next slow-path allocation parked (`size=512`); the doc's "recording-token flow" does not exist anywhere else, `token=<anything>` does the same |
| `break seq=3` after `break size=98304` (A) | **WRONG -- new, T12** | re-arming replaces the predicate, but the three threads already parked on `size=98304` stay parked until `continue`; with `UMEM_DEBUG=allverbose` the only `BREAK:` lines are the three from the first predicate |
| `break bogus=1` | PASS (rc 0) | `bad predicate: bogus=1` |
| `break cache=nosuch` | PASS-ish | `ok armed` for a cache that does not exist, never fires; no warning |
| `continue` with nothing armed | PASS | `ok continue` |
| `leaks` (B) | **WRONG -- new, T9** | 437 records, **every one `depth=1`**, and the one PC is inside libumem: 390 x `umem_cache_alloc_debug+457`, 47 x `umem_slab_alloc+920`. No caller frame, so `leak_site()` cannot be told from churn |
| `record --learn-leaks /tmp/r5B.set` (B) | WRONG -- T9 | rc 0, 442 `sig` lines, only 2 distinct PCs (the two libumem ones above), so the set is `{size} x {allocator PC}` |
| `break leaked --set /tmp/r5B.set` (B) | **WRONG -- T9** | `pushed 442 leak signatures`, `ok armed`; parked churn threads at `_umem_alloc size=64` and `size=4096` (not leaks), never at `leak_site` |
| `break leaked --set` with one `sig size=224 pc=...` (B) | **SILENT NO-OP -- T9** | 0 parked in 10 s. The leak is `umem_alloc(200)`; learn records the cache **bufsize** (224) and break_check compares the **request size** (200), so a learned set can never match its own leak. `break size=200` parks 3 threads in 3 s (control) |
| `break leaked --set` with `sig size=1500 pc=0x1` (A, allverbose) | WRONG -- T9 | 3 `BREAK: ... size=1500` lines: the PC is ignored (the code says so: `ponytail: size-only live match`); the doc says it matches "by size and allocation PC" |
| `record --learn-leaks` without audit (A) | **WRONG -- T7 confirmed** | stdout `learn: requires UMEM_DEBUG=audit`, stderr `umemctl: wrote leak set to /tmp/r5_t7.set`, rc 0; the file contains the refusal line |
| `leaks` without audit (A) | PASS (refused) | `leaks: requires UMEM_DEBUG=audit`, but rc 0 |
| `logtail`, 32 s (fresh A, slabs being created) | PASS | `slab_create 150 -> 151` in the window; output has `logtail start`, 8 `held` deltas and `slab_create total=151 (+1)`; `timeout` rc 124 |
| `record /tmp/r5A2_rec.log`, 32 s (fresh A) | PASS | 16 lines, file byte-identical to stdout, includes `slab_create total=152 (+1)` |
| `logtail`, 32 s (B, steady state) | PASS (quiet by design) | `slab_create 295 -> 295`, `bufs_held 4431 -> 4431`: only the banner. The stream emits only counter deltas; a steady-state target with churn served from magazines emits nothing, which reads like a hang |
| `stats` while a `logtail` is open (A) | **WRONG (documented in code, not in the doc) -- new, T11** | `stats` took 7003 ms: it waited for the 8 s logtail to end. The server is one client at a time (`umem_introspect.c` "ONE CLIENT AT A TIME"), so a running `logtail`/`record`/`monitor` blocks `continue` too. `docs/UMEMCTL.md` does not say so |
| `monitor --once` (A) | **WRONG -- T2 confirmed again** | `bufs in-use 0`, same second `caches` shows `umem_bufctl_cache ... 4793` held |
| `whatis freed_96k` (A, B, fresh runs) | **WRONG -- T6, diagnosed below** | 6 fresh A runs: `umem_alloc_98304` 1x, `umem_magazine_1` 4x, `umem_magazine_7` 1x; 3 fresh B runs: `umem_bufctl_audit_cache` 1x, `umem_magazine_1` 2x. `held_96k` right every time |
| `whatis freed_ptc` (B, audit) | PASS-ish | `state free`. Audit disables PTC for that class, so the buffer went to the slab |
| `whatis freed_ptc` (A) | WRONG -- T3 confirmed again | `state held` |
| `whatis held_big` (A, B) | WRONG -- T3 confirmed again | `not a umem buffer` |

### T6 diagnosis (no fix)

`whatis freed_96k` does not report on the 96 KiB buffer at all. It reports on
whichever **4 KiB slab lies just below it in memory**:

- The freed buffer's own slab is still live (`slab_base == freed_96k`, 1
  chunk, cache `umem_alloc_98304`; A: slab `0x7f4914ba6c90`). Nothing was
  released or reused.
- The answer slab (A: `0x7f4913a30fb8`) is a `umem_magazine_1` slab with
  `slab_base = 0x7f4913a30008`, which is **page start + colour 8**.
  `freed_96k - slab_base = 0xff8 < cache_slabsize (4096)`.
- `cmd_whatis()` (`umem_introspect.c` `slab_of`) accepts `addr - slab_base <
  cache_slabsize` and then keeps the candidate with the **smallest slabsize**.
  `slab_base` includes the colour offset (`umem.c` slab create:
  `sp->slab_base = slab + color`), so for any coloured 4 KiB slab the range
  runs `color` bytes past the slab's memory into the next page. The 98304
  slab (slabsize 98304) matches too, but the coloured 4096 one wins.
- It is not specific to freed or 96 KiB buffers: it applies to any address
  in the first `color` bytes of a page that follows a coloured small slab.
  `freed_96k` is page-aligned and happens to sit after one. Measured on A: of
  71 page-end addresses (first byte past a coloured 4 KiB slab), `umemctl
  whatis` attributed **62** to the slab below. The other 9 were not
  examined.
- The predecessor's hypothesis (slab released, address reused by a bufctl
  slab) is **refuted**: the 98304 slab is live and the attribution flips
  between runs with the colour layout, not with reuse.
- The same `base <= addr < base + cache_slabsize` test is in `umem_whatis()`
  (`umem_inspect.c` `whatis_visit`, used by `tools/umem`/gdb/lldb `whatis`)
  and in `UMEM_SLAB_MEMBER` (`umem_impl.h`, used by `umem_findslab` and the
  free-path checks in `umem.c`). `umem_whatis()` returns the first non-internal
  match instead of the smallest, so `tools/umem whatis` got `freed_96k` right
  on A and B. It is still wrong on a page-end address after a coloured user
  slab: `tools/umem whatis 0x7f7ebc5fd000` -> `0x7f7ebc5fcff0: umem_alloc_32
  ... ALLOCATED`, a buffer that is not at that address, in the slab below.
  The umem.c users were not probed (allocator code, out of scope here).

### tools/umem (gdb wrapper)

| command | status | evidence |
|---|---|---|
| `--pid A findleaks -n 3` | PASS | `5504 outstanding ... 13 classes`, PTC caveat printed, `<no stack; enable UMEM_DEBUG=audit>` |
| `--pid B findleaks -n 2` / `-f json` | WRONG -- T9 | every class `#0 0x7fe87fe7d1f9 in ??+0x0 ()` (inside libumem, unresolved); JSON `"stack":["0x7fe87fe7d1f9"]` |
| `--pid A findleaks -f json -n 0` | WRONG -- T5 confirmed | no class with bufsize > 131072, while `held_big[2]` (256 KiB) are live |
| `--pid A/B status` | PASS | 72 lines, `HELD` column + caveat footer |
| `--pid A log -n 5` | PASS | `transaction log disabled (UMEM_LOGGING=transaction=1m to enable)` |
| `--pid B log -n 2` | PASS (stacks T9) | `transaction log: 4698 records (showing 2)`, 1 frame each |
| `--pid A/B whatis held_4k` / `bufctl held_4k` | PASS | `umem_alloc_4096 (4096 bytes, ALLOCATED)`; B adds `stack (1 frame)` |
| `--pid A whatis freed_96k` | WRONG -- T3/T5 | `umem_alloc_98304 (98304 bytes, ALLOCATED)` for a freed buffer |
| `--pid B whatis freed_96k`, `whatis freed_ptc` | PASS | `CACHED (freed by the application; still resident in a magazine ...)` |
| `--pid A whatis freed_ptc` | WRONG -- T3 | `umem_alloc_48 (48 bytes, ALLOCATED)` |
| `--pid A/B whatis held_big` | WRONG -- T3 | `not in any umem cache` |
| `--pid A/B walk allocated -n 3` | PASS | `ALLOC 0x... size=16 cache=umem_magazine_1` |
| `--pid A/B walk freed -n 3` | PASS | `FREE  0x... size=16 cache=umem_magazine_1` |
| `--pid A walk log -n 3` (no log) | **WRONG -- new, T10** | `umem: gdb produced no report (the command produced no output).` rc 2. `log` on the same target prints `transaction log disabled`; `walk log` prints nothing, and the wrapper calls that an error |
| `--pid B walk log -n 2` | PASS (T9 stacks) | `? 0x7fe87e7faf80 size=100 cache=tt_user`: event kind printed as `?` |
| `--pid A/B snapshot /tmp/r5X.ums` | PASS | `wrote snapshot to ...` |
| `--dump r5A.ums / r5B.ums findleaks -n 2` | PASS | `findleaks (offline): 5503 buffers ...`; B has `STACK[0]` PCs |
| `--dump ... status` | PASS (label drift) | 70 lines; header still `INUSE` where live output says `HELD` |
| `--dump r5B.ums log -n 2` | PASS | 2 records with stacks |
| `--dump r5A.ums log -n 2` (no log) | SILENT | 0 lines, rc 0 (live `log` says "transaction log disabled") |
| `--core /tmp/r5core.78672 --exe tool_target findleaks` | **REFUSED -- as designed; T4 confirmed** | rc 2, `umem: --core is not supported and cannot be made to work as implemented.`; `tools/DEBUGGING.md` still advertises it |

### gdb helper (`gdb -p`, `source tools/gdb/umem_gdb.py`)

| command | status | evidence |
|---|---|---|
| `umem help` | PASS (incomplete) | lists no `walk` and no `bufctl` beyond an alias note; DEBUGGING.md's table has both |
| `umem findleaks -n 2`, `status`, `log -n 2`, `walk allocated|freed|log -n 2`, `whatis`, `bufctl`, `snapshot` (B) | PASS (T9 stacks, T3/T5 limits as above) | same output as `tools/umem`, which is this helper |
| `umem events on` / `off` | PASS (no effect, T1) | `umem events enabled` / `disabled` |
| control: `break _umem_alloc`, `continue` (A) | PASS | `Thread 3 hit Breakpoint 1, _umem_alloc (size=1500)` within 5 s |
| `umem break alloc`, `continue` 20 s (A) | **SILENT NO-OP -- T1 confirmed at the debugger level** | `Breakpoint 1 at ...: file umem_inspect.c, line 87`; the only stop in 20 s is the harness `SIGUSR2`; `info breakpoints` shows no hit |
| `umem break free`, 10 s (A) | **SILENT NO-OP -- T1** | only `SIGUSR2`, no hit |
| `umem break alloc -c tt_user`, 3 s (A) | SILENT NO-OP -- T1 | condition set, no hit |
| `umem break error`, dupfree (E) | **PASS** | `hit Breakpoint 1, umem_event_error (code=2, ...)`, `#1 umem_error (error=2) at umem.c:1190`, `#2 main () at tool_target.c:195`; `p code` -> 2. `umem_error()` calls the hook directly, bypassing the dead `umem_inspect_notify()`, so `break error` is the one event that works |
| `umem break error -c 99`, dupfree (E) | **WRONG -- new, T13** | message `breakpoint set on umem_event_error` with **no condition**; stopped on `code=2`. `-c` is parsed into `cache` and ignored for `error`; `code` is never assigned. lldb handles `-c` correctly (below) |

### lldb helper (`lldb`, `command script import tools/lldb/umem_lldb.py`, `process attach -p`)

| command | status | evidence |
|---|---|---|
| `umem help` | PASS | same list as gdb |
| `umem findleaks -n 2` (B) | PASS (T9 stacks) | `384 outstanding allocations ... 13 classes` |
| `umem status` (B) | **WRONG -- new, T14** | prints all 68 cache rows and the footer, then `error: library call failed: error: unknown error`; lldb `--batch` stops the rest of the script and exits 1. `status -f json` the same. Cause: `umem_status_dump` returns void, and lldb 19 flags every `(void)` expression result as `Fail()` with `error: unknown error` (0x1001) -- shown directly with `umem_inspect_enable_events(0)`; an `(int)` call on the same attach reports no error |
| `umem log -n 2` (B) | PASS | `transaction log: 4698 records (showing 2)` |
| `umem walk allocated|freed|log -n 2` (B) | PASS | same rows as gdb |
| `umem whatis held_4k`, `bufctl freed_96k`, `whatis held_big` (B) | PASS / PASS / T3 | `ALLOCATED` / `CACHED` / `not in any umem cache` |
| `umem snapshot` (B) | PASS | `wrote snapshot to /tmp/r5B_lldb.ums` |
| `umem events on` / `off` (B) | PASS (no effect, T1) | prints success; the `(void)` call's error is not checked here |
| control: `breakpoint set -n _umem_alloc` (A) | PASS | `stop reason = breakpoint 1.1 ... _umem_alloc(size=1500)` |
| `umem break alloc`, `continue` 20 s (A) | **SILENT NO-OP -- T1 confirmed** | `Breakpoint 1: where = libumem.so.1\`umem_event_alloc`; only stop is `signal SIGUSR2` |
| `umem break error`, dupfree (E) | PASS | `stop reason = breakpoint 1.1`, `umem_event_error(code=2, ...)`, `frame #2 main at tool_target.c:195`; `(int) code = 2` |
| `umem break error -c 99`, dupfree (E) | PASS | no stop at `code=2`; target ran on to `SIGABRT` (exit 134) |

### Findings status after this pass

- **T1 confirmed** on all three surfaces: C callback (predecessor),
  gdb `umem break alloc|free`, lldb `umem break alloc`, each with a positive
  control that stops within 5 s. `umem break error` works in both debuggers
  because `umem_error()` calls `umem_event_error()` directly.
- **T2 confirmed** again (`bufs in-use 0`).
- **T3 confirmed**: socket path (predecessor), oversize `not a umem buffer`
  in umemctl and `not in any umem cache` in tools/umem/gdb/lldb, PTC-freed
  buffer reads `held`/`ALLOCATED` without audit. `magazine reloads` is still
  in the doc and absent from `monitor` output.
- **T4 confirmed**: `--core` refused with rc 2; the doc still advertises it.
- **T5 confirmed** for oversize (no class > 128 KiB in `findleaks -n 0`
  with two 256 KiB buffers live). The PTC caveat is printed by `findleaks`
  itself but is absent from `tools/DEBUGGING.md`, whose caveat 2 says audit
  makes PTC "not an issue in practice".
- **T6 diagnosed**: colour-offset slab range overlap (above). Not a stale
  slab, not a reused address.
- **T7 confirmed**.
- New:
  - **T8** (MEDIUM): umemctl `break` sees only the `_umem_alloc` cache
    path. `umem_zalloc`, `umem_cache_alloc` (every user cache, and the
    `cache=` predicate on it), and oversize `umem_alloc` (> 128 KiB) never
    reach `umem_introspect_break_check()`, so `size=`/`cache=`/`seq=`/`leaked`
    silently never fire for them. The only call site is `umem.c` in
    `_umem_alloc` after `_umem_cache_alloc`. The docs promise "an allocation
    of exactly n bytes" and "an allocation from cache <name>".
  - **T9** (HIGH for the leak workflow): audit stacks on x86_64 are one frame
    deep and that frame is the allocator itself. `getpcstack()` walks frame
    pointers and the library is built `-O2` without
    `-fno-omit-frame-pointer` (`Makefile.am` already records the 2-frame
    depth for the P5.9 test). So `findleaks`, `leaks`, `whatis`, `log` and
    `learn` cannot name the leaking call site, which is their stated purpose.
    On top of that, `learn` writes `cache_bufsize` while `break leaked` tests
    the request size, so a learned set cannot match a leak whose size is not
    a cache size (tool_target's 200 -> 224), and the PC half of each
    signature is ignored. The two-phase "break before the leaked
    allocation" workflow does not work end to end. Not run on aarch64, which
    has its own walker.
  - **T10** (LOW): `tools/umem walk log` without a transaction log exits 2
    with "gdb produced no report" instead of the "log disabled" message
    `log` prints.
  - **T11** (LOW, doc): one client at a time; a running
    `logtail`/`record`/`monitor` blocks `stats` and `continue`.
  - **T12** (LOW): re-arming `break` while threads are parked leaves them
    parked on the retired predicate until `continue`.
  - **T13** (LOW): gdb `umem break error -c CODE` ignores the code.
  - **T14** (MEDIUM): lldb `umem status` always ends with an error and
    rc 1, which aborts the rest of a `--batch` script.
- Minor, recorded, no regression: `--dump status` header `INUSE` vs live
  `HELD`; `walk log` prints the event kind as `?`; `break cache=<unknown>`
  arms without a warning; `leaks`/`learn` refusals exit 0; `token=` is
  `seq=1` and ignores its argument.
- Debian bootstrap: confirmed on this box. `jemalloc-bin` has no candidate
  on Debian 13 (`apt-cache policy` knows only `libjemalloc2`), and
  `scripts/ec2/bootstrap.sh:26-31` falls back to a list without `lldb`
  when the first `apt-get install` fails. lldb-19 was installed by hand.
  Not fixed here.

## Regressions (XFAIL)

Rule: **every `XFAIL_TESTS` entry is an open finding.** A fix makes its test
exit 0, which `make check` reports as XPASS, a failure, until the entry is
removed from `XFAIL_TESTS`. `XFAIL_TESTS` must be empty before a release
claims the tools work. Exit codes: 0 fixed, 1 finding reproduces, 77 SKIP
(prerequisite missing), 99 ERROR (positive control failed or harness
broken). Shared harness: `test/debugger/tool_common.sh`.

`XFAIL_TESTS` (Makefile.am, commit `639a071`):

| finding | test | positive control |
|---|---|---|
| T1 | `test/debugger/test_tool_T1_event_cb.sh` | `test_event_cb` reports > 0 allocations made |
| T2 | `test/debugger/test_tool_T2_monitor.sh` | `umemctl stats` shows `bufs_held` > 0 |
| T7 | `test/debugger/test_tool_T7_learn_noaudit.sh` | `umemctl leaks` refuses with `requires UMEM_DEBUG=audit` |

Not yet covered, each **regression not yet written**: T3, T4, T6, T8, T9,
T10, T11, T12, T13, T14.

How they were shown to behave: `verify-isolated.sh intel-lo@r6xf 639a071` =
`639a071861e705e142b071a8127e2d3cc471bbb4`, Debian 13, `c7i.2xlarge`
i-07e18cf6a4c65488b (terminated). Log:
`docs/results/r6xf/tool-xfail-639a071-intel-lo.log`.

1. `clean-regen.sh --enable-introspect`, full `make check`: `TOTAL 57, PASS
   52, SKIP 2 (heap_ceiling, heap_ceiling_512), XFAIL 3, FAIL 0, XPASS 0,
   ERROR 0`. T1: `alloc=0 free=0 (1800 allocations made)`. T2: `stats`
   `bufs_held 4846`, `monitor --once` `bufs in-use 0`. T7: `leaks` refused;
   `record --learn-leaks` printed `learn: requires UMEM_DEBUG=audit` and
   `wrote leak set`, rc 0.
2. Default build, `make check TESTS='<the three>'`: T1 XFAIL (same `alloc=0
   free=0`), T2 and T7 SKIP (77, `built without --enable-introspect`); FAIL 0,
   XPASS 0, ERROR 0.
3. Harness-broken path (the override variable is in the scripts; pointing it
   at a missing binary was done only for this run):
   `TOOL_TARGET=/nonexistent/tool_target test_tool_T2_monitor.sh` -> rc 99;
   `TOOL_EVENT_CB=/nonexistent/test_event_cb test_tool_T1_event_cb.sh` ->
   rc 99.

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

## Round 7 status (@r7tooldoc, 2026-09-27)

Debian 13, c7i.2xlarge, `--enable-introspect`, all via `verify-isolated.sh`.
Logs: `docs/results/r7tooldoc/tool-{pre,post,pre36,post36}.log`.

| finding | regression | pre-fix (ref) | fix | post-fix |
|---|---|---|---|---|
| T2 | T2_monitor | XFAIL, `bufs in-use 0` vs `bufs_held 4656` (5143c2e) | 658238b | PASS, 4492 |
| T7 | T7_learn_noaudit | XFAIL, rc=0 (5143c2e) | 85501ff | PASS, rc=1 |
| T10 | T10_walk_log | XFAIL, rc=2 (5143c2e) | 08fb1a7 | PASS |
| T12 | T12_rearm | XFAIL, 1 BREAK line (5143c2e) | b0d676f | PASS, 2 |
| T13 | T13_gdb_error_code | XFAIL, stopped on code 2 (5143c2e) | 9d3aa14 | PASS |
| T14 | T14_lldb_status | XFAIL, `unknown error`, rc=1 (5143c2e) | 0c18ee1 | PASS |
| T3 (oversize) | T3_whatis_oversize | XFAIL, `not a umem buffer` (39569cd) | c254a6d | PASS, `arena umem_oversize size 262144` |
| T6 | T6_whatis_colour | XFAIL, freed_96k -> umem_magazine_1 (39569cd) | 0e395ef | PASS, 5/5 runs |
| T4, T5, T11, T3 doc | none (doc) | | 66f7a8c | |

Full `make check` at 0c18ee1: 61 total, 58 PASS, 2 SKIP, 1 XFAIL (T1), 0
FAIL/XPASS/ERROR. At 66f7a8c: 63 total, 60 PASS, 2 SKIP, 1 XFAIL (T1).

Still open from this set:
- T3 "cached": wired (cmd_whatis asks `umem_whatis()`), **unverified**. No
  target here puts a buffer in the magazine set where the branch can be shown
  to change. PTC-bin buffers read `held` by design: bins are thread-local and
  mutated lock-free by their owner.
- T6 sibling: `UMEM_SLAB_MEMBER` (`umem_impl.h`) has the same
  `slab_base + cache_slabsize` bound and is used on the umem.c free path. Not
  changed (allocator code, out of scope). Not probed.
- T1, T8, T9: @r7lib's.

## Status: T1 / T8 / T9 library fixes (agent @r7lib, stopped at the tool-call budget)

- **Committed `06a87cd`:** three new regressions, all in `XFAIL_TESTS`:
  `test_tool_T1_gdb_break.sh` (gdb `umem break alloc|free` must stop, control
  `break _umem_alloc`), `test_tool_T8_break_ptc.sh` (`break size=777`,
  `cache=tt_user`, `size=200000` must park, control `size=1500`, seen via
  the `BREAK:` line under `UMEM_DEBUG=allverbose`), and
  `test_tool_T9_leak_stack.sh` (a leak record's first pc must be in the
  target executable, and `learn` must emit `sig size=200`).
  **Not yet shown to fail pre-fix.** `verify-isolated.sh intel-lo@r7lib
  06a87cd prefix` was started (Debian, `--enable-introspect`), but
  `job.sh status|tail|fetch` returned nothing and I did not find out why.
  No result exists. Until a pre-fix run shows rc 1, treat all three as
  unverified: each could be vacuous, or could exit 99.
- **Fix drafted, NOT BUILT, not on master:** `origin refs/heads/r7lib-wip`
  = `40428b9`. One flag, `umem_hooks` (umem_base.h: bit EVENTS set by
  `umem_inspect_enable_events`, bit BREAK set by the umemctl arm), tested
  with `UMEM_HOOKS_ON()` at the public entry points `_umem_alloc` (now a
  wrapper over an always-inline body, so PTC and oversize returns are
  covered), `_umem_zalloc`, `_umem_alloc_align`, `_umem_free`,
  `_umem_free_align`, `_umem_cache_alloc/free` and both batch calls.
  `UMC_INTERNAL|UMC_QCACHE` caches are excluded, so a `umem_alloc` reports
  once. malloc reports through `umem_alloc` with the header base (marked
  `ponytail:`). The old `break_check` call in `_umem_alloc` is removed.
  Still owed: build on both arches, the pre/post test runs, the
  `hotpath_ab.sh` A/B (the wrapper changes the `_umem_alloc` layout, so a
  cost outside the null is a real possibility), and the gates.
- **T9 is incomplete in the draft.** Only the size half is there: `learn`
  decodes the request size from the buftag, and that works only with
  `UMF_BUFTAG`. `UMEM_DEBUG=audit` alone sets no buftag, so the size is
  still `cache_bufsize` there. The stack-depth diagnosis
  (`-fno-omit-frame-pointer` vs. the walk starting inside libumem) has
  **not been done**.
- EC2: intel-lo@r7lib and arm-lo@r7lib were terminated at stop. See the
  commit that adds this section.

## Status: T1 / T8 (agent @r7hook, 2026-09-27) -- fixed on a branch, NOT merged: default-off cost outside the null

Measured on Debian 13, c7i.2xlarge + c7g.2xlarge. Logs are in `docs/results/r7hook/`.

- **Pre-fix (5610d8a, `--enable-introspect`):** `test_tool_T1_event_cb`,
  `test_tool_T1_gdb_break` and `test_tool_T8_break_ptc` each exit **1** on
  both arches, and every control passes. `alloc=0 free=0 (1800 allocations
  made)`. gdb `break _umem_alloc` hits but `umem break alloc|free` never
  stops. `break size=1500` parks while `size=777`, `cache=tt_user` and
  `size=200000` do not. None exits 0 or 99, so no script needed a fix.
- **Fix = `a59e7d6`** on `origin/r7hook-fix`. This is the r7lib-wip draft
  40428b9 cherry-picked onto 1c1eac0 with no compile fixes needed, **minus
  its T9 `learn_printer` hunk** (T9 belongs to another agent). The same
  commit removes the three XFAIL lines.
- **Post-fix:** all three exit **0** on both arches:
  `alloc=1800 free=1800`, gdb stops in `umem_event_alloc`/`umem_event_free`,
  and all four break predicates park.
- **`make check -j1` at a59e7d6, both arches:** default 67 total, 57 PASS,
  10 SKIP, 0 FAIL/XPASS/ERROR. `--enable-introspect`: 67 total, 66 PASS,
  1 XFAIL (T9), 0 FAIL/XPASS/ERROR. The P8.5b lo gate (`--enable-rseq`)
  passed on both arches: 6-bug rseq, trailing store, commit safe/abort
  0/100, resize leak and alias. `test_first_alloc_small_stack`,
  `test_slab_member` and the 8t oracle ×4 all PASS. The gate is `-j1`
  because `-j8` OOM-killed `test_heap_ceiling[_512]` (exit 137; dmesg
  shows `Killed process test_heap_ceili`). That failure reproduces on
  1c1eac0 without the fix, and serial runs pass pre and post. It is fixed
  on master in 79cdd89, which a59e7d6 predates.
- **Hot-path A/B, default build (hooks off), pre=1c1eac0, post=a59e7d6,
  `hotpath_ab.sh`, 9 alternating pairs, one bench binary.** Every point is
  outside its null on both arches:

  | point | x86 A/B (null range) | arm A/B (null range) |
  |---|---|---|
  | 512 N=1 t=1 | −17.21 % (−0.12..+0.23) | −5.71 % (−0.09..+0.07) |
  | 512 N=64 t=1 | −7.94 % (−0.07..+0.11) | −2.84 % (−0.12..+0.03) |
  | 512 N=128 t=1 | −11.73 % (−0.12..+0.25) | −3.20 % (−0.23..+0.17) |
  | 16:64 N=1 t=1 | −8.93 % (−0.62..+1.40) | −3.81 % (−0.12..+0.13) |
  | 16:64 N=64 t=1 | −15.51 % (−0.12..+0.10) | −3.57 % (−0.22..+0.04) |
  | 16:1024 N=1 t=1 | −5.04 % (−0.22..+0.73) | −3.09 % (−0.23..+0.40) |
  | 16:1024 N=64 t=1 | −9.68 % (−0.38..+1.49) | −3.25 % (−0.35..+0.21) |
  | 512 N=1 t=8 | −6.16 % (−0.77..+1.43) | −5.95 % (−0.12..+0.03) |
  | 512 N=128 t=8 | −6.60 % (−0.82..+1.38) | −3.29 % (−0.05..+0.41) |
  | 16:64 N=1 t=8 | −2.37 % (−0.09..+1.25) | −3.87 % (−0.35..+0.42) |
  | 16:64 N=64 t=8 | −2.21 % (−0.72..+0.30) | −3.52 % (−0.61..+0.07) |
  | 16:1024 N=1 t=8 | −1.88 % (−0.07..+2.79)¹ | −3.18 % (−0.14..+0.05) |
  | 16:1024 N=64 t=8 | −3.22 % (−0.29..+0.72) | −3.38 % (−0.10..+0.16) |
  | insn/pair, 7 points | **+4.00..+5.29 %** (null ≤ ±0.14) | **+5.14..+6.69 %** (null ≤ ±0.38) |

  ¹ The median is outside the null, but this A/B's min..max (−4.63..+0.15)
  overlaps it. It is the only such point.

  That is about 7 extra instructions per alloc/free pair. One visible
  cause (x86 disassembly): `_umem_free` lost its shrink-wrapped prologue.
  Before the fix, the NULL test ran before any register push. After it,
  six pushes come first, because the hook call sits at the top of the
  function. This has not been profiled further, so it is not the whole
  explanation.
- **Not merged, coordinator's decision.** The CHANGELOG entry and the README
  "Known broken" edit sit on `r7hook-fix` on top of the fix. a59e7d6 is
  based on 1c1eac0, so it needs a rebase onto current master before it
  ships. Nothing was redesigned. Two options: ship with the cost on the
  record, or restructure the hook placement (for example, test in the
  PTC/magazine miss paths only and keep `_umem_free` shrink-wrappable)
  and re-measure.
- EC2: intel-lo@r7hook and arm-lo@r7hook terminated at stop.
