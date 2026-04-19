/*
 * Coverage-focused tests targeting uncovered code paths in libumem.
 *
 * These tests exercise batch operations, magazine mechanics,
 * slab destruction, malloc internals, and other paths that
 * existing unit tests don't reach.
 */

#include "../munit.h"
#include "../../umem.h"
#include "../../sys/vmem.h"
#include "../../umem_base.h"
#include "../../umem_gc_roots.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>

/* Batch alloc/free (declared in umem_base.h) */

/* Update thread trigger */
extern void umem_reap(void);

/* vmem_reap for exercising vmem reclaim */
extern void vmem_reap(void);

/* ---- Batch alloc/free tests ---- */

static MunitResult
test_batch_alloc_free(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    umem_cache_t *cp = umem_cache_create("batch_test", 64, 0,
        NULL, NULL, NULL, NULL, NULL, 0);
    munit_assert_not_null(cp);

    void *bufs[32];
    int got = umem_cache_alloc_batch(cp, bufs, 32, UMEM_DEFAULT);
    munit_assert_int(got, >, 0);

    for (int i = 0; i < got; i++) {
        munit_assert_not_null(bufs[i]);
        memset(bufs[i], 0xAA, 64);
    }

    umem_cache_free_batch(cp, bufs, got);
    umem_cache_destroy(cp);
    return MUNIT_OK;
}

static MunitResult
test_batch_zero_count(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    umem_cache_t *cp = umem_cache_create("batch_zero", 64, 0,
        NULL, NULL, NULL, NULL, NULL, 0);
    munit_assert_not_null(cp);

    void *bufs[4];
    int got = umem_cache_alloc_batch(cp, bufs, 0, UMEM_DEFAULT);
    munit_assert_int(got, ==, 0);

    umem_cache_free_batch(cp, bufs, 0);
    umem_cache_destroy(cp);
    return MUNIT_OK;
}

static MunitResult
test_batch_single(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    umem_cache_t *cp = umem_cache_create("batch_single", 128, 0,
        NULL, NULL, NULL, NULL, NULL, 0);
    munit_assert_not_null(cp);

    void *bufs[1];
    int got = umem_cache_alloc_batch(cp, bufs, 1, UMEM_DEFAULT);
    munit_assert_int(got, ==, 1);
    munit_assert_not_null(bufs[0]);

    umem_cache_free_batch(cp, bufs, 1);
    umem_cache_destroy(cp);
    return MUNIT_OK;
}

static MunitResult
test_batch_large(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    umem_cache_t *cp = umem_cache_create("batch_large", 256, 0,
        NULL, NULL, NULL, NULL, NULL, 0);
    munit_assert_not_null(cp);

    void *bufs[256];
    int got = umem_cache_alloc_batch(cp, bufs, 256, UMEM_DEFAULT);
    munit_assert_int(got, >, 0);

    umem_cache_free_batch(cp, bufs, got);
    umem_cache_destroy(cp);
    return MUNIT_OK;
}

/* ---- Slab destruction path ---- */

static MunitResult
test_slab_destroy(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    /* Create a NOMAGAZINE cache to force slab-level alloc/free */
    umem_cache_t *cp = umem_cache_create("slab_destroy", 512, 0,
        NULL, NULL, NULL, NULL, NULL, UMC_NOMAGAZINE);
    munit_assert_not_null(cp);

    /* Allocate enough to create multiple slabs */
    void *ptrs[200];
    int count = 0;
    for (int i = 0; i < 200; i++) {
        ptrs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
        if (ptrs[i] == NULL) break;
        count++;
    }
    munit_assert_int(count, >, 0);

    /* Free all to trigger slab destruction */
    for (int i = 0; i < count; i++) {
        umem_cache_free(cp, ptrs[i]);
    }

    /* Reap to force slab reclamation */
    umem_reap();

    umem_cache_destroy(cp);
    return MUNIT_OK;
}

/* ---- Magazine exhaustion and depot ---- */

