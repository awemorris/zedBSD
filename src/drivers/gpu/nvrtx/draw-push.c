/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of the first 3D draw that touch no hardware (see nvrtx-draw.h).
 *
 * It builds the method stream of the queue's draw state, a rendering's
 * colour targets, a draw's dynamic state, a program's binding, the clear
 * and the draw itself, and the words of a texture header and a sampler.
 *
 * The kernel has no floating point, so the floats the methods take are
 * built from their bits.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>

#include "drivers/gpu/nvrtx/nvrtx-draw.h"
#include "drivers/gpu/nvrtx/nvrtx-gsp.h"

/*
 * The 3D class's methods (TURING_A) and the strides of their arrays.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc597.h (the NVC597_* methods).
 */
#define M_SET_OBJECT				0x0000U
#define M_SET_ALIASED_LINE_WIDTH_ENABLE		0x020cU
#define M_SET_API_MANDATED_EARLY_Z		0x0210U
#define M_SET_L2_ROP_PREFETCH_READ		0x0218U
#define M_SET_INSTANCE_COUNT			0x0220U
#define M_SET_VARIABLE_PIXEL_RATE_SAMPLE_ORDER	0x0280U
#define M_SET_PS_OUTPUT_SAMPLE_MASK_USAGE	0x0300U
#define M_SET_L1_CONFIGURATION			0x0308U
#define M_SET_RENDER_ENABLE_CONTROL		0x030cU
#define M_SET_TESSELLATION_PARAMETERS		0x0320U
#define M_SET_SUBTILING_PERF_KNOB_A		0x0360U
#define M_SET_RASTER_ENABLE			0x037cU
#define M_SET_VERTEX_STREAM_SIZE_A		0x0600U
#define M_SET_HYBRID_ANTI_ALIAS_CONTROL		0x0754U
#define M_SET_SHADER_LOCAL_MEMORY_WINDOW	0x077cU
#define M_SET_COLOR_TARGET_A			0x0800U
#define M_SET_COLOR_TARGET_STRIDE		0x40U
#define M_SET_VIEWPORT_SCALE_X			0x0a00U
#define M_SET_VIEWPORT_COORDINATE_SWIZZLE	0x0a18U
#define M_SET_VIEWPORT_CLIP_HORIZONTAL		0x0c00U
#define M_SET_API_VISIBLE_CALL_LIMIT		0x0d64U
#define M_SET_CLEAR_RECT_HORIZONTAL		0x0d6cU
#define M_SET_VERTEX_ARRAY_START		0x0d74U
#define M_SET_VIEWPORT_Z_CLIP			0x0d7cU
#define M_SET_COLOR_CLEAR_VALUE			0x0d80U
#define M_SET_REDUCE_COLOR_THRESHOLDS_ENABLE	0x0d9cU
#define M_INVALIDATE_SHADER_CACHES_NO_WFI	0x0da4U
#define M_SET_FRONT_POLYGON_MODE		0x0dacU
#define M_SET_POLY_SMOOTH			0x0db4U
#define M_SET_DA_PRIMITIVE_RESTART_VERTEX_ARRAY	0x0de8U
#define M_SET_WINDOW_OFFSET_X			0x0df8U
#define M_SET_SCISSOR_ENABLE			0x0e00U
#define M_SET_SCISSOR_STRIDE			0x10U
#define M_SET_POST_Z_PS_IMASK			0x0f1cU
#define M_SET_VERTEX_STREAM_SUBSTITUTE_A	0x0f84U
#define M_SET_SINGLE_CT_WRITE_CONTROL		0x0f90U
#define M_SET_CT_MRT_ENABLE			0x0facU
#define M_SET_SAMPLE_MASK_X0_Y0			0x0fbcU
#define M_SET_BLEND_OPT_CONTROL			0x0fdcU
#define M_SET_SURFACE_CLIP_HORIZONTAL		0x0ff4U
#define M_SET_REDUCE_COLOR_THRESHOLDS_UNORM8	0x10ccU
#define M_SET_REDUCE_COLOR_THRESHOLDS_UNORM10	0x10e0U
#define M_SET_CLEAR_SURFACE_CONTROL		0x10f8U
#define M_SET_L2_ROP_NONINTERLOCKED_READ	0x10fcU
#define M_SET_VERTEX_ID_BASE			0x1118U
#define M_SET_POST_PS_INITIAL_COVERAGE		0x1138U
#define M_SET_FILL_VIA_TRIANGLE			0x113cU
#define M_SET_BLEND_PER_FORMAT_ENABLE		0x1140U
#define M_SET_VERTEX_ATTRIBUTE_A		0x1160U
#define M_SET_OFFSET_RENDER_TARGET_INDEX	0x11f0U
#define M_SET_CT_SELECT				0x121cU
#define M_SET_SAMPLER_BINDING			0x1234U
#define M_SET_L2_ROP_INTERLOCKED_READ		0x1290U
#define M_SET_DEPTH_TEST			0x12ccU
#define M_SET_SHADE_MODE			0x12d4U
#define M_SET_L2_ROP_NONINTERLOCKED_WRITE	0x12d8U
#define M_SET_L2_ROP_INTERLOCKED_WRITE		0x12dcU
#define M_SET_BLEND_STATE_PER_TARGET		0x12e4U
#define M_SET_DEPTH_WRITE			0x12e8U
#define M_SET_ALPHA_TEST			0x12ecU
#define M_SET_BLEND_SEPARATE_FOR_ALPHA		0x133cU
#define M_SET_SINGLE_ROP_CONTROL		0x135cU
#define M_SET_BLEND				0x1360U
#define M_SET_STENCIL_TEST			0x1380U
#define M_SET_PS_SATURATE			0x13a8U
#define M_SET_WINDOW_ORIGIN			0x13acU
#define M_INVALIDATE_SAMPLER_CACHE_NO_WFI	0x1424U
#define M_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI	0x1428U
#define M_SET_GLOBAL_BASE_VERTEX_INDEX		0x1434U
#define M_SET_GLOBAL_BASE_INSTANCE_INDEX	0x1438U
#define M_SET_PS_WARP_WATERMARKS		0x1450U
#define M_SET_PS_REGISTER_WATERMARKS		0x1454U
#define M_SET_USER_CLIP_ENABLE			0x1510U
#define M_SET_ZPASS_PIXEL_COUNT			0x1514U
#define M_SET_POINT_SIZE			0x1518U
#define M_SET_ZCULL_STATS			0x151cU
#define M_SET_POINT_SPRITE			0x1520U
#define M_SET_ANTI_ALIAS_ENABLE			0x1534U
#define M_SET_ZT_SELECT				0x1538U
#define M_SET_ANTI_ALIAS_ALPHA_CONTROL		0x153cU
#define M_SET_RENDER_ENABLE_C			0x1558U
#define M_SET_TEX_SAMPLER_POOL_A		0x155cU
#define M_SET_TEX_HEADER_POOL_A			0x1574U
#define M_SET_ACTIVE_ZCULL_REGION		0x1590U
#define M_SET_TWO_SIDED_STENCIL_TEST		0x1594U
#define M_SET_RT_LAYER				0x15ccU
#define M_SET_ANTI_ALIAS			0x15d0U
#define M_SET_EDGE_FLAG				0x15e4U
#define M_SET_POINT_SPRITE_SELECT		0x1604U
#define M_SET_ATTRIBUTE_DEFAULT			0x1610U
#define M_END					0x1614U
#define M_BEGIN					0x1618U
#define M_SET_DA_OUTPUT				0x164cU
#define M_SET_ANTI_ALIASED_POINT		0x1658U
#define M_SET_POINT_CENTER_MODE			0x165cU
#define M_SET_PROVOKING_VERTEX			0x1684U
#define M_SET_TWO_SIDED_LIGHT			0x1688U
#define M_CHECK_SPH_VERSION			0x16a8U
#define M_SET_ALPHA_TO_COVERAGE_OVERRIDE	0x16b4U
#define M_CHECK_AAM_VERSION			0x1794U
#define M_SET_VERTEX_STREAM_INSTANCE_A		0x1880U
#define M_SET_ATTRIBUTE_POINT_SIZE		0x1910U
#define M_OGL_SET_CULL				0x1918U
#define M_OGL_SET_FRONT_FACE			0x191cU
#define M_SET_VIEWPORT_PIXEL			0x1924U
#define M_SET_VIEWPORT_SCALE_OFFSET		0x192cU
#define M_SET_VIEWPORT_CLIP_CONTROL		0x193cU
#define M_SET_USER_CLIP_OP			0x1940U
#define M_SET_RENDER_ENABLE_OVERRIDE		0x1944U
#define M_SET_WINDOW_CLIP_ENABLE		0x194cU
#define M_SET_ZCULL_BOUNDS			0x196cU
#define M_SET_CLIP_ID_TEST			0x197cU
#define M_SET_BLEND_FLOAT_OPTION		0x19c0U
#define M_SET_LOGIC_OP				0x19c4U
#define M_SET_Z_COMPRESSION			0x19ccU
#define M_CLEAR_SURFACE				0x19d0U
#define M_SET_COLOR_COMPRESSION			0x19e0U
#define M_SET_CT_WRITE				0x1a00U
#define M_SET_VERTEX_STREAM_A_FORMAT		0x1c00U
#define M_SET_VERTEX_STREAM_A_STRIDE		0x10U
#define M_SET_PIPELINE_SHADER			0x2000U
#define M_SET_PIPELINE_REGISTER_COUNT		0x200cU
#define M_SET_PIPELINE_PROGRAM_ADDRESS_A	0x2014U
#define M_SET_PIPELINE_STRIDE			0x40U
#define M_SET_CONSTANT_BUFFER_SELECTOR_A	0x2380U
#define M_BIND_GROUP_CONSTANT_BUFFER		0x2410U
#define M_BIND_GROUP_STRIDE			0x20U
#define M_SET_COLOR_CLAMP			0x2600U
#define M_SET_VARIABLE_PIXEL_RATE_SHADING_CONTROL	0x2a00U
#define M_SET_SHADING_RATE_STRIDE		0x10U

/*
 * The counts of the arrays the queue's state sets in full.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_push_draw_state_init, nvk_mme_set_shading_rate_control).
 */
#define DRAW_COLOR_TARGETS		8U
#define DRAW_SCISSORS			16U
#define DRAW_GROUPS			5U
#define DRAW_GROUP_SLOTS		16U
#define DRAW_VERTEX_STREAMS		32U
#define DRAW_VERTEX_ATTRIBUTES		32U
#define DRAW_SAMPLE_ORDERS		13U
#define DRAW_SHADING_RATE_CONTROLS	16U

/*
 * The values of the queue's state (the fields' meanings beside them).
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc597.h (the fields of the NVC597_*
 * methods) and src/nouveau/vulkan/nvk_cmd_draw.c (nvk_push_draw_state_init).
 */
#define V_TRUE				1U
#define V_FALSE				0U
#define V_RENDER_ENABLE_TRUE		1U
#define V_CT_SELECT_ONE_TARGET		1U
#define V_ALPHA_TO_COVERAGE_BY_AA	1U
#define V_SHADE_OGL_SMOOTH		0x1d01U
#define V_CALL_LIMIT_128		8U
#define V_L1_48KB			3U
#define V_REDUCE_ALL_HIT_ONCE		0xffU
#define V_REDUCE_FP11_ALL_HIT_ONCE	0x3fU
#define V_SPH_VERSION_3			0x00030003U
#define V_AAM_VERSION_2			0x00020002U
#define V_L2_EVICT_NORMAL		(1U << 4)
#define V_BLEND_PER_FORMAT_SNORM8	(1U << 4)
#define V_ATTRIBUTE_DEFAULT_0001	0xeU
#define V_DA_VERTEX_ID_ARRAY_START	(1U << 12)
#define V_SAMPLE_MASK_USAGE		3U
#define V_FLOAT_ONE			0x3f800000U
#define V_POINT_SPRITE_ORIGIN_TOP	(1U << 2)
#define V_ZCULL_REGION_ALL		0x3fU
#define V_VIEWPORT_CLIP_INIT		0x18U
#define V_LOCAL_MEMORY_WINDOW		0xff000000U
#define V_PS_WARP_WATERMARKS		0x00400008U
#define V_PS_REGISTER_WATERMARKS	0x10000080U
#define V_GROUP_VALID			1U
#define V_GROUP_SLOT_SHIFT		4U

/*
 * A colour target: pitch layout, and the width of the null target written
 * for one not used.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc597.h
 * (NVC597_SET_COLOR_TARGET_MEMORY) and src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_CmdBeginRendering).
 */
#define V_TARGET_LAYOUT_PITCH		(1U << 12)
#define V_TARGET_NULL_WIDTH		64U

/*
 * The draw's raster state: the viewport's swizzle and clip, fill, front
 * face, the masks.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc597.h (the fields of the NVC597_*
 * methods) and src/nouveau/vulkan/nvk_cmd_draw.c (nvk_flush_vp_state,
 * nvk_flush_rs_state, nvk_flush_ms_state, nvk_flush_cb_state,
 * nvk_mme_set_anti_alias).
 */
#define V_VIEWPORT_SWIZZLE_IDENTITY	0x6420U
#define V_Z_CLIP_ZERO_TO_W		1U
#define V_VIEWPORT_CLIP_DRAW		0x181dU
#define V_POLYGON_FILL			0x1b02U
#define V_FRONT_FACE_CCW		0x901U
#define V_SAMPLE_MASK_ALL		0xffffU
#define V_CT_WRITE_RGBA			0x1111U
#define V_ANTI_ALIAS_1X1		0U
#define V_HYBRID_ONE_PASS		1U

/*
 * The program groups a vertex and a pixel program are bound to, and a pixel
 * program's subtiling.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_shader.c
 * (nvk_pipeline_bind_group, nvk_shader_fill_push).
 */
#define V_GROUP_VERTEX			0U
#define V_GROUP_PIXEL			4U
#define V_SUBTILING_A			0x20164010U
#define V_SUBTILING_B			0x20U

/*
 * The shader caches a push buffer's draws read: instructions, global data,
 * constants.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc597.h
 * (NVC597_INVALIDATE_SHADER_CACHES_NO_WFI).
 */
#define V_INVALIDATE_SHADERS		0x1011U

/*
 * A vertex attribute: active two floats, or inactive four (zero).
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc597.h
 * (NVC597_SET_VERTEX_ATTRIBUTE_A, NVC597_SET_VERTEX_STREAM_A_FORMAT) and
 * src/nouveau/vulkan/nvk_cmd_draw.c (nvk_flush_vi_state).
 */
#define V_ATTRIBUTE_R32_G32_FLOAT	((4U << 21) | (7U << 27))
#define V_ATTRIBUTE_INACTIVE		((1U << 6) | (1U << 21) | (7U << 27))
#define V_STREAM_ENABLE			(1U << 12)

/*
 * The clear: by its rectangle, all four channels of target 0.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc597.h (NVC597_CLEAR_SURFACE,
 * NVC597_SET_CLEAR_SURFACE_CONTROL, NVC597_SET_RENDER_ENABLE_OVERRIDE) and
 * src/nouveau/vulkan/nvk_cmd_clear.c (nvk_CmdClearAttachments).
 */
#define V_CLEAR_USE_RECT		(1U << 4)
#define V_CLEAR_RGBA			0x3cU
#define V_RENDER_ALWAYS			1U
#define V_RENDER_USE_ENABLE		0U

/*
 * The draw: a triangle list, the instance iterated from the first.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc597.h (NVC597_BEGIN) and
 * src/nouveau/vulkan/nvk_cmd_draw.c (nvk_mme_build_draw_loop).
 */
#define V_BEGIN_TRIANGLES		(4U | (1U << 31))

/*
 * An incrementing method header on the 3D subchannel (the channel class's
 * format).
 *
 * Value referenced: Mesa 25.3.6 src/nouveau/headers/nv_push.h
 * (NVC0_FIFO_PKHDR_SQ).
 */
#define DRAW_METHOD_INCREMENT		(1U << 29)

/*
 * The texture header (pitch) and the sampler.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc597tex.h (NVC597_TEXHEAD_PITCH_*),
 * src/nouveau/headers/nvidia/classes/cl9097tex.h (NV9097_TEXSAMP*),
 * src/nouveau/nil/descriptor.rs (nvb097_fill_image_view_desc) and
 * src/nouveau/vulkan/nvk_sampler.c (nvk_sampler_get_header).
 */
#define TIC_COMPONENTS_A8B8G8R8		0x08U
#define TIC_TYPE_UNORM			2U
#define TIC_SOURCE_R			2U
#define TIC_SOURCE_G			3U
#define TIC_SOURCE_B			4U
#define TIC_SOURCE_A			5U
#define TIC_VERSION_PITCH		2U
#define TIC_TYPE_TWO_D_NO_MIPMAP	7U
#define TIC_PROMOTE_TO_2_V		1U
#define TIC_BORDER_SAMPLER_COLOR	7U
#define TIC_SPREAD_FUNC_TWO		2U
#define TIC_SPREAD_FUNC_ONE		1U
#define TSC_CLAMP_TO_EDGE		2U
#define TSC_FONT_SIZE_2			1U
#define TSC_MAG_POINT			1U
#define TSC_MIN_POINT			1U
#define TSC_MIP_POINT			2U
#define TSC_CUBE_AUTO_SPAN_SEAM		2U

