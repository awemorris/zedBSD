/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The i386 task-context and signal-frame implementation.
 */

#include <hal/hal.h>

#include "asm.h"
#include "defs.h"
#include "int.h"
#include "percpu.h"
#include "task.h"

#define running_task running_tasks[hal_cpu_current()]

extern uint32_t tss_area[26];

static struct task_info *task_list;
static struct task_info *running_tasks[HAL_CPU_MAX];
static uint32_t task_count;
static size_t task_stack_bytes;
static uint8_t initial_fpregs[512] __attribute__((aligned(16)));
static volatile unsigned task_registry_lock;

static void tasklist_add(struct task_info *task);
static void tasklist_del(struct task_info *task);
static void set_initial_resume_frame(struct task_info *task, void (*start)(void *), void *arg, void *user_sp);

/*
 * Initializes task context for the bootstrap CPU.
 */
void
i386_task_init(
	void)
{
	struct task_info *task;
	uintptr_t stack;

	/* Rejects repeated bootstrap task initialization. */
	if (running_task != NULL)
		HAL_FATAL("hal_task_init called twice");

	/* Initializes the bootstrap CPU's descriptor state from its live stack. */
	stack = (uintptr_t)asm_get_esp();
	i386_percpu_init(0, stack);

	/* Allocates and initializes the bootstrap task record. */
	task = hal_malloc(sizeof(*task));

	/* Rejects bootstrap task allocation failure. */
	if (task == NULL)
		HAL_FATAL("initial HAL task allocation failed");
	hal_memset(task, 0, sizeof(*task));
	task->space = HAL_SPACE_SYS;
	task->run_cpu = 0;
	task->target_cpu = 0;

	/* Captures the processor's initialized floating-point state. */
	__asm__ volatile("fninit" : : : "memory");
	asm_fnsave(initial_fpregs);
	hal_memcpy(task->fpregs, initial_fpregs, sizeof(initial_fpregs));
	asm_frstor(task->fpregs);

	/* Publishes the bootstrap task as the current task. */
	tasklist_add(task);
	running_task = task;
}

/*
 * Initializes task context for one secondary CPU.
 */
void
i386_task_init_secondary(
	hal_cpu_id_t cpu,
	uintptr_t stack)
{
	struct task_info *task;

	/* Rejects the bootstrap logical CPU. */
	if (cpu == 0)
		HAL_FATAL("invalid secondary HAL task initialization");

	/* Rejects a CPU outside the configured inventory. */
	if (cpu >= hal_cpu_count())
		HAL_FATAL("invalid secondary HAL task initialization");

	/* Rejects repeated initialization on this CPU. */
	if (running_task != NULL)
		HAL_FATAL("invalid secondary HAL task initialization");

	/* Allocates and initializes the secondary CPU's bootstrap task. */
	task = hal_malloc(sizeof(*task));

	/* Rejects secondary bootstrap task allocation failure. */
	if (task == NULL)
		HAL_FATAL("secondary HAL task allocation failed");
	hal_memset(task, 0, sizeof(*task));
	task->space = HAL_SPACE_SYS;
	task->run_cpu = (int)cpu;
	task->target_cpu = cpu;
	task->sys_stack = (void *)(stack - SYS_STACK_SIZE);

	/* Restores the common initialized floating-point state. */
	hal_memcpy(task->fpregs, initial_fpregs, sizeof(initial_fpregs));
	asm_frstor(task->fpregs);

	/* Publishes the secondary bootstrap task as the current task. */
	tasklist_add(task);
	running_task = task;
}

/*
 * Initializes the public HAL task subsystem.
 */
void
hal_task_init(
	void)
{
	/* Initializes task state for the bootstrap CPU. */
	i386_task_init();
}

/*
 * Creates one kernel or user task context.
 */
