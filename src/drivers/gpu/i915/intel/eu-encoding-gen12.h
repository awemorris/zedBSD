/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Gen12 EU instruction encoding, field bit positions and values, for
 * compiler/eu.c.  The Gen12 native instruction is four 32-bit words (128
 * bits); ranges are HI, LO inclusive.
 */

#ifndef DRIVERS_GPU_I915_INTEL_EU_ENCODING_GEN12_H
#define DRIVERS_GPU_I915_INTEL_EU_ENCODING_GEN12_H

#define GEN12_EU_DWORDS			4U

/*
 * Control fields.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_inst.h (the Gen12
 * positions of opcode, swsb, exec_size, mask_control, saturate,
 * math_function).
 */
#define EU_OPCODE_HI			6
#define EU_OPCODE_LO			0
#define EU_SWSB_HI			15
#define EU_SWSB_LO			8
#define EU_EXEC_SIZE_HI			18
#define EU_EXEC_SIZE_LO			16
#define EU_NO_MASK_BIT			31
#define EU_SATURATE_BIT			34
#define EU_MATH_FUNCTION_HI		95
#define EU_MATH_FUNCTION_LO		92

/*
 * Predication and flags.  The conditional modifier shares bits 95:92 with
 * the math function: a math instruction has no conditional modifier.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_inst.h (the Gen12
 * positions of flag_subreg_nr, flag_reg_nr, pred_control, pred_inv,
 * cond_modifier).
 */
#define EU_FLAG_SUBREG_NR_BIT		22
#define EU_FLAG_REG_NR_BIT		23
#define EU_PRED_CONTROL_HI		27
#define EU_PRED_CONTROL_LO		24
#define EU_PRED_INV_BIT			28
#define EU_COND_MODIFIER_HI		95
#define EU_COND_MODIFIER_LO		92

/*
 * Destination (direct).  An operand is [file bit][subregister
 * 5][register 8].
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_inst.h (the Gen12
 * positions of the dst fields).
 */
#define EU_DST_REG_TYPE_HI		39
#define EU_DST_REG_TYPE_LO		36
#define EU_DST_HSTRIDE_HI		49
#define EU_DST_HSTRIDE_LO		48
#define EU_DST_REG_FILE_BIT		50
#define EU_DST_SUBREG_HI		55
#define EU_DST_SUBREG_LO		51
#define EU_DST_REG_NR_HI		63
#define EU_DST_REG_NR_LO		56

/*
 * Source 0.  An immediate is said by its own bit; the operand field
 * then stays clear.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_inst.h (the Gen12
 * positions of the src0 fields).
 */
#define EU_SRC0_REG_TYPE_HI		43
#define EU_SRC0_REG_TYPE_LO		40
#define EU_SRC0_ABS_BIT			44
#define EU_SRC0_NEGATE_BIT		45
#define EU_SRC0_IS_IMM_BIT		46
#define EU_SRC0_HSTRIDE_HI		65
#define EU_SRC0_HSTRIDE_LO		64
#define EU_SRC0_REG_FILE_BIT		66
#define EU_SRC0_SUBREG_HI		71
#define EU_SRC0_SUBREG_LO		67
#define EU_SRC0_REG_NR_HI		79
#define EU_SRC0_REG_NR_LO		72
#define EU_SRC0_WIDTH_HI		83
#define EU_SRC0_WIDTH_LO		81
#define EU_SRC0_VSTRIDE_HI		87
#define EU_SRC0_VSTRIDE_LO		84

/*
 * Source 1.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_inst.h (the Gen12
 * positions of the src1 fields).
 */
#define EU_SRC1_IS_IMM_BIT		47
#define EU_SRC1_REG_TYPE_HI		91
#define EU_SRC1_REG_TYPE_LO		88
#define EU_SRC1_HSTRIDE_HI		97
#define EU_SRC1_HSTRIDE_LO		96
#define EU_SRC1_REG_FILE_BIT		98
#define EU_SRC1_SUBREG_HI		103
#define EU_SRC1_SUBREG_LO		99
#define EU_SRC1_REG_NR_HI		111
#define EU_SRC1_REG_NR_LO		104
#define EU_SRC1_WIDTH_HI		115
#define EU_SRC1_WIDTH_LO		113
#define EU_SRC1_VSTRIDE_HI		119
#define EU_SRC1_VSTRIDE_LO		116
#define EU_SRC1_ABS_BIT			120
#define EU_SRC1_NEGATE_BIT		121

/*
 * A 32-bit immediate occupies the last word.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_inst.h (the Gen12
 * imm_ud).
 */
