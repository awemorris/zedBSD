/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "defs.h"

#if defined(HAL_BOARD_PCAT)
	.section .multiboot,"a"
	.balign 4
	multiboot_header:
	.long 0x1badb002
	.long 0x00010003
	.long -(0x1badb002 + 0x00010003)
	/* Multiboot v1 address fields.  The ELF's first allocated section is
	 * at file offset 0x1000, so GRUB copies the ELF header page just below
	 * the 2 MiB kernel and enters the physical bootstrap alias. */
	.long multiboot_header - SYS_START
	.long multiboot_header - SYS_START - 0x1000
	.long __low_image_end - SYS_START
	.long __low_end - SYS_START
	.long text_start - SYS_START
#endif

.code32
.data

/*
 * PDT/PT
 *
 * The initial tables live in kernel BSS (fixed low addresses collided
 * with the vmunix low segment at 0x20000).  Everything referenced
 * before CR0.PG is up goes through "- SYS_START" to reach the physical
 * alias, exactly like the rest of this file.
 *
 * The map built here:
 *   PDE[512..543] : SYS_START + 0..128MB -> phys 0..128MB (direct map)
 *   PDE[  0.. 31] : identity alias of the same 128MB, so physical
 *                   pointers held by the pre-HAL loader keep working
 *   PDE[960..963] : identity map of the Cirrus linear aperture at
 *                   0xf0000000 (16MB, cache-disabled)
 */
NUM_HIGH_PDE   = 32		/* 32 * 4MB = 128MB */
NUM_PTE_TOTAL  = 32 * 1024      /* 128MB / 4KB */
NUM_APERTURE_PDE = 4		/* 16MB at 0xf0000000 */
APIC_PDE = 1019			/* 0xfec00000-0xfeffffff */
PDE_SYS = PTE_PRESENT | PTE_WRITE
PTE_SYS = PTE_PRESENT | PTE_WRITE
PTE_APERTURE = PTE_PRESENT | PTE_WRITE | PTE_NOCACHE

.text

/*
 * Kernel entry point.
 */ 
.global text_start
text_start:

#if defined(HAL_BOARD_PCAT)
	cmpl	$0x2badb002, %eax
	jne	.L_bad_multiboot
#endif

/*
 * Save the boot handoff.  Our own IPL chain reaches here with EBX
 * holding the physical address of the zedBSD handoff block; it is
 * parked at ADDR_BOOT_INFO and the kernel entry validates its "B82H"
 * magic (there is no multiboot in this path).
 */
save_boot_info:
	movl	%ebx, (ADDR_BOOT_INFO)

/*
 * Clear the kernel BSS regions (the linker script provides the bounds;
 * the loader copies only file bytes).  The page tables below live in
 * BSS, so this must precede init_paging.
 */
clear_bss:
	xorl	%eax, %eax
	cld
#if defined(HAL_BOARD_PCAT)
	movl	$(__low_bss_start - SYS_START), %edi
	movl	$(__low_bss_end - SYS_START), %ecx
	subl	%edi, %ecx
	rep stosb
	movl	$(__high_bss_start - SYS_START), %edi
	movl	$(__high_bss_end - SYS_START), %ecx
	subl	%edi, %ecx
	rep stosb
#else
	movl	$(__kernel_bss_start - SYS_START), %edi
	movl	$(__kernel_bss_end - SYS_START), %ecx
	subl	%edi, %ecx
	rep stosb
#endif

/*
 * Setup GDT.
 */
init_gdt:

	/* GDTRにGDTをロードする */
	movl	$(gdtr - SYS_START), %ebx
	lgdt	(%ebx)

	/* 作成したセグメント上にジャンプする */
	ljmp	$(SEG_SYS_CODE), $(.L_new_seg - 0x80000000)
    .L_new_seg:

	/* セグメントを設定する */
	movl	$(SEG_SYS_DATA), %eax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %fs
	movw	%ax, %gs
	movw	%ax, %ss

/*
 * Enable paging.
 */
