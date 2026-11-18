/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 64-bit PowerPC entry, exception entry and return, task switch, the
 * first entries of new tasks, the firmware and hypervisor calls, and the
 * instructions the C code cannot reach.
 *
 * An exception arrives at its vector with translation off.  The stub the
 * HAL copies to every vector keeps r1, r3 and the condition register in
 * SPRG1 to SPRG3, takes the task's kernel stack from SPRG0 when the
 * processor came from user mode, otherwise goes below the interrupted
 * code's red zone, saves r4 and the count register in the frame, and
 * jumps with the vector's offset through the count register to the
 * common entry, which completes the frame, loads the kernel's TOC, turns
 * translation on and calls the C dispatcher.  The return restores the
 * frame and leaves through rfid.
 */

#include "defs.h"

/* Loads the kernel's machine state: 64-bit mode and the low bits. */
#define LOAD_MSR_KERNEL(reg) \
	lis reg,0x8000; \
	sldi reg,reg,32; \
	ori reg,reg,PPC64_MSR_LOW_KERNEL

/* Loads the kernel's TOC pointer. */
#define LOAD_TOC \
	lis %r2,.TOC.@ha; \
	addi %r2,%r2,.TOC.@l

	.abiversion 2

	.section .text.boot,"ax"
	.globl _start
	.type _start,@function
_start:
	/* The firmware enters in 32-bit mode; the kernel runs in 64-bit mode. */
	mfmsr %r0
	lis %r6,0x8000
	sldi %r6,%r6,32
	or %r0,%r0,%r6
	mtmsrd %r0
	isync
	LOAD_TOC

	/* C requires a zeroed BSS, which holds the stack. */
	lis %r6,__bss_start@ha
	addi %r6,%r6,__bss_start@l
	lis %r7,__bss_end@ha
	addi %r7,%r7,__bss_end@l
	li %r0,0
1:	cmpld %r6,%r7
	bge 2f
	std %r0,0(%r6)
	addi %r6,%r6,8
	b 1b
2:
	/* The firmware's client interface entry, kept for the firmware calls. */
	lis %r6,ppc64_firmware_entry@ha
	std %r5,ppc64_firmware_entry@l(%r6)

	lis %r1,ppc64_boot_stack_top@ha
	addi %r1,%r1,ppc64_boot_stack_top@l
	li %r0,0
	stdu %r0,-32(%r1)
	bl ppc64_cmain
3:	b 3b
	.size _start,.-_start

	.text

	/*
	 * Calls the firmware's client interface with the argument block in r3,
	 * in 32-bit mode, as the firmware runs.
	 */
	.globl ppc64_firmware_call
	.type ppc64_firmware_call,@function
ppc64_firmware_call:
	mflr %r0
	std %r0,16(%r1)
	stdu %r1,-64(%r1)
	std %r2,24(%r1)
	mfmsr %r0
	std %r0,32(%r1)
	lis %r4,ppc64_firmware_entry@ha
	ld %r4,ppc64_firmware_entry@l(%r4)
	mtctr %r4
	clrldi %r0,%r0,1
	mtmsrd %r0
	isync
	bctrl
	ld %r0,32(%r1)
	mtmsrd %r0
	isync
	ld %r2,24(%r1)
	addi %r1,%r1,64
	ld %r0,16(%r1)
	mtlr %r0
	blr
	.size ppc64_firmware_call,.-ppc64_firmware_call

	/*
	 * Makes a hypervisor call: r3 is the call, r4 points at eight
	 * doublewords that go in r4 to r11 and come back from them.  Returns
	 * the hypervisor's status.
	 */
	.globl ppc64_hcall
	.type ppc64_hcall,@function
ppc64_hcall:
	std %r4,-8(%r1)
	mr %r12,%r4
	ld %r4,0(%r12)
	ld %r5,8(%r12)
	ld %r6,16(%r12)
	ld %r7,24(%r12)
	ld %r8,32(%r12)
	ld %r9,40(%r12)
	ld %r10,48(%r12)
	ld %r11,56(%r12)
	sc 1
	ld %r12,-8(%r1)
	std %r4,0(%r12)
	std %r5,8(%r12)
	std %r6,16(%r12)
	std %r7,24(%r12)
	std %r8,32(%r12)
	std %r9,40(%r12)
	std %r10,48(%r12)
	std %r11,56(%r12)
	blr
	.size ppc64_hcall,.-ppc64_hcall

	/*
	 * The stub copied to every vector: r3 becomes the vector's offset
	 * (the li below is patched) and r1 the stack the frame goes on.
	 */
	.globl ppc64_vector_stub
	.globl ppc64_vector_stub_li
	.globl ppc64_vector_stub_end