#define EU_IMM32_HI			127
#define EU_IMM32_LO			96

/*
 * SEND / SENDC.  The operands carry a file bit and a register number and no
 * subregister, type or region; the two 32-bit descriptors are scattered:
 *   desc[31:30] -> 123:122   desc[29:25] -> 71:67   desc[24:20] -> 55:51
 *   desc[19:11] -> 121:113   desc[10:0]  -> 91:81
 *   ex_desc[31:28] -> 127:124  [27:26] -> 97:96  [25:24] -> 65:64  [23:11] -> 47:35  [10:6] -> 103:99
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_inst.h (the Gen12
 * send_desc, send_ex_desc and the send operand fields).
 */
#define EU_SEND_EOT_BIT			34
#define EU_SEND_SFID_HI			95
#define EU_SEND_SFID_LO			92

/*
 * Hardware opcodes.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu.c (the Gen12
 * hardware opcodes of the opcode_descs table).
 */
#define EU_OP_SYNC			1U
#define EU_OP_SEND			49U
#define EU_OP_SENDC			50U
#define EU_OP_MATH			56U
#define EU_OP_ADD			64U
#define EU_OP_MUL			65U
#define EU_OP_FRC			67U
#define EU_OP_RNDD			69U
#define EU_OP_MAD			91U
#define EU_OP_NOP			96U
#define EU_OP_MOV			97U
#define EU_OP_SEL			98U
#define EU_OP_NOT			100U
#define EU_OP_AND			101U
#define EU_OP_OR			102U
#define EU_OP_CMP			112U
#define EU_OP_XOR			103U
#define EU_OP_SHR			104U
#define EU_OP_SHL			105U
#define EU_OP_ASR			108U
#define EU_OP_RNDZ			71U
#define EU_OP_RNDE			70U
#define EU_OP_WHILE			39U
#define EU_OP_IF			34U
#define EU_OP_ENDIF			37U

/*
 * A branch's jump target: bytes from the branch instruction, in the
 * last word (bits 127:96, with the src0 immediate bit set).
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_inst.h
 * (brw_eu_inst_set_jip).
 */
#define EU_JIP_HI			127
#define EU_JIP_LO			96

/*
 * A branch's second target (an IF's or an ELSE's UIP): bytes from the
 * branch, in the third word.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_inst.h
 * (brw_eu_inst_set_uip).
 */
#define EU_UIP_HI			95
#define EU_UIP_LO			64

/*
 * Conditional modifiers: what a CMP tests, or which source a SEL
 * keeps.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_defines.h (enum
 * brw_conditional_mod).
 */
#define EU_COND_Z			1U
#define EU_COND_NZ			2U
#define EU_COND_G			3U
#define EU_COND_GE			4U
#define EU_COND_L			5U
#define EU_COND_LE			6U

/*
 * The predicate that enables a channel whose bit in the named flag
 * subregister is set.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_defines.h
 * (BRW_PREDICATE_NORMAL).
 */
#define EU_PREDICATE_NORMAL		1U

/*
 * The architecture register number of flag register f0; f1 is the
 * next one.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_defines.h
 * (BRW_ARF_FLAG).
 */
#define EU_ARF_FLAG			0x30U

/*
 * Register files as compiler/eu.c names them; the hardware has one bit
 * (GRF or not) and the immediate bit.
 */
#define EU_FILE_ARF			0U
#define EU_FILE_GRF			1U
#define EU_FILE_IMM			3U

/*
 * Register / immediate types: [float 8 | signed 4 | log2(bytes)].  An
 * immediate of eight signed 4-bit integers (V) is the signed byte encoding
 * with the vector immediate's size bits cleared; HF is the 16-bit float.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_reg_type.c
 * (brw_type_encode, Gen12).
 */
#define EU_TYPE_UW			1U
#define EU_TYPE_UD			2U
#define EU_TYPE_V			4U
#define EU_TYPE_D			6U
#define EU_TYPE_HF			9U
#define EU_TYPE_F			10U

/*
 * Exec size: log2 of the channel count.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_defines.h
 * (BRW_EXECUTE_*).
 */
#define EU_EXEC_SIZE_1			0U
#define EU_EXEC_SIZE_4			2U
#define EU_EXEC_SIZE_8			3U
#define EU_EXEC_SIZE_16			4U

/*
 * Regions.  hstride 0,1,2,4 -> 0..3; width 1,2,4,8,16 -> 0..4; vstride
 * 0,1,2,4,8,16 -> 0..5.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_defines.h
 * (BRW_HORIZONTAL_STRIDE_*, BRW_WIDTH_*, BRW_VERTICAL_STRIDE_*).
 */
