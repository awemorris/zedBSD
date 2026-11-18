/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPARC V9 HAL's tasks.
 *
 * A task is a saved set of register windows, a kernel stack and, for a
 * user task, the user context its last trap saved.  A switch saves every
 * window of the running task and loads every window of the next, along
 * with the trap registers of a task switched out while it handled a trap
 * from user mode.  The port runs on one processor; the task list and the
 * running task change with interrupts off.
 */

#include <hal/hal.h>
#include "asi.h"
#include "defs.h"
#include "runtime.h"
#include "task.h"

/* The window registers of a fresh context: all windows free but the current one. */
#define TASK_INITIAL_CANSAVE	6U
#define TASK_INITIAL_CLEANWIN	7U

/* The thread pointer is %g7. */
#define TASK_GLOBAL_THREAD_POINTER	7U

/* A window's in registers, by their index after its eight locals. */
#define TASK_WINDOW_IN0		8U
#define TASK_WINDOW_IN1		9U
#define TASK_WINDOW_IN2		10U
#define TASK_WINDOW_IN6		14U
#define TASK_WINDOW_IN7		15U

/* A window's first two locals, which a new task's entry reads. */
#define TASK_WINDOW_LOCAL0	0U
#define TASK_WINDOW_LOCAL1	1U

/* The condition codes and the window pointer of a trap state, kept apart from what exec clears. */
#define TASK_TSTATE_CWP_MASK	0x1fULL

/*
 * Every task, in creation order, and the one running.  Changed with
 * interrupts off; running is NULL only before the boot task is made.
 */
static struct sparcv9_task *task_list;
static struct sparcv9_task *running;

/*
 * The number of tasks and the bytes of their kernel stacks, for the
 * memory statistics.  Changed with the task list.
 */
static uint32_t task_count;
static size_t task_stack_bytes;

/*
 * The running task's kernel stack top and user window save area, which
 * the trap entry reads; changed with the running task.
 */
uintptr_t sparcv9_current_trap_sp;
struct sparcv9_window_context *sparcv9_current_user_windows;

static void list_add(struct sparcv9_task *task);
static void list_remove(struct sparcv9_task *task);
static void build_context(struct sparcv9_task *task, void (*start)(void *), void *argument, void *user_stack);
static void save_trap_state(struct sparcv9_task *task);
static void restore_trap_state(const struct sparcv9_task *task);
static void set_user_trap(uintptr_t pc, uintptr_t next_pc, uint64_t state);
static unsigned user_output_window(const struct sparcv9_window_context *context);

/*
 * Wraps the processor's current context as its initial task.
 */
hal_task_t
hal_task_create_for_init_context(
	void)
{
	struct sparcv9_task *task;
	bool enabled;

	/* The processor has one initial context. */
	if (running != NULL)
		HAL_FATAL("SPARC V9 initial task made twice");

	/* Allocates the task's record. */
	task = kernel_alloc(sizeof(*task));
	if (task == NULL)
		return NULL;

	/* The current context needs no stack of its own: it runs on the boot stack. */
	hal_memset(task, 0, sizeof(*task));
	task->space = HAL_SPACE_SYS;
	task->run_cpu = 0;

	/* Publishes the task as the one running. */
	enabled = hal_irq_disable();

	list_add(task);
	running = task;

	if (enabled)
		hal_irq_enable();

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
	struct sparcv9_task *task;
	bool enabled;

	/* A kernel task has no user stack, and a user task needs one. */
	if (start == NULL)
		return NULL;
	if (space == HAL_SPACE_SYS && user_stack_pointer != NULL)
		return NULL;
	if (space != HAL_SPACE_SYS && user_stack_pointer == NULL)
		return NULL;

	/* Allocates the task's record. */
	task = kernel_alloc(sizeof(*task));
	if (task == NULL)
		return NULL;
	hal_memset(task, 0, sizeof(*task));

	/* Allocates the kernel stack, 16-byte aligned. */
	task->stack_allocation = kernel_alloc(SPARCV9_SYS_STACK_SIZE + 15U);
	if (task->stack_allocation == NULL) {
		kernel_free(task);
		return NULL;
	}
	task->stack = (void *)(((uintptr_t)task->stack_allocation + 15U) & ~(uintptr_t)15U);

	/* Builds the context the first switch to the task loads. */
	task->space = space;
	task->run_cpu = -1;
	build_context(task, start, arg, user_stack_pointer);

	/* Publishes the task. */
	enabled = hal_irq_disable();

	task_stack_bytes += SPARCV9_SYS_STACK_SIZE;
	list_add(task);

	if (enabled)
		hal_irq_enable();

	/* Succeeded: the task runs at its first switch. */
	return task;
}

