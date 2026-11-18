/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 loader's last step: into the kernel.
 *
 * void zbl_riscv64_enter(uint64_t boot_hart, uint64_t handoff, uint64_t entry)
 *
 * Boot services have exited.  Interrupts go off, translation goes off (a
 * firmware that ran with an identity map leaves the same addresses), the
 * instruction fetch is made to see the kernel the loader wrote as data, and
 * the kernel is entered at its physical entry with the boot hart's id in a0
 * and the handoff's physical address in a1.  The kernel sets up its own
 * stack.
 */

	.option norelax
	.text
	.balign 4
	.global zbl_riscv64_enter
	.hidden zbl_riscv64_enter
	.type zbl_riscv64_enter, %function
zbl_riscv64_enter:
	csrci	sstatus, 2
	csrw	sie, zero
	csrw	satp, zero
	sfence.vma
	fence.i
	jr	a2
	.size zbl_riscv64_enter, . - zbl_riscv64_enter

	.section .note.GNU-stack,"",%progbits
