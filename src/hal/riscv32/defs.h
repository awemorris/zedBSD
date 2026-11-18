/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv32 HAL's address layout and register numbers.
 *
 * The kernel runs in supervisor mode with Sv32 translation.  The upper half
 * of the 32-bit space holds two windows, each a run of root-table entries:
 * the direct map of RAM, at the same addresses as the RAM itself (the image
 * is linked and runs there), and the device window.  The constants are
 * shared by the assembly sources, so they are plain numbers without C
 * suffixes where the assembler reads them.
 */

#ifndef KERN_HAL_RISCV32_DEFS_H
#define KERN_HAL_RISCV32_DEFS_H

/* Marks a parameter a function does not use. */
#define UNUSED_PARAMETER(parameter) ((void)(parameter))

/* The size of a page, the smallest unit a table entry maps. */
#define RISCV32_PAGE_SIZE		4096
#define RISCV32_PAGE_SHIFT		12

/* The span one root entry maps as a leaf. */
#define RISCV32_MEGAPAGE_SIZE		0x400000
#define RISCV32_MEGAPAGE_SHIFT		22

/* The entries of one table. */
#define RISCV32_TABLE_ENTRIES		1024

/* The mode number satp takes for Sv32, in its top bit. */
#define RISCV32_SATP_MODE_SV32		0x80000000

/*
 * The direct map: physical address P of RAM is reached at the same
 * address, through root entries 512 to 767 (1 GiB from 2 GiB).  RAM
 * outside that range is not used.
 */
#define RISCV32_DIRECT_BASE		0x80000000
#define RISCV32_DIRECT_ROOT_FIRST	512
#define RISCV32_DIRECT_ROOT_COUNT	256
#define RISCV32_DIRECT_LIMIT		0xc0000000

/*
 * The device window: device physical address P below 1 GiB is reached at
 * RISCV32_DEVICE_BASE + P, through root entries 768 to 1023.
 */
#define RISCV32_DEVICE_BASE		0xc0000000
#define RISCV32_DEVICE_ROOT_FIRST	768
#define RISCV32_DEVICE_ROOT_COUNT	256
#define RISCV32_DEVICE_LIMIT		0x40000000

/*
 * The kernel image: linked at its load address, in the direct map, so a
 * linked address is its physical address.  It starts on a megapage and
 * spans at most RISCV32_KERNEL_MAX_BYTES.
 */
#define RISCV32_KERNEL_LINK_PHYS	0x80400000
#define RISCV32_KERNEL_MAX_BYTES	0x1000000

/* The user half: root entries 0 to 511, the first page never mapped. */
#define RISCV32_USER_MINIMUM		0x1000
#define RISCV32_USER_LIMIT		0x80000000
#define RISCV32_USER_ROOT_COUNT		512

/* The most harts the HAL brings up. */
#define RISCV32_CPU_MAX			8

/*
 * Page-table entry bits.
 *
 * Values referenced: RISC-V privileged architecture (riscv/riscv-isa-manual),
 * the Sv32 page-table entry format.
 */
#define RISCV32_PTE_V			0x001
#define RISCV32_PTE_R			0x002
#define RISCV32_PTE_W			0x004
#define RISCV32_PTE_X			0x008
#define RISCV32_PTE_U			0x010
#define RISCV32_PTE_G			0x020
#define RISCV32_PTE_A			0x040
#define RISCV32_PTE_D			0x080

/*
 * One of the two bits the hardware leaves to software: a writable leaf
 * whose page may have been written since the kernel last cleaned it.
 */
#define RISCV32_PTE_SW_DIRTY		0x100

/* The physical page number field begins at bit 10. */
#define RISCV32_PTE_PPN_SHIFT		10

/*
 * Supervisor status bits.
 *
 * Values referenced: RISC-V privileged architecture (riscv/riscv-isa-manual),
 * the sstatus register.
 */
#define RISCV32_SSTATUS_SIE		0x2
#define RISCV32_SSTATUS_SPIE		0x20
#define RISCV32_SSTATUS_SPP		0x100
#define RISCV32_SSTATUS_VS		0x600
#define RISCV32_SSTATUS_FS		0x6000
#define RISCV32_SSTATUS_FS_INITIAL	0x2000
#define RISCV32_SSTATUS_FS_CLEAN	0x4000
#define RISCV32_SSTATUS_SUM		0x40000

/*
 * Supervisor interrupt bits, the same in sie and sip: software (the
 * notification between harts), timer and external (the PLIC).
 *
 * Values referenced: RISC-V privileged architecture (riscv/riscv-isa-manual),
 * the sie and sip registers.
 */
#define RISCV32_SIE_SSIE		0x2
#define RISCV32_SIE_STIE		0x20
#define RISCV32_SIE_SEIE		0x200

/*
 * Trap causes (scause): the interrupt flag is the top bit, and the codes
 * below it are the interrupts and exceptions a supervisor kernel sees.
 *
 * Values referenced: RISC-V privileged architecture (riscv/riscv-isa-manual),
 * the scause exception codes.
 */
#define RISCV32_CAUSE_INTERRUPT		0x80000000
#define RISCV32_IRQ_SOFTWARE		1
#define RISCV32_IRQ_TIMER		5
#define RISCV32_IRQ_EXTERNAL		9
#define RISCV32_EXC_INSN_MISALIGNED	0
#define RISCV32_EXC_INSN_ACCESS		1
#define RISCV32_EXC_ILLEGAL_INSN	2
#define RISCV32_EXC_BREAKPOINT		3
#define RISCV32_EXC_LOAD_MISALIGNED	4
#define RISCV32_EXC_LOAD_ACCESS		5
#define RISCV32_EXC_STORE_MISALIGNED	6
#define RISCV32_EXC_STORE_ACCESS	7
#define RISCV32_EXC_USER_ECALL		8
#define RISCV32_EXC_SUPERVISOR_ECALL	9
#define RISCV32_EXC_INSN_PAGE_FAULT	12
#define RISCV32_EXC_LOAD_PAGE_FAULT	13
#define RISCV32_EXC_STORE_PAGE_FAULT	15

/*
 * The trap frame: 32 register slots (slot 0, which x0 would take, is
 * unused), then sepc, sstatus, scause and stval.  trap.S and int.h agree
 * on these offsets.
 */
#define RISCV32_FRAME_SEPC		128
#define RISCV32_FRAME_SSTATUS		132
#define RISCV32_FRAME_SCAUSE		136
#define RISCV32_FRAME_STVAL		140
#define RISCV32_FRAME_SIZE		144

/*
 * The per-CPU record fields the trap entry reads through tp: the top of the
 * running task's kernel stack, and where the user stack pointer waits
 * while the frame is built.  percpu.h checks them.
 */
#define RISCV32_PERCPU_KERNEL_SP	4
#define RISCV32_PERCPU_USER_SP		8

/*
 * A secondary hart's boot block (smp.c): the translation to start with,
 * the stack top, the logical number and the hart id.
 */
#define RISCV32_BOOT_BLOCK_SATP		0
#define RISCV32_BOOT_BLOCK_STACK	4
#define RISCV32_BOOT_BLOCK_CPU		8
#define RISCV32_BOOT_BLOCK_HART		12
#define RISCV32_BOOT_BLOCK_SIZE		64

#endif
