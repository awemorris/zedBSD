/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The first OSD plane of the Amlogic VPU, as the bootloader left it.
 *
 * The plane reads a linear buffer: its address in 16-byte units, a row
 * pitch in 16-byte units, and the window of the buffer it shows.  The
 * display node only reads how the plane is set up and, to flip, writes
 * the buffer's address; it never changes the plane's window, format,
 * the blending or the output's mode.  The decoding here touches no
 * register, so it runs unchanged in a host test.
 */

#ifndef KERN_DRIVERS_GPU_MALI_DISPLAY_OSD_H
#define KERN_DRIVERS_GPU_MALI_DISPLAY_OSD_H

#include <stdint.h>

/*
 * The plane's registers, as byte offsets in the VPU's window (the
 * register's index times four), and the bits the node reads.  The
 * control word enables the plane's first block (bit 0), reads a linear
 * buffer rather than a canvas (bit 2) and enables the plane (bit 21).
 * The window words hold a first and a last pixel, [12:0] and [28:16].
 *
 * Values referenced:
 * hardkernel/kernel_common_drivers odroids7d-6.12.y 34daba9
 * drivers/drm/vpu-hw/meson_vpu_osd_mif.h (VIU_OSD1_CTRL_STAT,
 * VIU_OSD1_BLK0_CFG_W0, VIU_OSD1_BLK0_CFG_W1, VIU_OSD1_BLK0_CFG_W2,
 * VIU_OSD1_BLK1_CFG_W4, VIU_OSD1_BLK2_CFG_W4); meson_vpu_osd_mif.c
 * (osd_mem_mode: bit 2; osd_linear_addr_config: the address >> 4 and
 * the stride in [11:0]; the window words of osd_scope_config); Linux v7.3
 * drivers/gpu/drm/meson/meson_registers.h (VIU_OSD1_CTRL_STAT bits 0
 * and 21).
 */
#define MALI_OSD_CONTROL		(0x1a10U * 4U)
#define MALI_OSD_FORMAT			(0x1a1bU * 4U)
#define MALI_OSD_WINDOW_X		(0x1a1cU * 4U)
#define MALI_OSD_WINDOW_Y		(0x1a1dU * 4U)
#define MALI_OSD_ADDRESS		(0x1a14U * 4U)
#define MALI_OSD_STRIDE			(0x1a15U * 4U)
#define MALI_OSD_WINDOW_BYTES		0x40000U
#define MALI_OSD_CONTROL_BLOCK		0x00000001U
#define MALI_OSD_CONTROL_LINEAR		0x00000004U
#define MALI_OSD_CONTROL_ENABLE		0x00200000U
#define MALI_OSD_WINDOW_FIRST_MASK	0x1fffU
#define MALI_OSD_WINDOW_LAST_SHIFT	16U
#define MALI_OSD_STRIDE_MASK		0xfffU
#define MALI_OSD_UNIT_SHIFT		4U

/*
 * The plane's words as read, and what they say: the buffer's address,
 * its row pitch and the extent of the window, in bytes and pixels.
 */
struct mali_osd_plane {
	uint32_t control;
	uint32_t format;
	uint32_t window_x;
	uint32_t window_y;
	uint32_t address;
	uint32_t stride;
	uint64_t buffer;
	uint32_t pitch;
	uint32_t width;
	uint32_t height;
};

/*
 * Decodes the words of a plane; zero when the plane is on, reads a
 * linear buffer and shows a window from its top left corner.
 */
int mali_osd_decode(struct mali_osd_plane *plane);

/*
 * Reports whether a decoded plane shows a buffer at address, of width by
 * height pixels with rows pitch bytes apart: nonzero when it does.
 */
int mali_osd_shows(const struct mali_osd_plane *plane, uint64_t address, uint32_t width, uint32_t height, uint32_t pitch);

/*
 * Builds the address word that points the plane at a buffer; EINVAL
 * when the address is not 16-byte aligned, ERANGE when the word cannot
 * hold it.
 */
int mali_osd_address_word(uint64_t address, uint32_t *word);

#endif
