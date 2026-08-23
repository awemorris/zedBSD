/* MC68030 soft-float task creation, fork/exec, signal, and switching. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "exception.h"
#include "space.h"
#include "task.h"

#define M68K_SYSCALL_INSTRUCTION_SIZE 2U

static struct m68k_task *task_list;
static struct m68k_task *running_task;
static uint32_t task_count;
static size_t task_stack_bytes;

static struct m68k_task *
task_allocate(void)
{
	void *allocation = hal_malloc(sizeof(struct m68k_task) + 15U);
	struct m68k_task *task;

	if (allocation == NULL)
		return NULL;
	task = (struct m68k_task *)(((uintptr_t)allocation + 15U) &
		~(uintptr_t)15U);
	hal_memset(task, 0, sizeof(*task));
	task->allocation = allocation;
	task->run_cpu = -1;
	return task;
}

static int
valid_user_pc_sp(hal_space_t space, uintptr_t pc, uintptr_t sp)
{
	if (space == HAL_SPACE_SYS || pc < M68K030_PAGE_SIZE ||
	    pc >= M68K030_USER_LIMIT || (pc & 1U) != 0 ||
	    sp <= M68K030_PAGE_SIZE || sp >= M68K030_USER_LIMIT ||
	    (sp & 3U) != 0)
		return 0;
	return 1;
}

static void
task_add(struct m68k_task *task)
{
	struct m68k_task **link = &task_list;
	while (*link != NULL)
		link = &(*link)->next;
	*link = task;
	task->next = NULL;
	task_count++;
}

static void
task_remove(struct m68k_task *task)
{
	struct m68k_task **link = &task_list;
	while (*link != NULL && *link != task)
		link = &(*link)->next;
	if (*link == task) {
		*link = task->next;
		if (task_count != 0)
			task_count--;
	}
}

static uint8_t *
push32(uint8_t *stack, uint32_t value)
{
	stack -= 4;
	stack[0] = (uint8_t)(value >> 24);
	stack[1] = (uint8_t)(value >> 16);
	stack[2] = (uint8_t)(value >> 8);
	stack[3] = (uint8_t)value;
	return stack;
}

static void
write32(uint8_t *field, uint32_t value)
{
	field[0] = (uint8_t)(value >> 24);
	field[1] = (uint8_t)(value >> 16);
	field[2] = (uint8_t)(value >> 8);
	field[3] = (uint8_t)value;
}

static uint32_t
read32(const uint8_t *field)
{
	return (uint32_t)field[0] << 24 | (uint32_t)field[1] << 16 |
	    (uint32_t)field[2] << 8 | field[3];
}

void
hal_task_init(void)
{
	struct m68k_task *task;
	if (running_task != NULL)
		HAL_FATAL("m68k task init called twice");
	task = task_allocate();
	if (task == NULL)
		HAL_FATAL("initial m68k task allocation failed");
	task->space = HAL_SPACE_SYS;
	task->run_cpu = 0;
	task_add(task);
	running_task = task;
}

hal_task_t
hal_task_create(hal_space_t space, void (*start)(void *), void *argument,
		void *user_stack_pointer)
{
	struct m68k_task *task;
	uint8_t *stack;
	int user = space != HAL_SPACE_SYS;

	if (start == NULL || (user != (user_stack_pointer != NULL)) ||
	    (user && !valid_user_pc_sp(space, (uintptr_t)start,
	    (uintptr_t)user_stack_pointer)))
		return NULL;
	task = task_allocate();
	if (task == NULL)
		return NULL;
	task->kernel_stack = hal_malloc(M68K_KERNEL_STACK_SIZE);
	if (task->kernel_stack == NULL) {
		hal_free(task->allocation);
		return NULL;
	}
	task_stack_bytes += M68K_KERNEL_STACK_SIZE;
	task->space = space;
	stack = (uint8_t *)task->kernel_stack + M68K_KERNEL_STACK_SIZE;
	if (user) {
		stack = push32(stack, (uint32_t)(uintptr_t)user_stack_pointer);
		stack = push32(stack, (uint32_t)(uintptr_t)start);
	} else {
		stack = push32(stack, (uint32_t)(uintptr_t)argument);
		stack = push32(stack, (uint32_t)(uintptr_t)start);
	}
	/* The return PC must sit immediately above the 44-byte dispatch save;
	 * retain the already-pushed start arguments above it. */
	stack = push32(stack, (uint32_t)(uintptr_t)(user ?
		m68k_user_task_start : m68k_kernel_task_start));
	stack -= M68K_DISPATCH_REG_BYTES;
	hal_memset(stack, 0, M68K_DISPATCH_REG_BYTES);
	task->resume_sp = stack;
	task_add(task);
	return task;
}

void
m68k_task_enter_user_frame(struct m68k_saved_frame *frame)
{
	if (running_task != NULL)
		running_task->active_user_frame = frame;
}

