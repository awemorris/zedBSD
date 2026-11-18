/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 64-bit PowerPC tasks: the saved contexts the kernel's threads run in, and
 * the switch between them.
 *
 * A task's user context is the trap frame it entered the kernel with.  The
 * user's thread pointer is r2 of that frame, so a task's TLS is read and
 * written there; a task that has not yet entered user mode keeps it in the
 * task until it has a frame.
 */

#include <hal/hal.h>
#include "asm.h"
#include "defs.h"
#include "int.h"
#include "percpu.h"
#include "spin.h"
#include "task.h"

/*
 * The byte a kernel stack is filled with before its task first runs.  The
 * lowest byte that no longer holds it marks how deep the stack has grown,
 * because nothing else writes to a task's own stack.
 */
#define TASK_STACK_FILL_BYTE	0xa5U
#define TASK_STACK_FILL_WORD	0xa5a5a5a5U

/*
 * The lowest bytes of a kernel stack, which a task must never reach.  The
 * allocation's header lies just below the stack, so a stack that grows
 * past its bottom corrupts the allocator before anything else notices; a
 * task found to have written here stops the kernel instead.
 */
#define TASK_STACK_GUARD_BYTES	128U

/*
 * Every task the HAL has made, for the memory statistics.  The list and
 * the two counters beside it are kept under task_list_lock.
 */
static struct ppc64_task *task_list;
static uint32_t task_count;
static size_t task_stack_bytes;

/* Guards task_list, task_count and task_stack_bytes; zero when free. */
static volatile unsigned task_list_lock;

/*
 * The deepest kernel stack use measured so far, in bytes, over every task
 * destroyed since boot.  It only grows, by a compare-and-exchange, so each
 * new record is reported once; zero means no task has been measured.
 */
static size_t task_stack_high_water;

/*
 * The floating-point state a new task starts with: every register zero.
 * Never written; it is the zero of the image's BSS.
 */
static struct hal_fpregs initial_fpregs;

/*
 * The context self-test's own stack, the two saved stack pointers and the
 * turn counter.  They are used once, by the processor before the kernel
 * starts.
 */
static uint8_t self_stack[4096] __attribute__((aligned(16)));
static uintptr_t self_main_sp;
static uintptr_t self_task_sp;
static unsigned self_count;

static void task_add(struct ppc64_task *task);
static void task_del(struct ppc64_task *task);
static void build_kernel_stack(struct ppc64_task *task, void (*start)(void *), void *argument);
static struct ppc64_trap_frame *build_user_stack(struct ppc64_task *task);
static void reset_user_frame(struct ppc64_trap_frame *frame, uintptr_t entry, uintptr_t user_stack_pointer);
static struct ppc64_task *running_task(void);
static struct ppc64_trap_frame *running_user_frame(void);
static struct ppc64_trap_frame *task_frame(struct ppc64_task *task);
static int task_stack_guard_intact(const struct ppc64_task *task);
static size_t task_stack_depth(const struct ppc64_task *task);
static void task_stack_record_depth(const struct ppc64_task *task);
static void self_task(void);

/*
 * Wraps this processor's current context as its initial task.
 *
 * Returns NULL when the record cannot be allocated; the kernel decides
 * what to do about that.
 */
hal_task_t
hal_task_create_for_init_context(
	void)
{
	struct ppc64_percpu *cpu;
	struct ppc64_task *task;

	/* Refuses a second initial task on the same processor. */
	cpu = ppc64_percpu_current();
	if (cpu->running_task != NULL)
		HAL_FATAL("ppc64 bootstrap task established twice");

	/* Allocates the record; a failure is the kernel's to handle. */
	task = kernel_alloc(sizeof(*task));
	if (task == NULL)
		return NULL;

	/* Describes the context this processor is already running as a system task. */
	hal_memset(task, 0, sizeof(*task));
	task->space = HAL_SPACE_SYS;
	task->run_cpu = (int)cpu->logical_id;
	task->target_cpu = cpu->logical_id;
	task->fpregs = initial_fpregs;
	ppc64_fp_restore(&task->fpregs);

	/* Lists the task and makes it this processor's running task. */
	task_add(task);
	cpu->running_task = task;

	/* Succeeded: the processor's initial task. */
	return task;
}

