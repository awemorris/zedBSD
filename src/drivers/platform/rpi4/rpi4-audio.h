/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 4's headphone jack (ws207-p002): PWM1's two channels
 * on GPIO 40 and 41, fed by a legacy DMA channel, as an audio backend of
 * /dev/dsp (playback only, 16-bit stereo at 48 or 44.1 kHz).
 */

#ifndef KERN_DRIVERS_RPI4_AUDIO_H
#define KERN_DRIVERS_RPI4_AUDIO_H

#include <stdint.h>

/*
 * Finds the PWM, the DMA controller, the GPIO and the clock manager in
 * the firmware's device tree and publishes the jack.  Returns an errno; a
 * board without them boots without the jack.
 */
int drv_rpi4_audio_init(uint64_t fdt_physical);

#endif
