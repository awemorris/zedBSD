/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The boot logo: a binary PPM on the boot volume, drawn in the middle of
 * the screen.
 */

#ifndef KERN_UEFI_LOGO_H
#define KERN_UEFI_LOGO_H

#include <stddef.h>
#include <stdint.h>

#include "bootloader/include/amd64-handoff.h"
#include "bootloader/common/logo-path.h"

/* The largest logo file the loader takes. */
#define ZBL_UEFI_LOGO_FILE_MAX		(16U * 1024U * 1024U)

/*
 * Draws a binary PPM on the framebuffer; returns 1 when it was drawn.
 */
int
zbl_uefi_logo_draw(
	const uint8_t *data,
	size_t size,
	const struct zbl6_framebuffer *framebuffer);

#endif
