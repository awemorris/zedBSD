/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 32-bit PowerPC entry, exception entry and return, task switch, the
 * first entries of new tasks, the firmware call, and the instructions the
 * C code cannot reach.
 *
 * An exception arrives at its vector with translation off.  The stub the
 * HAL copies to every vector keeps r1, r3 and the condition register in
 * SPRG1 to SPRG3, takes the task's kernel stack from SPRG0 when the
 * processor came from user mode, and jumps with the vector's offset to the
 * common entry, which builds the frame, turns translation on and calls the
 * C dispatcher.  The return restores the frame and leaves through rfi.
 */

#include "defs.h"

	.machine "ppc7400"

	.section .text.boot,"ax"
	.globl _start
	.type _start,@function
_start:
	/*
	 * C requires a zeroed BSS, which holds the stack.  r3 and r4, the
	 * initial ramdisk's address and size the loader may pass, are kept
	 * for ppc32_cmain.
	 */
	lis %r6,__bss_start@ha
	addi %r6,%r6,__bss_start@l
	lis %r7,__bss_end@ha
	addi %r7,%r7,__bss_end@l
	li %r0,0
1:	cmplw %r6,%r7
	bge 2f
	stw %r0,0(%r6)
	addi %r6,%r6,4
	b 1b
2:
	/* The firmware's client interface entry, kept for the firmware calls. */
	lis %r6,ppc32_firmware_entry@ha
	stw %r5,ppc32_firmware_entry@l(%r6)

	lis %r1,ppc32_boot_stack_top@ha
	addi %r1,%r1,ppc32_boot_stack_top@l
	li %r0,0
	stwu %r0,-16(%r1)
	bl ppc32_cmain
3:	b 3b
	.size _start,.-_start

	.text

	/*
	 * Calls the firmware's client interface with the argument block in r3.
	 */
	.globl ppc32_firmware_call
	.type ppc32_firmware_call,@function
ppc32_firmware_call:
	mflr %r0
	stwu %r1,-32(%r1)
	stw %r0,36(%r1)
	lis %r4,ppc32_firmware_entry@ha
	lwz %r4,ppc32_firmware_entry@l(%r4)
	mtctr %r4
	bctrl
	lwz %r0,36(%r1)
	mtlr %r0
	addi %r1,%r1,32
	blr
	.size ppc32_firmware_call,.-ppc32_firmware_call

	/*
	 * The stub copied to every vector: r3 becomes the vector's offset
	 * (the li below is patched) and r1 the stack the frame goes on.
	 */
	.globl ppc32_vector_stub
	.globl ppc32_vector_stub_li
	.globl ppc32_vector_stub_end
ppc32_vector_stub:
	mtsprg1 %r1
	mtsprg2 %r3
	mfcr %r3
	mtsprg3 %r3
	mfsrr1 %r3
	andi. %r3,%r3,PPC32_MSR_PR
	beq 1f
	mfsprg0 %r1
1:	subi %r1,%r1,PPC32_FRAME_SIZE + PPC32_FRAME_CALL
ppc32_vector_stub_li:
	li %r3,0
	ba ppc32_exception_common
ppc32_vector_stub_end:

	/*
	 * The common entry: the frame at r1 + 16, translation still off.
	 */
	.globl ppc32_exception_common
	.type ppc32_exception_common,@function
