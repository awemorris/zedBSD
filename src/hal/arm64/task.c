/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The arm64 tasks: the saved contexts the kernel's threads run in, and the
 * switch between them on each CPU.
 */

#include <hal/hal.h>
#include "asm.h"
#include "task.h"
#include "int.h"
#include "percpu.h"
#include "spin.h"
#include "watch.h"

/* The bytes of one saved floating-point state (Q0-Q31, FPCR, FPSR). */
#define ARM64_FPREGS_SIZE 528U

/* The words asm_task_dispatch() pops from a stack it resumes: x19-x30. */
#define ARM64_DISPATCH_WORDS 12U

/*
 * The byte a kernel stack is filled with before its task first runs.  The
 * lowest byte that no longer holds it marks how deep the stack has grown,
 * because nothing else writes to a task's own stack.
 */
#define ARM64_STACK_FILL_BYTE 0xa5U
#define ARM64_STACK_FILL_WORD 0xa5a5a5a5a5a5a5a5ULL

/*
 * The lowest bytes of a kernel stack, which a task must never reach.  The
 * page-backed allocation's header lies just below the stack, so a stack
 * that grows past its bottom corrupts the allocator before anything else
 * notices; a task found to have written here stops the kernel instead
 * (BUG-321).
 */
#define ARM64_STACK_GUARD_BYTES 128U

/*
 * Every task the HAL has made, for the memory statistics.
 *
 * The list and the two counters beside it are kept under task_list_lock.
 */
static struct arm64_task *task_list;
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
 * The floating-point state a new task starts with.
 *
 * The boot CPU captures it once, from the state the firmware left, before
 * any task is made; every task made later starts from a copy.
 */
static uint8_t initial_fpregs[ARM64_FPREGS_SIZE] __attribute__((aligned(16)));

/*
 * The context self-test's own stack, the two saved stack pointers and the
 * turn counter.  They are used once, by the boot CPU before the kernel
 * starts.
 */
static uint8_t self_stack[4096] __attribute__((aligned(16)));
static uintptr_t self_main_sp;
static uintptr_t self_task_sp;
static unsigned self_count;

static void task_add(struct arm64_task *task);
static void task_del(struct arm64_task *task);
static void build_stack(struct arm64_task *task, void (*start)(void *), void *argument, void *user_stack_pointer);
static struct arm64_task *running_task(void);
static int task_stack_guard_intact(const struct arm64_task *task);
static size_t task_stack_depth(const struct arm64_task *task);
static void task_stack_record_depth(const struct arm64_task *task);
static struct arm64_exception_frame *running_user_frame(void);
static void self_task(void);

/*
 * Wraps this CPU's current context as its initial task.
 *
 * Returns NULL when the record cannot be allocated; the kernel decides
 * what to do about that rather than the HAL panicking on its behalf.
 */
hal_task_t
hal_task_create_for_init_context(
	void)
{
	struct arm64_percpu *cpu;
	struct arm64_task *task;

	/* Refuses a second initial task on the same CPU. */
	cpu = arm64_percpu_current();
	if (cpu->running_task != NULL)
		HAL_FATAL("bootstrap task established twice");

	/* The boot CPU captures the floating-point state new tasks start with. */
	if (cpu->logical_id == 0)
		arm64_fp_save(initial_fpregs);

	/* Allocates the record; a failure is the kernel's to handle. */
	task = kernel_alloc(sizeof(*task));
	if (task == NULL)
		return NULL;

	/* Describes the context this CPU is already running as a system task. */
	hal_memset(task, 0, sizeof(*task));
	task->space = HAL_SPACE_SYS;
	task->run_cpu = (int)cpu->logical_id;
	task->target_cpu = cpu->logical_id;
	hal_memcpy(task->fpregs, initial_fpregs, sizeof(initial_fpregs));

	/* Lists the task and makes it this CPU's running task. */
	task_add(task);
	cpu->running_task = task;

	/* Succeeded: the CPU's initial task. */
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
	struct arm64_task *task;
	int system;
	int has_stack;

	/* A task needs an entry. */
	if (start == NULL)
		return NULL;

	/* A system task has no user stack, and a user task must have one. */
	system = space == HAL_SPACE_SYS;
	has_stack = user_stack_pointer != NULL;
	if (system == has_stack)
		return NULL;

	/* Allocates the record. */
	task = kernel_alloc(sizeof(*task));
	if (task == NULL)
		return NULL;

	hal_memset(task, 0, sizeof(*task));

	/* Allocates the kernel stack, aligned to 16 bytes inside a larger block. */
	task->sys_stack_allocation = kernel_alloc(ARM64_SYS_STACK_SIZE + 15U);
	if (task->sys_stack_allocation == NULL) {
		kernel_free(task);
		return NULL;
	}

	/* Aligns the stack, and fills it so that its guard and its deepest use can be read later. */
	task->sys_stack = (void *)(((uintptr_t)task->sys_stack_allocation + 15U) & ~(uintptr_t)15U);
	hal_memset(task->sys_stack, (int)ARM64_STACK_FILL_BYTE, ARM64_SYS_STACK_SIZE);

	/* A new task runs nowhere and is resumed on the boot CPU unless moved. */
	task->space = space;
	task->run_cpu = -1;
	task->target_cpu = 0;
	hal_memcpy(task->fpregs, initial_fpregs, sizeof(initial_fpregs));

	/* Prepares the stack the first switch to the task pops. */
	build_stack(task, start, argument, user_stack_pointer);

	/* Lists the task and counts its stack. */
	task_add(task);

	/* Succeeded: the task is ready to be switched to. */
	return task;
}

/*
 * Records the exception frame the running task entered the kernel with.
 */
void
arm64_task_enter_user_frame(
	void *frame)
{
	struct arm64_task *task;

	/* The frame belongs to whatever task this CPU runs. */
	task = running_task();
	if (task != NULL)
		task->active_user_frame = frame;
}

/*
 * Forgets the running task's exception frame as it returns to user.
 */
void
arm64_task_leave_user_frame(
	void)
{
	struct arm64_task *task;

	/* The frame belongs to whatever task this CPU runs. */
	task = running_task();
	if (task != NULL)
		task->active_user_frame = NULL;
}

/*
 * Duplicates the running task's return-to-user context into a new task.
 *
 * The child resumes from the same user frame, with its own system call
 * result, its own copy of the floating-point state and the same TLS.
 */
hal_task_t
hal_task_fork_current(
	hal_space_t child_space,
	intptr_t child_result)
{
	struct arm64_exception_frame *source;
	struct arm64_exception_frame *copy;
	struct arm64_task *parent;
	struct arm64_task *child;
	uint64_t *resume;

	/* Only a user task inside a system call has a frame to duplicate. */
	parent = running_task();
	if (parent == NULL)
		return NULL;
	if (child_space == HAL_SPACE_SYS)
		return NULL;
	source = parent->active_user_frame;
	if (source == NULL)
		return NULL;

	/* Makes the child as a user task at the parent's return address. */
	child = hal_task_create(
		child_space,
		(void (*)(void *))(uintptr_t)source->elr,
		NULL,
		(void *)(uintptr_t)source->user_sp);
	if (child == NULL)
		return NULL;

	/*
	 * Replaces the child's first stack with one that returns to user
	 * through a copy of the parent's frame.
	 */
	resume = (uint64_t *)((uintptr_t)child->sys_stack + ARM64_SYS_STACK_SIZE -
			      sizeof(*source) - ARM64_DISPATCH_WORDS * sizeof(uint64_t));
	hal_memset(resume, 0, ARM64_DISPATCH_WORDS * sizeof(uint64_t));
	resume[ARM64_DISPATCH_WORDS - 1U] = (uintptr_t)arm64_user_frame_entry;
	copy = (struct arm64_exception_frame *)(resume + ARM64_DISPATCH_WORDS);
	*copy = *source;
	copy->x[0] = (uint64_t)child_result;
	child->resume_sp = (uintptr_t)resume;

	/* The child inherits the TLS and the floating-point state as they are now. */
	child->tls = parent->tls;
	arm64_fp_save(parent->fpregs);
	hal_memcpy(child->fpregs, parent->fpregs, sizeof(child->fpregs));

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
	struct arm64_task *task;

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
	struct arm64_exception_frame *frame;
	struct arm64_task *task;
	uint64_t irq;
	int valid;

	/* Refuses what hal_task_exec_validate() refuses. */
	valid = hal_task_exec_validate(new_space, entry, user_stack_pointer);
	if (valid != 0)
		return -1;

	/* Returns to user at the new entry with every register cleared. */
	task = running_task();
	frame = task->active_user_frame;
	hal_memset(frame, 0, sizeof(*frame));
	frame->elr = (uint64_t)entry;
	frame->user_sp = (uint64_t)user_stack_pointer;

	/*
	 * The new program starts without the old one's TLS, signals or
	 * floating point, in the new space.  While the task runs, its TLS and
	 * floating-point registers live in the CPU and a task switch saves
	 * them over the task's copies, so the copies and the registers are
	 * replaced with interrupts off: a switch between the two would save
	 * the old program's registers over the fresh copy, and the new
	 * program would start with the old values.
	 */
	irq = arm64_irq_save();

	task->space = new_space;
	task->tls = 0;
	task->signal_depth = 0;
	hal_memcpy(task->fpregs, initial_fpregs, sizeof(initial_fpregs));
	arm64_fp_restore(task->fpregs);
	__asm__ volatile("msr tpidr_el0, xzr" : : : "memory");
	hal_space_switch(new_space);

	arm64_irq_restore(irq);

	/* Succeeded: the return to user enters the new program. */
	return 0;
}

