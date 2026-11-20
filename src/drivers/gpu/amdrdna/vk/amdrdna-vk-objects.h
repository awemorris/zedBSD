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
 * Every colour image is linear, four bytes a pixel: the display, the image
 * descriptor and the colour target all read that layout, so nothing is
 * converted between them; its levels follow GFX10's linear mip chain.  An
 * image of one or two bytes a texel (R8_UNORM, R8G8_UNORM) is linear the
 * same way, sampled and copied but never drawn into.  A two-plane 4:2:0
 * image (NV12) is its two planes one after the other, each linear with
 * the same row pitch: a view of one plane samples it, and its planes are
 * copied.  A depth image (D16_UNORM, D32_SFLOAT, or D32_SFLOAT_S8_UINT
 * with its stencil) is laid out in the depth unit's 64 KiB Z swizzle, its
 * stencil a plane of its own after the depth's in the same swizzle: the
 * depth unit draws into it, the texture unit reads it in that swizzle,
 * and its copies are drawn through both.  A buffer or an image
 * names the memory it is bound to by holding that memory's object; the
 * memory's storage arrives with the blob libvulkan creates for it
 * (vk-memory.c).
 */

#ifndef DRIVERS_GPU_AMDRDNA_VK_AMDRDNA_VK_OBJECTS_H
#define DRIVERS_GPU_AMDRDNA_VK_AMDRDNA_VK_OBJECTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/amdrdna/vk/amdrdna-vk.h"
#include "drivers/gpu/amdrdna/compiler/gfx10-compile.h"
#include "drivers/gpu/amdrdna/compiler/gfx10-program.h"

/*
 * An image's rows are its width's bytes rounded up to this many, and an
 * image or a buffer starts on this boundary of its memory: a linear
 * colour target and image descriptor of GFX10 take their row pitch from the
 * width rounded up to 256 bytes, and their address from bit 8.
 */
#define AMDRDNA_VK_PITCH_ALIGN		256U
#define AMDRDNA_VK_RESOURCE_ALIGN		256U

/* The bytes of one pixel of the colour formats and of the depth format. */
#define AMDRDNA_VK_PIXEL_BYTES		4U

/*
 * A two-plane image: its second plane starts on this boundary after the
 * first, and the image on this boundary of its memory, so either plane
 * starts on a page of the GPU's address space.  An image a video engine
 * writes has its planes' rows allocated in whole groups of
 * AMDRDNA_VK_VIDEO_ROWS (the engine writes whole macroblock rows past a
 * height that is not one of them).
 */
#define AMDRDNA_VK_PLANE_ALIGN		4096U
#define AMDRDNA_VK_VIDEO_ROWS		64U

/* The widest and tallest image (the colour target's and the image descriptor's limit). */
#define AMDRDNA_VK_IMAGE_SIDE_MAX		16384U

/* The most levels an image has: down to one pixel from the widest side (the image descriptor's 4-bit level fields). */
#define AMDRDNA_VK_LEVELS_MAX			15U

/*
 * The most layers of a 2D image and the deepest 3D image, as the device
 * reports them (maxImageArrayLayers, maxImageDimension3D; the image
 * descriptor's 13-bit depth field holds more), and the most bytes of one
 * image (maxResourceSize).
 */
#define AMDRDNA_VK_LAYERS_MAX			2048U
#define AMDRDNA_VK_DEPTH_MAX			2048U
#define AMDRDNA_VK_RESOURCE_MAX			(1ULL << 30)

