/* MC68030 supervisor interrupt-mask primitives. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

	.text
	.even
	.globl hal_irq_disable
	.type hal_irq_disable,@function
hal_irq_disable:
	moveq #0,%d0
	move.w %sr,%d1
	andi.w #0x0700,%d1
	seq %d0                         /* true only when IPL was zero */
	andi.l #1,%d0
	ori.w #0x0700,%sr
	rts
	.size hal_irq_disable,.-hal_irq_disable

	.globl hal_irq_enable
	.type hal_irq_enable,@function
hal_irq_enable:
	andi.w #0xf8ff,%sr
	rts
	.size hal_irq_enable,.-hal_irq_enable
