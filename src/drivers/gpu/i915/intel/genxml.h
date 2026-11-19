/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Gen12 3D and GPGPU pipeline commands and state fields: the command
 * opcodes and packet lengths, the structure sizes, and the field values and
 * registers the render path programs.
 *
 * The 3D pipeline command header dword is
 *   (Command Type 3 << 29) | (Pipeline 3 << 27) | (Opcode << 24) |
 *   (Sub Opcode << 16) | (DWord Length = total dwords - 2)
 * The high sixteen bits of that dword are the opcode constants below.
 */

#ifndef DRIVERS_GPU_I915_INTEL_GENXML_H
#define DRIVERS_GPU_I915_INTEL_GENXML_H

/* Command opcodes (high sixteen bits of the header dword). */
/*
 * PIPELINE_SELECT is Command Type 3 / Command SubType 1 / Opcode 1 /
 * SubOpcode 4, so its header is 0x6904xxxx; 0x6104xxxx (SubType 0) is a
 * different command.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml and the files it
 * imports (PIPELINE_SELECT, STATE_BASE_ADDRESS, 3DSTATE_VS, 3DSTATE_PS,
 * 3DSTATE_PS_EXTRA, 3DSTATE_VERTEX_ELEMENTS, 3DPRIMITIVE).
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

/*
 * Packet lengths.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml (the "length" of
 * each instruction above).
 */
#define GEN12_3DSTATE_VS_DWORDS			9U
#define GEN12_3DSTATE_PS_DWORDS			12U
#define GEN12_3DSTATE_VERTEX_ELEMENTS_DWORDS	3U
#define GEN12_3DPRIMITIVE_DWORDS		7U
#define GEN12_PIPELINE_SELECT_DWORDS		1U
#define GEN12_STATE_BASE_ADDRESS_DWORDS		22U

/*
 * PIPELINE_SELECT: the 3D pipeline is selection value 0.  Gen12 applies the
 * selection only when its mask bits (15:8) cover it; the media sampler DOP
 * clock gate (bit 4) is masked in and set with it.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml (PIPELINE_SELECT:
 * Pipeline Selection, Mask Bits, Media Sampler DOP Clock Gate Enable).
 */
#define GEN12_PIPELINE_SELECT_3D		0U
#define GEN12_PIPELINE_SELECT_MASK		(0x13U << 8)
#define GEN12_PIPELINE_SELECT_MEDIA_DOP_GATE	(1U << 4)
#define GEN12_PIPELINE_SELECT_DWORD(pipeline)	\
	((GEN12_CMD_PIPELINE_SELECT << 16) | GEN12_PIPELINE_SELECT_MASK | \
	 GEN12_PIPELINE_SELECT_MEDIA_DOP_GATE | (pipeline))
/*
 * Fixed reference words, not derived from the macros above.
 */
_Static_assert(GEN12_PIPELINE_SELECT_DWORD(0U) == 0x69041310U,
	"Gen12 PIPELINE_SELECT(3D) must be 0x69041310");
_Static_assert(GEN12_PIPELINE_SELECT_DWORD(2U) == 0x69041312U,
	"Gen12 PIPELINE_SELECT(GPGPU) must be 0x69041312");


/*
 * The rest of the 3D pipeline.  Each opcode is the high sixteen bits of the
 * packet header and each DWORDS is the packet's total dword count, so
 * GEN12_CMD_HEADER() produces the header with the right DWord Length.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml and the files it
 * imports (the opcode fields and "length" of each instruction named below).
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

/*
 * Structure sizes in dwords.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml (the "length" of
 * RENDER_SURFACE_STATE, VERTEX_BUFFER_STATE, VERTEX_ELEMENT_STATE,
 * COLOR_CALC_STATE, BLEND_STATE, BLEND_STATE_ENTRY, CC_VIEWPORT).
 */
#define GEN12_RENDER_SURFACE_STATE_DWORDS	16U
#define GEN12_VERTEX_BUFFER_STATE_DWORDS	4U
#define GEN12_VERTEX_ELEMENT_STATE_DWORDS	2U
#define GEN12_COLOR_CALC_STATE_DWORDS		6U
#define GEN12_BLEND_STATE_DWORDS		1U
#define GEN12_BLEND_STATE_ENTRY_DWORDS		2U
#define GEN12_CC_VIEWPORT_DWORDS		2U

/*
 * 3D_Prim_Topo_Type: a screen-space rectangle needs no vertex shader; the
 * rest are Vulkan's list, strip and fan topologies.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen70.xml
 * (3D_Prim_Topo_Type).
 */
#define GEN12_3DPRIM_RECTLIST			15U
#define GEN12_3DPRIM_TRILIST			4U
#define GEN12_3DPRIM_POINTLIST			1U
#define GEN12_3DPRIM_LINELIST			2U
#define GEN12_3DPRIM_LINESTRIP			3U
#define GEN12_3DPRIM_TRISTRIP			5U
#define GEN12_3DPRIM_TRIFAN			6U

/*
 * 3D_Prim_Topo_Type of the topologies with adjacency, which only a draw
 * with a geometry shader takes: the line list and strip, the triangle list
 * and strip.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen70.xml (3D_Prim_Topo_Type
 * LINELIST_ADJ .. TRISTRIP_ADJ).
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
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml (3DSTATE_SF
 * Triangle Fan Provoking Vertex Select), src/intel/genxml/gen80.xml
 * (3DSTATE_CLIP Triangle Fan Provoking Vertex Select).
 */
#define GEN12_SF_FAN_PROVOKING_SHIFT		25U
#define GEN12_CLIP_FAN_PROVOKING_SHIFT		0U
#define GEN12_FAN_PROVOKING_SECOND		1U

/*
 * The point fields of 3DSTATE_SF dword 3: Point Width in bits 10:0 as u8.3
 * (8 is 1.0), Point Width Source in bit 11 (0 the vertex's, from dword 3 of
 * its VUE header; 1 the Point Width field).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml (3DSTATE_SF Point
 * Width, Point Width Source).
 */
#define GEN12_SF_POINT_WIDTH_ONE		8U
#define GEN12_SF_POINT_WIDTH_FROM_STATE		(1U << 11)

/*
 * 3DSTATE_WM dword 1 bits 16:11, Barycentric Interpolation Mode: one bit to
 * each mode, and the pixel thread payload carries the barycentrics of each
 * mode set in bit order: the perspective ones at the pixel centre, then the
 * linear ones.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen80.xml (3DSTATE_WM
 * Barycentric Interpolation Mode).
 */
#define GEN12_WM_BARYCENTRIC_PERSPECTIVE_PIXEL	(1U << 11)
#define GEN12_WM_BARYCENTRIC_LINEAR_PIXEL	(1U << 14)

/*
 * 3DSTATE_CLIP dword 2 bit 8, Non-Perspective Barycentric Enable: the
 * clipper passes on what the linear barycentrics need; it is set when the
 * pixel kernel interpolates without perspective.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen80.xml (3DSTATE_CLIP
 * Non-Perspective Barycentric Enable).
 */
#define GEN12_CLIP_NON_PERSPECTIVE_BARYCENTRIC	(1U << 8)

/*
 * 3DSTATE_CLIP dword 3 bit 5, Force Zero RTA Index Enable: every primitive
 * goes to render target array index 0.  It is set unless the last vertex
 * processing stage writes the layer.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen80.xml (3DSTATE_CLIP
 * Force Zero RTA Index Enable).
 */
#define GEN12_CLIP_FORCE_ZERO_RTA_INDEX		(1U << 5)

/*
 * 3DSTATE_GS dword 7 bits 12:11, Dispatch Mode: SIMD8, one input primitive
 * to a channel.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen110.xml (3DSTATE_GS
 * Dispatch Mode, DISPATCH_MODE_SIMD8).
 */
#define GEN12_GS_DISPATCH_MODE_SIMD8		3U

/*
 * 3DSTATE_GS dword 7 bits 19:15, Instance Control: the instances the stage
 * runs for each input primitive, less one (up to 32).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen90.xml (3DSTATE_GS
 * Instance Control).
 */
#define GEN12_GS_INSTANCE_CONTROL_SHIFT		15U
#define GEN12_GS_INSTANCE_CONTROL_MASK		0x1fU

/*
 * 3DSTATE_STREAMOUT dword 1 bit 30, API Rendering Disable: the primitives
 * go no further than the stream output stage, so the vertex shader runs and
 * nothing is rasterized.  Rasterizer discard sets it whether or not stream
 * output is on.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml
 * (3DSTATE_STREAMOUT API Rendering Disable).
 */
#define GEN12_STREAMOUT_RENDERING_DISABLE	(1U << 30)

/*
 * 3DSTATE_PS_EXTRA dword 1: bit 23 Pixel Shader Uses Source W and bit 24
 * Pixel Shader Uses Source Depth, which put the interpolated w and depth of
 * each pixel into the thread payload after the barycentrics.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen110.xml
 * (3DSTATE_PS_EXTRA).
 */
#define GEN12_3DSTATE_PS_EXTRA_USES_SOURCE_W	(1U << 23)
#define GEN12_3DSTATE_PS_EXTRA_USES_SOURCE_DEPTH	(1U << 24)

/*
 * 3DSTATE_SBE dword 1 bit 20 Point Sprite Texture Coordinate Origin (0 the
 * upper left, Vulkan's), dword 2 Point Sprite Texture Coordinate Enable, one
 * bit to an attribute the setup replaces with the point's texture
 * coordinate.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen90.xml (3DSTATE_SBE).
 */
#define GEN12_SBE_POINT_SPRITE_ORIGIN_SHIFT		20U
#define GEN12_SBE_POINT_SPRITE_ORIGIN_UPPER_LEFT	0U

/*
 * gl_PrimitiveID that no stage writes: 3DSTATE_SBE dword 1 bits 4:0
 * Primitive ID Override Attribute Select and bits 19:16 Primitive ID
 * Override Component X..W, and the attribute's SF_OUTPUT_ATTRIBUTE_DETAIL in
 * SBE_SWIZ: Constant Source PRIM_ID (3) in bits 10:9 and Component Override
 * X..W in bits 15:12.  Both are set together.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen90.xml (3DSTATE_SBE),
 * src/intel/genxml/gen60.xml (SF_OUTPUT_ATTRIBUTE_DETAIL).
 */
#define GEN12_SBE_PRIMITIVE_ID_OVERRIDE_XYZW		(0xFU << 16)
#define GEN12_SBE_SWIZ_PRIMITIVE_ID			((3U << 9) | (0xFU << 12))

/*
 * 3D_Vertex_Component_Control.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen80.xml
 * (3D_Vertex_Component_Control).
 */
#define GEN12_VFCOMP_NOSTORE			0U
#define GEN12_VFCOMP_STORE_SRC			1U
#define GEN12_VFCOMP_STORE_0			2U
#define GEN12_VFCOMP_STORE_1_FP			3U
#define GEN12_VFCOMP_STORE_1_INT		4U

/*
 * 3DSTATE_VF_SGVS dword 1: the vertex ID into component 0 of the element at
 * bits 5:0 (enable bit 15), the instance ID into component 0 of the element
 * at bits 21:16 (enable bit 31).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen80.xml (3DSTATE_VF_SGVS).
 */
#define GEN12_SGVS_VERTEX_ID_ENABLE		(1U << 15)
#define GEN12_SGVS_INSTANCE_ID_SHIFT		16U
#define GEN12_SGVS_INSTANCE_ID_ENABLE		(1U << 31)

/*
 * 3DSTATE_VF_SGVS_2 dword 1 (Gen11 and Gen12): extended parameter 0 into
 * the component at bits 14:13 of the element at bits 5:0 (enable bit 15;
 * bit 12 clear, the parameter the 3DPRIMITIVE gives), extended parameter 1
 * into the component at bits 30:29 of the element at bits 21:16 (enable
 * bit 31; bit 28 set, the draw's start instance location).
 *
 * Values referenced: Mesa 26.2.6 src/intel/genxml/gen110.xml
 * (3DSTATE_VF_SGVS_2).
 */
#define GEN11_SGVS2_XP0_COMPONENT_SHIFT		13U
#define GEN11_SGVS2_XP0_ENABLE			(1U << 15)
#define GEN11_SGVS2_XP1_ELEMENT_SHIFT		16U
#define GEN11_SGVS2_XP1_START_INSTANCE		(1U << 28)
#define GEN11_SGVS2_XP1_COMPONENT_SHIFT		29U
#define GEN11_SGVS2_XP1_ENABLE			(1U << 31)

/*
 * Surface formats.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml (SURFACE_FORMAT).
 */
#define GEN12_FORMAT_R32G32B32A32_FLOAT		0U
#define GEN12_FORMAT_R32G32B32_FLOAT		64U
#define GEN12_FORMAT_B8G8R8A8_UNORM		192U

/*
 * RENDER_SURFACE_STATE surface types, alignments and tile modes.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml
 * (RENDER_SURFACE_STATE Surface Type, Surface Vertical Alignment, Tile Mode).
 */
#define GEN12_SURFTYPE_2D			1U
#define GEN12_SURFTYPE_3D			2U
#define GEN12_SURFTYPE_CUBE			3U
#define GEN12_SURFTYPE_BUFFER			4U
#define GEN12_SURFTYPE_NULL			7U
#define GEN12_SURFACE_ALIGN_4			1U
#define GEN12_TILEMODE_LINEAR			0U
#define GEN12_TILEMODE_YMAJOR			3U

/*
 * BLEND_STATE_ENTRY colour clamp range.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen80.xml (BLEND_STATE_ENTRY
 * Color Clamp Range).
 */
#define GEN12_COLORCLAMP_RTFORMAT		2U

/* URB allocations are made in 8 KiB chunks and entries in 64-byte units. */
#define GEN12_URB_CHUNK_KB			8U

/*
 * The URB the vertex stage owns past the 32 KiB of push constants on the
 * target (3576 entries of 64 bytes fill it), and the most vertex entries.
 *
 * Values referenced: Mesa 25.0.7 src/intel/dev/intel_device_info.c
 * (GFX12_HW_INFO urb.max_entries).
 */
#define GEN12_URB_VS_BYTES			(3576U * 64U)
#define GEN12_URB_VS_ENTRIES			3576U

/*
 * With a geometry shader the vertex stage's URB is split: the vertex stage
 * keeps 21 of its chunks from chunk 4, the geometry stage the next 6
 * (48 KiB), so the URB in use does not grow.  A geometry stage needs at
 * least 2 entries (it runs in DUAL_OBJECT mode) and has at most 1548.
 *
 * Values referenced: Mesa 25.0.7 src/intel/common/intel_urb_config.c
 * (min_entries), src/intel/dev/intel_device_info.c (GFX12_HW_INFO
 * urb.max_entries).
 */
#define GEN12_URB_VS_START_CHUNK		4U
#define GEN12_URB_VS_SPLIT_CHUNKS		21U
#define GEN12_URB_GS_CHUNKS			6U
#define GEN12_URB_GS_MIN_ENTRIES		2U
#define GEN12_URB_GS_ENTRIES			1548U

/*
 * 3DSTATE_SF deref block size.
 *
 * Values referenced: Mesa 25.0.7 src/intel/common/intel_l3_config.h
 * (intel_urb_deref_block_size).
 */
#define GEN12_URB_DEREF_BLOCK_SIZE_32		0U
#define GEN12_URB_DEREF_BLOCK_SIZE_PER_POLY	1U

/* The pixel shader dispatch field is programmed as the thread count less one. */
#define GEN12_MAX_THREADS_PER_PSD		64U

/* IEEE-754 bit patterns; the kernel builds vertex data without floating point. */
#define GEN12_F32_0				0x00000000U
#define GEN12_F32_1				0x3f800000U

/*
 * Pipeline statistics counters (64-bit registers).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen70.xml
 * (IA_VERTICES_COUNT, IA_PRIMITIVES_COUNT, VS_INVOCATION_COUNT,
 * CL_INVOCATION_COUNT, CL_PRIMITIVES_COUNT, PS_INVOCATION_COUNT).
 */
#define GEN12_REG_IA_VERTICES_COUNT		0x2310U
#define GEN12_REG_IA_PRIMITIVES_COUNT		0x2318U
#define GEN12_REG_VS_INVOCATION_COUNT		0x2320U
#define GEN12_REG_CL_INVOCATION_COUNT		0x2338U
#define GEN12_REG_CL_PRIMITIVES_COUNT		0x2340U
#define GEN12_REG_PS_INVOCATION_COUNT		0x2348U

/*
 * The L3 cache is partitioned before the URB can hold a single vertex entry:
 * 32 ways of URB and 88 ways shared, written into L3ALLOC (URB in bits 7:1,
 * All in bits 31:25).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml (L3ALLOC),
 * src/intel/common/intel_l3_config.c (tgl_l3_configs).
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
 * A disabled depth buffer is still a typed one: SURFTYPE_NULL is paired
 * with D32_FLOAT, because format zero names a depth-stencil format and
 * makes the depth unit expect a stencil buffer that is not there.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml
 * (3DSTATE_DEPTH_BUFFER Surface Format).
 */
#define GEN12_DEPTH_FORMAT_D32_FLOAT		1U
#define GEN12_DEPTH_FORMAT_D16_UNORM		5U

/*
 * 3DSTATE_SBE attribute component format: all four channels active.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen90.xml (3DSTATE_SBE
 * Attribute Active Component Format).
 */
#define GEN12_ACF_XYZW				3U

/*
 * Fragments that reached the depth test, whether or not a shader ran.
 *
 * Values referenced: Mesa 25.0.7 src/intel/perf/intel_perf_regs.h
 * (PS_DEPTH_COUNT).
 */
#define GEN12_REG_PS_DEPTH_COUNT		0x2350U

/*
 * Every MOCS field carries the cache-table index shifted left by one; bit
 * zero of the field is reserved.  Index 2 is write-back and index 3
 * uncached in the Gen12 MOCS table; writing a raw index selects the entry
 * one bit to the right, and entries 0 and 1 are reserved.
 *
 * Values referenced: Mesa 25.0.7 src/intel/isl/isl.c (isl_device_setup_mocs),
 * Linux v6.19 drivers/gpu/drm/i915/gt/intel_mocs.c (tgl_mocs_table).
 */
#define GEN12_MOCS(index)			((index) << 1)

/*
 * 3DSTATE_RASTER cull modes.  Zero means cull both faces, so a rasterizer
 * left at its zeroed default discards every primitive; CULLMODE_NONE is
 * always named explicitly.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen80.xml (3DSTATE_RASTER
 * Cull Mode).
 */
#define GEN12_CULLMODE_BOTH			0U
#define GEN12_CULLMODE_NONE			1U
#define GEN12_CULLMODE_FRONT			2U
#define GEN12_CULLMODE_BACK			3U

/*
 * State programmed once per render context before any draw: the
 * depth-clear override packet cleared, sample positions, depth bounds and
 * the other stages' binding table pointers.  A context that skips them runs
 * with whatever the hardware or a previous owner left behind.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml and the files it
 * imports (the opcode fields and "length" of the instructions below).
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/vulkan/genX_init_state.c
 * (init_render_queue_state).
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

/*
 * 3DSTATE_SAMPLE_PATTERN dword 8: the single sample sits at the pixel
 * centre (u0.4 0.5).
 *
 * Values referenced: Mesa 25.0.7 src/intel/common/intel_sample_positions.h
 * (INTEL_SAMPLE_POS_1X).
 */
#define GEN12_SAMPLE_PATTERN_1X_CENTRE		((8U << 20) | (8U << 16))

/*
 * 3DSTATE_SAMPLE_PATTERN dword 8 bits 15:0 and dword 7: the standard two
 * and four sample positions, each sample a byte of X (bits 7:4) and Y (bits
 * 3:0) in sixteenths, sample 0 in the lowest byte: 2x (0.75, 0.75) and
 * (0.25, 0.25); 4x (0.375, 0.125), (0.875, 0.375), (0.125, 0.625) and
 * (0.625, 0.875).
 *
 * Values referenced: Mesa 25.0.7 src/intel/common/intel_sample_positions.h
 * (INTEL_SAMPLE_POS_2X, INTEL_SAMPLE_POS_4X).
 */
#define GEN12_SAMPLE_PATTERN_2X			((0x44U << 8) | 0xccU)
#define GEN12_SAMPLE_PATTERN_4X			((0xaeU << 24) | (0x2aU << 16) | (0xe6U << 8) | 0x62U)

/*
 * Instruction fetches fill the per-subslice instruction cache through the
 * L3, so the instruction base needs a write-back entry, never an uncached
 * one.
 *
 * Values referenced: Mesa 25.0.7 src/intel/isl/isl.c (isl_device_setup_mocs).
 */
#define I915_MOCS_WRITEBACK_INDEX		2U

/*
 * Push constants.  A 3DSTATE_CONSTANT_* is programmed for each stage before
 * its first draw even with nothing to push, and the pixel stage gets the
 * whole 32 KiB push constant region when it is the only user.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen90.xml
 * (3DSTATE_CONSTANT_VS and the other stages' 3DSTATE_CONSTANT_*).
 */
#define GEN12_CMD_3DSTATE_CONSTANT_VS		0x7815U
#define GEN12_CMD_3DSTATE_CONSTANT_GS		0x7816U
#define GEN12_CMD_3DSTATE_CONSTANT_PS		0x7817U
#define GEN12_CMD_3DSTATE_CONSTANT_HS		0x7819U
#define GEN12_CMD_3DSTATE_CONSTANT_DS		0x781AU
#define GEN12_3DSTATE_CONSTANT_DWORDS		11U
#define GEN12_PUSH_CONSTANT_KB			32U

/*
 * VERTEX_BUFFER_STATE: vertex fetches stay in the L3 on this generation.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml
 * (VERTEX_BUFFER_STATE L3 Bypass Disable).
 */
#define GEN12_VERTEX_BUFFER_L3_BYPASS_DISABLE	(1U << 25)

/*
 * Coarse pixel shading state is fetched through a pointer relative to the
 * dynamic state base; it points at a disabled CPS_STATE from device
 * initialisation.  The binding table pool is disabled the same way, so no
 * state of another context leaks in.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml
 * (3DSTATE_CPS_POINTERS, CPS_STATE), src/intel/genxml/gen80.xml
 * (3DSTATE_BINDING_TABLE_POOL_ALLOC).
 */
#define GEN12_CMD_3DSTATE_CPS_POINTERS		0x7822U
#define GEN12_CPS_STATE_DWORDS			8U

#define GEN12_CMD_3DSTATE_BINDING_TABLE_POOL_ALLOC	0x7919U
#define GEN12_3DSTATE_BINDING_TABLE_POOL_ALLOC_DWORDS	4U

/*
 * PIPE_CONTROL dword 1 bit 1 stalls the command streamer until the pixel
 * scoreboard is idle; bit 13 is Depth Stall Enable.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml (PIPE_CONTROL).
 */
#define PIPE_CONTROL_STALL_AT_SCOREBOARD	(1U << 1)
#define PIPE_CONTROL_DEPTH_STALL_ENABLE		(1U << 13)

/*
 * State pointers and the attribute swizzle the draw path programs.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml and the files it
 * imports (3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP,
 * 3DSTATE_SCISSOR_STATE_POINTERS, 3DSTATE_SAMPLER_STATE_POINTERS_PS, _VS
 * and _GS, 3DSTATE_SBE_SWIZ).
 */
#define GEN12_CMD_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP	0x7821U
#define GEN12_CMD_3DSTATE_SCISSOR_STATE_POINTERS	0x780FU
#define GEN12_CMD_3DSTATE_SAMPLER_STATE_POINTERS_PS	0x782FU
#define GEN12_CMD_3DSTATE_SAMPLER_STATE_POINTERS_VS	0x782BU
#define GEN12_CMD_3DSTATE_SAMPLER_STATE_POINTERS_GS	0x782EU
#define GEN12_CMD_3DSTATE_SBE_SWIZ		0x7851U
#define GEN12_3DSTATE_SBE_SWIZ_DWORDS		11U

/*
 * The index buffer of an indexed draw: dword 1 carries the MOCS in bits
 * 6:0, the Index Format in bits 9:8 and the L3 Bypass Disable in bit 11;
 * dwords 2 and 3 the address and dword 4 the size in bytes.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml
 * (3DSTATE_INDEX_BUFFER).
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
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml (3DPRIMITIVE).
 */
#define GEN12_3DPRIMITIVE_VERTEX_RANDOM		(1U << 8)

/*
 * 3DSTATE_VS and 3DSTATE_PS dwords 4-5, the same in both: Per-Thread
 * Scratch Space in bits 3:0 of dword 4, the scratch memory of one thread as
 * 1 KiB << n (n up to 11, 2 MiB); Scratch Space Base Pointer in bits 63:10
 * of the two dwords, an address relative to the General State Base
 * Address.  A thread's scratch starts at the pointer plus its thread id
 * times the per-thread space.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen110.xml (3DSTATE_VS),
 * src/intel/genxml/gen120.xml (3DSTATE_PS).
 */
#define GEN12_SCRATCH_SPACE_MASK		0x0000000fU
#define GEN12_SCRATCH_POINTER_ALIGN		1024U
#define GEN12_SCRATCH_SPACE_MIN_BYTES		1024U
#define GEN12_SCRATCH_SPACE_MAX			11U

/*
 * 3DSTATE_PS dword 6 bit 11 (Push Constant Enable): the pixel threads get
 * the push constants of 3DSTATE_CONSTANT_PS in front of their setup data.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml (3DSTATE_PS).
 */
#define GEN12_3DSTATE_PS_PUSH_CONSTANT_ENABLE	(1U << 11)

/*
 * 3DSTATE_PS_EXTRA dword 1 bit 28 (Pixel Shader Kills Pixel): the pixel
 * kernel may discard pixels, so the pixel stage must not write depth or
 * stencil for a pixel before the kernel has decided.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen110.xml
 * (3DSTATE_PS_EXTRA).
 */
#define GEN12_3DSTATE_PS_EXTRA_KILLS_PIXEL	(1U << 28)

/*
 * 3DSTATE_PS_EXTRA dword 1 bit 2 (Pixel Shader Has UAV): the pixel kernel
 * has effects besides its render-target writes, so the pixel stage runs it
 * for every pixel it passes.  Mesa sets it while an occlusion query is
 * active, for a correct PS_DEPTH_COUNT.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen110.xml and
 * gen90.xml (3DSTATE_PS_EXTRA); procedure referenced: Mesa 25.0.7
 * src/intel/vulkan/genX_gfx_state.c (update_ps_extra_has_uav).
 */
#define GEN12_3DSTATE_PS_EXTRA_HAS_UAV		(1U << 2)

/*
 * 3DSTATE_PS_EXTRA dword 1: bits 27:26 Pixel Shader Computed Depth Mode
 * (1, PSCDEPTH_ON: every render-target write carries the depth the kernel
 * computed), bit 29 oMask Present to Render Target (every write carries an
 * oMask before its colour), bits 1:0 Input Coverage Mask State (1, NORMAL:
 * the payload carries each pixel's coverage mask after the source w).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen110.xml
 * (3DSTATE_PS_EXTRA).
 */
#define GEN12_3DSTATE_PS_EXTRA_COMPUTED_DEPTH		(1U << 26)
#define GEN12_3DSTATE_PS_EXTRA_OMASK			(1U << 29)
#define GEN12_3DSTATE_PS_EXTRA_COVERAGE_MASK_NORMAL	(1U << 0)

/*
 * The mip fields of RENDER_SURFACE_STATE dword 5: MIP Count / LOD in bits
 * 3:0, Surface Min LOD in bits 7:4, Mip Tail Start LOD in bits 11:8.  A
 * sampled surface reads levels [Surface Min LOD, Surface Min LOD + MIP
 * Count]; a render target writes level MIP Count.  Surface QPitch is dword
 * 1 bits 14:0, in units of four rows.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml
 * (RENDER_SURFACE_STATE).
 */
#define GEN12_RSS_MIP_COUNT_SHIFT		0U
#define GEN12_RSS_SURFACE_MIN_LOD_SHIFT		4U
#define GEN12_RSS_MIP_TAIL_START_SHIFT		8U
#define GEN12_RSS_LOD_MASK			0xfU
#define GEN12_RSS_QPITCH_MASK			0x7fffU

/*
 * The array fields of RENDER_SURFACE_STATE: dword 0 Surface Array bit 28
 * and the six Cube Face Enables in bits 5:0; dword 3 Depth in bits 31:21
 * (the layers less one, the cubes less one, or a 3D surface's depth less
 * one); dword 4 Minimum Array Element in bits 28:18 and Render Target View
 * Extent in bits 17:7.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen110.xml
 * (RENDER_SURFACE_STATE).
 */
#define GEN12_RSS_SURFACE_ARRAY			(1U << 28)
#define GEN12_RSS_CUBE_FACES_ALL		0x3fU
#define GEN12_RSS_DEPTH_SHIFT			21U
#define GEN12_RSS_DEPTH_MASK			0x7ffU
#define GEN12_RSS_MIN_ARRAY_ELEMENT_SHIFT	18U
#define GEN12_RSS_VIEW_EXTENT_SHIFT		7U

/*
 * RENDER_SURFACE_STATE dword 1 bit 31, Enable Unorm Path In Color Pipe;
 * Gen9 has no such bit.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml
 * (RENDER_SURFACE_STATE).
 */
#define GEN12_RSS_UNORM_PATH			(1U << 31)

/*
 * The multisample fields: RENDER_SURFACE_STATE dword 4 Number of
 * Multisamples in bits 5:3 (the samples' log2) with Multisampled Surface
 * Storage Format bit 6 left MSFMT_MSS (every sample a slice, QPitch apart);
 * 3DSTATE_MULTISAMPLE dword 1 Number of Multisamples in bits 3:1;
 * 3DSTATE_RASTER dword 1 DX Multisample Rasterization Enable bit 12, which
 * with the DX10.1 API mode rasterizes on the sample pattern.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml and the files it
 * imports (RENDER_SURFACE_STATE, 3DSTATE_MULTISAMPLE, 3DSTATE_RASTER).
 */
#define GEN12_RSS_MULTISAMPLES_SHIFT		3U
#define GEN12_MULTISAMPLE_COUNT_SHIFT		1U
#define GEN12_RASTER_DX_MULTISAMPLE_ENABLE	(1U << 12)

/*
 * 3DSTATE_RASTER dword 1: Viewport Z Far Clip Test Enable bit 0 and Near
 * bit 26 (off for a clamped depth), Front and Back Face Fill Mode in bits
 * 6:5 and 4:3 (FILL_MODE_SOLID 0, WIREFRAME 1, POINT 2, which are
 * VkPolygonMode's values), Global Depth Offset Enable Point, Wireframe and
 * Solid in bits 7, 8 and 9; dwords 2, 3 and 4 the Global Depth Offset
 * Constant, Scale and Clamp as floats (a clamp of 0 clamps nothing, as
 * Vulkan's).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen90.xml and gen120.xml
 * (3DSTATE_RASTER), src/intel/vulkan/genX_gfx_state.c (3DSTATE_RASTER).
 */
#define GEN12_RASTER_Z_FAR_CLIP			(1U << 0)
#define GEN12_RASTER_Z_NEAR_CLIP		(1U << 26)
#define GEN12_RASTER_BACK_FILL_SHIFT		3U
#define GEN12_RASTER_FRONT_FILL_SHIFT		5U
#define GEN12_RASTER_FILL_MODE_MASK		3U
#define GEN12_RASTER_DEPTH_OFFSET_ALL		((1U << 7) | (1U << 8) | (1U << 9))

/*
 * SAMPLER_STATE fields, in dword and bit:
 *   dword 0: Texture LOD Bias 13:1 (s4.8), Min Mode Filter 16:14,
 *            Mag Mode Filter 19:17, Mip Mode Filter 21:20, LOD PreClamp
 *            Mode 28:27
 *   dword 1: Max LOD 19:8 (u4.8), Min LOD 31:20 (u4.8)
 *   dword 3: the address rounding enables 18:13 and the TCX, TCY and TCZ
 *            address control modes 8:6, 5:3 and 2:0
 * Mip Mode Filter is MIPFILTER_NONE 0, NEAREST 1, LINEAR 3.  The LOD bias
 * is clamped to [-16, 15.996] and both LOD limits to [0, 14].
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml (SAMPLER_STATE),
 * src/intel/vulkan/genX_init_state.c (the LOD and bias clamps).
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
 * More SAMPLER_STATE fields: dword 0 Anisotropic Algorithm bit 0 (EWA
 * approximation 1); dword 1 Cube Surface Control Mode bit 0 (OVERRIDE 1)
 * and Shadow Function 3:1 (PREFILTEROP_*); dword 2 Border Color Pointer
 * 31:6 (the dynamic state offset of a 64-byte aligned
 * SAMPLER_BORDER_COLOR_STATE, whose first four dwords are the red, green,
 * blue and alpha, floats or integers as the format reads); dword 3 Maximum
 * Anisotropy 21:19 (RATIO 2:1 0 .. 16:1 7), Non-normalized Coordinate
 * Enable bit 10, the address modes TCX 8:6, TCY 5:3, TCZ 2:0.  Min and Mag
 * Mode Filter take MAPFILTER_ANISOTROPIC 2.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen110.xml (SAMPLER_STATE).
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

/*
 * The PREFILTEROP values of the Shadow Function.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen110.xml (SAMPLER_STATE
 * Shadow Function).
 */
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
 * Colour blending.
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
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen80.xml (BLEND_STATE,
 * BLEND_STATE_ENTRY), src/intel/genxml/gen90.xml (3DSTATE_PS_BLEND,
 * COLOR_CALC_STATE).
 */
#define GEN12_BLEND_INDEPENDENT_ALPHA		(1U << 30)
#define GEN12_BLEND_WRITE_DISABLE_BLUE		(1U << 0)

/*
 * BLEND_STATE dword 0: Alpha To Coverage Enable in bit 31, Alpha To One
 * Enable in bit 29; 3DSTATE_PS_BLEND dword 1: Alpha To Coverage Enable in
 * bit 31.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen80.xml (BLEND_STATE),
 * src/intel/genxml/gen90.xml (3DSTATE_PS_BLEND).
 */
#define GEN12_BLEND_ALPHA_TO_COVERAGE		(1U << 31)
#define GEN12_BLEND_ALPHA_TO_ONE		(1U << 29)
#define GEN12_PS_BLEND_ALPHA_TO_COVERAGE	(1U << 31)
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
/*
 * BLEND_STATE_ENTRY dword 1: the logic operation's enable and function
 * (bits 63 and 62:59 of the entry).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen80.xml (BLEND_STATE_ENTRY
 * Logic Op Enable, Logic Op Function).
 */
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

/*
 * 3D_Color_Buffer_Blend_Factor (BLENDFACTOR_*).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen40.xml
 * (3D_Color_Buffer_Blend_Factor).
 */
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

/*
 * 3D_Color_Buffer_Blend_Function (BLENDFUNCTION_*).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen40.xml
 * (3D_Color_Buffer_Blend_Function).
 */
#define GEN12_BLENDFUNCTION_ADD			0U
#define GEN12_BLENDFUNCTION_SUBTRACT		1U
#define GEN12_BLENDFUNCTION_REVERSE_SUBTRACT	2U
#define GEN12_BLENDFUNCTION_MIN			3U
#define GEN12_BLENDFUNCTION_MAX			4U

/* The GPGPU pipeline's commands and the interface descriptor. */

/*
 * PIPELINE_SELECT's value for the GPGPU pipeline.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml (PIPELINE_SELECT
 * Pipeline Selection).
 */
#define GEN12_PIPELINE_SELECT_GPGPU		2U

