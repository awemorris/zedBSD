/* amd64 opaque CPU tasks. */
#include <hal/hal.h>
#include "task.h"
#include "defs.h"
#include "asm.h"
#include "descriptor.h"
#include "int.h"
#include "irq.h"
#include "percpu.h"

static struct amd64_task *task_list;
#define running_task (amd64_percpu_current()->running_task)
static uint8 initial_fpregs[512] __attribute__((aligned(16)));
static uint32 task_count;
static size_t task_stack_bytes;
static hal_task_t xmm_selftest_main;
static hal_task_t xmm_selftest_task;
static volatile unsigned xmm_selftest_stage;
static volatile unsigned initial_fpregs_ready;
static volatile unsigned task_registry_lock;
#define AMD64_SYSCALL_INSTRUCTION_SIZE 2U
static const uint8 xmm_main_pattern[16] = {
	0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
	0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01
};
static const uint8 xmm_task_pattern[16] = {
	0xa5, 0x5a, 0xc3, 0x3c, 0x96, 0x69, 0xf0, 0x0f,
	0x12, 0x21, 0x34, 0x43, 0x56, 0x65, 0x78, 0x87
};

static int
xmm_equal(const uint8 *left, const uint8 *right)
{
	unsigned i;

	for (i = 0; i < 16U; i++)
		if (left[i] != right[i])
			return 0;
	return 1;
}

static void
xmm_selftest_entry(void *argument)
{
	uint8 observed[16];

	(void)argument;
	amd64_xmm_load(xmm_task_pattern);
	xmm_selftest_stage = 1;
	hal_task_context_switch(xmm_selftest_main);
	amd64_xmm_store(observed);
	if (!xmm_equal(observed, xmm_task_pattern))
		HAL_FATAL("amd64 XMM task context corruption");
	xmm_selftest_stage = 2;
	hal_task_context_switch(xmm_selftest_main);
	HAL_FATAL("amd64 XMM self-test task resumed unexpectedly");
}

static void
xmm_context_selftest(void)
{
	uint8 observed[16];

	xmm_selftest_main = running_task;
	xmm_selftest_task = hal_task_create(HAL_SPACE_SYS, xmm_selftest_entry,
	    NULL, NULL);
	if (xmm_selftest_task == NULL)
		HAL_FATAL("amd64 XMM self-test task allocation failed");
	amd64_xmm_load(xmm_main_pattern);
	hal_task_context_switch(xmm_selftest_task);
	amd64_xmm_store(observed);
	if (xmm_selftest_stage != 1 || !xmm_equal(observed, xmm_main_pattern))
		HAL_FATAL("amd64 XMM initial task context corruption");
	hal_task_context_switch(xmm_selftest_task);
	if (xmm_selftest_stage != 2)
		HAL_FATAL("amd64 XMM self-test did not complete");
	hal_task_destroy(xmm_selftest_task);
	xmm_selftest_task = NULL;
	xmm_selftest_main = NULL;
	hal_puts("A64 XMM CONTEXT PASS\n");
}

static void *
task_fpregs(struct amd64_task *task)
{
	return (void *)(((uintptr_t)task->fpregs + 15U) & ~(uintptr_t)15U);
}

static void
tasklist_add(struct amd64_task *task)
{
	bool enabled = hal_irq_disable();
	struct amd64_task **link = &task_list;
	while (__atomic_exchange_n(&task_registry_lock, 1U,
	    __ATOMIC_ACQUIRE) != 0)
		__asm__ volatile("pause");
	while (*link != NULL) link = &(*link)->next;
	*link = task;
	task->next = NULL;
	task_count++;
	if (task->sys_stack != NULL)
		task_stack_bytes += AMD64_SYS_STACK_SIZE;
	__atomic_store_n(&task_registry_lock, 0U, __ATOMIC_RELEASE);
	if (enabled) hal_irq_enable();
}

