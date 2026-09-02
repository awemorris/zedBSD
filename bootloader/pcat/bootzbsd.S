/* PC/AT ZBL2 Stage 2: MBR/FAT16 and ELF32/ELF64 loader. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
.intel_syntax noprefix
.code16
.section .text
.global stage2_entry

#include "bootloader/include/disk-layout.inc"
#include "bootloader/include/stage2-header.inc"
#include "bootloader/include/mbr.inc"
#include "bootloader/include/fat16.inc"
#include "bootloader/include/elf.inc"
#include "bootloader/include/amd64-handoff.h"
#include "bootloader/include/boot-parameter-record.inc"
#include "bootloader/bios/fat-directory.h"
#include "bootloader/uefi/zedbsd-config.h"

.set PH_RECORD_SIZE, 28
.set FAT_ATTRIBUTE_DIRECTORY, 0x10
.set KERNEL_PHYSICAL_MIN, 0x00100000
.set KERNEL_PHYSICAL_LIMIT, 0x40000000
.set KERNEL_HIGH_WINDOW_MIN, 0x80000000
.set KERNEL_HIGH_WINDOW_LIMIT, 0xc0000000

/* LLVM's integrated assembler otherwise selects ELF32 near call/return
 * operands even inside .code16.  Keep hand-written real-mode routines on a
 * true 16-bit near-call ABI; the two .code16gcc C calls below remain 32-bit. */
.macro call16 target
	.byte 0xe8
	.word \target - . - 2
.endm
.macro ret16
	.byte 0xc3
.endm

stage2_header:
	.long ZBL2_MAGIC
	.word ZBL2_VERSION
	.word ZBL2_HEADER_SIZE
	.long stage2_end
	.word 0                         /* finalized sector count */
	.word ZBL2_MACHINE_PCAT
	.long stage2_entry
	.long 0                         /* finalized dword checksum */
	.long 0
	.long 0

stage2_entry:
	cli
	cld
	mov ax, cs
	cmp ax, 0x1000
	je stage2_resident
	/* DOS loaded the MZ module at an arbitrary paragraph.  Remember the
	 * current DOS drive, then move the complete loader to its fixed address. */
	mov ds, ax
	sti
	mov ah, 0x19
	int 0x21
	add al, 0x7e                  /* DOS C:=02 -> BIOS fixed disk 80h. */
	cli
	mov dx, cs
	mov ds, dx
	push ds
	xor dx, dx
	mov ds, dx
	mov dword ptr [0x0700], 0x3142545a
	mov byte ptr [0x0704], al
	pop ds
	mov ax, 0x1000
	mov es, ax
	xor si, si
	xor di, di
	mov cx, word ptr [ZBL2_SECTOR_COUNT]
	shl cx, 8
	rep movsw
	/* 16:16 immediate far jump (EA offset16 segment16). */
	.byte 0xea
	.word stage2_entry
	.word 0x1000
stage2_resident:
	mov ds, ax
	mov ss, ax
	mov esp, 0xf000
	xor eax, eax
	sti
	push ds
	xor ax, ax
	mov ds, ax
	cmp dword ptr [0x0700], 0x3142545a
	jne 9f
	mov dl, byte ptr [0x0704]
9:	pop ds
	mov byte ptr [boot_drive], dl
	mov si, offset msg_start
	call16 puts

	/* Conventional-memory query used by the Multiboot v1 handoff. */
	mov ax, 0x8800
	int 0x15
	jnc 1f
	mov ax, 0x3c00
1:	test ax, ax
	jnz 2f
	mov ax, 0x3c00
2:	movzx eax, ax
	mov dword ptr [mbi_mem_upper], eax
	call16 sample_vga_font

	xor eax, eax
	call16 disk_read_one
	cmp word ptr es:[MBR_SIGNATURE], 0xaa55
	jne fatal_mbr
	mov si, offset msg_mbr
	call16 puts

	/* Validate statuses, then select the first active primary entry. */
	mov si, MBR_TABLE_OFFSET
	xor bx, bx
	mov cx, 4
mbr_loop:
	mov al, byte ptr es:[si]
	cmp al, 0
	je mbr_status_ok
	cmp al, MBR_ACTIVE
	jne fatal_mbr
	test bx, bx
	jnz mbr_status_ok
	mov bl, 1
	mov al, byte ptr es:[si + 4]
	cmp al, MBR_TYPE_FAT16_16M
	je mbr_type_ok
	cmp al, MBR_TYPE_FAT16
	je mbr_type_ok
	cmp al, MBR_TYPE_FAT16_LBA
	je mbr_type_ok
	cmp al, MBR_TYPE_FAT32
	je mbr_type_ok
	cmp al, MBR_TYPE_FAT32_LBA
	jne fatal_partition
mbr_type_ok:
	mov eax, dword ptr es:[si + 8]
	test eax, eax
	jz fatal_partition
	mov dword ptr [partition_lba], eax
	mov edx, dword ptr es:[si + 12]
	test edx, edx
	jz fatal_partition
	mov dword ptr [partition_blocks], edx
	add eax, edx
	jc fatal_partition
	mov dword ptr [partition_end_lba], eax
	mov ax, 5
	sub ax, cx
	mov byte ptr [partition_index], al
mbr_status_ok:
	add si, MBR_ENTRY_SIZE
	loop mbr_loop
	test bx, bx
	jz fatal_partition
	mov si, offset msg_partition
	call16 puts

	call16 fat16_mount
	call16 load_zedbsd_configuration
	mov byte ptr [path_role], 1
	mov si, offset bios_configuration
	call16 fat_find_path
	mov si, offset msg_kernel
	call16 puts

	xor eax, eax
	call16 fat16_read_file_sector
	cmp dword ptr es:[0], 0x464c457f
	jne fatal_elf
	mov al, byte ptr es:[EI_CLASS]
	cmp al, ELFCLASS32
	je parse_elf32
	cmp al, ELFCLASS64
	je parse_elf64
	jmp fatal_elf

/* ------------------------------------------------------------------ */
/* ELF32/i386. */
parse_elf32:
	cmp dword ptr [file_size], 52
	jb fatal_elf
	cmp word ptr es:[18], EM_386
	jne fatal_elf
	cmp word ptr es:[40], 52
	jne fatal_elf
	cmp word ptr es:[42], 32
	jne fatal_elf
	movzx ecx, word ptr es:[44]
	test ecx, ecx
	jz fatal_elf
	cmp ecx, 8
	ja fatal_elf
	cmp word ptr es:[30], 0
	jne fatal_elf
	movzx esi, word ptr es:[28]
	mov eax, ecx
	shl eax, 5
	add eax, esi
	jc fatal_elf
	cmp eax, dword ptr [file_size]
	ja fatal_elf
	cmp eax, 512
	ja fatal_elf
	mov eax, dword ptr es:[24]
	cmp eax, KERNEL_PHYSICAL_MIN
	jb fatal_elf
	cmp eax, KERNEL_PHYSICAL_LIMIT
	jae fatal_elf
	mov dword ptr [elf_entry_low], eax
	mov dword ptr [elf_entry_high], 0
	mov byte ptr [elf_class], ELFCLASS32
	mov byte ptr [elf_entry_executable], 0
	mov word ptr [ph_load_count], 0
	mov di, offset ph_table
