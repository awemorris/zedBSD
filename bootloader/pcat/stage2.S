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

.set PH_RECORD_SIZE, 28

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
	mov ds, ax
	xor ax, ax
	mov ss, ax
	mov sp, 0x9000
	sti
	mov byte ptr [boot_drive], dl
	mov si, offset msg_start
	call puts

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
	call sample_vga_font

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
	mov si, offset msg_partition
	call puts

	call fat16_mount
	call select_kernel_name
	call fat16_find_vmunix
	mov si, offset msg_vmunix
	call puts

	xor eax, eax
	call fat16_read_file_sector
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
	movzx esi, word ptr es:[28]
	mov eax, ecx
	shl eax, 5
	add eax, esi
	cmp eax, 512
	ja fatal_elf
	mov eax, dword ptr es:[24]
	and eax, 0x7fffffff
	mov dword ptr [elf_entry_low], eax
	mov dword ptr [elf_entry_high], 0
	mov byte ptr [elf_class], ELFCLASS32
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
	mov dword ptr [di + 4], eax
	mov eax, dword ptr es:[si + 16]
	mov dword ptr [di + 8], eax
	mov edx, dword ptr es:[si + 20]
	cmp eax, edx
	ja fatal_elf
	mov dword ptr [di + 12], edx
	mov eax, dword ptr es:[si + 8]
	mov dword ptr [di + 16], eax
	mov dword ptr [di + 20], 0
	mov eax, dword ptr es:[si + 24]
	mov dword ptr [di + 24], eax
	add di, PH_RECORD_SIZE
	inc word ptr [ph_load_count]
elf32_ph_next:
	add si, 32
	loop elf32_ph_loop
	cmp word ptr [ph_load_count], 0
	je fatal_elf
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
	call unreal_gs
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
	call puts
	cmp byte ptr [elf_class], ELFCLASS64
	je enter_long_mode
	jmp enter_protected_mode

/* ------------------------------------------------------------------ */
/* FAT16. */
select_kernel_name:
	mov word ptr [vmunix_name_pointer], offset vmunix32_name
	pushfd
	pop eax
	mov ecx, eax
	xor eax, 0x00200000
	push eax
	popfd
	pushfd
	pop eax
	push ecx
	popfd
	xor eax, ecx
	test eax, 0x00200000
	jz 1f
	mov eax, 0x80000000
	cpuid
	cmp eax, 0x80000001
	jb 1f
	mov eax, 0x80000001
	cpuid
	and edx, 0x20100000
	cmp edx, 0x20100000
	jne 1f
	mov word ptr [vmunix_name_pointer], offset vmunix64_name
1:	ret

fat16_mount:
	mov eax, dword ptr [partition_lba]
	call disk_read_one
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
	movzx eax, word ptr es:[BPB_RESERVED_SECTORS]
	test eax, eax
	jz fatal_fat
	add eax, dword ptr [partition_lba]
	jc fatal_fat
	mov dword ptr [fat_lba], eax
	movzx eax, word ptr es:[BPB_SECTORS_FAT16]
	test eax, eax
	jz fatal_fat
	cmp byte ptr es:[BPB_EXT_SIGNATURE16], 0x29
	jne 1f
	mov edx, dword ptr es:[BPB_VOLUME_SERIAL16]
	mov dword ptr [boot_volume_serial], edx
	mov byte ptr [boot_uuid_valid], 1
1:
	mov dword ptr [fat_sectors], eax
	movzx ecx, byte ptr es:[BPB_FAT_COUNT]
	test ecx, ecx
	jz fatal_fat
	mul ecx
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
	ret

fat16_find_vmunix:
	mov byte ptr [file_cache_valid], 0
	mov byte ptr [fat_cache_valid], 0
	mov eax, dword ptr [root_lba]
	mov dword ptr [scan_lba], eax
	mov eax, dword ptr [root_entries]
	mov dword ptr [scan_entries], eax
fat_root_sector:
	cmp dword ptr [scan_entries], 0
	je fatal_vmunix
	mov eax, dword ptr [scan_lba]
	call disk_read_one
	xor bx, bx
	mov cx, 16
fat_root_entry:
	cmp dword ptr [scan_entries], 0
	je fatal_vmunix
	mov al, byte ptr es:[bx]
	test al, al
	jz fatal_vmunix
	cmp al, 0xe5
	je fat_root_next
	cmp byte ptr es:[bx + 11], 0x0f
	je fat_root_next
	push bx
	push cx
	mov si, word ptr [vmunix_name_pointer]
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
	mov ax, word ptr es:[bx + 26]
	cmp ax, 2
	jb fatal_vmunix
	mov word ptr [file_first_cluster], ax
	mov eax, dword ptr es:[bx + 28]
	test eax, eax
	jz fatal_vmunix
	mov dword ptr [file_size], eax
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

/* EAX=file-relative sector; returns it in ES:0000. */
fat16_read_file_sector:
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
	sub eax, 2
	movzx ecx, byte ptr [fat_spc]
	mul ecx
	add eax, dword ptr [data_lba]
	jc fatal_fat_chain
	movzx edx, byte ptr [cached_sector_in_cluster]
	add eax, edx
	jc fatal_fat_chain
	call disk_read_one
	mov eax, dword ptr [requested_sector]
	cmp eax, dword ptr [desired_sector]
	jae fat_file_read_done
	inc eax
	jmp fat_file_next_request
