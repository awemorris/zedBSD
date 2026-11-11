/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The i386 registers of a task, for a debugger.
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
	hal_task_t handle,
	struct hal_gpregs *registers)
{
	struct task_info *task;
	struct interrupt_frame *frame;

	if (handle == NULL || registers == NULL)
		return HAL_ERR_INVALID;
	task = handle;
	frame = task->active_user_frame;
	if (frame == NULL)
		return HAL_ERR_STATE;

	registers->eax = frame->regs.eax;
	registers->ebx = frame->regs.ebx;
	registers->ecx = frame->regs.ecx;
	registers->edx = frame->regs.edx;
	registers->esi = frame->regs.esi;
	registers->edi = frame->regs.edi;
	registers->ebp = frame->regs.ebp;
	registers->esp = frame->user_esp;
	registers->eip = frame->eip;
	registers->eflags = frame->eflags;
	registers->cs = frame->cs;
	registers->ss = frame->user_ss;
	registers->ds = frame->regs.ds;
	registers->es = frame->regs.es;
	registers->fs = 0;
	registers->gs = 0;

	return HAL_OK;
}

/*
 * Writes a task's user integer registers and its next instruction.
 */
int
hal_task_set_user_gpregs(
	hal_task_t handle,
	const struct hal_gpregs *registers)
{
	struct task_info *task;
	struct interrupt_frame *frame;

	if (handle == NULL || registers == NULL)
		return HAL_ERR_INVALID;
	task = handle;
	frame = task->active_user_frame;
	if (frame == NULL)
		return HAL_ERR_STATE;

	if (!user_address_valid(registers->eip))
		return HAL_ERR_INVALID;

	frame->regs.eax = registers->eax;
	frame->regs.ebx = registers->ebx;
	frame->regs.ecx = registers->ecx;
	frame->regs.edx = registers->edx;
	frame->regs.esi = registers->esi;
	frame->regs.edi = registers->edi;
	frame->regs.ebp = registers->ebp;
	frame->user_esp = registers->esp;
	frame->eip = registers->eip;
	frame->eflags = (frame->eflags & ~0x00000cd5U) | (registers->eflags & 0x00000cd5U);
	frame->regs.ds = registers->ds;
	frame->regs.es = registers->es;

	return HAL_OK;
}

/*
 * Reads a task's floating-point registers.
 */
int
hal_task_get_user_fpregs(
	hal_task_t handle,
	struct hal_fpregs *registers)
{
	struct task_info *task;

	if (handle == NULL || registers == NULL)
		return HAL_ERR_INVALID;
	task = handle;

	hal_memcpy(registers, task->fpregs, sizeof(*registers));
	return HAL_OK;
}

/*
 * Writes a task's floating-point registers.
 */
int
hal_task_set_user_fpregs(
	hal_task_t handle,
	const struct hal_fpregs *registers)
{
	struct task_info *task;

	if (handle == NULL || registers == NULL)
		return HAL_ERR_INVALID;
	task = handle;

	hal_memcpy(task->fpregs, registers, sizeof(*registers));
	return HAL_OK;
}

/*
 * Refuses to read vector registers.
 */
int
hal_task_get_user_vregs(
	hal_task_t task,
	struct hal_vregs *registers)
{
	UNUSED_PARAMETER(task);
	UNUSED_PARAMETER(registers);
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
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Sets single-stepping state.
 */
int
hal_task_set_single_step(
	hal_task_t handle,
	int enable)
{
	struct task_info *task;
	struct interrupt_frame *frame;

	if (handle == NULL)
		return HAL_ERR_INVALID;
	task = handle;
	frame = task->active_user_frame;
	if (frame == NULL)
		return HAL_ERR_STATE;

	if (enable)
		frame->eflags |= 0x100U;
	else
		frame->eflags &= ~0x100U;

	return HAL_OK;
}

/*
 * Reports single-stepping state.
 */
int
hal_task_get_single_step(
	hal_task_t handle,
	int *enable)
{
	struct task_info *task;
	struct interrupt_frame *frame;

	if (handle == NULL || enable == NULL)
		return HAL_ERR_INVALID;
	task = handle;
	frame = task->active_user_frame;
	if (frame == NULL)
		return HAL_ERR_STATE;

	*enable = (frame->eflags & 0x100U) != 0;
	return HAL_OK;
}

/*
 * Hardware debug points.
 */
int
hal_task_set_debug_points(
	hal_task_t task,
	const struct hal_debug_point *points,
	unsigned count)
{
	UNUSED_PARAMETER(points);
	if (task == NULL)
		return HAL_ERR_INVALID;
	if (count != 0)
		return HAL_ERR_UNSUPPORTED;
	return HAL_OK;
}

int
hal_task_get_debug_points(
	hal_task_t task,
	struct hal_debug_point *points,
	unsigned capacity,
	unsigned *count)
{
	UNUSED_PARAMETER(points);
	UNUSED_PARAMETER(capacity);
	if (task == NULL || count == NULL)
		return HAL_ERR_INVALID;
	*count = 0;
	return HAL_OK;
}

static int
user_address_valid(
	uint32_t value)
{
	if (value >= 0xC0000000U)
		return 0;
	return 1;
}
