/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Gen12 video commands: the MFX commands of an H.264 VLD decode in the
 * short format driver interface and the HCP commands of an H.265 decode in
 * the long format: the command opcodes and packet lengths, and the dwords
 * and bits of the fields the decodes program.
 *
 * A video command's header dword is
 *   (Command Type 3 << 29) | (Pipeline 2 << 27) | (Media Command Opcode << 24) |
 *   (SubOpcode A << 21) | (SubOpcode B << 16) | (DWord Length = dwords - 2)
 * and the high sixteen bits are the opcode constants below.  A field at bit
 * b of the command is bit (b % 32) of dword (b / 32).  An address field
 * keeps the address's own bits where they are: a field from bit 6 of a
 * dword takes a 64-byte aligned address whose low dword is written as it
 * is.
 */

#ifndef DRIVERS_GPU_I915_INTEL_GENXML_VIDEO_H
#define DRIVERS_GPU_I915_INTEL_GENXML_VIDEO_H

/* Builds a video command header from an opcode and the packet's dword count. */
#define GEN12_VIDEO_HEADER(opcode, dwords)	(((uint32_t)(opcode) << 16) | ((dwords) - 2U))

/*
 * I915_CMD_MI_FLUSH_DW: MI Command Opcode 38, five dwords, DWord Length
 * 3.  Dword 0 bit 7 is Video Pipeline Cache Invalidate.  The other four
 * dwords (the post-sync address and data) stay zero: no post-sync write.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen110.xml (MI_FLUSH_DW).
 */
#define GEN12_VIDEO_MI_FLUSH_DW			0x13000003U
#define GEN12_VIDEO_MI_FLUSH_DW_DWORDS		5U
#define GEN12_VIDEO_MI_FLUSH_DW_INVALIDATE	(1U << 7)

/*
 * MI_FORCE_WAKEUP: MI Command Opcode 29, two dwords, DWord
 * Length 0.  Dword 1 bit 9 is MFX Power Well Control and bits 31:16 the
 * Mask Bits that let it be written (the mask written is 768).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml
 * (MI_FORCE_WAKEUP).
 */
#define GEN12_VIDEO_MI_FORCE_WAKEUP		0x0e800000U
#define GEN12_VIDEO_MI_FORCE_WAKEUP_DWORDS	2U
#define GEN12_VIDEO_FORCE_WAKEUP_MFX		(1U << 9)
#define GEN12_VIDEO_FORCE_WAKEUP_MASK_SHIFT	16U
#define GEN12_VIDEO_FORCE_WAKEUP_MASK		768U

/*
 * MFX_WAIT: one dword, Command Type 3, Command Subtype 1 (bits
 * 28:27), SubOpcode 0, DWord Length 0 (its bias is one).  Bit 8 is MFX Sync
 * Control Flag.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen75.xml (MFX_WAIT).
 */
#define GEN12_VIDEO_MFX_WAIT			0x68000000U
#define GEN12_VIDEO_MFX_WAIT_SYNC		(1U << 8)

/*
 * The opcodes of the MFX commands (the high sixteen bits of the header).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml and the files it
 * imports (the opcode fields of each command below).
 */
#define GEN12_VIDEO_MFX_PIPE_MODE_SELECT	0x7000U
#define GEN12_VIDEO_MFX_SURFACE_STATE		0x7001U
#define GEN12_VIDEO_MFX_PIPE_BUF_ADDR_STATE	0x7002U
#define GEN12_VIDEO_MFX_IND_OBJ_BASE_ADDR_STATE	0x7003U
#define GEN12_VIDEO_MFX_BSP_BUF_BASE_ADDR_STATE	0x7004U
#define GEN12_VIDEO_MFX_QM_STATE		0x7007U
#define GEN12_VIDEO_MFX_AVC_IMG_STATE		0x7100U
#define GEN12_VIDEO_MFX_AVC_DIRECTMODE_STATE	0x7102U
#define GEN12_VIDEO_MFD_AVC_PICID_STATE		0x7125U
#define GEN12_VIDEO_MFD_AVC_DPB_STATE		0x7126U
#define GEN12_VIDEO_MFD_AVC_SLICEADDR		0x7127U
#define GEN12_VIDEO_MFD_AVC_BSD_OBJECT		0x7128U

/*
 * The packet lengths of the MFX commands, in dwords.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml and the files it
 * imports (the "length" of each command below).
 */
