/*
 * Executable X68000/MC68030 bootstrap contract.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * Native MC68030 entry.  It builds short descriptors, loads SRP and an empty
 * CRP, enables SRE, enters the high direct map, removes the temporary identity
 * map, clears the kernel BSS, and calls the X68k C bootstrap.  Stage 2 supplies
 * A0 = struct zedbsd_x68k_handoff.
 */

#define DIRECT_BASE             0x80000000
#define PAGE_SIZE               0x00001000
#define PAGE_DESCRIPTOR         0x00000001
#define WRITE_PROTECT           0x00000004
#define CACHE_INHIBIT           0x00000040
#define SHORT_TABLE_DESCRIPTOR  0x00000002
#define ROOT_ATTR_1024_SHORT    0x03ff0002
#define TC_4K_10_10_SRE         0x82c0aa00
#define CACR_ENABLE_CLEAR       0x00003909
#define X68K_HANDOFF_RAM_BYTES  32
#define X68K_MAX_INITIAL_RAM    0x00c00000

	.section .bootstrap,"ax"
	.globl _start
	.type _start,@function
_start:
	move.w #0x2700,%sr
	move.l %a0,%a5

	/* All bootstrap tables are physical low-memory objects at this point. */
	lea m68k030_bootstrap_tables,%a0
	move.w #7167,%d0             /* seven 4-KiB pages, in longwords */
1:	clr.l (%a0)+
	dbra %d0,1b

	/* Keep the first MiB temporarily visible for loader stack and handoff. */
	lea m68k030_bootstrap_identity_leaf,%a0
	clr.l %d0
	move.w #255,%d1
2:	move.l %d0,%d2
	ori.l #PAGE_DESCRIPTOR,%d2
	move.l %d2,(%a0)+
	addi.l #PAGE_SIZE,%d0
	dbra %d1,2b

	/* The high direct map contains only RAM reported by stage 2, capped to the
	 * initial 12-MiB backend.  Its unused leaf descriptors remain invalid. */
	move.l X68K_HANDOFF_RAM_BYTES(%a5),%d3
	cmpi.l #X68K_MAX_INITIAL_RAM,%d3
	bls.s 3f
	move.l #X68K_MAX_INITIAL_RAM,%d3
3:	andi.l #0xfffff000,%d3
	lsr.l #8,%d3
	lsr.l #4,%d3                 /* byte length -> page count */
	beq.s 5f
	subq.l #1,%d3
	lea m68k030_bootstrap_direct_leaf,%a0
	clr.l %d0
4:	move.l %d0,%d2
	ori.l #PAGE_DESCRIPTOR,%d2
	move.l %d2,(%a0)+
	addi.l #PAGE_SIZE,%d0
	dbra %d3,4b

5:	/* Fourth direct-map leaf: map only documented device apertures. */
	lea m68k030_bootstrap_direct_leaf+3*PAGE_SIZE,%a0
	move.l #0x00c00000+CACHE_INHIBIT+PAGE_DESCRIPTOR,%d0
	move.w #639,%d1             /* G-VRAM and text VRAM through 0xe7ffff */
7:	move.l %d0,(%a0)+
	addi.l #PAGE_SIZE,%d0
	dbra %d1,7b

	lea m68k030_bootstrap_direct_leaf+3*PAGE_SIZE,%a0
	move.l #0x00e80000+CACHE_INHIBIT+PAGE_DESCRIPTOR,640*4(%a0)
	move.l #0x00e82000+CACHE_INHIBIT+PAGE_DESCRIPTOR,642*4(%a0)
	move.l #0x00e84000+CACHE_INHIBIT+PAGE_DESCRIPTOR,644*4(%a0)
	move.l #0x00e86000+CACHE_INHIBIT+PAGE_DESCRIPTOR,646*4(%a0)
	move.l #0x00e88000+CACHE_INHIBIT+PAGE_DESCRIPTOR,648*4(%a0)
	move.l #0x00e8a000+CACHE_INHIBIT+PAGE_DESCRIPTOR,650*4(%a0)
	move.l #0x00e8e000+CACHE_INHIBIT+PAGE_DESCRIPTOR,654*4(%a0)
	move.l #0x00e94000+CACHE_INHIBIT+PAGE_DESCRIPTOR,660*4(%a0)
	move.l #0x00e96000+CACHE_INHIBIT+PAGE_DESCRIPTOR,662*4(%a0)
	move.l #0x00e98000+CACHE_INHIBIT+PAGE_DESCRIPTOR,664*4(%a0)
	move.l #0x00e9c000+CACHE_INHIBIT+PAGE_DESCRIPTOR,668*4(%a0)
	move.l #0x00ed0000+CACHE_INHIBIT+PAGE_DESCRIPTOR,720*4(%a0)
	move.l #0x00ed1000+CACHE_INHIBIT+PAGE_DESCRIPTOR,721*4(%a0)
	move.l #0x00ed2000+CACHE_INHIBIT+PAGE_DESCRIPTOR,722*4(%a0)
	move.l #0x00ed3000+CACHE_INHIBIT+PAGE_DESCRIPTOR,723*4(%a0)
	lea 768*4(%a0),%a0
	move.l #0x00f00000+CACHE_INHIBIT+WRITE_PROTECT+PAGE_DESCRIPTOR,%d0
	move.w #255,%d1
8:	move.l %d0,(%a0)+
	addi.l #PAGE_SIZE,%d0
	dbra %d1,8b

	lea m68k030_bootstrap_root,%a0
	move.l #m68k030_bootstrap_identity_leaf+SHORT_TABLE_DESCRIPTOR,0(%a0)
	move.l #m68k030_bootstrap_direct_leaf+SHORT_TABLE_DESCRIPTOR,2048(%a0)
	move.l #m68k030_bootstrap_direct_leaf+PAGE_SIZE+SHORT_TABLE_DESCRIPTOR,2052(%a0)
	move.l #m68k030_bootstrap_direct_leaf+2*PAGE_SIZE+SHORT_TABLE_DESCRIPTOR,2056(%a0)
	move.l #m68k030_bootstrap_direct_leaf+3*PAGE_SIZE+SHORT_TABLE_DESCRIPTOR,2060(%a0)

	/* MC68030UM MMU register setup: roots while disabled, flush, then TC. */
	lea m68k030_bootstrap_srp,%a0
	pmove (%a0),%srp
	lea m68k030_bootstrap_crp,%a0
	pmove (%a0),%crp
	pflush #0,#0
	lea m68k030_bootstrap_tc,%a0
	pmove (%a0),%tc

	/* This is intentionally an absolute high transfer, never a return. */
	jmp m68k_contract_high_entry
	.size _start,.-_start

	.balign PAGE_SIZE
	.globl m68k030_bootstrap_tables
m68k030_bootstrap_tables:
	.globl m68k030_bootstrap_root
m68k030_bootstrap_root:
	.space PAGE_SIZE
	.globl m68k030_bootstrap_empty_root
m68k030_bootstrap_empty_root:
	.space PAGE_SIZE
	.globl m68k030_bootstrap_identity_leaf
m68k030_bootstrap_identity_leaf:
	.space PAGE_SIZE
	.globl m68k030_bootstrap_direct_leaf
m68k030_bootstrap_direct_leaf:
	.space 4 * PAGE_SIZE
m68k030_bootstrap_tables_end:

	.balign 8
m68k030_bootstrap_srp:
	.long ROOT_ATTR_1024_SHORT
	.long m68k030_bootstrap_root
m68k030_bootstrap_crp:
	.long ROOT_ATTR_1024_SHORT
	.long m68k030_bootstrap_empty_root
m68k030_bootstrap_tc:
	.long TC_4K_10_10_SRE

	.section .text,"ax"
	.globl m68k_contract_high_entry
	.type m68k_contract_high_entry,@function
m68k_contract_high_entry:
	/* Switch the inherited loader SSP to its high direct-map alias. */
	move.l %sp,%d0
	ori.l #DIRECT_BASE,%d0
	move.l %d0,%sp
	move.l %a5,%d0
	ori.l #DIRECT_BASE,%d0
	move.l %d0,%a5

	/* Remove low aliases before any C/kernel work can retain low pointers. */
	clr.l m68k030_bootstrap_root + DIRECT_BASE
	pflush #0,#0
	move.l #CACR_ENABLE_CLEAR,%d0
	movec %d0,%cacr

	/* C starts with the language-required zero-initialized BSS.  Continue on
	 * the inherited stage-2 stack until the BSS-resident kernel stack is clear. */
	lea __bss_start,%a0
	lea __bss_end,%a1
6:	cmpa.l %a1,%a0
	bcc.s 7f
	clr.l (%a0)+
	bra.s 6b
7:	lea m68k_bootstrap_stack_top,%sp
	move.l %a5,-(%sp)
	jsr m68k_x68k_cmain
	addq.l #4,%sp
8:	stop #0x2700
	bra.s 8b
	.size m68k_contract_high_entry,.-m68k_contract_high_entry

	.section .rodata,"a"
m68k_contract_name:
	.asciz "zedBSD MC68030 executable MMU contract"

	.section .data,"aw"
	.globl m68k_contract_word
m68k_contract_word:
	.long 0

	.section .bss.bootstrap_stack,"aw",@nobits
	.balign 16
	.globl m68k_bootstrap_stack
m68k_bootstrap_stack:
	.space 16384
	.globl m68k_bootstrap_stack_top
m68k_bootstrap_stack_top:
