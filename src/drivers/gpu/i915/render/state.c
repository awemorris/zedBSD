/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Gen12 3D state (see state.h).
 *
 * The command list is the one proven to draw on this hardware by the
 * fixture draws, with what a Vulkan draw adds to it: an enabled vertex
 * shader, vertex buffers, push constants, the viewport and scissor, the depth
 * buffer, SBE and a sampled texture.  The words that depend on the compiled
 * kernels are packed from what the compiler reports about them.
 */

#include "state.h"
#include "batch.h"
#include "gfx.h"
#include "heap.h"
#include "math.h"
#include <kern/kcrt.h>

#include <kern/klog.h>

#include <uapi/errno.h>
#include <stdint.h>

#include "../intel/commands.h"
#include "../intel/genxml.h"

/* The genxml SURFACE_FORMAT values of the VkFormats the render paths read and write. */
/*
 * The push constant space (32 KiB) of every draw: 8 KiB to the vertex
 * stage from 0, 8 KiB to the geometry stage from 8 KiB, 16 KiB to the pixel
 * stage from 16 KiB (ws075-p007b), the same whether or not the draw has a
 * geometry stage, so the allocation never changes between draws.  Each
 * stage's push data is at most I915_GFX_PUSH_DATA_BYTES.
 */
#define I915_GFX_PUSH_ALLOC_VS_KB	8U
#define I915_GFX_PUSH_ALLOC_GS_KB	8U
#define I915_GFX_PUSH_ALLOC_PS_KB	16U

#define I915_GFX_SURFACE_R8G8B8A8_UNORM		0x0c7U
#define I915_GFX_SURFACE_B8G8R8A8_UNORM		0x0c0U
#define I915_GFX_SURFACE_R8G8B8A8_UNORM_SRGB	0x0c8U
#define I915_GFX_SURFACE_B8G8R8A8_UNORM_SRGB	0x0c1U
#define I915_GFX_SURFACE_R8G8B8A8_SINT		0x0caU
#define I915_GFX_SURFACE_R8G8B8A8_UINT		0x0cbU
#define I915_GFX_SURFACE_R16_UNORM		0x10aU
#define I915_GFX_SURFACE_R8_UNORM		0x140U
#define I915_GFX_SURFACE_R8G8_UNORM		0x106U
#define I915_GFX_SURFACE_R16G16B16A16_FLOAT	0x084U
#define I915_GFX_SURFACE_R11G11B10_FLOAT	0x0d3U
#define I915_GFX_SURFACE_R32_FLOAT		0x0d8U
#define I915_GFX_SURFACE_R32G32_FLOAT		0x085U
#define I915_GFX_SURFACE_R32G32B32_FLOAT	0x040U
#define I915_GFX_SURFACE_R32G32B32A32_FLOAT	0x000U

/*
 * The 32-bit integer ones, for integer vertex attributes (Mesa 25.0.7
 * src/intel/isl/isl.h, enum isl_format, which is genxml's SURFACE_FORMAT).
 */
#define I915_GFX_SURFACE_R32_SINT		0x0d6U
#define I915_GFX_SURFACE_R32_UINT		0x0d7U
#define I915_GFX_SURFACE_R32G32_SINT		0x086U
#define I915_GFX_SURFACE_R32G32_UINT		0x087U
#define I915_GFX_SURFACE_R32G32B32_SINT		0x041U
#define I915_GFX_SURFACE_R32G32B32_UINT		0x042U
#define I915_GFX_SURFACE_R32G32B32A32_SINT	0x001U
#define I915_GFX_SURFACE_R32G32B32A32_UINT	0x002U

/* The attribute of a vertex kernel's input that the fetcher generates instead of reading (gl_VertexIndex, gl_InstanceIndex). */
#define I915_GFX_NO_ATTRIBUTE			0xffffffffU

/* SAMPLER_STATE texture coordinate mode for an address mode past the table: CLAMP. */
#define I915_GFX_SAMPLER_CLAMP			2U

/*
 * The hardware form of the colour blend of attachment 0.
 *
 * It is worked out from the pipeline for each draw, on the stack, and read
 * by BLEND_STATE and 3DSTATE_PS_BLEND, which must agree.
 */
struct i915_gfx_blend {
	/* Nonzero when the colour buffer blends. */
	uint32_t enable;

	/* The BLENDFACTORs of the colour and of the alpha. */
	uint32_t src_color;
	uint32_t dst_color;
	uint32_t src_alpha;
	uint32_t dst_alpha;

	/* The BLENDFUNCTIONs of the colour and of the alpha. */
	uint32_t color_function;
	uint32_t alpha_function;

	/* Nonzero when the alpha blends by other factors or another function than the colour. */
	uint32_t independent_alpha;
};

/*
 * One vertex attribute format the fetcher reads: the VkFormat, its genxml
 * SURFACE_FORMAT, how many components it has and whether they are integers
 * (a missing w is then the integer 1, not 1.0).
 */
struct i915_gfx_vertex_format {
	uint32_t vk_format;
	uint32_t surface_format;
	uint32_t components;
	uint32_t integer;
};

/*
 * The EU instructions of a thread that only ends itself.
 *
 * They are a null render target write with end-of-thread: a thread that
 * starts anywhere in the instruction heap other than at a kernel retires at
 * once instead of running whatever the heap held.  Every draw and every
 * rectangle fills the heap with them before placing its kernels.
 */
static const uint32_t i915_gfx_eot_only[4] = {
	0x00030032U, 0x00001004U, 0x58007024U, 0x00c40000U,
};

/*
 * SAMPLER_STATE texture coordinate modes, indexed by VkSamplerAddressMode:
 * REPEAT is WRAP, then MIRROR, CLAMP, CLAMP_BORDER and MIRROR_ONCE.
 */
static const uint32_t i915_gfx_address_modes[5] = {
	0U,
	1U,
	2U,
	4U,
	5U,
};

/*
 * The Shadow Function of each VkCompareOp, as anv takes it
 * (genX_init_state.c, vk_to_intel_shadow_compare_op): the hardware's
 * prefilter operation is the opposite of the comparison.
 */
static const uint32_t i915_gfx_shadow_functions[8] = {
	GEN12_PREFILTEROP_ALWAYS,
	GEN12_PREFILTEROP_LEQUAL,
	GEN12_PREFILTEROP_NOTEQUAL,
	GEN12_PREFILTEROP_LESS,
	GEN12_PREFILTEROP_GEQUAL,
	GEN12_PREFILTEROP_EQUAL,
	GEN12_PREFILTEROP_GREATER,
	GEN12_PREFILTEROP_NEVER,
};

/*
 * The genxml COMPAREFUNCTION of each VkCompareOp: NEVER .. GREATER_OR_EQUAL
 * shift up by one and ALWAYS is zero.
 */
static const uint32_t i915_gfx_compare_functions[8] = {
	1U,
	2U,
	3U,
	4U,
	5U,
	6U,
	7U,
	0U,
};

/* The 3DSTATE_CONSTANT_* packets of the five stages, the vertex stage first. */
static const uint32_t i915_gfx_constant_opcodes[5] = {
	GEN12_CMD_3DSTATE_CONSTANT_VS,
	GEN12_CMD_3DSTATE_CONSTANT_HS,
	GEN12_CMD_3DSTATE_CONSTANT_DS,
	GEN12_CMD_3DSTATE_CONSTANT_GS,
	GEN12_CMD_3DSTATE_CONSTANT_PS,
};

/*
 * The BLENDFACTOR of each VkBlendFactor, as anv maps them
 * (genX_gfx_state.c vk_to_intel_blend): ZERO, ONE, SRC_COLOR,
 * ONE_MINUS_SRC_COLOR, DST_COLOR, ONE_MINUS_DST_COLOR, SRC_ALPHA,
 * ONE_MINUS_SRC_ALPHA, DST_ALPHA, ONE_MINUS_DST_ALPHA, CONSTANT_COLOR,
 * ONE_MINUS_CONSTANT_COLOR, CONSTANT_ALPHA, ONE_MINUS_CONSTANT_ALPHA,
 * SRC_ALPHA_SATURATE, SRC1_COLOR, ONE_MINUS_SRC1_COLOR, SRC1_ALPHA and
 * ONE_MINUS_SRC1_ALPHA.
 */
static const uint32_t i915_gfx_blend_factors[19] = {
	GEN12_BLENDFACTOR_ZERO,
	GEN12_BLENDFACTOR_ONE,
	GEN12_BLENDFACTOR_SRC_COLOR,
	GEN12_BLENDFACTOR_INV_SRC_COLOR,
	GEN12_BLENDFACTOR_DST_COLOR,
	GEN12_BLENDFACTOR_INV_DST_COLOR,
	GEN12_BLENDFACTOR_SRC_ALPHA,
	GEN12_BLENDFACTOR_INV_SRC_ALPHA,
	GEN12_BLENDFACTOR_DST_ALPHA,
	GEN12_BLENDFACTOR_INV_DST_ALPHA,
	GEN12_BLENDFACTOR_CONST_COLOR,
	GEN12_BLENDFACTOR_INV_CONST_COLOR,
	GEN12_BLENDFACTOR_CONST_ALPHA,
	GEN12_BLENDFACTOR_INV_CONST_ALPHA,
	GEN12_BLENDFACTOR_SRC_ALPHA_SATURATE,
	GEN12_BLENDFACTOR_SRC1_COLOR,
	GEN12_BLENDFACTOR_INV_SRC1_COLOR,
	GEN12_BLENDFACTOR_SRC1_ALPHA,
	GEN12_BLENDFACTOR_INV_SRC1_ALPHA,
};

/* The BLENDFUNCTION of each VkBlendOp: ADD, SUBTRACT, REVERSE_SUBTRACT, MIN and MAX. */
static const uint32_t i915_gfx_blend_functions[5] = {
	GEN12_BLENDFUNCTION_ADD,
	GEN12_BLENDFUNCTION_SUBTRACT,
	GEN12_BLENDFUNCTION_REVERSE_SUBTRACT,
	GEN12_BLENDFUNCTION_MIN,
	GEN12_BLENDFUNCTION_MAX,
};

/*
 * The vertex formats the draw takes: the 32-bit floats and integers, and the
 * 8-, 16- and 10-bit ones libGLESv2 hands the executor (normalized, scaled
 * and integer; 16-bit floats).  The SURFACE_FORMAT values are Mesa 25.0.7's
 * enum isl_format (src/intel/isl/isl.h, sha256
 * 71099bdc5b8657541525bdab4824d43f20ad169f8d7570b60ba6b251fdb6431a); Vulkan's
 * A2B10G10R10 packs red in the low bits, isl's R10G10B10A2 (anv's mapping).
 * The table is constant and lives for the kernel's lifetime.
 */
static const struct i915_gfx_vertex_format i915_gfx_vertex_formats[] = {
	{ VK_FORMAT_R32_SFLOAT, 0x0d8U, 1U, 0U },
	{ VK_FORMAT_R32G32_SFLOAT, 0x085U, 2U, 0U },
	{ VK_FORMAT_R32G32B32_SFLOAT, 0x040U, 3U, 0U },
	{ VK_FORMAT_R32G32B32A32_SFLOAT, 0x000U, 4U, 0U },
	{ VK_FORMAT_R32_SINT, 0x0d6U, 1U, 1U },
	{ VK_FORMAT_R32_UINT, 0x0d7U, 1U, 1U },
	{ VK_FORMAT_R32G32_SINT, 0x086U, 2U, 1U },
	{ VK_FORMAT_R32G32_UINT, 0x087U, 2U, 1U },
	{ VK_FORMAT_R32G32B32_SINT, 0x041U, 3U, 1U },
	{ VK_FORMAT_R32G32B32_UINT, 0x042U, 3U, 1U },
	{ VK_FORMAT_R32G32B32A32_SINT, 0x001U, 4U, 1U },
	{ VK_FORMAT_R32G32B32A32_UINT, 0x002U, 4U, 1U },
	{ VK_FORMAT_B8G8R8A8_UNORM, 0x0c0U, 4U, 0U },
	{ VK_FORMAT_R8_UNORM, 320U, 1U, 0U },
	{ VK_FORMAT_R8_SNORM, 321U, 1U, 0U },
	{ VK_FORMAT_R8_SINT, 322U, 1U, 1U },
	{ VK_FORMAT_R8_UINT, 323U, 1U, 1U },
	{ VK_FORMAT_R8_SSCALED, 329U, 1U, 0U },
	{ VK_FORMAT_R8_USCALED, 330U, 1U, 0U },
	{ VK_FORMAT_R8G8_UNORM, 262U, 2U, 0U },
	{ VK_FORMAT_R8G8_SNORM, 263U, 2U, 0U },
	{ VK_FORMAT_R8G8_SINT, 264U, 2U, 1U },
	{ VK_FORMAT_R8G8_UINT, 265U, 2U, 1U },
	{ VK_FORMAT_R8G8_SSCALED, 284U, 2U, 0U },
	{ VK_FORMAT_R8G8_USCALED, 285U, 2U, 0U },
	{ VK_FORMAT_R8G8B8_UNORM, 403U, 3U, 0U },
	{ VK_FORMAT_R8G8B8_SNORM, 404U, 3U, 0U },
	{ VK_FORMAT_R8G8B8_SSCALED, 405U, 3U, 0U },
	{ VK_FORMAT_R8G8B8_USCALED, 406U, 3U, 0U },
	{ VK_FORMAT_R8G8B8_UINT, 456U, 3U, 1U },
	{ VK_FORMAT_R8G8B8_SINT, 457U, 3U, 1U },
	{ VK_FORMAT_R8G8B8A8_UNORM, 199U, 4U, 0U },
	{ VK_FORMAT_R8G8B8A8_SNORM, 201U, 4U, 0U },
	{ VK_FORMAT_R8G8B8A8_SINT, 202U, 4U, 1U },
	{ VK_FORMAT_R8G8B8A8_UINT, 203U, 4U, 1U },
	{ VK_FORMAT_R8G8B8A8_SSCALED, 244U, 4U, 0U },
	{ VK_FORMAT_R8G8B8A8_USCALED, 245U, 4U, 0U },
	{ VK_FORMAT_R16_UNORM, 266U, 1U, 0U },
	{ VK_FORMAT_R16_SNORM, 267U, 1U, 0U },
	{ VK_FORMAT_R16_SINT, 268U, 1U, 1U },
	{ VK_FORMAT_R16_UINT, 269U, 1U, 1U },
	{ VK_FORMAT_R16_SFLOAT, 270U, 1U, 0U },
	{ VK_FORMAT_R16_SSCALED, 286U, 1U, 0U },
	{ VK_FORMAT_R16_USCALED, 287U, 1U, 0U },
	{ VK_FORMAT_R16G16_UNORM, 204U, 2U, 0U },
	{ VK_FORMAT_R16G16_SNORM, 205U, 2U, 0U },
	{ VK_FORMAT_R16G16_SINT, 206U, 2U, 1U },
	{ VK_FORMAT_R16G16_UINT, 207U, 2U, 1U },
	{ VK_FORMAT_R16G16_SFLOAT, 208U, 2U, 0U },
	{ VK_FORMAT_R16G16_SSCALED, 246U, 2U, 0U },
	{ VK_FORMAT_R16G16_USCALED, 247U, 2U, 0U },
	{ VK_FORMAT_R16G16B16_SFLOAT, 411U, 3U, 0U },
	{ VK_FORMAT_R16G16B16_UNORM, 412U, 3U, 0U },
	{ VK_FORMAT_R16G16B16_SNORM, 413U, 3U, 0U },
	{ VK_FORMAT_R16G16B16_SSCALED, 414U, 3U, 0U },
	{ VK_FORMAT_R16G16B16_USCALED, 415U, 3U, 0U },
	{ VK_FORMAT_R16G16B16_UINT, 432U, 3U, 1U },
	{ VK_FORMAT_R16G16B16_SINT, 433U, 3U, 1U },
	{ VK_FORMAT_R16G16B16A16_UNORM, 128U, 4U, 0U },
	{ VK_FORMAT_R16G16B16A16_SNORM, 129U, 4U, 0U },
	{ VK_FORMAT_R16G16B16A16_SINT, 130U, 4U, 1U },
	{ VK_FORMAT_R16G16B16A16_UINT, 131U, 4U, 1U },
	{ VK_FORMAT_R16G16B16A16_SFLOAT, 132U, 4U, 0U },
	{ VK_FORMAT_R16G16B16A16_SSCALED, 147U, 4U, 0U },
	{ VK_FORMAT_R16G16B16A16_USCALED, 148U, 4U, 0U },
	{ VK_FORMAT_A2B10G10R10_UNORM_PACK32, 194U, 4U, 0U },
	{ VK_FORMAT_A2B10G10R10_UINT_PACK32, 196U, 4U, 1U },
	{ VK_FORMAT_A2B10G10R10_SNORM_PACK32, 435U, 4U, 0U },
	{ VK_FORMAT_A2B10G10R10_USCALED_PACK32, 436U, 4U, 0U },
	{ VK_FORMAT_A2B10G10R10_SSCALED_PACK32, 437U, 4U, 0U },
};

static int i915_surface_format(uint32_t format, uint32_t *surface_format);
static int i915_vertex_format(uint32_t format, const struct i915_gfx_vertex_format **entry);
/*
 * What of an image a surface state shows: the view's type, its levels and
 * its layers (a 3D image's slices for a render target), the VkFormat its
 * texels are read as (0: the image's; a colour view may reinterpret them,
 * ws031-p033), the view's channel select when swizzle_set, and whether it
 * is the render target, which writes its first level and layer.
 */
struct i915_image_range {
	uint32_t view_type;
	uint32_t base_level;
	uint32_t level_count;
	uint32_t base_layer;
	uint32_t layer_count;
	uint32_t format;
	uint32_t channel_select;
	uint32_t swizzle_set;
	int render_target;
};

