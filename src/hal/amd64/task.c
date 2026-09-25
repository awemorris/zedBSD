/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 opaque task contexts and context-switch bookkeeping.
 */

#include <hal/hal.h>
#include "task.h"
#include "defs.h"
#include "asm.h"
#include "descriptor.h"
#include "int.h"
#include "irq.h"
#include "percpu.h"

#define running_task (amd64_percpu_current()->running_task)

static struct amd64_task *task_list;
static uint8_t initial_fpregs[512] __attribute__((aligned(16)));
static uint32_t task_count;
static size_t task_stack_bytes;
static hal_task_t xmm_selftest_main;
static hal_task_t xmm_selftest_task;
static volatile unsigned xmm_selftest_stage;
static volatile unsigned initial_fpregs_ready;
static volatile unsigned initial_fpregs_claimed;
static volatile unsigned task_registry_lock;
static const uint8_t xmm_main_pattern[16] = {
	0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
	0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01
};
static const uint8_t xmm_task_pattern[16] = {
	0xa5, 0x5a, 0xc3, 0x3c, 0x96, 0x69, 0xf0, 0x0f,
	0x12, 0x21, 0x34, 0x43, 0x56, 0x65, 0x78, 0x87
};

static int xmm_equal(const uint8_t *left, const uint8_t *right);
static void xmm_selftest_entry(void *argument);
static void xmm_context_selftest(void);
static void *task_fpregs(struct amd64_task *task);
static void *task_signal_fpregs(struct amd64_task *task, unsigned depth);
static void tasklist_add(struct amd64_task *task);
static void tasklist_del(struct amd64_task *task);
static void build_initial_stack(struct amd64_task *task, void (*start)(void *), void *argument, void *user_stack);

/*
 * Initializes task state for the current CPU.
 */
struct amd64_task *
amd64_task_init_cpu(
	int run_selftest)
{
	struct amd64_task *task;
	void *fpregs;
	hal_cpu_id_t cpu;

	/* Resolves the current CPU before inspecting its task state. */
	cpu = hal_cpu_current();

	/* Prevents a second initial-task installation on this CPU. */
	if (running_task != NULL)
		HAL_FATAL("amd64 initial task created twice on one CPU");

	/* Establishes or waits for the canonical initial FP image. */
	if (run_selftest) {
		__asm__ volatile("fninit; fxsave64 %0" : "=m"(initial_fpregs));
		__atomic_store_n(&initial_fpregs_ready, 1U, __ATOMIC_RELEASE);
	} else if (__atomic_load_n(
	    &initial_fpregs_ready,
	    __ATOMIC_ACQUIRE) == 0) {
		HAL_FATAL("amd64 AP task before BSP task initialization");
	}

	/* Allocates the initial task record. */
	task = kernel_alloc(sizeof(*task));
	if (task == NULL)
		return NULL;

	/* Initializes the task as a running system-space context. */
	hal_memset(task, 0, sizeof(*task));
	task->space = HAL_SPACE_SYS;
	task->run_cpu = (int)cpu;
	task->target_cpu = cpu;
	fpregs = task_fpregs(task);
	hal_memcpy(fpregs, initial_fpregs, sizeof(initial_fpregs));
	tasklist_add(task);
	running_task = task;

	/* Verifies FP context switching only on the first CPU. */
	if (run_selftest)
		xmm_context_selftest();

	/* Reports the published initial task. */
	return task;
}

/*
 * Wraps the calling CPU's current context as that CPU's initial task.
 * The first CPU to arrive publishes the canonical FP image and runs the
 * context self-test; every later CPU adopts that image.
 */
hal_task_t
hal_task_create_for_init_context(
	void)
{
	unsigned previous;

	/* Claims the FP-image role exactly once across all CPUs. */
	previous = __atomic_exchange_n(
		&initial_fpregs_claimed,
		1U,
		__ATOMIC_ACQ_REL);

	/* Establishes this CPU's initial task. */
	return amd64_task_init_cpu(previous == 0U);
}

/*
 * Creates an inactive amd64 task context.
 */
hal_task_t
hal_task_create(
	hal_space_t space,
	void (*start)(void *),
	void *argument,
	void *user_stack_pointer)
{
	struct amd64_task *task;
	void *fpregs;

	/* Validates the entry point and system-versus-user stack convention. */
	if (start == NULL ||
	    ((space == HAL_SPACE_SYS) != (user_stack_pointer == NULL)))
		return NULL;

	/* Allocates the task record. */
	task = kernel_alloc(sizeof(*task));
	if (task == NULL)
		return NULL;
	hal_memset(task, 0, sizeof(*task));

	/* Allocates the separately owned aligned kernel stack. */
	task->sys_stack_allocation = kernel_alloc(AMD64_SYS_STACK_SIZE + 15U);
	if (task->sys_stack_allocation == NULL) {
		kernel_free(task);
		return NULL;
	}

	/* Initializes the inactive execution context. */
	task->sys_stack = (void *)(((uintptr_t)task->sys_stack_allocation + 15U) &
	    ~(uintptr_t)15U);
	task->space = space;
	task->run_cpu = -1;
	task->target_cpu = hal_cpu_current();
	fpregs = task_fpregs(task);
	hal_memcpy(fpregs, initial_fpregs, sizeof(initial_fpregs));
	build_initial_stack(task, start, argument, user_stack_pointer);
	tasklist_add(task);

	/* Returns the registered inactive task. */
	return task;
}

/*
 * Registers the current user-return frame with its task.
 */
void
amd64_task_enter_user_frame(
	void *frame)
{
	/* Publishes the frame only when this CPU has a current task. */
	if (running_task != NULL)
		running_task->active_user_frame = frame;
}

/*
 * Withdraws the current user-return frame from its task.
 */
void
amd64_task_leave_user_frame(
	void)
{
	/* Clears the frame only when this CPU has a current task. */
	if (running_task != NULL)
		running_task->active_user_frame = NULL;
}

/*
 * Forks the current user task and interrupt frame.
 */
