/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The graphics path of the Vulkan executor: the objects a draw is made of,
 * the recorded form of a command buffer, and the entries that turn one
 * recorded draw into Gen12 commands.
 *
 * Minimal connection, happy path only.  The path exists so that the
 * standard application reaches the GPU through libvulkan and the ordinary
 * ioctls.  What it does not do is said where it does not do it (an `XXX:`
 * comment and a kernel message); nothing is accepted and then dropped.
 *
 *  - memory.c, image.c, descriptor.c, pipeline.c, render-pass.c, sync.c
 *                the objects: memory, buffer, image, view, sampler,
 *                descriptors, layouts, render pass, framebuffer, shader
 *                module, pipeline, semaphore; objects.c routes their
 *                commands
 *  - command.c   command pools and buffers, recording, vkQueueSubmit
 *  - draw.c      recorded draws into state heaps and the submission's
 *                batch, on the GPU
 *
 * A command buffer is recorded as a list of operations, not as GPU
 * commands.  vkQueueSubmit turns the operations of its command buffers, in
 * order, into one batch (clears and copies as rectangles, draws as draws,
 * each with the flushes it needs in front of it), runs the batch once, and
 * the submission is complete when vkQueueSubmit replies.
 */

#ifndef DRIVERS_GPU_I915_RENDER_GFX_H
#define DRIVERS_GPU_I915_RENDER_GFX_H

#include <libc/vulkan/vulkan_core.h>

#include <stdint.h>

struct i915_gem_object;
struct i915_render_device;
struct i915_render_session;
struct i915_shader_binary;
struct i915_wire_reader;
struct i915_wire_writer;

/*
 * The binding numbers one descriptor set holds (0 .. MAX_BINDINGS - 1:
 * libGLESv2 numbers its uniform blocks from 32 and its capture buffer 48),
 * how many bindings one layout lists, and how many of them may be dynamic
 * uniform buffers.
 */
#define I915_GFX_MAX_BINDINGS		64U
#define I915_GFX_MAX_LAYOUT_BINDINGS	32U
#define I915_GFX_MAX_DYNAMIC_BUFFERS	8U

/* How many attachments one render pass and one framebuffer hold, and how many of them one subpass draws colour into. */
#define I915_GFX_MAX_ATTACHMENTS	8U
#define I915_GFX_MAX_COLOR_ATTACHMENTS	4U

/* How many rectangles one vkCmdClearAttachments records. */
#define I915_GFX_MAX_CLEAR_RECTS	16U

/* How many vertex buffer bindings and vertex attributes one pipeline holds. */
#define I915_GFX_MAX_VERTEX_BINDINGS	4U
#define I915_GFX_MAX_VERTEX_ATTRIBUTES	16U

/* The most VUE slots after the position a vertex kernel writes, and fragment inputs a pixel kernel reads. */
#define I915_GFX_MAX_VARYINGS		16U

/* The most workgroups one dispatch has along each axis (the device reports it as maxComputeWorkGroupCount). */
#define I915_GFX_MAX_GROUP_COUNT	65535U

/* How many bytes of push constants a command buffer carries. */
#define I915_GFX_PUSH_BYTES		128U

/* How many descriptor sets a command buffer binds at once. */
#define I915_GFX_BOUND_SETS		4U

/*
 * The kinds of operation a command buffer is recorded as.
 *
 * The values start at one, so a zeroed operation is never mistaken for a
 * recorded one.
 */
enum i915_gfx_op_kind {
	I915_GFX_OP_COPY_BUFFER_TO_IMAGE = 1,
	I915_GFX_OP_COPY_IMAGE_TO_BUFFER,
	I915_GFX_OP_COPY_IMAGE,
	I915_GFX_OP_BLIT_IMAGE,
	I915_GFX_OP_CLEAR_IMAGE,
	I915_GFX_OP_BEGIN_PASS,
	I915_GFX_OP_END_PASS,
	I915_GFX_OP_BIND_PIPELINE,
	I915_GFX_OP_BIND_VERTEX_BUFFER,
	I915_GFX_OP_BIND_DESCRIPTOR_SET,
	I915_GFX_OP_PUSH_CONSTANTS,
	I915_GFX_OP_DRAW,
	I915_GFX_OP_CLEAR_ATTACHMENT,
	I915_GFX_OP_BIND_INDEX_BUFFER,
	I915_GFX_OP_DRAW_INDEXED,
	I915_GFX_OP_SET_VIEWPORT,
	I915_GFX_OP_SET_SCISSOR,
	I915_GFX_OP_COPY_BUFFER,
	I915_GFX_OP_SET_BLEND_CONSTANTS,
	I915_GFX_OP_QUERY_BEGIN,
	I915_GFX_OP_QUERY_END,
	I915_GFX_OP_QUERY_RESET,
	I915_GFX_OP_SET_STENCIL,
	I915_GFX_OP_RESOLVE_IMAGE,
	I915_GFX_OP_DISPATCH,
	I915_GFX_OP_DISPATCH_INDIRECT,
	I915_GFX_OP_VIDEO_BEGIN,
	I915_GFX_OP_VIDEO_CONTROL,
	I915_GFX_OP_VIDEO_DECODE,
	I915_GFX_OP_VIDEO_END
};

