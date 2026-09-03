/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "defs.h"

.code32
.text

/*
 * general interrupt handler (c-code calling bridge)
 */
general_int_handler:
	/*
	 * At this moment:
	 *	-8(%esp) ... int num
	 *	-4(%esp) ... error code	(CPU fault only)
	 *	 0(%esp) ... eip
	 *	 4(%esp) ... cs
	 *	 8(%esp) ... eflags
	 *	12(%esp) ... esp	(interrupt on usermode only)
	 *	16(%esp) ... ss		(interrupt on usermode only)
	 *
	 * We have to do "subl $8, %esp" first.
	 */

	/* Save GPRs. */
	subl	$8, %esp
	pushl	%ds
	pushl	%es
	pushal

	/* Set the data segments. */
	movl	$(SEG_SYS_DATA), %eax
	movw	%ax, %ds
	movw	%ax, %es

	/* Call int_handler(). */
	pushl	%esp
	call	EXT_C(int_handler)
	addl	$4, %esp

	/* Restore GPRs. */
	popal
	popl	%es
	popl	%ds
	addl	$8, %esp

	/* Return to the address where the interrupted occurred. */
	iret

/*
 * Processor Fault Handlers
 */

fault_0x00:
	movl $0x00, -8(%esp)
	jmp general_int_handler

fault_0x01:
	movl $0x01, -8(%esp)
	jmp general_int_handler

fault_0x02:
	movl $0x02, -8(%esp)
	jmp general_int_handler

fault_0x03:
	movl $0x03, -8(%esp)
	jmp general_int_handler

fault_0x04:
	movl $0x04, -8(%esp)
	jmp general_int_handler;

fault_0x05:
	movl $0x05, -8(%esp)
	jmp general_int_handler

fault_0x06:
	movl $0x06, -8(%esp)
	jmp general_int_handler

fault_0x07:
	movl $0x07, -8(%esp)
	jmp general_int_handler

fault_0x08:
	movl $0x08, -4(%esp)
	addl $4, %esp
	jmp general_int_handler

fault_0x09:
	movl $0x09, -8(%esp)
	jmp general_int_handler

fault_0x0a:
	movl $0x0a, -4(%esp)
	addl $4, %esp
	jmp general_int_handler

fault_0x0b:
	movl $0x0b, -4(%esp)
	addl $4, %esp
	jmp general_int_handler

fault_0x0c:
	movl $0x0c, -4(%esp)
	addl $4, %esp
	jmp general_int_handler

fault_0x0d:
	movl $0x0d, -4(%esp)
	addl $4, %esp
	jmp general_int_handler

fault_0x0e:
	movl $0x0e, -4(%esp)
	addl $4, %esp
	jmp general_int_handler

fault_0x0f:
	movl $0x0f, -8(%esp)
	jmp general_int_handler

fault_0x10:
	movl $0x10, -8(%esp)
	jmp general_int_handler

fault_0x11:
	movl $0x11, -4(%esp)
	addl $4, %esp
	jmp general_int_handler

fault_0x12:
	movl $0x12, -8(%esp)
	jmp general_int_handler

fault_0x13:
	movl $0x13, -8(%esp)
	jmp general_int_handler

/*
 * IRQ Handlers
 */

irq_0:
	movl $(INT_IRQ_BASE+0),  -8(%esp)
	jmp general_int_handler

irq_1:
	movl $(INT_IRQ_BASE+1),  -8(%esp)
	jmp general_int_handler

irq_2:
	movl $(INT_IRQ_BASE+2),  -8(%esp)
	jmp general_int_handler

irq_3:
	movl $(INT_IRQ_BASE+3),  -8(%esp)
	jmp general_int_handler

irq_4:
	movl $(INT_IRQ_BASE+4),  -8(%esp)
	jmp general_int_handler

irq_5:
	movl $(INT_IRQ_BASE+5),  -8(%esp)
	jmp general_int_handler

irq_6:
	movl $(INT_IRQ_BASE+6),  -8(%esp)
	jmp general_int_handler

irq_7:
	movl $(INT_IRQ_BASE+7),  -8(%esp)
	jmp general_int_handler

irq_8:
	movl $(INT_IRQ_BASE+8),  -8(%esp)
	jmp general_int_handler

irq_9:
	movl $(INT_IRQ_BASE+9),  -8(%esp)
	jmp general_int_handler

irq_10:
	movl $(INT_IRQ_BASE+10), -8(%esp)
	jmp general_int_handler

irq_11:
	movl $(INT_IRQ_BASE+11), -8(%esp)
	jmp general_int_handler

irq_12:
	movl $(INT_IRQ_BASE+12), -8(%esp)
	jmp general_int_handler

irq_13:
	movl $(INT_IRQ_BASE+13), -8(%esp)
	jmp general_int_handler

irq_14:
	movl $(INT_IRQ_BASE+14), -8(%esp)
	jmp general_int_handler

irq_15:
	movl $(INT_IRQ_BASE+15), -8(%esp)
	jmp general_int_handler

.global EXT_C(_asm_cpu_notify_handler)
EXT_C(_asm_cpu_notify_handler):
	movl $(INT_CPU_NOTIFY), -8(%esp)
	jmp general_int_handler
.global EXT_C(_asm_cpu_panic_handler)
EXT_C(_asm_cpu_panic_handler):
	movl $(INT_CPU_PANIC), -8(%esp)
	jmp general_int_handler
.global EXT_C(_asm_cpu_tlb_handler)
EXT_C(_asm_cpu_tlb_handler):
	movl $(INT_CPU_TLB), -8(%esp)
	jmp general_int_handler

/*
 * "syscall" Handler
 */

.global EXT_C(_asm_syscall_int_handler)
EXT_C(_asm_syscall_int_handler):
	movl $(INT_SYSCALL), -8(%esp)
	jmp general_int_handler;

/*
 * Undefined Interrupt
 */
.global EXT_C(_asm_undefined_int_handler)
EXT_C(_asm_undefined_int_handler):
	movl $(INT_UNDEF), -8(%esp)
	jmp general_int_handler


/*
 * Interrupt Handler Table
 *
 * void *asm_fault_int_handler_tbl[32];
 */
	.section .rodata
.global EXT_C(_asm_fault_int_handler_tbl)
EXT_C(_asm_fault_int_handler_tbl):
	.long	fault_0x00
	.long	fault_0x01
	.long	fault_0x02
	.long	fault_0x03
	.long	fault_0x04
	.long	fault_0x05
	.long	fault_0x06
	.long	fault_0x07
	.long	fault_0x08
	.long	fault_0x09
	.long	fault_0x0a
	.long	fault_0x0b
	.long	fault_0x0c
	.long	fault_0x0d
	.long	fault_0x0e
	.long	fault_0x0f
	.long	fault_0x10
	.long	fault_0x11
	.long	fault_0x12
	.long	fault_0x13
	.long	EXT_C(_asm_undefined_int_handler)	# 0x14
	.long	EXT_C(_asm_undefined_int_handler)	# 0x15
	.long	EXT_C(_asm_undefined_int_handler)	# 0x16
	.long	EXT_C(_asm_undefined_int_handler)	# 0x17
	.long	EXT_C(_asm_undefined_int_handler)	# 0x18
	.long	EXT_C(_asm_undefined_int_handler)	# 0x19
	.long	EXT_C(_asm_undefined_int_handler)	# 0x1a
	.long	EXT_C(_asm_undefined_int_handler)	# 0x1b
	.long	EXT_C(_asm_undefined_int_handler)	# 0x1c
	.long	EXT_C(_asm_undefined_int_handler)	# 0x1d
	.long	EXT_C(_asm_undefined_int_handler)	# 0x1e
	.long	EXT_C(_asm_undefined_int_handler)	# 0x1f

/*
 * IRQ Handler Table
 *
 * void *asm_irq_int_handler_tbl[16];
 */
.global EXT_C(_asm_irq_int_handler_tbl)
EXT_C(_asm_irq_int_handler_tbl):
	.long	irq_0
	.long	irq_1
	.long	irq_2
	.long	irq_3
	.long	irq_4
	.long	irq_5
	.long	irq_6
	.long	irq_7
	.long	irq_8
	.long	irq_9
	.long	irq_10
	.long	irq_11
	.long	irq_12
	.long	irq_13
	.long	irq_14
	.long	irq_15
