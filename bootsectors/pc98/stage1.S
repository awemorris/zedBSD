/*
 * zedBSD minimal real-mode system loader
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The FAT16 PBR (fixed disk) or the FDD IPL (FAT12 floppy) loads this file
 * at 1000:0000 and supplies a versioned bootstrap handoff at 0000:0700.
 * IO.SYS validates that handoff, loads vmunix from the already selected
 * BOOT volume, installs the BIOS gateway, and enters 32-bit protected mode.
 * Device discovery and all normal UI are intentionally owned by vmunix.
 */

	.code16
	.section .text
	.globl _start

	.set HANDOFF,             0x0700
	.set HANDOFF_MAGIC,       0x4839384c /* "L98H" */
	.set HANDOFF_VERSION,     1
	.set HANDOFF_SIZE,        24
	.set HO_MAGIC,            0
	.set HO_VERSION,          4
	.set HO_SIZE,             6
	.set HO_PARTITION_LBA,    8
	.set HO_BOOT_BIOS_ID,    12
	.set HO_HEADS,           13
	.set HO_SECTORS,         14
	.set HO_FILESYSTEM_HINT, 15
	.set HO_SECTOR_SIZE,     16
	.set HO_FLAGS,           18
	.set HO_SAVED_SI,        20
	.set HO_SAVED_DI,        22

	.set HO_FS_FAT16,         1
	.set HO_FS_FAT12,         2
	.set HO_FLAG_GEOMETRY,    1

	.set TABLE_SEG,           0x0800
	.set TEXT_SEG,            0xa000
	.set ATTR_SEG,            0xa200
	.set STACK_SEG,           0x9000
	/*
	 * vmunix is a two-segment ELF (subset enforced by
	 * scripts/patch-stage2.py): the low segment loads at 0x20000 and
	 * the high segment at 0x100000, so the size bound is a file-size
	 * sanity cap, not a memory window.
	 */
	.set STAGE2_MAX_SIZE,     0x400000

	.set DEV_CLASS_FDD,       1
	.set DEV_CLASS_IDE,       2
	.set DEV_CLASS_SCSI,      3
	.set DEV_FLAG_PRESENT,    1
	.set DEV_FLAG_GEOMETRY,   2
	.set DEV_FLAG_BOOT,       4
	.set DEV_DESC_SIZE,      16
	.set MAX_IDE_DEVICES,     4
	.set MAX_FIXED_DEVICES,  11
	.set PC98_WA_IDE_DRIVES, 0x055d

	.set DEV_CLASS,           0
	.set DEV_INDEX,           1
	.set DEV_BIOS_ID,         2
	.set DEV_FLAGS,           3
	.set DEV_SECTOR_SIZE,     4
	.set DEV_CYLINDERS,       6
	.set DEV_HEADS,           8
	.set DEV_SECTORS,         9

	/* Private descriptor used by the common BIOS read/chain helpers. */
	.set MENU_SIZE,          16
	.set MENU_TYPE,           0
	.set MENU_BIOS_ID,        1
	.set MENU_CLASS,          2
	.set MENU_PART_INDEX,     4
	.set MENU_HEADS,          5
	.set MENU_SECTORS,        6
	.set MENU_LBA,            8

_start:
	jmp	stage1_main

stage1_main:
	cli
	cld
	movw	$STACK_SEG, %ax
	movw	%ax, %ss
	movw	$0xfffe, %sp
	pushw	%cs
	popw	%ds
	sti

	call	validate_bootstrap_handoff
	jc	fatal_handoff
	call	initialize_boot_device
	call	enumerate_ide_devices
	call	load_stage2
	jc	fatal_stage2
	jmp	enter_stage2

fatal_handoff:
	movw	$fatal_handoff_message, %si
	jmp	fatal
fatal_stage2:
	movw	$fatal_stage2_message, %si
fatal:
	/* Fatal diagnostics are the only display output retained in IO.SYS. */
	movb	$0x42, %ah
	movb	$0xc0, %ch
	int	$0x18
	movb	$0x40, %ah
	int	$0x18
	pushw	%cs
	popw	%ds
	movw	$TEXT_SEG, %ax
	movw	%ax, %es
	movw	$ATTR_SEG, %ax
	movw	%ax, %gs
	xorw	%di, %di
1:
	lodsb
	testb	%al, %al
	jz	fatal_stop
	movb	%al, %es:(%di)
	movb	$0, %es:1(%di)
	movb	$0xe1, %gs:(%di)
	addw	$2, %di
	jmp	1b
fatal_stop:
	cli
1:
	hlt
	jmp	1b

validate_bootstrap_handoff:
	pushw	%ds
	xorw	%ax, %ax
	movw	%ax, %ds
	cmpl	$HANDOFF_MAGIC, HANDOFF+HO_MAGIC
	jne	1f
	cmpw	$HANDOFF_VERSION, HANDOFF+HO_VERSION
	jne	1f
	cmpw	$HANDOFF_SIZE, HANDOFF+HO_SIZE
	jb	1f
	movb	HANDOFF+HO_FILESYSTEM_HINT, %al
	cmpb	$HO_FS_FAT16, %al
	je	2f
	cmpb	$HO_FS_FAT12, %al
	jne	1f
2:
	cmpw	$512, HANDOFF+HO_SECTOR_SIZE
	jne	1f
	testw	$HO_FLAG_GEOMETRY, HANDOFF+HO_FLAGS
	jz	1f
	cmpb	$0, HANDOFF+HO_HEADS
	je	1f
	cmpb	$0, HANDOFF+HO_SECTORS
	je	1f
	popw	%ds
	movb	%al, fat_fs_hint
	clc
	ret
1:
	popw	%ds
	stc
	ret

/* Construct the boot-origin descriptor before enumerating the other disks. */
initialize_boot_device:
	pushw	%es
	pushw	%ds
	popw	%es
	movw	$device_table, %di
	xorw	%ax, %ax
	movw	$(DEV_DESC_SIZE * MAX_FIXED_DEVICES), %cx
	rep stosb
	popw	%es

	pushw	%ds
	xorw	%ax, %ax
	movw	%ax, %ds
	movb	HANDOFF+HO_BOOT_BIOS_ID, %al
	movb	HANDOFF+HO_HEADS, %ch
	movb	HANDOFF+HO_SECTORS, %cl
	movl	HANDOFF+HO_PARTITION_LBA, %edx
	popw	%ds

	movb	%al, boot_bios_id
	movb	%al, device_table+DEV_BIOS_ID
	movb	%al, %ah
	andb	$0x0f, %ah
	movb	%ah, device_table+DEV_INDEX
	movb	%al, gateway_menu+MENU_BIOS_ID
	movb	%ch, device_table+DEV_HEADS
	movb	%ch, gateway_menu+MENU_HEADS
	movb	%cl, device_table+DEV_SECTORS
	movb	%cl, gateway_menu+MENU_SECTORS
	movw	$512, device_table+DEV_SECTOR_SIZE
	movb	$(DEV_FLAG_PRESENT | DEV_FLAG_GEOMETRY | DEV_FLAG_BOOT), device_table+DEV_FLAGS
	movb	$DEV_CLASS_IDE, %bl
	movb	%al, %ah
	andb	$0xf0, %ah
	cmpb	$0xa0, %ah
	jne	1f
	movb	$DEV_CLASS_SCSI, %bl
