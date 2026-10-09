/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Pure native record encoders borrow already owned GPU intervals and never publish a job. */
#ifndef KERN_DRIVERS_GPU_BCM2711_NATIVE_STATE_H
#define KERN_DRIVERS_GPU_BCM2711_NATIVE_STATE_H

#include "drivers/gpu/bcm2711/shader.h"

#define BCM2711_NATIVE_SHADER_BYTES 36U
#define BCM2711_NATIVE_ATTRIBUTE_BYTES 16U
#define BCM2711_NATIVE_TEXTURE_BYTES 24U
#define BCM2711_NATIVE_SAMPLER_BYTES 24U

/* One immutable program binding borrows code metadata and separately owned uploaded code/uniform intervals. */
struct bcm2711_native_program {
	const struct bcm2711_shader_binary *binary;
	uint32_t code;
	uint32_t uniforms;
};

/* One draw supplies coordinate, vertex and fragment programs plus actual identified VPM capacity. */
struct bcm2711_native_shader {
	struct bcm2711_native_program programs[3];
	uint32_t defaults;
	uint32_t vpm_bytes;
};

/* One native float attribute describes a checked logical fetch interval and the exact leading scalar consumption. */
struct bcm2711_native_attribute {
	uint32_t address;
	uint32_t stride;
	uint32_t maximum_index;
	uint32_t components;
	uint32_t coordinate_values;
	uint32_t vertex_values;
};

/* One single-level RGBA/BGRA texture describes a strict UIF, non-XOR scratch image owned by its native job. */
struct bcm2711_native_texture {
	uint32_t address;
	uint32_t width;
	uint32_t height;
	uint32_t bytes;
	uint32_t swap_red_blue;
};

/* One native sampler uses hardware wrap codes: repeat 0, clamp-to-edge 1, mirrored-repeat 2. */
struct bcm2711_native_sampler {
	uint32_t nearest_mag;
	uint32_t nearest_min;
	uint32_t wrap_u;
	uint32_t wrap_v;
};

int bcm2711_native_shader_encode(const struct bcm2711_native_shader *shader, uint8_t *bytes, size_t capacity);
int bcm2711_native_attribute_encode(const struct bcm2711_native_attribute *attribute, uint8_t *bytes, size_t capacity);
int bcm2711_native_texture_encode(const struct bcm2711_native_texture *texture, uint8_t *bytes, size_t capacity);
int bcm2711_native_sampler_encode(const struct bcm2711_native_sampler *sampler, uint8_t *bytes, size_t capacity);
int bcm2711_native_texture_size(uint32_t width, uint32_t height, uint32_t *bytes);
int bcm2711_native_texture_copy(uint32_t width, uint32_t height, const void *raster, uint32_t pitch, size_t raster_bytes, void *tiled, size_t tiled_bytes);

#endif
