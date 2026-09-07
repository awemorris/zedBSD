/* Link-only observation of the real shutdown/device boundary. */
#include <kern/writeback.h>
#include <hal/hal.h>
void __real_drv_usb_shutdown(void);
void __real_kern_platform_halt(void);
void __wrap_drv_usb_shutdown(void)
{
 struct writeback_policy_stats policy;
 struct writeback_budget_stats budget;
 writeback_policy_snapshot(&policy);writeback_budget_snapshot(&budget);
 if(policy.mounts || policy.busy || budget.dirty || budget.reserved || budget.tickets)
  HAL_FATAL("shutdown probe found live writeback");
 hal_printf("WRITEBACK SHUTDOWN STORAGE CLEAN\n");
 __real_drv_usb_shutdown();
}
void __wrap_kern_platform_halt(void)
{
 hal_printf("WRITEBACK SHUTDOWN HALT PASS\n");
 __real_kern_platform_halt();
}
