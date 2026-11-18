/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The AMD GPUs the amdrdna driver recognizes.
 *
 * The device ID is only a gate: a GPU of the first RDNA generation is
 * looked at, a later one is named in the log and left to the firmware.
 * Which chip it really is comes from the graphics block's version in the
 * GPU's own block table.
 */

#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"

/*
 * The device IDs of the discrete RDNA GPUs and their chips.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_drv.c (pciidlist: the Navi10, Navi14,
 * Navi12, Sienna Cichlid, Navy Flounder, Dimgrey Cavefish and Beige Goby
 * entries) for the first two generations, and the PCI ID repository
 * (pci.ids, version 2025.06.09: the Navi 31, Navi 32 and Navi 33 GPU
 * entries) for the third, which Linux matches by class.
 */
static const struct amdrdna_product products[] = {
	{ 0x7310U, AMDRDNA_GENERATION_RDNA1, "navi10" },
	{ 0x7312U, AMDRDNA_GENERATION_RDNA1, "navi10" },
	{ 0x7318U, AMDRDNA_GENERATION_RDNA1, "navi10" },
	{ 0x7319U, AMDRDNA_GENERATION_RDNA1, "navi10" },
	{ 0x731aU, AMDRDNA_GENERATION_RDNA1, "navi10" },
	{ 0x731bU, AMDRDNA_GENERATION_RDNA1, "navi10" },
	{ 0x731eU, AMDRDNA_GENERATION_RDNA1, "navi10" },
	{ 0x731fU, AMDRDNA_GENERATION_RDNA1, "navi10" },
	{ 0x7340U, AMDRDNA_GENERATION_RDNA1, "navi14" },
	{ 0x7341U, AMDRDNA_GENERATION_RDNA1, "navi14" },
	{ 0x7347U, AMDRDNA_GENERATION_RDNA1, "navi14" },
	{ 0x734fU, AMDRDNA_GENERATION_RDNA1, "navi14" },
	{ 0x7360U, AMDRDNA_GENERATION_RDNA1, "navi12" },
	{ 0x7362U, AMDRDNA_GENERATION_RDNA1, "navi12" },
	{ 0x73a0U, AMDRDNA_GENERATION_RDNA2, "navi21" },
	{ 0x73a1U, AMDRDNA_GENERATION_RDNA2, "navi21" },
	{ 0x73a2U, AMDRDNA_GENERATION_RDNA2, "navi21" },
	{ 0x73a3U, AMDRDNA_GENERATION_RDNA2, "navi21" },
	{ 0x73a5U, AMDRDNA_GENERATION_RDNA2, "navi21" },
	{ 0x73a8U, AMDRDNA_GENERATION_RDNA2, "navi21" },
	{ 0x73a9U, AMDRDNA_GENERATION_RDNA2, "navi21" },
	{ 0x73abU, AMDRDNA_GENERATION_RDNA2, "navi21" },
	{ 0x73acU, AMDRDNA_GENERATION_RDNA2, "navi21" },
	{ 0x73adU, AMDRDNA_GENERATION_RDNA2, "navi21" },
	{ 0x73aeU, AMDRDNA_GENERATION_RDNA2, "navi21" },
	{ 0x73afU, AMDRDNA_GENERATION_RDNA2, "navi21" },
	{ 0x73bfU, AMDRDNA_GENERATION_RDNA2, "navi21" },
	{ 0x73c0U, AMDRDNA_GENERATION_RDNA2, "navi22" },
	{ 0x73c1U, AMDRDNA_GENERATION_RDNA2, "navi22" },
	{ 0x73c3U, AMDRDNA_GENERATION_RDNA2, "navi22" },
	{ 0x73daU, AMDRDNA_GENERATION_RDNA2, "navi22" },
	{ 0x73dbU, AMDRDNA_GENERATION_RDNA2, "navi22" },
	{ 0x73dcU, AMDRDNA_GENERATION_RDNA2, "navi22" },
	{ 0x73ddU, AMDRDNA_GENERATION_RDNA2, "navi22" },
	{ 0x73deU, AMDRDNA_GENERATION_RDNA2, "navi22" },
	{ 0x73dfU, AMDRDNA_GENERATION_RDNA2, "navi22" },
	{ 0x73e0U, AMDRDNA_GENERATION_RDNA2, "navi23" },
	{ 0x73e1U, AMDRDNA_GENERATION_RDNA2, "navi23" },
	{ 0x73e2U, AMDRDNA_GENERATION_RDNA2, "navi23" },
	{ 0x73e3U, AMDRDNA_GENERATION_RDNA2, "navi23" },
	{ 0x73e8U, AMDRDNA_GENERATION_RDNA2, "navi23" },
	{ 0x73e9U, AMDRDNA_GENERATION_RDNA2, "navi23" },
	{ 0x73eaU, AMDRDNA_GENERATION_RDNA2, "navi23" },
	{ 0x73ebU, AMDRDNA_GENERATION_RDNA2, "navi23" },
	{ 0x73ecU, AMDRDNA_GENERATION_RDNA2, "navi23" },
	{ 0x73edU, AMDRDNA_GENERATION_RDNA2, "navi23" },
	{ 0x73efU, AMDRDNA_GENERATION_RDNA2, "navi23" },
	{ 0x73ffU, AMDRDNA_GENERATION_RDNA2, "navi23" },
	{ 0x7420U, AMDRDNA_GENERATION_RDNA2, "navi24" },
	{ 0x7421U, AMDRDNA_GENERATION_RDNA2, "navi24" },
	{ 0x7422U, AMDRDNA_GENERATION_RDNA2, "navi24" },
	{ 0x7423U, AMDRDNA_GENERATION_RDNA2, "navi24" },
	{ 0x7424U, AMDRDNA_GENERATION_RDNA2, "navi24" },
	{ 0x743fU, AMDRDNA_GENERATION_RDNA2, "navi24" },
	{ 0x7448U, AMDRDNA_GENERATION_RDNA3, "navi31" },
	{ 0x744aU, AMDRDNA_GENERATION_RDNA3, "navi31" },
	{ 0x744cU, AMDRDNA_GENERATION_RDNA3, "navi31" },
	{ 0x745eU, AMDRDNA_GENERATION_RDNA3, "navi31" },
	{ 0x7460U, AMDRDNA_GENERATION_RDNA3, "navi32" },
	{ 0x7461U, AMDRDNA_GENERATION_RDNA3, "navi32" },
	{ 0x7470U, AMDRDNA_GENERATION_RDNA3, "navi32" },
	{ 0x747eU, AMDRDNA_GENERATION_RDNA3, "navi32" },
	{ 0x73f0U, AMDRDNA_GENERATION_RDNA3, "navi33" },
	{ 0x7480U, AMDRDNA_GENERATION_RDNA3, "navi33" },
	{ 0x7481U, AMDRDNA_GENERATION_RDNA3, "navi33" },
	{ 0x7483U, AMDRDNA_GENERATION_RDNA3, "navi33" },
	{ 0x7487U, AMDRDNA_GENERATION_RDNA3, "navi33" },
	{ 0x7489U, AMDRDNA_GENERATION_RDNA3, "navi33" },
	{ 0x748bU, AMDRDNA_GENERATION_RDNA3, "navi33" },
	{ 0x7499U, AMDRDNA_GENERATION_RDNA3, "navi33" }
};

