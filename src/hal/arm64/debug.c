/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Debugging a task on AArch64: its registers as a debugger reads and
 * writes them, stepping one instruction, and the hardware breakpoints and
 * watchpoints.
 *
 * A stopped task's user registers are the exception frame it entered the
 * kernel through; its floating-point registers are the area the task
 * switch saves them to.  Stepping and the hardware points are properties
 * of the task that the processor holds only while the task runs, so they
 * are kept in the task and loaded at each task switch.
 */

#include <hal/hal.h>
#include "asm.h"
#include "int.h"
#include "task.h"
#include "percpu.h"

/*
 * The debug control register (MDSCR_EL1): software step, and the enable
 * of breakpoint and watchpoint exceptions.
 */
#define ARM64_MDSCR_SS			(1ULL << 0)
#define ARM64_MDSCR_MDE			(1ULL << 15)

/*
 * The saved processor state (SPSR_EL1): the step bit, the condition flags
 * a program may change, and the exception level and stack field, which
 * must say EL0 for a frame that returns to user.
 */
#define ARM64_SPSR_SS			(1ULL << 21)
#define ARM64_SPSR_NZCV			0x00000000f0000000ULL
#define ARM64_SPSR_MODE			0x000000000000000fULL

/*
 * The first address above the user half.  A value at or above it in the
 * return frame would fault on the way out of the kernel.
 */
#define ARM64_DEBUG_USER_LIMIT		0x0001000000000000ULL

/*
 * The number of hardware points of each kind this port uses, whatever
 * the processor has beyond them.
 */
#define ARM64_DEBUG_BANK_MAX		4U

/*
 * Breakpoint control (DBGBCR<n>_EL1): enabled, matching at EL0 only, on
 * any of the four bytes of an A64 instruction.
 */
#define ARM64_DBGBCR_EL0_EXECUTE	((0xfULL << 5) | (2ULL << 1) | 1ULL)

/*
 * Watchpoint control (DBGWCR<n>_EL1): enabled, matching at EL0 only, the
 * kind of access in the load/store field, and the watched bytes of the
 * aligned doubleword in the byte-select field.
 */
#define ARM64_DBGWCR_ENABLE_EL0		((2ULL << 1) | 1ULL)
#define ARM64_DBGWCR_LOAD		(1ULL << 3)
#define ARM64_DBGWCR_STORE		(2ULL << 3)
#define ARM64_DBGWCR_BAS_SHIFT		5U

/*
 * How many breakpoints and watchpoints the processor has, read once from
 * ID_AA64DFR0_EL1 when the debug hardware is set up and never changed.
 */
static unsigned debug_breakpoint_count;
static unsigned debug_watchpoint_count;

/*
 * Whether a CPU's breakpoint and watchpoint registers hold the enabled
 * points of its running task is kept in that CPU's record
 * (debug_points_loaded): a switch to a task with no points clears them only
 * when there is something to clear.
 */

static struct arm64_exception_frame *task_user_frame(struct arm64_task *task);
static int user_address_valid(uint64_t value);
static uint64_t read_mdscr(void);
static void write_mdscr(uint64_t value);
static void write_breakpoint(unsigned index, uint64_t address, uint64_t control);
static void write_watchpoint(unsigned index, uint64_t address, uint64_t control);
static void load_debug_points(struct arm64_task *task);
static void clear_debug_points(void);
static int watchpoint_control(const struct hal_debug_point *point, uint64_t *address, uint64_t *control);

/*
 * Sets up the debug hardware and reads how many points it has.
 */
void
arm64_debug_init(void)
{
	uint64_t features;

	/*
	 * The OS lock is set at reset and suppresses every debug exception
	 * but BRK; releasing it lets stepping and the hardware points work.
	 */
	__asm__ volatile("msr oslar_el1, xzr" : : : "memory");
	__asm__ volatile("msr osdlr_el1, xzr" : : : "memory");
	__asm__ volatile("isb" : : : "memory");

	/* Reads the number of breakpoints and watchpoints, each stored minus one. */
	__asm__ volatile("mrs %0, id_aa64dfr0_el1" : "=r"(features));
	debug_breakpoint_count = (unsigned)((features >> 12) & 0xfU) + 1U;
	debug_watchpoint_count = (unsigned)((features >> 20) & 0xfU) + 1U;

	/* Uses no more of each bank than this port has registers named for. */
	if (debug_breakpoint_count > ARM64_DEBUG_BANK_MAX)
		debug_breakpoint_count = ARM64_DEBUG_BANK_MAX;
	if (debug_watchpoint_count > ARM64_DEBUG_BANK_MAX)
		debug_watchpoint_count = ARM64_DEBUG_BANK_MAX;

	/* Starts with every point disabled. */
	clear_debug_points();

	/*
	 * Enables breakpoint and watchpoint exceptions.  No point matches
	 * until one is enabled, and the points match at EL0 only, so the
	 * kernel's own accesses to user memory never stop on them.
	 */
	write_mdscr(read_mdscr() | ARM64_MDSCR_MDE);
}

/*
 * Loads the stepping and the hardware points of the task about to run.
 *
 * The caller has interrupts off: the registers written are this CPU's,
 * and so is the record that says whether they hold a task's points.
 */
void
arm64_debug_switch(
	struct arm64_task *to)
{
	struct arm64_percpu *cpu;
	uint64_t control;
	uint64_t wanted;

	/*
	 * The step bit of the control register arms stepping for the next
	 * return to EL0; it stays clear for a task that is not stepped, or
	 * that task would stop before its first instruction.
	 */
	control = read_mdscr();
	wanted = control & ~ARM64_MDSCR_SS;
	if (to->single_step)
		wanted |= ARM64_MDSCR_SS;

	/* Writes the control register only when it changes. */
	if (wanted != control)
		write_mdscr(wanted);

	/* Loads the task's points, or clears the previous task's. */
	cpu = arm64_percpu_current();
	if (to->debug_point_count != 0U) {
		load_debug_points(to);
	} else if (cpu->debug_points_loaded) {
		clear_debug_points();
	}
}

/*
 * Reads the user integer registers of a task.
 */
int
hal_task_get_user_gpregs(
	hal_task_t handle,
	struct hal_gpregs *registers)
{
	struct arm64_task *task;
	struct arm64_task *running;
	const struct arm64_exception_frame *frame;
	uint64_t thread_pointer;
	unsigned index;

	/* Names the task, and the task now on the processor. */
	task = handle;
	running = arm64_task_running();

	/* Requires a task with a user frame and somewhere to report. */
	if (registers == NULL)
		return -1;
	frame = task_user_frame(task);
	if (frame == NULL)
		return -1;

	/* Copies the general registers the task entered the kernel with. */
	for (index = 0; index < 31U; index++)
		registers->x[index] = frame->x[index];

	/* Copies the stack pointer, the next instruction and the processor state. */
	registers->sp = frame->user_sp;
	registers->pc = frame->elr;
	registers->pstate = frame->spsr;

	/*
	 * The thread pointer lives in the register while its task runs, and
	 * is written back to the task when the task leaves the processor.
	 */
	thread_pointer = (uint64_t)task->tls;
	if (task == running)
		__asm__ volatile("mrs %0, tpidr_el0" : "=r"(thread_pointer));
	registers->tpidr = thread_pointer;

	/* Succeeded. */
	return 0;
}

/*
 * Writes the user integer registers of a task.
 */
