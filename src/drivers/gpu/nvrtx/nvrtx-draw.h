/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The first 3D draw of nvrtx (the draw-test stage).
 *
 * draw-push.c holds the parts that touch no hardware, so the host test
 * runs them as the kernel does: the push buffers that set up the 3D class
 * (TURING_A) as a queue is set up, point it at a colour target,
 * bind the programs, the vertex buffer, constant buffer 0 and the texture
 * pools, clear and draw; and the IR of the draw's programs (a quad placed
 * by push constants, filled with one colour or with a texture).  draw.c
 * runs the stage on the GPU, on the graphics channel of the first compute
 * launch (compute.c).
 */

#ifndef DRIVERS_GPU_NVRTX_NVRTX_DRAW_H
#define DRIVERS_GPU_NVRTX_NVRTX_DRAW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/compiler/ir.h"

/*
 * The 3D class of Turing, and the subchannel it is bound to.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc597.h (TURING_A) and
 * src/nouveau/vulkan/nvk_queue.c (nvk_queue_init_context_state).
 */
#define NVRTX_DRAW_CLASS		0xc597U
#define NVRTX_DRAW_SUBCHANNEL		0U

/*
 * The pipeline's program types (SET_PIPELINE_SHADER), also used as their
 * indices.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc597.h
 * (NVC597_SET_PIPELINE_SHADER_TYPE_*) and src/nouveau/vulkan/nvk_shader.c
 * (mesa_to_nv9097_shader_type).
 */
#define NVRTX_DRAW_TYPE_VERTEX		1U
#define NVRTX_DRAW_TYPE_PIXEL		5U
#define NVRTX_DRAW_TYPES		6U

/*
 * The colour target format the draw renders (A8B8G8R8: red in the lowest
 * byte of a word).
 *
 * Value referenced: Mesa 25.3.6 src/nouveau/headers/nvidia/classes/clc597.h
 * (NVC597_SET_COLOR_TARGET_FORMAT_V_A8B8G8R8).
 */
#define NVRTX_DRAW_FORMAT_A8B8G8R8	0xd5U

/* The push constants the draw's programs read: the quad's corners, its texture's part, its colour. */
#define NVRTX_DRAW_PUSH_RECT		0x00U
#define NVRTX_DRAW_PUSH_TEXTURE		0x10U
#define NVRTX_DRAW_PUSH_COLOR		0x20U
#define NVRTX_DRAW_PUSH_BYTES		0x30U

/* The most instructions one of the draw's programs has. */
#define NVRTX_DRAW_IR_INSTRUCTIONS	32U

/*
 * A texture header and a sampler (Turing's TIC and TSC entries) are eight
 * words each.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc597tex.h (the 256-bit
 * NVC597_TEXHEAD_PITCH_* and NVC597_TEXSAMP* layouts).
 */
#define NVRTX_DRAW_TIC_WORDS		8U
#define NVRTX_DRAW_TSC_WORDS		8U

/*
 * A colour target: its GPU address (128-byte aligned), its size in pixels
 * and its pitch in bytes (a multiple of 128); pitch-linear, four bytes a
 * pixel.
 */
struct nvrtx_draw_target {
	uint64_t address;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
};

/*
 * A pitch-linear 2D texture of four bytes a pixel (A8B8G8R8) at a GPU
 * address (256-byte aligned), sampled with nearest filtering and clamped
 * coordinates.
 */
struct nvrtx_draw_texture {
	uint64_t address;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
};

/*
 * The IR of one of the draw's programs and the storage it lives in.
 *
 * Filled by nvrtx_draw_vertex_ir() or nvrtx_draw_fragment_ir(); the IR's
 * lists point into the record, which the caller keeps while the IR is
 * compiled.
 */
struct nvrtx_draw_ir {
	struct drv_gpu_shader_ir ir;
	struct drv_gpu_shader_ir_inst instructions[NVRTX_DRAW_IR_INSTRUCTIONS];
	struct drv_gpu_shader_ir_io input;
	struct drv_gpu_shader_ir_io output;
	struct drv_gpu_shader_ir_uniform uniform;
};

/*
 * Writes the 3D class's binding and the state a new queue is given; reports
 * where the push buffer goes on.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_queue.c
 * (nvk_queue_init_context_state) and nvk_cmd_draw.c
 * (nvk_push_draw_state_init).
 */
size_t nvrtx_push_draw_init(uint8_t *push, size_t at, uint64_t zero_page);

/* Writes the same binding and state for a generation's 3D class; reports where the push buffer goes on. */
size_t nvrtx_push_draw_init_class(uint8_t *push, size_t at, uint64_t zero_page, uint32_t threed_class);

/* Writes constant buffer 0's selection and its binding to every program group; reports where the push buffer goes on. */
size_t nvrtx_push_draw_constants(uint8_t *push, size_t at, uint64_t address, uint32_t size);

/* Writes colour target 0, the viewport and the raster, sample, depth and colour state of one draw; reports where the push buffer goes on. */
size_t nvrtx_push_draw_target(uint8_t *push, size_t at, const struct nvrtx_draw_target *target);

/* Writes the pipeline: a vertex and a pixel program (each at its header), the other stages off, the shader caches untouched; reports where the push buffer goes on. */
size_t nvrtx_push_draw_pipeline(uint8_t *push, size_t at, uint64_t vertex, uint32_t vertex_gprs, uint64_t pixel, uint32_t pixel_gprs);
size_t nvrtx_push_draw_program(uint8_t *push, size_t at, bool pixel, uint64_t address, uint32_t gprs);

/* Writes the invalidation of the shader caches at a push buffer's start, without a wait for idle; reports where the push buffer goes on. */
size_t nvrtx_push_draw_invalidate(uint8_t *push, size_t at);

/* Writes vertex stream 0 (two floats a vertex at attribute 0) and the attributes; reports where the push buffer goes on. */
size_t nvrtx_push_draw_vertices(uint8_t *push, size_t at, uint64_t address, uint32_t bytes, uint32_t stride);

/* Writes the texture header and sampler pools and the invalidation of their caches; reports where the push buffer goes on. */
size_t nvrtx_push_draw_textures(uint8_t *push, size_t at, uint64_t headers, uint32_t header_count, uint64_t samplers, uint32_t sampler_count);

/* Writes a clear of colour target 0 to a colour (four floats, by their bits) over width by height; reports where the push buffer goes on. */
size_t nvrtx_push_draw_clear(uint8_t *push, size_t at, const uint32_t *colour, uint32_t width, uint32_t height);

/* Writes a draw of a triangle list of vertices from first; reports where the push buffer goes on. */
size_t nvrtx_push_draw_triangles(uint8_t *push, size_t at, uint32_t first, uint32_t count);

/* Writes the texture header (TIC) of a texture into eight words. */
void nvrtx_draw_texture_header(uint32_t *header, const struct nvrtx_draw_texture *texture);

/* Writes a sampler (TSC) with nearest filtering and clamped coordinates into eight words. */
void nvrtx_draw_sampler(uint32_t *sampler);

/* Builds the IR of the quad's vertex program; with textured nonzero it also passes the texture coordinate on. */
void nvrtx_draw_vertex_ir(struct nvrtx_draw_ir *program, int textured);

/* Builds the IR of the quad's fragment program: the push constants' colour, or with textured nonzero the texture's sample. */
void nvrtx_draw_fragment_ir(struct nvrtx_draw_ir *program, int textured);

#endif /* DRIVERS_GPU_NVRTX_NVRTX_DRAW_H */
