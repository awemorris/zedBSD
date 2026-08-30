/*
 * zedBSD FAT16 partition boot record.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * PC-98 FAT16 uses a 1024-byte logical reserved sector.  Firmware need only
 * load its first physical 512 bytes, so that half explicitly reads the second
 * half before parsing FAT.  BOOTZBSD.EXE is an ordinary contiguous FAT root file;
 * INST.EXE and the image builder enforce the historical SYS-file constraint.
 */

	.code16
	.section .text
	.globl _start

#include "bootstrap.inc"
#include "bootloader/include/stage2-header.inc"

	.set LOAD_SEG,          0x0ffc   /* MZ header ends at physical 0x10000. */
	.set TABLE_SEG,         0x0a00
	.set STACK_SEG,         0x9000

_start:
	jmp	pbr_start
	nop
	.ascii	"NEC  5.0"
	.fill	0x003e - (. - _start), 1, 0

pbr_start:
	cli
	cld
	movw	%cs, %ax
	movw	%ax, %ds
	movw	%si, saved_si
	movw	%di, saved_di
	movw	$STACK_SEG, %ax
	movw	%ax, %ss
	movw	$0xfffe, %sp
	sti
	movl	0x001c, %eax
	movl	%eax, partition_lba

	pushw	%ds
	xorw	%dx, %dx
	movw	%dx, %ds
	movb	0x0584, %al
	popw	%ds
	andb	$0x7f, %al
	orb	$0x80, %al
	movb	%al, boot_drive
	call	bios_sense
	jc	boot_failure
	/* The continuation occupies three physical sectors immediately after
	 * this first sector.  Validate the BPB before reading them so a corrupt
	 * reserved count cannot make FAT or file data execute as PBR code.  The
	 * production PC-98 layout is FAT16; its 1024-byte logical sectors require
	 * two reserved sectors, while a 512-byte layout requires four. */
	cmpw	$0, 0x0016
	je	boot_failure
	cmpw	$512, 0x000b
	je	pbr_bps_512
	cmpw	$1024, 0x000b
	jne	boot_failure
	cmpw	$2, 0x000e
	jb	boot_failure
	movl	$2, %ecx
	jmp	pbr_check_total
pbr_bps_512:
	cmpw	$4, 0x000e
	jb	boot_failure
	movl	$1, %ecx
pbr_check_total:
	/* The BPB volume itself must contain this sector and all three physical
	 * continuation sectors.  Convert its logical-sector total before the
	 * first read, rejecting zero and multiplication overflow. */
	movzwl	0x0013, %eax
	testl	%eax, %eax
	jnz	pbr_total_ready
	movl	0x0020, %eax
pbr_total_ready:
	testl	%eax, %eax
	jz	boot_failure
	mull	%ecx
	testl	%edx, %edx
	jnz	boot_failure
	cmpl	$4, %eax
	jb	boot_failure
	movl	partition_lba, %edx
	addl	$3, %edx
	jc	boot_failure
	/* Firmware may load only 512 bytes; fetch the remaining reserved Stage 3
	 * sectors.  zedBSD formats at least four physical reserved sectors. */
	movl	partition_lba, %eax
	incl	%eax
	movw	%cs, %dx
	movw	%dx, %es
	movw	$0x0200, %bp
	movw	$3, %cx
3:
	call	bios_read
	jc	boot_failure
	incl	%eax
	addw	$512, %bp
	loop	3b
	pushw	%cs
	pushw	$pbr_continue
	lret

bios_sense:
	pushw	%ds
	xorw	%dx, %dx
	movw	%dx, %ds
	movb	%cs:boot_drive, %al
	movb	$0x84, %ah
	movw	$0x0100, %bx
	int	$0x1b
	popw	%ds
	jc	1f
	cmpb	$0x20, %ah
	jae	1f
	cmpw	$512, %bx
	jne	1f
	testb	%dh, %dh
	jz	1f
	testb	%dl, %dl
	jz	1f
	movb	%dh, disk_heads
	movb	%dl, disk_sectors
	movb	$0, disk_flags
	clc
	ret
1:
	stc
	ret