int
hal_task_set_user_gpregs(
	hal_task_t handle,
	const struct hal_gpregs *registers)
{
	struct arm64_task *task;
	struct arm64_task *running;
	struct arm64_exception_frame *frame;
	uint64_t irq;
	int valid;
	unsigned index;

	/* Names the task, and the task now on the processor. */
	task = handle;
	running = arm64_task_running();

	/* Requires a task with a user frame and something to write. */
	if (registers == NULL)
		return -1;
	frame = task_user_frame(task);
	if (frame == NULL)
		return -1;

	/*
	 * The next instruction and the stack pointer are what the return to
	 * EL0 uses, so a value outside the user half is refused here rather
	 * than faulting in the kernel.
	 */
	valid = user_address_valid(registers->pc);
	if (!valid)
		return -1;
	valid = user_address_valid(registers->sp);
	if (!valid)
		return -1;

	/* Writes the general registers the task returns with. */
	for (index = 0; index < 31U; index++)
		frame->x[index] = registers->x[index];
	frame->user_sp = registers->sp;
	frame->elr = registers->pc;

	/*
	 * Only the condition flags are the program's.  The exception level,
	 * the interrupt masks and the step bit belong to the kernel and keep
	 * the values the frame has.
	 */
	frame->spsr = (frame->spsr & ~ARM64_SPSR_NZCV) |
	    (registers->pstate & ARM64_SPSR_NZCV);

	/*
	 * The thread pointer follows the running task into the register, with
	 * interrupts off so that a task switch between the two stores does
	 * not save the old register over the new value.
	 */
	irq = arm64_irq_save();

	task->tls = (uintptr_t)registers->tpidr;
	if (task == running)
		__asm__ volatile("msr tpidr_el0, %0" : : "r"(registers->tpidr));

	arm64_irq_restore(irq);

	/* Succeeded. */
	return 0;
}

/*
 * Reads the floating-point and SIMD registers of a task.
 */
int
hal_task_get_user_fpregs(
	hal_task_t handle,
	struct hal_fpregs *registers)
{
	struct arm64_task *task;
	struct arm64_task *running;
	uint64_t control;
	uint64_t status;

	/* Names the task, and the task now on the processor. */
	task = handle;
	running = arm64_task_running();

	/* Requires a task and somewhere to report. */
	if (task == NULL || registers == NULL)
		return -1;

	/* Brings the live registers of a running task into its saved area. */
	if (task == running)
		arm64_fp_save(task->fpregs);

	/*
	 * The saved area holds the thirty-two registers, then the control
	 * register and the status register, eight bytes each.
	 */
	hal_memcpy(registers->v, task->fpregs, sizeof(registers->v));
	hal_memcpy(&control, task->fpregs + 512, sizeof(control));
	hal_memcpy(&status, task->fpregs + 520, sizeof(status));
	registers->fpcr = (uint32_t)control;
	registers->fpsr = (uint32_t)status;

	/* Succeeded. */
	return 0;
}

/*
 * Writes the floating-point and SIMD registers of a task.
 */
int
hal_task_set_user_fpregs(
	hal_task_t handle,
	const struct hal_fpregs *registers)
{
	struct arm64_task *task;
	struct arm64_task *running;
	uint64_t control;
	uint64_t status;
	uint64_t irq;

	/* Names the task, and the task now on the processor. */
	task = handle;
	running = arm64_task_running();

	/* Requires a task and something to write. */
	if (task == NULL || registers == NULL)
		return -1;

	/* The control and the status register, eight bytes each in the saved area. */
	control = registers->fpcr;
	status = registers->fpsr;

	/*
	 * Replaces the task's registers.  While the task runs, its live
	 * registers are the truth and a task switch saves them over the saved
	 * area, so the save, the write and the load go with interrupts off: a
	 * switch between them would lose the new values.
	 */
	irq = arm64_irq_save();

	/* Keeps the rest of a running task's live state in its saved area. */
	if (task == running)
		arm64_fp_save(task->fpregs);

	/* Writes the registers into the saved area in its own layout. */
	hal_memcpy(task->fpregs, registers->v, sizeof(registers->v));
	hal_memcpy(task->fpregs + 512, &control, sizeof(control));
	hal_memcpy(task->fpregs + 520, &status, sizeof(status));

	/* A running task takes the new values at once. */
	if (task == running)
		arm64_fp_restore(task->fpregs);

	arm64_irq_restore(irq);

	/* Succeeded. */
	return 0;
}

/*
 * Reports that this processor has no vector state beyond the
 * floating-point registers.
 */
int
hal_task_get_user_vregs(
	hal_task_t handle,
	struct hal_vregs *registers)
{
	(void)handle;
	(void)registers;

	/* Refuses: the set is empty on this architecture. */
	return -1;
}

/*
 * Reports that this processor has no vector state beyond the
 * floating-point registers.
 */
int
hal_task_set_user_vregs(
	hal_task_t handle,
	const struct hal_vregs *registers)
{
	(void)handle;
	(void)registers;

	/* Refuses: the set is empty on this architecture. */
	return -1;
}

/*
 * Asks that a task execute one instruction and then trap.
 */
int
hal_task_set_single_step(
	hal_task_t handle,
	int enable)
{
	struct arm64_task *task;
	struct arm64_task *running;
	struct arm64_exception_frame *frame;
	uint64_t irq;

	/* Names the task, and the task now on the processor. */
	task = handle;
	running = arm64_task_running();

	/* Requires a task with a user frame. */
	frame = task_user_frame(task);
	if (frame == NULL)
		return -1;

	/*
	 * The step bit of the saved state lets exactly one instruction run
	 * after the return to EL0; the control register's step bit, loaded
	 * with the task, makes the processor trap after it.
	 */
	if (enable) {
		task->single_step = 1;
		frame->spsr |= ARM64_SPSR_SS;
	} else {
		task->single_step = 0;
		frame->spsr &= ~ARM64_SPSR_SS;
	}

	/*
	 * A running task takes the request at once, on the CPU it runs on:
	 * with interrupts off, so that it is not moved to another CPU while
	 * the registers and the CPU's record are written.
	 */
	irq = arm64_irq_save();

	if (task == running)
		arm64_debug_switch(task);

	arm64_irq_restore(irq);

	/* Succeeded. */
	return 0;
}

/*
 * Reports whether a task was asked to execute one instruction.
 */
int
hal_task_get_single_step(
	hal_task_t handle,
	int *enable)
{
	struct arm64_task *task;

	/* Sees the handle as the task it names. */
	task = handle;

	/* Requires a task and somewhere to report. */
	if (task == NULL || enable == NULL)
		return -1;
	*enable = task->single_step;

	/* Succeeded. */
	return 0;
}

/*
 * Gives a task the complete set of debug points it is to run with.
 */
int
hal_task_set_debug_points(
	hal_task_t handle,
	const struct hal_debug_point *points,
	unsigned count)
{
	struct arm64_task *task;
	struct arm64_task *running;
	uint64_t address;
	uint64_t control;
	uint64_t irq;
	unsigned breakpoints;
	unsigned watchpoints;
	unsigned index;
	int error;

	/* Names the task, and the task now on the processor. */
	task = handle;
	running = arm64_task_running();

	/* Requires a task, and a set no larger than the interface allows. */
	if (task == NULL || count > HAL_DEBUG_POINT_MAX)
		return -1;
	if (count != 0U && points == NULL)
		return -1;

	/* Checks that every point can be expressed and that each bank has room. */
	breakpoints = 0;
	watchpoints = 0;
	for (index = 0; index < count; index++) {
		/* An instruction point covers the one instruction at an aligned address. */
		if (points[index].kind == HAL_DEBUG_KIND_EXECUTE) {
			if (points[index].length != 1U)
				return -1;
			if ((points[index].address & 3U) != 0U)
				return -1;
			breakpoints++;
			continue;
		}

		/* A data point must fit the byte-select field of a watchpoint. */
		error = watchpoint_control(&points[index], &address, &control);
		if (error != 0)
			return -1;
		watchpoints++;
	}

	/* Refuses a set that needs more registers of a kind than exist. */
	if (breakpoints > debug_breakpoint_count)
		return -1;
	if (watchpoints > debug_watchpoint_count)
		return -1;

	/* Records the accepted set. */
	for (index = 0; index < count; index++)
		task->debug_points[index] = points[index];
	task->debug_point_count = count;

	/*
	 * A running task takes them immediately, on the CPU it runs on: with
	 * interrupts off, so that it is not moved to another CPU half way
	 * through, leaving its points in the old CPU's registers while the
	 * old CPU's record says they are not loaded.
	 */
	irq = arm64_irq_save();

	if (task == running)
		arm64_debug_switch(task);

	arm64_irq_restore(irq);

	/* Succeeded. */
	return 0;
}

/*
 * Reports the debug points a task runs with.
 */