static void
tasklist_del(struct amd64_task *task)
{
	bool enabled = hal_irq_disable();
	struct amd64_task **link = &task_list;
	while (__atomic_exchange_n(&task_registry_lock, 1U,
	    __ATOMIC_ACQUIRE) != 0)
		__asm__ volatile("pause");
	while (*link != NULL && *link != task) link = &(*link)->next;
	if (*link == task) {
		*link = task->next;
		if (task_count != 0) task_count--;
		if (task->sys_stack != NULL &&
		    task_stack_bytes >= AMD64_SYS_STACK_SIZE)
			task_stack_bytes -= AMD64_SYS_STACK_SIZE;
	}
	__atomic_store_n(&task_registry_lock, 0U, __ATOMIC_RELEASE);
	if (enabled) hal_irq_enable();
}

void
amd64_task_init_cpu(int run_selftest)
{
	struct amd64_task *task;
	hal_cpu_id_t cpu = hal_cpu_current();

	if (running_task != NULL) HAL_FATAL("hal_task_init twice");
	if (run_selftest) {
		__asm__ volatile("fninit; fxsave64 %0" : "=m"(initial_fpregs));
		__atomic_store_n(&initial_fpregs_ready, 1U, __ATOMIC_RELEASE);
	} else if (__atomic_load_n(&initial_fpregs_ready,
	    __ATOMIC_ACQUIRE) == 0) {
		HAL_FATAL("amd64 AP task before BSP task initialization");
	}
	task = hal_malloc(sizeof(*task));
	if (task == NULL) HAL_FATAL("initial amd64 task allocation failed");
	hal_memset(task, 0, sizeof(*task));
	task->space = HAL_SPACE_SYS;
	task->run_cpu = (int)cpu;
	task->target_cpu = cpu;
	hal_memcpy(task_fpregs(task), initial_fpregs, sizeof(initial_fpregs));
	tasklist_add(task);
	running_task = task;
	if (run_selftest)
		xmm_context_selftest();
}

void
hal_task_init(void)
{
	amd64_task_init_cpu(1);
}

static void
build_initial_stack(struct amd64_task *task, void (*start)(void *),
	void *arg, void *user_sp)
{
	uintptr_t *sp = (uintptr_t *)((uintptr_t)task->sys_stack +
	    AMD64_SYS_STACK_SIZE);
	if (task->space == HAL_SPACE_SYS) {
		*--sp = (uintptr_t)arg;
		*--sp = (uintptr_t)start;
		*--sp = (uintptr_t)amd64_kernel_task_entry;
	} else {
		*--sp = SEG_USER_DATA | 3U;
		*--sp = (uintptr_t)user_sp;
		*--sp = 0x202U;
		*--sp = SEG_USER_CODE | 3U;
		*--sp = (uintptr_t)start;
		*--sp = (uintptr_t)amd64_user_task_entry;
	}
	*--sp = 0x202U;
	*--sp = 0;
	*--sp = 0;
	*--sp = 0;
	*--sp = 0;
	*--sp = 0;
	*--sp = 0;
	task->resume_rsp = (uintptr_t)sp;
}

hal_task_t
hal_task_create(hal_space_t space, void (*start)(void *), void *arg,
	void *user_stack_pointer)
{
	struct amd64_task *task;
	if (start == NULL ||
	    ((space == HAL_SPACE_SYS) != (user_stack_pointer == NULL))) return NULL;
	task = hal_malloc(sizeof(*task));
	if (task == NULL) return NULL;
	hal_memset(task, 0, sizeof(*task));
	task->sys_stack_allocation = hal_malloc(AMD64_SYS_STACK_SIZE + 15U);
	if (task->sys_stack_allocation == NULL) { hal_free(task); return NULL; }
	task->sys_stack = (void *)(((uintptr_t)task->sys_stack_allocation + 15U) &
	    ~(uintptr_t)15U);
	task->space = space;
	task->run_cpu = -1;
	task->target_cpu = hal_cpu_current();
	hal_memcpy(task_fpregs(task), initial_fpregs, sizeof(initial_fpregs));
	build_initial_stack(task, start, arg, user_stack_pointer);
	tasklist_add(task);
	return task;
}

