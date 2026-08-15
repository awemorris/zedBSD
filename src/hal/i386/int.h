#ifndef _SYS_ARCH_X86_INT_H_
#define _SYS_ARCH_X86_INT_H_

#include <hal/types.h>

/*
 * 割り込み発生時のスタックフレーム
 */
struct interrupt_frame {
	/* レジスタ保存域(44バイト)
	 * (カーネルのCコードを実行するために保存が必要なレジスタのみ) */
	struct {
		uint32	edi;		/* +0         */
		uint32	esi;		/* +4         */
		uint32	ebp;		/* +8         */
		uint32	_esp;		/* +16 (無視) */
		uint32	ebx;		/* +20        */
		uint32	edx;		/* +24        */
		uint32	ecx;		/* +28        */
		uint32	eax;		/* +32 pushal */
		uint16	es;		/* +36 push %es */	uint16 __pad_0;
		uint16	ds;		/* +40 push %ds */	uint16 __pad_1;
	} regs;

	/* 割り込み番号 */
	uint32	int_num;	/* +44 */

	/* エラーコード(int 0x0D, 0x0E 以外では不定値) */
	uint32	error_code;	/* +48 */

	/* 割り込み発生時のCS:EIPとEFLAGS */
	uint32	eip;		/* +52 */
	uint16	cs;			/* +56 */	uint16 __pad_2;
	uint32	eflags;		/* +60 */

	/* 動作レベル移行時の旧SS:ESP */
	uint32	user_esp;	/* +64 */
	uint16	user_ss;	/* +68 */	uint16 __pad_3;
};

/*
 * int.c
 */
void int_init(void);
void int_handler(struct interrupt_frame *fp);	/* called from trap.s */

/*
 * trap.S
 */
extern void *_asm_fault_int_handler_tbl[32];
extern void *_asm_irq_int_handler_tbl[16];
extern void *_asm_syscall_int_handler();
extern void *_asm_undefined_int_handler();

#endif
