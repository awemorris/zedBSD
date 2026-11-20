/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of the Vulkan executor's push buffer that touch no hardware
 * (see nvrtx-exec.h).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <libc/vulkan/vulkan_core.h>

#include "drivers/gpu/nvrtx/vk/nvrtx-exec.h"
#include "drivers/gpu/nvrtx/nvrtx-gsp.h"

/*
 * The 3D class's methods (TURING_A) and the strides of their arrays.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc597.h (the NVC597_* methods).
 */
#define M_WAIT_FOR_IDLE				0x0110U
#define M_SET_INSTANCE_COUNT			0x0220U
#define M_SET_VERTEX_STREAM_SIZE_A		0x0600U
#define M_SET_VERTEX_STREAM_SIZE_STRIDE		0x8U
#define M_SET_COLOR_TARGET_A			0x0800U
#define M_SET_COLOR_TARGET_STRIDE		0x40U
#define M_SET_VIEWPORT_SCALE_X			0x0a00U
#define M_SET_VIEWPORT_CLIP_HORIZONTAL		0x0c00U
#define M_SET_CLEAR_RECT_HORIZONTAL		0x0d6cU
#define M_SET_VERTEX_ARRAY_START		0x0d74U
#define M_SET_COLOR_CLEAR_VALUE			0x0d80U
#define M_INVALIDATE_SHADER_CACHES_NO_WFI	0x0da4U
#define M_SET_SCISSOR_ENABLE			0x0e00U
#define M_SET_SCISSOR_HORIZONTAL		0x0e04U
#define M_SET_SCISSOR_VERTICAL			0x0e08U
#define M_SET_SURFACE_CLIP_HORIZONTAL		0x0ff4U
#define M_SET_CLEAR_SURFACE_CONTROL		0x10f8U
#define M_SET_VERTEX_ID_BASE			0x1118U
#define M_SET_VERTEX_ATTRIBUTE_A		0x1160U
#define M_SET_CT_SELECT				0x121cU
#define M_INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI	0x1288U
#define M_SET_DEPTH_TEST			0x12ccU
#define M_SET_DEPTH_WRITE			0x12e8U
#define M_SET_BLEND				0x1360U
#define M_SET_BLEND_STRIDE			0x4U
#define M_SET_STENCIL_TEST			0x1380U
#define M_SET_GLOBAL_BASE_VERTEX_INDEX		0x1434U
#define M_SET_GLOBAL_BASE_INSTANCE_INDEX	0x1438U
#define M_SET_ZT_SELECT				0x1538U
#define M_SET_ANTI_ALIAS			0x15d0U
#define M_END					0x1614U
#define M_BEGIN					0x1618U
#define M_SET_VERTEX_STREAM_INSTANCE_A		0x1880U
#define M_OGL_SET_CULL				0x1918U
#define M_OGL_SET_FRONT_FACE			0x191cU
#define M_SET_VIEWPORT_CLIP_CONTROL		0x193cU
#define M_SET_RENDER_ENABLE_OVERRIDE		0x1944U
#define M_SET_LOGIC_OP				0x19c4U
#define M_CLEAR_SURFACE				0x19d0U
#define M_SET_COLOR_COMPRESSION			0x19e0U
#define M_SET_CT_WRITE				0x1a00U
#define M_SET_CT_WRITE_STRIDE			0x4U
#define M_SET_VERTEX_STREAM_A_FORMAT		0x1c00U
#define M_SET_VERTEX_STREAM_A_STRIDE		0x10U
#define M_SET_BLEND_PER_TARGET_SEPARATE		0x1e00U
#define M_SET_BLEND_PER_TARGET_STRIDE		0x20U
#define M_SET_RASTER_ENABLE			0x037cU
#define M_SET_FRONT_POLYGON_MODE		0x0dacU
#define M_SET_VIEWPORT_COORDINATE_SWIZZLE	0x0a18U
#define M_SET_VIEWPORT_Z_CLIP			0x0d7cU
#define M_SET_PROVOKING_VERTEX			0x1684U
#define M_SET_ANTI_ALIAS_ALPHA_CONTROL		0x153cU

/*
 * The layer a primitive goes to: SET_RT_LAYER's V (control 0, the layer
 * the method gives) or the last program before the rasterizer's
 * (GEOMETRY_SHADER_SELECTS_LAYER, bit 16).
 *
 * Values referenced: Mesa 25.0.7 src/nouveau/headers/nvidia/classes/clc597.h
 * (NVC597_SET_RT_LAYER, _CONTROL 16:16).
 */
#define M_SET_RT_LAYER				0x15ccU

/*
 * Whether the depth and stencil tests run before the fragment program
 * (the API's EarlyFragmentTests): SET_API_MANDATED_EARLY_Z, bit 0.  Without
 * it a program that writes memory runs for every fragment and the tests
 * follow it.
 *
 * Values referenced: Mesa 25.0.7 src/nouveau/headers/nvidia/classes/cl9097.h
 * (NV9097_SET_API_MANDATED_EARLY_Z 0x0210, ENABLE 0:0).
 */
#define M_SET_API_MANDATED_EARLY_Z		0x0210U

/*
 * The samples a pixel keeps, the same for each pixel of a 2x2 quad
 * (SET_SAMPLE_MASK_X0_Y0, _X1_Y0, _X0_Y1, _X1_Y1, 16 bits each).
 *
 * Values referenced: Mesa 25.0.7 src/nouveau/headers/nvidia/classes/clc597.h
 * (NVC597_SET_SAMPLE_MASK_X0_Y0 0x0fbc to _X1_Y1 0x0fc8).
 */
#define M_SET_SAMPLE_MASK_X0_Y0			0x0fbcU
#define EXEC_SAMPLE_MASK_PIXELS			4U
#define V_RT_LAYER_FROM_PROGRAM			(1U << 16)
#define M_SET_SAMPLE_MASK_X0_Y0			0x0fbcU
#define M_SET_SINGLE_CT_WRITE_CONTROL		0x0f90U

/*
 * The depth target, the depth and stencil tests, the index buffer and the
 * culling.
 *
 * Values referenced: Mesa 25.0.7
 * src/nouveau/headers/nvidia/classes/cl9097.h (NV9097_SET_ZT_*,
 * NV9097_SET_DEPTH_FUNC, NV9097_SET_Z_CLEAR_VALUE,
 * NV9097_SET_STENCIL_CLEAR_VALUE, NV9097_SET_STENCIL_OP_FAIL to
 * NV9097_SET_STENCIL_MASK, NV9097_SET_BACK_STENCIL_FUNC_REF to
 * NV9097_SET_BACK_STENCIL_FUNC_MASK, NV9097_SET_TWO_SIDED_STENCIL_TEST,
 * NV9097_SET_BACK_STENCIL_OP_FAIL to NV9097_SET_BACK_STENCIL_FUNC,
 * NV9097_SET_INDEX_BUFFER_*,
 * NV9097_DRAW_INDEX_BUFFER, NV9097_SET_DA_PRIMITIVE_RESTART,
 * NV9097_OGL_SET_CULL_FACE, NV9097_SET_Z_COMPRESSION) and clc597.h
 * (NVC597_SET_INDEX_BUFFER_SIZE_A, NVC597_SET_ZT_SPARSE).
 */
#define M_SET_INDEX_BUFFER_SIZE_A		0x0238U
#define M_SET_Z_CLEAR_VALUE			0x0d90U
#define M_SET_STENCIL_CLEAR_VALUE		0x0da0U
#define M_SET_BACK_STENCIL_FUNC_REF		0x0f54U
#define M_SET_ZT_A				0x0fe0U
#define M_SET_ZT_SPARSE				0x1208U
#define M_SET_ZT_SIZE_A				0x1228U
#define M_SET_DEPTH_FUNC			0x130cU
#define M_SET_STENCIL_OP_FAIL			0x1384U
#define M_SET_STENCIL_FUNC_REF			0x1394U
#define M_SET_TWO_SIDED_STENCIL_TEST		0x1594U
#define M_SET_BACK_STENCIL_OP_FAIL		0x1598U
#define M_SET_DA_PRIMITIVE_RESTART		0x1644U
#define M_SET_ZT_LAYER				0x179cU
#define M_SET_INDEX_BUFFER_A			0x17c8U
#define M_SET_INDEX_BUFFER_E			0x17d8U
#define M_SET_INDEX_BUFFER_F			0x17dcU
#define M_OGL_SET_CULL_FACE			0x1920U
#define M_SET_Z_COMPRESSION			0x19ccU
#define M_SET_CONSTANT_BUFFER_SELECTOR_A	0x2380U
#define M_BIND_GROUP_CONSTANT_BUFFER		0x2410U

/*
 * The offset of the loads into the selected constant buffer, and the
 * header whose first word goes to its method and the rest to the one after
 * it (the loads, each going on four bytes further).
 *
 * Values referenced: Mesa 25.0.7
 * src/nouveau/headers/nvidia/classes/clc597.h
 * (NVC597_LOAD_CONSTANT_BUFFER_OFFSET, NVC597_LOAD_CONSTANT_BUFFER) and
 * src/nouveau/headers/nv_push.h (NVC0_FIFO_PKHDR_1I).
 */
#define M_LOAD_CONSTANT_BUFFER_OFFSET		0x238cU
#define EXEC_METHOD_ONE_INCREMENT		(5U << 29)
#define M_BIND_GROUP_STRIDE			0x20U

/*
 * The colour targets, the ones after target 0 left null.
 *
 * Value referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_push_draw_state_init, nvk_CmdBeginRendering).
 */
#define EXEC_COLOR_TARGETS			8U

/*
 * An incrementing method header on the 3D subchannel (0).
 *
 * Value referenced: Mesa 25.3.6 src/nouveau/headers/nv_push.h
 * (NVC0_FIFO_PKHDR_SQ).
 */
#define EXEC_METHOD_INCREMENT			(1U << 29)

/*
 * The values written (the fields' meanings beside them).
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc597.h (the fields of the NVC597_*
 * methods) and src/nouveau/vulkan/nvk_cmd_draw.c (nvk_CmdBeginRendering,
 * nvk_flush_vp_state, nvk_flush_rs_state, nvk_flush_ms_state).
 */
#define V_TRUE					1U
#define V_FALSE					0U
#define V_FLOAT_ONE				0x3f800000U
#define V_TARGET_LAYOUT_PITCH			(1U << 12)
#define V_TARGET_LAYOUT_BLOCK_ONE_GOB		0U
#define V_TARGET_NULL_WIDTH			64U
#define V_CT_SELECT_ONE_TARGET			1U
#define V_CT_SELECT_TARGET_SHIFT		4U
#define V_CT_SELECT_TARGET_BITS			3U
#define V_CLEAR_TARGET_SHIFT			6U
#define V_CLEAR_LAYER_SHIFT			10U
#define V_ANTI_ALIAS_1X1			0U

/*
 * Four samples a pixel: the samples mode of two by two samples (the pixel's
 * samples a 2x2 block of the target's samples), the sample positions'
 * method (four words of four positions, each 4.4 bits of x and y), each
 * word's positions for one sample (the pixel's centre) and for four (the
 * standard positions (0.375, 0.125), (0.875, 0.375), (0.125, 0.625) and
 * (0.625, 0.875) in sixteenths, every pixel of the 2x2 grid alike), and
 * the alpha to coverage bit.
 *
 * Values referenced: Mesa 25.0.7 src/nouveau/headers/nvidia/classes/clc597.h
 * (NVC597_SET_ANTI_ALIAS_SAMPLES_MODE_2X2, NVC597_SET_ANTI_ALIAS_SAMPLE_POSITIONS
 * and its X0..Y3 fields, NVC597_SET_ANTI_ALIAS_ALPHA_CONTROL_ALPHA_TO_COVERAGE),
 * src/nouveau/vulkan/nvk_cmd_draw.c (nvk_flush_ms_state, vk_to_nak_sample_location)
 * and src/vulkan/runtime/vk_standard_sample_locations.c.
 */
#define V_ANTI_ALIAS_2X2			2U
#define M_SET_ANTI_ALIAS_SAMPLE_POSITIONS	0x11e0U
#define V_POSITIONS_ONE				0x88888888U
#define V_POSITIONS_FOUR			0xeaa26e26U
#define V_ALPHA_TO_COVERAGE			1U
#define V_VIEWPORT_SWIZZLE_IDENTITY		0x6420U
#define V_Z_CLIP_ZERO_TO_W			1U
#define V_VIEWPORT_CLIP_DRAW			0x181dU
#define V_POLYGON_FILL				0x1b02U
#define V_FRONT_FACE_CCW			0x901U
#define V_SAMPLE_MASK_ALL			0xffffU
#define V_CLEAR_USE_RECT			(1U << 4)
#define V_CLEAR_RGBA				0x3cU
#define V_RENDER_ALWAYS				1U
#define V_RENDER_USE_ENABLE			0U
#define V_STREAM_ENABLE				(1U << 12)
#define V_INVALIDATE_CONSTANTS			(1U << 12)

/*
 * The depth target's formats (Z16 for D16_UNORM, ZF32 for D32_SFLOAT,
 * S8Z24 for D24_UNORM_S8_UINT: the depth in the low 24 bits, the stencil
 * in the high 8, ZF32_X24S8 for D32_SFLOAT_S8_UINT: the depth's float,
 * then the stencil in the low 8 bits of the next word) and its blocks
 * (one GOB each way), the array size taken from
 * the third dimension, the depth and stencil functions (the OGL values in
 * the order of VkCompareOp), the stencil operations (the D3D values, one
 * more than VkStencilOp), the faces culled, the front faces, the index
 * sizes and clears of depth only and of stencil only.
 *
 * Values referenced: Mesa 25.0.7
 * src/nouveau/headers/nvidia/classes/cl9097.h (NV9097_SET_ZT_FORMAT_V_ZF32,
 * NV9097_SET_ZT_FORMAT_V_S8Z24, NV9097_SET_ZT_FORMAT_V_Z16,
 * NV9097_SET_ZT_FORMAT_V_ZF32_X24S8, NV9097_SET_ZT_BLOCK_SIZE,
 * NV9097_SET_ZT_SIZE_C_CONTROL, NV9097_SET_DEPTH_FUNC_V_OGL_*,
 * NV9097_SET_STENCIL_OP_FAIL_V_D3D_*, NV9097_OGL_SET_CULL_FACE_V_*,
 * NV9097_OGL_SET_FRONT_FACE_V_*, NV9097_SET_INDEX_BUFFER_E_INDEX_SIZE_*,
 * NV9097_CLEAR_SURFACE_Z_ENABLE, NV9097_CLEAR_SURFACE_STENCIL_ENABLE),
 * Mesa 26.2.6 src/nouveau/nil/nil_formats.csv (Z24_UNORM_S8_UINT as S8Z24)
 * and src/nouveau/vulkan/nvk_cmd_draw.c (vk_to_nv9097_front_face: Vulkan's
 * counterclockwise is the class's; vk_to_nv9097_stencil_op).
 */
#define V_ZT_FORMAT_ZF32			0x0aU
#define V_ZT_FORMAT_Z16				0x13U
#define V_ZT_FORMAT_S8Z24			0x16U
#define V_ZT_FORMAT_ZF32_X24S8			0x19U
#define V_ZT_BLOCK_ONE_GOB			0U
#define V_ZT_SIZE_ARRAY				0U
#define V_DEPTH_FUNC_NEVER			0x200U
#define V_CULL_FACE_FRONT			0x404U
#define V_CULL_FACE_BACK			0x405U
#define V_CULL_FACE_FRONT_AND_BACK		0x408U
#define V_FRONT_FACE_CW				0x900U

/*
 * The polygon modes of SET_FRONT_POLYGON_MODE and SET_BACK_POLYGON_MODE
 * (POINT 0x1b00, LINE 0x1b01, FILL 0x1b02: Vulkan's FILL, LINE, POINT
 * counted down from V_POLYGON_FILL); SET_LOGIC_OP_FUNC's
 * operations (CLEAR to SET in Vulkan's order of VkLogicOp from 0x1500);
 * SET_VIEWPORT_CLIP_CONTROL with the depth clamped to the viewport's
 * range instead of clipped: MIN_Z_ZERO_MAX_Z_ONE off, Z_CLIP_RANGE
 * MIN_Z_MAX_Z, the pixels' depth clamped, the geometry clipped in x and y
 * only, its z guardband at scale 256 (V_VIEWPORT_CLIP_DRAW is the same
 * with the depth clipped: MIN_Z_ZERO_MAX_Z_ONE, z guardband at scale 1,
 * FRUSTUM_XYZ_CLIP).
 *
 * Values referenced: Mesa 25.0.7
 * src/nouveau/headers/nvidia/classes/clc597.h (NVC597_SET_FRONT_POLYGON_MODE_V_*,
 * NVC597_SET_LOGIC_OP_FUNC, NVC597_SET_LOGIC_OP_FUNC_V_*,
 * NVC597_SET_VIEWPORT_CLIP_CONTROL and its fields) and
 * src/nouveau/vulkan/nvk_cmd_draw.c (nvk_flush_rs_state: the clip control
 * of a clamped depth, without depth clipping).
 */
#define M_SET_LOGIC_OP_FUNC			0x19c8U
#define V_LOGIC_OP_CLEAR			0x1500U
#define V_VIEWPORT_CLIP_CLAMP			0x1101aU
#define V_INDEX_SIZE_TWO_BYTES			1U
#define V_INDEX_SIZE_FOUR_BYTES			2U
#define V_CLEAR_Z				1U
#define V_CLEAR_STENCIL				2U
#define V_GROUP_VALID				1U
#define V_GROUP_SLOT_SHIFT			4U

/*
 * A vertex attribute: inactive, its component widths, its numerical type
 * (signed or unsigned normalized, signed or unsigned integer, signed or
 * unsigned scaled, float) and the swap of red and blue.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc597.h
 * (NVC597_SET_VERTEX_ATTRIBUTE_A: SOURCE, OFFSET, COMPONENT_BIT_WIDTHS,
 * NUMERICAL_TYPE and SWAP_R_AND_B).
 */
#define V_ATTRIBUTE_INACTIVE			(1U << 6)
#define V_ATTRIBUTE_OFFSET_SHIFT		7U
#define V_ATTRIBUTE_WIDTHS_SHIFT		21U
#define V_ATTRIBUTE_TYPE_SHIFT			27U
#define V_ATTRIBUTE_SNORM			1U
#define V_ATTRIBUTE_UNORM			2U
#define V_ATTRIBUTE_SINT			3U
#define V_ATTRIBUTE_UINT			4U
#define V_ATTRIBUTE_USCALED			5U
#define V_ATTRIBUTE_SSCALED			6U
#define V_ATTRIBUTE_FLOAT			7U
#define V_ATTRIBUTE_SWAP_RB			(1U << 31)
#define V_WIDTHS_R32_G32_B32_A32		0x01U
#define V_WIDTHS_R32_G32_B32			0x02U
#define V_WIDTHS_R16_G16_B16_A16		0x03U
#define V_WIDTHS_R32_G32			0x04U
#define V_WIDTHS_R16_G16_B16			0x05U
#define V_WIDTHS_R8_G8_B8_A8			0x0aU
#define V_WIDTHS_R16_G16			0x0fU
#define V_WIDTHS_R32				0x12U
#define V_WIDTHS_R8_G8_B8			0x13U
#define V_WIDTHS_R8_G8				0x18U
#define V_WIDTHS_R16				0x1bU
#define V_WIDTHS_R8				0x1dU
#define V_WIDTHS_A2B10G10R10			0x30U
#define V_WIDTHS_B10G11R11			0x31U
#define V_WIDTHS_INACTIVE			0x01U

/*
 * The vertex formats fetched: each one's component widths, numerical
 * type and whether red and blue are swapped (the BGR orders).  The 8 and
 * 16-bit normalized, scaled and integer formats of one to four
 * components, 16 and 32-bit floats, 32-bit integers, and the packed
 * 10-10-10-2 and 11-11-10 ones; an integer input reads an integer format
 * as it is.
 *
 * Values referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_format.c
 * (nvk_vf_formats: the widths, the swap and the type of each format; its
 * 64-bit ones are left out).
 */
struct exec_vertex_format {
	uint32_t format;
	uint32_t widths;
	uint32_t type;
	uint32_t swap;
};

static const struct exec_vertex_format exec_vertex_formats[] = {
	{ VK_FORMAT_R8_UNORM, V_WIDTHS_R8, V_ATTRIBUTE_UNORM, 0U },
	{ VK_FORMAT_R8_SNORM, V_WIDTHS_R8, V_ATTRIBUTE_SNORM, 0U },
	{ VK_FORMAT_R8_USCALED, V_WIDTHS_R8, V_ATTRIBUTE_USCALED, 0U },
	{ VK_FORMAT_R8_SSCALED, V_WIDTHS_R8, V_ATTRIBUTE_SSCALED, 0U },
	{ VK_FORMAT_R8_UINT, V_WIDTHS_R8, V_ATTRIBUTE_UINT, 0U },
	{ VK_FORMAT_R8_SINT, V_WIDTHS_R8, V_ATTRIBUTE_SINT, 0U },
	{ VK_FORMAT_R8G8_UNORM, V_WIDTHS_R8_G8, V_ATTRIBUTE_UNORM, 0U },
	{ VK_FORMAT_R8G8_SNORM, V_WIDTHS_R8_G8, V_ATTRIBUTE_SNORM, 0U },
	{ VK_FORMAT_R8G8_USCALED, V_WIDTHS_R8_G8, V_ATTRIBUTE_USCALED, 0U },
	{ VK_FORMAT_R8G8_SSCALED, V_WIDTHS_R8_G8, V_ATTRIBUTE_SSCALED, 0U },
	{ VK_FORMAT_R8G8_UINT, V_WIDTHS_R8_G8, V_ATTRIBUTE_UINT, 0U },
	{ VK_FORMAT_R8G8_SINT, V_WIDTHS_R8_G8, V_ATTRIBUTE_SINT, 0U },
	{ VK_FORMAT_R8G8B8_UNORM, V_WIDTHS_R8_G8_B8, V_ATTRIBUTE_UNORM, 0U },
	{ VK_FORMAT_R8G8B8_SNORM, V_WIDTHS_R8_G8_B8, V_ATTRIBUTE_SNORM, 0U },
	{ VK_FORMAT_R8G8B8_USCALED, V_WIDTHS_R8_G8_B8, V_ATTRIBUTE_USCALED, 0U },
	{ VK_FORMAT_R8G8B8_SSCALED, V_WIDTHS_R8_G8_B8, V_ATTRIBUTE_SSCALED, 0U },
	{ VK_FORMAT_R8G8B8_UINT, V_WIDTHS_R8_G8_B8, V_ATTRIBUTE_UINT, 0U },
	{ VK_FORMAT_R8G8B8_SINT, V_WIDTHS_R8_G8_B8, V_ATTRIBUTE_SINT, 0U },
	{ VK_FORMAT_B8G8R8_UNORM, V_WIDTHS_R8_G8_B8, V_ATTRIBUTE_UNORM, V_ATTRIBUTE_SWAP_RB },
	{ VK_FORMAT_B8G8R8_SNORM, V_WIDTHS_R8_G8_B8, V_ATTRIBUTE_SNORM, V_ATTRIBUTE_SWAP_RB },
	{ VK_FORMAT_B8G8R8_USCALED, V_WIDTHS_R8_G8_B8, V_ATTRIBUTE_USCALED, V_ATTRIBUTE_SWAP_RB },
	{ VK_FORMAT_B8G8R8_SSCALED, V_WIDTHS_R8_G8_B8, V_ATTRIBUTE_SSCALED, V_ATTRIBUTE_SWAP_RB },
	{ VK_FORMAT_B8G8R8_UINT, V_WIDTHS_R8_G8_B8, V_ATTRIBUTE_UINT, V_ATTRIBUTE_SWAP_RB },
	{ VK_FORMAT_B8G8R8_SINT, V_WIDTHS_R8_G8_B8, V_ATTRIBUTE_SINT, V_ATTRIBUTE_SWAP_RB },
	{ VK_FORMAT_R8G8B8A8_UNORM, V_WIDTHS_R8_G8_B8_A8, V_ATTRIBUTE_UNORM, 0U },
	{ VK_FORMAT_R8G8B8A8_SNORM, V_WIDTHS_R8_G8_B8_A8, V_ATTRIBUTE_SNORM, 0U },
	{ VK_FORMAT_R8G8B8A8_USCALED, V_WIDTHS_R8_G8_B8_A8, V_ATTRIBUTE_USCALED, 0U },
	{ VK_FORMAT_R8G8B8A8_SSCALED, V_WIDTHS_R8_G8_B8_A8, V_ATTRIBUTE_SSCALED, 0U },
	{ VK_FORMAT_R8G8B8A8_UINT, V_WIDTHS_R8_G8_B8_A8, V_ATTRIBUTE_UINT, 0U },
	{ VK_FORMAT_R8G8B8A8_SINT, V_WIDTHS_R8_G8_B8_A8, V_ATTRIBUTE_SINT, 0U },
	{ VK_FORMAT_B8G8R8A8_UNORM, V_WIDTHS_R8_G8_B8_A8, V_ATTRIBUTE_UNORM, V_ATTRIBUTE_SWAP_RB },
	{ VK_FORMAT_B8G8R8A8_SNORM, V_WIDTHS_R8_G8_B8_A8, V_ATTRIBUTE_SNORM, V_ATTRIBUTE_SWAP_RB },
	{ VK_FORMAT_B8G8R8A8_USCALED, V_WIDTHS_R8_G8_B8_A8, V_ATTRIBUTE_USCALED, V_ATTRIBUTE_SWAP_RB },
	{ VK_FORMAT_B8G8R8A8_SSCALED, V_WIDTHS_R8_G8_B8_A8, V_ATTRIBUTE_SSCALED, V_ATTRIBUTE_SWAP_RB },
	{ VK_FORMAT_B8G8R8A8_UINT, V_WIDTHS_R8_G8_B8_A8, V_ATTRIBUTE_UINT, V_ATTRIBUTE_SWAP_RB },
	{ VK_FORMAT_B8G8R8A8_SINT, V_WIDTHS_R8_G8_B8_A8, V_ATTRIBUTE_SINT, V_ATTRIBUTE_SWAP_RB },
	{ VK_FORMAT_A8B8G8R8_UNORM_PACK32, V_WIDTHS_R8_G8_B8_A8, V_ATTRIBUTE_UNORM, 0U },
	{ VK_FORMAT_A8B8G8R8_SNORM_PACK32, V_WIDTHS_R8_G8_B8_A8, V_ATTRIBUTE_SNORM, 0U },
	{ VK_FORMAT_A8B8G8R8_USCALED_PACK32, V_WIDTHS_R8_G8_B8_A8, V_ATTRIBUTE_USCALED, 0U },
	{ VK_FORMAT_A8B8G8R8_SSCALED_PACK32, V_WIDTHS_R8_G8_B8_A8, V_ATTRIBUTE_SSCALED, 0U },
	{ VK_FORMAT_A8B8G8R8_UINT_PACK32, V_WIDTHS_R8_G8_B8_A8, V_ATTRIBUTE_UINT, 0U },
	{ VK_FORMAT_A8B8G8R8_SINT_PACK32, V_WIDTHS_R8_G8_B8_A8, V_ATTRIBUTE_SINT, 0U },
	{ VK_FORMAT_A2R10G10B10_UNORM_PACK32, V_WIDTHS_A2B10G10R10, V_ATTRIBUTE_UNORM, V_ATTRIBUTE_SWAP_RB },
	{ VK_FORMAT_A2R10G10B10_SNORM_PACK32, V_WIDTHS_A2B10G10R10, V_ATTRIBUTE_SNORM, V_ATTRIBUTE_SWAP_RB },
	{ VK_FORMAT_A2R10G10B10_USCALED_PACK32, V_WIDTHS_A2B10G10R10, V_ATTRIBUTE_USCALED, V_ATTRIBUTE_SWAP_RB },
	{ VK_FORMAT_A2R10G10B10_SSCALED_PACK32, V_WIDTHS_A2B10G10R10, V_ATTRIBUTE_SSCALED, V_ATTRIBUTE_SWAP_RB },
	{ VK_FORMAT_A2R10G10B10_UINT_PACK32, V_WIDTHS_A2B10G10R10, V_ATTRIBUTE_UINT, V_ATTRIBUTE_SWAP_RB },
	{ VK_FORMAT_A2R10G10B10_SINT_PACK32, V_WIDTHS_A2B10G10R10, V_ATTRIBUTE_SINT, V_ATTRIBUTE_SWAP_RB },
	{ VK_FORMAT_A2B10G10R10_UNORM_PACK32, V_WIDTHS_A2B10G10R10, V_ATTRIBUTE_UNORM, 0U },
	{ VK_FORMAT_A2B10G10R10_SNORM_PACK32, V_WIDTHS_A2B10G10R10, V_ATTRIBUTE_SNORM, 0U },
	{ VK_FORMAT_A2B10G10R10_USCALED_PACK32, V_WIDTHS_A2B10G10R10, V_ATTRIBUTE_USCALED, 0U },
	{ VK_FORMAT_A2B10G10R10_SSCALED_PACK32, V_WIDTHS_A2B10G10R10, V_ATTRIBUTE_SSCALED, 0U },
	{ VK_FORMAT_A2B10G10R10_UINT_PACK32, V_WIDTHS_A2B10G10R10, V_ATTRIBUTE_UINT, 0U },
	{ VK_FORMAT_A2B10G10R10_SINT_PACK32, V_WIDTHS_A2B10G10R10, V_ATTRIBUTE_SINT, 0U },
	{ VK_FORMAT_B10G11R11_UFLOAT_PACK32, V_WIDTHS_B10G11R11, V_ATTRIBUTE_FLOAT, 0U },
	{ VK_FORMAT_R16_UNORM, V_WIDTHS_R16, V_ATTRIBUTE_UNORM, 0U },
	{ VK_FORMAT_R16_SNORM, V_WIDTHS_R16, V_ATTRIBUTE_SNORM, 0U },
	{ VK_FORMAT_R16_USCALED, V_WIDTHS_R16, V_ATTRIBUTE_USCALED, 0U },
	{ VK_FORMAT_R16_SSCALED, V_WIDTHS_R16, V_ATTRIBUTE_SSCALED, 0U },
	{ VK_FORMAT_R16_UINT, V_WIDTHS_R16, V_ATTRIBUTE_UINT, 0U },
	{ VK_FORMAT_R16_SINT, V_WIDTHS_R16, V_ATTRIBUTE_SINT, 0U },
	{ VK_FORMAT_R16_SFLOAT, V_WIDTHS_R16, V_ATTRIBUTE_FLOAT, 0U },
	{ VK_FORMAT_R16G16_UNORM, V_WIDTHS_R16_G16, V_ATTRIBUTE_UNORM, 0U },
	{ VK_FORMAT_R16G16_SNORM, V_WIDTHS_R16_G16, V_ATTRIBUTE_SNORM, 0U },
	{ VK_FORMAT_R16G16_USCALED, V_WIDTHS_R16_G16, V_ATTRIBUTE_USCALED, 0U },
	{ VK_FORMAT_R16G16_SSCALED, V_WIDTHS_R16_G16, V_ATTRIBUTE_SSCALED, 0U },
	{ VK_FORMAT_R16G16_UINT, V_WIDTHS_R16_G16, V_ATTRIBUTE_UINT, 0U },
	{ VK_FORMAT_R16G16_SINT, V_WIDTHS_R16_G16, V_ATTRIBUTE_SINT, 0U },
	{ VK_FORMAT_R16G16_SFLOAT, V_WIDTHS_R16_G16, V_ATTRIBUTE_FLOAT, 0U },
	{ VK_FORMAT_R16G16B16_UNORM, V_WIDTHS_R16_G16_B16, V_ATTRIBUTE_UNORM, 0U },
	{ VK_FORMAT_R16G16B16_SNORM, V_WIDTHS_R16_G16_B16, V_ATTRIBUTE_SNORM, 0U },
	{ VK_FORMAT_R16G16B16_USCALED, V_WIDTHS_R16_G16_B16, V_ATTRIBUTE_USCALED, 0U },
	{ VK_FORMAT_R16G16B16_SSCALED, V_WIDTHS_R16_G16_B16, V_ATTRIBUTE_SSCALED, 0U },
	{ VK_FORMAT_R16G16B16_UINT, V_WIDTHS_R16_G16_B16, V_ATTRIBUTE_UINT, 0U },
	{ VK_FORMAT_R16G16B16_SINT, V_WIDTHS_R16_G16_B16, V_ATTRIBUTE_SINT, 0U },
	{ VK_FORMAT_R16G16B16_SFLOAT, V_WIDTHS_R16_G16_B16, V_ATTRIBUTE_FLOAT, 0U },
	{ VK_FORMAT_R16G16B16A16_UNORM, V_WIDTHS_R16_G16_B16_A16, V_ATTRIBUTE_UNORM, 0U },
	{ VK_FORMAT_R16G16B16A16_SNORM, V_WIDTHS_R16_G16_B16_A16, V_ATTRIBUTE_SNORM, 0U },
	{ VK_FORMAT_R16G16B16A16_USCALED, V_WIDTHS_R16_G16_B16_A16, V_ATTRIBUTE_USCALED, 0U },
	{ VK_FORMAT_R16G16B16A16_SSCALED, V_WIDTHS_R16_G16_B16_A16, V_ATTRIBUTE_SSCALED, 0U },
	{ VK_FORMAT_R16G16B16A16_UINT, V_WIDTHS_R16_G16_B16_A16, V_ATTRIBUTE_UINT, 0U },
	{ VK_FORMAT_R16G16B16A16_SINT, V_WIDTHS_R16_G16_B16_A16, V_ATTRIBUTE_SINT, 0U },
	{ VK_FORMAT_R16G16B16A16_SFLOAT, V_WIDTHS_R16_G16_B16_A16, V_ATTRIBUTE_FLOAT, 0U },
	{ VK_FORMAT_R32_UINT, V_WIDTHS_R32, V_ATTRIBUTE_UINT, 0U },
	{ VK_FORMAT_R32_SINT, V_WIDTHS_R32, V_ATTRIBUTE_SINT, 0U },
	{ VK_FORMAT_R32_SFLOAT, V_WIDTHS_R32, V_ATTRIBUTE_FLOAT, 0U },
	{ VK_FORMAT_R32G32_UINT, V_WIDTHS_R32_G32, V_ATTRIBUTE_UINT, 0U },
	{ VK_FORMAT_R32G32_SINT, V_WIDTHS_R32_G32, V_ATTRIBUTE_SINT, 0U },
	{ VK_FORMAT_R32G32_SFLOAT, V_WIDTHS_R32_G32, V_ATTRIBUTE_FLOAT, 0U },
	{ VK_FORMAT_R32G32B32_UINT, V_WIDTHS_R32_G32_B32, V_ATTRIBUTE_UINT, 0U },
	{ VK_FORMAT_R32G32B32_SINT, V_WIDTHS_R32_G32_B32, V_ATTRIBUTE_SINT, 0U },
	{ VK_FORMAT_R32G32B32_SFLOAT, V_WIDTHS_R32_G32_B32, V_ATTRIBUTE_FLOAT, 0U },
	{ VK_FORMAT_R32G32B32A32_UINT, V_WIDTHS_R32_G32_B32_A32, V_ATTRIBUTE_UINT, 0U },
	{ VK_FORMAT_R32G32B32A32_SINT, V_WIDTHS_R32_G32_B32_A32, V_ATTRIBUTE_SINT, 0U },
	{ VK_FORMAT_R32G32B32A32_SFLOAT, V_WIDTHS_R32_G32_B32_A32, V_ATTRIBUTE_FLOAT, 0U }
};

/*
 * The draw: the topology, the instances iterated from the first.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc597.h (NVC597_BEGIN, its OP
 * values) and Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_draw.c
 * (vk_to_nv9097_primitive_topology).
 */
#define V_BEGIN_POINTS				0U
#define V_BEGIN_LINES				1U
#define V_BEGIN_LINE_STRIP			3U
#define V_BEGIN_TRIANGLES			4U
#define V_BEGIN_TRIANGLE_STRIP			5U
#define V_BEGIN_TRIANGLE_FAN			6U
#define V_BEGIN_LINES_ADJACENCY			0xaU
#define V_BEGIN_LINE_STRIP_ADJACENCY		0xbU
#define V_BEGIN_TRIANGLES_ADJACENCY		0xcU
#define V_BEGIN_TRIANGLE_STRIP_ADJACENCY	0xdU
#define V_BEGIN_ITERATE				(1U << 31)

/*
 * Points and lines: whether a point's size comes from the last program
 * before the rasterizer (else the fixed size one, the queue's start
 * state with square points from the top left, nvrtx_push_draw_init_class),
 * and the width of the (aliased) lines, then the aliased one.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc597.h
 * (NVC597_SET_ATTRIBUTE_POINT_SIZE, NVC597_SET_LINE_WIDTH_FLOAT,
 * NVC597_SET_ALIASED_LINE_WIDTH_FLOAT) and Mesa 25.0.7
 * src/nouveau/vulkan/nvk_cmd_draw.c (nvk_flush_rs_state: both widths).
 */
#define M_SET_LINE_WIDTH_FLOAT			0x13b0U
#define M_SET_ATTRIBUTE_POINT_SIZE		0x1910U

/*
 * The blend's operations and factors (the OGL values).
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc597.h
 * (NVC597_SET_BLEND_COLOR_OP, NVC597_SET_BLEND_COLOR_SOURCE_COEFF) and
 * src/nouveau/vulkan/nvk_cmd_draw.c (nvk_flush_cb_state).
 */
#define V_BLEND_ADD				0x8006U
#define V_BLEND_SUBTRACT			0x800aU
#define V_BLEND_REVERSE_SUBTRACT		0x800bU
#define V_BLEND_MIN				0x8007U
#define V_BLEND_MAX				0x8008U
#define V_FACTOR_ZERO				0x4000U
#define V_FACTOR_ONE				0x4001U
#define V_FACTOR_SRC_COLOR			0x4300U
#define V_FACTOR_ONE_MINUS_SRC_COLOR		0x4301U
#define V_FACTOR_SRC_ALPHA			0x4302U
#define V_FACTOR_ONE_MINUS_SRC_ALPHA		0x4303U
#define V_FACTOR_DST_ALPHA			0x4304U
#define V_FACTOR_ONE_MINUS_DST_ALPHA		0x4305U
#define V_FACTOR_DST_COLOR			0x4306U
#define V_FACTOR_ONE_MINUS_DST_COLOR		0x4307U
#define V_FACTOR_SRC_ALPHA_SATURATE		0x4308U
#define V_FACTOR_CONSTANT_COLOR			0xc001U
#define V_FACTOR_ONE_MINUS_CONSTANT_COLOR	0xc002U
#define V_FACTOR_CONSTANT_ALPHA			0xc003U
#define V_FACTOR_ONE_MINUS_CONSTANT_ALPHA	0xc004U
#define V_FACTOR_SRC1_COLOR			0xc900U
#define V_FACTOR_ONE_MINUS_SRC1_COLOR		0xc901U
#define V_FACTOR_SRC1_ALPHA			0xc902U
#define V_FACTOR_ONE_MINUS_SRC1_ALPHA		0xc903U

/*
 * The blend constants and the depth bias: the polygon offset of points,
 * lines and triangles (three methods in a row), its constant factor in
 * units of the depth format's least difference, its slope factor and
 * its clamp.
 *
 * Values referenced: Mesa 25.0.7
 * src/nouveau/headers/nvidia/classes/cl9097.h
 * (NV9097_SET_BLEND_CONST_RED to NV9097_SET_BLEND_CONST_ALPHA,
 * NV9097_SET_POLY_OFFSET_POINT to NV9097_SET_POLY_OFFSET_FILL,
 * NV9097_SET_DEPTH_BIAS_CONTROL, NV9097_SET_DEPTH_BIAS,
 * NV9097_SET_SLOPE_SCALE_DEPTH_BIAS, NV9097_SET_DEPTH_BIAS_CLAMP) and
 * src/nouveau/vulkan/nvk_cmd_draw.c (nvk_flush_rs_state,
 * nvk_flush_cb_state).
 */
#define M_SET_BLEND_CONST_RED			0x131cU
#define M_SET_POLY_OFFSET_POINT			0x0dc0U
#define M_SET_DEPTH_BIAS_CONTROL		0x1110U
#define M_SET_DEPTH_BIAS			0x15bcU
#define M_SET_SLOPE_SCALE_DEPTH_BIAS		0x156cU
#define M_SET_DEPTH_BIAS_CLAMP			0x187cU
#define V_DEPTH_BIAS_FORMAT_DEPENDENT		1U

/*
 * The geometry program's slot of the pipeline (type 4, its methods 64
 * bytes a type apart): enabled and its type, its header's address, its
 * registers and the program group whose constant buffers it reads (the
 * geometry group, 3).
 *
 * Values referenced: Mesa 25.0.7
 * src/nouveau/headers/nvidia/classes/clc597.h
 * (NVC597_SET_PIPELINE_SHADER(j), _TYPE_GEOMETRY,
 * NVC597_SET_PIPELINE_REGISTER_COUNT(j), NVC597_SET_PIPELINE_BINDING(j),
 * NVC597_SET_PIPELINE_PROGRAM_ADDRESS_A(j)) and
 * src/nouveau/vulkan/nvk_shader.c (nvk_shader_fill_push: the type the
 * index, the group the stage's).
 */
#define M_SET_PIPELINE_SHADER_GEOMETRY		0x2100U
#define M_SET_PIPELINE_REGISTER_COUNT_GEOMETRY	0x210cU
#define M_SET_PIPELINE_PROGRAM_ADDRESS_A_GEOMETRY	0x2114U
#define V_PIPELINE_TYPE_GEOMETRY		4U
#define V_PIPELINE_GROUP_GEOMETRY		3U

/*
 * The texture header (pitch) and the sampler.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc597tex.h (NVC597_TEXHEAD_PITCH_*,
 * NVC597_TEXSAMP*).
 */
#define TIC_SOURCE_ZERO				0U
#define TIC_SOURCE_R				2U
#define TIC_SOURCE_G				3U
#define TIC_SOURCE_B				4U
#define TIC_SOURCE_A				5U
#define TIC_SOURCE_ONE_INT			6U
#define TIC_SOURCE_ONE_FLOAT			7U
#define TIC_VERSION_PITCH			2U
#define TIC_TYPE_TWO_D_NO_MIPMAP		7U
#define TIC_PROMOTE_TO_2_V			1U
#define TIC_BORDER_SAMPLER_COLOR		7U
#define TIC_SPREAD_FUNC_TWO			2U
#define TIC_SPREAD_FUNC_ONE			1U
#define TSC_WRAP				0U
#define TSC_MIRROR				1U
#define TSC_CLAMP_TO_EDGE			2U
#define TSC_BORDER				3U
#define TSC_FONT_SIZE_2				1U
#define TSC_MAG_POINT				1U
#define TSC_MAG_LINEAR				2U
#define TSC_MIN_POINT				1U
#define TSC_MIN_LINEAR				2U
#define TSC_MIP_POINT				2U
#define TSC_CUBE_AUTO_SPAN_SEAM			2U
#define TSC_FORCE_UNNORMALIZED			1U

/*
 * The block-linear texture header (its version and the 2D type), and the
 * sampler's linear mipmap filter and the largest level of detail clamp
 * (4.8 fixed point).
 *
 * Values referenced: Mesa 25.0.7
 * src/nouveau/headers/nvidia/classes/clb097tex.h
 * (NVB097_TEXHEAD_BL_HEADER_VERSION_SELECT_BLOCKLINEAR,
 * NVB097_TEXHEAD_BL_TEXTURE_TYPE_TWO_D, NVB097_TEXSAMP1_MIP_FILTER_MIP_LINEAR,
 * NVB097_TEXSAMP2_MAX_LOD_CLAMP).
 */
#define TIC_VERSION_BLOCKLINEAR			3U
#define TIC_TYPE_TWO_D				1U

/*
 * The texture header of a buffer (its version and type, one dimension).
 *
 * Values referenced: Mesa 25.0.7
 * src/nouveau/headers/nvidia/classes/clb097tex.h
 * (NVB097_TEXHEAD_1D_HEADER_VERSION_SELECT_ONE_D_BUFFER,
 * NVB097_TEXHEAD_1D_TEXTURE_TYPE_ONE_D_BUFFER).
 */
#define TIC_VERSION_ONE_D_BUFFER		0U
#define TIC_MULTI_SAMPLE_2X2			2U
#define TIC_TYPE_ONE_D_BUFFER			6U

/*
 * The one- and two-channel 8-bit component sizes of a texture header (a
 * video picture's luma plane as R8, its chroma plane as R8G8, red in the
 * lowest byte), and blocks two GOBs high.
 *
 * Values referenced: Mesa 25.0.7
 * src/nouveau/headers/nvidia/classes/clb097tex.h
 * (NVB097_TEXHEAD_BL_COMPONENTS_SIZES_R8, _SIZES_G8R8,
 * NVB097_TEXHEAD_BL_GOBS_PER_BLOCK_HEIGHT_TWO_GOBS).
 */
#define TIC_COMPONENTS_R8			0x1dU
#define TIC_COMPONENTS_G8R8			0x18U
#define TIC_BLOCK_HEIGHT_TWO_GOBS		1U
#define TSC_MIP_LINEAR				3U
#define TSC_LOD_MAX				0xfffU

/*
 * The words of a texture header and of a sampler.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc597tex.h (the 256-bit
 * NVC597_TEXHEAD_PITCH_* and NVC597_TEXSAMP* layouts).
 */
#define EXEC_TIC_WORDS				8U
#define EXEC_TSC_WORDS				8U

/* The fixed point the viewport is worked in: eight fractional bits. */
#define EXEC_FIXED_FRACTION			8U

/* A float's fields. */
#define FLOAT_SIGN				0x80000000U
#define FLOAT_EXPONENT_SHIFT			23U
#define FLOAT_EXPONENT_MASK			0xffU
#define FLOAT_MANTISSA_MASK			0x7fffffU
#define FLOAT_HIDDEN				0x800000U
#define FLOAT_BIAS				127
#define FLOAT_MANTISSA_BITS			23

/*
 * The fixed point a clear colour's normalized value is quantized in: a
 * float's lowest bit that can still round to a step of a 16-bit channel
 * (2^-17 and its 23 bits below) is whole in it.
 */
#define EXEC_UNORM_FRACTION			40U

/*
 * E5B9G9R9_UFLOAT_PACK32: the bits of a channel's mantissa (N), the
 * exponent's bias (B), and the largest value it holds (sharedexp_max,
 * 65408 = 511/512 of 2^16) as a float's bits.
 *
 * Values referenced: the Vulkan 1.3 specification, "Shared Exponent".
 */
#define EXEC_SHARED_BITS			9
#define EXEC_SHARED_BIAS			15
#define EXEC_SHARED_LARGEST			0x477f8000U

static size_t exec_method(uint8_t *push, size_t at, uint32_t method, uint32_t count);
static size_t exec_set(uint8_t *push, size_t at, uint32_t method, uint32_t value);
static uint32_t exec_begin(uint32_t topology);
static uint32_t exec_blend_op(uint32_t op);
static uint32_t exec_stencil_op(uint32_t op);
static uint32_t exec_compare_op(uint32_t op);
static uint32_t exec_blend_factor(uint32_t factor);
static const struct exec_vertex_format *exec_vertex_format_find(uint32_t format);
static uint32_t exec_attribute_word(uint32_t format);
static uint32_t exec_address_mode(uint32_t mode);
static void exec_field(uint32_t *words, unsigned low, unsigned bits, uint32_t value);
static uint32_t exec_swizzle_source(uint32_t swizzle, const uint32_t *identity, uint32_t one);
static uint32_t exec_texel_bytes(uint32_t format);
static uint32_t exec_lod(uint32_t bits);
static uint32_t exec_unorm_bits(uint32_t format, uint32_t component);
static uint32_t exec_snorm_bits(uint32_t format);
static uint32_t exec_unorm_quantize(uint32_t bits, uint32_t width);
static bool exec_srgb_channel(uint32_t format, uint32_t component);
static uint32_t exec_srgb_quantize(uint32_t bits);
static uint32_t exec_shared_mantissa(uint32_t bits, int32_t scale);

/*
 * The sRGB steps of an 8-bit channel: exec_srgb_steps[q] is the linear
 * value (a float's bits) whose encoding is q / 255 exactly, and
 * exec_srgb_rounding[q] the least linear float whose encoding times 255
 * rounds to q + 1 or more (the halves up), so a linear value takes the
 * step of how many of them it reaches.  Computed from the specification's
 * transfer functions (the encoding 12.92 c below 0.0031308, else
 * 1.055 c^(1/2.4) - 0.055; the decoding its inverse) in double precision,
 * each step checked to encode back to itself.
 *
 * Values referenced: the Khronos Data Format Specification 1.3, "sRGB
 * transfer functions" (the EOTF and its inverse), which the Vulkan 1.3
 * specification names for the sRGB formats' conversion.
 */
#define EXEC_SRGB_STEPS				256U
static const uint32_t exec_srgb_steps[EXEC_SRGB_STEPS] = {
	0x00000000U, 0x399f22b4U, 0x3a1f22b4U, 0x3a6eb40eU, 0x3a9f22b4U, 0x3ac6eb61U, 0x3aeeb40eU, 0x3b0b3e5dU,
	0x3b1f22b4U, 0x3b33070aU, 0x3b46eb61U, 0x3b5b518eU, 0x3b70f18fU, 0x3b83e1c6U, 0x3b8fe616U, 0x3b9c87fdU,
	0x3ba9c9b6U, 0x3bb7ad6fU, 0x3bc6354aU, 0x3bd56360U, 0x3be539c1U, 0x3bf5ba71U, 0x3c0373b6U, 0x3c0c6153U,
	0x3c15a705U, 0x3c1f45beU, 0x3c293e6bU, 0x3c3391f7U, 0x3c3e4149U, 0x3c494d44U, 0x3c54b6c9U, 0x3c607eb4U,
	0x3c6ca5dfU, 0x3c792d22U, 0x3c830aa9U, 0x3c89af9fU, 0x3c9085dcU, 0x3c978dc6U, 0x3c9ec7c2U, 0x3ca63433U,
	0x3cadd37dU, 0x3cb5a602U, 0x3cbdac21U, 0x3cc5e63aU, 0x3cce54acU, 0x3cd6f7d5U, 0x3cdfd010U, 0x3ce8ddbaU,
	0x3cf2212dU, 0x3cfb9ac3U, 0x3d02a56aU, 0x3d0798ddU, 0x3d0ca7e6U, 0x3d11d2afU, 0x3d171964U, 0x3d1c7c30U,
	0x3d21fb3cU, 0x3d2796b2U, 0x3d2d4ebbU, 0x3d332381U, 0x3d39152bU, 0x3d3f23e4U, 0x3d454fd2U, 0x3d4b991dU,
	0x3d51ffecU, 0x3d588468U, 0x3d5f26b6U, 0x3d65e6fdU, 0x3d6cc563U, 0x3d73c20eU, 0x3d7add24U, 0x3d810b65U,
	0x3d84b793U, 0x3d88732eU, 0x3d8c3e48U, 0x3d9018f4U, 0x3d940344U, 0x3d97fd49U, 0x3d9c0715U, 0x3da020baU,
	0x3da44a4aU, 0x3da883d6U, 0x3daccd6fU, 0x3db12727U, 0x3db5910fU, 0x3dba0b38U, 0x3dbe95b3U, 0x3dc33090U,
	0x3dc7dbe0U, 0x3dcc97b4U, 0x3dd1641dU, 0x3dd6412bU, 0x3ddb2eeeU, 0x3de02d76U, 0x3de53cd4U, 0x3dea5d18U,
	0x3def8e51U, 0x3df4d090U, 0x3dfa23e5U, 0x3dff885eU, 0x3e027f06U, 0x3e05427fU, 0x3e080ea2U, 0x3e0ae377U,
	0x3e0dc104U, 0x3e10a753U, 0x3e13966aU, 0x3e168e51U, 0x3e198f0fU, 0x3e1c98acU, 0x3e1fab30U, 0x3e22c6a1U,
	0x3e25eb07U, 0x3e29186aU, 0x3e2c4ed0U, 0x3e2f8e42U, 0x3e32d6c5U, 0x3e362862U, 0x3e39831fU, 0x3e3ce703U,
	0x3e405417U, 0x3e43ca60U, 0x3e4749e6U, 0x3e4ad2afU, 0x3e4e64c3U, 0x3e520029U, 0x3e55a4e7U, 0x3e595305U,
	0x3e5d0a89U, 0x3e60cb7aU, 0x3e6495dfU, 0x3e6869beU, 0x3e6c471fU, 0x3e702e07U, 0x3e741e7eU, 0x3e78188bU,
	0x3e7c1c33U, 0x3e8014bfU, 0x3e822039U, 0x3e84308bU, 0x3e8645b8U, 0x3e885fc3U, 0x3e8a7eb0U, 0x3e8ca281U,
	0x3e8ecb3bU, 0x3e90f8dfU, 0x3e932b72U, 0x3e9562f6U, 0x3e979f6fU, 0x3e99e0e0U, 0x3e9c274cU, 0x3e9e72b6U,
	0x3ea0c321U, 0x3ea31890U, 0x3ea57307U, 0x3ea7d288U, 0x3eaa3716U, 0x3eaca0b6U, 0x3eaf0f68U, 0x3eb18332U,
	0x3eb3fc15U, 0x3eb67a14U, 0x3eb8fd34U, 0x3ebb8576U, 0x3ebe12deU, 0x3ec0a56eU, 0x3ec33d2aU, 0x3ec5da14U,
	0x3ec87c30U, 0x3ecb2380U, 0x3ecdd008U, 0x3ed081caU, 0x3ed338c9U, 0x3ed5f508U, 0x3ed8b68aU, 0x3edb7d52U,
	0x3ede4963U, 0x3ee11abfU, 0x3ee3f169U, 0x3ee6cd65U, 0x3ee9aeb5U, 0x3eec955bU, 0x3eef815cU, 0x3ef272b8U,
	0x3ef56974U, 0x3ef86593U, 0x3efb6716U, 0x3efe6e00U, 0x3f00bd2bU, 0x3f02460cU, 0x3f03d1a5U, 0x3f055ff7U,
	0x3f06f104U, 0x3f0884cdU, 0x3f0a1b54U, 0x3f0bb499U, 0x3f0d509fU, 0x3f0eef65U, 0x3f1090efU, 0x3f12353dU,
	0x3f13dc50U, 0x3f15862aU, 0x3f1732ccU, 0x3f18e237U, 0x3f1a946eU, 0x3f1c4970U, 0x3f1e0140U, 0x3f1fbbdeU,
	0x3f21794dU, 0x3f23398cU, 0x3f24fc9fU, 0x3f26c285U, 0x3f288b41U, 0x3f2a56d2U, 0x3f2c253cU, 0x3f2df67fU,
	0x3f2fca9cU, 0x3f31a194U, 0x3f337b6aU, 0x3f35581dU, 0x3f3737b0U, 0x3f391a24U, 0x3f3aff7aU, 0x3f3ce7b2U,
	0x3f3ed2cfU, 0x3f40c0d2U, 0x3f42b1bcU, 0x3f44a58eU, 0x3f469c49U, 0x3f4895efU, 0x3f4a9280U, 0x3f4c91ffU,
	0x3f4e946cU, 0x3f5099c9U, 0x3f52a216U, 0x3f54ad56U, 0x3f56bb88U, 0x3f58ccafU, 0x3f5ae0ccU, 0x3f5cf7dfU,
	0x3f5f11eaU, 0x3f612eefU, 0x3f634eeeU, 0x3f6571e9U, 0x3f6797e0U, 0x3f69c0d5U, 0x3f6beccaU, 0x3f6e1bbfU,
	0x3f704db5U, 0x3f7282aeU, 0x3f74baabU, 0x3f76f5aeU, 0x3f7933b6U, 0x3f7b74c6U, 0x3f7db8deU, 0x3f800000U
};
static const uint32_t exec_srgb_rounding[EXEC_SRGB_STEPS - 1U] = {
	0x391f22b4U, 0x39eeb40eU, 0x3a46eb61U, 0x3a8b3e5eU, 0x3ab3070bU, 0x3adacfb8U, 0x3b014c33U, 0x3b153089U,
	0x3b2914dfU, 0x3b3cf936U, 0x3b50f2d1U, 0x3b65fb9bU, 0x3b7c3403U, 0x3b89d060U, 0x3b962333U, 0x3ba314bdU,
	0x3bb0a731U, 0x3bbedcb7U, 0x3bcdb76dU, 0x3bdd3967U, 0x3bed64afU, 0x3bfe3b46U, 0x3c07df91U, 0x3c10f91bU,
	0x3c1a6b32U, 0x3c2436c8U, 0x3c2e5cc7U, 0x3c38de1aU, 0x3c43bba4U, 0x3c4ef648U, 0x3c5a8ee4U, 0x3c668654U,
	0x3c72dd71U, 0x3c7f950fU, 0x3c865702U, 0x3c8d148fU, 0x3c940396U, 0x3c9b247cU, 0x3ca277a6U, 0x3ca9fd78U,
	0x3cb1b653U, 0x3cb9a298U, 0x3cc1c2a9U, 0x3cca16e3U, 0x3cd29fa4U, 0x3cdb5d4bU, 0x3ce45032U, 0x3ced78b5U,
	0x3cf6d72eU, 0x3d0035fcU, 0x3d051bb4U, 0x3d0a1cecU, 0x3d0f39d0U, 0x3d14728aU, 0x3d19c745U, 0x3d1f382cU,
	0x3d24c567U, 0x3d2a6f22U, 0x3d303584U, 0x3d3618b7U, 0x3d3c18e4U, 0x3d423632U, 0x3d4870caU, 0x3d4ec8d2U,
	0x3d553e73U, 0x3d5bd1d3U, 0x3d628318U, 0x3d69526aU, 0x3d703feeU, 0x3d774bcaU, 0x3d7e7624U, 0x3d82df90U,
	0x3d869372U, 0x3d8a56cbU, 0x3d8e29abU, 0x3d920c27U, 0x3d95fe4fU, 0x3d9a0035U, 0x3d9e11ecU, 0x3da23384U,
	0x3da66510U, 0x3daaa6a0U, 0x3daef847U, 0x3db35a15U, 0x3db7cc1bU, 0x3dbc4e6bU, 0x3dc0e114U, 0x3dc58429U,
	0x3dca37b9U, 0x3dcefbd6U, 0x3dd3d08fU, 0x3dd8b5f5U, 0x3dddac19U, 0x3de2b30aU, 0x3de7cad9U, 0x3decf395U,
	0x3df22d50U, 0x3df77817U, 0x3dfcd3fcU, 0x3e012087U, 0x3e03dfaeU, 0x3e06a77bU, 0x3e0977f6U, 0x3e0c5126U,
	0x3e0f3314U, 0x3e121dc5U, 0x3e151143U, 0x3e180d95U, 0x3e1b12c2U, 0x3e1e20d1U, 0x3e2137cbU, 0x3e2457b6U,
	0x3e278099U, 0x3e2ab27dU, 0x3e2ded68U, 0x3e313161U, 0x3e347e70U, 0x3e37d49cU, 0x3e3b33ecU, 0x3e3e9c67U,
	0x3e420e15U, 0x3e4588fbU, 0x3e490d22U, 0x3e4c9a90U, 0x3e50314cU, 0x3e53d15dU, 0x3e577acaU, 0x3e5b2d9aU,
	0x3e5ee9d4U, 0x3e62af7eU, 0x3e667e9fU, 0x3e6a573eU, 0x3e6e3962U, 0x3e722511U, 0x3e761a52U, 0x3e7a192cU,
	0x3e7e21a5U, 0x3e8119e2U, 0x3e8327c7U, 0x3e853a86U, 0x3e875222U, 0x3e896e9dU, 0x3e8b8ffcU, 0x3e8db641U,
	0x3e8fe170U, 0x3e92118bU, 0x3e944696U, 0x3e968095U, 0x3e98bf89U, 0x3e9b0377U, 0x3e9d4c62U, 0x3e9f9a4cU,
	0x3ea1ed38U, 0x3ea4452bU, 0x3ea6a226U, 0x3ea9042eU, 0x3eab6b44U, 0x3eadd76dU, 0x3eb048aaU, 0x3eb2bf00U,
	0x3eb53a71U, 0x3eb7bb00U, 0x3eba40b1U, 0x3ebccb85U, 0x3ebf5b81U, 0x3ec1f0a7U, 0x3ec48af9U, 0x3ec72a7cU,
	0x3ec9cf32U, 0x3ecc791eU, 0x3ecf2842U, 0x3ed1dca2U, 0x3ed49641U, 0x3ed75521U, 0x3eda1946U, 0x3edce2b2U,
	0x3edfb168U, 0x3ee2856aU, 0x3ee55ebdU, 0x3ee83d63U, 0x3eeb215dU, 0x3eee0ab1U, 0x3ef0f95fU, 0x3ef3ed6bU,
	0x3ef6e6d8U, 0x3ef9e5a8U, 0x3efce9deU, 0x3efff37eU, 0x3f018145U, 0x3f030b82U, 0x3f049877U, 0x3f062827U,
	0x3f07ba92U, 0x3f094fb9U, 0x3f0ae79fU, 0x3f0c8244U, 0x3f0e1faaU, 0x3f0fbfd2U, 0x3f1162beU, 0x3f13086eU,
	0x3f14b0e4U, 0x3f165c22U, 0x3f180a29U, 0x3f19bafaU, 0x3f1b6e96U, 0x3f1d24ffU, 0x3f1ede36U, 0x3f209a3cU,
	0x3f225913U, 0x3f241abcU, 0x3f25df38U, 0x3f27a689U, 0x3f2970afU, 0x3f2b3dadU, 0x3f2d0d83U, 0x3f2ee032U,
	0x3f30b5bdU, 0x3f328e24U, 0x3f346968U, 0x3f36478bU, 0x3f38288fU, 0x3f3a0c73U, 0x3f3bf33aU, 0x3f3ddce5U,
	0x3f3fc975U, 0x3f41b8ebU, 0x3f43ab48U, 0x3f45a08fU, 0x3f4798bfU, 0x3f4993dbU, 0x3f4b91e3U, 0x3f4d92d8U,
	0x3f4f96bdU, 0x3f519d92U, 0x3f53a758U, 0x3f55b411U, 0x3f57c3beU, 0x3f59d65fU, 0x3f5bebf7U, 0x3f5e0486U,
	0x3f60200eU, 0x3f623e90U, 0x3f64600cU, 0x3f668485U, 0x3f68abfbU, 0x3f6ad670U, 0x3f6d03e5U, 0x3f6f345aU,
	0x3f7167d2U, 0x3f739e4dU, 0x3f75d7ccU, 0x3f781451U, 0x3f7a53ddU, 0x3f7c9671U, 0x3f7edc0eU
};

/*
 * The colour formats the executor samples and renders to: each's bytes a
 * texel, its colour target format, its texture header's component sizes
 * and data type (every component's), and where its red, green, blue and
 * alpha come from among the header's components (the image's identity).
 *
 * Values referenced: Mesa 25.0.7 src/nouveau/nil/nil_formats.csv (the
 * czt, tcs, type and src columns of R8_UNORM, R8G8_UNORM, B8G8R8A8_UNORM
 * and _SRGB, R8G8B8A8_UNORM and _SRGB, R10G10B10A2_UNORM,
 * R11G11B10_FLOAT, R16_FLOAT, R16G16_FLOAT, R16G16B16A16_FLOAT,
 * R16G16B16A16_UNORM, R32_FLOAT, R32G32_FLOAT, R32G32B32A32_FLOAT, the
 * _UINT and _SINT of R8, R8G8, R8G8B8A8, R16, R16G16, R16G16B16A16, R32,
 * R32G32, R32G32B32A32 and R10G10B10A2_UINT, R16_UNORM, R16G16_UNORM, the
 * _SNORM of R8, R8G8, R8G8B8A8, R16, R16G16 and R16G16B16A16, and
 * R9G9B9E5_FLOAT: Vulkan's A2B10G10R10_UNORM_PACK32,
 * B10G11R11_UFLOAT_PACK32, A2B10G10R10_UINT_PACK32 and
 * E5B9G9R9_UFLOAT_PACK32 are the packed ones; an integer format's missing
 * alpha is an integer one; R9G9B9E5_FLOAT's czt is NONE, so it is sampled
 * but not rendered to, its target here 0),
 * src/nouveau/headers/nvidia/classes/clc597.h
 * (NVC597_SET_COLOR_TARGET_FORMAT_V_*) and clb097tex.h
 * (NVB097_TEXHEAD_BL_COMPONENTS_SIZES_*, NVB097_TEXHEAD_BL_R_DATA_TYPE_NUM_*,
 * NVB097_TEXHEAD_BL_X_SOURCE_IN_*).
 */
#define EXEC_TIC_SNORM				1U
#define EXEC_TIC_UNORM				2U
#define EXEC_TIC_SINT				3U
#define EXEC_TIC_UINT				4U
#define EXEC_TIC_FLOAT				7U
#define EXEC_SOURCE_ONE				TIC_SOURCE_ONE_FLOAT
#define EXEC_SOURCE_INT				TIC_SOURCE_ONE_INT
#define EXEC_RGBA				{ TIC_SOURCE_R, TIC_SOURCE_G, TIC_SOURCE_B, TIC_SOURCE_A }
#define EXEC_RG_INT				{ TIC_SOURCE_R, TIC_SOURCE_G, TIC_SOURCE_ZERO, EXEC_SOURCE_INT }
#define EXEC_R_INT				{ TIC_SOURCE_R, TIC_SOURCE_ZERO, TIC_SOURCE_ZERO, EXEC_SOURCE_INT }
#define EXEC_RGB				{ TIC_SOURCE_R, TIC_SOURCE_G, TIC_SOURCE_B, EXEC_SOURCE_ONE }
#define EXEC_RG					{ TIC_SOURCE_R, TIC_SOURCE_G, TIC_SOURCE_ZERO, EXEC_SOURCE_ONE }
#define EXEC_R					{ TIC_SOURCE_R, TIC_SOURCE_ZERO, TIC_SOURCE_ZERO, EXEC_SOURCE_ONE }
static const struct nvrtx_exec_format exec_formats[] = {
	{ VK_FORMAT_B8G8R8A8_UNORM, 4U, 0xcfU, 0x08U, EXEC_TIC_UNORM, { TIC_SOURCE_B, TIC_SOURCE_G, TIC_SOURCE_R, TIC_SOURCE_A }, 0U },
	{ VK_FORMAT_B8G8R8A8_SRGB, 4U, 0xd0U, 0x08U, EXEC_TIC_UNORM, { TIC_SOURCE_B, TIC_SOURCE_G, TIC_SOURCE_R, TIC_SOURCE_A }, 1U },
	{ VK_FORMAT_R8G8B8A8_UNORM, 4U, 0xd5U, 0x08U, EXEC_TIC_UNORM, { TIC_SOURCE_R, TIC_SOURCE_G, TIC_SOURCE_B, TIC_SOURCE_A }, 0U },
	{ VK_FORMAT_R8G8B8A8_SRGB, 4U, 0xd6U, 0x08U, EXEC_TIC_UNORM, { TIC_SOURCE_R, TIC_SOURCE_G, TIC_SOURCE_B, TIC_SOURCE_A }, 1U },
	{ VK_FORMAT_R8_UNORM, 1U, 0xf3U, 0x1dU, EXEC_TIC_UNORM, { TIC_SOURCE_R, TIC_SOURCE_ZERO, TIC_SOURCE_ZERO, EXEC_SOURCE_ONE }, 0U },
	{ VK_FORMAT_R8G8_UNORM, 2U, 0xeaU, 0x18U, EXEC_TIC_UNORM, { TIC_SOURCE_R, TIC_SOURCE_G, TIC_SOURCE_ZERO, EXEC_SOURCE_ONE }, 0U },
	{ VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4U, 0xd1U, 0x09U, EXEC_TIC_UNORM, { TIC_SOURCE_R, TIC_SOURCE_G, TIC_SOURCE_B, TIC_SOURCE_A }, 0U },
	{ VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4U, 0xe0U, 0x21U, EXEC_TIC_FLOAT, { TIC_SOURCE_R, TIC_SOURCE_G, TIC_SOURCE_B, EXEC_SOURCE_ONE }, 0U },
	{ VK_FORMAT_R16_SFLOAT, 2U, 0xf2U, 0x1bU, EXEC_TIC_FLOAT, { TIC_SOURCE_R, TIC_SOURCE_ZERO, TIC_SOURCE_ZERO, EXEC_SOURCE_ONE }, 0U },
	{ VK_FORMAT_R16G16_SFLOAT, 4U, 0xdeU, 0x0cU, EXEC_TIC_FLOAT, { TIC_SOURCE_R, TIC_SOURCE_G, TIC_SOURCE_ZERO, EXEC_SOURCE_ONE }, 0U },
	{ VK_FORMAT_R16G16B16A16_SFLOAT, 8U, 0xcaU, 0x03U, EXEC_TIC_FLOAT, { TIC_SOURCE_R, TIC_SOURCE_G, TIC_SOURCE_B, TIC_SOURCE_A }, 0U },
	{ VK_FORMAT_R16G16B16A16_UNORM, 8U, 0xc6U, 0x03U, EXEC_TIC_UNORM, { TIC_SOURCE_R, TIC_SOURCE_G, TIC_SOURCE_B, TIC_SOURCE_A }, 0U },
	{ VK_FORMAT_R16_UNORM, 2U, 0xeeU, 0x1bU, EXEC_TIC_UNORM, EXEC_R, 0U },
	{ VK_FORMAT_R16G16_UNORM, 4U, 0xdaU, 0x0cU, EXEC_TIC_UNORM, EXEC_RG, 0U },
	{ VK_FORMAT_R8_SNORM, 1U, 0xf4U, 0x1dU, EXEC_TIC_SNORM, EXEC_R, 0U },
	{ VK_FORMAT_R8G8_SNORM, 2U, 0xebU, 0x18U, EXEC_TIC_SNORM, EXEC_RG, 0U },
	{ VK_FORMAT_R8G8B8A8_SNORM, 4U, 0xd7U, 0x08U, EXEC_TIC_SNORM, EXEC_RGBA, 0U },
	{ VK_FORMAT_R16_SNORM, 2U, 0xefU, 0x1bU, EXEC_TIC_SNORM, EXEC_R, 0U },
	{ VK_FORMAT_R16G16_SNORM, 4U, 0xdbU, 0x0cU, EXEC_TIC_SNORM, EXEC_RG, 0U },
	{ VK_FORMAT_R16G16B16A16_SNORM, 8U, 0xc7U, 0x03U, EXEC_TIC_SNORM, EXEC_RGBA, 0U },
	{ VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 4U, 0U, 0x20U, EXEC_TIC_FLOAT, EXEC_RGB, 0U },
	{ VK_FORMAT_R32_SFLOAT, 4U, 0xe5U, 0x0fU, EXEC_TIC_FLOAT, { TIC_SOURCE_R, TIC_SOURCE_ZERO, TIC_SOURCE_ZERO, EXEC_SOURCE_ONE }, 0U },
	{ VK_FORMAT_R32G32_SFLOAT, 8U, 0xcbU, 0x04U, EXEC_TIC_FLOAT, { TIC_SOURCE_R, TIC_SOURCE_G, TIC_SOURCE_ZERO, EXEC_SOURCE_ONE }, 0U },
	{ VK_FORMAT_R32G32B32A32_SFLOAT, 16U, 0xc0U, 0x01U, EXEC_TIC_FLOAT, { TIC_SOURCE_R, TIC_SOURCE_G, TIC_SOURCE_B, TIC_SOURCE_A }, 0U },
	{ VK_FORMAT_R8_UINT, 1U, 0xf6U, 0x1dU, EXEC_TIC_UINT, EXEC_R_INT, 0U },
	{ VK_FORMAT_R8_SINT, 1U, 0xf5U, 0x1dU, EXEC_TIC_SINT, EXEC_R_INT, 0U },
	{ VK_FORMAT_R8G8_UINT, 2U, 0xedU, 0x18U, EXEC_TIC_UINT, EXEC_RG_INT, 0U },
	{ VK_FORMAT_R8G8_SINT, 2U, 0xecU, 0x18U, EXEC_TIC_SINT, EXEC_RG_INT, 0U },
	{ VK_FORMAT_R8G8B8A8_UINT, 4U, 0xd9U, 0x08U, EXEC_TIC_UINT, EXEC_RGBA, 0U },
	{ VK_FORMAT_R8G8B8A8_SINT, 4U, 0xd8U, 0x08U, EXEC_TIC_SINT, EXEC_RGBA, 0U },
	{ VK_FORMAT_A2B10G10R10_UINT_PACK32, 4U, 0xd2U, 0x09U, EXEC_TIC_UINT, EXEC_RGBA, 0U },
	{ VK_FORMAT_R16_UINT, 2U, 0xf1U, 0x1bU, EXEC_TIC_UINT, EXEC_R_INT, 0U },
	{ VK_FORMAT_R16_SINT, 2U, 0xf0U, 0x1bU, EXEC_TIC_SINT, EXEC_R_INT, 0U },
	{ VK_FORMAT_R16G16_UINT, 4U, 0xddU, 0x0cU, EXEC_TIC_UINT, EXEC_RG_INT, 0U },
	{ VK_FORMAT_R16G16_SINT, 4U, 0xdcU, 0x0cU, EXEC_TIC_SINT, EXEC_RG_INT, 0U },
	{ VK_FORMAT_R16G16B16A16_UINT, 8U, 0xc9U, 0x03U, EXEC_TIC_UINT, EXEC_RGBA, 0U },
	{ VK_FORMAT_R16G16B16A16_SINT, 8U, 0xc8U, 0x03U, EXEC_TIC_SINT, EXEC_RGBA, 0U },
	{ VK_FORMAT_R32_UINT, 4U, 0xe4U, 0x0fU, EXEC_TIC_UINT, EXEC_R_INT, 0U },
	{ VK_FORMAT_R32_SINT, 4U, 0xe3U, 0x0fU, EXEC_TIC_SINT, EXEC_R_INT, 0U },
	{ VK_FORMAT_R32G32_UINT, 8U, 0xcdU, 0x04U, EXEC_TIC_UINT, EXEC_RG_INT, 0U },
	{ VK_FORMAT_R32G32_SINT, 8U, 0xccU, 0x04U, EXEC_TIC_SINT, EXEC_RG_INT, 0U },
	{ VK_FORMAT_R32G32B32A32_UINT, 16U, 0xc2U, 0x01U, EXEC_TIC_UINT, EXEC_RGBA, 0U },
	{ VK_FORMAT_R32G32B32A32_SINT, 16U, 0xc1U, 0x01U, EXEC_TIC_SINT, EXEC_RGBA, 0U },
};

/*
 * The depth formats the executor renders depth into, samples (its depth,
 * read as red) and copies: each's bytes a texel, its depth target format,
 * its texture header's component sizes and the data types of its depth
 * and of its stencil, whether it has a stencil, and the memory kind of
 * its pages (given at the bind: a depth target and a texture read of the
 * format both need it).
 *
 * Values referenced: Mesa 25.0.7 src/nouveau/nil/nil_formats.csv (the
 * czt, tcs and type columns of Z16_UNORM, Z32_FLOAT, Z24_UNORM_S8_UINT
 * and Z32_FLOAT_S8X24_UINT), src/nouveau/nil/image.rs
 * (tu102_choose_pte_kind, uncompressed),
 * src/nouveau/headers/nvidia/classes/clc597.h
 * (NVC597_SET_ZT_FORMAT_V_Z16, _ZF32, _S8Z24, _ZF32_X24S8) and
 * clb097tex.h (NVB097_TEXHEAD_BL_COMPONENTS_SIZES_Z16, _ZF32, _S8Z24,
 * _ZF32_X24S8).
 */
static const struct nvrtx_exec_depth_format exec_depth_formats[] = {
	{ VK_FORMAT_D16_UNORM, 2U, V_ZT_FORMAT_Z16, 0x3aU, EXEC_TIC_UNORM, EXEC_TIC_UNORM, 0U, NVRTX_MMU_KIND_Z16 },
	{ VK_FORMAT_D32_SFLOAT, 4U, V_ZT_FORMAT_ZF32, 0x2fU, EXEC_TIC_FLOAT, EXEC_TIC_FLOAT, 0U, NVRTX_MMU_KIND_GENERIC_MEMORY },
	{ VK_FORMAT_D24_UNORM_S8_UINT, 4U, V_ZT_FORMAT_S8Z24, 0x2bU, EXEC_TIC_UNORM, EXEC_TIC_UINT, 1U, NVRTX_MMU_KIND_S8Z24 },
	{ VK_FORMAT_D32_SFLOAT_S8_UINT, 8U, V_ZT_FORMAT_ZF32_X24S8, 0x30U, EXEC_TIC_FLOAT, EXEC_TIC_UINT, 1U, NVRTX_MMU_KIND_ZF32_X24S8 },
};

/* Reports a colour format's entry of the executor's formats, or NULL for one it does not sample or render to. */
const struct nvrtx_exec_format *
nvrtx_exec_format_find(
	uint32_t format)
{
	size_t index;

	/* Each entry in turn. */
	for (index = 0U; index < sizeof(exec_formats) / sizeof(exec_formats[0]); index++) {
		if (exec_formats[index].format == format)
			return &exec_formats[index];
	}

	/* Not one of them. */
	return NULL;
}

/* Reports whether vertex attributes fetch a format. */
bool
nvrtx_exec_vertex_format(
	uint32_t format)
{
	const struct exec_vertex_format *found;

	/* The vertex formats' table. */
	found = exec_vertex_format_find(format);
	return found != NULL;
}

/* Reports whether a colour format of the executor's is an integer one (read and written as integers, not blended or filtered). */
bool
nvrtx_exec_format_integer(
	uint32_t format)
{
	const struct nvrtx_exec_format *entry;

	/* One of the executor's colour formats, of unsigned or signed integers. */
	entry = nvrtx_exec_format_find(format);
	if (entry == NULL)
		return false;
	if (entry->type != EXEC_TIC_UINT && entry->type != EXEC_TIC_SINT)
		return false;

	/* Succeeded: an integer format. */
	return true;
}

/*
 * Reports whether a colour format of the executor's is read from a buffer
 * (a uniform texel buffer): every one with a colour target format but the
 * sRGB ones, as nil gives its formats the buffer support.
 *
 * Values referenced: Mesa 25.0.7 src/nouveau/nil/nil_formats.csv (the B
 * of the support column: the formats rendered to but B8G8R8A8_SRGB and
 * R8G8B8A8_SRGB, not R9G9B9E5_FLOAT).
 */
bool
nvrtx_exec_texel_format(
	uint32_t format)
{
	const struct nvrtx_exec_format *entry;

	/* One of the executor's colour formats, rendered to and not sRGB. */
	entry = nvrtx_exec_format_find(format);
	if (entry == NULL)
		return false;
	if (entry->target == 0U || entry->srgb != 0U)
		return false;

	/* Succeeded: a texel buffer's format. */
	return true;
}

/* Reports a depth format's entry of the executor's depth formats, or NULL for another format. */
const struct nvrtx_exec_depth_format *
nvrtx_exec_depth_format_find(
	uint32_t format)
{
	size_t index;

	/* Each entry in turn. */
	for (index = 0U; index < sizeof(exec_depth_formats) / sizeof(exec_depth_formats[0]); index++) {
		if (exec_depth_formats[index].format == format)
			return &exec_depth_formats[index];
	}

	/* Not one of them. */
	return NULL;
}

/* Reports the bytes a texel of one of the executor's colour formats takes (four for another). */
static uint32_t
exec_texel_bytes(
	uint32_t format)
{
	const struct nvrtx_exec_format *entry;

	/* The format's own. */
	entry = nvrtx_exec_format_find(format);
	if (entry == NULL)
		return 4U;

	/* Succeeded: its bytes. */
	return entry->bytes;
}

/*
 * Reports the colour target format of a VkFormat the executor renders
 * to, or false for another.
 */
bool
nvrtx_exec_target_format(
	uint32_t format,
	uint32_t *code)
{
	const struct nvrtx_exec_format *entry;

	/* One of the executor's colour formats that has a target format (not one only sampled). */
	entry = nvrtx_exec_format_find(format);
	if (entry == NULL || entry->target == 0U) {
		*code = 0U;
		return false;
	}

	/* Succeeded: its target format. */
	*code = entry->target;
	return true;
}

/*
 * Writes the colour targets (target 0 pitch or block linear, the others
 * of a pass of several block linear, each fragment program location its
 * target; a pass of no colour target has none, every target null) and the
 * rendering's fixed state: the surface clip, the targets after the pass's
 * null, the depth target (its format's, its row in texels) or none, the
 * samples a pixel (one, or four in 2x2 blocks: the targets' heights in
 * samples) and their positions, the layers (a layered pass's targets
 * each its layers from its first, an array pitch apart, a program's
 * gl_Layer choosing one), filled triangles without culling, every
 * sample, no alpha to coverage, no depth, stencil or logic operation, each
 * target's channels written by its own mask.
 *
 * Returns where the push buffer goes on.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_CmdBeginRendering: a block-linear target's width its row pitch over
 * the format's bytes; nvk_flush_cb_state: SET_CT_SELECT's target
 * count and the target of each location, SET_CT_WRITE for each target
 * with SET_SINGLE_CT_WRITE_CONTROL off when their masks differ; the
 * depth target's format, its array pitch and its width as the row's
 * pitch over the format's bytes).
 */
size_t
nvrtx_exec_target(
	uint8_t *push,
	size_t at,
	const struct nvrtx_exec_target *target)
{
	const struct nvrtx_exec_colour *other;
	const struct nvrtx_exec_depth_format *depth;
	uint32_t format;
	uint32_t index;
	uint32_t count;
	uint32_t select;
	uint32_t zt;
	uint32_t bytes;
	uint32_t rows;
	uint32_t layers;
	uint32_t positions;

	/* Each row of pixels two rows of samples with four samples a pixel; one layer, or the pass's. */
	rows = 1U;
	if (target->samples == 4U)
		rows = 2U;
	layers = 1U;
	if (target->layers > 1U)
		layers = target->layers;

	/* The format (one the caller checked; none for a pass of no colour target). */
	format = 0U;
	if (target->address != 0U)
		(void)nvrtx_exec_target_format(target->format, &format);

	/* The surface clip: the whole target. */
	at = exec_method(push, at, M_SET_SURFACE_CLIP_HORIZONTAL, 2U);
	nvrtx_put32(push, at, target->width << 16);
	nvrtx_put32(push, at + 4U, target->height << 16);
	at += 8U;

	/*
	 * Colour target 0: the address, the pitch in place of the width (block
	 * linear: the row's pixels), the height, the format, pitch layout (or
	 * block linear in one-GOB blocks, its array pitch the whole level), one
	 * layer.  A pass of no colour target has it null with the others.
	 */
	count = target->colour_count;
	if (count == 0U && target->address != 0U)
		count = 1U;
	if (target->address != 0U)
		at = exec_method(push, at, M_SET_COLOR_TARGET_A, 9U);
	if (target->address != 0U) {
		nvrtx_put32(push, at, (uint32_t)(target->address >> 32));
		nvrtx_put32(push, at + 4U, (uint32_t)(target->address & 0xffffffffU));
		if (target->block_linear != 0U) {
			nvrtx_put32(push, at + 8U, target->pitch / exec_texel_bytes(target->format));
			nvrtx_put32(push, at + 20U, V_TARGET_LAYOUT_BLOCK_ONE_GOB);
			if (layers > 1U)
				nvrtx_put32(push, at + 28U, (uint32_t)(target->array_pitch >> 2));
			else
				nvrtx_put32(push, at + 28U, (uint32_t)(((uint64_t)target->pitch * ((target->height * rows + 7U) & ~7U)) >> 2));
		} else {
			nvrtx_put32(push, at + 8U, target->pitch);
			nvrtx_put32(push, at + 20U, V_TARGET_LAYOUT_PITCH);
			nvrtx_put32(push, at + 28U, 0U);
		}
		nvrtx_put32(push, at + 12U, target->height * rows);
		nvrtx_put32(push, at + 16U, format);
		nvrtx_put32(push, at + 24U, layers);
		nvrtx_put32(push, at + 32U, 0U);
		at += 36U;
	}
	at = exec_set(push, at, M_SET_COLOR_COMPRESSION, V_FALSE);

	/* The pass's other targets, block linear like target 0's; an unused one null. */
	select = count;
	for (index = 1U; index < count; index++) {
		other = &target->others[index - 1U];
		if (other->address == 0U)
			continue;
		(void)nvrtx_exec_target_format(other->format, &format);
		at = exec_method(push, at, M_SET_COLOR_TARGET_A + index * M_SET_COLOR_TARGET_STRIDE, 9U);
		nvrtx_put32(push, at, (uint32_t)(other->address >> 32));
		nvrtx_put32(push, at + 4U, (uint32_t)(other->address & 0xffffffffU));
		nvrtx_put32(push, at + 8U, other->pitch / exec_texel_bytes(other->format));
		nvrtx_put32(push, at + 12U, other->height * rows);
		nvrtx_put32(push, at + 16U, format);
		nvrtx_put32(push, at + 20U, V_TARGET_LAYOUT_BLOCK_ONE_GOB);
		nvrtx_put32(push, at + 24U, layers);
		if (layers > 1U)
			nvrtx_put32(push, at + 28U, (uint32_t)(other->array_pitch >> 2));
		else
			nvrtx_put32(push, at + 28U, (uint32_t)(((uint64_t)other->pitch * ((other->height * rows + 7U) & ~7U)) >> 2));
		nvrtx_put32(push, at + 32U, 0U);
		at += 36U;
	}

	/* The other targets null (target 0 too in a pass of no colour target): no address, a 64-wide pitch, no format, one layer. */
	for (index = 0U; index < EXEC_COLOR_TARGETS; index++) {
		if (index == 0U && target->address != 0U)
			continue;
		if (index != 0U && index < count && target->others[index - 1U].address != 0U)
			continue;
		at = exec_method(push, at, M_SET_COLOR_TARGET_A + index * M_SET_COLOR_TARGET_STRIDE, 9U);
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
	}

	/*
	 * The depth target when there is one: its address, its format's depth
	 * target format (without a known format ZF32, S8Z24 with a stencil) in
	 * blocks of one GOB, one layer the array's pitch, its width the row's
	 * texels (the depth hardware has no tile width), its height,
	 * uncompressed and not sparse.  One sample a pixel.
	 */
	if (target->depth_address != 0U) {
		depth = nvrtx_exec_depth_format_find(target->depth_format);
		zt = V_ZT_FORMAT_ZF32;
		if (target->depth_stencil != 0U)
			zt = V_ZT_FORMAT_S8Z24;
		bytes = 4U;
		if (depth != NULL) {
			zt = depth->target;
			bytes = depth->bytes;
		}
		at = exec_method(push, at, M_SET_ZT_A, 5U);
		nvrtx_put32(push, at, (uint32_t)(target->depth_address >> 32));
		nvrtx_put32(push, at + 4U, (uint32_t)(target->depth_address & 0xffffffffU));
		nvrtx_put32(push, at + 8U, zt);
		nvrtx_put32(push, at + 12U, V_ZT_BLOCK_ONE_GOB);
		if (layers > 1U)
			nvrtx_put32(push, at + 16U, (uint32_t)(target->depth_array_pitch >> 2));
		else
			nvrtx_put32(push, at + 16U, (uint32_t)(((uint64_t)target->depth_pitch * ((target->depth_height * rows + 7U) & ~7U)) >> 2));
		at += 20U;
		at = exec_set(push, at, M_SET_ZT_SELECT, 1U);
		at = exec_method(push, at, M_SET_ZT_SIZE_A, 3U);
		nvrtx_put32(push, at, target->depth_pitch / bytes);
		nvrtx_put32(push, at + 4U, target->depth_height * rows);
		nvrtx_put32(push, at + 8U, layers | (V_ZT_SIZE_ARRAY << 16));
		at += 12U;
		at = exec_set(push, at, M_SET_ZT_LAYER, 0U);
		at = exec_set(push, at, M_SET_Z_COMPRESSION, V_FALSE);
		at = exec_set(push, at, M_SET_ZT_SPARSE, V_FALSE);
	} else {
		at = exec_set(push, at, M_SET_ZT_SELECT, 0U);
	}

	/* One sample a pixel, or four in 2x2 blocks; the samples' positions within a pixel. */
	positions = V_POSITIONS_ONE;
	if (target->samples == 4U) {
		at = exec_set(push, at, M_SET_ANTI_ALIAS, V_ANTI_ALIAS_2X2);
		positions = V_POSITIONS_FOUR;
	} else {
		at = exec_set(push, at, M_SET_ANTI_ALIAS, V_ANTI_ALIAS_1X1);
	}
	at = exec_method(push, at, M_SET_ANTI_ALIAS_SAMPLE_POSITIONS, 4U);
	for (index = 0U; index < 4U; index++)
		nvrtx_put32(push, at + index * 4U, positions);
	at += 16U;

	/* The viewport's swizzle and depth clip, rasterization on, filled, not culled, the first vertex provoking. */
	at = exec_set(push, at, M_SET_VIEWPORT_COORDINATE_SWIZZLE, V_VIEWPORT_SWIZZLE_IDENTITY);
	at = exec_set(push, at, M_SET_VIEWPORT_Z_CLIP, V_Z_CLIP_ZERO_TO_W);
	at = exec_set(push, at, M_SET_RASTER_ENABLE, V_TRUE);
	at = exec_set(push, at, M_SET_VIEWPORT_CLIP_CONTROL, V_VIEWPORT_CLIP_DRAW);
	at = exec_method(push, at, M_SET_FRONT_POLYGON_MODE, 2U);
	nvrtx_put32(push, at, V_POLYGON_FILL);
	nvrtx_put32(push, at + 4U, V_POLYGON_FILL);
	at += 8U;
	at = exec_set(push, at, M_OGL_SET_CULL, V_FALSE);
	at = exec_set(push, at, M_OGL_SET_FRONT_FACE, V_FRONT_FACE_CCW);
	at = exec_set(push, at, M_SET_PROVOKING_VERTEX, 0U);

	/* Lines one pixel wide (wide lines are not drawn). */
	at = exec_method(push, at, M_SET_LINE_WIDTH_FLOAT, 2U);
	nvrtx_put32(push, at, V_FLOAT_ONE);
	nvrtx_put32(push, at + 4U, V_FLOAT_ONE);
	at += 8U;

	/* Every sample, no alpha to coverage. */
	at = exec_set(push, at, M_SET_ANTI_ALIAS_ALPHA_CONTROL, 0U);
	at = exec_method(push, at, M_SET_SAMPLE_MASK_X0_Y0, 4U);
	for (index = 0U; index < 4U; index++)
		nvrtx_put32(push, at + index * 4U, V_SAMPLE_MASK_ALL);
	at += 16U;

	/* No depth, stencil or logic operation; location n writes target n, each by its own mask. */
	at = exec_set(push, at, M_SET_DEPTH_TEST, V_FALSE);
	at = exec_set(push, at, M_SET_DEPTH_WRITE, V_FALSE);
	at = exec_set(push, at, M_SET_STENCIL_TEST, V_FALSE);
	at = exec_set(push, at, M_SET_LOGIC_OP, V_FALSE);
	at = exec_set(push, at, M_SET_SINGLE_CT_WRITE_CONTROL, V_FALSE);
	for (index = 1U; index < count; index++)
		select |= index << (V_CT_SELECT_TARGET_SHIFT + index * V_CT_SELECT_TARGET_BITS);
	at = exec_set(push, at, M_SET_CT_SELECT, select);

	/* Succeeded: the rendering writes the target. */
	return at;
}