static int i915_image_surface_write(uint32_t *rss, const struct i915_gfx_image *image, const struct i915_image_range *range, uint32_t mocs);
static int i915_buffer_surface_write(uint32_t *rss, const struct i915_gfx_buffer_view *view, uint32_t mocs);
static void i915_state_target_range(const struct i915_gfx_draw_state *state, uint32_t slot, struct i915_image_range *range);
static const struct i915_gfx_view *i915_state_target_view(const struct i915_gfx_draw_state *state, uint32_t slot);
static int i915_state_target_integer(const struct i915_gfx_draw_state *state, uint32_t slot);
static uint32_t i915_blend_write_disables(uint32_t disable);
static void i915_state_stencil(const struct i915_gfx_draw_state *state, uint32_t *depth_state, uint32_t *masks, uint32_t *references);
static int i915_state_stencil_buffer(struct i915_gfx_batch *batch, const struct i915_gfx_draw_state *state, const struct i915_gfx_image *image, uint32_t mocs);
static void i915_state_null_surface_write(uint32_t *rss, uint32_t width, uint32_t height);
static int i915_state_write_targets(uint32_t *surface, const struct i915_gfx_draw_state *state, const struct i915_gfx_image *target, uint32_t mocs);
static uint32_t i915_state_dynamic_offset(const struct i915_gfx_draw_state *state, uint32_t set, uint32_t binding);
static uint32_t i915_sampler_mip_filter(uint32_t mipmap_mode);
static int i915_state_write_surfaces(uint32_t *surface, uint32_t *dynamic, const struct i915_gfx_draw_state *state, const struct i915_gfx_kernels *kernels, const struct i915_gfx_image *target, uint32_t mocs);
static int i915_state_viewport_source(const struct i915_gfx_draw_state *state, const uint32_t **viewport, const VkRect2D **scissor);
static void i915_state_write_viewport(uint32_t *dynamic, const uint32_t *viewport, const VkRect2D *scissor);
static void i915_state_write_blend(uint32_t *dynamic, const struct i915_gfx_draw_state *state, const struct i915_gfx_image *target);
static uint32_t i915_state_target_format(const struct i915_gfx_draw_state *state, uint32_t slot, const struct i915_gfx_image *target);
static int i915_state_format_takes_logic_op(uint32_t format);
static uint32_t i915_blend_logic_op(uint32_t op);
static int i915_state_write_push(uint8_t *data, const struct i915_gfx_draw_state *state, const struct i915_gfx_push_layout *layout);
static void i915_blend_equation(const struct i915_gfx_pipeline *pipeline, struct i915_gfx_blend *equation);
static uint32_t i915_blend_factor(uint32_t factor);
static uint32_t i915_blend_function(uint32_t op);
static int i915_blend_uses_second_source(uint32_t factor);
static uint32_t i915_state_input_slot(const struct i915_gfx_kernels *kernels, uint32_t inputs, uint32_t input);
static uint64_t i915_state_scratch(uint32_t per_thread_bytes, uint64_t offset);
static uint32_t i915_state_binding_instanced(const struct i915_gfx_pipeline *pipeline, uint32_t binding);

/*
 * Reports whether the vertex fetcher reads a format.
 *
 * The device's format properties claim the vertex buffer feature for these
 * formats, so a client binds its buffer objects as they are instead of
 * copying the vertices out of its own bytes first.  Returns 1 for a format
 * in the fetcher's table and 0 for any other.
 */
int
drv_i915_gfx_vertex_format_supported(
	uint32_t format)
{
	const struct i915_gfx_vertex_format *entry;
	int error;

	/* Looks the format up in the fetcher's table. */
	error = i915_vertex_format(format, &entry);
	if (error != 0)
		return 0;

	/* Succeeded: the fetcher reads the format. */
	return 1;
}

/*
 * Finds the surface format and the texel size of a texel buffer's format.
 *
 * A uniform texel buffer is a buffer surface the sampler reads with the ld
 * message; its formats are the ones the surface state names whose texel
 * size is known here.  Returns 0, or ENOTSUP for any other format.
 */
int
drv_i915_gfx_texel_buffer_format(
	uint32_t format,
	uint32_t *surface_format,
	uint32_t *bytes)
{
	int error;

	/* The texel's size; a format of another size is not a texel buffer's here. */
	switch (format) {
	case VK_FORMAT_R8_UNORM:
		*bytes = 1U;
		break;
	case VK_FORMAT_R8G8_UNORM:
		*bytes = 2U;
		break;
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SINT:
	case VK_FORMAT_R8G8B8A8_UINT:
	case VK_FORMAT_R32_SFLOAT:
	case VK_FORMAT_R32_SINT:
	case VK_FORMAT_R32_UINT:
		*bytes = 4U;
		break;
	case VK_FORMAT_R16G16B16A16_SFLOAT:
	case VK_FORMAT_R32G32_SFLOAT:
	case VK_FORMAT_R32G32_SINT:
	case VK_FORMAT_R32G32_UINT:
		*bytes = 8U;
		break;
	case VK_FORMAT_R32G32B32_SFLOAT:
	case VK_FORMAT_R32G32B32_SINT:
	case VK_FORMAT_R32G32B32_UINT:
		*bytes = 12U;
		break;
	case VK_FORMAT_R32G32B32A32_SFLOAT:
	case VK_FORMAT_R32G32B32A32_SINT:
	case VK_FORMAT_R32G32B32A32_UINT:
		*bytes = 16U;
		break;
	default:
		return ENOTSUP;
	}

	/* The surface format the surface state names it by. */
	error = i915_surface_format(format, surface_format);
	if (error != 0)
		return ENOTSUP;

	/* Succeeded: the format is a texel buffer's. */
	return 0;
}

/*
 * Writes everything a draw's batch points at into its slot of the state
 * object.
 *
 * The surface and dynamic heaps are cleared and refilled: the binding table
 * with the render target and the sampled textures, the samplers, blend,
 * viewports and scissor, then each stage's push data.  The kernels are not
 * written here: they wait in the pipeline's instruction window.  The
 * viewport and the scissor are the pipeline's,
 * or the ones the command buffer set when the pipeline declares them
 * dynamic.  Returns EINVAL or ENOTSUP for a draw whose bindings cannot be
 * written; the object is then partly written.
 */
int
drv_i915_gfx_write_state(
	uint8_t *page,
	const struct i915_gfx_draw_state *state,
	const struct i915_gfx_kernels *kernels,
	const struct i915_gfx_image *target,
	uint32_t mocs)
{
	const uint32_t *viewport;
	const VkRect2D *scissor;
	uint32_t *surface;
	uint32_t *dynamic;
	int error;

	/* Locates the two heaps and clears the whole slot. */
	surface = (uint32_t *)(void *)(page + I915_GFX_SURFACE_HEAP);
	dynamic = (uint32_t *)(void *)(page + I915_GFX_DYNAMIC_HEAP);
	kern_memset(page, 0, I915_GFX_SLOT_BYTES);

	/* Writes the binding table, the surfaces and the samplers. */
	error = i915_state_write_surfaces(surface, dynamic, state, kernels, target, mocs);
	if (error != 0)
		return error;

	/* Chooses the viewport and the scissor the draw uses: the pipeline's or the dynamic ones. */
	error = i915_state_viewport_source(state, &viewport, &scissor);
	if (error != 0)
		return error;

	/* Writes the viewports and the scissor, then the blend state and its constants. */
	i915_state_write_viewport(dynamic, viewport, scissor);
	i915_state_write_blend(dynamic, state, target);

	/* Fills the vertex stage's push data. */
	error = i915_state_write_push(page + I915_GFX_PUSH_BUFFER, state, &kernels->vs_push);
	if (error != 0)
		return error;

	/* Fills the pixel stage's push data. */
	error = i915_state_write_push(page + I915_GFX_PS_PUSH_BUFFER, state, &kernels->ps_push);
	if (error != 0)
		return error;

	/* Fills the geometry stage's push data, when the draw has a geometry kernel (an empty layout writes nothing). */
	error = i915_state_write_push(page + I915_GFX_GS_PUSH_BUFFER, state, &kernels->gs_push);
	if (error != 0)
		return error;

	/* Succeeded: the state object holds everything the batch points at. */
	return 0;
}

/*
 * Fills the push data of one kernel from the bound state: the push
 * constants, the uniform blocks' ranges and the storage buffers' addresses
 * (see i915_state_write_push()).
 *
 * A dispatch fills its cross-thread data with it (ws101-p004), with the
 * compute bind point's sets in place of the graphics ones.
 */
int
drv_i915_gfx_write_push(
	uint8_t *data,
	const struct i915_gfx_draw_state *state,
	const struct i915_gfx_push_layout *layout)
{
	int error;

	/* Fills the push data as a draw's stage does. */
	error = i915_state_write_push(data, state, layout);
	if (error != 0)
		return error;

	/* Succeeded: the push data is in place. */
	return 0;
}

/*
 * Writes the RENDER_SURFACE_STATE of a 2D one-level surface.
 *
 * The surface is checked first: a GPU address, an extent of at most 16384
 * in each direction and a pitch that holds a row of its texels.  An
 * R32_FLOAT surface is written without the unorm path bit, since it carries
 * a depth value's bits rather than a colour.  A depth surface (D32_SFLOAT,
 * D16_UNORM) is the Y-tiled R32_FLOAT or R16_UNORM it is laid out as (see
 * i915_image_surface_write()); a colour surface is linear unless it is
 * a sample of a multisampled image (surface->tiled).
 */
int
drv_i915_gfx_surface_write(
	uint32_t *rss,
	const struct i915_gfx_surface *surface,
	uint32_t mocs)
{
	uint32_t texel_bytes;
	uint32_t format;
	uint32_t unorm;
	uint32_t tile;
	int error;

	/* Refuses a surface with no storage. */
	if (surface->va == 0U)
		return EINVAL;

	/* Refuses an empty surface. */
	if (surface->width == 0U || surface->height == 0U)
		return EINVAL;

	/* Refuses a surface larger than a 2D surface state describes. */
	if (surface->width > 16384U || surface->height > 16384U)
		return EINVAL;

	/* Names the format and the tiling: a depth surface is Y-tiled, a 16-bit one two bytes a texel. */
	texel_bytes = 4U;
	tile = GEN12_TILEMODE_LINEAR;
	unorm = 1U << 31;
	if (surface->format == VK_FORMAT_D32_SFLOAT || surface->format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
		format = I915_GFX_SURFACE_R32_FLOAT;
		tile = GEN12_TILEMODE_YMAJOR;
		unorm = 0U;
	} else if (surface->format == VK_FORMAT_S8_UINT) {
		/* A stencil plane: its bytes as Y-tiled R8_UNORM (a clear writes value / 255). */
		format = I915_GFX_SURFACE_R8_UNORM;
		tile = GEN12_TILEMODE_YMAJOR;
		texel_bytes = 1U;
	} else if (surface->format == VK_FORMAT_D16_UNORM) {
		format = I915_GFX_SURFACE_R16_UNORM;
		tile = GEN12_TILEMODE_YMAJOR;
		texel_bytes = 2U;
	} else if (surface->format == VK_FORMAT_R16_UNORM) {
		format = I915_GFX_SURFACE_R16_UNORM;
		texel_bytes = 2U;
	} else {
		/* Refuses a format the surface state cannot name. */
		error = i915_surface_format(surface->format, &format);
		if (error != 0)
			return EINVAL;

		/* An image format's own texel; a vertex-only format counts as four bytes. */
		texel_bytes = drv_i915_gfx_format_bytes(surface->format);
		if (texel_bytes == 0U)
			texel_bytes = 4U;

		/* The depth-bits view carries no colour, so no unorm path. */
		if (surface->format == VK_FORMAT_R32_SFLOAT)
			unorm = 0U;

		/* A sample of a multisampled image is in Y tiles. */
		if (surface->tiled != 0U)
			tile = GEN12_TILEMODE_YMAJOR;
	}

	/* Refuses a pitch too short for a row of texels, and a Y-tiled surface of partial tiles. */
	if (surface->pitch < surface->width * texel_bytes)
		return EINVAL;
	if (tile == GEN12_TILEMODE_YMAJOR && ((surface->pitch & 127U) != 0U || (surface->va & 4095U) != 0U))
		return EINVAL;

	/*
	 * Fills the surface state as isl fills it for a 2D one-level surface:
	 * 2D, horizontal and vertical alignment 4, the tiling; the unorm path
	 * bit, MOCS and QPitch; width and height; pitch; mip tail start 1;
	 * identity channel select; the address.
	 */
	kern_memset(rss, 0, GEN12_RENDER_SURFACE_STATE_DWORDS * 4U);
	rss[0] = (GEN12_SURFTYPE_2D << 29) |
	    (format << 18) |
	    (GEN12_SURFACE_ALIGN_4 << 16) |
	    (GEN12_SURFACE_ALIGN_4 << 14) |
	    (tile << 12);
	rss[1] = unorm | (mocs << 24) | (((surface->height + 3U) & ~3U) / 4U);
	rss[2] = (surface->width - 1U) | ((surface->height - 1U) << 16);
	rss[3] = surface->pitch - 1U;
	rss[5] = 0x00000100U;
	rss[7] = (4U << 25) | (5U << 22) | (6U << 19) | (7U << 16);
	rss[8] = (uint32_t)surface->va;
	rss[9] = (uint32_t)(surface->va >> 32);

	/* Succeeded: the surface state describes the surface. */
	return 0;
}

/*
 * Writes the SAMPLER_STATE of a sampler.
 *
 * The fields are the ones anv fills (genX_init_state.c): the OpenGL LOD
 * pre-clamp, the mip filter of the mipmap mode, the two filters (both
 * anisotropic with anisotropy, the EWA algorithm) and the LOD bias; the LOD
 * range, the cube override (seamless cube maps) and the shadow function of
 * a depth comparison; address rounding for a linear filter, the maximum
 * anisotropy, unnormalized coordinates and the three address modes.  The
 * bias is clamped to [-16, 15.996] and both LOD limits to [0, 14], as anv
 * clamps them; the surface state then limits the levels to the ones the
 * view has.  An address mode past the table clamps.  The border colour
 * pointer is drv_i915_gfx_sampler_border_write()'s.
 */
void
drv_i915_gfx_sampler_write(
	uint32_t *state,
	const struct i915_gfx_sampler *sampler)
{
	uint32_t address_u;
	uint32_t address_v;
	uint32_t address_w;
	uint32_t mag_linear;
	uint32_t min_linear;
	uint32_t mip_filter;
	uint32_t rounding;
	uint32_t shadow;
	uint32_t anisotropy;
	uint32_t ratio;
	uint32_t extra;
	int32_t lod_bias;
	int32_t min_lod;
	int32_t max_lod;

	/* Translates the u address mode, clamping one the table does not know. */
	address_u = I915_GFX_SAMPLER_CLAMP;
	if (sampler->address_u < 5U)
		address_u = i915_gfx_address_modes[sampler->address_u];

	/* Translates the v address mode the same way. */
	address_v = I915_GFX_SAMPLER_CLAMP;
	if (sampler->address_v < 5U)
		address_v = i915_gfx_address_modes[sampler->address_v];

	/* And the w address mode. */
	address_w = I915_GFX_SAMPLER_CLAMP;
	if (sampler->address_w < 5U)
		address_w = i915_gfx_address_modes[sampler->address_w];

	/* The shadow function of a comparison; without one, NEVER's (the hardware's ALWAYS). */
	shadow = GEN12_PREFILTEROP_ALWAYS;
	if (sampler->compare_enable != 0U && sampler->compare_op < 8U)
		shadow = i915_gfx_shadow_functions[sampler->compare_op];

	/* The maximum anisotropy as anv rounds it: (ratio - 2) / 2, 0 .. 7, from a ratio of 2 .. 16. */
	anisotropy = 0U;
	if (sampler->anisotropy_enable != 0U) {
		ratio = (uint32_t)drv_i915_float_to_fixed(sampler->max_anisotropy, 0U, 2, 16);
		anisotropy = (ratio - 2U) / 2U;
	}

	/* Notes which of the two filters is linear. */
	mag_linear = 0U;
	if (sampler->mag_filter == VK_FILTER_LINEAR)
		mag_linear = 1U;
	min_linear = 0U;
	if (sampler->min_filter == VK_FILTER_LINEAR)
		min_linear = 1U;

	/* A linear filter in either direction turns address rounding on. */
	rounding = 0U;
	if (mag_linear != 0U || min_linear != 0U)
		rounding = 0x0007e000U;

	/* Anisotropy filters both ways anisotropically. */
	if (sampler->anisotropy_enable != 0U) {
		mag_linear = GEN12_MAPFILTER_ANISOTROPIC;
		min_linear = GEN12_MAPFILTER_ANISOTROPIC;
		rounding = 0x0007e000U;
	}

	/* Chooses how levels are picked and blended. */
	mip_filter = i915_sampler_mip_filter(sampler->mipmap_mode);

	/* Converts the bias and the LOD range to the sampler's fixed point, clamped as anv clamps them. */
	lod_bias = drv_i915_float_to_fixed(sampler->lod_bias,
					   GEN12_SAMPLER_LOD_FRACTION_BITS,
					   GEN12_SAMPLER_LOD_BIAS_MIN,
					   GEN12_SAMPLER_LOD_BIAS_MAX);
	min_lod = drv_i915_float_to_fixed(sampler->min_lod,
					  GEN12_SAMPLER_LOD_FRACTION_BITS,
					  0,
					  GEN12_SAMPLER_LOD_MAX);
	max_lod = drv_i915_float_to_fixed(sampler->max_lod,
					  GEN12_SAMPLER_LOD_FRACTION_BITS,
					  0,
					  GEN12_SAMPLER_LOD_MAX);

	/*
	 * Packs the sampler: the OpenGL LOD pre-clamp, the mip filter, the
	 * filters and the bias (13-bit two's complement); the LOD range; the
	 * address rounding and the address modes, w clamped.
	 */
	state[0] = (GEN12_CLAMP_MODE_OGL << GEN12_SAMPLER_LOD_PRECLAMP_SHIFT) |
	    (mip_filter << GEN12_SAMPLER_MIP_FILTER_SHIFT) |
	    (mag_linear << GEN12_SAMPLER_MAG_FILTER_SHIFT) |
	    (min_linear << GEN12_SAMPLER_MIN_FILTER_SHIFT) |
	    (((uint32_t)lod_bias & GEN12_SAMPLER_LOD_BIAS_MASK) << GEN12_SAMPLER_LOD_BIAS_SHIFT);
	if (sampler->anisotropy_enable != 0U)
		state[0] |= GEN12_SAMPLER_ANISOTROPIC_EWA;
	state[1] = ((uint32_t)max_lod << GEN12_SAMPLER_MAX_LOD_SHIFT) |
	    ((uint32_t)min_lod << GEN12_SAMPLER_MIN_LOD_SHIFT) |
	    (shadow << GEN12_SAMPLER_SHADOW_SHIFT) |
	    GEN12_SAMPLER_CUBE_OVERRIDE;
	state[2] = 0U;
	extra = anisotropy << GEN12_SAMPLER_MAX_ANISOTROPY_SHIFT;
	if (sampler->unnormalized != 0U)
		extra |= GEN12_SAMPLER_NON_NORMALIZED;
	state[3] = rounding | extra |
	    (address_u << GEN12_SAMPLER_TCX_SHIFT) |
	    (address_v << GEN12_SAMPLER_TCY_SHIFT) |
	    (address_w << GEN12_SAMPLER_TCZ_SHIFT);
}

/*
 * Writes a sampler's SAMPLER_BORDER_COLOR_STATE at `border`, `offset`
 * bytes into the dynamic state heap (64-byte aligned), and points the
 * SAMPLER_STATE at it: the four channels of the VkBorderColor, floats for
 * a FLOAT colour and integers for an INT one, as anv fills its border
 * colours (anv_device.c, the border colour pool).
 */
