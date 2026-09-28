stp	x29, x30, [sp, #-112]!
mov	x29, sp
adrp	x2, <__FRAME_END__>
stp	x21, x22, [sp, #32]
mrs	x21, tpidr_el0
stp	x27, x28, [sp, #80]
mov	x27, x0
adrp	x0, <memcpy@GLIBC_2.17>
ldr	x3, [x0, #2056]
add	x0, x0, #0x808
blr	x3
ldr	x2, [x2, #3856]
stp	x19, x20, [sp, #16]
ldr	w0, [x21, x0]
stp	x23, x24, [sp, #48]
mov	x24, x1
ldr	x19, [x2]
cmn	w0, #0x1
b.eq	<_umem_cache_free>  // b.none
mov	w1, w0
ldr	w0, [x27, #232]
and	w0, w1, w0
lsl	x0, x0, #3
ldr	w19, [x19, x0]
add	x19, x27, x19
cbz	x24, <_umem_cache_free>
ldr	w0, [x19, #76]
mov	w1, #0x2006                	// #8198
tst	w0, w1
b.ne	<_umem_cache_free>  // b.any
adrp	x0, <__FRAME_END__>
ldr	x0, [x0, #3480]
ldr	w0, [x0]
cbz	w0, <_umem_cache_free>
ldr	x0, [x27, #840]
cbz	x0, <_umem_cache_free>
adrp	x0, <memcpy@GLIBC_2.17>
ldr	x1, [x0, #2024]
add	x0, x0, #0x7e8
blr	x1
ldr	w0, [x21, x0]
cbz	w0, <_umem_cache_free>
adrp	x0, <memcpy@GLIBC_2.17>
ldr	x1, [x0, #2040]
add	x0, x0, #0x7f8
blr	x1
ldr	x0, [x21, x0]
cbz	x0, <_umem_cache_free>
ldr	w20, [x0]
tbz	w20, #31, <_umem_cache_free>
mov	x0, x19
bl	<pthread_mutex_lock@plt>
ldr	w1, [x19, #48]
ldr	w2, [x19, #72]
cmp	w1, w2
b.cc	<_umem_cache_free>  // b.lo, b.ul, b.last
add	x22, x27, #0x280
add	x23, x27, #0x340
stp	x25, x26, [sp, #64]
mov	x25, #0x1                   	// #1
mov	x26, #0xffffffffffffffff    	// #-1
b	<_umem_cache_free>
ldp	x5, x3, [x19, #56]
cbz	x5, <_umem_cache_free>
cmp	w1, w2
b.ne	<_umem_cache_free>  // b.any
cmp	w2, #0x0
b.le	<_umem_cache_free>
cbz	x3, <_umem_cache_free>
prfm	pldl1keep, [x3]
mov	w6, #0xffffffff            	// #-1
mov	w4, #0x0                   	// #0
stp	wzr, w1, [x19, #48]
mov	w1, #0x0                   	// #0
stp	x3, x5, [x19, #56]
adrp	x0, <memcpy@GLIBC_2.17>
ldr	x7, [x0, #2056]
add	x0, x0, #0x808
blr	x7
str	w6, [x21, x0]
cmp	w2, w4
b.hi	<_umem_cache_free>  // b.pmore
ldr	w3, [x19, #52]
cbz	w3, <_umem_cache_free>
cbz	w2, <_umem_cache_free>
mov	x1, x22
mov	x0, x27
bl	<umem_depot_alloc>
mov	x20, x0
cbz	x0, <_umem_cache_free>
ldr	x2, [x19, #64]
cbz	x2, <_umem_cache_free>
add	x1, x27, #0x200
mov	x0, x27
bl	<umem_depot_free>
ldr	x2, [x19, #56]
ldr	w0, [x19, #48]
cbz	x2, <_umem_cache_free>
ldr	w1, [x19, #72]
cmp	w0, w1
b.ne	<_umem_cache_free>  // b.any
cmp	w1, #0x0
b.le	<_umem_cache_free>
ldr	x1, [x19, #64]
cbz	x1, <_umem_cache_free>
prfm	pldl1keep, [x1]
mov	w3, #0xffffffff            	// #-1
mov	w1, #0x0                   	// #0
ldp	x25, x26, [sp, #64]
str	w0, [x19, #52]
adrp	x0, <memcpy@GLIBC_2.17>
ldr	x4, [x0, #2056]
add	x0, x0, #0x808
blr	x4
str	w3, [x21, x0]
stp	x20, x2, [x19, #56]
adrp	x0, <umem_profile_spec>
add	w6, w1, #0x1
ldr	x4, [x0, #464]
mov	x0, x19
ldp	x21, x22, [sp, #32]
ldp	x27, x28, [sp, #80]
eor	x4, x24, x4
ldp	x23, x24, [sp, #48]
ldr	w3, [x19, #84]
ldr	x5, [x19, #56]
add	x2, x5, w1, sxtw #3
add	x2, x2, #0x8
eor	x2, x4, x2, lsr #12
add	x1, x5, w1, sxtw #3
add	w3, w3, #0x1
prfm	pldl1keep, [x5]
str	w6, [x19, #48]
str	x2, [x1, #8]
str	w3, [x19, #84]
ldp	x19, x20, [sp, #16]
ldp	x29, x30, [sp], #112
b	<pthread_mutex_unlock@plt>
cmn	w1, #0x1
b.eq	<_umem_cache_free>  // b.none
adrp	x1, <_fini>
adrp	x0, <_fini>
add	x1, x1, #0xd68
add	x0, x0, #0xf58
mov	w2, #0xe87                 	// #3719
bl	<__umem_assert_failed@plt>
cmn	w0, #0x1
b.ne	<_umem_cache_free>  // b.any
ldr	w1, [x19, #72]
b	<_umem_cache_free>
ldr	x28, [x27, #472]
mov	x0, x19
bl	<pthread_mutex_unlock@plt>
mov	w1, #0x0                   	// #0
ldr	x0, [x28, #24]
bl	<_umem_cache_alloc@plt>
mov	x20, x0
mov	x0, x19
bl	<pthread_mutex_lock@plt>
cbz	x20, <_umem_cache_free>
ldadd	x25, x0, [x23]
ldrsw	x2, [x28]
add	x0, x20, #0x8
mov	w1, #0x0                   	// #0
lsl	x2, x2, #3
bl	<memset@plt>
ldr	w1, [x19, #72]
ldr	w0, [x28]
cmp	w1, w0
b.eq	<_umem_cache_free>  // b.none
mov	x0, x19
bl	<pthread_mutex_unlock@plt>
mov	x1, x20
ldr	x0, [x28, #24]
bl	<_umem_cache_free@plt>
ldadd	x26, x0, [x23]
mov	x0, x19
bl	<pthread_mutex_lock@plt>
ldr	w1, [x19, #48]
ldr	w2, [x19, #72]
mov	w4, w1
b	<_umem_cache_free>
mov	x2, x20
mov	x1, x22
mov	x0, x27
bl	<umem_depot_free>
ldr	w1, [x19, #48]
ldr	w2, [x19, #72]
mov	w4, w1
b	<_umem_cache_free>
mov	x0, x19
bl	<pthread_mutex_unlock@plt>
ldr	w0, [x27, #188]
tst	w0, #0x6
b.ne	<_umem_cache_free>  // b.any
ldr	x2, [x27, #152]
cbz	x2, <_umem_cache_free>
mov	x0, x24
ldr	x1, [x27, #168]
blr	x2
mov	x1, x24
mov	x0, x27
ldp	x19, x20, [sp, #16]
ldp	x21, x22, [sp, #32]
ldp	x23, x24, [sp, #48]
ldp	x25, x26, [sp, #64]
ldp	x27, x28, [sp, #80]
ldp	x29, x30, [sp], #112
b	<umem_slab_free>
bl	<umem_rseq_get_ncpus@plt>
cmp	w0, w20
b.le	<_umem_cache_free>
adrp	x0, <__FRAME_END__>
ldr	x1, [x27, #840]
ldr	x0, [x0, #4024]
ldr	w0, [x0]
cbz	w0, <_umem_cache_free>
sbfiz	x0, x20, #6, #32
mov	w2, w20
add	x23, x1, x0
mov	x1, x24
mov	x0, x23
bl	<umem_rseq_free_fastpath@plt>
cbz	w0, <_umem_cache_free>
stp	x25, x26, [sp, #64]
add	x25, x27, #0x280
mov	x0, x27
mov	x1, x25
bl	<umem_depot_alloc>
mov	x22, x0
cbz	x0, <_umem_cache_free>
mov	x2, x0
mov	w3, #0x0                   	// #0
mov	w1, w20
mov	x0, x27
bl	<umem_rseq_reload>
cbnz	w0, <_umem_cache_free>
mov	x1, x25
mov	x2, x22
mov	x0, x27
bl	<umem_depot_free>
ldp	x25, x26, [sp, #64]
b	<_umem_cache_free>
mov	w2, w20
mov	x0, x23
mov	x1, x24
bl	<umem_rseq_free_fastpath@plt>
ldp	x25, x26, [sp, #64]
cbnz	w0, <_umem_cache_free>
ldp	x19, x20, [sp, #16]
ldp	x21, x22, [sp, #32]
ldp	x23, x24, [sp, #48]
ldp	x27, x28, [sp, #80]
ldp	x29, x30, [sp], #112
ret
adrp	x0, <__FRAME_END__>
ldr	x0, [x0, #3480]
ldr	w0, [x0]
cbz	w0, <_umem_cache_free>
adrp	x0, <memcpy@GLIBC_2.17>
ldr	x1, [x0, #2024]
add	x0, x0, #0x7e8
blr	x1
ldr	w0, [x21, x0]
cbz	w0, <_umem_cache_free>
adrp	x0, <memcpy@GLIBC_2.17>
ldr	x1, [x0, #2040]
add	x0, x0, #0x7f8
blr	x1
ldr	x0, [x21, x0]
cbz	x0, <_umem_cache_free>
ldr	w1, [x0]
tbnz	w1, #31, <_umem_cache_free>
adrp	x0, <memcpy@GLIBC_2.17>
ldr	x2, [x0, #2056]
add	x0, x0, #0x808
blr	x2
str	w1, [x21, x0]
b	<_umem_cache_free>
bl	<sched_getcpu@plt>
mov	w2, w0
tbnz	w0, #31, <_umem_cache_free>
adrp	x0, <memcpy@GLIBC_2.17>
ldr	x3, [x0, #2056]
add	x0, x0, #0x808
blr	x3
mov	w1, w2
str	w2, [x21, x0]
b	<_umem_cache_free>
tst	w0, #0x6
b.ne	<_umem_cache_free>  // b.any
tbz	w0, #13, <_umem_cache_free>
mov	x2, x27
mov	x0, x24
ldr	x1, [x27, #128]
bl	<umem_hook_free>
b	<_umem_cache_free>
bl	<umem_rseq_register_thread@plt>
b	<_umem_cache_free>
ldr	x3, [x27, #312]
mov	x2, #0xc8ed                	// #51437
mov	x0, #0xf4ee                	// #62702
movk	x2, #0xa110, lsl #16
movk	x0, #0xf4ee, lsl #16
add	x1, x24, x3
ldp	x20, x4, [x1, #8]
eor	x2, x20, x2
eor	x0, x20, x0
cmp	x4, x2
b.eq	<_umem_cache_free>  // b.none
cmp	x4, x0
b.eq	<_umem_cache_free>  // b.none
add	x20, x27, #0xf0
mov	x0, x20
bl	<pthread_mutex_lock@plt>
ldr	x19, [x27, #400]
add	x3, x27, #0x180
cmp	x3, x19
b.ne	<_umem_cache_free>  // b.any
b	<_umem_cache_free>
ldr	x19, [x19, #16]
cmp	x3, x19
b.eq	<_umem_cache_free>  // b.none
ldp	x0, x1, [x19]
ldr	x2, [x19, #48]
sub	x1, x24, x1
ldr	x0, [x0, #288]
mul	x0, x0, x2
cmp	x1, x0
b.cs	<_umem_cache_free>  // b.hs, b.nlast
mov	x0, x20
bl	<pthread_mutex_unlock@plt>
ldr	x0, [x19]
cmp	x27, x0
b.eq	<_umem_cache_free>  // b.none
mov	x2, x24
mov	x1, x27
mov	w0, #0x3                   	// #3
ldp	x19, x20, [sp, #16]
ldp	x21, x22, [sp, #32]
ldp	x23, x24, [sp, #48]
ldp	x27, x28, [sp, #80]
ldp	x29, x30, [sp], #112
b	<umem_error>
bl	<umem_rseq_register_thread@plt>
b	<_umem_cache_free>
bl	<pthread_self@plt>
lsr	x2, x0, #12
cmp	w2, #0x0
cneg	w2, w2, lt	// lt = tstop
b	<_umem_cache_free>
adrp	x1, <_fini>
adrp	x0, <_fini>
add	x1, x1, #0xd68
add	x0, x0, #0xfd0
mov	w2, #0xe89                 	// #3721
bl	<__umem_assert_failed@plt>
str	x0, [x1, #16]
ldr	w0, [x27, #188]
tbnz	w0, #9, <_umem_cache_free>
ldr	x2, [x24, x3]
mov	x1, #0xface                	// #64206
movk	x1, #0xfeed, lsl #16
movk	x1, #0xface, lsl #32
movk	x1, #0xfeed, lsl #48
cmp	x2, x1
b.ne	<_umem_cache_free>  // b.any
tbnz	w0, #0, <_umem_cache_free>
ldr	x2, [x27, #152]
cbz	x2, <_umem_cache_free>
mov	x0, x24
ldr	x1, [x27, #168]
blr	x2
ldr	w1, [x27, #188]
ldr	w0, [x19, #76]
tbz	w1, #1, <_umem_cache_free>
ldr	x2, [x27, #320]
add	x2, x24, x2
cmp	x24, x2
b.cs	<_umem_cache_free>  // b.hs, b.nlast
mov	w3, #0xbeef                	// #48879
mov	x1, x24
movk	w3, #0xdead, lsl #16
str	w3, [x1], #8
stur	w3, [x1, #-4]
cmp	x2, x1
b.hi	<_umem_cache_free>  // b.pmore
tbz	w0, #13, <_umem_cache_free>
b	<_umem_cache_free>
ldp	x25, x26, [sp, #64]
b	<_umem_cache_free>
mov	x2, x24
mov	x1, x27
mov	w0, #0x2                   	// #2
b	<_umem_cache_free>
mov	x0, x20
bl	<pthread_mutex_unlock@plt>
b	<_umem_cache_free>
mov	x2, x24
mov	x1, x27
mov	w0, #0x1                   	// #1
b	<_umem_cache_free>
ldp	x25, x26, [sp, #64]
b	<_umem_cache_free>
tbnz	w0, #3, <_umem_cache_free>
add	x1, sp, #0x60
mov	w0, #0x1                   	// #1
bl	<clock_gettime@plt>
ldp	x0, x1, [sp, #96]
adrp	x22, <__FRAME_END__>
orr	x0, x1, x0, lsl #32
str	x0, [x20, #32]
bl	<pthread_self@plt>
ldr	w2, [x27, #188]
mov	x1, x0
add	x0, x20, #0x48
ldr	x22, [x22, #3872]
str	x1, [x20, #40]
ubfx	x2, x2, #4, #1
ldr	w1, [x22]
bl	<getpcstack@plt>
ldr	w2, [x22]
adrp	x3, <__FRAME_END__>
mov	x1, x20
str	w0, [x20, #64]
ldr	x3, [x3, #3520]
lsl	x2, x2, #3
add	x2, x2, #0x48
ldr	x0, [x3]
bl	<umem_log_enter>
str	x0, [x20, #48]
b	<_umem_cache_free>
ldr	x1, [x20, #8]
cmp	x24, x1
b.eq	<_umem_cache_free>  // b.none
mov	x2, x24
mov	x1, x27
mov	w0, #0x5                   	// #5
b	<_umem_cache_free>
adrp	x0, <__FRAME_END__>
mov	x1, x24
ldr	x2, [x27, #328]
ldr	x0, [x0, #3800]
ldr	x0, [x0]
bl	<umem_log_enter>
str	x0, [x20, #56]
b	<_umem_cache_free>
