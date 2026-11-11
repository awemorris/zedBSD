/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The frame dump of the boot-time decoder probe (ws236), built only by
 * test kernels (CONFIG_DRIVER_RPI4_GPU_PROBE).
 *
 * The probe keeps copies of a few decoded frames; /dev/mmal-probe reads
 * them back as records, each a text header of DRV_RPI4_MMAL_DUMP_HEADER
 * bytes (one line of key=value words, padded with spaces, ending in a
 * newline) followed by the frame's bytes exactly as the decoder gave them.
 * plan/ws236/tests/dump-compare.py reads the file.
 */

#ifndef KERN_DRIVERS_RPI4_MMAL_DUMP_H
#define KERN_DRIVERS_RPI4_MMAL_DUMP_H

#include <stdint.h>

/* The bytes of a record's header. */
#define DRV_RPI4_MMAL_DUMP_HEADER	512U

/*
 * Keeps a copy of one frame with its header line (without the newline),
 * publishing /dev/mmal-probe the first time.  ENOSPC when the record or
 * byte budget is used up, ENOMEM when no copy can be made.  Thread
 * context only; one caller at a time.
 */
int drv_rpi4_mmal_dump_add(const char *header, const uint8_t *data, uint32_t bytes);

/*
 * Publishes /dev/mmal-probe once; the first write to it calls on_run (in
 * the writer's thread), which starts the probe.  Thread context only.
 */
int drv_rpi4_mmal_dump_publish(void (*on_run)(void));

#endif
