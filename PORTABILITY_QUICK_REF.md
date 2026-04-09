# Portability Quick Reference Card

**Date:** 2026-04-09 | **Status:** 🔴 CRITICAL ISSUES

---

## Critical Checks Before Committing

### 1. Did you add architecture-specific code?

❌ **DON'T:**
```c
// x86_64-only inline assembly
__asm__ volatile ("lock; cmpxchgq %1, %2" ...);

// Hardcoded pointer width
#define PTR_MASK 0x0000FFFFFFFFFFFFULL  // Assumes 48-bit!
```

✅ **DO:**
```c
// Use C11 atomics (portable)
atomic_compare_exchange_strong_explicit(ptr, &old, new, ...);

// Runtime pointer width check
if ((uintptr_t)ptr & ~PTR_MASK) {
    umem_panic("VA width exceeded");
}
```

---

### 2. Did you add SIMD code?

❌ **DON'T:**
```c
// x86_64-only, no fallback
__m256i v = _mm256_loadu_si256(...);
```

✅ **DO:**
```c
#ifdef HAVE_AVX2
    __m256i v = _mm256_loadu_si256(...);
#elif defined(HAVE_SSE2)
    __m128i v = _mm_loadu_si128(...);
#elif defined(HAVE_NEON)
    uint64x2_t v = vld1q_u64(...);
#elif defined(HAVE_RVV)
    vuint64m1_t v = __riscv_vle64_v_u64m1(...);
#else
    // Generic fallback (required!)
    for (int i = 0; i < count; i++) { ... }
#endif
```

---

### 3. Did you add assembly code?

❌ **DON'T:**
```c
// No architecture guards
void my_func(void) {
    __asm__("movq %rax, %rbx");  // x86_64 only!
}
```

✅ **DO:**
```c
// Proper guards + fallback
#if defined(__x86_64__)
extern void my_func_x86_64(void);
#define my_func my_func_x86_64
#elif defined(__aarch64__)
extern void my_func_aarch64(void);
#define my_func my_func_aarch64
#else
static inline void my_func(void) {
    // C fallback (required!)
}
#endif
```

---

### 4. Did you modify the build system?

❌ **DON'T:**
```makefile
# Always include x86_64 assembly
SOURCES = code.c code_x86_64.S
```

✅ **DO:**
```makefile
# Conditional assembly
if ARCH_X86_64
ARCH_SOURCES = code_x86_64.S
endif
if ARCH_AARCH64
ARCH_SOURCES = code_aarch64.S
endif
SOURCES = code.c $(ARCH_SOURCES)
```

---

## Supported Architectures

| Arch | Status | SIMD | rseq | Pointers | Notes |
|------|--------|------|------|----------|-------|
| x86_64 | ✅ Full | AVX2/SSE2 | ✅ | 48-bit | Production |
| i386 | ✅ Full | Generic | ❌ | 32-bit | Stable |
| aarch64 | ⚠️ 48-bit only | NEON | ✅ | **48-bit only** | See note¹ |
| RISC-V Sv48 | ⚠️ Slow | Generic | ❌ | 48-bit | See note² |
| RISC-V Sv57 | 🔴 BROKEN | - | ❌ | 💥 57-bit | See note³ |

**Notes:**
1. **aarch64:** Breaks on CONFIG_ARM64_VA_BITS_52 - check with `grep CONFIG_ARM64_VA_BITS /boot/config-$(uname -r)`
2. **RISC-V Sv48:** Works but missing optimizations (RVV, rseq assembly)
3. **RISC-V Sv57:** Tagged pointers break - 57-bit VA exceeds 48-bit mask

---

## Common Pitfalls

### Pitfall #1: Assuming 48-bit Pointers

```c
// WRONG: Hardcoded 48-bit assumption
#define PTR_MASK 0x0000FFFFFFFFFFFFULL

// RIGHT: Add validation
static_assert(sizeof(void*) * 8 <= 64, "Pointer must fit in 64 bits");

#ifdef __aarch64__
  #if defined(CONFIG_ARM64_VA_BITS_52)
    #error "52-bit VA not supported"
  #endif
#endif
```

### Pitfall #2: x86-Only Atomics

```c
// WRONG: x86 inline assembly
__asm__ volatile ("lock; cmpxchg %1, %2" ...);

// RIGHT: C11 atomics
#include <stdatomic.h>
atomic_compare_exchange_strong_explicit(...);
```

### Pitfall #3: No SIMD Fallback

```c
// WRONG: x86-only, crashes on other archs
void vectorize(void) {
    __m256i v = _mm256_setzero_si256();
}

// RIGHT: Always provide fallback
#ifdef HAVE_AVX2
void vectorize_avx2(void) { ... }
#define vectorize vectorize_avx2
#else
void vectorize(void) {
    // Generic C implementation
}
#endif
```

### Pitfall #4: Missing Build Guards

