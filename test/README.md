# libumem Test Suite

All building and testing for this project runs on EC2, never on the
development host. See [AGENTS.md](../AGENTS.md) §1 and `scripts/ec2/`.

## What `make check` actually runs

`make check` is a **small smoke suite: 8 entries**, listed in `TESTS` in
`Makefile.am`:

```
umem_test  umem_test2  umem_test3  umem_ptc_fork_test  test/test_debug
test/debugger/test_inspect_e2e.sh  test/debugger/test_lldb_e2e.sh
test/stress/oracle_fast.sh
```

It is **not** the comprehensive suite. `test/test_main` — the
several-hundred-assertion unit suite — is deliberately *excluded*, along with
every property test, the concurrency oracle, and the lifecycle stress tests.
A green `make check` therefore proves much less than it looks like it does; a
passing test proves only what it actually executed.

The regressions added by the 2026-09-21 readiness work
(`test/unit/test_overflow_contracts`, `test/unit/test_hash_partition`,
`test/integration/test_fork_mt_load`,
`test/integration/test_update_thread_startup`,
`test/stress/interpose_regress.sh`, ...) are built by default and run
explicitly. Folding them into `TESTS` changes the suite's long-advertised
scope and runtime, which belongs in one coordinated change.

## Running things

```bash
make check                                              # 8-entry smoke suite
LD_LIBRARY_PATH=.libs test/.libs/test_main --no-fork     # comprehensive unit suite
LD_LIBRARY_PATH=.libs test/.libs/test_main --list        # what it contains
test/property/prop_alloc_free2                           # property tests
test/property/prop_cache
test/property/prop_fragmentation
test/integration/test_multithreaded
test/stress/stress_concurrency_oracle -d 10              # concurrency oracle
test/stress/stress_main -d 10                            # long-running stress
test/unit/test_hash_partition                            # weighted partitioning
make install-check                                       # installed prefix is usable
```

`make install-check` installs into a throwaway `DESTDIR` and compiles
`test/install/external_consumer.c` against nothing but that prefix. It exists
because installed headers used to `#include "config.h"`, which is never
installed — so an outside program could not compile against an install.

## Test organization

```
test/
├── unit/           ~36 unit sources; most are linked into test/test_main,
│                   a few are standalone binaries (test_overflow_contracts,
│                   test_aligned_contracts, test_hash_partition,
│                   test_rseq_fastpath, repro_cpu_node_oob)
├── property/       QuickCheck-style property tests (qc.c/h)
├── integration/    multithreading, signals, OOM, fork-under-load,
│                   realloc bootstrap, debug features, update-thread startup
├── stress/         concurrency oracle, long-running stress, targeted repros
│                   (rseq trailing store, naive reload race, calloc interpose
│                   race), plus the .sh drivers used by make check
├── bench/          benchmark harness — see bench/README.md
├── install/        installed-prefix external-consumer check
├── debugger/       gdb/lldb end-to-end scripts
├── munit.c/h       unit test framework
├── qc.c/h          property testing framework
└── tdigest.c/h     latency percentile estimation
```

Not every source in these directories is in the build. `Makefile.am` is the
authority on what is compiled; a file's presence here is not a claim that it
runs.

## Coverage

Last measured: **71.2% line coverage** of core sources (excluding the
vendored `sm.c`/`sm.h` sparsemap), **50.3%** across the whole repo including
`sm.c`, on 2026-09-06 at commit `ecd5fa0`. Method and reproduction:
[`../docs/results/2026-09-06-coverage-verification.md`](../docs/results/2026-09-06-coverage-verification.md).

This file previously claimed ">90% code coverage" and a ">90% Overall" target
"on track". Neither was measured; both contradicted the actual measurement in
README.md. There are no per-file coverage targets in force.

```bash
./configure --enable-coverage
make && make check
make coverage                       # lcov + genhtml -> test/coverage/
# or: ./scripts/generate-coverage.sh
```

Remember that coverage from `make check` alone reflects only those 8 entries.
Run `test_main` and the property/stress suites under the coverage build if you
want a number that means anything.

## Sanitizers and valgrind

```bash
./configure --enable-asan      # or --enable-ubsan, --enable-tsan
make clean && make && make check

valgrind --leak-check=full ./test/test_main
ASAN_OPTIONS=detect_leaks=1 ./test/test_main
```

## Writing tests

### Unit test (munit), linked into `test/test_main`

```c
#include <umem.h>
#include "../munit.h"

static MunitResult
test_my_feature(const MunitParameter params[], void* data)
{
    (void)params; (void)data;

    void *p = umem_alloc(64, UMEM_DEFAULT);
    munit_assert_not_null(p);
    memset(p, 0xAA, 64);
    munit_assert_uint8(*(uint8_t*)p, ==, 0xAA);
    umem_free(p, 64);

    return MUNIT_OK;
}

static MunitTest my_tests[] = {
    {"/my_feature", test_my_feature, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};

const MunitSuite my_suite = {
    "/my_suite", my_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};
```

Add the source to `test_test_main_SOURCES` in `Makefile.am`, and declare +
register the suite in `test/test_main.c`.

### Property test (qc)

```c
#include <umem.h>
#include "../qc.h"

static bool
prop_allocation_valid(qc_value_t v)
{
    size_t size = v.u64 % 8192 + 1;
    void *p = umem_alloc(size, UMEM_DEFAULT);
    if (p == NULL) return false;
    memset(p, 0xFF, size);          /* property: writable */
    umem_free(p, size);
    return true;
}

int main(void)
{
    qc_init();
    qc_check(prop_allocation_valid, QC_GEN_U64, 1000);
    return qc_report();
}
```

### Regression test for a fix

The project's evidence rule (AGENTS.md §6): a fix needs a check that **fails
before** the fix and **passes after**. Capture the failing run's output before
you fix anything — `docs/results/prefix-evidence/` holds these — and reference
it from the commit. A test written only after the fix proves the code compiles,
not that it addresses the defect.

## Guidelines

Do:

- test one concept per function, with a descriptive name
- clean up every allocation, including on the failure path
- distinguish PASS, SKIP, and FAIL honestly — a test that returns 0 because a
  prerequisite was missing must report SKIP, not success
- test failure cases, not just success

Don't:

- depend on test execution order or on global state between tests
- accept "any nonzero exit" as evidence that a detection feature worked
- ignore sanitizer or valgrind output

## CI

Forgejo Actions, `.forgejo/workflows/`: Linux x86_64 build × (normal, ASan,
UBSan, gcov), lint, and the tagged-release pipeline. That is the only
architecture with unattended coverage.

`aarch64-nightly.yml` exists and has been validated end-to-end by hand, but is
**not armed**: it needs repo secrets (`AWS_ACCESS_KEY_ID`,
`AWS_SECRET_ACCESS_KEY`, `EC2_SSH_PRIVATE_KEY`) that have never been added.
Do not count it as coverage.

There is no `.github/` directory in this repository; this file used to point
readers at `../.github/workflows/test.yml`, which has never existed here.

## Debugging a failure

```bash
./test/test_main --verbose
./test/test_main --filter="/umem_alloc/basic"
gdb --args ./test/test_main --filter="/failing_test"
valgrind --leak-check=full --track-origins=yes ./test/test_main
```

Preserve the original failure output before re-running anything (AGENTS.md
§5). A later pass does not explain an earlier failure, and re-running destroys
the only record.

## References

- **munit**: https://nemequ.github.io/munit/
- benchmark harness: [bench/README.md](bench/README.md)
- readiness workstream:
  [`../docs/plans/2026-09-21-production-readiness.md`](../docs/plans/2026-09-21-production-readiness.md)
