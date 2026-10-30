/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The push buffer the Vulkan executor runs.
 *
 * exec-push.c writes the 3D class's methods for what one recorded command
 * needs, touching no hardware, so the host test checks them as the kernel
 * writes them: a colour target in one of the executor's formats, the
 * viewport and scissor, the blend of the target, the vertex input, a draw,
 * a clear of a rectangle, the barrier between work that writes an image
 * and work that reads it, and the texture header and sampler of a view.
 *
 * The kernel has no floating point: the floats a method takes are built
 * from their bits, and the viewport's arithmetic is done in fixed point.
 */

#ifndef DRIVERS_GPU_NVRTX_NVRTX_EXEC_H
#define DRIVERS_GPU_NVRTX_NVRTX_EXEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The vertex attributes the class has, and the streams the executor sets.
 *
 * Value referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_flush_vi_state, the 32 attributes).
 */
#define NVRTX_EXEC_ATTRIBUTES		32U
#define NVRTX_EXEC_STREAMS		4U

/*
 * A colour target: its GPU address, size, pitch (bytes between rows),
 * VkFormat and whether it is block linear in one-GOB blocks (else pitch
 * linear); and the depth target beside it (D32_SFLOAT or
 * D24_UNORM_S8_UINT, four bytes a pixel, block linear of one-GOB blocks):
 * its GPU address (0 for none), its row pitch in bytes, its height and
 * whether it has a stencil.
 */
struct nvrtx_exec_target {
	uint64_t address;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t format;
	uint32_t block_linear;
	uint64_t depth_address;
	uint32_t depth_pitch;
	uint32_t depth_height;
	uint32_t depth_stencil;
};

/*
 * The stencil test of one face: its VkStencilOp for a failed stencil test,
 * a passed one and a failed depth test, its VkCompareOp, and its compare
 * mask, write mask and reference.
 */
struct nvrtx_exec_stencil_face {
	uint32_t fail;
	uint32_t pass;
	uint32_t depth_fail;
	uint32_t compare;
	uint32_t compare_mask;
	uint32_t write_mask;
	uint32_t reference;
};

/*
 * The depth and stencil tests of a draw: whether the depth test tests and
 * writes, and its VkCompareOp; whether the stencil test runs (only with a
 * target that has a stencil), and its front and back faces.
 */
struct nvrtx_exec_depth {
	uint32_t test;
	uint32_t write;
	uint32_t compare;
	uint32_t stencil;
	struct nvrtx_exec_stencil_face front;
	struct nvrtx_exec_stencil_face back;
};

/*
 * The blend of colour target 0: whether it blends, the Vulkan factors and
 * operations of the colour and of the alpha, and the channels written (the
 * VkColorComponentFlags).
 */
struct nvrtx_exec_blend {
	uint32_t enable;
	uint32_t colour_op;
	uint32_t colour_source;
	uint32_t colour_destination;
	uint32_t alpha_op;
	uint32_t alpha_source;
	uint32_t alpha_destination;
	uint32_t write_mask;
};

/* One vertex attribute: its location, stream, offset and VkFormat (a float format). */
struct nvrtx_exec_attribute {
	uint32_t location;
	uint32_t stream;
	uint32_t offset;
	uint32_t format;
};

/*
 * One vertex stream: whether it is bound, its GPU address, the bytes
 * readable from there, its stride and whether it steps per instance.
 */
struct nvrtx_exec_stream {
	uint32_t enabled;
	uint64_t address;
	uint64_t bytes;
	uint32_t stride;
	uint32_t instance;
};

/* The vertex input of a draw: its attributes and its streams. */
struct nvrtx_exec_vertex {
	uint32_t attribute_count;
	struct nvrtx_exec_attribute attributes[NVRTX_EXEC_ATTRIBUTES];
	struct nvrtx_exec_stream streams[NVRTX_EXEC_STREAMS];
};

/*
 * The bases and counts the GPU holds after a draw: the vertex numbers'
 * base (the base vertex index and the vertex id's), the first instance,
 * the instances, and whether primitive restart was written off.
 */
struct nvrtx_exec_bases {
	uint32_t vertex;
	uint32_t instance;
	uint32_t instances;
	bool restart_off;
};

/*
 * A draw: indexed or not, its topology, its first vertex or index and
 * count, its instances from the first, and an indexed draw's vertex
 * offset.
 */
struct nvrtx_exec_draw_args {
	bool indexed;
	uint32_t topology;
	uint32_t first;
	uint32_t count;
	uint32_t instances;
	uint32_t first_instance;
	int32_t vertex_offset;
};

/*
 * A sampler: the Vulkan filters, the mipmap mode and the levels' range
 * (float bits; both zero for level 0 alone), address modes and whether
 * coordinates are in pixels.
 */
