#include <sys/kern/sched.h>
#include <sys/kcrt/kcrt.h>
#include "int.h"
#include "irq.h"
#include "asm.h"
#include "pic.h"

/* 再スケジュールフラグ */
static int	 resched_flag;

/* forward declaration */
static void create_idt();
static void load_idt();
static void set_idt_entry(int index, int dpl, void *handler);
static void handle_fault(struct interrupt_frame *fp);


/*
 * 割り込み管理部を初期化する
 */
void i386_int_init(void)
{

	/* NOTE:
	 *	o この時点では割り込みは禁止(IPL_HIGH)されている。
	 *	o 割り込みの許可はIRQの初期化ルーチンでIRQをマスクしてから行う。 */

	/* IDTを作成する */
	create_idt();

	/* CPUにIDTをロードする */
	load_idt();

	/* cmain enables interrupts after the PIC and PIT are initialized. */
}

/*
 * 再スケジュールフラグをセットする
 * (現在の割り込み処理終了時に再スケジュールを行う)
 */
void int_set_resched_flag()
{
	resched_flag = 1;
}

/*
 * 一般割り込みハンドラ
 * (called from trap.s)
 *
 * NOTE: IDT内で割り込みゲートとして登録されているため、
 *		 ハンドラは割り込み禁止状態で開始される。
 */
void int_handler(struct interrupt_frame *fp)
{
	int is_handled, int_num;

	is_handled = 0;
	int_num    = fp->int_num;

	/* 再スケジュールフラグをクリアする */
	resched_flag = 0;

	/*
	 * IRQに割り当てられた割り込みの番号である場合
	 */
	if(int_num >= INT_IRQ_BASE && int_num <= INT_IRQ_BASE + IRQ_MAX) {
		/* ソフトウェアによるINT命令である場合、例外0Dとして処理する */

		int irq_num = int_num - INT_IRQ_BASE;
		int in_service = pic_get_irq_in_service();

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

	/* 割り込み処理ハンドラ内で再スケジュールが要求された場合*/
	if(resched_flag != 0)
		sched_yield();	/* タスクを切り替える */
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
}

/* IDTのエントリをセットする */
static void set_idt_entry(int index, int dpl, void *handler)
{
	uint8	*entry;

	/* エントリへのポインタを求める */
	entry = (uint8 *)(ADDR_IDT | SYS_START) + 8*index;

	/* 属性部をセットする */
	entry[5] = 0x8e | (dpl << 5);	/* 割り込みゲート */
//	entry[5] = 0x8f | (dpl << 5);	/* トラップゲート */

	/* コピーカウントをゼロにする */
	entry[4] = 0;

	/* ハンドラアドレスをセットる */
	*(uint16 *)(&entry[0]) = (uint16)((uint32)handler & 0xffff);
	*(uint16 *)(&entry[6]) = (uint16)((uint32)handler >> 16);
	*(uint16 *)(&entry[2]) = SEG_SYS_CODE;
}

/* IDTをロードする */
static void load_idt()
{
	uint8	idtr[6];

	/* IDTRの構造を用意する */
	*(uint16 *)&idtr[0] = 8*256-1;				/* IDTのリミット値 */
	*(uint32 *)&idtr[2] = ADDR_IDT + SYS_START;	/* IDTのリニアアドレス */

	/* LIDT命令でIDTをロードする */
	asm_lidt(idtr);
}

/* プロセッサフォールトハンドラ*/
static void handle_fault(struct interrupt_frame *fp)
{
	int int_num = fp->int_num;

	if (fp->cs & 3) {
		hal_printf("[INT] int 0x%02X handled!\n"
		       "CS:  %04X  EIP: %08X\n"
		       "DS:  %04X  ES:  %04X  SS: %04X\n"
		       "EAX: %08X  EBX: %08X  ECX: %08X  EDX: %08X\n"
		       "ESI: %08X  EDI: %08X  EBP: %08X  ESP: %08X\n"
		       "EFLAGS: %08X  ERRORCODE: %08X\n",
		       int_num,
		       fp->cs,
		       fp->eip,
		       fp->regs.ds,
		       fp->regs.es,
		       fp->user_ss,
		       fp->regs.eax,
		       fp->regs.ebx,
		       fp->regs.ecx,
		       fp->regs.edx,
		       fp->regs.esi,
		       fp->regs.edi,
		       fp->regs.ebp,
		       fp->user_esp,
		       fp->eflags,
		       fp->error_code);
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
