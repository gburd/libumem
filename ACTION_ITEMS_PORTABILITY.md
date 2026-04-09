# Portability Action Items

**Generated:** 2026-04-09
**Priority:** CRITICAL - Sun Microsystems Reviewer #3 Concerns

This document provides specific, actionable tasks to address the portability issues identified in PORTABILITY_AUDIT.md.

---

## Critical Path (P0) - Must Fix Before Next Release

### Issue #1: Add Tagged Pointer Validation

**Priority:** P0 - CRITICAL
**Estimated Time:** 2 hours
**Files:** `umem_impl.h`, `umem.c`

**Problem:**
Tagged pointers assume 48-bit virtual addresses. Breaks silently on:
- aarch64 with CONFIG_ARM64_VA_BITS=52
- RISC-V Sv57 (57-bit VA)

**Solution:**

1. Add compile-time checks in `umem_impl.h`:

```c
// After line 344 (after UMEM_VER_SHIFT definition)

/*
 * Compile-time verification of pointer width assumptions
 */
#if defined(__aarch64__)
  #if defined(__ARM_FEATURE_MEMORY_TAGGING)
    #error "ARM MTE (Memory Tagging Extension) conflicts with tagged pointers"
  #endif
  // ARMv8.2+ can have 52-bit VA - we only support 48-bit
  #ifdef CONFIG_ARM64_VA_BITS_52
    #error "aarch64 with 52-bit VA not supported - tagged pointers require 48-bit VA"
  #endif
#endif

#if defined(__riscv)
  // RISC-V Sv48 is OK (48-bit), Sv57/Sv64 are not
  #if defined(__riscv_xlen) && (__riscv_xlen > 48)
    #warning "RISC-V virtual address width >48 bits may cause issues"
  #endif
#endif
```

2. Add runtime check in `umem_init()` in `umem.c`:

```c
// In umem_init(), add after line 3500

#ifndef NDEBUG
static void
umem_verify_pointer_constraints(void)
{
	/*
	 * Verify that we can safely use tagged pointers.
	 * On debug builds, check that high bits are unused.
	 */
	void *test_ptr = malloc(64);
	uintptr_t addr = (uintptr_t)test_ptr;

	if (addr & ~UMEM_PTR_MASK) {
		umem_printf(
		    "FATAL: Pointer 0x%016lx uses bits above 48\n"
		    "Tagged pointers require 48-bit virtual address space\n"
		    "Architecture: %s\n"
		    "VA_BITS configuration may be incompatible\n",
		    addr,
#if defined(__x86_64__)
		    "x86_64"
#elif defined(__aarch64__)
		    "aarch64 (check CONFIG_ARM64_VA_BITS)"
#elif defined(__riscv)
		    "RISC-V (check VA mode: Sv48 vs Sv57)"
#else
		    "unknown"
#endif
		);
		free(test_ptr);
		umem_panic("Tagged pointer virtual address assumption violated");
	}
	free(test_ptr);
}

// Call this in umem_init()
umem_verify_pointer_constraints();
#endif
```

**Testing:**
```bash
# Test on aarch64 with 52-bit VA
uname -a | grep aarch64
grep CONFIG_ARM64_VA_BITS /boot/config-$(uname -r)
./umem_test  # Should error with clear message

# Test on RISC-V Sv57
# (requires QEMU or real hardware with Sv57)
```

**Acceptance Criteria:**
- ✅ Compile-time error on aarch64 52-bit VA
- ✅ Runtime error (debug builds) when high bits are used
- ✅ Clear error message pointing to VA configuration
- ✅ Documentation in README.md

---

### Issue #2: Fix rseq Build on Unsupported Architectures

**Priority:** P0 - CRITICAL
**Estimated Time:** 1 hour
**Files:** `Makefile.am`, `configure.ac`, `umem_rseq.c`

**Problem:**
`umem_rseq_x86_64.S` always included, breaks build on RISC-V/SPARC.

**Solution:**

1. Update `Makefile.am` (around line 45-50):