void
drv_i915_gfx_sampler_border_write(
	uint32_t *state,
	uint32_t *border,
	uint32_t offset,
	const struct i915_gfx_sampler *sampler)
{
	uint32_t one;
	uint32_t alpha;
	uint32_t colour;

	/* One is 1.0 for a float colour and 1 for an integer one. */
	one = 0x3f800000U;
	if (sampler->border_color == VK_BORDER_COLOR_INT_TRANSPARENT_BLACK ||
	    sampler->border_color == VK_BORDER_COLOR_INT_OPAQUE_BLACK ||
	    sampler->border_color == VK_BORDER_COLOR_INT_OPAQUE_WHITE)
		one = 1U;

	/* Transparent black is all zero; opaque black has alpha one; opaque white is all one. */
	alpha = 0U;
	colour = 0U;
	if (sampler->border_color == VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK ||
	    sampler->border_color == VK_BORDER_COLOR_INT_OPAQUE_BLACK) {
		alpha = one;
	} else if (sampler->border_color == VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE ||
		   sampler->border_color == VK_BORDER_COLOR_INT_OPAQUE_WHITE) {
		alpha = one;
		colour = one;
	}

	/* Writes the four channels and points the sampler at them. */
	kern_memset(border, 0, 64U);
	border[0] = colour;
	border[1] = colour;
	border[2] = colour;
	border[3] = alpha;
	state[2] = offset & GEN12_SAMPLER_BORDER_POINTER_MASK;
}

/*
 * Fills one instruction window with threads that only end themselves.
 *
 * A kernel copied in afterwards replaces the fill at its own offset; a
 * thread that starts anywhere else retires at once.
 */
void
drv_i915_gfx_instruction_heap_clear(
	uint8_t *window)
{
	unsigned at;

	/* Repeats the end-of-thread instructions over the whole window. */
	for (at = 0U; at + sizeof(i915_gfx_eot_only) <= I915_GFX_INSTRUCTION_BYTES; at += sizeof(i915_gfx_eot_only))
		kern_memcpy(window + at, i915_gfx_eot_only, sizeof(i915_gfx_eot_only));
}

/*
 * Emits the start of one operation of a 3D batch: the pipeline switch, the
 * state bases and the state anv programs once per context before its first
 * draw.
 *
 * The surface and dynamic heaps are the operation's slot at state_va, the
 * instruction heap its kernels' window at instruction_va, and the general
 * state its kernels' scratch buffer at general_va (0 when they spill
 * nothing: stateless accesses and scratch pointers are then absolute
 * below 4 GiB, and none is made).  A stalling
 * flush precedes the pipeline select and the base addresses, so whatever an
 * earlier operation of the batch wrote is flushed first, and the caches
 * that hold state or read textures are invalidated after them.
 */
void
drv_i915_gfx_emit_context_setup(
	struct i915_gfx_batch *batch,
	uint64_t state_va,
	uint64_t instruction_va,
	uint64_t general_va,
	uint32_t mocs)
{
	uint64_t surface;
	uint64_t dynamic;
	uint64_t instruction;
	uint32_t index;
	uint32_t pattern;

	/* Locates the three heaps the state bases name. */
	surface = state_va + I915_GFX_SURFACE_HEAP;
	dynamic = state_va + I915_GFX_DYNAMIC_HEAP;
	instruction = instruction_va;

	/* Flushes and stalls before the pipeline is switched to 3D. */
	drv_i915_batch_pipe_control(batch,
				    PIPE_CONTROL_CS_STALL |
				    PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH |
				    PIPE_CONTROL_DEPTH_CACHE_FLUSH |
				    PIPE_CONTROL_DC_FLUSH_ENABLE |
				    PIPE_CONTROL_FLUSH_ENABLE);
	drv_i915_batch_emit(batch, GEN12_PIPELINE_SELECT_DWORD(GEN12_PIPELINE_SELECT_3D));

	/*
	 * Programs STATE_BASE_ADDRESS: the general base at the scratch buffer
	 * (zero without one) and the indirect base at zero, the surface and
	 * dynamic heaps with the given MOCS, the instruction heap write-back
	 * (instruction fetches go through the L3), every size the largest, the
	 * bindless surface heap over the surface heap and no bindless sampler
	 * heap.
	 */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_STATE_BASE_ADDRESS, GEN12_STATE_BASE_ADDRESS_DWORDS));
	drv_i915_batch_emit(batch, 1U | (mocs << 4) | ((uint32_t)general_va & 0xfffff000U));
	drv_i915_batch_emit(batch, (uint32_t)(general_va >> 32));
	drv_i915_batch_emit(batch, mocs << 16);
	drv_i915_batch_emit(batch, 1U | (mocs << 4) | ((uint32_t)surface & 0xfffff000U));
	drv_i915_batch_emit(batch, (uint32_t)(surface >> 32));
	drv_i915_batch_emit(batch, 1U | (mocs << 4) | ((uint32_t)dynamic & 0xfffff000U));
	drv_i915_batch_emit(batch, (uint32_t)(dynamic >> 32));
	drv_i915_batch_emit(batch, 1U | (mocs << 4));
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, 1U | (GEN12_MOCS(I915_MOCS_WRITEBACK_INDEX) << 4) | ((uint32_t)instruction & 0xfffff000U));
	drv_i915_batch_emit(batch, (uint32_t)(instruction >> 32));
	drv_i915_batch_emit(batch, 1U | (0xfffffU << 12));
	drv_i915_batch_emit(batch, 1U | (0xfffffU << 12));
	drv_i915_batch_emit(batch, 1U | (0xfffffU << 12));
	drv_i915_batch_emit(batch, 1U | (0xfffffU << 12));
	drv_i915_batch_emit(batch, 1U | (mocs << 4) | ((uint32_t)surface & 0xfffff000U));
	drv_i915_batch_emit(batch, (uint32_t)(surface >> 32));
	drv_i915_batch_emit(batch, (4096U / 64U - 1U) << 12);
	drv_i915_batch_emit(batch, 1U | (mocs << 4));
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, 0U);

	/*
	 * Invalidates the caches that may hold state from before the new bases,
	 * and the vertex fetch cache, which may hold a buffer an earlier draw's
	 * shader wrote (transform feedback) or a copy filled.
	 */
	drv_i915_batch_pipe_control(batch,
				    PIPE_CONTROL_CS_STALL |
				    PIPE_CONTROL_STATE_CACHE_INVALIDATE |
				    PIPE_CONTROL_CONST_CACHE_INVALIDATE |
				    PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE |
				    PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE |
				    PIPE_CONTROL_VF_CACHE_INVALIDATE);

	/* Clears the state anv clears once per context. */
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_WM_HZ_OP, GEN12_3DSTATE_WM_HZ_OP_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_AA_LINE_PARAMETERS, GEN12_3DSTATE_AA_LINE_PARAMETERS_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_WM_CHROMAKEY, GEN12_3DSTATE_WM_CHROMAKEY_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_POLY_STIPPLE_OFFSET, GEN12_3DSTATE_POLY_STIPPLE_OFFSET_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_LINE_STIPPLE, GEN12_3DSTATE_LINE_STIPPLE_DWORDS);

	/* Places the single sample at the pixel centre, and two and four samples at the standard positions. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_SAMPLE_PATTERN, GEN12_3DSTATE_SAMPLE_PATTERN_DWORDS));
	for (index = 1U; index < GEN12_3DSTATE_SAMPLE_PATTERN_DWORDS; index++) {
		/* Dword 7 carries the 4x pattern, dword 8 the 1x and 2x ones; the 8x and 16x dwords are zero. */
		pattern = 0U;
		if (index == 7U)
			pattern = GEN12_SAMPLE_PATTERN_4X;
		if (index == 8U)
			pattern = GEN12_SAMPLE_PATTERN_1X_CENTRE | GEN12_SAMPLE_PATTERN_2X;
		drv_i915_batch_emit(batch, pattern);
	}

	/* Clears the depth bounds and the binding tables of the unused geometry stages. */
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_DEPTH_BOUNDS, GEN12_3DSTATE_DEPTH_BOUNDS_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_BINDING_TABLE_POINTERS_VS, GEN12_3DSTATE_POINTERS_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_BINDING_TABLE_POINTERS_HS, GEN12_3DSTATE_POINTERS_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_BINDING_TABLE_POINTERS_DS, GEN12_3DSTATE_POINTERS_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_BINDING_TABLE_POINTERS_GS, GEN12_3DSTATE_POINTERS_DWORDS);
}

/*
 * Returns the GEN primitive type (GEN12_3DPRIM_*) of a pipeline's Vulkan
 * topology: the point, line and triangle lists, the line and triangle
 * strips and the triangle fan, and the topologies with adjacency for a
 * pipeline with a geometry kernel (ws075-p007b); 0 for one with adjacency
 * and no geometry kernel, or of patches, which the draw path does not take.
 */
uint32_t
drv_i915_gfx_topology(
	const struct i915_gfx_pipeline *pipeline)
{
	/* The topologies with adjacency, which only a geometry kernel reads (anv passes them on as they are). */
	if (pipeline->gs_binary != NULL) {
		/* Vulkan's topology with adjacency, as GEN names it. */
		switch (pipeline->topology) {
		case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
			return GEN12_3DPRIM_LINELIST_ADJ;
		case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
			return GEN12_3DPRIM_LINESTRIP_ADJ;
		case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
			return GEN12_3DPRIM_TRILIST_ADJ;
		case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
			return GEN12_3DPRIM_TRISTRIP_ADJ;
		default:
			break;
		}
	}

	/* Vulkan's topology, as GEN names it. */
	switch (pipeline->topology) {
	case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
		return GEN12_3DPRIM_POINTLIST;
	case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
		return GEN12_3DPRIM_LINELIST;
	case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
		return GEN12_3DPRIM_LINESTRIP;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
		return GEN12_3DPRIM_TRILIST;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
		return GEN12_3DPRIM_TRISTRIP;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
		return GEN12_3DPRIM_TRIFAN;
	default:
		break;
	}

	/* A topology the draw path does not take. */
	return 0U;
}

/*
 * Returns the vertices of one primitive of a GEN primitive type, as a
 * geometry kernel receives them (its input primitive): 1 for points, 2 for
 * lines, 3 for triangles, 4 and 6 with adjacency; 0 for a type that has
 * none (the rectangle list).
 */
uint32_t
drv_i915_gfx_topology_vertices(
	uint32_t topology)
{
	/* The vertices of each type's primitive. */
	switch (topology) {
	case GEN12_3DPRIM_POINTLIST:
		return 1U;
	case GEN12_3DPRIM_LINELIST:
	case GEN12_3DPRIM_LINESTRIP:
		return 2U;
	case GEN12_3DPRIM_TRILIST:
	case GEN12_3DPRIM_TRISTRIP:
	case GEN12_3DPRIM_TRIFAN:
		return 3U;
	case GEN12_3DPRIM_LINELIST_ADJ:
	case GEN12_3DPRIM_LINESTRIP_ADJ:
		return 4U;
	case GEN12_3DPRIM_TRILIST_ADJ:
	case GEN12_3DPRIM_TRISTRIP_ADJ:
		return 6U;
	default:
		break;
	}

	/* A type no geometry kernel takes. */
	return 0U;
}

/*
 * Emits the vertex buffers and vertex elements of a draw.
 *
 * One vertex element feeds each attribute the vertex kernel reads, in the
 * kernel's payload order, which is the ascending order of the locations.
 * Returns EINVAL for an attribute, binding or buffer the draw lacks and
 * ENOTSUP for a vertex format or topology the path does not implement; the
 * batch is then incomplete and must not run.
 */
int
drv_i915_gfx_emit_vertex_input(
	struct i915_gfx_batch *batch,
	const struct i915_gfx_draw_state *state,
	const struct i915_gfx_kernels *kernels,
	uint32_t mocs)
{
	const struct i915_gfx_pipeline *pipeline;
	struct i915_gfx_buffer *buffer;
	uint32_t order[I915_GFX_MAX_VERTEX_ATTRIBUTES];
	uint32_t index;
	uint32_t other;
	uint32_t count;
	uint32_t binding;
	uint32_t attribute;
	const struct i915_gfx_vertex_format *vertex_format;
	uint32_t component_y;
	uint32_t component_z;
	uint32_t component_w;
	uint32_t topology;
	uint32_t instanced;
	uint32_t sgvs;
	uint64_t va;
	int error;

	/* A draw needs at least one input: an attribute, or a generated index. */
	pipeline = state->pipeline;
	count = kernels->vs_input_count;
	if (count == 0U)
		return EINVAL;

	/* Finds the pipeline attribute of every location the kernel reads; a generated index has none. */
	sgvs = 0U;
	for (index = 0U; index < count; index++) {
		/* gl_VertexIndex and gl_InstanceIndex: the fetcher writes component x of the element (3DSTATE_VF_SGVS). */
		if (kernels->vs_inputs[index] == I915_SHADER_LOCATION_VERTEX_INDEX) {
			sgvs |= index | GEN12_SGVS_VERTEX_ID_ENABLE;
			order[index] = I915_GFX_NO_ATTRIBUTE;
			continue;
		}

		/* gl_InstanceIndex likewise, its element named in the instance half of the dword. */
		if (kernels->vs_inputs[index] == I915_SHADER_LOCATION_INSTANCE_INDEX) {
			sgvs |= (index << GEN12_SGVS_INSTANCE_ID_SHIFT) | GEN12_SGVS_INSTANCE_ID_ENABLE;
			order[index] = I915_GFX_NO_ATTRIBUTE;
			continue;
		}

		/* Looks the location up among the pipeline's attributes. */
		for (other = 0U; other < pipeline->attribute_count; other++) {
			if (pipeline->attributes[other].location == kernels->vs_inputs[index])
				break;
		}

		/* Refuses a location the pipeline does not describe. */
		if (other == pipeline->attribute_count) {
			kern_logf("i915: vk: draw refused: the vertex shader reads location %u and the pipeline has no such attribute\n",
				  kernels->vs_inputs[index]);
			return EINVAL;
		}

		order[index] = other;
	}

	/* Emits one VERTEX_BUFFER_STATE for each binding of the pipeline (none for a pipeline without). */
	if (pipeline->binding_count != 0U)
		drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_VERTEX_BUFFERS, 1U + pipeline->binding_count * GEN12_VERTEX_BUFFER_STATE_DWORDS));
	for (index = 0U; index < pipeline->binding_count; index++) {
		/* Refuses a binding number past the bindings a command buffer tracks. */
		binding = pipeline->bindings[index].binding;
		if (binding >= I915_GFX_MAX_VERTEX_BINDINGS)
			return EINVAL;

		/* Refuses a binding with no buffer bound. */
		buffer = state->vertex[binding].buffer;
		if (buffer == NULL)
			return EINVAL;

		/* Refuses an offset past the end of the buffer. */
		if (state->vertex[binding].offset > buffer->size)
			return EINVAL;

		/* Resolves the buffer range to its GPU address. */
		va = drv_i915_gfx_memory_va(buffer->memory, buffer->offset + state->vertex[binding].offset);
		if (va == 0U)
			return EINVAL;

		/* Writes the binding, MOCS, address modify enable and stride; the address; the size. */
		drv_i915_batch_emit(batch,
				    (binding << 26) |
				    GEN12_VERTEX_BUFFER_L3_BYPASS_DISABLE |
				    (mocs << 16) |
				    (1U << 14) |
				    (pipeline->bindings[index].stride & 0xfffU));
		drv_i915_batch_emit(batch, (uint32_t)va);
		drv_i915_batch_emit(batch, (uint32_t)(va >> 32));
		drv_i915_batch_emit(batch, (uint32_t)(buffer->size - state->vertex[binding].offset));
	}

	/* Emits one VERTEX_ELEMENT_STATE for each attribute, in payload order. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_VERTEX_ELEMENTS, 1U + count * GEN12_VERTEX_ELEMENT_STATE_DWORDS));
	for (index = 0U; index < count; index++) {
		/* A generated index: an element of zeros the fetcher writes the index into. */
		attribute = order[index];
		if (attribute == I915_GFX_NO_ATTRIBUTE) {
			drv_i915_batch_emit(batch, (1U << 25) | (I915_GFX_SURFACE_R32G32B32A32_FLOAT << 16));
			drv_i915_batch_emit(batch,
					    (GEN12_VFCOMP_STORE_0 << 28) |
					    (GEN12_VFCOMP_STORE_0 << 24) |
					    (GEN12_VFCOMP_STORE_0 << 20) |
					    (GEN12_VFCOMP_STORE_0 << 16));
			continue;
		}

		/* Refuses a vertex format the fetcher is not given. */
		error = i915_vertex_format(pipeline->attributes[attribute].format, &vertex_format);
		if (error != 0)
			return ENOTSUP;

		/*
		 * Stores the components the format has; a missing y or z is 0 and
		 * a missing w is 1.0 (the integer 1 for an integer format).
		 */
		component_y = GEN12_VFCOMP_STORE_0;
		if (vertex_format->components > 1U)
			component_y = GEN12_VFCOMP_STORE_SRC;
		component_z = GEN12_VFCOMP_STORE_0;
		if (vertex_format->components > 2U)
			component_z = GEN12_VFCOMP_STORE_SRC;
		component_w = GEN12_VFCOMP_STORE_1_FP;
		if (vertex_format->integer != 0U)
			component_w = GEN12_VFCOMP_STORE_1_INT;
		if (vertex_format->components > 3U)
			component_w = GEN12_VFCOMP_STORE_SRC;

		/* Writes the binding, valid bit, format and offset; the component controls. */
		drv_i915_batch_emit(batch,
				    (pipeline->attributes[attribute].binding << 26) |
				    (1U << 25) |
				    (vertex_format->surface_format << 16) |
				    (pipeline->attributes[attribute].offset & 0xfffU));
		drv_i915_batch_emit(batch,
				    (GEN12_VFCOMP_STORE_SRC << 28) |
				    (component_y << 24) |
				    (component_z << 20) |
				    (component_w << 16));
	}

	/* Enables the vertex fetch statistics and clears the fetcher's other state. */
	drv_i915_batch_emit(batch, (GEN12_CMD_3DSTATE_VF_STATISTICS << 16) | 1U);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_VF, GEN12_3DSTATE_VF_DWORDS);
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_VF_SGVS, GEN12_3DSTATE_VF_SGVS_DWORDS));
	drv_i915_batch_emit(batch, sgvs);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_VF_SGVS_2, GEN12_3DSTATE_VF_SGVS_2_DWORDS);

	/*
	 * Steps an element once an instance when its binding's input rate is
	 * VK_VERTEX_INPUT_RATE_INSTANCE (instancing enable, step rate 1, as anv
	 * programs it); every other element steps per vertex.
	 */
	for (index = 0U; index < count; index++) {
		instanced = 0U;
		attribute = order[index];
		if (attribute != I915_GFX_NO_ATTRIBUTE)
			instanced = i915_state_binding_instanced(pipeline, pipeline->attributes[attribute].binding);
		drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_VF_INSTANCING, GEN12_3DSTATE_VF_INSTANCING_DWORDS));
		drv_i915_batch_emit(batch, index | (instanced << GEN12_VF_INSTANCING_ENABLE_SHIFT));
		drv_i915_batch_emit(batch, instanced);
	}

	/* Refuses a topology the draw path does not take (adjacency and patches). */
	topology = drv_i915_gfx_topology(pipeline);
	if (topology == 0U) {
		kern_logf("i915: vk: XXX unimplemented path: primitive topology %u\n", pipeline->topology);
		return ENOTSUP;
	}

	/* Draws the pipeline's topology. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_VF_TOPOLOGY, GEN12_3DSTATE_VF_TOPOLOGY_DWORDS));
	drv_i915_batch_emit(batch, topology);

	/* Succeeded: the vertex fetcher is programmed. */
	return 0;
}

