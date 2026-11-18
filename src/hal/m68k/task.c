/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MC68030 HAL's tasks: creation, fork and exec, signal delivery and
 * the switch between them, for the soft-float ABI.
 *
 * A task is a kernel stack whose top holds what the switch reloads: the
 * saved registers of task.S and the address it returns to.  A user task
 * returns to user mode through the exception frame its last trap left on
 * that stack.  The port runs on one processor; the task list and the
 * running task change with interrupts off or from a single caller.
 */

#include <hal/hal.h>
#include "defs.h"
#include "exception.h"
#include "space.h"
#include "task.h"

/* The alignment of a task's record. */
#define TASK_RECORD_ALIGNMENT	16U

/* The bytes of the format and vector word of an exception frame, from its start. */
#define TASK_FRAME_FORMAT_HIGH	6U
#define TASK_FRAME_FORMAT_LOW	7U

/* The offset of the program counter in an exception frame. */
#define TASK_FRAME_PC		2U

/*
 * Every task, in creation order, and the one running.  running_task is
 * NULL only before the boot task is made.
 */
static struct m68k_task *task_list;
static struct m68k_task *running_task;

/*
 * The number of tasks and the bytes of their kernel stacks, for the memory
 * statistics.  Changed with the task list.
 */
static uint32_t task_count;
static size_t task_stack_bytes;

static struct m68k_task *task_allocate(void);
static int valid_user_pc_sp(hal_space_t space, uintptr_t pc, uintptr_t sp);
static void task_add(struct m68k_task *task);
static void task_remove(struct m68k_task *task);
static uint8_t *push32(uint8_t *stack, uint32_t value);
static void write32(uint8_t *field, uint32_t value);
static uint32_t read32(const uint8_t *field);
static uint16_t frame_format_vector(const struct m68k_saved_frame *frame);

/*
 * Wraps the processor's current context as its initial task.
 */
hal_task_t
hal_task_create_for_init_context(
	void)
{
	struct m68k_task *task;

	/* The processor has one initial context. */
	if (running_task != NULL)
		HAL_FATAL("m68k initial task made twice");

	/* Allocates the task's record; the current context needs no stack of its own. */
	task = task_allocate();
	if (task == NULL)
		return NULL;

	/* Publishes the task as the one running. */
	task->space = HAL_SPACE_SYS;
	task->run_cpu = 0;
	task_add(task);
	running_task = task;

	/* Succeeded: the boot context is now a task. */
	return task;
}

/*
 * Creates a kernel task, or a user task that starts in a user space.
 */
hal_task_t
hal_task_create(
	hal_space_t space,
	void (*start)(void *p),
	void *arg,
	void *user_stack_pointer)
{
	struct m68k_task *task;
	uint8_t *stack;
	int user;
	int valid;

	/* A kernel task has no user stack, and a user task needs a valid one. */
	if (start == NULL)
		return NULL;
	user = 0;
	if (space != HAL_SPACE_SYS)
		user = 1;
	if (user && user_stack_pointer == NULL)
		return NULL;
	if (!user && user_stack_pointer != NULL)
		return NULL;
	if (user) {
		valid = valid_user_pc_sp(space, (uintptr_t)start, (uintptr_t)user_stack_pointer);
		if (!valid)
			return NULL;
	}

	/* Allocates the task's record. */
	task = task_allocate();
	if (task == NULL)
		return NULL;

	/* Allocates its kernel stack. */
	task->kernel_stack = kernel_alloc(M68K_KERNEL_STACK_SIZE);
	if (task->kernel_stack == NULL) {
		kernel_free(task->allocation);
		return NULL;
	}
	task_stack_bytes += M68K_KERNEL_STACK_SIZE;
	task->space = space;

	/*
	 * Pushes what the entry reads: the start function under the user stack
	 * or under the argument.
	 */
	stack = (uint8_t *)task->kernel_stack + M68K_KERNEL_STACK_SIZE;
	if (user) {
		stack = push32(stack, (uint32_t)(uintptr_t)user_stack_pointer);
	} else {
		stack = push32(stack, (uint32_t)(uintptr_t)arg);
	}
	stack = push32(stack, (uint32_t)(uintptr_t)start);

	/*
	 * Pushes the entry the first switch returns to, immediately above the
	 * registers the switch reloads, which start cleared.
	 */
	if (user) {
		stack = push32(stack, (uint32_t)(uintptr_t)m68k_user_task_start);
	} else {
		stack = push32(stack, (uint32_t)(uintptr_t)m68k_kernel_task_start);
	}
	stack -= M68K_DISPATCH_REG_BYTES;
	hal_memset(stack, 0, M68K_DISPATCH_REG_BYTES);
	task->resume_sp = stack;

	/* Succeeded: the task runs at its first switch. */
	task_add(task);
	return task;
}