static MunitResult
test_magazine_exhaustion(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    umem_cache_t *cp = umem_cache_create("mag_exhaust", 32, 0,
        NULL, NULL, NULL, NULL, NULL, 0);
    munit_assert_not_null(cp);

    /* Allocate many objects to exhaust magazines and hit depot */
    void *ptrs[2000];
    int count = 0;
    for (int i = 0; i < 2000; i++) {
        ptrs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
        if (ptrs[i] == NULL) break;
        count++;
    }

    /* Free all */
    for (int i = 0; i < count; i++) {
        umem_cache_free(cp, ptrs[i]);
    }

    /* Alloc again to exercise depot retrieval */
    int count2 = 0;
    for (int i = 0; i < 2000; i++) {
        ptrs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
        if (ptrs[i] == NULL) break;
        count2++;
    }

    for (int i = 0; i < count2; i++) {
        umem_cache_free(cp, ptrs[i]);
    }

    umem_cache_destroy(cp);
    return MUNIT_OK;
}

/* ---- Reap and reclaim paths ---- */

static MunitResult
test_reap_pressure(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    /* Create several caches */
    umem_cache_t *caches[5];
    for (int i = 0; i < 5; i++) {
        char name[32];
        snprintf(name, sizeof(name), "reap_%d", i);
        caches[i] = umem_cache_create(name, 64 * (i + 1), 0,
            NULL, NULL, NULL, NULL, NULL, 0);
        munit_assert_not_null(caches[i]);
    }

    /* Allocate and free to create reclaimable state */
    for (int c = 0; c < 5; c++) {
        void *ptrs[100];
        for (int i = 0; i < 100; i++) {
            ptrs[i] = umem_cache_alloc(caches[c], UMEM_DEFAULT);
            munit_assert_not_null(ptrs[i]);
        }
        for (int i = 0; i < 100; i++) {
            umem_cache_free(caches[c], ptrs[i]);
        }
    }

    /* Trigger reap */
    umem_reap();
    vmem_reap();

    for (int i = 0; i < 5; i++) {
        umem_cache_destroy(caches[i]);
    }

    return MUNIT_OK;
}

/* ---- malloc/calloc/realloc coverage ---- */

static MunitResult
test_malloc_basic(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    /* Small allocations */
    void *p = umem_alloc(1, UMEM_DEFAULT);
    munit_assert_not_null(p);
    umem_free(p, 1);

    /* Zero-size allocation */
    p = umem_alloc(0, UMEM_DEFAULT);
    /* May return NULL or a valid pointer */
    if (p) umem_free(p, 0);

    /* Various sizes to hit different size classes */
    size_t sizes[] = {1, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65,
                      127, 128, 255, 256, 512, 1024, 2048, 4096, 8192,
                      16384, 32768, 65536, 131072};
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        p = umem_alloc(sizes[i], UMEM_DEFAULT);
        munit_assert_not_null(p);
        memset(p, 0xBB, sizes[i]);
        umem_free(p, sizes[i]);
    }

    return MUNIT_OK;
}

static MunitResult
test_zalloc_various(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    size_t sizes[] = {1, 16, 64, 256, 1024, 4096, 65536};
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        void *p = umem_zalloc(sizes[i], UMEM_DEFAULT);
        munit_assert_not_null(p);
        /* Verify zeroed */
        unsigned char *bp = (unsigned char *)p;
        for (size_t j = 0; j < sizes[i]; j++) {
            munit_assert_uint8(bp[j], ==, 0);
        }
        umem_free(p, sizes[i]);
    }

    return MUNIT_OK;
}

/* ---- Aligned allocation coverage ---- */

static MunitResult
test_alloc_align_various(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    size_t aligns[] = {8, 16, 32, 64, 128, 256, 512, 1024, 4096};
    for (size_t i = 0; i < sizeof(aligns)/sizeof(aligns[0]); i++) {
        void *p = umem_alloc_align(64, aligns[i], UMEM_DEFAULT);
        if (p != NULL) {
            munit_assert_uint64((uintptr_t)p % aligns[i], ==, 0);
            umem_free_align(p, 64);
        }
    }

    return MUNIT_OK;
}

/* ---- Cache with constructor/destructor ---- */

