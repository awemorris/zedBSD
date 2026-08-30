/* PC/AT FAT16 stage 3: load BOOTZBSD.EXE and enter its MZ entry point. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
.intel_syntax noprefix
.code16
.section .text
.global _start

#include "bootloader/include/stage2-header.inc"

.set LOAD_SEG,  0x0ffc             /* MZ header ends at physical 0x10000. */
.set TABLE_SEG, 0x0a00

_start:
	jmp pbr_start
	nop
	.ascii "ZEDBSD  "
	/* FAT16 ends its extended BPB at 0x3e; FAT32 extends it to 0x5a. */
	.fill 0x005a - (. - _start), 1, 0
pbr_start:
	ljmp 0x07c0, pbr_rebased
pbr_rebased:
	cli
	cld
	mov ax, cs
	mov ds, ax
	xor ax, ax
	mov ss, ax
	mov sp, 0x7c00
	sti
	mov byte ptr [boot_drive], dl

	/* Fetch the remaining three reserved sectors containing Stage 3. */
	cmp word ptr [0x000b], 512
	jne fatal
	movzx edx, word ptr [0x0013]
	test edx, edx
	jnz 1f
	mov edx, dword ptr [0x0020]
1:	test edx, edx
	jz fatal
	mov eax, dword ptr [0x001c]
	add edx, eax
	jc fatal
	mov ebx, edx
	cmp word ptr [0x0016], 0
	jne 2f
	cmp word ptr [0x000e], 5
	jb fatal
	add eax, 2                    /* Keep FAT32 FSInfo in reserved sector 1. */
	jmp 3f
2:	cmp word ptr [0x000e], 4
	jb fatal
	inc eax
3:	mov edx, eax
	add edx, 3
	jc fatal
	cmp edx, ebx
	ja fatal
	mov word ptr [dap_offset], 0x0200
	mov word ptr [dap_segment], 0x07c0
	mov cx, 3
4:
	call read_one
	inc eax
	add word ptr [dap_offset], 512
	loop 4b
	push cs
	push offset pbr_continue
	retf

fatal:
	mov al, 'P'
	mov dx, 0x00e9
	out dx, al
	cli
1:	hlt
	jmp 1b

read_one:
	mov dword ptr [dap_lba], eax
	push eax
	push ecx
	push edx
	push si
	mov si, offset dap
	mov ah, 0x42
	mov dl, byte ptr [boot_drive]
	int 0x13
	jc fatal
	pop si
	pop edx
	pop ecx
	pop eax
	ret

read_volume_one:
	cmp eax, dword ptr [0x001c]
	jb fatal
	cmp eax, dword ptr [volume_end_lba]
	jae fatal
	jmp read_one

.balign 4
dap:
	.byte 0x10, 0
	.word 1
dap_offset: .word 0
dap_segment: .word 0
dap_lba: .quad 0
boot_drive: .byte 0

.org 0x01fe
	.word 0xaa55

.org 0x0200
pbr_continue:
	/* Stage 4 also uses this marker to distinguish PBR and DOS entry. */
	push es
	xor ax, ax
	mov es, ax
	mov dword ptr es:[0x0700], 0x3142545a
	mov al, byte ptr [boot_drive]
	mov byte ptr es:[0x0704], al
	pop es
	cmp word ptr [0x000b], 512
	jne fatal
	cmp word ptr [0x000e], 2
	jb fatal
	movzx edx, word ptr [0x0013]
	test edx, edx
	jnz 1f
	mov edx, dword ptr [0x0020]
1:	test edx, edx
	jz fatal
	add edx, dword ptr [0x001c]
	jc fatal
	mov dword ptr [volume_end_lba], edx

	mov eax, dword ptr [0x001c]
	movzx ecx, word ptr [0x000e]
	add eax, ecx
	jc fatal
	mov dword ptr [fat_lba], eax
	movzx ecx, byte ptr [0x0010]
	test ecx, ecx
	jz fatal
	movzx edx, word ptr [0x0016]
	test edx, edx
	jz fat32_layout
	imul ecx, edx
	jo fatal
	add eax, ecx
	jc fatal
	mov dword ptr [root_lba], eax
	movzx ecx, word ptr [0x0011]
	shl ecx, 5
	add ecx, 511
	shr ecx, 9
	mov word ptr [root_sectors], cx
	add eax, ecx
	jc fatal
	mov dword ptr [data_lba], eax
	cmp eax, dword ptr [volume_end_lba]
	jae fatal
	jmp root_layout_ready