/* Read physical LBA EAX to ES:BP through PC-98 INT 1Bh. */
bios_read:
	pushl	%eax
	pushw	%bx
	pushw	%cx
	pushw	%dx
	pushw	%si
	pushw	%di
	pushw	%ds
	xorl	%edx, %edx
	movzbl	%cs:disk_heads, %edi
	movzbl	%cs:disk_sectors, %ecx
	imull	%ecx, %edi
	divl	%edi
	movw	%ax, %cx
	movl	%edx, %eax
	xorl	%edx, %edx
	movzbl	%cs:disk_sectors, %edi
	divl	%edi
	movb	%al, %dh
	xorw	%ax, %ax
	movw	%ax, %ds
	movb	%cs:boot_drive, %al
	movb	$0x06, %ah
	movw	$512, %bx
	movw	%cs:saved_si, %si
	movw	%cs:saved_di, %di
	int	$0x1b
	pushfw
	popw	%ax
	movw	%ax, %cs:read_flags
	popw	%ds
	popw	%di
	popw	%si
	popw	%dx
	popw	%cx
	popw	%bx
	popl	%eax
	pushw	%cs:read_flags
	popfw
	ret

bios_read_volume:
	cmpl	partition_lba, %eax
	jb	boot_failure
	cmpl	volume_end_lba, %eax
	jae	boot_failure
	call	bios_read
	ret

boot_failure:
	movb	$'P', %al
	outb	%al, $0xe9
	movb	$0x06, %al
	outb	%al, $0x37
1:
	hlt
	jmp	1b

partition_lba:   .long 0
saved_si:        .word 0
saved_di:        .word 0
read_flags:      .word 0
boot_drive:      .byte 0
disk_heads:      .byte 0
disk_sectors:    .byte 0
disk_flags:      .byte 0

	.org	0x01fe
	.word	0xaa55

	/* Reserved-sector continuation of Stage 3. */
	.org	0x0200
pbr_continue:
	call	write_handoff
	cmpw	$1024, 0x000b
	je	1f
	cmpw	$512, 0x000b
	jne	boot_failure
	movb	$1, sector_scale
	cmpw	$2, 0x000e
	jb	boot_failure
	jmp	2f
1:	movb	$2, sector_scale
	cmpw	$1, 0x000e
	jb	boot_failure
2:
	/* Bound every filesystem read to the BPB-declared physical volume. */
	movzwl	0x0013, %eax
	testl	%eax, %eax
	jnz	3f
	movl	0x0020, %eax
3:	testl	%eax, %eax
	jz	boot_failure
	movzbl	sector_scale, %ecx
	mull	%ecx
	testl	%edx, %edx
	jnz	boot_failure
	addl	partition_lba, %eax
	jc	boot_failure
	cmpl	partition_lba, %eax
	jbe	boot_failure
	movl	%eax, volume_end_lba
	movzbl	0x000d, %eax
	testl	%eax, %eax
	jz	boot_failure
	movl	%eax, %edx
	decl	%edx
	testl	%edx, %eax
	jnz	boot_failure

	/* Convert the BPB layout to physical 512-byte LBAs. */
	movl	partition_lba, %eax
	movzwl	0x000e, %ecx
	movzbl	sector_scale, %edx
	imull	%edx, %ecx
	jo	boot_failure
	addl	%ecx, %eax
	jc	boot_failure
	movl	%eax, fat_lba
	movzbl	0x0010, %ecx
	testl	%ecx, %ecx
	jz	boot_failure
	movzwl	0x0016, %edx
	testl	%edx, %edx
	jz	fat32_layout
	movzbl	sector_scale, %ebx
	imull	%ebx, %edx
	jo	boot_failure
	movl	%edx, fat_sectors
	movl	fat_lba, %ebx
	addl	%edx, %ebx
	jc	boot_failure
	movl	%ebx, fat_end_lba
	imull	%edx, %ecx
	jo	boot_failure
	addl	%ecx, %eax
	jc	boot_failure
	movl	%eax, root_lba
	movzwl	0x0011, %ecx
	movw	%cx, root_entries
	shll	$5, %ecx
	addl	$511, %ecx
	shrl	$9, %ecx
	movw	%cx, root_sectors
	addl	%ecx, %eax
	jc	boot_failure
	cmpl	volume_end_lba, %eax
	ja	boot_failure
	movl	%eax, data_lba
	cmpl	volume_end_lba, %eax
	jae	boot_failure
	jmp	root_layout_ready

