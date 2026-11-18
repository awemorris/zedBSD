/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 64-bit PowerPC HAL's address layout and register numbers.
 *
 * RAM and the framebuffer are mapped at their own addresses in large
 * pages that stay for good; the user range goes through 4 KiB pages
 * entered as they are used.  The constants are shared by the assembly
 * sources.
 */

#ifndef KERN_HAL_PPC64_DEFS_H
#define KERN_HAL_PPC64_DEFS_H

/* Marks a parameter a function does not use. */
#define UNUSED_PARAMETER(parameter) ((void)(parameter))

/* The small page and a segment; the board names the large page. */
#define PPC64_PAGE_SIZE			4096
#define PPC64_PAGE_SHIFT		12
#define PPC64_SEGMENT_SHIFT		28

/*
 * RAM the kernel maps, and the user range: 6 GiB from 4 GiB, segments
 * 0x10 to 0x27, above the 32-bit addresses as the 64-bit user layout
 * asks.  The kernel puts the break and the mapping area 4 GiB above the
 * range's start, which leaves 2 GiB for the mappings and the stack.  The
 * segments are loaded into the segment lookaside buffer at each switch;
 * with the kernel's three, they fill 27 of the 32 entries the smallest
 * buffer of these processors holds.
 */
#define PPC64_RAM_LIMIT			0x20000000
#define PPC64_USER_MINIMUM		0x100000000
#define PPC64_USER_LIMIT		0x280000000
#define PPC64_USER_SEGMENT_FIRST	0x10
#define PPC64_USER_SEGMENT_COUNT	24

/*
 * Machine state bits.
 *
 * Values referenced: Power ISA Version 3.0B, Book III (the MSR, the
 * interrupt vectors, the segment lookaside buffer entry and the hashed
 * page table entry); the Linux on Power Architecture Platform Reference
 * (the hypervisor calls H_ENTER, H_REMOVE and H_CEDE).
 */
#define PPC64_MSR_EE			0x8000
#define PPC64_MSR_PR			0x4000
#define PPC64_MSR_FP			0x2000
#define PPC64_MSR_ME			0x1000
#define PPC64_MSR_IR			0x0020
#define PPC64_MSR_DR			0x0010
#define PPC64_MSR_RI			0x0002
#define PPC64_MSR_LOW_KERNEL		(PPC64_MSR_FP | PPC64_MSR_ME | PPC64_MSR_IR | PPC64_MSR_DR | PPC64_MSR_RI)

/* 64-bit mode, and the state a user task runs in (for the C code). */
#define PPC64_MSR_SF			0x8000000000000000
#define PPC64_MSR_USER			(PPC64_MSR_SF | PPC64_MSR_LOW_KERNEL | PPC64_MSR_EE | PPC64_MSR_PR)

/* The exception vectors the HAL takes by offset. */
#define PPC64_VECTOR_DSI		0x0300
#define PPC64_VECTOR_DSEGMENT		0x0380
#define PPC64_VECTOR_ISI		0x0400
#define PPC64_VECTOR_ISEGMENT		0x0480
#define PPC64_VECTOR_EXTERNAL		0x0500
#define PPC64_VECTOR_ALIGNMENT		0x0600
#define PPC64_VECTOR_PROGRAM		0x0700
#define PPC64_VECTOR_DECREMENTER	0x0900
#define PPC64_VECTOR_SYSCALL		0x0c00
#define PPC64_VECTOR_TRACE		0x0d00
#define PPC64_VECTOR_FIRST		0x0100
#define PPC64_VECTOR_END		0x1000
#define PPC64_VECTOR_STRIDE		0x0080

/* The data storage fault's status bits. */
#define PPC64_DSISR_NOT_FOUND		0x40000000
#define PPC64_DSISR_PROTECTION		0x08000000
#define PPC64_DSISR_STORE		0x02000000

/*
 * The trap frame the entry builds: the 32 general registers, then the
 * link, count, fixed-point exception and condition registers, the saved
 * program counter and machine state, the data fault's address and status
 * and the vector.  32 bytes below it are the C call's frame.
 */
#define PPC64_FRAME_LR			256
#define PPC64_FRAME_CTR			264
#define PPC64_FRAME_XER			272
#define PPC64_FRAME_CR			280
#define PPC64_FRAME_PC			288
#define PPC64_FRAME_MSR			296
#define PPC64_FRAME_DAR			304
#define PPC64_FRAME_DSISR		312
#define PPC64_FRAME_VECTOR		320
#define PPC64_FRAME_SIZE		336
#define PPC64_FRAME_CALL		32

/* The bytes below the stack pointer a function may use without a frame. */
#define PPC64_RED_ZONE			288

/*
 * The doublewords the task switch keeps on a stack: the back chain, the
 * condition register, the link register, the TOC, r14 to r31, and padding
 * to 16 bytes.
 */
#define PPC64_DISPATCH_WORDS		22
#define PPC64_DISPATCH_CR		1
#define PPC64_DISPATCH_LR		2
#define PPC64_DISPATCH_R14		4
#define PPC64_DISPATCH_R15		5

/* The hypervisor calls the HAL makes. */
#define PPC64_H_REMOVE			0x04
#define PPC64_H_ENTER			0x08
#define PPC64_H_CEDE			0xe0

#endif