1:
	cmpb	$0x30, %ah
	jne	2f
	movb	$DEV_CLASS_FDD, %bl
2:
	movb	%bl, device_table+DEV_CLASS
	movb	%bl, gateway_menu+MENU_CLASS
	movl	%edx, fat_partition_lba
	movl	%edx, gateway_menu+MENU_LBA
	movw	$gateway_menu, chain_entry
	movb	$1, device_count
	ret

/*
 * Build descriptors for every IDE unit reported by the BIOS before leaving
 * real mode.  0:055dh is a dense bitmap whose bit N denotes BIOS unit 80h+N.
 * Consulting it first avoids issuing SENSE to absent units on firmware which
 * does not return from such a request.  The boot-origin unit is already in
 * slot zero and is retained even when the firmware omitted its bitmap bit.
 */
enumerate_ide_devices:
	pushw	%ax
	pushw	%bx
	pushw	%cx
	pushw	%dx
	pushw	%di
	pushw	%ds
	xorw	%ax, %ax
	movw	%ax, %ds
	movb	PC98_WA_IDE_DRIVES, %bl
	popw	%ds
	andb	$0x0f, %bl
	movb	$0x80, %al
	movw	$MAX_IDE_DEVICES, %cx
1:
	testb	$1, %bl
	jz	2f
	cmpb	boot_bios_id, %al
	je	2f
	call	bios_sense
	jc	2f
	cmpb	$0x20, sense_status
	jae	2f
	cmpw	$512, sense_bytes
	jne	2f
	cmpb	$0, sense_heads
	je	2f
	cmpb	$0, sense_sectors
	je	2f
	call	bios_probe_read_lba0
	jc	2f
	cmpb	$0x20, probe_read_status
	jae	2f

	pushw	%ax
	movb	device_count, %al
	xorb	%ah, %ah
	shlw	$4, %ax
	movw	%ax, %di
	popw	%ax
	movb	$DEV_CLASS_IDE, device_table+DEV_CLASS(%di)
	movb	%al, %ah
	andb	$0x0f, %ah
	movb	%ah, device_table+DEV_INDEX(%di)
	movb	%al, device_table+DEV_BIOS_ID(%di)
	movb	$(DEV_FLAG_PRESENT | DEV_FLAG_GEOMETRY), device_table+DEV_FLAGS(%di)
	movw	sense_bytes, %dx
	movw	%dx, device_table+DEV_SECTOR_SIZE(%di)
	movw	sense_cylinders, %dx
	movw	%dx, device_table+DEV_CYLINDERS(%di)
	movb	sense_heads, %dl
	movb	%dl, device_table+DEV_HEADS(%di)
	movb	sense_sectors, %dl
	movb	%dl, device_table+DEV_SECTORS(%di)
	movb	sense_status, %dl
	movb	%dl, device_table+10(%di)
	incb	device_count
2:
	shrb	$1, %bl
	incb	%al
	decw	%cx
	jz	3f
	jmp	1b
3:
	popw	%di
	popw	%dx
	popw	%cx
	popw	%bx
	popw	%ax
	ret

/* Load and validate vmunix from the handoff BOOT FAT16 volume. */
load_stage2:
	movw	$0x0800, %ax
	movw	%ax, %es
	xorw	%bp, %bp
	movl	fat_partition_lba, %eax
	call	read_fixed_lba
	jc	load_stage2_error
	pushw	%cs
	popw	%ds
	movw	%es:0x0b, %ax
	cmpw	$512, %ax
	je	1f
	cmpw	$1024, %ax
	jne	load_stage2_error
	movb	$2, fat_sector_scale
	jmp	2f
1:
	movb	$1, fat_sector_scale
2:
	movzbl	%es:0x0d, %eax
	movzbl	fat_sector_scale, %ecx
	imull	%ecx, %eax
	testl	%eax, %eax
	jz	load_stage2_error
	cmpl	$510, %eax
	ja	load_stage2_error
	movw	%ax, fat_spc
	movl	fat_partition_lba, %eax
	movzwl	%es:0x0e, %ecx
	testl	%ecx, %ecx
	jz	load_stage2_error
	movzbl	fat_sector_scale, %edx
	imull	%edx, %ecx
	addl	%ecx, %eax
	movl	%eax, fat_lba
	movzbl	%es:0x10, %ecx
	testl	%ecx, %ecx
	jz	load_stage2_error
	movzwl	%es:0x16, %edx
	testl	%edx, %edx
	jz	load_stage2_error
	movzbl	fat_sector_scale, %ebx
	imull	%ebx, %edx
	imull	%edx, %ecx
	addl	%ecx, %eax
	movl	%eax, fat_root_lba
	movzwl	%es:0x11, %ecx
	testl	%ecx, %ecx
	jz	load_stage2_error
	movw	%cx, fat_root_entries
	shll	$5, %ecx
	addl	$511, %ecx
	shrl	$9, %ecx
	movw	%cx, fat_root_sectors
	addl	%ecx, %eax
	movl	%eax, fat_data_lba

	/* Search the FAT16 fixed root one physical sector at a time. */
	xorw	%dx, %dx
3:
	cmpw	fat_root_sectors, %dx
	jae	load_stage2_error
	movl	fat_root_lba, %eax
	movzwl	%dx, %ecx
	addl	%ecx, %eax
	xorw	%bp, %bp
	call	read_fixed_lba
	jc	load_stage2_error
	pushw	%cs
	popw	%ds
	xorw	%di, %di
	movw	$16, %cx
4:
	cmpb	$0, %es:(%di)
	je	load_stage2_error
	cmpb	$0xe5, %es:(%di)
	je	5f
	cmpb	$0x0f, %es:0x0b(%di)
	je	5f
	pushw	%cx
	pushw	%di
	movw	$stage2_name, %si
	movw	$11, %cx
	repe cmpsb
	popw	%di
	popw	%cx
	je	stage2_found
