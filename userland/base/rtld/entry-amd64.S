.code64
.text
.globl _rtld_start
.type _rtld_start, @function
_rtld_start:
	xorq %rbp, %rbp
	movq %rsp, %r12
	movq %r12, %rdi
	call rtld_main
	movq %r12, %rsp
	jmp *%rax
.size _rtld_start, .-_rtld_start

.globl rtld_syscall6
.type rtld_syscall6, @function
rtld_syscall6:
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
.size rtld_syscall6, .-rtld_syscall6
