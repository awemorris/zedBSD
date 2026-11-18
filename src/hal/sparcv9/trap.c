/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPARC V9 HAL's trap handling: what each trap type means, and which
 * kernel entry it goes to.
 *
 * The register-window and the direct-map misses never reach here: the
 * trap table's assembly handles them.  A miss on any other address is
 * resolved from the address space first and handed to the kernel only
 * when no mapping allows the access.
 */

#include <hal/hal.h>
#include "asi.h"
#include "defs.h"
#include "space.h"
#include "task.h"
#include "timer.h"
#include "trap.h"

/* The trap types the HAL tells apart. */
#define TRAP_ILLEGAL_INSTRUCTION	0x10U
#define TRAP_PRIVILEGED_OPCODE		0x11U
#define TRAP_DIVISION_BY_ZERO		0x28U
#define TRAP_MEMORY_NOT_ALIGNED		0x34U
#define TRAP_TICK_INTERRUPT		0x4eU
#define TRAP_INSTRUCTION_MISS		0x64U
#define TRAP_DATA_MISS			0x68U
#define TRAP_DATA_PROTECTION		0x6cU
#define TRAP_BREAKPOINT			0x101U
#define TRAP_SYSTEM_CALL		0x16dU

/* The trap table, in trap-table.S. */
extern char sparcv9_trap_table[];

static int trap_is_miss(uint64_t trap_type);
static uint64_t miss_tag(uint64_t trap_type);
static int miss_is_write(uint64_t trap_type);
static enum hal_trap_cause trap_cause(uint64_t trap_type);
static int deliver_user_fault(uintptr_t pc, uintptr_t address, int instruction, int write);

/*
 * Points the processor at the trap table.
 */
void
sparcv9_trap_init(
	void)
{
	/* Sets the table's base and spills the firmware's windows to its stack. */
	__asm__ volatile("wrpr %0, 0, %%tba\n\tflushw\n\tmembar #Sync" : : "r"(sparcv9_trap_table) : "memory");
}

/*
 * Brings in the entry page and the stack page of a new user program, then
 * returns to enter it.
 *
 * The program's first instruction and its first store are made with the
 * entries already in the TLB, so the entry path never takes a miss in the
 * middle of switching to user mode.
 */
void
sparcv9_user_task_prepare(
	uintptr_t entry,
	uintptr_t stack_pointer)
{
	int primed;
	int handled;

	/* Brings in the entry page, faulting it in through the kernel when it is not mapped yet. */
	primed = sparcv9_prime_mapping(entry, 1, 0);
	if (!primed) {
		handled = deliver_user_fault(entry, entry, 1, 0);
		if (handled != HAL_TRAP_RET_SUCCESS)
			HAL_FATAL("SPARC V9 initial user text fault failed");
		primed = sparcv9_prime_mapping(entry, 1, 0);
		if (!primed)
			HAL_FATAL("SPARC V9 initial user text is not mapped");
	}

	/* Brings in the stack page the same way. */
	primed = sparcv9_prime_mapping(stack_pointer, 0, 1);
	if (!primed) {
		handled = deliver_user_fault(entry, stack_pointer, 0, 1);
		if (handled != HAL_TRAP_RET_SUCCESS)
			HAL_FATAL("SPARC V9 initial user stack fault failed");
		primed = sparcv9_prime_mapping(stack_pointer, 0, 1);
		if (!primed)
			HAL_FATAL("SPARC V9 initial user stack is not mapped");
	}
}

/*
 * Handles a trap taken in privileged mode.
 */