```makefile
# Conditionally include rseq support - architecture-specific
if ENABLE_RSEQ

# Determine which architecture-specific assembly to include
RSEQ_COMMON_SOURCES = umem_rseq.c umem_rseq.h

if ARCH_X86_64
RSEQ_ASM_SOURCES = umem_rseq_x86_64.S
endif

if ARCH_AARCH64
RSEQ_ASM_SOURCES = umem_rseq_aarch64.S
endif

# RISC-V: No assembly yet, C fallback only
if ARCH_RISCV64
RSEQ_ASM_SOURCES =
endif

RSEQ_SOURCES = $(RSEQ_COMMON_SOURCES) $(RSEQ_ASM_SOURCES)
else
RSEQ_SOURCES =
endif
```

2. Add architecture conditionals to `configure.ac` (after line 305):

```bash
# After ENABLE_RSEQ conditional

# Check if rseq assembly is available for this architecture
if test "x$rseq" = "xyes"; then
    case "$host_cpu" in
        x86_64|amd64)
            AC_MSG_NOTICE([rseq: using x86_64 assembly implementation])
            AM_CONDITIONAL([ARCH_X86_64], true)
            ;;
        aarch64|arm64)
            AC_MSG_NOTICE([rseq: using aarch64 assembly implementation])
            AM_CONDITIONAL([ARCH_AARCH64], true)
            ;;
        riscv64)
            AC_MSG_WARN([rseq: no RISC-V assembly, using C fallback (slower)])
            AM_CONDITIONAL([ARCH_RISCV64], true)
            ;;
        *)
            AC_MSG_WARN([rseq: no assembly for $host_cpu, using C fallback])
            ;;
    esac
else
    AM_CONDITIONAL([ARCH_X86_64], false)
    AM_CONDITIONAL([ARCH_AARCH64], false)
    AM_CONDITIONAL([ARCH_RISCV64], false)
fi
```

3. Update `umem_rseq.c` to handle missing assembly (around line 256):

```c
// Replace line 256-267 with architecture checking:

#if defined(__x86_64__) || defined(__aarch64__)
	/*
	 * Fast path: use architecture-specific assembly implementation
	 * with true rseq critical section. The kernel will automatically
	 * restart if CPU migration occurs during the critical section.
	 */
	obj = umem_rseq_alloc_fastpath(rseq_cache, cpu_id);
	if (obj != NULL) {
		return obj;
	}

	/* Magazine empty or rseq restarted - go to slow path */
	return umem_cache_alloc_rseq_slowpath(cp, cpu_id, umflag);
#else
	/*
	 * C fallback for architectures without assembly implementation.
	 * This provides per-CPU caching but without true restartable
	 * sequences - relies on checking CPU ID before and after access.
	 *
	 * Performance: ~20-30% slower than assembly due to:
	 * - Race conditions requiring retry
	 * - No kernel guarantee of atomicity
	 * - Extra CPU ID checks
	 */
	umem_magazine_t *mag = (umem_magazine_t *)rseq_cache->loaded_mag;
	if (mag != NULL && rseq_cache->rounds > 0) {
		/* Paranoid: check CPU didn't change during access */
		int cpu_before = cpu_id;
		obj = mag->mag_objs[--rseq_cache->rounds];
		int cpu_after = umem_rseq_get_cpu();

		if (cpu_before != cpu_after) {
			/* Migration race - restore and retry */
			rseq_cache->rounds++;
			rseq_cache->migration_count++;
			rseq_cache->restart_count++;
			return umem_cache_alloc_rseq_slowpath(cp, cpu_after, umflag);
		}

		rseq_cache->alloc_count++;
		return obj;
	}

	return umem_cache_alloc_rseq_slowpath(cp, cpu_id, umflag);
#endif
```

**Testing:**
```bash
# Test build on RISC-V
./configure --host=riscv64-unknown-linux-gnu --enable-rseq
make  # Should succeed with C fallback warning

# Test on x86_64
./configure --enable-rseq
make  # Should use assembly
```

**Acceptance Criteria:**
- ✅ Clean build on x86_64 (uses assembly)
- ✅ Clean build on aarch64 (uses assembly)
- ✅ Clean build on RISC-V (uses C fallback with warning)
- ✅ Runtime performance within 30% on C fallback

---

### Issue #3: Document Architecture Limitations

**Priority:** P0 - CRITICAL
**Estimated Time:** 1 hour
**Files:** `README.md`, `PORTABILITY_AUDIT.md`

**Problem:**
No documentation of pointer width limitations or architecture requirements.

