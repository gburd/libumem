/*
 * Allocator implementations for benchmarking.
 * umem is linked directly (no dlopen needed).
 */

#include "bench_framework.h"
#include <umem.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

/* ========== libc (system malloc) ========== */
static void* libc_alloc(size_t size) {
    return malloc(size);
}

static void* libc_calloc(size_t nmemb, size_t size) {
    return calloc(nmemb, size);
}

static void* libc_realloc(void *ptr, size_t size) {
    return realloc(ptr, size);
}

static void libc_free(void *ptr) {
    free(ptr);
}

allocator_ops_t allocator_libc = {
    .name = "libc",
    .alloc = libc_alloc,
    .calloc = libc_calloc,
    .realloc = libc_realloc,
    .free = libc_free,
    .cleanup = NULL
};

/* ========== libumem (dynamically loaded) ========== */
/* Size tracking header - prepended to each allocation */
typedef struct {
    size_t size;
    uint64_t magic;
} umem_size_header_t;

#define UMEM_SIZE_MAGIC 0xDEADBEEFCAFEBABEULL

/* Forward declarations */
static void* umem_alloc_wrapper(size_t size);
static void umem_free_wrapper(void *ptr);

static void* umem_alloc_wrapper(size_t size) {
    /* Allocate extra space for size header */
    size_t total_size = size + sizeof(umem_size_header_t);
    void *raw_ptr = umem_alloc(total_size, UMEM_DEFAULT);
    if (!raw_ptr) {
        return NULL;
    }

    /* Store size and magic in header */
    umem_size_header_t *header = (umem_size_header_t *)raw_ptr;
    header->size = total_size;
    header->magic = UMEM_SIZE_MAGIC;

    /* Return pointer after header */
    return (void *)(header + 1);
}

static void* umem_calloc_wrapper(size_t nmemb, size_t size) {
    size_t user_size = nmemb * size;
    size_t total_size = user_size + sizeof(umem_size_header_t);
    void *raw_ptr = umem_zalloc(total_size, UMEM_DEFAULT);
    if (!raw_ptr) {
        return NULL;
    }

    /* Store size and magic in header */
    umem_size_header_t *header = (umem_size_header_t *)raw_ptr;
    header->size = total_size;
    header->magic = UMEM_SIZE_MAGIC;

    /* Return pointer after header */
    return (void *)(header + 1);
}

static void* umem_realloc_wrapper(void *ptr, size_t new_size) {
    /* umem is linked directly — no init needed */

    /* Standard realloc behavior */
    if (!ptr) {
        return umem_alloc_wrapper(new_size);
    }
    if (new_size == 0) {
        umem_free_wrapper(ptr);
        return NULL;
    }

    /* Get old size from header */
    umem_size_header_t *old_header = ((umem_size_header_t *)ptr) - 1;
    if (old_header->magic != UMEM_SIZE_MAGIC) {
        fprintf(stderr, "ERROR: realloc called with invalid pointer\n");
        return NULL;
    }

    size_t old_user_size = old_header->size - sizeof(umem_size_header_t);

    /* Allocate new block */
    void *new_ptr = umem_alloc_wrapper(new_size);
    if (!new_ptr) {
        return NULL;
    }

    /* Copy old data (use smaller of old and new size) */
    size_t copy_size = (old_user_size < new_size) ? old_user_size : new_size;
    memcpy(new_ptr, ptr, copy_size);

    /* Free old block */
    umem_free_wrapper(ptr);

    return new_ptr;
}

static void umem_free_wrapper(void *ptr) {
    if (!ptr) return;

    /* Get header before user pointer */
    umem_size_header_t *header = ((umem_size_header_t *)ptr) - 1;

    /* Verify magic number */
    if (header->magic != UMEM_SIZE_MAGIC) {
        fprintf(stderr, "ERROR: umem_free called with invalid pointer %p (magic=0x%lx)\n",
                ptr, (unsigned long)header->magic);
        return;
    }

    /* Free with original size */
    umem_free((void *)header, header->size);
}

allocator_ops_t allocator_umem = {
    .name = "umem",
    .alloc = umem_alloc_wrapper,
    .calloc = umem_calloc_wrapper,
    .realloc = umem_realloc_wrapper,
    .free = umem_free_wrapper,
    .cleanup = NULL
};

/* ========== jemalloc (if available) ========== */
#ifdef HAVE_JEMALLOC
#include <jemalloc/jemalloc.h>

static void jemalloc_cleanup(void) {
    /* Force epoch advancement to update stats */
    uint64_t epoch = 1;
    size_t sz = sizeof(epoch);
    je_mallctl("epoch", &epoch, &sz, &epoch, sz);
}

allocator_ops_t allocator_jemalloc = {
    .name = "jemalloc",
    .alloc = je_malloc,
    .calloc = je_calloc,
    .realloc = je_realloc,
    .free = je_free,
    .cleanup = jemalloc_cleanup
};
#else
allocator_ops_t allocator_jemalloc = {
    .name = "jemalloc (not available)",
    .alloc = NULL,
    .calloc = NULL,
    .realloc = NULL,
    .free = NULL,
    .cleanup = NULL
};
#endif

/* ========== tcmalloc (if available) ========== */
#ifdef HAVE_TCMALLOC
#include <gperftools/tcmalloc.h>

allocator_ops_t allocator_tcmalloc = {
    .name = "tcmalloc",
    .alloc = tc_malloc,
    .calloc = tc_calloc,
    .realloc = tc_realloc,
    .free = tc_free,
    .cleanup = NULL
};
#else
allocator_ops_t allocator_tcmalloc = {
    .name = "tcmalloc (not available)",
    .alloc = NULL,
    .calloc = NULL,
    .realloc = NULL,
    .free = NULL,
    .cleanup = NULL
};
#endif

/* ========== mimalloc (if available) ========== */
#ifdef HAVE_MIMALLOC
#include <mimalloc.h>

allocator_ops_t allocator_mimalloc = {
    .name = "mimalloc",
    .alloc = mi_malloc,
    .calloc = mi_calloc,
    .realloc = mi_realloc,
    .free = mi_free,
    .cleanup = NULL
};
#else
allocator_ops_t allocator_mimalloc = {
    .name = "mimalloc (not available)",
    .alloc = NULL,
    .calloc = NULL,
    .realloc = NULL,
    .free = NULL,
    .cleanup = NULL
};
#endif
