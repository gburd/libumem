# Nix Flake Usage Guide for libumem

This document describes how to use the Nix Flake for building and developing libumem.

## Quick Start

### Building libumem

```bash
# Build the package
nix build

# The result will be in the 'result' symlink
ls -la result/lib/
# libumem.so           - Core allocator
# libumem_malloc.so    - malloc replacement
```

### Development Shell

```bash
# Enter development environment
nix develop

# Inside the shell, you have access to:
# - autoconf, automake, libtool (build tools)
# - doxygen, graphviz (documentation)
# - gdb, valgrind (debugging)
# - clang-tools, cppcheck (code quality)

# Build from source
./autogen.sh
./configure
make
make check
```

### Running Tests

```bash
# Tests run automatically during build (doCheck = true)
nix build

# Run integration tests
nix build .#checks.x86_64-linux.integration
```

## Outputs

The flake produces three outputs:

### 1. `out` - Runtime Libraries

Location: `/nix/store/...-libumem-1.0.2/`

Contents:
- `lib/libumem.so` - Core allocator (umem_alloc/umem_free API)
- `lib/libumem_malloc.so` - malloc/free replacement (use with LD_PRELOAD)
- `share/man/man3/` - Man pages (umem_alloc.3, umem_cache_create.3, umem_debug.3)

### 2. `dev` - Development Headers

Location: `/nix/store/...-libumem-1.0.2-dev/`

Contents:
- `include/umem.h` - Main header file
- `include/sys/vmem.h` - Virtual memory header
- `lib/pkgconfig/libumem.pc` - pkg-config file for libumem
- `lib/pkgconfig/libumem-malloc.pc` - pkg-config file for libumem_malloc

### 3. `doc` - Documentation

Location: `/nix/store/...-libumem-1.0.2-doc/`

Contents:
- `share/doc/libumem/html/` - Doxygen-generated API documentation

## Using libumem in Other Nix Projects

### Method 1: As a Flake Input

Add to your `flake.nix`:

```nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    libumem.url = "git+https://codeberg.org/gregburd/libumem";
  };

  outputs = { self, nixpkgs, libumem }: {
    # Your package definition
    packages.x86_64-linux.myapp = pkgs.stdenv.mkDerivation {
      name = "myapp";
      buildInputs = [ libumem.packages.x86_64-linux.default ];
      # ...
    };
  };
}
```

### Method 2: Using pkg-config

```bash
# In your build environment
export PKG_CONFIG_PATH="$(nix build --no-link --print-out-paths .#libumem)/lib/pkgconfig:$PKG_CONFIG_PATH"

# Compile your program
gcc myapp.c $(pkg-config --cflags --libs libumem) -o myapp
```

### Method 3: Direct Linking

```bash
# Get the store paths
LIBUMEM_DEV=$(nix build --no-link --print-out-paths .#libumem.dev)
LIBUMEM_OUT=$(nix build --no-link --print-out-paths .#libumem)

# Compile with explicit paths
gcc myapp.c \
  -I$LIBUMEM_DEV/include \
  -L$LIBUMEM_OUT/lib \
  -lumem \
  -o myapp

# Run with library path
LD_LIBRARY_PATH=$LIBUMEM_OUT/lib ./myapp
```

## Example Program

```c
#include <umem.h>
#include <stdio.h>

int main() {
    // Basic allocation
    void *p = umem_alloc(100, UMEM_DEFAULT);
    if (!p) return 1;
    umem_free(p, 100);

    // Cache allocation
    umem_cache_t *cache = umem_cache_create(
        "my_cache", 64, 8, NULL, NULL, NULL, NULL, NULL, 0);
    void *obj = umem_cache_alloc(cache, UMEM_DEFAULT);
    umem_cache_free(cache, obj);
    umem_cache_destroy(cache);

    return 0;
}
```

Compile and run:
```bash
nix develop -c bash -c '
  gcc example.c -I$(nix build --no-link --print-out-paths .#libumem.dev)/include \
                 -L$(nix build --no-link --print-out-paths .#libumem)/lib \
                 -lumem -o example
  LD_LIBRARY_PATH=$(nix build --no-link --print-out-paths .#libumem)/lib ./example
'
```

## Using libumem_malloc (malloc replacement)

```bash
# Preload libumem_malloc to replace malloc/free
LD_PRELOAD=$(nix build --no-link --print-out-paths .#libumem)/lib/libumem_malloc.so ./your_program

# This makes all malloc/free calls use libumem instead
```

## Architecture Support

- **x86_64/i386**: Full PTC (Per-Thread Cache) optimization with runtime code generation
- **Other architectures**: Generic allocator without PTC (still fully functional)

PTC provides lock-free fast paths for allocation on supported architectures.

## Flake Commands Reference

```bash
# Build the package
nix build

# Enter development shell
nix develop

# Run all checks
nix flake check

# Show flake metadata
nix flake metadata

# Update flake inputs
nix flake update

# Show package info
nix path-info --derivation .#libumem

# Build specific output
nix build .#libumem.dev    # Development headers
nix build .#libumem.doc    # Documentation only

# Run integration tests
nix build .#checks.x86_64-linux.integration
```

## Continuous Integration

The flake is designed for CI/CD integration:

```yaml
# Example GitHub Actions workflow
- name: Build libumem
  run: nix build

- name: Run tests
  run: nix flake check

- name: Build documentation
  run: nix build .#libumem.doc
```

## Troubleshooting

### Tests fail with "PTC disabled"

This is normal on non-x86 architectures or systems with hardened kernels that restrict PROT_EXEC mmap. The library will still work, just without PTC optimization.

### "command not found" in development shell

Make sure you're inside the development shell:
```bash
nix develop
```

### Build fails with missing dependencies

Update the flake inputs:
```bash
nix flake update
nix build
```

### Can't find libraries at runtime

Set LD_LIBRARY_PATH:
```bash
export LD_LIBRARY_PATH=$(nix build --no-link --print-out-paths .#libumem)/lib:$LD_LIBRARY_PATH
```

Or use rpath during compilation:
```bash
gcc myapp.c -I$DEV/include -L$OUT/lib -lumem -Wl,-rpath,$OUT/lib -o myapp
```

## License

libumem is licensed under the CDDL (Common Development and Distribution License).