static int ctor_count;
static int dtor_count;

static int test_ctor(void *buf, void *unused, int flags)
{
    (void)unused; (void)flags;
    memset(buf, 0, 64);
    __atomic_add_fetch(&ctor_count, 1, __ATOMIC_RELAXED);
    return 0;
}

static void test_dtor(void *buf, void *unused)
{
    (void)buf; (void)unused;
    __atomic_add_fetch(&dtor_count, 1, __ATOMIC_RELAXED);
}

static MunitResult
test_ctor_dtor_batch(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    ctor_count = 0;
    dtor_count = 0;

    umem_cache_t *cp = umem_cache_create("ctor_batch", 64, 0,
        test_ctor, test_dtor, NULL, NULL, NULL, 0);
    munit_assert_not_null(cp);

    void *bufs[64];
    int got = umem_cache_alloc_batch(cp, bufs, 64, UMEM_DEFAULT);
    munit_assert_int(got, >, 0);

    umem_cache_free_batch(cp, bufs, got);
    umem_cache_destroy(cp);

    munit_assert_int(ctor_count, >, 0);

    return MUNIT_OK;
}

/* ---- Magazine + depot contention ---- */

struct thread_arg {
    umem_cache_t *cp;
    int count;
};

static void *alloc_free_thread(void *arg)
{
    struct thread_arg *ta = arg;
    void *ptrs[100];
    for (int iter = 0; iter < 10; iter++) {
        int n = 0;
        for (int i = 0; i < ta->count && i < 100; i++) {
            ptrs[i] = umem_cache_alloc(ta->cp, UMEM_DEFAULT);
            if (ptrs[i]) n++;
        }
        for (int i = 0; i < n; i++) {
            umem_cache_free(ta->cp, ptrs[i]);
        }
    }
    return NULL;
}

static MunitResult
test_concurrent_depot(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    umem_cache_t *cp = umem_cache_create("conc_depot", 128, 0,
        NULL, NULL, NULL, NULL, NULL, 0);
    munit_assert_not_null(cp);

    struct thread_arg ta = { .cp = cp, .count = 50 };
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, alloc_free_thread, &ta);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }

    umem_cache_destroy(cp);
    return MUNIT_OK;
}

/* ---- Large allocation paths (bypass cache) ---- */

static MunitResult
test_large_alloc_paths(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    /* Allocations larger than UMEM_MAXBUF go through vmem directly */
    size_t sizes[] = {256*1024, 512*1024, 1024*1024, 2*1024*1024};
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        void *p = umem_alloc(sizes[i], UMEM_DEFAULT);
        if (p != NULL) {
            memset(p, 0xCC, 4096);
            umem_free(p, sizes[i]);
        }
    }

    return MUNIT_OK;
}

/* ---- Cache with various flags ---- */

static MunitResult
test_cache_flags_combo(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    /* NOTOUCH */
    umem_cache_t *cp1 = umem_cache_create("flags_nt", 128, 0,
        NULL, NULL, NULL, NULL, NULL, UMC_NOTOUCH);
    if (cp1 != NULL) {
        void *p = umem_cache_alloc(cp1, UMEM_DEFAULT);
        if (p) umem_cache_free(cp1, p);
        umem_cache_destroy(cp1);
    }

    /* NODEBUG */
    umem_cache_t *cp2 = umem_cache_create("flags_nd", 64, 0,
        NULL, NULL, NULL, NULL, NULL, UMC_NODEBUG);
    if (cp2 != NULL) {
        void *p = umem_cache_alloc(cp2, UMEM_DEFAULT);
        if (p) umem_cache_free(cp2, p);
        umem_cache_destroy(cp2);
    }

    /* NOHASH */
    umem_cache_t *cp3 = umem_cache_create("flags_nh", 64, 0,
        NULL, NULL, NULL, NULL, NULL, UMC_NOHASH);
    if (cp3 != NULL) {
        void *p = umem_cache_alloc(cp3, UMEM_DEFAULT);
        if (p) umem_cache_free(cp3, p);
        umem_cache_destroy(cp3);
    }

    return MUNIT_OK;
}

