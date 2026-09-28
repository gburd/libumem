stp	x29, x30, [sp, #-112]!
mov	x29, sp
stp	x25, x26, [sp, #64]
adrp	x25, <__FRAME_END__>
ldr	x26, [x25, #3344]
stp	x19, x20, [sp, #16]
sub	x19, x0, #0x1
adrp	x20, <bootstrap_ptrs>
lsr	x19, x19, #3
add	x20, x20, #0xe38
stp	x21, x22, [sp, #32]
mov	w21, w1
mov	x22, #0x3fff                	// #16383
stp	x23, x24, [sp, #48]
mov	x23, x0
cmp	x19, x22
b.hi	<_umem_alloc>  // b.pmore
adrp	x1, <__FRAME_END__>
adrp	x0, <__FRAME_END__>
ldr	x1, [x1, #3816]
ldr	x0, [x0, #3528]
ldrsb	x24, [x1, x19]
ldr	x4, [x0, x19, lsl #3]
tbnz	w24, #31, <_umem_alloc>
adrp	x0, <__FRAME_END__>
ldr	x0, [x0, #3904]
nop
nop
stp	x27, x28, [sp, #80]
mrs	x27, tpidr_el0
ldr	x28, [x27, x0]
cbz	x28, <_umem_alloc>
sbfiz	x2, x24, #6, #32
add	x2, x28, x2
ldrh	w1, [x2, #8]
cbz	w1, <_umem_alloc>
ldr	x4, [x2]
sub	w1, w1, #0x1
adrp	x3, <umem_profile_spec>
and	x5, x1, #0xffff
add	x0, x4, w5, uxth #3
ldp	x19, x20, [sp, #16]
strh	w5, [x2, #8]
ldr	x1, [x4, x5, lsl #3]
ldp	x21, x22, [sp, #32]
ldp	x23, x24, [sp, #48]
eor	x0, x1, x0, lsr #12
ldp	x25, x26, [sp, #64]
ldp	x27, x28, [sp, #80]
ldr	x3, [x3, #464]
ldp	x29, x30, [sp], #112
eor	x0, x0, x3
ret
cbz	x23, <_umem_alloc>
ldr	x0, [x26]
cbz	x0, <_umem_alloc>
ldr	w0, [x20]
cbnz	w0, <_umem_alloc>
ldr	x0, [x25, #3344]
mov	w2, #0x1                   	// #1
mov	x1, x23
ldr	x0, [x0]
bl	<vmem_alloc@plt>
cbz	x0, <_umem_alloc>
ldp	x19, x20, [sp, #16]
ldp	x21, x22, [sp, #32]
ldp	x23, x24, [sp, #48]
ldp	x25, x26, [sp, #64]
ldp	x29, x30, [sp], #112
ret
adrp	x0, <memcpy@GLIBC_2.17>
ldr	x1, [x0, #1992]
add	x0, x0, #0x7c8
blr	x1
add	x28, x27, x0
ldr	w0, [x28, #4]
cbz	w0, <_umem_alloc>
ldp	x27, x28, [sp, #80]
mov	w1, w21
mov	x0, x4
str	x4, [sp, #96]
bl	<_umem_cache_alloc@plt>
ldr	x4, [sp, #96]
ldr	w1, [x4, #188]
tst	w1, #0x6
b.ne	<_umem_alloc>  // b.any
cbnz	x0, <_umem_alloc>
mov	x0, x4
mov	w1, w21
bl	<umem_alloc_retry>
cbnz	w0, <_umem_alloc>
mov	x0, #0x0                   	// #0
b	<_umem_alloc>
ldr	w0, [x20]
cbz	w0, <_umem_alloc>
mov	w1, w21
mov	x0, x23
ldp	x19, x20, [sp, #16]
ldp	x21, x22, [sp, #32]
ldp	x23, x24, [sp, #48]
ldp	x25, x26, [sp, #64]
ldp	x29, x30, [sp], #112
b	<umem_alloc_hooked>
bl	<umem_init@plt>
cbz	w0, <_umem_alloc>
ldr	x0, [x26]
cbnz	x0, <_umem_alloc>
adrp	x1, <_fini>
adrp	x0, <_fini>
stp	x27, x28, [sp, #80]
add	x1, x1, #0xd68
add	x0, x0, #0x4b0
mov	w2, #0x11e5                	// #4581
bl	<__umem_assert_failed@plt>
adrp	x0, <__FRAME_END__>
mov	x1, #0x0                   	// #0
ldr	x0, [x0, #3992]
mov	x3, x23
mov	x2, #0x0                   	// #0
ldr	x0, [x0]
bl	<umem_log_event>
mov	w1, w21
mov	x0, #0x0                   	// #0
bl	<umem_alloc_retry>
cbnz	w0, <_umem_alloc>
mov	x0, #0x0                   	// #0
b	<_umem_alloc>
mov	w0, #0x1                   	// #1
str	x4, [sp, #96]
str	w0, [x28, #4]
bl	<umem_ptc_get@plt>
str	wzr, [x28, #4]
mov	x28, x0
ldr	x4, [sp, #96]
cbz	x0, <_umem_alloc>
b	<_umem_alloc>
add	x0, x24, x24, lsl #2
add	x0, x28, x0, lsl #3
ldr	w2, [x0, #2320]
cmp	w2, #0x0
b.gt	<_umem_alloc>
ldr	w1, [x0, #2324]
cmp	w1, #0x0
b.gt	<_umem_alloc>
ldr	x1, [x0, #2336]
cbz	x1, <_umem_alloc>
adrp	x0, <__FRAME_END__>
ldr	x0, [x0, #3480]
ldr	w0, [x0]
cbz	w0, <_umem_alloc>
adrp	x0, <__FRAME_END__>
ldr	x0, [x0, #4024]
ldr	w0, [x0]
cbz	w0, <_umem_alloc>
ldr	x0, [x4, #840]
cbz	x0, <_umem_alloc>
adrp	x0, <memcpy@GLIBC_2.17>
ldr	x1, [x0, #2024]
add	x0, x0, #0x7e8
blr	x1
ldr	w0, [x27, x0]
cbz	w0, <_umem_alloc>
adrp	x0, <memcpy@GLIBC_2.17>
ldr	x1, [x0, #2040]
add	x0, x0, #0x7f8
blr	x1
ldr	x0, [x27, x0]
cbz	x0, <_umem_alloc>
ldr	w27, [x0]
tbnz	w27, #31, <_umem_alloc>
str	x4, [sp, #96]
bl	<umem_rseq_get_ncpus@plt>
cmp	w27, w0
ldr	x4, [sp, #96]
b.ge	<_umem_alloc>  // b.tcont
ldr	x3, [x4, #840]
sbfiz	x0, x27, #6, #32
mov	w1, w27
add	x3, x3, x0
mov	x0, x3
stp	x3, x4, [sp, #96]
bl	<umem_rseq_alloc_fastpath@plt>
ldp	x3, x4, [sp, #96]
cbz	x0, <_umem_alloc>
ldp	x27, x28, [sp, #80]
b	<_umem_alloc>
mov	x0, x4
mov	w1, w27
stp	x4, x3, [sp, #96]
bl	<umem_rseq_alloc_slowpath>
ldr	x4, [sp, #96]
cbz	w0, <_umem_alloc>
mov	w1, w27
ldr	x3, [sp, #104]
mov	x0, x3
bl	<umem_rseq_alloc_fastpath@plt>
ldr	x4, [sp, #96]
cbnz	x0, <_umem_alloc>
add	x1, x4, #0x200
mov	x0, x4
str	x4, [sp, #96]
bl	<umem_depot_alloc_trylock>
mov	x27, x0
ldr	x4, [sp, #96]
cbz	x0, <_umem_alloc>
mov	x0, #0x28                  	// #40
mov	w1, #0x1                   	// #1
madd	x0, x24, x0, x28
str	w1, [x28, #3744]
ldr	x1, [x0, #2304]
cbz	x1, <_umem_alloc>
ldr	x2, [x0, #2312]
cbz	x2, <_umem_alloc>
ldr	w3, [x0, #2324]
add	x1, x4, #0x280
mov	x0, x4
bl	<umem_ptc_mag_return_trylock>
mov	x0, #0x28                  	// #40
madd	x0, x24, x0, x28
ldr	w2, [x0, #2320]
ldr	w1, [x0, #2328]
ldr	x3, [x0, #2304]
str	w2, [x0, #2324]
str	w1, [x0, #2332]
str	x3, [x0, #2312]
adrp	x3, <__FRAME_END__>
mov	x4, #0xffffffffffffffb8    	// #-72
ldr	x5, [x3, #3976]
mov	x1, #0x28                  	// #40
madd	x1, x24, x1, x28
ldr	x0, [x5]
neg	x0, x0
bic	x0, x0, x27
sub	x0, x4, x0
ldr	x0, [x0]
ldr	x0, [x0, #128]
str	x27, [x1, #2304]
lsr	x0, x0, #3
sub	w0, w0, #0x1
str	w0, [x1, #2320]
str	w0, [x1, #2328]
add	x0, x28, #0xea0
stlr	wzr, [x0]
ldr	x2, [x1, #2304]
cbz	x2, <_umem_alloc>
ldr	x0, [x5]
ldr	w1, [x1, #2328]
neg	x0, x0
bic	x0, x0, x2
sub	x4, x4, x0
ldr	x0, [x4]
ldr	x0, [x0, #128]
lsr	x0, x0, #3
sub	w0, w0, #0x1
cmp	w1, w0
b.ne	<_umem_alloc>  // b.any
mov	x1, #0x28                  	// #40
madd	x1, x24, x1, x28
ldr	x0, [x1, #2312]
cbz	x0, <_umem_alloc>
ldr	x3, [x3, #3976]
mov	x4, #0xffffffffffffffb8    	// #-72
ldr	w5, [x1, #2332]
ldr	x1, [x3]
neg	x1, x1
bic	x0, x1, x0
sub	x0, x4, x0
ldr	x0, [x0]
ldr	x0, [x0, #128]
lsr	x0, x0, #3
sub	w0, w0, #0x1
cmp	w5, w0
b.ne	<_umem_alloc>  // b.any
mov	x0, #0x28                  	// #40
madd	x24, x24, x0, x28
ldr	w0, [x24, #2320]
tbnz	w0, #31, <_umem_alloc>
ldr	w1, [x24, #2328]
cmp	w0, w1
b.gt	<_umem_alloc>
ldr	w1, [x24, #2324]
tbnz	w1, #31, <_umem_alloc>
ldr	w3, [x24, #2332]
cmp	w1, w3
b.gt	<_umem_alloc>
sub	w3, w0, #0x1
adrp	x1, <umem_profile_spec>
add	x5, x2, w3, sxtw #3
ldr	x4, [x1, #464]
add	x0, x2, w0, sxtw #3
ldp	x27, x28, [sp, #80]
str	w3, [x24, #2320]
ldr	x1, [x5, #8]
eor	x1, x1, x4
eor	x0, x1, x0, lsr #12
b	<_umem_alloc>
cbz	x0, <_umem_alloc>
mov	w1, #0xfb                  	// #251
ldr	x2, [x4, #312]
mov	w3, #0xffffffbb            	// #-69
mul	w1, w23, w1
strb	w3, [x0, x23]
add	w1, w1, #0x1
add	x2, x0, x2
str	w1, [x2, #4]
b	<_umem_alloc>
str	x4, [x0, #2336]
b	<_umem_alloc>
str	x4, [sp, #96]
bl	<umem_rseq_register_thread@plt>
ldr	x4, [sp, #96]
b	<_umem_alloc>
mov	x3, #0x28                  	// #40
mov	w0, #0x1                   	// #1
madd	x3, x24, x3, x28
str	w0, [x28, #3744]
ldr	x0, [x3, #2304]
str	w1, [x3, #2320]
ldr	w1, [x3, #2328]
str	w2, [x3, #2324]
ldr	x4, [x3, #2312]
str	x0, [x3, #2312]
ldr	w0, [x3, #2332]
str	w1, [x3, #2332]
str	x4, [x3, #2304]
str	w0, [x3, #2328]
add	x28, x28, #0xea0
stlr	wzr, [x28]
ldr	w1, [x3, #2320]
adrp	x0, <umem_profile_spec>
ldp	x27, x28, [sp, #80]
ldr	x4, [x0, #464]
ldr	x2, [x3, #2304]
add	x0, x2, w1, sxtw #3
sub	w1, w1, #0x1
add	x2, x2, w1, sxtw #3
ldr	x2, [x2, #8]
str	w1, [x3, #2320]
eor	x1, x2, x4
eor	x0, x1, x0, lsr #12
b	<_umem_alloc>
mov	x1, #0x28                  	// #40
sub	w3, w2, #0x1
adrp	x0, <umem_profile_spec>
madd	x1, x24, x1, x28
ldr	x4, [x0, #464]
ldp	x27, x28, [sp, #80]
str	w3, [x1, #2320]
ldr	x0, [x1, #2304]
add	x5, x0, w3, sxtw #3
add	x0, x0, w2, sxtw #3
ldr	x1, [x5, #8]
eor	x1, x1, x4
eor	x0, x1, x0, lsr #12
b	<_umem_alloc>
adrp	x1, <_fini>
adrp	x0, <_fini>
add	x1, x1, #0xd68
add	x0, x0, #0x3b0
mov	w2, #0xa9b                 	// #2715
bl	<__umem_assert_failed@plt>
adrp	x1, <_fini>
adrp	x0, <_fini>
add	x1, x1, #0xd68
add	x0, x0, #0x3f8
mov	w2, #0xa9d                 	// #2717
bl	<__umem_assert_failed@plt>
adrp	x1, <_fini>
adrp	x0, <_fini>
add	x1, x1, #0xd68
add	x0, x0, #0x478
mov	w2, #0xaa0                 	// #2720
bl	<__umem_assert_failed@plt>
adrp	x1, <_fini>
adrp	x0, <_fini>
add	x1, x1, #0xd68
add	x0, x0, #0x448
mov	w2, #0xa9f                 	// #2719
bl	<__umem_assert_failed@plt>
