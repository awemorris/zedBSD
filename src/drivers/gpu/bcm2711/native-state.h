/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Pure native record encoders borrow already owned GPU intervals and never publish a job. */
#ifndef KERN_DRIVERS_GPU_BCM2711_NATIVE_STATE_H
#define KERN_DRIVERS_GPU_BCM2711_NATIVE_STATE_H

#include "drivers/gpu/bcm2711/compiler/shader.h"

/*
 * The shader state record: 36 bytes on V3D 4.2, 32 on the Raspberry Pi 5's
 * 7.1, which has no default-value pointer.  The encoder the board builds
 * reports its own size (bcm2711_native_shader_bytes()); the attribute
 * records follow the record at once.
 */
#define BCM2711_NATIVE_SHADER_BYTES 36U
#define BCM2712_NATIVE_SHADER_BYTES 32U
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
	/* Zero: one element per vertex; n: one element per n instances. */
	uint32_t divisor;
};

/*
 * The texel formats a native texture has.  RGBA8 is the zero value, so a
 * texture record cleared and filled without naming one is RGBA8.  R8 and
 * RG8 are the planes of a two-plane YUV picture.
 */
enum bcm2711_native_texel {
	BCM2711_NATIVE_TEXEL_RGBA8 = 0,
	BCM2711_NATIVE_TEXEL_R8,
	BCM2711_NATIVE_TEXEL_RG8
};

/*
 * One single-level texture describes a strict UIF, non-XOR scratch image owned by its native job.
 *
 * swap_red_blue applies only to RGBA8 (a BGRA source).
 */
struct bcm2711_native_texture {
	uint32_t address;
	uint32_t width;
	uint32_t height;
	uint32_t bytes;
	uint32_t swap_red_blue;
	enum bcm2711_native_texel texel;
};

/* One native sampler uses hardware wrap codes: repeat 0, clamp-to-edge 1, mirrored-repeat 2. */
struct bcm2711_native_sampler {
	uint32_t nearest_mag;
	uint32_t nearest_min;
	uint32_t wrap_u;
	uint32_t wrap_v;
};

uint32_t bcm2711_native_shader_bytes(void);
int bcm2711_native_shader_encode(const struct bcm2711_native_shader *shader, uint8_t *bytes, size_t capacity);
int bcm2711_native_attribute_encode(const struct bcm2711_native_attribute *attribute, uint8_t *bytes, size_t capacity);
int bcm2711_native_texture_encode(const struct bcm2711_native_texture *texture, uint8_t *bytes, size_t capacity);
int bcm2711_native_sampler_encode(const struct bcm2711_native_sampler *sampler, uint8_t *bytes, size_t capacity);
int bcm2711_native_texture_size(uint32_t width, uint32_t height, uint32_t *bytes);
int bcm2711_native_texture_size_texel(uint32_t width, uint32_t height, uint32_t texel_bytes, uint32_t *bytes);
int bcm2711_native_texture_offset(uint32_t x, uint32_t y, uint32_t height, uint32_t texel_bytes, uint32_t *offset);
int bcm2711_native_texel_layout(enum bcm2711_native_texel texel, uint32_t *texel_bytes, uint32_t *type);
int bcm2711_native_texture_copy_texel(uint32_t width, uint32_t height, uint32_t texel_bytes, const void *raster, uint32_t pitch, size_t raster_bytes, void *tiled, size_t tiled_bytes);
int bcm2711_native_texture_copy(uint32_t width, uint32_t height, const void *raster, uint32_t pitch, size_t raster_bytes, void *tiled, size_t tiled_bytes);
int bcm2711_native_texture_copy_box(uint32_t width, uint32_t height, const void *raster, uint32_t pitch, size_t raster_bytes, void *tiled, size_t tiled_bytes, const uint32_t *box);

#endif
