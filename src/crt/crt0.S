.code32
.text
.globl _start
.type _start, @function
_start:
	xorl %ebp, %ebp
	movl %esp, %ebx
	movl (%ebx), %eax
	leal 4(%ebx), %ecx
	leal 8(%ebx,%eax,4), %edx
	pushl %edx
	pushl %ecx
	pushl %eax
	call zedbsd_user_libc_init
	addl $12, %esp
	movl (%ebx), %eax
	leal 4(%ebx), %ecx
	leal 8(%ebx,%eax,4), %edx
	pushl %edx
	pushl %ecx
	pushl %eax
	call main
	pushl %eax
	call _exit
1:
	jmp 1b
.size _start, .-_start

.globl zedbsd_syscall6
.type zedbsd_syscall6, @function
zedbsd_syscall6:
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
.size zedbsd_syscall6, .-zedbsd_syscall6

.globl zedbsd_signal_restorer
.type zedbsd_signal_restorer, @function
zedbsd_signal_restorer:
	movl 12(%esp), %ebx
	movl 8(%esp), %ecx
	movl $80, %eax
	int $0xc2
	ud2
.size zedbsd_signal_restorer, .-zedbsd_signal_restorer