```makefile
# WRONG: Always includes x86_64 assembly
libumem_la_SOURCES = umem.c umem_x86_64.S

# RIGHT: Conditional
if ARCH_X86_64
ARCH_ASM = umem_x86_64.S
endif
libumem_la_SOURCES = umem.c $(ARCH_ASM)
```

---

## Pre-Commit Checklist

Before committing code with architecture-specific features:

- [ ] **Grep for x86-specific patterns:**
  ```bash
  grep -r "__x86_64__\|__amd64__\|_M_X64" src/
  grep -r "movq\|pushq\|%rax" src/
  grep -r "_mm_\|__m128\|__m256" src/
  ```

- [ ] **Check for missing fallbacks:**
  ```bash
  # Every SIMD path should have #else with fallback
  grep -A 20 "HAVE_AVX2\|HAVE_SSE2\|HAVE_NEON" src/ | grep -c "#else"
  ```

- [ ] **Verify build on multiple architectures:**
  ```bash
  for arch in x86_64 aarch64 riscv64; do
    ./configure --host=${arch}-linux-gnu
    make clean && make
  done
  ```

- [ ] **Run portability tests:**
  ```bash
  make check
  ./test/integration/test_multithreaded
  ```

- [ ] **Check for hardcoded assumptions:**
  ```bash
  grep -r "48.*bit\|0x0000FFFF" src/
  ```

---

## Quick Fixes

### Fix #1: Pointer Width Check

```c
// Add to umem_init()
#ifndef NDEBUG
void *test = malloc(1);
if ((uintptr_t)test & ~0x0000FFFFFFFFFFFFULL) {
    fprintf(stderr, "FATAL: Pointer exceeds 48 bits: %p\n", test);
    abort();
}
free(test);
#endif
```

### Fix #2: Portable Atomics

```c
// Replace inline assembly with:
#include <stdatomic.h>

uint64_t atomic_cas64(uint64_t *mem, uint64_t new, uint64_t old) {
    _Atomic uint64_t *p = (_Atomic uint64_t*)mem;
    atomic_compare_exchange_strong_explicit(
        p, &old, new,
        memory_order_acq_rel,
        memory_order_acquire
    );
    return old;  // Updated on failure
}
```

### Fix #3: SIMD with Fallback Template

```c
static inline int process_array(void **array, int count)
{
#ifdef HAVE_AVX2
    // x86_64 AVX2: 4 pointers/iteration
    __m256i zero = _mm256_setzero_si256();
    for (int i = 0; i + 4 <= count; i += 4) {
        __m256i v = _mm256_loadu_si256((__m256i*)&array[i]);
        // ... AVX2 logic
    }
    // Handle remainder
    for (; i < count; i++) { /* scalar */ }
#elif defined(HAVE_SSE2)
    // x86_64 SSE2: 2 pointers/iteration
    __m128i zero = _mm_setzero_si128();
    // ... similar to AVX2
#elif defined(HAVE_NEON)
    // aarch64 NEON: 2 pointers/iteration
    uint64x2_t zero = vdupq_n_u64(0);
    // ... similar to SSE2
#elif defined(HAVE_RVV)
    // RISC-V RVV: variable length
    size_t vl;
    for (int i = 0; i < count; i += vl) {
        vl = __riscv_vsetvl_e64m1(count - i);
        // ... RVV logic
    }
#else
    // Generic fallback (REQUIRED!)
    for (int i = 0; i < count; i++) {
        // Scalar C code
    }
#endif
}
```

---

## Testing Commands

```bash
# Check current architecture
uname -m

# Check VA_BITS on aarch64
grep CONFIG_ARM64_VA_BITS /boot/config-$(uname -r)

# Test pointer width at runtime
./umem_test --check-pointers

# Cross-compile test
nix develop .#riscv64
./configure --host=riscv64-unknown-linux-gnu
make

# Run portability tests
make check
./test/integration/test_multithreaded --threads=64
```

---

## When In Doubt

1. **Check existing code:** Look for similar patterns in `umem_simd.h`, `tmem_stubs.c`
2. **Always provide fallback:** Generic C code for unknown architectures
3. **Test on multiple architectures:** Use Nix devshells (`.#riscv64`, `.#aarch64`)
4. **Document limitations:** Update README.md with architecture requirements
5. **Ask for review:** Tag portability-sensitive PRs with `arch:portability` label

---

## Resources

- **Full audit:** PORTABILITY_AUDIT.md
- **Action items:** ACTION_ITEMS_PORTABILITY.md
- **Summary:** PORTABILITY_SUMMARY.md
- **Architecture support:** README.md § Architecture Support

---

## Red Flags 🚩

If you see these patterns, stop and review:

- `__asm__` without architecture guards
- `#ifdef __x86_64__` without `#else` fallback
- Hardcoded bit masks (0xFFFFFFFFFFFF...)
- SIMD intrinsics without generic path
- Assembly files in unconditional Makefile sources
- Comments saying "assumes x86_64" or "48-bit pointers"

**Remember:** Every optimization must work everywhere, or gracefully degrade.