/*
 * Reports the running task's user stack pointer, or 0 outside a system call.
 */
uintptr_t
hal_task_get_user_stack(
	void)
{
	struct arm64_exception_frame *frame;

	/* Only a task inside the kernel from user has a user stack to report. */
	frame = running_user_frame();
	if (frame == NULL)
		return 0;

	/* Succeeded: the stack the task returns to. */
	return (uintptr_t)frame->user_sp;
}

/*
 * Reports the active return-to-user context.
 *
 * The caller asks for the three values it wants and may omit any of
 * them, so the struct that used to carry all three is gone.
 */
int
hal_task_get_user_context(
	uintptr_t *pc,
	uintptr_t *stack_pointer,
	intptr_t *return_value)
{
	struct arm64_exception_frame *frame;

	/* Requires a frame that entered from user privilege. */
	frame = running_user_frame();
	if (frame == NULL)
		return -1;

	/* Reports each value the caller asked for. */
	if (pc != NULL)
		*pc = (uintptr_t)frame->elr;
	if (stack_pointer != NULL)
		*stack_pointer = (uintptr_t)frame->user_sp;
	if (return_value != NULL)
		*return_value = (intptr_t)frame->x[0];

	/* Succeeded: every value asked for is filled. */
	return 0;
}

/*
 * Makes the running task's return to user enter a signal handler.
 *
 * The interrupted frame and floating-point state are kept on the task's
 * signal stack under the token, which hal_task_signal_return() must show.
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
	struct arm64_exception_frame *frame;
	struct arm64_task *task;
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
	arm64_fp_save(task->signal_fpregs[depth]);
	task->signal_token[depth] = token;
	task->signal_depth = depth + 1U;

	/* Enters the handler with its three arguments and the restorer as the return. */
	frame->elr = handler;
	frame->user_sp = stack;
	frame->x[0] = (uint64_t)signal_number;
	frame->x[1] = information;
	frame->x[2] = context;
	frame->x[30] = restorer;

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
	struct arm64_exception_frame *frame;
	struct arm64_task *task;
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
	*value = (intptr_t)frame->x[0];
	arm64_fp_restore(task->signal_fpregs[depth]);
	task->signal_token[depth] = 0;
	task->signal_depth = depth;

	/* Succeeded: the return to user resumes the interrupted code. */
	return 0;
}

/*
 * Destroys a task that runs on no CPU.
 */