5:
	addw	$32, %di
	loop	4b
	incw	%dx
	jmp	3b

stage2_found:
	movw	%es:0x1a(%di), %ax
	cmpw	$2, %ax
	jb	load_stage2_error
	movw	%ax, fat_cluster
	movl	%es:0x1c(%di), %eax
	/* The ELF sector plus the B98S header sector are the minimum. */
	cmpl	$1024, %eax
	jb	load_stage2_error
	cmpl	$STAGE2_MAX_SIZE, %eax
	ja	load_stage2_error
	movl	%eax, stage2_file_size
	movl	%eax, stage2_remaining
	movl	$0, elf_file_off
	movl	$0, elf_run_sum
	movw	$0, ph_count_x4
	call	elf_prepare_highmem

	/*
	 * Stream the file one 512-byte physical sector at a time through
	 * the bounce buffer.  Sector 0 is the ELF header, which supplies
	 * the PT_LOAD table; every later sector is summed and its bytes
	 * scattered to the segment destinations, so the FAT walk itself
	 * is unchanged and never seeks.
	 */
stage2_cluster_loop:
	movw	$0, fat_sector_in_cluster
stage2_sector_loop:
	movzwl	fat_cluster, %eax
	subl	$2, %eax
	movzwl	fat_spc, %ecx
	imull	%ecx, %eax
	movzwl	fat_sector_in_cluster, %ecx
	addl	%ecx, %eax
	addl	fat_data_lba, %eax
	pushw	%cs
	popw	%es
	movw	$elf_bounce, %bp
	call	read_fixed_lba
	jc	load_stage2_error
	pushw	%cs
	popw	%ds
	call	elf_consume_sector
	jc	load_stage2_error
	addl	$512, elf_file_off
	cmpl	$512, stage2_remaining
	jbe	stage2_loaded
	subl	$512, stage2_remaining
	incw	fat_sector_in_cluster
	movw	fat_sector_in_cluster, %ax
	cmpw	fat_spc, %ax
	jb	stage2_sector_loop
	movw	fat_cluster, %ax
	call	stage2_fat_next
	cmpw	$0xfff8, %ax
	jae	load_stage2_error
	cmpw	$2, %ax
	jb	load_stage2_error
	movw	%ax, fat_cluster
	jmp	stage2_cluster_loop

stage2_loaded:
	/* The streamed byte-sum must match the patched header field. */
	movl	elf_run_sum, %eax
	cmpl	elf_expect_sum, %eax
	jne	load_stage2_error
	clc
	ret
load_stage2_error:
	stc
	ret

/*
 * PC-98 A20 gate, plus the GDT descriptor base used by the unreal-mode
 * copies below and by enter_stage2 later.
 */
elf_prepare_highmem:
	xorb	%al, %al
	outb	%al, $0xf2
	movb	$0x02, %al
	outb	%al, $0xf6
	movw	$0x439, %dx
	inb	%dx, %al
	andb	$0xfb, %al
	outb	%al, %dx
	xorb	%al, %al
	outb	%al, $0xf8
	outb	%al, $0x5f
	movl	$(0x10000 + gdt), %eax
	movl	%eax, gdt_desc+2
	lgdt	gdt_desc
	ret

/*
 * Dispatch one streamed sector sitting in elf_bounce.  DS = CS.
 * elf_file_off is the file offset of the sector; carry set on a
 * malformed image.
 */
elf_consume_sector:
	movl	elf_file_off, %eax
	testl	%eax, %eax
	jnz	1f
	jmp	elf_parse_header
1:
	cmpl	$512, %eax
	jne	2f
	call	elf_check_b98s
	jc	elf_consume_error
2:
	call	elf_sum_sector
	call	elf_copy_sector
	clc
	ret
elf_consume_error:
	stc
	ret

/*
 * Sector 0: the ELF header and, by the subset contract, every program
 * header.  Only identification is checked here; geometry (alignment,
 * ordering, placement) was enforced at build time by patch-stage2.py.
 */
elf_parse_header:
	cmpl	$0x464c457f, elf_bounce
	jne	elf_consume_error
	cmpw	$3, elf_bounce+18	/* e_machine: EM_386 */
	jne	elf_consume_error
	cmpl	$52, elf_bounce+28	/* e_phoff */
	jne	elf_consume_error
	cmpw	$32, elf_bounce+42	/* e_phentsize */
	jne	elf_consume_error
	movl	elf_bounce+24, %eax
	andl	$0x7fffffff, %eax
	movl	%eax, elf_entry
	movzwl	elf_bounce+44, %ecx	/* e_phnum */
	cmpw	$4, %cx
	ja	elf_consume_error
	xorw	%di, %di
	movw	$elf_bounce+52, %si
1:
	jcxz	3f
	cmpl	$1, (%si)		/* p_type == PT_LOAD */
	jne	2f
	cmpw	$16, %di
	jae	elf_consume_error
	movl	4(%si), %eax		/* p_offset */
	movl	%eax, ph_off(%di)
	movl	12(%si), %eax		/* p_paddr */
	andl	$0x7fffffff, %eax
	movl	%eax, ph_paddr(%di)
	movl	16(%si), %eax		/* p_filesz */
	movl	%eax, ph_filesz(%di)
	addw	$4, %di
2:
	addw	$32, %si
	decw	%cx
	jmp	1b
3:
	movw	%di, ph_count_x4
	testw	%di, %di
	jz	elf_consume_error
	clc
	ret

/* Sector 1 leads the low segment with the B98S v2 header. */
elf_check_b98s:
	cmpl	$0x53383942, elf_bounce
	jne	1f
	cmpw	$2, elf_bounce+4
	jne	1f
	cmpw	$20, elf_bounce+6
	jne	1f
	movl	elf_bounce+8, %eax
	cmpl	stage2_file_size, %eax
	jne	1f
	movl	elf_bounce+12, %eax
	cmpl	elf_entry, %eax
	jne	1f
	movl	elf_bounce+16, %eax
	movl	%eax, elf_expect_sum
	clc
	ret
1:
	stc
	ret

/*
 * Add this sector's payload bytes to the running sum.  The checksum
 * covers file offset 532 onward: sector 1 skips its 20-byte header,
 * and the final sector only counts up to the file size.
 */
elf_sum_sector:
	movl	stage2_remaining, %ecx
	cmpl	$512, %ecx
	jbe	1f
	movl	$512, %ecx
1:
	movw	$elf_bounce, %si
	cmpl	$512, elf_file_off
	jne	2f
	addw	$20, %si
	subl	$20, %ecx
