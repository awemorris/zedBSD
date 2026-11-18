/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The JH7110's glue for its Wave511 video decoder: the power domain,
 * clocks and resets around the core, where the core is, and its firmware.
 */

#ifndef KERN_DRIVERS_VF2_VPU_H
#define KERN_DRIVERS_VF2_VPU_H

#include <stddef.h>
#include <stdint.h>

#include <drivers/generic/fdt.h>

struct drv_wave5;

/*
 * Where the board's video decoder is when the device tree does not say.
 *
 * The vendor's tree ships the decoder's node disabled and mainline's has
 * none, so the board keeps this description of its own.  A node the tree
 * enables wins over it.
 */
struct drv_vf2_vpu_place {
	uint64_t registers;
	uint64_t register_size;
	uint32_t interrupt;
};

/*
 * Finds the video decoder and starts its bring-up.
 *
 * A kernel with the decoder powers it and boots its firmware on a thread
 * of its own once the root file system is up; a test kernel with the
 * probe publishes /dev/vf2-vpu-probe instead, whose read brings the
 * decoder up, probes it and brings it down again.
 */
int drv_vf2_vpu_attach(const struct drv_fdt *fdt, const struct drv_vf2_vpu_place *builtin);

/*
 * Powers the decoder, boots its firmware and appends what that showed to
 * a text, for the probe.  Reports the core, or NULL with the failure in
 * the text.
 */
struct drv_wave5 *drv_vf2_vpu_bring_up(char *text, size_t capacity, size_t *length);

/* Stops the decoder, holds it in reset, powers it off and frees the core. */
void drv_vf2_vpu_bring_down(struct drv_wave5 *core);

#endif
