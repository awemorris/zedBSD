/* Legacy-BIOS VBE 2.0+ linear-framebuffer selection for amd64 loaders.
 * Included by stage2.S and bootzbsd.S while they are still in real mode. */

.set VBE_CONTROLLER_INFO_SIZE, 512
.set VBE_MODE_INFO_SIZE,       256
.set VBE_MODE_SUPPORTED,       0x0001
.set VBE_MODE_GRAPHICS,        0x0010
.set VBE_MODE_LINEAR,          0x0080
.set VBE_MEMORY_DIRECT_COLOR,  6

/* Prefer a conservative 32-bpp mode.  1024x768, 800x600, and 640x480 are
 * deliberately the only accepted geometries so the early console always has
 * at least 80x25 cells and the graphics ABI receives a familiar mode. */
vbe_setup_framebuffer:
	pushad
	push ds
	push es
	mov ax, cs
	mov ds, ax
	mov es, ax
	mov di, offset vbe_controller_info
	mov dword ptr [di], 0x32454256       /* "VBE2" */
	mov ax, 0x4f00
	int 0x10
	mov dx, cs
	mov ds, dx
	cmp ax, 0x004f
	jne vbe_setup_done
	cmp dword ptr [vbe_controller_info], 0x41534556 /* "VESA" */
	jne vbe_setup_done
	cmp word ptr [vbe_controller_info + 4], 0x0200
	jb vbe_setup_done
	mov ax, word ptr [vbe_controller_info + 0x0e]
	mov word ptr [vbe_mode_list_offset], ax
	mov ax, word ptr [vbe_controller_info + 0x10]
	mov word ptr [vbe_mode_list_segment], ax
	mov word ptr [vbe_modes_left], 512
	mov byte ptr [vbe_best_rank], 0

vbe_mode_loop:
	cmp word ptr [vbe_modes_left], 0
	je vbe_mode_scan_done
	dec word ptr [vbe_modes_left]
	mov ax, word ptr [vbe_mode_list_segment]
	mov es, ax
	mov si, word ptr [vbe_mode_list_offset]
	mov dx, word ptr es:[si]
	add si, 2
	mov word ptr [vbe_mode_list_offset], si
	cmp dx, 0xffff
	je vbe_mode_scan_done
	mov word ptr [vbe_candidate_mode], dx
	mov ax, ds
	mov es, ax
	mov di, offset vbe_mode_info
	mov cx, dx
	mov ax, 0x4f01
	int 0x10
	mov dx, cs
	mov ds, dx
	cmp ax, 0x004f
	jne vbe_mode_loop
	mov ax, word ptr [vbe_mode_info]
	and ax, VBE_MODE_SUPPORTED | VBE_MODE_GRAPHICS | VBE_MODE_LINEAR
	cmp ax, VBE_MODE_SUPPORTED | VBE_MODE_GRAPHICS | VBE_MODE_LINEAR
	jne vbe_mode_loop
	cmp byte ptr [vbe_mode_info + 0x18], 1 /* NumberOfPlanes */
	jne vbe_mode_loop
	cmp byte ptr [vbe_mode_info + 0x19], 32 /* BitsPerPixel */
	jne vbe_mode_loop
	cmp byte ptr [vbe_mode_info + 0x1b], VBE_MEMORY_DIRECT_COLOR
	jne vbe_mode_loop
	cmp byte ptr [vbe_mode_info + 0x1f], 8 /* RedMaskSize */
	jne vbe_mode_loop
	cmp byte ptr [vbe_mode_info + 0x21], 8 /* GreenMaskSize */
	jne vbe_mode_loop
	cmp byte ptr [vbe_mode_info + 0x23], 8 /* BlueMaskSize */
	jne vbe_mode_loop

	mov byte ptr [vbe_candidate_rank], 0
	mov ax, word ptr [vbe_mode_info + 0x12]
	mov dx, word ptr [vbe_mode_info + 0x14]
	cmp ax, 1024
	jne 1f
	cmp dx, 768
	jne vbe_mode_loop
	mov byte ptr [vbe_candidate_rank], 3
	jmp 3f
1:	cmp ax, 800
	jne 2f
	cmp dx, 600
	jne vbe_mode_loop
	mov byte ptr [vbe_candidate_rank], 2
	jmp 3f
2:	cmp ax, 640
	jne vbe_mode_loop
	cmp dx, 480
	jne vbe_mode_loop
	mov byte ptr [vbe_candidate_rank], 1
3:	mov al, byte ptr [vbe_candidate_rank]
	cmp al, byte ptr [vbe_best_rank]
	jbe vbe_mode_loop

	/* Prefer the VBE 3 linear pitch, falling back to the VBE 2 pitch. */
	movzx eax, word ptr [vbe_mode_info + 0x32]
	test eax, eax
	jnz 4f
	movzx eax, word ptr [vbe_mode_info + 0x10]
4:	test eax, 3
	jnz vbe_mode_loop
	movzx edx, word ptr [vbe_mode_info + 0x12]
	shl edx, 2
	cmp eax, edx
	jb vbe_mode_loop
	mov edx, dword ptr [vbe_mode_info + 0x28]
	test edx, edx
	jz vbe_mode_loop

	/* Determine the byte order represented by a little-endian uint32_t pixel. */
	mov ecx, 2                         /* ZBL6_FRAMEBUFFER_BGRX8888 */
	cmp byte ptr [vbe_mode_info + 0x20], 16
	jne 5f
	cmp byte ptr [vbe_mode_info + 0x22], 8
	jne vbe_mode_loop
	cmp byte ptr [vbe_mode_info + 0x24], 0
	jne vbe_mode_loop
	jmp 6f
