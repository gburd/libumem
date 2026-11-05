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
 * Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */
/*
 * Portions Copyright 2006-2008 Message Systems, Inc.
 */

/* #pragma ident	"@(#)getpcstack.c	1.5	05/06/08 SMI" */

#include "config.h"
#include "misc.h"

#if HAVE_UCONTEXT_H
#include <ucontext.h>
#endif

#if HAVE_SYS_FRAME_H
#include <sys/frame.h>
#endif
#if HAVE_SYS_STACK_H
#include <sys/stack.h>
#endif

#include <stdio.h>
#include <stdint.h>

/*
 * LIBUMEM'S OWN FRAMES ARE NOT RECORDED (T9).
 *
 * A walk started in getpcstack() first passes through the allocator itself:
 * umem_cache_alloc_debug, _umem_cache_alloc, _umem_alloc, the malloc
 * interposer, ...  How many depends on the path, so no fixed skip count is
 * right.  Instead every LEADING pc inside libumem's executable segments is
 * dropped, so bc_stack[0] is the first frame outside the library.
 *
 * The ranges are the PF_X PT_LOAD segments of the object containing a given
 * address, found once with dl_iterate_phdr(), which does not allocate.  They
 * are registered from load-time constructors -- never lazily from the walk,
 * which runs under cache locks and would then order the loader lock inside
 * them.  Written once before use; a reader racing a late dlopen sees 0/0 and
 * simply skips nothing.
 *
 * Only a separately loaded object is registered.  ponytail: a static link puts
 * libumem in the main program's text, where this cannot tell library from
 * caller, so nothing is skipped there; a linker-script section range would
 * lift that if static audit stacks ever matter.
 */
#if defined(__linux__)
#include <link.h>
#define	UMEM_NINTERNAL	4
static struct { uintptr_t lo, hi; } umem_internal[UMEM_NINTERNAL];

static int
umem_internal_cb(struct dl_phdr_info *info, size_t sz, void *arg)
{
	uintptr_t want = (uintptr_t)arg;
	int i, n = 0, found = 0;
	uintptr_t lo[UMEM_NINTERNAL], hi[UMEM_NINTERNAL];

	(void) sz;
	for (i = 0; i < info->dlpi_phnum && n < UMEM_NINTERNAL; i++) {
		const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
		if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X))
			continue;
		lo[n] = info->dlpi_addr + ph->p_vaddr;
		hi[n] = lo[n] + ph->p_memsz;
		if (want >= lo[n] && want < hi[n])
			found = 1;
		n++;
	}
	if (!found)
		return (0);
	/* the main program (or a static link): caller code lives here too */
	if (info->dlpi_name == NULL || info->dlpi_name[0] == '\0')
		return (1);
	for (i = 0; i < n; i++) {
		int s;
		for (s = 0; s < UMEM_NINTERNAL; s++) {
			if (umem_internal[s].hi == 0 ||
			    umem_internal[s].lo == lo[i]) {
				umem_internal[s].lo = lo[i];
				umem_internal[s].hi = hi[i];
				break;
			}
		}
	}
	return (1);
}

/* Register the object containing addr as internal.  Constructor-time only. */
void
umem_pcstack_internal(const void *addr)
{
	(void) dl_iterate_phdr(umem_internal_cb, (void *)(uintptr_t)addr);
}

static int
umem_pc_internal(uintptr_t pc)
{
	int s;

	for (s = 0; s < UMEM_NINTERNAL && umem_internal[s].hi != 0; s++)
		if (pc >= umem_internal[s].lo && pc < umem_internal[s].hi)
			return (1);
	return (0);
}

static void umem_pcstack_self_init(void) __attribute__((constructor));
static void
umem_pcstack_self_init(void)
{
	umem_pcstack_internal((const void *)(uintptr_t)&umem_pc_internal);
}
#else
void
umem_pcstack_internal(const void *addr)
{
	(void) addr;
}

static int
umem_pc_internal(uintptr_t pc)
{
	(void) pc;
	return (0);
}
#endif

