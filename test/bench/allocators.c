/*
 * Allocator implementations for benchmarking.
 *
 * umem is linked directly (it's the library under test). Every other
 * allocator is loaded at runtime instead of being linked with -l<name>.
 * This matters for correctness, not just convenience: jemalloc/tcmalloc/
 * mimalloc/snmalloc/scudo/rpmalloc all define plain `malloc`/`free`/
 * `calloc`/`realloc` as *strong, global* symbols (most of them optionally
 * through a private-prefixed alias like je_malloc/tc_malloc/mi_malloc,
 * but the plain names are what actually gets exported and, if statically
 * linked, override the process-wide malloc symbol other code -- including
 * this file's own libc_alloc() -- resolves to). Verified empirically on
 * EC2 (see docs/results/2026-09-*-allocator-shootout.md provenance
 * notes): linking -ljemalloc into this binary made plain malloc()/
 * je_malloc() the *same* function, silently contaminating the "libc"
 * baseline with jemalloc's allocator.
 *
 * Two loading techniques are used, chosen per-allocator by what actually
 * works on the platforms this was verified against (x86_64 + aarch64,
 * glibc + musl):
 *
 *   1. dlopen(RTLD_LOCAL): loads the library into its own symbol scope so
 *      its malloc/free never leak into the process-wide symbol table;
 *      only our own dlsym'd function pointers call into it. Works for
 *      most allocators on glibc.
 *
 *   2. LD_PRELOAD + dlsym(RTLD_DEFAULT, ...): some allocators' TLS
 *      relocations (initial-exec model) fail dlopen(RTLD_LOCAL) outright
 *      with "cannot allocate memory in static TLS block", regardless of
 *      how large glibc's static-TLS surplus (GLIBC_TUNABLES=glibc.rtld.
 *      optional_static_tls) is set -- observed for scudo and full
 *      libtcmalloc.so on AL2023 aarch64/glibc, and for *every* non-libc
 *      allocator on musl/Alpine (which has no static-TLS-surplus tunable
 *      at all). IE-model TLS for the *initial* set of objects a process
 *      loads is resolved once at exec, before main() -- so LD_PRELOADing
 *      the library instead of dlopen()ing it after the fact sidesteps
 *      the problem entirely (verified on glibc/aarch64 and musl/x86_64).
 *      matrix.sh / install_extra_allocators.sh arrange the LD_PRELOAD;
 *      each allocator's *_try_load() here checks RTLD_DEFAULT for a
 *      symbol unique to it first and only falls back to dlopen if that
 *      symbol isn't already present in the process.
 *
 * A missing library or missing symbol leaves an allocator's ops all-NULL,
 * which bench_main already treats as "not available" and skips.
 */

#define _GNU_SOURCE
#include "bench_framework.h"
#include <umem.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <dlfcn.h>

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

/* ========== libumem (linked directly; it's the library under test) ===== */
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

/* ========== libumem via its LD_PRELOAD interposer ==========
 * What a drop-in user actually gets: plain malloc()/free() resolved to
 * libumem_malloc.so (8-byte malloc_data_t header, ownership classification
 * on every free()), NOT the 16-byte-header API wrapper above.  The two are
 * different code paths and must be labelled separately in any result.
 *
 * Available only when libumem_malloc.so is LD_PRELOADed (matrix.sh does
 * this for -a umem-preload); detected by its exported fork hook, which
 * libumem.so only references weakly, so RTLD_DEFAULT finds it iff the
 * interposer is loaded.  Under that preload the process-wide malloc IS
 * umem, so the "libc" entry is disabled in this process rather than left
 * to report umem's numbers under libc's name (the same contamination the
 * file header describes for statically linked competitors). */
static void* umem_preload_alloc(size_t size) { return malloc(size); }
static void* umem_preload_calloc(size_t n, size_t size) { return calloc(n, size); }
static void* umem_preload_realloc(void *p, size_t size) { return realloc(p, size); }
static void umem_preload_free(void *p) { free(p); }

allocator_ops_t allocator_umem_preload = {
    .name = "umem-preload (not available: LD_PRELOAD libumem_malloc.so)",
    .alloc = NULL, .calloc = NULL, .realloc = NULL, .free = NULL, .cleanup = NULL
};

__attribute__((constructor))
static void umem_preload_try_load(void) {
    if (dlsym(RTLD_DEFAULT, "umem_interpose_lockup") == NULL)
        return;
    allocator_umem_preload.name = "umem-preload";
    allocator_umem_preload.alloc = umem_preload_alloc;
    allocator_umem_preload.calloc = umem_preload_calloc;
    allocator_umem_preload.realloc = umem_preload_realloc;
    allocator_umem_preload.free = umem_preload_free;
    allocator_libc.name = "libc (not available: LD_PRELOAD=libumem_malloc.so owns malloc)";
    allocator_libc.alloc = NULL;
    allocator_libc.calloc = NULL;
    allocator_libc.realloc = NULL;
    allocator_libc.free = NULL;
}

/* ========== shared loader for every third-party allocator below =========
 * Each one just needs a path list to try with dlopen (first match wins,
 * so packaged and from-source locations both work), a marker symbol
 * unique to it (to detect an LD_PRELOAD), and the four symbol names.
 */
typedef struct dlopen_malloc_syms {
    void *handle;               /* dlopen() handle, or RTLD_DEFAULT if preloaded */
    void *(*alloc)(size_t);
    void *(*calloc)(size_t, size_t);
    void *(*realloc)(void *, size_t);
    void (*free)(void *);
} dlopen_malloc_syms_t;

/* Candidate paths are tried in order; NULL-terminated. Override any one
 * allocator's search path with UMEM_BENCH_<NAME>_PATH (colon-free single
 * path) if it's installed somewhere unusual. */
static void *dlopen_first(const char * const *paths, const char *env_override) {
    const char *override = env_override ? getenv(env_override) : NULL;
    if (override && *override) {
        void *h = dlopen(override, RTLD_NOW | RTLD_LOCAL);
        if (h) return h;
        fprintf(stderr, "WARNING: %s=%s failed to dlopen: %s\n",
                env_override, override, dlerror());
    }
    for (int i = 0; paths[i] != NULL; i++) {
        void *h = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
        if (h) return h;
    }
    return NULL;
}

static int dlopen_malloc_load(dlopen_malloc_syms_t *out, const char * const *paths,
                               const char *env_override, const char *alloc_sym,
                               const char *calloc_sym, const char *realloc_sym,
                               const char *free_sym) {
    memset(out, 0, sizeof(*out));
    out->handle = dlopen_first(paths, env_override);
    if (!out->handle) return -1;

    out->alloc = (void *(*)(size_t))dlsym(out->handle, alloc_sym);
    out->calloc = (void *(*)(size_t, size_t))dlsym(out->handle, calloc_sym);
    out->realloc = (void *(*)(void *, size_t))dlsym(out->handle, realloc_sym);
    out->free = (void (*)(void *))dlsym(out->handle, free_sym);

    if (!out->alloc || !out->calloc || !out->realloc || !out->free) {
        dlclose(out->handle);
        memset(out, 0, sizeof(*out));
        return -1;
    }
    return 0;
}

/* Preloaded-or-dlopen: see the file header comment for why this exists.
 * marker_sym must be exported by the target allocator and by nothing
 * else we might load. */
static int preloaded_or_dlopen_malloc_load(dlopen_malloc_syms_t *out,
                               const char *marker_sym,
                               const char * const *paths,
                               const char *env_override, const char *alloc_sym,
                               const char *calloc_sym, const char *realloc_sym,
                               const char *free_sym) {
    memset(out, 0, sizeof(*out));
    if (dlsym(RTLD_DEFAULT, marker_sym) != NULL) {
        out->alloc = (void *(*)(size_t))dlsym(RTLD_DEFAULT, alloc_sym);
        out->calloc = (void *(*)(size_t, size_t))dlsym(RTLD_DEFAULT, calloc_sym);
        out->realloc = (void *(*)(void *, size_t))dlsym(RTLD_DEFAULT, realloc_sym);
        out->free = (void (*)(void *))dlsym(RTLD_DEFAULT, free_sym);
        if (out->alloc && out->calloc && out->realloc && out->free) {
            out->handle = RTLD_DEFAULT;
            return 0;
        }
        memset(out, 0, sizeof(*out));
    }
    return dlopen_malloc_load(out, paths, env_override, alloc_sym, calloc_sym,
                               realloc_sym, free_sym);
}

/* ========== jemalloc ========== */
static dlopen_malloc_syms_t je_syms;

static void* jemalloc_alloc(size_t size) { return je_syms.alloc(size); }
static void* jemalloc_calloc(size_t n, size_t size) { return je_syms.calloc(n, size); }
static void* jemalloc_realloc(void *p, size_t size) { return je_syms.realloc(p, size); }
static void jemalloc_free(void *p) { je_syms.free(p); }

allocator_ops_t allocator_jemalloc = {
    .name = "jemalloc (not available)",
    .alloc = NULL, .calloc = NULL, .realloc = NULL, .free = NULL, .cleanup = NULL
};

__attribute__((constructor))
static void jemalloc_try_load(void) {
    static const char * const paths[] = {
        "libjemalloc.so.2", "libjemalloc.so", "/usr/lib64/libjemalloc.so",
        "/usr/lib/x86_64-linux-gnu/libjemalloc.so.2",
        "/usr/lib/aarch64-linux-gnu/libjemalloc.so.2", NULL
    };
    /* mallctl is jemalloc-specific and reliably present regardless of
     * --with-jemalloc-prefix / --with-mangling, unlike je_malloc. */
    if (preloaded_or_dlopen_malloc_load(&je_syms, "mallctl", paths,
                            "UMEM_BENCH_JEMALLOC_PATH",
                            "malloc", "calloc", "realloc", "free") == 0) {
        allocator_jemalloc.name = "jemalloc";
        allocator_jemalloc.alloc = jemalloc_alloc;
        allocator_jemalloc.calloc = jemalloc_calloc;
        allocator_jemalloc.realloc = jemalloc_realloc;
        allocator_jemalloc.free = jemalloc_free;
    }
}

/* ========== tcmalloc / gperftools ========== */
static dlopen_malloc_syms_t tc_syms;

static void* tcmalloc_alloc(size_t size) { return tc_syms.alloc(size); }
static void* tcmalloc_calloc(size_t n, size_t size) { return tc_syms.calloc(n, size); }
static void* tcmalloc_realloc(void *p, size_t size) { return tc_syms.realloc(p, size); }
static void tcmalloc_free(void *p) { tc_syms.free(p); }

allocator_ops_t allocator_tcmalloc = {
    .name = "tcmalloc (not available)",
    .alloc = NULL, .calloc = NULL, .realloc = NULL, .free = NULL, .cleanup = NULL
};

__attribute__((constructor))
static void tcmalloc_try_load(void) {
    /* Prefer *_minimal: the full libtcmalloc.so links libunwind for
     * CPU-profiler symbolization, which exhausted glibc's static-TLS
     * surplus under dlopen(RTLD_LOCAL) on AL2023 aarch64 even with
     * GLIBC_TUNABLES bumped. *_minimal is the same allocator, no
     * profiler, no libunwind, and dlopens cleanly on x86_64 + aarch64. */
    static const char * const paths[] = {
        "libtcmalloc_minimal.so.4", "libtcmalloc_minimal.so",
        "/usr/lib64/libtcmalloc_minimal.so", "/lib64/libtcmalloc_minimal.so.4",
        "libtcmalloc.so.4", "libtcmalloc.so", "/usr/lib64/libtcmalloc.so",
        "/usr/lib/x86_64-linux-gnu/libtcmalloc.so.4",
        "/usr/lib/aarch64-linux-gnu/libtcmalloc.so.4", NULL
    };
    /* tc_malloc/tc_free/... are always exported alongside plain
     * malloc/free, and are unambiguous even if some other loaded library
     * also happens to export plain names, so prefer them (as both the
     * marker and the symbols themselves). */
    if (preloaded_or_dlopen_malloc_load(&tc_syms, "tc_malloc", paths,
                            "UMEM_BENCH_TCMALLOC_PATH",
                            "tc_malloc", "tc_calloc", "tc_realloc", "tc_free") == 0) {
        allocator_tcmalloc.name = "tcmalloc";
        allocator_tcmalloc.alloc = tcmalloc_alloc;
        allocator_tcmalloc.calloc = tcmalloc_calloc;
        allocator_tcmalloc.realloc = tcmalloc_realloc;
        allocator_tcmalloc.free = tcmalloc_free;
    }
}

/* ========== mimalloc ========== */
static dlopen_malloc_syms_t mi_syms;

static void* mimalloc_alloc(size_t size) { return mi_syms.alloc(size); }
static void* mimalloc_calloc(size_t n, size_t size) { return mi_syms.calloc(n, size); }
static void* mimalloc_realloc(void *p, size_t size) { return mi_syms.realloc(p, size); }
static void mimalloc_free(void *p) { mi_syms.free(p); }

allocator_ops_t allocator_mimalloc = {
    .name = "mimalloc (not available)",
    .alloc = NULL, .calloc = NULL, .realloc = NULL, .free = NULL, .cleanup = NULL
};

__attribute__((constructor))
static void mimalloc_try_load(void) {
    static const char * const paths[] = {
        "libmimalloc.so", "/usr/local/lib/libmimalloc.so",
        "/usr/lib64/libmimalloc.so", "/usr/lib/x86_64-linux-gnu/libmimalloc.so",
        "/usr/lib/aarch64-linux-gnu/libmimalloc.so", NULL
    };
    if (preloaded_or_dlopen_malloc_load(&mi_syms, "mi_malloc", paths,
                            "UMEM_BENCH_MIMALLOC_PATH",
                            "mi_malloc", "mi_calloc", "mi_realloc", "mi_free") == 0) {
        allocator_mimalloc.name = "mimalloc";
        allocator_mimalloc.alloc = mimalloc_alloc;
        allocator_mimalloc.calloc = mimalloc_calloc;
        allocator_mimalloc.realloc = mimalloc_realloc;
        allocator_mimalloc.free = mimalloc_free;
    }
}

/* ========== snmalloc ==========
 * snmalloc's override shim (libsnmallocshim.so) only exports plain
 * malloc/free/calloc/realloc -- there's no sn_malloc-style private
 * prefix to dlsym instead. __malloc_end_pointer is snmalloc's own
 * (non-standard) extension and a reliable preloaded-marker/dlopen
 * target; RTLD_LOCAL on the dlopen fallback keeps the plain names out
 * of the global scope so they never shadow libc_alloc()'s malloc(). */
static dlopen_malloc_syms_t sn_syms;

static void* snmalloc_alloc(size_t size) { return sn_syms.alloc(size); }
static void* snmalloc_calloc(size_t n, size_t size) { return sn_syms.calloc(n, size); }
static void* snmalloc_realloc(void *p, size_t size) { return sn_syms.realloc(p, size); }
static void snmalloc_free(void *p) { sn_syms.free(p); }

allocator_ops_t allocator_snmalloc = {
    .name = "snmalloc (not available)",
    .alloc = NULL, .calloc = NULL, .realloc = NULL, .free = NULL, .cleanup = NULL
};

__attribute__((constructor))
static void snmalloc_try_load(void) {
    static const char * const paths[] = {
        "libsnmallocshim.so", "/usr/local/lib/libsnmallocshim.so", NULL
    };
    if (preloaded_or_dlopen_malloc_load(&sn_syms, "__malloc_end_pointer", paths,
                            "UMEM_BENCH_SNMALLOC_PATH",
                            "malloc", "calloc", "realloc", "free") == 0) {
        allocator_snmalloc.name = "snmalloc";
        allocator_snmalloc.alloc = snmalloc_alloc;
        allocator_snmalloc.calloc = snmalloc_calloc;
        allocator_snmalloc.realloc = snmalloc_realloc;
        allocator_snmalloc.free = snmalloc_free;
    }
}

/* ========== scudo ==========
 * LLVM/compiler-rt's hardened allocator. Ships as
 * libclang_rt.scudo_standalone-<arch>.so (or plain libscudo.so on
 * Alpine/musl); exports only plain names. See the file header comment
 * on preloaded_or_dlopen_malloc_load() -- scudo is the allocator that
 * surfaced the IE-TLS/dlopen problem in the first place (on aarch64). */