init_paging:

	/*
	 * Clear the page directory table.
	 *
	 * PDT has 1024 entries.
	 * Each entry is 4 bytes.
	*/
	movl    $(initial_pdt - SYS_START), %edi
	xorl    %eax, %eax
	movl    $1024, %ecx
	cld
	rep    stosl

	/*
	* Build page tables for 256MB.
	*
	* We place 64 page tables immediately after PDT:
	*
	*   ADDR_INIT_PT + 0x00000 : PT0
	*   ADDR_INIT_PT + 0x01000 : PT1
	*   ADDR_INIT_PT + 0x02000 : PT2
	*   ...
	*   ADDR_PT + 0x3f000 : PT64
	*
	* Each PT maps 4MB using 1024 entries.
	*
	* Total:
	*
	*   64 * 1024 PTEs = 65536 PTEs
	*   65536 * 4KB   = 256MB
	*/
	movl    $(initial_pts - SYS_START), %edi
	movl    $(0x00000000 + PTE_SYS), %eax
	movl    $(NUM_PTE_TOTAL), %ecx
	cld

    .L_fill_pte:
	/*
	* PTE format without PSE:
	*
	*   bits 31..12 : physical page base address
	*   bits 11..0  : flags
	*
	* eax starts as:
	*
	*   0x00000000 | flags
	*
	* and increases by 0x1000 for each 4KB page.
	*/
	stosl
	addl    $0x1000, %eax
	loop    .L_fill_pte

	/*
	* Fill PDE[512] - PDE[576].
	*
	* 0x80000000 / 4MB = 512
	*
	* So PDE[512] corresponds to virtual 0x80000000.
	*
	* Each PDE points to one 4KB page table.
	*/
	movl    $(initial_pdt - SYS_START + 512 * 4), %edi
	movl    $(initial_pts - SYS_START + PDE_SYS), %eax
	movl    $(NUM_HIGH_PDE), %ecx

  .L_fill_high_pde:
	/*
	* PDE points to the physical address of a page table.
	*
	* PDE[512] -> PT0  maps 0x80000000 - 0x803fffff
	* PDE[513] -> PT1  maps 0x80400000 - 0x807fffff
	* PDE[514] -> PT2  maps 0x80800000 - 0x80bfffff
	* ...
	*/
	movl    %eax, (%edi)
	addl    $4, %edi
	addl    $0x1000, %eax
	loop    .L_fill_high_pde

	/*
	* Identity-map the same 256MB through PDE[0..63].  The first 4MB
	* keeps the instruction stream valid across the CR0.PG write; the
	* rest keeps every physical pointer the pre-HAL loader handed out
	* (script arena, kernel-load destinations, VRAM) usable unchanged.
	*/
	movl    $(initial_pdt - SYS_START + 0 * 4), %edi
	movl    $(initial_pts - SYS_START + PDE_SYS), %eax
	movl    $(NUM_HIGH_PDE), %ecx
  .L_fill_identity_pde:
	movl    %eax, (%edi)
	addl    $4, %edi
	addl    $0x1000, %eax
	loop    .L_fill_identity_pde

	/*
	* Identity-map the Cirrus linear aperture: PTEs for
	* 0xf0000000-0xf0ffffff, then PDE[960..963].
	*/
	movl    $(aperture_pts - SYS_START), %edi
	movl    $(0xf0000000 + PTE_APERTURE), %eax
	movl    $(NUM_APERTURE_PDE * 1024), %ecx
  .L_fill_aperture_pte:
	stosl
	addl    $0x1000, %eax
	loop    .L_fill_aperture_pte
	movl    $(initial_pdt - SYS_START + 960 * 4), %edi
	movl    $(aperture_pts - SYS_START + PDE_SYS), %eax
	movl    $(NUM_APERTURE_PDE), %ecx
  .L_fill_aperture_pde:
	movl    %eax, (%edi)
	addl    $4, %edi
	addl    $0x1000, %eax
	loop    .L_fill_aperture_pde

	/* Map the architectural LAPIC and I/O APIC MMIO window uncached. */
	movl    $(apic_pts - SYS_START), %edi
	movl    $(0xfec00000 + PTE_APERTURE), %eax
	movl    $1024, %ecx
  .L_fill_apic_pte:
	stosl
	addl    $0x1000, %eax
	loop    .L_fill_apic_pte
	movl    $(initial_pdt - SYS_START + APIC_PDE * 4), %edi
	movl    $(apic_pts - SYS_START + PDE_SYS), %eax
	movl    %eax, (%edi)

	/*
	* Load CR3 with the physical address of PDT.
	*/
	movl    $(initial_pdt - SYS_START), %eax
	movl    %eax, %cr3

	/*
	* Enable paging by setting CR0.PG.
	*/
	movl    %cr0, %eax
	orl    $0x80000000, %eax
	movl    %eax, %cr0

	/*
	* Jump to the higher-half address.
	*
	* After this point, code runs at virtual address 0x80000000+.
	*/
	ljmp    $(SEG_SYS_CODE), $(.L_sys_area)
    .L_sys_area:
	/* The bootstrap GDTR used the physical alias before paging.  User
	 * address spaces deliberately omit that low mapping, so keep the GDT
	 * reachable through the supervisor-only higher-half mapping. */
	lgdt	gdtr_high

	/*
	 * Set startup stack in higher-half virtual address space.
	 */
	movl    $(ADDR_INIT_STACK + SIZE_INIT_STACK + SYS_START), %esp

/*
 * Setup TSS.
 */
