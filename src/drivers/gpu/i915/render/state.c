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
#define I915_GFX_SURFACE_R8G8B8A8_UNORM		0x0c7U
#define I915_GFX_SURFACE_B8G8R8A8_UNORM		0x0c0U
#define I915_GFX_SURFACE_R32_FLOAT		0x0d8U
#define I915_GFX_SURFACE_R32G32_FLOAT		0x085U
#define I915_GFX_SURFACE_R32G32B32_FLOAT	0x040U
#define I915_GFX_SURFACE_R32G32B32A32_FLOAT	0x000U

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

static int i915_surface_format(uint32_t format, uint32_t *surface_format);
static uint32_t i915_format_components(uint32_t format);
static int i915_image_surface_write(uint32_t *rss, const struct i915_gfx_image *image, uint32_t base_level, uint32_t level_count, uint32_t mocs);
static uint32_t i915_sampler_mip_filter(uint32_t mipmap_mode);
static int i915_state_write_surfaces(uint32_t *surface, uint32_t *dynamic, const struct i915_gfx_draw_state *state, const struct i915_gfx_kernels *kernels, const struct i915_gfx_image *target, uint32_t mocs);
static int i915_state_viewport_source(const struct i915_gfx_draw_state *state, const uint32_t **viewport, const VkRect2D **scissor);
static void i915_state_write_viewport(uint32_t *dynamic, const uint32_t *viewport, const VkRect2D *scissor);
static void i915_state_write_blend(uint32_t *dynamic, const struct i915_gfx_draw_state *state);
static int i915_state_write_push(uint8_t *data, const struct i915_gfx_draw_state *state, const struct i915_gfx_push_layout *layout);
static void i915_blend_equation(const struct i915_gfx_pipeline *pipeline, struct i915_gfx_blend *equation);
static uint32_t i915_blend_factor(uint32_t factor);
static uint32_t i915_blend_function(uint32_t op);
static int i915_blend_uses_second_source(uint32_t factor);
static uint32_t i915_state_input_slot(const struct i915_gfx_kernels *kernels, uint32_t inputs, uint32_t input);
static uint64_t i915_state_scratch(uint32_t per_thread_bytes, uint64_t offset);

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
	i915_state_write_blend(dynamic, state);

	/* Fills the vertex stage's push data. */
	error = i915_state_write_push(page + I915_GFX_PUSH_BUFFER, state, &kernels->vs_push);
	if (error != 0)
		return error;

	/* Fills the pixel stage's push data. */
	error = i915_state_write_push(page + I915_GFX_PS_PUSH_BUFFER, state, &kernels->ps_push);
	if (error != 0)
		return error;

	/* Succeeded: the state object holds everything the batch points at. */
	return 0;
}

/*
 * Writes the RENDER_SURFACE_STATE of a linear 2D surface.
 *
 * The surface is checked first: a GPU address, an extent of at most 16384
 * in each direction and a pitch that holds a row of four-byte texels.  An
 * R32_FLOAT surface is written without the unorm path bit, since it carries
 * a depth value's bits rather than a colour.
 */
