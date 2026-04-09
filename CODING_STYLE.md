# libumem Coding Style Guide

This document defines the coding standards for libumem, a portable version of Solaris libumem. The codebase maintains consistency with its Solaris heritage while incorporating modern C standards where appropriate.

## Table of Contents

1. [C Standard and Compiler Requirements](#c-standard-and-compiler-requirements)
2. [File Organization](#file-organization)
3. [Naming Conventions](#naming-conventions)
4. [Formatting Rules](#formatting-rules)
5. [Comments and Documentation](#comments-and-documentation)
6. [Type Definitions and Declarations](#type-definitions-and-declarations)
7. [Functions](#functions)
8. [Error Handling](#error-handling)
9. [Preprocessor Directives](#preprocessor-directives)
10. [Architecture-Specific Code](#architecture-specific-code)
11. [Modern C Features](#modern-c-features)

## C Standard and Compiler Requirements

### Standard: C17 (ISO/IEC 9899:2018)

**Rationale:**
- C17 provides technical corrections to C11 without introducing breaking changes
- Widely supported by GCC 4.8+, Clang 3.5+, and modern compilers
- Allows use of modern features while maintaining compatibility with Solaris heritage code
- Provides standardized atomics (`stdatomic.h`), thread-local storage, and static assertions

**Compatibility Notes:**
- Core Solaris-heritage files (umem.c, vmem.c) may contain C99-style code from original implementation
- New code should use C17 features where beneficial (atomics, `_Static_assert`, `_Alignas`)
- GNU extensions are acceptable when wrapped in feature detection macros

**Compiler Flags:**
```makefile
CFLAGS += -std=gnu17 -Wall -Wextra -Wno-unused-parameter
```

## File Organization

### Header Files

**Include Guards:**
Use traditional `#ifndef` include guards (not `#pragma once`) for Solaris compatibility:

```c
#ifndef _UMEM_IMPL_H
#define _UMEM_IMPL_H

/* header content */

#endif /* _UMEM_IMPL_H */
```

**Guard Naming:**
- Underscore prefix: `_FILENAME_H`
- All uppercase with underscores separating words
- Matches Solaris convention

**Header Structure:**
```c
/*
 * CDDL HEADER START
 * [Full CDDL license text]
 * CDDL HEADER END
 */

/*
 * Copyright notices
 */

#ifndef _HEADER_NAME_H
#define _HEADER_NAME_H

#include "config.h"  /* autoconf configuration */

#ifdef __cplusplus
extern "C" {
#endif

/* Feature detection macros */
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

/* Type definitions */
/* Function declarations */
/* Inline functions (if any) */

#ifdef __cplusplus
}
#endif

#endif /* _HEADER_NAME_H */
```

### Source Files

**Include Order:**
1. `config.h` (always first)
2. System headers (`<sys/*.h>`)
3. Standard C library headers (`<stdio.h>`, `<stdlib.h>`, etc.)
4. Public API headers (`<umem.h>`, `<sys/vmem.h>`)
5. Private implementation headers (`umem_impl.h`, `umem_base.h`)
6. Architecture-specific headers

**Example:**
```c
#include "config.h"

#include <sys/mman.h>
#include <sys/types.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <umem.h>
#include <sys/vmem.h>

#include "umem_impl.h"
#include "umem_base.h"
```

## Naming Conventions

### Public API Functions

**Format:** `umem_*` or `vmem_*`

```c
void *umem_alloc(size_t size, int flags);
void umem_free(void *buf, size_t size);
umem_cache_t *umem_cache_create(...);
void *vmem_alloc(vmem_t *vmp, size_t size, int vmflag);
```

### Internal Functions

**Static functions:** No prefix required, descriptive names

```c
static void copy_pattern(uint64_t pattern, void *buf_arg, size_t size);
static vmem_seg_t *vmem_getseg_global(void);
static int umem_add_update_unlocked(umem_cache_t *cp, int flags);
```

**Non-static internal functions:** `_umem_*` or `_vmem_*` prefix

```c
void *_umem_cache_alloc(umem_cache_t *cp, int flags);
void *_vmem_extend_alloc(vmem_t *vmp, void *vaddr, size_t size, ...);
```

### Types

**Structures:** `umem_*` or `vmem_*` prefix, `_t` suffix

```c
typedef struct umem_cache {
    /* fields */
} umem_cache_t;

typedef struct vmem_seg {
    /* fields */
} vmem_seg_t;
```

**Enums:** `umem_*` prefix, `_t` suffix optional

```c
typedef enum umem_numa_policy {
    UMEM_NUMA_POLICY_AUTO = 0,
    UMEM_NUMA_POLICY_LOCAL,
    UMEM_NUMA_POLICY_INTERLEAVE,
} umem_numa_policy_t;
```

**Structure Members:**
- No prefix/suffix for public API structures
- Private structures may use prefixes to avoid name collisions

```c
/* Public API structure */
typedef struct umem_cache {
    char            cache_name[32];
    size_t          cache_bufsize;
    size_t          cache_align;
} umem_cache_t;

/* Private structure with prefix */
typedef struct vmem_seg {
    uintptr_t       vs_start;
    uintptr_t       vs_end;
    struct vmem_seg *vs_anext;
    struct vmem_seg *vs_aprev;
} vmem_seg_t;
```

### Macros and Constants

**Public constants:** `UMEM_*` or `VMEM_*` prefix

```c
#define UMEM_DEFAULT      0x0000
#define UMEM_NOFAIL       0x0100
#define VM_BESTFIT        0x00
#define VM_INSTANTFIT     0x01
```

**Flag macros:** Prefix with `UMF_` (umem flags) or similar

```c
#define UMF_AUDIT         0x00000001
#define UMF_DEADBEEF      0x00000002
#define UMF_REDZONE       0x00000004
```

**Internal macros:** Use descriptive names, may use component prefix

```c
#define UMEM_CACHE_LINE_SIZE    64
#define VMEM_SEG_INITIAL        100
#define PAGESIZE                pagesize
```

### Variables

**Global variables:** Prefix with `umem_` or `vmem_`

```c
extern size_t pagesize;
extern vmem_t *vmem_heap;
extern uint32_t umem_max_ncpus;
```

**Static file-scope variables:** No specific prefix

```c
static vmem_seg_t vmem_seg0[VMEM_SEG_INITIAL];
static mutex_t vmem_list_lock = DEFAULTMUTEX;
```

**Local variables:** Descriptive names, conventional abbreviations acceptable

```c
void example_function(void)
{
    umem_cache_t *cp;        /* cache pointer */
    vmem_seg_t *vsp;         /* vmem segment pointer */
    size_t size;
    int cpu_seqid;
    int i, j;                /* loop counters */
}
```

## Formatting Rules

### Indentation and Whitespace

**Indentation:** Tabs (8-space width)

```c
void
example_function(void)
{
	if (condition) {
		do_something();
	}
}
```

**Line Length:** 80 characters maximum

Long lines should be broken at natural boundaries:

```c
/* Good */
static vmem_seg_t *
vmem_seg_create(vmem_t *vmp, vmem_seg_t *vprev,
    uintptr_t start, uintptr_t end)
{
	/* ... */
}

/* Good - break before operators */
if (cache->cache_flags & UMF_AUDIT &&
    cache->cache_flags & UMF_DEADBEEF &&
    size > UMEM_MAXBUF) {
	/* ... */
}
```

**Function continuation:** Align parameters with opening parenthesis:

```c
umem_cache_t *
umem_cache_create(const char *name, size_t bufsize,
    size_t align, umem_constructor_t *constructor,
    umem_destructor_t *destructor, umem_reclaim_t *reclaim,
    void *private, vmem_t *vmp, int cflags)
{
	/* ... */
}
```

### Braces and Block Structure

**Brace Style:** Linux kernel style (K&R variant)

```c
/* Function definitions: opening brace on new line */
void
function_name(void)
{
	/* body */
}

/* Control structures: opening brace on same line */
if (condition) {
	statement;
} else if (other_condition) {
	statement;
} else {
	statement;
}

while (condition) {
	statement;
}

for (i = 0; i < count; i++) {
	statement;
}

switch (value) {
case VALUE1:
	statement;
	break;
case VALUE2:
	statement;
	break;
default:
	statement;
	break;
}
```

**Single-statement blocks:** Always use braces (safety and consistency)

```c
/* Good */
if (condition) {
	return -1;
}

/* Avoid */
if (condition)
	return -1;
```

### Function Declarations

**Return type on separate line for definitions:**

```c
static void
copy_pattern(uint64_t pattern, void *buf_arg, size_t size)
{
	uint64_t *bufend = (uint64_t *)((char *)buf_arg + size);
	uint64_t *buf = buf_arg;

	while (buf < bufend)
		*buf++ = pattern;
}
```

**Prototypes in headers:** Single line if it fits, otherwise break at parameters

```c
/* Single line */
extern void umem_cache_destroy(umem_cache_t *cp);

/* Multi-line */
extern umem_cache_t *umem_cache_create(const char *name,
    size_t bufsize, size_t align, umem_constructor_t *constructor,
    umem_destructor_t *destructor, umem_reclaim_t *reclaim,
    void *private, vmem_t *vmp, int cflags);
```

### Spacing

**Binary operators:** Spaces around binary operators

```c
x = y + z;
if (a == b && c != d) {
	/* ... */
}
```

**Unary operators:** No space after

```c
*ptr = value;
&variable;
!condition;
~bitmask;
++counter;
```

**After keywords:** Space after control flow keywords

```c
if (condition)
while (condition)
for (i = 0; i < n; i++)
switch (value)
return value;
```

**Function calls:** No space before opening parenthesis

```c
function(arg1, arg2);
umem_alloc(size, flags);
```

**Casts:** No space after cast

```c
ptr = (uint64_t *)buffer;
value = (int)long_value;
```

**Pointer declarations:** Pointer asterisk adjacent to variable name

```c
char *name;
void *ptr;
umem_cache_t *cp;

/* Function pointers */
void (*callback)(void *arg);
```

## Comments and Documentation

### Comment Style

**Primary style:** C-style block comments `/* */`

```c
/*
 * This is a multi-line comment explaining
 * the algorithm or behavior.
 */
```

**C++ style acceptable for:** Short inline comments in modern code

```c
int cpu_id;  // Current CPU identifier
```

**Avoid in:** Core Solaris-heritage files (umem.c, vmem.c) for consistency

### Documentation Comments

**Function documentation:** Block comment above function

```c
/*
 * umem_cache_alloc - Allocate object from cache
 *
 * Allocates an object from the specified cache. If the cache has a
 * constructor, it will be called before returning the object.
 *
 * Parameters:
 *   cp     - Cache to allocate from
 *   flags  - Allocation flags (UMEM_DEFAULT or UMEM_NOFAIL)
 *
 * Returns:
 *   Pointer to allocated object, or NULL on failure (unless UMEM_NOFAIL)
 */
void *
umem_cache_alloc(umem_cache_t *cp, int flags)
{
	/* ... */
}
```

**Public API:** Use Doxygen-style comments in headers

```c
/*!
 * \brief Allocate memory from umem
 *
 * \param size Size in bytes to allocate
 * \param flags Allocation flags (UMEM_DEFAULT or UMEM_NOFAIL)
 * \return Pointer to allocated memory or NULL on failure
 */
extern void *umem_alloc(size_t size, int flags);
```

### Inline Comments

**Purpose:** Explain why, not what

```c
/* Good - explains reasoning */
/*
 * We must hold the cache lock here to prevent race conditions
 * with concurrent umem_cache_destroy() calls.
 */
(void) mutex_lock(&cp->cache_lock);

/* Avoid - restates obvious code */
/* Lock the mutex */
(void) mutex_lock(&cp->cache_lock);
```

**Algorithmic explanations:**

```c
/*
 * Binary search to find the appropriate size class.
 * Uses a table of power-of-2 sizes from 8 bytes to 16KB.
 */
for (low = 0, high = nalloc_sizes - 1; low <= high; ) {
	mid = (low + high) / 2;
	/* ... */
}
```

### Section Headers

**Large file organization:**

```c
/*
 * ============================================================================
 * Magazine Layer Implementation
 * ============================================================================
 */
```

### TODO and FIXME Comments

```c
/* TODO: Implement NUMA-aware slab allocation */
/* FIXME: Race condition possible on cache destruction */
/* XXX: This assumes page size is power of 2 */
```

## Type Definitions and Declarations

### Variable Declarations

**Modern C17 style:** Declare variables where needed (block scope)

```c
void
modern_function(void)
{
	/* Declare at first use */
	for (int i = 0; i < count; i++) {
		/* ... */
	}

	if (condition) {
		void *ptr = allocate_something();
		/* use ptr */
	}
}
```

**Solaris-heritage style:** Declarations at block start (C89-style)

This style is preserved in core files (umem.c, vmem.c) for consistency:

```c
static void
legacy_function(void)
{
	umem_cache_t *cp;
	vmem_seg_t *vsp;
	int i, j;
	size_t size;

	/* Function body */
	cp = find_cache();
	/* ... */
}
```

**New files:** Use modern C17 style with block-scope declarations

### Type Definitions

**typedef struct pattern:**

```c
typedef struct umem_cache {
	char            cache_name[32];
	size_t          cache_bufsize;
	umem_constructor_t *cache_constructor;
	umem_destructor_t *cache_destructor;
} umem_cache_t;
```

**Opaque types:** Forward declaration in public headers

```c
/* Public header */
typedef struct umem_cache umem_cache_t;

/* Implementation header */
struct umem_cache {
	/* actual definition */
};
```

### Alignment and Packing

**Cache-line alignment:** Use GCC attributes

```c
typedef struct umem_cpu_cache {
	/* ... */
} umem_cpu_cache_t __attribute__((aligned(64)));

#define UMEM_CACHE_ALIGNED __attribute__((aligned(UMEM_CACHE_LINE_SIZE)))
```

**Packing:** Use `__attribute__((packed))` sparingly

```c
typedef struct on_disk_format {
	uint32_t magic;
	uint16_t version;
	uint16_t flags;
} __attribute__((packed)) disk_format_t;
```

## Functions

### Function Naming

See [Naming Conventions](#naming-conventions) section.

### Function Length

**Maximum:** 100 lines (guideline, not hard limit)

**Rationale:**
- Improves readability and maintainability
- Easier to understand and test
- If a function exceeds 100 lines, consider refactoring

**Exception:** Auto-generated code, lookup tables, extensive switch statements

### Static Functions

**Default:** Mark internal functions `static` unless they need external linkage

```c
static void
internal_helper(void)
{
	/* ... */
}
```

### Inline Functions

**Use sparingly:** Only for performance-critical hot paths

```c
static inline int __attribute__((always_inline))
get_cached_cpu_hint(void)
{
	int hint = cached_cpu_hint;
	if (unlikely(hint == -1)) {
		/* ... */
	}
	return hint;
}
```

**Guidelines:**
- Must be in header files for compiler visibility
- Should be small (< 10 lines)
- Document why inlining is necessary

## Error Handling

### Return Value Conventions

**Pointer-returning functions:** Return `NULL` on error

```c
void *
umem_alloc(size_t size, int flags)
{
	/* ... */
	if (failure) {
		return (NULL);
	}
	return (buf);
}
```

**Integer-returning functions:** Return -1 on error, 0 on success

```c
int
umem_numa_init(void)
{
	/* ... */
	if (failure) {
		return (-1);
	}
	return (0);
}
```

**Return statement style:**

Both styles are acceptable; maintain consistency within a file:

```c
/* Solaris style with parentheses */
return (value);
return (NULL);

/* Standard C style without parentheses */
return value;
return NULL;
```

Core files (umem.c, vmem.c) use Solaris style; new files may use either.

### Error Checking

**Check all allocations:**

```c
cp = umem_cache_create(...);
if (cp == NULL) {
	/* handle error */
	return (-1);
}
```

**Use `unlikely()` for error paths in hot code:**

```c
if (unlikely(ptr == NULL)) {
	handle_error();
	return (NULL);
}
```

### Assertions

**Use `ASSERT()` macro for invariants:**

```c
ASSERT(cp != NULL);
ASSERT(size > 0);
ASSERT(mutex_held(&lock));
```

**Do not use for user input validation:**

```c
/* Wrong */
ASSERT(user_size < MAX_SIZE);

/* Correct */
if (user_size >= MAX_SIZE) {
	return (NULL);
}
```

## Preprocessor Directives

### Conditional Compilation

**Feature detection:** Use autoconf-generated `config.h` macros

```c
#ifdef HAVE_PTHREAD_H
#include <pthread.h>
#endif

#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif
```

**Platform-specific code:**

```c
#ifdef __linux__
	/* Linux-specific implementation */
#elif defined(__sun)
	/* Solaris implementation */
#elif defined(__FreeBSD__)
	/* FreeBSD implementation */
#else
	/* Generic fallback */
#endif
```

### Macro Definitions

**Multi-line macros:** Use backslash continuation, align backslashes

```c
#define VMEM_INSERT(vprev, vsp, type)				\
{								\
	vmem_seg_t *vnext = (vprev)->vs_##type##next;		\
	(vsp)->vs_##type##next = (vnext);			\
	(vsp)->vs_##type##prev = (vprev);			\
	(vprev)->vs_##type##next = (vsp);			\
	(vnext)->vs_##type##prev = (vsp);			\
}
```

**Function-like macros:** Use `do { ... } while (0)` pattern

```c
#define UMEM_PREFETCH_BATCH(base, stride, count)	\
	do {						\
		for (int _i = 0; _i < (count); _i++) {	\
			UMEM_PREFETCH_READ((char *)(base) + (_i) * (stride)); \
		}					\
	} while (0)
```

**Macro parameters:** Always parenthesize

```c
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
```

## Architecture-Specific Code

### Organization

**Architecture-specific code:** Separate files in `arch/` directory

```
arch/
  x86_64/
    umem_genasm_x86_64.c
  i386/
    umem_genasm_i386.c
  aarch64/
    umem_genasm_aarch64.c  (stub)
```

**Header organization:**

```c
/* Generic header */
#if defined(__x86_64__)
#include "arch/x86_64/specific.h"
#elif defined(__i386__)
#include "arch/i386/specific.h"
#elif defined(__aarch64__)
#include "arch/aarch64/specific.h"
#endif
```

### CPU Feature Detection

**Runtime detection:**

```c
#ifdef HAVE_SSE2
	if (cpu_has_sse2()) {
		/* SSE2 path */
	} else
#endif
	{
		/* Fallback path */
	}
```

**Compile-time selection:**

```c
#ifdef HAVE_AVX2
static inline int
umem_mag_scan_notnull(void **array, int count)
{
	/* AVX2 implementation */
}
#elif defined(HAVE_SSE2)
static inline int
umem_mag_scan_notnull(void **array, int count)
{
	/* SSE2 implementation */
}
#else
static inline int
umem_mag_scan_notnull(void **array, int count)
{
	/* Scalar implementation */
}
#endif
```

### Assembly Code

**Inline assembly:**

```c
static inline uint64_t
rdtsc(void)
{
#ifdef __x86_64__
	uint32_t low, high;
	__asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
	return ((uint64_t)high << 32) | low;
#else
	return 0;  /* Not available */
#endif
}
```

## Modern C Features

### C11/C17 Features to Use

**Atomics:** Use `<stdatomic.h>` for lock-free data structures

```c
#include <stdatomic.h>

typedef struct umem_tagged_ptr {
	atomic_uintptr_t tagged_value;
} umem_tagged_ptr_t;

/* Atomic operations */
atomic_store_explicit(&ptr->tagged_value, new_val, memory_order_release);
old_val = atomic_load_explicit(&ptr->tagged_value, memory_order_acquire);
```

**Static assertions:**

```c
_Static_assert(sizeof(umem_cache_t) <= 512,
    "umem_cache_t exceeds cache line boundary");
_Static_assert(UMEM_ALIGN >= sizeof(void *),
    "UMEM_ALIGN must be at least pointer size");
```

**Thread-local storage:**

```c
extern __thread int cached_cpu_hint;
__thread struct umem_rseq umem_rseq_area;
```

**Anonymous structs/unions (C11):**

```c
typedef union umem_tagged_ptr {
	atomic_uintptr_t tagged_value;
	struct {
		void *ptr;
		uint16_t tag;
		uint16_t reserved;
	};
} umem_tagged_ptr_t;
```

### Features to Avoid

**Variable-length arrays (VLA):** Avoid for portability and stack safety

```c
/* Avoid */
void function(int n) {
	char buffer[n];  /* VLA */
}

/* Use instead */
void function(int n) {
	char *buffer = alloca(n);  /* Or malloc for larger sizes */
}
```

**Designated initializers:** Use sparingly; may reduce portability to older compilers

```c
/* OK for internal structures */
static const umem_cache_t template = {
	.cache_bufsize = 64,
	.cache_align = 8,
};

/* Avoid in header files */
```

## Automated Formatting

### clang-format Configuration

The repository includes `.clang-format` with Solaris-compatible settings:

```yaml
BasedOnStyle: LLVM
IndentWidth: 8
UseTab: Always
TabWidth: 8
ColumnLimit: 80
BreakBeforeBraces: Linux
AllowShortIfStatementsOnASingleLine: false
AllowShortLoopsOnASingleLine: false
AllowShortFunctionsOnASingleLine: None
IndentCaseLabels: false
PointerAlignment: Right
```

### Running clang-format

**Format a single file:**
```bash
clang-format -i umem.c
```

**Format entire codebase:**
```bash
find . -name '*.c' -o -name '*.h' | xargs clang-format -i
```

**Pre-commit hook:** Install `prek` or use git hooks to enforce formatting

```bash
prek install
```

## Summary of Style Decisions

### Consistency Principles

1. **Solaris Heritage Preserved:**
   - K&R brace style
   - Tab indentation (8 spaces)
   - Traditional include guards
   - `return (value);` style in core files
   - C89-style variable declarations in umem.c/vmem.c

2. **Modern Improvements:**
   - C17 standard for new code
   - Block-scope variable declarations
   - Atomics from `<stdatomic.h>`
   - SIMD intrinsics for performance
   - Static assertions for compile-time checks

3. **Naming Conventions:**
   - Public API: `umem_*`, `vmem_*`
   - Internal: `_umem_*`, `_vmem_*`, or static
   - Types: `*_t` suffix
   - Macros: `UMEM_*`, `UMF_*`, `VM_*`

4. **Code Quality:**
   - Maximum 80-character lines
   - Maximum 100-line functions (guideline)
   - Always use braces for blocks
   - Check all allocation return values
   - Document public APIs thoroughly
   - Use `ASSERT()` for invariants, not user input

5. **Architecture Support:**
   - Separate files for architecture-specific code
   - Runtime feature detection where possible
   - Always provide fallback implementations

## Enforcement

### Code Review Checklist

Before submitting code for review:

- [ ] Code follows naming conventions
- [ ] No lines exceed 80 characters
- [ ] Functions are < 100 lines (or have good reason)
- [ ] All allocations are checked
- [ ] Public APIs are documented
- [ ] Architecture-specific code has fallbacks
- [ ] `clang-format` has been run
- [ ] No compiler warnings with `-Wall -Wextra`
- [ ] Code compiles with GCC and Clang

### Related Documentation

- [CONTRIBUTING.md](CONTRIBUTING.md) - Contribution guidelines and workflow
- [COMPILER_REQUIREMENTS.md](COMPILER_REQUIREMENTS.md) - Compiler versions and flags
- `.clang-format` - Automated formatting configuration

---

**Note:** This style guide is based on analysis of the existing libumem codebase and its Solaris heritage. When in doubt, match the style of surrounding code in the file you're editing.
