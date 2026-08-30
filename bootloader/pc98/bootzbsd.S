/* PC-98 ZBL2 Stage 2: PC/AT MBR/FAT16 and ELF32 loader. */
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
#include "include/boot/pc98-handoff.h"
#include "bootloader/bios/fat-directory.h"
#include "bootloader/uefi/zedbsd-config.h"

.set PH_RECORD_SIZE, 28

stage2_header:
	.long ZBL2_MAGIC
	.word ZBL2_VERSION
	.word ZBL2_HEADER_SIZE
	.long stage2_end
	.word 0                         /* finalized sector count */
	.word ZBL2_MACHINE_PC98
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
	/* DOS MZ entry: preserve the current drive and relocate the loader to
	 * the physical address used by its protected-mode handoff tables. */
	mov ds, ax
	sti
	mov ah, 0x19
	int 0x21
	add al, 0x80                  /* NEC DOS fixed disk A:=00 -> BIOS 80h. */
	cli
	mov dx, cs
	mov ds, dx
	push ds
	xor dx, dx
	mov ds, dx
	mov dword ptr [0x0700], 0x3150395a
	mov byte ptr [0x0704], al
	mov byte ptr [0x0705], 8
	mov byte ptr [0x0706], 17
	mov dword ptr [0x0714], 0x31534f44 /* DOS1: discover native table. */
	pop ds
	mov ax, 0x1000
	mov es, ax
	xor si, si
	xor di, di
	mov cx, word ptr [ZBL2_SECTOR_COUNT]
	shl cx, 8
	rep movsw
	ljmp 0x1000, stage2_entry
stage2_resident:
	mov ds, ax
	movzx ecx, word ptr [ZBL2_SECTOR_COUNT]
	shl ecx, 7
	xor esi, esi
	xor eax, eax
stage2_checksum_loop:
	add eax, dword ptr [si]
	add si, 4
	loop stage2_checksum_loop
	test eax, eax
	jnz fatal_stage2_checksum
	mov ax, ds
	mov ss, ax
	mov esp, 0x9000
	sti
	push ds
	xor ax, ax
	mov ds, ax
	cmp dword ptr [0x0700], 0x3150395a
	jne fatal_bootinfo_ds
	mov al, byte ptr [0x0704]
	mov ah, byte ptr [0x0705]
	mov dl, byte ptr [0x0706]
	mov cx, word ptr [0x0708]
	mov si, word ptr [0x070a]
	mov di, word ptr [0x070c]
	pop ds
	mov byte ptr [boot_drive], al
	mov byte ptr [boot_heads], ah
	mov byte ptr [boot_sectors], dl
	mov word ptr [boot_cylinders], cx
	mov word ptr [bios_si], si
	mov word ptr [bios_di], di
	push ds
	xor ax, ax
	mov ds, ax
	cmp dword ptr [0x0714], 0x31545250
	je 3f
	cmp dword ptr [0x0714], 0x31534f44
	jne 1f
	pop ds
	mov byte ptr [dos_boot], 1
	jmp 2f
3:
	mov eax, dword ptr [0x0710]
	pop ds
	mov dword ptr [partition_lba], eax
	mov dword ptr [partition_blocks], 0xffffffff
	mov byte ptr [partition_index], 1
	mov byte ptr [pbr_boot], 1
	jmp 2f
1:	pop ds
2:
	mov si, offset msg_start
	call puts

	cmp byte ptr [pbr_boot], 0
	jne partition_ready
	cmp byte ptr [dos_boot], 0
	jne native_partition

	xor eax, eax
	call disk_read_one
	cmp word ptr es:[MBR_SIGNATURE], 0xaa55
	jne fatal_mbr
	mov si, offset msg_mbr
	call puts

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
	mov ax, 5
	sub ax, cx
	mov byte ptr [partition_index], al
mbr_status_ok:
	add si, MBR_ENTRY_SIZE
	loop mbr_loop
	test bx, bx
	jz fatal_partition
	jmp partition_ready

native_partition:
	/* NEC HDD table is always the physical sector at LBA 1.  DOS drive C:
	 * denotes its first allocated partition, which is the first live entry. */
	mov eax, 1
	call disk_read_one
	xor si, si
	mov cx, 16
native_loop:
	cmp byte ptr es:[si], 0
	jne native_found
	add si, 32
	loop native_loop
	jmp fatal_partition
native_found:
	movzx eax, word ptr es:[si + 10] /* data-start cylinder */
	movzx edx, byte ptr es:[si + 9] /* head */
	imul eax, 8
	add eax, edx
	imul eax, 17
	movzx edx, byte ptr es:[si + 8] /* sector */
	add eax, edx
	mov dword ptr [partition_lba], eax
	mov dword ptr [partition_blocks], 0xffffffff
	mov byte ptr [partition_index], 1
partition_ready:
	mov si, offset msg_partition
	call puts

	call fat16_mount
	mov word ptr [lookup_name], offset config_name
	call fat16_find_root_file
	jc fatal_config_missing
	mov si, offset msg_config_found
	call puts
	call load_config_file
	mov si, offset msg_config_read
	call puts
	call parse_bootzbsd_config
	mov si, offset msg_config_parsed
	call puts
	mov si, offset parsed_configuration
	call fat16_find_path
	jc fatal_kernel_missing
	mov si, offset msg_vmunix
	call puts

	cmp dword ptr [file_size], 52
	jb fatal_elf
	xor eax, eax
	call fat16_read_file_sector
	cmp dword ptr es:[0], 0x464c457f
	jne fatal_elf
	mov al, byte ptr es:[EI_CLASS]
	cmp al, ELFCLASS32
	je parse_elf32
	cmp al, ELFCLASS64
	je fatal_elf64
	jmp fatal_elf

/* ------------------------------------------------------------------ */
/* ELF32/i386. */
parse_elf32:
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
	cmp eax, 512
	ja fatal_elf
	cmp eax, dword ptr [file_size]
	ja fatal_elf
	mov eax, dword ptr es:[24]
	and eax, 0x7fffffff
	cmp eax, 0x00100000
	jb fatal_elf
	cmp eax, 0x00f00000
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
	mov eax, dword ptr es:[si + 12]
	and eax, 0x7fffffff
	cmp eax, 0x00100000
	jb fatal_elf
	mov dword ptr [di + 4], eax
	mov eax, dword ptr es:[si + 16]
	mov dword ptr [di + 8], eax
	mov edx, dword ptr [di + 0]
	add edx, eax
	jc fatal_elf
	cmp edx, dword ptr [file_size]
	ja fatal_elf
	mov edx, dword ptr es:[si + 20]
	cmp eax, edx
	ja fatal_elf
	mov dword ptr [di + 12], edx
	add edx, dword ptr [di + 4]
	jc fatal_elf
	cmp edx, 0x00f00000
	jae fatal_elf
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
	dec cx
	jz elf32_ph_done
	jmp elf32_ph_loop
elf32_ph_done:
	cmp word ptr [ph_load_count], 0
	je fatal_elf
	cmp byte ptr [elf_entry_executable], 1
	jne fatal_elf
	mov si, offset msg_elf32
	call puts
	jmp load_all_segments

