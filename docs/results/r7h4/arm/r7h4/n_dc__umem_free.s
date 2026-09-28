cbz	x0, <_umem_free>
stp	x29, x30, [sp, #-96]!
sub	x3, x1, #0x1
mov	x29, sp
lsr	x3, x3, #3
stp	x19, x20, [sp, #16]
mov	x19, x0
mov	x0, #0x3fff                	// #16383
mov	x20, x1
cmp	x3, x0
b.hi	<_umem_free>  // b.pmore
adrp	x1, <__FRAME_END__>
adrp	x0, <__FRAME_END__>
ldr	x1, [x1, #3816]
stp	x21, x22, [sp, #32]
ldr	x0, [x0, #3528]
stp	x23, x24, [sp, #48]
ldrsb	w21, [x1, x3]
ldr	x24, [x0, x3, lsl #3]
tbnz	w21, #31, <_umem_free>
mrs	x23, tpidr_el0
stp	x25, x26, [sp, #64]
adrp	x0, <__FRAME_END__>
ldr	x0, [x0, #3904]
nop
nop
ldr	x22, [x23, x0]
cbz	x22, <_umem_free>
sbfiz	x1, x21, #6, #32
sxtw	x25, w21
add	x1, x22, x1
mov	w3, #0x80                  	// #128
ldrh	w2, [x1, #8]
cmp	w21, #0xc
b.le	<_umem_free>
mov	w3, #0x40                  	// #64
cmp	w21, #0x14
b.le	<_umem_free>
cmp	w21, #0x1b
mov	w3, #0x10                  	// #16
mov	w0, #0x20                  	// #32
csel	w3, w3, w0, gt
cmp	w2, w3
b.ge	<_umem_free>  // b.tcont
adrp	x3, <umem_profile_spec>
add	w5, w2, #0x1
ldr	x4, [x1]
add	x0, x4, w2, uxth #3
ldr	x3, [x3, #464]
strh	w5, [x1, #8]
eor	x1, x19, x3
eor	x0, x1, x0, lsr #12
str	x0, [x4, x2, lsl #3]
ldp	x19, x20, [sp, #16]
ldp	x21, x22, [sp, #32]
ldp	x23, x24, [sp, #48]
ldp	x25, x26, [sp, #64]
ldp	x29, x30, [sp], #96
ret
adrp	x0, <bootstrap_ptrs>
add	x0, x0, #0xe38
ldr	w0, [x0]
cbnz	w0, <_umem_free>
adrp	x0, <__FRAME_END__>
mov	x2, x1
ldr	x0, [x0, #3344]
mov	x1, x19
ldp	x19, x20, [sp, #16]
ldp	x29, x30, [sp], #96
ldr	x0, [x0]
b	<vmem_free@plt>
adrp	x0, <memcpy@GLIBC_2.17>
ldr	x1, [x0, #1992]
add	x0, x0, #0x7c8
blr	x1
ldr	w1, [x23, x0]
add	x25, x23, x0
cbz	w1, <_umem_free>
ldp	x25, x26, [sp, #64]
ldr	w0, [x24, #188]
tst	w0, #0x6
b.ne	<_umem_free>  // b.any
mov	x1, x19
mov	x0, x24
ldp	x19, x20, [sp, #16]
ldp	x21, x22, [sp, #32]
ldp	x23, x24, [sp, #48]
ldp	x29, x30, [sp], #96
b	<_umem_cache_free@plt>
adrp	x0, <bootstrap_ptrs>
add	x0, x0, #0xe38
ldr	w0, [x0]
cbz	w0, <_umem_free>
mov	x2, x20
mov	x1, x19
ldp	x19, x20, [sp, #16]
mov	x0, x24
ldp	x21, x22, [sp, #32]
ldp	x23, x24, [sp, #48]
ldp	x29, x30, [sp], #96
b	<umem_free_hooked>
ret
mov	w1, #0x1                   	// #1
str	w1, [x23, x0]
bl	<umem_ptc_get@plt>
str	wzr, [x25]
mov	x22, x0
cbz	x0, <_umem_free>
b	<_umem_free>
add	x0, x25, x25, lsl #2
add	x0, x22, x0, lsl #3
ldr	x1, [x0, #2336]
cbz	x1, <_umem_free>
add	x0, x25, x25, lsl #2
add	x0, x22, x0, lsl #3
ldr	x1, [x0, #2304]
cbz	x1, <_umem_free>
ldr	w2, [x0, #2320]
ldr	w3, [x0, #2328]
cmp	w2, w3
b.lt	<_umem_free>  // b.tstop
add	x0, x25, x25, lsl #2
add	x0, x22, x0, lsl #3
ldr	x2, [x0, #2312]
cbz	x2, <_umem_free>
ldr	w4, [x0, #2324]
ldr	w3, [x0, #2332]
cmp	w4, w3
b.lt	<_umem_free>  // b.tstop
adrp	x0, <__FRAME_END__>
ldr	x0, [x0, #3480]
ldr	w0, [x0]
cbz	w0, <_umem_free>
adrp	x0, <__FRAME_END__>
ldr	x0, [x0, #4024]
ldr	w0, [x0]
cbz	w0, <_umem_free>
ldr	x0, [x24, #840]
cbz	x0, <_umem_free>
adrp	x0, <memcpy@GLIBC_2.17>
ldr	x1, [x0, #2024]
add	x0, x0, #0x7e8
blr	x1
ldr	w0, [x23, x0]
cbz	w0, <_umem_free>
adrp	x0, <memcpy@GLIBC_2.17>
ldr	x1, [x0, #2040]
add	x0, x0, #0x7f8
blr	x1
ldr	x0, [x23, x0]
cbz	x0, <_umem_free>
ldr	w21, [x0]
tbnz	w21, #31, <_umem_free>
bl	<umem_rseq_get_ncpus@plt>
add	x23, x24, #0x280
cmp	w21, w0
b.ge	<_umem_free>  // b.tcont
ldr	x26, [x24, #840]
sbfiz	x0, x21, #6, #32
mov	w2, w21
mov	x1, x19
add	x26, x26, x0
mov	x0, x26
bl	<umem_rseq_free_fastpath@plt>
cbz	w0, <_umem_free>
mov	x1, x23
mov	x0, x24
str	x27, [sp, #80]
bl	<umem_depot_alloc>
mov	x27, x0
cbz	x0, <_umem_free>
mov	x2, x0
mov	w3, #0x0                   	// #0
mov	w1, w21
mov	x0, x24
bl	<umem_rseq_reload>
cbnz	w0, <_umem_free>
mov	x2, x27
mov	x1, x23
mov	x0, x24
bl	<umem_depot_free>
ldr	x27, [sp, #80]
nop
nop
add	x21, x25, x25, lsl #2
mov	w0, #0x1                   	// #1
add	x21, x22, x21, lsl #3
str	w0, [x22, #3744]
ldr	x2, [x21, #2304]
cbz	x2, <_umem_free>
ldr	w3, [x21, #2320]
add	x1, x24, #0x200
mov	x0, x24
bl	<umem_ptc_mag_return_trylock>
str	xzr, [x21, #2304]
str	wzr, [x21, #2320]
mov	x1, x23
mov	x0, x24
bl	<umem_depot_alloc_trylock>
add	x21, x22, #0xea0
cbz	x0, <_umem_free>
adrp	x4, <__FRAME_END__>
add	x1, x25, x25, lsl #2
mov	x5, #0xffffffffffffffb8    	// #-72
ldr	x6, [x4, #3976]
add	x1, x22, x1, lsl #3
str	x0, [x1, #2304]
ldr	x2, [x6]
neg	x2, x2
bic	x0, x2, x0
sub	x0, x5, x0
ldr	x0, [x0]
ldr	x0, [x0, #128]
str	wzr, [x1, #2320]
lsr	x0, x0, #3
sub	w0, w0, #0x1
str	w0, [x1, #2328]
stlr	wzr, [x21]
ldr	x3, [x1, #2304]
cbz	x3, <_umem_free>
ldr	x0, [x6]
ldr	w1, [x1, #2328]
neg	x0, x0
bic	x0, x0, x3
sub	x5, x5, x0
ldr	x0, [x5]
ldr	x0, [x0, #128]
lsr	x0, x0, #3
sub	w0, w0, #0x1
cmp	w1, w0
b.ne	<_umem_free>  // b.any
add	x0, x25, x25, lsl #2
add	x0, x22, x0, lsl #3
ldr	x1, [x0, #2312]
cbz	x1, <_umem_free>
ldr	x4, [x4, #3976]
mov	x2, #0xffffffffffffffb8    	// #-72
ldr	w5, [x0, #2332]
ldr	x0, [x4]
neg	x0, x0
bic	x1, x0, x1
sub	x0, x2, x1
ldr	x0, [x0]
ldr	x0, [x0, #128]
lsr	x0, x0, #3
sub	w0, w0, #0x1
cmp	w5, w0
b.ne	<_umem_free>  // b.any
add	x25, x25, x25, lsl #2
add	x0, x22, x25, lsl #3
ldr	w1, [x0, #2320]
tbnz	w1, #31, <_umem_free>
ldr	w2, [x0, #2328]
cmp	w1, w2
b.gt	<_umem_free>
ldr	w2, [x0, #2324]
tbnz	w2, #31, <_umem_free>
ldr	w4, [x0, #2332]
cmp	w2, w4
b.gt	<_umem_free>
adrp	x4, <umem_profile_spec>
add	x2, x3, w1, sxtw #3
add	w5, w1, #0x1
ldr	x4, [x4, #464]
add	x1, x3, w1, sxtw #3
add	x2, x2, #0x8
eor	x3, x19, x4
eor	x2, x3, x2, lsr #12
str	x2, [x1, #8]
str	w5, [x0, #2320]
b	<_umem_free>
add	x23, x24, #0x280
b	<_umem_free>
str	x24, [x0, #2336]
b	<_umem_free>
ldr	x3, [x24, #312]
lsl	x0, x20, #6
sub	x0, x0, x20
lsl	x0, x0, #2
sub	x0, x0, x20
add	x0, x0, #0x1
add	x2, x19, x3
ldr	w1, [x2, #4]
cmp	x0, w1, uxtw
b.eq	<_umem_free>  // b.none
mov	x0, #0xbeef                	// #48879
ldr	x2, [x19]
movk	x0, #0xdead, lsl #16
movk	x0, #0xbeef, lsl #32
movk	x0, #0xdead, lsl #48
cmp	x2, x0
b.eq	<_umem_free>  // b.none
mov	w4, #0xa33                 	// #2611
mov	w2, #0xf5cd                	// #62925
movk	w4, #0x9a02, lsl #16
movk	w2, #0x65fd, lsl #16
mov	w0, #0x197f                	// #6527
madd	w1, w1, w4, w2
movk	w0, #0x105, lsl #16
cmp	w1, w0
b.ls	<_umem_free>  // b.plast
mov	x2, x19
mov	x1, x24
mov	w0, #0x1                   	// #1
ldp	x19, x20, [sp, #16]
ldp	x21, x22, [sp, #32]
ldp	x23, x24, [sp, #48]
ldp	x29, x30, [sp], #96
b	<umem_error>
adrp	x4, <umem_profile_spec>
add	x3, x1, w2, sxtw #3
add	w5, w2, #0x1
ldr	x4, [x4, #464]
add	x2, x1, w2, sxtw #3
add	x3, x3, #0x8
eor	x1, x19, x4
eor	x1, x1, x3, lsr #12
str	x1, [x2, #8]
str	w5, [x0, #2320]
b	<_umem_free>
mov	x0, x24
bl	<umem_ptc_mag_prime>
cbnz	x0, <_umem_free>
stlr	wzr, [x21]
ldp	x25, x26, [sp, #64]
b	<_umem_free>
mov	w5, #0x1                   	// #1
str	w5, [x22, #3744]
ldr	w5, [x0, #2320]
str	x2, [x0, #2304]
ldr	w2, [x0, #2328]
str	x1, [x0, #2312]
str	w4, [x0, #2320]
str	w5, [x0, #2324]
str	w3, [x0, #2328]
str	w2, [x0, #2332]
add	x22, x22, #0xea0
stlr	wzr, [x22]
ldr	w1, [x0, #2320]
adrp	x2, <umem_profile_spec>
ldr	x3, [x2, #464]
ldr	x4, [x0, #2304]
add	x2, x4, w1, sxtw #3
add	x2, x2, #0x8
add	w5, w1, #0x1
add	x1, x4, w1, sxtw #3
eor	x3, x19, x3
eor	x2, x3, x2, lsr #12
str	x2, [x1, #8]
str	w5, [x0, #2320]
b	<_umem_free>
ldrb	w0, [x19, x20]
cmp	w0, #0xbb
b.ne	<_umem_free>  // b.any
mov	w0, #0xface                	// #64206
movk	w0, #0xfeed, lsl #16
str	w0, [x19, x3]
str	w0, [x2, #4]
b	<_umem_free>
mov	w4, #0xfb                  	// #251
mov	x1, x24
mov	x2, x19
mov	w0, #0x7                   	// #7
mul	w4, w20, w4
add	w4, w4, #0x1
str	w4, [x19, x3]
b	<_umem_free>
mov	x2, x19
mov	x1, x24
mov	w0, #0x2                   	// #2
b	<_umem_free>
bl	<umem_rseq_register_thread@plt>
b	<_umem_free>
mov	w2, w21
mov	x0, x26
mov	x1, x19
bl	<umem_rseq_free_fastpath@plt>
ldr	x27, [sp, #80]
cbnz	w0, <_umem_free>
b	<_umem_free>
ldr	x27, [sp, #80]
b	<_umem_free>
adrp	x1, <_fini>
adrp	x0, <_fini>
str	x27, [sp, #80]
add	x1, x1, #0xd68
add	x0, x0, #0x3f8
mov	w2, #0xa9d                 	// #2717
bl	<__umem_assert_failed@plt>
adrp	x1, <_fini>
adrp	x0, <_fini>
str	x27, [sp, #80]
add	x1, x1, #0xd68
add	x0, x0, #0x3b0
mov	w2, #0xa9b                 	// #2715
bl	<__umem_assert_failed@plt>
adrp	x1, <_fini>
adrp	x0, <_fini>
str	x27, [sp, #80]
add	x1, x1, #0xd68
add	x0, x0, #0x478
mov	w2, #0xaa0                 	// #2720
bl	<__umem_assert_failed@plt>
adrp	x1, <_fini>
adrp	x0, <_fini>
str	x27, [sp, #80]
add	x1, x1, #0xd68
add	x0, x0, #0x448
mov	w2, #0xa9f                 	// #2719
bl	<__umem_assert_failed@plt>
mov	x2, x1
mov	x0, #0x0                   	// #0
mov	x1, x19
ldp	x19, x20, [sp, #16]
ldp	x29, x30, [sp], #96
b	<umem_free_hooked>
