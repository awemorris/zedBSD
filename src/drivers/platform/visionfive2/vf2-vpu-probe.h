/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test kernels' probe of the Wave511 decoder's behaviour.
 */

#ifndef KERN_DRIVERS_VF2_VPU_PROBE_H
#define KERN_DRIVERS_VF2_VPU_PROBE_H

#include <stddef.h>

struct drv_wave5;

/*
 * Decodes the probe's streams in each of its ways on a booted core and
 * appends one line per picture and per run to a text.  Reports the text's
 * new length.
 */
size_t drv_vf2_vpu_probe_run(struct drv_wave5 *core, char *text, size_t capacity, size_t length);

#endif