/*
 * Destroys a task that is not running.
 */
void
hal_task_destroy(
	hal_task_t handle)
{
	struct sparcv9_task *task;
	bool enabled;

	/* Destroying nothing does nothing. */
	task = handle;
	if (task == NULL)
		return;

	/* A running task cannot free the stack it runs on. */
	if (task == running)
		HAL_FATAL("destroy current SPARC V9 task");

	/* Takes the task off the list. */
	enabled = hal_irq_disable();

	list_remove(task);
	if (task->stack != NULL)
		task_stack_bytes -= SPARCV9_SYS_STACK_SIZE;

	if (enabled)
		hal_irq_enable();

	/* Frees the stack and the record. */
	if (task->stack_allocation != NULL)
		kernel_free(task->stack_allocation);
	kernel_free(task);
}

/*
 * Switches the processor to another task.
 */
void
hal_task_context_switch(
	hal_task_t handle)
{
	struct sparcv9_task *to;
	struct sparcv9_task *from;

	/* Refuses a missing task, or a switch before the boot task exists. */
	to = handle;
	from = running;
	if (to == NULL || from == NULL)
		HAL_FATAL("invalid SPARC V9 task switch");

	/* Switching to the running task changes nothing. */
	if (to == from)
		return;

	/* A task runs on one processor at a time. */
	if (to->run_cpu >= 0)
		HAL_FATAL("SPARC V9 HAL task already running");

	/* Hands the processor over and keeps the outgoing task's trap registers. */
	from->run_cpu = -1;
	to->run_cpu = 0;
	save_trap_state(from);

	/* Points the trap entry at the incoming task and selects its space. */
	running = to;
	sparcv9_current_trap_sp = to->trap_sp;
	sparcv9_current_user_windows = &to->trap_windows;
	hal_space_switch(to->space);
	restore_trap_state(to);

	/* Saves the outgoing windows and loads the incoming ones; returns when switched back. */
	sparcv9_task_dispatch(&from->windows, &to->windows);
}

/*
 * Stops a kernel task whose function returned.
 */
void
sparcv9_task_returned(
	void)
{
	/* A task function must not return. */
	HAL_FATAL("SPARC V9 task returned");
	sparcv9_halt_forever();
}

/*
 * Reports the running task.
 */
hal_task_t
hal_task_get_current(
	void)
{
	/* Succeeded: the task the processor runs. */
	return running;
}

/*
 * Sets the thread pointer of a task.
 */
void
hal_task_set_tls(
	hal_task_t handle,
	uintptr_t value)
{
	struct sparcv9_task *task;

	/* Ignores a missing task. */
	task = handle;
	if (task == NULL)
		return;

	/* Keeps the value in both saved contexts, and in %g7 when the task runs. */
	task->tls = value;
	task->windows.global[TASK_GLOBAL_THREAD_POINTER] = value;
	task->trap_windows.global[TASK_GLOBAL_THREAD_POINTER] = value;
	if (task == running)
		sparcv9_tls_write(value);
}

/*
 * Reports the thread pointer of a task.
 */