/*
 * Creates a task that starts in a function (system) or at a user entry.
 */
hal_task_t
hal_task_create(
	hal_space_t space,
	void (*start)(void *),
	void *argument,
	void *user_stack_pointer)
{
	struct ppc64_trap_frame *frame;
	struct ppc64_task *task;
	int system;
	int has_stack;

	/* A task needs an entry. */
	if (start == NULL)
		return NULL;

	/* A system task has no user stack, and a user task must have one. */
	system = 0;
	if (space == HAL_SPACE_SYS)
		system = 1;
	has_stack = 0;
	if (user_stack_pointer != NULL)
		has_stack = 1;
	if (system == has_stack)
		return NULL;

	/* Allocates the record. */
	task = kernel_alloc(sizeof(*task));
	if (task == NULL)
		return NULL;
	hal_memset(task, 0, sizeof(*task));

	/* Allocates the kernel stack, aligned to 16 bytes inside a larger block. */
	task->sys_stack_allocation = kernel_alloc(PPC64_SYS_STACK_SIZE + 15U);
	if (task->sys_stack_allocation == NULL) {
		kernel_free(task);
		return NULL;
	}

	/* Aligns the stack, and fills it so that its guard and its deepest use can be read later. */
	task->sys_stack = (void *)(((uintptr_t)task->sys_stack_allocation + 15U) & ~(uintptr_t)15U);
	task->sys_stack_top = (uintptr_t)task->sys_stack + PPC64_SYS_STACK_SIZE;
	hal_memset(task->sys_stack, (int)TASK_STACK_FILL_BYTE, PPC64_SYS_STACK_SIZE);

	/* A new task runs nowhere and is resumed on the processor unless moved. */
	task->space = space;
	task->run_cpu = -1;
	task->target_cpu = 0;
	task->fpregs = initial_fpregs;

	/* Prepares the stack the first switch to the task pops. */
	if (system) {
		build_kernel_stack(task, start, argument);
	} else {
		frame = build_user_stack(task);
		reset_user_frame(frame, (uintptr_t)start, (uintptr_t)user_stack_pointer);
	}

	/* Lists the task and counts its stack. */
	task_add(task);

	/* Succeeded: the task is ready to be switched to. */
	return task;
}

/*
 * Records the trap frame the running task entered the kernel with.
 */
void
ppc64_task_enter_user_frame(
	struct ppc64_trap_frame *frame)
{
	struct ppc64_task *task;

	/* The frame belongs to whatever task this processor runs. */
	task = running_task();
	if (task != NULL)
		task->active_user_frame = frame;
}

/*
 * Forgets the running task's trap frame as it returns to user.
 */
void
ppc64_task_leave_user_frame(
	void)
{
	struct ppc64_task *task;

	/* The frame belongs to whatever task this processor runs. */
	task = running_task();
	if (task != NULL)
		task->active_user_frame = NULL;
}

/*
 * Marks a new user task's first frame as used: from here on the task's
 * user context is the frame of its traps.
 */
void
ppc64_task_first_user_entry(
	void)
{
	struct ppc64_task *task;

	/* The running task is the one entering user mode for the first time. */
	task = running_task();
	if (task != NULL)
		task->initial_user_frame = NULL;
}

/*
 * Duplicates the running task's return-to-user context into a new task.
 *
 * The child resumes from a copy of the same frame, with its own system
 * call result, its own copy of the floating-point state and the same
 * thread pointer.
 */
hal_task_t
hal_task_fork_current(
	hal_space_t child_space,
	intptr_t child_result)
{
	struct ppc64_trap_frame *source;
	struct ppc64_trap_frame *copy;
	struct ppc64_task *parent;
	struct ppc64_task *child;

	/* Only a user task inside a system call has a frame to duplicate. */
	parent = running_task();
	if (parent == NULL)
		return NULL;
	if (child_space == HAL_SPACE_SYS)
		return NULL;
	source = parent->active_user_frame;
	if (source == NULL)
		return NULL;

	/* Makes the child as a user task, then replaces its first frame with a copy of the parent's. */
	child = hal_task_create(
		child_space,
		(void (*)(void *))(uintptr_t)source->pc,
		NULL,
		(void *)(uintptr_t)source->r[1]);
	if (child == NULL)
		return NULL;
	copy = child->initial_user_frame;
	*copy = *source;
	copy->r[3] = (uint64_t)child_result;

	/* The child inherits the floating-point state as it is now. */
	ppc64_fp_save(&parent->fpregs);
	child->fpregs = parent->fpregs;

	/* Succeeded: the child is ready to be switched to. */
	return child;
}

