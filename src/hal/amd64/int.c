/* amd64 IDT, exception and syscall dispatch. */
#include <hal/hal.h>
#include <errno.h>
#include "int.h"
#include "task.h"
#include "irq.h"
#include "asm.h"
#include "space.h"

struct amd64_idt_entry {
	uint16_t offset_low;
	uint16_t selector;
	uint8_t ist;
	uint8_t type_attr;
	uint16_t offset_middle;
	uint32_t offset_high;
	uint32_t reserved;
} __attribute__((packed));

struct amd64_idtr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

static struct amd64_idt_entry idt[256] __attribute__((aligned(16)));
static hal_syscall_handler_t syscall_handler;
static hal_trap_handler_t trap_handlers[5];

_Static_assert(sizeof(struct amd64_idt_entry) == 16, "amd64 IDT entry");

static int
is_asynchronous_interrupt(int vector)
{
	return (vector >= INT_IRQ_BASE && vector <= INT_IRQ_BASE + IRQ_MAX) ||
	    (vector >= AMD64_VECTOR_MSI_BASE &&
	    vector < AMD64_VECTOR_MSI_BASE + AMD64_VECTOR_MSI_COUNT) ||
	    vector == AMD64_VECTOR_NOTIFY || vector == AMD64_VECTOR_TLB ||
	    vector == AMD64_VECTOR_ERROR || vector == AMD64_VECTOR_SPURIOUS;
}

static void
set_gate(unsigned vector, unsigned dpl, void *handler)
{
	uintptr_t address = (uintptr_t)handler;
	struct amd64_idt_entry *entry = &idt[vector];
	entry->offset_low = (uint16_t)address;
	entry->selector = SEG_KERNEL_CODE;
	entry->ist = 0;
	entry->type_attr = (uint8_t)(0x8eU | (dpl << 5));
	entry->offset_middle = (uint16_t)(address >> 16);
	entry->offset_high = (uint32_t)(address >> 32);
	entry->reserved = 0;
}

void
amd64_int_load(void)
{
	struct amd64_idtr idtr;
	idtr.limit = sizeof(idt) - 1U;
	idtr.base = (uintptr_t)idt;
	asm_lidt(&idtr);
}

void
amd64_int_init(void)
{
	unsigned index;
	for (index = 0; index < 256; index++)
		set_gate(index, 0, amd64_undefined_entry);
	for (index = 0; index < 32; index++)
		set_gate(index, 0, amd64_fault_table[index]);
	idt[8].ist = 1;
	for (index = 0; index < 16; index++)
		set_gate(INT_IRQ_BASE + index, 0, amd64_irq_table[index]);
	for (index = 0; index < AMD64_VECTOR_MSI_COUNT; index++)
		set_gate(AMD64_VECTOR_MSI_BASE + index, 0,
		    amd64_msi_table[index]);
	set_gate(AMD64_VECTOR_NOTIFY, 0, amd64_notify_entry);
	set_gate(AMD64_VECTOR_TLB, 0, amd64_tlb_entry);
	set_gate(AMD64_VECTOR_ERROR, 0, amd64_error_entry);
	set_gate(AMD64_VECTOR_SPURIOUS, 0, amd64_spurious_entry);
	set_gate(INT_SYSCALL, 3, amd64_syscall_entry);
	amd64_int_load();
}

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
		int handled;
		amd64_task_enter_user_frame(frame);
		handled = kernel_user_fault_handler((uint32_t)vector,
		    (uint32_t)frame->cs, frame->rip, frame->error_code, address) ==
		    HAL_TRAP_RET_SUCCESS;
		if (handled) {
			kernel_user_return_handler();
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
	    (uint32_t)vector, (uint32_t)(frame->rip >> 32), (uint32_t)frame->rip,
	    (uint32_t)frame->error_code, (uint32_t)(address >> 32),
	    (uint32_t)address);
	HAL_FATAL("unhandled amd64 fault");
}

void
int_handler(struct amd64_interrupt_frame *frame)
{
	int vector = (int)frame->vector;
	int user_interrupt = (frame->cs & 3U) == 3U &&
	    is_asynchronous_interrupt(vector);

	/*
	 * Asynchronous interrupts are a user-return safe point just like syscalls
	 * and user faults.  Register the interrupted frame only when the CPU will
	 * actually return to ring 3; kernel-origin interrupts must not expose a
	 * kernel frame to signal delivery.
	 */
	if (user_interrupt)
		amd64_task_enter_user_frame(frame);
	if (vector >= INT_IRQ_BASE && vector <= INT_IRQ_BASE + IRQ_MAX) {
		irq_handler(vector - INT_IRQ_BASE);
	} else if (vector >= AMD64_VECTOR_MSI_BASE &&
	    vector < AMD64_VECTOR_MSI_BASE + AMD64_VECTOR_MSI_COUNT) {
		irq_handler(IRQ_MSI_BASE + vector - AMD64_VECTOR_MSI_BASE);
	} else if (vector == AMD64_VECTOR_NOTIFY) {
		amd64_notify_interrupt();
	} else if (vector == AMD64_VECTOR_TLB) {
		amd64_tlb_interrupt();
	} else if (vector == AMD64_VECTOR_ERROR) {
		amd64_error_interrupt();
	} else if (vector == AMD64_VECTOR_SPURIOUS) {
		/* No acknowledgement is required for the APIC spurious vector. */
	} else if (vector == INT_SYSCALL && (frame->cs & 3U) == 3U) {
		uintptr_t args[HAL_SYSCALL_ARGS];
		kernel_user_int_handler((uint32_t)vector, (uint32_t)frame->cs,
		    frame->rip, frame->rax);
		args[0] = (uintptr_t)frame->rbx;
		args[1] = (uintptr_t)frame->rcx;
		args[2] = (uintptr_t)frame->rdx;
		args[3] = (uintptr_t)frame->rsi;
		args[4] = (uintptr_t)frame->rdi;
		args[5] = (uintptr_t)frame->rbp;
		amd64_task_enter_user_frame(frame);
		/* The generic callback owns accounting and its interruptible window. */
		frame->rax = syscall_handler != NULL ?
		    (uint64_t)syscall_handler((uint32_t)frame->rax, args) :
		    (uint64_t)(intptr_t)-ENOSYS;
		kernel_user_return_handler();
		amd64_task_leave_user_frame();
	} else if (vector >= 0 && vector < 32) {
		handle_fault(frame);
	} else {
		HAL_FATAL("undefined amd64 interrupt");
	}
	if (user_interrupt) {
		kernel_user_return_handler();
		amd64_task_leave_user_frame();
	}
}