/*
 * Records the user exception being handled, so fork, exec and signals can
 * replace what it returns to.
 */
void
m68k_task_enter_user_frame(
	struct m68k_saved_frame *frame)
{
	/* Nothing takes a user exception before the boot task exists. */
	if (running_task == NULL)
		return;

	/* Keeps the frame the exception saved. */
	running_task->active_user_frame = frame;
}

/*
 * Ends the user exception being handled.
 */
void
m68k_task_leave_user_frame(
	void)
{
	/* Nothing takes a user exception before the boot task exists. */
	if (running_task == NULL)
		return;

	/* No user exception is being handled any more. */
	running_task->active_user_frame = NULL;
}

/*
 * Creates a child task that returns from the current system call into a
 * copy of the caller's user context.
 */
hal_task_t
hal_task_fork_current(
	hal_space_t child_space,
	intptr_t child_syscall_result)
{
	struct m68k_saved_frame *source;
	struct m68k_task *child;
	uint16_t format_vector;
	size_t hardware_size;
	size_t frame_size;
	uint8_t *stack;

	/* Only a user task handling a system call can be forked. */
	if (running_task == NULL || child_space == HAL_SPACE_SYS)
		return NULL;
	source = running_task->active_user_frame;
	if (source == NULL)
		return NULL;

	/* Refuses an exception frame of a format the HAL does not know. */
	format_vector = frame_format_vector(source);
	hardware_size = m68k_exception_frame_size(format_vector);
	if (hardware_size == 0)
		return NULL;

	/* Allocates the child's record. */
	child = task_allocate();
	if (child == NULL)
		return NULL;

	/* Allocates its kernel stack. */
	child->kernel_stack = kernel_alloc(M68K_KERNEL_STACK_SIZE);
	if (child->kernel_stack == NULL) {
		kernel_free(child->allocation);
		return NULL;
	}
	task_stack_bytes += M68K_KERNEL_STACK_SIZE;
	child->space = child_space;
	child->tls = running_task->tls;

	/* Copies the caller's exception frame to the stack's top, with the child's own result. */
	frame_size = M68K_FRAME_HARDWARE_OFFSET + hardware_size;
	stack = (uint8_t *)child->kernel_stack + M68K_KERNEL_STACK_SIZE - frame_size;
	hal_memcpy(stack, source, frame_size);
	((struct m68k_saved_frame *)stack)->d[0] = (uint32_t)child_syscall_result;

	/* Makes the first switch return through the exception exit, with cleared registers. */
	stack = push32(stack, (uint32_t)(uintptr_t)m68k_exception_restore);
	stack -= M68K_DISPATCH_REG_BYTES;
	hal_memset(stack, 0, M68K_DISPATCH_REG_BYTES);
	child->resume_sp = stack;

	/* Succeeded: the child runs at its first switch. */
	task_add(child);
	return child;
}

/*
 * Checks that the current user exception can be replaced by a new program.
 */
int
hal_task_exec_validate(
	hal_space_t new_space,
	uintptr_t entry,
	uintptr_t user_stack_pointer)
{
	int valid;

	/* Only a user task handling a system call can exec. */
	if (running_task == NULL || new_space == HAL_SPACE_SYS)
		return -1;
	if (running_task->active_user_frame == NULL)
		return -1;

	/* Refuses an entry or a stack outside the user half. */
	valid = valid_user_pc_sp(new_space, entry, user_stack_pointer);
	if (!valid)
		return -1;

	/* Succeeded: the exception can be replaced. */
	return 0;
}

/*
 * Replaces the current user exception's return with the entry of a new
 * program in a new space.
 */
