stp	x29, x30, [sp, #-112]!
mov	x29, sp
stp	x21, x22, [sp, #32]
adrp	x22, <__FRAME_END__>
mrs	x21, tpidr_el0
stp	x25, x26, [sp, #64]
adrp	x25, <__FRAME_END__>
adrp	x26, <__FRAME_END__>
ldr	x22, [x22, #3856]
stp	x23, x24, [sp, #48]
adrp	x24, <umem_profile_spec>
mov	w23, w1
add	x24, x24, #0x138
ldr	x25, [x25, #3728]
stp	x27, x28, [sp, #80]
mov	x27, x0
stp	x19, x20, [sp, #16]
adrp	x0, <memcpy@GLIBC_2.17>
ldr	x1, [x0, #2056]
add	x0, x0, #0x808
blr	x1
ldr	w0, [x21, x0]
ldr	x19, [x22]
cmn	w0, #0x1
b.eq	<_umem_cache_alloc>  // b.none
mov	w1, w0
ldr	w0, [x27, #232]
ldr	x2, [x26, #3480]
and	w0, w1, w0
lsl	x0, x0, #3
ldr	w19, [x19, x0]
ldr	w0, [x2]
add	x19, x27, x19
cbz	w0, <_umem_cache_alloc>
ldr	x0, [x27, #840]
cbz	x0, <_umem_cache_alloc>
adrp	x0, <memcpy@GLIBC_2.17>
ldr	x1, [x0, #2024]
add	x0, x0, #0x7e8
blr	x1
ldr	w0, [x21, x0]
cbz	w0, <_umem_cache_alloc>
adrp	x0, <memcpy@GLIBC_2.17>
ldr	x1, [x0, #2040]
add	x0, x0, #0x7f8
blr	x1
ldr	x0, [x21, x0]
cbz	x0, <_umem_cache_alloc>
ldr	w28, [x0]
tbnz	w28, #31, <_umem_cache_alloc>
bl	<umem_rseq_get_ncpus@plt>
cmp	w0, w28
b.le	<_umem_cache_alloc>
adrp	x0, <__FRAME_END__>
ldr	x2, [x27, #840]
ldr	x0, [x0, #4024]
ldr	w0, [x0]
cbz	w0, <_umem_cache_alloc>
sbfiz	x0, x28, #6, #32
mov	w1, w28
add	x2, x2, x0
mov	x0, x2
str	x2, [sp, #104]
bl	<umem_rseq_alloc_fastpath@plt>
mov	x20, x0
cbnz	x0, <_umem_cache_alloc>
mov	w1, w28
mov	x0, x27
bl	<umem_rseq_alloc_slowpath>
ldr	x2, [sp, #104]
cbnz	w0, <_umem_cache_alloc>
nop
mov	x0, x19
bl	<pthread_mutex_lock@plt>
ldr	w4, [x19, #48]
cmp	w4, #0x0
b.gt	<_umem_cache_alloc>
ldr	w2, [x19, #52]
cmp	w2, #0x0
b.le	<_umem_cache_alloc>
ldp	x1, x20, [x19, #56]
cbz	x1, <_umem_cache_alloc>
add	w5, w2, w4
ldr	w3, [x19, #72]
cmp	w5, w3
b.ne	<_umem_cache_alloc>  // b.any
cmp	w3, #0x0
b.le	<_umem_cache_alloc>
cbz	x20, <_umem_cache_alloc>
prfm	pldl1keep, [x20]
mov	w3, #0xffffffff            	// #-1
str	w4, [x19, #52]
adrp	x0, <memcpy@GLIBC_2.17>
ldr	x4, [x0, #2056]
add	x0, x0, #0x808
blr	x4
str	w3, [x21, x0]
stp	x20, x1, [x19, #56]
sub	w4, w2, #0x1
add	x1, x20, w4, sxtw #3
add	x2, x20, w2, sxtw #3
mov	x0, x19
ldr	w3, [x19, #80]
ldr	x1, [x1, #8]
ldr	x5, [x24, #152]
add	w3, w3, #0x1
prfm	pldl1keep, [x20]
str	w4, [x19, #48]
str	w3, [x19, #80]
eor	x1, x1, x5
eor	x20, x1, x2, lsr #12
bl	<pthread_mutex_unlock@plt>
ldr	w0, [x19, #76]
mov	w1, #0x2006                	// #8198
tst	w0, w1
b.ne	<_umem_cache_alloc>  // b.any
mov	x0, x20
ldp	x19, x20, [sp, #16]
ldp	x21, x22, [sp, #32]
ldp	x23, x24, [sp, #48]
ldp	x25, x26, [sp, #64]
ldp	x27, x28, [sp, #80]
ldp	x29, x30, [sp], #112
ret
ldr	w1, [x19, #72]
cbz	w1, <_umem_cache_alloc>
add	x1, x27, #0x200
mov	x0, x27
bl	<umem_depot_alloc>
mov	x20, x0
cbz	x0, <_umem_cache_alloc>
ldr	x2, [x19, #64]
cbz	x2, <_umem_cache_alloc>
add	x1, x27, #0x280
mov	x0, x27
bl	<umem_depot_free>
ldr	x1, [x19, #56]
ldr	w4, [x19, #48]
ldr	w2, [x19, #72]
cbz	x1, <_umem_cache_alloc>
cbnz	w4, <_umem_cache_alloc>
cmp	w2, #0x0
b.le	<_umem_cache_alloc>
ldr	x3, [x19, #64]
cbz	x3, <_umem_cache_alloc>
prfm	pldl1keep, [x3]
b	<_umem_cache_alloc>
cmn	w4, #0x1
b.eq	<_umem_cache_alloc>  // b.none
adrp	x1, <_fini>
adrp	x0, <_fini>
add	x1, x1, #0xd68
add	x0, x0, #0xf58
mov	w2, #0xe87                 	// #3719
bl	<__umem_assert_failed@plt>
ldr	w3, [x19, #72]
b	<_umem_cache_alloc>
cmn	w4, #0x1
b.eq	<_umem_cache_alloc>  // b.none
adrp	x1, <_fini>
adrp	x0, <_fini>
add	x1, x1, #0xd68
add	x0, x0, #0xf58
mov	w2, #0xe87                 	// #3719
bl	<__umem_assert_failed@plt>
mov	x0, x19
bl	<pthread_mutex_unlock@plt>
mov	w1, w23
mov	x0, x27
bl	<umem_slab_alloc>
mov	x20, x0
cbz	x0, <_umem_cache_alloc>
ldr	w1, [x27, #188]
tst	w1, #0x6
b.ne	<_umem_cache_alloc>  // b.any
ldr	x3, [x27, #144]
cbz	x3, <_umem_cache_alloc>
mov	w2, #0x0                   	// #0
ldr	x1, [x27, #168]
blr	x3
cbz	w0, <_umem_cache_alloc>
add	x0, x27, #0x20
mov	x1, #0x1                   	// #1
ldadd	x1, x1, [x0]
mov	x1, x20
mov	x0, x27
bl	<umem_slab_free>
mov	w1, w23
mov	x0, x27
bl	<umem_alloc_retry>
cbnz	w0, <_umem_cache_alloc>
mov	x20, #0x0                   	// #0
b	<_umem_cache_alloc>
mov	x1, x0
mov	w2, w23
mov	x0, x27
bl	<umem_cache_alloc_debug>
cmn	w0, #0x1
b.eq	<_umem_cache_alloc>  // b.none
ldr	w0, [x27, #972]
tbz	w0, #13, <_umem_cache_alloc>
mov	x2, x27
mov	x0, x20
ldp	x19, x20, [sp, #16]
ldp	x21, x22, [sp, #32]
ldp	x23, x24, [sp, #48]
ldp	x25, x26, [sp, #64]
ldp	x27, x28, [sp, #80]
ldr	x1, [x2, #128]
ldp	x29, x30, [sp], #112
b	<umem_hook_alloc>
ldr	x0, [x26, #3480]
ldr	w0, [x0]
cbz	w0, <_umem_cache_alloc>
adrp	x0, <memcpy@GLIBC_2.17>
ldr	x1, [x0, #2024]
add	x0, x0, #0x7e8
blr	x1
ldr	w0, [x21, x0]
cbz	w0, <_umem_cache_alloc>
adrp	x0, <memcpy@GLIBC_2.17>
ldr	x1, [x0, #2040]
add	x0, x0, #0x7f8
blr	x1
ldr	x0, [x21, x0]
cbz	x0, <_umem_cache_alloc>
ldr	w1, [x0]
tbz	w1, #31, <_umem_cache_alloc>
bl	<sched_getcpu@plt>
mov	w2, w0
tbnz	w0, #31, <_umem_cache_alloc>
adrp	x0, <memcpy@GLIBC_2.17>
ldr	x3, [x0, #2056]
add	x0, x0, #0x808
blr	x3
mov	w1, w2
str	w2, [x21, x0]
b	<_umem_cache_alloc>
bl	<umem_rseq_register_thread@plt>
b	<_umem_cache_alloc>
tst	w0, #0x6
b.eq	<_umem_cache_alloc>  // b.none
mov	w2, w23
mov	x1, x20
mov	x0, x27
bl	<umem_cache_alloc_debug>
cmn	w0, #0x1
b.eq	<_umem_cache_alloc>  // b.none
ldr	w0, [x19, #76]
b	<_umem_cache_alloc>
cmp	x27, x25
b.ne	<_umem_cache_alloc>  // b.any
mov	x20, #0x0                   	// #0
b	<_umem_cache_alloc>
adrp	x0, <memcpy@GLIBC_2.17>
ldr	x2, [x0, #2056]
add	x0, x0, #0x808
blr	x2
str	w1, [x21, x0]
b	<_umem_cache_alloc>
mov	w1, w28
mov	x0, x2
bl	<umem_rseq_alloc_fastpath@plt>
mov	x20, x0
cbnz	x0, <_umem_cache_alloc>
b	<_umem_cache_alloc>
bl	<umem_rseq_register_thread@plt>
b	<_umem_cache_alloc>
bl	<pthread_self@plt>
lsr	x2, x0, #12
cmp	w2, #0x0
cneg	w2, w2, lt	// lt = tstop
b	<_umem_cache_alloc>
mov	w2, w4
ldr	x20, [x19, #56]
b	<_umem_cache_alloc>
adrp	x1, <_fini>
adrp	x0, <_fini>
add	x1, x1, #0xd68
add	x0, x0, #0xfd0
mov	w2, #0xe89                 	// #3721
bl	<__umem_assert_failed@plt>