int
hal_task_get_debug_points(
	hal_task_t handle,
	struct hal_debug_point *points,
	unsigned capacity,
	unsigned *count)
{
	struct arm64_task *task;
	unsigned index;

	/* Sees the handle as the task it names. */
	task = handle;

	/* Requires a task and somewhere to report the number. */
	if (task == NULL || count == NULL)
		return -1;

	/* Requires room for every point the task has. */
	if (task->debug_point_count > capacity)
		return -1;
	if (task->debug_point_count != 0U && points == NULL)
		return -1;

	/* Copies out the points the task runs with. */
	for (index = 0; index < task->debug_point_count; index++)
		points[index] = task->debug_points[index];
	*count = task->debug_point_count;

	/* Succeeded. */
	return 0;
}

/*
 * The frame a stopped task returns to EL0 through, or NULL when the task
 * has none to show.
 */
static struct arm64_exception_frame *
task_user_frame(
	struct arm64_task *task)
{
	struct arm64_exception_frame *frame;

	/* Requires a task. */
	if (task == NULL)
		return NULL;
	frame = task->active_user_frame;

	/* Requires a frame that returns to EL0. */
	if (frame == NULL)
		return NULL;
	if ((frame->spsr & ARM64_SPSR_MODE) != 0ULL)
		return NULL;

	/* Returns the user frame. */
	return frame;
}

/*
 * Tells whether an address lies in the user half.
 */
static int
user_address_valid(
	uint64_t value)
{
	/* Refuses an address in the kernel half or above the user range. */
	if (value >= ARM64_DEBUG_USER_LIMIT)
		return 0;

	/* Reports a user address. */
	return 1;
}

/*
 * Reads the debug control register.
 */
static uint64_t
read_mdscr(void)
{
	uint64_t value;

	/* Reads the register. */
	__asm__ volatile("mrs %0, mdscr_el1" : "=r"(value));

	/* Reports the register. */
	return value;
}

/*
 * Writes the debug control register, in effect for the next instruction.
 */
static void
write_mdscr(
	uint64_t value)
{
	__asm__ volatile("msr mdscr_el1, %0\n\tisb" : : "r"(value) : "memory");
}

/*
 * Writes one breakpoint's address and control.  The registers have a name
 * each rather than an index, so the index selects the instruction.
 */
static void
write_breakpoint(
	unsigned index,
	uint64_t address,
	uint64_t control)
{
	/* Selects the register pair of the point. */
	switch (index) {
	case 0:
		__asm__ volatile("msr dbgbvr0_el1, %0" : : "r"(address));
		__asm__ volatile("msr dbgbcr0_el1, %0" : : "r"(control));
		break;
	case 1:
		__asm__ volatile("msr dbgbvr1_el1, %0" : : "r"(address));
		__asm__ volatile("msr dbgbcr1_el1, %0" : : "r"(control));
		break;
	case 2:
		__asm__ volatile("msr dbgbvr2_el1, %0" : : "r"(address));
		__asm__ volatile("msr dbgbcr2_el1, %0" : : "r"(control));
		break;
	case 3:
		__asm__ volatile("msr dbgbvr3_el1, %0" : : "r"(address));
		__asm__ volatile("msr dbgbcr3_el1, %0" : : "r"(control));
		break;
	default:
		break;
	}
}

/*
 * Writes one watchpoint's address and control.
 */
static void
write_watchpoint(
	unsigned index,
	uint64_t address,
	uint64_t control)
{
	/* Selects the register pair of the point. */
	switch (index) {
	case 0:
		__asm__ volatile("msr dbgwvr0_el1, %0" : : "r"(address));
		__asm__ volatile("msr dbgwcr0_el1, %0" : : "r"(control));
		break;
	case 1:
		__asm__ volatile("msr dbgwvr1_el1, %0" : : "r"(address));
		__asm__ volatile("msr dbgwcr1_el1, %0" : : "r"(control));
		break;
	case 2:
		__asm__ volatile("msr dbgwvr2_el1, %0" : : "r"(address));
		__asm__ volatile("msr dbgwcr2_el1, %0" : : "r"(control));
		break;
	case 3:
		__asm__ volatile("msr dbgwvr3_el1, %0" : : "r"(address));
		__asm__ volatile("msr dbgwcr3_el1, %0" : : "r"(control));
		break;
	default:
		break;
	}
}

