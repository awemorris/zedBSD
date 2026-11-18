/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 5's HDMI audio: the BCM2712's two HDMI ports, the DMA
 * channel that feeds a port's audio FIFO, and the clock and reset block
 * that gates the ports' audio clock.
 */

#ifndef KERN_DRIVERS_RPI5_HDMI_AUDIO_H
#define KERN_DRIVERS_RPI5_HDMI_AUDIO_H

#include <stdint.h>

/*
 * Finds the HDMI ports, the DMA controller and the clock and reset block
 * in the firmware's device tree, logs the state the firmware left in them,
 * and offers the first port that takes sound to the audio core as a
 * playback device.  Writes nothing until a stream starts.  Returns an
 * errno; ENODEV when no port could carry audio.
 */
int drv_rpi5_hdmi_audio_init(uint64_t fdt_physical);

#endif