elf32_ph_loop:
	cmp dword ptr es:[si], PT_LOAD
	jne elf32_ph_next
	cmp word ptr [ph_load_count], 8
	jae fatal_elf
	mov eax, dword ptr es:[si + 4]
	test eax, 511
	jnz fatal_elf
	mov dword ptr [di + 0], eax
	add eax, dword ptr es:[si + 16]
	jc fatal_elf
	cmp eax, dword ptr [file_size]
	ja fatal_elf
	mov eax, dword ptr es:[si + 12]
	cmp eax, KERNEL_PHYSICAL_MIN
	jb fatal_elf
	cmp eax, KERNEL_PHYSICAL_LIMIT
	jae fatal_elf
	mov dword ptr [di + 4], eax
	mov eax, dword ptr es:[si + 16]
	mov dword ptr [di + 8], eax
	mov edx, dword ptr es:[si + 20]
	cmp eax, edx
	ja fatal_elf
	mov dword ptr [di + 12], edx
	mov eax, dword ptr [di + 4]
	add eax, edx
	jc fatal_elf
	cmp eax, KERNEL_PHYSICAL_LIMIT
	ja fatal_elf
	mov eax, dword ptr es:[si + 8]
	mov dword ptr [di + 16], eax
	mov dword ptr [di + 20], 0
	mov eax, dword ptr es:[si + 24]
	mov dword ptr [di + 24], eax
	test eax, PF_X
	jz 1f
	mov eax, dword ptr [elf_entry_low]
	cmp eax, dword ptr [di + 4]
	jb 1f
	mov edx, dword ptr [di + 4]
	add edx, dword ptr [di + 12]
	cmp eax, edx
	jae 1f
	mov byte ptr [elf_entry_executable], 1
1:
	add di, PH_RECORD_SIZE
	inc word ptr [ph_load_count]
elf32_ph_next:
	add si, 32
	dec ecx
	jnz elf32_ph_loop
	cmp word ptr [ph_load_count], 0
	je fatal_elf
	cmp byte ptr [elf_entry_executable], 1
	jne fatal_elf
	mov si, offset msg_elf32
	call16 puts
	jmp load_all_segments

/* ------------------------------------------------------------------ */
/* ELF64/x86-64, deliberately restricted to a small bootstrap subset. */
parse_elf64:
	cmp dword ptr [file_size], 64
	jb fatal_elf
	cmp word ptr es:[18], EM_X86_64
	jne fatal_elf
	cmp word ptr es:[52], 64
	jne fatal_elf
	cmp word ptr es:[54], 56
	jne fatal_elf
	movzx ecx, word ptr es:[56]
	test ecx, ecx
	jz fatal_elf
	cmp ecx, 8
	ja fatal_elf
	cmp dword ptr es:[36], 0
	jne fatal_elf
	mov esi, dword ptr es:[32]
	mov eax, ecx
	imul eax, 56
	add eax, esi
	jc fatal_elf64
	cmp eax, dword ptr [file_size]
	ja fatal_elf64
	cmp eax, 512
	ja fatal_elf
	mov eax, dword ptr es:[24]
	mov edx, dword ptr es:[28]
	cmp edx, 0xffffffff
	jne fatal_elf64
	cmp eax, KERNEL_HIGH_WINDOW_MIN
	jb fatal_elf64
	cmp eax, KERNEL_HIGH_WINDOW_LIMIT
	jae fatal_elf64
	mov dword ptr [elf_entry_low], eax
	mov dword ptr [elf_entry_high], edx
	mov byte ptr [elf_class], ELFCLASS64
	mov byte ptr [elf_entry_executable], 0
	mov word ptr [ph_load_count], 0
	mov di, offset ph_table
elf64_ph_loop:
	cmp dword ptr es:[si], PT_LOAD
	jne elf64_ph_next
	cmp word ptr [ph_load_count], 8
	jae fatal_elf
	cmp dword ptr es:[si + 12], 0
	jne fatal_elf64
	mov eax, dword ptr es:[si + 8]
	test eax, 511
	jnz fatal_elf64
	mov dword ptr [di + 0], eax
	add eax, dword ptr es:[si + 32]
	jc fatal_elf64
	cmp eax, dword ptr [file_size]
	ja fatal_elf64
	cmp dword ptr es:[si + 28], 0
	jne fatal_elf64
	mov eax, dword ptr es:[si + 24]
	cmp eax, KERNEL_PHYSICAL_MIN
	jb fatal_elf64
	cmp eax, KERNEL_PHYSICAL_LIMIT
	jae fatal_elf64
	mov dword ptr [di + 4], eax
	cmp dword ptr es:[si + 36], 0
	jne fatal_elf64
	mov eax, dword ptr es:[si + 32]
	mov dword ptr [di + 8], eax
	cmp dword ptr es:[si + 44], 0
	jne fatal_elf64
	mov edx, dword ptr es:[si + 40]
	cmp eax, edx
	ja fatal_elf64
	mov dword ptr [di + 12], edx
	mov eax, dword ptr [di + 4]
	add eax, edx
	jc fatal_elf64
	cmp eax, KERNEL_PHYSICAL_LIMIT
	ja fatal_elf64
	mov eax, dword ptr es:[si + 16]
	mov edx, dword ptr es:[si + 20]
	cmp edx, 0xffffffff
	jne fatal_elf64
	cmp eax, KERNEL_HIGH_WINDOW_MIN
	jb fatal_elf64
	cmp eax, KERNEL_HIGH_WINDOW_LIMIT
	jae fatal_elf64
	mov ebx, eax
	sub ebx, KERNEL_HIGH_WINDOW_MIN
	cmp ebx, dword ptr [di + 4]
	jne fatal_elf64
	mov dword ptr [di + 16], eax
	mov dword ptr [di + 20], edx
	mov eax, dword ptr es:[si + 4]
	mov dword ptr [di + 24], eax
	test eax, PF_X
	jz 1f
	mov eax, dword ptr [elf_entry_low]
	cmp eax, dword ptr [di + 16]
	jb 1f
	mov edx, dword ptr [di + 16]
	add edx, dword ptr [di + 12]
	cmp eax, edx
	jae 1f
	mov byte ptr [elf_entry_executable], 1
1:
	add di, PH_RECORD_SIZE
	inc word ptr [ph_load_count]
elf64_ph_next:
	add si, 56
	dec ecx
	jnz elf64_ph_loop
	cmp word ptr [ph_load_count], 0
	je fatal_elf
	cmp byte ptr [elf_entry_executable], 1
	jne fatal_elf64
	mov si, offset msg_elf64
	call16 puts

/* ------------------------------------------------------------------ */
load_all_segments:
	call16 unreal_gs
	mov word ptr [ph_cursor], offset ph_table
	mov ax, word ptr [ph_load_count]
	mov word ptr [ph_remaining], ax
load_ph_loop:
	cmp word ptr [ph_remaining], 0
	je load_complete
	mov si, word ptr [ph_cursor]
	mov eax, dword ptr [si + 0]
	shr eax, 9
	mov dword ptr [load_file_sector], eax
	mov eax, dword ptr [si + 4]
	mov dword ptr [load_destination], eax
	mov eax, dword ptr [si + 8]
	mov dword ptr [load_remaining], eax
load_file_loop:
	mov eax, dword ptr [load_remaining]
	test eax, eax
	jz zero_bss
	mov eax, dword ptr [load_file_sector]
	call16 fat16_read_file_sector
	mov ecx, 512
	cmp dword ptr [load_remaining], ecx
	jae 1f
	mov ecx, dword ptr [load_remaining]
1:	mov dword ptr [copy_count], ecx
	cli
	xor bx, bx
	mov edi, dword ptr [load_destination]
copy_sector_loop:
	mov edx, ecx
	shr ecx, 2
	jz copy_sector_tail
copy_sector_dword:
	mov eax, dword ptr es:[bx]
	mov dword ptr gs:[edi], eax
	add bx, 4
	add edi, 4
	dec ecx
	jnz copy_sector_dword
copy_sector_tail:
	mov ecx, edx
	and ecx, 3
	jz copy_sector_done
copy_sector_byte:
	mov al, byte ptr es:[bx]
	mov byte ptr gs:[edi], al
	inc bx
	inc edi
	dec ecx
	jnz copy_sector_byte
copy_sector_done:
	sti
	mov eax, dword ptr [copy_count]
	add dword ptr [load_destination], eax
	sub dword ptr [load_remaining], eax
	inc dword ptr [load_file_sector]
	jmp load_file_loop
zero_bss:
	cli
	cmp byte ptr [elf_class], ELFCLASS64
	je next_ph
	mov si, word ptr [ph_cursor]
	mov ecx, dword ptr [si + 12]
	sub ecx, dword ptr [si + 8]
	mov edi, dword ptr [si + 4]
	add edi, dword ptr [si + 8]
	mov dword ptr [load_remaining], ecx
zero_bss_chunk:
	mov ecx, dword ptr [load_remaining]
	test ecx, ecx
	jz next_ph
	cmp ecx, 0xfffc
	jbe 1f
	mov ecx, 0xfffc
1:	mov dword ptr [copy_count], ecx
	mov edx, ecx
	shr ecx, 2
	xor eax, eax
	jz zero_bss_tail
zero_bss_dword:
	mov dword ptr gs:[edi], eax
	add edi, 4
	dec ecx
	jnz zero_bss_dword
zero_bss_tail:
	mov ecx, edx
	and ecx, 3
	jz zero_bss_chunk_done
zero_bss_byte:
	mov byte ptr gs:[edi], 0
	inc edi
	dec ecx
	jnz zero_bss_byte
zero_bss_chunk_done:
	mov eax, dword ptr [copy_count]
	sub dword ptr [load_remaining], eax
	jmp zero_bss_chunk
next_ph:
	sti
	add word ptr [ph_cursor], PH_RECORD_SIZE
	dec word ptr [ph_remaining]
	jmp load_ph_loop
load_complete:
	mov si, offset msg_loaded
	call16 puts
	cmp byte ptr [elf_class], ELFCLASS64
	je enter_long_mode
	jmp enter_protected_mode

/* ------------------------------------------------------------------ */
/* FAT12/16/32. */
fat16_mount:
	mov eax, dword ptr [partition_lba]
	call16 disk_read_one
	cmp word ptr es:[BPB_BYTES_PER_SECTOR], 512
	jne fatal_fat
	mov al, byte ptr es:[BPB_SECTORS_CLUSTER]
	test al, al
	jz fatal_fat
	mov ah, al
	dec ah
	test al, ah
	jnz fatal_fat
	mov byte ptr [fat_spc], al
	movzx eax, word ptr es:[BPB_TOTAL16]
	test eax, eax
	jnz 1f
	mov eax, dword ptr es:[BPB_TOTAL32]
1:	test eax, eax
	jz fatal_fat
	cmp eax, dword ptr [partition_blocks]
	ja fatal_fat
	mov dword ptr [fat_total_sectors], eax
	add eax, dword ptr [partition_lba]
	jc fatal_fat
	mov dword ptr [fat_volume_end_lba], eax
	movzx eax, word ptr es:[BPB_RESERVED_SECTORS]
	test eax, eax
	jz fatal_fat
	add eax, dword ptr [partition_lba]
	jc fatal_fat
	mov dword ptr [fat_lba], eax
	movzx eax, word ptr es:[BPB_SECTORS_FAT16]
	test eax, eax
	jz fat32_mount
	cmp byte ptr es:[BPB_EXT_SIGNATURE16], 0x29
	jne 1f
	mov edx, dword ptr es:[BPB_VOLUME_SERIAL16]
	mov dword ptr [boot_volume_serial], edx
	mov byte ptr [boot_uuid_valid], 1
1:
	mov dword ptr [fat_sectors], eax
	mov edx, dword ptr [fat_lba]
	add edx, eax
	jc fatal_fat
	cmp edx, dword ptr [fat_volume_end_lba]
	ja fatal_fat
	mov dword ptr [fat_end_lba], edx
	movzx ecx, byte ptr es:[BPB_FAT_COUNT]
	test ecx, ecx
	jz fatal_fat
	mul ecx
	test edx, edx
	jnz fatal_fat
	add eax, dword ptr [fat_lba]
	jc fatal_fat
	mov dword ptr [root_lba], eax
	movzx eax, word ptr es:[BPB_ROOT_ENTRIES]
	test eax, eax
	jz fatal_fat
	mov dword ptr [root_entries], eax
	shl eax, 5
	jc fatal_fat
	add eax, 511
	jc fatal_fat
	shr eax, 9
	mov dword ptr [root_sectors], eax
	add eax, dword ptr [root_lba]
	jc fatal_fat
	mov dword ptr [data_lba], eax
	/* FAT12 and FAT16 share the fixed-root BPB.  Classify by the Microsoft
	 * data-cluster thresholds instead of trusting the cosmetic type string. */
	mov eax, dword ptr [fat_total_sectors]
	mov edx, dword ptr [data_lba]
	sub edx, dword ptr [partition_lba]
	cmp eax, edx
	jbe fatal_fat
	sub eax, edx
	movzx ecx, byte ptr [fat_spc]
	xor edx, edx
	div ecx
	mov dword ptr [fat_cluster_count], eax
	add eax, 2
	jc fatal_fat
	mov dword ptr [fat_cluster_limit], eax
	mov byte ptr [fat_type], FAT_TYPE_16
	cmp dword ptr [fat_cluster_count], 4085
	jae 2f
	mov byte ptr [fat_type], FAT_TYPE_12
	cmp dword ptr [fat_cluster_limit], 0x0ff0
	jae fatal_fat
	jmp 3f
2:	cmp dword ptr [fat_cluster_limit], 0xfff0
	jae fatal_fat
3:
	ret16

fat32_mount:
	cmp byte ptr es:[BPB_EXT_SIGNATURE32], 0x29
	jne 1f
	mov edx, dword ptr es:[BPB_VOLUME_SERIAL32]
	mov dword ptr [boot_volume_serial], edx
	mov byte ptr [boot_uuid_valid], 1
1:
	mov eax, dword ptr es:[BPB_SECTORS_FAT32]
	test eax, eax
	jz fatal_fat
	mov dword ptr [fat_sectors], eax
	mov edx, dword ptr [fat_lba]
	add edx, eax
	jc fatal_fat
	cmp edx, dword ptr [fat_volume_end_lba]
	ja fatal_fat
	mov dword ptr [fat_end_lba], edx
	movzx ecx, byte ptr es:[BPB_FAT_COUNT]
	test ecx, ecx
	jz fatal_fat
	mul ecx
	test edx, edx
	jnz fatal_fat
	add eax, dword ptr [fat_lba]
	jc fatal_fat
	mov dword ptr [data_lba], eax
	cmp eax, dword ptr [fat_volume_end_lba]
	jae fatal_fat
	mov eax, dword ptr es:[BPB_ROOT_CLUSTER32]
	and eax, 0x0fffffff
	cmp eax, 2
	jb fatal_fat
	mov dword ptr [root_cluster32], eax
	mov dword ptr [root_entries], 0
	mov byte ptr [fat_type], FAT_TYPE_32
	mov eax, dword ptr [data_lba]
	sub eax, dword ptr [partition_lba]
	mov edx, dword ptr [fat_total_sectors]
	cmp edx, eax
	jbe fatal_fat
	sub edx, eax
	mov eax, edx
	movzx ecx, byte ptr [fat_spc]
	xor edx, edx
	div ecx
	test eax, eax
	jz fatal_fat
	mov dword ptr [fat_cluster_count], eax
	add eax, 2
	jc fatal_fat
	mov dword ptr [fat_cluster_limit], eax
	cmp eax, 0x0ffffff0
	jae fatal_fat
	mov edx, dword ptr [fat_sectors]
	cmp edx, 0x01ffffff
	ja fatal_fat
	shl edx, 7
	cmp edx, eax
	jb fatal_fat
	mov eax, dword ptr [root_cluster32]
	cmp eax, dword ptr [fat_cluster_limit]
	jae fatal_fat
	ret16

/*
 * Resolve a bounded, NUL-terminated selected-volume path.  The p003 parser
 * has already rejected empty, dot, dot-dot, device, and volume components.
 * This resolver still enforces its own component and directory bounds so a
 * malformed filesystem cannot widen that contract.
 */
fat_find_path:
	mov byte ptr [file_cache_valid], 0
	mov byte ptr [fat_cache_valid], 0
	mov word ptr [path_cursor], si
	mov byte ptr [directory_is_root], 1
fat_path_component:
	mov si, word ptr [path_cursor]
	mov word ptr [path_component], si
	xor cx, cx
1:	mov al, byte ptr [si]
	test al, al
	jz 2f
	cmp al, '/'
	je 2f
	inc si
	inc cx
	cmp cx, 255
	ja fatal_path
	jmp 1b
2:	test cx, cx
	jz fatal_path
	mov word ptr [path_component_length], cx
	mov al, byte ptr [si]
	mov byte ptr [path_component_final], 1
	test al, al
	jz 3f
	mov byte ptr [path_component_final], 0
	inc si
3:	mov word ptr [path_cursor], si
	call16 fat_find_directory_component
	mov al, byte ptr [found_attribute]
	cmp byte ptr [path_component_final], 0
	je fat_path_directory
	test al, FAT_ATTRIBUTE_DIRECTORY
	jnz fatal_path
	mov eax, dword ptr [found_cluster]
	cmp eax, 2
	jb fatal_path
	cmp eax, dword ptr [fat_cluster_limit]
	jae fatal_path
	mov word ptr [file_first_cluster], ax
	mov dword ptr [file_first_cluster32], eax
	mov eax, dword ptr [found_size]
	test eax, eax
	jz fatal_path
	mov dword ptr [file_size], eax
	mov byte ptr [file_cache_valid], 0
	mov byte ptr [fat_cache_valid], 0
	ret16
fat_path_directory:
	test al, FAT_ATTRIBUTE_DIRECTORY
	jz fatal_path
	mov eax, dword ptr [found_cluster]
	cmp eax, 2
	jb fatal_path
	cmp eax, dword ptr [fat_cluster_limit]
	jae fatal_path
	mov dword ptr [directory_cluster], eax
	mov byte ptr [directory_is_root], 0
	jmp fat_path_component

fat_find_directory_component:
	push es
	mov ax, ds
	mov es, ax
	mov di, offset directory_state
	xor eax, eax
	mov cx, ZBL_BIOS_FAT_DIRECTORY_STATE_SIZE / 4
	rep stosd
	pop es
	cmp byte ptr [directory_is_root], 0
	je fat_scan_cluster_directory
	cmp byte ptr [fat_type], FAT_TYPE_32
	je fat_scan_root32
	mov eax, dword ptr [root_lba]
	mov dword ptr [scan_lba], eax
	mov eax, dword ptr [root_sectors]
	mov dword ptr [scan_sectors], eax
fat_scan_fixed_sector:
	cmp dword ptr [scan_sectors], 0
	je fatal_path
	mov eax, dword ptr [scan_lba]
	call16 fat_scan_one_sector
	cmp eax, -2
	je fatal_path
	cmp eax, -1
	jne fat_scan_found
	inc dword ptr [scan_lba]
	dec dword ptr [scan_sectors]
	jmp fat_scan_fixed_sector
fat_scan_root32:
	mov eax, dword ptr [root_cluster32]
	mov dword ptr [directory_cluster], eax
fat_scan_cluster_directory:
	mov eax, dword ptr [fat_cluster_count]
	mov dword ptr [scan_clusters], eax
fat_scan_cluster:
	cmp dword ptr [scan_clusters], 0
	je fatal_path
	dec dword ptr [scan_clusters]
	mov eax, dword ptr [directory_cluster]
	cmp eax, 2
	jb fatal_path
	cmp eax, dword ptr [fat_cluster_limit]
	jae fatal_path
	sub eax, 2
	movzx ecx, byte ptr [fat_spc]
	mul ecx
	test edx, edx
	jnz fatal_path
	add eax, dword ptr [data_lba]
	jc fatal_path
	mov dword ptr [scan_lba], eax
	movzx eax, byte ptr [fat_spc]
	mov dword ptr [scan_sectors], eax
	add eax, dword ptr [scan_lba]
	jc fatal_path
	cmp eax, dword ptr [fat_volume_end_lba]
	ja fatal_path
fat_scan_cluster_sector:
	cmp dword ptr [scan_sectors], 0
	je fat_scan_next_cluster
	mov eax, dword ptr [scan_lba]
	call16 fat_scan_one_sector
	cmp eax, -2
	je fatal_path
	cmp eax, -1
	jne fat_scan_found
	inc dword ptr [scan_lba]
	dec dword ptr [scan_sectors]
	jmp fat_scan_cluster_sector
fat_scan_next_cluster:
	mov eax, dword ptr [directory_cluster]
	cmp byte ptr [fat_type], FAT_TYPE_32
	je 1f
	call16 fat16_next_cluster
	movzx eax, ax
	cmp eax, 0xfff8
	jae fatal_path
	jmp 2f
1:	call16 fat32_next_cluster
	cmp eax, 0x0ffffff8
	jae fatal_path
2:	cmp eax, 2
	jb fatal_path
	cmp eax, dword ptr [fat_cluster_limit]
	jae fatal_path
	mov dword ptr [directory_cluster], eax
	jmp fat_scan_cluster

/* Return entry offset, -1 for no match, or -2 for directory end. */
fat_scan_one_sector:
	call16 fat_read_one
	xor bx, bx
	mov di, offset directory_sector
	mov cx, 512 / 4
1:	mov eax, dword ptr es:[bx]
	mov dword ptr [di], eax
	add bx, 4
	add di, 4
	loop 1b
	.code16gcc
	movzx eax, word ptr [path_component_length]
	push eax
	movzx eax, word ptr [path_component]
	push eax
	/* LLVM IAS does not apply .code16gcc to immediate pushes. */
	.byte 0x66, 0x68
	.long 512
	mov eax, offset directory_sector
	push eax
	mov eax, offset directory_state
	push eax
	call zbl_bios_fat_directory_search
	add esp, 20
	.code16
	ret16

fat_scan_found:
	mov bx, ax
	add bx, offset directory_sector
	mov al, byte ptr [bx + 11]
	mov byte ptr [found_attribute], al
	movzx eax, word ptr [bx + 26]
	cmp byte ptr [fat_type], FAT_TYPE_32
	jne 1f
	movzx edx, word ptr [bx + 20]
	shl edx, 16
	or eax, edx
	and eax, 0x0fffffff
1:	mov dword ptr [found_cluster], eax
	mov eax, dword ptr [bx + 28]
	mov dword ptr [found_size], eax
	ret16

load_zedbsd_configuration:
	cmp byte ptr [boot_uuid_valid], 0
	je fatal_config
	call16 format_boot_uuid
	mov byte ptr [path_role], 0
	mov si, offset zedbsd_config_path
	call16 fat_find_path
	mov eax, dword ptr [file_size]
	cmp eax, ZBL_ZEDBSD_CONFIG_FILE_MAX
	ja fatal_config
	mov dword ptr [config_size], eax
	mov dword ptr [config_read_sector], 0
	mov word ptr [config_copy_offset], 0
1:	movzx edx, word ptr [config_copy_offset]
	cmp edx, dword ptr [config_size]
	jae 3f
	mov eax, dword ptr [config_read_sector]
	call16 fat16_read_file_sector
	xor bx, bx
	mov di, word ptr [config_copy_offset]
	add di, offset config_source
	mov cx, 512 / 4
2:	mov eax, dword ptr es:[bx]
	mov dword ptr [di], eax
	add bx, 4
	add di, 4
	loop 2b
	inc dword ptr [config_read_sector]
	add word ptr [config_copy_offset], 512
	jmp 1b
3:
	.code16gcc
	/* LLVM IAS does not apply .code16gcc to immediate pushes. */
	.byte 0x66, 0x68
	.long 10
	mov eax, offset boot_uuid_text
	push eax
	push dword ptr [config_size]
	mov eax, offset config_source
	push eax
	mov eax, offset bios_configuration
	push eax
	call zbl_uefi_zedbsd_config_parse
	add esp, 20
	.code16
	test eax, eax
	jnz fatal_config
	push es
	mov ax, ds
	mov es, ax
	mov si, offset bios_configuration + \
		ZBL_ZEDBSD_CONFIG_PARAMETER_RECORD_OFFSET
	mov di, offset long_handoff + ZBL6_HANDOFF_V5_BIOS_PARAMETERS_OFFSET
	mov cx, ZEDBSD_BOOT_PARAMETER_RECORD_SIZE / 4
	rep movsd
	pop es
	mov si, offset msg_config
	call16 puts
	mov si, offset boot_uuid_text
	call16 puts
	mov si, offset msg_config_kernel
	call16 puts
	mov si, offset bios_configuration
	call16 puts
	mov si, offset msg_config_parameters
	call16 puts
	mov si, offset bios_configuration + \
		ZBL_ZEDBSD_CONFIG_PARAMETER_RECORD_OFFSET + \
	    ZEDBSD_BOOT_PARAMETER_RECORD_TEXT_OFFSET
	call16 puts
	mov al, '\n'
	call16 putc
	ret16

format_boot_uuid:
	mov ebx, dword ptr [boot_volume_serial]
	mov di, offset boot_uuid_text
	mov cx, 8
1:	rol ebx, 4
	mov al, bl
	and al, 0x0f
	cmp al, 10
	jb 2f
	add al, 'A' - 10
	jmp 3f
2:	add al, '0'
3:	mov byte ptr [di], al
	inc di
	cmp cx, 5
	jne 4f
	mov byte ptr [di], '-'
	inc di
4:	loop 1b
	mov byte ptr [di], 0
	ret16

/* EAX=file-relative sector; returns it in ES:0000. */
fat16_read_file_sector:
	mov edx, dword ptr [file_size]
	add edx, 511
	jc fatal_fat_chain
	shr edx, 9
	cmp eax, edx
	jae fatal_fat_chain
	cmp byte ptr [fat_type], FAT_TYPE_32
	je fat32_read_file_sector
	mov dword ptr [desired_sector], eax
fat_file_next_request:
	cmp byte ptr [file_cache_valid], 0
	je fat_file_request_selected
	mov edx, dword ptr [cached_file_sector]
	inc edx
	cmp eax, edx
	je fat_file_request_selected
	jb fat_file_request_selected
	/* Keep BIOS disk access sequential even across gaps between PT_LOADs. */
	mov eax, edx
fat_file_request_selected:
	mov dword ptr [requested_sector], eax
	cmp byte ptr [file_cache_valid], 0
	je fat_file_walk_from_start
	mov edx, dword ptr [cached_file_sector]
	inc edx
	cmp eax, edx
	jne fat_file_walk_from_start
	mov ax, word ptr [cached_cluster]
	mov dl, byte ptr [cached_sector_in_cluster]
	inc dl
	cmp dl, byte ptr [fat_spc]
	jb fat_file_cache_ready
	xor dl, dl
	call16 fat16_next_cluster
	movzx eax, ax
	cmp eax, dword ptr [fat_cluster_limit]
	jae fatal_fat_chain
	xor dl, dl
fat_file_cache_ready:
	mov word ptr [cached_cluster], ax
	mov byte ptr [cached_sector_in_cluster], dl
	mov eax, dword ptr [requested_sector]
	mov dword ptr [cached_file_sector], eax
	jmp fat_cluster_cached
fat_file_walk_from_start:
	mov eax, dword ptr [requested_sector]
	movzx ecx, byte ptr [fat_spc]
	xor edx, edx
	div ecx
	mov dword ptr [cluster_steps], eax
	cmp eax, dword ptr [fat_cluster_count]
	jae fatal_fat_chain
	mov dword ptr [sector_in_cluster], edx
	mov ax, word ptr [file_first_cluster]
fat_cluster_walk:
	cmp dword ptr [cluster_steps], 0
	je fat_cluster_found
	call16 fat16_next_cluster
	movzx edx, ax
	cmp edx, dword ptr [fat_cluster_limit]
	jae fatal_fat_chain
	dec dword ptr [cluster_steps]
	jmp fat_cluster_walk
fat_cluster_found:
	cmp ax, 2
	jb fatal_fat_chain
	cmp ax, 0xfff8
	jae fatal_fat_chain
	mov word ptr [cached_cluster], ax
	mov dl, byte ptr [sector_in_cluster]
	mov byte ptr [cached_sector_in_cluster], dl
	mov eax, dword ptr [requested_sector]
	mov dword ptr [cached_file_sector], eax
	mov byte ptr [file_cache_valid], 1
fat_cluster_cached:
	mov ax, word ptr [cached_cluster]
	movzx eax, ax
	sub eax, 2
	movzx ecx, byte ptr [fat_spc]
	mul ecx
	add eax, dword ptr [data_lba]
	jc fatal_fat_chain
	movzx edx, byte ptr [cached_sector_in_cluster]
	add eax, edx
	jc fatal_fat_chain
	cmp eax, dword ptr [fat_volume_end_lba]
	jae fatal_fat_chain
	call16 fat_read_one
	mov eax, dword ptr [requested_sector]
	cmp eax, dword ptr [desired_sector]
	jae fat_file_read_done
	inc eax
	jmp fat_file_next_request
fat_file_read_done:
	ret16

fat32_read_file_sector:
	mov dword ptr [desired_sector], eax
	cmp byte ptr [file_cache_valid], 0
	je fat32_file_from_start
	mov edx, dword ptr [cached_file_sector]
	inc edx
	cmp eax, edx
	jne fat32_file_from_start
	mov eax, dword ptr [cached_cluster32]
	mov bl, byte ptr [cached_sector_in_cluster]
	inc bl
	cmp bl, byte ptr [fat_spc]
	jb fat32_file_cache_ready
	xor bl, bl
	call16 fat32_next_cluster
	cmp eax, 0x0ffffff8
	jae fatal_fat_chain
	cmp eax, 2
	jb fatal_fat_chain
	cmp eax, dword ptr [fat_cluster_limit]
	jae fatal_fat_chain
fat32_file_cache_ready:
	mov dword ptr [cached_cluster32], eax
	mov byte ptr [cached_sector_in_cluster], bl
	movzx edx, bl
	mov dword ptr [sector_in_cluster], edx
	mov edx, dword ptr [desired_sector]
	mov dword ptr [cached_file_sector], edx
	jmp fat32_file_cluster_cached
fat32_file_from_start:
	mov eax, dword ptr [desired_sector]
	movzx ecx, byte ptr [fat_spc]
	xor edx, edx
	div ecx
	mov dword ptr [cluster_steps], eax
	cmp eax, dword ptr [fat_cluster_count]
	jae fatal_fat_chain
	mov dword ptr [sector_in_cluster], edx
	mov eax, dword ptr [file_first_cluster32]
fat32_file_walk:
	cmp dword ptr [cluster_steps], 0
	je fat32_file_cluster_ready
	call16 fat32_next_cluster
	cmp eax, 0x0ffffff8
	jae fatal_fat_chain
	cmp eax, 2
	jb fatal_fat_chain
	cmp eax, dword ptr [fat_cluster_limit]
	jae fatal_fat_chain
	dec dword ptr [cluster_steps]
	jmp fat32_file_walk
fat32_file_cluster_ready:
	cmp eax, 2
	jb fatal_fat_chain
	cmp eax, dword ptr [fat_cluster_limit]
	jae fatal_fat_chain
	mov dword ptr [cached_cluster32], eax
	mov edx, dword ptr [sector_in_cluster]
	mov byte ptr [cached_sector_in_cluster], dl
	mov edx, dword ptr [desired_sector]
	mov dword ptr [cached_file_sector], edx
	mov byte ptr [file_cache_valid], 1
fat32_file_cluster_cached:
	sub eax, 2
	movzx ecx, byte ptr [fat_spc]
	mul ecx
	test edx, edx
	jnz fatal_fat_chain
	add eax, dword ptr [data_lba]
	jc fatal_fat_chain
	add eax, dword ptr [sector_in_cluster]
	jc fatal_fat_chain
	cmp eax, dword ptr [fat_volume_end_lba]
	jae fatal_fat_chain
	call16 fat_read_one
	mov eax, dword ptr [desired_sector]
	ret16

fat32_next_cluster:
	push ebx
	push edx
	cmp eax, 2
	jb fatal_fat_chain
	cmp eax, dword ptr [fat_cluster_limit]
	jae fatal_fat_chain
	mov edx, eax
	shl eax, 2
	mov ebx, eax
	and ebx, 511
	mov word ptr [fat_offset32], bx
	shr eax, 9
	add eax, dword ptr [fat_lba]
	jc fatal_fat_chain
	cmp eax, dword ptr [fat_end_lba]
	jae fatal_fat_chain
	call16 fat_read_one
	movzx ebx, word ptr [fat_offset32]
	mov eax, dword ptr es:[bx]
	and eax, 0x0fffffff
	pop edx
	pop ebx
	ret16

/* AX=current cluster, AX=next cluster. */
fat16_next_cluster:
	mov byte ptr [fat_cluster_odd], al
	and byte ptr [fat_cluster_odd], 1
	movzx eax, ax
	cmp eax, 2
	jb fatal_fat_chain
	cmp eax, dword ptr [fat_cluster_limit]
	jae fatal_fat_chain
	cmp byte ptr [fat_type], FAT_TYPE_12
	je fat12_next_cluster
	shl eax, 1
	jmp fat_entry_offset_ready
fat12_next_cluster:
	mov edx, eax
	shr edx, 1
	add eax, edx
fat_entry_offset_ready:
	mov dx, ax
	and dx, 511
	mov word ptr [fat_offset], dx
	shr eax, 9
	add eax, dword ptr [fat_lba]
	jc fatal_fat_chain
	cmp eax, dword ptr [fat_end_lba]
	jae fatal_fat_chain
	cmp byte ptr [fat_cache_valid], 0
	je fat_cache_fill
	cmp eax, dword ptr [fat_cache_lba]
	je fat_cache_ready
fat_cache_fill:
	mov dword ptr [fat_cache_lba], eax
	call16 fat_read_one
	xor bx, bx
	mov di, offset fat_cache
	mov ecx, 512 / 4
fat_cache_copy:
	mov eax, dword ptr es:[bx]
	mov dword ptr [di], eax
	add bx, 4
	add di, 4
	dec ecx
	jnz fat_cache_copy
	cmp byte ptr [fat_type], FAT_TYPE_12
	jne fat_cache_mark_valid
	mov eax, dword ptr [fat_cache_lba]
	inc eax
	cmp eax, dword ptr [fat_end_lba]
	jae fatal_fat_chain
	call16 fat_read_one
	xor bx, bx
	mov di, offset fat_cache + 512
	mov ecx, 512 / 4
fat_cache_copy_second:
	mov eax, dword ptr es:[bx]
	mov dword ptr [di], eax
	add bx, 4
	add di, 4
	dec ecx
	jnz fat_cache_copy_second
fat_cache_mark_valid:
	mov byte ptr [fat_cache_valid], 1
