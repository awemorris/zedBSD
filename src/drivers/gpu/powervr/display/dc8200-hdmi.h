/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The VisionFive 2's HDMI transmitter, set to a mode by the display
 * driver when it lights the output itself.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_DC8200_HDMI_H
#define KERN_DRIVERS_GPU_POWERVR_DC8200_HDMI_H

#include "drivers/gpu/powervr/display/dc8200-mode.h"

/*
 * Sets the transmitter's PLLs, timing and drive for a mode and starts its
 * output.  registers is the transmitter's mapped window.  ETIMEDOUT means
 * a PLL did not report its lock in time; the output is started anyway.
 */
int powervr_dc_hdmi_start(volatile uint8_t *registers, const struct powervr_dc_mode *mode);

#endif