/*
 * The IR of the draw's programs: the float constants they load, by their
 * bits.
 */
#define IR_FLOAT_ZERO			0x00000000U
#define IR_FLOAT_ONE			0x3f800000U

static size_t draw_method(uint8_t *push, size_t at, uint32_t method, uint32_t count);
static size_t draw_set(uint8_t *push, size_t at, uint32_t method, uint32_t value);
static size_t draw_null_target(uint8_t *push, size_t at, uint32_t index);
static size_t draw_program(uint8_t *push, size_t at, uint32_t type, uint64_t address, uint32_t gprs, uint32_t group);
static uint32_t draw_float(uint32_t value);
static void draw_field(uint32_t *words, unsigned low, unsigned bits, uint32_t value);
static void draw_ir_mix(struct drv_gpu_shader_ir_inst *list, uint32_t *count, uint32_t base, uint32_t corner, uint32_t from, uint32_t to);
static void draw_ir_load(struct drv_gpu_shader_ir_inst *list, uint32_t *count, uint32_t dst, enum drv_gpu_shader_ir_op op, uint32_t location, uint32_t component, uint32_t immediate);
static void draw_ir_store(struct drv_gpu_shader_ir_inst *list, uint32_t *count, uint32_t location, uint32_t component, uint32_t value);

/*
 * Writes the 3D class's binding and the state a queue starts with,
 * without what needs macros (the private register writes, the macro
 * scratch): the render and compression enables, the colour target
 * selection, the blend, cache, attribute and point defaults, the window
 * and viewport defaults, every scissor off, the constant buffer groups
 * emptied, the vertex substitute (a zero page) and every vertex stream off,
 * with Turing's 3D class.
 *
 * Returns where the push buffer goes on.
 */
size_t
nvrtx_push_draw_init(
	uint8_t *push,
	size_t at,
	uint64_t zero_page)
{
	size_t end;

	/* The same state as any generation's, with Turing's class. */
	end = nvrtx_push_draw_init_class(push, at, zero_page, NVRTX_DRAW_CLASS);

	/* Succeeded: where the push buffer goes on. */
	return end;
}

/*
 * Writes the 3D class's binding and the state a queue starts with, as
 * nvrtx_push_draw_init does, for the generation's 3D class (Turing's,
 * Ampere's or Ada's: their methods are the same for this state).
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_push_draw_state_init).
 */