2:
	movl	elf_run_sum, %edx
	jecxz	4f
3:
	movzbl	(%si), %eax
	addl	%eax, %edx
	incw	%si
	decl	%ecx
	jnz	3b
4:
	movl	%edx, elf_run_sum
	ret

/*
 * Scatter the valid bytes of this sector to every PT_LOAD range they
 * intersect.  Segments are ascending and sector-aligned, so at most
 * one intersects, but the general walk costs nothing and needs no
 * assumptions.  Gap sectors between segments simply copy nowhere.
 */
elf_copy_sector:
	movl	stage2_remaining, %eax
	cmpl	$512, %eax
	jbe	1f
	movl	$512, %eax
1:
	movl	%eax, elf_valid
	xorw	%di, %di
2:
	cmpw	ph_count_x4, %di
	jae	6f
	/* EAX = lo = max(file_off, p_offset) */
	movl	elf_file_off, %eax
	movl	ph_off(%di), %ebx
	cmpl	%ebx, %eax
	jae	3f
	movl	%ebx, %eax
3:
	/* ECX = hi = min(file_off + valid, p_offset + p_filesz) */
	movl	elf_file_off, %ecx
	addl	elf_valid, %ecx
	movl	%ebx, %edx
	addl	ph_filesz(%di), %edx
	cmpl	%edx, %ecx
	jbe	4f
	movl	%edx, %ecx
4:
	cmpl	%eax, %ecx
	jbe	5f
	subl	%eax, %ecx		/* ECX = length */
	movl	%eax, %esi		/* ESI = bounce linear source */
	subl	elf_file_off, %esi
	addl	$(0x10000 + elf_bounce), %esi
	subl	%ebx, %eax		/* EAX = destination linear */
	addl	ph_paddr(%di), %eax
	pushw	%di
	movl	%eax, %edi
	call	unreal_copy
	popw	%di
5:
	addw	$4, %di
	jmp	2b
6:
	ret

/*
 * Copy ECX bytes from linear ESI to linear EDI.  The flat 4GB data
 * descriptor is loaded into FS through a protected-mode round trip on
 * every call, so nothing that happens between copies — BIOS calls
 * included — can leave a stale segment behind.
 */
unreal_copy:
	cli
	movl	%cr0, %eax
	orl	$1, %eax
	movl	%eax, %cr0
	movw	$0x10, %ax
	movw	%ax, %fs
	movl	%cr0, %eax
	andl	$0xfffffffe, %eax
	movl	%eax, %cr0
	sti
1:
	movb	%fs:(%esi), %al
	movb	%al, %fs:(%edi)
	incl	%esi
	incl	%edi
	decl	%ecx
	jnz	1b
	ret

stage2_fat_next:
	pushw	%bx
	pushw	%cx
	pushw	%dx
	pushw	%es
	pushw	%bp
	cmpb	$HO_FS_FAT12, fat_fs_hint
	je	stage2_fat12_next
	movzwl	%ax, %edx
	shll	$1, %edx
	movl	%edx, %eax
	shrl	$9, %eax
	addl	fat_lba, %eax
	andw	$511, %dx
	movw	$TABLE_SEG, %bx
	movw	%bx, %es
	xorw	%bp, %bp
	call	read_fixed_lba
	jc	1f
	movw	%dx, %bx
	movw	%es:(%bx), %ax
	jmp	2f
1:
	movw	$0xffff, %ax
2:
stage2_fat_next_done:
	pushw	%cs
	popw	%ds
	popw	%bp
	popw	%es
	popw	%dx
	popw	%cx
	popw	%bx
	ret

	/* 12-bit entries are packed three bytes per two clusters and may
	 * straddle a FAT sector; the cluster parity selects the nibble
	 * alignment.  End and reserved marks are normalized to 0xffff so
	 * the shared 0xfff8 checks in the caller keep working. */
stage2_fat12_next:
	movw	%ax, %cx
	movzwl	%ax, %edx
	movl	%edx, %eax
	shrl	$1, %eax
	addl	%edx, %eax
	movl	%eax, %edx
	shrl	$9, %eax
	addl	fat_lba, %eax
	andw	$511, %dx
	movw	$TABLE_SEG, %bx
	movw	%bx, %es
	xorw	%bp, %bp
	call	read_fixed_lba
	jc	3f
	movw	%dx, %bx
	movb	%es:(%bx), %dl
	cmpw	$511, %bx
	jne	1f
	incl	%eax
	call	read_fixed_lba
	jc	3f
	xorw	%bx, %bx
	movb	%es:(%bx), %dh
	jmp	2f
1:
	movb	%es:1(%bx), %dh
2:
	movw	%dx, %ax
	testb	$1, %cl
	jz	4f
	shrw	$4, %ax
	jmp	5f
4:
	andw	$0xfff, %ax
5:
	cmpw	$0xff0, %ax
	jb	stage2_fat_next_done
3:
	movw	$0xffff, %ax
	jmp	stage2_fat_next_done

/* Fixed-disk one-sector read. EAX=LBA, ES:BP=destination. */
read_fixed_lba:
	pushl	%eax
	pushw	%bx
	pushw	%cx
	pushw	%dx
	pushw	%si
	pushw	%di
	pushw	%ds
	pushw	%es
	pushw	%bp
	xorl	%edx, %edx
	movw	%cs:chain_entry, %si
	movzbl	%cs:MENU_HEADS(%si), %edi
	movzbl	%cs:MENU_SECTORS(%si), %ecx
	imull	%ecx, %edi
	testl	%edi, %edi
	jz	read_fixed_lba_invalid
	divl	%edi
	cmpl	$0xffff, %eax
	ja	read_fixed_lba_invalid
	movw	%ax, %cx
	movl	%edx, %eax
	xorl	%edx, %edx
	movzbl	%cs:MENU_SECTORS(%si), %edi
	divl	%edi
	movb	%al, %dh
	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%cs:chain_entry, %si
	movb	%cs:MENU_BIOS_ID(%si), %al
	movb	%al, %ah
	andb	$0xf0, %ah
	cmpb	$0x30, %ah
	jne	1f
	/* 1.44 MiB floppy: CL=cylinder, CH=N for 512 bytes, 1-origin DL. */
	movb	$2, %ch
	incb	%dl
	movb	$0x56, %ah
	jmp	2f
1:
	movb	$0x06, %ah
2:
	movw	$512, %bx
	movw	HANDOFF+HO_SAVED_SI, %si
	movw	HANDOFF+HO_SAVED_DI, %di
	int	$0x1b
	pushfw
	popw	%ax
	movw	%ax, %cs:disk_flags
	jmp	read_fixed_lba_done
