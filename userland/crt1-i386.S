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
	addl $12, %esp
	pushl %eax
	call exit
1:
	jmp 1b
.size _start, .-_start