/*
 * Loads a task's points into the breakpoint and watchpoint registers and
 * disables the registers it does not use.
 */
static void
load_debug_points(
	struct arm64_task *task)
{
	const struct hal_debug_point *point;
	uint64_t address;
	uint64_t control;
	unsigned breakpoint;
	unsigned watchpoint;
	unsigned index;
	int error;

	/* Assigns each point the next register of its kind. */
	breakpoint = 0;
	watchpoint = 0;
	for (index = 0; index < task->debug_point_count; index++) {
		point = &task->debug_points[index];

		/* An instruction point takes a breakpoint register. */
		if (point->kind == HAL_DEBUG_KIND_EXECUTE) {
			write_breakpoint(breakpoint, (uint64_t)point->address,
			    ARM64_DBGBCR_EL0_EXECUTE);
			breakpoint++;
			continue;
		}

		/* A data point takes a watchpoint register; it was checked when given. */
		error = watchpoint_control(point, &address, &control);
		if (error != 0)
			continue;
		write_watchpoint(watchpoint, address, control);
		watchpoint++;
	}

	/* Disables the breakpoints the task does not use. */
	for (; breakpoint < debug_breakpoint_count; breakpoint++)
		write_breakpoint(breakpoint, 0, 0);

	/* Disables the watchpoints the task does not use. */
	for (; watchpoint < debug_watchpoint_count; watchpoint++)
		write_watchpoint(watchpoint, 0, 0);

	/* Makes the new points take effect before the task runs. */
	__asm__ volatile("isb" : : : "memory");
	arm64_percpu_current()->debug_points_loaded = 1;
}

/*
 * Disables every breakpoint and watchpoint.
 */
static void
clear_debug_points(void)
{
	unsigned index;

	/* Disables the breakpoints. */
	for (index = 0; index < debug_breakpoint_count; index++)
		write_breakpoint(index, 0, 0);

	/* Disables the watchpoints. */
	for (index = 0; index < debug_watchpoint_count; index++)
		write_watchpoint(index, 0, 0);

	/* Makes the change take effect before the next task runs. */
	__asm__ volatile("isb" : : : "memory");
	arm64_percpu_current()->debug_points_loaded = 0;
}

/*
 * Builds the watchpoint address and control for a data point, or reports
 * that a watchpoint cannot express it.
 */
static int
watchpoint_control(
	const struct hal_debug_point *point,
	uint64_t *address,
	uint64_t *control)
{
	uint64_t access;
	uint64_t bytes;
	uint64_t offset;

	/* Selects the kind of access the point watches for. */
	switch (point->kind) {
	case HAL_DEBUG_KIND_WRITE:
		access = ARM64_DBGWCR_STORE;
		break;
	case HAL_DEBUG_KIND_READ:
		access = ARM64_DBGWCR_LOAD;
		break;
	case HAL_DEBUG_KIND_ACCESS:
		access = ARM64_DBGWCR_LOAD | ARM64_DBGWCR_STORE;
		break;
	default:
		return -1;
	}

	/* A data point covers one, two, four or eight bytes. */
	switch (point->length) {
	case 1U:
	case 2U:
	case 4U:
	case 8U:
		break;
	default:
		return -1;
	}

	/* The address is a multiple of the length, and in the user half. */
	if ((point->address & (uintptr_t)(point->length - 1U)) != 0U)
		return -1;
	if ((uint64_t)point->address >= ARM64_DEBUG_USER_LIMIT)
		return -1;

	/*
	 * The watchpoint names an aligned doubleword and, one bit per byte,
	 * which of its eight bytes are watched.
	 */
	offset = (uint64_t)point->address & 7ULL;
	bytes = ((1ULL << point->length) - 1ULL) << offset;
	*address = (uint64_t)point->address & ~7ULL;
	*control = ARM64_DBGWCR_ENABLE_EL0 | access |
	    (bytes << ARM64_DBGWCR_BAS_SHIFT);

	/* Succeeded. */
	return 0;
}