fat_file_read_done:
	ret

/* AX=current cluster, AX=next cluster. */
fat16_next_cluster:
	movzx eax, ax
	shl eax, 1
	mov dx, ax
	and dx, 511
	mov word ptr [fat_offset], dx
	shr eax, 9
	add eax, dword ptr [fat_lba]
	jc fatal_fat_chain
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
	mov byte ptr [fat_cache_valid], 1
fat_cache_ready:
	movzx ebx, word ptr [fat_offset]
	mov ax, word ptr [bx + fat_cache]
	cmp ax, 2
	jb fatal_fat_chain
	ret

/* ------------------------------------------------------------------ */
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
	ret

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
	ret

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
	ret

/* ------------------------------------------------------------------ */
enter_protected_mode:
	mov dword ptr [mbi_flags], 3
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
	mov eax, 0x80000000
	cpuid
	cmp eax, 0x80000001
	jb fatal_elf64
	mov eax, 0x80000001
	cpuid
	and edx, 0x20100000
	cmp edx, 0x20100000
	jne fatal_elf64
	call vbe_setup_framebuffer
	mov si, offset msg_go
	call puts
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
	call unreal_gs
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
	call vbe_map_framebuffer
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
	movzx r8d, word ptr [0x10000 + ph_load_count]
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
	mov rdi, offset long_handoff
	add rdi, 0x10000
	mov eax, dword ptr [0x10000 + elf_entry_low]
	mov edx, dword ptr [0x10000 + elf_entry_high]
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
	push dx
	mov dx, 0x00e9
	out dx, al
	pop dx
	pop ax
	ret

fatal_mbr:       mov al, 'M'; jmp fatal
fatal_partition: mov al, 'P'; jmp fatal
fatal_fat:       mov al, 'F'; jmp fatal
fatal_fat_chain: mov al, 'K'; jmp fatal
fatal_vmunix:    mov al, 'V'; jmp fatal
fatal_elf:       mov al, 'E'; jmp fatal
fatal_elf64:     mov al, '6'; jmp fatal
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

msg_start:     .asciz "S2 "
msg_mbr:       .asciz "MB "
msg_partition: .asciz "P1 "
msg_vmunix:    .asciz "VF "
msg_elf32:     .asciz "E3 "
msg_elf64:     .asciz "E6 "
msg_loaded:    .asciz "LD "
msg_go:        .asciz "GO\n"
msg_error:     .asciz "ERR "
#ifndef ZBL_KERNEL32_DOS_NAME
#ifdef ZBL_KERNEL_DOS_NAME
#define ZBL_KERNEL32_DOS_NAME ZBL_KERNEL_DOS_NAME
#else
#define ZBL_KERNEL32_DOS_NAME "VMUNIX     "
#endif
#endif
#ifndef ZBL_KERNEL64_DOS_NAME
#ifdef ZBL_KERNEL_DOS_NAME
#define ZBL_KERNEL64_DOS_NAME ZBL_KERNEL_DOS_NAME
#else
#define ZBL_KERNEL64_DOS_NAME "VMUNIX     "
#endif
#endif
vmunix32_name: .ascii ZBL_KERNEL32_DOS_NAME
vmunix64_name: .ascii ZBL_KERNEL64_DOS_NAME
vmunix_name_pointer: .word 0

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
fat_spc:          .byte 0
disk_retry:       .byte 0
file_cache_valid: .byte 0
fat_cache_valid:  .byte 0
boot_uuid_valid:  .byte 0
.balign 2
cached_cluster:     .word 0
cached_sector_in_cluster: .byte 0
.balign 2
file_first_cluster: .word 0
fat_offset:         .word 0
ph_load_count:      .word 0
ph_cursor:          .word 0
ph_remaining:       .word 0
.balign 4
partition_lba:      .long 0
partition_blocks:   .long 0
boot_volume_serial: .long 0
fat_lba:            .long 0
fat_sectors:        .long 0
root_lba:           .long 0
root_sectors:       .long 0
root_entries:       .long 0
data_lba:           .long 0
scan_lba:           .long 0
scan_entries:       .long 0
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

.balign 8
mbi:
mbi_flags:       .long 3
mbi_mem_lower:   .long 640
mbi_mem_upper:   .long 0
mbi_boot_device: .long 0

long_handoff:
	.long ZBL6_HANDOFF_MAGIC
	.word ZBL6_HANDOFF_VERSION
	.word ZBL6_HANDOFF_FB_SIZE
	.byte 0                         /* boot drive, populated at runtime */
	.byte 0                         /* partition index, populated at runtime */
	.word 0                         /* flags */
	.long 0                         /* conventional memory, KiB */
	.long 0                         /* extended memory, KiB */
	.long 0
	.space ZBL6_HANDOFF_FB_SIZE - ZBL6_HANDOFF_SIZE

fat_cache: .space 512

.balign 4
stage2_end:
