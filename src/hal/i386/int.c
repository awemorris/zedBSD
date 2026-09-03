/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Interrupt management.
 */

#include <hal/hal.h>
#include "int.h"
#include "task.h"
#include "irq.h"
#include "asm.h"
#include "pic.h"
#include "space.h"
#include <errno.h>

static hal_syscall_handler_t syscall_handler;

/* Forward declaration. */
static void create_idt();
static void load_idt();
static void set_idt_entry(int index, int dpl, void *handler);
static void handle_fault(struct interrupt_frame *fp);

static int
is_asynchronous_interrupt(int int_num)
{
	return (int_num >= INT_IRQ_BASE && int_num <= INT_IRQ_BASE + IRQ_MAX) ||
	    int_num == INT_CPU_NOTIFY || int_num == INT_CPU_TLB;
}


/*
 * Initialize the interrupt managemtn module.
 */
void i386_int_init(void)
{

	/*
	 * NOTE:
	 * - At this moment, IRQ are prohibited.
	 * - Allowing IRQ is done in the initialization of IRQ after initializing PIC.
	 */

	/* Create an IDT. */
	create_idt();

	/* Load the IDT. */
	load_idt();

	/* cmain enables interrupts after the PIC and PIT are initialized. */
}

void
i386_int_load(void)
{
	load_idt();
}

void
hal_syscall_set_handler(hal_syscall_handler_t handler)
{
	syscall_handler = handler;
}

/*
 * General Interrupt Handler
 * (called from trap.s)
 *
 * NOTE:
 *  - Registered as an "interrupt gate" in IDT.
 *  - So the handler is started with interrupts disabled.
 */
void int_handler(struct interrupt_frame *fp)
{
	int is_handled, int_num, user_interrupt;

	is_handled = 0;
	int_num    = fp->int_num;
	user_interrupt = (fp->cs & 3U) == 3U &&
	    is_asynchronous_interrupt(int_num);

	/* Only a frame returning to ring 3 is valid for signal delivery. */
	if (user_interrupt)
		i386_task_enter_user_frame(fp);

	/*
	 * IRQに割り当てられた割り込みの番号である場合
	 */
	if(int_num >= INT_IRQ_BASE && int_num <= INT_IRQ_BASE + IRQ_MAX) {
		/* ソフトウェアによるINT命令である場合、例外0Dとして処理する */

		int irq_num = int_num - INT_IRQ_BASE;
		int in_service = i386_interrupt_validate(irq_num);

		if(in_service == -1) {
			is_handled = 1;
			hal_printf("\nIRQ not in service (pic_isr=%02X, int=%02X)\n", in_service, int_num);
			HAL_FATAL("IRQ error");
		} else {
			/* IRQハンドラをコールする */
			irq_handler(irq_num);
			is_handled = 1;
		}
	}

	/*
	 * CPUの例外をハンドルする
	 */
	else if (int_num == INT_SYSCALL && (fp->cs & 3) == 3) {
		uintptr_t args[HAL_SYSCALL_ARGS];
		kernel_user_int_handler((uint32_t)int_num, fp->cs,
		    fp->eip, fp->regs.eax);
		args[0] = fp->regs.ebx;
		args[1] = fp->regs.ecx;
		args[2] = fp->regs.edx;
		args[3] = fp->regs.esi;
		args[4] = fp->regs.edi;
		args[5] = fp->regs.ebp;
		i386_task_enter_user_frame(fp);
		/* The generic callback owns accounting and its interruptible window. */
		fp->regs.eax = syscall_handler != NULL ?
			(uint32_t)syscall_handler(fp->regs.eax, args) :
			(uint32_t)-(int32_t)ENOSYS;
		kernel_user_return_handler();
		i386_task_leave_user_frame();
		is_handled = 1;
	}
	else if (int_num == INT_CPU_NOTIFY) {
		kernel_cpu_notify_handler(hal_cpu_current(), IRQ_MAX + 2U);
		is_handled = 1;
	}
	else if (int_num == INT_CPU_TLB) {
		i386_tlb_interrupt();
		is_handled = 1;
	}
	else if (int_num == INT_CPU_PANIC) {
		hal_cpu_park();
	}
	else if(int_num >= 0 && int_num <= 0x1f) {
		handle_fault(fp);
		is_handled = 1;
	}

	/*
	 * ハンドルされなかった場合
	 */
	if(!is_handled) {
		hal_printf("\nIRQ handler not installed (int %02X)\n" ,int_num);
		HAL_FATAL("IRQ error");
	}
	if (user_interrupt) {
		kernel_user_return_handler();
		i386_task_leave_user_frame();
	}

	/* 割り込み処理ハンドラ内で再スケジュールが要求された場合*/
}