/*
 * Emits 3DSTATE_INDEX_BUFFER for the bound index buffer of an indexed draw.
 *
 * The buffer runs from the bound offset to its end.  Returns EINVAL for an
 * index buffer that is not bound, not bound to storage, or bound at an
 * offset past its end or not aligned to its index size, and ENOTSUP for an
 * index type other than 16 and 32 bits; the batch is then incomplete and
 * must not run.
 */
int
drv_i915_gfx_emit_index_buffer(
	struct i915_gfx_batch *batch,
	const struct i915_gfx_draw_state *state,
	uint32_t mocs)
{
	struct i915_gfx_buffer *buffer;
	uint64_t offset;
	uint64_t va;
	uint32_t format;
	uint32_t index_bytes;

	/* Refuses an indexed draw with no index buffer bound. */
	buffer = state->index.buffer;
	offset = state->index.offset;
	if (buffer == NULL) {
		kern_logf("i915: vk: draw refused: an indexed draw with no index buffer bound\n");
		return EINVAL;
	}

	/* Picks the index format and size of the index type (one byte: VK_EXT_index_type_uint8). */
	if (state->index.type == VK_INDEX_TYPE_UINT8_EXT) {
		format = GEN12_INDEX_BYTE;
		index_bytes = 1U;
	} else if (state->index.type == VK_INDEX_TYPE_UINT16) {
		format = GEN12_INDEX_WORD;
		index_bytes = 2U;
	} else if (state->index.type == VK_INDEX_TYPE_UINT32) {
		format = GEN12_INDEX_DWORD;
		index_bytes = 4U;
	} else {
		kern_logf("i915: vk: XXX unimplemented path: index type %u\n", state->index.type);
		return ENOTSUP;
	}

	/* Refuses an offset at or past the end of the buffer. */
	if (offset >= buffer->size)
		return EINVAL;

	/* Refuses an offset that does not start an index. */
	if ((offset % index_bytes) != 0U)
		return EINVAL;

	/* Resolves the bound range to its GPU address. */
	va = drv_i915_gfx_memory_va(buffer->memory, buffer->offset + offset);
	if (va == 0U)
		return EINVAL;

	/*
	 * Writes the MOCS, the index format and the L3 bypass disable (as for
	 * the vertex buffers); the address; the bytes up to the end of the
	 * buffer.
	 */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_INDEX_BUFFER, GEN12_3DSTATE_INDEX_BUFFER_DWORDS));
	drv_i915_batch_emit(batch,
			    mocs |
			    (format << GEN12_INDEX_FORMAT_SHIFT) |
			    GEN12_INDEX_BUFFER_L3_BYPASS_DISABLE);
	drv_i915_batch_emit(batch, (uint32_t)va);
	drv_i915_batch_emit(batch, (uint32_t)(va >> 32));
	drv_i915_batch_emit(batch, (uint32_t)(buffer->size - offset));

	/* Succeeded: the index buffer is programmed. */
	return 0;
}

/*
 * Emits the push constant and URB allocations.
 *
 * The push constant space goes 8 KiB to the vertex stage, 8 KiB to the
 * geometry stage and 16 KiB to the pixel stage on every draw.  Without a
 * geometry stage (gs_entry_size 0) the vertex stage owns the URB past the
 * push constants: entries of vs_entry_size 64-byte units, as many as fit,
 * at most 3576 (a multiple of eight, as Mesa's intel_get_urb_config() keeps
 * the vertex stage's count).  With one (ws075-p007b) the vertex stage keeps
 * 21 of those chunks and the geometry stage the next 6: entries of
 * gs_entry_size units, at most 1548, a multiple of eight when an entry is
 * smaller than nine units.  The hull and domain stages get none.  Returns
 * 0, or ENOTSUP when fewer than two geometry entries fit; nothing is
 * emitted then.
 */
int
drv_i915_gfx_emit_urb(
	struct i915_gfx_batch *batch,
	uint32_t vs_entry_size,
	uint32_t gs_entry_size)
{
	uint32_t opcode;
	uint32_t vs_bytes;
	uint32_t entries;
	uint32_t gs_entries;
	uint32_t gs_start;

	/* Counts the geometry stage's entries in its chunks, and gives the vertex stage the chunks before them. */
	vs_bytes = GEN12_URB_VS_BYTES;
	gs_entries = 0U;
	gs_start = 0U;
	if (gs_entry_size != 0U) {
		vs_bytes = GEN12_URB_VS_SPLIT_CHUNKS * GEN12_URB_CHUNK_KB * 1024U;
		gs_start = GEN12_URB_VS_START_CHUNK + GEN12_URB_VS_SPLIT_CHUNKS;
		gs_entries = (GEN12_URB_GS_CHUNKS * GEN12_URB_CHUNK_KB * 1024U) / (gs_entry_size * 64U);
		if (gs_entries > GEN12_URB_GS_ENTRIES)
			gs_entries = GEN12_URB_GS_ENTRIES;
		if (gs_entry_size < 9U)
			gs_entries &= ~7U;

		/* Refuses entries so large that the geometry stage cannot run two (its DUAL_OBJECT minimum). */
		if (gs_entries < GEN12_URB_GS_MIN_ENTRIES)
			return ENOTSUP;
	}

	/* Counts the entries the vertex stage's URB holds. */
	entries = vs_bytes / (vs_entry_size * 64U);
	if (entries > GEN12_URB_VS_ENTRIES)
		entries = GEN12_URB_VS_ENTRIES;
	entries &= ~7U;

	/* Gives the vertex stage the first 8 KiB of the push constant space. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_PUSH_CONSTANT_ALLOC_VS, GEN12_3DSTATE_PUSH_CONSTANT_ALLOC_DWORDS));
	drv_i915_batch_emit(batch, (0U << 16) | I915_GFX_PUSH_ALLOC_VS_KB);

	/* Gives the hull and domain stages none. */
	for (opcode = GEN12_CMD_3DSTATE_PUSH_CONSTANT_ALLOC_HS; opcode < GEN12_CMD_3DSTATE_PUSH_CONSTANT_ALLOC_GS; opcode++)
		drv_i915_batch_zero(batch, opcode, GEN12_3DSTATE_PUSH_CONSTANT_ALLOC_DWORDS);

	/* Gives the geometry stage the next 8 KiB, whether or not the draw has one. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_PUSH_CONSTANT_ALLOC_GS, GEN12_3DSTATE_PUSH_CONSTANT_ALLOC_DWORDS));
	drv_i915_batch_emit(batch, (I915_GFX_PUSH_ALLOC_VS_KB << 16) | I915_GFX_PUSH_ALLOC_GS_KB);

	/* Gives the pixel stage the last 16 KiB. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_PUSH_CONSTANT_ALLOC_PS, GEN12_3DSTATE_PUSH_CONSTANT_ALLOC_DWORDS));
	drv_i915_batch_emit(batch, ((I915_GFX_PUSH_ALLOC_VS_KB + I915_GFX_PUSH_ALLOC_GS_KB) << 16) | I915_GFX_PUSH_ALLOC_PS_KB);

	/* Gives the vertex stage its URB past the push constants. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_URB_ALLOC_VS, GEN12_3DSTATE_URB_ALLOC_DWORDS));
	drv_i915_batch_emit(batch, (GEN12_URB_VS_START_CHUNK << 10) | (GEN12_URB_VS_START_CHUNK << 21) | (vs_entry_size - 1U));
	drv_i915_batch_emit(batch, entries | (entries << 16));

	/* Gives the hull and domain stages no URB entries. */
	for (opcode = GEN12_CMD_3DSTATE_URB_ALLOC_HS; opcode < GEN12_CMD_3DSTATE_URB_ALLOC_GS; opcode++) {
		drv_i915_batch_emit(batch, GEN12_CMD_HEADER(opcode, GEN12_3DSTATE_URB_ALLOC_DWORDS));
		drv_i915_batch_emit(batch, (5U << 10) | (5U << 21));
		drv_i915_batch_emit(batch, 0U);
	}

	/* Gives the geometry stage its chunks and entries, or none as the hull and domain stages. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_URB_ALLOC_GS, GEN12_3DSTATE_URB_ALLOC_DWORDS));
	if (gs_entries != 0U) {
		drv_i915_batch_emit(batch, (gs_start << 10) | (gs_start << 21) | (gs_entry_size - 1U));
		drv_i915_batch_emit(batch, gs_entries | (gs_entries << 16));
	} else {
		drv_i915_batch_emit(batch, (5U << 10) | (5U << 21));
		drv_i915_batch_emit(batch, 0U);
	}

	/* Succeeded: the push constants and the URB are allocated. */
	return 0;
}

/*
 * Emits the 3DSTATE_CONSTANT_* packets of all five stages.
 *
 * The vertex, the geometry (ws075-p007b) and the pixel stage each read
 * their own push data, as many registers as its kernel uses.  Like anv, the
 * buffer goes in the highest slot, so that slot 0 is never the only one in
 * use; a stage that reads nothing is given an empty packet.
 */
void
drv_i915_gfx_emit_constants(
	struct i915_gfx_batch *batch,
	uint64_t vs_push_va,
	uint32_t vs_push_regs,
	uint64_t gs_push_va,
	uint32_t gs_push_regs,
	uint64_t ps_push_va,
	uint32_t ps_push_regs,
	uint32_t mocs)
{
	unsigned stage;
	unsigned index;
	uint32_t push_regs;
	uint64_t push_va;

	/* Emits one packet for each stage. */
	for (stage = 0U; stage < 5U; stage++) {
		drv_i915_batch_emit(batch, GEN12_CMD_HEADER(i915_gfx_constant_opcodes[stage], GEN12_3DSTATE_CONSTANT_DWORDS) | (mocs << 8));

		/* The vertex stage (first), the geometry stage (fourth) and the pixel stage (last) read push data; the others none. */
		push_regs = 0U;
		push_va = 0U;
		if (stage == 0U) {
			push_regs = vs_push_regs;
			push_va = vs_push_va;
		} else if (stage == 3U) {
			push_regs = gs_push_regs;
			push_va = gs_push_va;
		} else if (stage == 4U) {
			push_regs = ps_push_regs;
			push_va = ps_push_va;
		}

		/* A stage with push data reads it from buffer 3. */
		if (push_regs != 0U) {
			drv_i915_batch_emit(batch, 0U);
			drv_i915_batch_emit(batch, push_regs << 16);
			for (index = 3U; index < 9U; index++)
				drv_i915_batch_emit(batch, 0U);
			drv_i915_batch_emit(batch, (uint32_t)push_va);
			drv_i915_batch_emit(batch, (uint32_t)(push_va >> 32));
			continue;
		}

		/* Every other packet reads nothing. */
		for (index = 1U; index < GEN12_3DSTATE_CONSTANT_DWORDS; index++)
			drv_i915_batch_emit(batch, 0U);
	}
}

/*
 * Returns the log2 of a sample count, as the multisample fields take it:
 * 0 for one sample (or none), 1 for two, 2 for four.
 */
uint32_t
drv_i915_gfx_samples_log2(
	uint32_t samples)
{
	/* Four samples. */
	if (samples >= 4U)
		return 2U;

	/* Two samples. */
	if (samples == 2U)
		return 1U;

	/* Succeeded: one sample. */
	return 0U;
}

/*
 * Emits 3DSTATE_CLIP, SF and RASTER of an ordinary Vulkan pipeline, as anv
 * programs them.
 *
 * A last stage that writes the point size has the setup take the point
 * width from each vertex; any other draws its points one pixel wide.  The
 * clipper takes each primitive's layer from the VUE only when the last
 * stage writes it (a geometry kernel, ws075-p007b), else layer 0; with a
 * geometry kernel the setup dereferences the URB a primitive at a time.
 */
void
drv_i915_gfx_emit_raster(
	struct i915_gfx_batch *batch,
	const struct i915_gfx_pipeline *pipeline,
	const struct i915_gfx_kernels *kernels)
{
	uint32_t cull;
	uint32_t counter_clockwise;
	uint32_t point_width;
	uint32_t linear;
	uint32_t multisample;
	uint32_t zero_layer;
	uint32_t deref;
	uint32_t index;

	/* The clipper prepares the linear barycentrics when the pixel kernel reads them. */
	linear = 0U;
	if (kernels->ps_linear_barycentrics != 0U)
		linear = GEN12_CLIP_NON_PERSPECTIVE_BARYCENTRIC;

	/*
	 * Forces render target array index 0 unless the last stage writes the
	 * layer (Vulkan: a layer not written is layer 0; anv's
	 * ForceZeroRTAIndexEnable).
	 */
	zero_layer = GEN12_CLIP_FORCE_ZERO_RTA_INDEX;
	if (kernels->gs_writes_layer != 0U)
		zero_layer = 0U;

	/* The setup dereferences the URB a primitive at a time when a geometry kernel is the last stage (Gen12, intel_get_urb_config()). */
	deref = GEN12_URB_DEREF_BLOCK_SIZE_32;
	if (kernels->gs_code != NULL)
		deref = GEN12_URB_DEREF_BLOCK_SIZE_PER_POLY;

	/*
	 * Clips with statistics, early cull and 8-bit subpixel precision; the
	 * D3D API mode (z in [0, 1]), viewport XY test and guardband, and the
	 * linear barycentrics when asked; a fan's provoking vertex is the
	 * second of each triangle (Vulkan's first-vertex convention); point
	 * widths 0.125 .. 255.875 and the forced layer 0.
	 */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_CLIP, GEN12_3DSTATE_CLIP_DWORDS));
	drv_i915_batch_emit(batch, (1U << 10) | (1U << 18));
	drv_i915_batch_emit(batch,
			    (1U << 31) | (1U << 30) | (1U << 28) | (1U << 26) | linear |
			    (GEN12_FAN_PROVOKING_SECOND << GEN12_CLIP_FAN_PROVOKING_SHIFT));
	drv_i915_batch_emit(batch, (1U << 17) | (2047U << 6) | zero_layer);

	/* The point width comes from the vertices when the last stage writes it, else it is 1.0 from state. */
	point_width = GEN12_SF_POINT_WIDTH_ONE | GEN12_SF_POINT_WIDTH_FROM_STATE;
	if (kernels->vs_point_size != 0U)
		point_width = GEN12_SF_POINT_WIDTH_ONE;

	/*
	 * Sets up with the viewport transform, statistics and line width 1.0;
	 * the URB deref block; the point width, the AA line distance and the
	 * fan's provoking vertex as the clipper's.
	 */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_SF, GEN12_3DSTATE_SF_DWORDS));
	drv_i915_batch_emit(batch, (1U << 1) | (1U << 10) | (128U << 12));
	drv_i915_batch_emit(batch, deref << 29);
	drv_i915_batch_emit(batch, point_width | (1U << 14) | (GEN12_FAN_PROVOKING_SECOND << GEN12_SF_FAN_PROVOKING_SHIFT));

	/* Translates the pipeline's cull mode; front and back together cull both. */
	switch (pipeline->cull_mode) {
	case VK_CULL_MODE_NONE:
		cull = GEN12_CULLMODE_NONE;
		break;
	case VK_CULL_MODE_FRONT_BIT:
		cull = GEN12_CULLMODE_FRONT;
		break;
	case VK_CULL_MODE_BACK_BIT:
		cull = GEN12_CULLMODE_BACK;
		break;
	default:
		cull = GEN12_CULLMODE_BOTH;
		break;
	}

	/* A counter-clockwise front face sets the front winding bit. */
	counter_clockwise = 0U;
	if (pipeline->front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE)
		counter_clockwise = 1U;

	/* A pipeline of several samples rasterizes on the sample pattern (anv's DXMultisampleRasterizationEnable). */
	multisample = 0U;
	if (pipeline->samples > 1U)
		multisample = GEN12_RASTER_DX_MULTISAMPLE_ENABLE;

	/* Rasterizes with z near and far clip tests, scissor, the cull mode, the winding, the DX10.1+ API mode and the samples. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_RASTER, GEN12_3DSTATE_RASTER_DWORDS));
	drv_i915_batch_emit(batch,
			    (1U << 0) |
			    (1U << 26) |
			    (1U << 1) |
			    (cull << 16) |
			    (counter_clockwise << 21) |
			    (2U << 22) |
			    multisample);
	for (index = 2U; index < GEN12_3DSTATE_RASTER_DWORDS; index++)
		drv_i915_batch_emit(batch, 0U);
}

/*
 * Emits the depth test, the depth buffer and the post-sync write anv makes
 * after the depth state.
 *
 * With no depth attachment the depth buffer is a null D32_FLOAT surface and
 * the test is off.  Returns EINVAL for a depth attachment with no storage or
 * in a format other than D32_SFLOAT and D16_UNORM.
 */