fat32_layout:
	mov edx, dword ptr [0x0024]
	test edx, edx
	jz fatal
	mov dword ptr [fat_sectors], edx
	imul ecx, edx
	jo fatal
	add eax, ecx
	jc fatal
	mov dword ptr [data_lba], eax
	cmp eax, dword ptr [volume_end_lba]
	jae fatal
	mov edx, dword ptr [volume_end_lba]
	sub edx, eax
	movzx ecx, byte ptr [0x000d]
	test ecx, ecx
	jz fatal
	mov eax, edx
	xor edx, edx
	div ecx
	test eax, eax
	jz fatal
	mov dword ptr [cluster_count], eax
	add eax, 2
	jc fatal
	mov dword ptr [cluster_limit], eax
	mov eax, dword ptr [fat_sectors]
	cmp eax, 0x01ffffff
	ja fatal
	shl eax, 7
	cmp eax, dword ptr [cluster_limit]
	jb fatal
	mov edx, dword ptr [0x002c]
	and edx, 0x0fffffff
	cmp edx, 2
	jb fatal
	cmp edx, dword ptr [cluster_limit]
	jae fatal
	mov dword ptr [root_cluster], edx
	mov eax, dword ptr [cluster_count]
	mov dword ptr [root_cluster_budget], eax
	sub edx, 2
	movzx ecx, byte ptr [0x000d]
	imul edx, ecx
	jo fatal
	mov eax, dword ptr [data_lba]
	add eax, edx
	jc fatal
	mov dword ptr [root_lba], eax
	movzx ecx, byte ptr [0x000d]
	mov word ptr [root_sectors], cx
	mov byte ptr [fat32_volume], 1
root_layout_ready:

	mov eax, dword ptr [root_lba]
	mov dword ptr [root_scan_lba], eax
	mov dx, word ptr [root_sectors]
root_sector:
	test dx, dx
	jz root_cluster_next
	mov eax, dword ptr [root_scan_lba]
	mov word ptr [dap_offset], 0
	mov word ptr [dap_segment], TABLE_SEG
	call read_volume_one
	mov ax, TABLE_SEG
	mov es, ax
	xor di, di
	mov cx, 16
root_entry:
	cmp byte ptr es:[di], 0
	je fatal
	cmp byte ptr es:[di], 0xe5
	je next_entry
	cmp byte ptr es:[di + 11], 0x0f
	je next_entry
	push cx
	push di
	mov si, offset loader_name
	mov cx, 11
	repe cmpsb
	pop di
	pop cx
	je loader_found
next_entry:
	add di, 32
	loop root_entry
	inc dword ptr [root_scan_lba]
	dec dx
	jmp root_sector

root_cluster_next:
	cmp byte ptr [fat32_volume], 0
	je fatal
	cmp dword ptr [root_cluster_budget], 0
	je fatal
	dec dword ptr [root_cluster_budget]
	mov eax, dword ptr [root_cluster]
	call pbr_fat32_next
	cmp eax, 0x0ffffff8
	jae fatal
	cmp eax, 2
	jb fatal
	cmp eax, dword ptr [cluster_limit]
	jae fatal
	mov dword ptr [root_cluster], eax
	sub eax, 2
	movzx ecx, byte ptr [0x000d]
	mul ecx
	test edx, edx
	jnz fatal
	add eax, dword ptr [data_lba]
	jc fatal
	mov dword ptr [root_scan_lba], eax
	movzx edx, byte ptr [0x000d]
	add edx, eax
	jc fatal
	cmp edx, dword ptr [volume_end_lba]
	ja fatal
	movzx edx, byte ptr [0x000d]
	jmp root_sector

pbr_fat32_next:
	push ebx
	push ecx
	push edx
	shl eax, 2
	mov ebx, eax
	and ebx, 511
	mov word ptr [fat_entry_offset], bx
	shr eax, 9
	add eax, dword ptr [fat_lba]
	jc fatal
	mov edx, dword ptr [fat_lba]
	add edx, dword ptr [fat_sectors]
	jc fatal
	cmp eax, edx
	jae fatal
	mov word ptr [dap_offset], 0
	mov word ptr [dap_segment], TABLE_SEG
	call read_volume_one
	mov ax, TABLE_SEG
	mov es, ax
	movzx ebx, word ptr [fat_entry_offset]
	mov eax, dword ptr es:[bx]
	and eax, 0x0fffffff
	pop edx
	pop ecx
	pop ebx
	ret

loader_found:
	movzx eax, word ptr es:[di + 26]
	cmp byte ptr [fat32_volume], 0
	je 1f
	movzx edx, word ptr es:[di + 20]
	shl edx, 16
	or eax, edx
	and eax, 0x0fffffff
1:
	cmp eax, 2
	jb fatal
	cmp byte ptr [fat32_volume], 0
	je 2f
	cmp eax, dword ptr [cluster_limit]
	jae fatal
2:
	sub eax, 2
	movzx ecx, byte ptr [0x000d]
	mul ecx
	test edx, edx
	jnz fatal
	add eax, dword ptr [data_lba]
	jc fatal
	mov ecx, dword ptr es:[di + 28]
	mov dword ptr [loader_size], ecx
	add ecx, 511
	jc fatal
	shr ecx, 9
	jz fatal
	cmp ecx, 127
	ja fatal
	mov edx, eax
	add edx, ecx
	jc fatal
	cmp edx, dword ptr [volume_end_lba]
	ja fatal
	mov word ptr [dap_offset], 0
	mov word ptr [dap_segment], LOAD_SEG
load_file:
	call read_volume_one
	inc eax
	add word ptr [dap_segment], 0x20
	loop load_file

	mov ax, LOAD_SEG
	mov es, ax
	cmp word ptr es:[0], 0x5a4d
	jne fatal
	cmp word ptr es:[6], 0
	jne fatal
	cmp word ptr es:[8], 4
	jne fatal
	cmp word ptr es:[0x14], ZBL2_HEADER_SIZE
	jne fatal
	cmp word ptr es:[0x16], 0
	jne fatal
	cmp dword ptr es:[0x40], ZBL2_MAGIC
	jne fatal
	cmp word ptr es:[0x44], ZBL2_VERSION
	jne fatal
	cmp word ptr es:[0x46], ZBL2_HEADER_SIZE
	jne fatal
	cmp word ptr es:[0x4e], ZBL2_MACHINE_PCAT
	jne fatal
	movzx eax, word ptr es:[0x4c]
	test eax, eax
	jz fatal
	cmp eax, 127
	ja fatal
	shl eax, 9
	add eax, 64
	jc fatal
	cmp eax, dword ptr [loader_size]
	jne fatal
	mov edx, dword ptr es:[0x48]
	cmp edx, ZBL2_HEADER_SIZE
	jb fatal
	movzx eax, word ptr es:[0x4c]
	shl eax, 9
	cmp edx, eax
	ja fatal
	mov eax, edx
	add eax, 511
	jc fatal
	shr eax, 9
	movzx ecx, word ptr es:[0x4c]
	cmp eax, ecx
	jne fatal
	mov eax, dword ptr es:[0x50]
	cmp eax, ZBL2_HEADER_SIZE
	jb fatal
	cmp eax, edx
	jae fatal
	mov cx, word ptr es:[0x4c]
	shl cx, 7
	mov bx, 0x40
	xor eax, eax
1:	add eax, dword ptr es:[bx]
	add bx, 4
	loop 1b
	test eax, eax
	jnz fatal
	mov ax, word ptr es:[8]
	add ax, word ptr es:[0x16]
	add ax, LOAD_SEG
	mov word ptr [entry_seg], ax
	mov ax, word ptr es:[0x14]
	mov word ptr [entry_off], ax
	mov dl, byte ptr [boot_drive]
	push word ptr [entry_seg]
	push word ptr [entry_off]
	retf

loader_name: .ascii "BOOTZBSDEXE"
root_lba: .long 0
root_scan_lba: .long 0
data_lba: .long 0
fat_lba: .long 0
fat_sectors: .long 0
root_cluster: .long 0
root_cluster_budget: .long 0
cluster_count: .long 0
cluster_limit: .long 0
volume_end_lba: .long 0
fat_entry_offset: .word 0
root_sectors: .word 0
entry_off: .word 0
entry_seg: .word 0
loader_size: .long 0
fat32_volume: .byte 0

.org 0x07fe
	.word 0xaa55