ppc64_vector_stub:
	mtsprg1 %r1
	mtsprg2 %r3
	mfcr %r3
	mtsprg3 %r3
	mfsrr1 %r3
	andi. %r3,%r3,PPC64_MSR_PR
	beq 1f
	mfsprg0 %r1
	addi %r1,%r1,PPC64_RED_ZONE
1:	subi %r1,%r1,PPC64_FRAME_SIZE + PPC64_FRAME_CALL + PPC64_RED_ZONE
	std %r4,PPC64_FRAME_CALL + 32(%r1)
	mfctr %r4
	std %r4,PPC64_FRAME_CALL + PPC64_FRAME_CTR(%r1)
	lis %r4,ppc64_exception_common@ha
	addi %r4,%r4,ppc64_exception_common@l
	mtctr %r4
ppc64_vector_stub_li:
	li %r3,0
	bctr
ppc64_vector_stub_end:

	/*
	 * The common entry: the frame at r1 + 32, translation still off.
	 */
	.globl ppc64_exception_common
	.type ppc64_exception_common,@function
ppc64_exception_common:
	std %r3,PPC64_FRAME_CALL + PPC64_FRAME_VECTOR(%r1)
	std %r0,PPC64_FRAME_CALL + 0(%r1)
	std %r2,PPC64_FRAME_CALL + 16(%r1)
	std %r5,PPC64_FRAME_CALL + 40(%r1)
	std %r6,PPC64_FRAME_CALL + 48(%r1)
	std %r7,PPC64_FRAME_CALL + 56(%r1)
	std %r8,PPC64_FRAME_CALL + 64(%r1)
	std %r9,PPC64_FRAME_CALL + 72(%r1)
	std %r10,PPC64_FRAME_CALL + 80(%r1)
	std %r11,PPC64_FRAME_CALL + 88(%r1)
	std %r12,PPC64_FRAME_CALL + 96(%r1)
	std %r13,PPC64_FRAME_CALL + 104(%r1)
	std %r14,PPC64_FRAME_CALL + 112(%r1)
	std %r15,PPC64_FRAME_CALL + 120(%r1)
	std %r16,PPC64_FRAME_CALL + 128(%r1)
	std %r17,PPC64_FRAME_CALL + 136(%r1)
	std %r18,PPC64_FRAME_CALL + 144(%r1)
	std %r19,PPC64_FRAME_CALL + 152(%r1)
	std %r20,PPC64_FRAME_CALL + 160(%r1)
	std %r21,PPC64_FRAME_CALL + 168(%r1)
	std %r22,PPC64_FRAME_CALL + 176(%r1)
	std %r23,PPC64_FRAME_CALL + 184(%r1)
	std %r24,PPC64_FRAME_CALL + 192(%r1)
	std %r25,PPC64_FRAME_CALL + 200(%r1)
	std %r26,PPC64_FRAME_CALL + 208(%r1)
	std %r27,PPC64_FRAME_CALL + 216(%r1)
	std %r28,PPC64_FRAME_CALL + 224(%r1)
	std %r29,PPC64_FRAME_CALL + 232(%r1)
	std %r30,PPC64_FRAME_CALL + 240(%r1)
	std %r31,PPC64_FRAME_CALL + 248(%r1)
	mfsprg1 %r0
	std %r0,PPC64_FRAME_CALL + 8(%r1)
	mfsprg2 %r0
	std %r0,PPC64_FRAME_CALL + 24(%r1)
	mfsprg3 %r0
	std %r0,PPC64_FRAME_CALL + PPC64_FRAME_CR(%r1)
	mflr %r0
	std %r0,PPC64_FRAME_CALL + PPC64_FRAME_LR(%r1)
	mfxer %r0
	std %r0,PPC64_FRAME_CALL + PPC64_FRAME_XER(%r1)
	mfsrr0 %r0
	std %r0,PPC64_FRAME_CALL + PPC64_FRAME_PC(%r1)
	mfsrr1 %r0
	std %r0,PPC64_FRAME_CALL + PPC64_FRAME_MSR(%r1)
	mfdar %r0
	std %r0,PPC64_FRAME_CALL + PPC64_FRAME_DAR(%r1)
	mfdsisr %r0
	std %r0,PPC64_FRAME_CALL + PPC64_FRAME_DSISR(%r1)
	li %r0,0
	std %r0,0(%r1)

	/* The kernel's TOC, translation on, interrupts still off, then C with the frame. */
	LOAD_TOC
	LOAD_MSR_KERNEL(%r0)
	mtmsrd %r0
	isync
	addi %r3,%r1,PPC64_FRAME_CALL
	bl ppc64_trap

	/* Returns through the frame at r1 + 32; a new user task's first entry comes here too. */
	.globl ppc64_trap_return
