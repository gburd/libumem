/*
 * Allocator implementations for benchmarking
 */

#include "bench_framework.h"
#include <stdlib.h>
#include <string.h>
#include <umem.h>

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

/* ========== libumem ========== */
static void* umem_alloc_wrapper(size_t size) {
    return umem_alloc(size, UMEM_DEFAULT);
}

static void* umem_calloc_wrapper(size_t nmemb, size_t size) {
    return umem_zalloc(nmemb * size, UMEM_DEFAULT);
}

static void* umem_realloc_wrapper(void *ptr, size_t size) {
    /* umem doesn't have realloc, so implement it */
    if (!ptr) {
        return umem_alloc(size, UMEM_DEFAULT);
    }
    if (size == 0) {
        umem_free(ptr, 0);
        return NULL;
    }
    /* Note: We don't know the old size, so this is inefficient */
    void *new_ptr = umem_alloc(size, UMEM_DEFAULT);
    if (new_ptr && ptr) {
        /* This is a simplified realloc - production code would need size tracking */
        memcpy(new_ptr, ptr, size);  /* Conservative copy */
        umem_free(ptr, 0);
    }
    return new_ptr;
}

static void umem_free_wrapper(void *ptr) {
    umem_free(ptr, 0);
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