read_fixed_lba_invalid:
	movw	$1, %cs:disk_flags
read_fixed_lba_done:
	popw	%bp
	popw	%es
	popw	%ds
	popw	%di
	popw	%si
	popw	%dx
	popw	%cx
	popw	%bx
	popl	%eax
	pushw	%cs:disk_flags
	popfw
	ret

/* Fixed-disk one-sector write. EAX=LBA, ES:BP=source. */
write_fixed_lba:
	pushl	%eax
	pushw	%bx
	pushw	%cx
	pushw	%dx
	pushw	%si
	pushw	%di
	pushw	%ds
	pushw	%es
	pushw	%bp
	xorl	%edx, %edx
	movw	%cs:chain_entry, %si
	movzbl	%cs:MENU_HEADS(%si), %edi
	movzbl	%cs:MENU_SECTORS(%si), %ecx
	imull	%ecx, %edi
	testl	%edi, %edi
	jz	write_fixed_lba_invalid
	divl	%edi
	cmpl	$0xffff, %eax
	ja	write_fixed_lba_invalid
	movw	%ax, %cx
	movl	%edx, %eax
	xorl	%edx, %edx
	movzbl	%cs:MENU_SECTORS(%si), %edi
	divl	%edi
	movb	%al, %dh
	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%cs:chain_entry, %si
	movb	%cs:MENU_BIOS_ID(%si), %al
	movb	%al, %ah
	andb	$0xf0, %ah
	cmpb	$0x30, %ah
	jne	1f
	/* 1.44 MiB floppy: CL=cylinder, CH=N for 512 bytes, 1-origin DL. */
	movb	$2, %ch
	incb	%dl
	movb	$0x55, %ah
	jmp	2f
1:
	movb	$0x05, %ah
2:
	movw	$512, %bx
	movw	HANDOFF+HO_SAVED_SI, %si
	movw	HANDOFF+HO_SAVED_DI, %di
	int	$0x1b
	pushfw
	popw	%ax
	movw	%ax, %cs:disk_flags
	jmp	write_fixed_lba_done
write_fixed_lba_invalid:
	movw	$1, %cs:disk_flags
write_fixed_lba_done:
	popw	%bp
	popw	%es
	popw	%ds
	popw	%di
	popw	%si
	popw	%dx
	popw	%cx
	popw	%bx
	popl	%eax
	pushw	%cs:disk_flags
	popfw
	ret

/*
 * Invoke one fixed-disk INT 1Bh/AH=84h SENSE operation. The PC-98 firmware
 * is not assumed to preserve general registers; only the documented result
 * is copied into the local result fields below.
 */
bios_sense:
	pushw	%ax
	pushw	%bx
	pushw	%cx
	pushw	%dx
	pushw	%si
	pushw	%di
	pushw	%ds
	pushw	%es
	pushw	%bp
	pushw	%ds
	xorw	%bx, %bx
	movw	%bx, %ds
	movw	HANDOFF+HO_SAVED_SI, %si
	movw	HANDOFF+HO_SAVED_DI, %di
	popw	%ds
	movw	$0x0100, %bx
	movb	$0x84, %ah
	int	$0x1b
	movb	%ah, %cs:sense_status
	movw	%bx, %cs:sense_bytes
	movw	%cx, %cs:sense_cylinders
	movb	%dh, %cs:sense_heads
	movb	%dl, %cs:sense_sectors
	pushfw
	popw	%ax
	movw	%ax, %cs:sense_flags
	popw	%bp
	popw	%es
	popw	%ds
	popw	%di
	popw	%si
	popw	%dx
	popw	%cx
	popw	%bx
	popw	%ax
	pushw	%cs:sense_flags
	popfw
	ret

/* Confirm an IDE SENSE result with one read-only sector-zero access. */
bios_probe_read_lba0:
	pushw	%ax
	pushw	%bx
	pushw	%cx
	pushw	%dx
	pushw	%si
	pushw	%di
	pushw	%ds
	pushw	%es
	pushw	%bp
	pushw	%ds
	xorw	%bx, %bx
	movw	%bx, %ds
	movw	HANDOFF+HO_SAVED_SI, %si
	movw	HANDOFF+HO_SAVED_DI, %di
	popw	%ds
	movw	$TABLE_SEG, %bx
	movw	%bx, %es
	xorw	%bp, %bp
	movw	$512, %bx
	xorw	%cx, %cx
	xorw	%dx, %dx
	movb	$0x06, %ah
	int	$0x1b
	movb	%ah, %cs:probe_read_status
	pushfw
	popw	%ax
	movw	%ax, %cs:probe_read_flags
	popw	%bp
	popw	%es
	popw	%ds
	popw	%di
	popw	%si
	popw	%dx
	popw	%cx
	popw	%bx
	popw	%ax
	pushw	%cs:probe_read_flags
	popfw
	ret

/* Return RTC seconds as binary 0..59, or ffh for invalid BCD. */
read_clock_second:
	pushw	%bx
	pushw	%cx
	pushw	%dx
	pushw	%si
	pushw	%di
	pushw	%ds
	pushw	%es
	pushw	%cs
	popw	%es
	movw	$clock_calendar, %bx
	xorb	%ah, %ah
	int	$0x1c
	movb	%es:5(%bx), %al
	movb	%al, %dl
	andb	$0x0f, %dl
	cmpb	$9, %dl
	ja	1f
	andb	$0xf0, %al
	shrb	$4, %al
	cmpb	$5, %al
	ja	1f
	movb	$10, %cl
	mulb	%cl
	addb	%dl, %al
	jmp	2f
1:
	movb	$0xff, %al
2:
	popw	%es
	popw	%ds
	popw	%di
	popw	%si
	popw	%dx
	popw	%cx
	popw	%bx
	ret

	.code32
bios_gateway32:
	pushl	%ebp
	pushl	%edi
	pushl	%esi
	pushl	%ebx
	movl	20(%esp), %eax
	testl	%eax, %eax
	jz	gateway32_invalid
	cmpl	$0x000ffff0, %eax
	ja	gateway32_invalid
	movl	%eax, (0x10000 + gateway_request)
	movb	$0, (0x10000 + gateway_active_service)
	cmpw	$11, (%eax)
	jne	gateway32_enter_real

	/* Validate a one-sector fixed-disk write before copying caller memory. */
	movzbl	4(%eax), %edx
	movl	%edx, %ecx
	andb	$0xfc, %cl
	cmpb	$0x80, %cl
	je	1f
	movl	%edx, %ecx
	andb	$0xf0, %cl
	cmpb	$0x30, %cl
	je	1f
	andb	$0xf8, %dl
	cmpb	$0xa0, %dl
	jne	gateway32_invalid