hal_task_t
hal_task_fork_current(
	hal_space_t child_space,
	intptr_t child_result)
{
	struct amd64_interrupt_frame *source;
	struct amd64_interrupt_frame *copy;
	struct amd64_task *child;
	uintptr_t *resume;
	void *running_fpregs;
	void *child_fpregs;

	/* Requires an active user frame and a user child address space. */
	if (running_task == NULL ||
	    child_space == HAL_SPACE_SYS ||
	    running_task->active_user_frame == NULL)
		return NULL;

	/* Requires the active frame to return to ring three. */
	source = running_task->active_user_frame;
	if ((source->cs & 3U) != 3U)
		return NULL;

	/* Creates the child with the parent's user entry and stack. */
	child = hal_task_create(
		child_space,
		(void (*)(void *))source->rip,
		NULL,
		(void *)(uintptr_t)source->rsp);
	if (child == NULL)
		return NULL;

	/* Builds a resume prefix immediately below the copied user frame. */
	resume = (uintptr_t *)((uintptr_t)child->sys_stack +
	    AMD64_SYS_STACK_SIZE - sizeof(*source) - 8U * sizeof(uintptr_t));
	hal_memset(resume, 0, 8U * sizeof(uintptr_t));

	/* Interrupts stay off until the frame entry stub has released the departed task. */
	resume[6] = 0x2U;
	resume[7] = (uintptr_t)amd64_user_frame_entry;
	copy = (struct amd64_interrupt_frame *)(resume + 8);
	*copy = *source;
	copy->rax = (uint64_t)child_result;
	child->resume_rsp = (uintptr_t)resume;
	child->tls = running_task->tls;

	/* Saves the live parent FP image before copying it to the child. */
	running_fpregs = task_fpregs(running_task);
	__asm__ volatile("fxsave64 (%0)"
	    :
	    : "r"(running_fpregs)
	    : "memory");
	child_fpregs = task_fpregs(child);
	running_fpregs = task_fpregs(running_task);
	hal_memcpy(child_fpregs, running_fpregs, 512U);

	/* Returns the complete child context. */
	return child;
}

/*
 * Validates an in-place user exec transition.
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

	/* Requires a non-system destination address space. */
	if (new_space == HAL_SPACE_SYS)
		return -1;

	/* Requires an active user-return frame. */
	if (running_task->active_user_frame == NULL)
		return -1;

	/* Requires nonzero user entry and stack addresses. */
	if (entry == 0 || user_stack_pointer == 0)
		return -1;

	/* Reports a valid user exec transition. */
	return 0;
}

/*
 * Replaces the current user execution context.
 */
int
hal_task_exec_current(
	hal_space_t new_space,
	uintptr_t entry,
	uintptr_t user_stack_pointer)
{
	struct amd64_interrupt_frame *frame;
	void *fpregs;
	uint64_t code_segment;
	uint64_t stack_segment;
	uint64_t flags;
	int error;

	/* Validates the complete exec transition before mutation. */
	error = hal_task_exec_validate(new_space, entry, user_stack_pointer);
	if (error != 0)
		return -1;

	/* Rebuilds the active frame for a clean ring-three entry. */
	frame = running_task->active_user_frame;
	code_segment = SEG_USER_CODE | 3U;
	stack_segment = SEG_USER_DATA | 3U;
	flags = 0x202U;
	hal_memset(frame, 0, sizeof(*frame));
	frame->rip = entry;
	frame->cs = code_segment;
	frame->rflags = flags;
	frame->rsp = user_stack_pointer;
	frame->ss = stack_segment;

	/* Resets task metadata and the architectural TLS base. */
	running_task->space = new_space;
	running_task->tls = 0;
	asm_write_msr(AMD64_MSR_FS_BASE, 0);
	running_task->signal_depth = 0;

	/* Restores the canonical initial floating-point state. */
	fpregs = task_fpregs(running_task);
	hal_memcpy(fpregs, initial_fpregs, sizeof(initial_fpregs));
	fpregs = task_fpregs(running_task);
	__asm__ volatile("fxrstor64 (%0)"
	    :
	    : "r"(fpregs)
	    : "memory");

	/* Activates the replacement address space. */
	hal_space_switch(new_space);

	/* Reports a completed exec transition. */
	return 0;
}

/*
 * Reports the current task's active user stack pointer.
 */
uintptr_t
hal_task_get_user_stack(
	void)
{
	struct amd64_interrupt_frame *frame;

	/* Resolves the active frame when a current task exists. */
	if (running_task != NULL)
		frame = running_task->active_user_frame;
	else
		frame = NULL;

	/* Rejects an absent or non-user frame. */
	if (frame == NULL || (frame->cs & 3U) != 3U)
		return 0;

	/* Returns the ring-three stack pointer. */
	return (uintptr_t)frame->rsp;
}

/*
 * Reads the current task's active user context.
 */
int
hal_task_get_user_context(
	uintptr_t *pc,
	uintptr_t *stack_pointer,
	intptr_t *return_value)
{
	struct amd64_interrupt_frame *frame;

	/* Resolves the active frame when a current task exists. */
	if (running_task != NULL)
		frame = running_task->active_user_frame;
	else
		frame = NULL;

	/* Requires a destination and ring-three frame. */
	if (frame == NULL || (frame->cs & 3U) != 3U)
		return -1;

	/* Copies each requested user-context field. */
	if (pc != NULL)
		*pc = (uintptr_t)frame->rip;
	if (stack_pointer != NULL)
		*stack_pointer = (uintptr_t)frame->rsp;
	if (return_value != NULL)
		*return_value = (intptr_t)frame->rax;

	/* Reports a complete context snapshot. */
	return 0;
}

/*
 * Redirects the current user frame into a signal handler.
 */