ppc32_exception_common:
	stw %r3,PPC32_FRAME_CALL + PPC32_FRAME_VECTOR(%r1)
	stw %r0,PPC32_FRAME_CALL + 0(%r1)
	stw %r2,PPC32_FRAME_CALL + 8(%r1)
	stmw %r4,PPC32_FRAME_CALL + 16(%r1)
	mfsprg1 %r0
	stw %r0,PPC32_FRAME_CALL + 4(%r1)
	mfsprg2 %r0
	stw %r0,PPC32_FRAME_CALL + 12(%r1)
	mfsprg3 %r0
	stw %r0,PPC32_FRAME_CALL + PPC32_FRAME_CR(%r1)
	mflr %r0
	stw %r0,PPC32_FRAME_CALL + PPC32_FRAME_LR(%r1)
	mfctr %r0
	stw %r0,PPC32_FRAME_CALL + PPC32_FRAME_CTR(%r1)
	mfxer %r0
	stw %r0,PPC32_FRAME_CALL + PPC32_FRAME_XER(%r1)
	mfsrr0 %r0
	lis %r4,ppc32_idle_nap_loop@ha
	addi %r4,%r4,ppc32_idle_nap_loop@l
	cmplw %r0,%r4
	blt 2f
	lis %r4,ppc32_idle_nap_loop_end@ha
	addi %r4,%r4,ppc32_idle_nap_loop_end@l
	cmplw %r0,%r4
	bge 2f
	lis %r4,ppc32_idle_nap_exit@ha
	addi %r4,%r4,ppc32_idle_nap_exit@l
	mr %r0,%r4
2:	stw %r0,PPC32_FRAME_CALL + PPC32_FRAME_PC(%r1)
	mfsrr1 %r0
	rlwinm %r0,%r0,0,14,12
	stw %r0,PPC32_FRAME_CALL + PPC32_FRAME_MSR(%r1)
	mfdar %r0
	stw %r0,PPC32_FRAME_CALL + PPC32_FRAME_DAR(%r1)
	mfdsisr %r0
	stw %r0,PPC32_FRAME_CALL + PPC32_FRAME_DSISR(%r1)
	li %r0,0
	stw %r0,0(%r1)

	/* Translation on, interrupts still off, then C with the frame. */
	li %r0,PPC32_MSR_KERNEL
	mtmsr %r0
	isync
	addi %r3,%r1,PPC32_FRAME_CALL
	bl ppc32_trap

	/* Returns through the frame at r1 + 16; a new user task's first entry comes here too. */
	.globl ppc32_trap_return
ppc32_trap_return:
	li %r0,PPC32_MSR_KERNEL
	mtmsr %r0
	isync
	lwz %r0,PPC32_FRAME_CALL + PPC32_FRAME_LR(%r1)
	mtlr %r0
	lwz %r0,PPC32_FRAME_CALL + PPC32_FRAME_CTR(%r1)
	mtctr %r0
	lwz %r0,PPC32_FRAME_CALL + PPC32_FRAME_XER(%r1)
	mtxer %r0
	lwz %r0,PPC32_FRAME_CALL + PPC32_FRAME_CR(%r1)
	mtcr %r0
	lwz %r0,PPC32_FRAME_CALL + PPC32_FRAME_PC(%r1)
	mtsrr0 %r0
	lwz %r0,PPC32_FRAME_CALL + PPC32_FRAME_MSR(%r1)
	mtsrr1 %r0
	lwz %r0,PPC32_FRAME_CALL + 0(%r1)
	lwz %r2,PPC32_FRAME_CALL + 8(%r1)
	lwz %r3,PPC32_FRAME_CALL + 12(%r1)
	lmw %r4,PPC32_FRAME_CALL + 16(%r1)
	lwz %r1,PPC32_FRAME_CALL + 4(%r1)
	rfi
	.size ppc32_exception_common,.-ppc32_exception_common

	/*
	 * Switches stacks: keeps r14 to r31, the link and the condition
	 * registers on the current stack, stores the stack pointer at *r3,
	 * loads the one at *r4 and pops the same words from it.
	 */
	.globl ppc32_task_dispatch
	.type ppc32_task_dispatch,@function
ppc32_task_dispatch:
	stwu %r1,-(PPC32_DISPATCH_WORDS * 4)(%r1)
	mflr %r0
	stw %r0,PPC32_DISPATCH_LR * 4(%r1)
	mfcr %r0
	stw %r0,8(%r1)
	stmw %r14,PPC32_DISPATCH_R14 * 4(%r1)
	stw %r1,0(%r3)
	lwz %r1,0(%r4)
	lmw %r14,PPC32_DISPATCH_R14 * 4(%r1)
	lwz %r0,8(%r1)
	mtcr %r0
	lwz %r0,PPC32_DISPATCH_LR * 4(%r1)
	mtlr %r0
	addi %r1,%r1,PPC32_DISPATCH_WORDS * 4
	blr
	.size ppc32_task_dispatch,.-ppc32_task_dispatch

	/*
	 * The first entry of a system task: r14 is its function and r15 the
	 * argument.  It releases the task the processor switched away from
	 * and starts with interrupts on.
	 */
	.globl ppc32_kernel_task_entry
	.type ppc32_kernel_task_entry,@function