size_t
nvrtx_push_draw_init_class(
	uint8_t *push,
	size_t at,
	uint64_t zero_page,
	uint32_t threed_class)
{
	/*
	 * Turing's variable pixel rate sample order.
	 *
	 * Values referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_cmd_draw.c
	 * (nvk_push_draw_state_init).
	 */
	static const uint32_t sample_order[DRAW_SAMPLE_ORDERS] = {
		0xa23eb139U, 0xfb72ea61U, 0xd950c843U, 0x88fac4e5U, 0x1ab3e1b6U,
		0xa98fedc2U, 0x2107654bU, 0xe0539773U, 0x698badcfU, 0x71032547U,
		0xdef05397U, 0x56789abcU, 0x00001234U
	};
	uint32_t index;
	uint32_t slot;

	/* Binds the 3D class to its subchannel. */
	at = draw_set(push, at, M_SET_OBJECT, threed_class);

	/* No tessellation, rendering on, compression on, one colour target. */
	at = draw_set(push, at, M_SET_TESSELLATION_PARAMETERS, 0U);
	at = draw_set(push, at, M_SET_RENDER_ENABLE_C, V_RENDER_ENABLE_TRUE);
	at = draw_set(push, at, M_SET_Z_COMPRESSION, V_TRUE);
	at = draw_method(push, at, M_SET_COLOR_COMPRESSION, DRAW_COLOR_TARGETS);
	for (index = 0U; index < DRAW_COLOR_TARGETS; index++) {
		nvrtx_put32(push, at, V_TRUE);
		at += 4U;
	}
	at = draw_set(push, at, M_SET_CT_SELECT, V_CT_SELECT_ONE_TARGET);

	/* Lines, restart, blending, the single write control, stencil and alpha to coverage. */
	at = draw_set(push, at, M_SET_ALIASED_LINE_WIDTH_ENABLE, V_TRUE);
	at = draw_set(push, at, M_SET_DA_PRIMITIVE_RESTART_VERTEX_ARRAY, V_FALSE);
	at = draw_set(push, at, M_SET_BLEND_SEPARATE_FOR_ALPHA, V_TRUE);
	at = draw_set(push, at, M_SET_SINGLE_CT_WRITE_CONTROL, V_TRUE);
	at = draw_set(push, at, M_SET_SINGLE_ROP_CONTROL, V_FALSE);
	at = draw_set(push, at, M_SET_TWO_SIDED_STENCIL_TEST, V_TRUE);
	at = draw_set(push, at, M_SET_ALPHA_TO_COVERAGE_OVERRIDE, V_ALPHA_TO_COVERAGE_BY_AA);

	/* Shading, the call limit, culling statistics, the L1 split. */
	at = draw_set(push, at, M_SET_SHADE_MODE, V_SHADE_OGL_SMOOTH);
	at = draw_set(push, at, M_SET_API_VISIBLE_CALL_LIMIT, V_CALL_LIMIT_128);
	at = draw_set(push, at, M_SET_ZCULL_STATS, V_TRUE);
	at = draw_set(push, at, M_SET_L1_CONFIGURATION, V_L1_48KB);

	/* The colour reduction thresholds, off. */
	at = draw_set(push, at, M_SET_REDUCE_COLOR_THRESHOLDS_ENABLE, V_FALSE);
	at = draw_set(push, at, M_SET_REDUCE_COLOR_THRESHOLDS_UNORM8, V_REDUCE_ALL_HIT_ONCE);
	at = draw_method(push, at, M_SET_REDUCE_COLOR_THRESHOLDS_UNORM10, 5U);
	nvrtx_put32(push, at, V_REDUCE_ALL_HIT_ONCE);
	nvrtx_put32(push, at + 4U, V_REDUCE_ALL_HIT_ONCE);
	nvrtx_put32(push, at + 8U, V_REDUCE_FP11_ALL_HIT_ONCE);
	nvrtx_put32(push, at + 12U, V_REDUCE_ALL_HIT_ONCE);
	nvrtx_put32(push, at + 16U, V_REDUCE_ALL_HIT_ONCE);
	at += 20U;

	/* The program header and anti-aliasing versions the driver's programs follow. */
	at = draw_set(push, at, M_CHECK_SPH_VERSION, V_SPH_VERSION_3);
	at = draw_set(push, at, M_CHECK_AAM_VERSION, V_AAM_VERSION_2);

	/* The raster operations' L2 policies. */
	at = draw_set(push, at, M_SET_L2_ROP_PREFETCH_READ, V_L2_EVICT_NORMAL);
	at = draw_set(push, at, M_SET_L2_ROP_NONINTERLOCKED_READ, V_L2_EVICT_NORMAL);
	at = draw_set(push, at, M_SET_L2_ROP_INTERLOCKED_READ, V_L2_EVICT_NORMAL);
	at = draw_set(push, at, M_SET_L2_ROP_NONINTERLOCKED_WRITE, V_L2_EVICT_NORMAL);
	at = draw_set(push, at, M_SET_L2_ROP_INTERLOCKED_WRITE, V_L2_EVICT_NORMAL);

	/* Blending per format, the attribute defaults, the vertex number, the render enable and the sample mask. */
	at = draw_set(push, at, M_SET_BLEND_PER_FORMAT_ENABLE, V_BLEND_PER_FORMAT_SNORM8);
	at = draw_set(push, at, M_SET_ATTRIBUTE_DEFAULT, V_ATTRIBUTE_DEFAULT_0001);
	at = draw_set(push, at, M_SET_DA_OUTPUT, V_DA_VERTEX_ID_ARRAY_START);
	at = draw_set(push, at, M_SET_RENDER_ENABLE_CONTROL, V_FALSE);
	at = draw_set(push, at, M_SET_PS_OUTPUT_SAMPLE_MASK_USAGE, V_SAMPLE_MASK_USAGE);

	/* Blend options, the alpha test, lighting, clamping and saturation. */
	at = draw_set(push, at, M_SET_BLEND_OPT_CONTROL, V_TRUE);
	at = draw_set(push, at, M_SET_BLEND_FLOAT_OPTION, V_TRUE);
	at = draw_set(push, at, M_SET_BLEND_STATE_PER_TARGET, V_TRUE);
	at = draw_set(push, at, M_SET_ALPHA_TEST, V_FALSE);
	at = draw_set(push, at, M_SET_TWO_SIDED_LIGHT, V_FALSE);
	at = draw_set(push, at, M_SET_COLOR_CLAMP, V_TRUE);
	at = draw_set(push, at, M_SET_PS_SATURATE, 0U);
	at = draw_set(push, at, M_SET_ZPASS_PIXEL_COUNT, V_TRUE);

	/* Points: size one, sprites with the origin at the top, not smoothed. */
	at = draw_set(push, at, M_SET_POINT_SIZE, V_FLOAT_ONE);
	at = draw_set(push, at, M_SET_ATTRIBUTE_POINT_SIZE, V_TRUE);
	at = draw_set(push, at, M_SET_POINT_SPRITE, V_TRUE);
	at = draw_set(push, at, M_SET_POINT_SPRITE_SELECT, V_POINT_SPRITE_ORIGIN_TOP);
	at = draw_set(push, at, M_SET_ANTI_ALIASED_POINT, V_FALSE);
	at = draw_set(push, at, M_SET_FILL_VIA_TRIANGLE, 0U);
	at = draw_set(push, at, M_SET_POLY_SMOOTH, V_FALSE);

	/* Pixel centres at half integers, multisample rasterization on, the coverage. */
	at = draw_set(push, at, M_SET_VIEWPORT_PIXEL, 0U);
	at = draw_set(push, at, M_SET_ANTI_ALIAS_ENABLE, V_TRUE);
	at = draw_set(push, at, M_SET_POST_PS_INITIAL_COVERAGE, V_TRUE);
	at = draw_set(push, at, M_SET_OFFSET_RENDER_TARGET_INDEX, V_FALSE);

	/* The window: origin at the upper left, no offset, no window or clip id test. */
	at = draw_set(push, at, M_SET_WINDOW_ORIGIN, 0U);
	at = draw_method(push, at, M_SET_WINDOW_OFFSET_X, 2U);
	nvrtx_put32(push, at, 0U);
	nvrtx_put32(push, at + 4U, 0U);
	at += 8U;
	at = draw_set(push, at, M_SET_ACTIVE_ZCULL_REGION, V_ZCULL_REGION_ALL);
	at = draw_set(push, at, M_SET_WINDOW_CLIP_ENABLE, V_FALSE);
	at = draw_set(push, at, M_SET_CLIP_ID_TEST, V_FALSE);

	/* The viewport transform on and its clip. */
	at = draw_set(push, at, M_SET_VIEWPORT_SCALE_OFFSET, V_TRUE);
	at = draw_set(push, at, M_SET_VIEWPORT_CLIP_CONTROL, V_VIEWPORT_CLIP_INIT);

	/* Every scissor off. */
	for (index = 0U; index < DRAW_SCISSORS; index++)
		at = draw_set(push, at, M_SET_SCISSOR_ENABLE + index * M_SET_SCISSOR_STRIDE, V_FALSE);

	/*
	 * Several colour targets, and Turing's variable pixel rate sample
	 * order.
	 */
	at = draw_set(push, at, M_SET_CT_MRT_ENABLE, V_TRUE);
	at = draw_method(push, at, M_SET_VARIABLE_PIXEL_RATE_SAMPLE_ORDER, DRAW_SAMPLE_ORDERS);
	for (index = 0U; index < DRAW_SAMPLE_ORDERS; index++) {
		nvrtx_put32(push, at, sample_order[index]);
		at += 4U;
	}

	/* The local memory window at the top of the low 4 GiB. */
	at = draw_set(push, at, M_SET_SHADER_LOCAL_MEMORY_WINDOW, V_LOCAL_MEMORY_WINDOW);

	/* Every constant buffer slot of every group unbound. */
	for (index = 0U; index < DRAW_GROUPS; index++) {
		for (slot = 0U; slot < DRAW_GROUP_SLOTS; slot++)
			at = draw_set(push, at, M_BIND_GROUP_CONSTANT_BUFFER + index * M_BIND_GROUP_STRIDE, slot << V_GROUP_SLOT_SHIFT);
	}

	/* The layer, point centres, edge flags and samplers bound apart from headers. */
	at = draw_set(push, at, M_SET_RT_LAYER, 0U);
	at = draw_set(push, at, M_SET_POINT_CENTER_MODE, 0U);
	at = draw_set(push, at, M_SET_EDGE_FLAG, V_TRUE);
	at = draw_set(push, at, M_SET_SAMPLER_BINDING, 0U);

	/* A missing vertex stream reads the zero page. */
	at = draw_method(push, at, M_SET_VERTEX_STREAM_SUBSTITUTE_A, 2U);
	nvrtx_put32(push, at, (uint32_t)(zero_page >> 32));
	nvrtx_put32(push, at + 4U, (uint32_t)(zero_page & 0xffffffffU));
	at += 8U;

	/* The pixel programs' watermarks (for Turing's 64 warps a TPC). */
	at = draw_set(push, at, M_SET_PS_WARP_WATERMARKS, V_PS_WARP_WATERMARKS);
	at = draw_set(push, at, M_SET_PS_REGISTER_WATERMARKS, V_PS_REGISTER_WATERMARKS);

	/* Every vertex stream off, and the base instance zero. */
	for (index = 0U; index < DRAW_VERTEX_STREAMS; index++)
		at = draw_set(push, at, M_SET_VERTEX_STREAM_A_FORMAT + index * M_SET_VERTEX_STREAM_A_STRIDE, 0U);
	at = draw_set(push, at, M_SET_GLOBAL_BASE_INSTANCE_INDEX, 0U);

	/* Succeeded: the queue's state is set. */
	return at;
}

/*
 * Selects constant buffer 0 (its size and address) and binds it to slot 0
 * of every program group, once for the root table.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_mme_select_cb0, nvk_push_draw_state_init).
 */
size_t
nvrtx_push_draw_constants(
	uint8_t *push,
	size_t at,
	uint64_t address,
	uint32_t size)
{
	uint32_t group;

	/* The buffer: its size, then its address. */
	at = draw_method(push, at, M_SET_CONSTANT_BUFFER_SELECTOR_A, 3U);
	nvrtx_put32(push, at, size);
	nvrtx_put32(push, at + 4U, (uint32_t)(address >> 32));
	nvrtx_put32(push, at + 8U, (uint32_t)(address & 0xffffffffU));
	at += 12U;

	/* Slot 0 of each group. */
	for (group = 0U; group < DRAW_GROUPS; group++)
		at = draw_set(push, at, M_BIND_GROUP_CONSTANT_BUFFER + group * M_BIND_GROUP_STRIDE, V_GROUP_VALID);

	/* Succeeded: every program reads the buffer as its constant buffer 0. */
	return at;
}

/*
 * Writes one draw's target and state: the surface clip, colour target 0
 * pitch-linear and the seven others null, no depth target, one sample;
 * the viewport over the whole target with depth 0 to 1; filled triangles,
 * no culling; every sample, no depth, stencil, logic operation or
 * blending, all four channels written.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_CmdBeginRendering, nvk_flush_vp_state, nvk_flush_rs_state,
 * nvk_flush_ms_state, nvk_flush_ds_state, nvk_flush_cb_state).
 */
