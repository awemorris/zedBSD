/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * i386 CPU-context implementation.
 */

#include <hal/hal.h>
#include "task.h"
#include "asm.h"
#include "int.h"

extern uint32 tss_area[26];

static struct task_info *task_list;
static struct task_info *running_task;
static uint8 initial_fpregs[512] __attribute__((aligned(16)));
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
	/* Establish and retain one canonical x87 image for new/exec tasks.
	 * FNSAVE resets the unit, so restore that same image immediately. */
	asm_fninit();
	asm_fnsave(initial_fpregs);
	asm_frstor(initial_fpregs);
	/* Preserve the TSS descriptor installed by locore; initialize its body. */
	hal_memset(tss_area, 0, 104U);
	tss_area[2] = SEG_SYS_DATA;
	task = hal_malloc(sizeof(*task));
	if (task == NULL)
		HAL_FATAL("initial HAL task allocation failed");
	hal_memset(task, 0, sizeof(*task));
	task->space = HAL_SPACE_SYS;
	task->run_cpu = 0;
	hal_memcpy(task->fpregs, initial_fpregs, sizeof(initial_fpregs));
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
	hal_memcpy(task->fpregs, initial_fpregs, sizeof(initial_fpregs));
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
i386_task_enter_user_frame(void *frame)
{
	if (running_task != NULL)
		running_task->active_user_frame = frame;
}

void
i386_task_leave_user_frame(void)
{
	if (running_task != NULL)
		running_task->active_user_frame = NULL;
}

hal_task_t
hal_task_fork_current(hal_space_t child_space, intptr_t child_result)
{
	struct interrupt_frame *source;
	struct task_info *child;
	struct task_resume_frame *resume;

	if (running_task == NULL || child_space == HAL_SPACE_SYS ||
	    running_task->active_user_frame == NULL)
		return NULL;
	source = running_task->active_user_frame;
	if ((source->cs & 3U) != 3U)
		return NULL;
	/* The running task's registers are live, not necessarily reflected in
	 * its last switch image.  Snapshot and resume them before allocation. */
	asm_fnsave(running_task->fpregs);
	asm_frstor(running_task->fpregs);
	child = hal_task_create(child_space, (void (*)(void *))source->eip,
	    NULL, (void *)(uintptr_t)source->user_esp);
	if (child == NULL)
		return NULL;
	resume = child->resume_esp;
	resume->edi = source->regs.edi;
	resume->esi = source->regs.esi;
	resume->ebp = source->regs.ebp;
	resume->ebx = source->regs.ebx;
	resume->edx = source->regs.edx;
	resume->ecx = source->regs.ecx;
	resume->eax = (uint32)child_result;
	resume->initial.user.eip = source->eip;
	resume->initial.user.cs = source->cs;
	resume->initial.user.eflags = source->eflags;
	resume->initial.user.esp = source->user_esp;
	resume->initial.user.ss = source->user_ss;
	child->tls = running_task->tls;
	hal_memcpy(child->fpregs, running_task->fpregs,
	    sizeof(child->fpregs));
	return child;
}

int
hal_task_exec_current(hal_space_t new_space, uintptr_t entry,
		      uintptr_t user_stack_pointer)
{
	struct interrupt_frame *frame;
	if (running_task == NULL || new_space == HAL_SPACE_SYS ||
	    running_task->active_user_frame == NULL || entry == 0 ||
	    user_stack_pointer == 0 || entry > UINT32_MAX ||
	    user_stack_pointer > UINT32_MAX)
		return -1;
	frame = running_task->active_user_frame;
	hal_memset(&frame->regs, 0, sizeof(frame->regs));
	frame->eip = (uint32)entry;
	frame->cs = SEG_USER_CODE | SEG_RPL_3;
	frame->eflags = EFLAGS_IF | EFLAGS_RSV1 | EFLAGS_IOPL_0;
	frame->user_esp = (uint32)user_stack_pointer;
	frame->user_ss = SEG_USER_DATA | SEG_RPL_3;
	running_task->space = new_space;
	running_task->tls = 0;
	running_task->signal_depth = 0;
	running_task->signal_token = 0;
	hal_memcpy(running_task->fpregs, initial_fpregs,
	    sizeof(running_task->fpregs));
	asm_frstor(running_task->fpregs);
	hal_page_switch_space(new_space);
	return 0;
}

uintptr_t hal_task_user_stack(void)
{
	struct interrupt_frame *f=running_task!=NULL?running_task->active_user_frame:NULL;
	return f!=NULL&&(f->cs&3U)==3U?f->user_esp:0;
}
int hal_task_signal_enter(uintptr_t h,uintptr_t sp,int sig,uintptr_t rest,uint32_t token)
{
	struct interrupt_frame *f=running_task!=NULL?running_task->active_user_frame:NULL;
	(void)sig;(void)rest;
	if(f==NULL||running_task->signal_depth!=0||h==0||sp==0||h>UINT32_MAX||sp>UINT32_MAX||token==0)return -1;
	running_task->signal_frame=*f;running_task->signal_token=token;
	running_task->signal_depth=1;f->eip=(uint32)h;f->user_esp=(uint32)sp;return 0;
}
int hal_task_signal_return(uint32_t token,intptr_t *value)
{
	struct interrupt_frame *f=running_task!=NULL?running_task->active_user_frame:NULL;
	if(f==NULL||value==NULL||running_task->signal_depth!=1||token==0||token!=running_task->signal_token)return -1;
	*f=running_task->signal_frame;*value=(intptr_t)(int32)f->regs.eax;running_task->signal_depth=0;running_task->signal_token=0;return 0;
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
