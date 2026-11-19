/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 68040 Macintosh boot block.
 *
 * The ROM reads these 1024 bytes from the start of the HFS boot partition
 * through the disk's own driver and enters them in supervisor mode, with
 * the ROM's memory, file and device services still running.  The code
 * finds the drive it came from, reads the second stage from the same
 * partition through the same driver, checks it and enters it.  It is
 * position independent: the ROM chooses where the block is in memory.
 */

#define _ASM_SRC_ 1
#include "bootloader/mac68k/boot-layout.h"

/*
 * Low-memory globals and trap words.
 *
 * Values referenced: Apple, Inside Macintosh: Operating System Utilities
 * (1994) and Inside Macintosh: Files (1992) (the low-memory globals
 * CPUFlag, MMU32Bit, BootDrive and DrvQHdr; the drive queue element's
 * dQDrive and dQRefNum; the I/O parameter block's ioVRefNum, ioRefNum,
 * ioBuffer, ioReqCount, ioActCount, ioPosMode and ioPosOffset; the trap
 * words of _Read, _NewPtr with its CLEAR and SYS bits, _SwapMMUMode and
 * _SysError).
 */
#define LOW_CPU_FLAG		0x012f
#define LOW_MMU32BIT		0x0cb2
#define LOW_BOOT_DRIVE		0x0210
#define LOW_DRIVE_QUEUE_HEAD	0x030a

#define DRIVE_NUMBER		6
#define DRIVE_DRIVER		8

#define PB_SIZE			64
#define PB_VREFNUM		22
#define PB_REFNUM		24
#define PB_BUFFER		32
#define PB_REQUEST		36
#define PB_ACTUAL		40
#define PB_POSITION_MODE	44
#define PB_POSITION		46
#define FS_FROM_START		1

#define TRAP_READ		0xa002
#define TRAP_NEW_PTR_CLEAR	0xa31e
#define TRAP_NEW_PTR_SYS_CLEAR	0xa71e
#define TRAP_SWAP_MMU_MODE	0xa05d
#define TRAP_SYS_ERROR		0xa9c9

/* The processor flag of the 68040, and the 32-bit addressing mode. */
#define CPU_FLAG_68040		4
#define MMU_MODE_32BIT		1

/*
 * _start()
 *
 * The header the ROM reads: the signature, the branch to the code, the
 * version, the names and the heap sizes.
 *
 * Values referenced: EMILE (github.com/vivier/EMILE, master)
 * first/first.S (the file names and the heap sizes of the boot block
 * header, which are Apple's own boot block values).
 */
	.text
	.globl _start
	.type _start,@function
_start:
	.short MAC68K_BOOT_SIGNATURE
	bra.w stage1_entry
	.org MAC68K_BOOT_VERSION_OFFSET
	.short MAC68K_BOOT_VERSION
	.short 0
	.byte 6
	.ascii "System"
	.org 26
	.byte 6
	.ascii "Finder"
	.org 42
	.byte 7
	.ascii "MacsBug"
	.org 58
	.byte 12
	.ascii "Disassembler"
	.org 74
	.byte 13
	.ascii "StartUpScreen"
	.org 90
	.byte 6
	.ascii "Finder"
	.org 106
	.byte 14
	.ascii "Clipboard File"
	.org 122
	.short 10
	.short 20
	.long 0x00004300
	.long 0x00008000
	.long 0x00020000
	.short 0
	.long 0
	.long 0

	/* The zedBSD fields: the image's tool fills all but the identity. */
	.org MAC68K_BOOT_INFO_OFFSET
	.long MAC68K_BOOT_INFO_MAGIC
	.short MAC68K_BOOT_INFO_VERSION
	.short 0
	.org MAC68K_BOOT_INFO_OFFSET + MAC68K_BOOT_INFO_SIZE, 0

	/*
	 * Procedure referenced: EMILE (github.com/vivier/EMILE, master)
	 * first/first.S (32-bit addressing through _SwapMMUMode, memory
	 * for the second stage from _NewPtr, the second stage read with
	 * _Read and entered).
	 */
	.org MAC68K_BOOT_CODE_OFFSET
stage1_entry:
	lea _start(%pc),%a6

	/* The second stage and the kernel are for the 68040 alone. */
	cmpi.b #CPU_FLAG_68040,LOW_CPU_FLAG.w
	bcs stage1_processor_error

	/* The second stage addresses memory with 32 bits. */
	tst.b LOW_MMU32BIT.w
	bne.s 1f
	moveq #MMU_MODE_32BIT,%d0
	.short TRAP_SWAP_MMU_MODE
1:
	/*
	 * Takes cleared memory for the second stage, from the current heap
	 * or else from the system heap, rounded up to whole sectors.
	 */
	move.l MAC68K_BOOT_STAGE2_MEMORY_OFFSET(%a6),%d7
	cmpi.l #MAC68K_STAGE2_LIMIT,%d7
	bhi stage1_stage2_error
	add.l #MAC68K_SECTOR_SIZE - 1,%d7
	and.l #-MAC68K_SECTOR_SIZE,%d7
	move.l %d7,%d0
	.short TRAP_NEW_PTR_CLEAR
	tst.w %d0
	beq.s 2f
	move.l %d7,%d0
	.short TRAP_NEW_PTR_SYS_CLEAR
	tst.w %d0
	bne stage1_memory_error
2:	movea.l %a0,%a4

	/* The parameter block of the reads, on the stack. */
	lea -PB_SIZE(%sp),%sp
	movea.l %sp,%a2

	/* Tries the drive the ROM says it started from. */
	movea.l LOW_DRIVE_QUEUE_HEAD.w,%a3
