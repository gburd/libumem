/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

#ifndef UMEM_STACKTRACE_H
#define UMEM_STACKTRACE_H

#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * GDB-style stack trace formatting for libumem error reporting.
 *
 * Output format:
 *   #0  0x7f3a2b1045a2 in umem_cache_free_debug+0x42 () at umem.c:1782
 *   #1  0x55a3001012f4 in my_function+0x18 () at myapp.c:42
 *
 * Three-tier symbol resolution:
 *   1. libdw (best): file + line via DWARF debug info
 *   2. addr2line (fallback): fork addr2line for file + line
 *   3. dladdr only (minimum): function + offset, no file:line
 */

void umem_stacktrace_print(uintptr_t *pcs, int depth,
    const char *header);
void umem_stacktrace_format(uintptr_t pc, int frame_num,
    char *buf, size_t bufsz);
int  umem_stacktrace_init(void);

#ifdef __cplusplus
}
#endif

#endif /* UMEM_STACKTRACE_H */
