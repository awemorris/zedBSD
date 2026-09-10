/* Actual generic fault entry: native diagnostics must not select policy. */
#include <hal/hal.h>
#include <kern/process.h>
#include <kern/thread.h>
#include <kern/signal.h>
#include <kern/user-probe.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static struct process process;
static struct thread thread;
static struct signal_info received;
static int enabled,entered,left,signo,vm_calls,vm_error;
static uint32_t required;
struct thread *thread_current(void) { return &thread; }
bool hal_irq_disable(void) { bool old=enabled;enabled=0;return old; }
void hal_irq_enable(void) { enabled=1; }
void sched_accounting_kernel_enter(void) { assert(!enabled);entered++; }
void sched_accounting_kernel_leave(void) { assert(!enabled);left++; }
void hal_fatal(const char *file,int line,const char *message)
{ fprintf(stderr,"%s:%d %s\n",file,line,message);abort(); }
int vmspace_fault(struct vmspace *vm,uintptr_t address,uint32_t access)
{ assert(vm==process.vmspace && enabled && address==0x2000);required=access;vm_calls++;return vm_error; }
int signal_send_process_info(struct process *p,int number,const struct signal_info *info)
{ assert(p==&process && enabled);signo=number;received=*info;return 0; }
int main(void)
{
 thread.proc=&process;process.vmspace=(struct vmspace *)&process;
 assert(kernel_user_fault_handler(HAL_TRAP_CAUSE_PAGE_FAULT,0x1000,0x2000,
     HAL_TRAP_MODE_WRITE,HAL_TRAP_DETAIL_NONE,6,0x10)==HAL_TRAP_RET_SUCCESS);
 assert(required==HAL_SPACE_WRITE && vm_calls==1 && !signo && !enabled);
 vm_error=EACCES;
 assert(kernel_user_fault_handler(HAL_TRAP_CAUSE_PAGE_FAULT,0x1000,0x2000,
     HAL_TRAP_MODE_READ,HAL_TRAP_DETAIL_NONE,0,2)==HAL_TRAP_RET_SUCCESS);
 assert(required==HAL_SPACE_READ && signo==SIGSEGV && received.code==SEGV_ACCERR);
 assert(kernel_user_fault_handler(HAL_TRAP_CAUSE_ILLEGAL_INSN,0x1000,0,
     HAL_TRAP_MODE_NONE,HAL_TRAP_DETAIL_NONE,14,0)==HAL_TRAP_RET_SUCCESS);
 assert(signo==SIGILL && received.address==0x1000 && vm_calls==2);
 assert(kernel_user_fault_handler(HAL_TRAP_CAUSE_ARITHMETIC,0x1000,0,
     HAL_TRAP_MODE_NONE,HAL_TRAP_DETAIL_INTOVF,14,0)==HAL_TRAP_RET_SUCCESS);
 assert(signo==SIGFPE && received.code==FPE_INTOVF);
 assert(kernel_user_fault_handler(HAL_TRAP_CAUSE_OTHER,0x1000,0,
     HAL_TRAP_MODE_NONE,HAL_TRAP_DETAIL_NONE,0,0)==HAL_TRAP_RET_SUCCESS);
 assert(signo==SIGBUS && vm_calls==2);
 assert(kernel_user_fault_handler(HAL_TRAP_CAUSE_PAGE_FAULT,0x1000,0x2000,
     HAL_TRAP_MODE_NONE,0,14,0)==HAL_TRAP_RET_FAILED && vm_calls==2);
 assert(entered==left && !enabled);
 enabled=1;
 assert(kernel_sys_fault_handler(HAL_TRAP_CAUSE_OTHER,0,0,HAL_TRAP_MODE_NONE,14,0)==HAL_TRAP_RET_FAILED);
 assert(enabled);
 user_probe_init();user_probe_syscall(123);
 assert(user_int_probe.count==1 && user_int_probe.eax==123 && user_int_probe.vector==0);
 puts("fixed fault entry: PASS normalized policy, diagnostic independence, IRQ balance");
 return 0;
}