uintptr_t
hal_task_get_tls(
	hal_task_t handle)
{
	struct sparcv9_task *task;

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
	struct sparcv9_task *task;

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
	struct sparcv9_task *task;

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
	struct sparcv9_task *task;

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
	struct sparcv9_task *task;

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
 * Records the user trap being handled, so fork, exec and signals can
 * replace the context it returns to.
 */
void
sparcv9_task_enter_user_frame(
	struct sparcv9_user_trap_frame *frame,
	uintptr_t pc,
	uintptr_t next_pc,
	uint64_t tstate,
	uint64_t trap_type)
{
	/* Nothing runs a user trap before the boot task exists. */
	if (running == NULL)
		return;

	/* Keeps where the trap came from and the registers it saved. */
	running->active_user_frame = frame;
	running->active_pc = pc;
	running->active_next_pc = next_pc;
	running->active_tstate = tstate;
	running->active_trap_type = trap_type;
}

/*
 * Ends the user trap being handled.
 */
void
sparcv9_task_leave_user_frame(
	void)
{
	/* Nothing runs a user trap before the boot task exists. */
	if (running == NULL)
		return;

	/* No user trap is being handled any more. */
	running->active_user_frame = NULL;
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
	struct sparcv9_task *child;
	struct sparcv9_user_trap_frame *frame;
	unsigned window;

	/* Only a user task handling a system call can be forked. */
	if (running == NULL || child_space == HAL_SPACE_SYS)
		return NULL;
	frame = running->active_user_frame;
	if (frame == NULL)
		return NULL;

	/* Creates the child on the caller's user stack. */
	child = hal_task_create(child_space,
				(void (*)(void *))(uintptr_t)running->active_next_pc,
				NULL,
				(void *)(frame->old_sp + SPARCV9_STACK_BIAS));
	if (child == NULL)
		return NULL;

	/* Copies the caller's user windows and gives the child its own result. */
	child->trap_windows = running->trap_windows;
	window = user_output_window(&child->trap_windows);
	child->trap_windows.window[window][TASK_WINDOW_IN0] = (uint64_t)child_syscall_result;

	/* Keeps the caller's thread pointer. */
	child->tls = sparcv9_tls_read();
	child->trap_windows.global[TASK_GLOBAL_THREAD_POINTER] = child->tls;

	/* Returns from the same trap the caller is in. */
	child->trap_level = 1;
	child->trap_pc = running->active_pc;
	child->trap_next_pc = running->active_next_pc;
	child->trap_state = running->active_tstate;
	child->trap_type = running->active_trap_type;

	/* Starts the child at the entry that reloads the user windows. */
	child->windows.window[1][TASK_WINDOW_IN7] = (uintptr_t)sparcv9_user_fork_entry - 8U;

	/* Succeeded: the child runs at its first switch. */
	return child;
}

/*
 * Checks that the current user trap can be replaced by a new program.
 */
int
hal_task_exec_validate(
	hal_space_t new_space,
	uintptr_t entry,
	uintptr_t user_stack_pointer)
{
	/* Only a user task handling a system call can exec. */
	if (running == NULL || new_space == HAL_SPACE_SYS)
		return -1;
	if (running->active_user_frame == NULL)
		return -1;

	/* Refuses a missing entry or a stack below the bias. */
	if (entry == 0 || user_stack_pointer < SPARCV9_STACK_BIAS)
		return -1;

	/* Succeeded: the trap can be replaced. */
	return 0;
}

/*
 * Replaces the current user trap's return with the entry of a new program
 * in a new space.
 */
int
hal_task_exec_current(
	hal_space_t new_space,
	uintptr_t entry,
	uintptr_t user_stack_pointer)
{
	struct sparcv9_user_trap_frame *frame;
	uint64_t state;
	int valid;

	/* Refuses a trap that cannot be replaced. */
	valid = hal_task_exec_validate(new_space, entry, user_stack_pointer);
	if (valid != 0)
		return -1;

	/* Keeps the trap state but its window pointer, which starts at zero. */
	frame = running->active_user_frame;
	state = running->active_tstate & ~TASK_TSTATE_CWP_MASK;

	/* Starts the user windows empty with the new stack. */
	hal_memset(&running->trap_windows, 0, sizeof(running->trap_windows));
	running->trap_windows.cansave = TASK_INITIAL_CANSAVE;
	running->trap_windows.cleanwin = TASK_INITIAL_CLEANWIN;
	running->trap_windows.window[1][TASK_WINDOW_IN6] = user_stack_pointer - SPARCV9_STACK_BIAS;

	/* Clears the saved user registers but the stack. */
	hal_memset(frame, 0, sizeof(*frame));
	frame->old_sp = user_stack_pointer - SPARCV9_STACK_BIAS;

	/* Gives the task the new space, no thread pointer and no signal in progress. */
	running->space = new_space;
	running->tls = 0;
	running->signal_depth = 0;

	/* Returns from the trap to the entry: done goes to the next pc. */
	running->active_pc = entry - 4U;
	running->active_next_pc = entry;
	running->active_tstate = state;
	hal_space_switch(new_space);
	set_user_trap(entry - 4U, entry, state);

	/* Succeeded: the trap returns into the new program. */
	return 0;
}

/*
 * Reports the stack pointer the current user trap returns to.
 */
uintptr_t
hal_task_get_user_stack(
	void)
{
	/* Outside a user trap there is none. */
	if (running == NULL || running->active_user_frame == NULL)
		return 0;

	/* Succeeded: the user's stack, without the bias. */
	return running->active_user_frame->old_sp + SPARCV9_STACK_BIAS;
}

/*
 * Reports the program counter, the stack pointer and the return value the
 * current user trap returns with.
 */
int
hal_task_get_user_context(
	uintptr_t *pc,
	uintptr_t *stack_pointer,
	intptr_t *return_value)
{
	struct sparcv9_user_trap_frame *frame;

	/* Outside a user trap there is none. */
	if (running == NULL || running->active_user_frame == NULL)
		return -1;
	frame = running->active_user_frame;

	/* Reports what the caller asked for. */
	if (pc != NULL)
		*pc = (uintptr_t)running->active_next_pc;
	if (stack_pointer != NULL)
		*stack_pointer = frame->old_sp + SPARCV9_STACK_BIAS;
	if (return_value != NULL)
		*return_value = (intptr_t)frame->out[0];

	/* Succeeded. */
	return 0;
}

/*
 * Makes the current user trap return into a signal handler.
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
	struct sparcv9_user_trap_frame *frame;
	unsigned window;
	unsigned depth;

	/* Only a user trap with room for one more handler can enter one. */
	if (running == NULL || running->active_user_frame == NULL)
		return -1;
	if (running->signal_depth >= HAL_SIGNAL_NEST_MAX)
		return -1;

	/* Refuses a missing handler, stack, return path or token. */
	if (handler == 0 || stack < SPARCV9_STACK_BIAS)
		return -1;
	if (restorer == 0 || token == 0)
		return -1;
	frame = running->active_user_frame;

	/* Keeps the interrupted context for the handler's return. */
	depth = running->signal_depth;
	running->signal_windows[depth] = running->trap_windows;
	running->signal_frame[depth] = *frame;
	running->signal_pc[depth] = running->active_pc;
	running->signal_next_pc[depth] = running->active_next_pc;
	running->signal_tstate[depth] = running->active_tstate;
	running->signal_token[depth] = token;
	running->signal_depth = depth + 1U;

	/* Passes the handler its arguments, its stack and its return address. */
	window = user_output_window(&running->trap_windows);
	running->trap_windows.window[window][TASK_WINDOW_IN0] = (uint64_t)signo;
	running->trap_windows.window[window][TASK_WINDOW_IN1] = siginfo;
	running->trap_windows.window[window][TASK_WINDOW_IN2] = ucontext;
	running->trap_windows.window[window][TASK_WINDOW_IN6] = stack - SPARCV9_STACK_BIAS;
	running->trap_windows.window[window][TASK_WINDOW_IN7] = restorer - 8U;
	frame->old_sp = stack - SPARCV9_STACK_BIAS;
	frame->out[0] = (uint64_t)signo;
	frame->out[1] = siginfo;
	frame->out[2] = ucontext;

	/* Returns from the trap into the handler. */
	running->active_pc = handler - 4U;
	running->active_next_pc = handler;
	set_user_trap(handler - 4U, handler, running->active_tstate);

	/* Succeeded: the trap returns into the handler. */
	return 0;
}

/*
 * Returns from a signal handler into the context it interrupted.
 */
int
hal_task_signal_return(
	uint32_t token,
	intptr_t *return_value)
{
	struct sparcv9_user_trap_frame *frame;
	unsigned depth;

	/* Only a user trap inside a handler can return from it. */
	if (running == NULL || running->active_user_frame == NULL)
		return -1;
	if (return_value == NULL || running->signal_depth == 0)
		return -1;
	if (token == 0)
		return -1;

	/* Refuses a token that is not the innermost handler's. */
	depth = running->signal_depth - 1U;
	if (token != running->signal_token[depth])
		return -1;
	frame = running->active_user_frame;

	/* Puts the interrupted context back. */
	running->trap_windows = running->signal_windows[depth];
	*frame = running->signal_frame[depth];
	*return_value = (intptr_t)frame->out[0];
	running->active_pc = running->signal_pc[depth];
	running->active_next_pc = running->signal_next_pc[depth];
	running->active_tstate = running->signal_tstate[depth];
	set_user_trap(running->active_pc, running->active_next_pc, running->active_tstate);

	/* Closes the handler's level. */
	running->signal_token[depth] = 0;
	running->signal_depth = depth;

	/* Succeeded: the trap returns into the interrupted context. */
	return 0;
}

/*
 * Reports the number of tasks and the bytes of their kernel stacks.
 */
void
sparcv9_task_memory_stats(
	uint32_t *count,
	size_t *stack_bytes)
{
	bool enabled;

	/* Samples both counters together. */
	enabled = hal_irq_disable();

	if (count != NULL)
		*count = task_count;
	if (stack_bytes != NULL)
		*stack_bytes = task_stack_bytes;

	if (enabled)
		hal_irq_enable();
}

/* Appends a task to the list; interrupts are off. */
static void
list_add(
	struct sparcv9_task *task)
{
	struct sparcv9_task **link;

	/* Finds the list's end. */
	link = &task_list;
	while (*link != NULL)
		link = &(*link)->next;

	/* Links the task there. */
	*link = task;
	task->next = NULL;
	task_count++;
}

/* Takes a task off the list; interrupts are off. */
static void
list_remove(
	struct sparcv9_task *task)
{
	struct sparcv9_task **link;

	/* Finds the link that points at the task. */
	link = &task_list;
	while (*link != NULL && *link != task)
		link = &(*link)->next;

	/* A task not on the list is left alone. */
	if (*link == NULL)
		return;

	/* Unlinks the task. */
	*link = task->next;
	if (task_count != 0)
		task_count--;
}

/*
 * Builds the context the first switch to a new task loads.
 *
 * The switch returns into the entry through the out registers of window
 * zero, which are the ins of window one: the stack pointer and the return
 * address.  The entry reads the start function and its argument, or the
 * user stack, from window zero's first two locals.
 */