/* ---- Cache alignment variations ---- */

static MunitResult
test_cache_align_variations(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    int aligns[] = {0, 8, 16, 32, 64, 128, 256};
    for (int i = 0; i < 7; i++) {
        char name[32];
        snprintf(name, sizeof(name), "align_%d", aligns[i]);
        umem_cache_t *cp = umem_cache_create(name, 100, aligns[i],
            NULL, NULL, NULL, NULL, NULL, 0);
        munit_assert_not_null(cp);

        void *p = umem_cache_alloc(cp, UMEM_DEFAULT);
        munit_assert_not_null(p);
        if (aligns[i] > 0) {
            munit_assert_uint64((uintptr_t)p % aligns[i], ==, 0);
        }
        umem_cache_free(cp, p);
        umem_cache_destroy(cp);
    }

    return MUNIT_OK;
}

/* ---- umem_free NULL ---- */

static MunitResult
test_free_null(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    umem_free(NULL, 0);
    umem_free(NULL, 64);
    umem_free_align(NULL, 0);

    return MUNIT_OK;
}

/* ---- Reclaim callback ---- */

static int reclaim_called;

static void test_reclaim(void *unused)
{
    (void)unused;
    __atomic_add_fetch(&reclaim_called, 1, __ATOMIC_RELAXED);
}

static MunitResult
test_cache_reclaim_callback(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    reclaim_called = 0;

    umem_cache_t *cp = umem_cache_create("reclaim_cb", 64, 0,
        NULL, NULL, test_reclaim, NULL, NULL, 0);
    munit_assert_not_null(cp);

    /* Allocate and free some objects */
    void *ptrs[50];
    for (int i = 0; i < 50; i++) {
        ptrs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
        munit_assert_not_null(ptrs[i]);
    }
    for (int i = 0; i < 50; i++) {
        umem_cache_free(cp, ptrs[i]);
    }

    /* Trigger reap which should call the reclaim callback */
    umem_reap();

    umem_cache_destroy(cp);
    return MUNIT_OK;
}

/* ---- Multiple reap calls ---- */

static MunitResult
test_multiple_reaps(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    /* Multiple reaps in succession */
    for (int i = 0; i < 10; i++) {
        umem_reap();
    }
    vmem_reap();

    return MUNIT_OK;
}

/* ---- Cache with reclaim + constructor failure ---- */

static int fail_ctor(void *buf, void *unused, int flags)
{
    (void)buf; (void)unused; (void)flags;
    return -1;
}

static MunitResult
test_ctor_failure(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    umem_cache_t *cp = umem_cache_create("ctor_fail", 64, 0,
        fail_ctor, NULL, NULL, NULL, NULL, 0);
    munit_assert_not_null(cp);

    void *p = umem_cache_alloc(cp, UMEM_DEFAULT);
    munit_assert_null(p);

    umem_cache_destroy(cp);
    return MUNIT_OK;
}

/* ---- vmem walk ---- */

static int walk_count;

static void walk_callback(void *unused, void *addr, size_t size)
{
    (void)unused; (void)addr; (void)size;
    walk_count++;
}

static MunitResult
test_vmem_walk_spans(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    walk_count = 0;

    vmem_t *vmp = vmem_create("walk_test", NULL, 0, 4096,
        NULL, NULL, NULL, 0, VM_NOSLEEP);
    munit_assert_not_null(vmp);

    vmem_walk(vmp, VMEM_ALLOC, walk_callback, NULL);
    vmem_walk(vmp, VMEM_FREE, walk_callback, NULL);

    vmem_destroy(vmp);
    return MUNIT_OK;
}

/* ---- vmem_size ---- */

static MunitResult
test_vmem_size_types(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    vmem_t *vmp = vmem_create("size_test", NULL, 0, 4096,
        NULL, NULL, NULL, 0, VM_NOSLEEP);
    munit_assert_not_null(vmp);

    size_t alloc_size = vmem_size(vmp, VMEM_ALLOC);
    size_t free_size = vmem_size(vmp, VMEM_FREE);
    (void)alloc_size;
    (void)free_size;

    vmem_destroy(vmp);
    return MUNIT_OK;
}

