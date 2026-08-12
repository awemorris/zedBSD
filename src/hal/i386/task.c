/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * i386 CPU-context implementation.
 */

#include <hal/hal.h>
#include "task.h"
#include "asm.h"

extern uint32 tss_area[26];

static struct task_info *task_list;
static struct task_info *running_task;
static uint32 task_count;
static size_t task_stack_bytes;

static void
tasklist_add(struct task_info *task)
{
	struct task_info **link = &task_list;

	while (*link != NULL)
		link = &(*link)->next;
	*link = task;
	task->next = NULL;
	task_count++;
}

static void
tasklist_del(struct task_info *task)
{
	struct task_info **link = &task_list;

	while (*link != NULL && *link != task)
		link = &(*link)->next;
	if (*link == task) {
		*link = task->next;
		if (task_count != 0)
			task_count--;
	}
	task->next = NULL;
}

void
i386_task_init(void)
{
	struct task_info *task;

	if (running_task != NULL)
		HAL_FATAL("hal_task_init called twice");
	/* Preserve the TSS descriptor installed by locore; initialize its body. */
	hal_memset(tss_area, 0, 104U);
	tss_area[2] = SEG_SYS_DATA;
	task = hal_malloc(sizeof(*task));
	if (task == NULL)
		HAL_FATAL("initial HAL task allocation failed");
	hal_memset(task, 0, sizeof(*task));
	task->space = HAL_SPACE_SYS;
	task->run_cpu = 0;
	tasklist_add(task);
	running_task = task;
}

void
hal_task_init(void)
{
	i386_task_init();
}

static void
set_initial_resume_frame(struct task_info *task, void (*start)(void *),
			 void *arg, void *user_sp)
{
	struct task_resume_frame *frame;

	frame = (struct task_resume_frame *)((uintptr_t)task->sys_stack +
		SYS_STACK_SIZE - sizeof(*frame));
	task->resume_esp = frame;
	hal_memset(frame, 0, sizeof(*frame));
	frame->eflags = asm_get_eflags();
	frame->ret_eip = (uint32)asm_task_entrypoint;
	if (task->space == HAL_SPACE_SYS) {
		frame->gs = frame->fs = frame->es = frame->ds = SEG_SYS_DATA;
		frame->initial.sys.eip = (uint32)start;
		frame->initial.sys.cs = SEG_SYS_CODE;
		frame->initial.sys.eflags =
			EFLAGS_IF | EFLAGS_RSV1 | EFLAGS_IOPL_0;
		frame->initial.sys.return_eip = 0;
		frame->initial.sys.param = (uint32)arg;
	} else {
		frame->gs = frame->fs = frame->es = frame->ds =
			SEG_USER_DATA | SEG_RPL_3;
		frame->initial.user.eip = (uint32)start;
		frame->initial.user.cs = SEG_USER_CODE | SEG_RPL_3;
		frame->initial.user.eflags =
			EFLAGS_IF | EFLAGS_RSV1 | EFLAGS_IOPL_0;
		frame->initial.user.esp = (uint32)user_sp;
		frame->initial.user.ss = SEG_USER_DATA | SEG_RPL_3;
	}
}

hal_task_t
hal_task_create(hal_space_t space, void (*start)(void *), void *arg,
		void *user_stack_pointer)
{
	struct task_info *task;

	if (start == NULL ||
	    ((space == HAL_SPACE_SYS) != (user_stack_pointer == NULL)))
		return NULL;
	task = hal_malloc(sizeof(*task));
	if (task == NULL)
		return NULL;
	hal_memset(task, 0, sizeof(*task));
	task->space = space;
	task->run_cpu = -1;
	task->sys_stack = hal_malloc(SYS_STACK_SIZE);
	if (task->sys_stack == NULL) {
		hal_free(task);
		return NULL;
	}
	task_stack_bytes += SYS_STACK_SIZE;
	set_initial_resume_frame(task, start, arg, user_stack_pointer);
	tasklist_add(task);
	return task;
}

void
hal_task_destroy(hal_task_t handle)
{
	struct task_info *task = handle;

	if (task == NULL)
		return;
	if (task == running_task)
		HAL_FATAL("destroying current HAL task");
	tasklist_del(task);
	if (task->sys_stack != NULL)
		task_stack_bytes -= SYS_STACK_SIZE;
	if (task->sys_stack != NULL)
		hal_free(task->sys_stack);
	hal_free(task);
}

void
hal_task_context_switch(hal_task_t handle)
{
	struct task_info *to = handle;
	struct task_info *from = running_task;

	if (to == NULL || from == NULL)
		HAL_FATAL("invalid HAL task switch");
	if (to == from)
		return;
	running_task = to;
	hal_page_switch_space(to->space);
	if (to->sys_stack != NULL)
		tss_area[1] = (uint32)to->sys_stack + SYS_STACK_SIZE;
	asm_fnsave(from->fpregs);
	if (to->resume_esp->ret_eip != (uint32)asm_task_entrypoint)
		asm_frstor(to->fpregs);
	asm_task_dispatch(&from->resume_esp, &to->resume_esp);
}

void
hal_cpu_idle(void)
{
	asm volatile("sti; hlt; cli" ::: "memory");
}

hal_task_t
hal_task_get_current(void)
{
	return running_task;
}

void hal_task_set_tls(hal_task_t handle, uintptr_t value)
{
	if (handle != NULL)
		((struct task_info *)handle)->tls = value;
}

uintptr_t hal_task_get_tls(hal_task_t handle)
{
	return handle != NULL ? ((struct task_info *)handle)->tls : 0;
}

void hal_task_set_private(hal_task_t handle, void *private_data)
{
	if (handle != NULL)
		((struct task_info *)handle)->private_data = private_data;
}

void *hal_task_get_private(hal_task_t handle)
{
	return handle != NULL ? ((struct task_info *)handle)->private_data : NULL;
}

hal_space_t hal_task_get_space(hal_task_t handle)
{
	return handle != NULL ? ((struct task_info *)handle)->space : HAL_SPACE_SYS;
}

void
hal_i386_task_memory_stats(uint32 *count, size_t *stack_bytes)
{
	if (count != NULL)
		*count = task_count;
	if (stack_bytes != NULL)
		*stack_bytes = task_stack_bytes;
}
