#include <hal/arch.h>

	.text
.balign 16
.global __syscall6
.type __syscall6, %function
__syscall6:
	mov x8, x0
	mov x0, x1
	mov x1, x2
	mov x2, x3
	mov x3, x4
	mov x4, x5
	mov x5, x6
	svc #0
	ret
.size __syscall6, .-__syscall6

.global __signal_restorer
.type __signal_restorer, %function
__signal_restorer:
	ldr w0, [sp, #HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET]
	ldr x1, [sp, #HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET]
	mov x8, #80
	svc #0
	brk #0
.size __signal_restorer, .-__signal_restorer
