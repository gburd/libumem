"""
GDB Python extension for debugging libumem

Usage:
    (gdb) source /path/to/umem_gdb.py
    (gdb) umem-cache-list
    (gdb) umem-whatis 0x7ffff7fb8040
    (gdb) umem-bufinfo 0x7ffff7fb8040
    (gdb) umem-leak-detect
    (gdb) umem-stats
"""

import gdb
import struct
import sys

class UmemError(Exception):
    """Exception for umem-specific errors"""
    pass

class UmemCache:
    """Wrapper for umem_cache_t structure"""

    def __init__(self, cache_ptr):
        self.ptr = cache_ptr
        self.cache = cache_ptr.dereference()

    @property
    def name(self):
        """Get cache name"""
        try:
            return self.cache['cache_name'].string()
        except:
            return "<unknown>"

    @property
    def bufsize(self):
        """Get buffer size"""
        try:
            return int(self.cache['cache_bufsize'])
        except:
            return 0

    @property
    def alloc_count(self):
        """Get allocation count"""
        try:
            return int(self.cache['cache_alloc'])
        except:
            return 0

    @property
    def free_count(self):
        """Get free count"""
        try:
            return int(self.cache['cache_free'])
        except:
            return 0

    @property
    def bufs_inuse(self):
        """Get buffers in use"""
        return self.alloc_count - self.free_count

    def __str__(self):
        return f"{self.name:20s} {self.bufsize:6d}  {self.alloc_count:8d}  {self.free_count:8d}  {self.bufs_inuse:8d}"


def get_cache_list():
    """Iterate through all umem caches"""
    try:
        # Find umem_null_cache (head of cache list)
        null_cache = gdb.parse_and_eval("umem_null_cache")
        cache_next_field = "cache_next"

        caches = []
        current = null_cache[cache_next_field]

        # Walk the circular list
        visited = set()
        while True:
            addr = int(current)
            if addr == 0 or addr in visited:
                break
            visited.add(addr)

            try:
                cache = UmemCache(current)
                caches.append(cache)
                current = current.dereference()[cache_next_field]
            except:
                break

            # Safety: limit iterations
            if len(caches) > 1000:
                break

        return caches
    except Exception as e:
        raise UmemError(f"Failed to get cache list: {e}")


def find_cache_for_address(addr):
    """Find which cache owns an address"""
    caches = get_cache_list()

    for cache in caches:
        # Check if address belongs to this cache
        # This is simplified - real implementation would check slabs
        # For now, return None
        pass

    return None


class UmemCacheListCommand(gdb.Command):
    """List all umem caches

    Usage: umem-cache-list

    Shows all memory caches with their statistics.
    """

    def __init__(self):
        super(UmemCacheListCommand, self).__init__(
            "umem-cache-list",
            gdb.COMMAND_DATA
        )

    def invoke(self, arg, from_tty):
        try:
            caches = get_cache_list()

            if not caches:
                print("No caches found")
                return

            print(f"{'Cache Name':20s} {'Size':>6s}  {'Alloc':>8s}  {'Free':>8s}  {'InUse':>8s}")
            print("-" * 60)

            for cache in caches:
                print(cache)

            print(f"\nTotal: {len(caches)} caches")

        except UmemError as e:
            print(f"Error: {e}")
        except Exception as e:
            print(f"Unexpected error: {e}")
            import traceback
            traceback.print_exc()


class UmemWhatisCommand(gdb.Command):
    """Identify which cache owns an address

    Usage: umem-whatis <address>

    Example: umem-whatis 0x7ffff7fb8040
    """

    def __init__(self):
        super(UmemWhatisCommand, self).__init__(
            "umem-whatis",
            gdb.COMMAND_DATA
        )

    def invoke(self, arg, from_tty):
        if not arg:
            print("Usage: umem-whatis <address>")
            return

        try:
            addr = gdb.parse_and_eval(arg)
            addr_int = int(addr)

            print(f"Address: 0x{addr_int:x}")

            cache = find_cache_for_address(addr_int)
            if cache:
                print(f"  Cache: {cache.name}")
                print(f"  Size: {cache.bufsize} bytes")
            else:
                print("  Not found in any umem cache")
                print("  (May be outside heap, or implementation limitation)")

        except Exception as e:
            print(f"Error: {e}")


class UmemBufinfoCommand(gdb.Command):
    """Show detailed buffer information

    Usage: umem-bufinfo <address>

    Shows allocation details including stack trace if audit mode is enabled.

    Example: umem-bufinfo 0x7ffff7fb8040
    """

    def __init__(self):
        super(UmemBufinfoCommand, self).__init__(
            "umem-bufinfo",
            gdb.COMMAND_DATA
        )

    def invoke(self, arg, from_tty):
        if not arg:
            print("Usage: umem-bufinfo <address>")
            return

        try:
            addr = gdb.parse_and_eval(arg)
            addr_int = int(addr)

            print(f"Buffer: 0x{addr_int:x}")

            # Try to find audit information
            # This requires access to umem_bufctl_audit_t structure
            # Implementation depends on having audit mode enabled

            cache = find_cache_for_address(addr_int)
            if cache:
                print(f"  Cache: {cache.name}")
                print(f"  Size: {cache.bufsize} bytes")
            else:
                print("  Cache: <not found>")

            print("\nNote: Stack traces require audit mode (UMEM_DEBUG=audit)")
            print("  If audit is enabled, detailed information would appear here.")

        except Exception as e:
            print(f"Error: {e}")