/* ---- umem_alloc stress with interleaved sizes ---- */

static MunitResult
test_interleaved_sizes(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    void *ptrs[500];
    size_t sizes[500];
    int count = 0;

    for (int i = 0; i < 500; i++) {
        sizes[i] = ((i * 37 + 13) % 4096) + 1;
        ptrs[i] = umem_alloc(sizes[i], UMEM_DEFAULT);
        if (ptrs[i] == NULL) break;
        count++;
    }

    /* Free in reverse order */
    for (int i = count - 1; i >= 0; i--) {
        umem_free(ptrs[i], sizes[i]);
    }

    return MUNIT_OK;
}

/* ---- SBO (Small Buffer Optimization) ---- */

static MunitResult
test_sbo_operations(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    int enabled = umem_sbo_enabled();
    if (!enabled) {
        return MUNIT_SKIP;
    }

    void *p = umem_sbo_alloc(32, UMEM_DEFAULT);
    if (p != NULL) {
        memset(p, 0xDD, 32);
        umem_sbo_free(p, 32);
    }

    umem_sbo_reset();

    return MUNIT_OK;
}

/* ---- umem_cache with large objects ---- */

static MunitResult
test_cache_large_objects(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    umem_cache_t *cp = umem_cache_create("large_obj", 16384, 0,
        NULL, NULL, NULL, NULL, NULL, 0);
    munit_assert_not_null(cp);

    void *ptrs[20];
    for (int i = 0; i < 20; i++) {
        ptrs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
        munit_assert_not_null(ptrs[i]);
        memset(ptrs[i], i & 0xFF, 16384);
    }

    for (int i = 0; i < 20; i++) {
        umem_cache_free(cp, ptrs[i]);
    }

    umem_cache_destroy(cp);
    return MUNIT_OK;
}

/* ---- GC roots scanning ---- */

static int root_mark_count;

static void root_mark_fn(void *ptr)
{
    (void)ptr;
    root_mark_count++;
}

static MunitResult
test_gc_thread_register(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    /* Thread registration and unregistration */
    int ret = umem_gc_thread_register();
    munit_assert_int(ret, ==, 0);

    int count = umem_gc_thread_count();
    munit_assert_int(count, >=, 1);

    ret = umem_gc_thread_unregister();
    munit_assert_int(ret, ==, 0);

    return MUNIT_OK;
}

static MunitResult
test_gc_scan_roots(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    root_mark_count = 0;

    /* Scan registers */
    umem_gc_scan_registers(root_mark_fn);

    /* Scan stack */
    void *low = NULL;
    void *high = NULL;
    int ret = umem_gc_get_stack_bounds(&low, &high);
    if (ret == 0 && low != NULL && high != NULL) {
        umem_gc_scan_stack(low, high, root_mark_fn);
    }

    /* Scan data segments */
    umem_gc_scan_data_segments(root_mark_fn);

    /* Full scan */
    umem_gc_scan_all_roots(root_mark_fn);

    return MUNIT_OK;
}

static MunitResult
test_gc_stack_bounds(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    void *low = NULL;
    void *high = NULL;
    int ret = umem_gc_get_stack_bounds(&low, &high);
    munit_assert_int(ret, ==, 0);
    munit_assert_not_null(low);
    munit_assert_not_null(high);
    munit_assert_ptr_not_equal(low, high);

    return MUNIT_OK;
}

/* ---- umem_malloc large (second_magic path) ---- */

extern void *umem_malloc(size_t);
extern void umem_malloc_free(void *);
extern void *umem_memalign(size_t, size_t);

static MunitResult
test_malloc_second_align(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    /* Allocations > UMEM_SECOND_ALIGN (16) hit the SECOND_MAGIC path */
    size_t sizes[] = {17, 32, 64, 128, 256, 512, 1024, 4096, 8192,
                      16384, 32768, 65536};
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        void *p = umem_malloc(sizes[i]);
        if (p != NULL) {
            memset(p, 0xEE, sizes[i]);
            umem_malloc_free(p);
        }
    }

    return MUNIT_OK;
}

