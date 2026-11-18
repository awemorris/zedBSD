/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The addresses of the registers of the two memory hubs.
 *
 * The graphics hub's registers lie in the first window of the graphics
 * block, the media hub's in the first window of its own block; both hubs
 * have the same registers in the same order.  A register kept once per
 * context or once per invalidation engine repeats at a fixed distance, one
 * or two dwords, from the first instance.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"

/* The contexts a hub translates for, and its invalidation engines. */
#define HUB_CONTEXTS			16U
#define HUB_ENGINES			18U

/*
 * Where one named hub register lies: its offset in each hub's window, the
 * distance between its instances (zero for a register kept once), and how
 * many instances it has.
 */
struct hub_place {
	uint16_t offset[AMDRDNA_HUB_COUNT];
	uint8_t stride;
	uint8_t instances;
};

/*
 * The place of every named hub register, in the order of
 * enum amdrdna_hub_register: the graphics hub's offset in the first
 * window of the graphics block and the media hub's in the first window of
 * its block.  The per-context control word repeats every dword, the
 * per-context table words every two; the invalidation engines' lock,
 * request and done words every dword, their range words every two.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_offset.h and
 * drivers/gpu/drm/amd/include/asic_reg/mmhub/mmhub_2_0_0_offset.h
 * (mmGCVM_L2_CNTL to mmGCVM_L2_CNTL5, mmGCVM_L2_PROTECTION_FAULT_CNTL,
 * mmGCVM_L2_PROTECTION_FAULT_CNTL2, mmGCVM_L2_PROTECTION_FAULT_STATUS,
 * mmGCVM_L2_PROTECTION_FAULT_ADDR_LO32 and _HI32,
 * mmGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32 and _HI32,
 * mmGCVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_LO32 to
 * mmGCVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_HI32, mmGCVM_CONTEXT0_CNTL,
 * mmGCVM_INVALIDATE_ENG0_SEM, mmGCVM_INVALIDATE_ENG0_REQ,
 * mmGCVM_INVALIDATE_ENG0_ACK, mmGCVM_INVALIDATE_ENG0_ADDR_RANGE_LO32 and
 * _HI32, mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32 to
 * mmGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32, mmGCMC_VM_FB_OFFSET,
 * mmGCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB and _MSB,
 * mmGCMC_VM_FB_LOCATION_BASE, mmGCMC_VM_FB_LOCATION_TOP, mmGCMC_VM_AGP_TOP,
 * mmGCMC_VM_AGP_BOT, mmGCMC_VM_AGP_BASE,
 * mmGCMC_VM_SYSTEM_APERTURE_LOW_ADDR and _HIGH_ADDR,
 * mmGCMC_VM_MX_L1_TLB_CNTL, and the same registers of the media hub,
 * named MMVM_ and MMMC_) and drivers/gpu/drm/amd/amdgpu/gfxhub_v2_0.c
 * (gfxhub_v2_0_init: the distances between contexts and engines).
 */
static const struct hub_place hub_places[AMDRDNA_HUB_REGISTER_COUNT] = {
	{ { 0x15e0U, 0x0680U }, 0U, 1U },
	{ { 0x15e1U, 0x0681U }, 0U, 1U },
	{ { 0x15e2U, 0x0682U }, 0U, 1U },
	{ { 0x15f8U, 0x0698U }, 0U, 1U },
	{ { 0x1601U, 0x06a1U }, 0U, 1U },
	{ { 0x15e8U, 0x0688U }, 0U, 1U },
	{ { 0x15e9U, 0x0689U }, 0U, 1U },
	{ { 0x15ecU, 0x068cU }, 0U, 1U },
	{ { 0x15edU, 0x068dU }, 0U, 1U },
	{ { 0x15eeU, 0x068eU }, 0U, 1U },
	{ { 0x15efU, 0x068fU }, 0U, 1U },
	{ { 0x15f0U, 0x0690U }, 0U, 1U },
	{ { 0x15f2U, 0x0692U }, 0U, 1U },
	{ { 0x15f3U, 0x0693U }, 0U, 1U },
	{ { 0x15f4U, 0x0694U }, 0U, 1U },
	{ { 0x15f5U, 0x0695U }, 0U, 1U },
	{ { 0x15f6U, 0x0696U }, 0U, 1U },
	{ { 0x15f7U, 0x0697U }, 0U, 1U },
	{ { 0x1620U, 0x06c0U }, 1U, HUB_CONTEXTS },
	{ { 0x1631U, 0x06d1U }, 1U, HUB_ENGINES },
	{ { 0x1643U, 0x06e3U }, 1U, HUB_ENGINES },
	{ { 0x1655U, 0x06f5U }, 1U, HUB_ENGINES },
	{ { 0x1667U, 0x0707U }, 2U, HUB_ENGINES },
	{ { 0x1668U, 0x0708U }, 2U, HUB_ENGINES },
	{ { 0x168bU, 0x072bU }, 2U, HUB_CONTEXTS },
	{ { 0x168cU, 0x072cU }, 2U, HUB_CONTEXTS },
	{ { 0x16abU, 0x074bU }, 2U, HUB_CONTEXTS },
	{ { 0x16acU, 0x074cU }, 2U, HUB_CONTEXTS },
	{ { 0x16cbU, 0x076bU }, 2U, HUB_CONTEXTS },
	{ { 0x16ccU, 0x076cU }, 2U, HUB_CONTEXTS },
	{ { 0x170bU, 0x0857U }, 0U, 1U },
	{ { 0x170cU, 0x0858U }, 0U, 1U },
	{ { 0x170dU, 0x0859U }, 0U, 1U },
	{ { 0x1720U, 0x086cU }, 0U, 1U },
	{ { 0x1721U, 0x086dU }, 0U, 1U },
	{ { 0x1722U, 0x086eU }, 0U, 1U },
	{ { 0x1723U, 0x086fU }, 0U, 1U },
	{ { 0x1724U, 0x0870U }, 0U, 1U },
	{ { 0x1725U, 0x0871U }, 0U, 1U },
	{ { 0x1726U, 0x0872U }, 0U, 1U },
	{ { 0x1727U, 0x0873U }, 0U, 1U }
};

/*
 * Turns one instance of a named hub register into its absolute dword index.
 *
 * block is the hub's IP block as the block table lists it: the graphics
 * block for the graphics hub, the media hub's block for the media hub.
 * Returns 0 for a block the table did not list, a register or hub outside
 * the lists, or an instance past the register's last.
 */
uint32_t
amdrdna_hub_index(
	const struct amdrdna_ip *block,
	enum amdrdna_hub hub,
	enum amdrdna_hub_register which,
	unsigned instance)
{
	const struct hub_place *place;
	uint32_t index;

	/* A register or a hub outside the lists has no address. */
	if ((unsigned)which >= (unsigned)AMDRDNA_HUB_REGISTER_COUNT)
		return 0U;
	if ((unsigned)hub >= (unsigned)AMDRDNA_HUB_COUNT)
		return 0U;

	/* The block must be listed, with its first window. */
	if (block->found == 0U || block->window_count == 0U)
		return 0U;

	/* An instance past the last one has no address. */
	place = &hub_places[which];
	if (instance >= place->instances)
		return 0U;

	/* The window's start, the register's offset and the instance's distance. */
	index = block->window[0] + place->offset[hub] + instance * place->stride;

	/* Succeeded: the register's dword index. */
	return index;
}

/*
 * Reports how many instances a named hub register has: 1, or one per
 * context or per invalidation engine.  A register outside the list has
 * none.
 */
unsigned
amdrdna_hub_instances(
	enum amdrdna_hub_register which)
{
	/* A register outside the list has no instance. */
	if ((unsigned)which >= (unsigned)AMDRDNA_HUB_REGISTER_COUNT)
		return 0U;

	/* Succeeded: the register's instance count. */
	return hub_places[which].instances;
}
