"""
LLDB Python extension for debugging libumem

Usage:
    (lldb) command script import /path/to/umem_lldb.py
    (lldb) umem-cache-list
    (lldb) umem-whatis 0x7ffff7fb8040
    (lldb) umem-bufinfo 0x7ffff7fb8040
    (lldb) umem-leak-detect
    (lldb) umem-stats
"""

import lldb
import sys

class UmemError(Exception):
    """Exception for umem-specific errors"""
    pass


class UmemCache:
    """Wrapper for umem_cache_t structure"""

    def __init__(self, cache_value):
        self.value = cache_value

    @property
    def name(self):
        """Get cache name"""
        try:
            name_field = self.value.GetChildMemberWithName('cache_name')
            return name_field.GetSummary().strip('"')
        except:
            return "<unknown>"

    @property
    def bufsize(self):
        """Get buffer size"""
        try:
            size_field = self.value.GetChildMemberWithName('cache_bufsize')
            return size_field.GetValueAsUnsigned()
        except:
            return 0

    @property
    def alloc_count(self):
        """Get allocation count"""
        try:
            alloc_field = self.value.GetChildMemberWithName('cache_alloc')
            return alloc_field.GetValueAsUnsigned()
        except:
            return 0

    @property
    def free_count(self):
        """Get free count"""
        try:
            free_field = self.value.GetChildMemberWithName('cache_free')
            return free_field.GetValueAsUnsigned()
        except:
            return 0

    @property
    def bufs_inuse(self):
        """Get buffers in use"""
        return self.alloc_count - self.free_count

    def __str__(self):
        return f"{self.name:20s} {self.bufsize:6d}  {self.alloc_count:8d}  {self.free_count:8d}  {self.bufs_inuse:8d}"


def get_cache_list(target):
    """Iterate through all umem caches"""
    try:
        # Find umem_null_cache
        null_cache_list = target.FindFirstGlobalVariable('umem_null_cache')
        if not null_cache_list.IsValid():
            raise UmemError("Could not find umem_null_cache")

        caches = []
        current = null_cache_list.GetChildMemberWithName('cache_next')

        visited = set()
        max_iterations = 1000

        for _ in range(max_iterations):
            addr = current.GetValueAsUnsigned()
            if addr == 0 or addr in visited:
                break
            visited.add(addr)

            try:
                cache = UmemCache(current.Dereference())
                caches.append(cache)
                current = current.Dereference().GetChildMemberWithName('cache_next')
            except:
                break

        return caches

    except Exception as e:
        raise UmemError(f"Failed to get cache list: {e}")


def umem_cache_list(debugger, command, result, internal_dict):
    """List all umem caches"""
    target = debugger.GetSelectedTarget()

    if not target:
        result.AppendMessage("No target selected")
        return

    try:
        caches = get_cache_list(target)

        if not caches:
            result.AppendMessage("No caches found")
            return

        result.AppendMessage(f"{'Cache Name':20s} {'Size':>6s}  {'Alloc':>8s}  {'Free':>8s}  {'InUse':>8s}")
        result.AppendMessage("-" * 60)

        for cache in caches:
            result.AppendMessage(str(cache))

        result.AppendMessage(f"\nTotal: {len(caches)} caches")

    except UmemError as e:
        result.AppendMessage(f"Error: {e}")
    except Exception as e:
        result.AppendMessage(f"Unexpected error: {e}")
        import traceback
        result.AppendMessage(traceback.format_exc())


def umem_whatis(debugger, command, result, internal_dict):
    """Identify which cache owns an address"""
    if not command:
        result.AppendMessage("Usage: umem-whatis <address>")
        return

    target = debugger.GetSelectedTarget()
    if not target:
        result.AppendMessage("No target selected")
        return

    try:
        # Evaluate address
        value = target.EvaluateExpression(command)
        if not value.IsValid():
            result.AppendMessage(f"Invalid address: {command}")
            return

        addr = value.GetValueAsUnsigned()
        result.AppendMessage(f"Address: 0x{addr:x}")

        # Try to find cache (simplified)
        result.AppendMessage("  Cache: <not implemented>")
        result.AppendMessage("  Note: Full implementation requires slab walking")

    except Exception as e:
        result.AppendMessage(f"Error: {e}")


def umem_bufinfo(debugger, command, result, internal_dict):
    """Show detailed buffer information"""
    if not command:
        result.AppendMessage("Usage: umem-bufinfo <address>")
        return

    target = debugger.GetSelectedTarget()
    if not target:
        result.AppendMessage("No target selected")
        return

    try:
        value = target.EvaluateExpression(command)
        if not value.IsValid():
            result.AppendMessage(f"Invalid address: {command}")
            return

        addr = value.GetValueAsUnsigned()
        result.AppendMessage(f"Buffer: 0x{addr:x}")
        result.AppendMessage("  Cache: <not implemented>")
        result.AppendMessage("\nNote: Stack traces require audit mode (UMEM_DEBUG=audit)")
        result.AppendMessage("  If audit is enabled, detailed information would appear here.")

    except Exception as e:
        result.AppendMessage(f"Error: {e}")


def umem_leak_detect(debugger, command, result, internal_dict):
    """Scan for memory leaks"""
    target = debugger.GetSelectedTarget()
    if not target:
        result.AppendMessage("No target selected")
        return

    result.AppendMessage("Scanning for memory leaks...")
    result.AppendMessage("\nNote: This is a simplified implementation.")
    result.AppendMessage("Full leak detection requires:")
    result.AppendMessage("  1. Audit mode enabled (UMEM_DEBUG=audit)")
    result.AppendMessage("  2. Access to allocation records")
    result.AppendMessage("  3. Reachability analysis from roots")
    result.AppendMessage("\nFor production leak detection, consider:")
    result.AppendMessage("  - Valgrind with --leak-check=full")
    result.AppendMessage("  - AddressSanitizer with leak detection")
    result.AppendMessage("  - heap profilers (gperftools, jemalloc profiling)")

    try:
        caches = get_cache_list(target)
        total_inuse = 0

        result.AppendMessage(f"\n{'Cache':20s} {'Buffers In Use':>15s}")
        result.AppendMessage("-" * 40)

        for cache in caches:
            inuse = cache.bufs_inuse
            if inuse > 0:
                result.AppendMessage(f"{cache.name:20s} {inuse:15d}")
                total_inuse += inuse

        if total_inuse > 0:
            result.AppendMessage(f"\nTotal buffers in use: {total_inuse}")
        else:
            result.AppendMessage("\nNo allocated buffers found")

    except Exception as e:
        result.AppendMessage(f"Error: {e}")


def umem_stats(debugger, command, result, internal_dict):
    """Show allocation statistics"""
    target = debugger.GetSelectedTarget()
    if not target:
        result.AppendMessage("No target selected")
        return

    try:
        caches = get_cache_list(target)

        total_allocs = 0
        total_frees = 0
        total_inuse = 0
        total_bytes = 0

        for cache in caches:
            total_allocs += cache.alloc_count
            total_frees += cache.free_count
            inuse = cache.bufs_inuse
            total_inuse += inuse
            total_bytes += inuse * cache.bufsize

        result.AppendMessage("Cache Statistics:")
        result.AppendMessage(f"  Total allocations: {total_allocs:,}")
        result.AppendMessage(f"  Total frees:       {total_frees:,}")
        result.AppendMessage(f"  Currently in use:  {total_inuse:,} buffers ({total_bytes:,} bytes)")

        # Show top caches by usage
        sorted_caches = sorted(
            [(c, c.bufs_inuse) for c in caches],
            key=lambda x: x[1],
            reverse=True
        )

        result.AppendMessage("\nTop caches by usage:")
        result.AppendMessage(f"{'Cache':20s} {'Buffers':>10s} {'Size':>10s} {'Total':>12s}")
        result.AppendMessage("-" * 55)

        for cache, inuse in sorted_caches[:10]:
            if inuse > 0:
                total = inuse * cache.bufsize
                result.AppendMessage(f"{cache.name:20s} {inuse:10d} {cache.bufsize:10d} {total:12,} bytes")

    except Exception as e:
        result.AppendMessage(f"Error: {e}")


def umem_help(debugger, command, result, internal_dict):
    """Show help for umem debugging commands"""
    result.AppendMessage("libumem LLDB Debugging Commands")
    result.AppendMessage("=" * 60)
    result.AppendMessage("")
    result.AppendMessage("umem-cache-list      List all memory caches")
    result.AppendMessage("umem-whatis <addr>   Identify which cache owns an address")
    result.AppendMessage("umem-bufinfo <addr>  Show detailed buffer information")
    result.AppendMessage("umem-leak-detect     Scan for memory leaks")
    result.AppendMessage("umem-stats           Show allocation statistics")
    result.AppendMessage("umem-help            Show this help")
    result.AppendMessage("")
    result.AppendMessage("Debug Modes (set via UMEM_DEBUG environment variable):")
    result.AppendMessage("  default     - Default debug features")
    result.AppendMessage("  audit       - Transaction logging with stack traces")
    result.AppendMessage("  contents    - Fill buffers with patterns")
    result.AppendMessage("  guards      - Red-zone guards")
    result.AppendMessage("  verify      - Consistency checks")
    result.AppendMessage("  firewall    - Guard pages")
    result.AppendMessage("  deadbeef    - Fill freed memory")
    result.AppendMessage("")
    result.AppendMessage("Example usage:")
    result.AppendMessage("  (lldb) command script import /path/to/umem_lldb.py")
    result.AppendMessage("  (lldb) run")
    result.AppendMessage("  (lldb) umem-cache-list")
    result.AppendMessage("  (lldb) umem-whatis $rax")
    result.AppendMessage("  (lldb) umem-bufinfo 0x7ffff7fb8040")
    result.AppendMessage("")
    result.AppendMessage("For full documentation, see docs/DEBUGGING.md")


def __lldb_init_module(debugger, internal_dict):
    """Initialize the module and register commands"""
    debugger.HandleCommand('command script add -f umem_lldb.umem_cache_list umem-cache-list')
    debugger.HandleCommand('command script add -f umem_lldb.umem_whatis umem-whatis')
    debugger.HandleCommand('command script add -f umem_lldb.umem_bufinfo umem-bufinfo')
    debugger.HandleCommand('command script add -f umem_lldb.umem_leak_detect umem-leak-detect')
    debugger.HandleCommand('command script add -f umem_lldb.umem_stats umem-stats')
    debugger.HandleCommand('command script add -f umem_lldb.umem_help umem-help')

    print("libumem LLDB extension loaded")
    print("Type 'umem-help' for available commands")