/*
 * Writes viewport 0 from a VkViewport's six floats (x, y, width, height, min
 * depth, max depth, as their bits): the scale half the size, the offset its
 * corner plus that, the depth's scale its range, and the clip its rectangle and
 * depth range.
 *
 * Returns where the push buffer goes on.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_flush_vp_state).
 */
size_t
nvrtx_exec_viewport(
	uint8_t *push,
	size_t at,
	const uint32_t *viewport)
{
	int64_t x;
	int64_t y;
	int64_t width;
	int64_t height;
	int64_t near;
	int64_t far;
	int64_t ymin;
	int64_t ymax;
	int64_t clip_x;
	int64_t clip_y;
	int64_t clip_width;
	int64_t clip_height;
	uint32_t z_low;
	uint32_t z_high;

	/* The values in fixed point; the depths' range in fixed point too. */
	x = nvrtx_exec_fixed(viewport[0], EXEC_FIXED_FRACTION);
	y = nvrtx_exec_fixed(viewport[1], EXEC_FIXED_FRACTION);
	width = nvrtx_exec_fixed(viewport[2], EXEC_FIXED_FRACTION);
	height = nvrtx_exec_fixed(viewport[3], EXEC_FIXED_FRACTION);
	near = nvrtx_exec_fixed(viewport[4], 16U);
	far = nvrtx_exec_fixed(viewport[5], 16U);

	/* Scale x, y, z, then offset x, y, z. */
	at = exec_method(push, at, M_SET_VIEWPORT_SCALE_X, 6U);
	nvrtx_put32(push, at, nvrtx_exec_float_fixed(width, EXEC_FIXED_FRACTION + 1U));
	nvrtx_put32(push, at + 4U, nvrtx_exec_float_fixed(height, EXEC_FIXED_FRACTION + 1U));
	nvrtx_put32(push, at + 8U, nvrtx_exec_float_fixed(far - near, 16U));
	nvrtx_put32(push, at + 12U, nvrtx_exec_float_fixed(2 * x + width, EXEC_FIXED_FRACTION + 1U));
	nvrtx_put32(push, at + 16U, nvrtx_exec_float_fixed(2 * y + height, EXEC_FIXED_FRACTION + 1U));
	nvrtx_put32(push, at + 20U, viewport[4]);
	at += 24U;

	/* The clip: the rectangle in whole pixels (a negative height reaches up), the depths in order. */
	ymin = y;
	ymax = y + height;
	if (height < 0) {
		ymin = y + height;
		ymax = y;
	}
	clip_x = x >> EXEC_FIXED_FRACTION;
	clip_y = ymin >> EXEC_FIXED_FRACTION;
	clip_width = ((x + width) >> EXEC_FIXED_FRACTION) - clip_x;
	clip_height = (ymax >> EXEC_FIXED_FRACTION) - clip_y;
	if (clip_x < 0)
		clip_x = 0;
	if (clip_y < 0)
		clip_y = 0;
	if (clip_width < 0)
		clip_width = 0;
	if (clip_height < 0)
		clip_height = 0;
	z_low = viewport[4];
	z_high = viewport[5];
	if (far < near) {
		z_low = viewport[5];
		z_high = viewport[4];
	}
	at = exec_method(push, at, M_SET_VIEWPORT_CLIP_HORIZONTAL, 4U);
	nvrtx_put32(push, at, (uint32_t)(clip_x & 0xffffU) | ((uint32_t)(clip_width & 0xffffU) << 16));
	nvrtx_put32(push, at + 4U, (uint32_t)(clip_y & 0xffffU) | ((uint32_t)(clip_height & 0xffffU) << 16));
	nvrtx_put32(push, at + 8U, z_low);
	nvrtx_put32(push, at + 12U, z_high);
	at += 16U;

	/* Succeeded: the viewport is set. */
	return at;
}

/*
 * Writes scissor 0: enabled, from (x0, y0) to (x1, y1), the far edges
 * excluded.
 *
 * Returns where the push buffer goes on.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_flush_vp_state).
 */
size_t
nvrtx_exec_scissor(
	uint8_t *push,
	size_t at,
	uint32_t x0,
	uint32_t y0,
	uint32_t x1,
	uint32_t y1)
{
	/* Enabled, then the two ranges. */
	at = exec_method(push, at, M_SET_SCISSOR_ENABLE, 3U);
	nvrtx_put32(push, at, V_TRUE);
	nvrtx_put32(push, at + 4U, (x0 & 0xffffU) | ((x1 & 0xffffU) << 16));
	nvrtx_put32(push, at + 8U, (y0 & 0xffffU) | ((y1 & 0xffffU) << 16));
	at += 12U;

	/* Succeeded: the scissor is set. */
	return at;
}

/*
 * Writes the blend of a colour target: on or off, the separate colour
 * and alpha operations and factors, and the channels written.
 *
 * Returns where the push buffer goes on.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_flush_cb_state).
 */
size_t
nvrtx_exec_blend(
	uint8_t *push,
	size_t at,
	uint32_t target,
	const struct nvrtx_exec_blend *blend)
{
	uint32_t mask;

	/* On or off. */
	at = exec_set(push, at, M_SET_BLEND + target * M_SET_BLEND_STRIDE, blend->enable);

	/* The separate alpha, the colour's operation and factors, the alpha's. */
	at = exec_method(push, at, M_SET_BLEND_PER_TARGET_SEPARATE + target * M_SET_BLEND_PER_TARGET_STRIDE, 7U);
	nvrtx_put32(push, at, V_TRUE);
	nvrtx_put32(push, at + 4U, exec_blend_op(blend->colour_op));
	nvrtx_put32(push, at + 8U, exec_blend_factor(blend->colour_source));
	nvrtx_put32(push, at + 12U, exec_blend_factor(blend->colour_destination));
	nvrtx_put32(push, at + 16U, exec_blend_op(blend->alpha_op));
	nvrtx_put32(push, at + 20U, exec_blend_factor(blend->alpha_source));
	nvrtx_put32(push, at + 24U, exec_blend_factor(blend->alpha_destination));
	at += 28U;

	/* The channels written: red, green, blue and alpha a nibble each. */
	mask = 0U;
	if ((blend->write_mask & VK_COLOR_COMPONENT_R_BIT) != 0U)
		mask |= 1U;
	if ((blend->write_mask & VK_COLOR_COMPONENT_G_BIT) != 0U)
		mask |= 1U << 4;
	if ((blend->write_mask & VK_COLOR_COMPONENT_B_BIT) != 0U)
		mask |= 1U << 8;
	if ((blend->write_mask & VK_COLOR_COMPONENT_A_BIT) != 0U)
		mask |= 1U << 12;
	at = exec_set(push, at, M_SET_CT_WRITE + target * M_SET_CT_WRITE_STRIDE, mask);

	/* Succeeded: the blend is set. */
	return at;
}

/*
 * Writes the blend constants (four floats as their bits, red to alpha).
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_flush_cb_state).
 */
size_t
nvrtx_exec_blend_constants(
	uint8_t *push,
	size_t at,
	const uint32_t *constants)
{
	uint32_t index;

	/* The four constants in a row. */
	at = exec_method(push, at, M_SET_BLEND_CONST_RED, 4U);
	for (index = 0U; index < 4U; index++)
		nvrtx_put32(push, at + index * 4U, constants[index]);
	at += 16U;

	/* Succeeded: the constants are set. */
	return at;
}

/*
 * Writes the depth bias: on or off for points, lines and triangles alike,
 * then its constant factor (in units of the depth format's least
 * difference), its slope factor and its clamp (floats as their bits).
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_flush_rs_state, the least representable value of the format).
 */
size_t
nvrtx_exec_depth_bias(
	uint8_t *push,
	size_t at,
	const struct nvrtx_exec_depth_bias *bias)
{
	uint32_t enable;

	/* On or off, for the three kinds of primitive. */
	enable = V_FALSE;
	if (bias->enable != 0U)
		enable = V_TRUE;
	at = exec_method(push, at, M_SET_POLY_OFFSET_POINT, 3U);
	nvrtx_put32(push, at, enable);
	nvrtx_put32(push, at + 4U, enable);
	nvrtx_put32(push, at + 8U, enable);
	at += 12U;

	/* The constant factor in the depth format's units, the slope factor and the clamp. */
	at = exec_set(push, at, M_SET_DEPTH_BIAS_CONTROL, V_DEPTH_BIAS_FORMAT_DEPENDENT);
	at = exec_set(push, at, M_SET_DEPTH_BIAS, bias->constant);
	at = exec_set(push, at, M_SET_SLOPE_SCALE_DEPTH_BIAS, bias->slope);
	at = exec_set(push, at, M_SET_DEPTH_BIAS_CLAMP, bias->clamp);

	/* Succeeded: the depth bias is set. */
	return at;
}

/*
 * Writes the vertex input: each attribute from its stream at its offset
 * (the others inactive), and each stream enabled with its address,
 * stride, size and rate, or disabled.
 *
 * Returns where the push buffer goes on.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_flush_vi_state, nvk_mme_bind_vb).
 */
size_t
nvrtx_exec_vertex(
	uint8_t *push,
	size_t at,
	const struct nvrtx_exec_vertex *vertex)
{
	const struct nvrtx_exec_attribute *attribute;
	const struct nvrtx_exec_stream *stream;
	uint32_t words[NVRTX_EXEC_ATTRIBUTES];
	uint32_t index;

	/* Every attribute inactive, then each of the vertex input's at its location. */
	for (index = 0U; index < NVRTX_EXEC_ATTRIBUTES; index++)
		words[index] = V_ATTRIBUTE_INACTIVE | (V_WIDTHS_INACTIVE << V_ATTRIBUTE_WIDTHS_SHIFT) |
		    (V_ATTRIBUTE_FLOAT << V_ATTRIBUTE_TYPE_SHIFT);
	for (index = 0U; index < vertex->attribute_count; index++) {
		attribute = &vertex->attributes[index];
		if (attribute->location >= NVRTX_EXEC_ATTRIBUTES)
			continue;
		words[attribute->location] = attribute->stream |
		    (attribute->offset << V_ATTRIBUTE_OFFSET_SHIFT) |
		    exec_attribute_word(attribute->format);
	}
	at = exec_method(push, at, M_SET_VERTEX_ATTRIBUTE_A, NVRTX_EXEC_ATTRIBUTES);
	for (index = 0U; index < NVRTX_EXEC_ATTRIBUTES; index++)
		nvrtx_put32(push, at + index * 4U, words[index]);
	at += NVRTX_EXEC_ATTRIBUTES * 4U;

	/* Each stream: its rate, its format word, address and frequency, then its size. */
	for (index = 0U; index < NVRTX_EXEC_STREAMS; index++) {
		stream = &vertex->streams[index];
		if (stream->enabled == 0U) {
			at = exec_set(push, at, M_SET_VERTEX_STREAM_A_FORMAT + index * M_SET_VERTEX_STREAM_A_STRIDE, 0U);
			continue;
		}

		at = exec_set(push, at, M_SET_VERTEX_STREAM_INSTANCE_A + index * 4U, stream->instance);
		at = exec_method(push, at, M_SET_VERTEX_STREAM_A_FORMAT + index * M_SET_VERTEX_STREAM_A_STRIDE, 4U);
		nvrtx_put32(push, at, (stream->stride & 0xfffU) | V_STREAM_ENABLE);
		nvrtx_put32(push, at + 4U, (uint32_t)(stream->address >> 32));
		nvrtx_put32(push, at + 8U, (uint32_t)(stream->address & 0xffffffffU));
		nvrtx_put32(push, at + 12U, stream->instance);
		at += 16U;
		at = exec_method(push, at, M_SET_VERTEX_STREAM_SIZE_A + index * M_SET_VERTEX_STREAM_SIZE_STRIDE, 2U);
		nvrtx_put32(push, at, (uint32_t)(stream->bytes >> 32));
		nvrtx_put32(push, at + 4U, (uint32_t)(stream->bytes & 0xffffffffU));
		at += 8U;
	}

	/* Succeeded: the vertex input is set. */
	return at;
}

/*
 * Writes a draw: the bases, the instances (iterated from the first), then
 * BEGIN with the topology, the vertex range and END.
 *
 * Returns where the push buffer goes on.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_mme_build_draw).
 */
size_t
nvrtx_exec_draw(
	uint8_t *push,
	size_t at,
	uint32_t topology,
	uint32_t first,
	uint32_t count,
	uint32_t instances,
	uint32_t first_instance)
{
	uint32_t begin;

	/* The topology. */
	begin = exec_begin(topology);

	/* The vertex numbers from the first, the instances from the first instance. */
	at = exec_set(push, at, M_SET_GLOBAL_BASE_VERTEX_INDEX, 0U);
	at = exec_set(push, at, M_SET_VERTEX_ID_BASE, 0U);
	at = exec_set(push, at, M_SET_GLOBAL_BASE_INSTANCE_INDEX, first_instance);
	at = exec_set(push, at, M_SET_INSTANCE_COUNT, instances);

	/* The primitives. */
	at = exec_set(push, at, M_BEGIN, begin);
	at = exec_method(push, at, M_SET_VERTEX_ARRAY_START, 2U);
	nvrtx_put32(push, at, first);
	nvrtx_put32(push, at + 4U, count);
	at += 8U;
	at = exec_set(push, at, M_END, 0U);

	/* Succeeded: the draw is written. */
	return at;
}

/*
 * Writes an indexed draw: the vertex offset as the base of the vertex
 * numbers and of the vertex index, the instances (iterated from the
 * first), then BEGIN with the topology, the index range (the count
 * starts it) and END.  Primitive restart is off.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_mme_build_set_draw_params, nvk_mme_build_draw_indexed_loop).
 */
size_t
nvrtx_exec_draw_indexed(
	uint8_t *push,
	size_t at,
	uint32_t topology,
	uint32_t first,
	uint32_t count,
	uint32_t instances,
	uint32_t first_instance,
	int32_t vertex_offset)
{
	uint32_t begin;

	/* The topology. */
	begin = exec_begin(topology);

	/* The vertex offset, the instances from the first instance, no restart. */
	at = exec_set(push, at, M_SET_GLOBAL_BASE_VERTEX_INDEX, (uint32_t)vertex_offset);
	at = exec_set(push, at, M_SET_VERTEX_ID_BASE, (uint32_t)vertex_offset);
	at = exec_set(push, at, M_SET_GLOBAL_BASE_INSTANCE_INDEX, first_instance);
	at = exec_set(push, at, M_SET_INSTANCE_COUNT, instances);
	at = exec_set(push, at, M_SET_DA_PRIMITIVE_RESTART, V_FALSE);

	/* The primitives: the first index, then the count, which draws. */
	at = exec_set(push, at, M_BEGIN, begin);
	at = exec_method(push, at, M_SET_INDEX_BUFFER_F, 2U);
	nvrtx_put32(push, at, first);
	nvrtx_put32(push, at + 4U, count);
	at += 8U;
	at = exec_set(push, at, M_END, 0U);

	/* Succeeded: the draw is written. */
	return at;
}

/*
 * Writes the index buffer: its address, the bytes readable from there and
 * the size of an index (a VkIndexType of 16 or 32 bits).
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_mme_bind_ib).
 */
size_t
nvrtx_exec_index_buffer(
	uint8_t *push,
	size_t at,
	uint64_t address,
	uint64_t bytes,
	uint32_t type)
{
	uint32_t size;

	/* The address, then the size. */
	at = exec_method(push, at, M_SET_INDEX_BUFFER_A, 2U);
	nvrtx_put32(push, at, (uint32_t)(address >> 32));
	nvrtx_put32(push, at + 4U, (uint32_t)(address & 0xffffffffU));
	at += 8U;
	at = exec_method(push, at, M_SET_INDEX_BUFFER_SIZE_A, 2U);
	nvrtx_put32(push, at, (uint32_t)(bytes >> 32));
	nvrtx_put32(push, at + 4U, (uint32_t)(bytes & 0xffffffffU));
	at += 8U;

	/* The index size. */
	size = V_INDEX_SIZE_FOUR_BYTES;
	if (type == VK_INDEX_TYPE_UINT16)
		size = V_INDEX_SIZE_TWO_BYTES;
	at = exec_set(push, at, M_SET_INDEX_BUFFER_E, size);

	/* Succeeded: the index buffer is set. */
	return at;
}

/*
 * Writes the culling (a VkCullModeFlags), the front face (a VkFrontFace),
 * where a point's size comes from: the last program before the
 * rasterizer when it writes one (point_size nonzero), else the fixed size
 * (one pixel, the queue's start state); whether primitives reach the
 * rasterizer at all (discard nonzero: the programs before it run, their
 * stores land, nothing is drawn); how both faces of a polygon are drawn
 * (a VkPolygonMode: filled, as lines or as points); and whether the depth
 * is clamped to the viewport's range (depth_clamp nonzero) rather than
 * the primitives clipped to it.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_flush_rs_state); the point size's source as Mesa 25.0.7
 * src/gallium/drivers/nouveau/nvc0/nvc0_state.c (nvc0_rasterizer_state_create:
 * the attribute's size only when the program gives it).
 */
size_t
nvrtx_exec_raster(
	uint8_t *push,
	size_t at,
	uint32_t cull_mode,
	uint32_t front_face,
	uint32_t point_size,
	uint32_t discard,
	uint32_t polygon_mode,
	uint32_t depth_clamp)
{
	uint32_t culled;
	uint32_t face;
	uint32_t mode;

	/* Rasterized, or discarded before the rasterizer. */
	at = exec_set(push, at, M_SET_RASTER_ENABLE, discard != 0U ? V_FALSE : V_TRUE);

	/* Both faces filled, outlined or as their vertices. */
	mode = V_POLYGON_FILL;
	if (polygon_mode <= VK_POLYGON_MODE_POINT)
		mode = V_POLYGON_FILL - polygon_mode;
	at = exec_method(push, at, M_SET_FRONT_POLYGON_MODE, 2U);
	nvrtx_put32(push, at, mode);
	nvrtx_put32(push, at + 4U, mode);
	at += 8U;

	/* The depth clipped to the viewport's range, or clamped to it. */
	at = exec_set(push, at, M_SET_VIEWPORT_CLIP_CONTROL, depth_clamp != 0U ? V_VIEWPORT_CLIP_CLAMP : V_VIEWPORT_CLIP_DRAW);

	/* Culled or not, and which faces. */
	culled = V_FALSE;
	if (cull_mode != VK_CULL_MODE_NONE)
		culled = V_TRUE;
	at = exec_set(push, at, M_OGL_SET_CULL, culled);
	if (cull_mode != VK_CULL_MODE_NONE) {
		face = V_CULL_FACE_FRONT_AND_BACK;
		if (cull_mode == VK_CULL_MODE_FRONT_BIT)
			face = V_CULL_FACE_FRONT;
		else if (cull_mode == VK_CULL_MODE_BACK_BIT)
			face = V_CULL_FACE_BACK;
		at = exec_set(push, at, M_OGL_SET_CULL_FACE, face);
	}

	/* The front face. */
	face = V_FRONT_FACE_CCW;
	if (front_face == VK_FRONT_FACE_CLOCKWISE)
		face = V_FRONT_FACE_CW;
	at = exec_set(push, at, M_OGL_SET_FRONT_FACE, face);

	/* The point size: the program's, or the fixed one. */
	at = exec_set(push, at, M_SET_ATTRIBUTE_POINT_SIZE, point_size != 0U ? V_TRUE : V_FALSE);

	/* Succeeded: the culling is set. */
	return at;
}

/*
 * Writes the logic operation of the colour targets: off (enable 0), or on
 * with an operation (a VkLogicOp), which then replaces the blend.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_flush_cb_state, vk_to_nv9097_logic_op).
 */
size_t
nvrtx_exec_logic_op(
	uint8_t *push,
	size_t at,
	uint32_t enable,
	uint32_t op)
{
	/* Off (an operation past SET too): nothing more. */
	if (op > VK_LOGIC_OP_SET)
		enable = 0U;
	if (enable == 0U) {
		at = exec_set(push, at, M_SET_LOGIC_OP, V_FALSE);
		return at;
	}

	/* On, and its operation. */
	at = exec_set(push, at, M_SET_LOGIC_OP, V_TRUE);
	at = exec_set(push, at, M_SET_LOGIC_OP_FUNC, V_LOGIC_OP_CLEAR + op);

	/* Succeeded: the logic operation is written. */
	return at;
}

