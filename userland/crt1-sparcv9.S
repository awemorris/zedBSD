.section ".text"
.align 4
.global _start
.type _start, #function
_start:
	clr %fp
	add %sp, 2047, %l0
	ldx [%l0], %l1
	add %l0, 8, %l2
	sllx %l1, 3, %l3
	add %l2, %l3, %l3
	add %l3, 8, %l3
	mov %l1, %o0
	mov %l2, %o1
	call zedbsd_user_libc_init
	 mov %l3, %o2
	mov %l1, %o0
	mov %l2, %o1
	call main
	 mov %l3, %o2
	call exit
	 nop
1:
	ba 1b
	 nop
.size _start, .-_start
