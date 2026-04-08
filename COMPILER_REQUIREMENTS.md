# Compiler Requirements

## Standard

libumem targets **C17** (ISO/IEC 9899:2018), matching PostgreSQL v16+ requirements.

Falls back to **C11** (ISO/IEC 9899:2011) if C17 is not available.

## Minimum Compiler Versions

- **GCC**: 9.0 or later
- **Clang**: 10.0 or later
- **MSVC**: 19.28 (VS 2019 16.8) or later (for Windows builds)

## Required Features

- C11/C17 atomics (`<stdatomic.h>`)
- Thread-local storage (`__thread` keyword or `_Thread_local`)
- `<stdbool.h>`, `<stdint.h>`, `<stddef.h>`

## Code Formatting

libumem uses **tab indentation** (width 8) following Solaris/OpenSolaris conventions.

### Automated Formatting

Use `clang-format` for consistent formatting:

```bash
# Format a single file
clang-format -i umem.c

# Format all C files
find . -name '*.c' -o -name '*.h' | xargs clang-format -i
```

Configuration is in `.clang-format` at the repository root.

### Style Guidelines

- **Indentation**: Tabs (width 8)
- **Bracing**: K&R style (Linux kernel style)
- **Line length**: 80 columns (soft limit)
- **Pointers**: Right-aligned (`int *ptr`)
- **Function length**: ≤100 lines preferred
- **Complexity**: Cyclomatic complexity ≤8 preferred

## Build Flags

The configure script automatically adds `-std=c17` or `-std=c11` to `CFLAGS`.

### Additional Recommended Flags

```bash
# Development build
./configure CFLAGS="-std=c17 -g -O2 -Wall -Wextra"

# Production build
./configure CFLAGS="-std=c17 -O3 -DNDEBUG -flto"

# Debug build with sanitizers
./configure CFLAGS="-std=c17 -g -O0 -fsanitize=address,undefined"
```

## Testing Compiler Support

```bash
# Test if your compiler supports C17
echo 'int main() { return 0; }' | gcc -std=c17 -x c - -o /dev/null

# Test with clang
echo 'int main() { return 0; }' | clang -std=c17 -x c - -o /dev/null
```

## Platform-Specific Notes

### Linux
- GCC 9+ and Clang 10+ fully support C17
- Ubuntu 20.04+ and Fedora 31+ have compatible compilers by default

### macOS
- Xcode 12+ (Clang 12+) fully supports C17
- May need `xcode-select --install` to ensure command-line tools are installed

### FreeBSD
- Clang 10+ included in FreeBSD 13+
- GCC available via ports/pkg

### Solaris/Illumos
- Oracle Solaris Studio 12.6+ supports C11
- GCC 9+ via OpenCSW or pkgsrc for C17 support

### Windows
- MSVC 2019 16.8+ supports C17
- MinGW-w64 with GCC 9+ or Clang 10+

## Verification

After building, verify your configuration:

```bash
# Check what standard was used
grep "^CFLAGS" config.log | head -1

# Verify atomics work
./umem_test
```
