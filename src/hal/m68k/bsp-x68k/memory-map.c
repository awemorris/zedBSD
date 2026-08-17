/* X68000 MMIO/VRAM/ROM map, kept outside the reusable m68k backend. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "memory-map.h"

#define RW_DEVICE (HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_NOCACHE | \
	HAL_SPACE_DEVICE)
#define RO_DEVICE (HAL_SPACE_READ | HAL_SPACE_NOCACHE | HAL_SPACE_DEVICE)

static const struct x68k_physical_mapping mappings[] = {
	{ 0x00c00000U, 0x00200000U, RW_DEVICE, "graphics VRAM" },
	{ 0x00e00000U, 0x00080000U, RW_DEVICE, "text VRAM" },
	{ 0x00e80000U, 0x00001000U, RW_DEVICE, "CRTC" },
	{ 0x00e82000U, 0x00001000U, RW_DEVICE, "video controller" },
	{ 0x00e84000U, 0x00001000U, RW_DEVICE, "DMAC" },
	{ 0x00e86000U, 0x00001000U, RW_DEVICE, "supervisor area" },
	{ 0x00e88000U, 0x00001000U, RW_DEVICE, "MFP" },
	{ 0x00e8a000U, 0x00001000U, RW_DEVICE, "RTC" },
	{ 0x00e8e000U, 0x00001000U, RW_DEVICE, "system ports" },
	{ 0x00e94000U, 0x00001000U, RW_DEVICE, "FDC" },
	{ 0x00e96000U, 0x00001000U, RW_DEVICE, "internal SCSI" },
	{ 0x00e98000U, 0x00001000U, RW_DEVICE, "SCC" },
	{ 0x00e9c000U, 0x00001000U, RW_DEVICE, "IOC" },
	{ 0x00ed0000U, 0x00004000U, RW_DEVICE, "SRAM" },
	{ 0x00f00000U, 0x00100000U, RO_DEVICE, "ROM" },
};

const struct x68k_physical_mapping *
x68k_physical_mappings(size_t *count)
{
	if (count != NULL)
		*count = sizeof(mappings) / sizeof(mappings[0]);
	return mappings;
}