/*
 * Checks that the running task may replace its program.
 */
int
hal_task_exec_validate(
	hal_space_t new_space,
	uintptr_t entry,
	uintptr_t user_stack_pointer)
{
	struct ppc64_task *task;

	/* Only a user task inside a system call can exec. */
	task = running_task();
	if (task == NULL)
		return -1;
	if (new_space == HAL_SPACE_SYS)
		return -1;
	if (task->active_user_frame == NULL)
		return -1;

	/* The new program needs an entry and a stack. */
	if (entry == 0)
		return -1;
	if (user_stack_pointer == 0)
		return -1;

	/* Succeeded: the exec may proceed. */
	return 0;
}

/*
 * Replaces the running task's program: a fresh frame, space and state.
 */
int
hal_task_exec_current(
	hal_space_t new_space,
	uintptr_t entry,
	uintptr_t user_stack_pointer)
{
	struct ppc64_task *task;
	bool enabled;
	int valid;

	/* Refuses what hal_task_exec_validate() refuses. */
	valid = hal_task_exec_validate(new_space, entry, user_stack_pointer);
	if (valid != 0)
		return -1;

	/* Returns to user at the new entry with every register cleared, the thread pointer too. */
	task = running_task();
	reset_user_frame(task->active_user_frame, entry, user_stack_pointer);

	/*
	 * The new program starts without the old one's signals or floating
	 * point, in the new space.  The floating-point registers live in the
	 * processor while the task runs and a task switch saves them over the
	 * task's copy, so both are replaced with interrupts off.
	 */
	enabled = ppc64_irq_save();

	task->space = new_space;
	task->tls = 0;
	task->signal_depth = 0;
	task->fpregs = initial_fpregs;
	ppc64_fp_restore(&task->fpregs);
	hal_space_switch(new_space);

	ppc64_irq_restore(enabled);

	/* Succeeded: the return to user enters the new program. */
	return 0;
}

/*
 * Reports the running task's user stack pointer, or 0 outside a user entry.
 */
uintptr_t
hal_task_get_user_stack(
	void)
{
	struct ppc64_trap_frame *frame;

	/* Only a task inside the kernel from user has a user stack to report. */
	frame = running_user_frame();
	if (frame == NULL)
		return 0;

	/* Succeeded: the stack the task returns to. */
	return (uintptr_t)frame->r[1];
}

/*
 * Reports the active return-to-user context.
 */
int
hal_task_get_user_context(
	uintptr_t *pc,
	uintptr_t *stack_pointer,
	intptr_t *return_value)
{
	struct ppc64_trap_frame *frame;

	/* Requires a frame that entered from user. */
	frame = running_user_frame();
	if (frame == NULL)
		return -1;

	/* Reports each value the caller asked for. */
	if (pc != NULL)
		*pc = (uintptr_t)frame->pc;
	if (stack_pointer != NULL)
		*stack_pointer = (uintptr_t)frame->r[1];
	if (return_value != NULL)
		*return_value = (intptr_t)frame->r[3];

	/* Succeeded: every value asked for is filled. */
	return 0;
}

/*
 * Makes the running task's return to user enter a signal handler.
 *
 * The interrupted frame and floating-point state are kept under the token,
 * which hal_task_signal_return() must show.  The handler returns to the
 * restorer through the link register.
 */