static MunitResult
test_malloc_memalign_various(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    /* Exercise memalign with various alignments and sizes */
    size_t aligns[] = {8, 16, 32, 64, 128, 256, 512, 1024, 4096};
    for (size_t i = 0; i < sizeof(aligns)/sizeof(aligns[0]); i++) {
        void *p = umem_memalign(aligns[i], 1024);
        if (p != NULL) {
            munit_assert_uint64((uintptr_t)p % aligns[i], ==, 0);
            umem_malloc_free(p);
        }
    }

    /* Small sizes with large alignment */
    void *p = umem_memalign(4096, 1);
    if (p != NULL) {
        munit_assert_uint64((uintptr_t)p % 4096, ==, 0);
        umem_malloc_free(p);
    }

    return MUNIT_OK;
}

/* ---- Cache with many different sizes to hit size tables ---- */

static MunitResult
test_size_class_coverage(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    /* Allocate every size from 1 to 512 to hit all size classes */
    void *ptrs[512];
    int count = 0;
    for (int i = 1; i <= 512; i++) {
        ptrs[count] = umem_alloc(i, UMEM_DEFAULT);
        if (ptrs[count] != NULL) count++;
    }

    for (int i = 0; i < count; i++) {
        umem_free(ptrs[i], i + 1);
    }

    return MUNIT_OK;
}

/* ---- vmem with import function (child arenas) ---- */

static MunitResult
test_vmem_child_arena(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    /* Create parent arena with source */
    vmem_t *parent = vmem_create("cov_parent", NULL, 0, 4096,
        NULL, NULL, NULL, 0, VM_NOSLEEP);
    munit_assert_not_null(parent);

    /* Create child that imports from parent */
    vmem_t *child = vmem_create("cov_child", NULL, 0, 4096,
        (vmem_alloc_t *)vmem_alloc,
        (vmem_free_t *)vmem_free,
        parent, 0, VM_NOSLEEP);

    if (child != NULL) {
        void *p = vmem_alloc(child, 4096, VM_NOSLEEP);
        if (p) vmem_free(child, p, 4096);
        vmem_destroy(child);
    }

    vmem_destroy(parent);
    return MUNIT_OK;
}

/* ---- Rapid cache create/destroy ---- */

static MunitResult
test_rapid_cache_lifecycle(const MunitParameter params[], void *data)
{
    (void)params; (void)data;

    for (int i = 0; i < 50; i++) {
        char name[32];
        snprintf(name, sizeof(name), "rapid_%d", i);
        umem_cache_t *cp = umem_cache_create(name, 32 + i * 8, 0,
            NULL, NULL, NULL, NULL, NULL, 0);
        if (cp != NULL) {
            void *p = umem_cache_alloc(cp, UMEM_DEFAULT);
            if (p) umem_cache_free(cp, p);
            umem_cache_destroy(cp);
        }
    }

    return MUNIT_OK;
}

/* ---- Suite definition ---- */

/* Forward declarations for additional coverage tests */
static MunitResult test_cov_update_thread_trigger(const MunitParameter[], void*);
static MunitResult test_cov_malloc_interpose(const MunitParameter[], void*);
static MunitResult test_cov_vmem_operations(const MunitParameter[], void*);