/*
 * REAL STACK BOUNDS FOR THE FRAME-POINTER WALK (P5.9).
 *
 * The Linux frame walks below dereference fp[0]/fp[1].  Alignment, a
 * monotonically-increasing chain and a 16 MiB span above the starting frame
 * were the only limits, and none of them bounds the walk to memory that is
 * actually this thread's stack:
 *
 *   - a corrupted saved-fp (UMEM_DEBUG=audit on a program that overflows a
 *     stack buffer) points anywhere;
 *   - a caller compiled WITHOUT frame pointers -- the -O2 default -- leaves
 *     an ordinary data value in %rbp/x29, so the "saved fp" is just whatever
 *     that function was using the register for.
 *
 * Either way the allocator READ an attacker-influenced address.  Read-only
 * (the only writes are pcstack[depth++], bounded by pcstack_limit, into the
 * caller's own bufctl), so this is a crash or an info leak, not code
 * execution -- but it is still an out-of-bounds read inside malloc.
 *
 * So ask the system where this thread's stack is and reject every frame
 * outside it: the mapping that contains the current stack pointer, read from
 * /proc/self/maps (see the re-entrancy note below for why not
 * pthread_getattr_np()).
 *
 * RE-ENTRANCY -- WHY THIS MUST NOT ALLOCATE.  getpcstack() runs on the
 * allocator's OWN internal paths: vmem_hash_insert() records it for every
 * audited segment (vmem.c), the UMEM_AUDIT macro records it per bufctl, and
 * both are reached with vmem/cache locks held while the arena state is
 * half-updated.  So this function and everything it calls MUST be
 * allocation-free.  The earlier implementation called glibc
 * pthread_getattr_np(), which mallocs a buffer for the thread's CPU-affinity
 * mask (nptl/pthread_getattr_np.c); under LD_PRELOAD that malloc is libumem's
 * own, so it re-entered the allocator from inside vmem_populate() and either
 * tripped the VMC_POPULATOR assert or deadlocked on the vmem locks.  An
 * in_lookup flag only stopped recursion back into THIS function; the
 * re-entrant malloc still ran.  (Codeberg bug report 2026-11-04.)
 *
 * So read the bounds without allocating: find the mapping that contains the
 * current stack pointer by parsing /proc/self/maps with raw open/read into a
 * stack buffer (no stdio, no malloc).  This is uniform for the main thread
 * and every other thread -- the current SP lands in exactly one mapping -- so
 * no pthread descriptor or main-vs-other special case is needed.  The answer
 * is cached per thread, so the cost is one maps read per thread.
 *
 * FALLBACK, stated rather than hidden: where no bounds are available (no
 * /proc, a parse miss, or a non-Linux build) the old 16 MiB-ceiling heuristic
 * still applies.  It is a heuristic: it bounds how far the walk can wander UP
 * from a frame that is genuinely on the stack, and nothing more.
 */
#if defined(__linux__)
#define	UMEM_HAVE_STACK_BOUNDS	1
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>		/* strtoull (allocation-free, used below) */

/*
 * Parse /proc/self/maps for the mapping that contains address `sp`, writing
 * its [start,end) to *lo/*hi.  Returns 1 on a hit, 0 otherwise.  Uses only
 * open/read/close and a fixed stack buffer -- NO stdio and NO allocation (see
 * the re-entrancy note above).  The file is read in chunks; a line split
 * across two reads is carried over in `line`.
 */
static int
umem_maps_find_sp(uintptr_t sp, uintptr_t *lo, uintptr_t *hi)
{
	char buf[4096];
	char line[256];
	size_t linelen = 0;
	int fd, hit = 0;
	ssize_t n;

	fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return (0);

	while (!hit && (n = read(fd, buf, sizeof (buf))) > 0) {
		ssize_t i;
		for (i = 0; i < n; i++) {
			char c = buf[i];
			if (c != '\n') {
				if (linelen < sizeof (line) - 1)
					line[linelen++] = c;
				continue;
			}
			/* complete line: "<start>-<end> ..." in hex */
			line[linelen] = '\0';
			linelen = 0;
			{
				char *p = line, *endp;
				uintptr_t start, end;
				start = (uintptr_t)strtoull(p, &endp, 16);
				if (endp == p || *endp != '-')
					continue;
				p = endp + 1;
				end = (uintptr_t)strtoull(p, &endp, 16);
				if (endp == p)
					continue;
				if (sp >= start && sp < end) {
					*lo = start;
					*hi = end;
					hit = 1;
					break;
				}
			}
		}
	}
	(void) close(fd);
	return (hit);
}
#endif

/*
 * Returns 1 and fills [*lop, *hip) with this thread's stack, or 0 if the
 * bounds could not be determined.  Never allocates (see the re-entrancy note
 * above); the cached path is a plain TLS read.
 */