int
drv_i915_gfx_emit_depth(
	struct i915_gfx_batch *batch,
	const struct i915_gfx_draw_state *state,
	const struct i915_gfx_image *depth,
	uint64_t scratch_va,
	uint32_t mocs)
{
	const struct i915_gfx_pipeline *pipeline;
	uint32_t depth_state;
	uint32_t stencil_masks;
	uint32_t stencil_references;
	uint32_t write_enable;
	uint32_t test_enable;
	uint32_t format;
	uint32_t layer;
	uint32_t layers;
	uint32_t rows;
	uint32_t index;
	uint64_t va;
	int has_depth;
	int error;

	/* Packs the depth write, the depth test and its compare function when there is a depth buffer. */
	pipeline = state->pipeline;
	depth_state = 0U;
	stencil_masks = 0U;
	stencil_references = 0U;
	has_depth = 0;
	if (depth != NULL && depth->format != VK_FORMAT_S8_UINT)
		has_depth = 1;
	if (has_depth != 0) {
		write_enable = 0U;
		if (pipeline->depth_write != 0U)
			write_enable = 1U;
		test_enable = 0U;
		if (pipeline->depth_test != 0U)
			test_enable = 1U;
		depth_state = write_enable | (test_enable << 1) | (i915_gfx_compare_functions[pipeline->depth_compare & 7U] << 5);
	}

	/* Adds the stencil test when the depth attachment has stencil and the pipeline tests it. */
	if (depth != NULL && depth->stencil != 0U && pipeline->stencil_test != 0U)
		i915_state_stencil(state, &depth_state, &stencil_masks, &stencil_references);

	/* Programs the depth and stencil tests. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_WM_DEPTH_STENCIL, GEN12_3DSTATE_WM_DEPTH_STENCIL_DWORDS));
	drv_i915_batch_emit(batch, depth_state);
	drv_i915_batch_emit(batch, stencil_masks);
	drv_i915_batch_emit(batch, stencil_references);

	/* Describes the depth buffer, or a null one. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_DEPTH_BUFFER, GEN12_3DSTATE_DEPTH_BUFFER_DWORDS));
	if (has_depth != 0) {
		/* Refuses a depth image with no storage or in another format. */
		va = drv_i915_gfx_memory_va(depth->memory, depth->offset);
		if (va == 0U)
			return EINVAL;
		if (depth->format == VK_FORMAT_D32_SFLOAT || depth->format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
			format = GEN12_DEPTH_FORMAT_D32_FLOAT;
		} else if (depth->format == VK_FORMAT_D16_UNORM) {
			format = GEN12_DEPTH_FORMAT_D16_UNORM;
		} else {
			return EINVAL;
		}

		/* The first layer the pass's depth view writes and how many, and the rows from layer to layer. */
		layer = 0U;
		layers = 1U;
		if (state->framebuffer != NULL && state->pass != NULL &&
		    state->pass->depth_attachment < state->framebuffer->view_count &&
		    state->framebuffer->views[state->pass->depth_attachment] != NULL) {
			layer = state->framebuffer->views[state->pass->depth_attachment]->base_layer;
			if (state->framebuffer->views[state->pass->depth_attachment]->layer_count > 1U)
				layers = state->framebuffer->views[state->pass->depth_attachment]->layer_count;
		}
		rows = depth->slice_rows;
		if (rows == 0U)
			rows = (depth->height + 3U) & ~3U;

		/*
		 * Writes what isl_emit_depth_stencil_hiz_s() writes: 2D, the
		 * format, write enable and the pitch (Y-tiled: Gen9+ depth always
		 * is); the address; the extent; MOCS, the first array element (the
		 * view's first layer) and the depth (the view's layers less one);
		 * the QPitch and the render target view extent, which is the depth
		 * again (isl_emit_depth_stencil.c of Mesa 25.0.7, sha256
		 * d0a71883586e838971621169f7439a9d1ebddb919241cf5937620e5742a56778,
		 * 147-163; gen120.xml 3DSTATE_DEPTH_BUFFER, Render Target View
		 * Extent bits 245-255, dword 7 bits 31:21).
		 */
		drv_i915_batch_emit(batch, (GEN12_SURFTYPE_2D << 29) | (1U << 28) | (format << 24) | (depth->pitch - 1U));
		drv_i915_batch_emit(batch, (uint32_t)va);
		drv_i915_batch_emit(batch, (uint32_t)(va >> 32));
		drv_i915_batch_emit(batch, ((depth->width - 1U) << 1) | ((depth->height - 1U) << 17));
		drv_i915_batch_emit(batch, mocs | ((layer & GEN12_RSS_DEPTH_MASK) << 8) |
		    (((layers - 1U) & GEN12_RSS_DEPTH_MASK) << 20));
		drv_i915_batch_emit(batch, 0U);
		drv_i915_batch_emit(batch, (rows / 4U) | (((layers - 1U) & GEN12_RSS_DEPTH_MASK) << 21));
	} else {
		/* A null depth buffer is still typed D32_FLOAT. */
		drv_i915_batch_emit(batch, (GEN12_SURFTYPE_NULL << 29) | (GEN12_DEPTH_FORMAT_D32_FLOAT << 24));
		for (index = 2U; index < GEN12_3DSTATE_DEPTH_BUFFER_DWORDS; index++)
			drv_i915_batch_emit(batch, 0U);
	}

	/* Describes the stencil buffer of an attachment with stencil, or a null one. */
	error = i915_state_stencil_buffer(batch, state, depth, mocs);
	if (error != 0)
		return error;

	/* Clears the hierarchical depth and the clear values. */
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_HIER_DEPTH_BUFFER, GEN12_3DSTATE_HIER_DEPTH_BUFFER_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_CLEAR_PARAMS, GEN12_3DSTATE_CLEAR_PARAMS_DWORDS);

	/* Makes the post-sync write anv makes after the depth state (Wa_1408224581, Wa_14014097488, Wa_14016712196). */
	drv_i915_batch_emit(batch, GFX_OP_PIPE_CONTROL(6));
	drv_i915_batch_emit(batch, PIPE_CONTROL_QW_WRITE);
	drv_i915_batch_emit(batch, (uint32_t)scratch_va);
	drv_i915_batch_emit(batch, (uint32_t)(scratch_va >> 32));
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, 0U);

	/* Succeeded: the depth state is programmed. */
	return 0;
}

/*
 * Emits 3DSTATE_VS for the vertex kernel.
 *
 * The kernel starts at the vertex kernel offset of the instruction heap and
 * runs SIMD8 with statistics; it reads its attributes from the URB in pairs
 * starting at 0.
 */
void
drv_i915_gfx_emit_vertex_shader(
	struct i915_gfx_batch *batch,
	const struct i915_gfx_kernels *kernels)
{
	uint64_t scratch;

	/* The scratch space of a kernel that spills: its per-thread size and the stage's buffer. */
	scratch = i915_state_scratch(kernels->vs_scratch_bytes, kernels->vs_scratch_offset);

	/*
	 * Writes the kernel start pointer; IEEE-754 with no samplers and no
	 * binding table; the scratch space; the first payload register and the
	 * URB read length; the thread count, statistics, SIMD8 and enable.
	 */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_VS, GEN12_3DSTATE_VS_DWORDS));
	drv_i915_batch_emit(batch, I915_GFX_VS_KERNEL);
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, (uint32_t)scratch);
	drv_i915_batch_emit(batch, (uint32_t)(scratch >> 32));
	drv_i915_batch_emit(batch, (kernels->vs_grf_start << 20) | (((kernels->vs_input_count + 1U) / 2U) << 11));
	drv_i915_batch_emit(batch, ((I915_GFX_MAX_VS_THREADS - 1U) << 22) | (1U << 10) | (1U << 2) | 1U);
	drv_i915_batch_emit(batch, 0U);
}

/*
 * Emits 3DSTATE_GS: the geometry kernel when the draw has one
 * (ws075-p007b), as anv programs it, else an empty packet (the stage off).
 *
 * The kernel starts at the geometry kernel offset and runs SIMD8 with
 * statistics, one input primitive to a channel, with no samplers and no
 * binding table.  Its payload carries the input vertices' URB handles (and
 * the primitive's number when it reads it) and no vertex data: every input
 * is pulled from the URB.  The vertex count of each thread's output is
 * dynamic, written by the kernel at the start of its URB entry.
 */
void
drv_i915_gfx_emit_geometry_shader(
	struct i915_gfx_batch *batch,
	const struct i915_gfx_kernels *kernels)
{
	uint64_t scratch;
	uint32_t grf;
	uint32_t primitive_id;

	/* A draw without a geometry kernel turns the stage off. */
	if (kernels->gs_code == NULL) {
		drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_GS, GEN12_3DSTATE_GS_DWORDS);
		return;
	}

	/* The scratch space of a kernel that spills, the first payload register after the fixed ones, and the primitive's number. */
	scratch = i915_state_scratch(kernels->gs_scratch_bytes, kernels->gs_scratch_offset);
	grf = kernels->gs_grf_start;
	primitive_id = 0U;
	if (kernels->gs_primitive_id != 0U)
		primitive_id = 1U;

	/*
	 * Writes the kernel start pointer; the vertices of an input primitive,
	 * IEEE-754, no samplers and no binding table; the scratch space; the
	 * first payload register (bits 3:0, and 5:4 in bits 30:29), the vertex
	 * handles included and no vertex data read, the output topology and
	 * vertex size (in 16-byte units, less one); enable, the primitive's
	 * number, statistics, SIMD8 dispatch, one instance and the control data
	 * header's size; the thread count and the control data format.
	 */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_GS, GEN12_3DSTATE_GS_DWORDS));
	drv_i915_batch_emit(batch, I915_GFX_GS_KERNEL);
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, kernels->gs_vertices_in & 0x3fU);
	drv_i915_batch_emit(batch, (uint32_t)scratch);
	drv_i915_batch_emit(batch, (uint32_t)(scratch >> 32));
	drv_i915_batch_emit(batch,
			    (grf & 0xfU) |
			    (1U << 10) |
			    (kernels->gs_output_topology << 17) |
			    ((kernels->gs_output_vertex_hwords * 2U - 1U) << 23) |
			    (((grf >> 4) & 0x3U) << 29));
	drv_i915_batch_emit(batch,
			    1U |
			    (primitive_id << 4) |
			    (1U << 10) |
			    (GEN12_GS_DISPATCH_MODE_SIMD8 << 11) |
			    (kernels->gs_control_hwords << 20));
	drv_i915_batch_emit(batch, (I915_GFX_MAX_GS_THREADS - 1U) | (kernels->gs_control_format << 31));
	drv_i915_batch_emit(batch, 0U);
}

/*
 * Emits SBE, SBE_SWIZ, WM, PS and PS_EXTRA for the pixel kernel.
 *
 * Every varying the last stage writes is read from VUE slot 2 on, and
 * fragment input n takes the slot the pipeline routed it from (the n-th
 * for a rectangle kernel).  The kernel starts at the pixel kernel offset
 * and runs 8-pixel dispatch only.
 */
void
drv_i915_gfx_emit_pixel_shader(
	struct i915_gfx_batch *batch,
	const struct i915_gfx_kernels *kernels)
{
	uint32_t index;
	uint32_t read_length;
	uint32_t inputs;
	uint32_t low;
	uint32_t high;
	uint32_t sampler_groups;
	uint32_t has_varyings;
	uint32_t push_enable;
	uint32_t kills;
	uint32_t barycentrics;
	uint32_t source;
	uint64_t scratch;

	/* Reads the varyings in pairs of slots, at least one pair. */
	read_length = 1U;
	if (kernels->varyings != 0U)
		read_length = (kernels->varyings + 1U) / 2U;

	/* Counts the fragment inputs: the routed ones, or every varying of a rectangle. */
	inputs = kernels->varyings;
	if (kernels->ps_inputs_mapped != 0U)
		inputs = kernels->ps_input_count;

	/*
	 * Programs SBE: the attribute swizzle and the read offset override, the
	 * number of attributes, the point sprite's origin at the upper left
	 * (Vulkan's), the read length and the read offset of slot 2; the
	 * attributes the point sprite's coordinate replaces; the Flat inputs'
	 * constant interpolation; every attribute with all four components
	 * active.
	 */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_SBE, GEN12_3DSTATE_SBE_DWORDS));
	drv_i915_batch_emit(batch,
			    (1U << 29) |
			    (1U << 28) |
			    (inputs << 22) |
			    (1U << 21) |
			    (GEN12_SBE_POINT_SPRITE_ORIGIN_UPPER_LEFT << GEN12_SBE_POINT_SPRITE_ORIGIN_SHIFT) |
			    (read_length << 11) |
			    (1U << 5));
	drv_i915_batch_emit(batch, kernels->ps_point_sprite_mask);
	drv_i915_batch_emit(batch, kernels->ps_flat_mask);
	drv_i915_batch_emit(batch, 0xffffffffU);
	drv_i915_batch_emit(batch, 0xffffffffU);

	/* Programs SBE_SWIZ: the source slot of each fragment input, two inputs to a dword. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_SBE_SWIZ, GEN12_3DSTATE_SBE_SWIZ_DWORDS));
	for (index = 0U; index < I915_GFX_MAX_VARYINGS; index += 2U) {
		low = i915_state_input_slot(kernels, inputs, index);
		high = i915_state_input_slot(kernels, inputs, index + 1U);
		drv_i915_batch_emit(batch, low | (high << 16));
	}

	/* Leaves the two swizzle control dwords at zero. */
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, 0U);

	/* The perspective barycentrics always; the linear ones when the kernel interpolates without perspective. */
	barycentrics = GEN12_WM_BARYCENTRIC_PERSPECTIVE_PIXEL;
	if (kernels->ps_linear_barycentrics != 0U)
		barycentrics |= GEN12_WM_BARYCENTRIC_LINEAR_PIXEL;

	/* Programs WM: statistics, the barycentrics, line AA width 1.0. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_WM, GEN12_3DSTATE_WM_DWORDS));
	drv_i915_batch_emit(batch, (1U << 31) | barycentrics | (1U << 6));

	/* The sampler count is programmed in groups of four samplers. */
	sampler_groups = (kernels->ps_samplers + 3U) / 4U;

	/* A kernel that reads push constants has them delivered in front of its setup data. */
	push_enable = 0U;
	if (kernels->ps_push_regs != 0U)
		push_enable = GEN12_3DSTATE_PS_PUSH_CONSTANT_ENABLE;

	/* The scratch space of a kernel that spills: its per-thread size and the stage's buffer. */
	scratch = i915_state_scratch(kernels->ps_scratch_bytes, kernels->ps_scratch_offset);

	/*
	 * Programs PS: kernel 0 is the SIMD8 one; the vector mask, the sampler
	 * count and the binding table entries (the render target and the
	 * samplers); the scratch space; the thread count, the push constant
	 * enable and 8-pixel dispatch; the first payload register.
	 */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_PS, GEN12_3DSTATE_PS_DWORDS));
	drv_i915_batch_emit(batch, I915_GFX_PS_KERNEL);
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, (1U << 30) | (sampler_groups << 27) | ((1U + kernels->ps_samplers) << 18));
	drv_i915_batch_emit(batch, (uint32_t)scratch);
	drv_i915_batch_emit(batch, (uint32_t)(scratch >> 32));
	drv_i915_batch_emit(batch, ((GEN12_MAX_THREADS_PER_PSD - 1U) << 23) | push_enable | 1U);
	drv_i915_batch_emit(batch, kernels->ps_grf_start << 16);
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, 0U);

	/* Notes whether the kernel reads attributes. */
	has_varyings = 0U;
	if (inputs != 0U)
		has_varyings = 1U;

	/* Notes whether the kernel discards pixels. */
	kills = 0U;
	if (kernels->ps_kills != 0U)
		kills = GEN12_3DSTATE_PS_EXTRA_KILLS_PIXEL;

	/* Notes whether the kernel reads the pixel's depth and w. */
	source = 0U;
	if (kernels->ps_source_depth != 0U)
		source |= GEN12_3DSTATE_PS_EXTRA_USES_SOURCE_DEPTH;
	if (kernels->ps_source_w != 0U)
		source |= GEN12_3DSTATE_PS_EXTRA_USES_SOURCE_W;

	/* Programs PS_EXTRA: valid, whether the kernel discards, reads the depth and w, and reads attributes. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_PS_EXTRA, GEN12_3DSTATE_PS_EXTRA_DWORDS));
	drv_i915_batch_emit(batch, (1U << 31) | kills | source | (has_varyings << 8));
}

/*
 * Packs dwords 4-5 of 3DSTATE_VS or PS: the Scratch Space Base Pointer of
 * the stage's part of the scratch buffer (an offset from the general state
 * base, which the draw sets to the buffer) and the Per-Thread Scratch Space
 * of the kernel
 * (1 KiB << n, the n Mesa's get_scratch_space() computes); zero for a
 * kernel that spills nothing.
 */
static uint64_t
i915_state_scratch(
	uint32_t per_thread_bytes,
	uint64_t offset)
{
	uint32_t space;
	uint32_t bytes;

	/* A kernel that spills nothing has no scratch space. */
	if (per_thread_bytes == 0U)
		return 0U;

	/* Finds n with 1 KiB << n equal to the kernel's space (the compiler rounds it to a power of two). */
	space = 0U;
	bytes = GEN12_SCRATCH_SPACE_MIN_BYTES;
	while (bytes < per_thread_bytes && space < GEN12_SCRATCH_SPACE_MAX) {
		bytes *= 2U;
		space++;
	}

	/* Succeeded: the pointer's bits 63:10 and the space in bits 3:0. */
	return (offset & ~(uint64_t)(GEN12_SCRATCH_POINTER_ALIGN - 1U)) | (uint64_t)(space & GEN12_SCRATCH_SPACE_MASK);
}

/*
 * Emits 3DSTATE_PS_BLEND for the pipeline's colour blend.
 *
 * It repeats what BLEND_STATE says of attachment 0, as anv keeps the two
 * consistent (genX_gfx_state.c): the one colour attachment is writeable.
 */
void
drv_i915_gfx_emit_ps_blend(
	struct i915_gfx_batch *batch,
	const struct i915_gfx_pipeline *pipeline)
{
	struct i915_gfx_blend equation;
	uint32_t word;

	/* Works out the hardware blend of attachment 0. */
	i915_blend_equation(pipeline, &equation);

	/* The render target is writeable; a blending one names its factors. */
	word = GEN12_PS_BLEND_HAS_WRITEABLE_RT;
	if (equation.enable != 0U) {
		word |= GEN12_PS_BLEND_ENABLE |
		    (equation.src_color << GEN12_PS_BLEND_SRC_FACTOR_SHIFT) |
		    (equation.dst_color << GEN12_PS_BLEND_DST_FACTOR_SHIFT) |
		    (equation.src_alpha << GEN12_PS_BLEND_SRC_ALPHA_FACTOR_SHIFT) |
		    (equation.dst_alpha << GEN12_PS_BLEND_DST_ALPHA_FACTOR_SHIFT);
	}

	/* An alpha that blends its own way says so. */
	if (equation.independent_alpha != 0U)
		word |= GEN12_PS_BLEND_INDEPENDENT_ALPHA;

	/* Emits the packet. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_PS_BLEND, GEN12_3DSTATE_PS_BLEND_DWORDS));
	drv_i915_batch_emit(batch, word);
}

/*
 * Emits the end of a one-operation 3D batch: the draw (see
 * drv_i915_gfx_emit_draw()) and the batch end.
 */
void
drv_i915_gfx_emit_primitive(
	struct i915_gfx_batch *batch,
	uint32_t width,
	uint32_t height,
	const struct i915_gfx_primitive *primitive)
{
	/* Draws the primitive, then ends the batch. */
	drv_i915_gfx_emit_draw(batch, width, height, primitive);
	drv_i915_gfx_emit_batch_end(batch);
}

/*
 * Emits the end of one operation of a 3D batch: the pixel stage's sampler
 * and binding table pointers, the drawing rectangle, the primitive and the
 * closing flush.
 *
 * The pixel pipeline is synced before the primitive, and every cache the
 * primitive wrote through is flushed after it.  A random-access primitive
 * reads its vertices through the index buffer programmed before it.
 */
void
drv_i915_gfx_emit_draw(
	struct i915_gfx_batch *batch,
	uint32_t width,
	uint32_t height,
	const struct i915_gfx_primitive *primitive)
{
	uint32_t access;

	/* Points the pixel stage at its sampler and its binding table. */
	drv_i915_batch_pointer(batch, GEN12_CMD_3DSTATE_SAMPLER_STATE_POINTERS_PS, I915_GFX_DYN_SAMPLER);
	drv_i915_batch_pointer(batch, GEN12_CMD_3DSTATE_BINDING_TABLE_POINTERS_PS, I915_GFX_BINDING_TABLE);

	/* Limits drawing to the target. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_DRAWING_RECTANGLE, GEN12_3DSTATE_DRAWING_RECTANGLE_DWORDS));
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, (width - 1U) | ((height - 1U) << 16));
	drv_i915_batch_emit(batch, 0U);

	/* Syncs the pixel pipeline before the primitive. */
	drv_i915_batch_pipe_control(batch,
				    PIPE_CONTROL_CS_STALL |
				    PIPE_CONTROL_STALL_AT_SCOREBOARD |
				    PIPE_CONTROL_DEPTH_STALL_ENABLE);

	/* An indexed draw fetches its vertices at random through the index buffer. */
	access = 0U;
	if (primitive->random_access != 0)
		access = GEN12_3DPRIMITIVE_VERTEX_RANDOM;

	/*
	 * Draws the primitive: the topology and the vertex access; the
	 * vertices of an instance and the first of them; the instances and the
	 * first of them; the base vertex.
	 */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DPRIMITIVE, GEN12_3DPRIMITIVE_DWORDS));
	drv_i915_batch_emit(batch, primitive->topology | access);
	drv_i915_batch_emit(batch, primitive->vertex_count);
	drv_i915_batch_emit(batch, primitive->start_vertex);
	drv_i915_batch_emit(batch, primitive->instance_count);
	drv_i915_batch_emit(batch, primitive->start_instance);
	drv_i915_batch_emit(batch, (uint32_t)primitive->base_vertex);

	/* Flushes what the primitive wrote. */
	drv_i915_batch_pipe_control(batch,
				    PIPE_CONTROL_CS_STALL |
				    PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH |
				    PIPE_CONTROL_DEPTH_CACHE_FLUSH |
				    PIPE_CONTROL_DC_FLUSH_ENABLE |
				    PIPE_CONTROL_FLUSH_ENABLE);
}