void
m68k_task_leave_user_frame(void)
{
	if (running_task != NULL)
		running_task->active_user_frame = NULL;
}

hal_task_t
hal_task_fork_current(hal_space_t child_space, intptr_t child_result)
{
	struct m68k_saved_frame *source;
	struct m68k_task *child;
	uint16_t format_vector;
	size_t hardware_size, frame_size;
	uint8_t *stack;

	if (running_task == NULL || child_space == HAL_SPACE_SYS ||
	    running_task->active_user_frame == NULL)
		return NULL;
	source = running_task->active_user_frame;
	format_vector = (uint16_t)((uint16_t)source->hardware[6] << 8 |
		source->hardware[7]);
	hardware_size = m68k_exception_frame_size(format_vector);
	if (hardware_size == 0)
		return NULL;
	child = task_allocate();
	if (child == NULL)
		return NULL;
	child->kernel_stack = hal_malloc(M68K_KERNEL_STACK_SIZE);
	if (child->kernel_stack == NULL) {
		hal_free(child->allocation);
		return NULL;
	}
	task_stack_bytes += M68K_KERNEL_STACK_SIZE;
	child->space = child_space;
	child->tls = running_task->tls;
	frame_size = M68K_FRAME_HARDWARE_OFFSET + hardware_size;
	stack = (uint8_t *)child->kernel_stack + M68K_KERNEL_STACK_SIZE -
		frame_size;
	hal_memcpy(stack, source, frame_size);
	((struct m68k_saved_frame *)stack)->d[0] = (uint32_t)child_result;
	stack = push32(stack, (uint32_t)(uintptr_t)m68k_exception_restore);
	stack -= M68K_DISPATCH_REG_BYTES;
	hal_memset(stack, 0, M68K_DISPATCH_REG_BYTES);
	child->resume_sp = stack;
	task_add(child);
	return child;
}

int
hal_task_exec_validate(hal_space_t new_space, uintptr_t entry,
	uintptr_t user_stack_pointer)
{
	return running_task != NULL && new_space != HAL_SPACE_SYS &&
	    valid_user_pc_sp(new_space, entry, user_stack_pointer) &&
	    running_task->active_user_frame != NULL ? 0 : -1;
}

int
hal_task_exec_current(hal_space_t new_space, uintptr_t entry,
		      uintptr_t user_stack_pointer)
{
	struct m68k_saved_frame *frame;
	if (hal_task_exec_validate(new_space, entry, user_stack_pointer) != 0)
		return -1;
	frame = running_task->active_user_frame;
	hal_memset(frame->d, 0, sizeof(frame->d));
	hal_memset(frame->a, 0, sizeof(frame->a));
	frame->usp = (uint32_t)user_stack_pointer;
	write32(frame->hardware + 2U, (uint32_t)entry);
	running_task->space = new_space;
	running_task->tls = 0;
	running_task->signal_depth = 0;
	hal_memset(running_task->signal_token, 0,
	    sizeof(running_task->signal_token));
	hal_page_switch_space(new_space);
	return 0;
}

uintptr_t
hal_task_user_stack(void)
{
	return running_task != NULL && running_task->active_user_frame != NULL ?
		running_task->active_user_frame->usp : 0;
}

int
hal_task_user_context(struct hal_user_context *context)
{
	struct m68k_saved_frame *frame = running_task != NULL ?
		running_task->active_user_frame : NULL;
	if (frame == NULL || context == NULL)
		return -1;
	context->pc = read32(frame->hardware + 2U);
	context->stack_pointer = frame->usp;
	context->return_value = (intptr_t)(int32_t)frame->d[0];
	return 0;
}

int
hal_task_signal_enter(uintptr_t handler, uintptr_t stack, int signal,
		      uintptr_t info, uintptr_t context, uintptr_t restorer,
		      uint32_t token)
{
	struct m68k_saved_frame *frame;
	uint16_t format_vector;
	size_t size;
	unsigned depth;
	(void)signal;
	(void)info;
	(void)context;
	(void)restorer;
	if (running_task == NULL || (frame = running_task->active_user_frame) ==
	    NULL || running_task->signal_depth >= HAL_SIGNAL_NEST_MAX ||
	    handler == 0 ||
	    stack == 0 || token == 0)
		return -1;
	format_vector = (uint16_t)((uint16_t)frame->hardware[6] << 8 |
		frame->hardware[7]);
	size = M68K_FRAME_HARDWARE_OFFSET +
		m68k_exception_frame_size(format_vector);
	if (size <= M68K_FRAME_HARDWARE_OFFSET ||
	    size > sizeof(running_task->signal_frame[0]))
		return -1;
	depth = running_task->signal_depth;
	hal_memcpy(running_task->signal_frame[depth], frame, size);
	running_task->signal_frame_size[depth] = size;
	running_task->signal_token[depth] = token;
	running_task->signal_depth = depth + 1U;
	write32(frame->hardware + 2U, (uint32_t)handler);
	frame->usp = (uint32_t)stack;
	return 0;
}

