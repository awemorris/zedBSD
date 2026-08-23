/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

	.code32
	/*
	 * B98S header, version 2.  This section leads the single kernel
	 * PT_LOAD at file offset 512.  File size,
	 * entry, and the payload byte-sum (file bytes from offset 532 to
	 * the end) are patched in by platform/pc98/tools/patch-stage2.py.
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
