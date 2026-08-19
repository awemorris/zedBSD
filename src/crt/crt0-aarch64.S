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
	bl _exit
1:
	wfe
	b 1b
	.size _start, .-_start

	.global zedbsd_syscall6
	.type zedbsd_syscall6, %function
zedbsd_syscall6:
	mov x8, x0
	mov x0, x1
	mov x1, x2
	mov x2, x3
	mov x3, x4
	mov x4, x5
	mov x5, x6
	svc #0
	ret
	.size zedbsd_syscall6, .-zedbsd_syscall6

	.global zedbsd_signal_restorer
	.type zedbsd_signal_restorer, %function
zedbsd_signal_restorer:
	ldr w0, [sp]
	ldr x1, [sp, #8]
	mov x8, #80
	svc #0
	brk #0
	.size zedbsd_signal_restorer, .-zedbsd_signal_restorer
