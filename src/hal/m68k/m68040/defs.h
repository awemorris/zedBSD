/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MC68040 HAL's address layout and register numbers.
 *
 * The supervisor reaches RAM and the device space through the transparent
 * translation registers, at the physical addresses themselves; only the
 * user half goes through the page tables.  A space's root is loaded into
 * both root pointers, so the supervisor reaches the user half of the
 * space it runs in, and the user never matches a supervisor-only
 * transparent window.  The constants are shared by the assembly sources.
 */

#ifndef KERN_HAL_M68K_M68040_DEFS_H
#define KERN_HAL_M68K_M68040_DEFS_H

/* Marks a parameter a function does not use. */
#define UNUSED_PARAMETER(parameter) ((void)(parameter))

/* The page the tables map. */
#define M68040_PAGE_SIZE		4096
#define M68040_PAGE_SHIFT		12

/*
 * The three table levels for 4 KiB pages: 128 root entries of 32 MiB, 128
 * pointer entries of 256 KiB and 64 page entries.
 *
 * Values referenced: M68040 User's Manual (MC68040UM/AD), section 3 (the
 * table index fields and the descriptor formats).
 */
#define M68040_ROOT_SHIFT		25
#define M68040_POINTER_SHIFT		18
#define M68040_ROOT_ENTRIES		128
#define M68040_POINTER_ENTRIES		128
#define M68040_PAGE_ENTRIES		64
#define M68040_ROOT_BYTES		512
#define M68040_POINTER_BYTES		512
#define M68040_PAGE_TABLE_BYTES		256

/*
 * Descriptor bits.  A root or pointer descriptor is resident when its two
 * low bits are 2 or 3, a page descriptor when they are 1 or 3.
 */
#define M68040_DESC_TYPE_MASK		0x3
#define M68040_DESC_TABLE		0x2
#define M68040_DESC_PAGE		0x1
#define M68040_DESC_WRITE_PROTECT	0x4
#define M68040_DESC_USED		0x8
#define M68040_DESC_MODIFIED		0x10
#define M68040_DESC_CM_COPYBACK		0x20
#define M68040_DESC_CM_SERIALIZED	0x40
#define M68040_DESC_SUPERVISOR		0x80
#define M68040_DESC_GLOBAL		0x400
#define M68040_ROOT_ADDRESS_MASK	0xfffffe00
#define M68040_POINTER_ADDRESS_MASK	0xfffffe00
#define M68040_PAGE_TABLE_ADDRESS_MASK	0xffffff00
#define M68040_PAGE_ADDRESS_MASK	0xfffff000

/* The translation control: enabled, 4 KiB pages. */
#define M68040_TC_ENABLE		0x8000

/*
 * The transparent translation registers: an address base and mask in the
 * top two bytes, enabled, matched in supervisor mode only, and a cache
 * mode.
 */
#define M68040_TT_ENABLE		0x8000
#define M68040_TT_SUPERVISOR		0x2000
#define M68040_TT_CM_COPYBACK		0x20
#define M68040_TT_CM_SERIALIZED		0x40

/*
 * The supervisor's windows: RAM from 0 up to 1 GiB, and the device space
 * at the top 16 MiB.  The user half lies between them.
 */
#define M68040_RAM_LIMIT		0x40000000
#define M68040_DEVICE_BASE		0xff000000
#define M68040_USER_MINIMUM		0x40000000
#define M68040_USER_LIMIT		0x80000000

/* The status register's fields. */
#define M68040_SR_SUPERVISOR		0x2000
#define M68040_SR_IPL_MASK		0x0700

/* The exception vectors the HAL takes by number. */
#define M68040_VECTOR_ACCESS		2
#define M68040_VECTOR_ADDRESS		3
#define M68040_VECTOR_ILLEGAL		4
#define M68040_VECTOR_ZERO_DIVIDE	5
#define M68040_VECTOR_PRIVILEGE		8
#define M68040_VECTOR_TRACE		9
#define M68040_VECTOR_LINE_A		10
#define M68040_VECTOR_LINE_F		11
#define M68040_VECTOR_SPURIOUS		24
#define M68040_VECTOR_AUTOVECTOR_1	25
#define M68040_VECTOR_AUTOVECTOR_7	31
#define M68040_VECTOR_TRAP_0		32
#define M68040_VECTOR_TRAP_15		47

/*
 * The trap frame the entry builds below the processor's frame: the user
 * stack pointer, d0 to d7 and a0 to a6, then the processor's status
 * register, program counter and format/vector word.
 */
#define M68040_FRAME_USP		0
#define M68040_FRAME_D0			4
#define M68040_FRAME_A0			36
#define M68040_FRAME_SR			64
#define M68040_FRAME_PC			66
#define M68040_FRAME_FORMAT		70
#define M68040_FRAME_SIZE		72

/* The words the task switch keeps on a stack: d2 to d7, a2 to a6, the return. */
#define M68040_DISPATCH_WORDS		12
#define M68040_DISPATCH_D2		0
#define M68040_DISPATCH_D3		1
#define M68040_DISPATCH_RETURN		11

#endif