/*
 * The headers of the media commands, their DWord Length already in (a
 * fixed length each).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen110.xml (MEDIA_VFE_STATE,
 * MEDIA_STATE_FLUSH), src/intel/genxml/gen80.xml (GPGPU_WALKER),
 * src/intel/genxml/gen60.xml (MEDIA_CURBE_LOAD,
 * MEDIA_INTERFACE_DESCRIPTOR_LOAD).
 */
#define GEN12_MEDIA_VFE_STATE_HEADER		0x70000007U
#define GEN12_MEDIA_CURBE_LOAD_HEADER		0x70010002U
#define GEN12_MEDIA_INTERFACE_DESCRIPTOR_LOAD_HEADER	0x70020002U
#define GEN12_MEDIA_STATE_FLUSH_HEADER		0x70040000U
#define GEN12_GPGPU_WALKER_HEADER		0x7105000dU

/*
 * Packet lengths.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen110.xml, gen80.xml,
 * gen60.xml (the instructions above), src/intel/genxml/gen120.xml
 * (INTERFACE_DESCRIPTOR_DATA).
 */
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
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen110.xml
 * (MEDIA_VFE_STATE).
 */
#define GEN12_VFE_MAX_THREADS_SHIFT		16U
#define GEN12_VFE_URB_ENTRIES_SHIFT		8U
#define GEN12_VFE_URB_ALLOCATION_SHIFT		16U

/* The URB entries and their size (they carry no push data on Gen12). */
#define GEN12_VFE_URB_ENTRIES			2U
#define GEN12_VFE_URB_ALLOCATION		2U

/*
 * The threads one dual-subslice runs, the VFE's Maximum Number of Threads
 * being that times the dual-subslices, less one.
 *
 * Values referenced: Mesa 25.0.7 src/intel/dev/intel_device_info.c
 * (GFX12_HW_INFO max_cs_threads).
 */
#define GEN12_VFE_THREADS_PER_DSS		112U

/*
 * INTERFACE_DESCRIPTOR_DATA: dword 2 bit 20 Thread Preemption Disable;
 * dword 5 the Constant URB Entry Read Length (31:16, the per-thread
 * registers); dword 6 the Number of Threads in GPGPU Thread Group (9:0), the
 * Shared Local Memory Size (20:16) and Barrier Enable (21); dword 7 the
 * Cross-Thread Constant Data Read Length (7:0, registers).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml
 * (INTERFACE_DESCRIPTOR_DATA).
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
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen80.xml (GPGPU_WALKER).
 */
#define GEN12_WALKER_INDIRECT			(1U << 10)
#define GEN12_WALKER_SIMD8			0U
#define GEN12_WALKER_SIMD_SHIFT			30U

/*
 * The registers an indirect GPGPU_WALKER takes its group counts from,
 * loaded with MI_LOAD_REGISTER_MEM from the indirect buffer.
 *
 * Values referenced: Mesa 25.0.7 src/intel/vulkan/genX_cmd_compute.c
 * (GPGPU_DISPATCHDIMX, GPGPU_DISPATCHDIMY, GPGPU_DISPATCHDIMZ).
 */
#define GEN12_GPGPU_DISPATCHDIMX		0x2500U
#define GEN12_GPGPU_DISPATCHDIMY		0x2504U
#define GEN12_GPGPU_DISPATCHDIMZ		0x2508U

/*
 * The registers an indirect 3DPRIMITIVE (dword 0 Indirect Parameter Enable,
 * bit 10) takes its counts and firsts from, loaded with
 * MI_LOAD_REGISTER_MEM from the draw's record.
 *
 * Values referenced: Mesa 25.0.7 src/intel/vulkan/genX_cmd_draw.c
 * (GFX7_3DPRIM_START_VERTEX .. GFX7_3DPRIM_BASE_VERTEX,
 * load_indirect_parameters), src/intel/genxml/gen120.xml (3DPRIMITIVE).
 */
#define GEN12_3DPRIMITIVE_INDIRECT		(1U << 10)
#define GEN12_3DPRIM_START_VERTEX		0x2430U
#define GEN12_3DPRIM_VERTEX_COUNT		0x2434U
#define GEN12_3DPRIM_INSTANCE_COUNT		0x2438U
#define GEN12_3DPRIM_START_INSTANCE		0x243CU
#define GEN12_3DPRIM_BASE_VERTEX		0x2440U

/*
 * The extended parameters of 3DPRIMITIVE (Gen11 and Gen12): dword 0 bit
 * 11 says three more dwords follow (parameters 0 to 2); an indirect draw
 * takes parameter 0 from its register.
 *
 * Values referenced: Mesa 26.2.6 src/intel/genxml/gen110.xml
 * (3DPRIMITIVE_EXTENDED: ten dwords, Extended Parameters Present, Extended
 * Parameter 0 to 2) and src/intel/vulkan/genX_cmd_draw.c
 * (GEN11_3DPRIM_XP0).
 */
#define GEN11_3DPRIMITIVE_EXTENDED		(1U << 11)
#define GEN11_3DPRIMITIVE_EXTENDED_DWORDS	10U
#define GEN11_3DPRIM_XP0			0x2690U

/* The dwords of MI_LOAD_REGISTER_MEM: the header, the register, the address's two words. */
#define GEN12_MI_LOAD_REGISTER_MEM_DWORDS	4U

#endif /* DRIVERS_GPU_I915_INTEL_GENXML_H */