ppc64_trap_return:
	LOAD_MSR_KERNEL(%r0)
	mtmsrd %r0
	isync
	ld %r0,PPC64_FRAME_CALL + PPC64_FRAME_LR(%r1)
	mtlr %r0
	ld %r0,PPC64_FRAME_CALL + PPC64_FRAME_CTR(%r1)
	mtctr %r0
	ld %r0,PPC64_FRAME_CALL + PPC64_FRAME_XER(%r1)
	mtxer %r0
	ld %r0,PPC64_FRAME_CALL + PPC64_FRAME_CR(%r1)
	mtcr %r0
	ld %r0,PPC64_FRAME_CALL + PPC64_FRAME_PC(%r1)
	mtsrr0 %r0
	ld %r0,PPC64_FRAME_CALL + PPC64_FRAME_MSR(%r1)
	mtsrr1 %r0
	ld %r0,PPC64_FRAME_CALL + 0(%r1)
	ld %r2,PPC64_FRAME_CALL + 16(%r1)
	ld %r3,PPC64_FRAME_CALL + 24(%r1)
	ld %r4,PPC64_FRAME_CALL + 32(%r1)
	ld %r5,PPC64_FRAME_CALL + 40(%r1)
	ld %r6,PPC64_FRAME_CALL + 48(%r1)
	ld %r7,PPC64_FRAME_CALL + 56(%r1)
	ld %r8,PPC64_FRAME_CALL + 64(%r1)
	ld %r9,PPC64_FRAME_CALL + 72(%r1)
	ld %r10,PPC64_FRAME_CALL + 80(%r1)
	ld %r11,PPC64_FRAME_CALL + 88(%r1)
	ld %r12,PPC64_FRAME_CALL + 96(%r1)
	ld %r13,PPC64_FRAME_CALL + 104(%r1)
	ld %r14,PPC64_FRAME_CALL + 112(%r1)
	ld %r15,PPC64_FRAME_CALL + 120(%r1)
	ld %r16,PPC64_FRAME_CALL + 128(%r1)
	ld %r17,PPC64_FRAME_CALL + 136(%r1)
	ld %r18,PPC64_FRAME_CALL + 144(%r1)
	ld %r19,PPC64_FRAME_CALL + 152(%r1)
	ld %r20,PPC64_FRAME_CALL + 160(%r1)
	ld %r21,PPC64_FRAME_CALL + 168(%r1)
	ld %r22,PPC64_FRAME_CALL + 176(%r1)
	ld %r23,PPC64_FRAME_CALL + 184(%r1)
	ld %r24,PPC64_FRAME_CALL + 192(%r1)
	ld %r25,PPC64_FRAME_CALL + 200(%r1)
	ld %r26,PPC64_FRAME_CALL + 208(%r1)
	ld %r27,PPC64_FRAME_CALL + 216(%r1)
	ld %r28,PPC64_FRAME_CALL + 224(%r1)
	ld %r29,PPC64_FRAME_CALL + 232(%r1)
	ld %r30,PPC64_FRAME_CALL + 240(%r1)
	ld %r31,PPC64_FRAME_CALL + 248(%r1)
	ld %r1,PPC64_FRAME_CALL + 8(%r1)
	rfid
	.size ppc64_exception_common,.-ppc64_exception_common

	/*
	 * Switches stacks: keeps r14 to r31, the TOC, the link and the
	 * condition registers on the current stack, stores the stack pointer
	 * at *r3, loads the one at *r4 and pops the same words from it.  r12
	 * carries the return address, so that a new stack's first function,
	 * entered at its global entry point, finds its TOC from it.
	 */
	.globl ppc64_task_dispatch
	.type ppc64_task_dispatch,@function