int
hal_task_signal_enter(
	uintptr_t handler,
	uintptr_t stack_pointer,
	int signal,
	uintptr_t information,
	uintptr_t context,
	uintptr_t restorer,
	uint32_t token)
{
	struct amd64_interrupt_frame *frame;
	void *fpregs;
	unsigned depth;

	UNUSED_PARAMETER(restorer);

	/* Resolves the active frame when a current task exists. */
	if (running_task != NULL)
		frame = running_task->active_user_frame;
	else
		frame = NULL;

	/* Validates the frame, nesting capacity, and signal entry data. */
	if (frame == NULL ||
	    running_task->signal_depth >= HAL_SIGNAL_NEST_MAX ||
	    handler == 0 ||
	    stack_pointer == 0 ||
	    token == 0)
		return -1;

	/* Saves the interrupted integer and floating-point contexts. */
	depth = running_task->signal_depth;
	running_task->signal_frame[depth] = *frame;
	fpregs = task_signal_fpregs(running_task, depth);
	__asm__ volatile("fxsave64 (%0)"
	    :
	    : "r"(fpregs)
	    : "memory");
	running_task->signal_token[depth] = token;
	running_task->signal_depth = depth + 1U;

	/* Rewrites the live frame for the signal-handler ABI. */
	frame->rip = handler;
	frame->rsp = stack_pointer;
	frame->rdi = (uint64_t)(uint32_t)signal;
	frame->rsi = information;
	frame->rdx = context;

	/* Reports a completed signal entry. */
	return 0;
}

/*
 * Restores the current task's interrupted signal context.
 */
int
hal_task_signal_return(
	uint32_t token,
	intptr_t *value)
{
	struct amd64_interrupt_frame *frame;
	void *fpregs;
	unsigned depth;

	/* Resolves the active frame when a current task exists. */
	if (running_task != NULL)
		frame = running_task->active_user_frame;
	else
		frame = NULL;

	/* Requires a live signal frame, result destination, and token. */
	if (frame == NULL ||
	    value == NULL ||
	    running_task->signal_depth == 0 ||
	    token == 0)
		return -1;

	/* Requires the token for the innermost saved signal context. */
	depth = running_task->signal_depth - 1U;
	if (token != running_task->signal_token[depth])
		return -1;

	/* Restores the integer result and floating-point context. */
	*frame = running_task->signal_frame[depth];
	*value = (intptr_t)frame->rax;
	fpregs = task_signal_fpregs(running_task, depth);
	__asm__ volatile("fxrstor64 (%0)"
	    :
	    : "r"(fpregs)
	    : "memory");
	running_task->signal_token[depth] = 0;
	running_task->signal_depth = depth;

	/* Reports a completed signal return. */
	return 0;
}

/*
 * Destroys one inactive amd64 task.
 */
void
hal_task_destroy(
	hal_task_t handle)
{
	struct amd64_task *task;

	/* Ignores an absent task handle. */
	task = handle;
	if (task == NULL)
		return;

	/* Refuses to destroy a task running on any CPU. */
	if (__atomic_load_n(&task->run_cpu, __ATOMIC_ACQUIRE) >= 0)
		HAL_FATAL("destroying running amd64 task");

	/* Withdraws the task from the global registry. */
	tasklist_del(task);

	/* Releases the separately owned kernel stack when present. */
	if (task->sys_stack != NULL)
		kernel_free(task->sys_stack_allocation);

	/* Releases the task record last. */
	kernel_free(task);
}

/*
 * Switches execution to one inactive amd64 task.
 */
void
hal_task_context_switch(
	hal_task_t handle)
{
	struct amd64_task *to;
	struct amd64_task *from;
	void *from_fpregs;
	void *to_fpregs;
	hal_cpu_id_t current_cpu;
	hal_cpu_id_t target_cpu;

	/* Resolves both task contexts before validating the transition. */
	to = handle;
	from = running_task;
	if (to == NULL || from == NULL)
		HAL_FATAL("invalid amd64 task switch");

	/* Leaves an already current task unchanged. */
	if (to == from)
		return;

	/* Requires the destination to be inactive. */
	if (__atomic_load_n(&to->run_cpu, __ATOMIC_ACQUIRE) >= 0)
		HAL_FATAL("amd64 HAL task already running");

	/* Requires the destination to target the current CPU. */
	target_cpu = (hal_cpu_id_t)__atomic_load_n(
		&to->target_cpu,
		__ATOMIC_ACQUIRE);
	current_cpu = hal_cpu_current();
	if (target_cpu != current_cpu)
		HAL_FATAL("amd64 HAL task resumed on wrong CPU");

	/*
	 * The departing task stays marked as running until the arriving
	 * context has left its stack: amd64_task_finish_switch() clears it
	 * after the stack switch below, so that another CPU cannot resume a
	 * task whose stack is still being pushed to.
	 */
	amd64_percpu_current()->switching_from = from;
	current_cpu = hal_cpu_current();
	__atomic_store_n(&to->run_cpu, (int)current_cpu, __ATOMIC_RELEASE);

	/* Switches task identity, address space, and architectural TLS. */
	from->tls = (uintptr_t)asm_read_msr(AMD64_MSR_FS_BASE);
	running_task = to;
	hal_space_switch(to->space);
	asm_write_msr(AMD64_MSR_FS_BASE, (uint64_t)to->tls);

	/*
	 * The debug registers are written only when one of the two tasks
	 * uses them, so a system with no debugger pays nothing.
	 */
	if (from->debug_point_count != 0U || to->debug_point_count != 0U)
		amd64_debug_load(to);

	/* Selects the destination kernel stack for future privilege changes. */
	if (to->sys_stack != NULL) {
		amd64_set_tss_rsp0(
			(uintptr_t)to->sys_stack + AMD64_SYS_STACK_SIZE);
	}

	/* Saves and restores FP state before the assembly stack switch. */
	from_fpregs = task_fpregs(from);
	to_fpregs = task_fpregs(to);
	__asm__ volatile("fxsave64 (%0)"
	    :
	    : "r"(from_fpregs)
	    : "memory");
	__asm__ volatile("fxrstor64 (%0)"
	    :
	    : "r"(to_fpregs)
	    : "memory");
	asm_task_dispatch(&from->resume_rsp, &to->resume_rsp);

	/* Resumed here by a later switch; releases the task that made it. */
	amd64_task_finish_switch();
}

/*
 * Marks the task this CPU just switched away from as no longer running.
 *
 * Every context that starts running on a CPU calls this first, whether it
 * resumed through asm_task_dispatch() or entered through one of the new
 * task stubs.  Only after this may hal_task_transfer() move that task to
 * another CPU.
 */
void
amd64_task_finish_switch(
	void)
{
	struct amd64_percpu *cpu;
	struct amd64_task *departed;

	/* Takes the departing task recorded by hal_task_context_switch(). */
	cpu = amd64_percpu_current();
	departed = cpu->switching_from;
	cpu->switching_from = NULL;

	/* Publishes that the departed task's stack is free to resume elsewhere. */
	if (departed != NULL)
		__atomic_store_n(&departed->run_cpu, -1, __ATOMIC_RELEASE);
}

