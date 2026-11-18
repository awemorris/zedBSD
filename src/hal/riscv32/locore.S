/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv32 kernel's first instructions, on the boot hart and on each
 * secondary hart.
 *
 * The firmware enters in supervisor mode with translation off: a0 is the
 * hart id and a1 the boot argument, at the physical address the image was
 * linked for.  The boot hart builds the boot table, which maps the high
 * 1 GiB of physical space from 2 GiB as it is (the image and the direct
 * map are both there) and the low 1 GiB through the device window, each in
 * megapages, and enters C at once: the image needs no jump, since it runs
 * at the addresses it is linked at.  The boot table stays for the
 * secondary harts, which start on it and move to the kernel's own root.
 */

#include "defs.h"

/*
 * The entry flags of the boot table's leaves: valid, readable, writable,
 * accessed and dirty, global; the direct map also executable.
 */
#define BOOT_DIRECT_LEAF	0xef
#define BOOT_DEVICE_LEAF	0xe7

	.option norelax

	.section .text.boot, "ax"
	.balign 4
	.global _start
	.type _start, %function
_start:
	/* Makes the instructions the loader wrote visible to this hart's fetch. */
	fence.i

	/* Interrupts off and nothing pending through sscratch; early traps stop. */
	csrw sie, zero
	csrci sstatus, 2
	csrw sscratch, zero
	lla t0, riscv32_early_trap
	csrw stvec, t0

	/* No user access from the kernel and no vector unit; the FP unit on, initial. */
	li t0, RISCV32_SSTATUS_SUM | RISCV32_SSTATUS_VS | RISCV32_SSTATUS_FS
	csrc sstatus, t0
	li t0, RISCV32_SSTATUS_FS_INITIAL
	csrs sstatus, t0

	/* Keeps the hart id, the boot argument and the image's physical start. */
	mv s0, a0
	mv s1, a1
	lla s2, _start

	/* The image must be where it is linked, which the boot table maps as it is. */
	li t0, RISCV32_KERNEL_LINK_PHYS
	bne t0, s2, riscv32_park

	/* The boot stack. */
	lla sp, riscv32_boot_stack_top

	/* C requires zeroed BSS, which holds the boot table and the stacks. */
	lla t0, __bss_start
	lla t1, __bss_end
1:	bgeu t0, t1, 2f
	sw zero, 0(t0)
	addi t0, t0, 4
	j 1b
2:

	/* Root entries 512 to 767: physical 2 GiB to 3 GiB as it is. */
	lla t0, riscv32_boot_root
	li t3, RISCV32_DIRECT_ROOT_FIRST * 4
	add t0, t0, t3
	li t1, RISCV32_DIRECT_ROOT_FIRST
	li t2, BOOT_DIRECT_LEAF
	li t3, RISCV32_DIRECT_ROOT_FIRST + RISCV32_DIRECT_ROOT_COUNT
3:	slli t4, t1, 20
	or t4, t4, t2
	sw t4, 0(t0)
	addi t0, t0, 4
	addi t1, t1, 1
	bltu t1, t3, 3b

	/* Entries 768 to 1023: the device window of the low 1 GiB. */
	li t1, 0
	li t2, BOOT_DEVICE_LEAF
	li t3, RISCV32_DEVICE_ROOT_COUNT
4:	slli t4, t1, 20
	or t4, t4, t2
	sw t4, 0(t0)
	addi t0, t0, 4
	addi t1, t1, 1
	bltu t1, t3, 4b

	/* Turns translation on with the boot table. */
	lla t0, riscv32_boot_root
	srli t0, t0, RISCV32_PAGE_SHIFT
	li t1, RISCV32_SATP_MODE_SV32
	or t0, t0, t1
	sfence.vma
	csrw satp, t0
	sfence.vma

	/* The early trap is already at its linked address; enters C. */
	mv a0, s0
	mv a1, s1
	mv a2, s2
	call riscv32_cmain
	j riscv32_park
	.size _start, . - _start

	/*
	 * The entry of a secondary hart the firmware started: a0 is its hart
	 * id and a1 the physical address of its boot block (smp.c), which
	 * holds the boot table's satp, the stack top at its linked address,
	 * the hart's logical number and its hart id.  Nothing but what the
	 * firmware promises is assumed: translation off, a0 and a1.
	 */
	.balign 4
	.global riscv32_secondary_entry
	.type riscv32_secondary_entry, %function
riscv32_secondary_entry:
	fence.i
	csrw sie, zero
	csrci sstatus, 2
	csrw sscratch, zero
	lla t0, riscv32_early_trap
	csrw stvec, t0
	li t0, RISCV32_SSTATUS_SUM | RISCV32_SSTATUS_VS | RISCV32_SSTATUS_FS
	csrc sstatus, t0
	li t0, RISCV32_SSTATUS_FS_INITIAL
	csrs sstatus, t0

	/* Reads the boot block, then turns translation on with the boot table. */
	lw t0, RISCV32_BOOT_BLOCK_SATP(a1)
	lw s1, RISCV32_BOOT_BLOCK_STACK(a1)
	lw s2, RISCV32_BOOT_BLOCK_CPU(a1)
	mv s0, a0
	sfence.vma
	csrw satp, t0
	sfence.vma

	mv sp, s1
	mv a0, s2
	mv a1, s0
	call riscv32_secondary_main
	j riscv32_park
	.size riscv32_secondary_entry, . - riscv32_secondary_entry

	/* A trap before the HAL's own entry is installed: the hart stops where it is. */
	.balign 4
	.global riscv32_early_trap
riscv32_early_trap:
	wfi
	j riscv32_early_trap

	/* Stops the hart for good. */
	.balign 4
	.global riscv32_park
riscv32_park:
	csrci sstatus, 2
	csrw sie, zero
1:	wfi
	j 1b

	.section .bss.boot, "aw", %nobits
	.balign 4096
	.global riscv32_boot_root
riscv32_boot_root:
	.skip 4096
	.balign 16
riscv32_boot_stack:
	.skip 16384
riscv32_boot_stack_top:
