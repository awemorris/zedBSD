#include <hal/hal.h>
#include "asm.h"
#include "task.h"
#include "int.h"

static struct arm64_task *task_list;
static struct arm64_task *running_task;
static uint8 initial_fpregs[528] __attribute__((aligned(16)));
static uint32 task_count;
static size_t task_stack_bytes;

static void task_add(struct arm64_task *t)
{struct arm64_task **p=&task_list;while(*p)p=&(*p)->next;*p=t;t->next=NULL;task_count++;}
static void task_del(struct arm64_task *t)
{struct arm64_task **p=&task_list;while(*p&&*p!=t)p=&(*p)->next;if(*p){*p=t->next;if(task_count)task_count--;}}

void hal_task_init(void)
{
	struct arm64_task *t;
	if(running_task)HAL_FATAL("hal_task_init twice");
	arm64_fp_save(initial_fpregs);
	t=hal_malloc(sizeof(*t));if(!t)HAL_FATAL("initial arm64 task allocation failed");
	hal_memset(t,0,sizeof(*t));t->space=HAL_SPACE_SYS;t->run_cpu=0;
	hal_memcpy(t->fpregs,initial_fpregs,sizeof(initial_fpregs));task_add(t);running_task=t;
}

static void build_stack(struct arm64_task *t,void(*start)(void *),void *arg,void *user_sp)
{
	uint64 *sp=(uint64 *)((uintptr_t)t->sys_stack+ARM64_SYS_STACK_SIZE);
	sp-=12;hal_memset(sp,0,12*sizeof(*sp));
	sp[0]=(uintptr_t)start;sp[1]=t->space==HAL_SPACE_SYS?(uintptr_t)arg:(uintptr_t)user_sp;
	sp[11]=(uintptr_t)(t->space==HAL_SPACE_SYS?arm64_kernel_task_entry:arm64_user_task_entry);
	t->resume_sp=(uintptr_t)sp;
}

hal_task_t hal_task_create(hal_space_t space,void(*start)(void *),void *arg,void *user_sp)
{
	struct arm64_task *t;if(!start||((space==HAL_SPACE_SYS)!=(user_sp==NULL)))return NULL;
	t=hal_malloc(sizeof(*t));if(!t)return NULL;hal_memset(t,0,sizeof(*t));
	t->sys_stack_allocation=hal_malloc(ARM64_SYS_STACK_SIZE+15);if(!t->sys_stack_allocation){hal_free(t);return NULL;}
	t->sys_stack=(void *)(((uintptr_t)t->sys_stack_allocation+15)&~(uintptr_t)15);
	t->space=space;t->run_cpu=-1;hal_memcpy(t->fpregs,initial_fpregs,sizeof(initial_fpregs));
	build_stack(t,start,arg,user_sp);task_stack_bytes+=ARM64_SYS_STACK_SIZE;task_add(t);return t;
}

void arm64_task_enter_user_frame(void *f)
{if(running_task)running_task->active_user_frame=f;}
void arm64_task_leave_user_frame(void)
{if(running_task)running_task->active_user_frame=NULL;}

hal_task_t hal_task_fork_current(hal_space_t child_space,intptr_t child_result)
{
	struct arm64_exception_frame *source,*copy;
	struct arm64_task *child;
	uint64 *resume;
	if(!running_task||child_space==HAL_SPACE_SYS||!running_task->active_user_frame)return NULL;
	source=running_task->active_user_frame;
	arm64_fp_save(running_task->fpregs);
	child=hal_task_create(child_space,(void(*)(void *))(uintptr_t)source->elr,
		NULL,(void *)(uintptr_t)source->user_sp);
	if(!child)return NULL;
	resume=(uint64 *)((uintptr_t)child->sys_stack+ARM64_SYS_STACK_SIZE-
		sizeof(*source)-12U*sizeof(uint64));
	hal_memset(resume,0,12U*sizeof(uint64));
	resume[11]=(uintptr_t)arm64_user_frame_entry;
	copy=(struct arm64_exception_frame *)(resume+12);
	*copy=*source;
	copy->x[0]=(uint64)child_result;
	child->resume_sp=(uintptr_t)resume;
	child->tls=running_task->tls;
	hal_memcpy(child->fpregs,running_task->fpregs,sizeof(child->fpregs));
	return child;
}

