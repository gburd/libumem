"""
libumem GDB integration.

The heavy lifting is in libumem itself (umem_inspect.c).  This file is a
thin front-end: it exposes the mdb-style dcmds as GDB commands and, for
a live (running or paused) target, invokes the library's entry points
via `gdb.parse_and_eval("umem_findleaks(...)")`.

Commands:

    umem findleaks [-f text|json] [-n MAX_CLASSES]
    umem log       [-f text|json] [-n MAX_RECORDS]
    umem status    [-f text|json]
    umem whatis  <addr>
    umem bufctl  <addr>       # alias: bufctl_audit
    umem walk    <addr_cache_name>

    umem break alloc [-s SIZE] [-c CACHE]    # break on user allocs
    umem break free  [-a ADDR] [-c CACHE]    # break on user frees
    umem break error                         # break on detected corruption

    umem events on|off                       # arm/disarm the hook points

Load:

    (gdb) source /path/to/tools/gdb/umem_gdb.py
    (gdb) umem-help
"""

import os
import re
import shlex
import tempfile

import gdb


# ---------------------------------------------------------------------------
# Helpers.
# ---------------------------------------------------------------------------


def _symbol_exists(name: str) -> bool:
    """Check whether a global symbol is available in the inferior."""
    try:
        s = gdb.lookup_global_symbol(name)
        return s is not None
    except gdb.error:
        return False


def _ensure_loaded() -> None:
    """Raise if libumem isn't mapped into the inferior."""
    if not _symbol_exists("umem_null_cache"):
        raise gdb.GdbError(
            "libumem does not appear to be loaded in the inferior.\n"
            "Run the program first, then retry."
        )


def _ensure_running() -> None:
    """Raise if the inferior is not started (no calls possible)."""
    try:
        inf = gdb.selected_inferior()
        if inf is None or inf.pid == 0:
            raise gdb.GdbError(
                "This command requires a running or paused inferior.\n"
                "Use `run`, `start`, or `attach <pid>` first."
            )
    except gdb.error as exc:
        raise gdb.GdbError(str(exc))


def _call_library(expr: str) -> str:
    """Call a library function that writes to a tmpfile and return the
    file contents.  We use a tmpfile instead of stderr redirection
    because gdb's `call` captures stdout but libumem writes fputs() to
    stderr, which is messier to redirect from inside gdb."""
    with tempfile.NamedTemporaryFile(mode="r", suffix=".umi", delete=False) as tf:
        path = tf.name
    try:
        # Cast all libc calls explicitly: glibc's debug symbols often
        # lack return-type info under heavy optimisation, and gdb refuses
        # to call functions of unknown return type.
        fopen_expr = '((void *(*)(const char *, const char *)) fopen)("{}", "w")'.format(
            path.replace('"', '\\"')
        )
        fp = gdb.parse_and_eval(fopen_expr)
        if int(fp) == 0:
            raise gdb.GdbError("fopen({}) failed in inferior".format(path))
        full_expr = expr.replace("$OUT$", "(void *) {}".format(int(fp)))
        gdb.parse_and_eval(full_expr)
        gdb.parse_and_eval(
            "((int (*)(void *)) fclose)((void *) {})".format(int(fp))
        )
        with open(path, "r") as f:
            return f.read()
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


# ---------------------------------------------------------------------------
# Core command class (prefix 'umem').
# ---------------------------------------------------------------------------


class UmemPrefix(gdb.Command):
    """Top-level `umem` prefix; use tab-completion for subcommands."""

    def __init__(self) -> None:
        super().__init__("umem", gdb.COMMAND_USER, prefix=True)


UmemPrefix()


# ---------------------------------------------------------------------------
# `umem findleaks`
# ---------------------------------------------------------------------------


class UmemFindleaks(gdb.Command):
    """Report currently allocated buffers grouped by stack-trace fingerprint.

Usage:
    umem findleaks [-f text|json] [-n MAX_CLASSES]

Requires UMEM_DEBUG=audit for stack traces.  Without audit, counts are
still accurate; only the representative stack is absent.  Allocator-
internal caches (UMC_NOHASH / UMC_QCACHE) are filtered out by default.
"""

    def __init__(self) -> None:
        super().__init__("umem findleaks", gdb.COMMAND_USER)

    def invoke(self, arg: str, from_tty: bool) -> None:
        _ensure_loaded()
        _ensure_running()
        fmt, max_classes = "text", 50
        args = shlex.split(arg or "")
        while args:
            a = args.pop(0)
            if a in ("-f", "--format") and args:
                fmt = args.pop(0)
            elif a in ("-n", "--max-classes") and args:
                max_classes = int(args.pop(0))
            else:
                raise gdb.GdbError("unknown arg: {}".format(a))
        fmt_enum = {"text": 0, "json": 1, "csv": 2}.get(fmt)
        if fmt_enum is None:
            raise gdb.GdbError("unknown format: {}".format(fmt))
        out = _call_library(
            "(unsigned long) umem_findleaks($OUT$, {}, {})"
            .format(fmt_enum, max_classes)
        )
        gdb.write(out)


UmemFindleaks()


class UmemWalk(gdb.Command):
    """Stream every allocated/freed/log entry.

Usage:
    umem walk [allocated|freed|log] [-f text|json] [-n N]

Default kind is 'allocated'.
"""

    def __init__(self) -> None:
        super().__init__("umem walk", gdb.COMMAND_USER)

    def invoke(self, arg: str, from_tty: bool) -> None:
        _ensure_loaded()
        _ensure_running()
        kind = "allocated"
        fmt = "text"
        n = 0
        args = shlex.split(arg or "")
        while args:
            a = args.pop(0)
            if a in ("allocated", "freed", "log"):
                kind = a
            elif a in ("-f", "--format") and args:
                fmt = args.pop(0)
            elif a in ("-n", "--max") and args:
                n = int(args.pop(0))
            else:
                raise gdb.GdbError("unknown arg: {}".format(a))
        fmt_enum = {"text": 0, "json": 1}.get(fmt)
        if fmt_enum is None:
            raise gdb.GdbError("unknown format: {}".format(fmt))
        out = _call_library(
            '(unsigned long) umem_walk_dump($OUT$, "{}", {}, {})'
            .format(kind, fmt_enum, n)
        )
        gdb.write(out)


UmemWalk()


# ---------------------------------------------------------------------------
# `umem log`
# ---------------------------------------------------------------------------


class UmemLog(gdb.Command):
    """Dump the transaction log in chronological order.

Usage:
    umem log [-f text|json] [-n MAX_RECORDS]

Requires UMEM_LOGGING=transaction=<size> (e.g. 1m).  Each record is
one allocation or free event with thread, timestamp, and stack.
"""

    def __init__(self) -> None:
        super().__init__("umem log", gdb.COMMAND_USER)

    def invoke(self, arg: str, from_tty: bool) -> None:
        _ensure_loaded()
        _ensure_running()
        fmt, n = "text", 0
        args = shlex.split(arg or "")
        while args:
            a = args.pop(0)
            if a in ("-f", "--format") and args:
                fmt = args.pop(0)
            elif a in ("-n", "--max-records") and args:
                n = int(args.pop(0))
            else:
                raise gdb.GdbError("unknown arg: {}".format(a))
        fmt_enum = {"text": 0, "json": 1}.get(fmt)
        if fmt_enum is None:
            raise gdb.GdbError("unknown format: {}".format(fmt))
        out = _call_library(
            "(unsigned long) umem_log_dump($OUT$, {}, {})".format(fmt_enum, n)
        )
        gdb.write(out)


UmemLog()


# ---------------------------------------------------------------------------
# `umem status`
# ---------------------------------------------------------------------------


class UmemStatus(gdb.Command):
    """Per-cache status (bufsize, inuse, total, memory, allocs, fails).

Usage:
    umem status [-f text|json]
"""

    def __init__(self) -> None:
        super().__init__("umem status", gdb.COMMAND_USER)

    def invoke(self, arg: str, from_tty: bool) -> None:
        _ensure_loaded()
        _ensure_running()
        fmt = "text"
        args = shlex.split(arg or "")
        while args:
            a = args.pop(0)
            if a in ("-f", "--format") and args:
                fmt = args.pop(0)
            else:
                raise gdb.GdbError("unknown arg: {}".format(a))
        fmt_enum = {"text": 0, "json": 1}.get(fmt)
        if fmt_enum is None:
            raise gdb.GdbError("unknown format: {}".format(fmt))
        out = _call_library(
            "(void) umem_status_dump($OUT$, {})".format(fmt_enum)
        )
        gdb.write(out)


