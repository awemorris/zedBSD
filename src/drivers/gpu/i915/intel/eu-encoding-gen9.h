/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Gen9 EU instruction encoding, field bit positions and values, for
 * compiler/eu.c.  The Gen9 native instruction is four 32-bit words (128
 * bits), like Gen12's; ranges are HI, LO inclusive.  The Align1 access mode
 * (bit 8 clear) is the only one compiler/eu.c emits.  Gen9 has no software
 * scoreboard: the hardware tracks register dependencies itself.
 */

#ifndef DRIVERS_GPU_I915_INTEL_EU_ENCODING_GEN9_H
#define DRIVERS_GPU_I915_INTEL_EU_ENCODING_GEN9_H

/*
 * Control fields.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_inst.h (the Gen9
 * positions of the control fields).
 */
#define GEN9_EU_OPCODE_HI		6
#define GEN9_EU_OPCODE_LO		0
#define GEN9_EU_PRED_CONTROL_HI		19
#define GEN9_EU_PRED_CONTROL_LO		16
#define GEN9_EU_PRED_INV_BIT		20
#define GEN9_EU_EXEC_SIZE_HI		23
#define GEN9_EU_EXEC_SIZE_LO		21

/*
 * The conditional modifier, the math function and a SEND's shared
 * function share bits 27:24.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_inst.h (the Gen9
 * positions of cond_modifier, math_function, sfid).
 */
#define GEN9_EU_COND_MODIFIER_HI	27
#define GEN9_EU_COND_MODIFIER_LO	24
#define GEN9_EU_MATH_FUNCTION_HI	27
#define GEN9_EU_MATH_FUNCTION_LO	24
#define GEN9_EU_SEND_SFID_HI		27
#define GEN9_EU_SEND_SFID_LO		24

#define GEN9_EU_SATURATE_BIT		31
#define GEN9_EU_FLAG_SUBREG_NR_BIT	32
#define GEN9_EU_FLAG_REG_NR_BIT		33
#define GEN9_EU_NO_MASK_BIT		34

/*
 * Destination (direct, Align1).  The register file has two bits.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_inst.h (the Gen9
 * positions of the dst fields).
 */
#define GEN9_EU_DST_REG_FILE_HI		36
#define GEN9_EU_DST_REG_FILE_LO		35
#define GEN9_EU_DST_REG_TYPE_HI		40
#define GEN9_EU_DST_REG_TYPE_LO		37
#define GEN9_EU_DST_SUBREG_HI		52
#define GEN9_EU_DST_SUBREG_LO		48
#define GEN9_EU_DST_REG_NR_HI		60
#define GEN9_EU_DST_REG_NR_LO		53
#define GEN9_EU_DST_HSTRIDE_HI		62
#define GEN9_EU_DST_HSTRIDE_LO		61

/*
 * Source 0 (direct, Align1).
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_inst.h (the Gen9
 * positions of the src0 fields).
 */
#define GEN9_EU_SRC0_REG_FILE_HI	42
#define GEN9_EU_SRC0_REG_FILE_LO	41
#define GEN9_EU_SRC0_REG_TYPE_HI	46
#define GEN9_EU_SRC0_REG_TYPE_LO	43
#define GEN9_EU_SRC0_SUBREG_HI		68
#define GEN9_EU_SRC0_SUBREG_LO		64
#define GEN9_EU_SRC0_REG_NR_HI		76
#define GEN9_EU_SRC0_REG_NR_LO		69
#define GEN9_EU_SRC0_ABS_BIT		77
#define GEN9_EU_SRC0_NEGATE_BIT		78
#define GEN9_EU_SRC0_HSTRIDE_HI		81
#define GEN9_EU_SRC0_HSTRIDE_LO		80
#define GEN9_EU_SRC0_WIDTH_HI		84
#define GEN9_EU_SRC0_WIDTH_LO		82
#define GEN9_EU_SRC0_VSTRIDE_HI		88
#define GEN9_EU_SRC0_VSTRIDE_LO		85