init_tss:

	/* TSSデスクリプタのベースフィールドを設定する */
	movl	$(gdt + SEG_TSS), %edi
	movl	$(tss_area), %eax
	movb	%al, 2(%edi)
	shrl	$8, %eax
	movb	%al, 3(%edi)
	shrl	$8, %eax
	movb	%al, 4(%edi)
	shrl	$8, %eax;
	movb	%al, 7(%edi)

	movl	$(SEG_TSS), %eax
	ltr 	%ax

/*
 * 一時的なIDTを作成する
 */
create_idt:

	/* エントリを作成する */
	movl	$(ADDR_IDT + SYS_START), %edi
	movl	$(on_bad_int), %eax
	movl	%eax, %ebx;	shrl	$16, %ebx
	movw	%ax,				0(%edi)
	movw	$(SEG_SYS_CODE),	2(%edi)
	movb	$0,					4(%edi)
	movb	$(0x8e),			5(%edi)
	movw	%bx,				6(%edi)
	movl	0(%edi), %eax
	movl	4(%edi), %ebx

	/* エントリを256個分コピーする */
	movl	$(ADDR_IDT + SYS_START), %edi
	movl	$256, %ecx
  .L_idt:
	movl	%eax, 0(%edi)
	movl	%ebx, 4(%edi)
	addl	$8, %edi
	loop .L_idt

	/* IDTRにロードする */
	movl	%esp, %ebx
	subl	$8, %ebx
	movw	$(256*8-1), (%ebx)
	movl	$(ADDR_IDT + SYS_START), 2(%ebx)
	lidt	(%ebx)

/*
 * Cのコードにジャンプする
 */
jump_to_extern_c:

	/*
	 * Enter ordinary C on the retained 64 KiB kernel boot stack.  Stage 1's
	 * physical handoff pointer was parked at ADDR_BOOT_INFO before paging;
	 * pass it through the C ABI instead of making the kernel recover it from
	 * a magic address.
	 */
	movl	$__kernel_stack_top, %esp
	xorl	%ebp, %ebp
	pushl	(ADDR_BOOT_INFO + SYS_START)
	call	cmain
  .L_cmain_returned:
	cli
	hlt
	jmp	.L_cmain_returned

#if defined(HAL_BOARD_PCAT)
.L_bad_multiboot:
	cli
	hlt
	jmp	.L_bad_multiboot
#endif

/*
 * Print message and halt.
 *  ESI = message
 */
on_panic:
	call print_message
  .L_on_panic_hlt_loop:
	hlt
	jmp .L_on_panic_hlt_loop

/*
 * Print message.
 *  ESI = message
 */
print_message:
#if defined(HAL_BOARD_PCAT)
	movl $0xb8000, %edi
	movb $0x0f, %al
	cld
    .L_msg_copy_loop:
	cmpb $0, (%esi)
	jz .L_msg_copy_end
	movsb
	stosb
    .L_msg_copy_end:
	ret
#endif
#if defined(HAL_BOARD_PC98)
	movl $0xa0000, %edi
	xor %al, %al
	cld
    .L_msg_copy_loop:
	cmpb $0, (%esi)
	jz .L_msg_copy_end
	movsb
	stosb
    .L_msg_copy_end:
	ret
#endif

/*
 * Initial interrupt handler.
 */
on_bad_int:
	movl	$(msg_bad_int), %esi
	jmp		on_panic

/*
 * extern "C" linkage procedures
 */

/* void asm_cli() */
	.global EXT_C(asm_cli)
EXT_C(asm_cli):
	cli
	ret

/* void asm_sti() */
	.global EXT_C(asm_sti)
EXT_C(asm_sti):
	sti
	ret

/* void asm_outb(uint16_t port, uint8_t data) */
	.global EXT_C(asm_outb)
EXT_C(asm_outb):
	pushl	%ebp
	movl	%esp, %ebp

	movl	 8(%ebp), %edx	/* port */
	movl	12(%ebp), %eax	/* data */

	outb	%al, %dx

	popl	%ebp
	ret

/* void asm_outw(uint16_t port, uint8_t data) */
	.global EXT_C(asm_outw)
EXT_C(asm_outw):
	pushl	%ebp
	movl	%esp, %ebp

	movl	 8(%ebp), %edx	/* port */
	movl	12(%ebp), %eax	/* data */

	outw	%ax, %dx

	popl	%ebp
	ret

/* uint8_t asm_inb(uint16_t port) */
	.global EXT_C(asm_inb)
EXT_C(asm_inb):
	pushl	%ebp
	movl	%esp, %ebp

	movl	8(%ebp), %edx	/* port */

	xorl	%eax, %eax
	inb		%dx, %al

	popl	%ebp
	ret

/* uint8_t asm_inw(uint16_t port) */
	.global EXT_C(asm_inw)
EXT_C(asm_inw):
	pushl	%ebp
	movl	%esp, %ebp

	movl	8(%ebp), %edx	/* port */

	xorl	%eax, %eax
	inw		%dx, %ax

	popl	%ebp
	ret