void
amd64_task_enter_user_frame(void *frame)
{
	if (running_task != NULL)
		running_task->active_user_frame = frame;
}

void
amd64_task_leave_user_frame(void)
{
	if (running_task != NULL)
		running_task->active_user_frame = NULL;
}

hal_task_t
hal_task_fork_current(hal_space_t child_space, intptr_t child_result)
{
	struct amd64_interrupt_frame *source, *copy;
	struct amd64_task *child;
	uintptr_t *resume;

	if (running_task == NULL || child_space == HAL_SPACE_SYS ||
	    running_task->active_user_frame == NULL)
		return NULL;
	source = running_task->active_user_frame;
	if ((source->cs & 3U) != 3U)
		return NULL;
	child = hal_task_create(child_space, (void (*)(void *))source->rip,
	    NULL, (void *)(uintptr_t)source->rsp);
	if (child == NULL)
		return NULL;
	resume = (uintptr_t *)((uintptr_t)child->sys_stack +
	    AMD64_SYS_STACK_SIZE - sizeof(*source) - 8U * sizeof(uintptr_t));
	hal_memset(resume, 0, 8U * sizeof(uintptr_t));
	resume[6] = 0x202U;
	resume[7] = (uintptr_t)amd64_user_frame_entry;
	copy = (struct amd64_interrupt_frame *)(resume + 8);
	*copy = *source;
	copy->rax = (uint64)child_result;
	child->resume_rsp = (uintptr_t)resume;
	child->tls = running_task->tls;
	/* The in-memory image is stale while this task is running. */
	__asm__ volatile("fxsave64 (%0)" : :
	    "r"(task_fpregs(running_task)) : "memory");
	hal_memcpy(task_fpregs(child), task_fpregs(running_task), 512U);
	return child;
}

int
hal_task_exec_validate(hal_space_t new_space, uintptr_t entry,
	uintptr_t user_stack_pointer)
{
	return running_task != NULL && new_space != HAL_SPACE_SYS &&
	    running_task->active_user_frame != NULL && entry != 0 &&
	    user_stack_pointer != 0 ? 0 : -1;
}

int
hal_task_exec_current(hal_space_t new_space, uintptr_t entry,
		      uintptr_t user_stack_pointer)
{
	struct amd64_interrupt_frame *frame;
	uint64 cs, ss, rflags;
	if (hal_task_exec_validate(new_space, entry, user_stack_pointer) != 0)
		return -1;
	frame = running_task->active_user_frame;
	cs = SEG_USER_CODE | 3U;
	ss = SEG_USER_DATA | 3U;
	rflags = 0x202U;
	hal_memset(frame, 0, sizeof(*frame));
	frame->rip = entry;
	frame->cs = cs;
	frame->rflags = rflags;
	frame->rsp = user_stack_pointer;
	frame->ss = ss;
	running_task->space = new_space;
	running_task->tls = 0;
	asm_write_msr(AMD64_MSR_FS_BASE, 0);
	running_task->signal_depth = 0;
	hal_memcpy(task_fpregs(running_task), initial_fpregs,
	    sizeof(initial_fpregs));
	__asm__ volatile("fxrstor64 (%0)" : :
	    "r"(task_fpregs(running_task)) : "memory");
	hal_page_switch_space(new_space);
	return 0;
}