/*
 * Ends a batch: MI_BATCH_BUFFER_END, and one MI_NOOP that keeps the batch
 * a whole number of quadwords.
 */
void
drv_i915_gfx_emit_batch_end(
	struct i915_gfx_batch *batch)
{
	/* Ends the batch. */
	drv_i915_batch_emit(batch, MI_BATCH_BUFFER_END);
	drv_i915_batch_emit(batch, MI_NOOP);
}

/* Translates a VkFormat to its genxml SURFACE_FORMAT; ENOTSUP for any other format. */
static int
i915_surface_format(
	uint32_t format,
	uint32_t *surface_format)
{
	/* Picks the surface format of each supported VkFormat. */
	switch (format) {
	case VK_FORMAT_R8G8B8A8_UNORM:
		*surface_format = I915_GFX_SURFACE_R8G8B8A8_UNORM;
		return 0;
	case VK_FORMAT_B8G8R8A8_UNORM:
		*surface_format = I915_GFX_SURFACE_B8G8R8A8_UNORM;
		return 0;
	case VK_FORMAT_R8G8B8A8_SRGB:
		*surface_format = I915_GFX_SURFACE_R8G8B8A8_UNORM_SRGB;
		return 0;
	case VK_FORMAT_B8G8R8A8_SRGB:
		*surface_format = I915_GFX_SURFACE_B8G8R8A8_UNORM_SRGB;
		return 0;
	case VK_FORMAT_R8G8B8A8_SINT:
		*surface_format = I915_GFX_SURFACE_R8G8B8A8_SINT;
		return 0;
	case VK_FORMAT_R8G8B8A8_UINT:
		*surface_format = I915_GFX_SURFACE_R8G8B8A8_UINT;
		return 0;
	case VK_FORMAT_R32_SFLOAT:
		*surface_format = I915_GFX_SURFACE_R32_FLOAT;
		return 0;
	case VK_FORMAT_R8_UNORM:
		*surface_format = I915_GFX_SURFACE_R8_UNORM;
		return 0;
	case VK_FORMAT_R8G8_UNORM:
		*surface_format = I915_GFX_SURFACE_R8G8_UNORM;
		return 0;
	case VK_FORMAT_R16G16B16A16_SFLOAT:
		*surface_format = I915_GFX_SURFACE_R16G16B16A16_FLOAT;
		return 0;
	case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
		*surface_format = I915_GFX_SURFACE_R11G11B10_FLOAT;
		return 0;
	case VK_FORMAT_R32G32_SFLOAT:
		*surface_format = I915_GFX_SURFACE_R32G32_FLOAT;
		return 0;
	case VK_FORMAT_R32G32B32_SFLOAT:
		*surface_format = I915_GFX_SURFACE_R32G32B32_FLOAT;
		return 0;
	case VK_FORMAT_R32G32B32A32_SFLOAT:
		*surface_format = I915_GFX_SURFACE_R32G32B32A32_FLOAT;
		return 0;
	default:
		break;
	}

	/* The 32-bit integers, delivered to the vertex kernel bit for bit. */
	switch (format) {
	case VK_FORMAT_R32_SINT:
		*surface_format = I915_GFX_SURFACE_R32_SINT;
		return 0;
	case VK_FORMAT_R32_UINT:
		*surface_format = I915_GFX_SURFACE_R32_UINT;
		return 0;
	case VK_FORMAT_R32G32_SINT:
		*surface_format = I915_GFX_SURFACE_R32G32_SINT;
		return 0;
	case VK_FORMAT_R32G32_UINT:
		*surface_format = I915_GFX_SURFACE_R32G32_UINT;
		return 0;
	case VK_FORMAT_R32G32B32_SINT:
		*surface_format = I915_GFX_SURFACE_R32G32B32_SINT;
		return 0;
	case VK_FORMAT_R32G32B32_UINT:
		*surface_format = I915_GFX_SURFACE_R32G32B32_UINT;
		return 0;
	case VK_FORMAT_R32G32B32A32_SINT:
		*surface_format = I915_GFX_SURFACE_R32G32B32A32_SINT;
		return 0;
	case VK_FORMAT_R32G32B32A32_UINT:
		*surface_format = I915_GFX_SURFACE_R32G32B32A32_UINT;
		return 0;
	default:
		break;
	}

	/* Any other format. */
	return ENOTSUP;
}

/* Finds the vertex format of a VkFormat in the table; ENOTSUP for a format the fetcher is not given. */
static int
i915_vertex_format(
	uint32_t format,
	const struct i915_gfx_vertex_format **entry)
{
	uint32_t index;

	/* Looks the format up. */
	for (index = 0U; index < sizeof(i915_gfx_vertex_formats) / sizeof(i915_gfx_vertex_formats[0]); index++) {
		if (i915_gfx_vertex_formats[index].vk_format == format) {
			/* Succeeded: the format's entry. */
			*entry = &i915_gfx_vertex_formats[index];
			return 0;
		}
	}

	/* Any other format. */
	return ENOTSUP;
}

/* Translates a VkSamplerMipmapMode to the SAMPLER_STATE mip filter, as anv does; any other mode takes the nearest level. */
static uint32_t
i915_sampler_mip_filter(
	uint32_t mipmap_mode)
{
	/* A linear mode blends the two nearest levels. */
	if (mipmap_mode == VK_SAMPLER_MIPMAP_MODE_LINEAR)
		return GEN12_MIPFILTER_LINEAR;

	/* Succeeded: the nearest level. */
	return GEN12_MIPFILTER_NEAREST;
}

/*
 * Writes the RENDER_SURFACE_STATE of a linear image, as isl fills it
 * (isl_surface_state.c) for the view a range names: the levels
 * [base_level, base_level + level_count) and the layers [base_layer,
 * base_layer + layer_count) of a sampled image, or the one level and layer
 * (a 3D image's depth) a render target writes.
 *
 * The surface starts at level 0 of slice 0 with the level-0 extent; the
 * hardware finds every other level itself in the 2D mip layout (image.c),
 * from the alignment and the pitch, and every other slice from the QPitch.
 * A sampled surface is 2D (a 1D image is one texel high, and arrays set
 * Surface Array), 3D, or a cube for a cube view (all six faces enabled,
 * Depth the cubes less one); a render target is 2D, or 3D for a 3D image,
 * and writes level MIP Count at Minimum Array Element.  Mip Tail Start is
 * the image's level count (no mip tail, isl_choose_miptail_start_level()
 * for a linear surface).  Returns EINVAL for an image with no storage, a
 * format the surface state cannot name, or levels or layers the image does
 * not have.
 */
static int
i915_image_surface_write(
	uint32_t *rss,
	const struct i915_gfx_image *image,
	const struct i915_image_range *range,
	uint32_t mocs)
{
	uint64_t va;
	uint32_t format;
	uint32_t rows;
	uint32_t type;
	uint32_t depth;
	uint32_t array;
	uint32_t faces;
	uint32_t slices;
	uint32_t lod;
	uint32_t tile;
	uint32_t samples;
	int error;

	/* Refuses an image with no storage. */
	va = drv_i915_gfx_memory_va(image->memory, image->offset);
	if (va == 0U)
		return EINVAL;

	/*
	 * Refuses a format the surface state cannot name.  A depth image is
	 * sampled as the R32 float it holds, in the Y tiles it is laid out in
	 * (isl samples a D32 surface as R32_FLOAT).
	 */
	tile = GEN12_TILEMODE_LINEAR;
	if (image->format == VK_FORMAT_D32_SFLOAT || image->format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
		format = I915_GFX_SURFACE_R32_FLOAT;
		tile = GEN12_TILEMODE_YMAJOR;
	} else if (image->format == VK_FORMAT_D16_UNORM) {
		format = I915_GFX_SURFACE_R16_UNORM;
		tile = GEN12_TILEMODE_YMAJOR;
	} else if (range->format != 0U) {
		error = i915_surface_format(range->format, &format);
		if (error != 0)
			return EINVAL;
	} else {
		error = i915_surface_format(image->format, &format);
		if (error != 0)
			return EINVAL;
	}

	/* Refuses an empty range of levels, and one past the image's last level. */
	if (range->level_count == 0U || range->base_level >= image->levels)
		return EINVAL;
	if (range->level_count > image->levels - range->base_level)
		return EINVAL;

	/* Refuses an empty range of slices, and one past the image's last one. */
	slices = drv_i915_gfx_image_slices(image, 0U);
	if (range->layer_count == 0U || range->base_layer >= slices)
		return EINVAL;
	if (range->layer_count > slices - range->base_layer)
		return EINVAL;

	/*
	 * The QPitch is the rows from slice to slice, in units of four; a single
	 * slice counts its whole layout, which for a format with stencil ends
	 * where the stencil plane starts.
	 */
	rows = image->slice_rows;
	if (rows == 0U && image->stencil != 0U)
		rows = (uint32_t)(image->stencil_offset / image->pitch);
	if (rows == 0U)
		rows = (uint32_t)(image->bytes / image->pitch);
	rows = (rows + 3U) & ~3U;

	/* A 3D image is a 3D surface: its depth, and for a render target the one slice written. */
	type = GEN12_SURFTYPE_2D;
	depth = range->layer_count - 1U;
	array = 0U;
	faces = 0U;
	if (image->type == VK_IMAGE_TYPE_3D) {
		type = GEN12_SURFTYPE_3D;
		depth = slices - 1U;
	} else if (range->render_target == 0 &&
		   (range->view_type == VK_IMAGE_VIEW_TYPE_CUBE || range->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)) {
		/* A cube view samples a cube: six faces a cube, all enabled. */
		type = GEN12_SURFTYPE_CUBE;
		depth = range->layer_count / 6U - 1U;
		array = GEN12_RSS_SURFACE_ARRAY;
		faces = GEN12_RSS_CUBE_FACES_ALL;
	} else if (range->layer_count > 1U || range->view_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY ||
		   range->view_type == VK_IMAGE_VIEW_TYPE_2D_ARRAY) {
		/* An array view is a surface array. */
		array = GEN12_RSS_SURFACE_ARRAY;
	}

	/*
	 * A multisampled image is Y-tiled with its samples as slices, QPitch
	 * apart (MSFMT_MSS), which the surface reaches as an array (isl sets
	 * Surface Array for every 2D surface); a render target writes it as
	 * multisampled.
	 */
	samples = 0U;
	if (image->samples > 1U && range->render_target != 0) {
		tile = GEN12_TILEMODE_YMAJOR;
		array = GEN12_RSS_SURFACE_ARRAY;
		samples = drv_i915_gfx_samples_log2(image->samples) << GEN12_RSS_MULTISAMPLES_SHIFT;
	} else if (image->samples > 1U) {
		/*
		 * A shader that samples it (texelFetch of a sampler2DMS) reads the
		 * same slices as a single-sampled 2D array, one layer a sample:
		 * the compiler puts the sample in the ld message's r (spirv.c).
		 */
		tile = GEN12_TILEMODE_YMAJOR;
		array = GEN12_RSS_SURFACE_ARRAY;
		depth = image->samples - 1U;
	}

	/* A render target writes level MIP Count; a sampled surface reads from Surface Min LOD. */
	lod = (((range->level_count - 1U) & GEN12_RSS_LOD_MASK) << GEN12_RSS_MIP_COUNT_SHIFT) |
	    ((range->base_level & GEN12_RSS_LOD_MASK) << GEN12_RSS_SURFACE_MIN_LOD_SHIFT);
	if (range->render_target != 0)
		lod = (range->base_level & GEN12_RSS_LOD_MASK) << GEN12_RSS_MIP_COUNT_SHIFT;

	/*
	 * Fills the surface state: the type, the array bit, the format, the
	 * horizontal and vertical alignment 4, linear, the cube faces; the unorm
	 * path bit, MOCS and QPitch; width and height; the depth and the pitch;
	 * the first array element and a render target's extent; the mip count,
	 * the first level and the mip tail start; identity channel select; the
	 * address.
	 */
	kern_memset(rss, 0, GEN12_RENDER_SURFACE_STATE_DWORDS * 4U);
	rss[0] = (type << 29) |
	    array |
	    (format << 18) |
	    (GEN12_SURFACE_ALIGN_4 << 16) |
	    (GEN12_SURFACE_ALIGN_4 << 14) |
	    (tile << 12) |
	    faces;
	rss[1] = (1U << 31) | (mocs << 24) | ((rows / 4U) & GEN12_RSS_QPITCH_MASK);
	rss[2] = (image->width - 1U) | ((image->height - 1U) << 16);
	rss[3] = ((depth & GEN12_RSS_DEPTH_MASK) << GEN12_RSS_DEPTH_SHIFT) | (image->pitch - 1U);
	rss[4] = ((range->base_layer & GEN12_RSS_DEPTH_MASK) << GEN12_RSS_MIN_ARRAY_ELEMENT_SHIFT) | samples;
	if (range->render_target != 0) {
		if (type == GEN12_SURFTYPE_3D)
			depth = 0U;
		rss[4] |= (depth & GEN12_RSS_DEPTH_MASK) << GEN12_RSS_VIEW_EXTENT_SHIFT;
	}

	/* The levels, the mip tail start, the view's channel select (identity for a target) and the address. */
	rss[5] = lod | ((image->levels & GEN12_RSS_LOD_MASK) << GEN12_RSS_MIP_TAIL_START_SHIFT);
	rss[7] = (4U << 25) | (5U << 22) | (6U << 19) | (7U << 16);
	if (range->render_target == 0 && range->swizzle_set != 0U)
		rss[7] = range->channel_select;
	rss[8] = (uint32_t)va;
	rss[9] = (uint32_t)(va >> 32);

	/* Succeeded: the surface state describes the image. */
	return 0;
}

/*
 * Writes the binding table, the render target and texture surfaces and the
 * samplers of a draw.
 *
 * Binding table entry 0 is the first render target and entry 1 + n the n-th
 * sampled image of the pixel kernel, with sampler n, as the compiler numbers
 * them; each is the view and the sampler bound at the (set, binding) the
 * kernel names.  The other render targets are at I915_SHADER_RT_BTI().
 */
static int
i915_state_write_surfaces(
	uint32_t *surface,
	uint32_t *dynamic,
	const struct i915_gfx_draw_state *state,
	const struct i915_gfx_kernels *kernels,
	const struct i915_gfx_image *target,
	uint32_t mocs)
{
	const struct i915_gfx_dset *set;
	const struct i915_gfx_view *view;
	const struct i915_gfx_sampler *sampler;
	struct i915_image_range range;
	uint32_t texture;
	uint32_t set_index;
	uint32_t binding;
	uint32_t rss;
	uint32_t border;
	int error;
	const char *missing;

	/* Points the render targets' binding table entries at the levels and layers their views write. */
	error = i915_state_write_targets(surface, state, target, mocs);
	if (error != 0)
		return error;

	/* Refuses a kernel that samples more textures than the binding table has room for. */
	if (kernels->ps_samplers > I915_GFX_MAX_TEXTURES) {
		kern_logf("i915: vk: XXX unimplemented path: %u sampled images in one fragment shader\n", kernels->ps_samplers);
		return ENOTSUP;
	}

	/* Describes each texture the kernel samples, with its sampler. */
	for (texture = 0U; texture < kernels->ps_samplers; texture++) {
		/* The rectangle kernel's one texture is set 0 binding 0; a pipeline's kernel names each. */
		set_index = 0U;
		binding = 0U;
		if (kernels->ps_sampler_sets != NULL) {
			set_index = kernels->ps_sampler_sets[texture];
			binding = kernels->ps_sampler_bindings[texture];
		}

		/* The set the kernel names. */
		set = NULL;
		if (set_index < I915_GFX_BOUND_SETS && binding < I915_GFX_MAX_BINDINGS)
			set = state->dset[set_index];

		/*
		 * A uniform texel buffer: a buffer surface read with ld, which
		 * takes no sampler state (the heap's entry stays zero).
		 */
		if (set != NULL &&
		    set->slots[binding].view == NULL &&
		    set->slots[binding].texel != NULL) {
			rss = I915_GFX_RSS_TEXTURE + texture * I915_GFX_RSS_BYTES;
			surface[I915_GFX_BINDING_TABLE / 4U + 1U + texture] = rss;
			error = i915_buffer_surface_write(&surface[rss / 4U], set->slots[binding].texel, mocs);
			if (error != 0) {
				kern_logf("i915: vk: draw refused: set %u binding %u: texel buffer cannot be read (error %d)\n",
					  set_index,
					  binding,
					  error);
				return error;
			}
			continue;
		}

		/* Refuses a draw whose set and binding lack the view or the sampler. */
		if (set == NULL ||
		    set->slots[binding].view == NULL ||
		    set->slots[binding].sampler == NULL) {
			missing = "no sampler";
			if (set == NULL) {
				missing = "no set bound";
			} else if (set->slots[binding].view == NULL) {
				missing = "no view";
			}

			/* Says which of the three is missing (BUG-117). */
			kern_logf("i915: vk: draw refused: set %u binding %u has no image view and sampler (%s)\n",
				  set_index,
				  binding,
				  missing);
			return EINVAL;
		}

		/* Points the texture's binding table entry at its surface state and describes the levels its view shows. */
		view = set->slots[binding].view;
		sampler = set->slots[binding].sampler;
		rss = I915_GFX_RSS_TEXTURE + texture * I915_GFX_RSS_BYTES;
		surface[I915_GFX_BINDING_TABLE / 4U + 1U + texture] = rss;
		range.view_type = view->view_type;
		range.base_level = view->base_level;
		range.level_count = view->level_count;
		range.base_layer = view->base_layer;
		range.layer_count = view->layer_count;
		if (range.layer_count == 0U)
			range.layer_count = 1U;
		range.format = drv_i915_gfx_view_format(view);
		range.channel_select = view->channel_select;
		range.swizzle_set = view->swizzle_set;
		range.render_target = 0;
		error = i915_image_surface_write(&surface[rss / 4U], view->image, &range, mocs);
		if (error != 0)
			return error;

		/* Writes the texture's sampler and its border colour. */
		drv_i915_gfx_sampler_write(&dynamic[(I915_GFX_DYN_SAMPLER + texture * I915_GFX_SAMPLER_BYTES) / 4U], sampler);
		border = I915_GFX_DYN_BORDER + texture * I915_GFX_BORDER_BYTES;
		drv_i915_gfx_sampler_border_write(&dynamic[(I915_GFX_DYN_SAMPLER + texture * I915_GFX_SAMPLER_BYTES) / 4U],
						  &dynamic[border / 4U],
						  border,
						  sampler);
	}

	/* Succeeded: the target, the textures and the samplers are described. */
	return 0;
}

/* Finds the dynamic offset the bind of a set gave the dynamic uniform or storage buffer at a binding; 0 for none. */
static uint32_t
i915_state_dynamic_offset(
	const struct i915_gfx_draw_state *state,
	uint32_t set,
	uint32_t binding)
{
	uint32_t index;

	/* Looks the binding up among the set's dynamic buffers. */
	for (index = 0U; index < state->dynamic_count[set] && index < I915_GFX_MAX_DYNAMIC_BUFFERS; index++) {
		if (state->dynamic_bindings[set][index] == binding)
			return state->dynamic_offsets[set][index];
	}

	/* Succeeded: not a dynamic buffer, no offset. */
	return 0U;
}

/*
 * Finds what render target `slot` of the draw writes: the level and the
 * layer (a 3D image's slice) its colour attachment view starts at; level 0
 * of layer 0 when the draw has no framebuffer (a test's state).
 */
static void
i915_state_target_range(
	const struct i915_gfx_draw_state *state,
	uint32_t slot,
	struct i915_image_range *range)
{
	const struct i915_gfx_view *view;

	/* One level and one layer, of a render target. */
	kern_memset(range, 0, sizeof(*range));
	range->view_type = VK_IMAGE_VIEW_TYPE_2D;
	range->level_count = 1U;
	range->layer_count = 1U;
	range->render_target = 1;

	/* The colour attachment view names the level, the layer and how many layers a draw may reach. */
	view = i915_state_target_view(state, slot);
	if (view == NULL)
		return;
	range->base_level = view->base_level;
	range->base_layer = view->base_layer;
	range->format = drv_i915_gfx_view_format(view);

	/*
	 * A view of several layers is a layered target: the render target
	 * view extent covers them, and the layer a primitive goes to is its
	 * render target array index (gl_Layer).  A 3D image's view keeps its
	 * one slice.
	 */
	if (view->layer_count > 1U && view->image != NULL && view->image->type != VK_IMAGE_TYPE_3D)
		range->layer_count = view->layer_count;
}

/* Finds the view colour slot `slot` of the draw's subpass draws into; NULL for none. */
static const struct i915_gfx_view *
i915_state_target_view(
	const struct i915_gfx_draw_state *state,
	uint32_t slot)
{
	uint32_t attachment;

	/* A draw without a framebuffer (a test's) has none. */
	if (state->framebuffer == NULL || state->pass == NULL)
		return NULL;

	/* The slot's attachment, when the framebuffer has a view for it. */
	attachment = drv_i915_gfx_pass_color(state->pass, slot);
	if (attachment >= state->framebuffer->view_count)
		return NULL;

	/* Succeeded: the view, or NULL for an attachment without one. */
	return state->framebuffer->views[attachment];
}

/*
 * Writes the render targets: slot n's view at binding table entry
 * I915_SHADER_RT_BTI(n), and a null surface of the draw's extent for a slot
 * the subpass does not draw into (a fragment shader may still write that
 * location; the write goes nowhere, as isl's null surface state makes it).
 * A draw without a framebuffer (a test's) writes `target` as slot 0.
 */
static int
i915_state_write_targets(
	uint32_t *surface,
	const struct i915_gfx_draw_state *state,
	const struct i915_gfx_image *target,
	uint32_t mocs)
{
	const struct i915_gfx_view *view;
	struct i915_image_range range;
	uint32_t slot;
	uint32_t rss;
	int error;

	/* Every slot a fragment shader may write. */
	for (slot = 0U; slot < I915_SHADER_MAX_COLOR_OUTPUTS; slot++) {
		/* The slot's surface state and its binding table entry. */
		rss = I915_GFX_RSS_TARGET;
		if (slot != 0U)
			rss = I915_GFX_RSS_EXTRA_TARGET + (slot - 1U) * I915_GFX_RSS_BYTES;
		surface[I915_GFX_BINDING_TABLE / 4U + I915_SHADER_RT_BTI(slot)] = rss;

		/* The slot's view, or for a test's state the given target as slot 0. */
		view = i915_state_target_view(state, slot);
		i915_state_target_range(state, slot, &range);
		if (view != NULL) {
			error = i915_image_surface_write(&surface[rss / 4U], view->image, &range, mocs);
		} else if (slot == 0U && state->framebuffer == NULL) {
			error = i915_image_surface_write(&surface[rss / 4U], target, &range, mocs);
		} else {
			i915_state_null_surface_write(&surface[rss / 4U], target->width, target->height);
			error = 0;
		}

		/* Refuses a view the surface state cannot describe. */
		if (error != 0)
			return error;
	}

	/* Succeeded: every render target slot is described. */
	return 0;
}

/*
 * Writes the RENDER_SURFACE_STATE of a null surface: SURFTYPE_NULL,
 * B8G8R8A8_UNORM, Y-tiled, of the given extent (isl_null_fill_state() for
 * Gen9+).  Writes to it are dropped.
 */
static void
i915_state_null_surface_write(
	uint32_t *rss,
	uint32_t width,
	uint32_t height)
{
	/* A null surface still names an extent of at least one texel. */
	if (width == 0U)
		width = 1U;
	if (height == 0U)
		height = 1U;

	/* The type, the format, the alignment and the tiling; the extent. */
	kern_memset(rss, 0, GEN12_RENDER_SURFACE_STATE_DWORDS * 4U);
	rss[0] = (GEN12_SURFTYPE_NULL << 29) |
	    (I915_GFX_SURFACE_B8G8R8A8_UNORM << 18) |
	    (GEN12_SURFACE_ALIGN_4 << 16) |
	    (GEN12_SURFACE_ALIGN_4 << 14) |
	    (GEN12_TILEMODE_YMAJOR << 12);
	rss[2] = (width - 1U) | ((height - 1U) << 16);
}

/*
 * Fills one stage's push data: the push constants the kernel reads from the
 * start of the command buffer's block, then each uniform block's range
 * from the buffer bound at its set and binding, moved by the bind's dynamic
 * offset for a dynamic uniform buffer (what lies past the bound range reads
 * as zero).  Returns EINVAL for a layout larger than the buffer or a
 * uniform block that is not bound to storage.
 */
static int
i915_state_write_push(
	uint8_t *data,
	const struct i915_gfx_draw_state *state,
	const struct i915_gfx_push_layout *layout)
{
	const struct i915_shader_block *block;
	const struct i915_gfx_dset *set;
	const struct i915_gfx_buffer *buffer;
	const uint8_t *source;
	uint64_t descriptor_offset;
	uint64_t range;
	uint64_t start;
	uint64_t bytes;
	uint64_t va;
	uint32_t constant_bytes;
	uint32_t index;
	uint32_t words[3];

	/* Refuses push data larger than its buffer. */
	if (layout->regs * 32U > I915_GFX_PUSH_DATA_BYTES)
		return EINVAL;

	/* Copies the push constants, never more than the command buffer carries. */
	constant_bytes = layout->constant_bytes;
	if (constant_bytes > I915_GFX_PUSH_BYTES)
		constant_bytes = I915_GFX_PUSH_BYTES;
	kern_memcpy(data, state->push, constant_bytes);

	/* Copies each uniform block's range after them. */
	for (index = 0U; index < layout->block_count; index++) {
		block = &layout->blocks[index];

		/* Refuses a block laid out past the buffer. */
		if (block->push_offset + block->bytes > I915_GFX_PUSH_DATA_BYTES)
			return EINVAL;

		/* The system storage buffer of a compute kernel (the group counts) is the dispatch's to fill. */
		if (block->set == I915_IR_SYSTEM_SET)
			continue;

		/* Finds the buffer bound at the block's set and binding. */
		set = NULL;
		if (block->set < I915_GFX_BOUND_SETS && block->binding < I915_GFX_MAX_BINDINGS)
			set = state->dset[block->set];
		buffer = NULL;
		if (set != NULL)
			buffer = set->slots[block->binding].buffer;
		if (buffer == NULL) {
			kern_logf("i915: vk: draw refused: set %u binding %u has no uniform or storage buffer\n", block->set, block->binding);
			return EINVAL;
		}

		/* The descriptor's offset, moved by the bind's dynamic offset for a dynamic uniform or storage buffer. */
		descriptor_offset = set->slots[block->binding].offset;
		if (set->slots[block->binding].dynamic != 0)
			descriptor_offset += i915_state_dynamic_offset(state, block->set, block->binding);

		/* The descriptor's range: to the buffer's end for VK_WHOLE_SIZE, nothing past it. */
		range = set->slots[block->binding].range;
		if (descriptor_offset >= buffer->size) {
			range = 0U;
		} else if (range == VK_WHOLE_SIZE || range > buffer->size - descriptor_offset) {
			range = buffer->size - descriptor_offset;
		}

		/*
		 * A storage buffer is delivered as the GPU address of its range, the
		 * low word first, and the range's bytes after it, which an
		 * OpArrayLength reads (ws101-p004).
		 */
		if (block->address != 0U) {
			va = drv_i915_gfx_memory_va(buffer->memory, buffer->offset + descriptor_offset);
			if (va == 0U)
				return EINVAL;
			words[0] = (uint32_t)va;
			words[1] = (uint32_t)(va >> 32);
			words[2] = (uint32_t)range;
			if (range > 0xFFFFFFFFULL)
				words[2] = 0xFFFFFFFFU;
			kern_memcpy(data + block->push_offset, words, sizeof(words));
			continue;
		}

		/* A block read wholly past the range reads zeros only. */
		if (block->offset >= range)
			continue;

		/* The bytes read that the range holds. */
		start = descriptor_offset + block->offset;
		bytes = range - block->offset;
		if (bytes > block->bytes)
			bytes = block->bytes;

		/* Copies the range from the buffer's storage. */
		source = drv_i915_gfx_memory_cpu(buffer->memory, buffer->offset + start, bytes);
		if (source == NULL)
			return EINVAL;
		kern_memcpy(data + block->push_offset, source, (size_t)bytes);
	}

	/* Succeeded: the stage's push data is in place. */
	return 0;
}

/*
 * Chooses the viewport and the scissor of a draw: the pipeline's own, or
 * the ones the command buffer set for a pipeline that declares them
 * dynamic; EINVAL when dynamic state was never set.
 */
static int
i915_state_viewport_source(
	const struct i915_gfx_draw_state *state,
	const uint32_t **viewport,
	const VkRect2D **scissor)
{
	const struct i915_gfx_pipeline *pipeline;

	/* Starts from the pipeline's own viewport and scissor. */
	pipeline = state->pipeline;
	*viewport = pipeline->viewport;
	*scissor = &pipeline->scissor;

	/* A dynamic viewport is the one vkCmdSetViewport recorded. */
	if (pipeline->dynamic_viewport != 0) {
		/* Refuses a draw that comes before any vkCmdSetViewport. */
		if (state->viewport_set == 0) {
			kern_logf("i915: vk: draw refused: the pipeline's viewport is dynamic and no viewport was set\n");
			return EINVAL;
		}

		*viewport = state->viewport;
	}

	/* A dynamic scissor is the one vkCmdSetScissor recorded. */
	if (pipeline->dynamic_scissor != 0) {
		/* Refuses a draw that comes before any vkCmdSetScissor. */
		if (state->scissor_set == 0) {
			kern_logf("i915: vk: draw refused: the pipeline's scissor is dynamic and no scissor was set\n");
			return EINVAL;
		}

		*scissor = &state->scissor;
	}

	/* Succeeded: the draw has its viewport and its scissor. */
	return 0;
}

/*
 * Writes the viewports and the scissor of a draw.
 *
 * The viewport is x, y, width, height, minDepth and maxDepth as float bits.
 * A negative height (VK_KHR_maintenance1) flips y: the transform takes it
 * as it is, and the viewport's rectangle runs from y + height to y, as anv
 * writes it (genX_cmd_buffer.c, the viewport's y_min and y_max).
 * XXX: the guardband is the viewport itself ([-1, 1] in NDC): correct, and
 * every primitive that leaves the viewport is clipped rather than trivially
 * accepted.
 */
static void
i915_state_write_viewport(
	uint32_t *dynamic,
	const uint32_t *viewport,
	const VkRect2D *scissor)
{
	uint32_t *words;
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	uint32_t half_width;
	uint32_t half_height;
	uint32_t right_edge;
	uint32_t top_edge;
	uint32_t bottom_edge;

	/* Gives CC_VIEWPORT the depth range. */
	dynamic[I915_GFX_DYN_CC_VIEWPORT / 4U] = viewport[4];
	dynamic[I915_GFX_DYN_CC_VIEWPORT / 4U + 1U] = viewport[5];

	/* Takes the viewport rectangle and its half extent. */
	x = viewport[0];
	y = viewport[1];
	width = viewport[2];
	height = viewport[3];
	half_width = drv_i915_float_half(width);
	half_height = drv_i915_float_half(height);

	/* Finds the far edges of the viewport, one pixel short of x + width and y + height. */
	right_edge = drv_i915_float_add(x, width);
	right_edge = drv_i915_float_sub(right_edge, I915_FLOAT_ONE);
	top_edge = y;
	bottom_edge = drv_i915_float_add(y, height);
	bottom_edge = drv_i915_float_sub(bottom_edge, I915_FLOAT_ONE);

	/* A negative height (its sign bit) runs up from y: the rectangle is y + height to one pixel short of y. */
	if ((height & I915_FLOAT_SIGN) != 0U) {
		top_edge = drv_i915_float_add(y, height);
		bottom_edge = drv_i915_float_sub(y, I915_FLOAT_ONE);
	}

	/*
	 * Fills SF_CLIP_VIEWPORT: the transform m00 m11 m22 m30 m31 m32, two
	 * reserved words, the guardband x- x+ y- y+ and the viewport x- x+ y- y+.
	 */
	words = &dynamic[I915_GFX_DYN_SF_CLIP_VIEWPORT / 4U];
	words[0] = half_width;
	words[1] = half_height;
	words[2] = drv_i915_float_sub(viewport[5], viewport[4]);
	words[3] = drv_i915_float_add(x, half_width);
	words[4] = drv_i915_float_add(y, half_height);
	words[5] = viewport[4];
	words[8] = I915_FLOAT_MINUS_ONE;
	words[9] = I915_FLOAT_ONE;
	words[10] = I915_FLOAT_MINUS_ONE;
	words[11] = I915_FLOAT_ONE;
	words[12] = x;
	words[13] = right_edge;
	words[14] = top_edge;
	words[15] = bottom_edge;

	/* Fills SCISSOR_RECT with the inclusive corners of the scissor. */
	words = &dynamic[I915_GFX_DYN_SCISSOR / 4U];
	words[0] = ((uint32_t)scissor->offset.x & 0xffffU) |
	    (((uint32_t)scissor->offset.y & 0xffffU) << 16);
	words[1] = (((uint32_t)scissor->offset.x + scissor->extent.width - 1U) & 0xffffU) |
	    ((((uint32_t)scissor->offset.y + scissor->extent.height - 1U) & 0xffffU) << 16);
}

/*
 * Writes BLEND_STATE with the entry of attachment 0 and the blend constants
 * of COLOR_CALC_STATE.
 *
 * Every entry clamps before and after blending to the render target's
 * format, as anv programs it; the entry blends as the pipeline says and
 * leaves the components the pipeline masks unwritten.  The blend constants
 * are the pipeline's, or the ones the command buffer set when the pipeline
 * declares them dynamic (zero until one is set, which Vulkan leaves
 * undefined).
 */
static void
i915_state_write_blend(
	uint32_t *dynamic,
	const struct i915_gfx_draw_state *state,
	const struct i915_gfx_image *target)
{
	const struct i915_gfx_pipeline *pipeline;
	struct i915_gfx_blend equation;
	const uint32_t *constants;
	uint32_t *words;
	uint32_t entry;
	uint32_t index;
	uint32_t slot;
	uint32_t disable;
	int integer;
	int blends;

	/* Works out the hardware blend of attachment 0. */
	pipeline = state->pipeline;
	i915_blend_equation(pipeline, &equation);

	/* A blending entry names its factors and functions. */
	entry = 0U;
	if (equation.enable != 0U) {
		entry |= GEN12_BLEND_ENABLE |
		    (equation.src_color << GEN12_BLEND_SRC_FACTOR_SHIFT) |
		    (equation.dst_color << GEN12_BLEND_DST_FACTOR_SHIFT) |
		    (equation.color_function << GEN12_BLEND_COLOR_FUNCTION_SHIFT) |
		    (equation.src_alpha << GEN12_BLEND_SRC_ALPHA_FACTOR_SHIFT) |
		    (equation.dst_alpha << GEN12_BLEND_DST_ALPHA_FACTOR_SHIFT) |
		    (equation.alpha_function << GEN12_BLEND_ALPHA_FUNCTION_SHIFT);
	}

	/*
	 * Fills BLEND_STATE: the independent alpha, then an entry and its clamps
	 * for every render target (the pipeline's one equation for all of them,
	 * as without independent blending), except that an integer target does
	 * not blend.
	 */
	words = &dynamic[I915_GFX_DYN_BLEND / 4U];
	words[0] = 0U;
	if (equation.independent_alpha != 0U)
		words[0] = GEN12_BLEND_INDEPENDENT_ALPHA;
	for (slot = 0U; slot < I915_SHADER_MAX_COLOR_OUTPUTS; slot++) {
		/* The slot's own write mask; the blend only where the slot blends. */
		disable = pipeline->color_write_disable;
		blends = 1;
		if (slot != 0U) {
			disable = pipeline->extra_write_disable[slot - 1U];
			if (pipeline->extra_blend_off[slot - 1U] != 0U)
				blends = 0;
		}

		/* An integer target never blends. */
		integer = i915_state_target_integer(state, slot);
		if (integer != 0)
			blends = 0;
		words[1U + 2U * slot] = i915_blend_write_disables(disable);
		if (blends != 0)
			words[1U + 2U * slot] |= entry;
		words[2U + 2U * slot] = 1U | (1U << 1) | (GEN12_COLORCLAMP_RTFORMAT << 2);

		/*
		 * The logic operation, on every target that takes one whether it
		 * blends or not, an integer one too; a float or an sRGB target
		 * passes the colour through (Vulkan, VkLogicOp; anv).
		 */
		if (pipeline->logic_op_enable != 0U &&
		    i915_state_format_takes_logic_op(i915_state_target_format(state, slot, target)) != 0)
			words[2U + 2U * slot] |= GEN12_BLEND_LOGIC_OP_ENABLE |
			    (i915_blend_logic_op(pipeline->logic_op) << GEN12_BLEND_LOGIC_OP_FUNCTION_SHIFT);
	}

	/* Takes the pipeline's blend constants, or the dynamic ones vkCmdSetBlendConstants set. */
	constants = pipeline->blend_constants;
	if (pipeline->dynamic_blend_constants != 0) {
		/* Names a draw before any vkCmdSetBlendConstants, whose constants Vulkan leaves undefined: it reads zeros. */
		if (state->blend_constants_set == 0)
			kern_logf("i915: vk: the pipeline's blend constants are dynamic and none were set; the draw blends with zeros\n");
		constants = state->blend_constants;
	}

	/* Gives COLOR_CALC_STATE the blend constants a constant factor reads. */
	words = &dynamic[I915_GFX_DYN_COLOR_CALC / 4U];
	for (index = 0U; index < 4U; index++)
		words[GEN12_CC_BLEND_CONSTANT_DWORD + index] = constants[index];
}

/*
 * Works out the hardware blend of attachment 0, as anv does
 * (genX_gfx_state.c): MIN and MAX use no factors, so theirs are ONE (the
 * hardware applies the factors whatever the function), and the alpha
 * blends independently when its factors or its function differ from the
 * colour's.  An equation that reads a second colour source is not blended
 * unless the fragment kernel writes one, as anv does.
 */
static void
i915_blend_equation(
	const struct i915_gfx_pipeline *pipeline,
	struct i915_gfx_blend *equation)
{
	int second;

	/* Starts with blending off, which is all a pipeline without blending, or with a logic operation, asks for. */
	kern_memset(equation, 0, sizeof(*equation));
	if (pipeline->blend_enable == 0U || pipeline->logic_op_enable != 0U)
		return;

	/* Translates the colour's factors and function. */
	equation->color_function = i915_blend_function(pipeline->blend_color_op);
	equation->src_color = i915_blend_factor(pipeline->blend_src_color);
	equation->dst_color = i915_blend_factor(pipeline->blend_dst_color);

	/* MIN and MAX of the colour take ONE, so the factors leave the values as they are. */
	if (pipeline->blend_color_op == VK_BLEND_OP_MIN || pipeline->blend_color_op == VK_BLEND_OP_MAX) {
		equation->src_color = GEN12_BLENDFACTOR_ONE;
		equation->dst_color = GEN12_BLENDFACTOR_ONE;
	}

	/* Translates the alpha's factors and function. */
	equation->alpha_function = i915_blend_function(pipeline->blend_alpha_op);
	equation->src_alpha = i915_blend_factor(pipeline->blend_src_alpha);
	equation->dst_alpha = i915_blend_factor(pipeline->blend_dst_alpha);

	/* MIN and MAX of the alpha take ONE the same way. */
	if (pipeline->blend_alpha_op == VK_BLEND_OP_MIN || pipeline->blend_alpha_op == VK_BLEND_OP_MAX) {
		equation->src_alpha = GEN12_BLENDFACTOR_ONE;
		equation->dst_alpha = GEN12_BLENDFACTOR_ONE;
	}

	/* Notes an equation that reads the second colour source of a dual-source write. */
	second = 0;
	if (i915_blend_uses_second_source(pipeline->blend_src_color) != 0) {
		second = 1;
	} else if (i915_blend_uses_second_source(pipeline->blend_dst_color) != 0) {
		second = 1;
	} else if (i915_blend_uses_second_source(pipeline->blend_src_alpha) != 0) {
		second = 1;
	} else if (i915_blend_uses_second_source(pipeline->blend_dst_alpha) != 0) {
		second = 1;
	}

	/* An equation of the second source blends only when the fragment kernel writes one (dual source, ws031-p032). */
	if (second != 0 && pipeline->dual_source == 0U) {
		kern_memset(equation, 0, sizeof(*equation));
		return;
	}

	/* The alpha blends independently when anything of it differs from the colour. */
	if (pipeline->blend_src_color != pipeline->blend_src_alpha) {
		equation->independent_alpha = 1U;
	} else if (pipeline->blend_dst_color != pipeline->blend_dst_alpha) {
		equation->independent_alpha = 1U;
	} else if (pipeline->blend_color_op != pipeline->blend_alpha_op) {
		equation->independent_alpha = 1U;
	}

	/* Succeeded: the colour buffer blends. */
	equation->enable = 1U;
}

/* Translates a VkBlendFactor to its BLENDFACTOR; a value past the table is ONE. */
static uint32_t
i915_blend_factor(
	uint32_t factor)
{
	/* A factor the table does not know blends as ONE. */
	if (factor >= sizeof(i915_gfx_blend_factors) / sizeof(i915_gfx_blend_factors[0]))
		return GEN12_BLENDFACTOR_ONE;

	/* Succeeded: the hardware factor. */
	return i915_gfx_blend_factors[factor];
}

/* Translates a VkBlendOp to its BLENDFUNCTION; an operation past the table adds. */
static uint32_t
i915_blend_function(
	uint32_t op)
{
	/* An operation the table does not know adds. */
	if (op >= sizeof(i915_gfx_blend_functions) / sizeof(i915_gfx_blend_functions[0]))
		return GEN12_BLENDFUNCTION_ADD;

	/* Succeeded: the hardware function. */
	return i915_gfx_blend_functions[op];
}

/* Reports whether a VkBlendFactor reads the second colour source of a dual-source write. */
static int
i915_blend_uses_second_source(
	uint32_t factor)
{
	/* The four factors of the second source. */
	if (factor >= VK_BLEND_FACTOR_SRC1_COLOR && factor <= VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA)
		return 1;

	/* Succeeded: any other factor reads the first source only. */
	return 0;
}

/*
 * Translates a VkLogicOp to its 3D_Logic_Op_Function (Mesa 25.0.7 anv
 * vk_to_intel_logic_op, values of genxml gen40.xml); a value past the
 * table copies.
 */
static uint32_t
i915_blend_logic_op(
	uint32_t op)
{
	static const uint8_t functions[16] = {
		0U,	/* CLEAR */
		8U,	/* AND */
		4U,	/* AND_REVERSE */
		12U,	/* COPY */
		2U,	/* AND_INVERTED */
		10U,	/* NO_OP */
		6U,	/* XOR */
		14U,	/* OR */
		1U,	/* NOR */
		9U,	/* EQUIVALENT */
		5U,	/* INVERT */
		13U,	/* OR_REVERSE */
		3U,	/* COPY_INVERTED */
		11U,	/* OR_INVERTED */
		7U,	/* NAND */
		15U	/* SET */
	};

	/* An operation the table does not know copies. */
	if (op >= sizeof(functions))
		return 12U;

	/* Succeeded: the hardware function. */
	return functions[op];
}

/*
 * Gives the VkFormat colour slot `slot` of the draw writes: its view's (as
 * the view reads the texels), or for a test's state without a framebuffer
 * the given target's as slot 0; 0 for a slot without a target.
 */
static uint32_t
i915_state_target_format(
	const struct i915_gfx_draw_state *state,
	uint32_t slot,
	const struct i915_gfx_image *target)
{
	const struct i915_gfx_view *view;

	/* The slot's view. */
	view = i915_state_target_view(state, slot);
	if (view != NULL)
		return drv_i915_gfx_view_format(view);

	/* A test's state draws slot 0 into the target it gives. */
	if (slot == 0U && state->framebuffer == NULL && target != NULL)
		return target->format;

	/* No target. */
	return 0U;
}

/* Tells whether a logic operation applies to a target of a VkFormat: not to a float or an sRGB one, nor to none. */
static int
i915_state_format_takes_logic_op(
	uint32_t format)
{
	/* The float and sRGB formats the executor draws into, and no format. */
	switch (format) {
	case 0U:
	case VK_FORMAT_R32G32B32A32_SFLOAT:
	case VK_FORMAT_R16G16B16A16_SFLOAT:
	case VK_FORMAT_R32_SFLOAT:
	case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
	case VK_FORMAT_R8G8B8A8_SRGB:
	case VK_FORMAT_B8G8R8A8_SRGB:
		return 0;
	default:
		/* Succeeded: a normalized or an integer format. */
		return 1;
	}
}

/*
 * Returns the VUE slot after the position fragment input `input` is read
 * from: the one the pipeline routed it from, or for a rectangle kernel the
 * input's own number; an input past the kernel's `inputs` takes slot 0.
 */
static uint32_t
i915_state_input_slot(
	const struct i915_gfx_kernels *kernels,
	uint32_t inputs,
	uint32_t input)
{
	/* An input the kernel does not read takes slot 0. */
	if (input >= inputs)
		return 0U;

	/* A rectangle kernel reads its slots in order. */
	if (kernels->ps_inputs_mapped == 0U)
		return input;

	/* Succeeded: the slot the pipeline routed the input from. */
	return kernels->ps_input_slots[input];
}

/* Reports 1 when the pipeline's vertex binding `binding` advances per instance, 0 otherwise. */
static uint32_t
i915_state_binding_instanced(
	const struct i915_gfx_pipeline *pipeline,
	uint32_t binding)
{
	uint32_t index;

	/* Finds the binding among the pipeline's. */
	for (index = 0U; index < pipeline->binding_count; index++) {
		if (pipeline->bindings[index].binding != binding)
			continue;

		/* An instance-rate binding steps per instance. */
		if (pipeline->bindings[index].input_rate == VK_VERTEX_INPUT_RATE_INSTANCE)
			return 1U;
		return 0U;
	}

	/* Succeeded: a binding the pipeline does not describe steps per vertex. */
	return 0U;
}

/* Reports 1 when colour slot `slot` of the draw draws into an integer format, which does not blend. */
static int
i915_state_target_integer(
	const struct i915_gfx_draw_state *state,
	uint32_t slot)
{
	const struct i915_gfx_view *view;

	/* A slot without a view blends nothing either way. */
	view = i915_state_target_view(state, slot);
	if (view == NULL)
		return 0;

	/* The integer formats the executor lays out, as the view reads the texels. */
	switch (drv_i915_gfx_view_format(view)) {
	case VK_FORMAT_R8G8B8A8_UINT:
	case VK_FORMAT_R8G8B8A8_SINT:
	case VK_FORMAT_R32_UINT:
	case VK_FORMAT_R32_SINT:
		return 1;
	default:
		/* Succeeded: a normalized or float format blends. */
		return 0;
	}
}

/* Reports a BLEND_STATE entry's write disable bits for the components a VkColorComponentFlags complement names. */
static uint32_t
i915_blend_write_disables(
	uint32_t disable)
{
	uint32_t bits;

	/* One bit a component. */
	bits = 0U;
	if ((disable & VK_COLOR_COMPONENT_R_BIT) != 0U)
		bits |= GEN12_BLEND_WRITE_DISABLE_RED;
	if ((disable & VK_COLOR_COMPONENT_G_BIT) != 0U)
		bits |= GEN12_BLEND_WRITE_DISABLE_GREEN;
	if ((disable & VK_COLOR_COMPONENT_B_BIT) != 0U)
		bits |= GEN12_BLEND_WRITE_DISABLE_BLUE;
	if ((disable & VK_COLOR_COMPONENT_A_BIT) != 0U)
		bits |= GEN12_BLEND_WRITE_DISABLE_ALPHA;

	/* Succeeded: the entry's disable bits. */
	return bits;
}

/*
 * The 3D_Stencil_Operation of each VkStencilOp: KEEP, ZERO, REPLACE,
 * INCRSAT, DECRSAT, INVERT (7), INCR (5) and DECR (6).
 */
static const uint32_t i915_gfx_stencil_ops[8] = {
	0U,
	1U,
	2U,
	3U,
	4U,
	7U,
	5U,
	6U,
};

/*
 * Adds the stencil test to 3DSTATE_WM_DEPTH_STENCIL's words, both faces
 * (double-sided): write and test enable, the functions and operations in
 * dword 1, the test and write masks in dword 2, the references in dword 3.
 * A dynamic mask or reference is the one vkCmdSetStencil* recorded.
 */
static void
i915_state_stencil(
	const struct i915_gfx_draw_state *state,
	uint32_t *depth_state,
	uint32_t *masks,
	uint32_t *references)
{
	const struct i915_gfx_pipeline *pipeline;
	uint32_t compare[2];
	uint32_t write[2];
	uint32_t reference[2];
	uint32_t face;

	/* The masks and references, from the pipeline or the command buffer. */
	pipeline = state->pipeline;
	for (face = 0U; face < 2U; face++) {
		compare[face] = pipeline->stencil_compare_mask[face];
		if (pipeline->dynamic_stencil_compare != 0)
			compare[face] = state->stencil_compare_mask[face];
		write[face] = pipeline->stencil_write_mask[face];
		if (pipeline->dynamic_stencil_write != 0)
			write[face] = state->stencil_write_mask[face];
		reference[face] = pipeline->stencil_reference[face];
		if (pipeline->dynamic_stencil_reference != 0)
			reference[face] = state->stencil_reference[face];
	}

	/* Write and test enable, double-sided; the front face's function and operations, then the back face's. */
	*depth_state |= (1U << 2) | (1U << 3) | (1U << 4) |
	    (i915_gfx_compare_functions[pipeline->stencil_compare[0] & 7U] << 8) |
	    (i915_gfx_stencil_ops[pipeline->stencil_pass[1] & 7U] << 11) |
	    (i915_gfx_stencil_ops[pipeline->stencil_depth_fail[1] & 7U] << 14) |
	    (i915_gfx_stencil_ops[pipeline->stencil_fail[1] & 7U] << 17) |
	    (i915_gfx_compare_functions[pipeline->stencil_compare[1] & 7U] << 20) |
	    (i915_gfx_stencil_ops[pipeline->stencil_pass[0] & 7U] << 23) |
	    (i915_gfx_stencil_ops[pipeline->stencil_depth_fail[0] & 7U] << 26) |
	    (i915_gfx_stencil_ops[pipeline->stencil_fail[0] & 7U] << 29);

	/* The back write and test masks, then the front ones; the back reference, then the front one. */
	*masks = (write[1] & 0xffU) | ((compare[1] & 0xffU) << 8) | ((write[0] & 0xffU) << 16) | ((compare[0] & 0xffU) << 24);
	*references = (reference[1] & 0xffU) | ((reference[0] & 0xffU) << 8);
}

/*
 * Emits 3DSTATE_STENCIL_BUFFER: for an attachment with stencil its stencil
 * plane (2D, write enable, the pitch; the address; the extent; MOCS, the
 * layer the depth view writes and the layers less one; Y tiles; the
 * QPitch), a null buffer otherwise.  Returns EINVAL for a plane with no
 * storage.
 */
static int
i915_state_stencil_buffer(
	struct i915_gfx_batch *batch,
	const struct i915_gfx_draw_state *state,
	const struct i915_gfx_image *image,
	uint32_t mocs)
{
	uint64_t va;
	uint32_t layer;
	uint32_t layers;
	uint32_t index;

	/* A null stencil buffer when the attachment has none. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_STENCIL_BUFFER, GEN12_3DSTATE_STENCIL_BUFFER_DWORDS));
	if (image == NULL || image->stencil == 0U) {
		drv_i915_batch_emit(batch, GEN12_SURFTYPE_NULL << 29);
		for (index = 2U; index < GEN12_3DSTATE_STENCIL_BUFFER_DWORDS; index++)
			drv_i915_batch_emit(batch, 0U);
		return 0;
	}

	/* The plane's address. */
	va = drv_i915_gfx_memory_va(image->memory, image->offset + image->stencil_offset);
	if (va == 0U)
		return EINVAL;

	/* The first layer the pass's depth view writes, and how many. */
	layer = 0U;
	layers = 1U;
	if (state->framebuffer != NULL && state->pass != NULL &&
	    state->pass->depth_attachment < state->framebuffer->view_count &&
	    state->framebuffer->views[state->pass->depth_attachment] != NULL) {
		layer = state->framebuffer->views[state->pass->depth_attachment]->base_layer;
		if (state->framebuffer->views[state->pass->depth_attachment]->layer_count > 1U)
			layers = state->framebuffer->views[state->pass->depth_attachment]->layer_count;
	}

	/*
	 * The plane: its first array element and depth are the view's layers,
	 * and the render target view extent the depth again, as for the depth
	 * buffer (gen120.xml 3DSTATE_STENCIL_BUFFER, bits 245-255).
	 */
	drv_i915_batch_emit(batch, (GEN12_SURFTYPE_2D << 29) | (1U << 28) | (image->stencil_pitch - 1U));
	drv_i915_batch_emit(batch, (uint32_t)va);
	drv_i915_batch_emit(batch, (uint32_t)(va >> 32));
	drv_i915_batch_emit(batch, ((image->width - 1U) << 1) | ((image->height - 1U) << 17));
	drv_i915_batch_emit(batch, mocs | ((layer & GEN12_RSS_DEPTH_MASK) << 8) |
	    (((layers - 1U) & GEN12_RSS_DEPTH_MASK) << 20));
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, (image->stencil_slice_rows / 4U) | (((layers - 1U) & GEN12_RSS_DEPTH_MASK) << 21));

	/* Succeeded: the stencil buffer is described. */
	return 0;
}

/*
 * Writes the RENDER_SURFACE_STATE of a uniform texel buffer, as isl fills
 * it for a buffer (isl_buffer_fill_state_s): SURFTYPE_BUFFER of the view's
 * format, linear, the element count less one spread over width (7 bits),
 * height (14) and depth (11), the texel size less one as the pitch, the
 * identity channel select and the first byte's address.  Refuses a view
 * whose buffer is not bound, whose range runs past the buffer, or that
 * holds no whole texel.
 */
static int
i915_buffer_surface_write(
	uint32_t *rss,
	const struct i915_gfx_buffer_view *view,
	uint32_t mocs)
{
	const struct i915_gfx_buffer *buffer;
	uint64_t size;
	uint64_t elements;
	uint64_t va;
	uint32_t format;
	uint32_t bytes;
	uint32_t last;
	int error;

	/* The buffer must be bound, and the view must start inside it. */
	buffer = view->buffer;
	if (buffer == NULL || buffer->memory == NULL)
		return EINVAL;
	if (view->offset > buffer->size)
		return EINVAL;

	/* The surface format and the texel size of the view's format. */
	error = drv_i915_gfx_texel_buffer_format(view->format, &format, &bytes);
	if (error != 0)
		return error;

	/* The bytes the view reads: to the buffer's end for VK_WHOLE_SIZE, and never past it. */
	size = buffer->size - view->offset;
	if (view->range != VK_WHOLE_SIZE) {
		if (view->range > size)
			return EINVAL;
		size = view->range;
	}

	/* Whole texels only, at most 2^27 of them (the device's limit). */
	elements = size / bytes;
	if (elements == 0U)
		return EINVAL;
	if (elements > (1U << 27))
		elements = 1U << 27;
	last = (uint32_t)(elements - 1U);

	/* The first texel's address. */
	va = drv_i915_gfx_memory_va(buffer->memory, buffer->offset + view->offset);

	/* Fills the surface state. */
	kern_memset(rss, 0, GEN12_RENDER_SURFACE_STATE_DWORDS * 4U);
	rss[0] = (GEN12_SURFTYPE_BUFFER << 29) |
	    (format << 18) |
	    (GEN12_SURFACE_ALIGN_4 << 16) |
	    (GEN12_SURFACE_ALIGN_4 << 14) |
	    (GEN12_TILEMODE_LINEAR << 12);
	rss[1] = mocs << 24;
	rss[2] = (last & 0x7fU) | (((last >> 7) & 0x3fffU) << 16);
	rss[3] = (((last >> 21) & 0x7ffU) << 21) | (bytes - 1U);
	rss[7] = (4U << 25) | (5U << 22) | (6U << 19) | (7U << 16);
	rss[8] = (uint32_t)va;
	rss[9] = (uint32_t)(va >> 32);

	/* Succeeded: the surface state describes the texels. */
	return 0;
}