1:
	movzbl	5(%eax), %ecx
	testl	%ecx, %ecx
	jz	gateway32_invalid
	movzbl	6(%eax), %edx
	testl	%edx, %edx
	jz	gateway32_invalid
	imull	%edx, %ecx
	movl	8(%eax), %eax
	xorl	%edx, %edx
	divl	%ecx
	cmpl	$0xffff, %eax
	ja	gateway32_invalid
	movl	(0x10000 + gateway_request), %eax
	movl	12(%eax), %esi
	testl	%esi, %esi
	jz	gateway32_invalid
	cmpl	$0xfffffe00, %esi
	ja	gateway32_invalid
	movl	$0x00008000, %edi
	movl	$128, %ecx
	cld
	rep movsl

gateway32_enter_real:
	movl	%esp, (0x10000 + gateway_saved_esp)
	cli
	ljmp	$0x18, $gateway16_pm

gateway32_invalid:
	movl	$0xffffffff, %eax
	popl	%ebx
	popl	%esi
	popl	%edi
	popl	%ebp
	ret

gateway32_return:
	movw	$0x10, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	%ax, %fs
	movw	%ax, %gs
	cmpb	$1, (0x10000 + gateway_active_service)
	jne	1f
	cmpl	$0, (0x10000 + gateway_result)
	jne	1f
	movl	$0x00008000, %esi
	movl	(0x10000 + gateway_buffer), %edi
	movl	$128, %ecx
	cld
	rep movsl
1:
	movl	(0x10000 + gateway_saved_esp), %esp
	movl	(0x10000 + gateway_result), %eax
	popl	%ebx
	popl	%esi
	popl	%edi
	popl	%ebp
	ret

	.code16
gateway16_pm:
	movw	$0x20, %ax
	movw	%ax, %ss
	movl	$0x0009eff0, %esp
	movl	%cr0, %eax
	andl	$0xfffffffe, %eax
	movl	%eax, %cr0
	ljmp	$0x1000, $gateway_real

gateway_real:
	movw	%cs, %ax
	movw	%ax, %ds
	movw	$STACK_SEG, %ax
	movw	%ax, %ss
	movl	$0x0000eff0, %esp
	sti
	movl	gateway_request, %eax
	movl	%eax, %ebx
	andw	$0x000f, %bx
	shrl	$4, %eax
	movw	%ax, %es
	movw	%es:(%bx), %ax
	cmpw	$13, %ax
	je	gateway_key_state
	cmpw	$12, %ax
	je	gateway_display_stop
	cmpw	$11, %ax
	je	gateway_disk_write
	cmpw	$10, %ax
	je	gateway_probe_fixed
	cmpw	$9, %ax
	je	gateway_clock_second
	cmpw	$8, %ax
	je	gateway_chain_boot
	cmpw	$7, %ax
	je	gateway_reprobe
	cmpw	$5, %ax
	je	gateway_return_menu
	cmpw	$4, %ax
	je	gateway_display_reset
	cmpw	$3, %ax
	je	gateway_key_poll
	cmpw	$2, %ax
	je	gateway_key_read
	cmpw	$1, %ax
	je	gateway_disk_read
	movl	$0xffffffff, gateway_result
	jmp	gateway_real_return

gateway_clock_second:
	call	read_clock_second
	movzbl	%al, %eax
	movl	%eax, %cs:gateway_result
	jmp	gateway_real_return

gateway_key_read:
	/*
	 * Use the baseline NEC BIOS calls rather than the AH=07h extended input
	 * service: some genuine ROM revisions leave AH=07h waiting even though
	 * their standard BIOS key buffer is non-empty.  AH=00h consumes the
	 * key-data pair.  The current Shift/Graph/Ctrl byte is read from the
	 * standard BIOS work area at 0000:053Ah because genuine ROM revisions
	 * also disagree about the AH=02h return register.  Keep the key in bits
	 * 15:0 and expose that modifier byte in bits 23:16 so the 32-bit Term
	 * adapter can distinguish C-SPC and Graph-x.
	 */
	movb	$0x00, %ah
	int	$0x18
	pushw	%ax
	pushw	%ds
	xorw	%ax, %ax
	movw	%ax, %ds
	movzbl	0x053a, %edx
	popw	%ds
	popw	%ax
	shll	$16, %edx
	movzwl	%ax, %eax
	orl	%edx, %eax
	movl	%eax, %cs:gateway_result
	jmp	gateway_real_return

gateway_key_poll:
	/*
	 * Poll the standard PC-9800 BIOS key-buffer count at 0000:0528h.  NEC
	 * ROM revisions do not agree with the compatible BIOS about which
	 * registers INT 18h/AH=01h preserves or uses to report an empty queue.
	 * The work-area count is the common ABI and avoids interpreting an
	 * undocumented return register.  The consuming AH=00h call remains the
	 * single source of the key-data and modifier snapshot.
	 *
	 * vmunix has no protected-mode IDT and therefore runs with IF clear.
	 * Do not use STI/HLT here: the compatible BIOS deliberately leaves the
	 * PIT stopped when no BIOS timer event is active, so an empty queue could
	 * sleep forever and prevent the menu timeout from advancing.  Returning
	 * immediately also preserves the poll service's non-blocking contract.
	 */
	pushw	%ds
	xorw	%ax, %ax
	movw	%ax, %ds
	cmpb	$0, 0x0528
	popw	%ds
	jz	2f
	movl	$0, %cs:gateway_result
	jmp	gateway_real_return
2:
	movl	$0xffffffff, %cs:gateway_result
	jmp	gateway_real_return

gateway_key_state:
	/*
	 * Read one byte of the real-time key state table the ROM keyboard
	 * handler keeps at 0000:052Ah (16 bytes, one bit per pressed key,
	 * scan code = group * 8 + bit).  request.status selects the group.
	 * The work-area read avoids interpreting INT 18h/AH=04h return
	 * registers, matching the key-poll service above.  The STI at
	 * gateway entry opens the interrupt window that lets the handler
	 * refresh the table while Stage 2 runs with IF clear.
	 */
	movw	%es:2(%bx), %bx
	cmpw	$15, %bx
	ja	3f
	pushw	%ds
	xorw	%ax, %ax
	movw	%ax, %ds
	movzbl	0x052a(%bx), %eax
	popw	%ds
	movl	%eax, %cs:gateway_result
	jmp	gateway_real_return
3:
	movl	$0xffffffff, %cs:gateway_result
	jmp	gateway_real_return

