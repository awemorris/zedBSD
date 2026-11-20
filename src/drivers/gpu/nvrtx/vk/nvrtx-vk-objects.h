/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's objects: what a published identity of each kind stands for,
 * and the handlers that make them.
 *
 * Every image is pitch linear, four bytes a pixel: the display, the
 * texture header and the colour target all read that layout, so nothing
 * is converted between them.  A buffer or an image names the memory it is
 * bound to by holding that memory's object; the memory's storage arrives
 * with the blob libvulkan creates for it (vk-memory.c).
 */

#ifndef DRIVERS_GPU_NVRTX_VK_OBJECTS_H
#define DRIVERS_GPU_NVRTX_VK_OBJECTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/nvrtx/vk/nvrtx-exec.h"
#include "drivers/gpu/nvrtx/vk/nvrtx-vk.h"

/*
 * An image's rows are this many bytes apart at least (a multiple of it),
 * and an image or a buffer starts on this boundary of its memory: the
 * display's offset (256) and stride (64), the texture header's pitch (32)
 * and the colour target's pitch (128) all divide it.
 */
#define NVRTX_VK_PITCH_ALIGN		256U
#define NVRTX_VK_RESOURCE_ALIGN		256U

/* The bytes of one pixel of every image format the executor takes. */
#define NVRTX_VK_PIXEL_BYTES		4U

/* The widest and tallest image (the colour target's and the texture header's limit). */
#define NVRTX_VK_IMAGE_SIDE_MAX		16384U

/*
 * The most layers of an image (and the most layer-faces of a cube map
 * array), and the largest side of a 3D image: what the device reports
 * (maxImageArrayLayers, maxImageDimension3D), within the texture header's
 * depth field and the colour target's third dimension.
 */
#define NVRTX_VK_IMAGE_LAYERS_MAX	2048U
#define NVRTX_VK_IMAGE_3D_SIDE_MAX	2048U

/*
 * The memory types the executor reports: the VRAM (device local, host
 * visible and coherent through the CPU's uncached window), and RAM (host
 * visible, coherent and cached: the GPU's accesses snoop the CPU's cache)
 * that only buffers take, for what the CPU reads back.
 */
#define NVRTX_VK_MEMORY_TYPE		0U
#define NVRTX_VK_MEMORY_SYSTEM		1U
#define NVRTX_VK_MEMORY_TYPES		2U

/*
 * The system heap's size reported: a share of the RAM the executor's
 * buffers of the cached type may take (each allocation is one contiguous
 * run, at most a blob's size).
 */
#define NVRTX_VK_SYSTEM_HEAP_BYTES	(1ULL << 30)

/*
 * A VkDeviceMemory.
 *
 * bytes is the size libvulkan asked for (whole pages).  Once the blob for
 * it is attached, cpu is the CPU's view of its first byte (through the
 * BAR1 window, or the direct map for RAM), va the GPU's address of it in
 * the work channel's space, and backing the node's record the memory holds
 * a reference to.  system is set for the cached type (its storage is RAM).
 */
struct nvrtx_vk_memory {
	uint64_t bytes;
	bool system;
	uint8_t *cpu;
	uint64_t va;
	void *backing;
	bool attached;
};

/*
 * A VkBuffer: its size and usage, and once bound the memory object it
 * holds and its offset in that memory.
 */
struct nvrtx_vk_buffer {
	uint64_t size;
	uint32_t usage;
	struct nvrtx_vk_object *memory;
	uint64_t offset;
};

/*
 * A VkBufferView: the buffer it holds, its format and the bytes it sees
 * (offset and range, whole texels).  The executor keeps the texture header
 * it made last for the view (header, valid once header_va is not 0): it
 * holds while the buffer's storage is at header_va.
 */
struct nvrtx_vk_buffer_view {
	struct nvrtx_vk_object *buffer;
	uint32_t format;
	uint64_t offset;
	uint64_t range;
	uint64_t header_va;
	uint32_t header[8];
};