#define GEN12_VIDEO_MFX_PIPE_MODE_SELECT_DWORDS		5U
#define GEN12_VIDEO_MFX_SURFACE_STATE_DWORDS		6U
#define GEN12_VIDEO_MFX_PIPE_BUF_ADDR_STATE_DWORDS	65U
#define GEN12_VIDEO_MFX_IND_OBJ_BASE_ADDR_STATE_DWORDS	26U
#define GEN12_VIDEO_MFX_BSP_BUF_BASE_ADDR_STATE_DWORDS	10U
#define GEN12_VIDEO_MFX_QM_STATE_DWORDS			18U
#define GEN12_VIDEO_MFX_AVC_IMG_STATE_DWORDS		21U
#define GEN12_VIDEO_MFX_AVC_DIRECTMODE_STATE_DWORDS	71U
#define GEN12_VIDEO_MFD_AVC_PICID_STATE_DWORDS		10U
#define GEN12_VIDEO_MFD_AVC_DPB_STATE_DWORDS		27U
#define GEN12_VIDEO_MFD_AVC_SLICEADDR_DWORDS		4U
#define GEN12_VIDEO_MFD_AVC_BSD_OBJECT_DWORDS		7U

/*
 * MFX_PIPE_MODE_SELECT, dword 1: Standard Select (3:0, AVC 2),
 * Codec Select (4, Decode 0), Pre Deblocking Output Enable (8), Post
 * Deblocking Output Enable (9), Decoder Mode Select (16:15, VLD 0) and
 * Decoder Short Format Mode (17, the short format driver interface 0).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen110.xml
 * (MFX_PIPE_MODE_SELECT).
 */
#define GEN12_VIDEO_MODE_STANDARD_AVC		2U
#define GEN12_VIDEO_MODE_POST_DEBLOCKING	(1U << 9)

/*
 * MFX_SURFACE_STATE.  Dword 1: Surface ID (3:0, the reference
 * picture 0).  Dword 2: Width (17:4) and Height (31:18), both less one.
 * Dword 3: Tile Walk (0, YMAJOR 1), Tiled Surface (1), Surface Pitch
 * (19:3, bytes less one), Interleave Chroma (27) and Surface Format
 * (31:28, PLANAR_420_8 4).  Dword 4: Y Offset for U(Cb) (14:0) and X Offset
 * (30:16).  Dword 5: Y Offset for V(Cr) (15:0) and X Offset (28:16).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen90.xml
 * (MFX_SURFACE_STATE).
 */
#define GEN12_VIDEO_SURFACE_WIDTH_SHIFT		4U
#define GEN12_VIDEO_SURFACE_HEIGHT_SHIFT	18U
#define GEN12_VIDEO_SURFACE_WALK_YMAJOR		(1U << 0)
#define GEN12_VIDEO_SURFACE_TILED		(1U << 1)
#define GEN12_VIDEO_SURFACE_PITCH_SHIFT		3U
#define GEN12_VIDEO_SURFACE_INTERLEAVE_CHROMA	(1U << 27)
#define GEN12_VIDEO_SURFACE_FORMAT_SHIFT	28U
#define GEN12_VIDEO_SURFACE_PLANAR_420_8	4U

/*
 * MFX_PIPE_BUF_ADDR_STATE: the dword each address starts at
 * (two dwords, the low and the high) and each attributes dword.  The
 * reference pictures are sixteen addresses of two dwords from dword 19,
 * with one attributes dword for all of them at 51.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen110.xml
 * (MFX_PIPE_BUF_ADDR_STATE).
 */
#define GEN12_VIDEO_BUF_PRE_DEBLOCKING		1U
#define GEN12_VIDEO_BUF_POST_DEBLOCKING		4U
#define GEN12_VIDEO_BUF_ORIGINAL_SOURCE		7U
#define GEN12_VIDEO_BUF_STREAM_OUT		10U
#define GEN12_VIDEO_BUF_INTRA_ROW_STORE		13U
#define GEN12_VIDEO_BUF_DEBLOCKING_ROW_STORE	16U
#define GEN12_VIDEO_BUF_REFERENCES		19U
#define GEN12_VIDEO_BUF_REFERENCE_ATTRIBUTES	51U
#define GEN12_VIDEO_BUF_MB_STATUS		52U
#define GEN12_VIDEO_BUF_MB_ILDB_STREAM_OUT	55U
#define GEN12_VIDEO_BUF_SECOND_MB_ILDB		58U
#define GEN12_VIDEO_BUF_COMPRESSION		61U
#define GEN12_VIDEO_BUF_SCALED_REFERENCE	62U

/*
 * MFX_IND_OBJ_BASE_ADDR_STATE: five objects of an address (two
 * dwords), an attributes dword and an upper bound (two dwords), from dword
 * 1 in steps of five; the first is the indirect bitstream object.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen80.xml
 * (MFX_IND_OBJ_BASE_ADDR_STATE).
 */
#define GEN12_VIDEO_IND_OBJECTS			5U
#define GEN12_VIDEO_IND_OBJECT_DWORDS		5U

/*
 * MFX_BSP_BUF_BASE_ADDR_STATE: the BSD/MPC row store at dword 1,
 * the MPR row store at dword 4 and the bitplane read buffer at dword 7,
 * each an address of two dwords and an attributes dword.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen90.xml
 * (MFX_BSP_BUF_BASE_ADDR_STATE).
 */
#define GEN12_VIDEO_BSP_BSD_MPC_ROW_STORE	1U
#define GEN12_VIDEO_BSP_MPR_ROW_STORE		4U
#define GEN12_VIDEO_BSP_BITPLANE		7U

/*
 * MFD_AVC_DPB_STATE.  Dword 1: Non-Existing Frame (bit i) and
 * Long Term Frame (bit 16 + i) of the sixteen DPB entries.  Dword 2: Used
 * for Reference (two bits from 2i, FRAME 3).  Dwords 3 to 10: the LTST
 * Frame Number List, sixteen bits each, two to a dword.  Dwords 11 to 26:
 * the view IDs and view orders, zero without MVC.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen90.xml
 * (MFD_AVC_DPB_STATE).
 */
#define GEN12_VIDEO_DPB_LONG_TERM_SHIFT		16U
#define GEN12_VIDEO_DPB_FRAME			3U
#define GEN12_VIDEO_DPB_FRAME_NUMBERS		3U

/*
 * MFD_AVC_PICID_STATE.  Dword 1 bit 0: PictureID Remapping
 * Disable (0, the 16-bit picture IDs are used).  Dwords 2 to 9: the
 * sixteen Picture IDs, sixteen bits each, two to a dword; 0xffff for an
 * entry no reference uses.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen75.xml
 * (MFD_AVC_PICID_STATE).
 */
#define GEN12_VIDEO_PICID_IDS			2U
#define GEN12_VIDEO_PICID_UNUSED		0xffffU

/*
 * MFX_AVC_IMG_STATE (Gen12 takes the 21-dword form, not the 14-dword form
 * of Gen8).
 *   dword 1: Frame Size (15:0, macroblocks)
 *   dword 2: Frame Width (7:0) and Frame Height (23:16), macroblocks less one
 *   dword 3: Image Structure (9:8, a frame 0), Weighted BiPrediction IDC
 *            (11:10), Weighted Prediction Enable (12), First Chroma QP
 *            Offset (20:16) and Second Chroma QP Offset (28:24), both signed
 *   dword 4: Field Picture (0), MBAFF Mode (1), Frame MB Only (2), 8x8 IDCT
 *            Transform Mode (3), Direct 8x8 Inference (4), Constrained Intra
 *            Prediction (5), Non-Reference Picture (6), Entropy Coding Sync
 *            Enable (7, CABAC) and Chroma Format IDC (11:10)
 *   dword 5: Trellis Quantization Chroma Disable (27)
 *   dword 13: Initial QP Value (7:0, signed), Number of Active Reference
 *            Pictures from L0 (13:8) and L1 (21:16), Number of Reference
 *            Frames (28:24)
 *   dword 14: Pic Order Present (0), Delta Pic Order Always Zero (1), Pic
 *            Order Count Type (3:2), Redundant Pic Count Present (11),
 *            Deblocking Filter Control Present (15), Log2 Max Frame Number
 *            (23:16) and Log2 Max Pic Order Count LSB (31:24), both less four
 *   dword 15: Current Picture Frame Number (31:16)
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen110.xml
 * (MFX_AVC_IMG_STATE).
 */
#define GEN12_VIDEO_IMG_HEIGHT_SHIFT		16U
#define GEN12_VIDEO_IMG_WEIGHTED_BIPRED_SHIFT	10U
#define GEN12_VIDEO_IMG_WEIGHTED_PRED		(1U << 12)
#define GEN12_VIDEO_IMG_CHROMA_OFFSET_SHIFT	16U
#define GEN12_VIDEO_IMG_SECOND_CHROMA_SHIFT	24U
#define GEN12_VIDEO_IMG_QP_OFFSET_MASK		0x1fU
#define GEN12_VIDEO_IMG_FRAME_MB_ONLY		(1U << 2)
#define GEN12_VIDEO_IMG_TRANSFORM_8X8		(1U << 3)
#define GEN12_VIDEO_IMG_DIRECT_8X8		(1U << 4)
#define GEN12_VIDEO_IMG_CONSTRAINED_INTRA	(1U << 5)
#define GEN12_VIDEO_IMG_NON_REFERENCE		(1U << 6)
#define GEN12_VIDEO_IMG_CABAC			(1U << 7)
#define GEN12_VIDEO_IMG_CHROMA_FORMAT_SHIFT	10U
#define GEN12_VIDEO_IMG_TRELLIS_CHROMA_DISABLE	(1U << 27)
#define GEN12_VIDEO_IMG_QP_MASK			0xffU
#define GEN12_VIDEO_IMG_L0_SHIFT		8U
#define GEN12_VIDEO_IMG_L1_SHIFT		16U
#define GEN12_VIDEO_IMG_REFERENCES_SHIFT	24U
#define GEN12_VIDEO_IMG_PIC_ORDER_PRESENT	(1U << 0)
#define GEN12_VIDEO_IMG_DELTA_ALWAYS_ZERO	(1U << 1)
#define GEN12_VIDEO_IMG_POC_TYPE_SHIFT		2U
#define GEN12_VIDEO_IMG_REDUNDANT_PIC_COUNT	(1U << 11)
#define GEN12_VIDEO_IMG_DEBLOCKING_CONTROL	(1U << 15)
#define GEN12_VIDEO_IMG_LOG2_FRAME_NUM_SHIFT	16U
#define GEN12_VIDEO_IMG_LOG2_POC_LSB_SHIFT	24U
#define GEN12_VIDEO_IMG_FRAME_NUM_SHIFT		16U

