/* Optional firmware graphics mode selection before the kernel handoff. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef KERN_UEFI_VIDEO_H
#define KERN_UEFI_VIDEO_H

#include "include/uefi.h"
#include <stddef.h>

/* Absence preserves the current mode; malformed or unavailable requests fail. */
EFI_STATUS zbl_uefi_video_select(EFI_BOOT_SERVICES *boot,
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, const char *text, size_t length);

#endif