int
sparcv9_trap_dispatch(
	uint64_t trap_type,
	uintptr_t pc,
	uintptr_t next_pc,
	uint64_t tstate)
{
	enum hal_trap_cause cause;
	enum hal_trap_mode mode;
	uintptr_t address;
	uint64_t tag;
	int instruction;
	int write;
	int miss;
	int resolved;
	int handled;

	UNUSED_PARAMETER(next_pc);
	UNUSED_PARAMETER(tstate);

	/* The tick goes to the timer, and the interrupted instruction is retried. */
	if (trap_type == TRAP_TICK_INTERRUPT) {
		sparcv9_timer_interrupt();
		return 0;
	}

	/* Tells the access apart: no miss has an address or a mode. */
	address = 0;
	instruction = 0;
	write = 0;
	mode = HAL_TRAP_MODE_NONE;
	miss = trap_is_miss(trap_type);
	if (miss) {
		/* Reads the page and the context of the miss before anything can miss again. */
		tag = miss_tag(trap_type);
		address = (uintptr_t)tag & ~(uintptr_t)SPARCV9_PAGE_MASK;
		instruction = 0;
		if (trap_type == TRAP_INSTRUCTION_MISS)
			instruction = 1;
		write = miss_is_write(trap_type);

		/* The access the kernel is told about. */
		mode = HAL_TRAP_MODE_READ;
		if (instruction) {
			mode = HAL_TRAP_MODE_EXEC;
		} else if (write) {
			mode = HAL_TRAP_MODE_WRITE;
		}

		/* A page the current space or the device window maps is written in, and retried. */
		resolved = sparcv9_resolve_miss(tag, instruction, write);
		if (resolved)
			return 0;
	}

	/* Lets the kernel handle the fault, a user copy's among them. */
	cause = trap_cause(trap_type);
	handled = kernel_sys_fault_handler(cause, mode, pc, address, (uintptr_t)trap_type, 0);
	if (handled == HAL_TRAP_RET_SUCCESS) {
		/* A fault the kernel mapped is written in, so the retry does not miss again. */
		if (miss)
			(void)sparcv9_prime_mapping(address, instruction, write);
		return 0;
	}

	/* Stops on a trap nothing handles. */
	hal_printf("SPARCV9 trap=%lx pc=%p address=%lx fault=%lx\n",
		   trap_type,
		   (void *)pc,
		   (uint64_t)address,
		   sparcv9_dmmu_read(SPARCV9_MMU_FAULT_ADDRESS));
	HAL_FATAL("unhandled SPARC V9 trap");

	/* Not reached: the fatal stop does not return. */
	return 0;
}

/*
 * Handles a trap taken in user mode.
 */
int
sparcv9_user_trap_dispatch(
	uint64_t trap_type,
	uintptr_t pc,
	uintptr_t next_pc,
	uint64_t tstate,
	struct sparcv9_user_trap_frame *frame)
{
	uintptr_t arguments[HAL_SYSCALL_ARGS];
	enum hal_trap_cause cause;
	uintptr_t address;
	uint64_t tag;
	unsigned index;
	int instruction;
	int write;
	int miss;
	int resolved;
	int handled;

	/* The trap entry always passes the frame it saved. */
	if (frame == NULL)
		HAL_FATAL("SPARC V9 missing user trap frame");

	/* Records the trap, so fork, exec and signals can change where it returns. */
	sparcv9_task_enter_user_frame(frame, pc, next_pc, tstate, trap_type);

	/* The tick goes to the timer; the user instruction is retried. */
	if (trap_type == TRAP_TICK_INTERRUPT) {
		sparcv9_timer_interrupt();
		kernel_user_return_handler();
		sparcv9_task_leave_user_frame();
		return 0;
	}

	/* A system call passes its arguments in the out registers and gets its result in the first. */
	if (trap_type == TRAP_SYSTEM_CALL) {
		for (index = 0; index < HAL_SYSCALL_ARGS; index++)
			arguments[index] = (uintptr_t)frame->out[index];
		frame->out[0] = (uint64_t)kernel_syscall_handler((uint32_t)frame->syscall_number, arguments);

		/* The trapping instruction is done; the program continues after it. */
		kernel_user_return_handler();
		sparcv9_task_leave_user_frame();
		return 1;
	}

	/* A miss is resolved from the space, or faulted in through the kernel. */
	miss = trap_is_miss(trap_type);
	if (miss) {
		/* Reads the page and the context of the miss before anything can miss again. */
		tag = miss_tag(trap_type);
		address = (uintptr_t)tag & ~(uintptr_t)SPARCV9_PAGE_MASK;
		instruction = 0;
		if (trap_type == TRAP_INSTRUCTION_MISS)
			instruction = 1;
		write = miss_is_write(trap_type);

		/*
		 * A page the space maps is written in.  The return is still a
		 * user-return point: a signal may have become pending meanwhile.
		 */
		resolved = sparcv9_resolve_miss(tag, instruction, write);
		if (resolved) {
			kernel_user_return_handler();
			sparcv9_task_leave_user_frame();
			return 0;
		}

		/*
		 * The kernel maps the page or queues a signal.  Pending signals are
		 * delivered before the retry; a signal leaves no mapping for the
		 * prime to find, and the handler runs instead.
		 */
		handled = deliver_user_fault(pc, address, instruction, write);
		if (handled == HAL_TRAP_RET_SUCCESS) {
			kernel_user_return_handler();
			(void)sparcv9_prime_mapping(address, instruction, write);
			sparcv9_task_leave_user_frame();
			return 0;
		}
		HAL_FATAL("SPARC V9 user page fault handler returned");
	}