/*
 * MFX_QM_STATE.  Dword 1: the matrix (1:0): 4x4 intra 0, 4x4
 * inter 1, 8x8 intra 2, 8x8 inter 3.  Dwords 2 to 17: the Forward Quantizer
 * Matrix, 64 bytes in raster order, four to a dword from the low byte (a
 * 4x4 matrix is the Y, Cb and Cr lists one after the other).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen90.xml (MFX_QM_STATE).
 */
#define GEN12_VIDEO_QM_4X4_INTRA		0U
#define GEN12_VIDEO_QM_4X4_INTER		1U
#define GEN12_VIDEO_QM_8X8_INTRA		2U
#define GEN12_VIDEO_QM_8X8_INTER		3U
#define GEN12_VIDEO_QM_BYTES			64U

/*
 * MFX_AVC_DIRECTMODE_STATE: sixteen Direct MV Buffer addresses
 * of two dwords from dword 1, their attributes at dword 33, the Direct MV
 * Buffer (Write) address at 34 and its attributes at 36, and the POC List
 * from dword 37: the top and the bottom field order counts of the sixteen
 * entries, then the current picture's (entries 32 and 33).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen80.xml
 * (MFX_AVC_DIRECTMODE_STATE).
 */
#define GEN12_VIDEO_DIRECT_BUFFERS		1U
#define GEN12_VIDEO_DIRECT_ATTRIBUTES		33U
#define GEN12_VIDEO_DIRECT_WRITE		34U
#define GEN12_VIDEO_DIRECT_WRITE_ATTRIBUTES	36U
#define GEN12_VIDEO_DIRECT_POC_LIST		37U
#define GEN12_VIDEO_DIRECT_POC_ENTRIES		34U

/*
 * MFD_AVC_SLICEADDR and MFD_AVC_BSD_OBJECT: dword 1 the
 * Indirect BSD Data Length (bytes), dword 2 the Indirect BSD Data Start
 * Address (28:0, bytes from the indirect bitstream object's address).
 * The BSD object's inline data are dwords 3 to 5:
 *   dword 4: Last Slice (3), Fix Prev MB Skipped (7)
 *   dword 5: Intra Prediction Error Control (0), Intra 8x8/4x4 Prediction
 *            Error Concealment Control (1), I Slice Concealment Mode (31,
 *            intra concealment 1)
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen110.xml
 * (MFD_AVC_SLICEADDR, MFD_AVC_BSD_OBJECT).
 */
#define GEN12_VIDEO_BSD_START_MASK		0x1fffffffU
#define GEN12_VIDEO_BSD_LAST_SLICE		(1U << 3)
#define GEN12_VIDEO_BSD_FIX_PREV_MB_SKIPPED	(1U << 7)
#define GEN12_VIDEO_BSD_INTRA_ERROR_CONTROL	(1U << 0)
#define GEN12_VIDEO_BSD_INTRA_CONCEALMENT	(1U << 1)
#define GEN12_VIDEO_BSD_I_SLICE_CONCEALMENT	(1U << 31)

/*
 * MEMORYADDRESSATTRIBUTES: MOCS (6:0) is the low field of the
 * dword; the rest (arbitration, compression, cache select, tiled resource
 * mode) stays zero.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen90.xml
 * (MEMORYADDRESSATTRIBUTES).
 */
#define GEN12_VIDEO_ATTRIBUTES_MOCS_MASK	0x7fU

/*
 * The HEVC decoder (HCP) of the same engine.
 *
 * MI_FORCE_WAKEUP dword 1 bit 8 is HEVC Power Well Control (under the
 * same mask bits as the MFX well).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml
 * (MI_FORCE_WAKEUP).
 */
