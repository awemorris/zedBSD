/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 HAL's address layout and register numbers.
 *
 * The kernel runs in supervisor mode with Sv39 translation.  The upper half
 * of the 39-bit space holds four windows, each a run of root-table entries:
 * the direct map of RAM, the device window, an unused reserve and the
 * window the kernel image is linked in.  The constants are shared by the
 * assembly sources, so they are plain numbers without C suffixes where the
 * assembler reads them.
 */

#ifndef KERN_HAL_RISCV64_DEFS_H
#define KERN_HAL_RISCV64_DEFS_H

/* Marks a parameter a function does not use. */
#define UNUSED_PARAMETER(parameter) ((void)(parameter))

/* The size of a page, the smallest unit a table entry maps. */
#define RISCV64_PAGE_SIZE		4096
#define RISCV64_PAGE_SHIFT		12

/* The span one second-level entry maps, and one root entry. */
#define RISCV64_MEGAPAGE_SIZE		0x200000
#define RISCV64_GIGAPAGE_SIZE		0x40000000
#define RISCV64_GIGAPAGE_SHIFT		30

/* The entries of one table. */
#define RISCV64_TABLE_ENTRIES		512

/*
 * The virtual address width of the translation mode in use (Sv39), and the
 * mode number satp takes for it.
 */
#define RISCV64_VA_BITS			39
#define RISCV64_SATP_MODE_SV39		8

/*
 * The direct map: physical address P is reached at RISCV64_DIRECT_BASE + P,
 * through root entries 256 to 383 (128 GiB).
 */
#define RISCV64_DIRECT_BASE		0xffffffc000000000
#define RISCV64_DIRECT_ROOT_FIRST	256
#define RISCV64_DIRECT_ROOT_COUNT	128
#define RISCV64_DIRECT_LIMIT		0x2000000000

/*
 * The device window: device physical address P is reached at
 * RISCV64_DEVICE_BASE + P, through root entries 384 to 447 (64 GiB).
 */
#define RISCV64_DEVICE_BASE		0xffffffe000000000
#define RISCV64_DEVICE_ROOT_FIRST	384
#define RISCV64_DEVICE_ROOT_COUNT	64
#define RISCV64_DEVICE_LIMIT		0x1000000000

/*
 * The early console's window of the screen the firmware left on: the
 * screen's memory from the 2 MiB block it starts in, through root entry
 * 448 (one gigabyte).
 */
#define RISCV64_FB_WINDOW_BASE		0xfffffff000000000
#define RISCV64_FB_WINDOW_ROOT_INDEX	448

/*
 * The kernel image window: the image is linked at RISCV64_KERNEL_VIRT_BASE
 * for the physical address RISCV64_KERNEL_LINK_PHYS, and runs wherever its
 * first byte was loaded, through root entry 510.  A linked address V is
 * reached at the physical address V - RISCV64_KERNEL_VIRT_BASE plus the
 * load address.
 */
#define RISCV64_KERNEL_VIRT_BASE	0xffffffff80000000
#define RISCV64_KERNEL_LINK_PHYS	0x80200000
#define RISCV64_KERNEL_ROOT_INDEX	510
#define RISCV64_KERNEL_MAX_BYTES	0x1000000

/* The user half: root entries 0 to 255, the first page never mapped. */
#define RISCV64_USER_MINIMUM		0x1000
#define RISCV64_USER_LIMIT		0x4000000000
#define RISCV64_USER_ROOT_COUNT		256

/* The most harts the HAL brings up. */
#define RISCV64_CPU_MAX			8

/*
 * Page-table entry bits.
 *
 * Values referenced: RISC-V privileged architecture (riscv/riscv-isa-manual),
 * the Sv39 page-table entry format.
 */
#define RISCV64_PTE_V			0x001
#define RISCV64_PTE_R			0x002
#define RISCV64_PTE_W			0x004
#define RISCV64_PTE_X			0x008
#define RISCV64_PTE_U			0x010
#define RISCV64_PTE_G			0x020
#define RISCV64_PTE_A			0x040
#define RISCV64_PTE_D			0x080

/*
 * One of the two bits the hardware leaves to software: a writable leaf
 * whose page may have been written since the kernel last cleaned it.
 */