int
hal_task_signal_enter(
	uintptr_t handler,
	uintptr_t stack,
	int signal_number,
	uintptr_t information,
	uintptr_t context,
	uintptr_t restorer,
	uint32_t token)
{
	struct ppc64_trap_frame *frame;
	struct ppc64_task *task;
	unsigned depth;

	/* Requires a user frame and room for one more nested handler. */
	frame = running_user_frame();
	if (frame == NULL)
		return -1;
	task = running_task();
	if (task->signal_depth >= HAL_SIGNAL_NEST_MAX)
		return -1;

	/* Requires a handler, a stack and a token to return with. */
	if (handler == 0)
		return -1;
	if (stack == 0)
		return -1;
	if (token == 0)
		return -1;

	/* Keeps the interrupted context under the token. */
	depth = task->signal_depth;
	task->signal_frame[depth] = *frame;
	ppc64_fp_save(&task->signal_fpregs[depth]);
	task->signal_token[depth] = token;
	task->signal_depth = depth + 1U;

	/* Enters the handler with its three arguments and the restorer as the return. */
	frame->pc = handler;
	frame->r[1] = stack;
	frame->r[3] = (uint64_t)(int64_t)signal_number;
	frame->r[4] = information;
	frame->r[5] = context;
	frame->lr = restorer;

	/* Succeeded: the return to user enters the handler. */
	return 0;
}

/*
 * Puts back the context a signal handler interrupted.
 */
int
hal_task_signal_return(
	uint32_t token,
	intptr_t *value)
{
	struct ppc64_trap_frame *frame;
	struct ppc64_task *task;
	unsigned depth;

	/* Requires a user frame, somewhere to report and a handler to leave. */
	frame = running_user_frame();
	if (frame == NULL)
		return -1;
	if (value == NULL)
		return -1;
	task = running_task();
	if (task->signal_depth == 0)
		return -1;
	if (token == 0)
		return -1;

	/* Refuses a token that is not the innermost handler's. */
	depth = task->signal_depth - 1U;
	if (token != task->signal_token[depth])
		return -1;

	/* Restores the interrupted frame and floating-point state. */
	*frame = task->signal_frame[depth];
	*value = (intptr_t)frame->r[3];
	ppc64_fp_restore(&task->signal_fpregs[depth]);
	task->signal_token[depth] = 0;
	task->signal_depth = depth;

	/* Succeeded: the return to user resumes the interrupted code. */
	return 0;
}

/*
 * Destroys a task that runs on no processor.
 */
void
hal_task_destroy(
	hal_task_t handle)
{
	struct ppc64_task *task;
	int run_cpu;

	/* A missing task is nothing to destroy. */
	task = handle;
	if (task == NULL)
		return;

	/* Refuses a task whose stack a processor still uses. */
	run_cpu = __atomic_load_n(&task->run_cpu, __ATOMIC_ACQUIRE);
	if (run_cpu >= 0)
		HAL_FATAL("destroy current ppc64 task");

	/* Unlists the task, then measures and frees its stack, then frees its record. */
	task_del(task);
	if (task->sys_stack != NULL) {
		task_stack_record_depth(task);
		kernel_free(task->sys_stack_allocation);
	}

	/* Frees the record last. */
	kernel_free(task);
}

/*
 * Switches this processor from the running task to another one.
 *
 * The departing task stays marked as running until the arriving context
 * has left its stack, so that another processor cannot resume a task whose
 * stack is still being pushed to.
 */
void
hal_task_context_switch(
	hal_task_t handle)
{
	struct ppc64_percpu *cpu;
	struct ppc64_task *to;
	struct ppc64_task *from;
	hal_cpu_id_t target_cpu;
	int run_cpu;
	int guard_intact;

	/* Resolves both tasks. */
	cpu = ppc64_percpu_current();
	to = handle;
	from = cpu->running_task;
	if (to == NULL || from == NULL)
		HAL_FATAL("invalid ppc64 task switch");

	/* A switch to the running task changes nothing. */
	if (to == from)
		return;

	/* Stops at a departing task that has grown into the bottom of its kernel stack. */
	if (from->sys_stack != NULL) {
		guard_intact = task_stack_guard_intact(from);
		if (!guard_intact)
			HAL_FATAL("ppc64 kernel stack overflow");
	}

	/* Requires a destination that runs nowhere. */
	run_cpu = __atomic_load_n(&to->run_cpu, __ATOMIC_ACQUIRE);
	if (run_cpu >= 0)
		HAL_FATAL("ppc64 HAL task already running");

	/* Requires a destination meant for this processor. */
	target_cpu = __atomic_load_n(&to->target_cpu, __ATOMIC_ACQUIRE);
	if (target_cpu != cpu->logical_id)
		HAL_FATAL("ppc64 HAL task resumed on wrong CPU");

	/*
	 * Records the departing task for ppc64_task_finish_switch(), which
	 * the arriving context runs once it is off the departing task's stack.
	 */
	cpu->switching_from = from;
	__atomic_store_n(&to->run_cpu, (int)cpu->logical_id, __ATOMIC_RELEASE);

	/* Keeps the departing task's floating-point state and installs the arriving one's. */
	ppc64_fp_save(&from->fpregs);
	cpu->running_task = to;
	hal_space_switch(to->space);
	ppc64_fp_restore(&to->fpregs);

	/* A trap from the arriving task's user code starts at the top of its kernel stack. */
	__asm__ volatile("mtsprg0 %0" : : "r"(to->sys_stack_top) : "memory");

	/* Switches the stack and the callee-saved registers. */
	ppc64_task_dispatch(&from->resume_sp, &to->resume_sp);

	/* Resumed here by a later switch; releases the task that made it. */
	ppc64_task_finish_switch();
}

