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
/*
 * Copyright 2004 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * SPARC assembly subroutines for libumem.
 *
 * Provides getfp() and flush_windows() used by getpcstack.c
 * for real stack walking on Solaris/Illumos SPARC.
 *
 * Built with GCC's .S preprocessing; uses GAS-compatible syntax
 * instead of Solaris asm_linkage.h macros.
 */

	.section ".text"

	.global	getfp
	.type	getfp, #function
getfp:
	retl
	mov	%fp, %o0
	.size	getfp, .-getfp

#ifndef UMEM_STANDALONE
	/*
	 * Non-standalone: use trap 0x3 (ST_FLUSH_WINDOWS)
	 * to flush all register windows to the stack.
	 */
	.global	flush_windows
	.type	flush_windows, #function
flush_windows:
	retl
	ta	0x3
	.size	flush_windows, .-flush_windows

	.global	_breakpoint
	.type	_breakpoint, #function
_breakpoint:
	retl
	ta	0x1
	.size	_breakpoint, .-_breakpoint

#else /* UMEM_STANDALONE */

#ifdef __sparcv9
	/*
	 * Standalone v9: use flushw instruction directly.
	 */
	.global	flush_windows
	.type	flush_windows, #function
flush_windows:
	retl
	flushw
	.size	flush_windows, .-flush_windows
#else
#error "This file does not provide a pre-v9 standalone flush_windows"
#endif

#endif /* UMEM_STANDALONE */