	/* Any other trap is a fault of the program; the kernel signals it and does not return. */
	hal_printf("SPARCV9 user trap=%lx pc=%p npc=%p tstate=%lx\n",
		   trap_type,
		   (void *)pc,
		   (void *)next_pc,
		   tstate);
	cause = trap_cause(trap_type);
	(void)kernel_user_fault_handler(cause, HAL_TRAP_MODE_NONE, pc, 0, (uintptr_t)trap_type, 0);
	HAL_FATAL("SPARC V9 user fault handler returned");

	/* Not reached: the fatal stop does not return. */
	return 0;
}

/* Reports whether a trap is a TLB miss or a write to a read-only page. */
static int
trap_is_miss(
	uint64_t trap_type)
{
	/* The three MMU traps the space resolves. */
	if (trap_type == TRAP_INSTRUCTION_MISS)
		return 1;
	if (trap_type == TRAP_DATA_MISS)
		return 1;
	if (trap_type == TRAP_DATA_PROTECTION)
		return 1;

	/* Any other trap. */
	return 0;
}

/* Reads the tag access value of an MMU trap: the page and the context. */
static uint64_t
miss_tag(
	uint64_t trap_type)
{
	uint64_t tag;

	/* An instruction miss is recorded by the instruction MMU. */
	if (trap_type == TRAP_INSTRUCTION_MISS) {
		tag = sparcv9_immu_read(SPARCV9_MMU_TAG_ACCESS);
		return tag;
	}

	/* A data miss or protection fault is recorded by the data MMU. */
	tag = sparcv9_dmmu_read(SPARCV9_MMU_TAG_ACCESS);
	return tag;
}

/*
 * Reports whether an MMU trap is known to come from a store.
 *
 * A fast miss does not record whether it was a load or a store: the MMU
 * updates its fault status only for the other faults.  A miss is taken as
 * a load, and the entry written for it is writable when the mapping is; a
 * store to a page mapped read-only then takes the protection trap, which
 * is a store by definition.
 */
static int
miss_is_write(
	uint64_t trap_type)
{
	/* Only the protection trap is known to be a store. */
	if (trap_type == TRAP_DATA_PROTECTION)
		return 1;

	/* A miss is taken as a load. */
	return 0;
}

/* Reports the kernel's name for what a trap type means. */
static enum hal_trap_cause
trap_cause(
	uint64_t trap_type)
{
	/* Names each trap the kernel tells apart. */
	switch (trap_type) {
	case TRAP_INSTRUCTION_MISS:
	case TRAP_DATA_MISS:
	case TRAP_DATA_PROTECTION:
		return HAL_TRAP_CAUSE_PAGE_FAULT;
	case TRAP_ILLEGAL_INSTRUCTION:
	case TRAP_PRIVILEGED_OPCODE:
		return HAL_TRAP_CAUSE_ILLEGAL_INSN;
	case TRAP_BREAKPOINT:
		return HAL_TRAP_CAUSE_BREAKPOINT;
	case TRAP_MEMORY_NOT_ALIGNED:
		return HAL_TRAP_CAUSE_ALIGNMENT;
	case TRAP_DIVISION_BY_ZERO:
		return HAL_TRAP_CAUSE_ARITHMETIC;
	default:
		break;
	}

	/* Any other trap. */
	return HAL_TRAP_CAUSE_OTHER;
}

/* Hands a user page fault to the kernel and reports what it did. */
static int
deliver_user_fault(
	uintptr_t pc,
	uintptr_t address,
	int instruction,
	int write)
{
	enum hal_trap_mode mode;
	uintptr_t vector;
	int handled;

	/* Names the access and the trap it came from. */
	mode = HAL_TRAP_MODE_READ;
	vector = TRAP_DATA_MISS;
	if (instruction) {
		mode = HAL_TRAP_MODE_EXEC;
		vector = TRAP_INSTRUCTION_MISS;
	} else if (write) {
		mode = HAL_TRAP_MODE_WRITE;
	}

	/* The kernel maps the page or queues a signal. */
	handled = kernel_user_fault_handler(HAL_TRAP_CAUSE_PAGE_FAULT, mode, pc, address, vector, 0);

	/* Reports the kernel's answer. */
	return handled;
}