**Solution:**

Add to `README.md` (after Architecture Support Matrix section):

```markdown
### Architecture-Specific Requirements

#### Virtual Address Width Limitation

The lock-free depot uses **tagged pointers** that pack a 48-bit pointer and
16-bit version counter into a single 64-bit word. This requires:

- **48-bit or smaller virtual address space**
- **Upper 16 bits of pointers must be zero**

**Supported Configurations:**

| Architecture | VA Width | Status | Notes |
|-------------|----------|--------|-------|
| x86_64 | 48-bit | ✅ | Canonical form, always 48-bit |
| aarch64 | 48-bit | ✅ | Default configuration |
| aarch64 | 52-bit | ❌ | **NOT SUPPORTED** - will crash |
| RISC-V Sv48 | 48-bit | ✅ | Supported |
| RISC-V Sv57 | 57-bit | ❌ | **NOT SUPPORTED** - will crash |

**Checking Your Configuration:**

```bash
# aarch64: Check VA_BITS configuration
grep CONFIG_ARM64_VA_BITS /boot/config-$(uname -r)
# Must show CONFIG_ARM64_VA_BITS_48=y (NOT _52)

# RISC-V: Check VA mode
dmesg | grep "Virtual kernel memory layout"
# Look for "vmalloc : 0xffxxxxxxxxxxxxxx" - must be 48-bit

# At runtime (debug builds)
./umem_test  # Will error if VA width incompatible
```

**Workarounds:**

If you must use 52-bit VA on aarch64:
1. Recompile kernel with CONFIG_ARM64_VA_BITS_48=y
2. Wait for alternative ABA solution (future release)

#### SIMD Performance

| Architecture | SIMD Support | Performance vs Scalar |
|-------------|--------------|----------------------|
| x86_64 | SSE2/AVX2 | 2x-4x faster |
| aarch64 | NEON | 2x faster |
| RISC-V (RVV) | Not yet | 1x (planned: 2x) |
| Other | Generic | 1x (baseline) |

#### Restartable Sequences (rseq)

| Architecture | rseq Support | Performance vs Fallback |
|-------------|--------------|------------------------|
| x86_64 | Assembly | ~2x faster |
| aarch64 | Assembly | ~2x faster |
| RISC-V | C fallback | ~1.3x faster than no rseq |
| Other | C fallback | ~1.3x faster |

**Note:** rseq requires Linux 4.18+ and proper kernel support.
```

**Acceptance Criteria:**
- ✅ Clear documentation of limitations
- ✅ Simple diagnostic commands
- ✅ Performance expectations per architecture
- ✅ Workaround guidance

---

## High Priority (P1) - Next Sprint

### Issue #4: Migrate to C11 Atomics

**Priority:** P1 - HIGH
**Estimated Time:** 6-8 hours
**Files:** `sol_compat.h`, `umem.c`

**Problem:**
x86-specific inline assembly for atomics. Not portable, prevents compiler optimizations.

**Solution:**

Replace in `sol_compat.h` (lines 124-168):

```c
// DELETE lines 124-168 (x86-specific assembly)

// REPLACE WITH:
#elif defined(__GNUC__) || defined(__clang__)
  /*
   * Modern C11 atomics - portable to all architectures.
   * Compiler generates optimal code for each platform.
   */

  static INLINE uint_t umem_atomic_cas(uint_t *mem, uint_t with, uint_t cmp)
  {
    _Atomic uint_t *atomic_mem = (_Atomic uint_t*)mem;
    atomic_compare_exchange_strong_explicit(
        atomic_mem, &cmp, with,
        memory_order_acq_rel, memory_order_acquire);
    return cmp;  // Updated to actual value on CAS failure
  }

  static INLINE uint64_t umem_atomic_cas64(uint64_t *mem, uint64_t with,
      uint64_t cmp)
  {
    _Atomic uint64_t *atomic_mem = (_Atomic uint64_t*)mem;
    atomic_compare_exchange_strong_explicit(
        atomic_mem, &cmp, with,
        memory_order_acq_rel, memory_order_acquire);
    return cmp;
  }

  static INLINE uint64_t umem_atomic_inc64(uint64_t *mem)
  {
    _Atomic uint64_t *atomic_mem = (_Atomic uint64_t*)mem;
    return atomic_fetch_add_explicit(atomic_mem, 1, memory_order_relaxed) + 1;
  }
  #define umem_atomic_inc64 umem_atomic_inc64
#else
  #error No atomic solution for your compiler
#endif
```

