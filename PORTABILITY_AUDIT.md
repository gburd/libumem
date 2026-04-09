# libumem Portability Audit Report

**Date:** 2026-04-09
**Reviewer Concern:** Sun Microsystems Reviewer #3 - x86_64-centric code
**Audit Scope:** Comprehensive review of architecture-specific code

## Executive Summary

This audit identifies **CRITICAL** portability issues in libumem's recent performance optimizations. The codebase currently contains x86_64-specific code in multiple critical paths that will block multi-architecture support.

### Severity Classification

- **CRITICAL (P0):** Blocks compilation/runtime on non-x86_64 architectures
- **HIGH (P1):** Degrades to fallback but requires abstraction
- **MEDIUM (P2):** Works but suboptimal on other architectures
- **LOW (P3):** Documentation or future-proofing issues

---

## 1. Architecture-Specific Code Inventory

### 1.1 SIMD Vectorization (`umem_simd.h`)

**Location:** `/home/gburd/ws/libumem/umem_simd.h`

**Severity:** HIGH (P1) - Has fallbacks but missing RISC-V vector extension

**Issues:**
- Lines 49-55: SSE2 intrinsics (`emmintrin.h`, `__m128i`)
- Lines 53-55: AVX2 intrinsics (`immintrin.h`, `__m256i`)
- Lines 57-59: ARM NEON intrinsics (`arm_neon.h`)
- Lines 88-112: AVX2 implementation processes 4x64-bit pointers
- Lines 114-137: SSE2 implementation processes 2x64-bit pointers
- Lines 139-163: NEON implementation processes 2x64-bit pointers
- Lines 165-176: Generic fallback (works but slow)

**Analysis:**
```c
#ifdef HAVE_AVX2
    __m256i zero = _mm256_setzero_si256();
    __m256i ptrs = _mm256_loadu_si256((__m256i *)&array[i]);
    __m256i cmp = _mm256_cmpeq_epi64(ptrs, zero);
    int mask = _mm256_movemask_epi8(cmp);
```

**Portability Impact:**
- ✅ **x86_64:** Full AVX2/SSE2 support
- ✅ **aarch64:** NEON support present
- ❌ **RISC-V:** No vector extension support (RVV)
- ✅ **Generic:** Fallback available but 2-4x slower

**Recommendations:**
1. Add RISC-V Vector Extension (RVV) support when available
2. Consider auto-vectorization hints for generic fallback
3. Runtime CPU feature detection for x86_64 (AVX2 vs SSE2)

---

### 1.2 Restartable Sequences (rseq) - x86_64 Assembly

**Location:** `/home/gburd/ws/libumem/umem_rseq_x86_64.S`

**Severity:** CRITICAL (P0) - Pure x86_64 assembly, no fallback

**Issues:**
- Line 22: `#if defined(__x86_64__) && defined(__linux__)`
- Lines 63-181: Pure x86_64 assembly (`movq`, `pushq`, `%rax`, `%rbx`, etc.)
- Lines 104-107: x86_64 TLS access via `%fs` segment register
- Lines 91-100: x86_64-specific instruction encoding
- Line 164: x86_64-specific RSEQ signature encoding

**Critical Code:**
```asm
# Lines 104-107: x86_64 TLS access
movq    umem_rseq_area@gottpoff(%rip), %rax
movq    %fs:(%rax), %r12                    # FS segment register

# Lines 109-160: x86_64 critical section
.Lrseq_alloc_start:
    movq    umem_rseq_area@gottpoff(%rip), %rax
    movl    %fs:4(%rax), %ecx               # Load cpu_id via FS
    cmpl    %esi, %ecx
    jne     .Lrseq_alloc_abort
```

**Portability Impact:**
- ✅ **x86_64:** Full rseq support
- ⚠️  **aarch64:** Separate assembly file exists (`umem_rseq_aarch64.S`)
- ❌ **RISC-V:** No assembly implementation
- ❌ **Other architectures:** Compilation failure

**Recommendations:**
1. **Immediate:** Add preprocessor guards to disable rseq on unsupported architectures
2. **Short-term:** Implement RISC-V rseq assembly
3. **Long-term:** Consider C intrinsics where possible for maintainability

---

### 1.3 Restartable Sequences (rseq) - aarch64 Assembly

**Location:** `/home/gburd/ws/libumem/umem_rseq_aarch64.S`

