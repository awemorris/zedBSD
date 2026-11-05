#include <hal/arch.h>

.code64
.text
.globl _start
.type _start, @function
_start:
	xorq %rbp, %rbp
	movq %rsp, %r12
	movq (%r12), %rdi
	leaq 8(%r12), %rsi
	leaq 16(%r12,%rdi,8), %rdx
	call __libc_init
	movq (%r12), %rdi
	leaq 8(%r12), %rsi
	leaq 16(%r12,%rdi,8), %rdx
	call main
	movq %rax, %rdi
	call _exit
1:
	pause
	jmp 1b
.size _start, .-_start

.globl __syscall6
.type __syscall6, @function
__syscall6:
	pushq %rbx
	pushq %rbp
	movq %rdi, %rax
	movq %rsi, %rbx
	movq %rdx, %r10
	movq %rcx, %rdx
	movq %r8, %rsi
	movq %r9, %rdi
	movq 24(%rsp), %rbp
	/*
	 * SYSCALL takes the second argument in r10, since it keeps the return
	 * address in rcx; rcx and r11 do not survive the call.
	 */
	syscall
	popq %rbp
	popq %rbx
	ret
.size __syscall6, .-__syscall6

/*
 * pid_t vfork(void);
 *
 * The vfork system call (169): the child runs in this address space,
 * on this stack, until it execs or calls _exit, and the caller waits
 * until then.  The return address is kept in rdx across the call, since
 * the child may overwrite the stack slot that held it before the parent
 * resumes.  A failure sets errno through __vfork_error.
 */
.globl vfork
.type vfork, @function
vfork:
	popq %rdx
	movq $169, %rax
	syscall
	pushq %rdx
	cmpq $-4095, %rax
	jae 1f
	ret
1:	movq %rax, %rdi
	jmp __vfork_error
.size vfork, .-vfork

/*
 * long __vfork_spawn(void (*entry)(void *), void *argument, void *stack_top);
 *
 * Starts a child that shares this address space until it execs or ends
 * (the vfork system call, 169), and runs entry(argument) in the child on
 * the stack below stack_top, so that the child never writes this
 * thread's stack.  The caller resumes when the child has execed or
 * ended, with the child's pid, or a negative errno.  entry must not
 * return; if it does, the child exits with status 127.
 */
.globl __vfork_spawn
.type __vfork_spawn, @function
__vfork_spawn:
	/* The child reads its entry, argument and stack from these three. */
	pushq %r12
	pushq %r13
	pushq %r14
	movq %rdi, %r12
	movq %rsi, %r13
	movq %rdx, %r14
	movq $169, %rax
	syscall
	testq %rax, %rax
	jnz 1f

	/* The child: its own stack before anything is written to one. */
	movq %r14, %rsp
	andq $-16, %rsp
	movq %r13, %rdi
	call *%r12

	/* entry returned: the child ends with 127 (exit is system call 1). */
	movq $1, %rax
	movq $127, %rbx
	syscall
	ud2

	/* The parent, once the child execed or ended. */
1:	popq %r14
	popq %r13
	popq %r12
	ret
.size __vfork_spawn, .-__vfork_spawn

.globl __signal_restorer
.type __signal_restorer, @function
__signal_restorer:
	movq HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET(%rsp), %rbx
	movq HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET(%rsp), %rcx
	movq $80, %rax
	int $0xc2
	ud2
.size __signal_restorer, .-__signal_restorer
