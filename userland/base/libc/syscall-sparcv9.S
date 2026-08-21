.section ".text"
.align 4
.global __syscall6
.type __syscall6, #function
__syscall6:
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
.size __syscall6, .-__syscall6

.global __signal_restorer
.type __signal_restorer, #function
__signal_restorer:
	add %sp, 2175, %o1
	ldx [%o1], %o1
	ldx [%sp + 2183], %o2
	mov 80, %o0
	call __syscall6
	 nop
	ta 0x70
.size __signal_restorer, .-__signal_restorer
