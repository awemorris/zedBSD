/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 */

/*
 * Copyright (C) 2016 Intel Corporation
 * Copyright (c) 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/*
 * The Gen12 3D pipeline commands and state fields of Mesa genxml: the
 * command opcodes and packet lengths, the structure sizes, and the field
 * values and registers the render path programs.
 *
 * zedBSD — transcribed hardware definitions (values only)
 *
 * Source: Mesa main @ ab691a1cc7bcd264bec8f735deb2127861ad15ef (MIT; values are
 * Intel PRM hardware facts).  SHA-256 of the files:
 *   src/intel/genxml/gen120.xml
 *     12fea5fdac709a3587cae64c3bd2b0d187a9f81ccd573160703c69327a521164
 *   src/intel/genxml/gen110.xml (imported by gen120.xml)
 *     b5159c099b41ef9afa10578f4215ee542634d2c422d59742b9c5c54af6ac93fa
 *   src/intel/genxml/gen70.xml (the pipeline statistics registers)
 *     fd58ca141b55246826628c3d8d4a0d142b2db51d1ad7033c21c4b282916883ae
 *   src/intel/common/intel_l3_config.h (the URB deref block sizes)
 *     70450c118ffc67771a1b727f2c502f8ff8fb8b930edb1cd34a322470480ffb01
 * The index buffer, the random vertex access of 3DPRIMITIVE and the pixel
 * stage's push constant enable were added from the same file of Mesa 25.0.7
 * (Debian source package 25.0.7-2+deb13u1):
 *   src/intel/genxml/gen120.xml
 *     e2452c7dd2d19f9c506f487ce98e938984b6bdf8a3ea2504c64afdd4facd542e
 * The mip fields of RENDER_SURFACE_STATE and the SAMPLER_STATE filter, LOD
 * and bias fields come from that same gen120.xml; the LOD clamps are the
 * ones anv programs (src/intel/vulkan/genX_init_state.c
 *     716543ab781499a3079174681a90943e9a28b2c1658c2f12dbe4f15df2bead4b).
 * The point, line, strip and fan values of 3D_Prim_Topo_Type and the
 * provoking vertex fields of 3DSTATE_SF and 3DSTATE_CLIP come from the same
 * Mesa 25.0.7 (Debian source package 25.0.7-2+deb13u1):
 *   src/intel/genxml/gen70.xml (3D_Prim_Topo_Type)
 *     dd7c942fc12afd2defdc435997ccdb5b48841d344f40625dbb2b4e746eca09ef
 *   src/intel/genxml/gen120.xml (3DSTATE_SF, the file above)
 *   src/intel/genxml/gen80.xml (3DSTATE_CLIP, which gen120.xml imports through gen110.xml and gen90.xml;
 *   and VFCOMP_STORE_1_INT of 3D_Vertex_Component_Control, and 3DSTATE_VF_SGVS)
 *     2962677cf69dc947345fd88bd7010427900160eb7a7b076463e6e8d28772439d
 * The genxml files carry no notice of their own; the notice above is the one
 * Mesa's generator (src/intel/genxml/gen_pack_header.py) puts on the headers
 * it produces from them, followed by the copyright line of intel_l3_config.h.
 *
 * The 3D pipeline command header dword is
 *   (Command Type 3 << 29) | (Pipeline 3 << 27) | (Opcode << 24) |
 *   (Sub Opcode << 16) | (DWord Length = total dwords - 2)
 * The high sixteen bits of that dword are the opcode constants below, taken
 * from each instruction's Command Type / Pipeline / 3D Command Opcode /
 * 3D Command Sub Opcode default fields.  The assembly logic is new.
 */

#ifndef DRIVERS_GPU_I915_INTEL_GENXML_H
#define DRIVERS_GPU_I915_INTEL_GENXML_H

/* Command opcodes (high sixteen bits of the header dword). */
/*
 * PIPELINE_SELECT is CommandType 3 / CommandSubType 1 / Opcode 1 / SubOpcode 4
 * = 0x6904xxxx (Linux gt/intel_gpu_commands.h PIPELINE_SELECT, gen7_renderstate
 * literal 0x69040000, genxml).  0x6104 (SubType 0) was a transcription error:
 * the GPU never switched pipelines for any batch that used it.
 */
#define GEN12_CMD_PIPELINE_SELECT		0x6904U
#define GEN12_CMD_STATE_BASE_ADDRESS		0x6101U
#define GEN12_CMD_3DSTATE_VS			0x7810U
#define GEN12_CMD_3DSTATE_PS			0x7820U
#define GEN12_CMD_3DSTATE_PS_EXTRA		0x784FU
#define GEN12_CMD_3DSTATE_VERTEX_ELEMENTS	0x7809U
#define GEN12_CMD_3DPRIMITIVE			0x7B00U

/* Builds a command header from an opcode and the packet's dword count. */
#define GEN12_CMD_HEADER(opcode, dwords)	(((opcode) << 16) | ((dwords) - 2U))

/* Packet lengths (gen120.xml instruction "length"). */
#define GEN12_3DSTATE_VS_DWORDS			9U
#define GEN12_3DSTATE_PS_DWORDS			12U
#define GEN12_3DSTATE_VERTEX_ELEMENTS_DWORDS	3U
#define GEN12_3DPRIMITIVE_DWORDS		7U
#define GEN12_PIPELINE_SELECT_DWORDS		1U
#define GEN12_STATE_BASE_ADDRESS_DWORDS		22U

/*
 * PIPELINE_SELECT: the 3D pipeline is selection value 0.  Gen12 applies the
 * selection only when its mask bits (15:8) cover it, and Mesa also masks in
 * and sets the media sampler DOP clock gate (gen12 mask 0x13, bit 4).
 */
#define GEN12_PIPELINE_SELECT_3D		0U
#define GEN12_PIPELINE_SELECT_MASK		(0x13U << 8)
#define GEN12_PIPELINE_SELECT_MEDIA_DOP_GATE	(1U << 4)
#define GEN12_PIPELINE_SELECT_DWORD(pipeline)	\
	((GEN12_CMD_PIPELINE_SELECT << 16) | GEN12_PIPELINE_SELECT_MASK | \
	 GEN12_PIPELINE_SELECT_MEDIA_DOP_GATE | (pipeline))
/*
 * Fixed reference words (not derived from the macros above): 0x6104xxxx is the
 * header of GPGPU_CSR_BASE_ADDRESS (SubType 0, 3 dwords), a different command.
 */
_Static_assert(GEN12_PIPELINE_SELECT_DWORD(0U) == 0x69041310U,
	"Gen12 PIPELINE_SELECT(3D) must be 0x69041310");
_Static_assert(GEN12_PIPELINE_SELECT_DWORD(2U) == 0x69041312U,
	"Gen12 PIPELINE_SELECT(GPGPU) must be 0x69041312");


/*
 * The rest of the 3D pipeline, transcribed from gen120.xml resolved through its
 * <import> chain of gen110.xml.  Each opcode is the high sixteen bits of the
 * packet header and each DWORDS is the packet's total dword count, so
 * GEN12_CMD_HEADER() produces the header with the right DWord Length.
 */