/* An occlusion query pool (fence.c). */
struct i915_gfx_query_pool;

/* A recorded video command (video.c). */
struct i915_video_command;

/*
 * One VkDeviceMemory.
 *
 * Its storage is the blob libvulkan exports for it right after the
 * allocation.  The allocation stays on the list of live allocations from its
 * publication to vkFreeMemory, so that the blob can find it by its identity.
 */
struct i915_gfx_memory {
	/* The next live allocation; the list serves blob attach and detach. */
	struct i915_gfx_memory *next;

	/* The executor device the allocation belongs to. */
	struct i915_render_device *vk;

	/* The open of the node that made it; only a blob of the same open is its storage. */
	struct i915_session *gpu;

	/* The wire id; the blob names the allocation by it as its blob_id. */
	uint64_t identity;

	/* The size libvulkan asked for. */
	uint64_t size;

	/* The blob's object; NULL until the blob arrives and after it leaves. */
	struct i915_gem_object *object;
};

/*
 * One VkBuffer: a range of an allocation once it is bound.
 */
struct i915_gfx_buffer {
	/* The size and usage the buffer was created with. */
	uint64_t size;
	uint32_t usage;

	/* The allocation and the offset the buffer is bound at; NULL until bound. */
	struct i915_gfx_memory *memory;
	uint64_t offset;
};

/*
 * One VkImage: a linear image in an allocation, with one or more mip
 * levels and one or more slices (array layers, or a 3D image's depth).
 *
 * A depth image is laid out in whole Y tiles, so its pitch and height are
 * rounded up; a colour image of one level and one slice is linear rows of
 * width * 4 bytes.  The levels of a mipmapped colour image share one pitch
 * and lie in the hardware's 2D mip layout, and every slice is a whole mip
 * layout `slice_rows` rows after the one before (image.c,
 * drv_i915_gfx_image_layout()).  A 1D image is a 2D image one texel high.
 */
struct i915_gfx_image {
	/* The VkFormat, the extent and the usage the image was created with. */
	uint32_t format;
	uint32_t width;
	uint32_t height;
	uint32_t usage;

	/* The bytes to a row, and the bytes the image occupies. */
	uint32_t pitch;
	uint64_t bytes;

	/* The mip levels, at least one. */
	uint32_t levels;

	/*
	 * The VkImageType, a 3D image's depth and the array layers (0 reads as
	 * one), and whether the image was created cube compatible.
	 */
	uint32_t type;
	uint32_t depth;
	uint32_t layers;
	uint32_t cube;

	/* The rows from one slice to the next (the QPitch). */
	uint32_t slice_rows;

	/*
	 * The samples per pixel (1, 2 or 4).  A multisampled colour image is
	 * Y-tiled and keeps each sample as a slice of its own, slice_rows
	 * apart (MSFMT_MSS); a multisampled depth or stencil image interleaves
	 * the samples of a pixel in its planes (MSFMT_DEPTH_STENCIL), whose
	 * extent in samples is sample_width by sample_height.  An image of one
	 * sample has its own extent there.
	 */
	uint32_t samples;
	uint32_t sample_width;
	uint32_t sample_height;

	/*
	 * A format with stencil (D32_SFLOAT_S8_UINT, S8_UINT): nonzero, and the
	 * separate stencil plane's offset from the image's start, its pitch and
	 * its rows from slice to slice (Y-tiled bytes, as Gen12 wants them).
	 */
	uint32_t stencil;
	uint64_t stencil_offset;
	uint32_t stencil_pitch;
	uint32_t stencil_slice_rows;

	/*
	 * A two-plane NV12 picture of the video decoder (ws083): nonzero, laid
	 * out in Y tiles, the interleaved CbCr plane chroma_offset bytes in,
	 * below chroma_rows rows of the Y plane, both at the image's pitch.
	 * The decoder writes it; a standard plane readback copies its R8 or
	 * R8G8 texels.  drv_i915_gfx_image_slice() refuses generic image uses.
	 */
	uint32_t planar;
	uint64_t chroma_offset;
	uint32_t chroma_rows;