/*
 * Writes where a primitive's layer of a layered target comes from: the
 * last program before the rasterizer when it writes the layer
 * (from_program nonzero, gl_Layer), else layer 0 of the target.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_shader.c
 * (nvk_shader_fill_push: SET_RT_LAYER's control
 * GEOMETRY_SHADER_SELECTS_LAYER when the shader writes the layer, else
 * V_SELECTS_LAYER with V 0).
 */
size_t
nvrtx_exec_layer_source(
	uint8_t *push,
	size_t at,
	uint32_t from_program)
{
	/* The program's layer, or layer 0. */
	at = exec_set(push, at, M_SET_RT_LAYER, from_program != 0U ? V_RT_LAYER_FROM_PROGRAM : 0U);

	/* Succeeded: the layer's source is written. */
	return at;
}

/*
 * Writes whether the depth and stencil tests run before the fragment
 * program: the program's EarlyFragmentTests mode.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_shader.c (a
 * fragment shader's state sets SET_API_MANDATED_EARLY_Z to its
 * early_fragment_tests).
 */
size_t
nvrtx_exec_early_tests(
	uint8_t *push,
	size_t at,
	uint32_t early)
{
	/* Before the program, or after it. */
	at = exec_set(push, at, M_SET_API_MANDATED_EARLY_Z, early != 0U ? 1U : 0U);

	/* Succeeded: the tests' place is written. */
	return at;
}

/*
 * Writes the layer of a layered target the primitives after it go to,
 * whatever the programs write: the view of a multiview draw.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_draw.c
 * (SET_RT_LAYER with V the view's index and control V_SELECTS_LAYER for
 * each view of the render's view mask).
 */
size_t
nvrtx_exec_layer(
	uint8_t *push,
	size_t at,
	uint32_t layer)
{
	/* The layer, the method's own value (control 0). */
	at = exec_set(push, at, M_SET_RT_LAYER, layer & 0xffffU);

	/* Succeeded: the layer is written. */
	return at;
}

/*
 * Writes the samples a pixel keeps (VkPipelineMultisampleStateCreateInfo's
 * pSampleMask, low 16 bits): the same mask for each pixel of the 2x2 quad.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_flush_ms_state: the four SET_SAMPLE_MASK methods of sample_mask &
 * 0xffff).
 */
size_t
nvrtx_exec_sample_mask(
	uint8_t *push,
	size_t at,
	uint32_t mask)
{
	uint32_t index;

	/* The four pixels of the quad. */
	at = exec_method(push, at, M_SET_SAMPLE_MASK_X0_Y0, EXEC_SAMPLE_MASK_PIXELS);
	for (index = 0U; index < EXEC_SAMPLE_MASK_PIXELS; index++)
		nvrtx_put32(push, at + index * 4U, mask & 0xffffU);
	at += EXEC_SAMPLE_MASK_PIXELS * 4U;

	/* Succeeded: the mask is written. */
	return at;
}

/* Writes whether a fragment's alpha decides its coverage of the samples. */
size_t
nvrtx_exec_alpha_coverage(
	uint8_t *push,
	size_t at,
	uint32_t enable)
{
	/* On or off. */
	at = exec_set(push, at, M_SET_ANTI_ALIAS_ALPHA_CONTROL, enable != 0U ? V_ALPHA_TO_COVERAGE : 0U);

	/* Succeeded: the control is written. */
	return at;
}

/*
 * Writes the depth test: on or off, the writes, and the function (a
 * VkCompareOp: the OGL values follow its order); then the stencil test:
 * on or off, and when on both faces' operations, functions, masks and
 * references, two-sided.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_flush_ds_state).
 */
size_t
nvrtx_exec_depth(
	uint8_t *push,
	size_t at,
	const struct nvrtx_exec_depth *depth)
{
	uint32_t compare;
	uint32_t test;
	uint32_t write;

	/* The function: NEVER to ALWAYS in order. */
	compare = depth->compare;
	if (compare > VK_COMPARE_OP_ALWAYS)
		compare = VK_COMPARE_OP_ALWAYS;

	/* The test, the writes (only with the test) and the function. */
	test = V_FALSE;
	write = V_FALSE;
	if (depth->test != 0U) {
		test = V_TRUE;
		if (depth->write != 0U)
			write = V_TRUE;
	}
	at = exec_set(push, at, M_SET_DEPTH_TEST, test);
	at = exec_set(push, at, M_SET_DEPTH_WRITE, write);
	if (depth->test != 0U)
		at = exec_set(push, at, M_SET_DEPTH_FUNC, V_DEPTH_FUNC_NEVER + compare);

	/* The stencil test, off unless asked; when on, the front face's state, then the back face's. */
	if (depth->stencil == 0U) {
		at = exec_set(push, at, M_SET_STENCIL_TEST, V_FALSE);
		return at;
	}
	at = exec_set(push, at, M_SET_STENCIL_TEST, V_TRUE);
	at = exec_set(push, at, M_SET_TWO_SIDED_STENCIL_TEST, V_TRUE);

	/* The front face: its operations and function, then its reference, compare mask and write mask. */
	at = exec_method(push, at, M_SET_STENCIL_OP_FAIL, 4U);
	nvrtx_put32(push, at, exec_stencil_op(depth->front.fail));
	nvrtx_put32(push, at + 4U, exec_stencil_op(depth->front.depth_fail));
	nvrtx_put32(push, at + 8U, exec_stencil_op(depth->front.pass));
	nvrtx_put32(push, at + 12U, exec_compare_op(depth->front.compare));
	at += 16U;
	at = exec_method(push, at, M_SET_STENCIL_FUNC_REF, 3U);
	nvrtx_put32(push, at, depth->front.reference & 0xffU);
	nvrtx_put32(push, at + 4U, depth->front.compare_mask & 0xffU);
	nvrtx_put32(push, at + 8U, depth->front.write_mask & 0xffU);
	at += 12U;

	/* The back face the same way (its reference, write mask and compare mask in that order). */
	at = exec_method(push, at, M_SET_BACK_STENCIL_OP_FAIL, 4U);
	nvrtx_put32(push, at, exec_stencil_op(depth->back.fail));
	nvrtx_put32(push, at + 4U, exec_stencil_op(depth->back.depth_fail));
	nvrtx_put32(push, at + 8U, exec_stencil_op(depth->back.pass));
	nvrtx_put32(push, at + 12U, exec_compare_op(depth->back.compare));
	at += 16U;
	at = exec_method(push, at, M_SET_BACK_STENCIL_FUNC_REF, 3U);
	nvrtx_put32(push, at, depth->back.reference & 0xffU);
	nvrtx_put32(push, at + 4U, depth->back.write_mask & 0xffU);
	nvrtx_put32(push, at + 8U, depth->back.compare_mask & 0xffU);
	at += 12U;

	/* Succeeded: the depth and stencil tests are set. */
	return at;
}

/* The class's value of a VkStencilOp (D3D_KEEP for one out of range). */
static uint32_t
exec_stencil_op(
	uint32_t op)
{
	/* KEEP to DECREMENT_AND_WRAP in order, one more than Vulkan's. */
	if (op > VK_STENCIL_OP_DECREMENT_AND_WRAP)
		op = VK_STENCIL_OP_KEEP;

	/* Succeeded: the D3D value. */
	return op + 1U;
}

/* The class's value of a VkCompareOp (ALWAYS for one out of range). */
static uint32_t
exec_compare_op(
	uint32_t op)
{
	/* NEVER to ALWAYS in order. */
	if (op > VK_COMPARE_OP_ALWAYS)
		op = VK_COMPARE_OP_ALWAYS;

	/* Succeeded: the OGL value. */
	return V_DEPTH_FUNC_NEVER + op;
}

/*
 * Writes a uniform buffer: the constant buffer of a size at an address,
 * bound to a slot of a program group (0 the vertex program's, 4 the
 * fragment program's).
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_cmd_flush_gfx_cbufs, nvk_mme_bind_cbuf_desc) and
 * nvk_shader.h (nvk_cbuf_binding_for_stage: the group is the stage).
 */
size_t
nvrtx_exec_uniform(
	uint8_t *push,
	size_t at,
	uint32_t group,
	uint32_t slot,
	uint64_t address,
	uint32_t size)
{
	/* The buffer: its size, then its address. */
	at = exec_method(push, at, M_SET_CONSTANT_BUFFER_SELECTOR_A, 3U);
	nvrtx_put32(push, at, size);
	nvrtx_put32(push, at + 4U, (uint32_t)(address >> 32));
	nvrtx_put32(push, at + 8U, (uint32_t)(address & 0xffffffffU));
	at += 12U;

	/* The group's slot. */
	at = exec_set(push, at, M_BIND_GROUP_CONSTANT_BUFFER + group * M_BIND_GROUP_STRIDE, V_GROUP_VALID | (slot << V_GROUP_SLOT_SHIFT));

	/* Succeeded: the group reads the buffer at the slot. */
	return at;
}

/*
 * Writes the selection of a constant buffer for the loads after it (its
 * size, then its address), binding it to no group.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_mme_select_cb0).
 */
size_t
nvrtx_exec_constants_select(
	uint8_t *push,
	size_t at,
	uint64_t address,
	uint32_t size)
{
	/* The size, then the address. */
	at = exec_method(push, at, M_SET_CONSTANT_BUFFER_SELECTOR_A, 3U);
	nvrtx_put32(push, at, size);
	nvrtx_put32(push, at + 4U, (uint32_t)(address >> 32));
	nvrtx_put32(push, at + 8U, (uint32_t)(address & 0xffffffffU));
	at += 12U;

	/* Succeeded: the loads after it go to the buffer. */
	return at;
}

/*
 * Writes count words into the selected constant buffer from the byte
 * offset on.  The loads are in order with the draws: a draw before them
 * reads the buffer as it was, a draw after them as written.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_cmd_flush_gfx_root_desc, nvk_push_draw_state_init).
 */
size_t
nvrtx_exec_constants_load(
	uint8_t *push,
	size_t at,
	uint32_t offset,
	const uint32_t *words,
	uint32_t count)
{
	uint32_t index;

	/* The offset, then the words, each to the load method. */
	nvrtx_put32(push, at, EXEC_METHOD_ONE_INCREMENT | ((count + 1U) << 16) | (M_LOAD_CONSTANT_BUFFER_OFFSET >> 2));
	nvrtx_put32(push, at + 4U, offset);
	at += 8U;
	for (index = 0U; index < count; index++)
		nvrtx_put32(push, at + index * 4U, words[index]);
	at += count * 4U;

	/* Succeeded: the words are in the buffer for the draws after them. */
	return at;
}

/*
 * Writes a draw (indexed or not) as nvrtx_exec_draw() and
 * nvrtx_exec_draw_indexed() do, but only the bases and counts that differ
 * from what held says the GPU has (everything when known is false); held
 * is then what the GPU has after it.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_mme_build_set_draw_params, nvk_mme_build_draw).
 */
size_t
nvrtx_exec_draw_held(
	uint8_t *push,
	size_t at,
	struct nvrtx_exec_bases *held,
	bool known,
	const struct nvrtx_exec_draw_args *draw)
{
	uint32_t vertex;
	uint32_t begin;

	/* The topology. */
	begin = exec_begin(draw->topology);

	/* The vertex numbers from the vertex offset of an indexed draw, from 0 for another. */
	vertex = 0U;
	if (draw->indexed)
		vertex = (uint32_t)draw->vertex_offset;
	if (!known || held->vertex != vertex) {
		at = exec_set(push, at, M_SET_GLOBAL_BASE_VERTEX_INDEX, vertex);
		at = exec_set(push, at, M_SET_VERTEX_ID_BASE, vertex);
		held->vertex = vertex;
	}

	/* The instances from the first instance. */
	if (!known || held->instance != draw->first_instance) {
		at = exec_set(push, at, M_SET_GLOBAL_BASE_INSTANCE_INDEX, draw->first_instance);
		held->instance = draw->first_instance;
	}
	if (!known || held->instances != draw->instances) {
		at = exec_set(push, at, M_SET_INSTANCE_COUNT, draw->instances);
		held->instances = draw->instances;
	}

	/* An indexed draw: no restart (written once), the first index, then the count, which draws. */
	if (!known)
		held->restart_off = false;
	if (draw->indexed) {
		if (!held->restart_off) {
			at = exec_set(push, at, M_SET_DA_PRIMITIVE_RESTART, V_FALSE);
			held->restart_off = true;
		}
		at = exec_set(push, at, M_BEGIN, begin);
		at = exec_method(push, at, M_SET_INDEX_BUFFER_F, 2U);
		nvrtx_put32(push, at, draw->first);
		nvrtx_put32(push, at + 4U, draw->count);
		at += 8U;
		at = exec_set(push, at, M_END, 0U);
		return at;
	}

	/* Another draw: the vertex range. */
	at = exec_set(push, at, M_BEGIN, begin);
	at = exec_method(push, at, M_SET_VERTEX_ARRAY_START, 2U);
	nvrtx_put32(push, at, draw->first);
	nvrtx_put32(push, at + 4U, draw->count);
	at += 8U;
	at = exec_set(push, at, M_END, 0U);

	/* Succeeded: the draw is written. */
	return at;
}

/*
 * Writes a clear of a colour target's layer over a rectangle (the far
 * edges excluded) to a colour (four floats as their bits, or the
 * integers), all four channels, whatever the render enable says.  The
 * target's VkFormat says how the colour is given: a normalized channel's
 * float is first quantized to the channel's step, rounded to the nearest
 * (the conversion Vulkan gives a float to a normalized fixed-point
 * value), since the class's own conversion of a clear value rounds a
 * value halfway between two steps down (0.5 to 0x7f of 8 bits); a signed
 * one's magnitude so, within -1 and 1, its sign kept; an sRGB channel's
 * linear float to the linear value of the step its encoding rounds to
 * (the class encodes that value back to the step), its alpha as an 8-bit
 * normalized one.
 *
 * Returns where the push buffer goes on.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_cmd_clear.c
 * (nvk_CmdClearAttachments: CLEAR_SURFACE's RT_ARRAY_INDEX, bits 25:10,
 * the layer).  The Vulkan 1.3 specification, "Conversion from
 * Floating-Point to Normalized Fixed-Point" (round to nearest).
 */
size_t
nvrtx_exec_clear(
	uint8_t *push,
	size_t at,
	uint32_t target,
	uint32_t format,
	uint32_t layer,
	const uint32_t *colour,
	uint32_t x0,
	uint32_t y0,
	uint32_t x1,
	uint32_t y1)
{
	uint32_t index;
	uint32_t width;
	uint32_t value;
	bool srgb;

	/* Rendered always, by the clear rectangle. */
	at = exec_set(push, at, M_SET_RENDER_ENABLE_OVERRIDE, V_RENDER_ALWAYS);
	at = exec_set(push, at, M_SET_CLEAR_SURFACE_CONTROL, V_CLEAR_USE_RECT);

	/* The colour, each normalized or sRGB channel quantized to its step. */
	at = exec_method(push, at, M_SET_COLOR_CLEAR_VALUE, 4U);
	for (index = 0U; index < 4U; index++) {
		value = colour[index];
		srgb = exec_srgb_channel(format, index);
		if (srgb)
			value = exec_srgb_quantize(value);
		width = exec_unorm_bits(format, index);
		if (width != 0U)
			value = exec_unorm_quantize(value, width);
		width = exec_snorm_bits(format);
		if (width != 0U)
			value = (value & FLOAT_SIGN) | exec_unorm_quantize(value & ~FLOAT_SIGN, width);
		nvrtx_put32(push, at + index * 4U, value);
	}
	at += 16U;

	/* The rectangle, then the clear. */
	at = exec_method(push, at, M_SET_CLEAR_RECT_HORIZONTAL, 2U);
	nvrtx_put32(push, at, (x0 & 0xffffU) | ((x1 & 0xffffU) << 16));
	nvrtx_put32(push, at + 4U, (y0 & 0xffffU) | ((y1 & 0xffffU) << 16));
	at += 8U;
	at = exec_set(push, at, M_CLEAR_SURFACE, V_CLEAR_RGBA | (target << V_CLEAR_TARGET_SHIFT) | (layer << V_CLEAR_LAYER_SHIFT));

	/* Back to the render enable. */
	at = exec_set(push, at, M_SET_RENDER_ENABLE_OVERRIDE, V_RENDER_USE_ENABLE);

	/* Succeeded: the rectangle is cleared. */
	return at;
}

/*
 * Writes a clear of the depth target's layer over a rectangle (the far
 * edges excluded) to a depth (a float as its bits), whatever the render
 * enable says.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_clear.c
 * (nvk_CmdClearAttachments).
 */
size_t
nvrtx_exec_clear_depth(
	uint8_t *push,
	size_t at,
	uint32_t layer,
	uint32_t depth,
	uint32_t x0,
	uint32_t y0,
	uint32_t x1,
	uint32_t y1)
{
	/* Rendered always, by the clear rectangle. */
	at = exec_set(push, at, M_SET_RENDER_ENABLE_OVERRIDE, V_RENDER_ALWAYS);
	at = exec_set(push, at, M_SET_CLEAR_SURFACE_CONTROL, V_CLEAR_USE_RECT);

	/* The depth, the rectangle, then the clear of depth only. */
	at = exec_set(push, at, M_SET_Z_CLEAR_VALUE, depth);
	at = exec_method(push, at, M_SET_CLEAR_RECT_HORIZONTAL, 2U);
	nvrtx_put32(push, at, (x0 & 0xffffU) | ((x1 & 0xffffU) << 16));
	nvrtx_put32(push, at + 4U, (y0 & 0xffffU) | ((y1 & 0xffffU) << 16));
	at += 8U;
	at = exec_set(push, at, M_CLEAR_SURFACE, V_CLEAR_Z | (layer << V_CLEAR_LAYER_SHIFT));

	/* Back to the render enable. */
	at = exec_set(push, at, M_SET_RENDER_ENABLE_OVERRIDE, V_RENDER_USE_ENABLE);

	/* Succeeded: the rectangle is cleared. */
	return at;
}

/*
 * Writes a clear of the depth target's stencil of a layer over a rectangle
 * (the far edges excluded) to a value (its low 8 bits), whatever the
 * render enable says.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_clear.c
 * (nvk_CmdClearAttachments).
 */
size_t
nvrtx_exec_clear_stencil(
	uint8_t *push,
	size_t at,
	uint32_t layer,
	uint32_t stencil,
	uint32_t x0,
	uint32_t y0,
	uint32_t x1,
	uint32_t y1)
{
	/* Rendered always, by the clear rectangle. */
	at = exec_set(push, at, M_SET_RENDER_ENABLE_OVERRIDE, V_RENDER_ALWAYS);
	at = exec_set(push, at, M_SET_CLEAR_SURFACE_CONTROL, V_CLEAR_USE_RECT);

	/* The value, the rectangle, then the clear of the stencil only. */
	at = exec_set(push, at, M_SET_STENCIL_CLEAR_VALUE, stencil & 0xffU);
	at = exec_method(push, at, M_SET_CLEAR_RECT_HORIZONTAL, 2U);
	nvrtx_put32(push, at, (x0 & 0xffffU) | ((x1 & 0xffffU) << 16));
	nvrtx_put32(push, at + 4U, (y0 & 0xffffU) | ((y1 & 0xffffU) << 16));
	at += 8U;
	at = exec_set(push, at, M_CLEAR_SURFACE, V_CLEAR_STENCIL | (layer << V_CLEAR_LAYER_SHIFT));

	/* Back to the render enable. */
	at = exec_set(push, at, M_SET_RENDER_ENABLE_OVERRIDE, V_RENDER_USE_ENABLE);

	/* Succeeded: the rectangle's stencil is cleared. */
	return at;
}

/*
 * Writes the geometry program's slot of the pipeline: the program (its
 * header's address and its registers, its constant buffers the geometry
 * group's), or the slot off when address is 0.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_shader.c
 * (nvk_shader_fill_push) and src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_cmd_flush_shaders: a stage without a shader disabled).
 */
size_t
nvrtx_exec_geometry_program(
	uint8_t *push,
	size_t at,
	uint64_t address,
	uint32_t gprs)
{
	/* No geometry program: the slot off, of its type. */
	if (address == 0U) {
		at = exec_set(push, at, M_SET_PIPELINE_SHADER_GEOMETRY, V_PIPELINE_TYPE_GEOMETRY << 4);
		return at;
	}

	/* Enabled, of its type. */
	at = exec_set(push, at, M_SET_PIPELINE_SHADER_GEOMETRY, V_TRUE | (V_PIPELINE_TYPE_GEOMETRY << 4));

	/* The address of its header. */
	at = exec_method(push, at, M_SET_PIPELINE_PROGRAM_ADDRESS_A_GEOMETRY, 2U);
	nvrtx_put32(push, at, (uint32_t)(address >> 32));
	nvrtx_put32(push, at + 4U, (uint32_t)(address & 0xffffffffU));
	at += 8U;

	/* Its registers and the group whose constant buffers it reads. */
	at = exec_method(push, at, M_SET_PIPELINE_REGISTER_COUNT_GEOMETRY, 2U);
	nvrtx_put32(push, at, gprs);
	nvrtx_put32(push, at + 4U, V_PIPELINE_GROUP_GEOMETRY);
	at += 8U;

	/* Succeeded: the geometry program is bound. */
	return at;
}

/*
 * Writes the barrier between work that wrote an image and work that reads
 * it: the 3D class waits for idle, then the texture data and the constant
 * caches are invalidated.
 *
 * Returns where the push buffer goes on.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_cmd_buffer.c
 * (nvk_cmd_flush_wait_dep, nvk_cmd_invalidate_deps).
 */
size_t
nvrtx_exec_barrier(
	uint8_t *push,
	size_t at)
{
	/* Everything before has finished. */
	at = exec_set(push, at, M_WAIT_FOR_IDLE, 0U);

	/* Nothing after reads a stale texel or constant. */
	at = exec_set(push, at, M_INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0U);
	at = exec_set(push, at, M_INVALIDATE_SHADER_CACHES_NO_WFI, V_INVALIDATE_CONSTANTS);

	/* Succeeded: the barrier is written. */
	return at;
}