hal_task_t
hal_task_create(
	hal_space_t space,
	void (*start)(void *),
	void *arg,
	void *user_stack_pointer)
{
	struct task_info *task;

	/* Requires an entry point and a stack form matching the address space. */
	if (start == NULL ||
	    ((space == HAL_SPACE_SYS) != (user_stack_pointer == NULL))) {
		return NULL;
	}

	/* Allocates and initializes the task record. */
	task = hal_malloc(sizeof(*task));

	/* Reports task-record allocation failure. */
	if (task == NULL)
		return NULL;
	hal_memset(task, 0, sizeof(*task));
	task->space = space;
	task->run_cpu = -1;
	task->target_cpu = hal_cpu_current();
	hal_memcpy(task->fpregs, initial_fpregs, sizeof(initial_fpregs));

	/* Allocates the task's owned kernel stack. */
	task->sys_stack = hal_malloc(SYS_STACK_SIZE);

	/* Releases the task record when stack allocation fails. */
	if (task->sys_stack == NULL) {
		hal_free(task);
		return NULL;
	}

	/* Builds the initial context and publishes the task registry entry. */
	task->owns_stack = 1;
	set_initial_resume_frame(task, start, arg, user_stack_pointer);
	tasklist_add(task);

	/* Returns the completed task handle. */
	return task;
}

/*
 * Publishes the active user interrupt frame for the current task.
 */
void
i386_task_enter_user_frame(
	void *frame)
{
	/* Records the frame only while a current task exists. */
	if (running_task != NULL)
		running_task->active_user_frame = frame;
}

/*
 * Clears the active user interrupt frame for the current task.
 */
void
i386_task_leave_user_frame(
	void)
{
	/* Clears the frame only while a current task exists. */
	if (running_task != NULL)
		running_task->active_user_frame = NULL;
}

/*
 * Forks the current user task into a child address space.
 */
hal_task_t
hal_task_fork_current(
	hal_space_t child_space,
	intptr_t child_result)
{
	struct interrupt_frame *source;
	struct task_info *child;
	struct task_resume_frame *resume;

	/* Requires an active user frame and a distinct user address space. */
	if (running_task == NULL || child_space == HAL_SPACE_SYS ||
	    running_task->active_user_frame == NULL) {
		return NULL;
	}
	source = running_task->active_user_frame;

	/* Rejects a frame which is not returning to user privilege. */
	if ((source->cs & 3U) != 3U)
		return NULL;

	/* Creates a child context at the parent's current user PC and stack. */
	child = hal_task_create(
		child_space,
		(void (*)(void *))source->eip,
		NULL,
		(void *)(uintptr_t)source->user_esp);

	/* Reports child allocation or initialization failure. */
	if (child == NULL)
		return NULL;

	/* Copies the parent's saved general and user return registers. */
	resume = child->resume_esp;
	resume->edi = source->regs.edi;
	resume->esi = source->regs.esi;
	resume->ebp = source->regs.ebp;
	resume->ebx = source->regs.ebx;
	resume->edx = source->regs.edx;
	resume->ecx = source->regs.ecx;
	resume->eax = (uint32_t)child_result;
	resume->initial.user.eip = source->eip;
	resume->initial.user.cs = source->cs;
	resume->initial.user.eflags = source->eflags;
	resume->initial.user.esp = source->user_esp;
	resume->initial.user.ss = source->user_ss;
	child->tls = running_task->tls;

	/* Snapshots the parent's live floating-point state into the child. */
	asm_fnsave(running_task->fpregs);
	asm_frstor(running_task->fpregs);
	hal_memcpy(child->fpregs, running_task->fpregs, sizeof(child->fpregs));

	/* Returns the completed child task. */
	return child;
}

/*
 * Validates an executable replacement for the current user task.
 */
int
hal_task_exec_validate(
	hal_space_t new_space,
	uintptr_t entry,
	uintptr_t user_stack_pointer)
{
	/* Requires a current task. */
	if (running_task == NULL)
		return -1;

	/* Requires a user address space and an active user frame. */
	if (new_space == HAL_SPACE_SYS)
		return -1;

	/* Requires a live user interrupt frame. */
	if (running_task->active_user_frame == NULL)
		return -1;

	/* Requires nonzero 32-bit entry and stack addresses. */
	if (entry == 0 || user_stack_pointer == 0)
		return -1;

	/* Rejects entry or stack addresses wider than the i386 ABI. */
	if (entry > UINT32_MAX || user_stack_pointer > UINT32_MAX)
		return -1;

	/* Reports a valid executable context. */
	return 0;
}

/*
 * Replaces the current user task's executable context.
 */
int
hal_task_exec_current(
	hal_space_t new_space,
	uintptr_t entry,
	uintptr_t user_stack_pointer)
{
	struct interrupt_frame *frame;
	int result;

	/* Validates the replacement before changing task state. */
	result = hal_task_exec_validate(new_space, entry, user_stack_pointer);

	/* Rejects a replacement which failed validation. */
	if (result != 0)
		return -1;

	/* Rebuilds the active frame at the new user entry and stack. */
	frame = running_task->active_user_frame;
	hal_memset(&frame->regs, 0, sizeof(frame->regs));
	frame->eip = (uint32_t)entry;
	frame->cs = SEG_USER_CODE | SEG_RPL_3;
	frame->eflags = EFLAGS_IF | EFLAGS_RSV1 | EFLAGS_IOPL_0;
	frame->user_esp = (uint32_t)user_stack_pointer;
	frame->user_ss = SEG_USER_DATA | SEG_RPL_3;

	/* Publishes the new space and clears inherited process-local state. */
	running_task->space = new_space;
	running_task->tls = 0;
	running_task->signal_depth = 0;

	/* Restores clean floating-point state and switches address spaces. */
	hal_memcpy(
		running_task->fpregs,
		initial_fpregs,
		sizeof(initial_fpregs));
	asm_frstor(running_task->fpregs);
	hal_page_switch_space(new_space);

	/* Reports a completed executable replacement. */
	return 0;
}

/*
 * Reports the current task's user stack pointer.
 */
uintptr_t
hal_task_user_stack(
	void)
{
	struct interrupt_frame *frame;

	/* Resolves the active frame only while a current task exists. */
	if (running_task != NULL) {
		frame = running_task->active_user_frame;
	} else {
		frame = NULL;
	}

	/* Rejects a missing frame or one outside user privilege. */
	if (frame == NULL || (frame->cs & 3U) != 3U)
		return 0;

	/* Returns the saved user stack pointer. */
	return frame->user_esp;
}

/*
 * Reports the current task's user-visible execution context.
 */
int
hal_task_user_context(
	struct hal_user_context *context)
{
	struct interrupt_frame *frame;

	/* Resolves the active frame only while a current task exists. */
	if (running_task != NULL) {
		frame = running_task->active_user_frame;
	} else {
		frame = NULL;
	}

	/* Requires a user frame and writable context storage. */
	if (frame == NULL || context == NULL || (frame->cs & 3U) != 3U)
		return -1;

	/* Copies the user PC, stack, and signed syscall return value. */
	context->pc = frame->eip;
	context->stack_pointer = frame->user_esp;
	context->return_value = (intptr_t)(int32_t)frame->regs.eax;

	/* Reports an available user context. */
	return 0;
}

/*
 * Enters one nested signal context for the current user task.
 */
int
hal_task_signal_enter(
	uintptr_t handler,
	uintptr_t stack_pointer,
	int signal,
	uintptr_t info,
	uintptr_t context,
	uintptr_t restorer,
	uint32_t token)
{
	struct interrupt_frame *frame;
	unsigned depth;

	UNUSED_PARAMETER(signal);
	UNUSED_PARAMETER(info);
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(restorer);

	/* Resolves the active frame only while a current task exists. */
	if (running_task != NULL) {
		frame = running_task->active_user_frame;
	} else {
		frame = NULL;
	}

	/* Requires an active frame and available signal nesting slot. */
	if (frame == NULL)
		return -1;

	/* Rejects a signal beyond the fixed nesting capacity. */
	if (running_task->signal_depth >= HAL_SIGNAL_NEST_MAX)
		return -1;

	/* Requires nonzero handler and stack values. */
	if (handler == 0 || stack_pointer == 0)
		return -1;

	/* Rejects handler or stack addresses wider than the i386 ABI. */
	if (handler > UINT32_MAX || stack_pointer > UINT32_MAX)
		return -1;

	/* Requires a nonzero token after validating the signal addresses. */
	if (token == 0)
		return -1;

	/* Saves the interrupted integer and floating-point signal state. */
	depth = running_task->signal_depth;
	running_task->signal_frame[depth] = *frame;
	asm_fnsave(running_task->signal_fpregs[depth]);
	asm_frstor(running_task->signal_fpregs[depth]);

	/* Publishes the token and new nested signal depth. */
	running_task->signal_token[depth] = token;
	running_task->signal_depth = depth + 1U;

	/* Redirects the active frame to the signal handler and stack. */
	frame->eip = (uint32_t)handler;
	frame->user_esp = (uint32_t)stack_pointer;

	/* Reports a saved and redirected signal context. */
	return 0;
}

