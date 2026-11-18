/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 HAL's address layout, descriptor bits and register numbers.
 *
 * The kernel runs in supervisor mode with the short-descriptor
 * translation tables and TTBCR.N = 1: the lower 2 GiB belong to the user
 * space TTBR0 selects, and the upper 2 GiB to the kernel's table, which
 * TTBR1 selects on every processor.  The upper half holds the direct
 * map of RAM from 0x80000000 (the image is linked and runs in it), the
 * device window from 0xc0000000 and the uncached window from
 * 0xd0000000.  The constants are shared by the assembly sources, so they
 * are plain numbers without C suffixes where the assembler reads them.
 *
 * The platform's makefile gives ARMV7_PHYS_BASE, the physical address
 * the direct map starts at (the board's first byte of RAM),
 * ARMV7_KERNEL_PHYS, where the loader puts the image, and
 * ARMV7_LOW_RAM_SKIP, how many bytes at the start of RAM the kernel never
 * maps nor uses (a whole number of sections, zero on most boards).
 */

#ifndef KERN_HAL_ARMV7_DEFS_H
#define KERN_HAL_ARMV7_DEFS_H

#if !defined(ARMV7_PHYS_BASE) || !defined(ARMV7_KERNEL_PHYS) || !defined(ARMV7_BOOT_MAP_BYTES)
#error "the armv7 platform makefile defines ARMV7_PHYS_BASE, ARMV7_KERNEL_PHYS and ARMV7_BOOT_MAP_BYTES"
#endif
#if !defined(ARMV7_LOW_RAM_SKIP)
#error "the armv7 platform makefile defines ARMV7_LOW_RAM_SKIP"
#endif

/* Marks a parameter a function does not use. */
#define UNUSED_PARAMETER(parameter) ((void)(parameter))

/* The size of a small page, the smallest unit a table entry maps. */
#define ARMV7_PAGE_SIZE			4096
#define ARMV7_PAGE_SHIFT		12

/* The span one first-level entry maps as a section. */
#define ARMV7_SECTION_SIZE		0x100000
#define ARMV7_SECTION_SHIFT		20

/*
 * The entries of the first-level table, of its half that TTBR0 covers
 * with TTBCR.N = 1, and of one second-level table.
 */
#define ARMV7_L1_ENTRIES		4096
#define ARMV7_L1_USER_ENTRIES		2048
#define ARMV7_L2_ENTRIES		256
#define ARMV7_L2_TABLE_BYTES		1024

/*
 * The bytes of a user space's first-level table (its 2048 entries) and the
 * alignment TTBR0 asks of it with TTBCR.N = 1; and the alignment of the
 * kernel's whole table, which TTBR1 always asks for.
 */
#define ARMV7_USER_L1_BYTES		8192
#define ARMV7_KERNEL_L1_ALIGNMENT	16384

/*
 * The direct map: physical address P of RAM, from ARMV7_PHYS_BASE, is
 * reached at P - ARMV7_PHYS_BASE + ARMV7_DIRECT_BASE, up to 1 GiB of it.
 * ARMV7_VIRT_OFFSET is the distance between the two.
 */
#define ARMV7_DIRECT_BASE		0x80000000
#define ARMV7_DIRECT_LIMIT		0xc0000000
#define ARMV7_DIRECT_BYTES		0x40000000
#define ARMV7_VIRT_OFFSET		(ARMV7_DIRECT_BASE - ARMV7_PHYS_BASE)

/*
 * The device window: device registers the kernel maps are reached here,
 * one section at a time, wherever the window had room.
 */
#define ARMV7_DEVICE_BASE		0xc0000000
#define ARMV7_DEVICE_LIMIT		0xd0000000
#define ARMV7_DEVICE_SECTIONS		256

/*
 * The uncached window: RAM handed to a device that does not snoop the
 * caches is mapped a second time here, page by page and uncached
 * (hal_pmem_map_uncached()).
 */
#define ARMV7_UNCACHED_BASE		0xd0000000
#define ARMV7_UNCACHED_LIMIT		0xe0000000
#define ARMV7_UNCACHED_PAGES		65536

/* The user half: TTBR0's 2 GiB, the first page never mapped. */
#define ARMV7_USER_MINIMUM		0x1000
#define ARMV7_USER_LIMIT		0x80000000

/* The most processors the HAL keeps a record for. */
#define ARMV7_CPU_MAX			4

/*
 * First-level descriptor bits.
 *
 * Values referenced: ARM Architecture Reference Manual ARMv7-A and
 * ARMv7-R edition (ARM DDI 0406C), the short-descriptor first-level
 * formats: the type in bits 1:0, B, C, XN, the domain, AP[1:0] (AP[0] the
 * access flag with SCTLR.AFE set), TEX[2:0], AP[2], S and nG of a section,
 * and the second-level table's base above bit 10 of a table entry.
 */
#define ARMV7_L1_TYPE_MASK		0x3
#define ARMV7_L1_TYPE_TABLE		0x1
#define ARMV7_L1_TYPE_SECTION		0x2
#define ARMV7_L1_SECTION_B		0x4
#define ARMV7_L1_SECTION_C		0x8
#define ARMV7_L1_SECTION_XN		0x10
#define ARMV7_L1_SECTION_AF		0x400
#define ARMV7_L1_SECTION_AP1		0x800
#define ARMV7_L1_SECTION_TEX0		0x1000
#define ARMV7_L1_SECTION_AP2		0x8000
#define ARMV7_L1_SECTION_S		0x10000
#define ARMV7_L1_SECTION_NG		0x20000
#define ARMV7_L1_SECTION_BASE_MASK	0xfff00000
#define ARMV7_L1_TABLE_BASE_MASK	0xfffffc00

/*
 * The kernel's sections: normal memory, write-back write-allocate and
 * shareable, the kernel's own (AP[2:1] = 00) and already accessed; and
 * device memory (TEX 000, C 0, B 1, which is shareable device), never
 * executable.  locore.S builds the boot map from the first.
 */
#define ARMV7_L1_KERNEL_NORMAL		0x1140e
#define ARMV7_L1_KERNEL_DEVICE		0x416

/*
 * Second-level small-page descriptor bits.
 *
 * Values referenced: ARM DDI 0406C, the short-descriptor second-level
 * small-page format: XN in bit 0, the type in bit 1, B, C, AP[1:0], TEX[2:0],
 * AP[2], S and nG, and the page's base above bit 12.
 */
#define ARMV7_L2_XN			0x1
#define ARMV7_L2_SMALL			0x2
#define ARMV7_L2_B			0x4
#define ARMV7_L2_C			0x8
#define ARMV7_L2_AF			0x10
#define ARMV7_L2_AP1			0x20
#define ARMV7_L2_TEX0			0x40
#define ARMV7_L2_AP2			0x200
#define ARMV7_L2_S			0x400
#define ARMV7_L2_NG			0x800
#define ARMV7_L2_BASE_MASK		0xfffff000

/*
 * The bits of a shadow entry, the HAL's own record of a user page.  The
 * small-page descriptor has no bit to spare, so each user page also has
 * this entry, 2 KiB before its hardware entry in the same page of tables;
 * the hardware entry is always made from it.  A present page is readable.
 */
#define ARMV7_PTE_PRESENT		0x001
#define ARMV7_PTE_WRITE			0x002
#define ARMV7_PTE_EXEC			0x004
#define ARMV7_PTE_USER			0x008
#define ARMV7_PTE_ACCESSED		0x010
#define ARMV7_PTE_DIRTY			0x020
#define ARMV7_PTE_ATTR_SHIFT		6
#define ARMV7_PTE_ATTR_MASK		0x0c0
#define ARMV7_PTE_ATTR_NORMAL		0
#define ARMV7_PTE_ATTR_NOCACHE		1
#define ARMV7_PTE_ATTR_DEVICE		2
#define ARMV7_PTE_FRAME_MASK		0xfffff000

/*
 * Where a page of user tables keeps its parts: the shadow entries of its
 * 2 MiB at the start, the hardware table of its first megabyte at 2 KiB
 * and of its second at 3 KiB.  Two first-level entries point at the two
 * hardware tables.
 */
#define ARMV7_TABLE_PAGE_HARDWARE	0x800
#define ARMV7_TABLE_PAGE_SPAN		0x200000
#define ARMV7_TABLE_PAGE_SHIFT		21

/*
 * The modes and mask bits of the program status registers.
 *
 * Values referenced: ARM DDI 0406C, the CPSR: M[4:0] (user 0x10, FIQ 0x11,
 * IRQ 0x12, supervisor 0x13, abort 0x17, hypervisor 0x1a, undefined 0x1b,
 * system 0x1f), T, F, I, A and E.
 */
#define ARMV7_PSR_MODE_MASK		0x1f
#define ARMV7_PSR_MODE_USR		0x10
#define ARMV7_PSR_MODE_FIQ		0x11
#define ARMV7_PSR_MODE_IRQ		0x12
#define ARMV7_PSR_MODE_SVC		0x13
#define ARMV7_PSR_MODE_ABT		0x17
#define ARMV7_PSR_MODE_HYP		0x1a
#define ARMV7_PSR_MODE_UND		0x1b
#define ARMV7_PSR_MODE_SYS		0x1f
#define ARMV7_PSR_T			0x20
#define ARMV7_PSR_F			0x40
#define ARMV7_PSR_I			0x80
#define ARMV7_PSR_A			0x100
#define ARMV7_PSR_E			0x200

/*
 * The system control register bits the HAL sets and clears.
 *
 * Values referenced: ARM DDI 0406C, SCTLR: M (MMU), A (alignment check),
 * C (data cache), Z (branch prediction), I (instruction cache), V (high
 * vectors), TRE (TEX remap) and AFE (access flag enable).
 */
#define ARMV7_SCTLR_M			0x1
#define ARMV7_SCTLR_A			0x2
#define ARMV7_SCTLR_C			0x4
#define ARMV7_SCTLR_Z			0x800
#define ARMV7_SCTLR_I			0x1000
#define ARMV7_SCTLR_V			0x2000
#define ARMV7_SCTLR_TRE			0x10000000
#define ARMV7_SCTLR_AFE			0x20000000
#define ARMV7_SCTLR_SET			0x20001805
#define ARMV7_SCTLR_CLEAR		0x10002002

/*
 * The translation table walk attributes of TTBR0 and TTBR1 on a processor
 * with the multiprocessing extensions: inner and outer write-back
 * write-allocate, shareable, inner shareable.
 *
 * Values referenced: ARM DDI 0406C, TTBR0 with the multiprocessing
 * extensions: IRGN[0] in bit 6, NOS in bit 5, RGN in bits 4:3, S in bit 1
 * and IRGN[1] in bit 0.  Procedure referenced: Linux v6.12
 * arch/arm/mm/proc-v7-2level.S, TTB_FLAGS_SMP.
 */
#define ARMV7_TTBR_FLAGS		0x6a

/* TTBCR.N = 1: TTBR0 translates the lower 2 GiB and TTBR1 the rest. */
#define ARMV7_TTBCR_SPLIT		0x1

/* DACR: domain 0, the only one the tables name, is checked as a client. */
#define ARMV7_DACR_CLIENT0		0x1

/*
 * The fault status codes of the short-descriptor format, FS[4] (bit 10 of
 * the status register) above FS[3:0].
 *
 * Values referenced: ARM DDI 0406C, the DFSR and IFSR fault status encodings
 * with the short-descriptor format; the names and their order follow Linux
 * v6.12 arch/arm/mm/fsr-2level.c (fsr_info, ifsr_info), read in their
 * ARMv7 meaning with SCTLR.AFE set.
 */
#define ARMV7_FS_ALIGNMENT		0x01
#define ARMV7_FS_DEBUG			0x02
#define ARMV7_FS_AF_SECTION		0x03
#define ARMV7_FS_ICACHE			0x04
#define ARMV7_FS_TRANSLATION_SECTION	0x05
#define ARMV7_FS_AF_PAGE		0x06
#define ARMV7_FS_TRANSLATION_PAGE	0x07
#define ARMV7_FS_EXTERNAL		0x08
#define ARMV7_FS_DOMAIN_SECTION		0x09
#define ARMV7_FS_DOMAIN_PAGE		0x0b
#define ARMV7_FS_WALK_EXTERNAL_L1	0x0c
#define ARMV7_FS_PERMISSION_SECTION	0x0d
#define ARMV7_FS_WALK_EXTERNAL_L2	0x0e
#define ARMV7_FS_PERMISSION_PAGE	0x0f
#define ARMV7_FS_ASYNC_EXTERNAL		0x16

/* The data fault status register's write, external and cache-maintenance bits. */
#define ARMV7_DFSR_WNR			0x800
#define ARMV7_DFSR_EXT			0x1000
#define ARMV7_DFSR_CM			0x2000

/*
 * The trap frame: r0 to r12, the user stack pointer and link register,
 * the supervisor link register of the interrupted code, the supervisor
 * stack pointer it had, a scratch word, the return address and the saved
 * status, 80 bytes.  vectors.S and int.h agree on these offsets.  The
 * return address and the status are where srsdb puts them and rfeia takes
 * them.
 */
#define ARMV7_FRAME_SP_USR		52
#define ARMV7_FRAME_LR_USR		56
#define ARMV7_FRAME_LR_SVC		60
#define ARMV7_FRAME_SP_SVC		64
#define ARMV7_FRAME_SCRATCH		68
#define ARMV7_FRAME_PC			72
#define ARMV7_FRAME_CPSR		76
#define ARMV7_FRAME_SIZE		80

/*
 * Where struct hal_fpregs keeps the floating-point status and control
 * register, past the thirty-two doubleword registers; fpregs.S and fpu.c
 * agree on it.
 */
#define ARMV7_FPREGS_FPSCR		256

/* The exception that built a trap frame, as vectors.S passes it to C. */
#define ARMV7_TRAP_RESET		0
#define ARMV7_TRAP_UNDEFINED		1
#define ARMV7_TRAP_SVC			2
#define ARMV7_TRAP_PREFETCH_ABORT	3
#define ARMV7_TRAP_DATA_ABORT		4
#define ARMV7_TRAP_UNUSED		5
#define ARMV7_TRAP_IRQ			6
#define ARMV7_TRAP_FIQ			7

#endif