**Performance Testing:**

```bash
# Benchmark before/after on multiple architectures
for arch in x86_64 aarch64 riscv64; do
    echo "Testing $arch..."
    make clean
    ./configure --host=${arch}-linux-gnu
    make
    ./test/bench/bench_depot_contention
done
```

**Expected Results:**
- x86_64: Similar performance (strong memory model)
- aarch64: Possible improvement (compiler can optimize barriers)
- RISC-V: Possible improvement (compiler handles weak memory model)

**Acceptance Criteria:**
- ✅ Zero inline assembly in `sol_compat.h`
- ✅ Performance within 5% of assembly version
- ✅ Clean compilation on all architectures
- ✅ Passes all atomic operation tests

---

### Issue #5: Implement RISC-V Vector Support

**Priority:** P1 - HIGH
**Estimated Time:** 8-12 hours
**Files:** `umem_simd.h`, `configure.ac`

**Problem:**
RISC-V uses generic fallback, missing 2-4x SIMD speedup.

**Solution:**

1. Add detection to `configure.ac`:

```bash
# After aarch64 NEON detection (line 116)

# RISC-V Vector Extension detection
if test "$GENASM_DIR" = "riscv64"; then
    AC_MSG_CHECKING([for RISC-V Vector Extension])
    AC_COMPILE_IFELSE(
        [AC_LANG_PROGRAM([[
            #include <riscv_vector.h>
            ]], [[
            size_t vl = __riscv_vsetvl_e64m1(4);
            vuint64m1_t v = __riscv_vmv_v_x_u64m1(0, vl);
        ]])],
        [AC_MSG_RESULT([yes])
         AC_DEFINE([HAVE_RVV], [1], [RISC-V Vector Extension available])],
        [AC_MSG_RESULT([no])
         AC_MSG_WARN([RISC-V Vector Extension not available - using scalar fallback])]
    )
fi
```

2. Add to `umem_simd.h` (before generic fallback, line 165):

```c
#elif defined(HAVE_RVV)
	/*
	 * RISC-V Vector Extension path
	 * Variable-length vectors (VLEN typically 128 or 256 bits)
	 * Processes 2-4 pointers depending on VLEN
	 */
	#include <riscv_vector.h>

	size_t vl;
	for (int i = 0; i < count; i += vl) {
		/* Set vector length for 64-bit elements */
		vl = __riscv_vsetvl_e64m1(count - i);

		/* Load pointers as 64-bit unsigned */
		vuint64m1_t ptrs = __riscv_vle64_v_u64m1((uint64_t *)&array[i], vl);

		/* Compare with zero, creating mask of NULL pointers */
		vbool64_t mask = __riscv_vmseq_vx_u64m1_b64(ptrs, 0, vl);

		/* Count NULL pointers */
		size_t null_count = __riscv_vcpop_m_b64(mask, vl);

		/* If not all NULL, we found non-NULL pointer */
		if (null_count != vl) {
			return 1;
		}
	}
	return 0;

#elif defined(HAVE_RVV)  /* umem_mag_init_fast version */
	#include <riscv_vector.h>

	vuint64m1_t zero = __riscv_vmv_v_x_u64m1(0, __riscv_vsetvl_e64m1(1));
	size_t vl;

	for (int i = 0; i < count; i += vl) {
		vl = __riscv_vsetvl_e64m1(count - i);
		__riscv_vse64_v_u64m1((uint64_t *)&array[i], zero, vl);
	}

	/* Handle remaining if count not multiple of vl */
	for (int i = (count / vl) * vl; i < count; i++) {
		array[i] = NULL;
	}
```

**Testing:**

```bash
# Requires QEMU with RVV support or real hardware
nix develop .#riscv64
./configure --host=riscv64-unknown-linux-gnu CFLAGS="-march=rv64gcv"
make
./test/bench/bench_simd

# Expected: 2-4x speedup vs scalar
```

**Acceptance Criteria:**
- ✅ Compiles with `-march=rv64gcv` on RISC-V
- ✅ Graceful fallback if RVV not available
- ✅ 2-4x speedup in `bench_simd` vs scalar
- ✅ Passes all SIMD tests

