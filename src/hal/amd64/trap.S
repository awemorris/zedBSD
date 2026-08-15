#include "defs.h"

.code64
.text

.macro SAVE_AND_CALL
	pushq %rax
	pushq %rbx
	pushq %rcx
	pushq %rdx
	pushq %rsi
	pushq %rdi
	pushq %rbp
	pushq %r8
	pushq %r9
	pushq %r10
	pushq %r11
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
	cld
	movq %rsp, %rdi
	movq %rsp, %rbx
	andq $-16, %rsp
	call int_handler
	movq %rbx, %rsp
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %r11
	popq %r10
	popq %r9
	popq %r8
	popq %rbp
	popq %rdi
	popq %rsi
	popq %rdx
	popq %rcx
	popq %rbx
	popq %rax
	addq $16, %rsp
	iretq
.endm

.global amd64_common_interrupt
amd64_common_interrupt:
	SAVE_AND_CALL

.macro ISR_NOERR n
.Lfault\n:
	pushq $0
	pushq $\n
	jmp amd64_common_interrupt
.endm

.macro ISR_ERR n
.Lfault\n:
	pushq $\n
	jmp amd64_common_interrupt
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

.macro IRQ n
.Lirq\n:
	pushq $0
	pushq $(INT_IRQ_BASE + \n)
	jmp amd64_common_interrupt
.endm

IRQ 0
IRQ 1
IRQ 2
IRQ 3
IRQ 4
IRQ 5
IRQ 6
IRQ 7
IRQ 8
IRQ 9
IRQ 10
IRQ 11
IRQ 12
IRQ 13
IRQ 14
IRQ 15

.global amd64_notify_entry
amd64_notify_entry:
	pushq $0
	pushq $AMD64_VECTOR_NOTIFY
	jmp amd64_common_interrupt

.global amd64_tlb_entry
amd64_tlb_entry:
	pushq $0
	pushq $AMD64_VECTOR_TLB
	jmp amd64_common_interrupt

.global amd64_error_entry
amd64_error_entry:
	pushq $0
	pushq $AMD64_VECTOR_ERROR
	jmp amd64_common_interrupt

.global amd64_spurious_entry
amd64_spurious_entry:
	pushq $0
	pushq $AMD64_VECTOR_SPURIOUS
	jmp amd64_common_interrupt

.global amd64_syscall_entry
amd64_syscall_entry:
	pushq $0
	pushq $INT_SYSCALL
	jmp amd64_common_interrupt

.global amd64_undefined_entry
amd64_undefined_entry:
	pushq $0
	pushq $0xff
	jmp amd64_common_interrupt

.section .rodata
.balign 8
.global amd64_fault_table
amd64_fault_table:
	.quad .Lfault0, .Lfault1, .Lfault2, .Lfault3
	.quad .Lfault4, .Lfault5, .Lfault6, .Lfault7
	.quad .Lfault8, .Lfault9, .Lfault10, .Lfault11
	.quad .Lfault12, .Lfault13, .Lfault14, .Lfault15
	.quad .Lfault16, .Lfault17, .Lfault18, .Lfault19
	.quad .Lfault20, .Lfault21, .Lfault22, .Lfault23
	.quad .Lfault24, .Lfault25, .Lfault26, .Lfault27
	.quad .Lfault28, .Lfault29, .Lfault30, .Lfault31

.global amd64_irq_table
amd64_irq_table:
	.quad .Lirq0, .Lirq1, .Lirq2, .Lirq3
	.quad .Lirq4, .Lirq5, .Lirq6, .Lirq7
	.quad .Lirq8, .Lirq9, .Lirq10, .Lirq11
	.quad .Lirq12, .Lirq13, .Lirq14, .Lirq15
