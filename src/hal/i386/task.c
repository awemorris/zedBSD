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
#include "percpu.h"

extern uint32 tss_area[26];

static struct task_info *task_list;
static struct task_info *running_tasks[HAL_CPU_MAX];
#define running_task running_tasks[hal_cpu_current()]
static uint32 task_count;
static size_t task_stack_bytes;
static uint8 initial_fpregs[512] __attribute__((aligned(16)));
static volatile unsigned task_registry_lock;
#define I386_SYSCALL_INSTRUCTION_SIZE 2U

static void
tasklist_add(struct task_info *task)
{
	bool enabled = hal_irq_disable();
	struct task_info **link = &task_list;

	while (__atomic_exchange_n(&task_registry_lock, 1U,
	    __ATOMIC_ACQUIRE) != 0)
		__asm__ volatile("pause");
	while (*link != NULL)
		link = &(*link)->next;
	*link = task;
	task->next = NULL;
	task_count++;
	if (task->owns_stack)
		task_stack_bytes += SYS_STACK_SIZE;
	__atomic_store_n(&task_registry_lock, 0U, __ATOMIC_RELEASE);
	if (enabled)
		hal_irq_enable();
}

static void
tasklist_del(struct task_info *task)
{
	bool enabled = hal_irq_disable();
	struct task_info **link = &task_list;

	while (__atomic_exchange_n(&task_registry_lock, 1U,
	    __ATOMIC_ACQUIRE) != 0)
		__asm__ volatile("pause");
	while (*link != NULL && *link != task)
		link = &(*link)->next;
	if (*link == task) {
		*link = task->next;
		if (task_count != 0)
			task_count--;
		if (task->owns_stack && task_stack_bytes >= SYS_STACK_SIZE)
			task_stack_bytes -= SYS_STACK_SIZE;
	}
	task->next = NULL;
	__atomic_store_n(&task_registry_lock, 0U, __ATOMIC_RELEASE);
	if (enabled)
		hal_irq_enable();
}

void
i386_task_init(void)
{
	struct task_info *task;

	if (running_task != NULL)
		HAL_FATAL("hal_task_init called twice");
	i386_percpu_init(0, (uintptr_t)asm_get_esp());
	task = hal_malloc(sizeof(*task));
	if (task == NULL)
		HAL_FATAL("initial HAL task allocation failed");
	hal_memset(task, 0, sizeof(*task));
	task->space = HAL_SPACE_SYS;
	task->run_cpu = 0;
	task->target_cpu = 0;
	asm_fnsave(initial_fpregs);
	asm_frstor(initial_fpregs);
	hal_memcpy(task->fpregs, initial_fpregs, sizeof(initial_fpregs));
	tasklist_add(task);
	running_task = task;
}

void
i386_task_init_secondary(hal_cpu_id_t cpu, uintptr_t stack)
{
	struct task_info *task;
	if (cpu == 0 || cpu >= hal_cpu_count() || running_task != NULL)
		HAL_FATAL("invalid secondary HAL task initialization");
	task = hal_malloc(sizeof(*task));
	if (task == NULL)
		HAL_FATAL("secondary HAL task allocation failed");
	hal_memset(task, 0, sizeof(*task));
	task->space = HAL_SPACE_SYS;
	task->run_cpu = (int)cpu;
	task->target_cpu = cpu;
	task->sys_stack = (void *)(stack - SYS_STACK_SIZE);
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
	task->target_cpu = hal_cpu_current();
	hal_memcpy(task->fpregs, initial_fpregs, sizeof(initial_fpregs));
	task->sys_stack = hal_malloc(SYS_STACK_SIZE);
	if (task->sys_stack == NULL) {
		hal_free(task);
		return NULL;
	}
	task->owns_stack = 1;
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
	asm_fnsave(running_task->fpregs);
	asm_frstor(running_task->fpregs);
	hal_memcpy(child->fpregs, running_task->fpregs,
	    sizeof(child->fpregs));
	return child;
}

int
hal_task_exec_validate(hal_space_t new_space, uintptr_t entry,
	uintptr_t user_stack_pointer)
{
	return running_task != NULL && new_space != HAL_SPACE_SYS &&
	    running_task->active_user_frame != NULL && entry != 0 &&
	    user_stack_pointer != 0 && entry <= UINT32_MAX &&
	    user_stack_pointer <= UINT32_MAX ? 0 : -1;
}

int
hal_task_exec_current(hal_space_t new_space, uintptr_t entry,
		      uintptr_t user_stack_pointer)
{
	struct interrupt_frame *frame;
	if (hal_task_exec_validate(new_space, entry, user_stack_pointer) != 0)
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
	hal_memcpy(running_task->fpregs, initial_fpregs,
	    sizeof(initial_fpregs));
	asm_frstor(running_task->fpregs);
	hal_page_switch_space(new_space);
	return 0;
}

uintptr_t hal_task_user_stack(void)
{
	struct interrupt_frame *f=running_task!=NULL?running_task->active_user_frame:NULL;
	return f!=NULL&&(f->cs&3U)==3U?f->user_esp:0;
}
int hal_task_user_context(struct hal_user_context *context)
{
	struct interrupt_frame *f=running_task!=NULL?running_task->active_user_frame:NULL;
	if(f==NULL||context==NULL||(f->cs&3U)!=3U)return -1;
	context->pc=f->eip;context->stack_pointer=f->user_esp;
	context->return_value=(intptr_t)(int32)f->regs.eax;return 0;
}
int hal_task_signal_enter(uintptr_t h,uintptr_t sp,int sig,uintptr_t info,
	uintptr_t context,uintptr_t rest,uint32_t token)
{
	struct interrupt_frame *f=running_task!=NULL?running_task->active_user_frame:NULL;
	unsigned depth;
	(void)sig;(void)info;(void)context;(void)rest;
	if(f==NULL||running_task->signal_depth>=HAL_SIGNAL_NEST_MAX||h==0||sp==0||h>UINT32_MAX||sp>UINT32_MAX||token==0)return -1;
	depth=running_task->signal_depth;running_task->signal_frame[depth]=*f;
	asm_fnsave(running_task->signal_fpregs[depth]);
	asm_frstor(running_task->signal_fpregs[depth]);
	running_task->signal_token[depth]=token;running_task->signal_depth=depth+1U;
	f->eip=(uint32)h;f->user_esp=(uint32)sp;return 0;
}
int hal_task_signal_return(uint32_t token,intptr_t *value)
{
	struct interrupt_frame *f=running_task!=NULL?running_task->active_user_frame:NULL;
	unsigned depth;
	if(f==NULL||value==NULL||running_task->signal_depth==0||token==0)return -1;
	depth=running_task->signal_depth-1U;if(token!=running_task->signal_token[depth])return -1;
	*f=running_task->signal_frame[depth];*value=(intptr_t)(int32)f->regs.eax;
	asm_frstor(running_task->signal_fpregs[depth]);
	running_task->signal_token[depth]=0;running_task->signal_depth=depth;return 0;
}
int hal_task_signal_restart(uint32_t token,uint32_t number,const uintptr_t args[HAL_SYSCALL_ARGS],intptr_t *value)
{
	struct interrupt_frame *f=running_task!=NULL?running_task->active_user_frame:NULL;
	unsigned depth;
	if(f==NULL||args==NULL||value==NULL||running_task->signal_depth==0||token==0)return -1;
	depth=running_task->signal_depth-1U;
	if(token!=running_task->signal_token[depth]||
	    running_task->signal_frame[depth].eip<
	    I386_SYSCALL_INSTRUCTION_SIZE)return -1;
	*f=running_task->signal_frame[depth];
	f->eip-=I386_SYSCALL_INSTRUCTION_SIZE;
	f->regs.eax=number;f->regs.ebx=(uint32)args[0];f->regs.ecx=(uint32)args[1];f->regs.edx=(uint32)args[2];f->regs.esi=(uint32)args[3];f->regs.edi=(uint32)args[4];f->regs.ebp=(uint32)args[5];
	*value=(intptr_t)(int32)number;
	asm_frstor(running_task->signal_fpregs[depth]);
	running_task->signal_token[depth]=0;running_task->signal_depth=depth;
	return 0;
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
	if (task->owns_stack)
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
	if (to->run_cpu >= 0)
		HAL_FATAL("i386 HAL task already running");
	if (to->target_cpu != hal_cpu_current())
		HAL_FATAL("i386 HAL task resumed on wrong CPU");
	from->run_cpu = -1;
	to->run_cpu = (int)hal_cpu_current();
	running_task = to;
	hal_page_switch_space(to->space);
	if (to->sys_stack != NULL)
		i386_percpu_set_kernel_stack(hal_cpu_current(),
		    (uint32)to->sys_stack + SYS_STACK_SIZE);
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

int
hal_task_transfer(hal_task_t handle, hal_cpu_id_t target_cpu)
{
	struct task_info *task = handle;
	if (task == NULL || target_cpu >= hal_cpu_count())
		return HAL_ERR_INVALID;
	if (task->run_cpu >= 0)
		return HAL_ERR_BUSY;
	task->target_cpu = target_cpu;
	return HAL_OK;
}

void
hal_i386_task_memory_stats(uint32 *count, size_t *stack_bytes)
{
	bool enabled = hal_irq_disable();
	while (__atomic_exchange_n(&task_registry_lock, 1U,
	    __ATOMIC_ACQUIRE) != 0)
		__asm__ volatile("pause");
	if (count != NULL)
		*count = task_count;
	if (stack_bytes != NULL)
		*stack_bytes = task_stack_bytes;
	__atomic_store_n(&task_registry_lock, 0U, __ATOMIC_RELEASE);
	if (enabled)
		hal_irq_enable();
}
