/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of the amdrdna executor that turn draws and transfers into
 * GFX10 work: the state a draw writes (vk-state.c), the image and sampler
 * descriptors (vk-descriptor.c), the executor's own quad programs
 * (vk-quad.c) and the float arithmetic done on bits (vk-float.c), since the
 * kernel has no floating point.
 */

#ifndef DRIVERS_GPU_AMDRDNA_VK_AMDRDNA_VK_EXEC_H
#define DRIVERS_GPU_AMDRDNA_VK_AMDRDNA_VK_EXEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/compiler/ir.h"
#include "drivers/gpu/amdrdna/vk/amdrdna-vk-gfx10.h"
#include "drivers/gpu/amdrdna/vk/amdrdna-vk-objects.h"

/* The push constants the executor's quad programs read: the quad's corners, its texture's part, its colour. */
#define AMDRDNA_QUAD_PUSH_RECT		0U
#define AMDRDNA_QUAD_PUSH_TEXTURE	16U
#define AMDRDNA_QUAD_PUSH_COLOUR	32U
#define AMDRDNA_QUAD_PUSH_BYTES		48U

/* The most instructions one of the quad programs has. */
#define AMDRDNA_QUAD_IR_INSTRUCTIONS	128U

/* What amdrdna_quad_depth_ir() writes a texel's bits as: a 32-bit float depth, a 16-bit normalized depth, the stencil. */
#define AMDRDNA_QUAD_DEPTH_32		0U
#define AMDRDNA_QUAD_DEPTH_16		1U
#define AMDRDNA_QUAD_STENCIL		2U

/*
 * A colour target: a 2D image of four-byte pixels at address (256-byte
 * aligned), its size, and the VkFormat it is written in, of samples
 * samples a pixel (0 is taken as 1).  An image of one sample is linear,
 * its rows its width's bytes rounded up to 256 apart, address the layer
 * drawn; one of AMDRDNA_VK_SAMPLES_MAX is in the render swizzle, address
 * its start, layers its layers and slice the one drawn.  The depth unit's
 * surface is described the same way, with its stencil plane at
 * stencil_address (0 for a depth image without stencil).
 */
struct amdrdna_vk_target {
	uint64_t address;
	uint32_t width;
	uint32_t height;
	uint32_t format;
	uint32_t samples;
	uint32_t slice;
	uint32_t layers;
	uint64_t stencil_address;
};

/*
 * The depth and stencil test of the draws: the depth test, its write and
 * compare (VkCompareOp), and the stencil test with each face's.
 */
struct amdrdna_vk_depth_stencil {
	uint32_t test;
	uint32_t write;
	uint32_t compare;
	uint32_t stencil;
	struct amdrdna_vk_stencil_face front;
	struct amdrdna_vk_stencil_face back;
};

/*
 * One of the quad programs being built: the IR and its storage (the
 * instructions, the interface records and the one sampled image).
 */
struct amdrdna_quad_ir {
	struct drv_gpu_shader_ir ir;
	struct drv_gpu_shader_ir_inst instructions[AMDRDNA_QUAD_IR_INSTRUCTIONS];
	struct drv_gpu_shader_ir_io input;
	struct drv_gpu_shader_ir_io output;
	struct drv_gpu_shader_ir_uniform uniform;
};