int
drv_i915_gfx_surface_write(
	uint32_t *rss,
	const struct i915_gfx_surface *surface,
	uint32_t mocs)
{
	uint32_t format;
	uint32_t unorm;
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

	/* Refuses a pitch too short for a row of four-byte texels. */
	if (surface->pitch < surface->width * 4U)
		return EINVAL;

	/* Refuses a format the surface state cannot name. */
	error = i915_surface_format(surface->format, &format);
	if (error != 0)
		return EINVAL;

	/* A colour surface takes the unorm path bit; the depth-bits view does not. */
	unorm = 1U << 31;
	if (surface->format == VK_FORMAT_R32_SFLOAT)
		unorm = 0U;

	/*
	 * Fills the surface state as isl fills it for a linear 2D one-level
	 * surface: 2D, horizontal and vertical alignment 4, linear; the unorm
	 * path bit, MOCS and QPitch; width and height; pitch; mip tail start 1;
	 * identity channel select; the address.
	 */
	kern_memset(rss, 0, GEN12_RENDER_SURFACE_STATE_DWORDS * 4U);
	rss[0] = (GEN12_SURFTYPE_2D << 29) |
	    (format << 18) |
	    (GEN12_SURFACE_ALIGN_4 << 16) |
	    (GEN12_SURFACE_ALIGN_4 << 14) |
	    (GEN12_TILEMODE_LINEAR << 12);
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
 * The fields are the ones anv fills for a sampler without anisotropy,
 * comparison or border colour (genX_init_state.c): the OpenGL LOD
 * pre-clamp, the mip filter of the mipmap mode, the two filters and the LOD
 * bias; the LOD range; address rounding for a linear filter and the u and v
 * address modes.  The bias is clamped to [-16, 15.996] and both LOD limits
 * to [0, 14], as anv clamps them; the surface state then limits the levels
 * to the ones the view has.  An address mode past the table clamps.
 */
void
drv_i915_gfx_sampler_write(
	uint32_t *state,
	const struct i915_gfx_sampler *sampler)
{
	uint32_t address_u;
	uint32_t address_v;
	uint32_t mag_linear;
	uint32_t min_linear;
	uint32_t mip_filter;
	uint32_t rounding;
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
	state[1] = ((uint32_t)max_lod << GEN12_SAMPLER_MAX_LOD_SHIFT) |
	    ((uint32_t)min_lod << GEN12_SAMPLER_MIN_LOD_SHIFT);
	state[2] = 0U;
	state[3] = rounding | (address_u << 6) | (address_v << 3) | 2U;
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

	/* Invalidates the caches that may hold state from before the new bases. */
	drv_i915_batch_pipe_control(batch,
				    PIPE_CONTROL_CS_STALL |
				    PIPE_CONTROL_STATE_CACHE_INVALIDATE |
				    PIPE_CONTROL_CONST_CACHE_INVALIDATE |
				    PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE |
				    PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE);

	/* Clears the state anv clears once per context. */
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_WM_HZ_OP, GEN12_3DSTATE_WM_HZ_OP_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_AA_LINE_PARAMETERS, GEN12_3DSTATE_AA_LINE_PARAMETERS_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_WM_CHROMAKEY, GEN12_3DSTATE_WM_CHROMAKEY_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_POLY_STIPPLE_OFFSET, GEN12_3DSTATE_POLY_STIPPLE_OFFSET_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_LINE_STIPPLE, GEN12_3DSTATE_LINE_STIPPLE_DWORDS);

	/* Places the single sample at the pixel centre. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_SAMPLE_PATTERN, GEN12_3DSTATE_SAMPLE_PATTERN_DWORDS));
	for (index = 1U; index < GEN12_3DSTATE_SAMPLE_PATTERN_DWORDS; index++) {
		/* Only dword 8 carries the 1x pattern; every other dword is zero. */
		pattern = 0U;
		if (index == 8U)
			pattern = GEN12_SAMPLE_PATTERN_1X_CENTRE;
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
	uint32_t format;
	uint32_t components;
	uint32_t component_y;
	uint32_t component_z;
	uint32_t component_w;
	uint64_t va;
	int error;

	/* A draw needs at least one attribute and one vertex buffer binding. */
	pipeline = state->pipeline;
	count = kernels->vs_input_count;
	if (count == 0U || pipeline->binding_count == 0U)
		return EINVAL;

	/* Finds the pipeline attribute of every location the kernel reads. */
	for (index = 0U; index < count; index++) {
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

	/* Emits one VERTEX_BUFFER_STATE for each binding of the pipeline. */
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
		/* Refuses a vertex format the surface formats do not cover. */
		attribute = order[index];
		error = i915_surface_format(pipeline->attributes[attribute].format, &format);
		if (error != 0)
			return ENOTSUP;

		/*
		 * Stores the components the format has; a missing y or z is 0 and
		 * a missing w is 1.0.
		 */
		components = i915_format_components(pipeline->attributes[attribute].format);
		component_y = GEN12_VFCOMP_STORE_0;
		if (components > 1U)
			component_y = GEN12_VFCOMP_STORE_SRC;
		component_z = GEN12_VFCOMP_STORE_0;
		if (components > 2U)
			component_z = GEN12_VFCOMP_STORE_SRC;
		component_w = GEN12_VFCOMP_STORE_1_FP;
		if (components > 3U)
			component_w = GEN12_VFCOMP_STORE_SRC;

		/* Writes the binding, valid bit, format and offset; the component controls. */
		drv_i915_batch_emit(batch,
				    (pipeline->attributes[attribute].binding << 26) |
				    (1U << 25) |
				    (format << 16) |
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
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_VF_SGVS, GEN12_3DSTATE_VF_SGVS_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_VF_SGVS_2, GEN12_3DSTATE_VF_SGVS_2_DWORDS);

	/* Turns instancing off for every vertex element. */
	for (index = 0U; index < count; index++) {
		drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_VF_INSTANCING, GEN12_3DSTATE_VF_INSTANCING_DWORDS));
		drv_i915_batch_emit(batch, index);
		drv_i915_batch_emit(batch, 0U);
	}

	/* Refuses a topology other than a triangle list. */
	if (pipeline->topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) {
		kern_logf("i915: vk: XXX unimplemented path: primitive topology %u\n", pipeline->topology);
		return ENOTSUP;
	}

	/* Draws triangle lists. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_VF_TOPOLOGY, GEN12_3DSTATE_VF_TOPOLOGY_DWORDS));
	drv_i915_batch_emit(batch, GEN12_3DPRIM_TRILIST);

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

	/* Picks the index format and size of the index type. */
	if (state->index.type == VK_INDEX_TYPE_UINT16) {
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
 * The push constant space is split in halves between the vertex and the
 * pixel stage.  The vertex stage owns the URB past the push constants:
 * entries of entry_size 64-byte units, as many as fit, at most 3576 (a
 * multiple of eight, as Mesa's intel_get_urb_config() keeps the vertex
 * stage's count); the other geometry stages get none.
 */
void
drv_i915_gfx_emit_urb(
	struct i915_gfx_batch *batch,
	uint32_t entry_size)
{
	uint32_t opcode;
	uint32_t entries;

	/* Counts the entries the URB past the push constants holds. */
	entries = GEN12_URB_VS_BYTES / (entry_size * 64U);
	if (entries > GEN12_URB_VS_ENTRIES)
		entries = GEN12_URB_VS_ENTRIES;
	entries &= ~7U;

	/* Gives the vertex stage the first half of the push constant space. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_PUSH_CONSTANT_ALLOC_VS, GEN12_3DSTATE_PUSH_CONSTANT_ALLOC_DWORDS));
	drv_i915_batch_emit(batch, (0U << 16) | (GEN12_PUSH_CONSTANT_KB / 2U));

	/* Gives the hull, domain and geometry stages none. */
	for (opcode = GEN12_CMD_3DSTATE_PUSH_CONSTANT_ALLOC_HS; opcode < GEN12_CMD_3DSTATE_PUSH_CONSTANT_ALLOC_PS; opcode++)
		drv_i915_batch_zero(batch, opcode, GEN12_3DSTATE_PUSH_CONSTANT_ALLOC_DWORDS);

	/* Gives the pixel stage the second half. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_PUSH_CONSTANT_ALLOC_PS, GEN12_3DSTATE_PUSH_CONSTANT_ALLOC_DWORDS));
	drv_i915_batch_emit(batch, ((GEN12_PUSH_CONSTANT_KB / 2U) << 16) | (GEN12_PUSH_CONSTANT_KB / 2U));

	/* Gives the vertex stage the URB past the push constants. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_URB_ALLOC_VS, GEN12_3DSTATE_URB_ALLOC_DWORDS));
	drv_i915_batch_emit(batch, (4U << 10) | (4U << 21) | (entry_size - 1U));
	drv_i915_batch_emit(batch, entries | (entries << 16));

	/* Gives the hull, domain and geometry stages no URB entries. */
	for (opcode = GEN12_CMD_3DSTATE_URB_ALLOC_HS; opcode <= GEN12_CMD_3DSTATE_URB_ALLOC_GS; opcode++) {
		drv_i915_batch_emit(batch, GEN12_CMD_HEADER(opcode, GEN12_3DSTATE_URB_ALLOC_DWORDS));
		drv_i915_batch_emit(batch, (5U << 10) | (5U << 21));
		drv_i915_batch_emit(batch, 0U);
	}
}

/*
 * Emits the 3DSTATE_CONSTANT_* packets of all five stages.
 *
 * The vertex and the pixel stage each read their own push data, as many
 * registers as its kernel uses.  Like anv, the buffer goes in the highest
 * slot, so that slot 0 is never the only one in use; a stage that reads
 * nothing is given an empty packet.
 */
void
drv_i915_gfx_emit_constants(
	struct i915_gfx_batch *batch,
	uint64_t vs_push_va,
	uint32_t vs_push_regs,
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

		/* The vertex stage (first) and the pixel stage (last) read push data; the others none. */
		push_regs = 0U;
		push_va = 0U;
		if (stage == 0U) {
			push_regs = vs_push_regs;
			push_va = vs_push_va;
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
 * Emits 3DSTATE_CLIP, SF and RASTER of an ordinary Vulkan pipeline, as anv
 * programs them.
 */
void
drv_i915_gfx_emit_raster(
	struct i915_gfx_batch *batch,
	const struct i915_gfx_pipeline *pipeline)
{
	uint32_t cull;
	uint32_t counter_clockwise;
	uint32_t index;

	/*
	 * Clips with statistics, early cull and 8-bit subpixel precision; the
	 * D3D API mode (z in [0, 1]), viewport XY test and guardband; point
	 * widths 0.125 .. 255.875.
	 */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_CLIP, GEN12_3DSTATE_CLIP_DWORDS));
	drv_i915_batch_emit(batch, (1U << 10) | (1U << 18));
	drv_i915_batch_emit(batch, (1U << 31) | (1U << 30) | (1U << 28) | (1U << 26));
	drv_i915_batch_emit(batch, (1U << 17) | (2047U << 6));

	/*
	 * Sets up with the viewport transform, statistics and line width 1.0;
	 * the URB deref block; point width 1.0 from state and the AA line
	 * distance.
	 */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_SF, GEN12_3DSTATE_SF_DWORDS));
	drv_i915_batch_emit(batch, (1U << 1) | (1U << 10) | (128U << 12));
	drv_i915_batch_emit(batch, GEN12_URB_DEREF_BLOCK_SIZE_32 << 29);
	drv_i915_batch_emit(batch, 8U | (1U << 11) | (1U << 14));

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

	/* Rasterizes with z near and far clip tests, scissor, the cull mode, the winding and the DX10.1+ API mode. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_RASTER, GEN12_3DSTATE_RASTER_DWORDS));
	drv_i915_batch_emit(batch,
			    (1U << 0) |
			    (1U << 26) |
			    (1U << 1) |
			    (cull << 16) |
			    (counter_clockwise << 21) |
			    (2U << 22));
	for (index = 2U; index < GEN12_3DSTATE_RASTER_DWORDS; index++)
		drv_i915_batch_emit(batch, 0U);
}

/*
 * Emits the depth test, the depth buffer and the post-sync write anv makes
 * after the depth state.
 *
 * With no depth attachment the depth buffer is a null D32_FLOAT surface and
 * the test is off.  Returns EINVAL for a depth attachment with no storage or
 * in a format other than D32_SFLOAT.
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
	uint32_t write_enable;
	uint32_t test_enable;
	uint32_t index;
	uint64_t va;

	/* Packs the depth write, the depth test and its compare function when there is a depth buffer. */
	pipeline = state->pipeline;
	depth_state = 0U;
	if (depth != NULL) {
		write_enable = 0U;
		if (pipeline->depth_write != 0U)
			write_enable = 1U;
		test_enable = 0U;
		if (pipeline->depth_test != 0U)
			test_enable = 1U;
		depth_state = write_enable | (test_enable << 1) | (i915_gfx_compare_functions[pipeline->depth_compare & 7U] << 5);
	}

	/* Programs the depth test. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_WM_DEPTH_STENCIL, GEN12_3DSTATE_WM_DEPTH_STENCIL_DWORDS));
	drv_i915_batch_emit(batch, depth_state);
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, 0U);

	/* Describes the depth buffer, or a null one. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_DEPTH_BUFFER, GEN12_3DSTATE_DEPTH_BUFFER_DWORDS));
	if (depth != NULL) {
		/* Refuses a depth image with no storage or in another format. */
		va = drv_i915_gfx_memory_va(depth->memory, depth->offset);
		if (va == 0U || depth->format != VK_FORMAT_D32_SFLOAT)
			return EINVAL;

		/*
		 * Writes what isl_emit_depth_stencil_hiz_s() writes: 2D, D32_FLOAT,
		 * write enable and the pitch (Y-tiled: Gen9+ depth always is); the
		 * address; the extent; MOCS; the QPitch.
		 */
		drv_i915_batch_emit(batch, (GEN12_SURFTYPE_2D << 29) | (1U << 28) | (GEN12_DEPTH_FORMAT_D32_FLOAT << 24) | (depth->pitch - 1U));
		drv_i915_batch_emit(batch, (uint32_t)va);
		drv_i915_batch_emit(batch, (uint32_t)(va >> 32));
		drv_i915_batch_emit(batch, ((depth->width - 1U) << 1) | ((depth->height - 1U) << 17));
		drv_i915_batch_emit(batch, mocs);
		drv_i915_batch_emit(batch, 0U);
		drv_i915_batch_emit(batch, ((depth->height + 3U) & ~3U) / 4U);
	} else {
		/* A null depth buffer is still typed D32_FLOAT. */
		drv_i915_batch_emit(batch, (GEN12_SURFTYPE_NULL << 29) | (GEN12_DEPTH_FORMAT_D32_FLOAT << 24));
		for (index = 2U; index < GEN12_3DSTATE_DEPTH_BUFFER_DWORDS; index++)
			drv_i915_batch_emit(batch, 0U);
	}

	/* Describes a null stencil buffer, and clears the hierarchical depth and the clear values. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_STENCIL_BUFFER, GEN12_3DSTATE_STENCIL_BUFFER_DWORDS));
	drv_i915_batch_emit(batch, GEN12_SURFTYPE_NULL << 29);
	for (index = 2U; index < GEN12_3DSTATE_STENCIL_BUFFER_DWORDS; index++)
		drv_i915_batch_emit(batch, 0U);
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
 * Emits SBE, SBE_SWIZ, WM, PS and PS_EXTRA for the pixel kernel.
 *
 * Every varying the vertex kernel writes is read from VUE slot 2 on, and
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
	 * number of attributes, the read length and the read offset of slot 2;
	 * every attribute with all four components active.
	 */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_SBE, GEN12_3DSTATE_SBE_DWORDS));
	drv_i915_batch_emit(batch,
			    (1U << 29) |
			    (1U << 28) |
			    (inputs << 22) |
			    (1U << 21) |
			    (read_length << 11) |
			    (1U << 5));
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, 0U);
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

	/* Programs WM: statistics, perspective pixel barycentrics, line AA width 1.0. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_WM, GEN12_3DSTATE_WM_DWORDS));
	drv_i915_batch_emit(batch, (1U << 31) | (1U << 11) | (1U << 6));

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

	/* Programs PS_EXTRA: valid, whether the kernel discards and whether it reads attributes. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_PS_EXTRA, GEN12_3DSTATE_PS_EXTRA_DWORDS));
	drv_i915_batch_emit(batch, (1U << 31) | kills | (has_varyings << 8));
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
	case VK_FORMAT_R32_SFLOAT:
		*surface_format = I915_GFX_SURFACE_R32_FLOAT;
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
		return ENOTSUP;
	}
}

/* Reports how many components a vertex format has; any other format counts as four. */
static uint32_t
i915_format_components(
	uint32_t format)
{
	/* Picks the component count of the float vertex formats. */
	switch (format) {
	case VK_FORMAT_R32_SFLOAT:
		return 1U;
	case VK_FORMAT_R32G32_SFLOAT:
		return 2U;
	case VK_FORMAT_R32G32B32_SFLOAT:
		return 3U;
	default:
		return 4U;
	}
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
 * Writes the RENDER_SURFACE_STATE of a linear 2D image, as isl fills it
 * (isl_surface_state.c): the levels [base_level, base_level + level_count)
 * of a sampled image, or level 0 (base 0, count 1) of a render target.
 *
 * The surface starts at level 0 with the level-0 extent; the hardware finds
 * every other level itself in the 2D mip layout (image.c), from the
 * alignment and the pitch.  Surface Min LOD is the first level and MIP
 * Count the levels after it; Mip Tail Start is the image's level count (no
 * mip tail, isl_choose_miptail_start_level() for a linear surface).
 * Returns EINVAL for an image with no storage, a format the surface state
 * cannot name, or a range of levels the image does not have.
 */