int
hal_task_exec_current(
	hal_space_t new_space,
	uintptr_t entry,
	uintptr_t user_stack_pointer)
{
	struct m68k_saved_frame *frame;
	int valid;

	/* Refuses an exception that cannot be replaced. */
	valid = hal_task_exec_validate(new_space, entry, user_stack_pointer);
	if (valid != 0)
		return -1;

	/* Clears the user registers and returns to the entry on the new stack. */
	frame = running_task->active_user_frame;
	hal_memset(frame->d, 0, sizeof(frame->d));
	hal_memset(frame->a, 0, sizeof(frame->a));
	frame->usp = (uint32_t)user_stack_pointer;
	write32(frame->hardware + TASK_FRAME_PC, (uint32_t)entry);

	/* Gives the task the new space, no thread pointer and no signal in progress. */
	running_task->space = new_space;
	running_task->tls = 0;
	running_task->signal_depth = 0;
	hal_memset(running_task->signal_token, 0, sizeof(running_task->signal_token));
	hal_space_switch(new_space);

	/* Succeeded: the exception returns into the new program. */
	return 0;
}

/*
 * Reports the stack pointer the current user exception returns to.
 */
uintptr_t
hal_task_get_user_stack(
	void)
{
	/* Outside a user exception there is none. */
	if (running_task == NULL || running_task->active_user_frame == NULL)
		return 0;

	/* Succeeded: the user stack pointer the exception saved. */
	return running_task->active_user_frame->usp;
}

/*
 * Reports the program counter, the stack pointer and the return value the
 * current user exception returns with.
 */
int
hal_task_get_user_context(
	uintptr_t *pc,
	uintptr_t *stack_pointer,
	intptr_t *return_value)
{
	struct m68k_saved_frame *frame;

	/* Outside a user exception there is none. */
	if (running_task == NULL || running_task->active_user_frame == NULL)
		return -1;
	frame = running_task->active_user_frame;

	/* Reports what the caller asked for: the frame's pc, the saved usp and %d0. */
	if (pc != NULL)
		*pc = read32(frame->hardware + TASK_FRAME_PC);
	if (stack_pointer != NULL)
		*stack_pointer = frame->usp;
	if (return_value != NULL)
		*return_value = (intptr_t)(int32_t)frame->d[0];

	/* Succeeded. */
	return 0;
}

/*
 * Makes the current user exception return into a signal handler.
 *
 * The C library's trampoline finds the signal number, the information and
 * the context on the stack it is given, so only the pc and the stack
 * change here.
 */
int
hal_task_signal_enter(
	uintptr_t handler,
	uintptr_t stack,
	int signo,
	uintptr_t siginfo,
	uintptr_t ucontext,
	uintptr_t restorer,
	uint32_t token)
{
	struct m68k_saved_frame *frame;
	uint16_t format_vector;
	size_t size;
	unsigned depth;

	UNUSED_PARAMETER(signo);
	UNUSED_PARAMETER(siginfo);
	UNUSED_PARAMETER(ucontext);
	UNUSED_PARAMETER(restorer);

	/* Only a user exception with room for one more handler can enter one. */
	if (running_task == NULL || running_task->active_user_frame == NULL)
		return -1;
	if (running_task->signal_depth >= HAL_SIGNAL_NEST_MAX)
		return -1;

	/* Refuses a missing handler, stack or token. */
	if (handler == 0 || stack == 0 || token == 0)
		return -1;
	frame = running_task->active_user_frame;

	/* Refuses a frame of a format the HAL does not know, or one too large to keep. */
	format_vector = frame_format_vector(frame);
	size = M68K_FRAME_HARDWARE_OFFSET + m68k_exception_frame_size(format_vector);
	if (size <= M68K_FRAME_HARDWARE_OFFSET)
		return -1;
	if (size > sizeof(running_task->signal_frame[0]))
		return -1;

	/* Keeps the interrupted frame for the handler's return. */
	depth = running_task->signal_depth;
	hal_memcpy(running_task->signal_frame[depth], frame, size);
	running_task->signal_frame_size[depth] = size;
	running_task->signal_token[depth] = token;
	running_task->signal_depth = depth + 1U;

	/* Returns into the handler on its stack. */
	write32(frame->hardware + TASK_FRAME_PC, (uint32_t)handler);
	frame->usp = (uint32_t)stack;

	/* Succeeded: the exception returns into the handler. */
	return 0;
}

/*
 * Returns from a signal handler into the frame it interrupted.
 */
int
hal_task_signal_return(
	uint32_t token,
	intptr_t *return_value)
{
	struct m68k_saved_frame *frame;
	unsigned depth;

	/* Only a user exception inside a handler can return from it. */
	if (running_task == NULL || running_task->active_user_frame == NULL)
		return -1;
	if (return_value == NULL || running_task->signal_depth == 0)
		return -1;
	if (token == 0)
		return -1;

	/* Refuses a token that is not the innermost handler's. */
	depth = running_task->signal_depth - 1U;
	if (token != running_task->signal_token[depth])
		return -1;

	/* Puts the interrupted frame back. */
	frame = running_task->active_user_frame;
	hal_memcpy(frame, running_task->signal_frame[depth], running_task->signal_frame_size[depth]);
	*return_value = (intptr_t)(int32_t)frame->d[0];

	/* Closes the handler's level. */
	running_task->signal_token[depth] = 0;
	running_task->signal_depth = depth;

	/* Succeeded: the exception returns into the interrupted frame. */
	return 0;
}

/*
 * Destroys a task that is not running.
 */
void
hal_task_destroy(
	hal_task_t handle)
{
	struct m68k_task *task;

	/* Destroying nothing does nothing; a running task cannot free its stack. */
	task = handle;
	if (task == NULL)
		return;
	if (task == running_task)
		HAL_FATAL("destroying current m68k task");

	/* Takes the task off the list and frees its stack and record. */
	task_remove(task);
	if (task->kernel_stack != NULL) {
		task_stack_bytes -= M68K_KERNEL_STACK_SIZE;
		kernel_free(task->kernel_stack);
	}
	kernel_free(task->allocation);
}

/*
 * Switches the processor to another task.
 */
void
hal_task_context_switch(
	hal_task_t handle)
{
	struct m68k_task *next;
	struct m68k_task *previous;

	/* Refuses a missing task, or a switch before the boot task exists. */
	next = handle;
	previous = running_task;
	if (next == NULL || previous == NULL)
		HAL_FATAL("invalid m68k task switch");

	/* Switching to the running task changes nothing. */
	if (next == previous)
		return;

	/* A task runs on one processor at a time. */
	if (next->run_cpu >= 0)
		HAL_FATAL("m68k HAL task already running");

	/* Hands the processor over, selects the incoming space and switches stacks. */
	previous->run_cpu = -1;
	next->run_cpu = 0;
	running_task = next;
	hal_space_switch(next->space);
	m68k_task_dispatch(&previous->resume_sp, next->resume_sp);
}

/*
 * Waits for the next interrupt with interrupts enabled, then disables them.
 */
void
hal_cpu_idle(
	void)
{
	/* STOP waits in task.S. */
	m68k_cpu_idle();
}

/*
 * Reports the running task.
 */
hal_task_t
hal_task_get_current(
	void)
{
	/* Succeeded: the task the processor runs. */
	return running_task;
}

/*
 * Sets the thread pointer of a task.
 */
void
hal_task_set_tls(
	hal_task_t handle,
	uintptr_t value)
{
	struct m68k_task *task;

	/* Ignores a missing task. */
	task = handle;
	if (task == NULL)
		return;

	/* Keeps the value, which the C library asks the kernel for. */
	task->tls = value;
}

/*
 * Reports the thread pointer of a task.
 */
uintptr_t
hal_task_get_tls(
	hal_task_t handle)
{
	struct m68k_task *task;

	/* A missing task has none. */
	task = handle;
	if (task == NULL)
		return 0;

	/* Succeeded: the value last set. */
	return task->tls;
}

/*
 * Stores the kernel's link from a task to its own record.
 */
void
hal_task_set_private(
	hal_task_t handle,
	void *private_data)
{
	struct m68k_task *task;

	/* Ignores a missing task. */
	task = handle;
	if (task == NULL)
		return;

	/* The HAL keeps the link and never follows it. */
	task->private_data = private_data;
}

/*
 * Reports the kernel's link from a task to its own record.
 */
void *
hal_task_get_private(
	hal_task_t handle)
{
	struct m68k_task *task;

	/* A missing task has none. */
	task = handle;
	if (task == NULL)
		return NULL;

	/* Succeeded: the link last stored. */
	return task->private_data;
}

/*
 * Reports the address space a task runs in.
 */
hal_space_t
hal_task_get_space(
	hal_task_t handle)
{
	struct m68k_task *task;

	/* A missing task is taken as the kernel's. */
	task = handle;
	if (task == NULL)
		return HAL_SPACE_SYS;

	/* Succeeded: the task's space. */
	return task->space;
}

/*
 * Moves a task to a processor; there is only one.
 */
int
hal_task_transfer(
	hal_task_t handle,
	hal_cpu_id_t target_cpu)
{
	struct m68k_task *task;

	/* Refuses a missing task or a processor that does not exist. */
	task = handle;
	if (task == NULL || target_cpu != 0)
		return HAL_ERR_INVALID;

	/* Refuses a task that is running. */
	if (task->run_cpu >= 0)
		return HAL_ERR_BUSY;

	/* Succeeded: the task already belongs to the only processor. */
	return HAL_OK;
}