/*
 * The chips the graphics block's version names.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.c (amdgpu_ucode_legacy_naming,
 * the GC versions of the first two generations) and
 * drivers/gpu/drm/amd/amdgpu/soc21.c (soc21_common_early_init: the GC
 * 11.0.0, 11.0.2 and 11.0.3 cases, whose revision offsets Mesa 25.3.6
 * src/amd/addrlib/src/amdgpu_asic_addr.h ranges name Navi31, Navi33 and
 * Navi32).
 */
static const struct graphics_chip {
	uint8_t major;
	uint8_t minor;
	uint8_t step;
	const char *chip;
} graphics_chips[] = {
	{ 10U, 1U, 10U, "navi10" },
	{ 10U, 1U, 1U, "navi14" },
	{ 10U, 1U, 2U, "navi12" },
	{ 10U, 3U, 0U, "navi21" },
	{ 10U, 3U, 2U, "navi22" },
	{ 10U, 3U, 4U, "navi23" },
	{ 10U, 3U, 5U, "navi24" },
	{ 11U, 0U, 0U, "navi31" },
	{ 11U, 0U, 2U, "navi33" },
	{ 11U, 0U, 3U, "navi32" }
};

/*
 * Finds a device ID among the known RDNA GPUs; NULL for any other.
 */
const struct amdrdna_product *
amdrdna_product_find(
	uint16_t device)
{
	size_t index;

	/* Looks the ID up in the table. */
	for (index = 0U; index < sizeof(products) / sizeof(products[0]); index++) {
		/* Stops at the ID's row. */
		if (products[index].device == device)
			return &products[index];
	}

	/* The ID is not an RDNA GPU the driver knows. */
	return NULL;
}

/*
 * Names a generation for the log.
 */
const char *
amdrdna_generation_name(
	enum amdrdna_generation generation)
{
	/* Each generation by its marketing name. */
	switch (generation) {
	case AMDRDNA_GENERATION_RDNA1:
		return "RDNA1";
	case AMDRDNA_GENERATION_RDNA2:
		return "RDNA2";
	case AMDRDNA_GENERATION_RDNA3:
		return "RDNA3";
	default:
		break;
	}

	/* Anything else is not a generation the driver knows. */
	return "unknown";
}

/*
 * Names the chip a graphics block's version stands for; NULL for a version
 * the driver does not know.
 */
const char *
amdrdna_graphics_chip_name(
	const struct amdrdna_ip *graphics)
{
	size_t index;

	/* Looks the version up. */
	for (index = 0U; index < sizeof(graphics_chips) / sizeof(graphics_chips[0]); index++) {
		/* Stops at a row whose three version parts all match. */
		if (graphics_chips[index].major == graphics->major &&
		    graphics_chips[index].minor == graphics->minor &&
		    graphics_chips[index].step == graphics->step)
			return graphics_chips[index].chip;
	}

	/* A version the driver does not know. */
	return NULL;
}