/* The levels an image has at most (a side of NVRTX_VK_IMAGE_SIDE_MAX halved to one). */
#define NVRTX_VK_LEVELS_MAX		15U

/*
 * A depth image is block linear in blocks of one GOB: its rows are a whole
 * number of GOBs wide and its height a whole number of GOBs tall, and it
 * starts on a GOB.
 *
 * Value referenced: Mesa 25.0.7 src/nouveau/nil/tiling.rs (the Fermi GOB of
 * 64 bytes by 8 rows, 512 bytes).
 */
#define NVRTX_VK_GOB_WIDTH		64U
#define NVRTX_VK_GOB_HEIGHT		8U
#define NVRTX_VK_GOB_BYTES		512U

/*
 * A depth image of a depth kind takes whole pages of its memory: its pages
 * are of its format's memory kind (Z16, S8Z24, ZF32_X24S8) while it is a
 * depth target, which no other resource's bytes may share.
 */
#define NVRTX_VK_KIND_ALIGN		4096U

/*
 * The two-plane 4:2:0 format of the video decoder's pictures, and the
 * boundary a video image starts on in its memory (whole blocks of its
 * block-linear planes).
 */
#define NVRTX_VK_FORMAT_NV12		VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
#define NVRTX_VK_VIDEO_ALIGN		4096U

/*
 * A VkImage: an image of levels levels and one sample, bytes long (a 2D
 * image of one layer unless said below); level n starts level_offset[n] bytes in, its rows level_pitch[n]
 * bytes apart (level 0's is pitch).  A colour image of one level is pitch
 * linear; one of more levels is tiled: block linear in one-GOB blocks, each
 * level a block-linear surface of its own after the one before (the
 * texture header's mipmapped layout for blocks one GOB high).  A depth
 * image (D16_UNORM, D32_SFLOAT, D24_UNORM_S8_UINT or D32_SFLOAT_S8_UINT,
 * the last two with a stencil) is tiled the same way whatever its levels
 * and layers; one whose format has a depth kind (kind, 0 for none) takes
 * whole pages, which are of that kind once kinded is set (its first use
 * as a depth target).
 * A video image (NV12, one level) has two block-linear planes in blocks
 * two GOBs high, each pitch bytes a row, plane n at plane_offset[n] and
 * plane_rows[n] rows tall: the luma bytes, then the chroma pairs at half
 * the height; only the video decoder writes it and only the copy engine
 * reads it out.  Once bound, the memory object it holds and its offset in
 * that memory.
 * A colour image of more than one layer (a 2D array, a cube map's six
 * faces, a cube map array's layer-faces) or a 3D image is tiled whatever
 * its levels: its layers come layer_bytes apart, each layer the image's
 * levels one after another, and a 3D image's level its slices one after
 * another, each slice a block-linear surface of the level's rows (blocks
 * one GOB deep).  type is the VkImageType: a 1D image is laid out as a 2D
 * image one row high, which is how the compiler reads it.  cube is set for
 * an image made cube compatible.  samples is its samples a pixel (1, or 4
 * for a 2D attachment of one level laid out as twice its width and height
 * in samples, its pitches the sample rows'; width and height stay the
 * pixels').
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/nil/image.rs (Image::new:
 * the array stride the levels' sizes rounded up to level 0's block, a 3D
 * level's slices one block deep each).
 */
struct nvrtx_vk_image {
	uint32_t format;
	uint32_t width;
	uint32_t height;
	uint32_t usage;
	uint32_t pitch;
	uint64_t bytes;
	uint32_t levels;
	uint32_t type;
	uint32_t layers;
	uint32_t slices;
	uint32_t samples;
	uint64_t layer_bytes;
	bool cube;
	bool depth;
	bool stencil;
	uint32_t kind;
	bool kinded;
	bool tiled;
	bool video;
	uint64_t plane_offset[2];
	uint32_t plane_rows[2];
	uint64_t level_offset[NVRTX_VK_LEVELS_MAX];
	uint32_t level_pitch[NVRTX_VK_LEVELS_MAX];
	struct nvrtx_vk_object *memory;
	uint64_t offset;
};

/*
 * A VkImageView: the image object it holds, the format it reads the image
 * as, the first level it sees and how many, where each of red, green,
 * blue and alpha comes from (VkComponentSwizzle, identity resolved), and
 * for a view of one plane of a video image the plane (1 the luma plane as
 * R8, 2 the chroma plane as R8G8; 0 any other view).  The executor keeps
 * the texture header it made last for the view (header, valid once
 * header_va is not 0): it holds while the image's storage is at
 * header_va.  type is the VkImageViewType; the view sees layer_count
 * layers from base_layer (one for a 3D view, which sees every slice).
 */
struct nvrtx_vk_view {
	struct nvrtx_vk_object *image;
	uint32_t format;
	uint32_t type;
	uint32_t base_level;
	uint32_t level_count;
	uint32_t base_layer;
	uint32_t layer_count;
	uint32_t swizzle[4];
	uint32_t plane;
	uint64_t header_va;
	uint32_t header[8];
};

/*
 * A VkSampler: the filters, the levels' range (minLod and maxLod as float
 * bits) and the address modes a texture read uses, whether coordinates
 * are in pixels, and whether a depth texture's reads compare and by which
 * VkCompareOp.  The executor keeps the sampler's words it made (words,
 * valid once words_made is set).
 */
struct nvrtx_vk_sampler {
	uint32_t mag_filter;
	uint32_t min_filter;
	uint32_t mipmap_mode;
	uint32_t min_lod;
	uint32_t max_lod;
	uint32_t address_u;
	uint32_t address_v;
	uint32_t address_w;
	uint32_t border_color;
	uint32_t unnormalized;
	uint32_t compare_enable;
	uint32_t compare_op;
	bool words_made;
	uint32_t words[8];
};

/*
 * The binding numbers a descriptor set layout may use (0 .. MAX - 1, and
 * as many bindings: libGLESv2 numbers its uniform blocks from 32, its
 * capture buffer 48, its storage buffers from 56 and its eight image
 * units' storage images from 64; a set keeps a descriptor for each
 * number), and the sets a pipeline layout holds.
 */
#define NVRTX_VK_BINDINGS_MAX		72U
#define NVRTX_VK_SETS_MAX		4U

/* The dynamic offsets one bind of descriptor sets gives at most (maxDescriptorSetUniformBuffersDynamic). */
#define NVRTX_VK_DYNAMIC_OFFSETS_MAX	8U

/* The vertex buffers and attributes a pipeline reads (the limits the device reports, maxVertexInputBindings and maxVertexInputAttributes). */
#define NVRTX_VK_VERTEX_BINDINGS_MAX	16U
#define NVRTX_VK_VERTEX_ATTRIBUTES_MAX	16U

/* The sampled images one program reads (each a texture handle in its constant buffer 0; maxPerStageDescriptorSampledImages). */
#define NVRTX_VK_TEXTURES_MAX		16U

/*
 * The descriptors one binding of a set holds at most (an array of
 * descriptors; maxDescriptorSetSampledImages, the most of the per-set
 * limits the device reports).
 */
#define NVRTX_VK_ELEMENTS_MAX		96U

/* The push constant bytes a pipeline takes (constant buffer 0 from 0). */
#define NVRTX_VK_PUSH_BYTES		256U

/*
 * A VkShaderModule: a copy of its SPIR-V words, allocated with it.
 */
struct nvrtx_vk_shader {
	uint32_t word_count;
	uint32_t *words;
};

/* One binding of a descriptor set layout: its number, descriptor type and count. */
struct nvrtx_vk_layout_binding {
	uint32_t binding;
	uint32_t type;
	uint32_t count;
};

/*
 * A VkDescriptorSetLayout: its bindings.  Combined image samplers and
 * uniform buffers are bound to something.
 */
struct nvrtx_vk_set_layout {
	uint32_t count;
	struct nvrtx_vk_layout_binding bindings[NVRTX_VK_BINDINGS_MAX];
};

/* A VkPipelineLayout: how many sets it binds and how many push constant bytes it declares. */
struct nvrtx_vk_pipeline_layout {
	uint32_t set_count;
	uint32_t push_bytes;
};

/* A VkDescriptorPool: the most sets it hands out (nothing else is bounded). */
struct nvrtx_vk_descriptor_pool {
	uint32_t max_sets;
};

/*
 * One descriptor of a descriptor set: the image view and the sampler a
 * combined image sampler names, the buffer, offset and range a uniform
 * buffer names, or the buffer view a uniform texel buffer names (texel),
 * each object held while the set names it.  dynamic is the
 * dynamic offsets the binding takes from a bind (its descriptor count for
 * a dynamic uniform or storage buffer, else 0), from the set's layout, on
 * the binding's first descriptor.
 */
struct nvrtx_vk_descriptor_slot {
	struct nvrtx_vk_object *view;
	struct nvrtx_vk_object *sampler;
	struct nvrtx_vk_object *buffer;
	struct nvrtx_vk_object *texel;
	uint64_t offset;
	uint64_t range;
	uint32_t dynamic;
};

/*
 * A VkDescriptorSet: the pool it came from (only compared, to free the
 * pool's sets with it), its bindings' first descriptors by number, and the
 * other descriptors of the bindings that are arrays: binding b's element e
 * (1 <= e < count[b]) is elements[first[b] + e - 1], allocated with the set
 * from its layout (nvrtx_vk_descriptor_find()).  The bindings that take a
 * bind's dynamic offsets are listed in the order of their numbers
 * (dynamic_bindings, dynamic_binding_count), with the offsets they take in
 * all (dynamic_total), so a bind does not look through every number.
 */
struct nvrtx_vk_descriptor_set {
	const void *pool;
	struct nvrtx_vk_descriptor_slot slots[NVRTX_VK_BINDINGS_MAX];
	struct nvrtx_vk_descriptor_slot *elements;
	uint32_t element_count;
	uint32_t first[NVRTX_VK_BINDINGS_MAX];
	uint32_t count[NVRTX_VK_BINDINGS_MAX];
	uint32_t dynamic_bindings[NVRTX_VK_BINDINGS_MAX];
	uint32_t dynamic_binding_count;
	uint32_t dynamic_total;
};

/* The attachments a render pass and a framebuffer have at most (four colour, one depth). */
#define NVRTX_VK_ATTACHMENTS_MAX	(NVRTX_EXEC_COLOURS + 1U)

/*
 * The views a multiview render pass may render (maxMultiviewViewCount):
 * bits 0 to 7 of its subpass's view mask, view v drawn into layer v of each
 * attachment's view.
 */
#define NVRTX_VK_VIEWS_MAX		8U

/*
 * A VkRenderPass of one subpass writing at most four colour attachments
 * and at most one depth attachment: how many colour attachments the
 * subpass names (colour, 0 for none) and for each its number among the
 * pass's attachments (VK_ATTACHMENT_UNUSED for none), its format and its
 * load operation; whether it writes a depth attachment, its number and
 * load operation and its stencil load operation.  The first colour
 * attachment is in colour_attachment, format and load_op too.  samples is
 * the samples a pixel of its attachments (1 or 4); resolves[n] the
 * attachment colour attachment n is resolved into at the pass's end
 * (VK_ATTACHMENT_UNUSED for none).
 */
struct nvrtx_vk_render_pass {
	uint32_t colour;
	uint32_t colour_attachment;
	uint32_t format;
	uint32_t load_op;
	uint32_t colour_attachments[NVRTX_EXEC_COLOURS];
	uint32_t formats[NVRTX_EXEC_COLOURS];
	uint32_t load_ops[NVRTX_EXEC_COLOURS];
	uint32_t depth;
	uint32_t depth_attachment;
	uint32_t depth_load_op;
	uint32_t stencil_load_op;
	uint32_t samples;
	uint32_t resolves[NVRTX_EXEC_COLOURS];

