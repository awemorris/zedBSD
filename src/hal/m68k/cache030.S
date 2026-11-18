/* Conservative MC68030 cache operations. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#define CACR_IC_ENABLE 0x0001
#define CACR_IC_CLEAR  0x0008
#define CACR_IC_BURST  0x0010
#define CACR_DC_ENABLE 0x0100
#define CACR_DC_CLEAR  0x0800
#define CACR_DC_BURST  0x1000
#define CACR_DC_WALLOC 0x2000
#define CACR_ENABLES      0x3101
#define CACR_CLEAR_ALL    0x3909
#define CACR_CLEAR_ICACHE 0x3109
#define CACR_CLEAR_DCACHE 0x3901

	.text
	.even

	.globl m68k030_cache_enable
	.type m68k030_cache_enable,@function
m68k030_cache_enable:
	move.l #CACR_CLEAR_ALL,%d0
	movec %d0,%cacr
	rts
	.size m68k030_cache_enable,.-m68k030_cache_enable

	.globl m68k030_cache_clear_all
	.type m68k030_cache_clear_all,@function
m68k030_cache_clear_all:
	move.l #CACR_CLEAR_ALL,%d0
	movec %d0,%cacr
	rts
	.size m68k030_cache_clear_all,.-m68k030_cache_clear_all

	.globl m68k030_icache_clear_all
	.type m68k030_icache_clear_all,@function
m68k030_icache_clear_all:
	move.l #CACR_CLEAR_ICACHE,%d0
	movec %d0,%cacr
	rts
	.size m68k030_icache_clear_all,.-m68k030_icache_clear_all

	.globl m68k030_dcache_clear_all
	.type m68k030_dcache_clear_all,@function
m68k030_dcache_clear_all:
	move.l #CACR_CLEAR_DCACHE,%d0
	movec %d0,%cacr
	rts
	.size m68k030_dcache_clear_all,.-m68k030_dcache_clear_all
