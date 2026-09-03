/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 interrupt descriptor table, exception, and syscall dispatcher.
 */

#include <hal/hal.h>
#include <errno.h>
#include "int.h"
#include "task.h"
#include "irq.h"
#include "asm.h"
#include "space.h"

struct amd64_idt_entry {
	uint16_t offset_low;
	uint16_t selector;
	uint8_t ist;
	uint8_t type_attr;
	uint16_t offset_middle;
	uint32_t offset_high;
	uint32_t reserved;
} __attribute__((packed));

struct amd64_idtr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

typedef char amd64_idt_entry_size_must_be_16[
	sizeof(struct amd64_idt_entry) == 16 ? 1 : -1];

static struct amd64_idt_entry idt[256] __attribute__((aligned(16)));
static hal_syscall_handler_t syscall_handler;
static hal_trap_handler_t trap_handlers[5];

static int is_asynchronous_interrupt(int vector);
static void set_gate(unsigned vector, unsigned dpl, void *handler);
static void handle_fault(struct amd64_interrupt_frame *frame);

/*
 * Loads the amd64 interrupt descriptor table.
 */
void
amd64_int_load(
	void)
{
	struct amd64_idtr idtr;

	/* Describes and installs the complete IDT. */
	idtr.limit = sizeof(idt) - 1U;
	idtr.base = (uintptr_t)idt;
	asm_lidt(&idtr);
}

/*
 * Initializes every amd64 interrupt descriptor.
 */
void
amd64_int_init(
	void)
{
	unsigned index;

	/* Installs a safe default for every vector. */
	for (index = 0; index < 256; index++)
		set_gate(index, 0, amd64_undefined_entry);

	/* Installs the architectural exception entry points. */
	for (index = 0; index < 32; index++)
		set_gate(index, 0, amd64_fault_table[index]);

	/* Selects the dedicated double-fault stack. */
	idt[8].ist = 1;

	/* Installs the legacy IRQ entry points. */
	for (index = 0; index < 16; index++)
		set_gate(INT_IRQ_BASE + index, 0, amd64_irq_table[index]);

	/* Installs the message-signaled interrupt entry points. */
	for (index = 0; index < AMD64_VECTOR_MSI_COUNT; index++) {
		set_gate(
			AMD64_VECTOR_MSI_BASE + index,
			0,
			amd64_msi_table[index]);
	}

	/* Installs the local APIC and syscall entry points. */
	set_gate(AMD64_VECTOR_NOTIFY, 0, amd64_notify_entry);
	set_gate(AMD64_VECTOR_TLB, 0, amd64_tlb_entry);
	set_gate(AMD64_VECTOR_ERROR, 0, amd64_error_entry);
	set_gate(AMD64_VECTOR_SPURIOUS, 0, amd64_spurious_entry);
	set_gate(INT_SYSCALL, 3, amd64_syscall_entry);

	/* Publishes the initialized IDT to this CPU. */
	amd64_int_load();
}

/*
 * Selects the kernel syscall callback.
 */
void
hal_syscall_set_handler(
	hal_syscall_handler_t handler)
{
	/* Publishes the callback used by the syscall vector. */
	syscall_handler = handler;
}

/*
 * Selects a kernel trap callback.
 */
void
hal_set_trap_handler(
	int trap,
	hal_trap_handler_t handler)
{
	/* Ignores unsupported generic trap slots. */
	if (trap < 0 || trap >= 5)
		return;

	/* Publishes the callback for the selected trap. */
	trap_handlers[trap] = handler;
}

/*
 * Dispatches an amd64 interrupt frame.
 */
void
int_handler(
	struct amd64_interrupt_frame *frame)
{
	uintptr_t args[HAL_SYSCALL_ARGS];
	int vector;
	int user_interrupt;

	/* Classifies the interrupted context before dispatch. */
	vector = (int)frame->vector;
	user_interrupt = (frame->cs & 3U) == 3U &&
	    is_asynchronous_interrupt(vector);

	/*
	 * Asynchronous interrupts are a user-return safe point just like syscalls
	 * and user faults. Register the interrupted frame only when the CPU will
	 * actually return to ring 3; kernel-origin interrupts must not expose a
	 * kernel frame to signal delivery.
	 */
	if (user_interrupt)
		amd64_task_enter_user_frame(frame);

	/* Dispatches the vector to its single architectural owner. */
	if (vector >= INT_IRQ_BASE && vector <= INT_IRQ_BASE + IRQ_MAX) {
		irq_handler(vector - INT_IRQ_BASE);
	} else if (vector >= AMD64_VECTOR_MSI_BASE &&
	    vector < AMD64_VECTOR_MSI_BASE + AMD64_VECTOR_MSI_COUNT) {
		irq_handler(IRQ_MSI_BASE + vector - AMD64_VECTOR_MSI_BASE);
	} else if (vector == AMD64_VECTOR_NOTIFY) {
		amd64_notify_interrupt();
	} else if (vector == AMD64_VECTOR_TLB) {
		amd64_tlb_interrupt();
	} else if (vector == AMD64_VECTOR_ERROR) {
		amd64_error_interrupt();
	} else if (vector == AMD64_VECTOR_SPURIOUS) {
		/* Leaves the unacknowledged APIC spurious vector untouched. */
	} else if (vector == INT_SYSCALL && (frame->cs & 3U) == 3U) {
		/* Reports the userspace software interrupt to the kernel. */
		kernel_user_int_handler(
			(uint32_t)vector,
			(uint32_t)frame->cs,
			frame->rip,
			frame->rax);

		/* Copies syscall arguments in the established register order. */
		args[0] = (uintptr_t)frame->rbx;
		args[1] = (uintptr_t)frame->rcx;
		args[2] = (uintptr_t)frame->rdx;
		args[3] = (uintptr_t)frame->rsi;
		args[4] = (uintptr_t)frame->rdi;
		args[5] = (uintptr_t)frame->rbp;

		/* Exposes the user frame throughout the interruptible syscall. */
		amd64_task_enter_user_frame(frame);

		/* Lets the generic callback own accounting and interruption. */
		if (syscall_handler != NULL) {
			frame->rax = (uint64_t)syscall_handler(
				(uint32_t)frame->rax,
				args);
		} else {
			frame->rax = (uint64_t)(intptr_t)-ENOSYS;
		}

		/* Completes delivery before withdrawing the user frame. */
		kernel_user_return_handler();
		amd64_task_leave_user_frame();
	} else if (vector >= 0 && vector < 32) {
		handle_fault(frame);
	} else {
		HAL_FATAL("undefined amd64 interrupt");
	}

