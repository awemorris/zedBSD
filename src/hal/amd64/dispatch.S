#include "defs.h"

.code64
.text
.global asm_task_dispatch
.type asm_task_dispatch, @function
asm_task_dispatch:
	pushfq
	pushq %rbx
	pushq %rbp
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
	movq %rsp, (%rdi)
	movq (%rsi), %rsp
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %rbp
	popq %rbx
	popfq
	ret
.size asm_task_dispatch, .-asm_task_dispatch

.global amd64_kernel_task_entry
.type amd64_kernel_task_entry, @function
amd64_kernel_task_entry:
	popq %rax
	popq %rdi
	call *%rax
	call amd64_task_returned
	ud2
.size amd64_kernel_task_entry, .-amd64_kernel_task_entry

.global amd64_user_task_entry
.type amd64_user_task_entry, @function
amd64_user_task_entry:
	iretq
.size amd64_user_task_entry, .-amd64_user_task_entry