---

### Issue #6: Implement RISC-V rseq Assembly

**Priority:** P1 - HIGH
**Estimated Time:** 12-16 hours
**Files:** `umem_rseq_riscv64.S` (new), `Makefile.am`, `umem_rseq.c`

**Problem:**
RISC-V uses C fallback for rseq, ~30% slower than assembly.

**Solution:**

Create `umem_rseq_riscv64.S`:

```asm
/*
 * RISC-V 64-bit assembly implementation of rseq critical sections
 * Requires Linux 4.18+ and RISC-V architecture
 */

#if defined(__riscv) && (__riscv_xlen == 64) && defined(__linux__)

.text

/*
 * rseq signature for abort points
 */
#define RSEQ_SIG 0x53053053

/*
 * Offsets into umem_rseq_cache_t structure
 */
#define CACHE_LOADED_MAG_OFFSET 0
#define CACHE_ROUNDS_OFFSET 16
#define CACHE_ALLOC_COUNT_OFFSET 24
#define CACHE_RESTART_COUNT_OFFSET 40

/*
 * umem_rseq_alloc_fastpath - Allocate using rseq critical section
 *
 * Arguments:
 *   a0 = cache (umem_rseq_cache_t *)
 *   a1 = cpu_id (int)
 *
 * Returns:
 *   a0 = allocated object or NULL
 */
.globl umem_rseq_alloc_fastpath
.type umem_rseq_alloc_fastpath, @function
.align 4

umem_rseq_alloc_fastpath:
	.cfi_startproc

	/* Save frame and callee-saved registers */
	addi	sp, sp, -96
	.cfi_adjust_cfa_offset 96
	sd	ra, 88(sp)
	.cfi_offset 1, -8
	sd	s0, 80(sp)
	.cfi_offset 8, -16
	sd	s1, 72(sp)
	.cfi_offset 9, -24
	sd	s2, 64(sp)
	.cfi_offset 18, -32

	/* Save arguments */
	mv	s0, a0		# s0 = cache
	mv	s1, a1		# s1 = cpu_id

	/* Set up rseq_cs structure on stack (32-byte aligned) */
	addi	s2, sp, 32	# s2 = &rseq_cs

	/* Fill rseq_cs: version=0, flags=0 */
	sd	zero, 0(s2)

	/* start_ip */
	lla	t0, .Lrseq_alloc_start
	sd	t0, 8(s2)

	/* post_commit_offset */
	lla	t1, .Lrseq_alloc_post_commit
	sub	t1, t1, t0
	sd	t1, 16(s2)

	/* abort_ip */
	lla	t2, .Lrseq_alloc_abort
	sd	t2, 24(s2)

	/* Get TLS base (tp = x4 in RISC-V) */
	/* Register rseq_cs with kernel */
	/* Note: RISC-V TLS uses tp directly */
	lla	t0, umem_rseq_area
	add	t0, t0, tp	# Add TLS offset
	sd	s2, 8(t0)	# umem_rseq_area.rseq_cs = &rseq_cs

.Lrseq_alloc_start:
	/* CRITICAL SECTION - kernel restarts here on migration */

	/* Verify CPU hasn't changed */
	lw	t1, 4(t0)	# Load current cpu_id
	bne	t1, s1, .Lrseq_alloc_abort

	/* Load rounds */
	lw	t2, CACHE_ROUNDS_OFFSET(s0)
	blez	t2, .Lrseq_alloc_empty

	/* Load magazine pointer */
	ld	t3, CACHE_LOADED_MAG_OFFSET(s0)
	beqz	t3, .Lrseq_alloc_empty

	/* Decrement rounds */
	addi	t4, t2, -1
	sw	t4, CACHE_ROUNDS_OFFSET(s0)

	/* Get object: mag->mag_round[rounds-1] */
	/* Magazine structure: void *mag_next, void *mag_round[] */
	slli	t2, t2, 3	# rounds * 8
	add	t3, t3, t2
	ld	a0, 0(t3)	# Load pointer

	/* Increment allocation counter */
	ld	t5, CACHE_ALLOC_COUNT_OFFSET(s0)
	addi	t5, t5, 1
	sd	t5, CACHE_ALLOC_COUNT_OFFSET(s0)

	/* COMMIT POINT */
.Lrseq_alloc_post_commit:
	/* Clear rseq_cs */
	lla	t0, umem_rseq_area
	add	t0, t0, tp
	sd	zero, 8(t0)

	/* Restore and return */
	ld	s2, 64(sp)
	ld	s1, 72(sp)
	ld	s0, 80(sp)
	ld	ra, 88(sp)
	addi	sp, sp, 96
	.cfi_adjust_cfa_offset -96
	ret

.Lrseq_alloc_empty:
	/* Magazine empty - return NULL */
	mv	a0, zero
	j	.Lrseq_alloc_post_commit

	/* ABORT HANDLER */
	.align 5
.Lrseq_alloc_abort:
	.word	RSEQ_SIG

	/* Increment restart counter */
	ld	t5, CACHE_RESTART_COUNT_OFFSET(s0)
	addi	t5, t5, 1
	sd	t5, CACHE_RESTART_COUNT_OFFSET(s0)

	/* Clear rseq_cs */
	lla	t0, umem_rseq_area
	add	t0, t0, tp
	sd	zero, 8(t0)

	/* Return NULL */
	mv	a0, zero
	ld	s2, 64(sp)
	ld	s1, 72(sp)
	ld	s0, 80(sp)
	ld	ra, 88(sp)
	addi	sp, sp, 96
	ret

	.cfi_endproc
.size umem_rseq_alloc_fastpath, .-umem_rseq_alloc_fastpath

/* TODO: Implement umem_rseq_free_fastpath similarly */

#endif /* __riscv && __linux__ */
```

