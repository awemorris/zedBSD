.text
.balign 16
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