5:	mov ecx, 1                         /* ZBL6_FRAMEBUFFER_RGBX8888 */
	cmp byte ptr [vbe_mode_info + 0x20], 0
	jne vbe_mode_loop
	cmp byte ptr [vbe_mode_info + 0x22], 8
	jne vbe_mode_loop
	cmp byte ptr [vbe_mode_info + 0x24], 16
	jne vbe_mode_loop
6:	mov al, byte ptr [vbe_candidate_rank]
	mov byte ptr [vbe_best_rank], al
	mov dx, word ptr [vbe_candidate_mode]
	mov word ptr [vbe_best_mode], dx
	mov dx, word ptr [vbe_mode_info + 0x12]
	mov word ptr [vbe_best_width], dx
	mov dx, word ptr [vbe_mode_info + 0x14]
	mov word ptr [vbe_best_height], dx
	mov edx, dword ptr [vbe_mode_info + 0x28]
	mov dword ptr [vbe_best_base], edx
	mov dword ptr [vbe_best_format], ecx
	movzx eax, word ptr [vbe_mode_info + 0x32]
	test eax, eax
	jnz 7f
	movzx eax, word ptr [vbe_mode_info + 0x10]
7:	mov edx, eax
	shr edx, 2
	mov dword ptr [vbe_best_stride], edx
	movzx edx, word ptr [vbe_mode_info + 0x14]
	imul eax, edx
	jo vbe_mode_loop
	mov ebx, dword ptr [vbe_mode_info + 0x28]
	add ebx, eax
	jc vbe_mode_loop
	mov dword ptr [vbe_best_size], eax
	cmp byte ptr [vbe_best_rank], 3
	jne vbe_mode_loop

vbe_mode_scan_done:
	cmp byte ptr [vbe_best_rank], 0
	je vbe_setup_done
	mov bx, word ptr [vbe_best_mode]
	or bx, 0x4000                    /* request the linear framebuffer */
	mov ax, 0x4f02
	int 0x10
	mov dx, cs
	mov ds, dx
	cmp ax, 0x004f
	jne vbe_setup_done
	or word ptr [long_handoff + ZBL6_HANDOFF_FLAGS_OFFSET], 0x0008
	mov eax, dword ptr [vbe_best_base]
	mov dword ptr [long_handoff + ZBL6_HANDOFF_FRAMEBUFFER_BASE_OFFSET], eax
	mov dword ptr [long_handoff + ZBL6_HANDOFF_FRAMEBUFFER_BASE_OFFSET + 4], 0
	mov eax, dword ptr [vbe_best_size]
	mov dword ptr [long_handoff + ZBL6_HANDOFF_FRAMEBUFFER_SIZE_OFFSET], eax
	mov dword ptr [long_handoff + ZBL6_HANDOFF_FRAMEBUFFER_SIZE_OFFSET + 4], 0
	movzx eax, word ptr [vbe_best_width]
	mov dword ptr [long_handoff + ZBL6_HANDOFF_FRAMEBUFFER_WIDTH_OFFSET], eax
	movzx eax, word ptr [vbe_best_height]
	mov dword ptr [long_handoff + ZBL6_HANDOFF_FRAMEBUFFER_HEIGHT_OFFSET], eax
	mov eax, dword ptr [vbe_best_stride]
	mov dword ptr [long_handoff + ZBL6_HANDOFF_FRAMEBUFFER_STRIDE_OFFSET], eax
	mov eax, dword ptr [vbe_best_format]
	mov dword ptr [long_handoff + ZBL6_HANDOFF_FRAMEBUFFER_FORMAT_OFFSET], eax

vbe_setup_done:
	pop es
	pop ds
	popad
	ret

/* Install the same early high-half framebuffer window used by the UEFI
 * transition.  GS must already carry the loader's flat unreal-mode cache. */
vbe_map_framebuffer:
	test word ptr [long_handoff + ZBL6_HANDOFF_FLAGS_OFFSET], 0x0008
	jz 2f
	mov edi, 0x00053000 + 511 * 8
	mov dword ptr gs:[edi], 0x00055003
	mov dword ptr gs:[edi + 4], 0
	mov eax, dword ptr [long_handoff + ZBL6_HANDOFF_FRAMEBUFFER_BASE_OFFSET]
	mov edx, eax
	and edx, 0x001fffff
	and eax, 0xffe00000
	or eax, 0x00000093              /* present, writable, uncached, 2 MiB */
	mov ebx, eax
	mov eax, dword ptr [long_handoff + ZBL6_HANDOFF_FRAMEBUFFER_SIZE_OFFSET]
	add eax, edx
	add eax, 0x001fffff
	shr eax, 21
	mov ecx, eax
	mov edi, 0x00055000 + 16 * 8
1:	mov dword ptr gs:[edi], ebx
	mov dword ptr gs:[edi + 4], 0
	add ebx, 0x00200000
	add edi, 8
	loop 1b
2:	ret

.balign 4
vbe_mode_list_offset:  .word 0
vbe_mode_list_segment: .word 0
vbe_modes_left:        .word 0
vbe_candidate_mode:    .word 0
vbe_best_mode:         .word 0
vbe_best_width:        .word 0
vbe_best_height:       .word 0
vbe_best_rank:         .byte 0
vbe_candidate_rank:    .byte 0
.balign 4
vbe_best_base:         .long 0
vbe_best_size:         .long 0
vbe_best_stride:       .long 0
vbe_best_format:       .long 0
vbe_controller_info:   .space VBE_CONTROLLER_INFO_SIZE
vbe_mode_info:         .space VBE_MODE_INFO_SIZE
