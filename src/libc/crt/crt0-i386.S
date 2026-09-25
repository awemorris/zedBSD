#include <hal/arch.h>

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
	call __libc_init
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
	movl HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET(%esp), %ebx
	movl HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET(%esp), %ecx
	movl $80, %eax
	int $0xc2
	ud2
.size __signal_restorer, .-__signal_restorer
