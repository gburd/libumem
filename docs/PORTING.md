# Architecture Porting Guide

Guide for porting libumem to new architectures.

## Overview

libumem is designed to be portable across different CPU architectures. The core allocator is architecture-independent, but the Per-Thread Cache (PTC) optimization requires architecture-specific assembly generation.

## Architecture Support Status

| Architecture | PTC Support | Status | Notes |
|--------------|-------------|--------|-------|
| x86_64 (AMD64) | ✓ Yes | Complete | Full PTC with genasm |
| i386 (x86) | ✓ Yes | Complete | Full PTC with genasm |
| SPARC | Partial | Needs work | genasm exists but umem_genasm_supported = 0 |
| RISC-V (rv64gc) | ✗ No | Planned | Phase 5 |
| aarch64 (ARM64) | ✗ No | Planned | Phase 5 |
| Windows x64 | ✗ No | Planned | Phase 6 |
| Windows ARM64 | ✗ No | Planned | Phase 6 |

## Porting Checklist

### 1. Create Architecture Directory

```bash
mkdir <arch>/
touch <arch>/umem_genasm.c
```

Example: `riscv64/umem_genasm.c`

### 2. Implement umem_genasm.c

The genasm file generates assembly code for fast-path allocation and deallocation.

**Template Structure:**

```c
#include "../config.h"
#include <stdio.h>
#include <umem_impl.h>

/* Architecture-specific registers and calling convention */
#define UMEM_GENASM_SUPPORTED 1

/* Register definitions for this architecture */
#define REG_SIZE_PARAM    /* Register holding 'size' parameter */
#define REG_FLAGS_PARAM   /* Register holding 'flags' parameter */
#define REG_RETURN        /* Register for return value */
#define REG_TEMP1         /* Temporary register 1 */
#define REG_TEMP2         /* Temporary register 2 */

int umem_genasm_supported = UMEM_GENASM_SUPPORTED;

static void
genasm_malloc(umem_cache_t *cp, int max_size, int cache_flags)
{
    /* Generate assembly for malloc fast path */
    /* 1. Get thread-local tmem pointer */
    /* 2. Check if tmem cache for this size has available buffers */
    /* 3. Pop buffer from tmem cache */
    /* 4. Return buffer or fall back to umem_cache_alloc */
}

static void
genasm_free(umem_cache_t *cp, int max_size)
{
    /* Generate assembly for free fast path */
    /* 1. Get thread-local tmem pointer */
    /* 2. Check if tmem cache has space */
    /* 3. Push buffer to tmem cache */
    /* 4. Return or fall back to umem_cache_free */
}

void
umem_genasm_malloc(umem_cache_t **caches, int ncaches, int cache_flags)
{
    if (!UMEM_GENASM_SUPPORTED) {
        return;
    }

    for (int i = 0; i < ncaches; i++) {
        genasm_malloc(caches[i], umem_genasm_minfragsize, cache_flags);
    }
}

void
umem_genasm_free(umem_cache_t **caches, int ncaches)
{
    if (!UMEM_GENASM_SUPPORTED) {
        return;
    }

    for (int i = 0; i < ncaches; i++) {
        genasm_free(caches[i], umem_genasm_minfragsize);
    }
}
```

### 3. Implement TLS Access (tmem_stubs.c)

Add thread-local storage access for your architecture.

**File:** `tmem_stubs.c`

```c
#if defined(__YOUR_ARCH__)
uintptr_t _tmem_get_base(void) {
    uintptr_t tp;
    __asm__ __volatile__("INSTRUCTION_TO_GET_THREAD_PTR %0" : "=r" (tp));
    return (uintptr_t)&_tmem - tp;
}
#endif
```

**Examples:**

```c
/* x86_64: Thread pointer in %fs */
#if defined(__x86_64__) || defined(__amd64)
uintptr_t _tmem_get_base(void) {
    return (uintptr_t)&_tmem;  /* Compiler handles %fs */
}
#endif

/* RISC-V: Thread pointer in 'tp' register (x4) */
#if defined(__riscv) && (__riscv_xlen == 64)
uintptr_t _tmem_get_base(void) {
    uintptr_t tp;
    __asm__ __volatile__("mv %0, tp" : "=r" (tp));
    return (uintptr_t)&_tmem - tp;
}
#endif

/* aarch64: Thread pointer in TPIDR_EL0 */
#if defined(__aarch64__) || defined(__arm64__)
uintptr_t _tmem_get_base(void) {
    uintptr_t tp;
    __asm__ __volatile__("mrs %0, tpidr_el0" : "=r" (tp));
    return (uintptr_t)&_tmem - tp;
}
#endif
```

### 4. Update configure.ac

Add architecture detection:

```bash
AC_MSG_CHECKING([system architecture])
case "$host_cpu" in
  # ... existing architectures ...

  riscv64)
    AC_MSG_RESULT(riscv64)
    GENASM_DIR="riscv64"
    AC_DEFINE(ARCH_RISCV64, [1], [RISC-V 64-bit architecture])
    ;;

  aarch64|arm64)
    AC_MSG_RESULT(aarch64)
    GENASM_DIR="aarch64"
    AC_DEFINE(ARCH_AARCH64, [1], [ARM64 architecture])
    ;;

  *)
    AC_MSG_RESULT($host_cpu)
    AC_MSG_WARN([Unsupported architecture: $host_cpu, PTC disabled])
    GENASM_DIR="sparc"  /* Falls back to non-PTC */
    ;;
esac

AC_SUBST(GENASM_DIR)
```

### 5. Update atomic.h (if needed)

