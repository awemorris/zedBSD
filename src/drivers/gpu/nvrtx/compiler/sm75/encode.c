/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The encoder of Turing (SM75) instructions (see encode.h).
 *
 * The placement of every field follows the facts in isa.h.  An ALU
 * instruction has three sources: the first is always a register; the
 * second and the third take one of the forms -- both registers, one
 * an immediate, one a constant buffer word -- and the form field says
 * which.  When the third source is the immediate or the constant
 * buffer word, the second source moves into the third's register
 * slot.
 */

#include "drivers/gpu/nvrtx/compiler/sm75/encode.h"

#include <stddef.h>

static void nvrtx_sm75_place_src2(struct nvrtx_sm75_word *word, const struct nvrtx_sm75_src *src);
static void nvrtx_sm75_place_wide(struct nvrtx_sm75_word *word, const struct nvrtx_sm75_src *src);
static void nvrtx_sm75_place_cb(struct nvrtx_sm75_word *word, const struct nvrtx_sm75_src *src);
static void nvrtx_sm75_memory(struct nvrtx_sm75_word *word);

/*
 * Writes a field of up to 64 bits at a bit position.
 */
void
nvrtx_sm75_set(
	struct nvrtx_sm75_word *word,
	unsigned low,
	unsigned bits,
	uint64_t value)
{
	unsigned bit;
	unsigned index;
	uint32_t mask;

	/* Each bit of the field, into the word that holds it. */
	for (bit = 0U; bit < bits; bit++) {
		index = (low + bit) / 32U;
		mask = 1U << ((low + bit) % 32U);

		/* Sets or clears the bit the value has there. */
		if (((value >> bit) & 1U) != 0U) {
			word->w[index] |= mask;
		} else {
			word->w[index] &= ~mask;
		}
	}
}

/*
 * Reads a field of up to 64 bits at a bit position.
 */
uint64_t
nvrtx_sm75_get(
	const struct nvrtx_sm75_word *word,
	unsigned low,
	unsigned bits)
{
	uint64_t value;
	unsigned bit;
	unsigned index;

	/* Each bit of the field, lowest first. */
	value = 0U;
	for (bit = 0U; bit < bits; bit++) {
		index = (low + bit) / 32U;

		/* A set bit of the instruction is the same bit of the value. */
		if (((word->w[index] >> ((low + bit) % 32U)) & 1U) != 0U)
			value |= (uint64_t)1U << bit;
	}

	/* Succeeded: the field's value. */
	return value;
}

/*
 * Starts an instruction: all bits zero, guarded by PT, no scoreboard.
 */
void
nvrtx_sm75_begin(
	struct nvrtx_sm75_word *word)
{
	/* Nothing is set yet. */
	word->w[0] = 0U;
	word->w[1] = 0U;
	word->w[2] = 0U;
	word->w[3] = 0U;

	/* Every thread runs it, and it neither writes nor reads a scoreboard. */
	nvrtx_sm75_guard(word, NVRTX_SM75_PT, 0);
	nvrtx_sm75_control(word, 0U, 0, NVRTX_SM75_BAR_NONE, NVRTX_SM75_BAR_NONE, 0U);
}

/*
 * Guards an instruction by a predicate, or its negation.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (set_pred).
 */
void
nvrtx_sm75_guard(
	struct nvrtx_sm75_word *word,
	unsigned predicate,
	int negate)
{
	/* The predicate and whether it is negated. */
	nvrtx_sm75_set(word, NVRTX_SM75_GUARD_LOW, NVRTX_SM75_PRED_BITS, predicate);
	nvrtx_sm75_set(word, NVRTX_SM75_GUARD_NOT, 1U, (negate != 0) ? 1U : 0U);
}

/*
 * Sets the scheduling control: the cycles before the next instruction, the
 * yield hint, the scoreboard the result releases and the one the source
 * read releases (NVRTX_SM75_BAR_NONE for none), and the scoreboards the
 * instruction waits for before it issues.  The operand reuse stays off.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (set_instr_deps).
 */
void
nvrtx_sm75_control(
	struct nvrtx_sm75_word *word,
	unsigned delay,
	int yield,
	unsigned write_bar,
	unsigned read_bar,
	unsigned wait_mask)
{
	/* The delay and the yield. */
	nvrtx_sm75_set(word, NVRTX_SM75_DELAY_LOW, NVRTX_SM75_DELAY_BITS, delay);
	nvrtx_sm75_set(word, NVRTX_SM75_YIELD, 1U, (yield != 0) ? 1U : 0U);

	/* The scoreboards. */
	nvrtx_sm75_set(word, NVRTX_SM75_WRITE_BAR_LOW, NVRTX_SM75_BAR_BITS, write_bar);
	nvrtx_sm75_set(word, NVRTX_SM75_READ_BAR_LOW, NVRTX_SM75_BAR_BITS, read_bar);
	nvrtx_sm75_set(word, NVRTX_SM75_WAIT_LOW, NVRTX_SM75_WAIT_BITS, wait_mask);
}

/*
 * Makes a register source.
 */
struct nvrtx_sm75_src
nvrtx_sm75_reg(
	uint32_t reg)
{
	struct nvrtx_sm75_src src;

	/* A plain register. */
	src.kind = NVRTX_SM75_SRC_REG;
	src.value = reg;
	src.cb_index = 0U;
	src.neg = 0;
	src.abs = 0;

	/* Succeeded: the operand. */
	return src;
}

/*
 * Makes an immediate source.
 */
struct nvrtx_sm75_src
nvrtx_sm75_imm(
	uint32_t bits)
{
	struct nvrtx_sm75_src src;

	/* The 32 bits as they are. */
	src.kind = NVRTX_SM75_SRC_IMM;
	src.value = bits;
	src.cb_index = 0U;
	src.neg = 0;
	src.abs = 0;

	/* Succeeded: the operand. */
	return src;
}

/*
 * Makes a constant buffer source: the word at a byte offset of a bound buffer.
 */
struct nvrtx_sm75_src
nvrtx_sm75_cb(
	uint32_t index,
	uint32_t offset)
{
	struct nvrtx_sm75_src src;

	/* The buffer and the offset. */
	src.kind = NVRTX_SM75_SRC_CB;
	src.value = offset;
	src.cb_index = index;
	src.neg = 0;
	src.abs = 0;

	/* Succeeded: the operand. */
	return src;
}

/*
 * Encodes the ALU form: the destination, the first source (a register or
 * none), and the second and third sources by the form they make.  A NULL
 * source is absent; the opcode is the 9-bit ALU opcode.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (encode_alu_base).
 */