ppc64_task_dispatch:
	stdu %r1,-(PPC64_DISPATCH_WORDS * 8)(%r1)
	mfcr %r0
	std %r0,PPC64_DISPATCH_CR * 8(%r1)
	mflr %r0
	std %r0,PPC64_DISPATCH_LR * 8(%r1)
	std %r2,24(%r1)
	std %r14,32(%r1)
	std %r15,40(%r1)
	std %r16,48(%r1)
	std %r17,56(%r1)
	std %r18,64(%r1)
	std %r19,72(%r1)
	std %r20,80(%r1)
	std %r21,88(%r1)
	std %r22,96(%r1)
	std %r23,104(%r1)
	std %r24,112(%r1)
	std %r25,120(%r1)
	std %r26,128(%r1)
	std %r27,136(%r1)
	std %r28,144(%r1)
	std %r29,152(%r1)
	std %r30,160(%r1)
	std %r31,168(%r1)
	std %r1,0(%r3)
	ld %r1,0(%r4)
	ld %r14,32(%r1)
	ld %r15,40(%r1)
	ld %r16,48(%r1)
	ld %r17,56(%r1)
	ld %r18,64(%r1)
	ld %r19,72(%r1)
	ld %r20,80(%r1)
	ld %r21,88(%r1)
	ld %r22,96(%r1)
	ld %r23,104(%r1)
	ld %r24,112(%r1)
	ld %r25,120(%r1)
	ld %r26,128(%r1)
	ld %r27,136(%r1)
	ld %r28,144(%r1)
	ld %r29,152(%r1)
	ld %r30,160(%r1)
	ld %r31,168(%r1)
	ld %r2,24(%r1)
	ld %r0,PPC64_DISPATCH_CR * 8(%r1)
	mtcr %r0
	ld %r0,PPC64_DISPATCH_LR * 8(%r1)
	mtlr %r0
	mr %r12,%r0
	addi %r1,%r1,PPC64_DISPATCH_WORDS * 8
	blr
	.size ppc64_task_dispatch,.-ppc64_task_dispatch

	/*
	 * The first entry of a system task: r14 is its function and r15 the
	 * argument.  It releases the task the processor switched away from
	 * and starts with interrupts on.
	 */
	.globl ppc64_kernel_task_entry
	.type ppc64_kernel_task_entry,@function
ppc64_kernel_task_entry:
	LOAD_TOC
	li %r0,0
	stdu %r0,-32(%r1)
	bl ppc64_task_finish_switch
	mfmsr %r0
	ori %r0,%r0,PPC64_MSR_EE
	mtmsrd %r0
	mr %r3,%r15
	mtctr %r14
	mr %r12,%r14
	bctrl
	LOAD_TOC
	bl ppc64_task_returned
1:	b 1b
	.size ppc64_kernel_task_entry,.-ppc64_kernel_task_entry

	/*
	 * The first entry of a task that starts in user mode, created or
	 * forked: r1 + 32 is the frame it returns through.
	 */
	.globl ppc64_user_frame_entry
	.type ppc64_user_frame_entry,@function
ppc64_user_frame_entry:
	LOAD_TOC
	bl ppc64_task_finish_switch
	bl ppc64_task_first_user_entry
	b ppc64_trap_return
	.size ppc64_user_frame_entry,.-ppc64_user_frame_entry

	/* Saves f0 to f31 and the status and control register at r3 (struct hal_fpregs). */
	.globl ppc64_fp_save
	.type ppc64_fp_save,@function
ppc64_fp_save:
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
	li %r4,0
	stw %r4,260(%r3)
	blr
	.size ppc64_fp_save,.-ppc64_fp_save

	.globl ppc64_fp_restore
	.type ppc64_fp_restore,@function
ppc64_fp_restore:
	lwz %r4,256(%r3)
	stdu %r1,-32(%r1)
	li %r5,0
	stw %r5,16(%r1)
	stw %r4,20(%r1)
	lfd %f0,16(%r1)
	mtfsf 0xff,%f0
	addi %r1,%r1,32
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
	.size ppc64_fp_restore,.-ppc64_fp_restore

	.section .bss.boot,"aw",@nobits
	.balign 16
ppc64_boot_stack:
	.space 16384
	.globl ppc64_boot_stack_top
ppc64_boot_stack_top:
	.balign 8
	.globl ppc64_firmware_entry
ppc64_firmware_entry:
	.space 8