	/* The allocation and the offset the image is bound at; NULL until bound. */
	struct i915_gfx_memory *memory;
	uint64_t offset;
};

/*
 * One VkImageView: a range of the image's mip levels, in the format the
 * view was created with.
 */
struct i915_gfx_view {
	/* The image the view shows, and the view's VkFormat. */
	struct i915_gfx_image *image;
	uint32_t format;

	/* The first mip level the view shows and how many, at least one. */
	uint32_t base_level;
	uint32_t level_count;

	/* The VkImageViewType, the first array layer the view shows and how many (0 reads as one). */
	uint32_t view_type;
	uint32_t base_layer;
	uint32_t layer_count;

	/*
	 * The component swizzle as the surface state's shader channel select
	 * word (RENDER_SURFACE_STATE dword 7), taken when swizzle_set says
	 * vkCreateImageView made it (ws031-p033); a view made otherwise (a
	 * test's, zeroed) reads as the identity.
	 */
	uint32_t channel_select;
	uint32_t swizzle_set;
};

/*
 * One VkSampler: the filters, the level selection and the address modes a
 * texture read uses.
 */
struct i915_gfx_sampler {
	/* The magnification and minification filters (VkFilter). */
	uint32_t mag_filter;
	uint32_t min_filter;

	/* The address modes along u and v (VkSamplerAddressMode). */
	uint32_t address_u;
	uint32_t address_v;

	/* How the level is chosen and blended between levels (VkSamplerMipmapMode). */
	uint32_t mipmap_mode;

	/* The LOD bias and the LOD range, as float bits. */
	uint32_t lod_bias;
	uint32_t min_lod;
	uint32_t max_lod;

	/* The address mode along w (VkSamplerAddressMode). */
	uint32_t address_w;

	/* A depth comparison when compare_enable is nonzero (VkCompareOp). */
	uint32_t compare_enable;
	uint32_t compare_op;

	/* The border colour (VkBorderColor), unnormalized coordinates, and anisotropy with its limit (float bits). */
	uint32_t border_color;
	uint32_t unnormalized;
	uint32_t anisotropy_enable;
	uint32_t max_anisotropy;
};

/*
 * One VkDescriptorSetLayout: the bindings a set of that layout has.
 */
struct i915_gfx_dsl {
	/* How many of the bindings below are used. */
	uint32_t count;

	/* The binding number, the VkDescriptorType and the stage flags of each binding. */
	struct {
		uint32_t binding;
		uint32_t type;
		uint32_t stages;
	} bindings[I915_GFX_MAX_LAYOUT_BINDINGS];
};

/*
 * One VkBufferView: a range of a buffer read as texels of one format, the
 * view a uniform texel buffer descriptor names (a GL buffer texture).  The
 * buffer is the application's and outlives every use of the view, as
 * Vulkan requires; the view is one allocation, freed by its destroy.
 */
struct i915_gfx_buffer_view {
	/* The buffer the view reads, and the VkFormat of its texels. */
	struct i915_gfx_buffer *buffer;
	uint32_t format;

	/* The first byte the view reads within the buffer, and how many (VK_WHOLE_SIZE: to the buffer's end). */
	uint64_t offset;
	uint64_t range;
};

/*
 * One VkDescriptorSet: what each binding of its layout was updated to.
 */
struct i915_gfx_dset {
	/*
	 * The layout the set was allocated with: a copy the set keeps
	 * (layout_copy), since the application may destroy the layout while
	 * the set is used (BUG-260); NULL for an unknown layout.
	 */
	struct i915_gfx_dsl *layout;
	struct i915_gfx_dsl layout_copy;

	/* The pool it was allocated from; the pool's destruction frees the set. */
	void *pool;

	/*
	 * What each binding, indexed by binding number, was updated to: the
	 * view and the sampler of a combined image sampler, or the buffer and
	 * the range of a uniform buffer, or the buffer view of a uniform texel
	 * buffer.  A dynamic uniform buffer's range moves by the dynamic
	 * offset its descriptor set bind gives it.
	 */
	struct {
		struct i915_gfx_view *view;
		struct i915_gfx_buffer_view *texel;
		struct i915_gfx_sampler *sampler;
		struct i915_gfx_buffer *buffer;
		uint64_t offset;
		uint64_t range;
		int dynamic;
	} slots[I915_GFX_MAX_BINDINGS];
};

/*
 * One VkRenderPass: one subpass with at most four colour attachments and one
 * depth attachment.
 */
struct i915_gfx_pass {
	/* The format and the load operations of each attachment, the stencil's apart. */
	uint32_t attachment_count;
	struct {
		uint32_t format;
		uint32_t load_op;
		uint32_t stencil_load_op;
	} attachments[I915_GFX_MAX_ATTACHMENTS];