/* ------------------------------------------------------------------ */
/* ELF64/x86-64, deliberately restricted to a small bootstrap subset. */
parse_elf64:
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
	cmp eax, 512
	ja fatal_elf
	mov eax, dword ptr es:[24]
	mov edx, dword ptr es:[28]
	cmp edx, 0xffffffff
	jne fatal_elf64
	cmp eax, 0x80000000
	jb fatal_elf64
	mov dword ptr [elf_entry_low], eax
	mov dword ptr [elf_entry_high], edx
	mov byte ptr [elf_class], ELFCLASS64
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
	cmp dword ptr es:[si + 28], 0
	jne fatal_elf64
	mov eax, dword ptr es:[si + 24]
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
	mov eax, dword ptr es:[si + 16]
	mov edx, dword ptr es:[si + 20]
	cmp edx, 0xffffffff
	jne fatal_elf64
	cmp eax, 0x80000000
	jb fatal_elf64
	mov dword ptr [di + 16], eax
	mov dword ptr [di + 20], edx
	mov eax, dword ptr es:[si + 4]
	mov dword ptr [di + 24], eax
	add di, PH_RECORD_SIZE
	inc word ptr [ph_load_count]
elf64_ph_next:
	add si, 56
	dec ecx
	jnz elf64_ph_loop
	cmp word ptr [ph_load_count], 0
	je fatal_elf
	mov si, offset msg_elf64
	call puts

/* ------------------------------------------------------------------ */
load_all_segments:
	call enable_a20_pc98
	call unreal_fs
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
	call fat16_read_file_sector
	mov ecx, 512
	cmp dword ptr [load_remaining], ecx
	jae 1f
	mov ecx, dword ptr [load_remaining]
1:	mov dword ptr [copy_count], ecx
	xor bx, bx
	mov edi, dword ptr [load_destination]
copy_sector_loop:
	mov edx, ecx
	shr ecx, 2
	jz copy_sector_tail
copy_sector_dword:
	mov eax, dword ptr es:[bx]
	mov dword ptr fs:[edi], eax
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
	mov byte ptr fs:[edi], al
	inc bx
	inc edi
	dec ecx
	jnz copy_sector_byte
copy_sector_done:
	mov eax, dword ptr [copy_count]
	add dword ptr [load_destination], eax
	sub dword ptr [load_remaining], eax
	inc dword ptr [load_file_sector]
	jmp load_file_loop
zero_bss:
	mov si, word ptr [ph_cursor]
	mov ecx, dword ptr [si + 12]
	sub ecx, dword ptr [si + 8]
	mov edi, dword ptr [si + 4]
	add edi, dword ptr [si + 8]
zero_bss_loop:
	mov edx, ecx
	shr ecx, 2
	xor eax, eax
	jz zero_bss_tail
zero_bss_dword:
	mov dword ptr fs:[edi], eax
	add edi, 4
	dec ecx
	jnz zero_bss_dword
zero_bss_tail:
	mov ecx, edx
	and ecx, 3
	jz next_ph
zero_bss_byte:
	mov byte ptr fs:[edi], 0
	inc edi
	dec ecx
	jnz zero_bss_byte
next_ph:
	add word ptr [ph_cursor], PH_RECORD_SIZE
	dec word ptr [ph_remaining]
	jmp load_ph_loop
load_complete:
	mov si, offset msg_loaded
	call puts
	cmp byte ptr [elf_class], ELFCLASS64
	je enter_long_mode
	jmp enter_protected_mode

/* ------------------------------------------------------------------ */
/* FAT16. */
fat16_mount:
	mov eax, dword ptr [partition_lba]
	call disk_read_one
	cmp word ptr es:[BPB_BYTES_PER_SECTOR], 512
	je 1f
	cmp word ptr es:[BPB_BYTES_PER_SECTOR], 1024
	jne fatal_fat
	mov byte ptr [fat_sector_scale], 2
	jmp 2f
1:	mov byte ptr [fat_sector_scale], 1
2:
	mov al, byte ptr es:[BPB_SECTORS_CLUSTER]
	test al, al
	jz fatal_fat
	mov ah, al
	dec ah
	test al, ah
	jnz fatal_fat
	movzx ax, al
	movzx cx, byte ptr [fat_sector_scale]
	imul ax, cx
	test ax, ax
	jz fatal_fat
	cmp ax, 0xff
	ja fatal_fat
	mov byte ptr [fat_spc], al
	movzx eax, word ptr es:[BPB_RESERVED_SECTORS]
	movzx ecx, byte ptr [fat_sector_scale]
	mul ecx
	test eax, eax
	jz fatal_fat
	add eax, dword ptr [partition_lba]
	jc fatal_fat
	mov dword ptr [fat_lba], eax
	movzx eax, word ptr es:[BPB_SECTORS_FAT16]
	test eax, eax
	jz fat32_mount
	movzx ecx, byte ptr [fat_sector_scale]
	mul ecx
	test eax, eax
	jz fatal_fat
	mov dword ptr [fat_sectors], eax
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
	add eax, 511
	shr eax, 9
	mov dword ptr [root_sectors], eax
	add eax, dword ptr [root_lba]
	jc fatal_fat
	mov dword ptr [data_lba], eax
	movzx eax, word ptr es:[BPB_TOTAL16]
	test eax, eax
	jnz 1f
	mov eax, dword ptr es:[BPB_TOTAL32]
1:	movzx ecx, byte ptr [fat_sector_scale]
	mul ecx
	test edx, edx
	jnz fatal_fat
	mov edx, eax
	add edx, dword ptr [partition_lba]
	jc fatal_fat
	mov dword ptr [fat_volume_end_lba], edx
	mov ecx, dword ptr [partition_blocks]
	cmp ecx, 0xffffffff
	je 3f
	add ecx, dword ptr [partition_lba]
	jc fatal_fat
	cmp edx, ecx
	ja fatal_fat
3:
	mov edx, dword ptr [data_lba]
	sub edx, dword ptr [partition_lba]
	cmp eax, edx
	jbe fatal_fat
	sub eax, edx
	movzx ecx, byte ptr [fat_spc]
	xor edx, edx
	div ecx
	test eax, eax
	jz fatal_fat
	mov dword ptr [fat_cluster_count], eax
	mov byte ptr [fat_type], FAT_TYPE_16
	cmp eax, 4085
	jae 2f
	mov byte ptr [fat_type], FAT_TYPE_12
2:
	cmp byte ptr es:[BPB_EXT_SIGNATURE16], 0x29
	jne fatal_fat
	mov eax, dword ptr es:[BPB_VOLUME_SERIAL16]
	call fat_uuid_format
	ret