/*
 * A depth image: its depth unit's surface is 64 KiB blocks (the swizzle
 * mode SW_64KB_Z_X, the only Z mode of GFX10), on a 64 KiB boundary of its
 * memory; a block of one sample is 128 by 128 pixels of four bytes
 * (D32_SFLOAT), or 256 wide and 128 tall of two bytes (D16_UNORM).  Its
 * stencil plane (with stencil) is the same swizzle's blocks of one-byte
 * values, 256 by 256 of one sample, on the next 64 KiB boundary after the
 * depth plane.  A block of four samples has a quarter of the pixels.
 *
 * Values referenced: Mesa 25.0.7 src/amd/addrlib/src/gfx10/gfx10addrlib.h
 * (Gfx10ZSwModeMask: ADDR_SW_64KB_Z_X and ADDR_SW_VAR_Z_X),
 * src/amd/addrlib/inc/addrtypes.h (ADDR_SW_64KB_Z_X 24),
 * src/amd/addrlib/src/core/addrlib2.cpp (Lib::ComputeThinBlockDimension:
 * the block's elements split between the width and the height) and
 * src/amd/common/ac_surface.c (gfx9_compute_surface: the stencil computed
 * with 8 bits a texel in the depth's swizzle mode; gfx9_compute_miptree:
 * stencil_offset the depth's size aligned to the stencil's base
 * alignment).
 */
#define AMDRDNA_VK_DEPTH_ALIGN			65536U

/*
 * The samples a pixel of an image may have: one, or four.  An image of
 * four samples (a colour image of four-byte pixels, or a depth image) has
 * one level and is laid out in 64 KiB blocks of the swizzle of its kind,
 * every sample of a pixel in the block (a colour image the colour unit's
 * render swizzle SW_64KB_R_X, of no FMASK: each sample stored whole); its
 * layers lie a slice's bytes apart, and the colour unit and the texture
 * unit find a layer from the image's start by its index.  The executor
 * draws into it and resolves it, the texture unit fetches its samples;
 * it is never copied.
 *
 * Values referenced: Mesa 25.0.7 src/amd/addrlib/src/gfx10/gfx10addrlib.cpp
 * (Gfx10Lib::ValidateSwModeParams and HwlGetPreferredSurfaceSetting:
 * colour MSAA in the render swizzle, not Z; Gfx10MsaaSwModeMask;
 * ComputeSurfaceInfoMacroTiled: one level's slice the pitch and the
 * height aligned to the block times the element's bytes and the
 * samples), src/amd/addrlib/inc/addrtypes.h (ADDR_SW_64KB_R_X 27) and
 * src/amd/vulkan/radv_image.c (RADV_DEBUG_NO_FMASK below GFX11: an MSAA
 * image without FMASK).
 */
#define AMDRDNA_VK_SAMPLES_MAX			4U
#define AMDRDNA_VK_BLOCK_BYTES			65536U

/*
 * The memory types the executor reports: device local only (VRAM the CPU
 * does not reach, heap 0), and device local, host visible and coherent
 * (the CPU's window, heap 1); every buffer and image takes either.
 */
#define AMDRDNA_VK_MEMORY_DEVICE	0U
#define AMDRDNA_VK_MEMORY_HOST		1U
#define AMDRDNA_VK_MEMORY_TYPES		2U

/*
 * A VkDeviceMemory.
 *
 * bytes is the size libvulkan asked for (whole pages).  Once its storage
 * is attached (a blob, or the executor's own outside the CPU's window),
 * cpu is the CPU's view of its first byte (through the VRAM window, or
 * system memory; NULL outside the window), va the GPU's address of it in
 * the session's address space, and backing the node's record the memory
 * holds a reference to.
 */
struct amdrdna_vk_memory {
	uint64_t bytes;
	uint8_t *cpu;
	uint64_t va;
	void *backing;
	bool attached;
};

/*
 * A VkBuffer: its size and usage, and once bound the memory object it
 * holds and its offset in that memory.
 */
struct amdrdna_vk_buffer {
	uint64_t size;
	uint32_t usage;
	struct amdrdna_vk_object *memory;
	uint64_t offset;
};

/*
 * A VkBufferView: the buffer object it holds, the format its texels are
 * read in (one of the formats amdrdna_vk_texel_format() fetches), and
 * the range of the buffer it names (offset and bytes, whole texels).
 */
struct amdrdna_vk_buffer_view {
	struct amdrdna_vk_object *buffer;
	uint32_t format;
	uint64_t offset;
	uint64_t range;
};

