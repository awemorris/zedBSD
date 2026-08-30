/* SPDX-License-Identifier: Zlib */
/*
 * Generic PC-98 disk IPL selector.
 * Copyright (C) 2026 Awe Morris
 *
 * The ROM loads this 512-byte sector at 1fc0:0000.  It relocates itself and,
 * without changing the display or waiting for input, loads exactly one
 * sector from LBA 2 of the current fixed disk back at the firmware load
 * address.  Code installed at LBA 2 is therefore independent of zedBSD and
 * may implement its own continuation-sector format.
 */

	.code16
	.section .text
	.globl _start

	.set RELOC_SEG,       0x9000
	.set LOAD_SEG,        0x1fc0

_start:
	jmp	relocate
	nop
	nop
	.ascii	"IPL1"
	.byte	0, 0, 0
	.byte	0x1e

relocate:
	cli
	cld
	/* Do not depend on the firmware's stack.  Some real PC-9821 ROMs enter
	 * the IPL with an SS:SP pair which is only suitable for firmware use. */
	movw	%cs, %ax
	movw	%ax, %ds
	movw	%si, saved_si
	movw	%di, saved_di
	movw	$RELOC_SEG, %ax
	movw	%ax, %ss
	movw	$0xfffe, %sp
	movw	%ax, %es
	xorw	%si, %si
	xorw	%di, %di
	movw	$256, %cx
	rep movsw
	ljmp	$RELOC_SEG, $relocated

relocated:
	movw	%cs, %ax
	movw	%ax, %ds
	sti
#if ZEDBSD_PC98_IPL_DIAGNOSTIC
	/* One bounded tone means that the ROM entered LBA 0 and this IPL now
	 * owns its stack.  Keep all physical tracing out of the normal image. */
	movw	$0x4531, %dx	/* "1E": Stage 1 entry. */
	movw	$1, %cx
	call	diagnostic_checkpoint
#endif
	/* The fixed-disk IPL has one job: enter the installed LBA 2 loader. */
	xorw	%ax, %ax
	movw	%ax, %ds
	movb	0x0584, %al
	movw	%cs, %dx
	movw	%dx, %ds
	andb	$0x7f, %al
	orb	$0x80, %al
	movb	%al, boot_id
	call	bios_sense
	jc	boot_failure_sense
	/* AH=84 is allowed to alter ES:BP, so establish the read destination
	 * only after SENSE and immediately before the AH=06 transfer. */
	call	prepare_target
	xorw	%ax, %ax
	movw	%ax, %ds
	movb	%cs:boot_id, %al
	movb	$0x06, %ah
	movw	$512, %bx
	xorw	%cx, %cx
	movw	$0x0002, %dx /* C=0, H=0, zero-based sector 2 */
	movw	%cs:saved_si, %si
	movw	%cs:saved_di, %di
	int	$0x1b
	jc	boot_failure_read

enter_target:
	xorw	%ax, %ax
	movw	%ax, %ds
	movb	%cs:boot_id, %al
	movb	%al, 0x0584
	movw	%cs:saved_si, %si
	movw	%cs:saved_di, %di
	sti
	ljmp	$LOAD_SEG, $0

prepare_target:
	movw	$LOAD_SEG, %ax
	movw	%ax, %es
	xorw	%bp, %bp
	ret

bios_sense:
	pushw	%ds
	xorw	%ax, %ax
	movw	%ax, %ds
	movb	%cs:boot_id, %al
	movb	$0x84, %ah
	movw	$0x0100, %bx
	movw	%cs:saved_si, %si
	movw	%cs:saved_di, %di
	int	$0x1b
	jc	1f
	cmpb	$0x20, %ah
	jae	1f
	cmpw	$512, %bx
	jne	1f
	testb	%dh, %dh
	jz	1f
	testb	%dl, %dl
	jz	1f
	popw	%ds
	clc
	ret
1:
	popw	%ds
	stc
	ret

boot_failure_sense:
	movw	$0x5331, %dx	/* "1S": LBA 0 SENSE failed. */
#if ZEDBSD_PC98_IPL_DIAGNOSTIC
	movw	$2, %cx
#endif
	jmp	boot_failure
boot_failure_read:
	movw	$0x5231, %dx	/* "1R": LBA 2 read failed. */
#if ZEDBSD_PC98_IPL_DIAGNOSTIC
	movw	$3, %cx
#endif
boot_failure:
	/* Failure-only physical diagnostic.  PC-98 text characters and their
	 * attributes occupy corresponding words at A000: and A200:. */
	movw	$0xa000, %ax
	movw	%ax, %es
	xorw	%bx, %bx
	xorw	%ax, %ax
	movb	%dl, %al
	movw	%ax, %es:(%bx)
	movb	%dh, %al
	movw	%ax, %es:2(%bx)
	movw	$0xa200, %ax
	movw	%ax, %es
	movw	$0x00e1, %es:(%bx)
	movw	$0x00e1, %es:2(%bx)
#if ZEDBSD_PC98_IPL_DIAGNOSTIC
	call	diagnostic_checkpoint
#else
	movb	%dl, %al
	outb	%al, $0xe9
	movb	%dh, %al
	outb	%al, $0xe9
	movb	$0x06, %al
	outb	%al, $0x37
#endif
1:
	hlt
	jmp	1b

#if ZEDBSD_PC98_IPL_DIAGNOSTIC
/*
 * Emit P<DX> to QEMU debug port E9 and a finite group of CX short tones.
 * IRQ0 supplies the delay, so the group remains countable across CPU speeds.
 * The caller has already established the private stack and enabled IRQs.
 */
diagnostic_checkpoint:
	pushw	%ax
	pushw	%bx
	pushw	%cx
	pushw	%dx
	movb	$'P', %al
	outb	%al, $0xe9
	movb	%dl, %al
	outb	%al, $0xe9
	movb	%dh, %al
	outb	%al, $0xe9
	movb	$'\n', %al
	outb	%al, $0xe9

	movb	%cl, %bl
1:
	/* AH=06h starts the standard PC-98 BIOS timed beep and, unlike a raw
	 * speaker write, also supplies the timer interrupts used by HLT below. */
	movb	$0x06, %ah
	movw	$8, %cx
	movw	$1000, %dx
	int	$0x1c
	movw	$8, %cx
	call	diagnostic_wait_ticks
	/* Start a second timed beep but gate the speaker off immediately.  Its
	 * active BIOS timer provides a CPU-speed-independent inter-tone pause. */
	movb	$0x06, %ah
	movw	$8, %cx
	movw	$1000, %dx
	int	$0x1c
	movb	$0x07, %al
	outb	%al, $0x37	/* speaker off */
	movw	$8, %cx
	call	diagnostic_wait_ticks
	decb	%bl
	jnz	1b
	/* Separate this group from the next-stage checkpoint. */
	movb	$0x06, %ah
	movw	$32, %cx
	movw	$1000, %dx
	int	$0x1c
	movb	$0x07, %al
	outb	%al, $0x37
	movw	$32, %cx
	call	diagnostic_wait_ticks
	popw	%dx
	popw	%cx
	popw	%bx
	popw	%ax
	ret

diagnostic_wait_ticks:
	sti
1:
	hlt
	loop	1b
	ret
#endif

saved_si:
	.word	0
saved_di:
	.word	0
boot_id:
	.byte	0

	.org	0x01fc
	.byte	9		/* PC-98 boot-menu version. */
	.byte	0		/* Reserved. */
	/* Required by later PC-9821 firmware before it will execute the IPL. */
	.word	0xaa55
