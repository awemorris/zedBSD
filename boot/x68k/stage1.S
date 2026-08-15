/* X68000 1 KiB SCSI primary boot. */
/* IOCS register contracts follow NetBSD/x68k stand/libiocs. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#define _ASM_SRC_ 1
#include "boot/x68k/boot-layout.h"

	.text
	.globl _start
	.type _start,@function
_start:
	bra.s stage1_start
	.ascii "zedBSD x68k"
	.byte 0
	.even

stage1_start:
	lea _start,%sp
	move.l %d4,%d7

	/* IOCS S_READEXT(manifest LBA, 1, target, 512-byte blocks, buffer). */
	moveq #-11,%d0
	moveq #38,%d1
	moveq #X68K_MANIFEST_LBA,%d2
	moveq #1,%d3
	move.l %d7,%d4
	moveq #1,%d5
	movea.l #X68K_MANIFEST_ADDRESS,%a1
	trap #15
	tst.l %d0
	bmi stage1_read_error

	movea.l #X68K_MANIFEST_ADDRESS,%a1
	cmpi.l #X68K_MANIFEST_MAGIC,X68K_MANIFEST_MAGIC_OFFSET(%a1)
	bne stage1_manifest_error
	cmpi.w #X68K_MANIFEST_VERSION,X68K_MANIFEST_VERSION_OFFSET(%a1)
	bne stage1_manifest_error
	cmpi.w #X68K_MANIFEST_SIZE,X68K_MANIFEST_HEADER_SIZE_OFFSET(%a1)
	bne stage1_manifest_error
	move.l X68K_MANIFEST_STAGE2_BYTES_OFFSET(%a1),%d3
	beq stage1_manifest_error
	cmpi.l #(X68K_STAGE2_LIMIT-X68K_STAGE2_DEFAULT_ADDRESS),%d3
	bhi stage1_manifest_error
	add.l #511,%d3
	lsr.l #8,%d3
	lsr.l #1,%d3

	moveq #-11,%d0
	moveq #38,%d1
	move.l X68K_MANIFEST_STAGE2_LBA_OFFSET(%a1),%d2
	move.l %d7,%d4
	moveq #1,%d5
	movea.l X68K_MANIFEST_STAGE2_LOAD_OFFSET(%a1),%a1
	trap #15
	tst.l %d0
	bmi stage1_read_error

	movea.l #X68K_MANIFEST_ADDRESS,%a1
	movea.l X68K_MANIFEST_STAGE2_LOAD_OFFSET(%a1),%a0
	move.l X68K_MANIFEST_STAGE2_BYTES_OFFSET(%a1),%d1
	bsr crc32
	movea.l #X68K_MANIFEST_ADDRESS,%a1
	cmp.l X68K_MANIFEST_STAGE2_CRC_OFFSET(%a1),%d0
	bne stage1_crc_error

	movea.l X68K_MANIFEST_STAGE2_ENTRY_OFFSET(%a1),%a2
	movea.l #X68K_MANIFEST_ADDRESS,%a0
	move.l %d7,%d4
	jmp (%a2)

/* a0=input, d1=length, d0=CRC32. */
crc32:
	moveq #-1,%d0
1:	tst.l %d1
	beq 4f
	moveq #0,%d2
	move.b (%a0)+,%d2
	eor.b %d2,%d0
	moveq #7,%d3
2:	lsr.l #1,%d0
	bcc 3f
	eori.l #0xedb88320,%d0
3:	dbra %d3,2b
	subq.l #1,%d1
	bra 1b
4:	not.l %d0
	rts

stage1_read_error:
	lea read_error(%pc),%a1
	bra stage1_fatal
stage1_manifest_error:
	lea manifest_error(%pc),%a1
	bra stage1_fatal
stage1_crc_error:
	lea crc_error(%pc),%a1
stage1_fatal:
	moveq #0x21,%d0
	trap #15
5:	stop #0x2700
	bra 5b

read_error:	.asciz "zedBSD E1 READ\r\n"
manifest_error:	.asciz "zedBSD E2 MANIFEST\r\n"
crc_error:	.asciz "zedBSD E3 CRC\r\n"
	.even
	.size _start,.-_start