fat32_mount:
	mov eax, dword ptr es:[BPB_SECTORS_FAT32]
	movzx ecx, byte ptr [fat_sector_scale]
	mul ecx
	test edx, edx
	jnz fatal_fat
	test eax, eax
	jz fatal_fat
	mov dword ptr [fat_sectors], eax
	movzx ecx, byte ptr es:[BPB_FAT_COUNT]
	test ecx, ecx
	jz fatal_fat
	mul ecx
	test edx, edx
	jnz fatal_fat
	add eax, dword ptr [fat_lba]
	jc fatal_fat
	mov dword ptr [data_lba], eax
	mov eax, dword ptr es:[BPB_ROOT_CLUSTER32]
	and eax, 0x0fffffff
	cmp eax, 2
	jb fatal_fat
	mov dword ptr [root_cluster32], eax
	mov dword ptr [root_entries], 0
	mov byte ptr [fat_type], FAT_TYPE_32
	mov eax, dword ptr es:[BPB_TOTAL32]
	movzx ecx, byte ptr [fat_sector_scale]
	mul ecx
	test edx, edx
	jnz fatal_fat
	mov edx, eax
	add edx, dword ptr [partition_lba]
	jc fatal_fat
	mov dword ptr [fat_volume_end_lba], edx
	mov ecx, dword ptr [partition_blocks]
	cmp ecx, 0xffffffff
	je 1f
	add ecx, dword ptr [partition_lba]
	jc fatal_fat
	cmp edx, ecx
	ja fatal_fat
1:
	mov edx, dword ptr [data_lba]
	sub edx, dword ptr [partition_lba]
	cmp eax, edx
	jbe fatal_fat
	sub eax, edx
	movzx ecx, byte ptr [fat_spc]
	xor edx, edx
	div ecx
	test eax, eax
	jz fatal_fat
	mov dword ptr [fat_cluster_count], eax
	cmp byte ptr es:[BPB_EXT_SIGNATURE32], 0x29
	jne fatal_fat
	mov eax, dword ptr es:[BPB_VOLUME_SERIAL32]
	call fat_uuid_format
	ret

fat16_find_root_file:
	mov byte ptr [file_cache_valid], 0
	mov byte ptr [fat_cache_valid], 0
	cmp byte ptr [fat_type], FAT_TYPE_32
	je fat32_find_root_file
	mov eax, dword ptr [root_lba]
	mov dword ptr [scan_lba], eax
	mov eax, dword ptr [root_entries]
	mov dword ptr [scan_entries], eax
fat_root_sector:
	cmp dword ptr [scan_entries], 0
	je fat_root_not_found
	mov eax, dword ptr [scan_lba]
	call disk_read_one
	xor bx, bx
	mov cx, 16
fat_root_entry:
	cmp dword ptr [scan_entries], 0
	je fat_root_not_found
	mov al, byte ptr es:[bx]
	test al, al
	jz fat_root_not_found
	cmp al, 0xe5
	je fat_root_next
	cmp byte ptr es:[bx + 11], 0x0f
	je fat_root_next
	push bx
	push cx
	mov si, word ptr [lookup_name]
	mov di, bx
	mov cx, 11
fat_name_loop:
	mov al, byte ptr [si]
	cmp al, byte ptr es:[di]
	jne fat_name_no
	inc si
	inc di
	loop fat_name_loop
	pop cx
	pop bx
	mov al, byte ptr es:[bx + 11]
	mov byte ptr [file_attributes], al
	movzx eax, word ptr es:[bx + 26]
	call fat_cluster_validate
	jc fatal_fat_chain
	mov word ptr [file_first_cluster], ax
	mov dword ptr [file_first_cluster32], eax
	mov eax, dword ptr es:[bx + 28]
	mov dword ptr [file_size], eax
	clc
	ret
fat_name_no:
	pop cx
	pop bx
fat_root_next:
	add bx, 32
	dec dword ptr [scan_entries]
	loop fat_root_entry
	inc dword ptr [scan_lba]
	jmp fat_root_sector

fat32_find_root_file:
	mov eax, dword ptr [root_cluster32]
	mov dword ptr [dir_cluster32], eax
	mov eax, dword ptr [fat_cluster_count]
	mov dword ptr [scan_clusters], eax
fat32_root_cluster:
	cmp dword ptr [scan_clusters], 0
	je fat_root_not_found
	dec dword ptr [scan_clusters]
	mov byte ptr [dir_sector], 0
fat32_root_sector:
	mov eax, dword ptr [dir_cluster32]
	call fat_cluster_to_lba
	jc fat_root_not_found
	mov dword ptr [scan_lba], eax
	movzx edx, byte ptr [dir_sector]
	add eax, edx
	jc fat_root_not_found
	cmp eax, dword ptr [fat_volume_end_lba]
	jae fat_root_not_found
	call disk_read_one
	xor bx, bx
	mov cx, 16
fat32_root_entry:
	mov al, byte ptr es:[bx]
	test al, al
	jz fat_root_not_found
	cmp al, 0xe5
	je fat32_root_next
	cmp byte ptr es:[bx + 11], 0x0f
	je fat32_root_next
	push bx
	push cx
	mov si, word ptr [lookup_name]
	mov di, bx
	mov cx, 11
fat32_name_loop:
	mov al, byte ptr [si]
	cmp al, byte ptr es:[di]
	jne fat32_name_no
	inc si
	inc di
	loop fat32_name_loop
	pop cx
	pop bx
	mov al, byte ptr es:[bx + 11]
	mov byte ptr [file_attributes], al
	movzx eax, word ptr es:[bx + 20]
	shl eax, 16
	mov ax, word ptr es:[bx + 26]
	and eax, 0x0fffffff
	call fat_cluster_validate
	jc fatal_fat_chain
	mov dword ptr [file_first_cluster32], eax
	mov eax, dword ptr es:[bx + 28]
	mov dword ptr [file_size], eax
	clc
	ret
fat32_name_no:
	pop cx
	pop bx
fat32_root_next:
	add bx, 32
	loop fat32_root_entry
	inc byte ptr [dir_sector]
	mov al, byte ptr [dir_sector]
	cmp al, byte ptr [fat_spc]
	jb fat32_root_sector
	mov eax, dword ptr [dir_cluster32]
	call fat32_next_cluster
	cmp eax, 0x0ffffff8
	jae fat_root_not_found
	mov dword ptr [dir_cluster32], eax
	jmp fat32_root_cluster

fat_root_not_found:
	stc
	ret

/* Validate a data-cluster number against both the FAT reserved ranges and
 * the data-cluster count derived from this volume.  EAX is preserved. */
fat_cluster_validate:
	cmp eax, 2
	jb 4f
	cmp byte ptr [fat_type], FAT_TYPE_32
	je 1f
	cmp byte ptr [fat_type], FAT_TYPE_12
	je 2f
	cmp eax, 0x0000fff0
	jae 4f
	jmp 3f
1:	cmp eax, 0x0ffffff0
	jae 4f
	jmp 3f
2:	cmp eax, 0x00000ff0
	jae 4f
3:	mov edx, eax
	sub edx, 2
	cmp edx, dword ptr [fat_cluster_count]
	jae 4f
	clc
	ret
4:	stc
	ret

/* Convert a validated cluster to its first physical 512-byte LBA and prove
 * that the complete cluster remains within this FAT volume. */
fat_cluster_to_lba:
	call fat_cluster_validate
	jc 1f
	sub eax, 2
	movzx ecx, byte ptr [fat_spc]
	mul ecx
	test edx, edx
	jnz 1f
	add eax, dword ptr [data_lba]
	jc 1f
	cmp eax, dword ptr [fat_volume_end_lba]
	jae 1f
	mov edx, eax
	add edx, ecx
	jc 1f
	cmp edx, dword ptr [fat_volume_end_lba]
	ja 1f
	clc
	ret
1:	stc
	ret

/* EAX is the FAT volume serial.  Emit the canonical XXXX-XXXX spelling used
 * by every zedbsd.cfg parser. */
fat_uuid_format:
	pushad
	mov edx, eax
	mov di, offset selected_uuid
	mov cx, 8
	xor bx, bx
fat_uuid_digit:
	rol edx, 4
	mov al, dl
	and al, 0x0f
	cmp al, 10
	jb 1f
	add al, 'A' - 10
	jmp 2f
1:	add al, '0'
2:	mov byte ptr [di], al
	inc di
	inc bx
	cmp bx, 4
	jne 3f
	mov byte ptr [di], '-'
	inc di
3:	loop fat_uuid_digit
	mov byte ptr [di], 0
	popad
	ret

/* Read a bounded BOOTZBSD.CFG into loader-owned conventional memory. */
load_config_file:
	test byte ptr [file_attributes], 0x10
	jnz fatal_config_invalid
	mov eax, dword ptr [file_size]
	cmp eax, ZBL_ZEDBSD_CONFIG_FILE_MAX
	ja fatal_config_too_long
	mov dword ptr [config_size], eax
	mov dword ptr [config_remaining], eax
	mov dword ptr [config_sector], 0
	mov di, offset config_source
load_config_sector:
	cmp dword ptr [config_remaining], 0
	je load_config_done
	mov eax, dword ptr [config_sector]
	call fat16_read_file_sector
	mov ecx, dword ptr [config_remaining]
	cmp ecx, 512
	jbe 1f
	mov ecx, 512
1:	mov dword ptr [copy_count], ecx
	xor bx, bx
load_config_copy:
	mov al, byte ptr es:[bx]
	mov byte ptr [di], al
	inc bx
	inc di
	dec cx
	jnz load_config_copy
	mov eax, dword ptr [copy_count]
	sub dword ptr [config_remaining], eax
	inc dword ptr [config_sector]
	jmp load_config_sector
load_config_done:
	ret

/* Split one parser-validated path component without narrowing its character
 * or length contract.  The shared C matcher resolves either its case-folded
 * LFN or its 8.3 alias from the on-disk directory entries. */
fat_path_component:
	mov si, word ptr [path_cursor]
	mov word ptr [path_component], si
	xor cx, cx
fat_path_character:
	mov al, byte ptr [si]
	test al, al
	jz fat_path_component_done
	cmp al, '/'
	je fat_path_more
	inc si
	inc cx
	cmp cx, 255
	ja fat_path_component_fail
	jmp fat_path_character
fat_path_more:
	mov byte ptr [path_has_more], 1
	inc si
	jmp 1f
fat_path_component_done:
	mov byte ptr [path_has_more], 0
1:	cmp cx, 0
	je fat_path_component_fail
	mov word ptr [path_component_length], cx
	mov word ptr [path_cursor], si
	mov di, offset fat_directory_state
	mov cx, ZBL_BIOS_FAT_DIRECTORY_STATE_SIZE / 4
	xor eax, eax
2:	mov dword ptr [di], eax
	add di, 4
	loop 2b
	clc
	ret
fat_path_component_fail:
	stc
	ret

/* Copy the latest physical directory sector into DS and call the shared
 * stateful LFN/8.3 matcher.  Its state persists across sector boundaries. */
fat_search_loaded_sector:
	xor si, si
	mov di, offset directory_sector_buffer
	mov cx, 512 / 4
1:	mov eax, dword ptr es:[si]
	mov dword ptr [di], eax
	add si, 4
	add di, 4
	loop 1b
	movzx esp, sp
	.code16gcc
	movzx eax, word ptr [path_component_length]
	push eax
	movzx eax, word ptr [path_component]
	push eax
	movzx eax, word ptr [directory_search_size]
	push eax
	mov eax, offset directory_sector_buffer
	push eax
	mov eax, offset fat_directory_state
	push eax
	call zbl_bios_fat_directory_search
	add esp, 20
	.code16
	ret

/* Capture a matched short entry from directory_sector_buffer+AX. */
fat_capture_entry:
	mov di, ax
	mov al, byte ptr [di + directory_sector_buffer + 11]
	mov byte ptr [file_attributes], al
	cmp byte ptr [fat_type], FAT_TYPE_32
	je fat_capture_entry32
	movzx eax, word ptr [di + directory_sector_buffer + 26]
	mov word ptr [file_first_cluster], ax
	mov dword ptr [file_first_cluster32], eax
	jmp fat_capture_entry_size
fat_capture_entry32:
	movzx eax, word ptr [di + directory_sector_buffer + 20]
	shl eax, 16
	mov ax, word ptr [di + directory_sector_buffer + 26]
	and eax, 0x0fffffff
	mov dword ptr [file_first_cluster32], eax
fat_capture_entry_size:
	mov eax, dword ptr [di + directory_sector_buffer + 28]
	mov dword ptr [file_size], eax
	mov eax, dword ptr [file_first_cluster32]
	call fat_cluster_validate
	jc 1f
	clc
	ret
1:	stc
	ret

/* Search a FAT12/16 fixed root or the FAT32 root cluster. */
fat_find_root_component:
	mov byte ptr [file_cache_valid], 0
	mov byte ptr [fat_cache_valid], 0
	cmp byte ptr [fat_type], FAT_TYPE_32
	je fat_find_root_component32
	mov eax, dword ptr [root_lba]
	mov dword ptr [scan_lba], eax
	mov eax, dword ptr [root_entries]
	mov dword ptr [scan_entries], eax
fat_component_root_sector:
	cmp dword ptr [scan_entries], 0
	je fat_component_not_found
	mov eax, dword ptr [scan_entries]
	cmp eax, 16
	jbe 1f
	mov eax, 16
1:	shl eax, 5
	mov word ptr [directory_search_size], ax
	mov eax, dword ptr [scan_lba]
	call disk_read_one
	call fat_search_loaded_sector
	test eax, eax
	jns fat_component_found
	cmp eax, -2
	je fat_component_not_found
	cmp dword ptr [scan_entries], 16
	jbe fat_component_not_found
	add dword ptr [scan_lba], 1
	sub dword ptr [scan_entries], 16
	jmp fat_component_root_sector
fat_find_root_component32:
	mov eax, dword ptr [root_cluster32]
	mov dword ptr [directory_cluster], eax
	jmp fat_find_cluster_component

/* Scan a subdirectory cluster chain with matcher state retained across all
 * physical sectors in the chain (PC-98 FAT uses 1024-byte logical sectors). */
fat_find_cluster_component:
	mov byte ptr [file_cache_valid], 0
	mov byte ptr [fat_cache_valid], 0
	mov word ptr [directory_search_size], 512
	mov eax, dword ptr [directory_cluster]
	mov dword ptr [dir_cluster32], eax
	mov eax, dword ptr [fat_cluster_count]
	mov dword ptr [scan_clusters], eax