/*
 * Restores one nested signal context for the current user task.
 */
int
hal_task_signal_return(
	uint32_t token,
	intptr_t *value)
{
	struct interrupt_frame *frame;
	unsigned depth;

	/* Resolves the active frame only while a current task exists. */
	if (running_task != NULL) {
		frame = running_task->active_user_frame;
	} else {
		frame = NULL;
	}

	/* Requires a frame, result storage, nesting state, and nonzero token. */
	if (frame == NULL || value == NULL ||
	    running_task->signal_depth == 0 || token == 0) {
		return -1;
	}

	/* Requires the token belonging to the innermost signal frame. */
	depth = running_task->signal_depth - 1U;

	/* Rejects a token which does not own the innermost frame. */
	if (token != running_task->signal_token[depth])
		return -1;

	/* Restores the saved user frame and reports its return register. */
	*frame = running_task->signal_frame[depth];
	*value = (intptr_t)(int32_t)frame->regs.eax;
	asm_frstor(running_task->signal_fpregs[depth]);

	/* Clears the consumed token and publishes the reduced depth. */
	running_task->signal_token[depth] = 0;
	running_task->signal_depth = depth;

	/* Reports a restored signal context. */
	return 0;
}

/*
 * Destroys one inactive HAL task.
 */
void
hal_task_destroy(
	hal_task_t handle)
{
	struct task_info *task;

	/* Ignores an empty task handle. */
	task = handle;

	/* Returns without work for an empty task handle. */
	if (task == NULL)
		return;

	/* Rejects destruction of the current CPU's running task. */
	if (task == running_task)
		HAL_FATAL("destroying current HAL task");

	/* Removes the task before releasing its owned storage. */
	tasklist_del(task);

	/* Releases the kernel stack only when this task owns it. */
	if (task->owns_stack)
		hal_free(task->sys_stack);

	/* Releases the unregistered task record itself. */
	hal_free(task);
}

/*
 * Switches the current CPU to another HAL task context.
 */
void
hal_task_context_switch(
	hal_task_t handle)
{
	struct task_info *to;
	struct task_info *from;

	/* Resolves both sides of the requested task switch. */
	to = handle;
	from = running_task;

	/* Requires two valid and distinct task records. */
	if (to == NULL || from == NULL)
		HAL_FATAL("invalid HAL task switch");

	/* Avoids switching a task to itself. */
	if (to == from)
		return;

	/* Requires an idle destination assigned to the current CPU. */
	if (to->run_cpu >= 0)
		HAL_FATAL("i386 HAL task already running");

	/* Rejects a destination assigned to another CPU. */
	if (to->target_cpu != hal_cpu_current())
		HAL_FATAL("i386 HAL task resumed on wrong CPU");

	/* Publishes task residency before switching address spaces. */
	from->run_cpu = -1;
	to->run_cpu = (int)hal_cpu_current();
	running_task = to;
	hal_page_switch_space(to->space);

	/* Updates privilege-transition state for an allocated task stack. */
	if (to->sys_stack != NULL) {
		i386_percpu_set_kernel_stack(
			hal_cpu_current(),
			(uint32_t)to->sys_stack + SYS_STACK_SIZE);
	}

	/* Saves floating-point state and dispatches the assembly context switch. */
	asm_fnsave(from->fpregs);
	asm_frstor(to->fpregs);
	asm_task_dispatch(&from->resume_esp, &to->resume_esp);
}

/*
 * Idles the current CPU until one interrupt arrives.
 */
