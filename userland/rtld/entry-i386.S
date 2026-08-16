.code32
.text
.globl _rtld_start
.type _rtld_start, @function
_rtld_start:
	xorl %ebp, %ebp
	movl %esp, %esi
	pushl %esi
	call rtld_main
	addl $4, %esp
	movl %esi, %esp
	jmp *%eax
.size _rtld_start, .-_rtld_start

.globl rtld_syscall6
.type rtld_syscall6, @function
rtld_syscall6:
	pushl %ebp
	pushl %edi
	pushl %esi
	pushl %ebx
	movl 20(%esp), %eax
	movl 24(%esp), %ebx
	movl 28(%esp), %ecx
	movl 32(%esp), %edx
	movl 36(%esp), %esi
	movl 40(%esp), %edi
	movl 44(%esp), %ebp
	int $0xc2
	popl %ebx
	popl %esi
	popl %edi
	popl %ebp
	ret
.size rtld_syscall6, .-rtld_syscall6