	/*
	 * The attachment indexes the subpass writes, or VK_ATTACHMENT_UNUSED:
	 * the first colour attachment, the depth attachment, and every colour
	 * attachment (color_count of them; a pass filled by hand with none
	 * has only color_attachment, see drv_i915_gfx_pass_color()).
	 */
	uint32_t color_attachment;
	uint32_t depth_attachment;
	uint32_t color_count;
	uint32_t color_attachments[I915_GFX_MAX_COLOR_ATTACHMENTS];
};

/*
 * One VkFramebuffer: the extent and the views a render pass draws into.
 */
struct i915_gfx_framebuffer {
	/* The extent of the framebuffer, and its layers (VkFramebufferCreateInfo.layers, at least one). */
	uint32_t width;
	uint32_t height;
	uint32_t layers;

	/* The view of each attachment; an unknown identity leaves NULL. */
	uint32_t view_count;
	struct i915_gfx_view *views[I915_GFX_MAX_ATTACHMENTS];
};

/*
 * One VkShaderModule: a copy of its SPIR-V words.
 *
 * The words are allocated together with the module and freed with it.
 */
struct i915_gfx_shader {
	/* The SPIR-V words and how many there are. */
	uint32_t *words;
	uint32_t word_count;
};

/*
 * One VkPipeline, graphics or compute: its stages, its fixed-function state
 * and its compiled kernels.
 *
 * The pipeline owns its kernels; they are made when the pipeline is created
 * and released when it is destroyed.  A compute pipeline (bind_point
 * VK_PIPELINE_BIND_POINT_COMPUTE, ws101-p003) has only its compute stage,
 * its kernel and the table of its threads' IDs; the graphics fields stay
 * zero.
 */
struct i915_gfx_pipeline {
	/* The bind point the pipeline is bound at: VK_PIPELINE_BIND_POINT_GRAPHICS (zero) or _COMPUTE. */
	uint32_t bind_point;

	/*
	 * The vertex, geometry and fragment shader modules; NULL when a stage
	 * is absent (the geometry stage is optional, ws075-p007b).
	 */
	struct i915_gfx_shader *vertex;
	struct i915_gfx_shader *geometry;
	struct i915_gfx_shader *fragment;

	/* The vertex buffer bindings: the binding number, the stride and the VkVertexInputRate. */
	uint32_t binding_count;
	struct {
		uint32_t binding;
		uint32_t stride;
		uint32_t input_rate;
	} bindings[I915_GFX_MAX_VERTEX_BINDINGS];

	/* The vertex attributes: the location, binding, VkFormat and offset. */
	uint32_t attribute_count;
	struct {
		uint32_t location;
		uint32_t binding;
		uint32_t format;
		uint32_t offset;
	} attributes[I915_GFX_MAX_VERTEX_ATTRIBUTES];

	/* The VkPrimitiveTopology. */
	uint32_t topology;

	/* x, y, width, height, minDepth and maxDepth of the viewport, as float bits. */
	uint32_t viewport[6];

	/* The scissor rectangle. */
	VkRect2D scissor;

	/*
	 * Nonzero when the viewport or the scissor is dynamic state: a draw then
	 * takes the one vkCmdSetViewport or vkCmdSetScissor recorded before it,
	 * and the pipeline's own is not used.
	 */
	int dynamic_viewport;
	int dynamic_scissor;

	/*
	 * Nonzero when the blend constants are dynamic state: a draw then takes
	 * the ones vkCmdSetBlendConstants recorded before it.
	 */
	int dynamic_blend_constants;

	/* Nonzero when the stencil compare mask, write mask and reference are dynamic state (vkCmdSetStencil*). */
	int dynamic_stencil_compare;
	int dynamic_stencil_write;
	int dynamic_stencil_reference;

	/*
	 * The stencil test: whether it is on, then for the front faces [0] and
	 * the back faces [1] the VkStencilOp of a failed test, of a passed one
	 * and of a passed one whose depth test failed, the VkCompareOp and the
	 * compare mask, write mask and reference.
	 */
	uint32_t stencil_test;
	uint32_t stencil_fail[2];
	uint32_t stencil_pass[2];
	uint32_t stencil_depth_fail[2];
	uint32_t stencil_compare[2];
	uint32_t stencil_compare_mask[2];
	uint32_t stencil_write_mask[2];
	uint32_t stencil_reference[2];

	/* The rasterization state. */
	uint32_t cull_mode;
	uint32_t front_face;

	/* Nonzero when the draws run the vertex shader only and rasterize nothing (rasterizerDiscardEnable). */
	uint32_t rasterizer_discard;