#define GEN12_CMD_3DSTATE_VF_STATISTICS		0x680BU
#define GEN12_CMD_3DSTATE_VF			0x780CU
#define GEN12_CMD_3DSTATE_VF_TOPOLOGY		0x784BU
#define GEN12_CMD_3DSTATE_VF_SGVS		0x784AU
#define GEN12_CMD_3DSTATE_VF_SGVS_2		0x7856U
#define GEN12_CMD_3DSTATE_VERTEX_BUFFERS	0x7808U
#define GEN12_CMD_3DSTATE_URB_ALLOC_VS		0x7858U
#define GEN12_CMD_3DSTATE_URB_ALLOC_HS		0x7859U
#define GEN12_CMD_3DSTATE_URB_ALLOC_DS		0x785AU
#define GEN12_CMD_3DSTATE_URB_ALLOC_GS		0x785BU
#define GEN12_CMD_3DSTATE_PUSH_CONSTANT_ALLOC_VS	0x7912U
#define GEN12_CMD_3DSTATE_PUSH_CONSTANT_ALLOC_HS	0x7913U
#define GEN12_CMD_3DSTATE_PUSH_CONSTANT_ALLOC_DS	0x7914U
#define GEN12_CMD_3DSTATE_PUSH_CONSTANT_ALLOC_GS	0x7915U
#define GEN12_CMD_3DSTATE_PUSH_CONSTANT_ALLOC_PS	0x7916U
#define GEN12_CMD_3DSTATE_HS			0x781BU
#define GEN12_CMD_3DSTATE_TE			0x781CU
#define GEN12_CMD_3DSTATE_DS			0x781DU
#define GEN12_CMD_3DSTATE_STREAMOUT		0x781EU
#define GEN12_CMD_3DSTATE_GS			0x7811U
#define GEN12_CMD_3DSTATE_CLIP			0x7812U
#define GEN12_CMD_3DSTATE_SF			0x7813U
#define GEN12_CMD_3DSTATE_RASTER		0x7850U
#define GEN12_CMD_3DSTATE_SBE			0x781FU
#define GEN12_CMD_3DSTATE_WM			0x7814U
#define GEN12_CMD_3DSTATE_WM_DEPTH_STENCIL	0x784EU
#define GEN12_CMD_3DSTATE_PS_BLEND		0x784DU
#define GEN12_CMD_3DSTATE_BINDING_TABLE_POINTERS_PS	0x782AU
#define GEN12_CMD_3DSTATE_BLEND_STATE_POINTERS	0x7824U
#define GEN12_CMD_3DSTATE_CC_STATE_POINTERS	0x780EU
#define GEN12_CMD_3DSTATE_VIEWPORT_STATE_POINTERS_CC	0x7823U
#define GEN12_CMD_3DSTATE_DRAWING_RECTANGLE	0x7900U
#define GEN12_CMD_3DSTATE_MULTISAMPLE		0x780DU
#define GEN12_CMD_3DSTATE_SAMPLE_MASK		0x7818U
#define GEN12_CMD_3DSTATE_DEPTH_BUFFER		0x7805U
#define GEN12_CMD_3DSTATE_STENCIL_BUFFER	0x7806U
#define GEN12_CMD_3DSTATE_HIER_DEPTH_BUFFER	0x7807U
#define GEN12_CMD_3DSTATE_CLEAR_PARAMS		0x7804U
#define GEN12_CMD_3DSTATE_PRIMITIVE_REPLICATION	0x786CU

#define GEN12_3DSTATE_VF_DWORDS			2U
#define GEN12_3DSTATE_VF_TOPOLOGY_DWORDS	2U
#define GEN12_3DSTATE_VF_SGVS_DWORDS		2U
#define GEN12_3DSTATE_VF_SGVS_2_DWORDS		3U
#define GEN12_3DSTATE_URB_ALLOC_DWORDS		3U
#define GEN12_3DSTATE_PUSH_CONSTANT_ALLOC_DWORDS	2U
#define GEN12_3DSTATE_HS_DWORDS			9U
#define GEN12_3DSTATE_TE_DWORDS			5U
#define GEN12_3DSTATE_DS_DWORDS			11U
#define GEN12_3DSTATE_STREAMOUT_DWORDS		5U
#define GEN12_3DSTATE_GS_DWORDS			10U
#define GEN12_3DSTATE_CLIP_DWORDS		4U
#define GEN12_3DSTATE_SF_DWORDS			4U
#define GEN12_3DSTATE_RASTER_DWORDS		5U
#define GEN12_3DSTATE_SBE_DWORDS		6U
#define GEN12_3DSTATE_WM_DWORDS			2U
#define GEN12_3DSTATE_WM_DEPTH_STENCIL_DWORDS	4U
#define GEN12_3DSTATE_PS_EXTRA_DWORDS		2U
#define GEN12_3DSTATE_PS_BLEND_DWORDS		2U
#define GEN12_3DSTATE_POINTERS_DWORDS		2U
#define GEN12_3DSTATE_DRAWING_RECTANGLE_DWORDS	4U
#define GEN12_3DSTATE_MULTISAMPLE_DWORDS	2U
#define GEN12_3DSTATE_SAMPLE_MASK_DWORDS	2U
#define GEN12_3DSTATE_DEPTH_BUFFER_DWORDS	8U
#define GEN12_3DSTATE_STENCIL_BUFFER_DWORDS	8U
#define GEN12_3DSTATE_HIER_DEPTH_BUFFER_DWORDS	5U
#define GEN12_3DSTATE_CLEAR_PARAMS_DWORDS	3U
#define GEN12_3DSTATE_PRIMITIVE_REPLICATION_DWORDS	6U

/* Structure sizes in dwords (gen120.xml struct lengths). */
#define GEN12_RENDER_SURFACE_STATE_DWORDS	16U
#define GEN12_VERTEX_BUFFER_STATE_DWORDS	4U
#define GEN12_VERTEX_ELEMENT_STATE_DWORDS	2U
#define GEN12_COLOR_CALC_STATE_DWORDS		6U
#define GEN12_BLEND_STATE_DWORDS		1U
#define GEN12_BLEND_STATE_ENTRY_DWORDS		2U
#define GEN12_CC_VIEWPORT_DWORDS		2U

/* 3D_Prim_Topo_Type: a screen-space rectangle needs no vertex shader; the rest are Vulkan's list, strip and fan topologies. */
#define GEN12_3DPRIM_RECTLIST			15U
#define GEN12_3DPRIM_TRILIST			4U
#define GEN12_3DPRIM_POINTLIST			1U
#define GEN12_3DPRIM_LINELIST			2U
#define GEN12_3DPRIM_LINESTRIP			3U
#define GEN12_3DPRIM_TRISTRIP			5U
#define GEN12_3DPRIM_TRIFAN			6U

/*
 * 3D_Prim_Topo_Type of the topologies with adjacency, which only a draw
 * with a geometry shader takes (ws075-p007b): the line list and strip, the
 * triangle list and strip (Mesa 25.0.7 gen70.xml, sha256
 * dd7c942fc12afd2defdc435997ccdb5b48841d344f40625dbb2b4e746eca09ef,
 * 3D_Prim_Topo_Type LINELIST_ADJ .. TRISTRIP_ADJ).
 */
#define GEN12_3DPRIM_LINELIST_ADJ		9U
#define GEN12_3DPRIM_LINESTRIP_ADJ		10U
#define GEN12_3DPRIM_TRILIST_ADJ		11U
#define GEN12_3DPRIM_TRISTRIP_ADJ		12U

/*
 * The triangle fan's provoking vertex select of 3DSTATE_SF (dword 3, bits
 * 26:25) and 3DSTATE_CLIP (dword 2, bits 1:0): 1 is the second vertex of
 * each triangle, Vulkan's first-vertex convention for a fan (the list and
 * strip selects stay 0, their first vertex).
 */
#define GEN12_SF_FAN_PROVOKING_SHIFT		25U
#define GEN12_CLIP_FAN_PROVOKING_SHIFT		0U
#define GEN12_FAN_PROVOKING_SECOND		1U

/*
 * The point fields of 3DSTATE_SF dword 3 (gen120.xml above): Point Width in
 * bits 10:0 as u8.3 (8 is 1.0), Point Width Source in bit 11 (0 the
 * vertex's, from dword 3 of its VUE header; 1 the Point Width field).
 */
#define GEN12_SF_POINT_WIDTH_ONE		8U
#define GEN12_SF_POINT_WIDTH_FROM_STATE		(1U << 11)

/*
 * 3DSTATE_WM dword 1 bits 16:11, Barycentric Interpolation Mode (gen80.xml
 * above): one bit to each of Mesa's enum intel_barycentric_mode, and the
 * payload carries the barycentrics of each mode set in that order
 * (brw_fs_thread_payload.cpp): the perspective ones at the pixel centre,
 * then the linear ones.
 */
#define GEN12_WM_BARYCENTRIC_PERSPECTIVE_PIXEL	(1U << 11)
#define GEN12_WM_BARYCENTRIC_LINEAR_PIXEL	(1U << 14)

/*
 * 3DSTATE_CLIP dword 2 bit 8, Non-Perspective Barycentric Enable (gen80.xml
 * above): the clipper passes on what the linear barycentrics need, which
 * anv sets when the pixel kernel interpolates without perspective
 * (Mesa 25.0.7 src/intel/vulkan/genX_pipeline.c, sha256
 * 9bf244df1284531767529980909176d09b72db037b6fdaa7f5874d997ebf7545:
 * uses_nonperspective_interp_modes).
 */
#define GEN12_CLIP_NON_PERSPECTIVE_BARYCENTRIC	(1U << 8)

/*
 * 3DSTATE_CLIP dword 3 bit 5, Force Zero RTA Index Enable (Mesa 25.0.7
 * gen80.xml, sha256
 * 2962677cf69dc947345fd88bd7010427900160eb7a7b076463e6e8d28772439d, start
 * 101): every primitive goes to render target array index 0.  anv sets it
 * unless the last vertex processing stage writes the layer (genX_pipeline.c
 * above, ForceZeroRTAIndexEnable; ws075-p007b).
 */
#define GEN12_CLIP_FORCE_ZERO_RTA_INDEX		(1U << 5)

/*
 * 3DSTATE_GS dword 7 bits 12:11, Dispatch Mode: SIMD8, one input
 * primitive to a channel (Mesa 25.0.7 gen110.xml, sha256
 * 6598e556ffedf4fe051c78af3bb08030a39af865c76e2784dba5374646fce35d,
 * DISPATCH_MODE_SIMD8; ws075-p007b).
 */
#define GEN12_GS_DISPATCH_MODE_SIMD8		3U

/*
 * 3DSTATE_STREAMOUT dword 1 bit 30, API Rendering Disable (gen12.xml, start
 * 62): the primitives go no further than the stream output stage, so the
 * vertex shader runs and nothing is rasterized.  anv sets it for
 * rasterizerDiscardEnable whether or not stream output is on (Mesa 25.0.7
 * src/intel/vulkan/genX_gfx_state.c: so.RenderingDisable).
 */
#define GEN12_STREAMOUT_RENDERING_DISABLE	(1U << 30)

/*
 * 3DSTATE_PS_EXTRA dword 1 (gen110.xml below): bit 23 Pixel Shader Uses
 * Source W and bit 24 Pixel Shader Uses Source Depth, which put the
 * interpolated w and depth of each pixel into the thread payload after the
 * barycentrics.
 */
#define GEN12_3DSTATE_PS_EXTRA_USES_SOURCE_W	(1U << 23)
#define GEN12_3DSTATE_PS_EXTRA_USES_SOURCE_DEPTH	(1U << 24)

/*
 * 3DSTATE_SBE (Mesa 25.0.7 gen90.xml, imported by gen120.xml through
 * gen110.xml; sha256
 * d86fb566b9292280e2d6a385107711fb7ee8008e5c54bb3ba70a2c0343262ddb): dword 1
 * bit 20 Point Sprite Texture Coordinate Origin (0 the upper left, Vulkan's),
 * dword 2 Point Sprite Texture Coordinate Enable, one bit to an attribute
 * the setup replaces with the point's texture coordinate.
 */
#define GEN12_SBE_POINT_SPRITE_ORIGIN_SHIFT		20U
#define GEN12_SBE_POINT_SPRITE_ORIGIN_UPPER_LEFT	0U

/*
 * gl_PrimitiveID that no stage writes (ws075-p007b b4): 3DSTATE_SBE dword 1
 * bits 4:0 Primitive ID Override Attribute Select and bits 19:16 Primitive
 * ID Override Component X..W (the same gen90.xml), and the attribute's
 * SF_OUTPUT_ATTRIBUTE_DETAIL in SBE_SWIZ: Constant Source PRIM_ID (3) in
 * bits 10:9 and Component Override X..W in bits 15:12 (Mesa 25.0.7
 * gen60.xml, imported by gen120.xml; sha256
 * 30fac841448b4239bbaedf92a77424ec054629d28f266c7190b3f592f4b9185c), as
 * anv sets both (src/intel/vulkan/genX_pipeline.c, emit_3dstate_sbe();
 * sha256 9bf244df1284531767529980909176d09b72db037b6fdaa7f5874d997ebf7545).
 */
#define GEN12_SBE_PRIMITIVE_ID_OVERRIDE_XYZW		(0xFU << 16)
#define GEN12_SBE_SWIZ_PRIMITIVE_ID			((3U << 9) | (0xFU << 12))

/* 3D_Vertex_Component_Control. */
#define GEN12_VFCOMP_NOSTORE			0U
#define GEN12_VFCOMP_STORE_SRC			1U
#define GEN12_VFCOMP_STORE_0			2U
#define GEN12_VFCOMP_STORE_1_FP			3U
#define GEN12_VFCOMP_STORE_1_INT		4U