fat_cache_ready:
	movzx ebx, word ptr [fat_offset]
	mov ax, word ptr [bx + fat_cache]
	cmp byte ptr [fat_type], FAT_TYPE_12
	jne fat_next_value_ready
	cmp byte ptr [fat_cluster_odd], 0
	jz 1f
	shr ax, 4
	jmp 2f
1:	and ax, 0x0fff
2:	cmp ax, 0x0ff8
	jb fat_next_value_ready
	mov ax, 0xfff8
fat_next_value_ready:
	cmp ax, 2
	jb fatal_fat_chain
	ret16

/* ------------------------------------------------------------------ */
fat_read_one:
	cmp eax, dword ptr [partition_lba]
	jb fatal_fat_chain
	cmp eax, dword ptr [fat_volume_end_lba]
	jae fatal_fat_chain
	jmp disk_read_one

disk_read_one:
	mov dword ptr [dap_lba], eax
	mov dword ptr [dap_lba + 4], 0
	mov word ptr [dap_offset], 0
	mov word ptr [dap_segment], ZBL_BOUNCE_SEG
	mov byte ptr [disk_retry], 3
1:	mov si, offset dap
	mov ah, 0x42
	mov dl, byte ptr [boot_drive]
	int 0x13
	jnc 2f
	xor ah, ah
	int 0x13
	dec byte ptr [disk_retry]
	jnz 1b
	jmp fatal_disk
2:	mov ax, ZBL_BOUNCE_SEG
	mov es, ax
	ret16

/* Preserve the BIOS 8x16 ASCII font before entering protected mode.  The
 * kernel copies this low-memory handoff during its earliest BSP setup. */
.set VGA_FONT_HANDOFF, 0x7000
.set VGA_FONT_MAGIC, 0x3854465a        /* "ZFT8" */
sample_vga_font:
	push ax
	push bx
	push cx
	push dx
	push si
	push di
	push bp
	push ds
	push es
	xor ax, ax
	mov es, ax
	mov dword ptr es:[VGA_FONT_HANDOFF], 0
	mov ax, 0x1130
	mov bh, 0x06                    /* ROM 8x16 font */
	int 0x10
	cmp cx, 16
	jne 1f
	mov ax, es
	mov ds, ax
	mov si, bp
	xor ax, ax
	mov es, ax
	mov di, VGA_FONT_HANDOFF + 16
	mov cx, 128 * 16
	rep movsb
	mov dword ptr es:[VGA_FONT_HANDOFF], VGA_FONT_MAGIC
	mov word ptr es:[VGA_FONT_HANDOFF + 4], 1
	mov word ptr es:[VGA_FONT_HANDOFF + 6], 16
	mov word ptr es:[VGA_FONT_HANDOFF + 8], 128
1:	pop es
	pop ds
	pop bp
	pop di
	pop si
	pop dx
	pop cx
	pop bx
	pop ax
	ret16

#include "bootloader/pcat/vbe.inc"

/* Give GS a flat hidden descriptor while returning to real mode. */
unreal_gs:
	cli
	lgdt [gdt_pointer]
	mov eax, cr0
	or eax, 1
	mov cr0, eax
	mov ax, 0x10
	mov gs, ax
	mov eax, cr0
	and eax, 0xfffffffe
	mov cr0, eax
	sti
	ret16

/* ------------------------------------------------------------------ */
enter_protected_mode:
	mov dword ptr [mbi_flags], 7
	mov dword ptr [mbi_mem_lower], 640
	movzx eax, byte ptr [boot_drive]
	shl eax, 24
	movzx edx, byte ptr [partition_index]
	dec edx
	shl edx, 16
	or eax, edx
	or eax, 0x0000ffff
	mov dword ptr [mbi_boot_device], eax
	mov si, offset msg_go
	call16 puts
	cli
	lgdt [gdt_pointer]
	mov eax, cr0
	or eax, 1
	mov cr0, eax
	.byte 0x66, 0xea
	.long 0x10000 + protected_entry
	.word 0x08

/* Build a one-GiB identity map and the usual ffffffff80000000 window. */
enter_long_mode:
	mov eax, 0x80000000
	cpuid
	cmp eax, 0x80000001
	jb fatal_elf64
	mov eax, 0x80000001
	cpuid
	and edx, 0x20100000
	cmp edx, 0x20100000
	jne fatal_elf64
	call16 vbe_setup_framebuffer
	mov si, offset msg_go
	call16 puts
	cmp byte ptr [boot_uuid_valid], 0
	je 1f
	or word ptr [long_handoff + ZBL6_HANDOFF_FLAGS_OFFSET], 0x0010
	mov eax, dword ptr [boot_volume_serial]
	mov dword ptr [long_handoff + ZBL6_HANDOFF_BOOT_SERIAL_OFFSET], eax
1:
	mov al, byte ptr [boot_drive]
	mov byte ptr [long_handoff + ZBL6_HANDOFF_BOOT_DRIVE_OFFSET], al
	mov al, byte ptr [partition_index]
	mov byte ptr [long_handoff + ZBL6_HANDOFF_PARTITION_OFFSET], al
	mov eax, dword ptr [mbi_mem_lower]
	mov dword ptr [long_handoff + ZBL6_HANDOFF_MEM_LOWER_OFFSET], eax
	mov eax, dword ptr [mbi_mem_upper]
	mov dword ptr [long_handoff + ZBL6_HANDOFF_MEM_UPPER_OFFSET], eax
	call16 unreal_gs
	mov edi, 0x00050000
	mov ecx, 0x6000 / 4
	xor eax, eax
1:	mov dword ptr gs:[edi], eax
	add edi, 4
	loop 1b
	mov edi, 0x00050000
	mov dword ptr gs:[edi], 0x00051003
	mov dword ptr gs:[edi + 4], 0
	mov edi, 0x00050000 + 511 * 8
	mov dword ptr gs:[edi], 0x00053003
	mov dword ptr gs:[edi + 4], 0
	mov edi, 0x00051000
	mov dword ptr gs:[edi], 0x00052003
	mov edi, 0x00053000 + 510 * 8
	mov dword ptr gs:[edi], 0x00054003
	mov edi, 0x00052000
	mov ebx, 0x00000083
	mov ecx, 512
2:	mov dword ptr gs:[edi], ebx
	mov dword ptr gs:[edi + 4], 0
	add ebx, 0x00200000
	add edi, 8
	loop 2b
	mov edi, 0x00054000
	mov ebx, 0x00000083
	mov ecx, 512
3:	mov dword ptr gs:[edi], ebx
	mov dword ptr gs:[edi + 4], 0
	add ebx, 0x00200000
	add edi, 8
	loop 3b
	call16 vbe_map_framebuffer
	cli
	mov eax, cr4
	or eax, 0x20
	mov cr4, eax
	mov ecx, 0xc0000080
	rdmsr
	or eax, 0x100
	wrmsr
	mov eax, 0x00050000
	mov cr3, eax
	lgdt [gdt_pointer]
	mov eax, cr0
	or eax, 0x80000001
	mov cr0, eax
	.byte 0x66, 0xea
	.long 0x10000 + long_entry
	.word 0x18

