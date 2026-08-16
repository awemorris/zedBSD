.text
.balign 16
.global _start
.type _start, %function
_start:
	mov x29, xzr
	mov x30, xzr
	mov x19, sp
	ldr x20, [x19]
	add x21, x19, #8
	add x22, x21, x20, lsl #3
	add x22, x22, #8
	mov x0, x20
	mov x1, x21
	mov x2, x22
	bl zedbsd_user_libc_init
	mov x0, x20
	mov x1, x21
	mov x2, x22
	bl main
	bl exit
1:
	wfe
	b 1b
.size _start, .-_start