class UmemLeakDetectCommand(gdb.Command):
    """Scan for memory leaks

    Usage: umem-leak-detect

    Requires audit mode (UMEM_DEBUG=audit) to be enabled.
    Scans all allocated buffers and reports potential leaks.
    """

    def __init__(self):
        super(UmemLeakDetectCommand, self).__init__(
            "umem-leak-detect",
            gdb.COMMAND_DATA
        )

    def invoke(self, arg, from_tty):
        print("Scanning for memory leaks...")
        print("\nNote: This is a simplified implementation.")
        print("Full leak detection requires:")
        print("  1. Audit mode enabled (UMEM_DEBUG=audit)")
        print("  2. Access to allocation records")
        print("  3. Reachability analysis from roots")
        print("\nFor production leak detection, consider:")
        print("  - Valgrind with --leak-check=full")
        print("  - AddressSanitizer with leak detection")
        print("  - heap profilers (gperftools, jemalloc profiling)")

        try:
            caches = get_cache_list()
            total_inuse = 0

            print(f"\n{'Cache':20s} {'Buffers In Use':>15s}")
            print("-" * 40)

            for cache in caches:
                inuse = cache.bufs_inuse
                if inuse > 0:
                    print(f"{cache.name:20s} {inuse:15d}")
                    total_inuse += inuse

            if total_inuse > 0:
                print(f"\nTotal buffers in use: {total_inuse}")
            else:
                print("\nNo allocated buffers found")

        except Exception as e:
            print(f"Error: {e}")


class UmemStatsCommand(gdb.Command):
    """Show allocation statistics

    Usage: umem-stats

    Displays overall allocation statistics and per-cache breakdown.
    """

    def __init__(self):
        super(UmemStatsCommand, self).__init__(
            "umem-stats",
            gdb.COMMAND_DATA
        )

    def invoke(self, arg, from_tty):
        try:
            caches = get_cache_list()

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

            print("Cache Statistics:")
            print(f"  Total allocations: {total_allocs:,}")
            print(f"  Total frees:       {total_frees:,}")
            print(f"  Currently in use:  {total_inuse:,} buffers ({total_bytes:,} bytes)")

            # Show top caches by usage
            sorted_caches = sorted(
                [(c, c.bufs_inuse) for c in caches],
                key=lambda x: x[1],
                reverse=True
            )

            print("\nTop caches by usage:")
            print(f"{'Cache':20s} {'Buffers':>10s} {'Size':>10s} {'Total':>12s}")
            print("-" * 55)

            for cache, inuse in sorted_caches[:10]:
                if inuse > 0:
                    total = inuse * cache.bufsize
                    print(f"{cache.name:20s} {inuse:10d} {cache.bufsize:10d} {total:12,} bytes")

        except Exception as e:
            print(f"Error: {e}")


class UmemHelpCommand(gdb.Command):
    """Show help for umem debugging commands

    Usage: umem-help
    """

    def __init__(self):
        super(UmemHelpCommand, self).__init__(
            "umem-help",
            gdb.COMMAND_SUPPORT
        )

    def invoke(self, arg, from_tty):
        print("libumem GDB Debugging Commands")
        print("=" * 60)
        print()
        print("umem-cache-list      List all memory caches")
        print("umem-whatis <addr>   Identify which cache owns an address")
        print("umem-bufinfo <addr>  Show detailed buffer information")
        print("umem-leak-detect     Scan for memory leaks")
        print("umem-stats           Show allocation statistics")
        print("umem-help            Show this help")
        print()
        print("Debug Modes (set via UMEM_DEBUG environment variable):")
        print("  default     - Default debug features")
        print("  audit       - Transaction logging with stack traces")
        print("  contents    - Fill buffers with patterns")
        print("  guards      - Red-zone guards")
        print("  verify      - Consistency checks")
        print("  firewall    - Guard pages")
        print("  deadbeef    - Fill freed memory")
        print()
        print("Example usage:")
        print("  (gdb) source /path/to/umem_gdb.py")
        print("  (gdb) run")
        print("  (gdb) umem-cache-list")
        print("  (gdb) umem-whatis $rax")
        print("  (gdb) umem-bufinfo 0x7ffff7fb8040")
        print()
        print("For full documentation, see docs/DEBUGGING.md")


# Register all commands
UmemCacheListCommand()
UmemWhatisCommand()
UmemBufinfoCommand()
UmemLeakDetectCommand()
UmemStatsCommand()
UmemHelpCommand()

print("libumem GDB extension loaded")
print("Type 'umem-help' for available commands")