Ensure atomic operations work on your architecture:

```c
#if defined(__YOUR_ARCH__)
#define casptr(a, b, c) __sync_val_compare_and_swap(a, b, c)
#define cas32(a, b, c) __sync_val_compare_and_swap(a, b, c)
#endif
```

Most modern architectures support GCC/Clang `__sync_*` or `__atomic_*` built-ins.

### 6. Update getpcstack.c

Add stack unwinding support for your architecture if needed for debugging:

```c
#if defined(__YOUR_ARCH__)
int getpcstack(uintptr_t *pcstack, int pcstack_limit) {
    /* Architecture-specific stack unwinding */
    /* Use libunwind, backtrace(), or manual frame pointer walking */
}
#endif
```

## Architecture-Specific Details

### Calling Conventions

Know your architecture's calling convention:

**x86_64 (System V AMD64 ABI)**:
- Arguments: rdi, rsi, rdx, rcx, r8, r9
- Return: rax
- Callee-saved: rbx, rbp, r12-r15
- Red zone: 128 bytes below stack pointer

**i386 (cdecl)**:
- Arguments: stack (right-to-left)
- Return: eax
- Callee-saved: ebx, esi, edi, ebp

**RISC-V (rv64gc)**:
- Arguments: a0-a7 (x10-x17)
- Return: a0-a1
- Callee-saved: s0-s11 (x8-x9, x18-x27)
- Thread pointer: tp (x4)

**aarch64 (ARM64)**:
- Arguments: x0-x7
- Return: x0-x1
- Callee-saved: x19-x28
- Thread pointer: TPIDR_EL0 (varies by OS)

**Windows x64**:
- Arguments: rcx, rdx, r8, r9
- Return: rax
- Callee-saved: rbx, rbp, rdi, rsi, rsp, r12-r15
- Shadow space: 32 bytes

### Thread-Local Storage

Each platform has different TLS mechanisms:

**Linux/BSD**: ELF TLS with `__thread` keyword
**Windows**: PE TLS with `__declspec(thread)`
**macOS**: Mach-O TLS

The `_tmem` variable uses TLS to maintain per-thread caches.

### Memory Ordering

Ensure atomic operations have proper memory ordering:

- **x86/x86_64**: Strong memory model (TSO)
- **ARM/aarch64**: Weak memory model (requires barriers)
- **RISC-V**: Weak memory model (RVWMO)

Use appropriate memory barriers (`__sync_synchronize()` or `__atomic_thread_fence()`).

## Testing New Ports

### 1. Build Tests

```bash
./configure
make
make check
```

### 2. PTC Tests

```bash
./umem_ptc_test        # Basic PTC functionality
./umem_ptc_fork_test   # Fork safety
./umem_ptc_bench       # Performance measurement
```

### 3. Architecture-Specific Tests

```bash
# Verify genasm support
UMEM_OPTIONS=perthread_cache=1m ./umem_ptc_test

# Benchmark with/without PTC
UMEM_OPTIONS=perthread_cache=1m ./umem_ptc_bench
UMEM_OPTIONS=perthread_cache=0 ./umem_ptc_bench
```

### 4. Multi-threaded Stress Test

```bash
./test/integration/test_multithreaded --threads 32 --iterations 10000000
```

### 5. Cross-compilation Testing

Use QEMU for testing on different architectures:

```bash
# RISC-V example
sudo apt install qemu-user-static gcc-riscv64-linux-gnu
riscv64-linux-gnu-gcc -o umem_test umem_test.c -lumem
qemu-riscv64-static ./umem_test
```

## Performance Considerations

### PTC Benefits

With PTC enabled:
- **Small allocations (<448B on x64)**: 5-10x faster
- **No lock contention**: Lock-free fast path
- **Cache-hot allocations**: Better temporal locality

### PTC Overhead

- Memory: ~1MB per thread (configurable)
- Cold start: First allocation populates cache
- Fork: Caches invalidated on fork

### When to Disable PTC

- Memory-constrained environments
- Many short-lived threads
- Debug/development builds

## Common Porting Pitfalls

1. **Incorrect register usage**: Violating calling convention
2. **Missing stack alignment**: Some architectures require 16-byte alignment
3. **Wrong TLS access**: Platform-specific TLS mechanisms
4. **Endianness issues**: Test on big-endian if applicable
5. **Atomic operation bugs**: Weak memory models need barriers
6. **Stack unwinding**: Architecture-specific frame layouts

## Reference Implementations

Study existing ports for guidance:

- **amd64/umem_genasm.c**: Most complete, well-documented
- **i386/umem_genasm.c**: Simpler calling convention
- **sparc/umem_genasm.c**: Template for non-PTC fallback

## Getting Help

- Review existing code in `amd64/` directory
- Check Solaris/Illumos documentation
- Ask on GitHub Issues
- Refer to architecture ABIs and calling conventions

## Contributing

When submitting a new port:

1. Include complete genasm implementation
2. Add architecture tests
3. Document any limitations
4. Provide performance benchmarks
5. Test on real hardware (not just QEMU)
6. Update this document

## See Also

- Architecture ABIs: [https://wiki.osdev.org/ABI](https://wiki.osdev.org/ABI)
- ELF TLS: [https://www.akkadia.org/drepper/tls.pdf](https://www.akkadia.org/drepper/tls.pdf)
- RISC-V Spec: [https://riscv.org/specifications/](https://riscv.org/specifications/)
- ARM64 ABI: [https://github.com/ARM-software/abi-aa](https://github.com/ARM-software/abi-aa)
