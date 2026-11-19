/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The graphics pipelines and draws of the PowerVR executor.  See
 * render-draw.h.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libc/vulkan/vulkan_core.h>
#include <uapi/errno.h>

#include "drivers/gpu/powervr/compiler/pds-encode.h"
#include "drivers/gpu/powervr/render-draw.h"
#include "drivers/gpu/powervr/render-pass.h"
#include "drivers/gpu/powervr/compiler/shader.h"

/*
 * The coefficient program's iterator words.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/csbgen/rogue/pds.xml (DOUT_FIELDS_DOUTI_SRC
 * f16_offset 0..7, f32_offset 8..15, perspective 16, size 18..19 from 1D
 * 0, pointsprite 23, shademodel 24..25 FLAT_VERTEX0 0 and GOURUAD 3);
 * src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_DOUT_FIELDS_DOUTI_SRC0_DEST_SHIFT 54,
 * _ISSUE_EN 1 << 63).
 */
#define DOUTI_F16_OFFSET_SHIFT	0U
#define DOUTI_F32_OFFSET_SHIFT	8U
#define DOUTI_PERSPECTIVE	(1U << 16)
#define DOUTI_SIZE_SHIFT	18U
#define DOUTI_POINT_SPRITE	(1U << 23)
#define DOUTI_GOURAUD		(3U << 24)
#define DOUTI_DEST_SHIFT	54U
#define DOUTI_ISSUE		(1ULL << 63)

/*
 * The primitive state's words.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/csbgen/rogue/ppp.xml (STATE_HEADER
 * pres_ispctl 0, pres_ispctl_fa 1, pres_ispctl_dbsc 5, pres_pds_state_ptr0
 * 6, _ptr1 7, _ptr3 9, pres_region_clip 10, pres_viewport 11,
 * view_port_count 12..15, pres_wclamp 16, pres_outselects 17,
 * pres_varying_word0 18, _word1 19, _word2 20, pres_ppp_ctrl 21,
 * pres_stream_out_size 22; STATE_ISPCTL scenable 16; STATE_ISPA objtype
 * 28..31 TRIANGLE 0, passtype 24..26 OPAQUE 0, TRANSLUCENT 1, PUNCH_THROUGH 2 and DEPTH_FEEDBACK 5,
 * dwritedisable 21, dcmpmode 17..19 (the VkCompareOp values),
 * pointlinewidth 8..15; STATE_ISPDBSC scindex 0..15;
 * STATE_PDS_SIZEINFO1 pds_tempsize 0..3 in units of 4, usc_varyingsize
 * 4..9 in units of 16, pds_varyingsize 10..15 in units of 4,
 * pds_uniformsize 23..31 in units of 4; STATE_PDS_SIZEINFO2
 * usc_sharedsize 23..31 in units of 16; OUTPUT_SEL vtxsize 24..31,
 * psprite_size_pres 18, rhw_pres 16; STATE_VARYING0 f32_linear 0..7, f32_flat 8..15;
 * REGION_CLIP0 mode 31 OUTSIDE 1, left 16..24, right 0..8; REGION_CLIP1
 * top 16..24, bottom 0..8; STATE_PPP_CTRL cullmode 0..1 CULL_CW 1 and
 * CULL_CCW 2, wclampen 5, flatshade_vtx 7..8 VERTEX_0 1, drawclippededges
 * 9, clip_mode 10..11 FRONT_REAR 1).
 */
#define HEADER_ISPCTL		(1U << 0)
#define HEADER_ISPCTL_FA	(1U << 1)
#define HEADER_ISPCTL_DBSC	(1U << 5)
#define HEADER_PDS_0		(1U << 6)
#define HEADER_PDS_1		(1U << 7)
#define HEADER_PDS_3		(1U << 9)
#define HEADER_REGION_CLIP	(1U << 10)
#define HEADER_VIEWPORT		(1U << 11)
#define HEADER_WCLAMP		(1U << 16)
#define HEADER_OUTSELECTS	(1U << 17)
#define HEADER_VARYING_0	(1U << 18)
#define HEADER_VARYING_1	(1U << 19)
#define HEADER_VARYING_2	(1U << 20)
#define HEADER_PPP_CTRL		(1U << 21)
#define HEADER_STREAM_OUT	(1U << 22)
#define ISPCTL_SCISSOR		(1U << 16)
#define ISPA_TRANSLUCENT	(1U << 24)
#define ISPA_PUNCH_THROUGH	(2U << 24)
#define ISPA_DEPTH_FEEDBACK	(5U << 24)
#define ISPA_PASS_MASK		(7U << 24)
#define ISPA_NO_DEPTH_WRITE	(1U << 21)
#define ISPA_COMPARE_SHIFT	17U
#define ISPA_LINE_WIDTH_SHIFT	8U
#define OUTPUT_RHW		(1U << 16)
#define OUTPUT_POINT_SIZE	(1U << 18)
#define CLIP_OUTSIDE		(1U << 31)
#define CTRL_CULL_CW		1U
#define CTRL_CULL_CCW		2U
#define CTRL_WCLAMP		(1U << 5)
#define CTRL_FLAT_VERTEX_0	(1U << 7)
#define CTRL_CLIPPED_EDGES	(1U << 9)
#define CTRL_FRONT_REAR		(1U << 10)
#define OFFSET_MASK		0xfffffff0U

/*
 * The line width of 1.0 in the ISPA word: 4.4 fixed point less one.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_setup_isp_faces_and_control: util_unsigned_fixed(width, 4), then
 * minus one when not zero).
 */
#define LINE_WIDTH_ONE		15U

/*
 * The w clamp of the primitive state, fui(1.0e-13f).
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_emit_ppp_state: wclamp.val = fui(1.0e-13f)).
 */
#define WCLAMP_VALUE		0x29e12e13U

/*
 * The region clip tile of a core of 16 x 16 tiles.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_get_geom_region_clip_align_size: 16 with tile_size_16x16).
 */
#define REGION_TILE		16U

/*
 * The vertex control stream's words.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/csbgen/rogue/vdm.xml (BLOCK_TYPE at
 * 29..31: PPP_STATE_UPDATE 0, PDS_STATE_UPDATE 1, VDM_STATE_UPDATE 2,
 * INDEX_LIST 3; PDS_STATE0 usc_common_size 16..24 in units of 64 bytes,
 * pds_data_size 0..5 in units of 16 bytes, usc_target ALL 0; PDS_STATE1
 * pds_data_addr 4..31, sd_type NONE 0; PDS_STATE2 pds_code_addr 4..31;
 * VDM_STATE0 vs_data_addr_present 27, vs_other_present 26, cam_size
 * 7..14, flatshade_control VERTEX_0 0; VDM_STATE5 vs_max_instances
 * 25..29, vs_usc_unified_size 10..15 and vs_pds_temp_size 6..9 and
 * vs_pds_data_size 0..5 in units of 16 bytes; INDEX_LIST0
 * index_addr_present 28, index_count_present 27,
 * index_instance_count_present 26, index_offset_present 25, index_size
 * 17..18 B16 1 and B32 2, primitive_topology 8..11 TRI_LIST 6 and
 * TRI_STRIP 9, index_base_addrmsb 0..7).
 */
#define BLOCK_SHIFT		29U
#define BLOCK_PPP		0U
#define BLOCK_PDS		1U
#define BLOCK_VDM		2U
#define BLOCK_INDEX		3U
#define VDM_DATA_PRESENT	(1U << 27)
#define VDM_OTHER_PRESENT	(1U << 26)
#define LIST_ADDRESS		(1U << 28)
#define LIST_COUNT		(1U << 27)
#define LIST_INSTANCES		(1U << 26)
#define LIST_OFFSET		(1U << 25)
#define LIST_SIZE_16		(1U << 17)
#define LIST_SIZE_32		(2U << 17)
#define LIST_TRIANGLE_LIST	(6U << 8)
#define LIST_TRIANGLE_STRIP	(9U << 8)

/*
 * The other primitive topologies of the index list, and the object type
 * of the ISPA word for each kind of primitive.
 *
 * And the words of the stencil faces and the depth bias: the header's
 * presence of the front ISPB and the back ISPA and ISPB, the ISP
 * control's two sides, B words present and depth bias, and the ISPB's
 * fields (the operations and comparisons have the VkStencilOp and
 * VkCompareOp values), off as Mesa leaves it.
 *
 * Values referenced: Mesa 26.2.5 src/imagination/csbgen/rogue/vdm.xml
 * (PRIMITIVE_TOPOLOGY POINT_LIST 0, LINE_LIST 1, LINE_STRIP 3, TRI_FAN
 * 11), src/imagination/csbgen/rogue/ppp.xml (STATE_HEADER pres_ispctl_fb
 * 2, _ba 3, _bb 4; STATE_ISPCTL two_sided 19, bpres 18, dbenable 17;
 * STATE_ISPA sref 0..7; STATE_ISPB scmpmode 25..27, sop1 22..24, sop2
 * 19..21, sop3 16..18, scmpmask 8..15, swmask 0..7; ISPB_STENCILOP and
 * CMPMODE; STATE_ISPDBSC dbindex 16..31; TA_STATE_ISPA objtype
 * 28..31; OBJTYPE TRIANGLE 0, LINE 1, SPRITE_01UV 4),
 * src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_setup_isp_faces_and_control: ispb_stencil_off KEEP, KEEP, KEEP
 * and ALWAYS; sop1 the fail, sop2 the depth fail, sop3 the pass) and
 * src/imagination/vulkan/pvr_csb_enum_helpers.h (pvr_ta_objtype: a
 * point list is SPRITE_01UV, a line list or strip LINE, the triangles
 * TRIANGLE).
 */