/*
 * Reports the number of tasks and the bytes of their kernel stacks.
 */
void
m68k030_task_memory_stats(
	uint32_t *count,
	size_t *stack_bytes)
{
	/* Reports what the caller asked for. */
	if (count != NULL)
		*count = task_count;
	if (stack_bytes != NULL)
		*stack_bytes = task_stack_bytes;
}

/* Allocates a cleared task record, aligned for the dispatch code. */
static struct m68k_task *
task_allocate(
	void)
{
	struct m68k_task *task;
	void *allocation;

	/* Allocates the record with room to align it. */
	allocation = kernel_alloc(sizeof(struct m68k_task) + TASK_RECORD_ALIGNMENT - 1U);
	if (allocation == NULL)
		return NULL;

	/* Aligns and clears it; the task is not running anywhere yet. */
	task = (struct m68k_task *)(((uintptr_t)allocation + TASK_RECORD_ALIGNMENT - 1U) & ~(uintptr_t)(TASK_RECORD_ALIGNMENT - 1U));
	hal_memset(task, 0, sizeof(*task));
	task->allocation = allocation;
	task->run_cpu = -1;

	/* Succeeded: the record. */
	return task;
}

/* Reports whether an entry and a stack are in the user half and aligned. */
static int
valid_user_pc_sp(
	hal_space_t space,
	uintptr_t pc,
	uintptr_t sp)
{
	/* A kernel space runs no user code. */
	if (space == HAL_SPACE_SYS)
		return 0;

	/* The entry is an even address past page zero, in the user half. */
	if (pc < M68K030_PAGE_SIZE || pc >= M68K030_USER_LIMIT)
		return 0;
	if ((pc & 1U) != 0)
		return 0;

	/* The stack is a longword-aligned address past page zero, in the user half. */
	if (sp <= M68K030_PAGE_SIZE || sp >= M68K030_USER_LIMIT)
		return 0;
	if ((sp & 3U) != 0)
		return 0;

	/* Both are usable. */
	return 1;
}

/* Appends a task to the list. */
static void
task_add(
	struct m68k_task *task)
{
	struct m68k_task **link;

	/* Finds the list's end. */
	link = &task_list;
	while (*link != NULL)
		link = &(*link)->next;

	/* Links the task there. */
	*link = task;
	task->next = NULL;
	task_count++;
}

/* Takes a task off the list. */
static void
task_remove(
	struct m68k_task *task)
{
	struct m68k_task **link;

	/* Finds the link that points at the task. */
	link = &task_list;
	while (*link != NULL && *link != task)
		link = &(*link)->next;

	/* A task not on the list is left alone. */
	if (*link != task)
		return;

	/* Unlinks the task. */
	*link = task->next;
	if (task_count != 0)
		task_count--;
}

/* Pushes a big-endian longword below a stack pointer and reports the new one. */
static uint8_t *
push32(
	uint8_t *stack,
	uint32_t value)
{
	/* Moves down and stores the longword. */
	stack -= 4;
	write32(stack, value);

	/* Succeeded: the new stack pointer. */
	return stack;
}

/* Stores a big-endian longword at a byte address. */
static void
write32(
	uint8_t *field,
	uint32_t value)
{
	/* Most significant byte first. */
	field[0] = (uint8_t)(value >> 24);
	field[1] = (uint8_t)(value >> 16);
	field[2] = (uint8_t)(value >> 8);
	field[3] = (uint8_t)value;
}

/* Loads a big-endian longword from a byte address. */
static uint32_t
read32(
	const uint8_t *field)
{
	uint32_t value;

	/* Most significant byte first. */
	value = (uint32_t)field[0] << 24;
	value |= (uint32_t)field[1] << 16;
	value |= (uint32_t)field[2] << 8;
	value |= field[3];

	/* Succeeded: the longword. */
	return value;
}

/* Reads the format and vector word of a saved exception frame. */
static uint16_t
frame_format_vector(
	const struct m68k_saved_frame *frame)
{
	uint16_t format_vector;

	/* The word is big-endian, after the status register and the pc. */
	format_vector = (uint16_t)((uint16_t)frame->hardware[TASK_FRAME_FORMAT_HIGH] << 8);
	format_vector |= frame->hardware[TASK_FRAME_FORMAT_LOW];

	/* Succeeded: the word. */
	return format_vector;
}