void
hal_cpu_idle(
	void)
{
	/* Opens the interrupt window for HLT and closes it after wakeup. */
	__asm__ volatile("sti; hlt; cli" : : : "memory");
}

/*
 * Returns the current CPU's running HAL task.
 */
hal_task_t
hal_task_get_current(
	void)
{
	/* Returns the CPU-local running task. */
	return running_task;
}

/*
 * Updates one task's thread-local-storage pointer.
 */
void
hal_task_set_tls(
	hal_task_t handle,
	uintptr_t value)
{
	/* Updates only a valid task handle. */
	if (handle != NULL)
		((struct task_info *)handle)->tls = value;
}

/*
 * Returns one task's thread-local-storage pointer.
 */
uintptr_t
hal_task_get_tls(
	hal_task_t handle)
{
	/* Reports an empty TLS value for an invalid handle. */
	if (handle == NULL)
		return 0;

	/* Returns the task's stored TLS value. */
	return ((struct task_info *)handle)->tls;
}

/*
 * Updates one task's kernel-private pointer.
 */
void
hal_task_set_private(
	hal_task_t handle,
	void *private_data)
{
	/* Updates only a valid task handle. */
	if (handle != NULL)
		((struct task_info *)handle)->private_data = private_data;
}

/*
 * Returns one task's kernel-private pointer.
 */
void *
hal_task_get_private(
	hal_task_t handle)
{
	/* Reports no private data for an invalid handle. */
	if (handle == NULL)
		return NULL;

	/* Returns the task's stored private pointer. */
	return ((struct task_info *)handle)->private_data;
}

/*
 * Returns one task's address-space handle.
 */
hal_space_t
hal_task_get_space(
	hal_task_t handle)
{
	/* Reports the system space for an invalid task handle. */
	if (handle == NULL)
		return HAL_SPACE_SYS;

	/* Returns the task's stored address-space handle. */
	return ((struct task_info *)handle)->space;
}

/*
 * Transfers an inactive task to another logical CPU.
 */
int
hal_task_transfer(
	hal_task_t handle,
	hal_cpu_id_t target_cpu)
{
	struct task_info *task;

	/* Resolves and validates the task before querying the CPU count. */
	task = handle;

	/* Rejects a missing task handle. */
	if (task == NULL)
		return HAL_ERR_INVALID;

	/* Rejects a logical CPU outside the configured inventory. */
	if (target_cpu >= hal_cpu_count())
		return HAL_ERR_INVALID;

	/* Rejects transfer while the task is running on any CPU. */
	if (task->run_cpu >= 0)
		return HAL_ERR_BUSY;

	/* Publishes the new target CPU. */
	task->target_cpu = target_cpu;

	/* Reports a completed transfer. */
	return HAL_OK;
}

/*
 * Reports i386 task-record and owned-stack memory usage.
 */
void
hal_i386_task_memory_stats(
	uint32_t *count,
	size_t *stack_bytes)
{
	bool enabled;

	/* Preserves interrupt state before acquiring the task registry lock. */
	enabled = hal_irq_disable();

	/* Waits until this CPU owns the task registry lock. */
	while (__atomic_exchange_n(
	    &task_registry_lock,
	    1U,
	    __ATOMIC_ACQUIRE) != 0U) {
		__asm__ volatile("pause");
	}

	/* Copies each requested memory counter. */
	if (count != NULL)
		*count = task_count;

	/* Publishes owned stack bytes when requested. */
	if (stack_bytes != NULL)
		*stack_bytes = task_stack_bytes;

	/* Releases the registry lock before restoring interrupt state. */
	__atomic_store_n(&task_registry_lock, 0U, __ATOMIC_RELEASE);

	/* Restores interrupts only when they were previously enabled. */
	if (enabled)
		hal_irq_enable();
}

/* Appends one task to the serialized task registry. */
static void
tasklist_add(
	struct task_info *task)
{
	struct task_info **link;
	bool enabled;

	/* Preserves interrupt state before selecting the registry head. */
	enabled = hal_irq_disable();
	link = &task_list;

	/* Waits until this CPU owns the task registry lock. */
	while (__atomic_exchange_n(
	    &task_registry_lock,
	    1U,
	    __ATOMIC_ACQUIRE) != 0U) {
		__asm__ volatile("pause");
	}