ppc32_kernel_task_entry:
	li %r0,0
	stwu %r0,-16(%r1)
	bl ppc32_task_finish_switch
	mfmsr %r0
	ori %r0,%r0,PPC32_MSR_EE
	mtmsr %r0
	mr %r3,%r15
	mtctr %r14
	bctrl
	bl ppc32_task_returned
1:	b 1b
	.size ppc32_kernel_task_entry,.-ppc32_kernel_task_entry

	/*
	 * The first entry of a task that starts in user mode, created or
	 * forked: r1 + 16 is the frame it returns through.
	 */
	.globl ppc32_user_frame_entry
	.type ppc32_user_frame_entry,@function
ppc32_user_frame_entry:
	bl ppc32_task_finish_switch
	bl ppc32_task_first_user_entry
	b ppc32_trap_return
	.size ppc32_user_frame_entry,.-ppc32_user_frame_entry

	/* Saves f0 to f31 and the status register at r3 (struct hal_fpregs). */
	.globl ppc32_fp_save
	.type ppc32_fp_save,@function
ppc32_fp_save:
	stfd %f0,0(%r3)
	stfd %f1,8(%r3)
	stfd %f2,16(%r3)
	stfd %f3,24(%r3)
	stfd %f4,32(%r3)
	stfd %f5,40(%r3)
	stfd %f6,48(%r3)
	stfd %f7,56(%r3)
	stfd %f8,64(%r3)
	stfd %f9,72(%r3)
	stfd %f10,80(%r3)
	stfd %f11,88(%r3)
	stfd %f12,96(%r3)
	stfd %f13,104(%r3)
	stfd %f14,112(%r3)
	stfd %f15,120(%r3)
	stfd %f16,128(%r3)
	stfd %f17,136(%r3)
	stfd %f18,144(%r3)
	stfd %f19,152(%r3)
	stfd %f20,160(%r3)
	stfd %f21,168(%r3)
	stfd %f22,176(%r3)
	stfd %f23,184(%r3)
	stfd %f24,192(%r3)
	stfd %f25,200(%r3)
	stfd %f26,208(%r3)
	stfd %f27,216(%r3)
	stfd %f28,224(%r3)
	stfd %f29,232(%r3)
	stfd %f30,240(%r3)
	stfd %f31,248(%r3)
	mffs %f0
	stfd %f0,256(%r3)
	lfd %f0,0(%r3)
	lwz %r4,260(%r3)
	stw %r4,256(%r3)
	blr
	.size ppc32_fp_save,.-ppc32_fp_save

	.globl ppc32_fp_restore
	.type ppc32_fp_restore,@function