/*
 * 3DSTATE_VF_SGVS dword 1: the vertex ID into component 0 of the element
 * at bits 5:0 (enable bit 15), the instance ID into component 0 of the
 * element at bits 21:16 (enable bit 31).
 */
#define GEN12_SGVS_VERTEX_ID_ENABLE		(1U << 15)
#define GEN12_SGVS_INSTANCE_ID_SHIFT		16U
#define GEN12_SGVS_INSTANCE_ID_ENABLE		(1U << 31)

/* Surface formats (isl_format numbering). */
#define GEN12_FORMAT_R32G32B32A32_FLOAT		0U
#define GEN12_FORMAT_R32G32B32_FLOAT		64U
#define GEN12_FORMAT_B8G8R8A8_UNORM		192U

/* RENDER_SURFACE_STATE surface types and alignments. */
#define GEN12_SURFTYPE_2D			1U
#define GEN12_SURFTYPE_3D			2U
#define GEN12_SURFTYPE_CUBE			3U
#define GEN12_SURFTYPE_BUFFER			4U
#define GEN12_SURFTYPE_NULL			7U
#define GEN12_SURFACE_ALIGN_4			1U
#define GEN12_TILEMODE_LINEAR			0U
#define GEN12_TILEMODE_YMAJOR			3U

/* BLEND_STATE_ENTRY colour clamp range. */
#define GEN12_COLORCLAMP_RTFORMAT		2U

/* URB allocations are made in 8 KiB chunks and entries in 64-byte units. */
#define GEN12_URB_CHUNK_KB			8U

/*
 * The URB the vertex stage owns past the 32 KiB of push constants on the
 * target (3576 entries of 64 bytes fill it), and the most vertex entries
 * (intel_device_info urb.max_entries[MESA_SHADER_VERTEX] of Gen12).
 */
#define GEN12_URB_VS_BYTES			(3576U * 64U)
#define GEN12_URB_VS_ENTRIES			3576U

/*
 * With a geometry shader (ws075-p007b) the vertex stage's URB is split: the
 * vertex stage keeps 21 of its chunks from chunk 4, the geometry stage the
 * next 6 (48 KiB), so the URB in use does not grow.  A geometry stage needs
 * at least 2 entries (it runs in DUAL_OBJECT mode) and has at most 1548
 * (Mesa 25.0.7 src/intel/common/intel_urb_config.c, sha256
 * e1e7ddc142f020e49c21c27085fab34b80f192afec19c03d3a4426cc8cddc414,
 * min_entries; src/intel/dev/intel_device_info.c, sha256
 * 1a3c7c6d87c4a60af5d1003096b4add0a34c4a02afc12b40a378cc060540afd8,
 * GFX12_HW_INFO urb.max_entries[MESA_SHADER_GEOMETRY]).
 */
#define GEN12_URB_VS_START_CHUNK		4U
#define GEN12_URB_VS_SPLIT_CHUNKS		21U
#define GEN12_URB_GS_CHUNKS			6U
#define GEN12_URB_GS_MIN_ENTRIES		2U
#define GEN12_URB_GS_ENTRIES			1548U

/* 3DSTATE_SF deref block size (intel_l3_config.h). */
#define GEN12_URB_DEREF_BLOCK_SIZE_32		0U
#define GEN12_URB_DEREF_BLOCK_SIZE_PER_POLY	1U

/* The pixel shader dispatch field is programmed as the thread count less one. */
#define GEN12_MAX_THREADS_PER_PSD		64U

/* IEEE-754 bit patterns; the kernel builds vertex data without floating point. */
#define GEN12_F32_0				0x00000000U
#define GEN12_F32_1				0x3f800000U

/* Pipeline statistics counters (gen70.xml register numbers, 64-bit each). */
#define GEN12_REG_IA_VERTICES_COUNT		0x2310U
#define GEN12_REG_IA_PRIMITIVES_COUNT		0x2318U
#define GEN12_REG_VS_INVOCATION_COUNT		0x2320U
#define GEN12_REG_CL_INVOCATION_COUNT		0x2338U
#define GEN12_REG_CL_PRIMITIVES_COUNT		0x2340U
#define GEN12_REG_PS_INVOCATION_COUNT		0x2348U

/*
 * The L3 cache is partitioned before the URB can hold a single vertex entry.
 * These are the Tiger Lake values Mesa validates: 32 ways of URB and 88 ways
 * shared, written into L3ALLOC (URB in bits 7:1, All in bits 31:25).
 */
#define GEN12_REG_L3ALLOC			0xB134U
#define GEN12_L3ALLOC_URB_WAYS			32U
#define GEN12_L3ALLOC_ALL_WAYS			88U
#define GEN12_L3ALLOC_VALUE \
	((GEN12_L3ALLOC_URB_WAYS << 1) | (GEN12_L3ALLOC_ALL_WAYS << 25))

#define GEN12_CMD_3DSTATE_VF_INSTANCING		0x7849U
#define GEN12_3DSTATE_VF_INSTANCING_DWORDS	3U
#define GEN12_VF_INSTANCING_ENABLE_SHIFT	8U

/*
 * A disabled depth buffer is still a typed one: Mesa's isl always pairs
 * SURFTYPE_NULL with D32_FLOAT, because format zero names a depth-stencil
 * format and makes the depth unit expect a stencil buffer that is not there.
 */
#define GEN12_DEPTH_FORMAT_D32_FLOAT		1U
#define GEN12_DEPTH_FORMAT_D16_UNORM		5U

/* 3DSTATE_SBE attribute component format: all four channels active. */
#define GEN12_ACF_XYZW				3U

/* Fragments that reached the depth test, whether or not a shader ran. */
#define GEN12_REG_PS_DEPTH_COUNT		0x2350U

/*
 * Every MOCS field carries the cache-table index shifted left by one; bit zero
 * of the field is reserved.  Mesa's isl programs Tiger Lake the same way
 * (internal = 2 << 1, uncached = 3 << 1), and the Linux Gen12 MOCS table gives
 * index 2 as write-back and index 3 as uncached.  Writing a raw index selects
 * the entry one bit to the right, and entries 0 and 1 are reserved.
 */
#define GEN12_MOCS(index)			((index) << 1)

/*
 * 3DSTATE_RASTER cull modes.  Zero means cull both faces, so a rasterizer left
 * at its zeroed default discards every primitive; both BLORP and anv name
 * CULLMODE_NONE explicitly for that reason.
 */
#define GEN12_CULLMODE_BOTH			0U
#define GEN12_CULLMODE_NONE			1U
#define GEN12_CULLMODE_FRONT			2U
#define GEN12_CULLMODE_BACK			3U

/*
 * State Mesa's anv programs once per render context before any draw: the
 * depth-clear override packet cleared, sample positions, depth bounds and
 * the other stages' binding table pointers.  A context that skips them runs
 * with whatever the hardware or a previous owner left behind.
 */
#define GEN12_CMD_3DSTATE_WM_HZ_OP		0x7852U
#define GEN12_CMD_3DSTATE_AA_LINE_PARAMETERS	0x790AU
#define GEN12_CMD_3DSTATE_WM_CHROMAKEY		0x784CU
#define GEN12_CMD_3DSTATE_SAMPLE_PATTERN	0x791CU
#define GEN12_CMD_3DSTATE_DEPTH_BOUNDS		0x7871U
#define GEN12_CMD_3DSTATE_POLY_STIPPLE_OFFSET	0x7906U
#define GEN12_CMD_3DSTATE_LINE_STIPPLE		0x7908U
#define GEN12_CMD_3DSTATE_BINDING_TABLE_POINTERS_VS	0x7826U
#define GEN12_CMD_3DSTATE_BINDING_TABLE_POINTERS_HS	0x7827U
#define GEN12_CMD_3DSTATE_BINDING_TABLE_POINTERS_DS	0x7828U
#define GEN12_CMD_3DSTATE_BINDING_TABLE_POINTERS_GS	0x7829U

#define GEN12_3DSTATE_WM_HZ_OP_DWORDS		5U
#define GEN12_3DSTATE_AA_LINE_PARAMETERS_DWORDS	3U
#define GEN12_3DSTATE_WM_CHROMAKEY_DWORDS	2U
#define GEN12_3DSTATE_SAMPLE_PATTERN_DWORDS	9U
#define GEN12_3DSTATE_DEPTH_BOUNDS_DWORDS	4U
#define GEN12_3DSTATE_POLY_STIPPLE_OFFSET_DWORDS	2U
#define GEN12_3DSTATE_LINE_STIPPLE_DWORDS	3U

/* 3DSTATE_SAMPLE_PATTERN dword 8: the single sample sits at the pixel centre (u0.4 0.5). */
#define GEN12_SAMPLE_PATTERN_1X_CENTRE		((8U << 20) | (8U << 16))

/*
 * 3DSTATE_SAMPLE_PATTERN dword 8 bits 15:0 and dword 7: the standard two
 * and four sample positions (Mesa intel_sample_positions.h,
 * INTEL_SAMPLE_POS_2X and _4X), each sample a byte of X (bits 7:4) and Y
 * (bits 3:0) in sixteenths, sample 0 in the lowest byte: 2x (0.75, 0.75)
 * and (0.25, 0.25); 4x (0.375, 0.125), (0.875, 0.375), (0.125, 0.625) and
 * (0.625, 0.875).
 */
#define GEN12_SAMPLE_PATTERN_2X			((0x44U << 8) | 0xccU)
#define GEN12_SAMPLE_PATTERN_4X			((0xaeU << 24) | (0x2aU << 16) | (0xe6U << 8) | 0x62U)

/*
 * Instruction fetches fill the per-subslice instruction cache through the L3,
 * so the instruction base needs a write-back entry; Mesa's isl names index 2
 * ("internal") for it on Tiger Lake and never an uncached one.
 */
#define I915_MOCS_WRITEBACK_INDEX		2U

/*
 * Push constants.  Every Mesa path programs a 3DSTATE_CONSTANT_* for each
 * stage before its first draw even with nothing to push, and gives the pixel
 * stage the whole 32 KiB push constant region when it is the only user.
 */
#define GEN12_CMD_3DSTATE_CONSTANT_VS		0x7815U
#define GEN12_CMD_3DSTATE_CONSTANT_GS		0x7816U
#define GEN12_CMD_3DSTATE_CONSTANT_PS		0x7817U
#define GEN12_CMD_3DSTATE_CONSTANT_HS		0x7819U
#define GEN12_CMD_3DSTATE_CONSTANT_DS		0x781AU
#define GEN12_3DSTATE_CONSTANT_DWORDS		11U
#define GEN12_PUSH_CONSTANT_KB			32U

/* VERTEX_BUFFER_STATE: vertex fetches stay in the L3 on this generation. */
#define GEN12_VERTEX_BUFFER_L3_BYPASS_DISABLE	(1U << 25)

/*
 * Coarse pixel shading state is fetched through a pointer relative to the
 * dynamic state base; anv points it at a disabled CPS_STATE at device
 * initialisation.  The binding table pool is disabled the same way, since
 * anv found that state leaking in from other contexts.
 */
#define GEN12_CMD_3DSTATE_CPS_POINTERS		0x7822U
#define GEN12_CPS_STATE_DWORDS			8U

#define GEN12_CMD_3DSTATE_BINDING_TABLE_POOL_ALLOC	0x7919U
#define GEN12_3DSTATE_BINDING_TABLE_POOL_ALLOC_DWORDS	4U

/* PIPE_CONTROL dw1 bit 1: stall the command streamer until the pixel scoreboard is idle. */
#define PIPE_CONTROL_STALL_AT_SCOREBOARD	(1U << 1)
#define PIPE_CONTROL_DEPTH_STALL_ENABLE		(1U << 13)

/*
 * State pointers and the attribute swizzle the Vulkan draw path programs
 * (gen120.xml 3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP, 3DSTATE_SCISSOR_STATE_POINTERS,
 * 3DSTATE_SAMPLER_STATE_POINTERS_PS, 3DSTATE_SBE_SWIZ).
 */
#define GEN12_CMD_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP	0x7821U
#define GEN12_CMD_3DSTATE_SCISSOR_STATE_POINTERS	0x780FU
#define GEN12_CMD_3DSTATE_SAMPLER_STATE_POINTERS_PS	0x782FU
#define GEN12_CMD_3DSTATE_SBE_SWIZ		0x7851U
#define GEN12_3DSTATE_SBE_SWIZ_DWORDS		11U

/*
 * The index buffer of an indexed draw (gen120.xml 3DSTATE_INDEX_BUFFER):
 * dword 1 carries the MOCS in bits 6:0, the Index Format in bits 9:8 and
 * the L3 Bypass Disable in bit 11; dwords 2 and 3 the address and dword 4
 * the size in bytes.
 */
#define GEN12_CMD_3DSTATE_INDEX_BUFFER		0x780AU
#define GEN12_3DSTATE_INDEX_BUFFER_DWORDS	5U
#define GEN12_INDEX_BYTE			0U
#define GEN12_INDEX_WORD			1U
#define GEN12_INDEX_DWORD			2U
#define GEN12_INDEX_FORMAT_SHIFT		8U
#define GEN12_INDEX_BUFFER_L3_BYPASS_DISABLE	(1U << 11)

/*
 * 3DPRIMITIVE dword 1 bit 8 (Vertex Access Type): RANDOM fetches every
 * vertex through the index buffer; SEQUENTIAL (0) numbers them in order.
 * Dword 6 is the Base Vertex Location added to every index.
 */
#define GEN12_3DPRIMITIVE_VERTEX_RANDOM		(1U << 8)

/*
 * 3DSTATE_VS (gen110.xml, imported by gen120.xml) and 3DSTATE_PS (gen120.xml)
 * dwords 4-5, the same in both: Per-Thread Scratch Space in bits 3:0 of
 * dword 4, the scratch memory of one thread as 1 KiB << n (n up to 11, 2
 * MiB); Scratch Space Base Pointer in bits 63:10 of the two dwords, an
 * address relative to the General State Base Address.  A thread's scratch
 * starts at the pointer plus its thread id times the per-thread space.
 * Mesa 25.0.7: gen110.xml sha256
 * 6598e556ffedf4fe051c78af3bb08030a39af865c76e2784dba5374646fce35d, gen120.xml
 * (above); anv programs them from the kernel's total_scratch
 * (src/intel/vulkan/genX_pipeline.c get_scratch_space(), gfx12.0 path).
 */
#define GEN12_SCRATCH_SPACE_MASK		0x0000000fU
#define GEN12_SCRATCH_POINTER_ALIGN		1024U
#define GEN12_SCRATCH_SPACE_MIN_BYTES		1024U
#define GEN12_SCRATCH_SPACE_MAX			11U

/*
 * 3DSTATE_PS dword 6 bit 11 (Push Constant Enable): the pixel threads get
 * the push constants of 3DSTATE_CONSTANT_PS in front of their setup data.
 */
#define GEN12_3DSTATE_PS_PUSH_CONSTANT_ENABLE	(1U << 11)

/*
 * 3DSTATE_PS_EXTRA dword 1 bit 28 (Pixel Shader Kills Pixel): the pixel
 * kernel may discard pixels, so the pixel stage must not write depth or
 * stencil for a pixel before the kernel has decided (gen110.xml, imported
 * by gen120.xml; Mesa 25.0.7 gen110.xml sha256
 * 6598e556ffedf4fe051c78af3bb08030a39af865c76e2784dba5374646fce35d).
 */
#define GEN12_3DSTATE_PS_EXTRA_KILLS_PIXEL	(1U << 28)

/*
 * The mip fields of RENDER_SURFACE_STATE dword 5 (gen120.xml): MIP Count /
 * LOD in bits 3:0, Surface Min LOD in bits 7:4, Mip Tail Start LOD in bits
 * 11:8.  A sampled surface reads levels [Surface Min LOD, Surface Min LOD +
 * MIP Count]; a render target writes level MIP Count.  Surface QPitch is
 * dword 1 bits 14:0, in units of four rows (isl_surface_state.c programs
 * the array pitch in rows shifted down by two).
 */
#define GEN12_RSS_MIP_COUNT_SHIFT		0U
#define GEN12_RSS_SURFACE_MIN_LOD_SHIFT		4U
#define GEN12_RSS_MIP_TAIL_START_SHIFT		8U
#define GEN12_RSS_LOD_MASK			0xfU
#define GEN12_RSS_QPITCH_MASK			0x7fffU

/*
 * The array fields of RENDER_SURFACE_STATE (gen110.xml, imported by
 * gen120.xml; Mesa 25.0.7 gen110.xml sha256
 * 6598e556ffedf4fe051c78af3bb08030a39af865c76e2784dba5374646fce35d): dword 0
 * Surface Array bit 28 and the six Cube Face Enables in bits 5:0; dword 3
 * Depth in bits 31:21 (the layers less one, the cubes less one, or a 3D
 * surface's depth less one); dword 4 Minimum Array Element in bits 28:18
 * and Render Target View Extent in bits 17:7.
 */
#define GEN12_RSS_SURFACE_ARRAY			(1U << 28)
#define GEN12_RSS_CUBE_FACES_ALL		0x3fU
#define GEN12_RSS_DEPTH_SHIFT			21U
#define GEN12_RSS_DEPTH_MASK			0x7ffU
#define GEN12_RSS_MIN_ARRAY_ELEMENT_SHIFT	18U
#define GEN12_RSS_VIEW_EXTENT_SHIFT		7U

/*
 * The multisample fields (gen120.xml): RENDER_SURFACE_STATE dword 4 Number
 * of Multisamples in bits 5:3 (the samples' log2) with Multisampled Surface
 * Storage Format bit 6 left MSFMT_MSS (every sample a slice, QPitch apart);
 * 3DSTATE_MULTISAMPLE dword 1 Number of Multisamples in bits 3:1;
 * 3DSTATE_RASTER dword 1 DX Multisample Rasterization Enable bit 12, which
 * with the DX10.1 API mode rasterizes on the sample pattern.
 */
#define GEN12_RSS_MULTISAMPLES_SHIFT		3U
#define GEN12_MULTISAMPLE_COUNT_SHIFT		1U
#define GEN12_RASTER_DX_MULTISAMPLE_ENABLE	(1U << 12)

/*
 * SAMPLER_STATE fields (gen120.xml), in dword and bit:
 *   dword 0: Texture LOD Bias 13:1 (s4.8), Min Mode Filter 16:14,
 *            Mag Mode Filter 19:17, Mip Mode Filter 21:20, LOD PreClamp
 *            Mode 28:27
 *   dword 1: Max LOD 19:8 (u4.8), Min LOD 31:20 (u4.8)
 *   dword 3: the address rounding enables 18:13 and the TCX, TCY and TCZ
 *            address control modes 8:6, 5:3 and 2:0
 * Mip Mode Filter is MIPFILTER_NONE 0, NEAREST 1, LINEAR 3.  anv
 * (genX_init_state.c) clamps the LOD bias to [-16, 15.996] and both LOD
 * limits to [0, 14].
 */
#define GEN12_SAMPLER_LOD_BIAS_SHIFT		1U
#define GEN12_SAMPLER_LOD_BIAS_MASK		0x1fffU
#define GEN12_SAMPLER_MIN_FILTER_SHIFT		14U
#define GEN12_SAMPLER_MAG_FILTER_SHIFT		17U
#define GEN12_SAMPLER_MIP_FILTER_SHIFT		20U
#define GEN12_SAMPLER_LOD_PRECLAMP_SHIFT	27U
#define GEN12_SAMPLER_MAX_LOD_SHIFT		8U
#define GEN12_SAMPLER_MIN_LOD_SHIFT		20U
#define GEN12_MAPFILTER_NEAREST			0U
#define GEN12_MAPFILTER_LINEAR			1U
/*
 * More SAMPLER_STATE fields (gen110.xml, imported by gen120.xml; Mesa 25.0.7
 * gen110.xml sha256
 * 6598e556ffedf4fe051c78af3bb08030a39af865c76e2784dba5374646fce35d): dword 0
 * Anisotropic Algorithm bit 0 (EWA approximation 1); dword 1 Cube Surface
 * Control Mode bit 0 (OVERRIDE 1) and Shadow Function 3:1 (PREFILTEROP_*);
 * dword 2 Border Color Pointer 31:6 (the dynamic state offset of a
 * 64-byte aligned SAMPLER_BORDER_COLOR_STATE, whose first four dwords are
 * the red, green, blue and alpha, floats or integers as the format
 * reads); dword 3 Maximum Anisotropy 21:19 (RATIO 2:1 0 .. 16:1 7),
 * Non-normalized Coordinate Enable bit 10, the address modes TCX 8:6, TCY
 * 5:3, TCZ 2:0.  Min and Mag Mode Filter take MAPFILTER_ANISOTROPIC 2.
 */
