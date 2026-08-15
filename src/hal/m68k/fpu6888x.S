/* Dedicated MC68881/MC68882 context operations; kernel C stays soft-float. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

	.text
	.even

	.globl m68k6888x_save_raw
	.type m68k6888x_save_raw,@function
m68k6888x_save_raw:
	move.l 4(%sp),%a0
	move.l 8(%sp),%a1
	move.l 12(%sp),%a2
	/* MC68881UM 6.4: suspend/internal FSAVE before programmer model. */
	fsave (%a0)
	clr.l %d0
	move.b 1(%a0),%d0
	addq.l #4,%d0
	tst.b (%a0)
	beq.s 1f
	fmovem.x %fp0-%fp7,(%a1)
	fmovem.l %fpcr/%fpsr/%fpiar,(%a2)
1:	rts
	.size m68k6888x_save_raw,.-m68k6888x_save_raw

	.globl m68k6888x_restore_raw
	.type m68k6888x_restore_raw,@function
m68k6888x_restore_raw:
	move.l 4(%sp),%a0
	move.l 8(%sp),%a1
	move.l 12(%sp),%a2
	/* Programmer model first, then FRESTORE resumes invisible state.  A NULL
	 * frame is restored alone because it resets the programmer model too. */
	tst.b (%a0)
	beq.s 2f
	fmovem.x (%a1),%fp0-%fp7
	fmovem.l (%a2),%fpcr/%fpsr/%fpiar
2:	frestore (%a0)
	rts
	.size m68k6888x_restore_raw,.-m68k6888x_restore_raw

	/* Recoverable, tightly-scoped F-line probe.  VBR must already point at
	 * writable RAM.  Interrupts are masked while vector 11 is replaced and
	 * the handler accepts only the PC/vector pair belonging to FNOP below. */
	.globl m68k6888x_probe_raw
	.type m68k6888x_probe_raw,@function
m68k6888x_probe_raw:
	movem.l %d2-%d3/%a2,-(%sp)
	move.w %sr,%d3
	ori.w #0x0700,%sr
	movec %vbr,%d2
	move.l %d2,%a2
	move.l 44(%a2),%d2
	move.l #.Lprobe_fault,44(%a2)
	moveq #0,%d0
.Lprobe_instruction:
	fnop
	moveq #1,%d0
.Lprobe_resume:
	move.l %d2,44(%a2)
	move.w %d3,%sr
	movem.l (%sp)+,%d2-%d3/%a2
	rts

.Lprobe_fault:
	move.w 6(%sp),%d1
	andi.w #0x0fff,%d1
	cmpi.w #44,%d1
	bne.s .Lprobe_unexpected
	cmpi.l #.Lprobe_instruction,2(%sp)
	beq.s .Lprobe_recover
	cmpi.l #.Lprobe_resume,2(%sp)
	bne.s .Lprobe_unexpected
.Lprobe_recover:
	move.l #.Lprobe_resume,2(%sp)
	moveq #0,%d0
	rte
.Lprobe_unexpected:
	stop #0x2700
	bra.s .Lprobe_unexpected
	.size m68k6888x_probe_raw,.-m68k6888x_probe_raw
