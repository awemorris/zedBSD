/* Actual C dispatch with mocked frame ownership and generic callbacks. */
#include <hal/hal.h>
#include "src/hal/arm64/int.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static int sys_calls,user_calls,syscalls,enters,leaves,returns,active;
static int seen_cause,seen_mode;
static uintptr_t seen_vector;
void arm64_task_enter_user_frame(struct arm64_exception_frame *f) { assert(f && !active);active=1;enters++; }
void arm64_task_leave_user_frame(void) { assert(active);active=0;leaves++; }
void kernel_user_return_handler(void) { assert(active);returns++; }
intptr_t kernel_syscall_handler(uint32_t n,const uintptr_t a[6])
{ assert(active && n==123 && a[0]==45);syscalls++;return 67; }
int kernel_sys_fault_handler(int cause,uintptr_t pc,uintptr_t addr,int mode,uintptr_t vector,uintptr_t raw)
{ (void)pc;(void)addr;(void)raw;assert(!active);sys_calls++;seen_cause=cause;seen_mode=mode;seen_vector=vector;return HAL_TRAP_RET_SUCCESS; }
int kernel_user_fault_handler(int cause,uintptr_t pc,uintptr_t addr,int mode,int detail,uintptr_t vector,uintptr_t raw)
{ (void)pc;(void)addr;(void)detail;(void)raw;assert(active);user_calls++;seen_cause=cause;seen_mode=mode;seen_vector=vector;return HAL_TRAP_RET_SUCCESS; }
int hal_printf(const char *format,...) { (void)format;return 0; }
void hal_fatal(const char *file,int line,const char *message)
{ fprintf(stderr,"%s:%d %s\n",file,line,message);abort(); }
#include "arm64-dispatch-extracted.h"
int main(void)
{
 struct arm64_exception_frame f;
 unsigned slot;
 memset(&f,0,sizeof(f));f.elr=0x1000;f.far=0x2000;
 for(slot=0;slot<16;slot++) {
  if(slot%4==1)continue;
  f.esr=((uint64_t)0x24<<26)|64;
  arm64_sync_handler(&f,slot);
  assert(seen_vector==slot && !active);
  assert(seen_cause==(slot%4==3?HAL_TRAP_CAUSE_MACHINE_CHECK:
      slot==0 || slot==4 || slot==8?HAL_TRAP_CAUSE_PAGE_FAULT:HAL_TRAP_CAUSE_OTHER));
  assert(seen_mode==(seen_cause==HAL_TRAP_CAUSE_PAGE_FAULT?HAL_TRAP_MODE_WRITE:HAL_TRAP_MODE_NONE));
 }
 f.esr=(uint64_t)0x3c<<26;arm64_sync_handler(&f,4);
 assert(seen_cause==HAL_TRAP_CAUSE_BREAKPOINT);
 f.esr=(uint64_t)0x15<<26;f.x[8]=123;f.x[0]=45;
 arm64_sync_handler(&f,8);assert(syscalls==1 && f.x[0]==67);
 arm64_sync_handler(&f,10);assert(syscalls==1 && seen_cause==HAL_TRAP_CAUSE_OTHER);
 assert(enters==leaves && enters==returns && sys_calls==7 && user_calls==7);
 puts("ARM64 dispatch: PASS native slots, async ESR isolation, frame ownership");
 return 0;
}