/*
 * The three-source (align1) layout of Gen12, the one MAD takes: each
 * field's highest and lowest bit.  A vertical stride is split over two bits
 * (0, 1, 4 or 8 elements as 0..3, the high bit first); source two has only
 * a horizontal stride.  The type is the low three bits of the operand's
 * size and sign (F is 2) with the execution type 1 for float.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_inst.h (the
 * three-source align1 fields, 12+), brw_reg_type.c
 * (brw_type_encode_for_3src), brw_eu_emit.c (to_3src_align1_vstride,
 * to_3src_align1_hstride).
 */
#define EU_3SRC_DST_REG_NR_HI		63U
#define EU_3SRC_DST_REG_NR_LO		56U
#define EU_3SRC_DST_FILE_BIT		50U
#define EU_3SRC_DST_TYPE_HI		38U
#define EU_3SRC_DST_TYPE_LO		36U
#define EU_3SRC_EXEC_FLOAT_BIT		39U
#define EU_3SRC_SRC0_REG_NR_HI		79U
#define EU_3SRC_SRC0_REG_NR_LO		72U
#define EU_3SRC_SRC0_SUBREG_HI		71U
#define EU_3SRC_SRC0_SUBREG_LO		67U
#define EU_3SRC_SRC0_FILE_BIT		66U
#define EU_3SRC_SRC0_HSTRIDE_HI		65U
#define EU_3SRC_SRC0_HSTRIDE_LO		64U
#define EU_3SRC_SRC0_VSTRIDE_HIGH_BIT	43U
#define EU_3SRC_SRC0_VSTRIDE_LOW_BIT	35U
#define EU_3SRC_SRC0_TYPE_HI		42U
#define EU_3SRC_SRC0_TYPE_LO		40U
#define EU_3SRC_SRC0_NEGATE_BIT		45U
#define EU_3SRC_SRC0_ABS_BIT		44U
#define EU_3SRC_SRC1_REG_NR_HI		111U
#define EU_3SRC_SRC1_REG_NR_LO		104U
#define EU_3SRC_SRC1_SUBREG_HI		103U
#define EU_3SRC_SRC1_SUBREG_LO		99U
#define EU_3SRC_SRC1_FILE_BIT		98U
#define EU_3SRC_SRC1_HSTRIDE_HI		97U
#define EU_3SRC_SRC1_HSTRIDE_LO		96U
#define EU_3SRC_SRC1_VSTRIDE_HIGH_BIT	91U
#define EU_3SRC_SRC1_VSTRIDE_LOW_BIT	83U
#define EU_3SRC_SRC1_TYPE_HI		90U
#define EU_3SRC_SRC1_TYPE_LO		88U
#define EU_3SRC_SRC1_NEGATE_BIT		87U
#define EU_3SRC_SRC1_ABS_BIT		86U
#define EU_3SRC_SRC2_REG_NR_HI		127U
#define EU_3SRC_SRC2_REG_NR_LO		120U
#define EU_3SRC_SRC2_SUBREG_HI		119U
#define EU_3SRC_SRC2_SUBREG_LO		115U
#define EU_3SRC_SRC2_FILE_BIT		114U
#define EU_3SRC_SRC2_HSTRIDE_HI		113U
#define EU_3SRC_SRC2_HSTRIDE_LO		112U
#define EU_3SRC_SRC2_TYPE_HI		82U
#define EU_3SRC_SRC2_TYPE_LO		80U
#define EU_3SRC_SRC2_NEGATE_BIT		85U
#define EU_3SRC_SRC2_ABS_BIT		84U
#define EU_3SRC_TYPE_F			2U
#define EU_3SRC_VSTRIDE_0		0U
#define EU_3SRC_VSTRIDE_8		3U

#define EU_HSTRIDE_0			0U
#define EU_HSTRIDE_1			1U
#define EU_HSTRIDE_2			2U
#define EU_WIDTH_1			0U
#define EU_WIDTH_2			1U
#define EU_WIDTH_4			2U
#define EU_WIDTH_8			3U
#define EU_VSTRIDE_0			0U
#define EU_VSTRIDE_1			1U
#define EU_VSTRIDE_2			2U
#define EU_VSTRIDE_4			3U
#define EU_VSTRIDE_8			4U
#define EU_VSTRIDE_16			5U

/*
 * Math function selectors.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_defines.h
 * (BRW_MATH_FUNCTION_*).
 */
#define EU_MATH_INV			1U
#define EU_MATH_LOG			2U
#define EU_MATH_EXP			3U
#define EU_MATH_SQRT			4U
#define EU_MATH_RSQ			5U
#define EU_MATH_SIN			6U
#define EU_MATH_COS			7U
#define EU_MATH_INT_DIV_QUOTIENT	12U
#define EU_MATH_INT_DIV_REMAINDER	13U

/*
 * Shared functions a SEND addresses.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_defines.h
 * (BRW_SFID_SAMPLER, GFX6_SFID_DATAPORT_RENDER_CACHE, BRW_SFID_URB,
 * GFX7_SFID_DATAPORT_DATA_CACHE).
 */
#define EU_SFID_SAMPLER			2U
#define EU_SFID_RENDER_CACHE		5U
#define EU_SFID_URB			6U
#define EU_SFID_DATA_CACHE		10U

/*
 * The message descriptor: the payload and reply lengths in registers,
 * and whether a header leads the payload.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu.h
 * (brw_message_desc).
 */
#define EU_DESC_MLEN_SHIFT		25
#define EU_DESC_RLEN_SHIFT		20
#define EU_DESC_HEADER_PRESENT		(1U << 19)

/*
 * A data-port descriptor: the binding table index, the message control
 * and the message type.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu.h (brw_dp_desc).
 */
#define EU_DP_CONTROL_SHIFT		8
#define EU_DP_TYPE_SHIFT		14

/*
 * The OWord block messages of the data cache, and the block of two
 * OWords (one SIMD8 register).
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_defines.h
 * (GFX7_DATAPORT_DC_OWORD_BLOCK_READ, GFX7_DATAPORT_DC_OWORD_BLOCK_WRITE,
 * BRW_DATAPORT_OWORD_BLOCK_2_OWORDS).
 */
#define EU_DP_OWORD_BLOCK_READ		0U
#define EU_DP_OWORD_BLOCK_WRITE		8U
#define EU_DP_OWORD_BLOCK_2_OWORDS	2U

/*
 * A sampler descriptor before Xe2: the binding table index in bits 7:0, the
 * sampler in 11:8, the message type in 16:12 and the SIMD mode in 18:17 (1:
 * SIMD8).  The message types are sample, sample with bias and sample with
 * LOD.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu.h
 * (brw_sampler_desc), brw_eu_defines.h (BRW_SAMPLER_SIMD_MODE_SIMD8,
 * GFX5_SAMPLER_MESSAGE_SAMPLE, GFX5_SAMPLER_MESSAGE_SAMPLE_BIAS,
 * GFX5_SAMPLER_MESSAGE_SAMPLE_LOD).
 */
#define EU_SAMPLER_INDEX_SHIFT		8
#define EU_SAMPLER_TYPE_SHIFT		12
#define EU_SAMPLER_TYPE_MASK		0x1FU
#define EU_SAMPLER_SIMD_SHIFT		17
#define EU_SAMPLER_SIMD8		1U
#define EU_SAMPLER_MESSAGE_SAMPLE	0U
#define EU_SAMPLER_MESSAGE_SAMPLE_BIAS	1U
#define EU_SAMPLER_MESSAGE_SAMPLE_LOD	2U

/*
 * The sampler message header of Gen11 and later: cleared, the texel offsets
 * in dword 2, the sampler state pointer in dword 3 -- r0.3 with its low five
 * bits cleared.
 *
 * Values referenced: Mesa 25.0.7
 * src/intel/compiler/brw_lower_logical_sends.cpp (lower_sampler_logical_send).
 */
#define EU_SAMPLER_HEADER_OFFSET_DWORD	2U
#define EU_SAMPLER_HEADER_STATE_DWORD	3U
#define EU_SAMPLER_STATE_POINTER_MASK	0xffffffe0U

/*
 * The binding table index of stateless, non-coherent accesses: the
 * scratch space.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_defines.h
 * (GFX8_BTI_STATELESS_NON_COHERENT).
 */
#define EU_BTI_STATELESS_NON_COHERENT	253U

/*
 * The scratch message header: the offset (in OWords) in dword 2; dwords 3
 * and 5 copy the per-thread scratch space (r0.3 bits 3:0) and the thread's
 * scratch base (r0.5 bits 31:10) out of the thread payload.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_generator.cpp
 * (generate_scratch_header).
 */
#define EU_SCRATCH_HEADER_OFFSET_DWORD	2U
#define EU_SCRATCH_HEADER_SIZE_DWORD	3U
#define EU_SCRATCH_HEADER_BASE_DWORD	5U
#define EU_SCRATCH_SIZE_MASK		0x0000000fU
#define EU_SCRATCH_BASE_MASK		0xfffffc00U

