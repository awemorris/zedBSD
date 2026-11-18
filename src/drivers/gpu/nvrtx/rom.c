/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reading the VBIOS of the GPU.
 *
 * Two windows show the ROM.  The display keeps a copy of the VBIOS in the
 * VRAM when the GPU's firmware brought the display up; the instance-memory
 * window of the registers can be pointed at it.  Otherwise the ROM itself is
 * read through its window in the registers, after the PCI configuration's
 * shadow bit is cleared.  Both are a write that is put back as soon as the
 * read is done; nothing else is written.  The first window whose chain of
 * images walks is used.
 */

#include <stddef.h>
#include <stdint.h>

#include <drivers/pci/pci.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-gpu.h"

/*
 * The instance-memory window: its base register (in 64 KiB units) and where it
 * shows in the registers.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/bios/shadowramin.c (pramin_init,
 * pramin_read).
 */
#define ROM_REG_WINDOW_BASE		0x001700U
#define ROM_WINDOW_AT			0x700000U
#define ROM_WINDOW_FALLBACK_OFFSET	0xf0000U

/*
 * The VGA-workspace register: valid bit, location field (1 = VRAM) and address
 * field.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/bios/shadowramin.c (pramin_init).
 */
#define ROM_VGA_VALID			0x8U
#define ROM_VGA_LOCATION_MASK		0x3U
#define ROM_VGA_IN_VRAM			0x1U
#define ROM_VGA_ADDRESS_MASK		0xffffff00U

/*
 * The ROM's own window in the registers, and the PCI configuration word whose
 * bit 0 shadows it.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/bios/shadowrom.c (nvbios_prom_read) and
 * nvkm/subdev/pci/base.c (nvkm_pci_rom_shadow).
 */
#define ROM_PROM_AT			0x300000U
#define ROM_PCI_SHADOW			0x50U
#define ROM_PCI_SHADOW_ON		0x1U

static void rom_copy(struct nvrtx_gpu *gpu, uint32_t window, uint8_t *data);
static int rom_from_vram(struct nvrtx_gpu *gpu, uint8_t *data);
static int rom_from_prom(struct nvrtx_gpu *gpu, uint8_t *data);

/*
 * Reads the VBIOS and walks its chain of images.
 *
 * Returns 0 with the VBIOS in gpu->vbios_data and its shape in gpu->vbios,
 * or the error of the last window tried.
 */
int
nvrtx_rom_read(
	struct nvrtx_gpu *gpu)
{
	uint8_t *data;
	int error;

	/* Allocates room for the largest VBIOS. */
	data = kern_malloc(NVRTX_VBIOS_MAX_BYTES);
	if (data == NULL)
		return ENOMEM;

	/* Tries the display's copy first, then the ROM itself. */
	error = rom_from_vram(gpu, data);
	if (error == 0)
		error = nvrtx_vbios_walk(data, NVRTX_VBIOS_MAX_BYTES, &gpu->vbios);
	if (error != 0) {
		nvrtx_stage_mark("layout vbios: no copy in VRAM (%d), reading the ROM", error);
		error = rom_from_prom(gpu, data);
		if (error == 0)
			error = nvrtx_vbios_walk(data, NVRTX_VBIOS_MAX_BYTES, &gpu->vbios);
	}

	/* Neither window gave a VBIOS whose chain walks. */
	if (error != 0) {
		kern_free(data);
		return error;
	}

	/* An image chain that runs past the window is broken. */
	if (gpu->vbios.end > NVRTX_VBIOS_MAX_BYTES) {
		kern_free(data);
		return EINVAL;
	}

	/* Succeeded: the VBIOS is kept until the boot memory is released. */
	gpu->vbios_data = data;
	gpu->vbios_size = gpu->vbios.end;
	gpu->vbios.size = gpu->vbios.end;
	return 0;
}

/* Copies the 1 MiB a register window shows, one word at a time. */
static void
rom_copy(
	struct nvrtx_gpu *gpu,
	uint32_t window,
	uint8_t *data)
{
	uint32_t at;
	uint32_t word;

	/* Reads each word and stores it little-endian. */
	for (at = 0U; at < NVRTX_VBIOS_MAX_BYTES; at += 4U) {
		word = nvrtx_rd32(gpu, window + at);
		nvrtx_put32(data, at, word);
	}
}

/*
 * Reads the display's copy of the VBIOS in the VRAM, when the display made one.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/bios/shadowramin.c (pramin_init,
 * pramin_read, pramin_fini).
 */
static int
rom_from_vram(
	struct nvrtx_gpu *gpu,
	uint8_t *data)
{
	uint32_t value;
	uint64_t address;
	uint32_t saved;

	/* A GPU without a display has no copy (the display-disabled fuse is the generation's register). */
	value = nvrtx_rd32(gpu, gpu->identity.chip.profile->display_fuse_register);
	if ((value & 0x1U) != 0U)
		return ENODEV;

	/* The copy must be valid and in the VRAM. */
	value = nvrtx_rd32(gpu, NVRTX_REG_VGA_WORKSPACE);
	if ((value & ROM_VGA_VALID) == 0U)
		return ENODEV;
	if ((value & ROM_VGA_LOCATION_MASK) != ROM_VGA_IN_VRAM)
		return ENODEV;

	/* Finds the copy's VRAM address; a zero field means the window's base plus 960 KiB. */
	address = (uint64_t)(value & ROM_VGA_ADDRESS_MASK) << 8;
	if (address == 0U)
		address = ((uint64_t)nvrtx_rd32(gpu, ROM_REG_WINDOW_BASE) << 16) + ROM_WINDOW_FALLBACK_OFFSET;

	/* Points the window at the copy, reads it, and puts the window back. */
	saved = nvrtx_rd32(gpu, ROM_REG_WINDOW_BASE);
	nvrtx_wr32(gpu, ROM_REG_WINDOW_BASE, (uint32_t)(address >> 16));
	rom_copy(gpu, ROM_WINDOW_AT, data);
	nvrtx_wr32(gpu, ROM_REG_WINDOW_BASE, saved);

	/* Succeeded: the copy is read; the walk decides whether it is a VBIOS. */
	return 0;
}

/*
 * Reads the ROM through its window, with the shadow turned off for the read.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/bios/shadowrom.c (nvbios_prom_init,
 * nvbios_prom_read, nvbios_prom_fini) and nvkm/subdev/pci/base.c
 * (nvkm_pci_rom_shadow).
 */
static int
rom_from_prom(
	struct nvrtx_gpu *gpu,
	uint8_t *data)
{
	struct drv_pci_device *pci;
	uint32_t saved;
	int error;

	/* Reads the shadow word, so it can be put back exactly. */
	pci = gpu->identity.pci;
	error = drv_pci_device_config_read32(pci, ROM_PCI_SHADOW, &saved);
	if (error != 0)
		return error;

	/* Turns the shadow off, reads the ROM, and puts the word back. */
	error = drv_pci_device_config_write32(pci, ROM_PCI_SHADOW, saved & ~ROM_PCI_SHADOW_ON);
	if (error != 0)
		return error;

	rom_copy(gpu, ROM_PROM_AT, data);
	error = drv_pci_device_config_write32(pci, ROM_PCI_SHADOW, saved);
	if (error != 0)
		return error;

	/* Succeeded: the ROM is read; the walk decides whether it is a VBIOS. */
	return 0;
}