/*
 * A VkImage: a 2D image of layers slices, or a 3D image of as many slices
 * as it is deep (type its VkImageType, slices either), of samples samples
 * a pixel (1, or AMDRDNA_VK_SAMPLES_MAX in the 64 KiB blocks of its
 * kind), bytes long, whose memory starts on an align boundary; once bound the
 * memory object it holds and its offset in that memory.  Its slices lie
 * one after the other, slice_bytes apart, each with every level of it (a
 * level of a 3D image has as many slices as it is deep: the first of
 * them); cube says six of its layers may be a cube's faces, and
 * slice_views that 2D views of a 3D image's slices may be drawn into.  A
 * colour image, or one of one or
 * two bytes a texel, has levels levels, pitch linear, level 0's rows pitch
 * bytes apart, texel_bytes a texel (amdrdna_vk_image_level gives each
 * level); a depth image (depth set) has one level, in the depth unit's
 * layout.  A two-plane image (planar set) has one level: its first plane
 * of one byte a texel at its start, its second of two bytes a texel
 * (half as wide and half as tall) at chroma_offset, both rows pitch bytes
 * apart (amdrdna_vk_image_plane gives each plane).  A depth image's
 * texel_bytes are its depth's (2 for D16_UNORM, else 4) and pitch the
 * bytes of a row of its blocks; one with stencil (stencil set) has its
 * stencil plane at stencil_offset.
 */
struct amdrdna_vk_image {
	uint32_t type;
	uint32_t format;
	uint32_t width;
	uint32_t height;
	uint32_t samples;
	uint32_t slices;
	uint64_t slice_bytes;
	bool cube;
	bool slice_views;
	uint32_t usage;
	uint32_t pitch;
	uint32_t levels;
	uint32_t texel_bytes;
	uint32_t align;
	bool depth;
	bool stencil;
	bool planar;
	uint64_t chroma_offset;
	uint64_t stencil_offset;
	uint64_t bytes;
	struct amdrdna_vk_object *memory;
	uint64_t offset;
};

/*
 * A VkImageView: the image object it holds, its VkImageViewType, the
 * format it reads the image as, the aspect it reads
 * (VK_IMAGE_ASPECT_PLANE_0_BIT or _1_BIT for a plane of a two-plane image,
 * else the aspect it was made with), where each of red, green, blue and
 * alpha comes from (VkComponentSwizzle, identity resolved), the levels it
 * reads (the first and how many) and the layers (the first and how many;
 * one, the first, of a 3D view of a 3D image; a 2D or 2D array view of a
 * 3D image's slices, one level, its depth slices).
 */
struct amdrdna_vk_view {
	struct amdrdna_vk_object *image;
	uint32_t type;
	uint32_t format;
	uint32_t aspect;
	uint32_t swizzle[4];
	uint32_t base_level;
	uint32_t level_count;
	uint32_t base_layer;
	uint32_t layer_count;
};

/* The most queries of a query pool. */
#define AMDRDNA_VK_QUERIES_MAX		256U

/*
 * The bytes of one query of an occlusion pool: the depth units' (render
 * backends') counts, each a begin and an end of 8 bytes, for as many
 * backends as the GPU's mask of enabled ones names bits; and of a
 * timestamp pool: one 64-bit timestamp.
 *
 * Values referenced: Mesa 25.0.7 src/amd/vulkan/radv_query.c
 * (radv_create_query_pool: an occlusion query's stride 16 times the
 * render backends, a timestamp query's 8).
 */
#define AMDRDNA_VK_QUERY_BACKENDS_MAX	32U
#define AMDRDNA_VK_QUERY_BACKEND_BYTES	16U
#define AMDRDNA_VK_QUERY_TIMESTAMP_BYTES	8U

/*
 * A VkQueryPool: its type and count queries.  A pool of result status
 * queries (VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR) keeps each query's
 * status (VkQueryResultStatusKHR: 0 not ready, 1 complete, negative a
 * failure) and whether it is available, which the executor writes as it
 * runs a submission's records, in their order, on the CPU; the node's
 * lock serializes that with vkGetQueryPoolResults.  An occlusion or a
 * timestamp pool keeps its queries in GPU memory of its own (token, cpu,
 * va; stride bytes a query), which the GPU writes and
 * vkGetQueryPoolResults reads; backends is the mask of the depth units
 * that write an occlusion query's counts.
 */
