.code32
.text
.globl _start
.type _start, @function
_start:
	/* One byte below the usable stack is the immutable guard page. */
	movb $0x5a, 0x7fefe000
	ud2
.size _start, .-_start
