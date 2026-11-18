/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MC68040 registers of a task, for a debugger.
 *
 * A task's user registers are its user frame; its floating-point
 * registers are its saved state, or the processor's while it runs.
 * Neither tracing nor hardware debug points are offered yet.
 */

#include <hal/hal.h>
#include "asm.h"
#include "defs.h"
#include "int.h"
#include "task.h"

static int user_address_valid(uint32_t value);

/*
 * Reads a task's user integer registers and its next instruction.
 */
int
hal_task_get_user_gpregs(
	hal_task_t task,
	struct hal_gpregs *registers)
{
	struct m68040_trap_frame *frame;
	unsigned index;

	/* Requires a task with a user frame and somewhere to put the registers. */
	if (task == NULL || registers == NULL)
		return HAL_ERR_INVALID;
	frame = m68040_task_user_frame(task);
	if (frame == NULL)
		return HAL_ERR_STATE;

	/* x0 always reads zero; the others are the frame's. */
	for (index = 0; index < 8U; index++)
		registers->d[index] = frame->d[index];
	for (index = 0; index < 7U; index++)
		registers->a[index] = frame->a[index];
	registers->a[7] = frame->usp;
	registers->sr = frame->sr;
	registers->pc = frame->pc;

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
	struct m68040_trap_frame *frame;
	unsigned index;
	int valid;

	/* Requires a task with a user frame and registers to write. */
	if (task == NULL || registers == NULL)
		return HAL_ERR_INVALID;
	frame = m68040_task_user_frame(task);
	if (frame == NULL)
		return HAL_ERR_STATE;

	/* Refuses a next instruction outside the user half. */
	valid = user_address_valid(registers->pc);
	if (!valid)
		return HAL_ERR_INVALID;

	/* Writes every register but x0, then the next instruction. */
	for (index = 0; index < 8U; index++)
		frame->d[index] = registers->d[index];
	for (index = 0; index < 7U; index++)
		frame->a[index] = registers->a[index];
	frame->usp = registers->a[7];
	frame->sr = (uint16_t)((frame->sr & 0xff00U) | (registers->sr & 0x001fU));
	frame->pc = registers->pc;

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
	struct m68040_task *target;
	struct m68040_task *running;
	bool enabled;

	/* Requires a task and somewhere to put the registers. */
	if (task == NULL || registers == NULL)
		return HAL_ERR_INVALID;
	target = task;

	/* A running caller's registers are in the processor; they are saved first. */
	enabled = m68040_irq_save();

	running = m68040_task_running();
	if (running == target)
		m68040_fp_save(&target->fpregs);
	*registers = target->fpregs;

	m68040_irq_restore(enabled);

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
	struct m68040_task *target;
	struct m68040_task *running;
	bool enabled;

	/* Requires a task and registers to write. */
	if (task == NULL || registers == NULL)
		return HAL_ERR_INVALID;
	target = task;

	/* The saved state, and the processor's too when the caller is the task. */
	enabled = m68040_irq_save();

	target->fpregs = *registers;
	running = m68040_task_running();
	if (running == target)
		m68040_fp_restore(&target->fpregs);

	m68040_irq_restore(enabled);

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
	uint32_t value)
{
	/* The user half ends below the kernel's addresses. */
	if (value >= M68040_USER_LIMIT)
		return 0;

	/* Succeeded: a user address. */
	return 1;
}