#define GEN12_VIDEO_FORCE_WAKEUP_HEVC		(1U << 8)

/*
 * The opcodes (the high sixteen bits of the header) of the HCP commands and
 * of the two VD commands that bracket them: Command Type 3, Pipeline 2,
 * Media Command Opcode 7 (bits 26:23) and the SubOpcode (22:16); the VD
 * pipeline flush is Media Command Opcode 15 with SubOpcodes 0.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen90.xml,
 * gen110.xml and gen120.xml (the opcode fields of HCP_PIPE_MODE_SELECT,
 * HCP_SURFACE_STATE, HCP_PIPE_BUF_ADDR_STATE, HCP_IND_OBJ_BASE_ADDR_STATE,
 * HCP_QM_STATE, HCP_PIC_STATE, HCP_TILE_STATE, HCP_REF_IDX_STATE,
 * HCP_WEIGHTOFFSET_STATE, HCP_SLICE_STATE, HCP_BSD_OBJECT,
 * VD_CONTROL_STATE and VD_PIPELINE_FLUSH).
 */
#define GEN12_VIDEO_HCP_PIPE_MODE_SELECT	0x7380U
#define GEN12_VIDEO_HCP_SURFACE_STATE		0x7381U
#define GEN12_VIDEO_HCP_PIPE_BUF_ADDR_STATE	0x7382U
#define GEN12_VIDEO_HCP_IND_OBJ_BASE_ADDR_STATE	0x7383U
#define GEN12_VIDEO_HCP_QM_STATE		0x7384U
#define GEN12_VIDEO_HCP_PIC_STATE		0x7390U
#define GEN12_VIDEO_HCP_TILE_STATE		0x7391U
#define GEN12_VIDEO_HCP_REF_IDX_STATE		0x7392U
#define GEN12_VIDEO_HCP_WEIGHTOFFSET_STATE	0x7393U
#define GEN12_VIDEO_HCP_SLICE_STATE		0x7394U
#define GEN12_VIDEO_HCP_BSD_OBJECT		0x73a0U
#define GEN12_VIDEO_VD_CONTROL_STATE		0x738aU
#define GEN12_VIDEO_VD_PIPELINE_FLUSH		0x7780U

/*
 * The packet lengths of those commands, in dwords (Gen12's: the slice,
 * picture, surface and mode commands are longer than Gen11's).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen90.xml,
 * gen110.xml and gen120.xml (the "length" of each command above).
 */
#define GEN12_VIDEO_HCP_PIPE_MODE_SELECT_DWORDS		7U
#define GEN12_VIDEO_HCP_SURFACE_STATE_DWORDS		5U
#define GEN12_VIDEO_HCP_PIPE_BUF_ADDR_STATE_DWORDS	121U
#define GEN12_VIDEO_HCP_IND_OBJ_BASE_ADDR_STATE_DWORDS	29U
#define GEN12_VIDEO_HCP_QM_STATE_DWORDS			18U
#define GEN12_VIDEO_HCP_PIC_STATE_DWORDS		41U
#define GEN12_VIDEO_HCP_TILE_STATE_DWORDS		17U
#define GEN12_VIDEO_HCP_REF_IDX_STATE_DWORDS		18U
#define GEN12_VIDEO_HCP_WEIGHTOFFSET_STATE_DWORDS	42U
#define GEN12_VIDEO_HCP_SLICE_STATE_DWORDS		13U
#define GEN12_VIDEO_HCP_BSD_OBJECT_DWORDS		3U
#define GEN12_VIDEO_VD_CONTROL_STATE_DWORDS		3U
#define GEN12_VIDEO_VD_PIPELINE_FLUSH_DWORDS		2U

/*
 * VD_CONTROL_STATE dword 1: Pipeline Initialization (0) and Memory
 * Implicit Flush (bit 66 of the command, dword 2 bit 2).  VD_PIPELINE_FLUSH
 * dword 1: HEVC Pipeline Done (0), VD Command/Message Parser Done (4) and
 * HEVC Pipeline Command Flush (16).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml
 * (VD_CONTROL_STATE, VD_PIPELINE_FLUSH).
 */
#define GEN12_VIDEO_VD_PIPELINE_INITIALIZATION	(1U << 0)
#define GEN12_VIDEO_VD_MEMORY_IMPLICIT_FLUSH	(1U << 2)
#define GEN12_VIDEO_VD_HEVC_DONE		(1U << 0)
#define GEN12_VIDEO_VD_PARSER_DONE		(1U << 4)
#define GEN12_VIDEO_VD_HEVC_COMMAND_FLUSH	(1U << 16)

/*
 * HCP_SURFACE_STATE: dword 1 the Surface Pitch less one (16:0) and the
 * Surface ID (31:28, the decoded picture 0, the reference pictures 5);
 * dword 2 the Y Offset for U(Cb) (14:0, the rows of the Y plane) and the
 * Surface Format (31:27, PLANAR_420_8 4); dword 3 the Default Alpha Value
 * (15:0).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml
 * (HCP_SURFACE_STATE).
 */
#define GEN12_VIDEO_HCP_SURFACE_ID_SHIFT	28U
#define GEN12_VIDEO_HCP_SURFACE_DECODED		0U
#define GEN12_VIDEO_HCP_SURFACE_REFERENCE	5U
#define GEN12_VIDEO_HCP_SURFACE_FORMAT_SHIFT	27U
#define GEN12_VIDEO_HCP_SURFACE_PLANAR_420_8	4U
#define GEN12_VIDEO_HCP_SURFACE_ALPHA		0xffffU

/*
 * HCP_PIPE_BUF_ADDR_STATE: the dword each buffer's address starts at (its
 * attributes follow the address, the reference pictures' and the
 * collocated motion vectors' after their eight addresses).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen110.xml
 * (HCP_PIPE_BUF_ADDR_STATE).
 */
#define GEN12_VIDEO_HCP_BUF_DECODED		1U
#define GEN12_VIDEO_HCP_BUF_LINE_BUFFERS	4U
#define GEN12_VIDEO_HCP_BUF_CURRENT_MV		31U
#define GEN12_VIDEO_HCP_BUF_REFERENCES		37U
#define GEN12_VIDEO_HCP_BUF_REFERENCE_ATTRIBUTES 53U
#define GEN12_VIDEO_HCP_BUF_COLLOCATED_MV	66U
#define GEN12_VIDEO_HCP_BUF_COLLOCATED_ATTRIBUTES 82U
#define GEN12_VIDEO_HCP_REFERENCES		8U

/*
 * HCP_PIC_STATE.  Dword 1: Frame Width (10:0) and Height (26:16) In
 * Minimum Coding Block Size, less one.  Dword 2: Min CU Size (1:0), LCU
 * Size (3:2), Min TU Size (5:4), Max TU Size (7:6), Min PCM Size (9:8),
 * Max PCM Size (11:10) and Chroma Subsampling (31:29).  Dword 3: Log2
 * Max Transform Skip Size (22:20).  Dword 4: Sample
 * Adaptive Offset Enable (3), PCM Enable (4), CU QP Delta Enable (5), Max
 * DQP Depth (7:6), PCM Loop Filter Disable (8), Constrained Intra
 * Prediction (9), Log2 Parallel Merge Level (12:10), Sign Data Hiding
 * (13), Loop Filter Enable (15, across tiles), Entropy Coding Sync Enable
 * (16), Tiling Enable (17), Weighted BiPredication Enable (18), Weighted
 * Predication Enable (19), Top Field (21), Transform Skip Enable (22), AMP
 * Enable (23), Transquant Bypass Enable (25) and Strong Intra Smoothing
 * Enable (26).  Dword 5: Picture Cb (4:0) and Cr (9:5) QP Offset, Intra
 * (12:10) and Inter (15:13) Max Transform Hierarchy Depth, Chroma (19:16)
 * and Luma (23:20) PCM Sample Bit Depth, Chroma (26:24) and Luma (29:27)
 * Bit Depth.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml
 * (HCP_PIC_STATE).
 */