#define HEADER_ISPCTL_FB	(1U << 2)
#define HEADER_ISPCTL_BA	(1U << 3)
#define HEADER_ISPCTL_BB	(1U << 4)
#define ISPCTL_TWO_SIDED	(1U << 19)
#define ISPCTL_B_PRESENT	(1U << 18)
#define ISPCTL_DEPTH_BIAS	(1U << 17)

/*
 * The ISP control's visibility test: the samples that pass are counted
 * in the visibility register the low field names, and the fragment
 * command's occlusion query base is where the registers are written.
 *
 * Values referenced: Mesa 26.2.6 src/imagination/csbgen/rogue/ppp.xml
 * (STATE_ISPCTL vistest 15, visreg 0..13) and
 * src/imagination/vulkan/pvr_arch_cmd_buffer.c (the draw's ISP control:
 * vistest and visreg while an occlusion query is active and the
 * rasterizer does not discard).
 */
#define ISPCTL_VISIBILITY_TEST	(1U << 15)
#define ISPCTL_VISIBILITY_MASK	0x3fffU

/*
 * A pipeline that discards its primitives before rasterization: the ISP
 * control's tag write off and the scissor test off, no depth or stencil
 * test, no depth bias and no visibility test, and no viewport words in
 * the state (the header leaves the viewport out).
 *
 * Values referenced: Mesa 26.2.6 src/imagination/csbgen/rogue/ppp.xml
 * (STATE_ISPCTL tagwritedisable 21, scenable 16) and
 * src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_setup_isp_faces_and_control: tagwritedisable set and scenable,
 * dbenable and vistest clear and no depth or stencil aspects under
 * rasterizer discard; pvr_setup_viewport: pres_viewport clear under
 * rasterizer discard).
 */
#define ISPCTL_TAG_WRITE_DISABLE	(1U << 21)
#define LIST_POINT_LIST		(0U << 8)
#define LIST_LINE_LIST		(1U << 8)
#define LIST_LINE_STRIP		(3U << 8)
#define LIST_TRIANGLE_FAN	(11U << 8)
#define ISPB_COMPARE_SHIFT	25U
#define ISPB_FAIL_SHIFT		22U
#define ISPB_DEPTH_FAIL_SHIFT	19U
#define ISPB_PASS_SHIFT		16U
#define ISPB_COMPARE_MASK_SHIFT	8U
#define ISPB_STENCIL_OFF	(7U << ISPB_COMPARE_SHIFT)
#define ISPA_OBJECT_SHIFT	28U
#define OBJECT_TRIANGLE		0U
#define OBJECT_LINE		1U
#define OBJECT_SPRITE		4U
#define UNIT_16			16U
#define UNIT_64			64U
#define FIELD_6_MAX		0x3fU
#define FIELD_4_MAX		0xfU
#define FIELD_9_MAX		0x1ffU

/*
 * The data sequencer's triangle merging, off for a draw that neither
 * writes depth nor compares it, as Mesa does.
 *
 * Values referenced: Mesa 26.2.5 src/imagination/csbgen/rogue/ppp.xml
 * (TA_STATE_PDS_SIZEINFO2 pds_tri_merge_disable 14);
 * src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_setup_triangle_merging_flag: disabled for lines, punch-through,
 * and a depth write disabled with the compare always).
 */
#define SIZEINFO2_TRI_MERGE_DISABLE	(1U << 14)

/*
 * The vertex cache entries and instances of a task of the BXE-4-32 for a
 * vertex of up to 13 output words (the largest of the smallest vertex
 * vector band).
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_arch_calculate_vertex_cam_size with simple_internal_parameter_format
 * and vdm_cam_size 64: the vector (outputs + 1 + 4) * 4 below 14 * 4 gives
 * cam 31 and 16 instances, below 20 * 4 cam 15 and 16, below 28 * 4 cam
 * 11 and 12, below 44 * 4 cam 7 and 8, else (with _v2) cam 7 and 4).
 */
#define VDM_INSTANCES_MAX	0x1fU

static int fixed_raster(const VkGraphicsPipelineCreateInfo *info, struct powervr_draw_fixed *fixed);
static void fixed_stencil_face(const VkStencilOpState *state, struct powervr_draw_stencil_face *face);
static void isp_stencil(const struct powervr_draw_ppp *input, uint32_t *front_a, uint32_t *front_b, uint32_t *back_a, uint32_t *back_b);
static int fixed_vertex(const VkPipelineVertexInputStateCreateInfo *input, struct powervr_draw_fixed *fixed);
static uint32_t format_channel_mask(uint32_t vk_format);
static int fixed_fragment(const VkGraphicsPipelineCreateInfo *info, const struct powervr_pass *pass, struct powervr_draw_fixed *fixed);
static int fixed_viewport(const VkPipelineViewportStateCreateInfo *state, struct powervr_draw_fixed *fixed);
static int fixed_dynamic(const VkPipelineDynamicStateCreateInfo *dynamic, struct powervr_draw_fixed *fixed);
static void cam_size(uint32_t outputs, uint32_t *cam, uint32_t *instances);
static uint32_t object_type(uint32_t topology);
static int rational_from_float(uint32_t bits, int64_t *mantissa, int32_t *exponent);
static int float_from_sum(uint32_t a, uint32_t b, int32_t scale_b, bool subtract, uint32_t *result);
static int32_t integer_from_float(uint32_t bits);
static uint32_t float_from_ratio(int64_t numerator, int64_t denominator);
static void viewport_bits(const VkViewport *viewport, uint32_t bits[POWERVR_DRAW_VIEWPORT_WORDS]);
static uint32_t round_units(uint32_t value, uint32_t unit);

/*
 * Keeps the fixed state of a graphics pipeline of the supported shape
 * and builds its shaders' keys: one vertex and one fragment stage,
 * line lists and strips and triangle lists, strips and fans (a point
 * list waits for the point size) without primitive restart, filled
 * polygons with or without depth bias and without clamp, one sample, one
 * viewport and scissor (static or dynamic), the depth and stencil tests
 * without bounds, blending without logic operations.  Other shapes are
 * refused (ENOTSUP); the subpass must be 0 of the pass (EINVAL).
 *
 * When the rasterizer discards every primitive, the viewport, the
 * multisample, the depth and stencil and the colour blend states are
 * not read: the specification has them ignored, so they may be absent.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_pipeline.c
 * (pvr_graphics_pipeline_compile: the vertex attribute formats and the
 * colour attachments' formats and blend as the compile's data); the
 * Vulkan 1.3 specification, VkGraphicsPipelineCreateInfo
 * (pViewportState, pMultisampleState, pDepthStencilState and
 * pColorBlendState ignored when rasterization is disabled).
 */
int
powervr_draw_fixed_build(
	const VkGraphicsPipelineCreateInfo *info,
	const struct powervr_pass *pass,
	struct powervr_draw_fixed *fixed)
{
	uint32_t index;
	bool vertex;
	bool fragment;
	int error;

	/* Refuses missing input or result, and a subpass but the first. */
	if (info == NULL || pass == NULL || fixed == NULL)
		return EINVAL;
	if (info->subpass != 0)
		return EINVAL;
	*fixed = (struct powervr_draw_fixed){ 0 };

	/* A vertex and a fragment stage only. */
	vertex = false;
	fragment = false;
	for (index = 0; index < info->stageCount; index++) {
		if (info->pStages[index].stage == VK_SHADER_STAGE_VERTEX_BIT) {
			vertex = true;
		} else if (info->pStages[index].stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
			fragment = true;
		} else {
			return ENOTSUP;
		}
	}
	if (!vertex || !fragment || info->pTessellationState != NULL)
		return ENOTSUP;

	/* The vertex input, the rasterisation, the colours and the dynamic state. */
	error = fixed_vertex(info->pVertexInputState, fixed);
	if (error != 0)
		return error;
	error = fixed_raster(info, fixed);
	if (error != 0)
		return error;
	error = fixed_fragment(info, pass, fixed);
	if (error != 0)
		return error;

	/* A rasterising pipeline's samples are its subpass's attachments' (the pass's 0 taken as 1). */
	if (!fixed->rasterizer_discard && (pass->color_count != 0 || pass->depth != POWERVR_PASS_UNUSED) &&
	    fixed->samples != (pass->samples == 4U ? 4U : 1U))
		return EINVAL;
	error = fixed_dynamic(info->pDynamicState, fixed);
	if (error != 0)
		return error;

	/*
	 * A static viewport and scissor when not dynamic; none for primitives
	 * discarded before rasterization (their state words leave the
	 * viewport out, and nothing reaches the scissor).
	 */
	if (!fixed->rasterizer_discard) {
		error = fixed_viewport(info->pViewportState, fixed);
		if (error != 0)
			return error;
	}

	/* The compile samples (the fragment's those of the draw), and the fragment shader's forms. */
	fixed->vertex_key.samples = 1;
	fixed->fragment_key.samples = fixed->samples == 4U ? 4U : 1U;
	fixed->fragment_key.forms = POWERVR_DRAW_FORMS;

	/* Points: the vertex program writes their size. */
	if (fixed->topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
		fixed->vertex_key.forms |= POWERVR_SHADER_FORM_POINT_SIZE;

	/*
	 * The front face gl_FrontFacing names: every point and line, else the
	 * triangles the core sees clockwise or counter-clockwise.
	 *
	 * Procedure referenced:
	 * Mesa 26.2.6 src/imagination/vulkan/pvr_arch_cmd_buffer.c (the front
	 * face operation of the fragment's shared words: always true for
	 * points and lines, swapped for a counter-clockwise front face).
	 */
	fixed->fragment_key.front_face = POWERVR_SHADER_FRONT_CLOCKWISE;
	if (fixed->front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE)
		fixed->fragment_key.front_face = POWERVR_SHADER_FRONT_COUNTER_CLOCKWISE;
	if (fixed->topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST || fixed->topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ||
	    fixed->topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP)
		fixed->fragment_key.front_face = POWERVR_SHADER_FRONT_ALWAYS;

	/* Succeeded: the fixed state and the keys. */
	return 0;
}

/*
 * Builds the coefficient program that iterates the fragment shader's
 * varyings, and the output select and varying words that describe the
 * vertex the vertex shader writes: w first when the fragment shader reads
 * it, then each varying the fragment reads at the place the vertex shader
 * writes it.  The vertex's varyings must come smooth before flat (the
 * order the core reads them in; ENOTSUP otherwise), and every varying the
 * fragment reads must be written (EINVAL).
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_pipeline.c
 * (pvr_graphics_pipeline_setup_fragment_coeff_program: the w iterator,
 * then one per varying with f32_offset = (the vertex output word - 2 -
 * 1 without z - 1 without w) * 2, perspective and Gouraud for smooth,
 * flat vertex 0 for flat, size 1D + count - 1, the destination its
 * coefficient set; pvr_alloc_vs_varyings: the smooth, flat and npc
 * counts); src/imagination/vulkan/pds/pvr_pds.c
 * (pvr_pds_coefficient_loading: a 64-bit constant an iterator with the
 * destination at 54 and the last one's issue bit, the data rounded to
 * four words, a DOUTI a constant, the last with the end);
 * src/imagination/vulkan/pvr_arch_cmd_buffer.c (pvr_setup_output_select:
 * rhw_pres when the fragment reads w, vtxsize the vertex outputs; the
 * f32_linear and f32_flat counts).
 */
int
powervr_draw_varyings_build(
	const struct powervr_shader_program *vertex,
	const struct powervr_shader_program *fragment,
	struct powervr_draw_varyings *varyings)
{
	const struct powervr_shader_varying *input;
	const struct powervr_shader_varying *output;
	uint64_t iterators[POWERVR_DRAW_ITERATORS];
	uint32_t smooth;
	uint32_t flat;
	uint32_t count;
	uint32_t index;
	uint32_t found;
	uint32_t source;
	bool seen_flat;
	int error;

	/* Refuses missing input or result, and stages that are not these two. */
	if (vertex == NULL || fragment == NULL || varyings == NULL)
		return EINVAL;
	if (vertex->stage != POWERVR_SHADER_VERTEX || fragment->stage != POWERVR_SHADER_FRAGMENT)
		return EINVAL;
	*varyings = (struct powervr_draw_varyings){ 0 };

	/* The vertex's varyings: smooth before flat, counted. */
	smooth = 0;
	flat = 0;
	seen_flat = false;
	for (index = 0; index < vertex->varying_count; index++) {
		output = &vertex->varyings[index];
		if (output->reg < 4U)
			return EINVAL;
		if (output->flat) {
			seen_flat = true;
			flat += output->components;
		} else if (seen_flat) {
			return ENOTSUP;
		} else {
			smooth += output->components;
		}
	}

	/* w first, when the fragment reads it. */
	count = 0;
	if (fragment->uses_w) {
		iterators[count] = DOUTI_GOURAUD;
		iterators[count] |= (uint64_t)(fragment->w_coefficient / 4U) << DOUTI_DEST_SHIFT;
		count++;
	}

	/*
	 * A point sprite's coordinates next, two words without perspective,
	 * when the fragment reads them.
	 *
	 * Procedure referenced:
	 * Mesa 26.2.6 src/imagination/vulkan/pvr_arch_pipeline.c (the
	 * fragment's coefficient program: an iterator of two words, Gouraud,
	 * point sprite, after z and w).
	 */
	if (fragment->point_coord_coefficient != POWERVR_SHADER_NO_REG) {
		iterators[count] = DOUTI_GOURAUD | DOUTI_POINT_SPRITE;
		iterators[count] |= (uint64_t)1U << DOUTI_SIZE_SHIFT;
		iterators[count] |= (uint64_t)(fragment->point_coord_coefficient / 4U) << DOUTI_DEST_SHIFT;
		count++;
	}

	/* Each varying the fragment reads, from where the vertex writes it. */
	for (index = 0; index < fragment->varying_count; index++) {
		input = &fragment->varyings[index];
		output = NULL;
		for (found = 0; found < vertex->varying_count; found++) {
			if (vertex->varyings[found].location == input->location)
				output = &vertex->varyings[found];
		}
		if (output == NULL || output->components < input->components || input->components == 0 || input->components > 4)
			return EINVAL;

		/* The word in the iterated vertex: past x and y, z unused, w when read. */
		source = output->reg - 3U;
		if (!fragment->uses_w)
			source--;
		iterators[count] = (uint64_t)(source * 2U) << DOUTI_F32_OFFSET_SHIFT;
		iterators[count] |= (uint64_t)(source * 2U) << DOUTI_F16_OFFSET_SHIFT;
		iterators[count] |= (uint64_t)(input->components - 1U) << DOUTI_SIZE_SHIFT;
		if (input->flat == 0 && input->linear != 0)
			iterators[count] |= DOUTI_GOURAUD;
		else if (input->flat == 0)
			iterators[count] |= DOUTI_PERSPECTIVE | DOUTI_GOURAUD;
		iterators[count] |= (uint64_t)(input->reg / 4U) << DOUTI_DEST_SHIFT;
		count++;
	}

	/* The program: a constant and a DOUTI an iterator, the last issuing and ending. */
	for (index = 0; index < count; index++) {
		if (index + 1U == count)
			iterators[index] |= DOUTI_ISSUE;
		varyings->data[index * 2U] = (uint32_t)iterators[index];
		varyings->data[index * 2U + 1U] = (uint32_t)(iterators[index] >> 32);
		error = powervr_pds_output(0, index + 1U == count ? 1U : 0U, POWERVR_PDS_OUTPUT_ITERATOR, index, 0, &varyings->code[index]);
		if (error != 0)
			return error;
	}
	varyings->data_words = round_units(count * 2U, 4U) * 4U;
	varyings->code_words = count;

	/* The output select, the varying words, the fragment's coefficients. */
	varyings->output_select = (vertex->vertex_outputs << 24) | (fragment->uses_w ? OUTPUT_RHW : 0U);
	if (vertex->point_size_reg != POWERVR_SHADER_NO_REG)
		varyings->output_select |= OUTPUT_POINT_SIZE;
	varyings->varying[0] = smooth | (flat << 8);
	varyings->varying[1] = 0;
	varyings->usc_varying_size = round_units(fragment->coefficients, 16U);

	/* Succeeded: the varyings. */
	return 0;
}

/*
 * The viewport transform's words: x and y centres and half extents,
 * minDepth and the depth range, each the float nearest the exact value
 * (as Mesa computes them in single precision, one rounding each).
 * Refuses values far apart in magnitude (ENOTSUP).
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_setup_viewport: m0 = width * 0.5, a0 = x + width * 0.5, the same
 * for y, m2 = maxDepth - minDepth, a2 = minDepth; written a0, m0, a1, m1,
 * a2, m2).
 */
int
powervr_draw_viewport_words(
	const VkViewport *viewport,
	uint32_t words[POWERVR_DRAW_VIEWPORT_WORDS])
{
	uint32_t bits[POWERVR_DRAW_VIEWPORT_WORDS];
	int error;

	/* Refuses missing input or result. */
	if (viewport == NULL || words == NULL)
		return EINVAL;
	viewport_bits(viewport, bits);

	/* x: the centre and the half width. */
	error = float_from_sum(bits[0], bits[2], -1, false, &words[0]);
	if (error != 0)
		return error;
	error = float_from_sum(0, bits[2], -1, false, &words[1]);
	if (error != 0)
		return error;

	/* y: the centre and the half height. */
	error = float_from_sum(bits[1], bits[3], -1, false, &words[2]);
	if (error != 0)
		return error;
	error = float_from_sum(0, bits[3], -1, false, &words[3]);
	if (error != 0)
		return error;

	/* Depth: the near value and the range. */
	words[4] = bits[4];
	error = float_from_sum(bits[5], bits[4], 0, true, &words[5]);
	if (error != 0)
		return error;

	/* Succeeded: the six words. */
	return 0;
}

/*
 * The scissor's two words (the overlap of the viewport and the scissor)
 * and the region clip that keeps the tiles outside it away.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_get_viewport_scissor_overlap: the viewport's corners truncated to
 * integers, a negative height flipped, an empty overlap when they miss;
 * pvr_setup_isp_depth_bias_scissor_state: IPF_SCISSOR_WORD_0 xmax 0..15,
 * xmin 16..31, WORD_1 ymax and ymin the same, the region clip in tiles
 * of 16 with right and bottom the last tile, OUTSIDE).
 */
int
powervr_draw_scissor_words(
	const VkViewport *viewport,
	const VkRect2D *scissor,
	uint32_t words[POWERVR_DRAW_SCISSOR_WORDS],
	uint32_t clip[2])
{
	uint32_t bits[POWERVR_DRAW_VIEWPORT_WORDS];
	int64_t view_x0;
	int64_t view_y0;
	int64_t view_x1;
	int64_t view_y1;
	int64_t x0;
	int64_t y0;
	int64_t x1;
	int64_t y1;
	int32_t height;
	uint32_t right;
	uint32_t bottom;

	/* Refuses missing input or result, and a negative scissor. */
	if (viewport == NULL || scissor == NULL || words == NULL || clip == NULL)
		return EINVAL;
	if (scissor->offset.x < 0 || scissor->offset.y < 0)
		return EINVAL;
	viewport_bits(viewport, bits);

	/* The viewport's rectangle, a negative height flipped. */
	view_x0 = integer_from_float(bits[0]);
	view_x1 = view_x0 + integer_from_float(bits[2]);
	height = integer_from_float(bits[3]);
	view_y0 = (int64_t)integer_from_float(bits[1]) + (height < 0 ? height : 0);
	view_y1 = (int64_t)integer_from_float(bits[1]) + (height > 0 ? height : 0);

	/* The overlap, empty when they miss or the scissor is empty. */
	x0 = scissor->offset.x;
	y0 = scissor->offset.y;
	x1 = x0 + scissor->extent.width;
	y1 = y0 + scissor->extent.height;
	if (scissor->extent.width == 0 || scissor->extent.height == 0 || x1 <= view_x0 || y1 <= view_y0 || x0 >= view_x1 || y0 >= view_y1) {
		x0 = 0;
		y0 = 0;
		x1 = 0;
		y1 = 0;
	} else {
		x0 = x0 > view_x0 ? x0 : view_x0;
		y0 = y0 > view_y0 ? y0 : view_y0;
		x1 = x1 < view_x1 ? x1 : view_x1;
		y1 = y1 < view_y1 ? y1 : view_y1;
	}
	if (x1 > 0xffff || y1 > 0xffff)
		return E2BIG;

	/* The words. */
	words[0] = ((uint32_t)x0 << 16) | (uint32_t)x1;
	words[1] = ((uint32_t)y0 << 16) | (uint32_t)y1;

	/* The region clip, in tiles. */
	right = 0;
	if (x1 != 0)
		right = (uint32_t)((x1 + REGION_TILE - 1) / REGION_TILE) - 1U;
	bottom = 0;
	if (y1 != 0)
		bottom = (uint32_t)((y1 + REGION_TILE - 1) / REGION_TILE) - 1U;
	clip[0] = CLIP_OUTSIDE | ((uint32_t)(x0 / REGION_TILE) << 16) | right;
	clip[1] = ((uint32_t)(y0 / REGION_TILE) << 16) | bottom;

	/* Succeeded: the scissor. */
	return 0;
}

/*
 * Packs a draw's primitive state block, the whole state for every draw:
 * the ISP control and front face words, the scissor index, the
 * fragment's data sequencer state (its task, its shared register program
 * and the sizes, its coefficient program, its shared register data), the
 * region clip, the viewport, the w clamp, the output select, the varying
 * words, the control and no stream output.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_emit_ppp_state: the header and the words in that order;
 * pvr_setup_isp_faces_and_control: ISPCTL scenable, ISPA objtype,
 * passtype, dcmpmode and dwritedisable from the depth state optimised
 * for the attachment (no depth attachment: always, no write), the line
 * width, no ISPB without stencil; pvr_setup_fragment_state_pointers:
 * SHADERBASE, TEXUNICODEBASE, SIZEINFO1 (uniform, varying and USC
 * varying sizes, no temporaries: the programs take none), SIZEINFO2
 * the shared registers, VARYINGBASE only with a coefficient program,
 * UNIFORMDATABASE; pvr_setup_ppp_control: drawclippededges,
 * wclampen, flat vertex 0, front and rear clipping, the cull mode by the
 * front face); src/imagination/vulkan/pvr_arch_pipeline.c (the pass type
 * opaque, translucent when the fragment shader blends;
 * pvr_fragment_state_save: punch-through when it discards, before the
 * others; depth feedback when it writes its depth, before punch-through).
 */
int
powervr_draw_ppp_build(
	const struct powervr_draw_ppp *input,
	uint32_t words[POWERVR_DRAW_PPP_WORDS],
	uint32_t *count)
{
	const struct powervr_draw_fixed *fixed;
	uint32_t header;
	uint32_t ispa;
	uint32_t front_a;
	uint32_t front_b;
	uint32_t back_a;
	uint32_t back_b;
	uint32_t swap;
	uint32_t isp_control;
	uint32_t compare;
	uint32_t control;
	uint32_t index;
	uint32_t uniform;
	uint32_t varying;
	uint32_t shared;
	uint32_t merge;
	bool ccw_front;

	/* Refuses missing input or result, and programs not on 16-byte boundaries. */
	if (input == NULL || input->fixed == NULL || input->varyings == NULL || words == NULL || count == NULL)
		return EINVAL;
	if (((input->fragment_kick_offset | input->shared_code_offset | input->shared_data_offset | input->coefficient_offset) & ~OFFSET_MASK) != 0)
		return EINVAL;
	fixed = input->fixed;

	/* Refuses sizes the fields cannot hold. */
	uniform = round_units(input->shared_data_words, 4U);
	varying = round_units(input->varyings->data_words, 4U);
	shared = round_units(input->fragment_shareds, 16U);
	if (uniform > FIELD_9_MAX || varying > FIELD_6_MAX || input->varyings->usc_varying_size > FIELD_6_MAX || shared > FIELD_9_MAX)
		return EINVAL;

	/* The header: everything present, one viewport. */
	header = HEADER_ISPCTL | HEADER_ISPCTL_FA | HEADER_ISPCTL_DBSC | HEADER_PDS_0 | HEADER_PDS_1 | HEADER_PDS_3 |
		 HEADER_REGION_CLIP | HEADER_VIEWPORT | HEADER_WCLAMP | HEADER_OUTSELECTS | HEADER_VARYING_0 |
		 HEADER_VARYING_1 | HEADER_VARYING_2 | HEADER_PPP_CTRL | HEADER_STREAM_OUT;

	/*
	 * No varying base without a coefficient program, as Mesa leaves
	 * pres_pds_state_ptr1 out (on the board, with this, no data sequencer
	 * temporaries and pixel merging off, each extra full-screen draw cost
	 * about 0.02 ms where it had cost 1.1 ms).
	 */
	if (input->varyings->code_words == 0)
		header &= ~HEADER_PDS_1;

	/* No viewport for primitives discarded before rasterization. */
	if (fixed->rasterizer_discard)
		header &= ~HEADER_VIEWPORT;

	/* The depth test as the attachment allows: none without one, nor for discarded primitives. */
	compare = VK_COMPARE_OP_ALWAYS;
	ispa = ISPA_NO_DEPTH_WRITE;
	if (input->has_depth && fixed->depth_test && !fixed->rasterizer_discard) {
		compare = fixed->depth_compare;
		if (fixed->depth_write)
			ispa = 0;
	}
	ispa |= compare << ISPA_COMPARE_SHIFT;
	ispa |= LINE_WIDTH_ONE << ISPA_LINE_WIDTH_SHIFT;
	ispa |= object_type(fixed->topology) << ISPA_OBJECT_SHIFT;
	if (fixed->depth_feedback) {
		ispa |= ISPA_DEPTH_FEEDBACK;
	} else if (fixed->discards) {
		ispa |= ISPA_PUNCH_THROUGH;
	} else if (fixed->blending) {
		ispa |= ISPA_TRANSLUCENT;
	} else if ((POWERVR_DRAW_TRIAL & POWERVR_DRAW_TRIAL_PUNCH_THROUGH) != 0U) {
		ispa |= ISPA_PUNCH_THROUGH;
	}

	/* The faces' words: the same for both without a stencil test on a stencil target. */
	front_a = ispa;
	back_a = ispa;
	front_b = ISPB_STENCIL_OFF;
	back_b = ISPB_STENCIL_OFF;
	if (input->has_depth && input->has_stencil && fixed->stencil_test && !fixed->rasterizer_discard)
		isp_stencil(input, &front_a, &front_b, &back_a, &back_b);

	/*
	 * Faces that differ: one face's words when the other is culled, both
	 * faces' when neither is (the back's first for a counter-clockwise
	 * front, the hardware's front being clockwise).
	 */
	isp_control = ISPCTL_SCISSOR;
	if (front_a != back_a || front_b != back_b) {
		if (fixed->cull_mode == VK_CULL_MODE_BACK_BIT) {
			/* The front's words alone. */
		} else if (fixed->cull_mode == VK_CULL_MODE_FRONT_BIT) {
			front_a = back_a;
			front_b = back_b;
		} else {
			header |= HEADER_ISPCTL_BA;
			isp_control |= ISPCTL_TWO_SIDED;
			if (fixed->front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE) {
				swap = front_a;
				front_a = back_a;
				back_a = swap;
				swap = front_b;
				front_b = back_b;
				back_b = swap;
			}
			if (back_b != ISPB_STENCIL_OFF)
				header |= HEADER_ISPCTL_FB | HEADER_ISPCTL_BB;
		}
	}
	if (front_b != ISPB_STENCIL_OFF)
		header |= HEADER_ISPCTL_FB;
	if ((header & (HEADER_ISPCTL_FB | HEADER_ISPCTL_BB)) != 0)
		isp_control |= ISPCTL_B_PRESENT;

	/* The depth bias, for the triangles of a draw with a depth target. */
	if (fixed->depth_bias && input->has_depth && object_type(fixed->topology) == OBJECT_TRIANGLE)
		isp_control |= ISPCTL_DEPTH_BIAS;

	/* The visibility register an occlusion query counts the draw's samples in. */
	if (input->visibility != 0)
		isp_control |= ISPCTL_VISIBILITY_TEST | ((input->visibility - 1U) & ISPCTL_VISIBILITY_MASK);

	/* Primitives discarded before rasterization: no tag written, no scissor, no bias, nothing counted. */
	if (fixed->rasterizer_discard)
		isp_control = (isp_control & ~(ISPCTL_SCISSOR | ISPCTL_DEPTH_BIAS | ISPCTL_VISIBILITY_TEST | ISPCTL_VISIBILITY_MASK)) | ISPCTL_TAG_WRITE_DISABLE;

	/* The ISP words, the depth bias and scissor indices. */
	index = 0;
	words[index++] = header;
	words[index++] = isp_control;
	words[index++] = front_a;
	if ((header & HEADER_ISPCTL_FB) != 0)
		words[index++] = front_b;
	if ((header & HEADER_ISPCTL_BA) != 0)
		words[index++] = back_a;
	if ((header & HEADER_ISPCTL_BB) != 0)
		words[index++] = back_b;
	words[index++] = ((input->depth_bias_index & 0xffffU) << 16) | (input->scissor_index & 0xffffU);

	/* The fragment's data sequencer state. */
	words[index++] = input->fragment_kick_offset;
	words[index++] = input->shared_code_offset;
	words[index++] = (uniform << 23) | (varying << 10) | (input->varyings->usc_varying_size << 4);
	merge = 0;
	if ((ispa & ISPA_NO_DEPTH_WRITE) != 0 && compare == VK_COMPARE_OP_ALWAYS)
		merge = SIZEINFO2_TRI_MERGE_DISABLE;
	if ((ispa & ISPA_PASS_MASK) == ISPA_PUNCH_THROUGH)
		merge = SIZEINFO2_TRI_MERGE_DISABLE;
	if (object_type(fixed->topology) == OBJECT_LINE)
		merge = SIZEINFO2_TRI_MERGE_DISABLE;
	words[index++] = (shared << 23) | merge;
	if (input->varyings->code_words != 0)
		words[index++] = input->coefficient_offset;
	words[index++] = input->shared_data_offset;

	/* The region clip and the viewport (none when the header leaves it out). */
	words[index++] = input->region_clip[0];
	words[index++] = input->region_clip[1];
	if ((header & HEADER_VIEWPORT) != 0) {
		words[index++] = input->viewport[0];
		words[index++] = input->viewport[1];
		words[index++] = input->viewport[2];
		words[index++] = input->viewport[3];
		words[index++] = input->viewport[4];
		words[index++] = input->viewport[5];
	}

	/* The w clamp, the output select and the varying words. */
	words[index++] = WCLAMP_VALUE;
	words[index++] = input->varyings->output_select;
	words[index++] = input->varyings->varying[0];
	words[index++] = input->varyings->varying[1];
	words[index++] = 0;

	/* The control: the cull mode by the front face, for triangles only (a point or line has no face). */
	control = CTRL_CLIPPED_EDGES | CTRL_WCLAMP | CTRL_FLAT_VERTEX_0 | CTRL_FRONT_REAR;
	ccw_front = fixed->front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE;
	if (object_type(fixed->topology) != OBJECT_TRIANGLE) {
		/* No cull. */
	} else if (fixed->cull_mode == VK_CULL_MODE_BACK_BIT || fixed->cull_mode == VK_CULL_MODE_FRONT_BIT) {
		if (ccw_front != (fixed->cull_mode == VK_CULL_MODE_FRONT_BIT)) {
			control |= CTRL_CULL_CW;
		} else {
			control |= CTRL_CULL_CCW;
		}
	}
	words[index++] = control;

	/* No stream output. */
	words[index++] = 0;

	/* Succeeded: the block and its words. */
	*count = index;
	return 0;
}

/*
 * Packs a draw's vertex control stream: the primitive state block, the
 * vertex shader's shared register program when it reads shared
 * registers, the vertex state, and the index list (with an index buffer
 * when indexed, the instances past one and the vertex offset).
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_emit_ppp_state: PPP_STATE0 and PPP_STATE1; pvr_emit_dirty_pds_state:
 * PDS_STATE0 usc_target ALL with the common size and the data size,
 * PDS_STATE1 the data with no shared data loader, PDS_STATE2 the code;
 * pvr_emit_dirty_vdm_state: VDM_STATE0 with the cam size and both
 * present, VDM_STATE2 the attribute data, VDM_STATE3 its code, VDM_STATE4
 * the outputs, VDM_STATE5 the instances, the unified store of the inputs
 * (four bytes a register), the temporaries (four bytes each) and the data
 * size; pvr_emit_vdm_index_list: the count always, the instances when
 * more than one (less one), the index offset when not zero, the index
 * buffer's size and address when indexed).
 */
int
powervr_draw_vdm_build(
	const struct powervr_draw_vdm *input,
	uint32_t words[POWERVR_DRAW_VDM_WORDS],
	uint32_t *count)
{
	uint32_t cam;
	uint32_t instances;
	uint32_t unified;
	uint32_t temps;
	uint32_t data;
	uint32_t list;
	uint32_t index;
	uint32_t shared_units;
	uint32_t data_units;

	/* Refuses missing input or result, misaligned programs and blocks, and topologies the core list does not take. */
	if (input == NULL || words == NULL || count == NULL)
		return EINVAL;
	if (((input->vertex_data_offset | input->vertex_code_offset | input->shared_code_offset | input->shared_data_offset) & ~OFFSET_MASK) != 0)
		return EINVAL;
	if ((input->ppp_address & 3U) != 0 || input->ppp_address >= (1ULL << 40) || input->ppp_words == 0 || input->ppp_words > 0xffU)
		return EINVAL;
	switch (input->topology) {
	case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
		list = LIST_POINT_LIST;
		break;
	case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
		list = LIST_LINE_LIST;
		break;
	case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
		list = LIST_LINE_STRIP;
		break;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
		list = LIST_TRIANGLE_LIST;
		break;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
		list = LIST_TRIANGLE_STRIP;
		break;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
		list = LIST_TRIANGLE_FAN;
		break;
	default:
		return ENOTSUP;
	}
	if (input->count == 0 || input->instances == 0)
		return EINVAL;

	/* Refuses sizes the fields cannot hold. */
	unified = round_units(input->vertex_inputs * 4U, UNIT_16);
	temps = round_units(input->vertex_temps * 4U, UNIT_16);
	data = round_units(input->vertex_data_words * 4U, UNIT_16);
	if (unified > FIELD_6_MAX || temps > FIELD_4_MAX || data > FIELD_6_MAX)
		return EINVAL;
	cam_size(input->vertex_outputs, &cam, &instances);

	/* The primitive state block. */
	index = 0;
	words[index++] = (BLOCK_PPP << BLOCK_SHIFT) | (input->ppp_words << 8) | (uint32_t)((input->ppp_address >> 32) & 0xffU);
	words[index++] = (uint32_t)input->ppp_address & ~3U;

	/* The vertex shader's shared register program, when it has one. */
	if (input->vertex_shareds != 0) {
		shared_units = round_units(input->vertex_shareds * 4U, UNIT_64);
		data_units = round_units(input->shared_data_words * 4U, UNIT_16);
		if (shared_units > FIELD_9_MAX || data_units > FIELD_6_MAX)
			return EINVAL;
		words[index++] = (BLOCK_PDS << BLOCK_SHIFT) | (shared_units << 16) | data_units;
		words[index++] = input->shared_data_offset;
		words[index++] = input->shared_code_offset;
	}

	/* The vertex state. */
	words[index++] = (BLOCK_VDM << BLOCK_SHIFT) | VDM_DATA_PRESENT | VDM_OTHER_PRESENT | (cam << 7);
	words[index++] = input->vertex_data_offset;
	words[index++] = input->vertex_code_offset;
	words[index++] = input->vertex_outputs;
	words[index++] = (instances << 25) | (unified << 10) | (temps << 6) | data;

	/* The index list's header. */
	list |= (BLOCK_INDEX << BLOCK_SHIFT) | LIST_COUNT;
	if (input->instances > 1)
		list |= LIST_INSTANCES;
	if (input->vertex_offset != 0)
		list |= LIST_OFFSET;
	if (input->indexed) {
		if (input->index_bytes == 2) {
			list |= LIST_SIZE_16;
		} else if (input->index_bytes == 4) {
			list |= LIST_SIZE_32;
		} else {
			return EINVAL;
		}
		if ((input->index_address & (input->index_bytes - 1U)) != 0 || input->index_address >= (1ULL << 40))
			return EINVAL;
		list |= LIST_ADDRESS | (uint32_t)((input->index_address >> 32) & 0xffU);
	}
	words[index++] = list;

	/* Its words: the index buffer, the count, the instances, the offset. */
	if (input->indexed)
		words[index++] = (uint32_t)input->index_address;
	words[index++] = input->count;
	if (input->instances > 1)
		words[index++] = input->instances - 1U;
	if (input->vertex_offset != 0)
		words[index++] = input->vertex_offset;

	/* Succeeded: the stream and its words. */
	*count = index;
	return 0;
}

/* The rasterisation, multisample, input assembly and depth state. */
static int
fixed_raster(
	const VkGraphicsPipelineCreateInfo *info,
	struct powervr_draw_fixed *fixed)
{
	const VkPipelineRasterizationStateCreateInfo *raster;
	const VkPipelineMultisampleStateCreateInfo *multisample;
	const VkPipelineDepthStencilStateCreateInfo *depth;

	/*
	 * Point lists (the vertex program writes each point's size, the
	 * sprite's), line lists and strips, and triangle lists, strips and
	 * fans, without restart.
	 */
	if (info->pInputAssemblyState == NULL || info->pInputAssemblyState->primitiveRestartEnable != VK_FALSE)
		return ENOTSUP;
	fixed->topology = info->pInputAssemblyState->topology;
	switch (fixed->topology) {
	case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
	case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
	case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
		break;
	default:
		return ENOTSUP;
	}

	/* Filled polygons, culled or not, without clamp; discarded before rasterization or not. */
	raster = info->pRasterizationState;
	if (raster == NULL || raster->polygonMode != VK_POLYGON_MODE_FILL || raster->depthClampEnable != VK_FALSE)
		return ENOTSUP;
	fixed->rasterizer_discard = raster->rasterizerDiscardEnable != VK_FALSE;
	if (raster->cullMode == VK_CULL_MODE_FRONT_AND_BACK)
		return ENOTSUP;
	fixed->cull_mode = raster->cullMode;
	fixed->front_face = raster->frontFace;

	/* The depth bias and its factors, as their float bits. */
	fixed->depth_bias = raster->depthBiasEnable != VK_FALSE;
	__builtin_memcpy(&fixed->depth_bias_factors[0], &raster->depthBiasConstantFactor, sizeof(fixed->depth_bias_factors[0]));
	__builtin_memcpy(&fixed->depth_bias_factors[1], &raster->depthBiasClamp, sizeof(fixed->depth_bias_factors[1]));
	__builtin_memcpy(&fixed->depth_bias_factors[2], &raster->depthBiasSlopeFactor, sizeof(fixed->depth_bias_factors[2]));

