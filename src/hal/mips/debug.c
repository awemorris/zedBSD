/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS registers of a task, for a debugger.
 *
 * A task's user registers are its user frame; its floating-point
 * registers are its saved state, or the unit's while the task owns it.
 * Status, Cause and BadVAddr are reported but never written: the Status
 * a task returns to user with is always built by the HAL.
 * Neither tracing nor hardware debug points are offered yet.
 */

#include <hal/hal.h>
#include "asm.h"
#include "fpu.h"
#include "spin.h"
#include "status.h"
#include "task.h"
#include "trap.h"

static int user_address_valid(uintptr_t value);

/*
 * Reads a task's user integer registers and its next instruction.
 */
int
hal_task_get_user_gpregs(
	hal_task_t task,
	struct hal_gpregs *registers)
{
	struct mips_trap_frame *frame;
	unsigned index;

	/* Requires a task with a user frame and somewhere to put the registers. */
	if (task == NULL || registers == NULL)
		return HAL_ERR_INVALID;
	frame = mips_task_user_frame(task);
	if (frame == NULL)
		return HAL_ERR_STATE;

	/* The frame's registers; zero, k0 and k1 read as zero, as the frame keeps them. */
	for (index = 0; index < 32U; index++)
		registers->r[index] = frame->r[index];
	registers->hi = frame->hi;
	registers->lo = frame->lo;
	registers->pc = frame->exception_pc;
	registers->status = frame->status;
	registers->cause = frame->cause;
	registers->badvaddr = frame->bad_address;

	/* Succeeded: the registers are copied. */
	return HAL_OK;
}

/*
 * Writes a task's user integer registers and its next instruction.
 */
int
hal_task_set_user_gpregs(
	hal_task_t task,
	const struct hal_gpregs *registers)
{
	struct mips_trap_frame *frame;
	unsigned index;
	int valid;

	/* Requires a task with a user frame and registers to write. */
	if (task == NULL || registers == NULL)
		return HAL_ERR_INVALID;
	frame = mips_task_user_frame(task);
	if (frame == NULL)
		return HAL_ERR_STATE;

	/* Refuses a next instruction outside the user half. */
	valid = user_address_valid(registers->pc);
	if (!valid)
		return HAL_ERR_INVALID;

	/*
	 * Writes every register but zero, k0 and k1, which the return does
	 * not load, then the next instruction.  Status, Cause and BadVAddr
	 * are not taken: the return builds its own Status.
	 */
	for (index = 1; index < 32U; index++) {
		if (index == 26U || index == 27U)
			continue;
		frame->r[index] = registers->r[index];
	}

	/* Writes the multiply and divide results and the restart address. */
	frame->hi = registers->hi;
	frame->lo = registers->lo;
	frame->exception_pc = registers->pc;

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
	struct mips_task *target;
	bool enabled;

	/* Requires a task and somewhere to put the registers. */
	if (task == NULL || registers == NULL)
		return HAL_ERR_INVALID;

	/* The handle is the HAL's task. */
	target = task;

	/* The unit's registers when the task owns it are saved first. */
	enabled = mips_irq_save();

	mips_fpu_sync(target);
	*registers = target->fpregs;

	mips_irq_restore(enabled);

	/* Succeeded: the registers are copied. */
	return HAL_OK;
}

/*
 * Writes a task's floating-point registers.
 */
int
hal_task_set_user_fpregs(
	hal_task_t task,
	const struct hal_fpregs *registers)
{
	struct mips_task *target;
	bool enabled;

	/* Requires a task and registers to write. */
	if (task == NULL || registers == NULL)
		return HAL_ERR_INVALID;

	/* The handle is the HAL's task. */
	target = task;

	/*
	 * The saved state, with a control register that is safe to load,
	 * and the unit's claim dropped, so that the task's next
	 * floating-point instruction loads the new state.
	 */
	enabled = mips_irq_save();

	target->fpregs = *registers;
	target->fpregs.fcsr = mips_float_control_sanitize(registers->fcsr);
	mips_fpu_forget(target);

	mips_irq_restore(enabled);

	/* Succeeded: the task runs with the registers. */
	return HAL_OK;
}

/*
 * Refuses to read vector registers: the processors this port runs on have
 * no vector unit in use.
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
	uintptr_t value)
{
	/* The user segment ends below the kernel's addresses. */
	if (value >= MIPS_USER_LIMIT)
		return 0;

	/* Succeeded: a user address. */
	return 1;
}