fat_component_dir_cluster:
	cmp dword ptr [scan_clusters], 0
	je fat_component_not_found
	dec dword ptr [scan_clusters]
	/* Reject free, reserved, end-of-chain, and out-of-volume cluster
	 * numbers before using one as an LBA or FAT-chain cursor. */
	mov eax, dword ptr [dir_cluster32]
	call fat_cluster_to_lba
	jc fat_component_not_found
	mov dword ptr [scan_lba], eax
	mov byte ptr [dir_sector], 0
fat_component_dir_sector:
	/* scan_lba is the checked first physical sector of this cluster. */
	mov eax, dword ptr [scan_lba]
	movzx edx, byte ptr [dir_sector]
	add eax, edx
	jc fat_component_not_found
	cmp eax, dword ptr [fat_volume_end_lba]
	jae fat_component_not_found
	call disk_read_one
	call fat_search_loaded_sector
	test eax, eax
	jns fat_component_found
	cmp eax, -2
	je fat_component_not_found
	inc byte ptr [dir_sector]
	mov al, byte ptr [dir_sector]
	cmp al, byte ptr [fat_spc]
	jb fat_component_dir_sector
	mov eax, dword ptr [dir_cluster32]
	cmp byte ptr [fat_type], FAT_TYPE_32
	je fat_component_dir_next32
	cmp eax, 0xffff
	ja fat_component_not_found
	call fat16_next_cluster
	movzx eax, ax
	cmp ax, 0xfff8
	jae fat_component_not_found
	jmp fat_component_dir_next_ready
fat_component_dir_next32:
	call fat32_next_cluster
	cmp eax, 0x0ffffff8
	jae fat_component_not_found
fat_component_dir_next_ready:
	mov dword ptr [dir_cluster32], eax
	jmp fat_component_dir_cluster
fat_component_found:
	call fat_capture_entry
	ret
fat_component_not_found:
	stc
	ret

/* Resolve the parser-validated configured kernel path. */
fat16_find_path:
	mov word ptr [path_cursor], si
	mov byte ptr [path_at_root], 1
fat_path_next:
	call fat_path_component
	jc fat_path_not_found
	cmp byte ptr [path_at_root], 0
	je 1f
	call fat_find_root_component
	jmp 2f
1:	call fat_find_cluster_component
2:	jc fat_path_not_found
	cmp byte ptr [path_has_more], 0
	je fat_path_final
	test byte ptr [file_attributes], 0x10
	jz fat_path_not_found
	mov eax, dword ptr [file_first_cluster32]
	cmp byte ptr [fat_type], FAT_TYPE_32
	je 3f
	movzx eax, word ptr [file_first_cluster]
3:	mov dword ptr [directory_cluster], eax
	cmp eax, 2
	jb fat_path_not_found
	mov byte ptr [path_at_root], 0
	jmp fat_path_next
fat_path_final:
	test byte ptr [file_attributes], 0x10
	jnz fat_path_not_found
	cmp dword ptr [file_size], 0
	je fat_path_not_found
	clc
	ret
fat_path_not_found:
	stc
	ret

/* Invoke the exact p003 parser compiled for the BIOS loader's 16-bit GCC
 * calling convention, then copy its versioned record into the PC-98 handoff.
 * The kernel path is the first member of parsed_configuration. */
parse_bootzbsd_config:
	movzx esp, sp
	.code16gcc
	mov eax, 10
	push eax
	mov eax, offset selected_uuid
	push eax
	push dword ptr [config_size]
	mov eax, offset config_source
	push eax
	mov eax, offset parsed_configuration
	push eax
	call zbl_uefi_zedbsd_config_parse
	add esp, 20
	.code16
	test eax, eax
	jnz fatal_config_invalid
	mov si, offset parsed_configuration + \
		ZBL_ZEDBSD_CONFIG_PARAMETER_RECORD_OFFSET
	mov di, offset pc98_handoff + ZEDBSD_PC98_PARAMETER_RECORD_OFFSET
	mov cx, ZEDBSD_BOOT_PARAMETER_RECORD_SIZE / 4
1:	mov eax, dword ptr [si]
	mov dword ptr [di], eax
	add si, 4
	add di, 4
	loop 1b
	ret

/* EAX=file-relative sector; returns it in ES:0000. */
fat16_read_file_sector:
	/* No caller may walk beyond the directory-advertised file size. */
	mov dword ptr [desired_sector], eax
	mov edx, dword ptr [file_size]
	test edx, edx
	jz fatal_fat_chain
	dec edx
	shr edx, 9
	cmp eax, edx
	ja fatal_fat_chain
	/* A corrupt size field must not make a cyclic or overlong file chain
	 * consume more clusters than the mounted data area can contain. */
	movzx ecx, byte ptr [fat_spc]
	xor edx, edx
	div ecx
	cmp eax, dword ptr [fat_cluster_count]
	jae fatal_fat_chain
	mov eax, dword ptr [desired_sector]
	cmp byte ptr [fat_type], FAT_TYPE_32
	je fat32_read_file_sector
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
	call fat16_next_cluster
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
	call fat16_next_cluster
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
	call fat_cluster_to_lba
	jc fatal_fat_chain
	movzx edx, byte ptr [cached_sector_in_cluster]
	add eax, edx
	jc fatal_fat_chain
	cmp eax, dword ptr [fat_volume_end_lba]
	jae fatal_fat_chain
	call disk_read_one
	mov eax, dword ptr [requested_sector]
	cmp eax, dword ptr [desired_sector]
	jae fat_file_read_done
	inc eax
	jmp fat_file_next_request
fat_file_read_done:
	ret

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
	call fat32_next_cluster
	cmp eax, 0x0ffffff8
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
	call fat32_next_cluster
	cmp eax, 0x0ffffff8
	jae fatal_fat_chain
	dec dword ptr [cluster_steps]
	jmp fat32_file_walk
fat32_file_cluster_ready:
	cmp eax, 2
	jb fatal_fat_chain
	mov dword ptr [cached_cluster32], eax
	mov edx, dword ptr [sector_in_cluster]
	mov byte ptr [cached_sector_in_cluster], dl
	mov edx, dword ptr [desired_sector]
	mov dword ptr [cached_file_sector], edx
	mov byte ptr [file_cache_valid], 1
fat32_file_cluster_cached:
	call fat_cluster_to_lba
	jc fatal_fat_chain
	add eax, dword ptr [sector_in_cluster]
	jc fatal_fat_chain
	cmp eax, dword ptr [fat_volume_end_lba]
	jae fatal_fat_chain
	call disk_read_one
	mov eax, dword ptr [desired_sector]
	ret