/*
 * Marks the task this processor just switched away from as no longer running.
 *
 * Every context that starts running on a processor calls this first, whether it
 * resumed in hal_task_context_switch() or entered through one of the new
 * task entries.  Only after this may hal_task_transfer() move that task to
 * another processor.
 */
void
ppc64_task_finish_switch(
	void)
{
	struct ppc64_percpu *cpu;
	struct ppc64_task *departed;

	/* Takes the departing task recorded by hal_task_context_switch(). */
	cpu = ppc64_percpu_current();
	departed = cpu->switching_from;
	cpu->switching_from = NULL;

	/* Publishes that the departed task's stack is free to resume elsewhere. */
	if (departed != NULL)
		__atomic_store_n(&departed->run_cpu, -1, __ATOMIC_RELEASE);
}

/*
 * Stops a system task that returned from its start function.
 */
void
ppc64_task_returned(
	void)
{
	/* A system task must never return. */
	HAL_FATAL("ppc64 task returned");
	ppc64_halt_forever();
}

/*
 * Reports the task running on the current processor.
 */
hal_task_t
hal_task_get_current(
	void)
{
	struct ppc64_task *task;

	/* The processor's record names its running task. */
	task = running_task();

	/* Succeeded: the running task. */
	return task;
}

/*
 * Reports the task running on the current processor, for the rest of the HAL.
 */
struct ppc64_task *
ppc64_task_running(
	void)
{
	struct ppc64_task *task;

	/* The processor's record names its running task. */
	task = running_task();

	/* Succeeded: the running task. */
	return task;
}

/*
 * Sets a task's user thread pointer (r2), in the frame it returns to user
 * with, and keeps it for a task that has no frame yet.
 */
void
hal_task_set_tls(
	hal_task_t handle,
	uintptr_t value)
{
	struct ppc64_trap_frame *frame;
	struct ppc64_task *task;
	bool enabled;

	/* A missing task has no TLS. */
	task = handle;
	if (task == NULL)
		return;

	/* Keeps the value and writes it where the next return to user reads it. */
	enabled = ppc64_irq_save();
	task->tls = value;
	frame = task_frame(task);
	if (frame != NULL)
		frame->r[2] = value;
	ppc64_irq_restore(enabled);
}

/*
 * Reports a task's user thread pointer as it returns to user with it.
 */
uintptr_t
hal_task_get_tls(
	hal_task_t handle)
{
	struct ppc64_trap_frame *frame;
	struct ppc64_task *task;
	uintptr_t value;
	bool enabled;

	/* A missing task has no TLS. */
	task = handle;
	if (task == NULL)
		return 0;

	/* The frame's register, or the kept value of a task with no frame. */
	enabled = ppc64_irq_save();
	value = task->tls;
	frame = task_frame(task);
	if (frame != NULL)
		value = frame->r[2];
	ppc64_irq_restore(enabled);

	/* Succeeded: the thread pointer. */
	return value;
}

/*
 * Stores the kernel's link from a task to its owner.
 */
void
hal_task_set_private(
	hal_task_t handle,
	void *private_data)
{
	struct ppc64_task *task;

	/* Stores the link on a task that exists. */
	task = handle;
	if (task != NULL)
		task->private_data = private_data;
}

