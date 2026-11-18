/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 kernel's first instructions, on the boot hart and on each
 * secondary hart.
 *
 * The firmware enters in supervisor mode with translation off: a0 is the
 * hart id and a1 the boot argument, at the physical address the image was
 * loaded at.  The boot hart builds the boot table, which maps the image at
 * the address it is linked at and the low physical space three ways: as
 * it is (so the instructions after the switch still fetch), through the
 * direct map and through the device window, each in gigapages.  It then
 * jumps to its linked address and enters C.  The boot table stays for the
 * secondary harts, which start on it and move to the kernel's own root.
 */

#include "defs.h"

/*
 * The entry flags of the boot table's leaves: valid, readable, writable,
 * accessed and dirty; the identity and image leaves also executable, the
 * kernel's leaves global.
 */
#define BOOT_IDENTITY_LEAF	0xcf
#define BOOT_KERNEL_LEAF	0xe7
#define BOOT_IMAGE_LEAF		0xef

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
	lla t0, riscv64_early_trap
	csrw stvec, t0

	/* No user access from the kernel and no vector unit; the FP unit on, initial. */
	li t0, RISCV64_SSTATUS_SUM | RISCV64_SSTATUS_VS | RISCV64_SSTATUS_FS
	csrc sstatus, t0
	li t0, RISCV64_SSTATUS_FS_INITIAL
	csrs sstatus, t0

	/* User mode may read the time counter, and no other counter. */
	li t0, RISCV64_SCOUNTEREN_TM
	csrw scounteren, t0

	/* Keeps the hart id, the boot argument and the image's physical start. */
	mv s0, a0
	mv s1, a1
	lla s2, _start

	/* The image must start on a megapage, which the image window maps in. */
	li t0, RISCV64_MEGAPAGE_SIZE - 1
	and t0, s2, t0
	bnez t0, riscv64_park

	/* The boot stack, at its physical address for now. */
	lla sp, riscv64_boot_stack_top

	/* C requires zeroed BSS, which holds the boot table and the stacks. */
	lla t0, __bss_start
	lla t1, __bss_end
1:	bgeu t0, t1, 2f
	sd zero, 0(t0)
	addi t0, t0, 8
	j 1b
2:

	/* Root entries 0 to 255: the low 256 GiB as it is, in gigapages. */
	lla t0, riscv64_boot_root
	li t1, 0
	li t2, BOOT_IDENTITY_LEAF
	li t3, RISCV64_USER_ROOT_COUNT
3:	slli t4, t1, 28
	or t4, t4, t2
	sd t4, 0(t0)
	addi t0, t0, 8
	addi t1, t1, 1
	bltu t1, t3, 3b

	/* Entries 256 to 383: the direct map of the low 128 GiB. */
	li t1, 0
	li t2, BOOT_KERNEL_LEAF
	li t3, RISCV64_DIRECT_ROOT_COUNT
4:	slli t4, t1, 28
	or t4, t4, t2
	sd t4, 0(t0)
	addi t0, t0, 8
	addi t1, t1, 1
	bltu t1, t3, 4b

	/* Entries 384 to 447: the device window of the low 64 GiB. */
	li t1, 0
	li t3, RISCV64_DEVICE_ROOT_COUNT
5:	slli t4, t1, 28
	or t4, t4, t2
	sd t4, 0(t0)
	addi t0, t0, 8
	addi t1, t1, 1
	bltu t1, t3, 5b

	/* Entry 510 points at the image window's table. */
	lla t0, riscv64_boot_root
	lla t1, riscv64_boot_image_table
	srli t4, t1, 2
	ori t4, t4, 1
	li t3, RISCV64_KERNEL_ROOT_INDEX * 8
	add t0, t0, t3
	sd t4, 0(t0)

	/* The image window: the image's first 16 MiB in megapages. */
	li t2, 0
	li t3, RISCV64_KERNEL_MAX_BYTES / RISCV64_MEGAPAGE_SIZE
	li t5, RISCV64_MEGAPAGE_SIZE
	mv t6, s2
