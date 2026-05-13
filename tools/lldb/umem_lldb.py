"""
libumem LLDB integration.

Mirrors the GDB integration in tools/gdb/umem_gdb.py.  Calls into the
library entry points exposed by umem_inspect.h on a live target.

Load:

    (lldb) command script import /path/to/tools/lldb/umem_lldb.py
    (lldb) umem help

Commands:

    umem findleaks [-f text|json] [-n MAX_CLASSES]
    umem log       [-f text|json] [-n MAX_RECORDS]
    umem status    [-f text|json]
    umem whatis  <addr>
    umem bufctl  <addr>
    umem snapshot <path>
    umem break alloc [-s SIZE] [-c CACHE]
    umem break free  [-a ADDR] [-c CACHE]
    umem break error [-c CODE]
    umem events on|off
"""

import os
import shlex
import tempfile

import lldb


# ---------------------------------------------------------------------------
# Inferior-call helpers.
# ---------------------------------------------------------------------------


def _target(debugger):
    return debugger.GetSelectedTarget()


def _process(target):
    return target.GetProcess()


def _is_live(debugger, result):
    t = _target(debugger)
    if not t or not t.IsValid():
        result.SetError("no target selected; `target create <binary>` first")
        return False
    p = _process(t)
    if not p or not p.IsValid() or p.GetState() == lldb.eStateExited:
        result.SetError(
            "no running/paused process; `process launch` or `process attach`"
        )
        return False
    return True


def _lookup_global(target, name):
    s = target.FindFirstGlobalVariable(name)
    if s and s.IsValid():
        return s
    return None


def _evaluate(debugger, expr):
    """Run an expression in the inferior and return the ValueObject."""
    target = _target(debugger)
    opts = lldb.SBExpressionOptions()
    opts.SetIgnoreBreakpoints(True)
    opts.SetTryAllThreads(True)
    opts.SetTimeoutInMicroSeconds(60 * 1000 * 1000)  # 60s
    opts.SetUnwindOnError(True)
    # Don't trap on fork() inside the inferior -- the addr2line
    # fallback used by umem_stacktrace forks a helper process.
    if hasattr(opts, "SetStopOthers"):
        opts.SetStopOthers(False)
    return target.EvaluateExpression(expr, opts)


def _call_library(debugger, result, expr):
    """Call a library function that writes to a tmpfile and return the
    file contents."""
    with tempfile.NamedTemporaryFile(
        mode="r", suffix=".umi", delete=False
    ) as tf:
        path = tf.name
    try:
        # lldb is stricter than gdb about function-pointer return
        # types and parameter types -- cast everything explicitly.
        fp_val = _evaluate(
            debugger,
            '((void *(*)(const char *, const char *)) fopen)("{}", "w")'
            .format(path.replace('"', '\\"')),
        )
        if not fp_val.IsValid() or fp_val.GetValueAsUnsigned() == 0:
            result.SetError("fopen failed in inferior (errno?)")
            return None
        fp = fp_val.GetValueAsUnsigned()
        # libumem entry points all take FILE* as first arg.  Cast the
        # tmpfile pointer through (void *) -> the inferior's FILE *.
        full = expr.replace("$OUT$", "((struct _IO_FILE *) {})".format(fp))
        v = _evaluate(debugger, full)
        if v.GetError() is not None and v.GetError().Fail():
            result.SetError(
                "library call failed: " + str(v.GetError())
            )
        _evaluate(
            debugger,
            "((int (*)(void *)) fclose)((void *) {})".format(fp),
        )
        with open(path, "r") as f:
            return f.read()
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


def _ensure_loaded(debugger, result):
    target = _target(debugger)
    if _lookup_global(target, "umem_null_cache") is None:
        result.SetError(
            "libumem not loaded in the inferior; start the target first"
        )
        return False
    return True


# ---------------------------------------------------------------------------
# Command dispatcher: one parent command with subcommand routing.
# ---------------------------------------------------------------------------


def _fmt_to_enum(s):
    return {"text": 0, "json": 1, "csv": 2}.get(s)


def _umem_findleaks(debugger, argv, result):
    if not _is_live(debugger, result) or not _ensure_loaded(debugger, result):
        return
    fmt, n = "text", 50
    while argv:
        a = argv.pop(0)
        if a in ("-f", "--format") and argv:
            fmt = argv.pop(0)
        elif a in ("-n", "--max-classes") and argv:
            n = int(argv.pop(0))
        else:
            result.SetError("unknown arg: {}".format(a))
            return
    fe = _fmt_to_enum(fmt)
    if fe is None:
        result.SetError("unknown format: {}".format(fmt))
        return
    out = _call_library(
        debugger, result,
        "(unsigned long) umem_findleaks($OUT$, "
        "(umem_inspect_format_t){}, (unsigned){})".format(fe, n),
    )
    if out is not None:
        result.Print(out)


def _umem_log(debugger, argv, result):
    if not _is_live(debugger, result) or not _ensure_loaded(debugger, result):
        return
    fmt, n = "text", 0
    while argv:
        a = argv.pop(0)
        if a in ("-f", "--format") and argv:
            fmt = argv.pop(0)
        elif a in ("-n", "--max-records") and argv:
            n = int(argv.pop(0))
        else:
            result.SetError("unknown arg: {}".format(a))
            return
    fe = _fmt_to_enum(fmt)
    if fe is None:
        result.SetError("unknown format: {}".format(fmt))
        return
    out = _call_library(
        debugger, result,
        "(unsigned long) umem_log_dump($OUT$, "
        "(umem_inspect_format_t){}, (unsigned){})".format(fe, n),
    )
    if out is not None:
        result.Print(out)


def _umem_status(debugger, argv, result):
    if not _is_live(debugger, result) or not _ensure_loaded(debugger, result):
        return
    fmt = "text"
    while argv:
        a = argv.pop(0)
        if a in ("-f", "--format") and argv:
            fmt = argv.pop(0)
        else:
            result.SetError("unknown arg: {}".format(a))
            return
    fe = _fmt_to_enum(fmt)
    if fe is None:
        result.SetError("unknown format: {}".format(fmt))
        return
    out = _call_library(
        debugger, result,
        "(void) umem_status_dump($OUT$, (umem_inspect_format_t){})"
        .format(fe),
    )
    if out is not None:
        result.Print(out)


def _umem_whatis(debugger, argv, result):
    if not _is_live(debugger, result) or not _ensure_loaded(debugger, result):
        return
    if not argv:
        result.SetError("usage: umem whatis <addr>")
        return
    addr_expr = " ".join(argv)
    addr_val = _evaluate(debugger, addr_expr)
    if not addr_val.IsValid():
        result.SetError("cannot evaluate address: {}".format(addr_expr))
        return
    out = _call_library(
        debugger, result,
        "(int) umem_bufctl_audit_dump($OUT$, (void *){})"
        .format(addr_val.GetValueAsUnsigned()),
    )
    if out is not None:
        result.Print(out)


def _umem_snapshot(debugger, argv, result):
    if not _is_live(debugger, result) or not _ensure_loaded(debugger, result):
        return
    if not argv:
        result.SetError("usage: umem snapshot <path>")
        return
    path = argv[0]
    v = _evaluate(
        debugger, '(int) umem_inspect_snapshot("{}")'.format(
            path.replace('"', '\\"'))
    )
    if v.GetValueAsSigned() != 0:
        result.SetError("snapshot failed")
        return
    result.Print("wrote snapshot to {}\n".format(path))


_EVENT_SYMBOLS = {
    "alloc": "umem_event_alloc",
    "free":  "umem_event_free",
    "error": "umem_event_error",
}


def _umem_break(debugger, argv, result):
    if not _is_live(debugger, result) or not _ensure_loaded(debugger, result):
        return
    if not argv:
        result.SetError("usage: umem break alloc|free|error [-s N] [-a ADDR] [-c NAME]")
        return
    ev = argv.pop(0)
    sym = _EVENT_SYMBOLS.get(ev)
    if sym is None:
        result.SetError("unknown event {}".format(ev))
        return
    size = None
    addr = None
    cache = None
    code = None
    while argv:
        a = argv.pop(0)
        if a in ("-s", "--size") and argv:
            size = int(argv.pop(0), 0)
        elif a in ("-a", "--addr") and argv:
            addr = argv.pop(0)
        elif a in ("-c", "--cache") and argv:
            if ev == "error":
                code = int(argv.pop(0), 0)
            else:
                cache = argv.pop(0)
        else:
            result.SetError("unknown arg: {}".format(a))
            return

    cond = None
    parts = []
    if size is not None:
        parts.append("size >= {}".format(size))
    if addr is not None:
        parts.append("buf == (void *){}".format(addr))
    if cache is not None:
        parts.append(
            'strcmp(((umem_cache_t *)cache)->cache_name, "{}") == 0'
            .format(cache)
        )
    if code is not None:
        parts.append("code == {}".format(code))
    if parts:
        cond = " && ".join(parts)

    _evaluate(debugger, "(void) umem_inspect_enable_events(1)")
    ci = debugger.GetCommandInterpreter()
    res = lldb.SBCommandReturnObject()
    if cond:
        ci.HandleCommand(
            'breakpoint set -n {} -c "{}"'.format(sym, cond.replace('"', '\\"')),
            res,
        )
    else:
        ci.HandleCommand("breakpoint set -n {}".format(sym), res)
    if res.Succeeded():
        result.Print(res.GetOutput() or "")
    else:
        result.SetError(res.GetError())


def _umem_events(debugger, argv, result):
    if not _is_live(debugger, result) or not _ensure_loaded(debugger, result):
        return
    if not argv:
        result.SetError("usage: umem events on|off")
        return
    a = argv[0].lower()
    on = a in ("on", "1", "enable")
    off = a in ("off", "0", "disable")
    if not (on or off):
        result.SetError("usage: umem events on|off")
        return
    _evaluate(
        debugger,
        "(void) umem_inspect_enable_events({})".format(1 if on else 0),
    )
    result.Print("umem events {}\n".format("enabled" if on else "disabled"))


def _umem_help(debugger, argv, result):
    result.Print("""\
libumem LLDB integration
========================

Live dcmds:
    umem findleaks [-f text|json] [-n N]
    umem log       [-f text|json] [-n N]
    umem status    [-f text|json]
    umem whatis <addr>                    alias: umem bufctl <addr>
    umem snapshot <path>

Breakpoints on memory events:
    umem events on
    umem break alloc [-s SIZE] [-c CACHE]
    umem break free  [-a ADDR] [-c CACHE]
    umem break error [-c CODE]
    umem events off

Environment:
    UMEM_DEBUG=audit
    UMEM_LOGGING=transaction=1m

See tools/DEBUGGING.md for worked examples.
""")


def _umem_walk(debugger, argv, result):
    if not _is_live(debugger, result) or not _ensure_loaded(debugger, result):
        return
    kind, fmt, n = "allocated", "text", 0
    while argv:
        a = argv.pop(0)
        if a in ("allocated", "freed", "log"):
            kind = a
        elif a in ("-f", "--format") and argv:
            fmt = argv.pop(0)
        elif a in ("-n", "--max") and argv:
            n = int(argv.pop(0))
        else:
            result.SetError("unknown arg: {}".format(a))
            return
    fe = _fmt_to_enum(fmt)
    if fe is None:
        result.SetError("unknown format: {}".format(fmt))
        return
    out = _call_library(
        debugger, result,
        '(unsigned long) umem_walk_dump($OUT$, "{}", '
        '(umem_inspect_format_t){}, (unsigned){})'.format(
            kind, fe, n),
    )
    if out is not None:
        result.Print(out)


_DISPATCH = {
    "findleaks": _umem_findleaks,
    "log":       _umem_log,
    "status":    _umem_status,
    "whatis":    _umem_whatis,
    "bufctl":    _umem_whatis,
    "snapshot":  _umem_snapshot,
    "walk":      _umem_walk,
    "break":     _umem_break,
    "events":    _umem_events,
    "help":      _umem_help,
}


def umem_cmd(debugger, command, result, internal_dict):
    """Top-level `umem` command dispatcher."""
    argv = shlex.split(command or "")
    if not argv:
        _umem_help(debugger, [], result)
        return
    sub = argv.pop(0)
    fn = _DISPATCH.get(sub)
    if fn is None:
        result.SetError(
            "unknown subcommand '{}'; try `umem help`".format(sub)
        )
        return
    fn(debugger, argv, result)


# ---------------------------------------------------------------------------
# Registration.
# ---------------------------------------------------------------------------


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand(
        'command script add -f umem_lldb.umem_cmd umem'
    )
    print("libumem LLDB extension loaded.  Try `umem help`.")
