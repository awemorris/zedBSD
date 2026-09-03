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
void
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
		HAL_FATAL("hal_task_init twice");

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
	task = hal_malloc(sizeof(*task));
	if (task == NULL)
		HAL_FATAL("initial amd64 task allocation failed");

	/* Initializes the task as a running system-space context. */
	hal_memset(task, 0, sizeof(*task));
	task->space = HAL_SPACE_SYS;
	task->run_cpu = (int)cpu;
	task->target_cpu = cpu;
	fpregs = task_fpregs(task);
	hal_memcpy(fpregs, initial_fpregs, sizeof(initial_fpregs));
	tasklist_add(task);
	running_task = task;

	/* Verifies FP context switching only on the BSP. */
	if (run_selftest)
		xmm_context_selftest();
}

/*
 * Initializes BSP task state and runs the context self-test.
 */
void
hal_task_init(
	void)
{
	/* Establishes the BSP's initial task. */
	amd64_task_init_cpu(1);
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
	task = hal_malloc(sizeof(*task));
	if (task == NULL)
		return NULL;
	hal_memset(task, 0, sizeof(*task));

	/* Allocates the separately owned aligned kernel stack. */
	task->sys_stack_allocation = hal_malloc(AMD64_SYS_STACK_SIZE + 15U);
	if (task->sys_stack_allocation == NULL) {
		hal_free(task);
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
	resume[6] = 0x202U;
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
	hal_page_switch_space(new_space);

	/* Reports a completed exec transition. */
	return 0;
}

/*
 * Reports the current task's active user stack pointer.
 */
uintptr_t
hal_task_user_stack(
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
 * Copies the current task's active user context.
 */
int
hal_task_user_context(
	struct hal_user_context *context)
{
	struct amd64_interrupt_frame *frame;

	/* Resolves the active frame when a current task exists. */
	if (running_task != NULL)
		frame = running_task->active_user_frame;
	else
		frame = NULL;

	/* Requires a destination and ring-three frame. */
	if (frame == NULL || context == NULL || (frame->cs & 3U) != 3U)
		return -1;

	/* Copies the generic user-context fields. */
	context->pc = (uintptr_t)frame->rip;
	context->stack_pointer = (uintptr_t)frame->rsp;
	context->return_value = (intptr_t)frame->rax;

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
		hal_free(task->sys_stack_allocation);

	/* Releases the task record last. */
	hal_free(task);
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

	/* Transfers running ownership in release order. */
	__atomic_store_n(&from->run_cpu, -1, __ATOMIC_RELEASE);
	current_cpu = hal_cpu_current();
	__atomic_store_n(&to->run_cpu, (int)current_cpu, __ATOMIC_RELEASE);

	/* Switches task identity, address space, and architectural TLS. */
	from->tls = (uintptr_t)asm_read_msr(AMD64_MSR_FS_BASE);
	running_task = to;
	hal_page_switch_space(to->space);
	asm_write_msr(AMD64_MSR_FS_BASE, (uint64_t)to->tls);

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
	/* Returns the GS-selected current task. */
	return running_task;
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

	/* Builds the register prefix consumed by the dispatch assembly. */
	*--stack = 0x202U;
	*--stack = 0;
	*--stack = 0;
	*--stack = 0;
	*--stack = 0;
	*--stack = 0;
	*--stack = 0;
	task->resume_rsp = (uintptr_t)stack;
}