static int
i915_image_surface_write(
	uint32_t *rss,
	const struct i915_gfx_image *image,
	uint32_t base_level,
	uint32_t level_count,
	uint32_t mocs)
{
	uint64_t va;
	uint32_t format;
	uint32_t rows;
	int error;

	/* Refuses an image with no storage. */
	va = drv_i915_gfx_memory_va(image->memory, image->offset);
	if (va == 0U)
		return EINVAL;

	/* Refuses a format the surface state cannot name. */
	error = i915_surface_format(image->format, &format);
	if (error != 0)
		return EINVAL;

	/* Refuses an empty range of levels, and one past the image's last level. */
	if (level_count == 0U || base_level >= image->levels)
		return EINVAL;
	if (level_count > image->levels - base_level)
		return EINVAL;

	/* The QPitch counts the rows of the whole layout, in units of four. */
	rows = (uint32_t)(image->bytes / image->pitch);
	rows = (rows + 3U) & ~3U;

	/*
	 * Fills the surface state: 2D, horizontal and vertical alignment 4,
	 * linear; the unorm path bit, MOCS and QPitch; width and height; pitch;
	 * the mip count, the first level and the mip tail start; identity
	 * channel select; the address.
	 */
	kern_memset(rss, 0, GEN12_RENDER_SURFACE_STATE_DWORDS * 4U);
	rss[0] = (GEN12_SURFTYPE_2D << 29) |
	    (format << 18) |
	    (GEN12_SURFACE_ALIGN_4 << 16) |
	    (GEN12_SURFACE_ALIGN_4 << 14) |
	    (GEN12_TILEMODE_LINEAR << 12);
	rss[1] = (1U << 31) | (mocs << 24) | ((rows / 4U) & GEN12_RSS_QPITCH_MASK);
	rss[2] = (image->width - 1U) | ((image->height - 1U) << 16);
	rss[3] = image->pitch - 1U;
	rss[5] = (((level_count - 1U) & GEN12_RSS_LOD_MASK) << GEN12_RSS_MIP_COUNT_SHIFT) |
	    ((base_level & GEN12_RSS_LOD_MASK) << GEN12_RSS_SURFACE_MIN_LOD_SHIFT) |
	    ((image->levels & GEN12_RSS_LOD_MASK) << GEN12_RSS_MIP_TAIL_START_SHIFT);
	rss[7] = (4U << 25) | (5U << 22) | (6U << 19) | (7U << 16);
	rss[8] = (uint32_t)va;
	rss[9] = (uint32_t)(va >> 32);

	/* Succeeded: the surface state describes the image. */
	return 0;
}

/*
 * Writes the binding table, the render target and texture surfaces and the
 * samplers of a draw.
 *
 * Binding table entry 0 is the render target and entry 1 + n the n-th
 * sampled image of the pixel kernel, with sampler n, as the compiler numbers
 * them; each is the view and the sampler bound at the (set, binding) the
 * kernel names.
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
	uint32_t texture;
	uint32_t set_index;
	uint32_t binding;
	uint32_t rss;
	int error;

	/* Points binding table entry 0 at the render target and describes its level 0. */
	surface[I915_GFX_BINDING_TABLE / 4U] = I915_GFX_RSS_TARGET;
	error = i915_image_surface_write(&surface[I915_GFX_RSS_TARGET / 4U], target, 0U, 1U, mocs);
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

		/* Refuses a draw whose set and binding lack the view or the sampler. */
		set = NULL;
		if (set_index < I915_GFX_BOUND_SETS && binding < I915_GFX_MAX_BINDINGS)
			set = state->dset[set_index];
		if (set == NULL ||
		    set->slots[binding].view == NULL ||
		    set->slots[binding].sampler == NULL) {
			kern_logf("i915: vk: draw refused: set %u binding %u has no image view and sampler\n", set_index, binding);
			return EINVAL;
		}

		/* Points the texture's binding table entry at its surface state and describes the levels its view shows. */
		view = set->slots[binding].view;
		sampler = set->slots[binding].sampler;
		rss = I915_GFX_RSS_TEXTURE + texture * I915_GFX_RSS_BYTES;
		surface[I915_GFX_BINDING_TABLE / 4U + 1U + texture] = rss;
		error = i915_image_surface_write(&surface[rss / 4U], view->image, view->base_level, view->level_count, mocs);
		if (error != 0)
			return error;

		/* Writes the texture's sampler. */
		drv_i915_gfx_sampler_write(&dynamic[(I915_GFX_DYN_SAMPLER + texture * I915_GFX_SAMPLER_BYTES) / 4U], sampler);
	}

	/* Succeeded: the target, the textures and the samplers are described. */
	return 0;
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
	uint32_t constant_bytes;
	uint32_t index;

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

		/* Finds the buffer bound at the block's set and binding. */
		set = NULL;
		if (block->set < I915_GFX_BOUND_SETS && block->binding < I915_GFX_MAX_BINDINGS)
			set = state->dset[block->set];
		buffer = NULL;
		if (set != NULL)
			buffer = set->slots[block->binding].buffer;
		if (buffer == NULL) {
			kern_logf("i915: vk: draw refused: set %u binding %u has no uniform buffer\n", block->set, block->binding);
			return EINVAL;
		}

		/* The descriptor's offset, moved by the bind's dynamic offset for a dynamic uniform buffer. */
		descriptor_offset = set->slots[block->binding].offset;
		if (set->slots[block->binding].dynamic != 0)
			descriptor_offset += state->dynamic_offsets[block->set][block->binding];

		/* The descriptor's range: to the buffer's end for VK_WHOLE_SIZE, nothing past it. */
		range = set->slots[block->binding].range;
		if (descriptor_offset >= buffer->size) {
			range = 0U;
		} else if (range == VK_WHOLE_SIZE || range > buffer->size - descriptor_offset) {
			range = buffer->size - descriptor_offset;
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
	bottom_edge = drv_i915_float_add(y, height);
	bottom_edge = drv_i915_float_sub(bottom_edge, I915_FLOAT_ONE);

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
	words[14] = y;
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
	const struct i915_gfx_draw_state *state)
{
	const struct i915_gfx_pipeline *pipeline;
	struct i915_gfx_blend equation;
	const uint32_t *constants;
	uint32_t *words;
	uint32_t entry;
	uint32_t index;

	/* Works out the hardware blend of attachment 0. */
	pipeline = state->pipeline;
	i915_blend_equation(pipeline, &equation);

	/* The components attachment 0 leaves as they are. */
	entry = 0U;
	if ((pipeline->color_write_disable & VK_COLOR_COMPONENT_R_BIT) != 0U)
		entry |= GEN12_BLEND_WRITE_DISABLE_RED;
	if ((pipeline->color_write_disable & VK_COLOR_COMPONENT_G_BIT) != 0U)
		entry |= GEN12_BLEND_WRITE_DISABLE_GREEN;
	if ((pipeline->color_write_disable & VK_COLOR_COMPONENT_B_BIT) != 0U)
		entry |= GEN12_BLEND_WRITE_DISABLE_BLUE;
	if ((pipeline->color_write_disable & VK_COLOR_COMPONENT_A_BIT) != 0U)
		entry |= GEN12_BLEND_WRITE_DISABLE_ALPHA;

	/* A blending entry names its factors and functions. */
	if (equation.enable != 0U) {
		entry |= GEN12_BLEND_ENABLE |
		    (equation.src_color << GEN12_BLEND_SRC_FACTOR_SHIFT) |
		    (equation.dst_color << GEN12_BLEND_DST_FACTOR_SHIFT) |
		    (equation.color_function << GEN12_BLEND_COLOR_FUNCTION_SHIFT) |
		    (equation.src_alpha << GEN12_BLEND_SRC_ALPHA_FACTOR_SHIFT) |
		    (equation.dst_alpha << GEN12_BLEND_DST_ALPHA_FACTOR_SHIFT) |
		    (equation.alpha_function << GEN12_BLEND_ALPHA_FUNCTION_SHIFT);
	}

	/* Fills BLEND_STATE: the independent alpha, then the entry and its clamps. */
	words = &dynamic[I915_GFX_DYN_BLEND / 4U];
	words[0] = 0U;
	if (equation.independent_alpha != 0U)
		words[0] = GEN12_BLEND_INDEPENDENT_ALPHA;
	words[1] = entry;
	words[2] = 1U | (1U << 1) | (GEN12_COLORCLAMP_RTFORMAT << 2);

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
 * colour's.  An equation that reads a second colour source is not blended,
 * as anv does when the shader writes no second source.
 */
static void
i915_blend_equation(
	const struct i915_gfx_pipeline *pipeline,
	struct i915_gfx_blend *equation)
{
	int second;

	/* Starts with blending off, which is all a pipeline without blending asks for. */
	kern_memset(equation, 0, sizeof(*equation));
	if (pipeline->blend_enable == 0U)
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

	/* XXX: the shaders write no second source, so such an equation is not blended. */
	if (second != 0) {
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