struct amdrdna_vk_query_pool {
	uint32_t type;
	uint32_t count;
	uint32_t stride;
	uint32_t backends;
	void *token;
	uint8_t *cpu;
	uint64_t va;
	int32_t status[AMDRDNA_VK_QUERIES_MAX];
	uint8_t available[AMDRDNA_VK_QUERIES_MAX];
};

/*
 * The result status query a submission's video decodes report into while
 * the submission runs: its pool and its query (no pool while none is
 * open).  A begin query opens it, an end query or a coding scope's begin
 * closes it.
 */
struct amdrdna_vk_video_query {
	struct amdrdna_vk_query_pool *pool;
	uint32_t query;
};

/*
 * A VkSampler: the filters and the address modes a texture read uses,
 * whether coordinates are in pixels, and the depth compare a compare read
 * makes (VkCompareOp, when compare_enable is set).
 */
struct amdrdna_vk_sampler {
	uint32_t mag_filter;
	uint32_t min_filter;
	uint32_t mipmap_mode;
	uint32_t address_u;
	uint32_t address_v;
	uint32_t address_w;
	uint32_t border_color;
	uint32_t unnormalized;
	uint32_t compare_enable;
	uint32_t compare_op;
};

/*
 * The binding numbers a descriptor set layout may use (0 .. MAX - 1, and
 * as many bindings: libGLESv2 numbers its uniform blocks from 32, its
 * capture buffer 48, its storage buffers from 56 and its eight image
 * units' storage images from 64), and the sets a pipeline layout holds.
 */
#define AMDRDNA_VK_BINDINGS_MAX		72U
#define AMDRDNA_VK_SETS_MAX		4U

/* The vertex buffers and attributes a pipeline reads. */
#define AMDRDNA_VK_VERTEX_BINDINGS_MAX	4U
#define AMDRDNA_VK_VERTEX_ATTRIBUTES_MAX	8U

/* The sampled images one program reads (each an image and a sampler descriptor in its resource table). */
#define AMDRDNA_VK_TEXTURES_MAX		16U

/* The storage images and storage texel buffers one program reads and writes (an image descriptor's place each). */
#define AMDRDNA_VK_STORAGE_IMAGES_MAX	8U

/* The push constant bytes a pipeline takes (the resource table from 0). */
#define AMDRDNA_VK_PUSH_BYTES		256U

/*
 * A VkShaderModule: a copy of its SPIR-V words, allocated with it.
 */
struct amdrdna_vk_shader {
	uint32_t word_count;
	uint32_t *words;
};

/* One binding of a descriptor set layout: its number, descriptor type and count. */
struct amdrdna_vk_layout_binding {
	uint32_t binding;
	uint32_t type;
	uint32_t count;
};

/*
 * A VkDescriptorSetLayout: its bindings.  Combined image samplers,
 * uniform buffers and storage buffers are bound.
 */
struct amdrdna_vk_set_layout {
	uint32_t count;
	struct amdrdna_vk_layout_binding bindings[AMDRDNA_VK_BINDINGS_MAX];
};

/* A VkPipelineLayout: how many sets it binds and how many push constant bytes it declares. */
struct amdrdna_vk_pipeline_layout {
	uint32_t set_count;
	uint32_t push_bytes;
};

/* A VkDescriptorPool: the most sets it hands out (nothing else is bounded). */
struct amdrdna_vk_descriptor_pool {
	uint32_t max_sets;
};

/* The words of an image descriptor and of a sampler. */
#define AMDRDNA_IMAGE_WORDS		8U
#define AMDRDNA_SAMPLER_WORDS		4U

/*
 * One binding of a descriptor set: the image view and the sampler a
 * combined image sampler names, the buffer, offset and range a uniform
 * or storage buffer names, or the buffer view a uniform texel buffer
 * names (texel); each object held while the set names it.  A
 * combined image sampler's two descriptors are made when the binding is
 * written (words_ready set, the view's image bound to memory then: its
 * address cannot change after), so that a draw copies them.
 */
struct amdrdna_vk_descriptor_slot {
	struct amdrdna_vk_object *view;
	struct amdrdna_vk_object *sampler;
	struct amdrdna_vk_object *buffer;
	struct amdrdna_vk_object *texel;
	uint64_t offset;
	uint64_t range;
	uint32_t image_words[AMDRDNA_IMAGE_WORDS];
	uint32_t sampler_words[AMDRDNA_SAMPLER_WORDS];
	bool words_ready;
};

/* The descriptors one set holds at most (every element of every binding). */
#define AMDRDNA_VK_SET_SLOTS_MAX	256U

/*
 * A VkDescriptorSet: the pool it came from (only compared, to free the
 * pool's sets with it), and from its layout a slot for each element of
 * each binding (slot_count of them in a run of the set's own; a binding's
 * count elements from first, by binding number), and how many dynamic
 * offsets each binding of a dynamic uniform or storage buffer takes from
 * a bind (its descriptor count; 0 for any other binding) and where its
 * first is among the set's (dynamic_first; the bindings' in the order of
 * their numbers, a binding's elements in theirs).
 */
struct amdrdna_vk_descriptor_set {
	const void *pool;
	struct amdrdna_vk_descriptor_slot *slots;
	uint32_t slot_count;
	uint16_t first[AMDRDNA_VK_BINDINGS_MAX];
	uint16_t count[AMDRDNA_VK_BINDINGS_MAX];
	uint8_t dynamic[AMDRDNA_VK_BINDINGS_MAX];
	uint8_t dynamic_first[AMDRDNA_VK_BINDINGS_MAX];
};

/* The dynamic offsets one bind of descriptor sets gives at most (the reported dynamic uniform and storage buffers). */
#define AMDRDNA_VK_DYNAMIC_MAX		12U

/*
 * The colour attachments a subpass writes: one colour target each of the
 * eight GFX10 has (CB_COLOR0 .. CB_COLOR7).
 *
 * Values referenced: Mesa 25.0.7 src/amd/vulkan/radv_constants.h (MAX_RTS
 * 8) and src/amd/vulkan/radv_physical_device.c (maxColorAttachments
 * MAX_RTS).
 */
#define AMDRDNA_VK_COLOURS_MAX		8U

/*
 * A VkRenderPass of one subpass writing at most AMDRDNA_VK_COLOURS_MAX
 * colour attachments and at most one depth attachment, all of samples
 * samples a pixel: how many colour attachments the subpass names
 * (colour_count) and for each its attachment number (VK_ATTACHMENT_UNUSED
 * for none), the attachment it is resolved into at the pass's end
 * (VK_ATTACHMENT_UNUSED for none), format and load operation; whether it
 * writes a depth attachment, its number, format and load operation (the
 * stencil's its own); and the subpass's views under multiview (bit n:
 * view n, drawn into layer n of each attachment's view; 0 for one view).
 */
struct amdrdna_vk_render_pass {
	uint32_t samples;
	uint32_t colour_count;
	uint32_t colour_attachments[AMDRDNA_VK_COLOURS_MAX];
	uint32_t resolve_attachments[AMDRDNA_VK_COLOURS_MAX];
	uint32_t formats[AMDRDNA_VK_COLOURS_MAX];
	uint32_t load_ops[AMDRDNA_VK_COLOURS_MAX];
	uint32_t depth;
	uint32_t depth_format;
	uint32_t depth_load_op;
	uint32_t stencil_load_op;
	uint32_t depth_attachment;
	uint32_t view_mask;
};

/* The most views a multiview subpass renders (maxMultiviewViewCount): view masks of bits 0 to 7. */
#define AMDRDNA_VK_VIEWS_MAX		8U

/* The attachments a render pass and a framebuffer have: the colour ones and a depth one. */
#define AMDRDNA_VK_ATTACHMENTS_MAX	(AMDRDNA_VK_COLOURS_MAX + 1U)

/*
 * A VkFramebuffer: its size and the views of its attachments by number,
 * held (count of them).
 */
struct amdrdna_vk_framebuffer {
	uint32_t width;
	uint32_t height;
	uint32_t count;
	struct amdrdna_vk_object *views[AMDRDNA_VK_ATTACHMENTS_MAX];
};

/*
 * One compiled program of a pipeline: what the compiler made of it (its
 * interface and the layout of its resource table), the register values
 * that run it, and the GPU address of its code (256-byte aligned).
 */
struct amdrdna_vk_program {
	struct amdrdna_gfx10_program *compiled;
	struct amdrdna_gfx10_registers registers;
	uint64_t va;
};

/* A vertex buffer binding of a pipeline: its number, stride and whether it steps per instance. */
struct amdrdna_vk_vertex_binding {
	uint32_t binding;
	uint32_t stride;
	uint32_t instance;
};

/* A vertex attribute of a pipeline: its location, binding, format and offset. */
struct amdrdna_vk_vertex_attribute {
	uint32_t location;
	uint32_t binding;
	uint32_t format;
	uint32_t offset;
};

/*
 * The stencil test of one face: the operations on a failed stencil test,
 * on a passed one and on a failed depth test (VkStencilOp), the compare
 * (VkCompareOp), and the compare mask, write mask and reference.
 */
struct amdrdna_vk_stencil_face {
	uint32_t fail;
	uint32_t pass;
	uint32_t depth_fail;
	uint32_t compare;
	uint32_t compare_mask;
	uint32_t write_mask;
	uint32_t reference;
};

/*
 * The blend of one colour target: whether it blends, the factors and the
 * functions of the colour and of the alpha (VkBlendFactor, VkBlendOp), and
 * the channels written.
 */
struct amdrdna_vk_blend {
	uint32_t enable;
	uint32_t colour_source;
	uint32_t colour_destination;
	uint32_t colour_op;
	uint32_t alpha_source;
	uint32_t alpha_destination;
	uint32_t alpha_op;
	uint32_t write_mask;
};

/*
 * A VkPipeline: its programs in a run of the executor's GPU memory
 * (code_token names the run to the node): a graphics pipeline's vertex and
 * fragment programs, or a compute pipeline's one; for a graphics pipeline
 * the vertex input, the topology, the culling and the depth bias, the
 * samples it rasterizes, its sample mask (the samples' bits) and whether
 * the alpha of colour 0 covers samples, the
 * blend of each colour attachment and the constants, the viewport and
 * scissor, the depth test, write and compare (VkCompareOp) and the
 * stencil test of both faces (floats as their bits).  dynamic has a bit
 * (1 << VkDynamicState) for each state a command buffer sets instead.
 * views holds, for a pipeline of a multiview pass whose programs read
 * gl_ViewIndex, a copy for each further view of the pass with the
 * programs compiled for it (NULL for view 0, which is the pipeline itself,
 * and for every view when the programs do not read it).
 */
struct amdrdna_vk_pipeline {
	void *code_token;
	uint32_t compute;
	struct amdrdna_vk_program vertex;
	struct amdrdna_vk_program fragment;
	struct amdrdna_vk_program kernel;
	uint32_t push_bytes;
	uint32_t binding_count;
	struct amdrdna_vk_vertex_binding bindings[AMDRDNA_VK_VERTEX_BINDINGS_MAX];
	uint32_t attribute_count;
	struct amdrdna_vk_vertex_attribute attributes[AMDRDNA_VK_VERTEX_ATTRIBUTES_MAX];
	uint32_t topology;
	uint32_t cull_mode;
	uint32_t front_face;
	uint32_t samples;
	uint32_t sample_mask;
	uint32_t alpha_to_coverage;
	struct amdrdna_vk_blend blend[AMDRDNA_VK_COLOURS_MAX];
	uint32_t blend_constants[4];
	uint32_t depth_bias_enable;
	uint32_t depth_bias[3];
	uint32_t dynamic;
	uint32_t viewport[6];
	VkRect2D scissor;
	uint32_t depth_test;
	uint32_t depth_write;
	uint32_t depth_compare;
	uint32_t stencil_test;
	struct amdrdna_vk_stencil_face stencil[2];
	struct amdrdna_vk_pipeline *views[AMDRDNA_VK_VIEWS_MAX];
};

/* The regions one copy, blit or clear records. */
#define AMDRDNA_VK_REGIONS_MAX		16U

/* A command buffer's states. */
#define AMDRDNA_VK_COMMAND_INITIAL	0U
#define AMDRDNA_VK_COMMAND_RECORDING	1U
#define AMDRDNA_VK_COMMAND_EXECUTABLE	2U
#define AMDRDNA_VK_COMMAND_INVALID	3U

/*
 * A VkFence: whether it is signaled (created so, or its submission's runs
 * have ended); submitted says a submission named it since its last reset,
 * and runs are that submission's (the newest run of each queue then),
 * which signal it once they have ended.
 */
struct amdrdna_vk_fence {
	bool signaled;
	bool submitted;
	struct amdrdna_vk_ticket runs[2];
};

/*
 * The queue families: the universal one (graphics, compute and transfer,
 * on the graphics queue) and the compute one (on a compute queue of its
 * own).
 */
#define AMDRDNA_VK_FAMILY_UNIVERSAL	0U
#define AMDRDNA_VK_FAMILY_COMPUTE	1U
#define AMDRDNA_VK_FAMILIES		2U

/* A VkCommandPool: its queue family, and compared to free and reset its command buffers with it. */
struct amdrdna_vk_command_pool {
	uint32_t family;
};

/*
 * The clear colours of a pass's beginning (by attachment number) or of a
 * clear of attachments (by colour attachment of the subpass): each
 * colour's four words (as their bits), and a bit (1 << number) for each
 * one given.
 */
struct amdrdna_vk_clear_colours {
	uint32_t mask;
	uint32_t words[AMDRDNA_VK_ATTACHMENTS_MAX][4];
};

/*
 * One recorded command: its opcode, the objects it names (each held while
 * the record lives), and its values.  regions points at its own run of
 * VkImageCopy, VkImageBlit, VkBufferImageCopy or VkClearRect records, and
 * colours at its own clear colours (a pass's beginning, a clear of
 * attachments); a push constant record's count bytes are at push_at in
 * its command buffer's push bytes; a bind of descriptor sets keeps its
 * dynamic offsets in dynamic.
 */
struct amdrdna_vk_record {
	uint32_t opcode;
	struct amdrdna_vk_object *objects[AMDRDNA_VK_SETS_MAX];
	uint32_t object_count;
	uint64_t offsets[AMDRDNA_VK_VERTEX_BINDINGS_MAX];
	uint32_t first;
	uint32_t count;
	uint32_t values[6];
	VkRect2D rect;
	void *regions;
	uint32_t region_count;
	struct amdrdna_vk_clear_colours *colours;
	uint32_t push_at;
	uint32_t dynamic[AMDRDNA_VK_DYNAMIC_MAX];
};

/*
 * A VkCommandBuffer: the pool it came from (only compared), its state, its
 * records in order (a growing array), the bytes of its push constant
 * records one after another (a growing run, push_used of push_capacity in
 * use) and the first error its recording met, which its End reports.  Both
 * runs stay for the next recording.
 */
struct amdrdna_vk_command_buffer {
	const void *pool;
	uint32_t state;
	struct amdrdna_vk_record *records;
	uint32_t record_count;
	uint32_t record_capacity;
	uint8_t *push_bytes;
	uint32_t push_used;
	uint32_t push_capacity;
	int error;
};

