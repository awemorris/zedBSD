/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GSI1 instruction encoder (gsi1-encode.h).
 */

#include "gsi1-encode.h"

#include <stddef.h>
#include <stdint.h>

/* The low five bits of a register number go in word0, the high two in word1. */
#define GSI1_LOW_REGISTER_BITS		0x1fU
#define GSI1_HIGH_REGISTER_SHIFT	5U
#define GSI1_HIGH_REGISTER_BITS		0x3U

/* The width of the fields an immediate is split into: 12 bits in word0, 10 in word1. */
#define GSI1_IMMEDIATE_LOW_BITS		0xfffU
#define GSI1_IMMEDIATE_HIGH_BITS	0x3ffU
#define GSI1_IMMEDIATE_LOW_WIDTH	12U

/* The bits of a texel offset (-8 to 7). */
#define GSI1_TEXEL_OFFSET_BITS		0xfU

/* The bits of a texture or sampler slot (0 to 15). */
#define GSI1_SLOT_BITS			0xfU

/* The bits of the sub-operation field and of a 7-bit field. */
#define GSI1_SUB_BITS			0x1fU
#define GSI1_SEVEN_BITS			0x7fU
#define GSI1_THREE_BITS			0x7U

static uint32_t encode_high_registers(uint32_t rd, uint32_t rs1, uint32_t rs2, uint32_t rs3);
static uint32_t encode_extra(const struct gsi1_extra *extra);
static void split_immediate(int32_t immediate, uint32_t *low, uint32_t *high);
static uint64_t join_words(uint32_t word0, uint32_t word1);

/*
 * Encodes an R-form instruction.
 *
 * The opcode, the two function fields and the low register bits fill
 * word0; word1 has the high register bits and the extra fields.
 */
uint64_t
gsi1_encode_r(
	uint32_t opcode,
	uint32_t funct3,
	uint32_t funct7,
	uint32_t rd,
	uint32_t rs1,
	uint32_t rs2,
	const struct gsi1_extra *extra)
{
	uint32_t word0;
	uint32_t word1;

	/* The RISC-V R layout of the low word. */
	word0 = (funct7 & GSI1_SEVEN_BITS) << GSI1_W0_FUNCT7_SHIFT;
	word0 |= (rs2 & GSI1_LOW_REGISTER_BITS) << GSI1_W0_RS2_SHIFT;
	word0 |= (rs1 & GSI1_LOW_REGISTER_BITS) << GSI1_W0_RS1_SHIFT;
	word0 |= (funct3 & GSI1_THREE_BITS) << GSI1_W0_FUNCT3_SHIFT;
	word0 |= (rd & GSI1_LOW_REGISTER_BITS) << GSI1_W0_RD_SHIFT;
	word0 |= opcode & GSI1_SEVEN_BITS;

	/* The extension word. */
	word1 = encode_high_registers(rd, rs1, rs2, 0U);
	word1 |= encode_extra(extra);

	/* Succeeded: the instruction's two words. */
	return join_words(word0, word1);
}

/*
 * Encodes an R4-form instruction: three sources, rs3 in word0's top five
 * bits, the format field (bits 26:25) zero.
 */
uint64_t
gsi1_encode_r4(
	uint32_t opcode,
	uint32_t funct3,
	uint32_t rd,
	uint32_t rs1,
	uint32_t rs2,
	uint32_t rs3,
	const struct gsi1_extra *extra)
{
	uint32_t word0;
	uint32_t word1;

	/* The RISC-V R4 layout of the low word. */
	word0 = (rs3 & GSI1_LOW_REGISTER_BITS) << GSI1_W0_RS3_SHIFT;
	word0 |= (rs2 & GSI1_LOW_REGISTER_BITS) << GSI1_W0_RS2_SHIFT;
	word0 |= (rs1 & GSI1_LOW_REGISTER_BITS) << GSI1_W0_RS1_SHIFT;
	word0 |= (funct3 & GSI1_THREE_BITS) << GSI1_W0_FUNCT3_SHIFT;
	word0 |= (rd & GSI1_LOW_REGISTER_BITS) << GSI1_W0_RD_SHIFT;
	word0 |= opcode & GSI1_SEVEN_BITS;

	/* The extension word. */
	word1 = encode_high_registers(rd, rs1, rs2, rs3);
	word1 |= encode_extra(extra);

	/* Succeeded: the instruction's two words. */
	return join_words(word0, word1);
}

/*
 * Encodes an I-form instruction.
 *
 * The immediate is sext(imm[11:0]) + (sext(imm_hi) << 12): its low 12
 * bits in word0 and the rest in word1's imm_hi.
 */
uint64_t
gsi1_encode_i(
	uint32_t opcode,
	uint32_t funct3,
	uint32_t rd,
	uint32_t rs1,
	int32_t immediate,
	uint32_t sub)
{
	uint32_t word0;
	uint32_t word1;
	uint32_t low;
	uint32_t high;

	/* The immediate's two parts. */
	split_immediate(immediate, &low, &high);

	/* The RISC-V I layout of the low word. */
	word0 = low << GSI1_W0_IMM_SHIFT;
	word0 |= (rs1 & GSI1_LOW_REGISTER_BITS) << GSI1_W0_RS1_SHIFT;
	word0 |= (funct3 & GSI1_THREE_BITS) << GSI1_W0_FUNCT3_SHIFT;
	word0 |= (rd & GSI1_LOW_REGISTER_BITS) << GSI1_W0_RD_SHIFT;
	word0 |= opcode & GSI1_SEVEN_BITS;

	/* The extension word: the high register bits, the sub-operation and the high immediate. */
	word1 = encode_high_registers(rd, rs1, 0U, 0U);
	word1 |= (sub & GSI1_SUB_BITS) << GSI1_W1_SUB_SHIFT;
	word1 |= high << GSI1_W1_IMM_HI_SHIFT;

	/* Succeeded: the instruction's two words. */
	return join_words(word0, word1);
}

/*
 * Encodes an I-form shift by a constant amount: the amount in imm[4:0]
 * and the shift's funct7 above it (srai's 0100000), no high immediate.
 */
uint64_t
gsi1_encode_shift(
	uint32_t funct3,
	uint32_t funct7,
	uint32_t rd,
	uint32_t rs1,
	uint32_t amount)
{
	uint32_t word0;
	uint32_t word1;
	uint32_t field;

	/* The amount and the function bits of the immediate field. */
	field = (amount & GSI1_LOW_REGISTER_BITS) | ((funct7 & GSI1_SEVEN_BITS) << 5);

	/* The RISC-V I layout of the low word. */
	word0 = field << GSI1_W0_IMM_SHIFT;
	word0 |= (rs1 & GSI1_LOW_REGISTER_BITS) << GSI1_W0_RS1_SHIFT;
	word0 |= (funct3 & GSI1_THREE_BITS) << GSI1_W0_FUNCT3_SHIFT;
	word0 |= (rd & GSI1_LOW_REGISTER_BITS) << GSI1_W0_RD_SHIFT;
	word0 |= GSI1_OP_OPIMM;

	/* The extension word has the high register bits only. */
	word1 = encode_high_registers(rd, rs1, 0U, 0U);

	/* Succeeded: the instruction's two words. */
	return join_words(word0, word1);
}

/*
 * Encodes an S-form instruction: the immediate's low 12 bits split around
 * rs2 as RISC-V places them, the rest in word1's imm_hi.
 */
uint64_t
gsi1_encode_s(
	uint32_t opcode,
	uint32_t funct3,
	uint32_t rs1,
	uint32_t rs2,
	int32_t immediate)
{
	uint32_t word0;
	uint32_t word1;
	uint32_t low;
	uint32_t high;

	/* The immediate's two parts. */
	split_immediate(immediate, &low, &high);

	/* The RISC-V S layout of the low word: imm[11:5] on top, imm[4:0] where rd would be. */
	word0 = (low >> 5) << GSI1_W0_FUNCT7_SHIFT;
	word0 |= (rs2 & GSI1_LOW_REGISTER_BITS) << GSI1_W0_RS2_SHIFT;
	word0 |= (rs1 & GSI1_LOW_REGISTER_BITS) << GSI1_W0_RS1_SHIFT;
	word0 |= (funct3 & GSI1_THREE_BITS) << GSI1_W0_FUNCT3_SHIFT;
	word0 |= (low & GSI1_LOW_REGISTER_BITS) << GSI1_W0_RD_SHIFT;
	word0 |= opcode & GSI1_SEVEN_BITS;

	/* The extension word: the high register bits of the sources and the high immediate. */
	word1 = encode_high_registers(0U, rs1, rs2, 0U);
	word1 |= high << GSI1_W1_IMM_HI_SHIFT;

	/* Succeeded: the instruction's two words. */
	return join_words(word0, word1);
}

/*
 * Encodes a branch.
 *
 * The offset counts instructions.  Its low 12 bits are scattered over
 * word0 as the RISC-V B layout scatters imm[12:1] (bit 11 in word0[31],
 * bits 9:4 in word0[30:25], bits 3:0 in word0[11:8], bit 10 in word0[7]),
 * the rest in word1's imm_hi.
 */
uint64_t
gsi1_encode_b(
	uint32_t funct3,
	uint32_t rs1,
	int32_t offset)
{
	uint32_t word0;
	uint32_t word1;
	uint32_t low;
	uint32_t high;

	/* The offset's two parts. */
	split_immediate(offset, &low, &high);

	/* The RISC-V B layout of the low word. */
	word0 = ((low >> 11) & 1U) << 31;
	word0 |= ((low >> 4) & 0x3fU) << 25;
	word0 |= (rs1 & GSI1_LOW_REGISTER_BITS) << GSI1_W0_RS1_SHIFT;
	word0 |= (funct3 & GSI1_THREE_BITS) << GSI1_W0_FUNCT3_SHIFT;
	word0 |= (low & 0xfU) << 8;
	word0 |= ((low >> 10) & 1U) << 7;
	word0 |= GSI1_OP_BRANCH;

	/* The extension word: the high bits of rs1 and of the offset. */
	word1 = encode_high_registers(0U, rs1, 0U, 0U);
	word1 |= high << GSI1_W1_IMM_HI_SHIFT;

	/* Succeeded: the instruction's two words. */
	return join_words(word0, word1);
}

/*
 * Encodes movi: lui's U layout with the value's high 20 bits in word0 and
 * its low 12 bits in word1[13:2].
 */
uint64_t
gsi1_encode_movi(
	uint32_t rd,
	uint32_t value)
{
	uint32_t word0;
	uint32_t word1;

	/* The U layout of the low word with imm[31:12]. */
	word0 = (value >> GSI1_IMMEDIATE_LOW_WIDTH) << GSI1_W0_UIMM_SHIFT;
	word0 |= (rd & GSI1_LOW_REGISTER_BITS) << GSI1_W0_RD_SHIFT;
	word0 |= GSI1_OP_LUI;

	/* The extension word: rd's high bits and imm[11:0]. */
	word1 = (rd >> GSI1_HIGH_REGISTER_SHIFT) & GSI1_HIGH_REGISTER_BITS;
	word1 |= (value & GSI1_IMMEDIATE_LOW_BITS) << GSI1_W1_MOVI_SHIFT;

	/* Succeeded: the instruction's two words. */
	return join_words(word0, word1);
}

/*
 * Reports whether an immediate fits the 22-bit signed offsets of the I, S
 * and B forms.
 */
int
gsi1_immediate_fits(
	int64_t immediate)
{
	/* Below the smallest offset. */
	if (immediate < (int64_t)GSI1_IMM_MIN)
		return 0;

	/* Above the largest offset. */
	if (immediate > (int64_t)GSI1_IMM_MAX)
		return 0;

	/* Succeeded: the offset can be encoded. */
	return 1;
}

/* Packs the high two bits of each register number into word1's low byte. */
static uint32_t
encode_high_registers(
	uint32_t rd,
	uint32_t rs1,
	uint32_t rs2,
	uint32_t rs3)
{
	uint32_t word1;

	/* Each register's bits 6:5 in its own two-bit field. */
	word1 = ((rd >> GSI1_HIGH_REGISTER_SHIFT) & GSI1_HIGH_REGISTER_BITS) << GSI1_W1_RD_SHIFT;
	word1 |= ((rs1 >> GSI1_HIGH_REGISTER_SHIFT) & GSI1_HIGH_REGISTER_BITS) << GSI1_W1_RS1_SHIFT;
	word1 |= ((rs2 >> GSI1_HIGH_REGISTER_SHIFT) & GSI1_HIGH_REGISTER_BITS) << GSI1_W1_RS2_SHIFT;
	word1 |= ((rs3 >> GSI1_HIGH_REGISTER_SHIFT) & GSI1_HIGH_REGISTER_BITS) << GSI1_W1_RS3_SHIFT;

	/* Succeeded: the register field of word1. */
	return word1;
}

/*
 * Packs the extra fields of word1: the modifiers and saturation (already
 * at their word1 positions), the sub-operation, and the texel offsets.
 */
static uint32_t
encode_extra(
	const struct gsi1_extra *extra)
{
	uint32_t word1;

	/* An instruction without extras has none of these bits. */
	if (extra == NULL)
		return 0U;

	/* The modifiers, saturation and half select are at their own positions. */
	word1 = extra->modifiers;

	/* The sub-operation. */
	word1 |= (extra->sub & GSI1_SUB_BITS) << GSI1_W1_SUB_SHIFT;

	/* A texture R form's sampler slot. */
	word1 |= (extra->sampler & GSI1_SLOT_BITS) << GSI1_W1_SAMP_SLOT_SHIFT;

	/* The texel offsets, four signed bits each. */
	word1 |= ((uint32_t)extra->offset_u & GSI1_TEXEL_OFFSET_BITS) << GSI1_W1_TEX_DU_SHIFT;
	word1 |= ((uint32_t)extra->offset_v & GSI1_TEXEL_OFFSET_BITS) << GSI1_W1_TEX_DV_SHIFT;
	word1 |= ((uint32_t)extra->offset_w & GSI1_TEXEL_OFFSET_BITS) << GSI1_W1_TEX_DW_SHIFT;

	/* Succeeded: the extra fields of word1. */
	return word1;
}

/*
 * Splits a 22-bit signed immediate into its sign-extended low 12 bits and
 * the 10 bits above, so that low + (high << 12) is the immediate.
 */
static void
split_immediate(
	int32_t immediate,
	uint32_t *low,
	uint32_t *high)
{
	int32_t low_value;
	int32_t high_value;

	/* The low part, sign-extended from its 12 bits. */
	low_value = (int32_t)((uint32_t)immediate & GSI1_IMMEDIATE_LOW_BITS);
	if (low_value >= 0x800)
		low_value -= 0x1000;

	/* What the low part leaves, in units of 4096. */
	high_value = (immediate - low_value) / 0x1000;

	/* Both parts as their fields' bits. */
	*low = (uint32_t)low_value & GSI1_IMMEDIATE_LOW_BITS;
	*high = (uint32_t)high_value & GSI1_IMMEDIATE_HIGH_BITS;
}

/* Joins word0 (low) and word1 (high) into one instruction. */
static uint64_t
join_words(
	uint32_t word0,
	uint32_t word1)
{
	uint64_t instruction;

	/* The low word first in memory, so in the low half. */
	instruction = (uint64_t)word1 << 32;
	instruction |= (uint64_t)word0;

	/* Succeeded: the instruction. */
	return instruction;
}