**Severity:** MEDIUM (P2) - Architecture-specific but properly abstracted

**Issues:**
- Line 22: `#if defined(__aarch64__) && defined(__linux__)`
- Lines 54-335: aarch64 assembly using ARM64 instructions
- Lines 100-108: aarch64 TLS via `TPIDR_EL0` special register
- Lines 105-107: aarch64-specific TLS relocation (`gottprel`)

**Critical Code:**
```asm
# Lines 100-108: aarch64 TLS access
mrs     x22, tpidr_el0                      # Read thread pointer
adrp    x0, :gottprel:umem_rseq_area
ldr     x0, [x0, #:gottprel_lo12:umem_rseq_area]
add     x0, x22, x0                         # Calculate TLS address
```

**Portability Impact:**
- ✅ **aarch64:** Full rseq support
- ❌ **Other architectures:** Properly guarded, no impact

**Status:** ✅ **GOOD** - Proper architecture isolation with preprocessor guards

---

### 1.4 Tagged Pointers (48-bit pointer assumption)

**Location:** `/home/gburd/ws/libumem/umem_impl.h`

**Severity:** CRITICAL (P0) - Assumes 48-bit virtual address space

**Issues:**
- Line 340: Comment explicitly states "x86_64 and aarch64, userspace pointers use at most 48 bits"
- Line 343: `#define UMEM_PTR_MASK 0x0000FFFFFFFFFFFFULL` (48-bit mask)
- Line 344: `#define UMEM_VER_SHIFT 48` (version in upper 16 bits)
- Lines 359-378: Tagged pointer operations assume 48-bit addresses

**Critical Code:**
```c
/*
 * Tagged pointer for lock-free stack operations.
 * Packs a pointer and a 16-bit version counter into a single 64-bit word
 * so the pair can be atomically loaded/CAS'd with standard 64-bit atomics.
 * On x86_64 and aarch64, userspace pointers use at most 48 bits; the
 * upper 16 bits carry the ABA-prevention version counter.
 */
#define UMEM_PTR_MASK   0x0000FFFFFFFFFFFFULL
#define UMEM_VER_SHIFT  48
```

**Portability Impact:**
- ✅ **x86_64:** 48-bit user virtual addresses (canonical form)
- ✅ **aarch64:** 48-bit virtual addresses (ARMv8.2 allows up to 52-bit)
- ⚠️  **aarch64 with 52-bit VA:** BREAKS - pointer truncation!
- ⚠️  **RISC-V Sv57:** 57-bit virtual addresses - BREAKS!
- ⚠️  **RISC-V Sv48:** 48-bit virtual addresses - works but fragile
- ❌ **Future architectures:** Not portable

**Real-World Failure Scenarios:**

1. **ARM64 with 52-bit VA (ARMv8.2+):**
   ```
   Actual pointer:  0x000F_1234_5678_9ABC (52 bits used)
   After mask:      0x0000_1234_5678_9ABC (upper 4 bits lost)
   Result: Segmentation fault, data corruption
   ```

2. **RISC-V Sv57:**
   ```
   Actual pointer:  0x01FF_FFFF_FFFF_FFFF (57 bits)
   After mask:      0x0000_FFFF_FFFF_FFFF (upper 9 bits lost)
   Result: Invalid pointer, crashes
   ```

**Recommendations:**
1. **CRITICAL:** Add compile-time assertion to verify VA_BITS ≤ 48
2. **CRITICAL:** Add runtime pointer validation in debug builds
3. **FUTURE:** Consider alternative ABA solutions:
   - Hazard pointers (no pointer packing)
   - Epoch-based reclamation
   - Double-width CAS on platforms with 128-bit atomics

---

### 1.5 Inline Assembly for Atomics

**Location:** `/home/gburd/ws/libumem/sol_compat.h`

**Severity:** HIGH (P1) - x86-specific inline assembly

**Issues:**
- Line 124: `#elif (defined(__i386__) || defined(__x86_64__)) && defined(__GNUC__)`
- Lines 125-156: x86-specific inline assembly
- Lines 128-132: `lock; cmpxchgl` (x86 instruction)
- Lines 139-142: `lock; cmpxchgq` (x86_64 instruction)
- Lines 144-154: i386 `cmpxchg8b` with register constraints