/*
 * The report semaphore: its address, its payload and its operation word
 * (a release of one word after the writes before it, or a report of four
 * words, a 64-bit value and a 64-bit timestamp, at a pipeline location),
 * the reports it takes (the passing samples' 64-bit count), the zpass
 * count's enable and the flush of the writes pending.
 *
 * Values referenced: Mesa 25.0.7 src/nouveau/headers/nvidia/classes/clc597.h
 * (NVC597_SET_REPORT_SEMAPHORE_A to _D, its OPERATION 1:0 RELEASE and
 * REPORT_ONLY, FLUSH_DISABLE 2:2, RELEASE 4:4 AFTER_ALL_PRECEEDING_WRITES,
 * PIPELINE_LOCATION 15:12, REPORT 27:23 ZPASS_PIXEL_CNT64, STRUCTURE_SIZE
 * 28:28 ONE_WORD; NVC597_SET_ZPASS_PIXEL_COUNT, NVC597_FLUSH_PENDING_WRITES).
 */
#define M_FLUSH_PENDING_WRITES			0x1144U
#define M_SET_ZPASS_PIXEL_COUNT			0x1514U
#define M_SET_REPORT_SEMAPHORE_A		0x1b00U
#define V_REPORT_RELEASE			0U
#define V_REPORT_ONLY				2U
#define V_REPORT_FLUSH_DISABLE			(1U << 2)
#define V_REPORT_AFTER_WRITES			(1U << 4)
#define V_REPORT_LOCATION_SHIFT			12U
#define V_REPORT_ZPASS_PIXEL_CNT64		(0x15U << 23)
#define V_REPORT_ONE_WORD			(1U << 28)
#define V_LOCATION_NONE				0x0U
#define V_LOCATION_DATA_ASSEMBLER		0x1U
#define V_LOCATION_VERTEX_SHADER		0x2U
#define V_LOCATION_GEOMETRY_SHADER		0x6U
#define V_LOCATION_ZCULL			0x7U
#define V_LOCATION_TESSELLATION_INIT		0x8U
#define V_LOCATION_TESSELLATION			0x9U
#define V_LOCATION_PIXEL_SHADER			0xaU
#define V_LOCATION_DEPTH_TEST			0xcU
#define V_LOCATION_ALL				0xfU

static size_t exec_report(uint8_t *push, size_t at, uint64_t va, uint32_t payload, uint32_t operation);
static uint32_t exec_location(uint32_t stages);

/*
 * Writes an occlusion query's begin or end: the count of samples passing
 * on at its begin and off at its end, then the count reported (four words
 * at the report's address: the 64-bit count and a timestamp).  A query's
 * result is its end's count less its begin's.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_query_pool.c
 * (nvk_cmd_begin_end_query, VK_QUERY_TYPE_OCCLUSION).
 */
size_t
nvrtx_exec_query_occlusion(
	uint8_t *push,
	size_t at,
	uint64_t report,
	bool begin)
{
	uint32_t operation;

	/* The count on from the begin, off after the end. */
	at = exec_set(push, at, M_SET_ZPASS_PIXEL_COUNT, begin ? 1U : 0U);

	/* The 64-bit count of passing samples, every stage done. */
	operation = V_REPORT_ONLY | V_REPORT_FLUSH_DISABLE | (V_LOCATION_ALL << V_REPORT_LOCATION_SHIFT) | V_REPORT_ZPASS_PIXEL_CNT64;
	at = exec_report(push, at, report, 0U, operation);

	/* Succeeded: the count is reported. */
	return at;
}

/*
 * Writes a timestamp query's report: four words at the report's address
 * (a value and the 64-bit timestamp in nanoseconds) once the stages a
 * VkPipelineStageFlags names have done the work before.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_query_pool.c
 * (nvk_CmdWriteTimestamp2) and nvk_event.c
 * (vk_stage_flags_to_nv9097_pipeline_location).
 */
size_t
nvrtx_exec_query_timestamp(
	uint8_t *push,
	size_t at,
	uint64_t report,
	uint32_t stages)
{
	uint32_t operation;

	/* A report alone at the stages' location. */
	operation = V_REPORT_ONLY | (exec_location(stages) << V_REPORT_LOCATION_SHIFT);
	at = exec_report(push, at, report, 0U, operation);

	/* Succeeded: the timestamp is reported. */
	return at;
}

/*
 * Writes a query's availability word (1 available, 0 not) once the writes
 * before it are done; the end of a query flushes its pending report first.
 *
 * Returns where the push buffer goes on.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_query_pool.c
 * (nvk_cmd_begin_end_query's end, nvk_CmdResetQueryPool).
 */
size_t
nvrtx_exec_query_available(
	uint8_t *push,
	size_t at,
	uint64_t available,
	uint32_t value,
	bool flush)
{
	uint32_t operation;

	/* The pending report written out first. */
	if (flush)
		at = exec_set(push, at, M_FLUSH_PENDING_WRITES, 0U);

	/* One word released after every write before it, every stage done. */
	operation = V_REPORT_RELEASE | V_REPORT_AFTER_WRITES | (V_LOCATION_ALL << V_REPORT_LOCATION_SHIFT) | V_REPORT_ONE_WORD;
	at = exec_report(push, at, available, value, operation);

	/* Succeeded: the word is written. */
	return at;
}

/* Writes the report semaphore: its address, its payload and its operation. */
static size_t
exec_report(
	uint8_t *push,
	size_t at,
	uint64_t va,
	uint32_t payload,
	uint32_t operation)
{
	/* The four words at once. */
	at = exec_method(push, at, M_SET_REPORT_SEMAPHORE_A, 4U);
	nvrtx_put32(push, at, (uint32_t)(va >> 32));
	nvrtx_put32(push, at + 4U, (uint32_t)(va & 0xffffffffU));
	nvrtx_put32(push, at + 8U, payload);
	nvrtx_put32(push, at + 12U, operation);

	/* Succeeded: the semaphore is written. */
	return at + 16U;
}

/*
 * Reports the pipeline location a report waits for after the stages a
 * VkPipelineStageFlags names: the last of them in the pipeline's order
 * (the end of all for the output, transfer and bottom stages, none for
 * the top, indirect, host and compute stages alone).
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/vulkan/nvk_event.c
 * (vk_stage_flags_to_nv9097_pipeline_location).
 */
static uint32_t
exec_location(
	uint32_t stages)
{
	/* Every stage done for the output, the transfers and the bottom. */
	if ((stages & (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT |
		       VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)) != 0U)
		return V_LOCATION_ALL;

	/* The fragment stages, then the geometry, tessellation and vertex stages, then the input. */
	if ((stages & VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT) != 0U)
		return V_LOCATION_DEPTH_TEST;
	if ((stages & VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT) != 0U)
		return V_LOCATION_PIXEL_SHADER;
	if ((stages & VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT) != 0U)
		return V_LOCATION_ZCULL;
	if ((stages & VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT) != 0U)
		return V_LOCATION_GEOMETRY_SHADER;
	if ((stages & VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT) != 0U)
		return V_LOCATION_TESSELLATION;
	if ((stages & VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT) != 0U)
		return V_LOCATION_TESSELLATION_INIT;
	if ((stages & VK_PIPELINE_STAGE_VERTEX_SHADER_BIT) != 0U)
		return V_LOCATION_VERTEX_SHADER;
	if ((stages & VK_PIPELINE_STAGE_VERTEX_INPUT_BIT) != 0U)
		return V_LOCATION_DATA_ASSEMBLER;

	/* Succeeded: the top of the pipe, the indirect, host and compute stages wait for nothing on the 3D engine. */
	return V_LOCATION_NONE;
}

/*
 * Writes the texture header of a pitch-linear 2D image in one of the
 * executor's colour formats (another reads as R8G8B8A8_UNORM) or its
 * depth formats (the depth read as red): the header's component sizes
 * and data types (a depth format's stencil its own type), and the sources
 * that move the image's channels and a view's swizzle into place (one an
 * integer one for an integer format).  An sRGB format is decoded.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/nil/descriptor.rs
 * (nvb097_fill_image_view_desc) and Mesa 25.0.7 src/nouveau/nil/tic.rs
 * (nvb097_set_th_bl_0: the data type of each component, a missing one the
 * first's; the integer one for a pure integer format).
 */
void
nvrtx_exec_texture_header(
	uint32_t *header,
	uint64_t address,
	uint32_t width,
	uint32_t height,
	uint32_t pitch,
	uint32_t format,
	const uint32_t *swizzle)
{
	static const uint32_t depth_identity[4] = { TIC_SOURCE_R, TIC_SOURCE_ZERO, TIC_SOURCE_ZERO, TIC_SOURCE_ONE_FLOAT };
	const struct nvrtx_exec_format *entry;
	const struct nvrtx_exec_depth_format *depth;
	const uint32_t *identity;
	uint32_t components;
	uint32_t type;
	uint32_t second;
	uint32_t one;
	uint32_t srgb;

	/* A depth format's entry: its depth as red, its stencil's own type. */
	depth = nvrtx_exec_depth_format_find(format);
	if (depth != NULL) {
		components = depth->components;
		type = depth->depth_type;
		second = depth->stencil_type;
		identity = depth_identity;
		one = TIC_SOURCE_ONE_FLOAT;
		srgb = 0U;
	} else {
		/* A colour format's entry (one the caller checked; R8G8B8A8_UNORM's for another), every component of its type. */
		entry = nvrtx_exec_format_find(format);
		if (entry == NULL)
			entry = nvrtx_exec_format_find(VK_FORMAT_R8G8B8A8_UNORM);
		components = entry->components;
		type = entry->type;
		second = entry->type;
		identity = entry->identity;
		one = TIC_SOURCE_ONE_FLOAT;
		if (entry->type == EXEC_TIC_UINT || entry->type == EXEC_TIC_SINT)
			one = TIC_SOURCE_ONE_INT;
		srgb = entry->srgb;
	}

	/* Starts empty. */
	kern_memset(header, 0, EXEC_TIC_WORDS * sizeof(uint32_t));

	/* The format: its component sizes, each component's data type, each source after the view's swizzle. */
	exec_field(header, 0U, 7U, components);
	exec_field(header, 7U, 3U, type);
	exec_field(header, 10U, 3U, second);
	exec_field(header, 13U, 3U, type);
	exec_field(header, 16U, 3U, type);
	exec_field(header, 19U, 3U, exec_swizzle_source(swizzle[0], identity, one));
	exec_field(header, 22U, 3U, exec_swizzle_source(swizzle[1], identity, one));
	exec_field(header, 25U, 3U, exec_swizzle_source(swizzle[2], identity, one));
	exec_field(header, 28U, 3U, exec_swizzle_source(swizzle[3], identity, one));

	/* The address (bits 31:5 and 48:32), the pitch header, the pitch (bits 20:5). */
	exec_field(header, 37U, 27U, (uint32_t)((address & 0xffffffffU) >> 5));
	exec_field(header, 64U, 17U, (uint32_t)(address >> 32));
	exec_field(header, 85U, 3U, TIC_VERSION_PITCH);
	exec_field(header, 96U, 16U, pitch >> 5);

	/*
	 * An sRGB format's colour decoded as it is sampled.
	 *
	 * Value referenced: Mesa 25.0.7 src/nouveau/headers/nvidia/classes/clb097tex.h
	 * (NVB097_TEXHEAD_PITCH_S_R_G_B_CONVERSION and _BL_, bit 150) and
	 * src/nouveau/nil/tic.rs (nvb097_fill_tic: set when the view's format
	 * is sRGB).
	 */
	exec_field(header, 150U, 1U, srgb);

	/* The level of detail quality, high. */
	exec_field(header, 112U, 1U, 1U);
	exec_field(header, 113U, 1U, 1U);
	exec_field(header, 114U, 1U, 1U);

	/* The size and the type: 2D without mipmaps, one level, depth one. */
	exec_field(header, 128U, 17U, width - 1U);
	exec_field(header, 151U, 4U, TIC_TYPE_TWO_D_NO_MIPMAP);
	exec_field(header, 160U, 16U, height - 1U);

	/*
	 * The sector promotion, border from the sampler, normalized coordinates
	 * and spread functions.
	 */
	exec_field(header, 155U, 2U, TIC_PROMOTE_TO_2_V);
	exec_field(header, 157U, 3U, TIC_BORDER_SAMPLER_COLOR);
	exec_field(header, 191U, 1U, 1U);
	exec_field(header, 215U, 2U, TIC_SPREAD_FUNC_TWO);
	exec_field(header, 217U, 2U, TIC_SPREAD_FUNC_ONE);
}

/*
 * Writes the texture header of a buffer's texels (a uniform texel buffer)
 * in one of the executor's colour formats: the format as a pitch-linear
 * image's with each channel its own, the whole address, the buffer
 * header, the count of texels less one, and the buffer type.
 *
 * Values referenced: Mesa 25.0.7 src/nouveau/nil/tic.rs
 * (nvb097_nil_fill_buffer_tic) and
 * src/nouveau/headers/nvidia/classes/clb097tex.h (NVB097_TEXHEAD_1D_*:
 * ADDRESS_BITS31TO0 63:32, ADDRESS_BITS47TO32 79:64, HEADER_VERSION
 * 87:85, WIDTH_MINUS_ONE_BITS31TO16 111:96, WIDTH_MINUS_ONE_BITS15TO0
 * 143:128, TEXTURE_TYPE 154:151, SECTOR_PROMOTION 156:155).
 */
void
nvrtx_exec_texture_header_buffer(
	uint32_t *header,
	uint64_t address,
	uint32_t texels,
	uint32_t format)
{
	static const uint32_t identity[4] = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A };

	/* The format's words as a pitch-linear image's; the rest is written again below. */
	nvrtx_exec_texture_header(header, 0U, 1U, 1U, 0U, format, identity);
	kern_memset(&header[1], 0, (EXEC_TIC_WORDS - 1U) * sizeof(uint32_t));

	/* The address, whole: bits 31:0, then 47:32. */
	exec_field(header, 32U, 32U, (uint32_t)(address & 0xffffffffU));
	exec_field(header, 64U, 16U, (uint32_t)(address >> 32));

	/* The buffer header, the texels less one in two halves, the buffer type and the sector promotion. */
	exec_field(header, 85U, 3U, TIC_VERSION_ONE_D_BUFFER);
	exec_field(header, 96U, 16U, (texels - 1U) >> 16);
	exec_field(header, 128U, 16U, (texels - 1U) & 0xffffU);
	exec_field(header, 151U, 4U, TIC_TYPE_ONE_D_BUFFER);
	exec_field(header, 155U, 2U, TIC_PROMOTE_TO_2_V);
}

/*
 * Writes the texture header of a tiled 2D image of four 8-bit channels
 * (block linear in one-GOB blocks, levels levels from address, level 0
 * width by height): the format as the pitch-linear header's, the block
 * linear header, the levels the view sees (from base_level, level_count
 * of them; one level alone is a surface of that level's own, as a quad's
 * source in pixels needs).
 */
void
nvrtx_exec_texture_header_tiled(
	uint32_t *header,
	uint64_t address,
	uint32_t width,
	uint32_t height,
	uint32_t levels,
	uint32_t base_level,
	uint32_t level_count,
	uint32_t format,
	const uint32_t *swizzle)
{
	struct nvrtx_exec_texture_shape shape;

	/* One 2D surface of the levels. */
	shape.width = width;
	shape.height = height;
	shape.depth = 1U;
	shape.type = NVRTX_EXEC_TEXTURE_2D;
	shape.levels = levels;
	shape.base_level = base_level;
	shape.level_count = level_count;
	shape.samples = 1U;
	nvrtx_exec_texture_header_shaped(header, address, &shape, format, swizzle);
}

/*
 * Writes the texture header of a tiled image of four 8-bit channels in a
 * shape (block linear in one-GOB blocks each way, one GOB deep too; its
 * levels from address, the layers after them the hardware finds from the
 * levels' sizes, which is why the header has the image's every level): the
 * format as the pitch-linear header's, the block linear header, the type,
 * the size and depth, and the levels the view sees.
 *
 * Values referenced: Mesa 25.0.7
 * src/nouveau/headers/nvidia/classes/clb097tex.h (NVB097_TEXHEAD_BL_*)
 * and src/nouveau/nil/tic.rs (nvb097_fill_tic, normalize_extent,
 * nil_rs_max_mip_level: no base layer field, the depth an array's layers
 * or a cube map array's cubes).
 */
void
nvrtx_exec_texture_header_shaped(
	uint32_t *header,
	uint64_t address,
	const struct nvrtx_exec_texture_shape *shape,
	uint32_t format,
	const uint32_t *swizzle)
{
	uint32_t index;

	/* The pitch-linear header's format and fields, its address, pitch and type taken out. */
	nvrtx_exec_texture_header(header, 0U, shape->width, shape->height, 0U, format, swizzle);
	for (index = 37U; index < 64U; index++)
		header[index / 32U] &= ~(1U << (index % 32U));
	for (index = 85U; index < 112U; index++)
		header[index / 32U] &= ~(1U << (index % 32U));
	for (index = 151U; index < 155U; index++)
		header[index / 32U] &= ~(1U << (index % 32U));

	/* The address (bits 31:9 and 47:32), block linear, blocks of one GOB each way, the type. */
	exec_field(header, 41U, 23U, (uint32_t)((address & 0xffffffffU) >> 9));
	exec_field(header, 64U, 16U, (uint32_t)(address >> 32));
	exec_field(header, 85U, 3U, TIC_VERSION_BLOCKLINEAR);
	exec_field(header, 151U, 4U, shape->type);

	/* The depth: the slices, the layers or the cubes. */
	exec_field(header, 176U, 14U, shape->depth - 1U);

	/* The levels: the image's last, and the view's first and last. */
	exec_field(header, 124U, 4U, shape->levels - 1U);
	exec_field(header, 224U, 4U, shape->base_level);
	exec_field(header, 228U, 4U, shape->base_level + shape->level_count - 1U);

	/*
	 * A multisampled image's samples: 2x2 a pixel (its size in pixels;
	 * the hardware finds a sample in its pixel's block).
	 *
	 * Values referenced: Mesa 25.0.7 src/nouveau/nil/tic.rs
	 * (nvb097_fill_tic: MULTI_SAMPLE_COUNT from the sample layout, the
	 * extent in pixels) and src/nouveau/headers/nvidia/classes/clb097tex.h
	 * (NVB097_TEXHEAD_BL_MULTI_SAMPLE_COUNT 235:232, _MODE_2X2 2).
	 */
	if (shape->samples == 4U)
		exec_field(header, 232U, 4U, TIC_MULTI_SAMPLE_2X2);
}

/*
 * Writes the texture header of one plane of a video picture: block linear
 * in blocks one GOB wide and two GOBs high, one level, width by height
 * texels of channels 8-bit unsigned normalized channels (1 the luma plane
 * as R8, 2 the chroma plane as R8G8).  The channels a plane does not have
 * read as zero, and alpha as one, before the view's swizzle.
 *
 * Values referenced: Mesa 25.0.7 src/nouveau/nil/tic.rs (nvb097_fill_tic,
 * TEXHEAD_BL_GOBS_PER_BLOCK_HEIGHT from the image's tiling).
 */
void
nvrtx_exec_texture_header_plane(
	uint32_t *header,
	uint64_t address,
	uint32_t width,
	uint32_t height,
	uint32_t channels,
	const uint32_t *swizzle)
{
	static const uint32_t red[4] = { TIC_SOURCE_R, TIC_SOURCE_ZERO, TIC_SOURCE_ZERO, TIC_SOURCE_ONE_FLOAT };
	static const uint32_t red_green[4] = { TIC_SOURCE_R, TIC_SOURCE_G, TIC_SOURCE_ZERO, TIC_SOURCE_ONE_FLOAT };
	const uint32_t *identity;
	uint32_t sizes;
	uint32_t source;
	uint32_t index;

	/* The tiled header of one level, its component sizes and sources taken out. */
	nvrtx_exec_texture_header_tiled(header, address, width, height, 1U, 0U, 1U, VK_FORMAT_R8G8B8A8_UNORM, swizzle);
	for (index = 0U; index < 7U; index++)
		header[index / 32U] &= ~(1U << (index % 32U));
	for (index = 19U; index < 31U; index++)
		header[index / 32U] &= ~(1U << (index % 32U));

	/* The plane's channels, each source after the view's swizzle. */
	identity = red;
	sizes = TIC_COMPONENTS_R8;
	if (channels == 2U) {
		identity = red_green;
		sizes = TIC_COMPONENTS_G8R8;
	}

	/* The component sizes. */
	exec_field(header, 0U, 7U, sizes);

	/* The red source: the view's swizzle of the plane's channel. */
	source = exec_swizzle_source(swizzle[0], identity, TIC_SOURCE_ONE_FLOAT);
	exec_field(header, 19U, 3U, source);

	/* The green source. */
	source = exec_swizzle_source(swizzle[1], identity, TIC_SOURCE_ONE_FLOAT);
	exec_field(header, 22U, 3U, source);

	/* The blue source. */
	source = exec_swizzle_source(swizzle[2], identity, TIC_SOURCE_ONE_FLOAT);
	exec_field(header, 25U, 3U, source);

	/* The alpha source. */
	source = exec_swizzle_source(swizzle[3], identity, TIC_SOURCE_ONE_FLOAT);
	exec_field(header, 28U, 3U, source);

	/* Blocks two GOBs high, as the decoder writes them. */
	exec_field(header, 99U, 3U, TIC_BLOCK_HEIGHT_TWO_GOBS);
}

/*
 * Writes a sampler: the address modes, the depth compare, the font filter
 * and sRGB border fields, the filters (nearest or linear, the mipmap mode),
 * the seamless cube setting, the coordinates' normalization and the
 * levels' range.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_sampler.c
 * (nvk_sampler_get_header).
 */