/*
 * Stops a task that returned through its kernel entry wrapper.
 */
void
amd64_task_returned(
	void)
{
	/* Reports the invalid return through the fatal path. */
	HAL_FATAL("amd64 task returned");

	/* Retains a physical halt fallback for a returning fatal handler. */
	for (;;)
		asm_hlt();
}

/*
 * Idles the current CPU until one interrupt arrives.
 */
void
hal_cpu_idle(
	void)
{
	/* Enables, waits for, and then disables local interrupts atomically. */
	__asm__ volatile("sti; hlt; cli" ::: "memory");
}

/*
 * Reports the task running on the current CPU.
 */
hal_task_t
hal_task_get_current(
	void)
{
	struct amd64_task *task;

	/* Traps on an absent selection; the CPU may change after this. */
	(void)amd64_percpu_current();

	/*
	 * Reads the running task in one GS-relative load.  Loading the state
	 * pointer and then the field is two loads, and a caller preempted and
	 * moved to another CPU between them would read the old CPU's task:
	 * another thread's.  One load is taken whole on one CPU, where the
	 * caller is the running task.
	 */
	__asm__ volatile("movq %%gs:%c1, %0"
			 : "=r"(task)
			 : "i"(AMD64_PERCPU_RUNNING_TASK));

	/* Returns the current task. */
	return task;
}

/*
 * Sets one task's userspace TLS base.
 */
void
hal_task_set_tls(
	hal_task_t handle,
	uintptr_t value)
{
	/* Updates only a valid task handle. */
	if (handle != NULL) {
		((struct amd64_task *)handle)->tls = value;

		/* Updates hardware immediately for the running task. */
		if (handle == running_task)
			asm_write_msr(AMD64_MSR_FS_BASE, (uint64_t)value);
	}
}

/*
 * Reports one task's userspace TLS base.
 */
uintptr_t
hal_task_get_tls(
	hal_task_t handle)
{
	uintptr_t value;

	/* Reads hardware for the currently running task. */
	if (handle == running_task) {
		value = (uintptr_t)asm_read_msr(AMD64_MSR_FS_BASE);

		/* Returns the live architectural TLS base. */
		return value;
	}

	/* Returns the saved TLS base for an inactive valid task. */
	if (handle != NULL)
		return ((struct amd64_task *)handle)->tls;

	/* Returns the neutral TLS value for an absent task. */
	return 0;
}

/*
 * Sets one task's kernel-private pointer.
 */
void
hal_task_set_private(
	hal_task_t handle,
	void *private_data)
{
	/* Updates only a valid task handle. */
	if (handle != NULL)
		((struct amd64_task *)handle)->private_data = private_data;
}

/*
 * Reports one task's kernel-private pointer.
 */
void *
hal_task_get_private(
	hal_task_t handle)
{
	/* Returns the saved pointer for a valid task. */
	if (handle != NULL)
		return ((struct amd64_task *)handle)->private_data;

	/* Returns no pointer for an absent task. */
	return NULL;
}

/*
 * Reports one task's address space.
 */
hal_space_t
hal_task_get_space(
	hal_task_t handle)
{
	/* Returns the saved address space for a valid task. */
	if (handle != NULL)
		return ((struct amd64_task *)handle)->space;

	/* Returns system space for an absent task. */
	return HAL_SPACE_SYS;
}

/*
 * Transfers an inactive task to a ready target CPU.
 */
int
hal_task_transfer(
	hal_task_t handle,
	hal_cpu_id_t target_cpu)
{
	struct amd64_task *task;
	struct hal_cpu_mask ready;
	unsigned cpu_count;
	int transferable;

	/* Validates the handle before querying the topology. */
	task = handle;
	if (task == NULL)
		return HAL_ERR_INVALID;
	cpu_count = hal_cpu_count();
	if (target_cpu >= cpu_count)
		return HAL_ERR_INVALID;

	/* Requires a task not currently running on any CPU. */
	if (__atomic_load_n(&task->run_cpu, __ATOMIC_ACQUIRE) >= 0)
		return HAL_ERR_BUSY;

	/* Refuses to move a task that owns an IRQ wait. */
	transferable = amd64_irq_task_transferable(task);
	if (!transferable)
		return HAL_ERR_BUSY;

	/* Requires the selected target CPU to be ready. */
	hal_cpu_ready_mask(&ready);
	if (!hal_cpu_mask_test(&ready, target_cpu))
		return HAL_ERR_STATE;

	/* Publishes the destination before another CPU can resume the task. */
	__atomic_store_n(&task->target_cpu, target_cpu, __ATOMIC_RELEASE);

	/* Reports a completed inactive-task transfer. */
	return HAL_OK;
}

/*
 * Reports registered task and kernel-stack accounting.
 */
void
hal_amd64_task_memory_stats(
	uint32_t *count,
	size_t *bytes)
{
	bool enabled;

	/* Acquires the task registry with local interrupts disabled. */
	enabled = hal_irq_disable();
	while (__atomic_exchange_n(
	    &task_registry_lock,
	    1U,
	    __ATOMIC_ACQUIRE) != 0) {
		__asm__ volatile("pause");
	}

	/* Copies each statistic requested by the caller. */
	if (count != NULL)
		*count = task_count;
	if (bytes != NULL)
		*bytes = task_stack_bytes;

	/* Releases the registry and restores prior interrupt state. */
	__atomic_store_n(&task_registry_lock, 0U, __ATOMIC_RELEASE);
	if (enabled)
		hal_irq_enable();
}

/* Compares two complete XMM self-test patterns. */
static int
xmm_equal(
	const uint8_t *left,
	const uint8_t *right)
{
	unsigned index;

	/* Compares every pattern byte in ascending order. */
	for (index = 0; index < 16U; index++) {
		/* Rejects the first byte which differs between the patterns. */
		if (left[index] != right[index])
			return 0;
	}

	/* Reports two identical patterns. */
	return 1;
}

/* Runs the child half of the XMM context-switch self-test. */
static void
xmm_selftest_entry(
	void *argument)
{
	uint8_t observed[16];
	int equal;

	UNUSED_PARAMETER(argument);

	/* Loads the child pattern and yields to the initial task. */
	amd64_xmm_load(xmm_task_pattern);
	xmm_selftest_stage = 1;
	hal_task_context_switch(xmm_selftest_main);

	/* Verifies that the child pattern survived the first switch. */
	amd64_xmm_store(observed);
	equal = xmm_equal(observed, xmm_task_pattern);
	if (!equal)
		HAL_FATAL("amd64 XMM task context corruption");

	/* Reports completion and yields to the initial task again. */
	xmm_selftest_stage = 2;
	hal_task_context_switch(xmm_selftest_main);
	HAL_FATAL("amd64 XMM self-test task resumed unexpectedly");
}

/* Runs the initial-task half of the XMM context-switch self-test. */
static void
xmm_context_selftest(
	void)
{
	uint8_t observed[16];
	int equal;

	/* Creates the paired self-test task. */
	xmm_selftest_main = running_task;
	xmm_selftest_task = hal_task_create(
		HAL_SPACE_SYS,
		xmm_selftest_entry,
		NULL,
		NULL);
	if (xmm_selftest_task == NULL)
		HAL_FATAL("amd64 XMM self-test task allocation failed");

	/* Loads the main pattern and makes the first round trip. */
	amd64_xmm_load(xmm_main_pattern);
	hal_task_context_switch(xmm_selftest_task);
	amd64_xmm_store(observed);

	/* Verifies the child stage and preserved main pattern in order. */
	if (xmm_selftest_stage != 1)
		HAL_FATAL("amd64 XMM initial task context corruption");
	equal = xmm_equal(observed, xmm_main_pattern);
	if (!equal)
		HAL_FATAL("amd64 XMM initial task context corruption");

	/* Completes the second child round trip. */
	hal_task_context_switch(xmm_selftest_task);
	if (xmm_selftest_stage != 2)
		HAL_FATAL("amd64 XMM self-test did not complete");

	/* Releases all temporary self-test ownership. */
	hal_task_destroy(xmm_selftest_task);
	xmm_selftest_task = NULL;
	xmm_selftest_main = NULL;
	hal_puts("A64 XMM CONTEXT PASS\n");
}

/* Aligns a task's floating-point save area. */
static void *
task_fpregs(
	struct amd64_task *task)
{
	void *result;

	/* Rounds the embedded storage upward to a 16-byte boundary. */
	result = (void *)(((uintptr_t)task->fpregs + 15U) &
	    ~(uintptr_t)15U);

	/* Returns the aligned save area. */
	return result;
}

/* Aligns one nested signal floating-point save area. */
static void *
task_signal_fpregs(
	struct amd64_task *task,
	unsigned depth)
{
	void *result;

	/* Rounds the selected embedded storage to a 16-byte boundary. */
	result = (void *)(((uintptr_t)task->signal_fpregs[depth] + 15U) &
	    ~(uintptr_t)15U);

	/* Returns the aligned nested save area. */
	return result;
}

/* Adds one task to the global accounting registry. */
static void
tasklist_add(
	struct amd64_task *task)
{
	struct amd64_task **link;
	bool enabled;

	/* Acquires the registry with local interrupts disabled. */
	enabled = hal_irq_disable();
	link = &task_list;
	while (__atomic_exchange_n(
	    &task_registry_lock,
	    1U,
	    __ATOMIC_ACQUIRE) != 0) {
		__asm__ volatile("pause");
	}

	/* Finds the list tail without changing registration order. */
	while (*link != NULL)
		link = &(*link)->next;

	/* Appends the task and updates registry accounting. */
	*link = task;
	task->next = NULL;
	task_count++;
	if (task->sys_stack != NULL)
		task_stack_bytes += AMD64_SYS_STACK_SIZE;

	/* Releases the registry and restores prior interrupt state. */
	__atomic_store_n(&task_registry_lock, 0U, __ATOMIC_RELEASE);
	if (enabled)
		hal_irq_enable();
}

/* Removes one task from the global accounting registry. */
static void
tasklist_del(
	struct amd64_task *task)
{
	struct amd64_task **link;
	bool enabled;

	/* Acquires the registry with local interrupts disabled. */
	enabled = hal_irq_disable();
	link = &task_list;
	while (__atomic_exchange_n(
	    &task_registry_lock,
	    1U,
	    __ATOMIC_ACQUIRE) != 0) {
		__asm__ volatile("pause");
	}

	/* Searches for the exact task record. */
	while (*link != NULL && *link != task)
		link = &(*link)->next;

	/* Unlinks a registered task and adjusts guarded accounting. */
	if (*link == task) {
		*link = task->next;

		/* Decrements the live-task count without underflow. */
		if (task_count != 0)
			task_count--;

		/* Removes this task's stack bytes from guarded accounting. */
		if (task->sys_stack != NULL &&
		    task_stack_bytes >= AMD64_SYS_STACK_SIZE)
			task_stack_bytes -= AMD64_SYS_STACK_SIZE;
	}

	/* Releases the registry and restores prior interrupt state. */
	__atomic_store_n(&task_registry_lock, 0U, __ATOMIC_RELEASE);
	if (enabled)
		hal_irq_enable();
}

/* Builds the assembly resume stack for one new task. */
static void
build_initial_stack(
	struct amd64_task *task,
	void (*start)(void *),
	void *argument,
	void *user_stack)
{
	uintptr_t *stack;

	/* Starts at the aligned top of the owned kernel stack. */
	stack = (uintptr_t *)((uintptr_t)task->sys_stack +
	    AMD64_SYS_STACK_SIZE);

	/* Builds the architecture wrapper frame for the task type. */
	if (task->space == HAL_SPACE_SYS) {
		*--stack = (uintptr_t)argument;
		*--stack = (uintptr_t)start;
		*--stack = (uintptr_t)amd64_kernel_task_entry;
	} else {
		*--stack = SEG_USER_DATA | 3U;
		*--stack = (uintptr_t)user_stack;
		*--stack = 0x202U;
		*--stack = SEG_USER_CODE | 3U;
		*--stack = (uintptr_t)start;
		*--stack = (uintptr_t)amd64_user_task_entry;
	}

	/*
	 * Builds the register prefix consumed by the dispatch assembly.  The
	 * flags leave interrupts off, so the entry stub can release the task
	 * this CPU departed from before any interrupt could switch again; the
	 * kernel stub enables them, the user stubs return through iretq.
	 */
	*--stack = 0x2U;
	*--stack = 0;
	*--stack = 0;
	*--stack = 0;
	*--stack = 0;
	*--stack = 0;
	*--stack = 0;
	task->resume_rsp = (uintptr_t)stack;
}

/*
 * Debugging a task
 */

/*
 * The flag bits a debugger may set.  The rest of the saved flags belong to
 * the kernel: interrupts stay enabled, the privilege level stays at zero,
 * and the reserved bit stays set, whatever was written.
 */
#define AMD64_USER_RFLAGS_MASK		0x0000000000254dd5ULL
#define AMD64_USER_RFLAGS_FIXED		0x0000000000000202ULL
#define AMD64_RFLAGS_TRAP		0x0000000000000100ULL

/*
 * A user address the processor will accept in the return frame.  A value
 * outside the low canonical half faults on the way out of the kernel,
 * which would be the kernel's fault rather than the debugger's.
 */
static int
amd64_user_address_valid(
	uint64_t value)
{
	/* Reports whether the address is canonical and below the hole. */
	return (value >> 47) == 0ULL;
}

/*
 * The frame a stopped task returns to user through, or NULL when the task
 * has none to show.
 */
static struct amd64_interrupt_frame *
task_user_frame(
	struct amd64_task *task)
{
	struct amd64_interrupt_frame *frame;

	/* Requires a task. */
	if (task == NULL)
		return NULL;
	frame = task->active_user_frame;

	/* Requires a frame that returns to ring three. */
	if (frame == NULL || (frame->cs & 3U) != 3U)
		return NULL;

	/* Returns the user frame. */
	return frame;
}

/*
 * Reads the user integer registers of a task.
 */
int
hal_task_get_user_gpregs(
	hal_task_t handle,
	struct hal_gpregs *registers)
{
	struct amd64_task *task;
	const struct amd64_interrupt_frame *frame;

	task = handle;

	/* Requires a task with a user frame and somewhere to report. */
	if (registers == NULL)
		return -1;
	frame = task_user_frame(task);
	if (frame == NULL)
		return -1;

	registers->rax = frame->rax;
	registers->rbx = frame->rbx;
	registers->rcx = frame->rcx;
	registers->rdx = frame->rdx;
	registers->rsi = frame->rsi;
	registers->rdi = frame->rdi;
	registers->rbp = frame->rbp;
	registers->rsp = frame->rsp;
	registers->r8 = frame->r8;
	registers->r9 = frame->r9;
	registers->r10 = frame->r10;
	registers->r11 = frame->r11;
	registers->r12 = frame->r12;
	registers->r13 = frame->r13;
	registers->r14 = frame->r14;
	registers->r15 = frame->r15;
	registers->rip = frame->rip;
	registers->rflags = frame->rflags;
	registers->cs = frame->cs;
	registers->ss = frame->ss;

	/*
	 * The thread pointer lives in the register while its task runs, and
	 * is written back to the task when the task leaves the processor.
	 */
	if (task == running_task)
		registers->fs_base = (uint64_t)asm_read_msr(AMD64_MSR_FS_BASE);
	else
		registers->fs_base = (uint64_t)task->tls;
	registers->gs_base = 0;

	/* Succeeded. */
	return 0;
}

/*
 * Writes the user integer registers of a task.
 */
int
hal_task_set_user_gpregs(
	hal_task_t handle,
	const struct hal_gpregs *registers)
{
	struct amd64_task *task;
	struct amd64_interrupt_frame *frame;

	task = handle;

	/* Requires a task with a user frame and something to write. */
	if (registers == NULL)
		return -1;
	frame = task_user_frame(task);
	if (frame == NULL)
		return -1;

	/*
	 * The instruction and stack pointers are the two the processor
	 * itself reads on the way out, so a value it would refuse is
	 * refused here instead of faulting in the kernel.
	 */
	if (!amd64_user_address_valid(registers->rip) ||
	    !amd64_user_address_valid(registers->rsp))
		return -1;

	frame->rax = registers->rax;
	frame->rbx = registers->rbx;
	frame->rcx = registers->rcx;
	frame->rdx = registers->rdx;
	frame->rsi = registers->rsi;
	frame->rdi = registers->rdi;
	frame->rbp = registers->rbp;
	frame->rsp = registers->rsp;
	frame->r8 = registers->r8;
	frame->r9 = registers->r9;
	frame->r10 = registers->r10;
	frame->r11 = registers->r11;
	frame->r12 = registers->r12;
	frame->r13 = registers->r13;
	frame->r14 = registers->r14;
	frame->r15 = registers->r15;
	frame->rip = registers->rip;

	/*
	 * Returns by IRETQ: SYSRET would replace rcx and r11, which the new
	 * registers set.
	 */
	frame->vector = INT_SYSCALL;

	/* The segment selectors are the kernel's and are not taken. */
	frame->rflags = (frame->rflags & ~AMD64_USER_RFLAGS_MASK) |
	    (registers->rflags & AMD64_USER_RFLAGS_MASK) |
	    AMD64_USER_RFLAGS_FIXED;

	/* The thread pointer follows the running task into the register. */
	if (task == running_task)
		asm_write_msr(AMD64_MSR_FS_BASE, registers->fs_base);
	task->tls = (uintptr_t)registers->fs_base;

	/* Succeeded. */
	return 0;
}

/*
 * The processor writes the x87 and vector state out together, so both of
 * the calls below read and write parts of one saved area.  A task that is
 * on a processor has the live registers rather than the saved ones, and
 * is written out first.
 */
static void *
task_current_fpregs(
	struct amd64_task *task)
{
	void *area;

	area = task_fpregs(task);

	/* Brings the live registers into the saved area. */
	if (task == running_task)
		__asm__ volatile("fxsave64 (%0)" : : "r"(area) : "memory");

	/* Returns the saved area. */
	return area;
}

static void
task_reload_fpregs(
	struct amd64_task *task,
	void *area)
{
	/* Returns the saved area to the live registers. */
	if (task == running_task)
		__asm__ volatile("fxrstor64 (%0)" : : "r"(area) : "memory");
}

/*
 * Reads the x87 state of a task.
 */
int
hal_task_get_user_fpregs(
	hal_task_t handle,
	struct hal_fpregs *registers)
{
	struct amd64_task *task;
	const uint8_t *area;

	task = handle;

	/* Requires a task and somewhere to report. */
	if (task == NULL || registers == NULL)
		return -1;
	area = task_current_fpregs(task);

	registers->control = (uint16_t)(area[0] | ((uint16_t)area[1] << 8));
	registers->status = (uint16_t)(area[2] | ((uint16_t)area[3] << 8));
	registers->tag = (uint16_t)area[4];
	registers->opcode = (uint16_t)(area[6] | ((uint16_t)area[7] << 8));
	hal_memcpy(&registers->instruction_pointer, area + 8, 8U);
	hal_memcpy(&registers->data_pointer, area + 16, 8U);
	hal_memcpy(registers->stack, area + 32, sizeof(registers->stack));

	/* Succeeded. */
	return 0;
}

/*
 * Writes the x87 state of a task.
 */
int
hal_task_set_user_fpregs(
	hal_task_t handle,
	const struct hal_fpregs *registers)
{
	struct amd64_task *task;
	uint8_t *area;

	task = handle;

	/* Requires a task and something to write. */
	if (task == NULL || registers == NULL)
		return -1;
	area = task_current_fpregs(task);

	area[0] = (uint8_t)(registers->control & 0xffU);
	area[1] = (uint8_t)((registers->control >> 8) & 0xffU);
	area[2] = (uint8_t)(registers->status & 0xffU);
	area[3] = (uint8_t)((registers->status >> 8) & 0xffU);
	area[4] = (uint8_t)(registers->tag & 0xffU);
	area[6] = (uint8_t)(registers->opcode & 0xffU);
	area[7] = (uint8_t)((registers->opcode >> 8) & 0xffU);
	hal_memcpy(area + 8, &registers->instruction_pointer, 8U);
	hal_memcpy(area + 16, &registers->data_pointer, 8U);
	hal_memcpy(area + 32, registers->stack, sizeof(registers->stack));
	task_reload_fpregs(task, area);

	/* Succeeded. */
	return 0;
}

/*
 * Reads the vector state of a task.
 */
int
hal_task_get_user_vregs(
	hal_task_t handle,
	struct hal_vregs *registers)
{
	struct amd64_task *task;
	const uint8_t *area;

	task = handle;

	/* Requires a task and somewhere to report. */
	if (task == NULL || registers == NULL)
		return -1;
	area = task_current_fpregs(task);

	hal_memcpy(&registers->control, area + 24, 4U);
	hal_memcpy(&registers->control_mask, area + 28, 4U);
	hal_memcpy(registers->xmm, area + 160, sizeof(registers->xmm));

	/* Succeeded. */
	return 0;
}

/*
 * Writes the vector state of a task.
 */
int
hal_task_set_user_vregs(
	hal_task_t handle,
	const struct hal_vregs *registers)
{
	struct amd64_task *task;
	uint8_t *area;
	uint32_t mask;
	uint32_t control;

	task = handle;

	/* Requires a task and something to write. */
	if (task == NULL || registers == NULL)
		return -1;
	area = task_current_fpregs(task);

	/*
	 * A control word with a bit the processor does not implement makes
	 * the reload fault, so only the bits this processor published as
	 * writable are taken.
	 */
	hal_memcpy(&mask, area + 28, 4U);
	if (mask == 0U)
		mask = 0x0000ffbfU;
	control = registers->control & mask;
	hal_memcpy(area + 24, &control, 4U);
	hal_memcpy(area + 160, registers->xmm, sizeof(registers->xmm));
	task_reload_fpregs(task, area);

	/* Succeeded. */
	return 0;
}

/*
 * Asks that a task execute one instruction and then trap.
 */
int
hal_task_set_single_step(
	hal_task_t handle,
	int enable)
{
	struct amd64_interrupt_frame *frame;

	/* Requires a task with a user frame. */
	frame = task_user_frame(handle);
	if (frame == NULL)
		return -1;

	/* Records the request in the flags the task returns with. */
	if (enable)
		frame->rflags |= AMD64_RFLAGS_TRAP;
	else
		frame->rflags &= ~AMD64_RFLAGS_TRAP;

	/* Succeeded. */
	return 0;
}

/*
 * Reports whether a task was asked to execute one instruction.
 */
int
hal_task_get_single_step(
	hal_task_t handle,
	int *enable)
{
	const struct amd64_interrupt_frame *frame;

	/* Requires a task with a user frame and somewhere to report. */
	if (enable == NULL)
		return -1;
	frame = task_user_frame(handle);
	if (frame == NULL)
		return -1;
	*enable = (frame->rflags & AMD64_RFLAGS_TRAP) != 0ULL ? 1 : 0;

	/* Succeeded. */
	return 0;
}

/*
 * Hardware debug points
 *
 * Four registers hold an address each, and one control word says for each
 * of them what to watch for and how many bytes.  The same four serve
 * instruction and data points, which is why a set is accepted or refused
 * as a whole.
 */

/* Control word: the local enable of one point, and its field. */
#define AMD64_DR7_LOCAL(index)		(1ULL << (2U * (index)))
#define AMD64_DR7_FIELD(index)		(16U + 4U * (index))
#define AMD64_DR7_RESERVED		0x0000000000000400ULL
#define AMD64_DR7_EXACT			0x0000000000000300ULL

/* What to watch for, as the control word spells it. */
#define AMD64_DR7_RW_EXECUTE		0ULL
#define AMD64_DR7_RW_WRITE		1ULL
#define AMD64_DR7_RW_ACCESS		3ULL

/* Status word: one bit per point, and the one-instruction bit. */
#define AMD64_DR6_POINT(index)		(1ULL << (index))
#define AMD64_DR6_STEP			0x0000000000004000ULL
#define AMD64_DR6_WRITABLE		0x000000000000e00fULL

static uint64_t
read_debug_status(
	void)
{
	uint64_t value;

	__asm__ volatile("movq %%dr6, %0" : "=r"(value));

	/* Returns the status word. */
	return value;
}