	/* Delivers pending user work before the asynchronous return. */
	if (user_interrupt) {
		kernel_user_return_handler();
		amd64_task_leave_user_frame();
	}
}

/* Classifies vectors that can interrupt a userspace instruction. */
static int
is_asynchronous_interrupt(
	int vector)
{
	/* Accepts legacy I/O APIC vectors. */
	if (vector >= INT_IRQ_BASE && vector <= INT_IRQ_BASE + IRQ_MAX)
		return 1;

	/* Accepts message-signaled interrupt vectors. */
	if (vector >= AMD64_VECTOR_MSI_BASE &&
	    vector < AMD64_VECTOR_MSI_BASE + AMD64_VECTOR_MSI_COUNT)
		return 1;

	/* Accepts the locally generated APIC vectors. */
	if (vector == AMD64_VECTOR_NOTIFY ||
	    vector == AMD64_VECTOR_TLB ||
	    vector == AMD64_VECTOR_ERROR ||
	    vector == AMD64_VECTOR_SPURIOUS)
		return 1;

	/* Rejects synchronous and undefined vectors. */
	return 0;
}

/* Installs one interrupt-gate descriptor. */
static void
set_gate(
	unsigned vector,
	unsigned dpl,
	void *handler)
{
	struct amd64_idt_entry *entry;
	uintptr_t address;

	/* Encodes the handler address and requested privilege level. */
	address = (uintptr_t)handler;
	entry = &idt[vector];
	entry->offset_low = (uint16_t)address;
	entry->selector = SEG_KERNEL_CODE;
	entry->ist = 0;
	entry->type_attr = (uint8_t)(0x8eU | (dpl << 5));
	entry->offset_middle = (uint16_t)(address >> 16);
	entry->offset_high = (uint32_t)(address >> 32);
	entry->reserved = 0;
}

/* Handles one architectural fault. */
static void
handle_fault(
	struct amd64_interrupt_frame *frame)
{
	uintptr_t address;
	int vector;
	int mode;
	int cause;
	int handled;

	/* Decodes the architectural vector and page-fault address. */
	vector = (int)frame->vector;
	if (vector == INT_PAGEFAULT)
		address = asm_get_cr2();
	else
		address = 0;

	/* Decodes the generic access mode without changing priority. */
	if (vector == INT_PAGEFAULT && (frame->error_code & 16U) != 0) {
		mode = HAL_TRAP_MODE_EXEC;
	} else if (vector == INT_PAGEFAULT &&
	    (frame->error_code & 2U) != 0) {
		mode = HAL_TRAP_MODE_WRITE;
	} else {
		mode = HAL_TRAP_MODE_READ;
	}

	/* Maps the architectural vector onto the generic trap causes. */
	if (vector == INT_PAGEFAULT) {
		cause = HAL_TRAP_CAUSE_PAGE_FAULT;
	} else if (vector == 6) {
		cause = HAL_TRAP_CAUSE_ILLEGAL_INSN;
	} else if (vector == 3) {
		cause = HAL_TRAP_CAUSE_BREAKPOINT;
	} else if (vector == 17) {
		cause = HAL_TRAP_CAUSE_ALIGNMENT;
	} else {
		cause = HAL_TRAP_CAUSE_MACHINE_CHECK;
	}

	/* Gives a userspace fault to the kernel's user-fault path. */
	if ((frame->cs & 3U) == 3U) {
		amd64_task_enter_user_frame(frame);
		handled = kernel_user_fault_handler(
			(uint32_t)vector,
			(uint32_t)frame->cs,
			frame->rip,
			frame->error_code,
			address) == HAL_TRAP_RET_SUCCESS;

		/* Completes a handled user fault at the user-return safe point. */
		if (handled) {
			kernel_user_return_handler();
			amd64_task_leave_user_frame();
			return;
		}

		/* Withdraws the frame before reporting a fatal callback return. */
		amd64_task_leave_user_frame();
		HAL_FATAL("amd64 user fault handler returned");
	}

	/* Gives a supported kernel fault to its registered trap callback. */
	if (cause >= 0 && cause < 5 && trap_handlers[cause] != NULL) {
		handled = trap_handlers[cause](
			(void *)(uintptr_t)frame->rip,
			(void *)address,
			mode) == HAL_TRAP_RET_SUCCESS;

		/* Resumes the kernel when the registered callback handled the fault. */
		if (handled)
			return;
	}

	/* Reports the complete unhandled fault before stopping the machine. */
	hal_printf(
		"amd64 fault v=%u rip=%08X:%08X err=%08X cr2=%08X:%08X\n",
		(uint32_t)vector,
		(uint32_t)(frame->rip >> 32),
		(uint32_t)frame->rip,
		(uint32_t)frame->error_code,
		(uint32_t)(address >> 32),
		(uint32_t)address);
	HAL_FATAL("unhandled amd64 fault");
}