size_t
nvrtx_push_draw_target(
	uint8_t *push,
	size_t at,
	const struct nvrtx_draw_target *target)
{
	uint32_t index;
	uint32_t half_width;
	uint32_t half_height;

	/* The viewport's scale and offset: half the target's size, as floats. */
	half_width = draw_float(target->width / 2U);
	half_height = draw_float(target->height / 2U);

	/* The surface clip: the whole target. */
	at = draw_method(push, at, M_SET_SURFACE_CLIP_HORIZONTAL, 2U);
	nvrtx_put32(push, at, target->width << 16);
	nvrtx_put32(push, at + 4U, target->height << 16);
	at += 8U;

	/* Colour target 0: the address, the pitch in place of the width, the height, the format, pitch layout, one layer. */
	at = draw_method(push, at, M_SET_COLOR_TARGET_A, 9U);
	nvrtx_put32(push, at, (uint32_t)(target->address >> 32));
	nvrtx_put32(push, at + 4U, (uint32_t)(target->address & 0xffffffffU));
	nvrtx_put32(push, at + 8U, target->pitch);
	nvrtx_put32(push, at + 12U, target->height);
	nvrtx_put32(push, at + 16U, NVRTX_DRAW_FORMAT_A8B8G8R8);
	nvrtx_put32(push, at + 20U, V_TARGET_LAYOUT_PITCH);
	nvrtx_put32(push, at + 24U, 1U);
	nvrtx_put32(push, at + 28U, 0U);
	nvrtx_put32(push, at + 32U, 0U);
	at += 36U;
	at = draw_set(push, at, M_SET_COLOR_COMPRESSION, V_FALSE);

	/* The other targets null, so no pointer of an earlier pass is left. */
	for (index = 1U; index < DRAW_COLOR_TARGETS; index++)
		at = draw_null_target(push, at, index);

	/* No depth target, one sample a pixel. */
	at = draw_set(push, at, M_SET_ZT_SELECT, 0U);
	at = draw_set(push, at, M_SET_ANTI_ALIAS, V_ANTI_ALIAS_1X1);

	/*
	 * Viewport 0 over the target: scale and offset half its size, depth 0
	 * to 1.
	 */
	at = draw_method(push, at, M_SET_VIEWPORT_SCALE_X, 6U);
	nvrtx_put32(push, at, half_width);
	nvrtx_put32(push, at + 4U, half_height);
	nvrtx_put32(push, at + 8U, V_FLOAT_ONE);
	nvrtx_put32(push, at + 12U, half_width);
	nvrtx_put32(push, at + 16U, half_height);
	nvrtx_put32(push, at + 20U, 0U);
	at += 24U;
	at = draw_method(push, at, M_SET_VIEWPORT_CLIP_HORIZONTAL, 4U);
	nvrtx_put32(push, at, target->width << 16);
	nvrtx_put32(push, at + 4U, target->height << 16);
	nvrtx_put32(push, at + 8U, 0U);
	nvrtx_put32(push, at + 12U, V_FLOAT_ONE);
	at += 16U;
	at = draw_set(push, at, M_SET_VIEWPORT_COORDINATE_SWIZZLE, V_VIEWPORT_SWIZZLE_IDENTITY);
	at = draw_set(push, at, M_SET_VIEWPORT_Z_CLIP, V_Z_CLIP_ZERO_TO_W);

	/*
	 * Rasterization: on, clipped in depth by the geometry clipper, filled,
	 * not culled.
	 */
	at = draw_set(push, at, M_SET_RASTER_ENABLE, V_TRUE);
	at = draw_set(push, at, M_SET_VIEWPORT_CLIP_CONTROL, V_VIEWPORT_CLIP_DRAW);
	at = draw_method(push, at, M_SET_FRONT_POLYGON_MODE, 2U);
	nvrtx_put32(push, at, V_POLYGON_FILL);
	nvrtx_put32(push, at + 4U, V_POLYGON_FILL);
	at += 8U;
	at = draw_set(push, at, M_OGL_SET_CULL, V_FALSE);
	at = draw_set(push, at, M_OGL_SET_FRONT_FACE, V_FRONT_FACE_CCW);
	at = draw_set(push, at, M_SET_PROVOKING_VERTEX, 0U);

	/* Samples: no alpha to coverage, every sample. */
	at = draw_set(push, at, M_SET_ANTI_ALIAS_ALPHA_CONTROL, 0U);
	at = draw_method(push, at, M_SET_SAMPLE_MASK_X0_Y0, 4U);
	for (index = 0U; index < 4U; index++)
		nvrtx_put32(push, at + index * 4U, V_SAMPLE_MASK_ALL);
	at += 16U;

	/* No depth or stencil test. */
	at = draw_set(push, at, M_SET_DEPTH_TEST, V_FALSE);
	at = draw_set(push, at, M_SET_DEPTH_WRITE, V_FALSE);
	at = draw_set(push, at, M_SET_STENCIL_TEST, V_FALSE);

	/*
	 * Colour: no logic operation or blending, all four channels, target 0
	 * at location 0.
	 */
	at = draw_set(push, at, M_SET_LOGIC_OP, V_FALSE);
	at = draw_set(push, at, M_SET_BLEND, V_FALSE);
	at = draw_set(push, at, M_SET_CT_WRITE, V_CT_WRITE_RGBA);
	at = draw_set(push, at, M_SET_SINGLE_CT_WRITE_CONTROL, V_TRUE);
	at = draw_set(push, at, M_SET_CT_SELECT, V_CT_SELECT_ONE_TARGET);

	/* Succeeded: the draw renders into the target. */
	return at;
}

/*
 * Writes the pipeline in the order of its types: a vertex program at type
 * 1 and a pixel program at type 5 (each index the same as its type, each at
 * its header's address, in its group), the other stages off, the pixel
 * program's tuning and what the shading rate and anti-aliasing macros set
 * for it, then the last vertex stage's layer and clip planes.  The
 * shader caches are not touched: nvrtx_push_draw_invalidate() empties them
 * once at the push buffer's start, so draws one after another do not wait
 * for each other's idle.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_shader.c
 * (nvk_shader_fill_push) and src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_mme_set_shading_rate_control, nvk_mme_set_anti_alias).
 */
size_t
nvrtx_push_draw_pipeline(
	uint8_t *push,
	size_t at,
	uint64_t vertex,
	uint32_t vertex_gprs,
	uint64_t pixel,
	uint32_t pixel_gprs)
{
	uint32_t type;
	uint32_t index;

	/*
	 * Each type of a Vulkan stage, from the vertex program to the pixel
	 * program: the program, or off.  Type 0 (the vertex program culling
	 * before the fetch) is left as the golden context has it.
	 */
	for (type = NVRTX_DRAW_TYPE_VERTEX; type < NVRTX_DRAW_TYPES; type++) {
		if (type == NVRTX_DRAW_TYPE_VERTEX) {
			at = draw_program(push, at, type, vertex, vertex_gprs, V_GROUP_VERTEX);
		} else if (type == NVRTX_DRAW_TYPE_PIXEL) {
			at = draw_program(push, at, type, pixel, pixel_gprs, V_GROUP_PIXEL);
		} else {
			at = draw_set(push, at, M_SET_PIPELINE_SHADER + type * M_SET_PIPELINE_STRIDE, type << 4);
		}
	}

	/*
	 * The pixel program: its subtiling, no early depth, no post-depth
	 * coverage, bounded culling.
	 */
	at = draw_method(push, at, M_SET_SUBTILING_PERF_KNOB_A, 2U);
	nvrtx_put32(push, at, V_SUBTILING_A);
	nvrtx_put32(push, at + 4U, V_SUBTILING_B);
	at += 8U;
	at = draw_set(push, at, M_SET_API_MANDATED_EARLY_Z, V_FALSE);
	at = draw_set(push, at, M_SET_POST_Z_PS_IMASK, V_FALSE);
	at = draw_set(push, at, M_SET_ZCULL_BOUNDS, 0U);

	/*
	 * What the shading rate and anti-aliasing macros write for a pixel
	 * program without sample shading at one sample a pixel: every
	 * variable pixel rate shading control off, then one pass a pixel with
	 * the centroid per fragment.
	 */
	for (index = 0U; index < DRAW_SHADING_RATE_CONTROLS; index++)
		at = draw_set(push, at, M_SET_VARIABLE_PIXEL_RATE_SHADING_CONTROL + index * M_SET_SHADING_RATE_STRIDE, V_FALSE);
	at = draw_set(push, at, M_SET_HYBRID_ANTI_ALIAS_CONTROL, V_HYBRID_ONE_PASS);

	/* The vertex program, the last before rasterization: the layer from the state, no clip planes. */
	at = draw_set(push, at, M_SET_RT_LAYER, 0U);
	at = draw_set(push, at, M_SET_USER_CLIP_ENABLE, 0U);
	at = draw_set(push, at, M_SET_USER_CLIP_OP, 0U);

	/* Succeeded: the pipeline is bound. */
	return at;
}

/*
 * Writes one program of a bound pipeline anew (the vertex program, or the
 * pixel program when pixel is set): what nvrtx_push_draw_pipeline() wrote
 * for the others stays.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_shader.c
 * (nvk_shader_fill_push).
 */
size_t
nvrtx_push_draw_program(
	uint8_t *push,
	size_t at,
	bool pixel,
	uint64_t address,
	uint32_t gprs)
{
	/* The pixel program, or the vertex program. */
	if (pixel) {
		at = draw_program(push, at, NVRTX_DRAW_TYPE_PIXEL, address, gprs, V_GROUP_PIXEL);
		return at;
	}
	at = draw_program(push, at, NVRTX_DRAW_TYPE_VERTEX, address, gprs, V_GROUP_VERTEX);

	/* Succeeded: the program is bound. */
	return at;
}

/*
 * Writes the invalidation of the shader caches a push buffer's draws read:
 * the programs' instructions, the global data and the constants, without
 * waiting for idle.
 *
 * The CPU writes programs and constants between runs, and a push buffer
 * starts after the run before it finished, so one invalidation at its
 * start serves every draw in it, as at a command buffer's start.  The form
 * that waits for idle (0x021c) is not used: on every draw it drained the
 * pipeline.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_cmd_buffer_begin_graphics).
 */
size_t
nvrtx_push_draw_invalidate(
	uint8_t *push,
	size_t at)
{
	/* The instruction, global data and constant caches, without a wait for idle. */
	at = draw_set(push, at, M_INVALIDATE_SHADER_CACHES_NO_WFI, V_INVALIDATE_SHADERS);

	/* Succeeded: the draws after it read the code and constants as written. */
	return at;
}

/*
 * Writes the vertex input: attribute 0 two floats from stream 0, the
 * others inactive; stream 0 per vertex at the buffer with its stride and
 * size.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_flush_vi_state, nvk_mme_bind_vb).
 */
size_t
nvrtx_push_draw_vertices(
	uint8_t *push,
	size_t at,
	uint64_t address,
	uint32_t bytes,
	uint32_t stride)
{
	uint32_t index;

	/* The attributes: 0 active, the rest zero. */
	at = draw_method(push, at, M_SET_VERTEX_ATTRIBUTE_A, DRAW_VERTEX_ATTRIBUTES);
	nvrtx_put32(push, at, V_ATTRIBUTE_R32_G32_FLOAT);
	for (index = 1U; index < DRAW_VERTEX_ATTRIBUTES; index++)
		nvrtx_put32(push, at + index * 4U, V_ATTRIBUTE_INACTIVE);
	at += DRAW_VERTEX_ATTRIBUTES * 4U;

	/* Stream 0 per vertex, enabled with its stride. */
	at = draw_set(push, at, M_SET_VERTEX_STREAM_INSTANCE_A, V_FALSE);
	at = draw_method(push, at, M_SET_VERTEX_STREAM_A_FORMAT, 4U);
	nvrtx_put32(push, at, stride | V_STREAM_ENABLE);
	nvrtx_put32(push, at + 4U, (uint32_t)(address >> 32));
	nvrtx_put32(push, at + 8U, (uint32_t)(address & 0xffffffffU));
	nvrtx_put32(push, at + 12U, 0U);
	at += 16U;

	/* Its size, past which the stream reads zero. */
	at = draw_method(push, at, M_SET_VERTEX_STREAM_SIZE_A, 2U);
	nvrtx_put32(push, at, 0U);
	nvrtx_put32(push, at + 4U, bytes);
	at += 8U;

	/* Succeeded: the vertices are bound. */
	return at;
}

/*
 * Writes the texture header pool and the sampler pool (their addresses
 * and last indices), each followed by the invalidation of its cache.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_queue.c
 * (nvk_queue_state_update).
 */
size_t
nvrtx_push_draw_textures(
	uint8_t *push,
	size_t at,
	uint64_t headers,
	uint32_t header_count,
	uint64_t samplers,
	uint32_t sampler_count)
{
	/* The headers. */
	at = draw_method(push, at, M_SET_TEX_HEADER_POOL_A, 3U);
	nvrtx_put32(push, at, (uint32_t)(headers >> 32));
	nvrtx_put32(push, at + 4U, (uint32_t)(headers & 0xffffffffU));
	nvrtx_put32(push, at + 8U, header_count - 1U);
	at += 12U;
	at = draw_set(push, at, M_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, 0U);

	/* The samplers. */
	at = draw_method(push, at, M_SET_TEX_SAMPLER_POOL_A, 3U);
	nvrtx_put32(push, at, (uint32_t)(samplers >> 32));
	nvrtx_put32(push, at + 4U, (uint32_t)(samplers & 0xffffffffU));
	nvrtx_put32(push, at + 8U, sampler_count - 1U);
	at += 12U;
	at = draw_set(push, at, M_INVALIDATE_SAMPLER_CACHE_NO_WFI, 0U);

	/* Succeeded: the programs' handles index these pools. */
	return at;
}

/*
 * Writes a clear of colour target 0: rendered whatever the render enable
 * says, by the clear rectangle, to the colour (the channels' float bits),
 * all four channels of layer 0.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_cmd_clear.c
 * (nvk_CmdClearAttachments).
 */
size_t
nvrtx_push_draw_clear(
	uint8_t *push,
	size_t at,
	const uint32_t *colour,
	uint32_t width,
	uint32_t height)
{
	uint32_t index;

	/* The clear renders even under a conditional render. */
	at = draw_set(push, at, M_SET_RENDER_ENABLE_OVERRIDE, V_RENDER_ALWAYS);
	at = draw_set(push, at, M_SET_CLEAR_SURFACE_CONTROL, V_CLEAR_USE_RECT);

	/* The colour. */
	at = draw_method(push, at, M_SET_COLOR_CLEAR_VALUE, 4U);
	for (index = 0U; index < 4U; index++)
		nvrtx_put32(push, at + index * 4U, colour[index]);
	at += 16U;

	/* The rectangle, then the clear. */
	at = draw_method(push, at, M_SET_CLEAR_RECT_HORIZONTAL, 2U);
	nvrtx_put32(push, at, width << 16);
	nvrtx_put32(push, at + 4U, height << 16);
	at += 8U;
	at = draw_set(push, at, M_CLEAR_SURFACE, V_CLEAR_RGBA);

	/* Back to the render enable. */
	at = draw_set(push, at, M_SET_RENDER_ENABLE_OVERRIDE, V_RENDER_USE_ENABLE);

	/* Succeeded: the target is cleared. */
	return at;
}

/*
 * Writes a draw of a triangle list: the bases zero, one instance, then
 * BEGIN, the vertex range and END, sent directly rather than through a
 * draw macro.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_mme_build_draw_loop).
 */
size_t
nvrtx_push_draw_triangles(
	uint8_t *push,
	size_t at,
	uint32_t first,
	uint32_t count)
{
	/* The vertex numbers start from the first, the instance from zero. */
	at = draw_set(push, at, M_SET_GLOBAL_BASE_VERTEX_INDEX, 0U);
	at = draw_set(push, at, M_SET_VERTEX_ID_BASE, 0U);
	at = draw_set(push, at, M_SET_INSTANCE_COUNT, 1U);

	/* The primitives. */
	at = draw_set(push, at, M_BEGIN, V_BEGIN_TRIANGLES);
	at = draw_method(push, at, M_SET_VERTEX_ARRAY_START, 2U);
	nvrtx_put32(push, at, first);
	nvrtx_put32(push, at + 4U, count);
	at += 8U;
	at = draw_set(push, at, M_END, 0U);

	/* Succeeded: the draw is written. */
	return at;
}

/*
 * Writes the texture header of a pitch-linear 2D texture of A8B8G8R8
 * UNORM, the Maxwell-and-later pitch header for a view of one level: the
 * format and its swizzle, the address, the pitch, the size, and the
 * quality, promotion, border and spread fields.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/nil/descriptor.rs
 * (nvb097_fill_image_view_desc).
 */
