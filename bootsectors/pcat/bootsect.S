/* 512-byte PC/AT MBR and ELF32 loader.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
.intel_syntax noprefix
.code16
.global _start

.equ KERNEL_LBA, 1
.equ HDR_SEG, 0x7000
.equ SEC_SEG, 0x7200
.equ VAR, 0x7e00
.equ boot_drive, VAR+0
.equ copy_len, VAR+2
.equ destination, VAR+4
.equ remaining, VAR+8
.equ file_offset, VAR+12
.equ kernel_entry, VAR+16
.equ mbi, VAR+20
.equ mbi_flags, mbi+0
.equ mbi_lower, mbi+4
.equ mbi_upper, mbi+8
.equ mbi_boot_device, mbi+12

.equ E_ENTRY, 24
.equ E_PHOFF, 28
.equ E_PHENTSIZE, 42
.equ E_PHNUM, 44
.equ P_TYPE, 0
.equ P_OFFSET, 4
.equ P_PADDR, 12
.equ P_FILESZ, 16

_start:
	cli
	cld
	xor ax, ax
	mov ds, ax
	mov ss, ax
	mov sp, 0x7c00
	mov [boot_drive], dl

	/* Enable A20 through the fast gate. */
	in al, 0x92
	or al, 2
	out 0x92, al

	/* Build the standard subset of Multiboot information. */
	mov dword ptr [mbi_flags], 3
	mov dword ptr [mbi_lower], 640
	mov ax, 0x8800
	int 0x15
	jnc 1f
	mov ax, 0x3c00
1:
	movzx eax, ax
	mov [mbi_upper], eax
	movzx eax, byte ptr [boot_drive]
	shl eax, 24
	or eax, 0x0000ffff
	mov [mbi_boot_device], eax

	call unreal_gs
	mov ax, HDR_SEG
	mov es, ax
	mov ax, KERNEL_LBA
	mov bx, HDR_SEG
	call read_sector

	mov al, 'E'
	cmp dword ptr es:[0], 0x464c457f
	jne fatal
	mov eax, dword ptr es:[E_ENTRY]
	mov [kernel_entry], eax
	mov cx, word ptr es:[E_PHNUM]
	mov bx, word ptr es:[E_PHENTSIZE]
	mov si, word ptr es:[E_PHOFF]

ph_loop:
	jcxz enter32
	cmp dword ptr es:[si+P_TYPE], 1
	jne ph_next
	call load_segment
ph_next:
	add si, bx
	loop ph_loop

enter32:
	lgdt [gdt_pointer]
	mov eax, cr0
	or al, 1
	mov cr0, eax
	jmp 0x08:protected_entry

load_segment:
	pushad
	mov eax, dword ptr es:[si+P_PADDR]
	mov [destination], eax
	mov eax, dword ptr es:[si+P_FILESZ]
	mov [remaining], eax
	mov eax, dword ptr es:[si+P_OFFSET]
	mov [file_offset], eax
load_loop:
	cmp dword ptr [remaining], 0
	je load_done
	mov eax, [file_offset]
	shr eax, 9
	add ax, KERNEL_LBA
	mov bx, SEC_SEG
	call read_sector
	mov eax, 512
	cmp eax, [remaining]
	jbe 1f
	mov eax, [remaining]
1:
	mov [copy_len], ax
	call copy_sector
	movzx eax, word ptr [copy_len]
	add [file_offset], eax
	add [destination], eax
	sub [remaining], eax
	jmp load_loop
load_done:
	popad
	ret

copy_sector:
	push ds
	xor si, si
	mov edi, [destination]
	mov cx, [copy_len]
	mov ax, SEC_SEG
	mov ds, ax
1:
	lodsb
	mov byte ptr gs:[edi], al
	inc edi
	loop 1b
	pop ds
	ret

read_sector:
	mov [dap_lba], ax
	mov [dap_segment], bx
	mov ah, 0x42
	mov dl, [boot_drive]
	mov si, offset dap
	push es
	int 0x13
	pop es
	mov al, 'D'
	jc fatal
	ret

unreal_gs:
	lgdt [gdt_pointer]
	mov eax, cr0
	or al, 1
	mov cr0, eax
	mov ax, 0x10
	mov gs, ax
	mov eax, cr0
	and al, 0xfe
	mov cr0, eax
	ret

fatal:
	mov ah, 0x0e
	int 0x10
	hlt
	jmp fatal

.code32
protected_entry:
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax
	mov ss, ax
	mov esp, 0x0009f000
	mov eax, 0x2badb002
	mov ebx, mbi
	jmp dword ptr [kernel_entry]

.code16
gdt:
	.quad 0
	.quad 0x00cf9a000000ffff
	.quad 0x00cf92000000ffff
gdt_end:
gdt_pointer:
	.word gdt_end-gdt-1
	.long gdt

.balign 4
dap:
	.byte 0x10, 0
	.word 1
	.word 0
dap_segment:
	.word 0
dap_lba:
	.quad 0

.org 0x1be
	.space 64
.org 510
	.word 0xaa55
