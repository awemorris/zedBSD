.code32
.text
.globl __syscall6
.type __syscall6, @function
__syscall6:
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
.size __syscall6, .-__syscall6

.globl __signal_restorer
.type __signal_restorer, @function
__signal_restorer:
	movl 12(%esp), %ebx
	movl 8(%esp), %ecx
	movl $80, %eax
	int $0xc2
	ud2
.size __signal_restorer, .-__signal_restorer
