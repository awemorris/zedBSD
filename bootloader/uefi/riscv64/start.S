/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 UEFI loader's entry.
 *
 * The firmware loads the image wherever it likes and calls the entry with
 * the image handle in a0 and the system table in a1, on its own stack.  The
 * image is linked for address 0 as a position-independent object, and the
 * firmware applies no relocation of its own (the image carries none for
 * it), so the entry first applies the image's own R_RISCV_RELATIVE
 * relocations for the address it was loaded at, then calls efi_main() and
 * returns its status to the firmware.
 */

/*
 * The relocation type the image's dynamic relocations may have, and the
 * size of one Elf64_Rela entry (offset, info, addend).
 *
 * Values referenced: riscv-non-isa/riscv-elf-psabi-doc riscv-elf.adoc
 * (R_RISCV_RELATIVE).
 */
#define RELA_RELATIVE	3
#define RELA_SIZE	24

/*
 * The entry the image's PE header names.
 *
 * Procedure referenced: Linux 6.8.12 arch/riscv/kernel/efi-header.S (an
 * EFI image built as an ELF with its own PE header in front).
 */
	.option norelax
	.section .text.start, "ax"
	.balign 4
	.global _start
	.hidden _start
	.type _start, %function
_start:
	/* Keeps the return address and the firmware's two arguments. */
	addi	sp, sp, -32
	sd	ra, 24(sp)
	sd	a0, 16(sp)
	sd	a1, 8(sp)

	/* The address the image was loaded at, and its relocation table. */
	lla	t0, __image_base
	lla	t1, __rela_start
	lla	t2, __rela_end

	/* Applies each relative relocation; any other kind stops the loader. */
1:	bgeu	t1, t2, 3f
	ld	t3, 8(t1)
	slli	t3, t3, 32
	srli	t3, t3, 32
	li	t4, RELA_RELATIVE
	bne	t3, t4, 4f
	ld	t5, 0(t1)
	ld	t6, 16(t1)
	add	t5, t5, t0
	add	t6, t6, t0
	sd	t6, 0(t5)
	addi	t1, t1, RELA_SIZE
	j	1b

	/* Runs the loader with the firmware's arguments. */
3:	ld	a0, 16(sp)
	ld	a1, 8(sp)
	call	efi_main

	/* Returns the loader's status to the firmware. */
	ld	ra, 24(sp)
	addi	sp, sp, 32
	ret

	/* A relocation the image cannot apply: EFI_LOAD_ERROR. */
4:	li	a0, 1
	slli	a0, a0, 63
	addi	a0, a0, 1
	ld	ra, 24(sp)
	addi	sp, sp, 32
	ret
	.size _start, . - _start

	.section .note.GNU-stack,"",%progbits