UmemStatus()


# ---------------------------------------------------------------------------
# `umem whatis` / `umem bufctl`
# ---------------------------------------------------------------------------


class UmemWhatis(gdb.Command):
    """Resolve an address to its owning cache, slab, and state.

Usage:
    umem whatis <addr>
"""

    def __init__(self) -> None:
        super().__init__("umem whatis", gdb.COMMAND_USER)

    def invoke(self, arg: str, from_tty: bool) -> None:
        _ensure_loaded()
        _ensure_running()
        if not arg.strip():
            raise gdb.GdbError("usage: umem whatis <addr>")
        addr = gdb.parse_and_eval(arg)
        out = _call_library(
            "(int) umem_bufctl_audit_dump($OUT$, (void *){})"
            .format(int(addr))
        )
        gdb.write(out)


class UmemBufctl(gdb.Command):
    """Alias for `umem whatis`; pretty-prints the bufctl_audit record."""

    def __init__(self) -> None:
        super().__init__("umem bufctl", gdb.COMMAND_USER)

    def invoke(self, arg: str, from_tty: bool) -> None:
        gdb.execute("umem whatis {}".format(arg))


UmemWhatis()
UmemBufctl()


# ---------------------------------------------------------------------------
# Breakpoints -- the core of the "debug naturally" UX.
# ---------------------------------------------------------------------------


_EVENT_SYMBOLS = {
    "alloc": "umem_event_alloc",
    "free":  "umem_event_free",
    "error": "umem_event_error",
}


class UmemBreak(gdb.Command):
    """Set a conditional breakpoint on a umem memory event.

Usage:
    umem break alloc [-s SIZE] [-c CACHE]
    umem break free  [-a ADDR] [-c CACHE]
    umem break error [-c CODE]

Examples:
    umem break alloc -s 1048576        # any allocation >= 1 MB
    umem break free  -a 0x7f1234000    # when this address is freed
    umem break alloc -c umem_alloc_64  # only in the 64-byte size class
    umem break error                   # any detected corruption

The event hooks are disabled by default for ~1 cycle/alloc overhead.
This command automatically runs `umem events on` if needed.

Inside the breakpoint you have live access to `buf`, `size`, and
`cache` arguments.  Typical follow-ups:
    bt
    umem whatis buf
    p (char[16]) *buf
"""

    def __init__(self) -> None:
        super().__init__("umem break", gdb.COMMAND_USER)

    def invoke(self, arg: str, from_tty: bool) -> None:
        _ensure_loaded()
        args = shlex.split(arg or "")
        if not args:
            raise gdb.GdbError(
                "usage: umem break alloc|free|error [flags]"
            )
        ev = args.pop(0)
        sym = _EVENT_SYMBOLS.get(ev)
        if sym is None:
            raise gdb.GdbError(
                "unknown event '{}' (expected alloc, free, error)".format(ev)
            )

        size = None
        addr = None
        cache = None
        code = None
        while args:
            a = args.pop(0)
            if a in ("-s", "--size") and args:
                size = int(args.pop(0), 0)
            elif a in ("-a", "--addr") and args:
                addr = args.pop(0)
            elif a in ("-c", "--cache") and args:
                cache = args.pop(0)
            else:
                raise gdb.GdbError("unknown arg: {}".format(a))

        conds = []
        if size is not None:
            conds.append("size >= {}".format(size))
        if addr is not None:
            conds.append("buf == (void *){}".format(addr))
        if cache is not None and ev != "error":
            conds.append(
                'strcmp(((umem_cache_t *)cache)->cache_name, "{}") == 0'
                .format(cache)
            )
        if code is not None and ev == "error":
            conds.append("code == {}".format(code))

        cond = ""
        if conds:
            cond = " if " + " && ".join(conds)

        try:
            _ensure_running()
            gdb.execute("call (void) umem_inspect_enable_events(1)")
        except gdb.GdbError:
            # Will apply once the inferior starts.
            pass

        cmd = "break {}{}".format(sym, cond)
        gdb.execute(cmd)
        gdb.write(
            "breakpoint set on {}{}; remember to `umem events on` "
            "if events weren't already enabled.\n".format(sym, cond or "")
        )


UmemBreak()


class UmemEvents(gdb.Command):
    """Enable or disable the umem event hook points.

Usage:
    umem events on
    umem events off

When off (default), the hook functions return immediately with ~1
cycle of overhead per alloc/free.  When on, `umem break` breakpoints
fire and any C callback registered via umem_inspect_set_event_cb runs.
"""

    def __init__(self) -> None:
        super().__init__("umem events", gdb.COMMAND_USER)

    def invoke(self, arg: str, from_tty: bool) -> None:
        _ensure_loaded()
        _ensure_running()
        arg = (arg or "").strip().lower()
        if arg in ("on", "1", "enable"):
            gdb.execute("call (void) umem_inspect_enable_events(1)")
            gdb.write("umem events enabled\n")
        elif arg in ("off", "0", "disable"):
            gdb.execute("call (void) umem_inspect_enable_events(0)")
            gdb.write("umem events disabled\n")
        else:
            raise gdb.GdbError("usage: umem events on|off")


UmemEvents()


# ---------------------------------------------------------------------------
# `umem snapshot`
# ---------------------------------------------------------------------------


class UmemSnapshot(gdb.Command):
    """Dump a human-readable snapshot to a file.

Usage:
    umem snapshot <path>
"""

    def __init__(self) -> None:
        super().__init__("umem snapshot", gdb.COMMAND_USER)

    def invoke(self, arg: str, from_tty: bool) -> None:
        _ensure_loaded()
        _ensure_running()
        path = (arg or "").strip()
        if not path:
            raise gdb.GdbError("usage: umem snapshot <path>")
        res = gdb.parse_and_eval(
            '(int) umem_inspect_snapshot("{}")'.format(path.replace('"', '\\"'))
        )
        if int(res) != 0:
            raise gdb.GdbError("snapshot failed (errno)")
        gdb.write("wrote snapshot to {}\n".format(path))


UmemSnapshot()


# ---------------------------------------------------------------------------
# Help.
# ---------------------------------------------------------------------------


class UmemHelp(gdb.Command):
    """Show libumem GDB integration help."""

    def __init__(self) -> None:
        super().__init__("umem help", gdb.COMMAND_SUPPORT)

    def invoke(self, arg: str, from_tty: bool) -> None:
        gdb.write("""\
libumem GDB integration
=======================

Live dcmds (require a running/paused inferior):
    umem findleaks [-f text|json] [-n N]
    umem log       [-f text|json] [-n N]
    umem status    [-f text|json]
    umem whatis  <addr>          alias: umem bufctl <addr>
    umem snapshot <path>

Breakpoints on memory events:
    umem events on                        # arm the hook points
    umem break alloc [-s SIZE] [-c CACHE]
    umem break free  [-a ADDR] [-c CACHE]
    umem break error [-c CODE]
    umem events off

Environment required for the richest output:
    UMEM_DEBUG=audit                 # record stack trace per buffer
    UMEM_LOGGING=transaction=1m      # enable transaction log

See tools/DEBUGGING.md for worked examples.
""")


UmemHelp()


# Backwards-compatible aliases for the old hyphenated names.
gdb.execute("define umem-findleaks\n  umem findleaks $arg0\nend", to_string=True)
gdb.execute("define umem-log\n  umem log $arg0\nend", to_string=True)
gdb.execute("define umem-status\n  umem status $arg0\nend", to_string=True)
gdb.execute("define umem-whatis\n  umem whatis $arg0\nend", to_string=True)
gdb.execute("define umem-help\n  umem help\nend", to_string=True)


print("libumem GDB extension loaded.  Try `umem help`.")
