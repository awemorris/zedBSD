/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The i386 interrupt-dispatch implementation.
 */

#include <hal/hal.h>

#include <errno.h>

#include "asm.h"
#include "int.h"
#include "irq.h"
#include "pic.h"
#include "space.h"
#include "task.h"

static int is_asynchronous_interrupt(int int_num);
static void create_idt(void);
static void load_idt(void);
static void set_idt_entry(int index, int dpl, void *handler);
static int trap_cause(int vector);
static void handle_fault(struct interrupt_frame *fp);

/*
 * Initializes the i386 interrupt descriptor table.
 */
void
i386_int_init(
	void)
{
	/* Creates and installs the descriptor table while IRQs remain disabled. */
	create_idt();
	load_idt();
}

/*
 * Reloads the i386 interrupt descriptor table on the current CPU.
 */
void
i386_int_load(
	void)
{
	/* Installs the shared descriptor-table image on this CPU. */
	load_idt();
}

/* Maps an x86 exception vector onto the generic trap cause. */
static int
trap_cause(
	int vector)
{
	/* Page faults carry an address and an access mode. */
	if (vector == 14)
		return HAL_TRAP_CAUSE_PAGE_FAULT;

	/* Instruction and breakpoint traps. */
	if (vector == 6)
		return HAL_TRAP_CAUSE_ILLEGAL_INSN;
	if (vector == 3)
		return HAL_TRAP_CAUSE_BREAKPOINT;
	if (vector == 17)
		return HAL_TRAP_CAUSE_ALIGNMENT;
	if (vector == 18)
		return HAL_TRAP_CAUSE_MACHINE_CHECK;

	/* Divide error, overflow, x87 and SIMD exceptions. */
	if (vector == 0 || vector == 4 || vector == 16 || vector == 19)
		return HAL_TRAP_CAUSE_ARITHMETIC;

	/* Invalid TSS, segment not present, stack and general protection. */
	if (vector >= 10 && vector <= 13)
		return HAL_TRAP_CAUSE_PROTECTION;

	/* Everything else is reported with its raw vector only. */
	return HAL_TRAP_CAUSE_OTHER;
}

/*
 * Dispatches one i386 interrupt frame.
 *
 * The assembly entry stubs install these handlers as interrupt gates, so the
 * processor enters this function with interrupts disabled.
 */
void
int_handler(
	struct interrupt_frame *fp)
{
	uintptr_t args[HAL_SYSCALL_ARGS];
	uint32_t syscall_result;
	int in_service;
	int int_num;
	int irq_num;
	int is_handled;
	int user_interrupt;

	/* Classifies the interrupt without disturbing short-circuit call order. */
	is_handled = 0;
	int_num = fp->int_num;
	user_interrupt = 0;

	/* Recognizes asynchronous interrupts which can return to user mode. */
	if ((fp->cs & 3U) == 3U) {
		user_interrupt = is_asynchronous_interrupt(int_num);
	}

	/* Makes an asynchronous user frame available for signal delivery. */
	if (user_interrupt)
		i386_task_enter_user_frame(fp);