fat32_layout:
	movl	0x0024, %edx
	testl	%edx, %edx
	jz	boot_failure
	movzbl	sector_scale, %ebx
	imull	%ebx, %edx
	jo	boot_failure
	movl	%edx, fat_sectors
	movl	fat_lba, %ebx
	addl	%edx, %ebx
	jc	boot_failure
	movl	%ebx, fat_end_lba
	imull	%edx, %ecx
	jo	boot_failure
	addl	%ecx, %eax
	jc	boot_failure
	movl	%eax, data_lba
	cmpl	volume_end_lba, %eax
	jae	boot_failure
	movl	volume_end_lba, %edx
	subl	%eax, %edx
	movzbl	0x000d, %ecx
	movzbl	sector_scale, %ebx
	imull	%ebx, %ecx
	jo	boot_failure
	movl	%edx, %eax
	xorl	%edx, %edx
	divl	%ecx
	testl	%eax, %eax
	jz	boot_failure
	movl	%eax, cluster_count
	addl	$2, %eax
	jc	boot_failure
	movl	%eax, cluster_limit
	movl	0x002c, %edx
	andl	$0x0fffffff, %edx
	cmpl	$2, %edx
	jb	boot_failure
	cmpl	cluster_limit, %edx
	jae	boot_failure
	movl	%edx, root_cluster
	movl	cluster_count, %eax
	movl	%eax, root_cluster_budget
	subl	$2, %edx
	movzbl	0x000d, %ecx
	movzbl	sector_scale, %ebx
	imull	%ebx, %ecx
	jo	boot_failure
	imull	%ecx, %edx
	jo	boot_failure
	movl	data_lba, %eax
	addl	%edx, %eax
	jc	boot_failure
	movl	%eax, root_lba
	movl	%eax, %edx
	addl	%ecx, %edx
	jc	boot_failure
	cmpl	volume_end_lba, %edx
	ja	boot_failure
	movw	%cx, root_sectors
	movb	$1, fat32_volume
root_layout_ready:

	/* Locate BOOTZBSD.EXE in the FAT16 root directory. */
	movl	root_lba, %eax
	movw	root_sectors, %dx
root_sector_loop:
	testw	%dx, %dx
	jz	root_cluster_next
	movw	$TABLE_SEG, %cx
	movw	%cx, %es
	xorw	%bp, %bp
	call	bios_read_volume
	jc	boot_failure
	xorw	%di, %di
	movw	$16, %cx
root_entry_loop:
	cmpb	$0, %es:(%di)
	je	boot_failure
	cmpb	$0xe5, %es:(%di)
	je	next_root_entry
	cmpb	$0x0f, %es:0x0b(%di)
	je	next_root_entry
	pushw	%cx
	pushw	%di
	movw	$loader_name, %si
	movw	$11, %cx
	repe cmpsb
	popw	%di
	popw	%cx
	je	io_found
next_root_entry:
	addw	$32, %di
	loop	root_entry_loop
	incl	%eax
	decw	%dx
	jmp	root_sector_loop

root_cluster_next:
	cmpb	$0, fat32_volume
	je	boot_failure
	cmpl	$0, root_cluster_budget
	je	boot_failure
	decl	root_cluster_budget
	movl	root_cluster, %eax
	call	pbr_fat32_next
	cmpl	$0x0ffffff8, %eax
	jae	boot_failure
	cmpl	$2, %eax
	jb	boot_failure
	cmpl	cluster_limit, %eax
	jae	boot_failure
	movl	%eax, root_cluster
	subl	$2, %eax
	movzbl	0x000d, %ecx
	movzbl	sector_scale, %edx
	imull	%edx, %ecx
	jo	boot_failure
	imull	%ecx, %eax
	jo	boot_failure
	addl	data_lba, %eax
	jc	boot_failure
	movl	%eax, %edx
	addl	%ecx, %edx
	jc	boot_failure
	cmpl	volume_end_lba, %edx
	ja	boot_failure
	movw	%cx, %dx
	jmp	root_sector_loop

pbr_fat32_next:
	pushl	%ebx
	pushl	%ecx
	pushl	%edx
	cmpl	$2, %eax
	jb	boot_failure
	cmpl	cluster_limit, %eax
	jae	boot_failure
	cmpl	$0x3fffffff, %eax
	ja	boot_failure
	shll	$2, %eax
	movl	%eax, %ebx
	andl	$511, %ebx
	movw	%bx, fat_entry_offset
	shrl	$9, %eax
	addl	fat_lba, %eax
	jc	boot_failure
	cmpl	fat_end_lba, %eax
	jae	boot_failure
	movw	$TABLE_SEG, %cx
	movw	%cx, %es
	xorw	%bp, %bp
	call	bios_read_volume
	jc	boot_failure
	movzwl	fat_entry_offset, %ebx
	movl	%es:(%bx), %eax
	andl	$0x0fffffff, %eax
	popl	%edx
	popl	%ecx
	popl	%ebx
	ret

io_found:
	movzwl	%es:0x1a(%di), %eax
	cmpb	$0, fat32_volume
	je	1f
	movzwl	%es:0x14(%di), %edx
	shll	$16, %edx
	orl	%edx, %eax
	andl	$0x0fffffff, %eax
1:
	cmpl	$2, %eax
	jb	boot_failure
	cmpb	$0, fat32_volume
	je	2f
	cmpl	cluster_limit, %eax
	jae	boot_failure
