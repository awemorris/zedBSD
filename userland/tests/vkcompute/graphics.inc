/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The graphics checks of vkcompute (-g), included by main.c.
 *
 * Each check draws into a 64 x 64 R32_UINT colour target, with a D32_SFLOAT
 * depth target beside it, both cleared (colour 0, depth 1.0), copies the
 * colour target into host memory and compares every pixel, and the words of
 * its storage buffer when it has one, with what C computes.  The shaders
 * are in graphics/ and reach the driver's shader compiler the way OpenGL's
 * do through a Vulkan driver:
 *
 *   g-ubo-frag    twelve uniform blocks in a fragment shader, one of them
 *                 64 KiB (maxUniformBufferRange), bound at an offset
 *   g-ubo-vert    a 4 KiB uniform block and a push constant in a vertex
 *                 shader, passed on as a flat varying
 *   g-gather      textureGather of each channel, with and without a
 *                 constant offset
 *   g-fragdepth   gl_FragDepth against the depth test of a second draw
 *   g-samplemask  gl_SampleMask and gl_SampleMaskIn
 *   g-vatomic     a vertex shader's buffer atomics
 *   g-vstore      a vertex shader's plain buffer loads and stores, a record
 *                 a vertex at its gl_VertexIndex
 *   g-vtex        a vertex shader samples the image
 *   g-gtex        a geometry shader samples the image
 *   g-depthbias   a depth bias of the pipeline and a dynamic one move a
 *                 draw's depth past the one before it
 *   g-depthclamp  a primitive past the far plane is clamped to it, not
 *                 clipped
 *   g-polyline, g-polypoint  a triangle outlined, and drawn as its vertices
 *   g-mrt-blend   two targets, each blended by its own equation
 *   g-dualsrc     the second colour of a fragment shader as the blend factor
 *   g-cleards     vkCmdClearDepthStencilImage's depth and stencil, which the
 *                 draws then test
 *   g-indirect    vkCmdDrawIndirect and vkCmdDrawIndexedIndirect of records
 *                 vkCmdUpdateBuffer wrote
 *   g-fill        vkCmdFillBuffer and vkCmdUpdateBuffer into a buffer
 *   g-timestamp   two timestamps around a draw, copied with
 *                 vkCmdCopyQueryPoolResults
 *   g-occlusion   an occlusion query of a draw over the whole target
 *                 counts its pixels, precisely when the device can
 *   g-occlusion-mrt  the same with two colour targets: still one count a
 *                 pixel, not one a target
 *   g-fatomic     a fragment shader's buffer atomics
 *   g-fdepthstore a fragment shader's atomics under the depth test that
 *                 comes after it: three draws (middle, front, behind)
 *                 each count every pixel, the hidden ones too
 *   g-fearlystore the same with early fragment tests: the draw behind
 *                 the front one never runs
 *   g-desccopy    descriptors of an array, one write running on into the next
 *                 binding, and a descriptor copy
 *   g-aniso       an anisotropic sampler reads texel centres exactly
 *   g-cubearray   a cube array image's faces
 *   g-formats     the 16-bit, signed normalized, packed and integer
 *                 formats' clears and copies
 *   g-blit3d      a 3D image of four slices blitted onto one of two
 *   g-sarray      an array of sampled images, each element a view of its own
 *   g-uarray      an array of uniform blocks
 *   g-querylod    textureQueryLod
 *   g-ginvoc      a geometry shader of four invocations, each covering a
 *                 quadrant of the target by its gl_InvocationID
 *   g-gpsize      a geometry shader's points of their own gl_PointSize, one
 *                 of them half what the vertex shader wrote
 *
 * Each check prints one line, PASS or FAIL with the first wrong word, or
 * UNSUPPORTED when the driver refused its pipeline; then the run prints
 * its verdict.  With -n one expected word of each check is made wrong on
 * purpose (as in the oracle), so every check must FAIL.
 *
 * The file is part of main.c's translation unit: it uses that file's
 * device, its memory type choice and its comparison of words.
 */

#include "graphics-shaders.h"

/* The colour target's side in pixels, and its pixels. */
#define GFX_SIDE		64U
#define GFX_PIXELS		(GFX_SIDE * GFX_SIDE)

/* The most descriptor bindings and draws of one check. */
#define GFX_MAX_BINDINGS	12U
#define GFX_MAX_DRAWS		3U

/* The word of the readback buffers (words[]) the second colour target's pixels are in. */
#define GFX_SECOND_TARGET	(GFX_MAX_BINDINGS + 1U)

/* The images and the buffer words of g-formats. */
#define GFX_FORMATS		8U
#define GFX_FORMAT_WORDS	(GFX_FORMATS * 16U)

/* The most vertices of one draw. */
#define GFX_MAX_VERTICES	300U

/* The side of the sampled image, in texels. */
#define GFX_TEXTURE_SIDE	4U

/* What a binding of a check's descriptor set is: a buffer, the 4 x 4 image, or the cube array image. */
enum gfx_binding_kind {
	GFX_UNIFORM,
	GFX_STORAGE,
	GFX_TEXTURE,
	GFX_CUBES
};

/*
 * One binding of a check's set 0: a buffer of `bytes` bound at byte
 * `offset` of a buffer that long more, its words filled before the draw;
 * or the 4 x 4 R8G8B8A8_UNORM image with a nearest sampler.
 */
struct gfx_binding {
	uint32_t binding;
	enum gfx_binding_kind kind;
	uint32_t bytes;
	uint32_t offset;
	VkShaderStageFlags stages;

	/* The array elements of the binding (0 reads as one); only the first is written by the generic code. */
	uint32_t count;
};

/*
 * One draw of a check: its two shaders, whether it tests and writes depth
 * (LESS), and its vertices -- three of a triangle covering the target at
 * depth z, or `vertices` points of one place whose y is their number.
 */
struct gfx_draw {
	const uint32_t *vertex;
	size_t vertex_size;
	const uint32_t *fragment;
	size_t fragment_size;
	int depth_test;
	float z;
	uint32_t vertices;

	/* A geometry shader between them (NULL for none). */
	const uint32_t *geometry;
	size_t geometry_size;

	/* The colour push.frag writes, pushed before the draw (0: nothing pushed). */
	uint32_t color;

	/* Nonzero for a smaller triangle, (16, 16), (48, 16), (16, 48) in pixels, instead of one over the target. */
	int small;
};

struct gfx_test;
struct gfx_objects;

/*
 * What a check needs beyond the generic draw: the pipeline's state, the
 * targets' formats, and the commands it records around the draws.  Every
 * field zero is the generic check.
 */
struct gfx_options {
	/* The colour targets' format (0: R32_UINT) and how many there are (0: one; at most two), and their blend. */
	VkFormat color_format;
	uint32_t color_count;
	int blend_given;
	VkPipelineColorBlendAttachmentState blend[2];

	/* The colours a logic operation OR, instead of a draw's replacing the one before (an integer target). */
	int logic_or;

	/*
	 * The depth target's format (0: D32_SFLOAT); nonzero depth_load keeps
	 * what vkCmdClearDepthStencilImage cleared it to before the pass
	 * (clear_depth, clear_stencil) instead of clearing it at the begin;
	 * stencil_draw is the draw (from 1) whose stencil test is EQUAL to
	 * clear_stencil.
	 */
	VkFormat depth_format;
	int depth_load;
	float clear_depth;
	uint32_t clear_stencil;
	uint32_t stencil_draw;

	/* The polygon mode, the depth clamp, and LESS_OR_EQUAL instead of LESS. */
	VkPolygonMode polygon_mode;
	int depth_clamp;
	int depth_lequal;

	/*
	 * Each draw's depth bias: its constant factor (0: off) and clamp; and
	 * the draw (from 1) whose bias is dynamic, set to dynamic_bias.
	 */
	float bias[GFX_MAX_DRAWS];
	float bias_clamp[GFX_MAX_DRAWS];
	uint32_t dynamic_draw;
	float dynamic_bias;

	/* Nonzero: the draws are vkCmdDrawIndirect and vkCmdDrawIndexedIndirect of records the commands write. */
	int indirect;

	/* Nonzero: the image's sampler is anisotropic (16) and linear. */
	int anisotropy;