	/* Finds the end of the task registry. */
	while (*link != NULL)
		link = &(*link)->next;

	/* Appends and accounts for the task and its optional owned stack. */
	*link = task;
	task->next = NULL;
	task_count++;

	/* Accounts for storage only when the task owns its stack. */
	if (task->owns_stack)
		task_stack_bytes += SYS_STACK_SIZE;

	/* Releases the registry lock before restoring interrupt state. */
	__atomic_store_n(&task_registry_lock, 0U, __ATOMIC_RELEASE);

	/* Restores interrupts only when they were previously enabled. */
	if (enabled)
		hal_irq_enable();
}

/* Removes one task from the serialized task registry. */
static void
tasklist_del(
	struct task_info *task)
{
	struct task_info **link;
	bool enabled;

	/* Preserves interrupt state before selecting the registry head. */
	enabled = hal_irq_disable();
	link = &task_list;

	/* Waits until this CPU owns the task registry lock. */
	while (__atomic_exchange_n(
	    &task_registry_lock,
	    1U,
	    __ATOMIC_ACQUIRE) != 0U) {
		__asm__ volatile("pause");
	}

	/* Finds the matching task registry link. */
	while (*link != NULL && *link != task)
		link = &(*link)->next;

	/* Unlinks and accounts for a task present in the registry. */
	if (*link == task) {
		*link = task->next;

		/* Avoids underflow while decrementing task-record accounting. */
		if (task_count != 0)
			task_count--;

		/* Decrements stack accounting only for a valid owned allocation. */
		if (task->owns_stack && task_stack_bytes >= SYS_STACK_SIZE)
			task_stack_bytes -= SYS_STACK_SIZE;
	}
	task->next = NULL;

	/* Releases the registry lock before restoring interrupt state. */
	__atomic_store_n(&task_registry_lock, 0U, __ATOMIC_RELEASE);

	/* Restores interrupts only when they were previously enabled. */
	if (enabled)
		hal_irq_enable();
}

/* Builds the first assembly resume frame for one new task. */
static void
set_initial_resume_frame(
	struct task_info *task,
	void (*start)(void *),
	void *arg,
	void *user_sp)
{
	struct task_resume_frame *frame;

	/* Places and clears the resume frame at the top of the kernel stack. */
	frame = (struct task_resume_frame *)((uintptr_t)task->sys_stack +
	    SYS_STACK_SIZE - sizeof(*frame));
	task->resume_esp = frame;
	hal_memset(frame, 0, sizeof(*frame));

	/* Installs the common dispatcher return context. */
	frame->eflags = asm_get_eflags();
	frame->ret_eip = (uint32_t)asm_task_entrypoint;

	/* Builds the privilege-specific initial register frame. */
	if (task->space == HAL_SPACE_SYS) {
		frame->ds = SEG_SYS_DATA;
		frame->es = SEG_SYS_DATA;
		frame->fs = SEG_SYS_DATA;
		frame->gs = SEG_SYS_DATA;
		frame->initial.sys.eip = (uint32_t)start;
		frame->initial.sys.cs = SEG_SYS_CODE;
		frame->initial.sys.eflags =
		    EFLAGS_IF | EFLAGS_RSV1 | EFLAGS_IOPL_0;
		frame->initial.sys.return_eip = 0;
		frame->initial.sys.param = (uint32_t)arg;
	} else {
		frame->ds = SEG_USER_DATA | SEG_RPL_3;
		frame->es = SEG_USER_DATA | SEG_RPL_3;
		frame->fs = SEG_USER_DATA | SEG_RPL_3;
		frame->gs = SEG_USER_DATA | SEG_RPL_3;
		frame->initial.user.eip = (uint32_t)start;
		frame->initial.user.cs = SEG_USER_CODE | SEG_RPL_3;
		frame->initial.user.eflags =
		    EFLAGS_IF | EFLAGS_RSV1 | EFLAGS_IOPL_0;
		frame->initial.user.esp = (uint32_t)user_sp;
		frame->initial.user.ss = SEG_USER_DATA | SEG_RPL_3;
	}
}
