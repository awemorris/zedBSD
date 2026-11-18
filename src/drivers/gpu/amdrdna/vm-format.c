/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The words of the GPU's address translation: where the table that maps
 * system memory sits in the GPU's address space, the entries of that table
 * and of a context's page tables, the request that drops stale
 * translations, the control words of the memory hubs, and what a page
 * fault reports.
 *
 * Context 0 (the kernel's) maps system memory through one flat table; the
 * other contexts walk four levels of 512-entry tables, from the root to
 * the leaf.  Nothing here touches the hardware, so the same code runs in
 * the kernel and in the host tests.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"

/*
 * Where the table that maps system memory may go: below the 2^47 hole of
 * the GPU's 48-bit address space, on a 4 GiB boundary, before the frame
 * buffer or after it, whichever leaves the larger room.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_gmc.h (AMDGPU_GMC_HOLE_START),
 * drivers/gpu/drm/amd/amdgpu/gmc_v10_0.c (the 48-bit address mask) and
 * drivers/gpu/drm/amd/amdgpu/amdgpu_gmc.c (amdgpu_gmc_gart_location: the
 * 4 GiB alignment and the best-fit rule).
 */
#define FORMAT_ADDRESS_TOP		0x00007fffffffffffULL
#define FORMAT_GART_ALIGNMENT		0x0000000100000000ULL

/*
 * The levels of a context's address space: each level's index takes nine
 * bits of the address, the leaf's starting at bit 12.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gmc_v10_0.c (gmc_v10_0_sw_init: 48-bit, four
 * levels, 9-bit blocks) and amdgpu_vm_pt.c (amdgpu_vm_pt_level_shift).
 */
#define FORMAT_LEVEL_BITS		9U
#define FORMAT_LEVEL_MASK		0x1ffU
#define FORMAT_PAGE_SHIFT		12U

/*
 * The invalidation request: one bit per context in bits 15:0, the flush
 * type in bits 18:16 (0, a heavyweight flush), and the bits that drop the
 * cached leaf entries, the three directory levels and the first-level
 * translation (bits 19 to 23).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_sh_mask.h
 * (GCVM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PTES to
 * __INVALIDATE_L1_PTES) and drivers/gpu/drm/amd/amdgpu/gfxhub_v2_0.c
 * (gfxhub_v2_0_get_invalidate_req).
 */
#define FORMAT_INVALIDATE_ALL_LEVELS	0x00f80000U
#define FORMAT_INVALIDATE_CONTEXT_MASK	0x0000ffffU

/*
 * The fields of the hub's second-level cache control words: the cache
 * enable, fragment processing, the directory tag mode, the default page
 * sent to system memory, the fault classification of directories, the
 * identity access mode of context 1 (bits 20:19) and the identity
 * fragment size (bits 25:21); the two flush bits of the second word; the
 * bank select (bits 5:0) and large-fragment size (bits 19:15) of the
 * third, whose reset value is 0x80100007; the two physical-request bits of
 * the fourth (reset 0xc1); the small-fragment size (bits 4:0) of the fifth
 * (reset 0x3fe0).  Both hubs lay them out the same way.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_sh_mask.h
 * (GCVM_L2_CNTL__ENABLE_L2_CACHE, __ENABLE_L2_FRAGMENT_PROCESSING,
 * __L2_PDE0_CACHE_TAG_GENERATION_MODE,
 * __ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY, __PDE_FAULT_CLASSIFICATION,
 * __CONTEXT1_IDENTITY_ACCESS_MODE, __IDENTITY_MODE_FRAGMENT_SIZE;
 * GCVM_L2_CNTL2__INVALIDATE_ALL_L1_TLBS, __INVALIDATE_L2_CACHE;
 * GCVM_L2_CNTL3__BANK_SELECT, __L2_CACHE_BIGK_FRAGMENT_SIZE;
 * GCVM_L2_CNTL4__VMC_TAP_PDE_REQUEST_PHYSICAL,
 * __VMC_TAP_PTE_REQUEST_PHYSICAL; GCVM_L2_CNTL5__L2_CACHE_SMALLK_FRAGMENT_SIZE),
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_default.h
 * (mmGCVM_L2_CNTL3_DEFAULT, mmGCVM_L2_CNTL4_DEFAULT,
 * mmGCVM_L2_CNTL5_DEFAULT) and drivers/gpu/drm/amd/amdgpu/gfxhub_v2_0.c
 * (gfxhub_v2_0_init_cache_regs: the values chosen when no further
 * translation is used: bank 9, large fragment 6, identity access mode 1).
 */
#define FORMAT_CACHE_ENABLE		0x00000001U
#define FORMAT_CACHE_FRAGMENTS		0x00000002U
#define FORMAT_CACHE_DIRECTORY_TAGS	0x00000100U
#define FORMAT_CACHE_DEFAULT_TO_SYSTEM	0x00000800U
#define FORMAT_CACHE_DIRECTORY_FAULTS	0x00040000U
#define FORMAT_CACHE_IDENTITY_MODE_MASK	0x00180000U
#define FORMAT_CACHE_IDENTITY_MODE_ONE	0x00080000U
#define FORMAT_CACHE_IDENTITY_SIZE_MASK	0x03e00000U
#define FORMAT_CACHE_FLUSH_FIRST_LEVEL	0x00000001U
#define FORMAT_CACHE_FLUSH_SECOND_LEVEL	0x00000002U
#define FORMAT_CACHE_3_RESET		0x80100007U
#define FORMAT_CACHE_3_BANK_MASK	0x0000003fU
#define FORMAT_CACHE_3_BANK		0x00000009U
#define FORMAT_CACHE_3_LARGE_MASK	0x000f8000U
#define FORMAT_CACHE_3_LARGE		0x00030000U
#define FORMAT_CACHE_4_RESET		0x000000c1U
#define FORMAT_CACHE_4_PHYSICAL_MASK	0x000000c0U
#define FORMAT_CACHE_5_RESET		0x00003fe0U
#define FORMAT_CACHE_5_SMALL_MASK	0x0000001fU

/*
 * The fields of the hub's first-level translation control word: the
 * enable, the system access mode (bits 4:3, 3 for the driver's model), the
 * unmapped-aperture access bit, the advanced driver model, and the memory
 * type (bits 13:11, 3 uncached).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_sh_mask.h
 * (GCMC_VM_MX_L1_TLB_CNTL__ENABLE_L1_TLB, __SYSTEM_ACCESS_MODE,
 * __SYSTEM_APERTURE_UNMAPPED_ACCESS, __ENABLE_ADVANCED_DRIVER_MODEL,
 * __MTYPE), drivers/gpu/drm/amd/include/navi10_enum.h (MTYPE_UC) and
 * drivers/gpu/drm/amd/amdgpu/gfxhub_v2_0.c (gfxhub_v2_0_init_tlb_regs).
 */
#define FORMAT_TRANSLATE_ENABLE		0x00000001U
#define FORMAT_TRANSLATE_MODE_MASK	0x00000018U
#define FORMAT_TRANSLATE_MODE_DRIVER	0x00000018U
#define FORMAT_TRANSLATE_UNMAPPED	0x00000020U
#define FORMAT_TRANSLATE_ADVANCED	0x00000040U
#define FORMAT_TRANSLATE_TYPE_MASK	0x00003800U
#define FORMAT_TRANSLATE_TYPE_UNCACHED	0x00001800U

/*
 * The fields of a context's control word: the enable, the table depth
 * (bits 2:1), the table block size (bits 6:3, 0 for 512 entries), the
 * retry-on-fault bit, and seven pairs of bits, one pair per kind of fault
 * (range, dummy page, level-0 directory, valid, read, write, execute):
 * the lower bit raises an interrupt, the upper one sends the access to the
 * default page.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_sh_mask.h
 * (GCVM_CONTEXT1_CNTL__ENABLE_CONTEXT, __PAGE_TABLE_DEPTH,
 * __PAGE_TABLE_BLOCK_SIZE, __RETRY_PERMISSION_OR_INVALID_PAGE_FAULT,
 * __RANGE_PROTECTION_FAULT_ENABLE_INTERRUPT to
 * __EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT) and
 * drivers/gpu/drm/amd/amdgpu/gfxhub_v2_0.c
 * (gfxhub_v2_0_setup_vmid_config, gfxhub_v2_0_enable_system_domain and
 * the fault interrupt mask set up in gfxhub_v2_0_init).
 */
#define FORMAT_CONTEXT_ENABLE		0x00000001U
#define FORMAT_CONTEXT_DEPTH_MASK	0x00000006U
#define FORMAT_CONTEXT_DEPTH_FOUR	0x00000006U
#define FORMAT_CONTEXT_BLOCK_MASK	0x00000078U
#define FORMAT_CONTEXT_RETRY		0x00000080U
#define FORMAT_CONTEXT_DEFAULTS		0x00555400U
#define FORMAT_CONTEXT_INTERRUPTS	0x002aaa00U

/*
 * The fields of the hub's fault control words: the bit that clears the
 * latched fault so the next one is recorded, the eleven bits (2 to 12)
 * that send a faulting access to the default page, and, in the second
 * word, the retry of a page-migration entry read.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_sh_mask.h
 * (GCVM_L2_PROTECTION_FAULT_CNTL__CLEAR_PROTECTION_FAULT_STATUS_ADDR,
 * __RANGE_PROTECTION_FAULT_ENABLE_DEFAULT to
 * __EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT;
 * GCVM_L2_PROTECTION_FAULT_CNTL2__ACTIVE_PAGE_MIGRATION_PTE_READ_RETRY)
 * and drivers/gpu/drm/amd/amdgpu/gfxhub_v2_0.c
 * (gfxhub_v2_0_set_fault_enable_default,
 * gfxhub_v2_0_init_system_aperture_regs).
 */
#define FORMAT_FAULT_REARM		0x00000001U
#define FORMAT_FAULT_DEFAULTS		0x00001ffcU
#define FORMAT_FAULT_MIGRATION_RETRY	0x00040000U

/*
 * What a fault interrupt's second data word says about the access (retry,
 * read, write, execute; its low four bits are address bits 47:44), and the
 * fields of the hub's latched fault word: more faults, the walker error
 * (bits 3:1), the failed permissions (bits 7:4), a mapping error, the
 * client unit (bits 17:9), a write, and the context (bits 23:20).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_gmc.h
 * (AMDGPU_GMC9_FAULT_SOURCE_DATA_RETRY, _READ, _WRITE, _EXE),
 * drivers/gpu/drm/amd/amdgpu/gmc_v10_0.c (gmc_v10_0_process_interrupt:
 * the address from the first two data words) and
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_sh_mask.h
 * (GCVM_L2_PROTECTION_FAULT_STATUS__MORE_FAULTS, __WALKER_ERROR,
 * __PERMISSION_FAULTS, __MAPPING_ERROR, __CID, __RW, __VMID).
 */
#define FORMAT_ACCESS_RETRY		0x00000080U
#define FORMAT_ACCESS_READ		0x00000040U
#define FORMAT_ACCESS_WRITE		0x00000020U
#define FORMAT_ACCESS_EXECUTE		0x00000010U
#define FORMAT_ACCESS_KIND_MASK		0x000000f0U
#define FORMAT_ACCESS_HIGH_MASK		0x0000000fU
#define FORMAT_ACCESS_HIGH_SHIFT	44U
#define FORMAT_LATCH_MORE		0x00000001U
#define FORMAT_LATCH_WALKER_SHIFT	1U
#define FORMAT_LATCH_WALKER_MASK	0x00000007U
#define FORMAT_LATCH_DENIED_SHIFT	4U
#define FORMAT_LATCH_DENIED_MASK	0x0000000fU
#define FORMAT_LATCH_UNMAPPED		0x00000100U
#define FORMAT_LATCH_UNIT_SHIFT		9U
#define FORMAT_LATCH_UNIT_MASK		0x000001ffU
#define FORMAT_LATCH_WROTE		0x00040000U
#define FORMAT_LATCH_CONTEXT_SHIFT	20U
#define FORMAT_LATCH_CONTEXT_MASK	0x0000000fU

/*
 * Finds where the table that maps system memory goes in the GPU's address
 * space, given the frame buffer's place [frame_buffer_start,
 * frame_buffer_end] and the table's reach in bytes.
 *
 * The range goes below the frame buffer when that room holds it and is the
 * smaller of the two rooms (or the room above does not hold it), else at
 * the top of the space; then it is aligned down to 4 GiB.  Returns 0 with
 * the range's start, or ENOSPC when neither room holds it.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_gmc.c (amdgpu_gmc_gart_location, the
 * best-fit placement).
 */
int
amdrdna_gart_place(
	uint64_t frame_buffer_start,
	uint64_t frame_buffer_end,
	uint64_t bytes,
	uint64_t *start)
{
	uint64_t room_below;
	uint64_t room_above;
	uint64_t above_start;

	/* A frame buffer past the usable space, or an empty reach, has no answer. */
	if (bytes == 0U)
		return ENOSPC;
	if (frame_buffer_end >= FORMAT_ADDRESS_TOP)
		return ENOSPC;

	/* The room below the frame buffer and the room above its next 4 GiB boundary. */
	room_below = frame_buffer_start;
	above_start = (frame_buffer_end + FORMAT_GART_ALIGNMENT) & ~(FORMAT_GART_ALIGNMENT - 1U);
	room_above = 0U;
	if (above_start <= FORMAT_ADDRESS_TOP)
		room_above = FORMAT_ADDRESS_TOP + 1U - above_start;

	/* A reach neither room holds is refused. */
	if (bytes > room_below && bytes > room_above)
		return ENOSPC;

	/* Below when that room is the snug one, or the only one; else at the top. */
	if (room_below >= bytes && room_below < room_above) {
		*start = 0U;
	} else if (room_above < bytes) {
		*start = 0U;
	} else {
		*start = (FORMAT_ADDRESS_TOP - bytes + 1U) & ~(FORMAT_GART_ALIGNMENT - 1U);
	}

	/* Succeeded: the range starts at a 4 GiB boundary. */
	return 0;
}

/*
 * Makes the entry of the system-memory table that maps one page of system
 * memory: present, in system memory, coherent with the CPU's caches,
 * readable, writable, executable and uncached.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gmc_v10_0.c (gmc_v10_0_gart_init: uncached
 * and executable) and amdgpu_ttm.c (amdgpu_ttm_tt_pde_flags,
 * amdgpu_ttm_tt_pte_flags: system, snooped for cached pages, readable,
 * writable).
 */
uint64_t
amdrdna_gart_system_entry(
	uint64_t bus_address)
{
	uint64_t entry;

	/* The page's address with the bits of a present system page. */
	entry = bus_address & AMDRDNA_PAGE_ADDRESS_MASK;
	entry |= AMDRDNA_PAGE_PRESENT;
	entry |= AMDRDNA_PAGE_SYSTEM;
	entry |= AMDRDNA_PAGE_COHERENT;
	entry |= AMDRDNA_PAGE_READ;
	entry |= AMDRDNA_PAGE_WRITE;
	entry |= AMDRDNA_PAGE_EXECUTE;
	entry |= AMDRDNA_PAGE_TYPE_UNCACHED << AMDRDNA_PAGE_TYPE_SHIFT;

	/* Succeeded: the entry. */
	return entry;
}

/*
 * Makes the entry of the system-memory table for a page nothing is mapped
 * at: the dummy page's address with no bit set, so the hub sees no
 * present page.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_gart.c (amdgpu_gart_unbind: the dummy
 * page with no flags; the system bit must be clear for an invalid entry).
 */
uint64_t
amdrdna_gart_unused_entry(
	uint64_t dummy_address)
{
	uint64_t entry;

	/* Only the dummy page's address. */
	entry = dummy_address & AMDRDNA_PAGE_ADDRESS_MASK;

	/* Succeeded: an entry that maps nothing. */
	return entry;
}

/*
 * Makes the leaf entry of a context's page table: the page's address and
 * the access bits and memory type the caller chose (AMDRDNA_PAGE_ bits).
 */
uint64_t
amdrdna_vm_leaf_entry(
	uint64_t address,
	uint64_t access)
{
	uint64_t entry;

	/* The page's address, then the bits outside the address field. */
	entry = address & AMDRDNA_PAGE_ADDRESS_MASK;
	entry |= access & ~AMDRDNA_PAGE_ADDRESS_MASK;

	/* Succeeded: the leaf entry. */
	return entry;
}

/*
 * Makes the directory entry that points at the next level's table: the
 * table's address, present, and for a table in system memory the system
 * and coherent bits.
 */
uint64_t
amdrdna_vm_table_entry(
	uint64_t table_address,
	bool in_system)
{
	uint64_t entry;

	/* The table's address and the present bit. */
	entry = table_address & AMDRDNA_TABLE_ADDRESS_MASK;
	entry |= AMDRDNA_PAGE_PRESENT;

	/* A table in system memory is read through the CPU's coherent path. */
	if (in_system) {
		entry |= AMDRDNA_PAGE_SYSTEM;
		entry |= AMDRDNA_PAGE_COHERENT;
	}

	/* Succeeded: the directory entry. */
	return entry;
}

/*
 * Splits a context's address into the index it takes at each level of its
 * page tables, root first: bits 47:39, 38:30, 29:21 and 20:12.
 */
void
amdrdna_vm_split(
	uint64_t address,
	unsigned index[AMDRDNA_VM_LEVELS])
{
	unsigned level;
	unsigned shift;

	/* Each level takes nine bits, the leaf's lowest at bit 12. */
	for (level = 0U; level < AMDRDNA_VM_LEVELS; level++) {
		/* The root's bits are the highest. */
		shift = FORMAT_PAGE_SHIFT + FORMAT_LEVEL_BITS * (AMDRDNA_VM_LEVELS - 1U - level);
		index[level] = (unsigned)((address >> shift) & FORMAT_LEVEL_MASK);
	}
}

/*
 * Makes the invalidation request that drops every cached translation of
 * one context, at every level.
 */
uint32_t
amdrdna_invalidate_word(
	unsigned context)
{
	uint32_t word;

	/* The context's bit and every level's drop bit, with a heavyweight flush. */
	word = (1U << context) & FORMAT_INVALIDATE_CONTEXT_MASK;
	word |= FORMAT_INVALIDATE_ALL_LEVELS;

	/* Succeeded: the request word. */
	return word;
}

/*
 * Turns the hub's first second-level cache control word into the driver's:
 * the cache on, no fragment processing, the default page sent to system
 * memory, the plain directory tag mode and fault classification, identity
 * access mode 1 and no identity fragment size.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfxhub_v2_0.c (gfxhub_v2_0_init_cache_regs).
 */
uint32_t
amdrdna_hub_cache_word(
	uint32_t old)
{
	uint32_t word;

	/* Clears every field the driver sets. */
	word = old;
	word &= ~FORMAT_CACHE_FRAGMENTS;
	word &= ~FORMAT_CACHE_DIRECTORY_TAGS;
	word &= ~FORMAT_CACHE_DIRECTORY_FAULTS;
	word &= ~FORMAT_CACHE_IDENTITY_MODE_MASK;
	word &= ~FORMAT_CACHE_IDENTITY_SIZE_MASK;

	/* Turns the cache on and the default page out to system memory, identity mode 1. */
	word |= FORMAT_CACHE_ENABLE;
	word |= FORMAT_CACHE_DEFAULT_TO_SYSTEM;
	word |= FORMAT_CACHE_IDENTITY_MODE_ONE;

	/* Succeeded: the new control word. */
	return word;
}

/*
 * Turns the hub's second cache control word into one that also drops
 * every first-level translation and the second-level cache.
 */
uint32_t
amdrdna_hub_cache_flush_word(
	uint32_t old)
{
	uint32_t word;

	/* Both flush bits on top of what the word holds. */
	word = old;
	word |= FORMAT_CACHE_FLUSH_FIRST_LEVEL;
	word |= FORMAT_CACHE_FLUSH_SECOND_LEVEL;

	/* Succeeded: the new control word. */
	return word;
}

/*
 * Makes the hub's third cache control word: its reset value with bank
 * select 9 and a large-fragment size of 6.
 */
uint32_t
amdrdna_hub_cache_word_3(void)
{
	uint32_t word;

	/* The reset value with the two fields replaced. */
	word = FORMAT_CACHE_3_RESET;
	word &= ~FORMAT_CACHE_3_BANK_MASK;
	word &= ~FORMAT_CACHE_3_LARGE_MASK;
	word |= FORMAT_CACHE_3_BANK;
	word |= FORMAT_CACHE_3_LARGE;

	/* Succeeded: the control word. */
	return word;
}

/*
 * Makes the hub's fourth cache control word: its reset value with both
 * directory and leaf requests made through translation, not physically.
 */
uint32_t
amdrdna_hub_cache_word_4(void)
{
	uint32_t word;

	/* The reset value without the two physical-request bits. */
	word = FORMAT_CACHE_4_RESET;
	word &= ~FORMAT_CACHE_4_PHYSICAL_MASK;

	/* Succeeded: the control word. */
	return word;
}

/*
 * Makes the hub's fifth cache control word: its reset value with a
 * small-fragment size of 0.
 */
uint32_t
amdrdna_hub_cache_word_5(void)
{
	uint32_t word;

	/* The reset value without the small-fragment size. */
	word = FORMAT_CACHE_5_RESET;
	word &= ~FORMAT_CACHE_5_SMALL_MASK;

	/* Succeeded: the control word. */
	return word;
}

/*
 * Turns the hub's first-level translation control word into the driver's:
 * translation on, the driver's system access mode, the advanced driver
 * model, unmapped aperture accesses not let through, and uncached memory.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfxhub_v2_0.c (gfxhub_v2_0_init_tlb_regs).
 */
uint32_t
amdrdna_hub_translation_word(
	uint32_t old)
{
	uint32_t word;

	/* Clears the fields the driver sets. */
	word = old;
	word &= ~FORMAT_TRANSLATE_MODE_MASK;
	word &= ~FORMAT_TRANSLATE_UNMAPPED;
	word &= ~FORMAT_TRANSLATE_TYPE_MASK;

	/* Sets translation, the access mode, the driver model and the memory type. */
	word |= FORMAT_TRANSLATE_ENABLE;
	word |= FORMAT_TRANSLATE_MODE_DRIVER;
	word |= FORMAT_TRANSLATE_ADVANCED;
	word |= FORMAT_TRANSLATE_TYPE_UNCACHED;

	/* Succeeded: the new control word. */
	return word;
}

/*
 * Turns context 0's control word into the driver's: on, one flat table,
 * no retry on a fault.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfxhub_v2_0.c
 * (gfxhub_v2_0_enable_system_domain).
 */
uint32_t
amdrdna_hub_system_context_word(
	uint32_t old)
{
	uint32_t word;

	/* A flat table and no retry. */
	word = old;
	word &= ~FORMAT_CONTEXT_DEPTH_MASK;
	word &= ~FORMAT_CONTEXT_RETRY;

	/* The context on. */
	word |= FORMAT_CONTEXT_ENABLE;

	/* Succeeded: the new control word. */
	return word;
}

/*
 * Turns the control word of one of contexts 1 to 15 into the driver's: on,
 * four levels of 512-entry tables, every kind of fault sent to the default
 * page, and no retry on a fault (the driver has no recoverable faults, and
 * a retrying context can flood the interrupt ring).
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfxhub_v2_0.c (gfxhub_v2_0_setup_vmid_config).
 */
uint32_t
amdrdna_hub_user_context_word(
	uint32_t old)
{
	uint32_t word;

	/* Clears the depth, the block size and the retry bit. */
	word = old;
	word &= ~FORMAT_CONTEXT_DEPTH_MASK;
	word &= ~FORMAT_CONTEXT_BLOCK_MASK;
	word &= ~FORMAT_CONTEXT_RETRY;

	/* Four levels, every fault to the default page, the context on. */
	word |= FORMAT_CONTEXT_DEPTH_FOUR;
	word |= FORMAT_CONTEXT_DEFAULTS;
	word |= FORMAT_CONTEXT_ENABLE;

	/* Succeeded: the new control word. */
	return word;
}

/*
 * Turns the hub's fault control word into one that sends every kind of
 * faulting access to the default page instead of stopping the client.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfxhub_v2_0.c
 * (gfxhub_v2_0_set_fault_enable_default with true).
 */
uint32_t
amdrdna_hub_fault_default_word(
	uint32_t old)
{
	uint32_t word;

	/* All eleven default-page bits on. */
	word = old | FORMAT_FAULT_DEFAULTS;

	/* Succeeded: the new control word. */
	return word;
}

/*
 * Turns the hub's second fault control word into one that retries the
 * read of a page-migration entry.
 */
uint32_t
amdrdna_hub_fault_retry_word(
	uint32_t old)
{
	uint32_t word;

	/* The retry bit on. */
	word = old | FORMAT_FAULT_MIGRATION_RETRY;

	/* Succeeded: the new control word. */
	return word;
}

/*
 * Turns the hub's fault control word into the write that clears the
 * latched fault, so the hub records the next one.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gmc_v10_0.c (gmc_v10_0_process_interrupt).
 */
uint32_t
amdrdna_hub_fault_rearm_word(
	uint32_t old)
{
	uint32_t word;

	/* The clear bit on. */
	word = old | FORMAT_FAULT_REARM;

	/* Succeeded: the word to write. */
	return word;
}

/*
 * Turns a context's control word into one that does (on) or does not raise
 * an interrupt for each kind of fault.
 */
uint32_t
amdrdna_hub_fault_interrupt_word(
	uint32_t old,
	bool on)
{
	uint32_t word;

	/* Sets or clears the seven interrupt bits. */
	word = old & ~FORMAT_CONTEXT_INTERRUPTS;
	if (on)
		word |= FORMAT_CONTEXT_INTERRUPTS;

	/* Succeeded: the new control word. */
	return word;
}

/*
 * Decodes a page fault from its interrupt and the hub's latched fault
 * word: the hub that reported it, the faulting address, the kind of
 * access, the process address-space ID, and the latched fields.
 */
void
amdrdna_vm_fault_decode(
	const struct amdrdna_interrupt *interrupt,
	uint32_t latched,
	struct amdrdna_vm_fault *fault)
{
	uint32_t access;

	/* The media hub reports as its own client; every other client is the graphics hub. */
	fault->hub = AMDRDNA_HUB_GRAPHICS;
	if (interrupt->client == AMDRDNA_CLIENT_MEDIA_HUB)
		fault->hub = AMDRDNA_HUB_MEDIA;

	/* The address: the page number, with its top four bits in the second word. */
	access = interrupt->data[1];
	fault->fault_address = (uint64_t)interrupt->data[0] << FORMAT_PAGE_SHIFT;
	fault->fault_address |= (uint64_t)(access & FORMAT_ACCESS_HIGH_MASK) << FORMAT_ACCESS_HIGH_SHIFT;

	/* The kind of access, from the second word: a retry, a read, a write, a fetch. */
	fault->retried = false;
	if ((access & FORMAT_ACCESS_RETRY) != 0U)
		fault->retried = true;
	fault->reading = false;
	if ((access & FORMAT_ACCESS_READ) != 0U)
		fault->reading = true;
	fault->writing = false;
	if ((access & FORMAT_ACCESS_WRITE) != 0U)
		fault->writing = true;
	fault->executing = false;
	if ((access & FORMAT_ACCESS_EXECUTE) != 0U)
		fault->executing = true;

	/* The four access bits as they came, and the process address space the hub attached. */
	fault->access_bits = access & FORMAT_ACCESS_KIND_MASK;
	fault->space_id = interrupt->space_id;

	/* The hub's latched word and its numeric fields. */
	fault->latched = latched;
	fault->walker_error = (latched >> FORMAT_LATCH_WALKER_SHIFT) & FORMAT_LATCH_WALKER_MASK;
	fault->denied = (latched >> FORMAT_LATCH_DENIED_SHIFT) & FORMAT_LATCH_DENIED_MASK;
	fault->unit = (latched >> FORMAT_LATCH_UNIT_SHIFT) & FORMAT_LATCH_UNIT_MASK;
	fault->context = (latched >> FORMAT_LATCH_CONTEXT_SHIFT) & FORMAT_LATCH_CONTEXT_MASK;

	/* The latched flags: faults behind this one, a mapping error, a write. */
	fault->more_behind = false;
	if ((latched & FORMAT_LATCH_MORE) != 0U)
		fault->more_behind = true;
	fault->unmapped = false;
	if ((latched & FORMAT_LATCH_UNMAPPED) != 0U)
		fault->unmapped = true;
	fault->unit_wrote = false;
	if ((latched & FORMAT_LATCH_WROTE) != 0U)
		fault->unit_wrote = true;
}
