/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The registers of a stopped thread, as a debugger sees them.
 *
 * These describe the same state the kernel's own hardware layer holds,
 * and are written out again here rather than shared with it: what a
 * process may see is an interface this system publishes and keeps,
 * while the kernel's internal form is free to change with the machine
 * it is describing.
 */

#ifndef KERN_UAPI_REG_H
#define KERN_UAPI_REG_H

#include <stdint.h>

#if defined(__x86_64__)

/*
 * The integer registers.  Only the two segment selectors the processor
 * saves are part of a thread's user state; the thread pointer appears as
 * the segment base the C library keeps it in.
 */
struct reg {
	uint64_t r_rax, r_rbx, r_rcx, r_rdx;
	uint64_t r_rsi, r_rdi, r_rbp, r_rsp;
	uint64_t r_r8, r_r9, r_r10, r_r11;
	uint64_t r_r12, r_r13, r_r14, r_r15;
	uint64_t r_rip, r_rflags;
	uint64_t r_cs, r_ss;
	uint64_t r_fs_base;
	uint64_t r_gs_base;
};

/*
 * The x87 state.  Each of the eight registers is ten bytes wide and is
 * held in sixteen, which is how the processor writes them out.
 */
struct fpreg {
	uint16_t fp_control;
	uint16_t fp_status;
	uint16_t fp_tag;
	uint16_t fp_opcode;
	uint64_t fp_instruction_pointer;
	uint64_t fp_data_pointer;
	uint8_t fp_stack[8][16];
};

/*
 * The vector state.
 */
struct xmmreg {
	uint32_t xmm_control;
	uint32_t xmm_control_mask;
	uint8_t xmm_register[16][16];
};

#elif defined(__i386__)

/*
 * The integer registers of the 32-bit x86 processor.
 */
struct reg {
	uint32_t r_eax, r_ebx, r_ecx, r_edx;
	uint32_t r_esi, r_edi, r_ebp, r_esp;
	uint32_t r_eip, r_eflags;
	uint32_t r_cs, r_ss, r_ds, r_es, r_fs, r_gs;
};

/*
 * The x87 floating-point state.
 */
struct fpreg {
	uint16_t fp_control;
	uint16_t fp_status;
	uint16_t fp_tag;
	uint16_t fp_opcode;
	uint32_t fp_instruction_pointer;
	uint32_t fp_data_pointer;
	uint8_t fp_stack[8][16];
};

#elif defined(__aarch64__)

/*
 * The integer registers: the thirty-one general registers (x30 is the
 * link register), the stack pointer, the address of the next instruction,
 * the saved processor state, and the thread pointer (TPIDR_EL0).  Of the
 * processor state a debugger may change only the condition flags.
 */
struct reg {
	uint64_t r_x[31];
	uint64_t r_sp;
	uint64_t r_pc;
	uint64_t r_pstate;
	uint64_t r_tpidr;
};

/*
 * The floating-point and SIMD state: the thirty-two 128-bit registers and
 * the status and control registers.
 */
struct fpreg {
	uint8_t fp_v[32][16];
	uint32_t fp_fpsr;
	uint32_t fp_fpcr;
};

#elif defined(__riscv) && __riscv_xlen == 64

/*
 * The integer registers: the thirty-two general registers (x0 always reads
 * as zero, x1 is the return address, x2 the stack pointer and x4 the thread
 * pointer) and the address of the next instruction.
 */
struct reg {
	uint64_t r_x[32];
	uint64_t r_pc;
};

/*
 * The floating-point state: the thirty-two 64-bit registers and the
 * control and status register, padded to a whole number of words.
 */
struct fpreg {
	uint64_t fp_f[32];
	uint32_t fp_fcsr;
	uint32_t fp_reserved;
};

#elif defined(__powerpc__) && !defined(__powerpc64__)

/*
 * The integer registers of the 32-bit PowerPC processor: the thirty-two
 * general registers (r1 is the stack pointer and r2 the thread pointer),
 * the address of the next instruction, the machine state, and the link,
 * count, condition and fixed-point exception registers.  Of the machine
 * state a debugger may not change anything.
 */
struct reg {
	uint32_t r_r[32];
	uint32_t r_pc;
	uint32_t r_msr;
	uint32_t r_lr;
	uint32_t r_ctr;
	uint32_t r_cr;
	uint32_t r_xer;
};

/*
 * The floating-point state: the thirty-two 64-bit registers and the
 * status and control register, padded to a whole number of doublewords.
 */
struct fpreg {
	uint64_t fp_f[32];
	uint32_t fp_fpscr;
	uint32_t fp_reserved;
};

#elif defined(__powerpc64__)

/*
 * The integer registers of the 64-bit PowerPC processor: the thirty-two
 * general registers (r1 is the stack pointer, r2 the TOC pointer and r13
 * the thread pointer), the address of the next instruction, the machine
 * state, and the link, count, condition and fixed-point exception
 * registers.  Of the machine state a debugger may not change anything.
 */
struct reg {
	uint64_t r_r[32];
	uint64_t r_pc;
	uint64_t r_msr;
	uint64_t r_lr;
	uint64_t r_ctr;
	uint64_t r_cr;
	uint64_t r_xer;
};

/*
 * The floating-point state: the thirty-two 64-bit registers and the
 * status and control register, padded to a whole number of doublewords.
 */
struct fpreg {
	uint64_t fp_f[32];
	uint32_t fp_fpscr;
	uint32_t fp_reserved;
};

#elif defined(__m68k__)

/*
 * The integer registers of the 680x0 processor: the eight data registers,
 * the eight address registers (a7 is the user stack pointer and a6 the
 * frame pointer), the status register, and the address of the next
 * instruction.  Of the status register a debugger may change only the
 * condition codes.
 */
struct reg {
	uint32_t r_d[8];
	uint32_t r_a[8];
	uint32_t r_sr;
	uint32_t r_pc;
};

/*
 * The floating-point state: the eight registers in the 96-bit extended
 * memory format (three words each), then the control, status and
 * instruction address registers.
 */
struct fpreg {
	uint32_t fp_fp[8][3];
	uint32_t fp_fpcr;
	uint32_t fp_fpsr;
	uint32_t fp_fpiar;
};

#else

#error "this architecture does not describe its registers yet"

#endif

#endif