	/* The views the subpass renders (VkRenderPassMultiviewCreateInfo); zero for a pass of one view. */
	uint32_t view_mask;
};

/* A VkFramebuffer: its size, its layers (a layered pass draws into each) and the views of its attachments in order, held. */
struct nvrtx_vk_framebuffer {
	uint32_t width;
	uint32_t height;
	uint32_t layers;
	uint32_t view_count;
	struct nvrtx_vk_object *views[NVRTX_VK_ATTACHMENTS_MAX];
};

/* Where a program's sampled image or buffer comes from: a descriptor set, a binding and the element of its array (0 for one that is not). */
struct nvrtx_vk_texture_source {
	uint32_t set;
	uint32_t binding;
	uint32_t element;
};

/* The uniform buffers one program reads (constant buffers 1 on). */
#define NVRTX_VK_UNIFORMS_MAX		7U

/*
 * The storage buffers one program reads and writes (its entries of the
 * storage table in its constant buffer 0, the compiler's 32), uniform
 * buffers past the constant buffers among them; a compute program's group
 * counts too.
 */
#define NVRTX_VK_STORAGE_MAX		32U

/*
 * One compiled program of a pipeline: its address (its header, the code
 * after it), its registers, the descriptors its texture handles come
 * from, in the order of their slots in its constant buffer 0, the
 * descriptors of its storage table's entries in their order, and the
 * descriptors of its uniform buffers, in the order of their constant
 * buffers from 1.
 */
struct nvrtx_vk_program {
	uint64_t va;
	uint32_t gprs;
	uint32_t texture_count;
	struct nvrtx_vk_texture_source textures[NVRTX_VK_TEXTURES_MAX];
	uint32_t storage_count;
	struct nvrtx_vk_texture_source storage[NVRTX_VK_STORAGE_MAX];
	uint32_t uniform_count;
	struct nvrtx_vk_texture_source uniforms[NVRTX_VK_UNIFORMS_MAX];
};

/*
 * The descriptor set a compute program names for the buffer of its
 * dispatch's group counts (gl_NumWorkGroups); no layout has it.
 */
#define NVRTX_VK_SYSTEM_SET		0xffffffffU

/*
 * The compiled program of a compute pipeline: its address (code alone, no
 * header), its registers, its workgroup's size, its shared memory in bytes
 * and its workgroup barriers (what a launch's QMD says of it), the
 * descriptors of its storage buffers in the order of their entries of the
 * storage table (set NVRTX_VK_SYSTEM_SET for the group counts), and those
 * of its uniform buffers in the order of their constant buffers from 1.
 */
struct nvrtx_vk_kernel {
	uint64_t va;
	uint32_t gprs;
	uint32_t local_size[3];
	uint32_t shared_bytes;
	uint32_t barrier_count;
	uint32_t storage_count;
	struct nvrtx_vk_texture_source storage[NVRTX_VK_STORAGE_MAX];
	uint32_t uniform_count;
	struct nvrtx_vk_texture_source uniforms[NVRTX_VK_UNIFORMS_MAX];
};

/* A vertex buffer binding of a pipeline: its number, stride and whether it steps per instance. */
struct nvrtx_vk_vertex_binding {
	uint32_t binding;
	uint32_t stride;
	uint32_t instance;
};

/* A vertex attribute of a pipeline: its location, binding, format and offset. */
struct nvrtx_vk_vertex_attribute {
	uint32_t location;
	uint32_t binding;
	uint32_t format;
	uint32_t offset;
};

/*
 * The state a graphics pipeline takes from the command buffer
 * (NVRTX_VK_DYNAMIC_* bits of its dynamic mask) instead of keeping it.
 */
#define NVRTX_VK_DYNAMIC_DEPTH_BIAS		(1U << 0)
#define NVRTX_VK_DYNAMIC_BLEND_CONSTANTS	(1U << 1)
#define NVRTX_VK_DYNAMIC_STENCIL_COMPARE	(1U << 2)
#define NVRTX_VK_DYNAMIC_STENCIL_WRITE		(1U << 3)
#define NVRTX_VK_DYNAMIC_STENCIL_REFERENCE	(1U << 4)

/*
 * A VkPipeline.  A compute pipeline (compute set) has its program, kernel,
 * in a run of the executor's GPU memory (code_token names the run to the
 * node), and nothing else of the record is used.  A graphics pipeline has
 * its two programs (three with a geometry program, whose va is then not
 * 0) in such a run, the vertex input, the
 * topology, the culling, whether its last program before the rasterizer
 * writes the point size (point_size) and the layer (layer), whether primitives are discarded
 * before the rasterizer, the polygon mode (a VkPolygonMode), whether the
 * depth is clamped rather than clipped, the depth bias (whether it is on, its constant
 * factor, clamp and slope factor), the depth and stencil tests, the blend
 * of each colour attachment (colour_count of them, at least the first's
 * kept), the logic operation (whether it is on, its VkLogicOp) and the
 * blend constants, the viewport and scissor
 * unless they are dynamic, and the other state it takes from the command
 * buffer (dynamic, NVRTX_VK_DYNAMIC_* bits); floats as their bits.
 */
struct nvrtx_vk_pipeline {
	void *code_token;
	bool compute;
	struct nvrtx_vk_kernel kernel;
	struct nvrtx_vk_program vertex;
	struct nvrtx_vk_program fragment;
	struct nvrtx_vk_program geometry;
	uint32_t push_bytes;
	uint32_t binding_count;
	struct nvrtx_vk_vertex_binding bindings[NVRTX_VK_VERTEX_BINDINGS_MAX];
	uint32_t attribute_count;
	struct nvrtx_vk_vertex_attribute attributes[NVRTX_VK_VERTEX_ATTRIBUTES_MAX];
	uint32_t topology;
	uint32_t cull_mode;
	uint32_t front_face;
	uint32_t point_size;
	uint32_t layer;
	uint32_t raster_discard;
	uint32_t polygon_mode;
	uint32_t depth_clamp;
	uint32_t depth_bias_enable;
	uint32_t depth_bias[3];
	uint32_t depth_test;
	uint32_t depth_write;
	uint32_t depth_compare;
	uint32_t stencil_test;
	struct nvrtx_exec_stencil_face stencil_front;
	struct nvrtx_exec_stencil_face stencil_back;
	uint32_t colour_count;
	uint32_t samples;
	uint32_t alpha_coverage;
	uint32_t sample_mask;
	struct nvrtx_exec_blend blend[NVRTX_EXEC_COLOURS];
	uint32_t logic_op_enable;
	uint32_t logic_op;
	uint32_t blend_constants[4];
	uint32_t dynamic_viewport;
	uint32_t dynamic_scissor;
	uint32_t dynamic;
	uint32_t viewport[6];
	VkRect2D scissor;

	/*
	 * A compute pipeline made for vkCmdDispatchBase: its program counts
	 * the groups from the base the dispatch places after the group counts.
	 */
	uint32_t dispatch_base;

	/*
	 * The views of the subpass a graphics pipeline is made for, and for
	 * each view past the first a pipeline of the same state whose programs
	 * read that view as gl_ViewIndex (NULL where this one's programs serve:
	 * its first view, and every view when no program reads gl_ViewIndex).
	 * They are destroyed with it.
	 */
	uint32_t view_mask;
	struct nvrtx_vk_pipeline *views[NVRTX_VK_VIEWS_MAX];
};

/* The objects one record holds at most: a bind's sets, or its vertex buffers. */
#define NVRTX_VK_RECORD_OBJECTS		NVRTX_VK_VERTEX_BINDINGS_MAX

/* The regions one blit or clear records. */
#define NVRTX_VK_REGIONS_MAX		16U

/*
 * The regions one copy records: every level of every face of a cube map,
 * with a depth and stencil format's two aspects (libGLESv2 uploads a
 * texture in one copy of 6 x 15 x 2 regions).
 */
#define NVRTX_VK_COPY_REGIONS_MAX	256U

/*
 * The draws one indirect draw command reads at most (maxDrawIndirectCount:
 * the least Vulkan allows with multiDrawIndirect).  The executor reads the
 * records with the CPU and writes each draw as a direct one.
 */
#define NVRTX_VK_DRAW_INDIRECT_MAX	65535U

/* A command buffer's states. */
#define NVRTX_VK_COMMAND_INITIAL	0U
#define NVRTX_VK_COMMAND_RECORDING	1U
#define NVRTX_VK_COMMAND_EXECUTABLE	2U
#define NVRTX_VK_COMMAND_INVALID	3U

/*
 * A VkFence: whether it is signaled, and whether a submission named it and
 * the session's newest run then, which signals it once it has ended.
 */
struct nvrtx_vk_fence {
	bool signaled;
	bool submitted;
	struct nvrtx_vk_ticket run;
};

/* A VkCommandPool: only compared, to free and reset its command buffers with it. */
struct nvrtx_vk_command_pool {
	uint32_t family;
};

/*
 * One recorded command: its opcode, the objects it names (each held while
 * the record lives), and its values.  regions points at its own run of
 * VkImageCopy, VkImageBlit, VkBufferImageCopy or VkClearRect records (an
 * update of a buffer's region_count bytes; it and a fill keep the offset in
 * offsets[0], a fill its size in offsets[1] and its word in values[0]); a
 * push constant record's count bytes are at push_at in its command
 * buffer's push bytes; a bind of descriptor sets keeps its dynamic offsets
 * (dynamic_count of them); a bind of a pipeline or of descriptor sets
 * keeps its bind point in values[0] (0, VK_PIPELINE_BIND_POINT_GRAPHICS,
 * unless it is the compute one), and a dispatch its group counts in
 * values[0] to values[2].
 */
struct nvrtx_vk_record {
	uint32_t opcode;
	struct nvrtx_vk_object *objects[NVRTX_VK_RECORD_OBJECTS];
	uint32_t object_count;
	uint64_t offsets[NVRTX_VK_VERTEX_BINDINGS_MAX];
	uint32_t first;
	uint32_t count;
	uint32_t values[6];
	VkRect2D rect;
	void *regions;
	uint32_t region_count;
	uint32_t push_at;
	uint32_t dynamic_count;
	uint32_t dynamic[NVRTX_VK_DYNAMIC_OFFSETS_MAX];
};

/*
 * A VkCommandBuffer: the pool it came from (only compared), its state, its
 * records in order (a growing array), the bytes of its push constant
 * records one after another (a growing run, push_used of push_capacity in
 * use) and the first error its recording met, which its End reports.  Both
 * runs stay for the next recording.
 */
struct nvrtx_vk_command_buffer {
	const void *pool;
	uint32_t state;
	struct nvrtx_vk_record *records;
	uint32_t record_count;
	uint32_t record_capacity;
	uint8_t *push_bytes;
	uint32_t push_used;
	uint32_t push_capacity;
	int error;
};