ppc32_fp_restore:
	lwz %r4,256(%r3)
	stwu %r1,-16(%r1)
	stw %r4,12(%r1)
	li %r4,0
	stw %r4,8(%r1)
	lfd %f0,8(%r1)
	mtfsf 0xff,%f0
	addi %r1,%r1,16
	lfd %f0,0(%r3)
	lfd %f1,8(%r3)
	lfd %f2,16(%r3)
	lfd %f3,24(%r3)
	lfd %f4,32(%r3)
	lfd %f5,40(%r3)
	lfd %f6,48(%r3)
	lfd %f7,56(%r3)
	lfd %f8,64(%r3)
	lfd %f9,72(%r3)
	lfd %f10,80(%r3)
	lfd %f11,88(%r3)
	lfd %f12,96(%r3)
	lfd %f13,104(%r3)
	lfd %f14,112(%r3)
	lfd %f15,120(%r3)
	lfd %f16,128(%r3)
	lfd %f17,136(%r3)
	lfd %f18,144(%r3)
	lfd %f19,152(%r3)
	lfd %f20,160(%r3)
	lfd %f21,168(%r3)
	lfd %f22,176(%r3)
	lfd %f23,184(%r3)
	lfd %f24,192(%r3)
	lfd %f25,200(%r3)
	lfd %f26,208(%r3)
	lfd %f27,216(%r3)
	lfd %f28,224(%r3)
	lfd %f29,232(%r3)
	lfd %f30,240(%r3)
	lfd %f31,248(%r3)
	blr
	.size ppc32_fp_restore,.-ppc32_fp_restore

	/*
	 * Turns translation off, loads the block translations, the segment
	 * registers and the page table, and turns translation on with the
	 * kernel's machine state.  r3 points at the block values (eight
	 * instruction then eight data words, upper then lower of each, for
	 * the low four pairs; then the same for the high four), r4 at the
	 * sixteen segment registers, r5 is the table's SDR1, and r6 is
	 * nonzero when the processor has the high pairs: they are loaded
	 * too, and HID0 turns them on.
	 */
	.globl ppc32_mmu_load
	.type ppc32_mmu_load,@function
ppc32_mmu_load:
	mflr %r10
	mfmsr %r0
	li %r7,PPC32_MSR_IR | PPC32_MSR_DR
	ori %r7,%r7,PPC32_MSR_EE
	andc %r0,%r0,%r7
	mtmsr %r0
	isync
	lwz %r0,0(%r3)
	mtspr 528,%r0
	lwz %r0,4(%r3)
	mtspr 529,%r0
	lwz %r0,8(%r3)
	mtspr 530,%r0
	lwz %r0,12(%r3)
	mtspr 531,%r0
	lwz %r0,16(%r3)
	mtspr 532,%r0
	lwz %r0,20(%r3)
	mtspr 533,%r0
	lwz %r0,24(%r3)
	mtspr 534,%r0
	lwz %r0,28(%r3)
	mtspr 535,%r0
	lwz %r0,32(%r3)
	mtspr 536,%r0
	lwz %r0,36(%r3)
	mtspr 537,%r0
	lwz %r0,40(%r3)
	mtspr 538,%r0
	lwz %r0,44(%r3)
	mtspr 539,%r0
	lwz %r0,48(%r3)
	mtspr 540,%r0
	lwz %r0,52(%r3)
	mtspr 541,%r0
	lwz %r0,56(%r3)
	mtspr 542,%r0
	lwz %r0,60(%r3)
	mtspr 543,%r0
	cmpwi %r6,0
	beq 3f
	lwz %r0,64(%r3)
	mtspr 560,%r0
	lwz %r0,68(%r3)
	mtspr 561,%r0
	lwz %r0,72(%r3)
	mtspr 562,%r0
	lwz %r0,76(%r3)
	mtspr 563,%r0
	lwz %r0,80(%r3)
	mtspr 564,%r0
	lwz %r0,84(%r3)
	mtspr 565,%r0
	lwz %r0,88(%r3)
	mtspr 566,%r0
	lwz %r0,92(%r3)
	mtspr 567,%r0
	lwz %r0,96(%r3)
	mtspr 568,%r0
	lwz %r0,100(%r3)
	mtspr 569,%r0
	lwz %r0,104(%r3)
	mtspr 570,%r0
	lwz %r0,108(%r3)
	mtspr 571,%r0
	lwz %r0,112(%r3)
	mtspr 572,%r0
	lwz %r0,116(%r3)
	mtspr 573,%r0
	lwz %r0,120(%r3)
	mtspr 574,%r0
	lwz %r0,124(%r3)
	mtspr 575,%r0
	mfspr %r0,1008
	oris %r0,%r0,PPC32_HID0_HIGH_BAT_EN_UPPER
	sync
	mtspr 1008,%r0
	isync
3:	li %r6,0
	li %r7,16
	mtctr %r7
1:	lwz %r0,0(%r4)
	mtsrin %r0,%r6
	addi %r4,%r4,4
	addis %r6,%r6,0x1000
	bdnz 1b
	mtsdr1 %r5
	sync
	li %r6,0
	li %r7,64
	mtctr %r7
