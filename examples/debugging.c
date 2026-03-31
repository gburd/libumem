/*
 * Debugging examples for libumem
 *
 * Demonstrates how to use libumem's debugging features to detect memory issues.
 *
 * Compile and run with:
 *   gcc -o debugging debugging.c -lumem
 *   UMEM_DEBUG=default ./debugging
 */

#include <stdio.h>
#include <string.h>
#include <umem.h>

/* Example 1: Detecting use-after-free */
void example_use_after_free(void) {
    printf("\n=== Example 1: Use-After-Free Detection ===\n");
    printf("Run with: UMEM_DEBUG=guards ./debugging\n\n");

    void *ptr = umem_alloc(64, UMEM_DEFAULT);
    printf("Allocated 64 bytes at %p\n", ptr);

    memset(ptr, 0x42, 64);
    printf("Filled with pattern 0x42\n");

    umem_free(ptr, 64);
    printf("Freed memory (now filled with 0xdeadbeef)\n");

    /* UNCOMMENT TO TRIGGER ERROR:
     * In debug mode, this will abort because freed memory
     * is filled with 0xdeadbeef pattern
     */
    // memset(ptr, 0x99, 64);  /* Use after free! */
    // printf("ERROR: Used freed memory (should abort in debug mode)\n");

    printf("Good: Did not use freed memory\n");
}

/* Example 2: Detecting buffer overruns */
void example_buffer_overrun(void) {
    printf("\n=== Example 2: Buffer Overrun Detection ===\n");
    printf("Run with: UMEM_DEBUG=guards ./debugging\n\n");

    void *ptr = umem_alloc(64, UMEM_DEFAULT);
    printf("Allocated 64 bytes at %p\n", ptr);

    /* Write within bounds - OK */
    memset(ptr, 'A', 64);
    printf("Wrote 64 bytes - OK\n");

    /* UNCOMMENT TO TRIGGER ERROR:
     * This will corrupt the redzone and abort
     */
    // ((char *)ptr)[64] = 'X';  /* Write past end! */
    // umem_free(ptr, 64);       /* Redzone corruption detected */
    // printf("ERROR: Should have aborted due to redzone corruption\n");

    umem_free(ptr, 64);
    printf("Good: No buffer overrun detected\n");
}

/* Example 3: Detecting memory leaks */
void example_memory_leak(void) {
    printf("\n=== Example 3: Memory Leak Detection ===\n");
    printf("Run with: UMEM_DEBUG=audit UMEM_LOGGING=transaction ./debugging\n");
    printf("Then attach debugger and run umem-status\n\n");

    /* Allocate some memory */
    void *ptr1 = umem_alloc(128, UMEM_DEFAULT);
    void *ptr2 = umem_alloc(256, UMEM_DEFAULT);
    void *ptr3 = umem_alloc(512, UMEM_DEFAULT);

    printf("Allocated 3 buffers: %p, %p, %p\n", ptr1, ptr2, ptr3);

    /* Free two of them */
    umem_free(ptr1, 128);
    umem_free(ptr3, 512);
    printf("Freed ptr1 and ptr3\n");

    /* UNCOMMENT TO CREATE LEAK:
     * ptr2 is not freed - this is a leak
     */
    // printf("Leaking ptr2 (not freed)\n");
    // printf("Attach GDB and use umem-status to see leak with stack trace\n");
    // return;

    /* Good: Free all allocations */
    umem_free(ptr2, 256);
    printf("Good: Freed all allocations (no leak)\n");
}

/* Example 4: Using cache debugging */
void example_cache_debugging(void) {
    printf("\n=== Example 4: Cache Debugging ===\n");
    printf("Run with: UMEM_DEBUG=default ./debugging\n\n");

    umem_cache_t *cache = umem_cache_create(
        "debug_cache",
        128,
        0,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        0  /* Enable debug features */
    );

    printf("Created cache: debug_cache\n");

    /* Allocate and free multiple times */
    for (int i = 0; i < 5; i++) {
        void *obj = umem_cache_alloc(cache, UMEM_DEFAULT);
        printf("Allocated object %d at %p\n", i, obj);

        memset(obj, 'A' + i, 128);

        umem_cache_free(cache, obj);
        printf("Freed object %d\n", i);
    }

    umem_cache_destroy(cache);
    printf("Destroyed cache\n");
    printf("Attach debugger and use umem-cache to inspect cache state\n");
}

/* Example 5: Uninitialized memory detection */
void example_uninitialized_memory(void) {
    printf("\n=== Example 5: Uninitialized Memory Detection ===\n");
    printf("Run with: UMEM_DEBUG=guards ./debugging\n\n");

    /* umem_alloc() does NOT initialize memory */
    void *ptr = umem_alloc(64, UMEM_DEFAULT);
    printf("Allocated 64 bytes at %p (uninitialized)\n", ptr);

    /* In debug mode, new allocations are filled with 0xbaddcafe */
    unsigned char *bytes = (unsigned char *)ptr;
    printf("First 4 bytes: %02x %02x %02x %02x\n",
           bytes[0], bytes[1], bytes[2], bytes[3]);
    printf("If you see 0xbaddcafe pattern, memory is uninitialized\n");

    umem_free(ptr, 64);

    /* umem_zalloc() DOES initialize to zero */
    ptr = umem_zalloc(64, UMEM_DEFAULT);
    printf("\nAllocated 64 bytes with umem_zalloc (zeroed)\n");

    bytes = (unsigned char *)ptr;
    int all_zero = 1;
    for (int i = 0; i < 64; i++) {
        if (bytes[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    printf("Memory is%s zeroed\n", all_zero ? "" : " NOT");

    umem_free(ptr, 64);
    printf("Good: Used umem_zalloc to avoid uninitialized memory\n");
}

int main(void) {
    printf("libumem Debugging Examples\n");
    printf("==========================\n");
    printf("\nThese examples demonstrate libumem's debugging features.\n");
    printf("Some errors are commented out to prevent crashes.\n");
    printf("Uncomment them to see debug mode in action.\n");
    printf("\nIMPORTANT: Set UMEM_DEBUG environment variable:\n");
    printf("  UMEM_DEBUG=guards      - Pattern fill and redzone checking\n");
    printf("  UMEM_DEBUG=audit       - Stack traces on alloc/free\n");
    printf("  UMEM_DEBUG=default     - All features (audit,contents,guards)\n");
    printf("\nWith logging:\n");
    printf("  UMEM_LOGGING=transaction=1m  - Log recent allocations\n");

    example_use_after_free();
    example_buffer_overrun();
    example_memory_leak();
    example_cache_debugging();
    example_uninitialized_memory();

    printf("\n=== All Examples Completed ===\n");
    printf("\nTo see debug features in action:\n");
    printf("1. Set UMEM_DEBUG=default\n");
    printf("2. Uncomment error-triggering code\n");
    printf("3. Run and observe abort with error message\n");
    printf("4. Attach debugger and use umem-status for details\n");
    printf("\nFor memory leak detection:\n");
    printf("1. Set UMEM_DEBUG=audit UMEM_LOGGING=transaction=1m\n");
    printf("2. Run program\n");
    printf("3. Attach GDB: gdb -p $(pidof debugging)\n");
    printf("4. Load extension: source tools/gdb/umem_gdb.py\n");
    printf("5. Check status: umem-status\n");

    return 0;
}