int amdrdna_vk_memory_route(struct amdrdna_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int amdrdna_vk_resource_route(struct amdrdna_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int amdrdna_vk_layout_route(struct amdrdna_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int amdrdna_vk_target_route(struct amdrdna_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int amdrdna_vk_pipeline_route(struct amdrdna_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int amdrdna_vk_command_route(struct amdrdna_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int amdrdna_vk_record_route(struct amdrdna_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
void amdrdna_vk_command_reset(struct amdrdna_vk_command_buffer *command);
int amdrdna_vk_video_route(struct amdrdna_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
bool amdrdna_vk_video_recorded(uint32_t opcode);
int amdrdna_vk_video_record(struct amdrdna_vk_session *session, struct amdrdna_vk_command_buffer *command, uint32_t opcode, struct i915_wire_reader *reader, struct amdrdna_vk_record *record);
bool amdrdna_vk_video_waits(const struct amdrdna_vk_record *record);
int amdrdna_vk_video_run(struct amdrdna_vk_session *session, const struct amdrdna_vk_record *record, struct amdrdna_vk_video_query *query, uint64_t wait_us);
int amdrdna_vk_queue_route(struct amdrdna_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int amdrdna_vk_execute(struct amdrdna_vk_session *session, struct amdrdna_vk_command_buffer *command);
void amdrdna_vk_execute_last(struct amdrdna_vk_session *session, struct amdrdna_vk_ticket *last);

uint32_t amdrdna_vk_result(int error);
uint64_t amdrdna_vk_create_tail(struct i915_wire_reader *reader);
void amdrdna_vk_create_reply(struct amdrdna_vk_session *session, struct i915_wire_writer *reply, enum i915_vk_object_kind kind, uint64_t identity, void *payload, int (*destroy)(struct amdrdna_vk_session *, void *), int error);
int amdrdna_vk_destroy_plain(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, enum i915_vk_object_kind kind);
void *amdrdna_vk_payload(struct amdrdna_vk_session *session, enum i915_vk_object_kind kind, uint64_t identity);
int amdrdna_vk_free_payload(struct amdrdna_vk_session *session, void *payload);

struct amdrdna_vk_descriptor_slot *amdrdna_vk_set_slot(const struct amdrdna_vk_descriptor_set *set, uint32_t binding, uint32_t element);

bool amdrdna_vk_format_colour(uint32_t format);
bool amdrdna_vk_format_drawn(uint32_t format);
bool amdrdna_vk_format_depth(uint32_t format);
bool amdrdna_vk_format_stencil(uint32_t format);
bool amdrdna_vk_format_narrow(uint32_t format, uint32_t *texel_bytes);
bool amdrdna_vk_format_storage(uint32_t format, uint32_t *texel_bytes);
bool amdrdna_vk_format_planar(uint32_t format);
bool amdrdna_vk_image_moved(const struct amdrdna_vk_image *image);
int amdrdna_vk_image_level(const struct amdrdna_vk_image *image, uint32_t level, struct amdrdna_vk_image *part);
int amdrdna_vk_image_plane(const struct amdrdna_vk_image *image, uint32_t plane, struct amdrdna_vk_image *part);
int amdrdna_vk_image_part(const struct amdrdna_vk_image *image, uint32_t aspect, uint32_t level, struct amdrdna_vk_image *part);
int amdrdna_vk_image_slice(const struct amdrdna_vk_image *image, uint32_t level, uint32_t slice, struct amdrdna_vk_image *part);
void amdrdna_vk_query_reset(struct amdrdna_vk_query_pool *pool, uint32_t first, uint32_t count);
void amdrdna_vk_query_value(const struct amdrdna_vk_query_pool *pool, uint32_t query, uint64_t *value, uint32_t *available);
uint32_t amdrdna_vk_query_record(uint64_t value, uint32_t available, uint32_t flags, uint8_t *record);
int amdrdna_vk_buffer_address(const struct amdrdna_vk_buffer *buffer, uint64_t *va, uint8_t **cpu);
int amdrdna_vk_image_address(const struct amdrdna_vk_image *image, uint64_t *va, uint8_t **cpu);

#endif /* DRIVERS_GPU_AMDRDNA_VK_AMDRDNA_VK_OBJECTS_H */