static void
write_debug_status(
	uint64_t value)
{
	__asm__ volatile("movq %0, %%dr6" : : "r"(value));
}

/*
 * Writes a task's debug points into the processor.
 */
void
amd64_debug_load(
	struct amd64_task *task)
{
	uintptr_t address[HAL_DEBUG_POINT_MAX];
	unsigned index;

	/* Disables every point before the addresses move under them. */
	__asm__ volatile("movq %0, %%dr7" : : "r"(0ULL));

	/* Leaves the registers disabled for a task that uses none. */
	if (task == NULL || task->debug_point_count == 0U)
		return;

	/* Process each element required by the operation. */
	for (index = 0; index < HAL_DEBUG_POINT_MAX; index++) {
		address[index] = index < task->debug_point_count
		    ? task->debug_points[index].address : 0;
	}
	__asm__ volatile("movq %0, %%dr0" : : "r"(address[0]));
	__asm__ volatile("movq %0, %%dr1" : : "r"(address[1]));
	__asm__ volatile("movq %0, %%dr2" : : "r"(address[2]));
	__asm__ volatile("movq %0, %%dr3" : : "r"(address[3]));

	/* Publishes the points by enabling them together. */
	__asm__ volatile("movq %0, %%dr7" : : "r"(task->debug_control));
}

/*
 * Reports which of the running task's debug points the processor stopped
 * on, and clears the status so that the next stop is unambiguous.
 */
int
amd64_debug_hit(
	uintptr_t *address,
	int *mode)
{
	uint64_t status;
	unsigned index;

	status = read_debug_status();
	write_debug_status(status & ~AMD64_DR6_WRITABLE);

	/* Requires a running task to attribute the stop to. */
	if (running_task == NULL)
		return 0;

	/* Process each element required by the operation. */
	for (index = 0; index < running_task->debug_point_count; index++) {
		/* Handles the status condition. */
		if ((status & AMD64_DR6_POINT(index)) == 0ULL)
			continue;
		if (address != NULL)
			*address = running_task->debug_points[index].address;

		/* Reports the kind the point was watching for. */
		if (mode != NULL) {
			switch (running_task->debug_points[index].kind) {
			case HAL_DEBUG_KIND_EXECUTE:
				*mode = HAL_TRAP_MODE_EXEC;
				break;
			case HAL_DEBUG_KIND_WRITE:
				*mode = HAL_TRAP_MODE_WRITE;
				break;
			default:
				*mode = HAL_TRAP_MODE_READ;
				break;
			}
		}

		/* Reports that a point matched. */
		return 1;
	}

	/* Reports that no point matched; the stop was the step. */
	return 0;
}

/*
 * Builds the control word one point contributes, or reports that this
 * processor cannot express the point.
 */
static int
debug_point_field(
	const struct hal_debug_point *point,
	unsigned index,
	uint64_t *field)
{
	uint64_t watch;
	uint64_t length;

	/* Rejects a kind this processor does not implement. */
	switch (point->kind) {
	case HAL_DEBUG_KIND_EXECUTE:
		watch = AMD64_DR7_RW_EXECUTE;
		break;
	case HAL_DEBUG_KIND_WRITE:
		watch = AMD64_DR7_RW_WRITE;
		break;
	case HAL_DEBUG_KIND_ACCESS:
		watch = AMD64_DR7_RW_ACCESS;
		break;
	default:
		return -1;
	}

	/*
	 * An instruction point covers the one instruction at the address.
	 * A data point covers one, two, four or eight bytes, and the
	 * address is a multiple of that length.
	 */
	if (watch == AMD64_DR7_RW_EXECUTE) {
		if (point->length != 1U)
			return -1;
		length = 0ULL;
	} else {
		switch (point->length) {
		case 1U: length = 0ULL; break;
		case 2U: length = 1ULL; break;
		case 8U: length = 2ULL; break;
		case 4U: length = 3ULL; break;
		default: return -1;
		}
		if ((point->address & (uintptr_t)(point->length - 1U)) != 0)
			return -1;
	}
	*field = AMD64_DR7_LOCAL(index) |
	    ((watch | (length << 2)) << AMD64_DR7_FIELD(index));

	/* Succeeded. */
	return 0;
}

/*
 * Gives a task the complete set of debug points it is to run with.
 */
int
hal_task_set_debug_points(
	hal_task_t handle,
	const struct hal_debug_point *points,
	unsigned count)
{
	struct amd64_task *task;
	uint64_t control;
	uint64_t field;
	unsigned index;

	task = handle;

	/* Requires a task, and a set this processor has registers for. */
	if (task == NULL || count > HAL_DEBUG_POINT_MAX)
		return -1;
	if (count != 0U && points == NULL)
		return -1;

	/* Builds the whole control word before any of it is published. */
	control = count != 0U ? (AMD64_DR7_RESERVED | AMD64_DR7_EXACT) : 0ULL;

	/* Process each element required by the operation. */
	for (index = 0; index < count; index++) {
		/* Handles a point this processor cannot express. */
		if (debug_point_field(&points[index], index, &field) != 0)
			return -1;
		control |= field;
	}

	/* Records the accepted set. */
	for (index = 0; index < count; index++)
		task->debug_points[index] = points[index];
	task->debug_point_count = count;
	task->debug_control = control;

	/* A running task takes them immediately. */
	if (task == running_task)
		amd64_debug_load(task);

	/* Succeeded. */
	return 0;
}

/*
 * Reports the debug points a task runs with.
 */
int
hal_task_get_debug_points(
	hal_task_t handle,
	struct hal_debug_point *points,
	unsigned capacity,
	unsigned *count)
{
	struct amd64_task *task;
	unsigned index;

	task = handle;

	/* Requires a task and somewhere to report the number. */
	if (task == NULL || count == NULL)
		return -1;

	/* Requires room for every point the task has. */
	if (task->debug_point_count > capacity)
		return -1;
	if (task->debug_point_count != 0U && points == NULL)
		return -1;

	/* Process each element required by the operation. */
	for (index = 0; index < task->debug_point_count; index++)
		points[index] = task->debug_points[index];
	*count = task->debug_point_count;

	/* Succeeded. */
	return 0;
}