uintptr_t hal_task_user_stack(void)
{
	struct amd64_interrupt_frame *f=running_task!=NULL?running_task->active_user_frame:NULL;
	return f!=NULL&&(f->cs&3U)==3U?(uintptr_t)f->rsp:0;
}
int hal_task_user_context(struct hal_user_context *context)
{
	struct amd64_interrupt_frame *f=running_task!=NULL?running_task->active_user_frame:NULL;
	if(f==NULL||context==NULL||(f->cs&3U)!=3U)return -1;
	context->pc=(uintptr_t)f->rip;context->stack_pointer=(uintptr_t)f->rsp;
	context->return_value=(intptr_t)f->rax;return 0;
}
int hal_task_signal_enter(uintptr_t h,uintptr_t sp,int sig,uintptr_t info,
	uintptr_t context,uintptr_t rest,uint32_t token)
{
	struct amd64_interrupt_frame *f=running_task!=NULL?running_task->active_user_frame:NULL;
	unsigned depth;
	(void)rest;
	if(f==NULL||running_task->signal_depth>=HAL_SIGNAL_NEST_MAX||h==0||sp==0||token==0)return -1;
	depth=running_task->signal_depth;running_task->signal_frame[depth]=*f;
	__asm__ volatile("fxsave64 (%0)" : :
	    "r"(running_task->signal_fpregs[depth]) : "memory");
	running_task->signal_token[depth]=token;running_task->signal_depth=depth+1U;
	f->rip=h;f->rsp=sp;f->rdi=(uint64)(uint32)sig;f->rsi=info;f->rdx=context;return 0;
}
int hal_task_signal_return(uint32_t token,intptr_t *value)
{
	struct amd64_interrupt_frame *f=running_task!=NULL?running_task->active_user_frame:NULL;
	unsigned depth;
	if(f==NULL||value==NULL||running_task->signal_depth==0||token==0)return -1;
	depth=running_task->signal_depth-1U;if(token!=running_task->signal_token[depth])return -1;
	*f=running_task->signal_frame[depth];*value=(intptr_t)f->rax;
	__asm__ volatile("fxrstor64 (%0)" : :
	    "r"(running_task->signal_fpregs[depth]) : "memory");
	running_task->signal_token[depth]=0;running_task->signal_depth=depth;return 0;
}
int hal_task_signal_restart(uint32_t token,uint32_t number,const uintptr_t args[HAL_SYSCALL_ARGS],intptr_t *value)
{
	struct amd64_interrupt_frame *f=running_task!=NULL?running_task->active_user_frame:NULL;
	unsigned depth;
	if(f==NULL||args==NULL||value==NULL||running_task->signal_depth==0||token==0)return-1;
	depth=running_task->signal_depth-1U;
	if(token!=running_task->signal_token[depth]||
	    running_task->signal_frame[depth].rip<
	    AMD64_SYSCALL_INSTRUCTION_SIZE)return-1;
	*f=running_task->signal_frame[depth];
	f->rip-=AMD64_SYSCALL_INSTRUCTION_SIZE;f->rax=number;
	f->rbx=args[0];f->rcx=args[1];f->rdx=args[2];f->rsi=args[3];
	f->rdi=args[4];f->rbp=args[5];*value=(intptr_t)number;
	__asm__ volatile("fxrstor64 (%0)" : :
	    "r"(running_task->signal_fpregs[depth]) : "memory");
	running_task->signal_token[depth]=0;running_task->signal_depth=depth;
	return 0;
}

void
hal_task_destroy(hal_task_t handle)
{
	struct amd64_task *task = handle;
	if (task == NULL) return;
	if (__atomic_load_n(&task->run_cpu, __ATOMIC_ACQUIRE) >= 0)
		HAL_FATAL("destroying running amd64 task");
	tasklist_del(task);
	if (task->sys_stack != NULL) {
		hal_free(task->sys_stack_allocation);
	}
	hal_free(task);
}

