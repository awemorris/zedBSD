/* amd64 IDT, exception and syscall dispatch. */
#include <kern/sched.h>
#include <hal/hal.h>
#include <errno.h>
#include "int.h"
#include "task.h"
#include "irq.h"
#include "asm.h"

struct amd64_idt_entry {
	uint16 offset_low;
	uint16 selector;
	uint8 ist;
	uint8 type_attr;
	uint16 offset_middle;
	uint32 offset_high;
	uint32 reserved;
} __attribute__((packed));

struct amd64_idtr {
	uint16 limit;
	uint64 base;
} __attribute__((packed));

static struct amd64_idt_entry idt[256] __attribute__((aligned(16)));
static int resched_flag;
static hal_syscall_handler_t syscall_handler;
static hal_user_return_handler_t user_return_handler;
void hal_user_return_set_handler(hal_user_return_handler_t h) { user_return_handler = h; }
void hal_user_return_invoke(void) { if (user_return_handler != NULL) user_return_handler(); }
static hal_user_int_handler_t user_int_handler;
static hal_user_fault_handler_t user_fault_handler;
static hal_trap_handler_t trap_handlers[5];

_Static_assert(sizeof(struct amd64_idt_entry) == 16, "amd64 IDT entry");

static void
set_gate(unsigned vector, unsigned dpl, void *handler)
{
	uintptr_t address = (uintptr_t)handler;
	struct amd64_idt_entry *entry = &idt[vector];
	entry->offset_low = (uint16)address;
	entry->selector = SEG_KERNEL_CODE;
	entry->ist = 0;
	entry->type_attr = (uint8)(0x8eU | (dpl << 5));
	entry->offset_middle = (uint16)(address >> 16);
	entry->offset_high = (uint32)(address >> 32);
	entry->reserved = 0;
}

void
amd64_int_init(void)
{
	struct amd64_idtr idtr;
	unsigned index;
	for (index = 0; index < 256; index++)
		set_gate(index, 0, amd64_undefined_entry);
	for (index = 0; index < 32; index++)
		set_gate(index, 0, amd64_fault_table[index]);
	idt[8].ist = 1;
	for (index = 0; index < 16; index++)
		set_gate(INT_IRQ_BASE + index, 0, amd64_irq_table[index]);
	set_gate(INT_SYSCALL, 3, amd64_syscall_entry);
	idtr.limit = sizeof(idt) - 1U;
	idtr.base = (uintptr_t)idt;
	asm_lidt(&idtr);
}

void int_set_resched_flag(void) { resched_flag = 1; }
void hal_reschedule_on_interrupt_return(void) { resched_flag = 1; }
void hal_user_int_set_handler(hal_user_int_handler_t h) { user_int_handler = h; }
void hal_user_fault_set_handler(hal_user_fault_handler_t h) { user_fault_handler = h; }
void hal_syscall_set_handler(hal_syscall_handler_t h) { syscall_handler = h; }

void
hal_set_trap_handler(int trap, hal_trap_handler_t handler)
{
	if (trap >= 0 && trap < 5) trap_handlers[trap] = handler;
}

static void
handle_fault(struct amd64_interrupt_frame *frame)
{
	int vector = (int)frame->vector;
	uintptr_t address = vector == INT_PAGEFAULT ? asm_get_cr2() : 0;
	int mode = vector == INT_PAGEFAULT && (frame->error_code & 16U) ?
	    HAL_TRAP_MODE_EXEC : vector == INT_PAGEFAULT &&
	    (frame->error_code & 2U) ? HAL_TRAP_MODE_WRITE : HAL_TRAP_MODE_READ;
	int cause = vector == INT_PAGEFAULT ? HAL_TRAP_CAUSE_PAGE_FAULT :
	    vector == 6 ? HAL_TRAP_CAUSE_ILLEGAL_INSN :
	    vector == 3 ? HAL_TRAP_CAUSE_BREAKPOINT :
	    vector == 17 ? HAL_TRAP_CAUSE_ALIGNMENT :
	    HAL_TRAP_CAUSE_MACHINE_CHECK;

	if ((frame->cs & 3U) == 3U) {
		struct hal_user_trap trap;
		int handled;
		trap.vector = (uint32)vector;
		trap.cs = (uint32)frame->cs;
		trap.eip = (uint32)frame->rip;
		trap.eax = (uint32)frame->rax;
		trap.error_code = (uint32)frame->error_code;
		trap.fault_address = (uint32)address;
		amd64_task_enter_user_frame(frame);
		handled = user_fault_handler != NULL &&
		    user_fault_handler(&trap) == HAL_TRAP_RET_SUCCESS;
		if (handled) {
			hal_user_return_invoke();
			amd64_task_leave_user_frame();
			return;
		}
		amd64_task_leave_user_frame();
		HAL_FATAL("amd64 user fault handler returned");
	}
	if (cause >= 0 && cause < 5 && trap_handlers[cause] != NULL &&
	    trap_handlers[cause]((void *)(uintptr_t)frame->rip,
	    (void *)address, mode) == HAL_TRAP_RET_SUCCESS) return;
	hal_printf("amd64 fault v=%u rip=%08X:%08X err=%08X cr2=%08X:%08X\n",
	    (uint32)vector, (uint32)(frame->rip >> 32), (uint32)frame->rip,
	    (uint32)frame->error_code, (uint32)(address >> 32),
	    (uint32)address);
	HAL_FATAL("unhandled amd64 fault");
}

void
int_handler(struct amd64_interrupt_frame *frame)
{
	int vector = (int)frame->vector;
	resched_flag = 0;
	if (vector >= INT_IRQ_BASE && vector <= INT_IRQ_BASE + IRQ_MAX) {
		irq_handler(vector - INT_IRQ_BASE);
	} else if (vector == INT_SYSCALL && (frame->cs & 3U) == 3U) {
		uintptr_t args[HAL_SYSCALL_ARGS];
		struct hal_user_trap trap;
		trap.vector = (uint32)vector;
		trap.cs = (uint32)frame->cs;
		trap.eip = (uint32)frame->rip;
		trap.eax = (uint32)frame->rax;
		trap.error_code = trap.fault_address = 0;
		if (user_int_handler != NULL) user_int_handler(&trap);
		args[0] = (uint32)frame->rbx;
		args[1] = (uint32)frame->rcx;
		args[2] = (uint32)frame->rdx;
		args[3] = (uint32)frame->rsi;
		args[4] = (uint32)frame->rdi;
		args[5] = (uint32)frame->rbp;
		amd64_task_enter_user_frame(frame);
		frame->rax = syscall_handler != NULL ?
		    (uint32)syscall_handler((uint32)frame->rax, args) :
		    (uint32)-(int32)ENOSYS;
		hal_user_return_invoke();
		amd64_task_leave_user_frame();
	} else if (vector >= 0 && vector < 32) {
		handle_fault(frame);
	} else {
		HAL_FATAL("undefined amd64 interrupt");
	}
	if (resched_flag) sched_yield();
}