gateway_display_reset:
	movb	$0x42, %ah
	movb	$0xc0, %ch
	int	$0x18
	movb	$0x40, %ah
	int	$0x18
	movl	$0, %cs:gateway_result
	jmp	gateway_real_return

gateway_display_stop:
	movb	$0x41, %ah
	int	$0x18
	movl	$0, %cs:gateway_result
	jmp	gateway_real_return

/* vmunix now owns its menu. These legacy service numbers remain stable. */
gateway_return_menu:
gateway_reprobe:
	movl	$0, %cs:gateway_result
	jmp	gateway_real_return

/*
 * Probe exactly one fixed BIOS unit. request.status supplies the IDE/SCSI
 * class and request.buffer receives one 16-byte zedbsd_device descriptor.
 */
gateway_probe_fixed:
	movb	%es:2(%bx), %al
	movb	%al, gateway_probe_class
	movb	%es:4(%bx), %al
	movb	%al, gateway_bios_id
	movl	%es:12(%bx), %eax
	movl	%eax, gateway_buffer
	testl	%eax, %eax
	jz	gateway_probe_failed
	cmpl	$0x000ffff0, %eax
	ja	gateway_probe_failed
	cmpb	$DEV_CLASS_IDE, gateway_probe_class
	je	1f
	cmpb	$DEV_CLASS_SCSI, gateway_probe_class
	jne	gateway_probe_failed
	movb	gateway_bios_id, %al
	andb	$0xf8, %al
	cmpb	$0xa0, %al
	jne	gateway_probe_failed
	jmp	2f
1:
	movb	gateway_bios_id, %al
	andb	$0xfc, %al
	cmpb	$0x80, %al
	jne	gateway_probe_failed
2:
	movb	gateway_bios_id, %al
	call	bios_sense
	pushw	%cs
	popw	%ds
	jc	gateway_probe_failed
	cmpb	$0x20, sense_status
	jae	gateway_probe_failed
	cmpw	$512, sense_bytes
	jne	gateway_probe_failed
	cmpb	$0, sense_heads
	je	gateway_probe_failed
	cmpb	$0, sense_sectors
	je	gateway_probe_failed
	cmpb	$DEV_CLASS_IDE, gateway_probe_class
	jne	3f
	movb	gateway_bios_id, %al
	call	bios_probe_read_lba0
	pushw	%cs
	popw	%ds
	jc	gateway_probe_failed
	cmpb	$0x20, probe_read_status
	jae	gateway_probe_failed
3:
	movl	gateway_buffer, %eax
	movw	%ax, %di
	andw	$0x000f, %di
	shrl	$4, %eax
	movw	%ax, %es
	pushw	%di
	movw	$DEV_DESC_SIZE, %cx
	xorb	%al, %al
	cld
	rep stosb
	popw	%di
	movb	gateway_probe_class, %al
	movb	%al, %es:DEV_CLASS(%di)
	movb	gateway_bios_id, %al
	movb	%al, %ah
	andb	$0x0f, %ah
	movb	%ah, %es:DEV_INDEX(%di)
	movb	%al, %es:DEV_BIOS_ID(%di)
	movb	$(DEV_FLAG_PRESENT | DEV_FLAG_GEOMETRY), %es:DEV_FLAGS(%di)
	movw	sense_bytes, %ax
	movw	%ax, %es:DEV_SECTOR_SIZE(%di)
	movw	sense_cylinders, %ax
	movw	%ax, %es:DEV_CYLINDERS(%di)
	movb	sense_heads, %al
	movb	%al, %es:DEV_HEADS(%di)
	movb	sense_sectors, %al
	movb	%al, %es:DEV_SECTORS(%di)
	movb	sense_status, %al
	movb	%al, %es:10(%di)
	movl	$0, %cs:gateway_result
	jmp	gateway_real_return

gateway_probe_failed:
	movl	$0xffffffff, %cs:gateway_result
	jmp	gateway_real_return

/* Chain a fixed-disk IPL or partition PBR without a Stage-1 menu table. */
gateway_chain_boot:
	pushw	%cs
	popw	%ds
	movw	%es:2(%bx), %ax
	movw	%ax, gateway_module_type
	movb	%es:4(%bx), %al
	movb	%al, gateway_menu+MENU_BIOS_ID
	movb	%es:5(%bx), %al
	movb	%al, gateway_menu+MENU_HEADS
	movb	%es:6(%bx), %al
	movb	%al, gateway_menu+MENU_SECTORS
	movl	%es:8(%bx), %eax
	movl	%eax, gateway_lba
	movl	%eax, gateway_menu+MENU_LBA
	movw	$gateway_menu, chain_entry
	movw	$0x1fc0, %ax
	movw	%ax, %es
	movl	$1, %eax
	movw	$0x0200, %bp
	call	read_fixed_lba
	jc	gateway_chain_error
	movl	gateway_lba, %eax
	xorw	%bp, %bp
	call	read_fixed_lba
	jc	gateway_chain_error
	cmpw	$0, gateway_module_type
	je	gateway_chain_jump

	/* Locate the selected native entry by its IPL LBA and move it first. */
	xorw	%si, %si
	movw	$16, %cx
gateway_chain_find_partition:
	cmpb	$0, %es:0x0200(%si)
	je	gateway_chain_next_partition
	movzwl	%es:0x0206(%si), %eax
	movzbl	gateway_menu+MENU_HEADS, %edx
	imull	%edx, %eax
	movzbl	%es:0x0205(%si), %edx
	addl	%edx, %eax
	movzbl	gateway_menu+MENU_SECTORS, %edx
	imull	%edx, %eax
	movzbl	%es:0x0204(%si), %edx
	addl	%edx, %eax
	cmpl	gateway_lba, %eax
	je	gateway_chain_partition_found
gateway_chain_next_partition:
	addw	$32, %si
	loop	gateway_chain_find_partition
	jmp	gateway_chain_error

gateway_chain_partition_found:
	addw	$0x0200, %si
	pushw	%ds
	pushw	%es
	popw	%ds
	movw	$0x0200, %di
	movw	$16, %cx
	rep movsw
	popw	%ds

gateway_chain_jump:
	cli
	/*
	 * Keep the complete BIOS unit number in the BIOS work area.  In
	 * particular, 0x81 must remain HDD 2: storing only its low seven bits
	 * makes NEC's DOS PBR continue loading MSDOS.SYS from HDD 1.
	 */
	movb	gateway_menu+MENU_BIOS_ID, %al
	pushw	%ds
	xorw	%dx, %dx
	movw	%dx, %ds
	movb	%al, 0x0584
	movw	HANDOFF+HO_SAVED_SI, %si
	movw	HANDOFF+HO_SAVED_DI, %di
	popw	%ds
	sti
	ljmp	$0x1fc0, $0

