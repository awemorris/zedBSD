/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The boot-time probe of the VideoCore H.264 decoder's output order, built
 * only by test kernels (CONFIG_DRIVER_RPI4_GPU_PROBE).
 */

#ifndef KERN_DRIVERS_RPI4_MMAL_PROBE_H
#define KERN_DRIVERS_RPI4_MMAL_PROBE_H

/* Starts the probe; it reports in the kernel log once VCHIQ connects. */
int drv_rpi4_mmal_probe_start(void);

#endif