#define GEN12_SAMPLER_ANISOTROPIC_EWA		1U
#define GEN12_SAMPLER_CUBE_OVERRIDE		1U
#define GEN12_SAMPLER_SHADOW_SHIFT		1U
#define GEN12_SAMPLER_BORDER_POINTER_MASK	0xffffffc0U
#define GEN12_SAMPLER_MAX_ANISOTROPY_SHIFT	19U
#define GEN12_SAMPLER_NON_NORMALIZED		(1U << 10)
#define GEN12_SAMPLER_TCX_SHIFT			6U
#define GEN12_SAMPLER_TCY_SHIFT			3U
#define GEN12_SAMPLER_TCZ_SHIFT			0U
#define GEN12_MAPFILTER_ANISOTROPIC		2U

/* The PREFILTEROP values of the Shadow Function (gen110.xml). */
#define GEN12_PREFILTEROP_ALWAYS		0U
#define GEN12_PREFILTEROP_NEVER			1U
#define GEN12_PREFILTEROP_LESS			2U
#define GEN12_PREFILTEROP_EQUAL			3U
#define GEN12_PREFILTEROP_LEQUAL		4U
#define GEN12_PREFILTEROP_GREATER		5U
#define GEN12_PREFILTEROP_NOTEQUAL		6U
#define GEN12_PREFILTEROP_GEQUAL		7U

#define GEN12_MIPFILTER_NONE			0U
#define GEN12_MIPFILTER_NEAREST			1U
#define GEN12_MIPFILTER_LINEAR			3U
#define GEN12_CLAMP_MODE_OGL			2U
#define GEN12_SAMPLER_LOD_FRACTION_BITS		8U
#define GEN12_SAMPLER_LOD_MAX			(14 * 256)
#define GEN12_SAMPLER_LOD_BIAS_MIN		(-16 * 256)
#define GEN12_SAMPLER_LOD_BIAS_MAX		(16 * 256 - 1)

/*
 * Colour blending.  gen120.xml takes these from the files it imports (Mesa
 * 25.0.7, the same Debian source package): BLEND_STATE and
 * BLEND_STATE_ENTRY from gen80.xml
 *     2962677cf69dc947345fd88bd7010427900160eb7a7b076463e6e8d28772439d,
 * 3DSTATE_PS_BLEND from gen90.xml
 *     d86fb566b9292280e2d6a385107711fb7ee8008e5c54bb3ba70a2c0343262ddb,
 * COLOR_CALC_STATE from gen90.xml, and the 3D_Color_Buffer_Blend_Factor and
 * 3D_Color_Buffer_Blend_Function enumerations from gen40.xml
 *     8fe6663fa39cdfc6cd1cc4481e464824c2dcc5ea7d7d1a4d2e4a5791aa32f206.
 *
 * BLEND_STATE dword 0: Independent Alpha Blend Enable in bit 30.
 * BLEND_STATE_ENTRY dword 0: Write Disable Blue, Green, Red, Alpha in bits
 * 0..3, Alpha Blend Function 7:5, Destination Alpha Blend Factor 12:8,
 * Source Alpha Blend Factor 17:13, Color Blend Function 20:18, Destination
 * Blend Factor 25:21, Source Blend Factor 30:26, Color Buffer Blend Enable
 * 31.  (Dword 1 carries the clamps, GEN12_COLORCLAMP_RTFORMAT above.)
 * 3DSTATE_PS_BLEND dword 1: Independent Alpha Blend Enable in bit 7,
 * Destination Blend Factor 13:9, Source Blend Factor 18:14, Destination
 * Alpha Blend Factor 23:19, Source Alpha Blend Factor 28:24, Color Buffer
 * Blend Enable 29, Has Writeable RT 30.
 * COLOR_CALC_STATE dwords 2..5: the blend constant colour R, G, B, A as
 * floats.
 */
#define GEN12_BLEND_INDEPENDENT_ALPHA		(1U << 30)
#define GEN12_BLEND_WRITE_DISABLE_BLUE		(1U << 0)
#define GEN12_BLEND_WRITE_DISABLE_GREEN		(1U << 1)
#define GEN12_BLEND_WRITE_DISABLE_RED		(1U << 2)
#define GEN12_BLEND_WRITE_DISABLE_ALPHA		(1U << 3)
#define GEN12_BLEND_ALPHA_FUNCTION_SHIFT	5U
#define GEN12_BLEND_DST_ALPHA_FACTOR_SHIFT	8U
#define GEN12_BLEND_SRC_ALPHA_FACTOR_SHIFT	13U
#define GEN12_BLEND_COLOR_FUNCTION_SHIFT	18U
#define GEN12_BLEND_DST_FACTOR_SHIFT		21U
#define GEN12_BLEND_SRC_FACTOR_SHIFT		26U
#define GEN12_BLEND_ENABLE			(1U << 31)
#define GEN12_BLEND_BLENDING_BITS		0xfffffff0U
/* BLEND_STATE_ENTRY dword 1: the logic operation's enable and function (Mesa 25.0.7 genxml gen80.xml, bits 63 and 59..62). */
#define GEN12_BLEND_LOGIC_OP_ENABLE		(1U << 31)
#define GEN12_BLEND_LOGIC_OP_FUNCTION_SHIFT	27U
#define GEN12_PS_BLEND_INDEPENDENT_ALPHA	(1U << 7)
#define GEN12_PS_BLEND_DST_FACTOR_SHIFT		9U
#define GEN12_PS_BLEND_SRC_FACTOR_SHIFT		14U
#define GEN12_PS_BLEND_DST_ALPHA_FACTOR_SHIFT	19U
#define GEN12_PS_BLEND_SRC_ALPHA_FACTOR_SHIFT	24U
#define GEN12_PS_BLEND_ENABLE			(1U << 29)
#define GEN12_PS_BLEND_HAS_WRITEABLE_RT		(1U << 30)
#define GEN12_CC_BLEND_CONSTANT_DWORD		2U

/* 3D_Color_Buffer_Blend_Factor (BLENDFACTOR_*). */
#define GEN12_BLENDFACTOR_ONE			1U
#define GEN12_BLENDFACTOR_SRC_COLOR		2U
#define GEN12_BLENDFACTOR_SRC_ALPHA		3U
#define GEN12_BLENDFACTOR_DST_ALPHA		4U
#define GEN12_BLENDFACTOR_DST_COLOR		5U
#define GEN12_BLENDFACTOR_SRC_ALPHA_SATURATE	6U
#define GEN12_BLENDFACTOR_CONST_COLOR		7U
#define GEN12_BLENDFACTOR_CONST_ALPHA		8U
#define GEN12_BLENDFACTOR_SRC1_COLOR		9U
#define GEN12_BLENDFACTOR_SRC1_ALPHA		10U
#define GEN12_BLENDFACTOR_ZERO			17U
#define GEN12_BLENDFACTOR_INV_SRC_COLOR		18U
#define GEN12_BLENDFACTOR_INV_SRC_ALPHA		19U
#define GEN12_BLENDFACTOR_INV_DST_ALPHA		20U
#define GEN12_BLENDFACTOR_INV_DST_COLOR		21U
#define GEN12_BLENDFACTOR_INV_CONST_COLOR	23U
#define GEN12_BLENDFACTOR_INV_CONST_ALPHA	24U
#define GEN12_BLENDFACTOR_INV_SRC1_COLOR	25U
#define GEN12_BLENDFACTOR_INV_SRC1_ALPHA	26U

