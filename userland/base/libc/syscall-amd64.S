.code64
.text
.globl __syscall6
.type __syscall6, @function
__syscall6:
	pushq %rbx
	pushq %rbp
	movq %rdi, %rax
	movq %rsi, %rbx
	movq %rdx, %r10
	movq %rcx, %r11
	movq %r10, %rcx
	movq %r11, %rdx
	movq %r8, %rsi
	movq %r9, %rdi
	movq 24(%rsp), %rbp
	int $0xc2
	popq %rbp
	popq %rbx
	ret
.size __syscall6, .-__syscall6

.globl __signal_restorer
.type __signal_restorer, @function
__signal_restorer:
	movq (%rsp), %rbx
	movq 8(%rsp), %rcx
	movq $80, %rax
	int $0xc2
	ud2
.size __signal_restorer, .-__signal_restorer
