/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 registers of a task, for a debugger.
 *
 * A task's user registers are its user frame and its thread pointer; its
 * floating-point registers are the copy the task keeps, brought up to date
 * with the unit before a read and dropped by the unit after a write
 * (fpu.c).  ARMv7 has no single-step facility and the HAL gives no
 * hardware debug points to a task yet, so neither is offered.
 */

#include <hal/hal.h>
#include "asm.h"
#include "defs.h"
#include "fpu.h"
#include "int.h"
#include "task.h"

/*
 * The status bits a debugger may change: the condition flags and Q, the
 * GE bits and T.  The mode, the masks and the endianness stay the
 * kernel's, so a task always returns to user mode with interrupts on.
 */
#define DEBUG_CPSR_USER_BITS	0xf80f0020U

static int user_address_valid(uint32_t value);

/*
 * Reads a task's user integer registers, its restart address, status and
 * thread pointer.
 */
int
hal_task_get_user_gpregs(
	hal_task_t task,
	struct hal_gpregs *registers)
{
	struct armv7_trap_frame *frame;
	unsigned index;

	/* Requires a task with a user frame and somewhere to put the registers. */
	if (task == NULL || registers == NULL)
		return HAL_ERR_INVALID;
	frame = armv7_task_user_frame(task);
	if (frame == NULL)
		return HAL_ERR_STATE;

	/* r0 to r12, then the banked user registers and the return state. */
	for (index = 0; index < 13U; index++)
		registers->r[index] = frame->r[index];
	registers->sp = frame->sp_usr;
	registers->lr = frame->lr_usr;
	registers->pc = frame->pc;
	registers->cpsr = frame->cpsr;
	registers->tpidruro = (uint32_t)hal_task_get_tls(task);

	/* Succeeded: the registers are copied. */
	return HAL_OK;
}

/*
 * Writes a task's user integer registers, its restart address, the status
 * bits a debugger may change and its thread pointer.
 */
int
hal_task_set_user_gpregs(
	hal_task_t task,
	const struct hal_gpregs *registers)
{
	struct armv7_trap_frame *frame;
	unsigned index;
	int valid;

	/* Requires a task with a user frame and registers to write. */
	if (task == NULL || registers == NULL)
		return HAL_ERR_INVALID;
	frame = armv7_task_user_frame(task);
	if (frame == NULL)
		return HAL_ERR_STATE;

	/* Refuses a restart address outside the user half. */
	valid = user_address_valid(registers->pc);
	if (!valid)
		return HAL_ERR_INVALID;

	/* Writes every register, the restart address and the user's status bits. */
	for (index = 0; index < 13U; index++)
		frame->r[index] = registers->r[index];
	frame->sp_usr = registers->sp;
	frame->lr_usr = registers->lr;
	frame->pc = registers->pc;
	frame->cpsr = ARMV7_PSR_MODE_USR | (registers->cpsr & DEBUG_CPSR_USER_BITS);
	hal_task_set_tls(task, registers->tpidruro);

	/* Succeeded: the task returns to user with the registers. */
	return HAL_OK;
}

/*
 * Reads a task's floating-point registers.
 */
int
hal_task_get_user_fpregs(
	hal_task_t task,
	struct hal_fpregs *registers)
{
	struct armv7_task *target;
	bool enabled;

	/* Requires a task and somewhere to put the registers. */
	if (task == NULL || registers == NULL)
		return HAL_ERR_INVALID;
	target = task;

	/* Copies the task's registers, as the unit has them, as one step against a switch. */
	enabled = armv7_irq_save();

	armv7_fpu_sync(&target->fpu);
	*registers = target->fpu.regs;

	armv7_irq_restore(enabled);

	/* Succeeded: the registers are copied. */
	return HAL_OK;
}

/*
 * Writes a task's floating-point registers.  The exception register is
 * read only, so the task keeps its own, and the status and control
 * register keeps only the bits the unit holds.
 */
int
hal_task_set_user_fpregs(
	hal_task_t task,
	const struct hal_fpregs *registers)
{
	struct armv7_task *target;
	uint32_t exception;
	bool enabled;

	/* Requires a task and registers to write. */
	if (task == NULL || registers == NULL)
		return HAL_ERR_INVALID;
	target = task;

	/* Replaces the task's registers as one step against a switch. */
	enabled = armv7_irq_save();

	exception = target->fpu.regs.fpexc;
	target->fpu.regs = *registers;
	target->fpu.regs.fpexc = exception;
	target->fpu.regs.fpscr = armv7_fpu_sanitize_fpscr(registers->fpscr);
	armv7_fpu_forget(&target->fpu);

	armv7_irq_restore(enabled);

	/* Succeeded: the task has the registers. */
	return HAL_OK;
}

/*
 * Refuses to read vector registers: the Advanced SIMD registers are the
 * floating-point ones.
 */
int
hal_task_get_user_vregs(
	hal_task_t task,
	struct hal_vregs *registers)
{
	UNUSED_PARAMETER(task);
	UNUSED_PARAMETER(registers);

	/* Failed: there is no such set. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Refuses to write vector registers.
 */
int
hal_task_set_user_vregs(
	hal_task_t task,
	const struct hal_vregs *registers)
{
	UNUSED_PARAMETER(task);
	UNUSED_PARAMETER(registers);

	/* Failed: there is no such set. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Accepts turning single-stepping off, and refuses turning it on.
 */
int
hal_task_set_single_step(
	hal_task_t task,
	int enable)
{
	/* Requires a task. */
	if (task == NULL)
		return HAL_ERR_INVALID;

	/* Stepping cannot be arranged on this port. */
	if (enable)
		return HAL_ERR_UNSUPPORTED;

	/* Succeeded: the task does not step, as it never does. */
	return HAL_OK;
}

/*
 * Reports that a task does not single-step.
 */
int
hal_task_get_single_step(
	hal_task_t task,
	int *enable)
{
	/* Requires a task and somewhere to report. */
	if (task == NULL || enable == NULL)
		return HAL_ERR_INVALID;

	/* Succeeded: stepping is always off. */
	*enable = 0;
	return HAL_OK;
}

/*
 * Accepts an empty set of hardware debug points, and refuses any point.
 */
int
hal_task_set_debug_points(
	hal_task_t task,
	const struct hal_debug_point *points,
	unsigned count)
{
	UNUSED_PARAMETER(points);

	/* Requires a task. */
	if (task == NULL)
		return HAL_ERR_INVALID;

	/* No point can be given to a task here. */
	if (count != 0)
		return HAL_ERR_UNSUPPORTED;

	/* Succeeded: the task runs with no points, as it always does. */
	return HAL_OK;
}

/*
 * Reports that a task has no hardware debug points.
 */
int
hal_task_get_debug_points(
	hal_task_t task,
	struct hal_debug_point *points,
	unsigned capacity,
	unsigned *count)
{
	UNUSED_PARAMETER(points);
	UNUSED_PARAMETER(capacity);

	/* Requires a task and somewhere to report. */
	if (task == NULL || count == NULL)
		return HAL_ERR_INVALID;

	/* Succeeded: no points. */
	*count = 0;
	return HAL_OK;
}

/* Reports whether a value is an address in the user half. */
static int
user_address_valid(
	uint32_t value)
{
	/* The user half ends below the kernel's addresses. */
	if (value >= ARMV7_USER_LIMIT)
		return 0;

	/* Succeeded: a user address. */
	return 1;
}