/*
 * Reports the kernel's link from a task to its owner.
 */
void *
hal_task_get_private(
	hal_task_t handle)
{
	struct ppc64_task *task;

	/* A missing task has no owner. */
	task = handle;
	if (task == NULL)
		return NULL;

	/* Succeeded: the stored link. */
	return task->private_data;
}

/*
 * Reports the address space a task runs in.
 */
hal_space_t
hal_task_get_space(
	hal_task_t handle)
{
	struct ppc64_task *task;

	/* A missing task is taken as a system task. */
	task = handle;
	if (task == NULL)
		return HAL_SPACE_SYS;

	/* Succeeded: the task's space. */
	return task->space;
}

/*
 * Moves a task that runs nowhere to another processor, where it is resumed next.
 */
int
hal_task_transfer(
	hal_task_t handle,
	hal_cpu_id_t target_cpu)
{
	struct ppc64_task *task;
	struct hal_cpu_mask ready;
	unsigned count;
	int run_cpu;
	int target_ready;

	/* Requires a task and a processor the kernel knows. */
	task = handle;
	if (task == NULL)
		return HAL_ERR_INVALID;
	count = hal_cpu_count();
	if (target_cpu >= count)
		return HAL_ERR_INVALID;

	/* Refuses a task whose stack a processor still uses. */
	run_cpu = __atomic_load_n(&task->run_cpu, __ATOMIC_ACQUIRE);
	if (run_cpu >= 0)
		return HAL_ERR_BUSY;

	/* Requires the target processor to be up. */
	hal_cpu_ready_mask(&ready);
	target_ready = hal_cpu_mask_test(&ready, target_cpu);
	if (!target_ready)
		return HAL_ERR_STATE;

	/* Publishes the destination before another processor can resume the task. */
	__atomic_store_n(&task->target_cpu, target_cpu, __ATOMIC_RELEASE);

	/* Succeeded: the task is resumed on the target processor next. */
	return HAL_OK;
}

/*
 * Reports a task's user frame for the debugger: the one it entered the
 * kernel with, or the one it will first return through; NULL for a task
 * with neither.
 */
struct ppc64_trap_frame *
ppc64_task_user_frame(
	struct ppc64_task *task)
{
	struct ppc64_trap_frame *frame;

	/* The same choice as the thread pointer's. */
	frame = task_frame(task);

	/* Succeeded: the frame, or NULL. */
	return frame;
}

/*
 * Reports how many tasks exist and how many kernel-stack bytes they hold.
 */
void
ppc64_task_memory_stats(
	uint32_t *count,
	size_t *bytes)
{
	bool enabled;

	/* Samples both counters together, for whichever the caller asked. */
	enabled = ppc64_spin_lock(&task_list_lock);

	if (count != NULL)
		*count = task_count;
	if (bytes != NULL)
		*bytes = task_stack_bytes;

	ppc64_spin_unlock(&task_list_lock, enabled);
}

/*
 * Proves that ppc64_task_dispatch() switches stacks both ways, before the
 * kernel relies on it.
 */
void
ppc64_context_selftest(
	void)
{
	unsigned long *stack_pointer;

	/* Builds a stack that dispatches into self_task(). */
	stack_pointer = (unsigned long *)(self_stack + sizeof(self_stack));
	stack_pointer -= PPC64_DISPATCH_WORDS;
	hal_memset(stack_pointer, 0, PPC64_DISPATCH_WORDS * sizeof(*stack_pointer));
	stack_pointer[PPC64_DISPATCH_LR] = (uintptr_t)self_task;
	self_task_sp = (uintptr_t)stack_pointer;

	/* The first switch runs the task to its first yield. */
	ppc64_task_dispatch(&self_main_sp, &self_task_sp);
	if (self_count != 1)
		HAL_FATAL("ppc64 context selftest first switch");

	/* The second switch resumes it from that yield. */
	ppc64_task_dispatch(&self_main_sp, &self_task_sp);
	if (self_count != 2)
		HAL_FATAL("ppc64 context selftest second switch");

	hal_puts("PPC64 CONTEXT PASS\n");
}