2:
	subl	$2, %eax
	movzbl	0x000d, %ecx
	movzbl	sector_scale, %edx
	imull	%edx, %ecx
	jo	boot_failure
	imull	%ecx, %eax
	jo	boot_failure
	addl	data_lba, %eax
	jc	boot_failure
	movl	%es:0x1c(%di), %ecx
	movl	%ecx, loader_file_size
	addl	$511, %ecx
	jc	boot_failure
	shrl	$9, %ecx
	jz	boot_failure
	cmpl	$127, %ecx
	ja	boot_failure
	movl	%eax, %edx
	addl	%ecx, %edx
	jc	boot_failure
	cmpl	volume_end_lba, %edx
	ja	boot_failure

	/* The system loader is contiguous.  Load and enter its DOS MZ image. */
	movw	$LOAD_SEG, %dx
	movw	%dx, %es
	xorw	%bp, %bp
load_io:
	call	bios_read_volume
	jc	boot_failure
	incl	%eax
	addw	$512, %bp
	loop	load_io

	/* The image builder's contiguous SYS-file contract determines how the
	 * bytes are read.  Authenticate the fixed MZ envelope and the complete
	 * inner ZBL2 image before trusting either header as executable control
	 * flow.  Stage 2 verifies the checksum again after entry. */
	cmpw	$0x5a4d, %es:0
	jne	boot_failure
	cmpw	$0, %es:0x06
	jne	boot_failure
	cmpw	$4, %es:0x08
	jne	boot_failure
	cmpw	$ZBL2_HEADER_SIZE, %es:0x14
	jne	boot_failure
	cmpw	$0, %es:0x16
	jne	boot_failure
	cmpw	$0x40, %es:0x18
	jne	boot_failure
	cmpl	$ZBL2_MAGIC, %es:0x40
	jne	boot_failure
	cmpw	$ZBL2_VERSION, %es:0x44
	jne	boot_failure
	cmpw	$ZBL2_HEADER_SIZE, %es:0x46
	jne	boot_failure
	cmpw	$ZBL2_MACHINE_PC98, %es:0x4e
	jne	boot_failure
	cmpl	$ZBL2_HEADER_SIZE, %es:0x50
	jne	boot_failure
	movzwl	%es:0x4c, %ecx
	testl	%ecx, %ecx
	jz	boot_failure
	cmpl	$127, %ecx
	ja	boot_failure
	movl	%ecx, %edx
	shll	$9, %edx
	movl	%edx, %eax
	addl	$64, %eax
	jc	boot_failure
	cmpl	loader_file_size, %eax
	jne	boot_failure
	movl	%es:0x48, %eax
	cmpl	$ZBL2_HEADER_SIZE, %eax
	jb	boot_failure
	cmpl	%edx, %eax
	ja	boot_failure
	shll	$7, %ecx
	movw	$0x40, %di
	xorl	%eax, %eax
zbl2_checksum_loop:
	addl	%es:(%di), %eax
	addw	$4, %di
	decl	%ecx
	jnz	zbl2_checksum_loop
	testl	%eax, %eax
	jnz	boot_failure
	pushw	$(LOAD_SEG + 4)
	pushw	$ZBL2_HEADER_SIZE
	lret

write_handoff:
	pushw	%es
	xorw	%ax, %ax
	movw	%ax, %es
	movl	$HANDOFF_MAGIC, %es:HANDOFF
	movb	boot_drive, %al
	movb	%al, %es:HANDOFF+4
	movb	disk_heads, %al
	movb	%al, %es:HANDOFF+5
	movb	disk_sectors, %al
	movb	%al, %es:HANDOFF+6
	movw	$0, %es:HANDOFF+8
	movw	saved_si, %ax
	movw	%ax, %es:HANDOFF+10
	movw	saved_di, %ax
	movw	%ax, %es:HANDOFF+12
	movl	partition_lba, %eax
	movl	%eax, %es:HANDOFF+16
	movl	$0x31545250, %es:HANDOFF+20
	popw	%es
	ret

loader_name:     .ascii "BOOTZBSDEXE"
loader_file_size: .long 0
root_lba:        .long 0
data_lba:        .long 0
fat_lba:         .long 0
fat_end_lba:     .long 0
fat_sectors:     .long 0
volume_end_lba:  .long 0
root_cluster:    .long 0
root_cluster_budget: .long 0
cluster_count:   .long 0
cluster_limit:   .long 0
fat_entry_offset: .word 0
root_entries:    .word 0
root_sectors:    .word 0
sector_scale:    .byte 2
fat32_volume:    .byte 0

	.org	0x07fe
	.word	0xaa55