/* void asm_fnsave(void *save_area) */
	.global EXT_C(asm_fnsave)
EXT_C(asm_fnsave):
	movl	4(%esp), %eax
	fnsave	(%eax)
	ret

/* void asm_frstor(void *save_area) */
	.global EXT_C(asm_frstor)
EXT_C(asm_frstor):
	movl	4(%esp), %eax
	frstor	(%eax)
	ret

/* uint32_t asm_get_eflags() */
	.global EXT_C(asm_get_eflags)
EXT_C(asm_get_eflags):
	pushf
	popl	%eax
	ret

/* uint32_t asm_get_eip() */
	.global EXT_C(asm_get_eip)
EXT_C(asm_get_eip):
	call .L_eip
  .L_eip:
	popl	%eax
	ret

/* uint32_t asm_get_esp() */
	.global EXT_C(asm_get_esp)
EXT_C(asm_get_esp):
	pushl	%esp
	popl	%eax
	ret

/* uint32_t asm_get_cr3() */
.global EXT_C(asm_get_cr2)
EXT_C(asm_get_cr2):
	movl	%cr2, %eax
	ret

/* uint32_t asm_get_cr3() */
	.global EXT_C(asm_get_cr3)
EXT_C(asm_get_cr3):
	movl	%cr3, %eax
	ret

/* uint32_t asm_set_cr3() */
	.global EXT_C(asm_set_cr3)
EXT_C(asm_set_cr3):
	movl	4(%esp), %eax
	movl	%eax, %cr3
	ret

/* void asm_hlt() */
	.global EXT_C(asm_hlt)
EXT_C(asm_hlt):
	hlt
	ret

/* void asm_lidt(void *idt_desc) */
	.global EXT_C(asm_lidt)
EXT_C(asm_lidt):
	movl	4(%esp), %eax
	lidt	(%eax)
	ret

/* void asm_load_pdt(uint32_t addr) */
	.global EXT_C(asm_load_cr3)
EXT_C(asm_load_cr3):
	movl	4(%esp), %eax
	movl	%eax, %cr3
	ret

/* void asm_flash_tlb() */
	.global EXT_C(asm_flash_tlb)
EXT_C(asm_flash_tlb):
	movl	%cr3, %eax
	movl	%eax, %cr3
	ret

/*
 * Messages
 */
msg_boot:			.ascii "Kernel booted.\0"
msg_cpuid_not_supported:	.ascii "Unsupported old CPU! (CPUID not supported)\0"
msg_pse_not_supported:		.ascii "Unsupported old CPU! (PSE not supported)\0"
msg_fpu_not_implemented:	.ascii "Unsupported old CPU! (on-chip FPU not implemented)\0"
msg_bad_int:			.ascii "Bad Interrupt\0"

/*
 * GDT (Direct Mapped)
 */
gdt:
	.byte 0, 0, 0, 0, 0, 0, 0, 0			/* 0x0000: ignored */
	.byte 0xff, 0xff, 0, 0, 0, 0x9a, 0xcf, 0	/* 0x0008: sys code */
	.byte 0xff, 0xff, 0, 0, 0, 0x92, 0xcf, 0	/* 0x0010: sys data */
	.byte 0xff, 0xff, 0, 0, 0, 0xf8, 0xcf, 0	/* 0x0018: user code */
	.byte 0xff, 0xff, 0, 0, 0, 0xf2, 0xcf, 0	/* 0x0020: user data */
	.byte 103,  0,    0, 0, 0, 0x89, 0,    0	/* 0x0028: TSS */
gdt_end:

/*
 * GDTR
 */
gdtr:
	.word gdt_end - gdt - 1
	.long gdt - 0x80000000

gdtr_high:
	.word gdt_end - gdt - 1
	.long gdt

/*
 * TSS
 */
	.global EXT_C(tss_area)
EXT_C(tss_area):
	.space 104		/* TODO: CPUごとに用意 */

/*
 * small work area
 */
work_area:
	.space 256

/*
 * Initial page tables (see the note at the top of this file).
 */
	.section .bss
	.balign 4096
initial_pdt:
	.space 4096
initial_pts:
	.space NUM_HIGH_PDE * 4096
aperture_pts:
	.space NUM_APERTURE_PDE * 4096
apic_pts:
	.space 4096

	.balign 16
__kernel_stack:
	/*
	 * Noct's i386 JIT can nest through the runtime, libc services, and the
	 * kernel command path deeply enough to use about 49 KiB.  The old link
	 * layout accidentally supplied spill room below its nominal 32 KiB
	 * stack; keep that memory explicit so it cannot corrupt the adjacent
	 * Cirrus aperture page tables when object placement changes.
	 */
	.space 65536
	.globl __kernel_stack_top
__kernel_stack_top:
