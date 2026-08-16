.code64
.text
.globl zedbsd_syscall6
.type zedbsd_syscall6, @function
zedbsd_syscall6:
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
.size zedbsd_syscall6, .-zedbsd_syscall6

.globl zedbsd_signal_restorer
.type zedbsd_signal_restorer, @function
zedbsd_signal_restorer:
	movq (%rsp), %rbx
	movq 8(%rsp), %rcx
	movq $80, %rax
	int $0xc2
	ud2
.size zedbsd_signal_restorer, .-zedbsd_signal_restorer