void
hal_task_destroy(
	hal_task_t handle)
{
	struct arm64_task *task;
	int run_cpu;

	/* A missing task is nothing to destroy. */
	task = handle;
	if (task == NULL)
		return;

	/* Refuses a task whose stack a CPU still uses. */
	run_cpu = __atomic_load_n(&task->run_cpu, __ATOMIC_ACQUIRE);
	if (run_cpu >= 0)
		HAL_FATAL("destroy current arm64 task");

	/* Unlists the task, then measures and frees its stack, then frees its record. */
	task_del(task);
	if (task->sys_stack != NULL) {
		task_stack_record_depth(task);
		kernel_free(task->sys_stack_allocation);
	}
	kernel_free(task);
}

/*
 * Switches this CPU from the running task to another one.
 *
 * The departing task stays marked as running until the arriving context
 * has left its stack, so that another CPU cannot resume a task whose
 * stack is still being pushed to.
 */
void
hal_task_context_switch(
	hal_task_t handle)
{
	struct arm64_percpu *cpu;
	struct arm64_task *to;
	struct arm64_task *from;
	hal_cpu_id_t target_cpu;
	uint64_t tls;
	int run_cpu;
	int guard_intact;

	/* Resolves both tasks. */
	cpu = arm64_percpu_current();
	to = handle;
	from = cpu->running_task;
	if (to == NULL || from == NULL)
		HAL_FATAL("invalid arm64 task switch");

	/* A switch to the running task changes nothing. */
	if (to == from)
		return;

	/*
	 * Stops at a departing task that has grown into the bottom of its
	 * kernel stack, before the overrun corrupts the memory below it
	 * further (BUG-321).
	 */
	if (from->sys_stack != NULL) {
		guard_intact = task_stack_guard_intact(from);
		if (!guard_intact)
			HAL_FATAL("arm64 kernel stack overflow");
	}

	/* Requires a destination that runs nowhere. */
	run_cpu = __atomic_load_n(&to->run_cpu, __ATOMIC_ACQUIRE);
	if (run_cpu >= 0)
		HAL_FATAL("arm64 HAL task already running");

	/* Requires a destination meant for this CPU. */
	target_cpu = __atomic_load_n(&to->target_cpu, __ATOMIC_ACQUIRE);
	if (target_cpu != cpu->logical_id)
		HAL_FATAL("arm64 HAL task resumed on wrong CPU");

	/*
	 * Records the departing task for arm64_task_finish_switch(), which the
	 * arriving context runs once it is off the departing task's stack.
	 */
	cpu->switching_from = from;
	__atomic_store_n(&to->run_cpu, (int)cpu->logical_id, __ATOMIC_RELEASE);

	/*
	 * The stall watch counts the switch, which shows this CPU's scheduler
	 * moving, and stops naming an interrupt: the arriving task does not
	 * run inside the departing one's.
	 */
	__atomic_store_n(&cpu->watch_switches, cpu->watch_switches + 1U, __ATOMIC_RELAXED);
	__atomic_store_n(&cpu->watch_irq_id, 0U, __ATOMIC_RELAXED);

	/* Keeps the departing task's floating-point state and TLS. */
	arm64_fp_save(from->fpregs);
	__asm__ volatile("mrs %0, tpidr_el0" : "=r"(tls));
	from->tls = (uintptr_t)tls;

	/* Installs the arriving task's identity, space and floating-point state. */
	cpu->running_task = to;
	hal_space_switch(to->space);
	arm64_fp_restore(to->fpregs);

	/* The processor takes the stepping and the debug points of the task it runs. */
	arm64_debug_switch(to);

	/* Installs the arriving task's TLS, then its stack and registers. */
	__asm__ volatile("msr tpidr_el0, %0" : : "r"((uint64_t)to->tls));
	asm_task_dispatch(&from->resume_sp, &to->resume_sp);

	/* Resumed here by a later switch; releases the task that made it. */
	arm64_task_finish_switch();
}

