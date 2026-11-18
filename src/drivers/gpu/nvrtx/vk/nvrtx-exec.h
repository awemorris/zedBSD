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
 * viewport and scissor, the blend of the target and its constants, the
 * depth bias, the vertex input, a draw, a clear of a rectangle, the
 * barrier between work that writes an image and work that reads it, the
 * reports of the occlusion and timestamp queries, and the texture header
 * and sampler of a view.
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
 * The vertex attributes the class has, and the streams the executor sets
 * (one for each vertex buffer binding a pipeline may have).
 *
 * Value referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_flush_vi_state, the 32 attributes).
 */
#define NVRTX_EXEC_ATTRIBUTES		32U
#define NVRTX_EXEC_STREAMS		16U

/*
 * The colour targets a pass draws into at most (the fragment program's
 * locations 0 to 3, each its own target).
 */
#define NVRTX_EXEC_COLOURS		4U

/*
 * A colour target after the first of a pass of several: its GPU address
 * (0 for an unused attachment, whose writes go nowhere), its pitch (bytes
 * between rows), its height and its VkFormat, and in a layered pass the
 * bytes between its layers; block linear in one-GOB blocks.
 */
struct nvrtx_exec_colour {
	uint64_t address;
	uint32_t pitch;
	uint32_t height;
	uint32_t format;
	uint64_t array_pitch;
};

/*
 * A pass's colour targets and depth target: colour target 0's GPU
 * address, size, pitch (bytes between rows), VkFormat and whether it is
 * block linear in one-GOB blocks (else pitch linear); the number of colour
 * targets (0 for 1) and those after the first (block linear, of target
 * 0's size or more); and the depth target beside them (one of the depth
 * formats, nvrtx_exec_depth_format_find(), block linear of one-GOB
 * blocks): its GPU address (0 for none), its row pitch in bytes, its
 * height, whether it has a stencil and its VkFormat.  A pass of no colour
 * target has colour target 0's address 0 and the depth target's size.
 * samples is the samples a pixel of every target (0 or 1 for one, 4 for
 * four: the heights stay the pixels', the rows and pitches are of the
 * targets' samples, two rows of samples a row of pixels).  layers is the
 * layers of a layered pass (0 or 1 for one), each target's from its
 * address, its array_pitch (colour target 0's, another's, the depth
 * target's depth_array_pitch) bytes apart.
 */