fat32_next_cluster:
	push ebx
	push edx
	call fat_cluster_validate
	jc fatal_fat_chain
	shl eax, 2
	mov ebx, eax
	and ebx, 511
	mov word ptr [fat_offset32], bx
	shr eax, 9
	cmp eax, dword ptr [fat_sectors]
	jae fatal_fat_chain
	add eax, dword ptr [fat_lba]
	jc fatal_fat_chain
	cmp eax, dword ptr [fat_volume_end_lba]
	jae fatal_fat_chain
	call disk_read_one
	movzx ebx, word ptr [fat_offset32]
	mov eax, dword ptr es:[bx]
	and eax, 0x0fffffff
	pop edx
	pop ebx
	ret

/* AX=current cluster, AX=next cluster. */
fat16_next_cluster:
	movzx eax, ax
	call fat_cluster_validate
	jc fatal_fat_chain
	mov byte ptr [fat_cluster_odd], al
	and byte ptr [fat_cluster_odd], 1
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
	cmp eax, dword ptr [fat_sectors]
	jae fatal_fat_chain
	add eax, dword ptr [fat_lba]
	jc fatal_fat_chain
	cmp eax, dword ptr [fat_volume_end_lba]
	jae fatal_fat_chain
	cmp byte ptr [fat_cache_valid], 0
	je fat_cache_fill
	cmp eax, dword ptr [fat_cache_lba]
	je fat_cache_ready
fat_cache_fill:
	mov dword ptr [fat_cache_lba], eax
	call disk_read_one
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
	/* FAT16 entries never straddle a 512-byte physical sector.  A FAT12
	 * entry needs one byte from the following sector only at offset 511;
	 * reject that case if it would cross this FAT copy's own boundary. */
	cmp byte ptr [fat_type], FAT_TYPE_12
	jne fat_cache_fill_done
	cmp word ptr [fat_offset], 511
	jne fat_cache_fill_done
	mov eax, dword ptr [fat_cache_lba]
	inc eax
	jc fatal_fat_chain
	mov edx, dword ptr [fat_lba]
	add edx, dword ptr [fat_sectors]
	jc fatal_fat_chain
	cmp eax, edx
	jae fatal_fat_chain
	cmp eax, dword ptr [fat_volume_end_lba]
	jae fatal_fat_chain
	call disk_read_one
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
fat_cache_fill_done:
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
	ret

/* ------------------------------------------------------------------ */
disk_read_one:
	mov dword ptr [disk_lba], eax
	pushad
	push ds
	push es
	push bp
	mov eax, dword ptr cs:[disk_lba]
	xor edx, edx
	movzx edi, byte ptr cs:[boot_heads]
	movzx ecx, byte ptr cs:[boot_sectors]
	imul edi, ecx
	test edi, edi
	jz pc98_disk_fail
	div edi
	cmp eax, 0xffff
	ja pc98_disk_fail
	mov cx, ax
	mov eax, edx
	xor edx, edx
	movzx edi, byte ptr cs:[boot_sectors]
	div edi
	mov dh, al
	mov ax, ZBL_BOUNCE_SEG
	mov es, ax
	xor bp, bp
	xor ax, ax
	mov ds, ax
	mov al, byte ptr cs:[boot_drive]
	mov ah, 0x06
	mov bx, 512
	mov si, word ptr cs:[bios_si]
	mov di, word ptr cs:[bios_di]
	int 0x1b
	jc pc98_disk_fail
	cmp ah, 0x20
	jae pc98_disk_fail
	pop bp
	pop es
	pop ds
	popad
	mov ax, ZBL_BOUNCE_SEG
	mov es, ax
	ret
pc98_disk_fail:
	pop bp
	pop es
	pop ds
	popad
	jmp fatal_disk

/* Match the proven PC-98 loader sequence before touching >= 1 MiB. */
enable_a20_pc98:
	xor al, al
	out 0xf2, al
	mov al, 0x02
	out 0xf6, al
	mov dx, 0x0439
	in al, dx
	and al, 0xfb
	out dx, al
	xor al, al
	out 0xf8, al
	out 0x5f, al
	ret

/* Give FS a flat hidden descriptor while returning to real mode. */
unreal_fs:
	cli
	lgdt [gdt_pointer]
	mov eax, cr0
	or eax, 1
	mov cr0, eax
	mov ax, 0x10
	mov fs, ax
	mov eax, cr0
	and eax, 0xfffffffe
	mov cr0, eax
	sti
	ret

/* ------------------------------------------------------------------ */
prepare_pc98_handoff:
	mov dword ptr [pc98_handoff + 0], 0x48323842
	mov word ptr [pc98_handoff + 4], 2
	mov word ptr [pc98_handoff + 6], ZEDBSD_PC98_PARAMETER_HANDOFF_SIZE
	mov al, byte ptr [boot_drive]
	mov byte ptr [pc98_handoff + 9], al
	/* Version 2 selects the native PC-98 partition by LBA.  The scheme and
	 * index bytes are reserved and must remain zero. */
	mov word ptr [pc98_handoff + 10], 0
	mov eax, offset device_table
	add eax, 0x10000
	mov dword ptr [pc98_handoff + 12], eax
	mov eax, offset bios_gateway32
	add eax, 0x10000
	mov dword ptr [pc98_handoff + 16], eax
	mov eax, dword ptr [partition_lba]
	mov dword ptr [pc98_handoff + 20], eax

	mov byte ptr [device_table + 0], 2
	mov al, byte ptr [boot_drive]
	mov ah, al
	and ah, 0x0f
	mov byte ptr [device_table + 1], ah
	mov byte ptr [device_table + 2], al
	mov byte ptr [device_table + 3], 7
	mov word ptr [device_table + 4], 512
	mov ax, word ptr [boot_cylinders]
	mov word ptr [device_table + 6], ax
	mov al, byte ptr [boot_heads]
	mov byte ptr [device_table + 8], al
	mov al, byte ptr [boot_sectors]
	mov byte ptr [device_table + 9], al
	mov al, byte ptr [boot_drive]
	and al, 0x0f
	mov byte ptr [device_table + 10], al
	mov dword ptr [device_table + 11], 0
	mov byte ptr [device_table + 15], 0
	mov byte ptr [device_count], 1
	call enumerate_ide_devices
	mov al, byte ptr [device_count]
	mov byte ptr [pc98_handoff + 8], al
	ret

/* Enumerate only BIOS-advertised IDE units; SENSE on absent units may hang. */
enumerate_ide_devices:
	pushad
	push ds
	xor ax, ax
	mov ds, ax
	mov bl, byte ptr [0x055d]
	pop ds
	and bl, 0x0f
	mov al, 0x80
	mov cx, 4
enumerate_ide_loop:
	test bl, 1
	jz enumerate_ide_next
	cmp al, byte ptr [boot_drive]
	je enumerate_ide_next
	mov byte ptr [sense_bios_id], al
	push ax
	push bx
	push cx
	push ds
	xor bx, bx
	mov ds, bx
	mov al, byte ptr cs:[sense_bios_id]
	mov ah, 0x84
	mov bx, 0x0100
	mov si, word ptr cs:[bios_si]
	mov di, word ptr cs:[bios_di]
	int 0x1b
	mov word ptr cs:[sense_cylinders], cx
	mov byte ptr cs:[sense_heads], dh
	mov byte ptr cs:[sense_sectors], dl
	mov byte ptr cs:[sense_status], ah
	pop ds
	pop cx
	pop bx
	pop ax
	cmp byte ptr [sense_status], 0x20
	jae enumerate_ide_next
	cmp byte ptr [sense_heads], 0
	je enumerate_ide_next
	cmp byte ptr [sense_sectors], 0
	je enumerate_ide_next
	movzx edi, byte ptr [device_count]
	cmp edi, 4
	jae enumerate_ide_done
	shl di, 4
	mov byte ptr [device_table + edi + 0], 2
	mov ah, al
	and ah, 0x0f
	mov byte ptr [device_table + edi + 1], ah
	mov byte ptr [device_table + edi + 2], al
	mov byte ptr [device_table + edi + 3], 3
	mov word ptr [device_table + edi + 4], 512
	mov dx, word ptr [sense_cylinders]
	mov word ptr [device_table + edi + 6], dx
	mov dl, byte ptr [sense_heads]
	mov byte ptr [device_table + edi + 8], dl
	mov dl, byte ptr [sense_sectors]
	mov byte ptr [device_table + edi + 9], dl
	mov dl, byte ptr [sense_status]
	mov byte ptr [device_table + edi + 10], dl
	inc byte ptr [device_count]
enumerate_ide_next:
	shr bl, 1
	inc al
	dec cx
	jnz enumerate_ide_loop
enumerate_ide_done:
	popad
	ret

/* ------------------------------------------------------------------ */
.code32
bios_gateway32:
	push ebp
	push edi
	push esi
	push ebx
	mov eax, dword ptr [esp + 20]
	test eax, eax
	jz gateway32_invalid
	cmp word ptr [eax], 1
	jne gateway32_invalid
	mov dword ptr [0x10000 + gateway_request], eax
	mov dl, byte ptr [eax + 4]
	mov byte ptr [0x10000 + gateway_bios_id], dl
	mov dl, byte ptr [eax + 5]
	test dl, dl
	jz gateway32_invalid
	mov byte ptr [0x10000 + gateway_heads], dl
	mov dl, byte ptr [eax + 6]
	test dl, dl
	jz gateway32_invalid
	mov byte ptr [0x10000 + gateway_sectors], dl
	mov edx, dword ptr [eax + 8]
	mov dword ptr [0x10000 + gateway_lba], edx
	mov edx, dword ptr [eax + 12]
	test edx, edx
	jz gateway32_invalid
	mov dword ptr [0x10000 + gateway_buffer], edx
	mov eax, cr0
	mov dword ptr [0x10000 + gateway_saved_cr0], eax
	sgdt [0x10000 + gateway_saved_gdtr]
	lgdt [0x10000 + gdt_pointer]
	mov dword ptr [0x10000 + gateway_saved_esp], esp
	cli
	.att_syntax prefix
	ljmp $0x18, $gateway16_pm
	.intel_syntax noprefix
gateway32_invalid:
	mov eax, 0xffffffff
	pop ebx
	pop esi
	pop edi
	pop ebp
	ret

gateway32_return:
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov fs, ax
	mov gs, ax
	cmp dword ptr [0x10000 + gateway_result], 0
	jne gateway32_finish
	mov esi, 0x00080000
	mov edi, dword ptr [0x10000 + gateway_buffer]
	mov ecx, 512 / 4
	cld
	rep movsd
gateway32_finish:
	lgdt [0x10000 + gateway_saved_gdtr]
	mov esp, dword ptr [0x10000 + gateway_saved_esp]
	mov eax, dword ptr [0x10000 + gateway_result]
	pop ebx
	pop esi
	pop edi
	pop ebp
	ret

.code16
gateway16_pm:
	mov ax, 0x20
	mov ds, ax
	mov ss, ax
	mov sp, 0xeff0
	mov eax, dword ptr [gateway_saved_cr0]
	and eax, 0x7ffffffe
	mov cr0, eax
	.att_syntax prefix
	ljmp $0x1000, $gateway_real
	.intel_syntax noprefix

gateway_real:
	mov ax, cs
	mov ds, ax
	mov ax, 0x9000
	mov ss, ax
	mov sp, 0xeff0
	sti
	mov eax, dword ptr [gateway_lba]
	xor edx, edx
	movzx ebx, byte ptr [gateway_heads]
	movzx ecx, byte ptr [gateway_sectors]
	imul ebx, ecx
	test ebx, ebx
	jz gateway_disk_error
	div ebx
	cmp eax, 0xffff
	ja gateway_disk_error
	mov cx, ax
	mov eax, edx
	xor edx, edx
	movzx ebx, byte ptr [gateway_sectors]
	div ebx
	mov dh, al
	mov ax, ZBL_BOUNCE_SEG
	mov es, ax
	xor bp, bp
	xor ax, ax
	mov ds, ax
	mov al, byte ptr cs:[gateway_bios_id]
	mov ah, 0x06
	mov bx, 512
	mov si, word ptr cs:[bios_si]
	mov di, word ptr cs:[bios_di]
	int 0x1b
	jc gateway_disk_error_cs
	cmp ah, 0x20
	jae gateway_disk_error_cs
	mov dword ptr cs:[gateway_result], 0
	jmp gateway_real_return
gateway_disk_error:
	mov dword ptr [gateway_result], 0xffffffff
	jmp gateway_real_return
gateway_disk_error_cs:
	mov dword ptr cs:[gateway_result], 0xffffffff
gateway_real_return:
	cli
	push cs
	pop ds
	lgdt [gdt_pointer]
	mov eax, dword ptr [gateway_saved_cr0]
	mov cr0, eax
	.att_syntax prefix
	ljmpl *gateway_pm_return
	.intel_syntax noprefix

/* ------------------------------------------------------------------ */
enter_protected_mode:
	call prepare_pc98_handoff
	mov si, offset msg_go
	call puts
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
	mov si, offset msg_go
	call puts
	call unreal_fs
	mov edi, 0x00050000
	mov ecx, 0x5000 / 4
	xor eax, eax
1:	mov dword ptr fs:[edi], eax
	add edi, 4
	loop 1b
	mov edi, 0x00050000
	mov dword ptr fs:[edi], 0x00051003
	mov dword ptr fs:[edi + 4], 0
	mov edi, 0x00050000 + 511 * 8
	mov dword ptr fs:[edi], 0x00053003
	mov dword ptr fs:[edi + 4], 0
	mov edi, 0x00051000
	mov dword ptr fs:[edi], 0x00052003
	mov edi, 0x00053000 + 510 * 8
	mov dword ptr fs:[edi], 0x00054003
	mov edi, 0x00052000
	mov ebx, 0x00000083
	mov ecx, 512
2:	mov dword ptr fs:[edi], ebx
	mov dword ptr fs:[edi + 4], 0
	add ebx, 0x00200000
	add edi, 8
	loop 2b
	mov edi, 0x00054000
	mov ebx, 0x00000083
	mov ecx, 512
