/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The i386 interrupt-frame and assembly-handler contract.
 */

#ifndef _SYS_ARCH_X86_INT_H_
#define _SYS_ARCH_X86_INT_H_

#include <hal/types.h>

/*
 * Interrupt Stackframe
 */
struct interrupt_frame {
	/*
	 * Register Save Area (44 bytes)
	 */
	struct {
		uint32_t edi;		/* +0 */
		uint32_t esi;		/* +4 */
		uint32_t ebp;		/* +8 */
		uint32_t _esp;		/* +16 (ignore) */
		uint32_t ebx;		/* +20 */
		uint32_t edx;		/* +24 */
		uint32_t ecx;		/* +28 */
		uint32_t eax;		/* +32 pushal */
		uint16_t es;		/* +36 push %es */
		uint16_t __pad_0;
		uint16_t ds;		/* +40 push %ds */
		uint16_t __pad_1;
	} regs;

	/* Interrupt number. */
	uint32_t int_num;	/* +44 */

	/* Error code. (only int 0x0D, 0x0E are fixed) */
	uint32_t error_code;	/* +48 */

	/* CS:EIP and EFLAGS */
	uint32_t eip;		/* +52 */
	uint16_t cs;		/* +56 */
	uint16_t __pad_2;
	uint32_t eflags;	/* +60 */

	/* 動作レベル移行時の旧SS:ESP */
	uint32_t user_esp;	/* +64 */
	uint16_t user_ss;	/* +68 */
	uint16_t __pad_3;
};

/*
 * int.c
 */
void int_init(void);
void int_handler(struct interrupt_frame *fp);	/* called from trap.s */
void i386_int_load(void);

/*
 * trap.S
 */
extern void *_asm_fault_int_handler_tbl[32];
extern void *_asm_irq_int_handler_tbl[16];
extern void *_asm_syscall_int_handler(void);
extern void *_asm_cpu_notify_handler(void);
extern void *_asm_cpu_panic_handler(void);
extern void *_asm_cpu_tlb_handler(void);
extern void *_asm_undefined_int_handler(void);

#endif