	/* Dispatches the vector according to its reserved interrupt class. */
	if (int_num >= INT_IRQ_BASE && int_num <= INT_IRQ_BASE + IRQ_MAX) {
		/* Validates that hardware, rather than a user INT, raised the IRQ. */
		irq_num = int_num - INT_IRQ_BASE;
		in_service = i386_interrupt_validate(irq_num);

		/* Rejects a vector which the active controller did not report. */
		if (in_service == -1) {
			is_handled = 1;
			hal_printf(
				"\nIRQ not in service (pic_isr=%02X, int=%02X)\n",
				in_service,
				int_num);
			HAL_FATAL("IRQ error");
		} else {
			/* Delivers the validated IRQ to the registered service. */
			irq_handler(irq_num);
			is_handled = 1;
		}
	} else if (int_num == INT_SYSCALL && (fp->cs & 3U) == 3U) {
		/* Captures the ABI argument registers. */
		args[0] = fp->regs.ebx;
		args[1] = fp->regs.ecx;
		args[2] = fp->regs.edx;
		args[3] = fp->regs.esi;
		args[4] = fp->regs.edi;
		args[5] = fp->regs.ebp;
		i386_task_enter_user_frame(fp);

		/* Lets the generic kernel own accounting and interruption. */
		syscall_result = (uint32_t)kernel_syscall_handler(
			fp->regs.eax,
			args);
		fp->regs.eax = syscall_result;

		/* Completes accounting before releasing the active user frame. */
		kernel_user_return_handler();
		i386_task_leave_user_frame();
		is_handled = 1;
	} else if (int_num == INT_CPU_NOTIFY) {
		/* Delivers the scheduler notification for the current CPU. */
		kernel_cpu_notify_handler(hal_cpu_current(), IRQ_MAX + 2U);
		is_handled = 1;
	} else if (int_num == INT_CPU_TLB) {
		/* Services pending remote TLB invalidations. */
		i386_tlb_interrupt();
		is_handled = 1;
	} else if (int_num == INT_CPU_PANIC) {
		/* Parks this CPU after a remote panic request. */
		hal_cpu_park();
	} else if (int_num >= 0 && int_num <= 0x1f) {
		/* Handles processor-defined fault vectors. */
		handle_fault(fp);
		is_handled = 1;
	}

	/* Rejects every vector outside the installed dispatch classes. */
	if (!is_handled) {
		hal_printf("\nIRQ handler not installed (int %02X)\n", int_num);
		HAL_FATAL("IRQ error");
	}

	/* Completes signal delivery for an asynchronous user interruption. */
	if (user_interrupt) {
		kernel_user_return_handler();
		i386_task_leave_user_frame();
	}
}

/* Tests whether a vector is delivered asynchronously. */
static int
is_asynchronous_interrupt(
	int int_num)
{
	/* Accepts any hardware IRQ vector. */
	if (int_num >= INT_IRQ_BASE && int_num <= INT_IRQ_BASE + IRQ_MAX)
		return 1;

	/* Accepts the interprocessor notification vector. */
	if (int_num == INT_CPU_NOTIFY)
		return 1;

	/* Accepts the interprocessor TLB vector. */
	if (int_num == INT_CPU_TLB)
		return 1;

	/* Rejects synchronous vectors. */
	return 0;
}

/* Builds the shared interrupt descriptor table. */
static void
create_idt(
	void)
{
	int i;

	/* Installs the undefined-vector handler in every table entry. */
	for (i = 0; i < 256; i++)
		set_idt_entry(i, 0, _asm_undefined_int_handler);

	/* Installs the processor-fault entry stubs. */
	for (i = 0; i < 32; i++)
		set_idt_entry(i, 0, _asm_fault_int_handler_tbl[i]);

	/* Installs the legacy IRQ entry stubs. */
	for (i = 0; i < 16; i++) {
		set_idt_entry(
			i + INT_IRQ_BASE,
			0,
			_asm_irq_int_handler_tbl[i]);
	}

	/* Installs the system-call and interprocessor entry stubs. */
	set_idt_entry(INT_SYSCALL, 3, _asm_syscall_int_handler);
	set_idt_entry(INT_CPU_NOTIFY, 0, _asm_cpu_notify_handler);
	set_idt_entry(INT_CPU_PANIC, 0, _asm_cpu_panic_handler);
	set_idt_entry(INT_CPU_TLB, 0, _asm_cpu_tlb_handler);
}

