/* SPARC V9 trap policy and initial trap self-tests. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "asi.h"
#include "defs.h"
#include "space.h"
#include "trap.h"
#include <kern/sched.h>
#include <errno.h>

extern char sparcv9_trap_table[];
static hal_trap_handler_t trap_handlers[5];
static hal_syscall_handler_t syscall_handler;
static hal_user_int_handler_t user_int_handler;
static hal_user_fault_handler_t user_fault_handler;
static int reschedule_pending;
static int user_fault_active;

static void
handle_timer(void)
{
	extern void sun4u_timer_interrupt(void);
	sun4u_timer_interrupt();
	if (reschedule_pending && !user_fault_active) {
		reschedule_pending = 0;
		sched_yield();
	}
}

static int
deliver_user_fault(uintptr_t pc, uintptr_t address, int instruction,
	int write, uint64 value)
{
	struct hal_user_trap trap;

	trap.vector = 14U;
	trap.cs = 3U;
	trap.eip = pc;
	trap.eax = value;
	trap.error_code = instruction ? 0x10U : write ? 2U : 0U;
	trap.fault_address = address;
	if (user_fault_handler != NULL) {
		int result;
		user_fault_active++;
		result = user_fault_handler(&trap);
		user_fault_active--;
		return result;
	}
	return HAL_TRAP_RET_FAILED;
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
	volatile uint64 *probe;
	uint64 pattern = 0x5350415243563901ULL;

	__asm__ volatile("wrpr %0, 0, %%tba\n\tflushw\n\tmembar #Sync" : :
	    "r"(sparcv9_trap_table) : "memory");
	sparcv9_window_selftest();
	hal_puts("SPARCV9 WINDOW PASS\n");
	probe = (volatile uint64 *)(SPARCV9_DIRECT_BASE + 0x00800000UL);
	*probe = pattern;
	if (*probe != pattern)
		HAL_FATAL("SPARC V9 direct-map miss self-test failed");
	hal_puts("SPARCV9 TRAP PASS\nSPARCV9 MMU PASS\n");
}

int
sparcv9_trap_dispatch(uint64 trap_type, uintptr_t pc, uintptr_t next_pc,
	uint64 tstate)
{
	(void)next_pc;
	(void)tstate;
	if (trap_type == 0x4eU) {
		handle_timer();
		return 0;
	}
	hal_printf("SPARCV9 trap=%llx pc=%p target=%llx access=%llx sfar=%llx\n",
	    trap_type, (void *)pc,
	    sparcv9_mmu_read(SPARCV9_ASI_DMMU, SPARCV9_MMU_TAG_TARGET),
	    sparcv9_mmu_read(SPARCV9_ASI_DMMU, SPARCV9_MMU_TAG_ACCESS),
	    sparcv9_mmu_read(SPARCV9_ASI_DMMU, 0x20));
	HAL_FATAL("unhandled SPARC V9 trap");
	return 0;
}

int
sparcv9_user_trap_dispatch(uint64 trap_type, uintptr_t pc,
	uintptr_t next_pc, uint64 tstate, struct sparcv9_user_trap_frame *frame)
{
	uintptr_t address;
	int instruction;
	int write;

	(void)next_pc;
	(void)tstate;
	if (frame == NULL)
		HAL_FATAL("SPARC V9 missing user trap frame");
	if (trap_type == 0x4eU) {
		handle_timer();
		return 0;
	}
	if (trap_type == 0x16dU) {
		struct hal_user_trap trap;
		uintptr_t args[HAL_SYSCALL_ARGS];
		unsigned i;

		trap.vector = 0xc2U;
		trap.cs = 3U;
		trap.eip = pc;
		trap.eax = frame->syscall_number;
		trap.error_code = 0;
		trap.fault_address = 0;
		if (user_int_handler != NULL)
			user_int_handler(&trap);
		for (i = 0; i < HAL_SYSCALL_ARGS; i++)
			args[i] = (uintptr_t)frame->out[i];
		frame->out[0] = (uint64)(syscall_handler != NULL ?
		    syscall_handler((uint32)frame->syscall_number, args) : -ENOSYS);
		if (reschedule_pending) {
			reschedule_pending = 0;
			sched_yield();
		}
		return 1;
	}
	if (trap_type == 0x64U || trap_type == 0x68U) {
		instruction = trap_type == 0x64U;
		address = (uintptr_t)sparcv9_mmu_read(instruction ?
		    SPARCV9_ASI_IMMU : SPARCV9_ASI_DMMU,
		    SPARCV9_MMU_TAG_ACCESS) & ~SPARCV9_PAGE_MASK;
		write = !instruction &&
		    (sparcv9_mmu_read(SPARCV9_ASI_DMMU, 0x18) & 4U) != 0;
		if (sparcv9_resolve_miss(address, instruction, write))
			return 0;
		if (deliver_user_fault(pc, address, instruction, write,
		    frame->out[0]) == HAL_TRAP_RET_SUCCESS &&
		    sparcv9_prime_mapping(address, instruction, write))
			return 0;
		HAL_FATAL("SPARC V9 user page fault handler returned");
	}
	{
		struct hal_user_trap trap;
		trap.vector = trap_type == 0x34U ? 17U :
		    trap_type == 0x101U ? 3U : 6U;
		trap.cs = 3U;
		trap.eip = pc;
		trap.eax = frame->out[0];
		trap.error_code = 0;
		trap.fault_address = 0;
		if (user_fault_handler != NULL)
			(void)user_fault_handler(&trap);
	}
	HAL_FATAL("SPARC V9 user fault handler returned");
	return 0;
}

void hal_syscall_set_handler(hal_syscall_handler_t h){syscall_handler=h;}
void hal_user_int_set_handler(hal_user_int_handler_t h){user_int_handler=h;}
void hal_user_fault_set_handler(hal_user_fault_handler_t h){user_fault_handler=h;}
void hal_reschedule_on_interrupt_return(void){reschedule_pending=1;}

void
hal_set_trap_handler(int trap, hal_trap_handler_t handler)
{
	if (trap < 0 || trap >= 5)
		HAL_FATAL("bad SPARC V9 trap handler");
	trap_handlers[trap] = handler;
}