#define RISCV64_PTE_SW_DIRTY		0x100

/* The physical page number field begins at bit 10. */
#define RISCV64_PTE_PPN_SHIFT		10

/*
 * Supervisor status bits.
 *
 * Values referenced: RISC-V privileged architecture (riscv/riscv-isa-manual),
 * the sstatus register.
 */
#define RISCV64_SSTATUS_SIE		0x2
#define RISCV64_SSTATUS_SPIE		0x20
#define RISCV64_SSTATUS_SPP		0x100
#define RISCV64_SSTATUS_VS		0x600
#define RISCV64_SSTATUS_FS		0x6000
#define RISCV64_SSTATUS_FS_INITIAL	0x2000
#define RISCV64_SSTATUS_FS_CLEAN	0x4000
#define RISCV64_SSTATUS_SUM		0x40000

/*
 * The counters user mode may read: the time counter (rdtime), so that a
 * program can time its own work finer than the scheduler's tick.  The
 * cycle and instruction counters stay the kernel's.
 *
 * Values referenced: RISC-V privileged architecture (riscv/riscv-isa-manual),
 * the scounteren register (TM is bit 1).
 */
#define RISCV64_SCOUNTEREN_TM		0x2

/*
 * Supervisor interrupt bits, the same in sie and sip: software (the
 * notification between harts), timer and external (the PLIC).
 *
 * Values referenced: RISC-V privileged architecture (riscv/riscv-isa-manual),
 * the sie and sip registers.
 */
#define RISCV64_SIE_SSIE		0x2
#define RISCV64_SIE_STIE		0x20
#define RISCV64_SIE_SEIE		0x200

/*
 * Trap causes (scause): the interrupt flag is the top bit, and the codes
 * below it are the interrupts and exceptions a supervisor kernel sees.
 *
 * Values referenced: RISC-V privileged architecture (riscv/riscv-isa-manual),
 * the scause exception codes.
 */
#define RISCV64_CAUSE_INTERRUPT		0x8000000000000000
#define RISCV64_IRQ_SOFTWARE		1
#define RISCV64_IRQ_TIMER		5
#define RISCV64_IRQ_EXTERNAL		9
#define RISCV64_EXC_INSN_MISALIGNED	0
#define RISCV64_EXC_INSN_ACCESS		1
#define RISCV64_EXC_ILLEGAL_INSN	2
#define RISCV64_EXC_BREAKPOINT		3
#define RISCV64_EXC_LOAD_MISALIGNED	4
#define RISCV64_EXC_LOAD_ACCESS		5
#define RISCV64_EXC_STORE_MISALIGNED	6
#define RISCV64_EXC_STORE_ACCESS	7
#define RISCV64_EXC_USER_ECALL		8
#define RISCV64_EXC_SUPERVISOR_ECALL	9
#define RISCV64_EXC_INSN_PAGE_FAULT	12
#define RISCV64_EXC_LOAD_PAGE_FAULT	13
#define RISCV64_EXC_STORE_PAGE_FAULT	15

/*
 * The trap frame: 32 register slots (slot 0, which x0 would take, is
 * unused), then sepc, sstatus, scause and stval.  trap.S and int.h agree
 * on these offsets.
 */
#define RISCV64_FRAME_SEPC		256
#define RISCV64_FRAME_SSTATUS		264
#define RISCV64_FRAME_SCAUSE		272
#define RISCV64_FRAME_STVAL		280
#define RISCV64_FRAME_SIZE		288

/*
 * The per-CPU record fields the trap entry reads through tp: the top of the
 * running task's kernel stack, and where the user stack pointer waits
 * while the frame is built.  percpu.h checks them.
 */
#define RISCV64_PERCPU_KERNEL_SP	8
#define RISCV64_PERCPU_USER_SP		16

/*
 * A secondary hart's boot block (smp.c): the translation to start with,
 * the stack top, the logical number and the hart id.
 */
#define RISCV64_BOOT_BLOCK_SATP		0
#define RISCV64_BOOT_BLOCK_STACK	8
#define RISCV64_BOOT_BLOCK_CPU		16
#define RISCV64_BOOT_BLOCK_HART		24
#define RISCV64_BOOT_BLOCK_SIZE		64

#endif