struct nvrtx_exec_sampler {
	uint32_t mag_filter;
	uint32_t min_filter;
	uint32_t mipmap_mode;
	uint32_t min_lod;
	uint32_t max_lod;
	uint32_t address_u;
	uint32_t address_v;
	uint32_t address_w;
	uint32_t unnormalized;
};

bool nvrtx_exec_target_format(uint32_t format, uint32_t *code);
size_t nvrtx_exec_target(uint8_t *push, size_t at, const struct nvrtx_exec_target *target);
size_t nvrtx_exec_viewport(uint8_t *push, size_t at, const uint32_t *viewport);
size_t nvrtx_exec_scissor(uint8_t *push, size_t at, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
size_t nvrtx_exec_blend(uint8_t *push, size_t at, const struct nvrtx_exec_blend *blend);
size_t nvrtx_exec_vertex(uint8_t *push, size_t at, const struct nvrtx_exec_vertex *vertex);
size_t nvrtx_exec_draw(uint8_t *push, size_t at, uint32_t topology, uint32_t first, uint32_t count, uint32_t instances, uint32_t first_instance);
size_t nvrtx_exec_draw_indexed(uint8_t *push, size_t at, uint32_t topology, uint32_t first, uint32_t count, uint32_t instances, uint32_t first_instance, int32_t vertex_offset);
size_t nvrtx_exec_index_buffer(uint8_t *push, size_t at, uint64_t address, uint64_t bytes, uint32_t type);
size_t nvrtx_exec_raster(uint8_t *push, size_t at, uint32_t cull_mode, uint32_t front_face);
size_t nvrtx_exec_depth(uint8_t *push, size_t at, const struct nvrtx_exec_depth *depth);
size_t nvrtx_exec_uniform(uint8_t *push, size_t at, uint32_t group, uint32_t slot, uint64_t address, uint32_t size);
size_t nvrtx_exec_constants_select(uint8_t *push, size_t at, uint64_t address, uint32_t size);
size_t nvrtx_exec_constants_load(uint8_t *push, size_t at, uint32_t offset, const uint32_t *words, uint32_t count);
size_t nvrtx_exec_draw_held(uint8_t *push, size_t at, struct nvrtx_exec_bases *held, bool known, const struct nvrtx_exec_draw_args *draw);
size_t nvrtx_exec_clear(uint8_t *push, size_t at, const uint32_t *colour, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
size_t nvrtx_exec_clear_depth(uint8_t *push, size_t at, uint32_t depth, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
size_t nvrtx_exec_clear_stencil(uint8_t *push, size_t at, uint32_t stencil, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
size_t nvrtx_exec_barrier(uint8_t *push, size_t at);

/*
 * One region of a block-linear plane (blocks two GOBs high) copied out to
 * a buffer by the copy engine: the plane's first byte, its bytes a row
 * and its rows, the region's origin in bytes across and rows down, the
 * region's first byte in the buffer and the buffer's row pitch, and the
 * region's bytes a row and rows.
 */
struct nvrtx_exec_plane_copy {
	uint64_t source;
	uint32_t source_pitch;
	uint32_t source_rows;
	uint32_t x_bytes;
	uint32_t y;
	uint64_t destination;
	uint32_t destination_pitch;
	uint32_t line_bytes;
	uint32_t lines;
};

/* The push buffer bytes one plane copy takes at most. */
#define NVRTX_EXEC_PLANE_COPY_BYTES	96U

size_t nvrtx_exec_copy_plane(uint8_t *push, size_t at, const struct nvrtx_exec_plane_copy *copy);
void nvrtx_exec_texture_header(uint32_t *header, uint64_t address, uint32_t width, uint32_t height, uint32_t pitch, uint32_t format, const uint32_t *swizzle);
void nvrtx_exec_texture_header_tiled(uint32_t *header, uint64_t address, uint32_t width, uint32_t height, uint32_t levels, uint32_t base_level, uint32_t level_count, uint32_t format, const uint32_t *swizzle);
void nvrtx_exec_texture_header_plane(uint32_t *header, uint64_t address, uint32_t width, uint32_t height, uint32_t channels, const uint32_t *swizzle);
void nvrtx_exec_sampler_words(uint32_t *words, const struct nvrtx_exec_sampler *sampler);
uint32_t nvrtx_exec_float(int32_t value);
int64_t nvrtx_exec_fixed(uint32_t bits, uint32_t fraction);
uint32_t nvrtx_exec_float_fixed(int64_t value, uint32_t fraction);

#endif /* DRIVERS_GPU_NVRTX_NVRTX_EXEC_H */