static void
build_context(
	struct sparcv9_task *task,
	void (*start)(void *),
	void *argument,
	void *user_stack)
{
	struct sparcv9_window_context *context;
	uintptr_t top;
	uintptr_t stack_pointer;

	/* The kernel stack's top and the biased stack pointer of the first frame. */
	top = ((uintptr_t)task->stack + SPARCV9_SYS_STACK_SIZE) & ~(uintptr_t)15U;
	stack_pointer = top - SPARCV9_STACK_BIAS - SPARCV9_STACK_FRAME_SIZE;

	/* Starts in window zero with every other window free. */
	context = &task->windows;
	hal_memset(context, 0, sizeof(*context));
	context->cwp = 0;
	context->cansave = TASK_INITIAL_CANSAVE;
	context->cleanwin = TASK_INITIAL_CLEANWIN;

	/* Gives the entry the start function and its argument, or the user stack. */
	context->window[0][TASK_WINDOW_LOCAL0] = (uintptr_t)start;
	context->window[0][TASK_WINDOW_LOCAL1] = (uintptr_t)user_stack;
	if (task->space == HAL_SPACE_SYS)
		context->window[0][TASK_WINDOW_LOCAL1] = (uintptr_t)argument;

	/* Returns into the kernel or the user entry with the first frame's stack. */
	context->window[1][TASK_WINDOW_IN6] = stack_pointer;
	context->window[1][TASK_WINDOW_IN7] = (uintptr_t)sparcv9_user_task_entry - 8U;
	if (task->space == HAL_SPACE_SYS)
		context->window[1][TASK_WINDOW_IN7] = (uintptr_t)sparcv9_kernel_task_entry - 8U;

	/* A trap from user mode switches to the stack's top. */
	task->trap_sp = top - SPARCV9_STACK_BIAS;
}

/*
 * Keeps the trap registers of a task switched out while it handles a trap
 * from user mode.
 */
static void
save_trap_state(
	struct sparcv9_task *task)
{
	/* Reads the trap level; a switch happens at level 0 or 1 only. */
	task->trap_level = sparcv9_read_trap_level();
	if (task->trap_level > 1)
		HAL_FATAL("SPARC V9 task switch above TL1");

	/* At level 1, keeps where the trap returns to. */
	if (task->trap_level != 0) {
		__asm__ volatile("rdpr %%tpc, %0" : "=r"(task->trap_pc));
		__asm__ volatile("rdpr %%tnpc, %0" : "=r"(task->trap_next_pc));
		__asm__ volatile("rdpr %%tstate, %0" : "=r"(task->trap_state));
		__asm__ volatile("rdpr %%tt, %0" : "=r"(task->trap_type));
	}
}

/* Puts back the trap registers a task was switched out with. */
static void
restore_trap_state(
	const struct sparcv9_task *task)
{
	/* Sets the trap level the task ran at. */
	__asm__ volatile("wrpr %0, 0, %%tl" : : "r"(task->trap_level) : "memory");

	/* At level 1, puts back where the trap returns to. */
	if (task->trap_level != 0) {
		__asm__ volatile("wrpr %0, 0, %%tpc" : : "r"(task->trap_pc));
		__asm__ volatile("wrpr %0, 0, %%tnpc" : : "r"(task->trap_next_pc));
		__asm__ volatile("wrpr %0, 0, %%tstate" : : "r"(task->trap_state));
		__asm__ volatile("wrpr %0, 0, %%tt" : : "r"(task->trap_type));
	}
}

/* Sets where the current trap returns to. */
static void
set_user_trap(
	uintptr_t pc,
	uintptr_t next_pc,
	uint64_t state)
{
	/* Writes the trap's pc, next pc and state together. */
	__asm__ volatile("wrpr %0, 0, %%tpc\n\twrpr %1, 0, %%tnpc\n\twrpr %2, 0, %%tstate" : : "r"(pc), "r"(next_pc), "r"(state) : "memory");
}

/*
 * Reports the saved window whose ins are the user's out registers at the
 * trap: the one after the window the trap was taken in.
 */
static unsigned
user_output_window(
	const struct sparcv9_window_context *context)
{
	/* Succeeded: the next window, wrapping. */
	return (unsigned)((context->cwp + 1U) & (SPARCV9_NWINDOWS - 1U));
}