static MunitTest coverage_tests[] = {
    { "/batch_alloc_free", test_batch_alloc_free, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/batch_zero_count", test_batch_zero_count, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/batch_single", test_batch_single, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/batch_large", test_batch_large, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/slab_destroy", test_slab_destroy, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/magazine_exhaustion", test_magazine_exhaustion, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/reap_pressure", test_reap_pressure, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/malloc_basic", test_malloc_basic, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/zalloc_various", test_zalloc_various, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/alloc_align_various", test_alloc_align_various, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/ctor_dtor_batch", test_ctor_dtor_batch, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/concurrent_depot", test_concurrent_depot, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/large_alloc_paths", test_large_alloc_paths, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/cache_flags_combo", test_cache_flags_combo, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/cache_align_variations", test_cache_align_variations, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/free_null", test_free_null, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/cache_reclaim_callback", test_cache_reclaim_callback, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/multiple_reaps", test_multiple_reaps, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/ctor_failure", test_ctor_failure, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/vmem_walk_spans", test_vmem_walk_spans, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/vmem_size_types", test_vmem_size_types, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/interleaved_sizes", test_interleaved_sizes, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/sbo_operations", test_sbo_operations, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/cache_large_objects", test_cache_large_objects, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/gc_thread_register", test_gc_thread_register, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/gc_scan_roots", test_gc_scan_roots, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/gc_stack_bounds", test_gc_stack_bounds, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/malloc_second_align", test_malloc_second_align, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/malloc_memalign_various", test_malloc_memalign_various, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/size_class_coverage", test_size_class_coverage, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/vmem_child_arena", test_vmem_child_arena, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/rapid_cache_lifecycle", test_rapid_cache_lifecycle, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/update_thread_trigger", test_cov_update_thread_trigger, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { "/malloc_interpose_paths", test_cov_malloc_interpose, NULL, NULL,
      MUNIT_TEST_OPTION_NONE, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

MunitSuite suite_coverage = {
    "/coverage",
    coverage_tests,
    NULL,
    1,
    MUNIT_SUITE_OPTION_NONE
};

/*
 * Additional coverage: trigger update thread and envvar parsing
 */

static MunitResult
test_cov_update_thread_trigger(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    /* Allocate and free enough to trigger magazine reaping */
    umem_cache_t *cp = umem_cache_create("cov_update", 64, 0,
        NULL, NULL, NULL, NULL, NULL, 0);
    munit_assert_not_null(cp);

    void *ptrs[500];
    for (int i = 0; i < 500; i++) {
        ptrs[i] = umem_cache_alloc(cp, UMEM_DEFAULT);
        munit_assert_not_null(ptrs[i]);
    }
    for (int i = 0; i < 500; i++)
        umem_cache_free(cp, ptrs[i]);

    /* Force reap which exercises update thread code paths */
    umem_reap();

    /* Sleep briefly to let update thread process */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 }; /* 100ms */
    nanosleep(&ts, NULL);

    umem_reap();
    umem_cache_destroy(cp);
    return MUNIT_OK;
}

static MunitResult
test_cov_malloc_interpose(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    /* Exercise malloc paths that go through libumem when interposed */
    void *p1 = umem_alloc(7, UMEM_DEFAULT);  /* Odd size */
    void *p2 = umem_alloc(1, UMEM_DEFAULT);  /* Minimum */
    void *p3 = umem_alloc(131072, UMEM_DEFAULT); /* UMEM_MAXBUF */
    
    munit_assert_not_null(p1);
    munit_assert_not_null(p2);
    munit_assert_not_null(p3);
    
    umem_free(p1, 7);
    umem_free(p2, 1);
    umem_free(p3, 131072);
    return MUNIT_OK;
}

static MunitResult
test_cov_vmem_operations(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    /* Exercise vmem paths: create arena with source, allocate, free */
    vmem_t *src = vmem_create("cov_src", NULL, 0, 64,
        NULL, NULL, NULL, 0, VM_NOSLEEP);
    if (src == NULL) return MUNIT_SKIP;
    
    /* Add a span */
    void *span = malloc(8192);
    munit_assert_not_null(span);
    void *added = vmem_add(src, span, 8192, VM_NOSLEEP);
    if (added == NULL) {
        free(span);
        vmem_destroy(src);
        return MUNIT_SKIP;
    }
    
    /* Allocate from it */
    void *v1 = vmem_alloc(src, 128, VM_NOSLEEP);
    void *v2 = vmem_alloc(src, 256, VM_NOSLEEP);
    if (v1) vmem_free(src, v1, 128);
    if (v2) vmem_free(src, v2, 256);
    
    /* Walk the arena */
    vmem_walk(src, VMEM_ALLOC | VMEM_FREE, NULL, NULL);
    
    free(span);
    return MUNIT_OK;
}