/* IDTを作成する */
static void create_idt()
{
	int i;

	/* すべてのエントリに未定義割り込みハンドラをセットする */
	for(i=0; i<256; i++)
		set_idt_entry(i, 0, _asm_undefined_int_handler);

	/* 32個のフォールトハンドラをセットする */
	for(i=0; i<32; i++)
		set_idt_entry(i, 0, _asm_fault_int_handler_tbl[i]);

	/* 16個のIRQハンドラをセットする */
	for(i=0; i<16; i++)
		set_idt_entry(i+INT_IRQ_BASE, 0, _asm_irq_int_handler_tbl[i]);

	/* システムコールハンドラをセットする */
	set_idt_entry(INT_SYSCALL, 3, _asm_syscall_int_handler);
	set_idt_entry(INT_CPU_NOTIFY, 0, _asm_cpu_notify_handler);
	set_idt_entry(INT_CPU_PANIC, 0, _asm_cpu_panic_handler);
	set_idt_entry(INT_CPU_TLB, 0, _asm_cpu_tlb_handler);
}

/* IDTのエントリをセットする */
static void set_idt_entry(int index, int dpl, void *handler)
{
	uint8_t	*entry;

	/* エントリへのポインタを求める */
	entry = (uint8_t *)(ADDR_IDT | SYS_START) + 8*index;

	/* 属性部をセットする */
	entry[5] = 0x8e | (dpl << 5);	/* 割り込みゲート */
//	entry[5] = 0x8f | (dpl << 5);	/* トラップゲート */

	/* コピーカウントをゼロにする */
	entry[4] = 0;

	/* ハンドラアドレスをセットる */
	*(uint16_t *)(&entry[0]) = (uint16_t)((uint32_t)handler & 0xffff);
	*(uint16_t *)(&entry[6]) = (uint16_t)((uint32_t)handler >> 16);
	*(uint16_t *)(&entry[2]) = SEG_SYS_CODE;
}

/* IDTをロードする */
static void load_idt()
{
	uint8_t	idtr[6];

	/* IDTRの構造を用意する */
	*(uint16_t *)&idtr[0] = 8*256-1;				/* IDTのリミット値 */
	*(uint32_t *)&idtr[2] = ADDR_IDT + SYS_START;	/* IDTのリニアアドレス */

	/* LIDT命令でIDTをロードする */
	asm_lidt(idtr);
}

/* プロセッサフォールトハンドラ*/
static void handle_fault(struct interrupt_frame *fp)
{
	int int_num = fp->int_num;

	if (fp->cs & 3) {
		int handled;
		i386_task_enter_user_frame(fp);
		handled = kernel_user_fault_handler((uint32_t)int_num, fp->cs,
		    fp->eip, fp->error_code,
		    int_num == INT_PAGEFAULT ? asm_get_cr2() : 0) ==
		    HAL_TRAP_RET_SUCCESS;
		if (handled) {
			kernel_user_return_handler();
			i386_task_leave_user_frame();
			return;
		}
		i386_task_leave_user_frame();
		HAL_FATAL("user fault handler returned");
	} else {
		hal_printf("[INT] int 0x%02X handled!\n"
		       "CS:  %04X  EIP: %08X\n"
		       "DS:  %04X  ES:  %04X\n"
		       "EAX: %08X  EBX: %08X  ECX: %08X  EDX: %08X\n"
		       "ESI: %08X  EDI: %08X  EBP: %08X\n"
		       "EFLAGS: %08X  ERRORCODE: %08X\n",
		       int_num,
		       fp->cs,
		       fp->eip,
		       fp->regs.ds,
		       fp->regs.es,
		       fp->regs.eax,
		       fp->regs.ebx,
		       fp->regs.ecx,
		       fp->regs.edx,
		       fp->regs.esi,
		       fp->regs.edi,
		       fp->regs.ebp,
		       fp->eflags,
		       fp->error_code);
	}

	/* Halt. */
	for(;;)
		asm_hlt();
}