/* Populates one interrupt descriptor table entry. */
static void
set_idt_entry(
	int index,
	int dpl,
	void *handler)
{
	uint8_t *entry;

	/* Locates the selected descriptor in the shared table. */
	entry = (uint8_t *)(ADDR_IDT | SYS_START) + 8 * index;

	/* Configures an interrupt gate at the requested privilege level. */
	entry[5] = 0x8e | (dpl << 5);

	/* Clears the unused gate copy count. */
	entry[4] = 0;

	/* Encodes the handler offset and kernel code selector. */
	*(uint16_t *)&entry[0] = (uint16_t)((uint32_t)handler & 0xffffU);
	*(uint16_t *)&entry[6] = (uint16_t)((uint32_t)handler >> 16);
	*(uint16_t *)&entry[2] = SEG_SYS_CODE;
}

/* Installs the shared interrupt descriptor table. */
static void
load_idt(
	void)
{
	uint8_t idtr[6];

	/* Encodes the descriptor-table extent and linear address. */
	*(uint16_t *)&idtr[0] = 8 * 256 - 1;
	*(uint32_t *)&idtr[2] = ADDR_IDT + SYS_START;

	/* Loads the prepared descriptor-table register. */
	asm_lidt(idtr);
}

/* Handles one processor fault frame. */
static void
handle_fault(
	struct interrupt_frame *fp)
{
	uint32_t fault_address;
	int handled;
	int int_num;
	int cause;
	int mode;

	/* Captures the processor fault number. */
	int_num = fp->int_num;

	/* Captures CR2 and the access mode only for a page fault. */
	fault_address = 0;
	mode = HAL_TRAP_MODE_NONE;
	if (int_num == INT_PAGEFAULT) {
		fault_address = asm_get_cr2();
		if ((fp->error_code & 16U) != 0)
			mode = HAL_TRAP_MODE_EXEC;
		else if ((fp->error_code & 2U) != 0)
			mode = HAL_TRAP_MODE_WRITE;
		else
			mode = HAL_TRAP_MODE_READ;
	}

	/* Maps the architectural vector onto the generic trap causes. */
	cause = trap_cause(int_num);

	/* Delegates user faults while retaining kernel faults for diagnostics. */
	if ((fp->cs & 3U) != 0U) {
		/* Offers the active user frame before observing fault state. */
		i386_task_enter_user_frame(fp);

		/* Delegates the captured fault to the generic entry. */
		handled = kernel_user_fault_handler(
			cause,
			mode,
			fp->eip,
			fault_address,
			(uintptr_t)int_num,
			fp->error_code);

		/* Completes a successfully handled user fault. */
		if (handled == HAL_TRAP_RET_SUCCESS) {
			kernel_user_return_handler();
			i386_task_leave_user_frame();
			return;
		}

		/* Rejects an unhandled user fault after releasing its frame. */
		i386_task_leave_user_frame();
		HAL_FATAL("user fault handler returned");
	} else {
		/* Gives a supervisor fault to the kernel's fixed entry first. */
		handled = kernel_sys_fault_handler(
			cause,
			mode,
			fp->eip,
			fault_address,
			(uintptr_t)int_num,
			fp->error_code);
		if (handled == HAL_TRAP_RET_SUCCESS)
			return;

		/* Reports the complete kernel fault register frame. */
		hal_printf(
			"[INT] int 0x%02X handled!\n"
			"CS:  %04X  EIP: %08X\n"
			"DS:  %04X  ES:  %04X\n"
			"EAX: %08X  EBX: %08X  ECX: %08X  EDX: %08X\n"
			"ESI: %08X  EDI: %08X  EBP: %08X\n"
			"EFLAGS: %08X  ERRORCODE: %08X\n",
			int_num,
			fp->cs,
			fp->eip,
			fp->regs.ds,
			fp->regs.es,
			fp->regs.eax,
			fp->regs.ebx,
			fp->regs.ecx,
			fp->regs.edx,
			fp->regs.esi,
			fp->regs.edi,
			fp->regs.ebp,
			fp->eflags,
			fp->error_code);
	}

	/* Halts permanently after an unrecoverable processor fault. */
	for (;;)
		asm_hlt();
}
