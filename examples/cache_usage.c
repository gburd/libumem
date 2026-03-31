/*
 * Object cache usage examples for libumem
 *
 * Demonstrates cache creation, allocation, and use with constructors/destructors.
 */

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>
#include <umem.h>

/* Example object structure */
typedef struct user_object {
    int id;
    char name[64];
    pthread_mutex_t lock;
    void *data;
} user_object_t;

/* Constructor: Initialize object to constructed state */
static int user_object_constructor(void *buf, void *arg, int flags) {
    user_object_t *obj = (user_object_t *)buf;

    /* Initialize the mutex */
    pthread_mutex_init(&obj->lock, NULL);

    /* Set default values */
    obj->id = -1;
    obj->name[0] = '\0';
    obj->data = NULL;

    printf("   Constructor called for object at %p\n", obj);
    return 0;
}

/* Destructor: Verify object in constructed state and cleanup */
static void user_object_destructor(void *buf, void *arg) {
    user_object_t *obj = (user_object_t *)buf;

    /* Verify object is in constructed state */
    assert(obj->data == NULL);  /* Should be reset before free */

    /* Cleanup mutex */
    pthread_mutex_destroy(&obj->lock);

    printf("   Destructor called for object at %p\n", obj);
}

int main(void) {
    printf("libumem Object Cache Examples\n");
    printf("==============================\n\n");

    /* Example 1: Simple cache without constructors */
    printf("1. Simple cache (no constructors):\n");
    umem_cache_t *simple_cache = umem_cache_create(
        "simple_objects",        /* name */
        sizeof(user_object_t),   /* size */
        0,                       /* alignment (0 = default) */
        NULL,                    /* constructor */
        NULL,                    /* destructor */
        NULL,                    /* reclaim */
        NULL,                    /* private data */
        NULL,                    /* source */
        0                        /* flags */
    );

    if (simple_cache == NULL) {
        fprintf(stderr, "   Failed to create simple cache\n");
        return 1;
    }
    printf("   Created cache: simple_objects\n");

    /* Allocate from cache */
    user_object_t *obj1 = umem_cache_alloc(simple_cache, UMEM_DEFAULT);
    if (obj1 == NULL) {
        fprintf(stderr, "   Allocation failed\n");
        return 1;
    }
    printf("   Allocated object at %p\n", obj1);

    /* Use the object */
    obj1->id = 100;
    strcpy(obj1->name, "Alice");
    printf("   Set object: id=%d, name=%s\n", obj1->id, obj1->name);

    /* Free back to cache */
    umem_cache_free(simple_cache, obj1);
    printf("   Freed object back to cache\n\n");

    /* Example 2: Cache with constructors/destructors */
    printf("2. Cache with constructors/destructors:\n");
    umem_cache_t *managed_cache = umem_cache_create(
        "managed_objects",
        sizeof(user_object_t),
        0,
        user_object_constructor,
        user_object_destructor,
        NULL,
        NULL,
        NULL,
        0
    );

    if (managed_cache == NULL) {
        fprintf(stderr, "   Failed to create managed cache\n");
        return 1;
    }
    printf("   Created cache: managed_objects\n");

    /* Allocate - constructor will be called */
    user_object_t *obj2 = umem_cache_alloc(managed_cache, UMEM_DEFAULT);
    if (obj2 == NULL) {
        fprintf(stderr, "   Allocation failed\n");
        return 1;
    }
    printf("   Allocated object (constructor ran)\n");

    /* Use the object - mutex is already initialized */
    pthread_mutex_lock(&obj2->lock);
    obj2->id = 200;
    strcpy(obj2->name, "Bob");
    pthread_mutex_unlock(&obj2->lock);
    printf("   Used object with mutex protection\n");

    /* Must return to constructed state before freeing */
    obj2->data = NULL;  /* Reset any dynamic state */

    /* Free - destructor will be called */
    umem_cache_free(managed_cache, obj2);
    printf("   Freed object (destructor ran)\n\n");

    /* Example 3: Multiple allocations from cache */
    printf("3. Multiple allocations from cache:\n");
    user_object_t *objects[5];

    for (int i = 0; i < 5; i++) {
        objects[i] = umem_cache_alloc(managed_cache, UMEM_DEFAULT);
        if (objects[i] == NULL) {
            fprintf(stderr, "   Allocation %d failed\n", i);
            return 1;
        }
        objects[i]->id = 1000 + i;
        snprintf(objects[i]->name, sizeof(objects[i]->name), "User%d", i);
    }
    printf("   Allocated 5 objects from cache\n");

    /* Use them */
    for (int i = 0; i < 5; i++) {
        printf("   Object %d: id=%d, name=%s\n", i, objects[i]->id, objects[i]->name);
    }

    /* Free them */
    for (int i = 0; i < 5; i++) {
        objects[i]->data = NULL;  /* Return to constructed state */
        umem_cache_free(managed_cache, objects[i]);
    }
    printf("   Freed all objects back to cache\n\n");

    /* Example 4: Cache with alignment */
    printf("4. Cache with 64-byte alignment:\n");
    umem_cache_t *aligned_cache = umem_cache_create(
        "aligned_objects",
        sizeof(user_object_t),
        64,                      /* 64-byte alignment */
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        0
    );

    if (aligned_cache == NULL) {
        fprintf(stderr, "   Failed to create aligned cache\n");
        return 1;
    }
    printf("   Created cache with 64-byte alignment\n");

    user_object_t *obj4 = umem_cache_alloc(aligned_cache, UMEM_DEFAULT);
    if (obj4 == NULL) {
        fprintf(stderr, "   Allocation failed\n");
        return 1;
    }

    uintptr_t addr = (uintptr_t)obj4;
    if ((addr & 63) == 0) {
        printf("   Object at %p is correctly aligned to 64 bytes\n", obj4);
    } else {
        printf("   ERROR: Object not properly aligned!\n");
        return 1;
    }

    umem_cache_free(aligned_cache, obj4);
    printf("   Freed object\n\n");

    /* Example 5: Cache with UMC_NODEBUG flag */
    printf("5. High-performance cache (UMC_NODEBUG):\n");
    umem_cache_t *fast_cache = umem_cache_create(
        "fast_objects",
        sizeof(user_object_t),
        0,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        UMC_NODEBUG              /* Disable debug overhead */
    );

    if (fast_cache == NULL) {
        fprintf(stderr, "   Failed to create fast cache\n");
        return 1;
    }
    printf("   Created cache with debugging disabled\n");
    printf("   (UMC_NODEBUG improves performance but skips debug checks)\n");

    user_object_t *obj5 = umem_cache_alloc(fast_cache, UMEM_DEFAULT);
    if (obj5 == NULL) {
        fprintf(stderr, "   Allocation failed\n");
        return 1;
    }
    printf("   Allocated from fast cache\n");

    umem_cache_free(fast_cache, obj5);
    printf("   Freed object\n\n");

    /* Cleanup: Destroy all caches */
    printf("Cleaning up caches:\n");
    umem_cache_destroy(simple_cache);
    printf("   Destroyed simple_cache\n");
    umem_cache_destroy(managed_cache);
    printf("   Destroyed managed_cache\n");
    umem_cache_destroy(aligned_cache);
    printf("   Destroyed aligned_cache\n");
    umem_cache_destroy(fast_cache);
    printf("   Destroyed fast_cache\n\n");

    printf("All examples completed successfully!\n");
    printf("\nKey takeaways:\n");
    printf("- Caches are ideal for frequently allocated objects of the same size\n");
    printf("- Constructors/destructors maintain type-stable state\n");
    printf("- Objects must return to constructed state before freeing\n");
    printf("- Use UMC_NODEBUG for production performance\n");
    printf("- Alignment can be specified for hardware requirements\n");

    return 0;
}
