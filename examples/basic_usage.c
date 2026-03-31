/*
 * Basic usage examples for libumem
 *
 * Demonstrates simple allocation, zero-allocation, and aligned allocation.
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <umem.h>

int main(void) {
    printf("libumem Basic Usage Examples\n");
    printf("=============================\n\n");

    /* Example 1: Simple allocation */
    printf("1. Simple allocation:\n");
    void *ptr1 = umem_alloc(1024, UMEM_DEFAULT);
    if (ptr1 == NULL) {
        fprintf(stderr, "   Allocation failed\n");
        return 1;
    }
    printf("   Allocated 1024 bytes at %p\n", ptr1);

    /* Use the memory */
    memset(ptr1, 0xAA, 1024);
    printf("   Filled with pattern 0xAA\n");

    /* Free the memory */
    umem_free(ptr1, 1024);
    printf("   Freed memory\n\n");

    /* Example 2: Zero-initialized allocation */
    printf("2. Zero-initialized allocation:\n");
    void *ptr2 = umem_zalloc(2048, UMEM_DEFAULT);
    if (ptr2 == NULL) {
        fprintf(stderr, "   Allocation failed\n");
        return 1;
    }
    printf("   Allocated 2048 bytes at %p\n", ptr2);

    /* Verify it's zeroed */
    unsigned char *bytes = (unsigned char *)ptr2;
    int all_zero = 1;
    for (int i = 0; i < 2048; i++) {
        if (bytes[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    printf("   Memory is%s zeroed\n", all_zero ? "" : " NOT");
    assert(all_zero);

    umem_free(ptr2, 2048);
    printf("   Freed memory\n\n");

    /* Example 3: Aligned allocation */
    printf("3. Aligned allocation (64-byte alignment):\n");
    void *ptr3 = umem_alloc_align(4096, 64, UMEM_DEFAULT);
    if (ptr3 == NULL) {
        fprintf(stderr, "   Allocation failed\n");
        return 1;
    }
    printf("   Allocated 4096 bytes at %p\n", ptr3);

    /* Verify alignment */
    uintptr_t addr = (uintptr_t)ptr3;
    if ((addr & 63) == 0) {
        printf("   Correctly aligned to 64 bytes\n");
    } else {
        printf("   ERROR: Not properly aligned!\n");
        return 1;
    }

    umem_free_align(ptr3, 4096);
    printf("   Freed memory\n\n");

    /* Example 4: Multiple allocations */
    printf("4. Multiple small allocations:\n");
    void *ptrs[10];
    for (int i = 0; i < 10; i++) {
        ptrs[i] = umem_alloc(64, UMEM_DEFAULT);
        if (ptrs[i] == NULL) {
            fprintf(stderr, "   Allocation %d failed\n", i);
            return 1;
        }
        sprintf((char *)ptrs[i], "Buffer %d", i);
    }
    printf("   Allocated 10 buffers of 64 bytes each\n");

    /* Use them */
    for (int i = 0; i < 10; i++) {
        printf("   Buffer %d: %s\n", i, (char *)ptrs[i]);
    }

    /* Free them */
    for (int i = 0; i < 10; i++) {
        umem_free(ptrs[i], 64);
    }
    printf("   Freed all buffers\n\n");

    /* Example 5: UMEM_NOFAIL (careful with this!) */
    printf("5. UMEM_NOFAIL allocation:\n");
    printf("   Note: This will exit program on allocation failure\n");

    /* Set up a nofail callback */
    static int nofail_callback(void) {
        fprintf(stderr, "   NOFAIL callback invoked!\n");
        fprintf(stderr, "   Out of memory - exiting\n");
        return UMEM_CALLBACK_EXIT(1);
    }
    umem_nofail_callback(nofail_callback);

    void *ptr5 = umem_alloc(512, UMEM_NOFAIL);
    /* This will never be NULL - program exits if allocation fails */
    printf("   Allocated 512 bytes at %p (guaranteed non-NULL)\n", ptr5);
    umem_free(ptr5, 512);
    printf("   Freed memory\n\n");

    printf("All examples completed successfully!\n");
    return 0;
}