/* Lists a new task and counts its kernel stack. */
static void
task_add(
	struct ppc64_task *task)
{
	struct ppc64_task **link;
	bool enabled;

	/* Appends the task at the end of the list. */
	enabled = ppc64_spin_lock(&task_list_lock);

	link = &task_list;
	while (*link != NULL)
		link = &(*link)->next;
	*link = task;
	task->next = NULL;
	task_count++;
	if (task->sys_stack != NULL)
		task_stack_bytes += PPC64_SYS_STACK_SIZE;

	ppc64_spin_unlock(&task_list_lock, enabled);
}

/* Unlists a task and stops counting its kernel stack. */
static void
task_del(
	struct ppc64_task *task)
{
	struct ppc64_task **link;
	bool enabled;

	/* Finds the task's link and takes it out, if it is listed. */
	enabled = ppc64_spin_lock(&task_list_lock);

	link = &task_list;
	while (*link != NULL && *link != task)
		link = &(*link)->next;
	if (*link != NULL) {
		*link = task->next;
		if (task_count != 0)
			task_count--;
		if (task->sys_stack != NULL)
			task_stack_bytes -= PPC64_SYS_STACK_SIZE;
	}

	ppc64_spin_unlock(&task_list_lock, enabled);
}

/*
 * Builds the stack the first switch to a system task pops: s0 is the
 * function, s1 its argument, and ra the entry that calls them.
 */
static void
build_kernel_stack(
	struct ppc64_task *task,
	void (*start)(void *),
	void *argument)
{
	unsigned long *stack_pointer;

	/* Reserves the words ppc64_task_dispatch() pops. */
	stack_pointer = (unsigned long *)task->sys_stack_top;
	stack_pointer -= PPC64_DISPATCH_WORDS;
	hal_memset(stack_pointer, 0, PPC64_DISPATCH_WORDS * sizeof(*stack_pointer));

	/* The function, its argument and the entry the dispatch returns into. */
	stack_pointer[PPC64_DISPATCH_R14] = (uintptr_t)start;
	stack_pointer[PPC64_DISPATCH_R15] = (uintptr_t)argument;
	stack_pointer[PPC64_DISPATCH_LR] = (uintptr_t)ppc64_kernel_task_entry;
	task->resume_sp = (uintptr_t)stack_pointer;
}

/*
 * Builds the stack the first switch to a user task pops: a user frame at
 * the top, and below it the dispatch words returning into the entry that
 * goes to user through that frame.  Reports the frame.
 */
static struct ppc64_trap_frame *
build_user_stack(
	struct ppc64_task *task)
{
	struct ppc64_trap_frame *frame;
	unsigned long *stack_pointer;

	/* The frame at the top of the stack. */
	frame = (struct ppc64_trap_frame *)(task->sys_stack_top - sizeof(*frame));
	hal_memset(frame, 0, sizeof(*frame));

	/* The call area below it, then the dispatch words. */
	stack_pointer = (unsigned long *)((uintptr_t)frame - PPC64_FRAME_CALL);
	stack_pointer -= PPC64_DISPATCH_WORDS;
	hal_memset(stack_pointer, 0, (PPC64_DISPATCH_WORDS * sizeof(*stack_pointer)) + PPC64_FRAME_CALL);
	stack_pointer[PPC64_DISPATCH_LR] = (uintptr_t)ppc64_user_frame_entry;
	task->resume_sp = (uintptr_t)stack_pointer;

	/* Succeeded: the frame the task first returns through. */
	task->initial_user_frame = frame;
	return frame;
}

/*
 * Fills a user frame for a program's first instruction: every register
 * zero but the stack pointer, user mode with interrupts on after the
 * return, and the floating-point unit on and clean.
 */
static void
reset_user_frame(
	struct ppc64_trap_frame *frame,
	uintptr_t entry,
	uintptr_t user_stack_pointer)
{
	/* Clears every register. */
	hal_memset(frame, 0, sizeof(*frame));

	/* The entry, the stack, and the user's machine state. */
	frame->pc = entry;
	frame->r[1] = user_stack_pointer;
	frame->msr = PPC64_MSR_USER;
}