static int
umem_stack_bounds(uintptr_t *lop, uintptr_t *hip)
{
#ifdef UMEM_HAVE_STACK_BOUNDS
	static __thread uintptr_t cached_lo
	    __attribute__((tls_model("initial-exec")));
	static __thread uintptr_t cached_hi
	    __attribute__((tls_model("initial-exec")));

	if (cached_hi == 0) {
		/*
		 * The SP of THIS frame is on the stack whose mapping we want.
		 * The whole lookup is allocation-free, so no in_lookup guard is
		 * needed any more -- but a missed parse just leaves cached_hi 0
		 * and we report "unknown" (the heuristic then applies), the same
		 * as before.
		 */
		uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
		uintptr_t lo = 0, hi = 0;
		if (umem_maps_find_sp(sp, &lo, &hi) && hi > lo) {
			cached_lo = lo;
			cached_hi = hi;
		} else {
			return (0);
		}
	}
	*lop = cached_lo;
	*hip = cached_hi;
	return (1);
#else
	(void) lop;
	(void) hip;
	return (0);
#endif
}

/*
 * Is the frame at fp safe to dereference?  Both fp[0] and fp[1] must lie
 * inside the thread stack.  With no bounds available, fall back to the
 * heuristic ceiling.  (Alignment is checked by the caller: it differs per
 * architecture.)
 */
static int
umem_frame_readable(uintptr_t fp, int have_bounds, uintptr_t lo, uintptr_t hi,
    uintptr_t ceiling)
{
	const uintptr_t need = 2 * sizeof (uintptr_t);

	if (have_bounds)
		return (hi >= need && fp >= lo && fp <= hi - need);
	return (fp < ceiling);
}

#if defined(EC_UMEM_DUMMY_PCSTACK) && (defined(__amd64) || defined(__x86_64__) || defined(__i386))
/*
 * Linux x86 (32/64-bit) stack unwinding.
 *
 * The Solaris getpcstack() below relies on stack_getbounds()/thr_stksegment(),
 * which do not exist on Linux, so those platforms otherwise fall back to a
 * dummy that returns 0 -- leaving UMEM_DEBUG=audit records with empty stacks.
 * We instead walk the frame-pointer chain directly. This requires the library
 * to be built with -fno-omit-frame-pointer (audit is a debug build anyway).
 *
 * On x86 the frame layout at [fp] is { saved_rbp, return_addr }, identical in
 * shape to the aarch64 path above.
 */
#define UMEM_HAVE_REAL_PCSTACK 1

/*ARGSUSED*/
int
getpcstack(uintptr_t *pcstack, int pcstack_limit, int check_sigthread)
{
	uintptr_t *fp = (uintptr_t *)__builtin_frame_address(0);
	uintptr_t *nextfp;
	uintptr_t fp_ceiling, stk_lo = 0, stk_hi = 0;
	int have_bounds;
	int depth = 0;
	uintptr_t first_internal = 0;

	if (check_sigthread)
		return (0);		/* not safe from a signal handler */

	/*
	 * Real stack bounds where the system will tell us (P5.9); the 16 MiB
	 * span above the starting frame is only the fallback.  See
	 * umem_stack_bounds() above.
	 */
	have_bounds = umem_stack_bounds(&stk_lo, &stk_hi);
	fp_ceiling = (uintptr_t)fp + (16 * 1024 * 1024);

	while (depth < pcstack_limit && fp != NULL) {
		/* frame pointers are pointer-aligned */
		if ((uintptr_t)fp & (sizeof (uintptr_t) - 1))
			break;

		/* Never dereference a frame outside this thread's stack */
		if (!umem_frame_readable((uintptr_t)fp, have_bounds,
		    stk_lo, stk_hi, fp_ceiling))
			break;

		nextfp = (uintptr_t *)(fp[0]);	/* saved frame pointer */
		/* return address; leading libumem frames are dropped */
		if (depth > 0 || !umem_pc_internal(fp[1]))
			pcstack[depth++] = fp[1];
		else if (first_internal == 0)
			first_internal = fp[1];

		/* stack grows down: a valid caller frame is at a higher address */
		if (nextfp <= fp)
			break;
		fp = nextfp;
	}

	/*
	 * The walk never left libumem (x86_64 without --enable-frame-pointers:
	 * %rbp holds data after the first frame).  Keep that one frame rather
	 * than an empty stack; findleaks/leaks print why it is only one.
	 */
	if (depth == 0 && first_internal != 0 && pcstack_limit > 0)
		pcstack[depth++] = first_internal;

	return (depth);
}

