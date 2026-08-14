#include <hal/hal.h>
#include "defs.h"
#include "task.h"

static struct sparcv9_task*tasks,*running;static uint32 task_count;static size_t stack_bytes;
uintptr_t sparcv9_current_trap_sp;
struct sparcv9_window_context *sparcv9_current_user_windows;
static void add(struct sparcv9_task*t){struct sparcv9_task**p=&tasks;while(*p)p=&(*p)->next;*p=t;t->next=NULL;task_count++;}
static void remove_task(struct sparcv9_task*t){struct sparcv9_task**p=&tasks;while(*p&&*p!=t)p=&(*p)->next;if(*p){*p=t->next;if(task_count)task_count--;}}
void hal_task_init(void){struct sparcv9_task*t;if(running)HAL_FATAL("hal_task_init twice");t=hal_malloc(sizeof(*t));if(!t)HAL_FATAL("initial SPARC V9 task allocation failed");hal_memset(t,0,sizeof(*t));t->space=HAL_SPACE_SYS;add(t);running=t;}
static void build(struct sparcv9_task*t,void(*start)(void*),void*arg,void*user_sp){uintptr_t top=((uintptr_t)t->stack+SPARCV9_SYS_STACK_SIZE)&~15UL;uintptr_t sp=top-SPARCV9_STACK_BIAS-SPARCV9_STACK_FRAME_SIZE;struct sparcv9_window_context*c=&t->windows;hal_memset(c,0,sizeof(*c));c->cwp=0;c->cansave=6;c->cleanwin=7;c->window[0][0]=(uintptr_t)start;c->window[0][1]=t->space==HAL_SPACE_SYS?(uintptr_t)arg:(uintptr_t)user_sp;/* CWP+1 inputs overlap the initial window's outputs. */c->window[1][14]=sp;c->window[1][15]=(uintptr_t)(t->space==HAL_SPACE_SYS?sparcv9_kernel_task_entry:sparcv9_user_task_entry)-8U;t->trap_sp=top-SPARCV9_STACK_BIAS;}
hal_task_t hal_task_create(hal_space_t s,void(*start)(void*),void*arg,void*usp){struct sparcv9_task*t;if(!start||((s==HAL_SPACE_SYS)!=(usp==NULL)))return NULL;t=hal_malloc(sizeof(*t));if(!t)return NULL;hal_memset(t,0,sizeof(*t));t->stack_allocation=hal_malloc(SPARCV9_SYS_STACK_SIZE+15U);if(!t->stack_allocation){hal_free(t);return NULL;}t->stack=(void*)(((uintptr_t)t->stack_allocation+15U)&~15UL);t->space=s;build(t,start,arg,usp);stack_bytes+=SPARCV9_SYS_STACK_SIZE;add(t);return t;}
void hal_task_destroy(hal_task_t h){struct sparcv9_task*t=h;if(!t)return;if(t==running)HAL_FATAL("destroy current SPARC V9 task");remove_task(t);if(t->stack){stack_bytes-=SPARCV9_SYS_STACK_SIZE;hal_free(t->stack_allocation);}hal_free(t);}
static void save_trap_state(struct sparcv9_task*t){__asm__ volatile("rdpr %%tl,%0":"=r"(t->trap_level));if(t->trap_level>1)HAL_FATAL("SPARC V9 task switch above TL1");if(t->trap_level){__asm__ volatile("rdpr %%tpc,%0":"=r"(t->trap_pc));__asm__ volatile("rdpr %%tnpc,%0":"=r"(t->trap_next_pc));__asm__ volatile("rdpr %%tstate,%0":"=r"(t->trap_state));__asm__ volatile("rdpr %%tt,%0":"=r"(t->trap_type));}}
static void restore_trap_state(const struct sparcv9_task*t){__asm__ volatile("wrpr %0,0,%%tl"::"r"(t->trap_level):"memory");if(t->trap_level){__asm__ volatile("wrpr %0,0,%%tpc"::"r"(t->trap_pc));__asm__ volatile("wrpr %0,0,%%tnpc"::"r"(t->trap_next_pc));__asm__ volatile("wrpr %0,0,%%tstate"::"r"(t->trap_state));__asm__ volatile("wrpr %0,0,%%tt"::"r"(t->trap_type));}}
void hal_task_context_switch(hal_task_t h){struct sparcv9_task*to=h,*from=running;if(!to||!from)HAL_FATAL("invalid SPARC V9 task switch");if(to==from)return;save_trap_state(from);running=to;sparcv9_current_trap_sp=to->trap_sp;sparcv9_current_user_windows=&to->trap_windows;hal_page_switch_space(to->space);restore_trap_state(to);sparcv9_task_dispatch(&from->windows,&to->windows);}
void sparcv9_task_returned(void){HAL_FATAL("SPARC V9 task returned");for(;;)__asm__ volatile("nop");}
hal_task_t hal_task_get_current(void){return running;}
void hal_task_set_tls(hal_task_t h,uintptr_t v){if(h)((struct sparcv9_task*)h)->tls=v;}
uintptr_t hal_task_get_tls(hal_task_t h){return h?((struct sparcv9_task*)h)->tls:0;}
void hal_task_set_private(hal_task_t h,void*p){if(h)((struct sparcv9_task*)h)->private_data=p;}
void *hal_task_get_private(hal_task_t h){return h?((struct sparcv9_task*)h)->private_data:NULL;}
hal_space_t hal_task_get_space(hal_task_t h){return h?((struct sparcv9_task*)h)->space:HAL_SPACE_SYS;}
void hal_sparcv9_task_memory_stats(uint32*c,size_t*b){if(c)*c=task_count;if(b)*b=stack_bytes;}

static uint8 self_stack[4096]__attribute__((aligned(16)));static struct sparcv9_window_context main_context,test_context;static unsigned count;
static void self_task(void){count=1;sparcv9_task_dispatch(&test_context,&main_context);count=2;sparcv9_task_dispatch(&test_context,&main_context);for(;;)__asm__ volatile("nop");}
void sparcv9_context_selftest(void){uintptr_t top=(uintptr_t)(self_stack+sizeof(self_stack))&~15UL;hal_memset(&test_context,0,sizeof(test_context));test_context.cansave=6;test_context.cleanwin=7;test_context.window[1][14]=top-SPARCV9_STACK_BIAS-SPARCV9_STACK_FRAME_SIZE;test_context.window[1][15]=(uintptr_t)self_task-8U;sparcv9_task_dispatch(&main_context,&test_context);if(count!=1)HAL_FATAL("SPARC V9 context selftest first switch");sparcv9_task_dispatch(&main_context,&test_context);if(count!=2)HAL_FATAL("SPARC V9 context selftest second switch");hal_puts("SPARCV9 CONTEXT PASS\n");}
