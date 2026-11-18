/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The virt board's framebuffer.
 *
 * The emulator's RAM framebuffer device shows a block of guest memory as
 * the screen once the guest writes the block's address and format to the
 * device's configuration file.  The block lives in the kernel image, 640
 * by 400 pixels of 32 bits, so that 80 by 25 cells of the 8x16 font fill
 * it exactly and the early console, which draws on it, needs no margin.
 */

#include <hal/hal.h>
#include <kern/boot.h>
#include "../fbcons.h"
#include "../space.h"
#include "fwcfg.h"
#include "framebuffer.h"

/* The screen: its size in pixels, the bytes of a row, and of the whole. */
#define FRAMEBUFFER_WIDTH	640U
#define FRAMEBUFFER_HEIGHT	400U
#define FRAMEBUFFER_STRIDE	(FRAMEBUFFER_WIDTH * 4U)
#define FRAMEBUFFER_PIXELS	(FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT)

/*
 * The pixel format: 32 bits, the top byte unused, then red, green and
 * blue, as the DRM fourcc code XR24 names it.
 *
 * Values referenced: Linux 6.8.12 include/uapi/drm/drm_fourcc.h (the
 * XRGB8888 fourcc code, the characters "XR24").
 */
#define FRAMEBUFFER_FOURCC_XRGB8888	0x34325258U

/* The configuration file of the RAM framebuffer device. */
#define FRAMEBUFFER_CONFIG_FILE		"etc/ramfb"

/*
 * The device's configuration, big-endian and without padding.
 *
 * Values referenced: QEMU v10.0.0 hw/display/ramfb.c (the configuration's
 * fields: a 64-bit address, then 32-bit fourcc, flags, width, height and
 * stride, 28 bytes, all big-endian).
 */
struct framebuffer_config {
	uint64_t address;
	uint32_t fourcc;
	uint32_t flags;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
} __attribute__((packed));

_Static_assert(sizeof(struct framebuffer_config) == 28U, "the device reads 28 bytes");

/*
 * The screen's memory, which the emulator reads as the display.
 *
 * It is in the kernel image so that its physical address is fixed from the
 * start, and page-aligned as the device asks.  The kernel's own console
 * draws into it after the HAL's early console.
 */
static uint32_t framebuffer_pixels[FRAMEBUFFER_PIXELS] __attribute__((aligned(4096)));

/*
 * The configuration written to the device, kept in the image so the device
 * can read it by physical address.  Written once on the boot hart.
 */
static struct framebuffer_config framebuffer_config_block __attribute__((aligned(16)));

/*
 * Nonzero once the device shows the memory and the early console draws on
 * it.  Set once on the boot hart before any other hart starts.
 */
static int framebuffer_on;

static int attach_console(void);

/*
 * Points the RAM framebuffer device at the screen's memory.
 *
 * Returns HAL_OK once the device shows it and the early console draws on
 * it, or why not: there is no configuration interface, or no such device.
 */
int
virt_framebuffer_init(
	void)
{
	uint64_t pixels_phys;
	uint16_t selector;
	uint32_t size;
	int error;

	/* Finds the device's configuration file; a machine without the device has none. */
	error = virt_fwcfg_find(FRAMEBUFFER_CONFIG_FILE, &selector, &size);
	if (error != HAL_OK)
		return error;

	/* Refuses a configuration of another layout than the one written here. */
	if (size != sizeof(framebuffer_config_block))
		return HAL_ERR_UNSUPPORTED;

	/* Describes the memory: where it is, its format, size and row length. */
	pixels_phys = riscv64_virt_to_phys(framebuffer_pixels);
	framebuffer_config_block.address = __builtin_bswap64(pixels_phys);
	framebuffer_config_block.fourcc = __builtin_bswap32(FRAMEBUFFER_FOURCC_XRGB8888);
	framebuffer_config_block.flags = 0;
	framebuffer_config_block.width = __builtin_bswap32(FRAMEBUFFER_WIDTH);
	framebuffer_config_block.height = __builtin_bswap32(FRAMEBUFFER_HEIGHT);
	framebuffer_config_block.stride = __builtin_bswap32(FRAMEBUFFER_STRIDE);

	/* Writes the description, after which the device shows the memory. */
	error = virt_fwcfg_write(selector, &framebuffer_config_block, sizeof(framebuffer_config_block));
	if (error != HAL_OK)
		return error;

	/* Hands the shown memory to the early console, which clears it and draws its text. */
	error = attach_console();
	if (error != HAL_OK)
		return error;

	/* The screen is on for the boot log. */
	framebuffer_on = 1;

	/* Succeeded: drawing reaches the screen. */
	return HAL_OK;
}

/*
 * Writes the screen's address and format to the boot log.
 */
void
virt_framebuffer_describe(
	void)
{
	uint64_t pixels_phys;

	/* Describes a shown screen; a missing one was reported when it was looked for. */
	if (!framebuffer_on)
		return;

	/* Writes the screen's size and where its memory is. */
	pixels_phys = riscv64_virt_to_phys(framebuffer_pixels);
	hal_printf("RISCV64 FRAMEBUFFER %ux%u phys=%lx\n",
		   FRAMEBUFFER_WIDTH,
		   FRAMEBUFFER_HEIGHT,
		   pixels_phys);
}

/*
 * Describes the screen to the early console: the memory in the image, its
 * size, row length and the XRGB8888 layout, which is blue first in memory.
 * The emulator reads guest memory as it is, so nothing is written through.
 */
static int
attach_console(
	void)
{
	struct riscv64_fbcons_screen screen;
	int error;

	/* Describes the memory the device shows. */
	screen.address = (uintptr_t)framebuffer_pixels;
	screen.width = FRAMEBUFFER_WIDTH;
	screen.height = FRAMEBUFFER_HEIGHT;
	screen.pitch = FRAMEBUFFER_STRIDE;
	screen.format = KERN_BOOT_FRAMEBUFFER_BGRX8888;
	screen.flush = NULL;

	/* Attaches the console, which draws what it printed so far. */
	error = riscv64_fbcons_attach(&screen);
	if (error != HAL_OK)
		return error;

	/* Succeeded: the console draws on the screen. */
	return HAL_OK;
}