/*
 * SWSB, the software scoreboard byte of Gen12.0:
 *   regdist n                  = n          wait for the n-th in-order instruction before this one
 *   token set                  = 0x40 | id  an out-of-order instruction (MATH, SEND) names itself
 *   regdist n + token set      = 0x80 | n << 4 | id
 *   sync.nop on a token's dst  = 0x20 | id  wait until that instruction has written its destination
 *   sync.nop on a token's src  = 0x30 | id  wait until it has read its sources
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_defines.h (the
 * Gen12 SWSB encoding of tgl_swsb_encode).
 */
#define EU_SWSB_REGDIST(n)		((uint32_t)(n))
#define EU_SWSB_REGDIST_SET(n, id)	(0x80U | ((uint32_t)(n) << 4) | (uint32_t)(id))
#define EU_SWSB_SYNC_DST(id)		(0x20U | (uint32_t)(id))
#define EU_SWSB_SYNC_SRC(id)		(0x30U | (uint32_t)(id))

/*
 * The messages of compute shaders: the thread spawner and the A64 untyped
 * atomic of data cache 1 (the operation in the message control's bits 3:0,
 * bit 4 for 64-bit data, bit 5 when the old value is returned; binding
 * table entry 253).  A compute thread ends with a one-register message of
 * r0 to the thread spawner, descriptor 0.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_defines.h
 * (BRW_SFID_THREAD_SPAWNER, HSW_SFID_DATAPORT_DATA_CACHE_1,
 * GFX8_DATAPORT_DC_PORT1_A64_UNTYPED_ATOMIC_OP), brw_eu.h
 * (brw_dp_a64_untyped_atomic_desc).
 */
#define EU_SFID_THREAD_SPAWNER		7U
#define EU_SFID_DATA_CACHE_1		12U
#define EU_DP_A64_UNTYPED_ATOMIC	0x12U
#define EU_DP_ATOMIC_RETURN		(1U << 5)
#define EU_DESC_TERMINATE		(1U << EU_DESC_MLEN_SHIFT)

/*
 * Shared memory, fences and the workgroup barrier of compute shaders: the
 * gateway's barrier message, the untyped surface read, write and atomic
 * (the channel mask in the control's bits 3:0, 0xe for one channel; SIMD8
 * as 2 in bits 5:4 for read and write, bit 4 for an atomic; the old value
 * returned in bit 5), the memory fence with its commit bit 5, the binding
 * table entries of the data cache (0) and of shared memory (254), and the
 * sync functions.  The barrier ID is bits 30:24 of r0.2.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_defines.h
 * (BRW_SFID_MESSAGE_GATEWAY, BRW_MESSAGE_GATEWAY_SFID_BARRIER_MSG,
 * GFX7_DATAPORT_DC_MEMORY_FENCE, HSW_DATAPORT_DC_PORT1_UNTYPED_SURFACE_READ,
 * HSW_DATAPORT_DC_PORT1_UNTYPED_ATOMIC_OP,
 * HSW_DATAPORT_DC_PORT1_UNTYPED_SURFACE_WRITE, GFX7_BTI_SLM, TGL_SYNC_ALLWR,
 * TGL_SYNC_BAR), brw_eu.h (brw_dp_untyped_surface_rw_desc,
 * brw_dp_untyped_atomic_desc, brw_mdc_cmask), brw_fs_nir.cpp (emit_barrier).
 */
#define EU_SFID_GATEWAY			3U
#define EU_GATEWAY_BARRIER		4U
#define EU_DP_UNTYPED_READ		1U
#define EU_DP_UNTYPED_ATOMIC		2U
#define EU_DP_UNTYPED_WRITE		9U
#define EU_DP_UNTYPED_ONE_CHANNEL	0x0eU
#define EU_DP_UNTYPED_SIMD8		(2U << 4)
#define EU_DP_ATOMIC_SIMD8		(1U << 4)
#define EU_DP_MEMORY_FENCE		7U
#define EU_DP_FENCE_COMMIT		(1U << 5)
#define EU_BTI_DATA_CACHE		0U
#define EU_BTI_SLM			254U
#define EU_SYNC_ALLWR			3U
#define EU_SYNC_BAR			0x0eU
#define EU_BARRIER_ID_DWORD		2U
#define EU_BARRIER_ID_MASK		0x7f000000U

#endif /* DRIVERS_GPU_I915_INTEL_EU_ENCODING_GEN12_H */