gateway_chain_error:
	pushw	%cs
	popw	%ds
	movl	$0x0008ffff, gateway_result
	jmp	gateway_real_return

gateway_disk_read:
	movb	$1, gateway_active_service
	movb	%es:4(%bx), %al
	movb	%al, gateway_menu+MENU_BIOS_ID
	movb	%es:5(%bx), %al
	movb	%al, gateway_menu+MENU_HEADS
	movb	%es:6(%bx), %al
	movb	%al, gateway_menu+MENU_SECTORS
	movl	%es:8(%bx), %eax
	movl	%eax, gateway_lba
	movl	%es:12(%bx), %eax
	movl	%eax, gateway_buffer
	xorw	%bp, %bp
	movw	$TABLE_SEG, %ax
	movw	%ax, %es
	movw	$gateway_menu, chain_entry
	movl	gateway_lba, %eax
	call	read_fixed_lba
	jc	gateway_disk_error
	pushw	%cs
	popw	%ds
	movl	$0, gateway_result
	jmp	gateway_real_return
gateway_disk_error:
	pushw	%cs
	popw	%ds
	movl	$0x0001ffff, gateway_result
	jmp	gateway_real_return

gateway_disk_write:
	movb	$2, gateway_active_service
	movb	%es:4(%bx), %al
	movb	%al, gateway_menu+MENU_BIOS_ID
	movb	%es:5(%bx), %al
	movb	%al, gateway_menu+MENU_HEADS
	movb	%es:6(%bx), %al
	movb	%al, gateway_menu+MENU_SECTORS
	movl	%es:8(%bx), %eax
	movl	%eax, gateway_lba
	movl	%es:12(%bx), %eax
	movl	%eax, gateway_buffer
	xorw	%bp, %bp
	movw	$TABLE_SEG, %ax
	movw	%ax, %es
	movw	$gateway_menu, chain_entry
	movl	gateway_lba, %eax
	call	write_fixed_lba
	jc	gateway_disk_write_error
	pushw	%cs
	popw	%ds
	movl	$0, gateway_result
	jmp	gateway_real_return
gateway_disk_write_error:
	pushw	%cs
	popw	%ds
	movl	$0x000bffff, gateway_result

gateway_real_return:
	cli
	movw	%cs, %ax
	movw	%ax, %ds
	lgdt	gdt_desc
	movl	%cr0, %eax
	orl	$1, %eax
	movl	%eax, %cr0
	ljmpl	*gateway_pm_return

enter_stage2:
	movl	$0x48323842, stage2_handoff
	movw	$2, stage2_handoff+4
	movw	$24, stage2_handoff+6
	movb	device_count, %al
	movb	%al, stage2_handoff+8
	pushw	%ds
	xorw	%ax, %ax
	movw	%ax, %ds
	movb	HANDOFF+HO_BOOT_BIOS_ID, %al
	popw	%ds
	movb	%al, stage2_handoff+9
	movl	$(0x10000 + device_table), stage2_handoff+12
	movl	$(0x10000 + bios_gateway32), stage2_handoff+16
	movl	fat_partition_lba, %eax
	movl	%eax, stage2_handoff+20
	movl	$(0x10000 + protected_entry), %eax
	movl	%eax, pm_jump
	movl	$(0x10000 + gdt), %eax
	movl	%eax, gdt_desc+2
	lgdt	gdt_desc
	cli
	movl	%cr0, %eax
	orl	$1, %eax
	movl	%eax, %cr0
	ljmpl	*pm_jump

	.code32
protected_entry:
	movw	$0x10, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	%ax, %fs
	movw	%ax, %gs
	movl	$(0x10000 + stage2_handoff), %ebx
	/* elf_parse_header already validated and saved the physical entry. */
	movl	(0x10000 + elf_entry), %eax
	jmp	*%eax
	.code16

fatal_handoff_message: .asciz "zedBSD: invalid PBR handoff"
fatal_stage2_message:  .asciz "zedBSD: cannot load vmunix"
stage2_name:           .ascii "VMUNIX     "

	.balign 2
disk_flags:            .word 0
chain_entry:           .word 0
clock_calendar:        .space 6
device_count:          .byte 0
boot_bios_id:          .byte 0
fat_sector_scale:      .byte 0
fat_fs_hint:           .byte 0
	.balign 2
fat_root_entries:      .word 0
fat_root_sectors:      .word 0
fat_cluster:           .word 0
fat_spc:               .word 0
fat_sector_in_cluster: .word 0
ph_count_x4:           .word 0
	.balign 4
fat_partition_lba:     .long 0
fat_lba:               .long 0
fat_root_lba:          .long 0
fat_data_lba:          .long 0
stage2_file_size:      .long 0
stage2_remaining:      .long 0
elf_file_off:          .long 0
elf_entry:             .long 0
elf_expect_sum:        .long 0
elf_run_sum:           .long 0
elf_valid:             .long 0
ph_off:                .long 0, 0, 0, 0
ph_paddr:              .long 0, 0, 0, 0
ph_filesz:             .long 0, 0, 0, 0
	.balign 4
elf_bounce:            .space 512

	.balign 4
stage2_handoff:        .space 24
pm_jump:               .long 0
	.word 0x08
	.balign 8
gdt:
	.quad 0
	.quad 0x00cf9a000000ffff
	.quad 0x00cf92000000ffff
	.quad 0x00009a010000ffff
	.quad 0x000092000000ffff
gdt_end:
gdt_desc:
	.word gdt_end - gdt - 1
	.long 0
gateway_pm_return:
	.long 0x10000 + gateway32_return
	.word 0x08
gateway_request:        .long 0
gateway_saved_esp:      .long 0
gateway_result:         .long 0
gateway_active_service: .byte 0
gateway_bios_id:        .byte 0
gateway_probe_class:    .byte 0
sense_status:           .byte 0
sense_heads:            .byte 0
sense_sectors:          .byte 0
probe_read_status:      .byte 0
	.balign 2
sense_bytes:            .word 0
sense_cylinders:        .word 0
sense_flags:            .word 0
probe_read_flags:       .word 0
	.balign 4
gateway_lba:            .long 0
gateway_buffer:         .long 0
gateway_module_type:    .word 0
	.balign 16
gateway_menu:           .space MENU_SIZE
	.balign 16
device_table:           .space DEV_DESC_SIZE * MAX_FIXED_DEVICES
