/*
 * SPARC V9 kernel entry skeleton.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

	.section ".text.boot", #alloc, #execinstr
	.balign 8192
	.global _start
	.type _start, #function
_start:
	flushw
	setx	sparcv9_boot_stack_top, %g1, %sp
	sub	%sp, 2047, %sp
	setx	sparcv9_boot_client, %g1, %l0
	stx	%o0, [%l0]
	setx	sparcv9_boot_handoff, %g1, %l0
	stx	%o2, [%l0]
	call	sparcv9_cmain
	 mov	%o2, %o0
1:
	ba	1b
	 nop
	.size _start, .-_start

	.section ".bss.boot", #alloc, #write
	.balign 8192
	.global sparcv9_boot_client
sparcv9_boot_client:
	.skip 8
	.global sparcv9_boot_handoff
sparcv9_boot_handoff:
	.skip 8
sparcv9_boot_stack:
	.skip 32768
sparcv9_boot_stack_top:
	/* Reserve ABI register-window spill slots before the following BSS. */
	.skip 4096
