push   %r15
push   %r14
push   %r13
push   %r12
lea    0x200(%rdi),%r12
push   %rbp
mov    %esi,%ebp
push   %rbx
mov    %rdi,%rbx
sub    $0x18,%rsp
mov    X(%rip),%rax        # <umem_cpus@@Base-0x21eb8>
mov    (%rax),%r15
data16 lea X(%rip),%rdi        # <cached_cpu_hint@@Base>
data16 data16 rex.W call <__tls_get_addr@plt>
mov    (%rax),%eax
cmp    $0xffffffff,%eax
je     <_umem_cache_alloc>
mov    %eax,%r13d
mov    %r13d,%eax
and    0xe8(%rbx),%eax
mov    (%r15,%rax,8),%r8d
mov    X(%rip),%rax        # <umem_rseq_enabled@@Base-0xc9cae0>
mov    (%rax),%esi
lea    (%rbx,%r8,1),%r13
test   %esi,%esi
je     <_umem_cache_alloc>
cmpq   $0x0,0x348(%rbx)
je     <_umem_cache_alloc>
data16 lea X(%rip),%rdi        # <umem_rseq_registered@@Base>
data16 data16 rex.W call <__tls_get_addr@plt>
mov    (%rax),%ecx
test   %ecx,%ecx
je     <_umem_cache_alloc>
data16 lea X(%rip),%rdi        # <umem_rseq_cpu_idp@@Base>
data16 data16 rex.W call <__tls_get_addr@plt>
mov    (%rax),%rax
test   %rax,%rax
je     <_umem_cache_alloc>
mov    (%rax),%r14d
test   %r14d,%r14d
js     <_umem_cache_alloc>
call   <umem_rseq_get_ncpus@plt>
cmp    %r14d,%eax
jle    <_umem_cache_alloc>
mov    X(%rip),%rax        # <umem_rseq_asm_safe@@Base-0xc9cb74>
mov    (%rax),%edx
test   %edx,%edx
je     <_umem_cache_alloc>
mov    %r14d,%r15d
mov    %r14d,%esi
shl    $0x6,%r15
add    0x348(%rbx),%r15
mov    %r15,%rdi
call   <umem_rseq_alloc_fastpath@plt>
mov    %rax,%r8
test   %rax,%rax
je     <_umem_cache_alloc>
testw  $0x2006,0x44(%r13)
jne    <_umem_cache_alloc>
add    $0x18,%rsp
mov    %r8,%rax
pop    %rbx
pop    %rbp
pop    %r12
pop    %r13
pop    %r14
pop    %r15
ret
mov    %r14d,%esi
mov    %rbx,%rdi
call   <umem_rseq_alloc_slowpath>
test   %eax,%eax
je     <_umem_cache_alloc>
mov    %r14d,%esi
mov    %r15,%rdi
call   <umem_rseq_alloc_fastpath@plt>
mov    %rax,%r8
test   %rax,%rax
jne    <_umem_cache_alloc>
nopl   (%rax)
mov    %r13,%rdi
call   <pthread_mutex_lock@plt>
mov    0x28(%r13),%edx
test   %edx,%edx
jg     <_umem_cache_alloc>
movslq 0x2c(%r13),%r15
test   %r15d,%r15d
jle    <_umem_cache_alloc>
mov    0x30(%r13),%rax
test   %rax,%rax
je     <_umem_cache_alloc>
mov    0x40(%r13),%ecx
lea    (%r15,%rdx,1),%esi
cmp    %ecx,%esi
jne    <_umem_cache_alloc>
test   %ecx,%ecx
jle    <_umem_cache_alloc>
mov    0x38(%r13),%r14
test   %r14,%r14
je     <_umem_cache_alloc>
prefetcht0 (%r14)
mov    %rax,0x38(%r13)
mov    %edx,0x2c(%r13)
mov    %r14,0x30(%r13)
data16 lea X(%rip),%rdi        # <cached_cpu_hint@@Base>
data16 data16 rex.W call <__tls_get_addr@plt>
movl   $0xffffffff,(%rax)
lea    -0x1(%r15),%eax
prefetcht0 (%r14)
mov    %r13,%rdi
mov    %eax,0x28(%r13)
cltq
mov    0x8(%r14,%rax,8),%r9
addl   $0x1,0x48(%r13)
lea    (%r14,%r15,8),%rax
xor    X(%rip),%r9        # <umem_link_cookie>
shr    $0xc,%rax
xor    %rax,%r9
mov    %r9,0x8(%rsp)
call   <pthread_mutex_unlock@plt>
testw  $0x2006,0x44(%r13)
mov    0x8(%rsp),%r8
je     <_umem_cache_alloc>
mov    %ebp,%ecx
mov    %r8,%rdx
mov    %r13,%rsi
mov    %rbx,%rdi
call   <umem_cache_alloc_tail>
mov    0x8(%rsp),%r8
cmp    $0xffffffff,%eax
je     <_umem_cache_alloc>
jmp    <_umem_cache_alloc>
nopl   0x0(%rax)
mov    0x40(%r13),%eax
test   %eax,%eax
je     <_umem_cache_alloc>
mov    %r12,%rsi
mov    %rbx,%rdi
call   <umem_depot_alloc>
mov    %rax,%r14
test   %rax,%rax
je     <_umem_cache_alloc>
mov    0x38(%r13),%rdx
test   %rdx,%rdx
je     <_umem_cache_alloc>
lea    0x280(%rbx),%rsi
mov    %rbx,%rdi
call   <umem_depot_free>
mov    0x30(%r13),%rax
mov    0x28(%r13),%edx
test   %rax,%rax
je     <_umem_cache_alloc>
test   %edx,%edx
jne    <_umem_cache_alloc>
movslq 0x40(%r13),%r15
test   %r15d,%r15d
jle    <_umem_cache_alloc>
mov    0x38(%r13),%rsi
test   %rsi,%rsi
je     <_umem_cache_alloc>
prefetcht0 (%rsi)
jmp    <_umem_cache_alloc>
cs nopw 0x0(%rax,%rax,1)
cmp    $0xffffffff,%edx
je     <_umem_cache_alloc>
mov    $0xe99,%edx
lea    X(%rip),%rsi        # <_fini>
lea    X(%rip),%rdi        # <lookup.0>
call   <__umem_assert_failed@plt>
nopl   (%rax)
mov    0x40(%r13),%ecx
jmp    <_umem_cache_alloc>
nopl   0x0(%rax)
cmp    $0xffffffff,%edx
jne    <_umem_cache_alloc>
jmp    <_umem_cache_alloc>
nopw   0x0(%rax,%rax,1)
mov    X(%rip),%rax        # <umem_rseq_enabled@@Base-0xc9cae0>
mov    (%rax),%r8d
test   %r8d,%r8d
je     <_umem_cache_alloc>
data16 lea X(%rip),%rdi        # <umem_rseq_registered@@Base>
data16 data16 rex.W call <__tls_get_addr@plt>
mov    (%rax),%edi
test   %edi,%edi
je     <_umem_cache_alloc>
data16 lea X(%rip),%rdi        # <umem_rseq_cpu_idp@@Base>
data16 data16 rex.W call <__tls_get_addr@plt>
mov    (%rax),%rax
test   %rax,%rax
je     <_umem_cache_alloc>
mov    (%rax),%r13d
test   %r13d,%r13d
jns    <_umem_cache_alloc>
call   <sched_getcpu@plt>
mov    %eax,%r13d
test   %eax,%eax
jns    <_umem_cache_alloc>
call   <pthread_self@plt>
shr    $0xc,%rax
mov    %eax,%edx
neg    %edx
cmovns %edx,%eax
mov    %eax,%r13d
data16 lea X(%rip),%rdi        # <cached_cpu_hint@@Base>
data16 data16 rex.W call <__tls_get_addr@plt>
mov    %r13d,(%rax)
jmp    <_umem_cache_alloc>
call   <umem_rseq_register_thread@plt>
jmp    <_umem_cache_alloc>
nopw   0x0(%rax,%rax,1)
mov    %r13,%rdi
call   <pthread_mutex_unlock@plt>
mov    %ebp,%esi
mov    %rbx,%rdi
call   <umem_slab_alloc>
mov    %rax,%r8
test   %rax,%rax
je     <_umem_cache_alloc>
testb  $0x6,0xbc(%rbx)
jne    <_umem_cache_alloc>
mov    0x90(%rbx),%rax
test   %rax,%rax
je     <_umem_cache_alloc>
mov    %r8,0x8(%rsp)
xor    %edx,%edx
mov    %r8,%rdi
mov    0xa8(%rbx),%rsi
call   *%rax
mov    0x8(%rsp),%r8
test   %eax,%eax
je     <_umem_cache_alloc>
lock addq $0x1,0x20(%rbx)
mov    %r8,%rsi
mov    %rbx,%rdi
call   <umem_slab_free>
mov    %ebp,%esi
mov    %rbx,%rdi
call   <umem_alloc_retry>
test   %eax,%eax
jne    <_umem_cache_alloc>
xor    %r8d,%r8d
jmp    <_umem_cache_alloc>
nopw   0x0(%rax,%rax,1)
call   <umem_rseq_register_thread@plt>
jmp    <_umem_cache_alloc>
mov    %ebp,%edx
mov    %rax,%rsi
mov    %rbx,%rdi
mov    %rax,0x8(%rsp)
call   <umem_cache_alloc_debug>
mov    0x8(%rsp),%r8
cmp    $0xffffffff,%eax
je     <_umem_cache_alloc>
testb  $0x20,0x3c5(%rbx)
je     <_umem_cache_alloc>
mov    0x80(%rbx),%rsi
add    $0x18,%rsp
mov    %rbx,%rdx
mov    %r8,%rdi
pop    %rbx
pop    %rbp
pop    %r12
pop    %r13
pop    %r14
pop    %r15
jmp    <umem_hook_alloc>
nopl   0x0(%rax,%rax,1)
mov    %r8,0x8(%rsp)
mov    %ebp,%ecx
mov    %r8,%rdx
jmp    <_umem_cache_alloc>
mov    0x30(%r13),%r14
movslq %edx,%r15
jmp    <_umem_cache_alloc>
cmp    X(%rip),%rbx        # <umem_null_cache@@Base-0x21c60>
jne    <_umem_cache_alloc>
jmp    <_umem_cache_alloc>
mov    $0xe9b,%edx
lea    X(%rip),%rsi        # <_fini>
lea    X(%rip),%rdi        # <_fini>
call   <__umem_assert_failed@plt>
nopl   (%rax)
