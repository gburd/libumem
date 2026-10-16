# libumem fuzz harness

`fuzz_malloc.c` drives the public allocation API --
`malloc`/`free`/`realloc`/`calloc`/`posix_memalign` -- the way a program
running under `LD_PRELOAD=libumem_malloc.so` would. The harness links against
no internal libumem header: it calls the standard libc names, so whichever
allocator is loaded is the one under test.

The fuzzer input is read as a 4-byte-per-step opcode stream (op, slot, size-lo,
size-hi), driving random sizes, alignments, and alloc/free/realloc
interleavings against a bounded 64-slot live set. The live set is drained at
the end of every program so a leak-sanitizing build sees a clean exit for a
well-formed input.

## Build + run

Two ways to build it, depending on whether a fuzzing engine is available.

### libFuzzer (clang)

```sh
clang -g -O1 -fsanitize=fuzzer,address \
    -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION \
    test/fuzz/fuzz_malloc.c -o fuzz_malloc
# Fuzz the system allocator:
./fuzz_malloc -max_total_time=60 test/fuzz/corpus
# Fuzz the libumem interposer:
LD_PRELOAD=$PWD/.libs/libumem_malloc.so \
    ./fuzz_malloc -max_total_time=60 test/fuzz/corpus
```

### AFL (afl-clang-fast)

```sh
afl-clang-fast test/fuzz/fuzz_malloc.c -o fuzz_malloc
afl-fuzz -i test/fuzz/corpus -o findings -- ./fuzz_malloc
```

### Standalone replay (no engine; what `make check` / CI runs)

Built with a plain compiler the file has a `main()` that replays each file
named on argv (or stdin), so the harness can be exercised over the seed corpus
with no libFuzzer/AFL runtime linked:

```sh
cc test/fuzz/fuzz_malloc.c -o fuzz_malloc
LD_PRELOAD=$PWD/.libs/libumem_malloc.so ./fuzz_malloc test/fuzz/corpus/*
```

`corpus/` holds a few hand-written seeds covering malloc/realloc/free, calloc +
posix_memalign, and a churn pattern. CI runs the harness bounded (a short
libFuzzer `-max_total_time`), not open-ended; open-ended fuzzing is a manual /
dispatch activity.