2:	tlbie %r6
	addi %r6,%r6,0x1000
	bdnz 2b
	sync
	li %r0,PPC32_MSR_KERNEL
	mtsrr0 %r10
	mtsrr1 %r0
	rfi
	.size ppc32_mmu_load,.-ppc32_mmu_load

	/* Loads the user segment registers 4 to 7 from the four words at r3. */
	.globl ppc32_load_user_segments
	.type ppc32_load_user_segments,@function
ppc32_load_user_segments:
	lwz %r0,0(%r3)
	mtsr 4,%r0
	lwz %r0,4(%r3)
	mtsr 5,%r0
	lwz %r0,8(%r3)
	mtsr 6,%r0
	lwz %r0,12(%r3)
	mtsr 7,%r0
	isync
	blr
	.size ppc32_load_user_segments,.-ppc32_load_user_segments

	/* Drops the cached translation of one address, everywhere. */
	.globl ppc32_tlbie
	.type ppc32_tlbie,@function
ppc32_tlbie:
	sync
	tlbie %r3
	sync
	blr
	.size ppc32_tlbie,.-ppc32_tlbie

	/*
	 * Waits for an interrupt in the power-saving state, then closes the
	 * mask.  The state is MSR[POW] when ppc32_idle_mode is not 0 and
	 * ppc32_idle_power holds it; otherwise the wait only opens the mask
	 * once.  The power-saving state is entered from a loop that sets
	 * MSR[POW] again and again and never clears it itself: the exception
	 * entry, finding it interrupted the loop, returns to
	 * ppc32_idle_nap_exit instead, with the power-saving bit cleared, so
	 * the wait ends at the first interrupt and POW is never taken back
	 * while the processor may still be entering the state.  When ppc32_idle_prefetch holds MSSCR0
	 * bits (the 7450 family's L2 prefetch engines, which must be idle
	 * before nap), the wait clears them, flushes the image's first line
	 * four times so the engines drain, and restores MSSCR0 after the
	 * interrupt; the interrupt keeps r6 and r7.
	 */
	.globl ppc32_cpu_idle
	.type ppc32_cpu_idle,@function
ppc32_cpu_idle:
	mfmsr %r3
	li %r5,0
	li %r6,0
	lis %r4,ppc32_idle_mode@ha
	lwz %r4,ppc32_idle_mode@l(%r4)
	cmpwi %r4,0
	beq 1f
	lis %r5,ppc32_idle_power@ha
	lwz %r5,ppc32_idle_power@l(%r5)
	lis %r6,ppc32_idle_prefetch@ha
	lwz %r6,ppc32_idle_prefetch@l(%r6)
	cmpwi %r6,0
	beq 1f
	mfspr %r7,1014
	andc %r8,%r7,%r6
	sync
	mtspr 1014,%r8
	sync
	isync
	lis %r8,_start@ha
	addi %r8,%r8,_start@l
	dcbf 0,%r8
	dcbf 0,%r8
	dcbf 0,%r8
	dcbf 0,%r8
	sync
1:	ori %r4,%r3,PPC32_MSR_EE
	or %r4,%r4,%r5
	cmpwi %r5,0
	beq 2f
	.globl ppc32_idle_nap_loop
ppc32_idle_nap_loop:
	sync
	mtmsr %r4
	isync
	b ppc32_idle_nap_loop
	.globl ppc32_idle_nap_loop_end
ppc32_idle_nap_loop_end:
2:	sync
	mtmsr %r4
	isync
	.globl ppc32_idle_nap_exit
ppc32_idle_nap_exit:
	mtmsr %r3
	isync
	cmpwi %r6,0
	beqlr
	sync
	mtspr 1014,%r7
	sync
	isync
	blr
	.size ppc32_cpu_idle,.-ppc32_cpu_idle

	.section .bss.boot,"aw",@nobits
	.balign 16
ppc32_boot_stack:
	.space 16384
	.globl ppc32_boot_stack_top
ppc32_boot_stack_top:

	.balign 4
	.globl ppc32_firmware_entry
ppc32_firmware_entry:
	.space 4