void
hal_task_context_switch(hal_task_t handle)
{
	struct amd64_task *to = handle, *from = running_task;
	if (to == NULL || from == NULL) HAL_FATAL("invalid amd64 task switch");
	if (to == from) return;
	if (__atomic_load_n(&to->run_cpu, __ATOMIC_ACQUIRE) >= 0)
		HAL_FATAL("amd64 HAL task already running");
	if (__atomic_load_n(&to->target_cpu, __ATOMIC_ACQUIRE) !=
	    hal_cpu_current())
		HAL_FATAL("amd64 HAL task resumed on wrong CPU");
	__atomic_store_n(&from->run_cpu, -1, __ATOMIC_RELEASE);
	__atomic_store_n(&to->run_cpu, (int)hal_cpu_current(),
	    __ATOMIC_RELEASE);
	from->tls = (uintptr_t)asm_read_msr(AMD64_MSR_FS_BASE);
	running_task = to;
	hal_page_switch_space(to->space);
	asm_write_msr(AMD64_MSR_FS_BASE, (uint64)to->tls);
	if (to->sys_stack != NULL)
		amd64_set_tss_rsp0((uintptr_t)to->sys_stack + AMD64_SYS_STACK_SIZE);
	__asm__ volatile("fxsave64 (%0)" : : "r"(task_fpregs(from)) : "memory");
	__asm__ volatile("fxrstor64 (%0)" : : "r"(task_fpregs(to)) : "memory");
	asm_task_dispatch(&from->resume_rsp, &to->resume_rsp);
}

void amd64_task_returned(void)
{
	HAL_FATAL("amd64 task returned");
	for (;;) asm_hlt();
}
void hal_cpu_idle(void) { __asm__ volatile("sti; hlt; cli" ::: "memory"); }
hal_task_t hal_task_get_current(void) { return running_task; }
void hal_task_set_tls(hal_task_t t, uintptr_t v)
{
	if (t != NULL) {
		((struct amd64_task *)t)->tls = v;
		if (t == running_task)
			asm_write_msr(AMD64_MSR_FS_BASE, (uint64)v);
	}
}
uintptr_t hal_task_get_tls(hal_task_t t)
{
	if (t == running_task)
		return (uintptr_t)asm_read_msr(AMD64_MSR_FS_BASE);
	return t != NULL ? ((struct amd64_task *)t)->tls : 0;
}
void hal_task_set_private(hal_task_t t, void *p)
{ if (t != NULL) ((struct amd64_task *)t)->private_data = p; }
void *hal_task_get_private(hal_task_t t)
{ return t != NULL ? ((struct amd64_task *)t)->private_data : NULL; }
hal_space_t hal_task_get_space(hal_task_t t)
{ return t != NULL ? ((struct amd64_task *)t)->space : HAL_SPACE_SYS; }
int hal_task_transfer(hal_task_t handle, hal_cpu_id_t target_cpu)
{
	struct amd64_task *task = handle;
	struct hal_cpu_mask ready;
	if (task == NULL || target_cpu >= hal_cpu_count()) return HAL_ERR_INVALID;
	if (__atomic_load_n(&task->run_cpu, __ATOMIC_ACQUIRE) >= 0)
		return HAL_ERR_BUSY;
	if (!amd64_irq_task_transferable(task))
		return HAL_ERR_BUSY;
	hal_cpu_ready_mask(&ready);
	if (!hal_cpu_mask_test(&ready, target_cpu)) return HAL_ERR_STATE;
	__atomic_store_n(&task->target_cpu, target_cpu, __ATOMIC_RELEASE);
	return HAL_OK;
}
void hal_amd64_task_memory_stats(uint32 *count, size_t *bytes)
{
	bool enabled = hal_irq_disable();
	while (__atomic_exchange_n(&task_registry_lock, 1U,
	    __ATOMIC_ACQUIRE) != 0)
		__asm__ volatile("pause");
	if (count != NULL) *count = task_count;
	if (bytes != NULL) *bytes = task_stack_bytes;
	__atomic_store_n(&task_registry_lock, 0U, __ATOMIC_RELEASE);
	if (enabled) hal_irq_enable();
}
