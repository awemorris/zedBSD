/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* One complete state sequence resets inherited binning state before a rebased triangle-array draw. */
#ifndef KERN_DRIVERS_GPU_BCM2711_NATIVE_BIN_H
#define KERN_DRIVERS_GPU_BCM2711_NATIVE_BIN_H

#include <stddef.h>
#include "drivers/gpu/bcm2711/native-viewport.h"

#define BCM2711_NATIVE_BIN_BYTES 116U

/* All addresses borrow complete cleaned storage from the enclosing native draw owner; numerical state owns no DMA lifetime. */
struct bcm2711_native_bin {
	struct bcm2711_native_viewport viewport;
	struct bcm2711_native_viewport_clip clipper;
	uint32_t window[4];
	uint32_t shader;
	uint32_t attributes;
	uint32_t vertices;
	uint32_t forward;
	uint32_t reverse;
	uint32_t clockwise;
	uint32_t flat;
	uint32_t noperspective;
};

int bcm2711_native_bin_encode(const struct bcm2711_native_bin *state, uint8_t *bytes, size_t capacity);

#endif