int nvrtx_vk_memory_route(struct nvrtx_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int nvrtx_vk_resource_route(struct nvrtx_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int nvrtx_vk_layout_route(struct nvrtx_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int nvrtx_vk_target_route(struct nvrtx_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int nvrtx_vk_pipeline_route(struct nvrtx_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int nvrtx_vk_command_route(struct nvrtx_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int nvrtx_vk_record_route(struct nvrtx_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
void nvrtx_vk_command_reset(struct nvrtx_vk_command_buffer *command);
int nvrtx_vk_queue_route(struct nvrtx_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int nvrtx_vk_execute(struct nvrtx_vk_session *session, struct nvrtx_vk_command_buffer *command);
void nvrtx_vk_execute_show_memory(struct nvrtx_vk_session *session);

int nvrtx_vk_video_route(struct nvrtx_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
bool nvrtx_vk_video_recorded(uint32_t opcode);
int nvrtx_vk_video_record(struct nvrtx_vk_session *session, struct nvrtx_vk_command_buffer *command, uint32_t opcode, struct i915_wire_reader *reader, struct nvrtx_vk_record *record);
void nvrtx_vk_video_drop(struct nvrtx_vk_record *record);
int nvrtx_vk_video_run(struct nvrtx_vk_session *session, const struct nvrtx_vk_record *record);
int nvrtx_vk_video_copy_queries(struct nvrtx_vk_session *session, const struct nvrtx_vk_record *record);

/*
 * Where a recorded query command of an occlusion or timestamp pool writes
 * (vk-video.c's pools): the pool's VkQueryType, the command's first query
 * and its queries, a timestamp's VkPipelineStageFlags, the GPU addresses
 * of the first query's availability word (each next query's 4 bytes on)
 * and of its reports (each next query's stride on; an occlusion query's
 * begin report, then its end report 16 bytes on).
 */
struct nvrtx_vk_query_place {
	uint32_t type;
	uint32_t first;
	uint32_t count;
	uint32_t stages;
	uint64_t available;
	uint64_t reports;
	uint32_t stride;
};
bool nvrtx_vk_video_query_place(const struct nvrtx_vk_record *record, struct nvrtx_vk_query_place *place);

uint32_t nvrtx_vk_result(int error);
uint64_t nvrtx_vk_create_tail(struct i915_wire_reader *reader);
void nvrtx_vk_create_reply(struct nvrtx_vk_session *session, struct i915_wire_writer *reply, enum i915_vk_object_kind kind, uint64_t identity, void *payload, int (*destroy)(struct nvrtx_vk_session *, void *), int error);
int nvrtx_vk_destroy_plain(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, enum i915_vk_object_kind kind);
void *nvrtx_vk_payload(struct nvrtx_vk_session *session, enum i915_vk_object_kind kind, uint64_t identity);
int nvrtx_vk_free_payload(struct nvrtx_vk_session *session, void *payload);

bool nvrtx_vk_format_colour(uint32_t format);
bool nvrtx_vk_format_depth(uint32_t format);
uint32_t nvrtx_vk_format_bytes(uint32_t format);
void nvrtx_vk_image_level(const struct nvrtx_vk_image *image, uint32_t level, uint64_t *offset, uint32_t *width, uint32_t *height, uint32_t *pitch);
int nvrtx_vk_image_surface(const struct nvrtx_vk_image *image, uint32_t level, uint32_t layer, uint64_t *offset, uint32_t *width, uint32_t *height, uint32_t *pitch);
uint32_t nvrtx_vk_image_surfaces(const struct nvrtx_vk_image *image, uint32_t level);
int nvrtx_vk_buffer_address(const struct nvrtx_vk_buffer *buffer, uint64_t *va, uint8_t **cpu);

/* Finds a set's descriptor of a binding and element (NULL outside the set's layout). */
const struct nvrtx_vk_descriptor_slot *nvrtx_vk_descriptor_find(const struct nvrtx_vk_descriptor_set *set, uint32_t binding, uint32_t element);
int nvrtx_vk_image_address(const struct nvrtx_vk_image *image, uint64_t *va, uint8_t **cpu);
int nvrtx_vk_buffer_cpu(struct nvrtx_vk_session *session, const struct nvrtx_vk_buffer *buffer, uint8_t **cpu);
int nvrtx_vk_image_cpu(struct nvrtx_vk_session *session, const struct nvrtx_vk_image *image, uint8_t **cpu);

#endif /* DRIVERS_GPU_NVRTX_VK_OBJECTS_H */