**Testing:**

```bash
# Build and test
nix develop .#riscv64
./configure --host=riscv64-unknown-linux-gnu --enable-rseq
make
qemu-riscv64-static -cpu rv64,rseq=on ./umem_test

# Benchmark vs C fallback
./test/bench/bench_rseq
```

**Acceptance Criteria:**
- ✅ Compiles on RISC-V
- ✅ rseq critical sections work correctly
- ✅ ~2x faster than C fallback
- ✅ Passes stress tests

---

## Medium Priority (P2) - Next Quarter

### Issue #7: Runtime CPU Feature Detection

**Priority:** P2 - MEDIUM
**Estimated Time:** 6-8 hours
**Files:** `umem_init.c`, `umem_simd.h`

**Implementation:** See PORTABILITY_AUDIT.md Section 2.3

### Issue #8: Architecture Abstraction Layer

**Priority:** P2 - MEDIUM
**Estimated Time:** 2-3 days
**Impact:** Long-term maintainability

**Implementation:** See PORTABILITY_AUDIT.md Section 3.3, Strategy 1

### Issue #9: Alternative ABA Solution

**Priority:** P2 - MEDIUM
**Estimated Time:** 1-2 weeks
**Files:** `umem_impl.h`, `umem.c`

**Implementation:** See PORTABILITY_AUDIT.md Section 3.3, Strategy 2

---

## Summary

### This Week (P0)
1. **4 hours:** Add pointer width validation
2. **2 hours:** Fix rseq build guards
3. **2 hours:** Document limitations

**Total: 8 hours** (1 day)

### Next Sprint (P1)
1. **8 hours:** Migrate to C11 atomics
2. **12 hours:** RISC-V vector support
3. **16 hours:** RISC-V rseq assembly

**Total: 36 hours** (4.5 days)

### Next Quarter (P2)
1. **8 hours:** Runtime CPU detection
2. **16 hours:** Architecture abstraction layer
3. **80 hours:** Alternative ABA solution

**Total: 104 hours** (13 days)

---

## Acceptance Testing

After each phase, verify:

```bash
# Clean build on all architectures
for arch in x86_64 i386 aarch64 riscv64; do
    ./configure --host=${arch}-linux-gnu
    make clean && make -j$(nproc)
    make check
done

# Performance regression tests
./test/bench/bench_main
./test/bench/bench_depot_contention
./test/bench/bench_simd

# Stress tests
./test/integration/test_threading_stress
```

**Success Criteria:**
- ✅ All architectures build cleanly
- ✅ Tests pass on all architectures
- ✅ Performance within 10% of baseline
- ✅ Clear error messages on unsupported configs