/* Reports the task the current processor runs, which is the caller's. */
static struct ppc64_task *
running_task(
	void)
{
	struct ppc64_percpu *cpu;
	struct ppc64_task *task;
	bool enabled;

	/*
	 * Reads the processor's record and its running task with interrupts off.
	 * The thread pointer names the processor, not the task; a caller preempted
	 * between the two reads and resumed on another processor would read the old
	 * processor's task, another thread's.
	 */
	enabled = ppc64_irq_save();
	cpu = ppc64_percpu_current();
	task = cpu->running_task;
	ppc64_irq_restore(enabled);

	/* Succeeded: the running task, or NULL before the processor's initial task. */
	return task;
}

/* Reports the running task's user frame, or NULL outside a user entry. */
static struct ppc64_trap_frame *
running_user_frame(
	void)
{
	struct ppc64_task *task;

	/* A processor without a task has no frame. */
	task = running_task();
	if (task == NULL)
		return NULL;

	/* Succeeded: the frame, or NULL when the task is not inside a user entry. */
	return task->active_user_frame;
}

/* Reports the frame a task returns to user through next, or NULL. */
static struct ppc64_trap_frame *
task_frame(
	struct ppc64_task *task)
{
	/* A task inside a user entry returns through the frame it entered with. */
	if (task->active_user_frame != NULL)
		return task->active_user_frame;

	/* A task that never ran returns through the frame it was built with, or has none. */
	return task->initial_user_frame;
}

/* Reports whether a kernel stack's lowest bytes still hold their fill. */
static int
task_stack_guard_intact(
	const struct ppc64_task *task)
{
	const uint32_t *word;
	size_t index;

	/* Compares the guard a word at a time, from the bottom of the stack. */
	word = task->sys_stack;
	for (index = 0; index < TASK_STACK_GUARD_BYTES / sizeof(*word); index++) {
		if (word[index] != TASK_STACK_FILL_WORD)
			return 0;
	}

	/* Succeeded: the task has never reached its guard. */
	return 1;
}

/* Measures how many bytes of its kernel stack a task has ever used. */
static size_t
task_stack_depth(
	const struct ppc64_task *task)
{
	const uint32_t *word;
	size_t count;
	size_t index;

	/* Finds the lowest word that the task's own frames have overwritten. */
	word = task->sys_stack;
	count = PPC64_SYS_STACK_SIZE / sizeof(*word);
	index = 0;
	while (index < count && word[index] == TASK_STACK_FILL_WORD)
		index++;

	/* Reports the distance from that word to the top of the stack. */
	return (count - index) * sizeof(*word);
}

/*
 * Checks a dead task's kernel stack and reports a new deepest use past
 * half of the stack.
 */
static void
task_stack_record_depth(
	const struct ppc64_task *task)
{
	size_t depth;
	size_t previous;
	int guard_intact;
	int raised;

	/* Stops at a stack that was overrun after its last switch away. */
	guard_intact = task_stack_guard_intact(task);
	if (!guard_intact)
		HAL_FATAL("ppc64 kernel stack overflow");

	/* Raises the boot-wide record when this stack went deeper than any before. */
	depth = task_stack_depth(task);
	previous = __atomic_load_n(&task_stack_high_water, __ATOMIC_RELAXED);
	raised = 0;
	while (depth > previous && !raised) {
		raised = __atomic_compare_exchange_n(
			&task_stack_high_water,
			&previous,
			depth,
			0,
			__ATOMIC_RELAXED,
			__ATOMIC_RELAXED);
	}

	/* Reports a new record deep enough to matter. */
	if (raised && depth > PPC64_SYS_STACK_SIZE / 2U) {
		hal_printf(
			"ppc64: kernel stack high water %lu of %lu bytes\n",
			(unsigned long)depth,
			(unsigned long)PPC64_SYS_STACK_SIZE);
	}
}

/* Runs the self-test's task: yields back twice, counting each turn. */
static void
self_task(
	void)
{
	/* The first turn. */
	self_count = 1;
	ppc64_task_dispatch(&self_task_sp, &self_main_sp);

	/* The second turn. */
	self_count = 2;
	ppc64_task_dispatch(&self_task_sp, &self_main_sp);

	/* The test never resumes the task a third time. */
	ppc64_halt_forever();
}
