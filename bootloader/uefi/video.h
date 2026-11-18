/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The optional firmware graphics mode selection before the kernel handoff.
 */

#ifndef KERN_UEFI_VIDEO_H
#define KERN_UEFI_VIDEO_H

#include "include/uefi.h"
#include <stddef.h>
#include <stdint.h>

/*
 * A video= request is set and must exist.  Without one, the mode that best
 * fits the monitor's own size (monitor_width by monitor_height, 0 by 0 for
 * unknown) is set when the firmware has one that fits, else the preferred
 * mode when the firmware has it (0 by 0 for none), and otherwise the current
 * mode is kept; malformed or unavailable video= requests fail.
 */
EFI_STATUS
zbl_uefi_video_select(
	EFI_BOOT_SERVICES *boot,
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
	const char *text,
	size_t length,
	UINT32 monitor_width,
	UINT32 monitor_height,
	UINT32 preferred_width,
	UINT32 preferred_height);

/*
 * Reads the size of the monitor a GOP drives from the firmware's EDID
 * protocols.  Returns 1 with the size, or 0 when no usable EDID is found.
 */
int
zbl_uefi_video_monitor_size(
	EFI_BOOT_SERVICES *boot,
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
	UINT32 *width,
	UINT32 *height);

/*
 * Decodes the active size of an EDID's preferred timing.  Returns 1 with the
 * size, or 0 for a block that is not a valid EDID 1.x base block or whose
 * preferred timing is absent or interlaced.
 */
int
zbl_edid_preferred_size(
	const uint8_t *edid,
	size_t size,
	UINT32 *width,
	UINT32 *height);

#endif