3:	move.l %a3,%d0
	beq.s 5f
	move.w DRIVE_NUMBER(%a3),%d0
	cmp.w LOW_BOOT_DRIVE.w,%d0
	bne.s 4f
	bsr try_drive
	beq.s stage1_found
4:	movea.l (%a3),%a3
	bra.s 3b

	/* Tries every drive in the queue. */
5:	movea.l LOW_DRIVE_QUEUE_HEAD.w,%a3
6:	move.l %a3,%d0
	beq stage1_drive_error
	bsr try_drive
	beq.s stage1_found
	movea.l (%a3),%a3
	bra.s 6b

	/*
	 * Reads the second stage from the same partition, whole sectors into
	 * the memory taken for it.
	 */
stage1_found:
	move.l MAC68K_BOOT_STAGE2_BYTES_OFFSET(%a6),%d1
	beq stage1_stage2_error
	add.l #MAC68K_SECTOR_SIZE - 1,%d1
	and.l #-MAC68K_SECTOR_SIZE,%d1
	cmp.l %d7,%d1
	bhi stage1_stage2_error
	move.l MAC68K_BOOT_STAGE2_OFFSET_OFFSET(%a6),%d0
	add.l %d5,%d0
	movea.l %a4,%a1
	bsr read_bytes
	bne stage1_read_error

	/* Checks the second stage against the sum and the identity the image's tool wrote. */
	movea.l %a4,%a0
	move.l MAC68K_BOOT_STAGE2_BYTES_OFFSET(%a6),%d1
	bsr crc32
	cmp.l MAC68K_BOOT_STAGE2_CRC_OFFSET(%a6),%d0
	bne stage1_stage2_error
	cmpi.l #MAC68K_STAGE2_MAGIC,MAC68K_STAGE2_MAGIC_OFFSET(%a4)
	bne stage1_stage2_error

	/* The code just read sits in the data cache: pushes it and refills the instruction cache. */
	cpusha %bc

	/*
	 * Enters the second stage with the drive number, the driver's
	 * reference number, the partition's offset on the drive and the boot
	 * block's address.
	 */
	moveq #0,%d0
	move.w %d3,%d0
	move.w %d4,%d1
	ext.l %d1
	move.l %d5,%d2
	movea.l %a6,%a0
	jmp (%a4)

/*
 * Tries one drive of the queue (a3): reports Z set, with the drive (d3),
 * its driver (d4) and the partition's offset on it (d5), when the drive
 * holds this boot block, at its start or at the partition's place.
 */
try_drive:
	move.w DRIVE_NUMBER(%a3),%d3
	move.w DRIVE_DRIVER(%a3),%d4
	moveq #0,%d5
	bsr read_and_compare
	beq.s 1f
	move.l MAC68K_BOOT_PARTITION_BLOCK_OFFSET(%a6),%d5
	beq.s 2f
	moveq #9,%d0
	lsl.l %d0,%d5
	bsr read_and_compare
1:	rts
2:	moveq #1,%d0
	rts

/* Reads the sector at d5 of the drive and reports Z set when it is this boot block's first. */
read_and_compare:
	move.l %d5,%d0
	move.l #MAC68K_SECTOR_SIZE,%d1
	movea.l %a4,%a1
	bsr read_bytes
	bne.s 2f
	movea.l %a4,%a0
	movea.l %a6,%a1
	move.w #(MAC68K_SECTOR_SIZE / 4) - 1,%d2
1:	cmpm.l (%a0)+,(%a1)+
	dbne %d2,1b
2:	rts

/*
 * Reads d1 bytes at byte d0 of the drive (d3, driver d4) into a1 through
 * the driver; reports Z set when the whole request was read.
 */
read_bytes:
	movea.l %a2,%a0
	moveq #(PB_SIZE / 2) - 1,%d2
1:	clr.w (%a0)+
	dbra %d2,1b
	move.w %d3,PB_VREFNUM(%a2)
	move.w %d4,PB_REFNUM(%a2)
	move.l %a1,PB_BUFFER(%a2)
	move.l %d1,PB_REQUEST(%a2)
	move.w #FS_FROM_START,PB_POSITION_MODE(%a2)
	move.l %d0,PB_POSITION(%a2)
	movea.l %a2,%a0
	.short TRAP_READ
	tst.w %d0
	bne.s 2f
	move.l PB_ACTUAL(%a2),%d0
	cmp.l PB_REQUEST(%a2),%d0
2:	rts

/*
 * Sums d1 bytes at a0 into d0 with the CRC-32 of zlib (reflected,
 * 0xedb88320); keeps the drive's registers d3 to d5.
 */
crc32:
	moveq #-1,%d0
1:	tst.l %d1
	beq.s 4f
	moveq #0,%d2
	move.b (%a0)+,%d2
	eor.b %d2,%d0
	moveq #7,%d6
2:	lsr.l #1,%d0
	bcc.s 3f
	eori.l #0xedb88320,%d0
3:	dbra %d6,2b
	subq.l #1,%d1
	bra.s 1b
4:	not.l %d0
	rts

stage1_processor_error:
	move.w #MAC68K_BOOT_ERROR_PROCESSOR,%d0
	bra.s stage1_fatal

stage1_memory_error:
	move.w #MAC68K_BOOT_ERROR_MEMORY,%d0
	bra.s stage1_fatal

stage1_drive_error:
	move.w #MAC68K_BOOT_ERROR_DRIVE,%d0
	bra.s stage1_fatal

stage1_read_error:
	move.w #MAC68K_BOOT_ERROR_READ,%d0
	bra.s stage1_fatal

stage1_stage2_error:
	move.w #MAC68K_BOOT_ERROR_STAGE2,%d0

/* Stops in the ROM's system error alert with the code in d0. */
stage1_fatal:
	.short TRAP_SYS_ERROR
1:	bra.s 1b
	.size _start,.-_start
