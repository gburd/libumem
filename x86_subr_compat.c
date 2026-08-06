/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
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
 * x86 (amd64 / i386) getfp() and _breakpoint() for the illumos/Solaris
 * build.
 *
 * The historic bundled helper i386_subr_sol.s provides these in assembly,
 * but it #includes <sys/asm_linkage.h> (for ENTRY/SET_SIZE), which is not
 * present in a minimal illumos userland header set.  This compiler-portable
 * C unit provides the same two tiny helpers with no header dependency, so
 * the SOLARIS x86 build resolves getfp (used by getpcstack.c) and
 * _breakpoint (used by umem_agent_support.c) without the missing symbols
 * that otherwise abort libumem.so on the first stack walk.
 *
 * On x86, __builtin_frame_address(0) yields the current frame pointer
 * (%rbp / %ebp), matching what the asm getfp returned.
 *
 * Only compiled on the SOLARIS x86 branch (see Makefile.am); Linux/FreeBSD
 * x86 use getpcstack.c's frame-pointer walk and never reference getfp.
 */

void *
getfp(void)
{
	return (__builtin_frame_address(0));
}

#ifndef UMEM_STANDALONE
void
_breakpoint(void)
{
	__asm__ __volatile__("int $3");
}
#endif