int hal_task_exec_current(hal_space_t new_space,uintptr_t entry,uintptr_t user_sp)
{
	struct arm64_exception_frame *frame;
	if(!running_task||new_space==HAL_SPACE_SYS||!running_task->active_user_frame||
		!entry||!user_sp)return -1;
	frame=running_task->active_user_frame;
	hal_memset(frame,0,sizeof(*frame));
	frame->elr=(uint64)entry;
	frame->user_sp=(uint64)user_sp;
	running_task->space=new_space;
	running_task->tls=0;
	running_task->signal_depth=0;running_task->signal_token=0;
	hal_memcpy(running_task->fpregs,initial_fpregs,sizeof(initial_fpregs));
	arm64_fp_restore(running_task->fpregs);
	hal_page_switch_space(new_space);
	return 0;
}
uintptr_t hal_task_user_stack(void){struct arm64_exception_frame*f=running_task!=NULL?running_task->active_user_frame:NULL;return f!=NULL?(uintptr_t)f->user_sp:0;}
int hal_task_signal_enter(uintptr_t h,uintptr_t sp,int sig,uintptr_t rest,uint32_t token){struct arm64_exception_frame*f=running_task!=NULL?running_task->active_user_frame:NULL;if(f==NULL||running_task->signal_depth!=0||h==0||sp==0||token==0)return-1;running_task->signal_frame=*f;running_task->signal_token=token;running_task->signal_depth=1;f->elr=h;f->user_sp=sp;f->x[0]=(uint64)sig;f->x[30]=rest;return 0;}
int hal_task_signal_return(uint32_t token,intptr_t*value){struct arm64_exception_frame*f=running_task!=NULL?running_task->active_user_frame:NULL;if(f==NULL||value==NULL||running_task->signal_depth!=1||token==0||token!=running_task->signal_token)return-1;*f=running_task->signal_frame;*value=(intptr_t)f->x[0];running_task->signal_depth=0;running_task->signal_token=0;return 0;}
void hal_task_destroy(hal_task_t h)
{
	struct arm64_task *t=h;if(!t)return;if(t==running_task)HAL_FATAL("destroy current arm64 task");task_del(t);
	if(t->sys_stack){task_stack_bytes-=ARM64_SYS_STACK_SIZE;hal_free(t->sys_stack_allocation);}hal_free(t);
}
void hal_task_context_switch(hal_task_t h)
{
	struct arm64_task *to=h,*from=running_task;uint64 tls;
	if(!to||!from)HAL_FATAL("invalid arm64 task switch");
	if(to==from)return;
	arm64_fp_save(from->fpregs);__asm__ volatile("mrs %0,tpidr_el0":"=r"(tls));from->tls=(uintptr_t)tls;
	running_task=to;hal_page_switch_space(to->space);arm64_fp_restore(to->fpregs);
	__asm__ volatile("msr tpidr_el0,%0"::"r"((uint64)to->tls));
	asm_task_dispatch(&from->resume_sp,&to->resume_sp);
}
void arm64_task_returned(void){HAL_FATAL("arm64 task returned");for(;;)arm64_wfi();}
void hal_cpu_idle(void){arm64_irq_unmask();arm64_wfi();arm64_irq_mask();}
hal_task_t hal_task_get_current(void){return running_task;}
void hal_task_set_tls(hal_task_t h,uintptr_t v){struct arm64_task*t=h;if(t){t->tls=v;if(t==running_task)__asm__ volatile("msr tpidr_el0,%0"::"r"((uint64)v));}}
uintptr_t hal_task_get_tls(hal_task_t h){return h?((struct arm64_task*)h)->tls:0;}
void hal_task_set_private(hal_task_t h,void*p){if(h)((struct arm64_task*)h)->private_data=p;}
void *hal_task_get_private(hal_task_t h){return h?((struct arm64_task*)h)->private_data:NULL;}
hal_space_t hal_task_get_space(hal_task_t h){return h?((struct arm64_task*)h)->space:HAL_SPACE_SYS;}
void hal_arm64_task_memory_stats(uint32*c,size_t*b){if(c)*c=task_count;if(b)*b=task_stack_bytes;}

static uint8 self_stack[4096] __attribute__((aligned(16)));
static uintptr_t self_main_sp,self_task_sp;
static unsigned self_count;
static void self_task(void)
{
	self_count=1;asm_task_dispatch(&self_task_sp,&self_main_sp);
	self_count=2;asm_task_dispatch(&self_task_sp,&self_main_sp);
	for(;;)arm64_wfi();
}
void arm64_context_selftest(void)
{
	uint64 *sp=(uint64 *)(self_stack+sizeof(self_stack));sp-=12;hal_memset(sp,0,12*sizeof(*sp));sp[11]=(uintptr_t)self_task;self_task_sp=(uintptr_t)sp;
	asm_task_dispatch(&self_main_sp,&self_task_sp);if(self_count!=1)HAL_FATAL("context selftest first switch");
	asm_task_dispatch(&self_main_sp,&self_task_sp);if(self_count!=2)HAL_FATAL("context selftest second switch");
	hal_puts("ARM64 CONTEXT PASS\n");
}
