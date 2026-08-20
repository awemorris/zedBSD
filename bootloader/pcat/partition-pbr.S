/* PC/AT FAT16 stage 3: load BOOTZBSD.EXE and enter its MZ entry point. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
.intel_syntax noprefix
.code16
.section .text
.global _start

.set LOAD_SEG,  0x0ffc             /* MZ header ends at physical 0x10000. */
.set TABLE_SEG, 0x0a00

_start:
	jmp pbr_start
	nop
	.ascii "ZEDBSD  "
	.fill 0x003e - (. - _start), 1, 0
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
	mov eax, dword ptr [0x001c]
	cmp word ptr [0x0016], 0
	jne 1f
	add eax, 2                    /* Keep FAT32 FSInfo in reserved sector 1. */
	jmp 2f
1:	inc eax
2:
	mov word ptr [dap_offset], 0x0200
	mov word ptr [dap_segment], 0x07c0
	mov cx, 3
3:
	call read_one
	inc eax
	add word ptr [dap_offset], 512
	loop 3b
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

	mov eax, dword ptr [0x001c]
	movzx ecx, word ptr [0x000e]
	add eax, ecx
	mov dword ptr [fat_lba], eax
	movzx ecx, byte ptr [0x0010]
	movzx edx, word ptr [0x0016]
	test edx, edx
	jz fat32_layout
	imul ecx, edx
	add eax, ecx
	mov dword ptr [root_lba], eax
	movzx ecx, word ptr [0x0011]
	shl ecx, 5
	add ecx, 511
	shr ecx, 9
	mov word ptr [root_sectors], cx
	add eax, ecx
	mov dword ptr [data_lba], eax
	jmp root_layout_ready

fat32_layout:
	mov edx, dword ptr [0x0024]
	test edx, edx
	jz fatal
	imul ecx, edx
	add eax, ecx
	mov dword ptr [data_lba], eax
	mov edx, dword ptr [0x002c]
	and edx, 0x0fffffff
	cmp edx, 2
	jb fatal
	mov dword ptr [root_cluster], edx
	sub edx, 2
	movzx ecx, byte ptr [0x000d]
	imul edx, ecx
	add eax, edx
	mov dword ptr [root_lba], eax
	movzx ecx, byte ptr [0x000d]
	mov word ptr [root_sectors], cx
	mov byte ptr [fat32_volume], 1
root_layout_ready:

	mov eax, dword ptr [root_lba]
	mov dx, word ptr [root_sectors]
root_sector:
	test dx, dx
	jz root_cluster_next
	mov word ptr [dap_offset], 0
	mov word ptr [dap_segment], TABLE_SEG
	call read_one
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
	inc eax
	dec dx
	jmp root_sector

root_cluster_next:
	cmp byte ptr [fat32_volume], 0
	je fatal
	mov eax, dword ptr [root_cluster]
	call pbr_fat32_next
	cmp eax, 0x0ffffff8
	jae fatal
	mov dword ptr [root_cluster], eax
	sub eax, 2
	movzx ecx, byte ptr [0x000d]
	imul eax, ecx
	add eax, dword ptr [data_lba]
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
	mov word ptr [dap_offset], 0
	mov word ptr [dap_segment], TABLE_SEG
	call read_one
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
	sub eax, 2
	movzx ecx, byte ptr [0x000d]
	imul eax, ecx
	add eax, dword ptr [data_lba]
	mov ecx, dword ptr es:[di + 28]
	add ecx, 511
	shr ecx, 9
	jz fatal
	cmp ecx, 127
	ja fatal
	mov word ptr [dap_offset], 0
	mov word ptr [dap_segment], LOAD_SEG
load_file:
	call read_one
	inc eax
	add word ptr [dap_segment], 0x20
	loop load_file

	mov ax, LOAD_SEG
	mov es, ax
	cmp word ptr es:[0], 0x5a4d
	jne fatal
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
data_lba: .long 0
fat_lba: .long 0
root_cluster: .long 0
fat_entry_offset: .word 0
root_sectors: .word 0
entry_off: .word 0
entry_seg: .word 0
fat32_volume: .byte 0

.org 0x07fe
	.word 0xaa55