	/* The samples the pipeline rasterizes (1, 2 or 4), and which of them it writes. */
	uint32_t samples;
	uint32_t sample_mask;

	/* The depth test state. */
	uint32_t depth_test;
	uint32_t depth_write;
	uint32_t depth_compare;

	/*
	 * The colour blend of attachment 0: whether it is on, the VkBlendFactor
	 * and VkBlendOp of the colour and of the alpha, and the blend constants
	 * as float bits.  All zero is blending off.
	 */
	uint32_t blend_enable;
	uint32_t blend_src_color;
	uint32_t blend_dst_color;
	uint32_t blend_color_op;
	uint32_t blend_src_alpha;
	uint32_t blend_dst_alpha;
	uint32_t blend_alpha_op;
	uint32_t blend_constants[4];

	/*
	 * The logic operation (ws031-p032): nonzero logic_op_enable applies the
	 * VkLogicOp logic_op to every target that takes one (not a float or an
	 * sRGB one), and blends none.
	 */
	uint32_t logic_op_enable;
	uint32_t logic_op;

	/*
	 * Nonzero when the fragment kernel writes a second colour (dual source,
	 * ws031-p032), so that an equation of SRC1 factors blends; the
	 * compiled kernel's, copied when the kernels are prepared.
	 */
	uint32_t dual_source;

	/*
	 * The colour components attachment 0 does NOT write, as the complement
	 * of its VkColorComponentFlags; zero writes every component.
	 */
	uint32_t color_write_disable;

	/*
	 * Colour attachments 1 to 3 (index n - 1): the components each does NOT
	 * write, and nonzero for one that does not blend while attachment 0
	 * does.  Zero, as a pipeline filled by hand leaves them, writes every
	 * component with attachment 0's blend.
	 */
	uint32_t extra_write_disable[I915_GFX_MAX_COLOR_ATTACHMENTS - 1U];
	uint32_t extra_blend_off[I915_GFX_MAX_COLOR_ATTACHMENTS - 1U];

	/* Nonzero once the kernels are prepared. */
	int kernels_ready;

	/*
	 * Where the kernels were last placed: the session record (struct
	 * i915_gfx_session) whose kernel object holds them, the instruction
	 * window and the window generation.  Zero until the first draw; a
	 * record, window or generation that no longer matches places them again.
	 */
	const void *kernel_owner;
	uint32_t kernel_window;
	uint32_t kernel_generation;

	/*
	 * The kernels the executor's compiler made; NULL in a reference-kernel
	 * build, and gs_binary NULL for a pipeline without a geometry stage.
	 */
	struct i915_shader_binary *vs_binary;
	struct i915_shader_binary *gs_binary;
	struct i915_shader_binary *fs_binary;

	/*
	 * Compute (ws101-p003): the compute stage's module and kernel, and the
	 * per-thread push data of one workgroup -- for each of its `threads`
	 * SIMD8 threads, I915_SHADER_PER_THREAD_REGS registers of eight dwords
	 * (the local IDs x, y, z and the linear index of each channel) -- which
	 * every dispatch copies behind the cross-thread data.  `right_mask` is
	 * the channels the last thread runs.  The table is made with the kernel
	 * and freed with it.
	 */
	struct i915_gfx_shader *compute;
	struct i915_shader_binary *cs_binary;
	uint32_t *thread_ids;
	uint32_t threads;
	uint32_t right_mask;
};

/*
 * A linear 2D surface in the session's address space: an image, a buffer
 * region or a scanout buffer that a GPU rectangle reads or writes.
 */
struct i915_gfx_surface {
	/* The GPU address of the first texel. */
	uint64_t va;

	/* The extent, and the bytes to a row. */
	uint32_t width;
	uint32_t height;
	uint32_t pitch;

	/* The VkFormat: R8G8B8A8 or B8G8R8A8 UNORM, or R32_SFLOAT. */
	uint32_t format;

	/*
	 * Nonzero for a colour surface laid out in Y tiles (one sample of a
	 * multisampled image); zero for a linear one.  A depth or stencil
	 * format is Y-tiled whatever this says.
	 */
	uint32_t tiled;
};

/*
 * A rectangle of a surface that a GPU rectangle reads or writes.
 */
struct i915_gfx_rect {
	/* The top-left corner. */
	int32_t x;
	int32_t y;

	/* The width and the height. */
	uint32_t w;
	uint32_t h;
};

/*
 * One recorded operation of a command buffer.
 *
 * The objects it names are the application's; a recorded operation holds no
 * reference to them.
 */
struct i915_gfx_op {
	/* Which member of the union the operation is. */
	enum i915_gfx_op_kind kind;

	union {
		/* A query begin or end (count 1), or a reset of `count` queries from `first`. */
		struct {
			struct i915_gfx_query_pool *pool;
			uint32_t first;
			uint32_t count;
		} query;

		/* A copy between a buffer and an image, in either direction. */
		struct {
			struct i915_gfx_buffer *buffer;
			struct i915_gfx_image *image;
			VkBufferImageCopy region;
		} copy;

		/* A copy between two images. */
		struct {
			struct i915_gfx_image *src;
			struct i915_gfx_image *dst;
			VkImageCopy region;
		} image_copy;

		/* A scaled copy between two images with a filter. */
		struct {
			struct i915_gfx_image *src;
			struct i915_gfx_image *dst;
			VkImageBlit region;
			uint32_t filter;
		} blit;

		/* A clear of a range of an image's levels to four words. */
		struct {
			struct i915_gfx_image *image;
			uint32_t words[4];
			uint32_t base_level;
			uint32_t level_count;
			uint32_t base_layer;
			uint32_t layer_count;
		} clear_image;

		/* The start of a render pass and the clears its attachments load with. */
		struct {
			struct i915_gfx_pass *pass;
			struct i915_gfx_framebuffer *framebuffer;
			uint32_t clear_count;
			uint32_t clear_is_depth[I915_GFX_MAX_ATTACHMENTS];

			/* A colour clear is RGBA float bits; a depth clear is word 0. */
			uint32_t clear_words[I915_GFX_MAX_ATTACHMENTS][4];
		} begin;

		/* The pipeline a bind names. */
		struct i915_gfx_pipeline *pipeline;

		/* A video coding command: its record, owned by the operation (video.c). */
		struct i915_video_command *video;

		/* A vertex buffer bind. */
		struct {
			uint32_t binding;
			struct i915_gfx_buffer *buffer;
			uint64_t offset;
		} vertex;

		/*
		 * A descriptor set bind at a bind point (VkPipelineBindPoint), with
		 * the binding number and the dynamic offset of each dynamic uniform
		 * buffer of the set.
		 */
		struct {
			uint32_t bind_point;
			uint32_t set;
			struct i915_gfx_dset *dset;
			uint32_t dynamic_count;
			uint32_t dynamic_bindings[I915_GFX_MAX_DYNAMIC_BUFFERS];
			uint32_t dynamic_offsets[I915_GFX_MAX_DYNAMIC_BUFFERS];
		} descriptor;

		/* A push constant update. */
		struct {
			uint32_t offset;
			uint32_t size;
			uint8_t bytes[I915_GFX_PUSH_BYTES];
		} push;

		/* A draw. */
		struct {
			uint32_t vertex_count;
			uint32_t instance_count;
			uint32_t first_vertex;
			uint32_t first_instance;
		} draw;

		/* An index buffer bind: the buffer, the offset and the VkIndexType. */
		struct {
			struct i915_gfx_buffer *buffer;
			uint64_t offset;
			uint32_t type;
		} index;

		/* An indexed draw. */
		struct {
			uint32_t index_count;
			uint32_t instance_count;
			uint32_t first_index;
			int32_t vertex_offset;
			uint32_t first_instance;
		} draw_indexed;

		/* A dynamic viewport: x, y, width, height, minDepth and maxDepth as float bits. */
		uint32_t viewport[6];

		/* A dynamic scissor rectangle. */
		VkRect2D scissor;

		/* Dynamic blend constants: R, G, B and A as float bits. */
		uint32_t blend_constants[4];

		/* A dynamic stencil value: which (0 compare mask, 1 write mask, 2 reference), the faces (VkStencilFaceFlags) and the value. */
		struct {
			uint32_t which;
			uint32_t faces;
			uint32_t value;
		} stencil;

		/* A dispatch of groups x by y by z workgroups (ws101-p003). */
		struct {
			uint32_t groups[3];
		} dispatch;

		/* A dispatch whose three group counts are in a buffer at an offset (vkCmdDispatchIndirect, ws101-p007). */
		struct {
			struct i915_gfx_buffer *buffer;
			uint64_t offset;
		} dispatch_indirect;

		/* A copy of one region between two buffers. */
		struct {
			struct i915_gfx_buffer *src;
			struct i915_gfx_buffer *dst;
			VkBufferCopy region;
		} buffer_copy;

		/* A clear of one rectangle of the colour or the depth attachment of the pass in progress. */
		struct {
			uint32_t is_depth;

			/* The subpass's colour attachment a colour clear names (VkClearAttachment.colorAttachment). */
			uint32_t color_index;

			/* The aspects a depth / stencil clear clears (VkImageAspectFlags). */
			uint32_t aspects;

			/* A colour clear is RGBA float bits; a depth / stencil clear is the depth's bits and the stencil. */
			uint32_t words[4];
			struct i915_gfx_rect rect;

			/* The layers of the attachment's view the rectangle clears (VkClearRect), from the view's first. */
			uint32_t base_layer;
			uint32_t layer_count;
		} clear_attachment;
	} u;
};

/*
 * What is bound when a draw is reached.
 *
 * It is built while a command buffer is executed and lives only for that
 * execution.
 */
struct i915_gfx_draw_state {
	/* The render pass and the framebuffer in progress. */
	struct i915_gfx_pass *pass;
	struct i915_gfx_framebuffer *framebuffer;

	/* The bound graphics pipeline. */
	struct i915_gfx_pipeline *pipeline;

	/*
	 * What is bound at the compute bind point (ws101-p003): the pipeline,
	 * the descriptor sets and their dynamic offsets, apart from the
	 * graphics ones as Vulkan keeps them.  The push constants are shared.
	 */
	struct i915_gfx_pipeline *compute_pipeline;
	struct i915_gfx_dset *compute_dset[I915_GFX_BOUND_SETS];
	uint32_t compute_dynamic_count[I915_GFX_BOUND_SETS];
	uint32_t compute_dynamic_bindings[I915_GFX_BOUND_SETS][I915_GFX_MAX_DYNAMIC_BUFFERS];
	uint32_t compute_dynamic_offsets[I915_GFX_BOUND_SETS][I915_GFX_MAX_DYNAMIC_BUFFERS];

	/* The bound vertex buffers and their offsets. */
	struct {
		struct i915_gfx_buffer *buffer;
		uint64_t offset;
	} vertex[I915_GFX_MAX_VERTEX_BINDINGS];

	/* The bound descriptor sets. */
	struct i915_gfx_dset *dset[I915_GFX_BOUND_SETS];

	/* The dynamic offset of each dynamic uniform buffer of each bound set, by binding number. */
	uint32_t dynamic_count[I915_GFX_BOUND_SETS];
	uint32_t dynamic_bindings[I915_GFX_BOUND_SETS][I915_GFX_MAX_DYNAMIC_BUFFERS];
	uint32_t dynamic_offsets[I915_GFX_BOUND_SETS][I915_GFX_MAX_DYNAMIC_BUFFERS];

	/* The push constants. */
	uint8_t push[I915_GFX_PUSH_BYTES];

	/* The bound index buffer, its offset and its VkIndexType. */
	struct {
		struct i915_gfx_buffer *buffer;
		uint64_t offset;
		uint32_t type;
	} index;

	/* The viewport vkCmdSetViewport set, as float bits; valid once viewport_set is nonzero. */
	uint32_t viewport[6];
	int viewport_set;

	/* The scissor vkCmdSetScissor set; valid once scissor_set is nonzero. */
	VkRect2D scissor;
	int scissor_set;

	/* The blend constants vkCmdSetBlendConstants set, as float bits; valid once blend_constants_set is nonzero. */
	uint32_t blend_constants[4];
	int blend_constants_set;

	/* The stencil masks and references vkCmdSetStencil* set, front [0] and back [1]. */
	uint32_t stencil_compare_mask[2];
	uint32_t stencil_write_mask[2];
	uint32_t stencil_reference[2];
};

/*
 * What one draw asks for: the counts and the first elements of a vkCmdDraw
 * or a vkCmdDrawIndexed.
 *
 * It lives on the stack of the command buffer execution for one draw.
 */
struct i915_gfx_draw_args {
	/* Nonzero for an indexed draw, which reads its vertices through the bound index buffer. */
	int indexed;

	/* The vertices of a draw, or the indices of an indexed draw. */
	uint32_t count;

	/* The instances. */
	uint32_t instance_count;

	/* The first vertex, or the first index. */
	uint32_t first;

	/* What an indexed draw adds to every index; zero for a draw. */
	int32_t vertex_offset;

	/* The first instance. */
	uint32_t first_instance;
};

/*
 * Routes an object command (objects.c) or a recording command (command.c).
 * `handled` is cleared for an opcode the module does not own.
 */