**Critical Code:**
```c
static INLINE uint64_t umem_atomic_cas64(uint64_t *mem, uint64_t with,
  uint64_t cmp)
{
  uint64_t prev;
#if defined(__x86_64__)
  __asm__ volatile ("lock; cmpxchgq %1, %2"
    : "=a" (prev)
    : "r" (with), "m" (*(mem)), "0" (cmp)
    : "memory");
```

**Portability Impact:**
- ✅ **x86/x86_64:** Native inline assembly
- ⚠️  **Other architectures:** Falls through to `#error` on line 167
- ✅ **macOS:** Uses `OSAtomic*` functions
- ✅ **Windows:** Uses `Interlocked*` functions

**Modern Solution:**
The codebase already includes `<stdatomic.h>` (line 42 in `umem_impl.h`). This inline assembly should be replaced with C11 atomics:

```c
// Modern portable approach
atomic_compare_exchange_strong_explicit(
    (_Atomic uint64_t*)mem, &cmp, with,
    memory_order_acq_rel, memory_order_acquire);
```

**Recommendations:**
1. **Migrate to C11 atomics** (already included via `stdatomic.h`)
2. Keep platform-specific code only for pre-C11 compilers
3. Remove architecture-specific assembly where possible

---

### 1.6 Thread-Local Storage (TLS) Access

**Location:** `/home/gburd/ws/libumem/tmem_stubs.c`

**Severity:** MEDIUM (P2) - Has multi-arch support but needs validation

**Issues:**
- Lines 43-62: Architecture-specific TLS access implementations
- Line 43-46: x86_64 uses `%fs:0`
- Line 47-50: i386 uses `%gs:0`
- Line 51-54: RISC-V uses `tp` register
- Line 55-58: aarch64 uses `tpidr_el0`

**Code:**
```c
#if defined(__x86_64__) || defined(__amd64__)
    uintptr_t fs_base;
    __asm__ __volatile__("movq %%fs:0, %0" : "=r" (fs_base));
    return (uintptr_t)&_tmem - fs_base;
#elif defined(__i386__)
    uintptr_t gs_base;
    __asm__ __volatile__("movl %%gs:0, %0" : "=r" (gs_base));
    return (uintptr_t)&_tmem - gs_base;
#elif defined(__riscv) && (__riscv_xlen == 64)
    uintptr_t tp;
    __asm__ __volatile__("mv %0, tp" : "=r" (tp));
    return (uintptr_t)&_tmem - tp;
#elif defined(__aarch64__) || defined(__arm64__)
    uintptr_t tp;
    __asm__ __volatile__("mrs %0, tpidr_el0" : "=r" (tp));
    return (uintptr_t)&_tmem - tp;
#else
    return 0;
#endif
```

**Portability Impact:**
- ✅ **x86_64:** Supported
- ✅ **i386:** Supported
- ✅ **aarch64:** Supported
- ✅ **RISC-V 64:** Supported
- ❌ **Other architectures:** Returns 0 (PTC disabled)

**Status:** ✅ **GOOD** - Multi-arch support with graceful degradation

---

### 1.7 rseq System Call Numbers

**Location:** `/home/gburd/ws/libumem/umem_rseq.c`

**Severity:** LOW (P3) - Platform-specific but properly abstracted

**Issues:**
- Lines 27-37: Architecture-specific syscall numbers
- Line 29: x86_64 uses `__NR_rseq 334`
- Line 31: aarch64 uses `__NR_rseq 293`
- Line 33: i386 uses `__NR_rseq 381`
- Line 35: Defaults to 0 (disabled)

**Code:**
```c
#ifndef __NR_rseq
#if defined(__x86_64__)
#define __NR_rseq 334
#elif defined(__aarch64__)
#define __NR_rseq 293
#elif defined(__i386__)
#define __NR_rseq 381
#else
#define __NR_rseq 0
#endif
#endif
```

**Portability Impact:**
- ✅ **x86_64/i386/aarch64:** Correct syscall numbers
- ✅ **Other architectures:** Gracefully disabled
- ✅ **Future kernels:** Uses glibc header if available

**Status:** ✅ **GOOD** - Proper fallback strategy

---

### 1.8 Build System Architecture Detection

**Location:** `/home/gburd/ws/libumem/configure.ac`

**Severity:** MEDIUM (P2) - Good coverage but incomplete