#elif defined(__aarch64__) || defined(__arm64__)
/*
 * ARM64 (aarch64) stack unwinding
 *
 * Frame pointer (fp/x29) points to {previous_fp, return_address} pair.
 * We use direct pointer arithmetic instead of struct frame.
 */

/*ARGSUSED*/
int
getpcstack(uintptr_t *pcstack, int pcstack_limit, int check_sigthread)
{
	uintptr_t *fp;
	uintptr_t *nextfp;
	uintptr_t fp_ceiling, stk_lo = 0, stk_hi = 0;
	int have_bounds;
	int depth = 0;

	uintptr_t first_internal = 0;

	if (check_sigthread) {
		/* Skip if in signal handler - not safe */
		return (0);
	}

	/* Get current frame pointer (x29) */
	__asm__ __volatile__(
		"mov %0, x29"
		: "=r" (fp)
	);

	/*
	 * Real stack bounds where the system will tell us (P5.9); the 16 MiB
	 * span above the starting frame is only the fallback.  See
	 * umem_stack_bounds() above.
	 */
	have_bounds = umem_stack_bounds(&stk_lo, &stk_hi);
	fp_ceiling = (uintptr_t)fp + (16 * 1024 * 1024);

	/* Walk the frame pointer chain */
	while (depth < pcstack_limit && fp != NULL) {
		/* Validate frame pointer alignment (16-byte on ARM64) */
		if ((uintptr_t)fp & 0xf) {
			break;
		}

		/* Never dereference a frame outside this thread's stack */
		if (!umem_frame_readable((uintptr_t)fp, have_bounds,
		    stk_lo, stk_hi, fp_ceiling)) {
			break;
		}

		/* nextfp = *fp (previous frame pointer at [fp + 0]) */
		nextfp = (uintptr_t *)(*fp);

		/* return_address = *(fp + 1) (at [fp + 8]); leading libumem
		 * frames are dropped */
		if (depth == 0 && umem_pc_internal(*(fp + 1))) {
			if (first_internal == 0)
				first_internal = *(fp + 1);
		} else if (depth < pcstack_limit) {
			pcstack[depth] = *(fp + 1);
			depth++;
		}

		/* Stop if frame pointer doesn't advance */
		if (nextfp <= fp) {
			break;
		}

		fp = nextfp;
	}

	/*
	 * The walk never left libumem (x86_64 without --enable-frame-pointers:
	 * %rbp holds data after the first frame).  Keep that one frame rather
	 * than an empty stack; findleaks/leaks print why it is only one.
	 */
	if (depth == 0 && first_internal != 0 && pcstack_limit > 0)
		pcstack[depth++] = first_internal;

	return (depth);
}

#elif defined(__linux__) && defined(__riscv) && (__riscv_xlen == 64)
/*
 * RISC-V 64-bit (riscv64) stack unwinding.
 *
 * The RISC-V ELF psABI frame pointer is s0/x8.  With a standard frame
 * (requires -fno-omit-frame-pointer; the AAL requests it via
 * --enable-frame-pointers, like the x86_64 walk above), s0 points just
 * ABOVE the saved register area, so the saved return address is at s0[-1]
 * (fp-8) and the previous frame pointer is at s0[-2] (fp-16).  This differs
 * from x86/aarch64, where the pair sits at fp[0]/fp[1].
 *
 * The P5.9 stack-bounds guard still applies: a wild or data-holding s0 (a
 * caller built without frame pointers) must never fault the allocator.
 * umem_frame_readable() bounds [fp-16, fp) against this thread's stack.
 */
#define UMEM_HAVE_REAL_PCSTACK 1

