#include <hal/hal.h>
#include "src/hal/m68k/exception.h"
#include "src/hal/m68k/frame-offsets.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static int active,enabled,seen_cause,seen_mode,seen_detail,seen_vector,enters,leaves;
void m68k_exception_dispatch(struct m68k_saved_frame *);
void m68k_task_enter_user_frame(struct m68k_saved_frame *f) { assert(f && !active && !enabled);active=1;enters++; }
void m68k_task_leave_user_frame(void) { assert(active);active=0;leaves++; }
void kernel_user_return_handler(void) { assert(active && !enabled); }
bool hal_irq_disable(void) { bool old=enabled;enabled=0;return old; }
int hal_printf(const char *format,...) { (void)format;return 0; }
void hal_fatal(const char *file,int line,const char *message) { fprintf(stderr,"%s:%d %s\n",file,line,message);abort(); }
uint32_t m68k030_test_user_read(uintptr_t a) { assert(a==0x2000);return M68K030_MMUSR_INVALID; }
uint32_t m68k030_test_user_write(uintptr_t a) { return m68k030_test_user_read(a); }
uint32_t m68k030_test_user_exec(uintptr_t a) { return m68k030_test_user_read(a); }
intptr_t kernel_syscall_handler(uint32_t n,const uintptr_t args[6]) { assert(active && n==123 && args[0]==45 && args[5]==67);return 89; }
int kernel_user_fault_handler(int cause,uintptr_t pc,uintptr_t address,int mode,int detail,uintptr_t vector,uintptr_t raw)
{ assert(active && pc==0x1000);(void)address;(void)raw;seen_cause=cause;seen_mode=mode;seen_detail=detail;seen_vector=vector;return HAL_TRAP_RET_SUCCESS; }
int kernel_sys_fault_handler(int cause,uintptr_t pc,uintptr_t address,int mode,uintptr_t vector,uintptr_t raw)
{ assert(!active && pc==0x1000);(void)address;(void)raw;seen_cause=cause;seen_mode=mode;seen_vector=vector;return HAL_TRAP_RET_SUCCESS; }
int main(void)
{
 struct m68k_saved_frame *f=calloc(1,sizeof(*f)+92);
 unsigned user,i;
 const unsigned vectors[]={3,4,5,9,12};
 const int causes[]={HAL_TRAP_CAUSE_ALIGNMENT,HAL_TRAP_CAUSE_ILLEGAL_INSN,HAL_TRAP_CAUSE_ARITHMETIC,HAL_TRAP_CAUSE_BREAKPOINT,HAL_TRAP_CAUSE_OTHER};
 assert(f);f->hardware[4]=0x10;
 for(user=0;user<2;user++)for(i=0;i<5;i++) {
  f->hardware[0]=user?0:0x20;f->hardware[7]=vectors[i]*4;enabled=1;
  m68k_exception_dispatch(f);
  assert(seen_cause==causes[i] && seen_vector==(int)vectors[i]);
  assert(seen_mode==HAL_TRAP_MODE_NONE && !active && enabled==!user);
  if(user && vectors[i]==5)assert(seen_detail==HAL_TRAP_DETAIL_INTDIV);
 }
 f->hardware[0]=0;f->hardware[7]=128;f->d[0]=123;f->d[1]=45;f->a[0]=67;
 m68k_exception_dispatch(f);assert(f->d[0]==89 && !active);
 f->hardware[6]=0xa0;f->hardware[7]=8;f->hardware[18]=0x20;
 for(i=0;i<3;i++) {
  f->hardware[10]=i==2?0:1;f->hardware[11]=i==0?0x40:0;
  m68k_exception_dispatch(f);
  assert(seen_cause==HAL_TRAP_CAUSE_PAGE_FAULT && seen_vector==2);
  assert(seen_mode==(i==0?HAL_TRAP_MODE_READ:i==1?HAL_TRAP_MODE_WRITE:HAL_TRAP_MODE_EXEC));
 }
 assert(enters==leaves);free(f);puts("m68k fixed entry: PASS frame routing, modes, syscall and ownership");return 0;
}