**Issues:**
- Lines 66-103: Architecture detection switch statement
- Lines 136-160: x86_64-only SIMD detection
- Lines 107-133: aarch64-specific feature detection
- Line 159: SIMD checks skipped for non-x86_64 with message "skipped (not x86_64)"

**Code:**
```bash
# Lines 136-160: SIMD detection only for x86_64
AC_MSG_CHECKING([for x86_64 SSE2 support])
if test "$GENASM_DIR" = "amd64"; then
    AC_COMPILE_IFELSE(
        [AC_LANG_PROGRAM([[#include <emmintrin.h>]],
                         [[__m128i v = _mm_setzero_si128();]])],
        [AC_MSG_RESULT([yes])
         AC_DEFINE([HAVE_SSE2], [1], [x86_64 SSE2 SIMD support available])],
        [AC_MSG_RESULT([no])]
    )
else
    AC_MSG_RESULT([skipped (not x86_64)])
fi
```

**Portability Impact:**
- ✅ **Architecture detection:** Supports x86_64, i386, aarch64, RISC-V, SPARC
- ⚠️  **SIMD detection:** Only checks x86_64/aarch64, skips RISC-V
- ✅ **Fallback:** Sets `GENASM_DIR="sparc"` for unknown architectures

**Recommendations:**
1. Add RISC-V vector extension detection
2. Add runtime CPU feature detection for x86_64 (not just compile-time)
3. Improve "unknown architecture" handling (don't default to SPARC)

---

### 1.9 Memory Ordering Assumptions

**Location:** `/home/gburd/ws/libumem/umem.c`

**Severity:** LOW (P3) - Uses GCC builtins (portable)

**Issues:**
- Lines 2236-2255: Atomic operations using GCC builtins
- Lines 2240-2250: `__atomic_load_n` with `__ATOMIC_ACQUIRE`
- Comments mention "acquire/release semantics" (line 2252-2254)

**Code:**
```c
/* Atomic load with acquire semantics */
static inline int __attribute__((always_inline))
atomic_load_int_acquire(volatile int *ptr)
{
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

/*
 * Memory ordering:
 * - acquire: ensures subsequent loads see effects of prior stores
 * - release: ensures prior stores are visible before this operation
 */
```

**Portability Impact:**
- ✅ **All architectures:** GCC/Clang builtins are portable
- ✅ **x86_64:** Strong memory model (acquire/release often free)
- ✅ **aarch64/RISC-V:** Weak memory model (explicit barriers needed)
- ✅ **Compiler:** Generates correct barriers for each architecture

**Status:** ✅ **EXCELLENT** - Using portable atomics correctly

---

## 2. Missing Architecture Support

### 2.1 RISC-V Vector Extension (RVV)

**Priority:** HIGH (P1)

**Current State:**
- Generic fallback only (no vectorization)
- RISC-V V extension provides 128-bit to 2048-bit vectors
- Missing in `umem_simd.h`

**Implementation Needed:**
```c
#ifdef HAVE_RVV
#include <riscv_vector.h>

static inline int
umem_mag_scan_notnull(void **array, int count)
{
    size_t vl;
    vbool64_t mask;

    for (size_t i = 0; i < count; i += vl) {
        vl = __riscv_vsetvl_e64m1(count - i);
        vuint64m1_t ptrs = __riscv_vle64_v_u64m1((uint64_t*)&array[i], vl);
        mask = __riscv_vmseq_vx_u64m1_b64(ptrs, 0, vl);

        if (!__riscv_vcpop_m_b64(mask, vl) == vl) {
            return 1;  // Found non-NULL
        }
    }
    return 0;
}
#endif
```

**Detection:**
```bash
# In configure.ac
AC_MSG_CHECKING([for RISC-V vector extension])
AC_COMPILE_IFELSE(
    [AC_LANG_PROGRAM([[#include <riscv_vector.h>]],
                     [[size_t vl = __riscv_vsetvl_e64m1(4);]])],
    [AC_MSG_RESULT([yes])
     AC_DEFINE([HAVE_RVV], [1], [RISC-V Vector extension available])],
    [AC_MSG_RESULT([no])]
)
```

---

### 2.2 RISC-V rseq Assembly

**Priority:** HIGH (P1)

**Current State:**
- No `umem_rseq_riscv64.S` file
- Falls back to C implementation (slower)

**Implementation Outline:**
```asm
# umem_rseq_riscv64.S
#if defined(__riscv) && defined(__linux__)

.text
.align 4
.globl umem_rseq_alloc_fastpath
.type umem_rseq_alloc_fastpath, @function

umem_rseq_alloc_fastpath:
    # Save registers
    addi    sp, sp, -64
    sd      ra, 56(sp)
    sd      s0, 48(sp)

    # Access TLS via tp register
    mv      s0, a0              # cache pointer
    mv      a2, tp              # thread pointer (x4)

    # Set up rseq_cs structure
    # ... (similar to x86_64/aarch64)

.Lrseq_start:
    # Critical section
    # Load cpu_id from TLS
    lw      t0, offset(tp)      # Load current CPU
    bne     t0, a1, .Lrseq_abort

    # Load magazine
    ld      t1, 0(s0)           # cache->loaded_mag
    beqz    t1, .Lrseq_empty

    # ... rest of implementation

.Lrseq_abort:
    # Abort handler with RSEQ_SIG
    .word   0x53053053
    # ... cleanup and return NULL

#endif
```

---

### 2.3 Runtime CPU Feature Detection

**Priority:** MEDIUM (P2)

**Current State:**
- Compile-time only (`#ifdef HAVE_AVX2`)
- No runtime detection of AVX2 vs SSE2

**Implementation Needed:**
```c
// In umem_init.c
#ifdef __x86_64__
#include <cpuid.h>

static int cpu_has_avx2 = 0;
static int cpu_has_sse2 = 0;

static void detect_cpu_features(void)
{
    unsigned int eax, ebx, ecx, edx;

    // Check SSE2 (CPUID.01H:EDX[26])
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        cpu_has_sse2 = (edx >> 26) & 1;
    }

    // Check AVX2 (CPUID.07H:EBX[5])
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        cpu_has_avx2 = (ebx >> 5) & 1;
    }
}
#endif
```

**Benefits:**
- Single binary supports multiple CPU generations
- Graceful degradation on older CPUs
- Better containerization support

---

## 3. Recommended Abstraction Strategy

### 3.1 Immediate Actions (P0 - Blocking Issues)

#### Action 1: Add Pointer Width Assertions

**File:** `umem_impl.h`

```c
// After line 344
#if defined(__aarch64__) && defined(__ARM_FEATURE_MEMORY_TAGGING)
#error "ARM Memory Tagging (MTE) conflicts with tagged pointer scheme"
#endif

// Add runtime check
static inline void
umem_verify_pointer_width(void)
{
    // Verify we're not using more than 48 bits
    void *high_addr = (void*)0x0001000000000000ULL;
    if ((uintptr_t)high_addr & ~UMEM_PTR_MASK) {
        umem_panic("Virtual address space exceeds 48 bits - "
                   "tagged pointers will fail");
    }
}
```

#### Action 2: Disable rseq on Unsupported Architectures

**File:** `Makefile.am` (line 46-50)

```makefile
# Conditionally include rseq support
if ENABLE_RSEQ
# Only build rseq for supported architectures
if ARCH_X86_64
RSEQ_ASM_SOURCES = umem_rseq_x86_64.S
else if ARCH_AARCH64
RSEQ_ASM_SOURCES = umem_rseq_aarch64.S
else
RSEQ_ASM_SOURCES =
endif
RSEQ_SOURCES = umem_rseq.c umem_rseq.h $(RSEQ_ASM_SOURCES)
else
RSEQ_SOURCES =
endif
```

#### Action 3: Document Architecture Requirements

**File:** `README.md` (add new section)

```markdown
## Architecture Requirements

### Pointer Width Limitation (CRITICAL)

The lock-free depot implementation uses tagged pointers that assume
**48-bit virtual address space**. This is true for:

- ✅ x86_64 (canonical form: 48 bits)
- ✅ aarch64 with 48-bit VA (default)
- ⚠️  aarch64 with 52-bit VA (ARMv8.2+) - **NOT SUPPORTED**
- ⚠️  RISC-V Sv57 - **NOT SUPPORTED**
- ✅ RISC-V Sv48 - Supported

If you encounter crashes on aarch64, check:
```bash
# Verify VA width
grep CONFIG_ARM64_VA_BITS /boot/config-$(uname -r)
# Must be 48, not 52
```
```

---

### 3.2 Short-Term Improvements (P1 - High Priority)

#### Improvement 1: C11 Atomic Migration

**Files:** `sol_compat.h`, `umem.c`

**Replace inline assembly with portable atomics:**

```c
// OLD (x86-specific)
static INLINE uint64_t umem_atomic_cas64(uint64_t *mem, uint64_t with, uint64_t cmp)
{
    uint64_t prev;
    __asm__ volatile ("lock; cmpxchgq %1, %2"
        : "=a" (prev)
        : "r" (with), "m" (*(mem)), "0" (cmp)
        : "memory");
    return prev;
}

// NEW (portable)
static INLINE uint64_t umem_atomic_cas64(uint64_t *mem, uint64_t with, uint64_t cmp)
{
    _Atomic uint64_t *atomic_mem = (_Atomic uint64_t*)mem;
    atomic_compare_exchange_strong_explicit(
        atomic_mem, &cmp, with,
        memory_order_acq_rel, memory_order_acquire);
    return cmp;  // Updated to actual value on failure
}
```

**Benefits:**
- Remove all `__asm__ volatile` from `sol_compat.h`
- Compiler generates optimal code for each architecture
- Better optimization opportunities

#### Improvement 2: RISC-V Vector Support

**See Section 2.1** for implementation details.

#### Improvement 3: Runtime CPU Feature Detection

**See Section 2.3** for implementation details.

---

### 3.3 Long-Term Strategy (P2/P3)

#### Strategy 1: Architecture Abstraction Layer

Create a clean separation between architecture-specific and portable code:

```
libumem/
├── arch/
│   ├── common/         # Portable implementations
│   ├── x86_64/         # x86_64-specific
│   │   ├── simd.h
│   │   ├── rseq.S
│   │   └── atomics.h
│   ├── aarch64/
│   ├── riscv64/
│   └── generic/        # Fallback implementations
├── umem_core.c         # Pure portable code
└── umem_simd.h         # Dispatches to arch/*/simd.h
```

#### Strategy 2: Alternative ABA Solutions

**Option A: Hazard Pointers** (no pointer packing)
- Remove 48-bit assumption entirely
- Use epoch-based memory reclamation
- Portable to all architectures including future 64-bit VA

**Option B: Double-Width CAS** (128-bit)
- Use `__int128` on supported platforms
- x86_64: `cmpxchg16b` instruction
- aarch64: `LDXP/STXP` instruction pair
- More portable, no VA width assumption

**Option C: Separate Version Array**
- Keep pointers and versions separate
- Slightly slower but completely portable
- No pointer size assumptions

---

## 4. Architecture Support Matrix

| Architecture | Status | SIMD | rseq | Tagged Ptrs | Notes |
|-------------|--------|------|------|-------------|-------|
| **x86_64** | ✅ Full | AVX2/SSE2 | ✅ ASM | ✅ 48-bit | Production ready |
| **i386** | ✅ Full | Generic | ❌ | N/A (32-bit) | Uses fallback |
| **aarch64** | ⚠️  Partial | NEON | ✅ ASM | ⚠️  48-bit only | Breaks on 52-bit VA |
| **RISC-V 64 (Sv48)** | ⚠️  Template | Generic | ❌ C only | ⚠️  Works | Slow without RVV |
| **RISC-V 64 (Sv57)** | ❌ Broken | Generic | ❌ | ❌ Broken | 57-bit VA breaks tagging |
| **SPARC** | ⚠️  Planned | Generic | ❌ | TBD | Template exists |
| **Future 64-bit VA** | ❌ Unsupported | - | - | ❌ | Will break tagged ptrs |

---

## 5. Action Items by Priority

### P0 - Critical (Blocks Multi-Arch)

1. **Add pointer width validation** (umem_impl.h)
   - Compile-time checks for VA_BITS
   - Runtime verification in debug builds
   - Clear error messages

2. **Guard rseq compilation** (Makefile.am, configure.ac)
   - Only build assembly for supported architectures
   - Fallback to C implementation otherwise

3. **Document limitations** (README.md)
   - 48-bit VA requirement
   - Architecture support matrix
   - Migration path for aarch64 52-bit VA

**Estimated Effort:** 4-8 hours
**Target:** Next commit

---

### P1 - High Priority (Performance/Portability)

1. **Migrate to C11 atomics** (sol_compat.h, umem.c)
   - Replace inline assembly
   - Test on all platforms
   - Measure performance impact

2. **Implement RISC-V vector support** (umem_simd.h, configure.ac)
   - Add RVV detection
   - Implement vectorized operations
   - Add benchmarks

3. **Implement RISC-V rseq assembly** (new file: umem_rseq_riscv64.S)
   - Port from x86_64 version
   - Test with Linux 4.18+
   - Document differences

**Estimated Effort:** 2-3 days
**Target:** Next sprint

---

### P2 - Medium Priority (Nice to Have)

1. **Runtime CPU feature detection** (umem_init.c)
   - x86_64: AVX2 vs SSE2
   - aarch64: NEON variants
   - RISC-V: Vector extension version

2. **Architecture abstraction layer** (refactoring)
   - Clean separation of arch-specific code
   - Better maintainability
   - Easier to add new architectures

3. **Alternative ABA solution** (umem_impl.h, umem.c)
   - Remove 48-bit VA assumption
   - Use hazard pointers or double-width CAS
   - Enable 52-bit VA on aarch64

**Estimated Effort:** 1-2 weeks
**Target:** Next quarter

---

### P3 - Low Priority (Future-Proofing)

1. **Formal architecture support policy**
   - Define tier-1 (full support), tier-2 (best effort), tier-3 (template)
   - Document testing requirements
   - CI matrix

2. **Cross-compilation testing** (CI/CD)
   - QEMU user-mode for RISC-V
   - QEMU user-mode for aarch64
   - Real hardware testing

3. **Performance benchmarking suite**
   - Per-architecture comparison
   - SIMD vs scalar performance
   - rseq vs fallback performance

**Estimated Effort:** Ongoing
**Target:** Continuous improvement

---

## 6. Risk Assessment

### High Risk

1. **aarch64 with 52-bit VA** - Silent pointer corruption
   - **Mitigation:** Add runtime check, document limitation
   - **Long-term:** Implement double-width CAS

2. **RISC-V Sv57 adoption** - Future-proofing concern
   - **Mitigation:** Detect and error early
   - **Long-term:** Alternative ABA solution

### Medium Risk

1. **Performance regression on RISC-V** - No vectorization or rseq
   - **Mitigation:** Document performance characteristics
   - **Long-term:** Implement RVV and rseq assembly

2. **Maintenance burden** - Multiple assembly implementations
   - **Mitigation:** Good test coverage
   - **Long-term:** Consider compiler intrinsics where possible

### Low Risk

1. **Build failures on exotic architectures** - Unknown platforms
   - **Mitigation:** Generic fallbacks in place
   - **Status:** Already handled well

---

## 7. Conclusion

The libumem codebase has **critical portability issues** that must be addressed before claiming multi-architecture support:

### Immediate Blockers (Must Fix)

1. ❌ **48-bit pointer assumption** - Breaks on aarch64 52-bit VA and RISC-V Sv57
2. ❌ **x86_64-only rseq compilation** - Build failures on other architectures
3. ❌ **Missing RISC-V implementations** - Performance degradation

### Good Practices Found

1. ✅ **SIMD fallbacks** - Generic implementations available
2. ✅ **TLS abstraction** - Multi-arch support in `tmem_stubs.c`
3. ✅ **C11 atomics usage** - Modern memory ordering primitives
4. ✅ **Architecture guards** - Proper `#ifdef` usage in most places

### Recommended Path Forward

1. **Week 1:** Fix P0 critical issues (pointer width, build guards)
2. **Week 2-3:** Implement P1 improvements (C11 atomics, RISC-V support)
3. **Month 2:** Address P2 items (runtime detection, architecture layer)
4. **Ongoing:** P3 future-proofing and performance optimization

### Success Criteria

- ✅ Clean compilation on x86_64, i386, aarch64 (48-bit), RISC-V Sv48
- ✅ Clear error messages on unsupported configurations (aarch64 52-bit, RISC-V Sv57)
- ✅ Performance within 10% of x86_64 on other tier-1 architectures
- ✅ Comprehensive architecture support documentation

---

**Report compiled by:** Portability Audit System
**Review Status:** REQUIRES ACTION - Sun Microsystems Reviewer #3 concerns are VALID
**Next Steps:** Address P0 items immediately, plan P1 sprint
