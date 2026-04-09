# Portability Audit - Document Index

**Audit Date:** 2026-04-09
**Audit Scope:** Comprehensive review of x86_64-centric code (Sun Microsystems Reviewer #3 concern)
**Status:** 🔴 CRITICAL ISSUES IDENTIFIED - ACTION REQUIRED

---

## Executive Summary

Sun Microsystems Reviewer #3's concerns about x86_64-centric code are **VALID and CRITICAL**. Recent performance optimizations (SIMD vectorization, rseq, lock-free depot with tagged pointers) introduced architecture-specific code that:

1. **BREAKS** on aarch64 with 52-bit VA (pointer truncation)
2. **BREAKS** on RISC-V Sv57 (pointer truncation)
3. **FAILS TO BUILD** on RISC-V/SPARC (unconditional x86_64 assembly compilation)
4. **DEGRADES PERFORMANCE** by 40-70% on RISC-V (missing SIMD and rseq assembly)

**Good news:** Issues are fixable with 8 hours of critical work + 4.5 days of optimization.

---

## Document Guide

### For Project Managers / Decision Makers

**Start here:** [`PORTABILITY_SUMMARY.md`](PORTABILITY_SUMMARY.md) (7 pages)
- Executive summary with impact analysis
- Architecture support matrix
- Risk assessment
- Time estimates (8 hours P0, 36 hours P1, 104 hours P2)

### For Developers / Implementers

**Start here:** [`ACTION_ITEMS_PORTABILITY.md`](ACTION_ITEMS_PORTABILITY.md) (20 pages)
- Specific actionable tasks with code examples
- Issue #1-9 with full implementation details
- Testing procedures
- Acceptance criteria

**Quick reference:** [`PORTABILITY_QUICK_REF.md`](PORTABILITY_QUICK_REF.md) (7 pages)
- Pre-commit checklist
- Common pitfalls to avoid
- Quick fixes and templates
- Red flags to watch for

### For Architects / Technical Reviewers

**Start here:** [`PORTABILITY_AUDIT.md`](PORTABILITY_AUDIT.md) (25 pages)
- Comprehensive technical analysis
- Code location inventory (exact file:line references)
- Memory layout analysis (48-bit pointer packing)
- Architecture abstraction strategies
- Long-term recommendations

---

## Document Details

| Document | Pages | Lines | Size | Purpose |
|----------|-------|-------|------|---------|
| **PORTABILITY_AUDIT.md** | 25 | 874 | 25KB | Full technical analysis |
| **ACTION_ITEMS_PORTABILITY.md** | 20 | 800 | 20KB | Implementation guide |
| **PORTABILITY_SUMMARY.md** | 7 | 274 | 7.3KB | Executive summary |
| **PORTABILITY_QUICK_REF.md** | 7 | 354 | 7.6KB | Developer cheat sheet |
| **PORTABILITY_INDEX.md** | 1 | - | - | This document |

**Total:** ~60 pages, 2,302 lines of documentation

---

## Critical Issues At A Glance

### Issue #1: Tagged Pointer 48-bit Assumption (P0 - CRITICAL)

**Impact:** 💥 Silent data corruption on aarch64 52-bit VA, RISC-V Sv57

**Location:** `umem_impl.h:340-378`

**Fix Time:** 2 hours

**Details:** See ACTION_ITEMS_PORTABILITY.md Issue #1

---

### Issue #2: x86_64 Assembly Always Compiled (P0 - CRITICAL)

**Impact:** ❌ Build failures on RISC-V, SPARC

**Location:** `Makefile.am:47`, `umem_rseq_x86_64.S`

**Fix Time:** 1 hour

**Details:** See ACTION_ITEMS_PORTABILITY.md Issue #2

---

### Issue #3: Missing RISC-V Optimizations (P1 - HIGH)

**Impact:** 🐌 40-70% performance degradation

**Location:** `umem_simd.h`, `umem_rseq.c`

**Fix Time:** 28 hours (SIMD: 12h, rseq: 16h)

**Details:** See ACTION_ITEMS_PORTABILITY.md Issues #5, #6

---

## Timeline

### Week 1 (P0 - Blocking Issues)

**Time:** 8 hours (1 day)

**Deliverables:**
1. Pointer width validation (compile-time + runtime)
2. Conditional assembly compilation
3. Architecture limitations documented

**Result:** Clean builds on all architectures, no silent failures

### Weeks 2-3 (P1 - Performance)

**Time:** 36 hours (4.5 days)

**Deliverables:**
1. C11 atomics migration (remove inline assembly)
2. RISC-V SIMD support (RVV)
3. RISC-V rseq assembly

**Result:** RISC-V performance within 10% of x86_64

### Month 2+ (P2 - Future-Proofing)

**Time:** 104 hours (13 days)

**Deliverables:**
1. Runtime CPU feature detection
2. Architecture abstraction layer
3. Alternative ABA solution (remove VA width limit)

**Result:** Support for future 52-bit+ VA architectures

---

## Key Findings

### What's Broken 🔴

1. **Tagged pointers** - Assume 48-bit VA (lines: umem_impl.h:340-378)
2. **Build system** - Always compiles x86_64 assembly (Makefile.am:47)
3. **Inline assembly** - x86-specific atomics (sol_compat.h:124-168)
4. **SIMD** - No RISC-V vector support (umem_simd.h:165-176)
5. **rseq** - No RISC-V assembly (missing umem_rseq_riscv64.S)

### What Works Well ✅

1. **TLS abstraction** - Multi-arch support (tmem_stubs.c:43-62)
2. **Memory ordering** - Portable atomics (umem.c:2236-2255)
3. **Fallbacks** - Generic C implementations available
4. **Architecture detection** - Good build system (configure.ac:66-103)
5. **Documentation** - Good inline comments

---

## Testing Strategy

### Immediate Testing (After P0 Fixes)

```bash
# Build on all supported architectures
for arch in x86_64 i386 aarch64 riscv64; do
  ./configure --host=${arch}-linux-gnu
  make clean && make
  make check
done

# Verify pointer width checks work
./configure CFLAGS="-DDEBUG"
make
./umem_test  # Should validate pointer constraints
```

### Performance Testing (After P1 Improvements)

```bash
# Benchmark across architectures
./test/bench/bench_main
./test/bench/bench_depot_contention
./test/bench/bench_simd

# Stress test
./test/integration/test_threading_stress --threads=64 --duration=300
```

### Regression Testing (Ongoing)

```bash
# CI matrix
- x86_64 (native)
- i386 (cross-compile)
- aarch64 (QEMU user-mode)
- riscv64 (QEMU user-mode)
```

---

## Architecture Support Matrix

| Architecture | Build | Runtime | Perf | SIMD | rseq | Notes |
|-------------|-------|---------|------|------|------|-------|
| **x86_64** | ✅ | ✅ | 100% | AVX2/SSE2 | ✅ ASM | Production ready |
| **i386** | ✅ | ✅ | 95% | Generic | ❌ | Stable |
| **aarch64 (48-bit)** | ✅ | ✅ | 90% | NEON | ✅ ASM | Production ready |
| **aarch64 (52-bit)** | ✅ | 💥 | N/A | - | - | **BROKEN** (P0) |
| **RISC-V Sv48** | ⚠️ | ✅ | 60% | Generic | ❌ C | Slow (P1) |
| **RISC-V Sv57** | ⚠️ | 💥 | N/A | - | - | **BROKEN** (P0) |
| **SPARC** | ⚠️ | ⚠️ | 50% | Generic | ❌ | Template only |
| **Other** | ❌ | ❌ | N/A | - | - | Unsupported |

---

## Recommended Reading Order

### If you have 5 minutes:
Read [`PORTABILITY_SUMMARY.md`](PORTABILITY_SUMMARY.md) sections 1-2

### If you have 30 minutes:
1. [`PORTABILITY_SUMMARY.md`](PORTABILITY_SUMMARY.md) - Full overview
2. [`ACTION_ITEMS_PORTABILITY.md`](ACTION_ITEMS_PORTABILITY.md) - Issues #1-3 (P0)

### If you have 2 hours:
1. [`PORTABILITY_SUMMARY.md`](PORTABILITY_SUMMARY.md)
2. [`ACTION_ITEMS_PORTABILITY.md`](ACTION_ITEMS_PORTABILITY.md) - All issues
3. [`PORTABILITY_QUICK_REF.md`](PORTABILITY_QUICK_REF.md)

### If you're implementing fixes:
1. [`ACTION_ITEMS_PORTABILITY.md`](ACTION_ITEMS_PORTABILITY.md) - Your specific issue
2. [`PORTABILITY_QUICK_REF.md`](PORTABILITY_QUICK_REF.md) - Keep open as reference
3. [`PORTABILITY_AUDIT.md`](PORTABILITY_AUDIT.md) - For deep technical details

### If you're reviewing code:
Keep [`PORTABILITY_QUICK_REF.md`](PORTABILITY_QUICK_REF.md) open during review

---

## Frequently Asked Questions

### Q: How bad is this?

**A:** Critical but fixable. The issues are real (crashes on some configs, build failures on others), but we have clear fixes that take 8 hours for critical issues, 4.5 days for optimization.

### Q: Can we ship without fixing these?

**A:** No. The tagged pointer issue causes silent data corruption on some valid configurations (aarch64 52-bit VA). This is a **security and stability issue**, not just performance.

### Q: Which issues must be fixed immediately?

**A:** P0 issues #1-3 (8 hours total):
1. Pointer width validation
2. Conditional assembly compilation
3. Documentation of limitations

### Q: Can we delay P1 optimizations?

**A:** Yes, but RISC-V will be 40-70% slower than x86_64. If RISC-V is tier-1 target, fix P1 in next sprint.

### Q: What about P2 future-proofing?

**A:** Can be delayed. P2 enables support for future 52-bit+ VA architectures, but current configs work with P0+P1 fixes.

### Q: How do we prevent this in the future?

**A:**
1. Use [`PORTABILITY_QUICK_REF.md`](PORTABILITY_QUICK_REF.md) pre-commit checklist
2. Add portability CI checks (build on multiple architectures)
3. Require arch-specific code to have fallbacks
4. Run `grep -r "__x86_64__" src/` before each PR

---

## Contact & Next Steps

### Immediate Actions

1. **Review:** Project lead reviews [`PORTABILITY_SUMMARY.md`](PORTABILITY_SUMMARY.md)
2. **Assign:** Assign P0 issues #1-3 to developers (8 hours total)
3. **Track:** Create GitHub issues for P1 items
4. **Test:** Set up CI for multi-arch builds

### Long-Term Actions

1. **Policy:** Adopt portability guidelines from [`PORTABILITY_QUICK_REF.md`](PORTABILITY_QUICK_REF.md)
2. **CI:** Add aarch64 and RISC-V to build matrix
3. **Review:** Require portability review for arch-specific code
4. **Training:** Share portability best practices with team

---

## Document Revision History

| Date | Version | Changes |
|------|---------|---------|
| 2026-04-09 | 1.0 | Initial comprehensive audit |

---

**Audit Status:** ✅ COMPLETE - Ready for implementation

**Next Review:** After P0 fixes (1 week), then after P1 optimizations (3 weeks)