/*
 * Marks the task this CPU just switched away from as no longer running.
 *
 * Every context that starts running on a CPU calls this first, whether it
 * resumed in hal_task_context_switch() or entered through one of the new
 * task stubs.  Only after this may hal_task_transfer() move that task to
 * another CPU.
 */
void
arm64_task_finish_switch(
	void)
{
	struct arm64_percpu *cpu;
	struct arm64_task *departed;

	/* Takes the departing task recorded by hal_task_context_switch(). */
	cpu = arm64_percpu_current();
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
arm64_task_returned(
	void)
{
	/* A system task must never return. */
	HAL_FATAL("arm64 task returned");

	/* Retains a halt for a fatal handler that returns. */
	for (;;)
		arm64_wfi();
}

/*
 * Idles the current CPU until one interrupt arrives.
 */
void
hal_cpu_idle(
	void)
{
	/* Waits with interrupts on, and leaves with them off again. */
	arm64_irq_unmask();
	arm64_wfi();
	arm64_irq_mask();
}

/*
 * Reports the task running on the current CPU.
 */
hal_task_t
hal_task_get_current(
	void)
{
	struct arm64_task *task;

	/* The CPU's record names its running task. */
	task = running_task();

	/* Succeeded: the running task. */
	return task;
}

/*
 * Reports the task running on the current CPU, for the rest of the HAL.
 */
struct arm64_task *
arm64_task_running(
	void)
{
	struct arm64_task *task;

	/* The CPU's record names its running task. */
	task = running_task();

	/* Succeeded: the running task. */
	return task;
}

/*
 * Sets a task's user TLS, in the register too when the task runs here.
 */
void
hal_task_set_tls(
	hal_task_t handle,
	uintptr_t value)
{
	struct arm64_task *task;
	struct arm64_task *running;
	uint64_t irq;

	/* A missing task has no TLS. */
	task = handle;
	if (task == NULL)
		return;

	/*
	 * Keeps the value, and loads it when the task is the caller.  With
	 * interrupts off, so that a task switch between the two stores does
	 * not save the old register over the new value.
	 */
	irq = arm64_irq_save();

	task->tls = value;
	running = running_task();
	if (task == running)
		__asm__ volatile("msr tpidr_el0, %0" : : "r"((uint64_t)value));

	arm64_irq_restore(irq);
}

/*
 * Reports a task's user TLS as last saved or set.
 */
uintptr_t
hal_task_get_tls(
	hal_task_t handle)
{
	struct arm64_task *task;

	/* A missing task has no TLS. */
	task = handle;
	if (task == NULL)
		return 0;

	/* Succeeded: the saved value. */
	return task->tls;
}

/*
 * Stores the kernel's link from a task to its owner.
 */
void
hal_task_set_private(
	hal_task_t handle,
	void *private_data)
{
	struct arm64_task *task;

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
	struct arm64_task *task;

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
	struct arm64_task *task;

	/* A missing task is taken as a system task. */
	task = handle;
	if (task == NULL)
		return HAL_SPACE_SYS;

	/* Succeeded: the task's space. */
	return task->space;
}

/*
 * Moves a task that runs nowhere to another CPU, where it is resumed next.
 */
int
hal_task_transfer(
	hal_task_t handle,
	hal_cpu_id_t target_cpu)
{
	struct arm64_task *task;
	struct hal_cpu_mask ready;
	unsigned count;
	int run_cpu;
	int target_ready;

	/* Requires a task and a CPU the kernel knows. */
	task = handle;
	if (task == NULL)
		return HAL_ERR_INVALID;
	count = hal_cpu_count();
	if (target_cpu >= count)
		return HAL_ERR_INVALID;

	/* Refuses a task whose stack a CPU still uses. */
	run_cpu = __atomic_load_n(&task->run_cpu, __ATOMIC_ACQUIRE);
	if (run_cpu >= 0)
		return HAL_ERR_BUSY;

	/* Requires the target CPU to be up. */
	hal_cpu_ready_mask(&ready);
	target_ready = hal_cpu_mask_test(&ready, target_cpu);
	if (!target_ready)
		return HAL_ERR_STATE;

	/* Publishes the destination before another CPU can resume the task. */
	__atomic_store_n(&task->target_cpu, target_cpu, __ATOMIC_RELEASE);

	/* Succeeded: the task is resumed on the target CPU next. */
	return HAL_OK;
}

/*
 * Reports how many tasks exist and how many kernel-stack bytes they hold.
 */
void
hal_arm64_task_memory_stats(
	uint32_t *count,
	size_t *bytes)
{
	bool enabled;

	/* Samples both counters together, for whichever the caller asked. */
	enabled = arm64_spin_lock(&task_list_lock);

	if (count != NULL)
		*count = task_count;
	if (bytes != NULL)
		*bytes = task_stack_bytes;

	arm64_spin_unlock(&task_list_lock, enabled);
}

/*
 * Proves that asm_task_dispatch() switches stacks both ways, before the
 * kernel relies on it.
 */
void
arm64_context_selftest(
	void)
{
	uint64_t *stack_pointer;

	/* Builds a stack that dispatches into self_task(). */
	stack_pointer = (uint64_t *)(self_stack + sizeof(self_stack));
	stack_pointer -= ARM64_DISPATCH_WORDS;
	hal_memset(stack_pointer, 0, ARM64_DISPATCH_WORDS * sizeof(*stack_pointer));
	stack_pointer[ARM64_DISPATCH_WORDS - 1U] = (uintptr_t)self_task;
	self_task_sp = (uintptr_t)stack_pointer;

	/* The first switch runs the task to its first yield. */
	asm_task_dispatch(&self_main_sp, &self_task_sp);
	if (self_count != 1)
		HAL_FATAL("context selftest first switch");

	/* The second switch resumes it from that yield. */
	asm_task_dispatch(&self_main_sp, &self_task_sp);
	if (self_count != 2)
		HAL_FATAL("context selftest second switch");

	hal_puts("ARM64 CONTEXT PASS\n");
}

/* Lists a new task and counts its kernel stack. */
static void
task_add(
	struct arm64_task *task)
{
	struct arm64_task **link;
	bool enabled;

	/* Appends the task at the end of the list. */
	enabled = arm64_spin_lock(&task_list_lock);

	link = &task_list;
	while (*link != NULL)
		link = &(*link)->next;
	*link = task;
	task->next = NULL;
	task_count++;
	if (task->sys_stack != NULL)
		task_stack_bytes += ARM64_SYS_STACK_SIZE;

	arm64_spin_unlock(&task_list_lock, enabled);
}

/* Unlists a task and stops counting its kernel stack. */
static void
task_del(
	struct arm64_task *task)
{
	struct arm64_task **link;
	bool enabled;

	/* Finds the task's link and takes it out, if it is listed. */
	enabled = arm64_spin_lock(&task_list_lock);

	link = &task_list;
	while (*link != NULL && *link != task)
		link = &(*link)->next;
	if (*link != NULL) {
		*link = task->next;
		if (task_count != 0)
			task_count--;
		if (task->sys_stack != NULL)
			task_stack_bytes -= ARM64_SYS_STACK_SIZE;
	}

	arm64_spin_unlock(&task_list_lock, enabled);
}

/* Builds the stack the first switch to a new task pops. */
static void
build_stack(
	struct arm64_task *task,
	void (*start)(void *),
	void *argument,
	void *user_stack_pointer)
{
	uint64_t *stack_pointer;

	/* Reserves the twelve words asm_task_dispatch() pops: x19-x30. */
	stack_pointer = (uint64_t *)((uintptr_t)task->sys_stack + ARM64_SYS_STACK_SIZE);
	stack_pointer -= ARM64_DISPATCH_WORDS;
	hal_memset(stack_pointer, 0, ARM64_DISPATCH_WORDS * sizeof(*stack_pointer));

	/*
	 * x19 is the entry and x20 its argument (a system task) or the user
	 * stack (a user task); x30 is the stub the dispatch returns into.
	 */
	stack_pointer[0] = (uintptr_t)start;
	if (task->space == HAL_SPACE_SYS) {
		stack_pointer[1] = (uintptr_t)argument;
		stack_pointer[ARM64_DISPATCH_WORDS - 1U] = (uintptr_t)arm64_kernel_task_entry;
	} else {
		stack_pointer[1] = (uintptr_t)user_stack_pointer;
		stack_pointer[ARM64_DISPATCH_WORDS - 1U] = (uintptr_t)arm64_user_task_entry;
	}

	task->resume_sp = (uintptr_t)stack_pointer;
}

/* Reports whether a kernel stack's lowest bytes still hold their fill. */
static int
task_stack_guard_intact(
	const struct arm64_task *task)
{
	const uint64_t *word;
	size_t index;

	/* Compares the guard a word at a time, from the bottom of the stack. */
	word = task->sys_stack;
	for (index = 0; index < ARM64_STACK_GUARD_BYTES / sizeof(*word); index++) {
		if (word[index] != ARM64_STACK_FILL_WORD)
			return 0;
	}

	/* Succeeded: the task has never reached its guard. */
	return 1;
}

/* Measures how many bytes of its kernel stack a task has ever used. */
static size_t
task_stack_depth(
	const struct arm64_task *task)
{
	const uint64_t *word;
	size_t count;
	size_t index;

	/* Finds the lowest word that the task's own frames have overwritten. */
	word = task->sys_stack;
	count = ARM64_SYS_STACK_SIZE / sizeof(*word);
	index = 0;
	while (index < count && word[index] == ARM64_STACK_FILL_WORD)
		index++;

	/* Reports the distance from that word to the top of the stack. */
	return (count - index) * sizeof(*word);
}

/*
 * Checks a dead task's kernel stack and reports a new deepest use.
 *
 * A record past half of the stack would have overflowed the 16 KiB stacks
 * of earlier kernels, so it is worth a line on the console.
 */
static void
task_stack_record_depth(
	const struct arm64_task *task)
{
	size_t depth;
	size_t previous;
	int guard_intact;
	int raised;

	/* Stops at a stack that was overrun after its last switch away. */
	guard_intact = task_stack_guard_intact(task);
	if (!guard_intact)
		HAL_FATAL("arm64 kernel stack overflow");

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
	if (raised && depth > ARM64_SYS_STACK_SIZE / 2U) {
		hal_printf(
			"arm64: kernel stack high water %lu of %lu bytes\n",
			(unsigned long)depth,
			(unsigned long)ARM64_SYS_STACK_SIZE);
	}
}

/* Reports the task the current CPU runs, which is the caller's. */
static struct arm64_task *
running_task(
	void)
{
	struct arm64_percpu *cpu;
	struct arm64_task *task;
	uint64_t irq;

	/*
	 * Reads the CPU's record and its running task with interrupts off.
	 * TPIDR_EL1 names the CPU, not the task, so the two reads are two
	 * instructions; a caller preempted between them and resumed on another
	 * CPU would read the old CPU's task, another thread's (as BUG-082 did
	 * on amd64), and a mutex released by it would trap as not its owner.
	 * With interrupts off the caller stays where it read the record.
	 */
	irq = arm64_irq_save();
	cpu = arm64_percpu_current();
	task = cpu->running_task;
	arm64_irq_restore(irq);

	/* Succeeded: the running task, or NULL before the CPU's initial task. */
	return task;
}

/* Reports the running task's user frame, or NULL outside a user entry. */
static struct arm64_exception_frame *
running_user_frame(
	void)
{
	struct arm64_task *task;

	/* A CPU without a task has no frame. */
	task = running_task();
	if (task == NULL)
		return NULL;

	/* Succeeded: the frame, or NULL when the task is not inside a user entry. */
	return task->active_user_frame;
}

/* Runs the self-test's task: yields back twice, counting each turn. */
static void
self_task(
	void)
{
	/* The first turn. */
	self_count = 1;
	asm_task_dispatch(&self_task_sp, &self_main_sp);

	/* The second turn. */
	self_count = 2;
	asm_task_dispatch(&self_task_sp, &self_main_sp);

	/* The test never resumes the task a third time. */
	for (;;)
		arm64_wfi();
}