/*ARGSUSED*/
int
getpcstack(uintptr_t *pcstack, int pcstack_limit, int check_sigthread)
{
	uintptr_t *fp;
	uintptr_t *nextfp;
	uintptr_t fp_ceiling, stk_lo = 0, stk_hi = 0;
	int have_bounds;
	int depth = 0;
	uintptr_t first_internal = 0;

	if (check_sigthread)
		return (0);		/* not safe from a signal handler */

	/* Current frame pointer (s0/x8). */
	__asm__ __volatile__("mv %0, s0" : "=r" (fp));

	/*
	 * Real stack bounds where the system will tell us (P5.9); the 16 MiB
	 * span above the starting frame is only the fallback.
	 */
	have_bounds = umem_stack_bounds(&stk_lo, &stk_hi);
	fp_ceiling = (uintptr_t)fp + (16 * 1024 * 1024);

	while (depth < pcstack_limit && fp != NULL) {
		/* frame pointers are pointer-aligned */
		if ((uintptr_t)fp & (sizeof (uintptr_t) - 1))
			break;

		/*
		 * The RISC-V pair sits BELOW fp (at fp-16/fp-8), so the
		 * readable range is [fp-16, fp).  Reject a frame whose
		 * saved-register slots fall outside this thread's stack.
		 */
		if ((uintptr_t)fp < 2 * sizeof (uintptr_t))
			break;
		if (!umem_frame_readable((uintptr_t)fp - 2 * sizeof (uintptr_t),
		    have_bounds, stk_lo, stk_hi, fp_ceiling))
			break;

		nextfp = (uintptr_t *)(fp[-2]);	/* saved frame pointer */
		/* return address at fp[-1]; leading libumem frames dropped */
		if (depth > 0 || !umem_pc_internal(fp[-1]))
			pcstack[depth++] = fp[-1];
		else if (first_internal == 0)
			first_internal = fp[-1];

		/* stack grows down: a valid caller frame is higher */
		if (nextfp <= fp)
			break;
		fp = nextfp;
	}

	/*
	 * The walk never left libumem (built without frame pointers: s0 holds
	 * data after the first frame).  Keep that one frame rather than an
	 * empty stack; findleaks/leaks print why it is only one.
	 */
	if (depth == 0 && first_internal != 0 && pcstack_limit > 0)
		pcstack[depth++] = first_internal;

	return (depth);
}

#elif defined(__linux__) && defined(__arm__)
/*
 * ARM 32-bit (armhf, AAPCS) stack unwinding.
 *
 * AAPCS leaves the frame-pointer register ABI- and mode-dependent: GCC uses
 * r11 for ARM code and r7 for Thumb-2.  With -fno-omit-frame-pointer (the
 * AAL requests it via --enable-frame-pointers, like the x86_64 walk above)
 * GCC emits the APCS-style frame on ARM where the frame-pointer register
 * points at a slot holding the saved fp, and the saved return address (lr)
 * is in the adjacent higher slot.  We read the pair at fp[0]/fp[1] and treat
 * fp as pointing at { saved_fp, saved_lr } -- the common AAPCS case.
 *
 * This is a heuristic shape (ARM has never had one mandated frame layout),
 * so the P5.9 stack-bounds guard is load-bearing here: a wild or
 * data-holding fp (a caller built without frame pointers, or the "wrong"
 * fp register for this TU's mode) must never fault the allocator.  Every
 * dereference is bounds-checked against this thread's stack and the chain
 * must strictly increase; a frame that fails either check simply stops the
 * walk instead of reading hostile memory.
 */
#define UMEM_HAVE_REAL_PCSTACK 1

/*ARGSUSED*/
int
getpcstack(uintptr_t *pcstack, int pcstack_limit, int check_sigthread)
{
	uintptr_t *fp;
	uintptr_t *nextfp;
	uintptr_t fp_ceiling, stk_lo = 0, stk_hi = 0;
	int have_bounds;
	int depth = 0;
	uintptr_t first_internal = 0;

	if (check_sigthread)
		return (0);		/* not safe from a signal handler */

	/*
	 * Current frame pointer: r7 in Thumb, r11 in ARM.  __thumb__ is
	 * defined when this TU is compiled as Thumb (the armhf default on
	 * Debian), so pick the register the compiler actually used.
	 */
#if defined(__thumb__)
	__asm__ __volatile__("mov %0, r7" : "=r" (fp));
#else
	__asm__ __volatile__("mov %0, r11" : "=r" (fp));
#endif

	/*
	 * Real stack bounds where the system will tell us (P5.9); the 16 MiB
	 * span above the starting frame is only the fallback.
	 */
	have_bounds = umem_stack_bounds(&stk_lo, &stk_hi);
	fp_ceiling = (uintptr_t)fp + (16 * 1024 * 1024);

	while (depth < pcstack_limit && fp != NULL) {
		/* frame pointers are pointer-aligned */
		if ((uintptr_t)fp & (sizeof (uintptr_t) - 1))
			break;

		/* Never dereference a frame outside this thread's stack */
		if (!umem_frame_readable((uintptr_t)fp, have_bounds,
		    stk_lo, stk_hi, fp_ceiling))
			break;

		nextfp = (uintptr_t *)(fp[0]);	/* saved frame pointer */
		/* saved lr at fp[1]; leading libumem frames dropped */
		if (depth > 0 || !umem_pc_internal(fp[1]))
			pcstack[depth++] = fp[1];
		else if (first_internal == 0)
			first_internal = fp[1];

		/* stack grows down: a valid caller frame is higher */
		if (nextfp <= fp)
			break;
		fp = nextfp;
	}

	/*
	 * The walk never left libumem (built without frame pointers: the fp
	 * register holds data after the first frame).  Keep that one frame
	 * rather than an empty stack; findleaks/leaks print why it is only one.
	 */
	if (depth == 0 && first_internal != 0 && pcstack_limit > 0)
		pcstack[depth++] = first_internal;

	return (depth);
}