void
nvrtx_draw_texture_header(
	uint32_t *header,
	const struct nvrtx_draw_texture *texture)
{
	/* Starts empty. */
	kern_memset(header, 0, NVRTX_DRAW_TIC_WORDS * sizeof(uint32_t));

	/* The format: four 8-bit unsigned normalized channels read as R, G, B, A. */
	draw_field(header, 0U, 7U, TIC_COMPONENTS_A8B8G8R8);
	draw_field(header, 7U, 3U, TIC_TYPE_UNORM);
	draw_field(header, 10U, 3U, TIC_TYPE_UNORM);
	draw_field(header, 13U, 3U, TIC_TYPE_UNORM);
	draw_field(header, 16U, 3U, TIC_TYPE_UNORM);
	draw_field(header, 19U, 3U, TIC_SOURCE_R);
	draw_field(header, 22U, 3U, TIC_SOURCE_G);
	draw_field(header, 25U, 3U, TIC_SOURCE_B);
	draw_field(header, 28U, 3U, TIC_SOURCE_A);

	/* The address (bits 31:5 and 48:32), the pitch header, the pitch (bits 20:5). */
	draw_field(header, 37U, 27U, (uint32_t)((texture->address & 0xffffffffU) >> 5));
	draw_field(header, 64U, 17U, (uint32_t)(texture->address >> 32));
	draw_field(header, 85U, 3U, TIC_VERSION_PITCH);
	draw_field(header, 96U, 16U, texture->pitch >> 5);

	/* The level of detail quality, high. */
	draw_field(header, 112U, 1U, 1U);
	draw_field(header, 113U, 1U, 1U);
	draw_field(header, 114U, 1U, 1U);

	/* The size and the type: 2D without mipmaps, one level, depth one. */
	draw_field(header, 128U, 17U, texture->width - 1U);
	draw_field(header, 151U, 4U, TIC_TYPE_TWO_D_NO_MIPMAP);
	draw_field(header, 160U, 16U, texture->height - 1U);

	/*
	 * The sector promotion, border from the sampler, normalized coordinates
	 * and spread functions.
	 */
	draw_field(header, 155U, 2U, TIC_PROMOTE_TO_2_V);
	draw_field(header, 157U, 3U, TIC_BORDER_SAMPLER_COLOR);
	draw_field(header, 191U, 1U, 1U);
	draw_field(header, 215U, 2U, TIC_SPREAD_FUNC_TWO);
	draw_field(header, 217U, 2U, TIC_SPREAD_FUNC_ONE);
}

/*
 * Writes a sampler for nearest filtering with clamped coordinates and one
 * level: clamp to edge in each direction, the font filter and sRGB border
 * fields, point filters, the seamless cube setting, no reduction, the
 * header's normalization, levels of detail 0 to 0.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_sampler.c
 * (nvk_sampler_get_header).
 */
void
nvrtx_draw_sampler(
	uint32_t *sampler)
{
	/* Starts empty. */
	kern_memset(sampler, 0, NVRTX_DRAW_TSC_WORDS * sizeof(uint32_t));

	/* Word 0: the address modes, the border conversion and the font filter. */
	draw_field(sampler, 0U, 3U, TSC_CLAMP_TO_EDGE);
	draw_field(sampler, 3U, 3U, TSC_CLAMP_TO_EDGE);
	draw_field(sampler, 6U, 3U, TSC_CLAMP_TO_EDGE);
	draw_field(sampler, 13U, 1U, 1U);
	draw_field(sampler, 14U, 3U, TSC_FONT_SIZE_2);
	draw_field(sampler, 17U, 3U, TSC_FONT_SIZE_2);

	/* Word 1: the filters and the cube seams. */
	draw_field(sampler, 32U + 0U, 3U, TSC_MAG_POINT);
	draw_field(sampler, 32U + 4U, 2U, TSC_MIN_POINT);
	draw_field(sampler, 32U + 6U, 2U, TSC_MIP_POINT);
	draw_field(sampler, 32U + 8U, 2U, TSC_CUBE_AUTO_SPAN_SEAM);
}

/*
 * Builds the IR of the quad's vertex program, as the frontend would parse
 *
 *   layout(push_constant) uniform Q { vec4 rect; vec4 texture; vec4 colour; } q;
 *   layout(location = 0) in vec2 corner;
 *   layout(location = 0) out vec2 texcoord;      (textured only)
 *   void main() {
 *       gl_Position = vec4(mix(q.rect.xy, q.rect.zw, corner), 0.0, 1.0);
 *       texcoord = mix(q.texture.xy, q.texture.zw, corner);
 *   }
 *
 * (the compositor's quad.vert, mix written as a + (b - a) * t).
 */
void
nvrtx_draw_vertex_ir(
	struct nvrtx_draw_ir *program,
	int textured)
{
	struct drv_gpu_shader_ir_inst *list;
	struct drv_gpu_shader_ir *ir;
	uint32_t count;

	/* Starts empty, the instructions appended in order. */
	kern_memset(program, 0, sizeof(*program));
	list = program->instructions;
	ir = &program->ir;
	count = 0U;

	/* v0, v1 = the corner; v2..v5 = the rectangle's corners. */
	draw_ir_load(list, &count, 0U, DRV_GPU_IR_LOAD_INPUT, 0U, 0U, 0U);
	draw_ir_load(list, &count, 1U, DRV_GPU_IR_LOAD_INPUT, 0U, 1U, 0U);
	draw_ir_load(list, &count, 2U, DRV_GPU_IR_LOAD_PUSH, 0U, 0U, NVRTX_DRAW_PUSH_RECT);
	draw_ir_load(list, &count, 3U, DRV_GPU_IR_LOAD_PUSH, 0U, 0U, NVRTX_DRAW_PUSH_RECT + 4U);
	draw_ir_load(list, &count, 4U, DRV_GPU_IR_LOAD_PUSH, 0U, 0U, NVRTX_DRAW_PUSH_RECT + 8U);
	draw_ir_load(list, &count, 5U, DRV_GPU_IR_LOAD_PUSH, 0U, 0U, NVRTX_DRAW_PUSH_RECT + 12U);

	/* v8 = x, v11 = y; v12 = 0, v13 = 1; the position. */
	draw_ir_mix(list, &count, 6U, 0U, 2U, 4U);
	draw_ir_mix(list, &count, 9U, 1U, 3U, 5U);
	draw_ir_load(list, &count, 12U, DRV_GPU_IR_CONST, 0U, 0U, IR_FLOAT_ZERO);
	draw_ir_load(list, &count, 13U, DRV_GPU_IR_CONST, 0U, 0U, IR_FLOAT_ONE);
	draw_ir_store(list, &count, DRV_GPU_IR_LOCATION_POSITION, 0U, 8U);
	draw_ir_store(list, &count, DRV_GPU_IR_LOCATION_POSITION, 1U, 11U);
	draw_ir_store(list, &count, DRV_GPU_IR_LOCATION_POSITION, 2U, 12U);
	draw_ir_store(list, &count, DRV_GPU_IR_LOCATION_POSITION, 3U, 13U);
	ir->value_count = 14U;

	/* Textured: v14..v17 = the texture's part, v20 and v23 = the coordinate at location 0. */
	if (textured != 0) {
		draw_ir_load(list, &count, 14U, DRV_GPU_IR_LOAD_PUSH, 0U, 0U, NVRTX_DRAW_PUSH_TEXTURE);
		draw_ir_load(list, &count, 15U, DRV_GPU_IR_LOAD_PUSH, 0U, 0U, NVRTX_DRAW_PUSH_TEXTURE + 4U);
		draw_ir_load(list, &count, 16U, DRV_GPU_IR_LOAD_PUSH, 0U, 0U, NVRTX_DRAW_PUSH_TEXTURE + 8U);
		draw_ir_load(list, &count, 17U, DRV_GPU_IR_LOAD_PUSH, 0U, 0U, NVRTX_DRAW_PUSH_TEXTURE + 12U);
		draw_ir_mix(list, &count, 18U, 0U, 14U, 16U);
		draw_ir_mix(list, &count, 21U, 1U, 15U, 17U);
		draw_ir_store(list, &count, 0U, 0U, 20U);
		draw_ir_store(list, &count, 0U, 1U, 23U);
		ir->value_count = 24U;
		program->output.location = 0U;
		program->output.components = 2U;
		ir->outputs = &program->output;
		ir->output_count = 1U;
	}

	/* The corner is input 0. */
	program->input.location = 0U;
	program->input.components = 2U;

	/* The program. */
	ir->stage = DRV_GPU_STAGE_VERTEX;
	ir->inputs = &program->input;
	ir->input_count = 1U;
	ir->instructions = list;
	ir->instruction_count = count;
	ir->push_bytes = NVRTX_DRAW_PUSH_BYTES;
}

/*
 * Builds the IR of the quad's fragment program, as the frontend would
 * parse
 *
 *   layout(location = 0) out vec4 colour;
 *   void main() { colour = q.colour; }                       (one colour)
 *
 *   layout(set = 0, binding = 0) uniform sampler2D image;
 *   layout(location = 0) in vec2 texcoord;
 *   void main() { colour = texture(image, texcoord); }       (textured,
 *                                                             quad.frag)
 */