	/* Nothing past rasterization for primitives that never reach it: no sample count, no depth or stencil test. */
	if (fixed->rasterizer_discard)
		return 0;

	/*
	 * One sample, or four (a render of four samples' draws, each sample's
	 * fragment run on its own); sample shading is then what every draw
	 * does.  Four samples with only some of them in the sample mask, or
	 * with the alpha to coverage or to one, are not taken.
	 *
	 * Procedure referenced: Mesa 26.2.6 src/imagination/vulkan/
	 * pvr_arch_cmd_buffer.c (the fragment program's DOUTU sample rate
	 * FULL for more than one rasterization sample; a partial sample mask
	 * through a pass-through program, pvr_needs_fs_passthrough) and
	 * pvr_arch_pipeline.c (alpha to coverage in the fragment program).
	 */
	fixed->samples = 1;
	multisample = info->pMultisampleState;
	if (multisample != NULL && multisample->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT) {
		if (multisample->rasterizationSamples != VK_SAMPLE_COUNT_4_BIT)
			return ENOTSUP;
		if (multisample->pSampleMask != NULL && (multisample->pSampleMask[0] & 0xfU) != 0xfU)
			return ENOTSUP;
		if (multisample->alphaToCoverageEnable != VK_FALSE || multisample->alphaToOneEnable != VK_FALSE)
			return ENOTSUP;
		fixed->samples = 4;
	}

	/* The depth and stencil tests, no bounds. */
	depth = info->pDepthStencilState;
	if (depth != NULL) {
		if (depth->depthBoundsTestEnable != VK_FALSE)
			return ENOTSUP;
		fixed->depth_test = depth->depthTestEnable != VK_FALSE;
		fixed->depth_write = depth->depthWriteEnable != VK_FALSE;
		fixed->depth_compare = depth->depthCompareOp;
		if (!fixed->depth_test)
			fixed->depth_write = false;
		fixed->stencil_test = depth->stencilTestEnable != VK_FALSE;
		fixed_stencil_face(&depth->front, &fixed->stencil[0]);
		fixed_stencil_face(&depth->back, &fixed->stencil[1]);
	}

	/* Succeeded: the state. */
	return 0;
}

/* Keeps one face's stencil state. */
static void
fixed_stencil_face(
	const VkStencilOpState *state,
	struct powervr_draw_stencil_face *face)
{
	/* The operations, the comparison, the masks and the reference. */
	face->fail_op = state->failOp;
	face->pass_op = state->passOp;
	face->depth_fail_op = state->depthFailOp;
	face->compare_op = state->compareOp;
	face->compare_mask = state->compareMask & 0xffU;
	face->write_mask = state->writeMask & 0xffU;
	face->reference = state->reference & 0xffU;
}

/* The vertex bindings and attributes, and the vertex key's formats. */
static int
fixed_vertex(
	const VkPipelineVertexInputStateCreateInfo *input,
	struct powervr_draw_fixed *fixed)
{
	const VkVertexInputAttributeDescription *attribute;
	uint32_t index;
	const struct powervr_format *format;

	/* No vertex input is no binding and no attribute. */
	if (input == NULL)
		return 0;
	if (input->vertexBindingDescriptionCount > POWERVR_DRAW_BINDINGS || input->vertexAttributeDescriptionCount > POWERVR_DRAW_ATTRIBUTES)
		return ENOTSUP;

