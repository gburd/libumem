# libumem Examples

Example programs demonstrating libumem features.

## palloc_integration.c

Demonstrates how to integrate PostgreSQL's palloc memory management with umem hooks for tracking and debugging.

**Features shown:**
- Hook registration
- Allocation tracking
- Statistics collection
- Custom allocator integration

**Build:**
```bash
gcc -o palloc_example palloc_integration.c -L.. -lumem -I..
```

**Run:**
```bash
LD_LIBRARY_PATH=.. ./palloc_example
```

**Expected output:**
```
PostgreSQL palloc integration with umem hooks
==============================================

palloc hook registered

Allocating memory with palloc...
  palloc(64) = 0x...
  palloc(128) = 0x...
  palloc(256) = 0x...

Statistics after allocations:
Hook: palloc
  Allocations:     3
  Frees:           0
  Reallocs:        0
  Bytes allocated: 448
  Bytes freed:     0
  Current bytes:   448
  Peak bytes:      448

Freeing memory...

Reallocating ptr3 from 256 to 512 bytes...
  repalloc = 0x...

Final statistics:
Hook: palloc
  Allocations:     4
  Frees:           2
  Reallocs:        1
  Bytes allocated: 960
  Bytes freed:     192
  Current bytes:   512
  Peak bytes:      512
...
```

## Integration Pattern

To integrate your application's allocator with umem hooks:

### 1. Define Hook Structure

```c
static umem_hook_t my_hook = {
    .hook_name = "my_allocator",
    .hook_alloc = my_alloc_impl,
    .hook_free = my_free_impl,
    .hook_realloc = my_realloc_impl,  /* Optional */
    .hook_arg = &my_context
};
```

### 2. Implement Hook Functions

```c
static void *my_alloc_impl(size_t size, void *arg) {
    MyContext *ctx = (MyContext *)arg;

    /* Your allocation logic */
    void *ptr = your_allocator(size, ctx);

    return ptr;
}

static void my_free_impl(void *ptr, void *arg) {
    MyContext *ctx = (MyContext *)arg;

    /* Your free logic */
    your_deallocator(ptr, ctx);
}
```

### 3. Register Hook

```c
if (umem_hook_register(&my_hook) != 0) {
    /* Handle error */
}
```

### 4. Use Tracking Functions

```c
void *my_malloc(size_t size) {
    return umem_hook_track_alloc(&my_hook, size);
}

void my_free(void *ptr, size_t size) {
    umem_hook_track_free(&my_hook, ptr, size);
}
```

### 5. View Statistics

```c
/* Dump stats for your hook */
umem_hook_dump_one(stdout, &my_hook);

/* Or dump all hooks */
umem_hook_dump(stdout);
```

### 6. Cleanup

```c
umem_hook_unregister(&my_hook);
```

## Use Cases

### Database Memory Management

Track allocations across different memory contexts (like PostgreSQL's MemoryContext system).

### Scripting Language Integration

Integrate Lua, Python, or other language allocators for unified tracking.

### Application-Specific Pools

Track allocations from custom memory pools or arenas.

### Multi-Tenant Systems

Track memory usage per tenant or user.

## Debugging with Hooks

Hooks integrate with umem's debugging features:

```bash
# Enable umem debugging
UMEM_DEBUG=audit ./my_app

# In debugger
(gdb) source umem_gdb.py
(gdb) umem-stats
# Shows both umem caches and registered hooks
```

## Performance Considerations

Hook tracking has minimal overhead:
- Simple counter increments (atomic if needed)
- No allocation/free overhead
- Optional - can be disabled in production

For production use:
```c
#ifdef DEBUG
    return umem_hook_track_alloc(&my_hook, size);
#else
    return my_alloc_impl(size, my_context);
#endif
```

## See Also

- `umem_hooks.h` - Hook API reference
- `docs/DEBUGGING.md` - Debugging guide
- PostgreSQL source - Real palloc implementation