void
nvrtx_exec_sampler_words(
	uint32_t *words,
	const struct nvrtx_exec_sampler *sampler)
{
	uint32_t mag;
	uint32_t min;
	uint32_t mip;

	/* Starts empty. */
	kern_memset(words, 0, EXEC_TSC_WORDS * sizeof(uint32_t));

	/* Word 0: the address modes, the border conversion and the font filter. */
	exec_field(words, 0U, 3U, exec_address_mode(sampler->address_u));
	exec_field(words, 3U, 3U, exec_address_mode(sampler->address_v));
	exec_field(words, 6U, 3U, exec_address_mode(sampler->address_w));

	/*
	 * A depth compare: on, by its function (the ZC values in the order of
	 * VkCompareOp).
	 *
	 * Values referenced: Mesa 25.0.7
	 * src/nouveau/headers/nvidia/classes/clb097tex.h
	 * (NVB097_TEXSAMP0_DEPTH_COMPARE, NVB097_TEXSAMP0_DEPTH_COMPARE_FUNC_ZC_*)
	 * and src/nouveau/vulkan/nvk_sampler.c (vk_to_9097_texsamp_compare_op).
	 */
	if (sampler->compare_enable != 0U) {
		exec_field(words, 9U, 1U, 1U);
		exec_field(words, 10U, 3U, sampler->compare_op & 7U);
	}
	exec_field(words, 13U, 1U, 1U);
	exec_field(words, 14U, 3U, TSC_FONT_SIZE_2);
	exec_field(words, 17U, 3U, TSC_FONT_SIZE_2);

	/* Word 1: the filters, one level, the cube seams, the normalization. */
	mag = TSC_MAG_POINT;
	if (sampler->mag_filter == VK_FILTER_LINEAR)
		mag = TSC_MAG_LINEAR;
	min = TSC_MIN_POINT;
	if (sampler->min_filter == VK_FILTER_LINEAR)
		min = TSC_MIN_LINEAR;
	mip = TSC_MIP_POINT;
	if (sampler->mipmap_mode == VK_SAMPLER_MIPMAP_MODE_LINEAR)
		mip = TSC_MIP_LINEAR;
	exec_field(words, 32U + 0U, 3U, mag);
	exec_field(words, 32U + 4U, 2U, min);
	exec_field(words, 32U + 6U, 2U, mip);
	exec_field(words, 32U + 8U, 2U, TSC_CUBE_AUTO_SPAN_SEAM);
	if (sampler->unnormalized != 0U)
		exec_field(words, 32U + 25U, 1U, TSC_FORCE_UNNORMALIZED);

	/* Word 2: the levels' range in 4.8 fixed point, within 0 and the largest. */
	exec_field(words, 64U + 0U, 12U, exec_lod(sampler->min_lod));
	exec_field(words, 64U + 12U, 12U, exec_lod(sampler->max_lod));
}

/* Reports a level of detail (a float's bits) in 4.8 fixed point, within 0 and the largest. */
static uint32_t
exec_lod(
	uint32_t bits)
{
	int64_t value;

	/* Negative is 0, beyond 4.8 is its largest. */
	value = nvrtx_exec_fixed(bits, 8U);
	if (value < 0)
		return 0U;
	if (value > (int64_t)TSC_LOD_MAX)
		return TSC_LOD_MAX;

	/* Succeeded: the level in fixed point. */
	return (uint32_t)value;
}

/*
 * Reports the bits of a colour channel (0 red to 3 alpha) of an unsigned
 * normalized colour format the executor renders to (an sRGB format's
 * alpha too), or 0 for a channel whose clear colour is not quantized as a
 * normalized one (float and integer formats, an sRGB format's colour).
 */
static uint32_t
exec_unorm_bits(
	uint32_t format,
	uint32_t component)
{
	/* The formats of 8 bits a channel, of 16, and the packed one of 10 and 2. */
	switch (format) {
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8_UNORM:
	case VK_FORMAT_R8G8_UNORM:
		return 8U;
	case VK_FORMAT_R16_UNORM:
	case VK_FORMAT_R16G16_UNORM:
	case VK_FORMAT_R16G16B16A16_UNORM:
		return 16U;
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		if (component == 3U)
			return 2U;
		return 10U;
	case VK_FORMAT_B8G8R8A8_SRGB:
	case VK_FORMAT_R8G8B8A8_SRGB:
		if (component == 3U)
			return 8U;
		return 0U;
	default:
		break;
	}

	/* Not a normalized format: its colour goes as it is. */
	return 0U;
}

/*
 * Reports the bits of a signed normalized colour format's channels, less
 * the sign (its largest value 2^bits - 1), or 0 for another format.
 */
static uint32_t
exec_snorm_bits(
	uint32_t format)
{
	/* The formats of 8 bits a channel and of 16. */
	switch (format) {
	case VK_FORMAT_R8_SNORM:
	case VK_FORMAT_R8G8_SNORM:
	case VK_FORMAT_R8G8B8A8_SNORM:
		return 7U;
	case VK_FORMAT_R16_SNORM:
	case VK_FORMAT_R16G16_SNORM:
	case VK_FORMAT_R16G16B16A16_SNORM:
		return 15U;
	default:
		break;
	}

	/* Not a signed normalized format. */
	return 0U;
}

/*
 * Reports a colour of three floats (their bits; the fourth is not used)
 * packed as E5B9G9R9_UFLOAT_PACK32: each channel within 0 and the largest
 * the format holds (NaN 0), the shared exponent from the largest channel,
 * then each channel's 9-bit mantissa rounded to the nearest at that
 * exponent.  Worked in integers, the kernel having no floating point.
 *
 * Procedure referenced: the Vulkan 1.3 specification, "Shared Exponent"
 * (N 9, B 15, Emax 31: sharedexp_max, exp', max_s, exp_shared and the
 * channels' rounding).  Mesa 25.0.7 src/nouveau/vulkan/nvk_cmd_clear.c
 * (nvk_CmdClearColorImage: a format that is not a colour target is
 * cleared as the integer format of its size, the colour packed).
 */
uint32_t
nvrtx_exec_rgb9e5(
	const uint32_t *colour)
{
	uint32_t channels[3];
	uint32_t largest;
	uint32_t mantissa;
	uint32_t packed;
	int32_t exponent;
	uint32_t index;

	/* Each channel within 0 and 65408 (511/512 of 2^16), the largest of them. */
	largest = 0U;
	for (index = 0U; index < 3U; index++) {
		channels[index] = colour[index];
		if ((channels[index] & FLOAT_SIGN) != 0U ||
		    (channels[index] & ~FLOAT_SIGN) > (FLOAT_EXPONENT_MASK << FLOAT_EXPONENT_SHIFT))
			channels[index] = 0U;
		if (channels[index] > EXEC_SHARED_LARGEST)
			channels[index] = EXEC_SHARED_LARGEST;
		if (channels[index] > largest)
			largest = channels[index];
	}

	/* exp' = max(-B - 1, floor(log2(largest))) + 1 + B: a denormal or zero has the least. */
	exponent = 0;
	if (((largest >> FLOAT_EXPONENT_SHIFT) & FLOAT_EXPONENT_MASK) != 0U) {
		exponent = (int32_t)((largest >> FLOAT_EXPONENT_SHIFT) & FLOAT_EXPONENT_MASK) - FLOAT_BIAS;
		if (exponent < -EXEC_SHARED_BIAS - 1)
			exponent = -EXEC_SHARED_BIAS - 1;
		exponent += 1 + EXEC_SHARED_BIAS;
	}

	/* One more when the largest channel's mantissa rounds up to 2^N. */
	mantissa = exec_shared_mantissa(largest, exponent - EXEC_SHARED_BIAS - EXEC_SHARED_BITS);
	if (mantissa >= (1U << EXEC_SHARED_BITS))
		exponent++;

	/* Each channel's mantissa at the shared exponent, then the exponent. */
	packed = 0U;
	for (index = 0U; index < 3U; index++) {
		mantissa = exec_shared_mantissa(channels[index], exponent - EXEC_SHARED_BIAS - EXEC_SHARED_BITS);
		packed |= mantissa << (index * EXEC_SHARED_BITS);
	}
	packed |= (uint32_t)exponent << (3U * EXEC_SHARED_BITS);

	/* Succeeded: the packed texel. */
	return packed;
}

/*
 * Reports floor(value / 2^scale + 0.5) for a float of 0 or more (its
 * bits, at most 65408), the mantissa of a shared exponent format.
 */
static uint32_t
exec_shared_mantissa(
	uint32_t bits,
	int32_t scale)
{
	uint64_t mantissa;
	int32_t exponent;
	int32_t shift;

	/* The float's mantissa (its hidden bit when normal) and the power of two it is scaled by. */
	exponent = (int32_t)((bits >> FLOAT_EXPONENT_SHIFT) & FLOAT_EXPONENT_MASK);
	mantissa = bits & FLOAT_MANTISSA_MASK;
	if (exponent != 0)
		mantissa |= FLOAT_HIDDEN;
	else
		exponent = 1;
	shift = exponent - FLOAT_BIAS - FLOAT_MANTISSA_BITS - scale;

	/* Moved to the scale, a half rounded up. */
	if (shift >= 0)
		return (uint32_t)(mantissa << shift);
	if (shift < -40)
		return 0U;
	mantissa = (mantissa + ((uint64_t)1 << (-shift - 1))) >> -shift;

	/* Succeeded: the scaled value. */
	return (uint32_t)mantissa;
}

/*
 * Reports a float (its bits) quantized to a normalized channel of width
 * bits: within 0 and 1 (NaN and negatives 0), times the channel's largest
 * value rounded to the nearest (a half up), and that step given back as
 * the float nearest below it, so that the class's conversion of it lands
 * on the step.  Worked in fixed point, the kernel having no floating
 * point.
 */
static uint32_t
exec_unorm_quantize(
	uint32_t bits,
	uint32_t width)
{
	uint64_t largest;
	uint64_t fixed;
	uint64_t step;
	uint32_t quantized;

	/* NaN, a negative and zero are 0; 1 and more (infinity too) are 1. */
	if ((bits & ~FLOAT_SIGN) > (FLOAT_EXPONENT_MASK << FLOAT_EXPONENT_SHIFT))
		return 0U;
	if ((bits & FLOAT_SIGN) != 0U)
		return 0U;
	if (bits >= V_FLOAT_ONE)
		return V_FLOAT_ONE;

	/* The step nearest the value times the largest, a half rounded up. */
	largest = ((uint64_t)1 << width) - 1U;
	fixed = (uint64_t)nvrtx_exec_fixed(bits, EXEC_UNORM_FRACTION);
	step = (fixed * largest + ((uint64_t)1 << (EXEC_UNORM_FRACTION - 1U))) >> EXEC_UNORM_FRACTION;

	/* The step over the largest in fixed point, rounded, then as a float. */
	fixed = ((step << EXEC_UNORM_FRACTION) + largest / 2U) / largest;
	quantized = nvrtx_exec_float_fixed((int64_t)fixed, EXEC_UNORM_FRACTION);

	/* Succeeded: the quantized value's bits. */
	return quantized;
}

/* Reports whether a channel of a colour format is an sRGB one (red, green or blue of the 8-bit sRGB formats). */
static bool
exec_srgb_channel(
	uint32_t format,
	uint32_t component)
{
	/* The sRGB formats; their alpha is linear. */
	if (format != VK_FORMAT_B8G8R8A8_SRGB && format != VK_FORMAT_R8G8B8A8_SRGB)
		return false;
	if (component == 3U)
		return false;

	/* Succeeded: an sRGB channel. */
	return true;
}

/*
 * Reports a linear float (its bits) quantized to an 8-bit sRGB channel:
 * within 0 and 1 (NaN and negatives 0), the step whose encoding the
 * value's encoding times 255 rounds to (a half up), as that step's linear
 * value (exec_srgb_steps), which the class's own encoding lands on.
 */
static uint32_t
exec_srgb_quantize(
	uint32_t bits)
{
	uint32_t low;
	uint32_t high;
	uint32_t middle;

	/* NaN, a negative and zero are 0; 1 and more (infinity too) are 1. */
	if ((bits & ~FLOAT_SIGN) > (FLOAT_EXPONENT_MASK << FLOAT_EXPONENT_SHIFT))
		return 0U;
	if ((bits & FLOAT_SIGN) != 0U)
		return 0U;
	if (bits >= V_FLOAT_ONE)
		return V_FLOAT_ONE;

	/* How many roundings the value reaches (a positive float's bits order as its values). */
	low = 0U;
	high = EXEC_SRGB_STEPS - 1U;
	while (low < high) {
		middle = (low + high) / 2U;
		if (bits >= exec_srgb_rounding[middle])
			low = middle + 1U;
		else
			high = middle;
	}

	/* Succeeded: the step's linear value. */
	return exec_srgb_steps[low];
}

/* Reports the float bits of a whole number (exact up to 2^24). */
uint32_t
nvrtx_exec_float(
	int32_t value)
{
	uint32_t bits;

	/* A whole number is a fixed point value without fraction. */
	bits = nvrtx_exec_float_fixed((int64_t)value, 0U);

	/* Succeeded: the float's bits. */
	return bits;
}

/*
 * Reports a float, given by its bits, in fixed point with fraction
 * fractional bits, truncated toward zero; a value too large for 40
 * integer bits saturates, a denormal is zero.
 */
int64_t
nvrtx_exec_fixed(
	uint32_t bits,
	uint32_t fraction)
{
	int64_t value;
	int32_t exponent;
	int32_t shift;
	uint64_t mantissa;

	/* The exponent and the mantissa with its hidden bit. */
	exponent = (int32_t)((bits >> FLOAT_EXPONENT_SHIFT) & FLOAT_EXPONENT_MASK);
	if (exponent == 0)
		return 0;
	mantissa = (uint64_t)((bits & FLOAT_MANTISSA_MASK) | FLOAT_HIDDEN);

	/* The mantissa moved to the fixed point's scale. */
	shift = exponent - FLOAT_BIAS - FLOAT_MANTISSA_BITS + (int32_t)fraction;
	if (shift > 39) {
		value = (int64_t)1 << 62;
	} else if (shift >= 0) {
		value = (int64_t)(mantissa << shift);
	} else if (shift > -64) {
		value = (int64_t)(mantissa >> (uint32_t)(-shift));
	} else {
		value = 0;
	}

	/* The sign. */
	if ((bits & FLOAT_SIGN) != 0U)
		value = -value;

	/* Succeeded: the value in fixed point. */
	return value;
}

/*
 * Reports the float bits of a fixed point value with fraction fractional
 * bits: exact when its significant bits fit the mantissa, truncated
 * otherwise.
 */
uint32_t
nvrtx_exec_float_fixed(
	int64_t value,
	uint32_t fraction)
{
	uint64_t magnitude;
	uint32_t sign;
	uint32_t mantissa;
	int32_t top;
	int32_t exponent;

	/* Zero has no bits set. */
	if (value == 0)
		return 0U;

	/* The sign and the magnitude. */
	sign = 0U;
	magnitude = (uint64_t)value;
	if (value < 0) {
		sign = FLOAT_SIGN;
		magnitude = (uint64_t)(-value);
	}

	/* The highest bit set is the hidden bit. */
	top = 63;
	while ((magnitude & ((uint64_t)1 << top)) == 0U)
		top--;

	/* The exponent; a value too small for a normal float is zero. */
	exponent = top - (int32_t)fraction + FLOAT_BIAS;
	if (exponent <= 0)
		return 0U;

	/* The mantissa's 23 bits below the hidden bit. */
	if (top > FLOAT_MANTISSA_BITS) {
		mantissa = (uint32_t)(magnitude >> (uint32_t)(top - FLOAT_MANTISSA_BITS));
	} else {
		mantissa = (uint32_t)(magnitude << (uint32_t)(FLOAT_MANTISSA_BITS - top));
	}

	/* Succeeded: the float's bits. */
	return sign | ((uint32_t)exponent << FLOAT_EXPONENT_SHIFT) | (mantissa & FLOAT_MANTISSA_MASK);
}

/* Writes a method header on the 3D subchannel: incrementing, count words from method. */
static size_t
exec_method(
	uint8_t *push,
	size_t at,
	uint32_t method,
	uint32_t count)
{
	/* The header: incrementing, the count, the subchannel (0), the method in words. */
	nvrtx_put32(push, at, EXEC_METHOD_INCREMENT | (count << 16) | (method >> 2));

	/* Succeeded: the words follow. */
	return at + 4U;
}

/* Writes one method with one word. */
static size_t
exec_set(
	uint8_t *push,
	size_t at,
	uint32_t method,
	uint32_t value)
{
	/* The header, then the word. */
	at = exec_method(push, at, method, 1U);
	nvrtx_put32(push, at, value);

	/* Succeeded: past the word. */
	return at + 4U;
}

/*
 * Reports BEGIN's word for a VkPrimitiveTopology the executor draws (a
 * triangle list for another; the pipeline refuses the others): the op,
 * the instances iterated.
 */
static uint32_t
exec_begin(
	uint32_t topology)
{
	uint32_t op;

	/* The op of the topology. */
	switch (topology) {
	case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
		op = V_BEGIN_POINTS;
		break;
	case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
		op = V_BEGIN_LINES;
		break;
	case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
		op = V_BEGIN_LINE_STRIP;
		break;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
		op = V_BEGIN_TRIANGLE_STRIP;
		break;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
		op = V_BEGIN_TRIANGLE_FAN;
		break;
	case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
		op = V_BEGIN_LINES_ADJACENCY;
		break;
	case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
		op = V_BEGIN_LINE_STRIP_ADJACENCY;
		break;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
		op = V_BEGIN_TRIANGLES_ADJACENCY;
		break;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
		op = V_BEGIN_TRIANGLE_STRIP_ADJACENCY;
		break;
	default:
		op = V_BEGIN_TRIANGLES;
		break;
	}

	/* Succeeded: the op, the instances iterated. */
	return op | V_BEGIN_ITERATE;
}

/* Reports the class's value of a VkBlendOp (addition for any other). */
static uint32_t
exec_blend_op(
	uint32_t op)
{
	/* The five operations. */
	switch (op) {
	case VK_BLEND_OP_SUBTRACT:
		return V_BLEND_SUBTRACT;
	case VK_BLEND_OP_REVERSE_SUBTRACT:
		return V_BLEND_REVERSE_SUBTRACT;
	case VK_BLEND_OP_MIN:
		return V_BLEND_MIN;
	case VK_BLEND_OP_MAX:
		return V_BLEND_MAX;
	default:
		break;
	}

	/* Succeeded: addition. */
	return V_BLEND_ADD;
}

/* Reports the class's value of a VkBlendFactor (one for one out of range). */
static uint32_t
exec_blend_factor(
	uint32_t factor)
{
	/* The factors of colour and alpha. */
	switch (factor) {
	case VK_BLEND_FACTOR_ZERO:
		return V_FACTOR_ZERO;
	case VK_BLEND_FACTOR_SRC_COLOR:
		return V_FACTOR_SRC_COLOR;
	case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
		return V_FACTOR_ONE_MINUS_SRC_COLOR;
	case VK_BLEND_FACTOR_DST_COLOR:
		return V_FACTOR_DST_COLOR;
	case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:
		return V_FACTOR_ONE_MINUS_DST_COLOR;
	case VK_BLEND_FACTOR_SRC_ALPHA:
		return V_FACTOR_SRC_ALPHA;
	case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
		return V_FACTOR_ONE_MINUS_SRC_ALPHA;
	case VK_BLEND_FACTOR_DST_ALPHA:
		return V_FACTOR_DST_ALPHA;
	case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:
		return V_FACTOR_ONE_MINUS_DST_ALPHA;
	case VK_BLEND_FACTOR_CONSTANT_COLOR:
		return V_FACTOR_CONSTANT_COLOR;
	case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR:
		return V_FACTOR_ONE_MINUS_CONSTANT_COLOR;
	case VK_BLEND_FACTOR_CONSTANT_ALPHA:
		return V_FACTOR_CONSTANT_ALPHA;
	case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA:
		return V_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
	case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE:
		return V_FACTOR_SRC_ALPHA_SATURATE;
	case VK_BLEND_FACTOR_SRC1_COLOR:
		return V_FACTOR_SRC1_COLOR;
	case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR:
		return V_FACTOR_ONE_MINUS_SRC1_COLOR;
	case VK_BLEND_FACTOR_SRC1_ALPHA:
		return V_FACTOR_SRC1_ALPHA;
	case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA:
		return V_FACTOR_ONE_MINUS_SRC1_ALPHA;
	default:
		break;
	}

	/* Succeeded: one. */
	return V_FACTOR_ONE;
}

/* Finds a vertex format the attributes fetch (NULL for one they do not). */
static const struct exec_vertex_format *
exec_vertex_format_find(
	uint32_t format)
{
	uint32_t index;

	/* The table. */
	for (index = 0U; index < sizeof(exec_vertex_formats) / sizeof(exec_vertex_formats[0]); index++) {
		if (exec_vertex_formats[index].format == format)
			return &exec_vertex_formats[index];
	}

	/* Not fetched. */
	return NULL;
}

/* Reports an attribute word's component widths and numerical type for a vertex format (four floats for one not fetched). */
static uint32_t
exec_attribute_word(
	uint32_t format)
{
	const struct exec_vertex_format *found;

	/* The format's widths and type. */
	found = exec_vertex_format_find(format);
	if (found != NULL)
		return (found->widths << V_ATTRIBUTE_WIDTHS_SHIFT) | (found->type << V_ATTRIBUTE_TYPE_SHIFT) | found->swap;

	/* Four floats. */
	return (V_WIDTHS_R32_G32_B32_A32 << V_ATTRIBUTE_WIDTHS_SHIFT) | (V_ATTRIBUTE_FLOAT << V_ATTRIBUTE_TYPE_SHIFT);
}

/* Reports the sampler's value of a VkSamplerAddressMode (clamp to edge for any other). */
static uint32_t
exec_address_mode(
	uint32_t mode)
{
	/* Repeat, mirror and border. */
	switch (mode) {
	case VK_SAMPLER_ADDRESS_MODE_REPEAT:
		return TSC_WRAP;
	case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:
		return TSC_MIRROR;
	case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:
		return TSC_BORDER;
	default:
		break;
	}

	/* Succeeded: clamp to edge. */
	return TSC_CLAMP_TO_EDGE;
}

/* Sets a field of bits words, from bit low (counted over the words) for bits bits. */
static void
exec_field(
	uint32_t *words,
	unsigned low,
	unsigned bits,
	uint32_t value)
{
	unsigned word;
	unsigned shift;

	/* The value's bits only (a field of 32 bits takes it whole). */
	if (bits < 32U)
		value &= (1U << bits) - 1U;

	/* Into its first word, and the rest into the next one when it crosses it. */
	word = low / 32U;
	shift = low % 32U;
	words[word] |= value << shift;
	if (shift + bits > 32U)
		words[word + 1U] |= value >> (32U - shift);
}

/*
 * Reports a header source for a channel of a view's swizzle: one of the
 * image's channels (through where the format keeps it), zero or one (the
 * format's one, a float's or an integer's).
 */
static uint32_t
exec_swizzle_source(
	uint32_t swizzle,
	const uint32_t *identity,
	uint32_t one)
{
	/* The constants, then the image's channels. */
	switch (swizzle) {
	case VK_COMPONENT_SWIZZLE_ZERO:
		return TIC_SOURCE_ZERO;
	case VK_COMPONENT_SWIZZLE_ONE:
		return one;
	case VK_COMPONENT_SWIZZLE_R:
		return identity[0];
	case VK_COMPONENT_SWIZZLE_G:
		return identity[1];
	case VK_COMPONENT_SWIZZLE_B:
		return identity[2];
	default:
		break;
	}

	/* Succeeded: alpha. */
	return identity[3];
}