struct nvrtx_exec_target {
	uint64_t address;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t format;
	uint32_t block_linear;
	uint32_t colour_count;
	struct nvrtx_exec_colour others[NVRTX_EXEC_COLOURS - 1U];
	uint64_t depth_address;
	uint32_t depth_pitch;
	uint32_t depth_height;
	uint32_t depth_stencil;
	uint32_t depth_format;
	uint32_t samples;
	uint32_t layers;
	uint64_t array_pitch;
	uint64_t depth_array_pitch;
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
 * The blend of a colour target: whether it blends, the Vulkan factors and
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

/*
 * The depth bias of a draw: whether it is on, and its constant factor,
 * slope factor and clamp (floats as their bits).
 */
struct nvrtx_exec_depth_bias {
	uint32_t enable;
	uint32_t constant;
	uint32_t slope;
	uint32_t clamp;
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
 * (float bits; both zero for level 0 alone), address modes, whether
 * coordinates are in pixels, and whether a depth texture's reads compare
 * against a reference and the VkCompareOp they compare by.
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
	uint32_t compare_enable;
	uint32_t compare_op;
};

/*
 * A colour format the executor samples and renders to (every one of them
 * blends and filters): its VkFormat, its bytes a texel, its colour target
 * format, its texture header's component sizes and data type, the
 * header's sources of its red, green, blue and alpha, and whether its
 * colour is sRGB encoded (sampled decoded, rendered encoded).
 */
struct nvrtx_exec_format {
	uint32_t format;
	uint32_t bytes;
	uint32_t target;
	uint32_t components;
	uint32_t type;
	uint32_t identity[4];
	uint32_t srgb;
};

/*
 * A depth format the executor renders depth into, samples and copies: its
 * VkFormat, its bytes a texel, its depth target format, its texture
 * header's component sizes and the data types of the depth and of the
 * stencil (the components after the first), whether it has a stencil, and
 * the memory kind its pages take as a depth target (0 for the kind every
 * page has).
 */
struct nvrtx_exec_depth_format {
	uint32_t format;
	uint32_t bytes;
	uint32_t target;
	uint32_t components;
	uint32_t depth_type;
	uint32_t stencil_type;
	uint32_t stencil;
	uint32_t kind;
};

const struct nvrtx_exec_format *nvrtx_exec_format_find(uint32_t format);
bool nvrtx_exec_format_integer(uint32_t format);
bool nvrtx_exec_texel_format(uint32_t format);
uint32_t nvrtx_exec_rgb9e5(const uint32_t *colour);
bool nvrtx_exec_vertex_format(uint32_t format);
const struct nvrtx_exec_depth_format *nvrtx_exec_depth_format_find(uint32_t format);
bool nvrtx_exec_target_format(uint32_t format, uint32_t *code);
size_t nvrtx_exec_target(uint8_t *push, size_t at, const struct nvrtx_exec_target *target);
size_t nvrtx_exec_viewport(uint8_t *push, size_t at, const uint32_t *viewport);
size_t nvrtx_exec_scissor(uint8_t *push, size_t at, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
size_t nvrtx_exec_blend(uint8_t *push, size_t at, uint32_t target, const struct nvrtx_exec_blend *blend);
size_t nvrtx_exec_blend_constants(uint8_t *push, size_t at, const uint32_t *constants);
size_t nvrtx_exec_depth_bias(uint8_t *push, size_t at, const struct nvrtx_exec_depth_bias *bias);
size_t nvrtx_exec_vertex(uint8_t *push, size_t at, const struct nvrtx_exec_vertex *vertex);
size_t nvrtx_exec_draw(uint8_t *push, size_t at, uint32_t topology, uint32_t first, uint32_t count, uint32_t instances, uint32_t first_instance);
size_t nvrtx_exec_draw_indexed(uint8_t *push, size_t at, uint32_t topology, uint32_t first, uint32_t count, uint32_t instances, uint32_t first_instance, int32_t vertex_offset);
size_t nvrtx_exec_index_buffer(uint8_t *push, size_t at, uint64_t address, uint64_t bytes, uint32_t type);
size_t nvrtx_exec_sample_mask(uint8_t *push, size_t at, uint32_t mask);
size_t nvrtx_exec_layer_source(uint8_t *push, size_t at, uint32_t from_program);
size_t nvrtx_exec_raster(uint8_t *push, size_t at, uint32_t cull_mode, uint32_t front_face, uint32_t point_size, uint32_t discard, uint32_t polygon_mode, uint32_t depth_clamp);
size_t nvrtx_exec_logic_op(uint8_t *push, size_t at, uint32_t enable, uint32_t op);
size_t nvrtx_exec_alpha_coverage(uint8_t *push, size_t at, uint32_t enable);
size_t nvrtx_exec_depth(uint8_t *push, size_t at, const struct nvrtx_exec_depth *depth);
size_t nvrtx_exec_uniform(uint8_t *push, size_t at, uint32_t group, uint32_t slot, uint64_t address, uint32_t size);
size_t nvrtx_exec_constants_select(uint8_t *push, size_t at, uint64_t address, uint32_t size);
size_t nvrtx_exec_constants_load(uint8_t *push, size_t at, uint32_t offset, const uint32_t *words, uint32_t count);
size_t nvrtx_exec_draw_held(uint8_t *push, size_t at, struct nvrtx_exec_bases *held, bool known, const struct nvrtx_exec_draw_args *draw);
size_t nvrtx_exec_clear(uint8_t *push, size_t at, uint32_t target, uint32_t format, uint32_t layer, const uint32_t *colour, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
size_t nvrtx_exec_clear_depth(uint8_t *push, size_t at, uint32_t layer, uint32_t depth, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
size_t nvrtx_exec_clear_stencil(uint8_t *push, size_t at, uint32_t layer, uint32_t stencil, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
size_t nvrtx_exec_barrier(uint8_t *push, size_t at);
size_t nvrtx_exec_geometry_program(uint8_t *push, size_t at, uint64_t address, uint32_t gprs);
size_t nvrtx_exec_query_occlusion(uint8_t *push, size_t at, uint64_t report, bool begin);
size_t nvrtx_exec_query_timestamp(uint8_t *push, size_t at, uint64_t report, uint32_t stages);
size_t nvrtx_exec_query_available(uint8_t *push, size_t at, uint64_t available, uint32_t value, bool flush);

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

/*
 * One side of a rectangle the copy engine copies: its surface's first
 * byte, its row pitch in bytes, whether it is block linear in one-GOB
 * blocks (else pitch linear) and its rows, and the rectangle's origin in
 * texels across and rows down.
 */
struct nvrtx_exec_copy_side {
	uint64_t address;
	uint32_t pitch;
	uint32_t block_linear;
	uint32_t rows;
	uint32_t x;
	uint32_t y;
};

/*
 * A rectangle the copy engine copies texel by texel with the texels'
 * components remapped: the source and the destination, a component's
 * bytes (1, 2 or 4), the components of a source texel and of a
 * destination texel (1 to 4), for each destination component the source
 * component it takes (0 to 3) or NVRTX_EXEC_COPY_KEEP to leave it as it
 * is, and the rectangle's texels across and rows.
 */
struct nvrtx_exec_rect_copy {
	struct nvrtx_exec_copy_side source;
	struct nvrtx_exec_copy_side destination;
	uint32_t component_bytes;
	uint32_t source_components;
	uint32_t destination_components;
	uint32_t remap[4];
	uint32_t width;
	uint32_t height;
};

/*
 * The remap of a destination component that is not written, and the push
 * buffer bytes one rectangle copy takes at most.
 *
 * Value referenced: NVIDIA open-gpu-doc classes/dma-copy/clc5b5.h
 * (NVC5B5_SET_REMAP_COMPONENTS_DST_X_NO_WRITE).
 */
#define NVRTX_EXEC_COPY_KEEP		6U
#define NVRTX_EXEC_RECT_COPY_BYTES	128U

size_t nvrtx_exec_copy_rect(uint8_t *push, size_t at, const struct nvrtx_exec_rect_copy *copy);

/*
 * The texture types a block-linear texture header names: a 1D view is read
 * as 2D one row high (the compiler passes its coordinates so), and a 1D
 * array view as a 2D array.
 *
 * Values referenced: Mesa 25.0.7
 * src/nouveau/headers/nvidia/classes/clb097tex.h
 * (NVB097_TEXHEAD_BL_TEXTURE_TYPE_TWO_D, _THREE_D, _CUBEMAP,
 * _TWO_D_ARRAY, _CUBEMAP_ARRAY).
 */
#define NVRTX_EXEC_TEXTURE_2D		1U
#define NVRTX_EXEC_TEXTURE_3D		2U
#define NVRTX_EXEC_TEXTURE_CUBE		3U
#define NVRTX_EXEC_TEXTURE_2D_ARRAY	5U
#define NVRTX_EXEC_TEXTURE_CUBE_ARRAY	8U

/*
 * What a block-linear texture header describes: level 0's width and height
 * and its depth (a 3D image's slices, an array's layers, a cube map
 * array's cubes; 1 otherwise), the texture type (NVRTX_EXEC_TEXTURE_*),
 * the image's levels and the view's first level and number of levels, and
 * the samples a pixel (1, or 4 of a multisampled image's 2x2 blocks).
 */
struct nvrtx_exec_texture_shape {
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t type;
	uint32_t levels;
	uint32_t base_level;
	uint32_t level_count;
	uint32_t samples;
};
void nvrtx_exec_texture_header(uint32_t *header, uint64_t address, uint32_t width, uint32_t height, uint32_t pitch, uint32_t format, const uint32_t *swizzle);
void nvrtx_exec_texture_header_tiled(uint32_t *header, uint64_t address, uint32_t width, uint32_t height, uint32_t levels, uint32_t base_level, uint32_t level_count, uint32_t format, const uint32_t *swizzle);
void nvrtx_exec_texture_header_shaped(uint32_t *header, uint64_t address, const struct nvrtx_exec_texture_shape *shape, uint32_t format, const uint32_t *swizzle);
void nvrtx_exec_texture_header_buffer(uint32_t *header, uint64_t address, uint32_t texels, uint32_t format);
void nvrtx_exec_texture_header_plane(uint32_t *header, uint64_t address, uint32_t width, uint32_t height, uint32_t channels, const uint32_t *swizzle);
void nvrtx_exec_sampler_words(uint32_t *words, const struct nvrtx_exec_sampler *sampler);
uint32_t nvrtx_exec_float(int32_t value);
int64_t nvrtx_exec_fixed(uint32_t bits, uint32_t fraction);
uint32_t nvrtx_exec_float_fixed(int64_t value, uint32_t fraction);

#endif /* DRIVERS_GPU_NVRTX_NVRTX_EXEC_H */