void amdrdna_vk_state_start(struct amdrdna_vk_pm4 *pm4);
void amdrdna_vk_state_start_kernel(struct amdrdna_vk_pm4 *pm4);
int amdrdna_vk_state_target(struct amdrdna_vk_pm4 *pm4, const struct amdrdna_vk_target *target);
int amdrdna_vk_state_targets(struct amdrdna_vk_pm4 *pm4, const struct amdrdna_vk_target *targets, uint32_t count);
void amdrdna_vk_state_viewport(struct amdrdna_vk_pm4 *pm4, const uint32_t *viewport);
void amdrdna_vk_state_guard_band(struct amdrdna_vk_pm4 *pm4, const uint32_t *viewport, uint32_t topology);
void amdrdna_vk_state_scissor(struct amdrdna_vk_pm4 *pm4, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
int amdrdna_vk_state_blend(struct amdrdna_vk_pm4 *pm4, const struct amdrdna_vk_blend *blend);
int amdrdna_vk_state_blends(struct amdrdna_vk_pm4 *pm4, const struct amdrdna_vk_blend *blends, uint32_t count);
bool amdrdna_vk_blend_dual(const struct amdrdna_vk_blend *blend);
void amdrdna_vk_state_blend_constants(struct amdrdna_vk_pm4 *pm4, const uint32_t *constants);
void amdrdna_vk_state_raster(struct amdrdna_vk_pm4 *pm4, uint32_t cull_mode, uint32_t front_face, bool depth_bias);
void amdrdna_vk_state_depth_bias(struct amdrdna_vk_pm4 *pm4, const uint32_t *bias, uint32_t depth_format);
void amdrdna_vk_state_culling(const uint32_t *viewport, uint32_t topology, uint32_t cull_mode, uint32_t front_face, uint32_t *words);
void amdrdna_vk_state_programs(struct amdrdna_vk_pm4 *pm4, const struct amdrdna_vk_program *vertex, const struct amdrdna_vk_program *fragment, uint64_t vertex_table, uint64_t fragment_table, uint32_t base_vertex, uint32_t first_instance, uint32_t instance_mask, const uint32_t *culling);
void amdrdna_vk_state_program_data(struct amdrdna_vk_pm4 *pm4, const struct amdrdna_vk_program *vertex, uint64_t vertex_table, uint64_t fragment_table, uint32_t base_vertex, uint32_t first_instance, uint32_t instance_mask, const uint32_t *culling);
int amdrdna_vk_state_draw(struct amdrdna_vk_pm4 *pm4, uint32_t topology, bool primitive_shader, uint32_t vertex_count, uint32_t instance_count);
int amdrdna_vk_state_draw_indexed(struct amdrdna_vk_pm4 *pm4, uint32_t topology, bool primitive_shader, uint32_t index_type, uint64_t indices, uint32_t index_room, uint32_t index_count, uint32_t instance_count);
int amdrdna_vk_state_depth_target(struct amdrdna_vk_pm4 *pm4, const struct amdrdna_vk_target *target);
void amdrdna_vk_state_samples(struct amdrdna_vk_pm4 *pm4, uint32_t samples, uint32_t sample_mask, bool alpha_to_coverage);
void amdrdna_vk_state_depth(struct amdrdna_vk_pm4 *pm4, const struct amdrdna_vk_depth_stencil *depth);
void amdrdna_vk_state_kernel(struct amdrdna_vk_pm4 *pm4, const struct amdrdna_vk_program *kernel, uint64_t table);
void amdrdna_vk_state_barrier(struct amdrdna_vk_pm4 *pm4, uint64_t sync_address, uint32_t sync_value);
void amdrdna_vk_state_barrier_kernel(struct amdrdna_vk_pm4 *pm4, uint64_t sync_address, uint32_t sync_value);
bool amdrdna_vk_target_format(uint32_t format, uint32_t *layout);
uint32_t amdrdna_vk_target_export(uint32_t format, bool alpha);
uint32_t amdrdna_vk_pitch(uint32_t width);

bool amdrdna_vk_image_format(uint32_t format, uint32_t *code, bool *alternate);
int amdrdna_vk_image_descriptor(uint64_t address, uint32_t width, uint32_t height, uint32_t format, const uint32_t *swizzle, uint32_t *words);
int amdrdna_vk_depth_descriptor(const struct amdrdna_vk_image *image, uint32_t aspect, bool raw, const uint32_t *swizzle, uint32_t *words);
int amdrdna_vk_bits_descriptor(uint64_t address, uint32_t width, uint32_t height, uint32_t texel_bytes, uint32_t *words);
void amdrdna_vk_image_descriptor_levels(uint32_t *words, uint32_t base, uint32_t last, uint32_t levels);
void amdrdna_vk_image_descriptor_slices(uint32_t *words, uint32_t type, uint32_t slices);
void amdrdna_vk_image_descriptor_samples(uint32_t *words, uint32_t samples, bool arrayed, uint32_t base_layer, uint32_t layers);
void amdrdna_vk_sampler_descriptor(const struct amdrdna_vk_sampler *sampler, uint32_t *words);
int amdrdna_vk_texture_descriptors(const struct amdrdna_vk_descriptor_slot *slot, uint32_t *image_words, uint32_t *sampler_words);
bool amdrdna_vk_vertex_format(uint32_t format, uint32_t *code, uint32_t *components, uint32_t *bytes);
bool amdrdna_vk_texel_format(uint32_t format, uint32_t *code, uint32_t *components, uint32_t *bytes);
int amdrdna_vk_storage_descriptor(const struct amdrdna_vk_descriptor_slot *slot, uint32_t *words);

void amdrdna_quad_vertex_ir(struct amdrdna_quad_ir *program, int textured);
void amdrdna_quad_fragment_ir(struct amdrdna_quad_ir *program, int textured);
void amdrdna_quad_resolve_ir(struct amdrdna_quad_ir *program);
void amdrdna_quad_depth_ir(struct amdrdna_quad_ir *program, uint32_t mode);
void amdrdna_quad_pack_ir(struct amdrdna_quad_ir *program, uint32_t texels);

uint32_t amdrdna_vk_float(int32_t value);
int64_t amdrdna_vk_fixed(uint32_t bits, uint32_t fraction);
uint32_t amdrdna_vk_float_fixed(int64_t value, uint32_t fraction);
uint32_t amdrdna_vk_float_scale(uint32_t bits, uint32_t power);

#endif /* DRIVERS_GPU_AMDRDNA_VK_AMDRDNA_VK_EXEC_H */