#elif defined(__MACH__)
/*
 * Darwin doesn't have any exposed frame info, so give it some space.
 */
#define UMEM_FRAMESIZE (2 * sizeof(long long))

#elif (defined(__sparc) || defined(__sparcv9)) && HAVE_SYS_STACK_H
extern void flush_windows(void);
#define	UMEM_FRAMESIZE	MINFRAME

#elif defined(__i386) || defined(__amd64)
/*
 * On x86, MINFRAME is defined to be 0, but we want to be sure we can
 * dereference the entire frame structure.
 */
#define	UMEM_FRAMESIZE	(sizeof (struct frame))

#elif defined(__aarch64__) || defined(__arm64__)
/*
 * ARM64 uses direct frame pointer walking, frame size is 16 bytes
 * (previous fp + return address, both 64-bit)
 */
#define	UMEM_FRAMESIZE	(2 * sizeof(void *))

#elif !defined(EC_UMEM_DUMMY_PCSTACK)
#error needs update for new architecture
#endif

#if !defined(UMEM_HAVE_REAL_PCSTACK) && \
    !(defined(__aarch64__) || defined(__arm64__))
/*
 * Get a pc-only stacktrace.  Used for kmem_alloc() buffer ownership tracking.
 * Returns MIN(current stack depth, pcstack_limit).
 *
 * Note: ARM64 has its own implementation above.
 */
/*ARGSUSED*/
int
getpcstack(uintptr_t *pcstack, int pcstack_limit, int check_signal)
{
#ifdef EC_UMEM_DUMMY_PCSTACK
	(void) check_signal;
	if (pcstack_limit <= 0)
		return (0);
#if defined(HAVE_BACKTRACE)
	/*
	 * Fallback for arches without a dedicated frame-pointer walk above
	 * (x86 and aarch64 have their own UMEM_HAVE_REAL_PCSTACK paths).
	 * Uses backtrace(3) with dlopen-recursion guards.
	 */
	{
		extern int backtrace(void **, int);
		/*
		 * backtrace(3) lazily dlopens libgcc_s on first use; the
		 * dlopen path calls malloc, which (when libumem interposes)
		 * re-enters the allocator and back into getpcstack.  Guard
		 * with a TLS re-entry flag, a process-wide "warmed" flag
		 * (set by umem_stacktrace_init via a non-allocator path),
		 * and an interposing flag.
		 */
		static __thread int in_backtrace = 0;
		extern int umem_backtrace_warmed;
		extern int umem_malloc_is_interposing;

		if (in_backtrace)
			return (0);
		if (!umem_backtrace_warmed)
			return (0);
		if (umem_malloc_is_interposing)
			return (0);
		in_backtrace = 1;

		void *raw[64];
		int cap = pcstack_limit + 2;
		if (cap > (int)(sizeof (raw) / sizeof (raw[0])))
			cap = (int)(sizeof (raw) / sizeof (raw[0]));
		int got = backtrace(raw, cap);

		in_backtrace = 0;

		if (got <= 2)
			return (0);
		/* skip getpcstack and its caller as before, then any further
		 * leading libumem pcs */
		int i = 2;
		int out = 0;
		while (i < got && umem_pc_internal((uintptr_t)raw[i]))
			i++;
		for (; i < got && out < pcstack_limit; i++)
			pcstack[out++] = (uintptr_t)raw[i];
		return (out);
	}
#else
	(void) pcstack;
	(void) pcstack_limit;
	return (0);
#endif
#else
	struct frame *fp;
	struct frame *nextfp, *minfp;
	int depth = 0;
	uintptr_t base = 0;
	size_t size = 0;
#ifndef UMEM_STANDALONE
	int on_altstack = 0;
	uintptr_t sigbase = 0;
	size_t sigsize = 0;

	stack_t st;

	if (stack_getbounds(&st) != 0) {
		if (thr_stksegment(&st) != 0 ||
		    (uintptr_t)st.ss_sp < st.ss_size) {
			return (0);		/* unable to get stack bounds */
		}
		/*
		 * thr_stksegment(3C) has a slightly different interface than
		 * stack_getbounds(3C) -- correct it
		 */
		st.ss_sp = (void *)(((uintptr_t)st.ss_sp) - st.ss_size);
		st.ss_flags = 0;		/* can't be on-stack */
	}
	on_altstack = (st.ss_flags & SS_ONSTACK);

	if (st.ss_size != 0) {
		base = (uintptr_t)st.ss_sp;
		size = st.ss_size;
	} else {
		/*
		 * If size == 0, then ss_sp is the *top* of the stack.
		 *
		 * Since we only allow increasing frame pointers, and we
		 * know our caller set his up correctly, we can treat ss_sp
		 * as an upper bound safely.
		 */
		base = 0;
		size = (uintptr_t)st.ss_sp;
	}

	if (check_signal != 0) {
		void (*sigfunc)() = NULL;
		int sigfuncsize = 0;
		extern void thr_sighndlrinfo(void (**)(), int *);

		thr_sighndlrinfo(&sigfunc, &sigfuncsize);
		sigbase = (uintptr_t)sigfunc;
		sigsize = sigfuncsize;
	}
#else /* UMEM_STANDALONE */
	base = (uintptr_t)umem_min_stack;
	size = umem_max_stack - umem_min_stack;
#endif

	/*
	 * shorten size so that fr_savfp and fr_savpc will be within the stack
	 * bounds.
	 */
	if (size >= UMEM_FRAMESIZE - 1)
		size -= (UMEM_FRAMESIZE - 1);
	else
		size = 0;

#if defined(__sparc) || defined(__sparcv9)
	flush_windows();
#endif

	/* LINTED alignment */
	fp = (struct frame *)((caddr_t)getfp() + STACK_BIAS);

	minfp = fp;

	if (((uintptr_t)fp - base) >= size)
		return (0);	/* the frame pointer isn't in our stack */

	while (depth < pcstack_limit) {
		uintptr_t tmp;

		/* LINTED alignment */
		nextfp = (struct frame *)((caddr_t)fp->fr_savfp + STACK_BIAS);
		tmp = (uintptr_t)nextfp;

		/*
		 * Check nextfp for validity.  It must be properly aligned,
		 * increasing compared to the last %fp (or the top of the
		 * stack we just switched to), and it must be inside
		 * [base, base + size).
		 */
		if (tmp != SA(tmp))
			break;
		else if (nextfp <= minfp || (tmp - base) >= size) {
#ifndef UMEM_STANDALONE
			if (tmp == NULL || !on_altstack)
				break;
			/*
			 * If we're on an alternate signal stack, try jumping
			 * to the main thread stack.
			 *
			 * If the main thread stack has an unlimited size, we
			 * punt, since we don't know where the frame pointer's
			 * been.
			 *
			 * (thr_stksegment() returns the *top of stack*
			 * in ss_sp, not the bottom)
			 */
			if (thr_stksegment(&st) == 0) {
				if (st.ss_size >= (uintptr_t)st.ss_sp ||
				    st.ss_size < UMEM_FRAMESIZE - 1)
					break;

				on_altstack = 0;
				base = (uintptr_t)st.ss_sp - st.ss_size;
				size = st.ss_size - (UMEM_FRAMESIZE - 1);
				minfp = (struct frame *)base;
				continue;		/* try again */
			}
#endif
			break;
		}

#ifndef UMEM_STANDALONE
		if (check_signal && (fp->fr_savpc - sigbase) <= sigsize)
			umem_panic("called from signal handler");
#endif
		pcstack[depth++] = fp->fr_savpc;
		fp = nextfp;
		minfp = fp;
	}
	return (depth);
#endif
}
#endif /* !(defined(__aarch64__) || defined(__arm64__)) */
