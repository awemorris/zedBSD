/* SPARC V9 trap policy and initial trap self-tests. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "asi.h"
#include "defs.h"
#include "space.h"
#include "task.h"
#include "trap.h"
#include "irq.h"
#include <errno.h>

extern char sparcv9_trap_table[];
static hal_trap_handler_t trap_handlers[5];
static hal_syscall_handler_t syscall_handler;
static int user_fault_active;

static void
handle_timer(void)
{
	extern void sun4u_timer_interrupt(hal_irq_ack_t acknowledge);
	sun4u_timer_interrupt(sparcv9_irq_begin(0));
}

static int
deliver_user_fault(uintptr_t pc, uintptr_t address, int instruction,
	int write, uint64_t value)
{
	int result;

	(void)value;
	user_fault_active++;
	result = kernel_user_fault_handler(14U, 3U, pc,
	    instruction ? 0x10U : write ? 2U : 0U, address);
	user_fault_active--;
	return result;
}

void
sparcv9_user_task_prepare(uintptr_t entry, uintptr_t stack_pointer)
{
	if (!sparcv9_prime_mapping(entry, 1, 0)) {
		if (deliver_user_fault(entry, entry, 1, 0, 0) !=
		    HAL_TRAP_RET_SUCCESS || !sparcv9_prime_mapping(entry, 1, 0))
			HAL_FATAL("SPARC V9 initial user text fault failed");
	}
	if (!sparcv9_prime_mapping(stack_pointer, 0, 1)) {
		if (deliver_user_fault(entry, stack_pointer, 0, 1, 0) !=
		    HAL_TRAP_RET_SUCCESS ||
		    !sparcv9_prime_mapping(stack_pointer, 0, 1))
			HAL_FATAL("SPARC V9 initial user stack fault failed");
	}
}

void
sparcv9_trap_init(void)
{
	volatile uint64_t *probe;
	uint64_t pattern = 0x5350415243563901ULL;

	__asm__ volatile("wrpr %0, 0, %%tba\n\tflushw\n\tmembar #Sync" : :
	    "r"(sparcv9_trap_table) : "memory");
	sparcv9_window_selftest();
	hal_puts("SPARCV9 WINDOW PASS\n");
	probe = (volatile uint64_t *)(SPARCV9_DIRECT_BASE + 0x00800000UL);
	*probe = pattern;
	if (*probe != pattern)
		HAL_FATAL("SPARC V9 direct-map miss self-test failed");
	hal_puts("SPARCV9 TRAP PASS\nSPARCV9 MMU PASS\n");
}

int
sparcv9_trap_dispatch(uint64_t trap_type, uintptr_t pc, uintptr_t next_pc,
	uint64_t tstate)
{
	int cause, mode = HAL_TRAP_MODE_READ;
	uintptr_t address = 0;
	(void)next_pc;
	(void)tstate;
	if (trap_type == 0x4eU) {
		handle_timer();
		return 0;
	}
	if (trap_type == 0x64U || trap_type == 0x68U) {
		int instruction = trap_type == 0x64U;
		address = (uintptr_t)sparcv9_mmu_read(instruction ?
		    SPARCV9_ASI_IMMU : SPARCV9_ASI_DMMU,
		    SPARCV9_MMU_TAG_ACCESS) & ~SPARCV9_PAGE_MASK;
		mode = instruction ? HAL_TRAP_MODE_EXEC :
		    (sparcv9_mmu_read(SPARCV9_ASI_DMMU, 0x18) & 4U) ?
		    HAL_TRAP_MODE_WRITE : HAL_TRAP_MODE_READ;
		cause = HAL_TRAP_CAUSE_PAGE_FAULT;
	} else if (trap_type == 0x10U) {
		cause = HAL_TRAP_CAUSE_ILLEGAL_INSN;
	} else if (trap_type == 0x101U) {
		cause = HAL_TRAP_CAUSE_BREAKPOINT;
	} else if (trap_type == 0x34U) {
		cause = HAL_TRAP_CAUSE_ALIGNMENT;
	} else {
		cause = HAL_TRAP_CAUSE_MACHINE_CHECK;
	}
	if (trap_handlers[cause] != NULL &&
	    trap_handlers[cause]((void *)pc, (void *)address, mode) ==
	    HAL_TRAP_RET_SUCCESS)
		return 0;
	hal_printf("SPARCV9 trap=%llx pc=%p target=%llx access=%llx sfar=%llx\n",
	    trap_type, (void *)pc,
	    sparcv9_mmu_read(SPARCV9_ASI_DMMU, SPARCV9_MMU_TAG_TARGET),
	    sparcv9_mmu_read(SPARCV9_ASI_DMMU, SPARCV9_MMU_TAG_ACCESS),
	    sparcv9_mmu_read(SPARCV9_ASI_DMMU, 0x20));
	HAL_FATAL("unhandled SPARC V9 trap");
	return 0;
}

int
sparcv9_user_trap_dispatch(uint64_t trap_type, uintptr_t pc,
	uintptr_t next_pc, uint64_t tstate, struct sparcv9_user_trap_frame *frame)
{
	uintptr_t address;
	int instruction;
	int write;

	(void)next_pc;
	(void)tstate;
	if (frame == NULL)
		HAL_FATAL("SPARC V9 missing user trap frame");
	sparcv9_task_enter_user_frame(frame, pc, next_pc, tstate, trap_type);
	if (trap_type == 0x4eU) {
		handle_timer();
		kernel_user_return_handler();
		sparcv9_task_leave_user_frame();
		return 0;
	}
	if (trap_type == 0x16dU) {
		uintptr_t args[HAL_SYSCALL_ARGS];
		unsigned i;

		kernel_user_int_handler(0xc2U, 3U, pc,
		    frame->syscall_number);
		for (i = 0; i < HAL_SYSCALL_ARGS; i++)
			args[i] = (uintptr_t)frame->out[i];
		/* The generic callback owns accounting and its interruptible window. */
		frame->out[0] = (uint64_t)(syscall_handler != NULL ?
		    syscall_handler((uint32_t)frame->syscall_number, args) : -ENOSYS);
		kernel_user_return_handler();
		sparcv9_task_leave_user_frame();
		return 1;
	}
	if (trap_type == 0x64U || trap_type == 0x68U) {
		instruction = trap_type == 0x64U;
		address = (uintptr_t)sparcv9_mmu_read(instruction ?
		    SPARCV9_ASI_IMMU : SPARCV9_ASI_DMMU,
		    SPARCV9_MMU_TAG_ACCESS) & ~SPARCV9_PAGE_MASK;
		write = !instruction &&
		    (sparcv9_mmu_read(SPARCV9_ASI_DMMU, 0x18) & 4U) != 0;
		if (sparcv9_resolve_miss(address, instruction, write)) {
			/* A fast-path miss is still a user-return safe point.  An IPI or
			 * signal may have become pending while the mapping was resolved. */
			kernel_user_return_handler();
			sparcv9_task_leave_user_frame();
			return 0;
		}
		if (deliver_user_fault(pc, address, instruction, write,
		    frame->out[0]) == HAL_TRAP_RET_SUCCESS) {
			/*
			 * A successful generic handler either installed the mapping or
			 * queued a user signal.  Deliver pending signals before retrying;
			 * unlike a demand fault, SIGBUS/SIGSEGV intentionally has no PTE
			 * for prime_mapping() to find.
			 */
			kernel_user_return_handler();
			(void)sparcv9_prime_mapping(address, instruction, write);
			sparcv9_task_leave_user_frame();
			return 0;
		}
		HAL_FATAL("SPARC V9 user page fault handler returned");
	}
	{
		uint32_t vector;
		hal_printf("SPARCV9 user trap=%llx pc=%p npc=%p tstate=%llx o0=%llx sp=%llx\n",
		    trap_type, (void *)pc, (void *)next_pc, tstate,
		    frame->out[0], frame->old_sp + SPARCV9_STACK_BIAS);
		vector = trap_type == 0x34U ? 17U :
		    trap_type == 0x101U ? 3U : 6U;
		(void)kernel_user_fault_handler(vector, 3U, pc, 0, 0);
	}
	HAL_FATAL("SPARC V9 user fault handler returned");
	return 0;
}

void hal_syscall_set_handler(hal_syscall_handler_t h){syscall_handler=h;}

void
hal_set_trap_handler(int trap, hal_trap_handler_t handler)
{
	if (trap < 0 || trap >= 5)
		HAL_FATAL("bad SPARC V9 trap handler");
	trap_handlers[trap] = handler;
}
