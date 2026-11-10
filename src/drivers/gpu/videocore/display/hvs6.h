/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The BCM2712's display path: the HVS6 compositor and the two pixel valves
 * that feed the HDMI ports.
 *
 * The firmware sets the mode and keeps the HDMI encoders, the pixel valves'
 * timing and the compositor's own configuration; the driver takes over only
 * the display list of the channel the firmware shows the console on, and
 * the pixel valve's vertical-blank interrupt.
 */

#ifndef KERN_DRIVERS_GPU_BCM2711_DISPLAY_HVS6_H
#define KERN_DRIVERS_GPU_BCM2711_DISPLAY_HVS6_H

#include <stdbool.h>
#include <stdint.h>

#include <drivers/generic/fdt.h>
#include <kern/lock.h>
#include <kern/platform.h>

#include "drivers/gpu/videocore/bcm2711-private.h"

/* The stage-mark family of the HVS6 display, and its boot parameter prefix. */
#define BCM2712_FAMILY_DISPLAY		"hvs6"

/* The compositor's output channels and the pixel valves (HDMI0, HDMI1). */
#define BCM2712_HVS_CHANNELS		3U
#define BCM2712_HVS_VALVES		2U

/* The vertical blanks the display's stage R1 times (an odd count, an even number of frames between them). */
#define BCM2712_VBLANK_TIMED		7U

/* The most planes of one display list the readout keeps. */
#define BCM2712_HVS_LIST_PLANES		8U

/*
 * One plane of a display list, as the readout decoded it.
 *
 * start is the word the plane begins at and words how many it has; the
 * fields are those of an unscaled raster plane, and scaled says the plane
 * has the scaled-size word, which shifts the others by one.
 */
struct bcm2712_hvs_plane {
	uint32_t start;
	uint32_t words;
	uint32_t control;
	uint32_t format;
	uint32_t order;
	uint32_t address_mode;
	bool scaled;
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	uint32_t pointer0;
	uint64_t address;
	uint32_t pitch;
	uint32_t upm_handle;
	uint32_t upm_base;
};

/*
 * The display path.
 *
 * One instance exists for the system.  base is the Pi 4's display record,
 * which the shared display device and the HVS6 flip code (hvs6-flip.c, which
 * keeps the names of the Pi 4's display-flip.c) are handed: its compositor
 * window is the HVS6's, its timing windows and lines the pixel valves', its
 * port and channel the channel the firmware shows.  Discovery fills the
 * windows and the interrupt lines; the readout fills the rest from the
 * hardware.  guard protects the vertical-blank count and times against
 * R1's handler.
 */
struct bcm2712_display {
	struct bcm2711_display base;
	bool present;
	struct bcm2711_window iommu;
	struct kern_boot_framebuffer screen;
	bool screen_known;

	/* The register layout: C0 (0x53) or D0 (0x54), and where channel 0's block starts and how far apart the blocks are. */
	uint32_t version;
	uint32_t channel_base;
	uint32_t channel_stride;

	/* The display list memory's words and the unified pixel memory's size in 256-byte words. */
	uint32_t list_words;
	uint32_t ubm_words;

	/* The channel the firmware shows, its list, and where that list ends (its END word). */
	uint32_t channel;
	uint32_t firmware_head;
	uint32_t firmware_end;
	uint32_t plane_count;
	uint32_t skipped_elements;
	struct bcm2712_hvs_plane planes[BCM2712_HVS_LIST_PLANES];

	/* The driver's own list, once it took the channel over: the console list, and its words with the END word. */
	bool adopted;
	uint32_t own_head;
	uint32_t own_words;

	/* The shown channel's control word when the device attached, which the flip code checks stays the same. */
	uint32_t attach_control0;

	/*
	 * Vertical blanks counted by the pixel valve's handler, how many of
	 * them it timed, the monotonic counter at each timed one, and the
	 * counter's frequency.
	 */
	struct spinlock guard;
	volatile uint64_t vblank_count;
	volatile uint64_t vblank_timed;
	volatile uint64_t vblank_counters[BCM2712_VBLANK_TIMED];
	volatile uint64_t vblank_counter_hz;
};

/* The channel registers the flip code reads and writes. */
enum bcm2712_hvs_register {
	BCM2712_HVS_CTRL0,
	BCM2712_HVS_LPTRS,
	BCM2712_HVS_DL
};

/* The display list memory's place in the compositor's window, and the list-head field of LPTRS and DL. */
#define BCM2712_HVS_LIST_MEMORY		0x4000U
#define BCM2712_HVS_HEAD_MASK		0xfffU

/* The flip slots: three lists of 128 words after the console list's 256. */
#define BCM2712_HVS_CONSOLE_WORDS	256U
#define BCM2712_HVS_SLOT_WORDS		128U

/* The pixel valve's control registers and their enable bits, and its interrupt status and the vertical blank's bit. */
#define BCM2712_VALVE_CONTROL		0x00U
#define BCM2712_VALVE_V_CONTROL		0x04U
#define BCM2712_VALVE_INTEN		0x24U
#define BCM2712_VALVE_INTSTAT		0x28U
#define BCM2712_VALVE_VFP_START		0x80U

/*
 * The unified-pixel-memory handles and bases (in 256-byte words) of the
 * driver's planes: the primary, the two overlays and the cursor, from the
 * top of the 1024-word memory, each sized for a 16384-byte row (129 words)
 * but the cursor's 256-byte row (3 words).  The firmware's console plane
 * keeps its own.
 */
#define BCM2712_UPM_PRIMARY		((634U << 16) | (27U << 10))
#define BCM2712_UPM_OVERLAY0		((763U << 16) | (28U << 10))
#define BCM2712_UPM_OVERLAY1		((892U << 16) | (29U << 10))
#define BCM2712_UPM_CURSOR		((1021U << 16) | (30U << 10))
#define BCM2712_UPM_LOWEST_BASE		634U
#define BCM2712_UPM_LOWEST_HANDLE	28U

/*
 * Converts the shared display record to the HVS6 record that holds it.
 *
 * Only the BCM2712 attachment creates displays on this board, inside a
 * struct bcm2712_display, so the conversion always holds.
 */
static __inline struct bcm2712_display *
bcm2712_display_of(
	struct bcm2711_display *display)
{
	/* The shared record is the first member of the HVS6 record. */
	return (struct bcm2712_display *)(void *)display;
}

/* Reports the window offset of one register of the shown channel. */
uint32_t bcm2712_display_register_offset(const struct bcm2712_display *display, enum bcm2712_hvs_register which);

int bcm2712_display_discover(const struct drv_fdt *fdt, struct bcm2712_display *display, const struct kern_boot_framebuffer *screen);
int bcm2712_display_readout(struct bcm2712_display *display);
int bcm2712_display_adopt(struct bcm2712_display *display);
int bcm2712_display_vblank_test(struct bcm2712_display *display);
int bcm2712_display_overlay_test(struct bcm2712_display *display);
int bcm2712_display_attach_device(struct bcm2712_display *display);

#endif
