/*
 * Boots
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

	.code32
	/*
	 * B98S header, version 2.  BOOT.SYS is a two-segment ELF and this
	 * section leads the low segment at file offset 512.  file size,
	 * entry, and the payload byte-sum (file bytes from offset 532 to
	 * the end) are patched in by scripts/patch-stage2.py.
	 */
	.section .header, "a"
	.globl __image_start
__image_start:
	.ascii "B98S"
	.word 2
	.word 20
	.long 0		/* file size, patched */
	.long 0		/* absolute physical entry, patched */
	.long 0		/* payload byte-sum, patched */

	.section .note.GNU-stack, "", @progbits
