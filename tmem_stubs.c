#include "config.h"
#include <stddef.h>
#include <stdint.h>

#define TMEM_NENTRIES 16

/*
 * The tmem_t structure provides per-thread caching for PTC (per-thread cache).
 * On Solaris, this lives inside the ulwp_t at a known offset from the thread
 * pointer (%fs:0).  On Linux, we use __thread storage and compute the %fs
 * offset at runtime.
 *
 * The structure layout must match what genasm expects:
 *   offset 0:   tm_size  (size_t) - total cached bytes for this thread
 *   offset 8:   tm_roots (void*[NENTRIES]) - linked list heads per cache
 */
typedef struct tmem {
	size_t		tm_size;
	void		*tm_roots[TMEM_NENTRIES];
} tmem_t;

static __thread tmem_t _tmem __attribute__((tls_model("initial-exec")));
static __thread void (*_tmem_cleanup_func)(void *, int) = NULL;

/*
 * Return the %fs-relative offset of the tmem structure.
 *
 * On Linux x86_64, __thread variables with initial-exec TLS model are
 * accessed at fixed negative offsets from %fs:0 (the thread pointer).
 * We compute this offset by taking the difference between the variable's
 * address and the value of %fs:0.
 *
 * On Solaris, this would return the offset within the ulwp_t.
 */
uintptr_t
_tmem_get_base(void)
{
#if defined(__x86_64__) || defined(__amd64__)
	uintptr_t fs_base;
	__asm__ __volatile__("movq %%fs:0, %0" : "=r" (fs_base));
	return (uintptr_t)&_tmem - fs_base;
#elif defined(__i386__)
	uintptr_t gs_base;
	__asm__ __volatile__("movl %%gs:0, %0" : "=r" (gs_base));
	return (uintptr_t)&_tmem - gs_base;
#else
	return 0;
#endif
}

void
_tmem_set_cleanup(void (*f)(void *, int))
{
	_tmem_cleanup_func = f;
}

int
_tmem_get_nentries(void)
{
	return TMEM_NENTRIES;
}