static dlopen_malloc_syms_t scudo_syms;

static void* scudo_alloc(size_t size) { return scudo_syms.alloc(size); }
static void* scudo_calloc(size_t n, size_t size) { return scudo_syms.calloc(n, size); }
static void* scudo_realloc(void *p, size_t size) { return scudo_syms.realloc(p, size); }
static void scudo_free(void *p) { scudo_syms.free(p); }

allocator_ops_t allocator_scudo = {
    .name = "scudo (not available)",
    .alloc = NULL, .calloc = NULL, .realloc = NULL, .free = NULL, .cleanup = NULL
};

__attribute__((constructor))
static void scudo_try_load(void) {
    static const char * const paths[] = {
        "libscudo_standalone.so", "libscudo.so",
        "libclang_rt.scudo_standalone-x86_64.so",
        "libclang_rt.scudo_standalone-aarch64.so",
        NULL
    };
    if (preloaded_or_dlopen_malloc_load(&scudo_syms, "__scudo_print_stats", paths,
                            "UMEM_BENCH_SCUDO_PATH",
                            "malloc", "calloc", "realloc", "free") == 0) {
        allocator_scudo.name = "scudo";
        allocator_scudo.alloc = scudo_alloc;
        allocator_scudo.calloc = scudo_calloc;
        allocator_scudo.realloc = scudo_realloc;
        allocator_scudo.free = scudo_free;
    }
}

/* ========== rpmalloc ==========
 * rpmalloc's own API (rpmalloc/rpfree/rpcalloc/rprealloc) needs a
 * one-time rpmalloc_initialize() and a per-thread rpmalloc_thread_
 * initialize() before first use (see rpmalloc.h). Since this benchmark's
 * worker threads are plain pthreads created by bench_framework.c with no
 * rpmalloc-specific hook, we lazily call rpmalloc_thread_initialize() on
 * first use per thread via a thread-local guard -- cheap (one branch) and
 * correct regardless of how many threads the workload spins up. */
static dlopen_malloc_syms_t rp_syms;
static void (*rp_thread_init)(void);
static __thread int rp_thread_ready = 0;

static inline void rpmalloc_ensure_thread(void) {
    if (!rp_thread_ready) {
        rp_thread_init();
        rp_thread_ready = 1;
    }
}

static void* rpmalloc_alloc(size_t size) { rpmalloc_ensure_thread(); return rp_syms.alloc(size); }
static void* rpmalloc_calloc(size_t n, size_t size) { rpmalloc_ensure_thread(); return rp_syms.calloc(n, size); }
static void* rpmalloc_realloc_op(void *p, size_t size) { rpmalloc_ensure_thread(); return rp_syms.realloc(p, size); }
static void rpmalloc_free_op(void *p) { rpmalloc_ensure_thread(); rp_syms.free(p); }

allocator_ops_t allocator_rpmalloc = {
    .name = "rpmalloc (not available)",
    .alloc = NULL, .calloc = NULL, .realloc = NULL, .free = NULL, .cleanup = NULL
};

__attribute__((constructor))
static void rpmalloc_try_load(void) {
    static const char * const paths[] = {
        "librpmalloc.so", "/usr/local/lib/librpmalloc.so", NULL
    };
    if (preloaded_or_dlopen_malloc_load(&rp_syms, "rpmalloc", paths,
                            "UMEM_BENCH_RPMALLOC_PATH",
                            "rpmalloc", "rpcalloc", "rprealloc", "rpfree") == 0) {
        int (*rp_init)(void *) = (int (*)(void *))dlsym(rp_syms.handle, "rpmalloc_initialize");
        rp_thread_init = (void (*)(void))dlsym(rp_syms.handle, "rpmalloc_thread_initialize");
        if (rp_init && rp_thread_init) {
            rp_init(NULL);
            allocator_rpmalloc.name = "rpmalloc";
            allocator_rpmalloc.alloc = rpmalloc_alloc;
            allocator_rpmalloc.calloc = rpmalloc_calloc;
            allocator_rpmalloc.realloc = rpmalloc_realloc_op;
            allocator_rpmalloc.free = rpmalloc_free_op;
        }
    }
}
