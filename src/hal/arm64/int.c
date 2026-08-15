#include <hal/hal.h>
#include "asm.h"
#include "int.h"
#include "task.h"
#include "irq.h"
#include "bsp-rpi4/gic.h"
#include <kern/sched.h>
#include <errno.h>

extern char arm64_vectors[];
static hal_trap_handler_t trap_handlers[5];
static hal_syscall_handler_t syscall_handler;
static hal_user_return_handler_t user_return_handler;
void hal_user_return_set_handler(hal_user_return_handler_t h){user_return_handler=h;}
void hal_user_return_invoke(void){if(user_return_handler!=NULL)user_return_handler();}
static hal_user_int_handler_t user_int_handler;
static hal_user_fault_handler_t user_fault_handler;
static int resched_pending;

void arm64_int_init(void)
{
	__asm__ volatile("msr vbar_el1, %0\n\tisb"::"r"(arm64_vectors):"memory");
	rpi4_gic_init();
	hal_timer_set_freq(100);
	hal_puts("ARM64 EXCEPTION PASS\nARM64 IRQ READY\n");
}

void arm64_sync_handler(struct arm64_exception_frame *f,uint64 vector)
{
	uint32 ec=(uint32)((f->esr>>26)&0x3f);
	if (vector == 0) {
		int cause = (ec == 0x20 || ec == 0x21 || ec == 0x24 ||
		    ec == 0x25) ? HAL_TRAP_CAUSE_PAGE_FAULT :
		    ec == 0x3c ? HAL_TRAP_CAUSE_BREAKPOINT :
		    (ec == 0x22 || ec == 0x26) ? HAL_TRAP_CAUSE_ALIGNMENT :
		    ec == 0 ? HAL_TRAP_CAUSE_ILLEGAL_INSN :
		    HAL_TRAP_CAUSE_MACHINE_CHECK;
		int mode = (ec == 0x20 || ec == 0x21) ? HAL_TRAP_MODE_EXEC :
		    (f->esr & (1ULL << 6)) ? HAL_TRAP_MODE_WRITE :
		    HAL_TRAP_MODE_READ;
		uintptr_t address = cause == HAL_TRAP_CAUSE_PAGE_FAULT ?
		    (uintptr_t)f->far : 0;

		if (trap_handlers[cause] != NULL &&
		    trap_handlers[cause]((void *)(uintptr_t)f->elr,
		    (void *)address, mode) == HAL_TRAP_RET_SUCCESS)
			return;
	}
	if (vector == 8 && ec == 0x15) {
		struct hal_user_trap trap;
		uintptr_t args[HAL_SYSCALL_ARGS];
		unsigned i;

		trap.vector = 0xc2U;
		trap.cs = 3U;
		trap.eip = f->elr;
		trap.eax = f->x[8];
		trap.error_code = 0;
		trap.fault_address = 0;
		if (user_int_handler != NULL)
			user_int_handler(&trap);
		for (i = 0; i < HAL_SYSCALL_ARGS; i++)
			args[i] = (uintptr_t)f->x[i];
		arm64_task_enter_user_frame(f);
		f->x[0] = (uint64)(syscall_handler != NULL ?
			syscall_handler((uint32)f->x[8], args) : -ENOSYS);
		hal_user_return_invoke();
		arm64_task_leave_user_frame();
		if (resched_pending) {
			resched_pending = 0;
			sched_yield();
		}
		return;
	}
	if (vector == 8 && (ec == 0x20 || ec == 0x21 ||
	    ec == 0x24 || ec == 0x25)) {
		struct hal_user_trap trap;
		int handled;
		trap.vector = 14U;
		trap.cs = 3U;
		trap.eip = f->elr;
		trap.eax = f->x[0];
		trap.error_code = (ec == 0x20 || ec == 0x21) ? 0x10U :
			((f->esr & (1ULL << 6)) ? 2U : 0U);
		trap.fault_address = f->far;
		arm64_task_enter_user_frame(f);
		handled = user_fault_handler != NULL &&
		    user_fault_handler(&trap) == HAL_TRAP_RET_SUCCESS;
		if (handled) {
			hal_user_return_invoke();
			arm64_task_leave_user_frame();
			return;
		}
		arm64_task_leave_user_frame();
		HAL_FATAL("AArch64 user page fault handler returned");
	}
	if (vector == 8) {
		struct hal_user_trap trap;
		int handled;
		trap.vector = (ec == 0x3c) ? 3U :
			(ec == 0x22 || ec == 0x26) ? 17U : 6U;
		trap.cs = 3U;
		trap.eip = f->elr;
		trap.eax = f->x[0];
		trap.error_code = 0;
		trap.fault_address = f->far;
		arm64_task_enter_user_frame(f);
		handled = user_fault_handler != NULL &&
		    user_fault_handler(&trap) == HAL_TRAP_RET_SUCCESS;
		if (handled) {
			hal_user_return_invoke();
			arm64_task_leave_user_frame();
			return;
		}
		arm64_task_leave_user_frame();
		HAL_FATAL("AArch64 user fault handler returned");
	}
	hal_printf("ARM64 sync vector=%u ec=%x esr=%llx elr=%llx far=%llx\n",
	    (uint32)vector,ec,f->esr,f->elr,f->far);
	HAL_FATAL("unhandled AArch64 synchronous exception");
}

void arm64_irq_handler(struct arm64_exception_frame *f)
{
	uint32 iar,id;(void)f;
	iar=rpi4_gic_ack();id=iar&0x3ff;
	if(id>=1020)return;
	irq_enter_isr((int)id);arm64_irq_dispatch(id);irq_leave_isr((int)id);rpi4_gic_eoi(iar);
	if (resched_pending) {
		resched_pending = 0;
		sched_yield();
	}
}

void hal_set_trap_handler(int trap,hal_trap_handler_t h){if(trap<0||trap>=5)HAL_FATAL("bad trap");trap_handlers[trap]=h;}
void hal_syscall_set_handler(hal_syscall_handler_t h){syscall_handler=h;}
void hal_user_int_set_handler(hal_user_int_handler_t h){user_int_handler=h;}
void hal_user_fault_set_handler(hal_user_fault_handler_t h){user_fault_handler=h;}
void hal_reschedule_on_interrupt_return(void){resched_pending=1;}