	/* Bindings stepped per vertex or per instance (a divisor of one). */
	for (index = 0; index < input->vertexBindingDescriptionCount; index++) {
		if (input->pVertexBindingDescriptions[index].inputRate != VK_VERTEX_INPUT_RATE_VERTEX &&
		    input->pVertexBindingDescriptions[index].inputRate != VK_VERTEX_INPUT_RATE_INSTANCE)
			return ENOTSUP;
		fixed->bindings[index].binding = input->pVertexBindingDescriptions[index].binding;
		fixed->bindings[index].stride = input->pVertexBindingDescriptions[index].stride;
		fixed->bindings[index].instance = input->pVertexBindingDescriptions[index].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE;
	}
	fixed->binding_count = input->vertexBindingDescriptionCount;

	/* Attributes of a vertex format the executor takes (one it reports for vertex buffers), the format in the key. */
	for (index = 0; index < input->vertexAttributeDescriptionCount; index++) {
		attribute = &input->pVertexAttributeDescriptions[index];
		if (attribute->location >= POWERVR_SHADER_INPUTS)
			return ENOTSUP;
		format = powervr_format_find(attribute->format);
		if (format == NULL || (format->buffer_features & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) == 0)
			return ENOTSUP;
		fixed->attributes[index].location = attribute->location;
		fixed->attributes[index].binding = attribute->binding;
		fixed->attributes[index].format = attribute->format;
		fixed->attributes[index].offset = attribute->offset;
		fixed->vertex_key.attribute_formats[attribute->location] = attribute->format;
	}
	fixed->attribute_count = input->vertexAttributeDescriptionCount;

	/* Succeeded: the vertex input. */
	return 0;
}

/* The one viewport and scissor, each kept unless the command buffer sets it. */
static int
fixed_viewport(
	const VkPipelineViewportStateCreateInfo *state,
	struct powervr_draw_fixed *fixed)
{
	/* One viewport and one scissor. */
	if (state == NULL || state->viewportCount != 1 || state->scissorCount != 1)
		return ENOTSUP;

	/* The static viewport, then the static scissor. */
	if (!fixed->dynamic_viewport) {
		if (state->pViewports == NULL)
			return EINVAL;
		__builtin_memcpy(&fixed->viewport, &state->pViewports[0], sizeof(fixed->viewport));
	}
	if (!fixed->dynamic_scissor) {
		if (state->pScissors == NULL)
			return EINVAL;
		fixed->scissor = state->pScissors[0];
	}

	/* Succeeded: the viewport and the scissor. */
	return 0;
}

/*
 * The colour attachments' formats and blend in the fragment key.  A draw
 * that blends, or that leaves a channel of an attachment unwritten (a
 * write mask short of the format's channels, none for an attachment the
 * draw buffers leave out), passes translucent: an opaque one would hide
 * the earlier objects of its pixels, and with them what they wrote to
 * the channels it keeps.
 *
 * Procedure referenced: Mesa 26.2.6 src/imagination/vulkan/
 * pvr_arch_pipeline.c (pvr_fragment_state_save: a fragment shader that
 * reads its output, as a partial write mask makes it, passes
 * translucent).
 */
static int
fixed_fragment(
	const VkGraphicsPipelineCreateInfo *info,
	const struct powervr_pass *pass,
	struct powervr_draw_fixed *fixed)
{
	const VkPipelineColorBlendStateCreateInfo *blend;
	const VkPipelineColorBlendAttachmentState *attachment;
	struct powervr_shader_blend *key;
	uint32_t channels;
	uint32_t index;

	/*
	 * One blend attachment a colour of the subpass, no logic operation;
	 * no blend state read for primitives discarded before rasterization
	 * (each colour then unblended with every channel, as no fragment
	 * reaches it).
	 */
	blend = NULL;
	if (!fixed->rasterizer_discard)
		blend = info->pColorBlendState;
	if (!fixed->rasterizer_discard && pass->color_count != 0 && (blend == NULL || blend->attachmentCount != pass->color_count))
		return EINVAL;
	if (blend != NULL && blend->logicOpEnable != VK_FALSE)
		return ENOTSUP;
	if (pass->color_count > POWERVR_SHADER_OUTPUTS)
		return ENOTSUP;

	/* Each colour's format and blend. */
	for (index = 0; index < pass->color_count; index++) {
		if (pass->colors[index] == POWERVR_PASS_UNUSED)
			continue;
		fixed->fragment_key.output_formats[index] = pass->attachments[pass->colors[index]].format;
		key = &fixed->fragment_key.blend[index];
		if (blend == NULL) {
			key->write_mask = 0xfU;
			continue;
		}
		attachment = &blend->pAttachments[index];
		key->enable = attachment->blendEnable != VK_FALSE;
		key->source_color = (uint8_t)attachment->srcColorBlendFactor;
		key->destination_color = (uint8_t)attachment->dstColorBlendFactor;
		key->color_operation = (uint8_t)attachment->colorBlendOp;
		key->source_alpha = (uint8_t)attachment->srcAlphaBlendFactor;
		key->destination_alpha = (uint8_t)attachment->dstAlphaBlendFactor;
		key->alpha_operation = (uint8_t)attachment->alphaBlendOp;
		key->write_mask = (uint8_t)(attachment->colorWriteMask & 0xfU);
		if (key->enable)
			fixed->blending = true;
		channels = format_channel_mask(fixed->fragment_key.output_formats[index]);
		if ((key->write_mask & channels) != channels)
			fixed->blending = true;
	}

	/* The blend constants a factor may name, as their float bits. */
	if (blend != NULL) {
		for (index = 0; index < 4U; index++)
			__builtin_memcpy(&fixed->blend_constants[index], &blend->blendConstants[index], sizeof(fixed->blend_constants[index]));
	}

	/* Succeeded: the colours. */
	return 0;
}

/* The mask of a colour format's channels (red 1 to alpha 8) a write mask covers; 0xf for a format not in the table. */
static uint32_t
format_channel_mask(
	uint32_t vk_format)
{
	const struct powervr_format *format;
	uint32_t mask;
	unsigned channel;

	/* Every channel of a format the table lacks. */
	format = powervr_format_find(vk_format);
	if (format == NULL)
		return 0xfU;

	/* Each output channel the format stores (not a constant zero or one). */
	mask = 0;
	for (channel = 0; channel < 4U; channel++) {
		if (format->channels[channel] <= POWERVR_CHANNEL_3)
			mask |= 1U << channel;
	}

	/* Succeeded: the mask. */
	return mask;
}

/*
 * The dynamic states: the viewport, the scissor, the blend constants,
 * the depth bias and the stencil masks and references come from the
 * command buffer; the line width is taken and ignored, since every line
 * is one pixel wide (the device reports no wide lines).  The depth
 * bounds and the extended states are refused.
 */
static int
fixed_dynamic(
	const VkPipelineDynamicStateCreateInfo *dynamic,
	struct powervr_draw_fixed *fixed)
{
	uint32_t index;
	VkDynamicState state;

	/* No dynamic state. */
	if (dynamic == NULL)
		return 0;

	/* Each one, refused when the executor does not take it. */
	for (index = 0; index < dynamic->dynamicStateCount; index++) {
		state = dynamic->pDynamicStates[index];
		switch (state) {
		case VK_DYNAMIC_STATE_VIEWPORT:
			fixed->dynamic_viewport = true;
			break;
		case VK_DYNAMIC_STATE_SCISSOR:
			fixed->dynamic_scissor = true;
			break;
		case VK_DYNAMIC_STATE_LINE_WIDTH:
			break;
		case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
			fixed->dynamic_blend_constants = true;
			break;
		case VK_DYNAMIC_STATE_DEPTH_BIAS:
			fixed->dynamic_depth_bias = true;
			break;
		case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
			fixed->dynamic_stencil_compare_mask = true;
			break;
		case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:
			fixed->dynamic_stencil_write_mask = true;
			break;
		case VK_DYNAMIC_STATE_STENCIL_REFERENCE:
			fixed->dynamic_stencil_reference = true;
			break;
		default:
			return ENOTSUP;
		}
	}