	/* What the check makes after the generic resources and layout, and records before the pass, before each draw and after the pass. */
	VkResult (*prepare)(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
	void (*before_pass)(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
	void (*before_draw)(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects, uint32_t draw);
	void (*after_pass)(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
};

/*
 * One graphics check: its bindings, its draws, the push constant word the
 * vertex stage reads (when push is nonzero) and the C function that
 * compares the target's pixels (buffer 0 of the comparison) and the words
 * of each binding's buffer (buffer 1 + n) with the reference.
 */
struct gfx_test {
	const char *name;
	uint32_t binding_count;
	struct gfx_binding bindings[GFX_MAX_BINDINGS];
	uint32_t draw_count;
	struct gfx_draw draws[GFX_MAX_DRAWS];
	int push;
	uint32_t push_word;
	void (*check)(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
	const struct gfx_options *options;
};

/*
 * The objects of one check's run, destroyed by gfx_destroy() however far
 * the run got.  The host buffers stay mapped from creation to destruction.
 */
struct gfx_objects {
	VkBuffer buffers[GFX_MAX_BINDINGS];
	VkDeviceMemory memories[GFX_MAX_BINDINGS];
	uint32_t *words[GFX_MAX_BINDINGS + 2U];

	/* The second colour target, its copy, and what a check makes for itself (each destroyed when made). */
	VkImage color2;
	VkDeviceMemory color2_memory;
	VkImageView color2_view;
	VkBuffer readback2;
	VkDeviceMemory readback2_memory;
	VkBuffer extra_buffers[6];
	VkDeviceMemory extra_memories[6];
	uint32_t *extra_words[6];
	VkImage extra_images[GFX_FORMATS];
	VkDeviceMemory extra_image_memories[GFX_FORMATS];
	VkImageView cube_view;
	VkImageView extra_views[2];
	VkQueryPool queries;
	VkImage texture;
	VkDeviceMemory texture_memory;
	VkImageView texture_view;
	VkSampler sampler;
	VkImage color;
	VkDeviceMemory color_memory;
	VkImageView color_view;
	VkImage depth;
	VkDeviceMemory depth_memory;
	VkImageView depth_view;
	VkBuffer readback;
	VkDeviceMemory readback_memory;
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkRenderPass render_pass;
	VkFramebuffer framebuffer;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout pipeline_layout;
	VkShaderModule modules[2U * GFX_MAX_DRAWS];
	VkShaderModule geometry_modules[GFX_MAX_DRAWS];
	VkPipeline pipelines[GFX_MAX_DRAWS];
	VkDescriptorPool pool;
	VkDescriptorSet set;
	VkCommandPool command_pool;
	VkCommandBuffer command;
	VkFence fence;
	const char *failed_call;
};

static void gfx_check_ubo_frag(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_check_ubo_vert(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_check_gather(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_check_fragdepth(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_check_samplemask(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_check_vatomic(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_check_vstore(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_check_vtex(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_check_gtex(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_check_constant(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_check_outline(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_check_dots(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_check_mrt(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_check_fill(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_check_timestamp(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_check_fatomic(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_check_fdepthstore(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_check_desccopy(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_check_aniso(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_check_cubearray(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_check_formats(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_clear_depth_image(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
static VkResult gfx_prepare_indirect(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
static void gfx_write_records(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
static void gfx_write_fill(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
static VkResult gfx_prepare_timestamp(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
static void gfx_first_timestamp(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
static void gfx_last_timestamp(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
static VkResult gfx_prepare_occlusion(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
static void gfx_begin_occlusion(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
static void gfx_end_occlusion(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
static void gfx_check_occlusion(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static VkResult gfx_prepare_desccopy(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
static void gfx_upload_cubes(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
static VkResult gfx_prepare_formats(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
static void gfx_record_formats(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
static VkResult gfx_prepare_sarray(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
static VkResult gfx_prepare_uarray(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
static void gfx_check_sarray(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_check_uarray(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_check_querylod(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_check_ginvoc(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_check_gpsize(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static uint32_t gfx_swizzled(uint32_t word, uint32_t element);
static VkResult gfx_prepare_blit3d(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
static void gfx_record_blit3d(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
static void gfx_check_blit3d(struct oracle_check *check, const struct gfx_test *test, uint32_t *const *words);
static void gfx_image_barrier(VkCommandBuffer command, VkImage image, VkImageAspectFlags aspects, uint32_t layers, VkImageLayout from, VkImageLayout to);
static void gfx_memory_barrier(VkCommandBuffer command);
static uint32_t gfx_cube_word(uint32_t layer);
static uint32_t gfx_texel_word(uint32_t x, uint32_t y);
static uint32_t gfx_count_color(const uint32_t *pixels, uint32_t color, int *other);

/* The shaders and their sizes, as a draw names them. */
#define GFX_SHADER(name)	vkcompute_graphics_##name, sizeof(vkcompute_graphics_##name)

/* The vertices of the points g-vatomic draws: one hundred triangles. */
#define GFX_VATOMIC_VERTICES	300U

/* The colours of push.frag the checks draw with, by the check. */
#define GFX_DEPTHBIAS_COLOR	7U
#define GFX_DEPTHCLAMP_COLOR	5U
#define GFX_OUTLINE_COLOR	9U

/* The stencil value vkCmdClearDepthStencilImage clears g-cleards' stencil to. */
#define GFX_CLEARDS_STENCIL	0x37U

/* The writes of all four channels, and a blend of one attachment. */
#define GFX_RGBA	(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT)

/* Whether the occlusion query of the check being run counts precisely (set when it begins). */
static int gfx_occlusion_precise;

/* The colour mrt.frag and dualsrc.frag blend to, as R8G8B8A8_UNORM bytes: (0.2, 0.4, 0.6, 1.0). */
#define GFX_BLENDED_WORD	0xff996633U

/*
 * The checks' own state.  The tables are constant for the life of the
 * program.
 */
static const struct gfx_options gfx_depthbias_options = {
	.logic_or = 1,
	.bias = { 0.0f, -4194304.0f, 0.0f },
	.bias_clamp = { 0.0f, -0.0625f, 0.0f },
	.dynamic_draw = 3U,
	.dynamic_bias = -4194304.0f
};
static const struct gfx_options gfx_depthclamp_options = {
	.depth_clamp = 1,
	.depth_lequal = 1
};
static const struct gfx_options gfx_polyline_options = {
	.polygon_mode = VK_POLYGON_MODE_LINE
};
static const struct gfx_options gfx_polypoint_options = {
	.polygon_mode = VK_POLYGON_MODE_POINT
};
static const struct gfx_options gfx_mrt_options = {
	.color_format = VK_FORMAT_R8G8B8A8_UNORM,
	.color_count = 2U,
	.blend_given = 1,
	.blend = {
		{ VK_TRUE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
		  VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, GFX_RGBA },
		{ VK_TRUE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD,
		  VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, GFX_RGBA }
	}
};
static const struct gfx_options gfx_dualsrc_options = {
	.color_format = VK_FORMAT_R8G8B8A8_UNORM,
	.blend_given = 1,
	.blend = {
		{ VK_TRUE, VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
		  VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, GFX_RGBA }
	}
};
/* The vertex stage's stores and atomics draw nothing: an 8-bit target, which every device draws into. */
static const struct gfx_options gfx_vertex_store_options = {
	.color_format = VK_FORMAT_R8G8B8A8_UNORM
};
static const struct gfx_options gfx_cleards_options = {
	.logic_or = 1,
	.depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT,
	.depth_load = 1,
	.clear_depth = 0.25f,
	.clear_stencil = GFX_CLEARDS_STENCIL,
	.stencil_draw = 2U,
	.before_pass = gfx_clear_depth_image
};
static const struct gfx_options gfx_indirect_options = {
	.logic_or = 1,
	.indirect = 1,
	.prepare = gfx_prepare_indirect,
	.before_pass = gfx_write_records
};
static const struct gfx_options gfx_fill_options = {
	.before_pass = gfx_write_fill
};
static const struct gfx_options gfx_timestamp_options = {
	.prepare = gfx_prepare_timestamp,
	.before_pass = gfx_first_timestamp,
	.after_pass = gfx_last_timestamp
};
static const struct gfx_options gfx_occlusion_options = {
	.prepare = gfx_prepare_occlusion,
	.before_pass = gfx_begin_occlusion,
	.after_pass = gfx_end_occlusion
};
static const struct gfx_options gfx_occlusion_mrt_options = {
	.color_format = VK_FORMAT_R8G8B8A8_UNORM,
	.color_count = 2U,
	.blend_given = 1,
	.blend = {
		{ VK_TRUE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
		  VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, GFX_RGBA },
		{ VK_TRUE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD,
		  VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, GFX_RGBA }
	},
	.prepare = gfx_prepare_occlusion,
	.before_pass = gfx_begin_occlusion,
	.after_pass = gfx_end_occlusion
};
static const struct gfx_options gfx_desccopy_options = {
	.prepare = gfx_prepare_desccopy
};
static const struct gfx_options gfx_aniso_options = {
	.anisotropy = 1
};
static const struct gfx_options gfx_cubearray_options = {
	.before_pass = gfx_upload_cubes
};
static const struct gfx_options gfx_formats_options = {
	.prepare = gfx_prepare_formats,
	.before_pass = gfx_record_formats
};
static const struct gfx_options gfx_sarray_options = {
	.prepare = gfx_prepare_sarray
};
static const struct gfx_options gfx_uarray_options = {
	.prepare = gfx_prepare_uarray
};
static const struct gfx_options gfx_blit3d_options = {
	.prepare = gfx_prepare_blit3d,
	.before_pass = gfx_record_blit3d
};

/*
 * The graphics checks, in the order they run.  The table is constant for
 * the life of the program.
 */
static const struct gfx_test gfx_tests[] = {
	{
		"g-ubo-frag",
		12U, {
			{ 0U, GFX_UNIFORM, 16U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U },
			{ 1U, GFX_UNIFORM, 16U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U },
			{ 2U, GFX_UNIFORM, 16U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U },
			{ 3U, GFX_UNIFORM, 16U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U },
			{ 4U, GFX_UNIFORM, 16U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U },
			{ 5U, GFX_UNIFORM, 16U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U },
			{ 6U, GFX_UNIFORM, 16U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U },
			{ 7U, GFX_UNIFORM, 16U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U },
			{ 8U, GFX_UNIFORM, 16U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U },
			{ 9U, GFX_UNIFORM, 16U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U },
			{ 10U, GFX_UNIFORM, 16U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U },
			{ 11U, GFX_UNIFORM, 65536U, 256U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U }
		},
		1U, { { GFX_SHADER(position_vert), GFX_SHADER(ubo_frag_frag), 0, 0.0f, 3U, NULL, 0U, 0U, 0 } },
		0, 0U, gfx_check_ubo_frag, NULL
	},
	{
		"g-ubo-vert",
		1U, { { 0U, GFX_UNIFORM, 4096U, 0U, VK_SHADER_STAGE_VERTEX_BIT, 0U } },
		1U, { { GFX_SHADER(ubo_vert_vert), GFX_SHADER(flat_frag), 0, 0.0f, 3U, NULL, 0U, 0U, 0 } },
		1, 0x5a5a1234U, gfx_check_ubo_vert, NULL
	},
	{
		"g-gather",
		1U, { { 0U, GFX_TEXTURE, 0U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U } },
		1U, { { GFX_SHADER(position_vert), GFX_SHADER(gather_frag), 0, 0.0f, 3U, NULL, 0U, 0U, 0 } },
		0, 0U, gfx_check_gather, NULL
	},
	{
		"g-fragdepth",
		0U, { { 0U, GFX_UNIFORM, 0U, 0U, 0U, 0U } },
		2U, {
			{ GFX_SHADER(position_vert), GFX_SHADER(depth_frag), 1, 0.0f, 3U, NULL, 0U, 0U, 0 },
			{ GFX_SHADER(position_vert), GFX_SHADER(two_frag), 1, 0.5f, 3U, NULL, 0U, 0U, 0 }
		},
		0, 0U, gfx_check_fragdepth, NULL
	},
	{
		"g-samplemask",
		0U, { { 0U, GFX_UNIFORM, 0U, 0U, 0U, 0U } },
		1U, { { GFX_SHADER(position_vert), GFX_SHADER(samplemask_frag), 0, 0.0f, 3U, NULL, 0U, 0U, 0 } },
		0, 0U, gfx_check_samplemask, NULL
	},
	{
		"g-vatomic",
		1U, { { 0U, GFX_STORAGE, 64U, 0U, VK_SHADER_STAGE_VERTEX_BIT, 0U } },
		1U, { { GFX_SHADER(vatomic_vert), GFX_SHADER(none_frag), 0, 0.0f, GFX_VATOMIC_VERTICES, NULL, 0U, 0U, 0 } },
		0, 0U, gfx_check_vatomic, &gfx_vertex_store_options
	},
	{
		"g-vstore",
		1U, { { 0U, GFX_STORAGE, 16U + 16U * GFX_VATOMIC_VERTICES, 0U, VK_SHADER_STAGE_VERTEX_BIT, 0U } },
		1U, { { GFX_SHADER(vstore_vert), GFX_SHADER(none_frag), 0, 0.0f, GFX_VATOMIC_VERTICES, NULL, 0U, 0U, 0 } },
		0, 0U, gfx_check_vstore, &gfx_vertex_store_options
	},
	{
		"g-vtex",
		1U, { { 0U, GFX_TEXTURE, 0U, 0U, VK_SHADER_STAGE_VERTEX_BIT, 0U } },
		1U, { { GFX_SHADER(vtex_vert), GFX_SHADER(flat_frag), 0, 0.0f, 3U, NULL, 0U, 0U, 0 } },
		0, 0U, gfx_check_vtex, NULL
	},
	{
		"g-gtex",
		1U, { { 0U, GFX_TEXTURE, 0U, 0U, VK_SHADER_STAGE_GEOMETRY_BIT, 0U } },
		1U, { { GFX_SHADER(position_vert), GFX_SHADER(flat_frag), 0, 0.0f, 3U, GFX_SHADER(gtex_geom), 0U, 0 } },
		0, 0U, gfx_check_gtex, NULL
	},
	{
		"g-depthbias",
		0U, { { 0U, GFX_UNIFORM, 0U, 0U, 0U, 0U } },
		3U, {
			{ GFX_SHADER(position_vert), GFX_SHADER(push_frag), 1, 0.5f, 3U, NULL, 0U, 1U, 0 },
			{ GFX_SHADER(position_vert), GFX_SHADER(push_frag), 1, 0.5f, 3U, NULL, 0U, 2U, 0 },
			{ GFX_SHADER(position_vert), GFX_SHADER(push_frag), 1, 0.5f, 3U, NULL, 0U, 4U, 0 }
		},
		0, GFX_DEPTHBIAS_COLOR, gfx_check_constant, &gfx_depthbias_options
	},
	{
		"g-depthclamp",
		0U, { { 0U, GFX_UNIFORM, 0U, 0U, 0U, 0U } },
		1U, { { GFX_SHADER(position_vert), GFX_SHADER(push_frag), 1, 1.5f, 3U, NULL, 0U, GFX_DEPTHCLAMP_COLOR, 0 } },
		0, GFX_DEPTHCLAMP_COLOR, gfx_check_constant, &gfx_depthclamp_options
	},
	{
		"g-polyline",
		0U, { { 0U, GFX_UNIFORM, 0U, 0U, 0U, 0U } },
		1U, { { GFX_SHADER(position_vert), GFX_SHADER(push_frag), 0, 0.0f, 3U, NULL, 0U, GFX_OUTLINE_COLOR, 1 } },
		0, 0U, gfx_check_outline, &gfx_polyline_options
	},
	{
		"g-polypoint",
		0U, { { 0U, GFX_UNIFORM, 0U, 0U, 0U, 0U } },
		1U, { { GFX_SHADER(position_vert), GFX_SHADER(push_frag), 0, 0.0f, 3U, NULL, 0U, GFX_OUTLINE_COLOR, 1 } },
		0, 0U, gfx_check_dots, &gfx_polypoint_options
	},
	{
		"g-mrt-blend",
		0U, { { 0U, GFX_UNIFORM, 0U, 0U, 0U, 0U } },
		1U, { { GFX_SHADER(position_vert), GFX_SHADER(mrt_frag), 0, 0.0f, 3U, NULL, 0U, 0U, 0 } },
		0, 2U, gfx_check_mrt, &gfx_mrt_options
	},
	{
		"g-dualsrc",
		0U, { { 0U, GFX_UNIFORM, 0U, 0U, 0U, 0U } },
		1U, { { GFX_SHADER(position_vert), GFX_SHADER(dualsrc_frag), 0, 0.0f, 3U, NULL, 0U, 0U, 0 } },
		0, 1U, gfx_check_mrt, &gfx_dualsrc_options
	},
	{
		"g-cleards",
		0U, { { 0U, GFX_UNIFORM, 0U, 0U, 0U, 0U } },
		2U, {
			{ GFX_SHADER(position_vert), GFX_SHADER(push_frag), 1, 0.5f, 3U, NULL, 0U, 1U, 0 },
			{ GFX_SHADER(position_vert), GFX_SHADER(push_frag), 1, 0.125f, 3U, NULL, 0U, 2U, 0 }
		},
		0, 2U, gfx_check_constant, &gfx_cleards_options
	},
	{
		"g-indirect",
		0U, { { 0U, GFX_UNIFORM, 0U, 0U, 0U, 0U } },
		2U, {
			{ GFX_SHADER(position_vert), GFX_SHADER(push_frag), 0, 0.0f, 3U, NULL, 0U, 1U, 0 },
			{ GFX_SHADER(position_vert), GFX_SHADER(push_frag), 0, 0.0f, 3U, NULL, 0U, 2U, 0 }
		},
		0, 3U, gfx_check_constant, &gfx_indirect_options
	},
	{
		"g-fill",
		1U, { { 0U, GFX_STORAGE, 128U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U } },
		0U, { { NULL, 0U, NULL, 0U, 0, 0.0f, 0U, NULL, 0U, 0U, 0 } },
		0, 0U, gfx_check_fill, &gfx_fill_options
	},
	{
		"g-timestamp",
		1U, { { 0U, GFX_STORAGE, 64U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U } },
		1U, { { GFX_SHADER(position_vert), GFX_SHADER(two_frag), 0, 0.0f, 3U, NULL, 0U, 0U, 0 } },
		0, 2U, gfx_check_timestamp, &gfx_timestamp_options
	},
	{
		"g-occlusion",
		1U, { { 0U, GFX_STORAGE, 64U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U } },
		1U, { { GFX_SHADER(position_vert), GFX_SHADER(two_frag), 0, 0.0f, 3U, NULL, 0U, 0U, 0 } },
		0, 1U, gfx_check_occlusion, &gfx_occlusion_options
	},
	{
		"g-occlusion-mrt",
		1U, { { 0U, GFX_STORAGE, 64U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U } },
		1U, { { GFX_SHADER(position_vert), GFX_SHADER(mrt_frag), 0, 0.0f, 3U, NULL, 0U, 0U, 0 } },
		0, 2U, gfx_check_occlusion, &gfx_occlusion_mrt_options
	},
	{
		"g-fatomic",
		1U, { { 0U, GFX_STORAGE, 16U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U } },
		1U, { { GFX_SHADER(position_vert), GFX_SHADER(fatomic_frag), 0, 0.0f, 3U, NULL, 0U, 0U, 0 } },
		0, 0U, gfx_check_fatomic, NULL
	},
	{
		"g-fdepthstore",
		1U, { { 0U, GFX_STORAGE, 16U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U } },
		3U, {
			{ GFX_SHADER(position_vert), GFX_SHADER(fdepthstore_frag), 1, 0.5f, 3U, NULL, 0U, 1U, 0 },
			{ GFX_SHADER(position_vert), GFX_SHADER(fdepthstore_frag), 1, 0.25f, 3U, NULL, 0U, 2U, 0 },
			{ GFX_SHADER(position_vert), GFX_SHADER(fdepthstore_frag), 1, 0.75f, 3U, NULL, 0U, 3U, 0 }
		},
		0, 0U, gfx_check_fdepthstore, NULL
	},
	{
		"g-fearlystore",
		1U, { { 0U, GFX_STORAGE, 16U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U } },
		2U, {
			{ GFX_SHADER(position_vert), GFX_SHADER(fearlystore_frag), 1, 0.25f, 3U, NULL, 0U, 2U, 0 },
			{ GFX_SHADER(position_vert), GFX_SHADER(fearlystore_frag), 1, 0.75f, 3U, NULL, 0U, 3U, 0 }
		},
		0, 0U, gfx_check_fdepthstore, NULL
	},
	{
		"g-desccopy",
		3U, {
			{ 0U, GFX_UNIFORM, 16U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 3U },
			{ 1U, GFX_UNIFORM, 16U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U },
			{ 2U, GFX_UNIFORM, 16U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U }
		},
		1U, { { GFX_SHADER(position_vert), GFX_SHADER(ubo_two_frag), 0, 0.0f, 3U, NULL, 0U, 0U, 0 } },
		0, 0U, gfx_check_desccopy, &gfx_desccopy_options
	},
	{
		"g-aniso",
		1U, { { 0U, GFX_TEXTURE, 0U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U } },
		1U, { { GFX_SHADER(position_vert), GFX_SHADER(aniso_frag), 0, 0.0f, 3U, NULL, 0U, 0U, 0 } },
		0, 0U, gfx_check_aniso, &gfx_aniso_options
	},
	{
		"g-cubearray",
		1U, { { 0U, GFX_CUBES, 0U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U } },
		1U, { { GFX_SHADER(position_vert), GFX_SHADER(cubearray_frag), 0, 0.0f, 3U, NULL, 0U, 0U, 0 } },
		0, 0U, gfx_check_cubearray, &gfx_cubearray_options
	},
	{
		"g-formats",
		1U, { { 0U, GFX_STORAGE, GFX_FORMAT_WORDS * 4U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U } },
		0U, { { NULL, 0U, NULL, 0U, 0, 0.0f, 0U, NULL, 0U, 0U, 0 } },
		0, 0U, gfx_check_formats, &gfx_formats_options
	},
	{
		"g-blit3d",
		1U, { { 0U, GFX_STORAGE, 64U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U } },
		0U, { { NULL, 0U, NULL, 0U, 0, 0.0f, 0U, NULL, 0U, 0U, 0 } },
		0, 0U, gfx_check_blit3d, &gfx_blit3d_options
	},
	{
		"g-sarray",
		1U, { { 0U, GFX_TEXTURE, 0U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 3U } },
		1U, { { GFX_SHADER(position_vert), GFX_SHADER(sarray_frag), 0, 0.0f, 3U, NULL, 0U, 0U, 0 } },
		0, 0U, gfx_check_sarray, &gfx_sarray_options
	},
	{
		"g-uarray",
		1U, { { 1U, GFX_UNIFORM, 16U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 3U } },
		1U, { { GFX_SHADER(position_vert), GFX_SHADER(uarray_frag), 0, 0.0f, 3U, NULL, 0U, 0U, 0 } },
		0, 0U, gfx_check_uarray, &gfx_uarray_options
	},
	{
		"g-querylod",
		1U, { { 0U, GFX_TEXTURE, 0U, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, 0U } },
		1U, { { GFX_SHADER(position_vert), GFX_SHADER(querylod_frag), 0, 0.0f, 3U, NULL, 0U, 0U, 0 } },
		0, 0U, gfx_check_querylod, NULL
	},
	{
		"g-ginvoc",
		0U, { { 0U, GFX_UNIFORM, 0U, 0U, 0U, 0U } },
		1U, { { GFX_SHADER(position_vert), GFX_SHADER(flat_frag), 0, 0.0f, 3U, GFX_SHADER(ginvoc_geom), 0U, 0 } },
		0, 0U, gfx_check_ginvoc, NULL
	},
	{
		"g-gpsize",
		0U, { { 0U, GFX_UNIFORM, 0U, 0U, 0U, 0U } },
		1U, { { GFX_SHADER(psize_vert), GFX_SHADER(flat_frag), 0, 0.0f, 3U, GFX_SHADER(gpsize_geom), 0U, 0 } },
		0, 0U, gfx_check_gpsize, NULL
	}
};

/* The number of graphics checks. */
#define GFX_TEST_COUNT	(sizeof(gfx_tests) / sizeof(gfx_tests[0]))

static int gfx_main(int argc, char **argv);
static enum oracle_outcome gfx_run_test(struct vkcompute *run, const struct gfx_test *test, int corrupt, int *lost);
static VkResult gfx_host_buffer(struct vkcompute *run, struct gfx_objects *objects, VkBufferUsageFlags usage, VkDeviceSize bytes, VkBuffer *buffer, VkDeviceMemory *memory, void **mapped);
static VkResult gfx_image(struct vkcompute *run, struct gfx_objects *objects, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage *image, VkDeviceMemory *memory, VkImageView *view);
static VkResult gfx_create_resources(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
static VkResult gfx_create_texture(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
static VkResult gfx_create_cubes(struct vkcompute *run, struct gfx_objects *objects);
static VkResult gfx_create_vertices(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
static VkResult gfx_create_targets(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
static VkResult gfx_device_image(struct vkcompute *run, struct gfx_objects *objects, const VkImageCreateInfo *image_info, VkImage *image, VkDeviceMemory *memory);
static VkResult gfx_create_layout(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
static VkResult gfx_create_pipeline(struct vkcompute *run, const struct gfx_test *test, const struct gfx_draw *draw, struct gfx_objects *objects, uint32_t index);
static VkResult gfx_record(struct vkcompute *run, const struct gfx_test *test, struct gfx_objects *objects);
static void gfx_destroy(struct vkcompute *run, struct gfx_objects *objects);
static uint32_t gfx_device_memory_type(struct vkcompute *run, uint32_t type_bits);
static uint32_t gfx_block_word(uint32_t binding, uint32_t index);
static uint32_t gfx_texel(uint32_t x, uint32_t y, uint32_t channel);
static uint32_t gfx_clamp_texel(int32_t coordinate);

/*
 * Runs the graphics checks: every one, or the ones named after -g; -n is
 * the negative control.  Returns 0 when every check run passed.
 */
static int
gfx_main(
	int argc,
	char **argv)
{
	struct vkcompute run;
	VkQueueFamilyProperties families[8];
	const struct gfx_test *test;
	enum oracle_outcome outcome;
	uint32_t count;
	uint32_t index;
	uint32_t selected;
	uint32_t passed;
	VkResult status;
	int corrupt;
	int named;
	int chosen;
	int lost;
	int arg;

	/* Reads the options after -g: -n, and the names of the checks to run. */
	corrupt = 0;
	named = 0;
	for (arg = 2; arg < argc; arg++) {
		if (strcmp(argv[arg], "-n") == 0) {
			corrupt = 1;
			continue;
		}
		named = 1;
	}

	/* Opens the instance and the device; with none, no check can run. */
	memset(&run, 0, sizeof(run));
	status = vkcompute_open_device(&run);
	if (status != VK_SUCCESS) {
		printf("vkcompute: no Vulkan device (%s %d)\n", run.failed_call, (int)status);
		vkcompute_destroy(&run);
		return 1;
	}

	/* The queue the device was opened with must draw. */
	count = sizeof(families) / sizeof(families[0]);
	vkGetPhysicalDeviceQueueFamilyProperties(run.physical, &count, families);
	if (run.family >= count || (families[run.family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
		printf("vkcompute: %s: graphics FAIL, the compute queue family %u does not draw\n", run.name, run.family);
		vkcompute_destroy(&run);
		return 1;
	}

	/* Says that every failure to come is meant, under the negative control. */
	if (corrupt != 0)
		printf("vkcompute: %s: negative control: every graphics check must FAIL\n", run.name);

	/* Runs each selected check once, in the table's order. */
	lost = 0;
	selected = 0U;
	passed = 0U;
	for (index = 0U; index < GFX_TEST_COUNT; index++) {
		test = &gfx_tests[index];

		/* Skips a check the names leave out. */
		chosen = 1;
		if (named != 0) {
			chosen = 0;
			for (arg = 2; arg < argc; arg++) {
				if (strcmp(argv[arg], test->name) == 0)
					chosen = 1;
			}
		}
		if (chosen == 0)
			continue;
		selected++;

		/* Counts what a lost or hung device leaves without running. */
		if (lost != 0) {
			printf("vkcompute: %s: FAIL, not run (the device stopped answering)\n", test->name);
			continue;
		}

		/* Runs the check, which prints its own line. */
		outcome = gfx_run_test(&run, test, corrupt, &lost);
		if (outcome == ORACLE_PASS)
			passed++;
	}

	/* Prints the verdict: PASS needs every selected check to pass, and at least one to be selected. */
	if (selected != 0U && passed == selected) {
		printf("vkcompute: %s: graphics PASS, %u/%u\n", run.name, passed, selected);
	} else {
		printf("vkcompute: %s: graphics FAIL, %u/%u\n", run.name, passed, selected);
	}

	/* Leaves a hung device to the process exit: waiting for it to go idle could block for ever. */
	if (lost == 0)
		vkcompute_destroy(&run);

	/* Reports anything but every check passing as a failure. */
	if (selected == 0U || passed != selected)
		return 1;

	/* Succeeded: every selected graphics check passed. */
	return 0;
}

/*
 * Runs one graphics check: makes its resources and pipelines, draws,
 * copies the colour target out, waits and compares.  Prints the check's
 * line; sets *lost when the device stopped answering.
 */
static enum oracle_outcome
gfx_run_test(
	struct vkcompute *run,
	const struct gfx_test *test,
	int corrupt,
	int *lost)
{
	struct gfx_objects objects;
	struct oracle_check check;
	VkFenceCreateInfo fence_info;
	VkSubmitInfo submit;
	VkResult status;
	uint32_t index;

	/* Starts with no objects, so the teardown can run from any point. */
	memset(&objects, 0, sizeof(objects));

	/* Makes the buffers, the image, the targets and the vertices. */
	status = gfx_create_resources(run, test, &objects);
	if (status != VK_SUCCESS) {
		printf("vkcompute: %s: FAIL, %s %d\n", test->name, objects.failed_call, (int)status);
		gfx_destroy(run, &objects);
		return ORACLE_FAIL;
	}

	/* Makes the set layout, the pipeline layout, the render pass and the set. */
	status = gfx_create_layout(run, test, &objects);
	if (status != VK_SUCCESS) {
		printf("vkcompute: %s: FAIL, %s %d\n", test->name, objects.failed_call, (int)status);
		gfx_destroy(run, &objects);
		return ORACLE_FAIL;
	}

	/* Makes what the check needs for itself. */
	if (test->options != NULL && test->options->prepare != NULL) {
		status = test->options->prepare(run, test, &objects);
		if (status != VK_SUCCESS) {
			printf("vkcompute: %s: FAIL, %s %d\n", test->name, objects.failed_call, (int)status);
			gfx_destroy(run, &objects);
			return ORACLE_FAIL;
		}
	}

	/* Makes each draw's pipeline; a refused one is the driver's UNSUPPORTED. */
	for (index = 0U; index < test->draw_count; index++) {
		status = gfx_create_pipeline(run, test, &test->draws[index], &objects, index);
		if (status != VK_SUCCESS) {
			printf("vkcompute: %s: UNSUPPORTED, %s %d\n", test->name, objects.failed_call, (int)status);
			gfx_destroy(run, &objects);
			return ORACLE_UNSUPPORTED;
		}
	}

	/* Records the draws and the copy of the colour target. */
	status = gfx_record(run, test, &objects);
	if (status != VK_SUCCESS) {
		printf("vkcompute: %s: FAIL, %s %d\n", test->name, objects.failed_call, (int)status);
		gfx_destroy(run, &objects);
		return ORACLE_FAIL;
	}

	/* Submits them and waits; VK_TIMEOUT means the device hung. */
	memset(&fence_info, 0, sizeof(fence_info));
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	objects.failed_call = "vkCreateFence";
	status = vkCreateFence(run->device, &fence_info, NULL, &objects.fence);
	if (status == VK_SUCCESS) {
		memset(&submit, 0, sizeof(submit));
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &objects.command;
		objects.failed_call = "vkQueueSubmit";
		status = vkQueueSubmit(run->queue, 1, &submit, objects.fence);
	}
	if (status == VK_SUCCESS) {
		objects.failed_call = "vkWaitForFences";
		status = vkWaitForFences(run->device, 1, &objects.fence, VK_TRUE, VKCOMPUTE_WAIT_NS);
	}

	/* A device that did not finish is left alone: the next checks are not run. */
	if (status != VK_SUCCESS) {
		printf("vkcompute: %s: FAIL, %s %d\n", test->name, objects.failed_call, (int)status);
		if (status == VK_TIMEOUT || status == VK_ERROR_DEVICE_LOST) {
			*lost = 1;
			return ORACLE_FAIL;
		}
		gfx_destroy(run, &objects);
		return ORACLE_FAIL;
	}

	/* Compares the pixels and the buffers with the reference. */
	memset(&check, 0, sizeof(check));
	check.corrupt = corrupt;
	test->check(&check, test, objects.words);
	gfx_destroy(run, &objects);

	/* Prints the first wrong word of a failed check. */
	if (check.bad != 0U || check.compared == 0U) {
		printf("vkcompute: %s: FAIL, %u/%u words (buffer %u word %u: 0x%08x, expected 0x%08x)\n",
		       test->name,
		       check.compared - check.bad,
		       check.compared,
		       check.first_buffer,
		       check.first_index,
		       check.first_got,
		       check.first_expected);
		return ORACLE_FAIL;
	}

	/* Succeeded: every word holds what the reference computes. */
	printf("vkcompute: %s: PASS, %u/%u words\n", test->name, check.compared, check.compared);
	return ORACLE_PASS;
}

/*
 * Creates a buffer in host-visible, host-coherent memory and maps it; the
 * call that failed is left in objects->failed_call.
 */
static VkResult
gfx_host_buffer(
	struct vkcompute *run,
	struct gfx_objects *objects,
	VkBufferUsageFlags usage,
	VkDeviceSize bytes,
	VkBuffer *buffer,
	VkDeviceMemory *memory,
	void **mapped)
{
	VkBufferCreateInfo buffer_info;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate;
	VkResult status;

	/* Creates the buffer. */
	memset(&buffer_info, 0, sizeof(buffer_info));
	buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.size = bytes;
	buffer_info.usage = usage;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	objects->failed_call = "vkCreateBuffer";
	status = vkCreateBuffer(run->device, &buffer_info, NULL, buffer);
	if (status != VK_SUCCESS)
		return status;

	/* Allocates memory the CPU sees without flushes. */
	vkGetBufferMemoryRequirements(run->device, *buffer, &requirements);
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = oracle_memory_type(run, requirements.memoryTypeBits);
	objects->failed_call = "host-visible memory type";
	if (allocate.memoryTypeIndex == VK_MAX_MEMORY_TYPES)
		return VK_ERROR_FEATURE_NOT_PRESENT;
	objects->failed_call = "vkAllocateMemory";
	status = vkAllocateMemory(run->device, &allocate, NULL, memory);
	if (status != VK_SUCCESS)
		return status;

	/* Binds and maps it. */
	objects->failed_call = "vkBindBufferMemory";
	status = vkBindBufferMemory(run->device, *buffer, *memory, 0);
	if (status != VK_SUCCESS)
		return status;
	objects->failed_call = "vkMapMemory";
	status = vkMapMemory(run->device, *memory, 0, VK_WHOLE_SIZE, 0, mapped);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the buffer is mapped. */
	return VK_SUCCESS;
}

/* Creates a 64 x 64 image of the target's size in device memory, and its view. */
static VkResult
gfx_image(
	struct vkcompute *run,
	struct gfx_objects *objects,
	VkFormat format,
	VkImageUsageFlags usage,
	VkImageAspectFlags aspect,
	VkImage *image,
	VkDeviceMemory *memory,
	VkImageView *view)
{
	VkImageCreateInfo image_info;
	VkImageViewCreateInfo view_info;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate;
	VkResult status;

	/* Creates the image. */
	memset(&image_info, 0, sizeof(image_info));
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = format;
	image_info.extent.width = GFX_SIDE;
	image_info.extent.height = GFX_SIDE;
	image_info.extent.depth = 1;
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_info.usage = usage;
	image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	objects->failed_call = "vkCreateImage";
	status = vkCreateImage(run->device, &image_info, NULL, image);
	if (status != VK_SUCCESS)
		return status;

	/* Allocates and binds its memory. */
	vkGetImageMemoryRequirements(run->device, *image, &requirements);
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = gfx_device_memory_type(run, requirements.memoryTypeBits);
	objects->failed_call = "vkAllocateMemory";
	status = vkAllocateMemory(run->device, &allocate, NULL, memory);
	if (status != VK_SUCCESS)
		return status;
	objects->failed_call = "vkBindImageMemory";
	status = vkBindImageMemory(run->device, *image, *memory, 0);
	if (status != VK_SUCCESS)
		return status;

	/* Creates its view. */
	memset(&view_info, 0, sizeof(view_info));
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.image = *image;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = format;
	view_info.subresourceRange.aspectMask = aspect;
	view_info.subresourceRange.levelCount = 1;
	view_info.subresourceRange.layerCount = 1;
	objects->failed_call = "vkCreateImageView";
	status = vkCreateImageView(run->device, &view_info, NULL, view);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the image and its view. */
	return VK_SUCCESS;
}

/*
 * Makes a check's resources: each binding's buffer (filled: binding n's
 * word i is gfx_block_word(n, i); a storage buffer is zeros), the image,
 * the colour and depth targets, the host buffer the colour target is
 * copied into, and the vertices.
 */
static VkResult
gfx_create_resources(
	struct vkcompute *run,
	const struct gfx_test *test,
	struct gfx_objects *objects)
{
	const struct gfx_binding *binding;
	VkBufferUsageFlags usage;
	uint32_t *words;
	uint32_t index;
	uint32_t word;
	VkResult status;
	void *mapped;

	/* Each binding's buffer: its bytes after its offset. */
	for (index = 0U; index < test->binding_count; index++) {
		binding = &test->bindings[index];
		if (binding->kind == GFX_TEXTURE || binding->kind == GFX_CUBES)
			continue;

		/* Creates and maps the buffer; a storage buffer may also be filled, written and copied to by commands. */
		usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		if (binding->kind == GFX_STORAGE)
			usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		status = gfx_host_buffer(run, objects, usage, binding->offset + binding->bytes,
					 &objects->buffers[index], &objects->memories[index], &mapped);
		if (status != VK_SUCCESS)
			return status;

		/* Fills the words the shader sees; a storage buffer starts at zero, the bytes before the offset are a pattern. */
		words = (uint32_t *)((char *)mapped + binding->offset);
		objects->words[1U + index] = words;
		memset(mapped, 0xa5, binding->offset);
		for (word = 0U; word < binding->bytes / 4U; word++) {
			words[word] = 0U;
			if (binding->kind == GFX_UNIFORM)
				words[word] = gfx_block_word(binding->binding, word);
		}
	}

	/* Makes the image, or the cube array image, when a binding samples it. */
	for (index = 0U; index < test->binding_count; index++) {
		if (test->bindings[index].kind == GFX_TEXTURE) {
			status = gfx_create_texture(run, test, objects);
			if (status != VK_SUCCESS)
				return status;
		} else if (test->bindings[index].kind == GFX_CUBES) {
			status = gfx_create_cubes(run, objects);
			if (status != VK_SUCCESS)
				return status;
		}
	}

	/* Makes the targets and the host buffer of each colour target's copy. */
	status = gfx_create_targets(run, test, objects);
	if (status != VK_SUCCESS)
		return status;
	status = gfx_host_buffer(run, objects, VK_BUFFER_USAGE_TRANSFER_DST_BIT, GFX_PIXELS * 4U,
				 &objects->readback, &objects->readback_memory, &mapped);
	if (status != VK_SUCCESS)
		return status;
	objects->words[0] = (uint32_t *)mapped;
	memset(mapped, 0xa5, GFX_PIXELS * 4U);
	if (objects->color2 != VK_NULL_HANDLE) {
		status = gfx_host_buffer(run, objects, VK_BUFFER_USAGE_TRANSFER_DST_BIT, GFX_PIXELS * 4U,
					 &objects->readback2, &objects->readback2_memory, &mapped);
		if (status != VK_SUCCESS)
			return status;
		objects->words[GFX_SECOND_TARGET] = (uint32_t *)mapped;
		memset(mapped, 0xa5, GFX_PIXELS * 4U);
	}

	/* Makes the vertices. */
	status = gfx_create_vertices(run, test, objects);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: every resource of the check is made. */
	return VK_SUCCESS;
}

/*
 * Makes the 4 x 4 R8G8B8A8_UNORM image with the texels of gfx_texel(), in
 * linear tiling and host-visible memory written directly, and its view and
 * a nearest, clamped sampler.
 */
static VkResult
gfx_create_texture(
	struct vkcompute *run,
	const struct gfx_test *test,
	struct gfx_objects *objects)
{
	VkImageCreateInfo image_info;
	VkImageViewCreateInfo view_info;
	VkSamplerCreateInfo sampler_info;
	VkImageSubresource subresource;
	VkSubresourceLayout layout;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate;
	uint8_t *texels;
	uint32_t x;
	uint32_t y;
	uint32_t channel;
	VkResult status;
	void *mapped;

	/* Creates the image, linear so that the CPU writes its texels. */
	memset(&image_info, 0, sizeof(image_info));
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
	image_info.extent.width = GFX_TEXTURE_SIDE;
	image_info.extent.height = GFX_TEXTURE_SIDE;
	image_info.extent.depth = 1;
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_LINEAR;
	image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_info.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
	objects->failed_call = "vkCreateImage (texture)";
	status = vkCreateImage(run->device, &image_info, NULL, &objects->texture);
	if (status != VK_SUCCESS)
		return status;

	/* Allocates host-visible memory for it and binds it. */
	vkGetImageMemoryRequirements(run->device, objects->texture, &requirements);
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = oracle_memory_type(run, requirements.memoryTypeBits);
	objects->failed_call = "host-visible memory type (texture)";
	if (allocate.memoryTypeIndex == VK_MAX_MEMORY_TYPES)
		return VK_ERROR_FEATURE_NOT_PRESENT;
	objects->failed_call = "vkAllocateMemory (texture)";
	status = vkAllocateMemory(run->device, &allocate, NULL, &objects->texture_memory);
	if (status != VK_SUCCESS)
		return status;
	objects->failed_call = "vkBindImageMemory (texture)";
	status = vkBindImageMemory(run->device, objects->texture, objects->texture_memory, 0);
	if (status != VK_SUCCESS)
		return status;

	/* Writes the texels row by row at the image's own pitch. */
	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vkGetImageSubresourceLayout(run->device, objects->texture, &subresource, &layout);
	objects->failed_call = "vkMapMemory (texture)";
	status = vkMapMemory(run->device, objects->texture_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
	if (status != VK_SUCCESS)
		return status;
	for (y = 0U; y < GFX_TEXTURE_SIDE; y++) {
		texels = (uint8_t *)mapped + layout.offset + y * layout.rowPitch;
		for (x = 0U; x < GFX_TEXTURE_SIDE; x++) {
			for (channel = 0U; channel < 4U; channel++)
				texels[4U * x + channel] = (uint8_t)gfx_texel(x, y, channel);
		}
	}
	vkUnmapMemory(run->device, objects->texture_memory);

	/* Creates its view. */
	memset(&view_info, 0, sizeof(view_info));
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.image = objects->texture;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.levelCount = 1;
	view_info.subresourceRange.layerCount = 1;
	objects->failed_call = "vkCreateImageView (texture)";
	status = vkCreateImageView(run->device, &view_info, NULL, &objects->texture_view);
	if (status != VK_SUCCESS)
		return status;

	/* Creates the sampler: nearest, clamped to the edge (a gather reads four texels whatever the filter), or anisotropic. */
	memset(&sampler_info, 0, sizeof(sampler_info));
	sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler_info.magFilter = VK_FILTER_NEAREST;
	sampler_info.minFilter = VK_FILTER_NEAREST;
	sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if (test->options != NULL && test->options->anisotropy != 0) {
		sampler_info.magFilter = VK_FILTER_LINEAR;
		sampler_info.minFilter = VK_FILTER_LINEAR;
		sampler_info.anisotropyEnable = VK_TRUE;
		sampler_info.maxAnisotropy = 16.0f;
	}
	objects->failed_call = "vkCreateSampler";
	status = vkCreateSampler(run->device, &sampler_info, NULL, &objects->sampler);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the image, its view and its sampler. */
	return VK_SUCCESS;
}

/*
 * Makes the vertex buffer: each draw's vertices one after another -- a
 * triangle over the whole target (or a smaller one) at the draw's depth,
 * or points of one place whose y is their number.
 */
static VkResult
gfx_create_vertices(
	struct vkcompute *run,
	const struct gfx_test *test,
	struct gfx_objects *objects)
{
	const struct gfx_draw *draw;
	float *vertex;
	uint32_t total;
	uint32_t index;
	uint32_t number;
	VkResult status;
	void *mapped;

	/* Counts the vertices of every draw (a buffer of one for a check that draws nothing). */
	total = 0U;
	for (index = 0U; index < test->draw_count; index++)
		total += test->draws[index].vertices;
	if (total == 0U)
		total = 1U;

	/* Creates and maps the buffer, four floats to a vertex. */
	status = gfx_host_buffer(run, objects, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, (VkDeviceSize)total * 16U,
				 &objects->vertices, &objects->vertex_memory, &mapped);
	if (status != VK_SUCCESS)
		return status;

	/* Writes each draw's vertices. */
	vertex = (float *)mapped;
	for (index = 0U; index < test->draw_count; index++) {
		draw = &test->draws[index];
		for (number = 0U; number < draw->vertices; number++) {
			vertex[0] = 0.0f;
			vertex[1] = (float)number;
			vertex[2] = draw->z;
			vertex[3] = 1.0f;
			if (draw->vertices == 3U) {
				vertex[0] = -1.0f;
				vertex[1] = -1.0f;
				if (number == 1U)
					vertex[0] = 3.0f;
				if (number == 2U)
					vertex[1] = 3.0f;
			}
			if (draw->vertices == 3U && draw->small != 0) {
				vertex[0] = -0.5f;
				vertex[1] = -0.5f;
				if (number == 1U)
					vertex[0] = 0.5f;
				if (number == 2U)
					vertex[1] = 0.5f;
			}
			vertex += 4;
		}
	}

	/* Succeeded: the vertices are in place. */
	return VK_SUCCESS;
}

/*
 * Makes the colour targets (R32_UINT, or the check's format; one, or two)
 * and the depth target (D32_SFLOAT, or the check's format, which commands
 * may also clear).
 */
static VkResult
gfx_create_targets(
	struct vkcompute *run,
	const struct gfx_test *test,
	struct gfx_objects *objects)
{
	VkFormat color_format;
	VkFormat depth_format;
	VkImageAspectFlags depth_aspects;
	VkResult status;

	/* The formats the check asks for. */
	color_format = VK_FORMAT_R32_UINT;
	depth_format = VK_FORMAT_D32_SFLOAT;
	if (test->options != NULL && test->options->color_format != VK_FORMAT_UNDEFINED)
		color_format = test->options->color_format;
	if (test->options != NULL && test->options->depth_format != VK_FORMAT_UNDEFINED)
		depth_format = test->options->depth_format;
	depth_aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
	if (depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT)
		depth_aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;

	/* The colour target, which is drawn and then copied out. */
	status = gfx_image(run, objects, color_format,
			   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			   VK_IMAGE_ASPECT_COLOR_BIT, &objects->color, &objects->color_memory, &objects->color_view);
	if (status != VK_SUCCESS)
		return status;

	/* The second colour target of a check of two. */
	if (test->options != NULL && test->options->color_count == 2U) {
		status = gfx_image(run, objects, color_format,
				   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
				   VK_IMAGE_ASPECT_COLOR_BIT, &objects->color2, &objects->color2_memory, &objects->color2_view);
		if (status != VK_SUCCESS)
			return status;
	}

	/* The depth target. */
	status = gfx_image(run, objects, depth_format,
			   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			   depth_aspects, &objects->depth, &objects->depth_memory, &objects->depth_view);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the targets. */
	return VK_SUCCESS;
}

/*
 * Makes the set layout of the check's bindings, the pipeline layout (with
 * the vertex stage's push constant word when the check has one), the
 * render pass and framebuffer of the two targets, and the descriptor set.
 */
static VkResult
gfx_create_layout(
	struct vkcompute *run,
	const struct gfx_test *test,
	struct gfx_objects *objects)
{
	VkDescriptorSetLayoutBinding bindings[GFX_MAX_BINDINGS];
	VkDescriptorSetLayoutCreateInfo set_info;
	VkPushConstantRange push_range;
	VkPipelineLayoutCreateInfo layout_info;
	VkAttachmentDescription attachments[3];
	VkAttachmentReference color_references[2];
	VkAttachmentReference depth_reference;
	VkSubpassDescription subpass;
	VkRenderPassCreateInfo pass_info;
	VkImageView views[3];
	VkFramebufferCreateInfo framebuffer_info;
	VkDescriptorPoolSize pool_sizes[3];
	VkDescriptorPoolCreateInfo pool_info;
	VkDescriptorSetAllocateInfo set_allocate;
	VkDescriptorBufferInfo buffer_infos[GFX_MAX_BINDINGS];
	VkDescriptorImageInfo image_info;
	VkDescriptorImageInfo cube_info;
	VkWriteDescriptorSet writes[GFX_MAX_BINDINGS];
	const struct gfx_binding *binding;
	uint32_t index;
	uint32_t targets;
	VkResult status;

	/* Lays out the bindings. */
	memset(bindings, 0, sizeof(bindings));
	for (index = 0U; index < test->binding_count; index++) {
		binding = &test->bindings[index];
		bindings[index].binding = binding->binding;
		bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		if (binding->kind == GFX_STORAGE)
			bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		if (binding->kind == GFX_TEXTURE || binding->kind == GFX_CUBES)
			bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[index].descriptorCount = 1;
		if (binding->count != 0U)
			bindings[index].descriptorCount = binding->count;
		bindings[index].stageFlags = binding->stages;
	}
	memset(&set_info, 0, sizeof(set_info));
	set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_info.bindingCount = test->binding_count;
	set_info.pBindings = bindings;
	objects->failed_call = "vkCreateDescriptorSetLayout";
	status = vkCreateDescriptorSetLayout(run->device, &set_info, NULL, &objects->set_layout);
	if (status != VK_SUCCESS)
		return status;

	/* The pipeline layout: the set, and the push constants: the vertex stage's word, then push.frag's colour. */
	memset(&push_range, 0, sizeof(push_range));
	push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	push_range.size = 8U;
	memset(&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &objects->set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push_range;
	objects->failed_call = "vkCreatePipelineLayout";
	status = vkCreatePipelineLayout(run->device, &layout_info, NULL, &objects->pipeline_layout);
	if (status != VK_SUCCESS)
		return status;

	/*
	 * The render pass: each colour target cleared to 0 and kept for the
	 * copy, then the depth target cleared to 1.0 (stencil 0), or kept as
	 * the commands before the pass cleared it.
	 */
	targets = 1U;
	if (objects->color2 != VK_NULL_HANDLE)
		targets = 2U;
	memset(attachments, 0, sizeof(attachments));
	attachments[0].format = VK_FORMAT_R32_UINT;
	if (test->options != NULL && test->options->color_format != VK_FORMAT_UNDEFINED)
		attachments[0].format = test->options->color_format;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	attachments[1] = attachments[0];
	attachments[targets] = attachments[0];
	attachments[targets].format = VK_FORMAT_D32_SFLOAT;
	if (test->options != NULL && test->options->depth_format != VK_FORMAT_UNDEFINED)
		attachments[targets].format = test->options->depth_format;
	attachments[targets].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[targets].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[targets].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	if (test->options != NULL && test->options->depth_load != 0) {
		attachments[targets].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		attachments[targets].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		attachments[targets].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	}
	color_references[0].attachment = 0;
	color_references[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color_references[1].attachment = 1;
	color_references[1].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	depth_reference.attachment = targets;
	depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = targets;
	subpass.pColorAttachments = color_references;
	subpass.pDepthStencilAttachment = &depth_reference;
	memset(&pass_info, 0, sizeof(pass_info));
	pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	pass_info.attachmentCount = targets + 1U;
	pass_info.pAttachments = attachments;
	pass_info.subpassCount = 1;
	pass_info.pSubpasses = &subpass;
	objects->failed_call = "vkCreateRenderPass";
	status = vkCreateRenderPass(run->device, &pass_info, NULL, &objects->render_pass);
	if (status != VK_SUCCESS)
		return status;

	/* The framebuffer of the targets. */
	views[0] = objects->color_view;
	views[1] = objects->color2_view;
	views[targets] = objects->depth_view;
	memset(&framebuffer_info, 0, sizeof(framebuffer_info));
	framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer_info.renderPass = objects->render_pass;
	framebuffer_info.attachmentCount = targets + 1U;
	framebuffer_info.pAttachments = views;
	framebuffer_info.width = GFX_SIDE;
	framebuffer_info.height = GFX_SIDE;
	framebuffer_info.layers = 1;
	objects->failed_call = "vkCreateFramebuffer";
	status = vkCreateFramebuffer(run->device, &framebuffer_info, NULL, &objects->framebuffer);
	if (status != VK_SUCCESS)
		return status;

	/* A pool with room for every kind of binding, and the set. */
	pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	pool_sizes[0].descriptorCount = GFX_MAX_BINDINGS;
	pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	pool_sizes[1].descriptorCount = GFX_MAX_BINDINGS;
	pool_sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	pool_sizes[2].descriptorCount = GFX_MAX_BINDINGS;
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = 1;
	pool_info.poolSizeCount = 3;
	pool_info.pPoolSizes = pool_sizes;
	objects->failed_call = "vkCreateDescriptorPool";
	status = vkCreateDescriptorPool(run->device, &pool_info, NULL, &objects->pool);
	if (status != VK_SUCCESS)
		return status;
	memset(&set_allocate, 0, sizeof(set_allocate));
	set_allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	set_allocate.descriptorPool = objects->pool;
	set_allocate.descriptorSetCount = 1;
	set_allocate.pSetLayouts = &objects->set_layout;
	objects->failed_call = "vkAllocateDescriptorSets";
	status = vkAllocateDescriptorSets(run->device, &set_allocate, &objects->set);
	if (status != VK_SUCCESS)
		return status;

	/* Points each binding at its buffer's range, or at the image and its sampler. */
	memset(writes, 0, sizeof(writes));
	memset(&image_info, 0, sizeof(image_info));
	image_info.sampler = objects->sampler;
	image_info.imageView = objects->texture_view;
	image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	cube_info = image_info;
	cube_info.imageView = objects->cube_view;
	for (index = 0U; index < test->binding_count; index++) {
		binding = &test->bindings[index];
		writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[index].dstSet = objects->set;
		writes[index].dstBinding = binding->binding;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType = bindings[index].descriptorType;
		buffer_infos[index].buffer = objects->buffers[index];
		buffer_infos[index].offset = binding->offset;
		buffer_infos[index].range = binding->bytes;
		writes[index].pBufferInfo = &buffer_infos[index];
		if (binding->kind == GFX_TEXTURE) {
			writes[index].pBufferInfo = NULL;
			writes[index].pImageInfo = &image_info;
		}
		if (binding->kind == GFX_CUBES) {
			writes[index].pBufferInfo = NULL;
			writes[index].pImageInfo = &cube_info;
		}
	}
	if (test->binding_count != 0U)
		vkUpdateDescriptorSets(run->device, test->binding_count, writes, 0, NULL);

	/* Succeeded: the layouts, the pass and the set. */
	return VK_SUCCESS;
}

/*
 * Makes one draw's pipeline: its two shaders (three with a geometry
 * shader), the vertex buffer's vec4 at location 0, triangles, the target's
 * viewport, no culling, depth LESS when the draw tests depth, the colour's
 * red channel written; then what the check's options change.
 */
static VkResult
gfx_create_pipeline(
	struct vkcompute *run,
	const struct gfx_test *test,
	const struct gfx_draw *draw,
	struct gfx_objects *objects,
	uint32_t index)
{
	const struct gfx_options *options;
	static const struct gfx_options none;
	VkShaderModuleCreateInfo module_info;
	VkPipelineShaderStageCreateInfo stages[3];
	VkPipelineColorBlendAttachmentState blend_attachments[2];
	VkPipelineDynamicStateCreateInfo dynamic;
	VkDynamicState dynamic_bias;
	uint32_t targets;
	VkVertexInputBindingDescription vertex_binding;
	VkVertexInputAttributeDescription vertex_attribute;
	VkPipelineVertexInputStateCreateInfo vertex_input;
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkViewport viewport;
	VkRect2D scissor;
	VkPipelineViewportStateCreateInfo viewport_state;
	VkPipelineRasterizationStateCreateInfo raster;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineDepthStencilStateCreateInfo depth;
	VkPipelineColorBlendAttachmentState blend_attachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkGraphicsPipelineCreateInfo pipeline_info;
	VkResult status;

	/* The two shader modules. */
	memset(&module_info, 0, sizeof(module_info));
	module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module_info.codeSize = draw->vertex_size;
	module_info.pCode = draw->vertex;
	objects->failed_call = "vkCreateShaderModule (vertex)";
	status = vkCreateShaderModule(run->device, &module_info, NULL, &objects->modules[2U * index]);
	if (status != VK_SUCCESS)
		return status;
	module_info.codeSize = draw->fragment_size;
	module_info.pCode = draw->fragment;
	objects->failed_call = "vkCreateShaderModule (fragment)";
	status = vkCreateShaderModule(run->device, &module_info, NULL, &objects->modules[2U * index + 1U]);
	if (status != VK_SUCCESS)
		return status;
	memset(stages, 0, sizeof(stages));
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = objects->modules[2U * index];
	stages[0].pName = "main";
	stages[1] = stages[0];
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = objects->modules[2U * index + 1U];

	/* The geometry shader of a draw that has one. */
	if (draw->geometry != NULL) {
		module_info.codeSize = draw->geometry_size;
		module_info.pCode = draw->geometry;
		objects->failed_call = "vkCreateShaderModule (geometry)";
		status = vkCreateShaderModule(run->device, &module_info, NULL, &objects->geometry_modules[index]);
		if (status != VK_SUCCESS)
			return status;
		stages[2] = stages[0];
		stages[2].stage = VK_SHADER_STAGE_GEOMETRY_BIT;
		stages[2].module = objects->geometry_modules[index];
	}

	/* The check's options, none for a generic check. */
	options = test->options;
	if (options == NULL)
		options = &none;

	/* The vertex input: one vec4 at location 0, sixteen bytes apart. */
	memset(&vertex_binding, 0, sizeof(vertex_binding));
	vertex_binding.stride = 16U;
	vertex_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	memset(&vertex_attribute, 0, sizeof(vertex_attribute));
	vertex_attribute.format = VK_FORMAT_R32G32B32A32_SFLOAT;
	memset(&vertex_input, 0, sizeof(vertex_input));
	vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertex_input.vertexBindingDescriptionCount = 1;
	vertex_input.pVertexBindingDescriptions = &vertex_binding;
	vertex_input.vertexAttributeDescriptionCount = 1;
	vertex_input.pVertexAttributeDescriptions = &vertex_attribute;
	memset(&assembly, 0, sizeof(assembly));
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	/* The whole target, depths 0 to 1. */
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = (float)GFX_SIDE;
	viewport.height = (float)GFX_SIDE;
	viewport.maxDepth = 1.0f;
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent.width = GFX_SIDE;
	scissor.extent.height = GFX_SIDE;
	memset(&viewport_state, 0, sizeof(viewport_state));
	viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport_state.viewportCount = 1;
	viewport_state.pViewports = &viewport;
	viewport_state.scissorCount = 1;
	viewport_state.pScissors = &scissor;

	/* Triangles filled (or as the check asks), no culling, one sample; the check's depth clamp and depth bias. */
	memset(&raster, 0, sizeof(raster));
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = options->polygon_mode;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;
	if (options->depth_clamp != 0)
		raster.depthClampEnable = VK_TRUE;
	if (options->bias[index] != 0.0f || options->dynamic_draw == index + 1U) {
		raster.depthBiasEnable = VK_TRUE;
		raster.depthBiasConstantFactor = options->bias[index];
		raster.depthBiasClamp = options->bias_clamp[index];
	}

	/* The depth bias of the draw whose bias is dynamic comes from vkCmdSetDepthBias. */
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic_bias = VK_DYNAMIC_STATE_DEPTH_BIAS;
	dynamic.dynamicStateCount = 1;
	dynamic.pDynamicStates = &dynamic_bias;
	memset(&multisample, 0, sizeof(multisample));
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	/* The depth test and write of a draw that tests depth. */
	memset(&depth, 0, sizeof(depth));
	depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depth.depthCompareOp = VK_COMPARE_OP_LESS;
	if (options->depth_lequal != 0)
		depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	if (draw->depth_test != 0) {
		depth.depthTestEnable = VK_TRUE;
		depth.depthWriteEnable = VK_TRUE;
	}

	/* The stencil test of the draw the check names: EQUAL to what the stencil was cleared to. */
	if (options->stencil_draw == index + 1U) {
		depth.stencilTestEnable = VK_TRUE;
		depth.front.failOp = VK_STENCIL_OP_KEEP;
		depth.front.passOp = VK_STENCIL_OP_KEEP;
		depth.front.depthFailOp = VK_STENCIL_OP_KEEP;
		depth.front.compareOp = VK_COMPARE_OP_EQUAL;
		depth.front.compareMask = 0xffU;
		depth.front.reference = options->clear_stencil;
		depth.back = depth.front;
	}

	/* Each target's red channel, no blending; or the check's blend; or its targets ORed. */
	targets = 1U;
	if (options->color_count == 2U)
		targets = 2U;
	memset(&blend_attachment, 0, sizeof(blend_attachment));
	blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
	blend_attachments[0] = blend_attachment;
	blend_attachments[1] = blend_attachment;
	if (options->blend_given != 0) {
		blend_attachments[0] = options->blend[0];
		blend_attachments[1] = options->blend[1];
	}
	memset(&blend, 0, sizeof(blend));
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = targets;
	blend.pAttachments = blend_attachments;
	if (options->logic_or != 0) {
		blend.logicOpEnable = VK_TRUE;
		blend.logicOp = VK_LOGIC_OP_OR;
	}

	/* Creates the pipeline. */
	memset(&pipeline_info, 0, sizeof(pipeline_info));
	pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipeline_info.stageCount = 2;
	if (draw->geometry != NULL)
		pipeline_info.stageCount = 3;
	pipeline_info.pStages = stages;
	if (options->dynamic_draw == index + 1U)
		pipeline_info.pDynamicState = &dynamic;
	pipeline_info.pVertexInputState = &vertex_input;
	pipeline_info.pInputAssemblyState = &assembly;
	pipeline_info.pViewportState = &viewport_state;
	pipeline_info.pRasterizationState = &raster;
	pipeline_info.pMultisampleState = &multisample;
	pipeline_info.pDepthStencilState = &depth;
	pipeline_info.pColorBlendState = &blend;
	pipeline_info.layout = objects->pipeline_layout;
	pipeline_info.renderPass = objects->render_pass;
	objects->failed_call = "vkCreateGraphicsPipelines";
	status = vkCreateGraphicsPipelines(run->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &objects->pipelines[index]);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the draw's pipeline. */
	return VK_SUCCESS;
}

/*
 * Records the check: the image made ready for sampling, the render pass
 * with each draw, then the colour target copied into the host buffer.
 */
static VkResult
gfx_record(
	struct vkcompute *run,
	const struct gfx_test *test,
	struct gfx_objects *objects)
{
	VkCommandPoolCreateInfo pool_info;
	VkCommandBufferAllocateInfo allocate;
	VkCommandBufferBeginInfo begin;
	VkImageMemoryBarrier barrier;
	VkClearValue clears[3];
	VkRenderPassBeginInfo pass_begin;
	uint32_t push[2];
	VkBufferImageCopy copy;
	VkMemoryBarrier host;
	VkDeviceSize vertex_offset;
	uint32_t first;
	uint32_t index;
	VkResult status;

	/* Creates the command pool and buffer. */
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.queueFamilyIndex = run->family;
	objects->failed_call = "vkCreateCommandPool";
	status = vkCreateCommandPool(run->device, &pool_info, NULL, &objects->command_pool);
	if (status != VK_SUCCESS)
		return status;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocate.commandPool = objects->command_pool;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1;
	objects->failed_call = "vkAllocateCommandBuffers";
	status = vkAllocateCommandBuffers(run->device, &allocate, &objects->command);
	if (status != VK_SUCCESS)
		return status;
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	objects->failed_call = "vkBeginCommandBuffer";
	status = vkBeginCommandBuffer(objects->command, &begin);
	if (status != VK_SUCCESS)
		return status;

	/* Makes the image's host writes visible to the fragment shader that samples it. */
	if (objects->texture != VK_NULL_HANDLE) {
		memset(&barrier, 0, sizeof(barrier));
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = objects->texture;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier(objects->command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				     0, 0, NULL, 0, NULL, 1, &barrier);
	}

	/* Records what the check does before the pass. */
	if (test->options != NULL && test->options->before_pass != NULL)
		test->options->before_pass(run, test, objects);

	/* Starts the render pass: colour 0 (each target), depth 1.0 and stencil 0 (unless kept). */
	memset(clears, 0, sizeof(clears));
	clears[1].depthStencil.depth = 1.0f;
	clears[2].depthStencil.depth = 1.0f;
	if (objects->color2 != VK_NULL_HANDLE)
		memset(&clears[1], 0, sizeof(clears[1]));
	memset(&pass_begin, 0, sizeof(pass_begin));
	pass_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass_begin.renderPass = objects->render_pass;
	pass_begin.framebuffer = objects->framebuffer;
	pass_begin.renderArea.extent.width = GFX_SIDE;
	pass_begin.renderArea.extent.height = GFX_SIDE;
	pass_begin.clearValueCount = 2;
	if (objects->color2 != VK_NULL_HANDLE)
		pass_begin.clearValueCount = 3;
	pass_begin.pClearValues = clears;
	vkCmdBeginRenderPass(objects->command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);

	/*
	 * Draws each draw with its pipeline, the set, the push constants (the
	 * vertex stage's word and the draw's colour) and its vertices; an
	 * indirect check draws the first by vkCmdDrawIndirect, the second by
	 * vkCmdDrawIndexedIndirect through the index buffer.
	 */
	vertex_offset = 0;
	vkCmdBindVertexBuffers(objects->command, 0, 1, &objects->vertices, &vertex_offset);
	first = 0U;
	for (index = 0U; index < test->draw_count; index++) {
		vkCmdBindPipeline(objects->command, VK_PIPELINE_BIND_POINT_GRAPHICS, objects->pipelines[index]);
		if (test->binding_count != 0U) {
			vkCmdBindDescriptorSets(objects->command, VK_PIPELINE_BIND_POINT_GRAPHICS, objects->pipeline_layout,
						0, 1, &objects->set, 0, NULL);
		}
		push[0] = 0U;
		if (test->push != 0)
			push[0] = test->push_word;
		push[1] = test->draws[index].color;
		vkCmdPushConstants(objects->command, objects->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				   0, 8U, push);
		if (test->options != NULL && test->options->dynamic_draw == index + 1U)
			vkCmdSetDepthBias(objects->command, test->options->dynamic_bias, 0.0f, 0.0f);
		if (test->options != NULL && test->options->before_draw != NULL)
			test->options->before_draw(run, test, objects, index);
		if (test->options != NULL && test->options->indirect != 0 && index == 0U) {
			vkCmdDrawIndirect(objects->command, objects->extra_buffers[0], 0, 1, 16U);
		} else if (test->options != NULL && test->options->indirect != 0) {
			vkCmdBindIndexBuffer(objects->command, objects->extra_buffers[1], 0, VK_INDEX_TYPE_UINT32);
			vkCmdDrawIndexedIndirect(objects->command, objects->extra_buffers[0], 16U, 1, 20U);
		} else {
			vkCmdDraw(objects->command, test->draws[index].vertices, 1, first, 0);
		}
		first += test->draws[index].vertices;
	}
	vkCmdEndRenderPass(objects->command);

	/* Records what the check does after the pass. */
	if (test->options != NULL && test->options->after_pass != NULL)
		test->options->after_pass(run, test, objects);

	/* Copies each colour target, in the layout the pass left it, into its host buffer. */
	memset(&copy, 0, sizeof(copy));
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 1;
	copy.imageExtent.width = GFX_SIDE;
	copy.imageExtent.height = GFX_SIDE;
	copy.imageExtent.depth = 1;
	vkCmdCopyImageToBuffer(objects->command, objects->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, objects->readback, 1, &copy);
	if (objects->color2 != VK_NULL_HANDLE)
		vkCmdCopyImageToBuffer(objects->command, objects->color2, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, objects->readback2, 1, &copy);

	/* Makes the copy and the shaders' buffer writes visible to the CPU. */
	memset(&host, 0, sizeof(host));
	host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(objects->command, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
			     VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
	objects->failed_call = "vkEndCommandBuffer";
	status = vkEndCommandBuffer(objects->command);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the check is recorded. */
	return VK_SUCCESS;
}

/* Destroys whatever a check's run made, in the reverse order of making it. */
static void
gfx_destroy(
	struct vkcompute *run,
	struct gfx_objects *objects)
{
	uint32_t index;

	/* Waits for the device first; nothing is in use after it. */
	vkDeviceWaitIdle(run->device);

	/* The fence, the commands, the set and the pipelines. */
	if (objects->fence != VK_NULL_HANDLE)
		vkDestroyFence(run->device, objects->fence, NULL);
	if (objects->command_pool != VK_NULL_HANDLE)
		vkDestroyCommandPool(run->device, objects->command_pool, NULL);
	if (objects->pool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(run->device, objects->pool, NULL);
	for (index = 0U; index < GFX_MAX_DRAWS; index++) {
		if (objects->pipelines[index] != VK_NULL_HANDLE)
			vkDestroyPipeline(run->device, objects->pipelines[index], NULL);
	}
	for (index = 0U; index < 2U * GFX_MAX_DRAWS; index++) {
		if (objects->modules[index] != VK_NULL_HANDLE)
			vkDestroyShaderModule(run->device, objects->modules[index], NULL);
	}
	for (index = 0U; index < GFX_MAX_DRAWS; index++) {
		if (objects->geometry_modules[index] != VK_NULL_HANDLE)
			vkDestroyShaderModule(run->device, objects->geometry_modules[index], NULL);
	}
	if (objects->queries != VK_NULL_HANDLE)
		vkDestroyQueryPool(run->device, objects->queries, NULL);

	/* The framebuffer, the pass and the layouts. */
	if (objects->framebuffer != VK_NULL_HANDLE)
		vkDestroyFramebuffer(run->device, objects->framebuffer, NULL);
	if (objects->render_pass != VK_NULL_HANDLE)
		vkDestroyRenderPass(run->device, objects->render_pass, NULL);
	if (objects->pipeline_layout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(run->device, objects->pipeline_layout, NULL);
	if (objects->set_layout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(run->device, objects->set_layout, NULL);

	/* The images, their views, the sampler and their memory. */
	if (objects->sampler != VK_NULL_HANDLE)
		vkDestroySampler(run->device, objects->sampler, NULL);
	if (objects->texture_view != VK_NULL_HANDLE)
		vkDestroyImageView(run->device, objects->texture_view, NULL);
	if (objects->texture != VK_NULL_HANDLE)
		vkDestroyImage(run->device, objects->texture, NULL);
	if (objects->texture_memory != VK_NULL_HANDLE)
		vkFreeMemory(run->device, objects->texture_memory, NULL);
	if (objects->color_view != VK_NULL_HANDLE)
		vkDestroyImageView(run->device, objects->color_view, NULL);
	if (objects->color != VK_NULL_HANDLE)
		vkDestroyImage(run->device, objects->color, NULL);
	if (objects->color_memory != VK_NULL_HANDLE)
		vkFreeMemory(run->device, objects->color_memory, NULL);
	if (objects->depth_view != VK_NULL_HANDLE)
		vkDestroyImageView(run->device, objects->depth_view, NULL);
	if (objects->depth != VK_NULL_HANDLE)
		vkDestroyImage(run->device, objects->depth, NULL);
	if (objects->depth_memory != VK_NULL_HANDLE)
		vkFreeMemory(run->device, objects->depth_memory, NULL);

	/* The second colour target, the cube array's view and a check's own images. */
	if (objects->color2_view != VK_NULL_HANDLE)
		vkDestroyImageView(run->device, objects->color2_view, NULL);
	if (objects->color2 != VK_NULL_HANDLE)
		vkDestroyImage(run->device, objects->color2, NULL);
	if (objects->color2_memory != VK_NULL_HANDLE)
		vkFreeMemory(run->device, objects->color2_memory, NULL);
	if (objects->cube_view != VK_NULL_HANDLE)
		vkDestroyImageView(run->device, objects->cube_view, NULL);
	for (index = 0U; index < 2U; index++) {
		if (objects->extra_views[index] != VK_NULL_HANDLE)
			vkDestroyImageView(run->device, objects->extra_views[index], NULL);
	}
	for (index = 0U; index < GFX_FORMATS; index++) {
		if (objects->extra_images[index] != VK_NULL_HANDLE)
			vkDestroyImage(run->device, objects->extra_images[index], NULL);
		if (objects->extra_image_memories[index] != VK_NULL_HANDLE)
			vkFreeMemory(run->device, objects->extra_image_memories[index], NULL);
	}

	/* A check's own buffers, the second target's copy. */
	for (index = 0U; index < sizeof(objects->extra_buffers) / sizeof(objects->extra_buffers[0]); index++) {
		if (objects->extra_buffers[index] != VK_NULL_HANDLE)
			vkDestroyBuffer(run->device, objects->extra_buffers[index], NULL);
		if (objects->extra_memories[index] != VK_NULL_HANDLE)
			vkFreeMemory(run->device, objects->extra_memories[index], NULL);
	}
	if (objects->readback2 != VK_NULL_HANDLE)
		vkDestroyBuffer(run->device, objects->readback2, NULL);
	if (objects->readback2_memory != VK_NULL_HANDLE)
		vkFreeMemory(run->device, objects->readback2_memory, NULL);

	/* The buffers and their memory (freeing mapped memory unmaps it). */
	if (objects->readback != VK_NULL_HANDLE)
		vkDestroyBuffer(run->device, objects->readback, NULL);
	if (objects->readback_memory != VK_NULL_HANDLE)
		vkFreeMemory(run->device, objects->readback_memory, NULL);
	if (objects->vertices != VK_NULL_HANDLE)
		vkDestroyBuffer(run->device, objects->vertices, NULL);
	if (objects->vertex_memory != VK_NULL_HANDLE)
		vkFreeMemory(run->device, objects->vertex_memory, NULL);
	for (index = 0U; index < GFX_MAX_BINDINGS; index++) {
		if (objects->buffers[index] != VK_NULL_HANDLE)
			vkDestroyBuffer(run->device, objects->buffers[index], NULL);
		if (objects->memories[index] != VK_NULL_HANDLE)
			vkFreeMemory(run->device, objects->memories[index], NULL);
	}
}

/* Picks a memory type for a target: device-local when there is one the image takes, else any it takes. */
static uint32_t
gfx_device_memory_type(
	struct vkcompute *run,
	uint32_t type_bits)
{
	VkPhysicalDeviceMemoryProperties memory;
	uint32_t type;

	/* The first device-local type the image accepts. */
	vkGetPhysicalDeviceMemoryProperties(run->physical, &memory);
	for (type = 0; type < memory.memoryTypeCount; type++) {
		if ((type_bits & (1U << type)) == 0)
			continue;
		if ((memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0)
			return type;
	}

	/* Else the first type it accepts at all. */
	for (type = 0; type < memory.memoryTypeCount; type++) {
		if ((type_bits & (1U << type)) != 0)
			return type;
	}

	/* No type fits; the allocation will say so. */
	return 0U;
}

/* Word `index` of uniform binding `binding`: the same on every run, so that the reference computes it again. */
static uint32_t
gfx_block_word(
	uint32_t binding,
	uint32_t index)
{
	/* A hash of the binding and the word. */
	return oracle_hash((binding << 20) ^ index ^ 0x9e3779b9U);
}

/* The byte of channel `channel` of texel (x, y) of the sampled image. */
static uint32_t
gfx_texel(
	uint32_t x,
	uint32_t y,
	uint32_t channel)
{
	/* A hash of the texel and the channel, a byte of it. */
	return oracle_hash(0x7e000000U ^ (y << 8) ^ (x << 4) ^ channel) & 0xffU;
}

/* A texel coordinate clamped to the image, as CLAMP_TO_EDGE does. */
static uint32_t
gfx_clamp_texel(
	int32_t coordinate)
{
	/* Below the first texel and past the last one read the edge texels. */
	if (coordinate < 0)
		return 0U;
	if (coordinate >= (int32_t)GFX_TEXTURE_SIDE)
		return GFX_TEXTURE_SIDE - 1U;

	/* Inside the image the coordinate stands. */
	return (uint32_t)coordinate;
}

/* g-ubo-frag: (x + 64 y) ^ the sum of eleven small blocks' words ^ four words of the 64 KiB one. */
static void
gfx_check_ubo_frag(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint32_t sum;
	uint32_t big;
	uint32_t x;
	uint32_t y;
	uint32_t expected;

	UNUSED_PARAMETER(test);

	/* b0.v.x + b1.v.y + ... + b10.v.z: binding n's component n % 4. */
	sum = 0U;
	for (x = 0U; x <= 10U; x++)
		sum += gfx_block_word(x, x % 4U);

	/* big.v[0].x ^ big.v[1234].y ^ big.v[4079].w, without the tail's word. */
	big = gfx_block_word(11U, 0U) ^ gfx_block_word(11U, 4U * 1234U + 1U) ^ gfx_block_word(11U, 4U * 4079U + 3U);

	/* Each pixel, with its own word of the tail: big.tail[x % 16].z. */
	for (y = 0U; y < GFX_SIDE; y++) {
		for (x = 0U; x < GFX_SIDE; x++) {
			expected = (x + GFX_SIDE * y) ^ sum ^ big ^ gfx_block_word(11U, 4U * (4080U + x % 16U) + 2U);
			oracle_expect(check, 0U, x + GFX_SIDE * y, words[0][x + GFX_SIDE * y], expected);
		}
	}
}

/* g-ubo-vert: the vertex shader's v[3].x ^ v[128].y ^ v[255].z ^ the push word, ^ (x + 64 y). */
static void
gfx_check_ubo_vert(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint32_t value;
	uint32_t x;
	uint32_t y;

	/* What the vertex shader passes on. */
	value = gfx_block_word(0U, 4U * 3U) ^ gfx_block_word(0U, 4U * 128U + 1U) ^ gfx_block_word(0U, 4U * 255U + 2U) ^
		test->push_word;

	/* Each pixel mixes it with its place. */
	for (y = 0U; y < GFX_SIDE; y++) {
		for (x = 0U; x < GFX_SIDE; x++)
			oracle_expect(check, 0U, x + GFX_SIDE * y, words[0][x + GFX_SIDE * y], value ^ (x + GFX_SIDE * y));
	}
}

/*
 * g-gather: at u = (x % 3 + 1) / 4, v = (y % 3 + 1) / 4 the four texels
 * are columns i0 = x % 3, i1 = i0 + 1 and rows j0 = y % 3, j1 = j0 + 1,
 * one column further right (clamped) on bands where (y / 3) % 2 is 1; the
 * reply is (i0, j1), (i1, j1), (i1, j0), (i0, j0) of channel (x / 3) % 4.
 */
static void
gfx_check_gather(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint32_t channel;
	uint32_t shift;
	uint32_t i0;
	uint32_t i1;
	uint32_t j0;
	uint32_t j1;
	uint32_t x;
	uint32_t y;
	uint32_t expected;

	UNUSED_PARAMETER(test);

	/* Each pixel's four texels, packed a byte each. */
	for (y = 0U; y < GFX_SIDE; y++) {
		for (x = 0U; x < GFX_SIDE; x++) {
			channel = (x / 3U) % 4U;
			shift = (y / 3U) % 2U;
			i0 = gfx_clamp_texel((int32_t)(x % 3U + shift));
			i1 = gfx_clamp_texel((int32_t)(x % 3U + 1U + shift));
			j0 = y % 3U;
			j1 = j0 + 1U;
			expected = gfx_texel(i0, j1, channel) |
				   (gfx_texel(i1, j1, channel) << 8) |
				   (gfx_texel(i1, j0, channel) << 16) |
				   (gfx_texel(i0, j0, channel) << 24);
			oracle_expect(check, 0U, x + GFX_SIDE * y, words[0][x + GFX_SIDE * y], expected);
		}
	}
}

/* g-fragdepth: the second draw (depth 0.5) wins where the first one's computed depth (x + 0.5) / 64 is larger. */
static void
gfx_check_fragdepth(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint32_t x;
	uint32_t y;
	uint32_t expected;

	UNUSED_PARAMETER(test);

	/* Colour 1 left of the middle, colour 2 from it on. */
	for (y = 0U; y < GFX_SIDE; y++) {
		for (x = 0U; x < GFX_SIDE; x++) {
			expected = 1U;
			if (x >= GFX_SIDE / 2U)
				expected = 2U;
			oracle_expect(check, 0U, x + GFX_SIDE * y, words[0][x + GFX_SIDE * y], expected);
		}
	}
}

/* g-samplemask: odd columns 100 + the one sample's coverage bit, even columns the clear colour 0. */
static void
gfx_check_samplemask(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint32_t x;
	uint32_t y;
	uint32_t expected;

	UNUSED_PARAMETER(test);

	/* The sample mask keeps the odd columns only. */
	for (y = 0U; y < GFX_SIDE; y++) {
		for (x = 0U; x < GFX_SIDE; x++) {
			expected = 0U;
			if ((x & 1U) != 0U)
				expected = 101U;
			oracle_expect(check, 0U, x + GFX_SIDE * y, words[0][x + GFX_SIDE * y], expected);
		}
	}
}

/* g-vatomic: the count of vertices, every vertex's bit set, nothing drawn. */
static void
gfx_check_vatomic(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint32_t expected;
	uint32_t index;
	uint32_t bits;

	UNUSED_PARAMETER(test);

	/* The count: each vertex once (a draw without indices shades every vertex). */
	oracle_expect(check, 1U, 0U, words[1][0], GFX_VATOMIC_VERTICES);

	/* The bits of the vertices' numbers, and none past them. */
	for (index = 0U; index < 15U; index++) {
		expected = 0U;
		for (bits = 0U; bits < 32U; bits++) {
			if (32U * index + bits < GFX_VATOMIC_VERTICES)
				expected |= 1U << bits;
		}
		oracle_expect(check, 1U, 1U + index, words[1][1U + index], expected);
	}

	/* The points cover no pixel. */
	for (index = 0U; index < GFX_PIXELS; index++)
		oracle_expect(check, 0U, index, words[0][index], 0U);
}

/* g-vstore: the four words before the records left 0, each vertex's record, nothing drawn. */
static void
gfx_check_vstore(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	const uint32_t *record;
	uint32_t index;
	float half;
	uint32_t bits;

	UNUSED_PARAMETER(test);

	/* The words before the records, which the shader only reads. */
	for (index = 0U; index < 4U; index++)
		oracle_expect(check, 1U, index, words[1][index], 0U);

	/* Vertex n's record: n, 3n + 7, the bits of n / 2 as a float, n ^ 0xa5a5a5a5. */
	for (index = 0U; index < GFX_VATOMIC_VERTICES; index++) {
		record = &words[1][4U + 4U * index];
		half = (float)index * 0.5f;
		memcpy(&bits, &half, sizeof(bits));
		oracle_expect(check, 1U, 4U + 4U * index, record[0], index);
		oracle_expect(check, 1U, 5U + 4U * index, record[1], index * 3U + 7U);
		oracle_expect(check, 1U, 6U + 4U * index, record[2], bits);
		oracle_expect(check, 1U, 7U + 4U * index, record[3], index ^ 0xa5a5a5a5U);
	}

	/* The points cover no pixel. */
	for (index = 0U; index < GFX_PIXELS; index++)
		oracle_expect(check, 0U, index, words[0][index], 0U);
}

/*
 * Makes the 4 x 4 R8G8B8A8_UNORM cube array of two cubes (twelve layers),
 * whose layer n is gfx_cube_word(n) everywhere, with its cube array view
 * and a nearest sampler; the texels are copied in by gfx_upload_cubes()
 * from the buffer made here.
 */
static VkResult
gfx_create_cubes(
	struct vkcompute *run,
	struct gfx_objects *objects)
{
	VkImageCreateInfo image_info;
	VkImageViewCreateInfo view_info;
	VkSamplerCreateInfo sampler_info;
	uint32_t *words;
	uint32_t index;
	VkResult status;
	void *mapped;

	/* The image, optimal and filled by a copy. */
	memset(&image_info, 0, sizeof(image_info));
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
	image_info.extent.width = GFX_TEXTURE_SIDE;
	image_info.extent.height = GFX_TEXTURE_SIDE;
	image_info.extent.depth = 1;
	image_info.mipLevels = 1;
	image_info.arrayLayers = 12;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	status = gfx_device_image(run, objects, &image_info, &objects->extra_images[0], &objects->extra_image_memories[0]);
	if (status != VK_SUCCESS)
		return status;

	/* The texels the copy takes: each layer's word sixteen times. */
	status = gfx_host_buffer(run, objects, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 12U * 16U * 4U,
				 &objects->extra_buffers[5], &objects->extra_memories[5], &mapped);
	if (status != VK_SUCCESS)
		return status;
	words = (uint32_t *)mapped;
	for (index = 0U; index < 12U * 16U; index++)
		words[index] = gfx_cube_word(index / 16U);

	/* The cube array view of the two cubes. */
	memset(&view_info, 0, sizeof(view_info));
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.image = objects->extra_images[0];
	view_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
	view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.levelCount = 1;
	view_info.subresourceRange.layerCount = 12;
	objects->failed_call = "vkCreateImageView (cube array)";
	status = vkCreateImageView(run->device, &view_info, NULL, &objects->cube_view);
	if (status != VK_SUCCESS)
		return status;

	/* A nearest sampler. */
	memset(&sampler_info, 0, sizeof(sampler_info));
	sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler_info.magFilter = VK_FILTER_NEAREST;
	sampler_info.minFilter = VK_FILTER_NEAREST;
	sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	objects->failed_call = "vkCreateSampler (cube array)";
	status = vkCreateSampler(run->device, &sampler_info, NULL, &objects->sampler);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the cube array, its view and its sampler. */
	return VK_SUCCESS;
}

/* Creates an image in device memory and binds it; the call that failed is left in objects->failed_call. */
static VkResult
gfx_device_image(
	struct vkcompute *run,
	struct gfx_objects *objects,
	const VkImageCreateInfo *image_info,
	VkImage *image,
	VkDeviceMemory *memory)
{
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate;
	VkResult status;

	/* Creates the image. */
	objects->failed_call = "vkCreateImage";
	status = vkCreateImage(run->device, image_info, NULL, image);
	if (status != VK_SUCCESS)
		return status;

	/* Allocates and binds its memory. */
	vkGetImageMemoryRequirements(run->device, *image, &requirements);
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = gfx_device_memory_type(run, requirements.memoryTypeBits);
	objects->failed_call = "vkAllocateMemory";
	status = vkAllocateMemory(run->device, &allocate, NULL, memory);
	if (status != VK_SUCCESS)
		return status;
	objects->failed_call = "vkBindImageMemory";
	status = vkBindImageMemory(run->device, *image, *memory, 0);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the image is bound. */
	return VK_SUCCESS;
}

/* Records a layout change of an image's first level and `layers` layers, after everything before it. */
static void
gfx_image_barrier(
	VkCommandBuffer command,
	VkImage image,
	VkImageAspectFlags aspects,
	uint32_t layers,
	VkImageLayout from,
	VkImageLayout to)
{
	VkImageMemoryBarrier barrier;

	/* Every access before, every access after. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.oldLayout = from;
	barrier.newLayout = to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = aspects;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = layers;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			     0, 0, NULL, 0, NULL, 1, &barrier);
}

/* The word of every texel of layer `layer` of the cube array. */
static uint32_t
gfx_cube_word(
	uint32_t layer)
{
	/* A hash of the layer. */
	return oracle_hash(0x6c000000U ^ layer);
}

/* g-cleards: the depth target's depth and stencil cleared by vkCmdClearDepthStencilImage before the pass. */
static void
gfx_clear_depth_image(
	struct vkcompute *run,
	const struct gfx_test *test,
	struct gfx_objects *objects)
{
	VkClearDepthStencilValue value;
	VkImageSubresourceRange range;

	UNUSED_PARAMETER(run);

	/* Into the clear's layout, cleared, then into the layout the pass keeps it in. */
	gfx_image_barrier(objects->command, objects->depth, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 1U,
			  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	value.depth = test->options->clear_depth;
	value.stencil = test->options->clear_stencil;
	memset(&range, 0, sizeof(range));
	range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	range.levelCount = 1;
	range.layerCount = 1;
	vkCmdClearDepthStencilImage(objects->command, objects->depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
	gfx_image_barrier(objects->command, objects->depth, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 1U,
			  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
}

/* g-indirect: the records' buffer (zeros until the commands write it) and the index buffer {0, 1, 2}. */
static VkResult
gfx_prepare_indirect(
	struct vkcompute *run,
	const struct gfx_test *test,
	struct gfx_objects *objects)
{
	uint32_t *words;
	VkResult status;
	void *mapped;

	UNUSED_PARAMETER(test);

	/* The records' buffer, which vkCmdUpdateBuffer writes. */
	status = gfx_host_buffer(run, objects, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 64U,
				 &objects->extra_buffers[0], &objects->extra_memories[0], &mapped);
	if (status != VK_SUCCESS)
		return status;
	memset(mapped, 0, 64U);

	/* The index buffer. */
	status = gfx_host_buffer(run, objects, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, 16U,
				 &objects->extra_buffers[1], &objects->extra_memories[1], &mapped);
	if (status != VK_SUCCESS)
		return status;
	words = (uint32_t *)mapped;
	words[0] = 0U;
	words[1] = 1U;
	words[2] = 2U;
	words[3] = 0U;

	/* Succeeded: both buffers. */
	return VK_SUCCESS;
}

/*
 * g-indirect: writes the two records before the pass: three vertices of
 * one instance from vertex 0; three indices of one instance from index 0,
 * each moved by the vertex offset 3 (the second draw's vertices).
 */
static void
gfx_write_records(
	struct vkcompute *run,
	const struct gfx_test *test,
	struct gfx_objects *objects)
{
	static const uint32_t draw[4] = { 3U, 1U, 0U, 0U };
	static const uint32_t indexed[5] = { 3U, 1U, 0U, 3U, 0U };

	UNUSED_PARAMETER(run);
	UNUSED_PARAMETER(test);

	/* The two records, then their writes made visible to the indirect reads. */
	vkCmdUpdateBuffer(objects->command, objects->extra_buffers[0], 0U, sizeof(draw), draw);
	vkCmdUpdateBuffer(objects->command, objects->extra_buffers[0], 16U, sizeof(indexed), indexed);
	gfx_memory_barrier(objects->command);
}

/*
 * g-fill: fills words 4 to 11 of the buffer, writes words 10 to 13 over
 * them, and fills words 16 to its end (VK_WHOLE_SIZE).
 */
static void
gfx_write_fill(
	struct vkcompute *run,
	const struct gfx_test *test,
	struct gfx_objects *objects)
{
	static const uint32_t update[4] = { 11U, 22U, 33U, 44U };

	UNUSED_PARAMETER(run);
	UNUSED_PARAMETER(test);

	/* The fill, the update over its end, the fill to the end of the buffer, each after the one before. */
	vkCmdFillBuffer(objects->command, objects->buffers[0], 16U, 32U, 0xa5c3e17fU);
	gfx_memory_barrier(objects->command);
	vkCmdUpdateBuffer(objects->command, objects->buffers[0], 40U, sizeof(update), update);
	gfx_memory_barrier(objects->command);
	vkCmdFillBuffer(objects->command, objects->buffers[0], 64U, VK_WHOLE_SIZE, 0x01020304U);
	gfx_memory_barrier(objects->command);
}

/* g-timestamp: the pool of two timestamps. */
static VkResult
gfx_prepare_timestamp(
	struct vkcompute *run,
	const struct gfx_test *test,
	struct gfx_objects *objects)
{
	VkQueryPoolCreateInfo pool_info;
	VkResult status;

	UNUSED_PARAMETER(test);

	/* The pool. */
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
	pool_info.queryCount = 2;
	objects->failed_call = "vkCreateQueryPool (timestamp)";
	status = vkCreateQueryPool(run->device, &pool_info, NULL, &objects->queries);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the pool. */
	return VK_SUCCESS;
}

/* g-timestamp: resets the pool and takes the first timestamp before the pass. */
static void
gfx_first_timestamp(
	struct vkcompute *run,
	const struct gfx_test *test,
	struct gfx_objects *objects)
{
	UNUSED_PARAMETER(run);
	UNUSED_PARAMETER(test);

	/* The reset, then the first timestamp. */
	vkCmdResetQueryPool(objects->command, objects->queries, 0, 2);
	vkCmdWriteTimestamp(objects->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, objects->queries, 0);
}

/* g-timestamp: takes the second timestamp after the pass and copies both, 64-bit with availability, into the buffer. */
static void
gfx_last_timestamp(
	struct vkcompute *run,
	const struct gfx_test *test,
	struct gfx_objects *objects)
{
	UNUSED_PARAMETER(run);
	UNUSED_PARAMETER(test);

	/* The second timestamp, then the copy of both records sixteen bytes apart. */
	vkCmdWriteTimestamp(objects->command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, objects->queries, 1);
	vkCmdCopyQueryPoolResults(objects->command, objects->queries, 0, 2, objects->buffers[0], 0U, 16U,
				  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT);
	gfx_memory_barrier(objects->command);
}

/* g-occlusion and g-occlusion-mrt: the pool of one occlusion query. */
static VkResult
gfx_prepare_occlusion(
	struct vkcompute *run,
	const struct gfx_test *test,
	struct gfx_objects *objects)
{
	VkQueryPoolCreateInfo pool_info;
	VkResult status;

	UNUSED_PARAMETER(test);

	/* The pool. */
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	pool_info.queryType = VK_QUERY_TYPE_OCCLUSION;
	pool_info.queryCount = 1;
	objects->failed_call = "vkCreateQueryPool (occlusion)";
	status = vkCreateQueryPool(run->device, &pool_info, NULL, &objects->queries);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the pool. */
	return VK_SUCCESS;
}

/* g-occlusion and g-occlusion-mrt: resets the query and begins it, precise when the device can, before the pass. */
static void
gfx_begin_occlusion(
	struct vkcompute *run,
	const struct gfx_test *test,
	struct gfx_objects *objects)
{
	VkPhysicalDeviceFeatures features;
	VkQueryControlFlags control;

	UNUSED_PARAMETER(test);

	/* Precise counts where the device has them (the device was made with every feature it reports). */
	vkGetPhysicalDeviceFeatures(run->physical, &features);
	control = 0U;
	gfx_occlusion_precise = 0;
	if (features.occlusionQueryPrecise) {
		control = VK_QUERY_CONTROL_PRECISE_BIT;
		gfx_occlusion_precise = 1;
	}

	/* The reset, then the begin. */
	vkCmdResetQueryPool(objects->command, objects->queries, 0, 1);
	vkCmdBeginQuery(objects->command, objects->queries, 0, control);
}

/* g-occlusion and g-occlusion-mrt: ends the query after the pass and copies it, 64-bit with availability, into the buffer. */
static void
gfx_end_occlusion(
	struct vkcompute *run,
	const struct gfx_test *test,
	struct gfx_objects *objects)
{
	UNUSED_PARAMETER(run);
	UNUSED_PARAMETER(test);

	/* The end, then the copy of the record. */
	vkCmdEndQuery(objects->command, objects->queries, 0);
	vkCmdCopyQueryPoolResults(objects->command, objects->queries, 0, 1, objects->buffers[0], 0U, 16U,
				  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT);
	gfx_memory_barrier(objects->command);
}

/*
 * g-desccopy: four buffers of one word each (gfx_block_word(20 + n, 0));
 * binding 0's three elements written with the first three in one write; a
 * write of two descriptors from binding 0's element 2 on, whose second
 * runs on into binding 1; binding 0's element 1 copied into binding 2.
 */
static VkResult
gfx_prepare_desccopy(
	struct vkcompute *run,
	const struct gfx_test *test,
	struct gfx_objects *objects)
{
	VkDescriptorBufferInfo infos[4];
	VkWriteDescriptorSet writes[2];
	VkCopyDescriptorSet copy;
	uint32_t index;
	VkResult status;
	void *mapped;

	UNUSED_PARAMETER(test);

	/* The four buffers and their words. */
	for (index = 0U; index < 4U; index++) {
		status = gfx_host_buffer(run, objects, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 16U,
					 &objects->extra_buffers[index], &objects->extra_memories[index], &mapped);
		if (status != VK_SUCCESS)
			return status;
		memset(mapped, 0, 16U);
		*(uint32_t *)mapped = gfx_block_word(20U + index, 0U);
		infos[index].buffer = objects->extra_buffers[index];
		infos[index].offset = 0;
		infos[index].range = 16U;
	}

	/* The two writes, in order, then the copy. */
	memset(writes, 0, sizeof(writes));
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = objects->set;
	writes[0].dstBinding = 0;
	writes[0].dstArrayElement = 0;
	writes[0].descriptorCount = 3;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writes[0].pBufferInfo = &infos[0];
	writes[1] = writes[0];
	writes[1].dstArrayElement = 2;
	writes[1].descriptorCount = 2;
	writes[1].pBufferInfo = &infos[2];
	memset(&copy, 0, sizeof(copy));
	copy.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
	copy.srcSet = objects->set;
	copy.srcBinding = 0;
	copy.srcArrayElement = 1;
	copy.dstSet = objects->set;
	copy.dstBinding = 2;
	copy.dstArrayElement = 0;
	copy.descriptorCount = 1;
	vkUpdateDescriptorSets(run->device, 2, writes, 1, &copy);

	/* Succeeded: the descriptors are in place. */
	return VK_SUCCESS;
}

/* g-cubearray: copies the twelve layers' texels into the cube array before the pass. */
static void
gfx_upload_cubes(
	struct vkcompute *run,
	const struct gfx_test *test,
	struct gfx_objects *objects)
{
	VkBufferImageCopy copy;

	UNUSED_PARAMETER(run);
	UNUSED_PARAMETER(test);

	/* Into the copy's layout, the twelve layers copied, then into the sampled layout. */
	gfx_image_barrier(objects->command, objects->extra_images[0], VK_IMAGE_ASPECT_COLOR_BIT, 12U,
			  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	memset(&copy, 0, sizeof(copy));
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 12;
	copy.imageExtent.width = GFX_TEXTURE_SIDE;
	copy.imageExtent.height = GFX_TEXTURE_SIDE;
	copy.imageExtent.depth = 1;
	vkCmdCopyBufferToImage(objects->command, objects->extra_buffers[5], objects->extra_images[0],
			       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	gfx_image_barrier(objects->command, objects->extra_images[0], VK_IMAGE_ASPECT_COLOR_BIT, 12U,
			  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

/*
 * g-formats: the formats, each a 2 x 1 image; a nonzero `copied` is
 * uploaded from the buffer's words 16 n + 8 on and copied back to 16 n,
 * any other is cleared to `clear` and copied back to 16 n.
 */
struct gfx_format_case {
	VkFormat format;
	int copied;
	VkClearColorValue clear;
	uint32_t upload[2];
};

/* The cases, in the order of their images.  The table is constant for the life of the program. */
static const struct gfx_format_case gfx_format_cases[GFX_FORMATS] = {
	{ VK_FORMAT_R16G16B16A16_UNORM, 0, { .float32 = { 0.2f, 0.4f, 0.6f, 1.0f } }, { 0U, 0U } },
	{ VK_FORMAT_R8G8B8A8_SNORM, 0, { .float32 = { -1.0f, 0.0f, 1.0f, 64.0f / 127.0f } }, { 0U, 0U } },
	{ VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 0, { .float32 = { 1.0f, 0.5f, 0.25f, 0.0f } }, { 0U, 0U } },
	{ VK_FORMAT_R16_SINT, 0, { .int32 = { -5, 0, 0, 0 } }, { 0U, 0U } },
	{ VK_FORMAT_A2B10G10R10_UNORM_PACK32, 0, { .float32 = { 1.0f, 0.0f, 1.0f, 1.0f / 3.0f } }, { 0U, 0U } },
	{ VK_FORMAT_R8G8B8A8_SNORM, 1, { .uint32 = { 0U, 0U, 0U, 0U } }, { 0x807f0180U, 0x00ff8001U } },
	{ VK_FORMAT_R16_SFLOAT, 1, { .uint32 = { 0U, 0U, 0U, 0U } }, { 0x00017e01U, 0U } },
	{ VK_FORMAT_R16G16B16A16_UINT, 0, { .uint32 = { 1U, 2U, 3U, 65535U } }, { 0U, 0U } }
};

/* The words each case's copy brings back: two texels' worth (four words for an eight-byte format). */
static const uint32_t gfx_format_words[GFX_FORMATS][4] = {
	{ 13107U | (26214U << 16), 39321U | (65535U << 16), 13107U | (26214U << 16), 39321U | (65535U << 16) },
	{ 0x407f0081U, 0x407f0081U, 0U, 0U },
	{ 0x81010100U, 0x81010100U, 0U, 0U },
	{ 0xfffbfffbU, 0U, 0U, 0U },
	{ 0x7ff003ffU, 0x7ff003ffU, 0U, 0U },
	{ 0x807f0180U, 0x00ff8001U, 0U, 0U },
	{ 0x00017e01U, 0U, 0U, 0U },
	{ 0x00020001U, 0xffff0003U, 0x00020001U, 0xffff0003U }
};

/* g-formats: the images, and each copied case's words in the buffer (the rest of it is zeros). */
static VkResult
gfx_prepare_formats(
	struct vkcompute *run,
	const struct gfx_test *test,
	struct gfx_objects *objects)
{
	VkImageCreateInfo image_info;
	uint32_t index;
	VkResult status;

	UNUSED_PARAMETER(test);

	/* Each case's image: 2 x 1, optimal, cleared and copied. */
	for (index = 0U; index < GFX_FORMATS; index++) {
		memset(&image_info, 0, sizeof(image_info));
		image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		image_info.imageType = VK_IMAGE_TYPE_2D;
		image_info.format = gfx_format_cases[index].format;
		image_info.extent.width = 2;
		image_info.extent.height = 1;
		image_info.extent.depth = 1;
		image_info.mipLevels = 1;
		image_info.arrayLayers = 1;
		image_info.samples = VK_SAMPLE_COUNT_1_BIT;
		image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
		image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		status = gfx_device_image(run, objects, &image_info, &objects->extra_images[index], &objects->extra_image_memories[index]);
		if (status != VK_SUCCESS)
			return status;

		/* The words a copied case is uploaded from. */
		objects->words[1][16U * index + 8U] = gfx_format_cases[index].upload[0];
		objects->words[1][16U * index + 9U] = gfx_format_cases[index].upload[1];
	}

	/* Succeeded: the images and the words. */
	return VK_SUCCESS;
}

/* g-formats: clears or uploads each image, then copies it back into the buffer, before the pass. */
static void
gfx_record_formats(
	struct vkcompute *run,
	const struct gfx_test *test,
	struct gfx_objects *objects)
{
	VkImageSubresourceRange range;
	VkBufferImageCopy copy;
	uint32_t index;

	UNUSED_PARAMETER(run);
	UNUSED_PARAMETER(test);

	/* Each case's image. */
	memset(&range, 0, sizeof(range));
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = 1;
	range.layerCount = 1;
	memset(&copy, 0, sizeof(copy));
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 1;
	copy.imageExtent.width = 2;
	copy.imageExtent.height = 1;
	copy.imageExtent.depth = 1;
	for (index = 0U; index < GFX_FORMATS; index++) {
		/* Cleared, or uploaded from the buffer's words 16 n + 8 on. */
		gfx_image_barrier(objects->command, objects->extra_images[index], VK_IMAGE_ASPECT_COLOR_BIT, 1U,
				  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		if (gfx_format_cases[index].copied != 0) {
			copy.bufferOffset = (16U * index + 8U) * 4U;
			vkCmdCopyBufferToImage(objects->command, objects->buffers[0], objects->extra_images[index],
					       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		} else {
			vkCmdClearColorImage(objects->command, objects->extra_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					     &gfx_format_cases[index].clear, 1, &range);
		}

		/* Copied back to the buffer's words 16 n on. */
		gfx_image_barrier(objects->command, objects->extra_images[index], VK_IMAGE_ASPECT_COLOR_BIT, 1U,
				  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		copy.bufferOffset = 16U * index * 4U;
		vkCmdCopyImageToBuffer(objects->command, objects->extra_images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				       objects->buffers[0], 1, &copy);
	}
	gfx_memory_barrier(objects->command);
}

/* Records a barrier between every memory access before it and every one after it. */
static void
gfx_memory_barrier(
	VkCommandBuffer command)
{
	VkMemoryBarrier barrier;

	/* All writes before, all accesses after. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			     0, 1, &barrier, 0, NULL, 0, NULL);
}

/* The bytes of texel (x, y) of the 4 x 4 image packed into a word, red first. */
static uint32_t
gfx_texel_word(
	uint32_t x,
	uint32_t y)
{
	/* The four channels' bytes. */
	return gfx_texel(x, y, 0U) | (gfx_texel(x, y, 1U) << 8) | (gfx_texel(x, y, 2U) << 16) | (gfx_texel(x, y, 3U) << 24);
}

/* g-vtex: texel (2, 1), which the vertex shader sampled, mixed with each pixel's place. */
static void
gfx_check_vtex(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint32_t value;
	uint32_t index;

	UNUSED_PARAMETER(test);

	/* Each pixel. */
	value = gfx_texel_word(2U, 1U);
	for (index = 0U; index < GFX_PIXELS; index++)
		oracle_expect(check, 0U, index, words[0][index], value ^ index);
}

/* g-gtex: texel (3, 2), which the geometry shader sampled, mixed with each pixel's place. */
static void
gfx_check_gtex(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint32_t value;
	uint32_t index;

	UNUSED_PARAMETER(test);

	/* Each pixel. */
	value = gfx_texel_word(3U, 2U);
	for (index = 0U; index < GFX_PIXELS; index++)
		oracle_expect(check, 0U, index, words[0][index], value ^ index);
}

/* A check whose every pixel is one colour: the table's push_word. */
static void
gfx_check_constant(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint32_t index;

	/* Each pixel. */
	for (index = 0U; index < GFX_PIXELS; index++)
		oracle_expect(check, 0U, index, words[0][index], test->push_word);
}

/* Counts the pixels of the target that hold `color`; *other is set when any holds anything but it and 0. */
static uint32_t
gfx_count_color(
	const uint32_t *pixels,
	uint32_t color,
	int *other)
{
	uint32_t count;
	uint32_t index;

	/* Each pixel. */
	count = 0U;
	*other = 0;
	for (index = 0U; index < GFX_PIXELS; index++) {
		if (pixels[index] == color) {
			count++;
		} else if (pixels[index] != 0U) {
			*other = 1;
		}
	}

	/* Succeeded: the count. */
	return count;
}

/*
 * g-polyline: the outline of the triangle (16, 16), (48, 16), (16, 48):
 * between 60 and 160 pixels (the three edges are about 109 long), none
 * inside it at (20, 20) or (30, 30) and none of another colour.
 */
static void
gfx_check_outline(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint32_t count;
	uint32_t in_range;
	int other;

	UNUSED_PARAMETER(test);

	/* The outline's pixels, as one word: 1 when their count is as an outline's. */
	count = gfx_count_color(words[0], GFX_OUTLINE_COLOR, &other);
	in_range = 0U;
	if (count >= 60U && count <= 160U && other == 0)
		in_range = 1U;
	oracle_expect(check, 0U, GFX_PIXELS, in_range, 1U);

	/* The inside is not drawn. */
	oracle_expect(check, 0U, 20U + GFX_SIDE * 20U, words[0][20U + GFX_SIDE * 20U], 0U);
	oracle_expect(check, 0U, 30U + GFX_SIDE * 30U, words[0][30U + GFX_SIDE * 30U], 0U);
}

/* g-polypoint: the three vertices as points, 3 to 12 pixels, none inside the triangle. */
static void
gfx_check_dots(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint32_t count;
	uint32_t in_range;
	int other;

	UNUSED_PARAMETER(test);

	/* The points' pixels, as one word: 1 when their count is as three points'. */
	count = gfx_count_color(words[0], GFX_OUTLINE_COLOR, &other);
	in_range = 0U;
	if (count >= 3U && count <= 12U && other == 0)
		in_range = 1U;
	oracle_expect(check, 0U, GFX_PIXELS, in_range, 1U);

	/* The edges and the inside are not drawn. */
	oracle_expect(check, 0U, 30U + GFX_SIDE * 16U, words[0][30U + GFX_SIDE * 16U], 0U);
	oracle_expect(check, 0U, 20U + GFX_SIDE * 20U, words[0][20U + GFX_SIDE * 20U], 0U);
}

/*
 * g-mrt-blend and g-dualsrc: the first target holds the blended colour
 * (0.2, 0.4, 0.6, 1.0) everywhere; the second target, of a check of two
 * (push_word), keeps the clear colour, which its own equation (ZERO, ONE)
 * leaves.
 */
static void
gfx_check_mrt(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint32_t index;

	/* Each pixel of the first target, then of the second. */
	for (index = 0U; index < GFX_PIXELS; index++)
		oracle_expect(check, 0U, index, words[0][index], GFX_BLENDED_WORD);
	if (test->push_word < 2U)
		return;
	for (index = 0U; index < GFX_PIXELS; index++)
		oracle_expect(check, GFX_SECOND_TARGET, index, words[GFX_SECOND_TARGET][index], 0U);
}

/* g-fill: the fill's words 4 to 9, the update's 10 to 13, zeros 0 to 3 and 14 to 15, the second fill's 16 to 31. */
static void
gfx_check_fill(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint32_t expected;
	uint32_t index;

	UNUSED_PARAMETER(test);

	/* Each word of the buffer. */
	for (index = 0U; index < 32U; index++) {
		expected = 0U;
		if (index >= 4U && index < 10U)
			expected = 0xa5c3e17fU;
		if (index >= 10U && index < 14U)
			expected = 11U * (index - 9U);
		if (index >= 16U)
			expected = 0x01020304U;
		oracle_expect(check, 1U, index, words[1][index], expected);
	}

	/* Nothing was drawn. */
	for (index = 0U; index < GFX_PIXELS; index++)
		oracle_expect(check, 0U, index, words[0][index], 0U);
}

/*
 * g-timestamp: both timestamps available, the first nonzero and the second
 * not before it (one word: 1 when so), and the draw's colour 2.
 */
static void
gfx_check_timestamp(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint64_t first;
	uint64_t second;
	uint32_t ordered;
	uint32_t index;

	/* The availability words. */
	oracle_expect(check, 1U, 2U, words[1][2], 1U);
	oracle_expect(check, 1U, 3U, words[1][3], 0U);
	oracle_expect(check, 1U, 6U, words[1][6], 1U);
	oracle_expect(check, 1U, 7U, words[1][7], 0U);

	/* The two timestamps in order. */
	first = (uint64_t)words[1][0] | ((uint64_t)words[1][1] << 32);
	second = (uint64_t)words[1][4] | ((uint64_t)words[1][5] << 32);
	ordered = 0U;
	if (first != 0U && second >= first)
		ordered = 1U;
	oracle_expect(check, 1U, 16U, ordered, 1U);

	/* The draw. */
	for (index = 0U; index < GFX_PIXELS; index++)
		oracle_expect(check, 0U, index, words[0][index], test->push_word);
}

/*
 * g-occlusion and g-occlusion-mrt: the query available and its count, and
 * the draw's colours (2, or g-mrt-blend's two targets).  A precise count is
 * the target's pixels, or the pixels times the colour targets the shader
 * writes: Intel's Gen12 counts PS_DEPTH_COUNT once for each render-target
 * write message, and Mesa's anv and iris give the same (on the Latitude
 * 5330's Alder Lake, Debian's Mesa 25.2.6 anv counted 8192 for a 64 x 64
 * draw writing two targets, 4096 writing one of the two).
 * Without precise queries any count but 0 passes (word 16 is 1 when it is
 * nonzero).
 */
static void
gfx_check_occlusion(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint32_t expected;
	uint32_t targets;
	uint32_t index;

	/* The colour targets the draw's shader writes. */
	targets = 1U;
	if (test->options != NULL && test->options->color_count == 2U)
		targets = 2U;

	/* The availability, then the count: the pixels, or the pixels once for each target written, when precise. */
	oracle_expect(check, 1U, 2U, words[1][2], 1U);
	oracle_expect(check, 1U, 3U, words[1][3], 0U);
	if (gfx_occlusion_precise) {
		expected = GFX_PIXELS;
		if (words[1][0] == GFX_PIXELS * targets)
			expected = GFX_PIXELS * targets;
		oracle_expect(check, 1U, 0U, words[1][0], expected);
		oracle_expect(check, 1U, 1U, words[1][1], 0U);
	} else {
		oracle_expect(check, 1U, 16U, (words[1][0] | words[1][1]) != 0U ? 1U : 0U, 1U);
	}

	/* The draw: colour 2 everywhere, or the two targets of g-mrt-blend. */
	if (test->push_word == 2U) {
		gfx_check_mrt(check, test, words);
		return;
	}
	for (index = 0U; index < GFX_PIXELS; index++)
		oracle_expect(check, 0U, index, words[0][index], 2U);
}

/* g-fatomic: every pixel counted once, every column's bit set, colour 7. */
static void
gfx_check_fatomic(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint32_t index;

	UNUSED_PARAMETER(test);

	/* The count and the columns. */
	oracle_expect(check, 1U, 0U, words[1][0], GFX_PIXELS);
	oracle_expect(check, 1U, 1U, words[1][1], 0xffffffffU);
	oracle_expect(check, 1U, 2U, words[1][2], 0xffffffffU);

	/* The colour. */
	for (index = 0U; index < GFX_PIXELS; index++)
		oracle_expect(check, 0U, index, words[0][index], 7U);
}

/*
 * g-fdepthstore and g-fearlystore: the front draw (number 2) wins every
 * pixel.  With the depth test after the shader every draw counts every
 * pixel, the one in the middle drawn first and the one behind drawn last
 * too; with early fragment tests the one behind counts none.
 */
static void
gfx_check_fdepthstore(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint32_t index;
	uint32_t behind;

	/* No draw is numbered 0. */
	oracle_expect(check, 1U, 0U, words[1][0], 0U);

	/* The middle draw (only in g-fdepthstore) and the front one count every pixel. */
	if (test->draw_count == 3U)
		oracle_expect(check, 1U, 1U, words[1][1], GFX_PIXELS);
	else
		oracle_expect(check, 1U, 1U, words[1][1], 0U);
	oracle_expect(check, 1U, 2U, words[1][2], GFX_PIXELS);

	/* The draw behind: every pixel after its shader ran, none when the test came first. */
	behind = GFX_PIXELS;
	if (test->draw_count == 2U)
		behind = 0U;
	oracle_expect(check, 1U, 3U, words[1][3], behind);

	/* The colour of the front draw. */
	for (index = 0U; index < GFX_PIXELS; index++)
		oracle_expect(check, 0U, index, words[0][index], 2U);
}

/* g-desccopy: binding 1 holds the fourth buffer (the write that ran on), binding 2 the second (the copy). */
static void
gfx_check_desccopy(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint32_t expected;
	uint32_t index;

	UNUSED_PARAMETER(test);

	/* Each pixel. */
	expected = gfx_block_word(23U, 0U) ^ (gfx_block_word(21U, 0U) << 1);
	for (index = 0U; index < GFX_PIXELS; index++)
		oracle_expect(check, 0U, index, words[0][index], expected);
}

/* g-aniso: texel (x % 4, y % 4) at each pixel. */
static void
gfx_check_aniso(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint32_t x;
	uint32_t y;

	UNUSED_PARAMETER(test);

	/* Each pixel. */
	for (y = 0U; y < GFX_SIDE; y++) {
		for (x = 0U; x < GFX_SIDE; x++)
			oracle_expect(check, 0U, x + GFX_SIDE * y, words[0][x + GFX_SIDE * y], gfx_texel_word(x % 4U, y % 4U));
	}
}

/* g-cubearray: layer 6 (cube 1's +X) left of the middle, layer 2 (cube 0's +Y) from it on. */
static void
gfx_check_cubearray(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint32_t expected;
	uint32_t x;
	uint32_t y;

	UNUSED_PARAMETER(test);

	/* Each pixel. */
	for (y = 0U; y < GFX_SIDE; y++) {
		for (x = 0U; x < GFX_SIDE; x++) {
			expected = gfx_cube_word(6U);
			if (x >= GFX_SIDE / 2U)
				expected = gfx_cube_word(2U);
			oracle_expect(check, 0U, x + GFX_SIDE * y, words[0][x + GFX_SIDE * y], expected);
		}
	}
}

/* g-formats: each case's words as the table gives them, two texels' worth. */
static void
gfx_check_formats(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint32_t index;
	uint32_t word;
	uint32_t count;

	UNUSED_PARAMETER(test);

	/* Each case: four words for an eight-byte format, two for a four-byte one, one for a two-byte one. */
	for (index = 0U; index < GFX_FORMATS; index++) {
		count = 2U;
		if (gfx_format_cases[index].format == VK_FORMAT_R16G16B16A16_UNORM ||
		    gfx_format_cases[index].format == VK_FORMAT_R16G16B16A16_UINT)
			count = 4U;
		if (gfx_format_cases[index].format == VK_FORMAT_R16_SINT ||
		    gfx_format_cases[index].format == VK_FORMAT_R16_SFLOAT)
			count = 1U;
		for (word = 0U; word < count; word++)
			oracle_expect(check, 1U, 16U * index + word, words[1][16U * index + word], gfx_format_words[index][word]);
	}
}

/* g-blit3d: the two 3D images, 2 x 1 x 4 and 2 x 1 x 2, and the source's words in the buffer's words 8 to 15. */
static VkResult
gfx_prepare_blit3d(
	struct vkcompute *run,
	const struct gfx_test *test,
	struct gfx_objects *objects)
{
	VkImageCreateInfo image_info;
	uint32_t index;
	VkResult status;

	UNUSED_PARAMETER(test);

	/* The two images: four slices, then two. */
	for (index = 0U; index < 2U; index++) {
		memset(&image_info, 0, sizeof(image_info));
		image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		image_info.imageType = VK_IMAGE_TYPE_3D;
		image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
		image_info.extent.width = 2;
		image_info.extent.height = 1;
		image_info.extent.depth = 4U >> index;
		image_info.mipLevels = 1;
		image_info.arrayLayers = 1;
		image_info.samples = VK_SAMPLE_COUNT_1_BIT;
		image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
		image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		status = gfx_device_image(run, objects, &image_info, &objects->extra_images[index], &objects->extra_image_memories[index]);
		if (status != VK_SUCCESS)
			return status;
	}

	/* The source's eight texels, two to a slice. */
	for (index = 0U; index < 8U; index++)
		objects->words[1][8U + index] = gfx_cube_word(16U + index);

	/* Succeeded: the images and the words. */
	return VK_SUCCESS;
}

/* g-blit3d: uploads the source, blits its four slices onto the destination's two (nearest), and copies them back. */
static void
gfx_record_blit3d(
	struct vkcompute *run,
	const struct gfx_test *test,
	struct gfx_objects *objects)
{
	VkBufferImageCopy copy;
	VkImageBlit blit;

	UNUSED_PARAMETER(run);
	UNUSED_PARAMETER(test);

	/* The source, from the buffer's words 8 to 15. */
	gfx_image_barrier(objects->command, objects->extra_images[0], VK_IMAGE_ASPECT_COLOR_BIT, 1U,
			  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	memset(&copy, 0, sizeof(copy));
	copy.bufferOffset = 8U * 4U;
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 1;
	copy.imageExtent.width = 2;
	copy.imageExtent.height = 1;
	copy.imageExtent.depth = 4;
	vkCmdCopyBufferToImage(objects->command, objects->buffers[0], objects->extra_images[0],
			       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	gfx_image_barrier(objects->command, objects->extra_images[0], VK_IMAGE_ASPECT_COLOR_BIT, 1U,
			  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

	/* The blit of the whole source onto the whole destination, its depth halved. */
	gfx_image_barrier(objects->command, objects->extra_images[1], VK_IMAGE_ASPECT_COLOR_BIT, 1U,
			  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	memset(&blit, 0, sizeof(blit));
	blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.srcSubresource.layerCount = 1;
	blit.srcOffsets[1].x = 2;
	blit.srcOffsets[1].y = 1;
	blit.srcOffsets[1].z = 4;
	blit.dstSubresource = blit.srcSubresource;
	blit.dstOffsets[1].x = 2;
	blit.dstOffsets[1].y = 1;
	blit.dstOffsets[1].z = 2;
	vkCmdBlitImage(objects->command, objects->extra_images[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		       objects->extra_images[1], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

	/* The destination back into the buffer's words 0 to 3. */
	gfx_image_barrier(objects->command, objects->extra_images[1], VK_IMAGE_ASPECT_COLOR_BIT, 1U,
			  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	copy.bufferOffset = 0U;
	copy.imageExtent.depth = 2;
	vkCmdCopyImageToBuffer(objects->command, objects->extra_images[1], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			       objects->buffers[0], 1, &copy);
	gfx_memory_barrier(objects->command);
}

/* g-blit3d: the destination's slice 0 is the source's slice 1, its slice 1 the source's slice 3 (the slices under their centres). */
static void
gfx_check_blit3d(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	UNUSED_PARAMETER(test);

	/* The four texels. */
	oracle_expect(check, 1U, 0U, words[1][0], gfx_cube_word(16U + 2U));
	oracle_expect(check, 1U, 1U, words[1][1], gfx_cube_word(16U + 3U));
	oracle_expect(check, 1U, 2U, words[1][2], gfx_cube_word(16U + 6U));
	oracle_expect(check, 1U, 3U, words[1][3], gfx_cube_word(16U + 7U));
}

/*
 * g-sarray: elements 1 and 2 of the array, views of the 4 x 4 image whose
 * components are turned by one and by two places (element 0 is the
 * generic view, written by the generic code).
 */
static VkResult
gfx_prepare_sarray(
	struct vkcompute *run,
	const struct gfx_test *test,
	struct gfx_objects *objects)
{
	static const VkComponentSwizzle turned[4] = {
		VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_R
	};
	static const VkComponentSwizzle twice[4] = {
		VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G
	};
	VkImageViewCreateInfo view_info;
	VkDescriptorImageInfo infos[2];
	VkWriteDescriptorSet write;
	uint32_t index;
	VkResult status;

	UNUSED_PARAMETER(test);

	/* The two turned views. */
	for (index = 0U; index < 2U; index++) {
		memset(&view_info, 0, sizeof(view_info));
		view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view_info.image = objects->texture;
		view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
		if (index == 0U) {
			view_info.components.r = turned[0];
			view_info.components.g = turned[1];
			view_info.components.b = turned[2];
			view_info.components.a = turned[3];
		} else {
			view_info.components.r = twice[0];
			view_info.components.g = twice[1];
			view_info.components.b = twice[2];
			view_info.components.a = twice[3];
		}
		view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		view_info.subresourceRange.levelCount = 1;
		view_info.subresourceRange.layerCount = 1;
		objects->failed_call = "vkCreateImageView (array element)";
		status = vkCreateImageView(run->device, &view_info, NULL, &objects->extra_views[index]);
		if (status != VK_SUCCESS)
			return status;
		infos[index].sampler = objects->sampler;
		infos[index].imageView = objects->extra_views[index];
		infos[index].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	/* Elements 1 and 2 in one write. */
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = objects->set;
	write.dstBinding = 0;
	write.dstArrayElement = 1;
	write.descriptorCount = 2;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = infos;
	vkUpdateDescriptorSets(run->device, 1, &write, 0, NULL);

	/* Succeeded: the three elements. */
	return VK_SUCCESS;
}

/* g-uarray: elements 1 and 2 of the array, buffers of one word each (gfx_block_word(30 + n, 0)). */
static VkResult
gfx_prepare_uarray(
	struct vkcompute *run,
	const struct gfx_test *test,
	struct gfx_objects *objects)
{
	VkDescriptorBufferInfo infos[2];
	VkWriteDescriptorSet write;
	uint32_t index;
	VkResult status;
	void *mapped;

	UNUSED_PARAMETER(test);

	/* The two buffers. */
	for (index = 0U; index < 2U; index++) {
		status = gfx_host_buffer(run, objects, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 16U,
					 &objects->extra_buffers[index], &objects->extra_memories[index], &mapped);
		if (status != VK_SUCCESS)
			return status;
		memset(mapped, 0, 16U);
		*(uint32_t *)mapped = gfx_block_word(31U + index, 0U);
		infos[index].buffer = objects->extra_buffers[index];
		infos[index].offset = 0;
		infos[index].range = 16U;
	}

	/* Elements 1 and 2 in one write. */
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = objects->set;
	write.dstBinding = 1;
	write.dstArrayElement = 1;
	write.descriptorCount = 2;
	write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	write.pBufferInfo = infos;
	vkUpdateDescriptorSets(run->device, 1, &write, 0, NULL);

	/* Succeeded: the three elements. */
	return VK_SUCCESS;
}

/* A texel's packed bytes as a view turned by `element` places reads them: the bytes rotated down by 8 bits each place. */
static uint32_t
gfx_swizzled(
	uint32_t word,
	uint32_t element)
{
	/* No turn for element 0. */
	if (element == 0U)
		return word;

	/* Succeeded: red takes green, green blue, and so on, `element` times. */
	return (word >> (8U * element)) | (word << (32U - 8U * element));
}

/* g-sarray: texel (2, 1) through element x % 3's view. */
static void
gfx_check_sarray(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint32_t x;
	uint32_t y;

	UNUSED_PARAMETER(test);

	/* Each pixel. */
	for (y = 0U; y < GFX_SIDE; y++) {
		for (x = 0U; x < GFX_SIDE; x++)
			oracle_expect(check, 0U, x + GFX_SIDE * y, words[0][x + GFX_SIDE * y], gfx_swizzled(gfx_texel_word(2U, 1U), x % 3U));
	}
}

/* g-uarray: element x % 3's word: the generic buffer's (binding 1, word 0) for element 0, the check's own for the others. */
static void
gfx_check_uarray(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint32_t expected;
	uint32_t x;
	uint32_t y;

	UNUSED_PARAMETER(test);

	/* Each pixel. */
	for (y = 0U; y < GFX_SIDE; y++) {
		for (x = 0U; x < GFX_SIDE; x++) {
			expected = gfx_block_word(1U, 0U);
			if (x % 3U != 0U)
				expected = gfx_block_word(30U + x % 3U, 0U);
			oracle_expect(check, 0U, x + GFX_SIDE * y, words[0][x + GFX_SIDE * y], expected);
		}
	}
}

/* g-querylod: the computed level 2 (32 sixteenths) and the level read 0 (one level, maxLod 0) at every pixel. */
static void
gfx_check_querylod(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint32_t index;

	UNUSED_PARAMETER(test);

	/* Each pixel. */
	for (index = 0U; index < GFX_PIXELS; index++)
		oracle_expect(check, 0U, index, words[0][index], 32U);
}

/*
 * g-ginvoc: invocation i of the geometry shader covered quadrant i (left to
 * right, then top to bottom) with 0x1000 (i + 1), mixed with each pixel's
 * place.
 */
static void
gfx_check_ginvoc(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	uint32_t quadrant;
	uint32_t index;
	uint32_t x;
	uint32_t y;

	UNUSED_PARAMETER(test);

	/* Each pixel, by the quadrant it is in. */
	for (index = 0U; index < GFX_PIXELS; index++) {
		x = index % GFX_SIDE;
		y = index / GFX_SIDE;
		quadrant = 0U;
		if (x >= GFX_SIDE / 2U)
			quadrant += 1U;
		if (y >= GFX_SIDE / 2U)
			quadrant += 2U;
		oracle_expect(check, 0U, index, words[0][index], (0x1000U * (quadrant + 1U)) ^ index);
	}
}

/*
 * g-gpsize: the squares of the five points -- first pixel and side -- each
 * with 0x1000 (n + 1) mixed with each pixel's place, and the clear colour
 * 0 everywhere else.  A point of side s at (cx, cy) covers the pixels whose
 * centres are within s / 2 of it; none of the centres is on an edge.
 */
static void
gfx_check_gpsize(
	struct oracle_check *check,
	const struct gfx_test *test,
	uint32_t *const *words)
{
	static const uint32_t squares[5][3] = {
		{ 7U, 7U, 2U },
		{ 22U, 6U, 5U },
		{ 44U, 12U, 8U },
		{ 8U, 40U, 16U },
		{ 56U, 56U, 1U }
	};
	uint32_t expected;
	uint32_t point;
	uint32_t index;
	uint32_t x;
	uint32_t y;

	UNUSED_PARAMETER(test);

	/* Each pixel: the point whose square it is in, or none. */
	for (index = 0U; index < GFX_PIXELS; index++) {
		x = index % GFX_SIDE;
		y = index / GFX_SIDE;
		expected = 0U;
		for (point = 0U; point < 5U; point++) {
			if (x >= squares[point][0] && x < squares[point][0] + squares[point][2] &&
			    y >= squares[point][1] && y < squares[point][1] + squares[point][2])
				expected = (0x1000U * (point + 1U)) ^ index;
		}
		oracle_expect(check, 0U, index, words[0][index], expected);
	}
}
