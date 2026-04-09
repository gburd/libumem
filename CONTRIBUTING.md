# Contributing to libumem

Thank you for considering contributing to libumem!

## Getting Started

### Prerequisites

- C compiler (GCC 4.8+ or Clang 3.5+)
- autoconf, automake, libtool
- Python 3 (for test scripts)
- Optional: Nix (for reproducible builds)

### Building

```bash
./autogen.sh
./configure
make
make check
```

Or with Nix:

```bash
nix develop
make
```

## Code Standards

libumem maintains consistency with its Solaris heritage while incorporating modern C17 features. See [CODING_STYLE.md](CODING_STYLE.md) for comprehensive guidelines.

### Quick Reference

- **Standard:** C17 (ISO/IEC 9899:2018)
- **Line length:** 80 characters maximum
- **Indentation:** Tabs (8-space width)
- **Brace style:** Linux kernel style (K&R variant)
- **Comments:** C-style `/* */` (C++ `//` acceptable in new code)

### Naming Conventions

- **Public API:** `umem_*`, `vmem_*`
- **Internal functions:** `_umem_*`, `_vmem_*`, or static
- **Types:** `umem_*_t`, `vmem_*_t` suffix
- **Constants:** `UMEM_*`, `UMF_*`, `VM_*` prefix
- **Macros:** All caps with underscores

### Code Formatting

Run `clang-format` before committing:

```bash
# Format specific files
clang-format -i umem.c umem_impl.h

# Format all modified files
git diff --name-only --diff-filter=AM | grep '\.[ch]$' | xargs clang-format -i
```

The repository includes `.clang-format` with Solaris-compatible settings.

### Comments

- Doxygen-style for public APIs
- Implementation comments explain "why", not "what"
- No commented-out code (use version control)

## Testing Requirements

### Coverage Threshold

**New code must achieve >95% line coverage.**

Measure coverage:

```bash
./configure --enable-coverage
make clean
make check
genhtml coverage.info -o coverage
xdg-open coverage/index.html
```

### Test Organization

```
test/
  ├── test_main.c              # Unit tests
  ├── property/                # Property-based tests
  │   ├── prop_alloc_free2.c
  │   ├── prop_cache.c
  │   └── prop_fragmentation.c
  ├── integration/             # Integration tests
  │   ├── test_signals.c
  │   ├── test_oom.c
  │   └── test_multithreaded.c
  └── bench/                   # Benchmarks
      └── bench_main.c
```

### Adding Tests

1. Unit tests go in `test/test_main.c` or new files in `test/`
2. Property-based tests in `test/property/`
3. Benchmarks in `test/bench/`
4. Update `Makefile.am` to include new test programs

### Running Tests

```bash
# All tests
make check

# Specific test
./test/test_main

# With debugging
UMEM_DEBUG=default make check

# With sanitizers
./configure CFLAGS="-fsanitize=address,undefined"
make check
```

## Performance Testing

### Benchmarking

Before submitting performance-related changes:

```bash
cd test/bench
make
./bench_allocators.sh umem > before.txt

# Make your changes

make clean && make
./bench_allocators.sh umem > after.txt

# Compare results
diff -u before.txt after.txt
```

### Performance Regression Policy

Performance changes must:
1. Show <5% regression on any existing benchmark
2. Provide new benchmarks demonstrating improvement
3. Document tradeoffs in commit message

## Submitting Changes

### Commit Messages

Follow conventional commit format:

```
type(scope): brief description

Detailed explanation of the change.

Fixes: #123
```

Types:
- `feat`: New feature
- `fix`: Bug fix
- `perf`: Performance improvement
- `refactor`: Code refactoring
- `test`: Test additions/changes
- `docs`: Documentation updates
- `build`: Build system changes

Examples:
```
feat(ptc): add aarch64 PTC support

Implement Per-Thread Cache assembly generation for aarch64.
Provides lock-free fast path for allocations ≤256 bytes.

Performance: 2x throughput improvement for small allocations
on 64-core ARM server.

Fixes: #45
```

### Pull Request Process

1. **Create feature branch**: `git checkout -b feature/my-feature`

2. **Make changes**:
   - Write tests first (TDD)
   - Implement feature
   - Ensure tests pass: `make check`
   - Check coverage: `make coverage`
   - Run linters if available

3. **Commit changes**:
   - One logical change per commit
   - Write clear commit messages
   - Reference issue numbers

4. **Push and create PR**:
   ```bash
   git push origin feature/my-feature
   # Create PR on GitHub
   ```

5. **PR description should include**:
   - What: What does this change do?
   - Why: Why is this change needed?
   - How: How does it work (for complex changes)?
   - Testing: How was this tested?
   - Performance: Any performance implications?

6. **Address review feedback**:
   - Make requested changes
   - Push new commits (no force-push during review)
   - Respond to comments

7. **Merge**: Maintainer will merge when approved

### PR Checklist

- [ ] Tests added/updated and passing
- [ ] Coverage >95% for new code
- [ ] Documentation updated (man pages, README, etc.)
- [ ] No performance regression (<5% on benchmarks)
- [ ] Commit messages follow guidelines
- [ ] Code follows style guidelines (see [CODING_STYLE.md](CODING_STYLE.md))
- [ ] `clang-format` has been run on modified files
- [ ] No compiler warnings with `-Wall -Wextra`
- [ ] Works on all supported platforms (x86_64, i386)

## Architecture Porting

Porting to a new architecture? See [docs/PORTING.md](docs/PORTING.md) for detailed guide.

Quick checklist:
1. Create `arch/umem_genasm.c` (can start with template)
2. Update `configure.ac` for new architecture
3. Implement TLS access and atomic operations
4. Write architecture-specific tests
5. Run full test suite on target hardware

## Documentation

### Man Pages

Man pages use troff format:
- `umem_*.3` - API documentation
- `umem_*.7` - Overview/guide documentation

Update man pages when changing public APIs.

### Markdown Documentation

- `README.md` - Project overview
- `docs/*.md` - Technical documentation
- `examples/README.md` - Example documentation

## Code Review Guidelines

### For Contributors

- Keep PRs focused (one feature/fix per PR)
- Respond to feedback promptly
- Be open to suggestions
- Test thoroughly before submitting

### For Reviewers

- Be respectful and constructive
- Focus on code, not person
- Explain reasoning for requested changes
- Approve when requirements met

## Bug Reports

### Before Reporting

1. Search existing issues
2. Test with latest version
3. Reproduce with minimal example

### Bug Report Template

```markdown
**Describe the bug**
Clear description of the issue.

**To Reproduce**
Steps to reproduce:
1. Compile with: ...
2. Run with: ...
3. See error: ...

**Expected behavior**
What should happen.

**Environment**
- OS: [e.g., Ubuntu 22.04]
- Architecture: [e.g., x86_64]
- Compiler: [e.g., GCC 11.3]
- libumem version: [e.g., commit hash or release]

**Additional context**
- Core dump? Valgrind output? Debug logs?
```

## Feature Requests

Feature requests welcome! Please include:
- Use case: Why is this needed?
- Proposed API: How should it work?
- Alternatives: Other approaches considered?
- Willingness to implement: Can you contribute?

## Questions?

- Open a discussion on GitHub
- Check existing documentation
- Review examples in `examples/`

## License

By contributing, you agree that your contributions will be licensed under the CDDL 1.0 license.