#define GEN12_VIDEO_HCP_PIC_HEIGHT_SHIFT	16U
#define GEN12_VIDEO_HCP_PIC_LCU_SHIFT		2U
#define GEN12_VIDEO_HCP_PIC_MIN_TU_SHIFT	4U
#define GEN12_VIDEO_HCP_PIC_MAX_TU_SHIFT	6U
#define GEN12_VIDEO_HCP_PIC_MIN_PCM_SHIFT	8U
#define GEN12_VIDEO_HCP_PIC_MAX_PCM_SHIFT	10U
#define GEN12_VIDEO_HCP_PIC_CHROMA_SHIFT	29U
#define GEN12_VIDEO_HCP_PIC_SKIP_SIZE_SHIFT	20U
#define GEN12_VIDEO_HCP_PIC_SAO			(1U << 3)
#define GEN12_VIDEO_HCP_PIC_PCM			(1U << 4)
#define GEN12_VIDEO_HCP_PIC_CU_QP_DELTA		(1U << 5)
#define GEN12_VIDEO_HCP_PIC_DQP_DEPTH_SHIFT	6U
#define GEN12_VIDEO_HCP_PIC_PCM_FILTER_OFF	(1U << 8)
#define GEN12_VIDEO_HCP_PIC_CONSTRAINED_INTRA	(1U << 9)
#define GEN12_VIDEO_HCP_PIC_MERGE_LEVEL_SHIFT	10U
#define GEN12_VIDEO_HCP_PIC_SIGN_HIDING		(1U << 13)
#define GEN12_VIDEO_HCP_PIC_FILTER_TILES	(1U << 15)
#define GEN12_VIDEO_HCP_PIC_ENTROPY_SYNC	(1U << 16)
#define GEN12_VIDEO_HCP_PIC_TILES		(1U << 17)
#define GEN12_VIDEO_HCP_PIC_WEIGHTED_BIPRED	(1U << 18)
#define GEN12_VIDEO_HCP_PIC_WEIGHTED_PRED	(1U << 19)
#define GEN12_VIDEO_HCP_PIC_TOP_FIELD		(1U << 21)
#define GEN12_VIDEO_HCP_PIC_TRANSFORM_SKIP	(1U << 22)
#define GEN12_VIDEO_HCP_PIC_AMP			(1U << 23)
#define GEN12_VIDEO_HCP_PIC_TRANSQUANT_BYPASS	(1U << 25)
#define GEN12_VIDEO_HCP_PIC_STRONG_INTRA	(1U << 26)
#define GEN12_VIDEO_HCP_PIC_CR_OFFSET_SHIFT	5U
#define GEN12_VIDEO_HCP_PIC_INTRA_DEPTH_SHIFT	10U
#define GEN12_VIDEO_HCP_PIC_INTER_DEPTH_SHIFT	13U
#define GEN12_VIDEO_HCP_PIC_PCM_CHROMA_SHIFT	16U
#define GEN12_VIDEO_HCP_PIC_PCM_LUMA_SHIFT	20U
#define GEN12_VIDEO_HCP_PIC_DEPTH_CHROMA_SHIFT	24U
#define GEN12_VIDEO_HCP_PIC_DEPTH_LUMA_SHIFT	27U
#define GEN12_VIDEO_HCP_QP_OFFSET_MASK		0x1fU

/*
 * HCP_QM_STATE dword 1: Prediction Type (0, intra 0), SizeID (2:1),
 * Color Component (4:3) and DC Coefficient (12:5); then the 64 bytes of
 * the matrix, four a dword from the low byte.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen110.xml
 * (HCP_QM_STATE).
 */
#define GEN12_VIDEO_HCP_QM_SIZE_SHIFT		1U
#define GEN12_VIDEO_HCP_QM_COLOR_SHIFT		3U
#define GEN12_VIDEO_HCP_QM_DC_SHIFT		5U
#define GEN12_VIDEO_HCP_QM_BYTES		64U

/*
 * HCP_TILE_STATE: dword 1 Number of Tile Rows (4:0) and Columns (9:5),
 * less one; the low eight bits of each tile column's first CTB from
 * dword 2 (four a dword, five dwords) and of each tile row's from dword 7
 * (six dwords); the two high bits of each from dword 13 (columns) and 15
 * (rows), two bits an entry from the low bit of a 64-bit field.
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen110.xml
 * (HCP_TILE_STATE, HCP_TILE_POSITION_IN_CTB_MSB) and gen90.xml
 * (HCP_TILE_POSITION_IN_CTB).
 */
#define GEN12_VIDEO_HCP_TILE_COLUMNS_SHIFT	5U
#define GEN12_VIDEO_HCP_TILE_COLUMN_DWORDS	5U
#define GEN12_VIDEO_HCP_TILE_ROW_DWORDS		6U

/*
 * HCP_REF_IDX_STATE: dword 1 Reference Picture List Select (0) and Number
 * of Reference Indexes Active less one (4:1); sixteen entries from dword
 * 2: Reference Picture tb Value (7:0), List Entry (10:8, the reference
 * picture's place in HCP_PIPE_BUF_ADDR_STATE), Chroma (11) and Luma (12)
 * Weighted Prediction, Long Term Reference (13), Field Pic (14) and Top
 * Field (15).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen90.xml
 * (HCP_REF_IDX_STATE, HCP_REF_LIST_ENTRY).
 */
#define GEN12_VIDEO_HCP_REF_ACTIVE_SHIFT	1U
#define GEN12_VIDEO_HCP_REF_ENTRIES		16U
#define GEN12_VIDEO_HCP_REF_ENTRY_SHIFT		8U
#define GEN12_VIDEO_HCP_REF_CHROMA_WEIGHTED	(1U << 11)
#define GEN12_VIDEO_HCP_REF_LUMA_WEIGHTED	(1U << 12)
#define GEN12_VIDEO_HCP_REF_LONG_TERM		(1U << 13)
#define GEN12_VIDEO_HCP_REF_TOP_FIELD		(1U << 15)

