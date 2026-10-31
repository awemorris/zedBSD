#include <hal/arch.h>

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
	bl __libc_init
	mov x0, x20
	mov x1, x21
	mov x2, x22
	bl main
	bl _exit
1:
	wfe
	b 1b
	.size _start, .-_start

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

/*
 * long __vfork_spawn(void (*entry)(void *), void *argument, void *stack_top);
 *
 * The vfork system call (169) for posix_spawn, as in the shared library's
 * system call entry: the child runs entry(argument) on the stack below
 * stack_top until it execs or ends, and the caller resumes then with the
 * child's pid, or a negative errno.  entry must not return; if it does,
 * the child exits with status 127.
 */
	.balign 16
	.global __vfork_spawn
	.hidden __vfork_spawn
	.type __vfork_spawn, %function
__vfork_spawn:
	/* The child reads its entry, argument and stack from x19 to x21. */
	stp x19, x20, [sp, #-32]!
	str x21, [sp, #16]
	mov x19, x0
	mov x20, x1
	mov x21, x2
	mov x8, #169
	svc #0
	cbnz x0, 1f

	/* The child: its own stack before anything is written to one. */
	and x9, x21, #-16
	mov sp, x9
	mov x29, #0
	mov x0, x20
	blr x19

	/* entry returned: the child ends with 127 (exit is system call 1). */
	mov x0, #127
	mov x8, #1
	svc #0
	brk #0

	/* The parent, once the child execed or ended. */
1:	ldr x21, [sp, #16]
	ldp x19, x20, [sp], #32
	ret
	.size __vfork_spawn, .-__vfork_spawn

	.global __signal_restorer
	.type __signal_restorer, %function
__signal_restorer:
	ldr w0, [sp, #HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET]
	ldr x1, [sp, #HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET]
	mov x8, #80
	svc #0
	brk #0
	.size __signal_restorer, .-__signal_restorer
