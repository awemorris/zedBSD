/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 32-bit PowerPC HAL's address layout and register numbers.
 *
 * The supervisor reaches RAM and the framebuffer through block address
 * translation, at the physical addresses themselves; only the user half
 * goes through the hashed page table.  The constants are shared by the
 * assembly sources.
 */

#ifndef KERN_HAL_PPC32_DEFS_H
#define KERN_HAL_PPC32_DEFS_H

/* Marks a parameter a function does not use. */
#define UNUSED_PARAMETER(parameter) ((void)(parameter))

/* The page the hashed table maps, and a segment. */
#define PPC32_PAGE_SIZE			4096
#define PPC32_PAGE_SHIFT		12
#define PPC32_SEGMENT_SHIFT		28

/* The image's load address, where the firmware enters it. */
#define PPC32_KERNEL_LOAD		0x01000000

/*
 * The supervisor's windows: RAM from 0 up to 1 GiB at most, in as many
 * blocks as the processor has block translations to spare, and the user
 * half between 1 GiB and 2 GiB, segments 4 to 7.  The device windows
 * are above the user half.
 */
#define PPC32_RAM_WINDOW_LIMIT		0x40000000
#define PPC32_USER_MINIMUM		0x40000000
#define PPC32_USER_LIMIT		0x80000000
#define PPC32_USER_SEGMENT_FIRST	4
#define PPC32_USER_SEGMENT_COUNT	4

/*
 * The block translations: four instruction and four data pairs on every
 * processor, and four more of each on the 7455 and the later 7450-family
 * processors, which HID0's high BAT enable turns on (its upper half for
 * oris: bit 8 from the most significant end).  On the 603, 750 and 7400
 * the same HID0 bit is doze.
 *
 * Values referenced: MPC7450 RISC Microprocessor Family Reference Manual
 * (MPC7450UM), HID0 and the SPR numbers of IBAT4U to DBAT7L (560 to 575).
 */
#define PPC32_BAT_PAIRS			4
#define PPC32_BAT_PAIRS_HIGH		8
#define PPC32_HID0_HIGH_BAT_EN_UPPER	0x0080

/*
 * Machine state bits.
 *
 * Values referenced: PowerPC Microprocessor Family: The Programming
 * Environments for 32-Bit Microprocessors (MPCFPE32B), the MSR, the
 * exception vectors, the BAT, segment register and page table entry
 * formats.
 */
#define PPC32_MSR_EE			0x8000
#define PPC32_MSR_PR			0x4000
#define PPC32_MSR_FP			0x2000
#define PPC32_MSR_ME			0x1000
#define PPC32_MSR_IR			0x0020
#define PPC32_MSR_DR			0x0010
#define PPC32_MSR_RI			0x0002
#define PPC32_MSR_KERNEL		(PPC32_MSR_FP | PPC32_MSR_ME | PPC32_MSR_IR | PPC32_MSR_DR | PPC32_MSR_RI)
#define PPC32_MSR_USER			(PPC32_MSR_KERNEL | PPC32_MSR_EE | PPC32_MSR_PR)

/* The exception vectors the HAL takes by offset. */
#define PPC32_VECTOR_DSI		0x0300
#define PPC32_VECTOR_ISI		0x0400
#define PPC32_VECTOR_EXTERNAL		0x0500
#define PPC32_VECTOR_ALIGNMENT		0x0600
#define PPC32_VECTOR_PROGRAM		0x0700
#define PPC32_VECTOR_FP_UNAVAILABLE	0x0800
#define PPC32_VECTOR_DECREMENTER	0x0900
#define PPC32_VECTOR_SYSCALL		0x0c00
#define PPC32_VECTOR_TRACE		0x0d00
#define PPC32_VECTOR_FIRST		0x0100
#define PPC32_VECTOR_END		0x3000
#define PPC32_VECTOR_STRIDE		0x0100

/* The data storage fault's status bits. */
#define PPC32_DSISR_NOT_FOUND		0x40000000
#define PPC32_DSISR_PROTECTION		0x08000000
#define PPC32_DSISR_STORE		0x02000000

/*
 * The trap frame the entry builds: the 32 general registers, then the
 * link, count, fixed-point exception and condition registers, the saved
 * program counter and machine state, the data fault's address and status
 * and the vector.  16 bytes below it hold the C call's back chain.
 */
#define PPC32_FRAME_LR			128
#define PPC32_FRAME_CTR			132
#define PPC32_FRAME_XER			136
#define PPC32_FRAME_CR			140
#define PPC32_FRAME_PC			144
#define PPC32_FRAME_MSR			148
#define PPC32_FRAME_DAR			152
#define PPC32_FRAME_DSISR		156
#define PPC32_FRAME_VECTOR		160
#define PPC32_FRAME_SIZE		176
#define PPC32_FRAME_CALL		16

/*
 * The words the task switch keeps on a stack: the back chain, the link
 * and condition registers, a pad word, r14 to r31, and padding to 16
 * bytes.
 */
#define PPC32_DISPATCH_WORDS		24
#define PPC32_DISPATCH_LR		1
#define PPC32_DISPATCH_R14		4
#define PPC32_DISPATCH_R15		5

#endif
