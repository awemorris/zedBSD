.code32
.text
.globl _start
.type _start, @function
_start:
	movl $0x49334332, %eax
	int $0xc2
1:
	jmp 1b
.size _start, .-_start