int
hal_task_signal_return(uint32_t token, intptr_t *return_value)
{
	struct m68k_saved_frame *frame;
	unsigned depth;
	if (running_task == NULL || (frame = running_task->active_user_frame) ==
	    NULL || return_value == NULL || running_task->signal_depth == 0 ||
	    token == 0)
		return -1;
	depth = running_task->signal_depth - 1U;
	if (token != running_task->signal_token[depth])
		return -1;
	hal_memcpy(frame, running_task->signal_frame[depth],
		running_task->signal_frame_size[depth]);
	*return_value = (intptr_t)(int32_t)frame->d[0];
	running_task->signal_token[depth] = 0;
	running_task->signal_depth = depth;
	return 0;
}

int
hal_task_signal_restart(uint32_t token, uint32_t number,
			const uintptr_t arguments[HAL_SYSCALL_ARGS],
			intptr_t *return_value)
{
	struct m68k_saved_frame *frame;
	uint32_t pc;
	unsigned depth;
	if (running_task == NULL || (frame = running_task->active_user_frame) ==
	    NULL || arguments == NULL || return_value == NULL ||
	    running_task->signal_depth == 0 || token == 0)
		return -1;
	depth = running_task->signal_depth - 1U;
	if (token != running_task->signal_token[depth])
		return -1;
	hal_memcpy(frame, running_task->signal_frame[depth],
	    running_task->signal_frame_size[depth]);
	pc = read32(frame->hardware + 2U);
	if (pc < M68K_SYSCALL_INSTRUCTION_SIZE)
		return -1;
	write32(frame->hardware + 2U,
	    pc - M68K_SYSCALL_INSTRUCTION_SIZE);
	frame->d[0] = number;
	frame->d[1] = (uint32_t)arguments[0];
	frame->d[2] = (uint32_t)arguments[1];
	frame->d[3] = (uint32_t)arguments[2];
	frame->d[4] = (uint32_t)arguments[3];
	frame->d[5] = (uint32_t)arguments[4];
	frame->a[0] = (uint32_t)arguments[5];
	*return_value = (intptr_t)(int32_t)number;
	running_task->signal_token[depth] = 0;
	running_task->signal_depth = depth;
	return 0;
}

void
hal_task_destroy(hal_task_t handle)
{
	struct m68k_task *task = handle;
	if (task == NULL)
		return;
	if (task == running_task)
		HAL_FATAL("destroying current m68k task");
	task_remove(task);
	if (task->kernel_stack != NULL) {
		task_stack_bytes -= M68K_KERNEL_STACK_SIZE;
		hal_free(task->kernel_stack);
	}
	hal_free(task->allocation);
}

void
hal_task_context_switch(hal_task_t handle)
{
	struct m68k_task *next = handle;
	struct m68k_task *previous = running_task;
	if (next == NULL || previous == NULL)
		HAL_FATAL("invalid m68k task switch");
	if (next == previous)
		return;
	if (next->run_cpu >= 0)
		HAL_FATAL("m68k HAL task already running");
	previous->run_cpu = -1;
	next->run_cpu = 0;
	running_task = next;
	hal_page_switch_space(next->space);
	m68k_task_dispatch(&previous->resume_sp, next->resume_sp);
}

void hal_cpu_idle(void) { m68k_cpu_idle(); }
hal_task_t hal_task_get_current(void) { return running_task; }
void hal_task_set_tls(hal_task_t h, uintptr_t v) { if (h != NULL) ((struct m68k_task *)h)->tls = v; }
uintptr_t hal_task_get_tls(hal_task_t h) { return h != NULL ? ((struct m68k_task *)h)->tls : 0; }
void hal_task_set_private(hal_task_t h, void *p) { if (h != NULL) ((struct m68k_task *)h)->private_data = p; }
void *hal_task_get_private(hal_task_t h) { return h != NULL ? ((struct m68k_task *)h)->private_data : NULL; }
hal_space_t hal_task_get_space(hal_task_t h) { return h != NULL ? ((struct m68k_task *)h)->space : HAL_SPACE_SYS; }

int
hal_task_transfer(hal_task_t handle, hal_cpu_id_t target_cpu)
{
	struct m68k_task *task = handle;
	if (task == NULL || target_cpu != 0)
		return HAL_ERR_INVALID;
	if (task->run_cpu >= 0)
		return HAL_ERR_BUSY;
	return HAL_OK;
}

void
hal_m68k_task_memory_stats(uint32_t *count, size_t *stack_bytes)
{
	if (count != NULL)
		*count = task_count;
	if (stack_bytes != NULL)
		*stack_bytes = task_stack_bytes;
}