int drv_i915_gfx_obj_dispatch(struct i915_render_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int drv_i915_gfx_rec_dispatch(struct i915_render_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);

/*
 * The CPU view of `bytes` bytes of a memory range, or NULL when the memory
 * has no storage yet; and the GPU address of a memory range in the session's
 * address space, or 0 when it has no storage (memory.c).
 */
uint8_t *drv_i915_gfx_memory_cpu(struct i915_gfx_memory *memory, uint64_t offset, uint64_t bytes);
void drv_i915_gfx_memory_release(struct i915_gfx_memory *memory);
uint64_t drv_i915_gfx_memory_va(struct i915_gfx_memory *memory, uint64_t offset);

/*
 * Lays an image out from its format, extent and levels, describes one of
 * its levels as a surface, and reports a format's bytes to a texel (image.c).
 */
int drv_i915_gfx_image_layout(struct i915_gfx_image *image);
int drv_i915_gfx_image_level(const struct i915_gfx_image *image, uint32_t level, struct i915_gfx_surface *surface);
int drv_i915_gfx_image_slice(const struct i915_gfx_image *image, uint32_t level, uint32_t slice, struct i915_gfx_surface *surface);
uint32_t drv_i915_gfx_format_bytes(uint32_t format);
uint32_t drv_i915_gfx_view_format(const struct i915_gfx_view *view);

/* The attachment a render pass's colour slot draws into, VK_ATTACHMENT_UNUSED for none (render-pass.c). */
uint32_t drv_i915_gfx_pass_color(const struct i915_gfx_pass *pass, uint32_t slot);

/* The view whose level a draw covers: the first colour attachment's, or the depth's (draw.c). */
struct i915_gfx_draw_state;
const struct i915_gfx_view *drv_i915_gfx_draw_extent_view(const struct i915_gfx_draw_state *state);
uint32_t drv_i915_gfx_image_slices(const struct i915_gfx_image *image, uint32_t level);
uint32_t drv_i915_gfx_depth_clear_word(uint32_t format, uint32_t depth);

/* Releases what the session's draws kept: the state, batch and kernel objects (draw.c). */
void drv_i915_gfx_session_close(struct i915_render_session *session);

/* Frees every object a closing session did not destroy (objects.c). */
void drv_i915_gfx_objects_release(struct i915_render_session *session);

/* Prepares and releases a pipeline's kernels, a graphics or a compute pipeline's (pipeline-prepare.c). */
int drv_i915_gfx_pipeline_prepare(struct i915_render_session *session, struct i915_gfx_pipeline *pipeline);
int drv_i915_gfx_compute_prepare(struct i915_render_session *session, struct i915_gfx_pipeline *pipeline);
void drv_i915_gfx_pipeline_release(struct i915_gfx_pipeline *pipeline);

/*
 * Records one draw or indexed draw into the submission's batch, or runs it
 * to its end outside a submission (draw.c).
 */
int drv_i915_gfx_draw(struct i915_render_session *session, const struct i915_gfx_draw_state *state, const struct i915_gfx_draw_args *args);

/*
 * Opens the batch of a submission, and runs what it recorded and closes it
 * (draw.c).
 */
int drv_i915_gfx_submit_begin(struct i915_render_session *session);
int drv_i915_gfx_submit_end(struct i915_render_session *session);

/*
 * One rectangle on the GPU (blit.c): a copy of src_rect of `src` into
 * dst_rect of `dst`, scaled when the sizes differ with `linear` selecting
 * the filter (I915_GFX_RECT_LINEAR, nonzero for the linear one) and
 * mirroring the source along x or y (I915_GFX_RECT_MIRROR_X, _Y), or, with
 * src NULL, a fill of dst_rect with the four float words of `clear`.  drv_i915_gfx_rect records it into the submission's
 * batch, or runs it to its end outside a submission;
 * drv_i915_gfx_rect_build only writes the session's state and batch objects
 * and returns the batch address, for a caller that runs the batch itself
 * (the display, on the serving thread).
 */
#define I915_GFX_RECT_LINEAR		1
#define I915_GFX_RECT_MIRROR_X		2
#define I915_GFX_RECT_MIRROR_Y		4

int drv_i915_gfx_rect_prepare(struct i915_render_session *session);
int drv_i915_gfx_rect(struct i915_render_session *session, const struct i915_gfx_surface *dst, const struct i915_gfx_rect *dst_rect, const struct i915_gfx_surface *src, const struct i915_gfx_rect *src_rect, const uint32_t clear[4], int linear);
int drv_i915_gfx_rect_build(struct i915_render_session *session, const struct i915_gfx_surface *dst, const struct i915_gfx_rect *dst_rect, const struct i915_gfx_surface *src, const struct i915_gfx_rect *src_rect, const uint32_t clear[4], int linear, uint64_t *batch_va);
int drv_i915_gfx_resolve(struct i915_render_session *session, const struct i915_gfx_surface *dst, const struct i915_gfx_rect *dst_rect, const struct i915_gfx_surface *samples, const struct i915_gfx_rect *src_rect);

#endif