/* 3D_Color_Buffer_Blend_Function (BLENDFUNCTION_*). */
#define GEN12_BLENDFUNCTION_ADD			0U
#define GEN12_BLENDFUNCTION_SUBTRACT		1U
#define GEN12_BLENDFUNCTION_REVERSE_SUBTRACT	2U
#define GEN12_BLENDFUNCTION_MIN			3U
#define GEN12_BLENDFUNCTION_MAX			4U

/*
 * The GPGPU pipeline's commands and the interface descriptor (ws101-p004),
 * from Mesa 25.0.7 (MIT; Debian source package 25.0.7-2+deb13u1; values are
 * Intel PRM hardware facts).  gen120.xml imports them from:
 *   src/intel/genxml/gen110.xml (MEDIA_VFE_STATE, MEDIA_STATE_FLUSH)
 *     6598e556ffedf4fe051c78af3bb08030a39af865c76e2784dba5374646fce35d
 *   src/intel/genxml/gen80.xml (GPGPU_WALKER)
 *     2962677cf69dc947345fd88bd7010427900160eb7a7b076463e6e8d28772439d
 *   src/intel/genxml/gen60.xml (MEDIA_CURBE_LOAD, MEDIA_INTERFACE_DESCRIPTOR_LOAD)
 *     30fac841448b4239bbaedf92a77424ec054629d28f266c7190b3f592f4b9185c
 *   src/intel/genxml/gen120.xml (INTERFACE_DESCRIPTOR_DATA)
 *     e2452c7dd2d19f9c506f487ce98e938984b6bdf8a3ea2504c64afdd4facd542e
 * The values programmed follow anv (src/intel/vulkan/genX_pipeline.c
 *     9bf244df1284531767529980909176d09b72db037b6fdaa7f5874d997ebf7545,
 * genX_cmd_compute.c
 *     c0455a006e15a2b719c2545f818e3084a13fb5a959c86d213b2386d976311349)
 * and the compute test that ran on the Latitude 5330 (tests/execution/eu-test.c).
 */

/* PIPELINE_SELECT's value for the GPGPU pipeline. */
#define GEN12_PIPELINE_SELECT_GPGPU		2U

/* The headers of the media commands, their DWord Length already in (a fixed length each). */
#define GEN12_MEDIA_VFE_STATE_HEADER		0x70000007U
#define GEN12_MEDIA_CURBE_LOAD_HEADER		0x70010002U
#define GEN12_MEDIA_INTERFACE_DESCRIPTOR_LOAD_HEADER	0x70020002U
#define GEN12_MEDIA_STATE_FLUSH_HEADER		0x70040000U
#define GEN12_GPGPU_WALKER_HEADER		0x7105000dU

/* Packet lengths. */
#define GEN12_MEDIA_VFE_STATE_DWORDS		9U
#define GEN12_MEDIA_CURBE_LOAD_DWORDS		4U
#define GEN12_MEDIA_INTERFACE_DESCRIPTOR_LOAD_DWORDS	4U
#define GEN12_MEDIA_STATE_FLUSH_DWORDS		2U
#define GEN12_GPGPU_WALKER_DWORDS		15U
#define GEN12_INTERFACE_DESCRIPTOR_DWORDS	8U

/*
 * MEDIA_VFE_STATE: dword 1 the Per Thread Scratch Space (bits 3:0) and the
 * Scratch Space Base Pointer's bits 31:10, dword 2 its bits 47:32; dword 3
 * the Maximum Number of Threads (31:16) and the Number of URB Entries
 * (15:8); dword 5 the URB Entry Allocation Size (31:16) and the CURBE
 * Allocation Size (15:0, in registers).
 */
#define GEN12_VFE_MAX_THREADS_SHIFT		16U
#define GEN12_VFE_URB_ENTRIES_SHIFT		8U
#define GEN12_VFE_URB_ALLOCATION_SHIFT		16U

/* The URB entries and their size anv and the compute test program (they carry no push data on Gen12). */
#define GEN12_VFE_URB_ENTRIES			2U
#define GEN12_VFE_URB_ALLOCATION		2U

/* The threads one dual-subslice runs, the VFE's Maximum Number of Threads being that times the dual-subslices, less one. */
#define GEN12_VFE_THREADS_PER_DSS		112U

/*
 * INTERFACE_DESCRIPTOR_DATA: dword 2 bit 20 Thread Preemption Disable;
 * dword 5 the Constant URB Entry Read Length (31:16, the per-thread
 * registers); dword 6 the Number of Threads in GPGPU Thread Group (9:0),
 * the Shared Local Memory Size (20:16) and Barrier Enable (21); dword 7 the
 * Cross-Thread Constant Data Read Length (7:0, registers).
 */
#define GEN12_IDD_PREEMPTION_DISABLE		(1U << 20)
#define GEN12_IDD_PER_THREAD_LENGTH_SHIFT	16U
#define GEN12_IDD_SLM_SIZE_SHIFT		16U
#define GEN12_IDD_BARRIER_ENABLE		(1U << 21)

/*
 * GPGPU_WALKER: dword 0 bit 10 Indirect Parameter Enable; dword 4 the SIMD
 * Size (31:30, 0 for SIMD8) and the Thread Width Counter Maximum (5:0);
 * dwords 7, 10 and 12 the group counts along x, y and z; dwords 13 and 14
 * the right and the bottom execution masks.
 */
#define GEN12_WALKER_INDIRECT			(1U << 10)
#define GEN12_WALKER_SIMD8			0U
#define GEN12_WALKER_SIMD_SHIFT			30U

/*
 * The registers an indirect GPGPU_WALKER takes its group counts from
 * (ws101-p007): GPGPU_DISPATCHDIMX, _DIMY and _DIMZ of gen120.xml's render
 * command streamer, which anv loads with MI_LOAD_REGISTER_MEM from the
 * indirect buffer (genX_cmd_compute.c, compute_load_indirect_params).
 */
#define GEN12_GPGPU_DISPATCHDIMX		0x2500U
#define GEN12_GPGPU_DISPATCHDIMY		0x2504U
#define GEN12_GPGPU_DISPATCHDIMZ		0x2508U

/* The dwords of MI_LOAD_REGISTER_MEM: the header, the register, the address's two words. */
#define GEN12_MI_LOAD_REGISTER_MEM_DWORDS	4U

#endif /* DRIVERS_GPU_I915_INTEL_GENXML_H */