/*
 * HCP_WEIGHTOFFSET_STATE: dword 1 Reference Picture List Select (0);
 * sixteen luma entries from dword 2 (Delta Luma Weight 7:0, Luma Offset
 * 15:8), sixteen chroma entries from dword 18 (Delta Chroma Weight and
 * Chroma Offset of Cb in 7:0 and 15:8, of Cr in 23:16 and 31:24), then
 * eight dwords of the offsets' high bytes (zero at eight bits).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen110.xml
 * (HCP_WEIGHTOFFSET_STATE, HCP_WEIGHTOFFSET_LUMA_ENTRY) and gen90.xml
 * (HCP_WEIGHTOFFSET_CHROMA_ENTRY).
 */
#define GEN12_VIDEO_HCP_WEIGHT_ENTRIES		16U
#define GEN12_VIDEO_HCP_WEIGHT_HIGH_DWORDS	8U

/*
 * HCP_SLICE_STATE.  Dword 1: Slice Horizontal (9:0) and Vertical (25:16)
 * Position in CTBs; dword 2 those of the next slice.  Dword 3: Slice Type
 * (1:0, B 0, P 1, I 2), Last Slice (2), Slice QP Sign (3), Dependent Slice
 * (4), Slice Temporal MVP Enable (5), Slice QP (11:6), Slice Cb (16:12)
 * and Cr (21:17) QP Offset.  Dword 4: Slice Header Disable Deblocking
 * Filter (0), Slice TC (4:1) and Beta (8:5) Offset Div2, Slice Loop Filter
 * Enable (10), Slice SAO Chroma (11) and Luma (12), MVD L1 Zero (13), Low
 * Delay (14), Collocated From L0 (15), Log2 Weight Denominator Chroma
 * (18:16) and Luma (21:19), CABAC Init (22), Max Merge Index (25:23) and
 * Collocated MV Temporal Buffer Index (28:26).  Dword 5: Slice Header
 * Length (15:0).  Dword 11: Original Slice Start Ctb X (9:0) and Y
 * (25:16).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen120.xml
 * (HCP_SLICE_STATE).
 */
#define GEN12_VIDEO_HCP_SLICE_Y_SHIFT		16U
#define GEN12_VIDEO_HCP_SLICE_LAST		(1U << 2)
#define GEN12_VIDEO_HCP_SLICE_QP_SIGN		(1U << 3)
#define GEN12_VIDEO_HCP_SLICE_DEPENDENT		(1U << 4)
#define GEN12_VIDEO_HCP_SLICE_TEMPORAL_MVP	(1U << 5)
#define GEN12_VIDEO_HCP_SLICE_QP_SHIFT		6U
#define GEN12_VIDEO_HCP_SLICE_CB_SHIFT		12U
#define GEN12_VIDEO_HCP_SLICE_CR_SHIFT		17U
#define GEN12_VIDEO_HCP_SLICE_DEBLOCKING_OFF	(1U << 0)
#define GEN12_VIDEO_HCP_SLICE_TC_SHIFT		1U
#define GEN12_VIDEO_HCP_SLICE_BETA_SHIFT	5U
#define GEN12_VIDEO_HCP_SLICE_FILTER_SLICES	(1U << 10)
#define GEN12_VIDEO_HCP_SLICE_SAO_CHROMA	(1U << 11)
#define GEN12_VIDEO_HCP_SLICE_SAO_LUMA		(1U << 12)
#define GEN12_VIDEO_HCP_SLICE_MVD_L1_ZERO	(1U << 13)
#define GEN12_VIDEO_HCP_SLICE_LOW_DELAY		(1U << 14)
#define GEN12_VIDEO_HCP_SLICE_COLLOCATED_L0	(1U << 15)
#define GEN12_VIDEO_HCP_SLICE_CHROMA_DENOM_SHIFT 16U
#define GEN12_VIDEO_HCP_SLICE_LUMA_DENOM_SHIFT	19U
#define GEN12_VIDEO_HCP_SLICE_CABAC_INIT	(1U << 22)
#define GEN12_VIDEO_HCP_SLICE_MERGE_SHIFT	23U
#define GEN12_VIDEO_HCP_SLICE_COLLOCATED_SHIFT	26U
#define GEN12_VIDEO_HCP_SLICE_OFFSET_MASK	0xfU
#define GEN12_VIDEO_HCP_SLICE_HEADER_MASK	0xffffU

/*
 * HCP_BSD_OBJECT: dword 1 the Indirect BSD Data Length (bytes), dword 2
 * the Indirect BSD Data Start Address (28:0, bytes from the indirect
 * bitstream object's address).
 *
 * Values referenced: Mesa 25.0.7 src/intel/genxml/gen90.xml
 * (HCP_BSD_OBJECT).
 */
#define GEN12_VIDEO_HCP_BSD_START_MASK		0x1fffffffU

#endif /* DRIVERS_GPU_I915_INTEL_GENXML_VIDEO_H */
