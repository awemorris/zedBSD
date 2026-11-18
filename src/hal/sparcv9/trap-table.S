/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The trap table: two banks of 512 entries of 32 bytes, the first for
 * traps taken at trap level 0 and the second for those taken above it.
 */

	.section ".trap_table", #alloc, #execinstr
	.global sparcv9_trap_table
	.type sparcv9_trap_table, #object
sparcv9_trap_table:
	.macro trap_bank
	.set trap_type, 0
	.rept 512
	.if (trap_type >= 0x80) && (trap_type <= 0xbf)
		ba,a sparcv9_window_spill
		.skip 28
	.elseif (trap_type >= 0xc0) && (trap_type <= 0xff)
		ba,a sparcv9_window_fill
		.skip 28
	.elseif trap_type == 0x64
		ba,a sparcv9_fast_instruction_miss
		.skip 28
	.elseif trap_type == 0x68
		ba,a sparcv9_fast_data_miss
		.skip 28
	.else
		ba sparcv9_trap_entry
		 nop
		.skip 24
	.endif
	.set trap_type, trap_type + 1
	.endr
	.endm
	trap_bank
	/* TL > 1 selects the second complete 16 KiB bank. */
	trap_bank
	.size sparcv9_trap_table, .-sparcv9_trap_table