void
nvrtx_draw_fragment_ir(
	struct nvrtx_draw_ir *program,
	int textured)
{
	struct drv_gpu_shader_ir_inst *list;
	struct drv_gpu_shader_ir *ir;
	uint32_t component;
	uint32_t colour;
	uint32_t count;

	/* Starts empty, the instructions appended in order. */
	kern_memset(program, 0, sizeof(*program));
	list = program->instructions;
	ir = &program->ir;
	count = 0U;

	/*
	 * Textured: v0, v1 = the coordinate, v2..v5 = the sample of image
	 * (0, 0) there.  One colour: v0..v3 = the push constants' colour.
	 */
	if (textured != 0) {
		draw_ir_load(list, &count, 0U, DRV_GPU_IR_LOAD_INPUT, 0U, 0U, 0U);
		draw_ir_load(list, &count, 1U, DRV_GPU_IR_LOAD_INPUT, 0U, 1U, 0U);
		list[count].op = DRV_GPU_IR_SAMPLE;
		list[count].dst = 2U;
		list[count].src[0] = 0U;
		list[count].src[1] = 1U;
		list[count].location = 0U;
		list[count].immediate = 0U;
		count++;
		colour = 2U;
		program->input.location = 0U;
		program->input.components = 2U;
		program->uniform.set = 0U;
		program->uniform.binding = 0U;
		program->uniform.kind = DRV_GPU_IR_UNIFORM_SAMPLED_IMAGE;
		ir->inputs = &program->input;
		ir->input_count = 1U;
		ir->uniforms = &program->uniform;
		ir->uniform_count = 1U;
	} else {
		for (component = 0U; component < 4U; component++)
			draw_ir_load(list, &count, component, DRV_GPU_IR_LOAD_PUSH, 0U, 0U, NVRTX_DRAW_PUSH_COLOR + component * 4U);
		colour = 0U;
		ir->push_bytes = NVRTX_DRAW_PUSH_BYTES;
	}

	/* The four channels to colour 0. */
	for (component = 0U; component < 4U; component++)
		draw_ir_store(list, &count, 0U, component, colour + component);
	program->output.location = 0U;
	program->output.components = 4U;

	/* The program. */
	ir->stage = DRV_GPU_STAGE_FRAGMENT;
	ir->outputs = &program->output;
	ir->output_count = 1U;
	ir->instructions = list;
	ir->instruction_count = count;
	ir->value_count = colour + 4U;
}

/* Writes an incrementing method header on the 3D subchannel and reports where its data goes. */
static size_t
draw_method(
	uint8_t *push,
	size_t at,
	uint32_t method,
	uint32_t count)
{
	/* The header: incrementing, the count, the subchannel, the method's word address. */
	nvrtx_put32(push, at, DRAW_METHOD_INCREMENT | (count << 16) | (NVRTX_DRAW_SUBCHANNEL << 13) | (method >> 2));

	/* Succeeded: the data follows the header. */
	return at + 4U;
}

/* Writes one method with one value and reports where the push buffer goes on. */
static size_t
draw_set(
	uint8_t *push,
	size_t at,
	uint32_t method,
	uint32_t value)
{
	/* The header, then the value. */
	at = draw_method(push, at, method, 1U);
	nvrtx_put32(push, at, value);

	/* Succeeded: past the value. */
	return at + 4U;
}

/*
 * Writes colour target index as a null target: no address, disabled,
 * compressed.
 */
static size_t
draw_null_target(
	uint8_t *push,
	size_t at,
	uint32_t index)
{
	/* The target's nine words. */
	at = draw_method(push, at, M_SET_COLOR_TARGET_A + index * M_SET_COLOR_TARGET_STRIDE, 9U);
	nvrtx_put32(push, at, 0U);
	nvrtx_put32(push, at + 4U, 0U);
	nvrtx_put32(push, at + 8U, V_TARGET_NULL_WIDTH);
	nvrtx_put32(push, at + 12U, 0U);
	nvrtx_put32(push, at + 16U, 0U);
	nvrtx_put32(push, at + 20U, 0U);
	nvrtx_put32(push, at + 24U, 1U);
	nvrtx_put32(push, at + 28U, 0U);
	nvrtx_put32(push, at + 32U, 0U);
	at += 36U;

	/* Its compression left on. */
	at = draw_set(push, at, M_SET_COLOR_COMPRESSION + index * 4U, V_TRUE);

	/* Succeeded: past the target. */
	return at;
}

/* Writes one enabled program of the pipeline: its type, its header's address, its registers and its group. */
static size_t
draw_program(
	uint8_t *push,
	size_t at,
	uint32_t type,
	uint64_t address,
	uint32_t gprs,
	uint32_t group)
{
	uint32_t base;

	base = type * M_SET_PIPELINE_STRIDE;

	/* Enabled, of its type. */
	at = draw_set(push, at, M_SET_PIPELINE_SHADER + base, V_TRUE | (type << 4));

	/* The address of its header. */
	at = draw_method(push, at, M_SET_PIPELINE_PROGRAM_ADDRESS_A + base, 2U);
	nvrtx_put32(push, at, (uint32_t)(address >> 32));
	nvrtx_put32(push, at + 4U, (uint32_t)(address & 0xffffffffU));
	at += 8U;

	/* Its registers and the group whose constant buffers it reads. */
	at = draw_method(push, at, M_SET_PIPELINE_REGISTER_COUNT + base, 2U);
	nvrtx_put32(push, at, gprs);
	nvrtx_put32(push, at + 4U, group);
	at += 8U;

	/* Succeeded: past the program. */
	return at;
}

/* Reports the bits of a float that holds a whole number below 2^24. */
static uint32_t
draw_float(
	uint32_t value)
{
	uint32_t exponent;

	/* Zero has no leading bit. */
	if (value == 0U)
		return 0U;

	/* The exponent is the place of the leading bit. */
	exponent = 31U - (uint32_t)__builtin_clz(value);

	/* Succeeded: the biased exponent and the bits below the leading one. */
	return ((exponent + 127U) << 23) | ((value << (23U - exponent)) & 0x7fffffU);
}

/* Sets a field of up to 32 bits of a descriptor at a bit position. */
static void
draw_field(
	uint32_t *words,
	unsigned low,
	unsigned bits,
	uint32_t value)
{
	unsigned bit;
	uint32_t mask;

	/* Each bit of the field into its word. */
	for (bit = 0U; bit < bits; bit++) {
		mask = 1U << ((low + bit) % 32U);

		/* Sets or clears the bit the value has there. */
		if (((value >> bit) & 1U) != 0U) {
			words[(low + bit) / 32U] |= mask;
		} else {
			words[(low + bit) / 32U] &= ~mask;
		}
	}
}

/* Appends base = to - from, base + 1 = base * corner, base + 2 = from + base + 1: from mixed toward to by corner. */
static void
draw_ir_mix(
	struct drv_gpu_shader_ir_inst *list,
	uint32_t *count,
	uint32_t base,
	uint32_t corner,
	uint32_t from,
	uint32_t to)
{
	struct drv_gpu_shader_ir_inst *instruction;

	/* The span. */
	instruction = &list[(*count)++];
	instruction->op = DRV_GPU_IR_FSUB;
	instruction->dst = base;
	instruction->src[0] = to;
	instruction->src[1] = from;

	/* The part of it the corner takes. */
	instruction = &list[(*count)++];
	instruction->op = DRV_GPU_IR_FMUL;
	instruction->dst = base + 1U;
	instruction->src[0] = base;
	instruction->src[1] = corner;

	/* From the start. */
	instruction = &list[(*count)++];
	instruction->op = DRV_GPU_IR_FADD;
	instruction->dst = base + 2U;
	instruction->src[0] = from;
	instruction->src[1] = base + 1U;
}

/* Appends an instruction that defines dst without sources: an input, a push constant word or a constant. */
static void
draw_ir_load(
	struct drv_gpu_shader_ir_inst *list,
	uint32_t *count,
	uint32_t dst,
	enum drv_gpu_shader_ir_op op,
	uint32_t location,
	uint32_t component,
	uint32_t immediate)
{
	struct drv_gpu_shader_ir_inst *instruction;

	/* The next instruction. */
	instruction = &list[(*count)++];
	instruction->op = op;
	instruction->dst = dst;
	instruction->location = location;
	instruction->component = component;
	instruction->immediate = immediate;
}

/* Appends the store of a value to an output's component. */
static void
draw_ir_store(
	struct drv_gpu_shader_ir_inst *list,
	uint32_t *count,
	uint32_t location,
	uint32_t component,
	uint32_t value)
{
	struct drv_gpu_shader_ir_inst *instruction;

	/* The next instruction. */
	instruction = &list[(*count)++];
	instruction->op = DRV_GPU_IR_STORE_OUTPUT;
	instruction->location = location;
	instruction->component = component;
	instruction->src[0] = value;
}