void
nvrtx_sm75_alu(
	struct nvrtx_sm75_word *word,
	uint32_t opcode,
	uint32_t dst,
	const struct nvrtx_sm75_src *src0,
	const struct nvrtx_sm75_src *src1,
	const struct nvrtx_sm75_src *src2)
{
	uint32_t form;

	/* The destination. */
	nvrtx_sm75_set(word, NVRTX_SM75_DST_LOW, NVRTX_SM75_REG_BITS, dst);

	/* The first source is a register. */
	if (src0 != NULL) {
		nvrtx_sm75_set(word, NVRTX_SM75_SRC0_LOW, NVRTX_SM75_REG_BITS, src0->value);
		nvrtx_sm75_set(word, NVRTX_SM75_SRC0_ABS, 1U, (src0->abs != 0) ? 1U : 0U);
		nvrtx_sm75_set(word, NVRTX_SM75_SRC0_NEG, 1U, (src0->neg != 0) ? 1U : 0U);
	}

	/* The third source chooses the form when it is not a register. */
	if (src2 != NULL && src2->kind == NVRTX_SM75_SRC_IMM) {
		/* The immediate in the wide slot, the second source in the third's register slot. */
		nvrtx_sm75_set(word, NVRTX_SM75_IMM_LOW, NVRTX_SM75_IMM_BITS, src2->value);
		nvrtx_sm75_place_src2(word, src1);
		form = NVRTX_SM75_FORM_IMM_SRC2;
	} else if (src2 != NULL && src2->kind == NVRTX_SM75_SRC_CB) {
		/* The constant buffer word in the wide slot, the second source in the third's register slot. */
		nvrtx_sm75_place_cb(word, src2);
		nvrtx_sm75_place_src2(word, src1);
		form = NVRTX_SM75_FORM_CB_SRC2;
	} else {
		/* The third source, a register or none, in its slot; the second chooses the form. */
		nvrtx_sm75_place_src2(word, src2);
		form = NVRTX_SM75_FORM_REG;
		if (src1 != NULL && src1->kind == NVRTX_SM75_SRC_IMM) {
			nvrtx_sm75_set(word, NVRTX_SM75_IMM_LOW, NVRTX_SM75_IMM_BITS, src1->value);
			form = NVRTX_SM75_FORM_IMM;
		} else if (src1 != NULL && src1->kind == NVRTX_SM75_SRC_CB) {
			nvrtx_sm75_place_cb(word, src1);
			form = NVRTX_SM75_FORM_CB;
		} else if (src1 != NULL) {
			nvrtx_sm75_place_wide(word, src1);
		}
	}

	/* The opcode and the form. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_ALU_OPCODE_BITS, opcode);
	nvrtx_sm75_set(word, NVRTX_SM75_ALU_FORM_LOW, NVRTX_SM75_ALU_FORM_BITS, form);
}

/*
 * MOV dst, src: all four lanes of a quad written.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpMov::encode).
 */
void
nvrtx_sm75_mov(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	const struct nvrtx_sm75_src *src)
{
	/* The source is the second operand. */
	nvrtx_sm75_alu(word, NVRTX_SM75_OP_MOV, dst, NULL, src, NULL);
	nvrtx_sm75_set(word, NVRTX_SM75_MOV_LANES_LOW, 4U, NVRTX_SM75_MOV_LANES_ALL);
}

/*
 * FADD dst = a + b.  A register b is the second operand; any other b is
 * the third, with RZ in the second.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpFAdd::encode).
 */
void
nvrtx_sm75_fadd(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	const struct nvrtx_sm75_src *a,
	const struct nvrtx_sm75_src *b)
{
	struct nvrtx_sm75_src zero;

	/* RZ, the zero register, as an operand. */
	zero = nvrtx_sm75_reg(NVRTX_SM75_RZ);

	/* The operands by the kind of b. */
	if (b->kind == NVRTX_SM75_SRC_REG) {
		nvrtx_sm75_alu(word, NVRTX_SM75_OP_FADD, dst, a, b, NULL);
	} else {
		nvrtx_sm75_alu(word, NVRTX_SM75_OP_FADD, dst, a, &zero, b);
	}

	/* Round to nearest, denormals kept. */
	nvrtx_sm75_set(word, NVRTX_SM75_ROUND_LOW, 2U, NVRTX_SM75_ROUND_NEAREST);
}

/*
 * FMUL dst = a * b, with no post-divide.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpFMul::encode).
 */
void
nvrtx_sm75_fmul(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	const struct nvrtx_sm75_src *a,
	const struct nvrtx_sm75_src *b)
{
	struct nvrtx_sm75_src zero;

	/* RZ, the zero register, as an operand. */
	zero = nvrtx_sm75_reg(NVRTX_SM75_RZ);

	/* The product, RZ as the third operand. */
	nvrtx_sm75_alu(word, NVRTX_SM75_OP_FMUL, dst, a, b, &zero);
	nvrtx_sm75_set(word, NVRTX_SM75_ROUND_LOW, 2U, NVRTX_SM75_ROUND_NEAREST);
	nvrtx_sm75_set(word, NVRTX_SM75_FMUL_PDIV_LOW, 3U, NVRTX_SM75_FMUL_PDIV_NONE);
}

/*
 * FFMA dst = a * b + c.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpFFma::encode).
 */
void
nvrtx_sm75_ffma(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	const struct nvrtx_sm75_src *a,
	const struct nvrtx_sm75_src *b,
	const struct nvrtx_sm75_src *c)
{
	/* The three operands as they are. */
	nvrtx_sm75_alu(word, NVRTX_SM75_OP_FFMA, dst, a, b, c);
	nvrtx_sm75_set(word, NVRTX_SM75_ROUND_LOW, 2U, NVRTX_SM75_ROUND_NEAREST);
}

/*
 * FMNMX: the smaller of a and b when min (the predicate PT), else the larger
 * (!PT).
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpFMnMx::encode).
 */
void
nvrtx_sm75_fmnmx(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	const struct nvrtx_sm75_src *a,
	const struct nvrtx_sm75_src *b,
	int min)
{
	struct nvrtx_sm75_src zero;

	/* RZ, the zero register, as an operand. */
	zero = nvrtx_sm75_reg(NVRTX_SM75_RZ);

	/* The two operands, RZ as the third, and the choice. */
	nvrtx_sm75_alu(word, NVRTX_SM75_OP_FMNMX, dst, a, b, &zero);
	nvrtx_sm75_set(word, NVRTX_SM75_PSRC_A_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
	nvrtx_sm75_set(word, NVRTX_SM75_PSRC_A_NOT, 1U, (min != 0) ? 0U : 1U);
}

/*
 * FSETP predicate = compare(a, b) AND PT; the second destination is PT
 * (none).
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpFSetP::encode).
 */
void
nvrtx_sm75_fsetp(
	struct nvrtx_sm75_word *word,
	uint32_t predicate,
	uint32_t compare,
	const struct nvrtx_sm75_src *a,
	const struct nvrtx_sm75_src *b)
{
	/* The operands; there is no register destination. */
	nvrtx_sm75_alu(word, NVRTX_SM75_OP_FSETP, NVRTX_SM75_RZ, a, b, NULL);

	/* The comparison, combined by AND with the accumulator PT. */
	nvrtx_sm75_set(word, NVRTX_SM75_SETOP_LOW, 2U, NVRTX_SM75_SETOP_AND);
	nvrtx_sm75_set(word, NVRTX_SM75_CMP_LOW, 4U, compare);
	nvrtx_sm75_set(word, NVRTX_SM75_PSRC_A_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);

	/* The destinations. */
	nvrtx_sm75_set(word, NVRTX_SM75_PDST0_LOW, NVRTX_SM75_PRED_BITS, predicate);
	nvrtx_sm75_set(word, NVRTX_SM75_PDST1_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
}

/*
 * ISETP predicate = compare(a, b) AND PT, signed or unsigned.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpISetP::encode).
 */
void
nvrtx_sm75_isetp(
	struct nvrtx_sm75_word *word,
	uint32_t predicate,
	uint32_t compare,
	int is_signed,
	const struct nvrtx_sm75_src *a,
	const struct nvrtx_sm75_src *b)
{
	/* The operands; there is no register destination. */
	nvrtx_sm75_alu(word, NVRTX_SM75_OP_ISETP, NVRTX_SM75_RZ, a, b, NULL);

	/* The low compare and the accumulator are PT. */
	nvrtx_sm75_set(word, NVRTX_SM75_PSRC_C_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
	nvrtx_sm75_set(word, NVRTX_SM75_PSRC_A_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);

	/* The signedness, the AND, the comparison. */
	nvrtx_sm75_set(word, NVRTX_SM75_SIGNED, 1U, (is_signed != 0) ? 1U : 0U);
	nvrtx_sm75_set(word, NVRTX_SM75_SETOP_LOW, 2U, NVRTX_SM75_SETOP_AND);
	nvrtx_sm75_set(word, NVRTX_SM75_CMP_LOW, 3U, compare);

	/* The destinations. */
	nvrtx_sm75_set(word, NVRTX_SM75_PDST0_LOW, NVRTX_SM75_PRED_BITS, predicate);
	nvrtx_sm75_set(word, NVRTX_SM75_PDST1_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
}

/*
 * IADD3 dst = a + b + c, the carry inputs !PT (false) and the carry outputs
 * PT (none).
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpIAdd3::encode).
 */
void
nvrtx_sm75_iadd3(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	const struct nvrtx_sm75_src *a,
	const struct nvrtx_sm75_src *b,
	const struct nvrtx_sm75_src *c)
{
	/* The three operands. */
	nvrtx_sm75_alu(word, NVRTX_SM75_OP_IADD3, dst, a, b, c);

	/* No carry in. */
	nvrtx_sm75_set(word, NVRTX_SM75_PSRC_A_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
	nvrtx_sm75_set(word, NVRTX_SM75_PSRC_A_NOT, 1U, 1U);
	nvrtx_sm75_set(word, NVRTX_SM75_PSRC_B_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
	nvrtx_sm75_set(word, NVRTX_SM75_PSRC_B_NOT, 1U, 1U);

	/* No carry out. */
	nvrtx_sm75_set(word, NVRTX_SM75_PDST0_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
	nvrtx_sm75_set(word, NVRTX_SM75_PDST1_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
}

/*
 * IMAD dst = a * b + c, the low 32 bits.  The carry output is PT (none)
 * and the carry input !PT (false).
 *
 * Value referenced: the SM75 code the SASS compiler emits (IMAD's carry
 * operands).
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpIMad::encode).
 */
void
nvrtx_sm75_imad(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	const struct nvrtx_sm75_src *a,
	const struct nvrtx_sm75_src *b,
	const struct nvrtx_sm75_src *c,
	int is_signed)
{
	/* The three operands and the signedness. */
	nvrtx_sm75_alu(word, NVRTX_SM75_OP_IMAD, dst, a, b, c);
	nvrtx_sm75_set(word, NVRTX_SM75_SIGNED, 1U, (is_signed != 0) ? 1U : 0U);

	/* No carry out, no carry in. */
	nvrtx_sm75_set(word, NVRTX_SM75_PDST0_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
	nvrtx_sm75_set(word, NVRTX_SM75_PSRC_A_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
	nvrtx_sm75_set(word, NVRTX_SM75_PSRC_A_NOT, 1U, 1U);
}

/*
 * IMAD.WIDE.U32: the dst pair = a * b + the c pair, unsigned.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpIMad64::encode).
 */
void
nvrtx_sm75_imad_wide(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	const struct nvrtx_sm75_src *a,
	const struct nvrtx_sm75_src *b,
	const struct nvrtx_sm75_src *c)
{
	/* The three operands, unsigned. */
	nvrtx_sm75_alu(word, NVRTX_SM75_OP_IMAD_WIDE, dst, a, b, c);
	nvrtx_sm75_set(word, NVRTX_SM75_SIGNED, 1U, 0U);

	/* No carry out, no carry in. */
	nvrtx_sm75_set(word, NVRTX_SM75_PDST0_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
	nvrtx_sm75_set(word, NVRTX_SM75_PSRC_A_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
	nvrtx_sm75_set(word, NVRTX_SM75_PSRC_A_NOT, 1U, 1U);
}

/*
 * LOP3 dst = lut(a, b, c), with no predicate output.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpLop3::encode).
 */
void
nvrtx_sm75_lop3(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	const struct nvrtx_sm75_src *a,
	const struct nvrtx_sm75_src *b,
	const struct nvrtx_sm75_src *c,
	uint32_t lut)
{
	/* The three operands and the table. */
	nvrtx_sm75_alu(word, NVRTX_SM75_OP_LOP3, dst, a, b, c);
	nvrtx_sm75_set(word, NVRTX_SM75_LUT_LOW, 8U, lut);

	/* The predicate input is !PT and the predicate output PT (none). */
	nvrtx_sm75_set(word, NVRTX_SM75_PSRC_A_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
	nvrtx_sm75_set(word, NVRTX_SM75_PSRC_A_NOT, 1U, 1U);
	nvrtx_sm75_set(word, NVRTX_SM75_PDST0_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
}

/*
 * SHF as a 32-bit shift, the count taken modulo 32: left puts the value in
 * the low operand and returns the low half; right puts it in the high
 * operand and returns the high half, unsigned or signed.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpShf::encode) and src/nouveau/compiler/nak/builder.rs (SSABuilder::shl
 * and SSABuilder::shr).
 */
void
nvrtx_sm75_shift(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	uint32_t value,
	const struct nvrtx_sm75_src *count,
	int right,
	int arithmetic)
{
	struct nvrtx_sm75_src source;
	struct nvrtx_sm75_src zero;
	uint32_t type;

	/* The shifted register and RZ, the zero register, as operands. */
	source = nvrtx_sm75_reg(value);
	zero = nvrtx_sm75_reg(NVRTX_SM75_RZ);

	/* The funnel's low and high operands by the direction. */
	if (right != 0) {
		nvrtx_sm75_alu(word, NVRTX_SM75_OP_SHF, dst, &zero, count, &source);
	} else {
		nvrtx_sm75_alu(word, NVRTX_SM75_OP_SHF, dst, &source, count, &zero);
	}

	/* The type: signed for a left shift and an arithmetic right one, unsigned for a logical right one. */
	type = NVRTX_SM75_SHF_TYPE_I32;
	if (right != 0 && arithmetic == 0)
		type = NVRTX_SM75_SHF_TYPE_U32;

	/* The type, the wrap, the direction and the half. */
	nvrtx_sm75_set(word, NVRTX_SM75_SHF_TYPE_LOW, 2U, type);
	nvrtx_sm75_set(word, NVRTX_SM75_SHF_WRAP, 1U, 1U);
	nvrtx_sm75_set(word, NVRTX_SM75_SHF_RIGHT, 1U, (right != 0) ? 1U : 0U);
	nvrtx_sm75_set(word, NVRTX_SM75_SHF_HIGH, 1U, (right != 0) ? 1U : 0U);
}

/*
 * SEL dst = predicate (or its negation) ? a : b.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpSel::encode).
 */
void
nvrtx_sm75_sel(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	const struct nvrtx_sm75_src *a,
	const struct nvrtx_sm75_src *b,
	uint32_t predicate,
	int negate)
{
	/* The two operands and the predicate. */
	nvrtx_sm75_alu(word, NVRTX_SM75_OP_SEL, dst, a, b, NULL);
	nvrtx_sm75_set(word, NVRTX_SM75_PSRC_A_LOW, NVRTX_SM75_PRED_BITS, predicate);
	nvrtx_sm75_set(word, NVRTX_SM75_PSRC_A_NOT, 1U, (negate != 0) ? 1U : 0U);
}

/*
 * MUFU dst = function(src).
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpMuFu::encode).
 */
void
nvrtx_sm75_mufu(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	uint32_t function,
	uint32_t src)
{
	struct nvrtx_sm75_src source;

	/* The source register as an operand. */
	source = nvrtx_sm75_reg(src);

	/* The source is the second operand. */
	nvrtx_sm75_alu(word, NVRTX_SM75_OP_MUFU, dst, NULL, &source, NULL);
	nvrtx_sm75_set(word, NVRTX_SM75_MUFU_LOW, 6U, function);
}

/*
 * I2F dst = the float of the 32-bit integer src, rounded to nearest.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpI2F::encode).
 */
void
nvrtx_sm75_i2f(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	uint32_t src,
	int is_signed)
{
	struct nvrtx_sm75_src source;

	/* The source register as an operand. */
	source = nvrtx_sm75_reg(src);

	/* The source, its signedness, both sizes 32 bits, the rounding. */
	nvrtx_sm75_alu(word, NVRTX_SM75_OP_I2F, dst, NULL, &source, NULL);
	nvrtx_sm75_set(word, NVRTX_SM75_I2F_SIGNED, 1U, (is_signed != 0) ? 1U : 0U);
	nvrtx_sm75_set(word, NVRTX_SM75_CVT_DST_SIZE_LOW, 2U, NVRTX_SM75_CVT_SIZE_32);
	nvrtx_sm75_set(word, NVRTX_SM75_CVT_SRC_SIZE_LOW, 2U, NVRTX_SM75_CVT_SIZE_32);
	nvrtx_sm75_set(word, NVRTX_SM75_ROUND_LOW, 2U, NVRTX_SM75_ROUND_NEAREST);
}

/*
 * F2I dst = the 32-bit integer of the float src, rounded toward zero.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpF2I::encode).
 */
void
nvrtx_sm75_f2i(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	uint32_t src,
	int is_signed)
{
	struct nvrtx_sm75_src source;

	/* The source register as an operand. */
	source = nvrtx_sm75_reg(src);

	/* The source, the result's signedness, both sizes 32 bits, the rounding. */
	nvrtx_sm75_alu(word, NVRTX_SM75_OP_F2I, dst, NULL, &source, NULL);
	nvrtx_sm75_set(word, NVRTX_SM75_F2I_SIGNED, 1U, (is_signed != 0) ? 1U : 0U);
	nvrtx_sm75_set(word, NVRTX_SM75_CVT_DST_SIZE_LOW, 2U, NVRTX_SM75_CVT_SIZE_32);
	nvrtx_sm75_set(word, NVRTX_SM75_CVT_SRC_SIZE_LOW, 2U, NVRTX_SM75_CVT_SIZE_32);
	nvrtx_sm75_set(word, NVRTX_SM75_ROUND_LOW, 2U, NVRTX_SM75_ROUND_ZERO);
}

/*
 * FRND dst = src rounded to an integral float.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpFRnd::encode).
 */
void
nvrtx_sm75_frnd(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	uint32_t src,
	uint32_t rounding)
{
	struct nvrtx_sm75_src source;

	/* The source register as an operand. */
	source = nvrtx_sm75_reg(src);

	/* The source, both sizes 32 bits, the rounding. */
	nvrtx_sm75_alu(word, NVRTX_SM75_OP_FRND, dst, NULL, &source, NULL);
	nvrtx_sm75_set(word, NVRTX_SM75_CVT_DST_SIZE_LOW, 2U, NVRTX_SM75_CVT_SIZE_32);
	nvrtx_sm75_set(word, NVRTX_SM75_CVT_SRC_SIZE_LOW, 2U, NVRTX_SM75_CVT_SIZE_32);
	nvrtx_sm75_set(word, NVRTX_SM75_ROUND_LOW, 2U, rounding);
}

/*
 * S2R dst = a special register.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpS2R::encode).
 */
void
nvrtx_sm75_s2r(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	uint32_t special)
{
	/* The opcode, the destination, the register's number. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_S2R);
	nvrtx_sm75_set(word, NVRTX_SM75_DST_LOW, NVRTX_SM75_REG_BITS, dst);
	nvrtx_sm75_set(word, NVRTX_SM75_S2R_INDEX_LOW, 8U, special);
}

/*
 * PIXLD dst = a value of the pixel (NVRTX_SM75_PIXLD_*), no predicate
 * written.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpPixLd::encode).
 */
void
nvrtx_sm75_pixld(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	uint32_t value)
{
	/* The opcode, the destination, the value and PT as the predicate destination. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_PIXLD);
	nvrtx_sm75_set(word, NVRTX_SM75_DST_LOW, NVRTX_SM75_REG_BITS, dst);
	nvrtx_sm75_set(word, NVRTX_SM75_PIXLD_VALUE_LOW, 3U, value);
	nvrtx_sm75_set(word, NVRTX_SM75_PIXLD_PREDICATE_LOW, 3U, NVRTX_SM75_PT);
}

/*
 * LDC dst = the 32- or 64-bit word at a byte offset of a bound constant
 * buffer, indexed by RZ.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpLdc::encode).
 */
void
nvrtx_sm75_ldc(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	uint32_t index,
	uint32_t offset,
	int wide)
{
	/* The opcode, the destination, the index register RZ, the mode, a bound buffer. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_LDC);
	nvrtx_sm75_set(word, NVRTX_SM75_DST_LOW, NVRTX_SM75_REG_BITS, dst);
	nvrtx_sm75_set(word, NVRTX_SM75_SRC0_LOW, NVRTX_SM75_REG_BITS, NVRTX_SM75_RZ);
	nvrtx_sm75_set(word, NVRTX_SM75_LDC_MODE_LOW, 2U, 0U);
	nvrtx_sm75_set(word, NVRTX_SM75_CB_BINDLESS, 1U, 0U);

	/* The buffer, the offset and the size. */
	nvrtx_sm75_set(word, NVRTX_SM75_CB_INDEX_LOW, NVRTX_SM75_CB_INDEX_BITS, index);
	nvrtx_sm75_set(word, NVRTX_SM75_CB_OFFSET_LOW, NVRTX_SM75_CB_OFFSET_BITS, offset);
	nvrtx_sm75_set(word, NVRTX_SM75_MEM_TYPE_LOW, 3U, (wide != 0) ? NVRTX_SM75_MEM_B64 : NVRTX_SM75_MEM_B32);
}

/*
 * LDG.E dst = the 32-bit word at the 64-bit address in a register pair.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpLd::encode).
 */
void
nvrtx_sm75_ldg(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	uint32_t address)
{
	/* The opcode, no predicate output, the access. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_LDG);
	nvrtx_sm75_set(word, NVRTX_SM75_PDST0_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
	nvrtx_sm75_memory(word);

	/* The destination and the address, no offset. */
	nvrtx_sm75_set(word, NVRTX_SM75_DST_LOW, NVRTX_SM75_REG_BITS, dst);
	nvrtx_sm75_set(word, NVRTX_SM75_SRC0_LOW, NVRTX_SM75_REG_BITS, address);
	nvrtx_sm75_set(word, NVRTX_SM75_MEM_OFFSET_LOW, NVRTX_SM75_MEM_OFFSET_BITS, 0U);
}

/*
 * STG.E: the 32-bit data register to the 64-bit address in a register pair.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpSt::encode).
 */
void
nvrtx_sm75_stg(
	struct nvrtx_sm75_word *word,
	uint32_t address,
	uint32_t data)
{
	/* The opcode and the access. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_STG);
	nvrtx_sm75_memory(word);

	/* The address, the data, no offset. */
	nvrtx_sm75_set(word, NVRTX_SM75_SRC0_LOW, NVRTX_SM75_REG_BITS, address);
	nvrtx_sm75_set(word, NVRTX_SM75_STORE_DATA_LOW, NVRTX_SM75_REG_BITS, data);
	nvrtx_sm75_set(word, NVRTX_SM75_MEM_OFFSET_LOW, NVRTX_SM75_MEM_OFFSET_BITS, 0U);
}

/*
 * Sets a global access's memory order for a shader model.
 *
 * Before SM80 the scope (77..78, the GPU's) and the order (79..80, strong)
 * are two fields; from SM80 on they are one (77..80), in which strong at
 * the GPU's scope is 7.
 *
 * Value referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (set_mem_order).
 */
void
nvrtx_sm75_memory_order(
	struct nvrtx_sm75_word *word,
	uint32_t sm)
{
	/* Ampere and Ada: the one field. */
	if (sm >= NVRTX_SM_AMPERE_FIRST) {
		nvrtx_sm75_set(word, NVRTX_SM80_MEM_ORDER_LOW, NVRTX_SM80_MEM_ORDER_BITS, NVRTX_SM80_MEM_STRONG_GPU);
		return;
	}

	/* Turing: the scope and the order. */
	nvrtx_sm75_set(word, NVRTX_SM75_MEM_SCOPE_LOW, 2U, NVRTX_SM75_MEM_SCOPE_GPU);
	nvrtx_sm75_set(word, NVRTX_SM75_MEM_ORDER_LOW, 2U, NVRTX_SM75_MEM_ORDER_STRONG);
}

/*
 * EXIT, unconditional.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpExit::encode).
 */
void
nvrtx_sm75_exit(
	struct nvrtx_sm75_word *word)
{
	/* The opcode; the predicate source is PT. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_EXIT);
	nvrtx_sm75_set(word, NVRTX_SM75_PSRC_A_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
}

/*
 * NOP.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpNop::encode).
 */
void
nvrtx_sm75_nop(
	struct nvrtx_sm75_word *word)
{
	/* The opcode alone. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_NOP);
}

/*
 * BRA by a signed number of instructions from the next one (-1 is the BRA
 * itself).
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpBra::encode, get_rel_offset and set_rel_offset).
 */
void
nvrtx_sm75_bra(
	struct nvrtx_sm75_word *word,
	int32_t instructions)
{
	int64_t words;

	/* The offset counts 32-bit words, four to an instruction. */
	words = (int64_t)instructions * (int64_t)NVRTX_SM75_INSTRUCTION_WORDS;

	/* The opcode, not uniform, the predicate source PT, the offset. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_BRA);
	nvrtx_sm75_set(word, NVRTX_SM75_SRC1_LOW, 1U, 0U);
	nvrtx_sm75_set(word, NVRTX_SM75_PSRC_A_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
	nvrtx_sm75_set(word, NVRTX_SM75_BRA_OFFSET_LOW, NVRTX_SM75_BRA_OFFSET_BITS, (uint64_t)words);
}

/*
 * LDS dst = the 32-bit word at the shared memory offset in a register.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpLd::encode).
 */
void
nvrtx_sm75_lds(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	uint32_t offset)
{
	/* The opcode, the size, the destination and the offset register. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_LDS);
	nvrtx_sm75_set(word, NVRTX_SM75_MEM_TYPE_LOW, 3U, NVRTX_SM75_MEM_B32);
	nvrtx_sm75_set(word, NVRTX_SM75_DST_LOW, NVRTX_SM75_REG_BITS, dst);
	nvrtx_sm75_set(word, NVRTX_SM75_SRC0_LOW, NVRTX_SM75_REG_BITS, offset);
}

/*
 * STS: the 32-bit data register to the shared memory offset in a register.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpSt::encode).
 */
void
nvrtx_sm75_sts(
	struct nvrtx_sm75_word *word,
	uint32_t offset,
	uint32_t data)
{
	/* The opcode, the size, the offset and the data registers. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_STS);
	nvrtx_sm75_set(word, NVRTX_SM75_MEM_TYPE_LOW, 3U, NVRTX_SM75_MEM_B32);
	nvrtx_sm75_set(word, NVRTX_SM75_SRC0_LOW, NVRTX_SM75_REG_BITS, offset);
	nvrtx_sm75_set(word, NVRTX_SM75_STORE_DATA_LOW, NVRTX_SM75_REG_BITS, data);
}

/*
 * An atomic returning the old value: ATOMG on the 64-bit address in a
 * register pair (strong at the GPU's scope), or ATOMS on the shared memory
 * offset in a register.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpAtom::encode).
 */
void
nvrtx_sm75_atomic(
	struct nvrtx_sm75_word *word,
	int shared,
	uint32_t dst,
	uint32_t address,
	uint32_t data,
	uint32_t operation,
	int is_signed)
{
	/* The opcode and, for global memory, the access. */
	if (shared != 0) {
		nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_ATOMS);
	} else {
		nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_ATOMG);
		nvrtx_sm75_set(word, NVRTX_SM75_PDST0_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
		nvrtx_sm75_memory(word);
	}

	/* The registers, the operation and the type. */
	nvrtx_sm75_set(word, NVRTX_SM75_DST_LOW, NVRTX_SM75_REG_BITS, dst);
	nvrtx_sm75_set(word, NVRTX_SM75_SRC0_LOW, NVRTX_SM75_REG_BITS, address);
	nvrtx_sm75_set(word, NVRTX_SM75_STORE_DATA_LOW, NVRTX_SM75_REG_BITS, data);
	nvrtx_sm75_set(word, NVRTX_SM75_MEM_OFFSET_LOW, NVRTX_SM75_MEM_OFFSET_BITS, 0U);
	nvrtx_sm75_set(word, NVRTX_SM75_ATOM_OP_LOW, 4U, operation);
	nvrtx_sm75_set(word, NVRTX_SM75_ATOM_TYPE_LOW, 3U, (is_signed != 0) ? NVRTX_SM75_ATOM_I32 : NVRTX_SM75_ATOM_U32);
}

/*
 * A compare and exchange returning the old value, global or shared.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpAtom::encode).
 */
void
nvrtx_sm75_atomic_cas(
	struct nvrtx_sm75_word *word,
	int shared,
	uint32_t dst,
	uint32_t address,
	uint32_t compare,
	uint32_t data)
{
	/* The opcode and, for global memory, the access. */
	if (shared != 0) {
		nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_ATOMS_CAS);
	} else {
		nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_ATOMG_CAS);
		nvrtx_sm75_set(word, NVRTX_SM75_PDST0_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
		nvrtx_sm75_memory(word);
	}

	/* The registers and the type (unsigned 32 bits). */
	nvrtx_sm75_set(word, NVRTX_SM75_DST_LOW, NVRTX_SM75_REG_BITS, dst);
	nvrtx_sm75_set(word, NVRTX_SM75_SRC0_LOW, NVRTX_SM75_REG_BITS, address);
	nvrtx_sm75_set(word, NVRTX_SM75_STORE_DATA_LOW, NVRTX_SM75_REG_BITS, compare);
	nvrtx_sm75_set(word, NVRTX_SM75_CAS_DATA_LOW, NVRTX_SM75_REG_BITS, data);
	nvrtx_sm75_set(word, NVRTX_SM75_MEM_OFFSET_LOW, NVRTX_SM75_MEM_OFFSET_BITS, 0U);
	nvrtx_sm75_set(word, NVRTX_SM75_ATOM_TYPE_LOW, 3U, NVRTX_SM75_ATOM_U32);
}

/*
 * MEMBAR of a scope, not for MMIO and not sequentially consistent.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpMemBar::encode).
 */
void
nvrtx_sm75_membar(
	struct nvrtx_sm75_word *word,
	uint32_t scope)
{
	/* The opcode and the scope. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_MEMBAR);
	nvrtx_sm75_set(word, NVRTX_SM75_MEMBAR_SCOPE_LOW, 3U, scope);
}

/*
 * BAR.SYNC 0: every thread of the workgroup waits here.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpBar::encode).
 */
void
nvrtx_sm75_bar(
	struct nvrtx_sm75_word *word)
{
	/* The opcode alone names barrier 0 and the whole workgroup. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_BAR);
}

/*
 * BSSY: a convergence barrier for the threads that reach the BSYNC so many
 * instructions after the next one.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpBSSy::encode, get_rel_offset and set_rel_offset).
 */
void
nvrtx_sm75_bssy(
	struct nvrtx_sm75_word *word,
	uint32_t barrier,
	int32_t instructions)
{
	int64_t words;

	/* The offset counts 32-bit words, four to an instruction. */
	words = (int64_t)instructions * (int64_t)NVRTX_SM75_INSTRUCTION_WORDS;

	/* The opcode, the barrier, the offset, the predicate source PT. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_BSSY);
	nvrtx_sm75_set(word, NVRTX_SM75_CONVERGE_BAR_LOW, 4U, barrier);
	nvrtx_sm75_set(word, NVRTX_SM75_BSSY_OFFSET_LOW, NVRTX_SM75_BSSY_OFFSET_BITS, (uint64_t)words);
	nvrtx_sm75_set(word, NVRTX_SM75_PSRC_A_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
}

/*
 * BSYNC: waits until every thread the barrier collects has come.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpBSync::encode).
 */
void
nvrtx_sm75_bsync(
	struct nvrtx_sm75_word *word,
	uint32_t barrier)
{
	/* The opcode, the barrier, the predicate source PT. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_BSYNC);
	nvrtx_sm75_set(word, NVRTX_SM75_CONVERGE_BAR_LOW, 4U, barrier);
	nvrtx_sm75_set(word, NVRTX_SM75_PSRC_A_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
}

/*
 * Makes a BRA conditional on a predicate (the branch's own condition source).
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpBra::encode).
 */
void
nvrtx_sm75_bra_if(
	struct nvrtx_sm75_word *word,
	uint32_t predicate,
	int negate)
{
	/* The condition and its negation. */
	nvrtx_sm75_set(word, NVRTX_SM75_PSRC_A_LOW, NVRTX_SM75_PRED_BITS, predicate);
	nvrtx_sm75_set(word, NVRTX_SM75_PSRC_A_NOT, 1U, (negate != 0) ? 1U : 0U);
}

/*
 * ALD dst = one word of the vertex's input attribute at an address (the
 * vertex and the offset RZ).
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpALd::encode).
 */
void
nvrtx_sm75_ald(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	uint32_t address)
{
	/* The opcode, the destination, the registers RZ, the address, one component. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_ALD);
	nvrtx_sm75_set(word, NVRTX_SM75_DST_LOW, NVRTX_SM75_REG_BITS, dst);
	nvrtx_sm75_set(word, NVRTX_SM75_SRC0_LOW, NVRTX_SM75_REG_BITS, NVRTX_SM75_RZ);
	nvrtx_sm75_set(word, NVRTX_SM75_SRC1_LOW, NVRTX_SM75_REG_BITS, NVRTX_SM75_RZ);
	nvrtx_sm75_set(word, NVRTX_SM75_ATTR_ADDRESS_LOW, NVRTX_SM75_ATTR_ADDRESS_BITS, address);
	nvrtx_sm75_set(word, NVRTX_SM75_ATTR_COMPONENTS_LOW, 2U, 0U);
}

/*
 * AST: one word of the vertex's output attribute at an address (the vertex
 * and the offset RZ).
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpASt::encode).
 */
void
nvrtx_sm75_ast(
	struct nvrtx_sm75_word *word,
	uint32_t address,
	uint32_t data)
{
	/* The opcode, the data, the registers RZ, the address, one component. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_AST);
	nvrtx_sm75_set(word, NVRTX_SM75_STORE_DATA_LOW, NVRTX_SM75_REG_BITS, data);
	nvrtx_sm75_set(word, NVRTX_SM75_AST_VERTEX_LOW, NVRTX_SM75_REG_BITS, NVRTX_SM75_RZ);
	nvrtx_sm75_set(word, NVRTX_SM75_SRC0_LOW, NVRTX_SM75_REG_BITS, NVRTX_SM75_RZ);
	nvrtx_sm75_set(word, NVRTX_SM75_ATTR_ADDRESS_LOW, NVRTX_SM75_ATTR_ADDRESS_BITS, address);
	nvrtx_sm75_set(word, NVRTX_SM75_ATTR_COMPONENTS_LOW, 2U, 0U);
}

/*
 * ALD: one word of an input vertex's attribute at an address, the vertex's
 * handle in a register (the offset RZ).
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/compiler/nak/sm70.rs
 * (OpALd::encode: the vertex in 32..39, the offset in 24..31).
 */
void
nvrtx_sm75_ald_vertex(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	uint32_t address,
	uint32_t vertex)
{
	/* The plain ALD, then the vertex's handle. */
	nvrtx_sm75_ald(word, dst, address);
	nvrtx_sm75_set(word, NVRTX_SM75_SRC1_LOW, NVRTX_SM75_REG_BITS, vertex);
}

/*
 * AST: one word of the output vertex's attribute at an address, the
 * vertex's handle in a register (the offset RZ).
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/compiler/nak/sm70.rs
 * (OpASt::encode: the vertex in 64..71) and
 * src/nouveau/compiler/nak_nir_lower_gs_intrinsics.c (rewrite_ast_nv: a
 * geometry program's stores to the handle it emits).
 */
void
nvrtx_sm75_ast_vertex(
	struct nvrtx_sm75_word *word,
	uint32_t address,
	uint32_t data,
	uint32_t vertex)
{
	/* The plain AST, then the vertex's handle. */
	nvrtx_sm75_ast(word, address, data);
	nvrtx_sm75_set(word, NVRTX_SM75_AST_VERTEX_LOW, NVRTX_SM75_REG_BITS, vertex);
}

/*
 * ISBERD dst = the attribute handle of the input vertex an index names.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/compiler/nak/sm70.rs
 * (OpIsberd::encode).
 */
void
nvrtx_sm75_isberd(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	uint32_t index)
{
	/* The opcode, the destination, the index. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_ISBERD);
	nvrtx_sm75_set(word, NVRTX_SM75_DST_LOW, NVRTX_SM75_REG_BITS, dst);
	nvrtx_sm75_set(word, NVRTX_SM75_SRC0_LOW, NVRTX_SM75_REG_BITS, index);
}

/*
 * OUT dst = the handle after emitting the vertex a handle names, cutting
 * the strip, or ending the thread's output (NVRTX_SM75_OUT_*), on stream 0
 * (RZ): the register form of the ALU encoding.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/compiler/nak/sm70.rs
 * (OpOut::encode and OpOutFinal::encode).
 */
void
nvrtx_sm75_out(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	uint32_t handle,
	uint32_t type)
{
	struct nvrtx_sm75_src source;
	struct nvrtx_sm75_src stream;

	/* The handle and stream 0 as registers, the type. */
	source = nvrtx_sm75_reg(handle);
	stream = nvrtx_sm75_reg(NVRTX_SM75_RZ);
	nvrtx_sm75_alu(word, NVRTX_SM75_OP_OUT, dst, &source, &stream, NULL);
	nvrtx_sm75_set(word, NVRTX_SM75_OUT_TYPE_LOW, 2U, type);
}

/*
 * IPA dst = the fragment's value of the attribute at an address, at the
 * pixel's centre, with no offset.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpIpa::encode).
 */
void
nvrtx_sm75_ipa(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	uint32_t address,
	uint32_t frequency)
{
	/* The opcode, the destination, the address in words, the location and the frequency, the offset RZ, no predicate. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_IPA);
	nvrtx_sm75_set(word, NVRTX_SM75_DST_LOW, NVRTX_SM75_REG_BITS, dst);
	nvrtx_sm75_set(word, NVRTX_SM75_IPA_ADDRESS_LOW, 8U, address >> 2);
	nvrtx_sm75_set(word, NVRTX_SM75_IPA_FREQUENCY_LOW, 2U, frequency);
	nvrtx_sm75_set(word, NVRTX_SM75_SRC1_LOW, NVRTX_SM75_REG_BITS, NVRTX_SM75_RZ);
	nvrtx_sm75_set(word, NVRTX_SM75_PDST0_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
}

/*
 * TEX.2D with its handle in a constant buffer: four channels into two
 * register pairs, the coordinates from a pair, a level of detail mode and
 * register, no fault predicate, normal eviction.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpTex::encode and set_tex_cb_ref).
 */
void
nvrtx_sm75_tex(
	struct nvrtx_sm75_word *word,
	uint32_t first,
	uint32_t second,
	uint32_t coordinates,
	uint32_t lod,
	uint32_t lod_mode,
	uint32_t index,
	uint32_t offset)
{
	/* The opcode and the handle's place. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_TEX);
	nvrtx_sm75_set(word, NVRTX_SM75_TEX_HANDLE_LOW, NVRTX_SM75_TEX_HANDLE_BITS, offset / 4U);
	nvrtx_sm75_set(word, NVRTX_SM75_TEX_BUFFER_LOW, 5U, index);

	/* The destinations and the sources. */
	nvrtx_sm75_set(word, NVRTX_SM75_DST_LOW, NVRTX_SM75_REG_BITS, first);
	nvrtx_sm75_set(word, NVRTX_SM75_TEX_DST2_LOW, NVRTX_SM75_REG_BITS, second);
	nvrtx_sm75_set(word, NVRTX_SM75_SRC0_LOW, NVRTX_SM75_REG_BITS, coordinates);
	nvrtx_sm75_set(word, NVRTX_SM75_SRC1_LOW, NVRTX_SM75_REG_BITS, lod);

	/* 2D, all four channels, the level of detail mode, no fault predicate, normal eviction. */
	nvrtx_sm75_set(word, NVRTX_SM75_TEX_DIM_LOW, 3U, NVRTX_SM75_TEX_DIM_2D);
	nvrtx_sm75_set(word, NVRTX_SM75_TEX_CHANNELS_LOW, 4U, 0xfU);
	nvrtx_sm75_set(word, NVRTX_SM75_TEX_LOD_LOW, 3U, lod_mode);
	nvrtx_sm75_set(word, NVRTX_SM75_PDST0_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
	nvrtx_sm75_set(word, NVRTX_SM75_MEM_EVICT_LOW, 3U, NVRTX_SM75_MEM_EVICT_NORMAL);
}

/*
 * Makes a TEX a TLD, which fetches the texel at integer coordinates and an
 * explicit level (or level zero).
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpTld::encode).
 */
void
nvrtx_sm75_tex_fetch(
	struct nvrtx_sm75_word *word)
{
	/* The opcode; every other field is the TEX's. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_TLD);
}

/*
 * Makes a TEX or a TLD read a constant texel offset from the register after
 * its level of detail.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpTex::encode and OpTld::encode).
 */
void
nvrtx_sm75_tex_offset(
	struct nvrtx_sm75_word *word)
{
	/* AOFFI. */
	nvrtx_sm75_set(word, NVRTX_SM75_TEX_AOFFI, 1U, 1U);
}

/*
 * A texture instruction with its handle in a constant buffer (TEX, TLD,
 * TLD4, TMML, TXD or TXQ): the channels written into the pair first and,
 * past two, the pair second (RZ for none), its two source vectors (RZ for
 * none), the dimension (NVRTX_SM75_TEX_DIM_*), the handle's word at offset
 * of constant buffer index.  TEX and TLD get the normal eviction priority,
 * TLD4 its not-evict-first bit; all but TMML and TXQ write no fault
 * predicate.
 *
 * Procedure referenced: Mesa 25.0.7
 * src/nouveau/codegen/nv50_ir_emit_gv100.cpp (CodeEmitterGV100::emitTEX to
 * emitTXQ) and src/nouveau/compiler/nak/sm70.rs (OpTex::encode to
 * OpTxq::encode).
 */
void
nvrtx_sm75_texture(
	struct nvrtx_sm75_word *word,
	uint32_t opcode,
	uint32_t first,
	uint32_t second,
	uint32_t src0,
	uint32_t src1,
	uint32_t dim,
	uint32_t channels,
	uint32_t index,
	uint32_t offset)
{
	/* The opcode and the handle's place. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, opcode);
	nvrtx_sm75_set(word, NVRTX_SM75_TEX_HANDLE_LOW, NVRTX_SM75_TEX_HANDLE_BITS, offset / 4U);
	nvrtx_sm75_set(word, NVRTX_SM75_TEX_BUFFER_LOW, 5U, index);

	/* The destinations and the sources. */
	nvrtx_sm75_set(word, NVRTX_SM75_DST_LOW, NVRTX_SM75_REG_BITS, first);
	nvrtx_sm75_set(word, NVRTX_SM75_TEX_DST2_LOW, NVRTX_SM75_REG_BITS, second);
	nvrtx_sm75_set(word, NVRTX_SM75_SRC0_LOW, NVRTX_SM75_REG_BITS, src0);
	nvrtx_sm75_set(word, NVRTX_SM75_SRC1_LOW, NVRTX_SM75_REG_BITS, src1);

	/* The channels, and the dimension of all but TXQ, whose query shares its bits. */
	nvrtx_sm75_set(word, NVRTX_SM75_TEX_CHANNELS_LOW, 4U, channels);
	if (opcode != NVRTX_SM75_OP_TXQ)
		nvrtx_sm75_set(word, NVRTX_SM75_TEX_DIM_LOW, 3U, dim);

	/* The fault predicate (none) and the eviction by the instruction. */
	if (opcode != NVRTX_SM75_OP_TMML && opcode != NVRTX_SM75_OP_TXQ)
		nvrtx_sm75_set(word, NVRTX_SM75_PDST0_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
	if (opcode == NVRTX_SM75_OP_TEX || opcode == NVRTX_SM75_OP_TLD)
		nvrtx_sm75_set(word, NVRTX_SM75_MEM_EVICT_LOW, 3U, NVRTX_SM75_MEM_EVICT_NORMAL);
	if (opcode == NVRTX_SM75_OP_TLD4)
		nvrtx_sm75_set(word, NVRTX_SM75_TLD4_KEEP, 1U, 1U);
}

/*
 * Sets a TEX's or a TLD's level of detail mode (NVRTX_SM75_TEX_LOD_*).
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/compiler/nak/sm70.rs
 * (set_tex_lod_mode).
 */
void
nvrtx_sm75_tex_lod(
	struct nvrtx_sm75_word *word,
	uint32_t mode)
{
	/* The mode. */
	nvrtx_sm75_set(word, NVRTX_SM75_TEX_LOD_LOW, 3U, mode);
}

/*
 * Makes a TEX or a TLD4 compare with the depth reference (DC).
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/compiler/nak/sm70.rs
 * (OpTex::encode and OpTld4::encode, z_cmpr).
 */
void
nvrtx_sm75_tex_compare(
	struct nvrtx_sm75_word *word)
{
	/* DC. */
	nvrtx_sm75_set(word, NVRTX_SM75_TEX_COMPARE, 1U, 1U);
}

/*
 * Makes a TLD fetch a sample of a multisampled image (.MS; the bit a TEX
 * takes for its compare).
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/compiler/nak/sm70.rs
 * (OpTld::encode, is_ms at bit 78).
 */
void
nvrtx_sm75_tld_multisample(
	struct nvrtx_sm75_word *word)
{
	/* MS. */
	nvrtx_sm75_set(word, NVRTX_SM75_TEX_COMPARE, 1U, 1U);
}

/*
 * Sets the channel a TLD4 gathers.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/compiler/nak/sm70.rs
 * (OpTld4::encode).
 */
void
nvrtx_sm75_tld4_channel(
	struct nvrtx_sm75_word *word,
	uint32_t channel)
{
	/* The channel. */
	nvrtx_sm75_set(word, NVRTX_SM75_TLD4_CHANNEL_LOW, 2U, channel);
}

/*
 * SHFL dst = src of another lane of the quad: the lane by index or by a
 * butterfly, an immediate lane, the quad's clamp, no in-bounds predicate.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/compiler/nak/sm70.rs
 * (OpShfl::encode, the immediate lane and clamp form).
 */
void
nvrtx_sm75_shfl(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	uint32_t src,
	uint32_t lane,
	uint32_t mode)
{
	/* The opcode, the destination, the source, the lane, the clamp and the mode. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_SHFL);
	nvrtx_sm75_set(word, NVRTX_SM75_DST_LOW, NVRTX_SM75_REG_BITS, dst);
	nvrtx_sm75_set(word, NVRTX_SM75_SRC0_LOW, NVRTX_SM75_REG_BITS, src);
	nvrtx_sm75_set(word, NVRTX_SM75_SHFL_CLAMP_LOW, 13U, NVRTX_SM75_SHFL_QUAD);
	nvrtx_sm75_set(word, NVRTX_SM75_SHFL_LANE_LOW, 5U, lane);
	nvrtx_sm75_set(word, NVRTX_SM75_SHFL_MODE_LOW, 2U, mode);
	nvrtx_sm75_set(word, NVRTX_SM75_PDST0_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
}

/*
 * FSWZADD dst = each quad lane's operation (ops, NVRTX_SM75_FSWZADD_*) of a
 * and b, rounded to nearest, denormals kept.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/compiler/nak/sm70.rs
 * (OpFSwzAdd::encode).
 */
void
nvrtx_sm75_fswzadd(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	uint32_t a,
	uint32_t b,
	uint32_t ops)
{
	/* The opcode, the destination, the two sources and the operations. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_FSWZADD);
	nvrtx_sm75_set(word, NVRTX_SM75_DST_LOW, NVRTX_SM75_REG_BITS, dst);
	nvrtx_sm75_set(word, NVRTX_SM75_SRC0_LOW, NVRTX_SM75_REG_BITS, a);
	nvrtx_sm75_set(word, NVRTX_SM75_FSWZADD_SECOND_LOW, NVRTX_SM75_REG_BITS, b);
	nvrtx_sm75_set(word, NVRTX_SM75_FSWZADD_OPS_LOW, 8U, ops);
	nvrtx_sm75_set(word, NVRTX_SM75_ROUND_LOW, 2U, NVRTX_SM75_ROUND_NEAREST);
}

/*
 * F2F dst = src converted between 32-bit and 16-bit floats, rounded to
 * nearest: to a 16-bit one in the low half (to_half), or from the low or the
 * high half (high) of src.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/compiler/nak/sm70.rs
 * (OpF2F::encode).
 */
void
nvrtx_sm75_f2f(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	uint32_t src,
	int to_half,
	int high)
{
	struct nvrtx_sm75_src source;

	/* The source register as the second operand. */
	source = nvrtx_sm75_reg(src);
	nvrtx_sm75_alu(word, NVRTX_SM75_OP_F2F, dst, NULL, &source, NULL);

	/* The sizes, the half read, the rounding. */
	if (to_half != 0) {
		nvrtx_sm75_set(word, NVRTX_SM75_CVT_DST_SIZE_LOW, 2U, NVRTX_SM75_CVT_SIZE_16);
		nvrtx_sm75_set(word, NVRTX_SM75_CVT_SRC_SIZE_LOW, 2U, NVRTX_SM75_CVT_SIZE_32);
	} else {
		nvrtx_sm75_set(word, NVRTX_SM75_CVT_DST_SIZE_LOW, 2U, NVRTX_SM75_CVT_SIZE_32);
		nvrtx_sm75_set(word, NVRTX_SM75_CVT_SRC_SIZE_LOW, 2U, NVRTX_SM75_CVT_SIZE_16);
	}
	if (high != 0)
		nvrtx_sm75_set(word, NVRTX_SM75_F2F_HIGH_LOW, 2U, 1U);
	nvrtx_sm75_set(word, NVRTX_SM75_ROUND_LOW, 2U, NVRTX_SM75_ROUND_NEAREST);
}

/*
 * PRMT dst = the bytes of a (0..3) and b (4..7) the selector's nibbles name,
 * by index.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/compiler/nak/sm70.rs
 * (OpPrmt::encode).
 */
void
nvrtx_sm75_prmt(
	struct nvrtx_sm75_word *word,
	uint32_t dst,
	uint32_t a,
	uint32_t selector,
	uint32_t b)
{
	struct nvrtx_sm75_src first;
	struct nvrtx_sm75_src chooser;
	struct nvrtx_sm75_src last;

	/* The first source, the selector as the immediate, the third source. */
	first = nvrtx_sm75_reg(a);
	chooser = nvrtx_sm75_imm(selector);
	last = nvrtx_sm75_reg(b);
	nvrtx_sm75_alu(word, NVRTX_SM75_OP_PRMT, dst, &first, &chooser, &last);
	nvrtx_sm75_set(word, NVRTX_SM75_PRMT_MODE_LOW, 3U, 0U);
}

/*
 * KILL: the fragment is discarded (the predicate source PT; the guard chooses
 * the threads).
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (OpKill::encode).
 */
void
nvrtx_sm75_kill(
	struct nvrtx_sm75_word *word)
{
	/* The opcode and the predicate source. */
	nvrtx_sm75_set(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS, NVRTX_SM75_OP_KILL);
	nvrtx_sm75_set(word, NVRTX_SM75_PSRC_A_LOW, NVRTX_SM75_PRED_BITS, NVRTX_SM75_PT);
}

/*
 * Places a source in the third source's register slot, with that slot's abs
 * and neg; none places nothing.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (encode_alu_src2).
 */
static void
nvrtx_sm75_place_src2(
	struct nvrtx_sm75_word *word,
	const struct nvrtx_sm75_src *src)
{
	/* An absent source leaves the slot zero. */
	if (src == NULL)
		return;

	/* The register and its modifiers. */
	nvrtx_sm75_set(word, NVRTX_SM75_SRC2_LOW, NVRTX_SM75_REG_BITS, src->value);
	nvrtx_sm75_set(word, NVRTX_SM75_SRC2_ABS, 1U, (src->abs != 0) ? 1U : 0U);
	nvrtx_sm75_set(word, NVRTX_SM75_SRC2_NEG, 1U, (src->neg != 0) ? 1U : 0U);
}

/*
 * Places a register in the second source's slot, with its abs and neg.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (encode_alu_reg).
 */
static void
nvrtx_sm75_place_wide(
	struct nvrtx_sm75_word *word,
	const struct nvrtx_sm75_src *src)
{
	/* The register and its modifiers. */
	nvrtx_sm75_set(word, NVRTX_SM75_SRC1_LOW, NVRTX_SM75_REG_BITS, src->value);
	nvrtx_sm75_set(word, NVRTX_SM75_SRC1_ABS, 1U, (src->abs != 0) ? 1U : 0U);
	nvrtx_sm75_set(word, NVRTX_SM75_SRC1_NEG, 1U, (src->neg != 0) ? 1U : 0U);
}

/*
 * Places a constant buffer word in the wide slot: its offset and its buffer,
 * bound, with its abs and neg.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (encode_alu_cb and set_src_cb).
 */
static void
nvrtx_sm75_place_cb(
	struct nvrtx_sm75_word *word,
	const struct nvrtx_sm75_src *src)
{
	/* The offset, the buffer, a bound buffer. */
	nvrtx_sm75_set(word, NVRTX_SM75_CB_OFFSET_LOW, NVRTX_SM75_CB_OFFSET_BITS, src->value);
	nvrtx_sm75_set(word, NVRTX_SM75_CB_INDEX_LOW, NVRTX_SM75_CB_INDEX_BITS, src->cb_index);
	nvrtx_sm75_set(word, NVRTX_SM75_CB_BINDLESS, 1U, 0U);

	/* The modifiers. */
	nvrtx_sm75_set(word, NVRTX_SM75_SRC1_ABS, 1U, (src->abs != 0) ? 1U : 0U);
	nvrtx_sm75_set(word, NVRTX_SM75_SRC1_NEG, 1U, (src->neg != 0) ? 1U : 0U);
}

/*
 * Sets a global access: a 64-bit address, 32 bits, strong at the GPU's scope,
 * normal eviction.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sm70_encode.rs
 * (set_mem_access).
 */
static void
nvrtx_sm75_memory(
	struct nvrtx_sm75_word *word)
{
	/* The address size and the data size. */
	nvrtx_sm75_set(word, NVRTX_SM75_MEM_A64, 1U, 1U);
	nvrtx_sm75_set(word, NVRTX_SM75_MEM_TYPE_LOW, 3U, NVRTX_SM75_MEM_B32);

	/* The scope, the order and the eviction priority. */
	nvrtx_sm75_set(word, NVRTX_SM75_MEM_SCOPE_LOW, 2U, NVRTX_SM75_MEM_SCOPE_GPU);
	nvrtx_sm75_set(word, NVRTX_SM75_MEM_ORDER_LOW, 2U, NVRTX_SM75_MEM_ORDER_STRONG);
	nvrtx_sm75_set(word, NVRTX_SM75_MEM_EVICT_LOW, 3U, NVRTX_SM75_MEM_EVICT_NORMAL);
}