3:	mov dword ptr fs:[edi], ebx
	mov dword ptr fs:[edi + 4], 0
	add ebx, 0x00200000
	add edi, 8
	loop 3b
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
	mov ebx, offset pc98_handoff
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
	/* Keep this unreachable compatibility stub representable in the ELF32
	 * container used by the active i386 loader.  These are the ordinary
	 * x86-64 encodings with 32-bit absolute relocation fields. */
	.byte 0x48, 0xc7, 0xc7
	.long long_handoff
	add rdi, 0x10000
	.byte 0x8b, 0x04, 0x25
	.long 0x10000 + elf_entry_low
	.byte 0x8b, 0x14, 0x25
	.long 0x10000 + elf_entry_high
	shl rdx, 32
	or rax, rdx
	jmp rax

.code16
puts:
	lodsb
	test al, al
	jz 1f
	call putc
	jmp puts
1:	ret
putc:
	push ax
	push bx
	push dx
	push es
	mov dx, 0x00e9
	out dx, al
	cmp al, 10
	jne 1f
	mov ax, word ptr [text_cursor]
	xor dx, dx
	mov bx, 160
	div bx
	inc ax
	mul bx
	mov word ptr [text_cursor], ax
	jmp 2f
1:	mov bx, word ptr [text_cursor]
	push ax
	mov ax, 0xa000
	mov es, ax
	pop ax
	mov byte ptr es:[bx], al
	mov byte ptr es:[bx + 1], 0
	push ax
	mov ax, 0xa200
	mov es, ax
	mov byte ptr es:[bx], 0xe1
	pop ax
	add word ptr [text_cursor], 2
2:	pop es
	pop dx
	pop bx
	pop ax
	ret

fatal_bootinfo_ds:
	pop ds
	mov al, 'B'
	jmp fatal

fatal_mbr:       mov al, 'M'; jmp fatal
fatal_partition: mov al, 'P'; jmp fatal
fatal_fat:       mov al, 'F'; jmp fatal
fatal_fat_chain: mov al, 'K'; jmp fatal
fatal_config_missing:  mov al, 'Q'; jmp fatal
fatal_config_too_long: mov al, 'L'; jmp fatal
fatal_config_invalid:  mov al, 'G'; jmp fatal
fatal_kernel_missing:  mov al, 'V'; jmp fatal
fatal_elf:       mov al, 'E'; jmp fatal
fatal_elf64:     mov al, '6'; jmp fatal
fatal_stage2_checksum: mov al, 'C'; jmp fatal
fatal_disk:      mov al, 'D'
fatal:
	push ax
	mov si, offset msg_error
	call puts
	pop ax
	call putc
	mov al, '\n'
	call putc
	cli
1:	hlt
	jmp 1b

msg_start:     .asciz "S4 "
msg_mbr:       .asciz "MB "
msg_partition: .asciz "P1 "
msg_config_found:  .asciz "CF "
msg_config_read:   .asciz "CR "
msg_config_parsed: .asciz "CP "
msg_vmunix:    .asciz "VF "
msg_elf32:     .asciz "E3 "
msg_elf64:     .asciz "E6 "
msg_loaded:    .asciz "LD "
msg_go:        .asciz "GO\n"
msg_error:     .asciz "ERR "
config_name:   .ascii "BOOTZBSDCFG"

.balign 8
gdt:
	.quad 0
	.quad 0x00cf9a000000ffff
	.quad 0x00cf92000000ffff
	.quad 0x00009a010000ffff
	.quad 0x000092010000ffff
gdt_end:
gdt_pointer:
	.word gdt_end - gdt - 1
	.long 0x10000 + gdt
gateway_pm_return:
	.long 0x10000 + gateway32_return
	.word 0x08

.balign 4
dap:
	.byte 0x10, 0
	.word 1
dap_offset:  .word 0
dap_segment: .word 0
dap_lba:     .quad 0

ph_table: .space PH_RECORD_SIZE * 8

boot_drive:       .byte 0
boot_heads:       .byte 0
boot_sectors:     .byte 0
partition_index:  .byte 0
elf_class:        .byte 0
elf_entry_executable: .byte 0
fat_spc:          .byte 0
fat_sector_scale: .byte 1
fat_type:         .byte FAT_TYPE_16
fat_cluster_odd:  .byte 0
dir_sector:       .byte 0
disk_retry:       .byte 0
pbr_boot:         .byte 0
dos_boot:         .byte 0
file_cache_valid: .byte 0
fat_cache_valid:  .byte 0
device_count:     .byte 0
sense_bios_id:    .byte 0
sense_heads:      .byte 0
sense_sectors:    .byte 0
sense_status:     .byte 0
file_attributes:  .byte 0
path_at_root:     .byte 0
path_has_more:    .byte 0
.balign 2
cached_cluster:     .word 0
cached_sector_in_cluster: .byte 0
.balign 2
boot_cylinders:     .word 0
sense_cylinders:    .word 0
bios_si:            .word 0
bios_di:            .word 0
text_cursor:        .word 0
file_first_cluster: .word 0
fat_offset:         .word 0
fat_offset32:       .word 0
ph_load_count:      .word 0
ph_cursor:          .word 0
ph_remaining:       .word 0
lookup_name:         .word 0
path_cursor:         .word 0
path_component:      .word 0
path_component_length: .word 0
directory_search_size: .word 0
	.balign 4
partition_lba:      .long 0
root_cluster32:     .long 0
dir_cluster32:      .long 0
file_first_cluster32: .long 0
cached_cluster32:   .long 0
partition_blocks:   .long 0
fat_lba:            .long 0
fat_sectors:        .long 0
fat_cluster_count:  .long 0
root_lba:           .long 0
root_sectors:       .long 0
root_entries:       .long 0
data_lba:           .long 0
fat_volume_end_lba: .long 0
scan_lba:           .long 0
scan_entries:       .long 0
scan_clusters:      .long 0
file_size:          .long 0
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
disk_lba:           .long 0
gateway_request:    .long 0
gateway_saved_esp:  .long 0
gateway_saved_cr0:  .long 0
gateway_result:     .long 0
gateway_lba:        .long 0
gateway_buffer:     .long 0
config_size:        .long 0
config_remaining:   .long 0
config_sector:      .long 0
directory_cluster:  .long 0
gateway_saved_gdtr: .space 6
gateway_bios_id:    .byte 0
gateway_heads:      .byte 0
gateway_sectors:    .byte 0

.balign 8
mbi:
mbi_flags:       .long 3
mbi_mem_lower:   .long 640
mbi_mem_upper:   .long 0
mbi_boot_device: .long 0

long_handoff:
	.long 0x364c425a                 /* ZBL6 */
	.long 1
	.long partition_index
	.long boot_drive

.balign 4
pc98_handoff:
	.space ZEDBSD_PC98_HANDOFF_COMMON_SIZE
	.space ZEDBSD_BOOT_PARAMETER_RECORD_SIZE
device_table: .space 16 * 4
fat_cache: .space 1024

.balign 4
selected_uuid: .space 10
config_source: .space ZBL_ZEDBSD_CONFIG_FILE_MAX
parsed_configuration:
	.space ZBL_ZEDBSD_CONFIG_RESULT_SIZE
directory_sector_buffer: .space 512
fat_directory_state: .space ZBL_BIOS_FAT_DIRECTORY_STATE_SIZE
