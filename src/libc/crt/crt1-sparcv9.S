.section ".text"
.align 4
.global _start
.type _start, #function
_start:
	.cfi_startproc
	/*
	 * Nothing called this, so a debugger walking the stack stops here:
	 * the register it would return through holds no return address.
	 */
	.cfi_undefined %o7
	clr %fp
	add %sp, 2047, %l0
	ldx [%l0], %l1
	add %l0, 8, %l2
	sllx %l1, 3, %l3
	add %l2, %l3, %l3
	add %l3, 8, %l3
	/* Reserve the ABI caller frame before the first non-leaf call. */
	sub %sp, 176, %sp
	mov %l1, %o0
	mov %l2, %o1
	call __libc_init
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
	.cfi_endproc
.size _start, .-_start