.code32
protected_entry:
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax
	mov ss, ax
	mov esp, 0x0009f000
	cld
	mov eax, 0x2badb002
	mov ebx, offset mbi
	add ebx, 0x10000
	jmp dword ptr [0x10000 + elf_entry_low]

.code64
long_entry:
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov rsp, 0x000000000009f000
	xor rbp, rbp
	cld
	/* Clear every ELF64 p_memsz - p_filesz range through the native
	 * identity map.  Do not depend on unreal-mode segment cache state. */
	mov ebx, offset ph_load_count
	add ebx, 0x10000
	movzx r8d, word ptr [rbx]
	mov ebx, offset ph_table
	add ebx, 0x10000
.Lzero64_next:
	test r8d, r8d
	jz .Lzero64_done
	mov ecx, dword ptr [rbx + 12]
	sub ecx, dword ptr [rbx + 8]
	mov edi, dword ptr [rbx + 4]
	add edi, dword ptr [rbx + 8]
	xor eax, eax
	rep stosb
	add ebx, PH_RECORD_SIZE
	dec r8d
	jmp .Lzero64_next
.Lzero64_done:
	mov edi, offset long_handoff
	add edi, 0x10000
	mov ebx, offset elf_entry_low
	add ebx, 0x10000
	mov eax, dword ptr [rbx]
	mov edx, dword ptr [rbx + 4]
	shl rdx, 32
	or rax, rdx
	jmp rax

.code16
puts:
	lodsb
	test al, al
	jz 1f
	call16 putc
	jmp puts
1:	ret16
putc:
	push ax
	push dx
	mov dx, 0x00e9
	out dx, al
	pop dx
	pop ax
	ret16

fatal_mbr:       mov al, 'M'; jmp fatal
fatal_partition: mov al, 'P'; jmp fatal
fatal_fat:       mov al, 'F'; jmp fatal
fatal_fat_chain: mov al, 'K'; jmp fatal
fatal_path:
	cmp byte ptr [path_role], 0
	je fatal_config
	mov al, 'V'
	jmp fatal
fatal_config:    mov al, 'C'; jmp fatal
fatal_elf:       mov al, 'E'; jmp fatal
fatal_elf64:     mov al, '6'; jmp fatal
fatal_disk:      mov al, 'D'
fatal:
	push ax
	mov si, offset msg_error
	call16 puts
	pop ax
	call16 putc
	mov al, '\n'
	call16 putc
	cli
1:	hlt
	jmp 1b

msg_start:     .asciz "S4 "
msg_mbr:       .asciz "MB "
msg_partition: .asciz "P1 "
msg_kernel:    .asciz "KF "
msg_config:    .asciz "CF UUID="
msg_config_kernel: .asciz " KERNEL="
msg_config_parameters: .asciz " PARAMS="
msg_elf32:     .asciz "E3 "
msg_elf64:     .asciz "E6 "
msg_loaded:    .asciz "LD "
msg_go:        .asciz "GO\n"
msg_error:     .asciz "ERR "
zedbsd_config_path: .asciz "zedbsd.cfg"
boot_uuid_text: .space 10

.balign 8
gdt:
	.quad 0
	.quad 0x00cf9a000000ffff
	.quad 0x00cf92000000ffff
	.quad 0x00af9a000000ffff
gdt_end:
gdt_pointer:
	.word gdt_end - gdt - 1
	.long 0x10000 + gdt

.balign 4
dap:
	.byte 0x10, 0
	.word 1
dap_offset:  .word 0
dap_segment: .word 0
dap_lba:     .quad 0

ph_table: .space PH_RECORD_SIZE * 8

boot_drive:       .byte 0
partition_index:  .byte 0
elf_class:        .byte 0
elf_entry_executable: .byte 0
fat_spc:          .byte 0
fat_type:         .byte FAT_TYPE_16
fat_cluster_odd:  .byte 0
dir_sector:       .byte 0
disk_retry:       .byte 0
file_cache_valid: .byte 0
fat_cache_valid:  .byte 0
boot_uuid_valid:  .byte 0
path_role:        .byte 0
directory_is_root: .byte 0
path_component_final: .byte 0
found_attribute:  .byte 0
.balign 2
cached_cluster:     .word 0
cached_sector_in_cluster: .byte 0
.balign 2
file_first_cluster: .word 0
fat_offset:         .word 0
fat_offset32:       .word 0
ph_load_count:      .word 0
ph_cursor:          .word 0
ph_remaining:       .word 0
path_cursor:        .word 0
path_component:     .word 0
path_component_length: .word 0
config_copy_offset: .word 0
.balign 4
partition_lba:      .long 0
partition_end_lba:  .long 0
root_cluster32:     .long 0
dir_cluster32:      .long 0
file_first_cluster32: .long 0
cached_cluster32:   .long 0
partition_blocks:   .long 0
fat_total_sectors:  .long 0
fat_volume_end_lba: .long 0
boot_volume_serial: .long 0
fat_lba:            .long 0
fat_end_lba:        .long 0
fat_sectors:        .long 0
fat_cluster_count:  .long 0
fat_cluster_limit:  .long 0
root_lba:           .long 0
root_sectors:       .long 0
root_entries:       .long 0
data_lba:           .long 0
scan_lba:           .long 0
scan_entries:       .long 0
scan_sectors:       .long 0
scan_clusters:      .long 0
directory_cluster:  .long 0
found_cluster:      .long 0
found_size:         .long 0
file_size:          .long 0
config_size:        .long 0
config_read_sector: .long 0
requested_sector:   .long 0
desired_sector:     .long 0
cached_file_sector:  .long 0
fat_cache_lba:       .long 0
cluster_steps:      .long 0
sector_in_cluster:  .long 0
elf_entry_low:      .long 0
elf_entry_high:     .long 0
load_file_sector:   .long 0
load_destination:   .long 0
load_remaining:     .long 0
copy_count:         .long 0

.balign 8
mbi:
mbi_flags:       .long 7
mbi_mem_lower:   .long 640
mbi_mem_upper:   .long 0
mbi_boot_device: .long 0
mbi_cmdline:
	.long 0x10000 + long_handoff + \
	    ZBL6_HANDOFF_V5_BIOS_PARAMETERS_OFFSET + \
	    ZEDBSD_BOOT_PARAMETER_RECORD_TEXT_OFFSET

long_handoff:
	.long ZBL6_HANDOFF_MAGIC
	.word ZBL6_HANDOFF_V5_VERSION
	.word ZBL6_HANDOFF_V5_BIOS_SIZE
	.byte 0                         /* boot drive, populated at runtime */
	.byte 0                         /* partition index, populated at runtime */
	.word ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS
	.long 0                         /* conventional memory, KiB */
	.long 0                         /* extended memory, KiB */
	.long 0
	.space ZBL6_HANDOFF_FB_SIZE - ZBL6_HANDOFF_SIZE
	.space ZEDBSD_BOOT_PARAMETER_RECORD_SIZE

fat_cache: .space 1024
directory_state: .space ZBL_BIOS_FAT_DIRECTORY_STATE_SIZE
directory_sector: .space 512
bios_configuration:
	.space ZBL_ZEDBSD_CONFIG_RESULT_SIZE
config_source: .space ZBL_ZEDBSD_CONFIG_FILE_MAX
