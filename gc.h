/*
 * gc.h - Drop-in replacement for Boehm GC
 *
 * Include this header instead of <gc/gc.h> to use libumem's
 * conservative garbage collector with Boehm-compatible API.
 */

#ifndef _GC_GC_H
#define	_GC_GC_H

/*
 * EXPERIMENTAL API -- not production-ready.
 * This API may change without notice. Do not use in production code
 * without thorough testing. See README.md for stability guarantees.
 */
#if !defined(UMEM_ENABLE_EXPERIMENTAL) && !defined(HAVE_CONFIG_H)
#error "This header requires #define UMEM_ENABLE_EXPERIMENTAL before inclusion"
#endif

#include "umem_gc.h"

#endif /* _GC_GC_H */