/*
 * Source 1 (direct, Align1).
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_inst.h (the Gen9
 * positions of the src1 fields).
 */
#define GEN9_EU_SRC1_REG_FILE_HI	90
#define GEN9_EU_SRC1_REG_FILE_LO	89
#define GEN9_EU_SRC1_REG_TYPE_HI	94
#define GEN9_EU_SRC1_REG_TYPE_LO	91
#define GEN9_EU_SRC1_SUBREG_HI		100
#define GEN9_EU_SRC1_SUBREG_LO		96
#define GEN9_EU_SRC1_REG_NR_HI		108
#define GEN9_EU_SRC1_REG_NR_LO		101
#define GEN9_EU_SRC1_ABS_BIT		109
#define GEN9_EU_SRC1_NEGATE_BIT		110
#define GEN9_EU_SRC1_HSTRIDE_HI		113
#define GEN9_EU_SRC1_HSTRIDE_LO		112
#define GEN9_EU_SRC1_WIDTH_HI		116
#define GEN9_EU_SRC1_WIDTH_LO		114
#define GEN9_EU_SRC1_VSTRIDE_HI		120
#define GEN9_EU_SRC1_VSTRIDE_LO		117

/*
 * A 32-bit immediate occupies the last word, whichever source it is.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_inst.h (the Gen9
 * positions of imm_ud).
 */
#define GEN9_EU_IMM32_HI		127
#define GEN9_EU_IMM32_LO		96

/*
 * A branch's jump target and second target: bytes from the branch, as
 * on Gen12.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_inst.h (the Gen9
 * positions of jip, uip).
 */
#define GEN9_EU_JIP_HI			127
#define GEN9_EU_JIP_LO			96
#define GEN9_EU_UIP_HI			95
#define GEN9_EU_UIP_LO			64

/*
 * SEND / SENDC: the descriptor is source 1's UD immediate, of which bits
 * 126:96 carry descriptor bits 30:0 and bit 127 is the end-of-thread bit.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_inst.h (the Gen9
 * positions of send_desc, eot).
 */
#define GEN9_EU_SEND_DESC_HI		126
#define GEN9_EU_SEND_DESC_LO		96
#define GEN9_EU_SEND_EOT_BIT		127

/*
 * The extended descriptor of a single-payload SEND (Gen9 to Gen11): its bits
 * 31:28, 27:24, 23:20 and 19:16 share the instruction with source 1's type
 * (94:91) and source 0's region (88:85, 83:80) and subregister (67:64).  It
 * is written after the operands -- zero for every message compiler/eu.c
 * sends this way -- so those operand fields end up clear.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_inst.h
 * (brw_eu_inst_set_send_ex_desc).
 */
#define GEN9_EU_SEND_EX_DESC_31_28_HI	94
#define GEN9_EU_SEND_EX_DESC_31_28_LO	91
#define GEN9_EU_SEND_EX_DESC_27_24_HI	88
#define GEN9_EU_SEND_EX_DESC_27_24_LO	85
#define GEN9_EU_SEND_EX_DESC_23_20_HI	83
#define GEN9_EU_SEND_EX_DESC_23_20_LO	80
#define GEN9_EU_SEND_EX_DESC_19_16_HI	67
#define GEN9_EU_SEND_EX_DESC_19_16_LO	64

/*
 * SENDS / SENDSC, a message of two payloads: a one-bit destination file, the
 * second payload's file and register, and the extended descriptor's bits
 * 31:16 and 9:6 (the length of the second payload); its other bits must be
 * zero.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_inst.h
 * (brw_eu_inst_set_sends_ex_desc and the Gen9 sends fields).
 */
#define GEN9_EU_SENDS_DST_FILE_BIT	35
#define GEN9_EU_SENDS_SRC1_FILE_BIT	36
#define GEN9_EU_SENDS_SRC1_REG_NR_HI	51
#define GEN9_EU_SENDS_SRC1_REG_NR_LO	44
#define GEN9_EU_SENDS_EX_DESC_HIGH_HI	95
#define GEN9_EU_SENDS_EX_DESC_HIGH_LO	80
#define GEN9_EU_SENDS_EX_MLEN_HI	67
#define GEN9_EU_SENDS_EX_MLEN_LO	64
#define GEN9_EU_SENDS_EX_DESC_HIGH_SHIFT	16
#define GEN9_EU_SENDS_EX_MLEN_SHIFT	6
#define GEN9_EU_SENDS_EX_MLEN_MASK	0xfU
#define GEN9_EU_SENDS_EX_DESC_ZERO_MASK	0x0000fc3fU

/*
 * Register files: two bits, the immediate one of them.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_inst.h
 * (brw_reg_file_to_hw_reg_file).
 */
#define GEN9_EU_FILE_ARF		0U
#define GEN9_EU_FILE_GRF		1U
#define GEN9_EU_FILE_IMM		3U

/*
 * Hardware opcodes before Gen12.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu.c (the opcode_descs
 * rows for Gen9).
 */
#define GEN9_EU_OP_MOV			1U
#define GEN9_EU_OP_SEL			2U
#define GEN9_EU_OP_NOT			4U
#define GEN9_EU_OP_AND			5U
#define GEN9_EU_OP_OR			6U
#define GEN9_EU_OP_XOR			7U
#define GEN9_EU_OP_SHR			8U
#define GEN9_EU_OP_SHL			9U
#define GEN9_EU_OP_ASR			12U
#define GEN9_EU_OP_CMP			16U
#define GEN9_EU_OP_IF			34U
#define GEN9_EU_OP_ENDIF		37U
#define GEN9_EU_OP_WHILE		39U
#define GEN9_EU_OP_WAIT			48U
#define GEN9_EU_OP_SEND			49U
#define GEN9_EU_OP_SENDC		50U
#define GEN9_EU_OP_SENDS		51U
#define GEN9_EU_OP_SENDSC		52U
#define GEN9_EU_OP_MATH			56U
#define GEN9_EU_OP_ADD			64U
#define GEN9_EU_OP_MUL			65U
#define GEN9_EU_OP_FRC			67U
#define GEN9_EU_OP_RNDD			69U
#define GEN9_EU_OP_RNDE			70U
#define GEN9_EU_OP_RNDZ			71U
#define GEN9_EU_OP_NOP			126U

/*
 * Register types of a register operand.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_reg_type.c
 * (brw_type_encode, before Gen11).
 */
#define GEN9_EU_TYPE_UD			0U
#define GEN9_EU_TYPE_D			1U
#define GEN9_EU_TYPE_UW			2U
#define GEN9_EU_TYPE_W			3U
#define GEN9_EU_TYPE_F			7U
#define GEN9_EU_TYPE_HF			10U

/*
 * Immediate types that differ from the register ones: the vector V and
 * the half float.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_reg_type.c
 * (brw_type_encode, before Gen11).
 */
#define GEN9_EU_IMM_TYPE_V		6U
#define GEN9_EU_IMM_TYPE_HF		11U

/*
 * The notification register n0 a WAIT waits on: the gateway answers a
 * thread's barrier message there.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_eu_defines.h
 * (BRW_ARF_NOTIFICATION_COUNT).
 */
#define GEN9_EU_ARF_NOTIFICATION	0x90U

/*
 * The messages whose form differs before Gen11 (compiler/compile*.c): the
 * "do not dereference URB" bit of the message that ends a compute thread,
 * and the barrier ID's mask in r0.2 on Gen9.
 *
 * Values referenced: Mesa 25.0.7 src/intel/compiler/brw_fs_visitor.cpp
 * (emit_cs_terminate), brw_fs_nir.cpp (emit_barrier).
 */
#define GEN9_EU_DESC_TERMINATE_NO_URB	(1U << 4)
#define GEN9_EU_BARRIER_ID_MASK		0x8f000000U

#endif /* DRIVERS_GPU_I915_INTEL_EU_ENCODING_GEN9_H */
