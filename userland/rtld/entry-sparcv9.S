.section ".text"
.align 4
.global _rtld_start
.type _rtld_start, #function
_rtld_start:
	clr %fp
	add %sp, 2047, %l0
	mov %l0, %o0
	call rtld_main
	 nop
	sub %l0, 2047, %sp
	jmp %o0
	 nop
.size _rtld_start, .-_rtld_start

.global rtld_syscall6
.type rtld_syscall6, #function
rtld_syscall6:
	mov %o0, %g1
	mov %o1, %o0
	mov %o2, %o1
	mov %o3, %o2
	mov %o4, %o3
	mov %o5, %o4
	ldx [%sp + 2223], %o5
	stx %l0, [%sp + 2231]
	mov %o5, %l0
	mov %g1, %o5
	ta 0x6d
	ldx [%sp + 2231], %l0
	retl
	 nop
.size rtld_syscall6, .-rtld_syscall6