6:	srli t4, t6, 2
	ori t4, t4, BOOT_IMAGE_LEAF
	sd t4, 0(t1)
	addi t1, t1, 8
	add t6, t6, t5
	addi t2, t2, 1
	bltu t2, t3, 6b

	/* Turns translation on with the boot table. */
	lla t0, riscv64_boot_root
	srli t0, t0, RISCV64_PAGE_SHIFT
	li t1, RISCV64_SATP_MODE_SV39
	slli t1, t1, 60
	or t0, t0, t1
	sfence.vma
	csrw satp, t0
	sfence.vma

	/* Jumps to the linked address of the code that follows. */
	lla t0, riscv64_high_entry_address
	ld t0, 0(t0)
	jr t0

riscv64_high_entry:
	/* The stack and the early trap at their linked addresses, then C. */
	lla sp, riscv64_boot_stack_top
	lla t0, riscv64_early_trap
	csrw stvec, t0
	mv a0, s0
	mv a1, s1
	mv a2, s2
	call riscv64_cmain
	j riscv64_park
	.size _start, . - _start

	/*
	 * The entry of a secondary hart the firmware started: a0 is its hart
	 * id and a1 the physical address of its boot block (smp.c), which
	 * holds the boot table's satp, the stack top at its linked address,
	 * the hart's logical number and its hart id.  Nothing but what the
	 * firmware promises is assumed: translation off, a0 and a1.
	 */
	.balign 4
	.global riscv64_secondary_entry
	.type riscv64_secondary_entry, %function
riscv64_secondary_entry:
	/* Makes the image's instructions visible to this hart's fetch. */
	fence.i

	/* Interrupts off and nothing pending through sscratch; early traps stop. */
	csrw sie, zero
	csrci sstatus, 2
	csrw sscratch, zero
	lla t0, riscv64_early_trap
	csrw stvec, t0

	/* No user access from the kernel and no vector unit; the FP unit on, initial. */
	li t0, RISCV64_SSTATUS_SUM | RISCV64_SSTATUS_VS | RISCV64_SSTATUS_FS
	csrc sstatus, t0
	li t0, RISCV64_SSTATUS_FS_INITIAL
	csrs sstatus, t0

	/* User mode may read the time counter, and no other counter. */
	li t0, RISCV64_SCOUNTEREN_TM
	csrw scounteren, t0

	/* Reads the boot block before translation changes. */
	ld t0, RISCV64_BOOT_BLOCK_SATP(a1)
	ld s1, RISCV64_BOOT_BLOCK_STACK(a1)
	ld s2, RISCV64_BOOT_BLOCK_CPU(a1)
	mv s0, a0

	/* Turns translation on with the boot table and jumps to the linked address. */
	sfence.vma
	csrw satp, t0
	sfence.vma
	lla t0, riscv64_secondary_high_address
	ld t0, 0(t0)
	jr t0

riscv64_secondary_high:
	/* The hart's boot stack and the early trap at their linked addresses, then C. */
	mv sp, s1
	lla t0, riscv64_early_trap
	csrw stvec, t0
	mv a0, s2
	mv a1, s0
	call riscv64_secondary_main
	j riscv64_park
	.size riscv64_secondary_entry, . - riscv64_secondary_entry

	/* A trap before the HAL's own entry is installed: the hart stops where it is. */
	.balign 4
	.global riscv64_early_trap
riscv64_early_trap:
	wfi
	j riscv64_early_trap

	/* Stops the hart for good. */
	.balign 4
	.global riscv64_park
riscv64_park:
	csrci sstatus, 2
	csrw sie, zero
1:	wfi
	j 1b

	/* The linked addresses the jumps above load. */
	.balign 8
riscv64_high_entry_address:
	.dword riscv64_high_entry
riscv64_secondary_high_address:
	.dword riscv64_secondary_high

	/*
	 * The boot table's root and the image window's table below it, which
	 * space.c also reads for the secondary harts' satp, and the boot hart's
	 * stack.  They are zeroed with the rest of the BSS before use.
	 */
	.section .bss.boot, "aw", %nobits
	.balign 4096
	.global riscv64_boot_root
riscv64_boot_root:
	.skip 4096
riscv64_boot_image_table:
	.skip 4096
	.balign 16
riscv64_boot_stack:
	.skip 16384
riscv64_boot_stack_top:
