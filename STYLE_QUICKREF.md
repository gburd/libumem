# libumem Style Quick Reference

One-page reference for common style questions. See [CODING_STYLE.md](CODING_STYLE.md) for full details.

## The Essentials

```c
/*
 * Function: return type on separate line
 * Tabs for indentation (8-space width)
 * 80-character line limit
 */
static void
example_function(umem_cache_t *cp, size_t size, int flags)
{
	int i;
	void *ptr;

	/* C-style comments for blocks */
	if (condition) {
		statement;  // C++ style OK for inline
	}

	/* Block-scope declarations OK in new code */
	for (int j = 0; j < count; j++) {
		process(j);
	}

	/* Check all allocations */
	ptr = umem_alloc(size, flags);
	if (unlikely(ptr == NULL)) {
		return;
	}

	/* Both return styles acceptable */
	return (value);  // Solaris style
	return value;    // Standard C style
}
```

## Quick Decisions

| Question | Answer |
|----------|--------|
| C standard? | C17 (use C11/C17 features) |
| Tabs or spaces? | Tabs (8-space width) |
| Line length? | 80 characters max |
| Brace style? | Linux/K&R (control on same line, functions on new line) |
| Comments? | `/* */` primary, `//` OK for inline |
| Include guards? | `#ifndef _FILE_H` (not `#pragma once`) |
| Variable declarations? | Block scope in new code, function start in legacy |
| Return style? | `return (value);` or `return value;` - be consistent in file |

## Naming

```c
/* Public API */
void *umem_alloc(size_t size, int flags);
umem_cache_t *umem_cache_create(...);

/* Internal (non-static) */
void *_umem_cache_alloc(umem_cache_t *cp, int flags);

/* Static (no prefix) */
static void copy_pattern(uint64_t pattern, void *buf, size_t size);

/* Types */
typedef struct umem_cache umem_cache_t;

/* Macros and constants */
#define UMEM_DEFAULT    0x0000
#define UMF_AUDIT       0x00000001
```

## Common Patterns

### Error Checking
```c
ptr = umem_alloc(size, UMEM_DEFAULT);
if (ptr == NULL) {
	/* handle error */
	return (-1);
}

/* Hot path with unlikely() */
if (unlikely(ptr == NULL)) {
	handle_error();
}
```

### Assertions
```c
ASSERT(cp != NULL);           /* For invariants */
ASSERT(size > 0);
ASSERT(mutex_held(&lock));

/* NOT for user input */
if (user_size >= MAX_SIZE) {  /* Validate user input */
	return (NULL);
}
```

### Architecture-Specific
```c
#ifdef HAVE_AVX2
	/* AVX2 implementation */
#elif defined(HAVE_SSE2)
	/* SSE2 implementation */
#else
	/* Fallback */
#endif
```

### Modern C Features
```c
/* Atomics */
#include <stdatomic.h>
atomic_store_explicit(&ptr, val, memory_order_release);

/* Thread-local */
extern __thread int cached_cpu_hint;

/* Static assertions */
_Static_assert(sizeof(cache_t) <= 512, "Cache too large");
```

## Before You Commit

```bash
# Format your changes
clang-format -i modified_file.c

# Or format all modified files
git diff --name-only | grep '\.[ch]$' | xargs clang-format -i

# Check for warnings
make CFLAGS="-Wall -Wextra -Werror"
```

## When Editing Existing Files

**Match the existing style in that file:**

- **umem.c, vmem.c:** Solaris heritage style
  - Variables at function start
  - `return (value);` with parentheses
  - C-style comments only

- **umem_simd.h, umem_rseq.h, umem_numa.h:** Modern style
  - Block-scope declarations
  - Either return style
  - Inline comments OK

**Golden Rule:** Make your code look like the code around it.

## Quick Links

- Full guide: [CODING_STYLE.md](CODING_STYLE.md)
- Contributing: [CONTRIBUTING.md](CONTRIBUTING.md)
- Style audit: [CODE_STYLE_AUDIT.md](CODE_STYLE_AUDIT.md)
- Formatter config: [.clang-format](.clang-format)

---

**Remember:** Consistency within a file matters more than perfect adherence to any single standard.
