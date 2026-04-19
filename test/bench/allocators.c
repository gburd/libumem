/*
 * Allocator implementations for benchmarking
 * Note: Uses dlopen to load allocators dynamically to avoid malloc override
 */

#include "bench_framework.h"
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdio.h>

#define UMEM_DEFAULT 0

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
static void *umem_handle = NULL;
static void* (*umem_alloc_fn)(size_t, int) = NULL;
static void* (*umem_zalloc_fn)(size_t, int) = NULL;
static void (*umem_free_fn)(void*, size_t) = NULL;
static pthread_once_t umem_once = PTHREAD_ONCE_INIT;
static int umem_init_result = 0;

/* Size tracking header - prepended to each allocation */
typedef struct {
    size_t size;
    uint64_t magic;
} umem_size_header_t;

#define UMEM_SIZE_MAGIC 0xDEADBEEFCAFEBABEULL

/* Forward declarations */
static void* umem_alloc_wrapper(size_t size);
static void umem_free_wrapper(void *ptr);

static void umem_do_init(void) {
    const char *paths[] = {
        "./.libs/libumem.so.0",
        ".libs/libumem.so.0",
        "../.libs/libumem.so.0",
        "libumem.so.0",
        "libumem.so",
        NULL
    };

    for (int i = 0; paths[i] != NULL; i++) {
        umem_handle = dlopen(paths[i], RTLD_LAZY | RTLD_GLOBAL);
        if (umem_handle) break;
    }

    if (!umem_handle) {
        fprintf(stderr, "Failed to load libumem: %s\n", dlerror());
        return;
    }

    umem_alloc_fn = dlsym(umem_handle, "umem_alloc");
    umem_zalloc_fn = dlsym(umem_handle, "umem_zalloc");
    umem_free_fn = dlsym(umem_handle, "umem_free");

    if (!umem_alloc_fn || !umem_zalloc_fn || !umem_free_fn) {
        fprintf(stderr, "Failed to load umem symbols: %s\n", dlerror());
        dlclose(umem_handle);
        umem_handle = NULL;
        return;
    }

    umem_init_result = 1;
}

static int umem_init_once(void) {
    pthread_once(&umem_once, umem_do_init);
    return umem_init_result;

    return 1;
}

static void* umem_alloc_wrapper(size_t size) {
    if (!umem_init_once()) {
        return NULL;
    }

    /* Allocate extra space for size header */
    size_t total_size = size + sizeof(umem_size_header_t);
    void *raw_ptr = umem_alloc_fn(total_size, UMEM_DEFAULT);
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
    if (!umem_init_once()) {
        return NULL;
    }

    size_t user_size = nmemb * size;
    size_t total_size = user_size + sizeof(umem_size_header_t);
    void *raw_ptr = umem_zalloc_fn(total_size, UMEM_DEFAULT);
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
    if (!umem_init_once()) return NULL;

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
    if (!umem_init_once()) return;

    /* Get header before user pointer */
    umem_size_header_t *header = ((umem_size_header_t *)ptr) - 1;

    /* Verify magic number */
    if (header->magic != UMEM_SIZE_MAGIC) {
        fprintf(stderr, "ERROR: umem_free called with invalid pointer %p (magic=0x%lx)\n",
                ptr, (unsigned long)header->magic);
        return;
    }

    /* Free with original size */
    umem_free_fn((void *)header, header->size);
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