	/* Succeeded: the dynamic states. */
	return 0;
}

/*
 * Works out each face's ISPA and ISPB words of a stencil test: the
 * reference in the ISPA beside the depth state, the comparison, the
 * operations and the masks in the ISPB; the masks and references the
 * command buffer's where the pipeline made them dynamic.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_setup_isp_faces_and_control: front_a and back_a with sref, front_b
 * and back_b with swmask, scmpmask, sop3 the pass, sop2 the depth fail,
 * sop1 the fail and scmpmode the comparison).
 */
static void
isp_stencil(
	const struct powervr_draw_ppp *input,
	uint32_t *front_a,
	uint32_t *front_b,
	uint32_t *back_a,
	uint32_t *back_b)
{
	const struct powervr_draw_fixed *fixed;
	const struct powervr_draw_stencil_face *face;
	uint32_t reference;
	uint32_t compare_mask;
	uint32_t write_mask;
	uint32_t a[2];
	uint32_t b[2];
	uint32_t side;

	/* Each face from its own state. */
	fixed = input->fixed;
	for (side = 0; side < 2U; side++) {
		face = &fixed->stencil[side];

		/* The masks and the reference: the pipeline's, or the command buffer's. */
		reference = face->reference;
		compare_mask = face->compare_mask;
		write_mask = face->write_mask;
		if (input->dynamic != NULL && fixed->dynamic_stencil_reference)
			reference = input->dynamic->stencil_reference[side] & 0xffU;
		if (input->dynamic != NULL && fixed->dynamic_stencil_compare_mask)
			compare_mask = input->dynamic->stencil_compare_mask[side] & 0xffU;
		if (input->dynamic != NULL && fixed->dynamic_stencil_write_mask)
			write_mask = input->dynamic->stencil_write_mask[side] & 0xffU;

		/* The words. */
		a[side] = (side == 0 ? *front_a : *back_a) | reference;
		b[side] = (face->compare_op & 7U) << ISPB_COMPARE_SHIFT;
		b[side] |= (face->fail_op & 7U) << ISPB_FAIL_SHIFT;
		b[side] |= (face->depth_fail_op & 7U) << ISPB_DEPTH_FAIL_SHIFT;
		b[side] |= (face->pass_op & 7U) << ISPB_PASS_SHIFT;
		b[side] |= compare_mask << ISPB_COMPARE_MASK_SHIFT;
		b[side] |= write_mask;
	}

	/* The faces' words. */
	*front_a = a[0];
	*front_b = b[0];
	*back_a = a[1];
	*back_b = b[1];
}

/* The ISPA object type of a primitive topology: a sprite for points, a line for lines, else a triangle. */
static uint32_t
object_type(
	uint32_t topology)
{
	/* Each kind of primitive. */
	switch (topology) {
	case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
		return OBJECT_SPRITE;
	case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
	case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
		return OBJECT_LINE;
	default:
		break;
	}

	/* The triangles. */
	return OBJECT_TRIANGLE;
}

/* The BXE-4-32's vertex cache entries and task instances for a vertex of outputs words. */
static void
cam_size(
	uint32_t outputs,
	uint32_t *cam,
	uint32_t *instances)
{
	uint32_t vector;

	/* The vertex vector with its scratch, in words, by band. */
	vector = (outputs + 1U + 4U) * 4U;
	if (vector < 14U * 4U) {
		*cam = 31;
		*instances = 16;
	} else if (vector < 20U * 4U) {
		*cam = 15;
		*instances = 16;
	} else if (vector < 28U * 4U) {
		*cam = 11;
		*instances = 12;
	} else if (vector < 44U * 4U) {
		*cam = 7;
		*instances = 8;
	} else {
		*cam = 7;
		*instances = 4;
	}
}

/* A float's value as mantissa * 2^exponent; refuses NaN and infinity. */
static int
rational_from_float(
	uint32_t bits,
	int64_t *mantissa,
	int32_t *exponent)
{
	uint32_t field;

	/* The exponent field: all ones is not a number. */
	field = (bits >> 23) & 0xffU;
	if (field == 0xffU)
		return EINVAL;

	/* Normal and subnormal values. */
	*mantissa = (int64_t)(bits & 0x7fffffU);
	if (field != 0)
		*mantissa |= 0x800000;
	*exponent = (int32_t)(field == 0 ? 1U : field) - 150;
	if ((bits & 0x80000000U) != 0)
		*mantissa = -*mantissa;

	/* Succeeded: the value. */
	return 0;
}

/*
 * The float nearest a + b * 2^scale_b (or a - b * 2^scale_b), exactly
 * rounded.  Refuses values more than 2^36 apart in scale (ENOTSUP), which
 * a viewport does not have.
 */
static int
float_from_sum(
	uint32_t a,
	uint32_t b,
	int32_t scale_b,
	bool subtract,
	uint32_t *result)
{
	int64_t mantissa_a;
	int64_t mantissa_b;
	int32_t exponent_a;
	int32_t exponent_b;
	int32_t low;
	int64_t sum;
	int error;

	/* The two values. */
	error = rational_from_float(a, &mantissa_a, &exponent_a);
	if (error != 0)
		return error;
	error = rational_from_float(b, &mantissa_b, &exponent_b);
	if (error != 0)
		return error;
	exponent_b += scale_b;
	if (subtract)
		mantissa_b = -mantissa_b;

	/* Zero terms take the other's exponent. */
	if (mantissa_a == 0)
		exponent_a = exponent_b;
	if (mantissa_b == 0)
		exponent_b = exponent_a;

	/* Both over the lower exponent, refused when too far apart. */
	low = exponent_a < exponent_b ? exponent_a : exponent_b;
	if (exponent_a - low > 36 || exponent_b - low > 36 || low < -62 || low > 62)
		return ENOTSUP;
	sum = mantissa_a * ((int64_t)1 << (exponent_a - low)) + mantissa_b * ((int64_t)1 << (exponent_b - low));

	/* The sum's float: a ratio with a power of two below or above. */
	if (sum == 0) {
		*result = 0;
	} else if (low >= 0) {
		if (low > 24)
			return ENOTSUP;
		*result = float_from_ratio(sum * ((int64_t)1 << low), 1);
	} else {
		*result = float_from_ratio(sum, (int64_t)1 << (-low));
	}

	/* Succeeded: the float. */
	return 0;
}

/* A float truncated toward zero to an integer (as a C cast), clamped to the int32 range. */
static int32_t
integer_from_float(
	uint32_t bits)
{
	int64_t mantissa;
	int32_t exponent;
	int64_t value;
	int special;

	/* NaN and infinity count as zero. */
	special = rational_from_float(bits, &mantissa, &exponent);
	if (special != 0)
		return 0;

	/* Shifts the mantissa to the integer, toward zero. */
	if (exponent >= 0) {
		if (exponent > 30)
			return mantissa < 0 ? INT32_MIN : INT32_MAX;
		value = mantissa * ((int64_t)1 << exponent);
	} else if (exponent < -40) {
		value = 0;
	} else {
		value = (mantissa < 0 ? -((-mantissa) >> (-exponent)) : mantissa >> (-exponent));
	}

	/* Clamped. */
	if (value > INT32_MAX)
		return INT32_MAX;
	if (value < INT32_MIN)
		return INT32_MIN;
	return (int32_t)value;
}

/*
 * The bits of the float nearest numerator / denominator (round to
 * nearest, ties to even), for a positive denominator and a quotient in
 * the normal range.  The same as transfer-program.c's.
 */
static uint32_t
float_from_ratio(
	int64_t numerator,
	int64_t denominator)
{
	uint64_t dividend;
	uint64_t divisor;
	uint64_t quotient;
	uint64_t remainder;
	uint64_t mantissa;
	uint32_t sign;
	int32_t exponent;
	unsigned sticky;

	/* Zero, and the sign of the quotient. */
	if (numerator == 0 || denominator == 0)
		return 0;
	sign = 0;
	if ((numerator < 0) != (denominator < 0))
		sign = 0x80000000U;
	dividend = (uint64_t)(numerator < 0 ? -numerator : numerator);
	divisor = (uint64_t)(denominator < 0 ? -denominator : denominator);

	/* The quotient to 26 significant bits and whether anything is left. */
	quotient = dividend / divisor;
	remainder = dividend % divisor;
	exponent = 25;
	sticky = 0;
	while (quotient >= (1ULL << 26)) {
		sticky |= (unsigned)(quotient & 1U);
		quotient >>= 1;
		exponent++;
	}
	while (quotient < (1ULL << 25)) {
		remainder <<= 1;
		quotient <<= 1;
		if (remainder >= divisor) {
			quotient |= 1U;
			remainder -= divisor;
		}
		exponent--;
	}
	if (remainder != 0)
		sticky = 1;

	/* Rounds the 24-bit mantissa to nearest, ties to even. */
	mantissa = quotient >> 2;
	if ((quotient & 2U) != 0 && ((quotient & 1U) != 0 || sticky != 0 || (mantissa & 1U) != 0))
		mantissa++;
	if (mantissa == (1ULL << 24)) {
		mantissa >>= 1;
		exponent++;
	}

	/* The bits. */
	return sign | ((uint32_t)(exponent + 127) << 23) | ((uint32_t)mantissa & 0x7fffffU);
}

/*
 * A viewport's six floats as their bits (x, y, width, height, minDepth,
 * maxDepth), copied as bytes: the kernel does no floating-point work.
 */
static void
viewport_bits(
	const VkViewport *viewport,
	uint32_t bits[POWERVR_DRAW_VIEWPORT_WORDS])
{
	/* The six fields in their order. */
	__builtin_memcpy(bits, viewport, POWERVR_DRAW_VIEWPORT_WORDS * sizeof(uint32_t));
}

/* Rounds a size up to whole units. */
static uint32_t
round_units(
	uint32_t value,
	uint32_t unit)
{
	/* The units that hold the size. */
	return (value + unit - 1U) / unit;
}
